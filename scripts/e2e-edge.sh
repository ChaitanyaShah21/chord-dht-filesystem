#!/usr/bin/env bash
# e2e-edge.sh -- adversarial size sweep across the piece boundary (R10).
#
# PIECE_SIZE is 512*1024 = 524288. The happy path with one 300 KB file exercises
# exactly one code path: a single short piece. This sweeps the boundaries where
# off-by-one errors actually live -- zero bytes, one byte, exactly one piece, one
# byte either side of a piece, exactly two pieces, and a multi-piece file.
#
# THIS SCRIPT IS EXPECTED TO FAIL until defects R3 and R4 are closed.
# See docs/failures.md. A failing test that pins a known open defect is worth more
# than a passing test that avoids it.
#
# The signature of R3 is visible in the GOT_SZ column: every row receives the
# PREVIOUS row's size. That one-row shift is the diagnosis, not the sizes.
set -uo pipefail
set +m   # no job-control "Killed" notices when cleanup() reaps the background processes

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="${TMPDIR:-/tmp}/p2p-edge-$$"
TRACKER_PORT="${TRACKER_PORT:-7100}"

# Reap every process this suite starts, and wait until the ports are actually free.
# Killing by PID was not enough: the clients run inside process substitution, so
# killing the `tail` that feeds them leaves the client alive holding 6881/6882. The
# next run then dies with a bind error that looks exactly like a product bug.
reap () {
  pkill -9 -f "$ROOT/tracker" 2>/dev/null
  pkill -9 -f "$ROOT/client"  2>/dev/null
  pkill -9 -f 'tail -f -n [+]1' 2>/dev/null
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

CASES=(
  "empty:0"               # zero pieces. ceil(0/PIECE_SIZE) == 0
  "one_byte:1"            # one piece, one byte
  "minus1:524287"         # one byte short of a full piece
  "exact_1piece:524288"   # exactly one piece, no remainder
  "plus1:524289"          # one piece plus one byte -> two pieces, second is 1 byte
  "exact_2piece:1048576"  # exactly two pieces
  "multi:3000000"         # six pieces, last one short
)

mkdir -p "$WORK/alice" "$WORK/bob"
cd "$WORK" || exit 1
reap || { echo "FAIL: ports busy, cannot start"; exit 1; }

"$ROOT/tracker" "$TRACKER_PORT" >tracker.out 2>tracker.err & TPID=$!; disown
sleep 1

for c in "${CASES[@]}"; do
  head -c "${c##*:}" /dev/urandom > "alice/${c%%:*}.bin"
done

: > a.in; : > b.in
tail -f -n +1 a.in > >("$ROOT/client" 127.0.0.1 "$TRACKER_PORT" 6881 >alice.out 2>alice.err) & ATL=$!; disown
tail -f -n +1 b.in > >("$ROOT/client" 127.0.0.1 "$TRACKER_PORT" 6882 >bob.out   2>bob.err)   & BTL=$!; disown
sleep 2

printf 'create_user alice pw\nlogin alice pw\ncreate_group g1\n' >> a.in; sleep 2
printf 'create_user bob pw\nlogin bob pw\njoin_group g1\n'        >> b.in; sleep 2
printf 'list_requests g1\naccept_request g1 bob\n'                >> a.in; sleep 2

for c in "${CASES[@]}"; do printf 'upload_file g1 alice/%s.bin\n' "${c%%:*}" >> a.in; done
sleep 4
for c in "${CASES[@]}"; do
  printf 'download_file g1 alice/%s.bin bob/%s.out\n' "${c%%:*}" "${c%%:*}" >> b.in
  sleep 2
done
sleep 6

echo "--- e2e-edge (PIECE_SIZE = 524288) ---"
printf '%-16s %10s %10s  %s\n' CASE EXPECT_SZ GOT_SZ VERDICT
fail=0
for c in "${CASES[@]}"; do
  n="${c%%:*}"; b="${c##*:}"
  e=$(sha1sum "alice/$n.bin" 2>/dev/null | awk '{print $1}')
  g=$(sha1sum "bob/$n.out"   2>/dev/null | awk '{print $1}')
  gs=$(stat -c%s "bob/$n.out" 2>/dev/null || echo "-")
  if [[ -n "$g" && "$e" == "$g" ]]; then v=PASS; else v=FAIL; fail=1; fi
  printf '%-16s %10s %10s  %s\n' "$n" "$b" "$gs" "$v"
done
echo
echo "--- bob.err ---"; cat bob.err
[[ $fail -ne 0 ]] && echo "
EXPECTED FAILURE while R3/R4 are open -- see docs/failures.md"
exit $fail
