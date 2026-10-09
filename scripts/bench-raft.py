#!/usr/bin/env python3
"""bench-raft.py -- the replicated tracker's measurements (decisions D-073, D-082).

Every repetition starts fresh tracker processes in fresh directories. Nothing is
taken from a replica's word about itself except its role and term (STATUS), and
those only to find the leader and to count elections: times come from this
harness's monotonic clock around real requests on fresh connections, as the client
makes them.

Modes (any combination; run in this order):
  --election E[,E...]   for each election-timeout base E (ms): a three-replica group
                        with one writer registering names back to back; the leader is
                        SIGKILLed --kills times (default 20), restarted after each. Per
                        kill: the election time (kill -> a surviving replica leads in a
                        higher term, polled every 5 ms) and the client's unavailability
                        (kill -> the first registration acknowledged after it).
  --spurious E[,E...]   D-073's rule: for each E, --runs (default 3) runs of --seconds
                        (default 30) of a three-replica group under saturating write load
                        (--clients, default 16), no failure injected; counts leader changes.
  --writes              registrations and resolves: the single tracker at its own commit
                        (--old-bin) against groups of 1, 3 and 5 replicas, logs on disk and on
                        tmpfs (where a flush costs nothing), 1 client and --clients clients,
                        --reps repetitions (default 3).
  --replay N[,N...]     start-up of a group of one from a log of N registrations, written
                        directly in the log's format: until a RESOLVE of the last name answers.

The writer follows the client's rules (src/group.cpp, D-081): fresh connection per
request, REDIRECT followed at once, UNAVAILABLE / refused / timeout -> next replica, a
100 ms pause after a round with no answer, 3 s per exchange. Raw rows go to
bench/raw/raft-<label>-*.csv (not committed); a JSON summary to bench/raw/raft-<label>.json.
"""
import argparse, csv, hashlib, json, os, shutil, signal, socket, statistics, subprocess, sys, threading, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASE_PORT = 7900
RAW = os.path.join(ROOT, "bench", "raw")


def key(s):
    return hashlib.sha1(s.encode()).hexdigest()


def exchange(port, line, timeout=3.0):
    """One request on a fresh connection; (reply, kind) where kind is ok/refused/timeout/closed."""
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=timeout) as s:
            s.settimeout(timeout)
            s.sendall(line.encode() + b"\n")
            r = s.makefile("rb").readline()
            if not r:
                return "", "closed"
            return r.decode().strip(), "ok"
    except ConnectionRefusedError:
        return "", "refused"
    except socket.timeout:
        return "", "timeout"
    except OSError:
        return "", "closed"


class GroupClient:
    """The client's leader-finding loop (group.cpp), in Python for the harness."""

    def __init__(self, ports, wait=10.0):
        self.ports, self.wait, self.preferred = ports, wait, 0

    def ask(self, line):
        end = time.monotonic() + self.wait
        n = len(self.ports)
        while time.monotonic() < end:
            for k in range(n):
                port = self.ports[(self.preferred + k) % n]
                for _ in range(n + 1):
                    reply, kind = exchange(port, line)
                    if kind != "ok":
                        break
                    if reply.startswith("REDIRECT "):
                        port = int(reply.rsplit(":", 1)[1])
                        continue
                    if reply.startswith("UNAVAILABLE"):
                        break
                    if port in self.ports:
                        self.preferred = self.ports.index(port)
                    return reply
            time.sleep(min(0.1, max(0.0, end - time.monotonic())))
        return None


class Group:
    def __init__(self, binary, n, election, workdir, old=False):
        self.binary, self.n, self.election, self.old = binary, n, election, old
        self.ports = [BASE_PORT + i for i in range(n)]
        self.dirs = [os.path.join(workdir, f"r{i}") for i in range(n)]
        self.procs = {}
        for d in self.dirs:
            shutil.rmtree(d, ignore_errors=True)
            os.makedirs(d)
        self.list = ",".join(f"127.0.0.1:{p}" for p in self.ports)

    def start(self, i):
        if self.old:
            cmd = [self.binary, str(self.ports[i]), "--dir", self.dirs[i]]
        elif self.n == 1:
            cmd = [self.binary, str(self.ports[i]), "--dir", self.dirs[i], "--ip", "127.0.0.1", "--election", str(self.election)]
        else:
            cmd = [self.binary, str(self.ports[i]), "--dir", self.dirs[i], "--ip", "127.0.0.1", "--group", self.list,
                   "--id", str(i), "--election", str(self.election)]
        self.procs[i] = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=open(os.path.join(self.dirs[i], "err.log"), "a"))

    def start_all(self):
        for i in range(self.n):
            self.start(i)
        c = GroupClient(self.ports, wait=15)
        if c.ask("REGISTER warmup " + key("warmup")) not in ("OK",):
            raise RuntimeError("the group did not come up")

    def kill(self, i):
        p = self.procs.get(i)
        if p and p.poll() is None:
            p.kill()
            p.wait()

    def stop(self):
        for i in list(self.procs):
            self.kill(i)

    def status(self, i):
        w = exchange(self.ports[i], "STATUS", 1.0)[0].split()
        return w if len(w) == 8 else None

    def leader(self, exclude=(), within=10.0):
        end = time.monotonic() + within
        while time.monotonic() < end:
            best = None
            for i in range(self.n):
                if i in exclude:
                    continue
                s = self.status(i)
                if s and s[2] == "leader" and (best is None or int(s[3]) > best[1]):
                    best = (i, int(s[3]))
            if best:
                return best
            time.sleep(0.005)
        return None

    def converged(self, within=10.0):
        end = time.monotonic() + within
        while time.monotonic() < end:
            lt = self.leader(within=2)
            if lt:
                ls = self.status(lt[0])
                if ls and all(int((self.status(i) or ['0'] * 8)[6]) >= int(ls[5]) for i in range(self.n) if self.procs[i].poll() is None):
                    return True
            time.sleep(0.02)
        return False


