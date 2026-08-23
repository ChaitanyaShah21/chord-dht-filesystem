# Glossary — chord-dht-filesystem

Every term defined once — plain language first, then technically. Grows as we go.
**If a term is used anywhere in this project and is not here, that is a bug.**

The plain-language version is not padding. "Never give a bare definition" is standing advice
from people who got selected: start with the problem the concept solves, then the need, then the
method, then the advantages and disadvantages. The two-part entries below are that answer in
note form.

---

### P2P (peer-to-peer)

**Plain:** a university department with no library room. Every student keeps their books on
their own desk, and there is a noticeboard in the corridor where people pin index cards saying
what they have and which room they are in. Nobody carries a book to a central place.

**Technical:** a distributed architecture in which every participant is simultaneously a client
and a server, and the bulk data does not pass through any central node.

**Why it exists:** a central server's outbound bandwidth is a hard ceiling and a single point of
failure. In a P2P system, each new participant brings capacity as well as demand, so serving
capacity grows with popularity instead of collapsing under it.

**Trade-off:** you lose central control — no single place to enforce consistency, authorisation
or availability, and every one of those has to be rebuilt as a distributed protocol.

**In this project:** `client.cpp` is both a downloader and a server; the `tracker` only ever
holds metadata and never sees a file byte.

---

### Tracker

**Plain:** the noticeboard. It holds index cards, never books.

**Technical:** a process holding metadata about the system — which files exist, how they are
split into pieces, the hash of each piece, and which peers currently claim to hold them. It
performs **peer discovery** and nothing else.

**Why it exists:** peers need some way to find each other before any direct connection is
possible. Something has to answer "who has this?".

**Trade-off:** it is a single point of failure and a central authority in a system whose whole
point is not having one. This is exactly the tension that Phase 2's Raft replication addresses,
and that the Chord ring reduces by owning placement itself.

**In this project:** `tracker.cpp`. Currently one process. See fork **F2** for what it is
allowed to know.

---

### Piece / chunk

**Plain:** a chapter of a book. You can photocopy chapter 7 from one person and chapter 8 from
someone else at the same time, and check each photocopy came out right before binding it in.

**Technical:** a fixed-size contiguous byte range of a file — 512 KB here, with the final piece
shorter when the size does not divide evenly. Each piece has its own hash and is transferred and
verified independently.

**Why it exists:** three separate reasons, and being able to name all three is the good answer.
(1) **Parallelism** — different pieces can come from different peers simultaneously.
(2) **Cheap recovery** — a corrupted or interrupted piece costs one piece to redo, not the whole
file. (3) **Partial sharing** — a peer that has 30% of a file can already serve that 30%.

**Trade-off:** smaller pieces parallelise better and recover more cheaply but multiply the
metadata and the number of lookups; larger pieces mean fewer lookups but one slow peer can
dominate the transfer. That is fork **F5**, and it gets settled by a measured curve rather than
by choosing a fashionable number.

**In this project:** `PIECE_SIZE` at `client.cpp:26`.

---

### SHA-1 (Secure Hash Algorithm 1)

**Plain:** a fingerprint of a chunk of data. Twenty bytes that change completely if any single
bit of the input changes, so you can tell whether a copy arrived intact without comparing it to
the original.

**Technical:** a cryptographic hash function producing a 160-bit digest. Processes the input in
512-bit blocks through a Merkle–Damgård construction, 80 rounds over five 32-bit registers, with
the message length appended as padding.

**Why it exists here:** a peer you did not choose sent you bytes over a link you do not control.
The hash is what lets you detect corruption — accidental or deliberate — before writing to disk.

**Trade-off:** **SHA-1 is broken for collision resistance** — the SHAttered attack (2017)
produced two different documents with the same digest. That does not matter here, because this
defends against *corruption*, not against an adversary who gets to choose both inputs. Being
able to state that distinction is the difference between "I used SHA-1" and understanding why it
is still acceptable in this position. It would not be acceptable for content addressing in a
system with untrusted publishers.

