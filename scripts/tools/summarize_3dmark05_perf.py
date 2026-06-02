#!/usr/bin/env python3
"""Summarize a 3DMark05 dxmt9 perf output directory.

The script consumes `result.json` and, when present, `[dxmt9-perf-encoder]`
lines from `dxmt9.log`. If a diagnostic run is interrupted before result.json
is written, it can synthesize a partial result from the final `[dxmt9-perf]`
line. It is intentionally narrow: the output is a compact triage report for
the GT1 bottleneck work tracked in specs/perfomance.plan.md.
"""

from __future__ import annotations

import argparse
import csv
import json
import re
import sys
from pathlib import Path
from typing import Any

KEY_VALUE_RE = re.compile(r"\b([A-Za-z0-9_]+)=([^\s]+)")
ENCODER_PREFIX = "[dxmt9-perf-encoder "
STREAM_PREFIX = "[dxmt9-perf-encoder-stream "
PROBE_DRAW_PREFIX = "[dxmt9-perf-indexed-probe-draw "
PERF_PREFIX = "[dxmt9-perf] "
BRIDGE_PREFIX = "[dxmt9-bridge-perf] "

RUN_COUNTERS = (
    "present_encoded",
    "draw_calls",
    "draw_indexed",
    "draw_expanded_indexed",
    "render_pass_begin",
    "render_pass_end",
    "render_pass_load_action_load",
    "render_pass_load_action_clear",
    "render_pass_load_action_dontcare",
    "render_pass_load_action_depth_load",
    "render_pass_load_action_depth_clear",
    "render_pass_load_action_depth_dontcare",
    "render_pass_load_action_stencil_load",
    "render_pass_load_action_stencil_clear",
    "render_pass_load_action_stencil_dontcare",
    "render_pass_store_action_store",
    "render_pass_store_action_dontcare",
    "render_pass_store_action_resolve",
    "render_pass_store_action_depth_store",
    "render_pass_store_action_depth_dontcare",
    "render_pass_store_action_stencil_store",
    "render_pass_store_action_stencil_dontcare",
    "render_pass_tile_preservation_bytes",
    "render_pass_same_key_adjacent",
    "render_pass_same_key_reentry",
    "render_pass_same_key_reentry_preservation_bytes",
    "render_pass_same_key_reentry_color_preservation_bytes",
    "render_pass_same_key_reentry_depth_preservation_bytes",
    "render_pass_transition_rt_change_same_depth",
    "render_pass_transition_same_rt_depth_change",
    "render_pass_transition_rt_depth_change",
    "render_pass_color_proof_allow_next_clear",
    "render_pass_color_proof_allow_dead_no_present",
    "render_pass_color_proof_block_null_color",
    "render_pass_color_proof_block_no_lookahead",
    "render_pass_color_proof_block_draw_target",
    "render_pass_color_proof_block_texture_sample",
    "render_pass_color_proof_block_surface_copy",
    "render_pass_color_proof_block_stretch_rect",
    "render_pass_color_proof_block_readback",
    "render_pass_color_proof_block_color_fill",
    "render_pass_color_proof_block_msaa_resolve",
    "render_pass_color_proof_block_present",
    "render_pass_color_proof_block_dead_no_present_disabled",
    "render_pass_depth_proof_allow_next_clear",
    "render_pass_depth_proof_allow_dead_no_present",
    "render_pass_depth_proof_block_null_depth",
    "render_pass_depth_proof_block_no_lookahead",
    "render_pass_depth_proof_block_msaa_resolve",
    "render_pass_depth_proof_block_draw_depth",
    "render_pass_depth_proof_block_shadow_sample",
    "render_pass_depth_proof_block_surface_copy",
    "render_pass_depth_proof_block_stretch_rect",
    "render_pass_depth_proof_block_readback",
    "render_pass_depth_proof_block_color_fill",
    "render_pass_depth_proof_block_depth_resolve",
    "render_pass_depth_proof_block_present",
    "commit_chunk_draw_run_scans",
    "commit_chunk_draw_run_submits",
    "commit_chunk_draw_run_records",
    "commit_chunk_draw_run_binding_override_records",
    "commit_chunk_draw_run_binding_override_bytes",
    "commit_chunk_draw_run_binding_override_stream_records",
    "commit_chunk_draw_run_binding_override_ib_records",
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
    "commit_chunk_draw_delta_stream_handle",
    "commit_chunk_draw_delta_stream_offset",
    "commit_chunk_draw_delta_stream_stride",
    "commit_chunk_draw_delta_ib",
    "commit_chunk_draw_delta_ib_handle",
    "argbuf_hybrid_bytes_per_encoder",
    "transient_upload_bytes",
    "transient_upload_cpu_ms",
    "encode_draw_cpu_ms",
    "submit_draw_cpu_ms",
    "encode_draw_stream_bind_cpu_ms",
    "gpu_command_buffer_time_ms",
    "completion_wait_ms",
    "map_buffer_wait_ms",
    "queue_sequence_wait_ms",
)

