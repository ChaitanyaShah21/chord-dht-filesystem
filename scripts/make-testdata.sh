#!/usr/bin/env bash
#
# Regenerate the test corpus into testdata/.
#
# These files used to live in the repository root and were committed — 121 MB of
# random bytes that bloated every clone and told a reader nothing. They are
# generated here instead.
#
# The byte stream is DETERMINISTIC, not random: AES-256 in counter mode is run
# over an infinite stream of zeros with a fixed passphrase, so every machine
# produces byte-identical files. That matters because a throughput number is only
# comparable across runs if the input is the same, and a SHA-1 recorded in
# BENCHMARKS.md is only checkable if the file can be reproduced.
#
# /dev/urandom cannot do this — it has no seed you can pin.
#
# Usage:  ./scripts/make-testdata.sh [output_dir]     (default: testdata)

set -euo pipefail

OUT="${1:-testdata}"
SEED="chordfs-testdata-v1"
PIECE=$((512 * 1024))   # the current piece size; the edge cases below key off it

mkdir -p "$OUT"

# gen <name> <bytes> — deterministic pseudorandom bytes, same on every machine
gen() {
    local name="$1" bytes="$2"
    local path="$OUT/$name"
    if [ -f "$path" ] && [ "$(stat -c%s "$path")" = "$bytes" ]; then
        printf '  %-22s %10s bytes  (already present)\n' "$name" "$bytes"
        return
    fi
    # `|| true` on the openssl side is load-bearing: head exits as soon as it has
    # $bytes, openssl takes SIGPIPE on its next write, and `set -o pipefail` would
    # otherwise treat that normal shutdown as a failure and abort the script.
    { openssl enc -aes-256-ctr -pass "pass:$SEED-$name" -nosalt -in /dev/zero 2>/dev/null || true; } \
        | head -c "$bytes" > "$path"
    printf '  %-22s %10s bytes\n' "$name" "$bytes"
}

echo "Generating test corpus in $OUT/ (piece size ${PIECE} bytes)"
echo
echo "Throughput ladder — the baseline and scaling benchmarks read these:"
gen small.bin        102400        # 100 KB   — under one piece
gen medium.bin       1048576       # 1 MB     — 2 pieces
gen large.bin        5242880       # 5 MB     — 10 pieces
gen large.dat        10485760      # 10 MB    — 20 pieces
gen huge.iso         104857600     # 100 MB   — 200 pieces

echo
echo "Piece-boundary edge cases — the adversarial checks read these:"
gen edge-empty.bin   0                      # zero bytes: how many pieces is that?
gen edge-exact.bin   $((PIECE))             # exactly one piece, no remainder
gen edge-plus1.bin   $((PIECE + 1))         # one piece + a 1-byte tail
gen edge-minus1.bin  $((PIECE - 1))         # one byte short of a full piece

echo
echo "Manifest (record these in BENCHMARKS.md alongside any number taken from them):"
( cd "$OUT" && sha1sum -- *.bin *.dat *.iso 2>/dev/null | sort -k2 )
