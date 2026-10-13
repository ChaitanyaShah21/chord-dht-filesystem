#!/usr/bin/env python3
"""make-dashboard.py -- write deploy/grafana/dashboards/chordfs.json (decision D-088).

The dashboard is generated, so each panel is one readable line of PromQL here instead of
forty lines of JSON, and the JSON in the repository is always what this script says.
Run it after any change:  python3 deploy/grafana/make-dashboard.py

Every panel answers a question the system's own claims raise; docs/observability.md
lists each metric and why it is there.
"""
import json, os

DS = {"type": "prometheus", "uid": "chordfs-prom"}
RATE = "$__rate_interval"
panels, next_id = [], 1


def panel(kind, title, targets, x, y, w, h, unit="short", desc="", **extra):
    """One panel at grid position (x, y), w of 24 columns wide, h rows high."""
    global next_id
    p = {"id": next_id, "type": kind, "title": title, "description": desc, "datasource": DS,
         "gridPos": {"x": x, "y": y, "w": w, "h": h},
         "targets": [{"refId": chr(65 + i), "datasource": DS, "expr": e, "legendFormat": legend}
                     for i, (e, legend) in enumerate(targets)],
         "fieldConfig": {"defaults": {"unit": unit}, "overrides": []}, "options": {}}
    if kind == "timeseries":
        p["fieldConfig"]["defaults"]["custom"] = {"lineWidth": 1, "fillOpacity": 10, "showPoints": "never"}
        p["options"] = {"legend": {"displayMode": "list", "placement": "bottom"}, "tooltip": {"mode": "multi"}}
    if kind == "stat":
        p["options"] = {"reduceOptions": {"calcs": ["lastNotNull"]}, "colorMode": "value", "graphMode": "none"}
        # One colour: these are counts, not alarms (Grafana's default turns anything above 80 red).
        p["fieldConfig"]["defaults"]["color"] = {"mode": "fixed", "fixedColor": "#2a78d6"}
    p.update(extra)
    panels.append(p)
    next_id += 1


def quantile(q, metric, by=""):
    group = f"le{', ' + by if by else ''}"
    return f"histogram_quantile({q}, sum by ({group}) (rate({metric}_bucket[{RATE}])))"


def row(title, y):
    global next_id
    panels.append({"id": next_id, "type": "row", "title": title, "collapsed": False, "panels": [],
                   "gridPos": {"x": 0, "y": y, "w": 24, "h": 1}})
    next_id += 1


# ---- the cluster at a glance ------------------------------------------------
row("Cluster", 0)
panel("stat", "Nodes up", [('sum(up{component="node"})', "")], 0, 1, 3, 4)
panel("stat", "Tracker replicas up", [('sum(up{component="tracker"})', "")], 3, 1, 3, 4)
panel("stat", "Raft leaders", [('sum(chordfs_raft_role{role="leader"})', "")], 6, 1, 3, 4,
      desc="Replicas that believe they lead. 1 when healthy; 0 during an election.")
panel("stat", "Raft term", [("max(chordfs_raft_term)", "")], 9, 1, 3, 4)
panel("stat", "Chunks held (all copies)", [("sum(chordfs_store_chunks)", "")], 12, 1, 4, 4)
panel("stat", "Bytes held (all copies)", [("sum(chordfs_store_bytes)", "")], 16, 1, 4, 4, unit="bytes")
panel("stat", "Names", [('max(chordfs_names)', "")], 20, 1, 4, 4)
panel("state-timeline", "Process up (a gap is a fault)", [("up", "{{instance}} {{component}}")], 0, 5, 24, 6,
      options={"showValue": "never", "mergeValues": True, "rowHeight": 0.8},
      fieldConfig={"defaults": {"unit": "short", "mappings": [
          {"type": "value", "options": {"0": {"text": "down", "color": "red"}, "1": {"text": "up", "color": "green"}}}],
          "color": {"mode": "thresholds"},
          "thresholds": {"mode": "absolute", "steps": [{"color": "red", "value": None}, {"color": "green", "value": 1}]}},
          "overrides": []})

# ---- the data plane -----------------------------------------------------------
row("Data plane", 11)
panel("timeseries", "Throughput through the nodes",
      [(f"sum by (direction) (rate(chordfs_chunk_bytes_total[{RATE}]))", "{{direction}}")], 0, 12, 12, 8, unit="Bps",
      desc="Chunk bytes received (PUT, STORE) and sent (CHUNK) by every node, summed: the system's throughput "
           "whatever the clients' lifetimes (D-085).")
panel("timeseries", "Chunk request latency at the node, p50 and p99",
      [(quantile(0.5, "chordfs_chunk_request_seconds", "verb").replace("_bucket[", '_bucket{verb=~"put|store|get"}['), "p50 {{verb}}"),
       (quantile(0.99, "chordfs_chunk_request_seconds", "verb").replace("_bucket[", '_bucket{verb=~"put|store|get"}['), "p99 {{verb}}")],
      12, 12, 12, 8, unit="s",
      desc="From the request read to the reply sent. A PUT is answered once W copies exist.")