ENCODER_SUM_KEYS = (
    "draw_calls",
    "indexed_draws",
    "expanded_indexed_draws",
    "ffp_draws",
    "programmable_draws",
    "pretransformed_draws",
    "textured_draws",
    "cull_none_draws",
    "cull_front_draws",
    "cull_back_draws",
    "fill_solid_draws",
    "fill_wireframe_draws",
    "depth_enabled_draws",
    "depth_write_draws",
    "depth_func_less_draws",
    "depth_func_lessequal_draws",
    "depth_func_always_draws",
    "depth_func_other_draws",
    "scissor_enabled_draws",
    "alpha_blend_enabled_draws",
    "blend_state_samples",
    "blend_state_changes",
    "blend_state_unique",
    "blend_state_unique_overflows",
    "blend_state_last",
    "blend_enabled_noop_draws",
    "blend_constant_factor_draws",
    "alpha_test_enabled_draws",
    "alpha_test_effective_draws",
    "clip_plane_enabled_draws",
    "primitive_count",
    "triangle_estimate",
    "vertex_count",
    "indexed_triangle_opaque_depth_write_draws",
    "indexed_triangle_opaque_depth_write_primitives",
    "indexed_triangle_opaque_depth_write_vertices",
    "indexed_triangle_depth_read_draws",
    "indexed_triangle_depth_read_primitives",
    "indexed_triangle_depth_read_vertices",
    "indexed_triangle_alpha_blend_draws",
    "indexed_triangle_alpha_blend_primitives",
    "indexed_triangle_alpha_blend_vertices",
    "indexed_triangle_scissor_draws",
    "indexed_triangle_scissor_primitives",
    "indexed_triangle_scissor_vertices",
    "indexed_triangle_textured_draws",
    "indexed_triangle_textured_primitives",
    "indexed_triangle_textured_vertices",
    "indexed_triangle_large_4096_draws",
    "indexed_triangle_large_4096_primitives",
    "indexed_triangle_large_4096_vertices",
    "indexed_triangle_large_4096_opaque_depth_write_draws",
    "indexed_triangle_large_4096_opaque_depth_write_primitives",
    "indexed_triangle_large_4096_opaque_depth_write_vertices",
    "indexed_triangle_large_4096_depth_read_draws",
    "indexed_triangle_large_4096_depth_read_primitives",
    "indexed_triangle_large_4096_depth_read_vertices",
    "indexed_triangle_large_4096_alpha_blend_draws",
    "indexed_triangle_large_4096_alpha_blend_primitives",
    "indexed_triangle_large_4096_alpha_blend_vertices",
    "indexed_triangle_large_4096_scissor_draws",
    "indexed_triangle_large_4096_scissor_primitives",
    "indexed_triangle_large_4096_scissor_vertices",
    "indexed_triangle_large_4096_textured_draws",
    "indexed_triangle_large_4096_textured_primitives",
    "indexed_triangle_large_4096_textured_vertices",
    "draw_geometry_signature_samples",
    "draw_geometry_signature_unique",
    "draw_geometry_signature_unique_overflows",
    "draw_geometry_signature_duplicates",
    "draw_geometry_signature_consecutive_duplicates",
    "indexed_base_vertex_samples",
    "indexed_base_vertex_nonzero_draws",
    "indexed_base_vertex_negative_draws",
    "indexed_base_vertex_positive_draws",
    "native_base_vertex_requested_draws",
    "native_base_vertex_used_draws",
    "native_base_vertex_skipped_negative_draws",
    "split_large_indexed_source_draws",
    "split_large_indexed_metal_draws",
    "split_large_indexed_extra_draws",
    "split_large_indexed_stream0_span_limit",
    "split_large_indexed_chunk_stream0_span_max",
    "split_large_indexed_primitive_count",
    "indexed_order_probe_draws",
    "indexed_order_probe_skipped",
    "indexed_order_probe_bytes",
    "indexed_order_optimized_draws",
    "indexed_order_optimized_skipped",
    "indexed_order_optimized_bytes",
    "probe_scissor_rect_draws",
    "probe_scissor_rect_skipped",
    "probe_scissor_rect_area_delta_pixels",
    "probe_disable_alpha_blend_draws",
    "probe_disable_depth_write_draws",
    "probe_depth_func_always_draws",
    "indexed_vertex_reuse_samples",
    "indexed_vertex_reuse_skipped",
    "indexed_vertex_reference_count",
    "indexed_unique_vertex_estimate",
    "indexed_vertex_cache_miss_estimate_16",
    "indexed_vertex_cache_miss_estimate_32",
    "indexed_vertex_cache_miss_estimate_64",
    "stream_state_samples",
    "stream_metal_binds",
    "stream_metal_bind_firsts",
    "stream_metal_bind_handle_changes",
    "stream_metal_bind_offset_changes",
    "stream_unique_handles",
    "stream_unique_bytes",
    "stream_handle_changes",
    "stream_offset_changes",
    "stream_stride_changes",
    "ib_state_samples",
    "ib_metal_binds",
    "ib_handle_changes",
    "ib_unique_handles",
    "ib_unique_bytes",
    "pso_state_samples",
    "pso_state_samples_per_draw",
    "pso_handle_changes",
    "pso_unique_handles",
    "shader_variant_changes",
    "shader_variant_unique",
    "vsout_layout_changes",
    "vsout_layout_unique",
    "vsout_layout_cache_hits",
    "vsout_layout_cache_misses",
    "argbuf_table_bytes",
    "argbuf_cbuf_bytes",
    "argbuf_cbuf_vs_bytes",
    "argbuf_cbuf_ffp_vs_bytes",
    "argbuf_cbuf_ps_bytes",
    "argbuf_cbuf_ffp_ps_bytes",
    "argbuf_cbuf_vs_first_bytes",
    "argbuf_cbuf_vs_rewrite_changed_bytes",
    "argbuf_cbuf_vs_rewrite_unchanged_bytes",
    "argbuf_cbuf_vs_float_changed_bytes",
    "argbuf_cbuf_vs_int_changed_bytes",
    "argbuf_cbuf_vs_bool_changed_bytes",
    "argbuf_cbuf_vs_uploads",
    "argbuf_cbuf_vs_full_struct_uploads",
    "argbuf_cbuf_vs_usage_unknown_uploads",
    "argbuf_cbuf_vs_usage_indexed_float_uploads",
    "argbuf_cbuf_vs_plan_float_regs_sum",
    "argbuf_cbuf_vs_plan_float_regs_max",
    "argbuf_cbuf_vs_dirty_float_regs_sum",
    "argbuf_cbuf_vs_dirty_float_regs_max",
    "argbuf_cbuf_vs_usage_float_regs_sum",
    "argbuf_cbuf_vs_usage_float_regs_max",
    "argbuf_cbuf_ffp_vs_first_bytes",
    "argbuf_cbuf_ffp_vs_rewrite_changed_bytes",
    "argbuf_cbuf_ffp_vs_rewrite_unchanged_bytes",
    "argbuf_cbuf_ffp_vs_matrix_changed_bytes",
    "argbuf_cbuf_ffp_vs_material_changed_bytes",
    "argbuf_cbuf_ffp_vs_light_changed_bytes",
    "argbuf_cbuf_ffp_vs_blend_changed_bytes",
    "argbuf_cbuf_ffp_vs_tex_transform_changed_bytes",
    "argbuf_cbuf_ffp_vs_clip_changed_bytes",
    "argbuf_cbuf_ffp_vs_viewport_changed_bytes",
    "argbuf_cbuf_ffp_vs_fog_point_changed_bytes",
    "set_vertex_bytes_bytes",
    "set_vertex_bytes_slot5_bytes",
    "set_vertex_bytes_other_bytes",
    "transient_vertex_bytes",
    "transient_vertex_user_bytes",
    "transient_vertex_preupload_bytes",
    "transient_vertex_decl_fallback_bytes",
    "transient_vertex_expanded_main_bytes",
    "transient_vertex_expanded_extra_bytes",
    "transient_index_bytes",
    "transient_index_user_bytes",
    "transient_index_preupload_bytes",
    "transient_index_shadow_fallback_bytes",
    "transient_index_probe_reorder_bytes",
    "transient_index_optimized_order_bytes",
)

TOP_ENCODER_KEYS = (
    "draw_calls",
    "stream_metal_binds",
    "stream_metal_bind_handle_changes",
    "ib_metal_binds",
    "ib_handle_changes",
    "ffp_draws",
    "pretransformed_draws",
    "cull_none_draws",
    "cull_back_draws",
    "depth_enabled_draws",
    "depth_write_draws",
    "blend_state_changes",
    "blend_state_unique",
    "blend_enabled_noop_draws",
    "blend_constant_factor_draws",
    "probe_disable_alpha_blend_draws",
    "probe_disable_depth_write_draws",
    "probe_depth_func_always_draws",
    "scissor_enabled_draws",
    "x8_rt_texture_binding_samples",
    "x8_shader_alpha_fill_samples",
    "vertex_count",
    "triangle_estimate",
    "draw_primitive_min",
    "draw_primitive_max",
    "draw_vertex_min",
    "draw_vertex_max",
    "draw_primitive_bucket_1_63",
    "draw_primitive_bucket_64_255",
    "draw_primitive_bucket_256_1023",
    "draw_primitive_bucket_1024_4095",
    "draw_primitive_bucket_4096_plus",
    "draw_vertex_bucket_1_255",
    "draw_vertex_bucket_256_1023",
    "draw_vertex_bucket_1024_4095",
    "draw_vertex_bucket_4096_16383",
    "draw_vertex_bucket_16384_plus",
    "draw_geometry_signature_unique",
    "draw_geometry_signature_duplicates",
    "draw_geometry_signature_duplicate_ratio",
    "indexed_base_vertex_nonzero_draws",
    "indexed_base_vertex_negative_draws",
    "native_base_vertex_used_draws",
    "split_large_indexed_source_draws",
    "split_large_indexed_extra_draws",
    "split_large_indexed_stream0_span_limit",
    "split_large_indexed_chunk_stream0_span_max",
    "indexed_vertex_reference_count",
    "indexed_unique_vertex_estimate",
    "indexed_vertex_reuse_ratio",
    "indexed_vertex_cache_miss_estimate_16",
    "indexed_vertex_cache_miss_estimate_32",
    "indexed_vertex_cache_miss_estimate_64",
    "indexed_vertex_cache_miss_over_unique_16",
    "indexed_vertex_cache_miss_over_unique_32",
    "indexed_vertex_cache_miss_over_unique_64",
    "pso_handle_changes",
    "pso_state_samples_per_draw",
    "shader_variant_changes",
    "vsout_layout_changes",
    "vsout_layout_cache_hits",
    "vsout_layout_cache_misses",
    "argbuf_cbuf_bytes",
    "argbuf_table_bytes",
    "set_vertex_bytes_bytes",
    "transient_vertex_bytes",
    "transient_index_bytes",
)

