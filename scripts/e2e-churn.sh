#!/usr/bin/env bash
# e2e-churn.sh -- joins, kills, freezes and rejoins on a ring of real node processes.
#
# Every check is made by an oracle in this harness (scripts/ringcheck.py), never
# by the nodes: the correct successor, predecessor, successor list and fingers of
# every live node are computed from the live addresses alone, and each node is
# probed for what it actually holds (decision D-038).
#
#   1. a ring built by joins converges, and every lookup reaches the right owner
#   2. four nodes joining at the same moment, through the same node
#   3. a node killed (SIGKILL), then three adjacent nodes killed together
#   4. a node frozen (SIGSTOP): evicted after two strikes, back after SIGCONT
#   5. a node killed and restarted at its old address before the ring notices
#   6. a join through a dead address gives up and exits non-zero
#   7. on a fixed ring (no maintenance, so nothing races the test): a lookup
#      through a dead node fails without --retries, and with --retries the
#      client's SUSPECT makes the namer evict it and the retry succeeds (D-036)
#
# NODE_CMD runs a different node binary, e.g. the ThreadSanitizer build:
#   NODE_CMD="setarch $(uname -m) -R ./node-tsan" scripts/e2e-churn.sh
set -uo pipefail
set +m

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="${TMPDIR:-/tmp}/p2p-churn-$$"
BASE_PORT="${BASE_PORT:-7600}"
LAST_PORT=$((BASE_PORT + 59))

# Kill only this suite's nodes, matched by port -- never a machine-wide pkill
# (two suites once killed each other's nodes that way). The pattern lives in this file, not on a shell's command line,
# so it cannot match the shell running it.
reap() {
  local p i
  for p in $(seq "$BASE_PORT" "$LAST_PORT"); do
    pkill -CONT -f "node[-a-z]* 127.0.0.1 $p " 2>/dev/null
    pkill -9 -f "node[-a-z]* 127.0.0.1 $p " 2>/dev/null
  done
  for i in $(seq 40); do
    ss -ltn 2>/dev/null | grep -qE ":($(seq -s '|' "$BASE_PORT" "$LAST_PORT"))[[:space:]]" || return 0
    sleep 0.25
  done
  return 1
}
cleanup() { reap >/dev/null 2>&1; rm -rf "$WORK"; }
trap cleanup EXIT

for bin in node lookup; do
  [[ -x "$ROOT/$bin" ]] || { echo "FAIL: $ROOT/$bin not built. Run 'make' first."; exit 1; }
done
mkdir -p "$WORK"
reap || { echo "FAIL: ports $BASE_PORT-$LAST_PORT busy, cannot start"; exit 1; }

echo "--- e2e-churn (real processes; ${NODE_CMD:-$ROOT/node}) ---"
cd "$ROOT"
python3 - "$ROOT" "$WORK" "$BASE_PORT" "${NODE_CMD:-$ROOT/node}" <<'PY'
import os, random, shlex, signal, subprocess, sys, time
root, work, base, node_cmd = sys.argv[1], sys.argv[2], int(sys.argv[3]), shlex.split(sys.argv[4])
sys.path.insert(0, os.path.join(root, "scripts"))
from ringcheck import Cluster, Oracle, Poller, ident, wait_converged, wait_listening

PERIOD, TIMEOUT, R = 100, 500, 8
failures = 0
def check(ok, what):
    global failures
    failures += 0 if ok else 1
    print(f"  {'PASS' if ok else 'FAIL'}  {what}", flush=True)

def addr(i):
    return f"127.0.0.1:{base + i}"

class Nodes(Cluster):
    """Cluster, but started through NODE_CMD so a sanitizer build can be swapped in."""
    def start(self, a, mode):
        ip, port = a.split(":")
        log = open(os.path.join(self.logdir, f"node-{port}.log"), "a")
        self.procs[a] = subprocess.Popen(node_cmd + [ip, port] + mode + self.opts,
                                         stdout=subprocess.DEVNULL, stderr=log)
        log.close()
        return self.procs[a]

