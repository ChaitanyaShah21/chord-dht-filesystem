#!/usr/bin/env python3
"""bench-gc.py -- deletion's numbers (decisions D-089 to D-092).

    scripts/bench-gc.py <label> [--reps 3] [--chunks 1000,4000,16000] [--reclaim]

Every repetition starts a fresh cluster with scripts/cluster.sh -- three tracker replicas,
8 node processes x 8 ring members, a collection key, a 6 s grace -- and drives it with the
real client and the real `gc`. Nothing measured is taken from a process's word about itself
except the collector's own report, which is checked against the stores on disk.

  --reclaim   20 files of 4 MiB (8 chunks each), every pair sharing half its chunks; delete
              10 of them, wait out the grace, collect. Measures the bytes on disk before and
              after (every store directory walked), the bytes the collector says it freed,
              what was left after a second collection, and checks every remaining file still
              downloads whole. Run with repair's periodic full scan on (the default) -- a
              copy dropped on one replica may be put back from another not yet swept.
  --chunks N  one file of N chunks of 4 KiB (small chunks: a collection's work is per chunk,
              not per byte), deleted; the collection's mark and sweep times against N.

Raw rows go to bench/raw/gc-<label>.csv (not committed); a JSON summary is printed at the end.
"""
import argparse, csv, json, os, random, shutil, statistics, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
from chunkcheck import Placement, ring_correct   # noqa: E402

TRACKER_PORT, NODE_BASE = 7000, 7010
GRACE = 6
MB = 1 << 20


class Cluster:
    def __init__(self, work, nodes=8, args=""):
        self.dir, self.nodes = work, nodes
        self.key = os.path.join(work + "-key")
        with open(self.key, "w") as f:
            f.write("ab" * 20 + "\n")
        self.env = dict(os.environ, CLUSTER_DIR=work, TRACKERS="3", TRACKER_PORT=str(TRACKER_PORT), NODE_BASE=str(NODE_BASE),
                        VNODES="8", PERIOD="1000", METRICS_OFFSET="0",
                        NODE_ARGS=f"--gc-key {self.key} --gc-grace {GRACE} {args}")
        self.group = ",".join(f"127.0.0.1:{TRACKER_PORT + i}" for i in range(3))
        self.ring = f"127.0.0.1:{NODE_BASE},127.0.0.1:{NODE_BASE + 1}"

    def __enter__(self):
        shutil.rmtree(self.dir, ignore_errors=True)
        r = subprocess.run([f"{ROOT}/scripts/cluster.sh", "up", str(self.nodes)], env=self.env, capture_output=True, timeout=300)
        if r.returncode != 0:
            raise RuntimeError("cluster.sh up failed: " + r.stdout.decode()[-400:])
        # The nodes joined one after another: wait for the ring oracle, then three periods
        # for the successor lists, before writing anything (see BENCHMARKS.md §8: a chunk
        # written while the ring converges can land where the converged ring never asks).
        placement = Placement([f"127.0.0.1:{NODE_BASE + i}" for i in range(self.nodes)], 8)
        deadline = time.monotonic() + 120
        while time.monotonic() < deadline and not ring_correct(placement):
            time.sleep(0.5)
        if not ring_correct(placement):
            raise RuntimeError("the ring did not converge")
        time.sleep(3)
        return self

    def __exit__(self, *exc):
        subprocess.run([f"{ROOT}/scripts/cluster.sh", "down"], env=self.env, capture_output=True, timeout=120)

    def client(self, *args):
        r = subprocess.run([f"{ROOT}/client", "--tracker", self.group, "--ring", self.ring, "--quiet", "--tracker-wait", "2000"]
                           + list(args), capture_output=True, timeout=900)
        if r.returncode != 0:
            raise RuntimeError("client " + " ".join(args[:2]) + " failed: " + r.stderr.decode()[-300:])
        return r.stdout.decode()

    def gc(self):
        t0 = time.monotonic()
        r = subprocess.run([f"{ROOT}/gc", "--tracker", self.group, "--ring", self.ring, "--gc-key", self.key, "--quiet",
                            "--tracker-wait", "2000", "--assume-upload-limit", "1", "--deadline", "2"],
                           capture_output=True, timeout=1800)
        wall = (time.monotonic() - t0) * 1000
        out = r.stdout.decode().split()
        if r.returncode != 0 or out[:2] != ["gc", "ok"]:
            raise RuntimeError("gc failed: " + r.stderr.decode()[-300:])
        n = {k: float(v) for k, v in zip(out[2::2], out[3::2]) if k != "dry-run"}
        n["wall_ms"] = wall
        return n

    def disk(self):
        """Bytes of chunk files in every store, and how many files."""
        total = files = 0
        for i in range(self.nodes):
            for dirpath, _, names in os.walk(os.path.join(self.dir, f"store-{NODE_BASE + i}")):
                if os.path.basename(dirpath) == "tmp":
                    continue
                for n in names:
                    total += os.path.getsize(os.path.join(dirpath, n))
                    files += 1
        return total, files


