# Defence Log — chord-dht-filesystem

**What this file is for.** Every design decision and every subsystem in this project will be
questioned by someone trying to find the edge of what I understand. This file is where those
questions get asked and answered *before* the room, not during it.

**How to use it.** Read it end to end the night before an interview. Anything marked `WEAK` is a
known gap — known gaps are manageable, unknown ones decide the outcome.

**The rule that keeps it honest:** if a claim is on the resume and not in this file, it comes
off the resume.

---

## Status summary

| | Count |
|---|---|
| Answers I can give cold | 0 — nothing rehearsed out loud yet |
| Marked SOLID on the facts | 25 |
| Marked `WEAK` — scheduled | 6 |
| Marked `WEAK` — not yet scheduled | 0 |

Last full read-through: never. **First read-through due end of W1.**

---

## Part 1 — Decisions

One entry per fork resolved in `ARCHITECTURE.md`. The question is phrased the way an interviewer
would actually ask it, not the way the decision was framed internally.

### D-001 · Why Chord?

**They ask:** "Why did you pick Chord? Kademlia is what people actually deploy."

**I answer:**
Chord's structure is a ring of nodes ordered by hash, where each node knows its successor and a
finger table of nodes at exponentially increasing distances — so a lookup halves the remaining
distance each hop and finishes in O(log N). Kademlia routes by XOR distance instead, and its
big practical advantage is that its routing table is populated as a side effect of ordinary
traffic, plus it can query k nodes in parallel and tolerate slow ones.

I picked Chord for two reasons. First, the failure and repair story is *explicit* — successor
lists, a stabilisation protocol, a defined convergence period — which means it is measurable,
and I wanted "the ring reconverges in X seconds after a kill" to be a number I could plot rather
than a claim. Second, Chord's correctness argument is small enough to hold in my head and
defend: as long as every node's successor pointer is correct, lookups terminate correctly even
if every finger table is stale. That separation between correctness and performance is the thing
I actually understand about it.

**The alternative I rejected:** Kademlia — I would have got parallel lookups and better churn
resistance essentially for free, but I would have lost the crisp, instrumentable
"stabilisation repaired the ring in X seconds" measurement, because in Kademlia repair is
diffuse and continuous rather than a discrete event.

**Where they push next:** "So what actually breaks in Chord under high churn?"
> Successor pointers, first. Chord's correctness rests entirely on successors being right; if
> enough consecutive nodes on the ring fail within one stabilisation period, the ring can
> partition into two loops that both think they are complete. That is why a successor *list* of
> r entries exists rather than a single pointer — it survives r−1 simultaneous failures. The
> finger tables degrading only costs hops, not correctness. Kademlia degrades more gracefully
> here, and that is a real advantage I gave up.

**Evidence:** `BENCHMARKS.md` §2, §4 — pending (hop count vs ring size; reconvergence time).

**Confidence:** `WEAK` — **the answer above is currently reasoning, not experience. I have not
read the Chord paper yet.** Scheduled: W1, and this entry gets rewritten in my own words after.

---

### D-002 · Inheriting broken code

**They ask:** "This started as a course assignment. What did you actually add?"

**I answer:**
It started as a coursework peer-to-peer file sharer, and when I came back to it in August it
did not compile. Two of the compile errors were in *committed* code, which told me the last
build that ever worked was never the one in the repository. I fixed the build, then found two
runtime failures that mattered more: tracker state did not survive a restart, and downloads
failed while leaving a full-size file of zeros on disk — a silent corruption, which is worse
than a crash because nothing tells you.

What I added on top is the whole distributed layer: Chord routing with finger tables, virtual
nodes for key distribution, three-way successor replication, a stabilisation thread, and
chunked parallel transfer. The coursework part is the socket plumbing underneath. I kept the
original git history deliberately rather than starting a clean repository, because the diff
between what it was and what it is *is* the evidence.

**The alternative I rejected:** starting greenfield — cleaner, but I would have spent a week
rewriting working socket code and lost the debugging story, which is the part of this project
I can talk about most concretely.

**Where they push next:** "Show me the worst bug you found in it."
> The persistence one. Tracker state is an append-only command log replayed at startup, but
> replay called the same handler as a live client with an empty username — and every command's
> own authorisation guard (`you can only do this as yourself`) rejected it. Every restart
> silently lost every group. What made it findable was the log file itself: `create_group g1
> alice` appeared six times, once per restart, because the group vanished and got recreated.
> The data was telling me it had been lost, and nobody had read it.

**Evidence:** `PROGRESS.md` § Audit — R1, with the reproduction.

**Confidence:** SOLID on the facts. `WEAK` on delivery — not yet said out loud in 90 seconds.

---

### D-003 · Committing a bad state deliberately

**They ask:** "There's a commit here called `wip` that regresses three functions. Explain that."

**I answer:**
That is the working tree exactly as I left it in November, committed unchanged in August before
I did anything else. It fixed two compile errors and it silently dropped three command handlers
I had written earlier. I committed it as-is rather than tidying it, because the alternative was
writing a story about November in August — and the regression is itself the lesson: everything
uncommitted is invisible, including my own losses.

**The alternative I rejected:** restoring the three handlers first so a single clean commit
landed. It would have read better and it would have been a fiction I then had to maintain.

**Where they push next:** "Why not just squash all of it?"
> Because the history is the evidence that this was built over months rather than generated in
> a weekend. Forty small commits with real messages read as engineering; four commits called
> "update" read as a dump.

**Evidence:** none — process decision.

**Confidence:** SOLID.

---

### D-004 · Removing features

**They ask:** "You had users and groups working. Why delete them?"

**I answer:**
They were an assignment requirement, not a property of the system I am building now. The cost
was not the code itself — it was that every subsystem I was about to add would have had to
respect group membership: replication, stabilisation, read repair, anti-entropy. Four
subsystems taxed by an access-control concern that has nothing to do with the distributed
problem. Removing it makes the code map one-to-one onto the DHT literature.

The code is still in the history if I need it, and access control is a stated non-goal rather
than an oversight.

**The alternative I rejected:** keeping authentication only, dropping groups — cheap, and it
would have answered "how do you stop anyone joining the ring?". I would rather answer that
question honestly as a non-goal with a real design sketch than half-build it.

**Where they push next:** "So how *would* you stop a malicious node joining the ring?"
> Node identity would have to stop being self-assigned. Right now a node picks its own position
> by hashing its address, so an attacker who can pick addresses can place themselves adjacent
> to a target key and become its successor — that is the Sybil/eclipse attack Chord is known to
> be vulnerable to. The standard mitigations are making identity expensive (a cryptographic
> puzzle, or a certificate from an authority binding address to node identifier) and having
> lookups verify results independently rather than trusting the routing.

