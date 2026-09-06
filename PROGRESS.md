# Progress — chord-dht-filesystem

Read this first in every session (R16), together with `ARCHITECTURE.md` and anything marked
`WEAK` in `DEFENCE.md`.

---

## Where we are right now

**Phase:** 0 — Resurrection and audit
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
**Next step:** **implement F8/option B and turn `e2e-persistence.sh` green.** After that,
decide fork **F7** (zero-filled file left by a failed download), then take the **baseline
throughput number** that closes Phase 0.
**Blocked on:** nothing. F7 is Chaitanya's call when we reach it (R6).

**Teaching progress (fresh pass, 2 Sep):** Part 1 system shape ✅ · Part 2 the wire ✅ ·
Part 3 tracker state, data structures and locking ✅ (all comprehension checks answered and
graded). **Part 4 — persistence and the replay path — is next, and pairs with the R1 fix.**
Remaining planned: Part 5 the transfer path, Part 6 the latent defects, Part 7 Chord.

**Working-style changes agreed 2 Sep, carried into every later session:**
- **No assigned required reading.** Teach the concepts inline; `LEARNING.md` is a lookup index
  and an interview-prep plan, not a to-do list. The deadline is real and the project has to
  exist before it can be defended.
- **Commit messages in Chaitanya's own style** — short lowercase subject, no `fix:`/`docs:`
  prefix, a body only where the *why* is not visible in the diff, and **no `Co-Authored-By`
  trailer**. The four portfolio-era commits were rewritten to match; the ten coursework commits
  were never touched.

**Days to 28 Sep 2026 (hard deadline):** 36

---

## The deadlines

| Date | What must be true | Status |
|---|---|---|
| 28 Sep 2026 | **HARD.** MVP done and benchmarked, on GitHub, README + architecture diagram + benchmark plots. Resume locks. | ON TRACK |
| early Oct 2026 | Online-assessment window opens; project freezes for documentation only | ON TRACK |
| 16–20 Dec 2026 | Defence week — a full hour of someone attacking the architecture | ON TRACK |
| 22 Dec 2026 | Interview-ready | ON TRACK |

---

## Phases

| # | Phase | Week | Budget | Spent | Status | Ends with |
|---|---|---|---|---|---|---|
| 0 | Resurrection and audit | W1 | 6 h | 1.5 h | IN PROGRESS | Baseline throughput number + `docs/postmortem-resurrection.md` |
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
| 1 | **Rename the directory** `/home/csharp/os-assignment3` → `/home/csharp/chord-dht-filesystem` and reopen the folder in VSCode | `os-assignment3` on a resume reads as coursework | end of W1 | TODO |
| 2 | **Create the GitHub repository** `chord-dht-filesystem`, decide public/private, `git remote add origin` | There is **no remote at all** today. Nothing is backed up, and the commit history — which is itself evidence — exists on one laptop | end of W1 | TODO |
| 3 | **Install Docker Desktop** with WSL2 integration | Deployment kit item 1. Images must be **arm64** — this machine is aarch64. Has a download and a reboot in it | before W5 | TODO |

---

## Weekly tracker

One row per week. The point is to notice a slip in week 2 rather than week 6.

