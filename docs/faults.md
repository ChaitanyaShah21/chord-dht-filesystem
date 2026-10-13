# Fault injection: what the system survives, shown

A workload of uploads and downloads runs against a real cluster while processes are killed, frozen and restarted and
packets are lost and delayed; afterwards, four invariants decide whether anything acknowledged was lost. This guide
covers the decision, the harness, the invariants and what the runs found. The decision links to its full entry in
[`ARCHITECTURE.md`](../ARCHITECTURE.md).

> **Status:** run: 9 runs of 180 s, 114 faults; every invariant held in every run ([`BENCHMARKS.md`](../BENCHMARKS.md)
> §11).

## The decision

### Signals, netem, invariants — [D-094](../ARCHITECTURE.md#d-094--faults-are-signals-to-real-processes-and-netem-in-a-private-network-namespace-under-a-live-workload-judged-by-four-invariants)

`scripts/faults.py <label> [--seconds 180] [--seed 1] [--netem]` — the whole schedule comes from the seed.

| Fault | How | For how long |
|---|---|---|
| a node dies | `SIGKILL`, then restarted from its store, joining through another node | 5–10 s |
| a node freezes | `SIGSTOP`, then `SIGCONT` | 4 s |
| the tracker leader dies | `SIGKILL`, then restarted from its log | 3 s |
| a tracker follower freezes | `SIGSTOP`, then `SIGCONT` | 3 s |
| the network degrades (`--netem`) | `tc qdisc replace dev lo root netem delay 10ms 5ms loss 5%`, inside `unshare -rn` | 10 s |

| Alternative | Why not |
|---|---|
| **Signals only** | loopback never loses or delays a packet |
| **A proxy between every pair** | the proxy's addresses would become the ring's identifiers |
| **Firewall partitions** | root, for a fault the Raft suite covers with `SIGSTOP` |

## The real-network profile — [D-095](../ARCHITECTURE.md#d-095--the-loopback-defaults-stay-a-real-network-profile-puts-every-deadline-above-a-syns-retransmission-timeout-measured-under-loss)

Under packet loss the default timers turn every lost packet into a failure: a node's calls give up after 200 ms and
Raft's after 150 ms, but TCP retransmits a lost segment after at least 200 ms and a lost SYN after 1 s. The defaults
stay — they were chosen by measured rules on this host, and every benchmark is tied to them — and a profile for a real
network puts every deadline above a SYN's retransmission:

```bash
NODE_ARGS="--timeout 1200" TRACKER_ARGS="--election 1200" scripts/cluster.sh up 6
scripts/faults.py net --profile network --netem       # measured in BENCHMARKS.md §11
```

| Alternative | Why not |
|---|---|
| **Record it as a limit only** | leaves no tested answer for running across machines |
| **Change the defaults** | every number measured since D-039 would describe settings nobody runs |

## What the runs found

![Throughput and failed operations over time, faults shaded](figures/faults.png)

- **Nothing acknowledged was lost.** Six runs with the default timers and three with the real-network profile: every
  acknowledged name resolved to its key, every acknowledged file downloaded byte for byte, no download ever returned
  wrong bytes, and every chunk was back on its three correct processes within 5 s of the last fault.
- **Faults cost time, not errors.** The client retries a chunk on another replica, falls through a dead coordinator,
  follows the tracker's redirects and waits out an election; 3 of 11,224 uploads failed (unacknowledged) and no download
  did. But in 9–25 seconds of each run nothing completed: a frozen node stalls everything routed through it until two
  strikes evict it, and 10 s of 5 % loss stalls nearly everything.
- **The timers chosen on loopback misread a lossy network** — the finding that became D-095 below. 200 ms and 150 ms sit
  under TCP's retransmission timeouts; under loss the default timers produced hundreds of strikes, dozens of live nodes
  evicted and over two hundred tracker elections in three minutes. With the profile: tens of strikes, one or two
  evictions, a handful of elections.
- **And the next limit after that:** the profile did not change throughput or p99 latency under loss, because every
  lookup hop and every chunk is a fresh TCP connection (D-024), each exposed to a lost SYN's one-second retransmission.

| Fault | What a client sees | Why |
|---|---|---|
| a node killed | a few hundred milliseconds | the refused connection is proof: evicted at once, reads and writes fall through (D-013, D-049) |
| a node frozen | up to two strikes — about 0.4 s by default, 2.4 s with the profile | a timeout is only suspicion (D-035) |
| the tracker leader killed | about 0.2 s (E = 150 ms) | an election, and the client is redirected (D-073, D-081) |
| a follower frozen | nothing — until it thaws, raises its term and forces an election | no pre-vote (D-077) |
| 5 % loss, 10 ± 5 ms delay | stalls of seconds | a lost SYN waits 1 s, and every exchange opens a connection |

## How a run is judged

The four invariants (above) are checked from outside, after every process is back and the ring has converged; the
counts — coordinated writes that failed and were retried, strikes, evictions by evidence, tracker elections — come from
every process's structured log (D-086). Raw rows per run go to `bench/raw/faults-<label>-ops.csv` and `-faults.csv`;
`scripts/plot-faults.py` draws `docs/figures/faults.png`.
