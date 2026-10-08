#!/usr/bin/env python3
"""plot-reconverge.py -- summarise the reconvergence benchmark and draw its figure.

  scripts/plot-reconverge.py <commit>

Reads bench/raw/reconverge-<commit>.csv and bench/raw/cpu-<commit>.csv (written by
scripts/bench-reconverge.py), and writes:
  docs/figures/reconverge.csv   one row per configuration: median, min and max of each
                                milestone over its runs, and the totals that matter
  docs/figures/reconverge.png   four panels, each with its own y-axis:
     a. time to each milestone against the stabilisation period T (64 nodes, one kill)
     b. CPU per node against T (the price of a short period)
     c. time to each milestone against ring size N, at the chosen T
     d. time to each milestone against the number of nodes killed at once, at the chosen T
Each point is the median of its runs; bars run from the fastest to the slowest run.
"""
import csv, os, statistics, sys
from collections import defaultdict

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
COMMIT = sys.argv[1]
RAW = os.path.join(ROOT, "bench", "raw", f"reconverge-{COMMIT}.csv")
CPU = os.path.join(ROOT, "bench", "raw", f"cpu-{COMMIT}.csv")
OUT_CSV = os.path.join(ROOT, "docs", "figures", "reconverge.csv")
OUT_PNG = os.path.join(ROOT, "docs", "figures", "reconverge.png")

# Three categorical slots, validated together (light surface, adjacent and all pairs).
# Aqua is below 3:1 against the surface, so every series also has its own marker
# shape and a direct label, and the numbers are in the CSV and BENCHMARKS.md.
SURFACE, INK, INK_2, GRID = "#fcfcfb", "#0b0b0b", "#52514e", "#e4e3df"
SERIES = [("ring_s", "ring correct", "#2a78d6", "o"),
          ("lists_s", "successor lists correct", "#eb6834", "s"),
          ("fingers_s", "fingers correct", "#1baf7a", "^")]

rows = list(csv.DictReader(open(RAW)))
groups = defaultdict(list)
for r in rows:
    key = (r["tag"], int(r["n"]), int(r["k"]), int(r["adjacent"]), int(r["period_ms"]), float(r["load_rate"]))
    groups[key].append(r)


def num(v):
    return None if v in ("", "None") else float(v)


summary = []
for key, rs in sorted(groups.items()):
    tag, n, k, adj, period, load = key
    out = {"tag": tag, "n": n, "k": k, "adjacent": adj, "period_ms": period, "load_rate": load,
           "runs": len(rs), "converged": sum(int(r["converged"]) for r in rs)}
    for col, _, _, _ in SERIES:
        vals = [num(r[col]) for r in rs if num(r[col]) is not None]
        name = col[:-2]
        out[f"{name}_median_s"] = round(statistics.median(vals), 3) if vals else ""
        out[f"{name}_min_s"] = round(min(vals), 3) if vals else ""
        out[f"{name}_max_s"] = round(max(vals), 3) if vals else ""
    out["poll_ms_median"] = round(statistics.median(float(r["poll_ms_median"]) for r in rs), 1)
    out["poll_ms_max"] = round(max(float(r["poll_ms_max"]) for r in rs), 1)
    out["false_evictions"] = sum(int(r["false_evictions"]) for r in rs)
    out["orphaned"] = sum(int(r["orphaned"]) for r in rs)
    out["after_wrong"] = sum(int(r["after_wrong"]) for r in rs)
    out["after_lookups"] = sum(int(r["after_lookups"]) for r in rs)
    if load:
        for c in ("load_after", "load_correct", "load_dead_owner", "load_wrong", "load_error", "load_retried"):
            out[c] = sum(int(r[c]) for r in rs)
        out["load_last_bad_s_max"] = max(float(r["load_last_bad_s"]) for r in rs)
    summary.append(out)

fields = []
for s in summary:
    for f in s:
        if f not in fields:
            fields.append(f)
with open(OUT_CSV, "w", newline="") as f:
    w = csv.DictWriter(f, fieldnames=fields)
    w.writeheader()
    w.writerows(summary)
print(f"wrote {OUT_CSV}")


def series_of(tag, xkey):
    """[(x, {milestone: (median, min, max)})] for one tag, sorted by x."""
    pts = []
    for s in summary:
        if s["tag"] != tag:
            continue
        vals = {}
        for col, _, _, _ in SERIES:
            name = col[:-2]
            if s[f"{name}_median_s"] != "":
                vals[col] = (s[f"{name}_median_s"], s[f"{name}_min_s"], s[f"{name}_max_s"])
        pts.append((s[xkey], vals))
    return sorted(pts)


plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 9.5})
fig, axes = plt.subplots(2, 2, figsize=(10, 7.2), dpi=150)
fig.patch.set_facecolor(SURFACE)


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


