# Testing strategy

Eleven unit-test programs, fourteen end-to-end suites on real processes, ThreadSanitizer builds, and
mutation testing to check that the tests can fail. The whole set runs in about four and a half minutes on
the development machine.

```bash
make clean && make && make check                 # build (warning-free) and the unit tests
for s in scripts/e2e-*.sh; do "$s" || break; done # the end-to-end suites
make tsan                                         # ThreadSanitizer builds: node, client, tracker, test-replica, test-obs, test-gc
```

## Two layers, because each catches what the other cannot

**In process (`tests/`, `make check`).** Every piece of logic is written so that it never opens a
socket: the lookup loop, a member's routing state, the chunk plane, the transfer engine and the
namespace each take the network as a function passed in (D-018, D-024, D-060). The tests therefore
build rings by hand and can construct what a correct deployment never produces — a stale finger
table, a reply that points backwards, a node that lies for ever, a manifest that lies about a
chunk's length, a disk that refuses writes — and can order every node's maintenance round by hand.

| Program | Checks | What it covers |
|---|---:|---|
| `test-chord` | 127 | identifier arithmetic at the wrap-around, interval tests, finger construction, routing on hand-built rings against an independent model |
| `test-net` | 37 | framing edge cases, refused / timed out / closed over real sockets, the connect deadline, read deadlines against trickled lines and bodies |
| `test-lookup` | 70 | the lookup loop against correct, lying and broken rings; the progress check; a ring that needs exactly the 65-request cap |
| `test-stabilize` | 97 | a whole ring on one thread: every round order of two joins between the same pair, kills and freezes at chosen moments, fast restarts, collisions, 200 random churn histories |
| `test-chunk` | 113 | the store, body framing, virtual nodes, replica sets against a brute-force oracle, writes against dead and slow replicas, reads past corrupt copies, repair, the orphaned-sibling livelock, repair's per-arc digest (a healthy scan sends no listing; a missing copy, an older node answering `ERR`) |
| `test-transfer` | 87 | manifests (26 malformed ones refused one rule at a time), trees, the worker pool, whole transfers against an in-memory ring that loses, delays and lies |
| `test-names` | 46 | the namespace as a state machine: request parsing, applying entries, write-once per owner, deletion by the owner's secret only, paged listing with and without keys |
| `test-raft` | 175 | the Raft core in a seeded simulation — loss, duplication, reordering, partitions, crashes — with the safety properties checked after every step and every client answer checked for linearizability; Figure 8 by hand; the vote, append, commit and ReadIndex rules; the log file; the wire format; the client finding the leader |
| `test-replica` | 32 | three tracker replicas in one process with real threads: election, redirects, contending clients, a cut-off leader, a waiting request whose entry is replaced, restart, a failing disk |
| `test-obs` | 107 | the metrics text format rule by rule (cumulative buckets, `+Inf` = `_count` while 200 renders race observations, escaping, refused registrations); the metrics port's parser and reader against split, oversized, trickled and truncated heads; the logger's escaping and 8 threads × 500 lines through one pipe; request ids and their thread-local scope |
| `test-gc` | 65 | dropping only what is old enough, renewal by a duplicate write, a restart counting as a write, 2,000 rounds of a write racing a drop; `KEYS` and `DROP` through the node's handler; the ring walk; whole collections: a chunk and a manifest shared by two files, an unreadable tree, a dry run, the deadline, a node with too short a grace, an upload in flight and one reusing old orphans |

**On real processes (`scripts/e2e-*.sh`).** These catch what the in-process layer cannot: a program
that does not use the tested code at all, real sockets, real signals, real disks.

