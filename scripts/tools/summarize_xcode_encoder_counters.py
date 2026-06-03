#!/usr/bin/env python3
"""Summarize Xcode Metal encoder counters and join dxmt9 encoder attribution.

The script consumes the CSV produced by Xcode's "Export Encoder Counters".
When a dxmt9 log or `3dmark05-perf-encoders.csv` is provided, it joins rows by
the render encoder label `RenderPass[seq=...,enc=...]`.
"""

from __future__ import annotations

import argparse
import csv
import re
import sys
from pathlib import Path
from typing import Any

KEY_VALUE_RE = re.compile(r"\b([A-Za-z0-9_]+)=([^\s]+)")
DXMT_ENCODER_PREFIX = "[dxmt9-perf-encoder "
RENDER_PASS_LABEL_RE = re.compile(
    r"RenderPass\[seq=(\d+),enc=(\d+),rt=(0x[0-9a-fA-F]+),depth=(0x[0-9a-fA-F]+)\]"
)

REQUIRED_XCODE_COUNTER_COLUMNS = (
    "Index",
    "Encoder Index",
    "CommandBuffer Index",
    "CommandBuffer Label",
    "Encoder Label",
    "GPU Time",
    "Bytes Written To Device Memory",
    "Buffer Device Memory Bytes Written",
    "VS Buffer Device Memory Bytes Written",
    "VS Invocations",
    "FS Invocations",
    "Primitives",
    "Pixels Rasterized",
    "Tiled Vertex Buffer Bytes",
    "Tiled Vertex Buffer Primitive Blocks Bytes",
    "VS Buffer L1 Bytes Written",
    "VS Last Level Cache Bytes Written",
    "Vertex Stage Time",
    "VS Buffer Write Limiter",
    "VS ALU Limiter",
    "Buffer Write Limiter",
    "Last Level Cache Limiter",
    "MMU Limiter",
)

SUMMARY_FIELDS = (
    "run",
    "seq",
    "enc",
    "xcode_index",
    "encoder_index",
    "command_buffer_index",
    "command_buffer_label",
    "gpu_share_pct",
    "gpu_ms",
    "rt",
    "depth",
    "device_write_mib",
    "buffer_write_mib",
    "vs_write_mib",
    "vs_buffer_write_mib",
    "fs_write_mib",
    "fs_buffer_write_mib",
    "texture_write_mib",
    "depth_write_mib",
    "tiled_vertex_buffer_mib",
    "tiled_primitive_block_mib",
    "vertices",
    "vs_invocations",
    "vs_buffer_bytes_per_vs_invocation",
    "vs_buffer_bytes_per_fragment",
    "vs_buffer_bytes_per_primitive",
    "vs_buffer_bytes_per_post_clipped_primitive",
    "vs_buffer_bytes_per_primitive_tile_estimate",
    "vs_buffer_bytes_per_pixel",
    "vs_buffer_to_tiled_buffer_ratio",
    "vs_l1_write_mib",
    "vs_llc_write_mib",
    "vs_l1_to_device_write_ratio",
    "vs_llc_to_device_write_ratio",
    "vertex_stage_time_pct",
    "vs_buffer_write_limiter_pct",
    "vs_buffer_read_limiter_pct",
    "vs_alu_limiter_pct",
    "shaded_vertex_read_limiter_pct",
    "cull_unit_limiter_pct",
    "clip_unit_limiter_pct",
    "primitives",
    "post_clipped_primitives",
    "primitive_block_tile_intersections_pct",
    "tiling_block_utilization_pct",
    "primitives_rendered_pct",
    "primitives_culled_backface_pct",
    "primitives_culled_offscreen_pct",
    "primitives_clipped_pct",
    "fragments_per_primitive",
    "fs_tiles_processed",
    "primitives_per_tile",
    "pixels_rasterized",
    "fs_invocations",
    "varyings_per_fragment",
    "vs_occupancy_pct",
    "fs_occupancy_pct",
    "buffer_write_limiter_pct",
    "llc_limiter_pct",
    "mmu_limiter_pct",
)

JOINED_EXTRA_FIELDS = (
    "dxmt_rt_format",
    "dxmt_rt_width",
    "dxmt_rt_height",
    "dxmt_rt_bpp",
    "dxmt_rt_alias_texture",
    "dxmt_rt_texture_usage",
    "dxmt_rt_format_swizzle",
    "dxmt_rt_texture_needs_shader_read_view",
    "dxmt_depth_format",
    "dxmt_depth_width",
    "dxmt_depth_height",
    "dxmt_depth_bpp",
    "dxmt_depth_alias_texture",
    "dxmt_depth_texture_usage",
    "dxmt_depth_format_swizzle",
    "dxmt_depth_texture_needs_shader_read_view",
    "dxmt_draw_calls",
    "dxmt_indexed_draws",
    "dxmt_expanded_indexed_draws",
    "dxmt_ffp_draws",
    "dxmt_programmable_draws",
    "dxmt_pretransformed_draws",
    "dxmt_textured_draws",
    "dxmt_cull_none_draws",
    "dxmt_cull_front_draws",
    "dxmt_cull_back_draws",
    "dxmt_fill_solid_draws",
    "dxmt_fill_wireframe_draws",
    "dxmt_depth_enabled_draws",
    "dxmt_depth_write_draws",
    "dxmt_depth_func_less_draws",
    "dxmt_depth_func_lessequal_draws",
    "dxmt_depth_func_always_draws",
    "dxmt_depth_func_other_draws",
    "dxmt_scissor_enabled_draws",
    "dxmt_alpha_blend_enabled_draws",
    "dxmt_blend_state_samples",
    "dxmt_blend_state_changes",
    "dxmt_blend_state_unique",
    "dxmt_blend_state_last",
    "dxmt_blend_enabled_noop_draws",
    "dxmt_blend_constant_factor_draws",
    "dxmt_alpha_test_enabled_draws",
    "dxmt_alpha_test_effective_draws",
    "dxmt_clip_plane_enabled_draws",
    "dxmt_primitive_count",
    "dxmt_triangle_estimate",
    "dxmt_vertex_count",
    "dxmt_draw_primitive_min",
    "dxmt_draw_primitive_max",
    "dxmt_draw_vertex_min",
    "dxmt_draw_vertex_max",
    "dxmt_draw_primitive_bucket_1_63",
    "dxmt_draw_primitive_bucket_64_255",
    "dxmt_draw_primitive_bucket_256_1023",
    "dxmt_draw_primitive_bucket_1024_4095",
    "dxmt_draw_primitive_bucket_4096_plus",
    "dxmt_draw_vertex_bucket_1_255",
    "dxmt_draw_vertex_bucket_256_1023",
    "dxmt_draw_vertex_bucket_1024_4095",
    "dxmt_draw_vertex_bucket_4096_16383",
    "dxmt_draw_vertex_bucket_16384_plus",
    "dxmt_draw_geometry_signature_samples",
    "dxmt_draw_geometry_signature_unique",
    "dxmt_draw_geometry_signature_unique_overflows",
    "dxmt_draw_geometry_signature_duplicates",
    "dxmt_draw_geometry_signature_consecutive_duplicates",
    "dxmt_draw_geometry_signature_duplicate_ratio",
    "dxmt_draw_geometry_signature_consecutive_duplicate_ratio",
    "dxmt_draw_geometry_signature_last",
    "dxmt_indexed_triangle_opaque_depth_write_draws",
    "dxmt_indexed_triangle_opaque_depth_write_primitives",
    "dxmt_indexed_triangle_opaque_depth_write_vertices",
    "dxmt_indexed_triangle_depth_read_draws",
    "dxmt_indexed_triangle_depth_read_primitives",
    "dxmt_indexed_triangle_depth_read_vertices",
    "dxmt_indexed_triangle_alpha_blend_draws",
    "dxmt_indexed_triangle_alpha_blend_primitives",
    "dxmt_indexed_triangle_alpha_blend_vertices",
    "dxmt_indexed_triangle_scissor_draws",
    "dxmt_indexed_triangle_scissor_primitives",
    "dxmt_indexed_triangle_scissor_vertices",
    "dxmt_indexed_triangle_textured_draws",
    "dxmt_indexed_triangle_textured_primitives",
    "dxmt_indexed_triangle_textured_vertices",
    "dxmt_indexed_triangle_large_4096_draws",
    "dxmt_indexed_triangle_large_4096_primitives",
    "dxmt_indexed_triangle_large_4096_vertices",
    "dxmt_indexed_triangle_large_4096_opaque_depth_write_draws",
    "dxmt_indexed_triangle_large_4096_opaque_depth_write_primitives",
    "dxmt_indexed_triangle_large_4096_opaque_depth_write_vertices",
    "dxmt_indexed_triangle_large_4096_depth_read_draws",
    "dxmt_indexed_triangle_large_4096_depth_read_primitives",
    "dxmt_indexed_triangle_large_4096_depth_read_vertices",
    "dxmt_indexed_triangle_large_4096_alpha_blend_draws",
    "dxmt_indexed_triangle_large_4096_alpha_blend_primitives",
    "dxmt_indexed_triangle_large_4096_alpha_blend_vertices",
    "dxmt_indexed_triangle_large_4096_scissor_draws",
    "dxmt_indexed_triangle_large_4096_scissor_primitives",
    "dxmt_indexed_triangle_large_4096_scissor_vertices",
    "dxmt_indexed_triangle_large_4096_textured_draws",
    "dxmt_indexed_triangle_large_4096_textured_primitives",
    "dxmt_indexed_triangle_large_4096_textured_vertices",
    "dxmt_indexed_base_vertex_samples",
    "dxmt_indexed_base_vertex_nonzero_draws",
    "dxmt_indexed_base_vertex_negative_draws",
    "dxmt_indexed_base_vertex_positive_draws",
    "dxmt_indexed_base_vertex_min",
    "dxmt_indexed_base_vertex_max",
    "dxmt_native_base_vertex_requested_draws",
    "dxmt_native_base_vertex_used_draws",
    "dxmt_native_base_vertex_skipped_negative_draws",
    "dxmt_split_large_indexed_source_draws",
    "dxmt_split_large_indexed_metal_draws",
    "dxmt_split_large_indexed_extra_draws",
    "dxmt_split_large_indexed_primitive_limit",
    "dxmt_split_large_indexed_primitive_count",
    "dxmt_indexed_order_probe_draws",
    "dxmt_indexed_order_probe_skipped",
    "dxmt_indexed_order_probe_bytes",
    "dxmt_indexed_vertex_reuse_samples",
    "dxmt_indexed_vertex_reuse_skipped",
    "dxmt_indexed_vertex_reference_count",
    "dxmt_indexed_unique_vertex_estimate",
    "dxmt_indexed_vertex_cache_miss_estimate_16",
    "dxmt_indexed_vertex_cache_miss_estimate_32",
    "dxmt_indexed_vertex_cache_miss_estimate_64",
    "dxmt_indexed_cache_opt_candidate_draws",
    "dxmt_indexed_cache_opt_candidate_skipped",
    "dxmt_indexed_cache_opt_candidate_bytes",
    "dxmt_indexed_cache_opt_candidate_original_miss16",
    "dxmt_indexed_cache_opt_candidate_original_miss32",
    "dxmt_indexed_cache_opt_candidate_original_miss64",
    "dxmt_indexed_cache_opt_candidate_miss16",
    "dxmt_indexed_cache_opt_candidate_miss32",
    "dxmt_indexed_cache_opt_candidate_miss64",
    "dxmt_reordered_index_cache_lookups",
    "dxmt_reordered_index_cache_hits",
    "dxmt_reordered_index_cache_misses",
    "dxmt_reordered_index_cache_created",
    "dxmt_reordered_index_cache_created_bytes",
    "dxmt_texture_mask_or",
    "dxmt_fragment_texture_binding_samples",
    "dxmt_fragment_texture_binding_mask_or",
    "dxmt_x8_rt_texture_binding_samples",
    "dxmt_x8_rt_texture_binding_mask_or",
    "dxmt_x8_rt_texture_binding_unique_handles",
    "dxmt_x8_rt_texture_binding_unique_handle_overflows",
    "dxmt_x8_rt_texture_binding_shader_read_view_samples",
    "dxmt_x8_rt_texture_binding_active_rt_alias_samples",
    "dxmt_x8_shader_alpha_fill_samples",
    "dxmt_x8_shader_alpha_fill_mask_or",
    "dxmt_x8_rt_texture_binding_last_stage",
    "dxmt_x8_rt_texture_binding_last_handle",
    "dxmt_stream0_stride_min",
    "dxmt_stream0_stride_max",
    "dxmt_stream_state_samples",
    "dxmt_stream_metal_binds",
    "dxmt_stream_metal_bind_firsts",
    "dxmt_stream_metal_bind_handle_changes",
    "dxmt_stream_metal_bind_offset_changes",
    "dxmt_stream_unique_handles",
    "dxmt_stream_unique_bytes",
    "dxmt_stream_unique_dynamic_handles",
    "dxmt_stream_unique_writeonly_handles",
    "dxmt_stream_unique_default_pool_handles",
    "dxmt_stream_unique_managed_pool_handles",
    "dxmt_stream_unique_systemmem_pool_handles",
    "dxmt_stream_unique_scratch_pool_handles",
    "dxmt_stream_handle_changes",
    "dxmt_stream_offset_changes",
    "dxmt_stream_stride_changes",
    "dxmt_stream0_last_handle",
    "dxmt_stream0_last_offset",
    "dxmt_stream0_last_stride",
    "dxmt_ib_state_samples",
    "dxmt_ib_metal_binds",
    "dxmt_ib_handle_changes",
    "dxmt_ib_unique_handles",
    "dxmt_ib_unique_bytes",
    "dxmt_ib_unique_dynamic_handles",
    "dxmt_ib_unique_writeonly_handles",
    "dxmt_ib_unique_default_pool_handles",
    "dxmt_ib_unique_managed_pool_handles",
    "dxmt_ib_unique_systemmem_pool_handles",
    "dxmt_ib_unique_scratch_pool_handles",
    "dxmt_ib_last_handle",
    "dxmt_pso_state_samples",
    "dxmt_pso_state_samples_per_draw",
    "dxmt_pso_handle_changes",
    "dxmt_pso_unique_handles",
    "dxmt_pso_last_handle",
    "dxmt_shader_variant_changes",
    "dxmt_shader_variant_unique",
    "dxmt_shader_variant_last",
    "dxmt_vertex_shader_last",
    "dxmt_pixel_shader_last",
    "dxmt_vertex_shader_source_last",
    "dxmt_pixel_shader_source_last",
    "dxmt_vsout_layout_changes",
    "dxmt_vsout_layout_unique",
    "dxmt_vsout_layout_last",
    "dxmt_vsout_layout_cache_hits",
    "dxmt_vsout_layout_cache_misses",
    "dxmt_argbuf_table_bytes",
    "dxmt_argbuf_cbuf_bytes",
    "dxmt_argbuf_cbuf_vs_bytes",
    "dxmt_argbuf_cbuf_ffp_vs_bytes",
    "dxmt_argbuf_cbuf_ps_bytes",
    "dxmt_argbuf_cbuf_ffp_ps_bytes",
    "dxmt_argbuf_cbuf_vs_first_bytes",
    "dxmt_argbuf_cbuf_vs_rewrite_changed_bytes",
    "dxmt_argbuf_cbuf_vs_rewrite_unchanged_bytes",
    "dxmt_argbuf_cbuf_ffp_vs_first_bytes",
    "dxmt_argbuf_cbuf_ffp_vs_rewrite_changed_bytes",
    "dxmt_argbuf_cbuf_ffp_vs_rewrite_unchanged_bytes",
    "dxmt_set_vertex_bytes_calls",
    "dxmt_set_vertex_bytes_bytes",
    "dxmt_set_vertex_bytes_slot5_calls",
    "dxmt_set_vertex_bytes_slot5_bytes",
    "dxmt_set_vertex_bytes_other_calls",
    "dxmt_set_vertex_bytes_other_bytes",
    "dxmt_transient_vertex_bytes",
    "dxmt_transient_vertex_user_bytes",
    "dxmt_transient_vertex_preupload_bytes",
    "dxmt_transient_vertex_decl_fallback_bytes",
    "dxmt_transient_vertex_expanded_main_bytes",
    "dxmt_transient_vertex_expanded_extra_bytes",
    "dxmt_transient_index_bytes",
    "dxmt_transient_index_user_bytes",
    "dxmt_transient_index_preupload_bytes",
    "dxmt_transient_index_shadow_fallback_bytes",
    "dxmt_transient_index_probe_reorder_bytes",
    "dxmt_geometry_transient_bytes",
    "dxmt_cpu_writer_bytes",
    "dxmt_cpu_writer_mib",
    "dxmt_cpu_writer_to_buffer_write_ratio",
    "dxmt_vs_buffer_write_share",
    "dxmt_unexplained_buffer_write_mib",
    "dxmt_unexplained_buffer_write_ratio",
    "dxmt_argbuf_cbuf_to_buffer_write_ratio",
    "dxmt_transient_to_buffer_write_ratio",
    "dxmt_stream_handle_changes_per_draw",
    "dxmt_stream_offset_changes_per_draw",
    "dxmt_stream_stride_changes_per_draw",
    "dxmt_ib_handle_changes_per_draw",
    "dxmt_primitives_per_draw",
    "dxmt_vertices_per_draw",
    "dxmt_large_primitive_draw_share",
    "dxmt_large_vertex_draw_share",
    "dxmt_vs_buffer_bytes_per_dxmt_vertex",
    "dxmt_vs_invocations_per_dxmt_vertex",
    "dxmt_indexed_vertex_reuse_ratio",
    "dxmt_vs_invocations_per_indexed_unique_vertex",
    "dxmt_vs_buffer_bytes_per_indexed_unique_vertex",
    "dxmt_indexed_vertex_cache_miss_over_unique_16",
    "dxmt_indexed_vertex_cache_miss_over_unique_32",
    "dxmt_indexed_vertex_cache_miss_over_unique_64",
    "dxmt_vs_invocations_per_indexed_cache_miss_16",
    "dxmt_vs_invocations_per_indexed_cache_miss_32",
    "dxmt_vs_invocations_per_indexed_cache_miss_64",
    "dxmt_vs_buffer_bytes_per_indexed_cache_miss_16",
    "dxmt_vs_buffer_bytes_per_indexed_cache_miss_32",
    "dxmt_vs_buffer_bytes_per_indexed_cache_miss_64",
    "dxmt_stream0_input_min_mib",
    "dxmt_stream0_input_max_mib",
    "dxmt_vs_buffer_to_stream0_input_min_ratio",
    "dxmt_vs_buffer_to_stream0_input_max_ratio",
    "dxmt_vsout_texcoord_mask",
    "dxmt_vsout_texcoord_count",
    "dxmt_vsout_has_color",
    "dxmt_vsout_has_secondary_color",
    "dxmt_vsout_has_fog_factor",
    "dxmt_vsout_has_point_size",
    "dxmt_vsout_expected_stage_out_bytes_per_vertex",
    "dxmt_vs_buffer_to_expected_stage_out_ratio",
    "dxmt_named_tiled_buffer_mib",
    "dxmt_hidden_backend_write_mib",
    "dxmt_hidden_backend_write_ratio",
    "dxmt_tvb_pressure_proxy_mib",
    "dxmt_tvb_named_to_proxy_ratio",
    "dxmt_vs_buffer_write_to_tvb_proxy_ratio",
    "dxmt_backend_storage_class",
    "dxmt_backend_storage_confidence",
    "dxmt_backend_probe_hint",
    "dxmt_gpu_write_hint",
    "dxmt_write_owner_confidence",
)


