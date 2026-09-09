#!/usr/bin/env bash
# bench-baseline.sh -- §1 of BENCHMARKS.md: how fast is the inherited system
# before anything is optimised?
#
# This number cannot be recovered later. Once the transfer layer is rewritten in
# Phase 5 there is no way back to "what it was before", so every claim of
# improvement is measured against what this prints.
#
# Method, stated because a number without one is not a measurement:
#   * one tracker, one seeding peer, one downloading peer, all on this host
#   * timing starts immediately before the download command is written to the
#     driving pipe and stops when the downloading client reports the transfer
#     complete, after which the SHA-1 is verified and a mismatched run is
#     discarded rather than reported -- the time to produce a wrong file is not
#     a throughput
#   * completion is NOT detected by watching the destination file's size. The
#     client preallocates the destination with ftruncate before fetching a
#     single byte, so it is full-size immediately -- that is defect R2b, and a
#     first version of this harness fell straight into it and reported 165 MB/s
#     for the time to preallocate 1 MB. Size is never evidence of completeness
#   * every repetition gets a completely fresh tracker and pair of peers. Without
#     that, the downloader announces itself as a seeder on completion and the
#     next repetition can serve itself from its own copy
#   * completion is detected by polling the destination size every 20 ms, so
#     each figure carries up to 20 ms of granularity -- negligible at seconds,
#     stated because it is not negligible at the 1 MB size
set -uo pipefail
set +m

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TRACKER_PORT="${TRACKER_PORT:-7104}"
A_PORT=6887
B_PORT=6888
REPS="${REPS:-3}"
CASES="${CASES:-medium.bin large.dat huge.iso}"
POLL=0.02

reap () {
  pkill -9 -f "$ROOT/tracker $TRACKER_PORT" 2>/dev/null
  pkill -9 -f "$ROOT/client 127.0.0.1 $TRACKER_PORT" 2>/dev/null
  pkill -9 -f "tail -f -n [+]1 ._${TRACKER_PORT}.in" 2>/dev/null
  local i
  for i in $(seq 40); do
    ss -ltn 2>/dev/null | grep -qE ":(${TRACKER_PORT}|${A_PORT}|${B_PORT})[[:space:]]" || return 0
    sleep 0.25
  done
  return 1
}
WORK=""
cleanup() { reap >/dev/null 2>&1; [[ -n "$WORK" ]] && rm -rf "$WORK"; }
trap cleanup EXIT

for b in tracker client; do
  [[ -x "$ROOT/$b" ]] || { echo "FAIL: $ROOT/$b not built. Run 'make' first."; exit 1; }
done
[[ -d "$ROOT/testdata" ]] || { echo "FAIL: no testdata/. Run scripts/make-testdata.sh"; exit 1; }

COMMIT="$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
DIRTY=""
git -C "$ROOT" diff --quiet 2>/dev/null || DIRTY=" (WORKING TREE DIRTY -- this number is not reproducible)"

echo "--- bench-baseline ---"
echo "commit    : ${COMMIT}${DIRTY}"
echo "host      : $(uname -m), $(nproc) cores, kernel $(uname -r)"
echo "piece size: 512 KB (inherited)"
echo "reps      : $REPS per size, fresh processes each time"
echo

