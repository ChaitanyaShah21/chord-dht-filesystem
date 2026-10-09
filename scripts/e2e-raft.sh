#!/usr/bin/env bash
# e2e-raft.sh -- the replicated tracker on real processes (D-069 to D-082).
#
# Three tracker replicas on this machine, a one-node ring for the chunks, and the
# real client. Every failure is done to real processes:
#   1. the group elects exactly one leader; the client, given a follower first, is
#      redirected and registers a file; every replica agrees on the log
#   2. the leader is killed (SIGKILL): a new one is elected and writes continue
#   3. the killed replica restarts and catches up from its log and the leader
#   4. the minority leader: both followers frozen (SIGSTOP), the leader alone cannot
#      commit -- it answers UNAVAILABLE, never OK -- and steps down; thawed, the group
#      recovers and every replica holds the same names
#   5. the leader frozen: the other two elect a leader and carry on; thawed, the old
#      leader follows and catches up
#   6. a majority killed: a registration is refused, never acknowledged; restarted,
#      the group serves again
#   7. every replica killed and restarted: every acknowledged name is still there
#   8. a writer registering names continuously while the leader is killed: every
#      acknowledged name is found afterwards
#   9. a replica started with a different member list is refused
#
# Each replica is matched for cleanup by its own port, never a machine-wide pkill.
# TRACKER_CMD runs another tracker build (e.g. "setarch $(uname -m) -R ./tracker-tsan").
set -uo pipefail
set +m

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="${TMPDIR:-/tmp}/raft-e2e-$$"
BASE_PORT="${BASE_PORT:-7800}"
NODE_PORT=$((BASE_PORT + 5))

reap () {
  local p
  for p in $(seq "$BASE_PORT" $((BASE_PORT + 5))); do
    pkill -CONT -f "tracker[-a-z]* $p " 2>/dev/null
    pkill -9 -f "tracker[-a-z]* $p " 2>/dev/null
    pkill -9 -f "node 127.0.0.1 $p " 2>/dev/null
  done
}
cleanup() { reap; rm -rf "$WORK"; }
trap cleanup EXIT

for b in tracker node client; do [[ -x "$ROOT/$b" ]] || { echo "FAIL: $ROOT/$b not built. Run 'make' first."; exit 1; }; done
mkdir -p "$WORK"
reap

python3 - "$ROOT" "$WORK" "$BASE_PORT" "$NODE_PORT" "${TRACKER_CMD:-$ROOT/tracker}" <<'PY'
import hashlib, os, signal, socket, subprocess, sys, threading, time
root, work, base, node_port, tracker_cmd = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), sys.argv[5]
N, E = 3, 150
ports = [base + i for i in range(N)]
group = ",".join(f"127.0.0.1:{p}" for p in ports)
fails = 0
def check(what, ok):
    global fails
    fails += 0 if ok else 1
    print(f"  {'PASS' if ok else 'FAIL'}  {what}", flush=True)

procs = {}
def start(i, group_list=None):
    d = os.path.join(work, f"r{i}")
    os.makedirs(d, exist_ok=True)
    log = open(os.path.join(work, f"tracker-{i}.log"), "a")
    procs[i] = subprocess.Popen(tracker_cmd.split() + [str(ports[i]), "--dir", d, "--ip", "127.0.0.1",
                                "--group", group_list or group, "--id", str(i), "--election", str(E)],
                                stdout=subprocess.DEVNULL, stderr=log)
def kill(i):
    if i in procs and procs[i].poll() is None:
        procs[i].kill(); procs[i].wait()
def freeze(i): os.kill(procs[i].pid, signal.SIGSTOP)
def thaw(i): os.kill(procs[i].pid, signal.SIGCONT)
def alive(i): return i in procs and procs[i].poll() is None

def ask(i, line, timeout=3.0):
    try:
        with socket.create_connection(("127.0.0.1", ports[i]), timeout=timeout) as s:
            s.settimeout(timeout)
            s.sendall(line.encode() + b"\n")
            return s.makefile("rb").readline().decode().strip()
    except OSError as e:
        return f"NET {type(e).__name__}"

def status(i):
    w = ask(i, "STATUS", 1.0).split()
    return w if len(w) == 8 and w[0] == "STATUS" else None

def leader(exclude=(), within=5.0):
    """The replica that leads in the highest term, among those asked; None if none within `within` s."""
    end = time.monotonic() + within
    while time.monotonic() < end:
        best = None
        for i in range(N):
            if i in exclude or not alive(i): continue
            s = status(i)
            if s and s[2] == "leader" and (best is None or int(s[3]) > best[1]): best = (i, int(s[3]))
        if best: return best[0]
        time.sleep(0.02)
    return None

