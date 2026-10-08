#!/usr/bin/env python3
"""bench-transfer.py -- the Phase 5 measurements (decision D-066).

Every repetition starts a fresh ring of `node --wired` processes (each with its own
chunk store) and a fresh tracker, uploads a file with the real `client`, downloads
it, and checks the SHA-1 of what came back. Nothing measured here is reported by a
node about itself: times come from the client's own clock (its output line) and
from this harness's monotonic clock around the client process.

Modes (any combination; run in this order):
  --workers-curve   100 MB, 16 processes, workers 1 2 4 8 16 32. Applies D-056's rule,
                    fixed before measuring: the default is the smallest power of two
                    whose median download throughput is within 10 % of the best.
  --sizes           1, 10, 100 MB at 1 worker and at the default: the before/after
  --procs-curve     100 MB at the default, 3 4 8 16 32 processes
  --chunk-sweep     100 MB at the default, chunk sizes 64 KiB .. 8 MiB; needs --bin-dir
                    pointing at a build with the chunk limit raised (D-063)
  --dedup           D-062's workload on one 16-process ring
  --tracker N[,N]   tracker log bytes, resident memory and registrations per second

Throughput is bytes / 2^20 / seconds, as the baseline harness computed it. The
comparable download time is the client's ready_ms: from its start to every chunk
verified and written, before the fsync and rename (D-066). Raw rows go to
bench/raw/transfer-<commit>.csv and per-chunk latencies to bench/raw/latency-<commit>.csv
(not committed); a JSON summary is printed at the end for scripts/plot-transfer.py.
"""
import argparse, csv, hashlib, json, os, random, shutil, signal, socket, statistics, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
from chunkcheck import Placement, ring_correct   # noqa: E402

BASE_PORT, TRACKER_PORT = 7800, 7899
MB = 1 << 20


def sha1_file(path):
    h = hashlib.sha1()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def listening(port):
    try:
        socket.create_connection(("127.0.0.1", port), timeout=0.5).close()
        return True
    except OSError:
        return False


class Cluster:
    """A wired ring of `procs` node processes x `vnodes` members, and a tracker."""

    def __init__(self, bin_dir, work, procs, vnodes=8, period=1000, timeout=200):
        self.bin, self.work, self.procs, self.vnodes = bin_dir, work, procs, vnodes
        self.addrs = [f"127.0.0.1:{BASE_PORT + i}" for i in range(procs)]
        self.period, self.timeout, self.p = period, timeout, []

    def start(self):
        os.makedirs(self.work, exist_ok=True)
        members = os.path.join(self.work, "members.txt")
        open(members, "w").write("".join(a + "\n" for a in self.addrs))
        logs = open(os.path.join(self.work, "nodes.log"), "w")
        for a in self.addrs:
            ip, port = a.split(":")
            self.p.append(subprocess.Popen([f"{self.bin}/node", ip, port, "--wired", members, "--vnodes", str(self.vnodes),
                                            "--store", os.path.join(self.work, "store-" + port), "--period", str(self.period),
                                            "--timeout", str(self.timeout)], stdout=subprocess.DEVNULL, stderr=logs))
        self.p.append(subprocess.Popen([f"{self.bin}/tracker", str(TRACKER_PORT), "--dir", self.work],
                                       stdout=subprocess.DEVNULL, stderr=logs))
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline and not (all(listening(BASE_PORT + i) for i in range(self.procs)) and listening(TRACKER_PORT)):
            time.sleep(0.1)
        placement = Placement(self.addrs, self.vnodes)
        while time.monotonic() < deadline and not ring_correct(placement):
            time.sleep(0.5)
        if not ring_correct(placement):
            raise RuntimeError("the ring did not come up correct")

    def stop(self):
        for p in self.p:
            p.kill()
        for p in self.p:
            p.wait()
        self.p = []
        shutil.rmtree(self.work, ignore_errors=True)
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline and any(listening(BASE_PORT + i) for i in range(self.procs)):
            time.sleep(0.1)

    def stored_files(self):
        n = 0
        for i in range(self.procs):
            store = os.path.join(self.work, f"store-{BASE_PORT + i}")
            for sub in os.listdir(store) if os.path.isdir(store) else []:
                if sub != "tmp":
                    n += len(os.listdir(os.path.join(store, sub)))
        return n

    def client(self, args, stats=None):
        """Run the client; return (ok, fields from its output line, wall ms)."""
        cmd = [f"{self.bin}/client", "--tracker", f"127.0.0.1:{TRACKER_PORT}", "--ring", ",".join(self.addrs[:2]), "--quiet"]
        if stats:
            cmd += ["--stats", stats]
        t0 = time.monotonic()
        r = subprocess.run(cmd + args, capture_output=True, timeout=600)
        wall = (time.monotonic() - t0) * 1000
        words = r.stdout.decode().split()
        fields = {}
        if r.returncode == 0 and "ok" in words:
            rest = words[words.index("ok") + 1:]
            fields = {rest[i]: rest[i + 1] for i in range(0, len(rest) - 1, 2)}
            fields["key"] = words[2]
        else:
            sys.stderr.write(r.stderr.decode()[-500:])
        return r.returncode == 0, fields, wall


