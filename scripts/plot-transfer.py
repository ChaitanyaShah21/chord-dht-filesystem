#!/usr/bin/env python3
"""plot-transfer.py -- summarise the Phase 5 transfer benchmarks and draw their figures.

  scripts/plot-transfer.py <commit-label> --baseline <MB/s> [--baseline-sep <MB/s>]

Reads bench/raw/transfer-<label>.csv and bench/raw/latency-<label>.csv (written by
scripts/bench-transfer.py). --baseline is the single-seeder baseline's 100 MB median
re-run the same day at its own commit (D-066); --baseline-sep the September figure.
Writes:
  docs/figures/transfer.csv            one row per summarised configuration
  docs/figures/transfer-workers.png    a. 100 MB download and upload throughput against workers, with the baseline
                                       b. per-chunk download latency, p50 and p99, against workers
                                       c. 100 MB download throughput against ring processes
  docs/figures/transfer-chunks.png     a. 100 MB throughput against chunk size
                                       b. per-chunk download latency, p50 and p99, against chunk size
Throughput points are medians of the repetitions; bars run from the slowest to the fastest.
"""
import argparse, csv, os, statistics
from collections import defaultdict

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIG = os.path.join(ROOT, "docs", "figures")
MB = 1 << 20

# The palette of the earlier figures: categorical slots validated together, each
# series also with its own marker shape.
SURFACE, INK, INK_2, GRID = "#fcfcfb", "#0b0b0b", "#52514e", "#e4e3df"
BLUE, ORANGE, GREEN = "#2a78d6", "#eb6834", "#1baf7a"

ap = argparse.ArgumentParser()
ap.add_argument("label")
ap.add_argument("--baseline", type=float, required=True)
ap.add_argument("--baseline-sep", type=float, default=71.2)
a = ap.parse_args()
rows = list(csv.DictReader(open(os.path.join(ROOT, "bench", "raw", f"transfer-{a.label}.csv"))))
lat = defaultdict(list)          # (mode, file, procs, workers, chunk, build, op) -> [micros]
lat_path = os.path.join(ROOT, "bench", "raw", f"latency-{a.label}.csv")
if os.path.exists(lat_path):
    for line in open(lat_path):
        f = line.strip().split(",")
        mode, file, procs, workers, chunk, build, op = f[:7]
        if f[13] == "1":
            lat[(mode, file, int(procs), int(workers), int(chunk), build, op)].append(int(f[10]))


def pct(xs, q):
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(q * len(xs)))] if xs else None


def style(ax, title, xlabel, ylabel):
    ax.set_facecolor(SURFACE)
    ax.set_title(title, loc="left", fontsize=10, color=INK)
    ax.set_xlabel(xlabel, fontsize=9, color=INK_2)
    ax.set_ylabel(ylabel, fontsize=9, color=INK_2)
    ax.grid(True, color=GRID, linewidth=0.8)
    ax.set_axisbelow(True)
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)
    for s in ("left", "bottom"):
        ax.spines[s].set_color(GRID)
    ax.tick_params(colors=INK_2, labelsize=8)


summary = []


def group(mode, key, build=None):
    """{x: [rows]} for one mode, keyed by `key`, ok rows only."""
    out = defaultdict(list)
    for r in rows:
        if r["mode"] == mode and r["ok"] == "1" and (build is None or r["build"] == build):
            out[int(r[key])].append(r)
    return dict(sorted(out.items()))


def tput(rs, field):
    return [int(r["bytes"]) / MB / (float(r[field]) / 1000) for r in rs]


def summarise(table, x_name, x, rs, extra=None):
    down = tput(rs, "get_ready_ms")
    down_wall = tput(rs, "get_wall_ms")
    up = tput(rs, "put_wall_ms")
    s = {"table": table, x_name: x, "file": rs[0]["file"], "bytes": rs[0]["bytes"], "procs": rs[0]["procs"],
         "workers": rs[0]["workers"], "chunk": rs[0]["chunk"], "build": rs[0]["build"], "reps": len(rs),
         "get_ready_mbps_median": round(statistics.median(down), 1), "get_ready_mbps_min": round(min(down), 1),
         "get_ready_mbps_max": round(max(down), 1), "get_wall_mbps_median": round(statistics.median(down_wall), 1),
         "get_ready_ms_median": round(statistics.median(float(r["get_ready_ms"]) for r in rs), 1),
         "get_sync_ms_median": round(statistics.median(float(r["get_sync_ms"]) for r in rs), 1),
         "put_wall_mbps_median": round(statistics.median(up), 1), "put_wall_ms_median": round(statistics.median(float(r["put_wall_ms"]) for r in rs), 1)}
    s.update(extra or {})
    summary.append(s)
    return down, up


