#!/usr/bin/env python3
"""bench-observe.py -- what observability costs (decisions D-083 to D-087).

    scripts/bench-observe.py <label> [--nodes 16] [--reps 3] [--scrapes 300] [--size-mb 100]

Each repetition starts a fresh cluster with scripts/cluster.sh (three tracker replicas and
--nodes node processes, 8 ring members each, every process with a metrics port), uploads one
file with the real client, and measures, from outside the processes:

  scrape   GET /metrics on every process, --scrapes times each, one after another: the
           latency of a whole exchange (connect, request, response, close) and the body's
           size -- first with the cluster idle, then on four nodes while a client downloads
           the file over and over with 8 workers
  logs     the bytes every process's log grew by over one upload and one download of the
           file: at the default level (info), and in a second cluster at debug, where every
           routing hop is a line

The cost on the transfer path itself is measured by bench-transfer.py at this commit and at
the commit before observability was added (BENCHMARKS.md §8). Raw rows go to
bench/raw/observe-<label>.csv (not committed); a JSON summary is printed at the end.
"""
import argparse, csv, json, os, shutil, socket, statistics, subprocess, sys, threading, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TRACKER_PORT, NODE_BASE, OFFSET = 7000, 7010, 1000
MB = 1 << 20


def scrape(port):
    """One whole exchange with a metrics port: (milliseconds, body bytes)."""
    t0 = time.perf_counter()
    with socket.create_connection(("127.0.0.1", port), timeout=5) as s:
        s.sendall(b"GET /metrics HTTP/1.1\r\nHost: bench\r\n\r\n")
        data = b""
        while True:
            b = s.recv(65536)
            if not b:
                break
            data += b
    ms = (time.perf_counter() - t0) * 1000
    head, _, body = data.partition(b"\r\n\r\n")
    if not head.startswith(b"HTTP/1.0 200"):
        raise RuntimeError(f"scrape of {port} answered {head[:40]!r}")
    return ms, len(body)


def pct(xs, p):
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(round(p / 100 * (len(xs) - 1))))]


