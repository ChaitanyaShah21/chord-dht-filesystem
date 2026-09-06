# Authorship — chord-dht-filesystem

Two jobs. First, honesty about how the project was built. Second, and more usefully: a standing
check on which parts of this repository Chaitanya could currently explain in an interview.

**The rule:** anything he cannot explain at 11pm on no sleep either gets learned or gets cut.
There is no third option, and "it's in the repo" is not a defence. Every line of a resume gets
attacked, and a keyword that cannot be defended is worse than an absent one.

---

## Legend

| Mark | Meaning |
|---|---|
| **H** | Human-written |
| **AH** | AI-generated, then substantially edited by hand |
| **A** | AI-generated, reviewed and understood, unmodified |
| **X** | External — library, vendored code, course-supplied starter |

| Defence | Meaning |
|---|---|
| ✅ | Can explain it cold, line by line, including why it is built this way |
| ⚠️ | Can explain what it does, not yet why it is built this way |
| ❌ | Cannot currently defend — **must be learned or removed before the resume goes out** |

---

## Files

| File | Author | Defence | Notes |
|---|---|---|---|
| `tracker.cpp` | **H** | ⚠️ | Written Sep–Nov 2025. Re-taught 23 Aug 2026 (Parts 1–2: system shape, framing). The R2 address-map fix and `SO_REUSEADDR` are ✅ — both were reproduced, isolated and written up by hand. **Not yet covered:** data structures, the three mutexes and their ordering, `handle_command`'s guard, the replay path in `main`. Part 3, in progress 1 Sep 2026. |
| `client.cpp` | **H**, announcer rewritten **AH** | ⚠️ | Same. Framing and the desync are ✅ — R3 was found, diagnosed and fixed by hand, and the `condition_variable` + generation-counter announcer is understood (predicate guards against both a lost notify and a spurious wake). **The transfer path is now covered** (Part 5, 6 Sep): manifest construction and the short last piece, the `SEEDERS` sentinel in `FILE_INFO` and why it survives only because hashes are hex, `ftruncate` preallocation and sparse files, the `fetch_add` work queue, the `while (total < piece_size)` short-read loop, and length-prefixed framing for data against line framing for control. **Not yet covered:** `DownloadTask`'s move constructor. |
| `sha1.h` | **H** wrapper over **X** | ⚠️ | The wrapper is trivial and understood. **The library internals are ❌** — see below. |
| `Makefile` | **H**, repaired **AH** | ⚠️ | B1 and B2 fixed 1 Sep 2026. **Can explain:** why there is no `sha1.o` (header-only, all `inline`), and why `-lcrypto` is a *link*-time flag on the client rule only. **Not yet covered:** why the explicit `$(CLIENT_OBJ): sha1.h` line matters. |
| `.gitignore` | **A** | ✅ | Skill template, project binaries named by hand. |
| `scripts/e2e-smoke.sh`, `scripts/e2e-edge.sh` | **A** | ⚠️ | Written 1 Sep 2026. **The reason `e2e-edge.sh` exists is ✅** — a single-file test cannot see R3, because the desync is *caused by* the first success. **Not yet covered:** the `tail -f` driving trick and why `disown` is there. |
| `scripts/make-testdata.sh` | **A** | ⚠️ | Generated 23 Aug 2026. The `pipefail`/`head`/`SIGPIPE` interaction is understood and written up as `PROGRESS.md` E1. The AES-CTR-for-determinism trick is understood in principle; **not yet able to explain CTR mode itself.** |
| `CLAUDE.md`, `PROGRESS.md`, `ARCHITECTURE.md`, `DEFENCE.md`, `BENCHMARKS.md`, `GLOSSARY.md`, `LEARNING.md`, `SCALE_NOTES.md`, `AUTHORSHIP.md` | **A** from skill templates, filled from a real audit | n/a | Working documents, not deliverables. The **audit findings inside them were reproduced on this machine**, not assumed. |
| `README.md` | **X** (coursework-era) | ❌ | **Claims multi-tracker synchronisation that has never existed** (defect C1). Must be replaced before the repository is public — this is currently the single most dangerous file here. |

