# Failure Log — what was broken, how it was found, why the fix works

**What this file is.** One entry per defect that was actually reproduced on this machine,
written while the detail was fresh. It is a deliverable, not housekeeping (R20): *"tell me
about a bug you found and how you debugged it"* is asked in almost every round, and it is
nearly impossible to answer convincingly from memory.

**The rule that keeps it honest:** nothing goes in this file that was not reproduced. Reading
code and forming a theory is a *hypothesis*; it is written down as a hypothesis and marked
CONFIRMED or REFUTED only after it has been run. Two hypotheses in this file are marked
REFUTED, deliberately — a log that only records the guesses that turned out right is a log
that has been edited after the fact, and it reads that way.

**Entry format.** Symptom → how it was found → root cause → why it was hard to see → fix →
trade-off accepted → how recurrence is prevented → the interview question it answers.

| ID | Defect | Severity | Status |
|---|---|---|---|
| B1 | `make` fails instantly: no rule to make target `sha1.o` | Blocker | **FIXED** |
| B2 | `client` fails to link: undefined reference to `SHA1` | Blocker | **FIXED** |
| B5 | Tracker cannot restart within the TIME_WAIT window (`EADDRINUSE`) | High | **FIXED** |
| R2 | Every download fails; a full-size file of zeros is left on disk | Critical | **FIXED** (root cause) |
| R3 | Permanent request/response desync after the first successful download | Critical | **FIXED** 2 Sep 2026 |
| R4 | A zero-byte file cannot be uploaded at all | Medium | **OPEN** |
| R2b | A failed download still leaves a full-size zero-filled file behind | High | **OPEN** — fork below |
| R1 | Tracker state does not survive a restart | Critical | **OPEN** — next |

Session date: 1 Sep 2026 · baseline commit `219a9b8` · g++ 13.3.0, C++17, aarch64 under WSL2.

---

## B1 — `make` dies before compiling anything

**Symptom**

```
$ make
g++ -std=c++17 -Wall -pthread -O2   -c -o tracker.o tracker.cpp
make: *** No rule to make target 'sha1.o', needed by 'tracker'.  Stop.
```

**How it was found:** running `make` on a clean checkout. This is the first thing anyone
cloning the repository would hit, which is why it is defect number one.

**Root cause:** the `Makefile` declared

```make
TRACKER_SRC = tracker.cpp sha1.cpp
CLIENT_SRC  = client.cpp sha1.cpp
```

and derived object files with `$(TRACKER_SRC:.cpp=.o)`. That asks `make` to produce `sha1.o`
from `sha1.cpp`. **There is no `sha1.cpp` and there never was.** `sha1.h` is header-only —
every function in it is declared `inline`, so its code is compiled directly into each
translation unit that includes it. There is no separate object file to build, and listing one
made `make` look for a rule it could not find.

**Why it was hard to see:** it is not visibly wrong. A `sha1.h`/`sha1.cpp` pair is the normal
shape, and the `Makefile` reads exactly like every other `Makefile`. The error message names
`sha1.o`, a file nobody ever wrote, so the natural first move is to go looking for what deleted
it rather than to question whether it was ever supposed to exist.

**Fix:** drop `sha1.cpp` from both source lists.

**Trade-off accepted:** header-only means `sha1.h`'s code is compiled once per translation
unit, so it is compiled twice here and the two copies are merged at link time. At two files
this costs nothing measurable. If `sha1.h` ever grows real logic, moving it to a `.cpp` is the
right call — and then this `Makefile` line comes back for a real reason.

**Prevented from recurring:** an explicit dependency line was added —

```make
$(CLIENT_OBJ): sha1.h
```

Without it, editing `sha1.h` leaves the stale `client.o` in place and the next `make` silently
links yesterday's code. That is a nastier bug than this one, because it produces a *wrong
binary* rather than an error.

**Interview question it answers:** *"What is the difference between a header-only library and
one with a separate source file, and when would you pick each?"*

---

## B2 — `client` compiles but does not link

**Symptom**

