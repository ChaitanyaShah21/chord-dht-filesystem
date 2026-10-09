#!/usr/bin/env bash
# e2e-gc.sh -- deletion on real processes (D-089 to D-092).
#
# Three tracker replicas, four nodes with a collection key and an 8 s grace, the
# real client and the real collector. Every claim is checked from outside: chunk
# files are counted in the nodes' store directories, keys computed from the test's
# own files.
#   1. owners: only the upload's token deletes a name; another token, the owner hash,
#      and a name registered with no owner are all refused; no secret reaches a log
#   2. a chunk shared by two files survives deleting one of them; every chunk only
#      the deleted file used is gone from every store once the grace has passed; the
#      bytes freed match what the stores lost
#   3. nothing young is dropped: a collection straight after a delete drops nothing
#   4. the race: an upload that has stored its chunks but not yet registered survives
#      a collection, and so does one reusing old orphans of a deleted file
#   5. an upload slower than its limit does not register its name
#   6. the collector refuses: a name whose manifest is missing stops it before any
#      drop; settings that break the grace inequality skip every node; a DROP with the
#      wrong key is refused
#   7. a restarted node counts every chunk it holds as just written
#
# Processes are stopped by PID or matched by their own port, never a machine-wide pkill.
set -uo pipefail
set +m

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="${TMPDIR:-/tmp}/gc-e2e-$$"
BASE_PORT="${BASE_PORT:-7400}"

reap () {
  local p
  for p in $(seq "$BASE_PORT" $((BASE_PORT + 15))); do
    pkill -9 -f "tracker[-a-z]* $p " 2>/dev/null
    pkill -9 -f "node[-a-z]* 127.0.0.1 $p " 2>/dev/null
  done
}
cleanup() { reap; rm -rf "$WORK"; }
trap cleanup EXIT

for b in tracker node client gc; do [[ -x "$ROOT/$b" ]] || { echo "FAIL: $ROOT/$b not built. Run 'make' first."; exit 1; }; done
mkdir -p "$WORK"
reap

python3 - "$ROOT" "$WORK" "$BASE_PORT" "${NODE_CMD:-$ROOT/node}" <<'PY'
import hashlib, os, re, socket, subprocess, sys, threading, time
root, work, base, node_cmd = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
tports = [base + i for i in range(3)]
nports = [base + 10 + i for i in range(4)]
group = ",".join(f"127.0.0.1:{p}" for p in tports)
ring = f"127.0.0.1:{nports[0]},127.0.0.1:{nports[1]}"
GRACE, CHUNK = 8, 65536
fails = 0
def check(what, ok):
    global fails
    fails += 0 if ok else 1
    print(f"  {'PASS' if ok else 'FAIL'}  {what}", flush=True)

logs = os.path.join(work, "logs"); os.makedirs(logs)
keyfile = os.path.join(work, "gc.key")
with open(keyfile, "w") as f: f.write("5" * 40 + "\n")
procs = {}
def start_tracker(i):
    d = os.path.join(work, f"t{i}"); os.makedirs(d, exist_ok=True)
    procs[tports[i]] = subprocess.Popen([f"{root}/tracker", str(tports[i]), "--dir", d, "--ip", "127.0.0.1", "--group", group,
                                         "--id", str(i)], stdout=subprocess.DEVNULL,
                                        stderr=open(os.path.join(logs, f"tracker-{tports[i]}.log"), "a"))
def store_dir(p): return os.path.join(work, f"s{p}")
def start_node(i, join=True):
    p = nports[i]
    how = ["--join", f"127.0.0.1:{nports[0]}"] if join else ["--create"]
    procs[p] = subprocess.Popen(node_cmd.split() + ["127.0.0.1", str(p)] + how + ["--vnodes", "4", "--period", "200",
                                "--store", store_dir(p), "--scan", "0", "--gc-key", keyfile, "--gc-grace", str(GRACE)],
                                stdout=subprocess.DEVNULL, stderr=open(os.path.join(logs, f"node-{p}.log"), "a"))
def listening(port, timeout=15):
    t0 = time.time()
    while time.time() - t0 < timeout:
        try: socket.create_connection(("127.0.0.1", port), timeout=0.5).close(); return True
        except OSError: time.sleep(0.05)
    return False

