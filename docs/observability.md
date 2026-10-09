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
