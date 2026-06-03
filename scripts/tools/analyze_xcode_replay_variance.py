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


def _fmt(value: Any) -> str:
    if value is None:
        return ""
    if isinstance(value, float):
        return repr(value)
    return str(value)


def write_summary_csv(
    path: Path,
    summary: dict[str, dict[str, dict[str, Any]]],
    metric_order: list[str],
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fields = ["encoder", "metric", "n", "mean", "stddev", "cv_pct"]
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        for label in sorted(summary.keys()):
            per_metric = summary[label]
            for metric in metric_order:
                if metric not in per_metric:
                    continue
                stats = per_metric[metric]
                writer.writerow({
                    "encoder": label,
                    "metric": metric,
                    "n": stats["n"],
                    "mean": _fmt(stats["mean"]),
                    "stddev": _fmt(stats["stddev"]),
                    "cv_pct": _fmt(stats["cv_pct"]),
                })


def _max_cv(per_metric: dict[str, dict[str, Any]]) -> float:
    best = 0.0
    for stats in per_metric.values():
        cv = stats.get("cv_pct")
        if isinstance(cv, (int, float)) and cv > best:
            best = float(cv)
    return best


def write_report(
    path: Path,
    summary: dict[str, dict[str, dict[str, Any]]],
    missing: dict[str, set[str]],
    metric_order: list[str],
    n_inputs: int,
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    lines: list[str] = []
    lines.append("# Xcode Replay Variance Report")
    lines.append("")
    lines.append(f"inputs: {n_inputs}")
    lines.append("")
    lines.append("## Per-Encoder Variance")
    lines.append("")
    for label in sorted(summary.keys()):
        per_metric = summary[label]
        lines.append(f"## Encoder `{label}`")
        lines.append("")
        lines.append("| Metric | n | Mean | Stddev | CV% |")
        lines.append("|---|---:|---:|---:|---:|")
        for metric in metric_order:
            if metric not in per_metric:
                continue
            stats = per_metric[metric]
            mean = stats["mean"]
            stddev = stats["stddev"]
            cv_pct = stats["cv_pct"]
            mean_text = f"{mean:.6g}" if isinstance(mean, (int, float)) else ""
            stddev_text = f"{stddev:.6g}" if isinstance(stddev, (int, float)) else ""
            cv_text = f"{cv_pct:.2f}" if isinstance(cv_pct, (int, float)) else ""
            lines.append(
                f"| `{metric}` | {stats['n']} | {mean_text} | {stddev_text} | {cv_text} |"
            )
        lines.append("")

    if missing:
        lines.append("## Rows Missing From Some Inputs")
        lines.append("")
        for label in sorted(missing.keys()):
            absent = ", ".join(f"`{name}`" for name in sorted(missing[label]))
            lines.append(f"- `{label}` — missing from: {absent}")
        lines.append("")

    lines.append("## Summary (top variance)")
    lines.append("")
    lines.append("| Encoder | Max CV% across reported metrics |")
    lines.append("|---|---:|")
    ranked = sorted(summary.keys(), key=lambda l: -_max_cv(summary[l]))
    for label in ranked:
        max_cv = _max_cv(summary[label])
        lines.append(f"| `{label}` | {max_cv:.2f} |")
    lines.append("")

    path.write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csvs", nargs="+", type=Path,
                        help="N >= 3 Xcode counter CSV paths")
    parser.add_argument("--output", type=Path, required=True,
                        help="Markdown report path")
    parser.add_argument("--summary-output", type=Path,
                        help="optional reduced per-(encoder, metric) CSV path")
    parser.add_argument("--metric", action="append", default=None,
                        help="repeatable; defaults to a built-in set "
                              "(see METRIC_MAPPING)")
    parser.add_argument("--max-cv-pct", type=float, default=None,
                        help="exit nonzero if any (encoder, metric) has CV%% "
                              "above this limit")
    args = parser.parse_args()
    if len(args.csvs) < 3:
        parser.error("at least 3 CSV paths are required")
    metrics = list(args.metric) if args.metric else list(DEFAULT_METRICS)
    for metric in metrics:
        if metric not in METRIC_MAPPING:
            parser.error(
                f"unknown metric {metric!r}; "
                f"known: {sorted(METRIC_MAPPING.keys())}"
            )
    samples, missing = collect_samples(args.csvs, tuple(metrics))
    summary = summarize_variance(samples)
    write_report(args.output, summary, missing, metrics, len(args.csvs))
    if args.summary_output:
        write_summary_csv(args.summary_output, summary, metrics)
    if args.max_cv_pct is not None:
        violations: list[tuple[str, str, float]] = []
        for label, per_metric in summary.items():
            for metric, stats in per_metric.items():
                cv = stats.get("cv_pct")
                if isinstance(cv, (int, float)) and cv > args.max_cv_pct:
                    violations.append((label, metric, float(cv)))
        if violations:
            for label, metric, cv in violations:
                print(
                    f"requirement failed: encoder {label!r} metric {metric!r} "
                    f"cv_pct={cv:.4f} exceeds --max-cv-pct {args.max_cv_pct:.4f}",
                    file=sys.stderr,
                )
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