panel("timeseries", "Chunk requests by result",
      [(f"sum by (verb, result) (rate(chordfs_chunk_requests_total[{RATE}])) > 0", "{{verb}} {{result}}")],
      0, 20, 12, 8, unit="reqps")
panel("timeseries", "Lookup hops (nodes' own lookups)",
      [(quantile(0.5, "chordfs_lookup_hops"), "p50"), (quantile(0.99, "chordfs_lookup_hops"), "p99"),
       (f"sum(rate(chordfs_lookup_hops_sum[{RATE}])) / sum(rate(chordfs_lookup_hops_count[{RATE}]))", "mean")],
      12, 20, 12, 8, desc="Fingers followed per lookup: about half of log2 of the number of ring members.")

# ---- the ring's health ----------------------------------------------------------
row("Ring maintenance and repair", 28)
panel("timeseries", "Strikes and evictions",
      [(f"sum(rate(chordfs_ring_strikes_total[{RATE}]))", "strikes"),
       (f"sum by (evidence) (rate(chordfs_ring_evictions_total[{RATE}]))", "evicted: {{evidence}}"),
       (f"sum(rate(chordfs_ring_successor_changes_total[{RATE}]))", "closer successor adopted")],
      0, 29, 12, 8, unit="ops")
panel("timeseries", "Shortest successor list, orphaned members",
      [("min(chordfs_ring_successor_list_min)", "shortest list"), ("sum(chordfs_ring_orphaned_members)", "orphaned")],
      12, 29, 12, 8, desc="Margin before a member is cut off: the list has 8 entries when healthy.")
panel("timeseries", "Copies: time from acknowledgement to the last copy, p99",
      [(quantile(0.99, "chordfs_put_replication_lag_seconds"), "p99 lag"),
       (f"sum(rate(chordfs_put_copies_short_total[{RATE}]))", "writes ending short of 3 copies /s")],
      0, 37, 12, 8, unit="s")
panel("timeseries", "Repair",
      [(f"sum(rate(chordfs_repair_chunks_sent_total[{RATE}]))", "chunks re-replicated /s"),
       (f"sum(rate(chordfs_repair_failures_total[{RATE}]))", "failed offers /s"),
       ("sum(chordfs_repair_pending_arcs)", "arcs awaiting a re-check")], 12, 37, 12, 8)

# ---- the control plane -----------------------------------------------------------
row("Tracker (Raft)", 45)
panel("timeseries", "Term and commit index per replica",
      [("chordfs_raft_term", "term {{instance}}"), ("chordfs_raft_commit_index", "commit {{instance}}")], 0, 46, 8, 8)
panel("timeseries", "Leader changes and elections",
      [(f"sum(increase(chordfs_raft_leader_changes_total[1m]))", "leader changes / min"),
       (f"sum(increase(chordfs_raft_elections_total[1m]))", "elections started / min")], 8, 46, 8, 8)
panel("timeseries", "Log flush (fdatasync), p50 and p99",
      [(quantile(0.5, "chordfs_raft_flush_seconds"), "p50"), (quantile(0.99, "chordfs_raft_flush_seconds"), "p99")],
      16, 46, 8, 8, unit="s", desc="Each step's batch, flushed before any reply (D-074); the write path's main cost (D-082).")
panel("timeseries", "Tracker requests by result",
      [(f"sum by (verb, result) (rate(chordfs_tracker_requests_total[{RATE}])) > 0", "{{verb}} {{result}}")],
      0, 54, 12, 8, unit="reqps")
panel("timeseries", "Tracker request latency, p99",
      [(quantile(0.99, "chordfs_tracker_request_seconds", "verb"), "{{verb}}")], 12, 54, 12, 8, unit="s")

# ---- errors -------------------------------------------------------------------
row("Errors and connections", 62)
panel("timeseries", "Errors by type",
      [(f"sum by (type) (rate(chordfs_request_errors_total[{RATE}]))", "{{type}}"),
       (f"sum by (error) (rate(chordfs_lookup_failures_total[{RATE}]))", "lookup: {{error}}")], 0, 63, 12, 8, unit="ops")
panel("timeseries", "Open connections (a thread each)",
      [("sum by (component) (chordfs_connections_open)", "{{component}}")], 12, 63, 12, 8)

dashboard = {
    "uid": "chordfs", "title": "chord-dht-filesystem", "tags": ["chordfs"], "timezone": "browser",
    "schemaVersion": 39, "version": 1, "editable": False, "refresh": "5s",
    "time": {"from": "now-15m", "to": "now"}, "panels": panels,
    "annotations": {"list": [{"builtIn": 1, "datasource": {"type": "grafana", "uid": "-- Grafana --"},
                              "enable": True, "hide": True, "iconColor": "rgba(0, 211, 255, 1)",
                              "name": "Annotations & Alerts", "type": "dashboard"}]},
}
out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "dashboards", "chordfs.json")
with open(out, "w") as f:
    json.dump(dashboard, f, indent=1)
    f.write("\n")
print("wrote", out, f"({len(panels)} panels)")
