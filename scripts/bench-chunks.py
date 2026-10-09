#!/usr/bin/env python3
"""bench-chunks.py -- the Phase 4 measurements (BENCHMARKS.md §6; decisions D-051, D-053).

Modes (one per run of the script):
  --balance          load balance against virtual nodes per process (D-051's rule):
                       1. sim-balance for P in {16, 64}, V in {1, 2, ..., 128}, 20 address sets,
                          100,000 keys each; the rule picks V;
                       2. a real ring checks the simulation exactly: P = 16 processes, chunks written
                          through the nodes, files counted in every store against sim-balance --predict;
                       3. idle CPU per process against V, 16 processes.
  --wsweep           write latency, acknowledgement and the window with fewer than three copies, for
                     W = 1, 2, 3 interleaved on one ring through three phases: healthy, one replica
                     process frozen (SIGSTOP), one killed (D-053).
  --crash            durability when the coordinator dies the moment it acknowledges, per W.
  --repair           time from killing one process until every chunk is again on its three correct
                     live processes, for --vnodes values.

Every ring is N `node --wired` processes on 127.2.<run>.1 from port 21000, each with its own
store under WORK (removed at exit). Raw rows go to bench/raw/<mode>-<commit>.csv (gitignored);
scripts/plot-chunks.py makes the committed summary and figures. Warns if the tree is dirty.
"""
import datetime, argparse, csv, json, os, random, re, shutil, signal, statistics, subprocess, sys, threading, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
from chunkcheck import Placement, have, ring_correct, under_replicated   # noqa: E402

WORK = os.path.join(os.environ.get("TMPDIR", "/tmp"), f"bench-chunks-{os.getpid()}")
BASE_PORT = 21000
CHUNK = 512 * 1024


def commit_state():
    head = subprocess.run(["git", "-C", ROOT, "rev-parse", "--short", "HEAD"], capture_output=True, text=True).stdout.strip()
    dirty = subprocess.run(["git", "-C", ROOT, "status", "--porcelain", "--untracked-files=no"],
                           capture_output=True, text=True).stdout.strip()
    return head, bool(dirty)


HEAD, DIRTY = commit_state()


def save(name, rows):
    if not rows:
        return
    os.makedirs(os.path.join(ROOT, "bench", "raw"), exist_ok=True)
    path = os.path.join(ROOT, "bench", "raw", f"{name}-{HEAD}.csv")
    new = not os.path.exists(path)
    with open(path, "a", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0]))
        if new:
            w.writeheader()
        w.writerows(rows)
    print(f"raw: {path}", flush=True)


def wait_until(pred, limit, step=0.1):
    deadline = time.monotonic() + limit
    while time.monotonic() < deadline:
        if pred():
            return True
        time.sleep(step)
    return False


class Ring:
    """N wired node processes with stores; always killed on exit."""

    def __init__(self, n, v, run, period=1000, timeout=200, scan=60, store=True):
        self.addrs = [f"127.2.{run % 250}.1:{BASE_PORT + i}" for i in range(n)]
        self.v, self.period, self.timeout, self.scan, self.store = v, period, timeout, scan, store
        self.dir = os.path.join(WORK, f"ring-{run}")
        shutil.rmtree(self.dir, ignore_errors=True)
        os.makedirs(os.path.join(self.dir, "logs"))
        self.file = os.path.join(self.dir, "members.txt")
        open(self.file, "w").write("".join(a + "\n" for a in self.addrs))
        self.procs = {}

    def store_of(self, a):
        return os.path.join(self.dir, "store-" + a.split(":")[1])

    def log_of(self, a):
        return os.path.join(self.dir, "logs", a.split(":")[1] + ".log")

    def start(self, a):
        ip, port = a.split(":")
        cmd = [os.path.join(ROOT, "node"), ip, port, "--wired", self.file, "--vnodes", str(self.v),
               "--period", str(self.period), "--timeout", str(self.timeout), "--scan", str(self.scan)]
        if self.store:
            cmd += ["--store", self.store_of(a)]
        log = open(self.log_of(a), "a")
        self.procs[a] = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=log)
        log.close()

    def __enter__(self):
        for a in self.addrs:
            self.start(a)
        return self

    def live(self):
        return [a for a in self.addrs if a in self.procs]

    def placement(self):
        return Placement(self.live(), self.v)

    def converge(self, limit=60):
        return wait_until(lambda: ring_correct(self.placement()), limit, 0.25)

    def kill(self, a):
        p = self.procs.pop(a)
        p.send_signal(signal.SIGCONT)
        p.kill()
        p.wait()

    def __exit__(self, *exc):
        for a in list(self.procs):
            self.kill(a)
        shutil.rmtree(self.dir, ignore_errors=True)
        return False


