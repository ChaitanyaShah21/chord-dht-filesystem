#!/usr/bin/env bash
# e2e-smoke.sh -- the happy path, verified by hash.
#
# Starts a tracker and two clients, uploads one file from alice, downloads it as
# bob, and compares SHA-1 end to end. Exits non-zero on any mismatch.
#
# This is the regression test for defect R2 (docs/failures.md): before the fix it
# produced a full-size file of zeros and the hash comparison below caught it.
set -uo pipefail
set +m   # no job-control "Killed" notices when cleanup() reaps the background processes

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="${TMPDIR:-/tmp}/p2p-smoke-$$"
TRACKER_PORT="${TRACKER_PORT:-7100}"
SIZE="${SIZE:-300000}"

# Reap every process this suite starts, and wait until the ports are actually free.
# Killing by PID was not enough: the clients run inside process substitution, so
# killing the `tail` that feeds them leaves the client alive holding 6881/6882. The
# next run then dies with a bind error that looks exactly like a product bug.
reap () {
  # Match the port too, not just the binary: a bare "$ROOT/tracker" pattern
  # kills every tracker on the machine, including one another suite is using.
  # That is error-log entry E6 -- two green suites, run together, both red.
  pkill -9 -f "$ROOT/tracker $TRACKER_PORT" 2>/dev/null
  pkill -9 -f "$ROOT/client 127.0.0.1 $TRACKER_PORT" 2>/dev/null
  pkill -9 -f "tail -f -n [+]1 ._${TRACKER_PORT}.in" 2>/dev/null
  local i
  for i in $(seq 40); do
    ss -ltn 2>/dev/null | grep -qE ":(${TRACKER_PORT}|6881|6882)[[:space:]]" || return 0
    sleep 0.25
  done
  echo "WARN: ports still held after 10s:"
  ss -ltn | grep -E ":(${TRACKER_PORT}|6881|6882)[[:space:]]"
  return 1
}

cleanup() {
  reap >/dev/null 2>&1
  rm -rf "$WORK"
}
trap cleanup EXIT

for b in tracker client; do
  [[ -x "$ROOT/$b" ]] || { echo "FAIL: $ROOT/$b not built. Run 'make' first."; exit 1; }
done

mkdir -p "$WORK/alice" "$WORK/bob"
cd "$WORK" || exit 1
reap || { echo "FAIL: ports busy, cannot start"; exit 1; }

"$ROOT/tracker" "$TRACKER_PORT" >tracker.out 2>tracker.err & TPID=$!; disown
sleep 1

head -c "$SIZE" /dev/urandom > alice/testfile.bin
EXPECT=$(sha1sum alice/testfile.bin | awk '{print $1}')

: > a_${TRACKER_PORT}.in; : > b_${TRACKER_PORT}.in
tail -f -n +1 a_${TRACKER_PORT}.in > >("$ROOT/client" 127.0.0.1 "$TRACKER_PORT" 6881 >alice.out 2>alice.err) & ATL=$!; disown
tail -f -n +1 b_${TRACKER_PORT}.in > >("$ROOT/client" 127.0.0.1 "$TRACKER_PORT" 6882 >bob.out   2>bob.err)   & BTL=$!; disown
sleep 2

printf 'create_user alice pw\nlogin alice pw\ncreate_group g1\n'   >> a_${TRACKER_PORT}.in; sleep 2
printf 'create_user bob pw\nlogin bob pw\njoin_group g1\n'          >> b_${TRACKER_PORT}.in; sleep 2
printf 'list_requests g1\naccept_request g1 bob\n'                  >> a_${TRACKER_PORT}.in; sleep 2
printf 'upload_file g1 alice/testfile.bin\n'                        >> a_${TRACKER_PORT}.in; sleep 3
printf 'download_file g1 alice/testfile.bin bob/got.bin\n'          >> b_${TRACKER_PORT}.in; sleep 8

GOT=$(sha1sum bob/got.bin 2>/dev/null | awk '{print $1}')
GOTSZ=$(stat -c%s bob/got.bin 2>/dev/null || echo "-")

echo "--- e2e-smoke ---"
echo "size     : $SIZE bytes -> got $GOTSZ"
echo "expected : $EXPECT"
echo "got      : ${GOT:-<no file>}"

if [[ "$EXPECT" == "$GOT" ]]; then
  echo "PASS"
  exit 0
fi
echo "FAIL -- transferred bytes do not match the source"
echo "--- bob.err ---";     cat bob.err
echo "--- alice.err ---";   cat alice.err
echo "--- tracker.err ---"; cat tracker.err
exit 1
