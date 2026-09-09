#!/usr/bin/env bash
# e2e-framing.sh -- one command, one reply. Always.
#
# Regression test for defect R6 (docs/failures.md) and for the framing rule
# decided as fork F9: a reply is exactly one line, guaranteed by send_all rather
# than by the author of each message.
#
# Why this drives a raw socket instead of ./client: the defect is a property of
# the bytes on the wire. The client reads one line per reply, so it would show
# the *symptom* (answers arriving one behind) without ever revealing which side
# emitted an extra line. A raw socket counts lines, which is the actual claim.
set -uo pipefail
set +m

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="${TMPDIR:-/tmp}/p2p-framing-$$"
TRACKER_PORT="${TRACKER_PORT:-7102}"

# Reap only this suite's processes. Matching on the binary path alone kills
# every tracker on the machine, including one another suite is using -- that is
# error-log entry E6, where two green suites run together came out red.
reap () {
  pkill -9 -f "$ROOT/tracker $TRACKER_PORT" 2>/dev/null
  local i
  for i in $(seq 40); do
    ss -ltn 2>/dev/null | grep -qE ":${TRACKER_PORT}[[:space:]]" || return 0
    sleep 0.25
  done
  echo "WARN: port $TRACKER_PORT still held after 10s"
  return 1
}
cleanup() { reap >/dev/null 2>&1; rm -rf "$WORK"; }
trap cleanup EXIT

[[ -x "$ROOT/tracker" ]] || { echo "FAIL: $ROOT/tracker not built. Run 'make' first."; exit 1; }

mkdir -p "$WORK"; cd "$WORK" || exit 1
reap || { echo "FAIL: port busy, cannot start"; exit 1; }

"$ROOT/tracker" "$TRACKER_PORT" >t.out 2>t.err & disown
sleep 1

python3 - "$TRACKER_PORT" <<'PY'
import socket, sys, time

port = int(sys.argv[1])
s = socket.create_connection(("127.0.0.1", port), timeout=10)
s.settimeout(5)
buf = b""

def readline():
    """One reply, or None if the tracker sent nothing within the timeout."""
    global buf
    while b"\n" not in buf:
        try:
            chunk = s.recv(4096)
        except socket.timeout:
            return None
        if not chunk:
            return None
        buf += chunk
    line, _, buf = buf.partition(b"\n")
    return line.decode(errors="replace").rstrip("\r")

def send(cmd):
    s.sendall((cmd + "\n").encode())
    time.sleep(0.2)

readline()                      # the TRACKERS greeting

fails = []
def check(label, ok, detail=""):
    print(f"  {'PASS' if ok else 'FAIL'}  {label}{('  -- ' + detail) if detail and not ok else ''}")
    if not ok:
        fails.append(label)

# Every malformed command that used to answer in two lines. After each one, a
# command whose reply is unmistakable: if framing broke, the answer to the probe
# is the leftover half of the previous reply instead.
# Each of these reaches its "Invalid input. Use: ..." branch: in every one of
# these handlers the argument check runs before the authorisation check, so an
# unauthenticated socket lands on the reply that used to be two lines.
malformed = [
    "create_user",
    "login",
    "create_group",
    "join_group",
    "upload_file g1 alice",
]

for i, bad in enumerate(malformed):
    send(bad)
    reply = readline()
    check(f"{bad.split()[0]:<12} answers in one line",
          reply is not None and "Use:" in reply,
          f"got {reply!r}")

    # The probe. Its reply is distinctive, so a one-behind stream is visible.
    probe_user = f"probe{i}"
    send(f"create_user {probe_user} pw")
    reply = readline()
    check(f"stream still aligned after {bad.split()[0]}",
          reply is not None and "created" in reply.lower(),
          f"got {reply!r}")

# Nothing may be left over: a second reply to the last command means two lines
# were emitted and the desync is simply one probe away.
leftover = readline()
check("no unread reply left in the stream", leftover is None, f"leftover {leftover!r}")

print()
if fails:
    print(f"FAIL -- {len(fails)} check(s) failed (defect R6)")
    sys.exit(1)
print("PASS -- one command, one reply, on every malformed-input path")
PY
rc=$?

if [[ $rc -ne 0 ]]; then
  echo "--- tracker.err ---"; cat t.err
fi
exit $rc
