#!/usr/bin/env python3
"""run-reconverge.py -- the whole reconvergence benchmark at one commit (BENCHMARKS.md section 3).

Runs scripts/bench-reconverge.py in a fixed order and applies the selection rules of
decisions D-039, D-041 and D-042 IN CODE, so neither the period nor the deadline is
picked by hand:

  1. CPU per idle node against the period T (64 nodes).
  2. The period sweep: 64 nodes, one node killed, 10 runs per period, deadline D = 200 ms.
     RULE (D-042): T = the longest period at which at least 9 of 10 runs reach "ring
     correct" within 1 s, with no live node wrongly evicted in any run.
  3. Calibration under the loads the benchmark actually runs (D-041): 64 nodes at the
     shortest period, and 256 nodes at the chosen T.
     RULE (D-042): D = the smallest of 100, 200, 500, 1000 ms that is at least ten times
     the 99.9th-percentile exchange AND longer than the slowest exchange, at both loads.
  4. At the chosen T and D: ring size 16..256; 2..16 nodes killed at random; 4, 8 and 9
     adjacent nodes killed (8 is the successor-list length); one kill under 200 and
     1000 lookups per second.
  5. For the record: calibration at 256 nodes and 50 ms, the load that saturates this machine.

Everything goes to bench/raw/*-<commit>.csv and bench/raw/run-reconverge-<commit>.log.
"""
import csv, os, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BENCH = [sys.executable, os.path.join(ROOT, "scripts", "bench-reconverge.py")]
PERIODS = [50, 100, 200, 500, 1000, 2000]
CANDIDATES = [100, 200, 500, 1000]

head = subprocess.run(["git", "-C", ROOT, "rev-parse", "--short", "HEAD"], capture_output=True,
                      text=True).stdout.strip()
os.makedirs(os.path.join(ROOT, "bench", "raw"), exist_ok=True)
LOG = open(os.path.join(ROOT, "bench", "raw", f"run-reconverge-{head}.log"), "a")


def say(text):
    print(text, flush=True)
    LOG.write(text + "\n")
    LOG.flush()


def bench(*args):
    say("$ bench-reconverge.py " + " ".join(args))
    p = subprocess.run(BENCH + list(args), cwd=ROOT, capture_output=True, text=True)
    for line in (p.stdout + p.stderr).splitlines():
        say("  " + line)
    if p.returncode != 0:
        sys.exit(f"bench-reconverge.py failed with status {p.returncode}")


def rows(name, **match):
    with open(os.path.join(ROOT, "bench", "raw", f"{name}-{head}.csv")) as f:
        return [r for r in csv.DictReader(f) if all(str(r[k]) == str(v) for k, v in match.items())]


say(f"=== run-reconverge at {head}, {time.ctime()}")

# 1
bench("--cpu", "64", "--periods", ",".join(map(str, PERIODS)), "--seconds", "20", "--timeout", "1000")

# 2
bench("--sizes", "64", "--periods", ",".join(map(str, PERIODS)), "--kills", "1", "--runs", "10",
      "--timeout", "200", "--tag", "period")
chosen = None
for T in PERIODS:
    rs = rows("reconverge", tag="period", period_ms=T)
    within = sum(1 for r in rs if r["ring_s"] not in ("", "None") and float(r["ring_s"]) <= 1.0)
    false_ev = sum(int(r["false_evictions"]) for r in rs)
    ok = len(rs) == 10 and within >= 9 and false_ev == 0
    say(f"RULE T: period {T} ms: {within}/{len(rs)} runs ring-correct within 1 s, "
        f"{false_ev} false evictions -> {'qualifies' if ok else 'does not qualify'}")
    if ok:
        chosen = T                                   # the LONGEST qualifying period wins
if chosen is None:
    sys.exit("RULE T: no period qualifies; stopping")
say(f"RULE T -> T = {chosen} ms")

# 3
for n, T in ((64, PERIODS[0]), (256, chosen)):
    bench("--calibrate", str(n), "--periods", str(T), "--seconds", "30", "--timeout", "1000")
cal = [r for r in rows("calibrate")
       if (int(r["n"]), int(r["period_ms"])) in ((64, PERIODS[0]), (256, chosen))]
need_tail = 10 * max(float(r["p999_ms"]) for r in cal)
need_max = max(float(r["max_ms"]) for r in cal)
fits = [c for c in CANDIDATES if c >= need_tail and c > need_max]
say(f"RULE D: 10 x p99.9 = {need_tail:.1f} ms, slowest = {need_max:.1f} ms -> candidates {fits}")
if not fits:
    sys.exit("RULE D: no candidate qualifies; stopping")
D = fits[0]
say(f"RULE D -> D = {D} ms")

# 4
T, Dt = str(chosen), str(D)
bench("--sizes", "16,32,64,128,256", "--periods", T, "--kills", "1", "--runs", "5", "--timeout", Dt, "--tag", "size")
bench("--sizes", "64", "--periods", T, "--kills", "2,4,8,16", "--runs", "5", "--timeout", Dt, "--tag", "kills")
bench("--sizes", "64", "--periods", T, "--kills", "4,8,9", "--runs", "5", "--timeout", Dt, "--adjacent",
      "--tag", "adjacent")
bench("--sizes", "64", "--periods", T, "--kills", "1", "--runs", "5", "--timeout", Dt, "--load", "200",
      "--tag", "load200")
bench("--sizes", "64", "--periods", T, "--kills", "1", "--runs", "5", "--timeout", Dt, "--load", "1000",
      "--tag", "load1000")

# 5
bench("--calibrate", "256", "--periods", "50", "--seconds", "30", "--timeout", "1000")
say(f"=== done {time.ctime()}  T = {chosen} ms, D = {D} ms")
