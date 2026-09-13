#!/usr/bin/env bash
# e2e-node.sh -- a fixed ring of real node processes answers every lookup correctly.
#
# Starts N nodes from one membership file, then walks lookups from every node
# for keys chosen to sit on boundaries, and checks each answer against an
# oracle computed here, independently of the C++ code: SHA-1 of "ip:port",
# top 16 hex digits, nearest node clockwise. The checker is not the thing
# being checked (decision D-020).
#
# Also checks: a one-node ring owns every key; malformed requests get ERR and
# the node survives; an over-long line is dropped; a node that is not in its
# own membership file refuses to start.
set -uo pipefail
set +m   # no job-control "Killed" notices when cleanup reaps the nodes

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="${TMPDIR:-/tmp}/p2p-node-$$"
BASE_PORT="${BASE_PORT:-7300}"
N="${N:-8}"
ALONE_PORT=$((BASE_PORT + N))          # the one-node ring
STRAY_PORT=$((BASE_PORT + N + 1))      # a node missing from its own file

# Kill only this suite's nodes, by exact command line -- never a machine-wide
# pkill, which is how two suites once killed each other (error log E6). The
# trailing space stops port 7300 from also matching 73001.
reap() {
  local p i
  for p in $(seq "$BASE_PORT" "$STRAY_PORT"); do
    pkill -9 -f "$ROOT/node 127.0.0.1 $p " 2>/dev/null
  done
  for i in $(seq 40); do
    ss -ltn 2>/dev/null | grep -qE ":($(seq -s '|' "$BASE_PORT" "$STRAY_PORT"))[[:space:]]" || return 0
    sleep 0.25
  done
  return 1
}
cleanup() { reap >/dev/null 2>&1; rm -rf "$WORK"; }
trap cleanup EXIT

[[ -x "$ROOT/node" ]] || { echo "FAIL: $ROOT/node not built. Run 'make' first."; exit 1; }
mkdir -p "$WORK"
reap || { echo "FAIL: ports $BASE_PORT-$STRAY_PORT busy, cannot start"; exit 1; }

# ---- the ring ----
{
  echo "# e2e-node ring"
  for p in $(seq "$BASE_PORT" $((BASE_PORT + N - 1))); do echo "127.0.0.1:$p"; done
} > "$WORK/members.txt"
echo "127.0.0.1:$ALONE_PORT" > "$WORK/alone.txt"

for p in $(seq "$BASE_PORT" $((BASE_PORT + N - 1))); do
  "$ROOT/node" 127.0.0.1 "$p" "$WORK/members.txt" 2>"$WORK/node-$p.err" &
done
"$ROOT/node" 127.0.0.1 "$ALONE_PORT" "$WORK/alone.txt" 2>"$WORK/node-$ALONE_PORT.err" &

echo "--- e2e-node ($N-node ring + one-node ring) ---"

python3 - "$BASE_PORT" "$N" "$ALONE_PORT" <<'PY'
import hashlib, random, socket, sys, time

base, n, alone = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
MOD = 1 << 64
ports = list(range(base, base + n))
failures = 0

def check(ok, what):
    global failures
    if not ok:
        failures += 1
    print(f"  {'PASS' if ok else 'FAIL'}  {what}")

# ---- the oracle: independent of chord.cpp ----
def ident(port):
    return int(hashlib.sha1(f"127.0.0.1:{port}".encode()).hexdigest()[:16], 16)

def oracle_owner(k, ring_ports):
    # nearest node clockwise from k; distance 0 means k is the node's own id
    return min(ring_ports, key=lambda p: (ident(p) - k) % MOD)

# ---- talking to nodes ----
conns = {}
def ask(port, line):
    if port not in conns:
        s = socket.create_connection(("127.0.0.1", port), timeout=5)
        conns[port] = (s, s.makefile("rb"))
    s, f = conns[port]
    s.sendall(line.encode() + b"\n")
    reply = f.readline()
    if not reply.endswith(b"\n"):
        raise ConnectionError(f"no complete reply from {port} to {line!r}")
    return reply.decode().rstrip("\n")

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

check(all(wait_ready(p) for p in ports + [alone]), "every node started and answers PING")

# ---- PING reports the identifier the oracle expects ----
check(all(ask(p, "PING") == f"PONG {ident(p):016x}" for p in ports),
      "each node's identifier matches SHA-1 of its own address")