ENCODER_CSV_KEYS = (
    "seq",
    "encoder",
    "rt",
    "depth",
    "rt_format",
    "rt_width",
    "rt_height",
    "rt_bpp",
    "rt_alias_texture",
    "rt_texture_usage",
    "rt_format_swizzle",
    "rt_texture_needs_shader_read_view",
    "depth_format",
    "depth_width",
    "depth_height",
    "depth_bpp",
    "depth_alias_texture",
    "depth_texture_usage",
    "depth_format_swizzle",
    "depth_texture_needs_shader_read_view",
    "end_reason",
    "draw_calls",
    "indexed_draws",
    "expanded_indexed_draws",
    "ffp_draws",
    "programmable_draws",
    "pretransformed_draws",
    "textured_draws",
    "cull_none_draws",
    "cull_front_draws",
    "cull_back_draws",
    "fill_solid_draws",
    "fill_wireframe_draws",
    "depth_enabled_draws",
    "depth_write_draws",
    "depth_func_less_draws",
    "depth_func_lessequal_draws",
    "depth_func_always_draws",
    "depth_func_other_draws",
    "scissor_enabled_draws",
    "alpha_blend_enabled_draws",
    "blend_state_samples",
    "blend_state_changes",
    "blend_state_unique",
    "blend_state_unique_overflows",
    "blend_state_last",
    "blend_enabled_noop_draws",
    "blend_constant_factor_draws",
    "alpha_test_enabled_draws",
    "alpha_test_effective_draws",
    "clip_plane_enabled_draws",
    "point_draws",
    "line_draws",
    "triangle_draws",
    "primitive_count",
    "triangle_estimate",
    "vertex_count",
    "indexed_triangle_opaque_depth_write_draws",
    "indexed_triangle_opaque_depth_write_primitives",
    "indexed_triangle_opaque_depth_write_vertices",
    "indexed_triangle_depth_read_draws",
    "indexed_triangle_depth_read_primitives",
    "indexed_triangle_depth_read_vertices",
    "indexed_triangle_alpha_blend_draws",
    "indexed_triangle_alpha_blend_primitives",
    "indexed_triangle_alpha_blend_vertices",
    "indexed_triangle_scissor_draws",
    "indexed_triangle_scissor_primitives",
    "indexed_triangle_scissor_vertices",
    "indexed_triangle_textured_draws",
    "indexed_triangle_textured_primitives",
    "indexed_triangle_textured_vertices",
    "indexed_triangle_large_4096_draws",
    "indexed_triangle_large_4096_primitives",
    "indexed_triangle_large_4096_vertices",
    "indexed_triangle_large_4096_opaque_depth_write_draws",
    "indexed_triangle_large_4096_opaque_depth_write_primitives",
    "indexed_triangle_large_4096_opaque_depth_write_vertices",
    "indexed_triangle_large_4096_depth_read_draws",
    "indexed_triangle_large_4096_depth_read_primitives",
    "indexed_triangle_large_4096_depth_read_vertices",
    "indexed_triangle_large_4096_alpha_blend_draws",
    "indexed_triangle_large_4096_alpha_blend_primitives",
    "indexed_triangle_large_4096_alpha_blend_vertices",
    "indexed_triangle_large_4096_scissor_draws",
    "indexed_triangle_large_4096_scissor_primitives",
    "indexed_triangle_large_4096_scissor_vertices",
    "indexed_triangle_large_4096_textured_draws",
    "indexed_triangle_large_4096_textured_primitives",
    "indexed_triangle_large_4096_textured_vertices",
    "texture_mask_or",
    "fragment_texture_binding_samples",
    "fragment_texture_binding_mask_or",
    "x8_rt_texture_binding_samples",
    "x8_rt_texture_binding_mask_or",
    "x8_rt_texture_binding_unique_handles",
    "x8_rt_texture_binding_unique_handle_overflows",
    "x8_rt_texture_binding_shader_read_view_samples",
    "x8_rt_texture_binding_active_rt_alias_samples",
    "x8_shader_alpha_fill_samples",
    "x8_shader_alpha_fill_mask_or",
    "x8_rt_texture_binding_last_stage",
    "x8_rt_texture_binding_last_handle",
    "draw_primitive_min",
    "draw_primitive_max",
    "draw_vertex_min",
    "draw_vertex_max",
    "draw_primitive_bucket_1_63",
    "draw_primitive_bucket_64_255",
    "draw_primitive_bucket_256_1023",
    "draw_primitive_bucket_1024_4095",
    "draw_primitive_bucket_4096_plus",
    "draw_vertex_bucket_1_255",
    "draw_vertex_bucket_256_1023",
    "draw_vertex_bucket_1024_4095",
    "draw_vertex_bucket_4096_16383",
    "draw_vertex_bucket_16384_plus",
    "draw_geometry_signature_samples",
    "draw_geometry_signature_unique",
    "draw_geometry_signature_unique_overflows",
    "draw_geometry_signature_duplicates",
    "draw_geometry_signature_consecutive_duplicates",
    "draw_geometry_signature_duplicate_ratio",
    "draw_geometry_signature_consecutive_duplicate_ratio",
    "draw_geometry_signature_last",
    "indexed_base_vertex_samples",
    "indexed_base_vertex_nonzero_draws",
    "indexed_base_vertex_negative_draws",
    "indexed_base_vertex_positive_draws",
    "indexed_base_vertex_min",
    "indexed_base_vertex_max",
    "native_base_vertex_requested_draws",
    "native_base_vertex_used_draws",
    "native_base_vertex_skipped_negative_draws",
    "split_large_indexed_source_draws",
    "split_large_indexed_metal_draws",
    "split_large_indexed_extra_draws",
    "split_large_indexed_primitive_limit",
    "split_large_indexed_stream0_span_limit",
    "split_large_indexed_chunk_stream0_span_max",
    "split_large_indexed_primitive_count",
    "indexed_order_probe_draws",
    "indexed_order_probe_skipped",
    "indexed_order_probe_bytes",
    "indexed_order_optimized_draws",
    "indexed_order_optimized_skipped",
    "indexed_order_optimized_bytes",
    "probe_scissor_rect_draws",
    "probe_scissor_rect_skipped",
    "probe_scissor_rect_area_delta_pixels",
    "probe_disable_alpha_blend_draws",
    "probe_disable_depth_write_draws",
    "probe_depth_func_always_draws",
    "indexed_vertex_reuse_samples",
    "indexed_vertex_reuse_skipped",
    "indexed_vertex_reference_count",
    "indexed_unique_vertex_estimate",
    "indexed_vertex_reuse_ratio",
    "indexed_vertex_cache_miss_estimate_16",
    "indexed_vertex_cache_miss_estimate_32",
    "indexed_vertex_cache_miss_estimate_64",
    "indexed_vertex_cache_miss_over_unique_16",
    "indexed_vertex_cache_miss_over_unique_32",
    "indexed_vertex_cache_miss_over_unique_64",
    "stream0_stride_min",
    "stream0_stride_max",
    "stream_state_samples",
    "stream_metal_binds",
    "stream_metal_bind_firsts",
    "stream_metal_bind_handle_changes",
    "stream_metal_bind_offset_changes",
    "stream_unique_handles",
    "stream_unique_handle_overflows",
    "stream_unique_bytes",
    "stream_unique_dynamic_handles",
    "stream_unique_writeonly_handles",
    "stream_unique_default_pool_handles",
    "stream_unique_managed_pool_handles",
    "stream_unique_systemmem_pool_handles",
    "stream_unique_scratch_pool_handles",
    "stream_handle_changes",
    "stream_offset_changes",
    "stream_stride_changes",
    "stream0_last_handle",
    "stream0_last_offset",
    "stream0_last_stride",
    "ib_state_samples",
    "ib_metal_binds",
    "ib_handle_changes",
    "ib_unique_handles",
    "ib_unique_handle_overflows",
    "ib_unique_bytes",
    "ib_unique_dynamic_handles",
    "ib_unique_writeonly_handles",
    "ib_unique_default_pool_handles",
    "ib_unique_managed_pool_handles",
    "ib_unique_systemmem_pool_handles",
    "ib_unique_scratch_pool_handles",
    "ib_last_handle",
    "pso_state_samples",
    "pso_state_samples_per_draw",
    "pso_handle_changes",
    "pso_unique_handles",
    "pso_unique_handle_overflows",
    "pso_last_handle",
    "shader_variant_changes",
    "shader_variant_unique",
    "shader_variant_unique_overflows",
    "shader_variant_last",
    "vertex_shader_last",
    "pixel_shader_last",
    "vertex_shader_source_last",
    "pixel_shader_source_last",
    "vsout_layout_changes",
    "vsout_layout_unique",
    "vsout_layout_unique_overflows",
    "vsout_layout_last",
    "vsout_layout_cache_hits",
    "vsout_layout_cache_misses",
    "argbuf_table_bytes",
    "argbuf_cbuf_bytes",
    "argbuf_cbuf_vs_bytes",
    "argbuf_cbuf_ffp_vs_bytes",
    "argbuf_cbuf_ps_bytes",
    "argbuf_cbuf_ffp_ps_bytes",
    "argbuf_cbuf_vs_first_bytes",
    "argbuf_cbuf_vs_rewrite_changed_bytes",
    "argbuf_cbuf_vs_rewrite_unchanged_bytes",
    "argbuf_cbuf_vs_float_changed_bytes",
    "argbuf_cbuf_vs_int_changed_bytes",
    "argbuf_cbuf_vs_bool_changed_bytes",
    "argbuf_cbuf_vs_uploads",
    "argbuf_cbuf_vs_full_struct_uploads",
    "argbuf_cbuf_vs_usage_unknown_uploads",
    "argbuf_cbuf_vs_usage_indexed_float_uploads",
    "argbuf_cbuf_vs_plan_float_regs_sum",
    "argbuf_cbuf_vs_plan_float_regs_max",
    "argbuf_cbuf_vs_dirty_float_regs_sum",
    "argbuf_cbuf_vs_dirty_float_regs_max",
    "argbuf_cbuf_vs_usage_float_regs_sum",
    "argbuf_cbuf_vs_usage_float_regs_max",
    "argbuf_cbuf_ffp_vs_first_bytes",
    "argbuf_cbuf_ffp_vs_rewrite_changed_bytes",
    "argbuf_cbuf_ffp_vs_rewrite_unchanged_bytes",
    "argbuf_cbuf_ffp_vs_matrix_changed_bytes",
    "argbuf_cbuf_ffp_vs_material_changed_bytes",
    "argbuf_cbuf_ffp_vs_light_changed_bytes",
    "argbuf_cbuf_ffp_vs_blend_changed_bytes",
    "argbuf_cbuf_ffp_vs_tex_transform_changed_bytes",
    "argbuf_cbuf_ffp_vs_clip_changed_bytes",
    "argbuf_cbuf_ffp_vs_viewport_changed_bytes",
    "argbuf_cbuf_ffp_vs_fog_point_changed_bytes",
    "set_vertex_bytes_calls",
    "set_vertex_bytes_bytes",
    "set_vertex_bytes_slot5_calls",
    "set_vertex_bytes_slot5_bytes",
    "set_vertex_bytes_other_calls",
    "set_vertex_bytes_other_bytes",
    "transient_vertex_bytes",
    "transient_vertex_user_bytes",
    "transient_vertex_preupload_bytes",
    "transient_vertex_decl_fallback_bytes",
    "transient_vertex_expanded_main_bytes",
    "transient_vertex_expanded_extra_bytes",
    "transient_index_bytes",
    "transient_index_user_bytes",
    "transient_index_preupload_bytes",
    "transient_index_shadow_fallback_bytes",
    "transient_index_probe_reorder_bytes",
    "transient_index_optimized_order_bytes",
)