def pct(xs, p):
    xs = sorted(xs)
    if not xs:
        return float("nan")
    k = (len(xs) - 1) * p / 100
    f = int(k)
    return xs[f] if f + 1 >= len(xs) else xs[f] + (xs[f + 1] - xs[f]) * (k - f)


def mode_election(a, summary, label):
    rows = []
    for e in a.election:
        print(f"== election: E = {e} ms, {a.kills} leader kills", flush=True)
        g = Group(a.bin, 3, e, os.path.join(a.work, f"el-{e}"))
        try:
            g.start_all()
            done = threading.Event()
            acks = []                                # completion times of acknowledged writes
            lock = threading.Lock()

            def writer():
                c, n = GroupClient(g.ports, wait=30), 0
                while not done.is_set():
                    name = f"e{e}-{n:06d}"
                    if c.ask(f"REGISTER {name} {key(name)}") == "OK":
                        with lock:
                            acks.append(time.monotonic())
                    n += 1

            th = threading.Thread(target=writer)
            th.start()
            for k in range(a.kills):
                time.sleep(0.5)
                lt = g.leader()
                if not lt:
                    raise RuntimeError("no leader before a kill")
                old, old_term = lt
                t0 = time.monotonic()
                g.kill(old)
                new = None
                while time.monotonic() - t0 < 30:
                    for i in range(3):
                        if i == old:
                            continue
                        s = g.status(i)
                        if s and s[2] == "leader" and int(s[3]) > old_term:
                            new = (i, int(s[3]), time.monotonic())
                            break
                    if new:
                        break
                    time.sleep(0.005)
                elect_ms = (new[2] - t0) * 1000 if new else float("nan")
                while time.monotonic() - t0 < 30:
                    with lock:
                        after = [t for t in acks if t > t0]
                    if after:
                        break
                    time.sleep(0.005)
                with lock:
                    before = [t for t in acks if t <= t0]
                    after = [t for t in acks if t > t0]
                unavail_ms = (after[0] - t0) * 1000 if after else float("nan")
                gap_ms = (after[0] - before[-1]) * 1000 if after and before else float("nan")
                rows.append({"election": e, "kill": k, "elect_ms": round(elect_ms, 1), "unavailable_ms": round(unavail_ms, 1),
                             "gap_ms": round(gap_ms, 1), "terms": new[1] - old_term if new else -1})
                print(f"   kill {k:2d}: elected after {elect_ms:7.1f} ms (term +{rows[-1]['terms']}), "
                      f"first write {unavail_ms:7.1f} ms after the kill, gap {gap_ms:7.1f} ms", flush=True)
                g.start(old)
                g.converged()
            done.set()
            th.join()
        finally:
            g.stop()
        el = [r["elect_ms"] for r in rows if r["election"] == e]
        un = [r["unavailable_ms"] for r in rows if r["election"] == e]
        summary.setdefault("election", {})[e] = {
            "kills": len(el), "elect_p50": pct(el, 50), "elect_p99": pct(el, 99), "elect_min": min(el), "elect_max": max(el),
            "unavail_p50": pct(un, 50), "unavail_p99": pct(un, 99), "unavail_min": min(un), "unavail_max": max(un),
            "split_votes": sum(1 for r in rows if r["election"] == e and r["terms"] > 1)}
        print(f"   E={e}: election p50 {pct(el, 50):.0f} ms p99 {pct(el, 99):.0f} (min {min(el):.0f}, max {max(el):.0f}); "
              f"unavailable p50 {pct(un, 50):.0f} p99 {pct(un, 99):.0f}", flush=True)
    write_csv(f"raft-{label}-election.csv", rows)


