#!/usr/bin/env bash
# e2e-persistence.sh -- does the tracker's state actually survive a restart?
#
# This is the regression test for defect R1 (docs/failures.md): the tracker writes
# an append-only command log and replays it at startup, but replays every command
# with an empty client_user, so each one is rejected by its own authorisation guard.
# The user table survives (create_user has no such guard); everything a user ever
# did is silently discarded.
#
# Committed RED on purpose, same as D-006. A test that has never failed has not
# been shown to test anything.
#
# Phase A builds state and proves it is live. The tracker is then killed and
# restarted on the same port, in the same directory, so it reads the same
# state_<port>.log. Phase B asks for the same four facts back.
set -uo pipefail
set +m

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="${TMPDIR:-/tmp}/p2p-persist-$$"
TRACKER_PORT="${TRACKER_PORT:-7101}"

reap () {
  pkill -9 -f "$ROOT/tracker" 2>/dev/null
  pkill -9 -f "$ROOT/client"  2>/dev/null
  pkill -9 -f 'tail -f -n [+]1' 2>/dev/null
  local i
  for i in $(seq 40); do
    ss -ltn 2>/dev/null | grep -qE ":(${TRACKER_PORT}|6883|6884)[[:space:]]" || return 0
    sleep 0.25
  done
  echo "WARN: ports still held after 10s:"
  ss -ltn | grep -E ":(${TRACKER_PORT}|6883|6884)[[:space:]]"
  return 1
}
cleanup() { reap >/dev/null 2>&1; rm -rf "$WORK"; }
trap cleanup EXIT

for b in tracker client; do
  [[ -x "$ROOT/$b" ]] || { echo "FAIL: $ROOT/$b not built. Run 'make' first."; exit 1; }
done

mkdir -p "$WORK/alice" "$WORK/bob"
cd "$WORK" || exit 1
reap || { echo "FAIL: ports busy, cannot start"; exit 1; }

head -c 4096 /dev/urandom > alice/testfile.bin

# ---------------------------------------------------------------- phase A
"$ROOT/tracker" "$TRACKER_PORT" >t1.out 2>t1.err & disown
sleep 1

: > a.in; : > b.in
tail -f -n +1 a.in > >("$ROOT/client" 127.0.0.1 "$TRACKER_PORT" 6883 >a1.out 2>a1.err) & disown
tail -f -n +1 b.in > >("$ROOT/client" 127.0.0.1 "$TRACKER_PORT" 6884 >b1.out 2>b1.err) & disown
sleep 2

printf 'create_user alice pw\nlogin alice pw\ncreate_group g1\n' >> a.in; sleep 2
printf 'create_user bob pw\nlogin bob pw\njoin_group g1\n'       >> b.in; sleep 2
printf 'list_requests g1\naccept_request g1 bob\n'               >> a.in; sleep 2
printf 'upload_file g1 alice/testfile.bin\n'                     >> a.in; sleep 3
printf 'list_groups\nlist_members g1\n'                          >> a.in; sleep 2

echo "--- phase A: state built while the tracker is running ---"
grep -qE '(^|[[:space:]])g1' a1.out && echo "  live: list_groups sees g1"     || echo "  live: list_groups MISSING g1 -- phase A itself broke"
grep -q  'bob'               a1.out && echo "  live: list_members sees bob"   || echo "  live: list_members MISSING bob -- phase A itself broke"
[[ -s "state_${TRACKER_PORT}.log" ]] && echo "  live: state_${TRACKER_PORT}.log written, $(wc -l < "state_${TRACKER_PORT}.log") records" \
                                     || echo "  live: NO LOG FILE WRITTEN"

# ---------------------------------------------------------------- restart
reap >/dev/null 2>&1
sleep 1
echo "--- restarting tracker on the same port, same directory ---"
"$ROOT/tracker" "$TRACKER_PORT" >t2.out 2>t2.err & disown
sleep 1

# ---------------------------------------------------------------- phase B
: > c.in
tail -f -n +1 c.in > >("$ROOT/client" 127.0.0.1 "$TRACKER_PORT" 6883 >a2.out 2>a2.err) & disown
sleep 2
printf 'create_user alice pw\nlogin alice pw\n'                      >> c.in; sleep 2
printf 'list_groups\nlist_members g1\n'                              >> c.in; sleep 2
printf 'get_file_info g1 alice/testfile.bin\n'                       >> c.in; sleep 2

fails=0
check () { # check <description> <pattern> <file>
  if grep -qE "$2" "$3"; then
    printf '  PASS  %s\n' "$1"
  else
    printf '  FAIL  %s\n' "$1"; fails=$((fails+1))
  fi
}

echo "--- phase B: the same four facts, asked after the restart ---"
check "create_user replayed  (alice already exists)" 'already exists'      a2.out
check "create_group replayed (list_groups shows g1)" '(^|[[:space:]])g1'   a2.out
check "accept_request replayed (g1 still has bob)"   'bob'                 a2.out
check "upload_file replayed  (manifest still known)" 'FILE_INFO'           a2.out

echo
echo "--- what the restarted tracker replayed (t2.err) ---"
sed -n '1,12p' t2.err
echo "--- the log it replayed from ---"
cat "state_${TRACKER_PORT}.log"
echo "--- phase B client transcript ---"
cat a2.out

if (( fails == 0 )); then echo; echo "PASS -- state survives a restart"; exit 0; fi
echo; echo "FAIL -- $fails/4 facts did not survive the restart (defect R1)"
exit 1
