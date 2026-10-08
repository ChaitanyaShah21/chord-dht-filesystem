#!/usr/bin/env python3
"""plot-chunks.py -- summarise the Phase 4 benchmarks and draw their figures.

  scripts/plot-chunks.py <commit>

Reads bench/raw/{balance,balance-check,balance-cpu,wsweep,crash,repair}-<commit>.csv (written
by scripts/bench-chunks.py) and writes:
  docs/figures/chunks.csv            one row per summarised configuration
  docs/figures/chunks-balance.png    a. spread of stored copies per process against virtual nodes
                                        per process (coefficient of variation), 16 and 64 processes
                                     b. idle CPU per process against virtual nodes per process
  docs/figures/chunks-wsweep.png     the coordinator's time to answer a write (median and 99th
                                     percentile, from its own log, so the client's lookup is left
                                     out) and the fraction acknowledged, for W = 1, 2, 3, per phase
Points are medians; bars run from the smallest to the largest of the runs or address sets.
"""
import csv, os, statistics, sys
from collections import defaultdict

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
COMMIT = sys.argv[1]
RAW = lambda name: os.path.join(ROOT, "bench", "raw", f"{name}-{COMMIT}.csv")
FIG = os.path.join(ROOT, "docs", "figures")

# The palette already used by reconverge.png: three categorical slots validated together,
# each series also with its own marker shape and a direct label.
SURFACE, INK, INK_2, GRID = "#fcfcfb", "#0b0b0b", "#52514e", "#e4e3df"
BLUE, ORANGE, GREEN = "#2a78d6", "#eb6834", "#1baf7a"


def read(name):
    p = RAW(name)
    return list(csv.DictReader(open(p))) if os.path.exists(p) else []


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

# ---- balance ----
bal = read("balance")
cpu = read("balance-cpu")
check = read("balance-check")
if bal:
    fig, (a, b) = plt.subplots(1, 2, figsize=(10, 4), facecolor=SURFACE)
    for P, color, marker in ((16, BLUE, "o"), (64, ORANGE, "s")):
        vs = sorted({int(r["V"]) for r in bal if int(r["P"]) == P})
        med, lo, hi = [], [], []
        for v in vs:
            xs = [float(r["stored_cv"]) for r in bal if int(r["P"]) == P and int(r["V"]) == v]
            med.append(statistics.median(xs)); lo.append(min(xs)); hi.append(max(xs))
            owned = [float(r["owned_cv"]) for r in bal if int(r["P"]) == P and int(r["V"]) == v]
            mm = [float(r["stored_max_over_mean"]) for r in bal if int(r["P"]) == P and int(r["V"]) == v]
            short = [float(r["short"]) for r in bal if int(r["P"]) == P and int(r["V"]) == v]
            summary.append({"table": "balance", "P": P, "V": v, "sets": len(xs),
                            "stored_cv_median": round(statistics.median(xs), 4), "stored_cv_min": round(min(xs), 4),
                            "stored_cv_max": round(max(xs), 4), "owned_cv_median": round(statistics.median(owned), 4),
                            "stored_max_over_mean_median": round(statistics.median(mm), 3),
                            "short_sets_max": max(short)})
        a.errorbar(vs, med, yerr=[[m - l for m, l in zip(med, lo)], [h - m for m, h in zip(med, hi)]], color=color,
                   marker=marker, markersize=6, linewidth=2, capsize=3, label=f"{P} processes")
    a.axhline(0.25, color=INK_2, linewidth=1, linestyle="--")
    a.annotate("rule: 0.25", (1, 0.25), textcoords="offset points", xytext=(2, 4), fontsize=8, color=INK_2)
    a.set_xscale("log", base=2)
    a.legend(fontsize=8, frameon=False)
    style(a, "a. Spread of stored copies across processes", "virtual nodes per process (V)",
          "coefficient of variation (std / mean)")
    if cpu:
        vs = [int(r["V"]) for r in cpu]
        ys = [float(r["cpu_pct"]) for r in cpu]
        b.plot(vs, ys, color=GREEN, marker="^", markersize=7, linewidth=2)
        for r in cpu:
            summary.append({"table": "balance-cpu", "P": int(r["processes"]), "V": int(r["V"]),
                            "cpu_pct_per_process": float(r["cpu_pct"]), "converged": int(r["converged"])})
        b.set_xscale("log", base=2)
    style(b, "b. Idle CPU per process (16 processes, T = 1 s)", "virtual nodes per process (V)", "% of one core")
    fig.tight_layout()
    fig.savefig(os.path.join(FIG, "chunks-balance.png"), dpi=150, facecolor=SURFACE)
for r in check:
    summary.append({"table": "balance-check", "V": int(r["V"]), "chunks": int(r["chunks"]), "exact": int(r["exact"])})

