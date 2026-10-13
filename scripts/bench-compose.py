#!/usr/bin/env python3
"""bench-compose.py -- the container deployment: transfers, and what a dead container's address costs.

    scripts/bench-compose.py [--reps 5] [--trials 10] [--mb 100]

Starts compose.yaml under its own project name (three tracker replicas, five nodes x 8
members, Prometheus, Grafana; one host, one bridge network), waits for the ring oracle, then:
  1. healthy: --reps uploads and downloads through the client container, each of its own
     seeded random file of --mb MiB (a file already stored would deduplicate, chunk by
     chunk, and measure that instead), each download compared by SHA-1; the client's own
     put/get total_ms
  2. node3 killed: how the four survivors evicted its members, by refusal or by strikes
     (their eviction counters, read by Prometheus, before the kill and 20 s after it); then
     connect to its address from a fresh container each trial, against connect to a live
     container's closed port -- the time until the kernel answers
  3. name resolution at the tracker group (`client resolve`, timed around the process in
     its container), healthy and then with tracker1 -- the first address every new client
     tries -- dead; and whole downloads with tracker1 dead, compared by SHA-1. (The
     client's own resolve_ms is the manifests' fetch from the ring, not this.)
Prints every run and the medians. The stack is taken down with its volumes on exit.
Linux only: the oracle needs a route from the host to the container network.
"""
import argparse, hashlib, json, os, random, statistics, subprocess, sys, time, urllib.parse

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
from chunkcheck import Placement, ring_correct   # noqa: E402