def converged(within=5.0):
    """Every live replica has applied as far as the leader has committed."""
    end = time.monotonic() + within
    while time.monotonic() < end:
        l = leader(within=1.0)
        if l is not None:
            ls = status(l)
            if all(status(i) and int(status(i)[6]) >= int(ls[5]) for i in range(N) if alive(i)): return True
        time.sleep(0.05)
    return False

def register(name, k, wait=8.0):
    """A REGISTER through the group, as the client does it: follow REDIRECT, retry UNAVAILABLE."""
    end = time.monotonic() + wait
    i = 0
    while time.monotonic() < end:
        r = ask(i % N, f"REGISTER {name} {k}", 3.0)
        if r.startswith("REDIRECT "):
            p = int(r.split(":")[-1]); i = ports.index(p) if p in ports else i + 1
            continue
        if r in ("OK",) or r.startswith("EXISTS"): return r
        i += 1
        time.sleep(0.02)
    return "GAVE UP"

key = lambda s: hashlib.sha1(s.encode()).hexdigest()
client = [f"{root}/client", "--tracker", group, "--ring", f"127.0.0.1:{node_port}", "--w", "1", "--quiet"]
def client_run(*args, wait_ms=10000, order=None):
    tl = ",".join(f"127.0.0.1:{ports[i]}" for i in order) if order else group
    cmd = [f"{root}/client", "--tracker", tl, "--tracker-wait", str(wait_ms), "--ring", f"127.0.0.1:{node_port}", "--w", "1", "--quiet"]
    return subprocess.run(cmd + list(args), capture_output=True, timeout=60)

acknowledged = {}    # every name the group said OK to, and its key

