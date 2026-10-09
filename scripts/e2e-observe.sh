#!/usr/bin/env bash
# e2e-observe.sh -- metrics, logs and request ids on real processes (D-083 to D-087).
#
# Three tracker replicas and four nodes, each with a metrics port; the real client.
#   1. every process answers GET /metrics with 200, the text format's content type,
#      an exact Content-Length, and a body that an independent parser here accepts:
#      every sample in a declared family, histogram buckets cumulative, +Inf == _count
#      (and `promtool check metrics` too, when PROMTOOL names one)
#   2. the numbers mean what they say: after one upload and one download, the nodes'
#      byte counters cover the file's copies, exactly one replica reports itself
#      leader, every replica has the same commit index, the nodes' lookups have hop
#      counts, maintenance rounds keep counting
#   3. a killed node is counted as an eviction by refusal; a killed leader as a leader
#      change and an election on the survivors
#   4. the metrics port under hostile input: a head over 4 KiB (431), a byte every
#      500 ms (cut off near the 2 s deadline), connections that send nothing, another
#      method (405), another path (404), garbage (400) -- and the protocol port keeps
#      answering throughout
#   5. one upload and one download, each with a chosen request id, reconstructed from
#      every process's log by scripts/trace.py: the client, the tracker leader, every
#      coordinator and every replica that stored a copy, with counts checked against
#      what the client reported
#
# Processes are stopped by PID or matched by their own port, never a machine-wide pkill.
set -uo pipefail
set +m

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="${TMPDIR:-/tmp}/observe-e2e-$$"
BASE_PORT="${BASE_PORT:-7900}"

reap () {
  local p
  for p in $(seq "$BASE_PORT" $((BASE_PORT + 15))); do
    pkill -CONT -f "tracker[-a-z]* $p " 2>/dev/null
    pkill -9 -f "tracker[-a-z]* $p " 2>/dev/null
    pkill -CONT -f "node[-a-z]* 127.0.0.1 $p " 2>/dev/null
    pkill -9 -f "node[-a-z]* 127.0.0.1 $p " 2>/dev/null
  done
}
cleanup() { reap; rm -rf "$WORK"; }
trap cleanup EXIT

for b in tracker node client; do [[ -x "$ROOT/$b" ]] || { echo "FAIL: $ROOT/$b not built. Run 'make' first."; exit 1; }; done
mkdir -p "$WORK"
reap

python3 - "$ROOT" "$WORK" "$BASE_PORT" "${PROMTOOL:-}" "${NODE_CMD:-$ROOT/node}" "${TRACKER_CMD:-$ROOT/tracker}" <<'PY'
import hashlib, os, re, signal, socket, subprocess, sys, threading, time
root, work, base, promtool, node_cmd, tracker_cmd = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4], sys.argv[5], sys.argv[6]
tports = [base + i for i in range(3)]            # trackers
nports = [base + 10 + i for i in range(4)]       # nodes
M = 1000                                         # a process's metrics port is its port + M
group = ",".join(f"127.0.0.1:{p}" for p in tports)
ring = f"127.0.0.1:{nports[0]},127.0.0.1:{nports[1]}"
fails = 0
def check(what, ok):
    global fails
    fails += 0 if ok else 1
    print(f"  {'PASS' if ok else 'FAIL'}  {what}", flush=True)

logs = os.path.join(work, "logs"); os.makedirs(logs)
procs = {}
def start_tracker(i):
    d = os.path.join(work, f"t{i}"); os.makedirs(d, exist_ok=True)
    procs[tports[i]] = subprocess.Popen(tracker_cmd.split() + [str(tports[i]), "--dir", d, "--ip", "127.0.0.1", "--group", group,
                                        "--id", str(i), "--metrics", str(tports[i] + M)],
                                        stdout=subprocess.DEVNULL, stderr=open(os.path.join(logs, f"tracker-{tports[i]}.log"), "a"))
def start_node(i, join=True):
    p = nports[i]
    how = ["--join", f"127.0.0.1:{nports[0]}"] if join else ["--create"]
    procs[p] = subprocess.Popen(node_cmd.split() + ["127.0.0.1", str(p)] + how + ["--vnodes", "4", "--period", "200",
                                "--store", os.path.join(work, f"s{p}"), "--metrics", str(p + M), "--log-level", "debug"],
                                stdout=subprocess.DEVNULL, stderr=open(os.path.join(logs, f"node-{p}.log"), "a"))
