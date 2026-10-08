# Where This Breaks at Scale — chord-dht-filesystem

"What breaks first at 10x?" is asked in essentially every systems-design round, and it is
answered far better from observations recorded while building than from speculation in the room.

Captured **as they happen**, not reconstructed at the end.

Format: what was observed → what it implies at scale → what would be done about it.

**Every entry carries a confidence marker, and `REASONED` is not `MEASURED`.** Presenting a
reasoned bottleneck as a measured one is the failure mode this file exists to prevent.

---

### 23 Aug 2026 — audit of the inherited system, before any measurement

**Observed:** nothing measured yet — the system does not build. What follows is read off the
code and is marked accordingly.

**The mechanism (tracker):** `handle_command` takes **one global mutex, `state_mtx`, for its
entire body** (`tracker.cpp:128`), and every command from every client goes through it. The
tracker also spawns **one detached thread per connected client** (`tracker.cpp:466`) with no
upper bound.

**At 10x (≈30 concurrent clients):** the thread-per-client model still holds — 30 threads is
nothing. `state_mtx` is likely still fine because every command is short and purely in-memory.
The first real symptom is more subtle: `append_update_to_file` opens the state file, appends and
closes it **on every single recorded command** (`tracker.cpp:78`), while holding `log_mtx`. That
is an `open`/`write`/`close` syscall trio per write, serialised.

**At 1000x (≈3,000 clients):** a different bottleneck entirely. 3,000 threads at 8 MB of default
stack each is ~24 GB of virtual address space and a scheduler working hard for nothing, on a
machine with 7.5 GiB of RAM. The thread-per-connection model is what fails, well before the
mutex does — and the fix is an event loop (`epoll`) with a bounded worker pool, not a finer lock.

**Would do:** keep the file handle open and `fsync` on a policy rather than per-write (cheap,
large win); then bound the thread count with a pool; only then consider splitting `state_mtx`
per-group, which needs a stated lock order to stay deadlock-free and should not be done on
suspicion.

**Confidence:** `REASONED` — read off the code, **not measured**. The genuinely useful version of
this entry comes after the Phase 0 baseline.

---

### 23 Aug 2026 — the transfer path

**Observed:** read off the code.

**The mechanism:** three separate things that are each fine at one peer and each get worse
together. (1) `write_piece` **opens, seeks, writes and closes the destination file for every
piece** (`client.cpp:107-128`) — 200 open/close pairs for a 100 MB file. (2) `recv_line` issues
**one `recv` syscall per byte** (`client.cpp:183`). (3) The download worker pool is capped at
`min(peers.size(), 4)` (`client.cpp:578`), so **a single seeder gives exactly one worker** and no
parallelism at all.

**At 10x (100 MB → 1 GB, or 4 peers → 40):** the worker cap binds first and hardest. With 40
peers available the code still runs 4 workers, so 90% of available serving capacity is idle. The
per-piece file open becomes measurable but stays second-order.

**At 1000x:** the design assumption underneath breaks — **the whole manifest is one line of
text**. `get_file_info` returns every piece hash in a single newline-delimited response, so a
1 TB file at 512 KB pieces is ~2 million hashes × 41 bytes ≈ 82 MB **in one line**, built in a
single `ostringstream` in memory while `state_mtx` is held. That is the real scaling wall, and it
is a *protocol* wall, not a performance one.

**Would do:** decouple worker count from peer count (workers should track available parallelism,
not seeders); paginate or range-request the manifest; keep the destination file descriptor open
for the life of the download.

**Confidence:** `REASONED`. The manifest-size arithmetic is arithmetic, not measurement, but the
82 MB figure is a hard consequence of the format rather than an estimate.

---

### Pending — after the Phase 0 baseline

The first `MEASURED` entry lands with `BENCHMARKS.md` §1.

---

## The bottleneck ladder

Kept current. The order in which things fall over as load rises — the strongest possible answer
to the 10x question, because it shows the system was thought about rather than just built.

**Current system (legacy). All `REASONED` until the baseline exists.**

