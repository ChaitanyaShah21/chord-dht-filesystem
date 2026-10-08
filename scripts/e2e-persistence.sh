#!/usr/bin/env bash
# e2e-persistence.sh -- does the tracker's state actually survive a restart?
#
# Phase A registers names over a raw socket and proves they are live. The tracker
# is then killed with SIGKILL -- no chance to tidy up -- and restarted on the same
# port and directory, so it replays the same log. Phase B asks for the same facts
# back. Phase C damages the log the way a crash or a bad disk would (a torn final
# record, a record whose key was cut short, a line in the old tracker's format)
# and checks that recovery keeps every good record, skips the bad ones, says so,
# and accepts new registrations afterwards (D-058).
#
# History: written in Phase 0 as the regression test for defect R1 -- the
# coursework tracker replayed its log through its own authorisation guards, which
# refused every record, so nothing but the user list survived a restart -- and
# committed failing on purpose (D-006) until D-009 fixed it. Re-pointed at the
# namespace tracker in Phase 5 (D-060); the property is the same: what the tracker
# acknowledged before the restart, it still knows after it.
set -uo pipefail
set +m

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="${TMPDIR:-/tmp}/p2p-persist-$$"
TRACKER_PORT="${TRACKER_PORT:-7101}"

reap () {
  pkill -9 -f "$ROOT/tracker $TRACKER_PORT" 2>/dev/null     # this port only (E6)
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
mkdir -p "$WORK"
reap || { echo "FAIL: port $TRACKER_PORT busy, cannot start"; exit 1; }

python3 - "$ROOT" "$WORK" "$TRACKER_PORT" <<'PY'
import hashlib, os, socket, subprocess, sys, time
root, work, port = sys.argv[1], sys.argv[2], int(sys.argv[3])
log_path = os.path.join(work, f"names_{port}.log")
fails = 0
def check(what, ok):
    global fails
    fails += 0 if ok else 1
    print(f"  {'PASS' if ok else 'FAIL'}  {what}", flush=True)

def start(tag):
    err = open(os.path.join(work, f"tracker-{tag}.err"), "w")
    p = subprocess.Popen([f"{root}/tracker", str(port), "--dir", work], stdout=subprocess.DEVNULL, stderr=err)
    for _ in range(100):
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.5).close()
            return p
        except OSError:
            time.sleep(0.05)
    return p

def ask(line):
    with socket.create_connection(("127.0.0.1", port), timeout=5) as s:
        s.sendall(line.encode() + b"\n")
        return s.makefile("rb").readline().decode().strip()

key = lambda i: hashlib.sha1(f"manifest {i}".encode()).hexdigest()
names = {f"report-{i}.pdf": key(i) for i in range(4)}

print("--- phase A: state built while the tracker is running ---")
t = start("a")
for n, k in names.items():
    check(f"REGISTER {n}", ask(f"REGISTER {n} {k}") == "OK")
check("a taken name is refused", ask(f"REGISTER report-0.pdf {key(99)}") == f"EXISTS {key(0)}")
check("the log holds four records", open(log_path).read().count("\n") == 4)

print("--- SIGKILL, then restart on the same port and directory ---")
t.kill(); t.wait()
t = start("b")
print("--- phase B: the same facts, asked after the restart ---")
for n, k in names.items():
    check(f"RESOLVE {n} after the restart", ask(f"RESOLVE {n}") == f"FILE {k}")
check("still written once after the restart", ask(f"REGISTER report-1.pdf {key(98)}") == f"EXISTS {key(1)}")
check("LIST after the restart", ask("LIST") == "NAMES 4 " + " ".join(sorted(names)))

print("--- phase C: a damaged log ---")
t.kill(); t.wait()
good = open(log_path, "rb").read()
with open(log_path, "ab") as f:
    rec = f"REGISTER cut.pdf {key(5)}"
    rec += " " + hashlib.sha1(rec.encode()).hexdigest()[:8]
    f.write(rec[:40].encode() + b"\n")          # a record cut inside its key, newline intact
    f.write(b"0 create_user alice pw\n")         # the old tracker's log format
    f.write(b"REGISTER torn.pdf " + key(6)[:20].encode())   # torn by a crash: no newline
t = start("c")
report = open(os.path.join(work, "tracker-c.err")).read()
for n, k in names.items():
    check(f"{n} survives the damage", ask(f"RESOLVE {n}") == f"FILE {k}")
check("the cut and the torn records are not replayed", ask("RESOLVE cut.pdf") == "MISSING" and ask("RESOLVE torn.pdf") == "MISSING")
check("recovery says what it skipped and cut off", "2 damaged records skipped" in report and "torn final record" in report)
check("the torn tail is cut off the file", not open(log_path, "rb").read().endswith(key(6)[:20].encode()))
check("a registration after recovery succeeds", ask(f"REGISTER after.pdf {key(7)}") == "OK")
t.kill(); t.wait()
t = start("d")
check("and survives the next restart", ask("RESOLVE after.pdf") == f"FILE {key(7)}")
t.kill(); t.wait()

print("--- what the tracker reported on each start ---")
for tag in "abcd":
    print(f"  [{tag}] " + open(os.path.join(work, f"tracker-{tag}.err")).read().strip().replace("\n", "\n      "))
if fails == 0:
    print("\nPASS -- state survives a restart")
    sys.exit(0)
print(f"\nFAIL -- {fails} check(s) failed")
sys.exit(1)
PY