def kill(port):
    procs[port].kill(); procs[port].wait()

def listening(port, timeout=15):
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.5).close(); return True
        except OSError: time.sleep(0.05)
    return False

def http(port, raw, timeout=5.0):
    """Send raw bytes to the metrics port; return (status, headers, body, seconds)."""
    t0 = time.time()
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as s:
        s.settimeout(timeout)
        s.sendall(raw)
        data = b""
        while True:
            try: b = s.recv(65536)
            except (socket.timeout, ConnectionResetError): break
            if not b: break
            data += b
    head, _, body = data.partition(b"\r\n\r\n")
    lines = head.decode(errors="replace").split("\r\n")
    status = int(lines[0].split()[1]) if lines and len(lines[0].split()) > 1 else 0
    headers = {l.split(":", 1)[0].lower(): l.split(":", 1)[1].strip() for l in lines[1:] if ":" in l}
    return status, headers, body, time.time() - t0

def scrape(port):
    st, h, body, _ = http(port + M, b"GET /metrics HTTP/1.1\r\nHost: x\r\n\r\n")
    return st, h, body.decode()

# ---- an independent parser of the text format (the oracle for part 1) --------------
NAME = re.compile(r"^[a-zA-Z_:][a-zA-Z0-9_:]*$")
SAMPLE = re.compile(r'^([a-zA-Z_:][a-zA-Z0-9_:]*)(\{(.*)\})? (\S+)$')
LABEL = re.compile(r'([a-zA-Z_][a-zA-Z0-9_]*)="((?:[^"\\]|\\.)*)"(,|$)')
def parse_exposition(text):
    """-> (samples {(name, frozenset(labels)): value}, types {family: type}, problems [str])"""
    problems, types, samples, helped = [], {}, {}, set()
    if not text.endswith("\n"): problems.append("no final newline")
    for ln in text.split("\n")[:-1]:
        if ln.startswith("# HELP "):
            fam = ln.split(" ")[2]
            if fam in helped: problems.append(f"second HELP for {fam}")
            helped.add(fam); continue
        if ln.startswith("# TYPE "):
            parts = ln.split(" ")
            if len(parts) != 4 or parts[3] not in ("counter", "gauge", "histogram") or not NAME.match(parts[2]):
                problems.append(f"bad TYPE line: {ln}"); continue
            if parts[2] in types: problems.append(f"second TYPE for {parts[2]}")
            types[parts[2]] = parts[3]; continue
        m = SAMPLE.match(ln)
        if not m: problems.append(f"not a sample: {ln!r}"); continue
        name, lab_text, value = m.group(1), m.group(3) or "", m.group(4)
        labs, pos = [], 0
        while pos < len(lab_text):
            lm = LABEL.match(lab_text, pos)
            if not lm: problems.append(f"bad labels: {ln!r}"); break
            labs.append((lm.group(1), lm.group(2))); pos = lm.end()
        try: v = float(value.replace("+Inf", "inf"))
        except ValueError: problems.append(f"bad value: {ln!r}"); continue
        fam = name
        for suf in ("_bucket", "_sum", "_count"):
            if name.endswith(suf) and types.get(name[:-len(suf)]) == "histogram": fam = name[:-len(suf)]
        if fam not in types: problems.append(f"sample before its TYPE: {name}")
        elif types[fam] == "counter" and v < 0: problems.append(f"negative counter {name}")
        key = (name, frozenset(labs))
        if key in samples: problems.append(f"duplicate series {ln!r}")
        samples[key] = v
    # histograms: buckets cumulative, +Inf == _count
    for fam, t in types.items():
        if t != "histogram": continue
        groups = {}
        for (name, labs), v in samples.items():
            if name == fam + "_bucket":
                rest = frozenset(l for l in labs if l[0] != "le")
                le = dict(labs)["le"]
                groups.setdefault(rest, []).append((float(le.replace("+Inf", "inf")), v))
        for rest, bs in groups.items():
            bs.sort()
            if any(bs[i][1] > bs[i + 1][1] for i in range(len(bs) - 1)): problems.append(f"{fam}: buckets not cumulative")
            if bs[-1][0] != float("inf"): problems.append(f"{fam}: no +Inf bucket")
            if samples.get((fam + "_count", rest)) != bs[-1][1]: problems.append(f"{fam}: _count != +Inf bucket")
            if (fam + "_sum", rest) not in samples: problems.append(f"{fam}: no _sum")
    return samples, types, problems

