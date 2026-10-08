# Storage and replication

How a chunk is kept on disk, written three times, read back verified, and restored after a death.
Code: [`src/store.{h,cpp}`](../src/store.h) (the on-disk store),
[`src/chunk.{h,cpp}`](../src/chunk.h) (the write coordinator, replica sets and verified reads) and
[`src/repair.{h,cpp}`](../src/repair.h) (re-replication). Results:
[`BENCHMARKS.md`](../BENCHMARKS.md) §6.

## Content addressing

A chunk's key is the SHA-1 of its bytes (D-014). Two replicas therefore cannot disagree — the value
at a key is by definition the bytes that hash to it — so the data plane has **no versions, no
vector clocks and no conflict resolution**; reads are self-verifying; and identical chunks in
different files are stored once. With no "latest version" for a read to miss, the quorum rule
`W + R > RF` does not apply: `W` buys durability and `R` availability, set independently.

## On disk (D-045)

`node … --store <dir>` keeps each chunk as one file, `<dir>/<first two hex characters>/<key>`. A
write goes to `<dir>/tmp/<key>.<n>`, is flushed with `fsync`, renamed over the final name, and the
directory is flushed; only then is it counted as stored. The rename is the commit point: a node
killed mid-write leaves a temporary file (deleted at start-up), never a final-named file holding
part of a chunk. The key is validated — exactly 40 lowercase hex characters — before any path is
built, so `GET ../../etc/passwd` never reaches the filesystem. An in-memory sorted index of keys
makes "which keys lie in this arc?" a range query.

## The replica set (D-048)

`RF = 3`: walk the ring clockwise from the key's owner and take each member whose **process** is not
already in the set, until there are three. With virtual nodes a member's successor often belongs to
the same process, and the Chord paper's "next two members" would then put two copies where one
crash destroys both. Processes are compared by address, which is what would count machines in a
real deployment.

## Writes: the owner coordinates (D-046)

1. The client looks up the key's owner and sends `PUT <key> <w> <length>` with the body.
2. The owner checks the bytes hash to the key, stores its own copy, and sends `STORE` to the other two
   replicas **in parallel**, one thread each, from one shared read-only buffer (D-052).
3. It answers `OK <copies>` as soon as `w` copies are confirmed, its own included — so `w = 2` waits
   for the faster replica, not the slower — and logs when the last copy lands: the window with
   fewer than three copies.

`w` travels in each request (default 2), so one ring under one failure can serve all three settings
side by side. If the owner cannot be reached, the client falls through to the next distinct process
in the replica set, which coordinates instead. A failed write is not rolled back: a copy that was
written is the right bytes under the right name, and repair completes it.

**At most 32 chunk bodies** are held in memory per process, counted by one atomic `fetch_add`; a
request beyond that gets `ERR busy` with its body drained, not buffered (D-052).

## Reads: one replica, verified, falling through (D-049)

`R = 1`. The client sends `GET` to the owner and hashes what comes back. If the connection fails, the
answer is `MISSING`, or the bytes do not hash to the key, it asks the member that answered its
lookup — the owner's predecessor, which has just proved it is alive — for its successor list, and
tries the next distinct processes in turn. That keeps reads working in the window after an owner
dies and before the ring evicts it. A read does not repair what it finds.

## Repair: the owner's job (D-050)

Each process runs a repair thread, once per period, separate from maintenance:

- **Owner check, level-triggered.** For each member, if its arc or its replica set changed since the
  last check, ask the other replicas `HAVE` (20 keys per request) and send `STORE` for what is
  missing. After a death the next replica becomes the owner of the dead process's keys — it already
  holds them — sees its arc change, and copies them to the new third process.
- **A full scan every 60 rounds:** the same check for every arc; a **scrub** that re-hashes every
  local chunk and replaces any that rotted with a verified copy read through the ring; and a
  **hand-off** of chunks this process holds but no longer owns, sent as a `PUT` so the owner places
  all three copies.

Nothing is deleted except a corrupt file, which is replaced. Storage only grows: there is no
deletion and no collector (D-023 designs owner-token unnaming and a mark-and-sweep collector; neither
is built).

## Measured (`BENCHMARKS.md` §6, commit `7d99fc0`)

| | Result |
|---|---|
| Load balance, 64 processes | busiest stores **1.52×** the mean with 8 virtual nodes per process, against 2.80× with one; coefficient of variation 0.20 against 0.57 (median of 20 address sets) |
| Simulation against reality | on 16 real processes, every store held exactly the number of chunks `sim-balance` predicted, at V = 1 and V = 8 |
| Re-replication | one of 16 processes killed, 300 chunks of 512 KiB: every chunk it held back on three correct live processes in a median **2.2 s** (1.8–2.9 s, 5 runs) |
| W with one replica dead | W = 2 acknowledged **300/300** writes; W = 3 **244/300** until the ring evicted it |
| W = 1's price | answers in 14 ms against 31 ms for W = 2, but each acknowledged chunk is on one disk for about 18 ms; killing the coordinator at the acknowledgement left no other copy in 20 of 20 trials |

- **V = 8** was chosen by a rule fixed before measuring — the smallest power of two whose coefficient
  of variation of stored copies is at most 0.25 at 16 and 64 processes (D-051). Each virtual node
  costs about half a percent of a core.
- **W = 2 and W = 3 cost the same while every replica is healthy** (31 vs 32 ms): on loopback both
  replicas are equally fast. The difference appears only when one is slow or dead — a prediction in
  D-015 that the measurement did not bear out, and recorded as such.
- Spreading repair over more processes did not make it faster here: every process shares one host's
  cores and disk, so the time is detection plus one repair round.

## Tested by

`test-chunk` (105 checks: path-traversal keys, zero bytes, exactly 512 KiB and one byte more, a body
in uneven pieces and a peer hanging up mid-body over a real socket, replica sets against a
brute-force oracle on 30 random rings, `W` against dead and slow replicas, reads past a corrupt
copy, repair after a death) and `scripts/e2e-chunk.sh` (6 processes × 4 members: placement on disk
against a Python oracle, corruption, two of three replicas killed, hostile requests, repair). See
[`testing.md`](testing.md).
