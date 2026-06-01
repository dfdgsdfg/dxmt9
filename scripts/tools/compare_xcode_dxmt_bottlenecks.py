#!/usr/bin/env python3
"""Compare two Xcode+dxmt joined encoder summaries.

Inputs are `frame<N>-xcode-dxmt-joined-summary.csv` files produced by
`summarize_xcode_encoder_counters.py`. The output is a compact Markdown report
for validating whether a candidate change reduced the known GT1 bottlenecks.
"""

from __future__ import annotations

import argparse
import csv
import sys
from pathlib import Path
from typing import Any


def as_float(value: Any) -> float:
    try:
        return float(str(value))
    except (TypeError, ValueError):
        return 0.0


def as_int(value: Any) -> int:
    try:
        text = str(value)
        if text.startswith(("0x", "0X")):
            return int(text, 16)
        return int(float(text))
    except (TypeError, ValueError):
        return 0


def first(row: dict[str, str], *keys: str) -> str:
    for key in keys:
        value = row.get(key)
        if value not in (None, ""):
            return value
    return ""


def load_rows(path: Path) -> list[dict[str, str]]:
    if not path.exists():
        raise SystemExit(f"missing joined summary: {path}")
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def dxmt_cpu_writer_bytes(row: dict[str, str]) -> int:
    explicit = first(row, "dxmt_cpu_writer_bytes", "cpu_writer_bytes")
    if explicit:
        return as_int(explicit)
    return (
        as_int(first(row, "dxmt_argbuf_table_bytes", "argbuf_table_bytes")) +
        as_int(first(row, "dxmt_argbuf_cbuf_bytes", "argbuf_cbuf_bytes")) +
        as_int(first(row, "dxmt_set_vertex_bytes_bytes", "set_vertex_bytes_bytes")) +
        as_int(first(row, "dxmt_transient_vertex_bytes", "transient_vertex_bytes")) +
        as_int(first(row, "dxmt_transient_index_bytes", "transient_index_bytes"))
    )


def row_unexplained_buffer_write_mib(row: dict[str, str]) -> float:
    explicit = first(row, "dxmt_unexplained_buffer_write_mib")
    if explicit:
        return as_float(explicit)
    buffer_write_mib = as_float(row.get("buffer_write_mib"))
    cpu_writer_mib = dxmt_cpu_writer_bytes(row) / 1024.0 / 1024.0
    return max(buffer_write_mib - cpu_writer_mib, 0.0)


def dxmt_vsout_expected_bytes(row: dict[str, str]) -> int:
    explicit = first(row, "dxmt_vsout_expected_stage_out_bytes_per_vertex")
    if explicit:
        return as_int(explicit)
    layout_key = as_int(first(row, "dxmt_vsout_layout_last", "vsout_layout_last"))
    if not layout_key:
        return 0
    texcoord_count = int((layout_key & 0xff).bit_count())
    has_color = 1 if (layout_key & (1 << 8)) else 0
    has_secondary = 1 if (layout_key & (1 << 9)) else 0
    has_fog = 1 if (layout_key & (1 << 10)) else 0
    has_point_size = 1 if (layout_key & (1 << 11)) else 0
    return (
        16 +
        16 * has_color +
        16 * has_secondary +
        16 * texcoord_count +
        4 * has_fog +
        4 * has_point_size
    )