def value(samples, name, **labels):
    """Sum of every series of `name` whose labels include `labels`."""
    want = set(labels.items())
    return sum(v for (n, labs), v in samples.items() if n == name and want <= set(labs))

def metrics(port):
    st, h, text = scrape(port)
    return parse_exposition(text)[0]

def tracker_status(port):
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=2) as s:
            s.sendall(b"STATUS\n"); return s.makefile("rb").readline().decode().split()
    except OSError: return []

def leader():
    for p in tports:
        st = tracker_status(p) if procs[p].poll() is None else []
        if len(st) > 2 and st[2] == "leader": return p
    return None

def client(*args, rid=None, log=None):
    cmd = [f"{root}/client", "--tracker", group, "--ring", ring]
    if rid: cmd += ["--request-id", rid]
    if log: cmd += ["--log", log]
    return subprocess.run(cmd + list(args), capture_output=True, timeout=120)

# ---- start ----------------------------------------------------------------------
for i in range(3): start_tracker(i)
ok = all(listening(p) and listening(p + M) for p in tports)
start_node(0, join=False)
ok = ok and listening(nports[0])
for i in range(1, 4):
    start_node(i); ok = ok and listening(nports[i])
ok = ok and all(listening(p + M) for p in nports)
t0 = time.time()
while leader() is None and time.time() - t0 < 10: time.sleep(0.1)
time.sleep(2.0)                                   # ten rounds: let the ring settle
check("three tracker replicas and four nodes start, each with a metrics port", ok and leader() is not None)

print("-- 1. every process's /metrics is well formed")
for p in tports + nports:
    st, h, text = scrape(p)
    samples, types, problems = parse_exposition(text)
    good = (st == 200 and h.get("content-type") == "text/plain; version=0.0.4; charset=utf-8" and
            int(h.get("content-length", -1)) == len(text.encode()) and not problems and len(samples) > 20)
    check(f"{p}: 200, text format 0.0.4, exact length, {len(samples)} samples, {len(types)} families"
          + ("" if not problems else f" -- {problems[:3]}"), good)
if promtool:
    for p in (tports[0], nports[0]):
        st, h, text = scrape(p)
        r = subprocess.run([promtool, "check", "metrics"], input=text.encode(), capture_output=True)
        out = (r.stdout + r.stderr).decode().strip()
        check(f"promtool check metrics accepts {p}'s exposition" + (f" -- {out[:300]}" if out else ""), r.returncode == 0)
else:
    print("  (PROMTOOL not set: promtool check skipped)")