| Order | What saturates first | At roughly | Symptom | Fix |
|---|---|---|---|---|
| 1 | **Download worker cap** — `min(peers, 4)` | any file, >4 seeders | throughput flat regardless of how many peers are available | decouple worker count from seeder count |
| 2 | **Manifest in one line** | ~10 GB files, or ~20k pieces | tracker builds a multi-MB string under `state_mtx`; response may exceed what the client's line reader tolerates | ~~paginate the manifest~~ — **removed structurally by D-017**: the tracker returns a 40-byte manifest *hash* and the manifest itself is fetched from the ring, so no large string is ever built |
| 3 | **Per-piece `open`/`close`** on the destination | ~1 GB files | syscall time becomes visible next to transfer time | hold the fd for the download |
| 4 | **`state_mtx` global lock** | ~100s of concurrent clients | command latency rises uniformly for everyone | per-group locks, with a stated lock order |
| 5 | **Thread per client** | ~1,000 clients | memory exhaustion from thread stacks before CPU saturates | `epoll` + bounded worker pool |
| 6 | **Single tracker** | — | not a load limit — an *availability* limit. It is a single point of failure at any scale | Raft group (Block 2) |

**Target system (Chord). Predictions, to be checked against measurement — and recorded now
precisely so they can be checked.**