class Bench:
    def __init__(self, args):
        self.args = args
        self.commit = subprocess.run(["git", "-C", ROOT, "rev-parse", "--short", "HEAD"], capture_output=True, text=True).stdout.strip()
        dirty = subprocess.run(["git", "-C", ROOT, "diff", "--quiet"]).returncode != 0
        self.label = self.commit + ("-dirty" if dirty else "")
        os.makedirs(os.path.join(ROOT, "bench", "raw"), exist_ok=True)
        self.rows_path = os.path.join(ROOT, "bench", "raw", f"transfer-{self.label}.csv")
        self.lat_path = os.path.join(ROOT, "bench", "raw", f"latency-{self.label}.csv")
        self.rows = []
        self.summary = {"commit": self.label, "dirty": dirty}
        self.scratch = args.scratch
        self.default_workers = args.default_workers
        print(f"bench-transfer at {self.label}{'  (WORKING TREE DIRTY: not reproducible)' if dirty else ''}", flush=True)
        print(f"host: {os.uname().machine}, {os.cpu_count()} cores, kernel {os.uname().release}", flush=True)
        t0, w0 = time.monotonic(), time.time()
        time.sleep(1)
        print(f"timer sanity: monotonic {time.monotonic() - t0:.3f} s, wall {time.time() - w0:.3f} s over a 1 s sleep", flush=True)

    def record(self, row):
        self.rows.append(row)
        new = not os.path.exists(self.rows_path)
        with open(self.rows_path, "a", newline="") as f:
            w = csv.DictWriter(f, fieldnames=list(row.keys()))
            if new:
                w.writeheader()
            w.writerow(row)

    def latency(self, config, stats):
        if not os.path.exists(stats):
            return
        with open(stats) as src, open(self.lat_path, "a") as out:
            for line in src:
                out.write(config + "," + line)

    def one(self, mode, file, procs, workers, rep, chunk=512 * 1024, bin_dir=None, vnodes=8):
        """One repetition: fresh ring, upload, download, verify."""
        bin_dir = bin_dir or ROOT
        src = os.path.join(ROOT, "testdata", file)
        size = os.path.getsize(src)
        work = os.path.join(self.scratch, f"ring-{mode}-{rep}")
        c = Cluster(bin_dir, work, procs, vnodes)
        try:
            c.start()
            config = f"{mode},{file},{procs},{workers},{chunk},{os.path.basename(bin_dir)}"
            up_stats, down_stats = os.path.join(work, "up.csv"), os.path.join(work, "down.csv")
            ok_u, fu, wall_u = c.client(["--workers", str(workers), "--chunk-size", str(chunk), "put", src, "f"], up_stats)
            dest = os.path.join(work, "got")
            ok_d, fd, wall_d = c.client(["--workers", str(workers), "get", "f", dest], down_stats) if ok_u else (False, {}, 0)
            same = ok_d and sha1_file(dest) == sha1_file(src)
            self.latency(config + ",put", up_stats)
            self.latency(config + ",get", down_stats)
        finally:
            c.stop()
        row = {"mode": mode, "file": file, "bytes": size, "procs": procs, "vnodes": vnodes, "workers": workers, "chunk": chunk,
               "build": os.path.basename(bin_dir), "rep": rep, "ok": int(same),
               "put_wall_ms": round(wall_u, 3), "put_total_ms": fu.get("total_ms", ""), "put_chunk_ms": fu.get("chunk_ms", ""),
               "put_manifest_ms": fu.get("manifest_ms", ""), "get_wall_ms": round(wall_d, 3), "get_ready_ms": fd.get("ready_ms", ""),
               "get_total_ms": fd.get("total_ms", ""), "get_resolve_ms": fd.get("resolve_ms", ""), "get_fetch_ms": fd.get("fetch_ms", ""),
               "get_sync_ms": fd.get("sync_ms", ""), "chunks": fd.get("chunks", "")}
        self.record(row)
        mbps = size / MB / (float(row["get_ready_ms"]) / 1000) if same else 0
        print(f"  {mode:<8} {file:<11} procs {procs:>2} workers {workers:>2} chunk {chunk:>8} rep {rep}: "
              f"{'ok ' if same else 'FAILED'} put {row['put_wall_ms']:>9} ms  get ready {row['get_ready_ms']:>9} ms "
              f"({mbps:7.1f} MB/s) sync {row['get_sync_ms']} ms", flush=True)
        if not same:
            raise RuntimeError(f"a transfer failed or came back different: {row}")
        return row

    @staticmethod
    def mbps(rows):
        return [r["bytes"] / MB / (float(r["get_ready_ms"]) / 1000) for r in rows]

    def workers_curve(self):
        print("== workers curve: huge.iso, 16 processes", flush=True)
        medians = {}
        for w in [1, 2, 4, 8, 16, 32]:
            rows = [self.one("workers", "huge.iso", 16, w, rep) for rep in range(self.args.reps)]
            medians[w] = statistics.median(self.mbps(rows))
        best = max(medians.values())
        chosen = min(w for w, m in medians.items() if m >= 0.9 * best)
        print(f"   medians MB/s {medians}; best {best:.1f}; rule (smallest within 10 % of best) -> {chosen}", flush=True)
        self.summary["workers_medians"] = medians
        self.summary["workers_rule"] = chosen
        self.default_workers = chosen

    def sizes(self):
        print(f"== sizes: 1, 10, 100 MB at 1 worker and at {self.default_workers}, 16 processes", flush=True)
        for f in ["medium.bin", "large.dat", "huge.iso"]:
            for w in sorted({1, self.default_workers}):
                for rep in range(self.args.reps):
                    self.one("sizes", f, 16, w, rep)

    def procs_curve(self):
        print(f"== processes curve: huge.iso at {self.default_workers} workers", flush=True)
        for n in [3, 4, 8, 16, 32]:
            for rep in range(self.args.reps):
                self.one("procs", "huge.iso", n, self.default_workers, rep)

    def chunk_sweep(self):
        if not self.args.bin_dir:
            raise SystemExit("--chunk-sweep needs --bin-dir (a build with CHUNK_LIMIT=8388608)")
        print(f"== chunk-size sweep: huge.iso at {self.default_workers} workers, 16 processes, build {self.args.bin_dir}", flush=True)
        for chunk in [64 << 10, 256 << 10, 512 << 10, 2 << 20, 8 << 20]:
            for rep in range(self.args.reps):
                self.one("chunks", "huge.iso", 16, self.default_workers, rep, chunk=chunk, bin_dir=self.args.bin_dir)
        for rep in range(self.args.reps):           # 512 KiB on the default build too (D-063)
            self.one("chunks", "huge.iso", 16, self.default_workers, rep, chunk=512 << 10)

    def dedup(self):
        print("== deduplication (D-062), one 16-process ring", flush=True)
        work = os.path.join(self.scratch, "dedup")
        files = os.path.join(self.scratch, "dedup-files")
        os.makedirs(files, exist_ok=True)
        base = open(os.path.join(ROOT, "testdata", "huge.iso"), "rb").read()
        rng = random.Random(20261008)
        variants = [("base", base), ("same-bytes", base),
                    ("overwrite-1KiB-at-50MB", base[:50 * MB] + bytes(rng.randrange(256) for _ in range(1024)) + base[50 * MB + 1024:]),
                    ("insert-1-byte-at-1MiB", base[:MB] + b"\x00" + base[MB:]),
                    ("append-10MB", base + bytes(rng.getrandbits(8) for _ in range(10 * MB)))]
        log = subprocess.run(["git", "-C", ROOT, "log", "--format=%h", "-20"], capture_output=True, text=True).stdout.split()
        for i, h in enumerate(reversed(log)):
            tar = subprocess.run(["git", "-C", ROOT, "archive", "--format=tar", h], capture_output=True).stdout
            variants.append((f"git-{i:02d}-{h}", tar))
        c = Cluster(ROOT, work, 16)
        out = []
        try:
            c.start()
            for name, data in variants:
                p = os.path.join(files, name)
                open(p, "wb").write(data)
                before = c.stored_files()
                ok, f, _ = c.client(["--workers", str(self.default_workers), "put", p, name])
                # W = 2: wait until every copy has landed, so the count is stable.
                last, stable = -1, 0
                while stable < 5:
                    n = c.stored_files()
                    stable = stable + 1 if n == last else 0
                    last = n
                    time.sleep(0.2)
                new_files = last - before
                total = int(f.get("chunks", 0)) + int(f.get("manifests", 0))
                row = {"variant": name, "bytes": len(data), "ok": int(ok), "chunks": f.get("chunks"), "manifests": f.get("manifests"),
                       "new_files": new_files, "new_unique": new_files / 3, "shared": total - new_files / 3}
                out.append(row)
                print(f"   {name:<26} {len(data):>10} B  chunks+manifests {total:>4}  new unique {new_files / 3:>6.1f}"
                      f"  shared {row['shared']:>6.1f}", flush=True)
        finally:
            c.stop()
            shutil.rmtree(files, ignore_errors=True)
        self.summary["dedup"] = out

    def tracker(self, counts):
        print(f"== tracker footprint: {counts} names", flush=True)
        work = os.path.join(self.scratch, "tracker")
        os.makedirs(work, exist_ok=True)
        p = subprocess.Popen([f"{ROOT}/tracker", str(TRACKER_PORT), "--dir", work], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        while not listening(TRACKER_PORT):
            time.sleep(0.05)
        rss0 = int([l for l in open(f"/proc/{p.pid}/status") if l.startswith("VmRSS")][0].split()[1])
        # One connection, one request at a time, each answered before the next: the
        # rate is the tracker's own (one flush per registration, under one lock).
        s = socket.create_connection(("127.0.0.1", TRACKER_PORT))
        f = s.makefile("rb")
        out, done, t0 = [], 0, time.monotonic()
        try:
            for target in counts:
                while done < target:
                    name = f"file-{done:08d}.dat"
                    s.sendall(f"REGISTER {name} {hashlib.sha1(name.encode()).hexdigest()}\n".encode())
                    if f.readline() != b"OK\n":
                        raise RuntimeError("registration refused")
                    done += 1
                elapsed = time.monotonic() - t0
                rss = int([l for l in open(f"/proc/{p.pid}/status") if l.startswith("VmRSS")][0].split()[1])
                log_bytes = os.path.getsize(os.path.join(work, f"names_{TRACKER_PORT}.log"))
                row = {"names": done, "log_bytes": log_bytes, "log_bytes_per_name": log_bytes / done, "rss_kib": rss,
                       "rss_growth_kib": rss - rss0, "seconds": elapsed, "per_second": done / elapsed}
                out.append(row)
                print(f"   {done:>7} names: log {log_bytes} B ({log_bytes / done:.1f} B/name), RSS {rss} KiB (+{rss - rss0}), "
                      f"{done / elapsed:.0f} registrations/s", flush=True)
            # restart: replay time
            s.close()
            p.kill(); p.wait()
            t1 = time.monotonic()
            p = subprocess.Popen([f"{ROOT}/tracker", str(TRACKER_PORT), "--dir", work], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            while not listening(TRACKER_PORT):
                time.sleep(0.01)
            self.summary["tracker_replay_s"] = time.monotonic() - t1
            print(f"   restart with {done} names: listening after {self.summary['tracker_replay_s']:.2f} s", flush=True)
        finally:
            p.kill(); p.wait()
            shutil.rmtree(work, ignore_errors=True)
        self.summary["tracker"] = out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--workers-curve", action="store_true")
    ap.add_argument("--sizes", action="store_true")
    ap.add_argument("--procs-curve", action="store_true")
    ap.add_argument("--chunk-sweep", action="store_true")
    ap.add_argument("--dedup", action="store_true")
    ap.add_argument("--tracker", default="")
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--default-workers", type=int, default=8, help="used when --workers-curve is not run")
    ap.add_argument("--bin-dir", default="")
    ap.add_argument("--scratch", default=os.path.join(os.environ.get("TMPDIR", "/tmp"), "bench-transfer"))
    a = ap.parse_args()
    b = Bench(a)
    if a.workers_curve: b.workers_curve()
    if a.sizes: b.sizes()
    if a.procs_curve: b.procs_curve()
    if a.chunk_sweep: b.chunk_sweep()
    if a.dedup: b.dedup()
    if a.tracker: b.tracker([int(x) for x in a.tracker.split(",")])
    print("SUMMARY " + json.dumps(b.summary), flush=True)


if __name__ == "__main__":
    main()