STREAM_CSV_KEYS = (
    "seq",
    "encoder",
    "stream",
    "samples",
    "metal_binds",
    "metal_bind_firsts",
    "metal_bind_handle_changes",
    "metal_bind_offset_changes",
    "unique_handles",
    "unique_handle_overflows",
    "unique_bytes",
    "unique_dynamic_handles",
    "unique_writeonly_handles",
    "unique_default_pool_handles",
    "unique_managed_pool_handles",
    "unique_systemmem_pool_handles",
    "unique_scratch_pool_handles",
    "handle_changes",
    "offset_changes",
    "stride_changes",
    "last_handle",
    "last_offset",
    "last_stride",
)

PROBE_DRAW_CSV_KEYS = (
    "seq",
    "encoder",
    "encoder_draw_index",
    "draw_ordinal",
    "eligible",
    "applied",
    "optimized_eligible",
    "optimized_applied",
    "scissor_rect_eligible",
    "scissor_rect_applied",
    "alpha_blend_probe_applied",
    "depth_write_probe_applied",
    "depth_func_probe_applied",
    "reorder_bytes",
    "split_eligible",
    "split_would_apply",
    "split_chunk_count",
    "split_max_chunks_per_draw",
    "split_stream0_span_limit",
    "split_chunk_stream0_span_max",
    "split_primitive_count",
    "original_index_available",
    "original_index_unique",
    "original_index_min",
    "original_index_max",
    "original_index_span",
    "original_index_first",
    "original_index_last",
    "original_cache_miss16",
    "original_cache_miss32",
    "original_cache_miss64",
    "original_adjacent_delta_sum",
    "original_adjacent_delta_max",
    "original_backward_jumps",
    "original_triangle_index_span_sum",
    "original_triangle_index_span_max",
    "original_stream0_byte_min",
    "original_stream0_byte_max",
    "original_stream0_byte_span",
    "effective_index_available",
    "effective_index_unique",
    "effective_index_min",
    "effective_index_max",
    "effective_index_span",
    "effective_index_first",
    "effective_index_last",
    "effective_cache_miss16",
    "effective_cache_miss32",
    "effective_cache_miss64",
    "effective_adjacent_delta_sum",
    "effective_adjacent_delta_max",
    "effective_backward_jumps",
    "effective_triangle_index_span_sum",
    "effective_triangle_index_span_max",
    "effective_stream0_byte_min",
    "effective_stream0_byte_max",
    "effective_stream0_byte_span",
    "primitive_type",
    "primitive_count",
    "vertex_count",
    "texture_mask",
    "color_write",
    "alpha_blend",
    "src_blend",
    "dst_blend",
    "blend_op",
    "separate_alpha",
    "src_blend_alpha",
    "dst_blend_alpha",
    "blend_op_alpha",
    "alpha_test",
    "depth_enabled",
    "depth_write",
    "depth_func",
    "stencil",
    "clip_plane",
    "scissor",
    "scissor_l",
    "scissor_t",
    "scissor_r",
    "scissor_b",
    "original_scissor_l",
    "original_scissor_t",
    "original_scissor_r",
    "original_scissor_b",
    "cull",
    "fill",
    "base_vertex",
    "start_index",
    "index_type",
    "index_buffer",
    "stream0_handle",
    "stream0_offset",
    "stream0_stride",
    "pso",
    "shader_variant",
    "vs",
    "ps",
    "vsout",
)

RENDER_PASS_ACTION_GROUPS = (
    ("Color Load", (
        "render_pass_load_action_load",
        "render_pass_load_action_clear",
        "render_pass_load_action_dontcare",
    )),
    ("Depth Load", (
        "render_pass_load_action_depth_load",
        "render_pass_load_action_depth_clear",
        "render_pass_load_action_depth_dontcare",
    )),
    ("Stencil Load", (
        "render_pass_load_action_stencil_load",
        "render_pass_load_action_stencil_clear",
        "render_pass_load_action_stencil_dontcare",
    )),
    ("Color Store", (
        "render_pass_store_action_store",
        "render_pass_store_action_dontcare",
        "render_pass_store_action_resolve",
    )),
    ("Depth Store", (
        "render_pass_store_action_depth_store",
        "render_pass_store_action_depth_dontcare",
    )),
    ("Stencil Store", (
        "render_pass_store_action_stencil_store",
        "render_pass_store_action_stencil_dontcare",
    )),
)

RENDER_PASS_REENTRY_KEYS = (
    "render_pass_tile_preservation_bytes",
    "render_pass_same_key_adjacent",
    "render_pass_same_key_reentry",
    "render_pass_same_key_reentry_preservation_bytes",
    "render_pass_same_key_reentry_color_preservation_bytes",
    "render_pass_same_key_reentry_depth_preservation_bytes",
    "render_pass_transition_rt_change_same_depth",
    "render_pass_transition_same_rt_depth_change",
    "render_pass_transition_rt_depth_change",
)

STATE_DELTA_BREAK_KEYS = (
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
)

RENDER_PASS_DEPTH_PROOF_KEYS = (
    "render_pass_depth_proof_allow_next_clear",
    "render_pass_depth_proof_allow_dead_no_present",
    "render_pass_depth_proof_block_null_depth",
    "render_pass_depth_proof_block_no_lookahead",
    "render_pass_depth_proof_block_msaa_resolve",
    "render_pass_depth_proof_block_draw_depth",
    "render_pass_depth_proof_block_shadow_sample",
    "render_pass_depth_proof_block_surface_copy",
    "render_pass_depth_proof_block_stretch_rect",
    "render_pass_depth_proof_block_readback",
    "render_pass_depth_proof_block_color_fill",
    "render_pass_depth_proof_block_depth_resolve",
    "render_pass_depth_proof_block_present",
)

RENDER_PASS_COLOR_PROOF_KEYS = (
    "render_pass_color_proof_allow_next_clear",
    "render_pass_color_proof_allow_dead_no_present",
    "render_pass_color_proof_block_null_color",
    "render_pass_color_proof_block_no_lookahead",
    "render_pass_color_proof_block_draw_target",
    "render_pass_color_proof_block_texture_sample",
    "render_pass_color_proof_block_surface_copy",
    "render_pass_color_proof_block_stretch_rect",
    "render_pass_color_proof_block_readback",
    "render_pass_color_proof_block_color_fill",
    "render_pass_color_proof_block_msaa_resolve",
    "render_pass_color_proof_block_present",
    "render_pass_color_proof_block_dead_no_present_disabled",
)

BLEND_FACTOR_NAMES = {
    1: "Zero",
    2: "One",
    3: "SrcColor",
    4: "InvSrcColor",
    5: "SrcAlpha",
    6: "InvSrcAlpha",
    7: "DestAlpha",
    8: "InvDestAlpha",
    9: "DestColor",
    10: "InvDestColor",
    11: "SrcAlphaSat",
    12: "BothSrcAlpha",
    13: "BothInvSrcAlpha",
    14: "BlendFactor",
    15: "InvBlendFactor",
    16: "SrcColor2",
    17: "InvSrcColor2",
}

BLEND_OP_NAMES = {
    1: "Add",
    2: "Subtract",
    3: "RevSubtract",
    4: "Min",
    5: "Max",
}


def parse_number(value: Any) -> int | float | str | None:
    if value is None:
        return None
    if isinstance(value, (int, float)):
        return value
    text = str(value).rstrip("]")
    if text.startswith("0x"):
        return text
    try:
        if "." in text:
            return float(text)
        return int(text)
    except ValueError:
        return text


