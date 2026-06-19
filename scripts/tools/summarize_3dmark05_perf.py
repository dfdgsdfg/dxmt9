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
HEX_TOKEN_RE = re.compile(r"^0x[0-9a-fA-F]+$")
SUMMARY_COUNTER_ROW_RE = re.compile(r"^\| `([^`]+)` \| `([^`]+)` \|")
ENCODER_PREFIX = "[dxmt9-perf-encoder "
STREAM_PREFIX = "[dxmt9-perf-encoder-stream "
PROBE_DRAW_PREFIX = "[dxmt9-perf-indexed-probe-draw "
RENDER_PASS_REENTRY_PREFIX = "[dxmt9-perf-render-pass-reentry "
FRAME_PREFIX = "[dxmt9-perf-frame "
ARGBUF_DELTA_SOURCE_PREFIX = "[dxmt9-perf-argbuf-payload-delta-source "
VS_CONST_SETTER_RANGE_PREFIX = "[dxmt9-perf-vs-const-setter-range "
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
    "submit_present_cpu_ms",
    "submit_present_cpu_p50_ms",
    "submit_present_cpu_p95_ms",
    "submit_present_acquire_cpu_ms",
    "submit_present_acquire_cpu_p50_ms",
    "submit_present_acquire_cpu_p95_ms",
    "submit_present_commit_cpu_ms",
    "submit_present_commit_cpu_p50_ms",
    "submit_present_commit_cpu_p95_ms",
    "submit_present_boundary_cpu_ms",
    "submit_present_boundary_cpu_p50_ms",
    "submit_present_boundary_cpu_p95_ms",
    "prepare_slot_publish_cpu_ms",
    "prepare_slot_publish_cpu_p50_ms",
    "prepare_slot_publish_cpu_p95_ms",
    "prepare_slot_resource_mark_cpu_ms",
    "prepare_slot_resource_mark_cpu_p50_ms",
    "prepare_slot_resource_mark_cpu_p95_ms",
    "prepare_slot_pso_prefetch_cpu_ms",
    "prepare_slot_pso_prefetch_cpu_p50_ms",
    "prepare_slot_pso_prefetch_cpu_p95_ms",
    "chunk_publish_reason_unknown",
    "chunk_publish_reason_draw_limit",
    "chunk_publish_reason_payload_limit",
    "chunk_publish_reason_present",
    "chunk_publish_reason_present_acquire",
    "chunk_publish_reason_present_split_before",
    "chunk_publish_reason_flush",
    "chunk_publish_reason_stretch_split",
    "chunk_publish_reason_map_wait",
    "chunk_publish_commands_unknown",
    "chunk_publish_commands_draw_limit",
    "chunk_publish_commands_payload_limit",
    "chunk_publish_commands_present",
    "chunk_publish_commands_present_acquire",
    "chunk_publish_commands_present_split_before",
    "chunk_publish_commands_flush",
    "chunk_publish_commands_stretch_split",
    "chunk_publish_commands_map_wait",
    "chunk_publish_present_split_before_tail_empty",
    "chunk_publish_present_split_before_tail_draw_run",
    "chunk_publish_present_split_before_tail_clear",
    "chunk_publish_present_split_before_tail_surface_copy",
    "chunk_publish_present_split_before_tail_stretch_rect",
    "chunk_publish_present_split_before_tail_readback",
    "chunk_publish_present_split_before_tail_color_fill",
    "chunk_publish_present_split_before_tail_depth_resolve",
    "chunk_publish_present_split_before_tail_present",
    "chunk_publish_present_split_before_draw_only",
    "chunk_publish_slot_residency_samples",
    "chunk_publish_slot_residency_ms",
    "chunk_publish_slot_residency_max_ms",
    "chunk_publish_slot_residency_p50_ms",
    "chunk_publish_slot_residency_p95_ms",
    "chunk_publish_slot_residency_present_samples",
    "chunk_publish_slot_residency_present_ms",
    "chunk_publish_slot_residency_present_max_ms",
    "chunk_publish_slot_residency_present_p50_ms",
    "chunk_publish_slot_residency_present_p95_ms",
    "chunk_publish_slot_residency_nonpresent_samples",
    "chunk_publish_slot_residency_nonpresent_ms",
    "chunk_publish_slot_residency_nonpresent_max_ms",
    "chunk_publish_slot_residency_nonpresent_p50_ms",
    "chunk_publish_slot_residency_nonpresent_p95_ms",
    "chunk_publish_present_pre_present_opportunity_slots",
    "chunk_publish_present_pre_present_opportunity_tail_slots",
    "chunk_publish_present_pre_present_opportunity_nontail_slots",
    "chunk_publish_present_pre_present_opportunity_commands",
    "chunk_publish_present_pre_present_opportunity_draw_runs",
    "chunk_publish_present_pre_present_opportunity_draw_items",
    "chunk_publish_present_pre_present_opportunity_non_draw_commands",
    "chunk_publish_present_pre_present_opportunity_payload_bytes",
    "chunk_publish_present_pre_present_opportunity_residency_ms",
    "chunk_publish_present_pre_present_opportunity_residency_max_ms",
    "chunk_publish_present_pre_present_opportunity_residency_p50_ms",
    "chunk_publish_present_pre_present_opportunity_residency_p95_ms",
    "chunk_publish_present_pre_present_opportunity_tail_empty",
    "chunk_publish_present_pre_present_opportunity_tail_draw_run",
    "chunk_publish_present_pre_present_opportunity_tail_clear",
    "chunk_publish_present_pre_present_opportunity_tail_surface_copy",
    "chunk_publish_present_pre_present_opportunity_tail_stretch_rect",
    "chunk_publish_present_pre_present_opportunity_tail_readback",
    "chunk_publish_present_pre_present_opportunity_tail_color_fill",
    "chunk_publish_present_pre_present_opportunity_tail_depth_resolve",
    "chunk_publish_present_pre_present_opportunity_tail_present",
    "chunk_publish_present_pre_present_opportunity_draw_only",
    "completion_no_enqueue_first_publish_slot_samples",
    "completion_no_enqueue_first_publish_slot_commands",
    "completion_no_enqueue_first_publish_slot_commands_p50",
    "completion_no_enqueue_first_publish_slot_commands_p95",
    "completion_no_enqueue_first_publish_slot_draw_run_commands",
    "completion_no_enqueue_first_publish_slot_draw_items",
    "completion_no_enqueue_first_publish_slot_draw_items_p50",
    "completion_no_enqueue_first_publish_slot_draw_items_p95",
    "completion_no_enqueue_first_publish_slot_non_draw_commands",
    "completion_no_enqueue_first_publish_slot_payload_bytes",
    "completion_no_enqueue_first_publish_slot_payload_bytes_p50",
    "completion_no_enqueue_first_publish_slot_payload_bytes_p95",
    "completion_no_enqueue_first_publish_slot_present_commands",
    "encode_slot_pso_prefetch_cpu_ms",
    "encode_slot_pso_prefetch_cpu_p50_ms",
    "encode_slot_pso_prefetch_cpu_p95_ms",
    "encode_slot_pso_prefetch_commands",
    "encode_slot_pso_prefetch_candidates",
    "encode_slot_pso_prefetch_tile_candidates",
    "encode_slot_pso_prefetch_argbuf_stage2_candidates",
    "encode_slot_pso_prefetch_argbuf_resource_array_candidates",
    "encode_slot_pso_prefetch_state_copy_cpu_ms",
    "encode_slot_pso_prefetch_state_copy_cpu_p50_ms",
    "encode_slot_pso_prefetch_state_copy_cpu_p95_ms",
    "encode_slot_pso_prefetch_depth_lookup_cpu_ms",
    "encode_slot_pso_prefetch_depth_lookup_cpu_p50_ms",
    "encode_slot_pso_prefetch_depth_lookup_cpu_p95_ms",
    "encode_slot_pso_prefetch_tile_select_cpu_ms",
    "encode_slot_pso_prefetch_tile_select_cpu_p50_ms",
    "encode_slot_pso_prefetch_tile_select_cpu_p95_ms",
    "encode_slot_pso_prefetch_tile_base_lookup_cpu_ms",
    "encode_slot_pso_prefetch_tile_base_lookup_cpu_p50_ms",
    "encode_slot_pso_prefetch_tile_base_lookup_cpu_p95_ms",
    "encode_slot_pso_prefetch_tile_draw_lookup_cpu_ms",
    "encode_slot_pso_prefetch_tile_draw_lookup_cpu_p50_ms",
    "encode_slot_pso_prefetch_tile_draw_lookup_cpu_p95_ms",
    "encode_slot_pso_prefetch_argbuf_select_cpu_ms",
    "encode_slot_pso_prefetch_argbuf_select_cpu_p50_ms",
    "encode_slot_pso_prefetch_argbuf_select_cpu_p95_ms",
    "encode_slot_pso_prefetch_draw_key_resolve_cpu_ms",
    "encode_slot_pso_prefetch_draw_resolve_format_cpu_ms",
    "encode_slot_pso_prefetch_draw_resolve_variant_key_cpu_ms",
    "encode_slot_pso_prefetch_draw_resolve_shader_context_cpu_ms",
    "encode_slot_pso_prefetch_draw_resolve_x8_alpha_cpu_ms",
    "encode_slot_pso_prefetch_draw_resolve_vsout_layout_cpu_ms",
    "encode_slot_pso_prefetch_draw_resolve_fragmentless_cpu_ms",
    "encode_slot_pso_prefetch_draw_lookup_cpu_ms",
    "encode_slot_pso_prefetch_draw_lookup_cpu_p50_ms",
    "encode_slot_pso_prefetch_draw_lookup_cpu_p95_ms",
    "encode_slot_pso_prefetch_draw_semantic_key_cpu_ms",
    "encode_slot_pso_prefetch_draw_semantic_probe_cpu_ms",
    "encode_slot_pso_prefetch_draw_semantic_store_cpu_ms",
    "encode_slot_pso_prefetch_draw_semantic_memo_hits",
    "encode_slot_pso_prefetch_draw_semantic_memo_misses",
    "encode_slot_pso_prefetch_draw_semantic_memo_overflow",
    "encode_slot_pso_prefetch_draw_semantic_miss_probe_key_hits",
    "encode_slot_pso_prefetch_draw_semantic_miss_probe_key_same_semantic",
    "encode_slot_pso_prefetch_draw_semantic_miss_probe_key_diff_argbuf_selector",
    "encode_slot_pso_prefetch_draw_semantic_miss_probe_key_diff_vertex_decl",
    "encode_slot_pso_prefetch_draw_semantic_miss_probe_key_diff_shader",
    "encode_slot_pso_prefetch_draw_semantic_miss_probe_key_diff_render_state",
    "encode_slot_pso_prefetch_draw_semantic_miss_probe_key_diff_texture_handles",
    "encode_slot_pso_prefetch_draw_semantic_miss_probe_key_diff_texture_lod",
    "encode_slot_pso_prefetch_draw_semantic_miss_probe_key_diff_texture_stage",
    "encode_slot_pso_prefetch_draw_semantic_miss_probe_key_diff_sampler",
    "encode_slot_pso_prefetch_draw_semantic_miss_probe_key_diff_attachment",
    "encode_slot_pso_prefetch_draw_semantic_miss_probe_key_diff_clip_plane",
    "encode_slot_pso_prefetch_draw_semantic_miss_probe_key_diff_constant_usage",
    "encode_slot_pso_prefetch_draw_semantic_miss_probe_key_diff_single_field",
    "encode_slot_pso_prefetch_draw_semantic_miss_probe_key_diff_multi_field",
    "encode_slot_pso_prefetch_draw_semantic_miss_probe_key_diff_texture_handles_only",
    "encode_slot_pso_prefetch_draw_semantic_miss_probe_key_diff_texture_handles_with_others",
    "encode_slot_pso_prefetch_draw_semantic_miss_probe_key_diff_hash_only",
    "encode_slot_pso_prefetch_draw_semantic_miss_probe_key_diff_unknown",
    "encode_slot_pso_prefetch_draw_resource_shape_memo_candidates",
    "encode_slot_pso_prefetch_draw_resource_shape_memo_hits",
    "encode_slot_pso_prefetch_draw_resource_shape_memo_misses",
    "encode_slot_pso_prefetch_draw_resource_shape_memo_overflow",
    "encode_slot_pso_prefetch_draw_resource_shape_memo_stores",
    "encode_slot_pso_prefetch_draw_resource_shape_memo_validated_hits",
    "encode_slot_pso_prefetch_draw_resource_shape_memo_validated_misses",
    "encode_slot_pso_prefetch_draw_resource_shape_memo_mismatch_texture_mask",
    "encode_slot_pso_prefetch_draw_resource_shape_memo_mismatch_texture_types",
    "encode_slot_pso_prefetch_draw_resource_shape_memo_mismatch_x8_alpha",
    "encode_slot_pso_prefetch_draw_resource_shape_memo_mismatch_attachment",
    "encode_slot_pso_prefetch_draw_resource_shape_memo_mismatch_sampler_lod_bias",
    "encode_slot_pso_prefetch_draw_resource_shape_memo_mismatch_vsout",
    "encode_slot_pso_prefetch_draw_resource_shape_memo_mismatch_other",
    "encode_slot_pso_prefetch_draw_probe_key_memo_hits",
    "encode_slot_pso_prefetch_draw_probe_key_memo_misses",
    "encode_slot_pso_prefetch_draw_probe_key_memo_overflow",
    "encode_slot_pso_prefetch_draw_handle_adjacent_candidates",
    "encode_slot_pso_prefetch_draw_handle_adjacent_hits",
    "encode_slot_pso_prefetch_draw_handle_slot_repeat_hits",
    "encode_slot_pso_prefetch_draw_handle_slot_unique",
    "encode_slot_pso_prefetch_draw_handle_slot_overflow",
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
    "d3d9_draw_state_cache_direct_hits",
    "d3d9_draw_state_cache_direct_misses",
    "d3d9_draw_state_cache_direct_hit_with_index",
    "d3d9_draw_state_cache_direct_miss_with_index",
    "d3d9_draw_state_cache_direct_hit_no_index",
    "d3d9_draw_state_cache_direct_miss_no_index",
    "d3d9_draw_state_cache_batch_hits",
    "d3d9_draw_state_cache_batch_misses",
    "d3d9_draw_state_cache_uniform_refreshes",
    "d3d9_draw_state_cache_batch_miss_reason_unknown",
    "d3d9_draw_state_cache_batch_miss_reason_binding_only",
    "d3d9_draw_state_cache_batch_miss_reason_single_render_state",
    "d3d9_draw_state_cache_batch_miss_reason_single_texture",
    "d3d9_draw_state_cache_batch_miss_reason_single_fvf_vdecl",
    "d3d9_draw_state_cache_batch_miss_reason_single_shader",
    "d3d9_draw_state_cache_batch_miss_reason_single_rt_depth",
    "d3d9_draw_state_cache_batch_miss_reason_single_viewport_scissor",
    "d3d9_draw_state_cache_batch_miss_reason_single_tss_sampler",
    "d3d9_draw_state_cache_batch_miss_reason_single_ffp_clip",
    "d3d9_draw_state_cache_batch_miss_reason_single_broad",
    "d3d9_draw_state_cache_batch_miss_reason_mixed_2",
    "d3d9_draw_state_cache_batch_miss_reason_mixed_3",
    "d3d9_draw_state_cache_batch_miss_reason_mixed_4plus",
    "d3d9_draw_state_cache_batch_miss_reason_has_render_state",
    "d3d9_draw_state_cache_batch_miss_reason_has_texture",
    "d3d9_draw_state_cache_batch_miss_reason_has_fvf_vdecl",
    "d3d9_draw_state_cache_batch_miss_reason_has_shader",
    "d3d9_draw_state_cache_batch_miss_reason_has_rt_depth",
    "d3d9_draw_state_cache_batch_miss_reason_has_viewport_scissor",
    "d3d9_draw_state_cache_batch_miss_reason_has_tss_sampler",
    "d3d9_draw_state_cache_batch_miss_reason_has_ffp_clip",
    "d3d9_draw_state_cache_batch_miss_reason_has_broad",
    "d3d9_draw_state_cache_batch_miss_reason_has_texture_shader",
    "d3d9_draw_state_cache_batch_miss_reason_has_texture_fvf_vdecl",
    "d3d9_draw_state_cache_batch_miss_reason_has_shader_fvf_vdecl",
    "d3d9_draw_state_cache_batch_miss_reason_has_texture_tss_sampler",
    "d3d9_draw_state_cache_batch_miss_reason_has_texture_shader_fvf_vdecl",
    "d3d9_draw_state_cache_batch_miss_reason_has_texture_shader_tss_sampler",
    "d3d9_draw_state_cache_batch_miss_reason_has_texture_fvf_vdecl_tss_sampler",
    "d3d9_draw_state_cache_batch_miss_reason_has_shader_fvf_vdecl_tss_sampler",
    "d3d9_draw_state_cache_batch_miss_reason_has_texture_shader_fvf_vdecl_tss_sampler",
    "d3d9_snapshot_cache_batch_miss_semantic_reuse_probe_samples",
    "d3d9_snapshot_cache_batch_miss_semantic_reuse_probe_hits",
    "d3d9_snapshot_cache_batch_miss_semantic_reuse_probe_misses",
    "d3d9_snapshot_cache_batch_miss_semantic_reuse_probe_hit_distance_1",
    "d3d9_snapshot_cache_batch_miss_semantic_reuse_probe_hit_distance_2",
    "d3d9_snapshot_cache_batch_miss_semantic_reuse_probe_hit_distance_3_4",
    "d3d9_snapshot_cache_batch_miss_semantic_reuse_probe_hit_distance_5_8",
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
    "d3d9_snapshot_cache_direct_hit_cpu_ms",
    "d3d9_snapshot_cache_direct_miss_cpu_ms",
    "d3d9_snapshot_cache_batch_hit_cpu_ms",
    "d3d9_snapshot_cache_batch_miss_cpu_ms",
    "d3d9_snapshot_cache_binding_layout_cpu_ms",
    "d3d9_snapshot_cache_uniform_refresh_cpu_ms",
    "d3d9_snapshot_cache_uniform_build_cpu_ms",
    "d3d9_snapshot_cache_uniform_hash_cpu_ms",
    "d3d9_snapshot_cache_miss_shader_layout_cpu_ms",
    "d3d9_snapshot_cache_miss_uniform_build_cpu_ms",
    "d3d9_snapshot_cache_miss_hot_build_cpu_ms",
    "d3d9_snapshot_cache_direct_miss_shader_layout_cpu_ms",
    "d3d9_snapshot_cache_direct_miss_uniform_build_cpu_ms",
    "d3d9_snapshot_cache_direct_miss_hot_build_cpu_ms",
    "d3d9_snapshot_cache_batch_miss_shader_layout_cpu_ms",
    "d3d9_snapshot_cache_batch_miss_uniform_build_cpu_ms",
    "d3d9_snapshot_cache_batch_miss_hot_build_cpu_ms",
    "d3d9_snapshot_cache_batch_miss_hot_build_zero_init_cpu_ms",
    "d3d9_snapshot_cache_batch_miss_hot_build_key_cpu_ms",
    "d3d9_snapshot_cache_batch_miss_hot_build_key_zero_init_cpu_ms",
    "d3d9_snapshot_cache_batch_miss_hot_build_key_stream_cpu_ms",
    "d3d9_snapshot_cache_batch_miss_hot_build_key_shader_cpu_ms",
    "d3d9_snapshot_cache_batch_miss_hot_build_key_constant_cpu_ms",
    "d3d9_snapshot_cache_batch_miss_hot_build_key_texture_cpu_ms",
    "d3d9_snapshot_cache_batch_miss_hot_build_key_sampler_cpu_ms",
    "d3d9_snapshot_cache_batch_miss_hot_build_key_render_state_cpu_ms",
    "d3d9_snapshot_cache_batch_miss_hot_build_key_attachment_cpu_ms",
    "d3d9_snapshot_cache_batch_miss_hot_build_key_uniform_cpu_ms",
    "d3d9_snapshot_cache_batch_miss_hot_build_binding_copy_cpu_ms",
    "d3d9_snapshot_cache_batch_miss_hot_build_render_state_cpu_ms",
    "d3d9_snapshot_cache_batch_miss_hot_build_texture_stage_state_cpu_ms",
    "d3d9_snapshot_cache_batch_miss_hot_build_sampler_state_cpu_ms",
    "d3d9_snapshot_cache_batch_miss_hot_build_tail_copy_cpu_ms",
    "d3d9_snapshot_cache_batch_miss_hot_build_flat_render_reuse_hits",
    "d3d9_snapshot_cache_batch_miss_hot_build_flat_render_reuse_misses",
    "d3d9_snapshot_cache_batch_miss_hot_build_flat_tss_reuse_hits",
    "d3d9_snapshot_cache_batch_miss_hot_build_flat_tss_reuse_misses",
    "d3d9_snapshot_cache_batch_miss_hot_build_flat_sampler_reuse_hits",
    "d3d9_snapshot_cache_batch_miss_hot_build_flat_sampler_reuse_misses",
    "d3d9_snapshot_cache_batch_miss_uniform_nonconst_hash_reuse_hits",
    "d3d9_snapshot_cache_batch_miss_uniform_nonconst_hash_reuse_misses",
    "d3d9_snapshot_cache_batch_miss_uniform_payload_reuse_full",
    "d3d9_snapshot_cache_batch_miss_uniform_payload_reuse_nonconst",
    "d3d9_snapshot_cache_batch_miss_uniform_payload_full_build",
    "d3d9_snapshot_cache_batch_miss_uniform_build_calls",
    "d3d9_snapshot_cache_batch_miss_uniform_build_vs_const_copy_cpu_ms",
    "d3d9_snapshot_cache_batch_miss_uniform_build_ps_const_copy_cpu_ms",
    "d3d9_snapshot_cache_batch_miss_uniform_build_ffp_matrix_cpu_ms",
    "d3d9_snapshot_cache_batch_miss_uniform_build_ffp_material_light_cpu_ms",
    "d3d9_snapshot_cache_batch_miss_uniform_build_texture_transform_cpu_ms",
    "d3d9_snapshot_cache_batch_miss_uniform_build_clip_plane_cpu_ms",
    "d3d9_snapshot_cache_batch_miss_uniform_build_hash_cpu_ms",
    "d3d9_snapshot_cache_batch_miss_uniform_build_vs_const_hash_cpu_ms",
    "d3d9_snapshot_cache_batch_miss_uniform_build_ps_const_hash_cpu_ms",
    "d3d9_snapshot_cache_batch_miss_uniform_build_nonconst_hash_cpu_ms",
    "d3d9_snapshot_cache_batch_miss_uniform_build_nonconst_hash_world_view_proj_cpu_ms",
    "d3d9_snapshot_cache_batch_miss_uniform_build_nonconst_hash_ffp_world_view_cpu_ms",
    "d3d9_snapshot_cache_batch_miss_uniform_build_nonconst_hash_ffp_normal_matrix_cpu_ms",
    "d3d9_snapshot_cache_batch_miss_uniform_build_nonconst_hash_material_cpu_ms",
    "d3d9_snapshot_cache_batch_miss_uniform_build_nonconst_hash_lights_cpu_ms",
    "d3d9_snapshot_cache_batch_miss_uniform_build_nonconst_hash_ffp_blend_wvp_cpu_ms",
    "d3d9_snapshot_cache_batch_miss_uniform_build_nonconst_hash_texture_transforms_cpu_ms",
    "d3d9_snapshot_cache_batch_miss_uniform_build_nonconst_hash_clip_planes_cpu_ms",
    "d3d9_snapshot_cache_batch_miss_uniform_build_payload_combine_hash_cpu_ms",
    "d3d9_snapshot_cache_batch_miss_uniform_build_vs_const_hash_full",
    "d3d9_snapshot_cache_batch_miss_uniform_build_ps_const_hash_full",
    "d3d9_snapshot_cache_batch_miss_uniform_build_vs_const_hash_full_no_usage",
    "d3d9_snapshot_cache_batch_miss_uniform_build_ps_const_hash_full_no_usage",
    "d3d9_snapshot_cache_batch_miss_uniform_build_vs_const_hash_full_unknown",
    "d3d9_snapshot_cache_batch_miss_uniform_build_ps_const_hash_full_unknown",
    "d3d9_snapshot_cache_batch_miss_uniform_build_vs_const_hash_full_unknown_bytecode",
    "d3d9_snapshot_cache_batch_miss_uniform_build_ps_const_hash_full_unknown_bytecode",
    "d3d9_snapshot_cache_batch_miss_uniform_build_vs_const_hash_full_unknown_non_bytecode",
    "d3d9_snapshot_cache_batch_miss_uniform_build_ps_const_hash_full_unknown_non_bytecode",
    "d3d9_snapshot_cache_batch_miss_uniform_build_vs_const_hash_full_indexed_float",
    "d3d9_snapshot_cache_batch_miss_uniform_build_vs_const_hash_full_indexed_float_min_safe_bytes",
    "d3d9_snapshot_cache_batch_miss_uniform_build_vs_const_hash_full_indexed_float_potential_saved_bytes",
    "d3d9_snapshot_cache_batch_miss_uniform_build_ps_const_hash_full_indexed_float",
    "d3d9_snapshot_cache_batch_miss_uniform_build_vs_const_hash_full_indexed_int",
    "d3d9_snapshot_cache_batch_miss_uniform_build_ps_const_hash_full_indexed_int",
    "d3d9_snapshot_cache_batch_miss_uniform_build_vs_const_hash_full_indexed_bool",
    "d3d9_snapshot_cache_batch_miss_uniform_build_ps_const_hash_full_indexed_bool",
    "d3d9_snapshot_cache_batch_miss_uniform_build_vs_const_hash_bytes",
    "d3d9_snapshot_cache_batch_miss_uniform_build_ps_const_hash_bytes",
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
    "d3d9_snapshot_uniform_build_vs_const_hash_full_indexed_float_min_safe_bytes",
    "d3d9_snapshot_uniform_build_vs_const_hash_full_indexed_float_potential_saved_bytes",
    "d3d9_snapshot_uniform_build_ps_const_hash_full_indexed_float",
    "d3d9_snapshot_uniform_build_vs_const_hash_full_indexed_int",
    "d3d9_snapshot_uniform_build_ps_const_hash_full_indexed_int",
    "d3d9_snapshot_uniform_build_vs_const_hash_full_indexed_bool",
    "d3d9_snapshot_uniform_build_ps_const_hash_full_indexed_bool",
    "d3d9_snapshot_uniform_build_vs_const_hash_bytes",
    "d3d9_snapshot_uniform_build_ps_const_hash_bytes",
    "d3d9_snapshot_uniform_copy_cpu_ms",
    "d3d9_snapshot_uniform_materialized",
    "d3d9_snapshot_uniform_materialized_bytes",
    "d3d9_snapshot_uniform_materialized_compact_candidate_bytes",
    "d3d9_snapshot_uniform_materialized_compact_saved_bytes",
    "d3d9_snapshot_uniform_materialized_compact_fixed_bytes",
    "d3d9_snapshot_uniform_materialized_compact_vertex_bytes",
    "d3d9_snapshot_uniform_materialized_compact_pixel_bytes",
    "d3d9_snapshot_uniform_compact_cpu_ms",
    "d3d9_snapshot_uniform_compact_fixed_cpu_ms",
    "d3d9_snapshot_uniform_compact_vertex_stage_cpu_ms",
    "d3d9_snapshot_uniform_compact_pixel_stage_cpu_ms",
    "d3d9_snapshot_uniform_compact_fixed_payload_appends",
    "d3d9_snapshot_uniform_compact_fixed_payload_reuses",
    "d3d9_snapshot_uniform_compact_fixed_payload_reuse_saved_bytes",
    "d3d9_snapshot_submission_carrier_records",
    "d3d9_snapshot_submission_carrier_bytes",
    "d3d9_snapshot_submission_carrier_state_storage_bytes",
    "d3d9_snapshot_submission_carrier_uniform_storage_bytes",
    "d3d9_snapshot_submission_carrier_compact_uniform_storage_bytes",
    "d3d9_snapshot_submission_carrier_unused_uniform_storage_records",
    "d3d9_snapshot_submission_carrier_unused_uniform_storage_bytes",
    "d3d9_snapshot_uniform_elided",
    "d3d9_snapshot_uniform_elided_bytes",
    "d3d9_snapshot_uniform_adjacent_same_generation",
    "d3d9_snapshot_uniform_adjacent_same_generation_bytes",
    "d3d9_snapshot_uniform_adjacent_same_generation_same_state_lane",
    "d3d9_snapshot_uniform_adjacent_same_generation_same_state_lane_bytes",
    "d3d9_snapshot_uniform_adjacent_same_generation_diff_state_lane",
    "d3d9_snapshot_uniform_adjacent_same_generation_diff_state_lane_bytes",
    "d3d9_snapshot_uniform_adjacent_same_payload_hash",
    "d3d9_snapshot_uniform_adjacent_same_payload_hash_bytes",
    "d3d9_snapshot_uniform_adjacent_same_payload_hash_same_state_lane",
    "d3d9_snapshot_uniform_adjacent_same_payload_hash_same_state_lane_bytes",
    "d3d9_snapshot_uniform_adjacent_same_payload_hash_diff_state_lane",
    "d3d9_snapshot_uniform_adjacent_same_payload_hash_diff_state_lane_bytes",
    "d3d9_snapshot_uniform_adjacent_same_payload_hash_diff_generation",
    "d3d9_snapshot_uniform_adjacent_same_payload_hash_diff_generation_bytes",
    "d3d9_snapshot_uniform_adjacent_previous_payload",
    "d3d9_snapshot_uniform_adjacent_same_vs_const_hash",
    "d3d9_snapshot_uniform_adjacent_same_vs_const_hash_same_state_lane",
    "d3d9_snapshot_uniform_adjacent_same_vs_const_hash_diff_generation",
    "d3d9_snapshot_uniform_adjacent_same_ps_const_hash",
    "d3d9_snapshot_uniform_adjacent_same_ps_const_hash_same_state_lane",
    "d3d9_snapshot_uniform_adjacent_same_ps_const_hash_diff_generation",
    "d3d9_snapshot_uniform_adjacent_same_shader_const_hashes",
    "d3d9_snapshot_uniform_adjacent_same_shader_const_hashes_same_state_lane",
    "d3d9_snapshot_uniform_adjacent_same_shader_const_hashes_diff_generation",
    "d3d9_snapshot_uniform_adjacent_same_fixed_payload_hash",
    "d3d9_snapshot_uniform_adjacent_same_fixed_payload_hash_same_state_lane",
    "d3d9_snapshot_uniform_adjacent_same_fixed_payload_hash_diff_generation",
    "d3d9_snapshot_uniform_adjacent_same_fixed_and_shader_const_hashes",
    "d3d9_snapshot_uniform_adjacent_same_fixed_and_shader_const_hashes_same_state_lane",
    "d3d9_snapshot_uniform_adjacent_same_fixed_and_shader_const_hashes_diff_generation",
    "d3d9_snapshot_state_copy_cpu_ms",
    "d3d9_snapshot_state_materialized",
    "d3d9_snapshot_state_materialized_bytes",
    "d3d9_snapshot_state_elided",
    "d3d9_snapshot_state_elided_bytes",
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
    "draw_uniform_payload_lookup_semantic_hash_misses",
    "draw_uniform_payload_lookup_semantic_hash_miss_bytes",
    "draw_uniform_payload_lookup_cpu_ms",
    "draw_uniform_payload_lookup_cpu_max_ms",
    "draw_uniform_payload_lookup_bucket_cpu_ms",
    "draw_uniform_payload_lookup_bucket_cpu_max_ms",
    "draw_uniform_payload_appends",
    "draw_uniform_payload_append_bytes",
    "draw_uniform_fixed_payload_appends",
    "draw_uniform_fixed_payload_append_bytes",
    "draw_uniform_vertex_constants_appends",
    "draw_uniform_vertex_constants_append_bytes",
    "draw_uniform_pixel_constants_appends",
    "draw_uniform_pixel_constants_append_bytes",
    "draw_uniform_payload_materialized",
    "draw_uniform_payload_materialized_bytes",
    "draw_uniform_payload_materialize_fallbacks",
    "draw_uniform_payload_materialize_cpu_ms",
    "draw_uniform_payload_materialize_cpu_max_ms",
    "draw_uniform_payload_materialized_other",
    "draw_uniform_payload_materialized_other_bytes",
    "draw_uniform_payload_materialize_other_cpu_ms",
    "draw_uniform_payload_materialized_draw_encoder_command",
    "draw_uniform_payload_materialized_draw_encoder_command_bytes",
    "draw_uniform_payload_materialize_draw_encoder_command_cpu_ms",
    "draw_uniform_payload_materialized_draw_encoder_param",
    "draw_uniform_payload_materialized_draw_encoder_param_bytes",
    "draw_uniform_payload_materialize_draw_encoder_param_cpu_ms",
    "draw_uniform_payload_materialized_framegraph_command",
    "draw_uniform_payload_materialized_framegraph_command_bytes",
    "draw_uniform_payload_materialize_framegraph_command_cpu_ms",
    "draw_uniform_payload_materialized_framegraph_param",
    "draw_uniform_payload_materialized_framegraph_param_bytes",
    "draw_uniform_payload_materialize_framegraph_param_cpu_ms",
    "draw_uniform_payload_materialized_queue_observation",
    "draw_uniform_payload_materialized_queue_observation_bytes",
    "draw_uniform_payload_materialize_queue_observation_cpu_ms",
    "draw_uniform_payload_append_reserve_cpu_ms",
    "draw_uniform_payload_append_reserve_cpu_max_ms",
    "draw_uniform_payload_append_copy_cpu_ms",
    "draw_uniform_payload_append_copy_cpu_max_ms",
    "draw_uniform_payload_append_link_cpu_ms",
    "draw_uniform_payload_append_link_cpu_max_ms",
    "submit_draw_run_batch_groups",
    "submit_draw_run_batch_records",
    "submit_draw_run_batch_max_records",
    "submit_draw_run_batch_discarded_state_records",
    "submit_draw_run_batch_discarded_state_bytes",
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
    "submit_draw_run_batch_submission_adjacent_pairs",
    "submit_draw_run_batch_submission_adjacent_same_generation_lane",
    "submit_draw_run_batch_compat_pairs",
    "submit_draw_run_batch_compat_compatible",
    "submit_draw_run_batch_compat_incompatible",
    "submit_draw_run_batch_compat_same_generation_lane",
    "submit_draw_run_batch_compat_same_generation_lane_compatible",
    "submit_draw_run_batch_compat_same_generation_lane_incompatible",
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
    "submit_draw_run_batch_append_reserve_cpu_ms",
    "submit_draw_run_batch_append_reserve_cpu_max_ms",
    "submit_draw_run_batch_append_state_cpu_ms",
    "submit_draw_run_batch_append_state_cpu_max_ms",
    "submit_draw_run_batch_append_state_pso_cpu_ms",
    "submit_draw_run_batch_append_state_pso_cpu_max_ms",
    "submit_draw_run_batch_append_state_invariant_cpu_ms",
    "submit_draw_run_batch_append_state_invariant_cpu_max_ms",
    "submit_draw_run_batch_append_state_soa_cpu_ms",
    "submit_draw_run_batch_append_state_soa_cpu_max_ms",
    "submit_draw_run_batch_append_uniform_cpu_ms",
    "submit_draw_run_batch_append_uniform_cpu_max_ms",
    "submit_draw_run_batch_append_payload_cpu_ms",
    "submit_draw_run_batch_append_payload_cpu_max_ms",
    "submit_draw_run_batch_append_param_cpu_ms",
    "submit_draw_run_batch_append_param_cpu_max_ms",
    "submit_draw_run_batch_append_record_cpu_ms",
    "submit_draw_run_batch_append_record_cpu_max_ms",
    "submit_draw_run_batch_append_payload_bytes",
    "submit_draw_run_batch_append_params",
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
    "encode_draw_argbuf_open_call_cpu_ms",
    "encode_draw_argbuf_reopen_post_cpu_ms",
    "encode_draw_argbuf_reopen_table_probe_cpu_ms",
    "encode_draw_argbuf_reopen_table_shadow_store_cpu_ms",
    "encode_draw_argbuf_reopen_byte_account_cpu_ms",
    "encode_draw_argbuf_reopen_cbuf_cache_probe_cpu_ms",
    "encode_draw_argbuf_reopen_cbuf_dirty_scan_cpu_ms",
    "encode_draw_argbuf_reopen_cbuf_force_dirty_cpu_ms",
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
    "encode_draw_argbuf_cbuf_full_repoint_cpu_ms",
    "encode_draw_argbuf_cbuf_cached_repoint_vs_cpu_ms",
    "encode_draw_argbuf_cbuf_cached_repoint_ps_cpu_ms",
    "encode_draw_argbuf_cbuf_cached_repoint_ffp_vs_cpu_ms",
    "encode_draw_argbuf_cbuf_cached_repoint_ffp_ps_cpu_ms",
    "encode_draw_argbuf_cbuf_cached_repoint_calls",
    "encode_draw_argbuf_cbuf_cached_repoint_bytes",
    "encode_draw_argbuf_cbuf_cached_repoint_vs_calls",
    "encode_draw_argbuf_cbuf_cached_repoint_ps_calls",
    "encode_draw_argbuf_cbuf_cached_repoint_ffp_vs_calls",
    "encode_draw_argbuf_cbuf_cached_repoint_ffp_ps_calls",
    "encode_draw_argbuf_cbuf_cached_repoint_vs_bytes",
    "encode_draw_argbuf_cbuf_cached_repoint_ps_bytes",
    "encode_draw_argbuf_cbuf_cached_repoint_ffp_vs_bytes",
    "encode_draw_argbuf_cbuf_cached_repoint_ffp_ps_bytes",
    "encode_draw_argbuf_cbuf_content_probe_cpu_ms",
    "encode_draw_argbuf_cbuf_content_probe_vs_cpu_ms",
    "encode_draw_argbuf_cbuf_content_probe_ps_cpu_ms",
    "encode_draw_argbuf_cbuf_content_probe_ffp_ps_cpu_ms",
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
    "encode_draw_argbuf_cbuf_dirty_vs_identity_probe_calls",
    "encode_draw_argbuf_cbuf_dirty_vs_identity_hits",
    "encode_draw_argbuf_cbuf_dirty_vs_identity_misses",
    "encode_draw_argbuf_cbuf_dirty_vs_identity_no_cache",
    "encode_draw_argbuf_cbuf_dirty_vs_identity_hit_bytes",
    "encode_draw_argbuf_cbuf_dirty_vs_identity_miss_bytes",
    "encode_draw_argbuf_payload_delta_probe_calls",
    "encode_draw_argbuf_payload_delta_first",
    "encode_draw_argbuf_payload_delta_same",
    "encode_draw_argbuf_payload_delta_changed",
    "encode_draw_argbuf_payload_delta_changed_vs",
    "encode_draw_argbuf_payload_delta_changed_ps",
    "encode_draw_argbuf_payload_delta_changed_vs_ps",
    "encode_draw_argbuf_payload_delta_changed_nonconst_only",
    "encode_draw_argbuf_payload_delta_changed_vs_float",
    "encode_draw_argbuf_payload_delta_changed_vs_int",
    "encode_draw_argbuf_payload_delta_changed_vs_bool",
    "encode_draw_argbuf_payload_delta_changed_ps_float",
    "encode_draw_argbuf_payload_delta_changed_ps_int",
    "encode_draw_argbuf_payload_delta_changed_ps_bool",
    "encode_draw_argbuf_payload_delta_changed_vs_float_regs",
    "encode_draw_argbuf_payload_delta_changed_vs_float_regs_max",
    "encode_draw_argbuf_payload_delta_changed_vs_float_regs_le1",
    "encode_draw_argbuf_payload_delta_changed_vs_float_regs_le4",
    "encode_draw_argbuf_payload_delta_changed_vs_float_regs_le16",
    "encode_draw_argbuf_payload_delta_changed_vs_float_regs_le64",
    "encode_draw_argbuf_payload_delta_changed_vs_float_regs_gt64",
    "encode_draw_argbuf_payload_delta_changed_vs_float_regs_le1_sum",
    "encode_draw_argbuf_payload_delta_changed_vs_float_regs_le4_sum",
    "encode_draw_argbuf_payload_delta_changed_vs_float_regs_le16_sum",
    "encode_draw_argbuf_payload_delta_changed_vs_float_regs_le64_sum",
    "encode_draw_argbuf_payload_delta_changed_vs_float_regs_gt64_sum",
    "encode_draw_argbuf_payload_delta_changed_vs_float_prefix_regs",
    "encode_draw_argbuf_payload_delta_changed_vs_float_prefix_regs_max",
    "encode_draw_argbuf_payload_delta_changed_vs_float_span_regs",
    "encode_draw_argbuf_payload_delta_changed_vs_float_span_regs_max",
    "encode_draw_argbuf_payload_delta_changed_vs_float_full_prefix",
    "encode_draw_argbuf_payload_delta_changed_vs_float_full_prefix_regs",
    "encode_draw_argbuf_payload_delta_changed_ps_float_regs",
    "encode_draw_argbuf_payload_delta_changed_ps_float_regs_max",
    "encode_draw_argbuf_payload_delta_changed_ps_float_regs_le1",
    "encode_draw_argbuf_payload_delta_changed_ps_float_regs_le4",
    "encode_draw_argbuf_payload_delta_changed_ps_float_regs_le16",
    "encode_draw_argbuf_payload_delta_changed_ps_float_regs_le64",
    "encode_draw_argbuf_payload_delta_changed_ps_float_regs_gt64",
    "encode_draw_argbuf_payload_delta_changed_ps_float_regs_le1_sum",
    "encode_draw_argbuf_payload_delta_changed_ps_float_regs_le4_sum",
    "encode_draw_argbuf_payload_delta_changed_ps_float_regs_le16_sum",
    "encode_draw_argbuf_payload_delta_changed_ps_float_regs_le64_sum",
    "encode_draw_argbuf_payload_delta_changed_ps_float_regs_gt64_sum",
    "encode_draw_argbuf_payload_delta_reopen_first",
    "encode_draw_argbuf_payload_delta_reopen_payload_changed",
    "encode_draw_argbuf_payload_delta_reopen_payload_same",
    "encode_draw_argbuf_payload_delta_reopen_resource_array",
    "encode_draw_argbuf_payload_delta_reopen_cbuf_only",
    "encode_draw_argbuf_payload_delta_reopen_cbuf_only_first",
    "encode_draw_argbuf_payload_delta_reopen_cbuf_only_payload_changed",
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
    "encode_draw_issue_indexed_cpu_ms",
    "encode_draw_issue_nonindexed_cpu_ms",
    "encode_draw_issue_expanded_indexed_cpu_ms",
    "encode_draw_issue_split_indexed_cpu_ms",
    "encode_draw_issue_metal_cpu_ms",
    "encode_draw_issue_visibility_cpu_ms",
    "indexed_cache_opt_candidate_draws",
    "indexed_cache_opt_candidate_skipped",
    "indexed_cache_opt_candidate_bytes",
    "indexed_cache_opt_candidate_original_miss32",
    "indexed_cache_opt_candidate_miss32",
    "indexed_cache_opt_candidate_gate_pass",
    "indexed_cache_opt_candidate_gate_fail",
    "indexed_cache_opt_candidate_opaque_depth_draws",
    "indexed_cache_opt_candidate_screen_blend_draws",
    "indexed_cache_opt_candidate_primitive_bucket_1_63",
    "indexed_cache_opt_candidate_primitive_bucket_64_255",
    "indexed_cache_opt_candidate_primitive_bucket_256_1023",
    "indexed_cache_opt_candidate_primitive_bucket_1024_4095",
    "indexed_cache_opt_candidate_primitive_bucket_4096_plus",
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
    "commit_chunk_queue_draw_submission_emplace_cpu_ms",
    "commit_chunk_queue_draw_submission_emplace_cpu_p50_ms",
    "commit_chunk_queue_draw_submission_emplace_cpu_p95_ms",
    "commit_chunk_queue_draw_submission_snapshot_cpu_ms",
    "commit_chunk_queue_draw_submission_snapshot_cpu_p50_ms",
    "commit_chunk_queue_draw_submission_snapshot_cpu_p95_ms",
    "commit_chunk_index_bind_cpu_ms",
    "commit_chunk_index_bind_cpu_p50_ms",
    "commit_chunk_index_bind_cpu_p95_ms",
    "commit_chunk_replay_pending_flush_cpu_ms",
    "commit_chunk_replay_pending_flush_cpu_p50_ms",
    "commit_chunk_replay_pending_flush_cpu_p95_ms",
    "commit_chunk_replay_draw_record_cpu_ms",
    "commit_chunk_replay_draw_record_cpu_p50_ms",
    "commit_chunk_replay_draw_record_cpu_p95_ms",
    "commit_chunk_replay_non_draw_record_cpu_ms",
    "commit_chunk_replay_non_draw_record_cpu_p50_ms",
    "commit_chunk_replay_non_draw_record_cpu_p95_ms",
    "commit_chunk_replay_const_record_cpu_ms",
    "commit_chunk_replay_const_record_cpu_p50_ms",
    "commit_chunk_replay_const_record_cpu_p95_ms",
    "commit_chunk_replay_apply_state_record_cpu_ms",
    "commit_chunk_replay_apply_state_record_cpu_p50_ms",
    "commit_chunk_replay_apply_state_record_cpu_p95_ms",
    "commit_chunk_replay_clear_record_cpu_ms",
    "commit_chunk_replay_clear_record_cpu_p50_ms",
    "commit_chunk_replay_clear_record_cpu_p95_ms",
    "commit_chunk_replay_present_record_cpu_ms",
    "commit_chunk_replay_present_record_cpu_p50_ms",
    "commit_chunk_replay_present_record_cpu_p95_ms",
    "commit_chunk_replay_surface_record_cpu_ms",
    "commit_chunk_replay_surface_record_cpu_p50_ms",
    "commit_chunk_replay_surface_record_cpu_p95_ms",
    "commit_chunk_replay_query_record_cpu_ms",
    "commit_chunk_replay_query_record_cpu_p50_ms",
    "commit_chunk_replay_query_record_cpu_p95_ms",
    "commit_chunk_replay_other_record_cpu_ms",
    "commit_chunk_replay_other_record_cpu_p50_ms",
    "commit_chunk_replay_other_record_cpu_p95_ms",
    "commit_chunk_const_upload_cpu_ms",
    "commit_chunk_const_upload_cpu_p50_ms",
    "commit_chunk_const_upload_cpu_p95_ms",
    "completion_enqueue_samples",
    "completion_enqueue_pending_depth_max",
    "completion_enqueue_while_waiting",
    "completion_enqueue_while_waiting_present",
    "completion_wait_with_enqueue",
    "completion_wait_with_enqueue_ms",
    "completion_wait_without_enqueue",
    "completion_wait_without_enqueue_ms",
    "completion_present_wait_with_enqueue",
    "completion_present_wait_with_enqueue_ms",
    "completion_present_wait_without_enqueue",
    "completion_present_wait_without_enqueue_ms",
    "completion_wait_enqueues_during_wait",
    "completion_wait_enqueues_during_wait_max",
    "completion_wait_commit_chunk_entries",
    "completion_wait_commit_chunk_replay_starts",
    "completion_wait_commit_chunk_replay_ends",
    "completion_wait_commit_chunk_replay_cpu_ms",
    "completion_wait_commit_chunk_replay_cpu_max_ms",
    "completion_wait_commit_chunk_replay_cpu_p50_ms",
    "completion_wait_commit_chunk_replay_cpu_p95_ms",
    "completion_no_enqueue_wait_to_commit_chunk_entry",
    "completion_no_enqueue_wait_to_commit_chunk_entry_ms",
    "completion_no_enqueue_wait_to_commit_chunk_entry_p50_ms",
    "completion_no_enqueue_wait_to_commit_chunk_entry_p95_ms",
    "completion_no_enqueue_wait_to_commit_chunk_replay_start",
    "completion_no_enqueue_wait_to_commit_chunk_replay_start_ms",
    "completion_no_enqueue_wait_to_commit_chunk_replay_start_p50_ms",
    "completion_no_enqueue_wait_to_commit_chunk_replay_start_p95_ms",
    "completion_no_enqueue_wait_to_commit_chunk_replay_end",
    "completion_no_enqueue_wait_to_commit_chunk_replay_end_ms",
    "completion_no_enqueue_wait_to_commit_chunk_replay_end_p50_ms",
    "completion_no_enqueue_wait_to_commit_chunk_replay_end_p95_ms",
    "completion_no_enqueue_wait_to_commit_publish",
    "completion_no_enqueue_wait_to_commit_publish_ms",
    "completion_no_enqueue_wait_to_commit_publish_p50_ms",
    "completion_no_enqueue_wait_to_commit_publish_p95_ms",
    "completion_no_enqueue_commit_chunk_entries_before_publish",
    "completion_no_enqueue_commit_chunk_entries_before_publish_max",
    "completion_no_enqueue_commit_chunk_entries_before_publish_p50",
    "completion_no_enqueue_commit_chunk_entries_before_publish_p95",
    "completion_no_enqueue_commit_chunk_replay_starts_before_publish",
    "completion_no_enqueue_commit_chunk_replay_starts_before_publish_max",
    "completion_no_enqueue_commit_chunk_replay_starts_before_publish_p50",
    "completion_no_enqueue_commit_chunk_replay_starts_before_publish_p95",
    "completion_no_enqueue_commit_chunk_replay_ends_before_publish",
    "completion_no_enqueue_commit_chunk_replay_ends_before_publish_max",
    "completion_no_enqueue_commit_chunk_replay_ends_before_publish_p50",
    "completion_no_enqueue_commit_chunk_replay_ends_before_publish_p95",
    "completion_no_enqueue_commit_chunk_completed_replay_cpu_before_publish",
    "completion_no_enqueue_commit_chunk_completed_replay_cpu_before_publish_ms",
    "completion_no_enqueue_commit_chunk_completed_replay_cpu_before_publish_max_ms",
    "completion_no_enqueue_commit_chunk_completed_replay_cpu_before_publish_p50_ms",
    "completion_no_enqueue_commit_chunk_completed_replay_cpu_before_publish_p95_ms",
    "completion_no_enqueue_commit_chunk_active_replay_cpu_before_publish",
    "completion_no_enqueue_commit_chunk_active_replay_cpu_before_publish_ms",
    "completion_no_enqueue_commit_chunk_active_replay_cpu_before_publish_max_ms",
    "completion_no_enqueue_commit_chunk_active_replay_cpu_before_publish_p50_ms",
    "completion_no_enqueue_commit_chunk_active_replay_cpu_before_publish_p95_ms",
    "completion_no_enqueue_commit_chunk_inter_replay_gap_before_publish",
    "completion_no_enqueue_commit_chunk_inter_replay_gap_before_publish_ms",
    "completion_no_enqueue_commit_chunk_inter_replay_gap_before_publish_max_ms",
    "completion_no_enqueue_commit_chunk_inter_replay_gap_before_publish_p50_ms",
    "completion_no_enqueue_commit_chunk_inter_replay_gap_before_publish_p95_ms",
    "completion_no_enqueue_commit_publish_wait_before_publish",
    "completion_no_enqueue_commit_publish_wait_before_publish_ms",
    "completion_no_enqueue_commit_publish_wait_before_publish_max_ms",
    "completion_no_enqueue_commit_publish_wait_before_publish_p50_ms",
    "completion_no_enqueue_commit_publish_wait_before_publish_p95_ms",
    "completion_no_enqueue_commit_publish_on_before_publish_cpu",
    "completion_no_enqueue_commit_publish_on_before_publish_cpu_ms",
    "completion_no_enqueue_commit_publish_on_before_publish_cpu_max_ms",
    "completion_no_enqueue_commit_publish_on_before_publish_cpu_p50_ms",
    "completion_no_enqueue_commit_publish_on_before_publish_cpu_p95_ms",
    "completion_no_enqueue_commit_chunk_shape_samples_before_publish",
    "completion_no_enqueue_commit_chunk_records_before_publish",
    "completion_no_enqueue_commit_chunk_records_before_publish_max",
    "completion_no_enqueue_commit_chunk_records_before_publish_p50",
    "completion_no_enqueue_commit_chunk_records_before_publish_p95",
    "completion_no_enqueue_commit_chunk_chunks_with_draw_before_publish",
    "completion_no_enqueue_commit_chunk_chunks_with_present_before_publish",
    "completion_no_enqueue_commit_chunk_chunks_state_const_only_before_publish",
    "completion_no_enqueue_commit_chunk_chunks_no_draw_no_present_before_publish",
    "completion_no_enqueue_commit_chunk_draw_records_before_publish",
    "completion_no_enqueue_commit_chunk_const_records_before_publish",
    "completion_no_enqueue_commit_chunk_apply_state_records_before_publish",
    "completion_no_enqueue_commit_chunk_clear_records_before_publish",
    "completion_no_enqueue_commit_chunk_present_records_before_publish",
    "completion_no_enqueue_commit_chunk_surface_records_before_publish",
    "completion_no_enqueue_commit_chunk_query_records_before_publish",
    "completion_no_enqueue_commit_chunk_other_records_before_publish",
    "completion_no_enqueue_first_publish_slot_samples",
    "completion_no_enqueue_first_publish_slot_commands",
    "completion_no_enqueue_first_publish_slot_commands_max",
    "completion_no_enqueue_first_publish_slot_commands_p50",
    "completion_no_enqueue_first_publish_slot_commands_p95",
    "completion_no_enqueue_first_publish_slot_draw_run_commands",
    "completion_no_enqueue_first_publish_slot_draw_items",
    "completion_no_enqueue_first_publish_slot_draw_items_max",
    "completion_no_enqueue_first_publish_slot_draw_items_p50",
    "completion_no_enqueue_first_publish_slot_draw_items_p95",
    "completion_no_enqueue_first_publish_slot_non_draw_commands",
    "completion_no_enqueue_first_publish_slot_payload_bytes",
    "completion_no_enqueue_first_publish_slot_payload_bytes_max",
    "completion_no_enqueue_first_publish_slot_payload_bytes_p50",
    "completion_no_enqueue_first_publish_slot_payload_bytes_p95",
    "completion_no_enqueue_first_publish_slot_present_commands",
    "completion_no_enqueue_wait_to_encode_dequeue",
    "completion_no_enqueue_wait_to_encode_dequeue_ms",
    "completion_no_enqueue_wait_to_encode_dequeue_p50_ms",
    "completion_no_enqueue_wait_to_encode_dequeue_p95_ms",
    "completion_no_enqueue_wait_to_command_buffer_commit",
    "completion_no_enqueue_wait_to_command_buffer_commit_ms",
    "completion_no_enqueue_wait_to_command_buffer_commit_p50_ms",
    "completion_no_enqueue_wait_to_command_buffer_commit_p95_ms",
    "encode_dequeue_ready_depth_samples",
    "encode_dequeue_ready_depth_total",
    "encode_dequeue_ready_depth_max",
    "encode_dequeue_ready_depth_gt1",
    "encode_dequeue_ready_depth_gt2",
    "encode_dequeue_ready_depth_gt4",
    "completion_no_enqueue_stage_commit_entry_to_publish",
    "completion_no_enqueue_stage_commit_entry_to_publish_ms",
    "completion_no_enqueue_stage_commit_entry_to_publish_p50_ms",
    "completion_no_enqueue_stage_commit_entry_to_publish_p95_ms",
    "completion_no_enqueue_stage_publish_to_encode_dequeue",
    "completion_no_enqueue_stage_publish_to_encode_dequeue_ms",
    "completion_no_enqueue_stage_publish_to_encode_dequeue_p50_ms",
    "completion_no_enqueue_stage_publish_to_encode_dequeue_p95_ms",
    "completion_no_enqueue_stage_encode_dequeue_to_command_buffer_commit",
    "completion_no_enqueue_stage_encode_dequeue_to_command_buffer_commit_ms",
    "completion_no_enqueue_stage_encode_dequeue_to_command_buffer_commit_p50_ms",
    "completion_no_enqueue_stage_encode_dequeue_to_command_buffer_commit_p95_ms",
    "completion_no_enqueue_wait_to_next_enqueue",
    "completion_no_enqueue_wait_to_next_enqueue_ms",
    "completion_no_enqueue_wait_to_next_enqueue_p50_ms",
    "completion_no_enqueue_wait_to_next_enqueue_p95_ms",
    "completion_no_enqueue_wait_to_next_present_enqueue",
    "completion_no_enqueue_wait_to_next_present_enqueue_ms",
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
    "indexed_cache_opt_candidate_gate_pass",
    "indexed_cache_opt_candidate_gate_fail",
    "indexed_cache_opt_candidate_opaque_depth_draws",
    "indexed_cache_opt_candidate_screen_blend_draws",
    "indexed_cache_opt_candidate_primitive_bucket_1_63",
    "indexed_cache_opt_candidate_primitive_bucket_64_255",
    "indexed_cache_opt_candidate_primitive_bucket_256_1023",
    "indexed_cache_opt_candidate_primitive_bucket_1024_4095",
    "indexed_cache_opt_candidate_primitive_bucket_4096_plus",
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
    "indexed_cache_opt_candidate_gate_pass",
    "indexed_cache_opt_candidate_gate_fail",
    "indexed_cache_opt_candidate_opaque_depth_draws",
    "indexed_cache_opt_candidate_screen_blend_draws",
    "indexed_cache_opt_candidate_primitive_bucket_1_63",
    "indexed_cache_opt_candidate_primitive_bucket_64_255",
    "indexed_cache_opt_candidate_primitive_bucket_256_1023",
    "indexed_cache_opt_candidate_primitive_bucket_1024_4095",
    "indexed_cache_opt_candidate_primitive_bucket_4096_plus",
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
    "route_depth_only_draws",
    "route_depth_only_primitives",
    "route_depth_only_vertices",
    "route_programmable_textured_draws",
    "route_programmable_textured_primitives",
    "route_programmable_textured_vertices",
    "route_programmable_color_draws",
    "route_programmable_color_primitives",
    "route_programmable_color_vertices",
    "route_alpha_blend_primitives",
    "route_alpha_test_primitives",
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
    "indexed_cache_opt_candidate_gate_pass",
    "indexed_cache_opt_candidate_gate_fail",
    "indexed_cache_opt_candidate_opaque_depth_draws",
    "indexed_cache_opt_candidate_screen_blend_draws",
    "indexed_cache_opt_candidate_primitive_bucket_1_63",
    "indexed_cache_opt_candidate_primitive_bucket_64_255",
    "indexed_cache_opt_candidate_primitive_bucket_256_1023",
    "indexed_cache_opt_candidate_primitive_bucket_1024_4095",
    "indexed_cache_opt_candidate_primitive_bucket_4096_plus",
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

