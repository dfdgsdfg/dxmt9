#!/usr/bin/env python3
"""Gate a fragmentless depth-only route before Xcode/gputrace promotion."""

from __future__ import annotations

import argparse
import csv
import re
from pathlib import Path
from typing import Any


CSV_FIELDS = [
    "target_row",
    "overall_verdict",
    "gputrace_readiness",
    "route_status",
    "equality_status",
    "counter_status",
    "route_draw_coverage_pct",
    "route_primitive_coverage_pct",
    "route_vertex_coverage_pct",
    "target_indexed_draws",
    "target_primitives",
    "target_vertices",
    "probe_draws",
    "probe_primitives",
    "probe_vertices",
    "vsout_layout_last",
    "present_encoded",
    "draw_skipped_no_pipeline",
    "gpu_command_buffer_errors",
    "fragmentless_accept_logs",
    "fragmentless_reject_logs",
    "no_pipeline_logs",
    "equality_max_changed_pct",
    "equality_max_delta",
    "baseline_vs_buffer_write_mib",
    "treatment_vs_buffer_write_mib",
    "vs_buffer_write_delta_pct",
    "baseline_vs_bytes_per_invocation",
    "treatment_vs_bytes_per_invocation",
    "vs_bytes_per_invocation_delta_pct",
    "baseline_gpu_ms",
    "treatment_gpu_ms",
    "gpu_ms_delta_pct",
    "reason",
    "next_action",
]


COUNTER_RE = re.compile(r"\|\s*`(?P<name>[^`]+)`\s*\|\s*`?(?P<value>[^`|]+)`?\s*\|")


def as_int(value: Any) -> int:
    text = str(value or "").strip().replace(",", "")
    if not text:
        return 0
    try:
        return int(text, 0)
    except ValueError:
        try:
            return int(float(text))
        except ValueError:
            return 0


def as_float(value: Any) -> float:
    text = str(value or "").strip().replace(",", "")
    if text.endswith("%"):
        text = text[:-1]
    if not text:
        return 0.0
    try:
        return float(text)
    except ValueError:
        return 0.0


def fmt_float(value: float) -> str:
    return f"{value:.6f}"


def pct(numer: int, denom: int) -> float:
    return float(numer) / float(denom) * 100.0 if denom else 0.0


def delta_pct(after: float, before: float) -> float:
    return (after - before) / before * 100.0 if before else 0.0


def row_key(row: dict[str, str]) -> str:
    return f"{row.get('seq', '')}/{row.get('encoder') or row.get('enc', '')}"


def load_csv(path: Path) -> list[dict[str, str]]:
    if not path.exists():
        raise SystemExit(f"missing CSV: {path}")
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def load_summary_counters(path: Path | None) -> dict[str, int]:
    if path is None:
        return {}
    if not path.exists():
        raise SystemExit(f"missing perf summary: {path}")
    counters: dict[str, int] = {}
    for line in path.read_text(encoding="utf-8", errors="ignore").splitlines():
        match = COUNTER_RE.match(line)
        if match:
            counters[match.group("name")] = as_int(match.group("value"))
    return counters


def load_log_counts(paths: list[Path]) -> dict[str, int]:
    counts = {
        "fragmentless_accept_logs": 0,
        "fragmentless_reject_logs": 0,
        "no_pipeline_logs": 0,
    }
    for path in paths:
        if not path.exists():
            raise SystemExit(f"missing log: {path}")
        text = path.read_text(encoding="utf-8", errors="ignore")
        counts["fragmentless_accept_logs"] += text.count("fragmentless depth-only probe accepted")
        counts["fragmentless_reject_logs"] += text.count("fragmentless depth-only probe rejected")
        counts["no_pipeline_logs"] += text.count("draw skipped: no render pipeline")
        counts["no_pipeline_logs"] += text.count("skipped reason=no-pipeline")
    return counts


def target_encoder_row(path: Path, target_row: str) -> dict[str, str] | None:
    for row in load_csv(path):
        if row_key(row) == target_row:
            return row
    return None