def parse_value(raw: Any) -> int | float | str:
    text = str(raw).strip().rstrip("]")
    if text == "":
        return 0
    if text.startswith("0x"):
        return text
    if text.endswith("%"):
        text = text[:-1]
    try:
        if "." in text or "e" in text.lower():
            return float(text)
        return int(text)
    except ValueError:
        return text


def header_index(header: list[str], name: str, occurrence: int = 0) -> int | None:
    seen = 0
    for i, item in enumerate(header):
        if item == name:
            if seen == occurrence:
                return i
            seen += 1
    return None


def cell(row: list[str], indexes: dict[tuple[str, int], int | None], name: str,
         occurrence: int = 0) -> str:
    index = indexes.get((name, occurrence))
    if index is None or index >= len(row):
        return ""
    return row[index]


def numeric(row: list[str], indexes: dict[tuple[str, int], int | None], name: str,
            occurrence: int = 0) -> float:
    value = parse_value(cell(row, indexes, name, occurrence))
    if isinstance(value, (int, float)):
        return float(value)
    return 0.0


def read_csv_rows(path: Path) -> tuple[list[str], list[list[str]]]:
    with path.open(newline="", encoding="utf-8") as handle:
        rows = list(csv.reader(handle))
    if not rows:
        raise SystemExit(f"empty xcode csv: {path}")
    return rows[0], rows[1:]


def missing_required_xcode_columns(header: list[str]) -> list[str]:
    return [name for name in REQUIRED_XCODE_COUNTER_COLUMNS
            if header_index(header, name) is None]


def mib(value: float) -> float:
    return value / (1024.0 * 1024.0)


def parse_dxmt_kv_line(line: str) -> dict[str, Any]:
    parsed: dict[str, Any] = {}
    for key, raw in KEY_VALUE_RE.findall(line):
        parsed[key] = parse_value(raw)
    return parsed


def load_dxmt_from_log(path: Path) -> dict[tuple[int, int], dict[str, Any]]:
    rows: dict[tuple[int, int], dict[str, Any]] = {}
    if not path.exists():
        raise SystemExit(f"missing dxmt log: {path}")
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if not line.startswith(DXMT_ENCODER_PREFIX):
            continue
        parsed = parse_dxmt_kv_line(line)
        seq = parsed.get("seq")
        enc = parsed.get("encoder")
        if isinstance(seq, int) and isinstance(enc, int):
            rows[(seq, enc)] = parsed
    return rows


def load_dxmt_from_csv(path: Path) -> dict[tuple[int, int], dict[str, Any]]:
    rows: dict[tuple[int, int], dict[str, Any]] = {}
    if not path.exists():
        raise SystemExit(f"missing dxmt encoder csv: {path}")
    with path.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        for raw in reader:
            parsed = {key: parse_value(value) for key, value in raw.items()}
            seq = parsed.get("seq")
            enc = parsed.get("encoder")
            if isinstance(seq, int) and isinstance(enc, int):
                rows[(seq, enc)] = parsed
    return rows


def load_dxmt_streams_from_csv(path: Path) -> dict[tuple[int, int], list[dict[str, Any]]]:
    rows: dict[tuple[int, int], list[dict[str, Any]]] = {}
    if not path.exists():
        raise SystemExit(f"missing dxmt stream csv: {path}")
    with path.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        for raw in reader:
            parsed = {key: parse_value(value) for key, value in raw.items()}
            seq = parsed.get("seq")
            enc = parsed.get("encoder")
            if isinstance(seq, int) and isinstance(enc, int):
                rows.setdefault((seq, enc), []).append(parsed)
    for stream_rows in rows.values():
        stream_rows.sort(key=lambda row: as_int(row.get("stream")))
    return rows


def load_dxmt(args: argparse.Namespace) -> dict[tuple[int, int], dict[str, Any]]:
    if args.dxmt_encoders_csv:
        return load_dxmt_from_csv(args.dxmt_encoders_csv)
    if args.dxmt_log:
        return load_dxmt_from_log(args.dxmt_log)
    return {}


def summarize_xcode(path: Path, run_label: str) -> list[dict[str, Any]]:
    header, data = read_csv_rows(path)
    needed_names = {
        "Index", "Encoder Index", "CommandBuffer Index", "CommandBuffer Label",
        "Encoder Label", "GPU Time", "Bytes Written To Device Memory",
        "Buffer Device Memory Bytes Written", "VS Bytes Written To Device Memory",
        "VS Buffer Device Memory Bytes Written", "FS Bytes Written To Device Memory",
        "FS Buffer Device Memory Bytes Written", "Texture Device Memory Bytes Written",
        "Depth Texture Device Memory Bytes Written", "Tiled Vertex Buffer Bytes",
        "Tiled Vertex Buffer Primitive Blocks Bytes", "Vertices", "VS Invocations",
        "Primitives", "Post Clipped Primitives", "Pixels Rasterized",
        "FS Invocations", "Varyings Per Fragment", "VS Occupancy", "FS Occupancy",
        "VS Buffer L1 Bytes Written", "VS Last Level Cache Bytes Written",
        "Vertex Stage Time", "VS Buffer Write Limiter", "VS Buffer Read Limiter",
        "VS ALU Limiter", "Buffer Write Limiter", "Last Level Cache Limiter",
        "MMU Limiter", "Shaded Vertex Read Limiter", "Cull Unit Limiter",
        "Clip Unit Limiter", "Primitive Block Tile Intersections",
        "Tiling Block Utilization", "Primitives Rendered",
        "Primitives Culled (Back-Face)", "Primitives Culled (Off-Screen)",
        "Primitives Clipped", "Fragments Rasterized per Primitive",
        "FS Tiles Processed", "Primitives Per Tile",
    }
    indexes = {
        (name, occurrence): header_index(header, name, occurrence)
        for name in needed_names
        for occurrence in (0, 1)
    }

    total_gpu_ns = sum(numeric(row, indexes, "GPU Time") for row in data)
    summaries: list[dict[str, Any]] = []
    for row in data:
        label = cell(row, indexes, "Encoder Label")
        match = RENDER_PASS_LABEL_RE.search(label)
        seq = int(match.group(1)) if match else ""
        enc = int(match.group(2)) if match else ""
        rt = match.group(3) if match else ""
        depth = match.group(4) if match else ""
        gpu_ns = numeric(row, indexes, "GPU Time")
        vs_buffer_bytes = numeric(row, indexes, "VS Buffer Device Memory Bytes Written")
        vs_invocations = numeric(row, indexes, "VS Invocations")
        fs_invocations = numeric(row, indexes, "FS Invocations")
        primitives = numeric(row, indexes, "Primitives")
        post_clipped_primitives = numeric(row, indexes, "Post Clipped Primitives")
        pixels_rasterized = numeric(row, indexes, "Pixels Rasterized")
        fs_tiles_processed = numeric(row, indexes, "FS Tiles Processed")
        primitives_per_tile = numeric(row, indexes, "Primitives Per Tile")
        primitive_tile_estimate = fs_tiles_processed * primitives_per_tile
        tiled_buffer_bytes = (
            numeric(row, indexes, "Tiled Vertex Buffer Bytes") +
            numeric(row, indexes, "Tiled Vertex Buffer Primitive Blocks Bytes")
        )
        vs_l1_write_bytes = numeric(row, indexes, "VS Buffer L1 Bytes Written")
        vs_llc_write_bytes = numeric(row, indexes, "VS Last Level Cache Bytes Written")
        summaries.append({
            "run": run_label,
            "seq": seq,
            "enc": enc,
            "xcode_index": parse_value(cell(row, indexes, "Index")),
            "encoder_index": parse_value(cell(row, indexes, "Encoder Index")),
            "command_buffer_index": parse_value(cell(row, indexes, "CommandBuffer Index")),
            "command_buffer_label": cell(row, indexes, "CommandBuffer Label"),
            "gpu_share_pct": (gpu_ns / total_gpu_ns * 100.0) if total_gpu_ns else 0.0,
            "gpu_ms": gpu_ns / 1_000_000.0,
            "rt": rt,
            "depth": depth,
            "device_write_mib": mib(numeric(row, indexes, "Bytes Written To Device Memory")),
            "buffer_write_mib": mib(numeric(row, indexes, "Buffer Device Memory Bytes Written")),
            "vs_write_mib": mib(numeric(row, indexes, "VS Bytes Written To Device Memory")),
            "vs_buffer_write_mib": mib(vs_buffer_bytes),
            "fs_write_mib": mib(numeric(row, indexes, "FS Bytes Written To Device Memory")),
            "fs_buffer_write_mib": mib(numeric(row, indexes, "FS Buffer Device Memory Bytes Written")),
            "texture_write_mib": mib(numeric(row, indexes, "Texture Device Memory Bytes Written")),
            "depth_write_mib": mib(numeric(row, indexes, "Depth Texture Device Memory Bytes Written")),
            "tiled_vertex_buffer_mib": mib(numeric(row, indexes, "Tiled Vertex Buffer Bytes")),
            "tiled_primitive_block_mib": mib(
                numeric(row, indexes, "Tiled Vertex Buffer Primitive Blocks Bytes")),
            "vertices": parse_value(cell(row, indexes, "Vertices")),
            "vs_invocations": parse_value(cell(row, indexes, "VS Invocations")),
            "vs_buffer_bytes_per_vs_invocation":
                (vs_buffer_bytes / vs_invocations) if vs_invocations else 0.0,
            "vs_buffer_bytes_per_fragment":
                (vs_buffer_bytes / fs_invocations) if fs_invocations else 0.0,
            "vs_buffer_bytes_per_primitive":
                (vs_buffer_bytes / primitives) if primitives else 0.0,
            "vs_buffer_bytes_per_post_clipped_primitive":
                (vs_buffer_bytes / post_clipped_primitives) if post_clipped_primitives else 0.0,
            "vs_buffer_bytes_per_primitive_tile_estimate":
                (vs_buffer_bytes / primitive_tile_estimate) if primitive_tile_estimate else 0.0,
            "vs_buffer_bytes_per_pixel":
                (vs_buffer_bytes / pixels_rasterized) if pixels_rasterized else 0.0,
            "vs_buffer_to_tiled_buffer_ratio":
                (vs_buffer_bytes / tiled_buffer_bytes) if tiled_buffer_bytes else 0.0,
            "vs_l1_write_mib": mib(vs_l1_write_bytes),
            "vs_llc_write_mib": mib(vs_llc_write_bytes),
            "vs_l1_to_device_write_ratio":
                (vs_l1_write_bytes / vs_buffer_bytes) if vs_buffer_bytes else 0.0,
            "vs_llc_to_device_write_ratio":
                (vs_llc_write_bytes / vs_buffer_bytes) if vs_buffer_bytes else 0.0,
            "vertex_stage_time_pct": parse_value(cell(row, indexes, "Vertex Stage Time")),
            "vs_buffer_write_limiter_pct": parse_value(cell(row, indexes, "VS Buffer Write Limiter")),
            "vs_buffer_read_limiter_pct": parse_value(cell(row, indexes, "VS Buffer Read Limiter")),
            "vs_alu_limiter_pct": parse_value(cell(row, indexes, "VS ALU Limiter")),
            "shaded_vertex_read_limiter_pct": parse_value(cell(row, indexes, "Shaded Vertex Read Limiter")),
            "cull_unit_limiter_pct": parse_value(cell(row, indexes, "Cull Unit Limiter")),
            "clip_unit_limiter_pct": parse_value(cell(row, indexes, "Clip Unit Limiter")),
            "primitives": parse_value(cell(row, indexes, "Primitives")),
            "post_clipped_primitives": parse_value(cell(row, indexes, "Post Clipped Primitives")),
            "primitive_block_tile_intersections_pct":
                parse_value(cell(row, indexes, "Primitive Block Tile Intersections")),
            "tiling_block_utilization_pct":
                parse_value(cell(row, indexes, "Tiling Block Utilization")),
            "primitives_rendered_pct": parse_value(cell(row, indexes, "Primitives Rendered")),
            "primitives_culled_backface_pct":
                parse_value(cell(row, indexes, "Primitives Culled (Back-Face)")),
            "primitives_culled_offscreen_pct":
                parse_value(cell(row, indexes, "Primitives Culled (Off-Screen)")),
            "primitives_clipped_pct": parse_value(cell(row, indexes, "Primitives Clipped")),
            "fragments_per_primitive":
                parse_value(cell(row, indexes, "Fragments Rasterized per Primitive")),
            "fs_tiles_processed": parse_value(cell(row, indexes, "FS Tiles Processed")),
            "primitives_per_tile": parse_value(cell(row, indexes, "Primitives Per Tile")),
            "pixels_rasterized": parse_value(cell(row, indexes, "Pixels Rasterized")),
            "fs_invocations": parse_value(cell(row, indexes, "FS Invocations")),
            "varyings_per_fragment": parse_value(cell(row, indexes, "Varyings Per Fragment")),
            "vs_occupancy_pct": parse_value(cell(row, indexes, "VS Occupancy")),
            "fs_occupancy_pct": parse_value(cell(row, indexes, "FS Occupancy")),
            "buffer_write_limiter_pct": parse_value(cell(row, indexes, "Buffer Write Limiter")),
            "llc_limiter_pct": parse_value(cell(row, indexes, "Last Level Cache Limiter")),
            "mmu_limiter_pct": parse_value(cell(row, indexes, "MMU Limiter")),
        })
    return summaries


