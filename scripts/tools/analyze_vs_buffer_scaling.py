#!/usr/bin/env python3
"""Classify 3DMark05 VS buffer-write scaling across joined Xcode/dxmt CSVs."""

from __future__ import annotations

import argparse
import csv
import math
import sys
from pathlib import Path
from typing import Any


MIB = 1024.0 * 1024.0


def as_float(value: Any) -> float:
    try:
        text = str(value).strip()
        if not text:
            return 0.0
        if text.startswith(("0x", "0X")):
            return float(int(text, 16))
        return float(text)
    except (TypeError, ValueError):
        return 0.0


def as_int(value: Any) -> int:
    try:
        text = str(value).strip()
        if not text:
            return 0
        if text.startswith(("0x", "0X")):
            return int(text, 16)
        return int(float(text))
    except (TypeError, ValueError):
        return 0


def fmt_float(value: Any, digits: int = 3) -> str:
    return f"{as_float(value):.{digits}f}"


def fmt_signed_float(value: Any, digits: int = 3) -> str:
    numeric = as_float(value)
    prefix = "+" if numeric > 0.0 else ""
    return f"{prefix}{numeric:.{digits}f}"


def fmt_int(value: Any) -> str:
    return f"{as_int(value):,}"


def ratio(numerator: float, denominator: float) -> float:
    return numerator / denominator if denominator else 0.0


def pearson(xs: list[float], ys: list[float]) -> float:
    pairs = [(x, y) for x, y in zip(xs, ys) if math.isfinite(x) and math.isfinite(y)]
    if len(pairs) < 2:
        return 0.0
    x_values = [p[0] for p in pairs]
    y_values = [p[1] for p in pairs]
    x_mean = sum(x_values) / len(x_values)
    y_mean = sum(y_values) / len(y_values)
    x_var = sum((x - x_mean) ** 2 for x in x_values)
    y_var = sum((y - y_mean) ** 2 for y in y_values)
    if x_var == 0.0 or y_var == 0.0:
        return 0.0
    cov = sum((x - x_mean) * (y - y_mean) for x, y in pairs)
    return cov / math.sqrt(x_var * y_var)


