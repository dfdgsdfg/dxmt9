#!/usr/bin/env python3
"""Audit dxmt9_perf_counters.cpp for counter fields that are wired only halfway.

Two directions, because each catches a different silent failure:

1. FIELD -> TABLE. A field in `Counters` never referenced from `kCounterTable`
   never appears in the `[dxmt9-perf]` line, so the counter increments into
   nothing.

2. TABLE -> WRITER. A field referenced from `kCounterTable` that no `add()` or
   `store()` in this file ever writes reports 0.000 forever. That is worse than
   a missing row: the metric looks measured and negligible instead of absent.
   This direction was added after Task 10 deleted countDrawPacketActualChange
   and left fifteen `draw_packet_*` fields and their table rows behind. Neither
   existing audit saw it -- the table audit passed because the rows were present,
   and the callsite audit had nothing to check because the count*() declaration
   had gone with the function.

Parsing is text-based (no clang/AST dependency) so this runs as a
deterministic Meson test target.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SOURCE = REPO_ROOT / "src" / "dxmt9" / "dxmt9_perf_counters.cpp"

# Atomic field within the `Counters` struct: `std::atomic<...> NAME{0};`
ATOMIC_FIELD_RE = re.compile(
    r"^\s*std::atomic<\s*std::uint64_t\s*>\s+([A-Za-z_][A-Za-z0-9_]*)\s*\{",
    re.MULTILINE,
)

# PercentileRing field within the `Counters` struct: `PercentileRing NAME;`
RING_FIELD_RE = re.compile(
    r"^\s*PercentileRing\s+([A-Za-z_][A-Za-z0-9_]*)\s*;",
    re.MULTILINE,
)

# Reference to a Counters member from the table: `&Counters::NAME`.
COUNTER_REF_RE = re.compile(r"&Counters::([A-Za-z_][A-Za-z0-9_]*)")

# Occurrences of a field name anywhere in the file. Deliberately NOT a
# write-idiom match: the writers here use many shapes (add(), addMax(), direct
# .store/.fetch_add, PercentileRing .add, helpers taking the field by
# reference), and a regex over those produced hundreds of false positives when
# tried. What holds regardless of shape is that a writer must NAME the field. A
# field that occurs exactly twice -- its declaration and its one table row --
# therefore has no writer at all.
FIELD_NAME_RE_CACHE: dict[str, re.Pattern[str]] = {}


def parse_counters_struct(text: str) -> tuple[set[str], set[str]]:
    """Return (atomic field names, percentile ring field names)."""
    start = text.find("struct Counters {")
    if start < 0:
        raise SystemExit("audit: cannot locate `struct Counters {` in source")
    # End at the matching closing brace + semicolon.
    end = text.find("\n};", start)
    if end < 0:
        raise SystemExit("audit: cannot locate end of `struct Counters`")
    body = text[start:end]
    atomic_fields = set(ATOMIC_FIELD_RE.findall(body))
    ring_fields = set(RING_FIELD_RE.findall(body))
    return atomic_fields, ring_fields


def parse_table_references(text: str) -> set[str]:
    """Return the set of Counters member names referenced from any table row."""
    return set(COUNTER_REF_RE.findall(text))


def field_mentions(text: str, name: str) -> int:
    """How many times `name` appears as a whole word in the file."""
    pattern = FIELD_NAME_RE_CACHE.get(name)
    if pattern is None:
        pattern = re.compile(r"\b" + re.escape(name) + r"\b")
        FIELD_NAME_RE_CACHE[name] = pattern
    return len(pattern.findall(text))


def main() -> int:
    if not SOURCE.exists():
        print(f"audit: source not found: {SOURCE}", file=sys.stderr)
        return 2
    text = SOURCE.read_text(encoding="utf-8")

    atomic_fields, ring_fields = parse_counters_struct(text)
    referenced = parse_table_references(text)

    all_fields = atomic_fields | ring_fields
    missing = sorted(all_fields - referenced)

    if missing:
        print(
            "audit: kCounterTable is missing rows for the following Counters fields:",
            file=sys.stderr,
        )
        for name in missing:
            kind = "PercentileRing" if name in ring_fields else "atomic"
            print(f"  {name}  ({kind})", file=sys.stderr)
        print(
            f"\naudit: {len(missing)} field(s) declared in Counters but never\n"
            f"referenced from kCounterTable. New counters need both:\n"
            f"  1) the Counters field, AND\n"
            f"  2) one row in kCounterTable referencing the field.\n"
            f"Without both, the counter increments silently and never appears\n"
            f"in the [dxmt9-perf] line.",
            file=sys.stderr,
        )
        return 1

    # Direction 2: a table row whose field nothing ever writes reports 0 forever.
    # Declaration + table row = 2 mentions; a writer would make a third.
    never_written = sorted(
        name for name in (all_fields & referenced)
        if field_mentions(text, name) <= 2
    )
    if never_written:
        print(
            "audit: the following Counters fields have a kCounterTable row but\n"
            "       are never written by any count*() implementation:",
            file=sys.stderr,
        )
        for name in never_written:
            print(f"  {name}", file=sys.stderr)
        print(
            f"\naudit: {len(never_written)} field(s) will report 0 on every\n"
            f"[dxmt9-perf] line. That reads as \"measured, negligible\" rather than\n"
            f"\"not measured\", which is why this is an error and not a warning.\n"
            f"Either restore the writer or delete the field and its table row.\n"
        f"(Detected by name-occurrence: declaration + table row and nothing\n"
        f"else, so no writer of any shape mentions the field.)",
            file=sys.stderr,
        )
        return 1

    print(
        f"audit: ok — {len(atomic_fields)} atomic + {len(ring_fields)} "
        f"PercentileRing fields all referenced from kCounterTable and written "
        f"(total references: {len(referenced)})"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
