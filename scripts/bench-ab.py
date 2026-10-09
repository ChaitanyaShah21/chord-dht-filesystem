#!/usr/bin/env python3
"""bench-ab.py -- one transfer point at two commits, alternating, so drift on the machine hits both alike.

    scripts/bench-ab.py <worktree A> <worktree B> [--file huge.iso] [--workers 8] [--reps 10]

Each worktree is a clean checkout of a commit, built, with its testdata/ made; its own
scripts/bench-transfer.py is loaded and its `one()` run -- the same fresh ring of 16 wired
processes x 8 members, upload, download and SHA-1 check as BENCHMARKS.md §5 -- for A, then B,
then A again, and so on. Prints each run and the medians, minimums and maximums.
"""
import argparse, importlib.util, os, statistics, sys

MB = 1 << 20


def load(tree, name):
    spec = importlib.util.spec_from_file_location(name, os.path.join(tree, "scripts", "bench-transfer.py"))
    sys.path.insert(0, os.path.join(tree, "scripts"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    sys.path.pop(0)
    sys.modules.pop("chunkcheck", None)          # each tree imports its own copy
    return mod


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--file", default="huge.iso")
    ap.add_argument("--workers", type=int, default=8)
    ap.add_argument("--reps", type=int, default=10)
    ap.add_argument("--scratch", default=os.path.join(os.environ.get("TMPDIR", "/tmp"), "bench-ab"))
    a = ap.parse_args()
    benches = {}
    for label, tree in (("A", a.a), ("B", a.b)):
        mod = load(tree, f"bench_transfer_{label}")
        args = argparse.Namespace(reps=a.reps, default_workers=a.workers, bin_dir="", scratch=os.path.join(a.scratch, label),
                                  workers_curve=False, sizes=False, procs_curve=False, chunk_sweep=False, dedup=False, tracker="")
        os.makedirs(args.scratch, exist_ok=True)
        benches[label] = (mod, mod.Bench(args))
    results = {"A": [], "B": []}
    for rep in range(a.reps):
        for label in ("A", "B"):
            mod, b = benches[label]
            row = b.one("ab", a.file, 16, a.workers, rep)
            up = row["bytes"] / MB / (float(row["put_total_ms"]) / 1000)
            down = row["bytes"] / MB / (float(row["get_ready_ms"]) / 1000)
            results[label].append((up, down))
            print(f"{label} rep {rep}: upload {up:7.1f} MB/s  download {down:7.1f} MB/s", flush=True)
    for label, tree in (("A", a.a), ("B", a.b)):
        ups = [r[0] for r in results[label]]
        downs = [r[1] for r in results[label]]
        print(f"{label} {tree}: upload median {statistics.median(ups):.1f} ({min(ups):.1f}-{max(ups):.1f}) MB/s; "
              f"download median {statistics.median(downs):.1f} ({min(downs):.1f}-{max(downs):.1f}) MB/s")


if __name__ == "__main__":
    main()
