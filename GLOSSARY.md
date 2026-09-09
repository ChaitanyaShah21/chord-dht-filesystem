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

### Signal, and its default disposition

**Plain:** a tap on the shoulder from the operating system saying *something happened*. Each
kind of tap has a default reaction the OS applies unless the program says otherwise — and for
several of them the default reaction is "die immediately".

**Technical:** a signal is an asynchronous notification delivered to a process. Every signal
has a **disposition**: default (`SIG_DFL`), ignore (`SIG_IGN`), or a handler function. SIGPIPE
(signal 13) is raised when a process writes to a socket or pipe whose reader has gone, and its
default disposition is **terminate the process**. A process killed by signal *n* is reported by
the shell as exit status **128 + n**, so 141 means SIGPIPE and 139 means SIGSEGV.

**Why it matters:** a disposition belongs to the **process**, not the thread that triggered it.
One connection's write to a departed peer ends every other connection in the program.

**Trade-off:** `signal(SIGPIPE, SIG_IGN)` is process-wide, so any code in the same program that
genuinely wanted the signal no longer gets it. The per-call alternative, `MSG_NOSIGNAL`, is
local and explicit but has to be remembered at every send site for ever.

**In this project:** defect R9, fixed by decision D-011. `send` now returns -1 with `errno`
set to `EPIPE`, which the `if(n <= 0)` check in every send loop already handled correctly — it
had simply never been reached.

---

### FIN / RST, and `SO_LINGER`

**Plain:** two ways to end a phone call. FIN is "goodbye, I'm hanging up now." RST is pulling
the cable out of the wall.

**Technical:** a normal `close()` sends **FIN**, a graceful half-close: the peer is told no more
data is coming, and data already queued is still delivered. Setting the socket option
`SO_LINGER` with `l_onoff = 1` and `l_linger = 0` makes `close()` send **RST** instead, which
discards anything queued and tells the peer the connection no longer exists.

**Why it matters:** the difference decides *when* a write to a dead connection fails, which is
what makes an abrupt-disconnect bug reproducible instead of a race.

**In this project:** `scripts/e2e-hangup.sh` uses it deliberately to make defect R9 fire on
demand rather than occasionally.

---

### Carriage return (`\r`) and line feed (`\n`)

**Plain:** two instructions inherited from typewriters. Carriage return slams the print head
back to the left margin; line feed rolls the paper up one line.

**Technical:** `\r` is ASCII 13 (0x0D), `\n` is ASCII 10 (0x0A). Windows ends a line with both
(`\r\n`), Unix with `\n` alone, classic Mac with `\r` alone. A protocol that treats `\n` as its
delimiter must decide what to do with a stray `\r`, because clients from different platforms —
and anything driven through `telnet` — will send it.

**Why it matters:** beyond framing, a `\r` inside a *log line* moves the cursor back to the
start of the line, so later text overwrites earlier text. That is a real technique for forging
log entries, which is why sanitising it is not merely tidiness.

**In this project:** `send_all` in `tracker.cpp` treats both as delimiters and replaces either
one found inside a reply (decision D-010).

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

### Identifier space (the Chord ring)

**Plain:** a circular corridor in a cloakroom with numbered positions 0 to 63, where 63 is
followed by 0 again. Both the coats and the attendants are given positions on that same corridor.

**Technical:** the integers `0 … 2^m − 1` arranged in a circle, so that `2^m − 1` wraps to `0`.
Chord sets `m = 160`, the output width of SHA-1 (Secure Hash Algorithm 1), so identifiers are
just SHA-1 outputs read as numbers. A node's identifier is `SHA-1(IP address and port)`; a key's
identifier is `SHA-1(the key)`.

**Why it exists:** putting nodes and keys into **one** namespace is what makes them comparable
at all. Without that, "which node is nearest this key" is not even a question you can ask.

**Trade-off:** a node cannot choose its position — that is deliberate (it stops a node placing
itself on top of valuable keys) but it also means positions land unevenly, which is the load
imbalance virtual nodes exist to fix.

**In this project:** not yet built. `m` and the derivation of node identifiers are settled in
Phase 1 alongside the five forks.

---

### Successor

**Plain:** hand your ticket to the first attendant you meet walking clockwise from your coat's
position. That attendant has your coat.

**Technical:** `successor(k)` is the first node whose identifier is **greater than or equal to
`k`**, moving clockwise around the ring and wrapping past zero if necessary. Key `k` is owned by
`successor(k)`. The comparison is **inclusive** — a key landing exactly on a node's identifier
belongs to that node — so the arc a node owns is half-open: `(predecessor_id, my_id]`.

