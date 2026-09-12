# Progress — chord-dht-filesystem

Read this first in every session (R16), together with `ARCHITECTURE.md` and anything marked
`WEAK` in `DEFENCE.md`.

---

## Where we are right now

**Phase:** 1 — Design forks resolved. *(Phase 0 closed 9 Sep 2026, tag `phase-0-complete`.)*
**Last completed step:** **The system builds and transfers a file end to end for the first
time.** Fixed B1 + B2 (the Makefile), B5 (`SO_REUSEADDR`, found this session) and the root
cause of R2 (the address-map typo). `scripts/e2e-smoke.sh` transfers a 300 KB file and the
SHA-1 matches. Adversarial size sweep `scripts/e2e-edge.sh` added; it found **R3 and R4**, both
new. All of it written up in `docs/failures.md`.
**Also done 2 Sep:** forks **F6** and **F6a** decided and implemented (D-007, D-008).
`update_seeder` is a real tracker command, the client reads its reply, and the announcer thread
owns its own connection instead of sharing the main loop's socket. `scripts/e2e-edge.sh` goes
from **1/7 to 6/7 passing**; the only remaining failure is R4, the zero-byte file.
**Also done 6 Sep:** Teaching Part 4 (persistence and the replay path) delivered and its
comprehension checks answered. **R1 is now reproduced by a script rather than remembered:**
`scripts/e2e-persistence.sh` builds state, restarts the tracker on the same port and directory,
and asks for the same four facts back — **1/4 survives**. Committed RED on purpose (D-006
precedent). Fork **F8** decided: option B, split admission from effect.
**F8/option B implemented (D-009).** `e2e-persistence.sh` is **4/4 green**, the live path is
byte-identical to before, and the recovery path survives corrupt logs, coursework-era logs,
duplicate records and a restart that changes nothing.
**Then the machine ate it.** The WSL2 restart that fixed the timer fault discarded every write
still sitting in the page cache: `tracker.cpp` and its binaries came back **zero bytes**, and
eleven git objects — including the commit holding the fix — came back **zero bytes**, leaving
the repository unopenable. Full account in error-log entry **E5**. The four documents survived,
so **D-009 was rebuilt from its own specification on 6 Sep** and re-verified from scratch:
`e2e-persistence.sh` 4/4, `e2e-smoke.sh` PASS, `e2e-edge.sh` 6/7, and the live path proven
byte-identical to `66ea0ff` across a 20-command conversation.
**Timers are still not right:** after the restart `sleep 2` takes about 5 s. Sleeps running
*long* is safe for harnesses that wait, so the shell suites are trustworthy again but slow;
anything measuring **time** stays untrustworthy until this clears, which blocks the Phase 0
baseline benchmark.
**Also done 9 Sep:** teaching **Part 6** (the latent defects) delivered, comprehension checks
answered 3/3, and the taxonomy written into `DEFENCE.md`. **The README was corrected** — it had
gone stale in the *opposite* direction to defect C1, telling a public repository that the build
was broken and the transfer and persistence paths failed, none of which had been true for a
week. Fork **F9 resolved and implemented (D-010)**: `send_all` owns the one-reply-one-line
invariant, `scripts/e2e-framing.sh` is the regression test, and error-log entry **E6** — the two
suites that killed each other through a machine-wide `pkill` — is fixed and proven by running
smoke and persistence concurrently.

**Also done 9 Sep:** fork **F10** resolved and implemented (**D-011**, closes **R9**) — found
while walking through `send_all` line by line for teaching, which is the second defect this week
found by reading rather than running. Five suites now, all green except the one deliberate
failure: smoke PASS · persistence 4/4 · framing 11/11 · hangup 2/2 · edge 6/7 (R4, by design).

**Working-style change agreed 9 Sep:** **teach every piece of code written, not only the code
belonging to a numbered teaching Part** — including shell and Python in `scripts/`. The reason
is that the repository is judged by whether he can walk through it, and a fix he cannot explain
is indistinguishable from one somebody else made. This session is the evidence for the rule as
well as the occasion for it: R9 was found *because* `send_all` was being explained aloud.

**PHASE 0 IS CLOSED — 9 Sep 2026, tag `phase-0-complete`.** Timers were verified sane first
(`sleep 2` measured 2.008 s, monotonic and wall clocks in agreement), then the baseline was
measured on a clean tree at `e381179`: **20.9 / 46.2 / 71.2 MB/s** at 1 / 10 / 100 MB, median of
three runs each. Recorded in `BENCHMARKS.md` §1 with method, spread and every reason the number
flatters the system. It is a *sequential* baseline by construction — one seeder means the worker
pool runs one thread — which is exactly the right "before" for the Phase 5 parallel-transfer
claim, and it could not have been recovered after that code lands.

**Deferred out of Phase 0 by decision, not by drift:** `docs/postmortem-resurrection.md` moves
to W6, the documentation week, because every fact it needs is already written in
`docs/failures.md` and the defect register and assembling it is prose work. Fork **F7** (R2b)
moves to Phase 5, where the transfer layer is rewritten anyway.