class Client:
    """One `chunk batch` process; one command in, one line out."""

    def __init__(self, start):
        self.p = subprocess.Popen([os.path.join(ROOT, "chunk"), "batch", start], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, text=True, bufsize=1)

    def ask(self, line):
        self.p.stdin.write(line + "\n")
        self.p.stdin.flush()
        return self.p.stdout.readline().split()

    def close(self):
        self.p.stdin.close()
        self.p.wait()


# ---------------------------------------------------------------------------- balance

def balance(args):
    sim = os.path.join(ROOT, "sim-balance")
    rows = []
    for P in (16, 64):
        for V in (1, 2, 4, 8, 16, 32, 64, 128):
            out = subprocess.run([sim, str(P), str(V), str(args.keys), str(args.sets)], capture_output=True,
                                 text=True, check=True).stdout
            for line in out.splitlines():
                p, v, s, omm, ocv, smm, scv, short = line.split()
                rows.append({"P": int(p), "V": int(v), "set": int(s), "owned_max_over_mean": float(omm),
                             "owned_cv": float(ocv), "stored_max_over_mean": float(smm), "stored_cv": float(scv),
                             "short": float(short)})
    save("balance", rows)
    chosen = None
    for V in (1, 2, 4, 8, 16, 32, 64, 128):
        meds = {P: statistics.median(r["stored_cv"] for r in rows if r["P"] == P and r["V"] == V) for P in (16, 64)}
        ok = all(m <= 0.25 for m in meds.values())
        print(f"rule: V={V:3d} median stored CV  P=16 {meds[16]:.3f}  P=64 {meds[64]:.3f}  {'meets' if ok else '-'}",
              flush=True)
        if ok and chosen is None:
            chosen = V
    print(f"RULE -> V = {chosen}", flush=True)

    # The real ring must store exactly what the simulation predicts.
    check = []
    for V in sorted({1, chosen or 1}):
        with Ring(16, V, run=100 + V, period=1000, timeout=200, scan=0) as ring:
            ring.converge()
            c = Client(ring.addrs[0])
            keys = []
            for i in range(args.check_chunks):
                w = c.ask(f"putrand 3 1024 {V * 1000003 + i}")
                if len(w) > 2 and w[2] == "ok":
                    keys.append(w[1])
            c.close()
            time.sleep(2)
            # Only chunk files count: anything under tmp/ is a write in progress.
            actual = {a: sum(len(fs) for d, _, fs in os.walk(ring.store_of(a)) if not d.endswith("/tmp"))
                      for a in ring.addrs}
            pred = subprocess.run([sim, "--predict", str(V), ring.file], input="\n".join(keys), capture_output=True,
                                  text=True, check=True).stdout.split("\n")
            predicted = {l.split()[0]: int(l.split()[1]) for l in pred if l.strip()}
            exact = all(actual[a] == predicted[a] for a in ring.addrs)
            print(f"real ring V={V}: {len(keys)} chunks; per-process files {[actual[a] for a in ring.addrs]}; "
                  f"predicted {[predicted[a] for a in ring.addrs]}; exact match {exact}", flush=True)
            check.append({"V": V, "chunks": len(keys), "exact": int(exact),
                          "actual": json.dumps([actual[a] for a in ring.addrs]),
                          "predicted": json.dumps([predicted[a] for a in ring.addrs])})
    save("balance-check", check)

    # Idle CPU per process against V (16 processes, no store activity).
    tick = os.sysconf("SC_CLK_TCK")
    cpu = []
    for V in (1, 2, 4, 8, 16, 32):
        with Ring(16, V, run=150 + V, period=1000, timeout=200, scan=0, store=False) as ring:
            ok = ring.converge(120)
            time.sleep(4)

            def total():
                t = 0
                for p in ring.procs.values():
                    w = open(f"/proc/{p.pid}/stat").read().rsplit(")", 1)[1].split()
                    t += int(w[11]) + int(w[12])
                return t
            a, t0 = total(), time.monotonic()
            time.sleep(args.seconds)
            b, t1 = total(), time.monotonic()
        pct = 100.0 * (b - a) / tick / (t1 - t0) / 16
        print(f"cpu V={V}: {pct:.3f}% of one core per process (converged first: {ok})", flush=True)
        cpu.append({"V": V, "processes": 16, "seconds": args.seconds, "converged": int(ok), "cpu_pct": round(pct, 4)})
    save("balance-cpu", cpu)