| Week | Dates | Planned | Actually done | Hours | Verdict |
|---|---|---|---|---|---|
| W1 | 24–30 Aug | Get it building and running. Write down what was broken and why. Read the Chord paper. Sketch the target architecture. Resolve the five forks. | Repo hygiene + document set + teaching Parts 1–2 | 1.5 | |
| W2 | 31 Aug–6 Sep | Chord routing: finger tables, O(log N) lookup on a fixed ring. Correctness before joins or failures. | | | |
| W3 | 7–13 Sep | Node join and leave, plus the stabilisation thread that repairs finger tables. | | | |
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
| Q1 | Routing — **iterative or recursive** lookup? | Phase 2 | end of W1 |
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
| **R1** | **Persistence is dead.** Start tracker on 7100 → `create_group g1` → restart → `list_groups` returns **`No groups`**. Root cause: `tracker.cpp:448` replays the log via `handle_command(cmdline, "", false)` with an **empty `client_user`**, and every recorded command is rejected by its own guard — `if(client_user.empty() \|\| owner != client_user) return "Error: you can only perform this command as yourself"`. `create_group`/`join_group`/`upload_file` additionally require `online_users.count(user)`, and nobody is logged in during replay. **Reproduced 6 Sep 2026** by `scripts/e2e-persistence.sh`: the log file is written **correctly and completely** (6 records, every command present and well-formed) and the restarted tracker still answers `No groups` / `Group not found` / `File not found in group`. Only `create_user` survives, because it is the one mutating command with no `client_user` guard. That the write path is provably fine isolates the fault to replay alone. **Correction to the earlier entry:** the `state_8000.log` that held `create_group g1 alice` six times was a local run from the 23 Aug audit and was never committed (logs are gitignored), so it cannot be re-examined; the scripted reproduction replaces it as the evidence. | **OPEN** — fix is F8/option B, in progress |
| **R2** | **Download fails end-to-end and leaves silent corruption.** Root cause **isolated 1 Sep 2026**: `tracker.cpp:233` read `user_info.substr(user.find('@') + 1)` — searching `user` (already stripped to `"alice"`, no `'@'`) while slicing `user_info`. `find` returned `npos`; `npos + 1` **wrapped to 0**; `substr(0)` returned the whole string. The address map held `alice -> alice@127.0.0.1:6881`, `get_file_info` emitted `alice@alice@127.0.0.1:6881`, and `inet_pton` rejected `"alice@127.0.0.1"`. Every peer connection failed before a socket was opened. | **FIXED** 1 Sep 2026 — `docs/failures.md` R2 |
| **R2b** | The *silent-corruption half* of R2, still live: a **failed** download leaves a full-size zero-filled file, because `ftruncate` preallocates and nothing cleans up. Size alone is not evidence of completeness. | **OPEN** — fork **F7** |
| **R3** | **FIXED 2 Sep 2026** (D-007, D-008). Found 1 Sep by the adversarial size sweep, not in the original audit. **Permanent request/response desync after the first successful download.** `client.cpp:822` sends `update_seeder` and never reads the reply; the tracker does not implement `update_seeder`, so it returns `Unknown command` (`tracker.cpp:358`), which the next `recv_line` eats. From then on every reply is one behind — the downloader sizes the destination from one file and verifies against another file's hashes. **Same class as D2 but with no concurrency at all**, which proves a socket mutex was never the answer. | **FIXED** 2 Sep 2026 — `docs/failures.md` R3 |
| **R4** | **Found 1 Sep 2026.** A **zero-byte file cannot be uploaded**: the manifest loop `while ((n = read(...)) > 0)` never executes, `piece_hashes` is empty, and the tracker rejects it with `Error: no piece hashes given`. The client never reads that reply, so it reports success. | **OPEN** — Phase 5 |

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

## Where things live

| What | Path |
|---|---|
| Source | `tracker.cpp`, `client.cpp`, `sha1.h` (repo root — to be split into `src/` in Phase 2) |
| Build | `Makefile` (repo root) — **currently broken, see B1/B2**, and never committed |
| Test data generator | `scripts/make-testdata.sh` |
| Test data (gitignored) | `testdata/` — regenerate with `./scripts/make-testdata.sh` |
| Tests | `scripts/e2e-smoke.sh` (happy path, R2 regression) · `scripts/e2e-edge.sh` (size sweep across piece boundaries, R3/R4) · `scripts/e2e-persistence.sh` (restart survival, R1 — **currently red on purpose**) |
| Benchmark harness | not yet — Phase 0, with the baseline measurement |
| Figures | `docs/figures/` — not yet |
| Build output (gitignored) | `tracker`, `client`, `*.o` at repo root |
| Last coursework-era state | tag `pre-resurrection` (`7f724c8`) |