```
/usr/bin/ld: client.o: in function `upload_file_to_tracker(...)':
client.cpp:(.text+0x5670): undefined reference to `SHA1'
collect2: error: ld returned 1 exit status
```

**How it was found:** after fixing B1, by compiling `client.cpp` to an object file and then
attempting to link it on its own.

**Root cause:** `sha1.h` calls OpenSSL's `SHA1()`. The *declaration* of `SHA1` comes from
`<openssl/sha.h>`, which is present at `/usr/include/openssl/`, so the **compile** step
succeeds. The *machine code* for `SHA1` lives in `libcrypto`, and nothing named that library at
**link** time. `CXXFLAGS` had no `-lcrypto`.

This is the single most useful thing about the error: compiling and linking are separate steps
that fail for separate reasons. A missing header is a compile error and names a file; a missing
library is a link error and names a *symbol*. `undefined reference` always means the second.

**Correction to the original audit:** `PROGRESS.md` recorded B2 as affecting both binaries. It
does not. `tracker.cpp` does not include `sha1.h` and hashes nothing today, so it links with no
`-lcrypto` at all. Verified by linking `tracker.o` alone: it succeeds. The audit entry was
written from reading, not from running.

**Fix:** a separate `CRYPTO_LDLIBS = -lcrypto`, applied to the `client` link rule only.

**Trade-off accepted:** putting `-lcrypto` in `CXXFLAGS` globally would have been one line
shorter and would have worked. It was rejected because it links a cryptography library into a
binary that does not use it, and because `CXXFLAGS` is conceptually compile-time flags —
mixing link-time libraries into it is how a `Makefile` becomes untraceable. The tracker will
need SHA-1 in the Chord phase (hashing node identifiers and keys onto the ring); the flag gets
added to its rule *then*, when it is true.

**Interview question it answers:** *"What is the difference between a compile error and a link
error? Given `undefined reference to X`, what are the three things that could be wrong?"*
(Answers: the library was not named; it was named but in the wrong order relative to the object
that needs it; or the symbol has a different name than expected — C++ name mangling, usually
fixed with `extern "C"`.)

---

## B5 — the tracker cannot be restarted (found during this session, not in the original audit)

**Symptom:** the second and every subsequent run of the end-to-end test failed at startup:

```
bind: Address already in use
```

with **no process listening on that port** — `ss -ltn` showed nothing bound to 7100.

**How it was found:** by accident, by running the smoke test twice in a row. The first run
passed and the second did not, which is the signature worth recognising: *a test that only
fails on the second run is almost never testing what you think it is.*

**How it was isolated:** the temptation was to conclude "a process leaked". That hypothesis was
checked and refuted — `ps` showed no tracker alive. So it was reproduced deliberately instead:
start the tracker, open a real TCP connection to it, `kill -9` the tracker, leave the client end
of the connection open, and immediately try to bind again.

```
--- sockets on 7101 right after kill ---
FIN-WAIT-1 0  1  127.0.0.1:7101   127.0.0.1:34268
CLOSE-WAIT 25 0  127.0.0.1:34268  127.0.0.1:7101
--- immediate rebind attempt ---
bind: Address already in use
```

Deterministic, in two seconds, every time.

**Root cause:** the listening socket is gone with the process, but the *connections it accepted*
are not. They linger in `FIN-WAIT` and then `TIME_WAIT`, and they are still bound to local port
7101. By default the kernel refuses to bind a port that any socket still occupies, so `bind()`
returns `EADDRINUSE`. The tracker never set `SO_REUSEADDR`, which is the option that says
"bind anyway, past sockets that are merely winding down".

TIME_WAIT is not a bug being worked around. It exists so that a delayed duplicate packet from
the old connection cannot be delivered to a new connection that happens to reuse the same
four-tuple. It lasts twice the maximum segment lifetime — on Linux, 60 seconds.

**Why it was hard to see:** the error message is actively misleading. "Address already in use"
strongly implies another program has the port, and every instinct says to go hunting for it.
The port genuinely is in use — by sockets with no process attached, which no process listing
will ever show. `ss -tan` shows them; `ss -ltn` (listening only) does not, which is the flag
most people reach for first.

