#!/usr/bin/env python3
"""plot-gc.py -- summarise the collector's benchmarks and draw their figure.

  scripts/plot-gc.py <before-label> <after-label>

Reads bench/raw/gc-<label>.csv (written by scripts/bench-gc.py) for the commit before the
drop stopped flushing its directory and the commit after. Writes:
  docs/figures/gc.csv   one row per (commit, chunk count): medians of the repetitions
  docs/figures/gc.png   a. copies dropped per second while sweeping, before and after
                        b. a whole collection's time against the chunk copies held: listing what
                           is held (everything live) and sweeping it (everything garbage)
"""
import csv, os, statistics, sys
from collections import defaultdict

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIG = os.path.join(ROOT, "docs", "figures")
RAW = os.path.join(ROOT, "bench", "raw")
SURFACE, INK, INK_2, GRID = "#fcfcfb", "#0b0b0b", "#52514e", "#e4e3df"
BLUE, ORANGE = "#2a78d6", "#eb6834"
plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 9.5})


def style(ax, title, xlabel, ylabel):
    ax.set_facecolor(SURFACE)
    ax.set_title(title, color=INK, loc="left", fontsize=10.5)
    ax.set_xlabel(xlabel, color=INK)
    ax.set_ylabel(ylabel, color=INK)
    ax.grid(axis="y", color=GRID, linewidth=0.8)
    ax.set_axisbelow(True)
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        ax.spines[side].set_color(INK_2)
    ax.tick_params(colors=INK_2)


before, after = sys.argv[1], sys.argv[2]
summary = []
curves = {}
for label in (before, after):
    rows = [r for r in csv.DictReader(open(os.path.join(RAW, f"gc-{label}.csv"))) if r["mode"] == "chunks"]
    by = defaultdict(list)
    for r in rows:
        by[int(r["chunks"])].append(r)
    for n in sorted(by):
        rs = by[n]
        rate = [float(r["dead_dropped"]) / (float(r["dead_sweep_ms"]) / 1000) for r in rs]
        s = {"commit": label, "chunks": n, "copies": int(float(rs[0]["live_held"])), "reps": len(rs),
             "mark_ms": round(statistics.median(float(r["live_mark_ms"]) for r in rs), 1),
             "list_ms": round(statistics.median(float(r["live_sweep_ms"]) for r in rs), 1),
             "sweep_ms": round(statistics.median(float(r["dead_sweep_ms"]) for r in rs), 1),
             "dropped": int(statistics.median(float(r["dead_dropped"]) for r in rs)),
             "garbage": int(statistics.median(float(r["dead_garbage"]) for r in rs)),
             "drops_per_s": round(statistics.median(rate)), "drops_per_s_min": round(min(rate)), "drops_per_s_max": round(max(rate)),
             "deadline_hit": max(int(r["deadline_hit"]) for r in rs)}
        summary.append(s)
    curves[label] = [s for s in summary if s["commit"] == label]

with open(os.path.join(FIG, "gc.csv"), "w", newline="") as f:
    w = csv.DictWriter(f, fieldnames=list(summary[0].keys()))
    w.writeheader()
    w.writerows(summary)

fig, axes = plt.subplots(1, 2, figsize=(10, 3.9), dpi=150)
fig.patch.set_facecolor(SURFACE)
ax = axes[0]
for i, (label, color, name) in enumerate(((before, ORANGE, f"before ({before}): a flush per drop"),
                                          (after, BLUE, f"after ({after})"))):
    xs = [s["copies"] for s in curves[label]]
    ax.plot(xs, [s["drops_per_s"] for s in curves[label]], "o-", color=color, label=name)
    ax.vlines(xs, [s["drops_per_s_min"] for s in curves[label]], [s["drops_per_s_max"] for s in curves[label]], color=color)
ax.set_xscale("log")
ax.set_yscale("log")
ax.legend(frameon=False, fontsize=8.5)
style(ax, "a. Copies dropped per second while sweeping", "chunk copies held (8 nodes, 3 copies each)", "drops / s")
ax = axes[1]
xs = [s["copies"] for s in curves[after]]
ax.plot(xs, [s["list_ms"] / 1000 for s in curves[after]], "o-", color=BLUE, label="all live: list every node's chunks")
ax.plot(xs, [s["sweep_ms"] / 1000 for s in curves[after]], "s-", color=ORANGE, label="all garbage: list and drop them")
ax.set_xscale("log")
ax.legend(frameon=False, fontsize=8.5)
style(ax, f"b. One collection's time ({after})", "chunk copies held", "seconds")
fig.tight_layout()
fig.savefig(os.path.join(FIG, "gc.png"), facecolor=SURFACE)
for s in summary:
    print(s)
