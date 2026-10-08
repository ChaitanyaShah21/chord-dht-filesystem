#!/usr/bin/env bash
# e2e-transfer.sh -- whole files through the ring, on real processes (Phase 5).
#
# A tracker and a wired ring of 8 node processes x 4 virtual nodes, driven by the
# real `client`. Where every chunk SHOULD be is computed by the oracle in
# scripts/chunkcheck.py from the live addresses alone; where it IS is read off disk.
#
#   1. round trips: 0 bytes, 1 byte, 512 KiB + 1, 5 MB; every chunk and the manifest on
#      exactly the oracle's three processes
#   2. the manifest tree: fan-out 4 (two levels), fan-out 2 (four levels), and a file
#      that would need five levels, refused before its name is registered
#   3. names: written once -- the same file again succeeds, a different one is refused
#   4. two clients upload identical content at once: one copy of each chunk, three times
#   5. a corrupt copy on disk: the download falls through and still verifies
#   6. a node killed mid-download: the download completes from the other replicas
#   7. a chunk lost everywhere: the download fails, leaves no file and no part file,
#      and an older file at the destination is untouched (D-055)
#   8. the client killed (SIGKILL) and stopped (SIGTERM) mid-download
#   9. the tracker down: uploads fail cleanly; a download by key still works (D-057)
#
# NODE_CMD and CLIENT_CMD run other binaries, e.g. the ThreadSanitizer builds:
#   NODE_CMD="setarch $(uname -m) -R ./node-tsan" CLIENT_CMD="setarch $(uname -m) -R ./client-tsan" scripts/e2e-transfer.sh
set -uo pipefail
set +m

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="${TMPDIR:-/tmp}/p2p-transfer-$$"
BASE_PORT="${BASE_PORT:-7720}"
LAST_PORT=$((BASE_PORT + 9))           # 8 nodes, then the tracker at BASE_PORT + 9

# Kill only this suite's processes, matched by port -- never a machine-wide pkill.
reap() {
  local p i
  for p in $(seq "$BASE_PORT" "$LAST_PORT"); do
    pkill -CONT -f "node[-a-z]* 127.0.0.1 $p " 2>/dev/null
    pkill -9 -f "node[-a-z]* 127.0.0.1 $p " 2>/dev/null
    pkill -9 -f "tracker[-a-z]* $p " 2>/dev/null
  done
  for i in $(seq 40); do
    ss -ltn 2>/dev/null | grep -qE ":($(seq -s '|' "$BASE_PORT" "$LAST_PORT"))[[:space:]]" || return 0
    sleep 0.25
  done
  return 1
}
cleanup() { reap >/dev/null 2>&1; [[ -n "${KEEP_WORK:-}" ]] && echo "kept $WORK" || rm -rf "$WORK"; }
trap cleanup EXIT

for bin in node tracker client chunk; do
  [[ -x "$ROOT/$bin" ]] || { echo "FAIL: $ROOT/$bin not built. Run 'make' first."; exit 1; }
done
mkdir -p "$WORK"
reap || { echo "FAIL: ports $BASE_PORT-$LAST_PORT busy, cannot start"; exit 1; }

echo "--- e2e-transfer (real processes; ${NODE_CMD:-$ROOT/node}, ${CLIENT_CMD:-$ROOT/client}) ---"
cd "$ROOT"
python3 - "$ROOT" "$WORK" "$BASE_PORT" "${NODE_CMD:-$ROOT/node}" "${CLIENT_CMD:-$ROOT/client}" <<'PY'
import glob, hashlib, os, random, shlex, signal, socket, subprocess, sys, time
root, work, base, node_cmd, client_cmd = sys.argv[1], sys.argv[2], int(sys.argv[3]), shlex.split(sys.argv[4]), shlex.split(sys.argv[5])
sys.path.insert(0, os.path.join(root, "scripts"))
from chunkcheck import Placement, exchange, key_of

N, V, PERIOD, TIMEOUT = 8, 4, 200, 500
CHUNK = 512 * 1024
failures = 0
def check(ok, what):
    global failures
    failures += 0 if ok else 1
    print(f"  {'PASS' if ok else 'FAIL'}  {what}", flush=True)

addrs = [f"127.0.0.1:{base + i}" for i in range(N)]
tracker_port = base + 9
TRACKER = f"127.0.0.1:{tracker_port}"
RING = f"{addrs[0]},{addrs[1]}"           # the client's entry points: never killed below
members_file = os.path.join(work, "members.txt")
open(members_file, "w").write("".join(a + "\n" for a in addrs))
logs = os.path.join(work, "logs")
files = os.path.join(work, "files")
os.makedirs(logs)
os.makedirs(files)
procs = {}

