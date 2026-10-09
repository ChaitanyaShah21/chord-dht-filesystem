#!/usr/bin/env bash
# e2e-chord.sh -- the C++ lookup originator against a ring of real node processes.
#
# Starts N nodes, then drives ./lookup (the originator, lookup.cpp) over real
# sockets and checks every answer against oracles written here, independently
# of the C++ code: the owner is the nearest node clockwise from the key, and the
# path is a distance-based model of the finger rule, compared node by node.
#
# Then breaks the ring on purpose:
#   - a node killed with SIGKILL must produce a "refused" error naming that node,
#     and lookups that do not pass through it must still succeed;
#   - a node frozen with SIGSTOP must produce a "timeout" error naming that node,
#     after the originator's 5 s deadline -- and must work again after SIGCONT,
#     because a timeout is suspicion, not proof of death (decision D-013).
set -uo pipefail
set +m   # no job-control "Killed" notices when cleanup reaps the nodes

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="${TMPDIR:-/tmp}/p2p-chord-$$"
BASE_PORT="${BASE_PORT:-7500}"
N="${N:-16}"
LAST_PORT=$((BASE_PORT + N - 1))

# Kill only this suite's nodes, by exact command line and port -- never a
# machine-wide pkill, which once let two suites kill each other's nodes. The trailing space stops port 7500 from
# also matching 75001.
reap() {
  local p i
  for p in $(seq "$BASE_PORT" "$LAST_PORT"); do
    pkill -CONT -f "$ROOT/node 127.0.0.1 $p " 2>/dev/null   # a stopped process cannot die until resumed
    pkill -9 -f "$ROOT/node 127.0.0.1 $p " 2>/dev/null
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

for p in $(seq "$BASE_PORT" "$LAST_PORT"); do echo "127.0.0.1:$p"; done > "$WORK/members.txt"

# Record each node's process id at launch, so the tests can signal exactly one.
PIDS=""
for p in $(seq "$BASE_PORT" "$LAST_PORT"); do
  "$ROOT/node" 127.0.0.1 "$p" "$WORK/members.txt" 2>"$WORK/node-$p.err" &
  PIDS="$PIDS $p:$!"
  disown "$!"    # the tests kill nodes on purpose; no job notices for that. reap() still finds them.
done

echo "--- e2e-chord ($N-node ring, C++ originator) ---"

python3 - "$ROOT" "$BASE_PORT" "$N" "$PIDS" <<'PY'
import hashlib, os, random, signal, socket, subprocess, sys, time

root, base, n = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
pid_of = {int(p): int(q) for p, q in (x.split(":") for x in sys.argv[4].split())}
MOD = 1 << 64
ports = list(range(base, base + n))
failures = 0

def check(ok, what):
    global failures
    if not ok:
        failures += 1
    print(f"  {'PASS' if ok else 'FAIL'}  {what}")

# ---- oracles, independent of the C++ code ----
def ident(port):
    return int(hashlib.sha1(f"127.0.0.1:{port}".encode()).hexdigest()[:16], 16)

def oracle_owner(k):
    return min(ports, key=lambda p: (ident(p) - k) % MOD)

FINGERS = {p: [oracle_owner((ident(p) + (1 << i)) % MOD) for i in range(64)] for p in ports}

def model_path(k, start):
    """The ports an iterative originator should ask, and the owner it should get."""
    cur, path = start, []
    while True:
        path.append(cur)
        me, succ = ident(cur), FINGERS[cur][0]
        s = (ident(succ) - me) % MOD
        d = (k - me) % MOD
        if s == 0 or 0 < d <= s:
            return path, succ
        span = d if d != 0 else MOD
        ahead = [f for f in FINGERS[cur] if 0 < (ident(f) - me) % MOD < span]
        cur = max(ahead, key=lambda f: (ident(f) - me) % MOD) if ahead else succ

# ---- driving the C++ originator ----
def run_lookup(start, keys, timeout=60):
    """Run ./lookup once from `start`; return (exit status, output lines, seconds)."""
    t0 = time.monotonic()
    proc = subprocess.run([f"{root}/lookup", "127.0.0.1", str(start)],
                          input="".join(f"{k}\n" for k in keys), capture_output=True,
                          text=True, timeout=timeout)
    return proc.returncode, proc.stdout.splitlines(), time.monotonic() - t0

def parse(line):
    """'<key> OWNER <id> <ip>:<port> requests R hops H path a,b' or the ERROR form."""
    head, _, message = line.partition(" : ")
    w = head.split()
    out = {"key": w[0], "status": w[1]}
    if w[1] == "OWNER":
        out.update(owner_id=w[2], owner_port=int(w[3].split(":")[1]),
                   requests=int(w[5]), hops=int(w[7]), path=w[9])
    else:
        out.update(kind=w[2], requests=int(w[4]), hops=int(w[6]), path=w[8], message=message)
    out["path_ports"] = [] if out["path"] == "-" else [int(x.split(":")[1]) for x in out["path"].split(",")]
    return out

def wait_ready(port):
    deadline = time.time() + 10
    while time.time() < deadline:
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=1)
            s.sendall(b"PING\n")
            ok = s.makefile("rb").readline().startswith(b"PONG ")
            s.close()
            if ok:
                return True
        except OSError:
            time.sleep(0.1)
    return False

check(all(wait_ready(p) for p in ports), "every node started and answers PING")

# ---- correctness: every start node, keys on every boundary ----
ids = sorted(ident(p) for p in ports)
keys = set()
for i in ids:
    keys.update({i, (i + 1) % MOD, (i - 1) % MOD})
