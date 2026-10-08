# Consensus: the replicated tracker

The tracker holds one thing — `name → manifest key` (D-017, D-058) — and until this work it was one process: the
single point of failure left in the system. This guide covers replacing it with a **Raft group**: the decisions
and the alternatives considered for each, how the code is laid out, how it is tested, and what it measured. Every
decision links to its full entry in [`ARCHITECTURE.md`](../ARCHITECTURE.md).

> **Status:** being built. The decisions below were made before any of its code was written; sections on the
> implementation, tests and numbers are added as each part is finished.

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
