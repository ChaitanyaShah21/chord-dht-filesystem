#!/usr/bin/env bash
# e2e-persistence.sh -- does the tracker's state actually survive a restart?
#
# Phase A registers names over a raw socket and proves they are live. The tracker
# is then killed with SIGKILL -- no chance to tidy up -- and restarted on the same
# port and directory, so it replays the same log. Phase B asks for the same facts
# back. Phase C damages the end of the log the way a crash would (a torn final
# record, records that fail their check, a record in the old namespace log's
# format) and checks that recovery keeps every good record, cuts the damaged tail
# off, says so, and accepts new registrations afterwards. Phase D damages a record
# far from the end, which no crash can do, and checks the tracker refuses to start
# (D-074).
#
# History: written in Phase 0 as the regression test for defect R1 -- the
# coursework tracker replayed its log through its own authorisation guards, which
# refused every record, so nothing but the user list survived a restart -- and
# committed failing on purpose (D-006) until D-009 fixed it. Re-pointed at the
# namespace tracker in Phase 5 (D-060), and at the Raft log when the tracker became
# a replicated group (D-080: a single tracker is a group of one, with the group's
# log, raft_<port>.log). The property is the same: what the tracker acknowledged
# before the restart, it still knows after it.
set -uo pipefail
set +m

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="${TMPDIR:-/tmp}/p2p-persist-$$"
TRACKER_PORT="${TRACKER_PORT:-7101}"

reap () {
  pkill -9 -f "$ROOT/tracker $TRACKER_PORT" 2>/dev/null     # this port only, never machine-wide
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
log_path = os.path.join(work, f"raft_{port}.log")
fails = 0
def check(what, ok):
    global fails
    fails += 0 if ok else 1
    print(f"  {'PASS' if ok else 'FAIL'}  {what}", flush=True)

def start(tag):
    err = open(os.path.join(work, f"tracker-{tag}.err"), "w")
    p = subprocess.Popen([f"{root}/tracker", str(port), "--dir", work], stdout=subprocess.DEVNULL, stderr=err)
    for _ in range(100):
        if p.poll() is not None:
            return p
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

def record(body):                       # a log record, as raft_log.cpp writes it
    return body + " " + hashlib.sha1(body.encode()).hexdigest()[:8]

def registrations():
    return [l for l in open(log_path).read().splitlines() if l.startswith("ENTRY ") and " REGISTER " in l]

key = lambda i: hashlib.sha1(f"manifest {i}".encode()).hexdigest()
names = {f"report-{i}.pdf": key(i) for i in range(4)}

print("--- phase A: state built while the tracker is running ---")
t = start("a")
for n, k in names.items():
    check(f"REGISTER {n}", ask(f"REGISTER {n} {k}") == "OK")
check("a taken name is refused", ask(f"REGISTER report-0.pdf {key(99)}") == f"EXISTS {key(0)}")
check("the log holds an entry for each of the five registrations", len(registrations()) == 5)

print("--- SIGKILL, then restart on the same port and directory ---")
t.kill(); t.wait()
t = start("b")
print("--- phase B: the same facts, asked after the restart ---")
for n, k in names.items():
    check(f"RESOLVE {n} after the restart", ask(f"RESOLVE {n}") == f"FILE {k}")
check("still written once after the restart", ask(f"REGISTER report-1.pdf {key(98)}") == f"EXISTS {key(1)}")
check("LIST after the restart", ask("LIST") == "NAMES 4 " + " ".join(sorted(names)))

print("--- phase C: a crash's damaged tail ---")
t.kill(); t.wait()
good = open(log_path, "rb").read()
last = sum(1 for l in good.decode().splitlines() if l.startswith("ENTRY "))
term = max(int(l.split()[1]) for l in good.decode().splitlines() if l.startswith("TERM "))
with open(log_path, "ab") as f:
    cut = record(f"ENTRY {last + 1} {term} REGISTER cut.pdf {key(5)}")
    f.write(cut[:60].encode() + b"\n")                    # a record cut inside its key, newline intact
    flip = record(f"ENTRY {last + 1} {term} REGISTER flipped.pdf {key(8)}")
    flip = flip.replace(key(8), key(8)[:-1] + ("0" if key(8)[-1] != "0" else "1"))
    f.write(flip.encode() + b"\n")                        # one key character changed: only the check can tell
    f.write(record(f"ENTRY {last + 1} {term} REGISTER after-damage.pdf {key(9)}").encode() + b"\n")   # intact, after it
    old = f"REGISTER old.pdf {key(4)}"
    f.write((old + " " + hashlib.sha1(old.encode()).hexdigest()[:8]).encode() + b"\n")   # the old namespace log's format
    f.write(b"ENTRY 99 1 REGISTER torn.pdf " + key(6)[:20].encode())                     # torn: no newline
t = start("c")
report = open(os.path.join(work, "tracker-c.err")).read()
for n, k in names.items():
    check(f"{n} survives the damage", ask(f"RESOLVE {n}") == f"FILE {k}")
check("the cut, altered, old-format and torn records are not replayed -- nor the intact one after them",
      all(ask(f"RESOLVE {n}") == "MISSING" for n in ["cut.pdf", "flipped.pdf", "after-damage.pdf", "old.pdf", "torn.pdf"]))
check("recovery says what it cut off, and that intact-looking records were among it",
      "cut off" in report and "intact-looking" in report)
check("the damaged tail is gone from the file", open(log_path, "rb").read().startswith(good) and b"torn.pdf" not in open(log_path, "rb").read())
check("a registration after recovery succeeds", ask(f"REGISTER after.pdf {key(7)}") == "OK")
t.kill(); t.wait()
t = start("d")
check("and survives the next restart", ask("RESOLVE after.pdf") == f"FILE {key(7)}")
t.kill(); t.wait()

print("--- phase D: damage far from the end, which no crash can cause ---")
lines = open(log_path).read().splitlines()
entries = sum(1 for l in lines if l.startswith("ENTRY "))
term = max(int(l.split()[1]) for l in lines if l.startswith("TERM "))
with open(log_path, "a") as f:            # ~400 KiB of well-formed registrations after everything else
    for i in range(4000):
        entries += 1
        f.write(record(f"ENTRY {entries} {term} REGISTER bulk-{i:05d}.dat {key(1000 + i)}") + "\n")
data = open(log_path, "rb").read()
at = data.index(b" REGISTER report-2.pdf ")
damaged = data[:at + 30] + (b"0" if data[at + 30:at + 31] != b"0" else b"1") + data[at + 31:]
open(log_path, "wb").write(damaged)
t = start("e")
try:
    rc = t.wait(timeout=10)
except subprocess.TimeoutExpired:
    rc = None
    t.kill(); t.wait()
err = open(os.path.join(work, "tracker-e.err")).read()
check("the tracker refuses to start", rc not in (None, 0))
check("and says why: the damage is further back than a crash can reach", "refusing to start" in err)
check("the file is left as it was, for an operator to inspect", open(log_path, "rb").read() == damaged)

print("--- what the tracker reported on each start ---")
for tag in "abcde":
    print(f"  [{tag}] " + open(os.path.join(work, f"tracker-{tag}.err")).read().strip().replace("\n", "\n      "))
if fails == 0:
    print("\nPASS -- state survives a restart")
    sys.exit(0)
print(f"\nFAIL -- {fails} check(s) failed")
sys.exit(1)
PY