| Order | What saturates first | At roughly | Symptom | Fix |
|---|---|---|---|---|
| 1 | **Lookup round trips** (if fork F1 chooses iterative) | large rings | latency grows as hops × round-trip time; on loopback invisible, on a real network dominant | recursive lookup, or cache finger entries aggressively |
| 2 | **Stabilisation traffic** | large rings, high churn | background traffic grows with N × frequency, competing with transfers. **Measured 7 Oct (`b171554`): 0.44 % of a core per idle node at a 1 s period, 5.0 % at 50 ms; 256 nodes at 50 ms saturates this 8-core machine (load average 66)** | back off the period adaptively |
| 3 | **Replication write amplification** (if F3 chooses sync-to-all-3) | write-heavy load | every write is as slow as the slowest of three successors | quorum W=2. **Measured 8 Oct (`7d99fc0`): with healthy replicas W = 2 and W = 3 cost the same on loopback (31 vs 32 ms per 512 KiB write, both flushed); with one replica dead W = 3 failed 56 of 300 writes until eviction, W = 2 none** |
| 3a | **Virtual-node maintenance** | V × N members | every member stabilises on its own, so background CPU grows with V. **Measured 8 Oct: about 0.5 % of a core per member at T = 1 s (3.75 % per process at V = 8)** | a shared routing table per process (D-047's rejected option) |
| 4 | **Free RAM on this host** | **2048 node processes** within a 3.4 GiB budget (measured 7 Oct: about 1 MiB per node at that size, growing with N — see the 7 Oct entry) | cannot start more nodes; **this is a measurement artefact, not a system property, and is labelled as such on the plot** | in-process replay past the ceiling (D-027); cloud VMs — cut-order item 3 |

---

## Why not full membership? — the rejected O(1)-hop design

Recorded 9 Sep 2026. This is the "why not just tell everyone about
everyone, and do one-hop lookups?" question, which is asked about every distributed hash table.

**The design:** every node keeps the complete membership list. A lookup is a local computation
plus one network hop to the owner. No routing, no finger tables, no stabilisation.

**Why it does not scale — and the number that says so.** Routing state is O(N) per node, which
is not the problem; 10,000 entries is nothing. The problem is the **event rate**. With average
node lifetime `L`, a ring of `N` nodes generates membership events at rate `N/L`, and each event
must reach all `N` nodes — so total membership traffic scales as **N²/L**, and each individual
node's share of it grows **linearly with N**. There is a ring size past which every node spends
more bandwidth announcing who exists than serving data. There is also a second cost: `N` mutable
replicas of one list that disagree during churn is a *replication* problem invented to avoid a
*routing* problem.

**Where the crossover is:** not at 10 nodes. At small `N` this design is strictly better than
Chord — one hop, no stabilisation, no routing bugs. Amazon's Dynamo and Cassandra gossip full
membership and do one-hop lookups for exactly this reason, because they run at hundreds of nodes
inside one datacentre. **The honest framing is that full membership is correct when `churn × N`
stays small, and Chord is the design for when it does not.** `REASONED`, not measured — this
project will not run at a scale where the crossover is observable, and claiming otherwise would
be inventing a number.

---

### 12 Sep 2026 — a 32-bit ring works perfectly until virtual nodes land

**Observed:** nothing measured — this is arithmetic, done while resolving F12 and marked
accordingly. `REASONED`, not `MEASURED`.

**The mechanism.** A node's position on the ring is `hash(ip:port)` truncated to `m` bits. Two
nodes landing on the same point is a genuine fault: the interval test evaluates the arc `(n, n]`
as the **entire ring**, so a node whose successor shares its identifier concludes it owns every
key in existence — silently, with no crash and no error. Ownership, and with it the `RF = 3`
replica set, becomes "whichever node you happened to ask".

**At today's scale (a ring of 8–64 real nodes, one identity each):** 32 bits is completely safe.
The birthday bound gives a collision probability around 2 × 10⁻¹³ at 64 nodes. Every test would
pass, forever, and the choice would look vindicated.

**At Phase 4 (virtual nodes, ~100 identities per process):** a 1000-node ring is ~100,000 ring
identities. In 2³² the birthday bound gives **p ≈ 0.69** — a collision is the *expected* outcome,
not the tail. The subsystem that breaks it is one this project has already scheduled and has not
built yet, which is exactly why the observation is worth recording now rather than rediscovering
in W4.

**The general shape, which is the transferable part:** the parameter did not become wrong because
load grew. It became wrong because a **later feature multiplied the number of identities by 100
while leaving the space fixed**. Scale questions are usually asked as "what happens at 10x the
traffic"; this is the other kind — 10x the *entities*, from a design change rather than from
users.

**Would do — and did:** ruled 32 bits out at design time on the number rather than after a
symptom (D-019). At 64 bits the same 100,000 identities collide with probability ≈ 3 × 10⁻¹⁰, and
the next line of defence is structural rather than probabilistic: invariant **I8** makes the "am I
alone?" test compare addresses rather than identifiers, which downgrades a collision from "one
node swallows the ring" to "two nodes disagree about a boundary". Detection and recovery is
fork **F15**, scheduled for Phase 3 with the join path.

---

### 13 Sep 2026 — the node has one thread per connection and no read timeout

**Observed:** nothing measured. This comes from reading `node.cpp`, so it is
`REASONED`, not `MEASURED`.

**The mechanism.** Every accepted connection gets a detached thread that blocks in `recv_line`
until the peer sends a line or closes. There is no bound on the number of threads and no read
timeout.

**At today's scale (8 nodes, one test driver):** harmless. The suite opens a few connections per
node and closes them.

**At the scale of the hop-count sweep (thousands of nodes on one machine):** each node's thread
count stays small, because an iterative originator opens one short-lived connection per hop. The
real ceiling is processes and memory (D-018), not threads.

**Where it actually breaks:**
- **A peer that connects and never sends.** It holds a thread forever. Enough of them (a slow-loris
  pattern, or simply a crashed originator whose connections never got a FIN) exhaust threads or
  memory on that node. The `MAX_LINE` cap stops a peer from sending *too much*; nothing stops a
  peer from sending *nothing*.
- **Detached threads at high connection rates.** Around 8 MB of reserved stack per thread is
  virtual, not resident, but thread creation costs a system call per connection.

**Would do.** A read timeout (`SO_RCVTIMEO`) is the one-line fix for the idle peer. But choosing its
value is the same dead-versus-slow question as fork F4 / D-013, so it is **not** being set silently
here; it belongs with Phase 3's failure detection. A bounded worker pool, or `epoll`, is the fix for
connection rate, and it is only worth doing if a measurement shows thread creation matters.

---

### 13 Sep 2026 — a fresh connection per hop fills the originator's TIME_WAIT table

**Observed:** nothing measured. This follows from D-024 and one kernel setting read on this machine,
so it is `REASONED`, not `MEASURED`.

**The mechanism.** The originator opens a connection for each hop and closes it first, so every
finished connection sits in `TIME_WAIT` for 60 s. `TIME_WAIT` blocks the four-tuple (source address
and port, destination address and port), so the budget is roughly 28,232 ephemeral ports
(32768–60999) **per destination node per minute**.

**At today's scale:** safe. `net.ipv4.tcp_tw_reuse = 2` on this kernel lets `TIME_WAIT` ports be
reused for loopback traffic, and every benchmark runs on loopback.

**At 10x (a real network, sustained lookups):** loopback reuse no longer applies. The first node of
every lookup is usually the same one, so it is the first destination to exhaust: above about 470
lookups a second through one entry node (28,232 ports ÷ 60 s), the ports run out and `connect` fails
with `EADDRNOTAVAIL`. That failure looks like a network error, not like a full table.

**Would do.** Reuse connections at the originator, measured with a before and after. Phase 3 also
has to map a failure on a reused socket to the same evidence as a refused connection (D-013).

---

### 13 Sep 2026 — storage only ever grows

**Observed:** nothing measured. This follows from D-014 and D-023, so it is `REASONED`, not
`MEASURED`.

**The mechanism.** Chunks are content-addressed and immutable, and the MVP has no deletion. Changing
one byte of a file produces a new chunk with a new key; the old chunk stays. Uploading the wrong
file leaves its chunks behind for good. Every chunk is stored three times.

**At today's scale:** irrelevant. Test files are uploaded and the ring is torn down.

**At 10x use (files edited and re-uploaded regularly):** stored bytes approach **every version of
every file ever uploaded, times three**, rather than the current files times three. Deduplication
softens it for unchanged chunks. Because chunk boundaries are fixed offsets, though, inserting one
byte near the start of a file shifts every later chunk and dedup saves almost nothing.

**Would do.** D-023's mark-and-sweep collector with a grace period, scheduled for Block 2 and first
in the cut order. The insertion problem is separate. It is solved by **content-defined chunking**,
where boundaries are chosen by a rolling hash of the content rather than by fixed offsets, so an
insertion only disturbs nearby chunks. That is a change to D-016 and is noted here, not scheduled.

---

### 7 Oct 2026 — the membership list dies by scope, but its memory stays in the process

**Observed:** `MEASURED` on 7 Oct 2026, per-node memory of real `node` processes read from
`/proc/<pid>/smaps_rollup` (median of up to 64 sampled processes per ring) and, for the heap, from
`/proc/<pid>/smaps` of one node. The private and heap figures come from a probe run just before
the benchmark, with node sources unchanged since `9637591` and therefore identical to `e042857`. The
proportional-set figures were measured again in the `e042857` benchmark run (`BENCHMARKS.md` §2)
and agree within 1 KiB.

| Ring size | Private memory per node | Proportional set size per node | `[heap]` resident |
|---|---|---|---|
| 16 | 720 KiB | 932 KiB | 152 KiB |
| 256 | 756 KiB | 777 KiB | — |
| 1024 | 864 KiB | 869 KiB | — |
| 2048 | 948 KiB | 950 KiB | 380 KiB |

**The mechanism.** D-020 makes the membership list a local variable in `bootstrap()`, so no lookup
can ever read it. That holds: the list is destroyed when the function returns. But destroying it
returns the memory to the C++ allocator, not to the operating system. The heap grows from 152 KiB
to 380 KiB between a 16-member and a 2048-member ring. The 228 KiB difference matches the growth
in private memory exactly, about 114 bytes per member: the parsed `Peer` entries plus the file's
text, freed but still resident.

**At today's scale:** harmless, and it does not touch routing. It costs about 0.45 GiB across a
2048-node ring on one machine, and it is part of why per-node cost rises from 0.7 to 0.95 MiB.

**At 10x (20,000 members):** about 2.2 MiB of dead heap per node, so a one-machine ring of that
size would spend 44 GiB holding a list nobody reads. Total resident memory grows as O(N²) while
the routing state stays O(N log N), which is exactly the cost D-020 meant to avoid.

**Would do.** Call `malloc_trim(0)` at the end of bootstrap (glibc-specific, one line), or parse the
membership file as a stream so the whole text is never held. Neither is done now. The wired ring is
Phase 2 scaffolding that joins replace in Phase 3, and the ceiling it sets is already measured.
**Confidence:** `MEASURED` for the numbers; the 10x figure is arithmetic from them.

---

### 7 Oct 2026 — how far real processes go on this machine

**Observed:** `MEASURED` at commit `e042857` (`BENCHMARKS.md` §2). Per-node cost, measured as the
drop in the kernel's `MemAvailable` (which includes per-process kernel structures, not only user
memory), was 963 KiB per node at 1024 nodes and 1063 KiB at 2048. The slowest of three 2048-node
ring start-ups took 7.6 s, because every node hashes the whole membership list (about 4.2 million
SHA-1 computations in total, on 8 cores).