**Evidence:** none — scoping decision.

**Confidence:** `WEAK` on the follow-up — the Sybil/eclipse answer is read, not reasoned from
the paper. Scheduled: W1 with the Chord reading.

---

### D-005 · The build

**They ask:** "It didn't compile when you came back to it. What was actually wrong?"

**I answer:**
Two things, and they failed at two different stages, which is the useful part. First, the
`Makefile` listed `sha1.cpp` as a source file. There is no `sha1.cpp` — `sha1.h` is header-only,
every function in it is `inline`, so it compiles into whatever includes it and there is no
object file to build. `make` died looking for a rule to produce `sha1.o`.

Second, once that was gone, `client` compiled but would not link: `undefined reference to
SHA1`. `sha1.h` calls OpenSSL's `SHA1()`. The declaration comes from a header that is present,
so compiling succeeds; the machine code lives in `libcrypto`, and nothing named that library at
link time. That is the distinction the error is telling you — a missing header is a compile
error and names a file, a missing library is a link error and names a symbol.

I put `-lcrypto` on the client's link rule only, not in the global flags, because
`tracker.cpp` does not include `sha1.h` and links clean without it. I checked that rather than
assuming it — my own audit note had claimed both binaries were affected, and it was wrong.

**The alternative I rejected:** `-lcrypto` in `CXXFLAGS` globally. One line shorter, and it
links a crypto library into a binary that does not use it. "Why does your tracker link OpenSSL?"
has no good answer.

**Where they push next:** "Given `undefined reference to X`, what are the three possibilities?"
> The library was never named. It was named but placed before the object that needs it, and the
> linker resolves left to right so it had nothing to satisfy yet. Or the symbol is mangled
> differently than expected — a C symbol referenced from C++ without `extern "C"`.

**Evidence:** `docs/failures.md` B1, B2. `ARCHITECTURE.md` D-005.

**Confidence:** SOLID.

---

### D-006 · The bug I am most pleased with

**They ask:** "Tell me about a bug you found and how you debugged it."

**I answer:**
Every download failed and left a full-size file of zeros at the destination — right size,
entirely wrong content, nothing printed. That is worse than a crash, because the file passes
the only check most people apply.

I bisected the pipeline by speaking each protocol by hand instead of reading code. I opened a
socket to the seeder and typed `GET_PIECE` myself: it returned the correct header and 300,000
bytes with the correct SHA-1, so the whole serving side was clear in one command. Then I asked
the tracker for the manifest by hand, and the seeder field came back as
`alice@alice@127.0.0.1:6881`. The username was in there twice.

The cause was one line in the tracker's upload handler:
`user_address_map[user] = user_info.substr(user.find('@') + 1)`. It searches `user` and slices
`user_info`. By that point `user` has already been stripped to `"alice"`, which has no `'@'`, so
`find` returns `npos`. `npos` is `SIZE_MAX`, not −1, so `npos + 1` wraps to zero, and
`substr(0)` returns the whole string. The map ended up holding the address as
`alice@127.0.0.1:6881` instead of `127.0.0.1:6881`, the manifest doubled the name, and the
downloader handed `"alice@127.0.0.1"` to `inet_pton`, which rejected it before a socket was ever
opened.

What makes it interesting is that the unsigned wraparound *suppressed* the crash that would have
located it. Any other value of `npos + 1` would have made `substr` throw `out_of_range` at the
exact line at fault. Zero is the one value it accepts as completely normal.

**Where they push next:** "How would you stop that class of bug?"
> Three things, in order of how much they buy. Stop packing two fields into one token — the
> whole failure is downstream of `user@ip:port` being a single space-delimited string that
> something eventually splits in the wrong place. Never compute an index on one string and apply
> it to another; the guard offset was already computed correctly two lines above and simply was
> not reused. And test the boundary, not the happy path — I added a size sweep afterwards and it
> immediately found two more defects.

**Where they push after that:** "Why didn't your tests catch it?"
> There weren't any, and that is the honest answer. The first thing I wrote after fixing it was
> an end-to-end script that compares SHA-1, because the failure mode here was silent corruption
> and only a hash comparison detects that.

**Evidence:** `docs/failures.md` R2. `PROGRESS.md` § Audit R2.

**Confidence:** SOLID — this is the strongest answer in the file.

---

### D-007 · The desync that a mutex would not have fixed

**They ask:** "You have a lock on the socket and the protocol still desynchronised. Why?"

**I answer:**
Because the lock guards the wrong granularity. It makes each `send` atomic. The invariant that
actually matters is *send-then-receive* atomic **as a pair**, and no mutex around `send`
expresses that.

I have two instances of it. The one I like better has no concurrency in it at all. After a
download completes, the client sends `update_seeder` to the tracker and never reads the reply.
The tracker does not implement that command, so it answers `Unknown command`. That reply sits in
the receive buffer, and the next read consumes it instead of the response actually being waited
for. From then on the client is permanently one response behind — single-threaded, no race, no
lock that could have helped.

It corrupts data rather than just confusing the user: the downloader ends up reading the
*previous* file's manifest, so it sizes the destination from one file and verifies the bytes
against another file's hashes.

**How I found it:** a size sweep across the piece boundary. The error messages only said "failed
to download piece 0". The diagnosis was in the size column — every file received the previous
file's size. A constant shift of exactly one is a stream desync, and nothing else looks like
that.

**Where they push next:** "So what is the fix?"
> Three options and they are genuinely different. Implement the command and read its reply,
> which restores the pairing but means specifying what re-announcing a seeder should do. Delete
> the send, which fixes the desync in one line but means a peer that finished downloading never
> announces that it can now seed, so the swarm never grows past the original uploader. Or give
> the announcement its own connection, which keeps the feature and removes the sharing — and
> that is the same fix the heartbeat thread needs, so it settles both. I have not taken it yet;
> it is an open fork because it sets the rule for every fire-and-forget message in the system.

**Where they push after that:** "Generalise it."
> Prefer removing sharing to guarding it. A lock is what you reach for when the sharing is
> genuinely necessary. A request/response socket is not a shared resource you serialise access
> to — it is a *conversation*, and conversations do not interleave.

**And what I did about it:** implemented the command properly and read the reply, so there is
one universal rule — every request gets exactly one response, read before the next is sent. Then
I found the same bug had a twin: a heartbeat thread writing down the *main loop's* socket every
30 seconds, which would have re-created the desync on a timer. That one got its own connection.
Two threads cannot share one request/response conversation whatever you do to it, because one
can consume the other's reply.