def load_rows(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        rows = list(csv.DictReader(handle))
    for row in rows:
        row["_source"] = str(path)
        if not row.get("run"):
            row["run"] = path.parent.parent.name
    return rows


def dxmt_cpu_writer_mib(row: dict[str, str]) -> float:
    explicit = as_float(row.get("dxmt_cpu_writer_mib"))
    if explicit:
        return explicit
    return (
        as_float(row.get("dxmt_argbuf_table_bytes")) +
        as_float(row.get("dxmt_argbuf_cbuf_bytes")) +
        as_float(row.get("dxmt_set_vertex_bytes_bytes")) +
        as_float(row.get("dxmt_transient_vertex_bytes")) +
        as_float(row.get("dxmt_transient_index_bytes"))
    ) / MIB


def expected_vsout_mib(row: dict[str, str]) -> float:
    return (
        as_float(row.get("dxmt_vsout_expected_stage_out_bytes_per_vertex")) *
        as_float(row.get("vs_invocations"))
    ) / MIB


def stream0_input_mib(row: dict[str, str]) -> float:
    explicit = as_float(row.get("dxmt_stream0_input_max_mib"))
    if explicit:
        return explicit
    return (
        as_float(row.get("dxmt_vertex_count")) *
        as_float(row.get("dxmt_stream0_stride_max"))
    ) / MIB


def tiled_buffer_mib(row: dict[str, str]) -> float:
    return (
        as_float(row.get("tiled_vertex_buffer_mib")) +
        as_float(row.get("tiled_primitive_block_mib"))
    )


def state_churn(row: dict[str, str]) -> float:
    return (
        as_float(row.get("dxmt_stream_handle_changes")) +
        as_float(row.get("dxmt_stream_offset_changes")) +
        as_float(row.get("dxmt_stream_stride_changes")) +
        as_float(row.get("dxmt_ib_handle_changes"))
    )


def geometry_signature_duplicates(row: dict[str, str]) -> float:
    return as_float(row.get("dxmt_draw_geometry_signature_duplicates"))


def large_primitive_draws(row: dict[str, str]) -> float:
    return (
        as_float(row.get("dxmt_draw_primitive_bucket_1024_4095")) +
        as_float(row.get("dxmt_draw_primitive_bucket_4096_plus"))
    )


def large_vertex_draws(row: dict[str, str]) -> float:
    return (
        as_float(row.get("dxmt_draw_vertex_bucket_4096_16383")) +
        as_float(row.get("dxmt_draw_vertex_bucket_16384_plus"))
    )


def metric_value(row: dict[str, str], metric: str) -> float:
    if metric in row and str(row.get(metric, "")).strip():
        return as_float(row.get(metric))
    special = {
        "cpu_writer_mib": dxmt_cpu_writer_mib,
        "expected_vsout_mib": expected_vsout_mib,
        "stream0_input_mib": stream0_input_mib,
        "tiled_buffer_mib": tiled_buffer_mib,
        "state_churn": state_churn,
        "geometry_signature_duplicates": geometry_signature_duplicates,
        "large_primitive_draws": large_primitive_draws,
        "large_vertex_draws": large_vertex_draws,
    }
    if metric in special:
        return special[metric](row)
    return as_float(row.get(metric))


def aggregate(rows: list[dict[str, str]], top_n: int) -> dict[str, Any]:
    sorted_rows = sorted(rows, key=lambda row: as_float(row.get("gpu_ms")), reverse=True)
    top = sorted_rows[:top_n]
    vs_mib = sum(as_float(row.get("vs_buffer_write_mib")) for row in top)
    buffer_mib = sum(as_float(row.get("buffer_write_mib")) for row in top)
    gpu_ms = sum(as_float(row.get("gpu_ms")) for row in top)
    vs_invocations = sum(as_float(row.get("vs_invocations")) for row in top)
    primitives = sum(as_float(row.get("primitives")) for row in top)
    post_clipped = sum(as_float(row.get("post_clipped_primitives")) for row in top)
    pixels = sum(as_float(row.get("pixels_rasterized")) for row in top)
    dxmt_vertices = sum(as_float(row.get("dxmt_vertex_count")) for row in top)
    expected_mib = sum(expected_vsout_mib(row) for row in top)
    stream_input_mib = sum(stream0_input_mib(row) for row in top)
    tiled_mib = sum(tiled_buffer_mib(row) for row in top)
    cpu_writer = sum(dxmt_cpu_writer_mib(row) for row in top)
    churn = sum(state_churn(row) for row in top)
    draws = sum(as_float(row.get("dxmt_draw_calls")) for row in top)
    large_prims = sum(large_primitive_draws(row) for row in top)
    large_vertices = sum(large_vertex_draws(row) for row in top)
    return {
        "run": top[0].get("run", "") if top else "",
        "source": top[0].get("_source", "") if top else "",
        "encoder_rows": len(rows),
        "top_n": len(top),
        "top_row_keys": ",".join(
            f"{as_int(row.get('seq'))}/{as_int(row.get('enc'))}" for row in top),
        "gpu_ms": gpu_ms,
        "vs_buffer_mib": vs_mib,
        "vs_buffer_write_mib": vs_mib,
        "buffer_write_mib": buffer_mib,
        "vs_buffer_share": ratio(vs_mib, buffer_mib),
        "cpu_writer_mib": cpu_writer,
        "cpu_writer_ratio": ratio(cpu_writer, buffer_mib),
        "expected_vsout_mib": expected_mib,
        "vsout_ratio": ratio(vs_mib, expected_mib),
        "stream0_input_mib": stream_input_mib,
        "stream0_input_ratio": ratio(vs_mib, stream_input_mib),
        "tiled_buffer_mib": tiled_mib,
        "tiled_ratio": ratio(vs_mib, tiled_mib),
        "vs_b_per_vs_invocation": ratio(vs_mib * MIB, vs_invocations),
        "vs_b_per_primitive": ratio(vs_mib * MIB, primitives),
        "vs_b_per_post_clipped_primitive": ratio(vs_mib * MIB, post_clipped),
        "vs_b_per_pixel": ratio(vs_mib * MIB, pixels),
        "vs_b_per_dxmt_vertex": ratio(vs_mib * MIB, dxmt_vertices),
        "vs_invocations": vs_invocations,
        "dxmt_vertex_count": dxmt_vertices,
        "primitives": primitives,
        "post_clipped_primitives": post_clipped,
        "pixels_rasterized": pixels,
        "fs_invocations": sum(as_float(row.get("fs_invocations")) for row in top),
        "draw_calls": draws,
        "state_churn": churn,
        "geometry_signature_duplicates": sum(
            geometry_signature_duplicates(row) for row in top),
        "large_primitive_draws": large_prims,
        "large_vertex_draws": large_vertices,
        "large_primitive_draw_share": ratio(large_prims, draws),
        "large_vertex_draw_share": ratio(large_vertices, draws),
        "draw_primitive_max": max(
            (as_float(row.get("dxmt_draw_primitive_max")) for row in top),
            default=0.0),
        "draw_vertex_max": max(
            (as_float(row.get("dxmt_draw_vertex_max")) for row in top),
            default=0.0),
        "state_churn_per_draw": ratio(churn, draws),
        "weighted_vs_write_limiter": weighted(top, "vs_buffer_write_limiter_pct"),
        "weighted_vertex_stage_time": weighted(top, "vertex_stage_time_pct"),
        "weighted_vs_alu_limiter": weighted(top, "vs_alu_limiter_pct"),
    }


def weighted(rows: list[dict[str, str]], key: str) -> float:
    total_gpu = sum(as_float(row.get("gpu_ms")) for row in rows)
    if not total_gpu:
        return 0.0
    return sum(as_float(row.get(key)) * as_float(row.get("gpu_ms")) for row in rows) / total_gpu


def correlation_rows(rows: list[dict[str, str]]) -> list[dict[str, Any]]:
    metrics = (
        ("vs_invocations", "VS invocations"),
        ("dxmt_vertex_count", "dxmt vertices"),
        ("primitives", "primitives"),
        ("post_clipped_primitives", "post-clipped primitives"),
        ("pixels_rasterized", "pixels"),
        ("fs_invocations", "FS invocations"),
        ("expected_vsout_mib", "expected VSOut bytes"),
        ("stream0_input_mib", "stream0 input bytes"),
        ("tiled_buffer_mib", "tiled vertex+primitive bytes"),
        ("cpu_writer_mib", "dxmt CPU writer bytes"),
        ("state_churn", "stream/IB state churn"),
        ("geometry_signature_duplicates", "geometry signature duplicates"),
        ("large_primitive_draws", "large primitive draws"),
        ("large_vertex_draws", "large vertex draws"),
        ("dxmt_draw_primitive_max", "max primitive count/draw"),
        ("dxmt_draw_vertex_max", "max vertex count/draw"),
        ("vs_buffer_write_limiter_pct", "VS buffer-write limiter"),
        ("vertex_stage_time_pct", "vertex stage time pct"),
    )
    target = [metric_value(row, "vs_buffer_write_mib") for row in rows]
    out = []
    for key, label in metrics:
        values = [metric_value(row, key) for row in rows]
        out.append({
            "metric": label,
            "pearson_r": pearson(values, target),
            "nonzero_rows": sum(1 for value in values if value != 0.0),
        })
    return sorted(out, key=lambda item: abs(item["pearson_r"]), reverse=True)


def dominant_binary(row: dict[str, str],
                    enabled_key: str,
                    total_key: str = "dxmt_draw_calls") -> str:
    if enabled_key not in row or str(row.get(enabled_key, "")).strip() == "":
        return "unknown"
    total = as_float(row.get(total_key))
    enabled = as_float(row.get(enabled_key))
    if total <= 0.0:
        return "unknown"
    if enabled <= 0.0:
        return "off"
    if enabled >= total:
        return "on"
    return "mixed"


def dominant_cull(row: dict[str, str]) -> str:
    draws = as_float(row.get("dxmt_draw_calls"))
    if not any(str(row.get(key, "")).strip() for key in (
        "dxmt_cull_none_draws",
        "dxmt_cull_front_draws",
        "dxmt_cull_back_draws",
    )):
        return "unknown"
    buckets = (
        ("none", as_float(row.get("dxmt_cull_none_draws"))),
        ("front", as_float(row.get("dxmt_cull_front_draws"))),
        ("back", as_float(row.get("dxmt_cull_back_draws"))),
    )
    if draws <= 0.0:
        return "unknown"
    if sum(value for _, value in buckets) <= 0.0:
        return "unknown"
    name, value = max(buckets, key=lambda item: item[1])
    return name if value >= draws else "mixed"


def dominant_depth(row: dict[str, str]) -> str:
    draws = as_float(row.get("dxmt_draw_calls"))
    if not any(str(row.get(key, "")).strip() for key in (
        "dxmt_depth_enabled_draws",
        "dxmt_depth_write_draws",
    )):
        return "unknown"
    enabled = as_float(row.get("dxmt_depth_enabled_draws"))
    writes = as_float(row.get("dxmt_depth_write_draws"))
    if draws <= 0.0:
        return "unknown"
    if enabled <= 0.0:
        return "off"
    if enabled >= draws and writes <= 0.0:
        return "read"
    if enabled >= draws and writes >= draws:
        return "write"
    return "mixed"


def state_shape(row: dict[str, str]) -> str:
    return ",".join((
        f"cull={dominant_cull(row)}",
        f"depth={dominant_depth(row)}",
        f"scissor={dominant_binary(row, 'dxmt_scissor_enabled_draws')}",
        f"blend={dominant_binary(row, 'dxmt_alpha_blend_enabled_draws')}",
        f"textured={dominant_binary(row, 'dxmt_textured_draws')}",
        f"ffp={dominant_binary(row, 'dxmt_ffp_draws')}",
        f"preT={dominant_binary(row, 'dxmt_pretransformed_draws')}",
    ))


def shape_rows(rows: list[dict[str, str]],
               limit: int = 12,
               include_unknown: bool = False) -> list[dict[str, Any]]:
    groups: dict[str, dict[str, Any]] = {}
    for row in rows:
        if as_float(row.get("vs_buffer_write_mib")) <= 0.0:
            continue
        key = state_shape(row)
        if not include_unknown and "unknown" in key:
            continue
        group = groups.setdefault(key, {
            "shape": key,
            "rows": 0,
            "gpu_ms": 0.0,
            "vs_buffer_mib": 0.0,
            "buffer_write_mib": 0.0,
            "vs_invocations": 0.0,
            "primitives": 0.0,
            "post_clipped_primitives": 0.0,
            "pixels": 0.0,
            "draw_calls": 0.0,
            "state_churn": 0.0,
            "weighted_vs_write_limiter_sum": 0.0,
            "weighted_vs_alu_limiter_sum": 0.0,
            "top_encoder": "",
            "top_encoder_gpu_ms": 0.0,
        })
        gpu_ms = as_float(row.get("gpu_ms"))
        group["rows"] += 1
        group["gpu_ms"] += gpu_ms
        group["vs_buffer_mib"] += as_float(row.get("vs_buffer_write_mib"))
        group["buffer_write_mib"] += as_float(row.get("buffer_write_mib"))
        group["vs_invocations"] += as_float(row.get("vs_invocations"))
        group["primitives"] += as_float(row.get("primitives"))
        group["post_clipped_primitives"] += as_float(row.get("post_clipped_primitives"))
        group["pixels"] += as_float(row.get("pixels_rasterized"))
        group["draw_calls"] += as_float(row.get("dxmt_draw_calls"))
        group["state_churn"] += state_churn(row)
        group["weighted_vs_write_limiter_sum"] += (
            as_float(row.get("vs_buffer_write_limiter_pct")) * gpu_ms)
        group["weighted_vs_alu_limiter_sum"] += (
            as_float(row.get("vs_alu_limiter_pct")) * gpu_ms)
        if gpu_ms > group["top_encoder_gpu_ms"]:
            group["top_encoder_gpu_ms"] = gpu_ms
            group["top_encoder"] = (
                f"{row.get('run', '')}:seq={row.get('seq', '')},enc={row.get('enc', '')}")
    out = []
    for group in groups.values():
        gpu_ms = as_float(group["gpu_ms"])
        vs_mib = as_float(group["vs_buffer_mib"])
        group["vs_share"] = ratio(vs_mib, sum(as_float(g["vs_buffer_mib"]) for g in groups.values()))
        group["vs_b_per_vs_invocation"] = ratio(vs_mib * MIB, as_float(group["vs_invocations"]))
        group["vs_b_per_primitive"] = ratio(vs_mib * MIB, as_float(group["primitives"]))
        group["vs_b_per_post_clipped_primitive"] = ratio(
            vs_mib * MIB, as_float(group["post_clipped_primitives"]))
        group["state_churn_per_draw"] = ratio(
            as_float(group["state_churn"]), as_float(group["draw_calls"]))
        group["weighted_vs_write_limiter"] = ratio(
            as_float(group["weighted_vs_write_limiter_sum"]), gpu_ms)
        group["weighted_vs_alu_limiter"] = ratio(
            as_float(group["weighted_vs_alu_limiter_sum"]), gpu_ms)
        out.append(group)
    return sorted(out, key=lambda item: item["vs_buffer_mib"], reverse=True)[:limit]


def pct_delta(value: float, baseline: float) -> float:
    return ((value - baseline) / baseline * 100.0) if baseline else 0.0


def find_baseline(aggregates: list[dict[str, Any]], name: str | None) -> dict[str, Any] | None:
    if not name:
        return None
    exact = [row for row in aggregates if str(row.get("run", "")) == name]
    if exact:
        return exact[0]
    matches = [row for row in aggregates if name in str(row.get("run", ""))]
    if len(matches) == 1:
        return matches[0]
    return None


def candidate_kind(run: str) -> str:
    text = run.lower()
    if "forceexpand" in text or "force-expand" in text:
        return "negative-geometry"
    if "opaque-proof" in text or "opaque_depth" in text or "opaque-depth" in text:
        return "locality-reorder"
    if "screenblend-proof" in text or "screen-blend-proof" in text:
        return "locality-reorder"
    if "cacheopt" in text or "index-cache" in text or "index_cache" in text:
        return "locality-reorder"
    if any(token in text for token in (
        "half-vsout",
        "live-vsout",
        "trim-varyings",
        "trim_unused_varyings",
        "trim-unused-varyings",
        "texturewhite",
        "texture-white",
        "disable-scissor",
        "disable-cull",
        "depth-func",
        "alpha-blend",
        "backend-shape",
    )):
        return "non-reorder-backend-shape"
    return "unknown"


def backend_shape_gate(row: dict[str, Any]) -> tuple[str, str]:
    kind = str(row.get("candidate_kind", ""))
    if kind != "non-reorder-backend-shape":
        return "not-applicable", f"{kind or 'unknown'} candidate"
    if not row.get("geometry_stable"):
        return "reject", "geometry/top-row shape drifted"
    gpu_delta = as_float(row.get("gpu_delta_pct"))
    vs_b_delta = as_float(row.get("vs_b_per_inv_delta_pct"))
    vs_inv_delta = as_float(row.get("vs_invocations_delta_pct"))
    if vs_b_delta <= -5.0 and gpu_delta <= -2.0 and abs(vs_inv_delta) <= 2.0:
        return (
            "pass",
            "bytes/inv moved materially, GPU improved, and VS invocation count stayed stable",
        )
    reasons = []
    if vs_b_delta > -5.0:
        reasons.append("bytes/inv reduction < 5%")
    if gpu_delta > -2.0:
        reasons.append("GPU did not improve by >= 2%")
    if abs(vs_inv_delta) > 2.0:
        reasons.append("VS invocation count moved too much for backend-shape attribution")
    return "reject", "; ".join(reasons)


def vs_write_delta_components(row: dict[str, Any], baseline: dict[str, Any]) -> dict[str, Any]:
    baseline_mib = as_float(baseline.get("vs_buffer_mib"))
    row_mib = as_float(row.get("vs_buffer_mib"))
    baseline_invocations = as_float(baseline.get("vs_invocations"))
    row_invocations = as_float(row.get("vs_invocations"))
    baseline_bpi = as_float(baseline.get("vs_b_per_vs_invocation"))
    row_bpi = as_float(row.get("vs_b_per_vs_invocation"))
    invocation_effect_mib = (
        (row_invocations - baseline_invocations) *
        ((baseline_bpi + row_bpi) / 2.0) /
        MIB
    )
    bytes_per_invocation_effect_mib = (
        (row_bpi - baseline_bpi) *
        ((baseline_invocations + row_invocations) / 2.0) /
        MIB
    )
    total_delta_mib = row_mib - baseline_mib
    residual_mib = (
        total_delta_mib -
        invocation_effect_mib -
        bytes_per_invocation_effect_mib
    )
    effects = {
        "invocations": abs(invocation_effect_mib),
        "bytes_per_invocation": abs(bytes_per_invocation_effect_mib),
    }
    primary = max(effects, key=effects.get)
    if effects[primary] < 0.001:
        primary = "none"
    return {
        "vs_write_delta_mib": total_delta_mib,
        "invocation_effect_mib": invocation_effect_mib,
        "bytes_per_invocation_effect_mib": bytes_per_invocation_effect_mib,
        "residual_mib": residual_mib,
        "primary_mover": primary,
    }


def baseline_deltas(aggregates: list[dict[str, Any]],
                    baseline: dict[str, Any] | None,
                    limit: int) -> list[dict[str, Any]]:
    if not baseline:
        return []
    rows = []
    base_run = str(baseline.get("run", ""))
    for row in aggregates:
        run = str(row.get("run", ""))
        gpu_delta = pct_delta(as_float(row.get("gpu_ms")), as_float(baseline.get("gpu_ms")))
        vs_delta = pct_delta(
            as_float(row.get("vs_buffer_mib")),
            as_float(baseline.get("vs_buffer_mib")))
        draw_delta = pct_delta(
            as_float(row.get("draw_calls")),
            as_float(baseline.get("draw_calls")))
        vertex_delta = pct_delta(
            as_float(row.get("dxmt_vertex_count")),
            as_float(baseline.get("dxmt_vertex_count")))
        primitive_delta = pct_delta(
            as_float(row.get("primitives")),
            as_float(baseline.get("primitives")))
        vs_invocations_delta = pct_delta(
            as_float(row.get("vs_invocations")),
            as_float(baseline.get("vs_invocations")))
        vs_b_per_inv_delta = pct_delta(
            as_float(row.get("vs_b_per_vs_invocation")),
            as_float(baseline.get("vs_b_per_vs_invocation")))
        row_keys_match = str(row.get("top_row_keys", "")) == str(
            baseline.get("top_row_keys", ""))
        vsout_delta = pct_delta(
            as_float(row.get("expected_vsout_mib")),
            as_float(baseline.get("expected_vsout_mib")))
        tiled_delta = pct_delta(
            as_float(row.get("tiled_buffer_mib")),
            as_float(baseline.get("tiled_buffer_mib")))
        geometry_stable = (
            row_keys_match and
            abs(draw_delta) <= 1.0 and
            abs(vertex_delta) <= 5.0 and
            abs(primitive_delta) <= 5.0)
        vs_moved = abs(vs_delta) >= 5.0
        gpu_moved = abs(gpu_delta) >= 2.0
        if run == base_run:
            verdict = "baseline"
        elif geometry_stable and vs_moved:
            verdict = "shape-stable VS-moved"
        elif geometry_stable and gpu_moved:
            verdict = "shape-stable GPU-only"
        elif geometry_stable:
            verdict = "shape-stable unchanged"
        elif vs_moved:
            verdict = "shape-drift VS-moved"
        else:
            verdict = "shape-drift inconclusive"
        candidate = candidate_kind(run)
        item = {
            "run": run,
            "candidate_kind": candidate,
            "gpu_delta_pct": gpu_delta,
            "vs_delta_pct": vs_delta,
            "draw_delta_pct": draw_delta,
            "vertex_delta_pct": vertex_delta,
            "primitive_delta_pct": primitive_delta,
            "vs_invocations_delta_pct": vs_invocations_delta,
            "vs_b_per_inv_delta_pct": vs_b_per_inv_delta,
            "row_keys_match": row_keys_match,
            "vsout_delta_pct": vsout_delta,
            "tiled_delta_pct": tiled_delta,
            "geometry_stable": geometry_stable,
            "verdict": verdict,
        }
        gate, reason = backend_shape_gate(item)
        item["backend_shape_gate"] = gate
        item["backend_shape_reason"] = reason
        item.update(vs_write_delta_components(row, baseline))
        rows.append(item)
    non_base = [row for row in rows if row["verdict"] != "baseline"]
    return sorted(
        non_base,
        key=lambda row: (
            0 if "VS-moved" in str(row["verdict"]) else 1,
            -abs(as_float(row["vs_delta_pct"])),
            -abs(as_float(row["gpu_delta_pct"])),
            str(row["run"])))[:limit]


def write_summary(path: Path,
                  aggregates: list[dict[str, Any]],
                  encoder_correlations: list[dict[str, Any]],
                  aggregate_correlations: list[dict[str, Any]],
                  state_shapes: list[dict[str, Any]],
                  baseline_delta_rows: list[dict[str, Any]],
                  baseline_run: str | None) -> None:
    lines = [
        "# VS Buffer Scaling Analysis",
        "",
        "This report compares joined Xcode/dxmt encoder summaries. It is meant to",
        "classify whether Xcode's VS buffer-write bucket is explained by explicit",
        "dxmt CPU writers, ordinary VSOut width, stream input bytes, tiled counters,",
        "or hidden GPU-side vertex/backend storage.",
        "",
        "## Top Encoder Aggregates",
        "",
        "| Run | GPU ms | VS buffer MiB | CPU writer MiB | CPU/buffer | Expected VSOut MiB | VS/VSOut | Stream0 input MiB | VS/input | Tiled MiB | VS/tiled | VS B/VS inv | VS B/prim | VS write limiter % | VS ALU limiter % |",
        "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for row in aggregates:
        lines.append(
            "| "
            + " | ".join([
                str(row["run"]),
                fmt_float(row["gpu_ms"]),
                fmt_float(row["vs_buffer_mib"]),
                fmt_float(row["cpu_writer_mib"]),
                fmt_float(row["cpu_writer_ratio"], 4),
                fmt_float(row["expected_vsout_mib"]),
                fmt_float(row["vsout_ratio"], 1),
                fmt_float(row["stream0_input_mib"]),
                fmt_float(row["stream0_input_ratio"], 1),
                fmt_float(row["tiled_buffer_mib"]),
                fmt_float(row["tiled_ratio"], 1),
                fmt_float(row["vs_b_per_vs_invocation"], 1),
                fmt_float(row["vs_b_per_primitive"], 1),
                fmt_float(row["weighted_vs_write_limiter"], 2),
                fmt_float(row["weighted_vs_alu_limiter"], 2),
            ])
            + " |"
        )
    lines.extend([
        "",
        "## Encoder Row Correlation",
        "",
        "| Metric | Pearson r vs VS buffer MiB | Nonzero rows |",
        "|---|---:|---:|",
    ])
    for row in encoder_correlations:
        lines.append(
            f"| {row['metric']} | `{fmt_float(row['pearson_r'], 3)}` | `{fmt_int(row['nonzero_rows'])}` |"
        )
    lines.extend([
        "",
        "## Run Aggregate Correlation",
        "",
        "Run-level correlation is low-signal when all captures use the same frame and",
        "scene. It is still useful as a guard against A/B experiments that should",
        "have moved a candidate metric but did not move VS buffer writes.",
        "",
        "| Metric | Pearson r vs VS buffer MiB | Nonzero runs |",
        "|---|---:|---:|",
    ])
    for row in aggregate_correlations:
        lines.append(
            f"| {row['metric']} | `{fmt_float(row['pearson_r'], 3)}` | `{fmt_int(row['nonzero_rows'])}` |"
        )
    if baseline_delta_rows:
        lines.extend([
            "",
            "## Baseline Delta Triage",
            "",
            f"Baseline: `{baseline_run}`. Geometry-stable means top aggregate",
            "row keys match, draw count changed by at most 1%, and top aggregate",
            "vertex/primitive counts changed by at most 5%. VS-moved means top",
            "aggregate VS buffer write changed by at least 5%; GPU-moved means",
            "GPU time changed by at least 2%.",
            "",
            "| Run | Verdict | Row keys | GPU delta % | VS buffer delta % | VS invocations delta % | VS B/inv delta % | Draw delta % | Vertex delta % | Primitive delta % | VSOut delta % | Tiled delta % |",
            "|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
        ])
        for row in baseline_delta_rows:
            lines.append(
                "| "
                + " | ".join([
                    str(row["run"]),
                    str(row["verdict"]),
                    "match" if row["row_keys_match"] else "drift",
                    fmt_float(row["gpu_delta_pct"], 2),
                    fmt_float(row["vs_delta_pct"], 2),
                    fmt_float(row["vs_invocations_delta_pct"], 2),
                    fmt_float(row["vs_b_per_inv_delta_pct"], 2),
                    fmt_float(row["draw_delta_pct"], 2),
                    fmt_float(row["vertex_delta_pct"], 2),
                    fmt_float(row["primitive_delta_pct"], 2),
                    fmt_float(row["vsout_delta_pct"], 2),
                    fmt_float(row["tiled_delta_pct"], 2),
                ])
                + " |"
            )
        backend_rows = [
            row for row in baseline_delta_rows
            if row.get("candidate_kind") == "non-reorder-backend-shape"
        ]
        if backend_rows:
            lines.extend([
                "",
                "## Non-Reorder Backend-Shape Gate",
                "",
                "This gate is intentionally stricter than the generic VS-buffer",
                "delta triage. A non-reorder backend-shape candidate must keep",
                "geometry stable, reduce `VS B/VS invocation` by at least 5%,",
                "improve top GPU time by at least 2%, and avoid explaining the",
                "movement primarily through VS invocation count changes.",
                "",
                "| Run | Gate | Reason | GPU delta % | VS B/inv delta % | VS invocation delta % |",
                "|---|---|---|---:|---:|---:|",
            ])
            for row in backend_rows:
                lines.append(
                    "| "
                    + " | ".join([
                        str(row["run"]),
                        str(row["backend_shape_gate"]),
                        str(row["backend_shape_reason"]),
                        fmt_float(row["gpu_delta_pct"], 2),
                        fmt_float(row["vs_b_per_inv_delta_pct"], 2),
                        fmt_float(row["vs_invocations_delta_pct"], 2),
                    ])
                    + " |"
                )
        lines.extend([
            "",
            "## VS Write Delta Attribution",
            "",
            "This decomposition uses the same midpoint formula as",
            "`compare_xcode_dxmt_bottlenecks.py`: `VS write delta =",
            "invocation-count effect + bytes/inv effect + residual`. It is the",
            "main preflight signal for separating locality wins from non-reorder",
            "backend-shape candidates.",
            "",
            "| Run | Total VS write delta MiB | Invocation-count effect MiB | Bytes/inv effect MiB | Residual MiB | Primary mover |",
            "|---|---:|---:|---:|---:|---|",
        ])
        for row in baseline_delta_rows:
            lines.append(
                "| "
                + " | ".join([
                    str(row["run"]),
                    fmt_signed_float(row["vs_write_delta_mib"]),
                    fmt_signed_float(row["invocation_effect_mib"]),
                    fmt_signed_float(row["bytes_per_invocation_effect_mib"]),
                    fmt_signed_float(row["residual_mib"]),
                    str(row["primary_mover"]),
                ])
                + " |"
            )
    lines.extend([
        "",
        "## Render-State Shape Split",
        "",
        "| Shape | Rows | GPU ms | VS buffer MiB | VS share | VS B/VS inv | VS B/prim | Churn/draw | VS write limiter % | Top encoder |",
        "|---|---:|---:|---:|---:|---:|---:|---:|---:|---|",
    ])
    for row in state_shapes:
        lines.append(
            "| "
            + " | ".join([
                str(row["shape"]),
                fmt_int(row["rows"]),
                fmt_float(row["gpu_ms"]),
                fmt_float(row["vs_buffer_mib"]),
                fmt_float(row["vs_share"], 3),
                fmt_float(row["vs_b_per_vs_invocation"], 1),
                fmt_float(row["vs_b_per_primitive"], 1),
                fmt_float(row["state_churn_per_draw"], 2),
                fmt_float(row["weighted_vs_write_limiter"], 2),
                str(row["top_encoder"]),
            ])
            + " |"
        )

    def nonzero_range(key: str) -> tuple[float, float]:
        values = [as_float(row[key]) for row in aggregates if as_float(row[key]) != 0.0]
        return (min(values), max(values)) if values else (0.0, 0.0)

    min_cpu_ratio, max_cpu_ratio = nonzero_range("cpu_writer_ratio")
    min_vsout_ratio, max_vsout_ratio = nonzero_range("vsout_ratio")
    min_tiled_ratio, max_tiled_ratio = nonzero_range("tiled_ratio")
    lines.extend([
        "",
        "## Classification",
        "",
        f"- dxmt CPU writer ratio range: `{min_cpu_ratio:.4f}x` to `{max_cpu_ratio:.4f}x`.",
        f"- VS buffer / expected VSOut range: `{min_vsout_ratio:.1f}x` to `{max_vsout_ratio:.1f}x`.",
        f"- VS buffer / named tiled-buffer counters range: `{min_tiled_ratio:.1f}x` to `{max_tiled_ratio:.1f}x`.",
        "- Current classification: explicit dxmt CPU writers and source-visible",
        "  VSOut width are rejected as first-order owners. Named tiled-buffer",
        "  counters are also too small to be the whole bucket. The surviving",
        "  hypothesis is hidden Apple GPU vertex-stage/parameter/tiler storage or",
        "  compiler-internal storage below the MSL/AIR source shapes already tested.",
        "",
    ])
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines), encoding="utf-8")