# One repetition: bring everything up, upload, time one download, tear down.
# Prints "<seconds> <ok|hashfail|timeout>".
one_run () {
  local file="$1" bytes="$2" expect="$3"
  WORK="${TMPDIR:-/tmp}/p2p-bench-$$-$RANDOM"
  mkdir -p "$WORK/alice" "$WORK/bob"; cd "$WORK" || return 1
  cp "$ROOT/testdata/$file" "alice/$file"

  reap || { echo "0 portsbusy"; return 1; }
  "$ROOT/tracker" "$TRACKER_PORT" >tracker.out 2>tracker.err & disown
  sleep 1

  : > "a_${TRACKER_PORT}.in"; : > "b_${TRACKER_PORT}.in"
  tail -f -n +1 "a_${TRACKER_PORT}.in" > >("$ROOT/client" 127.0.0.1 "$TRACKER_PORT" $A_PORT >a.out 2>a.err) & disown
  tail -f -n +1 "b_${TRACKER_PORT}.in" > >("$ROOT/client" 127.0.0.1 "$TRACKER_PORT" $B_PORT >b.out 2>b.err) & disown
  sleep 2

  printf 'create_user alice pw\nlogin alice pw\ncreate_group g1\n' >> "a_${TRACKER_PORT}.in"; sleep 2
  printf 'create_user bob pw\nlogin bob pw\njoin_group g1\n'       >> "b_${TRACKER_PORT}.in"; sleep 2
  printf 'list_requests g1\naccept_request g1 bob\n'               >> "a_${TRACKER_PORT}.in"; sleep 2
  printf 'upload_file g1 alice/%s\n' "$file"                       >> "a_${TRACKER_PORT}.in"

  # Wait for the manifest to be hashed rather than sleeping a guessed amount:
  # 100 MB of SHA-1 is not instant and guessing wrong either wastes time or
  # times the hashing as though it were transfer.
  local i
  for i in $(seq 600); do
    grep -qi "upload" a.out a.err 2>/dev/null && break
    sleep 0.1
  done
  sleep 1

  local t0 t1 sz
  t0=$(date +%s.%N)
  printf 'download_file g1 alice/%s bob/got.bin\n' "$file" >> "b_${TRACKER_PORT}.in"

  local waited=0
  while :; do
    # The client prints this only after download_file_multipeer returns with
    # every piece verified. Watching the file size instead would time ftruncate.
    grep -q "Download completed" b.err 2>/dev/null && break
    sleep "$POLL"
    waited=$(echo "$waited + $POLL" | bc)
    if (( $(echo "$waited > 300" | bc) )); then echo "0 timeout"; return 1; fi
  done
  t1=$(date +%s.%N)
  sz=$(stat -c%s bob/got.bin 2>/dev/null || echo 0)
  [[ "$sz" == "$bytes" ]] || { echo "0 shortfile"; return 1; }

  local got; got=$(sha1sum bob/got.bin | awk '{print $1}')
  if [[ "$got" != "$expect" ]]; then echo "0 hashfail"; return 1; fi
  echo "$(echo "$t1 - $t0" | bc) ok"
}

printf "%-12s %10s %8s   %s\n" "FILE" "BYTES" "RUN" "SECONDS   MB/s"
for file in $CASES; do
  bytes=$(stat -c%s "$ROOT/testdata/$file")
  expect=$(sha1sum "$ROOT/testdata/$file" | awk '{print $1}')
  times=()
  for r in $(seq "$REPS"); do
    read -r secs status <<<"$(one_run "$file" "$bytes" "$expect")"
    cd "$ROOT"; rm -rf "$WORK"; WORK=""
    if [[ "$status" != "ok" ]]; then
      printf "%-12s %10s %8s   %s\n" "$file" "$bytes" "$r" "FAILED ($status)"
      continue
    fi
    mbps=$(echo "scale=2; ($bytes / 1048576) / $secs" | bc)
    printf "%-12s %10s %8s   %-9s %s\n" "$file" "$bytes" "$r" "$(printf '%.3f' "$secs")" "$mbps"
    times+=("$secs")
  done
  if [[ ${#times[@]} -gt 0 ]]; then
    median=$(printf '%s\n' "${times[@]}" | sort -n | awk '{a[NR]=$1} END{print (NR%2)?a[(NR+1)/2]:(a[NR/2]+a[NR/2+1])/2}')
    medmb=$(echo "scale=2; ($bytes / 1048576) / $median" | bc)
    printf "%-12s %10s %8s   %-9s %s\n" "$file" "$bytes" "MEDIAN" "$(printf '%.3f' "$median")" "$medmb"
  fi
  echo
done
echo "Record these in BENCHMARKS.md §1 against commit ${COMMIT}."
