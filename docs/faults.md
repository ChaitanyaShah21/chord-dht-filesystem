# Fault injection: what the system survives, shown

A workload of uploads and downloads runs against a real cluster while processes are killed, frozen and restarted and
packets are lost and delayed; afterwards, four invariants decide whether anything acknowledged was lost. This guide
covers the decision, the harness, the invariants and what the runs found. The decision links to its full entry in
[`ARCHITECTURE.md`](../ARCHITECTURE.md).

> **Status:** decided; being run.

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
