# Required Reading — chord-dht-filesystem

Per concept: what to read, what to take from it, roughly how long, and explicitly what to skip.
Reading a paper end to end is usually the wrong move — reading the three sections that matter is
the right one.

Sources here are also the answer to "how did you learn this?", which comes up more often than
expected.

---

## Sockets, framing and partial reads

**Needed for:** Phase 0 — understanding the inherited code well enough to fix R2, and to defend
the wire protocol. Also directly relevant to the target company whose round 1 is reported as C
and operating-system internals.

| # | Source | Take from it | Skip | Time |
|---|---|---|---|---|
| 1 | Beej's Guide to Network Programming, §6.2 and §7.5 — https://beej.us/guide/bgnet/ | The `sendall` loop, and **why every `send`/`recv` needs one**. This is the single most reused pattern in the codebase. | §1–5 (setup), and everything on IPv6 for now | 30 min |
| 2 | `man 2 recv` — RETURN VALUE section only | Exactly what `0` versus `-1` mean, and that a short read is **normal, not an error** | Everything else on the page | 5 min |
| 3 | Stevens, *UNIX Network Programming* Vol. 1, §3.9 | `readn` / `writen` / `readline`. **Notice that Stevens' `readline` keeps a static buffer precisely to avoid one-byte-at-a-time reads** — `recv_line` at `client.cpp:183` is the version he warns about | The `select`/`poll` material until Phase 5 | 25 min |

**Read on:** *pending*

**What I actually took away:** *pending — written after reading, in my own words.*

**Still unclear:** *pending* → goes to `DEFENCE.md` Part 4 if it stays unclear.

---

## Chord and distributed hash tables

**Needed for:** Phases 2–4, the entire data plane. **This is the headline of the project.**
Currently `DEFENCE.md` weak spot 1 — the D-001 answer is reasoning, not knowledge.

**W1 reading is §1–2 only.** The rest lands better after the routing is built, and reading it
now would decide forks F1–F5 by absorption rather than by choice, which is what R11 forbids.

| # | Source | Take from it | Skip | Time |
|---|---|---|---|---|
| 1 | Stoica et al., *Chord: A Scalable Peer-to-peer Lookup Service for Internet Applications*, SIGCOMM 2001 — https://pdos.csail.mit.edu/papers/ton:chord/paper-ton.pdf — **§1–2 in W1** | Just the problem statement: what does a DHT replace the central index *with*, and what does Chord claim to guarantee | **§3 onward until W2** — deliberately | 30 min |
| 2 | Same paper, **§4 (Chord protocol) in W2** | Four things specifically: (a) why the finger table has **m** entries and not **N**; (b) what `stabilize()` and `fix_fingers()` each repair, and **what happens if only one of them runs**; (c) why successor *lists* exist separately from finger tables; (d) what the paper says about concurrent joins | §6 simulation results on first pass | 60 min |
| 3 | Same paper, §5 (concurrent operations and failures) | The correctness argument: **lookups remain correct as long as successor pointers are correct, even if every finger table is stale.** That separation between correctness and performance is the thing to be able to state cold | — | 30 min |
| 4 | MIT 6.824 lecture notes on Chord (or the Kademlia paper §2 for contrast) | Enough about the XOR metric to answer "why not Kademlia?" without bluffing | Implementation details of Kademlia | 20 min |

**Read on:** *pending — W1*

**What I actually took away:** *pending.*

**Still unclear:** *pending.*

---

## The C++ concurrency this project actually uses

**Needed for:** all phases. Named explicitly because "I am not fluent in the memory model,
`std::atomic`, or reasoning about races beyond *put a mutex on it*" is the honest starting
position, and one target company's round is reported as C and operating-system internals.

| # | Source | Take from it | Skip | Time |
|---|---|---|---|---|
| 1 | Williams, *C++ Concurrency in Action* (2nd ed.), §3.1–3.2 | Why `lock_guard` and not manual `lock`/`unlock`; **lock ordering as the way to prove absence of deadlock**, which is the answer to "how do you know it can't deadlock?" | §3.3 onward for now | 40 min |
| 2 | Same, §5.1–5.2 | What `std::atomic` guarantees and what it does not — specifically that atomics do **not compose**: two atomic operations are not jointly atomic | The full memory-ordering taxonomy (`memory_order_acquire` etc.) until it is actually needed | 40 min |
| 3 | `man 7 pthreads` + `std::thread::detach` reference | Detached versus joinable, and the consequence: **detached threads mean no clean shutdown path** | — | 15 min |

**Read on:** *pending*

**What I actually took away:** *pending.*

**Still unclear:** *pending.*

---

## Consistency, availability and quorums

**Needed for:** fork **F3** — the most consequential decision in the design and the one the
project round will land on. Also `DEFENCE.md` weak spot 2.

| # | Source | Take from it | Skip | Time |
|---|---|---|---|---|
| 1 | Brewer, *CAP Twelve Years Later: How the "Rules" Have Changed* (IEEE Computer, 2012) | **Why the naive "pick two" framing is wrong**: CAP is a choice made *during a partition*, not a permanent property. This distinction alone separates a good answer from a memorised one | — | 25 min |
| 2 | DeCandia et al., *Dynamo: Amazon's Highly Available Key-value Store* (SOSP 2007), §4.5 and §5 | Quorum with N, R, W and the rule `R + W > N`; what read repair and anti-entropy actually do and when each fires | The gossip and load-balancing sections until Phase 2 | 40 min |
| 3 | Same paper, §4.7 | Hinted handoff — worth knowing it exists as the answer to "what if the successor is down when you write?" | — | 10 min |

**Read on:** *pending — W1, before the fork session*

**What I actually took away:** *pending.*

**Still unclear:** *pending.*

---

## Raft

**Needed for:** Phase 2 (W9–W10). **Cut-order item 1** — if the project slips, this reading is
the first thing that goes, so it is not scheduled before W9.

| # | Source | Take from it | Skip | Time |
|---|---|---|---|---|
| 1 | Ongaro & Ousterhout, *In Search of an Understandable Consensus Algorithm* (USENIX ATC 2014), §5 | Leader election, log replication, and the safety argument | §6 onward on first pass | 60 min |
| 2 | https://raft.github.io/ — the visualisation | Watch an election happen and a partition heal. Ten minutes here is worth an hour of prose | — | 15 min |

**Read on:** *not before W9.*