print("-- 2. the numbers mean what they say")
data = os.path.join(work, "file.bin")
with open(data, "wb") as f: f.write(os.urandom(3 * 524288 + 1000))       # 4 chunks
before = {p: metrics(p) for p in nports}
r = client("put", data, "observed")
r2 = client("get", "observed", os.path.join(work, "back.bin"))
same = r.returncode == 0 and r2.returncode == 0 and open(data, "rb").read() == open(os.path.join(work, "back.bin"), "rb").read()
check("an upload and a download through the cluster", same)
after = {p: metrics(p) for p in nports}
bin_ = sum(value(after[p], "chordfs_chunk_bytes_total", direction="in") - value(before[p], "chordfs_chunk_bytes_total", direction="in") for p in nports)
bout = sum(value(after[p], "chordfs_chunk_bytes_total", direction="out") - value(before[p], "chordfs_chunk_bytes_total", direction="out") for p in nports)
size = os.path.getsize(data)
check(f"bytes in across nodes ({bin_}) cover three copies of the file ({3 * size}) and its manifest", 3 * size <= bin_ < 3 * size + 10000)
check(f"bytes out across nodes ({bout}) cover one copy of the file ({size})", size <= bout < size + 10000)
puts = sum(value(after[p], "chordfs_chunk_requests_total", verb="put", result="ok") - value(before[p], "chordfs_chunk_requests_total", verb="put", result="ok") for p in nports)
stores = sum(value(after[p], "chordfs_chunk_requests_total", verb="store", result="ok") - value(before[p], "chordfs_chunk_requests_total", verb="store", result="ok") for p in nports)
check(f"5 PUTs (4 chunks + 1 manifest) were coordinated ({puts}), each with 2 STOREs ({stores})", puts == 5 and stores == 10)
lat = sum(value(after[p], "chordfs_chunk_request_seconds_count", verb="put") for p in nports)
check("every coordinated PUT was timed", lat >= 5)
tm = {p: metrics(p) for p in tports}
leaders = [p for p in tports if value(tm[p], "chordfs_raft_role", role="leader") == 1]
check(f"exactly one replica reports role=leader ({leaders}), and it is the one STATUS names ({leader()})", leaders == [leader()])
time.sleep(0.5)
tm = {p: metrics(p) for p in tports}
commits = {value(tm[p], "chordfs_raft_commit_index") for p in tports}
terms = {value(tm[p], "chordfs_raft_term") for p in tports}
check(f"every replica has one commit index ({commits}) and one term ({terms})", len(commits) == 1 and len(terms) == 1)
check("the leader counted one REGISTER answered OK",
      value(tm[leaders[0]], "chordfs_tracker_requests_total", verb="register", result="ok") == 1)
check("every replica's flushes were timed", all(value(tm[p], "chordfs_raft_flush_seconds_count") > 0 for p in tports))
check("names gauge is 1 on every replica", all(value(tm[p], "chordfs_names") == 1 for p in tports))
hops = sum(value(after[p], "chordfs_lookup_hops_count") for p in nports)
check(f"the nodes' own lookups (finger repair) have hop counts ({int(hops)})", hops > 50)
m1 = value(metrics(nports[1]), "chordfs_ring_rounds_total"); time.sleep(1.0); m2 = value(metrics(nports[1]), "chordfs_ring_rounds_total")
check(f"maintenance rounds keep counting: {m2 - m1:.0f} in 1 s (4 members, 200 ms period: about 20)", 10 <= m2 - m1 <= 30)
check("every node's shortest successor list is 3 entries or more", all(value(after[p], "chordfs_ring_successor_list_min") >= 3 for p in nports))
check("stored chunks gauge counts every copy (15 = 5 chunks x 3)", sum(value(after[p], "chordfs_store_chunks") for p in nports) == 15)

print("-- 3. failures show up as counted events")
ev0 = sum(value(metrics(p), "chordfs_ring_evictions_total", evidence="refused") for p in nports[:3])
kill(nports[3])
time.sleep(2.0)
ev1 = sum(value(metrics(p), "chordfs_ring_evictions_total", evidence="refused") for p in nports[:3])
check(f"a killed node is evicted by refusal on the survivors ({ev0:.0f} -> {ev1:.0f})", ev1 > ev0)
old = leader()
survivors = [p for p in tports if p != old]
lc0 = {p: value(metrics(p), "chordfs_raft_leader_changes_total") for p in survivors}
el0 = sum(value(metrics(p), "chordfs_raft_elections_total") for p in survivors)
kill(old)
t0 = time.time()
while (leader() in (None, old)) and time.time() - t0 < 10: time.sleep(0.05)
time.sleep(0.5)
lc1 = {p: value(metrics(p), "chordfs_raft_leader_changes_total") for p in survivors}
el1 = sum(value(metrics(p), "chordfs_raft_elections_total") for p in survivors)
check(f"a killed leader: both survivors count a leader change ({lc0} -> {lc1}), and an election was started ({el0:.0f} -> {el1:.0f})",
      all(lc1[p] > lc0[p] for p in survivors) and el1 > el0)
start_tracker(tports.index(old)); listening(old); listening(old + M)

