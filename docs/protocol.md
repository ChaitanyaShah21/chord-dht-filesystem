# Wire protocol reference

Every program speaks the same framing, implemented once in [`src/net.cpp`](../src/net.cpp)
(decisions D-010, D-021, D-044, D-060 in [`ARCHITECTURE.md`](../ARCHITECTURE.md)). Plain TCP,
IPv4, no encryption or authentication.

## Framing

- **A request is one line; a reply is one line.** Lines end in `\n` (a trailing `\r` is dropped).
  `send_line` strips any terminator the caller supplied and replaces any `\n` or `\r` *inside* a
  line with a space, logging it as a bug — so no message can ever become two and leave the
  connection one reply behind (defect R6, D-010).
- **A line is at most 1,024 bytes** (`MAX_LINE`). A peer that sends more without a newline is
  disconnected rather than buffered.
- **Words are separated by single spaces.** A leading, trailing or doubled space produces an empty
  word, so the message fails its word count instead of being quietly accepted.
- **Bodies.** A line whose first word is `PUT`, `STORE`, `CHUNK` or `APPEND` is followed by a binary body,
  and its **last word is the body's length in bytes**: digits only, no sign, no leading zero (`0`
  alone is an empty body), at most 7 digits, at most 512 KiB (`MAX_BODY`, D-044). The length is
  checked before any memory is allocated. The line reader reads one byte per call and never past
  its newline, so the body's first byte is still in the socket when the header has been parsed; the
  body is then read in a loop until exactly that many bytes have arrived.
- **One exchange per connection** for every originator (D-024): connect, send, read one reply,
  close. A server answers requests on a connection until the peer closes it.
- **`SIGPIPE` is ignored** by every program (D-011), so a peer hanging up is an error value, not a
  process exit.