def load(g, clients, seconds, prefix, stop_at=None):
    """`clients` threads registering distinct names back to back for `seconds`; returns per-request latencies (s)."""
    lat, lock, done = [], threading.Lock(), threading.Event()

    def run(c):
        cl, n = GroupClient(g.ports, wait=30), 0
        while not done.is_set():
            name = f"{prefix}-{c}-{n:07d}"
            t = time.monotonic()
            r = cl.ask(f"REGISTER {name} {key(name)}")
            if r == "OK":
                with lock:
                    lat.append(time.monotonic() - t)
            n += 1
            if stop_at and n >= stop_at:
                break

    ths = [threading.Thread(target=run, args=(c,)) for c in range(clients)]
    t0 = time.monotonic()
    for t in ths:
        t.start()
    if stop_at:
        for t in ths:
            t.join()
    else:
        time.sleep(seconds)
        done.set()
        for t in ths:
            t.join()
    return lat, time.monotonic() - t0


def mode_spurious(a, summary, label):
    rows = []
    for e in a.spurious:
        for run in range(a.runs):
            g = Group(a.bin, 3, e, os.path.join(a.work, f"sp-{e}-{run}"))
            try:
                g.start_all()
                lt = g.leader()
                terms_seen, stop = {lt[1]}, threading.Event()

                def watch():
                    while not stop.is_set():
                        for i in range(3):
                            s = g.status(i)
                            if s and s[2] == "leader":
                                terms_seen.add(int(s[3]))
                        time.sleep(0.05)

                w = threading.Thread(target=watch)
                w.start()
                lat, wall = load(g, a.clients, a.seconds, f"sp{e}r{run}")
                stop.set()
                w.join()
                final = max(int((g.status(i) or [0, 0, 0, 0])[3]) for i in range(3))
                rows.append({"election": e, "run": run, "seconds": round(wall, 1), "writes": len(lat),
                             "per_second": round(len(lat) / wall, 1), "leader_changes": len(terms_seen) - 1,
                             "term_increase": final - lt[1]})
                print(f"== spurious E={e} run {run}: {len(lat)} writes in {wall:.1f} s ({len(lat) / wall:.0f}/s), "
                      f"leader changes {len(terms_seen) - 1}, term +{final - lt[1]}", flush=True)
            finally:
                g.stop()
        rs = [r for r in rows if r["election"] == e]
        summary.setdefault("spurious", {})[e] = {"runs": len(rs), "leader_changes": sum(r["leader_changes"] for r in rs),
                                                 "term_increase": sum(r["term_increase"] for r in rs),
                                                 "per_second": statistics.median(r["per_second"] for r in rs)}
    write_csv(f"raft-{label}-spurious.csv", rows)


def mode_writes(a, summary, label):
    configs = []
    if a.old_bin:
        configs.append(("single tracker (before)", a.old_bin, 1, True))
    configs += [("group of 1", a.bin, 1, False), ("group of 3", a.bin, 3, False), ("group of 5", a.bin, 5, False)]
    rows = []
    for rep in range(a.reps):
        for name, binary, n, old in configs:
            for media in ("disk", "tmpfs"):
                base = a.work if media == "disk" else f"/dev/shm/raft-bench-{os.getpid()}"
                for clients in (1, a.clients):
                    g = Group(binary, n, a.default_election, os.path.join(base, "w"), old=old)
                    try:
                        if old:
                            for i in range(n):
                                g.start(i)
                            while exchange(g.ports[0], "PING")[1] != "ok":
                                time.sleep(0.02)
                        else:
                            g.start_all()
                        per_client = a.writes // clients
                        lat, wall = load(g, clients, 0, f"w{rep}", stop_at=per_client)
                        # reads: one client resolving names that exist
                        c, rl = GroupClient(g.ports), []
                        for i in range(a.reads):
                            nm = f"w{rep}-0-{i % per_client:07d}"
                            t = time.monotonic()
                            r = c.ask(f"RESOLVE {nm}")
                            if r and r.startswith("FILE "):
                                rl.append(time.monotonic() - t)
                        row = {"config": name, "replicas": n, "media": media, "clients": clients, "rep": rep,
                               "writes": len(lat), "seconds": round(wall, 3), "per_second": round(len(lat) / wall, 1),
                               "w_p50_ms": round(pct(lat, 50) * 1000, 2), "w_p99_ms": round(pct(lat, 99) * 1000, 2),
                               "reads": len(rl), "r_p50_ms": round(pct(rl, 50) * 1000, 3), "r_p99_ms": round(pct(rl, 99) * 1000, 3)}
                        rows.append(row)
                        print(f"   rep {rep} {name:<24} {media:<5} {clients:>2} clients: {row['per_second']:7.1f} writes/s, "
                              f"p50 {row['w_p50_ms']:6.2f} ms p99 {row['w_p99_ms']:7.2f} ms; resolve p50 {row['r_p50_ms']:.3f} "
                              f"p99 {row['r_p99_ms']:.3f} ms", flush=True)
                    finally:
                        g.stop()
                        shutil.rmtree(os.path.join(base, "w"), ignore_errors=True)
                if media == "tmpfs":
                    shutil.rmtree(base, ignore_errors=True)
    write_csv(f"raft-{label}-writes.csv", rows)
    out = {}
    for name, _, n, _ in configs:
        for media in ("disk", "tmpfs"):
            for clients in (1, a.clients):
                rs = [r for r in rows if r["config"] == name and r["media"] == media and r["clients"] == clients]
                if not rs:
                    continue
                med = lambda k: statistics.median(r[k] for r in rs)
                out[f"{name}|{media}|{clients}"] = {k: med(k) for k in ("per_second", "w_p50_ms", "w_p99_ms", "r_p50_ms", "r_p99_ms")} | {
                    "spread": round((max(r["per_second"] for r in rs) - min(r["per_second"] for r in rs)) / med("per_second") * 100, 1)}
    summary["writes"] = out