**The derivation.** The documented budget is ~3.4 GiB free. Reserving 1 GiB of headroom for the
originator, the oracle, the page cache and measurement noise leaves about 2.4 GiB. 2048 nodes cost
about 2.1 GiB and fit. 4096 nodes would cost **at least** 4096 × 1.04 MiB ≈ 4.2 GiB, more than the
whole budget, and the per-node figure is still rising with N. So the real-process series stops at
2048, and the in-process replay (D-027) continues the curve.

**At 10x:** startup is O(N) per node and O(N²) in total, so a 20,000-node wired ring would need
about 400 million hashes to start. That is another reason the Phase 3 join path, not a membership
file, is how a large ring gets built.

**Confidence:** `MEASURED`. On the day of measurement the machine reported more free memory than the
documented 3.4 GiB (about 8.7 GiB available), so the ceiling is set by the documented budget rather
than by the machine on one particular day.

---

### 7 Oct 2026 — maintenance cost grows as N / T, and one machine runs out of cores first

**Observed:** `MEASURED` at `b171554` (`BENCHMARKS.md` §4). On an idle 64-node ring, CPU per node falls as one
over the stabilisation period: 5.04 % of a core at 50 ms, 2.73 % at 100 ms, 0.77 % at 500 ms, 0.44 % at 1000 ms,
0.25 % at 2000 ms. Each round is a `NEIGHBOURS`, a `NOTIFY`, a `PING` and one finger lookup of a few hops, each a
fresh TCP connection (D-024), so the cost is mostly connection set-up. With 256 nodes at 50 ms the machine
saturated: load average 66 on eight cores, median node-to-node exchange 5.3 ms against 0.3 ms at a 1 s period, and
a slowest exchange of 455 ms.

