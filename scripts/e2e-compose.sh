#!/usr/bin/env bash
# e2e-compose.sh -- the container deployment, end to end (D-064, D-088).
#
# Builds the image and starts compose.yaml under its own project name: three tracker
# replicas, five nodes, Prometheus and Grafana, each container at its fixed address.
#   1. the stack starts: every process is scraped by Prometheus, exactly one replica
#      leads, and the ring is correct by the oracle in chunkcheck.py, which is computed
#      from the live addresses alone and reached from the host over the bridge network
#   2. Grafana has its data source and the dashboard provisioned
#   3. a file goes in through the client container and comes back with the same SHA-1;
#      then every query on the dashboard returns at least one series
#   4. the tracker leader and one node are killed: the file still comes back whole, a
#      new file is acknowledged by the new leader, and every chunk is back on its three
#      correct live processes
#   5. the killed containers rejoin, then the whole stack is stopped and started again
#      on its volumes: both names survive and both files come back whole
# Prints the client's own timings, including resolution while the first-listed tracker
# address belongs to a dead container (a vanished address answers nothing; see
# BENCHMARKS.md §12). Linux only: the oracle needs a route to the container network.
#
# The stack is always taken down with its volumes, by project name, on exit.
set -uo pipefail
set +m

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROJECT="${PROJECT:-chordfs-e2e}"
WORK="$ROOT/cluster/compose-e2e-$$"          # under the repository, which the client container mounts as /work
DC=(docker compose -f "$ROOT/compose.yaml" -p "$PROJECT")

cleanup() { "${DC[@]}" down -v --remove-orphans >/dev/null 2>&1; rm -rf "$WORK"; }
trap cleanup EXIT

command -v docker >/dev/null || { echo "FAIL: docker is not installed"; exit 1; }
if docker network ls --format '{{.Name}}' | grep -q '_ring$' && ! docker network ls --format '{{.Name}}' | grep -qx "${PROJECT}_ring"; then
  echo "FAIL: another compose project already holds the ring network's subnet (172.28.0.0/24);"
  echo "      take it down first (docker compose down)"; exit 1
fi
mkdir -p "$WORK"
echo "--- e2e-compose: building and starting the stack ---"
if ! "${DC[@]}" up -d --build > "$WORK/up.out" 2>&1; then
  echo "FAIL: docker compose up"; tail -30 "$WORK/up.out"; exit 1
fi

python3 - "$ROOT" "$WORK" "$PROJECT" <<'PY'
import hashlib, json, os, re, subprocess, sys, time, urllib.parse, urllib.request
root, work, project = sys.argv[1], sys.argv[2], sys.argv[3]
sys.path.insert(0, os.path.join(root, "scripts"))
from chunkcheck import Placement, ring_correct, under_replicated

DC = ["docker", "compose", "-f", os.path.join(root, "compose.yaml"), "-p", project]
NODES = {f"node{i}": f"172.28.0.{10 + i}:7001" for i in range(1, 6)}
TRACKERS = {"tracker1": "172.28.0.10", "tracker2": "172.28.0.20", "tracker3": "172.28.0.30"}
VNODES = 8
fails = 0

def check(what, ok):
    global fails
    fails += 0 if ok else 1
    print(f"  {'PASS' if ok else 'FAIL'}  {what}", flush=True)
    return ok

def dc(*args, timeout=600):
    return subprocess.run(DC + list(args), capture_output=True, text=True, timeout=timeout)

def wait_until(pred, limit, step=0.5):
    end = time.monotonic() + limit
    while time.monotonic() < end:
        try:
            if pred():
                return True
        except Exception:
            pass
        time.sleep(step)
    return False

def prom(query):
    """One instant query, asked from inside the Prometheus container (it publishes no port)."""
    url = "http://localhost:9090/api/v1/query?query=" + urllib.parse.quote(query)
    r = dc("exec", "-T", "prometheus", "wget", "-qO-", url, timeout=30)
    return json.loads(r.stdout)["data"]["result"]

def grafana(path):
    with urllib.request.urlopen("http://localhost:3000" + path, timeout=5) as resp:
        return resp.status, json.loads(resp.read() or b"null")

def client(*args):
    """The client container, as this user so that its files can be removed."""
    r = dc("run", "--rm", "--user", f"{os.getuid()}:{os.getgid()}", "client", *args, timeout=300)
    line = next((l for l in r.stdout.splitlines() if l.split()[:1] in (["put"], ["get"])), "")
    fields = line.split()
    # "put <name> <key> ok bytes <n> chunks <n> ... total_ms <ms> [token <t>]": pairs from field 4
    stats = {fields[i]: fields[i + 1] for i in range(4, len(fields) - 1, 2)} if " ok " in line else {}
    return r, line, stats

def sha1(path):
    with open(path, "rb") as f:
        return hashlib.sha1(f.read()).hexdigest()

def held_keys(services):
    keys = set()
    for s in services:
        r = dc("exec", "-T", s, "find", "/data", "-type", "f", "-printf", "%f\n", timeout=30)
        keys |= {k for k in r.stdout.split() if re.fullmatch(r"[0-9a-f]{40}", k)}
    return keys

