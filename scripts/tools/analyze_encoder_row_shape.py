#!/usr/bin/env python3
"""Compare dxmt encoder-row shape between no-gputrace probe runs.

This is a cheap preflight before spending an Xcode gputrace export. It compares
`3dmark05-perf-encoders.csv` rows by `seq/encoder` and rejects row-number drift
when draw, vertex, or triangle counts no longer match the reference capture.
"""

from __future__ import annotations

import argparse
import csv
from pathlib import Path
from typing import Any


SHAPE_FIELDS = (
    "draw_calls",
    "vertex_count",
    "triangle_estimate",
)

CONTEXT_FIELDS = (
    "indexed_draws",
    "expanded_indexed_draws",
    "primitive_count",
    "alpha_blend_enabled_draws",
    "scissor_enabled_draws",
    "depth_enabled_draws",
    "depth_write_draws",
    "indexed_triangle_depth_read_draws",
    "indexed_triangle_alpha_blend_draws",
    "indexed_triangle_textured_draws",
    "indexed_cache_opt_candidate_draws",
    "indexed_cache_opt_candidate_miss_delta_32",
    "reordered_index_cache_hits",
    "reordered_index_cache_created",
)


def parse_row_key(value: str) -> tuple[str, str]:
    text = value.strip()
    if "/" not in text:
        raise argparse.ArgumentTypeError(
            f"invalid row key '{value}', expected SEQ/ENC")
    seq, enc = (part.strip() for part in text.split("/", 1))
    if not seq or not enc:
        raise argparse.ArgumentTypeError(
            f"invalid row key '{value}', expected SEQ/ENC")
    return seq, enc


def row_label(row_key: tuple[str, str]) -> str:
    return f"{row_key[0]}/{row_key[1]}"


def as_float(value: Any) -> float:
    text = str(value or "").strip().replace(",", "")
    if not text:
        return 0.0
    try:
        return float(text)
    except ValueError:
        return 0.0


def pct_delta(after: float, before: float) -> float | None:
    if before == 0.0:
        return None
    return (after - before) / abs(before)


def fmt(value: float | None) -> str:
    if value is None:
        return "n/a"
    if value.is_integer():
        return f"{value:,.0f}"
    return f"{value:,.3f}"


def fmt_pct(value: float | None) -> str:
    if value is None:
        return "n/a"
    return f"{value * 100.0:+.2f}%"


def load_rows(path: Path) -> dict[tuple[str, str], dict[str, str]]:
    rows: dict[tuple[str, str], dict[str, str]] = {}
    with path.open(newline="", encoding="utf-8") as handle:
        for row in csv.DictReader(handle):
            seq = (row.get("seq") or "").strip()
            enc = (row.get("encoder") or row.get("enc") or "").strip()
            if seq and enc:
                rows[(seq, enc)] = row
    return rows


def compare_row(
    baseline_row: dict[str, str] | None,
    run_row: dict[str, str] | None,
    *,
    max_shape_delta_ratio: float,
) -> dict[str, Any]:
    if baseline_row is None:
        return {"status": "missing-baseline-row", "max_shape_ratio": 0.0}
    if run_row is None:
        return {"status": "missing-run-row", "max_shape_ratio": 0.0}
    max_ratio = 0.0
    for field in SHAPE_FIELDS:
        ratio = pct_delta(as_float(run_row.get(field)), as_float(baseline_row.get(field)))
        if ratio is not None:
            max_ratio = max(max_ratio, abs(ratio))
    status = "ok" if max_ratio <= max_shape_delta_ratio else "shape-drift"
    return {"status": status, "max_shape_ratio": max_ratio}


def write_csv(
    path: Path,
    row_key: tuple[str, str],
    baseline_row: dict[str, str] | None,
    run_row: dict[str, str] | None,
    comparison: dict[str, Any],
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fields = [
        "row",
        "status",
        "metric",
        "baseline",
        "run",
        "delta",
        "delta_pct",
        "max_shape_delta_pct",
    ]
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        for field in SHAPE_FIELDS + CONTEXT_FIELDS:
            before = as_float(baseline_row.get(field)) if baseline_row else None
            after = as_float(run_row.get(field)) if run_row else None
            delta = None if before is None or after is None else after - before
            writer.writerow({
                "row": row_label(row_key),
                "status": comparison["status"],
                "metric": field,
                "baseline": "" if before is None else repr(before),
                "run": "" if after is None else repr(after),
                "delta": "" if delta is None else repr(delta),
                "delta_pct": "" if before is None or after is None else (
                    "" if pct_delta(after, before) is None else repr(pct_delta(after, before))
                ),
                "max_shape_delta_pct": f"{comparison['max_shape_ratio'] * 100.0:.6g}",
            })


def markdown_report(
    baseline_path: Path,
    run_path: Path,
    row_key: tuple[str, str],
    baseline_row: dict[str, str] | None,
    run_row: dict[str, str] | None,
    comparison: dict[str, Any],
    max_shape_delta_ratio: float,
) -> str:
    lines = [
        "# Encoder Row Shape Drift",
        "",
        f"- Baseline: `{baseline_path}`",
        f"- Run: `{run_path}`",
        f"- Row: `{row_label(row_key)}`",
        f"- Max allowed shape delta: `{max_shape_delta_ratio * 100.0:.2f}%`",
        f"- Status: `{comparison['status']}`",
        f"- Max shape delta: `{comparison['max_shape_ratio'] * 100.0:.2f}%`",
        "",
        "| Metric | Baseline | Run | Delta | Delta % |",
        "|---|---:|---:|---:|---:|",
    ]
    for field in SHAPE_FIELDS + CONTEXT_FIELDS:
        before = as_float(baseline_row.get(field)) if baseline_row else None
        after = as_float(run_row.get(field)) if run_row else None
        delta = None if before is None or after is None else after - before
        delta_pct = None if before is None or after is None else pct_delta(after, before)
        lines.append(
            f"| `{field}` | `{fmt(before)}` | `{fmt(after)}` | "
            f"`{fmt(delta)}` | `{fmt_pct(delta_pct)}` |")
    lines.append("")
    return "\n".join(lines)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline-encoders", type=Path, required=True)
    parser.add_argument("--run-encoders", type=Path, required=True)
    parser.add_argument("--row-key", type=parse_row_key, required=True)
    parser.add_argument("--max-shape-delta-ratio", type=float, default=0.05)
    parser.add_argument("--require-stable-shape", action="store_true")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--csv-output", type=Path)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    baseline_rows = load_rows(args.baseline_encoders)
    run_rows = load_rows(args.run_encoders)
    baseline_row = baseline_rows.get(args.row_key)
    run_row = run_rows.get(args.row_key)
    comparison = compare_row(
        baseline_row,
        run_row,
        max_shape_delta_ratio=args.max_shape_delta_ratio,
    )
    if args.csv_output:
        write_csv(args.csv_output, args.row_key, baseline_row, run_row, comparison)
    report = markdown_report(
        args.baseline_encoders,
        args.run_encoders,
        args.row_key,
        baseline_row,
        run_row,
        comparison,
        args.max_shape_delta_ratio,
    )
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(report, encoding="utf-8")
    else:
        print(report)
    if args.require_stable_shape and comparison["status"] != "ok":
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