**Why it exists:** it is the entire ownership rule of the system, and it is computable by any
node from local information, with no central map to consult.

**Trade-off:** the `>=` is the single most error-prone character in the protocol. Written as `>`,
every key that lands exactly on a node identifier is owned by the wrong node — a bug that fires
for roughly one key in 2^160 and is therefore effectively untestable by sampling.

**In this project:** not yet built.

---

### Consistent hashing

**Plain:** the cloakroom's second scheme. Instead of "ticket number modulo the number of
attendants" — which reassigns nearly every coat in the building the moment one attendant clocks
on — each attendant stands at a position on the circular corridor and keeps the coats between
themselves and the previous attendant. A new attendant takes over one stretch of corridor and
nobody else is disturbed.

**Technical:** a hashing scheme in which nodes and keys are mapped into a shared identifier
space and each key is assigned to its successor node, so that adding or removing one node out of
`N` holding `K` keys relocates on average **K/N** keys. Naive `hash(key) mod N` relocates
approximately **K** — going from 4 buckets to 5 moves about 80% of all keys.

**Note on the word "consistent":** this is *not* the consistency of the
consistency/availability trade-off. Here it means only "the mapping changes minimally when the
node set changes". Conflating the two in an interview is expensive.

**Why it exists:** it makes membership changes cheap. Without it, every join and every failure
triggers a near-total reshuffle of the data — which in a system where membership changes are
routine means the system spends all its time moving data and none serving it.

**Trade-off:** `N` randomly-placed points do not divide a circle evenly. The expected largest
arc is on the order of `(log N)/N` rather than `1/N`, so the busiest node can hold several times
the average. That imbalance is the price, and **virtual nodes** are how it is paid back.

**In this project:** the placement rule for the target architecture. Phase 4 measures the
imbalance with and without virtual nodes, so the cost above becomes a plot rather than a claim.

---

### Finger table

**Plain:** the six extra phone numbers each person in the circle carries — not six arbitrary
ones, but the people standing 1, 2, 4, 8, 16 and 32 places ahead. It is a map that is detailed
where you are standing and blurry on the far side, which is all you need: you never have to know
who owns the target, only somebody closer to it than you are.

**Technical:** a per-node routing table of `m` entries (one per bit of the identifier space).
Entry `i` holds `successor((n + 2^(i-1)) mod 2^m)` — the node responsible for the point
`2^(i-1)` clockwise of this node. Offsets are **geometric**, so `finger[1]` is the immediate
successor and `finger[m]` is halfway round the ring. A lookup scans from `i = m` downward and
forwards to the first entry lying strictly inside `(my_id, k)` — the longest jump that does not
overshoot.

**Why it exists:** it buys the middle of the state-versus-hops trade-off. Knowing only the
successor is O(1) state and O(N) hops; knowing everyone is O(N) state and O(1) hops but O(N²/L)
churn traffic (see `SCALE_NOTES.md`). The finger table is O(log N) distinct entries and
O(log N) hops. Each hop at least **halves** the remaining distance, because for any distance `d`
there is a finger at offset between `d/2` and `d`.

**Why powers of two specifically:** a geometric table has resolution *proportional to distance*,
so after a hop the situation is structurally identical at half the scale — the recursion is
self-similar and there is no smallest useful scale. Evenly spaced offsets have a fixed absolute
resolution `g`: they close the distance to `g` and then stop helping entirely, leaving a linear
walk of `O(N/m)` successor steps.

**Why `m` entries is not `m` machines:** any offset smaller than the gap to the next node
resolves to that same next node. With `N` nodes the gap is `2^m/N`, so the first `m − log₂N`
fingers all collapse onto the immediate successor. 160 rows, about **log₂N distinct nodes**.

**O(m) versus O(log N):** hops are halvings, and you stop not at distance 1 but as soon as the
remaining arc holds no other node — an arc of `2^m/N`. Halving from `2^m` to `2^m/N` is `log₂N`
steps; the `2^m` cancels. `m` bounds the pathological ring, `N` describes the real one.

**Trade-off:** it must be repaired continuously as the ring changes, and that background traffic
is permanent. But **stale fingers cost hops, never correctness** — every finger is validated
against `(my_id, k)` before use, so a wrong finger is either skipped or is genuinely closer to
the target. The finger table is self-validating; the successor pointer, which the validation is
performed *against*, is not.

**In this project:** Phase 2 (W2). The headline benchmark is hop count versus ring size, plotted
against log₂N.

---

### Virtual node · Stabilisation · Successor list · Raft · Quorum · Read repair · Anti-entropy

*All pending — Phases 3–4 and 9–11. Each gets a full entry when it is taught, not before.*
