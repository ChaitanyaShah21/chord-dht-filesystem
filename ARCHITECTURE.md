# Architecture — chord-dht-filesystem

The current design first — what the system is, its parts, what must always hold, how it behaves
under concurrency and failure — and then the full **decision log**, D-001 to D-093: every decision,
the alternatives it rejected, and the evidence that has since confirmed or contradicted it. The
README's design rationales and [`docs/design.md`](docs/design.md) summarise it; the guides in
[`docs/`](docs/) describe each subsystem's code.

> **State as of 9 Oct 2026.** Files are cut into content-addressed chunks stored three times on a
> Chord ring, and a small tracker — a Raft group of three replicas — maps names to manifest keys.
> Everything in the current-state
> sections below is built, tested and measured ([`BENCHMARKS.md`](BENCHMARKS.md)). The coursework
> system this repository began as — a tracker with users, groups and seeder lists, and peers serving
> pieces to each other — was retired in Phase 5 (D-060); it is described in the decisions that
> changed it (D-004 to D-011), in [`docs/postmortem-resurrection.md`](docs/postmortem-resurrection.md),
> and in the history (`pre-resurrection`, and `e381179` for the transfer baseline).

---

## What this system does

Distributed file storage. A file is cut into 512 KiB chunks, each named by the SHA-1 of its own
bytes and stored on the first three distinct processes clockwise from that name on a Chord ring. A
**manifest** — the list of a file's chunk names and sizes — is itself a chunk in the ring. A small
**tracker** holds only `name → manifest key`, replicated by Raft across three replicas so that it survives the loss
of any one. A client asks the tracker for one key, then routes to
every chunk itself in O(log N) hops, fetches the chunks in parallel, verifies each against the name
it asked for, and assembles the file under a temporary name that is renamed into place only when it
is complete.

**Where it came from:** a BitTorrent-style file sharer — a tracker that knew which peers held which
files, and peers that served pieces to each other. The decisions in the log below turned it into
storage one at a time; D-022 records the shift and what it cost. The tracker, the last single point of
failure, became a Raft group in Phase 7 (D-069 – D-082; [`docs/consensus.md`](docs/consensus.md)).

**Scope boundary — what it deliberately does not do:**

- **No storage engine.** Chunks are files on disk (D-045). No log-structured merge tree, no
  compaction. The tracker's log is a plain append-only list of registrations (D-058), replayed
  whole at start-up. The interesting problems here are routing, replication and failure
  detection, and adding a storage engine would dilute all three.
- **No NAT traversal, no wide-area deployment.** Every peer is directly addressable. Hole
  punching and relays are a different project.
- **No encryption or authentication on the wire.** Stated as a limitation, not hidden. Running
  this on the open internet would need node identities bound to keys and authenticated channels —
  a better answer than a half-built TLS integration.
- **No incentive mechanism.** BitTorrent's tit-for-tat choking algorithm is deliberately out of
  scope; peers are assumed cooperative.
- **No users, groups or authentication** — removed in Phase 5, as D-004 decided. They were a
  coursework requirement, not a distributed-systems one; the cost is that anyone who can reach the
  tracker can register a name (D-059 limits the damage: names are written once).

Non-goals matter. "Why didn't you build X?" is answered much better by a stated scope boundary
than by an apology.

---

## The system as it exists today

Kept as diagram-as-text so it diffs in review and cannot silently drift from the design. The design
as first drawn, before any of it was built, is kept further down as a historical record.

```mermaid
flowchart TB
    C["<b>client</b><br/>put / get · workers pull chunk numbers from one counter (D-056)<br/>verifies every chunk · part file, fsync, rename (D-055)"]

    subgraph control["CONTROL PLANE — a Raft group (D-069 – D-082)"]
        direction LR
        T["<b>tracker replica · leader</b><br/>name → manifest key, nothing else (D-058)<br/>answers after its entry is applied (D-075)"]
        T2["<b>replica · follower</b><br/>log flushed before every reply (D-074)"]
        T3["<b>replica · follower</b><br/>redirects clients to the leader"]
        T -- "APPEND entries · heartbeats" --> T2
        T -- "APPEND" --> T3
    end

    subgraph ring["DATA PLANE — node processes, 8 ring members each (D-047, D-051)"]
        direction LR
        P1["<b>process A</b><br/>chunk store"] --> P2["<b>process B</b><br/>chunk store"] --> P3["<b>process C</b><br/>chunk store"] --> P4["<b>process D</b><br/>chunk store"] --> P1
    end

    C -- "1 · RESOLVE name / REGISTER name key" --> T
    T -- "2 · FILE manifest-key" --> C
    C -- "3 · FIND_SUCCESSOR, hop by hop (iterative, D-012)" --> ring
    C -- "4 · GET manifest, then GET every chunk in parallel" --> P2
    P2 -- "5 · CHUNK n + n bytes · hashed by the client (D-049)" --> C
    P2 -. "PUT: the owner stores, then STORE to two other processes (D-046)" .-> P3
```

**Reading the path.** An upload stores every chunk through its owner (which forwards two more
copies, answering after `W`), then the manifest (a tree of them for files over 4 GiB, D-054), and
only then registers the name — so a name never points at a file that is not all there. A download
resolves the name (steps 1–2), fetches and checks the manifest, then runs the lookup and `GET` for
every chunk concurrently (steps 3–5); a chunk that fails its hash or whose holder is dead is read
from the next replica. The tracker sees one short line per file and never a chunk, a node address or
a user.

---

## Components

| Component | Code | Responsibility | Owns | Talks to |
|---|---|---|---|---|
| `tracker` | `src/tracker.cpp`; `replica.cpp`, `raft.cpp`, `raft_log.cpp`, `raft_wire.cpp`, `names.cpp` | One replica of the namespace's Raft group: `REGISTER`, `RESOLVE`, `LIST`, `PING` for clients (D-058); `VOTE`, `APPEND` between replicas (D-071); a group of one is the single tracker (D-080) | its term, vote and log, `raft_<port>.log`; the `name → key` map applied from it | clients and the other replicas, one detached thread per connection; a ticker; a sender per peer |
| `node` | `src/node.cpp`; `stabilize.cpp`, `chunk.cpp`, `repair.cpp`, `store.cpp` | One process: several ring members, their routing state and maintenance; a chunk store; coordinating writes; repair; dropping chunks a collector found unused, once old enough (D-091) | each member's successor list, fingers, predecessor; the store's files and index | other nodes and clients, one detached thread per connection; a maintenance thread and a repair thread |
| `client` | `src/client.cpp`; `transfer.cpp`, `group.cpp` | Upload and download of whole files; names through the tracker group's leader, found by redirects (D-081); deleting a name with its upload's token (D-089) | nothing persistent; during a transfer, one chunk per worker and the part file | the tracker replicas, and ring nodes, over a fresh connection per exchange |
| `gc` | `src/gc_main.cpp`; `collect.cpp` | The collector: marks every chunk reachable from a live name, finds every node by walking the ring, and asks each to drop the rest (D-090) | nothing persistent; during a collection, every live key | the tracker group (`ENTRIES`), and every node (`NEIGHBOURS`, `GET`, `KEYS`, `DROP`) |
| `chunk`, `lookup` | `src/chunk_main.cpp`, `src/lookup_main.cpp` | Tools: one chunk, or one lookup, through the ring | nothing | ring nodes |
| `sim-hops`, `sim-balance` | `src/sim_hops.cpp`, `src/sim_balance.cpp` | Benchmark replays: the same routing (D-027) and placement (D-051) code with the ring in one process | nothing | — |
| framing | `src/net.cpp` | The one framing layer: lines, bodies, connect deadline, server read deadline, the request id prefix (D-010, D-044, D-025, D-061, D-087) | nothing | — |
| observability | `src/metrics.cpp`, `src/http.cpp`, `src/log.cpp` | Counters, histograms and gauges in the Prometheus text format; the metrics port's HTTP/1.0 responder; one `key=value` line per event and the request id (D-083 – D-087) | each process's registry; its request id | Prometheus, over `--metrics <port>` |
| hashing | `src/sha1.h` | Header-only wrapper over OpenSSL `SHA1()`, hex-encoding the 20-byte digest | nothing | — |

Wire formats: [`docs/protocol.md`](docs/protocol.md). Repository layout: D-068.

---

## Invariants

Things that must always hold, each tested. I1–I6 were written in the Phase 0 audit against the
coursework system, where all but I1 were violated; they are restated for the system that replaced it.

| # | Invariant | Enforced by | Tested by | Holds? |
|---|---|---|---|---|
| I1 | A chunk written to a file hashes to the key its manifest names | `get_chunk` hashes every chunk it accepts (D-049); its length is checked against the manifest (D-055) | `test-chunk`, `test-transfer`, `e2e-transfer.sh` (a corrupt copy) | ✓ |
| I2 | **A failed download leaves no file behind** | the part file is the only file written; it is renamed only when complete, and deleted on failure (D-055) | `test-transfer`, `e2e-transfer.sh` | ✓ (was ✗, R2b) |
| I3 | **Tracker state after restart equals state before restart** | every step's term, vote and entries are flushed before anything is sent; replay rebuilds the log, and committed entries are applied through the one apply function (D-009, D-074, D-080) | `test-raft`, `e2e-persistence.sh`, `e2e-raft.sh` | ✓ (was ✗, R1) |
| I4 | A node serves only chunks, by a key validated before any path is built | `valid_key` before every path (D-045); there is no other way to name a file | `test-chunk`, `e2e-chunk.sh` (traversal keys) | ✓ (was ✗, D3) |
| I5 | Malformed input from any peer cannot terminate a process | strict parsers that return errors (no throwing conversions on peer input); `SIGPIPE` ignored (D-011) | `test-lookup`, `test-chunk`, `test-transfer`, `test-names`, `e2e-framing.sh` | ✓ (was ✗, D4) |
| I6 | Every reply read is the reply to the request just sent | one exchange per fresh connection everywhere (D-024); no connection is shared between threads | by construction | ✓ (was ✗, D2) |
| I7 | **A routing identifier decides which *node*; it never decides which *object*.** The chunk store is keyed by the full 160-bit hash | D-019 — truncation is applied to placement only; the store (D-045) names every file by the full 40-character key, validated before any path is built | `test-chunk` (keys, store), `e2e-chunk.sh` | ✓ |
| I8 | **"Am I alone in the ring?" is answered by comparing *addresses*, never identifiers alone** — because `(n, n]` evaluates to the whole ring, so `successor.id == my_id` silently means "I own everything". With virtual nodes (D-047) a ring member is an address *and* an identifier, and a process is an address | D-019, D-047 — `same_node` compares address and identifier, `same_process` the address | `test-stabilize`, `test-chunk` (siblings) | ✓ |
| I9 | **A name never points at a file that is not all there** | the client registers a name only after every chunk and manifest is stored (D-017, D-058) | `e2e-transfer.sh` (a failed upload leaves its name unregistered) | ✓ |
| I10 | **At most one tracker leader per term, and no two replicas apply different entries at one index** | one vote per term, flushed before it is sent; votes only for logs at least as up to date; the commit rule counts only the leader's own term (D-074, Figure 8) | `test-raft`: checked after every step of seeded histories with loss, duplication, reordering, partitions and crashes | ✓ |
| I11 | **An acknowledged registration survives the loss of any minority of tracker replicas, and every answer is linearizable** | `OK` only after the request's own entry is committed by a majority and applied (D-075); reads by ReadIndex (D-076) | `test-raft` (a linearizability checker per history), `test-replica`, `e2e-raft.sh` (a writer through a leader kill) | ✓ |
| I13 | **No chunk any live name reaches is dropped, and no chunk of an acknowledged upload is ever dropped** | the mark reads every live tree or nothing is dropped (D-090); a node drops only what nothing has written for the grace, renewed by every write and reset by a restart, checked and unlinked under the store's lock (D-091) | `test-gc` (shared chunks and manifests, 2,000 racing write/drop rounds, an upload in flight, reused orphans), `e2e-gc.sh` | ✓ |
| I14 | **A name is deleted only with its owner's secret, and no secret is stored** | the tracker keeps SHA-1 of the secret; the leader hashes an `UNREGISTER`'s secret before proposing; ownerless names are never deleted (D-089) | `test-names`, `e2e-gc.sh` (other tokens, the hash itself, ownerless names; every log and Raft log searched for the secrets) | ✓ |
| I12 | **One event is one log line, and one metrics response is one well-formed exposition** | values with a space, `=`, a quote or a control character are quoted and escaped; one `write` per line; `+Inf` rendered as the sum of the buckets read (D-084, D-086) | `test-obs` (hostile values, 8 threads through one pipe), `e2e-observe.sh` (an independent parser on every process; every line of every log parsed) | ✓ |

---

## Concurrency model

### Tracker replica