def summarize(rows: list[dict[str, str]], top_n: int) -> dict[str, float]:
    top = rows[:top_n]
    top_vs_mib = sum(as_float(row.get("vs_buffer_write_mib")) for row in top)
    top_vs_bytes = top_vs_mib * 1024.0 * 1024.0
    top_vs_invocations = sum(as_float(row.get("vs_invocations")) for row in top)
    top_fs_invocations = sum(as_float(row.get("fs_invocations")) for row in top)
    top_primitives = sum(as_float(row.get("primitives")) for row in top)
    top_pixels = sum(as_float(row.get("pixels_rasterized")) for row in top)
    top_tiled_mib = sum(
        as_float(row.get("tiled_vertex_buffer_mib")) +
        as_float(row.get("tiled_primitive_block_mib"))
        for row in top
    )
    top_gpu_ms = sum(as_float(row.get("gpu_ms")) for row in top)
    top_buffer_write_mib = sum(as_float(row.get("buffer_write_mib")) for row in top)
    top_dxmt_vertices = sum(as_int(first(row, "dxmt_vertex_count", "vertex_count")) for row in top)
    top_draw_calls = float(sum(as_int(first(row, "dxmt_draw_calls", "draw_calls")) for row in top))
    top_pso_state_samples = float(
        sum(as_int(first(row, "dxmt_pso_state_samples", "pso_state_samples")) for row in top)
    )
    top_cpu_writer_mib = sum(dxmt_cpu_writer_bytes(row) for row in top) / 1024.0 / 1024.0
    top_unexplained_mib = sum(row_unexplained_buffer_write_mib(row) for row in top)
    top_vsout_expected_weight = sum(
        dxmt_vsout_expected_bytes(row) * as_float(row.get("vs_invocations")) for row in top
    )
    top_vsout_invocations = sum(
        as_float(row.get("vs_invocations")) for row in top if dxmt_vsout_expected_bytes(row)
    )
    top_vsout_expected_bytes = (
        top_vsout_expected_weight / top_vsout_invocations if top_vsout_invocations else 0.0
    )
    return {
        "encoders": float(len(rows)),
        "total_gpu_ms": sum(as_float(row.get("gpu_ms")) for row in rows),
        "total_buffer_write_mib": sum(as_float(row.get("buffer_write_mib")) for row in rows),
        "total_device_write_mib": sum(as_float(row.get("device_write_mib")) for row in rows),
        "top_gpu_ms": top_gpu_ms,
        "top_gpu_share_pct": sum(as_float(first(row, "gpu_share_pct", "cost_pct")) for row in top),
        "top_buffer_write_mib": top_buffer_write_mib,
        "top_vs_buffer_write_mib": top_vs_mib,
        "top_vs_buffer_write_share": (
            top_vs_mib / top_buffer_write_mib if top_buffer_write_mib else 0.0
        ),
        "top_fs_buffer_write_mib": sum(as_float(row.get("fs_buffer_write_mib")) for row in top),
        "top_texture_write_mib": sum(as_float(row.get("texture_write_mib")) for row in top),
        "top_depth_write_mib": sum(as_float(row.get("depth_write_mib")) for row in top),
        "top_device_write_mib": sum(as_float(row.get("device_write_mib")) for row in top),
        "top_vs_buffer_bytes_per_vs_invocation": (
            top_vs_bytes / top_vs_invocations if top_vs_invocations else 0.0
        ),
        "top_vs_buffer_bytes_per_fragment": (
            top_vs_bytes / top_fs_invocations if top_fs_invocations else 0.0
        ),
        "top_vs_buffer_bytes_per_primitive": (
            top_vs_bytes / top_primitives if top_primitives else 0.0
        ),
        "top_vs_buffer_bytes_per_pixel": (
            top_vs_bytes / top_pixels if top_pixels else 0.0
        ),
        "top_vs_buffer_to_tiled_buffer_ratio": (
            top_vs_mib / top_tiled_mib if top_tiled_mib else 0.0
        ),
        "top_primitive_block_tile_intersections_pct": (
            sum(as_float(row.get("primitive_block_tile_intersections_pct")) * as_float(row.get("gpu_ms")) for row in top) /
            top_gpu_ms
        ) if top_gpu_ms else 0.0,
        "top_fs_tiles_processed": sum(as_float(row.get("fs_tiles_processed")) for row in top),
        "top_shaded_vertex_read_limiter_pct": (
            sum(as_float(row.get("shaded_vertex_read_limiter_pct")) * as_float(row.get("gpu_ms")) for row in top) /
            top_gpu_ms
        ) if top_gpu_ms else 0.0,
        "top_cull_unit_limiter_pct": (
            sum(as_float(row.get("cull_unit_limiter_pct")) * as_float(row.get("gpu_ms")) for row in top) /
            top_gpu_ms
        ) if top_gpu_ms else 0.0,
        "top_clip_unit_limiter_pct": (
            sum(as_float(row.get("clip_unit_limiter_pct")) * as_float(row.get("gpu_ms")) for row in top) /
            top_gpu_ms
        ) if top_gpu_ms else 0.0,
        "top_vs_buffer_bytes_per_dxmt_vertex": (
            top_vs_bytes / top_dxmt_vertices if top_dxmt_vertices else 0.0
        ),
        "top_vsout_expected_stage_out_bytes_per_vertex": top_vsout_expected_bytes,
        "top_vs_buffer_to_expected_stage_out_ratio": (
            top_vs_bytes / top_vsout_expected_weight if top_vsout_expected_weight else 0.0
        ),
        "top_vs_l1_write_mib": sum(as_float(row.get("vs_l1_write_mib")) for row in top),
        "top_vs_llc_write_mib": sum(as_float(row.get("vs_llc_write_mib")) for row in top),
        "top_vertex_stage_time_pct": (
            sum(as_float(row.get("vertex_stage_time_pct")) * as_float(row.get("gpu_ms")) for row in top) /
            top_gpu_ms
        ) if top_gpu_ms else 0.0,
        "top_vs_alu_limiter_pct": (
            sum(as_float(row.get("vs_alu_limiter_pct")) * as_float(row.get("gpu_ms")) for row in top) /
            top_gpu_ms
        ) if top_gpu_ms else 0.0,
        "top_vs_buffer_write_limiter_pct": (
            sum(as_float(row.get("vs_buffer_write_limiter_pct")) * as_float(row.get("gpu_ms")) for row in top) /
            top_gpu_ms
        ) if top_gpu_ms else 0.0,
        "top_draw_calls": top_draw_calls,
        "top_ffp_draws": float(sum(as_int(first(row, "dxmt_ffp_draws", "ffp_draws")) for row in top)),
        "top_pretransformed_draws": float(
            sum(as_int(first(row, "dxmt_pretransformed_draws", "pretransformed_draws")) for row in top)
        ),
        "top_textured_draws": float(
            sum(as_int(first(row, "dxmt_textured_draws", "textured_draws")) for row in top)
        ),
        "top_dxmt_vertex_count": float(top_dxmt_vertices),
        "top_dxmt_triangle_estimate": float(
            sum(as_int(first(row, "dxmt_triangle_estimate", "triangle_estimate")) for row in top)
        ),
        "top_stream_handle_changes": float(
            sum(as_int(first(row, "dxmt_stream_handle_changes", "stream_handle_changes")) for row in top)
        ),
        "top_stream_offset_changes": float(
            sum(as_int(first(row, "dxmt_stream_offset_changes", "stream_offset_changes")) for row in top)
        ),
        "top_stream_stride_changes": float(
            sum(as_int(first(row, "dxmt_stream_stride_changes", "stream_stride_changes")) for row in top)
        ),
        "top_stream_unique_handles": float(
            sum(as_int(first(row, "dxmt_stream_unique_handles", "stream_unique_handles")) for row in top)
        ),
        "top_stream_unique_mib": sum(
            as_int(first(row, "dxmt_stream_unique_bytes", "stream_unique_bytes")) for row in top
        )
        / 1024.0
        / 1024.0,
        "top_ib_handle_changes": float(
            sum(as_int(first(row, "dxmt_ib_handle_changes", "ib_handle_changes")) for row in top)
        ),
        "top_ib_unique_handles": float(
            sum(as_int(first(row, "dxmt_ib_unique_handles", "ib_unique_handles")) for row in top)
        ),
        "top_ib_unique_mib": sum(
            as_int(first(row, "dxmt_ib_unique_bytes", "ib_unique_bytes")) for row in top
        )
        / 1024.0
        / 1024.0,
        "top_pso_handle_changes": float(
            sum(as_int(first(row, "dxmt_pso_handle_changes", "pso_handle_changes")) for row in top)
        ),
        "top_pso_state_samples": top_pso_state_samples,
        "top_pso_state_samples_per_draw": (
            top_pso_state_samples / top_draw_calls if top_draw_calls else 0.0
        ),
        "top_shader_variant_changes": float(
            sum(as_int(first(row, "dxmt_shader_variant_changes", "shader_variant_changes")) for row in top)
        ),
        "top_vsout_layout_changes": float(
            sum(as_int(first(row, "dxmt_vsout_layout_changes", "vsout_layout_changes")) for row in top)
        ),
        "top_vsout_layout_cache_hits": float(
            sum(as_int(first(row, "dxmt_vsout_layout_cache_hits",
                             "vsout_layout_cache_hits")) for row in top)
        ),
        "top_vsout_layout_cache_misses": float(
            sum(as_int(first(row, "dxmt_vsout_layout_cache_misses",
                             "vsout_layout_cache_misses")) for row in top)
        ),
        "top_argbuf_cbuf_mib": sum(
            as_int(first(row, "dxmt_argbuf_cbuf_bytes", "argbuf_cbuf_bytes")) for row in top
        )
        / 1024.0
        / 1024.0,
        "top_argbuf_table_mib": sum(
            as_int(first(row, "dxmt_argbuf_table_bytes", "argbuf_table_bytes")) for row in top
        )
        / 1024.0
        / 1024.0,
        "top_set_vertex_bytes_mib": sum(
            as_int(first(row, "dxmt_set_vertex_bytes_bytes", "set_vertex_bytes_bytes")) for row in top
        )
        / 1024.0
        / 1024.0,
        "top_dxmt_cpu_writer_mib": top_cpu_writer_mib,
        "top_dxmt_cpu_writer_to_buffer_write_ratio": (
            top_cpu_writer_mib / top_buffer_write_mib if top_buffer_write_mib else 0.0
        ),
        "top_unexplained_buffer_write_mib": top_unexplained_mib,
        "top_unexplained_buffer_write_ratio": (
            top_unexplained_mib / top_buffer_write_mib if top_buffer_write_mib else 0.0
        ),
        "top_transient_mib": sum(
            as_int(first(row, "dxmt_transient_vertex_bytes", "transient_vertex_bytes")) +
            as_int(first(row, "dxmt_transient_index_bytes", "transient_index_bytes"))
            for row in top
        )
        / 1024.0
        / 1024.0,
        "top_transient_vertex_expanded_mib": sum(
            as_int(first(row, "dxmt_transient_vertex_expanded_main_bytes",
                         "transient_vertex_expanded_main_bytes")) +
            as_int(first(row, "dxmt_transient_vertex_expanded_extra_bytes",
                         "transient_vertex_expanded_extra_bytes"))
            for row in top
        )
        / 1024.0
        / 1024.0,
        "top_transient_vertex_decl_fallback_mib": sum(
            as_int(first(row, "dxmt_transient_vertex_decl_fallback_bytes",
                         "transient_vertex_decl_fallback_bytes"))
            for row in top
        )
        / 1024.0
        / 1024.0,
        "top_transient_vertex_up_mib": sum(
            as_int(first(row, "dxmt_transient_vertex_user_bytes", "transient_vertex_user_bytes")) +
            as_int(first(row, "dxmt_transient_vertex_preupload_bytes",
                         "transient_vertex_preupload_bytes"))
            for row in top
        )
        / 1024.0
        / 1024.0,
        "top_transient_index_up_mib": sum(
            as_int(first(row, "dxmt_transient_index_user_bytes", "transient_index_user_bytes")) +
            as_int(first(row, "dxmt_transient_index_preupload_bytes",
                         "transient_index_preupload_bytes"))
            for row in top
        )
        / 1024.0
        / 1024.0,
        "top_transient_index_shadow_mib": sum(
            as_int(first(row, "dxmt_transient_index_shadow_fallback_bytes",
                         "transient_index_shadow_fallback_bytes"))
            for row in top
        )
        / 1024.0
        / 1024.0,
    }


