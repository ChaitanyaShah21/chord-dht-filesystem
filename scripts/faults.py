#!/usr/bin/env python3
"""faults.py -- uploads and downloads while processes are killed, frozen and restarted (plan phase 14).

    scripts/faults.py <label> [--seconds 180] [--seed 1] [--nodes 6] [--netem] [--profile network] [--keep]

Starts a cluster with scripts/cluster.sh (three tracker replicas, --nodes node processes x 8
members, metrics on), waits for the ring oracle, then for --seconds runs a workload and a fault
schedule side by side, and afterwards checks invariants from outside the system.

  workload   two uploaders (files of 256 KiB to 3 MiB, random bytes, each under a new name) and
             two downloaders (a random acknowledged name, fetched and compared byte for byte with
             what was uploaded), all the real `client`, each operation recorded with its outcome
  faults     every 8-15 s (seeded) one of: SIGKILL a node and restart it from its store 5-10 s
             later; SIGSTOP a node for 4 s; SIGKILL the tracker leader and restart it 3 s later;
             SIGSTOP a tracker follower for 3 s; and with --netem, 5 % packet loss with 10 ms +- 5 ms
             of delay on every connection for 10 s. At most one node and one tracker replica are
             down or frozen at a time.
  --netem    runs everything inside a new user and network namespace (`unshare -rn`), where the
             harness may change the loopback interface's queueing (netem) without being root
  --profile network
             the real-network timer profile (D-095): every node's call deadline 1,200 ms and the
             tracker's election base 1,200 ms, above a SYN's 1 s retransmission timeout

Also counted from every process's structured log (not judged -- the invariants judge): coordinated
writes that failed and were retried by the client, strikes, evictions by evidence, tracker elections.

Invariants, checked after the faults stop and every process is back:
  I-a  every acknowledged upload resolves to the key its upload reported (no acknowledged name lost)
  I-b  every acknowledged upload downloads byte for byte (no acknowledged data lost)
  I-c  every chunk of every acknowledged upload is back on its three correct processes within 60 s
  I-d  no download during the run returned bytes different from what was uploaded
Writes bench/raw/faults-<label>-ops.csv and -faults.csv (not committed) and prints a JSON summary.
"""
import argparse, csv, hashlib, json, os, random, shutil, signal, socket, subprocess, sys, threading, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
from chunkcheck import Placement, ring_correct, under_replicated   # noqa: E402

TRACKER_PORT, NODE_BASE, OFFSET, VNODES = 7000, 7010, 1000, 8
CHUNK = 512 * 1024


def listening(port):
    try:
        socket.create_connection(("127.0.0.1", port), timeout=0.5).close()
        return True
    except OSError:
        return False


def status(port):
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=1) as s:
            s.sendall(b"STATUS\n")
            return s.makefile("rb").readline().decode().split()
    except OSError:
        return []