**Fix:** `setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))` before `bind()`, with the
return value checked. The client's peer server already did this; only the tracker did not.

**Trade-off accepted:** `SO_REUSEADDR` weakens the protection TIME_WAIT provides — a delayed
packet from a previous connection could in principle be delivered to a new one on the same
four-tuple. In practice TCP sequence numbers make this vanishingly unlikely, and the cost of
*not* setting it is that the service cannot be restarted for a minute. Every production server
sets it. The important thing is to know what is being given up rather than to copy the line.

**The distinction that matters, and that gets asked:** `SO_REUSEADDR` is **not**
`SO_REUSEPORT`. `SO_REUSEADDR` allows binding past *lingering* sockets; it will still refuse a
second *live* listener on the same port. `SO_REUSEPORT` is the one that allows several live
listeners to share a port and have the kernel load-balance connections between them.

**Why this was a blocker rather than an annoyance:** every benchmark in `BENCHMARKS.md` has to
be repeatable, and R13 requires a before number and an after number. A service that cannot be
restarted inside 60 seconds cannot be benchmarked in a loop.

**Interview question it answers:** *"Your server won't restart — `Address already in use` — but
nothing is listening on the port. What's happening?"* This is a standard screening question.

---

## R2 — every download fails and leaves a full-size file of zeros

The most valuable entry in this file.

**Symptom:** two clients, one 300 KB file, everything on loopback. The transfer fails:

```
[client] failed to download piece 0 from all peers
Download incomplete: 0 / 1 pieces
```

and a **300,000-byte file consisting entirely of zeros** is left at the destination. The size
is exactly right. The content is entirely wrong. Nothing is printed to stdout.

That combination is worse than a crash. A crash tells you something happened. This produces a
file that passes every check a casual user would apply — it exists, and it is the right size.

### The hypotheses, in the order they were tried

**Hypothesis 1 — the seeder's peer server never started. REFUTED.**
`peer_server_thread_func` returns *silently* if `bind()` or `listen()` fails. If the seeder's
port were taken, it would still log in, still upload its manifest, still advertise itself as a
seeder, and serve nothing — with no message anywhere. That is a genuinely plausible cause of
exactly this symptom. It was refuted by the log: `[client] peer-server listening on port 6881`.
The server was up.

The silent-return was fixed anyway, as a diagnostic — see the note at the end of this entry.

**Hypothesis 2 — the seeder cannot find the file, or serves the wrong bytes. REFUTED.**
Tested by bypassing the downloader entirely and speaking the peer protocol by hand:

```
HEADER:    b'PIECE 300000'
BODY BYTES: 300000
BODY SHA1:  346f098d4d1bc25a592b21bfdab6fc3246fe71d2
expected:   346f098d4d1bc25a592b21bfdab6fc3246fe71d2
```

The serving side is perfect. This narrowed the fault to *the downloader never reaching the
seeder at all*, which is a very different search.

**Hypothesis 3 — the downloader is given a bad address. CONFIRMED.**
Asking the tracker for the manifest directly, by hand:

```
FILE_INFO alice/testfile.bin 300000 OWNER alice <hash> SEEDERS alice@alice@127.0.0.1:6881
```

`alice@alice@127.0.0.1:6881`. **The username appears twice.**

### Root cause