def reclaim(a, rows, summary):
    print("== reclaim: 20 files of 4 MiB, pairs sharing half their chunks; delete 10, collect twice", flush=True)
    for rep in range(1, a.reps + 1):
        rng = random.Random(1000 + rep)
        with Cluster(os.path.join(a.scratch, "cluster"), 8) as c:
            files, tokens = {}, {}
            for p in range(10):                          # pair p: files 2p and 2p+1 share 4 chunks
                common = rng.randbytes(4 * 512 * 1024)
                for j in range(2):
                    name = f"f{2 * p + j}"
                    data = common + rng.randbytes(4 * 512 * 1024) if j == 0 else rng.randbytes(4 * 512 * 1024) + common
                    path = os.path.join(a.scratch, name)
                    with open(path, "wb") as f:
                        f.write(data)
                    out = c.client("put", path, name).split()
                    files[name], tokens[name] = data, out[out.index("token") + 1]
            time.sleep(3)                                # repair settles
            disk0, files0 = c.disk()
            deleted = [f"f{2 * p}" for p in range(10)]   # one of each pair: the shared halves stay live
            for name in deleted:
                c.client("delete", name, tokens[name])
            time.sleep(GRACE + 1)
            g1 = c.gc()
            disk1, files1 = c.disk()
            time.sleep(GRACE + 1)
            g2 = c.gc()
            disk2, files2 = c.disk()
            whole = 0
            for name in files:
                if name in deleted:
                    continue
                back = os.path.join(a.scratch, "back")
                c.client("get", name, back)
                with open(back, "rb") as f:
                    whole += f.read() == files[name]
            # What the deleted files used alone: their 4 own chunks each, at three copies, plus manifests.
            own = 10 * 4 * 512 * 1024 * 3
            row = {"mode": "reclaim", "rep": rep, "disk_before": disk0, "files_before": files0,
                   "freed_1": disk0 - disk1, "reported_1": g1["bytes"], "dropped_1": g1["dropped"], "garbage_1": g1["garbage"],
                   "freed_2": disk1 - disk2, "reported_2": g2["bytes"], "dropped_2": g2["dropped"], "garbage_2": g2["garbage"],
                   "own_bytes_x3": own, "survivors_whole": whole, "gc_ms_1": g1["wall_ms"], "gc_ms_2": g2["wall_ms"]}
            rows.append(row)
            print("  " + json.dumps(row), flush=True)
    rr = [r for r in rows if r["mode"] == "reclaim"]
    summary["reclaim"] = {k: statistics.median(r[k] for r in rr) for k in rr[0] if k not in ("mode", "rep")}
    summary["reclaim_range"] = {k: [min(r[k] for r in rr), max(r[k] for r in rr)] for k in ("freed_1", "freed_2", "gc_ms_1")}


def chunk_curve(a, rows, summary):
    sizes = [int(x) for x in a.chunks.split(",")]
    print(f"== collection time against chunks: {sizes} chunks of 4 KiB", flush=True)
    for n in sizes:
        for rep in range(1, a.reps + 1):
            with Cluster(os.path.join(a.scratch, "cluster"), 8, "--scan 0") as c:
                path = os.path.join(a.scratch, f"chunks-{n}")
                with open(path, "wb") as f:
                    f.write(random.Random(n * 10 + rep).randbytes(n * 4096))
                out = c.client("--chunk-size", "4096", "--workers", "8", "put", path, "big").split()
                time.sleep(2)
                live = c.gc()                            # the file named: everything live, all of it marked
                c.client("delete", "big", out[out.index("token") + 1])
                time.sleep(GRACE + 1)
                dead = c.gc()                            # nothing named: everything garbage, all of it swept
                row = {"mode": "chunks", "chunks": n, "rep": rep, "live_mark_ms": live["mark_ms"], "live_sweep_ms": live["sweep_ms"],
                       "live_held": live["held"], "dead_sweep_ms": dead["sweep_ms"], "dead_dropped": dead["dropped"],
                       "dead_garbage": dead["garbage"], "dead_wall_ms": dead["wall_ms"]}
                rows.append(row)
                print("  " + json.dumps(row), flush=True)
    summary["chunks"] = {}
    for n in sizes:
        rr = [r for r in rows if r["mode"] == "chunks" and r["chunks"] == n]
        summary["chunks"][n] = {k: statistics.median(r[k] for r in rr) for k in rr[0] if k not in ("mode", "rep", "chunks")}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("label")
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--reclaim", action="store_true")
    ap.add_argument("--chunks", default="")
    ap.add_argument("--scratch", default=os.path.join(os.environ.get("TMPDIR", "/tmp"), "bench-gc"))
    a = ap.parse_args()
    os.makedirs(a.scratch, exist_ok=True)
    rows, summary = [], {"label": a.label, "grace_s": GRACE}
    if a.reclaim:
        reclaim(a, rows, summary)
    if a.chunks:
        chunk_curve(a, rows, summary)
    raw = os.path.join(ROOT, "bench", "raw")
    os.makedirs(raw, exist_ok=True)
    keys = sorted({k for r in rows for k in r})
    with open(os.path.join(raw, f"gc-{a.label}.csv"), "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=keys)
        w.writeheader()
        w.writerows(rows)
    print("SUMMARY " + json.dumps(summary))


if __name__ == "__main__":
    sys.exit(main())
