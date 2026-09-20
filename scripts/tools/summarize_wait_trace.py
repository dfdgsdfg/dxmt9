#!/usr/bin/env python3
"""Validate bounded wait evidence; elapsed intervals are not CPU/FPS costs."""
import argparse
import collections
import csv
import io
import json
from pathlib import Path
import re


SITES = {0: "query_receipt", 1: "resource_replay", 2: "buffer_lock",
         3: "buffer_unlock", 4: "query_queue_acquire"}


def load(path):
    lines = Path(path).read_text().splitlines()
    if not lines or not lines[0].startswith("# wait-trace-v1 "):
        raise ValueError("missing versioned header (window may not have closed)")
    meta = {k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", lines[0])}
    required = {"pid", "first", "last", "closed", "overflow", "incomplete",
                "unknown", "owners", "capacity", "mutex_min_ns", "storage", "cv_entries"}
    if not required <= meta.keys():
        raise ValueError("incomplete metadata")
    if (meta["closed"] != 1 or meta["storage"] != 1 or
            any(meta[k] for k in ("overflow", "incomplete", "unknown")) or
            not 0 <= meta["owners"] <= 256 or meta["pid"] <= 0 or
            not 0 < meta["first"] <= meta["last"] or meta["capacity"] != 32768):
        raise ValueError("unqualified closure/storage/identity/capacity")
    end = re.fullmatch(r"# end rows=(\d+)", lines[-1])
    if not end:
        raise ValueError("missing trailer")
    rows = [{k: int(v) for k, v in r.items()} for r in
            csv.DictReader(io.StringIO("\n".join(lines[1:-1])))]
    if len(rows) != int(end[1]) or len(rows) > meta["capacity"]:
        raise ValueError("row count mismatch")
    for r in rows:
        if (any(v < 0 for v in r.values()) or r["site"] not in SITES or
                not 0 < r["begin_ns"] <= r["acquired_ns"] <= r["end_ns"] or
                not r["tid"] or not r["first_frame"] <= r["last_frame"]):
            raise ValueError("invalid row identity/clock")
        if r["site"] == 4:
            if not r["queue"] or r["waits"]:
                raise ValueError("invalid query queue acquisition")
        elif (not 0 < r["owner"] <= meta["owners"] or not r["target"] or
              (r["waits"] and (r["state_before"] != 0 or r["state_after"] == 0))):
            raise ValueError("invalid replay owner or settlement")
        if r["site"] == 0 and r["raw_before"] != r["raw_after"]:
            raise ValueError("receipt changed its captured prefix")
    if sum(r["waits"] for r in rows) != meta["cv_entries"]:
        raise ValueError("missing CV entries")
    return meta, rows


def summarize(meta, rows):
    groups = collections.defaultdict(lambda: dict(rows=0, cv_entries=0,
        mutex_ms=0.0, cv_and_reacquire_ms=0.0, max_cv_ms=0.0))
    excluded = 0
    for r in rows:
        if not meta["first"] <= r["first_frame"] <= r["last_frame"] <= meta["last"]:
            excluded += 1
            continue
        g = groups[SITES[r["site"]]]
        g["rows"] += 1
        g["cv_entries"] += r["waits"]
        g["mutex_ms"] += (r["acquired_ns"] - r["begin_ns"]) / 1e6
        # Excludes predicate-only tails of mutex-only rows, not pretend waits.
        cv = (r["end_ns"] - r["acquired_ns"]) / 1e6 if r["waits"] else 0
        g["cv_and_reacquire_ms"] += cv
        g["max_cv_ms"] = max(g["max_cv_ms"], cv)
    return dict(metadata=meta, sites=dict(groups), excluded_cross_window=excluded,
        qualification="Bounded hooked-site elapsed evidence, not all app waits, "
                      "exclusive CPU, GPU time, lock-holder attribution or FPS savings.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path", type=Path)
    args = parser.parse_args()
    try:
        print(json.dumps(summarize(*load(args.path)), indent=2))
    except (ValueError, KeyError, OSError, TypeError) as error:
        parser.exit(1, f"unqualified wait trace: {error}\n")
