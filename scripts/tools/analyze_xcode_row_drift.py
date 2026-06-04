#!/usr/bin/env python3
"""Analyze selected seq/enc row drift across Xcode/dxmt joined summaries.

Inputs are joined-summary CSVs produced by summarize_xcode_encoder_counters.py.
The tool compares one baseline CSV against one or more candidate CSVs and
reports selected row-level metric deltas. It is intended for GT1 proof work
where a target-row win can be hidden by unrelated hot-row shape drift.
"""

from __future__ import annotations

import argparse
import csv
import sys
from pathlib import Path
from typing import Any


DEFAULT_METRICS = (
    "gpu_ms",
    "vs_buffer_write_mib",
    "vs_invocations",
    "vs_buffer_bytes_per_vs_invocation",
    "vs_buffer_bytes_per_primitive",
    "vs_buffer_write_limiter_pct",
    "buffer_write_limiter_pct",
    "vertex_stage_time_pct",
    "tiled_vertex_buffer_mib",
    "tiled_primitive_block_mib",
    "dxmt_named_tiled_buffer_mib",
    "dxmt_hidden_backend_write_mib",
    "dxmt_hidden_backend_write_ratio",
    "dxmt_vs_buffer_write_to_tvb_proxy_ratio",
    "dxmt_cpu_writer_mib",
    "dxmt_draw_calls",
    "dxmt_vertex_count",
    "dxmt_triangle_estimate",
    "dxmt_expanded_indexed_draws",
    "dxmt_indexed_vertex_cache_miss_estimate_32",
    "dxmt_reordered_index_cache_hits",
    "dxmt_reordered_index_cache_rejected_hits",
    "dxmt_draw_geometry_signature_samples",
    "dxmt_draw_geometry_signature_unique",
    "dxmt_draw_geometry_signature_duplicates",
    "dxmt_draw_geometry_signature_consecutive_duplicates",
    "dxmt_draw_geometry_signature_duplicate_ratio",
    "dxmt_draw_geometry_signature_consecutive_duplicate_ratio",
    "dxmt_indexed_cache_opt_candidate_draws",
    "dxmt_indexed_cache_opt_candidate_bytes",
    "dxmt_indexed_cache_opt_candidate_original_miss32",
    "dxmt_indexed_cache_opt_candidate_miss32",
    "dxmt_shader_variant_changes",
    "dxmt_ib_handle_changes",
    "dxmt_stream_handle_changes",
    "dxmt_transient_vertex_expanded_main_bytes",
    "dxmt_transient_vertex_expanded_extra_bytes",
)

SHAPE_METRICS = (
    "dxmt_draw_calls",
    "dxmt_vertex_count",
    "dxmt_triangle_estimate",
)


def parse_labeled_path(value: str) -> tuple[str, Path]:
    if "=" not in value:
        raise argparse.ArgumentTypeError(
            f"invalid labeled path '{value}', expected LABEL=PATH"
        )
    label, raw_path = value.split("=", 1)
    label = label.strip()
    raw_path = raw_path.strip()
    if not label or not raw_path:
        raise argparse.ArgumentTypeError(
            f"invalid labeled path '{value}', expected LABEL=PATH"
        )
    return label, Path(raw_path)


def parse_row_key(value: str) -> tuple[str, str]:
    text = value.strip()
    if "/" in text:
        seq, enc = (part.strip() for part in text.split("/", 1))
        if seq and enc:
            return seq, enc
    parts: dict[str, str] = {}
    for item in text.split(","):
        if "=" not in item:
            continue
        key, raw_value = item.split("=", 1)
        parts[key.strip()] = raw_value.strip()
    if parts.get("seq") and parts.get("enc"):
        return parts["seq"], parts["enc"]
    raise argparse.ArgumentTypeError(
        f"invalid row key '{value}', expected SEQ/ENC or seq=N,enc=M"
    )


def key_label(key: tuple[str, str]) -> str:
    return f"{key[0]}/{key[1]}"


def as_float(value: Any) -> float:
    text = str(value or "").strip().replace(",", "")
    if text == "":
        return 0.0
    try:
        return float(text)
    except ValueError:
        return 0.0


def load_keyed_rows(path: Path) -> dict[tuple[str, str], dict[str, str]]:
    rows: dict[tuple[str, str], dict[str, str]] = {}
    with path.open(newline="", encoding="utf-8") as handle:
        for row in csv.DictReader(handle):
            seq = (row.get("seq") or "").strip()
            enc = (row.get("enc") or "").strip()
            if seq and enc:
                rows[(seq, enc)] = row
    return rows


def pct_delta(after: float, before: float) -> float | None:
    if before == 0.0:
        return None
    return (after - before) / abs(before)


def fmt(value: float | None) -> str:
    if value is None:
        return "n/a"
    if abs(value) >= 10_000 or value.is_integer():
        return f"{value:,.0f}"
    return f"{value:,.3f}"


def fmt_pct(value: float | None) -> str:
    if value is None:
        return "n/a"
    return f"{value * 100.0:+.2f}%"


