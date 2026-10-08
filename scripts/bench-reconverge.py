#!/usr/bin/env python3
"""bench-reconverge.py -- time for the ring to repair itself after nodes are killed
(BENCHMARKS.md section 3; decisions D-038 and D-039).

One RUN:
  1. start N `node --wired` processes on loopback (a correct ring, then maintained),
     with --period T --timeout D --succ R;
  2. wait until every node's state matches the oracle and stays matched (the nodes start
     one after another, so the earliest briefly see the latest as dead), then let it run
     for SETTLE periods; only log lines written from here on are counted;
  3. optionally start lookup clients (`lookup --retries 3`) at a fixed total rate;
  4. SIGKILL k nodes chosen by the run's seed;
  5. poll every live node until all three milestones have held for HOLD seconds:
       ring     every live node's successor and predecessor equal the oracle's
       lists    every live node's successor list equals the oracle's
       fingers  every live node's 64 fingers equal the oracle's
     A milestone's time is the end of the first poll from which it held at every later
     poll. It is measured from just BEFORE the kill signal, so it is an upper bound,
     with a resolution of one poll (reported);
  6. after convergence, look up keys from several live nodes and check every owner;
  7. scan every node's log for evictions of nodes that were alive (false evictions).

The oracle, the probes and the judging live in scripts/ringcheck.py and share no code
with the C++ nodes. A node reports what it holds; only the harness decides whether it
is right.

Modes:
  --calibrate N        run an N-node ring at --period until it is verified converged, then
                       time NEIGHBOURS exchanges to every node for --seconds; prints the
                       distribution (D-039's rule for D)
  --cpu N              CPU per node over --seconds with the ring idle, for each --periods value
  (default)            the sweep given by --sizes, --periods, --kills, --load, --runs
  --keep DIR           also keep every run's node logs and the harness's timeline (kill time,
                       victims, every poll) under DIR, for explaining a run after the fact

Writes one CSV row per run to bench/raw/reconverge-<commit>.csv (gitignored); the
calibration and CPU modes write bench/raw/calibrate-<commit>.csv and cpu-<commit>.csv.
scripts/plot-reconverge.py turns them into the committed summary and figure. Prints a warning into its own output
if the working tree is dirty, because a number that cannot be tied to a commit cannot be
reproduced.
"""
import argparse, csv, json, os, random, re, shutil, signal, socket, statistics, subprocess, sys, threading, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
from ringcheck import Oracle, Poller, judge, ident, wait_converged, wait_listening   # noqa: E402

WORK = os.path.join(os.environ.get("TMPDIR", "/tmp"), f"bench-reconverge-{os.getpid()}")
BASE_PORT = 21000            # below 32768, the start of the kernel's ephemeral range
SETTLE_PERIODS = 4


def members_for(n, run):
    """Addresses of one ring. Every 127.x address is loopback on Linux; varying the
    third octet by run gives every run different identifiers."""
    return [f"127.2.{run % 250}.1:{BASE_PORT + i}" for i in range(n)]


def commit_state():
    head = subprocess.run(["git", "-C", ROOT, "rev-parse", "--short", "HEAD"], capture_output=True,
                          text=True).stdout.strip()
    dirty = subprocess.run(["git", "-C", ROOT, "status", "--porcelain", "--untracked-files=no"],
                           capture_output=True, text=True).stdout.strip()
    return head, bool(dirty)