def join_dxmt(row: dict[str, Any], dxmt: dict[tuple[int, int], dict[str, Any]]) -> dict[str, Any]:
    joined = dict(row)
    key = (row.get("seq"), row.get("enc"))
    source = dxmt.get(key) if isinstance(key[0], int) and isinstance(key[1], int) else None
    if source:
        joined["rt"] = source.get("rt", "")
        joined["depth"] = source.get("depth", "")
    mapping = {
        "dxmt_rt_format": "rt_format",
        "dxmt_rt_width": "rt_width",
        "dxmt_rt_height": "rt_height",
        "dxmt_rt_bpp": "rt_bpp",
        "dxmt_rt_alias_texture": "rt_alias_texture",
        "dxmt_rt_texture_usage": "rt_texture_usage",
        "dxmt_rt_format_swizzle": "rt_format_swizzle",
        "dxmt_rt_texture_needs_shader_read_view": "rt_texture_needs_shader_read_view",
        "dxmt_depth_format": "depth_format",
        "dxmt_depth_width": "depth_width",
        "dxmt_depth_height": "depth_height",
        "dxmt_depth_bpp": "depth_bpp",
        "dxmt_depth_alias_texture": "depth_alias_texture",
        "dxmt_depth_texture_usage": "depth_texture_usage",
        "dxmt_depth_format_swizzle": "depth_format_swizzle",
        "dxmt_depth_texture_needs_shader_read_view": "depth_texture_needs_shader_read_view",
        "dxmt_draw_calls": "draw_calls",
        "dxmt_indexed_draws": "indexed_draws",
        "dxmt_expanded_indexed_draws": "expanded_indexed_draws",
        "dxmt_ffp_draws": "ffp_draws",
        "dxmt_programmable_draws": "programmable_draws",
        "dxmt_pretransformed_draws": "pretransformed_draws",
        "dxmt_textured_draws": "textured_draws",
        "dxmt_cull_none_draws": "cull_none_draws",
        "dxmt_cull_front_draws": "cull_front_draws",
        "dxmt_cull_back_draws": "cull_back_draws",
        "dxmt_fill_solid_draws": "fill_solid_draws",
        "dxmt_fill_wireframe_draws": "fill_wireframe_draws",
        "dxmt_depth_enabled_draws": "depth_enabled_draws",
        "dxmt_depth_write_draws": "depth_write_draws",
        "dxmt_depth_func_less_draws": "depth_func_less_draws",
        "dxmt_depth_func_lessequal_draws": "depth_func_lessequal_draws",
        "dxmt_depth_func_always_draws": "depth_func_always_draws",
        "dxmt_depth_func_other_draws": "depth_func_other_draws",
        "dxmt_scissor_enabled_draws": "scissor_enabled_draws",
        "dxmt_alpha_blend_enabled_draws": "alpha_blend_enabled_draws",
        "dxmt_blend_state_samples": "blend_state_samples",
        "dxmt_blend_state_changes": "blend_state_changes",
        "dxmt_blend_state_unique": "blend_state_unique",
        "dxmt_blend_state_last": "blend_state_last",
        "dxmt_blend_enabled_noop_draws": "blend_enabled_noop_draws",
        "dxmt_blend_constant_factor_draws": "blend_constant_factor_draws",
        "dxmt_alpha_test_enabled_draws": "alpha_test_enabled_draws",
        "dxmt_alpha_test_effective_draws": "alpha_test_effective_draws",
        "dxmt_clip_plane_enabled_draws": "clip_plane_enabled_draws",
        "dxmt_primitive_count": "primitive_count",
        "dxmt_triangle_estimate": "triangle_estimate",
        "dxmt_vertex_count": "vertex_count",
        "dxmt_draw_primitive_min": "draw_primitive_min",
        "dxmt_draw_primitive_max": "draw_primitive_max",
        "dxmt_draw_vertex_min": "draw_vertex_min",
        "dxmt_draw_vertex_max": "draw_vertex_max",
        "dxmt_draw_primitive_bucket_1_63": "draw_primitive_bucket_1_63",
        "dxmt_draw_primitive_bucket_64_255": "draw_primitive_bucket_64_255",
        "dxmt_draw_primitive_bucket_256_1023": "draw_primitive_bucket_256_1023",
        "dxmt_draw_primitive_bucket_1024_4095": "draw_primitive_bucket_1024_4095",
        "dxmt_draw_primitive_bucket_4096_plus": "draw_primitive_bucket_4096_plus",
        "dxmt_draw_vertex_bucket_1_255": "draw_vertex_bucket_1_255",
        "dxmt_draw_vertex_bucket_256_1023": "draw_vertex_bucket_256_1023",
        "dxmt_draw_vertex_bucket_1024_4095": "draw_vertex_bucket_1024_4095",
        "dxmt_draw_vertex_bucket_4096_16383": "draw_vertex_bucket_4096_16383",
        "dxmt_draw_vertex_bucket_16384_plus": "draw_vertex_bucket_16384_plus",
        "dxmt_draw_geometry_signature_samples": "draw_geometry_signature_samples",
        "dxmt_draw_geometry_signature_unique": "draw_geometry_signature_unique",
        "dxmt_draw_geometry_signature_unique_overflows": "draw_geometry_signature_unique_overflows",
        "dxmt_draw_geometry_signature_duplicates": "draw_geometry_signature_duplicates",
        "dxmt_draw_geometry_signature_consecutive_duplicates": "draw_geometry_signature_consecutive_duplicates",
        "dxmt_draw_geometry_signature_last": "draw_geometry_signature_last",
        "dxmt_indexed_triangle_opaque_depth_write_draws": "indexed_triangle_opaque_depth_write_draws",
        "dxmt_indexed_triangle_opaque_depth_write_primitives": "indexed_triangle_opaque_depth_write_primitives",
        "dxmt_indexed_triangle_opaque_depth_write_vertices": "indexed_triangle_opaque_depth_write_vertices",
        "dxmt_indexed_triangle_depth_read_draws": "indexed_triangle_depth_read_draws",
        "dxmt_indexed_triangle_depth_read_primitives": "indexed_triangle_depth_read_primitives",
        "dxmt_indexed_triangle_depth_read_vertices": "indexed_triangle_depth_read_vertices",
        "dxmt_indexed_triangle_alpha_blend_draws": "indexed_triangle_alpha_blend_draws",
        "dxmt_indexed_triangle_alpha_blend_primitives": "indexed_triangle_alpha_blend_primitives",
        "dxmt_indexed_triangle_alpha_blend_vertices": "indexed_triangle_alpha_blend_vertices",
        "dxmt_indexed_triangle_scissor_draws": "indexed_triangle_scissor_draws",
        "dxmt_indexed_triangle_scissor_primitives": "indexed_triangle_scissor_primitives",
        "dxmt_indexed_triangle_scissor_vertices": "indexed_triangle_scissor_vertices",
        "dxmt_indexed_triangle_textured_draws": "indexed_triangle_textured_draws",
        "dxmt_indexed_triangle_textured_primitives": "indexed_triangle_textured_primitives",
        "dxmt_indexed_triangle_textured_vertices": "indexed_triangle_textured_vertices",
        "dxmt_indexed_triangle_large_4096_draws": "indexed_triangle_large_4096_draws",
        "dxmt_indexed_triangle_large_4096_primitives": "indexed_triangle_large_4096_primitives",
        "dxmt_indexed_triangle_large_4096_vertices": "indexed_triangle_large_4096_vertices",
        "dxmt_indexed_triangle_large_4096_opaque_depth_write_draws": "indexed_triangle_large_4096_opaque_depth_write_draws",
        "dxmt_indexed_triangle_large_4096_opaque_depth_write_primitives": "indexed_triangle_large_4096_opaque_depth_write_primitives",
        "dxmt_indexed_triangle_large_4096_opaque_depth_write_vertices": "indexed_triangle_large_4096_opaque_depth_write_vertices",
        "dxmt_indexed_triangle_large_4096_depth_read_draws": "indexed_triangle_large_4096_depth_read_draws",
        "dxmt_indexed_triangle_large_4096_depth_read_primitives": "indexed_triangle_large_4096_depth_read_primitives",
        "dxmt_indexed_triangle_large_4096_depth_read_vertices": "indexed_triangle_large_4096_depth_read_vertices",
        "dxmt_indexed_triangle_large_4096_alpha_blend_draws": "indexed_triangle_large_4096_alpha_blend_draws",
        "dxmt_indexed_triangle_large_4096_alpha_blend_primitives": "indexed_triangle_large_4096_alpha_blend_primitives",
        "dxmt_indexed_triangle_large_4096_alpha_blend_vertices": "indexed_triangle_large_4096_alpha_blend_vertices",
        "dxmt_indexed_triangle_large_4096_scissor_draws": "indexed_triangle_large_4096_scissor_draws",
        "dxmt_indexed_triangle_large_4096_scissor_primitives": "indexed_triangle_large_4096_scissor_primitives",
        "dxmt_indexed_triangle_large_4096_scissor_vertices": "indexed_triangle_large_4096_scissor_vertices",
        "dxmt_indexed_triangle_large_4096_textured_draws": "indexed_triangle_large_4096_textured_draws",
        "dxmt_indexed_triangle_large_4096_textured_primitives": "indexed_triangle_large_4096_textured_primitives",
        "dxmt_indexed_triangle_large_4096_textured_vertices": "indexed_triangle_large_4096_textured_vertices",
        "dxmt_indexed_base_vertex_samples": "indexed_base_vertex_samples",
        "dxmt_indexed_base_vertex_nonzero_draws": "indexed_base_vertex_nonzero_draws",
        "dxmt_indexed_base_vertex_negative_draws": "indexed_base_vertex_negative_draws",
        "dxmt_indexed_base_vertex_positive_draws": "indexed_base_vertex_positive_draws",
        "dxmt_indexed_base_vertex_min": "indexed_base_vertex_min",
        "dxmt_indexed_base_vertex_max": "indexed_base_vertex_max",
        "dxmt_native_base_vertex_requested_draws": "native_base_vertex_requested_draws",
        "dxmt_native_base_vertex_used_draws": "native_base_vertex_used_draws",
        "dxmt_native_base_vertex_skipped_negative_draws": "native_base_vertex_skipped_negative_draws",
        "dxmt_split_large_indexed_source_draws": "split_large_indexed_source_draws",
        "dxmt_split_large_indexed_metal_draws": "split_large_indexed_metal_draws",
        "dxmt_split_large_indexed_extra_draws": "split_large_indexed_extra_draws",
        "dxmt_split_large_indexed_primitive_limit": "split_large_indexed_primitive_limit",
        "dxmt_split_large_indexed_primitive_count": "split_large_indexed_primitive_count",
        "dxmt_indexed_order_probe_draws": "indexed_order_probe_draws",
        "dxmt_indexed_order_probe_skipped": "indexed_order_probe_skipped",
        "dxmt_indexed_order_probe_bytes": "indexed_order_probe_bytes",
        "dxmt_indexed_vertex_reuse_samples": "indexed_vertex_reuse_samples",
        "dxmt_indexed_vertex_reuse_skipped": "indexed_vertex_reuse_skipped",
        "dxmt_indexed_vertex_reference_count": "indexed_vertex_reference_count",
        "dxmt_indexed_unique_vertex_estimate": "indexed_unique_vertex_estimate",
        "dxmt_indexed_vertex_cache_miss_estimate_16": "indexed_vertex_cache_miss_estimate_16",
        "dxmt_indexed_vertex_cache_miss_estimate_32": "indexed_vertex_cache_miss_estimate_32",
        "dxmt_indexed_vertex_cache_miss_estimate_64": "indexed_vertex_cache_miss_estimate_64",
        "dxmt_indexed_cache_opt_candidate_draws": "indexed_cache_opt_candidate_draws",
        "dxmt_indexed_cache_opt_candidate_skipped": "indexed_cache_opt_candidate_skipped",
        "dxmt_indexed_cache_opt_candidate_bytes": "indexed_cache_opt_candidate_bytes",
        "dxmt_indexed_cache_opt_candidate_original_miss16": "indexed_cache_opt_candidate_original_miss16",
        "dxmt_indexed_cache_opt_candidate_original_miss32": "indexed_cache_opt_candidate_original_miss32",
        "dxmt_indexed_cache_opt_candidate_original_miss64": "indexed_cache_opt_candidate_original_miss64",
        "dxmt_indexed_cache_opt_candidate_miss16": "indexed_cache_opt_candidate_miss16",
        "dxmt_indexed_cache_opt_candidate_miss32": "indexed_cache_opt_candidate_miss32",
        "dxmt_indexed_cache_opt_candidate_miss64": "indexed_cache_opt_candidate_miss64",
        "dxmt_reordered_index_cache_lookups": "reordered_index_cache_lookups",
        "dxmt_reordered_index_cache_hits": "reordered_index_cache_hits",
        "dxmt_reordered_index_cache_misses": "reordered_index_cache_misses",
        "dxmt_reordered_index_cache_created": "reordered_index_cache_created",
        "dxmt_reordered_index_cache_created_bytes": "reordered_index_cache_created_bytes",
        "dxmt_texture_mask_or": "texture_mask_or",
        "dxmt_fragment_texture_binding_samples": "fragment_texture_binding_samples",
        "dxmt_fragment_texture_binding_mask_or": "fragment_texture_binding_mask_or",
        "dxmt_x8_rt_texture_binding_samples": "x8_rt_texture_binding_samples",
        "dxmt_x8_rt_texture_binding_mask_or": "x8_rt_texture_binding_mask_or",
        "dxmt_x8_rt_texture_binding_unique_handles": "x8_rt_texture_binding_unique_handles",
        "dxmt_x8_rt_texture_binding_unique_handle_overflows": "x8_rt_texture_binding_unique_handle_overflows",
        "dxmt_x8_rt_texture_binding_shader_read_view_samples": "x8_rt_texture_binding_shader_read_view_samples",
        "dxmt_x8_rt_texture_binding_active_rt_alias_samples": "x8_rt_texture_binding_active_rt_alias_samples",
        "dxmt_x8_shader_alpha_fill_samples": "x8_shader_alpha_fill_samples",
        "dxmt_x8_shader_alpha_fill_mask_or": "x8_shader_alpha_fill_mask_or",
        "dxmt_x8_rt_texture_binding_last_stage": "x8_rt_texture_binding_last_stage",
        "dxmt_x8_rt_texture_binding_last_handle": "x8_rt_texture_binding_last_handle",
        "dxmt_stream0_stride_min": "stream0_stride_min",
        "dxmt_stream0_stride_max": "stream0_stride_max",
        "dxmt_stream_state_samples": "stream_state_samples",
        "dxmt_stream_metal_binds": "stream_metal_binds",
        "dxmt_stream_metal_bind_firsts": "stream_metal_bind_firsts",
        "dxmt_stream_metal_bind_handle_changes": "stream_metal_bind_handle_changes",
        "dxmt_stream_metal_bind_offset_changes": "stream_metal_bind_offset_changes",
        "dxmt_stream_unique_handles": "stream_unique_handles",
        "dxmt_stream_unique_bytes": "stream_unique_bytes",
        "dxmt_stream_unique_dynamic_handles": "stream_unique_dynamic_handles",
        "dxmt_stream_unique_writeonly_handles": "stream_unique_writeonly_handles",
        "dxmt_stream_unique_default_pool_handles": "stream_unique_default_pool_handles",
        "dxmt_stream_unique_managed_pool_handles": "stream_unique_managed_pool_handles",
        "dxmt_stream_unique_systemmem_pool_handles": "stream_unique_systemmem_pool_handles",
        "dxmt_stream_unique_scratch_pool_handles": "stream_unique_scratch_pool_handles",
        "dxmt_stream_handle_changes": "stream_handle_changes",
        "dxmt_stream_offset_changes": "stream_offset_changes",
        "dxmt_stream_stride_changes": "stream_stride_changes",
        "dxmt_stream0_last_handle": "stream0_last_handle",
        "dxmt_stream0_last_offset": "stream0_last_offset",
        "dxmt_stream0_last_stride": "stream0_last_stride",
        "dxmt_ib_state_samples": "ib_state_samples",
        "dxmt_ib_metal_binds": "ib_metal_binds",
        "dxmt_ib_handle_changes": "ib_handle_changes",
        "dxmt_ib_unique_handles": "ib_unique_handles",
        "dxmt_ib_unique_bytes": "ib_unique_bytes",
        "dxmt_ib_unique_dynamic_handles": "ib_unique_dynamic_handles",
        "dxmt_ib_unique_writeonly_handles": "ib_unique_writeonly_handles",
        "dxmt_ib_unique_default_pool_handles": "ib_unique_default_pool_handles",
        "dxmt_ib_unique_managed_pool_handles": "ib_unique_managed_pool_handles",
        "dxmt_ib_unique_systemmem_pool_handles": "ib_unique_systemmem_pool_handles",
        "dxmt_ib_unique_scratch_pool_handles": "ib_unique_scratch_pool_handles",
        "dxmt_ib_last_handle": "ib_last_handle",
        "dxmt_pso_state_samples": "pso_state_samples",
        "dxmt_pso_handle_changes": "pso_handle_changes",
        "dxmt_pso_unique_handles": "pso_unique_handles",
        "dxmt_pso_last_handle": "pso_last_handle",
        "dxmt_shader_variant_changes": "shader_variant_changes",
        "dxmt_shader_variant_unique": "shader_variant_unique",
        "dxmt_shader_variant_last": "shader_variant_last",
        "dxmt_vertex_shader_last": "vertex_shader_last",
        "dxmt_pixel_shader_last": "pixel_shader_last",
        "dxmt_vertex_shader_source_last": "vertex_shader_source_last",
        "dxmt_pixel_shader_source_last": "pixel_shader_source_last",
        "dxmt_vsout_layout_changes": "vsout_layout_changes",
        "dxmt_vsout_layout_unique": "vsout_layout_unique",
        "dxmt_vsout_layout_last": "vsout_layout_last",
        "dxmt_vsout_layout_cache_hits": "vsout_layout_cache_hits",
        "dxmt_vsout_layout_cache_misses": "vsout_layout_cache_misses",
        "dxmt_argbuf_table_bytes": "argbuf_table_bytes",
        "dxmt_argbuf_cbuf_bytes": "argbuf_cbuf_bytes",
        "dxmt_argbuf_cbuf_vs_bytes": "argbuf_cbuf_vs_bytes",
        "dxmt_argbuf_cbuf_ffp_vs_bytes": "argbuf_cbuf_ffp_vs_bytes",
        "dxmt_argbuf_cbuf_ps_bytes": "argbuf_cbuf_ps_bytes",
        "dxmt_argbuf_cbuf_ffp_ps_bytes": "argbuf_cbuf_ffp_ps_bytes",
        "dxmt_argbuf_cbuf_vs_first_bytes": "argbuf_cbuf_vs_first_bytes",
        "dxmt_argbuf_cbuf_vs_rewrite_changed_bytes": "argbuf_cbuf_vs_rewrite_changed_bytes",
        "dxmt_argbuf_cbuf_vs_rewrite_unchanged_bytes": "argbuf_cbuf_vs_rewrite_unchanged_bytes",
        "dxmt_argbuf_cbuf_ffp_vs_first_bytes": "argbuf_cbuf_ffp_vs_first_bytes",
        "dxmt_argbuf_cbuf_ffp_vs_rewrite_changed_bytes": "argbuf_cbuf_ffp_vs_rewrite_changed_bytes",
        "dxmt_argbuf_cbuf_ffp_vs_rewrite_unchanged_bytes": "argbuf_cbuf_ffp_vs_rewrite_unchanged_bytes",
        "dxmt_set_vertex_bytes_calls": "set_vertex_bytes_calls",
        "dxmt_set_vertex_bytes_bytes": "set_vertex_bytes_bytes",
        "dxmt_set_vertex_bytes_slot5_calls": "set_vertex_bytes_slot5_calls",
        "dxmt_set_vertex_bytes_slot5_bytes": "set_vertex_bytes_slot5_bytes",
        "dxmt_set_vertex_bytes_other_calls": "set_vertex_bytes_other_calls",
        "dxmt_set_vertex_bytes_other_bytes": "set_vertex_bytes_other_bytes",
        "dxmt_transient_vertex_bytes": "transient_vertex_bytes",
        "dxmt_transient_vertex_user_bytes": "transient_vertex_user_bytes",
        "dxmt_transient_vertex_preupload_bytes": "transient_vertex_preupload_bytes",
        "dxmt_transient_vertex_decl_fallback_bytes": "transient_vertex_decl_fallback_bytes",
        "dxmt_transient_vertex_expanded_main_bytes": "transient_vertex_expanded_main_bytes",
        "dxmt_transient_vertex_expanded_extra_bytes": "transient_vertex_expanded_extra_bytes",
        "dxmt_transient_index_bytes": "transient_index_bytes",
        "dxmt_transient_index_user_bytes": "transient_index_user_bytes",
        "dxmt_transient_index_preupload_bytes": "transient_index_preupload_bytes",
        "dxmt_transient_index_shadow_fallback_bytes": "transient_index_shadow_fallback_bytes",
        "dxmt_transient_index_probe_reorder_bytes": "transient_index_probe_reorder_bytes",
    }
    for output_key, input_key in mapping.items():
        joined[output_key] = source.get(input_key, "") if source else ""
    if source:
        derive_dxmt_attribution(joined)
    return joined


def ratio(numerator: float, denominator: float) -> float:
    return numerator / denominator if denominator else 0.0


