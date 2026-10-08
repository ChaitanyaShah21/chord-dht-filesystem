# Design document

The design of chord-dht-filesystem as it stands, organised by decision, with the **alternatives
considered** for each and what choosing differently would have cost. Each section is a summary of
one or more entries in the decision log in [`ARCHITECTURE.md`](../ARCHITECTURE.md), which has the
full reasoning, the date each was decided, and the evidence that has since confirmed or contradicted
it. Subsystem guides: [routing](routing.md) · [membership](membership.md) · [storage](storage.md) ·
[transfer and namespace](transfer.md) · [consensus](consensus.md) · [protocol](protocol.md) · [testing](testing.md).

## Goals and non-goals

**Goals.** Store files across a ring of nodes so that a file survives the loss of any node and of
every one of its uploaders; route to any chunk in O(log N) hops with no central index of data; keep
every read verifiable by the reader; and make every performance claim a measured number tied to a
commit.

**Non-goals** (each a stated scope boundary, not an oversight): a storage engine (chunks are files);
NAT traversal or wide-area deployment; encryption or authentication on the wire; incentives for
untrusted volunteers; users and access control (D-004). Deletion is designed but not built (D-023).

## The shape of the system

- **Data plane:** `node` processes, each 8 Chord ring members (virtual nodes) with a chunk store.
  Chunks are content-addressed and stored on three distinct processes.
- **Control plane:** one `tracker` holding `name → manifest key`, nothing else.
- **Client:** drives every lookup hop itself, moves chunks with a pool of workers, verifies every
  chunk, and writes a file only once it is complete.

---

## 1. Lookups are iterative — [D-012](../ARCHITECTURE.md#d-012--lookups-are-iterative-the-originator-drives-every-hop)

The originator sends `FIND_SUCCESSOR k` to one node, gets back either the owner or a closer node,
and asks that node itself.