class Ring:
    """N wired node processes; always killed on exit."""

    def __init__(self, members, period, timeout, succ):
        self.members = members
        self.period, self.timeout, self.succ = period, timeout, succ
        self.procs = {}
        self.logdir = os.path.join(WORK, "logs")
        os.makedirs(self.logdir, exist_ok=True)
        for f in os.listdir(self.logdir):
            os.remove(os.path.join(self.logdir, f))
        self.file = os.path.join(WORK, "members.txt")
        with open(self.file, "w") as f:
            f.write("".join(m + "\n" for m in members))

    def __enter__(self):
        for m in self.members:
            ip, port = m.split(":")
            log = open(os.path.join(self.logdir, f"{ip}-{port}.log"), "w")
            self.procs[m] = subprocess.Popen(
                [os.path.join(ROOT, "node"), ip, port, "--wired", self.file, "--vnodes", "1", "--period", str(self.period),
                 "--timeout", str(self.timeout), "--succ", str(self.succ)],
                stdout=subprocess.DEVNULL, stderr=log)
            log.close()
        for m in self.members:
            if not wait_listening(m, 60):
                raise RuntimeError(f"{m} never started listening")
        return self

    def kill(self, m):
        self.procs[m].send_signal(signal.SIGKILL)

    def __exit__(self, *exc):
        for p in self.procs.values():
            p.kill()
        for p in self.procs.values():
            p.wait()
        return False

    def logs(self):
        out = {}
        for m in self.members:
            ip, port = m.split(":")
            with open(os.path.join(self.logdir, f"{ip}-{port}.log")) as f:
                out[m] = f.read()
        return out


