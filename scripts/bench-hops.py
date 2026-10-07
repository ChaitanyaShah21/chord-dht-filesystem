#!/usr/bin/env python3
"""bench-hops.py -- lookup hop count versus ring size (BENCHMARKS.md section 2).

Two series, one workload scheme (decision D-027):

  real  N `node` processes on loopback, queried by the `lookup` tool over TCP.
  sim   `sim-hops`: the same parse_members / build_fingers / route_step / lookup()
        code in one process, with a function call where the socket was.

For every ring, real or simulated, the workload is the same shape: up to 64
start nodes drawn at random from the ring, and 4096 / starts random 64-bit keys
from each, so 4096 lookups per ring. Every answer is checked against an oracle
written here, independently of the C++ code (nearest node clockwise from the
key). Real rings are also checked node by node against a model of the finger
rule, and replayed through sim-hops, which must return identical lines.

Ring identities differ between repetitions because addresses do: real ring r
uses 127.1.r.1 with ports from 20000 (every 127.x address is loopback on
Linux), and simulated ring r uses 10.r.x.y. Ports stay below 32768, the start of
the kernel's ephemeral range, so a node can never collide with a client socket.

Usage:
  scripts/bench-hops.py --real 2,4,8 --sim 2,4,8,1048576 [--reps 3] [--lookups 4096]
  scripts/bench-hops.py --memory 64,256,1024      # per-node memory only

Writes bench/raw/hops-<commit>.csv (every lookup; gitignored), and the summary
docs/figures/hops-vs-n.csv that the plot and BENCHMARKS.md are built from.
Prints a warning into its own output if the working tree is dirty, because a
number that cannot be tied to a commit cannot be reproduced.
"""
import argparse, bisect, csv, hashlib, math, os, random, socket, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WORK = os.path.join(os.environ.get("TMPDIR", "/tmp"), f"bench-hops-{os.getpid()}")
BASE_PORT = 20000
MOD = 1 << 64
MAX_STARTS = 64


def ident(addr):
    """Routing identifier of "ip:port": top 64 bits of SHA-1, as chord.cpp computes it."""
    return int(hashlib.sha1(addr.encode()).hexdigest()[:16], 16)


def real_members(n, rep):
    return [f"127.1.{rep}.1:{BASE_PORT + i}" for i in range(n)]


def sim_members(n, rep):
    return [f"10.{rep}.{(i >> 8) & 255}.{i & 255}:{7000 + (i >> 16)}" for i in range(n)]


class Oracle:
    """Owners by nearest-clockwise search, and (for small rings) the model path."""
    def __init__(self, members):
        pairs = sorted((ident(a), a) for a in members)
        self.ids = [p[0] for p in pairs]
        self.addr = [p[1] for p in pairs]
        self.id_of = {a: i for i, a in pairs}
        self.fingers = None

    def owner(self, k):
        i = bisect.bisect_left(self.ids, k)          # first id >= k, wrapping past the top
        return self.addr[i % len(self.ids)]

    def model_path(self, k, start):
        """The finger rule restated with clockwise distances; no C++ code involved."""
        if self.fingers is None:
            self.fingers = {a: [self.owner((self.id_of[a] + (1 << i)) % MOD) for i in range(64)]
                            for a in self.addr}
        cur, path = start, []
        while True:
            path.append(cur)
            me, succ = self.id_of[cur], self.fingers[cur][0]
            s = (self.id_of[succ] - me) % MOD
            d = (k - me) % MOD
            if s == 0 or 0 < d <= s:
                return path
            span = d if d != 0 else MOD
            ahead = [f for f in self.fingers[cur] if 0 < (self.id_of[f] - me) % MOD < span]
            cur = max(ahead, key=lambda f: (self.id_of[f] - me) % MOD) if ahead else succ


def workload(members, n, rep, lookups):
    """{start address: [keys]} -- the same shape for every ring, seeded by (n, rep)."""
    rng = random.Random(f"hops-{n}-{rep}")
    starts = rng.sample(members, min(n, MAX_STARTS))
    per = math.ceil(lookups / len(starts))
    return {s: [rng.randrange(MOD) for _ in range(per)] for s in starts}


def parse_line(line):
    head, _, message = line.partition(" : ")
    w = head.split()
    if w[1] != "OWNER":
        return {"key": w[0], "ok": False, "line": line}
    return {"key": w[0], "ok": True, "owner": w[3], "requests": int(w[5]), "hops": int(w[7]),
            "path": w[9].split(",")}


def run_sim(members_file, work):
    text = "".join(f"{s} {k:016x}\n" for s, keys in work.items() for k in keys)
    p = subprocess.run([f"{ROOT}/sim-hops", members_file], input=text, capture_output=True,
                       text=True, timeout=3600)
    return p.returncode, p.stdout.splitlines()