def client(*args, extra=()):
    return subprocess.run([f"{root}/client", "--tracker", group, "--ring", ring, "--chunk-size", str(CHUNK),
                           "--tracker-wait", "2000", "--quiet"] + list(extra) + list(args), capture_output=True, timeout=120)
def gc(*extra):
    r = subprocess.run([f"{root}/gc", "--tracker", group, "--ring", ring, "--gc-key", keyfile, "--tracker-wait", "2000",
                        "--assume-upload-limit", "2", "--deadline", "2"] + list(extra), capture_output=True, timeout=300)
    out = r.stdout.decode()
    nums = dict(zip(out.split()[2::2], out.split()[3::2])) if out.startswith("gc ok") else {}
    return r, {k: int(v) for k, v in nums.items() if v.isdigit()}

def token_of(r):
    w = r.stdout.decode().split()
    return w[w.index("token") + 1] if "token" in w else None

def ask(port, line, timeout=3):
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as s:
        s.sendall(line.encode() + b"\n"); return s.makefile("rb").readline().decode().strip()
def leader():
    for p in tports:
        try:
            st = ask(p, "STATUS").split()
            if len(st) > 2 and st[2] == "leader": return p
        except OSError: pass
    return None

def chunk_keys(data):
    return [hashlib.sha1(data[i:i + CHUNK]).hexdigest() for i in range(0, len(data), CHUNK)] or [hashlib.sha1(b"").hexdigest()]
def holders(key):
    return sum(os.path.exists(os.path.join(store_dir(p), key[:2], key)) for p in nports)
def store_bytes():
    total = 0
    for p in nports:
        for dirpath, _, names in os.walk(store_dir(p)):
            if os.path.basename(dirpath) == "tmp": continue
            total += sum(os.path.getsize(os.path.join(dirpath, n)) for n in names)
    return total
def write(name, data):
    path = os.path.join(work, name)
    with open(path, "wb") as f: f.write(data)
    return path
def read(path):
    with open(path, "rb") as f: return f.read()

# ---- start --------------------------------------------------------------------------
for i in range(3): start_tracker(i)
start_node(0, join=False); listening(nports[0])
for i in range(1, 4): start_node(i); listening(nports[i])
t0 = time.time()
while leader() is None and time.time() - t0 < 10: time.sleep(0.1)
time.sleep(2.0)
check("three tracker replicas and four nodes with a collection key start", leader() is not None)

rnd = os.urandom
common = rnd(6 * CHUNK)
a = common + rnd(4 * CHUNK)                    # a: 6 shared chunks + 4 of its own
b = common + rnd(3 * CHUNK)                    # b: the same 6 + 3 of its own
c = rnd(5 * CHUNK)
ra, rb, rc = client("put", write("a", a), "a"), client("put", write("b", b), "b"), client("put", write("c", c), "c")
ta, tb, tc = token_of(ra), token_of(rb), token_of(rc)
t_put = time.time()
check("three uploads, each printing its own token", all(r.returncode == 0 for r in (ra, rb, rc)) and len({ta, tb, tc}) == 3
      and all(t and re.fullmatch(r"[0-9a-f]{32}", t) for t in (ta, tb, tc)))

print("-- 1. owners")
r = client("delete", "a", tb)
check("another file's token is refused, and says so", r.returncode == 1 and b"does not own" in r.stderr)
check("sending the owner hash instead of the secret is refused too",
      client("delete", "a", hashlib.sha1(ta.encode()).hexdigest()).returncode == 1)
ckey = rc.stdout.decode().split()[2]
check("a name registered with no owner (the original REGISTER) is accepted", ask(leader(), f"REGISTER legacy {ckey}") == "OK")
check("and can never be deleted, whatever the token", client("delete", "legacy", tc).returncode == 1 and
      ask(leader(), "RESOLVE legacy") == f"FILE {ckey}")