Two things I found while fixing it that changed my mind about the shape. The `sock_mtx` everyone
would point to was taken in exactly one place — the heartbeat's `send` — and the main loop's
send/recv pair took no lock at all, so it was serialising against nothing. And the tracker has no
`seeders.erase` anywhere, so re-announcing achieved nothing: a heartbeat with no timeout on the
receiving side is not a heartbeat, it is traffic. That is why expiry is a scheduled piece of work
and not something I claim to have.

**Evidence:** `docs/failures.md` R3. `PROGRESS.md` error log E3. `ARCHITECTURE.md` D-007, D-008.
`scripts/e2e-edge.sh` goes from 1/7 to 6/7 passing, and the one-row size shift is gone.

**Confidence:** SOLID.

---

### B5 · The restart bug

**They ask:** "Your server won't restart — `Address already in use` — but nothing is listening
on that port. What's going on?"

**I answer:**
The listening socket died with the process, but the connections it had *accepted* did not. They
sit in `FIN-WAIT` and then `TIME_WAIT`, and they still hold that local port. The kernel refuses
to bind a port any socket still occupies, so `bind` returns `EADDRINUSE`. `ss -ltn` shows
nothing, because those are not listening sockets; `ss -tan` shows them.

`TIME_WAIT` is not a bug being worked around. It exists so a delayed duplicate packet from the
old connection cannot be delivered to a new connection that reuses the same four-tuple. It lasts
twice the maximum segment lifetime — 60 seconds on Linux.

The fix is `SO_REUSEADDR` before `bind`. It says "bind past sockets that are merely winding
down". What it costs is exactly the protection I just described, which sequence numbers make
essentially unreachable in practice. What not setting it costs is a service that cannot be
restarted for a minute — which means it cannot be benchmarked in a loop, and that is why I hit
it: my second test run failed and my first had passed.

**Where they push next:** "Isn't that `SO_REUSEPORT`?"
> No, and the difference matters. `SO_REUSEADDR` lets you bind past lingering sockets; it will
> still refuse a second *live* listener on the same port. `SO_REUSEPORT` is the one that allows
> several live listeners to share a port with the kernel load-balancing between them — that is a
> scaling tool, not a restart tool.

**Evidence:** `docs/failures.md` B5. `PROGRESS.md` error log E2.

**Confidence:** SOLID.

---

### D-008 · Soft state

**They ask:** "Your tracker replays a log at startup to rebuild state. Does it replay seeder
announcements too?"

**I answer:**
No, deliberately. I split the state in two. *"This file exists and here is its manifest"* is
durable — it stays true whether or not anyone is online, so it goes in the log. *"Peer X
currently holds this file"* is **soft state**: only true while X is alive. Replaying it at
startup would resurrect peers that left months ago, and the tracker would confidently hand out
addresses that refuse every connection.

So seeder announcements are held in memory only, and peers re-announce periodically. After a
restart the tracker knows what exists immediately and relearns who has it within one
announcement period. That is the same trade DHCP leases and DNS TTLs make: information with a
lifetime is refreshed, not persisted.

**Where they push next:** "Then how does a seeder ever get removed?"
> Today it does not, and that is the honest answer — there is no `seeders.erase` in the tracker
> at all. Which means the periodic re-announcement is currently doing nothing useful: a
> heartbeat only means something paired with a timeout on the receiving side. Adding the expiry
> is fork F4, and it is the same decision as "how do you decide a peer is dead" — the hard part
> is not the timer, it is distinguishing a peer that is slow from one that is gone.

**Where they push after that:** "What breaks if you set the timeout too low?"
> A peer that is merely slow gets evicted, its files lose a source, and it re-announces and
> comes back — so the seeder set flaps. Too high and you keep handing out addresses that are
> already dead, and every downloader pays a failed connection to find out. It is the same
> trade-off as any failure detector: detection latency against false positives.

**Evidence:** `ARCHITECTURE.md` D-008. `tracker.cpp` — `update_seeder` does not call
`append_update_to_file`; `apply_upload_file` does not touch `seeders` either (D-009).

**Confidence:** SOLID on the reasoning. `WEAK` on expiry — not built, scheduled as F4.

### D-009 · Recovery must not ask permission

**They ask:** "You persist a command log and replay it at startup. Walk me through what
happens when the process comes back up."

**I answer:**
It used to come back up empty, and silently. The log was written correctly — every record
present, every record well-formed — but `main` replayed it by calling the live command handler
with no user attached. Every mutating command begins by checking two things: that you are who
you claim to be, and that you are currently logged in. During recovery there is nobody to be,
and `online_users` is deliberately never persisted, so both checks fail. The handler returned
an error string, `main` ignored it, and the tracker started up having discarded everything
anyone had ever done. `create_user` is the one command without those guards, which is why the
user table was the only thing that ever survived a restart.

The real mistake is not the empty string that got passed. It is that the recovery path was
**re-running admission checks instead of re-applying effects**. A record in the log is
something that was already accepted; asking permission for it a second time, on behalf of
nobody, can only fail.

So I split every mutating command in two. An `apply_*` function holds the state transition and
nothing else — no requester, no liveness lookup, no soft state. The admission checks stay on
the live path. Recovery has its own entry point that calls `apply_*` directly, so it cannot
reach a guard: not because it skips one, but because it does not call the function the guards
live in.

**Where they push next:** "Why not just pass a flag that says 'I'm replaying, skip the checks'?"
> Because that leaves the guards in the recovery path, disabled by a boolean, and every guard
> anyone adds later is a fresh chance to forget it — failing silently, which is exactly how this
> bug survived ten months. I also deleted the `record` parameter and the default value on
> `client_user`, so the specific call that caused the bug no longer compiles. I would rather
> the compiler enforce it than a code review.

**Where they push after that:** "Is replaying the request the right thing to log at all?"
> No — logging the *effect* is better, and I can show you why in my own code. When a group
> owner leaves, the successor is whichever member `unordered_set` yields first. Nothing in the
> log determines that, so a replay can pick a different owner than the live run did. An effect
> log would have recorded the decision instead of the request, and could not drift. I did not
> build it because Phase 2 replaces this control plane with a Raft-replicated tracker, which
> brings the same structure back for a stronger reason — a follower applies entries with no
> client attached at all. The drift is registered as defect R5 rather than hidden.

**Where they push after that:** "How do you know the split didn't change behaviour?"
> I drove the old and new trackers through the same eighteen-command conversation — every
> command, every error case — reading each reply with a blocking read, and diffed the
> transcripts. Byte-identical on the live path; the only differences are the three answers
> after a restart, which is the entire intended change. I did it that way because the host's
> `nanosleep` had stopped firing that afternoon and all three shell test harnesses are built on
> `sleep`, so their results were worthless. A test whose result depends on the machine's timers
> is not evidence.