def milestones(ax, pts, label_last=True):
    for col, label, color, marker in SERIES:
        xs = [x for x, v in pts if col in v]
        med = [v[col][0] for x, v in pts if col in v]
        lo = [v[col][0] - v[col][1] for x, v in pts if col in v]
        hi = [v[col][2] - v[col][0] for x, v in pts if col in v]
        if not xs:
            continue
        ax.errorbar(xs, med, yerr=[lo, hi], color=color, marker=marker, markersize=6, linewidth=2,
                    elinewidth=1.2, capsize=3, markeredgecolor=SURFACE, markeredgewidth=1.2, label=label)
        if label_last:
            ax.annotate(label, (xs[-1], med[-1]), xytext=(6, 0), textcoords="offset points",
                        va="center", color=INK, fontsize=8.5)


chosen = [s for s in summary if s["tag"] == "size"]
chosen_t = chosen[0]["period_ms"] if chosen else None

ax = axes[0][0]
pts = series_of("period", "period_ms")
milestones(ax, pts)
ax.set_xscale("log")
if pts:
    xs = [x for x, _ in pts]
    ax.set_xticks(xs)
    ax.set_xticklabels([str(x) for x in xs])
    ax.set_xlim(xs[0] / 1.3, xs[-1] * 3.5)
    ax.plot(xs, [x / 1000 for x in xs], color=INK_2, linestyle=(0, (4, 3)), linewidth=1)
    ax.annotate("one period", (xs[2], xs[2] / 1000), xytext=(4, -12), textcoords="offset points",
                color=INK_2, fontsize=8)
    ax.axhline(1.0, color=INK_2, linewidth=0.8, linestyle=(0, (1, 2)))
    ax.annotate("1 s target", (xs[0], 1.0), xytext=(0, 4), textcoords="offset points", color=INK_2, fontsize=8)
if chosen_t:
    ax.axvline(chosen_t, color=GRID, linewidth=6, zorder=0)
ax.set_yscale("log")
style(ax, "a · after one kill, against the period (64 nodes)", "stabilisation period T (ms, log scale)",
      "seconds after the kill (log scale)")

ax = axes[0][1]
cpu = [r for r in csv.DictReader(open(CPU))] if os.path.exists(CPU) else []
if cpu:
    cpu.sort(key=lambda r: int(r["period_ms"]))
    xs = [int(r["period_ms"]) for r in cpu]
    ys = [float(r["cpu_pct_per_node"]) for r in cpu]
    ax.plot(xs, ys, color=INK, marker="D", markersize=6, linewidth=2, markeredgecolor=SURFACE)
    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xticks(xs)
    ax.set_xticklabels([str(x) for x in xs])
    ax.yaxis.set_major_locator(matplotlib.ticker.FixedLocator([0.25, 0.5, 1, 2, 4]))
    ax.yaxis.set_minor_locator(matplotlib.ticker.NullLocator())
    ax.yaxis.set_major_formatter(matplotlib.ticker.FuncFormatter(lambda v, _: f"{v:g} %"))
    for x, y in zip(xs, ys):
        ax.annotate(f"{y:.2f} %", (x, y), xytext=(6, 4), textcoords="offset points", color=INK, fontsize=8)
    if chosen_t:
        ax.axvline(chosen_t, color=GRID, linewidth=6, zorder=0)
style(ax, f"b · the price: CPU per idle node ({cpu[0]['n'] if cpu else '?'} nodes)",
      "stabilisation period T (ms, log scale)", "CPU per node, % of one core (log scale)")

ax = axes[1][0]
pts = series_of("size", "n")
milestones(ax, pts)
ax.set_xscale("log", base=2)
if pts:
    xs = [x for x, _ in pts]
    ax.set_xticks(xs)
    ax.set_xticklabels([str(x) for x in xs])
    ax.set_xlim(xs[0] / 1.3, xs[-1] * 3.5)
ax.set_ylim(bottom=0)
style(ax, f"c · after one kill, against ring size (T = {chosen_t} ms)", "ring size N (log scale)",
      "seconds after the kill")

ax = axes[1][1]
# One kill on 64 nodes is the size sweep's 64-node point: same ring, period and deadline.
one = [(1, v) for n, v in series_of("size", "n") if n == 64]
pts = sorted(one + series_of("kills", "k"), key=lambda p: p[0])
milestones(ax, pts)
ax.set_xscale("log", base=2)
if pts:
    xs = [x for x, _ in pts]
    ax.set_xticks(xs)
    ax.set_xticklabels([str(x) for x in xs])
    ax.set_xlim(xs[0] / 1.3, xs[-1] * 3.5)
ax.set_ylim(bottom=0)
style(ax, f"d · k nodes killed at random (64 nodes, T = {chosen_t} ms)",
      "nodes killed together (log scale)", "seconds after the kill")

handles, labels = axes[0][0].get_legend_handles_labels()
leg = fig.legend(handles, labels, loc="upper center", ncol=3, frameon=False, fontsize=9,
                 bbox_to_anchor=(0.5, 1.0))
for t in leg.get_texts():
    t.set_color(INK)
fig.suptitle("Time for the ring to repair itself after nodes are killed  (median of runs; bars: fastest to slowest)",
             color=INK, x=0.01, y=0.955, ha="left", fontsize=11)
fig.tight_layout(rect=(0, 0, 1, 0.93))
fig.savefig(OUT_PNG, facecolor=SURFACE)
print(f"wrote {OUT_PNG}")
