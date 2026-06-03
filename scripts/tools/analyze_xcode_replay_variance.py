#!/usr/bin/env python3
"""Analyze Xcode replay variance across multiple counter exports of the same .gputrace.

Reads N >= 3 raw Xcode "Export Encoder Counters" CSVs (all from the same
.gputrace, replayed and exported N times), normalizes the byte-valued
columns to MiB, and reports per-encoder per-metric mean / stddev / CV.

See docs/superpowers/specs/2026-06-03-xcode-replay-variance-design.md.
"""

from __future__ import annotations

import argparse
import csv
import statistics
import sys
from pathlib import Path
from typing import Any


METRIC_MAPPING: dict[str, tuple[str, float]] = {
    "gpu_ms":                     ("GPU Time",                                       1_000_000.0),
    "buffer_write_mib":           ("Buffer Device Memory Bytes Written",             1_048_576.0),
    "vs_buffer_write_mib":        ("VS Buffer Device Memory Bytes Written",          1_048_576.0),
    "vs_invocations":             ("VS Invocations",                                 1.0),
    "tiled_vertex_buffer_mib":    ("Tiled Vertex Buffer Bytes",                      1_048_576.0),
    "tiled_primitive_block_mib":  ("Tiled Vertex Buffer Primitive Blocks Bytes",     1_048_576.0),
}


DEFAULT_METRICS = (
    "gpu_ms",
    "vs_buffer_write_mib",
    "vs_invocations",
    "tiled_vertex_buffer_mib",
    "tiled_primitive_block_mib",
    "buffer_write_mib",
)


def _parse_float(value: Any) -> float | None:
    text = str(value).strip()
    if text == "":
        return None
    try:
        return float(text)
    except ValueError:
        return None


def read_xcode_counters(path: Path) -> list[dict[str, Any]]:
    """Read one raw Xcode counter CSV.

    Returns a list of dicts with:
      encoder_label: str
      metrics: dict[str, float]   (only metrics whose raw column was present
                                    and parseable; missing values are omitted,
                                    not zero-filled.)
    """
    out: list[dict[str, Any]] = []
    with path.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        for raw_row in reader:
            label = (raw_row.get("Encoder Label") or "").strip()
            if not label:
                continue
            metrics: dict[str, float] = {}
            for metric_name, (source_col, divisor) in METRIC_MAPPING.items():
                value = _parse_float(raw_row.get(source_col))
                if value is None:
                    continue
                metrics[metric_name] = value / divisor
            out.append({"encoder_label": label, "metrics": metrics})
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csvs", nargs="+", type=Path,
                        help="N >= 3 Xcode counter CSV paths")
    args = parser.parse_args()
    if len(args.csvs) < 3:
        parser.error("at least 3 CSV paths are required")
    return 0


if __name__ == "__main__":
    sys.exit(main())
