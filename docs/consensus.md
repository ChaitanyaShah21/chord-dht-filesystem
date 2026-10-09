# Consensus: the replicated tracker

The tracker holds one thing — `name → manifest key` (D-017, D-058) — and until this work it was one process: the
single point of failure left in the system. This guide covers replacing it with a **Raft group**: the decisions
and the alternatives considered for each, how the code is laid out, how it is tested, and what it measured. Every
decision links to its full entry in [`ARCHITECTURE.md`](../ARCHITECTURE.md).

> **Status:** built and tested. The decisions below were made before any of its code was written; the
> implementation and the tests follow them, and the measurements are in [`BENCHMARKS.md`](../BENCHMARKS.md) §7.

## What is replicated

A Raft group keeps an ordered log identical on a majority of replicas, and each replica applies the log, in order,
to a deterministic state machine. Here the state machine is the namespace and the log entries are its effects:

```
REGISTER <name> <manifest-key>     the first REGISTER of a name to be applied wins
NOOP                               appended by every new leader; changes nothing
```

The namespace was shaped for this before any of it was built: names are written once (D-059), so an entry never
needs to know what came before it except whether its name is taken, and the single tracker's log was already a
list of effects (D-058).

## The decisions

### Three replicas — [D-069](../ARCHITECTURE.md#d-069--the-tracker-becomes-a-raft-group-of-three-replicas-the-size-is-a-parameter-and-odd)

A group of `n` makes progress while a majority is up and connected: three tolerate one failure, five tolerate two.
The size is a list given to every replica, so any size runs; three is the default and five is measured beside it.

| Alternative | Why not |
|---|---|
| **Two replicas** | a majority of two is two: it stops at the first failure, more often than one tracker would |
| **Four** | tolerates one failure, like three, with an extra replica to wait for, and can split two against two |
| **Five by default** | two failures tolerated, at four calls per heartbeat and a write waiting for the second-fastest of four flushes |
| **Primary–backup** | someone has to decide the backup takes over; if the primary was only slow, there are two primaries |

### The protocol is a deterministic core — [D-070](../ARCHITECTURE.md#d-070--the-raft-core-is-a-deterministic-state-machine-no-sockets-no-threads-no-clock)

`src/raft.cpp` has no sockets, threads or clock. A tick with the time, a received call, a reply with the call it
answers, a proposal and a read are method calls; the calls to send, the entries to apply and the reads that can be
answered are what comes back. Storage is an interface the core writes through before any of that is released. A test
can therefore run five replicas over a simulated network that loses, delays, duplicates and reorders messages, crash
a replica between any two steps, and replay any history exactly from its seed.

| Alternative | Why not |
|---|---|
| **Raft inside the server** | the histories that break consensus could only be hoped for on real processes, never constructed or replayed |
| **An existing library** (etcd/raft, NuRaft) | against the no-external-libraries rule, and the point |

### Calls between replicas — [D-071](../ARCHITECTURE.md#d-071--replicas-talk-over-the-existing-framing-a-fresh-connection-per-call-the-clients-port-one-call-in-flight-per-follower)

```
VOTE <group> <term> <candidate> <last-index> <last-term>                                  -> VOTED <term> <0|1>
APPEND <group> <term> <leader> <prev-index> <prev-term> <commit> <seq> <count> <length>   -> APPENDED <term> <0|1> <index>
       followed by <length> bytes: <count> lines of "<term> <command>"
```

One fresh connection per call, on the port clients use, as every exchange in this project. At most one `APPEND` in
flight to each follower; the next carries everything that piled up meanwhile, up to 64 KiB. `<group>` is a
fingerprint of the member list (D-079). `<seq>` numbers the leader's calls so a read can tell which acknowledgements
came after it arrived (D-076).

| Alternative | Why not |
|---|---|
| **A persistent connection per pair** | reconnect logic, and a reply that may belong to an earlier call that timed out |
| **UDP** | batches larger than a datagram need a second framing scheme |
| **A separate port for replicas** | two addresses per replica for an isolation nothing measured needs |

### Concurrency — [D-072](../ARCHITECTURE.md#d-072--one-lock-over-the-raft-state-and-the-namespace-never-held-across-a-network-call-held-across-the-flush-one-sender-thread-per-peer)

One mutex over the core and the namespace. Network calls happen only outside it; the flush happens inside it, so no
thread can see — and no reply can carry — state that is not yet on disk. Threads: one per connection, a ticker every
5 ms, and one sender per peer, whose single pending slot always holds the newest call for that peer.