class Load:
    """Closed-loop lookup clients at a fixed total rate. Each client is one `lookup
    --retries 3` process fed one key at a time; every answer is timestamped."""

    def __init__(self, starts, rate, seed):
        self.results = []                    # (t_answer, key, words of the output line)
        self.lock = threading.Lock()
        self.stop = threading.Event()
        self.threads = []
        self.interval = len(starts) / rate if rate > 0 else 0
        for i, s in enumerate(starts):
            t = threading.Thread(target=self.client, args=(s, random.Random(f"{seed}-{i}")), daemon=True)
            self.threads.append(t)

    def client(self, start, rng):
        ip, port = start.split(":")
        p = subprocess.Popen([os.path.join(ROOT, "lookup"), "--retries", "3", ip, port],
                             stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
        nxt = time.monotonic()
        while not self.stop.is_set():
            k = rng.randrange(1 << 64)
            p.stdin.write(f"{k:016x}\n")
            p.stdin.flush()
            line = p.stdout.readline()
            if not line:
                break
            with self.lock:
                self.results.append((time.monotonic(), k, line.split()))
            nxt += self.interval
            delay = nxt - time.monotonic()
            if delay > 0:
                time.sleep(delay)
            else:
                nxt = time.monotonic()        # fell behind: do not burst to catch up
        p.stdin.close()
        p.wait()

    def __enter__(self):
        for t in self.threads:
            t.start()
        return self

    def __exit__(self, *exc):
        self.stop.set()
        for t in self.threads:
            t.join(timeout=30)
        return False


def one_run(n, k, period, timeout, succ, run, rate, limit_s, poller, adjacent=False, keep=None, tag=""):
    members = members_for(n, run)
    rng = random.Random(f"reconverge-{n}-{k}-{period}-{run}-{adjacent}")
    hold_s = max(2 * period / 1000, 0.5)
    row = {"n": n, "k": k, "adjacent": int(adjacent), "period_ms": period, "timeout_ms": timeout,
           "succ": succ, "run": run, "load_rate": rate}
    with Ring(members, period, timeout, succ) as ring:
        # The nodes start one after another, and a node's maintenance begins as soon
        # as it listens, so an early node can ping a neighbour that has not started
        # yet. That is refused -- proof of death, correctly acted on -- and the ring
        # repairs itself once every node is up. So the measured interval starts from
        # a state VERIFIED correct and stable, and only log lines written after that
        # point are counted.
        full = Oracle(members, succ)
        t_start = time.monotonic()
        ok, _, _ = wait_converged(poller, full, limit_s, stable_s=hold_s)
        row["start_correct"] = int(ok)
        row["startup_s"] = round(time.monotonic() - t_start, 2)
        time.sleep(SETTLE_PERIODS * period / 1000)
        log_offsets = {m: len(text) for m, text in ring.logs().items()}

        if adjacent:
            order = sorted(members, key=ident)
            first = rng.randrange(n)
            victims = [order[(first + i) % n] for i in range(k)]
        else:
            victims = rng.sample(members, k)
        live = [m for m in members if m not in victims]
        oracle = Oracle(live, succ)

        load = None
        if rate > 0:
            starts = rng.sample(live, min(4, len(live)))
            load = Load(starts, rate, f"{n}-{k}-{period}-{run}")
            load.__enter__()
            time.sleep(1.0)                   # let the clients reach a steady rate

        t_kill = time.monotonic()
        t_kill_wall = time.time()             # the nodes' log lines carry wall-clock times
        for v in victims:
            ring.kill(v)

        polls = []                            # (t_end - t_kill, ring ok, lists ok, fingers ok, duration)
        deadline = t_kill + limit_s
        since_all = None
        while time.monotonic() < deadline:
            st, t0, t1 = poller.poll(live)
            ok = judge(st, oracle)
            polls.append((t1 - t_kill, ok[0], ok[1], ok[2], t1 - t0))
            if ok[0] and ok[1] and ok[2]:
                since_all = since_all or t1
                if t1 - since_all >= hold_s:
                    break
            else:
                since_all = None
        t_end = time.monotonic()

        if load is not None:
            load.__exit__(None, None, None)

        def milestone(idx):
            """End of the first poll from which the milestone held at every later poll."""
            t = None
            for p in polls:
                if p[idx]:
                    t = t if t is not None else p[0]
                else:
                    t = None
            return t

        row.update(ring_s=milestone(1), lists_s=milestone(2), fingers_s=milestone(3),
                   converged=int(since_all is not None and t_end - since_all >= hold_s),
                   polls=len(polls), poll_ms_median=round(1000 * statistics.median(p[4] for p in polls), 1),
                   poll_ms_max=round(1000 * max(p[4] for p in polls), 1))

        # After convergence: lookups from several live nodes, every owner checked.
        wrong = total = 0
        for s in rng.sample(live, min(4, len(live))):
            keys = [rng.randrange(1 << 64) for _ in range(64)]
            ip, port = s.split(":")
            out = subprocess.run([os.path.join(ROOT, "lookup"), ip, port],
                                 input="".join(f"{x:016x}\n" for x in keys), capture_output=True,
                                 text=True, timeout=120).stdout.splitlines()
            for x, line in zip(keys, out):
                w = line.split()
                total += 1
                wrong += 0 if (w[1] == "OWNER" and w[3] == oracle.owner(x)) else 1
            wrong += abs(len(keys) - len(out))
        row.update(after_lookups=total, after_wrong=wrong)

        # False evictions: a node that was alive evicted by anyone, for any reason.
        logs = {m: text[log_offsets[m]:] for m, text in ring.logs().items()}
        false_evictions = 0
        orphaned = 0
        for m, text in logs.items():
            for line in text.splitlines():
                ev = re.search(r"evict (\S+) \(", line)
                if ev and ev.group(1) in live:
                    false_evictions += 1
            if m in live and "orphaned:" in text:
                orphaned += 1
        row.update(false_evictions=false_evictions, orphaned=orphaned)

        if keep:
            # Keep this run's evidence: every node's log, and the harness's own timeline
            # (kill time on the wall clock, victims, and every poll's verdicts).
            dest = os.path.join(keep, f"{tag}-n{n}-k{k}{'adj' if adjacent else ''}-T{period}-D{timeout}-run{run}")
            shutil.rmtree(dest, ignore_errors=True)
            shutil.copytree(ring.logdir, os.path.join(dest, "logs"))
            with open(os.path.join(dest, "run.json"), "w") as f:
                json.dump({"t_kill_wall": t_kill_wall, "victims": victims, "row": row,
                           "polls": [{"t": round(p[0], 3), "ring": p[1], "lists": p[2], "fingers": p[3],
                                      "poll_s": round(p[4], 3)} for p in polls]}, f, indent=1, default=str)

        if load is not None:
            res = load.results
            before = [r for r in res if r[0] < t_kill]
            after = [r for r in res if t_kill <= r[0] <= t_end]
            vset = set(victims)

            def classify(r):
                w = r[2]
                if len(w) < 2 or w[1] != "OWNER":
                    return "error"
                if w[3] == oracle.owner(r[1]):
                    return "correct"
                return "dead-owner" if w[3] in vset else "wrong"
            kinds = [classify(r) for r in after]
            bad = [r[0] - t_kill for r, c in zip(after, kinds) if c != "correct"]
            retried = sum(1 for r in after if len(r[2]) >= 2 and r[2][-2] == "attempts" and r[2][-1] != "1")
            row.update(load_before=len(before),
                       load_before_wrong=sum(1 for r in before if len(r[2]) < 2 or r[2][1] != "OWNER"
                                             or r[2][3] != full.owner(r[1])),
                       load_after=len(after), load_correct=kinds.count("correct"),
                       load_dead_owner=kinds.count("dead-owner"), load_wrong=kinds.count("wrong"),
                       load_error=kinds.count("error"), load_retried=retried,
                       load_last_bad_s=round(max(bad), 3) if bad else 0.0,
                       load_rate_achieved=round(len(after) / (t_end - t_kill), 1))
    return row


def calibrate(n, period, timeout, seconds, succ):
    """D-039's rule for D: time NEIGHBOURS round trips to every node of a running ring,
    from this process, while the ring runs at the given period."""
    members = members_for(n, 0)
    times = []
    with Ring(members, period, timeout, succ):
        # Time only a ring verified correct and stable. Nodes start one after another,
        # and a 256-node ring at a 1 s period takes about 16 s to settle after the last
        # one starts; timing exchanges before that measures start-up repair, not steady
        # state (an earlier version did, and its slowest exchange came from start-up).
        poller = Poller()
        converged, _, _ = wait_converged(poller, Oracle(members, succ), 300,
                                         stable_s=max(2 * period / 1000, 0.5))
        poller.close()
        if not converged:
            print(f"calibrate n={n} period={period} ms: ring never converged; exchanges below include "
                  f"start-up repair", flush=True)
        time.sleep(SETTLE_PERIODS * period / 1000)
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            for m in members:
                ip, port = m.split(":")
                t0 = time.monotonic()
                try:
                    s = socket.create_connection((ip, int(port)), timeout=5)
                    s.sendall(b"NEIGHBOURS\n")
                    s.makefile("rb").readline()
                    s.close()
                    times.append(time.monotonic() - t0)
                except OSError:
                    times.append(float("inf"))
    times.sort()
    pct = lambda q: times[max(0, int(q / 100 * len(times) + 0.999999) - 1)]
    row = {"n": n, "period_ms": period, "converged_first": int(converged), "exchanges": len(times), "median_ms": round(1000 * pct(50), 3),
           "p99_ms": round(1000 * pct(99), 3), "p999_ms": round(1000 * pct(99.9), 3),
           "max_ms": round(1000 * times[-1], 3)}
    print(f"calibrate n={n} period={period} ms: {len(times)} exchanges; median {row['median_ms']} ms  "
          f"p99 {row['p99_ms']} ms  p99.9 {row['p999_ms']} ms  max {row['max_ms']} ms", flush=True)
    return row


def cpu_per_node(n, periods, timeout, seconds, succ):
    """CPU seconds per node per second (as a percentage of one core), ring idle."""
    tick = os.sysconf("SC_CLK_TCK")
    out = []
    for period in periods:
        members = members_for(n, 0)
        with Ring(members, period, timeout, succ) as ring:
            time.sleep(SETTLE_PERIODS * period / 1000 + 1)

            def total():
                t = 0
                for p in ring.procs.values():
                    with open(f"/proc/{p.pid}/stat") as f:
                        w = f.read().rsplit(")", 1)[1].split()
                    t += int(w[11]) + int(w[12])      # utime + stime, in clock ticks
                return t
            a, t0 = total(), time.monotonic()
            time.sleep(seconds)
            b, t1 = total(), time.monotonic()
        pct = 100.0 * (b - a) / tick / (t1 - t0) / n
        print(f"cpu n={n} period={period} ms: {pct:.3f}% of one core per node", flush=True)
        out.append({"n": n, "period_ms": period, "seconds": seconds, "cpu_pct_per_node": round(pct, 4)})
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sizes", default="64")
    ap.add_argument("--periods", default="500")
    ap.add_argument("--kills", default="1")
    ap.add_argument("--adjacent", action="store_true", help="kill k nodes adjacent on the ring")
    ap.add_argument("--load", type=float, default=0.0, help="total lookups per second during the run")
    ap.add_argument("--timeout", type=int, default=1000)
    ap.add_argument("--succ", type=int, default=8)
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--limit", type=float, default=60.0)
    ap.add_argument("--tag", default="sweep")
    ap.add_argument("--calibrate", type=int, default=0)
    ap.add_argument("--cpu", type=int, default=0)
    ap.add_argument("--seconds", type=float, default=20.0)
    ap.add_argument("--keep", default="", help="directory to keep every run's node logs and timeline in")
    args = ap.parse_args()
    ints = lambda s: [int(x) for x in s.split(",") if x]

    head, dirty = commit_state()
    print(f"commit {head}{'  WARNING: working tree is dirty -- these numbers are not reproducible' if dirty else ''}",
          flush=True)
    for b in ("node", "lookup"):
        if not os.access(os.path.join(ROOT, b), os.X_OK):
            sys.exit(f"{b} not built; run make")
    os.makedirs(WORK, exist_ok=True)

    os.makedirs(os.path.join(ROOT, "bench", "raw"), exist_ok=True)

    def save(name, rows):
        path = os.path.join(ROOT, "bench", "raw", f"{name}-{head}.csv")
        new = not os.path.exists(path)
        with open(path, "a", newline="") as f:
            w = csv.DictWriter(f, fieldnames=list(rows[0]))
            if new:
                w.writeheader()
            w.writerows(rows)
        print(f"raw: {path}")

    if args.calibrate:
        save("calibrate", [calibrate(args.calibrate, period, args.timeout, args.seconds, args.succ)
                           for period in ints(args.periods)])
        return
    if args.cpu:
        save("cpu", cpu_per_node(args.cpu, ints(args.periods), args.timeout, args.seconds, args.succ))
        return

    raw_path = os.path.join(ROOT, "bench", "raw", f"reconverge-{head}.csv")
    fields = ["tag", "n", "k", "adjacent", "period_ms", "timeout_ms", "succ", "run", "load_rate",
              "start_correct", "startup_s", "converged", "ring_s", "lists_s", "fingers_s", "polls", "poll_ms_median",
              "poll_ms_max", "after_lookups", "after_wrong", "false_evictions", "orphaned",
              "load_before", "load_before_wrong", "load_after", "load_correct", "load_dead_owner",
              "load_wrong", "load_error", "load_retried", "load_last_bad_s", "load_rate_achieved"]
    new = not os.path.exists(raw_path)
    raw_file = open(raw_path, "a", newline="")
    raw = csv.DictWriter(raw_file, fieldnames=fields, extrasaction="ignore")
    if new:
        raw.writeheader()

    poller = Poller()
    try:
        for n in ints(args.sizes):
            for period in ints(args.periods):
                for k in ints(args.kills):
                    for run in range(args.runs):
                        row = one_run(n, k, period, args.timeout, args.succ, run, args.load, args.limit,
                                      poller, args.adjacent, args.keep, args.tag)
                        row["tag"] = args.tag
                        raw.writerow(row)
                        raw_file.flush()
                        f = lambda v: "-" if v is None else f"{v:.2f}"
                        print(f"  {args.tag} n={n} k={k}{'adj' if args.adjacent else ''} T={period} run={run}: "
                              f"ring {f(row['ring_s'])} s  lists {f(row['lists_s'])} s  fingers {f(row['fingers_s'])} s  "
                              f"poll {row['poll_ms_median']} ms  start_ok {row['start_correct']}  "
                              f"after {row['after_lookups'] - row['after_wrong']}/{row['after_lookups']}  "
                              f"false_evict {row['false_evictions']}  orphaned {row['orphaned']}"
                              + (f"  load: {row['load_correct']}/{row['load_after']} correct, "
                                 f"{row['load_dead_owner']} dead-owner, {row['load_wrong']} wrong, "
                                 f"{row['load_error']} error, last bad {row['load_last_bad_s']} s, "
                                 f"{row['load_rate_achieved']}/s" if args.load else ""), flush=True)
    finally:
        poller.close()
        raw_file.close()
    print(f"raw: {raw_path}")


if __name__ == "__main__":
    try:
        main()
    finally:
        subprocess.run(["rm", "-rf", WORK])