def write_aggregate_csv(path: Path, aggregates: list[dict[str, Any]]) -> None:
    fields = [
        "run",
        "source",
        "encoder_rows",
        "top_n",
        "top_row_keys",
        "gpu_ms",
        "vs_buffer_mib",
        "buffer_write_mib",
        "vs_buffer_share",
        "cpu_writer_mib",
        "cpu_writer_ratio",
        "expected_vsout_mib",
        "vsout_ratio",
        "stream0_input_mib",
        "stream0_input_ratio",
        "tiled_buffer_mib",
        "tiled_ratio",
        "vs_b_per_vs_invocation",
        "vs_invocations",
        "vs_b_per_primitive",
        "vs_b_per_post_clipped_primitive",
        "vs_b_per_pixel",
        "vs_b_per_dxmt_vertex",
        "dxmt_vertex_count",
        "primitives",
        "post_clipped_primitives",
        "draw_calls",
        "state_churn",
        "geometry_signature_duplicates",
        "large_primitive_draws",
        "large_vertex_draws",
        "large_primitive_draw_share",
        "large_vertex_draw_share",
        "draw_primitive_max",
        "draw_vertex_max",
        "state_churn_per_draw",
        "weighted_vs_write_limiter",
        "weighted_vertex_stage_time",
        "weighted_vs_alu_limiter",
    ]
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        for row in aggregates:
            writer.writerow({field: row.get(field, "") for field in fields})