def load_result(path: Path) -> dict[str, Any]:
    result_path = path / "result.json"
    if result_path.exists():
        return json.loads(result_path.read_text(encoding="utf-8"))
    log_path = path / "dxmt9.log"
    if not log_path.exists():
        raise SystemExit(f"missing result.json and dxmt9.log: {result_path}")
    counters: dict[str, Any] = {}
    bridge: dict[str, Any] = {}
    for line in log_path.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith(PERF_PREFIX):
            counters = parse_kv_line(line)
        elif line.startswith(BRIDGE_PREFIX):
            bridge = parse_kv_line(line)
    if not counters and not bridge:
        raise SystemExit(f"missing result.json and perf counters in dxmt9.log: {result_path}")
    return {
        "status": "partial-log",
        "capture_error": "missing result.json; synthesized from dxmt9.log",
        "dxmt9_perf_counters": counters,
        "dxmt9_bridge_counters": bridge,
    }


def parse_kv_line(line: str) -> dict[str, int | float | str]:
    parsed: dict[str, int | float | str] = {}
    for key, raw in KEY_VALUE_RE.findall(line):
        value = parse_number(raw)
        if value is not None:
            parsed[key] = value
    return parsed


def parse_encoder_lines(log_path: Path) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    encoders: list[dict[str, Any]] = []
    streams: list[dict[str, Any]] = []
    if not log_path.exists():
        return encoders, streams
    for line in log_path.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith(ENCODER_PREFIX):
            encoders.append(parse_kv_line(line))
        elif line.startswith(STREAM_PREFIX):
            streams.append(parse_kv_line(line))
    return encoders, streams


def parse_probe_draw_lines(log_path: Path) -> list[dict[str, Any]]:
    probe_draws: list[dict[str, Any]] = []
    if not log_path.exists():
        return probe_draws
    for line in log_path.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith(PROBE_DRAW_PREFIX):
            probe_draws.append(parse_kv_line(line))
    return probe_draws


def sum_key(rows: list[dict[str, Any]], key: str) -> int | float:
    total: int | float = 0
    for row in rows:
        value = row.get(key, 0)
        if isinstance(value, (int, float)):
            total += value
    return total


def fmt(value: Any) -> str:
    if value is None:
        return "missing"
    if isinstance(value, float):
        return f"{value:.3f}"
    if isinstance(value, int):
        return f"{value:,}"
    return str(value)


def pct(part: Any, total: Any) -> str:
    if not isinstance(part, (int, float)) or not isinstance(total, (int, float)) or total == 0:
        return "n/a"
    return f"{(part / total) * 100.0:.2f}%"


def ratio_text(numerator: Any, denominator: Any) -> str:
    if (
        not isinstance(numerator, (int, float)) or
        not isinstance(denominator, (int, float)) or
        denominator == 0
    ):
        return "n/a"
    return f"{numerator / denominator:.3f}"


def numeric_value(row: dict[str, Any], key: str) -> int | float:
    value = row.get(key, 0)
    if isinstance(value, (int, float)):
        return value
    return 0


def enum_name(names: dict[int, str], value: Any) -> str:
    if isinstance(value, int):
        return names.get(value, str(value))
    return str(value)


def blend_signature_text(row: dict[str, Any]) -> str:
    src = enum_name(BLEND_FACTOR_NAMES, row.get("src_blend"))
    dst = enum_name(BLEND_FACTOR_NAMES, row.get("dst_blend"))
    op = enum_name(BLEND_OP_NAMES, row.get("blend_op"))
    separate = numeric_value(row, "separate_alpha")
    color_write = row.get("color_write", "")
    text = f"rgb={src},{dst},{op}; sep={fmt(separate)}; write={color_write}"
    if separate:
        src_a = enum_name(BLEND_FACTOR_NAMES, row.get("src_blend_alpha"))
        dst_a = enum_name(BLEND_FACTOR_NAMES, row.get("dst_blend_alpha"))
        op_a = enum_name(BLEND_OP_NAMES, row.get("blend_op_alpha"))
        text += f"; alpha={src_a},{dst_a},{op_a}"
    return text


def alpha_blend_signature_rows(probe_draws: list[dict[str, Any]]) -> list[dict[str, Any]]:
    groups: dict[tuple[Any, ...], dict[str, Any]] = {}
    for row in probe_draws:
        if not numeric_value(row, "alpha_blend"):
            continue
        key = (
            row.get("seq"),
            row.get("encoder"),
            row.get("src_blend"),
            row.get("dst_blend"),
            row.get("blend_op"),
            row.get("separate_alpha"),
            row.get("src_blend_alpha"),
            row.get("dst_blend_alpha"),
            row.get("blend_op_alpha"),
            row.get("color_write"),
        )
        group = groups.setdefault(
            key,
            {
                "seq": row.get("seq"),
                "encoder": row.get("encoder"),
                "signature": blend_signature_text(row),
                "draws": 0,
                "primitives": 0,
                "vertices": 0,
                "large_4096_draws": 0,
                "large_4096_primitives": 0,
                "scissor_draws": 0,
                "depth_write_draws": 0,
                "psos": set(),
            },
        )
        primitives = numeric_value(row, "primitive_count")
        group["draws"] += 1
        group["primitives"] += primitives
        group["vertices"] += numeric_value(row, "vertex_count")
        if primitives >= 4096:
            group["large_4096_draws"] += 1
            group["large_4096_primitives"] += primitives
        if numeric_value(row, "scissor"):
            group["scissor_draws"] += 1
        if numeric_value(row, "depth_write"):
            group["depth_write_draws"] += 1
        pso = row.get("pso")
        if pso not in (None, ""):
            group["psos"].add(pso)

    rows = list(groups.values())
    for row in rows:
        row["pso_unique"] = len(row.pop("psos"))
    return sorted(
        rows,
        key=lambda row: (
            numeric_value(row, "primitives"),
            numeric_value(row, "draws"),
        ),
        reverse=True,
    )


def alpha_blend_signature_run_rows(probe_draws: list[dict[str, Any]]) -> list[dict[str, Any]]:
    rows = sorted(
        probe_draws,
        key=lambda row: (
            numeric_value(row, "seq"),
            numeric_value(row, "encoder"),
            numeric_value(row, "encoder_draw_index"),
            numeric_value(row, "draw_ordinal"),
        ),
    )
    runs: list[dict[str, Any]] = []
    current: dict[str, Any] | None = None
    current_key: tuple[Any, ...] | None = None

    def flush() -> None:
        nonlocal current, current_key
        if current is not None:
            current["pso_unique"] = len(current.pop("psos"))
            runs.append(current)
        current = None
        current_key = None

    for row in rows:
        if not numeric_value(row, "alpha_blend"):
            flush()
            continue

        key = (
            row.get("seq"),
            row.get("encoder"),
            row.get("src_blend"),
            row.get("dst_blend"),
            row.get("blend_op"),
            row.get("separate_alpha"),
            row.get("src_blend_alpha"),
            row.get("dst_blend_alpha"),
            row.get("blend_op_alpha"),
            row.get("color_write"),
        )
        draw_index = numeric_value(row, "encoder_draw_index")
        if current is None or key != current_key:
            flush()
            current_key = key
            current = {
                "seq": row.get("seq"),
                "encoder": row.get("encoder"),
                "first_draw": draw_index,
                "last_draw": draw_index,
                "signature": blend_signature_text(row),
                "draws": 0,
                "primitives": 0,
                "vertices": 0,
                "large_4096_draws": 0,
                "large_4096_primitives": 0,
                "scissor_draws": 0,
                "depth_write_draws": 0,
                "psos": set(),
            }

        primitives = numeric_value(row, "primitive_count")
        current["last_draw"] = draw_index
        current["draws"] += 1
        current["primitives"] += primitives
        current["vertices"] += numeric_value(row, "vertex_count")
        if primitives >= 4096:
            current["large_4096_draws"] += 1
            current["large_4096_primitives"] += primitives
        if numeric_value(row, "scissor"):
            current["scissor_draws"] += 1
        if numeric_value(row, "depth_write"):
            current["depth_write_draws"] += 1
        pso = row.get("pso")
        if pso not in (None, ""):
            current["psos"].add(pso)

    flush()
    return runs


