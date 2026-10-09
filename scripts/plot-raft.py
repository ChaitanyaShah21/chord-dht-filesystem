#!/usr/bin/env python3
"""plot-raft.py -- summarise the replicated tracker's benchmarks and draw their figures.

  scripts/plot-raft.py <label>

Reads bench/raw/raft-<label>-election.csv and bench/raw/raft-<label>-writes.csv (written by
scripts/bench-raft.py). Writes:
  docs/figures/raft.csv            one row per summarised configuration
  docs/figures/raft-election.png   a. election time after the leader is killed, against E: every kill,
                                      the median, and the band [E, 2E) a follower's timeout is drawn from
                                   b. the client's unavailability: kill -> first acknowledged registration
  docs/figures/raft-writes.png     a. registrations per second, one client and many, disk and tmpfs
                                   b. registration latency at one client, p50 and p99
Medians of the repetitions; bars on throughput run from the slowest to the fastest repetition.
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
BLUE, ORANGE, GREEN, PURPLE = "#2a78d6", "#eb6834", "#1baf7a", "#8a5cd1"

label = sys.argv[1]
el = list(csv.DictReader(open(os.path.join(RAW, f"raft-{label}-election.csv"))))
wr = list(csv.DictReader(open(os.path.join(RAW, f"raft-{label}-writes.csv"))))

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


summary = []

# ---- election ----
by_e = defaultdict(list)
for r in el:
    by_e[int(r["election"])].append(r)
es = sorted(by_e)
fig, axes = plt.subplots(1, 2, figsize=(10, 3.9), dpi=150)
fig.patch.set_facecolor(SURFACE)
for ax, col, title, color in ((axes[0], "elect_ms", "a. A new leader, after the leader is killed", BLUE),
                              (axes[1], "unavailable_ms", "b. The client's first acknowledged write after the kill", ORANGE)):
    for x, e in enumerate(es):
        vals = [float(r[col]) for r in by_e[e] if r[col] not in ("", "nan")]
        ax.scatter([x + (i % 5 - 2) * 0.04 for i in range(len(vals))], vals, s=12, color=color, alpha=0.55, zorder=3)
        ax.plot([x - 0.25, x + 0.25], [statistics.median(vals)] * 2, color=INK, linewidth=2, zorder=4)
        ax.text(x + 0.28, statistics.median(vals), f"{statistics.median(vals):.0f}", va="center", color=INK, fontsize=8.5)
        if col == "elect_ms":
            ax.add_patch(plt.Rectangle((x - 0.32, e), 0.64, e, color=GRID, alpha=0.7, zorder=1))
    ax.set_xticks(range(len(es)), [str(e) for e in es])
    style(ax, title, "election timeout base E (ms)", "milliseconds")
    ax.set_ylim(bottom=0)
axes[0].text(0.02, 0.97, "grey: [E, 2E), where a follower's timeout is drawn\nbar: median of the kills", transform=axes[0].transAxes,
             va="top", color=INK_2, fontsize=8)
fig.tight_layout()
fig.savefig(os.path.join(FIG, "raft-election.png"), facecolor=SURFACE)
for e in es:
    rs = by_e[e]
    ev = [float(r["elect_ms"]) for r in rs]
    uv = [float(r["unavailable_ms"]) for r in rs]
    summary.append({"what": "election", "config": f"E={e}", "n": len(rs), "median": round(statistics.median(ev), 1),
                    "p99_or_max": round(max(ev), 1), "min": round(min(ev), 1), "extra": f"unavailable median {statistics.median(uv):.0f} ms, max {max(uv):.0f} ms"})

# ---- writes ----
groups = defaultdict(list)
for r in wr:
    groups[(r["config"], r["media"], int(r["clients"]))].append(r)
configs = []
for r in wr:
    if r["config"] not in configs:
        configs.append(r["config"])
many = max(int(r["clients"]) for r in wr)
fig, axes = plt.subplots(1, 2, figsize=(10.5, 4.2), dpi=150)
fig.patch.set_facecolor(SURFACE)
series = [("disk", 1, BLUE, "disk, 1 client"), ("disk", many, ORANGE, f"disk, {many} clients"),
          ("tmpfs", 1, GREEN, "tmpfs, 1 client"), ("tmpfs", many, PURPLE, f"tmpfs, {many} clients")]
w = 0.2
for k, (media, clients, color, name) in enumerate(series):
    xs, ys, lo, hi = [], [], [], []
    for x, c in enumerate(configs):
        rs = groups.get((c, media, clients))
        if not rs:
            continue
        v = [float(r["per_second"]) for r in rs]
        m = statistics.median(v)
        xs.append(x + (k - 1.5) * w)
        ys.append(m)
        lo.append(m - min(v))
        hi.append(max(v) - m)
        summary.append({"what": "registrations/s", "config": f"{c}, {media}, {clients} client(s)", "n": len(rs), "median": round(m, 1),
                        "p99_or_max": round(max(v), 1), "min": round(min(v), 1),
                        "extra": f"latency p50 {statistics.median(float(r['w_p50_ms']) for r in rs):.2f} ms p99 "
                                 f"{statistics.median(float(r['w_p99_ms']) for r in rs):.2f} ms; resolve p50 "
                                 f"{statistics.median(float(r['r_p50_ms']) for r in rs):.3f} ms"})
    axes[0].bar(xs, ys, w, color=color, label=name, yerr=[lo, hi], ecolor=INK_2, capsize=2, zorder=3)
axes[0].set_xticks(range(len(configs)), [c.replace(" (before)", "\n(before)") for c in configs], fontsize=8.5)
axes[0].set_yscale("log")
style(axes[0], "a. Registrations per second", "", "registrations / s (log scale)")
axes[0].legend(frameon=False, fontsize=8, loc="upper right")
for k, (media, color) in enumerate((("disk", BLUE), ("tmpfs", GREEN))):
    xs, p50, p99 = [], [], []
    for x, c in enumerate(configs):
        rs = groups.get((c, media, 1))
        if not rs:
            continue
        xs.append(x + (k - 0.5) * 0.35)
        p50.append(statistics.median(float(r["w_p50_ms"]) for r in rs))
        p99.append(statistics.median(float(r["w_p99_ms"]) for r in rs))
    axes[1].bar(xs, p50, 0.33, color=color, label=f"{media}: p50", zorder=3)
    axes[1].scatter(xs, p99, color=INK, marker="_", s=180, zorder=4, label=f"{media}: p99" if k == 0 else None)
axes[1].set_xticks(range(len(configs)), [c.replace(" (before)", "\n(before)") for c in configs], fontsize=8.5)
style(axes[1], "b. One registration, one client", "", "milliseconds")
axes[1].legend(frameon=False, fontsize=8, loc="upper left")
fig.tight_layout()
fig.savefig(os.path.join(FIG, "raft-writes.png"), facecolor=SURFACE)

with open(os.path.join(FIG, "raft.csv"), "w", newline="") as f:
    wtr = csv.DictWriter(f, fieldnames=list(summary[0].keys()))
    wtr.writeheader()
    wtr.writerows(summary)
print("wrote docs/figures/raft.csv, raft-election.png, raft-writes.png")
for s in summary:
    print(s)
