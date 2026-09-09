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
| 2 | **Manifest in one line** | ~10 GB files, or ~20k pieces | tracker builds a multi-MB string under `state_mtx`; response may exceed what the client's line reader tolerates | paginate the manifest |
| 3 | **Per-piece `open`/`close`** on the destination | ~1 GB files | syscall time becomes visible next to transfer time | hold the fd for the download |
| 4 | **`state_mtx` global lock** | ~100s of concurrent clients | command latency rises uniformly for everyone | per-group locks, with a stated lock order |
| 5 | **Thread per client** | ~1,000 clients | memory exhaustion from thread stacks before CPU saturates | `epoll` + bounded worker pool |
| 6 | **Single tracker** | — | not a load limit — an *availability* limit. It is a single point of failure at any scale | Raft group (Phase 2) |

**Target system (Chord). Predictions, to be checked against measurement — and recorded now
precisely so they can be checked.**

| Order | What saturates first | At roughly | Symptom | Fix |
|---|---|---|---|---|
| 1 | **Lookup round trips** (if fork F1 chooses iterative) | large rings | latency grows as hops × round-trip time; on loopback invisible, on a real network dominant | recursive lookup, or cache finger entries aggressively |
| 2 | **Stabilisation traffic** | large rings, high churn | background traffic grows with N × frequency, competing with transfers | back off the period adaptively |
| 3 | **Replication write amplification** (if F3 chooses sync-to-all-3) | write-heavy load | every write is as slow as the slowest of three successors | quorum W=2 |
| 4 | **Free RAM on this host** | ~ring size TBD | cannot start more nodes; **this is a measurement artefact, not a system property, and must be labelled as such on the plot** | cloud VMs — cut-order item 2 |

---

## Why not full membership? — the rejected O(1)-hop design

Recorded 9 Sep 2026, during teaching step 7.2. This is the "why not just tell everyone about
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
