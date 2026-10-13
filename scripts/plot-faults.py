#!/usr/bin/env python3
"""plot-faults.py -- throughput and errors over time, with the faults marked (plan phase 14).

  scripts/plot-faults.py <label> [<label> ...]

Reads bench/raw/faults-<label>-ops.csv and -faults.csv (written by scripts/faults.py). Writes
docs/figures/faults.png (one row of panels per label: bytes moved per second by successful uploads
and downloads, in 2 s bins by completion time, above the failed operations per bin, with every
fault drawn as a shaded span) and docs/figures/faults.csv (per label and fault kind: how many, and
the operations that failed while one was in force).
"""
import csv, os, sys
from collections import defaultdict

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIG = os.path.join(ROOT, "docs", "figures")
RAW = os.path.join(ROOT, "bench", "raw")
SURFACE, INK, INK_2, GRID = "#fcfcfb", "#0b0b0b", "#52514e", "#e4e3df"
BLUE, ORANGE, RED = "#2a78d6", "#eb6834", "#d03b3b"
SHADE = {"kill-node": "#f2c2c2", "stop-node": "#f6dfb4", "kill-leader": "#c9c2ef", "stop-follower": "#c8e3f2", "netem": "#cfe8cf"}
plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 9})
BIN = 2.0

labels = sys.argv[1:]
fig, axes = plt.subplots(2 * len(labels), 1, figsize=(10, 3.4 * len(labels)), dpi=150, sharex=False,
                         gridspec_kw={"height_ratios": [3, 1] * len(labels)})
fig.patch.set_facecolor(SURFACE)
summary = []
for row, label in enumerate(labels):
    ops = list(csv.DictReader(open(os.path.join(RAW, f"faults-{label}-ops.csv"))))
    faults = list(csv.DictReader(open(os.path.join(RAW, f"faults-{label}-faults.csv"))))
    end = max(float(o["end_s"]) for o in ops)
    nb = int(end // BIN) + 1
    up, down, err = [0.0] * nb, [0.0] * nb, [0] * nb
    for o in ops:
        b = int(float(o["end_s"]) // BIN)
        if o["ok"] == "1":
            (up if o["op"] == "put" else down)[b] += int(o["bytes"]) / (1 << 20) / BIN
        else:
            err[b] += 1
    xs = [i * BIN for i in range(nb)]
    top, bottom = axes[2 * row], axes[2 * row + 1]
    for ax in (top, bottom):
        ax.set_facecolor(SURFACE)
        for f in faults:
            ax.axvspan(float(f["start_s"]), float(f["end_s"]), color=SHADE.get(f["kind"], "#dddddd"), lw=0)
        for side in ("top", "right"):
            ax.spines[side].set_visible(False)
        ax.tick_params(colors=INK_2)
        ax.set_xlim(0, end)
    top.step(xs, up, where="post", color=ORANGE, label="uploaded")
    top.step(xs, down, where="post", color=BLUE, label="downloaded")
    top.set_ylabel("MB/s", color=INK)
    top.set_title(f"{label}: throughput of successful operations, faults shaded", color=INK, loc="left", fontsize=10)
    handles = [plt.Rectangle((0, 0), 1, 1, color=c) for k, c in SHADE.items() if any(f["kind"] == k for f in faults)]
    names = [k for k in SHADE if any(f["kind"] == k for f in faults)]
    top.legend(list(top.get_legend_handles_labels()[0]) + handles, ["uploaded", "downloaded"] + names, frameon=False,
               fontsize=7.5, ncol=4, loc="upper right")
    bottom.bar(xs, err, width=BIN, align="edge", color=RED)
    bottom.set_ylim(0, max(1, max(err)) * 1.15)
    bottom.yaxis.get_major_locator().set_params(integer=True)
    bottom.set_ylabel("failed ops", color=INK)
    bottom.set_xlabel("seconds since the workload started", color=INK)
    for kind in sorted({f["kind"] for f in faults}):
        spans = [(float(f["start_s"]), float(f["end_s"])) for f in faults if f["kind"] == kind]
        failed = sum(1 for o in ops if o["ok"] == "0" and any(s <= float(o["end_s"]) <= e + 2 for s, e in spans))
        summary.append({"label": label, "fault": kind, "count": len(spans), "failed_ops_during": failed})
    summary.append({"label": label, "fault": "all", "count": len(faults),
                    "failed_ops_during": sum(1 for o in ops if o["ok"] == "0")})
fig.tight_layout()
fig.savefig(os.path.join(FIG, "faults.png"), facecolor=SURFACE)
with open(os.path.join(FIG, "faults.csv"), "w", newline="") as f:
    w = csv.DictWriter(f, fieldnames=["label", "fault", "count", "failed_ops_during"])
    w.writeheader()
    w.writerows(summary)
for s in summary:
    print(s)