def alpha_blend_signature_run_summary_rows(
    runs: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    groups: dict[tuple[Any, ...], dict[str, Any]] = {}
    for run in runs:
        key = (run.get("seq"), run.get("encoder"), run.get("signature"))
        group = groups.setdefault(
            key,
            {
                "seq": run.get("seq"),
                "encoder": run.get("encoder"),
                "signature": run.get("signature"),
                "runs": 0,
                "draws": 0,
                "primitives": 0,
                "large_4096_draws": 0,
                "large_4096_primitives": 0,
                "scissor_draws": 0,
                "depth_write_draws": 0,
                "max_run_draws": 0,
                "max_run_primitives": 0,
                "max_run_first_draw": None,
                "max_run_last_draw": None,
            },
        )
        group["runs"] += 1
        group["draws"] += numeric_value(run, "draws")
        group["primitives"] += numeric_value(run, "primitives")
        group["large_4096_draws"] += numeric_value(run, "large_4096_draws")
        group["large_4096_primitives"] += numeric_value(run, "large_4096_primitives")
        group["scissor_draws"] += numeric_value(run, "scissor_draws")
        group["depth_write_draws"] += numeric_value(run, "depth_write_draws")
        run_primitives = numeric_value(run, "primitives")
        if run_primitives > numeric_value(group, "max_run_primitives"):
            group["max_run_draws"] = numeric_value(run, "draws")
            group["max_run_primitives"] = run_primitives
            group["max_run_first_draw"] = run.get("first_draw")
            group["max_run_last_draw"] = run.get("last_draw")

    return sorted(
        groups.values(),
        key=lambda row: (
            numeric_value(row, "primitives"),
            numeric_value(row, "draws"),
        ),
        reverse=True,
    )


def write_csv(path: Path, rows: list[dict[str, Any]], keys: tuple[str, ...]) -> None:
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=keys, extrasaction="ignore")
        writer.writeheader()
        for row in rows:
            writer.writerow({key: row.get(key, "") for key in keys})


def load_existing_csv(path: Path) -> list[dict[str, Any]]:
    if not path.exists():
        return []
    with path.open(newline="", encoding="utf-8") as handle:
        rows = [dict(row) for row in csv.DictReader(handle)]
    for row in rows:
        for key, value in tuple(row.items()):
            row[key] = parse_number(value)
    return rows


def enrich_encoder_rows(encoders: list[dict[str, Any]]) -> None:
    for row in encoders:
        draws = numeric_value(row, "draw_calls")
        pso_samples = numeric_value(row, "pso_state_samples")
        signature_samples = numeric_value(row, "draw_geometry_signature_samples")
        signature_duplicates = numeric_value(row, "draw_geometry_signature_duplicates")
        consecutive_duplicates = numeric_value(
            row, "draw_geometry_signature_consecutive_duplicates")
        row["pso_state_samples_per_draw"] = (pso_samples / draws) if draws else 0.0
        row["draw_geometry_signature_duplicate_ratio"] = (
            signature_duplicates / signature_samples) if signature_samples else 0.0
        row["draw_geometry_signature_consecutive_duplicate_ratio"] = (
            consecutive_duplicates / signature_samples) if signature_samples else 0.0
        indexed_unique = numeric_value(row, "indexed_unique_vertex_estimate")
        indexed_refs = numeric_value(row, "indexed_vertex_reference_count")
        row["indexed_vertex_reuse_ratio"] = (
            indexed_refs / indexed_unique) if indexed_unique else 0.0
        for cache_size in (16, 32, 64):
            cache_misses = numeric_value(
                row, f"indexed_vertex_cache_miss_estimate_{cache_size}")
            row[f"indexed_vertex_cache_miss_over_unique_{cache_size}"] = (
                cache_misses / indexed_unique) if indexed_unique else 0.0