**Where they push after that:** "What did the refactor find that the bug report didn't?"
> Two things, both because the split forces you to classify every line as durable or soft.
> `upload_file` was writing soft state — it inserted the uploader into the seeder set and
> recorded an address, right next to the manifest — so replay would have resurrected peers
> that are long gone, the exact thing the seeder-announcement handler already refuses to do.
> And feeding a non-numeric file size to the old tracker killed the whole process: an uncaught
> `stoull` exception inside a detached thread. One malformed command took down every client's
> connection.

**Evidence:** `ARCHITECTURE.md` D-009 · `docs/failures.md` R1 · `scripts/e2e-persistence.sh`
(committed red at `66ea0ff`, green after the fix) · protocol transcript diff.

**Confidence:** SOLID on the reasoning and the evidence. `WEAK` on delivery — not yet said out
loud in 90 seconds.

---

---

### D-010 · The bug that had been sitting there since November

**They ask:** "You found a bug in code you wrote a year ago. Why had nobody noticed?"

**I answer:**
Five of the tracker's reply strings contained an embedded newline — `"Invalid input.\nUse:
create_user <user> <pass>"` and four like it. The control protocol is line-delimited, so the
client reads one reply as bytes-up-to-newline. A reply carrying its own newline arrives as two
replies, and from that point every answer on that connection is one behind. Permanently. The
connection stays open, the tracker keeps answering, and every answer is wrong.

Nobody noticed because those five strings are on the malformed-input path. Normal use never
reaches them, and the coursework tests certainly never did. It is a latent defect in the exact
sense: the code path existed the whole time and no test created the condition that fires it.

**They push:** "So you fixed the five strings."

I fixed them, but that is not the fix. Fixing the strings removes today's instances and adds no
rule — the sixth string somebody writes reintroduces it, silently. I put the guarantee in
`send_all`, the one function that frames a reply: it strips any newline inside the payload,
logs that it had to, and appends exactly one terminator. Now the invariant holds no matter what
a handler returns, and a handler bug becomes a logged warning instead of a corrupted session.

**They push harder:** "Why not length-prefix the replies and stop worrying about delimiters?"

That is the better protocol and I picked it for Phase 5 rather than now. My peer-to-peer path
already does exactly that — `PIECE <n>` followed by n raw bytes — because file data contains
newlines constantly and a delimiter is impossible there. The control channel does not need it
yet: every reply is a single line today, and length-prefixing costs a change to every send and
receive on both sides. The trigger to take it is a command that needs structured output.

**The part I would volunteer:** this is the same defect as R3, from the opposite direction. R3
was a *caller* that sent a command and never read the reply. R6 is a *message* that contains
the delimiter. Two different mistakes, one outcome, because nothing on the wire said how many
lines a reply is. Finding the second one is what told me it was a class rather than a bug.

**Confidence:** SOLID. Fix, regression test, and the pre-fix failure are all reproducible —
`scripts/e2e-framing.sh` passes 11/11 on the fix and fails 9/11 against the old binary.

---

### D-011 · The one-line fix that is not about the one line

**They ask:** "Show me a bug where the error handling was already correct."

**I answer:**
Every send loop in this project checks `if (n <= 0) return false` — `send` returning a
non-positive value means the write failed and the caller should give up on that socket. That
check was right and it never ran. On Linux, writing to a socket whose peer has closed raises
SIGPIPE, and a signal's **default disposition is to terminate the process**, so the program died
before `send` ever returned a value to check.

I reproduced it deterministically: a client pipelines two hundred commands, reads none of the
replies, then closes with `SO_LINGER` set to zero so the close sends RST instead of FIN — an
abrupt reset rather than an orderly half-close. The tracker exits **141**, which is 128 + 13,
and 13 is SIGPIPE. The fix is `signal(SIGPIPE, SIG_IGN)` in `main`; `send` then returns -1 with
`errno == EPIPE` and the existing check does exactly the right thing.

**They push:** "How bad is it really? One client's thread dies."

It is worse than that, and this is the part worth being precise about: **a signal disposition
belongs to the process, not the thread.** One client hanging up killed the tracker and with it
every other client's session. It is also not an attack — a client stopped with Ctrl-C at the
wrong moment does it, and on the peer-to-peer data path a peer going away after it has what it
needs is the *normal* case, not the exceptional one.

**They push harder:** "Why not `MSG_NOSIGNAL` on the sends instead?"

Because then the suppression has to be remembered at every call site that exists now and every
one added later, including the raw sends in the peer server — and one miss reinstates the whole
defect silently. Ignoring the signal once covers every send in both binaries by construction.
The cost is that it is process-wide, so code that genuinely wanted SIGPIPE would not get it;
for a network server that is the standard trade. If this ever became a library rather than a
program I would revisit it, because then the ignore is imposed on somebody else's process.

**What it says about the codebase:** it is the third decision in a row resolved the same way —
D-009 split admission from effect, D-010 gave framing an owner, D-011 removes a signal from the
control flow. Each rejected the variant that works only while everyone remembers a rule.

**Confidence:** SOLID. Reproduction, fix and the pre-fix failure are all in
`scripts/e2e-hangup.sh`, which asserts the tracker is not merely alive but still serving.

---

### D-012 · Why iterative lookups, when the Chord paper is recursive?

**They ask:** "Your lookup is iterative — the client talks to every node on the path itself.
The Chord paper's `find_successor` is recursive. Why did you change it?"

**I answer:**
Three reasons, and the first one is about measurement rather than performance.

The headline number for my routing layer is hop count against ring size, plotted against log₂N.
Under iterative routing my client *is* the loop, so it counts hops by incrementing a local
variable — the instrument sits outside the thing being measured. Under recursive routing the
ring reports its own hop count in a field threaded through the messages, and my plot shows what
the system says about itself. If someone asks me how I know that number is real, I want the
answer to be "my client counted the round trips it made", not "the nodes incremented a
counter".

Second, my next phase is entirely about killing nodes and measuring recovery. Iterative gives me
exact failure attribution — a node times out and I know precisely which one, and I retry
immediately with the next finger I already hold. Recursive gives me a timeout with no
attribution: something on a path of four nodes did not answer. That is the worst possible
property for the phase whose whole job is to explain what happens when nodes die.

Third, my codebase is thread-per-connection TCP. Recursive routing means every node on the path
holds a blocked thread for the whole lookup — threads waiting on threads. Iterative handlers
return instantly and hold nothing. Recursive would have meant accepting that or rewriting the
concurrency model, which is a cost you do not see in a textbook comparison.