| Alternative | Why not | What it would have cost |
|---|---|---|
| **Recursive** (the Chord paper's pseudocode): each node forwards on the originator's behalf | hop counts become the ring's own report; a timeout three hops away cannot be attributed; every node on the path holds a blocked thread for the lookup's duration | the headline measurement self-reported; nested timeouts; a rewrite of the thread-per-connection model |
| **Hybrid**: forward recursively, the last node answers the originator directly | optimises latency, the one cost invisible on loopback, and keeps both problems above | a third message pattern for no measurable gain |

**Accepted cost:** two network traversals per hop instead of one. The fix on a real network is a
short-lived lookup cache at the originator, not a different routing model.

## 2. A 64-bit ring under 160-bit keys — [D-019](../ARCHITECTURE.md#d-019--the-ring-is-64-bits-wide-keys-stay-160-bits-wide)

Placement uses the top 64 bits of SHA-1 in a machine word; a chunk's identity stays its full 160-bit
hash.

| Alternative | Why not |
|---|---|
| **The full 160 bits**, as the paper specifies | hand-written carry arithmetic directly under the hop-count measurement — and because finger tables are self-validating, a carry bug produces correct answers and a silently inflated hop count |
| **`unsigned __int128`** | a compiler extension with no stream operator, for headroom that is already unreachable |
| **32 bits** | with ~100,000 virtual-node identities, a 69 % chance of a node collision |

100,000 identities in 2⁶⁴ collide with probability about 3 × 10⁻¹⁰, and a collision is refused at
join rather than repaired ([D-028](../ARCHITECTURE.md#d-028--an-identifier-collision-at-join-is-refused-not-repaired)).

## 3. Chunks are content-addressed — [D-014](../ARCHITECTURE.md#d-014--chunks-are-content-addressed-the-key-is-the-hash-of-the-value)

A chunk's key is the SHA-1 of its bytes.

| Alternative | Why not |
|---|---|
| **Key = (file, chunk index)**, as the inherited protocol requested pieces | the value at a key becomes mutable, which brings back conflicting replicas, versioning, clock skew, read repair and the full quorum machinery — choosing to have a consistency problem |

**What it buys:** replicas cannot disagree; reads verify themselves; identical chunks are stored once;
`W + R > RF` stops applying, so `W` and `R` are set independently. **What it costs:** no in-place
update, garbage that only a collector could reclaim, and no control over placement.

## 4. Three copies, `W = 2`, `R = 1`, the owner coordinates — [D-015](../ARCHITECTURE.md#d-015--rf--3-w--2-r--1-and-w-is-a-runtime-parameter), [D-046](../ARCHITECTURE.md#d-046--the-owner-coordinates-a-write-it-stores-its-copy-sends-the-others-in-parallel-and-answers-after-w), [D-049](../ARCHITECTURE.md#d-049--a-read-asks-one-replica-checks-the-hash-itself-and-falls-through-to-the-next-on-any-failure)

| Alternative | Why not |
|---|---|
| **`W = 1`** with background propagation | an acknowledged write can be lost if the owner dies first — measured: at the acknowledgement no other copy existed in 20 of 20 trials |
| **`W = 3`** | a write fails during every first failure to protect against a second; measured: with one replica dead, 56 of 300 refused |
| **The client writes every replica** | three uploads from the client, the replica set to learn first, and a dead client leaves partial copies |
| **Chain replication** | latency becomes the sum of the hops; immutable chunks have no order for a chain to protect |
| **`R = 2`, compare two replicas** | detects nothing hashing does not, at twice the traffic |
| **Ask every replica at once** (hedged reads) | triples every read to speed up the rare failed one |

`W` is per request, so all three settings were measured on one ring under one failure ([D-053](../ARCHITECTURE.md#d-053--w-is-compared-on-one-ring-under-one-failure-and-repair-is-judged-from-outside-by-the-same-kind-of-oracle)).

## 5. Replicas on distinct processes, with virtual nodes — [D-047](../ARCHITECTURE.md#d-047--virtual-nodes-several-ring-members-per-process-one-listening-socket-requests-addressed-by-identifier), [D-048](../ARCHITECTURE.md#d-048--a-chunks-replicas-are-the-first-three-distinct-processes-clockwise-from-its-key), [D-051](../ARCHITECTURE.md#d-051--the-number-of-virtual-nodes-per-process-comes-off-a-measured-curve-by-a-rule-fixed-before-measuring)

| Alternative | Why not |
|---|---|
| **The next two ring members**, regardless of process | correct without virtual nodes, silently wrong with them: two copies in one process |
| **One port per virtual node** | nothing on the wire would say which members share a process |
| **One routing table per process** | changes the routing rule underneath the hop-count results |
| **A large fixed number of virtual nodes** (Cassandra's 256) | each member runs its own stabilisation here, so 256 per process is 256 times the traffic |

The number, 8, came off a measured curve by a rule written down first.

## 6. Failure detection: a refusal is proof, a timeout is suspicion — [D-013](../ARCHITECTURE.md#d-013--failure-detection-is-opportunistic-and-a-refused-connection-is-not-a-timeout), [D-035](../ARCHITECTURE.md#d-035--failure-handling-one-strike-for-a-refusal-two-for-anything-else-and-nothing-adopted-unverified)

Detection rides on traffic the system already sends; a refused connection evicts at once, a timeout
needs a second strike, and no candidate is adopted until it answers a ping.

| Alternative | Why not |
|---|---|
| **Active heartbeats** | permanent background traffic, two more constants, and on a test machine where every node shares eight cores, false positives manufactured by the rig |
| **A phi-accrual detector** (Cassandra) | the right answer on a real network; on loopback there is no jitter for it to adapt to |
| **Evict on one timeout** | scheduling stalls on shared cores would evict live nodes |
| **The paper's mechanics alone**, with no verify-before-adopt | a neighbour's stale pointer re-introduces a node that was just evicted |

**Measured limit:** a 5–9 s stall of the whole host evicted live nodes in 3 of 55 runs at 256 nodes —
no fixed deadline short enough to catch a freeze can tell the two apart.

## 7. A node that loses everyone it knew stops routing — [D-040](../ARCHITECTURE.md#d-040--a-node-that-has-lost-every-node-it-knew-refuses-to-route-and-rejoins-through-addresses-it-remembers), [D-067](../ARCHITECTURE.md#d-067--a-member-that-says-it-is-orphaned-is-evicted-at-once-by-any-node-its-own-siblings-included)

| Alternative | Why not |
|---|---|
| **Fall back to itself** (the first implementation) | `(n, n]` claims the whole ring; the tests showed a joiner routed through it forming a second ring that never merges |
| **Stay out, never rejoin** | one bad moment loses a node for good |
| **Orphans elect one of themselves to restart the ring** | an agreement problem, and under a partition it builds the second ring this prevents |
| **Fall back to a sibling virtual node** (for the livelock found later) | two orphaned siblings would claim the ring between them |

Consistency over availability for a node that cannot tell "everyone died" from "I am cut off".

## 8. Joins, stabilisation and the successor list — [D-029](../ARCHITECTURE.md#d-029--a-node-starts-one-of-four-ways-and-listens-only-once-it-has-a-successor), [D-032](../ARCHITECTURE.md#d-032--the-successor-list-holds-8-entries-by-default), [D-033](../ARCHITECTURE.md#d-033--leaving-is-crashing-there-is-no-departure-protocol), [D-034](../ARCHITECTURE.md#d-034--fix_fingers-one-lookup-per-round-and-its-answer-fills-every-finger-it-covers), [D-043](../ARCHITECTURE.md#d-043--stabilise-runs-to-its-fixed-point-within-a-round-not-one-step-per-round)

| Decision | Alternative considered | Why not |
|---|---|---|
| A joining node listens only once it has a successor | listen first (the paper's order) | a fast restart answers while still believing it is alone and claims every key |
| 8 successors | 3, the replication minimum; a length estimated at run time | two adjacent deaths leave a node one failure from orphaned; the estimate is noisy |
| Leaving is crashing | departure messages | saves at most one period, needs signal handling, falls back to the crash path when lost |
| One finger lookup per round fills every finger it covers | one finger per round; every finger every round | 64 rounds per refresh; 64 lookups per node per period for ever |
| Stabilise runs to its fixed point within a round | one step per round (the paper) | after a distant fallback, the walk home takes a period per node — nine adjacent deaths: worst 7.6 s before, 0.79 s after |

## 9. One lock per member, never held across a call — [D-030](../ARCHITECTURE.md#d-030--one-mutex-per-nodes-routing-state-never-held-across-a-network-call)

| Alternative | Why not |
|---|---|
| **A reader-writer lock** | readers barely hold the lock; more expensive per acquisition, for no measured contention |
| **Copy-on-write snapshots** | every update copies the state, and C++17's atomic `shared_ptr` is a hidden lock pool anyway |
| **Hold the lock for a whole round** | every lookup would queue behind a network call |

**Accepted cost:** the check-then-act gap — every result is re-validated against the state it was
based on before it is applied.

## 10. One framing layer: lines, and length-prefixed bodies — [D-010](../ARCHITECTURE.md#d-010--a-reply-is-exactly-one-line-and-the-framing-layer-guarantees-it), [D-044](../ARCHITECTURE.md#d-044--chunks-travel-as-a-header-line-followed-by-exactly-the-number-of-bytes-it-names), [D-060](../ARCHITECTURE.md#d-060--the-client-and-tracker-are-rebuilt-on-the-nodes-framing-the-legacy-transfer-path-is-removed-and-its-suites-re-pointed)

| Alternative | Why not |
|---|---|
| **Fix the five two-line replies and move on** | the next such string brings the defect back, silently |
| **Length-prefix every reply** | makes the human-readable control protocol harder to debug; no reply carries a payload any more |
| **The chunk inside the line, hex- or base64-encoded** | doubles every transfer and removes the line-length limit |
| **A separate data connection** | two connections whose failures must be matched up |
| **Keep the coursework client and tracker alongside the new ones** | three framing copies and two transfer designs |

## 11. A chunk is a flushed file, renamed into place — [D-045](../ARCHITECTURE.md#d-045--the-chunk-store-is-one-file-per-chunk-written-to-a-temporary-name-flushed-then-renamed)

| Alternative | Why not |
|---|---|
| **An append-only log with an index** (Bitcask) | a storage engine — compaction, recovery scans, torn records — outside the scope |
| **Memory only** | a restart loses everything, and three replicas would not fit |
| **No `fsync`** | five to eight times faster per write, and "stored" would mean "in a page cache a power cut erases" |

## 12. Repair is the owner's job — [D-050](../ARCHITECTURE.md#d-050--repair-is-driven-by-each-chunks-owner-checked-whenever-its-arc-or-replica-set-changes-and-in-a-periodic-full-scan)

| Alternative | Why not |
|---|---|
| **A periodic full scan only** | repair waits a whole scan period |
| **Every replica checks every chunk** | three times the traffic, for ever |
| **Hash-tree comparison** (Dynamo's anti-entropy) | the right answer at scale; more than a laptop's chunk counts need |

## 13. Manifests are chunks, and a large file gets a tree — [D-017](../ARCHITECTURE.md#d-017--the-tracker-is-a-mutable-namespace-over-an-immutable-store), [D-054](../ARCHITECTURE.md#d-054--a-manifest-is-a-text-chunk-listing-keys-and-byte-counts-and-a-large-file-gets-a-tree-of-them)

| Alternative | Why not |
|---|---|
| **The tracker holds full manifests** | its state, and any replicated log of it, grows with chunk count instead of file count |
| **A binary manifest** | more entries per chunk, unreadable by eye — and capacity stops mattering once there is a tree |
| **One flat manifest and a 4 GiB limit** | a size a disk image exceeds |
| **A chain of manifests** | a 40 GiB file waits for ten sequential fetches before its chunks can be fetched in parallel |

## 14. A download writes a part file, then renames — [D-055](../ARCHITECTURE.md#d-055--a-download-writes-a-part-file-flushes-it-and-renames-it-into-place-a-failure-leaves-nothing-named-like-the-file)

| Alternative | Why not |
|---|---|
| **Preallocate the destination** (the inherited client) | a failed transfer left a full-size file of zeros under the real name (defect R2b) |
| **Write the destination and delete it on failure** | a crash or `SIGKILL` still leaves a full-size partial file under the real name |
| **Buffer the whole file in memory** | memory equal to the file |
| **A fixed `.part` name, for resuming** | needs a record of finished chunks; not built |

## 15. A fixed pool of workers — [D-056](../ARCHITECTURE.md#d-056--a-transfer-runs-a-fixed-pool-of-workers-sized-at-run-time-one-chunk-that-cannot-be-moved-fails-the-whole-transfer)

| Alternative | Why not |
|---|---|
| **A thread per chunk** | thousands of threads, and every node's body cap hit at once |
| **An `epoll` event loop** | the scalable design, and a rewrite of every tested exchange |
| **Batch chunks by owner** | one slow owner stalls its whole batch |

The size, 8, came off the measured throughput curve by a rule fixed first.

## 16. The tracker holds names only, written once — [D-017](../ARCHITECTURE.md#d-017--the-tracker-is-a-mutable-namespace-over-an-immutable-store), [D-058](../ARCHITECTURE.md#d-058--the-tracker-becomes-the-namespace-four-messages-no-users-no-seeders-and-an-effect-log-flushed-before-each-answer), [D-059](../ARCHITECTURE.md#d-059--a-name-is-written-once-registering-the-same-key-again-succeeds-a-different-key-is-refused), [D-057](../ARCHITECTURE.md#d-057--a-client-is-told-one-or-more-ring-addresses-and-starts-every-lookup-at-the-first-that-answers)

| Alternative | Why not |
|---|---|
| **The tracker holds placement** (chunk → node) | two systems that can disagree about where a chunk lives, one right by construction, the other a cache that goes stale on every join |
| **The tracker hands clients ring addresses** | liveness state on the control plane; reads would need the tracker even with a manifest key in hand |
| **Last writer wins / versions / an overwrite flag** | with no access control, any client could replace any file |
| **A log without per-record checksums** | a record torn inside its last word replays as a different key |

**Accepted cost:** a single point of failure. Its log is a list of effects — what a Raft group would
replicate. Not built.

## 17. A peer gets a fixed time to send what it announced — [D-061](../ARCHITECTURE.md#d-061--a-server-gives-a-request-line-and-a-body-a-fixed-time-to-arrive-in-full)

| Alternative | Why not |
|---|---|
| **`SO_RCVTIMEO` alone** | a per-call timeout: one byte every 9.9 s keeps a 512 KiB body alive for two months |
| **An event loop with timers** | the scalable shape, and a rewrite of request handling |
| **No deadline** | any client could hold a node's 32 body slots for as long as it keeps sockets open |

**Accepted cost:** it bounds how long each connection holds a slot, not how many connections there are.

## 18. Distributed storage, not a file-sharing swarm — [D-022](../ARCHITECTURE.md#d-022--the-system-is-described-as-distributed-storage-not-peer-to-peer-file-sharing)

| Alternative | Why not |
|---|---|
| **Downloaders serve what they fetch** | needs an index of extra holders — the thing the tracker was designed not to be |
| **Trackerless BitTorrent** (peer lists in a DHT) | availability still depends on seeders staying online. Not weighed when the design was made; recorded afterwards, and labelled as such |

**What it costs:** a popular file no longer gains sources. **What it buys:** a file survives every one
of its uploaders going offline.

## 19. How the numbers are made — [D-038](../ARCHITECTURE.md#d-038--reconverged-is-three-milestones-judged-by-an-oracle-in-the-harness), [D-039](../ARCHITECTURE.md#d-039--the-stabilisation-period-and-the-call-deadline-come-off-measured-curves-by-rules-fixed-before-measuring), [D-053](../ARCHITECTURE.md#d-053--w-is-compared-on-one-ring-under-one-failure-and-repair-is-judged-from-outside-by-the-same-kind-of-oracle), [D-066](../ARCHITECTURE.md#d-066--the-transfer-path-is-measured-against-the-baseline-re-run-at-its-own-commit-and-against-itself-at-one-worker)

- **Correctness is judged by an oracle in the harness**, from the live addresses alone — never by the
  nodes. Rejected: nodes reporting their own convergence; successor pointers only; a lookup sweep only.
- **Parameters come off curves, by rules written before measuring** — the stabilisation period, the
  call deadline, the number of virtual nodes, the number of workers. When a rule failed, it was
  restated in the open and the failure kept ([D-041](../ARCHITECTURE.md#d-041--the-deadline-rule-is-applied-under-the-loads-the-benchmark-actually-runs), [D-042](../ARCHITECTURE.md#d-042--the-selection-rules-are-restated-in-statistics-that-a-repeat-run-reproduces)).
- **Comparisons change one variable.** The transfer speed-up is reported twice: against the old
  system re-run the same day at its own commit (5.4×), and against the same ring at one worker
  (3.7×, parallelism alone). Rejected: comparing with the September number alone, where machine drift
  is mixed in.
- **Predictions are written down first, and failed ones are kept.** Two failed: chunk size is not flat
  on loopback ([D-016](../ARCHITECTURE.md#d-016--512-kb-chunks-held-constant-for-the-headline-claim-and-swept-separately)), and `W = 2` is not faster than `W = 3` with healthy replicas ([D-015](../ARCHITECTURE.md#d-015--rf--3-w--2-r--1-and-w-is-a-runtime-parameter)).

## Deferred, with a design

| Item | Design | Why not now |
|---|---|---|
| Deletion ([D-023](../ARCHITECTURE.md#d-023--no-deletion-in-the-mvp-owner-token-unnaming-and-a-mark-and-sweep-collector-designed-for-later)) | owner token returned at upload; mark-and-sweep collector with a grace period | deleting a file's chunks can destroy another file's, and with no access control a plain delete is a vandalism tool |
| A replicated tracker | a Raft group replicating the effect log; every decision made before its code, in [consensus.md](consensus.md) | being built |
| Content-defined chunking | boundaries chosen by a rolling hash (FastCDC) | changes what a chunk is, and every chunk-size comparison with it |
| Hinted handoff ([D-015](../ARCHITECTURE.md#d-015--rf--3-w--2-r--1-and-w-is-a-runtime-parameter)) | a down replica's copy is written to the next node with a note | `W = 2` was enough under the measured failures |
| Connection reuse, owner caching | per-node connections; owners cached per arc | per-hop connections give one unambiguous failure signal; the cost is measured, not yet limiting |