def converge(live, limit, label):
    t0 = time.monotonic()
    ok, polls, v = wait_converged(poller, Oracle(live, R), limit, stable_s=3 * PERIOD / 1000)
    check(ok, f"{label}: successors, predecessors, lists and fingers all match the oracle "
              f"({time.monotonic() - t0:.2f} s, {polls} polls)" + ("" if ok else f" -- wrong: {v[3][:4]}"))
    return ok

def lookups_correct(live, starts, keys_per_start, label, retries=None):
    oracle = Oracle(live, R)
    rng = random.Random(len(live))
    wrong = total = 0
    for s in starts:
        ip, port = s.split(":")
        keys = [rng.randrange(1 << 64) for _ in range(keys_per_start)] + [ident(a) for a in live[:4]]
        cmd = [f"{root}/lookup"] + (["--retries", str(retries)] if retries is not None else []) + [ip, port]
        out = subprocess.run(cmd, input="".join(f"{k:016x}\n" for k in keys), capture_output=True,
                             text=True, timeout=120).stdout.splitlines()
        for k, line in zip(keys, out):
            w = line.split()
            total += 1
            if w[1] != "OWNER" or w[3] != oracle.owner(k):
                wrong += 1
                if wrong <= 3:
                    print(f"        {line}\n        oracle owner {oracle.owner(k)}")
        total += abs(len(keys) - len(out))
        wrong += abs(len(keys) - len(out))
    check(wrong == 0, f"{label}: {total} lookups from {len(starts)} nodes all reach the oracle's owner")