def fmt(value: float) -> str:
    if abs(value) >= 1000.0:
        return f"{value:,.3f}"
    return f"{value:.3f}"


def delta(after: float, before: float) -> tuple[float, str]:
    diff = after - before
    if before == 0:
        return diff, "n/a"
    return diff, f"{diff / before * 100.0:+.2f}%"


def verdict(before: dict[str, float], after: dict[str, float]) -> list[str]:
    notes: list[str] = []
    gpu_delta, gpu_pct = delta(after["top_gpu_ms"], before["top_gpu_ms"])
    write_delta, write_pct = delta(after["top_buffer_write_mib"], before["top_buffer_write_mib"])
    stream_delta, stream_pct = delta(
        after["top_stream_handle_changes"], before["top_stream_handle_changes"]
    )
    ib_delta, ib_pct = delta(after["top_ib_handle_changes"], before["top_ib_handle_changes"])
    if gpu_delta < -0.5:
        notes.append(f"Top encoder GPU time improved ({gpu_pct}).")
    elif gpu_delta > 0.5:
        notes.append(f"Top encoder GPU time regressed ({gpu_pct}).")
    else:
        notes.append("Top encoder GPU time is effectively unchanged.")
    if write_delta < -32.0:
        notes.append(f"Top buffer write traffic improved ({write_pct}).")
    elif write_delta > 32.0:
        notes.append(f"Top buffer write traffic regressed ({write_pct}).")
    else:
        notes.append("Top buffer write traffic is effectively unchanged.")
    unexplained_delta, unexplained_pct = delta(
        after["top_unexplained_buffer_write_mib"],
        before["top_unexplained_buffer_write_mib"],
    )
    if unexplained_delta < -32.0:
        notes.append(f"Unexplained top buffer write traffic improved ({unexplained_pct}).")
    elif unexplained_delta > 32.0:
        notes.append(f"Unexplained top buffer write traffic regressed ({unexplained_pct}).")
    elif after["top_unexplained_buffer_write_ratio"] > 0.90:
        notes.append("Unexplained top buffer write traffic remains dominant.")
    if (after["top_unexplained_buffer_write_ratio"] > 0.90 and
            after["top_pso_state_samples_per_draw"] < 0.90):
        notes.append(
            "PSO/VSOut attribution is incomplete; this comparison cannot prove "
            "a shader-source or VSOut-layout root cause."
        )
    if stream_delta < 0:
        notes.append(f"Stream handle churn improved ({stream_pct}).")
    elif stream_delta > 0:
        notes.append(f"Stream handle churn regressed ({stream_pct}).")
    if ib_delta < 0:
        notes.append(f"IB handle churn improved ({ib_pct}).")
    elif ib_delta > 0:
        notes.append(f"IB handle churn regressed ({ib_pct}).")
    return notes


