#!/usr/bin/env bash
# e2e-edge.sh -- adversarial size sweep across the chunk boundary.
#
# The chunk size is 512*1024 = 524288. One 300 KB file exercises exactly one code
# path: a single short chunk. This sweeps the boundaries where off-by-one errors
# actually live -- zero bytes, one byte, exactly one chunk, one byte either side of
# a chunk, exactly two chunks, and a multi-chunk file -- uploading each through the
# ring and downloading it back, compared by SHA-1.
#
# History: written in Phase 0 against the coursework transfer path, where it found
# defects R3 (every reply one behind after the first download) and R4 (a zero-byte
# file could not be uploaded) and was committed failing on purpose (D-006). R3 was
# fixed in Phase 0. Phase 5 retired that path (D-060) and this sweep now runs
# through the ring; R4 is fixed there by design -- a zero-byte file is a manifest
# with no entries -- so all seven cases are expected to pass.
set -uo pipefail
set +m

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="${TMPDIR:-/tmp}/p2p-edge-$$"
export CLUSTER_DIR="$WORK/cluster" TRACKERS=1 TRACKER_PORT="${TRACKER_PORT:-7105}" NODE_BASE="${NODE_BASE:-7751}" VNODES=4 PERIOD=200

cleanup() { "$ROOT/scripts/cluster.sh" down >/dev/null 2>&1; rm -rf "$WORK"; }
trap cleanup EXIT

for b in tracker node client; do
  [[ -x "$ROOT/$b" ]] || { echo "FAIL: $ROOT/$b not built. Run 'make' first."; exit 1; }
done
mkdir -p "$WORK/in" "$WORK/out"
"$ROOT/scripts/cluster.sh" up 4 > "$WORK/up.out" 2>&1 || { echo "FAIL: cluster.sh up"; cat "$WORK/up.out"; exit 1; }

CASES=(
  "empty:0"               # zero chunks: a manifest with no entries
  "one_byte:1"            # one chunk, one byte
  "minus1:524287"         # one byte short of a full chunk
  "exact_1chunk:524288"   # exactly one chunk, no remainder
  "plus1:524289"          # one chunk plus one byte -> two chunks, the second 1 byte
  "exact_2chunk:1048576"  # exactly two chunks
  "multi:3000000"         # six chunks, the last one short
)
C=("$ROOT/client" --tracker "127.0.0.1:$TRACKER_PORT" --ring "127.0.0.1:$NODE_BASE" --quiet)

echo "--- e2e-edge (chunk size = 524288) ---"
printf '%-16s %10s %10s %7s  %s\n' CASE EXPECT_SZ GOT_SZ CHUNKS VERDICT
fail=0
for c in "${CASES[@]}"; do
  n="${c%%:*}"; b="${c##*:}"
  head -c "$b" /dev/urandom > "$WORK/in/$n.bin"
  "${C[@]}" put "$WORK/in/$n.bin" "$n" > "$WORK/out/$n.put" 2>&1
  "${C[@]}" get "$n" "$WORK/out/$n.out" > "$WORK/out/$n.get" 2>&1
  e=$(sha1sum "$WORK/in/$n.bin" | awk '{print $1}')
  g=$(sha1sum "$WORK/out/$n.out" 2>/dev/null | awk '{print $1}')
  gs=$(stat -c%s "$WORK/out/$n.out" 2>/dev/null || echo "-")
  chunks=$(awk '{for (i = 1; i < NF; i++) if ($i == "chunks") print $(i + 1)}' "$WORK/out/$n.get")
  expect_chunks=$(( (b + 524287) / 524288 ))
  if [[ -n "$g" && "$e" == "$g" && "$chunks" == "$expect_chunks" ]]; then v=PASS; else v=FAIL; fail=1; fi
  printf '%-16s %10s %10s %7s  %s\n' "$n" "$b" "$gs" "${chunks:--}" "$v"
  [[ $v == FAIL ]] && cat "$WORK/out/$n.put" "$WORK/out/$n.get"
done
echo
[[ $fail -ne 0 ]] && { echo "FAIL -- a size at or near a chunk boundary did not survive the round trip"; exit 1; }
echo "PASS -- every size across the chunk boundary round-trips, zero bytes included (R4 fixed)"
exit 0