def store_of(a):
    return os.path.join(work, "store-" + a.split(":")[1])

def start_node(a):
    ip, port = a.split(":")
    log = open(os.path.join(logs, f"node-{port}.log"), "a")
    procs[a] = subprocess.Popen(node_cmd + [ip, port, "--wired", members_file, "--vnodes", str(V), "--store", store_of(a),
                                            "--period", str(PERIOD), "--timeout", str(TIMEOUT)],
                                stdout=subprocess.DEVNULL, stderr=log)

def start_tracker():
    log = open(os.path.join(logs, "tracker.log"), "a")
    procs["tracker"] = subprocess.Popen([f"{root}/tracker", str(tracker_port), "--dir", work],
                                        stdout=subprocess.DEVNULL, stderr=log)

def kill(name):
    p = procs.pop(name)
    p.send_signal(signal.SIGCONT)
    p.kill()
    p.wait()

def listening(port):
    try:
        socket.create_connection(("127.0.0.1", port), timeout=0.5).close()
        return True
    except OSError:
        return False

def wait_until(pred, limit):
    deadline = time.monotonic() + limit
    while time.monotonic() < deadline:
        if pred():
            return True
        time.sleep(0.1)
    return False

client_errs = []      # every client's standard error, searched for sanitizer reports at the end

def client(*args, timeout=120):
    r = subprocess.run(client_cmd + ["--tracker", TRACKER, "--ring", RING] + list(args), capture_output=True, timeout=timeout)
    client_errs.append(r.stderr)
    return r

def path(name):
    return os.path.join(files, name)

def write(name, data):
    open(path(name), "wb").write(data)
    return path(name)

def read(name):
    with open(path(name), "rb") as f:
        return f.read()

def holders_on_disk(key, live):
    return sorted(a for a in live if os.path.exists(os.path.join(store_of(a), key[:2], key)))

def manifest_keys(root_key):
    """The data chunk keys of a one-level manifest, read with the raw chunk client."""
    reply, body = exchange(addrs[0], f"GET {root_key}")   # any process: GET is answered by whoever holds it
    if not reply.startswith("CHUNK"):
        holder = [a for a in addrs if a in procs and os.path.exists(os.path.join(store_of(a), root_key[:2], root_key))][0]
        reply, body = exchange(holder, f"GET {root_key}")
    lines = body.decode().split("\n")
    return [l.split()[0] for l in lines[5:] if l]

def part_files():
    return [f for f in os.listdir(files) if ".part-" in f]

def put_key(r):
    out = r.stdout.decode().split()
    return out[2] if r.returncode == 0 and len(out) > 2 else None

rng = random.Random(20261008)
def rand(n):
    return bytes(rng.randrange(256) for _ in range(n)) if n < 200_000 else os.urandom(n)

