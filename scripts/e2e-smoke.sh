#!/usr/bin/env bash
# e2e-smoke.sh -- the happy path, verified by hash.
#
# Starts a three-replica tracker group and a five-node ring with scripts/cluster.sh, stores a file
# with the client, fetches it back under its name, and compares SHA-1 end to end.
# Exits non-zero on any mismatch. Also the test of cluster.sh itself (D-064):
# `up` must give a working cluster and `down` must stop every process it started.
#
# History: written in Phase 0 against the coursework tracker and peers, as the
# regression test for defect R2 (a download that produced a full-size file of
# zeros). Re-pointed at the ring in Phase 5 when that code was retired (D-060);
# the property it guards -- the bytes that come back are the bytes that went in,
# checked by hash -- is the same.
set -uo pipefail
set +m

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="${TMPDIR:-/tmp}/p2p-smoke-$$"
SIZE="${SIZE:-300000}"
export CLUSTER_DIR="$WORK/cluster" TRACKERS=3 TRACKER_PORT="${TRACKER_PORT:-7100}" NODE_BASE="${NODE_BASE:-7741}" VNODES=4 PERIOD=200

cleanup() { "$ROOT/scripts/cluster.sh" down >/dev/null 2>&1; rm -rf "$WORK"; }
trap cleanup EXIT

for b in tracker node client; do
  [[ -x "$ROOT/$b" ]] || { echo "FAIL: $ROOT/$b not built. Run 'make' first."; exit 1; }
done
mkdir -p "$WORK"
"$ROOT/scripts/cluster.sh" up 5 > "$WORK/up.out" 2>&1 || { echo "FAIL: cluster.sh up"; cat "$WORK/up.out"; exit 1; }

head -c "$SIZE" /dev/urandom > "$WORK/testfile.bin"
EXPECT=$(sha1sum "$WORK/testfile.bin" | awk '{print $1}')
TRACKER_LIST="127.0.0.1:$TRACKER_PORT,127.0.0.1:$((TRACKER_PORT + 1)),127.0.0.1:$((TRACKER_PORT + 2))"
C=("$ROOT/client" --tracker "$TRACKER_LIST" --ring "127.0.0.1:$NODE_BASE")
"${C[@]}" put "$WORK/testfile.bin" smoke.bin > "$WORK/put.out" 2> "$WORK/put.err"
"${C[@]}" get smoke.bin "$WORK/got.bin" > "$WORK/get.out" 2> "$WORK/get.err"
LISTED=$("${C[@]}" ls 2>/dev/null)

GOT=$(sha1sum "$WORK/got.bin" 2>/dev/null | awk '{print $1}')
GOTSZ=$(stat -c%s "$WORK/got.bin" 2>/dev/null || echo "-")

echo "--- e2e-smoke ---"
echo "size     : $SIZE bytes -> got $GOTSZ"
echo "expected : $EXPECT"
echo "got      : ${GOT:-<no file>}"
echo "listed   : $LISTED"

"$ROOT/scripts/cluster.sh" down > "$WORK/down.out" 2>&1
LEFT=0
for p in $(seq "$TRACKER_PORT" $((TRACKER_PORT + 2))) $(seq "$NODE_BASE" $((NODE_BASE + 4))); do
  ss -ltn 2>/dev/null | grep -qE ":$p[[:space:]]" && LEFT=1
done

if [[ "$EXPECT" == "$GOT" && "$LISTED" == "smoke.bin" && $LEFT -eq 0 ]]; then
  echo "PASS"
  exit 0
fi
[[ $LEFT -ne 0 ]] && echo "FAIL -- cluster.sh down left processes listening"
[[ "$EXPECT" != "$GOT" ]] && echo "FAIL -- the bytes that came back are not the bytes that went in"
echo "--- put ---"; cat "$WORK/put.out" "$WORK/put.err"
echo "--- get ---"; cat "$WORK/get.out" "$WORK/get.err"
for f in "$CLUSTER_DIR"/logs/*.log; do echo "--- $f"; tail -3 "$f"; done
exit 1