try:
    # A one-process ring holds the chunks; the tracker group is what is under test.
    node = subprocess.Popen([f"{root}/node", "127.0.0.1", str(node_port), "--create", "--vnodes", "2", "--period", "200",
                             "--store", os.path.join(work, "store")], stdout=subprocess.DEVNULL,
                            stderr=open(os.path.join(work, "node.log"), "w"))
    for i in range(N): start(i)

    print("--- 1. election, redirect, one log ---")
    l = leader(within=8.0)
    check("a leader is elected", l is not None)
    leaders = sum(1 for i in range(N) if (status(i) or [""] * 3)[2] == "leader")
    check("exactly one replica leads", leaders == 1)
    data = os.path.join(work, "f1.bin")
    open(data, "wb").write(os.urandom(300000))
    follower = (l + 1) % N
    time.sleep(3 * E / 1000)
    check("a follower answers a write with REDIRECT to the leader",
          ask(follower, f"REGISTER probe {key('p')}") == f"REDIRECT 127.0.0.1:{ports[l]}")
    r = client_run("put", data, "f1", order=[follower, (follower + 1) % N, (follower + 2) % N])
    check("the client, given a follower first, is redirected and registers the file", r.returncode == 0)
    k1 = r.stdout.decode().split()[2] if r.returncode == 0 else ""
    acknowledged["f1"] = k1
    check("the name resolves through the group", client_run("resolve", "f1").stdout.decode().strip() == k1)
    check("the log converges on every replica", converged())

    print("--- 2. the leader killed ---")
    t0 = time.monotonic()
    kill(l)
    l2 = leader(exclude=(l,), within=8.0)
    elected = time.monotonic() - t0
    check(f"a new leader is elected ({elected * 1000:.0f} ms after the kill)", l2 is not None and l2 != l)
    r = client_run("resolve", "f1")
    check("the client still resolves f1 (redirected past the dead replica)", r.stdout.decode().strip() == k1)
    open(os.path.join(work, "f2.bin"), "wb").write(os.urandom(100000))
    r = client_run("put", os.path.join(work, "f2.bin"), "f2")
    check("and a new file is registered with one replica down", r.returncode == 0)
    if r.returncode == 0: acknowledged["f2"] = r.stdout.decode().split()[2]

    print("--- 3. the killed replica restarts ---")
    start(l)
    check("it catches up with the leader", converged(8.0))
    check("and its log file holds f2's registration", "REGISTER f2 " in open(os.path.join(work, f"r{l}", f"raft_{ports[l]}.log")).read())

    print("--- 4. the minority leader: both followers frozen ---")
    l = leader()
    others = [i for i in range(N) if i != l]
    for i in others: freeze(i)
    t0 = time.monotonic()
    r = ask(l, f"REGISTER minority {key('m')}", 6.0)
    check(f"a write to the cut-off leader is not acknowledged: {r[:60]!r}", r != "OK" and not r.startswith("EXISTS"))
    stepped = False
    end = time.monotonic() + 5
    while time.monotonic() < end:
        s = status(l)
        if s and s[2] != "leader": stepped = True; break
        time.sleep(0.02)
    check(f"it has stepped down (check-quorum; seen {(time.monotonic() - t0) * 1000:.0f} ms after the freeze, the write having waited 2 s)", stepped)
    rd = ask(l, "RESOLVE f1", 4.0)
    check(f"and answers no read on its own: {rd[:50]!r}", rd.startswith("UNAVAILABLE") or rd.startswith("REDIRECT"))
    for i in others: thaw(i)
    l = leader(within=8.0)
    check("thawed, the group has a leader again", l is not None)
    check("and converges", converged(8.0))
    answer = ask(l, "RESOLVE minority")
    check(f"the unacknowledged write has one fate, whatever the log decided: {answer[:20]!r}",
          answer in ("MISSING", f"FILE {key('m')}"))

    print("--- 5. the leader frozen ---")
    l = leader()
    freeze(l)
    l2 = leader(exclude=(l,), within=8.0)
    check("the other two elect a leader", l2 is not None and l2 != l)
    check("and writes continue", register("frozen-time", key("ft")) == "OK")
    acknowledged["frozen-time"] = key("ft")
    thaw(l)
    time.sleep(4 * E / 1000)
    s = status(l)
    check("thawed, the old leader follows", s is not None and s[2] == "follower")
    check("and catches up", converged(8.0))

    print("--- 6. a majority killed ---")
    l = leader()
    survivor = l
    for i in range(N):
        if i != survivor: kill(i)
    r = ask(survivor, f"REGISTER doomed {key('d')}", 6.0)
    check(f"a write is not acknowledged with only one of three replicas: {r[:60]!r}", r != "OK")
    open(os.path.join(work, "f3.bin"), "wb").write(os.urandom(50000))
    r = client_run("put", os.path.join(work, "f3.bin"), "f3", wait_ms=2500)
    check("the client's upload fails and says the tracker could not be reached", r.returncode != 0 and b"could not be reached" in r.stderr)
    r = client_run("resolve", "f1", wait_ms=2500)
    check("a resolve fails too, rather than answer from one replica", r.returncode != 0)
    for i in range(N):
        if i != survivor: start(i)
    check("restarted, the group elects a leader", leader(within=8.0) is not None)
    check("and serves again", client_run("resolve", "f1").stdout.decode().strip() == k1)

    print("--- 7. every replica killed and restarted ---")
    for i in range(N): kill(i)
    for i in range(N): start(i)
    check("a leader after a full restart", leader(within=8.0) is not None)
    ok = all(client_run("resolve", n).stdout.decode().strip() == k for n, k in acknowledged.items())
    check(f"every one of the {len(acknowledged)} acknowledged names is still there", ok)

    print("--- 8. a writer running while the leader is killed ---")
    acked, stop = [], threading.Event()
    def writer():
        n = 0
        while not stop.is_set():
            name = f"w{n:05d}"
            if register(name, key(name), wait=5.0) == "OK": acked.append(name)
            n += 1
    th = threading.Thread(target=writer); th.start()
    time.sleep(1.0)
    before = len(acked)
    kill(leader())
    time.sleep(2.0)
    stop.set(); th.join()
    check(f"writes were acknowledged before and after the kill ({before} then {len(acked) - before})", before > 0 and len(acked) > before)
    l = leader(within=5.0)
    missing = [n for n in acked if ask(l, f"RESOLVE {n}") != f"FILE {key(n)}"]
    check(f"every one of {len(acked)} acknowledged names resolves after the failover", not missing)
    for i in range(N):
        if not alive(i): start(i)
    check("and the killed replica rejoins", converged(8.0))

    print("--- 9. a replica with a different member list ---")
    kill(2)
    wrong = ",".join([f"127.0.0.1:{ports[1]}", f"127.0.0.1:{ports[0]}", f"127.0.0.1:{ports[2]}"])
    start(2, wrong)
    time.sleep(1.5)
    log2 = open(os.path.join(work, "tracker-2.log")).read()
    check("its calls are refused, and it refuses theirs", "refusing calls from a replica with group" in log2)
    check("the other two still serve", client_run("resolve", "f1", order=[0, 1]).stdout.decode().strip() == k1)
    kill(2)
finally:
    for i in list(procs):
        try: thaw(i)
        except Exception: pass
        kill(i)
    node.kill(); node.wait()

sanitizer = [f for f in os.listdir(work) if f.startswith("tracker-") and "WARNING: ThreadSanitizer" in open(os.path.join(work, f)).read()]
check(f"no sanitizer report from any replica {sanitizer}", not sanitizer)
if fails == 0:
    print("\nPASS -- the replicated tracker elects, fails over, refuses without a majority, and keeps what it acknowledged")
    sys.exit(0)
print(f"\nFAIL -- {fails} check(s) failed")
for f in sorted(os.listdir(work)):
    if f.startswith("tracker-"):
        print(f"--- {f} (last 8) ---"); print("".join(open(os.path.join(work, f)).readlines()[-8:]))
sys.exit(1)
PY
