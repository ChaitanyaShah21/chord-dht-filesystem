#!/usr/bin/env python3
"""plot-hops.py -- draw docs/figures/hops-vs-n.png from docs/figures/hops-vs-n.csv.

The CSV is written by scripts/bench-hops.py. One chart, one y-axis:
  - in-process replay (sim-hops): mean as a line, 1st-99th percentile as a band;
  - real processes over TCP: mean as markers, 1st-99th percentile as error bars;
  - reference curves 1/2 log2 N (the Chord paper's prediction) and log2 N.
A "hop" is a finger followed, which is requests - 1 (decision D-026).
"""
import csv, math, os, sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "docs", "figures", "hops-vs-n.csv")
OUT = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "docs", "figures", "hops-vs-n.png")

# Two categorical slots (blue, orange) and recessive inks, checked with a
# colour-vision-deficiency validator against the light surface below.
SURFACE, INK, INK_2, GRID = "#fcfcfb", "#0b0b0b", "#52514e", "#e4e3df"
SIM, REAL = "#2a78d6", "#eb6834"

rows = list(csv.DictReader(open(SRC)))
def series(name):
    rs = sorted((r for r in rows if r["series"] == name), key=lambda r: int(r["n"]))
    return ([int(r["n"]) for r in rs], [float(r["mean_hops"]) for r in rs],
            [int(r["p1_hops"]) for r in rs], [int(r["p99_hops"]) for r in rs])

sn, smean, sp1, sp99 = series("sim")
rn, rmean, rp1, rp99 = series("real")

plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 10})
fig, ax = plt.subplots(figsize=(8, 4.8), dpi=150)
fig.patch.set_facecolor(SURFACE)
ax.set_facecolor(SURFACE)

all_n = sorted(set(sn) | set(rn))
xs = [2 ** (i / 4) for i in range(int(4 * math.log2(all_n[0])), int(4 * math.log2(all_n[-1])) + 1)]
ax.plot(xs, [math.log2(x) for x in xs], color=INK_2, linewidth=1, linestyle=(0, (1, 2)), zorder=1)
ax.plot(xs, [0.5 * math.log2(x) for x in xs], color=INK_2, linewidth=1.2, linestyle=(0, (5, 3)), zorder=1)
ax.annotate("log₂ N", (xs[-1], math.log2(xs[-1])), xytext=(6, 0), textcoords="offset points",
            va="center", color=INK_2)
ax.annotate("½ log₂ N", (xs[-1], 0.5 * math.log2(xs[-1])), xytext=(6, 0),
            textcoords="offset points", va="center", color=INK_2)

if sn:
    ax.fill_between(sn, sp1, sp99, color=SIM, alpha=0.14, linewidth=0, zorder=2,
                    label="in-process replay, 1st–99th percentile")
    ax.plot(sn, smean, color=SIM, linewidth=2, marker="o", markersize=4, zorder=3,
            label="in-process replay (sim-hops), mean")
if rn:
    lower = [m - lo for m, lo in zip(rmean, rp1)]
    upper = [hi - m for m, hi in zip(rmean, rp99)]
    ax.errorbar(rn, rmean, yerr=[lower, upper], fmt="D", color=REAL, markersize=6,
                markeredgecolor=SURFACE, markeredgewidth=1.2, elinewidth=1.4, capsize=3, zorder=4,
                label="real node processes over TCP, mean and 1st–99th percentile")
    ax.axvline(rn[-1], color=GRID, linewidth=1, zorder=0)
    ax.annotate(f"largest real ring: {rn[-1]:,} processes", (rn[-1], ax.get_ylim()[1]),
                xytext=(4, -12), textcoords="offset points", color=INK_2, fontsize=8.5)

ax.set_xscale("log", base=2)
ax.set_xlim(all_n[0] / 1.3, all_n[-1] * 3.2)
ax.set_ylim(bottom=0)
ax.set_xticks([n for n in all_n if int(math.log2(n)) % 2 == 0 or n == all_n[0]])
ax.get_xaxis().set_major_formatter(matplotlib.ticker.FuncFormatter(
    lambda v, _: f"2$^{{{int(round(math.log2(v)))}}}$"))
ax.set_xlabel("Ring size N (nodes, log scale)", color=INK)
ax.set_ylabel("Hops per lookup (fingers followed)", color=INK)
ax.set_title("Chord lookup path length grows as ½ log₂ N", color=INK, loc="left", fontsize=12)
ax.grid(axis="y", color=GRID, linewidth=0.8)
ax.set_axisbelow(True)
for side in ("top", "right"):
    ax.spines[side].set_visible(False)
for side in ("left", "bottom"):
    ax.spines[side].set_color(INK_2)
ax.tick_params(colors=INK_2)
leg = ax.legend(loc="lower right", frameon=False, fontsize=8.5)
for t in leg.get_texts():
    t.set_color(INK)

fig.tight_layout()
fig.savefig(OUT, facecolor=SURFACE)
print(f"wrote {OUT}")
