#!/usr/bin/env bash
# e2e-deadline.sh -- a peer gets a fixed time to send what it announced (D-061).
#
# One node process with a chunk store, started with --read-deadline 1000, and
# attacked from raw sockets the way a slow-loris client would:
#
#   1. connect and send nothing          -> closed at about 1 s
#   2. a request line, a byte at a time  -> closed at about 1 s, before it completes
#   3. a body, a few bytes at a time     -> closed at about 1 s; nothing stored, not even a temporary file
#   4. 32 bodies announced and withheld, sockets left OPEN -> the 33rd is refused
#      (ERR busy), and after the deadline the slots are free again without the
#      attacker closing anything
#   5. a whole 512 KiB body sent at once, and a slow but complete line, still work
#
# NODE_CMD runs a different node binary, e.g. the ThreadSanitizer build.
set -uo pipefail
set +m

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="${TMPDIR:-/tmp}/p2p-deadline-$$"
PORT="${PORT:-7760}"

# Kill only this suite's node, matched by its port -- never a machine-wide pkill (E6).
reap() {
  pkill -CONT -f "node[-a-z]* 127.0.0.1 $PORT " 2>/dev/null
  pkill -9 -f "node[-a-z]* 127.0.0.1 $PORT " 2>/dev/null
  local i
  for i in $(seq 40); do
    ss -ltn 2>/dev/null | grep -qE ":${PORT}[[:space:]]" || return 0
    sleep 0.25
  done
  return 1
}
cleanup() { reap >/dev/null 2>&1; rm -rf "$WORK"; }
trap cleanup EXIT

[[ -x "$ROOT/node" ]] || { echo "FAIL: $ROOT/node not built. Run 'make' first."; exit 1; }
mkdir -p "$WORK"
reap || { echo "FAIL: port $PORT busy, cannot start"; exit 1; }

echo "--- e2e-deadline (real process; ${NODE_CMD:-$ROOT/node}, read deadline 1000 ms) ---"
python3 - "$ROOT" "$WORK" "$PORT" "${NODE_CMD:-$ROOT/node}" <<'PY'
import hashlib, os, shlex, socket, subprocess, sys, time
root, work, port, node_cmd = sys.argv[1], sys.argv[2], int(sys.argv[3]), shlex.split(sys.argv[4])
DEADLINE = 1.0
failures = 0
def check(ok, what):
    global failures
    failures += 0 if ok else 1
    print(f"  {'PASS' if ok else 'FAIL'}  {what}", flush=True)

store = os.path.join(work, "store")
log = open(os.path.join(work, "node.log"), "w")
proc = subprocess.Popen(node_cmd + ["127.0.0.1", str(port), "--create", "--vnodes", "1", "--store", store,
                                    "--read-deadline", str(int(DEADLINE * 1000))],
                        stdout=subprocess.DEVNULL, stderr=log)

def connect():
    s = socket.create_connection(("127.0.0.1", port), timeout=10)
    s.settimeout(10)
    return s

def closed_after(s):
    """Seconds until the node closes the connection (recv returns b'' or a reset)."""
    t0 = time.monotonic()
    try:
        while s.recv(4096):
            pass
    except (ConnectionResetError, socket.timeout):
        pass
    return time.monotonic() - t0

def exchange(line, body=b""):
    s = connect()
    s.sendall(line.encode() + b"\n" + body)
    f = s.makefile("rb")
    reply = f.readline().decode().strip()
    data = b""
    if reply.startswith("CHUNK "):
        data = f.read(int(reply.split()[1]))
    s.close()
    return reply, data

key_of = lambda b: hashlib.sha1(b).hexdigest()

