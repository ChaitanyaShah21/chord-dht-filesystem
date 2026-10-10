# Deletion: unnaming and collecting

Removing a file is two separate problems. **Unnaming** takes the name out of the tracker — what a user means by
"delete". **Collecting** frees the chunks no remaining file uses — what bounds storage. Content addressing makes the
second one dangerous: two files that share a chunk store it once, so a file's chunks cannot simply be deleted with it.
This guide covers the decisions and the alternatives considered for each; every decision links to its full entry in
[`ARCHITECTURE.md`](../ARCHITECTURE.md). The design was first written down in D-023.

> **Status:** built, tested and measured ([`BENCHMARKS.md`](../BENCHMARKS.md) §9): a collection freed exactly what only
> the deleted files used — 62,928,030 bytes on disk in each of 3 runs, every remaining file whole — and drops 5,634
> copies a second.

## The decisions

### The owner's secret — [D-089](../ARCHITECTURE.md#d-089--a-name-is-deleted-by-its-owners-secret-the-uploader-chooses-it-the-tracker-keeps-only-its-hash-and-the-deletion-is-a-raft-entry)

```
REGISTER <name> <manifest-key> <owner-hash>     the uploader keeps the secret; the tracker keeps SHA-1 of it
UNREGISTER <name> <secret>                      -> OK | MISSING | DENIED
```

`client put` prints a random secret once; `client delete <name>` sends it back. The leader hashes it and the deletion
is a Raft entry like a registration, so every replica removes the name at the same point in the log. No secret is ever
in the log. A name registered without an owner (the old three-word form) can never be deleted.

| Alternative | Why not |
|---|---|
| **A token the tracker returns** | a retried registration must answer `OK` — and would hand the token to anyone who knows the name and key |
| **User accounts** | removed by D-004; one secret per name is enough |
| **Anyone may delete** | a vandalism tool |
| **Storing the secret** | every owner's secret in every replica's log file |

### The collector — [D-090](../ARCHITECTURE.md#d-090--a-separate-collector-marks-every-chunk-reachable-from-a-live-name-lists-each-nodes-chunks-and-asks-each-node-to-drop-the-rest)

`gc` lists every name and manifest key from the tracker group, walks every manifest tree through the ring with
verified reads, and so knows every live chunk. It finds every node by walking the ring, lists each node's chunks, and
sends back the ones nothing uses in `DROP` requests. If any tree cannot be read, nothing is dropped.

| Alternative | Why not |
|---|---|
| **The Raft leader collects** | the control plane deleting in the data plane; a leader change stops it half way |
| **Every node marks for itself** | the whole mark done once per node |
| **Nodes drop whatever is not in a set they are sent** | a forged or truncated set deletes everything |

### The race with uploads — [D-091](../ARCHITECTURE.md#d-091--a-chunk-is-dropped-only-after-a-grace-period-without-being-written-kept-by-each-nodes-monotonic-clock-a-restart-counts-as-a-write)

An upload stores its chunks first and registers its name last, so for a while its chunks are reachable from nothing.
A node therefore drops a chunk only if nothing has written it for a grace period G (default 2 h) — and writing a chunk
it already holds counts, so an old orphan reused by a new upload is renewed. Times are kept by the node's monotonic
clock, in memory; a restart counts as a write of everything. A client registers only within U (1 h) of starting; the
collector stops D (30 min) after it began, and refuses a node whose grace is below U + 10 s + D. Together these mean no
chunk of an acknowledged upload is ever dropped.

| Alternative | Why not |
|---|---|
| **Generations with upload leases** | two more consensus writes per upload, and a crashed client's lease still needs a timeout |
| **A pin per chunk** | the same timeout, plus pin state and a release per chunk |
| **Stopping uploads during a collection** | the tracker cannot see an upload until it registers |

### Who may drop — [D-092](../ARCHITECTURE.md#d-092--drop-carries-a-key-shared-by-the-nodes-and-the-collector-compared-in-constant-time)

`DROP` is the first request on the data plane that destroys anything. A node started with `--gc-key <file>` accepts it
only with the same key, compared in constant time; without one it refuses every `DROP`.