**They push:** "You have doubled your lookup latency to make it easier to measure. Isn't that
the tail wagging the dog?"

It is two network traversals per hop instead of one, so yes — roughly double. Two things about
that. It is a **stated, accepted cost**, not a discovery; it is written in the decision log with
the number. And on my measurement environment it is nearly free, because everything runs on
loopback, which has no real network latency — **which is declared at the top of
`BENCHMARKS.md`, and it means the environment flatters this exact choice.** I would rather
volunteer that than be caught by it. On a wide-area ring at 75 ms between regions and four hops
it is about 600 ms against 375 ms, and that gap is real.

**They push harder:** "So on a real deployment you would switch to recursive."

No — I would add a short-TTL lookup cache at the originator first. A stale cache entry costs one
wasted hop and then self-corrects, because a bad routing hint can only ever cost hops and never
correctness: every routing hint is validated against `(my_id, k)` before it is used, so it is
either rejected or is genuinely a node closer to the target. That reclaims most of the latency
gap without giving up failure attribution or measurable hop counts. Switching to recursive would
give up both to buy back something a cache buys more cheaply.

**The part I would volunteer:** I am diverging from the Chord paper here, and I am not the first.
Kademlia — the DHT that actually shipped at scale, in BitTorrent and Ethereum — is iterative,
and for the same reasons: it wants to drive its own timeouts and to query several candidates in
parallel rather than wait on one slow forwarder. Parallel queries are the next thing iterative
routing makes possible for me and recursive would not.

**What would change my mind:** ring members behind NAT. Iterative requires the originator to
open a connection to every node on the path, and that is impossible if the nodes are behind home
routers. But that scenario breaks my data plane harder than my control plane — an unreachable
peer cannot accept a file transfer either — so it needs hole punching regardless, and it is not
a routing decision.

**Confidence:** SOLID on the reasoning. `WEAK` on evidence until Phase 2 — the hop-count plot
that justifies the choice does not exist yet, and the honest statement today is "decided at
design time for measurability, not yet demonstrated".

---

### D-013 · How do you tell a dead node from a slow one?

**They ask:** "Your successor stops answering. How do you know it died rather than just being
busy? And what happens if you get it wrong?"

**I answer:**
I don't treat "it stopped answering" as one signal, because the operating system gives me two and
they are different evidence.

If I get **`ECONNREFUSED`**, the kernel on the far side actively sent me a reset — nothing is
listening on that port. That is proof, not suspicion, and I evict immediately. If I get a
**timeout**, I have learned almost nothing: dead, slow, a lost packet, or a node whose CPU is
contended all look identical. So a timeout marks the successor *suspect* and I require a second
independent failure, or confirmation from the next stabilisation round, before evicting. One
timeout is not evidence.

If I do get it wrong — evict a node that was only slow — nothing special happens, and that is
deliberate. The evicted node is **never told**. It re-inserts itself on its own next stabilise
round, because `notify` will be accepted by whoever is now its successor. There is no eviction
protocol and no un-eviction protocol, so there is no message that can be lost. A node recovers
from a false positive exactly the way it joined in the first place.

**They push:** "Why not heartbeats? Everyone uses heartbeats."

Because I get the same speed-up for free. Every lookup, every `notify`, every chunk transfer
already talks to my successor and already knows when it failed — I was throwing that away. Now
any failed call to the successor, from any code path, feeds the detector, so my detection time is
`min(next stabilise, next natural traffic)`.

That makes detection **load-adaptive**, and I think that is the right shape: detection speed only
matters when somebody is affected, and somebody being affected means traffic is flowing — which
is exactly when this is fastest. On an idle ring it degrades to plain stabilisation, and on an
idle ring nobody notices. Heartbeats buy speed during precisely the periods when nobody would
have cared, and charge constant background traffic for it.

**They push harder:** "That is a rationalisation. Heartbeats give you a bounded detection time
and yours depends on the workload."

Two honest answers. Yes — my detection time is a distribution, not a constant, and I publish two
numbers instead of one: reconvergence under load and reconvergence when idle. I would rather have
two honest numbers than one convenient one.

And heartbeats would have been actively wrong **on my hardware**. Every peer in my test ring runs
on the same 8 cores and 3.4 GiB of RAM, so at any interesting ring size nodes are intermittently
slow by construction. An aggressive heartbeat detector would generate node deaths that are an
artefact of my test rig rather than a property of my system — and I would then have plotted them.
Knowing that my measurement environment can manufacture the very events I am measuring is part of
why I chose this.

**They ask:** "What is your stabilisation period, and why that number?"

It is picked off a curve, not guessed. The period does double duty — it bounds worst-case
detection *and* it sets the repair rate, because stabilisation only tightens the successor pointer
by one node per round. So it appears twice in the reconvergence figure. Phase 3 plots
reconvergence against the period and I take the value from the knee.

**The part I would volunteer:** the right answer on a real network is a **phi-accrual** detector —
Cassandra's approach, where instead of a binary alive/dead you track the history of reply
latencies and emit a continuously rising suspicion level, so the threshold adapts as the network
genuinely slows. I did not build it because everything here is measured on loopback, which has
essentially no jitter, so phi-accrual would degenerate into a fixed timeout wrapped in statistics.
The real weakness of what I built is that a fixed timeout assumes a stationary latency
distribution, and a wide-area network does not have one.

**Confidence:** SOLID on the reasoning, and the `ECONNREFUSED`-versus-timeout distinction is the
part I would lead with. `WEAK` on evidence until Phase 3 — the reconvergence numbers, idle and
under load, do not exist yet.

---

### D-014 · Why don't you need vector clocks?

**They ask:** "You have three replicas of every chunk. What happens when they disagree — how do
you decide which one is right?"

**I answer:**
They cannot disagree. My key is `SHA-1` of the chunk's own contents, so the value stored at key
`abc123…` is by definition the bytes that hash to `abc123…`. There is no second candidate. A
chunk is immutable — change the data and you have a different key, not a new version of the same
one.

So there is no versioning anywhere in my data plane: no timestamps, no vector clocks, no
last-write-wins, no conflict resolution. I did not solve that problem, I chose a key that does
not have it.

**They push:** "Then what is your `W` and your `R`, and does `W + R > N` hold?"

It does not hold, and it does not apply — and that distinction is the answer. `W + R > N` exists
to guarantee that a read set and a write set overlap **so a read sees the most recent version**.
I have no versions. The inequality has nothing to constrain.

What that buys me is that `W` and `R` stop being coupled. `W` buys **durability** — how many
machines hold it before I tell the client the write succeeded. `R` buys **availability** — how
many replicas I must reach to get an answer. I set them independently, which a mutable store
cannot do.