**In this project:** `sha1.h`, a header-only wrapper over OpenSSL's `SHA1()` that hex-encodes
the 20-byte digest into a 40-character string.

---

### Framing

**Plain:** water from a tap, not parcels in the post. TCP guarantees every drop arrives in
order, and tells you nothing about where one bucketful ends and the next begins. Drawing those
boundaries is your job.

**Technical:** the mechanism by which a receiver determines message boundaries in a byte stream.
Two standard approaches: a **delimiter** (read until a sentinel byte) or a **length prefix**
(read a header giving the byte count, then exactly that many bytes).

**Why it exists:** TCP (Transmission Control Protocol) is a stream protocol. One `send` may
arrive as three `recv`s, or three `send`s may arrive as one.

**Trade-off:** delimiters are simple and human-readable but **impossible** when the payload can
contain the delimiter byte — which binary file data always can. Length prefixes handle arbitrary
bytes but require the sender to know the length up front.

**In this project:** both. Client↔tracker is newline-delimited (`tracker.cpp:365`) with a
`partial` buffer that survives across `recv` calls. Client↔client is length-prefixed
(`PIECE <n>\n` then n raw bytes, `client.cpp:362`) precisely because a `.iso` contains newline
bytes by chance.

---

### Short read / partial read

**Plain:** you ask the tap for a litre and get 400 ml, because that is what has arrived so far.
Not an error. Ask again for the rest.

**Technical:** `recv()` and `read()` return the number of bytes **actually transferred**, which
may be fewer than requested. The correct pattern is always a loop that tracks a running total,
offsets the destination pointer by it, and requests the remainder.

**Why it matters:** code that calls `recv` once and assumes it got everything works on loopback
with small messages — which is exactly the condition under which it gets tested — and fails on a
real network with large ones.

**Trade-off:** none. The loop is strictly correct; omitting it is strictly a bug.

**In this project:** done correctly at `client.cpp:530-534` (the piece loop) and
`client.cpp:158-164` (`send_all`).

---

### Mutex (mutual exclusion lock)

**Plain:** the key to a single-occupancy room. Whoever holds it may go in; everyone else waits
at the door. Only one person can be inside at a time.

**Technical:** a synchronisation primitive with exactly one owner at a time. `lock()` blocks
until acquisition; `unlock()` releases. In C++, `std::lock_guard<std::mutex>` wraps this in
**RAII** so the lock is released when the guard goes out of scope — including on an exception.

**Why it exists:** without it, two threads reading and writing the same memory produce a **data
race**, which in C++ is undefined behaviour, not merely a wrong answer.

**Trade-off — and this is the part that gets probed:** a mutex only protects the invariant you
wrap it around. Making each individual operation atomic does **not** make a *sequence* of
operations atomic. Coarse locks are easy to reason about and limit concurrency; fine-grained
locks are faster and require a stated **lock ordering** to stay deadlock-free.

**In this project:** the tracker's `state_mtx` is deliberately coarse — one lock over the whole
`handle_command` body. The client's `sock_mtx` is the cautionary example: it makes each `send`
atomic, but the invariant that mattered was *send-then-receive*, which no lock protected. That
is defect **D2**.

---

### RAII (Resource Acquisition Is Initialization)

**Plain:** a hotel keycard that stops working the moment you step out of the room, automatically
— you cannot forget to hand it back.

**Technical:** the C++ idiom in which a resource is owned by an object, acquired in its
constructor and released in its destructor. Because destructors run deterministically when scope
is left — including during exception unwinding — the release cannot be skipped.

**Why it exists:** manual paired calls (`lock`/`unlock`, `open`/`close`, `new`/`delete`) leak
whenever an early `return`, `break` or exception jumps over the second half.

**Trade-off:** the lifetime is tied to scope, so releasing early requires an explicit inner scope
or a different type.