def write_markdown(
    output: Path,
    run_dir: Path,
    result: dict[str, Any],
    encoders: list[dict[str, Any]],
    streams: list[dict[str, Any]],
    encoder_csv: Path,
    stream_csv: Path,
    probe_draws: list[dict[str, Any]] | None = None,
    probe_draw_csv: Path | None = None,
) -> None:
    counters = result.get("dxmt9_perf_counters", {})
    bridge = result.get("dxmt9_bridge_counters", {})
    probe_draws = probe_draws or []
    lines: list[str] = []

    lines.append("# 3DMark05 Perf Summary")
    lines.append("")
    lines.append(f"- Output: `{run_dir}`")
    lines.append(f"- Status: `{result.get('status', 'unknown')}`")
    lines.append(f"- Capture error: `{result.get('capture_error')}`")
    lines.append(f"- Encoder lines: `{len(encoders)}`")
    lines.append(f"- Stream lines: `{len(streams)}`")
    lines.append(f"- Encoder CSV: `{encoder_csv}`")
    lines.append(f"- Stream CSV: `{stream_csv}`")
    lines.append(f"- Indexed probe draw lines: `{len(probe_draws)}`")
    if probe_draw_csv is not None:
        lines.append(f"- Indexed probe draw CSV: `{probe_draw_csv}`")
    lines.append("")

    lines.append("## Run Counters")
    lines.append("")
    lines.append("| Counter | Value |")
    lines.append("|---|---:|")
    for key in RUN_COUNTERS:
        lines.append(f"| `{key}` | `{fmt(counters.get(key))}` |")
    lines.append("")

    draw_run_submits = counters.get("commit_chunk_draw_run_submits")
    draw_run_records = counters.get("commit_chunk_draw_run_records")
    submission_submits = counters.get("commit_chunk_draw_submission_batch_submits")
    submission_records = counters.get("commit_chunk_draw_submission_batch_records")
    backend_batch_groups = counters.get("submit_draw_run_batch_groups")
    backend_batch_records = counters.get("submit_draw_run_batch_records")
    lines.append("## Draw Batching Derived")
    lines.append("")
    lines.append("| Metric | Value |")
    lines.append("|---|---:|")
    lines.append(
        "| `draw_run_records_per_submit` | "
        f"`{ratio_text(draw_run_records, draw_run_submits)}` |"
    )
    lines.append(
        "| `draw_submission_batch_records_per_submit` | "
        f"`{ratio_text(submission_records, submission_submits)}` |"
    )
    lines.append(
        "| `draw_submission_batch_max_records` | "
        f"`{fmt(counters.get('commit_chunk_draw_submission_batch_max_records'))}` |"
    )
    lines.append(
        "| `backend_draw_run_batch_records_per_group` | "
        f"`{ratio_text(backend_batch_records, backend_batch_groups)}` |"
    )
    lines.append(
        "| `backend_draw_run_batch_max_records` | "
        f"`{fmt(counters.get('submit_draw_run_batch_max_records'))}` |"
    )
    lines.append(
        "| `const_upload_passthrough_per_submission_batch` | "
        f"`{ratio_text(counters.get('commit_chunk_draw_batch_const_upload_passthrough'), submission_submits)}` |"
    )
    lines.append("")

    submission_batch_size_keys = (
        "commit_chunk_draw_submission_batch_size_1",
        "commit_chunk_draw_submission_batch_size_2",
        "commit_chunk_draw_submission_batch_size_3_4",
        "commit_chunk_draw_submission_batch_size_5_8",
        "commit_chunk_draw_submission_batch_size_9_16",
        "commit_chunk_draw_submission_batch_size_17_32",
        "commit_chunk_draw_submission_batch_size_33_plus",
    )
    lines.append("### Submission Batch Size Histogram")
    lines.append("")
    lines.append("| Counter | Value | Share |")
    lines.append("|---|---:|---:|")
    for key in submission_batch_size_keys:
        value = counters.get(key)
        lines.append(f"| `{key}` | `{fmt(value)}` | `{pct(value, submission_submits)}` |")
    lines.append("")

    lines.append("## Render Pass Action Split")
    lines.append("")
    for title, keys in RENDER_PASS_ACTION_GROUPS:
        total = sum(
            counters.get(key, 0)
            for key in keys
            if isinstance(counters.get(key), (int, float))
        )
        lines.append(f"### {title}")
        lines.append("")
        lines.append("| Counter | Value | Share |")
        lines.append("|---|---:|---:|")
        for key in keys:
            value = counters.get(key)
            lines.append(f"| `{key}` | `{fmt(value)}` | `{pct(value, total)}` |")
        lines.append("")

    lines.append("## Render Pass Re-entry / Preservation")
    lines.append("")
    lines.append("| Counter | Value |")
    lines.append("|---|---:|")
    for key in RENDER_PASS_REENTRY_KEYS:
        lines.append(f"| `{key}` | `{fmt(counters.get(key))}` |")
    lines.append("")

    color_proof_total = sum(
        counters.get(key, 0)
        for key in RENDER_PASS_COLOR_PROOF_KEYS
        if isinstance(counters.get(key), (int, float))
    )
    lines.append("## Color Store Proof Split")
    lines.append("")
    lines.append("| Counter | Value | Share |")
    lines.append("|---|---:|---:|")
    for key in RENDER_PASS_COLOR_PROOF_KEYS:
        value = counters.get(key)
        lines.append(f"| `{key}` | `{fmt(value)}` | `{pct(value, color_proof_total)}` |")
    lines.append("")

    depth_proof_total = sum(
        counters.get(key, 0)
        for key in RENDER_PASS_DEPTH_PROOF_KEYS
        if isinstance(counters.get(key), (int, float))
    )
    lines.append("## Depth Store Proof Split")
    lines.append("")
    lines.append("| Counter | Value | Share |")
    lines.append("|---|---:|---:|")
    for key in RENDER_PASS_DEPTH_PROOF_KEYS:
        value = counters.get(key)
        lines.append(f"| `{key}` | `{fmt(value)}` | `{pct(value, depth_proof_total)}` |")
    lines.append("")

    state_delta_total = counters.get("commit_chunk_draw_run_break_state_delta")
    lines.append("## State-Delta Break Split")
    lines.append("")
    lines.append("| Counter | Value | Share |")
    lines.append("|---|---:|---:|")
    for key in STATE_DELTA_BREAK_KEYS:
        value = counters.get(key)
        lines.append(f"| `{key}` | `{fmt(value)}` | `{pct(value, state_delta_total)}` |")
    lines.append("")

    const_total = counters.get("commit_chunk_draw_run_break_type_const_upload")
    const_keys = [
        "commit_chunk_draw_run_break_type_const_vs_f",
        "commit_chunk_draw_run_break_type_const_vs_i",
        "commit_chunk_draw_run_break_type_const_vs_b",
        "commit_chunk_draw_run_break_type_const_ps_f",
        "commit_chunk_draw_run_break_type_const_ps_i",
        "commit_chunk_draw_run_break_type_const_ps_b",
    ]
    lines.append("## Const-Upload Break Split")
    lines.append("")
    lines.append("| Counter | Value | Share |")
    lines.append("|---|---:|---:|")
    for key in const_keys:
        value = counters.get(key)
        lines.append(f"| `{key}` | `{fmt(value)}` | `{pct(value, const_total)}` |")
    lines.append("")

    if encoders:
        lines.append("## Encoder Aggregates")
        lines.append("")
        lines.append("| Metric | Sum |")
        lines.append("|---|---:|")
        for key in ENCODER_SUM_KEYS:
            lines.append(f"| `{key}` | `{fmt(sum_key(encoders, key))}` |")
        lines.append("")

        cbuf_total = sum_key(encoders, "argbuf_cbuf_bytes")
        cbuf_class_keys = (
            "argbuf_cbuf_vs_bytes",
            "argbuf_cbuf_ffp_vs_bytes",
            "argbuf_cbuf_ps_bytes",
            "argbuf_cbuf_ffp_ps_bytes",
            "argbuf_cbuf_vs_first_bytes",
            "argbuf_cbuf_vs_rewrite_changed_bytes",
            "argbuf_cbuf_vs_rewrite_unchanged_bytes",
            "argbuf_cbuf_ffp_vs_first_bytes",
            "argbuf_cbuf_ffp_vs_rewrite_changed_bytes",
            "argbuf_cbuf_ffp_vs_rewrite_unchanged_bytes",
        )
        lines.append("## Argbuf Cbuf Split")
        lines.append("")
        lines.append("| Metric | Sum | Share of cbuf |")
        lines.append("|---|---:|---:|")
        for key in cbuf_class_keys:
            value = sum_key(encoders, key)
            lines.append(f"| `{key}` | `{fmt(value)}` | `{pct(value, cbuf_total)}` |")
        lines.append("")

        transient_vertex_total = sum_key(encoders, "transient_vertex_bytes")
        transient_index_total = sum_key(encoders, "transient_index_bytes")
        transient_keys = (
            ("transient_vertex_user_bytes", transient_vertex_total),
            ("transient_vertex_preupload_bytes", transient_vertex_total),
            ("transient_vertex_decl_fallback_bytes", transient_vertex_total),
            ("transient_vertex_expanded_main_bytes", transient_vertex_total),
            ("transient_vertex_expanded_extra_bytes", transient_vertex_total),
            ("transient_index_user_bytes", transient_index_total),
            ("transient_index_preupload_bytes", transient_index_total),
            ("transient_index_shadow_fallback_bytes", transient_index_total),
            ("transient_index_probe_reorder_bytes", transient_index_total),
            ("transient_index_optimized_order_bytes", transient_index_total),
        )
        lines.append("## Transient Upload Source Split")
        lines.append("")
        lines.append("| Metric | Sum | Share of class |")
        lines.append("|---|---:|---:|")
        for key, total in transient_keys:
            value = sum_key(encoders, key)
            lines.append(f"| `{key}` | `{fmt(value)}` | `{pct(value, total)}` |")
        lines.append("")

        def add_top_section(title: str, sort_key: str) -> None:
            top_rows = sorted(encoders, key=lambda row: numeric_value(row, sort_key), reverse=True)[:10]
            lines.append(f"## {title}")
            lines.append("")
            header = "| seq | enc | rt | depth | " + " | ".join(f"`{k}`" for k in TOP_ENCODER_KEYS) + " |"
            lines.append(header)
            lines.append("|---:|---:|---|---|" + "---:|" * len(TOP_ENCODER_KEYS))
            for row in top_rows:
                values = [
                    fmt(row.get("seq")),
                    fmt(row.get("encoder")),
                    fmt(row.get("rt")),
                    fmt(row.get("depth")),
                ]
                values.extend(fmt(row.get(key)) for key in TOP_ENCODER_KEYS)
                lines.append("| " + " | ".join(values) + " |")
            lines.append("")

        top = sorted(
            encoders,
            key=lambda row: numeric_value(row, "argbuf_cbuf_bytes"),
            reverse=True,
        )[:10]
        lines.append("## Top Encoders By Argbuf Cbuf Bytes")
        lines.append("")
        header = "| seq | enc | rt | depth | " + " | ".join(f"`{k}`" for k in TOP_ENCODER_KEYS) + " |"
        lines.append(header)
        lines.append("|---:|---:|---|---|" + "---:|" * len(TOP_ENCODER_KEYS))
        for row in top:
            values = [
                fmt(row.get("seq")),
                fmt(row.get("encoder")),
                fmt(row.get("rt")),
                fmt(row.get("depth")),
            ]
            values.extend(fmt(row.get(key)) for key in TOP_ENCODER_KEYS)
            lines.append("| " + " | ".join(values) + " |")
        lines.append("")

        add_top_section("Top Encoders By Transient Vertex Bytes", "transient_vertex_bytes")
        add_top_section("Top Encoders By Transient Index Bytes", "transient_index_bytes")
        add_top_section("Top Encoders By setVertexBytes", "set_vertex_bytes_bytes")
        add_top_section("Top Encoders By Argbuf Table Bytes", "argbuf_table_bytes")
        add_top_section("Top Encoders By Stream Handle Changes", "stream_handle_changes")
        add_top_section("Top Encoders By Stream Offset Changes", "stream_offset_changes")
        add_top_section("Top Encoders By Stream Stride Changes", "stream_stride_changes")
        add_top_section("Top Encoders By IB Handle Changes", "ib_handle_changes")

    if probe_draws:
        applied = [row for row in probe_draws if numeric_value(row, "applied")]
        eligible = [row for row in probe_draws if numeric_value(row, "eligible")]
        optimized_applied = [
            row for row in probe_draws if numeric_value(row, "optimized_applied")
        ]
        optimized_eligible = [
            row for row in probe_draws if numeric_value(row, "optimized_eligible")
        ]
        scissor_rect_applied = [
            row for row in probe_draws if numeric_value(row, "scissor_rect_applied")
        ]
        scissor_rect_eligible = [
            row for row in probe_draws if numeric_value(row, "scissor_rect_eligible")
        ]
        split_eligible = [
            row for row in probe_draws if numeric_value(row, "split_eligible")
        ]
        split_would_apply = [
            row for row in probe_draws if numeric_value(row, "split_would_apply")
        ]
        lines.append("## Indexed Probe Draw Samples")
        lines.append("")
        lines.append("| Metric | Value |")
        lines.append("|---|---:|")
        lines.append(f"| `rows` | `{fmt(len(probe_draws))}` |")
        lines.append(f"| `eligible` | `{fmt(len(eligible))}` |")
        lines.append(f"| `applied` | `{fmt(len(applied))}` |")
        lines.append(f"| `optimized_eligible` | `{fmt(len(optimized_eligible))}` |")
        lines.append(f"| `optimized_applied` | `{fmt(len(optimized_applied))}` |")
        lines.append(f"| `scissor_rect_eligible` | `{fmt(len(scissor_rect_eligible))}` |")
        lines.append(f"| `scissor_rect_applied` | `{fmt(len(scissor_rect_applied))}` |")
        lines.append(
            f"| `alpha_blend_probe_applied` | "
            f"`{fmt(sum_key(probe_draws, 'alpha_blend_probe_applied'))}` |"
        )
        lines.append(
            f"| `depth_write_probe_applied` | "
            f"`{fmt(sum_key(probe_draws, 'depth_write_probe_applied'))}` |"
        )
        lines.append(
            f"| `depth_func_probe_applied` | "
            f"`{fmt(sum_key(probe_draws, 'depth_func_probe_applied'))}` |"
        )
        lines.append(f"| `split_eligible` | `{fmt(len(split_eligible))}` |")
        lines.append(f"| `split_would_apply` | `{fmt(len(split_would_apply))}` |")
        lines.append(
            f"| `split_chunk_count` | `{fmt(sum_key(probe_draws, 'split_chunk_count'))}` |"
        )
        lines.append(
            f"| `split_primitive_count` | `{fmt(sum_key(probe_draws, 'split_primitive_count'))}` |"
        )
        lines.append(
            f"| `reorder_bytes` | `{fmt(sum_key(probe_draws, 'reorder_bytes'))}` |"
        )
        lines.append("")

        blend_rows = alpha_blend_signature_rows(probe_draws)
        if blend_rows:
            lines.append("### Alpha Blend Signature Breakdown")
            lines.append("")
            lines.append(
                "| seq | enc | signature | draws | prims | verts | "
                "large4096 draws/prims | scissor draws | depth-write draws | PSOs |"
            )
            lines.append("|---:|---:|---|---:|---:|---:|---:|---:|---:|---:|")
            for row in blend_rows[:16]:
                large = (
                    f"{fmt(row.get('large_4096_draws'))}/"
                    f"{fmt(row.get('large_4096_primitives'))}"
                )
                lines.append(
                    "| "
                    + " | ".join(
                        [
                            fmt(row.get("seq")),
                            fmt(row.get("encoder")),
                            fmt(row.get("signature")),
                            fmt(row.get("draws")),
                            fmt(row.get("primitives")),
                            fmt(row.get("vertices")),
                            large,
                            fmt(row.get("scissor_draws")),
                            fmt(row.get("depth_write_draws")),
                            fmt(row.get("pso_unique")),
                        ]
                    )
                    + " |"
                )
            lines.append("")

        blend_runs = alpha_blend_signature_run_rows(probe_draws)
        if blend_runs:
            blend_run_summary = alpha_blend_signature_run_summary_rows(blend_runs)
            if blend_run_summary:
                lines.append("### Alpha Blend Signature Run Summary")
                lines.append("")
                lines.append(
                    "| seq | enc | signature | runs | draws | prims | max run draw range | "
                    "max run draws/prims | large4096 draws/prims | scissor draws | depth-write draws |"
                )
                lines.append("|---:|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|")
                for row in blend_run_summary[:16]:
                    max_range = (
                        f"{fmt(row.get('max_run_first_draw'))}.."
                        f"{fmt(row.get('max_run_last_draw'))}"
                    )
                    max_run = (
                        f"{fmt(row.get('max_run_draws'))}/"
                        f"{fmt(row.get('max_run_primitives'))}"
                    )
                    large = (
                        f"{fmt(row.get('large_4096_draws'))}/"
                        f"{fmt(row.get('large_4096_primitives'))}"
                    )
                    lines.append(
                        "| "
                        + " | ".join(
                            [
                                fmt(row.get("seq")),
                                fmt(row.get("encoder")),
                                fmt(row.get("signature")),
                                fmt(row.get("runs")),
                                fmt(row.get("draws")),
                                fmt(row.get("primitives")),
                                max_range,
                                max_run,
                                large,
                                fmt(row.get("scissor_draws")),
                                fmt(row.get("depth_write_draws")),
                            ]
                        )
                        + " |"
                    )
                lines.append("")

            lines.append("### Alpha Blend Signature Runs")
            lines.append("")
            lines.append(
                "| seq | enc | first draw | last draw | signature | draws | prims | "
                "large4096 draws/prims | scissor draws | depth-write draws | PSOs |"
            )
            lines.append("|---:|---:|---:|---:|---|---:|---:|---:|---:|---:|---:|")
            for row in blend_runs[:24]:
                large = (
                    f"{fmt(row.get('large_4096_draws'))}/"
                    f"{fmt(row.get('large_4096_primitives'))}"
                )
                lines.append(
                    "| "
                    + " | ".join(
                        [
                            fmt(row.get("seq")),
                            fmt(row.get("encoder")),
                            fmt(row.get("first_draw")),
                            fmt(row.get("last_draw")),
                            fmt(row.get("signature")),
                            fmt(row.get("draws")),
                            fmt(row.get("primitives")),
                            large,
                            fmt(row.get("scissor_draws")),
                            fmt(row.get("depth_write_draws")),
                            fmt(row.get("pso_unique")),
                        ]
                    )
                    + " |"
                )
            lines.append("")

        lines.append(
            "| seq | enc | draw | applied | opt | srect | alpha/depth/dfunc | prims | "
            "orig uniq/span/c64 | eff uniq/span/c64 | scissor | "
            "scissor rect | original rect | stream0 | ib | pso |"
        )
        lines.append("|---:|---:|---:|---:|---:|---:|---|---:|---:|---:|---:|---|---|---|---|---|")
        for row in probe_draws[:32]:
            rect = (
                f"{fmt(row.get('scissor_l'))},"
                f"{fmt(row.get('scissor_t'))},"
                f"{fmt(row.get('scissor_r'))},"
                f"{fmt(row.get('scissor_b'))}"
            )
            original_rect = (
                f"{fmt(row.get('original_scissor_l'))},"
                f"{fmt(row.get('original_scissor_t'))},"
                f"{fmt(row.get('original_scissor_r'))},"
                f"{fmt(row.get('original_scissor_b'))}"
            )
            stream = (
                f"{row.get('stream0_handle', '')}+"
                f"{fmt(row.get('stream0_offset'))}/"
                f"{fmt(row.get('stream0_stride'))}"
            )
            original_locality = (
                f"{fmt(row.get('original_index_unique'))}/"
                f"{fmt(row.get('original_index_span'))}/"
                f"{fmt(row.get('original_cache_miss64'))}"
            )
            effective_locality = (
                f"{fmt(row.get('effective_index_unique'))}/"
                f"{fmt(row.get('effective_index_span'))}/"
                f"{fmt(row.get('effective_cache_miss64'))}"
            )
            state_probe = (
                f"{fmt(row.get('alpha_blend_probe_applied'))}/"
                f"{fmt(row.get('depth_write_probe_applied'))}/"
                f"{fmt(row.get('depth_func_probe_applied'))}"
            )
            lines.append(
                "| "
                + " | ".join(
                    [
                        fmt(row.get("seq")),
                        fmt(row.get("encoder")),
                        fmt(row.get("encoder_draw_index")),
                        fmt(row.get("applied")),
                        fmt(row.get("optimized_applied")),
                        fmt(row.get("scissor_rect_applied")),
                        state_probe,
                        fmt(row.get("primitive_count")),
                        original_locality,
                        effective_locality,
                        fmt(row.get("scissor")),
                        rect,
                        original_rect,
                        stream,
                        fmt(row.get("index_buffer")),
                        fmt(row.get("pso")),
                    ]
                )
                + " |"
            )
        lines.append("")

    if bridge:
        lines.append("## Bridge Launch Check")
        lines.append("")
        lines.append("| Counter | Value |")
        lines.append("|---|---:|")
        for key in ("bridge_factory", "bridge_draw", "bridge_present", "bridge_commit_chunk"):
            lines.append(f"| `{key}` | `{fmt(bridge.get(key))}` |")
        lines.append("")

    output.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output_dir", type=Path, help="experiment output directory")
    parser.add_argument(
        "--output",
        type=Path,
        help="markdown output path (default: <output_dir>/3dmark05-perf-summary.md)",
    )
    args = parser.parse_args()

    run_dir = args.output_dir
    result = load_result(run_dir)
    output = args.output or (run_dir / "3dmark05-perf-summary.md")
    encoder_csv = output.parent / "3dmark05-perf-encoders.csv"
    stream_csv = output.parent / "3dmark05-perf-encoder-streams.csv"
    probe_draw_csv = output.parent / "3dmark05-perf-indexed-probe-draws.csv"
    log_path = run_dir / "dxmt9.log"
    encoders, streams = parse_encoder_lines(log_path)
    probe_draws = parse_probe_draw_lines(log_path)
    if not log_path.exists():
        encoders = load_existing_csv(encoder_csv)
        streams = load_existing_csv(stream_csv)
        probe_draws = load_existing_csv(probe_draw_csv)
    enrich_encoder_rows(encoders)
    if log_path.exists() or not encoder_csv.exists():
        write_csv(encoder_csv, encoders, ENCODER_CSV_KEYS)
    if log_path.exists() or not stream_csv.exists():
        write_csv(stream_csv, streams, STREAM_CSV_KEYS)
    if log_path.exists() or not probe_draw_csv.exists():
        write_csv(probe_draw_csv, probe_draws, PROBE_DRAW_CSV_KEYS)
    write_markdown(
        output,
        run_dir,
        result,
        encoders,
        streams,
        encoder_csv,
        stream_csv,
        probe_draws,
        probe_draw_csv,
    )
    print(output)
    return 0


if __name__ == "__main__":
    sys.exit(main())
