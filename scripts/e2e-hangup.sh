#!/usr/bin/env bash
# e2e-hangup.sh -- a client that vanishes must not take the tracker with it.
#
# Regression test for defect R9 (docs/failures.md), closed by decision D-011.
# Before the fix this killed the tracker with SIGPIPE -- exit 141 -- after about
# twenty-six commands. No hostile intent is required: Ctrl-C on a client at the
# wrong moment is the same event.
set -uo pipefail
set +m   # no job-control "Killed" notice when cleanup reaps the tracker

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="${TMPDIR:-/tmp}/p2p-hangup-$$"
TRACKER_PORT="${TRACKER_PORT:-7103}"
N="${N:-200}"

reap () {
  pkill -9 -f "$ROOT/tracker $TRACKER_PORT" 2>/dev/null
  local i
  for i in $(seq 40); do
    ss -ltn 2>/dev/null | grep -qE ":${TRACKER_PORT}[[:space:]]" || return 0
    sleep 0.25
  done
  return 1
}
cleanup() { reap >/dev/null 2>&1; rm -rf "$WORK"; }
trap cleanup EXIT

[[ -x "$ROOT/tracker" ]] || { echo "FAIL: $ROOT/tracker not built. Run 'make' first."; exit 1; }

mkdir -p "$WORK"; cd "$WORK" || exit 1
reap || { echo "FAIL: port busy, cannot start"; exit 1; }

# Run the tracker as a real child of a subshell, so $? carries the signal that
# killed it. Backgrounding with `disown` loses exactly the evidence this test is
# about: 141 means 128 + 13, and 13 is SIGPIPE.
# The subshell's own stderr is silenced: when cleanup kills the tracker, the
# subshell prints "Killed" for its foreground child, which lands after the
# verdict and makes a passing run read like a crash. The tracker's stderr is
# already captured in t.err, so nothing diagnostic is lost.
( "$ROOT/tracker" "$TRACKER_PORT" >t.out 2>t.err; echo "$?" > exit.txt ) 2>/dev/null &
SUB=$!
sleep 1

python3 - "$TRACKER_PORT" "$N" <<'PY'
import socket, struct, sys
port, n = int(sys.argv[1]), int(sys.argv[2])
s = socket.create_connection(("127.0.0.1", port), timeout=10)
# Pipeline commands so the tracker still has replies to write after we are gone,
# and never read a single one: the replies pile up in its send buffer.
s.sendall(b"".join(b"create_user u%d pw\n" % i for i in range(n)))
# SO_LINGER with a zero timeout makes close() send RST rather than FIN, so the
# connection is destroyed rather than half-closed -- the abrupt case.
s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
s.close()
print(f"  client pipelined {n} commands, read none, and reset the connection")
PY

sleep 2
echo "--- e2e-hangup ---"

rc=0
if kill -0 "$SUB" 2>/dev/null; then
  echo "  PASS  tracker still running after the client vanished"
else
  status="$(cat exit.txt 2>/dev/null || echo '?')"
  echo "  FAIL  tracker died, exit status ${status}$([[ "$status" == "141" ]] && echo '  (128+13 = SIGPIPE, defect R9)')"
  rc=1
fi

# Still serving, not merely still alive: a surviving process that stopped
# accepting would pass a liveness check and fail every user.
if python3 - "$TRACKER_PORT" 2>/dev/null; then
  echo "  PASS  tracker still accepts a new connection and answers it"
else
  echo "  FAIL  tracker is alive but no longer serving"
  rc=1
fi <<'PY'
import socket, sys
try:
    s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=5)
    s.settimeout(5)
    s.recv(4096)                                  # greeting
    s.sendall(b"create_user survivor pw\n")
    sys.exit(0 if b"\n" in s.recv(4096) else 1)
except Exception:
    sys.exit(1)
PY

# Detach the subshell before cleanup kills it, or bash reports "Killed" after
# the verdict and makes a passing run look like a crash.
disown "$SUB" 2>/dev/null

echo
if [[ $rc -eq 0 ]]; then echo "PASS -- a peer hanging up does not kill the process"; else
  echo "FAIL -- defect R9"; echo "--- tracker.err (last 5) ---"; tail -5 t.err
fi
exit $rc