tokfile = write("token-a", (ta + "\n").encode())
r = client("delete", "a", extra=["--token-file", tokfile])
check("the upload's own token, from a file, deletes it", r.returncode == 0 and r.stdout.decode().strip() == "delete a ok")
ls = client("ls").stdout.decode().split()
check(f"and the name is gone ({sorted(ls)})", "a" not in ls and "b" in ls)
check("deleting it again: no such name", client("delete", "a", ta).returncode == 1)
secrets_logged = any(t in open(os.path.join(logs, f)).read() for f in os.listdir(logs) for t in (ta, tb, tc))
raft_logs = "".join(open(os.path.join(work, f"t{i}", f"raft_{tports[i]}.log")).read() for i in range(3))
check("no secret appears in any process's log or in any replica's Raft log; the owner hash does",
      not secrets_logged and ta not in raft_logs and hashlib.sha1(ta.encode()).hexdigest() in raft_logs)

print("-- 2 and 3. collecting")
only_a = [k for k in chunk_keys(a) if k not in set(chunk_keys(b))]
shared = [k for k in chunk_keys(a) if k in set(chunk_keys(b))]
check(f"a has {len(only_a)} chunks of its own and shares {len(shared)} with b, three copies each",
      len(only_a) == 4 and len(shared) == 6 and all(holders(k) == 3 for k in only_a + shared))
r, n = gc("--dry-run")
check(f"a dry run finds a's chunks and manifests as garbage and drops nothing ({n})", r.returncode == 0 and
      n.get("garbage", 0) >= 3 * len(only_a) and n.get("dropped") == 0 and all(holders(k) == 3 for k in only_a))
r, n = gc()
check(f"a collection within the grace drops nothing: every copy is young ({n.get('garbage')} garbage, {n.get('dropped')} dropped)",
      r.returncode == 0 and n.get("dropped") == 0 and all(holders(k) == 3 for k in only_a))
time.sleep(max(0.0, t_put + GRACE + 1 - time.time()))
before = store_bytes()
r, n = gc()
after = store_bytes()
check(f"after the grace: {n.get('dropped')} copies dropped, {n.get('bytes')} bytes freed", r.returncode == 0 and n.get("dropped", 0) > 0)
check("every chunk only a used is gone from every store", all(holders(k) == 0 for k in only_a))
check("every chunk a shared with b is still on three stores", all(holders(k) == 3 for k in shared))
check(f"the stores lost exactly the bytes the collector reports freeing ({before - after} = {n.get('bytes')})",
      before - after == n.get("bytes"))
check("that is at least three copies of a's own chunks", n.get("bytes", 0) >= 3 * 4 * CHUNK)
got_b, got_c = client("get", "b", os.path.join(work, "b.back")), client("get", "c", os.path.join(work, "c.back"))
check("b and c still download, byte for byte", got_b.returncode == 0 and got_c.returncode == 0 and
      read(os.path.join(work, "b.back")) == b and read(os.path.join(work, "c.back")) == c)
check("a is gone for good", client("get", "a", os.path.join(work, "a.back")).returncode == 1)
r, n = gc()
check(f"the next collection finds nothing more to drop ({n.get('garbage')} garbage)", r.returncode == 0 and n.get("garbage") == 0)