def errbar(ax, xs, series, color, marker, label):
    med = [statistics.median(v) for v in series]
    lo = [m - min(v) for m, v in zip(med, series)]
    hi = [max(v) - m for m, v in zip(med, series)]
    ax.errorbar(xs, med, yerr=[lo, hi], color=color, marker=marker, markersize=6, linewidth=2, capsize=3, label=label)


def latency_extra(mode, procs, workers, chunk, build, file="huge.iso"):
    d = lat.get((mode, file, procs, workers, chunk, build, "get"), [])
    u = lat.get((mode, file, procs, workers, chunk, build, "put"), [])
    return {"get_chunk_p50_ms": round(pct(d, 0.5) / 1000, 2) if d else "", "get_chunk_p99_ms": round(pct(d, 0.99) / 1000, 2) if d else "",
            "put_chunk_p50_ms": round(pct(u, 0.5) / 1000, 2) if u else "", "put_chunk_p99_ms": round(pct(u, 0.99) / 1000, 2) if u else "",
            "chunks_timed": len(d)}


# ---- sizes: the before/after table ----
for (f, w), rs in sorted({(r["file"], int(r["workers"])): None for r in rows if r["mode"] == "sizes"}.items()):
    sel = [r for r in rows if r["mode"] == "sizes" and r["ok"] == "1" and r["file"] == f and int(r["workers"]) == w]
    summarise("sizes", "workers_x", w, sel, latency_extra("sizes", int(sel[0]["procs"]), w, int(sel[0]["chunk"]), sel[0]["build"], f))

# ---- workers ----
fig, (a1, a2, a3) = plt.subplots(1, 3, figsize=(14, 4), facecolor=SURFACE)
wk = group("workers", "workers")
if wk:
    xs = list(wk)
    downs, ups = [], []
    for x, rs in wk.items():
        d, u = summarise("workers", "workers_x", x, rs, latency_extra("workers", 16, x, 524288, rs[0]["build"]))
        downs.append(d); ups.append(u)
    errbar(a1, xs, downs, BLUE, "o", "download (to every chunk verified and written)")
    errbar(a1, xs, ups, GREEN, "^", "upload (W = 2, every copy flushed)")
    a1.axhline(a.baseline, color=ORANGE, linewidth=1.5, linestyle="--")
    a1.annotate(f"single-seeder baseline, re-run same day: {a.baseline:.1f} MB/s", (xs[0], a.baseline), textcoords="offset points",
                xytext=(2, 4), fontsize=8, color=ORANGE)
    a1.set_xscale("log", base=2)
    a1.set_xticks(xs)
    a1.set_xticklabels([str(x) for x in xs])
    a1.legend(fontsize=7, frameon=False, loc="lower right")
    style(a1, "a. 100 MB through a 16-process ring", "workers", "MB/s (median; bars: slowest to fastest)")
    p50 = [pct(lat.get(("workers", "huge.iso", 16, x, 524288, wk[x][0]["build"], "get"), []), 0.5) for x in xs]
    p99 = [pct(lat.get(("workers", "huge.iso", 16, x, 524288, wk[x][0]["build"], "get"), []), 0.99) for x in xs]
    if all(v is not None for v in p50):
        a2.plot(xs, [v / 1000 for v in p50], color=BLUE, marker="o", linewidth=2, label="p50")
        a2.plot(xs, [v / 1000 for v in p99], color=ORANGE, marker="s", linewidth=2, label="p99")
        a2.set_xscale("log", base=2)
        a2.set_xticks(xs)
        a2.set_xticklabels([str(x) for x in xs])
        a2.legend(fontsize=8, frameon=False)
    style(a2, "b. Per-chunk download time (lookup + GET + write)", "workers", "ms per 512 KiB chunk")
