# Testing strategy

Seven unit-test programs, eleven end-to-end suites on real processes, a ThreadSanitizer build, and
mutation testing to check that the tests can fail. The whole set runs in about two minutes on the
development machine.

```bash
make clean && make && make check                 # build (warning-free) and the unit tests
for s in scripts/e2e-*.sh; do "$s" || break; done # the end-to-end suites
make tsan                                         # ThreadSanitizer builds of node and client
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
| `test-chunk` | 105 | the store, body framing, virtual nodes, replica sets against a brute-force oracle, writes against dead and slow replicas, reads past corrupt copies, repair, the orphaned-sibling livelock |
| `test-transfer` | 87 | manifests (26 malformed ones refused one rule at a time), trees, the worker pool, whole transfers against an in-memory ring that loses, delays and lies |
| `test-names` | 39 | the namespace and its log: torn and altered records, `/dev/full`, eight threads racing for one name |

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
| `e2e-persistence.sh` | tracker state survives `SIGKILL` and restart, including a torn record and a damaged one |
| `e2e-framing.sh` | one request, one reply, one line, on a raw socket, for every malformed-input path |
| `e2e-hangup.sh` | a client that vanishes mid-conversation does not take the tracker with it |

Each suite uses its own ports and kills only its own processes, matched by port — never a
machine-wide `pkill`, which once let two suites kill each other's processes and produced a failure
that looked exactly like a product regression.

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

Run through the churn, chunk, transfer and deadline suites with no report. The build was validated by
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

Every defect found this way is in [`failures.md`](failures.md) or the decision it changed.

## Continuous integration

`.github/workflows/ci.yml` builds (any warning fails), runs `make check` and the fast end-to-end suites
on x86-64 and arm64 runners; the churn suite and the ThreadSanitizer runs are started by hand, because
they kill and freeze processes on timers (D-065). **It has not run yet**: it runs once the repository
is pushed.