**Everything in `tracker.cpp` and `client.cpp` is hand-written by Chaitanya during the
coursework period.** The portfolio work begins at commit `7f724c8`. That distinction is real and
is worth stating plainly rather than blurring.

---

## Anything marked ❌

The working list. Each entry gets a date, because an undated gap is one that survives to
December.

| File / concept | Why it is a risk | Plan | By when |
|---|---|---|---|
| `README.md` claims a feature that does not exist | **A reviewer who runs it and finds the claim false is the worst possible outcome** — it converts "inexperienced" into "not trustworthy" | REMOVE and rewrite | **before the repository is made public** — W1 |
| OpenSSL SHA-1 internals | "Explain the internals of the libraries you used" is asked directly in project-design rounds | LEARN — block structure, Merkle–Damgård, why collision resistance being broken does not matter here | W1, with the `GLOSSARY.md` entry |
| Chord, consistent hashing, finger tables | The headline of the project | LEARN — `LEARNING.md` § Chord | W1 read, W2 build |
| Raft, quorums, read repair | Phase 2 keywords | LEARN — **not before W9.** Cut-order item 1 | W9 |

---

## Libraries used, and what he knows about their internals

"Explain the internals of the libraries you used" is asked directly in project-design rounds.
Using a library is fine; not knowing roughly how it works is not.

| Library | Used for | What I know about how it works internally | Depth |
|---|---|---|---|
| OpenSSL `libcrypto` — `SHA1()` | Per-piece and whole-file fingerprints | 160-bit digest, 512-bit input blocks, Merkle–Damgård construction, 80 rounds over five 32-bit registers, message length appended as padding. **Collision resistance is broken (SHAttered, 2017); pre-image resistance is not.** That is why it is still acceptable here — this detects corruption, not a chosen-input attack. On aarch64 OpenSSL uses ARMv8 crypto extensions. | ⚠️ — the round structure is stated, **not yet verified**. Verify before claiming it. |
| C++ Standard Library — `std::thread`, `std::mutex`, `std::lock_guard`, `std::atomic` | All concurrency | `lock_guard` is RAII over `pthread_mutex_lock`/`unlock`. `std::thread` wraps `pthread_create`; `detach` severs the join relationship so the thread cleans up its own resources. `atomic<size_t>::fetch_add` compiles to a single LSE atomic instruction on ARMv8. | ⚠️ — solid on `lock_guard` and `detach`; **the memory model and `memory_order` are ❌** and deliberately not claimed anywhere |
| POSIX sockets | All networking | Not a library so much as syscalls: `socket`/`bind`/`listen`/`accept`/`connect`/`send`/`recv`. The important internal fact is the **kernel socket buffer** — which is why a short read is normal and why framing is the caller's job. | ✅ |

**Deliberately not used, and why — this is itself an answer:** no Boost.Asio, no Protocol
Buffers, no gRPC, no serialisation library. The constraint came from the original coursework,
and it is worth keeping: hand-written framing is exactly what the C and operating-systems round
asks about, and "I wrote the wire protocol myself" is a stronger position than "the library
handled it". Optional gRPC for the control plane is a Phase 2 item **specifically so that "why
not gRPC?" has a real, comparative answer** rather than a defensive one.

---

## How this project was built

A short, honest paragraph, written once at the end. Assistance is normal and unremarkable; being
unable to explain the result is the only thing that actually costs anything.

*To be written at the end of Phase 2.* The facts it will have to state, recorded now so they are
not quietly softened later: the socket and threading layer is hand-written from the coursework
period; the Chord, replication and stabilisation layers are built with AI assistance in a
teaching mode where every concept is explained before any code is written and every design fork
is chosen by hand; the benchmark numbers are all measured on the machine described in
`BENCHMARKS.md`; and the working documents in this repository are AI-drafted from an audit whose
findings were reproduced, not assumed.
