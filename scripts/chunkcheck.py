"""chunkcheck.py -- the oracle and a raw client for the chunk plane (Phase 4).

Shared by e2e-chunk.sh and the Phase 4 benchmarks. Like ringcheck.py it shares no
code with the C++ nodes:

  member_id(addr, i)      identifier of member i of the process at addr (D-047)
  Placement               where every key's three copies belong, from the live
                          processes alone: the first RF distinct processes
                          clockwise from the key (D-048)
  have(addr, keys)        ask a process which keys it holds (HAVE, D-050)
  exchange(addr, line, body)
                          one raw request with the framing written out here from
                          the protocol description (D-044): a header line, then the
                          body's bytes; the reply line, then its body if it is CHUNK
  ring_correct(...)       is every live member's successor and predecessor right?
"""
import bisect, hashlib, socket

MOD = 1 << 64
RF = 3
MAX_HAVE = 20


def member_id(addr, i=0):
    """SHA-1("ip:port") for member 0, SHA-1("ip:port#i") for member i >= 1; top 64 bits."""
    s = addr if i == 0 else f"{addr}#{i}"
    return int(hashlib.sha1(s.encode()).hexdigest()[:16], 16)


def key_of(data):
    return hashlib.sha1(data).hexdigest()


def key_id(key):
    return int(key[:16], 16)


class Placement:
    """Members of the live processes, sorted by identifier."""

    def __init__(self, processes, v):
        pairs = sorted((member_id(a, i), a) for a in processes for i in range(v))
        self.ids = [p[0] for p in pairs]
        self.owners = [p[1] for p in pairs]
        self.processes = list(processes)

    def owner_index(self, k):
        return bisect.bisect_left(self.ids, k % MOD) % len(self.ids)

    def holders(self, key, rf=RF):
        """The first rf distinct processes clockwise from the key's owner."""
        out = []
        i = self.owner_index(key_id(key))
        for step in range(len(self.ids)):
            a = self.owners[(i + step) % len(self.ids)]
            if a not in out:
                out.append(a)
                if len(out) == rf:
                    break
        return out

    def successor(self, idx):
        return (self.ids[(idx + 1) % len(self.ids)], self.owners[(idx + 1) % len(self.ids)])

    def predecessor(self, idx):
        return (self.ids[idx - 1], self.owners[idx - 1])


def _readline(f):
    line = f.readline()
    if not line.endswith(b"\n"):
        raise ConnectionError("closed before a full line")
    return line[:-1].decode()


def exchange(addr, line, body=b"", timeout=5.0):
    """Send one request; return (reply line, reply body). Raises OSError on failure."""
    ip, port = addr.split(":")
    with socket.create_connection((ip, int(port)), timeout=timeout) as s:
        s.settimeout(timeout)
        s.sendall(line.encode() + b"\n" + body)
        f = s.makefile("rb")
        reply = _readline(f)
        data = b""
        if reply.startswith("CHUNK "):
            n = int(reply.split()[1])
            data = f.read(n)
            if len(data) != n:
                raise ConnectionError("closed mid-body")
        return reply, data


def have(addr, keys, timeout=5.0):
    """Which of `keys` the process at addr holds, as a list of booleans; None if unreachable."""
    out = []
    try:
        for i in range(0, len(keys), MAX_HAVE):
            batch = keys[i:i + MAX_HAVE]
            reply, _ = exchange(addr, "HAVE " + " ".join(batch), timeout=timeout)
            bits = reply.split()[1] if reply.startswith("HAVE ") else ""
            if len(bits) != len(batch):
                return None
            out += [b == "1" for b in bits]
    except (OSError, ValueError):
        return None
    return out


def under_replicated(placement, keys, timeout=5.0):
    """Keys not held by every one of their correct live processes."""
    by_proc = {}
    for k in keys:
        for a in placement.holders(k):
            by_proc.setdefault(a, []).append(k)
    missing = set()
    for a, ks in by_proc.items():
        got = have(a, ks, timeout)
        if got is None:
            missing.update(ks)
            continue
        missing.update(k for k, ok in zip(ks, got) if not ok)
    return missing


def ring_correct(placement, timeout=2.0):
    """Every live member's successor (as routed) and predecessor match the placement."""
    for idx, (mid, a) in enumerate(zip(placement.ids, placement.owners)):
        try:
            succ_line, _ = exchange(a, f"TO {mid:016x} FIND_SUCCESSOR {(mid + 1) % MOD:016x}", timeout=timeout)
            nb_line, _ = exchange(a, f"TO {mid:016x} NEIGHBOURS", timeout=timeout)
        except (OSError, ValueError):
            return False
        w = succ_line.split()
        sid, saddr = placement.successor(idx)
        if len(w) != 4 or w[0] != "OWNER" or int(w[1], 16) != sid or f"{w[2]}:{w[3]}" != saddr:
            return False
        n = nb_line.split()
        pid, paddr = placement.predecessor(idx)
        if len(placement.ids) > 1 and (len(n) < 4 or n[1] == "NONE" or int(n[1], 16) != pid
                                       or f"{n[2]}:{n[3]}" != paddr):
            return False
    return True