def summarize_equality(path: Path | None, max_changed_pct: float, max_delta: int) -> tuple[str, str, float, int]:
    if path is None:
        return (
            "missing-equality",
            "same-input color/depth equality CSV was not provided",
            0.0,
            0,
        )
    rows = load_csv(path)
    if not rows:
        return "missing-equality", "equality CSV has no rows", 0.0, 0
    max_changed = max(as_float(row.get("changed_pct")) for row in rows)
    observed_max_delta = max(as_int(row.get("max_delta")) for row in rows)
    if max_changed > max_changed_pct or observed_max_delta > max_delta:
        return (
            "failed-equality",
            (
                f"image/depth equality failed: max changed {max_changed:.6f}% "
                f"or max delta {observed_max_delta} exceeds gate"
            ),
            max_changed,
            observed_max_delta,
        )
    return "passed-equality", "same-input equality gate passed", max_changed, observed_max_delta


def find_xcode_row(path: Path, target_row: str) -> dict[str, str] | None:
    for row in load_csv(path):
        key = f"{row.get('seq', '')}/{row.get('enc', '')}"
        if key == target_row:
            return row
    return None


def summarize_counters(
    baseline_path: Path | None,
    treatment_path: Path | None,
    target_row: str,
    min_vs_bytes_drop_pct: float,
) -> tuple[str, str, dict[str, float]]:
    empty = {
        "baseline_vs_buffer_write_mib": 0.0,
        "treatment_vs_buffer_write_mib": 0.0,
        "vs_buffer_write_delta_pct": 0.0,
        "baseline_vs_bytes_per_invocation": 0.0,
        "treatment_vs_bytes_per_invocation": 0.0,
        "vs_bytes_per_invocation_delta_pct": 0.0,
        "baseline_gpu_ms": 0.0,
        "treatment_gpu_ms": 0.0,
        "gpu_ms_delta_pct": 0.0,
    }
    if baseline_path is None or treatment_path is None:
        return "missing-counters", "Xcode baseline/treatment counter summaries were not provided", empty

    baseline = find_xcode_row(baseline_path, target_row)
    treatment = find_xcode_row(treatment_path, target_row)
    if baseline is None or treatment is None:
        return "missing-counters", f"target row {target_row} is missing from one Xcode summary", empty

    base_vs = as_float(baseline.get("vs_buffer_write_mib"))
    treat_vs = as_float(treatment.get("vs_buffer_write_mib"))
    base_bpi = as_float(baseline.get("vs_buffer_bytes_per_vs_invocation"))
    treat_bpi = as_float(treatment.get("vs_buffer_bytes_per_vs_invocation"))
    base_gpu = as_float(baseline.get("gpu_ms"))
    treat_gpu = as_float(treatment.get("gpu_ms"))
    values = {
        "baseline_vs_buffer_write_mib": base_vs,
        "treatment_vs_buffer_write_mib": treat_vs,
        "vs_buffer_write_delta_pct": delta_pct(treat_vs, base_vs),
        "baseline_vs_bytes_per_invocation": base_bpi,
        "treatment_vs_bytes_per_invocation": treat_bpi,
        "vs_bytes_per_invocation_delta_pct": delta_pct(treat_bpi, base_bpi),
        "baseline_gpu_ms": base_gpu,
        "treatment_gpu_ms": treat_gpu,
        "gpu_ms_delta_pct": delta_pct(treat_gpu, base_gpu),
    }
    if values["vs_bytes_per_invocation_delta_pct"] <= -min_vs_bytes_drop_pct:
        return "passed-counters", "Xcode VS bytes/invocation moved enough for promotion", values
    return (
        "failed-counters",
        (
            "Xcode VS bytes/invocation did not move enough: "
            f"{values['vs_bytes_per_invocation_delta_pct']:.6f}%"
        ),
        values,
    )


def classify(
    *,
    route_status: str,
    equality_status: str,
    counter_status: str,
    route_reason: str,
    equality_reason: str,
    counter_reason: str,
) -> tuple[str, str, str, str]:
    if route_status != "passed-route":
        return (
            "blocked-route-not-clean",
            "blocked-route",
            route_reason,
            "fix route coverage/reject/error state before equality or Xcode counters",
        )
    if equality_status == "missing-equality":
        return (
            "route-reachable-needs-equality",
            "blocked-needs-equality",
            equality_reason,
            "capture same-input baseline/treatment color/depth output and run image/depth equality",
        )
    if equality_status != "passed-equality":
        return (
            "blocked-equality-fail",
            "blocked-equality",
            equality_reason,
            "debug depth/color semantic differences before any Xcode counter spend",
        )
    if counter_status == "missing-counters":
        return (
            "route-equal-ready-for-xcode",
            "ready-for-xcode-counters",
            counter_reason,
            "capture/export Xcode encoder counters for baseline and treatment",
        )
    if counter_status != "passed-counters":
        return (
            "rejected-counter-no-vswrite-move",
            "xcode-complete-rejected",
            counter_reason,
            "reject this backend denominator route or find a different counter-moving shape",
        )
    return (
        "promote-candidate",
        "xcode-complete-promote",
        "route, equality, and counter gates passed",
        "consider guarded production route only after visual/regression coverage",
    )


