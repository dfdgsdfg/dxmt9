#!/usr/bin/env python3
"""Verify cumulative perf-counter key order and kind identity against a golden."""

from __future__ import annotations

import argparse
import collections
import difflib
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
REPORT_SOURCE = REPO_ROOT / "src" / "dxmt9" / "dxmt9_perf_counters_report.cpp"
GOLDEN = REPO_ROOT / "scripts" / "check" / "perf_counter_schema.golden"

ROW_RE = re.compile(
    r'^\s*\{"(?P<key>[^"]+)",\s*CounterEntry::Kind::(?P<kind>[A-Za-z0-9_]+),'
    r'.*,\s*(?P<percentile>[0-9]+(?:\.[0-9]+)?)\},\s*$',
    re.MULTILINE,
)


def parse_schema(text: str) -> list[str]:
    start = text.find("constexpr CounterEntry kCounterTable[] = {")
    if start < 0:
        raise ValueError("cannot locate kCounterTable")
    end = text.find("\n};", start)
    if end < 0:
        raise ValueError("cannot locate end of kCounterTable")
    table = text[start:end]
    entries: list[str] = []
    for match in ROW_RE.finditer(table):
        kind = match.group("kind")
        identity = kind
        if kind in {"PercentileMs", "PercentileNs"}:
            identity += ":" + match.group("percentile")
        entries.append(f"{match.group('key')}\t{identity}")
    row_count = sum(1 for line in table.splitlines() if line.lstrip().startswith('{"'))
    if len(entries) != row_count:
        raise ValueError(
            f"parsed {len(entries)} of {row_count} kCounterTable rows"
        )
    return entries


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--print-schema",
        action="store_true",
        help="print normalized schema for intentional golden regeneration",
    )
    args = parser.parse_args()

    try:
        entries = parse_schema(REPORT_SOURCE.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        print(f"schema audit: {error}", file=sys.stderr)
        return 2

    counts = collections.Counter(entry.split("\t", 1)[0] for entry in entries)
    duplicates = sorted(key for key, count in counts.items() if count > 1)
    if duplicates:
        print("schema audit: duplicate cumulative counter keys:", file=sys.stderr)
        for key in duplicates:
            print(f"  {key}", file=sys.stderr)
        return 1

    normalized = ["# key\tkind-or-percentile"] + entries
    if args.print_schema:
        print("\n".join(normalized))
        return 0

    if not GOLDEN.exists():
        print(f"schema audit: golden not found: {GOLDEN}", file=sys.stderr)
        return 2
    expected = GOLDEN.read_text(encoding="utf-8").splitlines()
    if expected != normalized:
        print(
            "schema audit: cumulative perf-counter schema/order differs from golden:",
            file=sys.stderr,
        )
        diff = difflib.unified_diff(
            expected,
            normalized,
            fromfile=str(GOLDEN),
            tofile=str(REPORT_SOURCE),
            lineterm="",
        )
        for line in list(diff)[:200]:
            print(line, file=sys.stderr)
        return 1


    print(f"schema audit: ok — {len(entries)} ordered unique counter rows")
    return 0


if __name__ == "__main__":
    sys.exit(main())