| Shared state | Protected by | Held for how long | Why |
|---|---|---|---|
| the Raft core (term, vote, log, commit index, each peer's progress), the `name → key` map, waiting requests, each peer's pending call | `Replica::mu_` — one mutex (D-072) | one call into the core, **including its flush**; never a network call | No thread can see, and no reply can carry, state that is not yet durable. The cost: while a flush runs nothing else on that replica proceeds, and the leader's proposals are serialised at the flush rate (D-082) |

**Threads:** one `accept` loop and one detached thread per connection (a client's request waits on a
condition variable until its entry is applied or its read confirmed, at most 2 s); a ticker every 5 ms
(elections, heartbeats, check-quorum); one sender per peer, which makes its calls with the lock released; and
with `--metrics` one metrics thread, whose gauges copy the replica's state under the same lock — so a scrape
can wait behind a flush, never the other way round.

### Node

One mutex per ring member's routing state, never held across a network call (D-030); the store's index
has its own; the body cap is one atomic counter (D-052). Threads: the `accept` loop, one detached thread
per connection (bounded in time by the read deadline, not in number), one maintenance thread for all
members, one repair thread, and with `--metrics` one thread serving the metrics port, one connection at a
time (D-083). A coordinator's copy threads are joined by the request's own thread after it
answers (D-046).

**Metrics and logs** (D-084, D-086): every counter and histogram bucket is a `std::atomic` updated with relaxed
ordering, from any thread, without a lock; the registry's list of series is written before any thread that reads it
starts, and only read after. A gauge is a function the metrics thread calls, taking the one lock that guards what it
reads (a member's mutex, the store's, the tracker replica's). A log line is built in memory and written with one
`write`, so lines from different threads never interleave. The request id is `thread_local`: each connection
thread has its own, and work handed to a new thread (a coordinator's copies) is given the id explicitly.

### Client

`--workers` threads per transfer (D-056), sharing: one atomic counter for the next chunk number
(`fetch_add`), one atomic stop flag, the file descriptor (used only with `pread`/`pwrite`, which take
their offset as an argument, so no shared file position), a vector of per-chunk results in which each
worker writes only its own elements, and the first failure's message behind a mutex. The entry-point
preference (D-057) is an atomic index. A signal handler reads one lock-free atomic flag and calls
`unlink` (D-055).

**Detached versus joined:** node and tracker connection threads are detached and nothing joins them,
so neither program has a clean shutdown path; they are killed. Crash-only is the design (D-033), and
every write that matters — a stored chunk (D-045), a registered name (D-058) — is flushed before it is
acknowledged, so a kill loses nothing acknowledged. Worker threads are joined.

---

## Failure model

| Failure | Detected how | Detection time | Recovery | Measured in |
|---|---|---|---|---|
| A node process dies | a refused connection, by any caller (D-013, D-035) | the next call to it; at most a stabilisation period | evicted; reads and writes fall through to the next replica (D-049); its chunks re-replicated by their new owners (D-050) | `BENCHMARKS.md` §4, §6; `e2e-transfer.sh` (killed mid-download) |
| A node is frozen | two timed-out calls (D-035) | about two periods | as above, once evicted | §4, §6 |
| A chunk copy rots on disk | the reader's hash check; the scrub | at read time; within a full scan | read from the next replica; the copy replaced (D-050) | `e2e-chunk.sh`, `e2e-transfer.sh` |
| A chunk is lost from every replica | every replica fails the read | at read time | none: the download fails and writes nothing (D-055) | `e2e-transfer.sh` |
| The tracker leader dies or freezes | followers stop hearing heartbeats | an election timeout, E to 2E | a follower with an up-to-date log is elected; clients are redirected to it (D-073, D-081) | `BENCHMARKS.md` §7; `e2e-raft.sh` |
| A tracker leader is cut off from the majority | no replies from a majority | 2E (check-quorum, D-077) | it steps down; it could never commit or answer a read meanwhile | `e2e-raft.sh`, `test-replica` |
| A majority of tracker replicas is down | — | — | none until a majority is back: requests answer `UNAVAILABLE` and the client fails, never acknowledges; a download by manifest key still works (D-057) | `e2e-raft.sh`, `e2e-transfer.sh` |
| A tracker replica restarts | — | — | its log is replayed; a crash's torn tail is cut off and reported, damage further back refuses to start (D-074); it catches up from the leader | `e2e-persistence.sh`, `e2e-raft.sh` |
| A client dies mid-download | — | — | a `.part-` file is left (SIGKILL) or removed (SIGINT, SIGTERM) | `e2e-transfer.sh` |
| A peer sends slowly or not at all | the server's read deadline (D-061) | the deadline (10 s by default) | the connection is closed; its body slot and thread freed | `e2e-deadline.sh` |
| A collection meets a node that is down, missed, or whose grace is too short | the ring walk; `KEYS`' grace | during the collection | that node is not swept: its garbage stays until a later collection — never a loss (D-090) | `test-gc`, `e2e-gc.sh` |
| A collection cannot read a live manifest tree | the mark | before any `DROP` | nothing is dropped anywhere; the name is reported (D-090) | `test-gc`, `e2e-gc.sh` |

**What this system does not survive:** the loss of a majority of tracker replicas (names cannot be registered or
resolved until it is back); losing every replica of a chunk at once; and, on one host, a machine-wide stall longer than
the failure detector's patience (`BENCHMARKS.md` §4a). A tracker replica that loses its disk cannot be replaced
safely while the group runs: membership change is not built (D-079).

---

## Where it sits on the consistency/availability trade-off

**In two different places, on purpose** (D-014, D-015, D-040). CAP (Consistency, Availability,
Partition tolerance) only says something where there is more than one replica and a partition is
possible, so each plane is described separately.

- **Data plane — available, and conflict-free by construction.** Chunks are immutable and named by
  their own hash, so two replicas cannot disagree and there is no "latest version" for a read to
  miss; the quorum rule `W + R > RF` does not apply (D-014). `W` buys durability and `R` availability,
  independently: `W = 2` by default, so an acknowledged chunk is on two flushed disks; `R = 1`,
  because the reader verifies the hash itself. With one replica dead, `W = 2` acknowledged every write
  and `W = 3` refused 56 of 300 until the ring evicted it (`BENCHMARKS.md` §6).
- **Ring membership — consistency over availability for a node that has lost contact.** A node that
  loses every node it knew refuses to route rather than claim the whole ring (D-040): it cannot tell
  "everyone else died" from "I am cut off", and claiming the ring under a partition would build a
  second, disjoint ring.
- **Control plane — consistency over availability, by majority.** The namespace is replicated by Raft
  (D-069 – D-082): every registration and every read is linearizable, and the group serves while a majority of its
  replicas is up and connected. A minority — a cut-off leader included — refuses rather than answer: it cannot
  commit, and it cannot confirm a read (D-076, D-077).

**To move it:** `W` is per request (1, 2 or 3), so the data plane's durability/availability point is a
measured curve rather than a constant (D-046, D-053). Hinted handoff would raise write availability
without lowering `W` (named in D-015, not built). On the control plane the knob is the group's size: five replicas
would survive two failures (D-069).

---

## Reading the decision log

Each entry records the fork, the options with what each meant in practice, the choice, the rejected
alternatives and their cost, and — added as it arrived — the evidence. Entries are history: they keep
the reasoning as it was at the time, including predictions that later failed, and point forward to
the decision or measurement that settled them. Code paths in older entries name files as they were
then (`node.cpp`); since D-068 the sources are under `src/`.

| Phase | Dates | What was built | Decisions |
|---|---|---|---|
| 0 | 23 Aug – 9 Sep 2026 | the coursework system resurrected: build fixed, defects reproduced and fixed, the transfer baseline measured | D-001 – D-011 |
| 1 | 12 – 13 Sep 2026 | design only: the forks that shape everything after, decided before any code | D-012 – D-023 |
| 2 | 13 Sep – 7 Oct 2026 | routing on a fixed ring; the hop-count benchmark | D-024 – D-027 |
| 3 | 7 Oct 2026 | joins, stabilisation, failure handling; the reconvergence benchmark | D-028 – D-043 |
| 4 | 8 Oct 2026 | chunk storage, replication, virtual nodes, repair | D-044 – D-053 |
| 5 | 8 Oct 2026 | files through the ring; the tracker as a namespace; the transfer benchmark | D-054 – D-067 |
| 6 | 8 Oct 2026 | documentation and repository layout; no behaviour change | D-068 |
| 7 | 8 – 9 Oct 2026 | the replicated tracker: Raft, every fork decided before its code; the failover and write benchmarks | D-069 – D-082 |
| 8 | 9 Oct 2026 – | observability, deletion, anti-entropy and fault injection, every fork decided before its code | D-083 – D-093 |

---

## Open forks

**No fork is open.** All 85 recorded here (F1–F84 and F6a; F3a was decided inside D-014) are resolved, each by
the decision named in its last column. The rule while building was that nothing is implemented until
its fork is decided; the first five were held open until the Chord paper had been read, so that none
was decided by habit. The table is kept in the order the forks were raised.

| # | Fork | Why it changes things downstream | Status |
|---|---|---|---|
| **F1** | **Iterative or recursive lookup?** Iterative: I ask a peer, it replies "closer node is N3", I ask N3 myself. Recursive: I ask a peer and it forwards on my behalf, the answer comes back down the chain. | Iterative makes hop counting trivial and failures easy to attribute, at one round trip per hop. Recursive is lower latency but timeouts and partial failures get much harder — **and it costs the easy hop measurement the headline benchmark depends on.** | **RESOLVED** — D-012 |
| **F2** | **Does the tracker know where chunks are, or only what chunks exist?** | Manifest-only keeps the ring the single source of truth and the Raft log small. Tracker-holds-placement means one lookup instead of O(log N) hops — but creates **two systems that can now disagree** about where a chunk lives. | **RESOLVED** — D-017 |
| **F3** | **Replication — the consistency/availability knob.** Synchronous write to all three successors; or write-one-and-propagate; or quorum with W=2, R=2. | Sync-to-all: any replica is correct, writes as slow as the slowest successor (consistent + partition-tolerant). Write-one: fast writes, stale reads, **needs read repair**. Quorum: more to implement, much more to talk about. **This is the most consequential decision in the design.** | **RESOLVED** — D-014, D-015 |
| **F4** | **Who decides a peer is dead, and how long does it take?** Stabilisation period alone, or active heartbeats between successors. | Stabilisation alone is simplest and detection time is bounded by the period. Heartbeats detect faster but add background traffic and **force handling of a peer that is slow rather than dead** — which is the hard case. | **RESOLVED** — D-013 |
| **F5** | **Chunk size — what is a chunk, and why that number?** | Small chunks parallelise better and recover more cheaply but multiply lookups and metadata; large chunks mean fewer lookups but one slow peer dominates the transfer. **Pick a number now, then measure throughput at three sizes and let the plot justify it.** "512 KB because the assignment said so" and "64 KB because BitTorrent uses it" are both weak answers; a curve is a strong one. | **RESOLVED** — D-016 |

| **F6** | **The `update_seeder` desync — what should a peer do after it finishes downloading?** The client announces "I can seed this now" and never reads the reply; the tracker does not implement the command. Every reply after the first download is one behind. | This is not a typo, it is a missing piece of the protocol. Whatever is chosen sets the rule for **every** fire-and-forget message in the system — and the same shape recurs in D2's heartbeat. Deciding it once, deliberately, settles both. | **RESOLVED** — D-007 |
| **F6a** | **The heartbeat thread sends on the main loop's socket and ignores the reply**, so fixing F6 alone would re-create the desync every 30 seconds. | Sub-fork surfaced mid-implementation of F6, and decided on its own rather than in passing. | **RESOLVED** — D-008 |
| **F8** | **How does the tracker recover its state?** Replay re-ran every logged command through the live handler, which rejected all of them (defect R1). Options: a replay flag that makes the guards skip themselves; splitting admission from effect so recovery cannot reach a guard; or logging the effect rather than the request (event sourcing). | Sets whether the *class* of bug is avoided by remembering something or made impossible by structure — and the same question returns with a Raft-replicated tracker, which replicates log entries that other nodes must apply with no client attached. | **RESOLVED** — D-009 |
| **F9** | **How many lines is a reply?** Five tracker replies contain an embedded newline, so one command produces two lines and every later reply on that connection is one behind (defect R6). Options: fix the five strings; make the framing layer enforce the invariant; or length-prefix replies so the delimiter stops mattering. | Settles whether the rule lives in the framing layer or in every author's memory — and the same question returns on every new control message Chord adds. | **RESOLVED** — D-010 |
| **F10** | **What happens when the peer is gone?** Neither binary ignored `SIGPIPE`, so a `send` to a departed peer killed the process outright (defect R9). Options: ignore the signal process-wide; pass `MSG_NOSIGNAL` at every call site; or both. | Decides whether a remote party's ordinary behaviour can end this program — and the same question returns for every socket Chord adds. | **RESOLVED** — D-011 |
| **F7** | **What is on disk after a failed transfer?** Before Phase 5: a full-size, zero-filled file, indistinguishable from a real one by size. A temporary part file renamed into place; writing to the final name and deleting on failure; or holding the file in memory. | Sets whether the system is safe to use without reading its output carefully, and whether resumable downloads are possible later. | **RESOLVED** — D-055 |
| **F11** | **Is a ring member its own process, or is a participant one process?** A standalone `node` daemon with `client` as the originator outside the ring, or the existing `client` peer-server thread grown into the ring node. | Sets per-node resident set size, and therefore the **maximum ring size** that fits in 3.4 GiB — which is the x-axis of the Phase 2 hop-count plot. Also decides whether Phase 2 edits a working 1013-line transfer path. | **RESOLVED** — D-018 |
| **F12** | **How wide is the identifier space?** Full 160-bit SHA-1 with hand-written modular arithmetic, a 64-bit truncation using the machine word's own wrap-around, or 128 bits via a compiler extension. | Sets every data structure and every line of arithmetic in Phase 2 — and decides whether the code underneath the hop-count measurement is hand-written or free. | **RESOLVED** — D-019 |
| **F14** | **The node's wire protocol.** Where the framing code comes from, what a lookup carries, and whether a node may claim ownership from its predecessor. | Decides whether invariant-enforcing code exists once or three times, whether Phase 3 needs a second message type, and whether a second pointer becomes correctness-critical. | **RESOLVED** — D-021 |
| **F17** | **What is this system, in one line?** Phase 1's decisions turned a BitTorrent-style swarm into distributed storage. Options: say so; make "peer-to-peer" true again; or keep the wording and justify it in Chord's sense. | Decides whether the one-line description is accurate, and exposed a real alternative never weighed in Phase 1: trackerless BitTorrent. | **RESOLVED** — D-022 |
| **F18** | **Is deletion in scope, and when?** Unnaming a file, reclaiming unreferenced chunks, both, or neither. | Deduplication means deleting a file's chunks can destroy another file, and with no access control a delete command is a vandalism tool — so the obvious implementation is wrong twice. | **RESOLVED** — D-023: not in the MVP; owner-token unnaming plus a mark-and-sweep collector designed, not built |
| **F15** | **How is a node-identifier collision detected and recovered from?** Detect at join by asking `find_successor(my_id)` and comparing addresses, then re-hash with a salt; or detect at ring construction only; or rely on the birthday bound alone, as the Chord paper does. | Salting trades away the property that an identifier is a function of its address. | **RESOLVED** — D-028: refused at join, and never adopted |
| **F13** | **What does a node know when it starts, on a fixed ring with no joins?** Full membership as a construction input; successor only, with tables built by real lookups; or the harness computing every table offline. | Decides whether the hop-count plot measures routing alone or routing tangled with bootstrap — and whether total routing state is O(N log N) or O(N²), which bounds the ring size the plot can reach. | **RESOLVED** — D-020 |
| **F16** | **How far does the hop-count curve go?** Real processes only, capped by RAM; or real processes extended by an in-process simulation over the same pure functions, plotted together so they can be shown to agree where they overlap. | Hop count is a deterministic function of the routing tables, so a simulation can reach N = 10⁶. Logged rather than decided by implication. | **RESOLVED** — D-027 |
| **F19** | **The lookup originator.** (a) Where the loop lives; (b) connection handling across hops; (c) timeouts; (d) bounding a lookup that does not terminate; (e) what counts as a hop. | (a) decides whether the hostile cases can be tested without sockets; (b) decides how many failure signals one dead node produces; (e) shifts every point on the hop-count plot by one. | **RESOLVED** — D-024; part e in D-026 |
| **F20** | **Are the originator's socket timeouts set before or after `connect`?** Surfaced while building the lookup tool: D-024 fixed the values but not the order. | On Linux a blocking `connect` honours `SO_SNDTIMEO`, so the order decides whether a host that never answers the handshake costs 5 s or about 127 s. | **RESOLVED** — D-025 |
| **F21** | **How does a node start and join?** Joins only; joins plus the Phase 2 fixed ring kept as a mode; and whether a joining node listens before or after it has a successor. | Decides whether the reconvergence benchmark starts from a known state, whether the Phase 2 suites survive, and what a node restarted at its old address answers. | **RESOLVED** — D-029 |
| **F22** | **How is a node's routing state protected now that a background thread writes it?** One mutex; a reader-writer lock; or copy-on-write snapshots. | Phase 2 needed no lock because all writes preceded the first reader. Copying a string while it is assigned is undefined behaviour, so this is correctness, not tuning. | **RESOLVED** — D-030 |
| **F23** | **Which messages does stabilisation add, and what do they carry?** The paper's separate predecessor and successor-list queries, or one combined query; identifiers carried or recomputed; how a client's failure report reaches a node; how a harness reads fingers. | Sets round trips per period, whether a client can evict a node, and whether finger convergence can be observed at all. | **RESOLVED** — D-031 |
| **F24** | **How long is the successor list?** The replication minimum (3); log₂ of the largest ring measured (8); or estimated at run time from the gap to the successor. | r is the exponent in the probability of a node being orphaned, and it is the replica set Phase 4 reads. | **RESOLVED** — D-032 |
| **F25** | **Is there a graceful leave?** Crash-only; departure messages to the neighbours; or departure with a data hand-off. | Decides whether the system has one recovery path or two, and what a planned shutdown costs. | **RESOLVED** — D-033 |
| **F26** | **How does `fix_fingers` proceed?** One finger per round (the paper); all fingers every round; or one lookup per round whose answer fills every finger it covers. | Sets how long a dead finger survives and the permanent background load per node. | **RESOLVED** — D-034 |
| **F27** | **The failure-handling mechanics D-013 left open.** How many strikes; whether a candidate successor is verified before adoption; whether a surprising `NOTIFY` triggers a check; what replaces an evicted finger; what a node does when its whole successor list is gone. | Decides how fast a dead node leaves, whether it can be re-adopted from a neighbour's stale pointer, and whether an orphaned node claims the whole ring. | **RESOLVED** — D-035 |
| **F28** | **What does an originator do when it reaches a dead node?** Fail; retry silently; report to the node that named it, then retry; or resume with an exclusion. | Under iterative routing nodes never see a client's failures, so this decides whether D-013's detection adapts to load at all. | **RESOLVED** — D-036 |
| **F29** | **Does the 65-request cap survive stale fingers?** Keep it; raise it; or drop it and rely on the progress check. | D-024 derived 65 for correct fingers and asked Phase 3 to revisit it; stale fingers can need up to N requests. | **RESOLVED** — D-037 |
| **F30** | **What does "reconverged" mean, and who decides?** Successor pointers only; every piece of routing state; a lookup sweep; or nodes reporting it themselves. | It is the definition behind the Phase 3 headline number, and decides whether the measuring instrument sits outside the thing measured. | **RESOLVED** — D-038 |
| **F31** | **What are the stabilisation period T and the node's call deadline, and how are they swept?** Precedent; a multiple of each other; or measured curves with selection rules fixed before measuring. | T sets every reconvergence number twice over, and the deadline decides whether a slow node is evicted while alive. | **RESOLVED** — D-039 (rules); values from `BENCHMARKS.md` §4 |
| **F32** | **What does a node do when every node it knew has gone?** Fall back to itself; stay orphaned; or stay orphaned and rejoin through remembered addresses. Surfaced by the churn tests, not foreseen. | Falling back to itself claims the whole ring and can recruit joiners into a second ring that stabilisation cannot merge. | **RESOLVED** — D-040 |
| **F33** | **The deadline rule had no solution.** Its named load (256 nodes at 50 ms) saturated the machine, and the slowest exchange there needs D ≥ 3.46 s. Use the largest candidate; add larger ones; drop 50 ms; or apply the rule to the loads the benchmark actually runs. | Decides the call deadline, and whether a rule fixed before measuring may be narrowed after it fails. | **RESOLVED** — D-041 |
| **F34** | **Both selection rules gave different answers when the benchmark was repeated.** The period rule's 1 s median target sits on T = 2000 ms's own median; the deadline rule keyed on the single slowest of ~70,000 samples. Keep them with more runs; pick between runs; instrument the nodes; or restate them in statistics that repeat. | Decides T and D, and whether the numbers behind them are reproducible. | **RESOLVED** — D-042 |
| **F35** | **How fast does stabilise walk back after a distant fallback?** One step per round (the paper); run to the fixed point within a round; rejoin as an orphan; or a longer list. Surfaced by a 256-node ring that took minutes to converge. | Sets recovery after more than r adjacent deaths, and how long a wired ring takes to start. | **RESOLVED** — D-043 |
| **F36** | **How does a chunk travel on the wire?** A length-prefixed body after a control line; the chunk encoded inside the line; or a separate data connection. | Decides whether a peer can size an allocation, how a short read is handled, and whether one socket carries two framing schemes. | **RESOLVED** — D-044 |
| **F37** | **How and where is a chunk kept on disk?** One file per chunk with a temporary name and an atomic rename; an append-only log; or memory only — and whether a write is flushed before it counts. | Decides what a crash mid-write leaves behind, what an acknowledged write promises, and whether a peer's key can name a path. | **RESOLVED** — D-045 |
| **F38** | **Who replicates a write, and where does W live?** The owner sending in parallel; the client writing every replica; or a chain — and W per node or per request. | Decides write latency (the slower of two parallel sends or the sum of a chain), who uploads the chunk how many times, and whether W values can be compared on one ring. | **RESOLVED** — D-046 |
| **F39** | **How do virtual nodes share a process?** One listening port per member; one port with requests addressed by identifier; one shared routing table per process; or full membership. | Decides whether the wire can say which members share a process (replica placement needs it), what a refused connection proves, and the background traffic per process. | **RESOLVED** — D-047 |
| **F40** | **Which members hold a chunk's replicas when one process has several members?** The next members regardless of process; the first distinct processes clockwise; a separate replica list; or a longer successor list. | With virtual nodes, adjacent members often share a process, so the naive rule puts two copies where one crash destroys both. | **RESOLVED** — D-048 |
| **F41** | **How does a read find a good copy?** One replica, checked locally, falling through; two replicas compared; all replicas at once; and where the list of replicas comes from when the owner is dead. | Decides read traffic, what happens in the window after an owner dies, and whether a quorum is needed at all. | **RESOLVED** — D-049 |
| **F42** | **How is RF = 3 restored after a death, and who is responsible?** A periodic full scan; an event when the successor list changes; both; every replica checking every chunk; or hash-tree comparison. | Decides how long a chunk stays one copy short, how much background traffic repair costs, and whether a silently lost file is ever found. | **RESOLVED** — D-050 |
| **F43** | **How many virtual nodes per process?** One; a large fixed number such as 256; a number read off a measured balance curve by a rule fixed first; or in proportion to capacity. | Load balance improves as 1/√V while every member adds its own stabilisation traffic. | **RESOLVED** — D-051 (rule); value in `BENCHMARKS.md` |
| **F44** | **How much of a chunk is in memory, and is it bounded?** The whole body, shared between parallel sends, capped per process; streamed to disk while hashing; uncapped; or a waiting queue. | Sets memory per node under write load, whether a client can make a node allocate without limit, and how many processes fit on this machine. | **RESOLVED** — D-052 |
| **F45** | **How are W, durability and re-replication measured?** A fresh ring per W, or all W on one ring under one failure; durability from copy counts, or by killing the coordinator at acknowledgement; and who judges that RF = 3 is restored. | Decides whether the W comparison is between settings or between rings, and whether a durability claim was tested or inferred. | **RESOLVED** — D-053 |
| **F46** | **What is a file's manifest, and what happens when it does not fit in one chunk?** A text manifest, split into a tree of manifests when large; a binary manifest; one flat manifest and a file-size limit; or a chain of manifests. | Decides the largest file the system can hold, how many round trips a download makes before its chunks can be fetched in parallel, and whether a manifest can be read and checked by hand. | **RESOLVED** — D-054 |
| **F47** | **How is a transfer parallelised?** A fixed pool of workers taking chunk numbers from one counter, sized at run time; a thread per chunk; an event loop; or chunks batched by owner. And what does a transfer do when one chunk fails? | Sets throughput, the memory a transfer uses, how many connections land on a node at once, and whether throughput against parallelism can be measured as a curve. | **RESOLVED** — D-056 |
| **F48** | **How does a client find the ring?** Addresses on its command line, tried in order; addresses handed out by the tracker; a seed file; or learning the membership from a first contact. | Decides whether the tracker carries liveness state about nodes, whether a client can read a file with the tracker down, and where every lookup's first request lands. | **RESOLVED** — D-057 |
| **F49** | **What does the tracker become?** The name-to-manifest map alone, with an effect log; the legacy commands kept beside it; or a new program beside the legacy tracker. And what happens to users, groups, `update_seeder` and the persistence log format (D-009)? | Decides the size of the mutable state that consensus will later replicate, what recovery replays, and how much legacy machinery stays alive. | **RESOLVED** — D-058 |
| **F50** | **What happens when a name is registered twice?** Refuse a different key and accept the same one again; the last writer wins; keep versions; or overwrite on request. | With no access control and no deletion, decides whether one user can replace another's file, and whether a retried upload succeeds. | **RESOLVED** — D-059 |
| **F51** | **What happens to the legacy client and tracker code, their framing copies, and the suites that test them?** Rebuild both programs on the node's framing and remove the legacy path, re-pointing its suites; bolt the ring path onto the legacy client; or keep the legacy pair beside the new one. | Decides whether the framing rules exist once or three times (D-021), whether the legacy suites still test anything, and whether the Phase 0 baseline can be re-run. | **RESOLVED** — D-060 |
| **F52** | **How long may a peer take to send what it announced?** A deadline per request line and per body, enforced before every receive; a per-system-call timeout; an event loop with timers; or nothing. | A peer that announces a chunk and never sends it holds a memory slot and a thread; with the chunk path user-facing, any client can do it. | **RESOLVED** — D-061 |
| **F53** | **How is deduplication measured, and on what?** Counted at the stores, on stated synthetic edits plus one real corpus; reported by the client; a real corpus only; or not measured. | D-014 claims deduplication for free; the number decides whether that claim survives, and the workload decides whether the number means anything. | **RESOLVED** — D-062 |
| **F54** | **How is the chunk-size sweep run above the 512 KiB framing limit?** A second build with a raised limit, used for every point; a run-time limit on every program; a sweep only up to 512 KiB; or a raised default. | D-016 promised five sizes up to 8 MiB, and the framing and stores refuse anything above 512 KiB (D-044). | **RESOLVED** — D-063 |
| **F55** | **How does someone else start a ring, given that Docker is not installed on the development machine?** A tested local launcher plus container files marked not yet run; container files only; nothing until Docker is installed; or installing Docker now. | Decides what "how would someone run this?" can honestly be answered with, and what is claimed about containers. | **RESOLVED** — D-064 |
| **F56** | **What does continuous integration run?** The build, the unit tests and the fast end-to-end suites on every push, with the timing-sensitive suites on demand; unit tests only; everything on every push; or none. | Decides what a green check means, and whether a flaky check teaches everyone to ignore it. | **RESOLVED** — D-065 |
| **F57** | **How is the transfer path measured against the Phase 0 baseline?** Against a same-day re-run of the baseline at its own commit, with the clean single-variable comparison (one worker against many) beside it; or against the number recorded in September alone. And what are the curves? | The before/after is the phase's headline; moving several variables at once, or comparing across machine changes, would make it unattributable. | **RESOLVED** — D-066 |
| **F58** | **An orphaned ring member is named as successor by its own sibling, which never evicts it, so it can never rejoin. What breaks the cycle?** An `ERR orphaned` reply evicts that member at once, siblings included; the orphan copies its predecessor's list in place; wired nodes wait for every peer before their first round; or fall back to a sibling instead of orphaning. Found by the Phase 5 benchmark, not foreseen. | A ring started all at once, or a process that loses every other process it knew, can stay wrong for good — every lookup through that sibling ends at a member that refuses to route. | **RESOLVED** — D-067 |
| **F59** | **Where do sources, tests, objects and programs live?** Sources in `src/` and unit tests in `tests/`, or the flat layout kept; and where the build puts objects and programs. | Decides whether the repository reads at a glance, and whether every script, the container files and the documentation keep their paths. | **RESOLVED** — D-068 |
| **F60** | **How many tracker replicas?** Two; three; five; or primary–backup without consensus. | Sets how many failures the namespace survives, what each write waits for, and on one host how much of the cost is contention. | **RESOLVED** — D-069 |
| **F61** | **Where does the Raft protocol live?** In the server, among threads and sockets; in a socket-free core with time, storage and messages passed in; or in an external library. | Decides whether message loss, reordering and a crash between any two steps can be constructed and replayed in a test. | **RESOLVED** — D-070 |
| **F62** | **How do replicas talk?** A fresh connection per call; a persistent connection per pair; UDP; and on which port, with how many calls in flight. | Decides what a lost message is, whether a reply can be mistaken for another's, and how replication batches. | **RESOLVED** — D-071 |
| **F63** | **The replica's concurrency.** One lock held across the flush but never a network call; the lock released during the flush; or an event loop. And who sends to each peer. | Decides whether "flush before replying" holds by construction, and what a slow disk or a frozen peer delays. | **RESOLVED** — D-072 |
| **F64** | **Election timeout, heartbeat and call deadline.** The paper's example values; a large constant; or one base swept, with the default chosen by a rule fixed before measuring. | Sets failover time and whether a healthy group elects for no reason. | **RESOLVED** — D-073 (rule); value from `BENCHMARKS.md` §7 |
| **F65** | **How does a replica persist its term, vote and log?** One file of self-checking records; a separate file for term and vote; and what replay does with damage. | Decides what survives a crash, how many flushes a step costs, and whether damage is cut, skipped or refused. | **RESOLVED** — D-074 |
| **F66** | **When is a registration acknowledged, and what makes a retry safe?** After its own entry is applied, with no sessions; client sessions; at commit; or from local state. | Decides what `OK` promises across a leader change, and whether a retried registration can apply twice. | **RESOLVED** — D-075 |
| **F67** | **How are reads made linearizable?** Through the log; ReadIndex; leader leases; or follower reads of found names. | Decides what a read costs and whether a deposed or paused leader can answer stale. | **RESOLVED** — D-076 |
| **F68** | **Does a leader notice it is cut off, and does a returning replica disrupt the group?** Neither; check-quorum; check-quorum and pre-vote. | Decides what clients of a cut-off leader see, and whether a healed partition costs an election. | **RESOLVED** — D-077 |
| **F69** | **Snapshots and log compaction?** Snapshots with `InstallSnapshot`; local compaction; or none. | Decides restart and catch-up time, and whether there is a second replication path. | **RESOLVED** — D-078 |
| **F70** | **How does the group's membership change?** Fixed, with a fingerprint; joint consensus; or single-server changes. | Decides whether a replica can be replaced live, and what a configuration mistake breaks. | **RESOLVED** — D-079 |
| **F71** | **What happens to the single tracker?** Kept as a mode beside the group; a one-replica group; or its log kept under Raft. | Decides whether the existing suites test the replicated code, and how many persistence formats exist. | **RESOLVED** — D-080 |
| **F72** | **How does a client find the leader?** Every address on its command line with redirects; discovery from one address; followers forwarding; or DNS. | Decides what a client must be told, and what it does during an election. | **RESOLVED** — D-081 |
| **F73** | **Flush per proposal, or batch?** Measure the direct path first; group commit on the leader; or the leader flushing in parallel with replicating. | Decides write throughput, and whether the optimisation is chosen from a number. | **RESOLVED** — D-082: measure first |
| **F74** | **How does each process expose its metrics?** A hand-written HTTP/1.0 responder on a separate metrics port, in the Prometheus text format; a `METRICS` line on the protocol port, converted by an exporter; pushing them (StatsD, a Pushgateway); or a metrics library. | Decides whether Prometheus can scrape a process directly, whether a scrape competes with the data path for threads, and whether a dead process is told apart from a quiet one. | **RESOLVED** — D-083 |
| **F75** | **Which metrics, and as which instrument?** Histograms with fixed buckets for latencies and hop counts, counters and gauges for the rest, labels only from small fixed sets; summaries computing quantiles in the process; averages; or metrics derived from the logs. | Decides whether a p99 can be computed across processes, what one scrape costs, and whether a label can grow without bound. | **RESOLVED** — D-084 |
| **F76** | **A client runs for seconds; Prometheus scrapes every few. Where do transfer numbers come from?** The client serves metrics while it runs, and the dashboard's throughput and latency are measured at the nodes; a Pushgateway; a metrics file written at exit; or a long-running client. | Decides whether the dashboard's headline panels exist at all for short transfers, and what one more daemon would cost. | **RESOLVED** — D-085 |
| **F77** | **How does a process log?** A small hand-written logger writing one `key=value` line per event; `spdlog`; JSON lines; or the free-text lines kept. | Decides whether a log line can be parsed by a tool, whether lines from two threads can interleave, and whether the no-external-libraries rule holds. | **RESOLVED** — D-086 |
| **F78** | **How does one transfer's request id reach every process it touches?** A `RID <id>` prefix on every request line, carried on into a server's onward calls; a trailing field; a separate header line; or deriving it from the chunk key. | Decides whether one upload can be reconstructed from the logs of the client, the tracker and every node, and whether the framing's rules survive the change. | **RESOLVED** — D-087 |
| **F79** | **Prometheus and Grafana without Docker.** Configuration and a dashboard in the repository, run against the local cluster with the official release binaries downloaded outside it; configuration only; or waiting for Docker. | Decides whether the dashboard is a screenshot of a running system or a description of one. | **RESOLVED** — D-088 |
| **F80** | **Who may delete a name, and how is it proved?** A secret chosen by the uploader, of which the tracker keeps only a hash; a token the tracker generates and returns; user accounts; or anyone. | Decides whether delete is a vandalism tool, whether a retried registration stays safe without sessions (D-075), and whether a secret ever sits in the Raft log. | **RESOLVED** — D-089 |
| **F81** | **Who collects unreferenced chunks, and how does it learn what is live?** A separate collector that reads every name, walks every manifest tree through verified reads, lists each node's chunks and asks it to drop the rest; the Raft leader doing it; every node marking for itself; or nodes deleting whatever is not in a live set they are sent. | Decides which component may destroy data, what one bogus name or one unreadable manifest does to collection, and how much work a collection is. | **RESOLVED** — D-090 |
| **F82** | **An upload has stored its chunks but not yet registered its name, and the collector runs. What stops it deleting them?** A grace period measured by each node's monotonic clock, renewed whenever a chunk is written again, with a restart counting as a write; generations with upload leases through Raft; a pin per chunk; or stopping uploads during a collection. | The race that turns garbage collection into data loss — made worse by deduplication, where an upload's chunk may be an old orphan the collector already marked. | **RESOLVED** — D-091 |
| **F83** | **Who may tell a node to drop a chunk?** A shared key held by the nodes and the collector; anyone; a signed request with a nonce; or the node checking reachability itself. | `DROP` is the first request on the data plane that destroys anything; without access control any process that can reach a node could delete old chunks. | **RESOLVED** — D-092 |
| **F84** | **What do read repair and anti-entropy mean when replicas cannot disagree?** Record them as subsumed and measure only the window with fewer than three copies; compare a digest of each arc's keys with each replica before listing them; Merkle trees kept by every node; or read repair of *missing* copies by the reader. | Decides whether repair's periodic check costs a message per twenty chunks or one per arc, and what "the staleness window" means for content-addressed data. | **RESOLVED** — D-093 |

---

## Historical: the design as first drawn (12 Sep 2026)

*Not the current system — see "The system as it exists today" above.* This diagram was drawn in
Phase 1, after the first forks were resolved and before any Chord code existed: a diagram made
earlier would have decided them by implication. It is kept as the record of what was designed.
Since then: the Raft group behind the tracker was built in Phase 7 (D-069 – D-082); each node is a process with 8 ring
members (D-047) and a chunk's copies are on distinct processes (D-048); stabilisation asks
`NEIGHBOURS` and runs to its fixed point (D-031, D-043); finger repair fills every finger one lookup
covers (D-034); and the tracker holds names written once, with an effect log (D-058, D-059).

```mermaid
flowchart TB
    C["<b>client</b><br/>drives every hop itself — iterative, D-012<br/>verifies every chunk against the key it asked for"]

    subgraph control["CONTROL PLANE — small, mutable, consensus-backed"]
        T["<b>tracker</b><br/>filename → manifest hash · ~40 bytes per file<br/>D-017 · a Raft group is the planned next step"]
    end

    subgraph ring["DATA PLANE — Chord ring · immutable · content-addressed, D-014"]
        direction LR
        N8["<b>N8</b>"] --> N21["<b>N21</b>"] --> N32["<b>N32</b>"] --> N48["<b>N48</b>"] --> N8
    end

    C -- "1 · name" --> T
    T -- "2 · manifest hash" --> C
    C -- "3 · find_successor(hash) — one step, one answer" --> N8
    N8 -- "4 · 'closer node is N32'" --> C
    C -- "5 · client asks N32 itself" --> N32
    N32 -- "6 · 'N48 owns it'" --> C
    C -- "7 · GET &lt;manifest hash&gt;" --> N48
    N48 -- "8 · manifest = [h0 h1 … hk]" --> C
    C -- "9 · repeat 3–8 concurrently for every chunk hash" --> ring
```

The arrows inside the ring are **successor pointers**. They are the only routing state that has
to be correct; everything else is an accelerator.

**Reading the path.** Steps 1–2 are the only time the tracker is involved, and it returns 40
bytes. Steps 3–6 are the iterative lookup: each node answers *one* question and returns
immediately, and the **client** opens the next connection — which is what makes hop count a
number the client observes rather than one the ring reports. Steps 7–8 fetch the manifest, which
is itself just a chunk. Step 9 repeats the whole lookup-and-fetch for every chunk hash, issued
**concurrently**, because content addressing means consecutive chunks of one file live on
unrelated nodes and there is no locality to walk.

**What each node holds**, in strict order of how much it matters:

| Structure | Carries | If stale or wrong |
|---|---|---|
| **successor pointer** | correctness | wrong answers, returned confidently, undetectable |
| **successor list** (`r ≈ log₂N`) | survival of failure — **and it is the replica set** | node orphaned only if *all* `r` die within one period |
| **finger table** (`m` rows, ~`log₂N` distinct) | speed only | more hops, never a wrong answer |
| **predecessor** | knowing its own arc; joins | joins stall until repaired |
| **chunk store** | the data, keyed by `SHA-1` of its own contents | a failed hash check at the reader; try the next replica |

**What runs in the background on every node**, forever: `stabilize` every `T` seconds (asks the
successor for its predecessor, tightens the pointer, `notify`s), `fix_fingers` (refreshes one
finger per round), and `check_predecessor`. Failure detection rides on these plus **any** failed
call to the successor from any code path (D-013). Nothing is event-driven; everything is
re-derived, so the ring converges from any state.

**What this diagram deliberately does not show**, because it did not exist yet: the Raft group
behind the tracker (built in Phase 7). Since Phase 4 each node in the drawing is a
process with several ring members — virtual nodes, D-047 — and a chunk's three copies are on three different
processes (D-048).

---

## Decision log

The heart of this file. One entry per resolved fork. Every entry names the rejected alternative
and its cost — an entry without one is not finished, because a decision is only understood once
the alternatives and their costs are.

### D-001 — Repository named `chord-dht-filesystem`
**Date:** 23 Aug 2026 · **Phase:** 0 · **Commit:** none — the repository was renamed when it moved to its own remote, 6 Sep 2026

**The fork:** the repository was called `os-assignment3`, a name that describes the course it came
from rather than the system it was becoming.

**Options considered:**

| Option | What it means in practice | Cost |
|---|---|---|
| `chord-dht-filesystem` | Names the algorithm up front | Invites "why Chord and not Kademlia?" — a comparison the design is ready to make |
| `distributed-p2p-filestore` | Names the domain, not the algorithm | Safer if the design drifts from strict Chord, but does not signal that a *named protocol* was implemented |
| `chord-raft-filesystem` | Names both planes | Most descriptive of the plan — but **promises Raft, the component most likely to be deferred.** If Raft is not built the name claims something the repository does not contain |
| `ringfs` | Short, product-like | Says nothing about the algorithm; "Chord" and "DHT" are what someone searching for this would type |

**Chosen:** `chord-dht-filesystem`

**Reasoning:** it states the scope in three words and commits only to what the minimum viable
product guarantees. The Kademlia comparison it invites is a useful one — XOR metric versus
successor-ring topology.

**Rejected because:** `chord-raft-filesystem` names a component that might not be built. A
repository name is not the place to promise the most deferrable feature.

**What would change my mind:** if Raft ships and is solid, renaming is one command and the
history survives.

**Evidence:** none — naming, decided on reasoning.

---

### D-002 — Phase 0 is a minimum viable resurrection, not a full repair
**Date:** 23 Aug 2026 · **Phase:** 0 · **Commit:** `b6620aa`

**The fork:** the inherited system has at least fifteen identified defects (the audit of
23 Aug 2026; the ones reproduced are in `docs/failures.md`). How many get fixed before Chord work
starts?

**Options considered:**

| Option | What it means in practice | Cost |
|---|---|---|
| Minimum viable resurrection | Fix the build (B1, B2), fix state replay (R1), fix the download path (R2). Stop when one file transfers end-to-end and verifies. | ~6–8 h. Leaves D1–D8 open — but most are in code the Chord rewrite deletes anyway |
| Full repair | Also restore multi-tracker sync, fix the desync properly, close the traversal hole, make downloads non-blocking | ~14–16 h. **Roughly doubles the time before Chord routing work can start** |
| Build-only, then straight to Chord | Fix compilation, write the postmortem, move on | ~2–3 h. But with no working download there is **no baseline throughput number**, and every later improvement needs a before number to be measured against |

**Chosen:** minimum viable resurrection.

**Reasoning:** the point of Phase 0 is not a good legacy system — it is (a) a working,
debugged path whose failures are understood and (b) a **before** number that every later
benchmark is measured against. Both need the download path working. Neither needs multi-tracker sync.

**Rejected because:** full repair spends 8 extra hours hardening code that Phase 5 rewrites.
Build-only saves 4 hours and forfeits the baseline, which is the more expensive loss.

**What would change my mind:** if isolating R2 turns out to need the desync fixed first, D2
gets promoted into Phase 0.

**Evidence:** `BENCHMARKS.md` §1 (measured 9 Sep 2026 at `e381179`), the deliverable of this decision.

---

### D-003 — The uncommitted November tree is committed as-is, then fixed forward
**Date:** 23 Aug 2026 · **Phase:** 0 · **Commit:** `cd10483`, tagged `pre-resurrection`

**The fork:** 2,247 uncommitted lines sat in the working tree. They fixed two compile errors
(B3, B4) **and regressed three features** that existed at HEAD — `stop_share`, `update_seeder`
and `peer_reconnect_thread`.

**Options considered:**

| Option | What it means in practice | Cost |
|---|---|---|
| Commit as-is first | One honest `wip:` commit naming both what it fixed and what it regressed, then fix forward | An ugly commit sits in the history permanently |
| Reconstruct, then commit clean | Restore the three deleted functions from HEAD first, so one tidy commit lands | Reads better as a diff, but it is **a story about November written in August**, and it hides that the regression happened |
| Discard, start from HEAD | — | Not viable: HEAD does not compile at all, and it throws away real work |

**Chosen:** commit as-is, tagged `pre-resurrection`.

**Reasoning:** history is evidence. An ugly-but-true commit is worth more than a tidy invented
one, and the regression is itself part of the record — a working tree that had silently dropped
three handlers is a concrete example of why uncommitted work is a liability.

**Rejected because:** reconstructing would have made the diff look deliberate when it was not,
and the history would then record something that did not happen.

**What would change my mind:** nothing. This is settled.

**Evidence:** none — process decision.

---

### D-004 — Users, groups and authentication are dropped
**Date:** 23 Aug 2026 · **Phase:** 0 · **Executed:** Phase 5, `83d2a3b` (D-058, D-060) — **after** the baseline was measured at `e381179`

**The fork:** the inherited system has user accounts, passwords, groups with owners, join
requests and an approval workflow. Does that survive the pivot to Chord?

**Options considered:**

| Option | What it means in practice | Cost |
|---|---|---|
| Drop groups and auth | The ring stores content-addressed chunks; the tracker owns the manifest | Deletes working code — but it stays in git history, so nothing is actually lost |
| Keep both, Chord underneath | Preserves working code, the pivot looks continuous | **Every** new subsystem — replication, stabilisation, read repair, anti-entropy — must respect group membership. Four subsystems taxed by a concern outside the system's purpose |
| Keep auth, drop groups | Auth is cheap and answers "how do you stop anyone joining the ring?" | One extra concept to maintain that the Chord literature has no opinion about |

**Chosen:** drop both.

**Reasoning:** they were a coursework requirement, not a distributed-systems one. Removing them
makes the DHT literature map straight onto the code, and shrinks the surface that has to be
kept correct.

**Rejected because:** keeping them costs hours in four separate subsystems for a feature that
does not bear on routing, replication or failure handling.

**⚠ Sequencing constraint:** **this removal happens after the Phase 0 baseline is recorded, not
before.** The download path runs through `upload_file`, which checks group membership; deleting
that code first would destroy the "before" number that D-002 exists to produce.

**What would change my mind:** if the system's purpose became access control rather than
distributed storage.

**Evidence:** none — scoping decision.

---

### D-005 — Fix the build with the smallest change that is *true*, not the smallest change that works
**Date:** 1 Sep 2026 · **Phase:** 0 · **Commit:** this session

**The fork:** `client` failed to link with `undefined reference to SHA1`. Where does `-lcrypto` go?

| Option | What it means in practice | Cost |
|---|---|---|
| Add `-lcrypto` to `CXXFLAGS` | One line, works immediately, both binaries get it | Links a cryptography library into `tracker`, which hashes nothing today. Also conflates compile-time flags with link-time libraries, which is how a `Makefile` stops being readable |
| A separate `CRYPTO_LDLIBS`, applied to the `client` rule only | Names the dependency where it is real | Two lines instead of one, and it must be revisited when the tracker starts hashing |
| Move `sha1.h` to a real `sha1.cpp` and build `sha1.o` | Makes the original `Makefile` correct rather than deleting its claim | Solves a problem that does not exist — the header is genuinely header-only, and this adds a translation unit to justify a line that was simply wrong |

**Chosen:** a separate `CRYPTO_LDLIBS` on the `client` rule.

**Reasoning:** the `Makefile` should state what is true. `tracker.cpp` does not include `sha1.h`
and links clean without `-lcrypto` — verified, not assumed. The tracker *will* need SHA-1 in
Phase 2, when node identifiers and keys are hashed onto the ring; the flag gets added to its
rule then, when it is true. A dependency added early "because we'll need it" is a dependency
nobody can later explain.

**Rejected because:** the global-`CXXFLAGS` version is indistinguishable from not having
thought about it, and a tracker that links OpenSSL without using it has no reason to.

**What would change my mind:** the moment `tracker.cpp` hashes anything, which is Phase 2.

**Evidence:** `docs/failures.md` B1, B2. `make clean && make` builds both binaries with zero warnings.

---

### D-006 — A failing test is committed on purpose
**Date:** 1 Sep 2026 · **Phase:** 0 · **Commit:** this session

**The fork:** `scripts/e2e-edge.sh` sweeps file sizes across the piece boundary and currently
fails six of seven cases (only the 1-byte file passes, re-checked by running the suite at `24bdf2d`), because it found defects R3 and R4 which are not yet fixed. Does a
failing test get committed?

| Option | What it means in practice | Cost |
|---|---|---|
| Commit it failing, with the expectation documented in the script header | The known defect is pinned by something executable. Anyone who fixes R3 sees the row turn green | A red test in the repository, which looks like carelessness to someone who does not read the header |
| Hold it back until R3 and R4 are fixed | The repository is always green | The defect exists only as prose until then, and prose does not detect a regression |
| Mark the failing cases as skipped | Green, and the cases are still there | A skipped test is a test nobody looks at. The failure *is* the information |

**Chosen:** commit it failing, with the expectation stated in the script header and in
`docs/failures.md`.

**Reasoning:** the sweep is what found R3, and R3 is invisible to any test that transfers one
file. Its value is precisely that it is red. The output is also self-diagnosing — the one-row
shift in the size column names the defect without reading any code.

**Rejected because:** holding it back means the only record of R3 is a paragraph, and a
paragraph cannot tell you when the bug comes back.

**What would change my mind:** if the repository ever gets continuous integration that gates
merges, this moves behind an explicit `expected-failure` marker rather than a plain red.

**Evidence:** `docs/failures.md` R3, R4.

---

### D-007 — Every request gets exactly one response, and it is read before the next is sent
**Date:** 2 Sep 2026 · **Phase:** 0 · **Fork:** F6

**The fork:** after a download completed the client sent `update_seeder` and never read
the reply. The tracker did not implement the command, answered `Unknown command`, and that
reply sat in the buffer — every later response was one behind, for the life of the connection
(defect R3).

| Option | What it means in practice | Cost |
|---|---|---|
| Implement it and read the reply | Tracker gains a real handler; client consumes the response | One round trip per completed download — **per file, not per piece**, so noise against a transfer that just moved megabytes |
| Delete the send | One line, desync gone | A peer that finishes downloading never announces it can seed. **Every future download still comes from the original uploader and the swarm never grows** — which removes the property that makes peer-to-peer worth building |
| One-way notification, tracker never replies | Fastest, no round trip | The protocol now has two classes of message. Anyone who later adds a reply to that command silently re-breaks every subsequent response |

**Chosen:** implement it and read the reply.

**Reasoning:** it keeps **one universal invariant** — every request is followed by exactly one
response, read before the next request is sent. A rule with no exceptions is one that cannot be
got wrong by the next person to touch the protocol. The measured cost is one round trip per
completed file.

**Rejected because:** deleting the send is cheapest and guts the swarm. The one-way variant is
fastest and buys that speed by introducing a special case, which is exactly the shape of the
bug being fixed.

**What would change my mind:** if announcements ever became high-frequency — per piece rather
than per file — the round trip would start to matter and a batched or one-way channel would
earn its complexity.

**Evidence:** `scripts/e2e-edge.sh` — 6/7 cases pass, up from 1/7; the one-row size shift that
was R3's signature is gone. Seeder set verified to grow: `alice@...:6881` before bob's
download, `bob@...:6882 alice@...:6881` after.

---

### D-008 — The announcer owns its own connection; sharing is removed rather than guarded
**Date:** 2 Sep 2026 · **Phase:** 0 · **Fork:** F6a

**The fork:** surfaced while implementing D-007 and stopped for, rather than decided in
passing. The heartbeat thread wrote `update_seeder` down the **main loop's** socket and
ignored the reply (defect D2). Fixing F6 without touching it would have re-created the desync
on a 30-second timer.

Two facts found while investigating, both of which changed the options:

- `sock_mtx` was taken in **exactly one place** — the heartbeat's `send`. The main loop's
  send/recv pair took no lock at all. The lock everyone assumed was protecting the socket was
  serialising the heartbeat against nothing.
- There is **no `seeders.erase` anywhere** in the tracker. Nothing ever removes a seeder, so
  re-announcing achieved nothing. A heartbeat with no timeout on the receiving side is not a
  heartbeat; it is traffic.

| Option | What it means in practice | Cost |
|---|---|---|
| Widen `sock_mtx` to span send *and* receive | The lock finally covers the real transaction | The heartbeat briefly blocks the main loop, and one socket is still shared between two threads — the weaker shape |
| Delete the heartbeat until expiry exists | Removes code that has no effect today | Nothing re-populates the seeder set after a tracker restart; the feature returns with F4 |
| **Give the announcer its own connection** | One thread, one conversation, no sharing | An extra connection and socket per client |

**Chosen:** its own connection.

**Reasoning:** the invariant that broke is *send-then-receive as a pair*. **A mutex protects
state; this is a rule about sequence**, and no lock expresses it. Two threads cannot share one
request/response conversation regardless of locking, because thread A can consume thread B's
response. Removing the sharing makes the invariant hold by construction rather than by
discipline.

**Rejected because:** widening the lock makes the bug unreachable while leaving the bad shape
in place, and it is the fix that looks correct in review while still being one careless commit
away from breaking. Deleting the heartbeat is reasonable today but throws away the mechanism
F4 needs.

**Implementation notes:**

- The announcer **reads and discards the tracker's `TRACKERS` greeting** on connect. Leaving it
  would put the new connection one reply behind from its first request — the same bug, on the
  connection built to avoid it.
- It waits on a `condition_variable` with a **predicate over a generation counter**, not a bare
  `wait_for`. A bare wait would (a) miss a notification that arrives before the first wait and
  (b) treat a spurious wake-up as a real change. The counter makes both correct.
- It **copies the seed list under the lock and releases before doing network I/O**. Holding
  `seeding_mtx` across a blocking `send`/`recv` would stall the main loop on every upload.
- `update_seeder` is **not written to the update log**. Which peer currently holds a file is
  **soft state** — true only while that peer is alive. Replaying it at startup would resurrect
  peers that are long gone. Durable state is "this file exists and here is its manifest".

**Known limitation, stated rather than hidden:** the announcer does not reconnect if its
connection drops; the peer then stops being re-advertised. Acceptable while nothing expires
seeders — it becomes real work the moment F4 lands.

**Evidence:** no benchmark; verified functionally on three consecutive runs. (The coursework
transfer path this applied to was retired in Phase 5, D-060.)

---

### D-009 — Admission and effect are separate; recovery can only reach the effect

**Fork:** F8. **Date:** 6 Sep 2026. **Closes:** defect R1.

**The decision.** Every mutating command is split into an `apply_*` function that performs the
state transition and nothing else, and a set of admission checks that stay on the live path in
`handle_command`. Recovery has its own entry point, `replay_command`, which calls `apply_*`
directly and never calls `handle_command`.

**What the split means in practice.** A check belongs on the live path if it asks about the
*requester* or about *soft state* — "are you who you say you are?", "are you logged in?". A
check belongs in `apply_*` if it asks about *durable state* — "does this group exist?", "is
this user a member?" Those are deterministic: they were true when the command was accepted, so
they are true again when the log is replayed in the same order.

**Why this and not the smaller change.** The obvious repair is a `replaying` flag that makes
the guards skip themselves. It is twenty minutes of work and it leaves the guards in the
recovery path, disabled by a boolean. Every guard added later is a fresh opportunity to forget
the flag, and the failure is silent — which is exactly how R1 survived from November to
September without anyone noticing. Splitting the functions removes the possibility rather than
the symptom: recovery cannot skip a guard it has no code path to.

Two supporting changes turn "avoided" into "impossible":

- `handle_command`'s `bool record = true` parameter was deleted and `client_user` lost its
  default value, so the exact call that caused R1 — `handle_command(cmdline, "", false)` — **no
  longer compiles.**
- `replay_command` is an exhaustive, readable list of the six things recovery may do. Anything
  else is skipped with a message naming it, which also makes the tracker safe against the
  coursework-era log format that recorded reads and logins.

**Rejected: option A, a replay flag.** ~20 minutes instead of ~2 hours. Rejected because the
cost is paid later and silently, and because the guard-skipping flag is itself the bug pattern
that produced R1.

**Rejected: option C, log the effect rather than the request (event sourcing).** The tracker
would record `GROUP_CREATED g1 alice` instead of the client's `create_group g1 alice`, and
recovery would apply effects that have no notion of a requester at all. **This is the better
system and it is worth saying so.** It costs ~3 hours and a new on-disk format, spent on a
control plane that a Raft-replicated tracker would replace — and Raft brings the same
structure back for a stronger reason, because a follower applies log entries with no client
attached at all.

Option C also has a concrete correctness advantage that B does not: **an effect log cannot
drift.** `leave_group` picks the new owner with `*members.begin()`, whose result depends on
hashing rather than on anything recorded, so a replay may choose a different owner than the
live run did (defect R5). An effect log would have recorded the choice. B leaves that open; it
is registered rather than hidden.

**What the split forced into the open.** Classifying every line of every effect as durable or
soft caught `upload_file` writing soft state: it inserted the uploader into `fi.seeders` and
recorded an address in `user_address_map` alongside the manifest. Replaying that resurrects
peers that are long dead — the exact thing the `update_seeder` handler already refuses to do.
A flag would have replayed it and nobody would have looked.

---

### D-010 — A reply is exactly one line, and the framing layer guarantees it

**Fork:** F9. **Date:** 9 Sep 2026. **Closes:** defect R6.

**The decision.** `send_all` in `tracker.cpp` is the single place that frames a control reply.
It strips any terminator the caller supplied, replaces any remaining `\n` or `\r` **inside** the
payload with a space, logs loudly when it has to, and appends exactly one terminator. The
invariant "one command, one reply, one line" therefore holds regardless of what any handler
returns. The five reply strings that carried an embedded newline were fixed as well, so the
backstop stays silent in normal operation and fires only on a real bug.

**Why the invariant needs an owner.** The client reads a reply as bytes-up-to-newline. A reply
containing its own newline arrives as two replies, and every later reply on that connection is
one behind — permanently, on a connection that is otherwise healthy and answering. R6 and R3
are the same failure from opposite directions: R3 was a *caller* that never consumed its reply,
R6 is a *message* that contains the delimiter. Nothing on the wire says how many lines a reply
is, so any single violation by either party is unrecoverable.

**Rejected: option A, fix the five strings and stop.** Two minutes. Rejected because it removes
today's instances and adds no rule — the sixth such string reintroduces the defect, and the
failure is silent. This is the same shape as F8's rejected replay flag: a defect avoided by
everyone remembering, rather than made unreachable.

**Rejected: option C, length-prefixed replies** (`LEN <n>\n` then exactly n bytes). This is the
better protocol and the one the data path already uses for pieces, because it makes the
delimiter irrelevant rather than forbidden. It costs a change to every send and every receive
on both sides, at a time when the Chord work came first, and buys nothing until replies start
carrying arbitrary payloads. **Planned for Phase 5, where the transfer layer is rewritten anyway**
— and recorded here so that "why not length-prefix everything?" has an answer with a date on it.
(In Phase 5 it was withdrawn instead: no tracker reply carries a payload any more — D-060.)

**Deliberately not applied to the client.** `client.cpp`'s `send_all` is the raw-byte sender
that carries piece contents, which contain newline bytes constantly. Stripping delimiters there
would corrupt every transfer. Delimiter sanitising is correct on a text control channel and
catastrophic on a binary data channel — which is the same distinction that made two framing
schemes necessary in the first place.

**Cost accepted.** A multi-line reply is now impossible by construction. Every current reply is
single-line, including the list replies, which are space-separated; if a future command needs
structured output, that is the trigger to take option C rather than to weaken this.

**Verified.** `scripts/e2e-framing.sh` drives a raw socket — not the client, which reads one
line per reply and would hide which side emitted the extra one — sends each of the five
malformed commands, and after each sends a probe whose reply is unmistakable. 11/11 pass on the
fix. **Run against the pre-fix binary the same suite fails 9/11**, showing the stream one behind
and then two behind, which is what makes it a regression test rather than a passing assertion.

---

### D-011 — A peer's disconnect is an error value, not a signal

**Fork:** F10. **Date:** 9 Sep 2026. **Closes:** defect R9.

**The decision.** Both binaries call `signal(SIGPIPE, SIG_IGN)` as their first act in `main`.
Writing to a socket whose peer has gone then returns `-1` with `errno == EPIPE` instead of
raising a signal, and the `if(n <= 0) return false` already present in every send loop handles
it correctly.

**What was actually wrong.** Nothing in the error handling. The check was there and it was
right — it never got to run, because the default disposition of SIGPIPE terminates the process
before `send` returns. Reproduced deterministically: a client pipelines 200 commands, reads
none, and closes with `SO_LINGER` set to zero so the connection is reset rather than
half-closed. The tracker dies with **exit 141 — 128 + 13, SIGPIPE**, sometimes on the first
command. **This kills the whole tracker, not the one connection's thread**, because a signal
disposition is a property of the process. Every other client's session dies with it.

**Why this is worth a decision entry rather than a one-line fix.** The defect is that a *remote
party's behaviour* was routed into a *local control-flow mechanism* that ends the program.
Nothing about a peer hanging up is exceptional — on the data path it is the common case, since
a downloading peer that has what it needs simply goes away. A design that treats the normal
behaviour of an untrusted party as fatal has the failure model inverted.

**Rejected: option B, `MSG_NOSIGNAL` on every `send`.** Local and explicit at each call site,
and it does not change process-global behaviour. Rejected because the suppression then has to be
remembered at every existing and future send — including the peer server's raw `send` calls in
`client.cpp` — and one miss reinstates the defect. That is the "avoided by remembering" pattern
rejected in D-009 and again in D-010; three decisions now share that reasoning.

**Rejected: option C, both.** No additional safety over A here, since A already covers every
send in both binaries. Worth revisiting only if this code ever becomes a library, where the
process-wide ignore would be imposed on someone else's program.

**Cost accepted.** The ignore is process-wide, so any future code in these binaries that
genuinely wants SIGPIPE — a shell-pipeline tool, say — no longer gets it. For a network server
that is the standard trade and it is not close.

**Verified.** `scripts/e2e-hangup.sh` reproduces the hangup and asserts two things, not one: the
tracker is still running, **and** it still accepts a new connection and answers it. A process
that survived but stopped serving would pass a liveness check and fail every user. Against a
build with the fix commented out, the same suite fails both, reporting exit 141 by name.

---

### D-012 — Lookups are iterative: the originator drives every hop

**Fork:** F1. **Date:** 12 Sep 2026. **Gates:** Phase 2, all Chord routing code.

**The decision.** `find_successor` is a **single-step** remote call. A node answers either "I am
the owner" or "here is a node closer than me", and returns immediately. The **originator** runs
the loop, opening a connection to each successive node itself. A four-hop lookup is four
request/response exchanges initiated by the client, not one call that fans out through the ring.

**Why.** Four reasons, in the order they carried weight.

1. **It makes the headline benchmark an observation rather than a self-report.** Phase 2's
   deliverable is *hop count versus ring size, plotted against log₂N*. Under iterative routing
   the client counts its own loop iterations — the measurement instrument sits **outside** the
   thing being measured. Under recursive routing the ring reports its own hop count in a field
   threaded through the messages, and the plot shows what the system says about itself. Whether
   the number can be trusted has a good answer in the first case and a weak one in the second.
2. **Phase 3 is entirely about killing nodes.** Iterative gives exact failure attribution: a node
   times out and the originator knows precisely which one, and can retry immediately with the
   next finger it already holds. Recursive gives a timeout with no attribution — the worst
   possible property for the phase whose whole job is to measure what happens when nodes die.
3. **It fits the concurrency model that already exists.** Both binaries are thread-per-connection
   TCP. Under recursive routing every node on the path holds a **blocked thread** for the whole
   duration of the lookup; a ring under load would have threads waiting on threads waiting on
   threads. Under iterative routing every handler returns instantly and holds nothing. Choosing
   recursive would have meant either accepting that cost or rewriting the concurrency model —
   a cost invisible in a textbook comparison and very real here.
4. **Precedent, knowingly diverging from the paper.** The Chord paper's pseudocode is recursive
   (`return n'.find_successor(id)`). Kademlia — the DHT that actually shipped at scale, in
   BitTorrent and Ethereum — is **iterative**, for reasons 1 and 2. Diverging from the paper
   deliberately, and being able to name who else diverged and why, is stronger than following it.

**Rejected: option B, recursive.** Lower latency (roughly `hops × one-way` rather than
`2 × hops × one-way`), NAT-friendly since only the first hop must be reachable, and intermediate
nodes can cache what they forward. Rejected on three costs: **nested timeouts** — hop 3 stalling
blocks hops 2 and 1 and the originator, and deciding whose timeout fires and who retries is
genuinely hard to get right; **no failure attribution**; and **self-reported hop counts**, which
are exactly the number this project's first plot is made of.

**Rejected: option C, hybrid** (forward recursively, final node replies direct to the
originator). Cuts the return path, so it optimises **latency** — the one cost that is invisible
on loopback, where every number in this project is measured. It still leaves hop counts
self-reported and failure attribution absent, i.e. it optimises the thing that cannot be observed
here at the price of the two things that must be. A third message pattern to implement and debug
for no measurable gain.

**Cost accepted — stated up front rather than discovered.** **Two network traversals per hop
instead of one**, so lookup latency is roughly double recursive's, plus a connection setup per
hop unless connections are pooled. On loopback this is nearly free, and **`BENCHMARKS.md`
already declares at the top that loopback has no real network latency** — meaning the measurement
environment flatters this choice, and that flattery is declared rather than hidden. On a
wide-area deployment (~75 ms between regions, 4 hops) it is roughly 600 ms versus 375 ms. If the
ring is ever deployed on real VMs, the fix is **not** to change the routing model but to add a
short-TTL lookup cache at the originator: a stale entry costs one wasted hop and self-corrects,
because a bad routing hint can only cost hops and never correctness.

**What would change my mind:** ring members behind NAT, which makes iterative impossible because
the originator cannot reach hop 2. Noting that this scenario breaks the **data plane** harder
than the control plane — unreachable peers cannot accept transfers either — so it would need
STUN/TURN and hole punching regardless, and is not a routing decision.

**Evidence:** decided at design time, before any Chord code existed. The hop-count plot
(`BENCHMARKS.md` §2, `e042857`) is the evidence and the reason for the choice: 380,928 lookups
counted by the originator itself.

---

### D-013 — Failure detection is opportunistic, and a refused connection is not a timeout

**Fork:** F4. **Date:** 12 Sep 2026. **Gates:** Phase 3, the reconvergence benchmark.

**The decision.** Two parts.

**(a) Stabilisation is the detection floor; any failed call to the successor is also evidence.**
The periodic `stabilize()` already calls `successor.predecessor` every `T` seconds, and a failure
there means the successor is gone. On top of that, **every** other code path that talks to the
successor — lookups, `notify`, chunk transfers — reports its failures to the same detector
instead of discarding them. So:

```
detection time = min( time to next stabilise , time to next natural traffic )
```

Detection is therefore **load-adaptive**: near-instant on a busy ring, degrading to plain
stabilisation on an idle one — where nobody is affected by the delay anyway.

**(b) `ECONNREFUSED` and a timeout are different evidence and are treated differently.**
A refused connection means the kernel on the far side actively sent a reset: **nothing is
listening**, which is proof rather than suspicion, and the node is evicted immediately. A
**timeout is ambiguous** — dead, slow, a lost packet, or a node whose CPU is contended — so it
marks the successor *suspect* and requires a second independent failure, or confirmation by the
next `stabilize`, before eviction. One timeout is not evidence.

**Why.** Detection speed only matters when somebody is affected, and somebody being affected
means traffic is flowing — which is exactly when opportunistic detection is fastest. Paying for
constant heartbeats buys speed during the periods when nobody would have noticed the delay.

**Rejected: option B, active heartbeats between successors** (ping every `H` seconds, evict after
`k` misses). It buys detection time `k × H`, decoupled from the stabilisation period. Rejected on
four costs: permanent background traffic of `N/H` messages per second whether or not anything is
wrong; **two more tuning constants that both need justifying**, and "3 misses at 1 second" is a
precedent rather than a reason; **designed-in false positives** — a slow node is evicted while still alive
and still serving, and since it is never told, there is a window where two nodes believe they own
the same range; and, decisively, **this machine would manufacture those false positives**, because
every peer in a test ring shares the same 8 cores and 3.4 GiB, so at any interesting ring size
nodes are intermittently slow by construction. Deaths generated that way are a **measurement
artefact of the test rig**, and they would end up on the plot.

**Rejected: option A, stabilisation alone.** This is the floor of the chosen design, not an
alternative to it — C is A plus information that A throws away. Rejected as a *complete* answer
because detection is then as slow as `T` even under heavy load, and `T` cannot be lowered to fix
that without raising background traffic for every node, forever, to speed up a rare event.

**Rejected: option D, a phi-accrual failure detector** (Cassandra's approach: track the history
of reply latencies and emit a continuous suspicion level rather than a binary verdict, letting
the threshold adapt as the network slows). It is the better detector on a real network, and it is
rejected only because of where this is measured: **loopback has essentially no jitter**, so
phi-accrual degenerates to a fixed timeout wrapped in statistics. Significant work for no
improvement in the only environment that produces numbers. **It is what a real-network
deployment should use** — the weakness of a fixed timeout is that it assumes a stationary
latency distribution, and a wide-area network does not have one.

**Cost accepted.** Two.

1. **Detection time becomes a distribution rather than a constant**, because it depends on
   workload. Reconvergence under load and reconvergence when idle are two different measured
   numbers. That is less convenient than one figure and more accurate; both get published, with
   the explanation of why they differ.
2. **Every call site touching the successor must report its failures**, which is the
   "must-remember-at-every-call-site" shape rejected in D-009, D-010 and D-011. **Mitigated the
   same way D-010 was:** a single wrapper function owns every successor call and does the
   reporting inside itself, so the discipline lives in one place rather than in every author's
   memory.

**Two sub-decisions that come with it.**

- **`T`, the stabilisation period, is a measured number, not a guess.** It does double duty: it
  bounds worst-case detection *and* sets the repair rate at one node per round, so it appears
  twice in the reconvergence figure. Phase 3 plots
  reconvergence against `T` and the value is picked off the curve — the same discipline as F5's
  chunk size.
- **An evicted node is never told it was evicted.** If it was only slow, it re-inserts itself on
  its own next `stabilize`, because `notify` will be accepted by whoever is now its successor.
  There is no eviction protocol and no un-eviction protocol, and therefore no message that can be
  lost. This is the level-triggered property of D-013's parent design doing the work: a node
  rejoins after a false positive exactly the way it joined in the first place.

**What would change my mind:** a deployment where the ring is spread across a real wide-area
network with genuine latency variance. Then the fixed timeout underlying the "suspect" state is
the weak component, and phi-accrual earns its complexity.

**Evidence:** decided at design time. `BENCHMARKS.md` §4 (`b171554`): one node killed in a 64-node
ring is repaired in a median 0.63 s idle; under lookup load, finger repair falls from 3.99 s to
0.29 s because clients report the dead node (D-036).

---

### D-014 — Chunks are content-addressed: the key is the hash of the value

**Fork:** F3a, surfaced while preparing F3 and decided before it. **Date:** 12 Sep 2026.
**Gates:** F3, F2, F5, and the whole data plane.

**The decision.** A chunk's distributed-hash-table key is **`SHA-1(chunk contents)`**, so a
chunk's position on the ring is determined by its own bytes. A file's manifest is the ordered
list of its chunk hashes — which the existing system already computes and already verifies.

**What this buys, and it is structural rather than incremental.**

1. **Two replicas can never disagree.** The value stored at key `abc123…` is by definition the
   bytes that hash to `abc123…`. There is no newer version of them. **Conflicts do not exist**,
   so there is no versioning, no timestamps, no vector clocks, no last-write-wins and no conflict
   resolution anywhere in the data plane.
2. **Reads are self-verifying.** A client hashes what it received and compares it to the key it
   asked for. A corrupt, truncated or simply wrong answer is detected **locally by the reader**,
   with no second replica consulted. This is what makes `R = 1` safe.
3. **Deduplication is free.** Identical chunks in different files land on the same key and are
   stored once.

**The consequence that reframes F3.** The quorum inequality `W + R > RF` exists to guarantee that
a read set overlaps a write set **so that a read sees the latest version**. With immutable
content-addressed values there is no latest version, so the inequality is not satisfied or
violated — **it is inapplicable**, and `W` and `R` stop being coupled. `W` buys **durability**;
`R` buys **availability**; they are set independently. Knowing why the rule does not apply is
better than applying it by reflex.

**Rejected: key = `(file_id, chunk_index)`.** A smaller change — the current protocol already
requests pieces by index (`GET_PIECE <group> <file> <i>`). Rejected because the value at a key
then **changes** when the file is overwritten, which reintroduces mutability, and with it
conflicting replicas, versioning, clock skew, read repair and the full quorum machinery. It would
mean *choosing to have* a consistency problem, and paying for it in a phase that already
contains virtual nodes.

This is the same structural move as **D-009** (make the bug unreachable rather than avoidable),
**D-010** (put the invariant in the framing layer rather than in every author's memory) and
**D-011** (remove the failure mode rather than remember to suppress it). Four decisions in this
project now share that reasoning, and that consistency is itself worth stating.

**Cost accepted.**
- **No in-place update.** A changed file produces new keys; the superseded chunks are garbage
  until something collects them. No garbage collector is built (D-023 designs one).
- **Placement is not influenceable.** A chunk goes where its hash sends it, by construction.
- **The lookup path changes** from index-keyed to hash-keyed, so the legacy `GET_PIECE` protocol
  does not survive Phase 5 unchanged.

**What this does to the consistency/availability question — it moves it.** The data plane no
longer has a consistency problem. The **metadata** does: the name-to-manifest mapping is mutable,
and that is the plane a Raft group would replicate. So the system sits deliberately in two
different places: the data plane is content-addressed and immutable, so it is tuned purely for
durability and availability; the control plane is mutable, so it is the one that needs
consensus.

**Naming note.** Standard quorum notation uses `N` for the replication factor, and this project
already uses `N` for **ring size**. All documents use **`RF`** for replication factor and never
`N`, because two similar-looking values meaning different things is a hazard that has
already bitten this codebase (defect R2).

**Evidence (`BENCHMARKS.md` §5, `2e7c445`):** identical content shares every chunk, an in-place edit all but
one; an insertion near the start shares almost nothing, and so did 20 real commits of this repository (1 chunk of 97).

---

### D-015 — `RF = 3`, `W = 2`, `R = 1`, and `W` is a runtime parameter

**Fork:** F3. **Date:** 12 Sep 2026. **Depends on:** D-014 (content addressing).
**Gates:** Phase 4.

**The decision.** Three copies of every chunk — the owner plus its first two successors, which
D-014 and the successor list make the same set. A write is acknowledged when **the owner and at
least one successor** hold it; the third copy propagates in the background. A read consults
**one** replica, and on a failed hash check moves to the next. **`W` is a configuration value,
not a literal**, so it can be swept.

`R = 1` needs no defending: D-014 makes reads self-verifying, so a wrong answer is caught locally
by the reader. That is a retry loop, not a quorum.

**The mechanical detail that drives the latency arithmetic.** The owner writes its own copy
locally and sends to both successors **in parallel**. So `W = 2` waits for the **first** of the
two successors to answer — `min` of two round trips — while `W = 3` waits for both, `max` of two.
On a test host where every peer contends for the same 8 cores, one peer being briefly very slow
is the normal case, so the gap between `min` and `max` is large and `W = 2` is materially faster
than the name suggests.

**Why `W = 2`.** It is the point at which **an acknowledged write is a true statement** and a
single failure does not stop you.

**Rejected: `W = 1`, write-one-and-propagate.** Fastest writes, highest write availability, least
code. Rejected because its failure mode is **silent data loss on an acknowledged write**: the
owner accepts, reports success, and dies before propagating. For a file system, a system that
lies to the client about durability is the worst class of defect — the same category as D-010,
where the tracker kept answering confidently and every answer was wrong.

**Rejected: `W = 3`, synchronous to all.** Survives two simultaneous failures and keeps all three
replicas current. Rejected because **a write then fails whenever any one successor is down** —
which, during churn or mid-stabilisation while a successor pointer is still tightening, is a
meaningful fraction of the time — and because write latency becomes the `max` of two round trips,
so one slow peer dominates every write. It buys protection against a *second* simultaneous
failure by making writes fail during every *first* one.

**Cost accepted.**
- **The advertised `RF = 3` is briefly untrue on every write**, not only after a failure: between
  acknowledgement and background propagation there are two copies, so the system tolerates one
  further failure rather than two. This is the degraded window, and the response is to
  **measure it** rather than assume it away.
- **If both successors are unreachable, `W = 2` is unsatisfiable and the write fails loudly.**
  That is the availability cost and it is the correct behaviour, since the alternative is `W = 1`
  lying about it.

**Why `W` is configurable — this is half the decision.** Making `W` a parameter costs a variable
instead of a literal and converts the fork into a measurement: sweep `W ∈ {1, 2, 3}` and plot
**write latency** and **write success rate while a node is being killed**. That plot answers
*"where does this sit on the consistency/availability trade-off, and how would you move it?"*
with a measurement instead of an opinion: **here is the knob, and here is the measured cost of
each setting.**

**Named but not built: hinted handoff** (Dynamo's technique — if a successor is down, write the
copy to the next node along with a note saying who it really belongs to, and forward it when that
node returns). It raises write availability without lowering `W`. It is the next step if
`W = 2` writes were failing too often, and it was deliberately kept out of a phase that already
contains virtual nodes.

**Evidence:** `BENCHMARKS.md` §6 (`7d99fc0`): with one replica dead W = 2 acknowledged 300/300 writes and W = 3
244/300; W = 1 answers in 14 ms against 31 ms, and its acknowledged chunk is on one disk for a median 18 ms (the
degraded window); killing the coordinator at the acknowledgement left no other copy in 20 of 20 W = 1 trials and at
least two in every W = 2 and W = 3 trial. W = 2 and W = 3 cost the same with healthy replicas on loopback.

---

### D-016 — 512 KB chunks, held constant for the headline claim and swept separately

**Fork:** F5. **Date:** 12 Sep 2026. **Gates:** Phase 5.

**The decision.** Chunks are **512 KB** — the size the existing code already uses. Phase 5 sweeps
`{64 KB, 256 KB, 512 KB, 2 MB, 8 MB}` as a **separate experiment**, with parallelism held
constant, and the curve is published whatever it shows.

**Why 512 KB, and the reason is experimental rather than technical.** The Phase 0 baseline —
**71.2 MB/s at 100 MB, commit `e381179`** — was measured at 512 KB. Phase 5's headline claim is
parallel transfer against that baseline. **If Phase 5 changed chunk size *and* added parallelism,
the improvement could not be attributed to either.** Two variables moved, and the resulting number
would explain nothing. So chunk size is pinned at 512 KB for the before/after, and swept afterwards with
parallelism fixed. One controlled variable per claim.

**Five sweep points, not three**, roughly log-spaced, because three points cannot distinguish a
curve with a knee from a straight line — and the knee is the entire justification for whatever
number ends up in the README.

**The interaction that makes chunk count more expensive here than in a textbook.** D-012 chose
iterative routing, so a lookup costs `2 × hops` network traversals rather than `hops`. On a
20-node ring that is `2 × log₂20 ≈ 9` round trips **per chunk**. At 64 KB, a 100 MB file is 1,600
chunks and therefore ~14,400 round trips of pure lookup before any payload moves; at 4 MB it is
~225. Chunk size is not only an I/O parameter here — it is a **routing-load parameter**, and that
is a direct consequence of D-012.

**And the interaction with D-014.** Content addressing means consecutive chunks of one file hash
to unrelated ring positions, so there is **no locality to exploit** — every chunk is an
independent lookup to an arbitrary node. The mitigation is that the manifest supplies every hash
up front, so all lookups can be issued **concurrently** rather than serially. That is a Phase 5
implementation requirement created by two earlier decisions meeting, and it is recorded here so it
is designed rather than discovered.

**Rejected: 64 KB.** Best parallelism, cheapest recovery, finest deduplication granularity.
Rejected because lookup traffic dominates at this chunk count under iterative routing, manifests
grow to 32 KB for a 100 MB file, and per-chunk syscall overhead becomes visible against the
payload.

**Rejected: 4 MB.** Minimal lookups, tiny manifests, excellent sequential disk I/O. Rejected
because one slow peer then dominates an entire transfer, recovery from a failed chunk is coarse,
and deduplication almost never hits — a 4 MB span rarely repeats, so D-014's free dedup becomes
theoretical.

**Stated before the data exists, so it is not a surprise:** on loopback this curve may come out
**nearly flat**, because there is no network latency for larger chunks to amortise and the page
cache absorbs much of the I/O difference. **If it is flat, that is the finding** — "chunk size
barely matters on loopback, here is why, and here is what would change on a real network" is an
honest result, and reporting a flat curve as flat is worth more than tuning until something looks
interesting.

**Touches an open defect (at the time).** R4, the zero-byte file, was the one deliberate failure
in `scripts/e2e-edge.sh`. A 0-byte file is 0 chunks, and under D-014 the empty chunk has a
perfectly well-defined hash. F5 and R4 met in Phase 5, where the transfer layer was rewritten and
R4 was fixed (D-054).

**Evidence (`BENCHMARKS.md` §5, `2e7c445`):** at 512 KiB the new path is 5.4× the baseline re-run the same
day, 3.7× from parallelism alone. The sweep is **not** flat, against the prediction above: 100 MB/s at 64 KiB to
536 MB/s at 8 MiB, because each chunk pays a lookup and fresh connections whatever its size. 512 KiB stays, for the
reasons in §5: the comparison is pinned to it, and the costs of large chunks are the ones loopback cannot show.

---

### D-017 — The tracker is a mutable namespace over an immutable store

**Fork:** F2. **Date:** 12 Sep 2026. **Depends on:** D-014. **Gates:** the tracker, and any Raft work.

**The decision.** The tracker holds exactly one thing: **`filename → manifest hash`**, about
40 bytes per file. The **manifest itself is a content-addressed object stored in the ring**, like
any chunk, and fetching it is an ordinary lookup. The tracker holds no placement information at
all, because D-014 makes a chunk's location computable — it is `successor(SHA-1(chunk))`.

The read path:

```
tracker:  "myfile.iso"   →  manifest hash        (~40 bytes, one round trip)
ring:     manifest hash  →  the manifest         (an ordinary chunk lookup)
ring:     each chunk hash →  the chunk           (concurrent lookups)
```

**Why.** It makes the **mutable surface of the whole system as small as it can possibly be** — a
filename and a hash. The mutable surface is precisely what needs consensus, what can be
inconsistent, and what a Raft-replicated tracker has to replicate. Everything beneath it is immutable
and therefore conflict-free.

**This is Git's data model**, and the parallel is exact and worth stating: content-addressed
immutable objects (blobs, trees) with a small mutable namespace layered on top (refs and branch
names). Only the namespace needs consensus.

**What it buys.**
- **Tracker state is `O(files) × 40 bytes`**, not `O(total chunks)`. A future Raft log stays
  small, which is exactly what the original fork text predicted manifest-only would buy, and a
  small log makes replicating the tracker much cheaper.
- **It removes `SCALE_NOTES.md` bottleneck #2 structurally rather than mitigating it.** That entry
  reads *"tracker builds a multi-MB string under `state_mtx`; response may exceed what the
  client's line reader tolerates."* Under D-017 no multi-MB string is ever built, anywhere.
- **Manifests inherit replication for free.** They are chunks, so they get `RF = 3`, `W = 2`, the
  hash check and the same availability as any other object. No special-casing.
- **The tracker leaves the data path.** It is consulted once per file, for 40 bytes.

**Rejected: the tracker holds placement (chunk hash → node).** One lookup instead of `O(log N)`
hops. Rejected because it creates **two systems that can disagree** about where a chunk lives —
and the ring is right by construction while the tracker's copy is a cache that goes stale on
every join, every failure and every stabilisation round. It also re-centralises the exact thing
Chord exists to decentralise: if the tracker knows every chunk's location, the ring is decoration.

**Rejected: the tracker holds full manifests in its own state.** Simple, closest to the current
code, one round trip for everything. Rejected because tracker state — and therefore the Raft log
— would grow with total chunk count (a 10 GB file at 512 KB is 20,000 hashes in one record), it
preserves bottleneck #2 verbatim, and it keeps the tracker on the critical path serving
payload-sized responses.

**Cost accepted.**
- **One extra round trip**: name → hash, then hash → manifest.
- **The ring serves a non-uniform object.** A manifest for a large file is ~32 KB sitting among
  512 KB chunks. Harmless, but it means "every value is a chunk" is not quite true and the code
  should not assume a fixed size.
- **The tracker becomes required for discovery.** A manifest hash means nothing to a human, so
  there is no browsing the system without the namespace. The trade is deliberate: that is the
  component to make highly available with consensus.

**In one line, what the tracker is for:** *"It is a mutable namespace over an
immutable store. Human names change; content does not. The only thing that needs consensus is the
mapping between them."*

**Evidence (`BENCHMARKS.md` §5, `2e7c445`):** 77 bytes of log and about 230 bytes of memory per name at 10,000
and 100,000 names, independent of file size — the `O(files)` claim, measured.

---

### D-018 — A ring member is its own process, and the routing primitives are pure functions

**Fork:** F11. **Date:** 12 Sep 2026. **Gates:** Phase 2 and every file added from here on.

**The decision, in two parts.**

1. **A ring member is a standalone `node` binary.** `client` stays the user-facing command-line
   program and is the *originator* of lookups — outside the ring, exactly as the target diagram
   already draws it. A participant who both stores chunks and uses the system runs two processes.
2. **The routing primitives are free functions in `chord.h` / `chord.cpp`** — the wrap-around
   interval test, identifier arithmetic, `closest_preceding_finger` — each taking the state it
   needs as explicit arguments. Node state, the socket server and `main` stay in `node.cpp`.
   **There is no `ChordNode` class and no library interface.**

**Why part 1.**

- **Maximum ring size is the x-axis of the Phase 2 plot, and it is set by free RAM (~3.4 GiB).**
  A node binary with no download worker pool, no stdin loop and no tracker session has a far
  smaller resident set size than `client`. Per-node RSS is *measured* in Phase 2 and the ceiling
  derived from it rather than guessed, per the environment constraint already recorded.
- **The experiment needs no tracker and no login.** N nodes on N ports, started and killed by a
  script. Under option B every data point requires N full clients with stdin attached.
- **It keeps Phase 2 away from working code.** Adding routing to `client.cpp` means editing 1013
  lines that five green suites hang off, during the phase whose deliverable is a correctness
  claim about something else entirely.
- **Precedent.** Cassandra ships the `cassandra` daemon and `cqlsh`; IPFS ships `ipfs daemon` and
  the `ipfs` command. A storage-and-routing daemon separate from its client is the normal shape.

**Why part 2, and what was deliberately *not* done.**

- **Pure functions over explicit arguments make no promise about what a node is.** That matters
  because Phase 4 changes it: virtual nodes mean one process hosting many ring identities, and a
  header declaring "a node has an identifier and a finger table" is a promise Phase 4 breaks.
  A class with a published interface would have to be rewritten; free functions just get called
  with different arguments.
- **It gives the routing phase an in-process test for the wrap-around interval test** — the single piece of
  Phase 2 most likely to be silently wrong, and precisely the class of structural bug that only
  *constructed* data finds, never sampled or random data.
- **D-012 is what makes this cheap.** Under iterative
  routing the node-side answer is pure local computation — "I own it, or here is someone closer" —
  so the tested functions never touch a socket and need no fake network. Under recursive routing
  `find_successor` would call the next node, and testing it in-process would have required an
  interface and a mock object: a real abstraction bought at real cost. A fork resolved for
  benchmark reasons paid for this one.

**Rejected: option B — extend `client`, one process per participant.** The existing peer-server
thread grows into the ring node; no duplicated configuration, and the transfer path is already
there. Rejected on three costs: a heavier process per ring member and therefore a **shorter
headline plot**; a test harness that must drive N stdin loops and N tracker logins for every data
point; and the risk of breaking a working transfer path while building routing.

**Rejected: everything in one `node.cpp`.** Commits to no interface at all, which is a genuine
advantage, and extracting a seam later is cheap in a single-consumer repository. Rejected only
because the routing tests want the in-process seam *anyway*, and the narrowed split — pure functions, no
type — costs about twenty minutes of `Makefile` work and carries none of the design commitment
that a class would.

**A correction worth recording.** These two
were first posed as three peer options: separate binary, extend `client`, and "a Chord library".
The first and third are **not rival architectures** — they produce the same binary, the same
process model and the same numbers, and differ only in which file the code sits in. The genuine
fork was one process versus two; file layout is a sub-choice underneath it. The answer to "why
don't real systems do the library version?" is that **they do** — Cassandra's routing logic is not
welded into `main()` — but what they do not do is publish a stable interface before a second
consumer exists, which is a different thing from splitting a file.

**Cost accepted, stated up front.**

- **A participant runs two processes**, so there is no single "join the network" command. The
  deployment and any `docker compose` file must start both.
- **`client`'s peer server and the node's chunk store will overlap in Phase 5**, when transfer
  moves onto the ring, and one of them has to move or die. That is a scheduled duplication, not
  an oversight — it is named here so it is not rediscovered as a surprise.
- **A third object file and a hand-written header-dependency rule in the `Makefile`.** That is the
  exact mechanism behind defects B1 and B2, so the rule is written deliberately and
  `make clean && make` is verified rather than assumed.

**What would change my mind:** Phase 5 finding that the chunk store and the transfer path cannot
be sensibly split across two processes — in which case the merge is `client` linking `chord.o`,
which part 2 already leaves open.

**Evidence:** `BENCHMARKS.md` §2 (`e042857`): about 0.95 MiB proportional set size per node at
2048 nodes, which sets the real-process ceiling of the hop-count plot at 2048.

---

### D-019 — The ring is 64 bits wide; keys stay 160 bits wide

**Fork:** F12. **Date:** 12 Sep 2026. **Gates:** Phase 2, every identifier-handling line in
the project.

**The decision.** `m = 64`. A **routing identifier** is the top 8 bytes of a SHA-1 digest, held in
a `uint64_t`. A **key** remains the full 160-bit digest, 40 hexadecimal characters, exactly as
D-014 defines it. Finger tables have 64 rows.

**The distinction the whole decision rests on — identity versus placement.**

- A chunk's **identity** is `SHA-1(contents)`, 160 bits. It is what the reader re-hashes to verify
  what came back, and it is what a node stores it under.
- A chunk's **placement** is which node holds it, and that is decided by where its identifier
  lands on the ring.

Nothing requires the ring to have as many points as the key has bits. Truncating for placement
does not weaken verification, because verification never looks at the truncation. This is why
64 bits is not a shortened hash — it is a shortened *address*.

**Why.** The arithmetic, and specifically what a bug in it would do.

- **Unsigned integer overflow in C++ is defined to wrap modulo 2ⁿ** (signed overflow is undefined
  behaviour and must never be used for an identifier). So at `m = 64` the ring **is** the machine
  word: `n + (1ULL << i)` is ring arithmetic, computed by the CPU's fixed-width adder, which
  discards the carry out of bit 63 for free. There is no modular-arithmetic code to write, and
  therefore none to get wrong.
- At `m = 160` the finger offset `n + 2^i mod 2^160` is a hand-written carry-propagation loop over
  a 20-byte array — about ten lines, plus the tests that prove it.
- **The size of that code is not the argument; what a bug in it does is.** The finger table is
  *self-validating*: every entry is checked against `(my_id, k)` before use, so
  a wrong finger can only cost hops, never correctness. That is a gift everywhere else and a trap
  here. A carry bug produces wrong finger entries, routing still returns the right node, nothing
  crashes, no test fails — and **the hop count silently inflates.** Hop count is the entire output
  of Phase 2. The 160-bit path's failure mode is silent corruption of the one number the phase
  exists to produce, in code that has no natural alarm.
- **Collision headroom is ample and it was checked, not assumed.** By the birthday bound, 100,000
  ring identities — a 1000-node ring at Phase 4's ~100 virtual nodes each — collide in 2⁶⁴ with
  probability ≈ 3 × 10⁻¹⁰.
- **Precedent.** Cassandra's default partitioner (`Murmur3Partitioner`) produces **64-bit** tokens,
  and production clusters run thousands of nodes at 256 virtual nodes each — a quarter of a million
  identities in a 2⁶⁴ space. A 64-bit ring is what the most widely deployed consistent-hashing
  system in production actually ships.

**Rejected: option A, the full 160 bits.** What the Chord paper specifies, and it uses the whole
hash. Rejected on the failure mode above: it puts hand-written carry
propagation directly underneath the phase's only measurement, where a bug is invisible. The
honest accounting of the extra work is smaller than it first appears — comparison is `memcmp`
(byte order is numeric order, the digest being big-endian), printing already exists in `sha1.h`,
and digest-to-identifier is *free* at 160 bits and costs three lines at 64 — so the real delta is
one carry loop, roughly 60–80 lines with tests against roughly 5. That was stated plainly at the
time so the choice was made on the true number rather than on a scare.

**Rejected: option C, `unsigned __int128`.** Native arithmetic at 128 bits, available on this
aarch64 toolchain. Rejected because it is a GCC/Clang extension rather than standard C++17, and
it has **no stream output operator and no literal suffix** — so printing one for a log line or a
wire message is hand-written conversion, which is the same category of code option B was chosen to
remove, bought for headroom that is already unreachable at 64.

**Rejected outright, on a number, before it was offered: 32 bits.** Phase 4's virtual nodes put
~100 identities in each process, so a 1000-node ring is ~100,000 identities. In a 2³² space the
birthday bound gives roughly a **69 % chance of at least one node-identifier collision** — ambiguous
arc ownership as the *expected* outcome. It would have worked perfectly until Phase 4 and then
broken subtly, which is the worst available shape for a defect.

**What a collision does, and the two invariants that contain it.** Two *chunks* sharing a routing
identifier is harmless — they route to the same node, which stores them under their distinct full
keys. Two *nodes* sharing one is not. The sharpest mechanism is not ambiguity but a line of code:
the standard interval test evaluates `(n, n]` as the **entire ring**, because that is how a
single-node ring is expressed. So a node whose successor shares its identifier concludes it owns
every key in existence, confidently and silently. Hence **I7** and **I8** below. `I8` alone
downgrades a collision from "one node swallows the ring" to "two nodes disagree about a boundary",
and costs nothing, because the operating system already guarantees `ip:port` uniqueness far more
strongly than a hash does.

**Cost accepted.** "You didn't use the whole hash" is a natural objection, and the
identity-versus-placement distinction has to be stated rather than assumed. Detection and recovery from
a node-identifier collision is real work, deferred to Phase 3 as **F15** rather than settled here by
implication.

**Evidence:** decided at design time. The hop-count plot (`BENCHMARKS.md` §2) is the evidence; the
per-node memory measurement there sets the maximum ring size the plot can reach.

---

### D-020 — In Phase 2 the ring is wired from a membership file that no node keeps

**Fork:** F13. **Date:** 12 Sep 2026. **Gates:** Phase 2, the hop-count experiment.

**The decision.** Every node is launched with a file listing every `ip:port` in the ring. At
startup it hashes them, sorts, locates itself, and computes its predecessor, successor and all 64
finger entries with the pure functions in `chord.cpp`. **The list is a local variable inside the
bootstrap function**, so it is destroyed by scope when that function returns — it never becomes a
field of the node. A node's persistent state is only ever the five Chord fields.

**Why.**

1. **It isolates the variable.** Phase 2 asks *"does routing take log₂N hops when the table is
   correct?"* Phase 3 asks *"does stabilisation converge to a correct table?"* Two experiments,
   one variable each — the same principle D-016 used to pin the chunk size. A correct-table hop
   count is also the **"before"** that Phase 3's churn numbers are measured against; without it,
   degradation under churn has nothing to degrade *from*.
2. **Total routing state stays O(N log N) rather than O(N²), and that is the x-axis.** A
   membership entry is ~40 bytes. At 8192 nodes the finger tables routing actually reads cost
   21 MB; a retained membership list in every node costs **2.7 GB** — most of this machine's
   3.4 GiB. D-018 established that maximum ring size is bounded by free RAM and that maximum ring
   size *is* the x-axis of the plot, so retaining the list visibly shortens the curve. Keeping it
   would also make the system's steady state the full-membership design that `SCALE_NOTES.md`
   already records as the rejected alternative to Chord.
3. **It makes one specific bug impossible rather than forbidden.** A list held as node state can
   be scanned directly by `find_successor` to return the true owner in O(1) — correct, passing
   every test, and silently reducing the measured hop count to 1. Lifetime removes the
   possibility; discipline only forbids it. Same principle as D-009.
4. **It answers the obvious objection factually.** *"Your nodes know the whole membership, so the
   hop count is theatre."* → at the moment any lookup is served, the process does not hold the
   membership. That is checkable; "we have it but we don't use it" is not.

**Rejected: option B, successor only, tables built by real lookups.** No node ever holds global
membership, and `fix_fingers` is needed in Phase 3 anyway. Rejected on arithmetic: at startup no
node has fingers, so every lookup degenerates to an O(N) successor walk — a 1000-node ring building
64 fingers each is 64,000 lookups, the early ones up to 1000 hops apiece, so the ring takes longer
to bootstrap than to measure. It also **conflates bootstrap failure with routing failure** in the
phase whose entire output is hop count.

**Rejected: option C, the harness computes every table and hands each node its own.** No amnesia to
explain. Rejected because it creates **two implementations of the finger rule** — harness and node
— that can disagree, and a disagreement produces exactly the silent hop inflation D-019 was chosen
to avoid, plus N config files of 64 lines to keep in step.

**The thing kept out of the node deliberately.** A node holding the list could self-check that the
node it routed to really is the owner. That oracle is worth having — but it belongs in the
**harness**, which has the list and can compute the true owner independently. The checker must not
be the thing being checked, for the same reason D-012 put the measuring instrument outside the
measured system.

**Cost accepted.** The bootstrap path is Phase 2-only scaffolding, deleted in Phase 3 when joins
and `fix_fingers` replace it — roughly 20 lines written to be thrown away. (In the event it was
kept as the `--wired` start mode, D-029.) And the *"you know the whole membership"* objection has
to be answered rather than avoided.

**Evidence:** the hop-count plot (`BENCHMARKS.md` §2) is the evidence, and the membership check
is the harness oracle in `scripts/e2e-node.sh` and `scripts/bench-hops.py`.

---

### D-021 — The node's wire protocol: one framing copy, identifiers on the wire, successor-only ownership

**Fork:** F14 (three parts). **Date:** 13 Sep 2026. **Gates:** the node's first messages, and every Chord message after them.

**F14a — framing lives in a new `net.h` / `net.cpp`, used by the node only.** One tested copy
enforcing D-010 (a reply is exactly one line) and D-011 (a departed peer is an error value, not a
signal). `client.cpp` and `tracker.cpp` keep their own copies for now, and moving them onto it is a
separate, deliberate step.
- *Found while deciding:* the framing layer was **already duplicated**. Client and tracker each
  have a `send_all`, with different signatures, and D-010's guarantee lives in the tracker's copy.
- *Rejected — copying into `node.cpp`:* three copies of invariant-enforcing code, where a fix to
  one does not reach the others.
- *Rejected — migrating all three now:* touches both working binaries and five green suites during
  the routing phase, for about an hour the phase does not have.
- *Cost accepted:* three copies exist until the migration happens. **Logged as a known duplication,
  not an oversight.**

**F14b — `FIND_SUCCESSOR` carries the 16-hex-character routing identifier, not the 40-character
key.**
- Phase 3's `fix_fingers` looks up finger *starts*, which are ring positions with no chunk key
  behind them. A key-carrying message cannot express that lookup and would force a second message
  type.
- This follows I7: routing only ever asks *which node*.

**F14c — a node decides ownership from its successor pointer alone.** The rule is `k ∈ (me,
successor]` → "my successor owns it", otherwise → "ask this closer node". That is the Chord paper's
rule.
- *Rejected — also answering "I own it" when `k ∈ (predecessor, me]`:* it saves a hop only when
  the originator's *first* contact happens to be the owner, about 1/N of lookups, because
  closest-preceding routing always stops short of the owner. The price is that the **predecessor
  becomes correctness-critical**, so a stale predecessor would confidently claim keys it does not
  own.
- It would break the invariant that only the successor pointer must be right, for a negligible
  gain.

**Evidence:** decided at design time. F14a's deferred migration happened in Phase 5 (D-060): every
program now uses `net.cpp`.

---

### D-022 — The system is described as distributed storage, not peer-to-peer file sharing

**Fork:** F17. **Date:** 13 Sep 2026. **Gates:** the README headline and every one-line
description of the project.

**What prompted it.** The question of what the project was turning into. The accurate answer is
that Phase 1's decisions moved it from a BitTorrent-style swarm to **distributed storage**. No single
decision did it; four did together:
- **D-014:** a chunk's location is computed from its hash.
- **D-015:** every chunk has exactly three copies, whatever its popularity.
- **D-017:** nothing records who holds what.
- **D-018:** downloaders sit outside the ring and serve nobody.

The closest real systems are **Dynamo** for placement and **Git** for the data model. The old
description opened with "peer-to-peer", which to a reader who means BitTorrent describes a
different system.

**The decision (option A).** Describe it as what it is: fault-tolerant distributed file storage,
with content-addressed chunks on a Chord ring and three-way replication. The BitTorrent origin
stays in the README as history. "Is it P2P?" has a precise answer: *in Chord's sense, yes —
symmetric nodes, no central index; in BitTorrent's sense, no — placement is by hash with a fixed
replica count, and popularity-scaled capacity was traded for durability.*

**Rejected: B — make "peer-to-peer" true in the BitTorrent sense.** Downloaders would run nodes and
serve chunks they have fetched, so popular files gain sources. Content addressing makes that cache
trivially safe. Rejected because discovering those extra holders needs a placement index — the
thing D-017 rejected — and it is work beyond the minimum viable product.

**Rejected: C — keep the wording, justified in Chord's sense.** The Chord paper's title is
literally *"A Scalable Peer-to-peer Lookup Service"*, so it is true. Rejected because it spends the
first line of every description on a definitional argument instead of on what the system does.

**What the shift cost, stated plainly.** BitTorrent's defining property: **serving capacity that
grows with demand.** A file downloaded a million times still has three copies.

**What it bought.**
- A file survives every one of its uploaders going offline.
- Reads are self-verifying.
- Identical content is stored once.

**The trust model now fits one operator, not volunteers.** Every node is assumed honest, nobody is
incentivised to store strangers' chunks, and nothing is encrypted. On volunteer machines all three
break. This is a scope statement, not a flaw to hide.

**Rejected in hindsight — not weighed at the time: trackerless BitTorrent.** BitTorrent already
replaces its tracker with a DHT: Mainline DHT, a Kademlia variant. Keeping the swarm and moving
only peer discovery into the ring was a real, cheaper alternative. **It was never presented as a
fork in Phase 1**, so this entry is written after the fact and is labelled that way rather than
passed off as deliberation.
- *The reasoning that rejects it now:* Mainline DHT stores **who has the file** — peer lists, which
  go stale as peers come and go — while the data still lives only on seeders. Availability
  therefore still depends on people staying online. This design stores **the data itself** in the
  ring, replicated, so a file outlives its uploaders. That is the fault-tolerance property the
  project exists for.
- *The price:* popularity no longer adds capacity, as above.

**Evidence:** none — this is a positioning decision. The durability claim it rests on is measured
in `BENCHMARKS.md` §6 (reads succeed after replica owners are killed; three copies restored in a
median 2.2 s). The trackerless-BitTorrent comparison remains the least examined part of it.

---

### D-023 — No deletion in the MVP; owner-token unnaming and a mark-and-sweep collector designed for later

**Fork:** F18. **Date:** 13 Sep 2026. **Gates:** the Phase 5 scope and the work after it.

**What prompted it.** Deletion is needed for two reasons: storage would
otherwise grow forever, and a user should be able to remove a file uploaded by mistake. Until now
it had only been settled by accident. D-014 listed "superseded chunks are garbage, no collector" as
a cost, and user-facing delete was never presented as a fork.

**Deletion is two separate problems.**
- **Unnaming** — removing `movie.mp4 → <manifest hash>` from the tracker. This is what a user means
  by "delete".
- **Reclaiming** — freeing chunks that no remaining file references. This is what bounds storage.

**Two traps that make the obvious implementation wrong.**
1. **Deleting a file's chunks can destroy someone else's file.** Content addressing stores
   identical content once. If two people upload the same movie, their manifests list the same
   chunks, so "delete every chunk in my manifest" deletes the other person's file too.
2. **Without access control, `delete` is a vandalism tool.** D-004 dropped users and
   authentication, so anyone who can name a file could remove it. Any delete must at least prove
   ownership of the name.

**The decision (option A now, B and C later).**
- **MVP:** no deletion. Storage grows, and an accidental upload stays downloadable. Both are
  stated limitations, not hidden ones.
- **Later, option B — unnaming with an owner token.** At upload the tracker returns a random
  secret, and delete requires it. This proves ownership without reintroducing user accounts.
  Deleting a name orphans its chunks rather than freeing them.
- **Later, option C — a mark-and-sweep collector, in the style of `git gc`.**
  - *Mark:* every chunk hash reachable from any live name's manifest, manifests included.
  - *Sweep:* each node deletes the chunks it holds that are not in that set.
  - *Grace period:* only chunks **older than a grace period** are deleted. An upload in flight has
    stored its chunks before registering its name, so without this they look unreferenced and are
    deleted mid-upload. Git's prune expiry exists for exactly this race.
  - *A node that is down during a sweep* simply keeps its orphans until the next one. That is a
    leak, never data loss — the safe direction to fail in.
- **Order:** B and C come before Raft in the plan, and are the first work to be deferred if time
  runs short: a collector designed but not built is still a complete design, while a replicated
  tracker is the larger gain in fault tolerance.

**Rejected: option B in the MVP.** About 2–3 h added to Phase 5, already the largest phase, at a
time when the schedule was behind.

**Rejected: B and C both in the MVP.** About 8–11 h. It would almost certainly have displaced
the transfer path and its measurements.

**Rejected outright: per-chunk reference counts.** It puts a mutable counter on every chunk,
replicated three times, which brings back the replica-disagreement problem D-014 removed. A crash
between registering a name and incrementing the counts either leaks chunks or deletes one still in
use — and the second failure is data loss, the unsafe direction.

**Evidence:** none — design time. If built, the numbers owed are storage reclaimed per sweep, sweep
duration against total chunk count, and a test proving a chunk shared by two files survives
deleting one of them.

---

### D-024 — The lookup originator: a transport-agnostic module, and a fresh connection per hop

**Fork:** F19, parts a–e. **Date:** 13 Sep 2026. **Gates:** the `lookup` tool, the Phase 2
benchmark harness, and Phase 5's client. Part c is refined by D-025 (the timeouts are set before `connect`),
and the count left open in part e is fixed by D-026.

**F19a — the loop is a module, `lookup.h` / `lookup.cpp`, with the request/reply exchange passed
in.**
- One function drives a lookup from a starting node to `OWNER` and returns the owner, the nodes
  visited, and an error.
- The single operation "send this line to this peer and return its reply" is a parameter. Real
  sockets supply it in production; tests supply a ring built in process from `route_step`.
- A thin `lookup` binary reads many keys from standard input per run, so a sweep does not start a
  process per lookup. `client` links the same object file in Phase 5.
- *Why:* the cases that most need testing — a reply pointing backwards, a node naming itself, a
  malformed reply — cannot be produced by a correct ring of real processes. With the transport
  injected they are a few lines of constructed data. This extends D-018 from the node to the
  originator: routing logic never touches a socket.
- *Rejected — the loop written in a standalone binary's `main`:* fewest lines now, but testable
  only through real sockets, and Phase 5 would copy it into `client`, the duplication D-021 already
  records for framing.
- *Rejected — the loop written into `client.cpp` now:* that file has its own older framing copy
  (D-021), so this either uses it or starts the deferred migration, and it edits the working
  transfer path during the routing phase.
- *Cost accepted:* two files, a Makefile target, and one level of indirection (`std::function`) on
  a path whose latency Phase 2 does not measure.

**F19b — in Phase 2 each hop opens a fresh connection and closes it after the reply. Connection
reuse is added later, and only if a measured latency figure justifies it.**
- *Why:* the Phase 2 result is hop count, which connection handling cannot change, so reuse buys
  nothing that is measured. And each connect is fresh evidence about exactly one node: under D-013
  a refused connection is proof of death, and one event produces one signal to interpret.
- *Rejected — a connection cache keyed by address:* cheaper per hop, but a cached socket to a node
  that has since died fails on `send` or `recv` (a reset, a broken pipe, or a zero-byte read)
  rather than on `connect`. One event then has several signals, and telling a stale socket from a
  dead node needs a retry on a fresh connection — an ambiguity Phase 3's failure detector would
  inherit.
- *Cost accepted:* a TCP handshake per hop. And the originator closes first, so it holds every
  finished connection in `TIME_WAIT` for 60 s; a sweep can open more connections to one node than
  the 28,232 ephemeral ports (32768–60999) in that window. On this machine
  `net.ipv4.tcp_tw_reuse = 2`, which lets the kernel reuse `TIME_WAIT` ports **for loopback traffic
  only**, so the benchmark is safe here and the limit returns on a real network. Recorded in
  `SCALE_NOTES.md`.

**F19c — a fixed 5 s receive and send timeout on every originator socket, as a hang guard. A
connect deadline is scheduled into Phase 3.**
- `SO_RCVTIMEO` and `SO_SNDTIMEO` make a blocked `recv` or `send` give up. A timeout is reported as
  its own error, naming the node, and never merged with a refused connection: D-013 treats the two
  as different evidence.
- 5 s is a backstop set far above any loopback reply, not a failure-detection threshold. Phase 3
  replaces it with a measured value.
- *Why this covers Phase 2 completely:* on loopback a connection to a missing node is refused at
  once, and a frozen node's kernel still completes the handshake, so the only possible hang is at
  the read.
- *What it does not cover* (as first written; **D-025 closes this on Linux**): a machine that
  vanishes on a real network, where nothing answers the handshake. With `tcp_syn_retries = 6` on this kernel, `connect` blocks for about 127 s
  (1 + 2 + 4 + 8 + 16 + 32 + 64), calculated from the setting rather than measured.
- *Scheduled:* a non-blocking connect with a `poll` deadline, inside the single successor-call
  wrapper D-013 already requires, in **Phase 3**, estimated at 45 min. That is where the deadline's
  value is measured and where killed nodes make it testable.
- *Rejected — no timeout:* a stuck node hangs a benchmark silently, and an outer `timeout` kills the
  run without saying which node stalled.
- *Rejected — a non-blocking connect with a deadline now:* about 45 min added to a phase already
  behind schedule, a deadline value chosen before Phase 3 can measure one, and probably untestable here,
  because it needs an address that silently drops packets.

**F19d — every reply is checked for progress, and a lookup is capped at 65 requests.**
- A `NEXT` reply must name a node strictly inside `(current, k)`. An `OWNER` reply must name a node
  with `k ∈ (current, owner]`. Anything else fails the lookup with an error naming the node that
  sent the reply.
- `route_step` satisfies both even with stale tables: it only picks fingers inside `(self, k)`, and
  its successor fallback lies in that arc whenever the node is not the owner. So a violation means
  a buggy or dishonest node, never merely a stale one.
- The distance to `k` strictly shrinks, so no node can be visited twice. A cycle is impossible by
  construction rather than detected after the fact.
- *The cap.* Let `p` be `k`'s predecessor. With correct fingers, each `NEXT` more than halves the
  remaining clockwise distance to `p`: if `p` lies in `[n + 2^i, n + 2^(i+1))`, finger `i` lies in
  `[n + 2^i, p]`, so the jump covers at least `2^i`, which is more than half of `p − n`. That distance
  starts below 2^64, so at most 64 `NEXT` replies reach `p`, and `p` answers `OWNER`: **65 requests**.
  More than that on a fixed ring is a bug. Phase 3 revisits the value, because stale fingers can
  legitimately need more.
- *Rejected — a fixed hop cap alone:* the value would be arbitrary, and exceeding it says nothing
  about which node misbehaved.
- *Rejected — a visited set:* it catches a repeated node exactly, but not a node that keeps naming
  new addresses, and it reports a symptom rather than the node at fault.

**F19e — the loop records every node it queried; which count is plotted is decided before the hop-count benchmark (D-026).**
- Requests sent and node-to-node jumps differ by exactly one on every lookup, so both derive from the
  recorded path.
- The plotted definition is fixed before the first benchmark number is taken, against the Chord
  paper's own definition of path length.
- The path is also what names the failing node in Phase 3 (D-012, reason 2), so recording it costs
  nothing extra.
- *Rejected — fixing one count now:* it would choose a definition before checking the one it is
  compared against, and an off-by-one shifts every point on the plot.

**Evidence:** `test-lookup`'s in-process tests with constructed replies (70 checks, including
backwards, self-naming, overshooting and garbled replies and a ring that needs exactly 65 requests)
are the evidence for F19a and F19d.

---

### D-025 — The originator's timeouts are set before `connect`, so one 5 s deadline also bounds the handshake

**Fork:** F20, surfaced while building the lookup tool. **Date:** 7 Oct 2026. **Gates:** the socket
transport in `net.cpp`, and the connect-deadline item D-024 scheduled into Phase 3.

**The decision.** The transport creates the socket, sets `SO_RCVTIMEO` and `SO_SNDTIMEO` to 5 s,
and only then calls `connect`. On Linux a blocking `connect` honours the send timeout: socket(7)
states that when it expires, `connect` fails with `EINPROGRESS`. That error is reported as a
**timeout**, the same class as a stalled read, and never as a refusal.

**Checked on this machine before deciding**, with a 2 s timeout:

| Target | Timeout set before `connect` | Result |
|---|---|---|
| `127.0.0.1:1` (nothing listening) | yes | `ECONNREFUSED` after 0.00 s |
| `10.255.255.1:80` (nothing answers) | yes | `EINPROGRESS` after 2.01 s |
| `192.0.2.1:80` (documentation range, nothing answers) | yes | `EINPROGRESS` after 2.01 s |
| `192.0.2.1:80` | no | still blocked when killed at 8 s |

**Why.** D-024 F19c recorded that a vanished host blocks `connect` for about 127 s on this kernel,
and scheduled a non-blocking connect with a `poll` deadline into Phase 3. Setting the same options
one line earlier closes that gap now, on the platform this project runs on, with no new code path.
What is left for Phase 3 is choosing the value from a measurement, not building the mechanism.

**Rejected: set the options after `connect`.** This is what the D-024 text implied. It changes
nothing on loopback, but it keeps the 127 s hang on a real network for no benefit.

**Rejected: a non-blocking `connect` with a `poll` deadline, now.** It is portable to kernels that
ignore `SO_SNDTIMEO` during `connect`, and it would allow a connect deadline separate from the read
deadline. On Linux it behaves identically, and it is about 45 minutes of code with its own error
paths (`getsockopt(SO_ERROR)` after `poll`).

**Cost accepted.**
- **The behaviour is Linux-specific.** POSIX does not promise that `connect` honours `SO_SNDTIMEO`,
  so on another kernel the 127 s hang returns. The project builds and runs only on Linux.
- **`EINPROGRESS` on a blocking socket is surprising.** It normally means "a non-blocking connect
  has started". The mapping is commented where it happens, because a reader who does not know
  this rule would classify it as an unknown failure.
- **One value covers connect, send and receive.** Phase 3 may want a shorter connect deadline than
  read deadline; that split is the point where the `poll` version earns its cost.

**Evidence:** the table above, and a regression test. `test-net` connects to `192.0.2.1:80` with a
300 ms deadline and requires a timeout within 2 s. With the options moved after `connect` on purpose
(a mutation test), that same call blocked for **133.7 s** before failing. That measures the hang this
decision removes; the 127 s in D-024 was calculated from `tcp_syn_retries`. The 5 s value itself
remains a backstop, not a failure-detection threshold (D-024 F19c).

---

### D-026 — A hop is a finger followed: the plotted hop count is requests minus one

**Fork:** F19e. **Date:** 7 Oct 2026. **Gates:** every number on the hop-count plot.

**The decision.** For each lookup the originator records the path of every node it queried. Two
counts come from it, and both are kept in the raw data:

- **requests** — the number of `FIND_SUCCESSOR` messages sent, which is the path length in nodes
  queried;
- **hops** — the number of `NEXT` replies followed, which is **requests − 1**.

**The plotted and quoted figure is hops.**

**Why: it is the quantity the Chord paper measures.** Stoica et al. (SIGCOMM 2001, §6.3) define
path length as "the number of nodes traversed during a lookup operation", and explain the observed
mean of about ½ log₂ N this way: "the number of fingers we need to follow will be the number of ones
in the binary representation of the distance from node to query. Since the distance is random, we
expect half the log N bits to be ones." So the counted unit is a **finger followed**.

Mapping that onto this design, step by step:

1. In the paper the query starts **at** a ring node, which costs no traversal. Here the originator
   sits outside the ring (D-018), so its first request reaches the start node. That request
   corresponds to the paper's starting point, not to a finger.
2. Each `NEXT` reply is one finger followed: the node handed the lookup to its closest preceding
   finger.
3. The lookup ends when the key's predecessor answers `OWNER` with its successor. The owner itself
   is never contacted, in the paper or here.

So fingers followed = `NEXT` replies = requests − 1.

**A numerical check, not only a reading.** On 1024 random nodes the in-process routing simulation
measures a mean of 5.90 requests, which is 4.90 hops, against ½ log₂ 1024 = 5. Counting requests
would sit about one above the paper's curve at every N, an offset produced by definitions rather
than by routing.

**Rejected: plot requests.** Requests are what a client pays in round trips, so they are the
latency-relevant count, and they are reported alongside. As the plotted figure they would shift
every point up by one against the paper's curve, and anyone comparing the two would see a constant
gap with no routing explanation.

**Rejected: count the data fetch from the owner.** That is a Phase 5 transfer, not routing, and the
paper does not count it.

**Cost accepted.**
- **The paper's wording is looser than its explanation.** "Nodes traversed", read literally, could
  include the start node. The decision follows the paper's own derivation of ½ log₂ N, and the
  simulation agrees with that reading. The ambiguity is stated here rather than hidden.
- **Earlier test outputs counted requests.** The first routing-test figures (mean 2.37 on 8 nodes,
  5.90 on 1024 in-process) were requests. Test output now labels those counts "requests", so the two
  counts never share a name.

**Evidence:** the 1024-node simulation figure above; the full curve is `BENCHMARKS.md` §2, where
hops sit at 0.10–0.19 below ½ log₂ N at every ring size from 32 to 2²⁰, and requests sit one above
hops at every point.

---

### D-027 — The hop-count curve: real processes to a measured memory ceiling, then the same code in one process to 2²⁰ nodes

**Fork:** F16. **Date:** 7 Oct 2026. **Gates:** the hop-count benchmark.

**The decision.** The plot carries two series, each drawn from the same kind of workload:

1. **Real processes.** N `node` processes on loopback, queried by the `lookup` tool over TCP, for
   N in powers of two up to a ceiling **derived from measured per-node memory** rather than chosen.
2. **In-process replay.** `sim-hops` parses a membership file with `parse_members`, builds each
   asked node's table with `build_fingers`, decides its answer with `route_step`, and runs the loop
   with `lookup()`, the same function the `lookup` tool runs. Only the transport differs: a function
   call instead of a connection. N runs from 2 to 2²⁰ (1,048,576).

Agreement is shown two ways. **Every real ring is also replayed in `sim-hops` with the identical
workload, and every lookup must return the same owner by the same path, node for node.** Separately,
the two curves overlap on the plot wherever both exist.

**Why.**
- **Hop count is a deterministic function of the membership, the start node and the key.** The
  process boundary adds a transport and nothing else. The transport is tested on its own
  (`test-net`, `e2e-chord.sh`), so what the replay leaves out is covered elsewhere.
- **A logarithm needs doublings to be visible.** Processes on this machine reach ten doublings
  (2 to 2048 nodes, measured in `BENCHMARKS.md` §2); the replay reaches nineteen (2 to 2²⁰). A curve
  that keeps tracking ½ log₂ N for nineteen doublings is a much stronger claim than one that stops
  where memory does.
- **The replay is not a model.** It is the shipped routing code with a different transport, so a
  disagreement with the real ring would be a real bug, not a modelling error.

**Rejected: real processes only.** No simulation to justify, but the curve stops at the memory
ceiling, which is a property of this laptop rather than of the design.

**Rejected: simulation only.** It reaches any N cheaply, but the plot would then never touch the
system that actually runs, and "a simulation of the design" is not evidence that the binaries
behave that way.

**Rejected: a faster purpose-built simulator** (integer indices instead of addresses, fingers
precomputed in flat arrays). It would be several times faster, and it would be **a second
implementation of the finger rule**, the thing D-020 refused because two implementations can
disagree silently and inflate hop counts.

**Cost accepted.**
- **The simulated series measures routing, not the network.** It is labelled as in-process on the
  plot and in `BENCHMARKS.md`.
- **`sim-hops` renders replies itself**, three lines that mirror `node.cpp`. A change to the node's
  rendering would not reach the simulator by itself. The per-lookup replay of every real ring is what
  would catch such a divergence.
- **`sim-hops` rebuilds a node's 64 fingers each time that node is asked**, trading CPU for memory:
  64 fingers kept for each of 2²⁰ nodes would be about 3 GB, more than the memory budget.

**Evidence:** `BENCHMARKS.md` §2, at `e042857`. Every one of the 135,168 lookups on real rings (2 to
2048 nodes) was reproduced exactly by the replay, and the curve reaches 9.87 hops at 2²⁰ nodes.

---

### D-028 — An identifier collision at join is refused, not repaired

**Fork:** F15. **Date:** 7 Oct 2026. **Gates:** the join path (Phase 3).

**The decision.** Two parts, both comparing addresses for identity (I8).

1. **At join.** A joining node looks up its own identifier. If the owner it is given carries the
   same identifier at a different address, the node refuses to join: it exits with an error
   naming both addresses, and the operator starts it on another port.
2. **Always.** No node adopts a peer that carries its own identifier at another address — not as
   successor, predecessor or finger — and it logs the attempt.

**Why.**
- **The probability is negligible, and it was computed rather than assumed.** By the birthday
  bound, 100,000 identities in 2⁶⁴ collide with probability about 3 × 10⁻¹⁰ (D-019). An event that
  rare needs a loud, correct response, not an automatic one.
- **Refusing keeps the identifier a pure function of the address.** Every node, every test oracle
  and every harness computes SHA-1("ip:port") and gets the same answer, with nothing extra stored
  or carried.
- **The join lookup already returns the information.** If a node with my identifier exists, it is
  the owner of my identifier, so the lookup a join performs anyway finds it.

**Rejected: re-hash with a salt** (identifier = SHA-1("ip:port#n"), raising n until it is free).
Automatic, but the identifier is no longer derivable from the address. Every harness oracle would
have to learn each node's salt from the node itself, which puts the node's own report inside the
checker. And two colliding nodes joining at the same moment both see no collision, so salting at
join is not complete either.

**Rejected: rely on the birthday bound alone**, as the Chord paper does. Free, but the failure is
silent: two nodes sharing an identifier disagree about a boundary, and nothing reports it.
Detection costs one comparison on a lookup the join performs anyway.

**Cost accepted.**
- A collision stops a node from starting and needs an operator. At 3 × 10⁻¹⁰ that is accepted.
- **Two colliding nodes joining at the same moment are not caught at join**: each looks before the
  other is reachable. Part 2 contains that case — neither adopts the other, so neither concludes it
  owns the whole ring — but the arc between them stays ambiguous until one is restarted. Logged,
  not repaired.

**Evidence:** in-process tests construct the collision with chosen identifiers, since real SHA-1
output never produces one.

---

### D-029 — A node starts one of four ways, and listens only once it has a successor

**Fork:** F21. **Date:** 7 Oct 2026. **Gates:** `node`'s command line, and every Phase 3 test
and benchmark.

**The decision.**

| Command | Starting state | Maintenance thread |
|---|---|---|
| `node <ip> <port> <membership-file>` | Phase 2's fixed ring, wired from the file (D-020) | none — unchanged, so the hop-count benchmark and its suites run as before |
| `node <ip> <port> --create` | a ring of one: the successor is the node itself, no predecessor | yes |
| `node <ip> <port> --join <ip:port>` | successor found by looking up the node's own identifier through the known node; no predecessor | yes |
| `node <ip> <port> --wired <membership-file>` | the fixed-ring state plus a successor list, then maintained | yes |

The join lookup is `lookup()` from `lookup.cpp`, the loop the client runs. **A joining node does
not listen until that lookup has returned a successor**, so until then a connection to its address
is refused. A join that cannot complete is retried, one period apart, up to 30 times, and then the
node exits with an error.

**Why.**
- **The fixed ring stays as a mode** because the Phase 2 result is defined on it (D-020), and the
  hop-count benchmark, `e2e-node.sh` and `e2e-chord.sh` need tables that do not change under them.
  `e2e-chord.sh` kills a node and checks the path that still leads to it; a maintained ring would
  repair that path away.
- **The wired, maintained mode gives the reconvergence benchmark a known, correct starting state in
  seconds.** A 256-node ring built by joins is itself a convergence experiment, because every early
  table is stale, so a kill measured on top of one would measure two things at once. That is D-020's
  argument applied to Phase 3.
- **Joins are what the system does in use**, so `--create` and `--join` are tested end to end with
  real processes, including joins into a running ring and a node rejoining at its old address.
- **Listening only after the join removes a real hazard.** A node restarted at the same address
  before the ring notices its death is still named by its predecessor. If it answered while it still
  believed it was alone, `(n, n]` would make it claim every key, and the join lookup routed through
  it would return itself. Not listening means its old entry is refused, which is proof of death
  (D-013), so the ring evicts it, and the join is retried until the answer is no longer its own
  address.

**Rejected: joins only**, every ring built by joining. One code path, but the benchmark's starting
state would depend on how well a mass join had converged, and the Phase 2 suites would lose the
fixed tables they test against.

**Rejected: remove the fixed ring.** The Phase 2 numbers could no longer be reproduced with the
current binary.

**Rejected: listen first, then join** (the paper's order). Simpler, and wrong under a fast restart,
for the reason above.

**Cost accepted.**
- Four starting modes: a longer `main`, and every mode needs a test.
- The wired mode reads a membership file, which a deployed ring would not have. It exists for the
  benchmark's starting state, and the list still dies by scope (D-020).
- A join gives up after 30 attempts. A ring that cannot answer one lookup for that long is
  reported, not waited on forever.

**Evidence:** the join, rejoin and concurrent-join cases are tested in `scripts/e2e-churn.sh` and
in process (`test-stabilize`).

---

### D-030 — One mutex per node's routing state, never held across a network call

**Fork:** F22. **Date:** 7 Oct 2026. **Gates:** the node's concurrency model from Phase 3 on.

**The decision.** A node's mutable routing state — predecessor, successor list, fingers, and the
failure detector's strikes — sits behind one `std::mutex`. Handler threads take it to compute a
reply (`route_step` runs under it) or to copy what they need. The maintenance thread takes it to
copy state before a call and to apply the result after. **No thread holds it during a network
call.** Because the state can change while a call is in flight, every result is applied only if the
state it was computed from is still current — a stabilise answer, for example, is applied only if
the node it was asked of is still the successor — and is otherwise discarded, to be asked for again
next round.

**Why.**
- **Phase 3 adds a writer.** In Phase 2 every field was written before the first reader thread
  existed, so no lock was needed. Now the maintenance thread rewrites the successor while handler
  threads read it, and copying a `std::string` while another thread assigns it is undefined
  behaviour — the copy can read a buffer that has just been freed — not merely a stale value.
- **The critical sections are microseconds**: a 64-entry scan, or a copy of a few addresses.
  Time goes on network calls, and those are outside the lock.
- **Not holding the lock across I/O keeps one slow peer from stalling every request.** A maintenance
  call to a frozen node can take the whole deadline; with the lock held, every lookup the node
  answers would wait that long.
- **One lock has no lock order, so it cannot deadlock with itself.** The in-process tests also
  enforce the I/O rule: they run every node on one thread, so a call made while holding the lock
  would re-lock the same mutex — formally undefined behaviour, a hang in practice — and the test
  would not pass.

**Rejected: `std::shared_mutex`** (many readers or one writer). Readers would not block each other,
but they barely hold the lock, and a shared mutex costs more per acquisition than a plain one. It
buys concurrency where there is no measurable contention.

**Rejected: copy-on-write snapshots** (a `std::shared_ptr<const State>` swapped atomically). Readers
never block, but every update copies the whole state, and C++17's atomic `shared_ptr` functions are
implemented in libstdc++ with a hidden pool of mutexes, so it is a lock plus a copy.

**Rejected: hold the lock for a whole maintenance round.** The easiest to reason about, and it
would queue every lookup behind a network call.

**Cost accepted.**
- **The check-then-act gap is real**: state read before a call may be stale when the reply arrives.
  It is handled by re-validating at every point a result is applied — a discipline written into
  each one, and therefore the place a mistake would go.
- Handlers and the maintenance thread contend on one lock. Not measured, because nothing suggests it
  matters at these sizes.

**Evidence:** a ThreadSanitizer build of the node (`make tsan`), driven through the churn suite.

---

### D-031 — Phase 3's messages: `NEIGHBOURS`, `NOTIFY`, `SUSPECT`, and a read-only `FINGERS`

**Fork:** F23. **Date:** 7 Oct 2026. **Gates:** the node protocol (extends D-021).

**The decision.** Four new requests, each one line answered by one line (D-010). A *peer* on the
wire is `<16-hex id> <ip> <port>`, exactly as in `FIND_SUCCESSOR` replies.

| Request | Reply | Sent by |
|---|---|---|
| `NEIGHBOURS` | `NEIGHBOURS <pred> <n> <succ_1> … <succ_n>`, where `<pred>` is a peer or `NONE` | a node's stabilise, to its successor |
| `NOTIFY <peer>` | `OK` | a node's stabilise, to its successor: "I believe I am your predecessor" |
| `SUSPECT <ip> <port>` | `OK` | an originator, to the node that named a node it could not reach (D-036) |
| `FINGERS` | `FINGERS <n>` then `<i> <peer>` per run of equal fingers, `i` being where the run starts | test and benchmark harnesses only |

Replies are parsed as strictly as lookup replies: single spaces, exact word counts, canonical
addresses, ports without sign or leading zero.

**Why.**
- **One round trip per stabilise round instead of two.** The paper asks the successor for its
  predecessor and, separately, for its successor list. `NEIGHBOURS` returns both from one snapshot
  taken under the successor's lock, so the two cannot contradict each other.
- **Messages carry identifiers** rather than having the receiver hash the address, as
  `FIND_SUCCESSOR` replies already do (D-021). Deriving an identifier stays the business of the node
  that owns it, which is exactly what Phase 4's virtual nodes change.
- **`SUSPECT` is a hint, not a verdict.** The receiver probes the named node itself and acts only on
  its own evidence. A message that evicted a node on someone else's word would let any client
  remove any node.
- **`FINGERS` is read-only and exists for the harness**, which decides whether a table is correct by
  comparing it with an oracle computed from the live membership (D-038). Runs keep it short: about
  log₂ N distinct fingers rather than 64 lines of repeats.

**Rejected: separate `GET_PREDECESSOR` and `GET_SUCCESSORS`.** The paper's shape: two connections per
round, and two snapshots that can disagree.

**Rejected: receivers recompute identifiers from addresses.** It would catch a node lying about its
identifier, and it would fix the derivation into every receiver.

**Rejected: no finger query, with finger convergence inferred from lookups.** A lookup sweep
confirms routing (D-038 runs one), but it cannot say which finger of which node is wrong.

**Cost accepted.**
- **A line is at most 1024 bytes (`MAX_LINE`), so the successor list is capped at 16 entries.** A peer
  is at most 39 bytes on the wire (16 + 1 + 15 + 1 + 5, plus a separator), so 16 entries and a
  predecessor fit with room to spare. A `FINGERS` reply that would not fit is refused with `ERR`;
  that takes more than about 24 distinct fingers, which means rings of millions of nodes.
- Identifiers are believed as stated, as they already are in lookup replies.

---

### D-032 — The successor list holds 8 entries by default

**Fork:** F24. **Date:** 7 Oct 2026. **Gates:** survival of failures, and the replica set in
Phase 4.

**The decision.** Each node keeps the next r live nodes clockwise, obtained each round by taking
its successor's list, putting the successor in front, and cutting the result at r. r is 8 by
default and set with `--succ <1..16>`.

**Why.**
- **The list is what lets a node survive its successor's death**: it moves to the next live entry in
  the same round. A node is orphaned only if all r successors die within one stabilisation period.
- **r enters as an exponent.** If nodes fail independently, each with probability p within a period,
  a node is orphaned with probability pʳ. If half the ring fails at once, (½)⁸ = 1/256. The Chord
  paper's choice, r = O(log N), gives about one orphan in expectation after that event when
  r = log₂ N, and 8 is log₂ of 256, the largest ring the Phase 3 benchmark runs.
- **It is also the replica set**: D-015 places a chunk on its owner and the next two nodes, so r must
  be at least 2, and 8 leaves room to fail over after replicas die.

**Rejected: r = 3**, the replication minimum. Two adjacent deaths, the common case when many nodes
fail together, would leave a node one failure from orphaned.

**Rejected: r estimated at run time from the gap to the successor** (N ≈ 2⁶⁴ ÷ gap). It adapts to
ring size, but one gap is a noisy estimate, wrong by a factor of several, and a list length that
changes from round to round makes convergence harder to measure.

**Cost accepted.**
- Eight peers on every `NEIGHBOURS` reply, about 320 bytes per stabilise round.
- **After a death, a list takes about r rounds to be correct everywhere**, because each node learns
  the change from its successor one round after its successor does. Measured as the second milestone
  of D-038.
- **The exponent assumes independent failures.** Correlated failures — one rack, one switch — defeat
  it, and placement by hash cannot be steered around racks.

---

### D-033 — Leaving is crashing: there is no departure protocol

**Fork:** F25. **Date:** 7 Oct 2026. **Gates:** how a node leaves the ring.

**The decision.** A node leaves by stopping. `SIGTERM` and `SIGINT` keep their default action: the
process ends, the kernel closes its listening socket, and the next connection to its address is
refused. Its neighbours repair the ring exactly as they repair any death.

**Why.**
- **The crash path must exist and be fast whatever else is built**, because crashes are not
  announced. A departure protocol would be a second path to the same end state, exercised far less
  often than the first.
- **A clean exit is already the best case of a crash.** A refused connection is proof of death
  (D-013), so the predecessor evicts the node at its next stabilise without the second strike a
  timeout needs. A departure message would save at most one period.
- **It is the crash-only design argued by Candea and Fox**: if stopping and crashing are the same
  event, the recovery code runs every time anything stops, and so it stays correct.

**Rejected: a graceful leave** (`LEAVING` messages telling the predecessor its new successor and the
successor its new predecessor). It saves up to one period per planned departure. It costs signal
handling — a handler may call only async-signal-safe functions, so it needs a self-pipe or a
dedicated `sigwait` thread — two more messages, and the case where those messages are lost, which
falls back to the crash path anyway.

**Rejected for now: leaving with a data hand-off.** Moving a node's chunks to its successor before
it exits is what makes a graceful leave worth having, and it belongs with replication in Phase 4,
where three replicas already cover a departure.

**Cost accepted.** A planned departure leaves the ring briefly wrong — the predecessor names the
departed node as an owner until its next stabilise — exactly as a crash does. It is the same event
as the benchmark's kill, so it is measured.

---

### D-034 — `fix_fingers`: one lookup per round, and its answer fills every finger it covers

**Fork:** F26. **Date:** 7 Oct 2026. **Gates:** how long a stale finger survives; background load.

**The decision.** Each round, a node looks up the start of the next finger it has not refreshed,
`finger_start(n, i)`, with the same `lookup()` loop clients use, starting at itself. The owner `o`
fills finger `i` **and every following finger whose start also lies in `(n, o]`**, because they have
the same owner. The next round continues after the last finger filled, wrapping back to finger 1.
Finger 0 is the successor, and stabilise sets it.

**Why.**
- **Fingers are mostly repeats.** Of 64 starts only about log₂ N have distinct owners: the low
  fingers start a few units past `n` and all resolve to the successor. Filling forward refreshes a
  whole table in about log₂ N rounds instead of 64.
- **It is exact, not a heuristic.** If `o` is the first node at or after start `i`, and start `j`
  lies between start `i` and `o`, then no node lies between start `j` and `o` either, so `o` owns
  start `j` too.
- **One lookup per round keeps the background cost the paper's.**
- **A failed `fix_fingers` lookup is evidence too** (D-013): if it ends in a refusal, the node that
  refused is evicted from this node's tables, because a refusal is proof.

**Rejected: one finger per round** (the paper). 64 rounds per refresh — 64 s at a 1 s period —
most of them re-confirming that the low fingers equal the successor.

**Rejected: every finger every round.** The fastest repair, at 64 lookups per node per period,
permanently, to speed up a rare event.

**Cost accepted.**
- A finger pointing at a dead node survives until the cycle reaches it — about log₂ N periods —
  unless a refused lookup or a `SUSPECT` hint finds it first. Measured as D-038's third milestone.
- The lookup starts at the node itself, which costs one loopback connection to its own listener. It
  keeps one lookup path (the client's) instead of adding a special case.

**Refined while building, 7 Oct 2026.** A finger whose lookup fails is **skipped until the next
cycle**, not retried every round. The first version retried the same finger, so one finger whose
repair kept failing stopped every later finger from ever being refreshed — seen on a real ring as a
single wrong finger that persisted for 20 s after a node froze. Cost: a finger whose lookup fails
transiently waits one cycle, about log₂ N periods, instead of one. This and D-035's per-finger strikes
are independent defences: the in-process test of this stall passes with either one removed and fails
only with both removed.

---

### D-035 — Failure handling: one strike for a refusal, two for anything else, and nothing adopted unverified

**Fork:** F27, the mechanics D-013 left open. **Date:** 7 Oct 2026. **Gates:** how fast a dead
node leaves the ring, and whether it can come back by accident.

**The decision.** Every call a node makes to another node goes through one function that reports the
outcome to the failure detector — D-013's answer to "every call site must remember".

1. **A refusal evicts at once.** Anything else that is not a valid reply — a timeout, a closed
   connection, an `ERR`, a garbled line — is a **strike**. Two consecutive strikes against the
   successor or the predecessor evict it. A valid reply clears the strikes.
2. **Eviction removes the node everywhere**: successor list, predecessor, fingers. A finger that
   pointed at it is replaced by the node that followed it in the successor list when that is known,
   and otherwise by the successor, which is always a valid if slower next hop.
3. **When the successor is evicted, stabilise moves to the next entry in the same round**, so k
   adjacent refused nodes cost one round, not k.
4. **A candidate successor is verified before it is adopted.** When the successor reports a
   predecessor `x` lying between this node and the successor, this node pings `x` and adopts it only
   if `x` answers.
5. **A `NOTIFY` from a node that is not closer than the current predecessor triggers a ping of the
   current predecessor**, and the notifier replaces it if that ping is refused.
6. **The predecessor is pinged every round** (the paper's `check_predecessor`), under the same rules.
7. **If every entry in the successor list is gone**, the node takes the nearest node it still knows
   clockwise — among its fingers and predecessor — and stabilise tightens the pointer inward from
   there. Only a node that knows nobody falls back to itself.

**Why each part.**
- **1** is D-013(b): a refusal is proof, a timeout is suspicion, and the second strike is "a second
  independent failure, or confirmation by the next stabilise".
- **4** closes a window the paper leaves open. P evicts dead X and moves to S, but S still names X as
  its predecessor until S notices; the paper's stabilise would then adopt X again, and P would point
  at a dead node for another round. The ping costs one connection, only when the successor is about
  to change, which is rare.
- **5** is opportunistic detection: a notify from an unexpected node is evidence that something
  changed, so it is checked when it arrives instead of at the next round.
- **7**: falling back to itself would make the node claim the whole ring, `(n, n]`. Any known node
  clockwise is a safe start, because stabilise only ever moves the pointer inward.

**Rejected: the paper's mechanics alone** (stabilise and `check_predecessor`, no verification, no
probe on notify). Fewer messages, slower repair, and a window in which a dead node is re-adopted.

**Rejected: evict on one timeout.** Faster for frozen nodes. On this machine every node shares eight
cores, so a scheduling stall would evict live nodes — D-013's argument.

**Cost accepted.**
- A frozen node is evicted after two strikes: about two periods plus two deadlines, not one.
- After more than r adjacent deaths, recovery from fingers depends on the predecessors of the nodes
  fallen back on being right. Likely, not guaranteed.

**Refined while building, 7 Oct 2026 — four corrections, each found by a test rather than by review.**
- **Strikes count against every peer a node holds — successor, predecessor or finger — not only the
  two neighbours.** Found on a real ring (`e2e-churn.sh`): after a node was frozen, a third node's
  finger kept pointing at it for 20 s. Repairing a finger is a lookup, the lookup was routed through
  the frozen node by another stale finger, it timed out, and a timeout against a mere finger target
  counted for nothing, so the same failure repeated every round.
- **Any failure the last node asked can be blamed for is a strike against it** — no answer, `ERR`, a
  garbled or backwards reply — not only an unreachable node. Otherwise a finger pointing at an orphan,
  which answers `ERR` (D-040), could never be repaired.
- **When a call evicts the successor for any reason — a refusal or a second strike — stabilise moves to
  the next entry in the same round.** The first version did this only for a refusal; a frozen
  successor then cost one more round than necessary. Found by the in-process frozen-node test.
- **A `NOTIFY` probe that leads to the old predecessor's eviction, by refusal or by a second strike,
  installs the notifier.** The first version did so only on a refusal, which left the slot empty for
  a round after a second strike. Same test.

---

### D-036 — An originator that reaches a dead node tells the node that named it, then retries

**Fork:** F28. **Date:** 7 Oct 2026. **Gates:** lookup success during churn, and whether D-013's
detection adapts to load under iterative routing.

**The problem.** D-013 relied on natural traffic to speed up detection: any failed call to a
successor is reported to the detector. But under iterative routing (D-012), lookup traffic does not
flow between nodes. The originator talks to every node itself, so a node never sees a client's
failed connection. Left alone, client lookups would add nothing to detection in Phase 3, and
reconvergence under load would equal reconvergence when idle. The two decisions, each sound alone,
conflicted, and it surfaced only when the stabilisation path was designed.

**The decision.** `lookup_with_recovery()` wraps `lookup()`. If a lookup fails because a node it
was sent to could not be reached — refused, timed out or closed — and an earlier node had named it,
the originator sends `SUSPECT <ip> <port>` to the node that named it, then retries the whole lookup
from the start, up to a given number of attempts. The node receiving `SUSPECT` pings the suspect
itself and evicts it only if the ping is refused (D-031). `lookup()` is unchanged, so the Phase 2
benchmark and `sim-hops` are unaffected. The `lookup` tool takes `--retries <n>`; the default 0 is
Phase 2's behaviour.

**Why.**
- **It makes detection load-adaptive again.** On a busy ring the node holding a dead successor or
  finger learns of it within one client round trip, and the retry routes around it.
- **The node acts only on its own evidence.** A client cannot evict anyone by claiming they are
  dead; it can only cause a ping.
- **Retrying from the start needs no change to `FIND_SUCCESSOR`** and leaves the progress check
  exactly as it was.

**Rejected: fail the lookup** (Phase 2's behaviour). Simple and honest, and every lookup through a
dead node then fails until maintenance reaches it — for a finger, up to about log₂ N periods.

**Rejected: retry without telling anyone.** The retry takes the same path to the same dead node
until the ring repairs itself, so it only adds waiting.

**Rejected: resume at the previous node with an exclusion** (`FIND_SUCCESSOR k AVOID <node>`), the
paper's "try the next best finger". It saves the hops before the failure, and changes the request
format, the node's routing decision and the progress check, to save a few round trips.

**Cost accepted.**
- A retried lookup pays its earlier hops again.
- `SUSPECT` lets any client make a node send one ping per message; there is no rate limit.
- **A dead owner is never contacted during routing** — its predecessor answers `OWNER` for it — so a
  lookup for a dead node's keys returns that node until the predecessor stabilises. Phase 5's data
  fetch is what contacts the owner, and its failure will feed the same hint.

---

### D-037 — The 65-request cap stays during churn

**Fork:** F29 (D-024 asked Phase 3 to revisit the cap). **Date:** 7 Oct 2026. **Gates:** lookups and
`fix_fingers` while tables are stale.

**The decision.** `MAX_REQUESTS` stays 65. A lookup that needs more fails with `too-many-requests`,
and the churn benchmark counts how often that happens.

**Why.**
- **65 is derived and tight for correct fingers** (D-024). With stale fingers the progress check
  still guarantees termination — the distance to the key strictly shrinks, so no node is asked
  twice — but the bound becomes N, a walk along successor pointers.
- **A lookup needing more than 65 requests during churn is evidence of a badly stale ring.** Failing
  it with the reason named is more useful than waiting through a walk of the ring, and with D-036's
  retry the next attempt may take a repaired path.
- **The cap is also the defence against a node that keeps naming new, invented nodes** (the
  creeping-liar test). Raising it lengthens that.

**Rejected: raise the cap** (to 1024, say). Fewer failures during heavy churn, a weaker defence, and
a number with no derivation.

**Rejected: no cap, relying on the progress check.** Termination is guaranteed only if the nodes
named are real; a liar can name new identifiers for ever.

**Cost accepted.**
- A ring built by many simultaneous joins, before `fix_fingers` has run, can need more than 65
  requests for some lookups — joins included, which are retried (D-029). The benchmark's wired start
  avoids this.
- `fix_fingers` uses the same loop and the same cap; a lookup that fails is retried next round.

**Evidence:** the count of `too-many-requests` failures in the churn benchmark.

---

### D-038 — "Reconverged" is three milestones, judged by an oracle in the harness

**Fork:** F30. **Date:** 7 Oct 2026. **Gates:** the reconvergence benchmark (`BENCHMARKS.md` §4).

**The decision.** After a node is killed, the harness computes the correct state of every live node
from the set of live addresses alone — SHA-1 of each, sorted — and polls every live node until its
state matches. It records three times:

1. **Ring correct** — the headline. Every live node's successor and predecessor equal the oracle's.
   This is Chord's correctness condition: with right successors, every lookup reaches the right owner.
2. **Successor lists correct.** Every live node's list equals the next min(r, N − 1) live nodes.
3. **Fingers correct.** Every live node's 64 fingers equal the oracle's.

A milestone counts only once it **stays** true at every later poll for at least two periods, so a
state that flickers to correct and back is not reported as converged.

**How each is observed**, through production paths wherever one exists. The successor: the node is
sent `FIND_SUCCESSOR <its own id + 1>`, which it answers from its successor pointer in the code path
lookups use. The predecessor and list: `NEIGHBOURS`, the message stabilise itself relies on.
Fingers: the read-only `FINGERS`. The node reports what it holds; the harness decides whether that
is right. After the last milestone, lookups from every live node must return the oracle's owner.

**Time** runs from the moment SIGKILL has been delivered to the end of the first poll at which the
milestone holds (and keeps holding). It is an upper bound with a resolution of one poll, and the
poll duration is reported with every number.

**Why.**
- **The oracle lives outside the node**, for D-020's reason: the checker must not be the thing being
  checked. A node asserting "I am converged" would be measuring itself, and no node can know anyway:
  convergence is a property of the whole ring.
- **Three milestones because they repair at different speeds and mean different things**:
  correctness in about one or two periods, the list in about r, fingers in about log₂ N. One number
  would hide two of them.

**Rejected: successor pointers only.** Chord's correctness condition, but it hides how long a second
failure would be dangerous (lists) and how long lookups are slow (fingers).

**Rejected: a lookup sweep only.** Behavioural and convincing, but it cannot say which node is wrong,
and a sweep large enough to touch every node's arc is slow enough to blur the timing.

**Cost accepted.**
- Polling is load: three connections per live node per poll. None of these messages contacts a
  third node, so polling cannot itself speed up failure detection, but it costs CPU.
- The resolution is one poll, which grows with N.

---

### D-039 — The stabilisation period and the call deadline come off measured curves, by rules fixed before measuring

**Fork:** F31. D-013 required the period T to be measured, and D-024/D-025 left the node's call
deadline to be chosen in Phase 3. **Date:** 7 Oct 2026. **Gates:** every reconvergence number.

**The decision.** Both are command-line parameters, `--period <ms>` and `--timeout <ms>`. Their
defaults are picked from the Phase 3 benchmark by rules written down here, before any number was
taken:

- **The deadline D** bounds connect, send and receive on every node-to-node call (set before
  `connect`, as in D-025). It is the smallest of 100, 200, 500 and 1000 ms that is at least **ten
  times the slowest node-to-node exchange observed** while the heaviest benchmarked ring runs (256
  nodes at the shortest period swept).
- **The period T** is the **longest swept period whose median time to "ring correct" after one node
  is killed (64 nodes, idle) is at most 1 s**, provided no live node is wrongly evicted in any run
  at that period. Longest, because a longer period is cheaper; 1 s, because that window bounds how
  long a dead node's keys resolve to it. The per-node CPU cost of each period is measured alongside,
  so the price of the target is visible.

**Scheduling.** A round starts T after the previous one finished (a fixed delay), and each node's
first round starts after a random delay in [0, T), so nodes started together do not run their
rounds in step.

**Why.**
- **T does double duty** (D-013): it bounds how long a death goes unnoticed on an idle ring and sets
  how fast the repair spreads, one node per round. Reconvergence grows linearly with T while
  background cost falls as 1/T, so neither curve has a knee on its own. A rule stating the target is
  what turns two curves into one value, and writing it first stops the data choosing the rule.
- **D only matters for nodes that stop answering without refusing** — frozen or partitioned, not
  killed. Too short, and a stall on eight shared cores becomes a false strike; too long, and a frozen
  node takes two deadlines longer to evict. Ten times the worst observed exchange is a margin over
  the slowest thing this machine was seen to do, not over its average.

**Rejected: T and D by precedent** (a 1 s period, a 5 s deadline). Numbers with no derivation for
this system.

**Rejected: D as a multiple of T.** Couples two quantities measuring different things: how often to
check, and how long a healthy reply can take.

**Rejected: a fixed rate instead of a fixed delay.** A round delayed by a deadline would be followed
by rounds run back to back to catch up, exactly when a peer is already slow.

**Cost accepted.**
- Both values are measured on loopback with every node on one machine, so they transfer to a real
  network only as a method, not as numbers.
- The 1 s target is a stated objective, not derived from anything deeper. The curve is published so
  a different objective can be read off it.

**Evidence:** `BENCHMARKS.md` §4. The rules as first written failed twice and were restated (D-041, D-042);
applied in code at `b171554` they chose **T = 1000 ms** (9 of 10 runs repaired within 1 s; 2000 ms: 4 of 10) and
**D = 200 ms** (ten times the 99.9th-percentile exchange was 55 ms; the slowest exchange 116.7 ms). These are the
node's defaults. One of five 256-node runs then logged 77 evictions of live nodes at that deadline, which the
rule's no-false-eviction check, run only on 64 nodes, does not cover. Re-run with logs kept (`BENCHMARKS.md` §4a),
it recurred in 2 of 50 runs, each time during a 5–9 s machine-wide stall in the kernel that starved every process on
the host; the failure detector evicted peers that had not answered for seconds, as specified. The deadline is kept:
a stall that long is indistinguishable from a freeze by any deadline short enough to detect a freeze in seconds.

---

### D-040 — A node that has lost every node it knew refuses to route, and rejoins through addresses it remembers

**Fork:** F32, surfaced by the in-process churn tests while D-035 was being built. **Date:**
7 Oct 2026. **Gates:** what a node does after D-035's last fallback fails.

**What the tests found.** D-035 part 7 said a node whose whole successor list has died falls back
to the nearest node it knows, and "only a node that knows nobody falls back to itself". The first
implementation did exactly that, and the random-churn test showed what it costs. A newcomer knows
one node, its successor, until its first round. If that node dies first, the newcomer falls back to
itself, and `(n, n]` makes it claim the whole ring. Worse, a later join routed through it receives
`OWNER <itself>`, and the two form a **separate two-node ring** — seen in the test as nodes 21 and
23 pointing only at each other — which stabilisation can never merge back, because no pointer
leads from one ring to the other.

**The decision.** Three parts.

1. **Orphaned, not alone.** When eviction leaves a node with no other node it knows, it marks itself
   *orphaned*. An orphan answers `ERR` to `PING`, `FIND_SUCCESSOR`, `NEIGHBOURS` and `NOTIFY`, so it
   routes nothing and recruits nobody. Refusing `PING` matters: an orphan that still answered it was
   kept by its neighbours' liveness checks and re-adopted by their verify-before-adopt (D-035), and
   its own rejoin then waited for ever for the ring to forget it — a livelock, also found by the test.
2. **Rejoin through remembered addresses.** Every node keeps up to 32 addresses it has seen — the node
   it joined through, its successors, its predecessors, and the owners its finger lookups returned —
   newest first. An orphan does no maintenance; each round it runs the join procedure (D-029) through
   the next remembered address.
3. **A newcomer copies its successor's list at join**, with one `NEIGHBOURS` call, instead of waiting
   for its first stabilise. It no longer depends on one node during its first period. There is no
   `NOTIFY` at that point: the newcomer is not listening yet, and a successor that accepted it as
   predecessor would ping it, be refused, and evict it.

**Why.**
- **A node cannot tell "everyone else died" from "I am cut off".** Claiming the ring is right in the
  first case and builds a second, disjoint ring in the second. Refusing to route is wrong only in the
  first case, and then only until an operator restarts a node with `--create`. This chooses
  consistency over availability for a node that has lost contact, which is the side a storage system
  should be on.
- **Rejoining reuses the join path**, which already handles a contact that is dead, a ring that still
  names this address, and a collision.

**Rejected: fall back to itself** (the first implementation). Available, and it splits the ring
silently.

**Rejected: stay orphaned with no rejoin.** Safe, and a node that loses its neighbourhood for one
moment is lost for good.

**Rejected: let orphans elect one of themselves to re-create the ring.** That needs the orphans to
agree on which one, the agreement problem consensus protocols such as Raft exist for, and under a partition
it would build exactly the second ring this decision prevents.

**Rejected for now: a configured seed list** (Cassandra's approach: a few well-known addresses every
node can always rejoin through). It is the operational answer to a stranded orphan, and belongs with
deployment tooling, where there is an operator to configure it.

**Cost accepted.**
- **A stranded orphan — one whose every remembered address is dead or orphaned — stays out of the
  ring.** Under the test's deliberately harsh churn (16 to 48 nodes, a death or a freeze in about one
  step in five, some nodes frozen for long stretches), 191 of 200 histories converge once quiet; the
  other 9 are all stranded: 5 single orphans whose whole remembered world died while they were frozen,
  and 4 total collapses in which every survivor of a ring cut down to 4–6 nodes was orphaned.
- **The last survivors of a total collapse refuse service** instead of forming a ring. An operator
  restarts one with `--create`.
- An orphan costs one refused connection per round while its contacts are dead.

**Evidence:** `test-stabilize`: the orphan test (a newcomer whose eight known nodes die is orphaned,
refuses to route, and rejoins through the node it first joined through), and the churn property test
(200 histories; every one that does not converge is shown to contain a stranded orphan; no running
node ever claims the ring while others are in it).

---

### D-041 — The deadline rule is applied under the loads the benchmark actually runs

**Fork:** F33, surfaced by the first calibration run. **Date:** 7 Oct 2026. **Gates:** the value of the
node-to-node call deadline D (D-039).

**What happened.** D-039 fixed the rule before measuring: D is the smallest of 100, 200, 500 and 1000 ms
that is at least ten times the slowest node-to-node exchange observed "while the heaviest benchmarked ring
runs (256 nodes at the shortest period swept)". The first calibration, 256 nodes at 50 ms, measured a
slowest exchange of 345.5 ms (median 4.3 ms, 99th percentile 92.8 ms), which needs D ≥ 3.46 s — above
every candidate. The rule had no solution. The reason is itself a result: 256 nodes each running a round
every 50 ms drove the load average to 66 on eight cores. That configuration saturates the machine, and the
rule's wording had named it without anyone checking that it was runnable.

**The decision.** The rule is applied to the configurations the benchmark actually runs: the heaviest
point of the period sweep (64 nodes at 50 ms) and the heaviest point of the ring-size sweep (256 nodes at
the chosen period). The larger of their slowest exchanges decides D. The 256-node, 50 ms calibration is
kept and reported, as the measured limit of this machine. The period sweep runs at D = 200 ms, the value
the rule gives for its own heaviest point, so its "no live node wrongly evicted" condition is tested at a
deadline no more lenient than the final one.

**Rejected: D = 1000 ms, the largest candidate.** It gives a margin of 2.9 times the slowest exchange, under
a load no benchmark runs, and departs from the rule's stated margin without a reason.

**Rejected: add larger candidates** (2 s, 5 s). It keeps the wording and gives D ≈ 5 s, which makes a frozen
node take two 5 s deadlines to leave, to protect against a load the benchmark never applies.

**Rejected: drop 50 ms from the period sweep**, so that "the shortest period" is a runnable one. It hides
the result that 50 ms is where this machine runs out of cores.

**Cost accepted.** This changes a rule after seeing data, which is the thing writing the rule first was meant
to prevent. It is stated here rather than made silently, the original wording and the measurement that broke
it are both kept, and the change narrows the rule's scope without touching its margin.

**Evidence:** `BENCHMARKS.md` §4, the calibration table.

---

### D-042 — The selection rules are restated in statistics that a repeat run reproduces

**Fork:** F34, surfaced by running the whole benchmark a second time. **Date:** 7 Oct 2026. **Gates:** the
values of the stabilisation period T and the call deadline D. Supersedes the statistics in D-039 and D-041;
their targets (a 1 s window, a tenfold margin) are kept.

**What happened.** The benchmark was run twice on identical node code, with the rules of D-039 and D-041:

| | First run | Second run |
|---|---|---|
| Median time to ring correct, 64 nodes, T = 1000 ms (5 runs) | 0.37 s | 0.54 s |
| Median time to ring correct, 64 nodes, T = 2000 ms (5 runs) | 1.37 s | **0.96 s** |
| T chosen by "longest period with median ≤ 1 s" | 1000 ms | **2000 ms** |
| Slowest of ~70,000 exchanges, converged 64-node ring at 50 ms | 19.6 ms | **241.0 ms** |
| 99.9th percentile of the same | 4.3 ms | 3.8 ms |

**Both rules gave different answers on a repeat, so neither was measuring a property of the system.**
- **The period rule was ill-conditioned.** After a kill, the ring is correct once the dead node's
  predecessor runs its next round, which falls anywhere in the period. So the median time is a fixed
  fraction of T (0.4–0.7 T measured above 200 ms), and a 1 s target sits right on T = 2000 ms's median. With
  five runs, whether 2000 ms passes is a coin toss.
- **The deadline rule keyed on a single sample.** The maximum of tens of thousands of exchanges, timed from a
  Python process, is set by the rarest pause anywhere in the path, including the measuring process's own,
  and changed twelvefold between runs while the 99.9th percentile moved by 12 %.

**The decision.**
- **T is the longest swept period at which at least 9 of 10 runs reach "ring correct" within 1 s**, with no
  live node wrongly evicted in any run. The 9th of 10 sorted runs is a one-sided 99 % upper confidence bound
  on the median (it falls below the median only if 9 or 10 of 10 fair coin tosses land the same way, 1.1 %),
  so the rule now asks for the target to be met with confidence rather than on a coin toss. The period sweep
  runs 10 times per period instead of 5.
- **D is the smallest of 100, 200, 500 and 1000 ms that is at least ten times the 99.9th-percentile exchange
  and longer than the slowest exchange observed**, under each load the benchmark runs (D-041). The first
  clause keeps D-039's tenfold margin on a statistic that repeats; the second keeps the property that
  mattered — no exchange ever observed would have timed out.

**Rejected: keep the rules and run more repetitions.** More runs make the period rule's median more precise,
but a target sitting on the median stays a coin toss however precisely it is estimated; and the maximum only
grows with more samples.

**Rejected: report both runs' choices and pick one.** That is choosing after seeing the data, with extra
steps.

**Rejected: measure exchanges inside the nodes**, removing the measuring process from the timing. More
accurate, and it needs instrumentation in the node's hot path to fix a rule, when a robust statistic fixes
it without touching the node.

**Cost accepted.** This is the second change to a rule after seeing data (D-041 was the first). The justification
is what triggered it: the same rule, on the same code, produced two different answers, which is a reproducibility
failure rather than a disliked value. Every version of the rules, and both runs' data, are kept. The new rules
were fixed before the final run, which is the one reported.

**Evidence:** the table above; `BENCHMARKS.md` §4 for the final run.

---

### D-043 — Stabilise runs to its fixed point within a round, not one step per round

**Fork:** F35, surfaced by the reconvergence benchmark. **Date:** 7 Oct 2026. **Gates:** recovery after
more adjacent deaths than the successor list holds, and the start of a wired ring.

**What the benchmark found.** In 3 of 5 runs a 256-node wired ring did not converge from start-up within
60 s, and once it was still half wrong after 120 s. A diagnostic run caught the state: one successor cycle
of 245 nodes, winding exactly once round the identifier space, with 11 nodes off it — not a "loopy" ring,
and not a split. The log of the node responsible showed the mechanism: it had started before its 8
successors and several fingers were listening, evicted each on a refusal, fallen back to a node most of the
way round the ring (D-035 part 7), and was then **walking back one node per period** — the paper's
stabilise takes one inward step per round, so a distant fallback costs O(distance) periods. The same walk
explains the adjacent-kill results at `5b6830d`: 8 adjacent deaths (the whole list) took a median of 1.49 s
and 9 took 2.99 s, with a worst run of 7.6 s, against 0.29 s for 4.

**The decision.** In one stabilise round, after adopting a closer successor, the node asks that new
successor too, and keeps going until no closer live node is named — at most 64 inward steps per round. Every
step is an ordinary stabilise step: the candidate must lie strictly between the node and its current
successor, and must answer a ping before it is adopted (D-035). Because each step moves strictly inward, the
loop ends.

**Rejected: the paper's one step per round.** Simplest, and the measured cost above.

**Rejected: treat an exhausted list as orphaned and rejoin (D-040's path).** A join looks up the node's own
identifier, and while the node is still in other nodes' tables that lookup comes back to the node itself,
so it would first have to be evicted — two strikes — and would refuse to route meanwhile.

**Rejected: a longer successor list.** Makes exhaustion rarer, does nothing for a node that starts before
its neighbours, and leaves the walk as slow as it was.

**Cost accepted.**
- A round can now make up to 64 extra round trips (two per step: the question and the ping). In steady state
  it makes none: a step only happens when a closer node exists.
- A hostile successor cannot keep a node walking: each candidate must answer a ping from an address
  strictly closer, and the walk is capped.

**Evidence:** two in-process tests written against the old code first. Six adjacent deaths with r = 4, ten
rings of 64: ring correct after at most **5 rounds before, 1 after**. A ring of 128 in which half the nodes
start three rounds late: fully correct after at most **64 rounds before, 12 after**. The real-process
before and after are in `BENCHMARKS.md` §4.

---

### D-044 — Chunks travel as a header line followed by exactly the number of bytes it names

**Fork:** F36. **Date:** 8 Oct 2026. **Gates:** every chunk message in Phase 4, and Phase 5's transfer path.

**The decision.** The chunk plane uses the node's existing line protocol for every header and reply, and
a message that carries a chunk names the body's length as the **last word of its header line**, followed by
exactly that many raw bytes:

```
PUT <key> <w> <length>\n<length bytes>     ->  OK <copies>        | ERR <reason>
STORE <key> <length>\n<length bytes>       ->  OK                 | ERR <reason>
GET <key>                                  ->  CHUNK <length>\n<length bytes> | MISSING | ERR <reason>
HAVE <key> [<key> ...]   (1 to 20 keys)    ->  HAVE <one 0/1 digit per key>
```

`<key>` is the chunk's full SHA-1 as 40 lowercase hexadecimal characters (D-014, I7). `PUT` asks the
receiving node to coordinate a replicated write (D-046); `STORE` asks it to keep one copy and forward
nothing, so a replica can never start a second round of forwarding. Which verbs carry a body, and where
the length sits, is defined in one function shared by the sender and the receiver.

**Three rules the framing enforces.**
1. **The length is checked before anything is allocated.** A body longer than `MAX_CHUNK` (512 KiB, D-016)
   is refused on the header alone, so a peer cannot choose how much memory this node allocates — defect
   R7's lesson from the legacy transfer path, applied from the start.
2. **The body is read until exactly `length` bytes have arrived.** `recv` may return fewer bytes than
   asked; a short read is a loop, and a peer that closes mid-body is a failed transfer, never a short chunk.
3. **The header reader never reads past its newline.** `recv_line` takes one byte per call (D-021), so
   the body's first byte is still in the socket when the header has been parsed. A buffering line reader
   would have swallowed the start of the body.

**Rejected: the chunk inside the line, hex- or base64-encoded.** One framing scheme instead of two. It
doubles (hex) or adds a third (base64) to every transfer, and a 512 KiB chunk becomes a 700 KiB to 1 MiB
line, so `MAX_LINE` — which exists to stop an unbounded line from exhausting memory — would have to go.

**Rejected: a separate data connection, announced on the control connection.** Keeps control lines
small and lets a body stream to disk. It needs a second listening socket or port negotiation per
transfer, and two connections whose failures have to be matched up, for no gain at 512 KiB.

**Cost accepted.**
- Two framing schemes now coexist on one socket, chosen by the verb. Every new body-carrying verb has to
  be added to the one function that knows which verbs carry bodies.
- A chunk is at most 512 KiB. D-017 stores a file's manifest as a chunk, so one manifest holds at most
  12,800 chunk hashes — a 6.25 GiB file. A larger file needs a manifest split across chunks; that belongs
  to Phase 5, where manifests are built. *(Settled by D-054: a text manifest holds at most 8,192 entries,
  and a larger file gets a tree of manifests.)*
- A peer that sends the body slowly holds a thread until each `recv` times out; there is a deadline per
  system call, not per body.

**Evidence (8 Oct, `1328ce0`, `52a3f32`):** `test-chunk` checks the length rule on every boundary (0, exactly
524,288, 524,289, leading zeros, signs, eight digits, a trailing space), reassembles a 300 KB body sent in uneven
pieces over a real socket, and reads a peer that hangs up after 400 of 1,000 bytes as a failed transfer. On real
processes (`e2e-chunk.sh`) a node refuses a 512 KiB + 1 header before reading a byte, and survives a client that dies
mid-body with no file and no temporary file left. A mutant with the limit off by one was caught by both layers.

---

### D-045 — The chunk store is one file per chunk, written to a temporary name, flushed, then renamed

**Fork:** F37. **Date:** 8 Oct 2026. **Gates:** every byte a node keeps.

**The decision.** A node started with `--store <dir>` keeps each chunk as one file,
`<dir>/<first two hex characters>/<40-character key>`. A write goes to `<dir>/tmp/<key>.<unique suffix>`,
is flushed to disk with `fsync`, renamed over the final name, and the directory holding it is flushed too;
only then is the write reported as stored. A node without `--store` answers every chunk request with an
error and touches no disk, so every earlier mode and measurement is unchanged.

**The key is validated before it reaches the filesystem.** Exactly 40 characters, each `0-9` or `a-f` —
lowercase only, so one chunk cannot have two names. Anything else, including `../`, a slash, uppercase or
a 39-character key, is refused before a path is built. The key is the only peer-supplied part of any path.

**Why each part.**
- **Rename is the commit point.** `rename(2)` within one filesystem replaces the destination atomically:
  a reader sees the old file or the new one, never half of either. A node killed mid-write leaves a
  temporary file — never a final-named file holding part of a chunk. Temporary files left by a crash are
  deleted at start-up.
- **`fsync` before reporting success,** because "stored" is what an acknowledged write (D-015) counts.
  Without it the bytes sit in the page cache: they survive the process being killed, which is the failure
  this project tests, but not a power cut — and replicas in one rack lose power together. The directory
  is flushed as well, because the rename itself is a directory change. Measured on this machine (one
  sample of 40 writes of 512 KiB, taken while another benchmark ran — **an estimate of the order of
  magnitude, not a benchmark**): about 1.2 ms per write without `fsync`, 6.0 ms with the file flushed, 9.7 ms
  with the directory too. The write-latency benchmark measures it properly.
- **Two-character fan-out** keeps any one directory to 1/256 of the chunks. ext4 copes with large
  directories, but a listing or a scan of one directory of a million files is slow everywhere else.
- **An index in memory:** the set of keys held, loaded by listing the directories at start-up and updated
  on every store. Sorted by key, which for lowercase fixed-width hexadecimal is also numeric order, so the
  keys in one arc of the ring are a range query (repair, D-050).

**Two concurrent writes of the same chunk** each write their own temporary file and each rename it. The
last rename wins, and both wrote the same bytes — the key is the hash of the contents (D-014) — so the
result is the same either way. A write of a chunk already held does nothing more than check that the file
exists. A corrupt file on disk is not overwritten by this path; it is found by the scrub in D-050.

**Rejected: an append-only log with an index (Bitcask's design).** One file and sequential writes. It needs
compaction to reclaim space, a recovery scan of the log at start-up, and care that a torn final record is
discarded — a storage engine, which the project's scope rules out.

**Rejected: chunks in memory only.** Fastest, and a restart loses everything. A node holding 1,000 chunks of
512 KiB needs 500 MiB; at three replicas the benchmarks would not fit in this machine's memory.

**Rejected: no `fsync`.** Five to eight times faster per write, and "stored" would then mean "in a page
cache". Kept as the stated alternative if write latency ever matters more than surviving a power cut.

**Cost accepted.** Two flushes per stored chunk; the store keeps one directory entry per chunk; and
nothing ever deletes a chunk (D-023), so storage only grows.

**Evidence (8 Oct):** 40-character traversal keys, uppercase, 39 characters and an embedded NUL are refused before a
path is built (in process and on real processes, with a sentinel file checked); a restart removes a left-over temporary
file and reloads the index; eight threads writing one chunk at once leave one file. A mutant accepting any 40
characters was caught by both layers — after the in-process test was fixed: its first version used a traversal key
too short to reach the character check. Cost of the flushes: part of every write latency in `BENCHMARKS.md` §6.

---

### D-046 — The owner coordinates a write: it stores its copy, sends the others in parallel, and answers after W

**Fork:** F38. **Date:** 8 Oct 2026. **Depends on:** D-014, D-015. **Gates:** the write path and the `W` sweep.

**The decision.** A client looks up the owner of `SHA-1(chunk)` and sends it `PUT <key> <w> <length>` with
the body. The node that receives it:
1. checks the key, the length and then the hash of the body against the key — a mismatch is refused, so a
   chunk is never stored under a name it does not hash to;
2. stores its own copy (D-045) — copy 1;
3. sends `STORE <key> <length>` with the same body to each other member of the replica set (D-048),
   **in parallel**, one thread each;
4. answers `OK <copies>` as soon as `w` copies are confirmed, its own included, or `ERR` once enough
   replicas have failed that `w` can no longer be reached;
5. keeps waiting for the remaining replicas after answering, and logs how long the last one took — the
   window in which fewer than `RF` copies exist.

**`w` travels in each request** (1, 2 or 3; the client's default is 2). D-015 made `W` a runtime parameter;
putting it in the request rather than on the node's command line lets one ring, under one failure, serve
writes at every `W` — so the sweep compares settings on the same ring and the same kills, and an
application could ask for more durability for some writes than others (Cassandra's per-request consistency
level is the precedent).

**If the owner cannot be reached,** the client tells the member that answered its lookup (`SUSPECT`, D-036) and
sends the write to the next distinct process in that member's successor list, which coordinates it with its own
replica set — the same fall-through a read uses (D-049), so a write is not refused during the window before a dead
owner is evicted. An owner that **answers** `ERR` because it could not reach `w` copies has given the answer: the
client does not try another coordinator, which would face the same replicas and scatter extra copies. The one
exception is `ERR busy` (D-052), which says nothing about the replicas.

**A failed write is not rolled back.** If only one of two needed copies is confirmed the client gets `ERR`,
and the copy that was written stays. Nothing can be inconsistent about it — it is the right bytes under the
right name — and the client's retry or the next repair (D-050) completes it.

**Rejected: the client writes every replica itself.** Saves the owner's forwarding hop and keeps nodes
simpler. The client then has to learn the replica set (another request), push the body three times over its
own link instead of once, and carry the `W` logic; and a client that dies after one copy leaves the other
replicas to repair. D-015 already put the coordination on the owner.

**Rejected: chain replication** (owner → second → third, the acknowledgement travelling back). Each link
carries the body once, and the tail can serve reads that are guaranteed complete. But latency becomes the
**sum** of the hops rather than the slower of two parallel sends, a slow middle node delays every write
through it, and `W` stops meaning "the first `W` copies to finish" — with immutable chunks there is no order
for the chain to protect.

**Rejected: `W` on the node's command line.** Simpler request; but comparing `W` values would need a fresh
ring per value, and the comparison would then be between rings rather than between settings.

**Cost accepted.**
- The owner sends the body twice, so its outgoing traffic per write is twice the chunk.
- A client's lookup can be stale for up to a period after a join or a failure, so a `PUT` can land on a node
  that is not the owner. That node coordinates anyway, with its own replica set; the copies are real, and
  repair (D-050) moves them to where they belong.
- `W = 3` with a dead or frozen replica fails after the call deadline: the price of insisting on three.

**Evidence (`BENCHMARKS.md` §6, `7d99fc0`):** the coordinator answers a W = 2 write of 512 KiB in 31 ms (median) and
a W = 1 write in 14 ms; with one replica killed, W = 2 acknowledged 300 of 300 writes and W = 3 244 of 300. One
prediction failed: W = 2 is **not** faster than W = 3 while both replicas are healthy (31 vs 32 ms), because on loopback
the two replicas are equally fast, so the faster and the slower of two parallel copies take about the same time.

---

### D-047 — Virtual nodes: several ring members per process, one listening socket, requests addressed by identifier

**Fork:** F39. **Date:** 8 Oct 2026. **Depends on:** D-018 (routing primitives are free functions), D-019.
**Gates:** load balance, replica placement (D-048), and every message.

**The decision.**
1. **Identity.** A process at `ip:port` started with `--vnodes V` is `V` ring members. Member 0 keeps the
   identifier it always had, `SHA-1("ip:port")`; member `i ≥ 1` is `SHA-1("ip:port#i")`. The `#` cannot occur
   in a dotted quad or a port number, so no member of one process can hash the same string as a member of
   another. A process whose own members collide refuses to start.
2. **One listening socket per process; every request names its member.** A request to a ring member is
   prefixed `TO <16-hex identifier> `, added by the transport from the `Peer` it is sending to, so routing
   code and lookup code do not change. A request without the prefix goes to member 0, so a tool that predates
   virtual nodes still talks to the process's own address identity. A request for an identifier the process
   does not have is answered `ERR`, which the sender counts as a strike against that member. Chunk requests
   (D-044) are addressed to the process, not a member: the store belongs to the process.
3. **Each member keeps its own Phase 3 state** — predecessor, successor list, fingers, strikes — and runs the
   same maintenance round as before. One maintenance thread per process runs each member's round in turn.
4. **A member is an address and an identifier; a process is an address.** Routing state compares members
   (two members of one process are different ring members). Failure evidence is split by what it is
   evidence of: **a refused connection proves nothing is listening at that address, so every member of that
   process is evicted at once**; a timeout or a bad reply is about one exchange with one member, so the strike
   counts against that member. A node never strikes or evicts a member of its own process — it is that
   process. "Am I alone?" still compares addresses and identifiers, never identifiers alone (I8).
5. **Starting.** `--join` joins each member separately through the contact before the process listens
   (D-029). `--wired` reads a membership file of processes and expands every process into its `V` members,
   so every node must be started with the same `V`. The fixed-ring mode of Phase 2 stays one member per
   process. The default is set by D-051.

**Why one socket and an identifier on the wire.** Replicas must be placed on distinct *processes* (D-048), so
anything that sees the ring must be able to tell which members share a process. With one socket per process,
the address is the process, for free, everywhere a `Peer` already travels.

**Rejected: one listening port per member.** No change to any message — each member is a Phase 3 node that
happens to share a process. But then an address no longer identifies a process, and every peer and the
harness would need a second identity on the wire to keep replicas apart, which is the change this option
was meant to avoid; and a process would hold `V` listening sockets.

**Rejected: one routing table per process** (every member's fingers pooled, and a lookup answered from
whichever member is closest to the key). Fewer hops and `V` times less finger maintenance — the Chord paper
suggests it. It changes the routing rule underneath the hop-count measurement and the progress checks the
originator applies, so it is left as an optimisation to measure separately.

**Rejected: tokens with full membership** (Dynamo's and Cassandra's design: every node knows every token and
routes in one hop). It is the O(N) membership per node that D-020 and the scale notes reject for this design.

**Cost accepted.**
- Maintenance traffic per process grows `V`-fold: each member stabilises, checks its predecessor and repairs
  a finger every round. On one host that is `N × V` members' worth of background load.
- Joining takes `V` lookups; every request carries 22 more bytes.
- A peer can address any member of a process, including one the process does not have; that costs one `ERR`.

**Evidence (8 Oct):** in process, a ring of 8 processes × 4 members is correct again within two rounds after a process
dies and every lookup reaches the oracle's owner; a timed-out sibling is never struck; a refusal evicts a whole
process and a timeout one member. The Phase 3 suites pass unchanged with one member per process. On real processes,
`e2e-chunk.sh` runs 6 × 4 members, also under ThreadSanitizer. Mutants that ignore the `TO` prefix, cut a successor
list at a sibling, or strike a sibling are each caught.

---

### D-048 — A chunk's replicas are the first three distinct processes clockwise from its key

**Fork:** F40. **Date:** 8 Oct 2026. **Depends on:** D-015, D-047. **Gates:** what `RF = 3` means.

**The decision.** The replica set of a key `k` is built by walking the ring clockwise from the owner of `k`
(the first member at or after `k`) and taking each member whose **process** has not been taken yet, until
three processes are found. The owner computes it from its own successor list (D-032, eight entries): itself,
then each entry whose address differs from every process already chosen. If the list holds fewer than three
distinct processes, the set is smaller — degraded, and logged — but never contains one process twice.

**Why distinct processes.** With virtual nodes, two adjacent members often belong to one process: with `V`
members per process and `P` processes, a given member's successor belongs to the same process with
probability about `1/P`. Taking "the next two members" would then put two copies in one process, and one
crash would destroy two of the three. `RF = 3` is a claim about failures survived, so it has to count failure
domains — here processes, the only failure the system can tell apart. On a real deployment the same rule
would count machines or racks, which is the reason the comparison is by address rather than by identifier.

**Rejected: the next `RF − 1` members, whatever process.** The Chord paper's successor-list replication,
correct without virtual nodes and silently wrong with them.

**Rejected: a separate replica list maintained by stabilise,** with same-process members filtered out of the
successor list itself. Then the successor list would no longer list the next members of the ring, which is
what routing correctness and repair after a death depend on (D-035).

**Rejected: a longer successor list when virtual nodes are on.** Makes a shortfall rarer at small `P`, at the
cost of a longer `NEIGHBOURS` reply every round. The shortfall is measured first (D-051's simulation counts
how often eight members hold fewer than three processes) and the list stays at eight unless it shows up.

**Cost accepted.** On a ring of fewer than three processes, `RF = 3` cannot be met and is not pretended: the
set is as large as the ring allows and the write's `W` decides whether that is acceptable. The rule needs
every node to agree on the ring locally, which they do only after stabilisation — during repair, two nodes can
compute different sets for one key, and repair (D-050) settles it.

**Evidence (8 Oct):** the replica set of every member on 30 random rings (3–16 processes, 1–8 members each, 1,193
sets) equals a brute-force oracle and never repeats a process; a mutant comparing members instead of processes is
caught. On a real ring every chunk's copies are on exactly the oracle's three processes (`e2e-chunk.sh`), and the
load-balance simulation, which uses this rule, matched 2,000 real chunks file for file (`BENCHMARKS.md` §6).

---

### D-049 — A read asks one replica, checks the hash itself, and falls through to the next on any failure

**Fork:** F41. **Date:** 8 Oct 2026. **Depends on:** D-014, D-015, D-048. **Gates:** the read path; Phase 5's
parallel fetch.

**The decision.** To read key `k`, the client looks up `k`. The lookup's last answer comes from the member
just before `k`, which answers `OWNER <o>`. The client sends `GET <k>` to `o`'s process, hashes the bytes it
receives and compares the result with `k`. If the connection fails, the reply is `MISSING` or `ERR`, or the
hash differs, the client asks that last member for `NEIGHBOURS`, whose successor list begins with `o`, takes
the distinct processes from it in order (D-048's rule), and tries each in turn. The first chunk whose hash
matches is the answer. Every replica failing, or every replica answering wrongly, is a read failure that names
each replica and what it did.

**Why the replica list comes from the member before the key.** If the owner is dead it cannot be asked for its
successors. The member that answered `OWNER` is the owner's predecessor; its successor list is the owner
followed by the next members — the replica set, from a node that has just shown it is alive. It costs one
extra round trip, paid only when the first replica fails.

**Rejected: `R = 2`, compare two replicas.** Detects nothing that hashing does not: with content addressing a
wrong answer cannot hash to the key, so a single replica checked locally is as strong as any number compared
(D-014). It doubles read traffic.

**Rejected: ask all replicas at once, take the first good answer** (hedged reads). Lowest tail latency, and a
common answer to a slow replica. Triples the read traffic of every read to speed up the rare one. Phase 5's
parallel transfer may revisit it with a delay before the hedge.

**Rejected: the owner returns its replica list with `MISSING`.** One message fewer, but useless in exactly the
case that needs it — the owner being dead.

**Cost accepted.** A read of a key whose owner has just died pays a failed connection, one `NEIGHBOURS` and a
second `GET`. Until the owner's predecessor has evicted it, the lookup still names the dead owner (D-036's
window); the fall-through serves the read from the next replica during that window instead of failing it.
A read does not repair what it finds wrong; repair is the scrub's and the owner's job (D-050).

**Evidence (8 Oct):** with the owner's copy corrupted on disk, a read is served by the next replica and records why;
with the owner and the second replica killed, a read is served at once by the third (real processes); with every
copy corrupt the read fails naming all three. Mutants that skip the hash or never fall through are caught.

---

### D-050 — Repair is driven by each chunk's owner: checked whenever its arc or replica set changes, and in a periodic full scan

**Fork:** F42. **Date:** 8 Oct 2026. **Depends on:** D-046, D-048. **Gates:** whether `RF = 3` is restored after
a death, and the re-replication benchmark.

**The decision.** Each process runs a **repair thread**, separate from the maintenance thread, once per
period:
1. **Owner check, level-triggered.** For each of its members `v` with a known predecessor `p`, the process
   owns the arc `(p, v]`, and `v`'s replica set is computed by D-048's rule. If `p` or the replica set differs
   from what was last checked for `v`, every chunk it holds in that arc is checked: it asks each other replica
   `HAVE` (up to 20 keys per request) and sends `STORE` for each one missing.
2. **Full scan, every `--scan` rounds** (default 60): the same owner check for every arc regardless of change;
   a **scrub** that re-hashes every local chunk and replaces any file that no longer matches its key with a copy
   read through the ring — a read that verifies the hash itself (D-049); and a **hand-off** for every chunk the
   process holds but does not own — it looks up the chunk's owner and, if the owner lacks it, sends it as a
   `PUT`, so the owner replicates it to its whole replica set at once. Keys are taken in order, so one lookup
   covers every key up to the owner it names.
3. Nothing is ever deleted except a corrupt file found by the scrub, which is replaced. A chunk held by more
   than three processes stays (D-023).

**Two details found while writing the tests, by tracing them before running them.** A scrub that only deleted a bad file would leave that replica
missing until the owner's next full scan, because nothing about the ring changed; so the scrubbing process
fetches a verified copy itself. And a hand-off sent as a plain `STORE` would give the owner one copy and
nobody else, for the same reason; sent as `PUT`, the owner's coordination places all three.

**Why the owner.** One process checks each chunk, rather than all three. And after a death the new owner
already has a copy: the replica set was the first three distinct processes from the key, so when the owner's
process dies, the next member with a copy — the old second replica — becomes the owner, sees its arc grow
and its replica set change, and copies the chunk to the new third process.

**Why "changed since last checked", evaluated every round, and not an event.** The check compares the current
arc and replica set with the last ones checked, so a change is never missed for having happened while the
thread was busy — the same level-triggered shape as stabilisation. The full scan covers what no ring change
reveals: a file lost or corrupted on a process that stayed up, and chunks left on a process that no longer
owns them after a join.

**Rejected: periodic full scan only.** Simplest, and repair after a death waits up to a whole scan period.

**Rejected: every holder checks every chunk every round.** Three times the `HAVE` traffic, forever.

**Rejected: hash-tree (Merkle) comparison between replicas,** Dynamo's anti-entropy: replicas compare hashes
of key ranges and exchange only differences, so a check costs O(differences) rather than O(chunks). The right
answer at scale; named as the way this grows, and not built for chunk counts a laptop holds.

**Rejected: repair inside the maintenance round.** Moving many chunks would delay stabilisation, so a death
would slow the repair of the ring itself.

**Cost accepted.**
- A full scan re-hashes everything the process holds (about 1 s of CPU per GiB at OpenSSL's SHA-1 speed —
  `ESTIMATE`) and sends a `HAVE` for every owned chunk.
- An arc check after a change sends `HAVE`s for every chunk in the arc, not just the affected ones.
- Repair takes the owner's view of the ring; during reconvergence two processes may each think they own a key
  and both push it. `STORE` is idempotent, so the cost is traffic, not damage.
- Extra copies are never reclaimed.

**Evidence (`BENCHMARKS.md` §6, `7d99fc0`):** after one of 16 processes dies, every chunk it held is back on three
correct live processes in a median of 2.2 s (eight members per process) and 1.9 s (one). Found by the real-process
suite: a copy that failed its hash during an arc check was removed but not replaced, and when the only good copies
were then killed the chunk was gone; the check now replaces it with a verified copy, as the scrub does.

---

### D-051 — The number of virtual nodes per process comes off a measured curve, by a rule fixed before measuring

**Fork:** F43. **Date:** 8 Oct 2026. **Depends on:** D-047. **Gates:** `--vnodes`' default, and the
load-balance result.

**The decision.** `V` is the **smallest power of two for which the coefficient of variation of chunk copies
stored per process is at most 0.25**, in the median of 20 address sets, at both 16 and 64 processes, with
`RF = 3` placed by D-048's rule. The coefficient of variation (standard deviation divided by mean) is used
rather than the busiest process's share because a maximum is the least repeatable statistic there is — the
lesson of D-042. Until it is measured the code's default stays 1, and the benchmark step replaces it with the
rule's result, as D-039's period and deadline were.

**How it is measured.** Placement is a pure function of the addresses and the keys, so it is computed in
process with the node's own identifier functions, for `V ∈ {1, 2, 4, …, 128}`, 20 address sets of the form the
benchmarks use, and 100,000 random keys: per process, keys owned and copies stored, summarised as maximum over
mean and coefficient of variation. A real ring then checks the simulation: chunks are written through the
nodes, and every process's store must hold exactly the number of files the simulation predicts for those
addresses and keys. The cost side is measured on real processes: idle CPU per process against `V`.

**Why 0.25.** It is a target, not a derived number, and is stated as one. With a coefficient of variation of
0.25 the busiest of 64 processes stores about 1.6 times the mean (normal approximation, `ESTIMATE`), against
several times the mean with one member per process. Theory gives the shape to expect: one member's share of
the ring is roughly exponential (coefficient of variation 1), and a sum of `V` independent shares has a
coefficient of variation of `1/√V`, so for owned keys the rule lands near `V = 16`; stored copies average three
arcs and may need fewer.

**Rejected: `V = 1`.** No extra traffic; the busiest process holds several times its share, and the slowest
disk fills first.

**Rejected: a large fixed `V` such as 256** (Cassandra's long-time default). Near-perfect balance, but every
member here runs its own stabilisation, so 256 members per process is 256 times the background traffic —
Cassandra can afford it because its nodes exchange membership by gossip rather than maintaining Chord tables
per token, and it has since lowered the default to 16.

**Rejected: `V` proportional to each machine's capacity.** The real advantage of virtual nodes on mixed
hardware, and unnecessary when every process is identical.

**Cost accepted.** The rule's threshold is a choice, and the value is good for 16 to 64 processes; the curve
is published so another target can read its own `V` from it. Every earlier experiment ran with one member per
process; their harnesses pass `--vnodes 1` explicitly once the default changes.

**Evidence (`BENCHMARKS.md` §6, `7d99fc0`):** the rule chose **V = 8** — median coefficient of variation 0.203 at both
16 and 64 processes, against 0.273 and 0.277 at V = 4. The busiest of 64 processes stores 1.52 times the mean, against
2.80 with one member. Owned-key spread followed 1/√V to within 0.01 at every V. Cost: about half a percent of a core per
member at T = 1 s. The default was set to 8 at `6692a3e`; earlier harnesses pass `--vnodes 1`.

---

### D-052 — A chunk in flight is held whole in memory, shared between its parallel sends, and capped per process

**Fork:** F44. **Date:** 8 Oct 2026. **Gates:** memory per node under write load, and how many processes the
benchmarks can run.

**The decision.**
- A node receiving a chunk reads the whole body (at most 512 KiB) into memory, hashes it, then writes it. A
  coordinator sends that one buffer to both replicas through a shared, read-only pointer, so a write occupies
  one chunk's memory, not three.
- At most **32 chunk bodies** are held at once per process — about 16 MiB. A request that would exceed it is
  answered `ERR busy` **without its body being buffered**: the body's bytes are read in 4 KiB pieces and thrown
  away, so the connection stays in step and the answer is not lost. (Closing instead would leave unread bytes in
  the socket, and Linux answers a close with unread data by resetting the connection, which can destroy the
  `ERR busy` before the sender reads it.) The sender treats `busy` as a failed copy. The count is a single atomic
  counter, incremented and checked in one indivisible step.
- The store is on disk; the only per-chunk memory is its key in the index (about 100 bytes with the set's
  overhead — `ESTIMATE`). Benchmarks put every store under one scratch directory, removed afterwards.

**Rejected: stream the body to a temporary file while hashing incrementally.** Constant memory per transfer.
The coordinator would then have to read the file back to forward it, or forward while receiving and undo the
forwarding when the hash turns out wrong — and a chunk is only 512 KiB.

**Rejected: no cap.** A burst of writes, or a hostile client opening many connections, could make a node allocate
without limit; one-thread-per-connection already has that shape (see the scale notes), and memory would make it
worse.

**Rejected: a bounded queue that makes callers wait.** Smooths bursts, but a caller waiting on a full queue holds
its connection and thread, and the sender's deadline expires anyway — a slow failure instead of a fast one.

**Cost accepted.** 32 is a choice sized to this machine (16 MiB per process, a few hundred MiB for a benchmark
ring), not derived from a workload. Under a burst larger than 32 concurrent chunks per process, writes fail with
`busy` and must be retried.

**Evidence (8 Oct):** 32 bodies announced and withheld make the 33rd `ERR busy`, and its drained connection still answers
a `PING` (real processes); 64 threads × 20,000 attempts never hold more than 32 slots. A mutant that checks and then
increments in two steps survived both the stress test and ThreadSanitizer — a race on an atomic is not a data race, so
the sanitizer cannot see it, and the window is too narrow to hit; the single `fetch_add` is correct by construction,
not by test. The `ERR busy` reply was not lost on loopback even when the connection was closed instead of drained.

---

### D-053 — W is compared on one ring under one failure, and repair is judged from outside by the same kind of oracle

**Fork:** F45. **Date:** 8 Oct 2026. **Depends on:** D-015, D-046, D-050. **Gates:** the Phase 4 results.

**The decision.**
- **The `W` sweep** runs `W = 1, 2, 3` interleaved — one write at each in turn — from the same client threads, on
  the same ring, during the same failure, so every setting sees the same conditions. A run has three phases on a
  ring of 16 processes: healthy; one replica-holding process **frozen** (SIGSTOP: connections are accepted by the
  kernel and never answered, so every call to it waits for the deadline); and one process **killed** (connections
  refused until the ring evicts it). Each write is a fresh random 512 KiB chunk. Reported per `W` and phase: write
  latency at the 50th and 99th percentile, the fraction acknowledged, and, from the coordinators' logs, how long
  after the acknowledgement the last copy landed.
- **Durability after a coordinator crash** is measured directly: for each `W`, a write is sent, and the moment its
  acknowledgement arrives the coordinating process is killed. Afterwards — before any repair can run — the harness
  asks every live process whether it holds the chunk. An acknowledged write with no surviving copy is a lost
  write. Repeated enough times per `W` to report a rate with its count.
- **Re-replication** is the time from killing one process until every chunk is held by all three of its correct
  replica processes among the live ones — computed by the harness from the live addresses with its own
  implementation of D-048's rule, and checked by asking each process `HAVE`. Like D-038, the instrument is outside
  the thing measured, and a milestone counts only once it stays true.

**Rejected: a fresh ring per `W`.** Simpler harness, and the differences between `W` values would include the
differences between rings and kill timings.

**Rejected: inferring durability from the copy count at acknowledgement.** `W` copies at acknowledgement is true by
construction; whether an acknowledged write survives its coordinator's death depends on what happened after the
acknowledgement, which only a kill can show.

**Cost accepted.** Sixteen processes on eight cores, not a cluster: the frozen-replica phase measures the deadline
as much as the protocol, and the coordinator-crash test measures a race between the kill and the background
copies on loopback, where a real network would make that window wider.

**Evidence:** `BENCHMARKS.md` §6, at `7d99fc0`.

---

### D-054 — A manifest is a text chunk listing keys and byte counts, and a large file gets a tree of them

**Fork:** F46. **Date:** 8 Oct 2026. **Depends on:** D-014, D-017, D-044. **Gates:** the upload and download
paths, and the largest file the system holds.

**The decision.** A file is cut into fixed-size chunks (512 KiB, D-016), each stored under its own hash. Its
**manifest** is itself a chunk in the ring (D-017), in this text format, every line ending in `\n`:

```
chordfs-manifest 1
size <bytes of the file this manifest covers>
chunk <chunk size in bytes>
kind data | index
entries <n>
<40-hex key> <bytes>          (n lines, in file order)
```

- In a **data** manifest each entry is a chunk of the file, and its byte count is the chunk size, except the
  last, which is 1 to chunk-size bytes. A zero-byte file has a data manifest with zero entries.
- In an **index** manifest each entry is another manifest, with the number of file bytes it covers; every entry
  but the last covers a whole number of chunks, so chunk offsets come out the same at every level.
- A manifest holds at most **8,192 entries**. The largest possible entry line is 62 bytes (a key, a space, a
  20-digit number, a newline) and the header at most 83, so 8,192 entries fit in 507,987 bytes, under the 512 KiB
  chunk limit (D-044). One data manifest covers 4 GiB; two levels, 32 TiB.
- **Building:** if a file has more than 8,192 chunks, the data entries are cut into data manifests of 8,192, those
  manifests become the entries of the next level, and so on until one manifest — the **root** — remains. The
  root's key is what the tracker stores. The fan-out is a parameter of the builder, so a test can force a tree
  with a ten-chunk file; a reader accepts any fan-out up to 8,192.
- **Reading is strict and happens before anything is written:** the magic line, canonical numbers only, entries
  equal to the count, sums equal to `size` (checked without overflow), the data-entry size rule, every key valid
  (D-045), at most four levels, and each child's own `size` equal to the byte count its parent gave it. The whole
  tree is fetched and checked — each manifest verified against its key like any chunk (D-049) — before the
  destination file is created.
- The manifest holds **no file name, timestamp or permissions**, so identical content produces an identical
  manifest, and the manifest deduplicates like any chunk (D-014). The name lives only in the tracker.

**Why a tree.** The root's key commits to every byte of the file: each entry is the hash of what it names, so
checking the root against the key the tracker gave checks everything below it, one level at a time. This is the
structure of a Git tree and of IPFS's file graph. And it costs one round trip per level, not per manifest: all the
manifests of a level are known as soon as their parent is read.

**Rejected: a binary manifest** (20 raw bytes per hash, about 26,000 entries per chunk). Two and a half times the
capacity per manifest, but capacity stops mattering once there is a tree, and a text manifest can be read and
checked by eye with `chunk get` — the same reason the control protocol is lines.

**Rejected: one flat manifest and a size limit.** The simplest code, and an honest limit — 4 GiB at this format, a
size a disk image or a video exceeds.

**Rejected: a chain of manifests, each naming the next.** As simple as a flat list to build, but a reader learns
each manifest only from the one before it, so a 40 GiB file waits for ten sequential fetches before its chunks can
be fetched in parallel; a tree waits for its depth.

**Cost accepted.**
- One extra fetch per level before the chunks (none for files under 4 GiB beyond the root itself).
- The client holds the whole chunk list in memory — key, offset and length, about 100 bytes per chunk, so
  roughly 200 KiB per GiB of file (`ESTIMATE`).
- The per-entry byte count is redundant in a data manifest (it follows from `size` and `chunk`); it is kept so that
  every level has one format and one checker, and so that a wrong count is detected rather than trusted.

**Evidence (8 Oct, `9a3d03d`, `83d2a3b`):** `test-transfer` checks the format exactly, refuses 26 malformed manifests one rule at a time (a size of 2^64, counts that wrap past 2^64, 8,193 entries — refused before the lines are split — a carriage return, a data chunk short in the middle, an empty index), shows the largest possible manifest is 507,987 bytes, builds 8,193 chunks into two data manifests and an index and exactly 8,192 into one, and resolves trees of two and four levels while refusing five, a child smaller than its parent says and a child with another chunk size. On real processes (`e2e-transfer.sh`) a ten-chunk file at fan-out 4 is four manifests in two levels and a nine-chunk file at fan-out 2 eleven in four, both round-tripped; a file that would need five levels is refused before its name is registered. Five planted bugs in the manifest checks are each caught in process; the real-process suites never build a hostile manifest, by design.

---

### D-055 — A download writes a part file, flushes it, and renames it into place; a failure leaves nothing named like the file

**Fork:** F7 (open since the Phase 0 audit, defect R2b). **Date:** 8 Oct 2026. **Depends on:** D-054.
**Gates:** what a user can trust about a file that exists.

**The decision.** A download never writes to its destination name until the file is complete and flushed:
1. The whole manifest tree is fetched and checked first (D-054). Nothing touches the disk before that.
2. A part file is created **in the destination's own directory** — `rename(2)` is atomic only within one
   filesystem — named `<dest>.part-<pid>-<n>`, opened with `O_CREAT | O_EXCL` so it can never clobber anything,
   and sized to the file's length with `ftruncate` (a sparse file: no blocks are written).
3. Each chunk, once its bytes hash to its key and its length equals the manifest's count, is written at its offset
   with `pwrite`, looping over short writes.
4. When every chunk is written: `fsync` the part file, `rename` it over the destination, `fsync` the directory.
   Only then does the client report success. A file already at the destination is replaced in one step: a reader
   sees the old file or the new one, never a mixture.
5. **On any failure** the part file is unlinked and the destination is untouched — an older file there survives.
6. **On `SIGINT` or `SIGTERM`** a handler unlinks the part file and exits. `unlink` is on POSIX's list of
   async-signal-safe functions, so it may be called from a handler; the path it unlinks is written into a fixed
   buffer before the file is created, and a flag of type `volatile sig_atomic_t` says whether it is live.
   A signal that lands after the rename unlinks a name that no longer exists, which is harmless — so no ordering
   between the rename and clearing the flag is needed.
7. Only `SIGKILL` or a machine crash can leave a part file behind, and its name says it is partial. Nothing
   resumes from it.

**Rejected: preallocate the destination and write into it** — the legacy client's design, and defect R2b: a failed
download left a full-size file of zeros under the real name. Size is not evidence of completeness.

**Rejected: write to the destination and delete it on failure.** A crash or `SIGKILL` leaves a full-size partial file
under the real name, and a file that was already there is destroyed before its replacement is complete.

**Rejected: hold the whole file in memory and write it at the end.** Memory equal to the file size.

**Rejected: a fixed name, `<dest>.part`, so an interrupted download can be resumed.** Resuming needs a record of which
chunks are done, kept beside the part file and flushed as chunks land; and two downloads to one destination would
share one part file. Named as the way to add resume later, not built.

**Cost accepted.**
- An `fsync` of the whole file before success is reported — its time is measured and reported separately in the
  benchmarks (D-066).
- Disk space for the part file during a download, on the destination's filesystem.
- A `SIGKILL`ed client leaves a `.part-` file that nothing deletes.

**Evidence (8 Oct):** in process — a chunk lost from every replica fails the download naming it, with no file and no part file left; an older file at the destination survives; a manifest lying about a chunk's length is caught; the handler's cleanup run mid-download removes the part file. On real processes (`e2e-transfer.sh`) the same with a chunk deleted from all three stores; `SIGKILL` mid-download leaves exactly one `.part-` file and no destination, and `SIGTERM` leaves nothing and exits 143. `e2e-edge.sh`, which pinned R4 since September, passes 7/7. Mutants that write straight to the destination or keep the part file are caught by both layers; one that drops the `fsync` survives every test, as it must — only a power cut shows it. The flush and rename cost 67 ms for 100 MB (`BENCHMARKS.md` §5, `2e7c445`).

---

### D-056 — A transfer runs a fixed pool of workers, sized at run time; one chunk that cannot be moved fails the whole transfer

**Fork:** F47. **Date:** 8 Oct 2026. **Depends on:** D-016 (all chunk keys are known up front, so every lookup can
run at once). **Gates:** throughput, and the throughput-against-parallelism curve.

**The decision.**
- A transfer starts `--workers N` threads (1 to 64). Each takes the next chunk number from **one atomic counter**
  with `fetch_add` — the inherited client's work queue, kept because it is correct: `fetch_add` returns the old
  value and increments in one indivisible step, so no two workers take the same chunk and none is skipped.
- A worker does all of its chunk's work: for an upload, read the chunk with `pread` and write it through the ring
  (`put_chunk`, D-046); for a download, look up the owner, read through the replicas (`get_chunk`, D-049) and write
  it with `pwrite`. Memory is one chunk per worker.
- **A failed chunk is retried twice**, after 250 ms and then 500 ms — about half and one stabilisation period
  (D-039), so a node that has just died can be evicted between attempts. A chunk that fails three times sets a shared
  `stop` flag (an `std::atomic<bool>`); every worker finishes the chunk in hand and stops; the transfer fails as a
  whole (D-055 deletes the part file). A file with one missing chunk is not a file.
- **The default `N` comes off a measured curve, by a rule fixed before measuring:** the smallest power of two whose
  median 100 MB download throughput, on 16 processes, is within 10 % of the best median measured over
  `N ∈ {1, 2, 4, 8, 16, 32}`. Until it is measured the default is 8, the number of cores.

**Rejected: a thread per chunk.** No pool to size; a 4 GiB file starts 8,192 threads, and every node's 32-body cap
(D-052) is hit at once.

**Rejected: an event loop (`epoll`) with non-blocking sockets.** One thread drives hundreds of transfers and is how
a production client is built. It means rewriting every blocking exchange in `net.cpp`, `lookup.cpp` and `chunk.cpp`
as a resumable state machine — the routing and chunk code the earlier phases tested and measured.

**Rejected: chunks batched by owner, one connection per node.** Fewer connections, but every chunk still needs a lookup
to find its owner, and one slow owner then stalls its whole batch instead of one chunk.

**Cost accepted.**
- Parallelism is capped by the chunk count: a 1 MB file is two chunks, so two workers are all it can use.
- `N` workers put up to `N × RF` bodies in flight across the ring; on a three-process ring 32 workers can reach a
  node's 32-body cap, and `ERR busy` is then retried.
- Blocking threads: each worker spends most of its time waiting on a socket.

**Evidence (`BENCHMARKS.md` §5, `2e7c445`):** median 100 MB download 102 / 183 / 299 / **377** / 363 / 324 MB/s at 1 / 2 / 4 / 8 / 16 / 32 workers on 16 processes; the rule chose **8**, equal to the default, which stands. Per-chunk p50 rises from 9.7 ms at 8 workers to 24 and 51 ms at 16 and 32 — the host's eight cores, not the ring. In process: 1,000 jobs on 8 workers each run exactly once; a failed job stops new work; a chunk that fails twice succeeds on its third attempt. A planted load-then-store in place of `fetch_add` was caught by the pool test.

---

### D-057 — A client is told one or more ring addresses, and starts every lookup at the first that answers

**Fork:** F48. **Date:** 8 Oct 2026. **Depends on:** D-017, D-024. **Gates:** the client's command line.

**The decision.** The client takes `--ring <ip:port>[,<ip:port>...]`. Every lookup starts at the first listed
process that answers; if a put or a get fails because the lookup could not get past its start node, the next listed
address is tried, and is then preferred for the rest of the transfer. The client keeps no other membership: once a
lookup starts, routing finds everything else (D-012).

**Rejected: the tracker hands out ring addresses.** Nodes would register with the tracker and the tracker would have
to expire the dead ones: liveness state on the control plane, the soft state D-008 and D-009 kept out of it. A client
that already knows a manifest key could not read the file while the tracker was down, though nothing about reading a
chunk needs the tracker.

**Rejected: a seed file.** The same list in one more file.

**Rejected: ask the first contact for its successors and fingers, and start lookups across them.** Spreads every
lookup's first request over many nodes instead of one. Costs a request per transfer and a membership the client must
keep fresh; worth building only if the entry node's load is measured to matter — its share is one short line per
chunk.

**Cost accepted.** The user must name at least one live process. Every lookup's first request lands on the same
process.

**Evidence (8 Oct):** `e2e-transfer.sh` gives the client two entry points and kills only other processes; a download by manifest key with no tracker succeeds. The entry node's share of lookup load was not measured separately.

---

### D-058 — The tracker becomes the namespace: four messages, no users, no seeders, and an effect log flushed before each answer

**Fork:** F49. **Date:** 8 Oct 2026. **Depends on:** D-004, D-009, D-017, D-022. **Gates:** the tracker, its
recovery, and what a replicated tracker would replicate.

**The decision.** The tracker holds `name → manifest key` and nothing else (D-017):

```
REGISTER <name> <manifest-key>  ->  OK | EXISTS <key> | ERR <reason>
RESOLVE <name>                  ->  FILE <key> | MISSING | ERR <reason>
LIST [<after>]                  ->  NAMES <n> <name> ...   (sorted; as many as fit in one line; NAMES 0 = no more)
PING                            ->  PONG
```

- **Users, groups, logins and join requests are removed** — D-004, which was held back until the Phase 0 baseline
  had been measured through them. It has been (`e381179`).
- **Seeders, `update_seeder` and the `TRACKERS` greeting are removed.** No client serves data any more: chunks live
  in the ring at places computed from their hashes (D-014, D-022), so there is nobody to announce. The greeting
  advertised a multi-tracker feature that never existed (defect C1).
- **A name** is 1 to 200 bytes of visible ASCII — no space, no control character — so it is one word on the wire and
  one field in the log. It is never a path on the tracker's disk. A **key** is checked as a chunk key is (D-045).
- **Persistence is an effect log**, `names_<port>.log` in the tracker's directory, one line per registration:
  `REGISTER <name> <key> <check>`, where `<check>` is the first 8 hexadecimal characters of the SHA-1 of the rest of
  the line. Under the namespace lock, a new registration is appended and flushed with `fdatasync`, then applied, then
  answered `OK` — so the log's order is the order of the map's changes, and an acknowledged name survives a crash.
- **Recovery** replays each record through the same `apply_register` the live path uses (D-009's split, kept: there is
  no code path from replay to anything that could refuse it). A final line without its newline — torn by a crash
  mid-append — and any line whose check does not match are skipped and reported. Neither can be a registration
  that was acknowledged, because the acknowledgement follows the flush. The old `state_<port>.log` is not read.
- Line framing, the one-reply-one-line rule and the read deadline come from `net.cpp` (D-060, D-061); the
  namespace logic lives in `names.cpp` with no sockets, so it is tested in process.

**Why an effect log now.** D-009 kept the command log and split admission from effect, and named logging the effect
(event sourcing) as the better design, deferred. With users gone there is no admission left: every record *is* an
effect. It is also exactly what a replicated state machine replicates — an ordered list of deterministic changes —
so a Raft-replicated tracker would ship these lines.

**Rejected: keep the legacy commands beside the new ones.** Users, groups, soft seeder state and their recovery rules
kept alive for a transfer path that no longer exists.

**Rejected: a new namespace program beside the legacy tracker.** Two trackers to explain. The legacy pair is
reproducible from git at the baseline commit, and keeping it means maintaining it.

**Rejected: no checksum on records.** A record torn inside its last word — `...a3f9` cut to `...a3` — is still a
well-formed line and would replay as a different key; the newline rule alone catches only a tear that loses the
newline.

**Cost accepted.**
- One `fdatasync` per new registration, under one lock: registrations are serialised at roughly the disk's flush rate
  (about 6 ms each here, from D-045's estimate, so on the order of 150 per second — `ESTIMATE`, measured in D-066).
  Group commit is the fix.
- The log only grows: no deletion (D-023), no compaction.
- `LIST` is a full scan in pages; there is no search.

**Evidence (8 Oct, `BENCHMARKS.md` §5 at `2e7c445`):** 77 bytes of log and about 230 bytes of memory per name at 10,000 and 100,000 names; 197–254 registrations a second, the flush-bound ceiling stated above; 100,000 names replayed and listening in 0.39 s. `test-names`: torn final records cut off, records with a changed key character or a wrong check skipped, an old-format line skipped, a disk that refuses writes (`/dev/full`) answered `ERR` with nothing applied and later registrations refused, eight threads racing for one name with exactly one winner. `e2e-persistence.sh` repeats the damage cases after `SIGKILL` on a real tracker. A planted mutant that skips the record check was caught in process and, after the suite gained a same-length altered record, on real processes too; one that drops the flush survives every test, as expected.

---

### D-059 — A name is written once; registering the same key again succeeds, a different key is refused

**Fork:** F50. **Date:** 8 Oct 2026. **Depends on:** D-004, D-023, D-058. **Gates:** what `REGISTER` answers.

**The decision.** `REGISTER <name> <key>` for a name that is not taken records it and answers `OK`. For a name that
is taken: if the key is the same, it answers `OK` and records nothing — so a client whose `OK` was lost, and which
retries, succeeds; if the key differs, it answers `EXISTS <existing key>` and changes nothing. Of two concurrent
uploads of different content to one name, the first registration wins and the other client reports the conflict;
its chunks stay in the ring, unreferenced (D-023).

**Why.** There is no access control (D-004), so a name that can be replaced is a name anyone can replace — and the
replaced manifest becomes unreachable garbage, with no collector to reclaim it (D-023). A write-once namespace with
no deletion is also append-only, which is what keeps its log a plain list of effects (D-058).

**Rejected: the last writer wins.** The usual file-system semantics, and with no owners it lets any client replace
any file silently.

**Rejected: keep every version under a name.** Mutable history per name, a rule for which version a read gets, and
versions that accumulate with nothing to delete them.

**Rejected: overwrite only when asked (`--replace`).** The same vandalism with one more word on the command line,
until ownership exists.

**Cost accepted.** A mistaken upload holds its name until deletion with an owner token is built (D-023).
Changing a file means uploading it under a new name.

**Evidence (8 Oct):** `test-names` and `e2e-transfer.sh`: the same file under the same name again is `OK`, a different one is refused naming the first key, eight concurrent registrants of one name produce one winner and seven `EXISTS` naming it. A finding from the first run: the same bytes uploaded with a different fan-out are a different manifest, and so a different key — idempotence holds for the same parameters only.

---

### D-060 — The client and tracker are rebuilt on the node's framing; the legacy transfer path is removed, and its suites re-pointed

**Fork:** F51. **Date:** 8 Oct 2026. **Depends on:** D-010, D-011, D-021, D-024. **Closes:** the duplication
recorded in D-021.

**The decision.**
- **One framing copy.** `client.cpp` and `tracker.cpp` are rewritten on `net.cpp`, as the node and the `chunk` tool
  already are: one `send_line` enforcing one-reply-one-line (D-010), one `recv_line`, one body framing (D-044), one
  connect with its deadline set before the handshake (D-025). Both ignore `SIGPIPE` (D-011).
- **Logic without sockets, as everywhere else.** The client's work — chunking, manifests (D-054), the worker pool
  (D-056), the part file (D-055) — lives in `transfer.{h,cpp}`, and the tracker's in `names.{h,cpp}` (D-058), each
  with its network calls passed in, so both are tested in process. The client moves chunks with the same
  `put_chunk` and `get_chunk` the `chunk` tool uses (D-046, D-049).
- **The legacy BitTorrent-style code is removed from the tree:** the peer server and `GET_PIECE`, the download
  workers, the heartbeat, users and groups. It stays in history — `pre-resurrection` tags the coursework, and the
  Phase 0 baseline is reproducible at `e381179`.
- **The five legacy suites keep their names and their purpose, re-pointed at the new programs:** `e2e-smoke` (a file
  round trip compared by SHA-1 — the R2 regression), `e2e-edge` (the size sweep across the chunk boundary — R3 and
  R4; all seven cases are now expected to pass, because a zero-byte file is a manifest with zero entries),
  `e2e-persistence` (names survive a restart — R1), `e2e-framing` (malformed requests answer in exactly one line —
  R6), `e2e-hangup` (an abrupt disconnect does not kill the tracker — R9). Each script's header records the change.
- **D-010's plan for length-prefixed tracker replies is withdrawn.** It was scheduled for this phase because a reply
  might one day carry structured output. Under D-017 no tracker reply carries a payload — the largest is a page of
  names, bounded by the line limit — and length prefixes are used where payloads are, in the chunk plane (D-044).

**Defects this closes structurally:** R7 (a peer sized an allocation: `body_length` refuses an oversized length before
anything is allocated), R8 (no timeouts on the peer path: every client socket carries the 5 s deadline, D-024), D3
(a peer-chosen path reached `open`: the only peer-supplied part of any path is a validated key, D-045), and D1, D6,
D7 and D8, whose code is gone.

**Rejected: add the ring path to the legacy client.** Three framing copies remain, and one program carries two
transfer designs and a peer server nothing uses.

**Rejected: keep the legacy pair as separate programs** so the baseline can be re-run from the current tree. It is
re-run from its own commit instead (D-066), which is the stronger reproduction: the exact code that produced it.

**Cost accepted.** The legacy suites no longer test legacy code — it is gone; they test the same properties of the new
programs. The baseline can be re-measured only from `e381179`.

**Evidence (8 Oct, `83d2a3b`):** the five re-pointed suites pass, `e2e-edge.sh` 7/7. One property was lost and is recorded in the suite: the hangup test can no longer show the `SIGPIPE` ignore, because the new tracker stops at the first failed send and Linux raises the signal only on the second (a tracker built without the ignore passes). The baseline was re-run from its own commit on 8 Oct (`BENCHMARKS.md` §5).

---

### D-061 — A server gives a request line, and a body, a fixed time to arrive in full

**Fork:** F52. **Date:** 8 Oct 2026. **Depends on:** D-044, D-052. **Gates:** every receive a node or the tracker
makes on a connection it accepted.

**The decision.** A node and the tracker take `--read-deadline <ms>` (default **10,000 ms**). On a connection they
accepted:
- a **request line** must arrive in full within the deadline of the server starting to wait for it;
- a **body** must arrive in full within the deadline of its header having been read — and so must a refused body
  that is being drained (D-052).

The deadline is enforced in `net.cpp`: before every `recv`, the server waits with `poll()` for **the time remaining**,
not a fixed interval. A peer that trickles one byte at a time therefore cannot stretch it: a line or a body that is
not complete by the deadline closes the connection, which releases its body slot (D-052) and its thread. Clients
(the node calling another node, the `client`, the `chunk` tool) keep their own 5 s per-call deadline (D-024).

**The value.** 10 s is about the time to send a 512 KiB body at 0.4 Mbit/s; on loopback a body takes milliseconds, and
a legitimate sender writes its request as soon as it has connected — no exchange in this system leaves a connection
idle. Reasoned, not measured.

**Rejected: `SO_RCVTIMEO` alone.** A timeout per system call: one byte every 9.9 s keeps a 1,024-byte line alive for
nearly three hours and a 512 KiB body for two months, and that is the slow-loris attack itself.

**Rejected: an event loop with a timer per connection.** The scalable shape — no thread per idle connection at all —
and a rewrite of the node's request handling.

**Rejected: no deadline.** With the chunk path user-facing, any client could hold a node's 32 body slots for as long
as it keeps 32 sockets open.

**Cost accepted.**
- It bounds how long a connection holds a slot, not how many connections there are: thread-per-connection is still
  unbounded, and an attacker who reconnects every 10 s keeps 32 slots full. Per-source limits or authentication are
  the real defence; neither exists.
- Two system calls instead of one for every received byte of a request line on the server side (`recv_line` reads
  one byte per call, D-021).

**Evidence (8 Oct, `b4463e7`):** `test-net` — a line or body trickled a byte every 40 ms fails at a 200 ms deadline in under 500 ms; `e2e-deadline.sh` against a node and the tracker at a 1 s deadline — an idle connection closed at 1.00 s, a trickled line at 1.06 s, a trickled body at 1.02 s with nothing stored, and 32 withheld bodies freed after the deadline while the attacker's sockets stayed open. Planted mutants removing the deadline in `net.cpp` or in the node's read loop are caught.

---

### D-062 — Deduplication is counted at the stores, on stated edits of one file and on one real history

**Fork:** F53. **Date:** 8 Oct 2026. **Depends on:** D-014, D-016, D-054. **Gates:** the deduplication number owed
since D-014.

**The decision.**
- **Counted from outside.** Before and after each upload, the harness counts the chunk files in every node's store.
  New unique chunks = new files ÷ `RF`. The client is not asked: its view is not the system's.
- **The workload, stated before measuring,** from `testdata/huge.iso` (100 MB — exactly 200 chunks of 512 KiB):
  1. the same bytes under a second name;
  2. 1 KiB overwritten in place at the 50 MB mark;
  3. one byte inserted at the 1 MiB mark;
  4. 10 MB appended;
  5. a real history: 20 consecutive commits of this repository, each packed with `git archive` as a tar file,
     uploaded in order.
- **Predicted from fixed-size chunking:** (1) no new chunk, not even the manifest; (2) one new data chunk and a new
  manifest; (3) 198 of 200 data chunks new — every chunk after the insertion shifts by one byte; (4) 20 new chunks
  and a manifest — 100 MB is a whole number of chunks, so no existing chunk changes; (5) close to nothing shared,
  because the tar's first member changes in most commits and everything after it shifts.

**Rejected: the client reports what was already stored.** The `PUT` reply would have to say "already held" — a
protocol change made for a measurement, and a measurement taken by the component being measured.

**Rejected: a real corpus only.** One number that depends on the corpus, and explains nothing; the synthetic edits each
isolate one mechanism, and the real history shows which one dominates in practice.

**Rejected: content-defined chunking** (Rabin fingerprints, FastCDC) to fix case 3. Chunk boundaries chosen by the
content survive an insertion, which is why backup systems use them. It changes what a chunk is, and with it the
512 KiB baseline comparison (D-016). Named as the fix, not built.

**Cost accepted.** Cases 1 to 4 are arithmetic; measuring them checks that the implementation achieves the arithmetic,
not that deduplication is useful. Case 5 is one corpus.

**Evidence (`BENCHMARKS.md` §5, `2e7c445`):** every synthetic case matched its prediction exactly (0, 2, 200 and 21 new unique chunks); the real history of 20 commits shared 1 chunk of 97.

---

### D-063 — The chunk-size sweep runs on a second build whose only change is the largest chunk it accepts

**Fork:** F54. **Date:** 8 Oct 2026. **Depends on:** D-016, D-044, D-052. **Gates:** the chunk-size curve.

**The decision.**
- The largest chunk is a compile-time constant, `CHORDFS_MAX_CHUNK` (default 524,288), from which the framing limit
  (D-044) and the store limit (D-045) both derive. The benchmark builds a second copy of the tree in a scratch
  directory with `make CHUNK_LIMIT=8388608`, and runs **all five points** — 64 KiB, 256 KiB, 512 KiB, 2 MiB,
  8 MiB — on that one build, so the points differ only in the chunk size the client chooses (`--chunk-size`).
- The 512 KiB point is also run on the default build. The two should agree; if they do not, the raised limit itself
  changed something, and that is reported.
- The default build is unchanged: 512 KiB, everywhere.

**Rejected: a run-time limit flag on every program.** The limit is a constant in the framing, the store, the node and
the client; a flag threads one value through all of them, and two programs started with different values would
disagree about what a valid message is.

**Rejected: sweep only up to the limit** (32 to 512 KiB). Five honest points, but none on the large-chunk side whose
cost D-016 predicted (one slow peer holding a whole transfer).

**Rejected: raise the default to 8 MiB.** The body cap (D-052) was sized for 512 KiB: 32 bodies of 8 MiB is 256 MiB per
process under load.

**Cost accepted.** At 2 and 8 MiB each body slot holds 16 times the memory; the harness runs the sweep on a 16-process
ring with the default worker count, which stays well under this machine's memory (`ESTIMATE`: at most workers × 3 bodies
in flight). Two builds to keep straight: the harness records the limit and both fingerprints with every row.

**Evidence (`BENCHMARKS.md` §5, `2e7c445`):** 100.4 / 284.5 / 384.1 / 424.4 / 536.0 MB/s at 64 KiB / 256 KiB / 512 KiB / 2 MiB / 8 MiB; the 512 KiB point on the default build gave 373.0 MB/s, within the sweep build's spread.

---

### D-064 — A tested one-command local cluster now; container files written and marked not yet run

**Fork:** F55. **Date:** 8 Oct 2026. **Depends on:** D-018, D-019. **Gates:** the Quick start.

**The decision.**
1. **`scripts/cluster.sh up | down | status`** starts a tracker and N nodes on this host — the first `--create`,
   the rest `--join` it — each with its own store under one directory, and stops exactly the processes it started
   (by the PIDs it recorded, never a machine-wide `pkill`). The smoke suite drives it, so it is tested
   on every run.
2. **A `Dockerfile` and a `compose.yaml`** are written: a two-stage build from source on `ubuntu:24.04` (the image is
   built for whatever architecture builds it, arm64 or amd64), and a compose file with a tracker and five nodes on a
   network with a fixed subnet. The addresses are fixed because a node's dotted-quad address is also its ring
   identity (D-019) and `--join` takes an address, not a host name. **Docker is not installed on the development
   machine, so these files have never been run**; the README says so next to them, and no claim is made until they
   have been.

**Rejected: the container files only.** Nothing in them can be verified here, and they would be the only way offered to
start a ring.

**Rejected: wait until Docker is installed.** The one-command local cluster is useful now and testable now.

**Rejected: install Docker inside WSL now** (the `docker.io` package). A change to the development machine's setup that
belongs to its owner; Docker Desktop with WSL integration is the planned route.

**Cost accepted.** The container files are unverified until someone runs them, and a fixed subnet can collide with an
existing Docker network on the host that runs it.

**Evidence (8 Oct):** `scripts/cluster.sh` brings up the cluster for `e2e-smoke.sh` and its `down` leaves no port listening. The container files have not been run.

---

### D-065 — Continuous integration builds, runs the unit tests and the fast end-to-end suites; the timing-sensitive runs are on demand

**Fork:** F56. **Date:** 8 Oct 2026. **Gates:** what a green check on the repository means.

**The decision.** `.github/workflows/ci.yml`, on every push and pull request, on two hosted runners — `ubuntu-24.04`
(x86-64) and `ubuntu-24.04-arm` (the development machine's architecture):
- install `g++`, `make`, `libssl-dev`, `python3`;
- `make` with the build log checked for warnings (the build must stay warning-free), then `make check`;
- the end-to-end suites that finish in seconds and do not depend on timing under load: `e2e-node`, `e2e-chord`,
  `e2e-chunk`, `e2e-transfer`, `e2e-deadline`, `e2e-smoke`, `e2e-edge`, `e2e-persistence`, `e2e-framing`,
  `e2e-hangup`.

A second job, started by hand (`workflow_dispatch`), runs `e2e-churn` and the ThreadSanitizer builds — they kill and
freeze processes on timers, and a shared runner's scheduling makes them the likeliest to fail for reasons that are
not bugs.

**It has not run.** It runs only once the repository is pushed, and nothing has been pushed during this work. No status
badge is added until a run has passed.

**Rejected: unit tests only.** Misses the case the end-to-end suites exist for: a program that does not use the tested
code.

**Rejected: every suite on every push.** A check that fails now and then for timing reasons is a check people learn to
re-run and then to ignore.

**Rejected: no CI.**

**Cost accepted.** The x86-64 runner is not the development machine; the arm runner is, but hosted arm runners are a
newer offering whose availability is checked by the first run, not assumed. The timing-sensitive suites only run when
someone asks.

**Evidence:** none — not run; it runs once the repository is pushed.

---

### D-066 — The transfer path is measured against the baseline re-run at its own commit, and against itself at one worker

**Fork:** F57. **Date:** 8 Oct 2026. **Depends on:** D-016, D-053, D-056, D-062, D-063. **Gates:** every Phase 5
number.

**The decision.**
1. **Before and after.** The Phase 0 baseline (`BENCHMARKS.md` §1: one seeder, sequential, 512 KB pieces) is
   **re-measured the same day at its own commit**, `e381179`, with its own harness, as a control: the machine has
   changed since September (it now has more free memory), and a same-day re-run separates machine drift from the
   change. The new path is then measured with the same files (`medium.bin`, `large.dat`, `huge.iso`: 1, 10 and
   100 MB), the same 512 KiB chunks, on 16 processes × 8 virtual nodes, `W = 2`, a fresh ring and tracker for every
   repetition. Each repetition uploads (timed, reported on its own — the baseline has no upload figure) and then
   downloads.
   - **The comparable figure** is the client's own time from its start to every chunk verified and written —
     the baseline's clock also ran from the command to "every piece verified", and its client never flushed.
   - Also reported: the wall time to the client's exit, which adds the process start, the `fsync` and the rename
     (D-055).
   - **The single-variable comparison** is the new path at 1 worker against the default: same ring, same files,
     same code, only the parallelism changed. Against the baseline, the ring, replication, verification and
     parallelism all changed at once, and the report says so.
2. **Throughput against workers** `{1, 2, 4, 8, 16, 32}` at 100 MB on 16 processes — D-056's rule picks the default
   from it — and **against ring size** `{3, 4, 8, 16, 32}` processes at the default.
3. **Per-chunk latency:** the client records, for every chunk, the time from the start of its lookup to its bytes
   verified (and, for a download, written), retries included. p50 and p99 per configuration, pooled over repetitions.
4. **Chunk size** `{64, 256, 512, 2048, 8192}` KiB at 100 MB, workers at the default, 16 processes, on the sweep build
   (D-063); 512 KiB also on the default build.
5. **Deduplication** as D-062.
6. **The tracker:** log bytes and resident memory against names registered (10,000 and 100,000), and registrations
   per second — the evidence D-017 owes for its `O(files)` claim.

Median of at least three repetitions, with the spread; five where the spread exceeds 10 %. Each run records its commit,
build, seeds and configuration. Everything runs on one host (`BENCHMARKS.md` § Known distortions).

**Rejected: compare against the September number alone.** The host is not in the state it was; a difference could be
the machine.

**Rejected: report only new-path-against-baseline.** It moves parallelism, the ring, replication and verification at
once, so the improvement could not be attributed to parallelism — the one-variable rule D-016 exists to protect.

**Cost accepted.** The baseline's single seeder and this ring's 16 processes are different topologies; a fresh ring per
repetition makes the benchmark slower to run.

**Evidence:** `BENCHMARKS.md` §5, at `2e7c445`: the same-day control re-measured the baseline at 17.9 / 54.7 / 70.1 MB/s (September: 20.9 / 46.2 / 71.2); the new path is 5.4× it at 100 MB, and 3.7× its own one-worker figure.

---

### D-067 — A member that says it is orphaned is evicted at once, by any node, its own siblings included

**Fork:** F58, found by the Phase 5 benchmark rather than foreseen. **Date:** 8 Oct 2026. **Depends on:** D-035,
D-040, D-047. **Gates:** every ring with virtual nodes.

**What happened.** A fresh ring of 16 processes × 8 members, started at once by the benchmark harness, did not
become correct within 60 s, and was still wrong minutes later. One process's first maintenance round ran before the
other fifteen were listening. Every connection was refused, and a refusal is proof of death (D-035), so each of its
members evicted all fifteen. One member, O, was left knowing nobody and became an orphan (D-040): it refuses to
route and rejoins through remembered addresses. Its sibling S — a member of the same process, whose real successor
had just been evicted too — fell back onto O as its successor. Then three rules met:
- a member never strikes or evicts a member of its own process (D-047), so S kept O for ever;
- S could not rediscover its real successor, because it learns of it by asking its successor — O — for its
  predecessor, and an orphan answers every such request `ERR orphaned`;
- every rejoin lookup of O's ended at S, which named O itself as the owner, so `join` waited "for it to be evicted"
  (D-040's rule for a restarted node whose old entry is still in the ring).

Reproduced in process (`test-chunk`): with S's real successor placed just after S, as it was on the real ring, O
never rejoined in any of 20 round orders.

**The decision.** In the one wrapper every maintenance call goes through, a reply beginning `ERR orphaned` evicts the
member that sent it — that member only, at once, by any node, its own siblings included. It is the member's own
statement that it has left the ring: proof, like a refused connection, not suspicion like a timeout. Siblings remain
exempt from strikes for timeouts and from refusals, because those are evidence about the process, and if the process
were dead the caller would be dead too (D-047).

With it, the scenario heals in all 20 round orders within 12 rounds: S evicts O, falls back further, and — orphaned
in turn if it has nobody left — rejoins; the lookups for O then end at its real predecessor.

**Rejected: the orphan rejoins in place**, copying its successor list from the member that names it. Faster, but it
adopts a list it has not verified (D-035's rule), and it keeps in the ring a member that has just said it is not in
it.

**Rejected: wired nodes wait until every listed peer is listening before their first round.** It removes this start-up
cause, not the trap: a member can be orphaned later, when every other process it knew dies at once while its own
process survives, and the same three rules would hold it out for good.

**Rejected: fall back to a sibling instead of orphaning.** Two orphaned siblings would make a ring of their own and
claim every key between them — the split D-040 exists to prevent.

**Cost accepted.** A false `ERR orphaned` cannot be produced by a healthy member, but any peer can send that reply:
one bad reply evicts the member it names from the caller's tables, until the member's own stabilise re-inserts it.
That is the same trust the ring already gives a refused connection, and a member re-inserts itself the way it joined.

**Evidence (8 Oct):** `test-chunk` — the scenario under 20 round orders: 0 of 20 heal without this rule, 20 of 20 with
it. On real processes, every suite and every fresh ring of the Phase 5 benchmark (`BENCHMARKS.md` §5) came up correct.

---

### D-068 — Sources in `src/`, unit tests in `tests/`, objects in `build/`, programs at the root

**Fork:** F59. **Date:** 8 Oct 2026. **Gates:** the repository layout and the `Makefile`. No behaviour changes.

**The decision.**
- The 26 source and header files move to `src/` and the seven unit-test files to `tests/`, with `git mv`, so
  `git log --follow` keeps each file's history. Benchmark fingerprints are commits, so every one stays valid.
- Object files and the compiler's dependency files go to `build/`. **The programs stay at the repository root**
  (`./node`, `./client`, `./tracker`, …), where every script, the CI workflow, the container files and the
  documentation already run them.
- The `Makefile` now generates header dependencies with `-MMD -MP` instead of hand-written lists, and every object
  depends on `build/flags`, a file holding the compiler flags, rewritten only when they change — so a build with
  `CHUNK_LIMIT` (D-063) after a default build recompiles everything rather than linking objects built with the other
  limit. The `run-tracker` and `run-client` targets, which still used the retired coursework command lines (D-060),
  are removed; `scripts/cluster.sh` is how a cluster is started.

**Rejected: leave the tree flat.** No moves, no path changes — and 33 source files mixed with documents, scripts and
build output at the top level, which is the first thing a reader of the repository sees.

**Rejected: programs in `build/` or `bin/` too.** The cleanest root, at the price of changing every script's
`$ROOT/node`, the CI workflow, the Dockerfile and every command line in the documentation, for no change in what
anything does.

**Cost accepted.** Old links to root-level source files (for example from issue trackers or earlier documents) now
point one directory up from where the file is. The programs are still written to the root, so it holds build output
(ignored by git).

**Evidence:** `make clean && make`, `make check` and all eleven end-to-end suites pass unchanged after the move; touching
`src/store.h` rebuilds exactly the nine objects that include it.

---

### D-069 — The tracker becomes a Raft group of three replicas; the size is a parameter, and odd

**Fork:** F60. **Date:** 8 Oct 2026. **Depends on:** D-017, D-058, D-059. **Gates:** every decision in the
replicated tracker (D-069 – D-082), and what "the tracker is down" means.

**The decision.** The namespace (`name → manifest key`, D-058) is replicated by Raft across a fixed group of
tracker replicas. **Three by default**; the group is a list of addresses given to every replica, so any size
works, and one replica is the single tracker (D-080). Five is measured beside three, so the cost of tolerating a
second failure is a number rather than a guess.

**Why three.** A Raft group of `n` makes progress while a majority, `⌊n/2⌋ + 1`, is up and can reach each other,
so it tolerates `⌊(n − 1)/2⌋` failures:

| Replicas | Majority | Failures tolerated | A write waits for |
|---:|---:|---:|---|
| 1 | 1 | 0 | its own flush |
| 2 | 2 | **0** | both flushes |
| 3 | 2 | 1 | its own flush and the faster of two followers' |
| 4 | 3 | 1 | the second-fastest of three followers |
| 5 | 3 | 2 | the second-fastest of four followers |

Three is the smallest group that survives a failure. It matches the data plane, which keeps three copies of a
chunk and acknowledges a write after two (D-015): the system as a whole then tolerates one failure in each plane.

**Rejected: two replicas.** The majority of two is two, so a two-replica group stops at the first failure — and
with two machines that can fail instead of one, it stops *more* often than a single tracker. It buys a second
copy of the data and nothing else.

**Rejected: four.** Tolerates one failure, like three, with one more replica to wait for and to keep up; an even
group can also split two against two with no majority on either side.

**Rejected: five as the default.** Tolerates two failures, at four messages per heartbeat round and a write that
waits for the second-fastest of four flushes. On one host the five replicas also share one disk and eight cores, so
what it costs here is mostly contention — which is why it is measured rather than assumed.

**Rejected: primary–backup without consensus** (one fixed primary copying to a standby). Simple until the primary
fails: something has to decide that the standby takes over, and if that something is wrong — the primary was only
slow — two primaries accept writes. Raft's majority rule is that decision.

**Cost accepted.** A second simultaneous failure stops name registration and resolution until a majority is back.
Downloads by manifest key still work without any tracker (D-057). Three processes, three logs and three flushes
per write where there was one.

**Evidence (9 Oct, `BENCHMARKS.md` §7):** three replicas: a new leader 219 ms (median of 20 kills) after the leader is killed; 131 registrations a second at one client. Five replicas: 118 at one client and 256 at 16, against 264 for three — the second tolerated failure is cheap here, because the leader waits for the second-fastest of four followers' flushes on one disk. Failover was timed for three replicas only.

---

### D-070 — The Raft core is a deterministic state machine: no sockets, no threads, no clock

**Fork:** F61. **Date:** 8 Oct 2026. **Depends on:** D-018, D-024, D-060. **Gates:** how consensus is tested.

**The decision.** `src/raft.{h,cpp}` holds the protocol and nothing else. Every input is a method call — a tick
carrying the current time, a request received from another replica, a reply together with the request it
answers, a client's proposal, a read — and every output is returned: the requests to send, the entries to apply,
the reads that may now be answered. Durable state is written through a `Storage` interface the core calls
**before** any output is released. Time is a number passed in; randomness comes from a generator seeded at
construction. The server (`src/replica.cpp`) wraps the core in one lock, threads and sockets (D-072).

**Why.**
- **Consensus fails in histories, not in functions.** The bugs that matter need a message lost, delayed,
  duplicated or reordered, or a replica crashing between two particular steps. On real processes none of that
  can be chosen, and a failure that needed a particular interleaving cannot be replayed. With a pure core, a test
  runs any number of replicas over a simulated network and clock from a fixed seed: every history is
  reproducible, and the safety properties can be checked after every step.
- **It is how this project already builds everything.** Routing, stabilisation, the chunk plane, the transfer
  engine and the namespace all take the network as a parameter (D-018, D-024, D-060), so hostile rings are built
  in process. The same discipline applied to the part that most needs it.
- etcd's Raft library is built the same way (the application owns the network, the disk and the clock), for the
  same reason.

**Rejected: Raft written into the server** — threads, sockets, timers and protocol interleaved, which is how most
first implementations look. Testable only on real processes, where the interesting histories cannot be
constructed and a failure cannot be replayed.

**Rejected: an existing library** (etcd/raft, NuRaft, braft). Mature and tested, and against both this project's
rule of no external libraries and its point.

**Cost accepted.** Two layers to read instead of one. The wrapper's own concurrency — the lock, the sender threads,
the waiting clients — is not covered by the simulation; it is covered by an in-process test with real threads, the
end-to-end suites and ThreadSanitizer (`docs/consensus.md`).

**Evidence (9 Oct):** `test-raft` — 180 seeded histories (3 and 5 replicas; loss up to 30 %, duplication up to 20 %, reordering, partitions, crashes and restarts) with the four safety properties checked after every step and a linearizability check at the end of each; the leader crashed at 120 points of one replication; Figure 8 reproduced by hand. Mutation testing (24 planted bugs, 23 caught; see `docs/testing.md`) — the one bug that first survived, a vote given in a term already held but not saved, exposed a gap in the directed tests and has a test of its own.

---

### D-071 — Replicas talk over the existing framing: a fresh connection per call, the clients' port, one call in flight per follower

**Fork:** F62. **Date:** 8 Oct 2026. **Depends on:** D-010, D-024, D-044, D-061. **Gates:** the replication wire
format and what a lost message is.

**The decision.**
- **A fresh connection per call**, as every originator in this project does (D-024): connect, send one request,
  read one reply, close. A connection that fails is a lost message, and Raft is built to tolerate those.
- **Two calls.** `VOTE <group> <term> <candidate> <last-index> <last-term>` → `VOTED <term> <0|1>`, and
  `APPEND <group> <term> <leader> <prev-index> <prev-term> <commit> <seq> <count> <length>` followed by a body of
  `<count>` entries, one `<term> <command>` line each → `APPENDED <term> <0|1> <index>`, where `<index>` is the
  last index the follower now matches, or on failure where the leader should back up to. `APPEND` joins `PUT`,
  `STORE` and `CHUNK` as a verb whose last word is a body length (D-044); a body is at most 64 KiB of entries.
- **The same port as clients.** The verb tells a replica call from a client request, so a replica is one address
  and the client's list of replicas is the group's list.
- **At most one `APPEND` in flight per follower.** The next carries everything that accumulated meanwhile, so
  replication batches itself under load. A call unanswered within the call deadline (D-073) is presumed lost and
  sent again; `APPEND` is idempotent, so a duplicate is harmless.
- **A group fingerprint** — 8 hexadecimal characters of SHA-1 of the ordered member list — on every call, so a
  replica started with a different list is refused rather than counted (D-079).

**Why a fresh connection.** Every reply read is the reply to the request just sent, by construction (invariant
I6), with no request identifiers and no reconnect logic. The coursework's worst bugs were replies read on a shared
connection by the wrong reader (D-007, D-008).

**Rejected: a persistent connection per pair of replicas.** Saves a handshake per call — a round trip, tens of
microseconds on loopback. In exchange: reconnection on failure, and a reply on a reused connection that may belong
to an earlier call that timed out, unless every call carries an identifier. The desync class of bug, back.

**Rejected: UDP.** Raft tolerates loss, but a batch of entries larger than a datagram needs fragmentation and
reassembly — a second framing scheme for one feature.

**Rejected: a separate port for replica traffic.** Isolates client load from heartbeats, at the price of two
addresses per replica and a second listener; nothing measured suggests the contention matters.

**Cost accepted.** A TCP handshake per call, and a socket in `TIME_WAIT` at the sender after each: with heartbeats
every 60 ms (D-073), about 17 per second per follower. Because one call carries every pending entry, the call rate
is bounded by round trips, not by the write rate. No authentication: anyone who can reach a replica can send it
`VOTE` or `APPEND` (as for every message in this system).

**Evidence (9 Oct):** a three-replica registration on `tmpfs` — two processes, a round trip and its connection set-up, no flush cost — takes 0.95 ms at p50 (`BENCHMARKS.md` §7), which bounds what a fresh connection per call costs here. A replica with a different member list is refused (`e2e-raft.sh`).

---

### D-072 — One lock over the Raft state and the namespace, never held across a network call, held across the flush; one sender thread per peer

**Fork:** F63. **Date:** 8 Oct 2026. **Depends on:** D-030, D-070. **Gates:** the replica's concurrency model.

**The decision.** A replica's Raft core and its namespace sit behind **one `std::mutex`**. Every call into the core
happens under it; **no thread holds it during a network call**. The flush (`fdatasync`) that makes a step durable
**does** happen under it.

Threads: the `accept` loop and one per connection (as before, D-061's read deadline applies); a **ticker** that
calls the core every 5 ms, which starts elections and schedules heartbeats; and **one sender per peer**, which
takes the newest call pending for that peer, sends it without the lock, and feeds the reply back under it. Client
requests wait on a condition variable until their entry is applied or their read can be answered (D-075, D-076).

**Why the flush is inside the lock.** Raft's rule is that a replica updates stable storage *before* responding
— a vote, an append's success, or a leader counting its own entry. With the flush inside the lock, no other
thread can observe, and no reply can carry, state that is not yet durable: the rule holds by construction rather
than by every code path remembering it. It is the same reasoning that put the single tracker's append under its
lock (D-058).

**Why one sender per peer.** A follower that is slow or frozen blocks only its own sender for at most the call
deadline; the leader keeps replicating to the others and committing with them. A peer has **one pending slot**,
and a newer call replaces an unsent older one: safe, because every `APPEND` carries everything the follower needs
from its next index on, and a vote request is replaced only by a newer term's.

**Rejected: release the lock during the flush.** More concurrency — other threads could run while the disk works
— and from then on every reader must distinguish what is durable from what is only in memory. That is exactly where
implementations acknowledge entries that a crash then loses, or forget a vote.

**Rejected: an event loop owning all state** (one thread; others pass it messages). No lock at all, but a
blocking flush blocks the loop just as it blocks the lock holder, and the server would be rebuilt around an event
loop that nothing else in the project uses.

**Rejected: a thread per call** instead of a sender per peer. Unbounded threads when a peer is slow, and two
calls to one follower in flight at once, arriving in either order.

**Cost accepted.** While a flush runs — milliseconds — nothing else on that replica proceeds: heartbeats wait, so
a slow disk can delay them past a follower's timeout (D-073's rule tests for exactly this), and on the leader,
proposals are serialised at the flush rate (D-082 measures it). One lock means the namespace's own mutex is gone:
the namespace is protected by the replica's lock.

**Evidence (9 Oct):** ThreadSanitizer reports nothing through `test-replica-tsan` and `e2e-raft.sh` on `tracker-tsan`; a planted race (a sender taking its slot after releasing the lock): tsan. No election happened in 90 s of saturating writes at any E (`BENCHMARKS.md` §7), so flushes under the lock did not delay heartbeats past a timeout.

---

### D-073 — Timers: heartbeat E/5, election timeout uniform in [E, 2E), call deadline E; E comes off a measured sweep by a rule fixed now

**Fork:** F64. **Date:** 8 Oct 2026. **Depends on:** D-039, D-042, D-072. **Gates:** failover time, and whether a
healthy group holds spurious elections.

**The decision.** One parameter, the **election timeout base `E`** (`--election <ms>`), sets the rest:

- each replica's election timeout is drawn **uniformly from [E, 2E)**, and drawn again at every reset;
- the leader's **heartbeat interval is E/5** — a follower must miss about five heartbeats in a row before it can
  time out;
- the **deadline for a call to another replica is E**: a reply later than an election timeout is useless, because
  the follower would already have started an election;
- a leader that has not heard from a majority for **2E** steps down (D-077).

**The rule for E, fixed before measuring.** Candidates: 150, 300, 600 and 1200 ms. The default is **the smallest
candidate with no leader change in three 30-second runs of a three-replica group under its saturating write load,
with no failure injected**. Election time after a leader is killed is measured at every candidate (at least 20
kills each), so what each choice costs in failover is on the same plot. Until measured, `E = 300 ms` (`ESTIMATE`:
the upper end of the Raft paper's example range).

**Why a rule.** As with the stabilisation period (D-039): failover time grows with E, and spurious elections fall
with it, and neither curve has a knee. Stating the target before the data exists stops the data choosing it. The
target is "never elect without a failure" because a spurious election is an outage of up to one election time
that nothing caused — and the measured risk on this host is real: a flush under the lock delays heartbeats
(D-072), and the host has shown multi-second stalls (`BENCHMARKS.md` §4a).

**Why E/5 and [E, 2E).** The Raft paper requires `broadcast time ≪ election timeout ≪ time between failures`.
A ratio of 5 lets a heartbeat or two be delayed by a flush without an election; etcd uses 10 (100 ms and 1 s) at
twice the heartbeat traffic. Randomising over a range as wide as its base makes it likely that one replica times
out well before the others and wins before anyone else becomes a candidate — the paper's mechanism against split
votes; it measured that 5 ms of spread was already enough to avoid most of them.

**Rejected: the paper's example values by precedent** (150–300 ms, heartbeat 50 ms). Chosen for a dedicated
network, with no derivation for this host's flushes and stalls.

**Rejected: a large, safe constant** (1–2 s). No spurious elections, and every failover costs over a second.

**Rejected: separate parameters** for the heartbeat and the deadline. Three knobs that must keep a fixed
relationship to be correct, offered as if they were independent.

**Cost accepted.** The ratios are reasoned, not swept. A replica's timeout is drawn from its own generator, so two
replicas can still collide; the next round redraws.

**Evidence (9 Oct, `BENCHMARKS.md` §7):** no leader change at any candidate in 3 × 30 s at the saturating load (about 255 registrations a second) — **E = 150 ms** (`136cc01`). Election after a leader kill: median 219 / 435 / 783 / 1559 ms at E = 150 / 300 / 600 / 1200 (20 kills each), about 1.3 – 1.45 E; split votes 6 of 40 at 150 and 300 ms across two runs. The ratios (E/5, [E, 2E), call deadline E) were reasoned, not swept.

---

### D-074 — Each replica's state is one append-only file of self-checking records, flushed before anything leaves; a damaged tail is cut, damage further back refuses to start

**Fork:** F65. **Date:** 8 Oct 2026. **Depends on:** D-045, D-058, D-072. **Gates:** what a replica remembers
through a crash.

**The decision.** `<dir>/raft_<port>.log`, one record per line, each ending in 8 hexadecimal characters of SHA-1 of
the rest of its line (D-058's check):

```
TERM <term> <voted-for | ->        the current term and this term's vote
ENTRY <index> <term> <command>     a log entry
CUT <index>                        entries from <index> on are discarded
```

- **Persist before replying.** Everything one step changes — a new term, a vote, a cut, new entries — is appended as
  one batch and flushed with one `fdatasync` before the core releases any output: a vote, an append's reply, a call
  to another replica, or a leader counting its own entry. The paper's rule, made structural (D-070, D-072).
- **What is persisted:** the current term, the vote in it, and the log — exactly the paper's persistent state. The
  commit index is not: it is relearned from the leader, or, for a single replica, by committing its first entry.
- **Replay** rebuilds term, vote and log. It stops at the first record that is torn (no newline), fails its check,
  or is out of sequence. A crash can damage only what was written after the last completed flush — none of it
  acknowledged — but it may damage it in any order, so valid-looking records can follow a torn one. One batch is at
  most 256 KiB, so **damage that starts within the last 256 KiB of the file is a crash's tail**: it is cut off and
  reported. **Damage further back is not a crash** — something changed flushed data — and the replica refuses to
  start rather than forget a vote or an acknowledged entry.

**Why one file of records.** One flush per step whatever it changed, and term, vote and entries cannot disagree with
each other after a crash, because they are one ordered stream. Every record is self-checking for the reason D-058
gave: a record torn inside its last word is still a well-formed line, and only the check tells.

**Rejected: term and vote in their own small file**, replaced by write-to-temporary, flush, rename, flush the
directory (D-045's method). Three flushes per vote, and two files whose relative order after a crash must be
reasoned about.

**Rejected: refuse to start on any damage.** Safe against a bad disk, but an ordinary crash mid-batch can leave
intact records after a torn one, and every power cut would need an operator.

**Rejected: skip damaged records and continue** — the single tracker's rule (D-058). Safe there, where each record is
independent; in a Raft log a hole breaks the Log Matching property every other guarantee rests on.

**Cost accepted.** The file only grows: replaced entries and `CUT` records stay (no compaction, D-078). Damage to
flushed data within the last 256 KiB is indistinguishable from a crash's tail and is cut — outside the failure
model, as in the paper, which assumes stable storage. A replica that refuses to start needs replacing, which needs a
membership change (D-079).

**Evidence (9 Oct):** `test-raft` (a torn tail, an altered record with an intact one after it, an entry out of sequence, a second vote in one term, damage hundreds of KiB from the end refused, a write the disk refuses) and `e2e-persistence.sh` on a real tracker. Found while testing: damage that is not a crash's but lies inside the last 256 KiB is cut as one — a run cut 3,010 acknowledged records and took the term from 4 back to 1 — the cost stated above, demonstrated. Planted: no `fdatasync` SURVIVED; the tail not cut test-raft, e2e-persistence; far-back damage cut instead of refused test-raft, e2e-persistence.

---

### D-075 — A registration is answered once its own entry is applied; a retry is safe because a name is written once, so there are no client sessions

**Fork:** F66. **Date:** 8 Oct 2026. **Depends on:** D-059, D-072. **Gates:** what `OK` means, and what a client does
when it does not get one.

**The decision.** `REGISTER <name> <key>` at the leader becomes the log entry `REGISTER <name> <key>`, at some index
`i` in the leader's term `t`. The request waits until this replica **applies** index `i`:

- if the entry applied at `i` has term `t`, it is this request's own entry, and the reply is what applying it
  produced: `OK` (the name was free, or already had this key) or `EXISTS <key>` (an earlier entry took it). The
  outcome is decided by **log order**: the first `REGISTER` of a name to be applied wins, on every replica alike;
- if it has another term, this request's entry was overwritten by a later leader and never took effect:
  `UNAVAILABLE`;
- if nothing is applied at `i` within 2 s, the outcome is unknown: `UNAVAILABLE`.

`UNAVAILABLE` always means "retrying is safe": a second `REGISTER` of the same name and key answers `OK` if the
first took effect (D-059), and the same `EXISTS` if another key won.

**Why.** Linearizability needs each operation to take effect at one instant between its request and its reply. Here
that instant is the moment the entry is applied, in log order, and the reply is computed at that instant. Raft's
usual problem with retries — a command applied twice when a client retries after a leader crash — is answered by the
operation itself: applying the same registration twice changes nothing the second time.

**Rejected: client sessions** (a client identifier and sequence number in each entry, and a table of each client's
last reply in the state machine — the Raft paper §8, thesis chapter 6). The standard answer for operations that are
*not* idempotent. Here it would replicate, persist and expire a table to deduplicate something that already is.

**Rejected: answer at commit, before apply.** The outcome — `OK` or `EXISTS` — depends on every earlier entry, and is
not known until this one is applied in order.

**Rejected: answer from the leader's state when the name is already there** (no entry for a retry). Correct, because
names never change once applied — but a second path through the code that saves an entry only on retries and
conflicts, and would become wrong if names ever could change (D-023).

**Cost accepted.** A retried or conflicting registration still adds an entry: the log records attempts, the state
records effects. If an operation that is not idempotent is ever added — deletion (D-023) — client sessions are
needed.

**Evidence (9 Oct):** the linearizability checker in `test-raft` passed every history and crash point; `test-replica` and `e2e-raft.sh` (a writer through a leader kill: every acknowledged name found afterwards). Planted: `OK` at propose, before commit — test-replica, e2e-raft; a waiter given the applied result though its entry was replaced — test-replica (survived the full run; a test was added for it).

---

### D-076 — Reads are linearizable through ReadIndex at the leader; no leases, no follower reads

**Fork:** F67. **Date:** 8 Oct 2026. **Depends on:** D-072, D-075. **Gates:** what `RESOLVE` and `LIST` cost, and
whether they can return stale answers.

**The decision.** `RESOLVE` and `LIST` are answered only by the leader, by **ReadIndex** (Raft thesis §6.4):

1. the leader must have committed an entry of its own term — the no-op every new leader appends — so that its commit
   index covers everything committed before it was elected;
2. it records its commit index as the read's index;
3. it sends a round of heartbeats, and waits until a majority (itself included) has answered calls **sent after the
   read arrived**, still in its term — proof that no newer leader existed when the read was received;
4. it waits until it has applied up to the read's index, and answers from its state.

A follower answers every client request with `REDIRECT <leader>` or `UNAVAILABLE`.

**Why.** It is linearizable with no assumption about clocks: a read cannot miss a write that completed before it
started, and a deposed leader cannot answer at all — it cannot collect a majority in its old term. It costs a round
trip and no disk write.

**Rejected: reads through the log** (each read an entry). Trivially linearizable, and every read then costs a flush on
a majority of replicas and a slot in the log forever.

**Rejected: leader leases** (answer locally while a majority acknowledged within a lease shorter than the election
timeout). No round trip at all, and correct only if clocks advance at bounded rates and no process pauses for longer
than the lease's margin. This host has shown multi-second stalls (`BENCHMARKS.md` §4a): a leader paused across its
lease's end would serve a stale read with no error.

**Rejected: follower reads of found names.** Because a name never changes once applied, a `FILE` answer from any
replica's state is permanently right; only `MISSING` would need the leader. Cheap, and correct only while names are
write-once — deletion (D-023) would make it silently wrong.

**Cost accepted.** Every read costs a heartbeat round to a majority, and all client traffic lands on one replica. A
new leader serves no reads until its no-op commits — one round trip after it wins.

**Evidence (9 Oct):** planted — no majority confirmation: test-raft; no own-term commit first: test-raft; replies to calls sent before the read counted: test-raft. A resolve costs 0.84 ms through three replicas against 0.34 ms for the single tracker (`BENCHMARKS.md` §7).

---

### D-077 — A leader that has not heard from a majority for 2E steps down; pre-vote is not built

**Fork:** F68. **Date:** 8 Oct 2026. **Depends on:** D-073. **Gates:** what clients of a cut-off leader see.

**The decision.** **Check-quorum:** a leader that has not had a reply in its term from a majority of the group
(itself included) within the last **2E** — the longest election timeout any follower can draw — becomes a follower.
Its waiting requests are answered `UNAVAILABLE`, and new ones too, until it learns of a leader. **Pre-vote** (Raft
thesis §9.6) is not built.

**Why.** Without it, a leader cut off from the majority keeps believing it leads: it accepts requests it can never
commit, and its clients wait for their whole deadline. With it, the cut-off leader stops within 2E, by which time the
majority side may already have elected a new leader — so clients are redirected instead of stalled. Safety does not
depend on it: a cut-off leader can never commit (it lacks a majority) and can never answer a read (D-076).

**Rejected: the core protocol only.** Correct, and a cut-off leader's clients each wait out their full deadline.

**Rejected: check-quorum and pre-vote.** Pre-vote stops a replica that was cut off — and whose term kept rising with
every timeout — from forcing a healthy group into an election when it returns: before raising its term, a candidate
asks whether it could win. It costs a second call type and a second election state; what it saves is one election
per healed partition, which the failover measurements put a number on.

**Cost accepted.** A follower returning from a partition with a higher term deposes a working leader: one election's
unavailability (D-073's measurement). A leader that steps down while a majority is merely slow — every reply delayed
past 2E — causes an election that nothing needed.

**Evidence (9 Oct):** `e2e-raft.sh` (both followers frozen: the lone leader answered `UNAVAILABLE`, never `OK`, and stepped down), `test-replica`, `test-raft`. The cost, measured: in 20 of 20 heals in `test-replica` the returning replica's raised term forced an election. Check-quorum disabled (planted): test-raft, test-replica, e2e-raft.

---

### D-078 — No snapshots: the namespace is write-once, so a snapshot would be as large as the log

**Fork:** F69. **Date:** 8 Oct 2026. **Depends on:** D-023, D-059, D-074. **Gates:** restart time and catch-up.

**The decision.** No snapshots and no log compaction. A replica replays its whole file at start; a follower that is
behind, or new, catches up through ordinary `APPEND` calls of up to 64 KiB of entries each.

**Why.** Snapshots exist because a state machine's state is usually much smaller than its history — keys are
overwritten and deleted, so most of the log no longer matters. Here every name that is registered stays, with its
first key, for ever (D-059; no deletion, D-023): the state *is* the set of successful registrations. A snapshot
would remove only the no-ops, the term records, and repeated or conflicting attempts. The single tracker's numbers
bound the cost of not having one: 77 bytes per name, and 100,000 names replayed in 0.39 s (`BENCHMARKS.md` §5).

**Rejected: snapshots with `InstallSnapshot`** (Raft paper §7). The general answer, and a second replication path
(chunked snapshot transfer, a follower switching from entries to a snapshot and back), with its own failure cases,
to save almost nothing.

**Rejected: local compaction only** (each replica snapshots for itself and keeps entries until every follower has
them). Bounds the file, not catch-up, and still adds a snapshot format and its recovery.

**Cost accepted.** Restart and catch-up are proportional to the whole history, not to the state. If deletion is ever
built, names leave the state but not the log, and snapshots become worth having.

**Evidence (9 Oct, `BENCHMARKS.md` §7):** a group of one replays 100,000 registrations and answers in 0.324 s (0.039 s for 10,000); 90.9 bytes of log a name.

---

### D-079 — Membership is fixed: every replica is given the same list, and a fingerprint refuses a mismatched one

**Fork:** F70. **Date:** 8 Oct 2026. **Depends on:** D-069, D-071. **Gates:** how the group is changed.

**The decision.** The group is an ordered list of addresses, `--group <ip:port>,<ip:port>,…`, identical on every
replica, plus each replica's position in it, `--id <i>`. A replica's identity *is* its position. Every `VOTE` and
`APPEND` carries 8 hexadecimal characters of SHA-1 of the list, and a replica refuses a call whose fingerprint
differs from its own. Changing the group means stopping it and restarting every replica with the new list. **No
membership change protocol** — neither joint consensus nor single-server changes — is built.

**Why the fingerprint.** Majorities are computed from the list. Two replicas started with different lists can
disagree about what a majority is, and then two leaders in one term are possible. Refusing the call makes a
configuration mistake an outage, which is visible, instead of a safety failure, which is not.

**Rejected: joint consensus** (Raft paper §6). Changes membership live through a period in which decisions need
majorities of both the old and the new group. General, and the most intricate part of the protocol.

**Rejected: single-server changes** (thesis chapter 4). Add or remove one replica at a time through a log entry —
simpler, and the published version had a safety bug, found after publication, that needs a new leader to commit an
entry of its own term before changing membership. Worth building next; not needed to replicate the namespace.

**Cost accepted.**
- **No live replacement.** A replica whose disk is lost **must not** be restarted empty under its old identity: an
  empty replica can vote for a candidate missing committed entries, and a committed entry held by it and one other
  replica would then be on only one. Replacing it safely needs a membership change. Until then the group runs one
  short, tolerating one fewer failure.
- Restarting the whole group with a new list is an operator procedure, not a tested path.

**Evidence (9 Oct):** a replica started with a different member list is refused and refuses (`e2e-raft.sh`, `test-replica`); with the check removed (planted): test-replica, e2e-raft.

---

### D-080 — One code path: a one-replica group is the single tracker, and the namespace becomes Raft's state machine

**Fork:** F71. **Date:** 8 Oct 2026. **Depends on:** D-058, D-069, D-074. **Gates:** what `./tracker <port>` with no
group runs, and what happens to `names.cpp` and its log.

**The decision.**
- `./tracker <port>` with no `--group` is a **group of one**. It needs no election timeout — no other replica can lead
  — so it becomes leader as soon as its log is replayed, and commits each entry on its own flush. The single tracker
  and the replicated one are the same code.
- `names.cpp` becomes the **state machine**: validation, the map, `apply`, and the answers to `RESOLVE` and `LIST`. It
  loses its log, its flush and its lock: durability is the Raft log's job (D-074), and the lock is the replica's
  (D-072).
- `names_<port>.log` (D-058) is not read, as D-058 did not read the coursework's `state_<port>.log`.

**Why.** Two code paths for the same service means the path the existing suites exercise — the single tracker — is
not the one that matters. With one path, every existing suite (persistence, framing, hangup, deadline, transfer,
smoke, edge) runs against the replicated code.

**Rejected: keep the single tracker as a mode beside the group.** Two persistence formats, two recovery rules, two
sets of tests, and the simple one is the one most tests would keep using.

**Rejected: keep the namespace's own log under Raft**, as the state machine's persistence. Two logs per replica, two
flushes per write, and two files that can disagree after a crash.

**Cost accepted.** The single tracker now writes term records and a no-op at every start; its replay follows D-074
(stop at the first damaged record) rather than D-058 (skip it and continue). A log written by the old tracker is not
imported.

**Evidence (9 Oct):** every earlier suite — persistence, framing, hangup, deadline, transfer, smoke, edge — passes against the group of one; it registers 275 names a second at one client against 269 for the tracker it replaced (`BENCHMARKS.md` §7).

---

### D-081 — The client is given every replica's address, follows redirects, and retries until a deadline

**Fork:** F72. **Date:** 8 Oct 2026. **Depends on:** D-057, D-075, D-076. **Gates:** how a client finds the leader
and what it reports when there is none.

**The decision.** `client --tracker <ip:port>[,<ip:port>…]`, in the order given. For each request:

- a reply of `REDIRECT <ip:port>` is followed at once (at most as many redirects as there are replicas per round, so
  two replicas pointing at each other cannot loop);
- `UNAVAILABLE`, a refused connection or a timeout moves on to the next address;
- after a round in which no replica gave an answer, the client waits 100 ms and starts again from the first, until
  **10 s** have passed (`--tracker-wait <ms>`); then it fails and says what each replica answered;
- any other reply is the answer.

This lives in `src/group.cpp`, with the exchange passed in, so it is tested in process.

**Why.** It is what the client already does for the ring (D-057): addresses on the command line, tried in order. A
redirect costs one extra exchange; a dead replica costs one refused connection.

**Rejected: discover the group from one address** (any replica returns the member list). One address to configure,
one more message, and a list the client must trust from whoever answered first.

**Rejected: followers forward requests to the leader.** One address always works — and a follower holds a connection
open on someone else's behalf, the pattern iterative routing was chosen to avoid (D-012).

**Rejected: a DNS name for the group.** Needs a resolver and leaves the same retry logic to write.

**Cost accepted.** Every invocation carries the whole list. The client is a one-shot program, so it remembers no
leader between runs: the first request of every run starts at the first address.

**Evidence (9 Oct):** `test-raft` (scripted replicas: redirects, an election, two replicas naming each other, the deadline); `e2e-raft.sh`. From a leader's kill to the client's next acknowledged write: 231 ms median (217 – 426) at E = 150 ms. Redirects not followed (planted): test-raft. The client's exchange deadline with one replica is 3 s, longer than a replica's 2 s commit wait; a frozen leader costs a client that 3 s before it moves on (tested, not timed).

---

### D-082 — One flush per proposal on the leader, measured before any batching is built

**Fork:** F73. **Date:** 8 Oct 2026. **Depends on:** D-058, D-072, D-074. **Gates:** write throughput.

**The decision.** Build the direct path first: the leader appends and flushes each proposal under the lock (D-072);
a follower flushes once per `APPEND`, which carries every entry pending for it, so followers batch by themselves.
**Then measure** — write latency and throughput at one client and at many, for one, three and five replicas, with
the logs on disk and on `tmpfs`, where a flush costs nothing — and decide on batching from the numbers.

**Why measure first.** The single tracker's ceiling was its flush, about 200–250 registrations a second (D-058). A
group adds a round trip and a follower's flush to each write, but a follower's flush covers a whole batch, so where
the ceiling moves to is not obvious. The `tmpfs` comparison isolates exactly what the flushes cost with no change to
the code.

**Rejected, for now: group commit on the leader** (proposals collect in memory; one flush covers all of them; each
counts once flushed). The known fix, and more state between proposing and counting.

**Rejected, for now: flushing in parallel with replicating** (thesis §10.2.1: the leader may send entries before its
own flush, counting itself only once flushed). Removes the leader's flush from the critical path; the most intricate
of the three.

**Cost accepted.** Until measured, throughput under concurrent clients is bounded by one flush per registration on
the leader.

**Evidence (9 Oct, `BENCHMARKS.md` §7):** one client: 131 registrations a second through three replicas, 269 for the single tracker (p50 6.9 against 3.4 ms — two flushes in series); on `tmpfs` 0.95 ms, so about 86 % of a replicated write is flushing. Sixteen clients: 264 against 276 — followers batch, and the ceiling is the leader's flush, as before. **Decision from the numbers:** neither group commit nor the parallel leader flush is built — a registration is one per uploaded file, and under load the group already matches the single tracker; group commit would lift the concurrent ceiling, the parallel flush the one-client latency.

---

### D-083 — Every process serves its metrics over a minimal hand-written HTTP/1.0 responder on a separate port

**Fork:** F74. **Date:** 9 Oct 2026. **Depends on:** D-060, D-061. **Gates:** the dashboard, and every number it shows.

**The decision.** A node, a tracker replica and a client each take `--metrics <port>`. On that port one thread
accepts a connection, reads one HTTP request head, and answers `GET /metrics` with the Prometheus text exposition
format (version 0.0.4): `# HELP`, `# TYPE` and one line per series. Then it closes the connection. No metrics port
is opened unless one is asked for.

**Hardened, because the port is reachable by anyone who can reach the node:**
- **One thread, one connection at a time.** A flood of connections waits in the listen backlog; it cannot create
  threads, unlike the protocol port (D-061 bounds those in time, not in number).
- **The request head is capped at 4 KiB** and must arrive whole within 2 s, by the same deadline reader as D-061.
  A slow sender holds the one thread for at most 2 s; a long one is cut off at the cap.
- **Exactly one response, then close:** `200` for `GET /metrics`, `404` for another path, `405` for another method,
  `400` for anything that is not a request line. No keep-alive, no pipelining, no request body. The send is bounded
  by a 2 s send timeout, so a client that never reads cannot hold the thread either.
- The request parser is a pure function, tested in process with constructed input.

**Why a separate port.** The protocol port speaks lines, and `GET` is already a chunk verb (`GET <key>`, D-049), so an
HTTP request there would be read as a malformed chunk request. A separate port can also be firewalled off from the
data plane.

**Rejected: a `METRICS` line on the protocol port, scraped by an exporter.** Every process needs a second process to
translate, and a scrape shares the protocol port's threads.

**Rejected: pushing (StatsD over UDP, or a Pushgateway).** A dead process stops pushing, which looks like a quiet one;
when Prometheus scrapes, a failed scrape *is* the signal (`up == 0`). It also needs another daemon.

**Rejected: a metrics library (prometheus-cpp).** Against the no-external-libraries rule; the format is small enough
to write and test by hand.

**Cost accepted.** HTTP/1.0 only, no TLS and no authentication on the metrics port — like the protocol port (D-004).

---

### D-084 — Histograms with fixed buckets for latencies and hops, counters and gauges for the rest, labels only from fixed sets

**Fork:** F75. **Date:** 9 Oct 2026. **Depends on:** D-083. **Gates:** what the dashboard can compute.

**The decision.**
- **Latencies and hop counts are histograms**: a counter per bucket, plus a sum and a count, so a percentile is
  computed *after* the buckets of many processes are added together (`histogram_quantile`). Latency buckets run from
  0.5 ms to 10 s in a 1–2–5 series; hop buckets are 0 to 10 one at a time, then 12, 16, 24, 32 and 64.
- **Everything that only goes up is a counter** (`_total`); everything that is a level now is a gauge, read when the
  scrape arrives (successor-list length, chunks held, the Raft term).
- **Labels only from small fixed sets** — a verb, a result, an error type, a role. Never a chunk key, a file name, a
  request id or an address: each distinct label value is a new time series held for ever by Prometheus.
- Names follow the Prometheus conventions: a `chordfs_` prefix, base units (seconds, bytes), `_total` on counters.
- The metric set, and why each metric is there, is a table in `docs/observability.md`. Each one answers a question
  the system's own claims raise: is a lookup still O(log N), is the ring repairing itself, is any chunk short of
  three copies, does the tracker have one leader, where does a write's time go.

**Counters are `std::atomic` with relaxed ordering.** A counter publishes no other data, so it needs atomicity but
no ordering. A histogram's `+Inf` bucket is rendered as the sum of the bucket values read in that scrape, so the
exposition is always self-consistent (`_count` equals the `+Inf` bucket) even while observations land mid-scrape.

**Rejected: summaries** (quantiles computed inside each process). A p99 of p99s means nothing, so quantiles cannot be
combined across processes; and the quantile is fixed when the code is written.

**Rejected: averages only.** The tail is what a user waits on, and an average hides it.

**Rejected: metrics derived from the logs** (a log processor counting lines). The metrics would depend on the wording
of log lines, and on every line being kept.

**Cost accepted.** A quantile is only as precise as the bucket edges around it, and a value above the last bucket
is only known to be "more than 10 s".

---

### D-085 — The client serves metrics while it runs; the dashboard's throughput and latency are measured at the nodes

**Fork:** F76. **Date:** 9 Oct 2026. **Depends on:** D-083, D-084. **Gates:** the dashboard's transfer panels.

**The problem.** A `client put` or `get` is a process that lives for as long as one transfer — a 100 MB download takes
about 0.3 s here. Prometheus scrapes on an interval, so most client processes would end before any scrape.

**The decision.**
- The client takes `--metrics <port>` like every other process and serves its counters while it runs: chunks and
  bytes moved, per-chunk latency, its own lookups' hop counts. A long transfer is visible live.
- **The dashboard's throughput and latency panels come from the nodes**, which live as long as the ring: bytes in and
  out, requests by verb and result, and the time each chunk request takes to serve (for a `PUT`, until `W` copies
  exist). Summed over nodes, that is the system's throughput whatever the clients' lifetimes.

**Rejected: a Pushgateway.** Another daemon; the pushed values stay after the client has gone, so a dead client looks
alive; and Prometheus's own documentation limits it to service-level batch jobs.

**Rejected: a metrics file written at exit.** Needs a collector to read the files, and shows nothing during a long
transfer.

**Rejected: a long-running client process.** A different program from the one users run.

**Cost accepted.** The client's own view — its lookups, its retries — is visible only for transfers long enough to be
scraped. The workload driver of the fault-injection runs records every operation's outcome itself.

---

### D-086 — A hand-written logger: one `key=value` line per event, written with one system call

**Fork:** F77. **Date:** 9 Oct 2026. **Depends on:** D-010. **Gates:** reconstructing a request from the logs (D-087).

**The decision.** `src/log.{h,cpp}`: every event is one line in the `logfmt` style —
```
ts=2026-10-09T13:14:16.123Z level=info proc=node:7010 req=6f1c2a9e0b3d4c5a event=chunk.put key=ab12... copies=3 ms=1.9
```
- **Fields:** the wall-clock time in UTC to the millisecond, a level (`debug`, `info`, `warn`, `error`), the process
  (`node:7010`, `tracker:7001`, `client`), the request id when there is one (D-087), an event name, then the
  event's own fields. The levels below the one given by `--log-level` (default `info`) are not formatted at all.
- **One event is one line, always** — D-010's rule for messages applied to logs. A value containing a space, an
  `=`, a quote or a control character is quoted, with `"` and `\` escaped and a newline written as `\n`.
- **One `write(2)` per line.** Lines from different threads never interleave, and no lock is needed: on Linux a
  `write` of at most 4 KiB (`PIPE_BUF`) to a pipe is never split by another writer's, and a `write` to a regular file
  holds the file's position lock (since Linux 3.14), so two threads' lines land one after the other. Longer lines
  are cut to 4 KiB.
- The node's existing event texts (`evict 127.0.0.1:7011 (refused)`) are kept word for word in a `msg` field, so
  every harness that reads them still works.

**Rejected: `spdlog`.** A good library, against the no-external-libraries rule; and the parts used here — a level, a
timestamp, a line — are a hundred lines.

**Rejected: JSON lines.** Heavier escaping and harder to read by eye or grep; `key=value` parses as easily.

**Rejected: the free-text lines kept.** No level, no request id, and a field cannot be found without knowing the
sentence it sits in.

**Cost accepted.** Wall-clock timestamps from different machines are only as comparable as their clocks; the
request id, not the time, is what links lines across processes.

---

### D-087 — A request id travels as a `RID <id>` prefix on every request line, and a server carries it into its own calls

**Fork:** F78. **Date:** 9 Oct 2026. **Depends on:** D-021, D-044, D-047, D-086. **Gates:** tracing one transfer.

**The decision.**
- A client makes one request id per command — 16 random hexadecimal characters, or the one given by
  `--request-id` — and every line it sends carries it: lookups, chunk requests, tracker requests.
  ```
  RID 6f1c2a9e0b3d4c5a TO 3fa4c1d2e5b6a798 FIND_SUCCESSOR 9c2e...
  RID 6f1c2a9e0b3d4c5a PUT ab12... 2 524288
  ```
- A server takes the prefix off before anything else reads the line, and holds the id in a **thread-local** variable
  for as long as it handles that request: every line it logs carries it, and every call it makes on the request's
  behalf carries it on — a coordinator's `STORE`s to the other replicas (D-046), a `SUSPECT`'s probe.
- The prefix is optional and strictly parsed — `RID`, one space, 16 lowercase hexadecimal characters, one space, a
  non-empty request — so tools and harnesses that send bare lines are unaffected.
- `body_length` (D-044), the one place that decides which lines carry a body, looks past the prefix, so sender and
  receiver still cannot disagree about where a body starts.
- Background work has its own ids: each repair round that moves chunks makes one.

**Rejected: a trailing field.** A body-carrying line's *last* word is its length (D-044); a trailing id would break
that rule or need a second one.

**Rejected: a separate header line before each request.** A second line per request doubles what can go wrong in
the framing — exactly the class of desync that D-007 and D-010 removed.

**Rejected: deriving the id from the chunk key.** It names a chunk, not a transfer: one download's 200 chunks could
not be gathered, and two transfers of the same chunk could not be told apart.

**Cost accepted.** 21 bytes more per request line, far inside the 1,024-byte limit; and the id is chosen by the
client, so a hostile client can reuse another's — it is for tracing, not for security.

---

### D-088 — Prometheus and Grafana run from configuration in the repository, against the local cluster, with release binaries kept outside it

**Fork:** F79. **Date:** 9 Oct 2026. **Depends on:** D-064, D-083. **Gates:** the dashboard screenshot.

**The decision.**
- `deploy/` holds the Prometheus configuration, Grafana's provisioning files and the dashboard as JSON — all text,
  so they diff in review like the code.
- `scripts/observe.sh up|down` starts Prometheus and Grafana against the cluster that `scripts/cluster.sh` started,
  from the official arm64 or amd64 release archives unpacked in a directory **outside the repository**
  (`~/.local/opt` by default). Neither is linked into anything: they are tools, like Python for the harnesses.
- `compose.yaml` gains the same two services from their official images, beside a three-replica tracker group —
  written, not yet run, like the rest of the container files (D-064).

**Rejected: configuration only.** Queries that have never run against a live system are guesses; a dashboard that has
never rendered is a description.

**Rejected: waiting for Docker.** Docker is not installed, and the dashboard does not need it.

**Cost accepted.** The scripts depend on the release archives' layout; the versions used are recorded in
`docs/observability.md`.

---

### D-089 — A name is deleted by its owner's secret: the uploader chooses it, the tracker keeps only its hash, and the deletion is a Raft entry

**Fork:** F80. **Date:** 9 Oct 2026. **Depends on:** D-004, D-023, D-059, D-075. **Gates:** deletion, and the
collector (D-090).

**The decision.**
- `client put` makes a random 128-bit secret (or reads one with `--token-file`) and prints it once: whoever holds it
  owns the name. It sends only the secret's SHA-1:
  ```
  REGISTER <name> <manifest-key> <owner-hash>   -> OK | EXISTS <key> | ...
  UNREGISTER <name> <secret>                    -> OK | MISSING | DENIED | ...
  ```
- The leader hashes the secret and proposes `UNREGISTER <name> <owner-hash>`. The state machine applies it in log
  order like any registration: the name is removed if its stored owner hash matches, `DENIED` otherwise. **No secret
  is ever in the Raft log** — only hashes, and a hash cannot be turned back into a delete request.
- A name is written once **per owner** (refining D-059): the same key with the same owner hash is a retry and answers
  `OK`; a different key, or the same key under another owner, answers `EXISTS`. Once deleted, the name is free.
- The old three-word `REGISTER <name> <key>` is still accepted, and registers a name with **no owner: it can never be
  deleted**.
- Deleting a name frees no storage by itself: its chunks become garbage for the collector (D-090), and a chunk shared
  with another file stays live through that file's manifest.

**Rejected: a token the tracker generates and returns.** Raft replicas must apply identical entries, so the leader
would have to choose it and put it in the log. And a retried `REGISTER` (D-075) must be answered `OK`: either the retry
returns the token — so anyone who knows a name and its key (`RESOLVE` tells them) can re-send the registration and
receive the token — or it does not, and a client whose first reply was lost never learns its own token.

**Rejected: user accounts.** D-004 removed them; this needs one secret per name, not an identity.

**Rejected: anyone may delete.** With no access control, `DELETE movie.mp4` would let anyone remove anyone's file.

**Rejected: storing the secret itself.** Every owner's secret would sit in every replica's log file.

**Cost accepted.** The secret crosses the network in cleartext, once, when deleting (no TLS, as everywhere — D-004).
A lost secret means the name can never be deleted. The namespace is no longer write-once, so D-078's "a snapshot would
be as large as the log" now holds only until names are deleted.

---

### D-090 — A separate collector marks every chunk reachable from a live name, lists each node's chunks, and asks each node to drop the rest

**Fork:** F81. **Date:** 9 Oct 2026. **Depends on:** D-014, D-017, D-023, D-054, D-089. **Gates:** reclaiming storage.

**The decision.** A `gc` program, run by an operator (or a timer), in two phases, as `git gc` is:
- **Mark.** List every name and its manifest key from the tracker group (`ENTRIES`, a linearizable read like `LIST`,
  D-076). For each distinct manifest key, fetch the manifest tree through the ring with the client's verified reads
  (D-049, D-054). The live set is every manifest and every data chunk reachable from any name. **If any name's tree
  cannot be read completely, nothing is deleted**: the collection stops and names the file.
- **Find every node** by walking the ring from a seed with `NEIGHBOURS`, collecting distinct processes, until the walk
  returns to where it began.
- **Sweep.** For each node, list the chunks it holds (`KEYS`, a page at a time) and send the ones not in the live set
  back in `DROP` requests, twenty keys each. The node deletes only those old enough (D-091) and answers which.
- `--dry-run` marks and lists, and reports what would be dropped.

**Safe failures.** A node that is down, or missed by the walk, keeps its garbage until the next collection: a leak,
never a loss. A chunk dropped on one replica can be copied back by repair from a replica not yet swept; it is collected
by a later run.

**Rejected: the Raft leader collects.** The control plane would read and delete in the data plane, which D-017 keeps
it out of, and a leader change would stop a collection half way.

**Rejected: every node marks for itself.** Each node would read the whole namespace and every manifest tree — the mark
done N times — and every node would need the tracker's addresses.

**Rejected: nodes delete whatever is not in a live set they are sent** (or a Bloom filter of it). A forged or truncated
set would delete everything; here the node is only ever told what to delete, key by key.

**Cost accepted.** A collection is O(names + chunks) and holds every live key in memory. One name pointing at a
manifest that does not exist blocks every collection until that name is deleted — and only its owner can do that.

---

### D-091 — A chunk is dropped only after a grace period without being written, kept by each node's monotonic clock; a restart counts as a write

**Fork:** F82. **Date:** 9 Oct 2026. **Depends on:** D-023, D-045, D-046, D-090. **Gates:** whether collection can
lose an acknowledged upload.

**The race.** An upload stores every chunk and manifest first and registers its name last (D-017). Between the two, its
chunks are reachable from no name. A collector that marks in that window finds them unreferenced. Deduplication makes
it worse: an upload's chunk may already be held, as an *old orphan* of a deleted file. Storing it again then changes
nothing on disk — `store_put` answers "already held" — so its age says nothing about the upload now using it.

**The decision.**
- **A node drops a chunk only if it has not been written for G** (`--gc-grace`, default 2 h). "Written" includes being
  written *again*: a `PUT` or `STORE` of a chunk already held renews it, as git's `freshen` does for objects.
- **The clock is the node's monotonic clock, and the times are in memory.** A chunk not written since the process
  started counts as written at the start. So a restart makes every chunk young — a crash can only delay collection —
  and a wall clock that jumps forward cannot age anything, as file modification times would.
- **No gap between the check and the delete.** The age check and the removal hold the store's lock, and a duplicate
  write's "already held: renew" holds the same lock, so a write cannot land between them.
- **A client registers only within U of starting** (`--upload-limit`, default 1 h); a slower upload fails and says so.
- **The collector stops dropping D after it began listing names** (`--deadline`, default 30 min), and refuses to sweep
  a node whose grace is below U + W + D, where W is the client's 10 s tracker wait.

**Why that is enough.** Take an upload acknowledged at time `a`. If its name committed before the collector began
listing at `L`, the mark sees it. Otherwise `L < a`; the client started no earlier than `a − U − W`, and every copy it
wrote was written after that; every drop happens before `L + D < a + D`. So a drop comes less than `U + W + D ≤ G` after
the copy's last write, and the node refuses it. **No chunk of an acknowledged upload is dropped.** The argument uses
only durations, each measured by one process's own clock.

**Rejected: generations with upload leases through Raft.** Each upload would open and close a lease (two more
consensus writes), each chunk would carry its generation on disk, and a crashed client's lease still needs a timeout —
the time bound comes back, with more machinery around it.

**Rejected: a pin per chunk, released after registration.** The same time bound for crashed clients, plus pin state on
every node and a release message per chunk.

**Rejected: stopping uploads during a collection.** The tracker cannot see an upload until its last step, so it
cannot know which are in flight without leases.

**Cost accepted.** Garbage lives at least G (2 h). The guarantee covers acknowledged uploads only: a registration whose
client gave up (`UNAVAILABLE`, outcome unknown) and which commits late may name chunks already collected — running the
same `put` again stores them again and repairs it. The inequality holds only if the settings agree; the collector
checks each node's grace, but not the clients' limit. And the deadline bounds when a `DROP` is *sent*, not when it is
*carried out*: a node frozen with a `DROP` unread in its socket would carry it out when it resumes, possibly later than
the argument allows. Losing an acknowledged chunk that way needs every copy's node frozen through the same collection
for most of a grace period; a `DROP` carrying the time it was sent would close it, at the price of trusting clocks to
agree.


**Evidence (10 Oct, `BENCHMARKS.md` §9; `test-gc`, `e2e-gc.sh`):** an upload paused between storing and registering, and
one reusing old orphans of a deleted file, both survive a collection on real processes; 2,000 rounds of a write racing
a drop leave the index, the disk and the byte count agreeing. **One change from the numbers:** `store_drop` flushed the
fan-out directory after every unlink, which held a sweep to 255 drops a second; an unlink undone by a crash is garbage
kept — the safe direction — so the flush was removed (`478cee0`): 5,634 drops a second.
---

### D-092 — `DROP` carries a key shared by the nodes and the collector, compared in constant time

**Fork:** F83. **Date:** 9 Oct 2026. **Depends on:** D-004, D-090. **Gates:** who can destroy data.

**The decision.** A node started with `--gc-key <file>` accepts `DROP <key> <chunk-key> ...` only if the request
carries the same secret (at least 32 hexadecimal characters), compared in time that does not depend on where the first
difference is. A node started without one refuses every `DROP`. The collector reads the same file.

**Why.** Until now nothing on the data plane could destroy data: a chunk is named by its hash, so `PUT` and `STORE`
cannot overwrite it. `DROP` is the first destructive request, and with no access control (D-004) any process that
can reach a node could delete chunks older than the grace — live ones included, since the node does not know what is
live.

**Rejected: no check.** See above.

**Rejected: a signed request with a nonce** (an HMAC over the request and a counter). Stops a sniffed request being
replayed, at the cost of key handling and a replay window; the shared key is the step before it.

**Rejected: the node checking reachability itself.** It would have to read the namespace and every manifest — the
whole mark (D-090) on every node.

**Cost accepted.** The key crosses the network in cleartext with every `DROP`; anyone who can watch the traffic can
reuse it. Like every message here, it needs TLS to be private.

---

### D-093 — Anti-entropy compares one digest per arc before listing keys; read repair stays subsumed, and the window with fewer than three copies is measured

**Fork:** F84. **Date:** 9 Oct 2026. **Depends on:** D-014, D-049, D-050. **Gates:** the plan's "read repair and
anti-entropy" phase.

**What the phase means now.** Read repair and anti-entropy exist to reconcile replicas that hold *different values*
for a key. Here a key is the hash of its value (D-014), so two copies cannot differ: a copy is right or it is
missing, and a reader already rejects a wrong one by its hash (D-049). What is left of both is *missing* copies —
and the time a key spends with fewer than three of them.

**The decision.**
- **A digest before a listing.** Repair's arc check (D-050) — after a ring change, and in every full scan — first
  sends each replica `DIGEST <from> <to>`; the replica answers with the number of keys it holds in that arc and the
  SHA-1 of them in order. If the owner's own count and digest match, the replica holds exactly the owner's keys
  there, and the check is done in one exchange. Only on a mismatch does it list them with `HAVE`, as before.
- **The same in the other direction.** The full scan's hand-off — every process asking the owner of each chunk it holds
  but does not own whether the owner has it — was a `HAVE` per twenty *replica* copies, two thirds of everything
  stored. It now sends the owner a digest of the owner's arc first; a replica in step with its owner matches.
- **Read repair stays subsumed** by repair, and is not built (below).
- **The window is measured**, not assumed: the time from a write's acknowledgement to its last copy (the
  `chordfs_put_replication_lag_seconds` histogram, under load), and the time from a copy disappearing to its return.

**Why a digest and not a tree.** In a healthy ring every arc's replicas match, and one exchange per arc and replica is
the floor; a tree would only make the *mismatched* case cheaper, and a mismatch already means chunks have to move,
which costs more than listing their keys. A replica holding *more* than the owner (a copy the owner lacks) also
mismatches every time; the `HAVE` path then runs as before, and the full scan's hand-off moves that copy to the owner.

**Rejected: subsumed, measure only.** True of read repair, but leaves repair's full scan sending a `HAVE` for every
twenty chunks every minute, forever — the cost D-050 named and left.

**Rejected: Merkle trees on every node** (Dynamo's anti-entropy). O(differences) to find what differs, but a tree per
arc kept in step with every write, rebuilt or persisted across restarts — state to get wrong, for a saving that only
matters when arcs differ.

**Rejected: read repair of missing copies** (the reader sends a chunk back to a replica that answered `MISSING`).
Every fall-through read would upload a chunk from the client's link; repair restores it anyway within a scan; and when
the ring has moved, the reader often cannot tell which process *should* hold the chunk.

**Cost accepted.** The replica computes its digest by scanning its keys (O(keys held) of CPU, no I/O); a replica with
extra copies in an arc costs the old `HAVE` listing on every check until the hand-off moves them.