**Next step (12 Sep):** **Part 7 is complete — all five steps taught, comprehension 10/10.**
Phase 1 now resolves the five open forks. They are taken in **dependency order, not numeric
order**, because that is the order the build needs them: **F1** (routing) gates Phase 2 W2 and
is on the table now · **F4** (failure detection) gates Phase 3 W3 · **F3** (replication) gates
Phase 4 W4 · **F5** (chunk size) and **F2** (the tracker's job) gate Phase 5 W5.

**F1 is RESOLVED — 12 Sep, iterative (D-012).** Recorded in all three places per R12:
`ARCHITECTURE.md` decision log, `DEFENCE.md` D-012, and the README's Key Design Rationales.
**Next fork on the table: F4, failure detection.** R11's condition
— that no fork is decided before Chord is understood — is now satisfied by the teaching rather
than by assigned reading, per the 2 Sep working-style change.
**Blocked on:** nothing. F7 is Chaitanya's call when we reach it in Phase 5 (R6).

**Teaching progress (fresh pass, 2 Sep):** Part 1 system shape ✅ · Part 2 the wire ✅ ·
Part 3 tracker state, data structures and locking ✅ · **Part 4 persistence and the replay
path ✅** (6 Sep — comprehension checks answered and graded; `std::atomic` re-taught after a
gap in the Part 3 quiz and re-checked correct: atomic makes each operation indivisible, never a
sequence of them) · **Part 5 the transfer path ✅** (6 Sep — manifest, sentinel parsing,
preallocation and sparse files, the lock-free work queue, the short-read loop, length-prefixed
framing for data versus line framing for control; comprehension checks answered and graded, and
the reading pass found **R7** and **R8**).

**Part 5 grading, 6 Sep — four solid, two to sharpen.** Solid: durable versus session state;
hash-versus-TCP as two different threat models; the short-read loop. To sharpen, both *delivery*
gaps rather than knowledge gaps, and both re-checked before the defence rehearsal:
- **Why R1 is impossible rather than avoided.** The answer is *the guard does not exist on the
  replay path*, so there is no path to travel down — not "the apply functions do not need
  authentication", which describes why it works rather than why it cannot break.
- **Why `fetch_add` is safe when check-then-act is not.** One indivisible read-modify-write
  versus two atomic operations with a gap between them. "Because it is atomic" invites the
  follow-up and does not survive it.

**Part 5 also reversed an earlier call.** Part 5 had been ranked skippable; it is not.
**Chord replaces the lookup, not the transfer** — finding which node holds a chunk becomes a
routing problem, but moving the bytes stays this code. So the transfer path is permanent, it is
what the throughput benchmark measures, and "how does a file actually get from A to B" is the
first question anyone asks about a file system.

**Part 6 the latent defects ✅** (9 Sep — the five conditions a happy path never creates, the
twenty defects sorted into five classes by cause, and what actually found each. Comprehension
3/3: the caller-versus-message distinction inside class A, the two-source check that closes R7,
and why B4 cost minutes while D1 could cost an afternoon.)

**Part 7 Chord — in progress**, split into five steps because it is the concept the project is
named after: 7.1 consistent hashing · 7.2 the ring as a data structure and the O(N) lookup ·
7.3 finger tables and O(log N) · 7.4 joins and stabilisation · 7.5 failure, successor lists and
where replication attaches. The five forks are presented as decisions after 7.5.

**7.1 consistent hashing ✅** (9 Sep — the modulo-N reshuffle as the problem, one shared
identifier space as the fix, the successor rule with its inclusive boundary, and the `(log N)/N`
load imbalance flagged as a deliberate cost that Phase 4 pays back with virtual nodes.
Comprehension 2/2 on substance, with two corrections worth keeping: the successor is the first
node with identifier **>=** the key, not `<=`, and the correctness invariant is that **only the
successor pointer must be right** — every other piece of routing state is an accelerator whose
staleness costs hops, never correctness.)

**7.2 the ring as a data structure and the O(N) lookup ✅** (9 Sep — the four fields a node
holds, identifier-versus-address as an R10 confusion hazard, three-line `find_successor`, the
**wrap-around interval test** and why the naive `start < k <= end` loops forever on exactly one
arc, the single-node ring checked against the fix rather than assumed, and the three separate
costs of O(N) hops: latency, **failure exposure** — hops are the number of things that must all
be alive — and per-node lookup load rising with ring size. Comprehension 2/2 on substance, two
sharpenings: random data finds distribution bugs but **only construction finds structural ones**,
and the rejected full-membership design is not simply "bad" — it is what Dynamo and Cassandra
actually ship, correct until `churn × N` gets large. Logged in `SCALE_NOTES.md`.)

**7.3 finger tables and O(log N) ✅** (9 Sep — geometric offsets, the two interval tests three
lines apart with deliberately different endpoint rules, a three-hop worked lookup on a ten-node
ring, and the halving argument stated as a proof rather than a description. Comprehension: C6
strong — he identified `finger[1]`/the successor as the one corruptible field that breaks
correctness, unprompted. C5 reached the right conclusion (linear versus logarithmic) without the
mechanism, so the mechanism was supplied: even spacing has a **fixed absolute resolution** and
falls off a cliff at distance `g`, geometric spacing is **self-similar** and has no smallest
useful scale. Two follow-up questions he raised and had answered: why 160 rows collapse to
log₂N distinct nodes (any offset below the inter-node gap resolves to the same successor), and
where `N` enters a fixed 160-bit space (through **when you stop** — the arc `2^m/N` — not where
you start; the `2^m` cancels). New idea recorded: the finger table is **self-validating**,
because every entry is checked against `(my_id, k)` before use, so a corrupt finger can only
waste hops — while the successor, which that check is performed *against*, has nothing to
validate it.)

**7.3 follow-ups, 11 Sep.** Two pieces were re-taught on request because the first pass was too
compressed: (i) **why O(log N) rather than O(m)** — rebuilt with concrete numbers instead of
algebra. Halving counts **ratios, not distances**; a 1000x bigger identifier space adds *zero*
hops because it stretches the start line and the finish line equally, while 1000x more nodes
adds ten, because the finish line is the inter-node gap `2^m/N`. (ii) **C5 and C6(b)** re-taught
concretely: even spacing = six maps all at the same zoom, geometric = a map at every zoom, with
the numbers showing geometric costs +1 hop per doubling of N while even spacing *doubles*; and
the successor's special status stated as **"you cannot check the ruler with itself"** — fingers
are answers that get measured against the ring's real geometry, the successor is what the
measuring is done with.

**7.4 joins and stabilisation ✅** (11 Sep — `join`/`stabilize`/`notify`/`fix_fingers`, the
level-triggered versus edge-triggered distinction and where else it appears, the honest window in
which Chord returns confidently wrong answers, and the three regimes. Comprehension 2/2, both
strong. C7: he named `notify`'s interval test as the conflict resolver unprompted; sharpened with
the **monotonicity** argument — both pointers move inward only, so a tightening sequence on a
finite ring terminates — and with the intermediate state he skipped, where N32 briefly points
past N36 and is itself wrong. C8: he found the uncomfortable answer without being told —
stabilisation *does* repair a corrupted successor, but **one node per round**, so an eight-minute
window of silent wrongness on a 1,000-node ring. That derivation is the reason Phase 3's
time-to-reconverge benchmark exists.)

**7.5 successor lists and where replication attaches ✅** (12 Sep — the orphaned node problem,
the successor list maintained for free by shifting the successor's own list, the `r = log₂N`
argument with `r` entering **as an exponent** (`(1/2)^r = 1/N`, so ~1 orphan expected even after
half the network dies at once), and the coupling between `r` and the stabilisation period, since
"simultaneously" means "within one period". The payoff: **the successor list is the replica
set**, so failover needs no data movement and no coordination — the routing repair *is* the
failover. The volunteered cost: replicas are placed by ring adjacency, which is
`SHA-1(ip:port)` and deliberately not chooseable, so three replicas may share a rack, a switch
and a power strip. Comprehension 2/2. C9: correct that only 2 of 3 copies survive; corrected on
what "fault tolerance" means here — the advertised durability is a **steady-state** property and
the system spends real time below it, so repair *speed* can matter more than replication factor.
C10: he got the shape exactly — "the maths is right, it just does not take this scenario into
account"; sharpened to **correlation destroys the exponent**: if all `r` replicas share a failure
domain, `P(lose all r)` is not `p^r` but `P(the rack dies)`, a single term, and adding replicas
stops helping at all.)

**Session transcripts, 6 Sep.** Claude Code keys its transcripts by working directory, so the
sessions recorded under the old `os-assignment3` path could not be resumed after the move — the
directory they point at no longer exists. All four sessions (22 Aug → 6 Sep) are exported as
readable Markdown to `~/claude-transcripts/chord-dht-filesystem/`, 512 KB, kept **outside** the
repository so they cannot land in a commit by accident.

**Working-style changes agreed 2 Sep, carried into every later session:**
- **No assigned required reading.** Teach the concepts inline; `LEARNING.md` is a lookup index
  and an interview-prep plan, not a to-do list. The deadline is real and the project has to
  exist before it can be defended.
- **Commit messages in Chaitanya's own style** — short lowercase subject, no `fix:`/`docs:`
  prefix, a body only where the *why* is not visible in the diff, and **no `Co-Authored-By`
  trailer**. The four portfolio-era commits were rewritten to match; the ten coursework commits
  were never touched.

**Days to 28 Sep 2026 (hard deadline):** 16 *(as of 12 Sep 2026)*

---

## The deadlines

| Date | What must be true | Status |
|---|---|---|
| 28 Sep 2026 | **HARD.** MVP done and benchmarked, on GitHub, README + architecture diagram + benchmark plots. Resume locks. | ON TRACK |
| early Oct – end Nov 2026 | **Online-assessment season**, multiple companies, ~7 weeks. Project time materially reduced. OAs are never cut | — |
| **23–29 Nov 2026** | **Defence week.** Moved forward from 16–20 Dec | ON TRACK |
| **30 Nov 2026** | **Interview-ready.** Moved forward from 22 Dec | ON TRACK |
| early Dec 2026 onward | **Interviews begin, OAs continue back to back.** Assume zero project time | — |

> **Calendar corrected 12 Sep 2026.** The original plan assumed a months-long gap between the
> assessment window and interviews. **That gap does not exist** — OAs run through October and
> November and interviews start in early December, with both running back to back from then on.
> The old schedule put **defence week three weeks after the first interview**, which is the exact
> failure it exists to prevent. Defence week and interview-ready both move forward by three
> weeks. Option A was chosen over a 16–22 Nov slot: it keeps maximum build time, at the cost of
> only one day of buffer between rehearsal and the first interview.
> **Also corrected in `CLAUDE.md` and `PROMPT.md`.**

---

## Phases

| # | Phase | Week | Budget | Spent | Status | Ends with |
|---|---|---|---|---|---|---|
| 0 | Resurrection and audit | W1 | 6 h | ~6 h | **DONE 9 Sep 2026** — tag `phase-0-complete` | Baseline measured (§1, 71.2 MB/s at 100 MB). Postmortem deferred to W6 by decision |
| 1 | Design forks resolved | W1 | 6 h | | TODO | 5 decisions × 3 documents; diagram in README |
| 2 | Chord routing — finger tables, O(log N) lookup | W2 | 9 h | | TODO | Hop count vs ring size, plotted against log₂N |
| 3 | Node join / leave + stabilisation thread | W3 | 9 h | | TODO | Time-to-reconverge after a kill, measured |
| 4 | Virtual nodes + 3-way successor replication | W4 | 9 h | | TODO | Key-distribution evenness, with and without vnodes |
| 5 | Chunked parallel transfer + deployment kit 1–2 | W5 | 12 h | | TODO | Throughput vs peer count; p50/p99 chunk latency; CI badge |
| 6 | **Freeze at MVP** — documentation only | W6 | 6 h | | TODO | README, architecture diagram, design doc with "alternatives considered" |
| 7–8 | Frozen | W7–W8 | 0 h | | TODO | Touch only if a benchmark number changed |
| 9–10 | Raft tracker design + build; deployment kit 3–5 | W9–W10 | 12 h | | TODO | Leader election measured; Grafana dashboard |
| 11 | Read repair and anti-entropy | W11 | 4 h | | TODO | Staleness window, measured |
| 14 | Fault-injection runs | W14 | 4 h | | TODO | Recovery plots |
| 15 | Dashboard screenshotted into README | W15 | 3 h | | TODO | A screenshot, not a hypothetical |
| 16 | **Defence rehearsal** | W16 | 6 h | | TODO | Weak spots found, listed, then closed |

**Phase 1 budgeted:** 57 h against **70 h available** (weeks 1–8). 13 h slack.
**Phase 2 budgeted:** 29 h against **35 h available** (weeks 9–17). 6 h slack.

W1 is the heaviest project week at **12 h**.

---

## The cut order — decided in advance (R21)

When this slips, cut in **this order and only this order**. Do not re-plan under pressure;
re-planning is how the protected items get cut.

1. **The Raft tracker** (Phase 2, W9–W10)
2. **Deployment kit item 5** — the ring on actual free-tier cloud VMs
3. **High-level-design breadth**
4. **Core-CS breadth**

**Never cut:** algorithm practice · the flagship MVP · the benchmarks · **defence week**.

### The Raft trip-wire — decided 12 Sep, fires on a date not on a feeling

The corrected calendar leaves roughly **7 OA-season weeks at materially reduced hours** for a
Phase 2 budgeted at 29 h. On the face of it that does not fit, and the cut order says Raft goes
first.

**Raft is NOT cut today.** Cutting in September on a projection is re-planning under pressure in
the other direction — the plan has not actually slipped yet, and October's real hours are not yet
known. What is decided today is the **trigger**, so the call fires on a date rather than on how
the project feels in November.

| | |
|---|---|
| **Checkpoint** | **1 Nov 2026** |
| **Test** | Is the Raft tracker *started and demonstrably progressing* — design decided, log replication being built, commits in the last fortnight? |
| **If no** | **Raft is cut. No discussion, no re-planning.** Cut-order item 1, decided in advance for exactly this moment. |
| **If yes** | It continues, and the next thing at risk is cut-order item 2 (cloud VMs). |

**Why 1 Nov:** defence week starts 23 Nov. The work that must sit between Raft and defence week
is read repair (4 h), fault injection (4 h) and the dashboard (3 h) — about three weeks at
OA-season pace. Raft therefore has to be finished by early November to be worth starting, and
1 Nov is the last honest moment to find that out.

**The loss, stated plainly rather than spun:** Raft is the strongest *breadth* item in the
project — the consensus story, and "Chord for the data plane, Raft for the control plane" was
going to be a headline contrast. What survives a cut is the depth story, which is the one that
actually carries a project round.

---

### Stated stretch goal — internet deployment, and possibly a web GUI

Raised 12 Sep 2026. **Below the cut line by his own cut order, and logged here so it competes
openly rather than quietly.**

**What he wants:** deploy the ring so it is a functional file-sharing system over the real
internet, optionally with a web app front end, *if there is time before December*.

**Scheduling verdict — revised 12 Sep, same day, after the calendar was corrected.** The
original answer pointed at an October–November window. **That window does not exist**: October
and November are OA season, and OAs are never cut. December is worse — interviews and OAs run
back to back from early December, and defence week is 23–29 Nov.

So the honest position is that **this probably does not happen**, and if it does it is by
displacing something, below both cut lines. It is recorded here so that if the time ever appears
the thinking is already done — not because it is scheduled.

**Split into two items of very different value:**

1. **Deploy the ring on real VMs and measure it.** High value. It converts several `WEAK`
   deployment entries in `DEFENCE.md` to `SOLID`, and it produces the one number loopback can
   never produce — real network latency per lookup hop. This is already cut-order item 2
   (deployment kit item 5), so it is understood to be optional.
2. **The web GUI.** Decoration on top of (1), and only if (1) is already done and measured. Two
   cautions: it is a new thing to defend, and it is the part of the project a backend or
   distributed-systems interviewer cares least about — R15 warns that the most conspicuous item
   on the page should be the thing he most wants to be asked about, and a screenshot can quietly
   hijack that from the ring.

**Architectural note — this does not reopen F1.** A browser cannot speak raw TCP, so it is never
a ring member: it talks HTTP or WebSocket to a **gateway node**, which is itself a publicly
addressed ring member and runs the lookup. Iterative routing's requirement is that *the
originator* can reach every node directly, and the originator is the gateway. The only case that
would break iterative is ring members behind NAT (home laptops) — and that case breaks the
**data plane** far harder than the control plane, needing STUN/TURN and hole punching, which is a
project of its own and not something recursive routing would rescue.

**What deployment would add, if it happens:** a lookup-result cache on the gateway, short TTL.
A stale entry costs one wasted hop and self-corrects, because a bad routing hint can only cost
hops and never correctness (the finger table's self-validation property, taught in 7.3). That
reclaims most of iterative's latency gap without changing the routing model.

A failed online assessment ends the process before anyone reads the resume. A flagship that
cannot be defended under an hour of pressure fails the project round — and with no work
experience there is nothing else to fall back on.

The defence rehearsal is on the protected list. It is the phase that feels least like progress
and matters most. **It is also the one that gets dropped under pressure.**

---

## Manual actions only he can do

These gate the critical path and cannot be done for him.

| # | Action | Why it blocks | Deadline | Status |
|---|---|---|---|---|
| 1 | **Rename the directory** and reopen the folder in VSCode | `os-assignment3` on a resume reads as coursework | end of W1 | **DONE 6 Sep 2026** — now `/home/csharp/projects/chord-dht-filesystem` |
| 2 | **Create the GitHub repository** `chord-dht-filesystem`, decide public/private, `git remote add origin` | There was **no remote at all**, and on 6 Sep that cost a lost afternoon (error log E5) | end of W1 | **DONE 6 Sep 2026** — `github.com/ChaitanyaShah21/chord-dht-filesystem`, `master` and `pre-resurrection` pushed |
| 3 | **Install Docker Desktop** with WSL2 integration | Deployment kit item 1. Images must be **arm64** — this machine is aarch64. Has a download and a reboot in it | before W5 | TODO |

---

## Weekly tracker

One row per week. The point is to notice a slip in week 2 rather than week 6.

| Week | Dates | Planned | Actually done | Hours | Verdict |
|---|---|---|---|---|---|
| W1 | 24–30 Aug | Get it building and running. Write down what was broken and why. Read the Chord paper. Sketch the target architecture. Resolve the five forks. | Repo hygiene + document set + teaching Parts 1–2 | 1.5 | |
| W2 | 31 Aug–6 Sep | Chord routing: finger tables, O(log N) lookup on a fixed ring. Correctness before joins or failures. | | | |
| W3 | 7–13 Sep | Node join and leave, plus the stabilisation thread that repairs finger tables. | **Phase 0 closed instead** — R6/F9, R9/F10, README correction, E6, baseline measured. Two weeks behind the original plan; see the note below | ~4.5 | Behind plan, honestly logged |
| W4 | 14–20 Sep | Consistent hashing with virtual nodes; three-way successor replication. | | | |
| W5 | 21–27 Sep | Chunked parallel transfer with per-chunk SHA-1 verification. All four benchmarks. Deployment kit 1–2. **Then, in this order:** run benchmarks → write bullets from the real numbers → draw diagrams → write README → cut resume to one page. | | | |
| W6 | 28 Sep–4 Oct | **Freeze at MVP.** Documentation only. | | | |

---

## Open questions

Things not yet decided. These are the **five open forks**, held deliberately open until the
Chord paper has been read — a fork decided before the paper is one that cannot be defended in
December (R11). They move to `ARCHITECTURE.md` § Open Forks and get resolved in Phase 1.

| # | Question | Blocks | Decide by |
|---|---|---|---|
| ~~Q1~~ | ~~Routing — iterative or recursive lookup?~~ | Phase 2 | **RESOLVED 12 Sep — iterative. D-012.** |
| Q2 | The tracker's job — does it know **where chunks are**, or only **what chunks exist**? | Phase 2 | end of W1 |
| Q3 | Replication — sync-to-all-3, write-one-and-propagate, or **quorum W=2 R=2**? This is the consistency/availability knob and the most consequential decision in the design. | Phase 4 | end of W1 |
| Q4 | Failure detection — **stabilisation period alone**, or active successor heartbeats? | Phase 3 | end of W1 |
| Q5 | **Chunk size** — pick a number, then measure throughput at three sizes and let the plot justify it | Phase 5 | number by end of W1, curve in W5 |

**Q3 is the one the project round will land on.**

---

## Audit — defects found 23 Aug 2026, with status

Everything here was **reproduced on this machine**, not read off and assumed. This table is the
raw material for `docs/postmortem-resurrection.md`, which is a deliverable, not setup (R20).

### Build failures — it did not compile at all

| ID | Defect | Status |
|---|---|---|
| B1 | `make` dies instantly: `No rule to make target 'sha1.o'`. `Makefile:14` lists `sha1.cpp` as a source; only header-only `sha1.h` exists, and no `sha1.cpp` ever did. | **FIXED** 1 Sep 2026 — `docs/failures.md` B1 |
| B2 | Link fails with `undefined reference to SHA1`. `sha1.h` calls OpenSSL's `SHA1()`; `CXXFLAGS` has no `-lcrypto`. **Correction to this entry:** it affects `client` only — `tracker.cpp` does not include `sha1.h` and links clean without `-lcrypto`. The original entry was written from reading, not from running. | **FIXED** 1 Sep 2026 — `docs/failures.md` B2 |
| **B5** | **Found 1 Sep 2026, not in the original audit.** Tracker cannot restart inside the TIME_WAIT window: `bind: Address already in use` with **no process listening**. Connections accepted by the previous tracker still hold the local port; `SO_REUSEADDR` was never set. Blocked repeated benchmark runs entirely. | **FIXED** 1 Sep 2026 — `docs/failures.md` B5 |
| B3 | **Committed** `tracker.cpp` at `3c2ff00` did not compile: `broadcast_sync()` declared two `lock_guard<mutex>` variables both named `lock` in the same scope. | FIXED in `7f724c8` (Nov 2025, pre-portfolio) |
| B4 | **Committed** `client.cpp` at `3c2ff00` did not compile: `vector<DownloadTask>::push_back` requires a copy constructor, which the `atomic<size_t>` member deletes. | FIXED in `7f724c8` (Nov 2025, pre-portfolio) |

Reproduce B3/B4 at any time with `git stash && git checkout pre-resurrection~1 && make`.

### Runtime failures — reproduced live

| ID | Defect | Status |
|---|---|---|
| **R1** | **Persistence is dead.** Start tracker on 7100 → `create_group g1` → restart → `list_groups` returns **`No groups`**. Root cause: `tracker.cpp:448` replays the log via `handle_command(cmdline, "", false)` with an **empty `client_user`**, and every recorded command is rejected by its own guard — `if(client_user.empty() \|\| owner != client_user) return "Error: you can only perform this command as yourself"`. `create_group`/`join_group`/`upload_file` additionally require `online_users.count(user)`, and nobody is logged in during replay. **Reproduced 6 Sep 2026** by `scripts/e2e-persistence.sh`: the log file is written **correctly and completely** (6 records, every command present and well-formed) and the restarted tracker still answers `No groups` / `Group not found` / `File not found in group`. Only `create_user` survives, because it is the one mutating command with no `client_user` guard. That the write path is provably fine isolates the fault to replay alone. **Correction to the earlier entry:** the `state_8000.log` that held `create_group g1 alice` six times was a local run from the 23 Aug audit and was never committed (logs are gitignored), so it cannot be re-examined; the scripted reproduction replaces it as the evidence. | **FIXED** 6 Sep 2026 — D-009, `docs/failures.md` R1 |
| **R2** | **Download fails end-to-end and leaves silent corruption.** Root cause **isolated 1 Sep 2026**: `tracker.cpp:233` read `user_info.substr(user.find('@') + 1)` — searching `user` (already stripped to `"alice"`, no `'@'`) while slicing `user_info`. `find` returned `npos`; `npos + 1` **wrapped to 0**; `substr(0)` returned the whole string. The address map held `alice -> alice@127.0.0.1:6881`, `get_file_info` emitted `alice@alice@127.0.0.1:6881`, and `inet_pton` rejected `"alice@127.0.0.1"`. Every peer connection failed before a socket was opened. | **FIXED** 1 Sep 2026 — `docs/failures.md` R2 |
| **R2b** | The *silent-corruption half* of R2, still live: a **failed** download leaves a full-size zero-filled file, because `ftruncate` preallocates and nothing cleans up. Size alone is not evidence of completeness. | **OPEN** — fork **F7** |
| **R3** | **FIXED 2 Sep 2026** (D-007, D-008). Found 1 Sep by the adversarial size sweep, not in the original audit. **Permanent request/response desync after the first successful download.** `client.cpp:822` sends `update_seeder` and never reads the reply; the tracker does not implement `update_seeder`, so it returns `Unknown command` (`tracker.cpp:358`), which the next `recv_line` eats. From then on every reply is one behind — the downloader sizes the destination from one file and verifies against another file's hashes. **Same class as D2 but with no concurrency at all**, which proves a socket mutex was never the answer. | **FIXED** 2 Sep 2026 — `docs/failures.md` R3 |
| **R4** | **Found 1 Sep 2026.** A **zero-byte file cannot be uploaded**: the manifest loop `while ((n = read(...)) > 0)` never executes, `piece_hashes` is empty, and the tracker rejects it with `Error: no piece hashes given`. The client never reads that reply, so it reports success. | **OPEN** — Phase 5 |

| **R5** | **Found 6 Sep 2026**, by splitting admission from effect (D-009). **Replay is not deterministic for `leave_group`.** When the owner leaves, the successor is `*g.members.begin()` — whichever element `unordered_set` yields first, which depends on hashing and insertion history and on nothing that is recorded in the log. A replay may therefore choose a different owner than the live run did, so a recovered tracker can disagree with the one that crashed about who owns a group. **This is the concrete cost of logging the request rather than the effect** (option C of fork F8, rejected in D-009). | **OPEN** — fix is either a deterministic rule (lexicographically smallest member) or the move to an effect log |
| **R6** | **FIXED 9 Sep 2026** (D-010, fork F9). Found 6 Sep by the adversarial pass on the D-009 fix. **Five reply strings contained an embedded newline**, so one command produced two lines and every later reply on that connection was one behind — R3's desync from the opposite direction: R3 was a caller that never consumed its reply, R6 a message that contained the delimiter. Fixed by giving the invariant an owner: `send_all` strips interior delimiters, logs when it must, and appends exactly one terminator; the five strings were corrected so the backstop stays silent in normal use. The client's raw-byte `send_all` was deliberately left alone — it carries piece data full of newlines. | **FIXED** — `scripts/e2e-framing.sh` 11/11; the same suite fails 9/11 against the pre-fix binary |
| **R7** | **Found 6 Sep 2026**, reading the transfer path for Teaching Part 5. **A peer controls how much memory this client allocates.** `download_piece_from_peer` reads the `PIECE <n>` header and immediately does `vector<unsigned char> buffer(piece_size)` (`client.cpp:557`) with `n` taken straight off the wire and never compared against `PIECE_SIZE`. A peer answering `PIECE 99999999999` causes a `length_error`/`bad_alloc` that nothing catches, and the downloading client dies. The size is knowable — it is `min(PIECE_SIZE, filesize - offset)` — so the header should be *checked*, not trusted. | **OPEN** — Phase 5 |
| **R8** | **Found 6 Sep 2026**, same pass. **No socket in the client has a timeout.** `grep` finds one `setsockopt` in `client.cpp` and it is `SO_REUSEADDR` on the listening socket. A peer that completes the TCP handshake and then sends nothing blocks a download worker in `recv` **for ever**; with four workers, four such peers hang the transfer permanently with no error and no progress. This is the "slow rather than dead" case that fork F4 is about, arriving early on the data plane. | **OPEN** — Phase 5, and it is the reason F4 cannot be answered with "stabilisation alone" on the transfer path |

| **R9** | **FIXED 9 Sep 2026** (D-011, fork F10). Found while walking through `send_all` for teaching. **Any client could kill the tracker by disconnecting abruptly.** Neither binary ignored `SIGPIPE`, so a `send` to a departed peer raised it and its default disposition terminated the process — the whole process, not the connection's thread, because a signal disposition is process-wide. Reproduced at **exit 141 = 128 + 13**; the `if(n <= 0)` error check was already correct and simply never ran. Fixed with `signal(SIGPIPE, SIG_IGN)` in both `main`s. | **FIXED** — `scripts/e2e-hangup.sh`, which checks the tracker is still *serving*, not merely alive |

### Claimed but never implemented

| ID | Defect | Status |
|---|---|---|
| C1 | `readme.md` documented `./tracker 5001 127.0.0.1:5000` for multi-tracker synchronisation. `main` reads only `argv[1]`. `peer_addrs` is never populated. **`connect_to_peer()` is defined and never called.** `broadcast_sync` iterates an always-empty `peer_sockets`. The feature has never existed. | WON'T FIX — superseded by the Raft tracker (Phase 2). `readme.md` deleted; the false claim must not reappear in the new README. |

### Found by reading — not Phase 0 work, logged so they are never re-found

| ID | Defect | Disposition |
|---|---|---|
| D1 | **Dangling reference.** `active_downloads.emplace_back(...)` then `DownloadTask &task_ref = active_downloads.back()` (`client.cpp:801-804`). A second concurrent download reallocates the vector and `task_ref` becomes a use-after-free. | Adversarial check in Phase 0; likely obsolete after the Chord rewrite |
| D2 | **Protocol desync.** `seeder_heartbeat_thread` writes `update_seeder` down the **same socket** the main request/response loop uses, every 30 s. Nobody reads the reply, so the main loop's next `recv_line` eats it. `sock_mtx` makes each *send* atomic; the invariant that matters is *send-then-receive*, which no lock protects. **The junk filters at `tracker.cpp:390-401`** (discard lines starting `LOGIN_SUCCESS`/`Groups:`, discard anything under 2 chars, comment: *"ignore random single-character junk"*) **are scar tissue from this bug.** | → `DEFENCE.md`. Strong "tell me about a bug you found" *and* "when was a lock not enough" answer |
| D3 | **Directory traversal.** `GET_PIECE` does `open(filename)` on a **requester-supplied path** (`client.cpp:334`). A peer can request `../../etc/passwd` and be served it. | → `DEFENCE.md` under security. Fix when the transfer layer is rewritten in Phase 5 |
| D4 | **Unhandled exception kills the tracker.** `stoull(size_str)` on hostile input throws inside a detached thread → `std::terminate`. One malformed `upload_file` takes the whole tracker down. | → `DEFENCE.md`. Fix in Phase 5 |
| D5 | **One `recv()` syscall per byte** in `recv_line` (`client.cpp:183`). ~60 kernel round-trips to read a 60-byte response. | Candidate before/after benchmark |
| D6 | **`show_downloads` never worked.** `prepare_command_for_tracker` appends the username, so the client-side `line == "show_downloads"` intercept never matches and it goes to the tracker → `Unknown command`. Reproduced live. | Obsolete after rewrite; note in postmortem |
| D7 | Download **blocks the main loop**, so the coursework's own "concurrent downloads" requirement was never met. | Note in postmortem |
| D8 | `broadcast_sync` re-sends `update_log.back()` for **every** command including read-only ones like `list_groups`, so a read can re-broadcast an unrelated write. | Obsolete — dead code path (see C1) |

---

## Error log

**Every entry here is also an interview answer (R20).** "Tell me about a bug you found and how
you debugged it" is asked constantly and is nearly impossible to answer convincingly from
memory. Write the entry when the bug is fixed, while the detail is still fresh.

### E1 — `make-testdata.sh` aborted after the first file
**Date:** 23 Aug 2026 · **Commit that fixed it:** `e5a52fb`

**Symptom:** the generator printed the header, wrote `small.bin`, and exited 0 without writing
the other eight files or the manifest. No error message.

**How it was found:** ran it, counted the output files. Silent partial success, not a crash.

**Root cause:** `set -o pipefail` plus `openssl ... | head -c N`. `head` exits as soon as it
has N bytes; `openssl` then takes `SIGPIPE` on its next write and dies non-zero. `pipefail`
makes the pipeline's status the *rightmost non-zero* one, so a completely normal shutdown was
reported as failure, and `set -e` aborted the script.

**Why it was hard to see:** every individual piece is correct and idiomatic. `set -euo
pipefail` is the recommended bash preamble; `openssl | head -c` is the standard way to take a
fixed number of bytes from a stream. The bug only exists in their *combination*, and it
presents as success because the abort happens after a successful write.

**Fix:** `{ openssl ... || true; } | head -c "$bytes"` — the `|| true` absorbs the SIGPIPE exit
inside the pipeline so `pipefail` sees zero.

**Trade-off accepted:** `|| true` also swallows a genuine openssl failure. Acceptable here
because the caller verifies the output size on the next run and regenerates if it is wrong.

**How it is prevented from recurring:** `gen()` checks `stat -c%s` against the expected byte
count and rewrites the file if it does not match, so a truncated file cannot be silently
reused.

**Tellable in 90 seconds?** YES — it is a good small example of "each part correct, the
composition wrong", which is the shape of most real concurrency bugs too.

---

---

### E2 — the build was fixed, then the second test run failed and the first had passed
**Date:** 1 Sep 2026 · **Commit that fixed it:** see `docs/failures.md` B5

**Symptom:** `bind: Address already in use` on the second and every later run of the end-to-end
test, with **nothing listening on the port**.

**How it was found:** by running the same script twice. The first run passed. A test that only
fails on its second run is the signature worth learning — it means state survived that should
not have.

**Root cause:** the listening socket dies with the process, but the connections it *accepted* do
not; they sit in `FIN-WAIT` then `TIME_WAIT`, still holding the local port. The kernel refuses
to bind a port any socket still occupies. `SO_REUSEADDR` was never set on the tracker.

**Why it was hard to see:** the message says "address already in use", so every instinct is to
hunt for the process holding it. There is none. `ss -ltn` — listening sockets, the flag people
reach for — shows nothing; `ss -tan` shows the lingering connections.

**Fix:** `setsockopt(SO_REUSEADDR)` before `bind`, return value checked.

**Trade-off accepted:** it weakens TIME_WAIT's protection against a delayed packet from an old
connection landing in a new one on the same four-tuple. Sequence numbers make that essentially
impossible in practice, and the alternative is a service that cannot be restarted for 60
seconds — which means it cannot be benchmarked in a loop, and R13 requires that.

**Not to be confused with:** `SO_REUSEPORT`, which allows several *live* listeners to share a
port. `SO_REUSEADDR` still refuses a second live listener.

---

### E3 — one file transferred correctly; seven files revealed the transfer was one reply behind
**Date:** 1 Sep 2026 · **Status:** root cause found, fix is fork F6

**Symptom:** the size sweep across piece boundaries produced this:

```
CASE              EXPECT_SZ     GOT_SZ  VERDICT
empty                     0          -  FAIL
one_byte                  1          1  PASS
minus1               524287          -  FAIL
exact_1piece         524288     524287  FAIL
plus1                524289     524288  FAIL
exact_2piece        1048576     524289  FAIL
multi               3000000    1048576  FAIL
```

**How it was found:** not by the error messages, which say only "failed to download piece 0".
By the **sizes**. Each row received the *previous* row's size. A shift of exactly one is a
stream desync, and the constant offset is the whole diagnosis.

**Root cause:** after a successful download the client sends `update_seeder` and never reads
the reply. The tracker does not implement that command, so it answers `Unknown command`. That
reply stays in the receive buffer and the next `recv_line` consumes it instead of the response
being waited for. Permanently one behind, for the life of the connection.

**Why it was hard to see:** *the first download succeeds.* The desync is caused by that success
and only the second download pays for it. A test that transfers one file passes forever. This
is the entire argument for the adversarial sweep existing (R10).

**Why it is the better version of D2:** D2 is the same broken invariant with a background
heartbeat thread involved, which invites "so add a mutex". R3 has **no second thread at all**.
It proves the invariant that was violated is *send-then-receive as a pair*, and that no lock
expresses that invariant.

**Fix:** deliberately not taken yet — fork **F6** in `ARCHITECTURE.md` (R6, R11).

---

---

### E4 — a test suite that manufactured its own failures
**Date:** 2 Sep 2026

**Symptom:** immediately after fixing R3, the edge sweep failed every case with
`[client] peer-server bind: Address already in use` — a bind error on a port that should have
been free.

**How it was found:** by the diagnostic added the day before. The old code returned silently
from a failed `bind`; the new message named the port and said what the consequence was, so the
cause was visible in the first line of output instead of after an hour of protocol tracing.

**Root cause: in the test harness, not the product.** `cleanup()` killed by PID variables
`APID` and `BPID` that were **never assigned anywhere**. The clients run inside bash process
substitution, so killing the `tail` that feeds them does not kill the client — it stays alive
holding 6881/6882. The next run then failed at `bind`.

**Why it mattered more than it looks:** the failure was indistinguishable from a real
regression in the code I had just changed. **A test suite that leaks processes manufactures
failures that cost more to diagnose than the bugs it finds** — and it does it at exactly the
moment you are least able to tell the difference.

**Fix:** `reap()` kills by pattern and then waits, up to 10 s, for the ports to actually clear;
it runs both before the suite starts and from the `EXIT` trap. The suite now refuses to start
rather than producing a misleading red.

**The general lesson:** an unhelpful error message is not a cosmetic problem. The silent
`return` on a failed `bind` was fixed as a *diagnostic*, on the explicit grounds that it would
make the next occurrence take a minute instead of an hour. It paid that back within a day.

---

### E6 — two passing test suites failed the moment they ran at the same time
**Date:** 9 Sep 2026 · **Cost:** ~10 min, and a false "the persistence fix regressed" scare

**Symptom:** `e2e-smoke.sh` and `e2e-persistence.sh` were started concurrently to re-verify the
build after the repository move. **Both failed.** Smoke produced no destination file at all;
persistence reported `4/4 facts did not survive the restart (defect R1)` — the exact signature
of the bug D-009 had closed three days earlier. Run one at a time immediately afterwards, on the
same binaries and the same commit, both passed: smoke `PASS`, persistence `4/4`, edge 6/7.

**How it was found:** by noticing that the persistence tracker's own log said
`recovered 6 records` while the client transcript below it was **empty**. Recovery had worked
and the client had never spoken at all — which points at the harness, not at the tracker.

**Root cause: the suites reap by binary path, not by port.** Both call

```sh
pkill -9 -f "$ROOT/tracker"     # every tracker on this machine
pkill -9 -f "$ROOT/client"      # every client on this machine
```

at startup and again from their `EXIT` trap. The ports were deconflicted — smoke uses
7100/6881/6882, persistence 7101/6883/6884 — so the collision check that was actually run found
nothing. **Port isolation is not isolation when the cleanup step has a machine-wide side
effect.** Each suite killed the other's processes mid-run.

**Why it matters more than a harness bug.** The failure is indistinguishable from a product
regression: a missing file and a state-loss message that names a real, previously-open defect.
A test harness whose failure mode imitates the bug it is testing for is worse than no harness,
because it spends the debugging budget in the wrong place. It also means the suites cannot be
parallelised in continuous integration as written, which is where this would have bitten next.

**Fix (applied 9 Sep 2026):** match the port as well as the binary —
`pkill -9 -f "$ROOT/tracker $TRACKER_PORT"` — and derive the peer ports from the same variable,
so a suite can only ever kill its own processes. The feeder files were renamed
`a_${TRACKER_PORT}.in` for the same reason: the `tail -f` reaper matched every suite's feeder
because they all used identical relative filenames. **Proven by running smoke and persistence
concurrently: both pass.**

**Interview answer it feeds:** *"Tell me about a test that lied to you."* Two green suites, run
together, both red, no code changed — and the cause was a cleanup routine written for a machine
running one suite at a time.

### E5 — the machine returned files that were the right size and full of nothing
**Date:** 6 Sep 2026 · **Cost:** one commit, one afternoon's implementation, ~2 h to rebuild

**Symptom:** every git command failed with `error: object file .git/objects/64/5d3d… is empty`
and `fatal: bad object HEAD`. `tracker.cpp` existed, was listed at its normal path, and
contained **zero bytes**. So did `tracker` and `tracker.o`. The session transcript stopped
mid-line.

**How it was found:** by not trusting the report. The starting point was "there was more work
done but I can't see the chat", which sounds like a display problem. `git log` was the first
command run, and it failed — which moved the problem from the interface to the disk.

**Root cause: an unclean WSL2 shutdown, and git's default durability.** `uptime` said the
instance had been up 8 minutes. Earlier the same afternoon `nanosleep` had stopped firing on
this host, and the documented remedy — recorded in this very file — is `wsl --shutdown`. That
shutdown did not flush the page cache. When a program writes a file, the data sits in kernel
memory and reaches the disk seconds later; the file's **size** can be committed before its
**contents**. Kill the machine in that window and the file returns at full length, full of
zeros. Six artefacts show the same signature: eleven git objects, three build files, the tail
of `.git/logs/HEAD` as NUL bytes, and one torn line in the session transcript.

Git made it worse than it had to be. **It does not `fsync` loose objects by default** — it
writes them and trusts the kernel. That is exactly the distinction taught in Teaching Part 4
about the tracker's own log: an `ofstream` write survives a *process* crash and does not
survive a *power* loss. The lesson arrived from the environment rather than from the material.

**Why the loss was survivable:** because the design was written down separately from the code.
`ARCHITECTURE.md` (D-009), `DEFENCE.md`, `docs/failures.md` and `.git/COMMIT_EDITMSG` between
them named every function, every deletion, the soft-state rule, and both defects the work had
uncovered. Rebuilding was transcription against a specification, not redesign. **The documents
that exist for the interview turned out to be the backup.**

**What was checked before concluding it was unrecoverable:** git's loose objects and index,
VSCode's local history (last entry Nov 2025 — the file was written by tooling, not the editor),
Claude Code's file-history store, and a filesystem-wide search for any other copy. The last
intact commit, `66ea0ff`, still held a complete `tracker.cpp`, so the base to rebuild from was
real.

**Fix:** point `master` at `66ea0ff`, delete the eleven unreadable objects, rebuild the index
from the commit (`rm .git/index && git reset --mixed 66ea0ff` — the stale index still referenced
the deleted objects and blocked the reset), restore `tracker.cpp` from the intact blob, then
re-apply D-009 from the documents.

**Trade-off accepted:** moving the branch discards the lost commits' metadata permanently. The
alternative — grafting onto unreadable parents — produces a repository that fails `fsck` for
ever. A full copy of the directory was taken before anything was touched.

**How it is prevented from recurring:** two changes, both overdue.
`git config core.fsync loose-object,index,reference` makes git wait for the disk, at a small
cost per commit. And **manual action #2 — a GitHub remote — stopped being a to-do item.** The
entry in this file already said "nothing is backed up, and the commit history is itself
evidence". It has now cost real work.

**Tellable in 90 seconds?** YES, and it is the best one available: a durability failure diagnosed
from the *shape* of the damage rather than from any error message — every affected file the
right length and full of zeros — plus the reason git was vulnerable to it, and a recovery that
worked because the design was documented separately from the code.

---

## Where things live

| What | Path |
|---|---|
| Source | `tracker.cpp`, `client.cpp`, `sha1.h` (repo root — to be split into `src/` in Phase 2) |
| Build | `Makefile` (repo root) — **currently broken, see B1/B2**, and never committed |
| Test data generator | `scripts/make-testdata.sh` |
| Test data (gitignored) | `testdata/` — regenerate with `./scripts/make-testdata.sh` |
| Tests | `scripts/e2e-smoke.sh` (happy path, R2 regression) · `scripts/e2e-edge.sh` (size sweep across piece boundaries, R3/R4) · `scripts/e2e-persistence.sh` (restart survival, R1 — green) |
| Benchmark harness | not yet — Phase 0, with the baseline measurement |
| Figures | `docs/figures/` — not yet |
| Build output (gitignored) | `tracker`, `client`, `*.o` at repo root |
| Last coursework-era state | tag `pre-resurrection` (`7f724c8`) |
