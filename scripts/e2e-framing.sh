#!/usr/bin/env bash
# e2e-framing.sh -- one request, one reply. Always.
#
# Sends the tracker every kind of malformed request over a raw socket and checks
# that each is answered with exactly one line, by following each with a probe
# whose reply is unmistakable: if any malformed request produced two lines, the
# probe's answer would be the leftover line instead. Then a line longer than the
# line limit, which must close the connection rather than be buffered or split.
#
# Why a raw socket and not ./client: the defect is a property of the bytes on the
# wire. The client reads one line per reply, so it would show the symptom (answers
# arriving one behind) without revealing which side emitted the extra line.
#
# History: written in Phase 0 as the regression test for defect R6 -- five
# coursework tracker replies contained an embedded newline -- and for the rule
# decided as fork F9 (D-010): a reply is exactly one line, guaranteed by the
# framing layer rather than by each author. Re-pointed at the namespace tracker in
# Phase 5 (D-060), whose framing is net.cpp's send_line, the same code the node uses.
set -uo pipefail
set +m

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="${TMPDIR:-/tmp}/p2p-framing-$$"
TRACKER_PORT="${TRACKER_PORT:-7102}"

reap () {
  pkill -9 -f "$ROOT/tracker $TRACKER_PORT" 2>/dev/null     # this port only, never machine-wide
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

"$ROOT/tracker" "$TRACKER_PORT" --dir "$WORK" >t.out 2>t.err & disown
for _ in $(seq 50); do ss -ltn | grep -qE ":${TRACKER_PORT}[[:space:]]" && break; sleep 0.1; done

python3 - "$TRACKER_PORT" <<'PY'
import socket, sys

port = int(sys.argv[1])
s = socket.create_connection(("127.0.0.1", port), timeout=10)
s.settimeout(2)
buf = b""

def readline():
    """One reply, or None if the tracker sent nothing within the timeout."""
    global buf
    while b"\n" not in buf:
        try:
            chunk = s.recv(4096)
        except (socket.timeout, ConnectionResetError):   # a reset is a close too: the tracker
            return None                                  # closed with bytes still unread
        if not chunk:
            return None
        buf += chunk
    line, _, buf = buf.partition(b"\n")
    return line.decode(errors="replace")

fails = []
def check(label, ok, detail=""):
    print(f"  {'PASS' if ok else 'FAIL'}  {label}{('  -- ' + detail) if detail and not ok else ''}")
    if not ok:
        fails.append(label)

key = "a" * 40
malformed = [
    "", "REGISTER", "REGISTER only-a-name", f"REGISTER name {key} extra", f"REGISTER name {'A' * 40}",
    "REGISTER name ../../../../etc/passwd", f"REGISTER {'n' * 201} {key}", f"REGISTER  name {key}",
    "RESOLVE", "RESOLVE two words", "LIST a b", "register lowercase", "BOGUS", "PING extra",
    "\r", "REGISTER name\t" + key,
]
for i, bad in enumerate(malformed):
    s.sendall(bad.encode() + b"\n")
    reply = readline()
    check(f"{bad[:30]!r:<34} answers in one ERR line", reply is not None and reply.startswith("ERR "), f"got {reply!r}")
    s.sendall(b"PING\n")                                   # the probe: its reply is unmistakable
    reply = readline()
    check(f"stream still aligned after {bad[:20]!r}", reply == "PONG", f"got {reply!r}")

check("no unread reply left in the stream", readline() is None)

# A line longer than MAX_LINE (1,024 bytes) is not buffered without limit and not
# split into two requests: the connection is closed.
s.sendall(b"RESOLVE " + b"x" * 2000 + b"\n")
def closed():
    try:
        return s.recv(1) == b""
    except ConnectionResetError:
        return True
check("an over-long line closes the connection", readline() is None and closed())
s.close()
s = socket.create_connection(("127.0.0.1", port), timeout=10)
s.settimeout(2)
buf = b""
s.sendall(b"PING\n")
check("and a new connection is served normally", readline() == "PONG")

print()
if fails:
    print(f"FAIL -- {len(fails)} check(s) failed (defect R6)")
    sys.exit(1)
print("PASS -- one command, one reply, on every malformed-input path")
PY
rc=$?
[[ $rc -ne 0 ]] && { echo "--- tracker.err ---"; cat t.err; }
exit $rc