| Alternative | Why not |
|---|---|
| **No check** | anyone who can reach a node could delete old chunks, live ones included |
| **A signed request with a nonce** | stops replay of a sniffed request; the next step, not built |
| **The node checks reachability** | the whole mark on every node |

## How it works

**Deleting a name.**

```bash
./client --tracker $T --ring $R put movie.mp4 movie      # ... total_ms 812.4 token 3f9a…c21e  -- keep the token
./client --tracker $T delete movie 3f9a…c21e             # delete movie ok
./client --tracker $T delete movie --token-file tok.txt  # the same, keeping the token out of `ps`
```

The client sends `REGISTER movie <key> <SHA-1 of the token>`; deleting sends `UNREGISTER movie <token>`, which the
leader turns into the log entry `UNREGISTER movie <SHA-1 of the token>`. Uploading the same content under the same
name again succeeds (the name already means that file) but prints `token -`: the first upload's token is the one that
owns it. A name registered with the original three-word `REGISTER` has no owner and can never be deleted.

**Collecting.**

```bash
./gc --tracker $T --ring $R --gc-key gc.key            # nodes started with --gc-key gc.key
gc ok names 3 roots 2 live 16 nodes 4 skipped 0 held 63 garbage 15 dropped 15 bytes 788034 mark_ms 21.4 sweep_ms 48.0
```

| Step | Requests | What can go wrong, and what happens |
|---|---|---|
| list every name | `ENTRIES [<after>]` to the tracker group's leader, a page at a time | no leader: the collection stops, nothing dropped |
| mark | the client's verified reads of every manifest tree (`resolve_manifest`) | a tree cannot be read: **the collection stops before any drop**, naming the file |
| find every node | `NEIGHBOURS`, jumping a successor list at a time round the ring | a member does not answer: the nodes found so far are swept; the rest keep their garbage |
| list each node | `KEYS <after>`: 20 keys a page, and the node's grace | grace below U + W + D: that node is **not swept** |
| sweep | `DROP <key> <k1> … <k20>` → `DROPPED <bits> <bytes>` | past the deadline D: no more `DROP`s; the rest wait for the next collection |

**On the node.** `store_drop` checks the chunk's last write time against the grace and, if it is old enough, removes
it from the index and unlinks the file, all under the store's lock; `store_put` of a chunk already held renews it under
the same lock. Write times live in memory (`ChunkStore::touched`); a chunk not written since the process started
counts as written at the start; times older than the grace are forgotten at the start of each listing, because such a
chunk is old either way.

**What it never does.** Delete a chunk any live name reaches; delete a chunk written (or written again) within the
grace; delete anything when the mark is incomplete. **What it may do, by design:** leave garbage — on a node that was
down or not found, on a node whose grace is too short for the collector's settings, and wherever repair copied a chunk
back from a replica not yet swept. Each later collection takes more of it.

## How it is tested

| Layer | What | Where |
|---|---|---|
| In process | owners: every combination of key and owner hash, secrets of the wrong shape, the hash sent instead of the secret, ownerless names, a log of registrations and deletions applied twice | `test-names` |
| In process | the store: too young, old enough, renewed by a duplicate write, a restart, forgetting old times; **2,000 rounds of a write racing a drop**, the index, the disk and the byte count agreeing after every one | `test-gc` |
| In process | `KEYS` paging and `DROP` through the node's own handler: no key, the wrong key, a key one character short, a bad key or 21 keys refused whole; the constant-time comparison | `test-gc` |
| In process | whole collections on stores in temporary directories: **a chunk — and a manifest — shared by two files survives deleting one**; an unreadable tree stops everything; a dry run; young garbage; a node with too short a grace; the deadline; the wrong key; **an upload in flight and one reusing old orphans both survive** | `test-gc` |
| Real processes | the same, from outside: chunk files counted in the stores' directories, keys computed from the test's own files; tokens never in any log or Raft log; the bytes the stores lost equal the bytes the collector reports; a restarted node keeps everything (judged by file times) | `e2e-gc.sh` |