# ---------------------------------------------------------------------------- W sweep

def coordinator_windows(ring):
    """key -> (copies, answered_ms, last_ms) from every coordinator's log."""
    out = {}
    pat = re.compile(r"put ([0-9a-f]{40}): (\d+) of (\d+) copies, answered after (\d+) ms, last copy after (\d+) ms")
    for a in ring.addrs:
        try:
            for m in pat.finditer(open(ring.log_of(a)).read()):
                out.setdefault(m.group(1), []).append((int(m.group(2)), int(m.group(4)), int(m.group(5))))
        except OSError:
            pass
    return out


def wsweep(args):
    rows = []
    for run in range(args.runs):
        rng = random.Random(f"wsweep-{run}")
        with Ring(16, args.vnodes, run=200 + run) as ring:
            if not ring.converge():
                print("ring did not converge; skipping run", flush=True)
                continue
            victim = rng.choice(ring.addrs[1:])
            starts = [a for a in ring.addrs if a != victim]
            keyer = Client(starts[0])
            # Seeds whose chunk has the victim as its 2nd or 3rd replica (never the owner), so
            # every write in the fault phases goes through a healthy owner to the faulty replica.
            placement = ring.placement()
            seeds, s = [], run * 10_000_000
            while len(seeds) < 3 * 3 * args.writes:
                k = keyer.ask(f"keyrand {CHUNK} {s}")[1]
                if victim in placement.holders(k)[1:]:
                    seeds.append(s)
                s += 1
            keyer.close()
            clients = [Client(starts[1 + i]) for i in range(4)]
            lock = threading.Lock()

            def phase(name, seeds_here, t_fault):
                jobs = list(enumerate(seeds_here))

                def worker(c):
                    while True:
                        with lock:
                            if not jobs:
                                return
                            i, seed = jobs.pop(0)
                        w = 1 + i % 3                       # interleaved: 1, 2, 3, 1, 2, 3, ...
                        t = time.monotonic()
                        out = c.ask(f"putrand {w} {CHUNK} {seed}")
                        ok = len(out) > 2 and out[2] == "ok"
                        with lock:
                            rows.append({"run": run, "phase": name, "w": w, "key": out[1] if len(out) > 1 else "",
                                         "ok": int(ok), "copies": int(out[3]) if ok else 0,
                                         "latency_ms": round(int(out[4] if ok else out[3]) / 1000, 3),
                                         "t_since_fault_s": round(t - t_fault, 3) if t_fault else "",
                                         "vnodes": args.vnodes})
                threads = [threading.Thread(target=worker, args=(c,)) for c in clients]
                for th in threads:
                    th.start()
                for th in threads:
                    th.join()

            n = 3 * args.writes
            phase("healthy", seeds[:n], None)
            ring.procs[victim].send_signal(signal.SIGSTOP)
            t = time.monotonic()
            phase("frozen", seeds[n:2 * n], t)
            ring.procs[victim].send_signal(signal.SIGCONT)
            ring.converge()
            ring.kill(victim)
            t = time.monotonic()
            phase("killed", seeds[2 * n:3 * n], t)
            for c in clients:
                c.close()
            time.sleep(1)
            windows = coordinator_windows(ring)
            for r in rows:
                if r["run"] == run and r["key"] in windows:
                    copies, ans, last = windows[r["key"]][-1]
                    r["copies_final"], r["answered_ms"], r["last_copy_ms"] = copies, ans, last
        done = [r for r in rows if r["run"] == run]
        for ph in ("healthy", "frozen", "killed"):
            for w in (1, 2, 3):
                rs = [r for r in done if r["phase"] == ph and r["w"] == w]
                lat = sorted(r["latency_ms"] for r in rs if r["ok"])
                if rs:
                    p = lambda q: lat[min(len(lat) - 1, int(q * len(lat)))] if lat else float("nan")
                    print(f"run {run} {ph:7s} W={w}: acked {sum(r['ok'] for r in rs)}/{len(rs)}  "
                          f"p50 {p(0.5):.1f} ms  p99 {p(0.99):.1f} ms", flush=True)
    fields = sorted({k for r in rows for k in r})
    rows = [{k: r.get(k, "") for k in fields} for r in rows]
    save("wsweep", rows)