[`tracker.cpp:233`](../tracker.cpp#L233), in the `upload_file` handler:

```cpp
string user = user_info;                    // user_info == "alice@127.0.0.1:6881"
size_t atpos = user.find('@');
if (atpos != string::npos)
    user = user.substr(0, atpos);           // user is now "alice"
...
user_address_map[user] = user_info.substr(user.find('@') + 1);
//                       ^^^^^^^^^                ^^^^
//                       slices user_info          but searches `user`
```

The last line searches **`user`** and slices **`user_info`**. By the time it runs, `user` has
already been stripped to `"alice"`, which contains no `'@'`. So:

1. `user.find('@')` returns `std::string::npos`.
2. `npos` is not −1; it is `SIZE_MAX`, the largest `size_t`. It is *displayed* as −1 because
   the same bits read as −1 when interpreted as a signed integer.
3. `npos + 1` overflows and **wraps around to 0**. Unsigned overflow is defined behaviour in
   C++ — it wraps silently, no warning, no trap.
4. `user_info.substr(0)` returns **the whole string**.

So the map stored `"alice" -> "alice@127.0.0.1:6881"` instead of `"alice" -> "127.0.0.1:6881"`.
`get_file_info` then emits `s + "@" + address`, producing the doubled name. The downloader
splits on the *first* `'@'`, takes everything after it as `ip:port`, splits on `':'`, and hands
`"alice@127.0.0.1"` to `inet_pton`, which rejects it. `download_piece_from_peer` returns
`false` before opening a socket. Every peer fails, so every piece fails.

### Why it was hard to see

Four things conspired, and each one is a general lesson.

1. **The two variables differ by one suffix.** `user` and `user_info` sit two lines apart and
   read almost identically. R10's "two similar-looking values that mean different things" is
   exactly this case.
2. **`npos + 1 == 0` turns a bug into plausible output.** Had the wraparound produced a huge
   number, `substr` would have thrown `std::out_of_range` and the tracker would have died at
   the exact line at fault. Instead the one value that could not be more wrong is the one value
   `substr` treats as completely normal. The failure mode chosen by the language is the quiet
   one.
3. **The symptom is three components away from the cause.** The typo is in the tracker's
   *upload* path. It surfaces in the downloader's *connect* path, in a different process,
   during a different command, minutes later.
4. **The wrong data looks right.** `alice@alice@127.0.0.1:6881` is not obviously corrupt at a
   glance. It has the right shape, the right host and the right port. Only the repetition is
   wrong, and repetition is the thing eyes skip.

### Fix

Reuse `atpos`, which was computed from `user_info` and is already correct, and only record an
address when one was actually supplied:

```cpp
if (atpos != string::npos)
    user_address_map[user] = user_info.substr(atpos + 1);
```

The guard matters independently of the typo: a bare username with no `'@'` carries no address,
and the old code would have stored the username *as* the address.

### Verified

```
expected: aad75bb384f3765cdf5c71e1bdfedc75ace02618
got     : aad75bb384f3765cdf5c71e1bdfedc75ace02618
Download completed: bob/got.bin
```

### Trade-off accepted

The real fix is to stop passing `user@ip:port` as a single space-delimited token and to send the
address as its own field. That is the right shape, and it is deferred: the wire protocol is
being replaced wholesale in Phase 5, and reshaping it now means designing it twice. Logged in
`SCALE_NOTES.md` as a protocol smell — packing two fields into one token guarantees that
someone eventually splits it in the wrong place, which is precisely what happened here.

### Prevented from recurring

`scripts/e2e-smoke.sh` transfers a file and compares SHA-1 end to end. It fails loudly on
mismatch, so silent corruption cannot pass again.

### Related fix made at the same time

`peer_server_thread_func` returned silently on `bind`/`listen` failure. It now prints the errno
and states the consequence explicitly:

```
[client] FATAL: cannot serve pieces on port 6881;
         this client will advertise files it cannot actually serve
```

This did not cause R2 — hypothesis 1 was refuted. It is fixed because it makes *the next*
occurrence of this symptom take one minute instead of an hour. **A silent `return` on a failed
`bind` is a bug even when it is not today's bug.**

### Interview questions it answers

- *"Tell me about a bug you found and how you debugged it."* — the strongest answer available:
  a wrong-variable typo, an unsigned wraparound that suppressed the crash that would have
  located it, and a symptom three components downstream.
- *"What is `std::string::npos` and what happens if you add one to it?"*
- *"How do you debug a distributed system where the failure appears in a different process from
  the cause?"* — bisect the pipeline by speaking each protocol by hand. The manual `GET_PIECE`
  cleared the entire serving side in one command.

---

## R3 — permanent request/response desync after the first successful download (found this session)

**Symptom:** found by an adversarial size sweep (R10) rather than by the happy path. Seven
files at piece-size boundaries, uploaded and downloaded in sequence. `PIECE_SIZE` is 524288.

```
CASE              EXPECT_SZ     GOT_SZ  VERDICT
empty                     0          -  FAIL
one_byte                  1          1  PASS
exact_1piece         524288          -  FAIL
minus1               524287     524288  FAIL
plus1                524289     524287  FAIL
exact_2piece        1048576     524289  FAIL
multi               3000000    1048576  FAIL
```

**The tell is in the numbers, not in the verdicts.** `minus1` received `exact_1piece`'s size.
`plus1` received `minus1`'s. `exact_2piece` received `plus1`'s. **Every reply is one behind.**
That is not a size-handling bug. That is a stream desync, and the shift being exactly one is
the diagnosis.

**Root cause:** at [`client.cpp:822`](../client.cpp#L822), after a download completes:

```cpp
string update_cmd = "update_seeder " + groupid + " " + logged_in_user + " " + filename;
send_line(sock, update_cmd);          // sends. never reads the reply.
```

The tracker **does not implement `update_seeder`** — it is not in the command chain, so it
falls through to `else return "Unknown command";` at [`tracker.cpp:358`](../tracker.cpp#L358).
The tracker sends that reply. Nobody reads it. It sits in the socket receive buffer, and the
*next* `recv_line` on that socket consumes it instead of the response actually being waited
for. From that moment the client is permanently one response behind, on every command, for the
life of the connection.

It corrupts data rather than merely confusing the user: the downloader reads the *previous*
file's `FILE_INFO`, so it sizes the destination from one file's length and verifies against
another file's piece hashes.

**Why it was hard to see:** the happy path cannot show it. **One** download succeeds — the
desync is introduced by that success, and only the *second* download pays for it. A test that
transfers one file passes forever. This is why the size sweep exists.

**Relationship to the known defect D2:** D2 is the 30-second heartbeat thread writing down the
main loop's socket. R3 is the *same class* — a send with no matching receive — on the main
thread, with no concurrency involved at all. That makes R3 the better teaching example: it
proves the defect is not a race. A mutex around the socket would not have prevented it, because
nothing here is concurrent. The broken invariant is *send-then-receive as a pair*, and no lock
expresses that.

**Status: FIXED, 2 Sep 2026.** The fix was taken as a decision rather than silently
(R6, R11) — `ARCHITECTURE.md` D-007 and D-008. The options were:

| Option | What it means | Cost |
|---|---|---|
| Implement `update_seeder` in the tracker and read its reply | Restores the request/response pairing and makes the feature work | The feature has to be specified — what does re-announcing a seeder actually mean? |
| Delete the `send_line` call | One line; the desync is gone immediately | A peer that finished downloading never announces that it can now seed, so the swarm never grows past the original uploader |
| Fire-and-forget on a second connection | Keeps the announcement, removes the shared stream | An extra connection per announcement; the same fix D2 needs |

**What was done:** the tracker implements `update_seeder` and the client reads the reply, so
one rule holds everywhere — every request gets exactly one response, read before the next is
sent. The heartbeat thread, which had the same bug *with* a second thread (defect D2), was
given its **own connection** instead of a bigger lock.

**Two things found while fixing it, both of which changed the fix:**

- `sock_mtx` was taken in **exactly one place** — the heartbeat's `send`. The main loop's
  send/recv pair took no lock at all. The lock that looked like the socket's defence was
  serialising the heartbeat against nothing.
- There is **no `seeders.erase` anywhere** in the tracker. Nothing ever removes a seeder, so
  the 30-second re-announcement achieved nothing at all. *A heartbeat with no timeout on the
  receiving side is not a heartbeat; it is traffic.* Expiry is fork F4.

**Verified:** `scripts/e2e-edge.sh` goes from 1/7 to 6/7 passing (the remaining failure is R4,
the zero-byte file) and the one-row size shift is gone. A downloader now becomes an advertised
seeder — `alice@...:6881` before, `bob@...:6882 alice@...:6881` after — confirmed on three
consecutive runs.

**Also fixed at the same time — a defect in the test harness, not the product.** The first
attempt at that verification failed with `Connection refused`, and the cleanup in both e2e
scripts turned out to kill by PID variables (`APID`, `BPID`) that were **never assigned**. The
clients run inside process substitution, so killing the `tail` that feeds them leaves the client
alive holding ports 6881/6882. The next run then failed at `bind` — which my own new diagnostic
reported correctly, and which looks exactly like a product bug. Cleanup now kills by pattern and
waits for the ports to actually free. **A test suite that leaks processes manufactures failures
that cost more to diagnose than the bugs it finds.**

**Interview question it answers:** *"You have a mutex on the socket and the protocol still
desynchronised. Why?"* — because a mutex makes each *send* atomic, and the invariant that
matters is *send-then-receive* atomic as a pair. R3 sharpens it further: here there was no
second thread at all, so no lock could ever have helped. The general form: **a mutex protects
state; this is a rule about sequence.**

---

## R4 — a zero-byte file cannot be uploaded (found this session)

**Symptom:** uploading a 0-byte file appears to work at the client. Downloading it returns
`File not found in group`.

**Root cause:** `upload_file_to_tracker` builds the manifest with

```cpp
while ((n = read(fd, buffer.data(), PIECE_SIZE)) > 0)
    piece_hashes.push_back(sha1_bytes(buffer.data(), (size_t)n));
```

On an empty file the first `read` returns 0, the loop body never executes, and `piece_hashes` is
empty. The tracker then rejects the manifest with `Error: no piece hashes given`, so the file is
never registered. The client does not check the reply, so it reports success.

**Why it is a real defect and not a curiosity:** an empty file is a legitimate file. More
importantly it is the boundary case for "how many pieces does a file of size S have?" —
`ceil(0 / PIECE_SIZE) == 0`, and code written around `1 + size/PIECE_SIZE` gets it wrong in the
other direction. The general rule is that a length-prefixed protocol has to be able to say
"zero" and mean it.

**Status:** OPEN. Deferred to the Phase 5 transfer rewrite, where chunking is redesigned anyway.
Logged so it is never re-found.

**Interview question it answers:** *"What does your system do with a zero-byte file?"* — a
standard probe for whether boundary cases were actually tested or merely assumed.

---

## R2b — a failed download leaves a full-size zero-filled file behind

**Symptom:** the destination file from a failed transfer is exactly the right size and entirely
zeros. This is the *silent corruption* half of R2. Fixing R2's root cause stops it happening in
practice; it does not stop it happening when a transfer genuinely fails.

**Root cause:** the destination is opened and `ftruncate`d to the full size *before* any bytes
arrive, so workers can write pieces at their offsets in any order. When pieces fail, nothing
removes the file. A sparse, zero-filled file remains, indistinguishable from a real one by size.

**Status:** OPEN — a design fork (F7 in `ARCHITECTURE.md`), not a typo:

| Option | What it means | Cost |
|---|---|---|
| Delete the destination on incomplete download | Three lines. No corrupt file survives | Loses partial progress; a resumable download becomes impossible |
| Download to `dest.part`, rename to `dest` only on success | `rename()` within a filesystem is atomic, so the final name never exists in a partial state. This is what real downloaders do | Partial file left behind unless cleaned up; needs a resume story to justify keeping it |
| Leave it, print loudly | Zero work | An unreliable system that must be read carefully to be used safely. Rejected |

**Interview question it answers:** *"Your transfer fails halfway. What is on disk, and how does
the next run know not to trust it?"* — the answer people want is atomic rename, plus the
observation that size alone is never evidence of completeness.

---

## Reproducing all of this

```sh
make clean && make                       # B1, B2: builds and links with no warnings
scripts/e2e-smoke.sh                     # R2: one file, SHA-1 compared end to end
scripts/e2e-edge.sh                      # R3, R4: piece-boundary sweep. Currently FAILS by design
```

`scripts/e2e-edge.sh` is expected to fail until R3 and R4 are closed. **A failing test that
documents a known open defect is more useful than a passing test that avoids it** — it is what
turns the entry above into a regression check rather than a memory.

The B3/B4 compile errors in the original committed code are still reproducible:

```sh
git stash && git checkout pre-resurrection~1 && make
```