print("-- 4. the metrics port under hostile input")
mp = nports[0] + M
st, _, _, _ = http(mp, b"GET /metrics HTTP/1.1\r\nX: " + b"a" * 5000 + b"\r\n\r\n")
check("a 5 KB request head is refused with 431", st == 431)
st, _, _, _ = http(mp, b"POST /metrics HTTP/1.1\r\nContent-Length: 3\r\n\r\nabc")
check("POST answers 405", st == 405)
# A request whose body the server never reads: over TCP, closing with those bytes unread
# makes the kernel send a reset, which can destroy the response before the client reads
# it. The client here waits 300 ms before reading, so the reset (if any) arrives first.
with socket.create_connection(("127.0.0.1", mp), timeout=5) as s:
    s.sendall(b"POST /metrics HTTP/1.1\r\nContent-Length: 30000\r\n\r\n" + b"x" * 30000)
    time.sleep(0.3)
    try:
        late = s.recv(200)
    except ConnectionResetError:
        late = b"<reset>"
check(f"a 30 KB body the server never reads does not cost the client its answer ({late[:12]!r})", late.startswith(b"HTTP/1.0 405"))
st, _, _, _ = http(mp, b"GET /secret HTTP/1.1\r\n\r\n")
check("another path answers 404", st == 404)
st, _, _, _ = http(mp, b"\x16\x03\x01\x02\x00\x01\x00\x01\xfc\x03\x03\r\n\r\n")
check("a TLS hello (garbage) answers 400", st == 400)
st, _, _, _ = http(mp, b"GET /metrics HTTP/1.1\r\n\r\nGET /metrics HTTP/1.1\r\n\r\n")
check("two pipelined requests get one response (and a close)", st == 200)
# a trickler: one byte every 500 ms; the server must give up near 2 s
def trickle(port, result):
    t0 = time.time()
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=10) as s:
            for ch in b"GET /metrics HTTP/1.1\r\n":
                s.sendall(bytes([ch])); time.sleep(0.5)
                if time.time() - t0 > 8: break
            s.settimeout(5); result.append(s.recv(100))
    except OSError as e: result.append(e)
    result.append(time.time() - t0)
res = []
th = threading.Thread(target=trickle, args=(mp, res)); th.start(); th.join()
cut = res[-1]
check(f"a byte every 500 ms is cut off near the 2 s deadline (connection ended after {cut:.1f} s)", 1.8 <= cut <= 3.5)
# idle connections, one at a time: a scrape waits behind them; the protocol port does not
idles = [socket.create_connection(("127.0.0.1", mp)) for _ in range(3)]
t0 = time.time()
pong_ok = True
for _ in range(10):
    try:
        with socket.create_connection(("127.0.0.1", nports[0]), timeout=1) as s:
            s.sendall(b"PING\n"); pong_ok = pong_ok and s.makefile("rb").readline().startswith(b"PONG")
    except OSError: pong_ok = False
check(f"with 3 idle connections on the metrics port, the protocol port answers 10 PINGs at once ({time.time() - t0:.2f} s)",
      pong_ok and time.time() - t0 < 1.0)
st, _, body, took = http(mp, b"GET /metrics HTTP/1.1\r\n\r\n", timeout=15)
check(f"and a scrape waits behind them, one 2 s deadline each, then succeeds ({took:.1f} s)", st == 200 and 4.0 <= took <= 8.5)
for s in idles: s.close()
check("the node is still alive after all of it", procs[nports[0]].poll() is None and listening(nports[0], 1))
# the protocol port and a malformed request id announcing a body: refused, connection closed, body never read as lines
def raw_exchange(port, payload):
    data = b""
    with socket.create_connection(("127.0.0.1", port), timeout=3) as s:
        s.sendall(payload)
        try:
            while True:
                b = s.recv(4096)
                if not b: break
                data += b
        except ConnectionResetError:
            data += b"<reset>"
    return data
got = raw_exchange(nports[0], b"RID nothex0nothex0 PUT " + b"a" * 40 + b" 2 5\n")
check(f"a malformed request id is refused and the connection closed ({got!r})", got == b"ERR malformed request id\n")
# With more bytes already sent, closing with them unread makes the kernel reset the
# connection, which may destroy the ERR line before it is read (as for any refusal
# followed by a close); what must never happen is the next line being answered.
got = raw_exchange(nports[0], b"RID nothex0nothex0 PUT " + b"a" * 40 + b" 2 5\nPING\n")
check(f"and what followed it is never taken for a request ({got!r})", b"PONG" not in got)

