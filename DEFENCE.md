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
| Answers I can give cold | 0 — nothing rehearsed yet |
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

### Chord routing — *not built yet*
### Replication — *not built yet*
### Stabilisation — *not built yet*
### Chunked transfer — *not built yet*

---

## Part 3 — The questions that get asked about every project

These recur regardless of what was built. Answer each one *about this project*, with specifics.

| Question | Answer | Confidence |
|---|---|---|
| Draw the whole architecture on this whiteboard. | Legacy version: yes, it is five boxes. Target Chord version: not until Phase 1 draws it. | `WEAK` — W1 |
| Walk me through what happens on one request, end to end. | `ARCHITECTURE.md` § The diagram, steps 1–5: manifest up, manifest down, direct peer connect, length-prefixed piece, SHA-1 verify, write at offset. | SOLID for the legacy path |
| Where does this sit on the consistency/availability trade-off, and how would you flip it? | Right now: **neither, and saying so is the honest answer** — one tracker means nothing to be consistent between and nothing to stay available through. CAP only says something once there is more than one replica. The real answer arrives with fork F3. | `WEAK` — blocked on F3, W1 |
| What breaks first at 10x the load? | `SCALE_NOTES.md`. Current best answer: the tracker's single global `state_mtx`, then the one-thread-per-client model. | `WEAK` — no measurement yet |
| How would you scale it? | — | `WEAK` — W2 |
| What would you monitor after deployment, and why those metrics? | Planned dashboard: lookup hops, throughput, p99 chunk latency, stabilisation events, leader changes. **Asked verbatim at a target company — this must become a screenshot, not a hypothetical.** | `WEAK` — deployment kit item 3, W10 |
| How do you log errors? Can you trace one request across components? | Currently: `cerr` with a `[tracker]`/`[client]` prefix and no request identifier — a transfer touching four peers cannot be reconstructed. Planned: `spdlog` with levels and a request id threaded through. | `WEAK` — deployment kit item 4, W10 |
| How would someone else run this? | Currently: they cannot — `make` fails. That is the first thing being fixed. Target: `docker compose up` brings up a 5-peer ring and the tracker. | `WEAK` — W1 for `make`, W5 for compose |
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