print("-- 4. the race with uploads")
d = rnd(5 * CHUNK)
bg = subprocess.Popen([f"{root}/client", "--tracker", group, "--ring", ring, "--chunk-size", str(CHUNK), "--tracker-wait", "2000",
                       "--quiet", "--register-delay", "3000", "put", write("d", d), "d"], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
t0 = time.time()
while not all(holders(k) >= 2 for k in chunk_keys(d)) and time.time() - t0 < 10: time.sleep(0.05)
r, n = gc()                                   # d's chunks are stored, its name not yet registered
mid = all(holders(k) >= 2 for k in chunk_keys(d))
out, err = bg.communicate(timeout=60)
check(f"an upload between storing and registering: the collection finds its chunks unnamed ({n.get('garbage')} garbage) "
      "and drops none of them", r.returncode == 0 and n.get("garbage", 0) > 0 and mid)
check("and once it registers, the file downloads whole", bg.returncode == 0 and
      client("get", "d", os.path.join(work, "d.back")).returncode == 0 and read(os.path.join(work, "d.back")) == d)

e = rnd(4 * CHUNK)
re1 = client("put", write("e", e), "e")
check("e uploaded and deleted", re1.returncode == 0 and client("delete", "e", token_of(re1)).returncode == 0)
time.sleep(GRACE + 1)                         # e's chunks are now old orphans
check("e's chunks are still there, old, unnamed", all(holders(k) == 3 for k in chunk_keys(e)))
bg = subprocess.Popen([f"{root}/client", "--tracker", group, "--ring", ring, "--chunk-size", str(CHUNK), "--tracker-wait", "2000",
                       "--quiet", "--register-delay", "3000", "put", write("e2", e), "e2"], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
time.sleep(1.5)                               # its chunks re-sent (already held: renewed); its name not yet registered
r, n = gc()
kept = all(holders(k) == 3 for k in chunk_keys(e))
out, err = bg.communicate(timeout=60)
check(f"the same content uploaded again while a collection runs: its old chunks were renewed by the upload and kept "
      f"({n.get('garbage')} garbage listed, {n.get('dropped')} dropped)", r.returncode == 0 and kept)
check("and e2 downloads whole", bg.returncode == 0 and client("get", "e2", os.path.join(work, "e2.back")).returncode == 0 and
      read(os.path.join(work, "e2.back")) == e)

print("-- 5. the upload limit")
r = client("put", write("f", rnd(2 * CHUNK)), "f", extra=["--upload-limit", "1", "--register-delay", "1500"])
check("an upload slower than its limit fails and says why", r.returncode == 1 and b"longer than its limit" in r.stderr)
check("and its name was not registered", client("resolve", "f").returncode == 1)

print("-- 6. the collector refuses")
r, n = gc("--assume-upload-limit", "100")
check(f"settings that break the grace inequality skip every node ({n.get('skipped')} of {n.get('nodes')})",
      r.returncode == 0 and n.get("skipped") == 4 and n.get("dropped") == 0 and b"shorter than" in r.stderr)
secret = "9" * 32
missing = hashlib.sha1(b"a manifest nobody stored").hexdigest()
ask(leader(), f"REGISTER broken {missing} {hashlib.sha1(secret.encode()).hexdigest()}")
gg = rnd(3 * CHUNK)
rg = client("put", write("g", gg), "g"); client("delete", "g", token_of(rg))
time.sleep(GRACE + 1)
r, n = gc()
check("a name whose manifest cannot be read stops the collection before anything is dropped, naming the name",
      r.returncode == 1 and b"broken" in r.stderr and all(holders(k) == 3 for k in chunk_keys(gg)))
check("its owner deletes it", client("delete", "broken", secret).returncode == 0)
garbage_key = chunk_keys(gg)[0]
check("a DROP with the wrong key is refused by the node", ask(nports[0], f"DROP {'6' * 40} {garbage_key}") == "ERR wrong collection key")
check("a DROP with no key at all is not a request", ask(nports[0], f"DROP {garbage_key}").startswith("ERR"))

print("-- 7. a restart counts as a write")
victim = nports[2]
held_by_victim = [k for k in chunk_keys(gg) if os.path.exists(os.path.join(store_dir(victim), k[:2], k))]
t_kill = time.time()
procs[victim].kill(); procs[victim].wait()
start_node(2); listening(victim)
time.sleep(1.0)
r, n = gc()
victim_line = [l for l in r.stderr.decode().splitlines() if f":{victim} " in l]
check(f"a node restarted after its chunks aged keeps them all: young since the restart ({victim_line[0][4:] if victim_line else '?'})",
      r.returncode == 0 and held_by_victim and all(os.path.exists(os.path.join(store_dir(victim), k[:2], k)) for k in held_by_victim))
# The others drop their old copies. Any copy still on them was written after the kill:
# repair re-replicated the dead node's chunks (D-050), and a copy written then is young.
stale = [(p, k) for p in nports if p != victim for k in chunk_keys(gg)
         if os.path.exists(os.path.join(store_dir(p), k[:2], k)) and os.path.getmtime(os.path.join(store_dir(p), k[:2], k)) < t_kill]
check(f"while the others drop every copy older than the restart ({len(stale)} left); any copy left there was "
      "re-replicated after the kill, and is young", not stale)

print(f"e2e-gc: {'PASS' if fails == 0 else 'FAIL'} -- {fails} failed")
sys.exit(1 if fails else 0)
PY
rc=$?
if [[ $rc -eq 0 ]]; then echo "PASS -- only an owner deletes a name; only unused, old chunks are collected; uploads in flight survive"; else echo "FAIL"; fi
exit $rc