def build_gate_row(args: argparse.Namespace) -> dict[str, str]:
    target = target_encoder_row(args.encoder_csv, args.target_row)
    if target is None:
        base = {field: "" for field in CSV_FIELDS}
        base.update({
            "target_row": args.target_row,
            "overall_verdict": "blocked-missing-route-coverage",
            "gputrace_readiness": "blocked-route",
            "route_status": "missing-route",
            "equality_status": "not-run",
            "counter_status": "not-run",
            "reason": f"target row {args.target_row} missing from encoder CSV",
            "next_action": "rerun the row-scoped route smoke with encoder breakdown",
        })
        return base

    counters = load_summary_counters(args.perf_summary)
    log_counts = load_log_counts(args.log)

    target_draws = as_int(target.get("indexed_draws") or target.get("draw_calls"))
    target_prims = as_int(target.get("primitive_count"))
    target_vertices = as_int(target.get("vertex_count"))
    probe_draws = as_int(target.get("probe_fragmentless_depth_only_draws"))
    probe_prims = as_int(target.get("probe_fragmentless_depth_only_primitives"))
    probe_vertices = as_int(target.get("probe_fragmentless_depth_only_vertices"))
    draw_cov = pct(probe_draws, target_draws)
    prim_cov = pct(probe_prims, target_prims)
    vertex_cov = pct(probe_vertices, target_vertices)

    draw_skips = counters.get("draw_skipped_no_pipeline", 0)
    gpu_errors = counters.get("gpu_command_buffer_errors", 0)
    reject_logs = log_counts["fragmentless_reject_logs"]
    no_pipeline_logs = log_counts["no_pipeline_logs"]
    if (
        draw_cov >= args.min_route_coverage_pct
        and prim_cov >= args.min_route_coverage_pct
        and vertex_cov >= args.min_route_coverage_pct
        and draw_skips == 0
        and gpu_errors == 0
        and reject_logs == 0
        and no_pipeline_logs == 0
    ):
        route_status = "passed-route"
        route_reason = "fragmentless depth-only route covers the target row without reject/error evidence"
    else:
        route_status = "failed-route"
        route_reason = (
            f"coverage draw/prim/vertex={draw_cov:.3f}/{prim_cov:.3f}/{vertex_cov:.3f}% "
            f"draw_skips={draw_skips} gpu_errors={gpu_errors} reject_logs={reject_logs} "
            f"no_pipeline_logs={no_pipeline_logs}"
        )

    equality_status, equality_reason, equality_changed, equality_delta = summarize_equality(
        args.equality_csv,
        args.max_equality_changed_pct,
        args.max_equality_delta,
    )
    counter_status, counter_reason, counter_values = summarize_counters(
        args.xcode_baseline_csv,
        args.xcode_treatment_csv,
        args.target_row,
        args.min_vs_bytes_drop_pct,
    )
    verdict, readiness, reason, next_action = classify(
        route_status=route_status,
        equality_status=equality_status,
        counter_status=counter_status,
        route_reason=route_reason,
        equality_reason=equality_reason,
        counter_reason=counter_reason,
    )

    row = {
        "target_row": args.target_row,
        "overall_verdict": verdict,
        "gputrace_readiness": readiness,
        "route_status": route_status,
        "equality_status": equality_status,
        "counter_status": counter_status,
        "route_draw_coverage_pct": fmt_float(draw_cov),
        "route_primitive_coverage_pct": fmt_float(prim_cov),
        "route_vertex_coverage_pct": fmt_float(vertex_cov),
        "target_indexed_draws": str(target_draws),
        "target_primitives": str(target_prims),
        "target_vertices": str(target_vertices),
        "probe_draws": str(probe_draws),
        "probe_primitives": str(probe_prims),
        "probe_vertices": str(probe_vertices),
        "vsout_layout_last": target.get("vsout_layout_last", ""),
        "present_encoded": str(counters.get("present_encoded", 0)),
        "draw_skipped_no_pipeline": str(draw_skips),
        "gpu_command_buffer_errors": str(gpu_errors),
        "fragmentless_accept_logs": str(log_counts["fragmentless_accept_logs"]),
        "fragmentless_reject_logs": str(reject_logs),
        "no_pipeline_logs": str(no_pipeline_logs),
        "equality_max_changed_pct": fmt_float(equality_changed),
        "equality_max_delta": str(equality_delta),
        "reason": reason,
        "next_action": next_action,
    }
    for key, value in counter_values.items():
        row[key] = fmt_float(value)
    return {field: row.get(field, "") for field in CSV_FIELDS}