poller = Poller()
nodes = Nodes(os.path.join(root, "node"), os.path.join(work, "logs"), PERIOD, TIMEOUT, R)
try:
    # ---- 1. a ring built by joins ----
    live = [addr(0)]
    nodes.start(addr(0), ["--create"])
    check(wait_listening(addr(0)), "a node started with --create listens")
    rng = random.Random(20261007)
    for i in range(1, 16):
        known = rng.choice(live)
        nodes.start(addr(i), ["--join", known])
        if not wait_listening(addr(i)):
            check(False, f"{addr(i)} joining through {known} never started listening")
        live.append(addr(i))
    converge(live, 30, "16 nodes joined one at a time through random members")
    lookups_correct(live, live, 40, "after the joins")

    # ---- 2. concurrent joins through the same node ----
    batch = [addr(i) for i in range(16, 20)]
    for a in batch:
        nodes.start(a, ["--join", addr(0)])
    check(all(wait_listening(a) for a in batch), "four nodes joining at once through the same node all join")
    live += batch
    converge(live, 30, "after four concurrent joins")

    # ---- 3. kills ----
    victim = live[5]
    nodes.kill(victim)
    live.remove(victim)
    converge(live, 20, "one node killed")
    lookups_correct(live, live[:8], 40, "after the kill")

    order = sorted(live, key=ident)
    three = order[3:6]
    for a in three:
        nodes.kill(a)
        live.remove(a)
    converge(live, 20, "three adjacent nodes killed together")

    # ---- 4. a frozen node ----
    frozen = sorted(live, key=ident)[7]
    nodes.signal(frozen, signal.SIGSTOP)
    others = [a for a in live if a != frozen]
    t0 = time.monotonic()
    ok = converge(others, 20, "a SIGSTOPped node is evicted")
    if ok:
        bound = 3 * (PERIOD + TIMEOUT) / 1000 + 1.0
        check(time.monotonic() - t0 < bound,
              f"...within two strikes: {time.monotonic() - t0:.2f} s < {bound:.1f} s")
    nodes.signal(frozen, signal.SIGCONT)
    converge(live, 20, "after SIGCONT it re-inserts itself, with no message telling it to")
    evictors = [f for f in os.listdir(os.path.join(work, "logs"))
                if f"evict {frozen} (timeout, second strike)" in open(os.path.join(work, "logs", f)).read()]
    check(len(evictors) >= 1, f"the eviction was by two timeout strikes, never a refusal ({len(evictors)} node(s))")

    # ---- 5. killed and restarted at the same address at once ----
    back = sorted(live, key=ident)[2]
    nodes.kill(back)
    nodes.start(back, ["--join", live[0] if live[0] != back else live[1]])
    check(wait_listening(back, 20), "a node restarted at its old address immediately rejoins")
    converge(live, 30, "after the fast restart")
    lookups_correct(live, live[:6], 40, "after the fast restart")

    # ---- 6. joining through a dead address ----
    lone = addr(40)
    t0 = time.monotonic()
    p = subprocess.run(node_cmd + ["127.0.0.1", str(base + 40), "--join", addr(41), "--period", "20", "--vnodes", "1"],
                       capture_output=True, text=True, timeout=60)
    check(p.returncode == 1 and "giving up" in p.stderr,
          f"a join through a dead address gives up after its attempts and exits 1 ({time.monotonic() - t0:.1f} s)")

    nodes.kill_all()

    # ---- 7. the client's report and retry, on a fixed ring ----
    fixed = [addr(i) for i in range(44, 60)]
    with open(os.path.join(work, "fixed.txt"), "w") as f:
        f.write("".join(a + "\n" for a in fixed))
    procs = {a: subprocess.Popen(node_cmd + a.split(":") + [os.path.join(work, "fixed.txt")],
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL) for a in fixed}
    check(all(wait_listening(a) for a in fixed), "a 16-node fixed ring starts")
    found = None
    for s in fixed:
        ip, port = s.split(":")
        keys = [random.Random(s).randrange(1 << 64) for _ in range(30)]
        out = subprocess.run([f"{root}/lookup", ip, port], input="".join(f"{k:016x}\n" for k in keys),
                             capture_output=True, text=True).stdout.splitlines()
        for k, line in zip(keys, out):
            w = line.split()
            path = w[9].split(",")
            if len(path) >= 3 and path[1] != w[3]:
                found = (s, k, path)
                break
        if found:
            break
    check(found is not None, "a lookup with an intermediate hop exists")
    if found:
        s, k, path = found
        dead = path[1]
        procs[dead].kill(); procs[dead].wait()
        ip, port = s.split(":")
        plain = subprocess.run([f"{root}/lookup", ip, port], input=f"{k:016x}\n",
                               capture_output=True, text=True).stdout
        check(" ERROR refused " in plain and f"{dead} " in plain,
              f"without --retries: refused, naming {dead}")
        # Later hops may hold the same dead finger, so a retry can meet it again;
        # each meeting costs one SUSPECT and one more attempt.
        rec = subprocess.run([f"{root}/lookup", "--retries", "4", ip, port], input=f"{k:016x}\n",
                             capture_output=True, text=True).stdout.split()
        want = Oracle([a for a in fixed if a != dead], R).owner(k)
        check(rec[1] == "OWNER" and rec[3] == want and rec[-2] == "attempts" and int(rec[-1]) >= 2,
              f"with --retries: SUSPECT to the namer, a retry, and the right owner ({' '.join(rec[-2:])})")
        namer = path[0]
        from ringcheck import probe
        st = probe(namer)
        check(st is not None and dead not in st["fingers"],
              "the namer evicted the dead node on its own probe; nothing else in a fixed ring changes")
    for p in procs.values():
        p.kill(); p.wait()

    sanitizer = [f for f in os.listdir(os.path.join(work, "logs"))
                 if "ThreadSanitizer" in open(os.path.join(work, "logs", f)).read()]
    check(not sanitizer, "no ThreadSanitizer report in any node's log" if "tsan" in " ".join(node_cmd)
          else "no sanitizer output in any node's log")
finally:
    nodes.kill_all()
    poller.close()

sys.exit(1 if failures else 0)
PY
rc=$?
echo
if [[ $rc -eq 0 ]]; then echo "PASS -- the ring repairs itself through joins, deaths, freezes and restarts"
else
  echo "FAIL"
  for f in "$WORK"/logs/node-*.log; do [[ -f "$f" ]] && { echo "--- $f"; tail -4 "$f"; }; done
fi
exit $rc