# ---- lookups: walk NEXT until OWNER, the way an iterative originator does ----
def lookup(k, start):
    cur, hops = start, 0
    while hops <= n:                    # more than n steps on an n-node ring is a loop
        words = ask(cur, f"FIND_SUCCESSOR {k:016x}").split()
        hops += 1
        if len(words) != 4 or words[0] not in ("OWNER", "NEXT"):
            raise ValueError(f"malformed reply {words!r}")
        if words[0] == "OWNER":
            return int(words[3]), hops
        cur = int(words[3])
    raise RuntimeError(f"lookup for {k:016x} from {start} did not terminate")

ids = sorted(ident(p) for p in ports)
keys = set()
for i in ids:
    keys.update({i, (i + 1) % MOD, (i - 1) % MOD})    # on, just past, just before each node
keys.update({0, MOD - 1})                             # both ends of the space
rng = random.Random(20260913)
keys.update(rng.randrange(MOD) for _ in range(200))

wrong, worst, total = 0, 0, 0
for k in sorted(keys):
    for start in ports:
        owner, hops = lookup(k, start)
        total += 1
        worst = max(worst, hops)
        if owner != oracle_owner(k, ports):
            wrong += 1
            if wrong <= 3:
                print(f"        key {k:016x} from {start}: got {owner}, oracle {oracle_owner(k, ports)}")
check(wrong == 0, f"{total} lookups ({len(keys)} keys x {n} start nodes) all reach the oracle's owner")
check(worst <= n, f"no lookup took more than {n} hops (worst: {worst}) -- O(N) walk, no loops")

# ---- one-node ring: (n, n] is the whole ring ----
alone_reply_ok = all(
    ask(alone, f"FIND_SUCCESSOR {k:016x}").split()[:1] == ["OWNER"] and
    int(ask(alone, f"FIND_SUCCESSOR {k:016x}").split()[3]) == alone
    for k in [0, MOD - 1, ident(alone), (ident(alone) + 1) % MOD] + [rng.randrange(MOD) for _ in range(20)])
check(alone_reply_ok, "a one-node ring answers OWNER itself for every key")

# ---- malformed input: ERR, and the node keeps serving ----
victim = ports[0]
bad = ["", "FIND_SUCCESSOR", "FIND_SUCCESSOR xyz",
       "FIND_SUCCESSOR 0123456789abcdef extra", "FIND_SUCCESSOR 0x23456789abcdef",
       "HELLO", "find_successor 0123456789abcdef"]
check(all(ask(victim, b).startswith("ERR ") for b in bad), f"{len(bad)} malformed requests each get one ERR line")
check(ask(victim, "PING").startswith("PONG "), "the same connection still answers after them")

check(ask(victim, "PING\r").startswith("PONG "), "a request ending in \\r\\n is accepted")

# An over-long line: the node must drop the connection, not buffer forever.
s = socket.create_connection(("127.0.0.1", victim), timeout=5)
try:
    s.sendall(b"A" * 5000)
    s.settimeout(5)
    dropped = s.recv(1) == b""
except (ConnectionResetError, BrokenPipeError):
    dropped = True
except socket.timeout:
    dropped = False
s.close()
check(dropped, "a 5000-byte line with no newline gets the connection closed")

try:
    fresh = socket.create_connection(("127.0.0.1", victim), timeout=5)
    fresh.sendall(b"PING\n")
    alive = fresh.makefile("rb").readline().startswith(b"PONG ")
    fresh.close()
except OSError:
    alive = False
check(alive, "the node still accepts and answers new connections afterwards")

sys.exit(1 if failures else 0)
PY
rc=$?

# ---- a node missing from its own membership file must refuse to start ----
timeout 5 "$ROOT/node" 127.0.0.1 "$STRAY_PORT" "$WORK/members.txt" 2>"$WORK/stray.err"
stray_rc=$?
if [[ $stray_rc -eq 1 ]] && grep -q "is not in" "$WORK/stray.err"; then
  echo "  PASS  a node not listed in its membership file refuses to start"
else
  echo "  FAIL  unlisted node exited $stray_rc: $(head -1 "$WORK/stray.err")"
  rc=1
fi

echo
if [[ $rc -eq 0 ]]; then echo "PASS -- the fixed ring routes every lookup to the right owner"
else echo "FAIL"; for f in "$WORK"/node-*.err; do echo "--- $f"; tail -3 "$f"; done; fi
exit $rc