def write_report(path: Path, before: dict[str, float], after: dict[str, float],
                 before_label: str, after_label: str) -> None:
    keys = (
        "encoders",
        "total_gpu_ms",
        "total_buffer_write_mib",
        "total_device_write_mib",
        "top_gpu_ms",
        "top_gpu_share_pct",
        "top_buffer_write_mib",
        "top_vs_buffer_write_mib",
        "top_vs_buffer_write_share",
        "top_unexplained_buffer_write_mib",
        "top_unexplained_buffer_write_ratio",
        "top_vs_buffer_bytes_per_vs_invocation",
        "top_vs_buffer_bytes_per_fragment",
        "top_vs_buffer_bytes_per_primitive",
        "top_vs_buffer_bytes_per_pixel",
        "top_vs_buffer_to_tiled_buffer_ratio",
        "top_primitive_block_tile_intersections_pct",
        "top_fs_tiles_processed",
        "top_vs_buffer_bytes_per_dxmt_vertex",
        "top_vsout_expected_stage_out_bytes_per_vertex",
        "top_vs_buffer_to_expected_stage_out_ratio",
        "top_vs_l1_write_mib",
        "top_vs_llc_write_mib",
        "top_vertex_stage_time_pct",
        "top_vs_alu_limiter_pct",
        "top_vs_buffer_write_limiter_pct",
        "top_shaded_vertex_read_limiter_pct",
        "top_cull_unit_limiter_pct",
        "top_clip_unit_limiter_pct",
        "top_fs_buffer_write_mib",
        "top_texture_write_mib",
        "top_depth_write_mib",
        "top_device_write_mib",
        "top_draw_calls",
        "top_ffp_draws",
        "top_pretransformed_draws",
        "top_textured_draws",
        "top_dxmt_vertex_count",
        "top_dxmt_triangle_estimate",
        "top_stream_handle_changes",
        "top_stream_offset_changes",
        "top_stream_stride_changes",
        "top_stream_unique_handles",
        "top_stream_unique_mib",
        "top_ib_handle_changes",
        "top_ib_unique_handles",
        "top_ib_unique_mib",
        "top_pso_handle_changes",
        "top_pso_state_samples",
        "top_pso_state_samples_per_draw",
        "top_shader_variant_changes",
        "top_vsout_layout_changes",
        "top_vsout_layout_cache_hits",
        "top_vsout_layout_cache_misses",
        "top_argbuf_cbuf_mib",
        "top_argbuf_table_mib",
        "top_set_vertex_bytes_mib",
        "top_dxmt_cpu_writer_mib",
        "top_dxmt_cpu_writer_to_buffer_write_ratio",
        "top_transient_mib",
        "top_transient_vertex_expanded_mib",
        "top_transient_vertex_decl_fallback_mib",
        "top_transient_vertex_up_mib",
        "top_transient_index_up_mib",
        "top_transient_index_shadow_mib",
    )
    lines: list[str] = []
    lines.append("# Xcode/dxmt Bottleneck Comparison")
    lines.append("")
    lines.append(f"- Before: `{before_label}`")
    lines.append(f"- After: `{after_label}`")
    lines.append("")
    lines.append("## Verdict")
    lines.append("")
    for note in verdict(before, after):
        lines.append(f"- {note}")
    lines.append("")
    lines.append("## Metrics")
    lines.append("")
    lines.append("| Metric | Before | After | Delta | Delta % |")
    lines.append("|---|---:|---:|---:|---:|")
    for key in keys:
        diff, pct = delta(after[key], before[key])
        lines.append(
            f"| `{key}` | `{fmt(before[key])}` | `{fmt(after[key])}` | "
            f"`{fmt(diff)}` | `{pct}` |"
        )
    lines.append("")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines), encoding="utf-8")


