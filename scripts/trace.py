#!/usr/bin/env python3
"""trace.py -- reconstruct one request from every process's log (decisions D-086, D-087).

    scripts/trace.py <request-id> <log file or directory> [...]
    scripts/trace.py --json <request-id> ...      the matched events, one JSON object per line

Reads every log line written by src/log.cpp (key=value fields, one event per line), keeps
those whose req field is the id, sorts them by timestamp, and prints them with a summary:
which processes the request touched and what each did. Directories are searched for *.log.

The timestamps come from each process's wall clock. On one machine they share a clock;
across machines they would be only as close as the clocks, so the summary groups by
process rather than trusting the order across processes.
"""
import json, os, sys
from collections import Counter, OrderedDict


def parse_line(line):
    """One logfmt line -> dict, or None if it is not one of ours. Quoted values may hold
    spaces, '=' and escaped quotes; the escapes are the ones log.cpp writes."""
    fields, i, n = OrderedDict(), 0, len(line)
    while i < n:
        while i < n and line[i] == " ":
            i += 1
        if i >= n:
            break
        eq = line.find("=", i)
        if eq < 0:
            return None
        key = line[i:eq]
        if not key or " " in key:
            return None
        i = eq + 1
        if i < n and line[i] == '"':
            i += 1
            val = []
            while i < n and line[i] != '"':
                if line[i] == "\\" and i + 1 < n:
                    c = line[i + 1]
                    if c == "x" and i + 3 < n:
                        val.append(chr(int(line[i + 2:i + 4], 16)))
                        i += 4
                        continue
                    val.append({"n": "\n", "r": "\r", "t": "\t"}.get(c, c))
                    i += 2
                    continue
                val.append(line[i])
                i += 1
            if i >= n:
                return None                     # an unterminated quote: not a whole line
            i += 1
            fields[key] = "".join(val)
        else:
            sp = line.find(" ", i)
            sp = n if sp < 0 else sp
            fields[key] = line[i:sp]
            i = sp
    return fields if "ts" in fields and "event" in fields else None


def log_files(paths):
    for p in paths:
        if os.path.isdir(p):
            for dirpath, _, names in os.walk(p):
                for name in sorted(names):
                    if name.endswith(".log"):
                        yield os.path.join(dirpath, name)
        else:
            yield p


def collect(rid, paths):
    events = []
    for path in log_files(paths):
        with open(path, errors="replace") as f:
            for line in f:
                if "req=" + rid not in line:          # cheap filter before parsing
                    continue
                ev = parse_line(line.rstrip("\n"))
                if ev and ev.get("req") == rid:
                    ev["_file"] = os.path.basename(path)
                    events.append(ev)
    events.sort(key=lambda e: e["ts"])
    return events


def main(argv):
    as_json = "--json" in argv
    argv = [a for a in argv if a != "--json"]
    if len(argv) < 2:
        print(__doc__.strip().split("\n\n")[1])
        return 2
    rid, paths = argv[0], argv[1:]
    events = collect(rid, paths)
    if as_json:
        for e in events:
            print(json.dumps(e))
        return 0 if events else 1
    if not events:
        print(f"no events for request {rid}")
        return 1
    skip = {"ts", "level", "proc", "req", "event", "_file"}
    for e in events:
        rest = " ".join(f"{k}={v}" for k, v in e.items() if k not in skip)
        print(f"{e['ts']}  {e['proc']:<14} {e['event']:<20} {rest}")
    print()
    print(f"request {rid}: {len(events)} events in {len({e['proc'] for e in events})} processes")
    by_proc = OrderedDict()
    for e in events:
        by_proc.setdefault(e["proc"], Counter())[e["event"]] += 1
    for proc, counts in by_proc.items():
        print(f"  {proc:<14} " + ", ".join(f"{ev} x{c}" for ev, c in counts.most_common()))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