`R = 1` is always correct for me, because a read is **self-verifying**: the client hashes what
arrived and compares it to the key it asked for. A corrupt, truncated or wrong answer is caught
locally by the reader without consulting a second replica. If it fails the check, I go to the
next replica — that is a retry loop, not a quorum.

**They push harder:** "That sounds like you dodged the hard part of distributed systems."

I moved it rather than dodged it, and moved it deliberately. The **data** plane is immutable, so
it has no conflicts. The **metadata** plane — the file manifest, which chunks make up a file, and
which nodes claim to hold them — is genuinely mutable, and that is where consistency is a real
problem. That is the plane I replicate with consensus.

So the honest statement of where this system sits is that it is in two places at once, on
purpose: content-addressed and available on the data path, consensus-backed and consistent on the
control path. I would rather defend that than one uniform answer applied to two workloads that do
not want the same thing.

**The part I would volunteer:** it costs me in-place update. A changed file produces new keys and
the superseded chunks are garbage, and I have no garbage collector — that is a stated scope
boundary, not an oversight. It also means I cannot influence placement at all: a chunk goes where
its hash sends it, which is exactly the rack-awareness problem — three ring-adjacent replicas may
share a rack, and content addressing removes even the option of fixing that by placement.

**Confidence:** SOLID on the reasoning. `WEAK` on evidence — the deduplication rate is a Phase 5
measurement and does not exist yet.

---

### D-015 · Where does this sit on the consistency/availability trade-off?

**They ask:** "Three replicas. When do you tell the client the write succeeded?"

**I answer:**
When two of the three have it — the owner and whichever successor answers first. The third copy
propagates in the background.

The owner writes locally and sends to both successors in parallel, so `W = 2` waits for the
**faster** of the two, not a specific one. `W = 3` would wait for both, which means the slowest
peer sets the latency of every write. On my test rig every peer shares the same 8 cores, so one
peer being briefly very slow is the normal case and that difference is large.

**They push:** "Why not acknowledge on one and propagate everything in the background? It is
faster and always available."

Because then an acknowledged write can be lost. The owner accepts, tells the client "saved", and
dies before it propagates — the chunk is gone and the client believes it succeeded. For a file
system, a system that lies about durability is the worst defect class there is. `W = 2` is the
point where an acknowledged write is a **true statement** and a single failure still does not
stop me.

**They push:** "Then why not `W = 3` and be properly safe?"

Because `W = 3` makes a write fail whenever **any one** successor is down, and during churn — or
mid-stabilisation, while a successor pointer is still tightening — that is a meaningful fraction
of the time. It buys protection against a second simultaneous failure by making writes fail
during every first one. That is a bad trade for this workload.

**They push hardest:** "So where does this system sit on the consistency/availability trade-off,
and how would you flip it?"

Two answers, because it is deliberately in two places.

On the **data** plane there is no consistency question at all — chunks are content-addressed and
immutable, so replicas cannot disagree. What is left is durability against availability, and
that is the `W` knob. I did not pick a point on it and argue for it; I made `W` a runtime
parameter and measured the curve. Sweeping `W` from 1 to 3 gives me write latency and write
success rate while a node is being killed, so "how would you flip it" has a concrete answer:
this parameter, and this is what each setting costs in my own numbers.

On the **control** plane — the manifests, which chunks make up a file — the data is genuinely
mutable, so that is where consensus goes.

**The part I would volunteer:** `W = 2` means my advertised replication factor of three is
briefly untrue on **every write**, not just after a failure. There is a window between
acknowledgement and background propagation where two copies exist, so the system tolerates one
further failure rather than two. That is the same point as durability figures being steady-state
properties. I measure that window rather than assuming it away.

And if `W = 2` writes started failing too often, the fix is not to lower `W` — it is **hinted
handoff**: write the copy to the next node along with a note saying who it really belongs to, and
forward it when that node comes back. That raises write availability without weakening the
durability guarantee. I left it out of Phase 4 on time budget, not on merit.

**Confidence:** SOLID on the reasoning. `WEAK` on evidence until the Phase 4 `W`-sweep exists —
the whole strength of this answer is that the curve is measured, and it is not measured yet.

---

### D-016 · Why 512 KB chunks?

**They ask:** "Your chunk size is 512 KB. Why that number?"

**I answer:**
Two separate answers, because the number I *build* at and the number I can *justify* come from
different places.

I build at 512 KB because my Phase 0 baseline was measured at 512 KB. My headline transfer claim
is parallel transfer against that baseline, and if I changed chunk size at the same time as
adding parallelism I would have moved two variables and the improvement could not be attributed
to either. So chunk size is pinned for the before/after, and swept separately with parallelism
held constant. One controlled variable per claim.

The justification for the size itself is a curve, not an opinion. I sweep 64 KB, 256 KB, 512 KB,
2 MB and 8 MB and publish the throughput. Five points rather than three, because three cannot
distinguish a curve with a knee from a straight line, and the knee is the whole argument.

**They push:** "What do you expect the curve to show?"

Possibly nothing, and I would report that. Everything runs on loopback, which has no network
latency for a larger chunk to amortise, and the page cache absorbs much of the I/O difference. If
the curve is flat, the honest result is "chunk size barely matters on loopback, here is why, and
here is what would change on a real network". I would rather publish a flat curve than tune until
something looks interesting.

**They push harder:** "Smaller chunks parallelise better. Why not 64 KB?"

Because in my system chunk count is a **routing** cost, not just an I/O cost. My lookups are
iterative, so each one costs two network traversals per hop — about nine round trips on a 20-node
ring. At 64 KB a 100 MB file is 1,600 chunks, so that is roughly 14,400 round trips of pure
lookup before a single byte of payload moves. At 512 KB it is 200 chunks. Smaller chunks buy
parallelism with routing traffic, and that trade only shows up once you know the routing model.

There is a second effect. Because my chunks are content-addressed, consecutive chunks of one file
land on unrelated ring positions — there is no locality at all, so every chunk is an independent
lookup to an arbitrary node. What saves me is that the manifest gives me every hash up front, so
I issue the lookups concurrently rather than serially. That requirement exists because of two
earlier decisions meeting, and I designed for it rather than discovering it.

**The part I would volunteer:** 4 MB would have been defensible on lookup cost alone, and I
rejected it partly because it would make my own deduplication claim theoretical — a 4 MB span
almost never repeats across files, so content addressing would stop paying for itself.

**Confidence:** SOLID on the method and on the routing-cost argument. `WEAK` on the number until
the sweep exists — today the honest statement is "512 KB because it holds my baseline comparable,
and the curve that justifies it is Phase 5 work".