# ---------------------------------------------------------------------------- coordinator crash

def crash(args):
    rows = []
    with Ring(16, args.vnodes, run=230) as ring:
        ring.converge()
        rng = random.Random("crash")
        for trial in range(args.trials):
            for w in (1, 2, 3):
                live = ring.live()
                c = Client(rng.choice(live))
                seed = 50_000_000 + trial * 10 + w
                out = c.ask(f"putrand {w} {CHUNK} {seed}")
                t_ack = time.monotonic()
                ok = len(out) > 2 and out[2] == "ok"
                coord = out[5] if ok else ""
                if ok and coord in ring.procs:
                    ring.procs[coord].kill()                   # the moment it acknowledged
                    t_kill = time.monotonic()
                    ring.procs[coord].wait()
                    del ring.procs[coord]
                    # Copies on OTHER processes: what is left if the coordinator's machine, disk
                    # and all, is gone. Counted at once, and again after 2 s in case a copy that
                    # was already in a replica's socket buffer lands late.
                    count = lambda: sum(1 for a in ring.live() if (have(a, [out[1]]) or [False])[0])
                    wall = time.time()
                    now = count()
                    time.sleep(0.25)
                    later = count()
                    # Did any repair round send anything inside that window? If not, every copy
                    # counted at +250 ms came from the write itself, not from re-replication.
                    repaired = False
                    for a in ring.live():
                        # A structured line (D-086): ts=2026-10-09T13:56:32.667Z ... msg="repair: ..."
                        for m in re.finditer(r"ts=(\S+) .*?msg=\"repair: \d+ arcs checked, (\d+) chunks sent",
                                             open(ring.log_of(a)).read()):
                            when = datetime.datetime.fromisoformat(m.group(1).replace("Z", "+00:00")).timestamp()
                            if wall - 0.05 <= when <= wall + 0.3 and int(m.group(2)) > 0:
                                repaired = True
                    ring.start(coord)                            # back, from its own store
                    ring.converge()
                    back = Client(rng.choice(ring.live())).ask(f"get {out[1]}")
                    rows.append({"trial": trial, "w": w, "acked": 1, "copies_acked": int(out[3]),
                                 "kill_after_ack_ms": round(1000 * (t_kill - t_ack), 3),
                                 "other_copies_at_kill": now, "other_copies_after_250ms": later,
                                 "repair_ran_in_window": int(repaired),
                                 "lost_with_coordinator_disk": int(later == 0),
                                 "readable_after_restart": int(len(back) > 2 and back[2] == "ok"), "vnodes": args.vnodes})
                    print(f"trial {trial} W={w}: acked with {out[3]}, coordinator killed "
                          f"{1000 * (t_kill - t_ack):.2f} ms later; other processes hold {now} copies at once, "
                          f"{later} after 250 ms (repair in window: {int(repaired)}); readable after restart: {rows[-1]['readable_after_restart']}", flush=True)
                else:
                    rows.append({"trial": trial, "w": w, "acked": 0, "copies_acked": 0, "kill_after_ack_ms": "",
                                 "other_copies_at_kill": "", "other_copies_after_250ms": "", "repair_ran_in_window": "",
                                 "lost_with_coordinator_disk": "", "readable_after_restart": "", "vnodes": args.vnodes})
                c.close()
    save("crash", rows)