def failed_requirements(args: argparse.Namespace,
                        before: dict[str, float],
                        after: dict[str, float]) -> list[str]:
    failures: list[str] = []

    def require_decrease(key: str, label: str) -> None:
        if after[key] >= before[key]:
            failures.append(
                f"{label} did not decrease ({fmt(before[key])} -> {fmt(after[key])})"
            )

    def require_increase(key: str, label: str) -> None:
        if after[key] <= before[key]:
            failures.append(
                f"{label} did not increase ({fmt(before[key])} -> {fmt(after[key])})"
            )

    if args.require_top_gpu_decrease:
        require_decrease("top_gpu_ms", "top_gpu_ms")
    if args.require_top_buffer_write_decrease:
        require_decrease("top_buffer_write_mib", "top_buffer_write_mib")
    if args.require_top_vs_buffer_write_decrease:
        require_decrease("top_vs_buffer_write_mib", "top_vs_buffer_write_mib")
    if args.require_top_unexplained_buffer_write_decrease:
        require_decrease(
            "top_unexplained_buffer_write_mib",
            "top_unexplained_buffer_write_mib",
        )
    if args.require_stream_handle_churn_decrease:
        require_decrease("top_stream_handle_changes", "top_stream_handle_changes")
    if args.require_ib_handle_churn_decrease:
        require_decrease("top_ib_handle_changes", "top_ib_handle_changes")
    if args.require_argbuf_cbuf_decrease:
        require_decrease("top_argbuf_cbuf_mib", "top_argbuf_cbuf_mib")
    if args.require_transient_decrease:
        require_decrease("top_transient_mib", "top_transient_mib")
    if args.require_top_gpu_share_increase:
        require_increase("top_gpu_share_pct", "top_gpu_share_pct")
    if args.max_top_unexplained_buffer_write_ratio is not None:
        if after["top_unexplained_buffer_write_ratio"] > args.max_top_unexplained_buffer_write_ratio:
            failures.append(
                "top_unexplained_buffer_write_ratio exceeds limit "
                f"({fmt(after['top_unexplained_buffer_write_ratio'])}, "
                f"allowed <= {fmt(args.max_top_unexplained_buffer_write_ratio)})"
            )
    if args.max_top_gpu_regression_ms is not None:
        allowed = before["top_gpu_ms"] + args.max_top_gpu_regression_ms
        if after["top_gpu_ms"] > allowed:
            failures.append(
                "top_gpu_ms regressed beyond tolerance "
                f"({fmt(before['top_gpu_ms'])} -> {fmt(after['top_gpu_ms'])}, "
                f"allowed <= {fmt(allowed)})"
            )
    if args.max_top_buffer_write_regression_mib is not None:
        allowed = before["top_buffer_write_mib"] + args.max_top_buffer_write_regression_mib
        if after["top_buffer_write_mib"] > allowed:
            failures.append(
                "top_buffer_write_mib regressed beyond tolerance "
                f"({fmt(before['top_buffer_write_mib'])} -> "
                f"{fmt(after['top_buffer_write_mib'])}, allowed <= {fmt(allowed)})"
            )
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("before", type=Path)
    parser.add_argument("after", type=Path)
    parser.add_argument("--before-label", default="before")
    parser.add_argument("--after-label", default="after")
    parser.add_argument("--top", type=int, default=3)
    parser.add_argument("--output", type=Path, default=Path("xcode-dxmt-bottleneck-comparison.md"))
    parser.add_argument(
        "--require-top-gpu-decrease",
        action="store_true",
        help="exit nonzero unless top-N GPU time decreases",
    )
    parser.add_argument(
        "--require-top-buffer-write-decrease",
        action="store_true",
        help="exit nonzero unless top-N buffer write MiB decreases",
    )
    parser.add_argument(
        "--require-top-vs-buffer-write-decrease",
        action="store_true",
        help="exit nonzero unless top-N VS buffer write MiB decreases",
    )
    parser.add_argument(
        "--require-top-unexplained-buffer-write-decrease",
        action="store_true",
        help="exit nonzero unless top-N buffer write not explained by dxmt CPU writers decreases",
    )
    parser.add_argument(
        "--require-stream-handle-churn-decrease",
        action="store_true",
        help="exit nonzero unless top-N stream handle changes decrease",
    )
    parser.add_argument(
        "--require-ib-handle-churn-decrease",
        action="store_true",
        help="exit nonzero unless top-N IB handle changes decrease",
    )
    parser.add_argument(
        "--require-argbuf-cbuf-decrease",
        action="store_true",
        help="exit nonzero unless top-N dxmt argbuf cbuf MiB decreases",
    )
    parser.add_argument(
        "--require-transient-decrease",
        action="store_true",
        help="exit nonzero unless top-N dxmt transient MiB decreases",
    )
    parser.add_argument(
        "--require-top-gpu-share-increase",
        action="store_true",
        help="exit nonzero unless top-N frame GPU share increases",
    )
    parser.add_argument(
        "--max-top-gpu-regression-ms",
        type=float,
        help="exit nonzero if top-N GPU time regresses beyond this tolerance",
    )
    parser.add_argument(
        "--max-top-buffer-write-regression-mib",
        type=float,
        help="exit nonzero if top-N buffer write MiB regresses beyond this tolerance",
    )
    parser.add_argument(
        "--max-top-unexplained-buffer-write-ratio",
        type=float,
        help="exit nonzero if top-N unexplained buffer write / buffer write exceeds this ratio",
    )
    args = parser.parse_args()

    before = summarize(load_rows(args.before), args.top)
    after = summarize(load_rows(args.after), args.top)
    write_report(args.output, before, after, args.before_label, args.after_label)
    print(args.output)
    failures = failed_requirements(args, before, after)
    if failures:
        for failure in failures:
            print(f"requirement failed: {failure}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