**At 10x:** the total cost on one host is N × (cost per round) / T. 2560 nodes at the chosen 1 s period would need
about 11 cores, more than this machine has, before any lookup traffic. On a real deployment each node pays only its
own share, so the per-node figure is the one that transfers: under half a percent of a core.

**Would do:** reuse connections between neighbours (D-024 rejected a connection cache for lookups because one dead
node should produce one signal; a neighbour connection is a different case), and lengthen the period adaptively
when nothing has changed for many rounds. **Confidence:** `MEASURED` for the numbers; the 10x figure is arithmetic.

---

### 7 Oct 2026 — at 256 nodes a 200 ms deadline evicted live nodes in one run of five (explained 8 Oct)

**Observed:** `MEASURED` at `b171554`, then re-run with logs kept at `33e64d8` and `edf4225` (`BENCHMARKS.md` §4a).
The same configuration falsely evicted live nodes in 3 of 55 runs (77, 344 and 1,200 evictions). In both runs whose
logs were kept, the cause was a machine-wide stall of 5–9 s with every core in the kernel: 400–680 runnable tasks,
91–99 % system time, context switches down from about 80,000 a second to about 2,000. Every false eviction was two
consecutive 200 ms timeouts; the calling nodes' own timers fired on time (median call 212 ms), so it was the peers
that could not answer. The kernel-level trigger was not identified (no root, so no `perf`); compaction, memory
reclaim and a protocol feedback loop were ruled out. Short stalls in the same runs produced single strikes and no
evictions.

**At 10x:** on one host the stall starves every node at once, observers and observed alike, and the bigger the ring
the more live nodes get evicted and must re-insert themselves (ring correct after 7.9 s with 344 false evictions,
41.9 s with 1,200). With one node per machine a stall would affect one node, which is then correctly suspected.