| Alternative | Why not |
|---|---|
| **Release the lock during the flush** | every reader must then tell durable state from state in memory — where votes get forgotten |
| **An event loop owning all state** | a blocking flush blocks the loop just the same, and nothing else in the project uses one |
| **A thread per call** | unbounded threads behind a slow peer, and two calls to one follower arriving in either order |

### Timers — [D-073](../ARCHITECTURE.md#d-073--timers-heartbeat-e5-election-timeout-uniform-in-e-2e-call-deadline-e-e-comes-off-a-measured-sweep-by-a-rule-fixed-now)

One base, E: election timeouts uniform in [E, 2E) and redrawn at every reset; heartbeats every E/5; a call to another
replica gets E; a leader steps down after 2E without a majority. E is the smallest of 150, 300, 600 and 1200 ms with no
leader change in three 30-second runs under saturating write load — a rule fixed before measuring — and failover time
is measured at every candidate.

| Alternative | Why not |
|---|---|
| **The paper's 150–300 ms by precedent** | chosen for a dedicated network, not this host's flushes and stalls |
| **A large constant** (1–2 s) | no spurious elections, and every failover costs over a second |
| **Separate knobs** | three values that must keep a fixed relationship, offered as if independent |

### Persistence — [D-074](../ARCHITECTURE.md#d-074--each-replicas-state-is-one-append-only-file-of-self-checking-records-flushed-before-anything-leaves-a-damaged-tail-is-cut-damage-further-back-refuses-to-start)

```
TERM <term> <voted-for | -> <check>
ENTRY <index> <term> <command> <check>
CUT <index> <check>
```

`raft_<port>.log`, each record checked as the single tracker's were (D-058). Everything a step changes is written as
one batch and flushed once before anything leaves. Replay stops at the first torn, damaged or out-of-sequence record:
within the last 256 KiB (one batch at most) it is a crash's tail, cut and reported; further back it is a damaged
disk, and the replica refuses to start.