| Suite | What it proves |
|---|---|
| `e2e-node.sh` | a fixed 8-node ring routes every lookup to the right owner, with the hop counts an independent model predicts |
| `e2e-chord.sh` | the C++ originator on a 16-node ring: paths match a model; a killed and a frozen node are each reported as the right kind of failure, naming the right node |
| `e2e-churn.sh` | joins, kills, freezes and restarts on real processes; the ring repairs itself (22 checks) |
| `e2e-chunk.sh` | chunks on 6 processes × 4 members: placement on disk against an oracle, sizes, hostile requests, corruption, two of three replicas killed, repair |
| `e2e-transfer.sh` | files through 8 processes: a node killed mid-download, a corrupt copy, a chunk lost everywhere, the client killed with `SIGKILL` and stopped with `SIGTERM`, two identical uploads at once, the tracker down |
| `e2e-edge.sh` | every size across the chunk boundary round-trips — 0, 1, n−1, n, n+1, 2n and multi-chunk |
| `e2e-deadline.sh` | a silent, trickling or body-withholding peer is cut off at the deadline and frees what it held |
| `e2e-smoke.sh` | `scripts/cluster.sh up` gives a working cluster; one file round-trips, compared by SHA-1; `down` stops everything it started |
| `e2e-persistence.sh` | tracker state survives `SIGKILL` and restart; a crash's damaged tail is cut, damage further back refuses to start |
| `e2e-raft.sh` | three tracker replicas: the leader killed and frozen, both followers frozen (the lone leader never acknowledges), a majority killed (writes refused, never acknowledged), everything restarted, a writer through a failover, a mismatched member list |
| `e2e-framing.sh` | one request, one reply, one line, on a raw socket, for every malformed-input path |
| `e2e-hangup.sh` | a client that vanishes mid-conversation does not take the tracker with it |
| `e2e-observe.sh` | every process's `/metrics` accepted by an independent parser (and `promtool`); the numbers checked against what was done — bytes, copies, leaders, commit indices, evictions by refusal, leader changes; hostile requests to the metrics port; one upload and one download reconstructed from every process's log by request id |
| `e2e-gc.sh` | only an upload's token deletes its name, and no secret reaches a log; a chunk shared by two files survives deleting one; only old, unused copies are dropped, the bytes freed matching the stores; an upload in flight and a re-upload of old orphans survive a collection; the upload limit; the collector's refusals; a restarted node keeps everything |

Each suite uses its own ports and kills only its own processes, matched by port — never a
machine-wide `pkill`, which once let two suites kill each other's processes and produced a failure
that looked exactly like a product regression.

**Under faults (`scripts/faults.py`, D-094).** Not a pass/fail suite but a measured run: three minutes of real
uploads and downloads while a seeded schedule kills, freezes and restarts nodes and tracker replicas and — inside a
private network namespace — drops and delays packets, then four invariants checked from outside (no acknowledged name
lost, no acknowledged byte lost, three copies restored, no wrong download). Nine runs, 114 faults: all held
([`faults.md`](faults.md)).

## The oracle lives in the harness, never in the node

Wherever a test or a benchmark judges correctness, the answer is computed **independently of the
code under test**, from inputs the harness controls (D-020, D-038, D-053):

- the owner of a key is computed in Python from SHA-1 of the live addresses (`scripts/ringcheck.py`,
  `scripts/chunkcheck.py`), not asked of a node;
- "the ring has reconverged" is decided by polling every node and comparing with that oracle — a node
  only reports what it holds, never whether it is right;
- where a chunk's three copies *should* be is computed by the harness's own implementation of the
  replica-set rule and checked against `HAVE` answers and the files on disk;
- hop counts are checked against a model of the finger rule, not only owners — a wrong finger still
  reaches the right owner, just in more hops, and a test that checked owners alone would pass it.

The checker must not be the thing being checked.

## Mutation testing: can the tests fail?

A test suite that passes against broken code proves nothing, so each phase's tests were run against
deliberately planted bugs, each in a fresh copy of the tree with a clean build:

| Area | Planted | Caught | The ones that survive, and why |
|---|---:|---:|---|
| Joins, stabilisation, failure handling | 20 | 17 by the functional tests, 1 more only by ThreadSanitizer | two are independent defences against the same failure — each survives alone, the pair is caught; the third (a read without the lock) is a data race only the sanitizer sees |
| Chunk plane | 20 | 18 | one is covered twice over (the receiver re-checks the hash anyway); one is a check-then-increment race on an atomic counter that neither a stress test nor ThreadSanitizer can see — a race on an atomic is not a data race — so the single `fetch_add` is correct by construction, not by test |
| Transfer and namespace | 22 | 20 | both remove a flush before an acknowledgement (a download's `fsync`, a registration's `fdatasync`); a test on a running machine cannot cut the power |
| Observability | 22 | 20 by the functional tests, 1 more only by ThreadSanitizer (the replica's state read without its lock, seen because the sanitizer's report broke the suite's every-line-is-structured check) | closing a connection without a graceful drain: on Linux loopback a reset does not discard a response already queued, so no test here can see it |
| Anti-entropy | 6 | 6 | — |
| Deletion | 18 | 17 | the unlink done after the store's lock is released: the race is on a file, not on memory, so ThreadSanitizer cannot see it, and 2,000 racing rounds never hit a window of microseconds against a write that takes milliseconds — the lock around check and unlink is correct by construction, not by test |
| The replicated tracker | 24 | 22 by the functional tests, 1 more only by ThreadSanitizer | the flush before a reply (`fdatasync`), again: only a power cut shows it. Three others survived at first — a vote given in a term already held and not saved; a late vote reply from an older term counted; a waiting client told the result of the entry that replaced its own — each a gap in the directed tests, each closed with a test of its own |

In the replicated tracker, the in-process layers caught every functional mutant; the real-process suites caught 7 of
them as well. Committing an earlier term's entry by counting (Figure 8) is caught by its directed test every time and
by only 5 of 60 random histories — the subtle cases need tests built for them.

Mutation testing also found weak tests, which were then fixed: a traversal-key test too short to
reach the character check; a deadline test that hung instead of failing; a log-check test whose
damaged record failed key validation before it reached the checksum. And it found one property that
can no longer be tested: since the tracker was rewritten (D-060) it stops at the first failed send,
and Linux raises `SIGPIPE` only on the second send after a reset, so `e2e-hangup.sh` cannot tell a
tracker that ignores the signal from one that does not. That is recorded in the suite rather than
contrived around.

## ThreadSanitizer

`make tsan` builds `node-tsan` and `client-tsan` with every translation unit instrumented. On this
aarch64 kernel ThreadSanitizer needs address-space randomisation off:

```bash
NODE_CMD="setarch $(uname -m) -R ./node-tsan" CLIENT_CMD="setarch $(uname -m) -R ./client-tsan" scripts/e2e-transfer.sh
```

Run through the churn, chunk, transfer and deadline suites with no report. The tracker's replica code runs under it in
`test-replica-tsan` and in `e2e-raft.sh` with `TRACKER_CMD="setarch $(uname -m) -R ./tracker-tsan"`, with no report; a
planted race in it (a sender reading its slot after releasing the lock) was caught by ThreadSanitizer and by nothing
else. The build was validated by
removing one lock on purpose: it reported the race. There is no AddressSanitizer or
UndefinedBehaviorSanitizer build.

## Tests that found bugs

- **A failing test committed on purpose.** `e2e-edge.sh` was committed in September failing, pinning
  defects R3 and R4 (D-006). It passed 6 of 7 cases once R3 was fixed, and all 7 once R4 was.
- **A test that asserted the winner of a race.** `e2e-transfer.sh` once required the client to hit a
  node it had just killed; under load the dead node's neighbours sometimes evicted it first, which is
  equally correct. Found by looping the suite under load; the check now asserts the property, not the
  race.
- **The benchmark harness found a livelock** the tests had not: an orphaned virtual node held out of
  the ring for ever by its own sibling (D-067). It was reproduced in process before it was fixed — 0 of
  20 round orders healed before the fix, 20 of 20 after.

- **A test that assumed leadership survives a heal.** After a partition healed, `test-replica` asked the leader
  elected during it — and sometimes got `REDIRECT`: the cut-off replica had kept raising its term, and its return
  forced an election. That is the cost of having no pre-vote (D-077), now measured: 20 of 20 heals forced one. The test
  now asks whoever leads.
- **A test that thought every manifest was a file's own.** The first shared-chunk collection test expected all of
  the deleted file's manifests to go; one stayed, correctly: two files whose first four chunks are the same have the
  same first data manifest, byte for byte — one chunk, used by both.
- **The benchmarks found a read-availability window the tests had not.** Two benchmark downloads failed with every
  asked replica `MISSING`; the request ids showed the chunks on three other processes, written while a freshly joined
  ring was still converging. The harnesses now wait for the ring oracle before writing; the window is recorded in
  `SCALE_NOTES.md`.
- **A harness race.** `e2e-chord.sh` waited for a killed node's port to close and treated only "connection refused" as
  closed; a connect accepted into the dying listener's backlog and then reset crashed the harness. Any other error now
  means "not closed yet".

Every defect found this way is in [`failures.md`](failures.md) or the decision it changed.

## Continuous integration

`.github/workflows/ci.yml` builds (any warning fails), runs `make check` and the fast end-to-end suites
on x86-64 and arm64 runners; the churn suite and the ThreadSanitizer runs are started by hand, because
they kill and freeze processes on timers (D-065). **It has not run yet**: it runs once the repository
is pushed.