def derive_dxmt_attribution(joined: dict[str, Any]) -> None:
    argbuf_table_bytes = as_int(joined.get("dxmt_argbuf_table_bytes"))
    argbuf_cbuf_bytes = as_int(joined.get("dxmt_argbuf_cbuf_bytes"))
    set_vertex_bytes = as_int(joined.get("dxmt_set_vertex_bytes_bytes"))
    transient_vertex_bytes = as_int(joined.get("dxmt_transient_vertex_bytes"))
    transient_index_bytes = as_int(joined.get("dxmt_transient_index_bytes"))
    geometry_transient_bytes = transient_vertex_bytes + transient_index_bytes
    cpu_writer_bytes = (
        argbuf_table_bytes +
        argbuf_cbuf_bytes +
        set_vertex_bytes +
        geometry_transient_bytes
    )
    buffer_write_bytes = as_float(joined.get("buffer_write_mib")) * 1024.0 * 1024.0
    vs_buffer_write_bytes = as_float(joined.get("vs_buffer_write_mib")) * 1024.0 * 1024.0
    draw_calls = as_int(joined.get("dxmt_draw_calls"))
    signature_samples = as_int(joined.get("dxmt_draw_geometry_signature_samples"))
    signature_duplicates = as_int(joined.get("dxmt_draw_geometry_signature_duplicates"))
    consecutive_signature_duplicates = as_int(
        joined.get("dxmt_draw_geometry_signature_consecutive_duplicates"))
    dxmt_vertices = as_int(joined.get("dxmt_vertex_count"))
    dxmt_primitives = as_int(joined.get("dxmt_primitive_count"))
    vs_invocations = as_float(joined.get("vs_invocations"))
    indexed_vertex_refs = as_int(joined.get("dxmt_indexed_vertex_reference_count"))
    indexed_unique_vertices = as_int(joined.get("dxmt_indexed_unique_vertex_estimate"))
    indexed_cache_misses = {
        16: as_int(joined.get("dxmt_indexed_vertex_cache_miss_estimate_16")),
        32: as_int(joined.get("dxmt_indexed_vertex_cache_miss_estimate_32")),
        64: as_int(joined.get("dxmt_indexed_vertex_cache_miss_estimate_64")),
    }
    stream0_stride_min = as_int(joined.get("dxmt_stream0_stride_min"))
    stream0_stride_max = as_int(joined.get("dxmt_stream0_stride_max"))
    pso_state_samples = as_int(joined.get("dxmt_pso_state_samples"))
    stream_handle_changes = as_int(joined.get("dxmt_stream_handle_changes"))
    stream_offset_changes = as_int(joined.get("dxmt_stream_offset_changes"))
    stream_stride_changes = as_int(joined.get("dxmt_stream_stride_changes"))
    ib_handle_changes = as_int(joined.get("dxmt_ib_handle_changes"))

    joined["dxmt_geometry_transient_bytes"] = geometry_transient_bytes
    joined["dxmt_pso_state_samples_per_draw"] = ratio(
        float(pso_state_samples), float(draw_calls))
    joined["dxmt_draw_geometry_signature_duplicate_ratio"] = ratio(
        float(signature_duplicates), float(signature_samples))
    joined["dxmt_draw_geometry_signature_consecutive_duplicate_ratio"] = ratio(
        float(consecutive_signature_duplicates), float(signature_samples))
    joined["dxmt_cpu_writer_bytes"] = cpu_writer_bytes
    joined["dxmt_cpu_writer_mib"] = cpu_writer_bytes / (1024.0 * 1024.0)
    joined["dxmt_cpu_writer_to_buffer_write_ratio"] = ratio(
        float(cpu_writer_bytes), buffer_write_bytes)
    unexplained_buffer_write_bytes = max(buffer_write_bytes - float(cpu_writer_bytes), 0.0)
    joined["dxmt_vs_buffer_write_share"] = ratio(
        vs_buffer_write_bytes, buffer_write_bytes)
    joined["dxmt_unexplained_buffer_write_mib"] = (
        unexplained_buffer_write_bytes / (1024.0 * 1024.0))
    joined["dxmt_unexplained_buffer_write_ratio"] = ratio(
        unexplained_buffer_write_bytes, buffer_write_bytes)
    joined["dxmt_argbuf_cbuf_to_buffer_write_ratio"] = ratio(
        float(argbuf_cbuf_bytes), buffer_write_bytes)
    joined["dxmt_transient_to_buffer_write_ratio"] = ratio(
        float(geometry_transient_bytes), buffer_write_bytes)
    joined["dxmt_stream_handle_changes_per_draw"] = ratio(
        float(stream_handle_changes), float(draw_calls))
    joined["dxmt_stream_offset_changes_per_draw"] = ratio(
        float(stream_offset_changes), float(draw_calls))
    joined["dxmt_stream_stride_changes_per_draw"] = ratio(
        float(stream_stride_changes), float(draw_calls))
    joined["dxmt_ib_handle_changes_per_draw"] = ratio(
        float(ib_handle_changes), float(draw_calls))
    large_primitive_draws = (
        as_int(joined.get("dxmt_draw_primitive_bucket_1024_4095")) +
        as_int(joined.get("dxmt_draw_primitive_bucket_4096_plus"))
    )
    large_vertex_draws = (
        as_int(joined.get("dxmt_draw_vertex_bucket_4096_16383")) +
        as_int(joined.get("dxmt_draw_vertex_bucket_16384_plus"))
    )
    joined["dxmt_primitives_per_draw"] = ratio(
        float(dxmt_primitives), float(draw_calls))
    joined["dxmt_vertices_per_draw"] = ratio(
        float(dxmt_vertices), float(draw_calls))
    joined["dxmt_large_primitive_draw_share"] = ratio(
        float(large_primitive_draws), float(draw_calls))
    joined["dxmt_large_vertex_draw_share"] = ratio(
        float(large_vertex_draws), float(draw_calls))
    joined["dxmt_vs_buffer_bytes_per_dxmt_vertex"] = ratio(
        vs_buffer_write_bytes, float(dxmt_vertices))
    joined["dxmt_vs_invocations_per_dxmt_vertex"] = ratio(
        vs_invocations, float(dxmt_vertices))
    joined["dxmt_indexed_vertex_reuse_ratio"] = ratio(
        float(indexed_vertex_refs), float(indexed_unique_vertices))
    joined["dxmt_vs_invocations_per_indexed_unique_vertex"] = ratio(
        vs_invocations, float(indexed_unique_vertices))
    joined["dxmt_vs_buffer_bytes_per_indexed_unique_vertex"] = ratio(
        vs_buffer_write_bytes, float(indexed_unique_vertices))
    for cache_size, misses in indexed_cache_misses.items():
        joined[f"dxmt_indexed_vertex_cache_miss_over_unique_{cache_size}"] = ratio(
            float(misses), float(indexed_unique_vertices))
        joined[f"dxmt_vs_invocations_per_indexed_cache_miss_{cache_size}"] = ratio(
            vs_invocations, float(misses))
        joined[f"dxmt_vs_buffer_bytes_per_indexed_cache_miss_{cache_size}"] = ratio(
            vs_buffer_write_bytes, float(misses))
    stream0_input_min_bytes = dxmt_vertices * stream0_stride_min
    stream0_input_max_bytes = dxmt_vertices * stream0_stride_max
    joined["dxmt_stream0_input_min_mib"] = (
        stream0_input_min_bytes / (1024.0 * 1024.0))
    joined["dxmt_stream0_input_max_mib"] = (
        stream0_input_max_bytes / (1024.0 * 1024.0))
    joined["dxmt_vs_buffer_to_stream0_input_min_ratio"] = ratio(
        vs_buffer_write_bytes, float(stream0_input_min_bytes))
    joined["dxmt_vs_buffer_to_stream0_input_max_ratio"] = ratio(
        vs_buffer_write_bytes, float(stream0_input_max_bytes))

    layout_text = str(joined.get("dxmt_vsout_layout_last", "")).strip()
    if layout_text:
        layout_key = as_int(layout_text)
        texcoord_mask = layout_key & 0xff
        texcoord_count = int(texcoord_mask.bit_count())
        has_color = 1 if (layout_key & (1 << 8)) else 0
        has_secondary = 1 if (layout_key & (1 << 9)) else 0
        has_fog = 1 if (layout_key & (1 << 10)) else 0
        has_point_size = 1 if (layout_key & (1 << 11)) else 0
        expected_stage_out_bytes = (
            16 +
            16 * has_color +
            16 * has_secondary +
            16 * texcoord_count +
            4 * has_fog +
            4 * has_point_size
        )
        joined["dxmt_vsout_texcoord_mask"] = f"0x{texcoord_mask:02x}"
        joined["dxmt_vsout_texcoord_count"] = texcoord_count
        joined["dxmt_vsout_has_color"] = has_color
        joined["dxmt_vsout_has_secondary_color"] = has_secondary
        joined["dxmt_vsout_has_fog_factor"] = has_fog
        joined["dxmt_vsout_has_point_size"] = has_point_size
        joined["dxmt_vsout_expected_stage_out_bytes_per_vertex"] = expected_stage_out_bytes
        joined["dxmt_vs_buffer_to_expected_stage_out_ratio"] = ratio(
            as_float(joined.get("vs_buffer_bytes_per_vs_invocation")),
            float(expected_stage_out_bytes))
    else:
        joined["dxmt_vsout_texcoord_mask"] = ""
        joined["dxmt_vsout_texcoord_count"] = ""
        joined["dxmt_vsout_has_color"] = ""
        joined["dxmt_vsout_has_secondary_color"] = ""
        joined["dxmt_vsout_has_fog_factor"] = ""
        joined["dxmt_vsout_has_point_size"] = ""
        joined["dxmt_vsout_expected_stage_out_bytes_per_vertex"] = ""
        joined["dxmt_vs_buffer_to_expected_stage_out_ratio"] = ""

    cpu_writer_ratio = as_float(joined.get("dxmt_cpu_writer_to_buffer_write_ratio"))
    cbuf_ratio = as_float(joined.get("dxmt_argbuf_cbuf_to_buffer_write_ratio"))
    transient_ratio = as_float(joined.get("dxmt_transient_to_buffer_write_ratio"))
    vs_buffer_ratio = ratio(vs_buffer_write_bytes, buffer_write_bytes)
    vsout_ratio = as_float(joined.get("dxmt_vs_buffer_to_expected_stage_out_ratio"))
    vs_bytes_per_primitive = as_float(joined.get("vs_buffer_bytes_per_primitive"))
    named_tiled_buffer_mib = (
        as_float(joined.get("tiled_vertex_buffer_mib")) +
        as_float(joined.get("tiled_primitive_block_mib"))
    )
    named_tiled_buffer_bytes = named_tiled_buffer_mib * 1024.0 * 1024.0
    hidden_backend_write_bytes = max(
        vs_buffer_write_bytes - named_tiled_buffer_bytes - float(cpu_writer_bytes),
        0.0)
    hidden_backend_ratio = ratio(hidden_backend_write_bytes, vs_buffer_write_bytes)
    stream_per_draw = as_float(joined.get("dxmt_stream_handle_changes_per_draw"))
    ib_per_draw = as_float(joined.get("dxmt_ib_handle_changes_per_draw"))
    confidence = "low"
    significant_vs_buffer_write = vs_buffer_write_bytes >= 128.0 * 1024.0 * 1024.0
    if (significant_vs_buffer_write and
            vs_buffer_ratio > 0.90 and cpu_writer_ratio < 0.10 and
            (vsout_ratio == 0.0 or vsout_ratio > 4.0)):
        hint = "gpu_vs_buffer_write"
        confidence = "high"
    elif cbuf_ratio > 0.50 and cpu_writer_ratio > 0.50:
        hint = "argbuf_cbuf_upload"
        confidence = "high"
    elif cbuf_ratio > 0.50:
        hint = "argbuf_cbuf_upload"
        confidence = "medium"
    elif transient_ratio > 0.50 and cpu_writer_ratio > 0.50:
        hint = "geometry_transient_upload"
        confidence = "high"
    elif transient_ratio > 0.50:
        hint = "geometry_transient_upload"
        confidence = "medium"
    elif stream_per_draw > 0.75 or ib_per_draw > 0.75:
        hint = "stream_ib_churn"
        confidence = "medium"
    else:
        hint = "mixed"

    backend_class = "unclassified"
    backend_confidence = "low"
    backend_probe_hint = "inspect-row"
    if (hint == "gpu_vs_buffer_write" and hidden_backend_ratio > 0.90 and
            vsout_ratio > 4.0 and vs_bytes_per_primitive > 1024.0):
        backend_class = "hidden_vertex_tiler_parameter_storage"
        backend_confidence = "high"
        backend_probe_hint = "primitive-backend-pressure-or-state-shape-ab"
    elif (hint == "gpu_vs_buffer_write" and named_tiled_buffer_mib > 0.0 and
            hidden_backend_ratio <= 0.90):
        backend_class = "named_tiled_vertex_or_primitive_storage"
        backend_confidence = "medium"
        backend_probe_hint = "tile-density-or-render-pass-ab"
    elif hint == "gpu_vs_buffer_write":
        backend_class = "gpu_vertex_buffer_write_unknown"
        backend_confidence = "medium"
        backend_probe_hint = "shader-codegen-and-state-shape-ab"
    elif hint == "stream_ib_churn":
        backend_class = "cpu_state_churn_secondary"
        backend_confidence = "medium"
        backend_probe_hint = "draw-run-binding-coalescing"
    elif hint in {"argbuf_cbuf_upload", "geometry_transient_upload"}:
        backend_class = "explicit_dxmt_writer"
        backend_confidence = confidence
        backend_probe_hint = "reduce-explicit-writer-bytes"

    joined["dxmt_named_tiled_buffer_mib"] = named_tiled_buffer_mib
    joined["dxmt_hidden_backend_write_mib"] = (
        hidden_backend_write_bytes / (1024.0 * 1024.0))
    joined["dxmt_hidden_backend_write_ratio"] = hidden_backend_ratio
    expected_vsout_bytes = as_float(
        joined.get("dxmt_vsout_expected_stage_out_bytes_per_vertex"))
    if vs_invocations > 0.0 and expected_vsout_bytes > 0.0:
        tvb_proxy_mib = vs_invocations * expected_vsout_bytes / (1024.0 * 1024.0)
        joined["dxmt_tvb_pressure_proxy_mib"] = tvb_proxy_mib
        joined["dxmt_tvb_named_to_proxy_ratio"] = (
            named_tiled_buffer_mib / tvb_proxy_mib
        )
        joined["dxmt_vs_buffer_write_to_tvb_proxy_ratio"] = (
            (vs_buffer_write_bytes / (1024.0 * 1024.0)) / tvb_proxy_mib
        )
    else:
        joined["dxmt_tvb_pressure_proxy_mib"] = ""
        joined["dxmt_tvb_named_to_proxy_ratio"] = ""
        joined["dxmt_vs_buffer_write_to_tvb_proxy_ratio"] = ""
    joined["dxmt_backend_storage_class"] = backend_class
    joined["dxmt_backend_storage_confidence"] = backend_confidence
    joined["dxmt_backend_probe_hint"] = backend_probe_hint
    joined["dxmt_gpu_write_hint"] = hint
    joined["dxmt_write_owner_confidence"] = confidence