| Alternative | Why not |
|---|---|
| **Term and vote in their own file, renamed into place** | three flushes per vote, and two files whose order after a crash must be reasoned about |
| **Refuse to start on any damage** | an ordinary crash mid-batch can leave intact records after a torn one |
| **Skip damaged records** (the single tracker's rule) | a hole in a Raft log breaks Log Matching |

### Writes and retries — [D-075](../ARCHITECTURE.md#d-075--a-registration-is-answered-once-its-own-entry-is-applied-a-retry-is-safe-because-a-name-is-written-once-so-there-are-no-client-sessions)

A `REGISTER` becomes an entry at index `i` in term `t` and waits until index `i` is applied. Same term: the reply is
what applying it produced — `OK` or `EXISTS <key>`. Another term: it was overwritten, `UNAVAILABLE`. Nothing within
2 s: `UNAVAILABLE`. Every `UNAVAILABLE` may be retried, because registering the same name and key twice is `OK` the
second time (D-059).

| Alternative | Why not |
|---|---|
| **Client sessions** (identifier, sequence number, last reply per client) | replicates a table to deduplicate an operation that is already idempotent |
| **Answer at commit** | `OK` or `EXISTS` is only known once applied in order |
| **Answer retries from local state** | a second path that is right only while names can never change |

### Reads — [D-076](../ARCHITECTURE.md#d-076--reads-are-linearizable-through-readindex-at-the-leader-no-leases-no-follower-reads)

ReadIndex at the leader: once it has committed its own no-op, it notes its commit index, confirms it still leads by a
majority answering heartbeats sent after the read arrived, waits until it has applied that far, and answers. No disk
write and no clock assumption. Followers redirect.

| Alternative | Why not |
|---|---|
| **Reads through the log** | a flush on a majority for every read |
| **Leader leases** | correct only with bounded clock drift and pauses; this host has stalled for seconds |
| **Follower reads of found names** | right only while names can never change |

### Check-quorum, not pre-vote — [D-077](../ARCHITECTURE.md#d-077--a-leader-that-has-not-heard-from-a-majority-for-2e-steps-down-pre-vote-is-not-built)

A leader with no reply from a majority for 2E steps down and answers `UNAVAILABLE`, so its clients move on instead
of waiting out their deadline. Pre-vote is not built: a replica returning from a partition with a raised term still
forces one election.

| Alternative | Why not |
|---|---|
| **Neither** | a cut-off leader's clients each wait their whole deadline |
| **Pre-vote as well** | a second call type and election state, to save one election per healed partition |

### No snapshots — [D-078](../ARCHITECTURE.md#d-078--no-snapshots-the-namespace-is-write-once-so-a-snapshot-would-be-as-large-as-the-log)

Names are never overwritten or deleted, so the state is the set of successful registrations and a snapshot would
remove only no-ops, term records and repeated attempts. Restart replays the file; a lagging follower catches up by
ordinary `APPEND` calls.

| Alternative | Why not |
|---|---|
| **Snapshots and `InstallSnapshot`** | a second replication path, to save almost nothing |
| **Local compaction** | a snapshot format and recovery, bounding the file but not catch-up |

### Fixed membership — [D-079](../ARCHITECTURE.md#d-079--membership-is-fixed-every-replica-is-given-the-same-list-and-a-fingerprint-refuses-a-mismatched-one)

`--group a,b,c --id i` on every replica, identical lists. A fingerprint of the list on every call refuses a replica
started with a different one, turning a configuration mistake into a visible outage instead of two majorities.
**A replica whose disk is lost must not be restarted empty under its old identity**; replacing it needs a membership
change, which is not built.

| Alternative | Why not |
|---|---|
| **Joint consensus** | the most intricate part of Raft, for a group that does not change |
| **Single-server changes** | simpler; its published form needed a later fix — next to build, not needed now |

### One code path — [D-080](../ARCHITECTURE.md#d-080--one-code-path-a-one-replica-group-is-the-single-tracker-and-the-namespace-becomes-rafts-state-machine)

`./tracker <port>` alone is a group of one: leader at once, committing on its own flush. `names.cpp` is the state
machine and keeps no log of its own. The old `names_<port>.log` is not read.

| Alternative | Why not |
|---|---|
| **The old tracker as a mode** | two persistence formats and two recovery rules, and the old one would get most of the testing |
| **The namespace's log kept under Raft** | two logs and two flushes per write, which can disagree after a crash |

### Finding the leader — [D-081](../ARCHITECTURE.md#d-081--the-client-is-given-every-replicas-address-follows-redirects-and-retries-until-a-deadline)

`client --tracker a,b,c`: try in order; follow `REDIRECT` at once; on `UNAVAILABLE`, a refusal or a timeout try the
next; after a round with no answer wait 100 ms and go again, for up to 10 s (`--tracker-wait`). Lives in
`src/group.cpp`, tested in process.

| Alternative | Why not |
|---|---|
| **Discovery from one address** | a member list the client must trust from whoever answered |
| **Followers forward to the leader** | a follower holding a connection for someone else's request |
| **DNS** | a resolver, and the same retry logic anyway |

### Flushes — [D-082](../ARCHITECTURE.md#d-082--one-flush-per-proposal-on-the-leader-measured-before-any-batching-is-built)

The leader flushes each proposal; a follower flushes once per `APPEND`, which batches whatever is pending. Measured
first — on disk and on `tmpfs`, where a flush costs nothing — and batching decided from the numbers.

| Alternative | Why not, yet |
|---|---|
| **Group commit on the leader** | the known fix; built only if the numbers say the leader's flush is the limit |
| **Leader flushes in parallel with replicating** | the most intricate option |

## How the code is laid out

| File | What it is | Opens a socket? |
|---|---|---|
| `src/raft.{h,cpp}` | the protocol: elections, replication, the commit rule, ReadIndex, check-quorum | no — and no thread, no clock |
| `src/raft_log.{h,cpp}` | `FileStorage`: the replica's file of `TERM`/`ENTRY`/`CUT` records, replay, the tail rule | no |
| `src/raft_wire.{h,cpp}` | `VOTE`/`APPEND` and their replies as lines and bodies; strict parsers | no |
| `src/names.{h,cpp}` | the state machine: request parsing, `apply_command`, `answer_read` | no |
| `src/replica.{h,cpp}` | one lock around the core and the namespace; the ticker, a sender per peer, waiting clients | no — the network is passed in |
| `src/group.{h,cpp}` | the client's side: the address list and the leader-finding loop | no — the exchange is passed in |
| `src/tracker.cpp` | the program: command line, listener, a thread per connection | yes |

### One step of the core

Every input method of `Raft` ends in `finish()`, which does three things in this order:

1. **Persist.** If the step changed the term, the vote or the log, `Storage::save` is called once with the whole
   state and the first changed index. `FileStorage` writes one batch of records and calls `fdatasync` once. If it
   fails the core stops for good (fail-stop) and every later input is refused.
2. **Commit and apply.** A leader advances its commit index only now — its own new entries count towards a majority
   only once they are durable — and only over entries of its own term (Figure 8). Committed entries are applied in
   order through the apply function.
3. **Resolve reads.** A ReadIndex read is answered once its index is known (the leader has committed in its term),
   a majority has acknowledged calls sent after the read arrived, and the state is applied that far.

Outputs — calls to send, resolved reads — accumulate during the step and are taken by the caller after it returns,
so nothing leaves before step 1 has finished.

### The replica around it

`Replica::handle()` is called by a connection's thread with each request line. Calls from other replicas
(`VOTE`, `APPEND`) are checked against the group fingerprint and handed to the core under the lock; the reply is
whatever the core returns. A client's `REGISTER` registers a waiter at the index and term its entry will get, then
proposes it; the waiter is completed by the apply function when that index is applied — with the result if the
term matches, with `UNAVAILABLE` if a later leader replaced the entry — or times out after 2 s. Registering the
waiter *before* proposing matters for a group of one, which commits and applies inside `propose()` itself.

A **ticker** calls `tick()` every 5 ms. **One sender thread per peer** waits for its slot to be filled, takes the
newest call, releases the lock, makes the call with a deadline of E, and takes the lock again to hand the reply
(with the call it answers) to the core. A failed call is reported as such; the core waits a heartbeat before
calling that peer again, so a dead peer is not called in a tight loop.

### The client

`client --tracker a,b,c` asks through `ask_group()`: fresh connection per exchange with a 3 s deadline (longer than
a replica's 2 s commit wait, so a busy replica answers before the client gives up on it), redirects followed at once,
a 100 ms pause after a round in which no replica answered, and a 10 s overall deadline (`--tracker-wait`). On failure
it reports what each replica last said — "refused", "timeout", or the replica's own `UNAVAILABLE` reason.

## How it is tested

Three layers, as everywhere in this project, plus checks that the tests can fail.

**`test-raft` — the core in a simulated world.** Several cores run against a simulated network with a delay per
message, loss, duplication (and so reordering), partitions, and replicas that crash and restart from what their
storage holds, under a simulated clock, all from one seed. After every step it checks Raft's safety properties:

| Property | How it is checked |
|---|---|
| election safety — at most one leader per term | every time a replica is leader, the term's leader is recorded; a second one fails the run |
| log matching | every 25 simulated ms and at the end, for every pair of live replicas: if they hold the same term at an index, everything up to it is identical |
| leader completeness | when a replica wins, its log must contain every entry any replica has counted as committed |
| state machine safety | every applied entry is recorded per index; a different entry applied at the same index anywhere fails the run |
| committed entries never change | an entry counted as committed by any replica must be the same wherever else it is counted |
| linearizability | at the end of each history, a checker for write-once registers, per name: every `OK`, `EXISTS` and `FILE` agrees with the first registration applied, and the winning write fits at one instant after every `MISSING` read was sent and before any answer showing the name taken was received |
| recovery | after the hostile phase the network heals and every replica restarts; within the calm phase a leader must commit in its own term and every replica apply as far |

It runs 60 seeded histories each of three mixes (three replicas with 5% loss, 5% duplication, crashes and
partitions; five replicas with the same; three replicas with 30% loss, 20% duplication, slow links and batches of a
few entries), a sweep that crashes the leader at 120 different moments of one replication, and directed cases built
by hand: Figure 8 reproduced step by step, the vote rules, a vote surviving a restart, conflicting and stale appends,
the follower's commit bound, ReadIndex refusing replies to calls sent before the read, check-quorum, and a disk that
refuses writes. The linearizability checker is itself tested against hand-built histories it must reject. The same
program tests the log file (torn tails, altered records, out-of-sequence records, a second vote in one term, damage
too far back, a write the file-size limit refuses), the wire format, and the client's leader finding against
scripted replicas. `RAFT_SEEDS=<n>` runs more histories.

**`test-replica` — the server code with real threads.** Three replicas in one process, connected by a function
that calls the other replica's `handle()` unless the test has cut the link: an election, redirects, eight clients
contending for names, the leader cut off from both followers (its write is never acknowledged, a new leader takes
over, both acknowledged writes survive the heal), a replica restarted from its storage, and a disk that fails. It is
also what `test-replica-tsan` runs under ThreadSanitizer.

**`scripts/e2e-raft.sh` — real processes.** Three `tracker` processes, a one-node ring and the real client: the
leader killed (a new one elected, writes continue), the killed replica restarted (it catches up), both followers
frozen with `SIGSTOP` (the lone leader never acknowledges and steps down), the leader frozen, a majority killed (the
client's upload fails and says the tracker could not be reached), every replica restarted (every acknowledged name
is still there), a writer running through a leader kill (every acknowledged name is found afterwards), and a replica
started with a different member list (refused). `TRACKER_CMD` runs it on the ThreadSanitizer build.

Every suite that used the single tracker — persistence, framing, hangup, deadline, transfer, smoke, edge — now runs
against the replicated code, because the single tracker *is* a group of one.
