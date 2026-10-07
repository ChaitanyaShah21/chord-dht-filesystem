"""ringcheck.py -- the oracle and the probes shared by e2e-churn.sh and bench-reconverge.py.

The ORACLE computes what every live node should hold, from the set of live
addresses alone: SHA-1 of each address, sorted (decision D-038). It shares no
code with the C++ nodes, so agreeing with it proves something.

The PROBE asks a node what it holds, over one connection, through the node's
production paths where one exists:
  FIND_SUCCESSOR <its id + 1>  -> OWNER <successor>   (the code path every lookup uses)
  NEIGHBOURS                   -> its predecessor and successor list (what stabilise reads)
  FINGERS                      -> its finger table, one entry per run (read-only)
The node reports what it holds; only this module decides whether that is right.
"""
import bisect, hashlib, os, signal, socket, subprocess, time
from concurrent.futures import ThreadPoolExecutor

MOD = 1 << 64
ID_BITS = 64


def ident(addr):
    """Routing identifier of "ip:port": the top 64 bits of SHA-1, as chord.cpp computes it."""
    return int(hashlib.sha1(addr.encode()).hexdigest()[:16], 16)


class Oracle:
    """The correct state of every live node, from the live addresses alone."""

    def __init__(self, live, r):
        pairs = sorted((ident(a), a) for a in live)
        self.ids = [p[0] for p in pairs]
        self.addrs = [p[1] for p in pairs]
        self.index = {a: i for i, a in enumerate(self.addrs)}
        self.r = r

    def owner(self, k):
        """The first live node whose identifier is >= k, wrapping past the top."""
        i = bisect.bisect_left(self.ids, k % MOD)
        return self.addrs[i % len(self.addrs)]

    def successor(self, a):
        return self.addrs[(self.index[a] + 1) % len(self.addrs)]

    def predecessor(self, a):
        """None for a ring of one: a lone node has no predecessor."""
        if len(self.addrs) == 1:
            return None
        return self.addrs[(self.index[a] - 1) % len(self.addrs)]

    def successor_list(self, a):
        """The next min(r, N - 1) nodes clockwise; just [a] for a ring of one."""
        n = len(self.addrs)
        if n == 1:
            return [a]
        i = self.index[a]
        return [self.addrs[(i + s) % n] for s in range(1, min(self.r, n - 1) + 1)]

    def fingers(self, a):
        """finger[i] = owner(id(a) + 2^i), for i in 0..63."""
        me = ident(a)
        return [self.owner(me + (1 << i)) for i in range(ID_BITS)]


# ---- probing a node --------------------------------------------------------

def _peer(words, i):
    """'<id> <ip> <port>' at words[i:i+3] -> 'ip:port'."""
    return f"{words[i + 1]}:{words[i + 2]}"


def probe(addr, timeout=2.0):
    """What `addr` holds, or None if it could not be asked. One connection, three requests."""
    ip, port = addr.split(":")
    try:
        s = socket.create_connection((ip, int(port)), timeout=timeout)
        s.settimeout(timeout)
        f = s.makefile("rb")
        me = ident(addr)
        s.sendall(f"FIND_SUCCESSOR {(me + 1) % MOD:016x}\nNEIGHBOURS\nFINGERS\n".encode())
        owner = f.readline().decode().split()
        neigh = f.readline().decode().split()
        fing = f.readline().decode().split()
        s.close()
    except (OSError, ValueError):
        return None
    try:
        out = {}
        if owner[0] != "OWNER":
            return None
        out["succ"] = _peer(owner, 1)
        i = 1
        if neigh[1] == "NONE":
            out["pred"], i = None, 2
        else:
            out["pred"], i = _peer(neigh, 1), 4
        n = int(neigh[i])
        out["succs"] = [_peer(neigh, i + 1 + 3 * k) for k in range(n)]
        if fing[0] != "FINGERS":
            return None
        runs = int(fing[1])
        table = [None] * ID_BITS
        starts = [(int(fing[2 + 4 * k]), _peer(fing, 3 + 4 * k)) for k in range(runs)]
        for k, (start, peer) in enumerate(starts):
            end = starts[k + 1][0] if k + 1 < runs else ID_BITS
            for j in range(start, end):
                table[j] = peer
        out["fingers"] = table
        return out
    except (IndexError, ValueError):
        return None


def judge(states, oracle):
    """Three verdicts over every live node: (ring correct, lists correct, fingers correct),
    plus the addresses that were wrong on the first count, for diagnosis."""
    ring = lists = fingers = True
    wrong = []
    for a in oracle.addrs:
        st = states.get(a)
        if st is None:
            ring = lists = fingers = False
            wrong.append(a)
            continue
        if st["succ"] != oracle.successor(a) or st["pred"] != oracle.predecessor(a):
            ring = False
            wrong.append(a)
        if st["succs"] != oracle.successor_list(a):
            lists = False
        if st["fingers"] != oracle.fingers(a):
            fingers = False
    return ring, lists, fingers, wrong


class Poller:
    """Probe every live node concurrently; one poll = one probe of each."""

    def __init__(self, workers=32):
        self.pool = ThreadPoolExecutor(max_workers=workers)

    def poll(self, addrs):
        t0 = time.monotonic()
        results = list(self.pool.map(probe, addrs))
        return dict(zip(addrs, results)), t0, time.monotonic()

    def close(self):
        self.pool.shutdown(wait=True)


def wait_converged(poller, oracle, limit_s, stable_s=0.0):
    """Poll until all three verdicts hold and have held for `stable_s`.
    Returns (ok, polls, last verdict)."""
    deadline = time.monotonic() + limit_s
    since, polls, verdict = None, 0, None
    while time.monotonic() < deadline:
        states, _, t_end = poller.poll(oracle.addrs)
        polls += 1
        verdict = judge(states, oracle)
        if verdict[0] and verdict[1] and verdict[2]:
            since = since or t_end
            if t_end - since >= stable_s:
                return True, polls, verdict
        else:
            since = None
        time.sleep(0.02)
    return False, polls, verdict


# ---- running nodes ---------------------------------------------------------

class Cluster:
    """Node processes, each remembered by address so one can be signalled exactly."""

    def __init__(self, node_bin, logdir, period_ms, timeout_ms, succ=8):
        self.node_bin, self.logdir = node_bin, logdir
        self.opts = ["--period", str(period_ms), "--timeout", str(timeout_ms), "--succ", str(succ)]
        self.procs = {}
        os.makedirs(logdir, exist_ok=True)

    def start(self, addr, mode):
        """mode: ["--create"], ["--join", "ip:port"] or ["--wired", file]."""
        ip, port = addr.split(":")
        log = open(os.path.join(self.logdir, f"node-{port}.log"), "a")
        self.procs[addr] = subprocess.Popen([self.node_bin, ip, port] + mode + self.opts,
                                            stdout=subprocess.DEVNULL, stderr=log)
        log.close()
        return self.procs[addr]

    def signal(self, addr, sig):
        os.kill(self.procs[addr].pid, sig)

    def kill(self, addr):
        p = self.procs.pop(addr)
        p.send_signal(signal.SIGCONT)        # a stopped process cannot die until resumed
        p.kill()
        p.wait()

    def kill_all(self):
        for a in list(self.procs):
            self.kill(a)


def wait_listening(addr, limit_s=10.0):
    deadline = time.monotonic() + limit_s
    while time.monotonic() < deadline:
        ip, port = addr.split(":")
        try:
            socket.create_connection((ip, int(port)), timeout=0.5).close()
            return True
        except OSError:
            time.sleep(0.02)
    return False