try:
    for _ in range(100):
        try:
            if exchange("PING")[0].startswith("PONG"):
                break
        except OSError:
            time.sleep(0.1)
    check(exchange("PING")[0].startswith("PONG"), "the node is up")

    # 1. idle
    s = connect()
    t = closed_after(s)
    check(0.8 <= t <= 2.5, f"a connection that sends nothing is closed after {t:.2f} s")

    # 2. a trickled line: 'HAVE <40 chars>' would take 45 x 0.15 s = 6.75 s
    s = connect()
    t0 = time.monotonic()
    line = ("HAVE " + "a" * 40 + "\n").encode()
    cut = None
    for i, b in enumerate(line):
        try:
            s.sendall(bytes([b]))
        except OSError:
            cut = time.monotonic() - t0
            break
        time.sleep(0.15)
        s.setblocking(False)
        try:
            if s.recv(1) == b"":
                cut = time.monotonic() - t0
                break
        except BlockingIOError:
            pass
        finally:
            s.setblocking(True)
    check(cut is not None and cut < 2.5, f"a line sent a byte every 150 ms is cut off at {cut if cut is None else round(cut, 2)} s, before it completes")
    s.close()

    # 3. a trickled body
    body = os.urandom(1000)
    s = connect()
    s.sendall(f"STORE {key_of(body)} 1000\n".encode())
    t0 = time.monotonic()
    sent = 0
    try:
        while sent < 1000:
            s.sendall(body[sent:sent + 5])
            sent += 5
            time.sleep(0.1)
            s.setblocking(False)
            try:
                if s.recv(1) == b"":
                    break
            except BlockingIOError:
                pass
            finally:
                s.setblocking(True)
    except OSError:
        pass
    t = time.monotonic() - t0
    check(sent < 1000 and t < 2.5, f"a body sent 5 bytes every 100 ms is cut off at {t:.2f} s, after {sent} of 1000 bytes")
    s.close()
    time.sleep(0.2)
    path = os.path.join(store, key_of(body)[:2], key_of(body))
    check(not os.path.exists(path) and not os.listdir(os.path.join(store, "tmp")),
          "and nothing of it is stored, not even a temporary file")

    # 4. withheld bodies, sockets left open
    held = []
    for i in range(32):
        c = connect()
        c.sendall(f"STORE {'a' * 40} 524288\n".encode())
        held.append(c)
    time.sleep(0.3)
    reply, _ = exchange(f"STORE {key_of(b'x')} 1", b"x")
    check(reply == "ERR busy", f"with 32 bodies withheld, the 33rd is refused: {reply}")
    time.sleep(DEADLINE + 0.8)
    reply, _ = exchange(f"STORE {key_of(b'x')} 1", b"x")
    check(reply == "OK", f"after the deadline the slots are free again, though the attacker closed nothing: {reply}")
    gone = 0
    for c in held:
        c.settimeout(1)
        try:
            gone += c.recv(1) == b""
        except (ConnectionResetError, socket.timeout):
            gone += 1
        c.close()
    check(gone == 32, f"and all {gone} of the withheld connections were closed by the node")

    # 5. legitimate traffic is unaffected
    big = os.urandom(512 * 1024)
    reply, _ = exchange(f"STORE {key_of(big)} {len(big)}", big)
    check(reply == "OK", "a whole 512 KiB body sent at once is stored")
    reply, data = exchange(f"GET {key_of(big)}")
    check(data == big, "and read back")
    s = connect()
    for b in b"PING\n":
        s.sendall(bytes([b]))
        time.sleep(0.1)
    reply = s.makefile("rb").readline().decode().strip()
    check(reply.startswith("PONG"), "a slow line that completes within the deadline is answered")
    s.close()
    check(proc.poll() is None, "the node is still running")
    sanitizer = "Sanitizer" in open(os.path.join(work, "node.log")).read()
    check(not sanitizer, "no sanitizer output in the node's log")
finally:
    proc.kill()
    proc.wait()

print(f"== e2e-deadline: {'PASS -- a peer cannot hold a node by sending slowly' if failures == 0 else f'FAIL -- {failures} check(s) failed'}")
sys.exit(1 if failures else 0)
PY
