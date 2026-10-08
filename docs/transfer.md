# Transfer and namespace

How a whole file goes into the ring and comes back, and how names map to files.
Code: [`src/transfer.{h,cpp}`](../src/transfer.h) (chunking, manifests, the worker pool, the part
file — no sockets), [`src/client.cpp`](../src/client.cpp) (the command line),
[`src/names.{h,cpp}`](../src/names.h) (the namespace — no sockets) and
[`src/tracker.cpp`](../src/tracker.cpp). Results: [`BENCHMARKS.md`](../BENCHMARKS.md) §5.

## The client

```
client --tracker <ip:port> --ring <ip:port>[,<ip:port>...] [options] <command>
  put <file> <name> | get <name> <file> | get-key <key> <file> | info <name> | resolve <name> | ls
  --workers <1..64>  --w <1..3>  --chunk-size <bytes>  --fanout <2..8192>  --stats <file>  --quiet
```

`--ring` lists one or more ring nodes; every lookup starts at the first that answers, and the client
moves down the list if a start node stops answering (D-057). Routing finds everything else. The
tracker never hands out node addresses — that would put liveness state on the control plane — so a
download by manifest key (`get-key`) works with the tracker down.

## Upload

1. Cut the file into 512 KiB chunks (D-016). `--workers N` threads (default 8) take chunk numbers
   from one atomic counter (`fetch_add`), read their chunk with `pread`, and store it through its
   owner with `w` confirmed copies (D-046, D-056).
2. Build the **manifest** — a text chunk listing every chunk's key and byte count (D-054) — and store
   it the same way. More than 8,192 chunks (4 GiB) gets a tree: index manifests listing manifests,
   at most four levels. The manifest holds no name, so identical files share one.
3. Only then **register the name** with the tracker. A name never points at a file that is not all
   there (invariant I9). The file's size and modification time are checked before and after; a file
   that changes during the upload is refused.

## Download (D-055)

1. `RESOLVE <name>` → the root manifest key.
2. Fetch the whole manifest tree, verifying each manifest against its key, and check every count,
   sum and level — **before anything is written to disk**.
3. Create a part file `<dest>.part-<pid>-<n>` in the destination's directory (`O_EXCL`, sized with
   `ftruncate`). The workers fetch chunks in parallel, each verified against its key and its length
   against the manifest (D-049), and write them at their offsets with `pwrite` — positional writes,
   so the workers share no file offset.
4. `fsync` the part file, `rename` it over the destination, `fsync` the directory. An existing file
   at the destination is replaced whole or not at all.
5. **On any failure the part file is deleted and the destination is untouched.** `SIGINT` and
   `SIGTERM` delete it too, from a signal handler (`unlink` is async-signal-safe). Only `SIGKILL` or a
   crash can leave a partial file, and its name ends in `.part-`.

A failed chunk is retried twice, after 250 ms and 500 ms — about half and one stabilisation period,
so a freshly dead node can be evicted in between. A chunk that fails three times stops every worker
and fails the whole transfer: a file with one missing chunk is not a file.

The coursework client this replaced preallocated the destination at full size before fetching a
byte, so a failed download left a full-size file of zeros under the real name (defects R2, R2b in
[`failures.md`](failures.md)).

## The tracker: a namespace and nothing else (D-017, D-058, D-059)

`tracker <port> [--dir <dir>] [--ip <ip>] [--read-deadline <ms>]` holds `name → manifest key`.
Four messages — `REGISTER`, `RESOLVE`, `LIST`, `PING` (see [`protocol.md`](protocol.md)). It never
sees a chunk or a node address: a chunk's location is computed from its hash, so there is nothing for
it to track, and its state is O(files) rather than O(chunks).

- **A name is written once.** Registering a taken name with the same key succeeds and changes nothing
  (so a client that lost its `OK` can retry); with a different key it is refused with
  `EXISTS <key>`. With no access control, a replaceable name would be one anyone could replace.
- **An effect log, flushed before `OK`.** Each new name is appended to `names_<port>.log` as a
  self-checking record and `fdatasync`ed under the namespace lock before the tracker answers, so an
  acknowledged name survives a crash. At start-up the log is replayed through the same function the
  live path uses; a torn final record is cut off and a damaged one skipped. An ordered list of
  deterministic effects is exactly what a Raft-replicated tracker would replicate (not built).
- **Read deadlines.** Like a node, the tracker gives every request line 10 s to arrive whole,
  enforced against the time remaining, so a peer trickling bytes cannot hold a thread (D-061).

## Measured (`BENCHMARKS.md` §5, commit `2e7c445`)

16 processes × 8 virtual nodes, a fresh ring and tracker per repetition, every one of 116 transfers
compared by SHA-1, medians of 5. Download time is the client's own time to every chunk verified and
written, which is where the old system's clock stopped.

| 512 KiB chunks | 1 MB | 10 MB | 100 MB |
|---|---:|---:|---:|
| the coursework single-seeder system, re-run the same day at `e381179` | 17.9 MB/s | 54.7 | 70.1 |
| the ring, 1 worker | 73.3 | 123.9 | 103.2 |
| **the ring, 8 workers** | **101.8** | **313.2** | **378.2** |

- **5.4× the old system at 100 MB, and 3.7× from parallelism alone** (1 worker against 8 on the same
  ring) — the second comparison is the one that changes a single variable.
- With process start, flush and rename: 293 MB/s at 100 MB (the flush and rename take 67 ms).
- Per chunk at 8 workers: p50 9.4 ms, p99 31.4 ms. Uploads: 77 MB/s, every chunk on two flushed copies.
- **Workers:** 102 / 183 / 299 / **377** / 363 / 324 MB/s at 1 / 2 / 4 / 8 / 16 / 32 — the rule fixed
  before measuring chose 8, the number of cores; past it workers queue for CPU.
- **Chunk size** (on a build with the limit raised, D-063): 100 MB/s at 64 KiB to 536 MB/s at 8 MiB —
  *not* flat, against the prediction in D-016. 512 KiB stays: the comparison is pinned to it, and what
  large chunks cost (memory per in-flight chunk, re-fetch size, deduplication granularity) is what
  loopback cannot show.
- **Deduplication**, counted at the stores (D-062): the same bytes under a second name add nothing;
  1 KiB changed in place adds 2 of 201; one byte inserted near the start adds 200; twenty consecutive
  commits of this repository as tar files share **1 chunk of 97** — fixed-size chunks never realign
  after an insertion. Content-defined chunking is the known fix, not built.
- **The tracker:** 77 bytes of log and about 230 bytes of memory per name; 197–254 registrations a
  second, one flush each; 100,000 names replayed and listening in 0.39 s.

## Tested by

`test-transfer` (87 checks: 26 malformed manifests refused one rule at a time, the largest possible
manifest, trees, the pool, whole transfers against an in-memory ring that loses, delays and lies),
`test-names` (39: torn and altered records, a disk that refuses writes, eight threads racing for one
name), `scripts/e2e-transfer.sh` (8 processes: a node killed mid-download, a corrupt copy, a chunk
lost everywhere, the client killed with `SIGKILL` and `SIGTERM`, identical uploads at once, the
tracker down), `scripts/e2e-edge.sh` (0 bytes to 3 MB across the chunk boundary),
`scripts/e2e-persistence.sh`, `e2e-framing.sh`, `e2e-hangup.sh` and `e2e-deadline.sh`. See
[`testing.md`](testing.md).
