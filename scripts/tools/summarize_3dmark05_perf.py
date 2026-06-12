#!/usr/bin/env python3
"""Summarize a 3DMark05 dxmt9 perf output directory.

The script consumes `result.json` and, when present, `[dxmt9-perf-encoder]`
lines from `dxmt9.log`. If a diagnostic run is interrupted before result.json
is written, it can synthesize a partial result from the final `[dxmt9-perf]`
line. It is intentionally narrow: the output is a compact triage report for
the GT1 bottleneck work tracked in docs/perfomance/.
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
SUMMARY_COUNTER_ROW_RE = re.compile(r"^\| `([^`]+)` \| `([^`]+)` \|")
ENCODER_PREFIX = "[dxmt9-perf-encoder "
STREAM_PREFIX = "[dxmt9-perf-encoder-stream "
PROBE_DRAW_PREFIX = "[dxmt9-perf-indexed-probe-draw "
RENDER_PASS_REENTRY_PREFIX = "[dxmt9-perf-render-pass-reentry "
FRAME_PREFIX = "[dxmt9-perf-frame "
PERF_PREFIX = "[dxmt9-perf] "
BRIDGE_PREFIX = "[dxmt9-bridge-perf] "

RUN_COUNTERS = (
    "present_encoded",
    "present_schedule_requested_sync",
    "present_schedule_requested_immediate",
    "present_schedule_after_minimum_duration",
    "present_schedule_immediate",
    "present_minimum_duration_ms",
    "present_minimum_duration_max_ms",
    "draw_calls",
    "draw_indexed",
    "draw_expanded_indexed",
    "draw_skipped_no_pipeline",
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
    "render_split_final",
    "render_split_rt_change",
    "render_split_hazard",
    "render_split_clear",
    "render_split_surface_copy",
    "render_split_stretch",
    "render_split_readback",
    "render_split_color_fill",
    "render_split_present",
    "render_split_present_acquire",
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
    "render_pass_same_key_reentry_distance_1",
    "render_pass_same_key_reentry_distance_2",
    "render_pass_same_key_reentry_distance_3_4",
    "render_pass_same_key_reentry_distance_5_8",
    "render_pass_same_key_reentry_distance_9_16",
    "render_pass_same_key_reentry_distance_17_plus",
    "render_pass_same_key_reentry_distance_1_same_color",
    "render_pass_same_key_reentry_distance_1_same_color_preservation_bytes",
    "render_pass_same_key_reentry_distance_1_same_depth",
    "render_pass_same_key_reentry_distance_1_same_depth_preservation_bytes",
    "render_pass_same_key_reentry_distance_1_rt_depth_change",
    "render_pass_same_key_reentry_distance_1_rt_depth_change_preservation_bytes",
    "render_pass_same_key_reentry_distance_1_sample_change",
    "render_pass_same_key_reentry_distance_1_sample_change_preservation_bytes",
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
    "d3d9_draw_state_cache_hits",
    "d3d9_draw_state_cache_misses",
    "d3d9_draw_state_cache_hit_with_index",
    "d3d9_draw_state_cache_miss_with_index",
    "d3d9_draw_state_cache_hit_no_index",
    "d3d9_draw_state_cache_miss_no_index",
    "d3d9_draw_state_cache_uniform_refreshes",
    "d3d9_draw_state_cache_miss_after_unknown",
    "d3d9_draw_state_cache_miss_after_mutable_state",
    "d3d9_draw_state_cache_miss_after_draw_packet",
    "d3d9_draw_state_cache_miss_after_render_state",
    "d3d9_draw_state_cache_miss_after_texture",
    "d3d9_draw_state_cache_miss_after_stream",
    "d3d9_draw_state_cache_miss_after_index_buffer",
    "d3d9_draw_state_cache_miss_after_fvf_vdecl",
    "d3d9_draw_state_cache_miss_after_shader",
    "d3d9_draw_state_cache_miss_after_render_target_depth",
    "d3d9_draw_state_cache_miss_after_viewport_scissor",
    "d3d9_draw_state_cache_miss_after_texture_stage_sampler",
    "d3d9_draw_state_cache_miss_after_ffp_state",
    "d3d9_draw_state_cache_miss_after_clip_plane",
    "d3d9_draw_state_cache_miss_after_state_block",
    "d3d9_draw_state_cache_miss_after_reset",
    "d3d9_draw_state_cache_miss_after_swap_chain",
    "d3d9_draw_state_cache_miss_after_texture_lod",
    "d3d9_snapshot_draw_submission_cpu_ms",
    "d3d9_snapshot_draw_submission_cpu_max_ms",
    "d3d9_snapshot_draw_submission_cpu_p50_ms",
    "d3d9_snapshot_draw_submission_cpu_p95_ms",
    "d3d9_snapshot_draw_submission_cpu_p99_ms",
    "d3d9_snapshot_cache_lookup_cpu_ms",
    "d3d9_snapshot_cache_hit_cpu_ms",
    "d3d9_snapshot_cache_miss_cpu_ms",
    "d3d9_snapshot_cache_binding_layout_cpu_ms",
    "d3d9_snapshot_cache_uniform_refresh_cpu_ms",
    "d3d9_snapshot_cache_uniform_build_cpu_ms",
    "d3d9_snapshot_cache_uniform_hash_cpu_ms",
    "d3d9_snapshot_cache_miss_shader_layout_cpu_ms",
    "d3d9_snapshot_cache_miss_uniform_build_cpu_ms",
    "d3d9_snapshot_cache_miss_hot_build_cpu_ms",
    "d3d9_snapshot_uniform_build_calls",
    "d3d9_snapshot_uniform_build_vs_const_copy_cpu_ms",
    "d3d9_snapshot_uniform_build_ps_const_copy_cpu_ms",
    "d3d9_snapshot_uniform_build_ffp_matrix_cpu_ms",
    "d3d9_snapshot_uniform_build_ffp_material_light_cpu_ms",
    "d3d9_snapshot_uniform_build_texture_transform_cpu_ms",
    "d3d9_snapshot_uniform_build_clip_plane_cpu_ms",
    "d3d9_snapshot_uniform_build_hash_cpu_ms",
    "d3d9_snapshot_uniform_build_vs_const_hash_cpu_ms",
    "d3d9_snapshot_uniform_build_ps_const_hash_cpu_ms",
    "d3d9_snapshot_uniform_build_nonconst_hash_cpu_ms",
    "d3d9_snapshot_uniform_build_nonconst_hash_world_view_proj_cpu_ms",
    "d3d9_snapshot_uniform_build_nonconst_hash_ffp_world_view_cpu_ms",
    "d3d9_snapshot_uniform_build_nonconst_hash_ffp_normal_matrix_cpu_ms",
    "d3d9_snapshot_uniform_build_nonconst_hash_material_cpu_ms",
    "d3d9_snapshot_uniform_build_nonconst_hash_lights_cpu_ms",
    "d3d9_snapshot_uniform_build_nonconst_hash_ffp_blend_wvp_cpu_ms",
    "d3d9_snapshot_uniform_build_nonconst_hash_texture_transforms_cpu_ms",
    "d3d9_snapshot_uniform_build_nonconst_hash_clip_planes_cpu_ms",
    "d3d9_snapshot_uniform_build_payload_combine_hash_cpu_ms",
    "d3d9_snapshot_uniform_build_vs_const_hash_full",
    "d3d9_snapshot_uniform_build_ps_const_hash_full",
    "d3d9_snapshot_uniform_build_vs_const_hash_full_no_usage",
    "d3d9_snapshot_uniform_build_ps_const_hash_full_no_usage",
    "d3d9_snapshot_uniform_build_vs_const_hash_full_unknown",
    "d3d9_snapshot_uniform_build_ps_const_hash_full_unknown",
    "d3d9_snapshot_uniform_build_vs_const_hash_full_unknown_bytecode",
    "d3d9_snapshot_uniform_build_ps_const_hash_full_unknown_bytecode",
    "d3d9_snapshot_uniform_build_vs_const_hash_full_unknown_non_bytecode",
    "d3d9_snapshot_uniform_build_ps_const_hash_full_unknown_non_bytecode",
    "d3d9_snapshot_uniform_build_vs_const_hash_full_indexed_float",
    "d3d9_snapshot_uniform_build_ps_const_hash_full_indexed_float",
    "d3d9_snapshot_uniform_build_vs_const_hash_full_indexed_int",
    "d3d9_snapshot_uniform_build_ps_const_hash_full_indexed_int",
    "d3d9_snapshot_uniform_build_vs_const_hash_full_indexed_bool",
    "d3d9_snapshot_uniform_build_ps_const_hash_full_indexed_bool",
    "d3d9_snapshot_uniform_build_vs_const_hash_bytes",
    "d3d9_snapshot_uniform_build_ps_const_hash_bytes",
    "d3d9_snapshot_uniform_copy_cpu_ms",
    "d3d9_snapshot_state_copy_cpu_ms",
    "d3d9_snapshot_debug_snapshot_cpu_ms",
    "d3d9_snapshot_binding_override_cpu_ms",
    "d3d9_snapshot_binding_override_stream_scans",
    "d3d9_snapshot_binding_override_stream_records",
    "d3d9_snapshot_binding_override_index_records",
    "draw_uniform_payload_lookup_candidate_hits",
    "draw_uniform_payload_lookup_last_hits",
    "draw_uniform_payload_lookup_bucket_hits",
    "draw_uniform_payload_lookup_bucket_misses",
    "draw_uniform_payload_lookup_linear_hits",
    "draw_uniform_payload_lookup_bucket_probes",
    "draw_uniform_payload_lookup_bucket_collisions",
    "draw_uniform_payload_lookup_hash_collisions",
    "draw_uniform_payload_appends",
    "submit_draw_run_batch_groups",
    "submit_draw_run_batch_records",
    "submit_draw_run_batch_max_records",
    "submit_draw_run_binding_snapshot_cpu_ms",
    "submit_draw_run_binding_snapshot_cpu_max_ms",
    "submit_draw_run_payload_bytes_cpu_ms",
    "submit_draw_run_payload_bytes_cpu_max_ms",
    "submit_draw_run_slot_prepare_cpu_ms",
    "submit_draw_run_slot_prepare_cpu_max_ms",
    "submit_draw_run_resource_mark_cpu_ms",
    "submit_draw_run_resource_mark_cpu_max_ms",
    "submit_draw_run_append_cpu_ms",
    "submit_draw_run_append_cpu_max_ms",
    "submit_draw_run_chunk_commit_cpu_ms",
    "submit_draw_run_chunk_commit_cpu_max_ms",
    "submit_draw_run_batch_compat_scan_cpu_ms",
    "submit_draw_run_batch_compat_scan_cpu_max_ms",
    "submit_draw_run_batch_binding_override_cpu_ms",
    "submit_draw_run_batch_binding_override_cpu_max_ms",
    "submit_draw_run_batch_binding_snapshot_cpu_ms",
    "submit_draw_run_batch_binding_snapshot_cpu_max_ms",
    "submit_draw_run_batch_payload_bytes_cpu_ms",
    "submit_draw_run_batch_payload_bytes_cpu_max_ms",
    "submit_draw_run_batch_slot_prepare_cpu_ms",
    "submit_draw_run_batch_slot_prepare_cpu_max_ms",
    "submit_draw_run_batch_resource_mark_cpu_ms",
    "submit_draw_run_batch_resource_mark_cpu_max_ms",
    "submit_draw_run_batch_append_cpu_ms",
    "submit_draw_run_batch_append_cpu_max_ms",
    "submit_draw_run_batch_chunk_commit_cpu_ms",
    "submit_draw_run_batch_chunk_commit_cpu_max_ms",
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
    "draw_packet_actual_change_samples",
    "draw_packet_declared_any",
    "draw_packet_actual_any",
    "draw_packet_redundant_any",
    "draw_packet_declared_nonbinding",
    "draw_packet_actual_nonbinding",
    "draw_packet_redundant_nonbinding",
    "draw_packet_declared_uniform",
    "draw_packet_actual_uniform",
    "draw_packet_redundant_uniform",
    "draw_packet_redundant_texture",
    "draw_packet_redundant_shader",
    "draw_packet_redundant_render_state",
    "draw_packet_redundant_fvf_vdecl",
    "draw_packet_redundant_rt_depth",
    "draw_packet_redundant_viewport_scissor",
    "draw_packet_redundant_tss_sampler",
    "draw_packet_redundant_ffp",
    "draw_packet_redundant_clip",
    "argbuf_hybrid_bytes_per_encoder",
    "transient_upload_bytes",
    "transient_upload_cpu_ms",
    "encode_draw_cpu_ms",
    "encode_draw_cpu_max_ms",
    "encode_draw_cpu_p50_ms",
    "encode_draw_cpu_p95_ms",
    "encode_draw_cpu_p99_ms",
    "encode_draw_pipeline_lookup_cpu_ms",
    "encode_draw_pipeline_lookup_cpu_max_ms",
    "encode_draw_pipeline_lookup_cpu_p50_ms",
    "encode_draw_pipeline_lookup_cpu_p95_ms",
    "encode_draw_pipeline_lookup_cpu_p99_ms",
    "shader_variant_key_hash_cpu_ms",
    "shader_variant_key_hash_cpu_max_ms",
    "shader_variant_key_hash_cpu_p50_ms",
    "shader_variant_key_hash_cpu_p95_ms",
    "shader_variant_key_hash_cpu_p99_ms",
    "encode_draw_uniform_build_cpu_ms",
    "encode_draw_uniform_build_cpu_max_ms",
    "encode_draw_uniform_build_cpu_p50_ms",
    "encode_draw_uniform_build_cpu_p95_ms",
    "encode_draw_uniform_build_cpu_p99_ms",
    "encode_draw_fvf_decode_cpu_ms",
    "encode_draw_fvf_decode_cpu_max_ms",
    "encode_draw_fvf_decode_cpu_p50_ms",
    "encode_draw_fvf_decode_cpu_p95_ms",
    "encode_draw_fvf_decode_cpu_p99_ms",
    "encode_draw_pso_prefetch_handle_available",
    "encode_draw_pso_prefetch_handle_used",
    "encode_draw_pso_prefetch_handle_missing",
    "encode_draw_pso_prefetch_bypass_probe",
    "encode_draw_pso_prefetch_bypass_binding_override",
    "encode_draw_pso_prefetch_binding_override",
    "encode_draw_pso_prefetch_binding_override_compatible",
    "encode_draw_pso_prefetch_binding_override_incompatible",
    "submit_draw_cpu_ms",
    "encode_draw_stream_bind_cpu_ms",
    "encode_draw_stream_bind_raster_phase_cpu_ms",
    "encode_draw_stream_bind_raster_phase_calls",
    "encode_draw_stream_bind_ffp_stream_cpu_ms",
    "encode_draw_stream_bind_ffp_stream_calls",
    "encode_draw_stream_bind_shader_stream_cpu_ms",
    "encode_draw_stream_bind_shader_stream_calls",
    "encode_draw_stream_bind_texture_phase_cpu_ms",
    "encode_draw_stream_bind_texture_phase_calls",
    "encode_draw_stream_bind_index_phase_cpu_ms",
    "encode_draw_stream_bind_index_phase_calls",
    "encode_draw_texture_sampler_fragment_resolve_cpu_ms",
    "encode_draw_texture_sampler_fragment_resolve_calls",
    "encode_draw_texture_sampler_fragment_resolve_texture_cpu_ms",
    "encode_draw_texture_sampler_fragment_resolve_texture_calls",
    "encode_draw_texture_sampler_fragment_resource_array_cpu_ms",
    "encode_draw_texture_sampler_fragment_resource_array_calls",
    "encode_draw_texture_sampler_fragment_direct_cpu_ms",
    "encode_draw_texture_sampler_fragment_direct_calls",
    "encode_draw_texture_sampler_fragment_direct_texture_cpu_ms",
    "encode_draw_texture_sampler_fragment_direct_texture_calls",
    "encode_draw_texture_sampler_fragment_direct_texture_set_cpu_ms",
    "encode_draw_texture_sampler_fragment_direct_texture_set_calls",
    "encode_draw_texture_sampler_fragment_direct_sampler_cpu_ms",
    "encode_draw_texture_sampler_fragment_direct_sampler_calls",
    "encode_draw_texture_sampler_fragment_direct_sampler_set_cpu_ms",
    "encode_draw_texture_sampler_fragment_direct_sampler_set_calls",
    "encode_draw_texture_sampler_sampler_lookup_cpu_ms",
    "encode_draw_texture_sampler_sampler_lookup_calls",
    "encode_draw_texture_sampler_sampler_lookup_skipped_prehandle",
    "encode_draw_texture_sampler_lod_bias_cpu_ms",
    "encode_draw_texture_sampler_lod_bias_calls",
    "encode_draw_texture_sampler_vertex_resolve_cpu_ms",
    "encode_draw_texture_sampler_vertex_resolve_calls",
    "encode_draw_texture_sampler_vertex_direct_cpu_ms",
    "encode_draw_texture_sampler_vertex_direct_calls",
    "bind_texture",
    "bind_texture_skipped",
    "bind_sampler",
    "bind_sampler_skipped",
    "encode_draw_raster_state_cpu_ms",
    "encode_draw_vertex_stream_bind_cpu_ms",
    "encode_draw_texture_sampler_bind_cpu_ms",
    "encode_draw_binding_packet_cpu_ms",
    "encode_draw_binding_packet_plan_cpu_ms",
    "encode_draw_binding_packet_cache_cpu_ms",
    "encode_draw_binding_packet_cache_key_cpu_ms",
    "encode_draw_binding_packet_cache_hash_cpu_ms",
    "encode_draw_binding_packet_cache_probe_cpu_ms",
    "encode_draw_binding_packet_cache_store_cpu_ms",
    "encode_draw_binding_packet_cache_hits",
    "encode_draw_binding_packet_cache_misses",
    "encode_draw_binding_packet_cache_collisions",
    "encode_draw_binding_packet_texture_record_cpu_ms",
    "encode_draw_argbuf_setup_cpu_ms",
    "encode_draw_argbuf_open_cpu_ms",
    "encode_draw_argbuf_open_reserve_cpu_ms",
    "encode_draw_argbuf_open_set_argument_buffer_cpu_ms",
    "encode_draw_argbuf_table_bind_cpu_ms",
    "encode_draw_argbuf_table_bind_calls",
    "encode_draw_argbuf_table_bind_skipped",
    "encode_draw_argbuf_cbuf_update_cpu_ms",
    "encode_draw_argbuf_cbuf_update_calls",
    "encode_draw_argbuf_cbuf_update_dirty_calls",
    "encode_draw_argbuf_cbuf_update_skipped_clean",
    "encode_draw_argbuf_cbuf_update_write_calls",
    "encode_draw_argbuf_cbuf_build_cpu_ms",
    "encode_draw_argbuf_cbuf_upload_cpu_ms",
    "encode_draw_argbuf_cbuf_setbuffer_cpu_ms",
    "encode_draw_argbuf_cbuf_build_vs_cpu_ms",
    "encode_draw_argbuf_cbuf_build_ps_cpu_ms",
    "encode_draw_argbuf_cbuf_build_ffp_vs_cpu_ms",
    "encode_draw_argbuf_cbuf_build_ffp_ps_cpu_ms",
    "encode_draw_argbuf_cbuf_upload_vs_cpu_ms",
    "encode_draw_argbuf_cbuf_upload_ps_cpu_ms",
    "encode_draw_argbuf_cbuf_upload_ffp_vs_cpu_ms",
    "encode_draw_argbuf_cbuf_upload_ffp_ps_cpu_ms",
    "encode_draw_argbuf_cbuf_setbuffer_vs_cpu_ms",
    "encode_draw_argbuf_cbuf_setbuffer_ps_cpu_ms",
    "encode_draw_argbuf_cbuf_setbuffer_ffp_vs_cpu_ms",
    "encode_draw_argbuf_cbuf_setbuffer_ffp_ps_cpu_ms",
    "encode_draw_argbuf_cbuf_upload_plan_cpu_ms",
    "encode_draw_argbuf_cbuf_upload_plan_vs_cpu_ms",
    "encode_draw_argbuf_cbuf_upload_plan_ps_cpu_ms",
    "encode_draw_argbuf_cbuf_binding_hash_cpu_ms",
    "encode_draw_argbuf_cbuf_binding_hash_vs_cpu_ms",
    "encode_draw_argbuf_cbuf_binding_hash_ps_cpu_ms",
    "encode_draw_argbuf_cbuf_binding_hash_ffp_vs_cpu_ms",
    "encode_draw_argbuf_cbuf_binding_hash_ffp_ps_cpu_ms",
    "encode_draw_argbuf_cbuf_binding_write_cpu_ms",
    "encode_draw_argbuf_cbuf_binding_write_vs_cpu_ms",
    "encode_draw_argbuf_cbuf_binding_write_ps_cpu_ms",
    "encode_draw_argbuf_cbuf_binding_write_ffp_vs_cpu_ms",
    "encode_draw_argbuf_cbuf_binding_write_ffp_ps_cpu_ms",
    "encode_draw_argbuf_cbuf_observer_cpu_ms",
    "encode_draw_argbuf_cbuf_observer_vs_cpu_ms",
    "encode_draw_argbuf_cbuf_observer_ps_cpu_ms",
    "encode_draw_argbuf_cbuf_observer_ffp_vs_cpu_ms",
    "encode_draw_argbuf_cbuf_observer_ffp_ps_cpu_ms",
    "encode_draw_argbuf_cbuf_cache_merge_cpu_ms",
    "encode_draw_argbuf_cbuf_cached_repoint_cpu_ms",
    "encode_draw_argbuf_cbuf_cached_repoint_calls",
    "encode_draw_argbuf_cbuf_cached_repoint_bytes",
    "encode_draw_argbuf_cbuf_content_probe_cpu_ms",
    "encode_draw_argbuf_cbuf_content_probe_calls",
    "encode_draw_argbuf_cbuf_content_probe_vs_hits",
    "encode_draw_argbuf_cbuf_content_probe_vs_misses",
    "encode_draw_argbuf_cbuf_content_probe_ps_hits",
    "encode_draw_argbuf_cbuf_content_probe_ps_misses",
    "encode_draw_argbuf_cbuf_content_probe_ffp_ps_hits",
    "encode_draw_argbuf_cbuf_content_probe_ffp_ps_misses",
    "encode_draw_argbuf_cbuf_reopen_full_repoint_calls",
    "encode_draw_argbuf_cbuf_reopen_no_dirty_hash_mismatch",
    "encode_draw_argbuf_cbuf_reopen_partial_candidates",
    "encode_draw_argbuf_cbuf_reopen_dirty_vs",
    "encode_draw_argbuf_cbuf_reopen_dirty_ps",
    "encode_draw_argbuf_cbuf_reopen_dirty_ffp_vs",
    "encode_draw_argbuf_cbuf_reopen_dirty_ffp_ps",
    "encode_draw_argbuf_cbuf_update_vs_cpu_ms",
    "encode_draw_argbuf_cbuf_update_ps_cpu_ms",
    "encode_draw_argbuf_cbuf_update_ffp_vs_cpu_ms",
    "encode_draw_argbuf_cbuf_update_ffp_ps_cpu_ms",
    "encode_draw_argbuf_cbuf_update_vs_calls",
    "encode_draw_argbuf_cbuf_update_ps_calls",
    "encode_draw_argbuf_cbuf_update_ffp_vs_calls",
    "encode_draw_argbuf_cbuf_update_ffp_ps_calls",
    "encode_draw_argbuf_cbuf_update_vs_bytes",
    "encode_draw_argbuf_cbuf_update_ps_bytes",
    "encode_draw_argbuf_cbuf_update_ffp_vs_bytes",
    "encode_draw_argbuf_cbuf_update_ffp_ps_bytes",
    "encode_draw_index_setup_cpu_ms",
    "encode_draw_index_source_resolve_cpu_ms",
    "encode_draw_index_cache_lookup_cpu_ms",
    "encode_draw_index_cache_candidate_cpu_ms",
    "encode_draw_index_cache_original_measure_cpu_ms",
    "encode_draw_index_cache_candidate_build_cpu_ms",
    "encode_draw_index_cache_candidate_read_cpu_ms",
    "encode_draw_index_cache_candidate_adjacency_cpu_ms",
    "encode_draw_index_cache_candidate_select_cpu_ms",
    "encode_draw_index_cache_candidate_write_cpu_ms",
    "encode_draw_index_cache_candidate_select_calls",
    "encode_draw_index_cache_candidate_select_slots",
    "encode_draw_index_cache_candidate_select_scored",
    "encode_draw_index_cache_candidate_select_skipped",
    "encode_draw_index_cache_candidate_select_candidates_max",
    "encode_draw_index_cache_candidate_frontier_dropped",
    "encode_draw_index_cache_candidate_lazy_heap_pops",
    "encode_draw_index_cache_candidate_lazy_refreshes",
    "encode_draw_index_cache_candidate_lazy_stale_drops",
    "encode_draw_index_cache_candidate_lazy_accepted",
    "encode_draw_index_cache_candidate_bucket_vertex_visits",
    "encode_draw_index_cache_candidate_bucket_moves",
    "encode_draw_index_cache_candidate_bucket_selected",
    "encode_draw_index_cache_candidate_upper_bound_rejected",
    "encode_draw_index_cache_candidate_measure_cpu_ms",
    "encode_draw_index_cache_gate_cpu_ms",
    "encode_draw_index_cache_apply_cpu_ms",
    "encode_draw_issue_cpu_ms",
    "encode_draw_issue_cpu_max_ms",
    "encode_draw_issue_cpu_p50_ms",
    "encode_draw_issue_cpu_p95_ms",
    "encode_draw_issue_cpu_p99_ms",
    "indexed_cache_opt_candidate_draws",
    "indexed_cache_opt_candidate_skipped",
    "indexed_cache_opt_candidate_bytes",
    "indexed_cache_opt_candidate_original_miss32",
    "indexed_cache_opt_candidate_miss32",
    "reordered_index_cache_lookups",
    "reordered_index_cache_hits",
    "reordered_index_cache_rejected_hits",
    "reordered_index_cache_misses",
    "reordered_index_cache_created",
    "reordered_index_cache_created_bytes",
    "gpu_command_buffer_errors",
    "gpu_command_buffer_time_ms",
    "bridge_commit_latency_ns",
    "bridge_commit_latency_p50_ns",
    "bridge_commit_latency_p95_ns",
    "bridge_commit_latency_p99_ns",
    "commit_chunk_import_cpu_ms",
    "commit_chunk_import_cpu_p50_ms",
    "commit_chunk_import_cpu_p95_ms",
    "commit_chunk_handle_cpu_ms",
    "commit_chunk_handle_cpu_p50_ms",
    "commit_chunk_handle_cpu_p95_ms",
    "commit_chunk_replay_cpu_ms",
    "commit_chunk_replay_cpu_p50_ms",
    "commit_chunk_replay_cpu_p95_ms",
    "commit_chunk_draw_batch_submit_cpu_ms",
    "commit_chunk_draw_batch_submit_cpu_p50_ms",
    "commit_chunk_draw_batch_submit_cpu_p95_ms",
    "commit_chunk_apply_draw_state_cpu_ms",
    "commit_chunk_apply_draw_state_cpu_p50_ms",
    "commit_chunk_apply_draw_state_cpu_p95_ms",
    "commit_chunk_draw_run_scan_cpu_ms",
    "commit_chunk_draw_run_scan_cpu_p50_ms",
    "commit_chunk_draw_run_scan_cpu_p95_ms",
    "commit_chunk_draw_run_build_cpu_ms",
    "commit_chunk_draw_run_build_cpu_p50_ms",
    "commit_chunk_draw_run_build_cpu_p95_ms",
    "commit_chunk_draw_run_submit_cpu_ms",
    "commit_chunk_draw_run_submit_cpu_p50_ms",
    "commit_chunk_draw_run_submit_cpu_p95_ms",
    "commit_chunk_draw_run_final_bind_cpu_ms",
    "commit_chunk_draw_run_final_bind_cpu_p50_ms",
    "commit_chunk_draw_run_final_bind_cpu_p95_ms",
    "commit_chunk_queue_draw_submission_cpu_ms",
    "commit_chunk_queue_draw_submission_cpu_p50_ms",
    "commit_chunk_queue_draw_submission_cpu_p95_ms",
    "commit_chunk_const_upload_cpu_ms",
    "commit_chunk_const_upload_cpu_p50_ms",
    "commit_chunk_const_upload_cpu_p95_ms",
    "completion_dequeue_samples",
    "completion_dequeue_age_ms",
    "completion_dequeue_age_p50_ms",
    "completion_dequeue_age_p95_ms",
    "completion_pending_depth_max",
    "completion_dequeue_status_not_enqueued",
    "completion_dequeue_status_enqueued",
    "completion_dequeue_status_committed",
    "completion_dequeue_status_scheduled",
    "completion_dequeue_status_completed",
    "completion_dequeue_status_error",
    "completion_wait_status_not_enqueued",
    "completion_wait_status_not_enqueued_ms",
    "completion_wait_status_enqueued",
    "completion_wait_status_enqueued_ms",
    "completion_wait_status_committed",
    "completion_wait_status_committed_ms",
    "completion_wait_status_scheduled",
    "completion_wait_status_scheduled_ms",
    "completion_wait_ms",
    "map_buffer_total_ms",
    "map_buffer_mutex_wait_ms",
    "map_buffer_wait_ms",
    "queue_sequence_wait_ms",
)

CORRECTNESS_VISUAL_RUN_KEYS = (
    "draw_skipped_no_pipeline",
    "gpu_command_buffer_errors",
    "hazard_probe",
    "hazard_bloom",
    "hazard_exact",
    "hazard_bloom_false_positive",
    "render_pass_begin",
    "render_pass_tile_preservation_bytes",
    "render_pass_same_key_reentry",
    "render_pass_same_key_reentry_distance_1",
    "render_pass_same_key_reentry_distance_2",
    "render_pass_same_key_reentry_distance_3_4",
    "render_pass_same_key_reentry_distance_5_8",
    "render_pass_same_key_reentry_distance_9_16",
    "render_pass_same_key_reentry_distance_17_plus",
    "render_pass_same_key_reentry_distance_1_same_color",
    "render_pass_same_key_reentry_distance_1_same_depth",
    "render_pass_same_key_reentry_distance_1_rt_depth_change",
    "render_pass_same_key_reentry_distance_1_sample_change",
    "render_pass_same_key_reentry_preservation_bytes",
    "render_pass_transition_rt_depth_change",
    "render_split_rt_change",
    "render_split_hazard",
    "render_split_clear",
    "render_split_present",
    "completion_wait_ms",
    "map_buffer_total_ms",
    "map_buffer_mutex_wait_ms",
    "map_buffer_wait_ms",
    "queue_sequence_wait_ms",
)

CORRECTNESS_VISUAL_ENCODER_KEYS = (
    "blend_screen_draws",
    "blend_additive_draws",
    "blend_alpha_composite_draws",
    "alpha_blend_textured_draws",
    "alpha_blend_small_draws",
    "blend_state_unique_overflows",
    "x8_rt_texture_binding_samples",
    "x8_rt_texture_binding_unique_handle_overflows",
    "x8_shader_alpha_fill_samples",
    "draw_geometry_signature_unique_overflows",
    "stream_unique_handle_overflows",
    "ib_unique_handle_overflows",
    "pso_unique_handle_overflows",
    "shader_variant_unique_overflows",
    "vsout_layout_unique_overflows",
    "tile_ffp_fallback_gpu_family_draws",
    "tile_ffp_fallback_not_ffp_draws",
    "tile_ffp_fallback_precision_draws",
    "tile_ffp_fallback_unsupported_state_draws",
    "transient_vertex_decl_fallback_bytes",
    "transient_index_shadow_fallback_bytes",
    "probe_disable_alpha_blend_draws",
    "probe_disable_depth_write_draws",
    "probe_depth_func_always_draws",
    "probe_force_texture_white_draws",
    "probe_fragmentless_depth_only_draws",
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
    "blend_screen_draws",
    "blend_additive_draws",
    "blend_alpha_composite_draws",
    "alpha_blend_textured_draws",
    "alpha_blend_textured_primitives",
    "alpha_blend_textured_vertices",
    "alpha_blend_small_draws",
    "alpha_blend_small_primitives",
    "alpha_blend_small_vertices",
    "alpha_test_enabled_draws",
    "alpha_test_effective_draws",
    "clip_plane_enabled_draws",
    "primitive_count",
    "triangle_estimate",
    "vertex_count",
    "color_load_bytes",
    "color_store_bytes",
    "depth_load_bytes",
    "depth_store_bytes",
    "stencil_load_bytes",
    "stencil_store_bytes",
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
    "probe_force_texture_white_draws",
    "probe_fragmentless_depth_only_draws",
    "probe_fragmentless_depth_only_primitives",
    "probe_fragmentless_depth_only_vertices",
    "indexed_vertex_reuse_samples",
    "indexed_vertex_reuse_skipped",
    "indexed_vertex_reference_count",
    "indexed_unique_vertex_estimate",
    "indexed_vertex_cache_miss_estimate_16",
    "indexed_vertex_cache_miss_estimate_32",
    "indexed_vertex_cache_miss_estimate_64",
    "indexed_cache_opt_candidate_draws",
    "indexed_cache_opt_candidate_skipped",
    "indexed_cache_opt_candidate_bytes",
    "indexed_cache_opt_candidate_original_miss16",
    "indexed_cache_opt_candidate_original_miss32",
    "indexed_cache_opt_candidate_original_miss64",
    "indexed_cache_opt_candidate_miss16",
    "indexed_cache_opt_candidate_miss32",
    "indexed_cache_opt_candidate_miss64",
    "reordered_index_cache_lookups",
    "reordered_index_cache_hits",
    "reordered_index_cache_rejected_hits",
    "reordered_index_cache_misses",
    "reordered_index_cache_created",
    "reordered_index_cache_created_bytes",
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
    "transient_vertex_staged_stream_bytes",
    "transient_index_bytes",
    "transient_index_user_bytes",
    "transient_index_preupload_bytes",
    "transient_index_shadow_fallback_bytes",
    "transient_index_probe_reorder_bytes",
    "transient_index_optimized_order_bytes",
    "transient_index_staged_ib_bytes",
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
    "color_store_bytes",
    "depth_store_bytes",
    "blend_state_changes",
    "blend_state_unique",
    "blend_enabled_noop_draws",
    "blend_constant_factor_draws",
    "blend_screen_draws",
    "blend_additive_draws",
    "blend_alpha_composite_draws",
    "alpha_blend_textured_draws",
    "alpha_blend_small_draws",
    "probe_disable_alpha_blend_draws",
    "probe_disable_depth_write_draws",
    "probe_depth_func_always_draws",
    "probe_force_texture_white_draws",
    "probe_fragmentless_depth_only_draws",
    "probe_fragmentless_depth_only_primitives",
    "probe_fragmentless_depth_only_vertices",
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
    "indexed_cache_opt_candidate_draws",
    "indexed_cache_opt_candidate_skipped",
    "indexed_cache_opt_candidate_bytes",
    "indexed_cache_opt_candidate_miss_delta_16",
    "indexed_cache_opt_candidate_miss_delta_32",
    "indexed_cache_opt_candidate_miss_delta_64",
    "indexed_cache_opt_candidate_miss_delta_pct_16",
    "indexed_cache_opt_candidate_miss_delta_pct_32",
    "indexed_cache_opt_candidate_miss_delta_pct_64",
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
    "color_attachment_count",
    "color0_included",
    "color0_load_action",
    "color0_store_action",
    "color0_clear",
    "color_load_bytes",
    "color_store_bytes",
    "depth_included",
    "depth_load_action",
    "depth_store_action",
    "depth_clear",
    "depth_load_bytes",
    "depth_store_bytes",
    "stencil_included",
    "stencil_load_action",
    "stencil_store_action",
    "stencil_clear",
    "stencil_load_bytes",
    "stencil_store_bytes",
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
    "blend_screen_draws",
    "blend_additive_draws",
    "blend_alpha_composite_draws",
    "alpha_blend_textured_draws",
    "alpha_blend_textured_primitives",
    "alpha_blend_textured_vertices",
    "alpha_blend_small_draws",
    "alpha_blend_small_primitives",
    "alpha_blend_small_vertices",
    "alpha_test_enabled_draws",
    "alpha_test_effective_draws",
    "clip_plane_enabled_draws",
    "point_draws",
    "line_draws",
    "triangle_draws",
    "primitive_count",
    "triangle_estimate",
    "vertex_count",
    "tile_ffp_routed_tile_draws",
    "tile_ffp_routed_tile_primitives",
    "tile_ffp_routed_tile_vertices",
    "tile_ffp_routed_portable_draws",
    "tile_ffp_routed_portable_primitives",
    "tile_ffp_routed_portable_vertices",
    "tile_ffp_eligible_draws",
    "tile_ffp_eligible_primitives",
    "tile_ffp_eligible_vertices",
    "tile_ffp_fallback_gpu_family_draws",
    "tile_ffp_fallback_gpu_family_primitives",
    "tile_ffp_fallback_not_ffp_draws",
    "tile_ffp_fallback_not_ffp_primitives",
    "tile_ffp_fallback_precision_draws",
    "tile_ffp_fallback_precision_primitives",
    "tile_ffp_fallback_unsupported_state_draws",
    "tile_ffp_fallback_unsupported_state_primitives",
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
    "probe_force_texture_white_draws",
    "probe_fragmentless_depth_only_draws",
    "probe_fragmentless_depth_only_primitives",
    "probe_fragmentless_depth_only_vertices",
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
    "indexed_cache_opt_candidate_draws",
    "indexed_cache_opt_candidate_skipped",
    "indexed_cache_opt_candidate_bytes",
    "indexed_cache_opt_candidate_original_miss16",
    "indexed_cache_opt_candidate_original_miss32",
    "indexed_cache_opt_candidate_original_miss64",
    "indexed_cache_opt_candidate_miss16",
    "indexed_cache_opt_candidate_miss32",
    "indexed_cache_opt_candidate_miss64",
    "indexed_cache_opt_candidate_miss_delta_16",
    "indexed_cache_opt_candidate_miss_delta_32",
    "indexed_cache_opt_candidate_miss_delta_64",
    "indexed_cache_opt_candidate_miss_delta_pct_16",
    "indexed_cache_opt_candidate_miss_delta_pct_32",
    "indexed_cache_opt_candidate_miss_delta_pct_64",
    "reordered_index_cache_lookups",
    "reordered_index_cache_hits",
    "reordered_index_cache_rejected_hits",
    "reordered_index_cache_misses",
    "reordered_index_cache_created",
    "reordered_index_cache_created_bytes",
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
    "transient_vertex_staged_stream_bytes",
    "transient_index_bytes",
    "transient_index_user_bytes",
    "transient_index_preupload_bytes",
    "transient_index_shadow_fallback_bytes",
    "transient_index_probe_reorder_bytes",
    "transient_index_optimized_order_bytes",
    "transient_index_staged_ib_bytes",
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
    "command_index",
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
    "candidate_built",
    "candidate_gate_passed",
    "candidate_index_available",
    "candidate_index_unique",
    "candidate_index_min",
    "candidate_index_max",
    "candidate_index_span",
    "candidate_index_first",
    "candidate_index_last",
    "candidate_cache_miss16",
    "candidate_cache_miss32",
    "candidate_cache_miss64",
    "candidate_adjacent_delta_sum",
    "candidate_adjacent_delta_max",
    "candidate_backward_jumps",
    "candidate_triangle_index_span_sum",
    "candidate_triangle_index_span_max",
    "candidate_stream0_byte_min",
    "candidate_stream0_byte_max",
    "candidate_stream0_byte_span",
    "primitive_type",
    "primitive_count",
    "vertex_count",
    "texture_mask",
    "texture0",
    "texture1",
    "texture2",
    "texture3",
    "texture4",
    "texture5",
    "texture6",
    "texture7",
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
    "effective_index_source",
    "effective_index_offset",
    "effective_index_bytes",
    "stream0_handle",
    "stream0_offset",
    "stream0_stride",
    "stream_extra_bindings",
    "pso",
    "shader_variant",
    "vs",
    "ps",
    "vs_constants_hash",
    "ps_constants_hash",
    "uniform_payload_hash",
    "vsout",
)

RENDER_PASS_REENTRY_CSV_KEYS = (
    "frame",
    "rank",
    "a_rt",
    "a_depth",
    "a_samples",
    "b_rt",
    "b_depth",
    "b_samples",
    "count",
    "preservation_bytes",
    "prior_a_seq",
    "prior_a_encoder",
    "prior_a_pass",
    "first_seq",
    "first_encoder",
    "first_pass",
    "first_b_seq",
    "first_b_encoder",
    "first_b_pass",
    "last_seq",
    "last_encoder",
    "last_pass",
    "last_b_seq",
    "last_b_encoder",
    "last_b_pass",
    "b_reads_a_color",
    "b_reads_a_depth",
    "a_reads_b_color",
    "a_reads_b_depth",
    "a_color_proof",
    "a_depth_proof",
    "b_color_proof",
    "b_depth_proof",
    "a_color_touch_distance",
    "a_depth_touch_distance",
    "b_color_touch_distance",
    "b_depth_touch_distance",
)

FRAME_CSV_KEYS = (
    "frame",
    "wall_ms",
    "fps",
    "present_encoded",
    "submit_draw",
    "submit_present",
    "command_buffers",
    "render_pass_begin",
    "render_pass_end",
    "draw_calls",
    "draw_indexed",
    "draw_triangles",
    "draw_vertices",
    "bind_pipeline",
    "submit_draw_cpu_ms",
    "encode_chunk_calls",
    "encode_chunk_cpu_ms",
    "encode_draw_cpu_ms",
    "encode_draw_pipeline_lookup_cpu_ms",
    "encode_draw_uniform_build_cpu_ms",
    "encode_draw_binding_packet_cpu_ms",
    "encode_draw_argbuf_cbuf_update_cpu_ms",
    "encode_draw_stream_bind_cpu_ms",
    "encode_draw_issue_cpu_ms",
    "command_buffer_commit_cpu_ms",
    "completion_wait_ms",
    "present_acquire_wait_ms",
    "present_boundary_wait_ms",
    "present_token_wait_ms",
    "gpu_command_buffer_time_ms",
    "gpu_command_buffer_time_samples",
    "render_encoder_gpu_time_ms",
    "render_encoder_gpu_time_samples",
    "gpu_command_buffer_errors",
    "sub_command_buffers",
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
    "render_pass_same_key_reentry_distance_1",
    "render_pass_same_key_reentry_distance_2",
    "render_pass_same_key_reentry_distance_3_4",
    "render_pass_same_key_reentry_distance_5_8",
    "render_pass_same_key_reentry_distance_9_16",
    "render_pass_same_key_reentry_distance_17_plus",
    "render_pass_same_key_reentry_distance_1_same_color",
    "render_pass_same_key_reentry_distance_1_same_color_preservation_bytes",
    "render_pass_same_key_reentry_distance_1_same_depth",
    "render_pass_same_key_reentry_distance_1_same_depth_preservation_bytes",
    "render_pass_same_key_reentry_distance_1_rt_depth_change",
    "render_pass_same_key_reentry_distance_1_rt_depth_change_preservation_bytes",
    "render_pass_same_key_reentry_distance_1_sample_change",
    "render_pass_same_key_reentry_distance_1_sample_change_preservation_bytes",
    "render_pass_same_key_reentry_preservation_bytes",
    "render_pass_same_key_reentry_color_preservation_bytes",
    "render_pass_same_key_reentry_depth_preservation_bytes",
    "render_pass_transition_rt_change_same_depth",
    "render_pass_transition_same_rt_depth_change",
    "render_pass_transition_rt_depth_change",
)

RENDER_PASS_REENTRY_DISTANCE_KEYS = (
    "render_pass_same_key_reentry_distance_1",
    "render_pass_same_key_reentry_distance_2",
    "render_pass_same_key_reentry_distance_3_4",
    "render_pass_same_key_reentry_distance_5_8",
    "render_pass_same_key_reentry_distance_9_16",
    "render_pass_same_key_reentry_distance_17_plus",
)

RENDER_PASS_REENTRY_DISTANCE1_SHAPE_KEYS = (
    (
        "render_pass_same_key_reentry_distance_1_same_color",
        "render_pass_same_key_reentry_distance_1_same_color_preservation_bytes",
        "same color / different depth",
    ),
    (
        "render_pass_same_key_reentry_distance_1_same_depth",
        "render_pass_same_key_reentry_distance_1_same_depth_preservation_bytes",
        "different color / same depth",
    ),
    (
        "render_pass_same_key_reentry_distance_1_rt_depth_change",
        "render_pass_same_key_reentry_distance_1_rt_depth_change_preservation_bytes",
        "different color / different depth",
    ),
    (
        "render_pass_same_key_reentry_distance_1_sample_change",
        "render_pass_same_key_reentry_distance_1_sample_change_preservation_bytes",
        "same attachments / sample-count change",
    ),
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

LOAD_ACTION_NAMES = {
    0: "dontcare",
    1: "load",
    2: "clear",
}

STORE_ACTION_NAMES = {
    0: "dontcare",
    1: "store",
    2: "resolve",
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


def parse_summary_number(value: str) -> int | float | str | None:
    text = value.replace(",", "")
    return parse_number(text)


def load_existing_summary_result(summary_path: Path) -> dict[str, Any] | None:
    if not summary_path.exists():
        return None

    counters: dict[str, Any] = {}
    bridge: dict[str, Any] = {}
    section: str | None = None
    for line in summary_path.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("## "):
            if line == "## Run Counters":
                section = "run"
            elif line == "## Bridge Launch Check":
                section = "bridge"
            else:
                section = None
            continue

        if section is None:
            continue

        match = SUMMARY_COUNTER_ROW_RE.match(line)
        if not match:
            continue
        key, raw_value = match.groups()
        value = parse_summary_number(raw_value)
        if value is None:
            continue
        if section == "run":
            counters[key] = value
        elif section == "bridge":
            bridge[key] = value

    if not counters and not bridge:
        return None
    return {
        "status": "partial-summary",
        "capture_error": "missing result.json/dxmt9.log; synthesized from existing summary",
        "dxmt9_perf_counters": counters,
        "dxmt9_bridge_counters": bridge,
    }


def load_result(path: Path) -> dict[str, Any]:
    result_path = path / "result.json"
    if result_path.exists():
        return json.loads(result_path.read_text(encoding="utf-8"))
    log_path = path / "dxmt9.log"
    if not log_path.exists():
        summary_result = load_existing_summary_result(path / "3dmark05-perf-summary.md")
        if summary_result is not None:
            return summary_result
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


def parse_render_pass_reentry_lines(log_path: Path) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    if not log_path.exists():
        return rows
    for line in log_path.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith(RENDER_PASS_REENTRY_PREFIX):
            rows.append(parse_kv_line(line))
    return rows


def parse_frame_lines(log_path: Path) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    if not log_path.exists():
        return rows
    for line in log_path.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith(FRAME_PREFIX):
            rows.append(parse_kv_line(line))
    return rows


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


def per_event_preservation_bytes(row: dict[str, Any]) -> int | float:
    count = numeric_value(row, "count")
    total = numeric_value(row, "preservation_bytes")
    if count == 0:
        return total
    if isinstance(total, int) and isinstance(count, int) and total % count == 0:
        return total // count
    return total / count


def bool_value(row: dict[str, Any], key: str) -> bool | None:
    value = row.get(key)
    if value is None or value == "":
        return None
    if isinstance(value, bool):
        return value
    if isinstance(value, (int, float)):
        return value != 0
    text = str(value).strip().lower()
    if text in {"1", "true", "yes", "y"}:
        return True
    if text in {"0", "false", "no", "n"}:
        return False
    return None


def read_relation_text(row: dict[str, Any], color_key: str, depth_key: str) -> str:
    color = bool_value(row, color_key)
    depth = bool_value(row, depth_key)
    if color is None and depth is None:
        return "unknown"
    parts: list[str] = []
    if color:
        parts.append("color")
    if depth:
        parts.append("depth")
    return "+".join(parts) if parts else "none"


COLOR_STORE_PROOF_NAMES = {
    0: "AllowNextClear",
    1: "AllowDeadNoPresent",
    2: "BlockNullColor",
    3: "BlockNoLookahead",
    4: "BlockDrawTarget",
    5: "BlockTextureSample",
    6: "BlockSurfaceCopy",
    7: "BlockStretchRect",
    8: "BlockReadback",
    9: "BlockColorFill",
    10: "BlockMsaaResolve",
    11: "BlockPresent",
    12: "BlockDeadNoPresentDisabled",
}

DEPTH_STORE_PROOF_NAMES = {
    0: "AllowNextClear",
    1: "AllowDeadNoPresent",
    2: "BlockNullDepth",
    3: "BlockNoLookahead",
    4: "BlockMsaaResolve",
    5: "BlockDrawDepth",
    6: "BlockShadowSample",
    7: "BlockSurfaceCopy",
    8: "BlockStretchRect",
    9: "BlockReadback",
    10: "BlockColorFill",
    11: "BlockDepthResolve",
    12: "BlockPresent",
}


def proof_text(row: dict[str, Any], color_key: str, depth_key: str) -> str:
    color_value = row.get(color_key)
    depth_value = row.get(depth_key)
    color = (
        COLOR_STORE_PROOF_NAMES.get(color_value, str(color_value))
        if isinstance(color_value, int) else
        (str(color_value) if color_value not in (None, "") else "unknown")
    )
    depth = (
        DEPTH_STORE_PROOF_NAMES.get(depth_value, str(depth_value))
        if isinstance(depth_value, int) else
        (str(depth_value) if depth_value not in (None, "") else "unknown")
    )
    return f"color={color}; depth={depth}"


def touch_distance_text(row: dict[str, Any], color_key: str, depth_key: str) -> str:
    no_touch = 4294967295
    parts: list[str] = []
    for label, key in (("color", color_key), ("depth", depth_key)):
        value = row.get(key)
        if isinstance(value, int):
            parts.append(f"{label}={'none' if value >= no_touch else value}")
        elif value not in (None, ""):
            parts.append(f"{label}={value}")
        else:
            parts.append(f"{label}=unknown")
    return "; ".join(parts)


def aggregate_render_pass_reentry_rows(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    groups: dict[tuple[Any, ...], dict[str, Any]] = {}
    for row in rows:
        key = (
            row.get("a_rt", ""),
            row.get("a_depth", ""),
            row.get("a_samples", ""),
            row.get("b_rt", ""),
            row.get("b_depth", ""),
            row.get("b_samples", ""),
            row.get("b_reads_a_color", ""),
            row.get("b_reads_a_depth", ""),
            row.get("a_reads_b_color", ""),
            row.get("a_reads_b_depth", ""),
            row.get("a_color_proof", ""),
            row.get("a_depth_proof", ""),
            row.get("b_color_proof", ""),
            row.get("b_depth_proof", ""),
            row.get("a_color_touch_distance", ""),
            row.get("a_depth_touch_distance", ""),
            row.get("b_color_touch_distance", ""),
            row.get("b_depth_touch_distance", ""),
        )
        group = groups.setdefault(
            key,
            {
                "a_rt": row.get("a_rt", ""),
                "a_depth": row.get("a_depth", ""),
                "a_samples": row.get("a_samples", ""),
                "b_rt": row.get("b_rt", ""),
                "b_depth": row.get("b_depth", ""),
                "b_samples": row.get("b_samples", ""),
                "count": 0,
                "preservation_bytes": 0,
                "frames": 0,
                "first_seq": row.get("first_seq", ""),
                "first_encoder": row.get("first_encoder", ""),
                "first_b_seq": row.get("first_b_seq", ""),
                "first_b_encoder": row.get("first_b_encoder", ""),
                "last_seq": row.get("last_seq", ""),
                "last_encoder": row.get("last_encoder", ""),
                "last_b_seq": row.get("last_b_seq", ""),
                "last_b_encoder": row.get("last_b_encoder", ""),
                "b_reads_a_color": row.get("b_reads_a_color", ""),
                "b_reads_a_depth": row.get("b_reads_a_depth", ""),
                "a_reads_b_color": row.get("a_reads_b_color", ""),
                "a_reads_b_depth": row.get("a_reads_b_depth", ""),
                "a_color_proof": row.get("a_color_proof", ""),
                "a_depth_proof": row.get("a_depth_proof", ""),
                "b_color_proof": row.get("b_color_proof", ""),
                "b_depth_proof": row.get("b_depth_proof", ""),
                "a_color_touch_distance": row.get("a_color_touch_distance", ""),
                "a_depth_touch_distance": row.get("a_depth_touch_distance", ""),
                "b_color_touch_distance": row.get("b_color_touch_distance", ""),
                "b_depth_touch_distance": row.get("b_depth_touch_distance", ""),
            },
        )
        group["count"] += numeric_value(row, "count")
        group["preservation_bytes"] += numeric_value(row, "preservation_bytes")
        group["frames"] += 1
        group["last_seq"] = row.get("last_seq", group.get("last_seq", ""))
        group["last_encoder"] = row.get("last_encoder", group.get("last_encoder", ""))
        group["last_b_seq"] = row.get("last_b_seq", group.get("last_b_seq", ""))
        group["last_b_encoder"] = row.get(
            "last_b_encoder", group.get("last_b_encoder", "")
        )
    return sorted(
        groups.values(),
        key=lambda item: (
            numeric_value(item, "preservation_bytes"),
            numeric_value(item, "count"),
        ),
        reverse=True,
    )


def aggregate_render_pass_reentry_patterns(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    groups: dict[tuple[Any, ...], dict[str, Any]] = {}
    for row in rows:
        bytes_per_event = per_event_preservation_bytes(row)
        key = (
            row.get("b_depth", ""),
            row.get("b_samples", ""),
            row.get("a_depth", ""),
            row.get("a_samples", ""),
            row.get("first_encoder", ""),
            row.get("first_b_encoder", ""),
            bytes_per_event,
            row.get("b_reads_a_color", ""),
            row.get("b_reads_a_depth", ""),
            row.get("a_reads_b_color", ""),
            row.get("a_reads_b_depth", ""),
            row.get("a_color_proof", ""),
            row.get("a_depth_proof", ""),
            row.get("b_color_proof", ""),
            row.get("b_depth_proof", ""),
            row.get("a_color_touch_distance", ""),
            row.get("a_depth_touch_distance", ""),
            row.get("b_color_touch_distance", ""),
            row.get("b_depth_touch_distance", ""),
        )
        group = groups.setdefault(
            key,
            {
                "b_depth": row.get("b_depth", ""),
                "b_samples": row.get("b_samples", ""),
                "a_depth": row.get("a_depth", ""),
                "a_samples": row.get("a_samples", ""),
                "b_encoder": row.get("first_b_encoder", ""),
                "a_encoder": row.get("first_encoder", ""),
                "bytes_per_event": bytes_per_event,
                "b_reads_a_color": row.get("b_reads_a_color", ""),
                "b_reads_a_depth": row.get("b_reads_a_depth", ""),
                "a_reads_b_color": row.get("a_reads_b_color", ""),
                "a_reads_b_depth": row.get("a_reads_b_depth", ""),
                "a_color_proof": row.get("a_color_proof", ""),
                "a_depth_proof": row.get("a_depth_proof", ""),
                "b_color_proof": row.get("b_color_proof", ""),
                "b_depth_proof": row.get("b_depth_proof", ""),
                "a_color_touch_distance": row.get("a_color_touch_distance", ""),
                "a_depth_touch_distance": row.get("a_depth_touch_distance", ""),
                "b_color_touch_distance": row.get("b_color_touch_distance", ""),
                "b_depth_touch_distance": row.get("b_depth_touch_distance", ""),
                "count": 0,
                "preservation_bytes": 0,
                "frames": 0,
                "last_seq": row.get("last_seq", ""),
                "last_b_seq": row.get("last_b_seq", ""),
            },
        )
        group["count"] += numeric_value(row, "count")
        group["preservation_bytes"] += numeric_value(row, "preservation_bytes")
        group["frames"] += 1
        group["last_seq"] = row.get("last_seq", group.get("last_seq", ""))
        group["last_b_seq"] = row.get("last_b_seq", group.get("last_b_seq", ""))
    return sorted(
        groups.values(),
        key=lambda item: (
            numeric_value(item, "preservation_bytes"),
            numeric_value(item, "count"),
        ),
        reverse=True,
    )


def aggregate_render_pass_reentry_touch_distances(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    groups: dict[tuple[Any, ...], dict[str, Any]] = {}
    for row in rows:
        key = (
            row.get("b_color_touch_distance", ""),
            row.get("b_depth_touch_distance", ""),
            row.get("a_color_touch_distance", ""),
            row.get("a_depth_touch_distance", ""),
        )
        group = groups.setdefault(
            key,
            {
                "b_color_touch_distance": row.get("b_color_touch_distance", ""),
                "b_depth_touch_distance": row.get("b_depth_touch_distance", ""),
                "a_color_touch_distance": row.get("a_color_touch_distance", ""),
                "a_depth_touch_distance": row.get("a_depth_touch_distance", ""),
                "count": 0,
                "preservation_bytes": 0,
                "patterns": 0,
            },
        )
        group["count"] += numeric_value(row, "count")
        group["preservation_bytes"] += numeric_value(row, "preservation_bytes")
        group["patterns"] += 1
    return sorted(
        groups.values(),
        key=lambda item: (
            numeric_value(item, "count"),
            numeric_value(item, "preservation_bytes"),
        ),
        reverse=True,
    )


def encoder_lookup_key(row: dict[str, Any], seq_key: str, encoder_key: str) -> tuple[Any, Any]:
    return (
        parse_number(row.get(seq_key)),
        parse_number(row.get(encoder_key)),
    )


def build_encoder_lookup(encoders: list[dict[str, Any]]) -> dict[tuple[Any, Any], dict[str, Any]]:
    lookup: dict[tuple[Any, Any], dict[str, Any]] = {}
    for row in encoders:
        lookup[encoder_lookup_key(row, "seq", "encoder")] = row
    return lookup


def encoder_for_reentry_side(
    encoder_lookup: dict[tuple[Any, Any], dict[str, Any]],
    row: dict[str, Any],
    seq_key: str,
    encoder_key: str,
    fallback_seq_key: str,
    fallback_encoder_key: str,
) -> dict[str, Any] | None:
    encoder = encoder_lookup.get(encoder_lookup_key(row, seq_key, encoder_key))
    if encoder is not None:
        return encoder
    return encoder_lookup.get(encoder_lookup_key(row, fallback_seq_key, fallback_encoder_key))


def encoder_role(row: dict[str, Any] | None) -> str:
    if row is None:
        return "missing"
    draws = numeric_value(row, "draw_calls")
    if draws == 0:
        return "empty"
    depth_enabled = numeric_value(row, "depth_enabled_draws")
    depth_write = numeric_value(row, "depth_write_draws")
    textured = numeric_value(row, "textured_draws")
    alpha = numeric_value(row, "alpha_blend_enabled_draws")
    screen = numeric_value(row, "blend_screen_draws")
    additive = numeric_value(row, "blend_additive_draws")
    composite = numeric_value(row, "blend_alpha_composite_draws")

    if depth_write == draws and textured == 0 and alpha == 0:
        return "opaque-depth-write-untextured"
    if depth_write == 0 and depth_enabled > 0 and alpha > 0 and screen > 0:
        return "screen-blend-depth-read"
    if depth_write == 0 and depth_enabled > 0 and alpha > 0 and additive > 0:
        return "additive-depth-read"
    if depth_write == 0 and depth_enabled > 0 and alpha > 0 and composite > 0:
        return "alpha-depth-read"
    if depth_write == 0 and depth_enabled > 0 and textured == draws and alpha == 0:
        return "textured-depth-read-opaque"
    if depth_write > 0 and depth_write < draws:
        return "mixed-depth-write"
    if depth_enabled == 0 and alpha > 0:
        return "alpha-no-depth"
    if depth_enabled == 0 and textured > 0:
        return "textured-no-depth"
    if textured > 0:
        return "textured-mixed"
    return "mixed"


def has_present_value(row: dict[str, Any] | None, key: str) -> bool:
    return row is not None and key in row and row.get(key) not in ("", None)


def render_pass_action_name(names: dict[int, str], value: Any) -> str:
    parsed = parse_number(value)
    if isinstance(parsed, int):
        return names.get(parsed, str(parsed))
    if parsed in ("", None):
        return "unknown"
    return str(parsed)


def attachment_pass_action_text(
    row: dict[str, Any],
    label: str,
    included_key: str,
    load_key: str,
    store_key: str,
    clear_key: str,
    load_bytes_key: str,
    store_bytes_key: str,
) -> str | None:
    if not has_present_value(row, included_key) and not has_present_value(row, load_key):
        return None
    if numeric_value(row, included_key) <= 0:
        return None
    load = render_pass_action_name(LOAD_ACTION_NAMES, row.get(load_key))
    store = render_pass_action_name(STORE_ACTION_NAMES, row.get(store_key))
    clear = "clear" if numeric_value(row, clear_key) else "no-clear"
    bytes_total = numeric_value(row, load_bytes_key) + numeric_value(row, store_bytes_key)
    return f"{label}={load}/{store}/{clear}/{fmt(bytes_total)}B"


def encoder_pass_action_text(row: dict[str, Any] | None) -> str:
    if row is None:
        return "missing"
    parts: list[str] = []
    color_text = attachment_pass_action_text(
        row,
        "c0",
        "color0_included",
        "color0_load_action",
        "color0_store_action",
        "color0_clear",
        "color_load_bytes",
        "color_store_bytes",
    )
    if color_text is not None:
        color_count = numeric_value(row, "color_attachment_count")
        if color_count > 1:
            color_text += f";colors={fmt(color_count)}"
        parts.append(color_text)
    elif has_present_value(row, "color_attachment_count"):
        color_count = numeric_value(row, "color_attachment_count")
        if color_count > 0:
            parts.append(f"color-count={fmt(color_count)}")
    depth_text = attachment_pass_action_text(
        row,
        "d",
        "depth_included",
        "depth_load_action",
        "depth_store_action",
        "depth_clear",
        "depth_load_bytes",
        "depth_store_bytes",
    )
    if depth_text is not None:
        parts.append(depth_text)
    stencil_text = attachment_pass_action_text(
        row,
        "s",
        "stencil_included",
        "stencil_load_action",
        "stencil_store_action",
        "stencil_clear",
        "stencil_load_bytes",
        "stencil_store_bytes",
    )
    if stencil_text is not None:
        parts.append(stencil_text)
    return "; ".join(parts) if parts else "unknown"


def aggregate_render_pass_reentry_encoder_roles(
    rows: list[dict[str, Any]],
    encoders: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    encoder_lookup = build_encoder_lookup(encoders)
    groups: dict[tuple[Any, ...], dict[str, Any]] = {}
    for row in rows:
        prior_a_encoder = encoder_lookup.get(
            encoder_lookup_key(row, "prior_a_seq", "prior_a_encoder")
        )
        a_encoder = encoder_for_reentry_side(
            encoder_lookup,
            row,
            "last_seq",
            "last_encoder",
            "first_seq",
            "first_encoder",
        )
        b_encoder = encoder_for_reentry_side(
            encoder_lookup,
            row,
            "last_b_seq",
            "last_b_encoder",
            "first_b_seq",
            "first_b_encoder",
        )
        prior_a_role = encoder_role(prior_a_encoder)
        b_role = encoder_role(b_encoder)
        a_role = encoder_role(a_encoder)
        prior_a_pass_action = encoder_pass_action_text(prior_a_encoder)
        b_pass_action = encoder_pass_action_text(b_encoder)
        a_pass_action = encoder_pass_action_text(a_encoder)
        key = (
            prior_a_role,
            b_role,
            a_role,
            prior_a_pass_action,
            b_pass_action,
            a_pass_action,
            row.get("b_color_touch_distance", ""),
            row.get("b_depth_touch_distance", ""),
            row.get("a_color_touch_distance", ""),
            row.get("a_depth_touch_distance", ""),
        )
        group = groups.setdefault(
            key,
            {
                "prior_a_role": prior_a_role,
                "b_role": b_role,
                "a_role": a_role,
                "prior_a_pass_action": prior_a_pass_action,
                "b_pass_action": b_pass_action,
                "a_pass_action": a_pass_action,
                "b_color_touch_distance": row.get("b_color_touch_distance", ""),
                "b_depth_touch_distance": row.get("b_depth_touch_distance", ""),
                "a_color_touch_distance": row.get("a_color_touch_distance", ""),
                "a_depth_touch_distance": row.get("a_depth_touch_distance", ""),
                "count": 0,
                "preservation_bytes": 0,
                "rows": 0,
                "missing_prior_a_encoder_rows": 0,
                "missing_a_encoder_rows": 0,
                "missing_b_encoder_rows": 0,
                "prior_a_draws_weighted": 0,
                "b_draws_weighted": 0,
                "a_draws_weighted": 0,
                "prior_a_primitives_weighted": 0,
                "b_primitives_weighted": 0,
                "a_primitives_weighted": 0,
            },
        )
        count = numeric_value(row, "count")
        group["count"] += count
        group["preservation_bytes"] += numeric_value(row, "preservation_bytes")
        group["rows"] += 1
        if prior_a_encoder is None:
            group["missing_prior_a_encoder_rows"] += 1
        else:
            group["prior_a_draws_weighted"] += (
                numeric_value(prior_a_encoder, "draw_calls") * count
            )
            group["prior_a_primitives_weighted"] += (
                numeric_value(prior_a_encoder, "primitive_count") * count
            )
        if b_encoder is None:
            group["missing_b_encoder_rows"] += 1
        else:
            group["b_draws_weighted"] += numeric_value(b_encoder, "draw_calls") * count
            group["b_primitives_weighted"] += numeric_value(b_encoder, "primitive_count") * count
        if a_encoder is None:
            group["missing_a_encoder_rows"] += 1
        else:
            group["a_draws_weighted"] += numeric_value(a_encoder, "draw_calls") * count
            group["a_primitives_weighted"] += numeric_value(a_encoder, "primitive_count") * count

    for group in groups.values():
        count = numeric_value(group, "count")
        group["prior_a_draws_avg"] = (
            numeric_value(group, "prior_a_draws_weighted") / count if count else 0.0
        )
        group["b_draws_avg"] = (
            numeric_value(group, "b_draws_weighted") / count if count else 0.0
        )
        group["a_draws_avg"] = (
            numeric_value(group, "a_draws_weighted") / count if count else 0.0
        )
        group["b_primitives_avg"] = (
            numeric_value(group, "b_primitives_weighted") / count if count else 0.0
        )
        group["prior_a_primitives_avg"] = (
            numeric_value(group, "prior_a_primitives_weighted") / count if count else 0.0
        )
        group["a_primitives_avg"] = (
            numeric_value(group, "a_primitives_weighted") / count if count else 0.0
        )

    return sorted(
        groups.values(),
        key=lambda item: (
            numeric_value(item, "preservation_bytes"),
            numeric_value(item, "count"),
        ),
        reverse=True,
    )


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


def alpha_blend_material_rows(probe_draws: list[dict[str, Any]]) -> list[dict[str, Any]]:
    groups: dict[tuple[Any, ...], dict[str, Any]] = {}
    for row in probe_draws:
        if not numeric_value(row, "alpha_blend"):
            continue
        key = (
            row.get("seq"),
            row.get("encoder"),
            blend_signature_text(row),
            row.get("vsout"),
            row.get("shader_variant"),
            row.get("pso"),
        )
        group = groups.setdefault(
            key,
            {
                "seq": row.get("seq"),
                "encoder": row.get("encoder"),
                "signature": blend_signature_text(row),
                "vsout": row.get("vsout"),
                "shader_variant": row.get("shader_variant"),
                "pso": row.get("pso"),
                "draws": 0,
                "primitives": 0,
                "vertices": 0,
                "large_4096_draws": 0,
                "large_4096_primitives": 0,
                "scissor_draws": 0,
                "depth_write_draws": 0,
                "max_effective_stream0_byte_span": 0,
                "stream0_handles": set(),
                "index_buffers": set(),
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
        group["max_effective_stream0_byte_span"] = max(
            numeric_value(group, "max_effective_stream0_byte_span"),
            numeric_value(row, "effective_stream0_byte_span"),
        )
        stream0_handle = row.get("stream0_handle")
        if stream0_handle not in (None, ""):
            group["stream0_handles"].add(stream0_handle)
        index_buffer = row.get("index_buffer")
        if index_buffer not in (None, ""):
            group["index_buffers"].add(index_buffer)

    rows = list(groups.values())
    for row in rows:
        row["stream0_handle_unique"] = len(row.pop("stream0_handles"))
        row["index_buffer_unique"] = len(row.pop("index_buffers"))
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
            psos = current.pop("psos")
            shader_variants = current.pop("shader_variants")
            vsouts = current.pop("vsouts")
            stream0_handles = current.pop("stream0_handles")
            index_buffers = current.pop("index_buffers")
            current["pso_unique"] = len(psos)
            current["shader_variant_unique"] = len(shader_variants)
            current["vsout_unique"] = len(vsouts)
            current["stream0_handle_unique"] = len(stream0_handles)
            current["index_buffer_unique"] = len(index_buffers)
            current["_psos"] = psos
            current["_shader_variants"] = shader_variants
            current["_vsouts"] = vsouts
            current["_stream0_handles"] = stream0_handles
            current["_index_buffers"] = index_buffers
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
                "max_effective_stream0_byte_span": 0,
                "psos": set(),
                "shader_variants": set(),
                "vsouts": set(),
                "stream0_handles": set(),
                "index_buffers": set(),
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
        current["max_effective_stream0_byte_span"] = max(
            numeric_value(current, "max_effective_stream0_byte_span"),
            numeric_value(row, "effective_stream0_byte_span"),
        )
        pso = row.get("pso")
        if pso not in (None, ""):
            current["psos"].add(pso)
        shader_variant = row.get("shader_variant")
        if shader_variant not in (None, ""):
            current["shader_variants"].add(shader_variant)
        vsout = row.get("vsout")
        if vsout not in (None, ""):
            current["vsouts"].add(vsout)
        stream0_handle = row.get("stream0_handle")
        if stream0_handle not in (None, ""):
            current["stream0_handles"].add(stream0_handle)
        index_buffer = row.get("index_buffer")
        if index_buffer not in (None, ""):
            current["index_buffers"].add(index_buffer)

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
                "max_effective_stream0_byte_span": 0,
                "max_run_draws": 0,
                "max_run_primitives": 0,
                "max_run_first_draw": None,
                "max_run_last_draw": None,
                "psos": set(),
                "shader_variants": set(),
                "vsouts": set(),
                "stream0_handles": set(),
                "index_buffers": set(),
            },
        )
        group["runs"] += 1
        group["draws"] += numeric_value(run, "draws")
        group["primitives"] += numeric_value(run, "primitives")
        group["large_4096_draws"] += numeric_value(run, "large_4096_draws")
        group["large_4096_primitives"] += numeric_value(run, "large_4096_primitives")
        group["scissor_draws"] += numeric_value(run, "scissor_draws")
        group["depth_write_draws"] += numeric_value(run, "depth_write_draws")
        group["max_effective_stream0_byte_span"] = max(
            numeric_value(group, "max_effective_stream0_byte_span"),
            numeric_value(run, "max_effective_stream0_byte_span"),
        )
        group["psos"].update(run.get("_psos", set()))
        group["shader_variants"].update(run.get("_shader_variants", set()))
        group["vsouts"].update(run.get("_vsouts", set()))
        group["stream0_handles"].update(run.get("_stream0_handles", set()))
        group["index_buffers"].update(run.get("_index_buffers", set()))
        run_primitives = numeric_value(run, "primitives")
        if run_primitives > numeric_value(group, "max_run_primitives"):
            group["max_run_draws"] = numeric_value(run, "draws")
            group["max_run_primitives"] = run_primitives
            group["max_run_first_draw"] = run.get("first_draw")
            group["max_run_last_draw"] = run.get("last_draw")

    rows = list(groups.values())
    for row in rows:
        row["pso_unique"] = len(row.pop("psos"))
        row["shader_variant_unique"] = len(row.pop("shader_variants"))
        row["vsout_unique"] = len(row.pop("vsouts"))
        row["stream0_handle_unique"] = len(row.pop("stream0_handles"))
        row["index_buffer_unique"] = len(row.pop("index_buffers"))

    return sorted(
        rows,
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
            original_candidate_misses = numeric_value(
                row, f"indexed_cache_opt_candidate_original_miss{cache_size}")
            candidate_misses = numeric_value(
                row, f"indexed_cache_opt_candidate_miss{cache_size}")
            candidate_delta = candidate_misses - original_candidate_misses
            row[f"indexed_cache_opt_candidate_miss_delta_{cache_size}"] = (
                candidate_delta)
            row[f"indexed_cache_opt_candidate_miss_delta_pct_{cache_size}"] = (
                candidate_delta / original_candidate_misses * 100.0
                if original_candidate_misses else 0.0)


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
    render_pass_reentry_rows: list[dict[str, Any]] | None = None,
    render_pass_reentry_csv: Path | None = None,
    frame_rows: list[dict[str, Any]] | None = None,
    frame_csv: Path | None = None,
) -> None:
    counters = result.get("dxmt9_perf_counters", {})
    bridge = result.get("dxmt9_bridge_counters", {})
    probe_draws = probe_draws or []
    render_pass_reentry_rows = render_pass_reentry_rows or []
    frame_rows = frame_rows or []
    present_encoded = counters.get("present_encoded")
    render_pass_reentry_summary_rows = render_pass_reentry_rows
    if isinstance(present_encoded, (int, float)) and present_encoded > 0:
        render_pass_reentry_summary_rows = [
            row for row in render_pass_reentry_rows
            if numeric_value(row, "last_seq") <= present_encoded
        ]
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
    lines.append(f"- Render-pass re-entry lines: `{len(render_pass_reentry_rows)}`")
    if render_pass_reentry_rows:
        lines.append(
            "- Render-pass re-entry counter-window lines: "
            f"`{len(render_pass_reentry_summary_rows)}`"
        )
    if render_pass_reentry_csv is not None:
        lines.append(f"- Render-pass re-entry CSV: `{render_pass_reentry_csv}`")
    lines.append(f"- Frame sampling lines: `{len(frame_rows)}`")
    if frame_csv is not None:
        lines.append(f"- Frame sampling CSV: `{frame_csv}`")
    lines.append("")

    lines.append("## Run Counters")
    lines.append("")
    lines.append("| Counter | Value |")
    lines.append("|---|---:|")
    for key in RUN_COUNTERS:
        lines.append(f"| `{key}` | `{fmt(counters.get(key))}` |")
    lines.append("")

    lines.append("## Correctness / Visual-Coupling Counters")
    lines.append("")
    lines.append(
        "Use this block when bloom, muzzle, glow, or alpha-material visuals change. "
        "A timing gain can be a correctness win if skipped draws, error paths, "
        "fallbacks, overflows, pass churn, or waits also move."
    )
    lines.append("")
    lines.append("| Scope | Counter | Value |")
    lines.append("|---|---|---:|")
    for key in CORRECTNESS_VISUAL_RUN_KEYS:
        lines.append(f"| run | `{key}` | `{fmt(counters.get(key))}` |")
    if encoders:
        for key in CORRECTNESS_VISUAL_ENCODER_KEYS:
            lines.append(f"| encoder_sum | `{key}` | `{fmt(sum_key(encoders, key))}` |")
    lines.append("")

    sampled_frames = [row for row in frame_rows if numeric_value(row, "wall_ms") > 0]
    if sampled_frames:
        total_wall_ms = sum(numeric_value(row, "wall_ms") for row in sampled_frames)
        average_fps = (
            len(sampled_frames) * 1000.0 / total_wall_ms
            if total_wall_ms > 0 else 0.0
        )
        slow_frames = sorted(
            sampled_frames,
            key=lambda row: numeric_value(row, "wall_ms"),
            reverse=True,
        )[:16]
        lines.append("## Frame Sampling / Low-FPS Windows")
        lines.append("")
        lines.append(
            "`DXMT9_PERF_FRAME_SAMPLING=1` emits wall-clock Present deltas. "
            "Use this with effect/bloom/muzzle trace rows from the same run; "
            "a visual fix is performance-relevant only when slow-frame wall time "
            "or its wait/pass/draw counters move."
        )
        lines.append("")
        lines.append("| Metric | Value |")
        lines.append("|---|---:|")
        lines.append(f"| `sampled_frames` | `{fmt(len(sampled_frames))}` |")
        lines.append(f"| `sampled_wall_ms` | `{fmt(total_wall_ms)}` |")
        lines.append(f"| `sampled_avg_fps` | `{fmt(average_fps)}` |")
        lines.append("")
        lines.append(
            "| frame | wall ms | fps | draws | passes | command buffers | "
            "completion wait | present wait | GPU CB ms | encoder GPU ms | sub CBs | errors |"
        )
        lines.append("|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
        for row in slow_frames:
            present_wait = (
                numeric_value(row, "present_acquire_wait_ms") +
                numeric_value(row, "present_boundary_wait_ms") +
                numeric_value(row, "present_token_wait_ms")
            )
            passes = (
                f"{fmt(row.get('render_pass_begin'))}/"
                f"{fmt(row.get('render_pass_end'))}"
            )
            lines.append(
                "| "
                + " | ".join(
                    [
                        fmt(row.get("frame")),
                        fmt(row.get("wall_ms")),
                        fmt(row.get("fps")),
                        fmt(row.get("draw_calls")),
                        passes,
                        fmt(row.get("command_buffers")),
                        fmt(row.get("completion_wait_ms")),
                        fmt(present_wait),
                        fmt(row.get("gpu_command_buffer_time_ms")),
                        fmt(row.get("render_encoder_gpu_time_ms")),
                        fmt(row.get("sub_command_buffers")),
                        fmt(row.get("gpu_command_buffer_errors")),
                    ]
                )
                + " |"
            )
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

    reentry_total = counters.get("render_pass_same_key_reentry")
    lines.append("### Same-Key Re-entry Distance")
    lines.append("")
    lines.append("| Counter | Value | Share of re-entry |")
    lines.append("|---|---:|---:|")
    for key in RENDER_PASS_REENTRY_DISTANCE_KEYS:
        value = counters.get(key)
        lines.append(f"| `{key}` | `{fmt(value)}` | `{pct(value, reentry_total)}` |")
    lines.append("")

    distance1_total = counters.get("render_pass_same_key_reentry_distance_1")
    lines.append("### Same-Key Re-entry Distance-1 Shape")
    lines.append("")
    lines.append("| Shape | Count | Share of distance-1 | Preservation bytes |")
    lines.append("|---|---:|---:|---:|")
    for count_key, bytes_key, label in RENDER_PASS_REENTRY_DISTANCE1_SHAPE_KEYS:
        count_value = counters.get(count_key)
        bytes_value = counters.get(bytes_key)
        lines.append(
            f"| {label} | `{fmt(count_value)}` | `{pct(count_value, distance1_total)}` | "
            f"`{fmt(bytes_value)}` |"
        )
    lines.append("")

    if render_pass_reentry_summary_rows:
        pattern_rows = aggregate_render_pass_reentry_patterns(render_pass_reentry_summary_rows)
        aggregate_rows = aggregate_render_pass_reentry_rows(render_pass_reentry_summary_rows)
        touch_rows = aggregate_render_pass_reentry_touch_distances(
            render_pass_reentry_summary_rows)
        touch_total_count = sum(numeric_value(row, "count") for row in touch_rows)
        touch_total_bytes = sum(numeric_value(row, "preservation_bytes") for row in touch_rows)
        lines.append("### Same-Key Re-entry Touch Distance Distribution")
        lines.append("")
        lines.append(
            "| B next touch | A next touch | Count | Share | Preservation bytes | "
            "Byte share | Patterns |"
        )
        lines.append("|---|---|---:|---:|---:|---:|---:|")
        for row in touch_rows[:10]:
            lines.append(
                f"| `{touch_distance_text(row, 'b_color_touch_distance', 'b_depth_touch_distance')}` | "
                f"`{touch_distance_text(row, 'a_color_touch_distance', 'a_depth_touch_distance')}` | "
                f"`{fmt(row.get('count'))}` | `{pct(row.get('count'), touch_total_count)}` | "
                f"`{fmt(row.get('preservation_bytes'))}` | "
                f"`{pct(row.get('preservation_bytes'), touch_total_bytes)}` | "
                f"`{fmt(row.get('patterns'))}` |"
            )
        lines.append("")

        if encoders:
            role_rows = aggregate_render_pass_reentry_encoder_roles(
                render_pass_reentry_summary_rows, encoders)
            role_total_count = sum(numeric_value(row, "count") for row in role_rows)
            role_total_bytes = sum(numeric_value(row, "preservation_bytes") for row in role_rows)
            lines.append("### Same-Key Re-entry Encoder Role Pairs")
            lines.append("")
            lines.append(
                "| A1 role | B role | A2 role | A1 pass action | B pass action | "
                "A2 pass action | B next touch | A2 next touch | Count | Share | "
                "Preservation bytes | Byte share | Avg A1 draws | Avg B draws | "
                "Avg A2 draws | Avg A1 primitives | Avg B primitives | Avg A2 primitives | "
                "Rows | Missing encoders |"
            )
            lines.append(
                "|---|---|---|---|---|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|"
            )
            for row in role_rows[:12]:
                missing = (
                    numeric_value(row, "missing_prior_a_encoder_rows") +
                    numeric_value(row, "missing_b_encoder_rows") +
                    numeric_value(row, "missing_a_encoder_rows")
                )
                lines.append(
                    f"| `{row.get('prior_a_role')}` | "
                    f"`{row.get('b_role')}` | `{row.get('a_role')}` | "
                    f"`{row.get('prior_a_pass_action')}` | "
                    f"`{row.get('b_pass_action')}` | `{row.get('a_pass_action')}` | "
                    f"`{touch_distance_text(row, 'b_color_touch_distance', 'b_depth_touch_distance')}` | "
                    f"`{touch_distance_text(row, 'a_color_touch_distance', 'a_depth_touch_distance')}` | "
                    f"`{fmt(row.get('count'))}` | `{pct(row.get('count'), role_total_count)}` | "
                    f"`{fmt(row.get('preservation_bytes'))}` | "
                    f"`{pct(row.get('preservation_bytes'), role_total_bytes)}` | "
                    f"`{fmt(row.get('prior_a_draws_avg'))}` | "
                    f"`{fmt(row.get('b_draws_avg'))}` | `{fmt(row.get('a_draws_avg'))}` | "
                    f"`{fmt(row.get('prior_a_primitives_avg'))}` | "
                    f"`{fmt(row.get('b_primitives_avg'))}` | `{fmt(row.get('a_primitives_avg'))}` | "
                    f"`{fmt(row.get('rows'))}` | `{fmt(missing)}` |"
                )
            lines.append("")

        lines.append("### Same-Key Re-entry Top Patterns")
        lines.append("")
        lines.append(
            "| B depth | A depth | B->A encoder path | Count | Preservation bytes | "
            "Bytes/event | B reads A | A reads B | B store proof | A store proof | "
            "B next touch | A next touch | Frames | Last seq |"
        )
        lines.append("|---|---|---|---:|---:|---:|---|---|---|---|---|---|---:|---:|")
        for row in pattern_rows[:10]:
            lines.append(
                f"| `{row.get('b_depth', '')}` | `{row.get('a_depth', '')}` | "
                f"`{row.get('b_encoder', '')}->{row.get('a_encoder', '')}` | "
                f"`{fmt(row.get('count'))}` | `{fmt(row.get('preservation_bytes'))}` | "
                f"`{fmt(row.get('bytes_per_event'))}` | "
                f"`{read_relation_text(row, 'b_reads_a_color', 'b_reads_a_depth')}` | "
                f"`{read_relation_text(row, 'a_reads_b_color', 'a_reads_b_depth')}` | "
                f"`{proof_text(row, 'b_color_proof', 'b_depth_proof')}` | "
                f"`{proof_text(row, 'a_color_proof', 'a_depth_proof')}` | "
                f"`{touch_distance_text(row, 'b_color_touch_distance', 'b_depth_touch_distance')}` | "
                f"`{touch_distance_text(row, 'a_color_touch_distance', 'a_depth_touch_distance')}` | "
                f"`{fmt(row.get('frames'))}` | "
                f"`{fmt(row.get('last_seq'))}` |"
            )
        lines.append("")

        lines.append("### Same-Key Re-entry Top A/B Pairs")
        lines.append("")
        lines.append(
            "| A RT | A depth | B RT | B depth | Count | Preservation bytes | "
            "B reads A | A reads B | B store proof | A store proof | "
            "B next touch | A next touch | Frames | Last B->A seq/encoder |"
        )
        lines.append("|---|---|---|---|---:|---:|---|---|---|---|---|---|---:|---|")
        for row in aggregate_rows[:10]:
            last_b = f"{row.get('last_b_seq', '')}/{row.get('last_b_encoder', '')}"
            last_a = f"{row.get('last_seq', '')}/{row.get('last_encoder', '')}"
            last_path = f"{last_b}->{last_a}" if row.get("last_b_seq", "") else last_a
            lines.append(
                f"| `{row.get('a_rt', '')}` | `{row.get('a_depth', '')}` | "
                f"`{row.get('b_rt', '')}` | `{row.get('b_depth', '')}` | "
                f"`{fmt(row.get('count'))}` | `{fmt(row.get('preservation_bytes'))}` | "
                f"`{read_relation_text(row, 'b_reads_a_color', 'b_reads_a_depth')}` | "
                f"`{read_relation_text(row, 'a_reads_b_color', 'a_reads_b_depth')}` | "
                f"`{proof_text(row, 'b_color_proof', 'b_depth_proof')}` | "
                f"`{proof_text(row, 'a_color_proof', 'a_depth_proof')}` | "
                f"`{touch_distance_text(row, 'b_color_touch_distance', 'b_depth_touch_distance')}` | "
                f"`{touch_distance_text(row, 'a_color_touch_distance', 'a_depth_touch_distance')}` | "
                f"`{fmt(row.get('frames'))}` | "
                f"`{last_path}` |"
            )
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

        pass_preservation_keys = (
            "color_load_bytes",
            "color_store_bytes",
            "depth_load_bytes",
            "depth_store_bytes",
            "stencil_load_bytes",
            "stencil_store_bytes",
        )
        pass_preservation_total = sum(sum_key(encoders, key) for key in pass_preservation_keys)
        lines.append("## Encoder Pass Preservation Split")
        lines.append("")
        lines.append("| Metric | Sum | Share |")
        lines.append("|---|---:|---:|")
        for key in pass_preservation_keys:
            value = sum_key(encoders, key)
            lines.append(f"| `{key}` | `{fmt(value)}` | `{pct(value, pass_preservation_total)}` |")
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
            ("transient_vertex_staged_stream_bytes", transient_vertex_total),
            ("transient_index_user_bytes", transient_index_total),
            ("transient_index_preupload_bytes", transient_index_total),
            ("transient_index_shadow_fallback_bytes", transient_index_total),
            ("transient_index_probe_reorder_bytes", transient_index_total),
            ("transient_index_optimized_order_bytes", transient_index_total),
            ("transient_index_staged_ib_bytes", transient_index_total),
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
        unique_nonempty = lambda key: len({
            row.get(key) for row in probe_draws if row.get(key) not in (None, "")
        })
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
        lines.append(
            f"| `vs_constants_hash_unique` | "
            f"`{fmt(unique_nonempty('vs_constants_hash'))}` |"
        )
        lines.append(
            f"| `ps_constants_hash_unique` | "
            f"`{fmt(unique_nonempty('ps_constants_hash'))}` |"
        )
        lines.append(
            f"| `uniform_payload_hash_unique` | "
            f"`{fmt(unique_nonempty('uniform_payload_hash'))}` |"
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

        blend_material_rows = alpha_blend_material_rows(probe_draws)
        if blend_material_rows:
            lines.append("### Alpha Blend Material Breakdown")
            lines.append("")
            lines.append(
                "| seq | enc | signature | VSOut | shader | PSO | draws | prims | verts | "
                "large4096 draws/prims | scissor draws | depth-write draws | "
                "stream0 handles | IBs | max stream span |"
            )
            lines.append(
                "|---:|---:|---|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|"
            )
            for row in blend_material_rows[:24]:
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
                            fmt(row.get("vsout")),
                            fmt(row.get("shader_variant")),
                            fmt(row.get("pso")),
                            fmt(row.get("draws")),
                            fmt(row.get("primitives")),
                            fmt(row.get("vertices")),
                            large,
                            fmt(row.get("scissor_draws")),
                            fmt(row.get("depth_write_draws")),
                            fmt(row.get("stream0_handle_unique")),
                            fmt(row.get("index_buffer_unique")),
                            fmt(row.get("max_effective_stream0_byte_span")),
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
                    "max run draws/prims | large4096 draws/prims | scissor draws | "
                    "depth-write draws | PSOs | shaders | VSOuts | stream0 handles | IBs | "
                    "max stream span |"
                )
                lines.append(
                    "|---:|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|"
                    "---:|---:|---:|---:|---:|---:|"
                )
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
                                fmt(row.get("pso_unique")),
                                fmt(row.get("shader_variant_unique")),
                                fmt(row.get("vsout_unique")),
                                fmt(row.get("stream0_handle_unique")),
                                fmt(row.get("index_buffer_unique")),
                                fmt(row.get("max_effective_stream0_byte_span")),
                            ]
                        )
                        + " |"
                    )
                lines.append("")

            lines.append("### Alpha Blend Signature Runs")
            lines.append("")
            lines.append(
                "| seq | enc | first draw | last draw | signature | draws | prims | "
                "large4096 draws/prims | scissor draws | depth-write draws | PSOs | "
                "shaders | VSOuts | stream0 handles | IBs | max stream span |"
            )
            lines.append(
                "|---:|---:|---:|---:|---|---:|---:|---:|---:|---:|---:|"
                "---:|---:|---:|---:|---:|"
            )
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
                            fmt(row.get("shader_variant_unique")),
                            fmt(row.get("vsout_unique")),
                            fmt(row.get("stream0_handle_unique")),
                            fmt(row.get("index_buffer_unique")),
                            fmt(row.get("max_effective_stream0_byte_span")),
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
    render_pass_reentry_csv = output.parent / "3dmark05-perf-render-pass-reentry.csv"
    frame_csv = output.parent / "3dmark05-perf-frames.csv"
    log_path = run_dir / "dxmt9.log"
    encoders, streams = parse_encoder_lines(log_path)
    probe_draws = parse_probe_draw_lines(log_path)
    render_pass_reentry_rows = parse_render_pass_reentry_lines(log_path)
    frame_rows = parse_frame_lines(log_path)
    if not log_path.exists():
        encoders = load_existing_csv(encoder_csv)
        streams = load_existing_csv(stream_csv)
        probe_draws = load_existing_csv(probe_draw_csv)
        render_pass_reentry_rows = load_existing_csv(render_pass_reentry_csv)
        frame_rows = load_existing_csv(frame_csv)
    enrich_encoder_rows(encoders)
    if log_path.exists() or not encoder_csv.exists():
        write_csv(encoder_csv, encoders, ENCODER_CSV_KEYS)
    if log_path.exists() or not stream_csv.exists():
        write_csv(stream_csv, streams, STREAM_CSV_KEYS)
    if log_path.exists() or not probe_draw_csv.exists():
        write_csv(probe_draw_csv, probe_draws, PROBE_DRAW_CSV_KEYS)
    if log_path.exists() or not render_pass_reentry_csv.exists():
        write_csv(render_pass_reentry_csv, render_pass_reentry_rows, RENDER_PASS_REENTRY_CSV_KEYS)
    if log_path.exists() or not frame_csv.exists():
        write_csv(frame_csv, frame_rows, FRAME_CSV_KEYS)
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
        render_pass_reentry_rows,
        render_pass_reentry_csv,
        frame_rows,
        frame_csv,
    )
    print(output)
    return 0


if __name__ == "__main__":
    sys.exit(main())