pc = group("procs", "procs")
if pc:
    xs = list(pc)
    downs = []
    for x, rs in pc.items():
        d, _ = summarise("procs", "procs_x", x, rs, latency_extra("procs", x, int(rs[0]["workers"]), 524288, rs[0]["build"]))
        downs.append(d)
    errbar(a3, xs, downs, BLUE, "o", "download")
    a3.set_xscale("log", base=2)
    a3.set_xticks(xs)
    a3.set_xticklabels([str(x) for x in xs])
    style(a3, f"c. 100 MB download, {pc[xs[0]][0]['workers']} workers", "ring processes (8 members each)", "MB/s")
fig.tight_layout()
fig.savefig(os.path.join(FIG, "transfer-workers.png"), dpi=150, facecolor=SURFACE)

# ---- chunk size ----
cs_rows = [r for r in rows if r["mode"] == "chunks" and r["ok"] == "1"]
if cs_rows:
    sweep_build = next(r["build"] for r in cs_rows if int(r["chunk"]) != 524288)
    fig, (b1, b2) = plt.subplots(1, 2, figsize=(10, 4), facecolor=SURFACE)
    sw = group("chunks", "chunk", build=sweep_build)
    xs = list(sw)
    downs, ups = [], []
    for x, rs in sw.items():
        d, u = summarise("chunks", "chunk_x", x, rs, latency_extra("chunks", 16, int(rs[0]["workers"]), x, sweep_build))
        downs.append(d); ups.append(u)
    kib = [x // 1024 for x in xs]
    errbar(b1, kib, downs, BLUE, "o", "download")
    errbar(b1, kib, ups, GREEN, "^", "upload (W = 2)")
    default_build = [r for r in cs_rows if r["build"] != sweep_build]
    if default_build:
        d, _ = summarise("chunks-default-build", "chunk_x", 524288, default_build,
                         latency_extra("chunks", 16, int(default_build[0]["workers"]), 524288, default_build[0]["build"]))
        b1.plot([512], [statistics.median(d)], color=INK, marker="x", markersize=9, linestyle="none", label="512 KiB, default build")
    b1.set_xscale("log", base=2)
    b1.set_xticks(kib)
    b1.set_xticklabels([f"{k} KiB" if k < 1024 else f"{k // 1024} MiB" for k in kib])
    b1.legend(fontsize=8, frameon=False)
    style(b1, f"a. 100 MB, 16 processes, {sw[xs[0]][0]['workers']} workers", "chunk size", "MB/s")
    p50 = [pct(lat.get(("chunks", "huge.iso", 16, int(sw[x][0]["workers"]), x, sweep_build, "get"), []), 0.5) for x in xs]
    p99 = [pct(lat.get(("chunks", "huge.iso", 16, int(sw[x][0]["workers"]), x, sweep_build, "get"), []), 0.99) for x in xs]
    if all(v is not None for v in p50):
        b2.plot(kib, [v / 1000 for v in p50], color=BLUE, marker="o", linewidth=2, label="p50")
        b2.plot(kib, [v / 1000 for v in p99], color=ORANGE, marker="s", linewidth=2, label="p99")
        b2.set_xscale("log", base=2)
        b2.set_yscale("log")
        b2.set_xticks(kib)
        b2.set_xticklabels([f"{k} KiB" if k < 1024 else f"{k // 1024} MiB" for k in kib])
        b2.legend(fontsize=8, frameon=False)
    style(b2, "b. Per-chunk download time", "chunk size", "ms per chunk")
    fig.tight_layout()
    fig.savefig(os.path.join(FIG, "transfer-chunks.png"), dpi=150, facecolor=SURFACE)

fields = []
for s in summary:
    for k in s:
        if k not in fields:
            fields.append(k)
with open(os.path.join(FIG, "transfer.csv"), "w", newline="") as f:
    w = csv.DictWriter(f, fieldnames=fields)
    w.writeheader()
    w.writerows(summary)
for s in summary:
    print(s)
