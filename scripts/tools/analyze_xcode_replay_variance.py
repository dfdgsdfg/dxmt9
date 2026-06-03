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


def collect_samples(
    paths: list[Path],
    metric_names: tuple[str, ...] | list[str],
) -> tuple[dict[str, dict[str, list[float]]], dict[str, set[str]]]:
    """Read multiple Xcode counter CSVs and union rows by Encoder Label.

    Returns:
      samples: { encoder_label: { metric_name: [v1, v2, ...] } }
                where each per-metric list has length <= len(paths)
                depending on which CSVs reported that metric for that row.
      missing: { encoder_label: { csv_filename, ... } }
                CSVs in which an encoder_label that was seen elsewhere did
                not appear. Empty dict when all rows are present in all CSVs.
    """
    samples: dict[str, dict[str, list[float]]] = {}
    rows_per_csv: dict[str, set[str]] = {}
    for path in paths:
        rows = read_xcode_counters(path)
        labels_in_csv: set[str] = set()
        for row in rows:
            label = row["encoder_label"]
            labels_in_csv.add(label)
            bucket = samples.setdefault(label, {name: [] for name in metric_names})
            for name in metric_names:
                value = row["metrics"].get(name)
                if value is None:
                    continue
                bucket[name].append(value)
        rows_per_csv[path.name] = labels_in_csv
    missing: dict[str, set[str]] = {}
    for label in samples:
        absent_from = {
            name for name, labels in rows_per_csv.items() if label not in labels
        }
        if absent_from:
            missing[label] = absent_from
    return samples, missing


def summarize_variance(
    samples: dict[str, dict[str, list[float]]],
) -> dict[str, dict[str, dict[str, Any]]]:
    """Compute per-(encoder, metric) mean / sample stddev / CV%.

    For each (encoder, metric):
      n      = len(values)
      mean   = arithmetic mean if n >= 1 else None
      stddev = sample stddev (n-1 denominator) if n >= 2 else None
      cv_pct = 100 * stddev / mean if n >= 2 and mean > 0 else None
    """
    out: dict[str, dict[str, dict[str, Any]]] = {}
    for label, by_metric in samples.items():
        out[label] = {}
        for metric_name, values in by_metric.items():
            n = len(values)
            mean = statistics.mean(values) if n >= 1 else None
            stddev = statistics.stdev(values) if n >= 2 else None
            if stddev is not None and mean is not None and mean > 0.0:
                cv_pct = 100.0 * stddev / mean
            else:
                cv_pct = None
            out[label][metric_name] = {
                "n": n,
                "mean": mean,
                "stddev": stddev,
                "cv_pct": cv_pct,
            }
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