def compare_rows(
    baseline: tuple[str, dict[tuple[str, str], dict[str, str]]],
    runs: list[tuple[str, dict[tuple[str, str], dict[str, str]]]],
    row_keys: list[tuple[str, str]],
    metrics: list[str],
    shape_delta_ratio: float,
) -> list[dict[str, Any]]:
    baseline_label, baseline_rows = baseline
    out: list[dict[str, Any]] = []
    for row_key in row_keys:
        before = baseline_rows.get(row_key)
        for run_label, run_rows in runs:
            after = run_rows.get(row_key)
            shape_max_ratio = 0.0
            if before is not None and after is not None:
                for metric in SHAPE_METRICS:
                    ratio = pct_delta(
                        as_float(after.get(metric)),
                        as_float(before.get(metric)),
                    )
                    if ratio is not None:
                        shape_max_ratio = max(shape_max_ratio, abs(ratio))
            status = "ok"
            if before is None:
                status = "missing-baseline-row"
            elif after is None:
                status = "missing-run-row"
            elif shape_max_ratio > shape_delta_ratio:
                status = "shape-drift"
            elif (
                as_float(after.get("vs_buffer_write_mib"))
                > as_float(before.get("vs_buffer_write_mib"))
            ):
                status = "vs-write-regression"
            out.append({
                "baseline_label": baseline_label,
                "run_label": run_label,
                "row_key": row_key,
                "before": before,
                "after": after,
                "status": status,
                "shape_max_ratio": shape_max_ratio,
                "metrics": metrics,
            })
    return out


def metric_values(item: dict[str, Any], metric: str) -> tuple[float, float, float, float | None]:
    before = item["before"]
    after = item["after"]
    if before is None or after is None:
        return 0.0, 0.0, 0.0, None
    before_value = as_float(before.get(metric))
    after_value = as_float(after.get(metric))
    return before_value, after_value, after_value - before_value, pct_delta(after_value, before_value)


def decision_for_comparison(item: dict[str, Any]) -> tuple[str, str]:
    status = item["status"]
    if status == "missing-baseline-row":
        return "reject-missing-baseline", "fix the baseline row selector before interpreting counters"
    if status == "missing-run-row":
        return "reject-missing-run", "fix the candidate row selector before interpreting counters"
    if status == "shape-drift":
        return "reject-shape-drift", "rerun with a stable row/draw/vertex/triangle shape before opening Xcode"

    _, _, vs_write_delta, vs_write_pct = metric_values(item, "vs_buffer_write_mib")
    _, _, vs_inv_delta, vs_inv_pct = metric_values(item, "vs_invocations")
    _, _, gpu_delta, gpu_pct = metric_values(item, "gpu_ms")
    _, _, hidden_delta, hidden_pct = metric_values(item, "dxmt_hidden_backend_write_mib")
    _, _, expanded_delta, _ = metric_values(item, "dxmt_expanded_indexed_draws")
    _, _, reordered_hit_delta, _ = metric_values(item, "dxmt_reordered_index_cache_hits")

    if expanded_delta > 0:
        return "reject-flat-expansion", "preserve indexed submission; flat expansion raises VS invocations/backend write"
    if vs_write_pct is not None and vs_write_pct > 0.005:
        return "reject-vs-write-regression", "do not spend more Xcode time unless the row-local VS-write regression is explained"
    if (
        reordered_hit_delta > 0 and
        vs_write_pct is not None and vs_write_pct <= -0.05 and
        vs_inv_pct is not None and vs_inv_pct <= -0.05
    ):
        return "reorder-performance-mechanism", "requires exact/tolerance semantic image proof before production promotion"
    if (
        reordered_hit_delta <= 0 and
        vs_write_pct is not None and vs_write_pct <= -0.02 and
        hidden_pct is not None and hidden_pct <= -0.02 and
        (gpu_pct is None or gpu_pct <= 0.0)
    ):
        return "non-reorder-xcode-candidate", "promote only if the same-row semantic output is unchanged or the mutation is diagnostic-only"
    if (
        vs_write_pct is not None and vs_write_pct <= -0.01 and
        (gpu_pct is not None and gpu_pct > 0.0)
    ):
        return "secondary-counter-only", "keep as classifier; do not prioritize without target-row GPU improvement"
    if (
        vs_write_pct is not None and abs(vs_write_pct) < 0.005 and
        vs_inv_pct is not None and abs(vs_inv_pct) < 0.005 and
        abs(vs_write_delta) < 8.0 and
        abs(vs_inv_delta) < 4096.0
    ):
        return "no-material-movement", "skip new gputrace; row-local VS invocation/write did not move"
    if gpu_delta < 0 and hidden_delta < 0:
        return "weak-positive", "requires a focused proof gate before treating as a fix"
    return "inconclusive", "inspect per-row counters and add a narrower preflight gate"