---

### D-017 · If the ring knows where everything is, what is the tracker for?

**They ask:** "You built a distributed hash table so there would be no central index. Then you
kept a tracker. Isn't that the central index you were trying to remove?"

**I answer:**
It would be, if it held locations. It doesn't, and it can't usefully — a chunk's key is the hash
of its own contents, so its location is **computable**: it lives at `successor(SHA-1(chunk))`.
Anyone holding the hash can route to it. A tracker copy of that would be a cache of something the
ring already answers authoritatively, and it would go stale on every join, every failure and
every stabilisation round. Two systems that can disagree, where one of them is right by
construction.

What the ring genuinely cannot tell you is **which chunks make up a file, in what order**. You
can't discover that by routing, because you don't know the hashes to route to. So something must
hold it — and my tracker holds the smallest possible version: `filename → manifest hash`, about
40 bytes per file. The manifest itself is a content-addressed object in the ring, like any chunk.

**They push:** "So why not just put the manifest in the tracker too?"

Because then tracker state grows with total chunk count — a 10 GB file at 512 KB chunks is 20,000
hashes in one record — and that state is what I have to replicate with consensus. Keeping it at
40 bytes per file keeps the Raft log small. It also keeps the tracker off the data path
completely: it is consulted once per file and returns 40 bytes, never a payload.

There is a third benefit I did not design for and noticed afterwards: because the manifest is
just a chunk, it inherits replication, availability and the hash check for free. I did not have
to special-case it.

**They push harder:** "This sounds like you reinvented something."

It is Git's data model, and I would rather say so than have it pointed out. Content-addressed
immutable objects — blobs and trees — with a small mutable namespace on top, the refs. The
insight I took from it is that **only the namespace needs consensus**. Human names change;
content does not. The mapping between them is the only mutable thing in my system, and making
that surface as small as possible is the whole design.

**The part I would volunteer:** it costs an extra round trip, name to hash then hash to manifest.
And it makes the tracker **required for discovery** — a manifest hash means nothing to a human,
so there is no browsing the system without the namespace layer. That is a real centralisation and
I accept it deliberately, because it is the one component I am making highly available with
consensus rather than pretending is unnecessary.

**Confidence:** SOLID on the reasoning. `WEAK` on evidence — the `O(files)` claim about tracker
state is a Phase 5 measurement and does not exist yet.

---

## Part 2 — Subsystems

Three to five questions per subsystem, written when that subsystem is finished (R14). These are
the "go deeper until you break" questions — they get harder as they go down the list.

### Wire protocol and framing — *legacy layer, questions valid now*

| # | Question | Answer | Confidence |
|---|---|---|---|
| 1 | How do you know where one message ends and the next begins? | Two schemes for two conversations. Client↔tracker is newline-delimited with a `partial` buffer that survives across `recv` calls, so a split command is reassembled. Client↔client is length-prefixed: a `PIECE <n>\n` header then exactly n raw bytes — a delimiter is impossible there because binary file data contains newline bytes by chance. | SOLID |
| 2 | What happens if `recv` returns fewer bytes than you asked for? | It is expected, not an error — TCP is a byte stream, not a message queue. The piece loop advances a `total` counter, offsets the destination pointer by it, and asks for the remainder. A `recv` without that loop is a latent bug that passes on loopback and fails on a real network. | SOLID |
| 3 | You have a mutex on the socket. Why did the protocol still desync? | Because the mutex guards the wrong granularity. It makes each `send` atomic; the invariant that matters is *send-then-receive* atomic as a pair. A 30-second heartbeat thread shared the main loop's socket, the tracker replied, nobody read that reply, and the main loop's next read consumed the wrong response. | SOLID — this is the best answer in the file |
| 4 | How did you find it? | The tracker had filters discarding lines that started with its own reply prefixes, and a comment saying "ignore random single-character junk". Nothing in a correct protocol produces random junk. The filters were scar tissue — someone had treated the symptom. I worked back from the filter to the shared socket. | SOLID |
| 5 | Fix it without a lock. | Give the heartbeat its own connection to the tracker. The shared resource disappears, so there is nothing to serialise. The general form is: prefer removing sharing to guarding it — a lock is what you reach for when the sharing is genuinely necessary, and here it was not. | SOLID |

### Finding defects — *the method, valid now*

Written after Teaching Part 6 (6 Sep 2026). The subject is not the twenty defects in
`PROGRESS.md` — it is the classification, because *"what would you expect to break first?"* and
*"how do you find bugs like that?"* get asked in every round, and a list is not an answer to
either.

| # | Question | Answer | Confidence |
|---|---|---|---|
| 1 | What is a latent defect, and why did your tests not catch these? | A defect that exists in the code but has no path to fire under the conditions the tests create. The tests were not wrong, they tested the wrong thing. Five conditions the happy path never produces: a second thread arriving at the same instant; input chosen by someone who wants it to break; a peer that dies *during* an operation rather than before or after; a boundary value — empty, zero-length, first, last, exactly one piece; and ten or a thousand times the scale. Listing the conditions the tests never create is a search procedure, not a warning. | SOLID |
| 2 | Twenty defects. Group them. | Five classes. **A — one party violated the framing contract**, giving permanent stream desync: R3, R6, D2, and the single-`recv` mistake. **B — a value crossed a trust boundary and was believed**: R7 (the peer chooses an allocation size), D3 (the peer chooses a path reaching `open`), D4 (the client chooses a string reaching `stoull`), R8 (the peer chooses how long you wait, because nothing has a timeout). **C — the stored representation cannot reconstruct the state**: R1, R2b, R5. **D — lifetime and ownership**: D1 and B4. **E — correct but the cost is wrong**: D5, D7, the global `state_mtx`, one thread per client. Plus the honesty class — C1, D6, D8: features documented and never implemented. | SOLID |
| 3 | Class A has four members. Is that four bugs or one? | One bug, four times — and they are not the same *kind* of mistake, which is the point. R3 was a **caller** violating the contract: it sent a command and never read the reply, so one unconsumed reply sat in the stream for ever. R6 is a **message** violating the framing: five reply strings contain the delimiter itself, so one command produces two lines. Different mistakes, identical outcome, because the protocol never states how many lines a reply is. Fixing the five strings removes today's instances and adds no enforcement — the sixth string written in December reintroduces it. The class closes only when the count is guaranteed by construction: the framing layer strips delimiters on the way out, or the length is explicit on the wire. | SOLID |
| 4 | R7 lets a peer size an allocation on your machine. Close it. | The expected length is `min(PIECE_SIZE, filesize - idx * PIECE_SIZE)`, and the header must be compared for **equality** against it before a byte is allocated — too small is as wrong as too large. The property that makes the check real is where the two numbers come from: the expected size derives from the manifest, which came from the **tracker**, while the header came from the **peer**. Two different parties, so a lying peer cannot fabricate both. That also names the residual assumption honestly — the tracker is trusted in this design, and if it is not, the manifest needs signing. | SOLID |
| 5 | D1 and B4 are the same C++ fact. Which is worse? | The fact: `std::vector` guarantees contiguous storage, so growing it reallocates and **moves every element**, invalidating every pointer, reference and iterator into it. B4 is that fact caught by the type system — `push_back` needs the element copyable or movable, `std::atomic`'s copy constructor is deleted, so the program never compiled. D1 is the same fact caught by nothing: `active_downloads.back()` held as a reference, and the second concurrent download reallocates it into a use-after-free. **B4 cost minutes because the compiler is loud; D1 is undefined behaviour that passes every single-download test and corrupts memory later.** The general lesson is that a failure moved from run time to compile time is the cheapest fix available — which is the same argument as splitting admission from effect in D-009, one level down. | SOLID |

