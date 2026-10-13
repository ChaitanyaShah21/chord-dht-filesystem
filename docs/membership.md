# Membership and stabilisation

How nodes join, how the ring repairs itself when they die or freeze, and how that is measured.
Code: [`src/stabilize.{h,cpp}`](../src/stabilize.h) — a member's routing state and every change to
it, with the network passed in — and [`src/node.cpp`](../src/node.cpp) (sockets, threads, the
command line). Results: [`BENCHMARKS.md`](../BENCHMARKS.md) §4 and §4a.

## A process is several ring members

`node <ip> <port> --vnodes V` (default 8) is V ring members — `SHA-1("ip:port")` and
`SHA-1("ip:port#i")` — sharing one listening socket. Each member has its own predecessor, successor
list, fingers and failure-detector state; one maintenance thread runs every member's round in turn.
Requests to a member are prefixed `TO <identifier>` (D-047). Evidence is split by what it is about:
a **refused connection** says nothing listens at that address, so it evicts every member of that
process; a **timeout** is about one exchange with one member. A member never strikes or evicts a
member of its own process — with one exception, below.

## Starting

| Command | Starts as | Maintained |
|---|---|---|
| `node <ip> <port> <membership-file>` | the fixed ring, wired from a file that no node keeps (D-020) | no — the hop-count benchmark's ring |
| `node <ip> <port> --create` | a ring of one | yes |
| `node <ip> <port> --join <ip:port>` | looks up its own identifier through a known node; the owner becomes its successor | yes |
| `node <ip> <port> --wired <file>` | the fixed-ring state, then maintained — a known correct start for the reconvergence benchmark | yes |

A joining node **listens only once it has a successor** (D-029). A node restarted at its old address
before the ring noticed its death would otherwise answer while still believing it was alone, and
`(n, n]` would make it claim every key; not listening means its old entry is refused, which is proof
of death, so the ring evicts it and the join is retried. A joiner whose own identifier is already
owned by another address refuses to start (D-028). A newcomer copies its successor's successor list
at join (D-040).

## The maintenance round

Every period T (default 1000 ms, a fixed delay, first round at a random offset), for each member:

1. **Stabilise** (D-031, D-043): ask the successor `NEIGHBOURS` — its predecessor and successor list
   in one reply, from one snapshot. If its predecessor lies between this member and the successor,
   **ping it before adopting it** (D-035), then ask the new successor too, and keep going until
   nothing closer is named: the pointer reaches its fixed point within one round instead of
   walking one node per period. Take the successor's list, shifted by one, as this member's list
   (8 entries, D-032), and send `NOTIFY`.
2. **Check the predecessor** with a `PING`.
3. **Repair one finger** with one lookup (D-034).

## Failure handling (D-013, D-035)

- Every call to another node goes through one function that reports the outcome to the failure
  detector, so no call site can forget to.
- **A refusal evicts at once** — nothing is listening, which is proof. **Anything else** that is not a
  valid reply — a timeout, a closed connection, an `ERR`, a garbled line — is a **strike**, and two
  consecutive strikes against a peer evict it. Strikes count against every peer a member holds,
  fingers included.
- Eviction removes the peer from the successor list, predecessor and fingers. If the successor was
  evicted, stabilise moves to the next entry in the same round.
- A `NOTIFY` from an unexpected node triggers a ping of the current predecessor.
- **Nothing is adopted unverified.** A neighbour's stale pointer cannot re-introduce a node that was
  just evicted.
- An evicted node is never told; if it was only slow, it re-inserts itself through its own next
  stabilise, exactly as it joined.
- Clients add evidence: a client that reaches a dead node sends `SUSPECT` to the node that named it,
  which probes and evicts only on its own refusal (D-036). Under load, this is what makes finger
  repair fast.

**Leaving is crashing** (D-033): a node stops; its next connection is refused; its neighbours repair
the ring as they repair any death. One recovery path, exercised every time anything stops.

**When nothing refuses** (D-096): a refusal needs a live kernel at the dead address. A container's address — or a
whole host's — disappears with it, so calls to it time out instead, and the dead node is evicted by two strikes, as
if it were frozen. Measured in `BENCHMARKS.md` §12: of the evictions of a killed container's members, 8 were by
refusal (before Docker removed its endpoint) and 10 by strikes.

## A node that loses everyone it knew

If eviction leaves a member knowing no other node, it does not fall back to itself — `(n, n]` would
make it claim the whole ring, and the in-process churn tests showed a later joiner routed through
such a node forming a second ring that stabilisation can never merge. It becomes **orphaned**
(D-040): it answers `ERR orphaned` to everything the ring routes or checks liveness with, and each
round tries to rejoin through one of up to 32 addresses it remembers. This chooses consistency over
availability: a node cannot tell "everyone else died" from "I am cut off".

**A reply of `ERR orphaned` evicts that member at once, from any node, its own siblings included**
(D-067). Found by the transfer benchmark: a sibling that had fallen back onto an orphan kept it as
its successor for ever (siblings never strike each other), and every rejoin lookup ended at that
sibling, which named the orphan itself — a livelock. The orphan's own statement is proof, like a
refusal.

## Concurrency

One mutex per member's routing state, held only to compute a reply or copy state, **never across a
network call** (D-030). A frozen peer can delay one maintenance round but never a lookup. Because
state may change while a call is in flight, every result is applied only if the state it was based
on is still current. The in-process tests run every node on one thread, so a call made while
holding the lock would deadlock the test rather than pass it; a ThreadSanitizer build of the node is
run through the churn suite.

## The period and the deadline

Both came off measured curves by rules written before measuring (D-039, restated in D-041 and D-042
when the first versions failed — both failures are kept): **T = 1000 ms**, the longest period at
which at least 9 of 10 runs repair the ring within 1 s; **D = 200 ms** for node-to-node calls, the
smallest candidate at least ten times the 99.9th-percentile exchange and above the slowest one seen.

## Measured (`BENCHMARKS.md` §4, commit `b171554`)

"Reconverged" is decided by the test harness, never by the nodes (D-038): it computes what every
survivor should hold from the surviving addresses alone and polls until reality matches.

| One node killed, 64 nodes, T = 1 s | Median |
|---|---:|
| ring correct — every successor and predecessor right | **0.63 s** (0.17–1.03, 9 of 10 runs under 1 s) |
| successor lists correct | 3.48 s |
| every finger table correct | 5.32 s |

- Repair time does not grow with N: 0.42–0.56 s from 16 to 128 nodes.
- With k random nodes killed the ring waits for the slowest of k predecessors, close to the
  predicted T · 0.5^(1/k).
- 8 adjacent deaths (a whole successor list): 1.49 s before D-043, **0.36 s** after.
- Under lookup load, finger repair falls from 3.99 s to 0.29 s; of 28,790 lookups during and after
  kills none failed.
- Idle cost: 0.44 % of a core per node at T = 1 s; about half a percent per virtual node.
- **Stated limit:** at 256 nodes on one 8-core machine, 3 of 55 runs wrongly evicted live nodes.
  Each was a 5–9 s machine-wide stall in the kernel, during which no process on the host answered;
  the detector evicted peers silent that long, as specified (§4a).

## Tested by

`test-stabilize` (97 checks: every round order of two joins between the same pair, kills between a
notify and a stabilise, frozen nodes, fast restarts, collisions, and 200 random churn histories
judged by an oracle), `test-chunk`'s virtual-node and orphaned-sibling cases, and
`scripts/e2e-churn.sh` (22 checks on real processes, also under ThreadSanitizer). See
[`testing.md`](testing.md).