**Would do:** nothing in the protocol for this host: no deadline short enough to evict a frozen node in seconds can
tell a five-second stall from a freeze. For a real deployment, a phi-accrual detector (D-013's named alternative)
adapts the threshold to observed latency, and a "local pause" guard (Cassandra's) discards a node's own late
timeouts — which would not have helped here, because the observers were not late. For benchmarks, keep the
256-node point and say what it measures. **Confidence:** `MEASURED` for the mechanism; the stall's trigger is
unknown.

---

### 7 Oct 2026 — a wired ring's start-up is O(N) repair, and the harness's resolution is O(N)

**Observed:** `MEASURED` at `b171554`. Starting N `--wired` nodes one after another, the time until the harness
verified every node correct was 2.0 s at 16–64 nodes, 8.1 s at 128 and 12.1 s at 256 (medians). The nodes that
start first see the later ones as refused and evict them, then repair; before D-043 a node that had fallen back far
round the ring walked home one node per period, and a 256-node start failed to converge within 60 s in 3 of 5 runs.
Separately, one poll of every node by the harness took 9 ms at 16 nodes and 148 ms at 256, which is the timing
resolution of every reconvergence figure.

**At 10x:** a 2560-node start would be dominated by the same refusals; the harness's poll would take about 1.5 s,
coarser than the period. The harness would need to probe in parallel processes rather than Python threads.

**Would do:** start wired nodes with their first round delayed until all are listening (a start-up grace), if wired
start were ever used outside a benchmark. **Confidence:** `MEASURED`.

---

### 7 Oct 2026 — a client's `SUSPECT` costs the receiver one ping, without limit

**Observed:** `REASONED` from the code (D-031, D-036). Any client can send `SUSPECT <ip> <port>` naming a node the
receiver holds, and the receiver opens one connection to check it. Under the 769-per-second load runs, only the
lookups that reached the dead node sent one.

**At 10x:** at scale, a dead finger target shared by many nodes and hit by many clients produces a burst of
identical probes; a hostile client can make a node send one ping per message.

**Would do:** remember the outcome of a probe for a short time and answer repeat hints from it, and rate-limit hints
per client. **Confidence:** `REASONED`.

---

### 8 Oct 2026 — virtual nodes cost background CPU in proportion to their number

**Observed:** `MEASURED` at `7d99fc0` (`BENCHMARKS.md` §6). Idle CPU per process at a 1 s period, 16 processes: 0.53 % of
a core with one member, 3.75 % with eight, 10.4 % with 32 — about half a percent per member, because each member runs
its own stabilise, predecessor check and finger lookup every round (D-047). Eight members per process bring the
spread of stored copies down from a coefficient of variation of 0.57 to 0.20 at 64 processes.

**At 10x:** the cost is per member, so a ring of 64 processes at V = 8 maintains 512 members — on this host, the same
background load as a 512-node ring, which §4a shows is past where one 8-core machine stays healthy. On real hardware
each process pays only its own 3–4 %.

**Would do:** pool the members' finger tables per process (the Chord paper's suggestion, D-047's rejected option): one
finger table answers for all members, cutting both hops and maintenance by up to V. **Confidence:** `MEASURED` for the
cost; the saving is `REASONED`.

---

### 8 Oct 2026 — repair is detection-bound here, and a full scan is O(chunks)

**Observed:** `MEASURED` at `7d99fc0`. After one of 16 processes dies, every chunk it held (about 57 of 300, 29 MiB) is
back on three live processes in a median of 1.9 s with one member per process and 2.2 s with eight. Spreading the
repair over ten or more sending processes instead of three did not make it faster: the time is the ring noticing the
death plus one repair round, and the copying shares one host's cores and disk.

**At 10x:** ten times the data per process makes the copy itself visible (290 MiB from one host's disk); ten times the
chunks makes the full scan — a re-hash of everything a process holds, once a minute by default — cost about a second
of CPU per GiB (`ESTIMATE`) and a `HAVE` per owned chunk.

**Would do:** hash-tree (Merkle) comparison between replicas, so a scan costs in proportion to the differences
(D-050's named alternative); and a rate limit on repair traffic so it cannot crowd out client writes.
**Confidence:** `MEASURED` for the repair time; the 10x figures are arithmetic.

---

### 8 Oct 2026 — a frozen node costs a client its whole hang guard

**Observed:** `MEASURED` at `7d99fc0`. With one process frozen (SIGSTOP), the coordinators' own answer times stayed under
50 ms at W = 1 and 2, but the client-side 99th percentile rose to 5.5–10.5 s: a lookup routed through the frozen
process waits out the client's 5 s deadline (D-024) once or twice before its recovery moves on (D-036).

**At 10x:** with more lookups per second, more of them cross the frozen node in the two rounds before it is evicted;
the number of 5-second waits grows with the request rate, not with the ring.

**Would do:** a shorter client deadline for routing hops than for chunk transfers, or hedging a hop that has not
answered after a few round-trip times. **Confidence:** `MEASURED`.