class Run:
    def __init__(self, a):
        self.a = a
        self.dir = os.path.join(a.scratch, "cluster")
        self.t0 = None
        self.lock = threading.Lock()
        self.ops, self.faults = [], []
        self.acked = {}                      # name -> (key, sha1, path)
        self.mismatches = 0
        self.stop = threading.Event()
        self.trackers = [TRACKER_PORT + i for i in range(3)]
        self.nodes = [NODE_BASE + i for i in range(a.nodes)]
        self.group = ",".join(f"127.0.0.1:{p}" for p in self.trackers)
        self.ring = ",".join(f"127.0.0.1:{p}" for p in self.nodes[:3])
        # The timer profile (D-095): the loopback defaults, or deadlines above a SYN's retransmission.
        self.node_extra = ["--timeout", "1200"] if a.profile == "network" else []
        self.tracker_extra = ["--election", "1200"] if a.profile == "network" else []
        self.env = dict(os.environ, CLUSTER_DIR=self.dir, TRACKERS="3", TRACKER_PORT=str(TRACKER_PORT),
                        NODE_BASE=str(NODE_BASE), VNODES=str(VNODES), PERIOD="1000", METRICS_OFFSET=str(OFFSET),
                        NODE_ARGS=" ".join(["--scan", "10"] + self.node_extra), TRACKER_ARGS=" ".join(self.tracker_extra))
        self.procs = {}                      # port -> pid
        self.down = {}                       # port -> what is wrong with it
        self.rng = random.Random(a.seed)

    # ---- the cluster ----
    def up(self):
        shutil.rmtree(self.dir, ignore_errors=True)
        r = subprocess.run([f"{ROOT}/scripts/cluster.sh", "up", str(self.a.nodes)], env=self.env, capture_output=True, timeout=300)
        if r.returncode != 0:
            raise RuntimeError("cluster.sh up failed: " + r.stdout.decode()[-300:])
        for line in open(os.path.join(self.dir, "pids")):
            pid, what = line.split()
            self.procs[int(what.split("-")[1])] = int(pid)
        self.wait_ring(120)
        time.sleep(3)

    def wait_ring(self, seconds):
        placement = Placement([f"127.0.0.1:{p}" for p in self.nodes if p not in self.down], VNODES)
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if ring_correct(placement):
                return True
            time.sleep(0.25)
        return False

    def down_all(self):
        for pid in self.procs.values():
            for sig in (signal.SIGCONT, signal.SIGKILL):
                try:
                    os.kill(pid, sig)
                except ProcessLookupError:
                    pass
        subprocess.run([f"{ROOT}/scripts/cluster.sh", "down"], env=self.env, capture_output=True, timeout=120)

    def start_node(self, port):
        others = [p for p in self.nodes if p != port and p not in self.down]
        log = open(os.path.join(self.dir, "logs", f"node-{port}.log"), "a")
        p = subprocess.Popen([f"{ROOT}/node", "127.0.0.1", str(port), "--join", f"127.0.0.1:{others[0]}", "--vnodes", str(VNODES),
                              "--period", "1000", "--store", os.path.join(self.dir, f"store-{port}"), "--metrics",
                              str(port + OFFSET), "--scan", "10"] + self.node_extra, stdout=subprocess.DEVNULL, stderr=log,
                             start_new_session=True)
        self.procs[port] = p.pid

    def start_tracker(self, port):
        i = self.trackers.index(port)
        log = open(os.path.join(self.dir, "logs", f"tracker-{port}.log"), "a")
        p = subprocess.Popen([f"{ROOT}/tracker", str(port), "--dir", os.path.join(self.dir, f"tracker-{port}"), "--ip", "127.0.0.1",
                              "--group", self.group, "--id", str(i), "--metrics", str(port + OFFSET)] + self.tracker_extra,
                             stdout=subprocess.DEVNULL, stderr=log, start_new_session=True)
        self.procs[port] = p.pid

    def leader(self):
        for p in self.trackers:
            st = status(p) if p not in self.down else []
            if len(st) > 2 and st[2] == "leader":
                return p
        return None

    def now(self):
        return time.monotonic() - self.t0

    def mark(self, kind, target, start, end):
        with self.lock:
            self.faults.append({"kind": kind, "target": target, "start_s": round(start, 3), "end_s": round(end, 3)})
        print(f"  {start:7.1f} s  {kind:<16} {target}  until {end:.1f} s", flush=True)

    # ---- the workload ----
    def client(self, *args):
        return subprocess.run([f"{ROOT}/client", "--tracker", self.group, "--ring", self.ring, "--quiet"] + list(args),
                              capture_output=True, timeout=120)

    def record(self, op, t1, t2, ok, size, note=""):
        with self.lock:
            self.ops.append({"op": op, "start_s": round(t1, 3), "end_s": round(t2, 3), "ok": int(ok), "bytes": size,
                             "note": note[:160].replace("\n", " ")})

    def uploader(self, w):
        i = 0
        rng = random.Random(self.a.seed * 100 + w)
        while not self.stop.is_set():
            i += 1
            name = f"u{w}-{i}"
            data = rng.randbytes(rng.randint(256 * 1024, 3 * 1024 * 1024))
            path = os.path.join(self.a.scratch, "files", name)
            with open(path, "wb") as f:
                f.write(data)
            t1 = self.now()
            r = self.client("put", path, name)
            t2 = self.now()
            ok = r.returncode == 0
            if ok:
                with self.lock:
                    self.acked[name] = (r.stdout.decode().split()[2], hashlib.sha1(data).hexdigest(), path)
            self.record("put", t1, t2, ok, len(data), "" if ok else r.stderr.decode().strip())

    def downloader(self, w):
        rng = random.Random(self.a.seed * 1000 + w)
        while not self.stop.is_set():
            with self.lock:
                names = list(self.acked)
            if not names:
                time.sleep(0.2)
                continue
            name = rng.choice(names)
            key, sha, _ = self.acked[name]
            dest = os.path.join(self.a.scratch, f"dl-{w}")
            t1 = self.now()
            r = self.client("get", name, dest)
            t2 = self.now()
            ok = r.returncode == 0
            size = os.path.getsize(dest) if ok else 0
            if ok and hashlib.sha1(open(dest, "rb").read()).hexdigest() != sha:
                with self.lock:
                    self.mismatches += 1
                self.record("get", t1, t2, False, size, "BYTES DIFFER")
                continue
            self.record("get", t1, t2, ok, size, "" if ok else r.stderr.decode().strip())

    # ---- the faults ----
    def netem(self, on):
        if on:
            subprocess.run(["tc", "qdisc", "replace", "dev", "lo", "root", "netem", "delay", "10ms", "5ms", "loss", "5%"], check=True)
        else:
            subprocess.run(["tc", "qdisc", "del", "dev", "lo", "root"], capture_output=True)

    def injector(self):
        # With --netem, the network fault is drawn a third of the time (a seed once drew none at 1 in 5).
        kinds = ["kill-node", "stop-node", "kill-leader", "stop-follower"] + (["netem", "netem"] if self.a.netem else [])
        pending = []                          # (time, action)
        next_fault = 10.0
        while not self.stop.is_set():
            t = self.now()
            for due, act in [p for p in pending if p[0] <= t]:
                pending.remove((due, act))
                act()
            if t >= next_fault and t < self.a.seconds - 20:
                kind = self.rng.choice(kinds)
                busy_node = any(p in self.nodes for p in self.down)
                busy_tracker = any(p in self.trackers for p in self.down)
                if kind in ("kill-node", "stop-node") and not busy_node:
                    port = self.rng.choice(self.nodes)
                    pid = self.procs[port]
                    if kind == "kill-node":
                        os.kill(pid, signal.SIGKILL)
                        self.down[port] = "dead"
                        back = t + self.rng.uniform(5, 10)
                        def restart(port=port):
                            self.down.pop(port, None)
                            self.start_node(port)
                        pending.append((back, restart))
                    else:
                        os.kill(pid, signal.SIGSTOP)
                        self.down[port] = "frozen"
                        back = t + 4
                        def thaw(port=port, pid=pid):
                            os.kill(pid, signal.SIGCONT)
                            self.down.pop(port, None)
                        pending.append((back, thaw))
                    self.mark(kind, port, t, back)
                elif kind == "kill-leader" and not busy_tracker and self.leader():
                    port = self.leader()
                    os.kill(self.procs[port], signal.SIGKILL)
                    self.down[port] = "dead"
                    def restart_t(port=port):
                        self.down.pop(port, None)
                        self.start_tracker(port)
                    pending.append((t + 3, restart_t))
                    self.mark(kind, port, t, t + 3)
                elif kind == "stop-follower" and not busy_tracker and self.leader():
                    port = self.rng.choice([p for p in self.trackers if p != self.leader()])
                    pid = self.procs[port]
                    os.kill(pid, signal.SIGSTOP)
                    self.down[port] = "frozen"
                    def thaw_t(port=port, pid=pid):
                        os.kill(pid, signal.SIGCONT)
                        self.down.pop(port, None)
                    pending.append((t + 3, thaw_t))
                    self.mark(kind, port, t, t + 3)
                elif kind == "netem" and "lo" not in self.down:
                    self.netem(True)
                    self.down["lo"] = "netem"
                    def off():
                        self.netem(False)
                        self.down.pop("lo", None)
                    pending.append((t + 10, off))
                    self.mark(kind, "lo", t, t + 10)
                next_fault = t + self.rng.uniform(8, 15)
            if t >= self.a.seconds and not pending:
                break
            time.sleep(0.05)
        for _, act in sorted(pending, key=lambda p: p[0]):
            act()

    # ---- what the processes logged (counted, not judged: the invariants judge) ----
    def log_counts(self):
        """Coordinated writes that failed (the client retried them), strikes, evictions by
        evidence, and tracker elections, from every process's structured log (D-086)."""
        c = {"puts_failed_at_coordinator": 0, "strikes": 0, "evictions_refused": 0, "evictions_strikes": 0,
             "tracker_candidacies": 0, "tracker_max_term": 0}
        logs = os.path.join(self.dir, "logs")
        for name in os.listdir(logs):
            for line in open(os.path.join(logs, name), errors="replace"):
                if "event=chunk.put" in line and "result=error" in line:
                    c["puts_failed_at_coordinator"] += 1
                elif 'msg="strike ' in line:
                    c["strikes"] += 1
                elif 'msg="evict ' in line:
                    c["evictions_strikes" if "second strike" in line else "evictions_refused"] += 1
                elif "event=raft.role" in line:
                    c["tracker_candidacies"] += "role=candidate" in line
                    term = int(line.split("term=")[1].split()[0])
                    c["tracker_max_term"] = max(c["tracker_max_term"], term)
        return c

    # ---- invariants ----
    def chunk_keys(self, name):
        key, _, path = self.acked[name]
        data = open(path, "rb").read()
        ks = [hashlib.sha1(data[i:i + CHUNK]).hexdigest() for i in range(0, len(data), CHUNK)]
        return ks + [key]                     # files here are one manifest: the root is the only one

    def check(self):
        out = {}
        healed = time.monotonic()
        out["ring_converged"] = self.wait_ring(120)
        out["ring_converged_s"] = round(time.monotonic() - healed, 2)
        placement = Placement([f"127.0.0.1:{p}" for p in self.nodes], VNODES)
        keys = sorted({k for n in self.acked for k in self.chunk_keys(n)})
        t1 = time.monotonic()
        missing = under_replicated(placement, keys)
        while missing and time.monotonic() - t1 < 60:
            time.sleep(0.5)
            missing = under_replicated(placement, keys)
        out["chunks"] = len(keys)
        out["rf_restored"] = not missing
        out["rf_restored_s"] = round(time.monotonic() - t1, 2) if not missing else None
        out["still_short"] = len(missing)
        lost_names, wrong_bytes = [], []
        for name, (key, sha, _) in sorted(self.acked.items()):
            r = self.client("resolve", name)
            if r.returncode != 0 or r.stdout.decode().strip() != key:
                lost_names.append(name)
                continue
            dest = os.path.join(self.a.scratch, "final")
            g = self.client("get", name, dest)
            if g.returncode != 0 or hashlib.sha1(open(dest, "rb").read()).hexdigest() != sha:
                wrong_bytes.append(name)
        out.update(self.log_counts())
        out["acked"] = len(self.acked)
        out["I-a_names_lost"] = lost_names
        out["I-b_not_downloadable"] = wrong_bytes
        out["I-d_mismatched_downloads"] = self.mismatches
        return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("label")
    ap.add_argument("--seconds", type=int, default=180)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--nodes", type=int, default=6)
    ap.add_argument("--netem", action="store_true")
    ap.add_argument("--profile", choices=["loopback", "network"], default="loopback")
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--scratch", default=os.path.join(os.environ.get("TMPDIR", "/tmp"), "faults"))
    ap.add_argument("--inside", action="store_true", help=argparse.SUPPRESS)
    a = ap.parse_args()
    if a.netem and not a.inside:
        # Re-run this script inside a new user and network namespace: its own loopback,
        # which the harness may shape with netem without being root.
        cmd = ["unshare", "-rn", "sh", "-c", 'ip link set lo up && exec "$0" "$@"', sys.executable] + sys.argv + ["--inside"]
        return subprocess.call(cmd)
    shutil.rmtree(a.scratch, ignore_errors=True)
    os.makedirs(os.path.join(a.scratch, "files"))
    run = Run(a)
    run.up()
    print(f"cluster up: 3 tracker replicas, {a.nodes} nodes x {VNODES}; {a.seconds} s of workload and faults, seed {a.seed}", flush=True)
    run.t0 = time.monotonic()
    workers = [threading.Thread(target=run.uploader, args=(w,)) for w in range(2)] + \
              [threading.Thread(target=run.downloader, args=(w,)) for w in range(2)]
    for th in workers:
        th.start()
    try:
        run.injector()
        while run.now() < a.seconds:
            time.sleep(0.2)
        run.stop.set()
        for th in workers:
            th.join()
        summary = run.check()
    finally:
        if not a.keep:
            run.down_all()
    ops = run.ops
    puts, gets = [o for o in ops if o["op"] == "put"], [o for o in ops if o["op"] == "get"]
    summary.update({"label": a.label, "seed": a.seed, "seconds": a.seconds, "netem": a.netem, "profile": a.profile,
                    "faults": len(run.faults),
                    "puts": len(puts), "puts_ok": sum(o["ok"] for o in puts), "gets": len(gets), "gets_ok": sum(o["ok"] for o in gets)})
    raw = os.path.join(ROOT, "bench", "raw")
    os.makedirs(raw, exist_ok=True)
    for name, rows, fields in ((f"faults-{a.label}-ops.csv", ops, ["op", "start_s", "end_s", "ok", "bytes", "note"]),
                               (f"faults-{a.label}-faults.csv", run.faults, ["kind", "target", "start_s", "end_s"])):
        with open(os.path.join(raw, name), "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=fields)
            w.writeheader()
            w.writerows(rows)
    print("SUMMARY " + json.dumps(summary), flush=True)
    violated = summary["I-a_names_lost"] or summary["I-b_not_downloadable"] or summary["I-d_mismatched_downloads"] or \
        not summary["rf_restored"]
    return 1 if violated else 0


if __name__ == "__main__":
    sys.exit(main())
