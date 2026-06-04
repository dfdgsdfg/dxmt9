#!/usr/bin/env python3
"""Summarize primitive-conflict selector candidates.

This is a narrow helper for 3DMark05 GT1 mini-replay analysis. It consumes the
aggregate CSV produced from per-draw primitive conflict summaries and evaluates
whether simple runtime-shaped metrics can separate semantic failures from exact
passes.
"""

from __future__ import annotations

import argparse
import csv
from pathlib import Path
from typing import Any


METRICS = (
    ("owner_changed_pixels", "pixels", "Owner px"),
    ("color_changed_pixels", "color_changed_pixels", "Color px"),
    ("max_color_delta", "max_color_delta", "Max color"),
    ("max_abs_depth_delta", "max_abs_depth_delta", "Max depth"),
    ("max_uv0_delta", "max_uv0_delta", "Max UV0"),
    ("max_projected_tex7_delta", "max_projected_tex7_delta", "Max tex7"),
    ("max_tex1_delta", "max_tex1_delta", "Max tex1"),
    ("max_tex6_delta", "max_tex6_delta", "Max tex6"),
)


def as_float(value: Any) -> float:
    try:
        return float(str(value))
    except (TypeError, ValueError):
        return 0.0


def fmt(value: float) -> str:
    if value == int(value):
        return str(int(value))
    return f"{value:.9f}".rstrip("0").rstrip(".")


def load_rows(path: Path) -> list[dict[str, str]]:
    if not path.exists():
        raise SystemExit(f"missing CSV: {path}")
    with path.open(newline="", encoding="utf-8") as handle:
        rows = list(csv.DictReader(handle))
    if not rows:
        raise SystemExit(f"empty CSV: {path}")
    if "semantic_status" not in rows[0]:
        raise SystemExit("CSV must contain semantic_status")
    return rows


def split_rows(rows: list[dict[str, str]]) -> tuple[list[dict[str, str]], list[dict[str, str]]]:
    failures = [row for row in rows if row.get("semantic_status") == "fail"]
    passes = [row for row in rows if row.get("semantic_status") == "pass"]
    if not failures:
        raise SystemExit("CSV has no failing rows")
    if not passes:
        raise SystemExit("CSV has no passing rows")
    return failures, passes


def metric_verdict(fail_values: list[float], pass_values: list[float]) -> tuple[str, str]:
    fail_min = min(fail_values)
    fail_max = max(fail_values)
    pass_min = min(pass_values)
    pass_max = max(pass_values)
    if fail_min > 0 and pass_max == 0:
        return "exact-fail-only-positive", f"fail > 0 and all pass rows are 0"
    if fail_min > pass_max:
        threshold = (fail_min + pass_max) / 2.0
        return "threshold-high-separates", f"value > {fmt(threshold)} separates failures"
    if fail_max < pass_min:
        threshold = (fail_max + pass_min) / 2.0
        return "threshold-low-separates", f"value < {fmt(threshold)} separates failures"
    return "overlap", "pass and fail ranges overlap"


def summarize_metric(
    rows: list[dict[str, str]],
    failures: list[dict[str, str]],
    passes: list[dict[str, str]],
    key: str,
    field: str,
    label: str,
) -> dict[str, str]:
    fail_values = [as_float(row.get(field)) for row in failures]
    pass_values = [as_float(row.get(field)) for row in passes]
    verdict, reason = metric_verdict(fail_values, pass_values)
    return {
        "metric": key,
        "label": label,
        "fail_min": fmt(min(fail_values)),
        "fail_max": fmt(max(fail_values)),
        "pass_min": fmt(min(pass_values)),
        "pass_max": fmt(max(pass_values)),
        "verdict": verdict,
        "reason": reason,
    }


def summarize(rows: list[dict[str, str]]) -> list[dict[str, str]]:
    failures, passes = split_rows(rows)
    return [
        summarize_metric(rows, failures, passes, key, field, label)
        for key, field, label in METRICS
        if field in rows[0]
    ]


def decision(metrics: list[dict[str, str]]) -> tuple[str, str]:
    exact = [row["metric"] for row in metrics if row["verdict"] != "overlap"]
    non_color = [
        row["metric"] for row in metrics
        if row["metric"] not in {"color_changed_pixels", "max_color_delta"}
        and row["verdict"] != "overlap"
    ]
    color = [
        row["metric"] for row in metrics
        if row["metric"] in {"color_changed_pixels", "max_color_delta"}
        and row["verdict"] != "overlap"
    ]
    if color and not non_color:
        return (
            "final-color-oracle-required",
            "only final-color metrics separate fail rows from exact passes",
        )
    if exact:
        return (
            "candidate-runtime-selector-found",
            f"non-color separating metrics: {', '.join(non_color) or 'none'}",
        )
    return (
        "no-simple-selector",
        "all candidate metric ranges overlap",
    )


def write_csv(path: Path, metrics: list[dict[str, str]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fields = ("metric", "label", "fail_min", "fail_max", "pass_min", "pass_max", "verdict", "reason")
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(metrics)


def write_markdown(path: Path, input_csv: Path, rows: list[dict[str, str]], metrics: list[dict[str, str]]) -> None:
    failures, passes = split_rows(rows)
    verdict, reason = decision(metrics)
    lines = [
        "# Primitive Conflict Selector Summary",
        "",
        f"- Input: `{input_csv}`",
        f"- Pass rows: `{len(passes)}`",
        f"- Fail rows: `{len(failures)}`",
        f"- Decision: `{verdict}`",
        f"- Reason: {reason}.",
        "",
        "## Metric Ranges",
        "",
        "| Metric | Fail range | Pass range | Verdict | Reason |",
        "|---|---:|---:|---|---|",
    ]
    for row in metrics:
        lines.append(
            "| "
            f"{row['label']} | "
            f"`{row['fail_min']}..{row['fail_max']}` | "
            f"`{row['pass_min']}..{row['pass_max']}` | "
            f"`{row['verdict']}` | "
            f"{row['reason']} |"
        )
    lines.extend([
        "",
        "## Interpretation",
        "",
    ])
    if verdict == "final-color-oracle-required":
        lines.extend([
            "The tested geometry/depth/material deltas do not separate the semantic",
            "failure from exact-pass primitive-owner changes. Promotion needs a",
            "final-color/final-writer oracle or a non-reorder path.",
        ])
    elif verdict == "candidate-runtime-selector-found":
        lines.append("At least one non-color metric separates the current fail rows; validate it on a wider replay set before promotion.")
    else:
        lines.append("No simple selector separates the current fail rows.")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--csv-output", type=Path)
    args = parser.parse_args()

    rows = load_rows(args.input)
    metrics = summarize(rows)
    write_markdown(args.output, args.input, rows, metrics)
    if args.csv_output is not None:
        write_csv(args.csv_output, metrics)
    print(args.output)
    if args.csv_output is not None:
        print(args.csv_output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