keys.update({0, MOD - 1})
rng = random.Random(20261007)
keys.update(rng.randrange(MOD) for _ in range(200))
keys = sorted(keys)

total = wrong_owner = wrong_path = bad_counts = bad_exit = 0
worst = 0
for start in ports:
    rc, lines, _ = run_lookup(start, [f"{k:016x}" for k in keys])
    if rc != 0 or len(lines) != len(keys):
        bad_exit += 1
    for k, line in zip(keys, lines):
        r = parse(line)
        total += 1
        path, owner = model_path(k, start)
        if r["status"] != "OWNER" or r["owner_port"] != oracle_owner(k) or int(r["owner_id"], 16) != ident(oracle_owner(k)):
            wrong_owner += 1
            if wrong_owner <= 3:
                print(f"        {line}\n        oracle owner {oracle_owner(k)}")
            continue
        if r["path_ports"] != path:
            wrong_path += 1
            if wrong_path <= 3:
                print(f"        {line}\n        model path {path}")
        if r["requests"] != len(r["path_ports"]) or r["hops"] != r["requests"] - 1:
            bad_counts += 1
        worst = max(worst, r["requests"])

check(bad_exit == 0, f"./lookup exits 0 with one line per key from all {n} start nodes")
check(wrong_owner == 0, f"{total} lookups ({len(keys)} keys x {n} starts) all reach the oracle's owner")
check(wrong_path == 0, "every path equals the independent model's path, node for node")
check(bad_counts == 0, f"requests = path length and hops = requests - 1 on every line (worst {worst} requests)")

# ---- a bad input line is reported and the run continues ----
rc, lines, _ = run_lookup(ports[0], [f"{keys[0]:016x}", "not-a-key", f"{keys[1]:016x}"])
check(rc == 1 and len(lines) == 3 and lines[1].startswith("? ERROR bad-input") and
      lines[0].split()[1] == "OWNER" and lines[2].split()[1] == "OWNER",
      "a bad input line gets its own ERROR line, and the lookups after it still run")

# ---- pick victims that lie in the MIDDLE of some lookup's path ----
def through(victim, avoid=()):
    """A (key, start) whose model path visits `victim` after the first request."""
    for start in ports:
        if start == victim or start in avoid:
            continue
        for k in keys:
            path, _ = model_path(k, start)
            if victim in path[1:] and not set(avoid) & set(path):
                return k, start, path
    return None

by_traffic = sorted(ports, key=lambda p: -sum(p in model_path(k, s)[0][1:] for s in ports[:4] for k in keys[:40]))
dead, frozen = by_traffic[0], by_traffic[1]
k_dead, s_dead, path_dead = through(dead)
k_froz, s_froz, path_froz = through(frozen, avoid=(dead,))

# ---- a killed node: refused, naming that node ----
os.kill(pid_of[dead], signal.SIGKILL)
for _ in range(40):                                 # wait until its port is really closed
    try:
        socket.create_connection(("127.0.0.1", dead), timeout=0.5).close()
        time.sleep(0.05)
    except ConnectionRefusedError:
        break
    except OSError:                                 # accepted into the dying listener's backlog, then
        time.sleep(0.05)                            # reset as the kernel closed it: not closed yet

rc, lines, _ = run_lookup(s_dead, [f"{k_dead:016x}"])
r = parse(lines[0])
cut = path_dead.index(dead) + 1
check(r["status"] == "ERROR" and r["kind"] == "refused" and f"127.0.0.1:{dead} " in r["message"],
      f"a SIGKILLed node gives 'refused', naming 127.0.0.1:{dead}")
check(r["status"] == "ERROR" and r["path_ports"] == path_dead[:cut],
      "the reported path is the model's path up to and including the dead node")

rc, lines, _ = run_lookup(dead, [f"{k_dead:016x}"])
r = parse(lines[0])
check(r["status"] == "ERROR" and r["kind"] == "refused" and r["path_ports"] == [dead],
      "starting at the dead node fails on the first request, naming it")

clear = next(k for k in keys if dead not in model_path(k, s_dead)[0] and model_path(k, s_dead)[1] != dead)
rc, lines, _ = run_lookup(s_dead, [f"{clear:016x}"])
check(parse(lines[0])["status"] == "OWNER", "a lookup whose path avoids the dead node still succeeds")

# ---- a frozen node: timeout, naming that node, then recovery ----
os.kill(pid_of[frozen], signal.SIGSTOP)
rc, lines, secs = run_lookup(s_froz, [f"{k_froz:016x}"])
r = parse(lines[0])
check(r["status"] == "ERROR" and r["kind"] == "timeout" and f"127.0.0.1:{frozen} " in r["message"],
      f"a SIGSTOPped node gives 'timeout', naming 127.0.0.1:{frozen}")
check(4.5 <= secs < 8, f"the timeout fires at the 5 s deadline (took {secs:.2f} s)")
check(r["status"] == "ERROR" and r["kind"] != "refused", "a frozen node is never reported as refused (D-013)")

os.kill(pid_of[frozen], signal.SIGCONT)
rc, lines, _ = run_lookup(s_froz, [f"{k_froz:016x}"])
r = parse(lines[0])
check(r["status"] == "OWNER" and r["owner_port"] == oracle_owner(k_froz),
      "after SIGCONT the same lookup succeeds: a timeout was suspicion, not death")

sys.exit(1 if failures else 0)
PY
rc=$?

echo
if [[ $rc -eq 0 ]]; then echo "PASS -- the C++ originator routes correctly and names the failing node"
else echo "FAIL"; for f in "$WORK"/node-*.err; do echo "--- $f"; tail -3 "$f"; done; fi
exit $rc