def write_summary_csv(path: Path, comparisons: list[dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fields = [
        "row",
        "run",
        "status",
        "decision",
        "next_action",
        "shape_max_delta_pct",
        "metric",
        "baseline",
        "run_value",
        "delta",
        "delta_pct",
    ]
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        for item in comparisons:
            row = item["before"]
            after = item["after"]
            decision, next_action = decision_for_comparison(item)
            for metric in item["metrics"]:
                if row is None or after is None:
                    before_value = after_value = delta_value = None
                    delta_pct = None
                else:
                    before_value = as_float(row.get(metric))
                    after_value = as_float(after.get(metric))
                    delta_value = after_value - before_value
                    delta_pct = pct_delta(after_value, before_value)
                writer.writerow({
                    "row": key_label(item["row_key"]),
                    "run": item["run_label"],
                    "status": item["status"],
                    "decision": decision,
                    "next_action": next_action,
                    "shape_max_delta_pct": f"{item['shape_max_ratio'] * 100.0:.6g}",
                    "metric": metric,
                    "baseline": "" if before_value is None else repr(before_value),
                    "run_value": "" if after_value is None else repr(after_value),
                    "delta": "" if delta_value is None else repr(delta_value),
                    "delta_pct": "" if delta_pct is None else repr(delta_pct),
                })


def write_report(path: Path, comparisons: list[dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    lines: list[str] = []
    lines.append("# Xcode/dxmt Row Drift Report")
    lines.append("")
    lines.append("| Row | Run | Status | Max shape delta |")
    lines.append("|---|---|---|---:|")
    for item in comparisons:
        lines.append(
            f"| `{key_label(item['row_key'])}` | `{item['run_label']}` | "
            f"`{item['status']}` | `{item['shape_max_ratio'] * 100.0:.2f}%` |"
        )
    lines.append("")

    lines.append("## Decision Summary")
    lines.append("")
    lines.append("| Row | Run | Decision | Key deltas | Next action |")
    lines.append("|---|---|---|---|---|")
    for item in comparisons:
        decision, next_action = decision_for_comparison(item)
        _, _, gpu_delta, gpu_pct = metric_values(item, "gpu_ms")
        _, _, vs_write_delta, vs_write_pct = metric_values(item, "vs_buffer_write_mib")
        _, _, vs_inv_delta, vs_inv_pct = metric_values(item, "vs_invocations")
        key_deltas = (
            f"GPU `{fmt(gpu_delta)}` / `{fmt_pct(gpu_pct)}`, "
            f"VS write `{fmt(vs_write_delta)}` / `{fmt_pct(vs_write_pct)}`, "
            f"VS inv `{fmt(vs_inv_delta)}` / `{fmt_pct(vs_inv_pct)}`"
        )
        lines.append(
            f"| `{key_label(item['row_key'])}` | `{item['run_label']}` | "
            f"`{decision}` | {key_deltas} | {next_action} |"
        )
    lines.append("")

    for item in comparisons:
        lines.append(f"## `{key_label(item['row_key'])}` vs `{item['run_label']}`")
        lines.append("")
        row = item["before"]
        after = item["after"]
        if row is None or after is None:
            lines.append(f"status: `{item['status']}`")
            lines.append("")
            continue
        lines.append("| Metric | Baseline | Run | Delta | Delta % |")
        lines.append("|---|---:|---:|---:|---:|")
        for metric in item["metrics"]:
            before_value = as_float(row.get(metric))
            after_value = as_float(after.get(metric))
            delta_value = after_value - before_value
            lines.append(
                f"| `{metric}` | `{fmt(before_value)}` | `{fmt(after_value)}` | "
                f"`{fmt(delta_value)}` | `{fmt_pct(pct_delta(after_value, before_value))}` |"
            )
        lines.append("")
    path.write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", required=True, type=parse_labeled_path)
    parser.add_argument("--run", action="append", required=True, type=parse_labeled_path)
    parser.add_argument("--row-key", action="append", required=True, type=parse_row_key)
    parser.add_argument("--metric", action="append", default=None)
    parser.add_argument("--max-shape-delta-ratio", type=float, default=0.05)
    parser.add_argument("--require-shape-stable", action="store_true")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--summary-output", type=Path)
    args = parser.parse_args()

    baseline_label, baseline_path = args.baseline
    if not baseline_path.is_file():
        parser.error(f"missing baseline CSV: {baseline_path}")
    runs: list[tuple[str, dict[tuple[str, str], dict[str, str]]]] = []
    for run_label, run_path in args.run:
        if not run_path.is_file():
            parser.error(f"missing run CSV: {run_path}")
        runs.append((run_label, load_keyed_rows(run_path)))

    metrics = list(args.metric) if args.metric else list(DEFAULT_METRICS)
    comparisons = compare_rows(
        (baseline_label, load_keyed_rows(baseline_path)),
        runs,
        args.row_key,
        metrics,
        args.max_shape_delta_ratio,
    )
    write_report(args.output, comparisons)
    if args.summary_output:
        write_summary_csv(args.summary_output, comparisons)

    if args.require_shape_stable:
        failures = [item for item in comparisons if item["status"] == "shape-drift"]
        if failures:
            for item in failures:
                print(
                    "requirement failed: "
                    f"{key_label(item['row_key'])} in {item['run_label']} "
                    f"shape drift {item['shape_max_ratio'] * 100.0:.2f}% "
                    f"exceeds {args.max_shape_delta_ratio * 100.0:.2f}%",
                    file=sys.stderr,
                )
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