def write_delta_csv(path: Path, rows: list[dict[str, Any]]) -> None:
    fields = [
        "run",
        "candidate_kind",
        "verdict",
        "backend_shape_gate",
        "backend_shape_reason",
        "geometry_stable",
        "row_keys_match",
        "gpu_delta_pct",
        "vs_delta_pct",
        "vs_invocations_delta_pct",
        "vs_b_per_inv_delta_pct",
        "vs_write_delta_mib",
        "invocation_effect_mib",
        "bytes_per_invocation_effect_mib",
        "residual_mib",
        "primary_mover",
        "draw_delta_pct",
        "vertex_delta_pct",
        "primitive_delta_pct",
        "vsout_delta_pct",
        "tiled_delta_pct",
    ]
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        for row in rows:
            writer.writerow({field: row.get(field, "") for field in fields})


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("joined_csv", type=Path, nargs="+")
    parser.add_argument("--top-n", type=int, default=3)
    parser.add_argument("--report-output", type=Path)
    parser.add_argument("--aggregate-output", type=Path)
    parser.add_argument(
        "--delta-output",
        type=Path,
        help="Optional CSV for baseline-delta triage and VS write attribution.")
    parser.add_argument(
        "--baseline-run",
        default="current-normal-gputrace-r1",
        help="Run name or unique substring used for baseline delta triage.")
    parser.add_argument(
        "--delta-limit",
        type=int,
        default=24,
        help="Maximum baseline-delta rows to include in the report.")
    args = parser.parse_args()

    rows_by_run: list[list[dict[str, str]]] = []
    for path in args.joined_csv:
        if not path.exists():
            raise SystemExit(f"missing joined CSV: {path}")
        rows = load_rows(path)
        if not rows:
            continue
        rows_by_run.append(rows)
    if not rows_by_run:
        raise SystemExit("no joined rows loaded")

    aggregates = [aggregate(rows, args.top_n) for rows in rows_by_run]
    all_rows = [
        row
        for rows in rows_by_run
        for row in rows
        if as_float(row.get("gpu_ms")) > 0.0 and as_float(row.get("vs_buffer_write_mib")) > 0.0
    ]
    aggregate_rows = [
        {key: str(value) for key, value in row.items()}
        for row in aggregates
    ]
    encoder_correlations = correlation_rows(all_rows)
    aggregate_correlations = correlation_rows(aggregate_rows)
    states = shape_rows(all_rows)
    baseline = find_baseline(aggregates, args.baseline_run)
    deltas = baseline_deltas(aggregates, baseline, args.delta_limit)

    if args.aggregate_output:
        write_aggregate_csv(args.aggregate_output, aggregates)
    if args.delta_output:
        write_delta_csv(args.delta_output, deltas)
    if args.report_output:
        write_summary(args.report_output, aggregates,
                      encoder_correlations, aggregate_correlations, states,
                      deltas, baseline.get("run") if baseline else None)
    else:
        for row in aggregates:
            print(
                f"{row['run']}: gpu={row['gpu_ms']:.3f}ms "
                f"vs_buffer={row['vs_buffer_mib']:.3f}MiB "
                f"cpu_writer={row['cpu_writer_mib']:.3f}MiB "
                f"vs/vsout={row['vsout_ratio']:.1f}x "
                f"vs/tiled={row['tiled_ratio']:.1f}x"
            )
    return 0


if __name__ == "__main__":
    sys.exit(main())
