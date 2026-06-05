#!/usr/bin/env python3
"""Compare two 3DMark05 dxmt9 perf counter snapshots.

Inputs can be experiment output directories or direct result.json paths. Output
directories may also be interrupted partial-log runs if `dxmt9.log` contains a
final `[dxmt9-perf]` line. This complements `compare_xcode_dxmt_bottlenecks.py`:
Xcode encoder counters prove GPU-frame effects, while this report verifies the
run-level mechanisms that a candidate change intended to move, such as
render-pass store actions, same-key preservation bytes, draw-run formation, and
queue waits.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

from summarize_3dmark05_perf import RUN_COUNTERS, load_result


EXTRA_COUNTERS = (
    "render_split_rt_change",
    "render_split_clear",
    "render_split_present",
    "render_split_hazard",
    "render_pass_store_action_dontcare",
    "render_pass_store_action_depth_dontcare",
    "render_pass_color_proof_allow_dead_no_present",
    "render_pass_color_proof_block_dead_no_present_disabled",
    "render_pass_depth_proof_allow_dead_no_present",
    "completion_wait_ms",
    "queue_sequence_wait_ms",
    "map_buffer_wait_ms",
)

FOCUS_COUNTERS = (
    "present_encoded",
    "draw_calls",
    "render_pass_begin",
    "render_split_rt_change",
    "render_split_clear",
    "render_split_present",
    "render_pass_store_action_store",
    "render_pass_store_action_dontcare",
    "render_pass_store_action_depth_store",
    "render_pass_store_action_depth_dontcare",
    "render_pass_tile_preservation_bytes",
    "render_pass_same_key_reentry",
    "render_pass_same_key_reentry_preservation_bytes",
    "render_pass_same_key_reentry_color_preservation_bytes",
    "render_pass_same_key_reentry_depth_preservation_bytes",
    "render_pass_color_proof_allow_next_clear",
    "render_pass_color_proof_allow_dead_no_present",
    "render_pass_color_proof_block_draw_target",
    "render_pass_color_proof_block_texture_sample",
    "render_pass_color_proof_block_present",
    "render_pass_color_proof_block_dead_no_present_disabled",
    "render_pass_depth_proof_allow_next_clear",
    "render_pass_depth_proof_allow_dead_no_present",
    "render_pass_depth_proof_block_draw_depth",
    "commit_chunk_draw_run_submits",
    "commit_chunk_draw_run_records",
    "commit_chunk_draw_run_binding_override_records",
    "commit_chunk_draw_run_binding_override_bytes",
    "commit_chunk_draw_batch_const_upload_passthrough",
    "commit_chunk_draw_submission_batch_submits",
    "commit_chunk_draw_submission_batch_records",
    "commit_chunk_draw_submission_batch_max_records",
    "commit_chunk_draw_submission_batch_size_1",
    "commit_chunk_draw_submission_batch_size_2",
    "commit_chunk_draw_submission_batch_size_3_4",
    "commit_chunk_draw_submission_batch_size_5_8",
    "commit_chunk_draw_submission_batch_size_9_16",
    "commit_chunk_draw_submission_batch_size_17_32",
    "commit_chunk_draw_submission_batch_size_33_plus",
    "submit_draw_run_batch_groups",
    "submit_draw_run_batch_records",
    "submit_draw_run_batch_max_records",
    "commit_chunk_draw_run_break_type_const_upload",
    "commit_chunk_draw_run_break_type_const_upload_bytes",
    "commit_chunk_draw_run_break_type_const_upload_registers",
    "commit_chunk_draw_run_break_type_const_vs_f",
    "commit_chunk_draw_run_break_type_const_vs_f_bytes",
    "commit_chunk_draw_run_break_type_const_vs_f_registers",
    "commit_chunk_draw_run_break_type_const_vs_i",
    "commit_chunk_draw_run_break_type_const_vs_i_bytes",
    "commit_chunk_draw_run_break_type_const_vs_i_registers",
    "commit_chunk_draw_run_break_type_const_vs_b",
    "commit_chunk_draw_run_break_type_const_vs_b_bytes",
    "commit_chunk_draw_run_break_type_const_vs_b_registers",
    "commit_chunk_draw_run_break_type_const_ps_f",
    "commit_chunk_draw_run_break_type_const_ps_f_bytes",
    "commit_chunk_draw_run_break_type_const_ps_f_registers",
    "commit_chunk_draw_run_break_type_const_ps_i",
    "commit_chunk_draw_run_break_type_const_ps_i_bytes",
    "commit_chunk_draw_run_break_type_const_ps_i_registers",
    "commit_chunk_draw_run_break_type_const_ps_b",
    "commit_chunk_draw_run_break_type_const_ps_b_bytes",
    "commit_chunk_draw_run_break_type_const_ps_b_registers",
    "commit_chunk_draw_run_break_state_delta",
    "commit_chunk_draw_run_break_state_delta_stream_only",
    "commit_chunk_draw_run_break_state_delta_ib_only",
    "commit_chunk_draw_run_break_state_delta_texture_only",
    "commit_chunk_draw_run_break_state_delta_shader_only",
    "commit_chunk_draw_run_break_state_delta_fvf_vdecl_only",
    "commit_chunk_draw_run_break_state_delta_other_only",
    "commit_chunk_draw_run_break_state_delta_mixed",
    "commit_chunk_draw_run_break_state_delta_mixed_group2",
    "commit_chunk_draw_run_break_state_delta_mixed_group3",
    "commit_chunk_draw_run_break_state_delta_mixed_group4plus",
    "commit_chunk_draw_run_break_state_delta_stream_ib_only",
    "commit_chunk_draw_run_break_state_delta_mixed_with_stream",
    "commit_chunk_draw_run_break_state_delta_mixed_with_ib",
    "commit_chunk_draw_run_break_state_delta_mixed_with_texture",
    "commit_chunk_draw_run_break_state_delta_mixed_with_shader",
    "commit_chunk_draw_run_break_state_delta_mixed_with_fvf_vdecl",
    "commit_chunk_draw_run_break_state_delta_mixed_with_other",
    "commit_chunk_draw_run_break_state_delta_mixed_pair_stream_ib",
    "commit_chunk_draw_run_break_state_delta_mixed_pair_stream_texture",
    "commit_chunk_draw_run_break_state_delta_mixed_pair_stream_shader",
    "commit_chunk_draw_run_break_state_delta_mixed_pair_stream_fvf_vdecl",
    "commit_chunk_draw_run_break_state_delta_mixed_pair_ib_texture",
    "commit_chunk_draw_run_break_state_delta_mixed_pair_ib_shader",
    "commit_chunk_draw_run_break_state_delta_mixed_pair_ib_fvf_vdecl",
    "commit_chunk_draw_run_break_state_delta_mixed_pair_texture_shader",
    "commit_chunk_draw_run_break_state_delta_mixed_pair_texture_fvf_vdecl",
    "commit_chunk_draw_run_break_state_delta_mixed_pair_shader_fvf_vdecl",
    "commit_chunk_draw_delta_stream",
    "commit_chunk_draw_delta_ib",
    "argbuf_hybrid_bytes_per_encoder",
    "transient_upload_bytes",
    "encode_draw_cpu_ms",
    "submit_draw_cpu_ms",
    "gpu_command_buffer_time_ms",
    "completion_wait_ms",
    "map_buffer_wait_ms",
    "queue_sequence_wait_ms",
)


def result_path(path: Path) -> Path:
    if path.is_dir():
        return path / "result.json"
    return path


def counter_source_path(path: Path) -> Path:
    if not path.is_dir():
        return result_path(path)
    result = path / "result.json"
    if result.exists():
        return result
    log = path / "dxmt9.log"
    if log.exists():
        return log
    return result


def load_counters(path: Path) -> dict[str, Any]:
    if path.is_dir():
        data = load_result(path)
        counters = data.get("dxmt9_perf_counters")
        if not isinstance(counters, dict):
            raise SystemExit(f"missing dxmt9_perf_counters in {path}")
        return counters
    resolved = result_path(path)
    if not resolved.exists():
        raise SystemExit(f"missing result.json: {resolved}")
    data = json.loads(resolved.read_text(encoding="utf-8"))
    counters = data.get("dxmt9_perf_counters")
    if not isinstance(counters, dict):
        raise SystemExit(f"missing dxmt9_perf_counters in {resolved}")
    return counters


def number(value: Any) -> float | None:
    if isinstance(value, (int, float)):
        return float(value)
    try:
        text = str(value).strip()
        if text == "":
            return None
        return float(text)
    except (TypeError, ValueError):
        return None


def fmt_value(value: Any) -> str:
    numeric = number(value)
    if numeric is None:
        return "missing"
    if abs(numeric) >= 1000.0:
        return f"{numeric:,.3f}".rstrip("0").rstrip(".")
    if numeric.is_integer():
        return f"{int(numeric):,}"
    return f"{numeric:.3f}".rstrip("0").rstrip(".")


def delta(after: Any, before: Any) -> tuple[str, str]:
    a = number(after)
    b = number(before)
    if a is None or b is None:
        return "n/a", "n/a"
    diff = a - b
    pct = "n/a" if b == 0.0 else f"{diff / b * 100.0:+.2f}%"
    return fmt_value(diff), pct


def counter(counters: dict[str, Any], key: str) -> Any:
    return counters.get(key)


def ratio(counters: dict[str, Any], numerator: str, denominator: str) -> float | None:
    n = number(counter(counters, numerator))
    d = number(counter(counters, denominator))
    if n is None or d is None or d == 0.0:
        return None
    return n / d


def derived(counters: dict[str, Any]) -> dict[str, float | None]:
    present = number(counter(counters, "present_encoded")) or 0.0
    draws = number(counter(counters, "draw_calls")) or 0.0
    const_upload_breaks = number(counter(
        counters,
        "commit_chunk_draw_run_break_type_const_upload",
    )) or 0.0
    state_delta_breaks = number(counter(
        counters,
        "commit_chunk_draw_run_break_state_delta",
    )) or 0.0
    const_upload_passthrough = number(counter(
        counters,
        "commit_chunk_draw_batch_const_upload_passthrough",
    )) or 0.0
    const_upload_bytes = number(counter(
        counters,
        "commit_chunk_draw_run_break_type_const_upload_bytes",
    )) or 0.0
    const_upload_registers = number(counter(
        counters,
        "commit_chunk_draw_run_break_type_const_upload_registers",
    )) or 0.0
    const_subtypes = {
        "const_vs_f": number(counter(
            counters, "commit_chunk_draw_run_break_type_const_vs_f",
        )) or 0.0,
        "const_vs_i": number(counter(
            counters, "commit_chunk_draw_run_break_type_const_vs_i",
        )) or 0.0,
        "const_vs_b": number(counter(
            counters, "commit_chunk_draw_run_break_type_const_vs_b",
        )) or 0.0,
        "const_ps_f": number(counter(
            counters, "commit_chunk_draw_run_break_type_const_ps_f",
        )) or 0.0,
        "const_ps_i": number(counter(
            counters, "commit_chunk_draw_run_break_type_const_ps_i",
        )) or 0.0,
        "const_ps_b": number(counter(
            counters, "commit_chunk_draw_run_break_type_const_ps_b",
        )) or 0.0,
    }
    const_subtype_bytes = {
        "const_vs_f": number(counter(
            counters, "commit_chunk_draw_run_break_type_const_vs_f_bytes",
        )) or 0.0,
        "const_vs_i": number(counter(
            counters, "commit_chunk_draw_run_break_type_const_vs_i_bytes",
        )) or 0.0,
        "const_vs_b": number(counter(
            counters, "commit_chunk_draw_run_break_type_const_vs_b_bytes",
        )) or 0.0,
        "const_ps_f": number(counter(
            counters, "commit_chunk_draw_run_break_type_const_ps_f_bytes",
        )) or 0.0,
        "const_ps_i": number(counter(
            counters, "commit_chunk_draw_run_break_type_const_ps_i_bytes",
        )) or 0.0,
        "const_ps_b": number(counter(
            counters, "commit_chunk_draw_run_break_type_const_ps_b_bytes",
        )) or 0.0,
    }
    const_subtype_total = sum(const_subtypes.values())
    state_delta_subtypes = {
        "state_delta_stream_only": number(counter(
            counters, "commit_chunk_draw_run_break_state_delta_stream_only",
        )) or 0.0,
        "state_delta_ib_only": number(counter(
            counters, "commit_chunk_draw_run_break_state_delta_ib_only",
        )) or 0.0,
        "state_delta_texture_only": number(counter(
            counters, "commit_chunk_draw_run_break_state_delta_texture_only",
        )) or 0.0,
        "state_delta_shader_only": number(counter(
            counters, "commit_chunk_draw_run_break_state_delta_shader_only",
        )) or 0.0,
        "state_delta_fvf_vdecl_only": number(counter(
            counters, "commit_chunk_draw_run_break_state_delta_fvf_vdecl_only",
        )) or 0.0,
        "state_delta_other_only": number(counter(
            counters, "commit_chunk_draw_run_break_state_delta_other_only",
        )) or 0.0,
        "state_delta_mixed": number(counter(
            counters, "commit_chunk_draw_run_break_state_delta_mixed",
        )) or 0.0,
        "state_delta_mixed_group2": number(counter(
            counters, "commit_chunk_draw_run_break_state_delta_mixed_group2",
        )) or 0.0,
        "state_delta_mixed_group3": number(counter(
            counters, "commit_chunk_draw_run_break_state_delta_mixed_group3",
        )) or 0.0,
        "state_delta_mixed_group4plus": number(counter(
            counters, "commit_chunk_draw_run_break_state_delta_mixed_group4plus",
        )) or 0.0,
        "state_delta_stream_ib_only": number(counter(
            counters, "commit_chunk_draw_run_break_state_delta_stream_ib_only",
        )) or 0.0,
        "state_delta_mixed_pair_stream_ib": number(counter(
            counters, "commit_chunk_draw_run_break_state_delta_mixed_pair_stream_ib",
        )) or 0.0,
        "state_delta_mixed_pair_stream_texture": number(counter(
            counters, "commit_chunk_draw_run_break_state_delta_mixed_pair_stream_texture",
        )) or 0.0,
        "state_delta_mixed_pair_stream_shader": number(counter(
            counters, "commit_chunk_draw_run_break_state_delta_mixed_pair_stream_shader",
        )) or 0.0,
        "state_delta_mixed_pair_ib_texture": number(counter(
            counters, "commit_chunk_draw_run_break_state_delta_mixed_pair_ib_texture",
        )) or 0.0,
        "state_delta_mixed_pair_ib_shader": number(counter(
            counters, "commit_chunk_draw_run_break_state_delta_mixed_pair_ib_shader",
        )) or 0.0,
        "state_delta_mixed_pair_texture_shader": number(counter(
            counters, "commit_chunk_draw_run_break_state_delta_mixed_pair_texture_shader",
        )) or 0.0,
    }
    color_store = (number(counter(counters, "render_pass_store_action_store")) or 0.0) + (
        number(counter(counters, "render_pass_store_action_dontcare")) or 0.0
    )
    depth_store = (number(counter(counters, "render_pass_store_action_depth_store")) or 0.0) + (
        number(counter(counters, "render_pass_store_action_depth_dontcare")) or 0.0
    )
    metrics = {
        "draws_per_present": ratio(counters, "draw_calls", "present_encoded"),
        "passes_per_present": ratio(counters, "render_pass_begin", "present_encoded"),
        "tile_preservation_mib": (
            (number(counter(counters, "render_pass_tile_preservation_bytes")) or 0.0) /
            (1024.0 * 1024.0)
        ),
        "same_key_preservation_mib": (
            (number(counter(counters, "render_pass_same_key_reentry_preservation_bytes")) or 0.0) /
            (1024.0 * 1024.0)
        ),
        "color_dontcare_store_share_pct": (
            (number(counter(counters, "render_pass_store_action_dontcare")) or 0.0) /
            color_store * 100.0
            if color_store else None
        ),
        "depth_dontcare_store_share_pct": (
            (number(counter(counters, "render_pass_store_action_depth_dontcare")) or 0.0) /
            depth_store * 100.0
            if depth_store else None
        ),
        "completion_wait_ms_per_present": (
            (number(counter(counters, "completion_wait_ms")) or 0.0) / present
            if present else None
        ),
        "draw_run_records_per_submit": ratio(
            counters,
            "commit_chunk_draw_run_records",
            "commit_chunk_draw_run_submits",
        ),
        "draw_submission_batch_records_per_submit": ratio(
            counters,
            "commit_chunk_draw_submission_batch_records",
            "commit_chunk_draw_submission_batch_submits",
        ),
        "backend_draw_run_batch_records_per_group": ratio(
            counters,
            "submit_draw_run_batch_records",
            "submit_draw_run_batch_groups",
        ),
        "binding_override_records_per_draw_run_record": ratio(
            counters,
            "commit_chunk_draw_run_binding_override_records",
            "commit_chunk_draw_run_records",
        ),
        "const_upload_breaks_per_draw": (
            const_upload_breaks / draws if draws else None
        ),
        "const_upload_breaks_per_present": (
            const_upload_breaks / present if present else None
        ),
        "const_upload_break_bytes_per_draw": (
            const_upload_bytes / draws if draws else None
        ),
        "const_upload_break_bytes_per_present": (
            const_upload_bytes / present if present else None
        ),
        "const_upload_break_bytes_per_break": (
            const_upload_bytes / const_upload_breaks if const_upload_breaks else None
        ),
        "const_upload_registers_per_break": (
            const_upload_registers / const_upload_breaks if const_upload_breaks else None
        ),
        "const_uploads_per_state_delta_break": (
            const_upload_breaks / state_delta_breaks if state_delta_breaks else None
        ),
        "const_upload_passthrough_per_draw": (
            const_upload_passthrough / draws if draws else None
        ),
        "const_upload_passthrough_per_present": (
            const_upload_passthrough / present if present else None
        ),
        "state_delta_breaks_per_draw": (
            state_delta_breaks / draws if draws else None
        ),
        "stream_deltas_per_draw": ratio(
            counters, "commit_chunk_draw_delta_stream", "draw_calls",
        ),
        "ib_deltas_per_draw": ratio(
            counters, "commit_chunk_draw_delta_ib", "draw_calls",
        ),
        "const_upload_subtype_coverage_pct": (
            const_subtype_total / const_upload_breaks * 100.0
            if const_upload_breaks else None
        ),
    }
    for subtype, value in state_delta_subtypes.items():
        metrics[f"{subtype}_share_pct"] = (
            value / state_delta_breaks * 100.0 if state_delta_breaks else None
        )
    for subtype, value in const_subtypes.items():
        metrics[f"{subtype}_share_pct"] = (
            value / const_upload_breaks * 100.0 if const_upload_breaks else None
        )
    for subtype, value in const_subtype_bytes.items():
        metrics[f"{subtype}_byte_share_pct"] = (
            value / const_upload_bytes * 100.0 if const_upload_bytes else None
        )
    return metrics


def fmt_derived(value: float | None) -> str:
    if value is None:
        return "n/a"
    if abs(value) >= 1000.0:
        return f"{value:,.3f}"
    return f"{value:.3f}"


def write_report(
    output: Path,
    before_path: Path,
    after_path: Path,
    before_label: str,
    after_label: str,
    before: dict[str, Any],
    after: dict[str, Any],
) -> None:
    keys = tuple(dict.fromkeys((*FOCUS_COUNTERS, *RUN_COUNTERS, *EXTRA_COUNTERS)))
    before_derived = derived(before)
    after_derived = derived(after)

    lines: list[str] = []
    lines.append("# 3DMark05 Perf Counter Comparison")
    lines.append("")
    lines.append(f"- Before: `{before_label}` (`{before_path}`)")
    lines.append(f"- After: `{after_label}` (`{after_path}`)")
    lines.append("")
    lines.append("## Verdict")
    lines.append("")
    preservation_delta, preservation_pct = delta(
        after_derived["tile_preservation_mib"],
        before_derived["tile_preservation_mib"],
    )
    color_store_delta, color_store_pct = delta(
        counter(after, "render_pass_store_action_dontcare"),
        counter(before, "render_pass_store_action_dontcare"),
    )
    gpu_delta, gpu_pct = delta(
        counter(after, "gpu_command_buffer_time_ms"),
        counter(before, "gpu_command_buffer_time_ms"),
    )
    lines.append(
        f"- Tile preservation changed by `{preservation_delta} MiB` (`{preservation_pct}`)."
    )
    lines.append(
        f"- Color DontCare stores changed by `{color_store_delta}` (`{color_store_pct}`)."
    )
    lines.append(f"- GPU command-buffer time changed by `{gpu_delta} ms` (`{gpu_pct}`).")
    lines.append("")

    lines.append("## Derived Metrics")
    lines.append("")
    lines.append("| Metric | Before | After | Delta | Delta % |")
    lines.append("|---|---:|---:|---:|---:|")
    for key in before_derived:
        b = before_derived[key]
        a = after_derived[key]
        diff, pct = delta(a, b)
        lines.append(f"| `{key}` | `{fmt_derived(b)}` | `{fmt_derived(a)}` | `{diff}` | `{pct}` |")
    lines.append("")

    lines.append("## Counters")
    lines.append("")
    lines.append("| Counter | Before | After | Delta | Delta % |")
    lines.append("|---|---:|---:|---:|---:|")
    for key in keys:
        b = counter(before, key)
        a = counter(after, key)
        diff, pct = delta(a, b)
        lines.append(
            f"| `{key}` | `{fmt_value(b)}` | `{fmt_value(a)}` | `{diff}` | `{pct}` |"
        )
    lines.append("")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text("\n".join(lines), encoding="utf-8")


def failed_requirements(args: argparse.Namespace,
                        before: dict[str, Any],
                        after: dict[str, Any]) -> list[str]:
    failures: list[str] = []
    before_derived = derived(before)
    after_derived = derived(after)

    def metric_value(counters: dict[str, Any], key: str) -> float:
        return number(counter(counters, key)) or 0.0

    if args.require_color_dontcare_increase:
        before_value = metric_value(before, "render_pass_store_action_dontcare")
        after_value = metric_value(after, "render_pass_store_action_dontcare")
        if after_value <= before_value:
            failures.append(
                "render_pass_store_action_dontcare did not increase "
                f"({fmt_value(before_value)} -> {fmt_value(after_value)})"
            )

    if args.require_depth_dontcare_increase:
        before_value = metric_value(before, "render_pass_store_action_depth_dontcare")
        after_value = metric_value(after, "render_pass_store_action_depth_dontcare")
        if after_value <= before_value:
            failures.append(
                "render_pass_store_action_depth_dontcare did not increase "
                f"({fmt_value(before_value)} -> {fmt_value(after_value)})"
            )

    if args.require_tile_preservation_decrease:
        before_value = before_derived["tile_preservation_mib"] or 0.0
        after_value = after_derived["tile_preservation_mib"] or 0.0
        if after_value >= before_value:
            failures.append(
                "tile_preservation_mib did not decrease "
                f"({fmt_derived(before_value)} -> {fmt_derived(after_value)})"
            )

    if args.max_gpu_command_buffer_regression_ms is not None:
        before_value = metric_value(before, "gpu_command_buffer_time_ms")
        after_value = metric_value(after, "gpu_command_buffer_time_ms")
        allowed = before_value + args.max_gpu_command_buffer_regression_ms
        if after_value > allowed:
            failures.append(
                "gpu_command_buffer_time_ms regressed beyond tolerance "
                f"({fmt_value(before_value)} -> {fmt_value(after_value)}, "
                f"allowed <= {fmt_value(allowed)})"
            )

    if args.require_draw_run_records_increase:
        before_value = metric_value(before, "commit_chunk_draw_run_records")
        after_value = metric_value(after, "commit_chunk_draw_run_records")
        if after_value <= before_value:
            failures.append(
                "commit_chunk_draw_run_records did not increase "
                f"({fmt_value(before_value)} -> {fmt_value(after_value)})"
            )

    if args.require_draw_run_records_per_submit_increase:
        before_value = before_derived["draw_run_records_per_submit"] or 0.0
        after_value = after_derived["draw_run_records_per_submit"] or 0.0
        if after_value <= before_value:
            failures.append(
                "draw_run_records_per_submit did not increase "
                f"({fmt_derived(before_value)} -> {fmt_derived(after_value)})"
            )

    if args.require_binding_overrides_present:
        after_value = metric_value(after, "commit_chunk_draw_run_binding_override_records")
        if after_value <= 0.0:
            failures.append(
                "commit_chunk_draw_run_binding_override_records stayed zero"
            )

    if args.require_const_upload_passthrough_present:
        after_value = metric_value(after, "commit_chunk_draw_batch_const_upload_passthrough")
        if after_value <= 0.0:
            failures.append(
                "commit_chunk_draw_batch_const_upload_passthrough stayed zero"
            )

    if args.require_draw_submission_batch_present:
        after_submits = metric_value(after, "commit_chunk_draw_submission_batch_submits")
        after_records = metric_value(after, "commit_chunk_draw_submission_batch_records")
        after_max = metric_value(after, "commit_chunk_draw_submission_batch_max_records")
        if after_submits <= 0.0 or after_records <= 0.0 or after_max <= 0.0:
            failures.append(
                "commit_chunk_draw_submission_batch counters stayed zero "
                f"(submits={fmt_value(after_submits)}, "
                f"records={fmt_value(after_records)}, "
                f"max_records={fmt_value(after_max)})"
            )

    if args.require_const_upload_break_bytes_decrease:
        before_value = metric_value(before, "commit_chunk_draw_run_break_type_const_upload_bytes")
        after_value = metric_value(after, "commit_chunk_draw_run_break_type_const_upload_bytes")
        if after_value >= before_value:
            failures.append(
                "commit_chunk_draw_run_break_type_const_upload_bytes did not decrease "
                f"({fmt_value(before_value)} -> {fmt_value(after_value)})"
            )

    if args.max_const_upload_break_count_ratio is not None:
        before_value = metric_value(before, "commit_chunk_draw_run_break_type_const_upload")
        after_value = metric_value(after, "commit_chunk_draw_run_break_type_const_upload")
        if before_value == 0.0:
            if after_value > 0.0:
                failures.append(
                    "commit_chunk_draw_run_break_type_const_upload appeared from zero "
                    f"({fmt_value(before_value)} -> {fmt_value(after_value)})"
                )
        elif after_value / before_value > args.max_const_upload_break_count_ratio:
            failures.append(
                "commit_chunk_draw_run_break_type_const_upload exceeded count ratio "
                f"({fmt_value(before_value)} -> {fmt_value(after_value)}, "
                f"allowed <= {args.max_const_upload_break_count_ratio:.3f}x)"
            )

    if args.require_encode_draw_cpu_decrease:
        before_value = metric_value(before, "encode_draw_cpu_ms")
        after_value = metric_value(after, "encode_draw_cpu_ms")
        if after_value >= before_value:
            failures.append(
                "encode_draw_cpu_ms did not decrease "
                f"({fmt_value(before_value)} -> {fmt_value(after_value)})"
            )

    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("before", type=Path, help="baseline output dir or result.json")
    parser.add_argument("after", type=Path, help="candidate output dir or result.json")
    parser.add_argument("--before-label", default="before")
    parser.add_argument("--after-label", default="after")
    parser.add_argument("--output", type=Path, help="markdown output path")
    parser.add_argument(
        "--require-color-dontcare-increase",
        action="store_true",
        help="exit nonzero unless color StoreActionDontCare count increases",
    )
    parser.add_argument(
        "--require-depth-dontcare-increase",
        action="store_true",
        help="exit nonzero unless depth StoreActionDontCare count increases",
    )
    parser.add_argument(
        "--require-tile-preservation-decrease",
        action="store_true",
        help="exit nonzero unless estimated tile-preservation MiB decreases",
    )
    parser.add_argument(
        "--max-gpu-command-buffer-regression-ms",
        type=float,
        help="exit nonzero if gpu_command_buffer_time_ms regresses beyond this tolerance",
    )
    parser.add_argument(
        "--require-draw-run-records-increase",
        action="store_true",
        help="exit nonzero unless commit_chunk_draw_run_records increases",
    )
    parser.add_argument(
        "--require-draw-run-records-per-submit-increase",
        action="store_true",
        help="exit nonzero unless draw-run records per submit increases",
    )
    parser.add_argument(
        "--require-binding-overrides-present",
        action="store_true",
        help="exit nonzero unless stream/IB draw-run binding override records are emitted",
    )
    parser.add_argument(
        "--require-const-upload-passthrough-present",
        action="store_true",
        help="exit nonzero unless fallback draw batching crosses const-upload records",
    )
    parser.add_argument(
        "--require-draw-submission-batch-present",
        action="store_true",
        help="exit nonzero unless fallback draw submission batch counters are nonzero",
    )
    parser.add_argument(
        "--require-const-upload-break-bytes-decrease",
        action="store_true",
        help="exit nonzero unless const-upload draw-run break bytes decrease",
    )
    parser.add_argument(
        "--max-const-upload-break-count-ratio",
        type=float,
        help="exit nonzero if const-upload draw-run break count exceeds this after/before ratio",
    )
    parser.add_argument(
        "--require-encode-draw-cpu-decrease",
        action="store_true",
        help="exit nonzero unless encode_draw_cpu_ms decreases",
    )
    args = parser.parse_args()
    if (
        args.max_const_upload_break_count_ratio is not None and
        args.max_const_upload_break_count_ratio <= 0.0
    ):
        parser.error("--max-const-upload-break-count-ratio must be positive")

    before_path = counter_source_path(args.before)
    after_path = counter_source_path(args.after)
    output = args.output or after_path.with_name(
        f"{after_path.stem}-perf-counter-comparison.md"
    )
    before = load_counters(args.before)
    after = load_counters(args.after)
    write_report(
        output,
        before_path,
        after_path,
        args.before_label,
        args.after_label,
        before,
        after,
    )
    print(output)
    failures = failed_requirements(args, before, after)
    if failures:
        for failure in failures:
            print(f"requirement failed: {failure}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