def mem_of(pid):
    """VmRSS, and from smaps_rollup the proportional set size and private bytes, in KiB."""
    out = {}
    with open(f"/proc/{pid}/status") as f:
        for line in f:
            if line.startswith("VmRSS:"):
                out["rss"] = int(line.split()[1])
    with open(f"/proc/{pid}/smaps_rollup") as f:
        for line in f:
            w = line.split()
            if w[0] == "Pss:":
                out["pss"] = int(w[1])
            elif w[0] in ("Private_Clean:", "Private_Dirty:"):
                out["private"] = out.get("private", 0) + int(w[1])
    return out


def mem_available_kib():
    with open("/proc/meminfo") as f:
        for line in f:
            if line.startswith("MemAvailable:"):
                return int(line.split()[1])


def ping(addr):
    ip, port = addr.split(":")
    try:
        s = socket.create_connection((ip, int(port)), timeout=2)
        s.sendall(b"PING\n")
        ok = s.makefile("rb").readline().startswith(b"PONG ")
        s.close()
        return ok
    except OSError:
        return False


class RealRing:
    """N node processes, started together and always killed on exit."""
    def __init__(self, members):
        self.members = members
        self.file = os.path.join(WORK, "members.txt")
        with open(self.file, "w") as f:
            f.write("".join(m + "\n" for m in members))
        self.procs = []

    def __enter__(self):
        avail_before = mem_available_kib()
        t0 = time.monotonic()
        for m in self.members:
            ip, port = m.split(":")
            self.procs.append(subprocess.Popen([f"{ROOT}/node", ip, port, self.file],
                                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        pending = list(self.members)
        deadline = time.monotonic() + 300
        while pending and time.monotonic() < deadline:
            pending = [m for m in pending if not ping(m)]
            if pending:
                time.sleep(0.2)
        if pending:
            raise RuntimeError(f"{len(pending)} nodes never answered PING, e.g. {pending[0]}")
        self.startup_s = time.monotonic() - t0
        time.sleep(1)                                 # let the allocator settle after bootstrap
        self.avail_drop_kib = avail_before - mem_available_kib()
        sample = self.procs if len(self.procs) <= 64 else random.Random(1).sample(self.procs, 64)
        self.mem = [mem_of(p.pid) for p in sample]
        return self

    def __exit__(self, *exc):
        for p in self.procs:
            p.kill()
        for p in self.procs:
            p.wait()
        return False


def median(xs):
    xs = sorted(xs)
    return xs[len(xs) // 2] if len(xs) % 2 else (xs[len(xs) // 2 - 1] + xs[len(xs) // 2]) / 2


def pct(xs, q):
    """Nearest-rank percentile: the smallest value with at least q% of samples at or below it."""
    xs = sorted(xs)
    return xs[max(0, math.ceil(q / 100 * len(xs)) - 1)]


def commit_state():
    head = subprocess.run(["git", "-C", ROOT, "rev-parse", "--short", "HEAD"], capture_output=True,
                          text=True).stdout.strip()
    dirty = subprocess.run(["git", "-C", ROOT, "status", "--porcelain", "--untracked-files=no"],
                           capture_output=True, text=True).stdout.strip()
    return head, bool(dirty)


def measure_memory(sizes):
    os.makedirs(WORK, exist_ok=True)
    print("n,startup_s,rss_kib_median,pss_kib_median,private_kib_median,memavailable_drop_kib_per_node")
    for n in sizes:
        with RealRing(real_members(n, 0)) as ring:
            print(f"{n},{ring.startup_s:.1f},{median([m['rss'] for m in ring.mem])},"
                  f"{median([m['pss'] for m in ring.mem])},{median([m['private'] for m in ring.mem])},"
                  f"{ring.avail_drop_kib / n:.0f}", flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--real", default="")
    ap.add_argument("--sim", default="")
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--lookups", type=int, default=4096)
    ap.add_argument("--memory", default="")
    ap.add_argument("--summary", default=os.path.join(ROOT, "docs", "figures", "hops-vs-n.csv"))
    args = ap.parse_args()
    sizes = lambda s: [int(x) for x in s.split(",") if x]

    head, dirty = commit_state()
    print(f"commit {head}{'  WARNING: working tree is dirty -- these numbers are not reproducible' if dirty else ''}")
    for b in ("node", "lookup", "sim-hops"):
        if not os.access(os.path.join(ROOT, b), os.X_OK):
            sys.exit(f"{b} not built; run make all sim-hops")

    if args.memory:
        measure_memory(sizes(args.memory))
        return

    os.makedirs(WORK, exist_ok=True)
    os.makedirs(os.path.join(ROOT, "bench", "raw"), exist_ok=True)
    raw_path = os.path.join(ROOT, "bench", "raw", f"hops-{head}.csv")
    raw = csv.writer(open(raw_path, "w", newline=""))
    raw.writerow(["series", "n", "rep", "start", "key", "owner", "requests", "hops"])
    summary = []
    problems = 0

    for series, ns in (("real", sizes(args.real)), ("sim", sizes(args.sim))):
        for n in ns:
            hops, reqs, mem, startup, avail = [], [], [], [], []
            replay_same = replay_total = 0
            t_series = time.monotonic()
            for rep in range(args.reps):
                members = real_members(n, rep) if series == "real" else sim_members(n, rep)
                oracle = Oracle(members)
                work = workload(members, n, rep, args.lookups)
                mfile = os.path.join(WORK, f"sim-members-{n}-{rep}.txt")
                with open(mfile, "w") as f:
                    f.write("".join(m + "\n" for m in members))

                if series == "real":
                    with RealRing(members) as ring:
                        lines = []
                        for start, keys in work.items():
                            ip, port = start.split(":")
                            p = subprocess.run([f"{ROOT}/lookup", ip, port],
                                               input="".join(f"{k:016x}\n" for k in keys),
                                               capture_output=True, text=True, timeout=600)
                            lines += p.stdout.splitlines()
                        mem += ring.mem
                        startup.append(ring.startup_s)
                        avail.append(ring.avail_drop_kib / n)
                    rc, sim_lines = run_sim(mfile, work)      # the replay: same ring, same workload
                    replay_total += len(lines)
                    replay_same += sum(a == b for a, b in zip(lines, sim_lines))
                else:
                    rc, lines = run_sim(mfile, work)

                keys_in_order = [(s, k) for s, ks in work.items() for k in ks]
                if len(lines) != len(keys_in_order):
                    print(f"  PROBLEM {series} n={n} rep={rep}: {len(lines)} lines for {len(keys_in_order)} lookups")
                    problems += 1
                for (start, k), line in zip(keys_in_order, lines):
                    r = parse_line(line)
                    if not r["ok"] or r["owner"] != oracle.owner(k) or r["path"][0] != start or \
                            r["hops"] != r["requests"] - 1:
                        problems += 1
                        if problems <= 5:
                            print(f"  PROBLEM {line}  (oracle owner {oracle.owner(k)})")
                        continue
                    if series == "real" and n <= 4096 and r["path"] != oracle.model_path(k, start):
                        problems += 1
                        if problems <= 5:
                            print(f"  PROBLEM path {line}")
                    hops.append(r["hops"])
                    reqs.append(r["requests"])
                    raw.writerow([series, n, rep, start, f"{k:016x}", r["owner"], r["requests"], r["hops"]])

            row = {"series": series, "n": n, "rings": args.reps, "lookups": len(hops),
                   "mean_hops": round(sum(hops) / len(hops), 3), "p1_hops": pct(hops, 1),
                   "p50_hops": pct(hops, 50), "p99_hops": pct(hops, 99), "max_hops": max(hops),
                   "mean_requests": round(sum(reqs) / len(reqs), 3),
                   "half_log2_n": round(0.5 * math.log2(n), 3)}
            if series == "real":
                row.update(replay_identical=f"{replay_same}/{replay_total}",
                           startup_s_max=round(max(startup), 1),
                           rss_kib_median=median([m["rss"] for m in mem]),
                           pss_kib_median=median([m["pss"] for m in mem]),
                           memavailable_drop_kib_per_node=round(median(avail)))
            summary.append(row)
            print(f"  {series:4} n={n:>7}  mean {row['mean_hops']:.2f} hops  p1 {row['p1_hops']}  "
                  f"p99 {row['p99_hops']}  max {row['max_hops']}  (1/2 log2 n = {row['half_log2_n']:.2f})"
                  + (f"  replay {row['replay_identical']}  pss {row['pss_kib_median']} KiB" if series == "real" else "")
                  + f"  [{time.monotonic() - t_series:.0f} s]", flush=True)

    fields = ["series", "n", "rings", "lookups", "mean_hops", "p1_hops", "p50_hops", "p99_hops", "max_hops",
              "mean_requests", "half_log2_n", "replay_identical", "startup_s_max", "rss_kib_median",
              "pss_kib_median", "memavailable_drop_kib_per_node"]
    os.makedirs(os.path.dirname(args.summary), exist_ok=True)
    with open(args.summary, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        for row in summary:
            w.writerow(row)
    print(f"raw: {raw_path}\nsummary: {args.summary}\nproblems: {problems}")
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    try:
        main()
    finally:
        subprocess.run(["rm", "-rf", WORK])