ARGBUF_DELTA_SOURCE_CSV_KEYS = (
    "vs_hash",
    "ps_hash",
    "prefix_regs",
    "rows",
    "changed_regs",
    "span_regs",
    "full_prefix_rows",
    "full_prefix_regs",
    "overflow_rows",
    "overflow_changed_regs",
)

VS_CONST_SETTER_RANGE_CSV_KEYS = (
    "phase",
    "vs_hash",
    "ps_hash",
    "start",
    "count",
    "events",
    "range_regs",
    "changed_regs",
    "changed_span_regs",
    "full_range_events",
    "full_changed_events",
    "overflow_events",
    "overflow_range_regs",
    "overflow_changed_regs",
    "overflow_changed_span_regs",
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


def parsed_int_value(row: dict[str, Any], key: str) -> int | None:
    value = row.get(key)
    if isinstance(value, bool):
        return None
    if isinstance(value, int):
        return value
    return None


def parsed_hex_token(row: dict[str, Any], key: str) -> bool:
    value = row.get(key)
    return isinstance(value, str) and bool(HEX_TOKEN_RE.match(value))


def valid_vs_const_setter_range_row(row: dict[str, Any]) -> bool:
    phase = row.get("phase")
    if phase not in {"call", "flush"}:
        return False
    overflow = parsed_int_value(row, "overflow")
    if overflow == 1:
        return all(
            parsed_int_value(row, key) is not None
            for key in (
                "events",
                "range_regs",
                "changed_regs",
                "changed_span_regs",
                "full_range_events",
                "full_changed_events",
            )
        )
    if overflow != 0:
        return False
    if not parsed_hex_token(row, "vs_hash") or not parsed_hex_token(row, "ps_hash"):
        return False
    required_ints = (
        "start",
        "count",
        "events",
        "range_regs",
        "changed_regs",
        "changed_span_regs",
        "full_range_events",
        "full_changed_events",
    )
    if any(parsed_int_value(row, key) is None for key in required_ints):
        return False
    return parsed_int_value(row, "count") > 0


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


def parse_argbuf_delta_source_lines(log_path: Path) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    if not log_path.exists():
        return rows
    for line in log_path.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith(ARGBUF_DELTA_SOURCE_PREFIX):
            rows.append(parse_kv_line(line))
    return rows


def parse_vs_const_setter_range_lines(log_path: Path) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    if not log_path.exists():
        return rows
    for line in log_path.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith(VS_CONST_SETTER_RANGE_PREFIX):
            if not line.rstrip().endswith("]"):
                continue
            row = parse_kv_line(line)
            if valid_vs_const_setter_range_row(row):
                rows.append(row)
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


def require_uniform_compact_saved_bytes_present(result: dict[str, Any]) -> None:
    counters = result.get("dxmt9_perf_counters", {})
    if not isinstance(counters, dict):
        raise SystemExit("missing dxmt9_perf_counters")
    present = counters.get("present_encoded")
    saved = counters.get("d3d9_snapshot_uniform_materialized_compact_saved_bytes")
    saved_per_present = None
    if isinstance(present, (int, float)) and present > 0 and isinstance(saved, (int, float)):
        saved_per_present = saved / present
    if saved_per_present is None or saved_per_present <= 0.0:
        raise SystemExit(
            "uniform_compact_saved_bytes_per_present stayed zero "
            f"(present_encoded={fmt(present)}, "
            "d3d9_snapshot_uniform_materialized_compact_saved_bytes="
            f"{fmt(saved)})"
        )


def numeric_value(row: dict[str, Any], key: str) -> int | float:
    value = row.get(key, 0)
    if isinstance(value, (int, float)):
        return value
    return 0


def aggregate_argbuf_delta_sources(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    groups: dict[tuple[Any, Any, Any], dict[str, Any]] = {}
    for row in rows:
        if numeric_value(row, "overflow"):
            key = ("overflow", "overflow", "overflow")
            group = groups.setdefault(
                key,
                {
                    "vs_hash": "overflow",
                    "ps_hash": "overflow",
                    "prefix_regs": "overflow",
                    "rows": 0,
                    "changed_regs": 0,
                    "span_regs": 0,
                    "full_prefix_rows": 0,
                    "full_prefix_regs": 0,
                    "overflow_rows": 0,
                    "overflow_changed_regs": 0,
                },
            )
            group["overflow_rows"] += numeric_value(row, "rows")
            group["overflow_changed_regs"] += numeric_value(row, "changed_regs")
            continue
        key = (row.get("vs_hash", ""), row.get("ps_hash", ""), row.get("prefix_regs", ""))
        group = groups.setdefault(
            key,
            {
                "vs_hash": row.get("vs_hash", ""),
                "ps_hash": row.get("ps_hash", ""),
                "prefix_regs": row.get("prefix_regs", ""),
                "rows": 0,
                "changed_regs": 0,
                "span_regs": 0,
                "full_prefix_rows": 0,
                "full_prefix_regs": 0,
                "overflow_rows": 0,
                "overflow_changed_regs": 0,
            },
        )
        for field in (
            "rows",
            "changed_regs",
            "span_regs",
            "full_prefix_rows",
            "full_prefix_regs",
        ):
            group[field] += numeric_value(row, field)
    return sorted(
        groups.values(),
        key=lambda item: (
            numeric_value(item, "full_prefix_regs"),
            numeric_value(item, "changed_regs"),
            numeric_value(item, "rows"),
        ),
        reverse=True,
    )


def aggregate_vs_const_setter_ranges(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    groups: dict[tuple[Any, Any, Any, Any, Any], dict[str, Any]] = {}
    for row in rows:
        phase = row.get("phase", "")
        if numeric_value(row, "overflow"):
            key = (phase, "overflow", "overflow", "overflow", "overflow")
            group = groups.setdefault(
                key,
                {
                    "phase": phase,
                    "vs_hash": "overflow",
                    "ps_hash": "overflow",
                    "start": "overflow",
                    "count": "overflow",
                    "events": 0,
                    "range_regs": 0,
                    "changed_regs": 0,
                    "changed_span_regs": 0,
                    "full_range_events": 0,
                    "full_changed_events": 0,
                    "overflow_events": 0,
                    "overflow_range_regs": 0,
                    "overflow_changed_regs": 0,
                    "overflow_changed_span_regs": 0,
                },
            )
            group["overflow_events"] += numeric_value(row, "events")
            group["overflow_range_regs"] += numeric_value(row, "range_regs")
            group["overflow_changed_regs"] += numeric_value(row, "changed_regs")
            group["overflow_changed_span_regs"] += numeric_value(
                row, "changed_span_regs"
            )
            group["full_range_events"] += numeric_value(row, "full_range_events")
            group["full_changed_events"] += numeric_value(row, "full_changed_events")
            continue
        key = (
            phase,
            row.get("vs_hash", ""),
            row.get("ps_hash", ""),
            row.get("start", ""),
            row.get("count", ""),
        )
        group = groups.setdefault(
            key,
            {
                "phase": phase,
                "vs_hash": row.get("vs_hash", ""),
                "ps_hash": row.get("ps_hash", ""),
                "start": row.get("start", ""),
                "count": row.get("count", ""),
                "events": 0,
                "range_regs": 0,
                "changed_regs": 0,
                "changed_span_regs": 0,
                "full_range_events": 0,
                "full_changed_events": 0,
                "overflow_events": 0,
                "overflow_range_regs": 0,
                "overflow_changed_regs": 0,
                "overflow_changed_span_regs": 0,
            },
        )
        for field in (
            "events",
            "range_regs",
            "changed_regs",
            "changed_span_regs",
            "full_range_events",
            "full_changed_events",
        ):
            group[field] += numeric_value(row, field)
    return sorted(
        groups.values(),
        key=lambda item: (
            numeric_value(item, "changed_regs"),
            numeric_value(item, "range_regs"),
            numeric_value(item, "events"),
        ),
        reverse=True,
    )


def append_pacing_cpu_stage_derived(
    lines: list[str],
    counters: dict[str, Any],
    present_encoded: Any,
) -> None:
    completion_wait = counters.get("completion_wait_ms")
    completion_with_enqueue = counters.get("completion_wait_with_enqueue_ms")
    completion_without_enqueue = counters.get("completion_wait_without_enqueue_ms")
    overlap_share = None
    no_enqueue_share = None
    if isinstance(completion_wait, (int, float)) and completion_wait > 0:
        if isinstance(completion_with_enqueue, (int, float)):
            overlap_share = completion_with_enqueue / completion_wait * 100.0
        if isinstance(completion_without_enqueue, (int, float)):
            no_enqueue_share = completion_without_enqueue / completion_wait * 100.0

    stage_rows = (
        (
            "wait -> commit chunk entry",
            "completion_no_enqueue_wait_to_commit_chunk_entry_ms",
            "completion_no_enqueue_wait_to_commit_chunk_entry_p50_ms",
            "completion_no_enqueue_wait_to_commit_chunk_entry_p95_ms",
        ),
        (
            "commit entry -> publish",
            "completion_no_enqueue_stage_commit_entry_to_publish_ms",
            "completion_no_enqueue_stage_commit_entry_to_publish_p50_ms",
            "completion_no_enqueue_stage_commit_entry_to_publish_p95_ms",
        ),
        (
            "publish -> encode dequeue",
            "completion_no_enqueue_stage_publish_to_encode_dequeue_ms",
            "completion_no_enqueue_stage_publish_to_encode_dequeue_p50_ms",
            "completion_no_enqueue_stage_publish_to_encode_dequeue_p95_ms",
        ),
        (
            "encode dequeue -> command buffer commit",
            "completion_no_enqueue_stage_encode_dequeue_to_command_buffer_commit_ms",
            "completion_no_enqueue_stage_encode_dequeue_to_command_buffer_commit_p50_ms",
            "completion_no_enqueue_stage_encode_dequeue_to_command_buffer_commit_p95_ms",
        ),
        (
            "wait -> next enqueue",
            "completion_no_enqueue_wait_to_next_enqueue_ms",
            "completion_no_enqueue_wait_to_next_enqueue_p50_ms",
            "completion_no_enqueue_wait_to_next_enqueue_p95_ms",
        ),
    )
    timeline_rows = (
        (
            "wait -> commit chunk entry",
            "completion_no_enqueue_wait_to_commit_chunk_entry_ms",
            "completion_no_enqueue_wait_to_commit_chunk_entry_p50_ms",
            "completion_no_enqueue_wait_to_commit_chunk_entry_p95_ms",
        ),
        (
            "wait -> commit chunk replay start",
            "completion_no_enqueue_wait_to_commit_chunk_replay_start_ms",
            "completion_no_enqueue_wait_to_commit_chunk_replay_start_p50_ms",
            "completion_no_enqueue_wait_to_commit_chunk_replay_start_p95_ms",
        ),
        (
            "wait -> commit chunk replay end",
            "completion_no_enqueue_wait_to_commit_chunk_replay_end_ms",
            "completion_no_enqueue_wait_to_commit_chunk_replay_end_p50_ms",
            "completion_no_enqueue_wait_to_commit_chunk_replay_end_p95_ms",
        ),
        (
            "wait -> commit publish",
            "completion_no_enqueue_wait_to_commit_publish_ms",
            "completion_no_enqueue_wait_to_commit_publish_p50_ms",
            "completion_no_enqueue_wait_to_commit_publish_p95_ms",
        ),
        (
            "wait -> encode dequeue",
            "completion_no_enqueue_wait_to_encode_dequeue_ms",
            "completion_no_enqueue_wait_to_encode_dequeue_p50_ms",
            "completion_no_enqueue_wait_to_encode_dequeue_p95_ms",
        ),
        (
            "wait -> command buffer commit",
            "completion_no_enqueue_wait_to_command_buffer_commit_ms",
            "completion_no_enqueue_wait_to_command_buffer_commit_p50_ms",
            "completion_no_enqueue_wait_to_command_buffer_commit_p95_ms",
        ),
        (
            "wait -> next enqueue",
            "completion_no_enqueue_wait_to_next_enqueue_ms",
            "completion_no_enqueue_wait_to_next_enqueue_p50_ms",
            "completion_no_enqueue_wait_to_next_enqueue_p95_ms",
        ),
    )
    largest_row = "n/a"
    largest_row_value: float | None = None
    for label, _total_key, p50_key, _p95_key in stage_rows:
        value = counters.get(p50_key)
        if isinstance(value, (int, float)):
            if largest_row_value is None or value > largest_row_value:
                largest_row = label
                largest_row_value = value

    if no_enqueue_share is not None and no_enqueue_share >= 80.0:
        verdict = "under-pipelined-no-enqueue"
    elif overlap_share is not None and overlap_share >= 20.0:
        verdict = "overlap-active"
    else:
        verdict = "insufficient-p4-overlap-evidence"
    overlap_share_text = f"{fmt(overlap_share)}%" if overlap_share is not None else "n/a"
    no_enqueue_share_text = (
        f"{fmt(no_enqueue_share)}%" if no_enqueue_share is not None else "n/a"
    )

    lines.append("## Pacing / CPU Stage Derived")
    lines.append("")
    lines.append(
        "Use this block to decide whether an average-FPS change moved the "
        "P4 overlap problem, the replay/publish stage, or the backend encode "
        "stage. Current-run improvements should move these rows, not only a "
        "single leaf counter."
    )
    lines.append("")
    lines.append("| Metric | Value |")
    lines.append("|---|---:|")
    lines.append(
        "| `completion_wait_ms_per_present` | "
        f"`{ratio_text(completion_wait, present_encoded)}` |"
    )
    lines.append(
        "| `completion_wait_with_enqueue_ms_per_present` | "
        f"`{ratio_text(completion_with_enqueue, present_encoded)}` |"
    )
    lines.append(
        "| `completion_wait_without_enqueue_ms_per_present` | "
        f"`{ratio_text(completion_without_enqueue, present_encoded)}` |"
    )
    lines.append(
        "| `completion_wait_overlap_share` | "
        f"`{overlap_share_text}` |"
    )
    lines.append(
        "| `completion_wait_no_enqueue_share` | "
        f"`{no_enqueue_share_text}` |"
    )
    lines.append(
        "| `completion_enqueue_while_waiting_per_present` | "
        f"`{ratio_text(counters.get('completion_enqueue_while_waiting_present'), present_encoded)}` |"
    )
    lines.append(
        "| `completion_wait_commit_chunk_entries_per_present` | "
        f"`{ratio_text(counters.get('completion_wait_commit_chunk_entries'), present_encoded)}` |"
    )
    lines.append(
        "| `completion_wait_commit_chunk_replay_starts_per_present` | "
        f"`{ratio_text(counters.get('completion_wait_commit_chunk_replay_starts'), present_encoded)}` |"
    )
    lines.append(
        "| `completion_wait_commit_chunk_replay_ends_per_present` | "
        f"`{ratio_text(counters.get('completion_wait_commit_chunk_replay_ends'), present_encoded)}` |"
    )
    lines.append(
        "| `completion_wait_commit_chunk_replay_cpu_ms_per_present` | "
        f"`{ratio_text(counters.get('completion_wait_commit_chunk_replay_cpu_ms'), present_encoded)}` |"
    )
    lines.append(
        "| `chunk_publish_slot_residency_ms_per_present` | "
        f"`{ratio_text(counters.get('chunk_publish_slot_residency_ms'), present_encoded)}` |"
    )
    lines.append(
        "| `chunk_publish_slot_residency_present_ms_per_present` | "
        f"`{ratio_text(counters.get('chunk_publish_slot_residency_present_ms'), present_encoded)}` |"
    )
    lines.append(
        "| `chunk_publish_slot_residency_nonpresent_ms_per_present` | "
        f"`{ratio_text(counters.get('chunk_publish_slot_residency_nonpresent_ms'), present_encoded)}` |"
    )
    lines.append(
        "| `chunk_publish_present_pre_present_opportunity_slots_per_present` | "
        f"`{ratio_text(counters.get('chunk_publish_present_pre_present_opportunity_slots'), present_encoded)}` |"
    )
    lines.append(
        "| `chunk_publish_present_pre_present_opportunity_commands_per_present` | "
        f"`{ratio_text(counters.get('chunk_publish_present_pre_present_opportunity_commands'), present_encoded)}` |"
    )
    lines.append(
        "| `chunk_publish_present_pre_present_opportunity_draw_items_per_present` | "
        f"`{ratio_text(counters.get('chunk_publish_present_pre_present_opportunity_draw_items'), present_encoded)}` |"
    )
    lines.append(
        "| `chunk_publish_present_pre_present_opportunity_residency_ms_per_present` | "
        f"`{ratio_text(counters.get('chunk_publish_present_pre_present_opportunity_residency_ms'), present_encoded)}` |"
    )
    first_publish_slot_samples = counters.get(
        "completion_no_enqueue_first_publish_slot_samples"
    )
    lines.append(
        "| `no_enqueue_first_publish_slot_samples_per_present` | "
        f"`{ratio_text(first_publish_slot_samples, present_encoded)}` |"
    )
    lines.append(
        "| `no_enqueue_first_publish_slot_commands_per_slot` | "
        f"`{ratio_text(counters.get('completion_no_enqueue_first_publish_slot_commands'), first_publish_slot_samples)}` |"
    )
    lines.append(
        "| `no_enqueue_first_publish_slot_draw_items_per_slot` | "
        f"`{ratio_text(counters.get('completion_no_enqueue_first_publish_slot_draw_items'), first_publish_slot_samples)}` |"
    )
    lines.append(
        "| `no_enqueue_first_publish_slot_payload_bytes_per_slot` | "
        f"`{ratio_text(counters.get('completion_no_enqueue_first_publish_slot_payload_bytes'), first_publish_slot_samples)}` |"
    )
    lines.append(
        "| `commit_chunk_replay_cpu_ms_per_present` | "
        f"`{ratio_text(counters.get('commit_chunk_replay_cpu_ms'), present_encoded)}` |"
    )
    lines.append(
        "| `commit_chunk_queue_draw_submission_cpu_ms_per_present` | "
        f"`{ratio_text(counters.get('commit_chunk_queue_draw_submission_cpu_ms'), present_encoded)}` |"
    )
    lines.append(
        "| `d3d9_snapshot_draw_submission_cpu_ms_per_present` | "
        f"`{ratio_text(counters.get('d3d9_snapshot_draw_submission_cpu_ms'), present_encoded)}` |"
    )
    lines.append(
        "| `d3d9_snapshot_cache_lookup_cpu_ms_per_present` | "
        f"`{ratio_text(counters.get('d3d9_snapshot_cache_lookup_cpu_ms'), present_encoded)}` |"
    )
    lines.append(
        "| `encode_chunk_cpu_ms_per_present` | "
        f"`{ratio_text(counters.get('encode_chunk_cpu_ms'), present_encoded)}` |"
    )
    lines.append(
        "| `encode_draw_cpu_ms_per_present` | "
        f"`{ratio_text(counters.get('encode_draw_cpu_ms'), present_encoded)}` |"
    )
    lines.append("")

    lines.append("### No-Enqueue Cumulative Timeline")
    lines.append("")
    lines.append(
        "Cumulative wait-end rows are useful for ordering only. Do not subtract "
        "their p50/p95 values across rows; each row can have a different sample "
        "set, and `CommitPublish` may occur before full `commit_chunk` replay "
        "end for the sampled chunk."
    )
    lines.append("")
    lines.append("| Timeline point | total ms/present | p50 ms | p95 ms |")
    lines.append("|---|---:|---:|---:|")
    for label, total_key, p50_key, p95_key in timeline_rows:
        lines.append(
            f"| {label} | `{ratio_text(counters.get(total_key), present_encoded)}` | "
            f"`{fmt(counters.get(p50_key))}` | `{fmt(counters.get(p95_key))}` |"
        )
    lines.append("")

    lines.append("### No-Enqueue Commit Chunks Before Publish")
    lines.append("")
    lines.append(
        "Counts are sampled when the first `CommitPublish` after a no-enqueue "
        "completion wait is observed. Values above `1` mean one or more early "
        "`commit_chunk` entries or replays did not publish the next command "
        "buffer for that wait gap."
    )
    lines.append("")
    lines.append("| Event | total | per publish sample | max | p50 | p95 |")
    lines.append("|---|---:|---:|---:|---:|---:|")
    publish_samples = counters.get("completion_no_enqueue_wait_to_commit_publish")
    chunk_count_rows = (
        (
            "entries",
            "completion_no_enqueue_commit_chunk_entries_before_publish",
            "completion_no_enqueue_commit_chunk_entries_before_publish_max",
            "completion_no_enqueue_commit_chunk_entries_before_publish_p50",
            "completion_no_enqueue_commit_chunk_entries_before_publish_p95",
        ),
        (
            "replay starts",
            "completion_no_enqueue_commit_chunk_replay_starts_before_publish",
            "completion_no_enqueue_commit_chunk_replay_starts_before_publish_max",
            "completion_no_enqueue_commit_chunk_replay_starts_before_publish_p50",
            "completion_no_enqueue_commit_chunk_replay_starts_before_publish_p95",
        ),
        (
            "replay ends",
            "completion_no_enqueue_commit_chunk_replay_ends_before_publish",
            "completion_no_enqueue_commit_chunk_replay_ends_before_publish_max",
            "completion_no_enqueue_commit_chunk_replay_ends_before_publish_p50",
            "completion_no_enqueue_commit_chunk_replay_ends_before_publish_p95",
        ),
    )
    for label, total_key, max_key, p50_key, p95_key in chunk_count_rows:
        lines.append(
            f"| {label} | `{fmt(counters.get(total_key))}` | "
            f"`{ratio_text(counters.get(total_key), publish_samples)}` | "
            f"`{fmt(counters.get(max_key))}` | `{fmt(counters.get(p50_key))}` | "
            f"`{fmt(counters.get(p95_key))}` |"
        )
    lines.append("")

    completed_replay_cpu_ms = counters.get(
        "completion_no_enqueue_commit_chunk_completed_replay_cpu_before_publish_ms"
    )
    active_replay_cpu_ms = counters.get(
        "completion_no_enqueue_commit_chunk_active_replay_cpu_before_publish_ms"
    )
    inter_replay_gap_ms = counters.get(
        "completion_no_enqueue_commit_chunk_inter_replay_gap_before_publish_ms"
    )
    publish_wait_ms = counters.get(
        "completion_no_enqueue_commit_publish_wait_before_publish_ms"
    )
    on_before_publish_cpu_ms = counters.get(
        "completion_no_enqueue_commit_publish_on_before_publish_cpu_ms"
    )
    commit_entry_to_publish_ms = counters.get(
        "completion_no_enqueue_stage_commit_entry_to_publish_ms"
    )
    completed_replay_residual_ms = None
    replay_residual_ms = None
    completed_replay_share = None
    active_replay_share = None
    inter_replay_gap_share = None
    publish_wait_share = None
    replay_attributed_share = None
    replay_gap_attributed_share = None
    replay_wait_attributed_share = None
    replay_attributed_ms = None
    replay_gap_attributed_ms = None
    replay_wait_attributed_ms = None
    if (
        isinstance(completed_replay_cpu_ms, (int, float))
        and isinstance(commit_entry_to_publish_ms, (int, float))
    ):
        completed_replay_residual_ms = (
            commit_entry_to_publish_ms - completed_replay_cpu_ms
        )
        if commit_entry_to_publish_ms > 0:
            completed_replay_share = (
                completed_replay_cpu_ms / commit_entry_to_publish_ms * 100.0
            )
    if (
        isinstance(completed_replay_cpu_ms, (int, float))
        and isinstance(active_replay_cpu_ms, (int, float))
    ):
        replay_attributed_ms = completed_replay_cpu_ms + active_replay_cpu_ms
    if (
        isinstance(replay_attributed_ms, (int, float))
        and isinstance(inter_replay_gap_ms, (int, float))
    ):
        replay_gap_attributed_ms = replay_attributed_ms + inter_replay_gap_ms
    if (
        isinstance(replay_gap_attributed_ms, (int, float))
        and isinstance(publish_wait_ms, (int, float))
    ):
        replay_wait_attributed_ms = replay_gap_attributed_ms + publish_wait_ms
    if (
        isinstance(replay_attributed_ms, (int, float))
        and isinstance(commit_entry_to_publish_ms, (int, float))
    ):
        replay_residual_ms = commit_entry_to_publish_ms - replay_attributed_ms
        if commit_entry_to_publish_ms > 0:
            replay_attributed_share = (
                replay_attributed_ms / commit_entry_to_publish_ms * 100.0
            )
    replay_wait_residual_ms = None
    replay_gap_residual_ms = None
    if (
        isinstance(replay_gap_attributed_ms, (int, float))
        and isinstance(commit_entry_to_publish_ms, (int, float))
    ):
        replay_gap_residual_ms = commit_entry_to_publish_ms - replay_gap_attributed_ms
        if commit_entry_to_publish_ms > 0:
            replay_gap_attributed_share = (
                replay_gap_attributed_ms / commit_entry_to_publish_ms * 100.0
            )
    if (
        isinstance(replay_wait_attributed_ms, (int, float))
        and isinstance(commit_entry_to_publish_ms, (int, float))
    ):
        replay_wait_residual_ms = commit_entry_to_publish_ms - replay_wait_attributed_ms
        if commit_entry_to_publish_ms > 0:
            replay_wait_attributed_share = (
                replay_wait_attributed_ms / commit_entry_to_publish_ms * 100.0
            )
    if (
        isinstance(active_replay_cpu_ms, (int, float))
        and isinstance(commit_entry_to_publish_ms, (int, float))
        and commit_entry_to_publish_ms > 0
    ):
        active_replay_share = active_replay_cpu_ms / commit_entry_to_publish_ms * 100.0
    if (
        isinstance(inter_replay_gap_ms, (int, float))
        and isinstance(commit_entry_to_publish_ms, (int, float))
        and commit_entry_to_publish_ms > 0
    ):
        inter_replay_gap_share = inter_replay_gap_ms / commit_entry_to_publish_ms * 100.0
    if (
        isinstance(publish_wait_ms, (int, float))
        and isinstance(commit_entry_to_publish_ms, (int, float))
        and commit_entry_to_publish_ms > 0
    ):
        publish_wait_share = publish_wait_ms / commit_entry_to_publish_ms * 100.0
    completed_replay_share_text = (
        f"{fmt(completed_replay_share)}%"
        if completed_replay_share is not None
        else "n/a"
    )
    active_replay_share_text = (
        f"{fmt(active_replay_share)}%"
        if active_replay_share is not None
        else "n/a"
    )
    replay_attributed_share_text = (
        f"{fmt(replay_attributed_share)}%"
        if replay_attributed_share is not None
        else "n/a"
    )
    inter_replay_gap_share_text = (
        f"{fmt(inter_replay_gap_share)}%"
        if inter_replay_gap_share is not None
        else "n/a"
    )
    replay_gap_attributed_share_text = (
        f"{fmt(replay_gap_attributed_share)}%"
        if replay_gap_attributed_share is not None
        else "n/a"
    )
    publish_wait_share_text = (
        f"{fmt(publish_wait_share)}%"
        if publish_wait_share is not None
        else "n/a"
    )
    replay_wait_attributed_share_text = (
        f"{fmt(replay_wait_attributed_share)}%"
        if replay_wait_attributed_share is not None
        else "n/a"
    )
    lines.append("### No-Enqueue Commit Entry Publish Attribution")
    lines.append("")
    lines.append(
        "Completed replay CPU is the accumulated CPU time of chunks whose replay "
        "fully ended before the first `CommitPublish` after a no-enqueue wait. "
        "Active replay CPU is the current present-bearing chunk's replay work "
        "observed immediately before the first publish. Inter-replay gap is the "
        "wall time between a completed chunk replay and the next `commit_chunk` "
        "entry before that first publish. Publish wait is the queue `writeCv` wait "
        "immediately before that first publish. The residual keeps non-wait "
        "publish formation and unattributed handoff time as the next target. "
        "The `onBeforePublish` callback is sampled separately because it runs "
        "after this publish timestamp and belongs to the publish -> encode-dequeue "
        "window."
    )
    lines.append("")
    lines.append("| Metric | total ms/present | p50 ms | p95 ms |")
    lines.append("|---|---:|---:|---:|")
    lines.append(
        "| commit entry -> publish | "
        f"`{ratio_text(commit_entry_to_publish_ms, present_encoded)}` | "
        f"`{fmt(counters.get('completion_no_enqueue_stage_commit_entry_to_publish_p50_ms'))}` | "
        f"`{fmt(counters.get('completion_no_enqueue_stage_commit_entry_to_publish_p95_ms'))}` |"
    )
    lines.append(
        "| completed replay CPU before publish | "
        f"`{ratio_text(completed_replay_cpu_ms, present_encoded)}` | "
        f"`{fmt(counters.get('completion_no_enqueue_commit_chunk_completed_replay_cpu_before_publish_p50_ms'))}` | "
        f"`{fmt(counters.get('completion_no_enqueue_commit_chunk_completed_replay_cpu_before_publish_p95_ms'))}` |"
    )
    lines.append(
        "| active replay CPU before publish | "
        f"`{ratio_text(active_replay_cpu_ms, present_encoded)}` | "
        f"`{fmt(counters.get('completion_no_enqueue_commit_chunk_active_replay_cpu_before_publish_p50_ms'))}` | "
        f"`{fmt(counters.get('completion_no_enqueue_commit_chunk_active_replay_cpu_before_publish_p95_ms'))}` |"
    )
    lines.append(
        "| inter-replay producer gap before publish | "
        f"`{ratio_text(inter_replay_gap_ms, present_encoded)}` | "
        f"`{fmt(counters.get('completion_no_enqueue_commit_chunk_inter_replay_gap_before_publish_p50_ms'))}` | "
        f"`{fmt(counters.get('completion_no_enqueue_commit_chunk_inter_replay_gap_before_publish_p95_ms'))}` |"
    )
    lines.append(
        "| commit publish wait before publish | "
        f"`{ratio_text(publish_wait_ms, present_encoded)}` | "
        f"`{fmt(counters.get('completion_no_enqueue_commit_publish_wait_before_publish_p50_ms'))}` | "
        f"`{fmt(counters.get('completion_no_enqueue_commit_publish_wait_before_publish_p95_ms'))}` |"
    )
    lines.append(
        "| post-publish onBeforePublish CPU | "
        f"`{ratio_text(on_before_publish_cpu_ms, present_encoded)}` | "
        f"`{fmt(counters.get('completion_no_enqueue_commit_publish_on_before_publish_cpu_p50_ms'))}` | "
        f"`{fmt(counters.get('completion_no_enqueue_commit_publish_on_before_publish_cpu_p95_ms'))}` |"
    )
    lines.append(
        "| residual after completed replay only | "
        f"`{ratio_text(completed_replay_residual_ms, present_encoded)}` | "
        "`n/a` | `n/a` |"
    )
    lines.append(
        "| residual after completed + active replay | "
        f"`{ratio_text(replay_residual_ms, present_encoded)}` | "
        "`n/a` | `n/a` |"
    )
    lines.append(
        "| residual after completed + active replay + inter-replay gap | "
        f"`{ratio_text(replay_gap_residual_ms, present_encoded)}` | "
        "`n/a` | `n/a` |"
    )
    lines.append(
        "| residual after completed + active replay + inter-replay gap + publish wait | "
        f"`{ratio_text(replay_wait_residual_ms, present_encoded)}` | "
        "`n/a` | `n/a` |"
    )
    lines.append(
        "| completed replay share of commit entry -> publish | "
        f"`{completed_replay_share_text}` | `n/a` | `n/a` |"
    )
    lines.append(
        "| active replay share of commit entry -> publish | "
        f"`{active_replay_share_text}` | `n/a` | `n/a` |"
    )
    lines.append(
        "| inter-replay producer gap share of commit entry -> publish | "
        f"`{inter_replay_gap_share_text}` | `n/a` | `n/a` |"
    )
    lines.append(
        "| commit publish wait share of commit entry -> publish | "
        f"`{publish_wait_share_text}` | `n/a` | `n/a` |"
    )
    lines.append(
        "| completed + active replay share of commit entry -> publish | "
        f"`{replay_attributed_share_text}` | `n/a` | `n/a` |"
    )
    lines.append(
        "| completed + active replay + inter-replay gap share of commit entry -> publish | "
        f"`{replay_gap_attributed_share_text}` | `n/a` | `n/a` |"
    )
    lines.append(
        "| completed + active replay + inter-replay gap + publish wait share of commit entry -> publish | "
        f"`{replay_wait_attributed_share_text}` | `n/a` | `n/a` |"
    )
    lines.append("")

    lines.append("### No-Enqueue Before-Publish Chunk Shape")
    lines.append("")
    lines.append(
        "These rows classify the validated `commit_chunk` records replayed before "
        "the first publish after a no-enqueue completion wait. This separates "
        "producer activity that is building draw work from state/constant-only "
        "or present-bearing chunks."
    )
    lines.append("")
    shape_samples = counters.get(
        "completion_no_enqueue_commit_chunk_shape_samples_before_publish"
    )
    lines.append("| Chunk metric | total | per publish sample | per scanned chunk |")
    lines.append("|---|---:|---:|---:|")
    chunk_shape_rows = (
        (
            "scanned chunks",
            "completion_no_enqueue_commit_chunk_shape_samples_before_publish",
        ),
        (
            "chunks with draw",
            "completion_no_enqueue_commit_chunk_chunks_with_draw_before_publish",
        ),
        (
            "chunks with present",
            "completion_no_enqueue_commit_chunk_chunks_with_present_before_publish",
        ),
        (
            "state/const-only chunks",
            "completion_no_enqueue_commit_chunk_chunks_state_const_only_before_publish",
        ),
        (
            "no-draw/no-present chunks",
            "completion_no_enqueue_commit_chunk_chunks_no_draw_no_present_before_publish",
        ),
    )
    for label, total_key in chunk_shape_rows:
        value = counters.get(total_key)
        lines.append(
            f"| {label} | `{fmt(value)}` | `{ratio_text(value, publish_samples)}` | "
            f"`{ratio_text(value, shape_samples)}` |"
        )
    lines.append("")
    lines.append("| Record metric | total | per publish sample | per scanned chunk |")
    lines.append("|---|---:|---:|---:|")
    record_shape_rows = (
        (
            "all records",
            "completion_no_enqueue_commit_chunk_records_before_publish",
        ),
        (
            "draw records",
            "completion_no_enqueue_commit_chunk_draw_records_before_publish",
        ),
        (
            "const records",
            "completion_no_enqueue_commit_chunk_const_records_before_publish",
        ),
        (
            "apply-state records",
            "completion_no_enqueue_commit_chunk_apply_state_records_before_publish",
        ),
        (
            "clear records",
            "completion_no_enqueue_commit_chunk_clear_records_before_publish",
        ),
        (
            "present records",
            "completion_no_enqueue_commit_chunk_present_records_before_publish",
        ),
        (
            "surface/copy records",
            "completion_no_enqueue_commit_chunk_surface_records_before_publish",
        ),
        (
            "query records",
            "completion_no_enqueue_commit_chunk_query_records_before_publish",
        ),
        (
            "other records",
            "completion_no_enqueue_commit_chunk_other_records_before_publish",
        ),
    )
    for label, total_key in record_shape_rows:
        value = counters.get(total_key)
        lines.append(
            f"| {label} | `{fmt(value)}` | `{ratio_text(value, publish_samples)}` | "
            f"`{ratio_text(value, shape_samples)}` |"
        )
    lines.append("")
    lines.append(
        "| Records per scanned chunk | max | p50 | p95 |"
    )
    lines.append("|---|---:|---:|---:|")
    lines.append(
        "| record count | "
        f"`{fmt(counters.get('completion_no_enqueue_commit_chunk_records_before_publish_max'))}` | "
        f"`{fmt(counters.get('completion_no_enqueue_commit_chunk_records_before_publish_p50'))}` | "
        f"`{fmt(counters.get('completion_no_enqueue_commit_chunk_records_before_publish_p95'))}` |"
    )
    lines.append("")

    first_publish_slot_samples = counters.get(
        "completion_no_enqueue_first_publish_slot_samples"
    )
    if first_publish_slot_samples:
        lines.append("### No-Enqueue First-Publish Slot Shape")
        lines.append("")
        lines.append(
            "These rows size the queue slot published immediately after a "
            "no-enqueue completion wait. They are the direct numerator for "
            "pre-encode/open-command-buffer overlap candidates."
        )
        lines.append("")
        lines.append("| Slot metric | total | per present | per sampled slot |")
        lines.append("|---|---:|---:|---:|")
        first_publish_rows = (
            (
                "samples",
                "completion_no_enqueue_first_publish_slot_samples",
            ),
            (
                "commands",
                "completion_no_enqueue_first_publish_slot_commands",
            ),
            (
                "draw-run commands",
                "completion_no_enqueue_first_publish_slot_draw_run_commands",
            ),
            (
                "draw items",
                "completion_no_enqueue_first_publish_slot_draw_items",
            ),
            (
                "non-draw commands",
                "completion_no_enqueue_first_publish_slot_non_draw_commands",
            ),
            (
                "payload bytes",
                "completion_no_enqueue_first_publish_slot_payload_bytes",
            ),
            (
                "present commands",
                "completion_no_enqueue_first_publish_slot_present_commands",
            ),
        )
        for label, total_key in first_publish_rows:
            value = counters.get(total_key)
            lines.append(
                f"| {label} | `{fmt(value)}` | `{ratio_text(value, present_encoded)}` | "
                f"`{ratio_text(value, first_publish_slot_samples)}` |"
            )
        lines.append("")
        lines.append("| Slot percentile | max | p50 | p95 |")
        lines.append("|---|---:|---:|---:|")
        lines.append(
            "| commands | "
            f"`{fmt(counters.get('completion_no_enqueue_first_publish_slot_commands_max'))}` | "
            f"`{fmt(counters.get('completion_no_enqueue_first_publish_slot_commands_p50'))}` | "
            f"`{fmt(counters.get('completion_no_enqueue_first_publish_slot_commands_p95'))}` |"
        )
        lines.append(
            "| draw items | "
            f"`{fmt(counters.get('completion_no_enqueue_first_publish_slot_draw_items_max'))}` | "
            f"`{fmt(counters.get('completion_no_enqueue_first_publish_slot_draw_items_p50'))}` | "
            f"`{fmt(counters.get('completion_no_enqueue_first_publish_slot_draw_items_p95'))}` |"
        )
        lines.append(
            "| payload bytes | "
            f"`{fmt(counters.get('completion_no_enqueue_first_publish_slot_payload_bytes_max'))}` | "
            f"`{fmt(counters.get('completion_no_enqueue_first_publish_slot_payload_bytes_p50'))}` | "
            f"`{fmt(counters.get('completion_no_enqueue_first_publish_slot_payload_bytes_p95'))}` |"
        )
        lines.append("")

    lines.append("### Exposed No-Enqueue Stage Shape")
    lines.append("")
    lines.append("| Stage | total ms/present | p50 ms | p95 ms |")
    lines.append("|---|---:|---:|---:|")
    for label, total_key, p50_key, p95_key in stage_rows:
        lines.append(
            f"| {label} | `{ratio_text(counters.get(total_key), present_encoded)}` | "
            f"`{fmt(counters.get(p50_key))}` | `{fmt(counters.get(p95_key))}` |"
        )
    lines.append("")

    lines.append("### Pacing Verdict")
    lines.append("")
    lines.append(f"- Verdict: `{verdict}`.")
    lines.append(f"- Largest p50 no-enqueue row: `{largest_row}`.")
    lines.append("")


def append_pe_recorder_gap_call_derived(
    lines: list[str],
    pe_counters: dict[str, Any],
    present_encoded: Any,
) -> None:
    if not isinstance(pe_counters, dict) or not pe_counters:
        return

    focus_rows = (
        (
            "draw_indexed -> set_vs_const_f",
            "gapDrawIndexedVsConstF",
            pe_counters.get("interAppendTop1Ms"),
        ),
        (
            "draw_indexed -> apply_state",
            "gapDrawIndexedApplyState",
            pe_counters.get("interAppendTop2Ms"),
        ),
        (
            "draw_indexed -> draw_indexed",
            "gapDrawIndexedDrawIndexed",
            pe_counters.get("interAppendTop3Ms"),
        ),
        (
            "draw_indexed -> set_ps_const_f",
            "gapDrawIndexedPsConstF",
            pe_counters.get("interAppendTop4Ms"),
        ),
    )
    if not any(
        pe_counters.get(f"{prefix}Top1Samples")
        or pe_counters.get(f"{prefix}Top1Ms")
        for _, prefix, _ in focus_rows
    ):
        return

    lines.append("## PE Recorder Inter-Append Call-Family Attribution")
    lines.append("")
    lines.append(
        "These rows attribute each focused inter-append pair gap to the PE "
        "D3D9 call family active when the next appendable record was emitted. "
        "Use this after setter-body timers reject a direct CPU owner."
    )
    lines.append("")
    lines.append(
        "| Pair | pair ms/present | rank | call family | samples | ms | "
        "ms/present | max ms |"
    )
    lines.append("|---|---:|---:|---|---:|---:|---:|---:|")
    for pair_label, prefix, pair_ms in focus_rows:
        for rank in (1, 2):
            samples = pe_counters.get(f"{prefix}Top{rank}Samples")
            total_ms = pe_counters.get(f"{prefix}Top{rank}Ms")
            family = pe_counters.get(f"{prefix}Top{rank}CallFamily")
            max_ms = pe_counters.get(f"{prefix}Top{rank}MaxMs")
            if not samples and not total_ms:
                continue
            lines.append(
                f"| `{pair_label}` | `{ratio_text(pair_ms, present_encoded)}` | "
                f"`{rank}` | `{family if family is not None else 'unknown'}` | "
                f"`{fmt(samples)}` | `{fmt(total_ms)}` | "
                f"`{ratio_text(total_ms, present_encoded)}` | `{fmt(max_ms)}` |"
            )
    lines.append("")

    if any(
        pe_counters.get(f"{prefix}PhaseSamples")
        or pe_counters.get(f"{prefix}PreCallMs")
        or pe_counters.get(f"{prefix}InsideCallMs")
        for _, prefix, _ in focus_rows
    ):
        lines.append("### Focused Inter-Append Phase Split")
        lines.append("")
        lines.append(
            "This split divides each focused gap at the next PE D3D9 call "
            "entry. `pre-call` is time after the prior append returned and "
            "before the next D3D9 call entered; `inside-call` is time from "
            "that D3D9 call entry to the next append."
        )
        lines.append("")
        lines.append(
            "| Pair | split samples | pre-call ms | pre-call ms/present | "
            "inside-call ms | inside-call ms/present | pre-call share | "
            "inside-call share | pre max ms | inside max ms |"
        )
        lines.append("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
        for pair_label, prefix, _ in focus_rows:
            samples = pe_counters.get(f"{prefix}PhaseSamples")
            pre_ms = pe_counters.get(f"{prefix}PreCallMs")
            inside_ms = pe_counters.get(f"{prefix}InsideCallMs")
            if not samples and not pre_ms and not inside_ms:
                continue
            phase_total = None
            if isinstance(pre_ms, (int, float)) and isinstance(inside_ms, (int, float)):
                phase_total = pre_ms + inside_ms
            lines.append(
                f"| `{pair_label}` | `{fmt(samples)}` | "
                f"`{fmt(pre_ms)}` | `{ratio_text(pre_ms, present_encoded)}` | "
                f"`{fmt(inside_ms)}` | `{ratio_text(inside_ms, present_encoded)}` | "
                f"`{pct(pre_ms, phase_total)}` | `{pct(inside_ms, phase_total)}` | "
                f"`{fmt(pe_counters.get(f'{prefix}PreCallMaxMs'))}` | "
                f"`{fmt(pe_counters.get(f'{prefix}InsideCallMaxMs'))}` |"
            )
        lines.append("")

    if any(
        pe_counters.get(f"{prefix}TailSplitSamples")
        or pe_counters.get(f"{prefix}PrevCallTailMs")
        or pe_counters.get(f"{prefix}BetweenCallsMs")
        for _, prefix, _ in focus_rows
    ):
        lines.append("### Focused Pre-Call Tail Split")
        lines.append("")
        lines.append(
            "This split divides the `pre-call` phase again when the previous "
            "append happened inside a tracked `DrawIndexedPrimitive` call. "
            "`prev-call-tail` is time after the previous append returned but "
            "before that draw call returned; `between-calls` is time from the "
            "draw return to the next PE D3D9 call entry."
        )
        lines.append("")
        lines.append(
            "| Pair | split samples | prev-call-tail ms | "
            "prev-call-tail ms/present | between-calls ms | "
            "between-calls ms/present | prev-call-tail share | "
            "between-calls share | tail max ms | between max ms |"
        )
        lines.append("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
        for pair_label, prefix, _ in focus_rows:
            samples = pe_counters.get(f"{prefix}TailSplitSamples")
            tail_ms = pe_counters.get(f"{prefix}PrevCallTailMs")
            between_ms = pe_counters.get(f"{prefix}BetweenCallsMs")
            if not samples and not tail_ms and not between_ms:
                continue
            split_total = None
            if isinstance(tail_ms, (int, float)) and isinstance(between_ms, (int, float)):
                split_total = tail_ms + between_ms
            lines.append(
                f"| `{pair_label}` | `{fmt(samples)}` | "
                f"`{fmt(tail_ms)}` | `{ratio_text(tail_ms, present_encoded)}` | "
                f"`{fmt(between_ms)}` | `{ratio_text(between_ms, present_encoded)}` | "
                f"`{pct(tail_ms, split_total)}` | `{pct(between_ms, split_total)}` | "
                f"`{fmt(pe_counters.get(f'{prefix}PrevCallTailMaxMs'))}` | "
                f"`{fmt(pe_counters.get(f'{prefix}BetweenCallsMaxMs'))}` |"
            )
        lines.append("")

    if any(
        pe_counters.get(f"{prefix}BetweenTop1Samples")
        or pe_counters.get(f"{prefix}BetweenTop2Samples")
        for _, prefix, _ in focus_rows
    ):
        lines.append("### Focused Between-Calls Entry Families")
        lines.append("")
        lines.append(
            "This table counts PE D3D9 call entries observed after the "
            "previous `DrawIndexedPrimitive` returned and before the terminal "
            "append-producing D3D9 call. The terminal call itself is removed "
            "from the counts."
        )
        lines.append("")
        lines.append(
            "| Pair | rank | call family | entries | entries/window | "
            "entries/present |"
        )
        lines.append("|---|---:|---|---:|---:|---:|")
        for pair_label, prefix, _ in focus_rows:
            windows = pe_counters.get(f"{prefix}TailSplitSamples")
            for rank in (1, 2):
                entries = pe_counters.get(f"{prefix}BetweenTop{rank}Samples")
                family = pe_counters.get(
                    f"{prefix}BetweenTop{rank}CallFamily")
                if not entries:
                    continue
                lines.append(
                    f"| `{pair_label}` | `{rank}` | "
                    f"`{family if family is not None else 'unknown'}` | "
                    f"`{fmt(entries)}` | `{ratio_text(entries, windows)}` | "
                    f"`{ratio_text(entries, present_encoded)}` |"
                )
        lines.append("")

    if any(
        pe_counters.get(f"{prefix}BetweenTop1CallNameSamples")
        or pe_counters.get(f"{prefix}BetweenTop2CallNameSamples")
        for _, prefix, _ in focus_rows
    ):
        lines.append("### Focused Between-Calls Entry Names")
        lines.append("")
        lines.append(
            "This table uses the same window as the family table, but keeps "
            "a fixed exact-call-name bucket when the call is known. It helps "
            "split `unknown` families and confirms whether constant traffic is "
            "float/int/bool setter traffic."
        )
        lines.append("")
        lines.append(
            "| Pair | rank | call name | entries | entries/window | "
            "entries/present |"
        )
        lines.append("|---|---:|---|---:|---:|---:|")
        for pair_label, prefix, _ in focus_rows:
            windows = pe_counters.get(f"{prefix}TailSplitSamples")
            for rank in (1, 2):
                entries = pe_counters.get(
                    f"{prefix}BetweenTop{rank}CallNameSamples")
                call_name = pe_counters.get(
                    f"{prefix}BetweenTop{rank}CallName")
                if not entries:
                    continue
                lines.append(
                    f"| `{pair_label}` | `{rank}` | "
                    f"`{call_name if call_name is not None else 'unknown'}` | "
                    f"`{fmt(entries)}` | `{ratio_text(entries, windows)}` | "
                    f"`{ratio_text(entries, present_encoded)}` |"
                )
        lines.append("")


REPLAY_SNAPSHOT_CPU_STAGE_ROWS: tuple[tuple[str, str], ...] = (
    ("replay", "commit_chunk_replay_cpu_ms"),
    ("replay", "commit_chunk_replay_draw_record_cpu_ms"),
    ("replay", "commit_chunk_replay_non_draw_record_cpu_ms"),
    ("replay", "commit_chunk_replay_pending_flush_cpu_ms"),
    ("replay", "commit_chunk_draw_batch_submit_cpu_ms"),
    ("submission", "commit_chunk_queue_draw_submission_cpu_ms"),
    ("submission", "commit_chunk_queue_draw_submission_snapshot_cpu_ms"),
    ("submission", "commit_chunk_queue_draw_submission_emplace_cpu_ms"),
    ("submission", "commit_chunk_draw_run_submit_cpu_ms"),
    ("submission", "commit_chunk_draw_run_build_cpu_ms"),
    ("submission", "commit_chunk_draw_run_scan_cpu_ms"),
    ("batch-submit", "submit_draw_run_batch_append_cpu_ms"),
    ("batch-submit", "submit_draw_run_batch_append_uniform_cpu_ms"),
    ("batch-submit", "submit_draw_run_batch_append_state_cpu_ms"),
    ("batch-submit", "submit_draw_run_batch_compat_scan_cpu_ms"),
    ("batch-submit", "submit_draw_run_batch_binding_snapshot_cpu_ms"),
    ("snapshot", "d3d9_snapshot_draw_submission_cpu_ms"),
    ("snapshot", "d3d9_snapshot_cache_lookup_cpu_ms"),
    ("snapshot", "d3d9_snapshot_cache_batch_hit_cpu_ms"),
    ("snapshot", "d3d9_snapshot_cache_batch_miss_cpu_ms"),
    ("snapshot-miss", "d3d9_snapshot_cache_batch_miss_uniform_build_cpu_ms"),
    ("snapshot-miss", "d3d9_snapshot_cache_batch_miss_uniform_build_hash_cpu_ms"),
    ("snapshot-miss", "d3d9_snapshot_cache_batch_miss_uniform_build_vs_const_hash_cpu_ms"),
    ("snapshot-miss", "d3d9_snapshot_cache_batch_miss_uniform_build_ps_const_hash_cpu_ms"),
    ("snapshot-miss", "d3d9_snapshot_cache_batch_miss_uniform_build_nonconst_hash_cpu_ms"),
    ("snapshot-miss", "d3d9_snapshot_cache_batch_miss_hot_build_cpu_ms"),
    ("snapshot-miss", "d3d9_snapshot_cache_batch_miss_hot_build_key_cpu_ms"),
    ("snapshot-miss", "d3d9_snapshot_cache_batch_miss_hot_build_render_state_cpu_ms"),
    ("snapshot-miss", "d3d9_snapshot_cache_batch_miss_hot_build_texture_stage_state_cpu_ms"),
    ("snapshot-miss", "d3d9_snapshot_cache_batch_miss_hot_build_sampler_state_cpu_ms"),
    ("snapshot-miss", "d3d9_snapshot_cache_batch_miss_hot_build_tail_copy_cpu_ms"),
    ("snapshot-miss", "d3d9_snapshot_cache_batch_miss_shader_layout_cpu_ms"),
)


def append_replay_snapshot_cpu_derived(
    lines: list[str],
    counters: dict[str, Any],
    present_encoded: Any,
) -> None:
    replay_cpu = counters.get("commit_chunk_replay_cpu_ms")
    queue_submission = counters.get("commit_chunk_queue_draw_submission_cpu_ms")
    queue_snapshot = counters.get("commit_chunk_queue_draw_submission_snapshot_cpu_ms")
    snapshot = counters.get("d3d9_snapshot_draw_submission_cpu_ms")
    cache_lookup = counters.get("d3d9_snapshot_cache_lookup_cpu_ms")
    batch_miss = counters.get("d3d9_snapshot_cache_batch_miss_cpu_ms")
    batch_hit = counters.get("d3d9_snapshot_cache_batch_hit_cpu_ms")
    pending_flush = counters.get("commit_chunk_replay_pending_flush_cpu_ms")
    draw_batch_submit = counters.get("commit_chunk_draw_batch_submit_cpu_ms")

    stage_rows: list[tuple[str, str, int | float]] = []
    for scope, key in REPLAY_SNAPSHOT_CPU_STAGE_ROWS:
        value = counters.get(key)
        if isinstance(value, (int, float)) and value > 0:
            stage_rows.append((scope, key, value))
    stage_rows.sort(key=lambda row: row[2], reverse=True)

    largest_key = stage_rows[0][1] if stage_rows else "n/a"
    largest_value = stage_rows[0][2] if stage_rows else None

    lines.append("## Replay / Snapshot CPU Derived")
    lines.append("")
    lines.append(
        "Use this block when P4 exposes `commit entry -> publish` or when "
        "`commit_chunk_replay_cpu_ms` is the next CPU owner. Rows mix parent "
        "and child counters for ranking; do not sum them."
    )
    lines.append("")
    lines.append("| Metric | Value |")
    lines.append("|---|---:|")
    lines.append(
        "| `commit_chunk_replay_cpu_ms_per_present` | "
        f"`{ratio_text(replay_cpu, present_encoded)}` |"
    )
    lines.append(
        "| `commit_chunk_queue_draw_submission_cpu_ms_per_present` | "
        f"`{ratio_text(queue_submission, present_encoded)}` |"
    )
    lines.append(
        "| `commit_chunk_queue_draw_submission_snapshot_cpu_ms_per_present` | "
        f"`{ratio_text(queue_snapshot, present_encoded)}` |"
    )
    lines.append(
        "| `d3d9_snapshot_draw_submission_cpu_ms_per_present` | "
        f"`{ratio_text(snapshot, present_encoded)}` |"
    )
    lines.append(
        "| `d3d9_snapshot_cache_lookup_cpu_ms_per_present` | "
        f"`{ratio_text(cache_lookup, present_encoded)}` |"
    )
    lines.append(
        "| `d3d9_snapshot_cache_batch_miss_cpu_ms_per_present` | "
        f"`{ratio_text(batch_miss, present_encoded)}` |"
    )
    lines.append(
        "| `queue_submission_snapshot_share` | "
        f"`{pct(queue_snapshot, queue_submission)}` |"
    )
    lines.append(
        "| `snapshot_cache_lookup_share` | "
        f"`{pct(cache_lookup, snapshot)}` |"
    )
    lines.append(
        "| `snapshot_batch_miss_share_of_lookup` | "
        f"`{pct(batch_miss, cache_lookup)}` |"
    )
    lines.append(
        "| `snapshot_batch_hit_share_of_lookup` | "
        f"`{pct(batch_hit, cache_lookup)}` |"
    )
    lines.append(
        "| `pending_flush_share_of_replay` | "
        f"`{pct(pending_flush, replay_cpu)}` |"
    )
    lines.append(
        "| `draw_batch_submit_share_of_replay` | "
        f"`{pct(draw_batch_submit, replay_cpu)}` |"
    )
    lines.append("")

    lines.append("### Replay / Snapshot Candidate Ranking")
    lines.append("")
    lines.append(
        "| Rank | Scope | Counter | total ms | ms/present | "
        "share of replay | share of queue submission | share of snapshot |"
    )
    lines.append("|---:|---|---|---:|---:|---:|---:|---:|")
    for index, (scope, key, value) in enumerate(stage_rows[:16], start=1):
        lines.append(
            f"| {index} | {scope} | `{key}` | `{fmt(value)}` | "
            f"`{ratio_text(value, present_encoded)}` | "
            f"`{pct(value, replay_cpu)}` | `{pct(value, queue_submission)}` | "
            f"`{pct(value, snapshot)}` |"
        )
    lines.append("")

    lines.append("### Replay / Snapshot Verdict")
    lines.append("")
    lines.append(f"- Largest replay/snapshot row: `{largest_key}`.")
    if largest_value is not None:
        lines.append(
            "- Largest replay/snapshot row per present: "
            f"`{ratio_text(largest_value, present_encoded)}ms`."
        )
    lines.append("")


ENCODE_CPU_STAGE_ROWS: tuple[tuple[str, str], ...] = (
    ("slot", "encode_slot_pso_prefetch_cpu_ms"),
    ("pso", "encode_slot_pso_prefetch_draw_key_resolve_cpu_ms"),
    ("pso", "encode_slot_pso_prefetch_draw_resolve_variant_key_cpu_ms"),
    ("pso", "encode_slot_pso_prefetch_draw_lookup_cpu_ms"),
    ("pso", "encode_slot_pso_prefetch_depth_lookup_cpu_ms"),
    ("draw", "encode_draw_argbuf_setup_cpu_ms"),
    ("argbuf", "encode_draw_argbuf_open_cpu_ms"),
    ("argbuf", "encode_draw_argbuf_cbuf_update_cpu_ms"),
    ("argbuf", "encode_draw_argbuf_cbuf_update_vs_cpu_ms"),
    ("argbuf", "encode_draw_argbuf_cbuf_update_ps_cpu_ms"),
    ("binding", "encode_draw_binding_packet_cpu_ms"),
    ("binding", "encode_draw_binding_packet_plan_cpu_ms"),
    ("binding", "encode_draw_binding_packet_cache_cpu_ms"),
    ("stream", "encode_draw_stream_bind_cpu_ms"),
    ("stream", "encode_draw_texture_sampler_bind_cpu_ms"),
    ("stream", "encode_draw_vertex_stream_bind_cpu_ms"),
    ("stream", "encode_draw_index_setup_cpu_ms"),
    ("pipeline", "encode_draw_pipeline_lookup_cpu_ms"),
    ("draw", "encode_draw_issue_cpu_ms"),
    ("draw", "encode_draw_fvf_decode_cpu_ms"),
    ("draw", "encode_draw_raster_state_cpu_ms"),
)


def append_encode_cpu_derived(
    lines: list[str],
    counters: dict[str, Any],
    present_encoded: Any,
) -> None:
    encode_chunk = counters.get("encode_chunk_cpu_ms")
    encode_draw = counters.get("encode_draw_cpu_ms")
    pso_prefetch = counters.get("encode_slot_pso_prefetch_cpu_ms")
    stage_rows: list[tuple[str, str, int | float]] = []
    for scope, key in ENCODE_CPU_STAGE_ROWS:
        value = counters.get(key)
        if isinstance(value, (int, float)) and value > 0:
            stage_rows.append((scope, key, value))
    stage_rows.sort(key=lambda row: row[2], reverse=True)

    largest_key = stage_rows[0][1] if stage_rows else "n/a"
    largest_value = stage_rows[0][2] if stage_rows else None

    lines.append("## Encode CPU Derived")
    lines.append("")
    lines.append(
        "Use this block after the P4 summary names "
        "`encode dequeue -> command buffer commit` as the largest exposed row. "
        "Rows intentionally mix coarse owners and child counters for ranking; "
        "do not sum them."
    )
    lines.append("")
    lines.append("| Metric | Value |")
    lines.append("|---|---:|")
    lines.append(
        "| `encode_chunk_cpu_ms_per_present` | "
        f"`{ratio_text(encode_chunk, present_encoded)}` |"
    )
    lines.append(
        "| `encode_draw_cpu_ms_per_present` | "
        f"`{ratio_text(encode_draw, present_encoded)}` |"
    )
    lines.append(
        "| `encode_slot_pso_prefetch_cpu_ms_per_present` | "
        f"`{ratio_text(pso_prefetch, present_encoded)}` |"
    )
    lines.append(
        "| `encode_draw_share_of_encode_chunk` | "
        f"`{pct(encode_draw, encode_chunk)}` |"
    )
    lines.append("")

    lines.append("### Encode Candidate Ranking")
    lines.append("")
    lines.append(
        "| Rank | Scope | Counter | total ms | ms/present | "
        "share of encode_chunk | share of encode_draw |"
    )
    lines.append("|---:|---|---|---:|---:|---:|---:|")
    for index, (scope, key, value) in enumerate(stage_rows[:12], start=1):
        draw_share = pct(value, encode_draw) if scope != "slot" and scope != "pso" else "n/a"
        lines.append(
            f"| {index} | {scope} | `{key}` | `{fmt(value)}` | "
            f"`{ratio_text(value, present_encoded)}` | "
            f"`{pct(value, encode_chunk)}` | `{draw_share}` |"
        )
    lines.append("")

    lines.append("### Encode Verdict")
    lines.append("")
    lines.append(f"- Largest encode-stage row: `{largest_key}`.")
    if largest_value is not None:
        lines.append(
            "- Largest encode-stage row per present: "
            f"`{ratio_text(largest_value, present_encoded)}ms`."
        )
    lines.append("")


ARGBUF_CPU_STAGE_ROWS: tuple[tuple[str, str], ...] = (
    ("setup", "encode_draw_argbuf_setup_cpu_ms"),
    ("open", "encode_draw_argbuf_open_cpu_ms"),
    ("open", "encode_draw_argbuf_open_call_cpu_ms"),
    ("open", "encode_draw_argbuf_open_reserve_cpu_ms"),
    ("open", "encode_draw_argbuf_open_set_argument_buffer_cpu_ms"),
    ("open", "encode_draw_argbuf_reopen_post_cpu_ms"),
    ("open", "encode_draw_argbuf_reopen_table_probe_cpu_ms"),
    ("open", "encode_draw_argbuf_reopen_table_shadow_store_cpu_ms"),
    ("open", "encode_draw_argbuf_reopen_byte_account_cpu_ms"),
    ("open", "encode_draw_argbuf_reopen_cbuf_cache_probe_cpu_ms"),
    ("open", "encode_draw_argbuf_reopen_cbuf_dirty_scan_cpu_ms"),
    ("open", "encode_draw_argbuf_reopen_cbuf_force_dirty_cpu_ms"),
    ("open", "encode_draw_argbuf_table_bind_cpu_ms"),
    ("cbuf", "encode_draw_argbuf_cbuf_update_cpu_ms"),
    ("cbuf", "encode_draw_argbuf_cbuf_update_vs_cpu_ms"),
    ("cbuf", "encode_draw_argbuf_cbuf_update_ps_cpu_ms"),
    ("cbuf", "encode_draw_argbuf_cbuf_update_ffp_vs_cpu_ms"),
    ("cbuf", "encode_draw_argbuf_cbuf_update_ffp_ps_cpu_ms"),
    ("cbuf", "encode_draw_argbuf_cbuf_build_cpu_ms"),
    ("cbuf", "encode_draw_argbuf_cbuf_upload_cpu_ms"),
    ("cbuf", "encode_draw_argbuf_cbuf_setbuffer_cpu_ms"),
    ("cbuf", "encode_draw_argbuf_cbuf_cache_merge_cpu_ms"),
    ("cbuf", "encode_draw_argbuf_cbuf_cached_repoint_cpu_ms"),
    ("cbuf", "encode_draw_argbuf_cbuf_content_probe_cpu_ms"),
    ("cbuf", "encode_draw_argbuf_cbuf_binding_write_cpu_ms"),
)


def append_argbuf_cpu_derived(
    lines: list[str],
    counters: dict[str, Any],
    present_encoded: Any,
) -> None:
    argbuf_setup = counters.get("encode_draw_argbuf_setup_cpu_ms")
    argbuf_open = counters.get("encode_draw_argbuf_open_cpu_ms")
    cbuf_update = counters.get("encode_draw_argbuf_cbuf_update_cpu_ms")
    stage_rows: list[tuple[str, str, int | float]] = []
    for scope, key in ARGBUF_CPU_STAGE_ROWS:
        value = counters.get(key)
        if isinstance(value, (int, float)) and value > 0:
            stage_rows.append((scope, key, value))
    stage_rows.sort(key=lambda row: row[2], reverse=True)
    largest_key = stage_rows[0][1] if stage_rows else "n/a"
    largest_value = stage_rows[0][2] if stage_rows else None

    lines.append("## Argbuf CPU Derived")
    lines.append("")
    lines.append(
        "Use this block when `Encode CPU Derived` names "
        "`encode_draw_argbuf_setup_cpu_ms` as the top local owner. Rows mix "
        "argbuf parents and children for triage; do not sum them."
    )
    lines.append("")
    lines.append("| Metric | Value |")
    lines.append("|---|---:|")
    lines.append(
        "| `argbuf_setup_cpu_ms_per_present` | "
        f"`{ratio_text(argbuf_setup, present_encoded)}` |"
    )
    lines.append(
        "| `argbuf_open_cpu_ms_per_present` | "
        f"`{ratio_text(argbuf_open, present_encoded)}` |"
    )
    lines.append(
        "| `argbuf_cbuf_update_cpu_ms_per_present` | "
        f"`{ratio_text(cbuf_update, present_encoded)}` |"
    )
    lines.append(
        "| `argbuf_open_share_of_setup` | "
        f"`{pct(argbuf_open, argbuf_setup)}` |"
    )
    lines.append(
        "| `argbuf_cbuf_update_share_of_setup` | "
        f"`{pct(cbuf_update, argbuf_setup)}` |"
    )
    lines.append("")

    lines.append("### Argbuf Candidate Ranking")
    lines.append("")
    lines.append("| Rank | Scope | Counter | total ms | ms/present | share of argbuf_setup |")
    lines.append("|---:|---|---|---:|---:|---:|")
    for index, (scope, key, value) in enumerate(stage_rows[:12], start=1):
        lines.append(
            f"| {index} | {scope} | `{key}` | `{fmt(value)}` | "
            f"`{ratio_text(value, present_encoded)}` | `{pct(value, argbuf_setup)}` |"
        )
    lines.append("")

    lines.append("### Argbuf Mechanism Counters")
    lines.append("")
    lines.append("| Metric | Value |")
    lines.append("|---|---:|")
    lines.append(
        "| `argbuf_table_bind_skip_share` | "
        f"`{pct(counters.get('encode_draw_argbuf_table_bind_skipped'), counters.get('encode_draw_argbuf_table_bind_calls'))}` |"
    )
    lines.append(
        "| `argbuf_cbuf_update_dirty_share` | "
        f"`{pct(counters.get('encode_draw_argbuf_cbuf_update_dirty_calls'), counters.get('encode_draw_argbuf_cbuf_update_calls'))}` |"
    )
    lines.append(
        "| `argbuf_cbuf_update_write_share` | "
        f"`{pct(counters.get('encode_draw_argbuf_cbuf_update_write_calls'), counters.get('encode_draw_argbuf_cbuf_update_calls'))}` |"
    )
    lines.append(
        "| `argbuf_cbuf_no_dirty_hash_mismatch_per_present` | "
        f"`{ratio_text(counters.get('encode_draw_argbuf_cbuf_reopen_no_dirty_hash_mismatch'), present_encoded)}` |"
    )
    lines.append(
        "| `argbuf_cbuf_reopen_partial_candidates_per_present` | "
        f"`{ratio_text(counters.get('encode_draw_argbuf_cbuf_reopen_partial_candidates'), present_encoded)}` |"
    )
    lines.append(
        "| `argbuf_cbuf_cached_repoint_calls_per_present` | "
        f"`{ratio_text(counters.get('encode_draw_argbuf_cbuf_cached_repoint_calls'), present_encoded)}` |"
    )
    lines.append(
        "| `argbuf_cbuf_content_probe_vs_hit_share` | "
        f"`{pct(counters.get('encode_draw_argbuf_cbuf_content_probe_vs_hits'), counters.get('encode_draw_argbuf_cbuf_content_probe_calls'))}` |"
    )
    lines.append(
        "| `argbuf_cbuf_content_probe_ps_hit_share` | "
        f"`{pct(counters.get('encode_draw_argbuf_cbuf_content_probe_ps_hits'), counters.get('encode_draw_argbuf_cbuf_content_probe_calls'))}` |"
    )
    lines.append(
        "| `argbuf_cbuf_content_probe_ffp_ps_hit_share` | "
        f"`{pct(counters.get('encode_draw_argbuf_cbuf_content_probe_ffp_ps_hits'), counters.get('encode_draw_argbuf_cbuf_content_probe_calls'))}` |"
    )
    lines.append("")

    lines.append("### Argbuf Verdict")
    lines.append("")
    lines.append(f"- Largest argbuf-stage row: `{largest_key}`.")
    if largest_value is not None:
        lines.append(
            "- Largest argbuf-stage row per present: "
            f"`{ratio_text(largest_value, present_encoded)}ms`."
        )
    lines.append("")


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
    argbuf_delta_sources: list[dict[str, Any]] | None = None,
    argbuf_delta_source_csv: Path | None = None,
    vs_const_setter_ranges: list[dict[str, Any]] | None = None,
    vs_const_setter_range_csv: Path | None = None,
) -> None:
    counters = result.get("dxmt9_perf_counters", {})
    bridge = result.get("dxmt9_bridge_counters", {})
    pe_counters = result.get("dxmt9_pe_recorder_counters", {})
    probe_draws = probe_draws or []
    render_pass_reentry_rows = render_pass_reentry_rows or []
    frame_rows = frame_rows or []
    argbuf_delta_sources = argbuf_delta_sources or []
    vs_const_setter_ranges = vs_const_setter_ranges or []
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
    lines.append(f"- Argbuf payload delta source rows: `{len(argbuf_delta_sources)}`")
    if argbuf_delta_source_csv is not None:
        lines.append(f"- Argbuf payload delta source CSV: `{argbuf_delta_source_csv}`")
    lines.append(f"- VS const setter range rows: `{len(vs_const_setter_ranges)}`")
    if vs_const_setter_range_csv is not None:
        lines.append(f"- VS const setter range CSV: `{vs_const_setter_range_csv}`")
    lines.append("")

    lines.append("## Run Counters")
    lines.append("")
    lines.append("| Counter | Value |")
    lines.append("|---|---:|")
    for key in RUN_COUNTERS:
        lines.append(f"| `{key}` | `{fmt(counters.get(key))}` |")
    lines.append("")

    publish_reason_rows = (
        ("draw-limit", "chunk_publish_reason_draw_limit",
         "chunk_publish_commands_draw_limit"),
        ("payload-limit", "chunk_publish_reason_payload_limit",
         "chunk_publish_commands_payload_limit"),
        ("present", "chunk_publish_reason_present",
         "chunk_publish_commands_present"),
        ("present-acquire", "chunk_publish_reason_present_acquire",
         "chunk_publish_commands_present_acquire"),
        ("present-split-before", "chunk_publish_reason_present_split_before",
         "chunk_publish_commands_present_split_before"),
        ("flush", "chunk_publish_reason_flush",
         "chunk_publish_commands_flush"),
        ("stretch-split", "chunk_publish_reason_stretch_split",
         "chunk_publish_commands_stretch_split"),
        ("map-wait", "chunk_publish_reason_map_wait",
         "chunk_publish_commands_map_wait"),
        ("unknown", "chunk_publish_reason_unknown",
         "chunk_publish_commands_unknown"),
    )
    total_publish_reasons = sum(
        numeric_value(counters, count_key)
        for _, count_key, _ in publish_reason_rows
    )
    if total_publish_reasons:
        lines.append("## Chunk Publish Reason Derived")
        lines.append("")
        lines.append(
            "These rows classify why a queue `ChunkSlot` was published to the "
            "encode thread. Use them to separate useful overlap from "
            "render-pass-fragmenting early publish."
        )
        lines.append("")
        lines.append("| Reason | publishes | share | commands | commands/publish |")
        lines.append("|---|---:|---:|---:|---:|")
        for label, count_key, command_key in publish_reason_rows:
            publishes = counters.get(count_key)
            commands = counters.get(command_key)
            if not isinstance(publishes, (int, float)):
                publishes = 0
            if not isinstance(commands, (int, float)):
                commands = 0
            if publishes == 0 and commands == 0:
                continue
            lines.append(
                f"| {label} | `{fmt(publishes)}` | "
                f"`{pct(publishes, total_publish_reasons)}` | "
                f"`{fmt(commands)}` | `{ratio_text(commands, publishes)}` |"
            )
        lines.append("")

    present_split_before = numeric_value(
        counters,
        "chunk_publish_reason_present_split_before",
    )
    if present_split_before:
        tail_rows = (
            ("draw-run", "chunk_publish_present_split_before_tail_draw_run"),
            ("clear", "chunk_publish_present_split_before_tail_clear"),
            ("surface-copy", "chunk_publish_present_split_before_tail_surface_copy"),
            ("stretch-rect", "chunk_publish_present_split_before_tail_stretch_rect"),
            ("readback", "chunk_publish_present_split_before_tail_readback"),
            ("color-fill", "chunk_publish_present_split_before_tail_color_fill"),
            ("depth-resolve", "chunk_publish_present_split_before_tail_depth_resolve"),
            ("present", "chunk_publish_present_split_before_tail_present"),
            ("empty", "chunk_publish_present_split_before_tail_empty"),
        )
        lines.append("## PresentSplitBefore Tail Shape")
        lines.append("")
        lines.append(
            "These rows classify the last command in pre-Present split heads. "
            "A draw-run tail usually means an open-CB split is cutting through "
            "an active render pass and will close it as a chunk-final encoder."
        )
        lines.append("")
        lines.append("| Tail command | slots | share |")
        lines.append("|---|---:|---:|")
        for label, key in tail_rows:
            count = numeric_value(counters, key)
            if count == 0:
                continue
            lines.append(
                f"| {label} | `{fmt(count)}` | "
                f"`{pct(count, present_split_before)}` |"
            )
        draw_only = numeric_value(
            counters,
            "chunk_publish_present_split_before_draw_only",
        )
        lines.append("")
        lines.append("| Metric | Value |")
        lines.append("|---|---:|")
        lines.append(
            "| `draw_only_split_before_share` | "
            f"`{pct(draw_only, present_split_before)}` |"
        )
        lines.append("")

    opportunity_slots = numeric_value(
        counters,
        "chunk_publish_present_pre_present_opportunity_slots",
    )
    if opportunity_slots:
        opportunity_tail_slots = numeric_value(
            counters,
            "chunk_publish_present_pre_present_opportunity_tail_slots",
        )
        opportunity_payload_bytes = numeric_value(
            counters,
            "chunk_publish_present_pre_present_opportunity_payload_bytes",
        )
        lines.append("## Present Pre-Present Work Opportunity")
        lines.append("")
        lines.append(
            "These rows size work already resident in a Present-published slot "
            "before the Present command. A future CPU-ready candidate should "
            "move this work into ready-slot visibility without fragmenting it "
            "into many Metal command buffers."
        )
        lines.append("")
        lines.append("| Metric | Value |")
        lines.append("|---|---:|")
        lines.append(
            "| `slots_per_present` | "
            f"`{ratio_text(opportunity_slots, present_encoded)}` |"
        )
        lines.append(
            "| `tail_slot_share` | "
            f"`{pct(opportunity_tail_slots, opportunity_slots)}` |"
        )
        lines.append(
            "| `commands_per_slot` | "
            f"`{ratio_text(counters.get('chunk_publish_present_pre_present_opportunity_commands'), opportunity_slots)}` |"
        )
        lines.append(
            "| `draw_runs_per_slot` | "
            f"`{ratio_text(counters.get('chunk_publish_present_pre_present_opportunity_draw_runs'), opportunity_slots)}` |"
        )
        lines.append(
            "| `draw_items_per_slot` | "
            f"`{ratio_text(counters.get('chunk_publish_present_pre_present_opportunity_draw_items'), opportunity_slots)}` |"
        )
        lines.append(
            "| `non_draw_commands_per_slot` | "
            f"`{ratio_text(counters.get('chunk_publish_present_pre_present_opportunity_non_draw_commands'), opportunity_slots)}` |"
        )
        lines.append(
            "| `payload_mib` | "
            f"`{fmt(opportunity_payload_bytes / (1024.0 * 1024.0))}` |"
        )
        lines.append(
            "| `residency_ms_per_present` | "
            f"`{ratio_text(counters.get('chunk_publish_present_pre_present_opportunity_residency_ms'), present_encoded)}` |"
        )
        opportunity_tail_rows = (
            ("draw-run", "chunk_publish_present_pre_present_opportunity_tail_draw_run"),
            ("clear", "chunk_publish_present_pre_present_opportunity_tail_clear"),
            ("surface-copy", "chunk_publish_present_pre_present_opportunity_tail_surface_copy"),
            ("stretch-rect", "chunk_publish_present_pre_present_opportunity_tail_stretch_rect"),
            ("readback", "chunk_publish_present_pre_present_opportunity_tail_readback"),
            ("color-fill", "chunk_publish_present_pre_present_opportunity_tail_color_fill"),
            ("depth-resolve", "chunk_publish_present_pre_present_opportunity_tail_depth_resolve"),
            ("present", "chunk_publish_present_pre_present_opportunity_tail_present"),
            ("empty", "chunk_publish_present_pre_present_opportunity_tail_empty"),
        )
        if any(numeric_value(counters, key) for _, key in opportunity_tail_rows):
            lines.append("")
            lines.append("| Prefix tail command | slots | share |")
            lines.append("|---|---:|---:|")
            for label, key in opportunity_tail_rows:
                count = numeric_value(counters, key)
                if count == 0:
                    continue
                lines.append(
                    f"| {label} | `{fmt(count)}` | "
                    f"`{pct(count, opportunity_slots)}` |"
                )
            draw_only = numeric_value(
                counters,
                "chunk_publish_present_pre_present_opportunity_draw_only",
            )
            lines.append("")
            lines.append("| Metric | Value |")
            lines.append("|---|---:|")
            lines.append(
                "| `draw_only_pre_present_opportunity_share` | "
                f"`{pct(draw_only, opportunity_slots)}` |"
            )
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

    append_pacing_cpu_stage_derived(lines, counters, present_encoded)
    append_pe_recorder_gap_call_derived(lines, pe_counters, present_encoded)
    append_replay_snapshot_cpu_derived(lines, counters, present_encoded)
    append_encode_cpu_derived(lines, counters, present_encoded)
    append_argbuf_cpu_derived(lines, counters, present_encoded)

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

    uniform_materialized = counters.get("d3d9_snapshot_uniform_materialized")
    uniform_elided = counters.get("d3d9_snapshot_uniform_elided")
    uniform_append_bytes = counters.get("draw_uniform_payload_append_bytes")
    uniform_appends = counters.get("draw_uniform_payload_appends")
    uniform_fixed_append_bytes = counters.get(
        "draw_uniform_fixed_payload_append_bytes"
    )
    uniform_fixed_appends = counters.get("draw_uniform_fixed_payload_appends")
    uniform_vertex_append_bytes = counters.get(
        "draw_uniform_vertex_constants_append_bytes"
    )
    uniform_vertex_appends = counters.get("draw_uniform_vertex_constants_appends")
    uniform_pixel_append_bytes = counters.get(
        "draw_uniform_pixel_constants_append_bytes"
    )
    uniform_pixel_appends = counters.get("draw_uniform_pixel_constants_appends")
    uniform_backend_materialized = counters.get("draw_uniform_payload_materialized")
    uniform_backend_materialized_bytes = counters.get(
        "draw_uniform_payload_materialized_bytes"
    )
    uniform_backend_materialize_fallbacks = counters.get(
        "draw_uniform_payload_materialize_fallbacks"
    )
    uniform_backend_materialize_cpu = counters.get(
        "draw_uniform_payload_materialize_cpu_ms"
    )
    uniform_backend_materialize_sites = [
        ("other", "Other"),
        ("draw_encoder_command", "Draw encoder command"),
        ("draw_encoder_param", "Draw encoder param"),
        ("framegraph_command", "Framegraph command"),
        ("framegraph_param", "Framegraph param"),
        ("queue_observation", "Queue observation"),
    ]
    uniform_semantic_hash_misses = counters.get(
        "draw_uniform_payload_lookup_semantic_hash_misses"
    )
    uniform_semantic_hash_miss_bytes = counters.get(
        "draw_uniform_payload_lookup_semantic_hash_miss_bytes"
    )
    uniform_materialized_bytes = counters.get("d3d9_snapshot_uniform_materialized_bytes")
    uniform_compact_candidate_bytes = counters.get(
        "d3d9_snapshot_uniform_materialized_compact_candidate_bytes"
    )
    uniform_compact_saved_bytes = counters.get(
        "d3d9_snapshot_uniform_materialized_compact_saved_bytes"
    )
    uniform_compact_fixed_bytes = counters.get(
        "d3d9_snapshot_uniform_materialized_compact_fixed_bytes"
    )
    uniform_compact_vertex_bytes = counters.get(
        "d3d9_snapshot_uniform_materialized_compact_vertex_bytes"
    )
    uniform_compact_pixel_bytes = counters.get(
        "d3d9_snapshot_uniform_materialized_compact_pixel_bytes"
    )
    submission_carrier_records = counters.get(
        "d3d9_snapshot_submission_carrier_records"
    )
    submission_carrier_bytes = counters.get(
        "d3d9_snapshot_submission_carrier_bytes"
    )
    submission_carrier_uniform_storage_bytes = counters.get(
        "d3d9_snapshot_submission_carrier_uniform_storage_bytes"
    )
    submission_carrier_unused_uniform_storage_records = counters.get(
        "d3d9_snapshot_submission_carrier_unused_uniform_storage_records"
    )
    submission_carrier_unused_uniform_storage_bytes = counters.get(
        "d3d9_snapshot_submission_carrier_unused_uniform_storage_bytes"
    )
    uniform_adjacent_previous_payload = counters.get(
        "d3d9_snapshot_uniform_adjacent_previous_payload"
    )
    uniform_adjacent_same_fixed_payload_hash = counters.get(
        "d3d9_snapshot_uniform_adjacent_same_fixed_payload_hash"
    )
    uniform_adjacent_same_fixed_and_shader_const_hashes = counters.get(
        "d3d9_snapshot_uniform_adjacent_same_fixed_and_shader_const_hashes"
    )
    uniform_snapshot_total = None
    if isinstance(uniform_materialized, (int, float)) and isinstance(uniform_elided, (int, float)):
        uniform_snapshot_total = uniform_materialized + uniform_elided
    uniform_payload_record_append_bytes = None
    if isinstance(uniform_append_bytes, (int, float)):
        uniform_payload_record_append_bytes = uniform_append_bytes
        for component_bytes in (
            uniform_fixed_append_bytes,
            uniform_vertex_append_bytes,
            uniform_pixel_append_bytes,
        ):
            if isinstance(component_bytes, (int, float)):
                uniform_payload_record_append_bytes -= component_bytes
    uniform_stage_append_bytes = None
    if isinstance(uniform_vertex_append_bytes, (int, float)) or isinstance(
        uniform_pixel_append_bytes, (int, float)
    ):
        uniform_stage_append_bytes = 0
        for component_bytes in (
            uniform_vertex_append_bytes,
            uniform_pixel_append_bytes,
        ):
            if isinstance(component_bytes, (int, float)):
                uniform_stage_append_bytes += component_bytes
    uniform_compact_stage_bytes = None
    if isinstance(uniform_compact_vertex_bytes, (int, float)) or isinstance(
        uniform_compact_pixel_bytes, (int, float)
    ):
        uniform_compact_stage_bytes = 0
        for component_bytes in (
            uniform_compact_vertex_bytes,
            uniform_compact_pixel_bytes,
        ):
            if isinstance(component_bytes, (int, float)):
                uniform_compact_stage_bytes += component_bytes
    submission_carrier_unused_uniform_storage_mib = None
    if isinstance(submission_carrier_unused_uniform_storage_bytes, (int, float)):
        submission_carrier_unused_uniform_storage_mib = (
            submission_carrier_unused_uniform_storage_bytes / (1024 * 1024)
        )
    lines.append("## Uniform Payload Derived")
    lines.append("")
    lines.append(
        "Use this block to separate frontend snapshot copy width from backend "
        "unique uniform payload storage. A high materialized byte rate with a "
        "low append share points at snapshot/hash construction; a high append "
        "byte rate points at backend SoA storage/copy width."
    )
    lines.append("")
    lines.append("| Metric | Value |")
    lines.append("|---|---:|")
    lines.append(
        "| `uniform_materialized_bytes_per_present` | "
        f"`{ratio_text(uniform_materialized_bytes, present_encoded)}` |"
    )
    lines.append(
        "| `uniform_compact_candidate_bytes_per_present` | "
        f"`{ratio_text(uniform_compact_candidate_bytes, present_encoded)}` |"
    )
    lines.append(
        "| `uniform_compact_saved_bytes_per_present` | "
        f"`{ratio_text(uniform_compact_saved_bytes, present_encoded)}` |"
    )
    lines.append(
        "| `submission_carrier_bytes_per_record` | "
        f"`{ratio_text(submission_carrier_bytes, submission_carrier_records)}` |"
    )
    lines.append(
        "| `submission_carrier_uniform_storage_bytes_per_record` | "
        f"`{ratio_text(submission_carrier_uniform_storage_bytes, submission_carrier_records)}` |"
    )
    lines.append(
        "| `submission_carrier_unused_uniform_storage_records_per_present` | "
        f"`{ratio_text(submission_carrier_unused_uniform_storage_records, present_encoded)}` |"
    )
    lines.append(
        "| `submission_carrier_unused_uniform_storage_mib_per_present` | "
        f"`{ratio_text(submission_carrier_unused_uniform_storage_mib, present_encoded)}` |"
    )
    lines.append(
        "| `submission_carrier_unused_uniform_storage_bytes_per_record` | "
        f"`{ratio_text(submission_carrier_unused_uniform_storage_bytes, submission_carrier_records)}` |"
    )
    lines.append(
        "| `submission_carrier_unused_uniform_storage_share` | "
        f"`{pct(submission_carrier_unused_uniform_storage_bytes, submission_carrier_uniform_storage_bytes)}` |"
    )
    lines.append(
        "| `uniform_compact_fixed_bytes_per_present` | "
        f"`{ratio_text(uniform_compact_fixed_bytes, present_encoded)}` |"
    )
    lines.append(
        "| `uniform_compact_vertex_bytes_per_present` | "
        f"`{ratio_text(uniform_compact_vertex_bytes, present_encoded)}` |"
    )
    lines.append(
        "| `uniform_compact_pixel_bytes_per_present` | "
        f"`{ratio_text(uniform_compact_pixel_bytes, present_encoded)}` |"
    )
    lines.append(
        "| `uniform_compact_candidate_share_of_materialized_bytes` | "
        f"`{pct(uniform_compact_candidate_bytes, uniform_materialized_bytes)}` |"
    )
    lines.append(
        "| `uniform_compact_fixed_share_of_candidate_bytes` | "
        f"`{pct(uniform_compact_fixed_bytes, uniform_compact_candidate_bytes)}` |"
    )
    lines.append(
        "| `uniform_compact_vertex_share_of_candidate_bytes` | "
        f"`{pct(uniform_compact_vertex_bytes, uniform_compact_candidate_bytes)}` |"
    )
    lines.append(
        "| `uniform_compact_pixel_share_of_candidate_bytes` | "
        f"`{pct(uniform_compact_pixel_bytes, uniform_compact_candidate_bytes)}` |"
    )
    lines.append(
        "| `uniform_compact_saved_share_of_materialized_bytes` | "
        f"`{pct(uniform_compact_saved_bytes, uniform_materialized_bytes)}` |"
    )
    lines.append(
        "| `uniform_adjacent_same_fixed_payload_hash_share` | "
        f"`{pct(uniform_adjacent_same_fixed_payload_hash, uniform_adjacent_previous_payload)}` |"
    )
    lines.append(
        "| `uniform_adjacent_same_fixed_and_shader_const_hashes_share` | "
        f"`{pct(uniform_adjacent_same_fixed_and_shader_const_hashes, uniform_adjacent_previous_payload)}` |"
    )
    lines.append(
        "| `uniform_append_bytes_per_present` | "
        f"`{ratio_text(uniform_append_bytes, present_encoded)}` |"
    )
    lines.append(
        "| `uniform_fixed_append_bytes_per_present` | "
        f"`{ratio_text(uniform_fixed_append_bytes, present_encoded)}` |"
    )
    lines.append(
        "| `uniform_vertex_constants_append_bytes_per_present` | "
        f"`{ratio_text(uniform_vertex_append_bytes, present_encoded)}` |"
    )
    lines.append(
        "| `uniform_pixel_constants_append_bytes_per_present` | "
        f"`{ratio_text(uniform_pixel_append_bytes, present_encoded)}` |"
    )
    lines.append(
        "| `uniform_stage_constants_append_bytes_per_present` | "
        f"`{ratio_text(uniform_stage_append_bytes, present_encoded)}` |"
    )
    lines.append(
        "| `uniform_vertex_append_amplification_vs_compact_vertex` | "
        f"`{ratio_text(uniform_vertex_append_bytes, uniform_compact_vertex_bytes)}` |"
    )
    lines.append(
        "| `uniform_pixel_append_amplification_vs_compact_pixel` | "
        f"`{ratio_text(uniform_pixel_append_bytes, uniform_compact_pixel_bytes)}` |"
    )
    lines.append(
        "| `uniform_stage_append_amplification_vs_compact_stage` | "
        f"`{ratio_text(uniform_stage_append_bytes, uniform_compact_stage_bytes)}` |"
    )
    lines.append(
        "| `uniform_append_bytes_per_append` | "
        f"`{ratio_text(uniform_append_bytes, uniform_appends)}` |"
    )
    lines.append(
        "| `uniform_payload_record_append_bytes_per_append` | "
        f"`{ratio_text(uniform_payload_record_append_bytes, uniform_appends)}` |"
    )
    lines.append(
        "| `uniform_fixed_append_records_per_payload_append` | "
        f"`{ratio_text(uniform_fixed_appends, uniform_appends)}` |"
    )
    lines.append(
        "| `uniform_vertex_constants_append_records_per_payload_append` | "
        f"`{ratio_text(uniform_vertex_appends, uniform_appends)}` |"
    )
    lines.append(
        "| `uniform_pixel_constants_append_records_per_payload_append` | "
        f"`{ratio_text(uniform_pixel_appends, uniform_appends)}` |"
    )
    lines.append(
        "| `uniform_backend_materialized_bytes_per_present` | "
        f"`{ratio_text(uniform_backend_materialized_bytes, present_encoded)}` |"
    )
    lines.append(
        "| `uniform_backend_materialize_cpu_ms_per_present` | "
        f"`{ratio_text(uniform_backend_materialize_cpu, present_encoded)}` |"
    )
    lines.append(
        "| `uniform_backend_materialized_bytes_per_call` | "
        f"`{ratio_text(uniform_backend_materialized_bytes, uniform_backend_materialized)}` |"
    )
    lines.append(
        "| `uniform_backend_materialize_fallbacks` | "
        f"`{fmt(uniform_backend_materialize_fallbacks)}` |"
    )
    for site_key, label in uniform_backend_materialize_sites:
        site_count = counters.get(f"draw_uniform_payload_materialized_{site_key}")
        site_bytes = counters.get(
            f"draw_uniform_payload_materialized_{site_key}_bytes"
        )
        site_cpu = counters.get(
            f"draw_uniform_payload_materialize_{site_key}_cpu_ms"
        )
        lines.append(
            f"| `uniform_backend_materialize_{site_key}_share_pct` | "
            f"`{pct(site_count, uniform_backend_materialized)}` |"
        )
        lines.append(
            f"| `uniform_backend_materialize_{site_key}_bytes_per_present` | "
            f"`{ratio_text(site_bytes, present_encoded)}` |"
        )
        lines.append(
            f"| `uniform_backend_materialize_{site_key}_cpu_ms_per_present` | "
            f"`{ratio_text(site_cpu, present_encoded)}` |"
        )
    lines.append(
        "| `uniform_append_records_per_materialized_snapshot` | "
        f"`{ratio_text(uniform_appends, uniform_materialized)}` |"
    )
    lines.append(
        "| `uniform_semantic_hash_misses` | "
        f"`{fmt(uniform_semantic_hash_misses)}` |"
    )
    lines.append(
        "| `uniform_semantic_hash_miss_bytes_per_present` | "
        f"`{ratio_text(uniform_semantic_hash_miss_bytes, present_encoded)}` |"
    )
    lines.append(
        "| `uniform_append_bytes_share_of_materialized_bytes` | "
        f"`{pct(uniform_append_bytes, uniform_materialized_bytes)}` |"
    )
    lines.append(
        "| `uniform_fixed_append_bytes_share_of_append_bytes` | "
        f"`{pct(uniform_fixed_append_bytes, uniform_append_bytes)}` |"
    )
    lines.append(
        "| `uniform_vertex_constants_append_bytes_share_of_append_bytes` | "
        f"`{pct(uniform_vertex_append_bytes, uniform_append_bytes)}` |"
    )
    lines.append(
        "| `uniform_pixel_constants_append_bytes_share_of_append_bytes` | "
        f"`{pct(uniform_pixel_append_bytes, uniform_append_bytes)}` |"
    )
    lines.append(
        "| `uniform_snapshot_elision_share` | "
        f"`{pct(uniform_elided, uniform_snapshot_total)}` |"
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

    if argbuf_delta_sources:
        total_changed_regs = sum_key(argbuf_delta_sources, "changed_regs")
        total_full_prefix_regs = sum_key(argbuf_delta_sources, "full_prefix_regs")
        lines.append("## Argbuf Payload Delta Sources")
        lines.append("")
        lines.append("| vs_hash | ps_hash | prefix_regs | rows | changed_regs | changed_share | full_prefix_rows | full_prefix_regs | full_prefix_reg_share | span/changed |")
        lines.append("|---|---|---:|---:|---:|---:|---:|---:|---:|---:|")
        for row in argbuf_delta_sources[:16]:
            changed_regs = row.get("changed_regs")
            span_regs = row.get("span_regs")
            lines.append(
                f"| `{row.get('vs_hash', '')}` | "
                f"`{row.get('ps_hash', '')}` | "
                f"`{fmt(row.get('prefix_regs'))}` | "
                f"`{fmt(row.get('rows'))}` | "
                f"`{fmt(changed_regs)}` | "
                f"`{pct(changed_regs, total_changed_regs)}` | "
                f"`{fmt(row.get('full_prefix_rows'))}` | "
                f"`{fmt(row.get('full_prefix_regs'))}` | "
                f"`{pct(row.get('full_prefix_regs'), total_full_prefix_regs)}` | "
                f"`{ratio_text(span_regs, changed_regs)}` |"
            )
        overflow_rows = sum_key(argbuf_delta_sources, "overflow_rows")
        if overflow_rows:
            lines.append(
                f"| `overflow` | `overflow` | `overflow` | "
                f"`{fmt(overflow_rows)}` | "
                f"`{fmt(sum_key(argbuf_delta_sources, 'overflow_changed_regs'))}` | "
                "`n/a` | `0` | `0` | `n/a` | `n/a` |"
            )
        lines.append("")

    if vs_const_setter_ranges:
        total_changed_regs = (
            sum_key(vs_const_setter_ranges, "changed_regs")
            + sum_key(vs_const_setter_ranges, "overflow_changed_regs")
        )
        lines.append("## VS Const Setter Ranges")
        lines.append("")
        lines.append("| phase | vs_hash | ps_hash | start | count | events | range_regs | changed_regs | changed_share | range/changed | full_range_events | full_changed_events |")
        lines.append("|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
        for row in vs_const_setter_ranges[:20]:
            changed_regs = row.get("changed_regs")
            range_regs = row.get("range_regs")
            lines.append(
                f"| `{row.get('phase', '')}` | "
                f"`{row.get('vs_hash', '')}` | "
                f"`{row.get('ps_hash', '')}` | "
                f"`{fmt(row.get('start'))}` | "
                f"`{fmt(row.get('count'))}` | "
                f"`{fmt(row.get('events'))}` | "
                f"`{fmt(range_regs)}` | "
                f"`{fmt(changed_regs)}` | "
                f"`{pct(changed_regs, total_changed_regs)}` | "
                f"`{ratio_text(range_regs, changed_regs)}` | "
                f"`{fmt(row.get('full_range_events'))}` | "
                f"`{fmt(row.get('full_changed_events'))}` |"
            )
        overflow_rows = [
            row for row in vs_const_setter_ranges
            if numeric_value(row, "overflow_events")
        ]
        for row in sorted(
            overflow_rows,
            key=lambda item: (
                numeric_value(item, "overflow_changed_regs"),
                numeric_value(item, "overflow_range_regs"),
                numeric_value(item, "overflow_events"),
            ),
            reverse=True,
        ):
            changed_regs = row.get("overflow_changed_regs")
            range_regs = row.get("overflow_range_regs")
            lines.append(
                f"| `{row.get('phase', '')}` | `overflow` | `overflow` | "
                f"`overflow` | `overflow` | "
                f"`{fmt(row.get('overflow_events'))}` | "
                f"`{fmt(range_regs)}` | "
                f"`{fmt(changed_regs)}` | "
                f"`{pct(changed_regs, total_changed_regs)}` | "
                f"`{ratio_text(range_regs, changed_regs)}` | "
                f"`{fmt(row.get('full_range_events'))}` | "
                f"`{fmt(row.get('full_changed_events'))}` |"
            )
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
    parser.add_argument(
        "--require-uniform-compact-saved-bytes-present",
        action="store_true",
        help=(
            "fail if the current run has no conservative compact uniform "
            "saved bytes per present"
        ),
    )
    args = parser.parse_args()

    run_dir = args.output_dir
    result = load_result(run_dir)
    if args.require_uniform_compact_saved_bytes_present:
        require_uniform_compact_saved_bytes_present(result)
    output = args.output or (run_dir / "3dmark05-perf-summary.md")
    encoder_csv = output.parent / "3dmark05-perf-encoders.csv"
    stream_csv = output.parent / "3dmark05-perf-encoder-streams.csv"
    probe_draw_csv = output.parent / "3dmark05-perf-indexed-probe-draws.csv"
    render_pass_reentry_csv = output.parent / "3dmark05-perf-render-pass-reentry.csv"
    frame_csv = output.parent / "3dmark05-perf-frames.csv"
    argbuf_delta_source_csv = output.parent / "3dmark05-perf-argbuf-payload-delta-sources.csv"
    vs_const_setter_range_csv = output.parent / "3dmark05-perf-vs-const-setter-ranges.csv"
    log_path = run_dir / "dxmt9.log"
    encoders, streams = parse_encoder_lines(log_path)
    probe_draws = parse_probe_draw_lines(log_path)
    render_pass_reentry_rows = parse_render_pass_reentry_lines(log_path)
    frame_rows = parse_frame_lines(log_path)
    argbuf_delta_sources = aggregate_argbuf_delta_sources(
        parse_argbuf_delta_source_lines(log_path)
    )
    vs_const_setter_ranges = aggregate_vs_const_setter_ranges(
        parse_vs_const_setter_range_lines(log_path)
    )
    if not log_path.exists():
        encoders = load_existing_csv(encoder_csv)
        streams = load_existing_csv(stream_csv)
        probe_draws = load_existing_csv(probe_draw_csv)
        render_pass_reentry_rows = load_existing_csv(render_pass_reentry_csv)
        frame_rows = load_existing_csv(frame_csv)
        argbuf_delta_sources = load_existing_csv(argbuf_delta_source_csv)
        vs_const_setter_ranges = load_existing_csv(vs_const_setter_range_csv)
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
    if log_path.exists() or not argbuf_delta_source_csv.exists():
        write_csv(
            argbuf_delta_source_csv,
            argbuf_delta_sources,
            ARGBUF_DELTA_SOURCE_CSV_KEYS,
        )
    if log_path.exists() or not vs_const_setter_range_csv.exists():
        write_csv(
            vs_const_setter_range_csv,
            vs_const_setter_ranges,
            VS_CONST_SETTER_RANGE_CSV_KEYS,
        )
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
        argbuf_delta_sources,
        argbuf_delta_source_csv,
        vs_const_setter_ranges,
        vs_const_setter_range_csv,
    )
    print(output)
    return 0


if __name__ == "__main__":
    sys.exit(main())