def mode_replay(a, summary, label):
    out = {}
    for n in a.replay:
        d = os.path.join(a.work, f"replay-{n}")
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d)
        port = BASE_PORT
        rec = lambda b: b + " " + key(b)[:8] + "\n"
        with open(os.path.join(d, f"raft_{port}.log"), "w") as f:
            f.write(rec("TERM 1 0"))
            f.write(rec("ENTRY 1 1 NOOP"))
            for i in range(n):
                nm = f"file-{i:08d}.dat"
                f.write(rec(f"ENTRY {i + 2} 1 REGISTER {nm} {key(nm)}"))
        size = os.path.getsize(os.path.join(d, f"raft_{port}.log"))
        last = f"file-{n - 1:08d}.dat"
        t0 = time.monotonic()
        p = subprocess.Popen([a.bin, str(port), "--dir", d, "--ip", "127.0.0.1"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        while exchange(port, f"RESOLVE {last}", 1.0)[0] != f"FILE {key(last)}":
            time.sleep(0.005)
        took = time.monotonic() - t0
        rss = int([l for l in open(f"/proc/{p.pid}/status") if l.startswith("VmRSS")][0].split()[1])
        p.kill()
        p.wait()
        shutil.rmtree(d, ignore_errors=True)
        out[n] = {"log_bytes": size, "bytes_per_name": round(size / n, 1), "seconds": round(took, 3), "rss_kib": rss}
        print(f"== replay {n} names: log {size} B ({size / n:.1f} B/name), answering after {took:.3f} s, RSS {rss} KiB", flush=True)
    summary["replay"] = out


def write_csv(name, rows):
    if not rows:
        return
    os.makedirs(RAW, exist_ok=True)
    with open(os.path.join(RAW, name), "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("label", help="usually the commit")
    ap.add_argument("--bin", default=os.path.join(ROOT, "tracker"))
    ap.add_argument("--old-bin", default="", help="the single tracker built at its own commit")
    ap.add_argument("--work", default=os.path.join(ROOT, "bench", "work-raft"))
    ap.add_argument("--election", default="")
    ap.add_argument("--kills", type=int, default=20)
    ap.add_argument("--spurious", default="")
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--seconds", type=float, default=30)
    ap.add_argument("--clients", type=int, default=16)
    ap.add_argument("--writes", action="store_true")
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--write-count", dest="write_count", type=int, default=1600)
    ap.add_argument("--reads", type=int, default=500)
    ap.add_argument("--default-election", type=int, default=300)
    ap.add_argument("--replay", default="")
    a = ap.parse_args()
    a.election = [int(x) for x in a.election.split(",") if x]
    a.spurious = [int(x) for x in a.spurious.split(",") if x]
    a.replay = [int(x) for x in a.replay.split(",") if x]
    a.writes_mode, a.writes = a.writes, a.write_count
    os.makedirs(a.work, exist_ok=True)
    summary = {"label": a.label}
    signal.signal(signal.SIGINT, signal.default_int_handler)
    if a.election:
        mode_election(a, summary, a.label)
    if a.spurious:
        mode_spurious(a, summary, a.label)
    if a.writes_mode:
        mode_writes(a, summary, a.label)
    if a.replay:
        mode_replay(a, summary, a.label)
    os.makedirs(RAW, exist_ok=True)
    path = os.path.join(RAW, f"raft-{a.label}.json")
    old = json.load(open(path)) if os.path.exists(path) else {}
    old.update(summary)
    json.dump(old, open(path, "w"), indent=1, default=str)
    print(json.dumps(summary, indent=1, default=str))


if __name__ == "__main__":
    main()