# ---------------------------------------------------------------------------- re-replication

def repair(args):
    rows = []
    for V in [int(x) for x in args.vnodes_list.split(",")]:
        for run in range(args.runs):
            rng = random.Random(f"repair-{V}-{run}")
            with Ring(16, V, run=240 + run) as ring:
                ring.converge()
                c = Client(ring.addrs[0])
                keys = []
                for i in range(args.chunks):
                    out = c.ask(f"putrand 3 {CHUNK} {70_000_000 + run * 100_000 + i}")
                    if len(out) > 2 and out[2] == "ok":
                        keys.append(out[1])
                c.close()
                before = ring.placement()
                victim = rng.choice(ring.addrs[1:])
                held = [k for k in keys if victim in before.holders(k)]
                time.sleep(2 * ring.period / 1000)
                t0 = time.monotonic()
                ring.kill(victim)
                after = ring.placement()
                t_ring = t_rf = None
                deadline = t0 + 120
                while time.monotonic() < deadline and (t_ring is None or t_rf is None):
                    now = time.monotonic() - t0
                    if t_ring is None and ring_correct(after):
                        t_ring = now
                    if t_rf is None and not under_replicated(after, held):
                        t_rf = time.monotonic() - t0
                    time.sleep(0.05)
                senders = set()
                pat = re.compile(r"repair: \d+ arcs checked, (\d+) chunks sent")
                for a in ring.live():
                    txt = open(ring.log_of(a)).read()
                    if any(int(m.group(1)) > 0 for m in pat.finditer(txt)):
                        senders.add(a)
                row = {"vnodes": V, "run": run, "chunks": len(keys), "victim_held": len(held),
                       "bytes_to_copy": len(held) * CHUNK, "ring_s": round(t_ring, 3) if t_ring else "",
                       "rf_restored_s": round(t_rf, 3) if t_rf else "", "processes_sending": len(senders)}
                rows.append(row)
                print(f"V={V} run {run}: victim held {len(held)} of {len(keys)} chunks; ring correct "
                      f"{row['ring_s']} s; RF = 3 restored {row['rf_restored_s']} s; {len(senders)} processes sent",
                      flush=True)
    save("repair", rows)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--balance", action="store_true")
    ap.add_argument("--wsweep", action="store_true")
    ap.add_argument("--crash", action="store_true")
    ap.add_argument("--repair", action="store_true")
    ap.add_argument("--keys", type=int, default=100_000)
    ap.add_argument("--sets", type=int, default=20)
    ap.add_argument("--check-chunks", type=int, default=2000)
    ap.add_argument("--seconds", type=float, default=20.0)
    ap.add_argument("--vnodes", type=int, default=1)
    ap.add_argument("--vnodes-list", default="1")
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--writes", type=int, default=60, help="writes per W per phase")
    ap.add_argument("--trials", type=int, default=20)
    ap.add_argument("--chunks", type=int, default=300)
    args = ap.parse_args()
    print(f"commit {HEAD}{'  WARNING: working tree is dirty -- these numbers are not reproducible' if DIRTY else ''}",
          flush=True)
    for b in ("node", "chunk", "sim-balance"):
        if not os.access(os.path.join(ROOT, b), os.X_OK):
            sys.exit(f"{b} not built; run make")
    os.makedirs(WORK, exist_ok=True)
    try:
        if args.balance:
            balance(args)
        if args.wsweep:
            wsweep(args)
        if args.crash:
            crash(args)
        if args.repair:
            repair(args)
    finally:
        shutil.rmtree(WORK, ignore_errors=True)


if __name__ == "__main__":
    main()
