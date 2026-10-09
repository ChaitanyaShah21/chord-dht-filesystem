# Deletion: unnaming and collecting

Removing a file is two separate problems. **Unnaming** takes the name out of the tracker — what a user means by
"delete". **Collecting** frees the chunks no remaining file uses — what bounds storage. Content addressing makes the
second one dangerous: two files that share a chunk store it once, so a file's chunks cannot simply be deleted with it.
This guide covers the decisions and the alternatives considered for each; every decision links to its full entry in
[`ARCHITECTURE.md`](../ARCHITECTURE.md). The design was first written down in D-023.

> **Status:** decided; being built.

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