def write_csv(path: Path, row: dict[str, str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=CSV_FIELDS)
        writer.writeheader()
        writer.writerow(row)


def write_markdown(path: Path, row: dict[str, str], args: argparse.Namespace) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    lines = [
        "# Fragmentless Depth-Only Route Gate",
        "",
        f"- Target row: `{row['target_row']}`",
        f"- Encoder CSV: `{args.encoder_csv}`",
        f"- Overall verdict: `{row['overall_verdict']}`",
        f"- Xcode/gputrace readiness: `{row['gputrace_readiness']}`",
        f"- Reason: {row['reason']}",
        f"- Next action: {row['next_action']}",
        "",
        "## Route",
        "",
        "| Metric | Value |",
        "|---|---:|",
        f"| Route status | `{row['route_status']}` |",
        f"| Draw coverage | `{row['route_draw_coverage_pct']}%` |",
        f"| Primitive coverage | `{row['route_primitive_coverage_pct']}%` |",
        f"| Vertex coverage | `{row['route_vertex_coverage_pct']}%` |",
        f"| Target draws/prims/verts | `{row['target_indexed_draws']}` / `{row['target_primitives']}` / `{row['target_vertices']}` |",
        f"| Probe draws/prims/verts | `{row['probe_draws']}` / `{row['probe_primitives']}` / `{row['probe_vertices']}` |",
        f"| VSOut layout | `{row['vsout_layout_last']}` |",
        f"| present_encoded | `{row['present_encoded']}` |",
        f"| draw_skipped_no_pipeline | `{row['draw_skipped_no_pipeline']}` |",
        f"| gpu_command_buffer_errors | `{row['gpu_command_buffer_errors']}` |",
        f"| accept/reject/no-pipeline logs | `{row['fragmentless_accept_logs']}` / `{row['fragmentless_reject_logs']}` / `{row['no_pipeline_logs']}` |",
        "",
        "## Equality And Counters",
        "",
        "| Gate | Status | Evidence |",
        "|---|---|---|",
        f"| same-input equality | `{row['equality_status']}` | max changed `{row['equality_max_changed_pct']}%`, max delta `{row['equality_max_delta']}` |",
        f"| Xcode counters | `{row['counter_status']}` | VS B/inv delta `{row['vs_bytes_per_invocation_delta_pct']}%`, VS write delta `{row['vs_buffer_write_delta_pct']}%`, GPU delta `{row['gpu_ms_delta_pct']}%` |",
        "",
        "```mermaid",
        "flowchart TD",
        "  Route[route coverage/reject/error gate] --> Eq{same-input depth/color equality?}",
        "  Eq -- missing --> BlockEq[block Xcode spend]",
        "  Eq -- fail --> Debug[debug depth/color semantics]",
        "  Eq -- pass --> Xcode[Xcode encoder counter export]",
        "  Xcode --> Counter{VS bytes/inv moves?}",
        "  Counter -- yes --> Promote[promotion candidate]",
        "  Counter -- no --> Reject[reject backend denominator route]",
        "```",
        "",
    ]
    path.write_text("\n".join(lines), encoding="utf-8")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--encoder-csv", type=Path, required=True)
    parser.add_argument("--target-row", default="60/0")
    parser.add_argument("--perf-summary", type=Path)
    parser.add_argument("--log", type=Path, action="append", default=[])
    parser.add_argument("--equality-csv", type=Path)
    parser.add_argument("--xcode-baseline-csv", type=Path)
    parser.add_argument("--xcode-treatment-csv", type=Path)
    parser.add_argument("--min-route-coverage-pct", type=float, default=99.0)
    parser.add_argument("--max-equality-changed-pct", type=float, default=0.0)
    parser.add_argument("--max-equality-delta", type=int, default=0)
    parser.add_argument("--min-vs-bytes-drop-pct", type=float, default=5.0)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--csv-output", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    row = build_gate_row(args)
    write_csv(args.csv_output, row)
    write_markdown(args.output, row, args)
    print(args.output)
    print(args.csv_output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
