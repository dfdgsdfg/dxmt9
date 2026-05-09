#!/usr/bin/env python3
"""Audit dxmt9_perf_counters.cpp for missing kCounterTable entries.

Detects the silent-miss regression: a new field added to the `Counters`
struct that is never referenced from the `kCounterTable`. Such a field
would never appear in the `[dxmt9-perf]` line and would break tests that
look for it.

Parsing is text-based (no clang/AST dependency) so this runs as a
deterministic Meson test target.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
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

    print(
        f"audit: ok — {len(atomic_fields)} atomic + {len(ring_fields)} "
        f"PercentileRing fields all referenced from kCounterTable "
        f"(total references: {len(referenced)})"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
