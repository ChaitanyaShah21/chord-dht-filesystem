# Observability: metrics, logs and the dashboard

What a running cluster can be asked about itself, and how one transfer is followed through every process it
touched. This guide covers the decisions and the alternatives considered for each, the metrics and why each one is
there, the log format, the request id, and the dashboard. Every decision links to its full entry in
[`ARCHITECTURE.md`](../ARCHITECTURE.md).

> **Status:** decided; being built.

## The decisions

### Metrics over HTTP on their own port — [D-083](../ARCHITECTURE.md#d-083--every-process-serves-its-metrics-over-a-minimal-hand-written-http10-responder-on-a-separate-port)

Every process takes `--metrics <port>` and answers `GET /metrics` there in the Prometheus text format. One thread
serves one connection at a time: the request head must be at most 4 KiB and arrive within 2 s, the answer is one
response with a `Content-Length`, and the connection is closed. Nothing else is spoken on that port.

| Alternative | Why not |
|---|---|
| **A `METRICS` line on the protocol port, and an exporter** | a second process per process, and a scrape shares the data path's threads |
| **HTTP on the protocol port** | `GET <key>` is already a chunk request there |
| **Pushing (StatsD, a Pushgateway)** | a dead process stops pushing and looks quiet; a failed scrape is itself the signal |
| **prometheus-cpp** | an external library |

### Instruments and labels — [D-084](../ARCHITECTURE.md#d-084--histograms-with-fixed-buckets-for-latencies-and-hops-counters-and-gauges-for-the-rest-labels-only-from-fixed-sets)

Latencies and hop counts are histograms — a counter per bucket, a sum and a count — so percentiles are computed after
adding many processes' buckets together. Counters only go up; gauges are read at scrape time. A label takes values
only from a small fixed set: never a key, a name, a request id or an address.

| Alternative | Why not |
|---|---|
| **Summaries** | quantiles computed in each process cannot be combined: a p99 of p99s means nothing |
| **Averages** | hide the tail a user waits on |
| **Metrics counted from the logs** | they would depend on the wording of every log line |

### Where transfer numbers come from — [D-085](../ARCHITECTURE.md#d-085--the-client-serves-metrics-while-it-runs-the-dashboards-throughput-and-latency-are-measured-at-the-nodes)

A client process lives for one transfer, often shorter than a scrape interval. It still serves metrics while it runs
(`--metrics`), but the dashboard's throughput and latency are measured at the nodes, which live as long as the ring:
bytes in and out, and the time each chunk request takes to serve.

| Alternative | Why not |
|---|---|
| **A Pushgateway** | another daemon, and a dead client's last numbers stay up |
| **A metrics file at exit** | a collector to read it, and nothing during a long transfer |
| **A long-running client** | not the program users run |
