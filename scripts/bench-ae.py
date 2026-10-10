#!/usr/bin/env python3
"""bench-ae.py -- anti-entropy's cost, and the window with fewer than three copies (decision D-093).

    scripts/bench-ae.py <label> [--reps 3] [--scans 3]

Every repetition starts a fresh cluster with scripts/cluster.sh -- three tracker replicas, 8 node
processes x 8 members, metrics on, repair's full scan every 5 rounds of 1 s -- waits for the ring
oracle, and uploads one 64 MiB file in 16 KiB chunks (4,096 chunks, 12,288 copies) with 8 workers.
Then, from outside the processes:

  scan cost    every node's chordfs_chunk_requests_total, by verb, over --scans full-scan periods
               of a ring with nothing to repair: HAVE listings and DIGESTs received per full scan,
               summed over nodes, and the bytes they carry (request line + reply, counted from the
               formats, each with its round's 21-byte request id: a HAVE of 20 keys 847 + 26, a DIGEST 62 + 54)
  window       every coordinator's log line for every chunk of the upload ("answered after a ms,
               last copy after b ms"): b - a, the time a chunk spent acknowledged with fewer than
               three copies, as percentiles; and how many were already complete when acknowledged
  restore      20 chunk files deleted from one node's store directory behind its back: the time
               until every one is back on that disk (the scrub of the next full scan, D-050)

Raw rows go to bench/raw/ae-<label>.csv (not committed); a JSON summary is printed at the end.
"""
import argparse, csv, json, os, random, re, shutil, socket, statistics, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
from chunkcheck import Placement, ring_correct   # noqa: E402

TRACKER_PORT, NODE_BASE, OFFSET, NODES, SCAN, PERIOD_S = 7000, 7010, 1000, 8, 5, 1.0
HAVE_BYTES, DIGEST_BYTES = 21 + 826 + 26, 21 + 41 + 54   # request id prefix, request line, reply line


def metrics(port):
    with socket.create_connection(("127.0.0.1", port), timeout=5) as s:
        s.sendall(b"GET /metrics HTTP/1.1\r\n\r\n")
        data = b""
        while True:
            b = s.recv(65536)
            if not b:
                break
            data += b
    out = {}
    for line in data.decode().split("\r\n\r\n", 1)[1].splitlines():
        m = re.match(r'chordfs_chunk_requests_total\{verb="(\w+)",result="(\w+)"\} (\d+)', line)
        if m:
            out[m.group(1)] = out.get(m.group(1), 0) + int(m.group(3))
    return out


def pct(xs, p):
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(round(p / 100 * (len(xs) - 1))))]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("label")
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--scans", type=int, default=3)
    ap.add_argument("--scratch", default=os.path.join(os.environ.get("TMPDIR", "/tmp"), "bench-ae"))
    a = ap.parse_args()
    os.makedirs(a.scratch, exist_ok=True)
    work = os.path.join(a.scratch, "cluster")
    env = dict(os.environ, CLUSTER_DIR=work, TRACKERS="3", TRACKER_PORT=str(TRACKER_PORT), NODE_BASE=str(NODE_BASE),
               VNODES="8", PERIOD="1000", METRICS_OFFSET=str(OFFSET), NODE_ARGS=f"--scan {SCAN}")
    group = ",".join(f"127.0.0.1:{TRACKER_PORT + i}" for i in range(3))
    ring = f"127.0.0.1:{NODE_BASE},127.0.0.1:{NODE_BASE + 1}"
    ports = [NODE_BASE + i for i in range(NODES)]
    data = os.path.join(a.scratch, "file.bin")
    with open(data, "wb") as f:
        f.write(os.urandom(64 << 20))
    rows, reps = [], []
    for rep in range(1, a.reps + 1):
        shutil.rmtree(work, ignore_errors=True)
        r = subprocess.run([f"{ROOT}/scripts/cluster.sh", "up", str(NODES)], env=env, capture_output=True, timeout=300)
        if r.returncode != 0:
            raise RuntimeError("cluster.sh up failed: " + r.stdout.decode()[-300:])
        try:
            placement = Placement([f"127.0.0.1:{p}" for p in ports], 8)
            deadline = time.monotonic() + 120
            while time.monotonic() < deadline and not ring_correct(placement):
                time.sleep(0.5)
            time.sleep(3)
            up = subprocess.run([f"{ROOT}/client", "--tracker", group, "--ring", ring, "--quiet", "--chunk-size", "16384",
                                 "--workers", "8", "put", data, "f"], capture_output=True, timeout=600)
            if up.returncode != 0:
                raise RuntimeError("upload failed: " + up.stderr.decode()[-300:])
            time.sleep(2 * SCAN * PERIOD_S)           # let any repair the upload caused finish
            # ---- scan cost ----
            before = [metrics(p + OFFSET) for p in ports]
            time.sleep(a.scans * SCAN * PERIOD_S)
            after = [metrics(p + OFFSET) for p in ports]
            have = sum(x.get("have", 0) - y.get("have", 0) for x, y in zip(after, before)) / a.scans
            digest = sum(x.get("digest", 0) - y.get("digest", 0) for x, y in zip(after, before)) / a.scans
            stores = sum(x.get("store", 0) - y.get("store", 0) for x, y in zip(after, before))
            # ---- window ----
            windows, complete = [], 0
            pat = re.compile(r"answered after (\d+) ms, last copy after (\d+) ms")
            for p in ports:
                for m in pat.finditer(open(os.path.join(work, "logs", f"node-{p}.log")).read()):
                    w = int(m.group(2)) - int(m.group(1))
                    windows.append(w)
                    complete += w == 0
            # ---- restore ----
            victim = os.path.join(work, f"store-{ports[3]}")
            files = [os.path.join(d, n) for d, _, ns in os.walk(victim) if os.path.basename(d) != "tmp" for n in ns]
            gone = random.Random(rep).sample(files, 20)
            t0 = time.monotonic()
            for f in gone:
                os.unlink(f)
            while time.monotonic() - t0 < 60 and not all(os.path.exists(f) for f in gone):
                time.sleep(0.05)
            restore = time.monotonic() - t0 if all(os.path.exists(f) for f in gone) else float("nan")
        finally:
            subprocess.run([f"{ROOT}/scripts/cluster.sh", "down"], env=env, capture_output=True, timeout=120)
        out = {"rep": rep, "have_per_scan": have, "digest_per_scan": digest, "stores_during": stores,
               "scan_bytes": have * HAVE_BYTES + digest * DIGEST_BYTES, "writes": len(windows),
               "window_p50_ms": pct(windows, 50), "window_p99_ms": pct(windows, 99), "window_max_ms": max(windows),
               "complete_at_ack": complete, "restore_s": restore}
        print("  " + json.dumps(out), flush=True)
        reps.append(out)
        rows.extend({"rep": rep, "window_ms": w} for w in windows)
    raw = os.path.join(ROOT, "bench", "raw")
    os.makedirs(raw, exist_ok=True)
    with open(os.path.join(raw, f"ae-{a.label}.csv"), "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["rep", "window_ms"])
        w.writeheader()
        w.writerows(rows)
    med = {k: statistics.median(r[k] for r in reps) for k in reps[0] if k != "rep"}
    print("SUMMARY " + json.dumps({"label": a.label, "median": med,
                                   "range": {k: [min(r[k] for r in reps), max(r[k] for r in reps)] for k in med}}))


if __name__ == "__main__":
    sys.exit(main())
