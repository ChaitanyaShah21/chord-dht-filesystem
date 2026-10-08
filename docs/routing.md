# Routing

How a key is mapped to the node that owns it, and how a client finds that node.
Code: [`src/chord.{h,cpp}`](../src/chord.h) (the arithmetic, as pure functions) and
[`src/lookup.{h,cpp}`](../src/lookup.h) (the originator's loop). Results:
[`BENCHMARKS.md`](../BENCHMARKS.md) §2.

## Identifiers and keys

- A **key** is the full 160-bit SHA-1 of a chunk's bytes, 40 hex characters. It is what a chunk is
  stored under and what a reader re-hashes to verify it (D-014).
- A **routing identifier** is the top 64 bits of a SHA-1, held in a `uint64_t`. A node's identifier
  is `SHA-1("ip:port")` (or `"ip:port#i"` for its *i*-th virtual node, D-047); a key's identifier
  is its first 16 hex characters. The ring is the 64-bit machine word: unsigned arithmetic wraps
  modulo 2⁶⁴ by definition, so `n + (1ULL << i)` *is* ring arithmetic with no hand-written carry
  code underneath the hop-count measurement (D-019). Truncation applies to placement only, never
  to identity (invariant I7).
- "Am I alone?" is answered by comparing **addresses**, never identifiers alone, because the
  interval `(n, n]` is the whole ring (invariant I8).

## Who owns a key

The owner of identifier `k` is the first ring member at or after `k` clockwise — its *successor*.
A member answers a lookup from one test: is `k` in `(me, successor]`? If so, `OWNER <successor>`;
otherwise `NEXT <closest preceding finger>` (`route_step`). It never also claims keys in
`(predecessor, me]`: that would make the predecessor correctness-critical to save a hop on about
one lookup in N (D-021).

**Only the successor pointer must be right.** Fingers, the successor list and the predecessor can all
be stale at the cost of extra hops, never of a wrong answer — the separation of correctness from
performance that the whole design rests on.

## The finger table

Finger *i* of member `n` is the owner of `n + 2^i`, for *i* = 0…63. Of 64 fingers only about
log₂ N have distinct owners. They are refreshed one lookup per maintenance round, and each answer
fills every later finger whose start it also covers, so a whole table is refreshed in about
log₂ N rounds rather than 64 (D-034).

## The lookup: iterative, driven by the originator

The client (or a node repairing its fingers) asks a start node `FIND_SUCCESSOR k`, follows each
`NEXT` itself, and stops at `OWNER` (D-012). Each node answers one question and returns at once.

- **Why iterative:** the originator counts its own hops, so the hop-count measurement sits outside
  the system it measures; a failure names exactly which node failed; and no node holds a blocked
  thread for the duration of someone else's lookup. The cost is two network traversals per hop
  instead of one — nearly free on loopback, which is where every number here was measured.
- **Every reply is checked** (D-024): a `NEXT` must name a node strictly inside `(current, k)`, and
  an `OWNER` reply must satisfy `k ∈ (current, owner]`. A correct node passes both even with stale
  tables, so a violation names a faulty node, and since the distance to `k` strictly shrinks, a
  lookup cannot cycle.
- **At most 65 requests** (`MAX_REQUESTS`). Derived, not chosen: with correct fingers each `NEXT`
  more than halves the remaining distance in a 2⁶⁴ space; `test-lookup` builds a ring that needs
  exactly 65. Kept during churn as well (D-037).
- **A fresh connection per request**, with a 5 s deadline set before `connect` (D-024, D-025). One
  connection gives one unambiguous signal per node: refused (nothing listening — proof), timed out
  (suspicion), or closed.
- **Recovery** (`lookup_with_recovery`, D-036): if the node a lookup was sent to cannot be reached,
  the originator sends `SUSPECT <ip> <port>` to the node that named it — which probes the suspect
  and evicts it only on its own evidence — and retries from the start. This is how client traffic
  speeds up failure detection under iterative routing.
- **The transport is passed in.** The loop never opens a socket; tests hand it rings built in
  process, including rings that lie.

## What a hop is

The reported hop count is the number of `NEXT` replies followed — fingers followed, the Chord
paper's path length — which is **requests − 1**, because the originator's first request only
reaches the start node (D-026). Both counts are kept in the raw data.

## Measured

On fixed rings with correct tables (D-020), 380,928 lookups, every owner checked against an
independent oracle (`BENCHMARKS.md` §2, commit `e042857`):

| Ring | How | Mean hops | ½ log₂ N | 99th percentile |
|---:|---|---:|---:|---:|
| 2,048 | real `node` processes over TCP | 5.38 | 5.5 | 9 |
| 1,048,576 | `sim-hops`: the same routing code in one process | 9.87 | 10.0 | 15 |

Real processes stop at 2,048 because of measured memory (about 0.95 MiB proportional set size per
node); `sim-hops` replays the same `parse_members`, `build_fingers`, `route_step` and `lookup()`
with a function call for a socket, and reproduced all 135,168 real lookups exactly, owner and path
(D-027). The mean sits 0.10–0.19 hops below ½ log₂ N from 32 nodes up; that offset is measured and
not explained.

## Tested by

`test-chord` (127 checks: arithmetic at the wrap-around, hand-built rings, an independent model of
the finger rule), `test-lookup` (70: correct, lying and broken rings), `scripts/e2e-node.sh` (a real
8-node fixed ring; owners and hop counts against independent models) and `scripts/e2e-chord.sh` (a
real 16-node ring: paths against a model, one node killed, one frozen, each failure reported as the
right kind and naming the right node). See [`testing.md`](testing.md).