**In this project:** `lock_guard` throughout. **Counter-example worth noticing:** the raw
`open()`/`close()` file-descriptor pairs in `client.cpp` are *not* RAII — every early return has
to remember `close(fd)`, and `handle_peer_connection` has several such paths. That is a leak
waiting to happen, and it is the honest answer to "where would you use RAII that you currently
do not?".

---

### Detached thread

**Plain:** sending someone off on an errand and explicitly agreeing you will never wait for them
to come back. They lock up after themselves when done.

**Technical:** `std::thread::detach()` severs the association between the `std::thread` object
and the underlying thread of execution. The thread runs to completion independently and releases
its own resources. It can no longer be `join()`ed.

**Why it exists:** a connection handler's natural lifetime is the connection, not the parent's
scope. Without detaching, `std::thread`'s destructor calls `std::terminate` on a still-joinable
thread.

**Trade-off:** you give up ever knowing when it finished, so there is **no clean shutdown path**
— at exit, detached threads are killed wherever they happen to be, possibly mid-write. The
alternative is keeping a container of joinable threads plus a shutdown flag they check.

**In this project:** `thread(client_handler, cs).detach()` at `tracker.cpp:466`, one per client.
The download workers are the contrast — they are **joined** at `client.cpp:582`, because the
code genuinely needs to know when all pieces are done.

---

### `std::atomic`

**Plain:** a counter that many people can increment at once without a lock, because the
increment is a single indivisible machine instruction rather than the read-add-write sequence it
looks like in source.

**Technical:** a template providing operations guaranteed to be indivisible with respect to
other threads, plus defined ordering constraints on surrounding memory operations.
`fetch_add(1)` atomically increments and returns the previous value.

**Why it exists:** `x++` on a plain `int` is three steps — load, add, store — and two threads
interleaving them lose an increment. It is also a data race, therefore undefined behaviour.

**Trade-off:** atomics make each *operation* indivisible, not each *sequence*. They are cheaper
than a mutex for single-word updates and do not compose — two atomic operations are not jointly
atomic. Also **`std::atomic` is not copyable**, which propagates: any struct containing one
loses its implicit copy constructor.

**In this project:** `atomic<size_t> next_piece` at `client.cpp:552` is the work queue — each
worker calls `fetch_add(1)` to claim the next piece index, so no two workers take the same
piece, with no lock. That is the correct use. Its non-copyability is also **defect B4**:
`DownloadTask` holds an `atomic<size_t>`, which deleted its copy constructor, which broke
`vector::push_back` — and the fix was to write an explicit move constructor.

---

### DHT (distributed hash table)

**Plain:** replacing the corridor noticeboard with a rule everyone knows. Instead of one board
listing where everything is, the rule says "index cards for titles starting A–D live with the
person in room 101, E–H with room 102…" — so you compute where to look instead of asking a
central index. And when someone moves out, only their share of the cards has to be handed on.

**Technical:** a decentralised key-value store in which the key space is partitioned across
participating nodes by a hash function, and each node maintains routing state sufficient to
forward a lookup toward the node responsible for any given key — typically in O(log N) hops.

**Why it exists:** it removes the central index entirely. No single node knows the whole map,
and no single node's failure loses it.

**Trade-off:** a lookup is now a multi-hop network operation rather than a memory access, and
the routing state has to be repaired continuously as nodes join and fail.

**In this project:** the target architecture. **Not yet built** — Phase 2 onward.

---

### Chord

**Plain:** — *entry pending. Written after the paper is read in W1, in my own words.*

**Technical:** — *pending.*

> **Deliberately left blank.** Writing this entry from a summary rather than from the paper is
> exactly how a weak answer gets memorised. See `DEFENCE.md` Part 4, weak spot 1.

---

### Consistent hashing · Finger table · Virtual node · Stabilisation · Successor list · Raft · Quorum · Read repair · Anti-entropy

*All pending — Phases 1–4 and 9–11. Each gets a full entry when it is taught, not before.*