NODES = [f"172.28.0.{10 + i}:7001" for i in range(1, 6)]
MB = 1e6


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--reps", type=int, default=5)
    ap.add_argument("--trials", type=int, default=10)
    ap.add_argument("--mb", type=int, default=100)
    ap.add_argument("--project", default="chordfs-bench")
    a = ap.parse_args()
    dc_base = ["docker", "compose", "-f", os.path.join(ROOT, "compose.yaml"), "-p", a.project]
    work = os.path.join(ROOT, "cluster", f"bench-compose-{os.getpid()}")   # the client mounts the repository as /work
    os.makedirs(work, exist_ok=True)

    def dc(*args, timeout=600):
        return subprocess.run(dc_base + list(args), capture_output=True, text=True, timeout=timeout)

    def client(*args):
        r = dc("run", "--rm", "--user", f"{os.getuid()}:{os.getgid()}", "client", *args)
        line = next((l for l in r.stdout.splitlines() if l.split()[:1] in (["put"], ["get"])), "")
        f = line.split()
        return {f[i]: f[i + 1] for i in range(4, len(f) - 1, 2)} if " ok " in line else None

    def connect_ms(addr):
        """Seconds from connect() to the kernel's answer, timed inside a fresh client container."""
        ip, port = addr.split(":")
        script = (f's=$(date +%s%N); (exec 3<>/dev/tcp/{ip}/{port}) 2>/tmp/e; e=$(date +%s%N); '
                  f'echo $(( (e - s) / 1000000 )) $(tail -c 60 /tmp/e)')
        r = dc("run", "--rm", "--entrypoint", "bash", "client", "-c", script)
        out = r.stdout.strip().split(None, 1)
        return int(out[0]), (out[1].rsplit(": ", 1)[-1] if len(out) > 1 else "connected")

    TRACKERS = "172.28.0.10:7000,172.28.0.20:7000,172.28.0.30:7000"

    def name_ms(name):
        """Milliseconds for `client resolve <name>`: the process, its tracker calls and its exit."""
        script = (f's=$(date +%s%N); client --tracker {TRACKERS} resolve {name} >/dev/null 2>&1; r=$?; '
                  f'e=$(date +%s%N); echo $r $(( (e - s) / 1000000 ))')
        out = dc("run", "--rm", "--entrypoint", "bash", "client", "-c", script).stdout.split()
        if not out or out[0] != "0":
            sys.exit(f"resolve {name} failed")
        return int(out[1])

    def evictions():
        url = "http://localhost:9090/api/v1/query?query=" + urllib.parse.quote(
            'sum by (evidence) (chordfs_ring_evictions_total{instance!="172.28.0.13:8001"})')
        r = dc("exec", "-T", "prometheus", "wget", "-qO-", url, timeout=30)
        return {x["metric"]["evidence"]: float(x["value"][1]) for x in json.loads(r.stdout)["data"]["result"]}

    def sha1(p):
        h = hashlib.sha1()
        with open(p, "rb") as f:
            for b in iter(lambda: f.read(1 << 20), b""):
                h.update(b)
        return h.hexdigest()

    def wait_ring(live, limit=90):
        end = time.monotonic() + limit
        while time.monotonic() < end:
            try:
                if ring_correct(Placement(live, 8)):
                    return True
            except Exception:
                pass
            time.sleep(0.5)
        return False

    try:
        size = a.mb << 20
        files = []
        for i in range(a.reps):
            src = os.path.join(work, f"file{i}.bin")
            with open(src, "wb") as f:
                f.write(random.Random(i).randbytes(size))
            files.append((src, sha1(src)))
        rel = lambda p: "/work/" + os.path.relpath(p, ROOT)
        print(f"commit {subprocess.run(['git', '-C', ROOT, 'rev-parse', '--short', 'HEAD'], capture_output=True, text=True).stdout.strip()}"
              f", file {size} bytes", flush=True)
        r = dc("up", "-d", "--build")
        if r.returncode:
            sys.exit("docker compose up failed:\n" + r.stderr[-2000:])
        if not wait_ring(NODES):
            sys.exit("the ring did not converge")
        time.sleep(3)                              # the tracker group elects within ~1 s of its last replica

        print("1. healthy", flush=True)
        puts, gets = [], []
        for i, (src, want) in enumerate(files):
            p = client("put", rel(src), f"f{i}")
            dst = os.path.join(work, f"got{i}")
            g = client("get", f"f{i}", rel(dst))
            ok = bool(p and g) and sha1(dst) == want
            os.remove(dst) if os.path.exists(dst) else None
            if not ok:
                sys.exit(f"run {i}: transfer failed or bytes differ")
            puts.append(size / MB / (float(p["total_ms"]) / 1000))
            gets.append(size / MB / (float(g["total_ms"]) / 1000))
            print(f"   run {i}: put {puts[-1]:6.1f} MB/s ({p['total_ms']} ms)  get {gets[-1]:6.1f} MB/s ({g['total_ms']} ms)", flush=True)

        print("2. connect: a dead container's address against a live container's closed port", flush=True)
        before = evictions()
        dc("kill", "node3")
        time.sleep(20)                             # eviction, and two scrapes after it
        after = evictions()
        ev = {k: int(after.get(k, 0) - before.get(k, 0)) for k in sorted(set(before) | set(after))}
        print(f"   members of node3 evicted by the survivors, by evidence: {ev}", flush=True)
        dead, closed = [], []
        for _ in range(a.trials):
            ms, why = connect_ms("172.28.0.13:7001")
            dead.append(ms)
            ms2, why2 = connect_ms("172.28.0.11:7999")
            closed.append(ms2)
            print(f"   dead address {ms:5d} ms ({why.strip()})   closed port {ms2:3d} ms ({why2.strip()})", flush=True)

        print("3. name resolution at the tracker group, then with tracker1 dead", flush=True)
        healthy_names = [name_ms(f"f{i % a.reps}") for i in range(a.trials)]
        print(f"   healthy: {healthy_names} ms", flush=True)
        dc("kill", "tracker1")
        time.sleep(5)
        dead_names = [name_ms(f"f{i % a.reps}") for i in range(a.trials)]
        print(f"   tracker1 dead: {dead_names} ms", flush=True)
        dead_gets = []
        for i in range(a.reps):
            dst = os.path.join(work, f"res{i}")
            g = client("get", f"f{i}", rel(dst))
            if not g or sha1(dst) != files[i][1]:
                sys.exit(f"download {i} with tracker1 dead failed or bytes differ")
            os.remove(dst)
            dead_gets.append(float(g["total_ms"]))
            print(f"   get with tracker1 dead {g['total_ms']} ms", flush=True)

        med = statistics.median
        print("\nsummary (medians)")
        print(f"  healthy put {med(puts):.1f} MB/s (min {min(puts):.1f}, max {max(puts):.1f}); "
              f"get {med(gets):.1f} MB/s (min {min(gets):.1f}, max {max(gets):.1f}), n={a.reps}")
        print(f"  connect to a dead container's address {med(dead)} ms (min {min(dead)}, max {max(dead)}); "
              f"to a closed port {med(closed)} ms, n={a.trials}")
        print(f"  node3's eviction by evidence: {ev}")
        print(f"  client resolve: healthy {med(healthy_names)} ms (min {min(healthy_names)}, max {max(healthy_names)}); "
              f"tracker1 dead {med(dead_names)} ms (min {min(dead_names)}, max {max(dead_names)}), n={a.trials}")
        print(f"  get with tracker1 dead {med(dead_gets):.0f} ms (min {min(dead_gets):.0f}, max {max(dead_gets):.0f}), "
              f"against healthy {statistics.median([size / MB / g * 1000 for g in gets]):.0f} ms, n={a.reps}")
    finally:
        dc("down", "-v", "--remove-orphans")
        subprocess.run(["rm", "-rf", work])


if __name__ == "__main__":
    main()