print("-- 5. one request, reconstructed from every process's log")
time.sleep(1.0)
rid_put, rid_get = "a11ce0000000beef", "b0b0000000000001"
clog = os.path.join(logs, "client.log")
data2 = os.path.join(work, "traced.bin")
with open(data2, "wb") as f: f.write(os.urandom(5 * 524288 + 7))          # 6 chunks
r = client("put", data2, "traced", rid=rid_put, log=clog)
r2 = client("get", "traced", os.path.join(work, "traced.back"), rid=rid_get, log=clog)
check("the traced upload and download succeed", r.returncode == 0 and r2.returncode == 0)
put_line = r.stdout.decode().split()
nchunks, nmanifests = int(put_line[put_line.index("chunks") + 1]), int(put_line[put_line.index("manifests") + 1])
t = subprocess.run([sys.executable, f"{root}/scripts/trace.py", "--json", rid_put, logs], capture_output=True)
import json
evs = [json.loads(l) for l in t.stdout.decode().splitlines()]
procs_put = {e["proc"] for e in evs}
coord = [e for e in evs if e["event"] == "chunk.put"]
stored = [e for e in evs if e["event"] == "chunk.store"]
reg = [e for e in evs if e["event"] == "tracker.register"]
check(f"the upload's id is in the client's log, the tracker leader's and the nodes' ({sorted(procs_put)})",
      "client" in procs_put and any(p.startswith("tracker:") for p in procs_put) and sum(p.startswith("node:") for p in procs_put) >= 3)
check(f"one chunk.put per stored chunk and manifest ({len(coord)} = {nchunks} + {nmanifests})", len(coord) == nchunks + nmanifests)
check(f"two chunk.store per chunk.put, on processes other than the coordinator ({len(stored)})",
      len(stored) == 2 * len(coord) and all(s["proc"] != c["proc"] for c in coord for s in stored if s.get("key") == c.get("key")))
# A follower asked first answers REDIRECT -- that hop is in the trace too, under the same id.
check(f"the registration is answered OK once, by the leader, under the same id; the rest are redirects "
      f"({[(e['proc'], e.get('result')) for e in reg]})",
      [e.get("result") for e in reg].count("OK") == 1 and all(e.get("result") in ("OK", "REDIRECT") for e in reg))
hops_logged = [e for e in evs if e["event"] == "ring.request" and e.get("verb") == "FIND_SUCCESSOR"]
check(f"every lookup hop the client made is in some node's log ({len(hops_logged)} FIND_SUCCESSOR lines)", len(hops_logged) >= nchunks + nmanifests)
t2 = subprocess.run([sys.executable, f"{root}/scripts/trace.py", "--json", rid_get, logs], capture_output=True)
evg = [json.loads(l) for l in t2.stdout.decode().splitlines()]
gets = [e for e in evg if e["event"] == "chunk.get" and e.get("result") == "ok"]
check(f"the download's id: one chunk.get served per chunk and manifest ({len(gets)})", len(gets) == nchunks + nmanifests)
check("the two ids never mix", not any(e.get("req") == rid_put for e in evg) and not any(e.get("req") == rid_get for e in evs))
summary = subprocess.run([sys.executable, f"{root}/scripts/trace.py", rid_put, logs], capture_output=True).stdout.decode()
print("\n".join("     " + l for l in summary.strip().split("\n")[-8:]))
all_lines = 0; bad = 0
for name in os.listdir(logs):
    for line in open(os.path.join(logs, name), errors="replace"):
        all_lines += 1
        if not line.startswith("ts="): bad += 1
check(f"every one of {all_lines} log lines from {len(os.listdir(logs))} processes is a structured line", bad == 0)

print(f"e2e-observe: {'PASS' if fails == 0 else 'FAIL'} -- {fails} failed")
sys.exit(1 if fails else 0)
PY
rc=$?
if [[ $rc -eq 0 ]]; then echo "PASS -- every process's metrics parse and mean what they say, and one request is traced through every log"; else echo "FAIL"; fi
exit $rc