# ---- W sweep ----
ws = read("wsweep")
if ws:
    phases = ["healthy", "frozen", "killed"]
    fig, axes = plt.subplots(1, 3, figsize=(12, 4), facecolor=SURFACE, sharey=True)
    for ax, ph in zip(axes, phases):
        for w, color, marker in ((1, BLUE, "o"), (2, ORANGE, "s"), (3, GREEN, "^")):
            rs = [r for r in ws if r["phase"] == ph and int(r["w"]) == w]
            ok = sorted(float(r["latency_ms"]) for r in rs if int(r["ok"]))
            pct = lambda q: ok[min(len(ok) - 1, int(q * len(ok)))] if ok else None
            # The coordinator's own time to answer, every write (acknowledged or not) whose
            # coordinator logged it: this isolates W from the client's lookup.
            co = sorted(float(r["answered_ms"]) for r in rs if r.get("answered_ms") not in (None, ""))
            cpct = lambda q: co[min(len(co) - 1, int(q * len(co)))] if co else None
            windows = [int(r["last_copy_ms"]) - int(r["answered_ms"]) for r in rs
                       if int(r["ok"]) and r.get("answered_ms") not in (None, "")]
            summary.append({"table": "wsweep", "phase": ph, "W": w, "writes": len(rs),
                            "acked": sum(int(r["ok"]) for r in rs),
                            "client_latency_p50_ms": pct(0.5), "client_latency_p99_ms": pct(0.99),
                            "coordinator_answer_p50_ms": cpct(0.5), "coordinator_answer_p99_ms": cpct(0.99),
                            "coordinator_logged": len(co),
                            "window_median_ms": statistics.median(windows) if windows else "",
                            "window_max_ms": max(windows) if windows else ""})
            if co:
                lo, hi = max(cpct(0.5), 0.5), max(cpct(0.99), 0.5)       # 0 ms cannot sit on a log axis
                ax.plot([w, w], [lo, hi], color=color, linewidth=2)
                ax.plot([w], [lo], color=color, marker=marker, markersize=8)
                ax.plot([w], [hi], color=color, marker=marker, markersize=8, fillstyle="none")
            frac = f"{sum(int(r['ok']) for r in rs)}/{len(rs)} acked"
            ax.annotate(frac, (w, max(cpct(0.99), 0.5) if co else 1), textcoords="offset points", xytext=(0, 8),
                        ha="center", fontsize=8, color=INK_2)
        ax.set_xticks([1, 2, 3])
        ax.set_xlim(0.5, 3.5)
        ax.set_yscale("log")
        style(ax, {"healthy": "a. Healthy", "frozen": "b. One replica frozen", "killed": "c. One replica killed"}[ph],
              "W (copies before acknowledging)", "ms to answer (filled: median, open: p99)"
              if ph == "healthy" else "")
    fig.tight_layout()
    fig.savefig(os.path.join(FIG, "chunks-wsweep.png"), dpi=150, facecolor=SURFACE)

# ---- coordinator crash ----
cr = read("crash")
for w in (1, 2, 3):
    rs = [r for r in cr if int(r["w"]) == w and r["acked"] == "1"]
    if rs:
        summary.append({"table": "crash", "W": w, "trials": len(rs),
                        "zero_other_copies_at_kill": sum(1 for r in rs if int(r["other_copies_at_kill"]) == 0),
                        "zero_other_copies_after_250ms": sum(1 for r in rs if int(r["other_copies_after_250ms"]) == 0),
                        "repair_ran_in_window": sum(int(r["repair_ran_in_window"]) for r in rs),
                        "readable_after_restart": sum(int(r["readable_after_restart"]) for r in rs),
                        "kill_after_ack_ms_median": statistics.median(float(r["kill_after_ack_ms"]) for r in rs)})

# ---- repair ----
rp = read("repair")
for v in sorted({int(r["vnodes"]) for r in rp}):
    rs = [r for r in rp if int(r["vnodes"]) == v and r["rf_restored_s"] != ""]
    if rs:
        rf = [float(r["rf_restored_s"]) for r in rs]
        ring = [float(r["ring_s"]) for r in rs if r["ring_s"] != ""]
        summary.append({"table": "repair", "V": v, "runs": len(rs),
                        "rf_restored_median_s": round(statistics.median(rf), 3), "rf_restored_min_s": min(rf),
                        "rf_restored_max_s": max(rf), "ring_median_s": round(statistics.median(ring), 3) if ring else "",
                        "victim_held_median": statistics.median(int(r["victim_held"]) for r in rs),
                        "processes_sending_median": statistics.median(int(r["processes_sending"]) for r in rs)})

fields = []
for s in summary:
    for f in s:
        if f not in fields:
            fields.append(f)
with open(os.path.join(FIG, "chunks.csv"), "w", newline="") as f:
    w = csv.DictWriter(f, fieldnames=fields)
    w.writeheader()
    w.writerows(summary)
for s in summary:
    print(s)
