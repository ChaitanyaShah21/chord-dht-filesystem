# Postmortem: from coursework to distributed storage

How an operating-systems course assignment that did not build became a measured, replicated
storage system — what was broken, how it was found, and what each finding changed. The individual
defects, with reproductions, are in [`failures.md`](failures.md); the decisions are D-001 to D-011
(and onward) in [`ARCHITECTURE.md`](../ARCHITECTURE.md). Everything here happened, and the git
history shows it: none of it has been rewritten or squashed.

## What there was

A BitTorrent-style file sharer written for an operating-systems course in the autumn of 2025, in
C++ with POSIX sockets and threads: a **tracker** holding users, passwords, groups with owners and
join requests, and for every shared file its piece hashes and the list of peers seeding it; and a
**client** that logged in, uploaded a file's manifest, served pieces to other peers from a peer-server
thread, downloaded pieces from seeders with a small worker pool, and re-announced itself every 30
seconds from a heartbeat thread.

Ten commits on 1–2 October 2025 (`dc14232` … `3c2ff00`). Then 2,247 lines of work in November that
were never committed.

## What was wrong with it, on 23 August 2026

- **It did not build.** The last commit had two compile errors (B3, B4). The uncommitted November
  tree fixed those — and silently deleted three working handlers (`stop_share`, `update_seeder`,
  `peer_reconnect_thread`). Even then, `make` failed at once on a rule for a `sha1.o` that had never
  existed (B1), and the client failed to link against OpenSSL (B2).
- **The README claimed a feature that did not exist.** Multi-tracker synchronisation was described,
  and `connect_to_peer()` was defined and never called.
- **An audit by reading the code** listed twenty defects and two structural weaknesses: the tracker
  was a central index of every holder of every file, and a file vanished when its seeders left.

## What was done first, and why that order

**Commit the November tree as it was, then fix forward** (D-003). Tagged `pre-resurrection`
(`cd10483`). Reconstructing the deleted handlers first would have produced a tidier diff that told a
story about November written in August, and hidden that the regression happened.

**A minimum viable resurrection, not a full repair** (D-002): fix the build, fix the download path,
and stop when one file transfers and verifies — because the point was a **baseline number** every
later improvement would be measured against, and most of the other defects lived in code the
redesign would delete.

**Drop users, groups and authentication** (D-004) — but only *after* the baseline was measured
through them, because the download path ran through the group checks.

## What reproducing it found

Every defect below was reproduced before it was fixed, and two hypotheses on the way were refuted and
kept in the log.

| When | Defect | What it was | Lesson |
|---|---|---|---|
| 1 Sep | B1, B2 | a phantom `sha1.o`; `-lcrypto` missing at link time | compile errors name files, link errors name symbols |
| 1 Sep | B5 | the tracker could not restart for 60 s: accepted connections lingered in `TIME_WAIT` and `SO_REUSEADDR` was never set | found by running the smoke test twice |
| 1 Sep | **R2** | every download failed and left a full-size file of zeros. A wrong-variable typo (`user.find('@')` instead of `user_info.find('@')`) returned `npos`; `npos + 1` wrapped to 0; `substr(0)` returned the whole string; peers were advertised as `alice@alice@127.0.0.1` | found by speaking each protocol by hand, bisecting the pipeline |
| 1 Sep | **R3** | after the first download every reply was one behind for the life of the connection: the client sent `update_seeder`, which the tracker did not implement, and never read the reply | found by an adversarial size sweep — the received sizes were shifted by exactly one row |
| 1 Sep | R4 | a zero-byte file could not be uploaded | pinned by a test committed failing on purpose (D-006) |
| 2 Sep | D2 | the heartbeat thread wrote down the main loop's socket | a mutex protects state; this was a rule about sequence (D-008) |
| 6 Sep | **R1** | nothing survived a tracker restart: recovery replayed the log through the live handler, whose authorisation checks refused every command on behalf of nobody | split admission from effect, so recovery cannot reach a guard (D-009) |
| 9 Sep | R6 | five replies contained a newline, so one command produced two lines | the framing layer now owns the one-reply-one-line rule (D-010) |
| 9 Sep | **R9** | any client could kill the whole tracker by hanging up: `SIGPIPE` | the error handling was correct and never ran (D-011) |

Two problems were in the **test harness**, not the product, and cost more to diagnose than some
product bugs: cleanup killed by PID variables that were never assigned, so a client survived holding
its port and the next run failed at `bind`; and the suites reaped processes by binary name, so two
suites run together killed each other's processes — a failure that looked exactly like a regression.
Both now kill only their own processes, matched by port.

The machine contributed too. A WSL restart discarded writes still in the page cache, leaving source
files and git objects zero bytes long; the R1 fix was rebuilt from its written record and re-verified.
The host's `nanosleep` was for a while unreliable, which is why the R1 fix was checked by a transcript
comparison with blocking reads and no `sleep` at all. Every benchmark since records a timer sanity
check first.

## The baseline

`e381179`, 9 September 2026: one seeder, one downloader, 512 KB pieces, all on one host —
**71.2 MB/s for 100 MB**, sequential, because one seeder gave the client's worker pool exactly one
worker (`BENCHMARKS.md` §1). The first version of the harness reported 165 MB/s for 1 MB: it timed
the client's `ftruncate` of the destination, not the transfer. **Size is never evidence of
completeness** — the harness now waits for the client's own "every piece verified".

## From file sharing to storage

The redesign was decided before any of it was built, one recorded fork at a time (D-012 to D-022):

- **Iterative lookups** (D-012), so the client counts its own hops and knows exactly which node
  failed.
- **Content-addressed chunks** (D-014): a chunk's key is the hash of its bytes, so replicas cannot
  disagree and reads verify themselves.
- **Three copies, `W = 2`, `R = 1`** (D-015), with `W` a runtime parameter so its cost could be
  measured.
- **A tracker that holds only names** (D-017): a chunk's location is computed, so the tracker stops
  being an index of who holds what.
- **Ring members as their own processes**, routing as pure functions (D-018), so the logic could be
  tested without sockets.

Together those made it **storage, not a swarm** (D-022): placement by hash, a fixed replica count,
nothing recording who holds what, and clients that do not serve. The trade is that a popular file no
longer gains sources; in exchange a file survives every one of its uploaders going offline.

Then it was built and measured in order — routing (§2), joins and failure handling (§4), chunk
storage and replication (§6), files through the ring (§5). In October the coursework client and
tracker were removed from the tree (D-060). The regression suites written for R1, R2, R4, R6 and R9
kept their names and purpose and now run against the programs that replaced them; the baseline is
re-run from its own commit, and was, on the day the new path was measured: **378 MB/s against
70.1 MB/s at 100 MB, 3.7× of it from parallelism alone**.

## What carried over

- **Make the bug unreachable, not avoidable.** The replay flag, the five reply strings and the
  per-call `MSG_NOSIGNAL` were each rejected for the same reason: they depend on someone remembering.
  The same move recurs throughout the design — content addressing removes the conflict problem
  rather than solving it.
- **Reproduce before fixing; keep the refuted hypotheses.**
- **Commit a failing test that pins a known defect** rather than wait until it can pass.
- **A before number cannot be recovered afterwards.** It was taken first.