- **A request id may come first** (D-087): `RID <16 lowercase hexadecimal characters> <request>`. A server
  takes it off before anything else reads the line, logs it with every line about that request, and puts it on
  every call it makes for that request (a coordinator's `STORE`s, for example). The body rule looks past it:
  `RID 6f1c2a9e0b3d4c5a PUT <key> 2 524288` carries a body. A line that starts `RID ` but is not exactly that shape
  is answered `ERR malformed request id` and the connection closed.

## The metrics port

A process started with `--metrics <port>` answers on that port, on one thread, one connection at a time (D-083):

| Request | Reply |
|---|---|
| `GET /metrics HTTP/1.0` or `HTTP/1.1` (a query string is ignored) | `200`, `Content-Type: text/plain; version=0.0.4; charset=utf-8`, the Prometheus text format |
| another path | `404` |
| another method | `405` |
| anything that is not a request line | `400` |
| a request head over 4 KiB | `431` |

The whole head must arrive within 2 s and the response is sent within 2 s; then the connection is closed (after
`shutdown` and a brief drain, so that unread request bytes cannot make the kernel reset away the response). There
is no keep-alive. The metrics are listed in [`observability.md`](observability.md).

## Identifiers, keys and peers

| Thing | Wire form | Meaning |
|---|---|---|
| identifier | 16 hexadecimal characters | a 64-bit ring position: the top 64 bits of a SHA-1 (D-019) |
| key | 40 lowercase hexadecimal characters | a chunk's full SHA-1 — its identity (D-014). Uppercase, other lengths, `/` and `..` are refused before any path is built (D-045) |
| peer | `<identifier> <ip> <port>` | a ring member: a canonical dotted-quad IPv4 address and a port 1–65535 with no sign or leading zero |
| member identifier | `SHA-1("ip:port")` for member 0, `SHA-1("ip:port#i")` for member *i* | one process is several ring members (virtual nodes, D-047) |

## Ring messages (node)

A process may be several ring members, so a request for a member is prefixed
`TO <identifier> ` (D-047). Without the prefix it goes to member 0. A malformed prefix is answered
`ERR malformed TO prefix`; an identifier the process does not have, `ERR no ring member <id> here`.

| Request | Reply | Sent by | Decision |
|---|---|---|---|
| `PING` | `PONG <identifier>` | anyone checking liveness | D-035 |
| `FIND_SUCCESSOR <identifier>` | `OWNER <peer>` (my successor owns it) · `NEXT <peer>` (ask this closer node) | lookup originators, one step at a time | D-012, D-021 |
| `NEIGHBOURS` | `NEIGHBOURS <peer\|NONE> <n> <peer>…` — the predecessor, then the successor list, from one snapshot | a member's stabilise, to its successor | D-031 |
| `NOTIFY <peer>` | `OK` | a member's stabilise: "I believe I am your predecessor" | D-031, D-035 |
| `SUSPECT <ip> <port>` | `OK` | an originator that could not reach a node, to the node that named it — a hint; the receiver probes the suspect itself | D-036 |
| `FINGERS` | `FINGERS <n> <i> <peer> …` — one entry per run of equal fingers, *i* where the run starts | test and benchmark harnesses only (read-only) | D-031, D-038 |

Errors: `ERR empty request`, `ERR unknown request`, `ERR identifier must be 16 hexadecimal
characters`, `ERR <why>` for a malformed peer, `ERR too many distinct fingers for one line`. An
**orphaned** member (it has lost every node it knew, D-040) answers `ERR orphaned: rejoining the
ring` to `PING`, `FIND_SUCCESSOR`, `NEIGHBOURS` and `NOTIFY`; a reply beginning `ERR orphaned`
evicts that member from the caller's tables at once (D-067).

The originator checks every reply before believing it (D-024): a `NEXT` must name a node strictly
between the node asked and the key, and an `OWNER` reply must be one the key falls before. A
lookup sends at most 65 requests (`MAX_REQUESTS`).

## Chunk messages (node)

Chunk requests are for the **process**, not a member — the store belongs to the process — so they
carry no `TO` prefix (D-047). A node started without `--store` answers every one with
`ERR this node keeps no chunks`.

| Request | Reply | Decision |
|---|---|---|
| `PUT <key> <w> <length>` + body | `OK <copies>` once `w` copies (its own included) are confirmed on distinct processes · `ERR only <c> of <w> copies stored: <failures>` | D-046 — the receiver coordinates: checks the hash, stores, sends `STORE` to the other replicas in parallel |
| `STORE <key> <length>` + body | `OK` | D-046 — keep one copy, forward nothing |
| `GET <key>` | `CHUNK <length>` + body · `MISSING` | D-049 — the reader checks the bytes against the key itself |
| `HAVE <key> [<key> …]` (1 to 20 keys) | `HAVE <one 0/1 digit per key>` | D-050 — used by repair |
| `KEYS <after>` (`-` for the start) | `KEYS <n> <grace-seconds> <key> …` — up to 20 keys held after `<after>`, in order; `KEYS 0 <grace>` at the end | D-090 — the collector's listing; the grace lets it check D-091's inequality |
| `DROP <collection-key> <key> [<key> …]` (1 to 20 keys) | `DROPPED <one 0/1 digit per key> <bytes freed>` — `1`: dropped; `0`: written within the grace, or not held | D-090 – D-092 — each chunk is dropped only if nothing has written it for the node's grace |

`w` is `1`, `2` or `3`. Errors: `ERR key must be 40 lowercase hexadecimal characters`,
`ERR w must be 1, 2 or 3`, `ERR bytes do not hash to the key`, `ERR malformed chunk request`,
`ERR malformed body length` and `ERR chunk larger than <n> bytes` (both followed by a close, since
the stream position is then unknown), `ERR disk error`, and **`ERR busy`** — the process already
holds 32 chunk bodies in memory (D-052); the announced body is read and discarded so the reply is
not lost, and the sender treats it as a failed copy and may retry. `DROP` answers
`ERR deletion is not enabled on this node` without `--gc-key`, and `ERR wrong collection key`; a `DROP` with a bad
key in it, or more than 20, is refused whole before anything is dropped.

## Tracker messages

The tracker is a Raft group of replicas (D-069 – D-082; [`consensus.md`](consensus.md)); a single tracker is a group
of one. It holds `name → manifest key` and nothing else (D-058). A **name** is 1 to 200 bytes of visible ASCII (no
space, no control character).

### From clients

| Request | Reply |
|---|---|
| `REGISTER <name> <manifest-key> [<owner-hash>]` | `OK` — this registration, or an earlier one with the same key and the same owner hash, holds the name · `EXISTS <key>` — taken by a different key, or by the same key under another owner; nothing changes (D-059, D-089) |
| `UNREGISTER <name> <secret>` | `OK` — deleted · `DENIED` — the secret's SHA-1 is not the name's owner hash, or the name has no owner · `MISSING` (D-089) |
| `RESOLVE <name>` | `FILE <key>` · `MISSING` |
| `LIST [<after>]` | `NAMES <n> <name> …` — sorted, names after `<after>`, as many as fit in one line; `NAMES 0` means no more |
| `ENTRIES [<after>]` | `ENTRIES <n> <name> <key> …` — the same paging, with each name's manifest key: the collector's mark (D-090) |
| `PING` | `PONG` — any replica, leader or not |

An **owner hash** is the SHA-1 of the uploader's secret, 40 lowercase hexadecimal characters; a **secret** is 32 to 64
lowercase hexadecimal characters. The leader hashes an `UNREGISTER`'s secret before anything is logged: the Raft log
holds `UNREGISTER <name> <owner-hash>`, never the secret. A name registered without an owner hash can never be
deleted.

`REGISTER`, `UNREGISTER`, `RESOLVE`, `LIST` and `ENTRIES` are answered only by the leader. A registration is answered once its own log entry
is applied (D-075); a read once the leader has confirmed, through a majority, that it still leads (ReadIndex, D-076).
Any other replica, or a leader that cannot do either in time, answers instead:

| Reply | Meaning | What a client does |
|---|---|---|
| `REDIRECT <ip>:<port>` | not the leader; this replica believes the leader is there | ask that address at once |
| `UNAVAILABLE <reason>` | no leader is known (an election), or the request was not committed or confirmed within 2 s, or a later leader replaced it | ask the next replica; retrying is always safe (D-075) |

Errors: `ERR usage: …` for a wrong word count, `ERR a name is 1 to 200 visible ASCII characters`,
`ERR a key is 40 lowercase hexadecimal characters`, `ERR empty request`, `ERR unknown request`. A replica whose log
cannot be written stops (fail-stop) rather than answer.

### Between replicas (D-071)

```
VOTE <group> <term> <candidate> <last-index> <last-term>                                  -> VOTED <term> <0|1>
APPEND <group> <term> <leader> <prev-index> <prev-term> <commit> <seq> <count> <length>   -> APPENDED <term> <0|1> <index>
       + <length> bytes: <count> lines of "<term> <command>", each ending in '\n'
```

`<group>` is 8 hexadecimal characters of SHA-1 of the ordered member list; a call carrying another is answered
`ERR group mismatch` (D-079). `<candidate>` and `<leader>` are positions in that list. `APPENDED`'s `<index>` is, on
success, the last index the follower now matches; on failure, where the leader should try next. `<seq>` numbers the
leader's calls for ReadIndex. A body holds at most about 64 KiB of commands; its entries' terms must not fall and
must not exceed the call's term, and each command must be `NOOP` or `REGISTER <name> <key>`, or the whole call is
refused (`ERR malformed APPEND`). Since D-089 a command may also be `REGISTER <name> <key> <owner-hash>` or
`UNREGISTER <name> <owner-hash>`.

### For harnesses

| Request | Reply |
|---|---|
| `STATUS` | `STATUS <id> <role> <term> <leader-id or -> <commit> <applied> <last-index>` — read-only, any replica |

### The replica's log

`<dir>/raft_<port>.log`, one record per line, each ending in the first 8 hexadecimal characters of the SHA-1 of the
rest of its line (D-074):

```
TERM <term> <voted-for | ->
ENTRY <index> <term> <command>
CUT <index>                       (entries from <index> on are discarded)
```

Everything one step changes is appended as one batch and flushed with one `fdatasync` before the replica sends or
answers anything. At start-up the file is replayed up to the first record that is torn, fails its check, or is out
of sequence; if that is within the last 256 KiB it is a crash's tail and is cut off and reported, and if it is
further back the replica refuses to start. The single tracker's earlier `names_<port>.log` is not read.

## Manifests

A file's manifest is itself a chunk (D-017, D-054), in text:

```
chordfs-manifest 1
size <bytes of the file this manifest covers>
chunk <chunk size in bytes>
kind data | index
entries <n>
<40-hex key> <bytes>          (n lines, in file order)
```

A **data** manifest lists chunks; an **index** manifest lists child manifests, for files with more
than 8,192 chunks (4 GiB at 512 KiB). At most 8,192 entries per manifest and four levels. The reader
checks every count, sum and level, and fetches and verifies the whole tree, before it creates the
output file.

## Deadlines

| Where | Deadline | Set by | Decision |
|---|---|---|---|
| Every socket an originator opens (`client`, `chunk`, `lookup`) | 5 s on connect, send and receive — a hang guard | `LOOKUP_TIMEOUT_MS`, `CLIENT_TIMEOUT_MS` | D-024, D-025 |
| Every call one node makes to another | 200 ms on connect, send and receive | `node --timeout <ms>` | D-039, D-042 |
| A request line, or a body, arriving at a node or the tracker | the **whole** line or body within 10 s, enforced with `poll` on the time remaining | `--read-deadline <ms>` | D-061 |
| Every call one tracker replica makes to another | E (the election timeout base, 150 ms by default) on connect, send and receive | `tracker --election <ms>` | D-073 |
| A client request waiting at the tracker's leader for its commit or read confirmation | 2 s, then `UNAVAILABLE` | — | D-075, D-076 |
| A request head on a metrics port, and its response | 2 s each | `HTTP_DEADLINE_MS` | D-083 |
| An upload, from the client's start to its registration | 1 h, or the name is not registered | `client --upload-limit <s>` | D-091 |
| A chunk's age before a node will drop it | 2 h since it was last written, or since the node started | `node --gc-grace <s>` | D-091 |
| The collector's last `DROP`, after it began listing names | 30 min | `gc --deadline <s>` | D-091 |
| Each exchange a client makes with one tracker replica | 3 s; 10 s overall across replicas and retries | `client --tracker-wait <ms>` | D-081 |

The send and receive timeouts are set **before** `connect`, so a host that never answers the
handshake costs the deadline rather than the kernel's ~127 s of SYN retries (D-025). A timeout is
always reported apart from a refused connection: a refusal is proof that nothing is listening, a
timeout only suspicion (D-013).
