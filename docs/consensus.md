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