### Chord routing — *not built yet*
### Replication — *not built yet*
### Stabilisation — *not built yet*
### Chunked transfer — *not built yet*

---

## Part 3 — The questions that get asked about every project

These recur regardless of what was built. Answer each one *about this project*, with specifics.

| Question | Answer | Confidence |
|---|---|---|
| Draw the whole architecture on this whiteboard. | **Both versions now exist as diagram-as-text** — the legacy system and the target Chord system, in `ARCHITECTURE.md` and the README. The target one is nine numbered steps: two to the tracker, four of iterative lookup, two to fetch the manifest, then repeat concurrently per chunk. | `WEAK` on delivery only — never yet drawn from memory under time pressure |
| Walk me through what happens on one request, end to end. | `ARCHITECTURE.md` § The diagram, steps 1–5: manifest up, manifest down, direct peer connect, length-prefixed piece, SHA-1 verify, write at offset. | SOLID for the legacy path |
| Where does this sit on the consistency/availability trade-off, and how would you flip it? | **Answered by D-014/D-015/D-017 — deliberately in two places.** The data plane is content-addressed and immutable, so replicas cannot disagree and there is no consistency question; it is tuned purely for durability versus availability with `W`, which is a **runtime parameter** so "how would you flip it" is a measured curve rather than an opinion. The control plane is the mutable `filename → manifest hash` namespace, and that is where consensus goes. | **No longer blocked.** `WEAK` on evidence only — the `W`-sweep is Phase 4 |
| What breaks first at 10x the load? | `SCALE_NOTES.md`. Current best answer: the tracker's single global `state_mtx`, then the one-thread-per-client model. Also `recv_line`, which issues **one `recv` syscall per byte** — a 60-byte reply costs 60 kernel round trips (D5). | `WEAK` — no measurement yet |
| How would you scale it? | — | `WEAK` — W2 |
| What would you monitor after deployment, and why those metrics? | Planned dashboard: lookup hops, throughput, p99 chunk latency, stabilisation events, leader changes. **Asked verbatim at a target company — this must become a screenshot, not a hypothetical.** | `WEAK` — deployment kit item 3, W10 |
| How do you log errors? Can you trace one request across components? | Currently: `cerr` with a `[tracker]`/`[client]` prefix and no request identifier — a transfer touching four peers cannot be reconstructed. Planned: `spdlog` with levels and a request id threaded through. | `WEAK` — deployment kit item 4, W10 |
| How would someone else run this? | `make clean && make` builds both binaries with zero warnings, and `scripts/e2e-smoke.sh` transfers a file and verifies the SHA-1. Target: `docker compose up` brings up a 5-peer ring and the tracker. | SOLID for `make`; `WEAK` — W5 for compose |
| How do you know a change did not break it? | Currently: nothing. No tests, no continuous integration. First test lands with the B1/B2 fix. | `WEAK` — W1 and W5 |
| Tell me about a bug you found and how you debugged it. | Three candidates, strongest first: the socket-sharing desync (D2), the replay-authorisation persistence bug (R1), the `pipefail`+`head` script abort (E1). | SOLID on content, `WEAK` on delivery |
| What is the hardest part of this, technically? | — | `WEAK` — answer honestly once Chord is built |
| What would you do differently if you started again? | Framed the protocol as a request/response state machine from the start instead of assuming one socket could serve two conversation patterns. Every desync bug traces to that. | SOLID |
| What did you learn from building it? | — | `WEAK` |
| Explain the internals of OpenSSL's SHA-1 — not just what it does. | 512-bit blocks, Merkle–Damgård construction, 80 rounds over five 32-bit registers, length padding. Also: **SHA-1 is broken for collision resistance** (SHAttered, 2017) — fine here because this defends against corruption, not against an adversary, and that distinction is the real answer. | `WEAK` — verify the round structure before claiming it |
| Was this individual or team work? How did you split it? | Individual, start to finish. | SOLID |

---

## Part 4 — Known weak spots

The honest list. Kept current, worked down in order.

| # | Weak spot | Why it matters | Scheduled for | Status |
|---|---|---|---|---|
| 1 | **Chord itself is not yet understood** — D-001's answer is reasoning, not knowledge | It is the headline of the project and the first thing asked | W1 (paper) then W2 (build) | OPEN |
| 2 | **No consistency/availability position** — blocked on fork F3 | The single most likely place a systems round lands | W1 (decide), W4 (build) | OPEN |
| 3 | **No numbers at all yet** — every claim is currently unsupported | R15: a claim with no number comes off the resume | W1 baseline, W2–W5 the rest | OPEN |
| 4 | **Cannot answer "what would you monitor?"** with anything concrete | Asked *verbatim* at a target company | W10 — deployment kit item 3 | OPEN |
| 5 | **No tests and no CI** — "how do you know it still works?" has no answer | Asked in nearly every round | W1 (first test), W5 (CI) | OPEN |
| 6 | **Debugging stories not yet tellable in 90 seconds** | The content is solid; the delivery is untested | W6 and W16 rehearsals | OPEN |

**Rule:** a weak spot that is not scheduled is a weak spot that will still be there in December.
Every entry gets a date.

---

## Part 5 — Rehearsal log

Full-hour sessions with someone actively attacking the design. Record what they found — the
point of the rehearsal is the list of things it broke.

*No rehearsals yet. First one due W6 (short, after the freeze); the full hour is W16, 16–20 Dec.*

**W16 is on the never-cut list.** It is the phase that feels least like progress and matters
most, and it is the one that gets dropped under deadline pressure.