try:
    for a in addrs:
        start_node(a)
    start_tracker()
    check(wait_until(lambda: all(listening(base + i) for i in range(N)) and listening(tracker_port), 15),
          f"{N} node processes x {V} members and the tracker listen")
    time.sleep(PERIOD * 3 / 1000)
    live = list(addrs)
    placement = Placement(live, V)

    # ---- 1. round trips ----
    for label, n in [("zero", 0), ("one", 1), ("chunk+1", CHUNK + 1), ("5MB", 5_000_000)]:
        data = rand(n)
        r = client("--workers", "4", "put", write(label, data), label)
        g = client("--workers", "4", "get", label, path(label + ".out"))
        check(r.returncode == 0 and g.returncode == 0 and read(label + ".out") == data,
              f"{label}: {n} bytes up and back down, identical" + ("" if r.returncode == 0 else " -- " + r.stderr.decode()[-200:]))
    big_key = put_key(client("put", path("5MB"), "5MB"))       # same name, same file: written once, OK again
    keys = manifest_keys(big_key) + [big_key]
    check(len(keys) == 11, "the 5 MB file is 10 chunks and a manifest")
    check(wait_until(lambda: all(holders_on_disk(k, live) == sorted(placement.holders(k)) for k in keys), 10),
          "every chunk and the manifest are on exactly the oracle's three processes")

    # ---- 2. the manifest tree ----
    tree = rand(5_000_000)
    r = client("--fanout", "4", "put", write("tree4", tree), "tree4")
    i = client("info", "tree4")
    g = client("get", "tree4", path("tree4.out"))
    check(r.returncode == 0 and b" manifests 4 depth 2" in i.stdout and g.returncode == 0 and read("tree4.out") == tree,
          f"fan-out 4, ten chunks: three data manifests under an index, and back: {i.stdout.decode().strip()}")
    t2 = rand(4_500_000)
    r = client("--fanout", "2", "put", write("tree2", t2), "tree2")
    g = client("get", "tree2", path("tree2.out"))
    check(r.returncode == 0 and b"manifests 11 depth 4" in r.stdout and g.returncode == 0 and read("tree2.out") == t2,
          "fan-out 2, nine chunks: eleven manifests in four levels, and back")
    r = client("--fanout", "2", "put", write("tree5", rand(17 * CHUNK)), "tree5")
    res = client("resolve", "tree5")
    check(r.returncode != 0 and b"levels" in r.stderr and res.returncode != 0,
          "a file that would need five levels is refused, and its name never registered")

    # ---- 3. names are written once ----
    # "The same file" means the same manifest: the same bytes cut with the same chunk
    # size and fan-out. At the default fan-out this file has a different root key.
    r = client("--fanout", "4", "put", path("tree4"), "tree4")
    check(r.returncode == 0, "the same file under the same name again succeeds (a retried upload)")
    r = client("put", path("tree2"), "tree4")
    check(r.returncode != 0 and b"already registered" in r.stderr, "a different file under a taken name is refused")
    i = client("info", "tree4")
    check(b" manifests 4 depth 2" in i.stdout, "and the name still means the first file")
    ls = client("ls")
    check(ls.returncode == 0 and set(ls.stdout.decode().split()) >= {"zero", "one", "chunk+1", "5MB", "tree4", "tree2"},
          "ls lists the names")

    # ---- 4. identical content, uploaded twice at once ----
    same = rand(3_000_000)
    write("same", same)
    a1 = subprocess.Popen(client_cmd + ["--tracker", TRACKER, "--ring", RING, "put", path("same"), "same-a"], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    a2 = subprocess.Popen(client_cmd + ["--tracker", TRACKER, "--ring", RING, "put", path("same"), "same-b"], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    o1, _ = a1.communicate(timeout=60)
    o2, _ = a2.communicate(timeout=60)
    ka, kb = (o1.split()[2].decode() if a1.returncode == 0 else None), (o2.split()[2].decode() if a2.returncode == 0 else None)
    check(ka is not None and ka == kb, "two clients upload identical content at once: both succeed, one manifest key")
    if ka:
        ks = manifest_keys(ka) + [ka]
        check(wait_until(lambda: all(len(holders_on_disk(k, live)) == 3 for k in ks), 10),
              f"and each of its {len(ks)} chunks is stored on exactly three processes, once each")

    # ---- 5. a corrupt copy ----
    victim_key = manifest_keys(big_key)[3]
    owner = placement.holders(victim_key)[0]
    p = os.path.join(store_of(owner), victim_key[:2], victim_key)
    with open(p, "r+b") as f:
        f.seek(1000)
        b = f.read(1)
        f.seek(1000)
        f.write(bytes([b[0] ^ 0xFF]))
    g = client("get", "5MB", path("5MB.again"))
    check(g.returncode == 0 and read("5MB.again") == read("5MB"), "with one chunk's owner copy corrupted, the download still verifies")
    check(b"do not hash" in g.stderr, "and the client's note shows the corrupt copy was caught and passed over")

    # ---- 6. a node killed mid-download ----
    huge = os.urandom(60 * CHUNK)
    r = client("put", write("huge", huge), "huge")
    check(r.returncode == 0, "a 30 MiB file stored")
    # The victim owns an early chunk (one worker fetches them in order), so the client
    # reaches it milliseconds after the kill -- well before the ring can notice the
    # death (a 200 ms period) and route around it. A later chunk would make the
    # fall-through depend on timing. Never one of the client's entry points.
    hk = manifest_keys(put_key(r))
    first = next(i for i in range(1, len(hk)) if placement.holders(hk[i])[0] not in (addrs[0], addrs[1]))
    victim = placement.holders(hk[first])[0]
    dl = subprocess.Popen(client_cmd + ["--tracker", TRACKER, "--ring", RING, "--workers", "1", "get", "huge", path("huge.out")],
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    line = dl.stderr.readline()                         # "client: manifest ...": the chunks start now
    kill(victim)
    running_at_kill = dl.poll() is None
    out, err = dl.communicate(timeout=120)
    err = line + err
    client_errs.append(err)
    live = [a for a in addrs if a in procs]
    placement = Placement(live, V)
    check(dl.returncode == 0 and read("huge.out") == huge,
          f"{victim}, owner of chunk {first}, killed as the chunks began: the download completes, identical")
    # Who meets the dead node first is a race, and both outcomes are correct: the client
    # (its fetch is refused and falls through to the next replica, D-049) or the dead
    # node's neighbours (they evict it, and the client's lookup names the new owner, which
    # already holds a replica, D-035). Under load the neighbours sometimes win; asserting
    # the client's path made this check fail about one run in 25. What must hold either
    # way: the kill landed while the download was running, and the file came back whole.
    check(running_at_kill, f"the node died while the download was still running "
                           f"({err.count(b'refused')} fetches fell through past it; otherwise the ring had already moved on)")

    # ---- 7. a chunk lost everywhere ----
    small = rand(3 * CHUNK)
    k = put_key(client("put", write("doomed", small), "doomed"))
    lost = manifest_keys(k)[1]
    time.sleep(0.5)                                       # W = 2: let the third copy land first
    for a in live:
        f = os.path.join(store_of(a), lost[:2], lost)
        if os.path.exists(f):
            os.remove(f)
    write("keep", b"the previous version")
    before = sorted(os.listdir(files))
    g = client("get", "doomed", path("keep"), timeout=60)
    check(g.returncode != 0 and read("keep") == b"the previous version",
          "a chunk lost from every replica fails the download, and the older file at the destination is untouched")
    g = client("get", "doomed", path("doomed.out"), timeout=60)
    check(g.returncode != 0 and not os.path.exists(path("doomed.out")) and sorted(os.listdir(files)) == before and not part_files(),
          "and no file, no part file, is left behind")

    # ---- 8. the client killed mid-download ----
    for sig, expect in [(signal.SIGKILL, "a part file named as partial, and no file"), (signal.SIGTERM, "nothing at all")]:
        target = path(f"sig-{sig}.out")
        dl = subprocess.Popen(client_cmd + ["--tracker", TRACKER, "--ring", RING, "--workers", "1", "get", "huge", target],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        # Signal only once the part file exists, so the test is about what happens to
        # it -- the client prints its manifest line just before creating it.
        present = wait_until(lambda: any(f.startswith(f"sig-{sig}.out.part-") for f in part_files()), 10)
        dl.send_signal(sig)
        dl.wait()
        parts = [f for f in part_files() if f.startswith(f"sig-{sig}.out.part-")]
        if sig == signal.SIGKILL:
            check(present and not os.path.exists(target) and len(parts) == 1, f"SIGKILL mid-download leaves {expect}: {parts}")
            for f in parts:
                os.remove(os.path.join(files, f))
        else:
            check(present and dl.returncode == 128 + 15 and not os.path.exists(target) and not parts,
                  f"SIGTERM mid-download leaves {expect}: the handler removed the part file (exit {dl.returncode})")

    # ---- 9. the tracker down ----
    kill("tracker")
    r = client("put", path("one"), "while-down")
    check(r.returncode != 0 and b"could not be reached" in r.stderr, "with the tracker down, an upload fails and says why")
    g = subprocess.run(client_cmd + ["--ring", RING, "get-key", big_key, path("bykey.out")], capture_output=True, timeout=60)
    check(g.returncode == 0 and read("bykey.out") == read("5MB"), "but a download by manifest key needs no tracker")
    start_tracker()
    check(wait_until(lambda: listening(tracker_port), 10) and client("resolve", "5MB").stdout.decode().strip() == big_key,
          "the restarted tracker still knows every name")

    sanitizer = [f for f in os.listdir(logs) if "Sanitizer" in open(os.path.join(logs, f)).read()]
    sanitizer += ["a client" for e in client_errs if b"Sanitizer" in e]
    check(not sanitizer, f"no sanitizer output from any node, the tracker or a client {sanitizer}")
finally:
    for a in list(procs):
        kill(a)

print(f"== e2e-transfer: {'PASS -- files go through the ring and come back verified, or not at all' if failures == 0 else f'FAIL -- {failures} check(s) failed'}")
sys.exit(1 if failures else 0)
PY
status=$?
if [[ $status -ne 0 ]]; then
  for f in "$WORK"/logs/*.log; do [[ -f "$f" ]] && { echo "--- $f"; tail -4 "$f"; }; done
fi
exit $status
