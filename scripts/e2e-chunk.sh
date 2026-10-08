#!/usr/bin/env bash
# e2e-chunk.sh -- the chunk plane on real node processes (Phase 4).
#
# A wired ring of 6 processes x 4 virtual nodes, each with its own chunk store.
# Every check is made from outside: where a chunk SHOULD be is computed by the
# oracle in scripts/chunkcheck.py from the live addresses alone (D-048), and
# where it IS is asked with HAVE or read off the disk.
#
#   1. put and get through the `chunk` client; the copies land on exactly the
#      oracle's three processes, all different; a duplicate put adds nothing
#   2. sizes: zero bytes, exactly 512 KiB, 512 KiB + 1 (refused before allocating)
#   3. hostile requests: bytes that do not hash to their key, path-traversal keys,
#      a client that dies mid-body, a request for a member that does not exist,
#      more concurrent bodies than the cap
#   4. a corrupt copy on disk: the read falls through and still verifies
#   5. two of three replica processes killed: the read is still served at once
#   6. re-replication: every chunk back on its three correct live processes
#   7. the scrub: a copy corrupted on a live process is replaced by a good one
#
# NODE_CMD runs a different node binary, e.g. the ThreadSanitizer build:
#   NODE_CMD="setarch $(uname -m) -R ./node-tsan" scripts/e2e-chunk.sh
set -uo pipefail
set +m

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="${TMPDIR:-/tmp}/p2p-chunk-$$"
BASE_PORT="${BASE_PORT:-7700}"
LAST_PORT=$((BASE_PORT + 9))

# Kill only this suite's nodes, matched by port -- never a machine-wide pkill
# (error log E6). The pattern lives in this file, not on a shell's command line.
reap() {
  local p i
  for p in $(seq "$BASE_PORT" "$LAST_PORT"); do
    pkill -CONT -f "node[-a-z]* 127.0.0.1 $p " 2>/dev/null
    pkill -9 -f "node[-a-z]* 127.0.0.1 $p " 2>/dev/null
  done
  for i in $(seq 40); do
    ss -ltn 2>/dev/null | grep -qE ":($(seq -s '|' "$BASE_PORT" "$LAST_PORT"))[[:space:]]" || return 0
    sleep 0.25
  done
  return 1
}
cleanup() { reap >/dev/null 2>&1; [[ -n "${KEEP_WORK:-}" ]] && echo "kept $WORK" || rm -rf "$WORK"; }
trap cleanup EXIT

for bin in node chunk; do
  [[ -x "$ROOT/$bin" ]] || { echo "FAIL: $ROOT/$bin not built. Run 'make' first."; exit 1; }
done
mkdir -p "$WORK"
reap || { echo "FAIL: ports $BASE_PORT-$LAST_PORT busy, cannot start"; exit 1; }

echo "--- e2e-chunk (real processes; ${NODE_CMD:-$ROOT/node}) ---"
cd "$ROOT"
python3 - "$ROOT" "$WORK" "$BASE_PORT" "${NODE_CMD:-$ROOT/node}" <<'PY'
import hashlib, os, random, shlex, signal, socket, subprocess, sys, time
root, work, base, node_cmd = sys.argv[1], sys.argv[2], int(sys.argv[3]), shlex.split(sys.argv[4])
sys.path.insert(0, os.path.join(root, "scripts"))
from chunkcheck import Placement, exchange, have, key_of, ring_correct, under_replicated

N, V, PERIOD, TIMEOUT, SCAN = 6, 4, 200, 500, 5
failures = 0
def check(ok, what):
    global failures
    failures += 0 if ok else 1
    print(f"  {'PASS' if ok else 'FAIL'}  {what}", flush=True)

addrs = [f"127.0.0.1:{base + i}" for i in range(N)]
members_file = os.path.join(work, "members.txt")
open(members_file, "w").write("".join(a + "\n" for a in addrs))
procs, logs = {}, os.path.join(work, "logs")
os.makedirs(logs)

def store_of(a):
    return os.path.join(work, "store-" + a.split(":")[1])

def start(a):
    ip, port = a.split(":")
    log = open(os.path.join(logs, f"node-{port}.log"), "a")
    procs[a] = subprocess.Popen(node_cmd + [ip, port, "--wired", members_file, "--vnodes", str(V),
                                            "--store", store_of(a), "--period", str(PERIOD),
                                            "--timeout", str(TIMEOUT), "--scan", str(SCAN)],
                                stdout=subprocess.DEVNULL, stderr=log)
    log.close()

def kill(a):
    p = procs.pop(a)
    p.send_signal(signal.SIGCONT)
    p.kill()
    p.wait()

def wait_until(pred, limit):
    deadline = time.monotonic() + limit
    while time.monotonic() < deadline:
        if pred():
            return True
        time.sleep(0.1)
    return False