class Cluster:
    def __init__(self, work, nodes, level):
        self.dir, self.nodes, self.level = work, nodes, level
        self.env = dict(os.environ, CLUSTER_DIR=work, TRACKERS="3", TRACKER_PORT=str(TRACKER_PORT), NODE_BASE=str(NODE_BASE),
                        VNODES="8", PERIOD="1000", METRICS_OFFSET=str(OFFSET), LOG_LEVEL=level)
        self.trackers = [TRACKER_PORT + i for i in range(3)]
        self.node_ports = [NODE_BASE + i for i in range(nodes)]

    def __enter__(self):
        shutil.rmtree(self.dir, ignore_errors=True)
        r = subprocess.run([f"{ROOT}/scripts/cluster.sh", "up", str(self.nodes)], env=self.env, capture_output=True, timeout=300)
        if r.returncode != 0:
            raise RuntimeError("cluster.sh up failed: " + r.stdout.decode()[-400:])
        return self

    def __exit__(self, *exc):
        subprocess.run([f"{ROOT}/scripts/cluster.sh", "down"], env=self.env, capture_output=True, timeout=120)

    def client(self, *args):
        group = ",".join(f"127.0.0.1:{p}" for p in self.trackers)
        ring = f"127.0.0.1:{self.node_ports[0]},127.0.0.1:{self.node_ports[1]}"
        r = subprocess.run([f"{ROOT}/client", "--tracker", group, "--ring", ring, "--quiet"] + list(args),
                           capture_output=True, timeout=600)
        if r.returncode != 0:
            raise RuntimeError("client failed: " + r.stderr.decode()[-300:])
        return r

    def log_bytes(self):
        logs = os.path.join(self.dir, "logs")
        return {f: os.path.getsize(os.path.join(logs, f)) for f in os.listdir(logs)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("label")
    ap.add_argument("--nodes", type=int, default=16)
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--scrapes", type=int, default=300)
    ap.add_argument("--size-mb", type=int, default=100)
    ap.add_argument("--scratch", default=os.path.join(os.environ.get("TMPDIR", "/tmp"), "bench-observe"))
    a = ap.parse_args()
    os.makedirs(a.scratch, exist_ok=True)
    data = os.path.join(a.scratch, "file.bin")
    with open(data, "wb") as f:                      # random, so no two chunks are alike (no deduplication)
        for _ in range(a.size_mb):
            f.write(os.urandom(MB))
    size = os.path.getsize(data)
    rows, summary = [], {"label": a.label, "nodes": a.nodes, "size_bytes": size, "reps": []}

    for rep in range(1, a.reps + 1):
        print(f"-- rep {rep}", flush=True)
        out = {"rep": rep}
        with Cluster(os.path.join(a.scratch, "cluster"), a.nodes, "info") as c:
            before = c.log_bytes()
            c.client("put", data, "bench.bin")
            mid = c.log_bytes()
            c.client("get", "bench.bin", os.path.join(a.scratch, "back.bin"))
            after = c.log_bytes()
            out["log_info_upload_bytes"] = sum(mid[f] - before.get(f, 0) for f in mid)
            out["log_info_download_bytes"] = sum(after[f] - mid.get(f, 0) for f in after)

            # idle scrapes, every process
            idle, sizes = [], {}
            for p in c.trackers + c.node_ports:
                for _ in range(a.scrapes):
                    ms, n = scrape(p + OFFSET)
                    idle.append(ms)
                    rows.append({"rep": rep, "phase": "idle", "process": p, "ms": round(ms, 4), "bytes": n})
                sizes[p] = n
            out["idle_ms_p50"], out["idle_ms_p99"] = pct(idle, 50), pct(idle, 99)
            out["body_bytes_node"] = statistics.median(sizes[p] for p in c.node_ports)
            out["body_bytes_tracker"] = statistics.median(sizes[p] for p in c.trackers)

            # scrapes under load: downloads in a loop meanwhile
            stop, gets = threading.Event(), [0]
            def load():
                while not stop.is_set():
                    c.client("get", "bench.bin", os.path.join(a.scratch, "loop.bin"))
                    gets[0] += 1
            th = threading.Thread(target=load)
            th.start()
            time.sleep(1.0)
            busy = []
            for p in c.node_ports[:4]:
                for _ in range(a.scrapes):
                    ms, n = scrape(p + OFFSET)
                    busy.append(ms)
                    rows.append({"rep": rep, "phase": "load", "process": p, "ms": round(ms, 4), "bytes": n})
            stop.set()
            th.join()
            out["load_ms_p50"], out["load_ms_p99"], out["load_downloads"] = pct(busy, 50), pct(busy, 99), gets[0]

        with Cluster(os.path.join(a.scratch, "cluster-debug"), a.nodes, "debug") as c:
            before = c.log_bytes()
            c.client("put", data, "bench.bin")
            mid = c.log_bytes()
            c.client("get", "bench.bin", os.path.join(a.scratch, "back.bin"))
            after = c.log_bytes()
            out["log_debug_upload_bytes"] = sum(mid[f] - before.get(f, 0) for f in mid)
            out["log_debug_download_bytes"] = sum(after[f] - mid.get(f, 0) for f in after)
        print("   " + json.dumps(out), flush=True)
        summary["reps"].append(out)

    raw = os.path.join(ROOT, "bench", "raw")
    os.makedirs(raw, exist_ok=True)
    with open(os.path.join(raw, f"observe-{a.label}.csv"), "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["rep", "phase", "process", "ms", "bytes"])
        w.writeheader()
        w.writerows(rows)
    med = {k: statistics.median(r[k] for r in summary["reps"]) for k in summary["reps"][0] if k != "rep"}
    spread = {k: [min(r[k] for r in summary["reps"]), max(r[k] for r in summary["reps"])] for k in med}
    summary["median"], summary["range"] = med, spread
    print(json.dumps(summary, indent=1))


if __name__ == "__main__":
    sys.exit(main())