def write_rows(path: Path, rows: list[dict[str, Any]], fields: tuple[str, ...]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def as_float(value: Any) -> float:
    if isinstance(value, (int, float)):
        return float(value)
    try:
        return float(str(value))
    except (TypeError, ValueError):
        return 0.0


def as_int(value: Any) -> int:
    if isinstance(value, int):
        return value
    if isinstance(value, float):
        return int(value)
    try:
        text = str(value)
        if text.startswith(("0x", "0X")):
            return int(text, 16)
        return int(text)
    except (TypeError, ValueError):
        return 0


def fmt_float(value: Any, digits: int = 3) -> str:
    return f"{as_float(value):.{digits}f}"


def fmt_int(value: Any) -> str:
    return f"{as_int(value):,}"


def pick_gpu_hot_set(
    rows: list[dict[str, Any]],
    total_gpu_ms: float,
    target_share_pct: float,
) -> list[dict[str, Any]]:
    """Pick GPU-time-ranked rows until they represent the frame hot set."""
    if not rows:
        return []
    selected: list[dict[str, Any]] = []
    selected_gpu_ms = 0.0
    min_rows = min(3, len(rows))
    target = max(0.0, target_share_pct)
    for row in rows:
        selected.append(row)
        selected_gpu_ms += as_float(row.get("gpu_ms"))
        share = selected_gpu_ms / total_gpu_ms * 100.0 if total_gpu_ms else 0.0
        if len(selected) >= min_rows and (share >= target or len(selected) == len(rows)):
            break
    return selected


def count_field(rows: list[dict[str, Any]], field: str) -> str:
    counts: dict[str, int] = {}
    for row in rows:
        value = str(row.get(field) or "")
        if value:
            counts[value] = counts.get(value, 0) + 1
    return ", ".join(f"{key}:{value}" for key, value in sorted(counts.items()))


def encoder_keys(rows: list[dict[str, Any]]) -> str:
    keys = []
    for row in rows:
        seq = row.get("seq")
        enc = row.get("enc")
        if isinstance(seq, int) and isinstance(enc, int):
            keys.append(f"{seq}/{enc}")
        else:
            keys.append(str(row.get("xcode_index", "")))
    return ", ".join(key for key in keys if key)


def aggregate_brief(rows: list[dict[str, Any]], total_gpu_ms: float) -> dict[str, Any]:
    gpu_ms = sum(as_float(row.get("gpu_ms")) for row in rows)
    buffer_write_mib = sum(as_float(row.get("buffer_write_mib")) for row in rows)
    vs_buffer_write_mib = sum(as_float(row.get("vs_buffer_write_mib")) for row in rows)
    vs_invocations = sum(as_float(row.get("vs_invocations")) for row in rows)
    indexed_refs = sum(as_int(row.get("dxmt_indexed_vertex_reference_count")) for row in rows)
    indexed_unique = sum(as_int(row.get("dxmt_indexed_unique_vertex_estimate")) for row in rows)
    cache_miss_64 = sum(
        as_int(row.get("dxmt_indexed_vertex_cache_miss_estimate_64")) for row in rows
    )
    named_tiled_mib = sum(as_float(row.get("dxmt_named_tiled_buffer_mib")) for row in rows)
    hidden_backend_mib = sum(
        as_float(row.get("dxmt_hidden_backend_write_mib")) for row in rows
    )
    cpu_writer_bytes = sum(as_int(row.get("dxmt_cpu_writer_bytes")) for row in rows)
    vsout_expected_weight = sum(
        as_int(row.get("dxmt_vsout_expected_stage_out_bytes_per_vertex")) *
        as_float(row.get("vs_invocations"))
        for row in rows
    )
    vsout_invocations = sum(
        as_float(row.get("vs_invocations"))
        for row in rows
        if as_int(row.get("dxmt_vsout_expected_stage_out_bytes_per_vertex"))
    )
    vs_write_bytes = vs_buffer_write_mib * 1024.0 * 1024.0
    return {
        "encoder_count": len(rows),
        "encoder_keys": encoder_keys(rows),
        "gpu_ms": gpu_ms,
        "gpu_share_pct": gpu_ms / total_gpu_ms * 100.0 if total_gpu_ms else 0.0,
        "buffer_write_mib": buffer_write_mib,
        "vs_buffer_write_mib": vs_buffer_write_mib,
        "vs_buffer_write_share": (
            vs_buffer_write_mib / buffer_write_mib if buffer_write_mib else 0.0
        ),
        "vs_invocations": vs_invocations,
        "vs_bytes_per_invocation": (
            vs_write_bytes / vs_invocations if vs_invocations else 0.0
        ),
        "indexed_refs": indexed_refs,
        "indexed_unique": indexed_unique,
        "indexed_reuse_ratio": indexed_refs / indexed_unique if indexed_unique else 0.0,
        "vs_invocations_per_indexed_unique": (
            vs_invocations / indexed_unique if indexed_unique else 0.0
        ),
        "vs_bytes_per_indexed_unique": (
            vs_write_bytes / indexed_unique if indexed_unique else 0.0
        ),
        "cache_miss_64": cache_miss_64,
        "cache_miss_64_over_unique": (
            cache_miss_64 / indexed_unique if indexed_unique else 0.0
        ),
        "vs_invocations_per_cache_miss_64": (
            vs_invocations / cache_miss_64 if cache_miss_64 else 0.0
        ),
        "vs_bytes_per_cache_miss_64": (
            vs_write_bytes / cache_miss_64 if cache_miss_64 else 0.0
        ),
        "expected_vsout_bytes_per_vertex": (
            vsout_expected_weight / vsout_invocations if vsout_invocations else 0.0
        ),
        "vs_buffer_to_expected_vsout": (
            vs_write_bytes / vsout_expected_weight if vsout_expected_weight else 0.0
        ),
        "named_tiled_buffer_mib": named_tiled_mib,
        "hidden_backend_mib": hidden_backend_mib,
        "hidden_backend_ratio": (
            hidden_backend_mib / vs_buffer_write_mib if vs_buffer_write_mib else 0.0
        ),
        "cpu_writer_mib": cpu_writer_bytes / (1024.0 * 1024.0),
        "backend_class_summary": count_field(rows, "dxmt_backend_storage_class"),
        "backend_probe_summary": count_field(rows, "dxmt_backend_probe_hint"),
        "hint_summary": count_field(rows, "dxmt_gpu_write_hint"),
    }


def write_report(
    path: Path,
    joined: list[dict[str, Any]],
    run_label: str,
    dxmt_streams: dict[tuple[int, int], list[dict[str, Any]]] | None = None,
    hot_gpu_share_target: float = 95.0,
) -> None:
    total_gpu_ms = sum(as_float(row.get("gpu_ms")) for row in joined)
    total_buffer_write_mib = sum(as_float(row.get("buffer_write_mib")) for row in joined)
    total_device_write_mib = sum(as_float(row.get("device_write_mib")) for row in joined)
    top = joined[:3]
    hot_set = pick_gpu_hot_set(joined, total_gpu_ms, hot_gpu_share_target)
    hot = aggregate_brief(hot_set, total_gpu_ms)
    top_gpu_ms = sum(as_float(row.get("gpu_ms")) for row in top)
    top_buffer_write_mib = sum(as_float(row.get("buffer_write_mib")) for row in top)
    top_device_write_mib = sum(as_float(row.get("device_write_mib")) for row in top)
    top_vs_buffer_write_mib = sum(as_float(row.get("vs_buffer_write_mib")) for row in top)
    top_fs_buffer_write_mib = sum(as_float(row.get("fs_buffer_write_mib")) for row in top)
    top_texture_write_mib = sum(as_float(row.get("texture_write_mib")) for row in top)
    top_depth_write_mib = sum(as_float(row.get("depth_write_mib")) for row in top)
    top_tiled_vertex_buffer_mib = sum(as_float(row.get("tiled_vertex_buffer_mib")) for row in top)
    top_tiled_primitive_block_mib = sum(
        as_float(row.get("tiled_primitive_block_mib")) for row in top
    )
    top_named_tiled_buffer_mib = sum(
        as_float(row.get("dxmt_named_tiled_buffer_mib")) for row in top
    )
    top_hidden_backend_write_mib = sum(
        as_float(row.get("dxmt_hidden_backend_write_mib")) for row in top
    )
    top_vs_invocations = sum(as_float(row.get("vs_invocations")) for row in top)
    top_fs_invocations = sum(as_float(row.get("fs_invocations")) for row in top)
    top_primitives = sum(as_float(row.get("primitives")) for row in top)
    top_post_clipped_primitives = sum(
        as_float(row.get("post_clipped_primitives")) for row in top
    )
    top_pixels_rasterized = sum(as_float(row.get("pixels_rasterized")) for row in top)
    top_fs_tiles = sum(as_float(row.get("fs_tiles_processed")) for row in top)
    top_primitive_tile_estimate = sum(
        as_float(row.get("primitives_per_tile")) *
        as_float(row.get("fs_tiles_processed"))
        for row in top
    )
    top_tiled_buffer_mib = top_tiled_vertex_buffer_mib + top_tiled_primitive_block_mib
    top_vs_l1_write_mib = sum(as_float(row.get("vs_l1_write_mib")) for row in top)
    top_vs_llc_write_mib = sum(as_float(row.get("vs_llc_write_mib")) for row in top)
    top_vs_bytes_per_vs_invocation = (
        top_vs_buffer_write_mib * 1024.0 * 1024.0 / top_vs_invocations
        if top_vs_invocations else 0.0
    )
    top_vs_bytes_per_fragment = (
        top_vs_buffer_write_mib * 1024.0 * 1024.0 / top_fs_invocations
        if top_fs_invocations else 0.0
    )
    top_vs_bytes_per_primitive = (
        top_vs_buffer_write_mib * 1024.0 * 1024.0 / top_primitives
        if top_primitives else 0.0
    )
    top_vs_bytes_per_post_clipped_primitive = (
        top_vs_buffer_write_mib * 1024.0 * 1024.0 / top_post_clipped_primitives
        if top_post_clipped_primitives else 0.0
    )
    top_vs_bytes_per_primitive_tile = (
        top_vs_buffer_write_mib * 1024.0 * 1024.0 / top_primitive_tile_estimate
        if top_primitive_tile_estimate else 0.0
    )
    top_vs_bytes_per_pixel = (
        top_vs_buffer_write_mib * 1024.0 * 1024.0 / top_pixels_rasterized
        if top_pixels_rasterized else 0.0
    )
    top_vsout_expected_weight = sum(
        as_int(row.get("dxmt_vsout_expected_stage_out_bytes_per_vertex")) *
        as_float(row.get("vs_invocations"))
        for row in top
    )
    top_vsout_invocations = sum(
        as_float(row.get("vs_invocations"))
        for row in top
        if as_int(row.get("dxmt_vsout_expected_stage_out_bytes_per_vertex"))
    )
    top_vsout_expected_bytes = (
        top_vsout_expected_weight / top_vsout_invocations
        if top_vsout_invocations else 0.0
    )
    top_vsout_ratio = (
        top_vs_buffer_write_mib * 1024.0 * 1024.0 / top_vsout_expected_weight
        if top_vsout_expected_weight else 0.0
    )
    top_vs_to_tiled_ratio = (
        top_vs_buffer_write_mib / top_tiled_buffer_mib
        if top_tiled_buffer_mib else 0.0
    )
    top_hidden_backend_write_ratio = (
        top_hidden_backend_write_mib / top_vs_buffer_write_mib
        if top_vs_buffer_write_mib else 0.0
    )
    top_vs_l1_to_device_ratio = (
        top_vs_l1_write_mib / top_vs_buffer_write_mib
        if top_vs_buffer_write_mib else 0.0
    )
    top_vs_llc_to_device_ratio = (
        top_vs_llc_write_mib / top_vs_buffer_write_mib
        if top_vs_buffer_write_mib else 0.0
    )
    top_vertex_stage_time_pct = sum(
        as_float(row.get("vertex_stage_time_pct")) * as_float(row.get("gpu_ms"))
        for row in top
    ) / top_gpu_ms if top_gpu_ms else 0.0
    top_vs_alu_limiter_pct = sum(
        as_float(row.get("vs_alu_limiter_pct")) * as_float(row.get("gpu_ms"))
        for row in top
    ) / top_gpu_ms if top_gpu_ms else 0.0
    top_vs_buffer_write_limiter_pct = sum(
        as_float(row.get("vs_buffer_write_limiter_pct")) * as_float(row.get("gpu_ms"))
        for row in top
    ) / top_gpu_ms if top_gpu_ms else 0.0
    top_tile_intersections_pct = sum(
        as_float(row.get("primitive_block_tile_intersections_pct")) * as_float(row.get("gpu_ms"))
        for row in top
    ) / top_gpu_ms if top_gpu_ms else 0.0
    top_shaded_vertex_read_limiter_pct = sum(
        as_float(row.get("shaded_vertex_read_limiter_pct")) * as_float(row.get("gpu_ms"))
        for row in top
    ) / top_gpu_ms if top_gpu_ms else 0.0
    top_cull_unit_limiter_pct = sum(
        as_float(row.get("cull_unit_limiter_pct")) * as_float(row.get("gpu_ms"))
        for row in top
    ) / top_gpu_ms if top_gpu_ms else 0.0
    top_clip_unit_limiter_pct = sum(
        as_float(row.get("clip_unit_limiter_pct")) * as_float(row.get("gpu_ms"))
        for row in top
    ) / top_gpu_ms if top_gpu_ms else 0.0
    top_draws = sum(as_int(row.get("dxmt_draw_calls")) for row in top)
    top_ffp_draws = sum(as_int(row.get("dxmt_ffp_draws")) for row in top)
    top_pretransformed_draws = sum(as_int(row.get("dxmt_pretransformed_draws")) for row in top)
    top_textured_draws = sum(as_int(row.get("dxmt_textured_draws")) for row in top)
    top_cull_none_draws = sum(as_int(row.get("dxmt_cull_none_draws")) for row in top)
    top_cull_front_draws = sum(as_int(row.get("dxmt_cull_front_draws")) for row in top)
    top_cull_back_draws = sum(as_int(row.get("dxmt_cull_back_draws")) for row in top)
    top_depth_enabled_draws = sum(as_int(row.get("dxmt_depth_enabled_draws")) for row in top)
    top_depth_write_draws = sum(as_int(row.get("dxmt_depth_write_draws")) for row in top)
    top_scissor_enabled_draws = sum(as_int(row.get("dxmt_scissor_enabled_draws")) for row in top)
    top_alpha_blend_draws = sum(as_int(row.get("dxmt_alpha_blend_enabled_draws")) for row in top)
    top_alpha_test_draws = sum(as_int(row.get("dxmt_alpha_test_enabled_draws")) for row in top)
    top_alpha_test_effective_draws = sum(as_int(row.get("dxmt_alpha_test_effective_draws")) for row in top)
    top_clip_plane_draws = sum(as_int(row.get("dxmt_clip_plane_enabled_draws")) for row in top)
    top_dxmt_vertex_count = sum(as_int(row.get("dxmt_vertex_count")) for row in top)
    top_dxmt_triangle_estimate = sum(as_int(row.get("dxmt_triangle_estimate")) for row in top)
    top_draw_geometry_signature_samples = sum(
        as_int(row.get("dxmt_draw_geometry_signature_samples")) for row in top)
    top_draw_geometry_signature_unique = sum(
        as_int(row.get("dxmt_draw_geometry_signature_unique")) for row in top)
    top_draw_geometry_signature_duplicates = sum(
        as_int(row.get("dxmt_draw_geometry_signature_duplicates")) for row in top)
    top_draw_geometry_signature_consecutive_duplicates = sum(
        as_int(row.get("dxmt_draw_geometry_signature_consecutive_duplicates"))
        for row in top)
    top_draw_geometry_signature_duplicate_ratio = (
        top_draw_geometry_signature_duplicates / top_draw_geometry_signature_samples
        if top_draw_geometry_signature_samples else 0.0
    )
    top_draw_geometry_signature_consecutive_duplicate_ratio = (
        top_draw_geometry_signature_consecutive_duplicates /
        top_draw_geometry_signature_samples
        if top_draw_geometry_signature_samples else 0.0
    )
    top_indexed_base_vertex_samples = sum(
        as_int(row.get("dxmt_indexed_base_vertex_samples")) for row in top)
    top_indexed_base_vertex_nonzero = sum(
        as_int(row.get("dxmt_indexed_base_vertex_nonzero_draws")) for row in top)
    top_indexed_base_vertex_negative = sum(
        as_int(row.get("dxmt_indexed_base_vertex_negative_draws")) for row in top)
    top_indexed_base_vertex_positive = sum(
        as_int(row.get("dxmt_indexed_base_vertex_positive_draws")) for row in top)
    top_native_base_vertex_requested = sum(
        as_int(row.get("dxmt_native_base_vertex_requested_draws")) for row in top)
    top_native_base_vertex_used = sum(
        as_int(row.get("dxmt_native_base_vertex_used_draws")) for row in top)
    top_native_base_vertex_skipped_negative = sum(
        as_int(row.get("dxmt_native_base_vertex_skipped_negative_draws")) for row in top)
    top_split_large_source_draws = sum(
        as_int(row.get("dxmt_split_large_indexed_source_draws")) for row in top)
    top_split_large_metal_draws = sum(
        as_int(row.get("dxmt_split_large_indexed_metal_draws")) for row in top)
    top_split_large_extra_draws = sum(
        as_int(row.get("dxmt_split_large_indexed_extra_draws")) for row in top)
    top_split_large_primitives = sum(
        as_int(row.get("dxmt_split_large_indexed_primitive_count")) for row in top)
    top_indexed_vertex_reuse_samples = sum(
        as_int(row.get("dxmt_indexed_vertex_reuse_samples")) for row in top)
    top_indexed_vertex_reuse_skipped = sum(
        as_int(row.get("dxmt_indexed_vertex_reuse_skipped")) for row in top)
    top_indexed_vertex_reference_count = sum(
        as_int(row.get("dxmt_indexed_vertex_reference_count")) for row in top)
    top_indexed_unique_vertex_estimate = sum(
        as_int(row.get("dxmt_indexed_unique_vertex_estimate")) for row in top)
    top_indexed_cache_miss_estimates = {
        16: sum(as_int(row.get("dxmt_indexed_vertex_cache_miss_estimate_16")) for row in top),
        32: sum(as_int(row.get("dxmt_indexed_vertex_cache_miss_estimate_32")) for row in top),
        64: sum(as_int(row.get("dxmt_indexed_vertex_cache_miss_estimate_64")) for row in top),
    }
    top_dxmt_stream0_input_min_mib = sum(
        as_float(row.get("dxmt_stream0_input_min_mib")) for row in top
    )
    top_dxmt_stream0_input_max_mib = sum(
        as_float(row.get("dxmt_stream0_input_max_mib")) for row in top
    )
    top_stream_handles = sum(as_int(row.get("dxmt_stream_handle_changes")) for row in top)
    top_stream_offsets = sum(as_int(row.get("dxmt_stream_offset_changes")) for row in top)
    top_stream_strides = sum(as_int(row.get("dxmt_stream_stride_changes")) for row in top)
    top_stream_unique_handles = sum(as_int(row.get("dxmt_stream_unique_handles")) for row in top)
    top_stream_unique_bytes = sum(as_int(row.get("dxmt_stream_unique_bytes")) for row in top)
    top_ib_handles = sum(as_int(row.get("dxmt_ib_handle_changes")) for row in top)
    top_ib_unique_handles = sum(as_int(row.get("dxmt_ib_unique_handles")) for row in top)
    top_ib_unique_bytes = sum(as_int(row.get("dxmt_ib_unique_bytes")) for row in top)
    top_pso_handles = sum(as_int(row.get("dxmt_pso_handle_changes")) for row in top)
    top_pso_samples = sum(as_int(row.get("dxmt_pso_state_samples")) for row in top)
    top_pso_samples_per_draw = top_pso_samples / top_draws if top_draws else 0.0
    top_blend_state_changes = sum(as_int(row.get("dxmt_blend_state_changes")) for row in top)
    top_blend_state_unique = sum(as_int(row.get("dxmt_blend_state_unique")) for row in top)
    top_blend_enabled_noop_draws = sum(
        as_int(row.get("dxmt_blend_enabled_noop_draws")) for row in top)
    top_blend_constant_factor_draws = sum(
        as_int(row.get("dxmt_blend_constant_factor_draws")) for row in top)
    top_shader_variants = sum(as_int(row.get("dxmt_shader_variant_changes")) for row in top)
    top_vsout_layouts = sum(as_int(row.get("dxmt_vsout_layout_changes")) for row in top)
    top_vsout_cache_hits = sum(as_int(row.get("dxmt_vsout_layout_cache_hits")) for row in top)
    top_vsout_cache_misses = sum(as_int(row.get("dxmt_vsout_layout_cache_misses")) for row in top)
    top_cbuf_bytes = sum(as_int(row.get("dxmt_argbuf_cbuf_bytes")) for row in top)
    top_cbuf_vs_bytes = sum(as_int(row.get("dxmt_argbuf_cbuf_vs_bytes")) for row in top)
    top_cbuf_ffp_vs_bytes = sum(as_int(row.get("dxmt_argbuf_cbuf_ffp_vs_bytes")) for row in top)
    top_cbuf_ps_bytes = sum(as_int(row.get("dxmt_argbuf_cbuf_ps_bytes")) for row in top)
    top_cbuf_ffp_ps_bytes = sum(as_int(row.get("dxmt_argbuf_cbuf_ffp_ps_bytes")) for row in top)
    top_argbuf_table_bytes = sum(as_int(row.get("dxmt_argbuf_table_bytes")) for row in top)
    top_set_vertex_bytes = sum(as_int(row.get("dxmt_set_vertex_bytes_bytes")) for row in top)
    top_set_vertex_bytes_slot5 = sum(as_int(row.get("dxmt_set_vertex_bytes_slot5_bytes")) for row in top)
    top_set_vertex_bytes_other = sum(as_int(row.get("dxmt_set_vertex_bytes_other_bytes")) for row in top)
    top_transient_bytes = sum(
        as_int(row.get("dxmt_transient_vertex_bytes")) +
        as_int(row.get("dxmt_transient_index_bytes"))
        for row in top
    )
    top_transient_vertex_expanded_bytes = sum(
        as_int(row.get("dxmt_transient_vertex_expanded_main_bytes")) +
        as_int(row.get("dxmt_transient_vertex_expanded_extra_bytes"))
        for row in top
    )
    top_transient_vertex_decl_fallback_bytes = sum(
        as_int(row.get("dxmt_transient_vertex_decl_fallback_bytes")) for row in top
    )
    top_transient_vertex_user_bytes = sum(
        as_int(row.get("dxmt_transient_vertex_user_bytes")) +
        as_int(row.get("dxmt_transient_vertex_preupload_bytes"))
        for row in top
    )
    top_transient_index_user_bytes = sum(
        as_int(row.get("dxmt_transient_index_user_bytes")) +
        as_int(row.get("dxmt_transient_index_preupload_bytes"))
        for row in top
    )
    top_transient_index_shadow_bytes = sum(
        as_int(row.get("dxmt_transient_index_shadow_fallback_bytes")) for row in top
    )
    top_cpu_writer_bytes = sum(as_int(row.get("dxmt_cpu_writer_bytes")) for row in top)
    top_unexplained_buffer_write_mib = sum(
        as_float(row.get("dxmt_unexplained_buffer_write_mib")) for row in top
    )

    top_gpu_share = (top_gpu_ms / total_gpu_ms * 100.0) if total_gpu_ms else 0.0
    cbuf_mib = top_cbuf_bytes / (1024.0 * 1024.0)
    transient_mib = top_transient_bytes / (1024.0 * 1024.0)
    argbuf_table_mib = top_argbuf_table_bytes / (1024.0 * 1024.0)
    set_vertex_bytes_mib = top_set_vertex_bytes / (1024.0 * 1024.0)
    cpu_writer_mib = top_cpu_writer_bytes / (1024.0 * 1024.0)
    cpu_writer_to_buffer_ratio = (
        cpu_writer_mib / top_buffer_write_mib if top_buffer_write_mib else 0.0
    )
    cbuf_to_buffer_ratio = cbuf_mib / top_buffer_write_mib if top_buffer_write_mib else 0.0
    transient_to_buffer_ratio = (
        transient_mib / top_buffer_write_mib if top_buffer_write_mib else 0.0
    )
    vs_buffer_write_share = (
        top_vs_buffer_write_mib / top_buffer_write_mib if top_buffer_write_mib else 0.0
    )
    unexplained_buffer_write_ratio = (
        top_unexplained_buffer_write_mib / top_buffer_write_mib
        if top_buffer_write_mib else 0.0
    )
    vs_buffer_bytes_per_dxmt_vertex = (
        top_vs_buffer_write_mib * 1024.0 * 1024.0 / top_dxmt_vertex_count
        if top_dxmt_vertex_count else 0.0
    )
    vs_invocations_per_dxmt_vertex = (
        top_vs_invocations / top_dxmt_vertex_count
        if top_dxmt_vertex_count else 0.0
    )
    indexed_vertex_reuse_ratio = (
        top_indexed_vertex_reference_count / top_indexed_unique_vertex_estimate
        if top_indexed_unique_vertex_estimate else 0.0
    )
    vs_invocations_per_indexed_unique_vertex = (
        top_vs_invocations / top_indexed_unique_vertex_estimate
        if top_indexed_unique_vertex_estimate else 0.0
    )
    vs_buffer_bytes_per_indexed_unique_vertex = (
        top_vs_buffer_write_mib * 1024.0 * 1024.0 / top_indexed_unique_vertex_estimate
        if top_indexed_unique_vertex_estimate else 0.0
    )
    indexed_cache_miss_over_unique = {
        size: (
            misses / top_indexed_unique_vertex_estimate
            if top_indexed_unique_vertex_estimate else 0.0
        )
        for size, misses in top_indexed_cache_miss_estimates.items()
    }
    vs_invocations_per_indexed_cache_miss = {
        size: (top_vs_invocations / misses if misses else 0.0)
        for size, misses in top_indexed_cache_miss_estimates.items()
    }
    vs_buffer_bytes_per_indexed_cache_miss = {
        size: (
            top_vs_buffer_write_mib * 1024.0 * 1024.0 / misses
            if misses else 0.0
        )
        for size, misses in top_indexed_cache_miss_estimates.items()
    }
    top_vs_buffer_to_stream0_input_min_ratio = (
        top_vs_buffer_write_mib / top_dxmt_stream0_input_min_mib
        if top_dxmt_stream0_input_min_mib else 0.0
    )
    top_vs_buffer_to_stream0_input_max_ratio = (
        top_vs_buffer_write_mib / top_dxmt_stream0_input_max_mib
        if top_dxmt_stream0_input_max_mib else 0.0
    )
    hint_counts: dict[str, int] = {}
    confidence_counts: dict[str, int] = {}
    backend_class_counts: dict[str, int] = {}
    backend_probe_counts: dict[str, int] = {}
    for row in top:
        hint = str(row.get("dxmt_gpu_write_hint") or "")
        if hint:
            hint_counts[hint] = hint_counts.get(hint, 0) + 1
        confidence = str(row.get("dxmt_write_owner_confidence") or "")
        if confidence:
            confidence_counts[confidence] = confidence_counts.get(confidence, 0) + 1
        backend_class = str(row.get("dxmt_backend_storage_class") or "")
        if backend_class:
            backend_class_counts[backend_class] = backend_class_counts.get(backend_class, 0) + 1
        backend_probe = str(row.get("dxmt_backend_probe_hint") or "")
        if backend_probe:
            backend_probe_counts[backend_probe] = backend_probe_counts.get(backend_probe, 0) + 1
    hint_summary = ", ".join(f"{key}:{value}" for key, value in sorted(hint_counts.items()))
    confidence_summary = ", ".join(
        f"{key}:{value}" for key, value in sorted(confidence_counts.items())
    )
    backend_class_summary = ", ".join(
        f"{key}:{value}" for key, value in sorted(backend_class_counts.items())
    )
    backend_probe_summary = ", ".join(
        f"{key}:{value}" for key, value in sorted(backend_probe_counts.items())
    )

    notes: list[str] = []
    if top_buffer_write_mib > max(cbuf_mib * 64.0, 256.0):
        notes.append(
            "Top encoder cost is dominated by Xcode-reported device/buffer writes, "
            "not by dxmt argbuf cbuf upload bytes."
        )
    if top_buffer_write_mib and top_vs_buffer_write_mib / top_buffer_write_mib > 0.90:
        notes.append(
            "Top buffer writes are overwhelmingly VS buffer writes; focus on "
            "vertex-stage output/varying or spill-like traffic before FS/texture/depth writes."
        )
    if top_buffer_write_mib and cpu_writer_to_buffer_ratio < 0.10:
        notes.append(
            "dxmt-attributed CPU writer bytes explain less than 10% of top Xcode "
            "buffer writes; treat the remaining traffic as GPU-side vertex-stage output."
        )
    if top_vs_bytes_per_vs_invocation > 512.0:
        notes.append(
            "VS buffer writes exceed 512 bytes per VS invocation in the top encoders; "
            "this is too large to treat as simple color/texcoord varyings alone."
        )
    if top_vs_bytes_per_post_clipped_primitive > 1024.0:
        notes.append(
            "VS buffer writes are also kilobytes per post-clipped primitive; "
            "the bucket scales more like vertex-stage primitive/binning metadata than "
            "ordinary stage-output width."
        )
    if top_vs_to_tiled_ratio > 16.0:
        notes.append(
            "VS buffer writes are far larger than Xcode tiled vertex/primitive-block bytes; "
            "do not equate the bucket with explicit tiled-vertex storage only."
        )
    if top_hidden_backend_write_ratio > 0.90:
        notes.append(
            "After subtracting named tiled-buffer counters and dxmt CPU writers, "
            "more than 90% of top VS buffer writes remain hidden backend traffic."
        )
    if top_tile_intersections_pct > 0 and top_vs_to_tiled_ratio > 16.0:
        notes.append(
            "Xcode tiler counters are present but small compared with VS buffer writes; "
            "check compiler spill/internal VS buffer traffic before optimizing primitive-block storage."
        )
    if top_vsout_ratio > 4.0:
        notes.append(
            "VS buffer writes are more than 4x the expected VSOut stage-out width; "
            "ordinary varying width alone is unlikely to explain the traffic."
        )
    if top_vs_buffer_to_stream0_input_max_ratio > 16.0:
        notes.append(
            "VS buffer writes are far larger than the dxmt stream0 input-byte upper bound; "
            "inspect GPU-side vertex-stage spill/output traffic rather than input fetch alone."
        )
    if top_vertex_stage_time_pct > 90.0 and top_vs_alu_limiter_pct < 10.0:
        notes.append(
            "Top encoders are vertex-stage dominated without a matching VS ALU limiter; "
            "prioritize vertex-stage memory/tiler traffic over ALU optimization."
        )
    if top_draws and top_stream_handles / top_draws > 0.75:
        notes.append("Stream handle churn is near draw frequency in the top encoders.")
    if top_draws and top_stream_offsets / top_draws > 0.75:
        notes.append("Stream offset churn is near draw frequency in the top encoders.")
    if top_draws and top_stream_strides / top_draws > 0.25:
        notes.append("Stream stride changes are frequent enough to check vertex-decl/run batching.")
    if top_draws and top_ib_handles / top_draws > 0.75:
        notes.append("IB handle churn is near draw frequency in the top encoders.")
    if len(hot_set) > len(top):
        notes.append(
            "Top three encoders do not cover the configured hot-set GPU share; "
            "use the Hot Set Aggregate for whole-frame bottleneck conclusions."
        )
    if top_draws and top_dxmt_vertex_count == 0:
        notes.append(
            "dxmt geometry-shape attribution is absent for these joined rows; "
            "the log predates per-encoder primitive/vertex/FFP fields."
        )
    if top_draws and top_pso_samples_per_draw < 0.90:
        notes.append(
            "PSO/VSOut attribution does not cover draw frequency in the top encoders; "
            "the log is too old or incomplete for shader/layout root-cause claims."
        )
    if top_vsout_layouts > 0:
        notes.append(
            "Top encoders switch VSOut layouts; compare the per-row layout keys before "
            "treating VS buffer writes as one uniform shader shape."
        )
    if top_transient_bytes == 0:
        notes.append("Transient vertex/index writes are absent from the top encoders.")
    if not notes:
        notes.append("No single heuristic dominates; inspect the top encoder rows directly.")

    lines: list[str] = []
    lines.append("# Xcode Encoder Bottleneck Report")
    lines.append("")
    lines.append(f"- Run: `{run_label}`")
    lines.append(f"- Encoders: `{len(joined)}`")
    lines.append(f"- Total GPU: `{fmt_float(total_gpu_ms)} ms`")
    lines.append(f"- Total buffer write: `{fmt_float(total_buffer_write_mib)} MiB`")
    lines.append(f"- Total device write: `{fmt_float(total_device_write_mib)} MiB`")
    lines.append(f"- Top 3 GPU share: `{fmt_float(top_gpu_share, 2)}%`")
    lines.append(
        f"- Hot set GPU share: `{fmt_float(hot.get('gpu_share_pct'), 2)}%` "
        f"(`top {fmt_int(hot.get('encoder_count'))}`, target "
        f"`{fmt_float(hot_gpu_share_target, 2)}%`)"
    )
    lines.append("")
    lines.append("## Top 3 Aggregate")
    lines.append("")
    lines.append("| Metric | Value |")
    lines.append("|---|---:|")
    lines.append(f"| GPU time | `{fmt_float(top_gpu_ms)} ms` |")
    lines.append(f"| Buffer write | `{fmt_float(top_buffer_write_mib)} MiB` |")
    lines.append(f"| VS buffer write | `{fmt_float(top_vs_buffer_write_mib)} MiB` |")
    lines.append(f"| VS buffer / Xcode buffer write | `{fmt_float(vs_buffer_write_share, 3)}x` |")
    lines.append(
        f"| VS buffer bytes / VS invocation | `{fmt_float(top_vs_bytes_per_vs_invocation, 1)} B` |"
    )
    lines.append(
        f"| VS buffer bytes / fragment | `{fmt_float(top_vs_bytes_per_fragment, 1)} B` |"
    )
    lines.append(
        f"| VS buffer bytes / primitive | `{fmt_float(top_vs_bytes_per_primitive, 1)} B` |"
    )
    lines.append(
        "| VS buffer bytes / post-clipped primitive | "
        f"`{fmt_float(top_vs_bytes_per_post_clipped_primitive, 1)} B` |"
    )
    lines.append(
        "| VS buffer bytes / primitive-tile estimate | "
        f"`{fmt_float(top_vs_bytes_per_primitive_tile, 1)} B` |"
    )
    lines.append(
        f"| VS buffer bytes / rasterized pixel | `{fmt_float(top_vs_bytes_per_pixel, 1)} B` |"
    )
    lines.append(
        f"| VS buffer bytes / dxmt vertex | `{fmt_float(vs_buffer_bytes_per_dxmt_vertex, 1)} B` |"
    )
    lines.append(
        f"| VS invocations / dxmt vertex | `{fmt_float(vs_invocations_per_dxmt_vertex, 3)}x` |"
    )
    if top_indexed_vertex_reuse_samples or top_indexed_vertex_reuse_skipped:
        lines.append(
            f"| dxmt indexed reuse measured/skipped draws | "
            f"`{fmt_int(top_indexed_vertex_reuse_samples)} / "
            f"{fmt_int(top_indexed_vertex_reuse_skipped)}` |"
        )
        lines.append(
            f"| dxmt indexed references / unique estimate | "
            f"`{fmt_int(top_indexed_vertex_reference_count)} / "
            f"{fmt_int(top_indexed_unique_vertex_estimate)}` |"
        )
        lines.append(
            f"| dxmt indexed reference reuse ratio | "
            f"`{fmt_float(indexed_vertex_reuse_ratio, 3)}x` |"
        )
        lines.append(
            f"| VS invocations / dxmt indexed unique estimate | "
            f"`{fmt_float(vs_invocations_per_indexed_unique_vertex, 3)}x` |"
        )
        lines.append(
            f"| VS buffer bytes / dxmt indexed unique estimate | "
            f"`{fmt_float(vs_buffer_bytes_per_indexed_unique_vertex, 1)} B` |"
        )
        if any(top_indexed_cache_miss_estimates.values()):
            lines.append(
                f"| dxmt indexed cache miss estimate 16/32/64 | "
                f"`{fmt_int(top_indexed_cache_miss_estimates[16])} / "
                f"{fmt_int(top_indexed_cache_miss_estimates[32])} / "
                f"{fmt_int(top_indexed_cache_miss_estimates[64])}` |"
            )
            lines.append(
                f"| cache miss / unique estimate 16/32/64 | "
                f"`{fmt_float(indexed_cache_miss_over_unique[16], 3)}x / "
                f"{fmt_float(indexed_cache_miss_over_unique[32], 3)}x / "
                f"{fmt_float(indexed_cache_miss_over_unique[64], 3)}x` |"
            )
            lines.append(
                f"| VS invocations / cache miss 16/32/64 | "
                f"`{fmt_float(vs_invocations_per_indexed_cache_miss[16], 3)}x / "
                f"{fmt_float(vs_invocations_per_indexed_cache_miss[32], 3)}x / "
                f"{fmt_float(vs_invocations_per_indexed_cache_miss[64], 3)}x` |"
            )
            lines.append(
                f"| VS buffer bytes / cache miss 16/32/64 | "
                f"`{fmt_float(vs_buffer_bytes_per_indexed_cache_miss[16], 1)} / "
                f"{fmt_float(vs_buffer_bytes_per_indexed_cache_miss[32], 1)} / "
                f"{fmt_float(vs_buffer_bytes_per_indexed_cache_miss[64], 1)} B` |"
            )
    lines.append(
        f"| dxmt stream0 input min/max | `{fmt_float(top_dxmt_stream0_input_min_mib)} / {fmt_float(top_dxmt_stream0_input_max_mib)} MiB` |"
    )
    lines.append(
        f"| VS buffer / dxmt stream0 input min | `{fmt_float(top_vs_buffer_to_stream0_input_min_ratio, 1)}x` |"
    )
    lines.append(
        f"| VS buffer / dxmt stream0 input max | `{fmt_float(top_vs_buffer_to_stream0_input_max_ratio, 1)}x` |"
    )
    lines.append(
        f"| Expected VSOut bytes / vertex | `{fmt_float(top_vsout_expected_bytes, 1)} B` |"
    )
    lines.append(f"| VS buffer / expected VSOut | `{fmt_float(top_vsout_ratio, 1)}x` |")
    lines.append(f"| VS buffer / tiled buffer ratio | `{fmt_float(top_vs_to_tiled_ratio, 1)}x` |")
    lines.append(
        f"| Weighted primitive block tile intersections | `{fmt_float(top_tile_intersections_pct, 2)}%` |"
    )
    lines.append(f"| FS tiles processed | `{fmt_int(top_fs_tiles)}` |")
    lines.append(f"| VS L1 write | `{fmt_float(top_vs_l1_write_mib)} MiB` |")
    lines.append(f"| VS LLC write | `{fmt_float(top_vs_llc_write_mib)} MiB` |")
    lines.append(
        f"| VS L1 / VS device write | `{fmt_float(top_vs_l1_to_device_ratio, 2)}x` |"
    )
    lines.append(
        f"| VS LLC / VS device write | `{fmt_float(top_vs_llc_to_device_ratio, 2)}x` |"
    )
    lines.append(f"| Weighted vertex stage time | `{fmt_float(top_vertex_stage_time_pct, 2)}%` |")
    lines.append(f"| Weighted VS ALU limiter | `{fmt_float(top_vs_alu_limiter_pct, 2)}%` |")
    lines.append(
        f"| Weighted VS buffer-write limiter | `{fmt_float(top_vs_buffer_write_limiter_pct, 2)}%` |"
    )
    lines.append(
        f"| Weighted shaded-vertex-read limiter | `{fmt_float(top_shaded_vertex_read_limiter_pct, 2)}%` |"
    )
    lines.append(f"| Weighted cull-unit limiter | `{fmt_float(top_cull_unit_limiter_pct, 2)}%` |")
    lines.append(f"| Weighted clip-unit limiter | `{fmt_float(top_clip_unit_limiter_pct, 2)}%` |")
    lines.append(f"| FS buffer write | `{fmt_float(top_fs_buffer_write_mib)} MiB` |")
    lines.append(f"| Texture write | `{fmt_float(top_texture_write_mib)} MiB` |")
    lines.append(f"| Depth write | `{fmt_float(top_depth_write_mib)} MiB` |")
    lines.append(f"| Tiled vertex buffer | `{fmt_float(top_tiled_vertex_buffer_mib)} MiB` |")
    lines.append(
        f"| Tiled primitive-block buffer | `{fmt_float(top_tiled_primitive_block_mib)} MiB` |"
    )
    lines.append(f"| Named tiled buffer total | `{fmt_float(top_named_tiled_buffer_mib)} MiB` |")
    lines.append(
        f"| Hidden backend write estimate | `{fmt_float(top_hidden_backend_write_mib)} MiB` |"
    )
    lines.append(
        f"| Hidden backend / VS buffer write | `{fmt_float(top_hidden_backend_write_ratio, 3)}x` |"
    )
    lines.append(f"| Device write | `{fmt_float(top_device_write_mib)} MiB` |")
    lines.append(f"| dxmt draw calls | `{fmt_int(top_draws)}` |")
    lines.append(f"| dxmt FFP draws | `{fmt_int(top_ffp_draws)}` |")
    lines.append(f"| dxmt pre-transformed draws | `{fmt_int(top_pretransformed_draws)}` |")
    lines.append(f"| dxmt textured draws | `{fmt_int(top_textured_draws)}` |")
    lines.append(
        f"| dxmt cull none/front/back draws | `{fmt_int(top_cull_none_draws)} / {fmt_int(top_cull_front_draws)} / {fmt_int(top_cull_back_draws)}` |"
    )
    lines.append(f"| dxmt depth enabled/write draws | `{fmt_int(top_depth_enabled_draws)} / {fmt_int(top_depth_write_draws)}` |")
    lines.append(f"| dxmt scissor enabled draws | `{fmt_int(top_scissor_enabled_draws)}` |")
    lines.append(
        "| dxmt alpha blend/test/effective-test draws | "
        f"`{fmt_int(top_alpha_blend_draws)} / {fmt_int(top_alpha_test_draws)} / "
        f"{fmt_int(top_alpha_test_effective_draws)}` |")
    lines.append(f"| dxmt clip-plane enabled draws | `{fmt_int(top_clip_plane_draws)}` |")
    lines.append(f"| dxmt vertex count | `{fmt_int(top_dxmt_vertex_count)}` |")
    lines.append(f"| dxmt triangle estimate | `{fmt_int(top_dxmt_triangle_estimate)}` |")
    lines.append(
        f"| dxmt draw geometry signatures unique/duplicates | "
        f"`{fmt_int(top_draw_geometry_signature_unique)} / "
        f"{fmt_int(top_draw_geometry_signature_duplicates)}` |"
    )
    lines.append(
        f"| dxmt draw geometry signature duplicate ratio | "
        f"`{fmt_float(top_draw_geometry_signature_duplicate_ratio, 3)}x` |"
    )
    lines.append(
        f"| dxmt consecutive geometry duplicate ratio | "
        f"`{fmt_float(top_draw_geometry_signature_consecutive_duplicate_ratio, 3)}x` |"
    )
    lines.append(
        f"| dxmt indexed baseVertex samples/nonzero/neg/pos | "
        f"`{fmt_int(top_indexed_base_vertex_samples)} / "
        f"{fmt_int(top_indexed_base_vertex_nonzero)} / "
        f"{fmt_int(top_indexed_base_vertex_negative)} / "
        f"{fmt_int(top_indexed_base_vertex_positive)}` |"
    )
    lines.append(
        f"| dxmt native Metal baseVertex requested/used/skipped-neg | "
        f"`{fmt_int(top_native_base_vertex_requested)} / "
        f"{fmt_int(top_native_base_vertex_used)} / "
        f"{fmt_int(top_native_base_vertex_skipped_negative)}` |"
    )
    lines.append(
        f"| dxmt split large indexed source/metal/extra draws | "
        f"`{fmt_int(top_split_large_source_draws)} / "
        f"{fmt_int(top_split_large_metal_draws)} / "
        f"{fmt_int(top_split_large_extra_draws)}` |"
    )
    lines.append(
        f"| dxmt split large indexed primitives | `{fmt_int(top_split_large_primitives)}` |"
    )
    lines.append(f"| dxmt stream handle changes | `{fmt_int(top_stream_handles)}` |")
    lines.append(f"| dxmt stream offset changes | `{fmt_int(top_stream_offsets)}` |")
    lines.append(f"| dxmt stream stride changes | `{fmt_int(top_stream_strides)}` |")
    lines.append(f"| dxmt stream unique handles | `{fmt_int(top_stream_unique_handles)}` |")
    lines.append(
        f"| dxmt stream unique bytes | `{fmt_float(top_stream_unique_bytes / (1024.0 * 1024.0))} MiB` |"
    )
    lines.append(f"| dxmt IB handle changes | `{fmt_int(top_ib_handles)}` |")
    lines.append(f"| dxmt IB unique handles | `{fmt_int(top_ib_unique_handles)}` |")
    lines.append(
        f"| dxmt IB unique bytes | `{fmt_float(top_ib_unique_bytes / (1024.0 * 1024.0))} MiB` |"
    )
    lines.append(f"| dxmt PSO state samples | `{fmt_int(top_pso_samples)}` |")
    lines.append(
        f"| dxmt PSO state samples / draw | `{fmt_float(top_pso_samples_per_draw, 2)}` |"
    )
    lines.append(f"| dxmt PSO handle changes | `{fmt_int(top_pso_handles)}` |")
    lines.append(f"| dxmt blend state changes | `{fmt_int(top_blend_state_changes)}` |")
    lines.append(f"| dxmt blend state unique | `{fmt_int(top_blend_state_unique)}` |")
    lines.append(
        f"| dxmt blend-enabled no-op draws | `{fmt_int(top_blend_enabled_noop_draws)}` |"
    )
    lines.append(
        f"| dxmt constant-factor blend draws | `{fmt_int(top_blend_constant_factor_draws)}` |"
    )
    lines.append(f"| dxmt shader variant changes | `{fmt_int(top_shader_variants)}` |")
    lines.append(f"| dxmt VSOut layout changes | `{fmt_int(top_vsout_layouts)}` |")
    lines.append(f"| dxmt VSOut layout cache hits | `{fmt_int(top_vsout_cache_hits)}` |")
    lines.append(f"| dxmt VSOut layout cache misses | `{fmt_int(top_vsout_cache_misses)}` |")
    lines.append(f"| dxmt argbuf table bytes | `{fmt_float(argbuf_table_mib)} MiB` |")
    lines.append(f"| dxmt argbuf cbuf bytes | `{fmt_float(cbuf_mib)} MiB` |")
    lines.append(f"| dxmt setVertexBytes | `{fmt_float(set_vertex_bytes_mib)} MiB` |")
    lines.append(f"| dxmt CPU writer bytes | `{fmt_float(cpu_writer_mib)} MiB` |")
    lines.append(
        f"| unexplained Xcode buffer write | `{fmt_float(top_unexplained_buffer_write_mib)} MiB` |"
    )
    lines.append(
        f"| dxmt CPU writer / Xcode buffer write | `{fmt_float(cpu_writer_to_buffer_ratio, 3)}x` |"
    )
    lines.append(
        f"| unexplained / Xcode buffer write | `{fmt_float(unexplained_buffer_write_ratio, 3)}x` |"
    )
    lines.append(
        f"| dxmt cbuf / Xcode buffer write | `{fmt_float(cbuf_to_buffer_ratio, 3)}x` |"
    )
    lines.append(
        f"| dxmt transient / Xcode buffer write | `{fmt_float(transient_to_buffer_ratio, 3)}x` |"
    )
    if hint_summary:
        lines.append(f"| dxmt hint buckets | `{hint_summary}` |")
    if confidence_summary:
        lines.append(f"| dxmt hint confidence | `{confidence_summary}` |")
    if backend_class_summary:
        lines.append(f"| backend storage class | `{backend_class_summary}` |")
    if backend_probe_summary:
        lines.append(f"| backend next probe | `{backend_probe_summary}` |")
    if top_set_vertex_bytes:
        lines.append(
            f"| dxmt setVertexBytes slot5 | `{fmt_float(top_set_vertex_bytes_slot5 / (1024.0 * 1024.0))} MiB` |"
        )
        lines.append(
            f"| dxmt setVertexBytes other slots | `{fmt_float(top_set_vertex_bytes_other / (1024.0 * 1024.0))} MiB` |"
        )
    if top_cbuf_bytes:
        lines.append(
            f"| dxmt argbuf cbuf VS bytes | `{fmt_float(top_cbuf_vs_bytes / (1024.0 * 1024.0))} MiB` |"
        )
        lines.append(
            f"| dxmt argbuf cbuf FFP VS bytes | `{fmt_float(top_cbuf_ffp_vs_bytes / (1024.0 * 1024.0))} MiB` |"
        )
        lines.append(
            f"| dxmt argbuf cbuf PS bytes | `{fmt_float(top_cbuf_ps_bytes / (1024.0 * 1024.0))} MiB` |"
        )
        lines.append(
            f"| dxmt argbuf cbuf FFP PS bytes | `{fmt_float(top_cbuf_ffp_ps_bytes / (1024.0 * 1024.0))} MiB` |"
        )
    lines.append(f"| dxmt transient vertex/index bytes | `{fmt_float(transient_mib)} MiB` |")
    if top_transient_bytes:
        lines.append(
            f"| dxmt transient expanded vertex bytes | `{fmt_float(top_transient_vertex_expanded_bytes / (1024.0 * 1024.0))} MiB` |"
        )
        lines.append(
            f"| dxmt transient decl-fallback vertex bytes | `{fmt_float(top_transient_vertex_decl_fallback_bytes / (1024.0 * 1024.0))} MiB` |"
        )
        lines.append(
            f"| dxmt transient UP vertex bytes | `{fmt_float(top_transient_vertex_user_bytes / (1024.0 * 1024.0))} MiB` |"
        )
        lines.append(
            f"| dxmt transient UP index bytes | `{fmt_float(top_transient_index_user_bytes / (1024.0 * 1024.0))} MiB` |"
        )
        lines.append(
            f"| dxmt transient shadow index bytes | `{fmt_float(top_transient_index_shadow_bytes / (1024.0 * 1024.0))} MiB` |"
        )
    lines.append("")
    if len(hot_set) > len(top):
        lines.append("## Hot Set Aggregate")
        lines.append("")
        lines.append(
            f"Selected the top `{fmt_int(hot.get('encoder_count'))}` GPU-time encoders "
            f"to cover `{fmt_float(hot.get('gpu_share_pct'), 2)}%` of frame GPU time "
            f"(target `{fmt_float(hot_gpu_share_target, 2)}%`)."
        )
        lines.append("")
        lines.append("| Metric | Value |")
        lines.append("|---|---:|")
        lines.append(f"| Encoders | `{hot.get('encoder_keys', '')}` |")
        lines.append(f"| GPU time | `{fmt_float(hot.get('gpu_ms'))} ms` |")
        lines.append(f"| Buffer write | `{fmt_float(hot.get('buffer_write_mib'))} MiB` |")
        lines.append(f"| VS buffer write | `{fmt_float(hot.get('vs_buffer_write_mib'))} MiB` |")
        lines.append(
            f"| VS buffer / Xcode buffer write | `{fmt_float(hot.get('vs_buffer_write_share'), 3)}x` |"
        )
        lines.append(
            f"| VS invocations | `{fmt_int(hot.get('vs_invocations'))}` |"
        )
        lines.append(
            f"| VS buffer bytes / VS invocation | `{fmt_float(hot.get('vs_bytes_per_invocation'), 1)} B` |"
        )
        lines.append(
            f"| dxmt indexed references / unique estimate | "
            f"`{fmt_int(hot.get('indexed_refs'))} / {fmt_int(hot.get('indexed_unique'))}` |"
        )
        lines.append(
            f"| dxmt indexed reference reuse ratio | `{fmt_float(hot.get('indexed_reuse_ratio'), 3)}x` |"
        )
        lines.append(
            f"| VS invocations / dxmt indexed unique estimate | "
            f"`{fmt_float(hot.get('vs_invocations_per_indexed_unique'), 3)}x` |"
        )
        lines.append(
            f"| VS buffer bytes / dxmt indexed unique estimate | "
            f"`{fmt_float(hot.get('vs_bytes_per_indexed_unique'), 1)} B` |"
        )
        lines.append(
            f"| dxmt indexed cache miss estimate 64 | `{fmt_int(hot.get('cache_miss_64'))}` |"
        )
        lines.append(
            f"| cache miss / unique estimate 64 | "
            f"`{fmt_float(hot.get('cache_miss_64_over_unique'), 3)}x` |"
        )
        lines.append(
            f"| VS invocations / cache miss 64 | "
            f"`{fmt_float(hot.get('vs_invocations_per_cache_miss_64'), 3)}x` |"
        )
        lines.append(
            f"| VS buffer bytes / cache miss 64 | "
            f"`{fmt_float(hot.get('vs_bytes_per_cache_miss_64'), 1)} B` |"
        )
        lines.append(
            f"| Expected VSOut bytes / vertex | "
            f"`{fmt_float(hot.get('expected_vsout_bytes_per_vertex'), 1)} B` |"
        )
        lines.append(
            f"| VS buffer / expected VSOut | "
            f"`{fmt_float(hot.get('vs_buffer_to_expected_vsout'), 1)}x` |"
        )
        lines.append(
            f"| Named tiled buffer total | `{fmt_float(hot.get('named_tiled_buffer_mib'))} MiB` |"
        )
        lines.append(
            f"| Hidden backend write estimate | `{fmt_float(hot.get('hidden_backend_mib'))} MiB` |"
        )
        lines.append(
            f"| Hidden backend / VS buffer write | "
            f"`{fmt_float(hot.get('hidden_backend_ratio'), 3)}x` |"
        )
        lines.append(f"| dxmt CPU writer bytes | `{fmt_float(hot.get('cpu_writer_mib'))} MiB` |")
        if hot.get("hint_summary"):
            lines.append(f"| dxmt hint buckets | `{hot.get('hint_summary')}` |")
        if hot.get("backend_class_summary"):
            lines.append(f"| backend storage class | `{hot.get('backend_class_summary')}` |")
        if hot.get("backend_probe_summary"):
            lines.append(f"| backend next probe | `{hot.get('backend_probe_summary')}` |")
        lines.append("")
    lines.append("## Heuristic Notes")
    lines.append("")
    for note in notes:
        lines.append(f"- {note}")
    lines.append("")
    lines.append("## Hidden Backend Storage Classifier")
    lines.append("")
    lines.append(
        "The hidden estimate subtracts Xcode's named tiled vertex/primitive-block "
        "counters and dxmt-attributed CPU writer bytes from Xcode's VS buffer-write "
        "bucket. It is an attribution heuristic, not a separate hardware counter."
    )
    lines.append("")
    backend_header = [
        "seq", "enc", "VS buffer MiB", "named tiled MiB", "hidden est MiB",
        "hidden/VS", "VS B/prim", "VS/VSOut", "class", "confidence", "next probe",
    ]
    lines.append("| " + " | ".join(backend_header) + " |")
    lines.append("|" + "|".join("---:" for _ in backend_header) + "|")
    for row in joined[:10]:
        lines.append(
            "| "
            + " | ".join([
                str(row.get("seq", "")),
                str(row.get("enc", "")),
                fmt_float(row.get("vs_buffer_write_mib")),
                fmt_float(row.get("dxmt_named_tiled_buffer_mib")),
                fmt_float(row.get("dxmt_hidden_backend_write_mib")),
                fmt_float(row.get("dxmt_hidden_backend_write_ratio"), 3),
                fmt_float(row.get("vs_buffer_bytes_per_primitive"), 1),
                fmt_float(row.get("dxmt_vs_buffer_to_expected_stage_out_ratio"), 1),
                str(row.get("dxmt_backend_storage_class", "")),
                str(row.get("dxmt_backend_storage_confidence", "")),
                str(row.get("dxmt_backend_probe_hint", "")),
            ])
            + " |"
        )
    lines.append("")
    lines.append("## DXMT Encoder Writer/State Breakdown")
    lines.append("")
    breakdown_header = [
        "seq", "enc", "GPU ms", "draws", "prim/draw", "prim min/max",
        "vert/draw", "vert min/max", "large prim/vert", "baseV nz/neg/native", "baseV min/max",
        "split src/extra",
        "stream h/o/s", "stream h/o/s per draw",
        "IB hdl chg", "IB hdl/draw", "blend chg/uniq/noop/cf",
        "argbuf table KiB", "cbuf VS KiB",
        "cbuf FFPVS KiB", "cbuf PS KiB", "cbuf FFPPS KiB",
        "setVertexBytes KiB", "transient V KiB", "transient I KiB",
        "dxmt writer KiB", "writer/buffer", "unexplained MiB", "unexplained/buffer",
        "stream0 input MiB", "VS/input max",
        "VS inv/dxmt V", "VS hash", "VS source", "PS hash", "PS source", "VSOut key",
    ]
    lines.append("| " + " | ".join(breakdown_header) + " |")
    lines.append("|" + "|".join("---:" for _ in breakdown_header) + "|")
    for row in joined[:10]:
        draws = as_int(row.get("dxmt_draw_calls"))
        stream_handle = as_int(row.get("dxmt_stream_handle_changes"))
        stream_offset = as_int(row.get("dxmt_stream_offset_changes"))
        stream_stride = as_int(row.get("dxmt_stream_stride_changes"))
        stream_total = stream_handle + stream_offset + stream_stride
        ib_handle = as_int(row.get("dxmt_ib_handle_changes"))
        lines.append(
            "| "
            + " | ".join([
                str(row.get("seq", "")),
                str(row.get("enc", "")),
                fmt_float(row.get("gpu_ms")),
                fmt_int(draws),
                fmt_float(row.get("dxmt_primitives_per_draw"), 1),
                (
                    f"{fmt_int(row.get('dxmt_draw_primitive_min'))}/"
                    f"{fmt_int(row.get('dxmt_draw_primitive_max'))}"
                ),
                fmt_float(row.get("dxmt_vertices_per_draw"), 1),
                (
                    f"{fmt_int(row.get('dxmt_draw_vertex_min'))}/"
                    f"{fmt_int(row.get('dxmt_draw_vertex_max'))}"
                ),
                (
                    f"{fmt_float(row.get('dxmt_large_primitive_draw_share'), 2)}/"
                    f"{fmt_float(row.get('dxmt_large_vertex_draw_share'), 2)}"
                ),
                (
                    f"{fmt_int(row.get('dxmt_indexed_base_vertex_nonzero_draws'))}/"
                    f"{fmt_int(row.get('dxmt_indexed_base_vertex_negative_draws'))}/"
                    f"{fmt_int(row.get('dxmt_native_base_vertex_used_draws'))}"
                ),
                (
                    f"{fmt_int(row.get('dxmt_indexed_base_vertex_min'))}/"
                    f"{fmt_int(row.get('dxmt_indexed_base_vertex_max'))}"
                ),
                (
                    f"{fmt_int(row.get('dxmt_split_large_indexed_source_draws'))}/"
                    f"{fmt_int(row.get('dxmt_split_large_indexed_extra_draws'))}"
                ),
                f"{fmt_int(stream_handle)}/{fmt_int(stream_offset)}/{fmt_int(stream_stride)}",
                fmt_float(stream_total / draws if draws else 0.0, 2),
                fmt_int(ib_handle),
                fmt_float(ib_handle / draws if draws else 0.0, 2),
                (
                    f"{fmt_int(row.get('dxmt_blend_state_changes'))}/"
                    f"{fmt_int(row.get('dxmt_blend_state_unique'))}/"
                    f"{fmt_int(row.get('dxmt_blend_enabled_noop_draws'))}/"
                    f"{fmt_int(row.get('dxmt_blend_constant_factor_draws'))}"
                ),
                fmt_float(as_int(row.get("dxmt_argbuf_table_bytes")) / 1024.0, 1),
                fmt_float(as_int(row.get("dxmt_argbuf_cbuf_vs_bytes")) / 1024.0, 1),
                fmt_float(as_int(row.get("dxmt_argbuf_cbuf_ffp_vs_bytes")) / 1024.0, 1),
                fmt_float(as_int(row.get("dxmt_argbuf_cbuf_ps_bytes")) / 1024.0, 1),
                fmt_float(as_int(row.get("dxmt_argbuf_cbuf_ffp_ps_bytes")) / 1024.0, 1),
                fmt_float(as_int(row.get("dxmt_set_vertex_bytes_bytes")) / 1024.0, 1),
                fmt_float(as_int(row.get("dxmt_transient_vertex_bytes")) / 1024.0, 1),
                fmt_float(as_int(row.get("dxmt_transient_index_bytes")) / 1024.0, 1),
                fmt_float(as_int(row.get("dxmt_cpu_writer_bytes")) / 1024.0, 1),
                fmt_float(row.get("dxmt_cpu_writer_to_buffer_write_ratio"), 3),
                fmt_float(row.get("dxmt_unexplained_buffer_write_mib")),
                fmt_float(row.get("dxmt_unexplained_buffer_write_ratio"), 3),
                (
                    f"{fmt_float(row.get('dxmt_stream0_input_min_mib'))}/"
                    f"{fmt_float(row.get('dxmt_stream0_input_max_mib'))}"
                ),
                fmt_float(row.get("dxmt_vs_buffer_to_stream0_input_max_ratio"), 1),
                fmt_float(row.get("dxmt_vs_invocations_per_dxmt_vertex"), 3),
                str(row.get("dxmt_vertex_shader_last", "")),
                str(row.get("dxmt_vertex_shader_source_last", "")),
                str(row.get("dxmt_pixel_shader_last", "")),
                str(row.get("dxmt_pixel_shader_source_last", "")),
                str(row.get("dxmt_vsout_layout_last", "")),
            ])
            + " |"
        )
    lines.append("")
    lines.append("## DXMT Indexed Triangle State Class Breakdown")
    lines.append("")
    lines.append(
        "These buckets are not mutually exclusive. They split indexed triangle-list "
        "draws by backend-relevant state so row/material probes can target the "
        "geometry that actually dominates a hot encoder."
    )
    lines.append("")
    state_header = [
        "seq", "enc", "GPU ms", "VS write MiB",
        "opaque dw d/p/v", "depth-read d/p/v", "alpha-blend d/p/v",
        "scissor d/p/v", "textured d/p/v", "large4096 d/p/v",
        "large4096 opaque d/p/v", "large4096 depth-read d/p/v",
        "large4096 alpha d/p/v", "large4096 scissor d/p/v",
        "large4096 textured d/p/v",
    ]
    lines.append("| " + " | ".join(state_header) + " |")
    lines.append("|" + "|".join("---:" for _ in state_header) + "|")

    def state_triplet(row: dict[str, Any], prefix: str) -> str:
        return (
            f"{fmt_int(row.get(prefix + '_draws'))}/"
            f"{fmt_int(row.get(prefix + '_primitives'))}/"
            f"{fmt_int(row.get(prefix + '_vertices'))}"
        )

    for row in joined[:10]:
        lines.append(
            "| "
            + " | ".join([
                str(row.get("seq", "")),
                str(row.get("enc", "")),
                fmt_float(row.get("gpu_ms")),
                fmt_float(row.get("vs_buffer_write_mib")),
                state_triplet(row, "dxmt_indexed_triangle_opaque_depth_write"),
                state_triplet(row, "dxmt_indexed_triangle_depth_read"),
                state_triplet(row, "dxmt_indexed_triangle_alpha_blend"),
                state_triplet(row, "dxmt_indexed_triangle_scissor"),
                state_triplet(row, "dxmt_indexed_triangle_textured"),
                state_triplet(row, "dxmt_indexed_triangle_large_4096"),
                state_triplet(row, "dxmt_indexed_triangle_large_4096_opaque_depth_write"),
                state_triplet(row, "dxmt_indexed_triangle_large_4096_depth_read"),
                state_triplet(row, "dxmt_indexed_triangle_large_4096_alpha_blend"),
                state_triplet(row, "dxmt_indexed_triangle_large_4096_scissor"),
                state_triplet(row, "dxmt_indexed_triangle_large_4096_textured"),
            ])
            + " |"
        )
    lines.append("")
    if dxmt_streams is not None:
        lines.append("## DXMT Per-Stream Breakdown")
        lines.append("")
        stream_header = [
            "seq", "enc", "stream", "samples", "metal binds",
            "bind first/h/o", "unique handles", "unique KiB", "h/o/s changes",
            "last h/o/s", "dyn", "writeonly", "default", "managed",
            "systemmem", "scratch",
        ]
        lines.append("| " + " | ".join(stream_header) + " |")
        lines.append("|" + "|".join("---:" for _ in stream_header) + "|")
        emitted = 0
        for row in joined[:10]:
            seq = row.get("seq")
            enc = row.get("enc")
            if not isinstance(seq, int) or not isinstance(enc, int):
                continue
            for stream in dxmt_streams.get((seq, enc), []):
                emitted += 1
                lines.append(
                    "| "
                    + " | ".join([
                        str(seq),
                        str(enc),
                        str(stream.get("stream", "")),
                        fmt_int(stream.get("samples")),
                        fmt_int(stream.get("metal_binds")),
                        (
                            f"{fmt_int(stream.get('metal_bind_firsts'))}/"
                            f"{fmt_int(stream.get('metal_bind_handle_changes'))}/"
                            f"{fmt_int(stream.get('metal_bind_offset_changes'))}"
                        ),
                        fmt_int(stream.get("unique_handles")),
                        fmt_float(as_int(stream.get("unique_bytes")) / 1024.0, 1),
                        (
                            f"{fmt_int(stream.get('handle_changes'))}/"
                            f"{fmt_int(stream.get('offset_changes'))}/"
                            f"{fmt_int(stream.get('stride_changes'))}"
                        ),
                        (
                            f"{stream.get('last_handle', '')}/"
                            f"{stream.get('last_offset', '')}/"
                            f"{stream.get('last_stride', '')}"
                        ),
                        fmt_int(stream.get("unique_dynamic_handles")),
                        fmt_int(stream.get("unique_writeonly_handles")),
                        fmt_int(stream.get("unique_default_pool_handles")),
                        fmt_int(stream.get("unique_managed_pool_handles")),
                        fmt_int(stream.get("unique_systemmem_pool_handles")),
                        fmt_int(stream.get("unique_scratch_pool_handles")),
                    ])
                    + " |"
                )
        if emitted == 0:
            lines.append("| n/a | n/a | n/a | 0 | 0 | 0/0/0 | 0 | 0.0 | 0/0/0 | n/a | 0 | 0 | 0 | 0 | 0 | 0 |")
        lines.append("")
    lines.append("## Top Encoders")
    lines.append("")
    top_header = [
        "seq", "enc", "RT fmt/size usage/view", "Depth fmt/size usage/view",
        "X8 RT tex samples", "X8 alpha fill", "X8 RT tex mask/last", "GPU ms", "GPU %",
        "buffer write MiB",
        "device write MiB", "VS buffer MiB", "VS B/VS inv", "VS B/frag",
        "VS B/prim", "VS B/postclip", "VS B/primTile", "VS B/pixel",
        "VS/tiled", "VSOut B/V", "VS/VSOut",
        "VS LLC MiB",
        "vertex stage %", "VS ALU limiter %", "VS write limiter %",
        "shaded vertex read %", "cull %", "clip %",
        "tile intersects", "tiling util %", "prim/tile",
        "FS buffer MiB", "varyings/fragment", "buffer write limiter %",
        "LLC limiter %", "MMU limiter %", "draws", "FFP", "preT",
        "cull n/f/b", "depth e/w", "scissor", "alpha b/t/e",
        "blend chg/uniq/noop/cf", "clip planes",
        "dxmt vertices", "dxmt tris", "prim/draw", "prim min/max",
        "vert/draw", "vert min/max", "large prim/vert",
        "geom sig uniq/dup", "geom dup",
        "stream hdl chg", "stream off chg",
        "stream stride chg", "stream unique", "stream unique KiB",
        "stream0 h/o/s", "IB hdl chg", "IB unique", "IB unique KiB",
        "IB last", "PSO handle", "shader variant", "VS hash", "PS hash", "VSOut key",
        "VS source", "PS source",
        "PSO samples/draw", "VSOut changes", "VSOut cache h/m",
        "argbuf table KiB", "argbuf cbuf KiB",
        "setVertexBytes KiB", "transient KiB", "dxmt writer KiB",
        "writer/buffer", "unexplained MiB", "unexplained/buffer", "VS share",
        "hint", "confidence",
    ]
    lines.append("| " + " | ".join(top_header) + " |")
    lines.append("|" + "|".join("---:" for _ in top_header) + "|")
    for row in joined[:10]:
        def attachment_summary(prefix: str) -> str:
            fmt = as_int(row.get(f"dxmt_{prefix}_format"))
            width = as_int(row.get(f"dxmt_{prefix}_width"))
            height = as_int(row.get(f"dxmt_{prefix}_height"))
            usage = row.get(f"dxmt_{prefix}_texture_usage", "")
            needs_view = fmt_int(row.get(f"dxmt_{prefix}_texture_needs_shader_read_view"))
            swizzle = fmt_int(row.get(f"dxmt_{prefix}_format_swizzle"))
            alias = row.get(f"dxmt_{prefix}_alias_texture", "")
            if fmt == 0 and width == 0 and height == 0:
                return ""
            return f"fmt{fmt} {width}x{height} alias={alias} usage={usage} swz/view={swizzle}/{needs_view}"

        stream_unique_kib = as_int(row.get("dxmt_stream_unique_bytes")) / 1024.0
        ib_unique_kib = as_int(row.get("dxmt_ib_unique_bytes")) / 1024.0
        argbuf_table_kib = as_int(row.get("dxmt_argbuf_table_bytes")) / 1024.0
        cbuf_kib = as_int(row.get("dxmt_argbuf_cbuf_bytes")) / 1024.0
        set_vertex_bytes_kib = as_int(row.get("dxmt_set_vertex_bytes_bytes")) / 1024.0
        transient_kib = (
            as_int(row.get("dxmt_transient_vertex_bytes")) +
            as_int(row.get("dxmt_transient_index_bytes"))
        ) / 1024.0
        dxmt_writer_kib = as_int(row.get("dxmt_cpu_writer_bytes")) / 1024.0
        lines.append(
            "| "
            + " | ".join([
                str(row.get("seq", "")),
                str(row.get("enc", "")),
                attachment_summary("rt"),
                attachment_summary("depth"),
                fmt_int(row.get("dxmt_x8_rt_texture_binding_samples")),
                (
                    f"{fmt_int(row.get('dxmt_x8_shader_alpha_fill_samples'))}/"
                    f"{row.get('dxmt_x8_shader_alpha_fill_mask_or', '')}"
                ),
                (
                    f"{row.get('dxmt_x8_rt_texture_binding_mask_or', '')}/"
                    f"{row.get('dxmt_x8_rt_texture_binding_last_stage', '')}/"
                    f"{row.get('dxmt_x8_rt_texture_binding_last_handle', '')}"
                ),
                fmt_float(row.get("gpu_ms")),
                fmt_float(row.get("gpu_share_pct"), 2),
                fmt_float(row.get("buffer_write_mib")),
                fmt_float(row.get("device_write_mib")),
                fmt_float(row.get("vs_buffer_write_mib")),
                fmt_float(row.get("vs_buffer_bytes_per_vs_invocation"), 1),
                fmt_float(row.get("vs_buffer_bytes_per_fragment"), 1),
                fmt_float(row.get("vs_buffer_bytes_per_primitive"), 1),
                fmt_float(row.get("vs_buffer_bytes_per_post_clipped_primitive"), 1),
                fmt_float(row.get("vs_buffer_bytes_per_primitive_tile_estimate"), 1),
                fmt_float(row.get("vs_buffer_bytes_per_pixel"), 1),
                fmt_float(row.get("vs_buffer_to_tiled_buffer_ratio"), 1),
                fmt_int(row.get("dxmt_vsout_expected_stage_out_bytes_per_vertex")),
                fmt_float(row.get("dxmt_vs_buffer_to_expected_stage_out_ratio"), 1),
                fmt_float(row.get("vs_llc_write_mib")),
                fmt_float(row.get("vertex_stage_time_pct"), 2),
                fmt_float(row.get("vs_alu_limiter_pct"), 2),
                fmt_float(row.get("vs_buffer_write_limiter_pct"), 2),
                fmt_float(row.get("shaded_vertex_read_limiter_pct"), 2),
                fmt_float(row.get("cull_unit_limiter_pct"), 2),
                fmt_float(row.get("clip_unit_limiter_pct"), 2),
                fmt_float(row.get("primitive_block_tile_intersections_pct"), 2),
                fmt_float(row.get("tiling_block_utilization_pct"), 2),
                fmt_float(row.get("primitives_per_tile"), 1),
                fmt_float(row.get("fs_buffer_write_mib")),
                fmt_float(row.get("varyings_per_fragment")),
                fmt_float(row.get("buffer_write_limiter_pct"), 2),
                fmt_float(row.get("llc_limiter_pct"), 2),
                fmt_float(row.get("mmu_limiter_pct"), 2),
                fmt_int(row.get("dxmt_draw_calls")),
                fmt_int(row.get("dxmt_ffp_draws")),
                fmt_int(row.get("dxmt_pretransformed_draws")),
                (
                    f"{fmt_int(row.get('dxmt_cull_none_draws'))}/"
                    f"{fmt_int(row.get('dxmt_cull_front_draws'))}/"
                    f"{fmt_int(row.get('dxmt_cull_back_draws'))}"
                ),
                (
                    f"{fmt_int(row.get('dxmt_depth_enabled_draws'))}/"
                    f"{fmt_int(row.get('dxmt_depth_write_draws'))}"
                ),
                fmt_int(row.get("dxmt_scissor_enabled_draws")),
                (
                    f"{fmt_int(row.get('dxmt_alpha_blend_enabled_draws'))}/"
                    f"{fmt_int(row.get('dxmt_alpha_test_enabled_draws'))}/"
                    f"{fmt_int(row.get('dxmt_alpha_test_effective_draws'))}"
                ),
                (
                    f"{fmt_int(row.get('dxmt_blend_state_changes'))}/"
                    f"{fmt_int(row.get('dxmt_blend_state_unique'))}/"
                    f"{fmt_int(row.get('dxmt_blend_enabled_noop_draws'))}/"
                    f"{fmt_int(row.get('dxmt_blend_constant_factor_draws'))}"
                ),
                fmt_int(row.get("dxmt_clip_plane_enabled_draws")),
                fmt_int(row.get("dxmt_vertex_count")),
                fmt_int(row.get("dxmt_triangle_estimate")),
                fmt_float(row.get("dxmt_primitives_per_draw"), 1),
                (
                    f"{fmt_int(row.get('dxmt_draw_primitive_min'))}/"
                    f"{fmt_int(row.get('dxmt_draw_primitive_max'))}"
                ),
                fmt_float(row.get("dxmt_vertices_per_draw"), 1),
                (
                    f"{fmt_int(row.get('dxmt_draw_vertex_min'))}/"
                    f"{fmt_int(row.get('dxmt_draw_vertex_max'))}"
                ),
                (
                    f"{fmt_float(row.get('dxmt_large_primitive_draw_share'), 2)}/"
                    f"{fmt_float(row.get('dxmt_large_vertex_draw_share'), 2)}"
                ),
                (
                    f"{fmt_int(row.get('dxmt_draw_geometry_signature_unique'))}/"
                    f"{fmt_int(row.get('dxmt_draw_geometry_signature_duplicates'))}"
                ),
                fmt_float(row.get("dxmt_draw_geometry_signature_duplicate_ratio"), 2),
                fmt_int(row.get("dxmt_stream_handle_changes")),
                fmt_int(row.get("dxmt_stream_offset_changes")),
                fmt_int(row.get("dxmt_stream_stride_changes")),
                fmt_int(row.get("dxmt_stream_unique_handles")),
                fmt_float(stream_unique_kib, 1),
                (
                    f"{row.get('dxmt_stream0_last_handle', '')}/"
                    f"{row.get('dxmt_stream0_last_offset', '')}/"
                    f"{row.get('dxmt_stream0_last_stride', '')}"
                ),
                fmt_int(row.get("dxmt_ib_handle_changes")),
                fmt_int(row.get("dxmt_ib_unique_handles")),
                fmt_float(ib_unique_kib, 1),
                str(row.get("dxmt_ib_last_handle", "")),
                fmt_int(row.get("dxmt_pso_handle_changes")),
                str(row.get("dxmt_shader_variant_last", "")),
                str(row.get("dxmt_vertex_shader_last", "")),
                str(row.get("dxmt_pixel_shader_last", "")),
                str(row.get("dxmt_vsout_layout_last", "")),
                str(row.get("dxmt_vertex_shader_source_last", "")),
                str(row.get("dxmt_pixel_shader_source_last", "")),
                fmt_float(row.get("dxmt_pso_state_samples_per_draw"), 2),
                fmt_int(row.get("dxmt_vsout_layout_changes")),
                (
                    f"{fmt_int(row.get('dxmt_vsout_layout_cache_hits'))}/"
                    f"{fmt_int(row.get('dxmt_vsout_layout_cache_misses'))}"
                ),
                fmt_float(argbuf_table_kib, 1),
                fmt_float(cbuf_kib, 1),
                fmt_float(set_vertex_bytes_kib, 1),
                fmt_float(transient_kib, 1),
                fmt_float(dxmt_writer_kib, 1),
                fmt_float(row.get("dxmt_cpu_writer_to_buffer_write_ratio"), 3),
                fmt_float(row.get("dxmt_unexplained_buffer_write_mib")),
                fmt_float(row.get("dxmt_unexplained_buffer_write_ratio"), 3),
                fmt_float(row.get("dxmt_vs_buffer_write_share"), 3),
                str(row.get("dxmt_gpu_write_hint", "")),
                str(row.get("dxmt_write_owner_confidence", "")),
            ])
            + " |"
        )
    lines.append("")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def default_output_paths(xcode_csv: Path) -> tuple[Path, Path, Path]:
    name = xcode_csv.name
    if name.endswith("-counters-xcode.csv"):
        stem = name[:-len("-counters-xcode.csv")]
        return (
            xcode_csv.with_name(f"{stem}-counters-summary.csv"),
            xcode_csv.with_name(f"{stem}-xcode-dxmt-joined-summary.csv"),
            xcode_csv.with_name(f"{stem}-xcode-dxmt-bottleneck-report.md"),
        )
    return (
        xcode_csv.with_name(f"{xcode_csv.stem}-summary.csv"),
        xcode_csv.with_name(f"{xcode_csv.stem}-joined-summary.csv"),
        xcode_csv.with_name(f"{xcode_csv.stem}-bottleneck-report.md"),
    )


def check_top_pso_attribution(joined: list[dict[str, Any]],
                              top_count: int,
                              min_samples_per_draw: float) -> list[str]:
    top = joined[:top_count]
    draws = sum(as_int(row.get("dxmt_draw_calls")) for row in top)
    samples = sum(as_int(row.get("dxmt_pso_state_samples")) for row in top)
    if draws <= 0:
        return [
            "top encoder rows have no dxmt draw attribution; "
            "ensure DXMT9_PERF_ENCODER_BREAKDOWN=1 and join by RenderPass label"
        ]
    samples_per_draw = samples / draws
    if samples_per_draw < min_samples_per_draw:
        return [
            "top encoder PSO/VSOut attribution coverage is too low "
            f"({samples_per_draw:.3f} samples/draw, expected >= "
            f"{min_samples_per_draw:.3f}; samples={samples}, draws={draws})"
        ]
    return []


def check_top_dxmt_join_coverage(joined: list[dict[str, Any]],
                                 top_count: int,
                                 min_joined_fraction: float) -> list[str]:
    top = joined[:top_count]
    if not top:
        return ["no top encoder rows are available for dxmt join coverage"]
    labeled = [
        row for row in top
        if isinstance(row.get("seq"), int) and isinstance(row.get("enc"), int)
    ]
    joined_rows = [
        row for row in labeled
        if as_int(row.get("dxmt_draw_calls")) > 0
    ]
    if not labeled:
        return [
            "top Xcode encoder rows do not have RenderPass[seq=...,enc=...] labels"
        ]
    joined_fraction = len(joined_rows) / len(labeled)
    if joined_fraction < min_joined_fraction:
        return [
            "top encoder dxmt join coverage is too low "
            f"({joined_fraction:.3f}, expected >= {min_joined_fraction:.3f}; "
            f"joined={len(joined_rows)}, labeled={len(labeled)})"
        ]
    return []


def check_xcode_counter_coverage(xcode_csv: Path,
                                 summaries: list[dict[str, Any]]) -> list[str]:
    header, _ = read_csv_rows(xcode_csv)
    missing = missing_required_xcode_columns(header)
    failures = []
    if missing:
        failures.append(
            "Xcode encoder counter CSV is missing required columns: " +
            ", ".join(missing)
        )
    if not summaries:
        failures.append("Xcode encoder counter CSV has no encoder rows")
        return failures
    total_gpu_ms = sum(as_float(row.get("gpu_ms")) for row in summaries)
    if total_gpu_ms <= 0.0:
        failures.append("Xcode encoder counter CSV has no positive GPU Time")
    labeled_rows = [
        row for row in summaries
        if isinstance(row.get("seq"), int) and isinstance(row.get("enc"), int)
    ]
    if not labeled_rows:
        failures.append(
            "Xcode encoder labels do not include RenderPass[seq=...,enc=...] rows"
        )
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("xcode_csv", type=Path, help="Xcode Export Encoder Counters CSV")
    parser.add_argument("--dxmt-log", type=Path, help="dxmt9.log containing [dxmt9-perf-encoder] lines")
    parser.add_argument("--dxmt-encoders-csv", type=Path, help="CSV from summarize_3dmark05_perf.py")
    parser.add_argument("--dxmt-streams-csv", type=Path, help="stream CSV from summarize_3dmark05_perf.py")
    parser.add_argument("--run-label", default="xcode", help="run label written into output rows")
    parser.add_argument("--summary-output", type=Path, help="reduced Xcode summary CSV path")
    parser.add_argument("--joined-output", type=Path, help="Xcode+dxmt joined CSV path")
    parser.add_argument("--report-output", type=Path, help="Markdown bottleneck report path")
    parser.add_argument(
        "--require-top-pso-attribution",
        action="store_true",
        help="exit nonzero unless top encoder rows have PSO/VSOut samples near draw frequency",
    )
    parser.add_argument(
        "--require-xcode-counter-coverage",
        action="store_true",
        help="exit nonzero unless the Xcode CSV has the required counter columns and labeled rows",
    )
    parser.add_argument(
        "--require-dxmt-join-coverage",
        action="store_true",
        help="exit nonzero unless top Xcode encoder rows join to dxmt encoder attribution",
    )
    parser.add_argument(
        "--min-top-dxmt-joined-fraction",
        type=float,
        default=1.0,
        help="minimum labeled top-row fraction with dxmt draw attribution when --require-dxmt-join-coverage is set",
    )
    parser.add_argument(
        "--min-top-pso-samples-per-draw",
        type=float,
        default=0.90,
        help="minimum top-row dxmt_pso_state_samples/draw when --require-top-pso-attribution is set",
    )
    parser.add_argument(
        "--top",
        type=int,
        default=3,
        help="number of GPU-time-ranked encoder rows used by attribution coverage checks",
    )
    parser.add_argument(
        "--hot-gpu-share",
        type=float,
        default=95.0,
        help="GPU-time share target for the report's Hot Set Aggregate",
    )
    args = parser.parse_args()

    summary_output, joined_output, report_output = default_output_paths(args.xcode_csv)
    if args.summary_output:
        summary_output = args.summary_output
    if args.joined_output:
        joined_output = args.joined_output
    if args.report_output:
        report_output = args.report_output

    summaries = summarize_xcode(args.xcode_csv, args.run_label)
    summaries.sort(key=lambda row: float(row.get("gpu_ms", 0.0)), reverse=True)
    if args.require_xcode_counter_coverage:
        failures = check_xcode_counter_coverage(args.xcode_csv, summaries)
        if failures:
            for failure in failures:
                print(f"requirement failed: {failure}", file=sys.stderr)
            return 1
    dxmt = load_dxmt(args)
    dxmt_streams = (
        load_dxmt_streams_from_csv(args.dxmt_streams_csv)
        if args.dxmt_streams_csv else None
    )
    joined = [join_dxmt(row, dxmt) for row in summaries]

    write_rows(summary_output, summaries, SUMMARY_FIELDS)
    write_rows(joined_output, joined, SUMMARY_FIELDS + JOINED_EXTRA_FIELDS)
    write_report(report_output, joined, args.run_label, dxmt_streams, args.hot_gpu_share)

    print(summary_output)
    print(joined_output)
    print(report_output)
    if args.require_top_pso_attribution:
        failures = check_top_pso_attribution(
            joined, args.top, args.min_top_pso_samples_per_draw)
        if failures:
            for failure in failures:
                print(f"requirement failed: {failure}", file=sys.stderr)
            return 1
    if args.require_dxmt_join_coverage:
        failures = check_top_dxmt_join_coverage(
            joined, args.top, args.min_top_dxmt_joined_fraction)
        if failures:
            for failure in failures:
                print(f"requirement failed: {failure}", file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