def listening(a):
    try:
        socket.create_connection(tuple([a.split(":")[0], int(a.split(":")[1])]), timeout=0.5).close()
        return True
    except OSError:
        return False

def chunk(*args, data=b""):
    return subprocess.run([f"{root}/chunk"] + list(args), input=data, capture_output=True, timeout=60)

def disk_copies(key, live):
    return [a for a in live if os.path.exists(os.path.join(store_of(a), key[:2], key))]

rng = random.Random(20261008)
try:
    for a in addrs:
        start(a)
    check(wait_until(lambda: all(listening(a) for a in addrs), 15), f"{N} processes x {V} members listen")
    live = list(addrs)
    placement = Placement(live, V)
    check(wait_until(lambda: ring_correct(placement), 30), "the wired ring of 24 members is correct (vnode oracle)")

    # ---- 1. put and get ----
    data = bytes(rng.randrange(256) for _ in range(300_000))
    key = key_of(data)
    r = chunk("put", addrs[0], data=data)
    out = r.stdout.decode().split()
    check(r.returncode == 0 and out[:3] == ["put", key, "ok"] and int(out[3]) >= 2,
          f"chunk put: acknowledged with {out[3] if len(out) > 3 else '?'} copies (W = 2)")
    holders = placement.holders(key)
    check(len(set(holders)) == 3, f"the oracle's replica set is three distinct processes: {holders}")
    check(wait_until(lambda: sorted(disk_copies(key, live)) == sorted(holders), 5),
          "the copies on disk are exactly on the oracle's three processes, and nowhere else")
    r = chunk("get", addrs[3], key)
    check(r.returncode == 0 and r.stdout == data, "chunk get from another member returns the same bytes")
    r = chunk("put", addrs[5], data=data)
    check(r.returncode == 0 and sorted(disk_copies(key, live)) == sorted(holders),
          "a duplicate put succeeds and adds no copy (content addressing)")

    # ---- 2. sizes ----
    r = chunk("put", addrs[1], data=b"")
    empty = "da39a3ee5e6b4b0d3255bfef95601890afd80709"
    check(r.returncode == 0 and r.stdout.decode().split()[1] == empty, "a zero-byte chunk is stored under SHA-1 of nothing")
    r = chunk("get", addrs[2], empty)
    check(r.returncode == 0 and r.stdout == b"", "and read back as zero bytes")
    big = bytes(rng.randrange(256) for _ in range(512 * 1024))
    r = chunk("put", addrs[2], data=big)
    check(r.returncode == 0, "a chunk of exactly 512 KiB is stored")
    r = chunk("get", addrs[4], key_of(big))
    check(r.returncode == 0 and r.stdout == big, "and read back byte for byte")
    r = chunk("put", addrs[2], data=big + b"x")
    check(r.returncode != 0 and b"larger than 524288" in r.stderr, "the client refuses 512 KiB + 1 itself")
    reply, _ = exchange(addrs[0], f"STORE {key_of(big + b'x')} {512 * 1024 + 1}")
    check(reply.startswith("ERR chunk larger"), f"a node refuses a 512 KiB + 1 header before reading a byte: {reply}")

    # ---- 3. hostile requests ----
    other = b"not what the key says"
    reply, _ = exchange(addrs[0], f"PUT {key_of(b'something else')} 2 {len(other)}", other)
    check(reply.startswith("ERR") and "hash" in reply, f"bytes that do not hash to their key are refused: {reply}")
    check(not disk_copies(key_of(b"something else"), live), "and stored nowhere")
    sentinel = os.path.join(work, "outside")
    for bad in ["../../../../../../../../../../../../../../../../tmp/x", "../" * 13 + "outside",
                "DA39A3EE5E6B4B0D3255BFEF95601890AFD80709", "da39a3ee5e6b4b0d3255bfef95601890afd8070"]:
        reply, _ = exchange(addrs[1], f"GET {bad}")
        check(reply.startswith("ERR key"), f"GET {bad[:24]}... is refused: {reply}")
        reply, _ = exchange(addrs[1], f"STORE {bad} 3", b"abc")
        check(reply.startswith("ERR"), f"STORE {bad[:24]}... is refused: {reply}")
    check(not os.path.exists(sentinel) and not os.path.exists("/tmp/x"), "nothing was written outside the stores")

    half = bytes(rng.randrange(256) for _ in range(1000))
    s = socket.create_connection(("127.0.0.1", base + 3), timeout=5)
    s.sendall(f"STORE {key_of(half)} 1000\n".encode() + half[:400])
    s.close()                                          # dies mid-body
    time.sleep(0.3)
    reply, _ = exchange(addrs[3], "PING")
    check(reply.startswith("PONG"), "a client that dies mid-body does not hurt the node")
    check(not disk_copies(key_of(half), live) and not os.listdir(os.path.join(store_of(addrs[3]), "tmp")),
          "and nothing of the half-sent chunk is stored, not even a temporary file")

    reply, _ = exchange(addrs[2], "TO 0123456789abcdef PING")
    check(reply.startswith("ERR no ring member"), f"a request for a member the process does not have: {reply}")

    held = []
    for i in range(32):                                # 32 bodies announced and never sent
        c = socket.create_connection(("127.0.0.1", base + 4), timeout=5)
        c.sendall(f"STORE {'a' * 40} 524288\n".encode())
        held.append(c)
    time.sleep(0.3)
    reply, _ = exchange(addrs[4], f"STORE {'b' * 40} 10", b"0123456789")
    check(reply == "ERR busy", f"the 33rd concurrent body is refused before it is read: {reply}")
    for c in held:
        c.close()
    time.sleep(0.3)
    small = b"after the burst"
    reply, _ = exchange(addrs[4], f"STORE {key_of(small)} {len(small)}", small)
    check(reply == "OK", "once the 32 are gone the slots are free again")

    # ---- 4. a corrupt copy ----
    keys = []
    for i in range(40):
        d = bytes(rng.randrange(256) for _ in range(20_000 + i))
        r = chunk("put", addrs[i % N], data=d)
        if r.returncode == 0:
            keys.append(key_of(d))
    check(len(keys) == 40, "40 more chunks written")
    victim_key = keys[0]
    owner = placement.holders(victim_key)[0]
    path = os.path.join(store_of(owner), victim_key[:2], victim_key)
    with open(path, "r+b") as f:
        f.seek(100)
        b = f.read(1)
        f.seek(100)
        f.write(bytes([b[0] ^ 0xFF]))
    r = chunk("get", addrs[0], victim_key)
    check(r.returncode == 0 and key_of(r.stdout) == victim_key,
          "with the owner's copy corrupted, the read falls through and the bytes still verify")
    check(b"do not hash" in r.stderr, "and the client's record shows the owner's copy failed the hash")
    def owner_copy_good():
        try:
            with open(path, "rb") as f:
                return key_of(f.read()) == victim_key
        except OSError:
            return False
    check(wait_until(owner_copy_good, SCAN * PERIOD / 1000 * 4 + 5),
          "the owner's corrupt copy is found and replaced with a verified one")

    # ---- 5. kill two of three replicas ----
    k2 = keys[1]
    h2 = placement.holders(k2)
    survivors_needed = [a for a in h2[2:]]
    for a in h2[:2]:
        kill(a)
    starts = [a for a in addrs if a in procs]
    r = chunk("get", starts[0], k2)
    check(r.returncode == 0 and key_of(r.stdout) == k2,
          "with the owner and the second replica killed, the read is served by the third at once")
    live = [a for a in addrs if a in procs]
    placement = Placement(live, V)
    check(wait_until(lambda: ring_correct(placement), 20), "the ring repairs itself around the two dead processes")

    # ---- 6. re-replication ----
    all_keys = [key, empty, key_of(big)] + keys
    t0 = time.monotonic()
    ok = wait_until(lambda: not under_replicated(placement, all_keys), 30)
    check(ok, f"every chunk is back on its three correct live processes ({time.monotonic() - t0:.1f} s after the ring)")
    if not ok:
        for k in sorted(under_replicated(placement, all_keys))[:5]:
            print(f"        {k}: should be on {placement.holders(k)}, HAVE says "
                  f"{[(a, have(a, [k])) for a in live]}")

    # ---- 7. the scrub ----
    k3 = keys[5]
    h3 = Placement(live, V).holders(k3)
    p3 = os.path.join(store_of(h3[1]), k3[:2], k3)
    with open(p3, "r+b") as f:
        f.write(b"rot")
    def restored():
        try:
            with open(p3, "rb") as f:
                return key_of(f.read()) == k3
        except OSError:                                # removed by the scrub, not yet replaced
            return False
    check(wait_until(restored, SCAN * PERIOD / 1000 * 4 + 5), "a copy corrupted on a live process is replaced by a good one")

    sanitizer = [f for f in os.listdir(logs) if "Sanitizer" in open(os.path.join(logs, f)).read()]
    check(not sanitizer, "no sanitizer output in any node's log")
finally:
    for a in list(procs):
        kill(a)

print(f"== e2e-chunk: {'PASS -- chunks are stored, replicated, verified and repaired' if failures == 0 else f'FAIL -- {failures} check(s) failed'}")
sys.exit(1 if failures else 0)
PY
status=$?
if [[ $status -ne 0 ]]; then
  for f in "$WORK"/logs/node-*.log; do [[ -f "$f" ]] && { echo "--- $f"; tail -4 "$f"; }; done
fi
exit $status