def leader():
    res = prom('chordfs_raft_role{role="leader"} == 1')
    ips = [r["metric"]["instance"].split(":")[0] for r in res]
    return [s for s, ip in TRACKERS.items() if ip in ips]

live = dict(NODES)
print("1. the stack starts", flush=True)
check("all eight processes are scraped and up",
      wait_until(lambda: len([r for r in prom("up == 1")]) == 8, 90))
check("exactly one tracker replica leads", wait_until(lambda: len(leader()) == 1, 30))
check("the ring is correct by the oracle (5 processes x 8 members)",
      wait_until(lambda: ring_correct(Placement(list(live.values()), VNODES)), 60))

print("2. Grafana is provisioned", flush=True)
check("Grafana is healthy", wait_until(lambda: grafana("/api/health")[0] == 200, 60))
_, ds = grafana("/api/datasources")
check("the Prometheus data source points at the Prometheus container",
      any(d.get("url") == "http://172.28.0.40:9090" for d in ds or []))
_, found = grafana("/api/search")
check("the dashboard is provisioned", any(d.get("uid") == "chordfs" for d in found or []))

print("3. a file through the containers", flush=True)
first = os.path.join(work, "first.bin")
with open(first, "wb") as f:
    f.write(os.urandom(20 * 1024 * 1024 + 12345))          # 40 full chunks and a short one
rel = lambda p: "/work/" + os.path.relpath(p, root)
r, line, put1 = client("put", rel(first), "first")
check(f"put is acknowledged ({put1.get('total_ms', '?')} ms)", bool(put1))
r, line, get1 = client("get", "first", rel(first + ".got"))
check(f"get returns the same SHA-1 ({get1.get('total_ms', '?')} ms)",
      bool(get1) and os.path.exists(first + ".got") and sha1(first + ".got") == sha1(first))
time.sleep(6)                                               # three scrapes after the traffic
with open(os.path.join(root, "deploy/grafana/dashboards/chordfs.json")) as f:
    panels = json.load(f)["panels"]
exprs = [t["expr"] for p in panels for t in p.get("targets", [])]
empty = [e for e in exprs if not prom(e.replace("$__rate_interval", "1m"))]
check(f"every dashboard query returns data ({len(exprs) - len(empty)} of {len(exprs)})", not empty)
for e in empty:
    print(f"        empty: {e}")

print("4. the leader and a node are killed", flush=True)
lead = leader()[0]
dc("kill", lead, "node3")
del live["node3"]
print(f"        killed {lead} (the leader) and node3", flush=True)
r, line, get2 = client("get", "first", rel(first + ".got2"))
check(f"the file still comes back whole (resolve {get2.get('resolve_ms', '?')} ms, total {get2.get('total_ms', '?')} ms)",
      bool(get2) and sha1(first + ".got2") == sha1(first))
second = os.path.join(work, "second.bin")
with open(second, "wb") as f:
    f.write(os.urandom(5 * 1024 * 1024))
r, line, put2 = client("put", rel(second), "second")
check(f"a new file is acknowledged by the new leader ({put2.get('total_ms', '?')} ms)", bool(put2))
check("a different replica leads", wait_until(lambda: leader() and leader()[0] != lead, 10))
placement = Placement(list(live.values()), VNODES)
check("the ring repairs itself around the dead node", wait_until(lambda: ring_correct(placement), 30))
keys = held_keys(live)
check(f"every chunk ({len(keys)}) is back on its three correct live processes",
      len(keys) > 0 and wait_until(lambda: not under_replicated(placement, keys), 30, 1.0))

print("5. rejoin, then a full restart on the volumes", flush=True)
dc("start", lead, "node3")
live = dict(NODES)
check("the killed node rejoins: the ring of five is correct again",
      wait_until(lambda: ring_correct(Placement(list(live.values()), VNODES)), 60))
dc("stop", timeout=120)
dc("up", "-d", timeout=300)
check("after a full restart, one replica leads", wait_until(lambda: len(leader()) == 1, 60))
check("after a full restart, the ring is correct",
      wait_until(lambda: ring_correct(Placement(list(live.values()), VNODES)), 60))
r, _, _ = client("ls")
check("both names survive", sorted(r.stdout.split()) == ["first", "second"])
r, line, get3 = client("get", "second", rel(second + ".got"))
check("the second file comes back whole", bool(get3) and sha1(second + ".got") == sha1(second))
r, line, get4 = client("get", "first", rel(first + ".got3"))
check("the first file comes back whole", bool(get4) and sha1(first + ".got3") == sha1(first))

print("PASS" if fails == 0 else f"FAIL -- {fails} check(s) failed")
sys.exit(1 if fails else 0)
PY
status=$?
if [[ $status -ne 0 ]]; then
  for s in tracker1 tracker2 tracker3 node1 node2; do
    echo "--- $s"; "${DC[@]}" logs --no-log-prefix --tail 5 "$s" 2>&1
  done
fi
exit $status
