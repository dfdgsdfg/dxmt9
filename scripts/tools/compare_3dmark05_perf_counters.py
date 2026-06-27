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
import csv
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
    "render_split_tile_midpass",
    "completion_present_wait_ms",
    "completion_wait_with_enqueue_ms",
    "completion_wait_without_enqueue_ms",
    "completion_present_wait_with_enqueue_ms",
    "completion_present_wait_without_enqueue_ms",
    "completion_enqueue_while_waiting",
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
    "command_buffers",
    "sub_command_buffers",
    "render_pass_begin",
    "render_split_rt_change",
    "render_split_clear",
    "render_split_present",
    "render_split_tile_midpass",
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
    "d3d9_snapshot_state_copy_cpu_ms",
    "d3d9_snapshot_state_materialized",
    "d3d9_snapshot_state_materialized_bytes",
    "d3d9_snapshot_state_elided",
    "d3d9_snapshot_state_elided_bytes",
    "d3d9_snapshot_submission_carrier_records",
    "d3d9_snapshot_submission_carrier_bytes",
    "d3d9_snapshot_submission_carrier_state_storage_bytes",
    "d3d9_snapshot_submission_carrier_uniform_storage_bytes",
    "d3d9_snapshot_submission_carrier_compact_uniform_storage_bytes",
    "d3d9_snapshot_submission_carrier_unused_uniform_storage_records",
    "d3d9_snapshot_submission_carrier_unused_uniform_storage_bytes",
    "d3d9_snapshot_uniform_materialized",
    "d3d9_snapshot_uniform_materialized_bytes",
    "d3d9_snapshot_uniform_materialized_compact_candidate_bytes",
    "d3d9_snapshot_uniform_materialized_compact_saved_bytes",
    "d3d9_snapshot_uniform_materialized_compact_fixed_bytes",
    "d3d9_snapshot_uniform_materialized_compact_vertex_bytes",
    "d3d9_snapshot_uniform_materialized_compact_pixel_bytes",
    "d3d9_snapshot_uniform_elided",
    "d3d9_snapshot_uniform_elided_bytes",
    "d3d9_snapshot_uniform_adjacent_previous_payload",
    "d3d9_snapshot_uniform_adjacent_same_fixed_payload_hash",
    "d3d9_snapshot_uniform_adjacent_same_fixed_and_shader_const_hashes",
    "d3d9_snapshot_cache_uniform_build_cpu_ms",
    "d3d9_snapshot_cache_uniform_hash_cpu_ms",
    "d3d9_snapshot_cache_batch_miss_uniform_build_cpu_ms",
    "d3d9_snapshot_cache_batch_miss_uniform_build_hash_cpu_ms",
    "d3d9_snapshot_cache_batch_miss_uniform_build_vs_const_hash_cpu_ms",
    "d3d9_snapshot_cache_batch_miss_uniform_build_ps_const_hash_cpu_ms",
    "d3d9_snapshot_cache_batch_miss_uniform_build_nonconst_hash_cpu_ms",
    "d3d9_snapshot_cache_batch_miss_uniform_payload_reuse_full",
    "d3d9_snapshot_cache_batch_miss_uniform_payload_reuse_nonconst",
    "d3d9_snapshot_cache_batch_miss_uniform_payload_full_build",
    "d3d9_snapshot_cache_batch_miss_uniform_vs_const_hash_memo_probe",
    "d3d9_snapshot_cache_batch_miss_uniform_vs_const_hash_memo_hits",
    "d3d9_snapshot_cache_batch_miss_uniform_vs_const_hash_memo_misses",
    "d3d9_snapshot_cache_batch_miss_uniform_vs_const_hash_memo_stores",
    "d3d9_snapshot_cache_batch_miss_uniform_ps_const_hash_memo_probe",
    "d3d9_snapshot_cache_batch_miss_uniform_ps_const_hash_memo_hits",
    "d3d9_snapshot_cache_batch_miss_uniform_ps_const_hash_memo_misses",
    "d3d9_snapshot_cache_batch_miss_uniform_ps_const_hash_memo_stores",
    "d3d9_snapshot_uniform_copy_cpu_ms",
    "submit_draw_run_batch_append_uniform_cpu_ms",
    "draw_uniform_payload_lookup_cpu_ms",
    "draw_uniform_payload_lookup_semantic_hash_misses",
    "draw_uniform_payload_lookup_semantic_hash_miss_bytes",
    "draw_uniform_payload_appends",
    "draw_uniform_payload_append_bytes",
    "draw_uniform_fixed_payload_appends",
    "draw_uniform_fixed_payload_append_bytes",
    "draw_uniform_vertex_constants_appends",
    "draw_uniform_vertex_constants_append_bytes",
    "draw_uniform_pixel_constants_appends",
    "draw_uniform_pixel_constants_append_bytes",
    "draw_uniform_payload_append_fixed_find_cpu_ms",
    "draw_uniform_payload_append_vertex_find_cpu_ms",
    "draw_uniform_payload_append_pixel_find_cpu_ms",
    "draw_uniform_payload_append_fixed_append_cpu_ms",
    "draw_uniform_payload_append_vertex_append_cpu_ms",
    "draw_uniform_payload_append_pixel_append_cpu_ms",
    "draw_uniform_payload_materialized",
    "draw_uniform_payload_materialized_bytes",
    "draw_uniform_payload_materialize_fallbacks",
    "draw_uniform_payload_materialize_cpu_ms",
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
    "draw_uniform_payload_append_copy_cpu_ms",
    "submit_draw_run_batch_groups",
    "submit_draw_run_batch_records",
    "submit_draw_run_batch_max_records",
    "submit_draw_run_batch_discarded_state_records",
    "submit_draw_run_batch_discarded_state_bytes",
    "submit_draw_run_batch_submission_adjacent_same_generation_lane",
    "submit_draw_run_batch_compat_same_generation_lane",
    "submit_draw_run_batch_compat_same_generation_lane_compatible",
    "submit_draw_run_batch_compat_same_generation_lane_incompatible",
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
    "commit_chunk_replay_cpu_ms",
    "commit_chunk_queue_draw_submission_cpu_ms",
    "d3d9_snapshot_draw_submission_cpu_ms",
    "d3d9_snapshot_cache_lookup_cpu_ms",
    "encode_chunk_cpu_ms",
    "encode_draw_cpu_ms",
    "encode_draw_argbuf_setup_cpu_ms",
    "encode_draw_argbuf_open_cpu_ms",
    "encode_draw_argbuf_cbuf_update_cpu_ms",
    "encode_draw_argbuf_cbuf_update_vs_cpu_ms",
    "submit_draw_cpu_ms",
    "gpu_command_buffer_time_ms",
    "completion_wait_ms",
    "completion_present_wait_ms",
    "completion_wait_with_enqueue",
    "completion_wait_with_enqueue_ms",
    "completion_wait_without_enqueue",
    "completion_wait_without_enqueue_ms",
    "completion_present_wait_with_enqueue",
    "completion_present_wait_with_enqueue_ms",
    "completion_present_wait_without_enqueue",
    "completion_present_wait_without_enqueue_ms",
    "completion_enqueue_while_waiting",
    "completion_enqueue_while_waiting_present",
    "completion_wait_enqueues_during_wait",
    "completion_wait_enqueues_during_wait_max",
    "completion_wait_commit_chunk_entries",
    "completion_wait_commit_chunk_replay_starts",
    "completion_wait_commit_chunk_replay_ends",
    "completion_wait_commit_chunk_replay_cpu_ms",
    "completion_wait_commit_chunk_replay_cpu_max_ms",
    "completion_wait_commit_chunk_replay_cpu_p50_ms",
    "completion_wait_commit_chunk_replay_cpu_p95_ms",
    "chunk_publish_slot_residency_samples",
    "chunk_publish_slot_residency_ms",
    "chunk_publish_slot_residency_p50_ms",
    "chunk_publish_slot_residency_p95_ms",
    "chunk_publish_slot_residency_present_samples",
    "chunk_publish_slot_residency_present_ms",
    "chunk_publish_slot_residency_present_p50_ms",
    "chunk_publish_slot_residency_present_p95_ms",
    "chunk_publish_slot_residency_nonpresent_samples",
    "chunk_publish_slot_residency_nonpresent_ms",
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
    "completion_no_enqueue_first_publish_slot_pre_present_commands",
    "completion_no_enqueue_first_publish_slot_pre_present_commands_p50",
    "completion_no_enqueue_first_publish_slot_pre_present_commands_p95",
    "completion_no_enqueue_first_publish_slot_pre_present_draw_run_commands",
    "completion_no_enqueue_first_publish_slot_pre_present_draw_items",
    "completion_no_enqueue_first_publish_slot_pre_present_draw_items_p50",
    "completion_no_enqueue_first_publish_slot_pre_present_draw_items_p95",
    "completion_no_enqueue_first_publish_slot_pre_present_non_draw_commands",
    "completion_no_enqueue_first_publish_slot_pre_present_payload_bytes",
    "completion_no_enqueue_first_publish_slot_pre_present_payload_bytes_p50",
    "completion_no_enqueue_first_publish_slot_pre_present_payload_bytes_p95",
    "completion_no_enqueue_first_publish_slot_post_present_commands",
    "completion_no_enqueue_first_publish_slot_present_tail_slots",
    "completion_no_enqueue_first_publish_slot_present_nontail_slots",
    "completion_no_enqueue_stage_commit_entry_to_publish_ms",
    "completion_no_enqueue_stage_commit_entry_to_publish_p50_ms",
    "completion_no_enqueue_stage_commit_entry_to_publish_p95_ms",
    "completion_no_enqueue_stage_publish_to_encode_dequeue_ms",
    "completion_no_enqueue_stage_publish_to_encode_dequeue_p50_ms",
    "completion_no_enqueue_stage_publish_to_encode_dequeue_p95_ms",
    "completion_no_enqueue_stage_encode_dequeue_to_command_buffer_commit_ms",
    "completion_no_enqueue_stage_encode_dequeue_to_command_buffer_commit_p50_ms",
    "completion_no_enqueue_stage_encode_dequeue_to_command_buffer_commit_p95_ms",
    "completion_no_enqueue_wait_to_next_enqueue_ms",
    "completion_no_enqueue_wait_to_next_enqueue_p50_ms",
    "completion_no_enqueue_wait_to_next_enqueue_p95_ms",
    "encode_dequeue_ready_depth_samples",
    "encode_dequeue_ready_depth_total",
    "encode_dequeue_ready_depth_max",
    "encode_dequeue_ready_depth_gt1",
    "encode_dequeue_ready_depth_gt2",
    "encode_dequeue_ready_depth_gt4",
    "map_buffer_wait_ms",
    "queue_sequence_wait_ms",
)

PE_RECORDER_COUNTER_MAP = (
    ("commitCount", "pe_recorder_commit_count"),
    ("recordCountTotal", "pe_recorder_record_count_total"),
    ("recordCountMax", "pe_recorder_record_count_max"),
    ("payloadBytesTotal", "pe_recorder_payload_bytes_total"),
    ("payloadBytesMax", "pe_recorder_payload_bytes_max"),
    ("handleCountTotal", "pe_recorder_handle_count_total"),
    ("handleCountMax", "pe_recorder_handle_count_max"),
    ("chunkFillGapSamples", "pe_recorder_chunk_fill_gap_samples"),
    ("chunkFillGapMs", "pe_recorder_chunk_fill_gap_ms"),
    ("chunkFillGapMaxMs", "pe_recorder_chunk_fill_gap_max_ms"),
    ("chunkFirstRecordGapSamples", "pe_recorder_chunk_first_record_gap_samples"),
    ("chunkFirstRecordGapMs", "pe_recorder_chunk_first_record_gap_ms"),
    ("chunkFirstRecordGapMaxMs", "pe_recorder_chunk_first_record_gap_max_ms"),
    ("chunkActiveFillSamples", "pe_recorder_chunk_active_fill_samples"),
    ("chunkActiveFillMs", "pe_recorder_chunk_active_fill_ms"),
    ("chunkActiveFillMaxMs", "pe_recorder_chunk_active_fill_max_ms"),
    ("chunkInterAppendGapSamples", "pe_recorder_chunk_inter_append_gap_samples"),
    ("chunkInterAppendGapMs", "pe_recorder_chunk_inter_append_gap_ms"),
    ("chunkInterAppendGapMaxMs", "pe_recorder_chunk_inter_append_gap_max_ms"),
    ("chunkBridgeSamples", "pe_recorder_chunk_bridge_samples"),
    ("chunkBridgeMs", "pe_recorder_chunk_bridge_ms"),
    ("chunkBridgeMaxMs", "pe_recorder_chunk_bridge_max_ms"),
    ("recordAppendCalls", "pe_recorder_record_append_calls"),
    ("recordAppendCpuMs", "pe_recorder_record_append_cpu_ms"),
    ("recordAppendCpuMaxMs", "pe_recorder_record_append_cpu_max_ms"),
    ("recordAppendNoFlushCalls", "pe_recorder_record_append_no_flush_calls"),
    ("recordAppendNoFlushCpuMs", "pe_recorder_record_append_no_flush_cpu_ms"),
    ("recordAppendNoFlushCpuMaxMs", "pe_recorder_record_append_no_flush_cpu_max_ms"),
    ("vsConstFSetterCalls", "pe_recorder_vs_const_f_setter_calls"),
    ("vsConstFSetterRegs", "pe_recorder_vs_const_f_setter_regs"),
    ("vsConstFSetterCpuMs", "pe_recorder_vs_const_f_setter_cpu_ms"),
    ("vsConstFSetterCpuMaxMs", "pe_recorder_vs_const_f_setter_cpu_max_ms"),
    ("psConstFSetterCalls", "pe_recorder_ps_const_f_setter_calls"),
    ("psConstFSetterRegs", "pe_recorder_ps_const_f_setter_regs"),
    ("psConstFSetterCpuMs", "pe_recorder_ps_const_f_setter_cpu_ms"),
    ("psConstFSetterCpuMaxMs", "pe_recorder_ps_const_f_setter_cpu_max_ms"),
    ("constFlushCalls", "pe_recorder_const_flush_calls"),
    ("constFlushRecords", "pe_recorder_const_flush_records"),
    ("constFlushRegs", "pe_recorder_const_flush_regs"),
    ("constFlushCpuMs", "pe_recorder_const_flush_cpu_ms"),
    ("constFlushCpuMaxMs", "pe_recorder_const_flush_cpu_max_ms"),
    ("vsConstFFlushRecords", "pe_recorder_vs_const_f_flush_records"),
    ("vsConstFFlushRegs", "pe_recorder_vs_const_f_flush_regs"),
    ("vsConstFFlushCpuMs", "pe_recorder_vs_const_f_flush_cpu_ms"),
    ("psConstFFlushRecords", "pe_recorder_ps_const_f_flush_records"),
    ("psConstFFlushRegs", "pe_recorder_ps_const_f_flush_regs"),
    ("psConstFFlushCpuMs", "pe_recorder_ps_const_f_flush_cpu_ms"),
    ("chunkBarrierFlushCalls", "pe_recorder_chunk_barrier_flush_calls"),
    ("chunkBarrierConstCpuMs", "pe_recorder_chunk_barrier_const_cpu_ms"),
    ("chunkBarrierConstCpuMaxMs", "pe_recorder_chunk_barrier_const_cpu_max_ms"),
    ("applyStateBuildCalls", "pe_recorder_apply_state_build_calls"),
    ("applyStateBuildCpuMs", "pe_recorder_apply_state_build_cpu_ms"),
    ("applyStateBuildCpuMaxMs", "pe_recorder_apply_state_build_cpu_max_ms"),
)

PE_RECORDER_HOT_SETTERS = (
    ("Rt", "rt"),
    ("Ds", "ds"),
    ("ViewportScissor", "viewport_scissor"),
    ("Transform", "transform"),
    ("MaterialLightClip", "material_light_clip"),
    ("RenderState", "render_state"),
    ("TssSampler", "tss_sampler"),
    ("Texture", "texture"),
    ("VertexInput", "vertex_input"),
    ("Shader", "shader"),
)

PE_RECORDER_FOCUSED_GAP_PREFIXES = (
    ("gapDrawIndexedVsConstF", "draw_indexed_vs_const_f"),
    ("gapDrawIndexedApplyState", "draw_indexed_apply_state"),
    ("gapDrawIndexedDrawIndexed", "draw_indexed_draw_indexed"),
    ("gapDrawIndexedPsConstF", "draw_indexed_ps_const_f"),
)

PE_RECORDER_NUMERIC_FOCUS_COUNTERS = tuple(
    key for _, key in PE_RECORDER_COUNTER_MAP
) + tuple(
    f"pe_recorder_inter_append_top{rank}_{suffix}"
    for rank in range(1, 5)
    for suffix in ("samples", "ms", "max_ms")
)

PE_RECORDER_PER_PRESENT_COUNTERS = (
    "pe_recorder_commit_count",
    "pe_recorder_record_count_total",
    "pe_recorder_payload_bytes_total",
    "pe_recorder_handle_count_total",
    "pe_recorder_chunk_fill_gap_samples",
    "pe_recorder_chunk_fill_gap_ms",
    "pe_recorder_chunk_first_record_gap_samples",
    "pe_recorder_chunk_first_record_gap_ms",
    "pe_recorder_chunk_active_fill_samples",
    "pe_recorder_chunk_active_fill_ms",
    "pe_recorder_chunk_inter_append_gap_samples",
    "pe_recorder_chunk_inter_append_gap_ms",
    "pe_recorder_chunk_bridge_samples",
    "pe_recorder_chunk_bridge_ms",
    "pe_recorder_record_append_calls",
    "pe_recorder_record_append_cpu_ms",
    "pe_recorder_record_append_no_flush_calls",
    "pe_recorder_record_append_no_flush_cpu_ms",
    "pe_recorder_vs_const_f_setter_calls",
    "pe_recorder_vs_const_f_setter_regs",
    "pe_recorder_vs_const_f_setter_cpu_ms",
    "pe_recorder_ps_const_f_setter_calls",
    "pe_recorder_ps_const_f_setter_regs",
    "pe_recorder_ps_const_f_setter_cpu_ms",
    "pe_recorder_const_flush_calls",
    "pe_recorder_const_flush_records",
    "pe_recorder_const_flush_regs",
    "pe_recorder_const_flush_cpu_ms",
    "pe_recorder_vs_const_f_flush_records",
    "pe_recorder_vs_const_f_flush_regs",
    "pe_recorder_vs_const_f_flush_cpu_ms",
    "pe_recorder_ps_const_f_flush_records",
    "pe_recorder_ps_const_f_flush_regs",
    "pe_recorder_ps_const_f_flush_cpu_ms",
    "pe_recorder_chunk_barrier_flush_calls",
    "pe_recorder_chunk_barrier_const_cpu_ms",
    "pe_recorder_apply_state_build_calls",
    "pe_recorder_apply_state_build_cpu_ms",
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
        return augment_with_encoder_sidecar_metrics(
            path,
            augment_with_pe_recorder_metrics(data, counters),
        )
    resolved = result_path(path)
    if not resolved.exists():
        raise SystemExit(f"missing result.json: {resolved}")
    data = json.loads(resolved.read_text(encoding="utf-8"))
    counters = data.get("dxmt9_perf_counters")
    if not isinstance(counters, dict):
        raise SystemExit(f"missing dxmt9_perf_counters in {resolved}")
    return augment_with_encoder_sidecar_metrics(
        resolved.parent,
        augment_with_pe_recorder_metrics(data, counters),
    )


def augment_with_pe_recorder_metrics(
    data: dict[str, Any],
    counters: dict[str, Any],
) -> dict[str, Any]:
    pe_counters = data.get("dxmt9_pe_recorder_counters")
    if not isinstance(pe_counters, dict) or not pe_counters:
        return counters

    augmented = dict(counters)
    for source_key, target_key in PE_RECORDER_COUNTER_MAP:
        if source_key in pe_counters:
            augmented[target_key] = pe_counters[source_key]

    for source_prefix, target_prefix in PE_RECORDER_HOT_SETTERS:
        for source_suffix, target_suffix in (
            ("Calls", "calls"),
            ("Dirty", "dirty"),
            ("CpuMs", "cpu_ms"),
            ("CpuMaxMs", "cpu_max_ms"),
        ):
            source_key = f"hotSetter{source_prefix}{source_suffix}"
            if source_key in pe_counters:
                augmented[f"pe_recorder_hot_setter_{target_prefix}_{target_suffix}"] = (
                    pe_counters[source_key]
                )

    for rank in range(1, 5):
        source_prefix = f"interAppendTop{rank}"
        target_prefix = f"pe_recorder_inter_append_top{rank}"
        for source_suffix, target_suffix in (
            ("Prev", "prev"),
            ("Next", "next"),
            ("Samples", "samples"),
            ("Ms", "ms"),
            ("MaxMs", "max_ms"),
        ):
            source_key = f"{source_prefix}{source_suffix}"
            if source_key in pe_counters:
                augmented[f"{target_prefix}_{target_suffix}"] = pe_counters[source_key]

    for source_prefix, target_prefix in PE_RECORDER_FOCUSED_GAP_PREFIXES:
        target_base = f"pe_recorder_gap_{target_prefix}"
        for rank in (1, 2):
            for source_suffix, target_suffix in (
                ("CallFamily", "call_family"),
                ("Samples", "samples"),
                ("Ms", "ms"),
                ("MaxMs", "max_ms"),
            ):
                source_key = f"{source_prefix}Top{rank}{source_suffix}"
                if source_key in pe_counters:
                    augmented[f"{target_base}_top{rank}_{target_suffix}"] = (
                        pe_counters[source_key]
                    )
            for source_suffix, target_suffix in (
                ("CallFamily", "call_family"),
                ("Samples", "samples"),
                ("CallName", "call_name"),
                ("CallNameSamples", "call_name_samples"),
                ("CallNameCpuMs", "call_name_cpu_ms"),
                ("CallNameCpuMaxMs", "call_name_cpu_max_ms"),
            ):
                source_key = f"{source_prefix}BetweenTop{rank}{source_suffix}"
                if source_key in pe_counters:
                    augmented[f"{target_base}_between_top{rank}_{target_suffix}"] = (
                        pe_counters[source_key]
                    )
            for source_suffix, target_suffix in (
                ("PrevFamily", "prev_family"),
                ("NextFamily", "next_family"),
                ("Samples", "samples"),
                ("Ms", "ms"),
                ("MaxMs", "max_ms"),
            ):
                source_key = f"{source_prefix}BetweenGapTop{rank}{source_suffix}"
                if source_key in pe_counters:
                    augmented[f"{target_base}_between_gap_top{rank}_{target_suffix}"] = (
                        pe_counters[source_key]
                    )
            for source_suffix, target_suffix in (
                ("PrevCallName", "prev_call_name"),
                ("NextCallName", "next_call_name"),
                ("NameSamples", "name_samples"),
                ("NameMs", "name_ms"),
                ("NameMaxMs", "name_max_ms"),
            ):
                source_key = f"{source_prefix}BetweenGapTop{rank}{source_suffix}"
                if source_key in pe_counters:
                    augmented[
                        f"{target_base}_between_gap_top{rank}_{target_suffix}"
                    ] = pe_counters[source_key]
            for source_suffix, target_suffix in (
                ("PrevCallName", "prev_call_name"),
                ("NextCallName", "next_call_name"),
                ("CallerModule", "caller_module"),
                ("CallerRva", "caller_rva"),
                ("Samples", "samples"),
                ("Ms", "ms"),
                ("MaxMs", "max_ms"),
            ):
                source_key = f"{source_prefix}BetweenGapSiteTop{rank}{source_suffix}"
                if source_key in pe_counters:
                    augmented[
                        f"{target_base}_between_gap_site_top{rank}_{target_suffix}"
                    ] = pe_counters[source_key]
        for source_suffix, target_suffix in (
            ("PhaseSamples", "phase_samples"),
            ("PreCallMs", "pre_call_ms"),
            ("PreCallMaxMs", "pre_call_max_ms"),
            ("InsideCallMs", "inside_call_ms"),
            ("InsideCallMaxMs", "inside_call_max_ms"),
            ("TailSplitSamples", "tail_split_samples"),
            ("PrevCallTailMs", "prev_call_tail_ms"),
            ("PrevCallTailMaxMs", "prev_call_tail_max_ms"),
            ("BetweenCallsMs", "between_calls_ms"),
            ("BetweenCallsMaxMs", "between_calls_max_ms"),
            ("BetweenCallBodyCalls", "between_call_body_calls"),
            ("BetweenCallBodyCpuMs", "between_call_body_cpu_ms"),
            ("BetweenCallBodyCpuMaxMs", "between_call_body_cpu_max_ms"),
        ):
            source_key = f"{source_prefix}{source_suffix}"
            if source_key in pe_counters:
                augmented[f"{target_base}_{target_suffix}"] = pe_counters[source_key]

    return augmented


def csv_number(value: Any) -> float:
    parsed = number(value)
    return parsed if parsed is not None else 0.0


def add_counter(counters: dict[str, Any], key: str, value: float) -> None:
    counters[key] = (number(counters.get(key)) or 0.0) + value


def augment_with_encoder_sidecar_metrics(
    path: Path,
    counters: dict[str, Any],
) -> dict[str, Any]:
    csv_path = path / "3dmark05-perf-encoders.csv"
    if not csv_path.exists():
        return counters

    augmented = dict(counters)
    for key in (
        "encoder_sidecar_rows",
        "encoder_sidecar_end_reason_rt_change",
        "encoder_sidecar_end_reason_clear",
        "encoder_sidecar_end_reason_present",
        "encoder_sidecar_end_reason_final",
        "encoder_sidecar_end_reason_other",
        "encoder_sidecar_final_same_key_reopen",
        "encoder_sidecar_final_same_rt_reopen",
        "encoder_sidecar_final_same_depth_reopen",
        "encoder_sidecar_final_same_key_reopen_color_load_bytes",
        "encoder_sidecar_final_same_key_reopen_depth_load_bytes",
        "encoder_sidecar_final_same_key_reopen_final_color_store_bytes",
        "encoder_sidecar_final_same_key_reopen_final_depth_store_bytes",
        "encoder_sidecar_color_load_bytes",
        "encoder_sidecar_color_store_bytes",
        "encoder_sidecar_depth_load_bytes",
        "encoder_sidecar_depth_store_bytes",
    ):
        augmented.setdefault(key, 0.0)
    with csv_path.open(newline="", encoding="utf-8") as handle:
        rows = list(csv.DictReader(handle))
        for row in rows:
            add_counter(augmented, "encoder_sidecar_rows", 1.0)
            end_reason = (row.get("end_reason") or "unknown").strip().lower()
            if end_reason in {"rt_change", "clear", "present", "final"}:
                add_counter(augmented, f"encoder_sidecar_end_reason_{end_reason}", 1.0)
            else:
                add_counter(augmented, "encoder_sidecar_end_reason_other", 1.0)
            add_counter(
                augmented,
                "encoder_sidecar_color_load_bytes",
                csv_number(row.get("color_load_bytes")),
            )
            add_counter(
                augmented,
                "encoder_sidecar_color_store_bytes",
                csv_number(row.get("color_store_bytes")),
            )
            add_counter(
                augmented,
                "encoder_sidecar_depth_load_bytes",
                csv_number(row.get("depth_load_bytes")),
            )
            add_counter(
                augmented,
                "encoder_sidecar_depth_store_bytes",
                csv_number(row.get("depth_store_bytes")),
            )
        for row, next_row in zip(rows, rows[1:]):
            end_reason = (row.get("end_reason") or "unknown").strip().lower()
            if end_reason != "final":
                continue

            rt = (row.get("rt") or "").strip()
            depth = (row.get("depth") or "").strip()
            next_rt = (next_row.get("rt") or "").strip()
            next_depth = (next_row.get("depth") or "").strip()
            has_attachment_key = bool(rt or depth)

            if has_attachment_key and rt == next_rt:
                add_counter(augmented, "encoder_sidecar_final_same_rt_reopen", 1.0)
            if has_attachment_key and depth == next_depth:
                add_counter(augmented, "encoder_sidecar_final_same_depth_reopen", 1.0)
            if has_attachment_key and rt == next_rt and depth == next_depth:
                add_counter(augmented, "encoder_sidecar_final_same_key_reopen", 1.0)
                add_counter(
                    augmented,
                    "encoder_sidecar_final_same_key_reopen_color_load_bytes",
                    csv_number(next_row.get("color_load_bytes")),
                )
                add_counter(
                    augmented,
                    "encoder_sidecar_final_same_key_reopen_depth_load_bytes",
                    csv_number(next_row.get("depth_load_bytes")),
                )
                add_counter(
                    augmented,
                    "encoder_sidecar_final_same_key_reopen_final_color_store_bytes",
                    csv_number(row.get("color_store_bytes")),
                )
                add_counter(
                    augmented,
                    "encoder_sidecar_final_same_key_reopen_final_depth_store_bytes",
                    csv_number(row.get("depth_store_bytes")),
                )
    return augmented


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
    snapshot_state_copy_cpu_ms = number(counter(
        counters,
        "d3d9_snapshot_state_copy_cpu_ms",
    )) or 0.0
    snapshot_state_materialized = number(counter(
        counters,
        "d3d9_snapshot_state_materialized",
    )) or 0.0
    snapshot_state_materialized_bytes = number(counter(
        counters,
        "d3d9_snapshot_state_materialized_bytes",
    )) or 0.0
    snapshot_state_elided = number(counter(
        counters,
        "d3d9_snapshot_state_elided",
    )) or 0.0
    snapshot_state_elided_bytes = number(counter(
        counters,
        "d3d9_snapshot_state_elided_bytes",
    )) or 0.0
    submission_carrier_records = number(counter(
        counters,
        "d3d9_snapshot_submission_carrier_records",
    )) or 0.0
    submission_carrier_bytes = number(counter(
        counters,
        "d3d9_snapshot_submission_carrier_bytes",
    )) or 0.0
    submission_carrier_state_storage_bytes = number(counter(
        counters,
        "d3d9_snapshot_submission_carrier_state_storage_bytes",
    )) or 0.0
    submission_carrier_uniform_storage_bytes = number(counter(
        counters,
        "d3d9_snapshot_submission_carrier_uniform_storage_bytes",
    )) or 0.0
    submission_carrier_compact_uniform_storage_bytes = number(counter(
        counters,
        "d3d9_snapshot_submission_carrier_compact_uniform_storage_bytes",
    )) or 0.0
    submission_carrier_unused_uniform_storage_records = number(counter(
        counters,
        "d3d9_snapshot_submission_carrier_unused_uniform_storage_records",
    )) or 0.0
    submission_carrier_unused_uniform_storage_bytes = number(counter(
        counters,
        "d3d9_snapshot_submission_carrier_unused_uniform_storage_bytes",
    )) or 0.0
    uniform_materialized = number(counter(
        counters,
        "d3d9_snapshot_uniform_materialized",
    )) or 0.0
    uniform_materialized_bytes = number(counter(
        counters,
        "d3d9_snapshot_uniform_materialized_bytes",
    )) or 0.0
    uniform_compact_candidate_bytes = number(counter(
        counters,
        "d3d9_snapshot_uniform_materialized_compact_candidate_bytes",
    )) or 0.0
    uniform_compact_saved_bytes = number(counter(
        counters,
        "d3d9_snapshot_uniform_materialized_compact_saved_bytes",
    )) or 0.0
    uniform_compact_fixed_bytes = number(counter(
        counters,
        "d3d9_snapshot_uniform_materialized_compact_fixed_bytes",
    )) or 0.0
    uniform_compact_vertex_bytes = number(counter(
        counters,
        "d3d9_snapshot_uniform_materialized_compact_vertex_bytes",
    )) or 0.0
    uniform_compact_pixel_bytes = number(counter(
        counters,
        "d3d9_snapshot_uniform_materialized_compact_pixel_bytes",
    )) or 0.0
    uniform_elided = number(counter(
        counters,
        "d3d9_snapshot_uniform_elided",
    )) or 0.0
    uniform_adjacent_previous_payload = number(counter(
        counters,
        "d3d9_snapshot_uniform_adjacent_previous_payload",
    )) or 0.0
    uniform_adjacent_same_fixed_payload_hash = number(counter(
        counters,
        "d3d9_snapshot_uniform_adjacent_same_fixed_payload_hash",
    )) or 0.0
    uniform_adjacent_same_fixed_and_shader_const_hashes = number(counter(
        counters,
        "d3d9_snapshot_uniform_adjacent_same_fixed_and_shader_const_hashes",
    )) or 0.0
    uniform_appends = number(counter(
        counters,
        "draw_uniform_payload_appends",
    )) or 0.0
    uniform_append_bytes = number(counter(
        counters,
        "draw_uniform_payload_append_bytes",
    )) or 0.0
    uniform_fixed_appends = number(counter(
        counters,
        "draw_uniform_fixed_payload_appends",
    )) or 0.0
    uniform_fixed_append_bytes = number(counter(
        counters,
        "draw_uniform_fixed_payload_append_bytes",
    )) or 0.0
    uniform_vertex_appends = number(counter(
        counters,
        "draw_uniform_vertex_constants_appends",
    )) or 0.0
    uniform_vertex_append_bytes = number(counter(
        counters,
        "draw_uniform_vertex_constants_append_bytes",
    )) or 0.0
    uniform_pixel_appends = number(counter(
        counters,
        "draw_uniform_pixel_constants_appends",
    )) or 0.0
    uniform_pixel_append_bytes = number(counter(
        counters,
        "draw_uniform_pixel_constants_append_bytes",
    )) or 0.0
    uniform_backend_materialized = number(counter(
        counters,
        "draw_uniform_payload_materialized",
    )) or 0.0
    uniform_backend_materialized_bytes = number(counter(
        counters,
        "draw_uniform_payload_materialized_bytes",
    )) or 0.0
    uniform_backend_materialize_cpu_ms = number(counter(
        counters,
        "draw_uniform_payload_materialize_cpu_ms",
    )) or 0.0
    uniform_backend_materialize_fallbacks = number(counter(
        counters,
        "draw_uniform_payload_materialize_fallbacks",
    )) or 0.0
    uniform_backend_materialize_sites = (
        "other",
        "draw_encoder_command",
        "draw_encoder_param",
        "framegraph_command",
        "framegraph_param",
        "queue_observation",
    )
    uniform_semantic_hash_misses = number(counter(
        counters,
        "draw_uniform_payload_lookup_semantic_hash_misses",
    )) or 0.0
    uniform_semantic_hash_miss_bytes = number(counter(
        counters,
        "draw_uniform_payload_lookup_semantic_hash_miss_bytes",
    )) or 0.0
    snapshot_state_total = snapshot_state_materialized + snapshot_state_elided
    uniform_snapshot_total = uniform_materialized + uniform_elided
    uniform_payload_record_append_bytes = (
        uniform_append_bytes -
        uniform_fixed_append_bytes -
        uniform_vertex_append_bytes -
        uniform_pixel_append_bytes
    )
    uniform_stage_append_bytes = uniform_vertex_append_bytes + uniform_pixel_append_bytes
    uniform_compact_stage_bytes = uniform_compact_vertex_bytes + uniform_compact_pixel_bytes
    snapshot_cache_uniform_build_cpu_ms = number(counter(
        counters,
        "d3d9_snapshot_cache_uniform_build_cpu_ms",
    )) or 0.0
    snapshot_cache_uniform_hash_cpu_ms = number(counter(
        counters,
        "d3d9_snapshot_cache_uniform_hash_cpu_ms",
    )) or 0.0
    batch_miss_uniform_build_cpu_ms = number(counter(
        counters,
        "d3d9_snapshot_cache_batch_miss_uniform_build_cpu_ms",
    )) or 0.0
    batch_miss_uniform_hash_cpu_ms = number(counter(
        counters,
        "d3d9_snapshot_cache_batch_miss_uniform_build_hash_cpu_ms",
    )) or 0.0
    batch_miss_vs_const_hash_cpu_ms = number(counter(
        counters,
        "d3d9_snapshot_cache_batch_miss_uniform_build_vs_const_hash_cpu_ms",
    )) or 0.0
    batch_miss_vs_const_hash_reuse = number(counter(
        counters,
        "d3d9_snapshot_cache_batch_miss_uniform_vs_const_hash_reuse",
    )) or 0.0
    batch_miss_vs_const_hash_build = number(counter(
        counters,
        "d3d9_snapshot_cache_batch_miss_uniform_vs_const_hash_build",
    )) or 0.0
    batch_miss_ps_const_hash_cpu_ms = number(counter(
        counters,
        "d3d9_snapshot_cache_batch_miss_uniform_build_ps_const_hash_cpu_ms",
    )) or 0.0
    batch_miss_ps_const_hash_reuse = number(counter(
        counters,
        "d3d9_snapshot_cache_batch_miss_uniform_ps_const_hash_reuse",
    )) or 0.0
    batch_miss_ps_const_hash_build = number(counter(
        counters,
        "d3d9_snapshot_cache_batch_miss_uniform_ps_const_hash_build",
    )) or 0.0
    batch_miss_vs_const_hash_memo_probe = number(counter(
        counters,
        "d3d9_snapshot_cache_batch_miss_uniform_vs_const_hash_memo_probe",
    )) or 0.0
    batch_miss_vs_const_hash_memo_hits = number(counter(
        counters,
        "d3d9_snapshot_cache_batch_miss_uniform_vs_const_hash_memo_hits",
    )) or 0.0
    batch_miss_vs_const_hash_memo_misses = number(counter(
        counters,
        "d3d9_snapshot_cache_batch_miss_uniform_vs_const_hash_memo_misses",
    )) or 0.0
    batch_miss_vs_const_hash_memo_stores = number(counter(
        counters,
        "d3d9_snapshot_cache_batch_miss_uniform_vs_const_hash_memo_stores",
    )) or 0.0
    batch_miss_ps_const_hash_memo_probe = number(counter(
        counters,
        "d3d9_snapshot_cache_batch_miss_uniform_ps_const_hash_memo_probe",
    )) or 0.0
    batch_miss_ps_const_hash_memo_hits = number(counter(
        counters,
        "d3d9_snapshot_cache_batch_miss_uniform_ps_const_hash_memo_hits",
    )) or 0.0
    batch_miss_ps_const_hash_memo_misses = number(counter(
        counters,
        "d3d9_snapshot_cache_batch_miss_uniform_ps_const_hash_memo_misses",
    )) or 0.0
    batch_miss_ps_const_hash_memo_stores = number(counter(
        counters,
        "d3d9_snapshot_cache_batch_miss_uniform_ps_const_hash_memo_stores",
    )) or 0.0
    batch_miss_nonconst_hash_cpu_ms = number(counter(
        counters,
        "d3d9_snapshot_cache_batch_miss_uniform_build_nonconst_hash_cpu_ms",
    )) or 0.0
    batch_miss_vs_const_hash_total = (
        batch_miss_vs_const_hash_reuse + batch_miss_vs_const_hash_build
    )
    batch_miss_ps_const_hash_total = (
        batch_miss_ps_const_hash_reuse + batch_miss_ps_const_hash_build
    )
    snapshot_uniform_copy_cpu_ms = number(counter(
        counters,
        "d3d9_snapshot_uniform_copy_cpu_ms",
    )) or 0.0
    append_uniform_cpu_ms = number(counter(
        counters,
        "submit_draw_run_batch_append_uniform_cpu_ms",
    )) or 0.0
    uniform_lookup_cpu_ms = number(counter(
        counters,
        "draw_uniform_payload_lookup_cpu_ms",
    )) or 0.0
    uniform_append_copy_cpu_ms = number(counter(
        counters,
        "draw_uniform_payload_append_copy_cpu_ms",
    )) or 0.0
    discarded_state_records = number(counter(
        counters,
        "submit_draw_run_batch_discarded_state_records",
    )) or 0.0
    discarded_state_bytes = number(counter(
        counters,
        "submit_draw_run_batch_discarded_state_bytes",
    )) or 0.0
    adjacent_same_generation_lane = number(counter(
        counters,
        "submit_draw_run_batch_submission_adjacent_same_generation_lane",
    )) or 0.0
    compat_same_generation_lane = number(counter(
        counters,
        "submit_draw_run_batch_compat_same_generation_lane",
    )) or 0.0
    compat_same_generation_lane_compatible = number(counter(
        counters,
        "submit_draw_run_batch_compat_same_generation_lane_compatible",
    )) or 0.0
    compat_same_generation_lane_incompatible = number(counter(
        counters,
        "submit_draw_run_batch_compat_same_generation_lane_incompatible",
    )) or 0.0
    completion_wait_ms = number(counter(counters, "completion_wait_ms")) or 0.0
    completion_present_wait_ms = number(
        counter(counters, "completion_present_wait_ms")
    ) or 0.0
    completion_wait_with_enqueue_ms = number(
        counter(counters, "completion_wait_with_enqueue_ms")
    ) or 0.0
    completion_wait_without_enqueue_ms = number(
        counter(counters, "completion_wait_without_enqueue_ms")
    ) or 0.0
    completion_present_wait_with_enqueue_ms = number(
        counter(counters, "completion_present_wait_with_enqueue_ms")
    ) or 0.0
    completion_present_wait_without_enqueue_ms = number(
        counter(counters, "completion_present_wait_without_enqueue_ms")
    ) or 0.0
    completion_enqueue_while_waiting = number(
        counter(counters, "completion_enqueue_while_waiting")
    ) or 0.0
    completion_enqueue_while_waiting_present = number(
        counter(counters, "completion_enqueue_while_waiting_present")
    ) or 0.0
    completion_wait_commit_chunk_entries = number(
        counter(counters, "completion_wait_commit_chunk_entries")
    ) or 0.0
    completion_wait_commit_chunk_replay_starts = number(
        counter(counters, "completion_wait_commit_chunk_replay_starts")
    ) or 0.0
    completion_wait_commit_chunk_replay_ends = number(
        counter(counters, "completion_wait_commit_chunk_replay_ends")
    ) or 0.0
    completion_wait_commit_chunk_replay_cpu_ms = number(
        counter(counters, "completion_wait_commit_chunk_replay_cpu_ms")
    ) or 0.0
    chunk_publish_slot_residency_ms = number(
        counter(counters, "chunk_publish_slot_residency_ms")
    ) or 0.0
    chunk_publish_slot_residency_p50_ms = number(
        counter(counters, "chunk_publish_slot_residency_p50_ms")
    )
    chunk_publish_slot_residency_p95_ms = number(
        counter(counters, "chunk_publish_slot_residency_p95_ms")
    )
    chunk_publish_slot_residency_present_ms = number(
        counter(counters, "chunk_publish_slot_residency_present_ms")
    ) or 0.0
    chunk_publish_slot_residency_present_p50_ms = number(
        counter(counters, "chunk_publish_slot_residency_present_p50_ms")
    )
    chunk_publish_slot_residency_present_p95_ms = number(
        counter(counters, "chunk_publish_slot_residency_present_p95_ms")
    )
    chunk_publish_slot_residency_nonpresent_ms = number(
        counter(counters, "chunk_publish_slot_residency_nonpresent_ms")
    ) or 0.0
    chunk_publish_slot_residency_nonpresent_p50_ms = number(
        counter(counters, "chunk_publish_slot_residency_nonpresent_p50_ms")
    )
    chunk_publish_slot_residency_nonpresent_p95_ms = number(
        counter(counters, "chunk_publish_slot_residency_nonpresent_p95_ms")
    )
    chunk_publish_reason_present_split_before = number(counter(
        counters,
        "chunk_publish_reason_present_split_before",
    )) or 0.0
    chunk_publish_present_split_before_tail_draw_run = number(counter(
        counters,
        "chunk_publish_present_split_before_tail_draw_run",
    )) or 0.0
    chunk_publish_present_split_before_tail_clear = number(counter(
        counters,
        "chunk_publish_present_split_before_tail_clear",
    )) or 0.0
    chunk_publish_present_split_before_tail_surface_copy = number(counter(
        counters,
        "chunk_publish_present_split_before_tail_surface_copy",
    )) or 0.0
    chunk_publish_present_split_before_tail_stretch_rect = number(counter(
        counters,
        "chunk_publish_present_split_before_tail_stretch_rect",
    )) or 0.0
    chunk_publish_present_split_before_tail_readback = number(counter(
        counters,
        "chunk_publish_present_split_before_tail_readback",
    )) or 0.0
    chunk_publish_present_split_before_tail_color_fill = number(counter(
        counters,
        "chunk_publish_present_split_before_tail_color_fill",
    )) or 0.0
    chunk_publish_present_split_before_tail_depth_resolve = number(counter(
        counters,
        "chunk_publish_present_split_before_tail_depth_resolve",
    )) or 0.0
    chunk_publish_present_split_before_tail_present = number(counter(
        counters,
        "chunk_publish_present_split_before_tail_present",
    )) or 0.0
    chunk_publish_present_split_before_tail_empty = number(counter(
        counters,
        "chunk_publish_present_split_before_tail_empty",
    )) or 0.0
    chunk_publish_present_split_before_draw_only = number(counter(
        counters,
        "chunk_publish_present_split_before_draw_only",
    )) or 0.0
    chunk_publish_present_pre_present_opportunity_slots = number(counter(
        counters,
        "chunk_publish_present_pre_present_opportunity_slots",
    )) or 0.0
    chunk_publish_present_pre_present_opportunity_tail_slots = number(counter(
        counters,
        "chunk_publish_present_pre_present_opportunity_tail_slots",
    )) or 0.0
    chunk_publish_present_pre_present_opportunity_commands = number(counter(
        counters,
        "chunk_publish_present_pre_present_opportunity_commands",
    )) or 0.0
    chunk_publish_present_pre_present_opportunity_draw_runs = number(counter(
        counters,
        "chunk_publish_present_pre_present_opportunity_draw_runs",
    )) or 0.0
    chunk_publish_present_pre_present_opportunity_draw_items = number(counter(
        counters,
        "chunk_publish_present_pre_present_opportunity_draw_items",
    )) or 0.0
    chunk_publish_present_pre_present_opportunity_non_draw_commands = number(counter(
        counters,
        "chunk_publish_present_pre_present_opportunity_non_draw_commands",
    )) or 0.0
    chunk_publish_present_pre_present_opportunity_payload_bytes = number(counter(
        counters,
        "chunk_publish_present_pre_present_opportunity_payload_bytes",
    )) or 0.0
    chunk_publish_present_pre_present_opportunity_residency_ms = number(counter(
        counters,
        "chunk_publish_present_pre_present_opportunity_residency_ms",
    )) or 0.0
    chunk_publish_present_pre_present_opportunity_residency_p50_ms = number(counter(
        counters,
        "chunk_publish_present_pre_present_opportunity_residency_p50_ms",
    ))
    chunk_publish_present_pre_present_opportunity_residency_p95_ms = number(counter(
        counters,
        "chunk_publish_present_pre_present_opportunity_residency_p95_ms",
    ))
    chunk_publish_present_pre_present_opportunity_tail_draw_run = number(counter(
        counters,
        "chunk_publish_present_pre_present_opportunity_tail_draw_run",
    )) or 0.0
    chunk_publish_present_pre_present_opportunity_tail_clear = number(counter(
        counters,
        "chunk_publish_present_pre_present_opportunity_tail_clear",
    )) or 0.0
    chunk_publish_present_pre_present_opportunity_tail_surface_copy = number(counter(
        counters,
        "chunk_publish_present_pre_present_opportunity_tail_surface_copy",
    )) or 0.0
    chunk_publish_present_pre_present_opportunity_tail_stretch_rect = number(counter(
        counters,
        "chunk_publish_present_pre_present_opportunity_tail_stretch_rect",
    )) or 0.0
    chunk_publish_present_pre_present_opportunity_tail_readback = number(counter(
        counters,
        "chunk_publish_present_pre_present_opportunity_tail_readback",
    )) or 0.0
    chunk_publish_present_pre_present_opportunity_tail_color_fill = number(counter(
        counters,
        "chunk_publish_present_pre_present_opportunity_tail_color_fill",
    )) or 0.0
    chunk_publish_present_pre_present_opportunity_tail_depth_resolve = number(counter(
        counters,
        "chunk_publish_present_pre_present_opportunity_tail_depth_resolve",
    )) or 0.0
    chunk_publish_present_pre_present_opportunity_tail_present = number(counter(
        counters,
        "chunk_publish_present_pre_present_opportunity_tail_present",
    )) or 0.0
    chunk_publish_present_pre_present_opportunity_tail_empty = number(counter(
        counters,
        "chunk_publish_present_pre_present_opportunity_tail_empty",
    )) or 0.0
    chunk_publish_present_pre_present_opportunity_draw_only = number(counter(
        counters,
        "chunk_publish_present_pre_present_opportunity_draw_only",
    )) or 0.0
    encode_ready_depth_samples = number(counter(
        counters, "encode_dequeue_ready_depth_samples",
    )) or 0.0
    encode_ready_depth_total = number(counter(
        counters, "encode_dequeue_ready_depth_total",
    )) or 0.0
    encode_ready_depth_gt1 = number(counter(
        counters, "encode_dequeue_ready_depth_gt1",
    )) or 0.0
    encode_ready_depth_gt2 = number(counter(
        counters, "encode_dequeue_ready_depth_gt2",
    )) or 0.0
    encode_ready_depth_gt4 = number(counter(
        counters, "encode_dequeue_ready_depth_gt4",
    )) or 0.0
    commit_chunk_replay_cpu_ms = number(
        counter(counters, "commit_chunk_replay_cpu_ms")
    ) or 0.0
    commit_chunk_queue_draw_submission_cpu_ms = number(
        counter(counters, "commit_chunk_queue_draw_submission_cpu_ms")
    ) or 0.0
    d3d9_snapshot_draw_submission_cpu_ms = number(
        counter(counters, "d3d9_snapshot_draw_submission_cpu_ms")
    ) or 0.0
    d3d9_snapshot_cache_lookup_cpu_ms = number(
        counter(counters, "d3d9_snapshot_cache_lookup_cpu_ms")
    ) or 0.0
    encode_chunk_cpu_ms = number(counter(counters, "encode_chunk_cpu_ms")) or 0.0
    encode_draw_cpu_ms = number(counter(counters, "encode_draw_cpu_ms")) or 0.0
    argbuf_setup_cpu_ms = number(
        counter(counters, "encode_draw_argbuf_setup_cpu_ms")
    ) or 0.0
    argbuf_open_cpu_ms = number(
        counter(counters, "encode_draw_argbuf_open_cpu_ms")
    ) or 0.0
    argbuf_cbuf_update_cpu_ms = number(
        counter(counters, "encode_draw_argbuf_cbuf_update_cpu_ms")
    ) or 0.0
    argbuf_cbuf_update_vs_cpu_ms = number(
        counter(counters, "encode_draw_argbuf_cbuf_update_vs_cpu_ms")
    ) or 0.0
    no_enqueue_commit_entry_to_publish_ms = number(counter(
        counters,
        "completion_no_enqueue_stage_commit_entry_to_publish_ms",
    )) or 0.0
    no_enqueue_publish_to_encode_dequeue_ms = number(counter(
        counters,
        "completion_no_enqueue_stage_publish_to_encode_dequeue_ms",
    )) or 0.0
    no_enqueue_encode_dequeue_to_commit_ms = number(counter(
        counters,
        "completion_no_enqueue_stage_encode_dequeue_to_command_buffer_commit_ms",
    )) or 0.0
    no_enqueue_wait_to_next_enqueue_ms = number(counter(
        counters,
        "completion_no_enqueue_wait_to_next_enqueue_ms",
    )) or 0.0
    no_enqueue_completed_replay_cpu_before_publish_ms = number(counter(
        counters,
        "completion_no_enqueue_commit_chunk_completed_replay_cpu_before_publish_ms",
    )) or 0.0
    no_enqueue_active_replay_cpu_before_publish_ms = number(counter(
        counters,
        "completion_no_enqueue_commit_chunk_active_replay_cpu_before_publish_ms",
    )) or 0.0
    no_enqueue_inter_replay_gap_before_publish_ms = number(counter(
        counters,
        "completion_no_enqueue_commit_chunk_inter_replay_gap_before_publish_ms",
    )) or 0.0
    no_enqueue_commit_publish_wait_before_publish_ms = number(counter(
        counters,
        "completion_no_enqueue_commit_publish_wait_before_publish_ms",
    )) or 0.0
    no_enqueue_commit_publish_on_before_publish_cpu_ms = number(counter(
        counters,
        "completion_no_enqueue_commit_publish_on_before_publish_cpu_ms",
    )) or 0.0
    no_enqueue_first_publish_slot_samples = number(counter(
        counters,
        "completion_no_enqueue_first_publish_slot_samples",
    )) or 0.0
    no_enqueue_first_publish_slot_commands = number(counter(
        counters,
        "completion_no_enqueue_first_publish_slot_commands",
    )) or 0.0
    no_enqueue_first_publish_slot_draw_run_commands = number(counter(
        counters,
        "completion_no_enqueue_first_publish_slot_draw_run_commands",
    )) or 0.0
    no_enqueue_first_publish_slot_draw_items = number(counter(
        counters,
        "completion_no_enqueue_first_publish_slot_draw_items",
    )) or 0.0
    no_enqueue_first_publish_slot_non_draw_commands = number(counter(
        counters,
        "completion_no_enqueue_first_publish_slot_non_draw_commands",
    )) or 0.0
    no_enqueue_first_publish_slot_payload_bytes = number(counter(
        counters,
        "completion_no_enqueue_first_publish_slot_payload_bytes",
    )) or 0.0
    no_enqueue_first_publish_slot_present_commands = number(counter(
        counters,
        "completion_no_enqueue_first_publish_slot_present_commands",
    )) or 0.0
    no_enqueue_first_publish_slot_pre_present_commands = number(counter(
        counters,
        "completion_no_enqueue_first_publish_slot_pre_present_commands",
    )) or 0.0
    no_enqueue_first_publish_slot_pre_present_draw_run_commands = number(counter(
        counters,
        "completion_no_enqueue_first_publish_slot_pre_present_draw_run_commands",
    )) or 0.0
    no_enqueue_first_publish_slot_pre_present_draw_items = number(counter(
        counters,
        "completion_no_enqueue_first_publish_slot_pre_present_draw_items",
    )) or 0.0
    no_enqueue_first_publish_slot_pre_present_non_draw_commands = number(counter(
        counters,
        "completion_no_enqueue_first_publish_slot_pre_present_non_draw_commands",
    )) or 0.0
    no_enqueue_first_publish_slot_pre_present_payload_bytes = number(counter(
        counters,
        "completion_no_enqueue_first_publish_slot_pre_present_payload_bytes",
    )) or 0.0
    no_enqueue_first_publish_slot_post_present_commands = number(counter(
        counters,
        "completion_no_enqueue_first_publish_slot_post_present_commands",
    )) or 0.0
    no_enqueue_first_publish_slot_present_tail_slots = number(counter(
        counters,
        "completion_no_enqueue_first_publish_slot_present_tail_slots",
    )) or 0.0
    no_enqueue_before_publish_replay_cpu_ms = (
        no_enqueue_completed_replay_cpu_before_publish_ms +
        no_enqueue_active_replay_cpu_before_publish_ms
    )
    no_enqueue_before_publish_replay_gap_ms = (
        no_enqueue_before_publish_replay_cpu_ms +
        no_enqueue_inter_replay_gap_before_publish_ms
    )
    no_enqueue_before_publish_closure_ms = (
        no_enqueue_before_publish_replay_gap_ms +
        no_enqueue_commit_publish_wait_before_publish_ms
    )
    no_enqueue_before_publish_residual_ms = (
        no_enqueue_commit_entry_to_publish_ms -
        no_enqueue_before_publish_closure_ms
    )
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
    mib = 1024.0 * 1024.0
    encoder_sidecar_rows = number(counter(counters, "encoder_sidecar_rows"))
    has_encoder_sidecar = encoder_sidecar_rows is not None and encoder_sidecar_rows > 0

    def encoder_ratio(key: str) -> float | None:
        if not has_encoder_sidecar:
            return None
        return ratio(counters, key, "present_encoded")

    encoder_color_load_mib = (
        number(counter(counters, "encoder_sidecar_color_load_bytes")) or 0.0
    ) / mib
    encoder_color_store_mib = (
        number(counter(counters, "encoder_sidecar_color_store_bytes")) or 0.0
    ) / mib
    encoder_depth_load_mib = (
        number(counter(counters, "encoder_sidecar_depth_load_bytes")) or 0.0
    ) / mib
    encoder_depth_store_mib = (
        number(counter(counters, "encoder_sidecar_depth_store_bytes")) or 0.0
    ) / mib
    encoder_final_same_key_reopen = (
        number(counter(counters, "encoder_sidecar_final_same_key_reopen")) or 0.0
    )
    encoder_final_end_reason = (
        number(counter(counters, "encoder_sidecar_end_reason_final")) or 0.0
    )
    encoder_final_same_key_reopen_color_load_mib = (
        number(counter(
            counters,
            "encoder_sidecar_final_same_key_reopen_color_load_bytes",
        )) or 0.0
    ) / mib
    encoder_final_same_key_reopen_depth_load_mib = (
        number(counter(
            counters,
            "encoder_sidecar_final_same_key_reopen_depth_load_bytes",
        )) or 0.0
    ) / mib
    encoder_final_same_key_reopen_final_color_store_mib = (
        number(counter(
            counters,
            "encoder_sidecar_final_same_key_reopen_final_color_store_bytes",
        )) or 0.0
    ) / mib
    encoder_final_same_key_reopen_final_depth_store_mib = (
        number(counter(
            counters,
            "encoder_sidecar_final_same_key_reopen_final_depth_store_bytes",
        )) or 0.0
    ) / mib
    metrics = {
        "draws_per_present": ratio(counters, "draw_calls", "present_encoded"),
        "command_buffers_per_present": ratio(
            counters, "command_buffers", "present_encoded",
        ),
        "sub_command_buffers_per_present": ratio(
            counters, "sub_command_buffers", "present_encoded",
        ),
        "passes_per_present": ratio(counters, "render_pass_begin", "present_encoded"),
        "tile_preservation_mib": (
            (number(counter(counters, "render_pass_tile_preservation_bytes")) or 0.0) /
            mib
        ),
        "tile_preservation_mib_per_present": (
            (number(counter(counters, "render_pass_tile_preservation_bytes")) or 0.0) /
            mib / present
            if present else None
        ),
        "same_key_preservation_mib": (
            (number(counter(counters, "render_pass_same_key_reentry_preservation_bytes")) or 0.0) /
            mib
        ),
        "encoder_sidecar_rows_per_present": encoder_ratio("encoder_sidecar_rows"),
        "encoder_sidecar_rt_change_end_reason_per_present": encoder_ratio(
            "encoder_sidecar_end_reason_rt_change",
        ),
        "encoder_sidecar_clear_end_reason_per_present": encoder_ratio(
            "encoder_sidecar_end_reason_clear",
        ),
        "encoder_sidecar_present_end_reason_per_present": encoder_ratio(
            "encoder_sidecar_end_reason_present",
        ),
        "encoder_sidecar_final_end_reason_per_present": encoder_ratio(
            "encoder_sidecar_end_reason_final",
        ),
        "encoder_sidecar_final_same_key_reopen_per_present": encoder_ratio(
            "encoder_sidecar_final_same_key_reopen",
        ),
        "encoder_sidecar_final_same_key_reopen_share_pct": (
            encoder_final_same_key_reopen / encoder_final_end_reason * 100.0
            if has_encoder_sidecar and encoder_final_end_reason else None
        ),
        "encoder_sidecar_final_same_key_reopen_color_load_mib_per_present": (
            encoder_final_same_key_reopen_color_load_mib / present
            if has_encoder_sidecar and present else None
        ),
        "encoder_sidecar_final_same_key_reopen_depth_load_mib_per_present": (
            encoder_final_same_key_reopen_depth_load_mib / present
            if has_encoder_sidecar and present else None
        ),
        "encoder_sidecar_final_same_key_reopen_final_color_store_mib_per_present": (
            encoder_final_same_key_reopen_final_color_store_mib / present
            if has_encoder_sidecar and present else None
        ),
        "encoder_sidecar_final_same_key_reopen_final_depth_store_mib_per_present": (
            encoder_final_same_key_reopen_final_depth_store_mib / present
            if has_encoder_sidecar and present else None
        ),
        "encoder_sidecar_color_load_mib_per_present": (
            encoder_color_load_mib / present if has_encoder_sidecar and present else None
        ),
        "encoder_sidecar_color_store_mib_per_present": (
            encoder_color_store_mib / present if has_encoder_sidecar and present else None
        ),
        "encoder_sidecar_depth_load_mib_per_present": (
            encoder_depth_load_mib / present if has_encoder_sidecar and present else None
        ),
        "encoder_sidecar_depth_store_mib_per_present": (
            encoder_depth_store_mib / present if has_encoder_sidecar and present else None
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
            completion_wait_ms / present
            if present else None
        ),
        "completion_present_wait_ms_per_present": (
            completion_present_wait_ms / present if present else None
        ),
        "completion_wait_with_enqueue_ms_per_present": (
            completion_wait_with_enqueue_ms / present if present else None
        ),
        "completion_wait_without_enqueue_ms_per_present": (
            completion_wait_without_enqueue_ms / present if present else None
        ),
        "completion_wait_overlap_share_pct": (
            completion_wait_with_enqueue_ms / completion_wait_ms * 100.0
            if completion_wait_ms else None
        ),
        "completion_wait_no_enqueue_share_pct": (
            completion_wait_without_enqueue_ms / completion_wait_ms * 100.0
            if completion_wait_ms else None
        ),
        "completion_present_wait_with_enqueue_ms_per_present": (
            completion_present_wait_with_enqueue_ms / present if present else None
        ),
        "completion_present_wait_without_enqueue_ms_per_present": (
            completion_present_wait_without_enqueue_ms / present if present else None
        ),
        "completion_present_wait_overlap_share_pct": (
            completion_present_wait_with_enqueue_ms /
            completion_present_wait_ms * 100.0
            if completion_present_wait_ms else None
        ),
        "completion_present_wait_no_enqueue_share_pct": (
            completion_present_wait_without_enqueue_ms /
            completion_present_wait_ms * 100.0
            if completion_present_wait_ms else None
        ),
        "gpu_command_buffer_time_ms_per_present": ratio(
            counters,
            "gpu_command_buffer_time_ms",
            "present_encoded",
        ),
        "completion_enqueue_while_waiting_per_present": (
            completion_enqueue_while_waiting / present if present else None
        ),
        "completion_present_enqueue_while_waiting_per_present": (
            completion_enqueue_while_waiting_present / present if present else None
        ),
        "completion_wait_commit_chunk_entries_per_present": (
            completion_wait_commit_chunk_entries / present if present else None
        ),
        "completion_wait_commit_chunk_replay_starts_per_present": (
            completion_wait_commit_chunk_replay_starts / present if present else None
        ),
        "completion_wait_commit_chunk_replay_ends_per_present": (
            completion_wait_commit_chunk_replay_ends / present if present else None
        ),
        "completion_wait_commit_chunk_replay_cpu_ms_per_present": (
            completion_wait_commit_chunk_replay_cpu_ms / present if present else None
        ),
        "chunk_publish_slot_residency_ms_per_present": (
            chunk_publish_slot_residency_ms / present if present else None
        ),
        "chunk_publish_slot_residency_p50_ms": (
            chunk_publish_slot_residency_p50_ms
        ),
        "chunk_publish_slot_residency_p95_ms": (
            chunk_publish_slot_residency_p95_ms
        ),
        "chunk_publish_slot_residency_present_ms_per_present": (
            chunk_publish_slot_residency_present_ms / present if present else None
        ),
        "chunk_publish_slot_residency_present_p50_ms": (
            chunk_publish_slot_residency_present_p50_ms
        ),
        "chunk_publish_slot_residency_present_p95_ms": (
            chunk_publish_slot_residency_present_p95_ms
        ),
        "chunk_publish_slot_residency_nonpresent_ms_per_present": (
            chunk_publish_slot_residency_nonpresent_ms / present if present else None
        ),
        "chunk_publish_slot_residency_nonpresent_p50_ms": (
            chunk_publish_slot_residency_nonpresent_p50_ms
        ),
        "chunk_publish_slot_residency_nonpresent_p95_ms": (
            chunk_publish_slot_residency_nonpresent_p95_ms
        ),
        "chunk_publish_present_split_before_tail_draw_run_per_present": (
            chunk_publish_present_split_before_tail_draw_run / present
            if present else None
        ),
        "chunk_publish_present_split_before_tail_draw_run_share_pct": (
            chunk_publish_present_split_before_tail_draw_run /
            chunk_publish_reason_present_split_before * 100.0
            if chunk_publish_reason_present_split_before else None
        ),
        "chunk_publish_present_split_before_tail_clear_share_pct": (
            chunk_publish_present_split_before_tail_clear /
            chunk_publish_reason_present_split_before * 100.0
            if chunk_publish_reason_present_split_before else None
        ),
        "chunk_publish_present_split_before_tail_surface_copy_share_pct": (
            chunk_publish_present_split_before_tail_surface_copy /
            chunk_publish_reason_present_split_before * 100.0
            if chunk_publish_reason_present_split_before else None
        ),
        "chunk_publish_present_split_before_tail_stretch_rect_share_pct": (
            chunk_publish_present_split_before_tail_stretch_rect /
            chunk_publish_reason_present_split_before * 100.0
            if chunk_publish_reason_present_split_before else None
        ),
        "chunk_publish_present_split_before_tail_readback_share_pct": (
            chunk_publish_present_split_before_tail_readback /
            chunk_publish_reason_present_split_before * 100.0
            if chunk_publish_reason_present_split_before else None
        ),
        "chunk_publish_present_split_before_tail_color_fill_share_pct": (
            chunk_publish_present_split_before_tail_color_fill /
            chunk_publish_reason_present_split_before * 100.0
            if chunk_publish_reason_present_split_before else None
        ),
        "chunk_publish_present_split_before_tail_depth_resolve_share_pct": (
            chunk_publish_present_split_before_tail_depth_resolve /
            chunk_publish_reason_present_split_before * 100.0
            if chunk_publish_reason_present_split_before else None
        ),
        "chunk_publish_present_split_before_tail_present_share_pct": (
            chunk_publish_present_split_before_tail_present /
            chunk_publish_reason_present_split_before * 100.0
            if chunk_publish_reason_present_split_before else None
        ),
        "chunk_publish_present_split_before_tail_empty_share_pct": (
            chunk_publish_present_split_before_tail_empty /
            chunk_publish_reason_present_split_before * 100.0
            if chunk_publish_reason_present_split_before else None
        ),
        "chunk_publish_present_split_before_draw_only_share_pct": (
            chunk_publish_present_split_before_draw_only /
            chunk_publish_reason_present_split_before * 100.0
            if chunk_publish_reason_present_split_before else None
        ),
        "chunk_publish_present_pre_present_opportunity_slots_per_present": (
            chunk_publish_present_pre_present_opportunity_slots / present
            if present else None
        ),
        "chunk_publish_present_pre_present_opportunity_tail_slot_share_pct": (
            chunk_publish_present_pre_present_opportunity_tail_slots /
            chunk_publish_present_pre_present_opportunity_slots * 100.0
            if chunk_publish_present_pre_present_opportunity_slots else None
        ),
        "chunk_publish_present_pre_present_opportunity_commands_per_present": (
            chunk_publish_present_pre_present_opportunity_commands / present
            if present else None
        ),
        "chunk_publish_present_pre_present_opportunity_commands_per_slot": (
            chunk_publish_present_pre_present_opportunity_commands /
            chunk_publish_present_pre_present_opportunity_slots
            if chunk_publish_present_pre_present_opportunity_slots else None
        ),
        "chunk_publish_present_pre_present_opportunity_draw_runs_per_slot": (
            chunk_publish_present_pre_present_opportunity_draw_runs /
            chunk_publish_present_pre_present_opportunity_slots
            if chunk_publish_present_pre_present_opportunity_slots else None
        ),
        "chunk_publish_present_pre_present_opportunity_draw_items_per_present": (
            chunk_publish_present_pre_present_opportunity_draw_items / present
            if present else None
        ),
        "chunk_publish_present_pre_present_opportunity_draw_items_per_slot": (
            chunk_publish_present_pre_present_opportunity_draw_items /
            chunk_publish_present_pre_present_opportunity_slots
            if chunk_publish_present_pre_present_opportunity_slots else None
        ),
        "chunk_publish_present_pre_present_opportunity_non_draw_commands_per_slot": (
            chunk_publish_present_pre_present_opportunity_non_draw_commands /
            chunk_publish_present_pre_present_opportunity_slots
            if chunk_publish_present_pre_present_opportunity_slots else None
        ),
        "chunk_publish_present_pre_present_opportunity_payload_mib": (
            chunk_publish_present_pre_present_opportunity_payload_bytes /
            (1024.0 * 1024.0)
        ),
        "chunk_publish_present_pre_present_opportunity_payload_bytes_per_slot": (
            chunk_publish_present_pre_present_opportunity_payload_bytes /
            chunk_publish_present_pre_present_opportunity_slots
            if chunk_publish_present_pre_present_opportunity_slots else None
        ),
        "chunk_publish_present_pre_present_opportunity_residency_ms_per_present": (
            chunk_publish_present_pre_present_opportunity_residency_ms / present
            if present else None
        ),
        "chunk_publish_present_pre_present_opportunity_residency_p50_ms": (
            chunk_publish_present_pre_present_opportunity_residency_p50_ms
        ),
        "chunk_publish_present_pre_present_opportunity_residency_p95_ms": (
            chunk_publish_present_pre_present_opportunity_residency_p95_ms
        ),
        "chunk_publish_present_pre_present_opportunity_tail_draw_run_per_present": (
            chunk_publish_present_pre_present_opportunity_tail_draw_run / present
            if present else None
        ),
        "chunk_publish_present_pre_present_opportunity_tail_draw_run_share_pct": (
            chunk_publish_present_pre_present_opportunity_tail_draw_run /
            chunk_publish_present_pre_present_opportunity_slots * 100.0
            if chunk_publish_present_pre_present_opportunity_slots else None
        ),
        "chunk_publish_present_pre_present_opportunity_tail_clear_share_pct": (
            chunk_publish_present_pre_present_opportunity_tail_clear /
            chunk_publish_present_pre_present_opportunity_slots * 100.0
            if chunk_publish_present_pre_present_opportunity_slots else None
        ),
        "chunk_publish_present_pre_present_opportunity_tail_surface_copy_share_pct": (
            chunk_publish_present_pre_present_opportunity_tail_surface_copy /
            chunk_publish_present_pre_present_opportunity_slots * 100.0
            if chunk_publish_present_pre_present_opportunity_slots else None
        ),
        "chunk_publish_present_pre_present_opportunity_tail_stretch_rect_share_pct": (
            chunk_publish_present_pre_present_opportunity_tail_stretch_rect /
            chunk_publish_present_pre_present_opportunity_slots * 100.0
            if chunk_publish_present_pre_present_opportunity_slots else None
        ),
        "chunk_publish_present_pre_present_opportunity_tail_readback_share_pct": (
            chunk_publish_present_pre_present_opportunity_tail_readback /
            chunk_publish_present_pre_present_opportunity_slots * 100.0
            if chunk_publish_present_pre_present_opportunity_slots else None
        ),
        "chunk_publish_present_pre_present_opportunity_tail_color_fill_share_pct": (
            chunk_publish_present_pre_present_opportunity_tail_color_fill /
            chunk_publish_present_pre_present_opportunity_slots * 100.0
            if chunk_publish_present_pre_present_opportunity_slots else None
        ),
        "chunk_publish_present_pre_present_opportunity_tail_depth_resolve_share_pct": (
            chunk_publish_present_pre_present_opportunity_tail_depth_resolve /
            chunk_publish_present_pre_present_opportunity_slots * 100.0
            if chunk_publish_present_pre_present_opportunity_slots else None
        ),
        "chunk_publish_present_pre_present_opportunity_tail_present_share_pct": (
            chunk_publish_present_pre_present_opportunity_tail_present /
            chunk_publish_present_pre_present_opportunity_slots * 100.0
            if chunk_publish_present_pre_present_opportunity_slots else None
        ),
        "chunk_publish_present_pre_present_opportunity_tail_empty_share_pct": (
            chunk_publish_present_pre_present_opportunity_tail_empty /
            chunk_publish_present_pre_present_opportunity_slots * 100.0
            if chunk_publish_present_pre_present_opportunity_slots else None
        ),
        "chunk_publish_present_pre_present_opportunity_draw_only_share_pct": (
            chunk_publish_present_pre_present_opportunity_draw_only /
            chunk_publish_present_pre_present_opportunity_slots * 100.0
            if chunk_publish_present_pre_present_opportunity_slots else None
        ),
        "encode_ready_depth_avg": (
            encode_ready_depth_total / encode_ready_depth_samples
            if encode_ready_depth_samples else None
        ),
        "encode_ready_depth_gt1_per_present": (
            encode_ready_depth_gt1 / present if present else None
        ),
        "encode_ready_depth_gt2_per_present": (
            encode_ready_depth_gt2 / present if present else None
        ),
        "encode_ready_depth_gt4_per_present": (
            encode_ready_depth_gt4 / present if present else None
        ),
        "encode_ready_depth_gt1_share_pct": (
            encode_ready_depth_gt1 / encode_ready_depth_samples * 100.0
            if encode_ready_depth_samples else None
        ),
        "encode_ready_depth_gt2_share_pct": (
            encode_ready_depth_gt2 / encode_ready_depth_samples * 100.0
            if encode_ready_depth_samples else None
        ),
        "encode_ready_depth_gt4_share_pct": (
            encode_ready_depth_gt4 / encode_ready_depth_samples * 100.0
            if encode_ready_depth_samples else None
        ),
        "commit_chunk_replay_cpu_ms_per_present": (
            commit_chunk_replay_cpu_ms / present if present else None
        ),
        "commit_chunk_queue_draw_submission_cpu_ms_per_present": (
            commit_chunk_queue_draw_submission_cpu_ms / present if present else None
        ),
        "d3d9_snapshot_draw_submission_cpu_ms_per_present": (
            d3d9_snapshot_draw_submission_cpu_ms / present if present else None
        ),
        "d3d9_snapshot_cache_lookup_cpu_ms_per_present": (
            d3d9_snapshot_cache_lookup_cpu_ms / present if present else None
        ),
        "encode_chunk_cpu_ms_per_present": (
            encode_chunk_cpu_ms / present if present else None
        ),
        "encode_draw_cpu_ms_per_present": (
            encode_draw_cpu_ms / present if present else None
        ),
        "argbuf_setup_cpu_ms_per_present": (
            argbuf_setup_cpu_ms / present if present else None
        ),
        "argbuf_open_cpu_ms_per_present": (
            argbuf_open_cpu_ms / present if present else None
        ),
        "argbuf_cbuf_update_cpu_ms_per_present": (
            argbuf_cbuf_update_cpu_ms / present if present else None
        ),
        "argbuf_cbuf_update_vs_cpu_ms_per_present": (
            argbuf_cbuf_update_vs_cpu_ms / present if present else None
        ),
        "argbuf_open_share_of_setup_pct": (
            argbuf_open_cpu_ms / argbuf_setup_cpu_ms * 100.0
            if argbuf_setup_cpu_ms else None
        ),
        "argbuf_cbuf_update_share_of_setup_pct": (
            argbuf_cbuf_update_cpu_ms / argbuf_setup_cpu_ms * 100.0
            if argbuf_setup_cpu_ms else None
        ),
        "no_enqueue_stage_commit_entry_to_publish_ms_per_present": (
            no_enqueue_commit_entry_to_publish_ms / present if present else None
        ),
        "no_enqueue_stage_publish_to_encode_dequeue_ms_per_present": (
            no_enqueue_publish_to_encode_dequeue_ms / present if present else None
        ),
        "no_enqueue_stage_encode_dequeue_to_command_buffer_commit_ms_per_present": (
            no_enqueue_encode_dequeue_to_commit_ms / present if present else None
        ),
        "no_enqueue_wait_to_next_enqueue_ms_per_present": (
            no_enqueue_wait_to_next_enqueue_ms / present if present else None
        ),
        "no_enqueue_before_publish_completed_replay_cpu_ms_per_present": (
            no_enqueue_completed_replay_cpu_before_publish_ms / present
            if present else None
        ),
        "no_enqueue_before_publish_active_replay_cpu_ms_per_present": (
            no_enqueue_active_replay_cpu_before_publish_ms / present
            if present else None
        ),
        "no_enqueue_before_publish_inter_replay_gap_ms_per_present": (
            no_enqueue_inter_replay_gap_before_publish_ms / present
            if present else None
        ),
        "no_enqueue_before_publish_commit_publish_wait_ms_per_present": (
            no_enqueue_commit_publish_wait_before_publish_ms / present
            if present else None
        ),
        "no_enqueue_before_publish_on_before_publish_cpu_ms_per_present": (
            no_enqueue_commit_publish_on_before_publish_cpu_ms / present
            if present else None
        ),
        "no_enqueue_first_publish_slot_samples_per_present": (
            no_enqueue_first_publish_slot_samples / present if present else None
        ),
        "no_enqueue_first_publish_slot_commands_per_present": (
            no_enqueue_first_publish_slot_commands / present if present else None
        ),
        "no_enqueue_first_publish_slot_commands_per_slot": (
            no_enqueue_first_publish_slot_commands /
            no_enqueue_first_publish_slot_samples
            if no_enqueue_first_publish_slot_samples else None
        ),
        "no_enqueue_first_publish_slot_draw_run_commands_per_slot": (
            no_enqueue_first_publish_slot_draw_run_commands /
            no_enqueue_first_publish_slot_samples
            if no_enqueue_first_publish_slot_samples else None
        ),
        "no_enqueue_first_publish_slot_draw_items_per_slot": (
            no_enqueue_first_publish_slot_draw_items /
            no_enqueue_first_publish_slot_samples
            if no_enqueue_first_publish_slot_samples else None
        ),
        "no_enqueue_first_publish_slot_non_draw_commands_per_slot": (
            no_enqueue_first_publish_slot_non_draw_commands /
            no_enqueue_first_publish_slot_samples
            if no_enqueue_first_publish_slot_samples else None
        ),
        "no_enqueue_first_publish_slot_payload_bytes_per_slot": (
            no_enqueue_first_publish_slot_payload_bytes /
            no_enqueue_first_publish_slot_samples
            if no_enqueue_first_publish_slot_samples else None
        ),
        "no_enqueue_first_publish_slot_present_commands_per_slot": (
            no_enqueue_first_publish_slot_present_commands /
            no_enqueue_first_publish_slot_samples
            if no_enqueue_first_publish_slot_samples else None
        ),
        "no_enqueue_first_publish_slot_pre_present_commands_per_slot": (
            no_enqueue_first_publish_slot_pre_present_commands /
            no_enqueue_first_publish_slot_samples
            if no_enqueue_first_publish_slot_samples else None
        ),
        "no_enqueue_first_publish_slot_pre_present_draw_run_commands_per_slot": (
            no_enqueue_first_publish_slot_pre_present_draw_run_commands /
            no_enqueue_first_publish_slot_samples
            if no_enqueue_first_publish_slot_samples else None
        ),
        "no_enqueue_first_publish_slot_pre_present_draw_items_per_slot": (
            no_enqueue_first_publish_slot_pre_present_draw_items /
            no_enqueue_first_publish_slot_samples
            if no_enqueue_first_publish_slot_samples else None
        ),
        "no_enqueue_first_publish_slot_pre_present_non_draw_commands_per_slot": (
            no_enqueue_first_publish_slot_pre_present_non_draw_commands /
            no_enqueue_first_publish_slot_samples
            if no_enqueue_first_publish_slot_samples else None
        ),
        "no_enqueue_first_publish_slot_pre_present_payload_bytes_per_slot": (
            no_enqueue_first_publish_slot_pre_present_payload_bytes /
            no_enqueue_first_publish_slot_samples
            if no_enqueue_first_publish_slot_samples else None
        ),
        "no_enqueue_first_publish_slot_post_present_commands_per_slot": (
            no_enqueue_first_publish_slot_post_present_commands /
            no_enqueue_first_publish_slot_samples
            if no_enqueue_first_publish_slot_samples else None
        ),
        "no_enqueue_first_publish_slot_present_tail_share_pct": (
            no_enqueue_first_publish_slot_present_tail_slots /
            no_enqueue_first_publish_slot_samples * 100.0
            if no_enqueue_first_publish_slot_samples else None
        ),
        "no_enqueue_before_publish_replay_cpu_ms_per_present": (
            no_enqueue_before_publish_replay_cpu_ms / present if present else None
        ),
        "no_enqueue_before_publish_replay_gap_ms_per_present": (
            no_enqueue_before_publish_replay_gap_ms / present if present else None
        ),
        "no_enqueue_before_publish_closure_ms_per_present": (
            no_enqueue_before_publish_closure_ms / present if present else None
        ),
        "no_enqueue_before_publish_residual_ms_per_present": (
            no_enqueue_before_publish_residual_ms / present if present else None
        ),
        "no_enqueue_before_publish_completed_replay_share_pct": (
            no_enqueue_completed_replay_cpu_before_publish_ms /
            no_enqueue_commit_entry_to_publish_ms * 100.0
            if no_enqueue_commit_entry_to_publish_ms else None
        ),
        "no_enqueue_before_publish_active_replay_share_pct": (
            no_enqueue_active_replay_cpu_before_publish_ms /
            no_enqueue_commit_entry_to_publish_ms * 100.0
            if no_enqueue_commit_entry_to_publish_ms else None
        ),
        "no_enqueue_before_publish_inter_replay_gap_share_pct": (
            no_enqueue_inter_replay_gap_before_publish_ms /
            no_enqueue_commit_entry_to_publish_ms * 100.0
            if no_enqueue_commit_entry_to_publish_ms else None
        ),
        "no_enqueue_before_publish_commit_publish_wait_share_pct": (
            no_enqueue_commit_publish_wait_before_publish_ms /
            no_enqueue_commit_entry_to_publish_ms * 100.0
            if no_enqueue_commit_entry_to_publish_ms else None
        ),
        "no_enqueue_before_publish_replay_cpu_share_pct": (
            no_enqueue_before_publish_replay_cpu_ms /
            no_enqueue_commit_entry_to_publish_ms * 100.0
            if no_enqueue_commit_entry_to_publish_ms else None
        ),
        "no_enqueue_before_publish_replay_gap_share_pct": (
            no_enqueue_before_publish_replay_gap_ms /
            no_enqueue_commit_entry_to_publish_ms * 100.0
            if no_enqueue_commit_entry_to_publish_ms else None
        ),
        "no_enqueue_before_publish_closure_share_pct": (
            no_enqueue_before_publish_closure_ms /
            no_enqueue_commit_entry_to_publish_ms * 100.0
            if no_enqueue_commit_entry_to_publish_ms else None
        ),
        "no_enqueue_before_publish_residual_share_pct": (
            no_enqueue_before_publish_residual_ms /
            no_enqueue_commit_entry_to_publish_ms * 100.0
            if no_enqueue_commit_entry_to_publish_ms else None
        ),
        "no_enqueue_before_publish_entries_per_present": ratio(
            counters,
            "completion_no_enqueue_commit_chunk_entries_before_publish",
            "present_encoded",
        ),
        "no_enqueue_before_publish_replay_starts_per_present": ratio(
            counters,
            "completion_no_enqueue_commit_chunk_replay_starts_before_publish",
            "present_encoded",
        ),
        "no_enqueue_before_publish_replay_ends_per_present": ratio(
            counters,
            "completion_no_enqueue_commit_chunk_replay_ends_before_publish",
            "present_encoded",
        ),
        "no_enqueue_before_publish_scanned_chunks_per_present": ratio(
            counters,
            "completion_no_enqueue_commit_chunk_shape_samples_before_publish",
            "present_encoded",
        ),
        "no_enqueue_before_publish_records_per_present": ratio(
            counters,
            "completion_no_enqueue_commit_chunk_records_before_publish",
            "present_encoded",
        ),
        "no_enqueue_before_publish_records_per_scanned_chunk": ratio(
            counters,
            "completion_no_enqueue_commit_chunk_records_before_publish",
            "completion_no_enqueue_commit_chunk_shape_samples_before_publish",
        ),
        "no_enqueue_before_publish_chunks_with_draw_per_present": ratio(
            counters,
            "completion_no_enqueue_commit_chunk_chunks_with_draw_before_publish",
            "present_encoded",
        ),
        "no_enqueue_before_publish_chunks_with_present_per_present": ratio(
            counters,
            "completion_no_enqueue_commit_chunk_chunks_with_present_before_publish",
            "present_encoded",
        ),
        "no_enqueue_before_publish_chunks_state_const_only_per_present": ratio(
            counters,
            "completion_no_enqueue_commit_chunk_chunks_state_const_only_before_publish",
            "present_encoded",
        ),
        "no_enqueue_before_publish_chunks_no_draw_no_present_per_present": ratio(
            counters,
            "completion_no_enqueue_commit_chunk_chunks_no_draw_no_present_before_publish",
            "present_encoded",
        ),
        "no_enqueue_before_publish_draw_records_per_present": ratio(
            counters,
            "completion_no_enqueue_commit_chunk_draw_records_before_publish",
            "present_encoded",
        ),
        "no_enqueue_before_publish_const_records_per_present": ratio(
            counters,
            "completion_no_enqueue_commit_chunk_const_records_before_publish",
            "present_encoded",
        ),
        "no_enqueue_before_publish_apply_state_records_per_present": ratio(
            counters,
            "completion_no_enqueue_commit_chunk_apply_state_records_before_publish",
            "present_encoded",
        ),
        "no_enqueue_before_publish_clear_records_per_present": ratio(
            counters,
            "completion_no_enqueue_commit_chunk_clear_records_before_publish",
            "present_encoded",
        ),
        "no_enqueue_before_publish_present_records_per_present": ratio(
            counters,
            "completion_no_enqueue_commit_chunk_present_records_before_publish",
            "present_encoded",
        ),
        "no_enqueue_before_publish_surface_records_per_present": ratio(
            counters,
            "completion_no_enqueue_commit_chunk_surface_records_before_publish",
            "present_encoded",
        ),
        "no_enqueue_before_publish_query_records_per_present": ratio(
            counters,
            "completion_no_enqueue_commit_chunk_query_records_before_publish",
            "present_encoded",
        ),
        "no_enqueue_before_publish_other_records_per_present": ratio(
            counters,
            "completion_no_enqueue_commit_chunk_other_records_before_publish",
            "present_encoded",
        ),
        "snapshot_state_copy_cpu_ms_per_present": (
            snapshot_state_copy_cpu_ms / present if present else None
        ),
        "snapshot_state_materialized_per_present": (
            snapshot_state_materialized / present if present else None
        ),
        "snapshot_state_materialized_mib_per_present": (
            snapshot_state_materialized_bytes / mib / present if present else None
        ),
        "snapshot_state_elided_per_present": (
            snapshot_state_elided / present if present else None
        ),
        "snapshot_state_elided_mib_per_present": (
            snapshot_state_elided_bytes / mib / present if present else None
        ),
        "snapshot_state_elision_share_pct": (
            snapshot_state_elided / snapshot_state_total * 100.0
            if snapshot_state_total else None
        ),
        "submission_carrier_records_per_present": (
            submission_carrier_records / present if present else None
        ),
        "submission_carrier_bytes_per_present": (
            submission_carrier_bytes / present if present else None
        ),
        "submission_carrier_state_storage_bytes_per_present": (
            submission_carrier_state_storage_bytes / present if present else None
        ),
        "submission_carrier_uniform_storage_bytes_per_present": (
            submission_carrier_uniform_storage_bytes / present if present else None
        ),
        "submission_carrier_compact_uniform_storage_bytes_per_present": (
            submission_carrier_compact_uniform_storage_bytes / present
            if present else None
        ),
        "submission_carrier_unused_uniform_storage_records_per_present": (
            submission_carrier_unused_uniform_storage_records / present
            if present else None
        ),
        "submission_carrier_unused_uniform_storage_mib_per_present": (
            submission_carrier_unused_uniform_storage_bytes / mib / present
            if present else None
        ),
        "submission_carrier_bytes_per_record": (
            submission_carrier_bytes / submission_carrier_records
            if submission_carrier_records else None
        ),
        "submission_carrier_state_storage_bytes_per_record": (
            submission_carrier_state_storage_bytes / submission_carrier_records
            if submission_carrier_records else None
        ),
        "submission_carrier_uniform_storage_bytes_per_record": (
            submission_carrier_uniform_storage_bytes / submission_carrier_records
            if submission_carrier_records else None
        ),
        "submission_carrier_compact_uniform_storage_bytes_per_record": (
            submission_carrier_compact_uniform_storage_bytes /
            submission_carrier_records
            if submission_carrier_records else None
        ),
        "submission_carrier_unused_uniform_storage_bytes_per_record": (
            submission_carrier_unused_uniform_storage_bytes /
            submission_carrier_records
            if submission_carrier_records else None
        ),
        "submission_carrier_unused_uniform_storage_bytes_per_unused_record": (
            submission_carrier_unused_uniform_storage_bytes /
            submission_carrier_unused_uniform_storage_records
            if submission_carrier_unused_uniform_storage_records else None
        ),
        "submission_carrier_unused_uniform_storage_share_pct": (
            submission_carrier_unused_uniform_storage_bytes /
            submission_carrier_uniform_storage_bytes * 100.0
            if submission_carrier_uniform_storage_bytes else None
        ),
        "uniform_materialized_bytes_per_present": (
            uniform_materialized_bytes / present if present else None
        ),
        "uniform_compact_candidate_bytes_per_present": (
            uniform_compact_candidate_bytes / present if present else None
        ),
        "uniform_compact_saved_bytes_per_present": (
            uniform_compact_saved_bytes / present if present else None
        ),
        "uniform_compact_fixed_bytes_per_present": (
            uniform_compact_fixed_bytes / present if present else None
        ),
        "uniform_compact_vertex_bytes_per_present": (
            uniform_compact_vertex_bytes / present if present else None
        ),
        "uniform_compact_pixel_bytes_per_present": (
            uniform_compact_pixel_bytes / present if present else None
        ),
        "uniform_compact_candidate_share_of_materialized_bytes": (
            uniform_compact_candidate_bytes / uniform_materialized_bytes * 100.0
            if uniform_materialized_bytes else None
        ),
        "uniform_compact_fixed_share_of_candidate_bytes": (
            uniform_compact_fixed_bytes / uniform_compact_candidate_bytes * 100.0
            if uniform_compact_candidate_bytes else None
        ),
        "uniform_compact_vertex_share_of_candidate_bytes": (
            uniform_compact_vertex_bytes / uniform_compact_candidate_bytes * 100.0
            if uniform_compact_candidate_bytes else None
        ),
        "uniform_compact_pixel_share_of_candidate_bytes": (
            uniform_compact_pixel_bytes / uniform_compact_candidate_bytes * 100.0
            if uniform_compact_candidate_bytes else None
        ),
        "uniform_compact_saved_share_of_materialized_bytes": (
            uniform_compact_saved_bytes / uniform_materialized_bytes * 100.0
            if uniform_materialized_bytes else None
        ),
        "uniform_adjacent_same_fixed_payload_hash_share": (
            uniform_adjacent_same_fixed_payload_hash /
            uniform_adjacent_previous_payload * 100.0
            if uniform_adjacent_previous_payload else None
        ),
        "uniform_adjacent_same_fixed_and_shader_const_hashes_share": (
            uniform_adjacent_same_fixed_and_shader_const_hashes /
            uniform_adjacent_previous_payload * 100.0
            if uniform_adjacent_previous_payload else None
        ),
        "uniform_append_bytes_per_present": (
            uniform_append_bytes / present if present else None
        ),
        "uniform_fixed_append_bytes_per_present": (
            uniform_fixed_append_bytes / present if present else None
        ),
        "uniform_vertex_constants_append_bytes_per_present": (
            uniform_vertex_append_bytes / present if present else None
        ),
        "uniform_pixel_constants_append_bytes_per_present": (
            uniform_pixel_append_bytes / present if present else None
        ),
        "uniform_stage_constants_append_bytes_per_present": (
            uniform_stage_append_bytes / present if present else None
        ),
        "uniform_vertex_append_amplification_vs_compact_vertex": (
            uniform_vertex_append_bytes / uniform_compact_vertex_bytes
            if uniform_compact_vertex_bytes else None
        ),
        "uniform_pixel_append_amplification_vs_compact_pixel": (
            uniform_pixel_append_bytes / uniform_compact_pixel_bytes
            if uniform_compact_pixel_bytes else None
        ),
        "uniform_stage_append_amplification_vs_compact_stage": (
            uniform_stage_append_bytes / uniform_compact_stage_bytes
            if uniform_compact_stage_bytes else None
        ),
        "uniform_append_bytes_per_append": (
            uniform_append_bytes / uniform_appends if uniform_appends else None
        ),
        "uniform_payload_record_append_bytes_per_append": (
            uniform_payload_record_append_bytes / uniform_appends
            if uniform_appends else None
        ),
        "uniform_fixed_append_records_per_payload_append": (
            uniform_fixed_appends / uniform_appends if uniform_appends else None
        ),
        "uniform_vertex_constants_append_records_per_payload_append": (
            uniform_vertex_appends / uniform_appends if uniform_appends else None
        ),
        "uniform_pixel_constants_append_records_per_payload_append": (
            uniform_pixel_appends / uniform_appends if uniform_appends else None
        ),
        "uniform_backend_materialized_bytes_per_present": (
            uniform_backend_materialized_bytes / present if present else None
        ),
        "uniform_backend_materialize_cpu_ms_per_present": (
            uniform_backend_materialize_cpu_ms / present if present else None
        ),
        "uniform_backend_materialized_bytes_per_call": (
            uniform_backend_materialized_bytes / uniform_backend_materialized
            if uniform_backend_materialized else None
        ),
        "uniform_backend_materialize_fallbacks_per_present": (
            uniform_backend_materialize_fallbacks / present if present else None
        ),
        "uniform_append_records_per_materialized_snapshot": (
            uniform_appends / uniform_materialized if uniform_materialized else None
        ),
        "uniform_semantic_hash_misses_per_present": (
            uniform_semantic_hash_misses / present if present else None
        ),
        "uniform_semantic_hash_miss_bytes_per_present": (
            uniform_semantic_hash_miss_bytes / present if present else None
        ),
        "uniform_append_bytes_share_of_materialized_bytes": (
            uniform_append_bytes / uniform_materialized_bytes * 100.0
            if uniform_materialized_bytes else None
        ),
        "uniform_fixed_append_bytes_share_of_append_bytes": (
            uniform_fixed_append_bytes / uniform_append_bytes * 100.0
            if uniform_append_bytes else None
        ),
        "uniform_vertex_constants_append_bytes_share_of_append_bytes": (
            uniform_vertex_append_bytes / uniform_append_bytes * 100.0
            if uniform_append_bytes else None
        ),
        "uniform_pixel_constants_append_bytes_share_of_append_bytes": (
            uniform_pixel_append_bytes / uniform_append_bytes * 100.0
            if uniform_append_bytes else None
        ),
        "uniform_snapshot_elision_share": (
            uniform_elided / uniform_snapshot_total * 100.0
            if uniform_snapshot_total else None
        ),
        "snapshot_cache_uniform_build_cpu_ms_per_present": (
            snapshot_cache_uniform_build_cpu_ms / present if present else None
        ),
        "snapshot_cache_uniform_hash_cpu_ms_per_present": (
            snapshot_cache_uniform_hash_cpu_ms / present if present else None
        ),
        "snapshot_cache_batch_miss_uniform_build_cpu_ms_per_present": (
            batch_miss_uniform_build_cpu_ms / present if present else None
        ),
        "snapshot_cache_batch_miss_uniform_hash_cpu_ms_per_present": (
            batch_miss_uniform_hash_cpu_ms / present if present else None
        ),
        "snapshot_cache_batch_miss_vs_const_hash_cpu_ms_per_present": (
            batch_miss_vs_const_hash_cpu_ms / present if present else None
        ),
        "snapshot_cache_batch_miss_vs_const_hash_reuse_per_present": (
            batch_miss_vs_const_hash_reuse / present if present else None
        ),
        "snapshot_cache_batch_miss_vs_const_hash_build_per_present": (
            batch_miss_vs_const_hash_build / present if present else None
        ),
        "snapshot_cache_batch_miss_vs_const_hash_build_share_pct": (
            batch_miss_vs_const_hash_build / batch_miss_vs_const_hash_total * 100.0
            if batch_miss_vs_const_hash_total else None
        ),
        "snapshot_cache_batch_miss_vs_const_hash_memo_probe_per_present": (
            batch_miss_vs_const_hash_memo_probe / present if present else None
        ),
        "snapshot_cache_batch_miss_vs_const_hash_memo_hits_per_present": (
            batch_miss_vs_const_hash_memo_hits / present if present else None
        ),
        "snapshot_cache_batch_miss_vs_const_hash_memo_misses_per_present": (
            batch_miss_vs_const_hash_memo_misses / present if present else None
        ),
        "snapshot_cache_batch_miss_vs_const_hash_memo_stores_per_present": (
            batch_miss_vs_const_hash_memo_stores / present if present else None
        ),
        "snapshot_cache_batch_miss_vs_const_hash_memo_hit_share_pct": (
            batch_miss_vs_const_hash_memo_hits /
            batch_miss_vs_const_hash_memo_probe * 100.0
            if batch_miss_vs_const_hash_memo_probe else None
        ),
        "snapshot_cache_batch_miss_ps_const_hash_cpu_ms_per_present": (
            batch_miss_ps_const_hash_cpu_ms / present if present else None
        ),
        "snapshot_cache_batch_miss_ps_const_hash_reuse_per_present": (
            batch_miss_ps_const_hash_reuse / present if present else None
        ),
        "snapshot_cache_batch_miss_ps_const_hash_build_per_present": (
            batch_miss_ps_const_hash_build / present if present else None
        ),
        "snapshot_cache_batch_miss_ps_const_hash_build_share_pct": (
            batch_miss_ps_const_hash_build / batch_miss_ps_const_hash_total * 100.0
            if batch_miss_ps_const_hash_total else None
        ),
        "snapshot_cache_batch_miss_ps_const_hash_memo_probe_per_present": (
            batch_miss_ps_const_hash_memo_probe / present if present else None
        ),
        "snapshot_cache_batch_miss_ps_const_hash_memo_hits_per_present": (
            batch_miss_ps_const_hash_memo_hits / present if present else None
        ),
        "snapshot_cache_batch_miss_ps_const_hash_memo_misses_per_present": (
            batch_miss_ps_const_hash_memo_misses / present if present else None
        ),
        "snapshot_cache_batch_miss_ps_const_hash_memo_stores_per_present": (
            batch_miss_ps_const_hash_memo_stores / present if present else None
        ),
        "snapshot_cache_batch_miss_ps_const_hash_memo_hit_share_pct": (
            batch_miss_ps_const_hash_memo_hits /
            batch_miss_ps_const_hash_memo_probe * 100.0
            if batch_miss_ps_const_hash_memo_probe else None
        ),
        "snapshot_cache_batch_miss_nonconst_hash_cpu_ms_per_present": (
            batch_miss_nonconst_hash_cpu_ms / present if present else None
        ),
        "snapshot_uniform_copy_cpu_ms_per_present": (
            snapshot_uniform_copy_cpu_ms / present if present else None
        ),
        "submit_draw_run_batch_append_uniform_cpu_ms_per_present": (
            append_uniform_cpu_ms / present if present else None
        ),
        "draw_uniform_payload_lookup_cpu_ms_per_present": (
            uniform_lookup_cpu_ms / present if present else None
        ),
        "draw_uniform_payload_append_copy_cpu_ms_per_present": (
            uniform_append_copy_cpu_ms / present if present else None
        ),
        "submit_draw_run_batch_discarded_state_records_per_present": (
            discarded_state_records / present if present else None
        ),
        "submit_draw_run_batch_discarded_state_mib_per_present": (
            discarded_state_bytes / mib / present if present else None
        ),
        "submit_draw_run_batch_submission_adjacent_same_generation_lane_per_present": (
            adjacent_same_generation_lane / present if present else None
        ),
        "submit_draw_run_batch_compat_same_generation_lane_per_present": (
            compat_same_generation_lane / present if present else None
        ),
        "submit_draw_run_batch_compat_same_generation_lane_compatible_share_pct": (
            compat_same_generation_lane_compatible / compat_same_generation_lane * 100.0
            if compat_same_generation_lane else None
        ),
        "submit_draw_run_batch_compat_same_generation_lane_incompatible_per_present": (
            compat_same_generation_lane_incompatible / present if present else None
        ),
        "snapshot_cache_batch_miss_uniform_hash_share_pct": (
            batch_miss_uniform_hash_cpu_ms / batch_miss_uniform_build_cpu_ms * 100.0
            if batch_miss_uniform_build_cpu_ms else None
        ),
        "snapshot_cache_batch_miss_vs_const_hash_share_pct": (
            batch_miss_vs_const_hash_cpu_ms / batch_miss_uniform_hash_cpu_ms * 100.0
            if batch_miss_uniform_hash_cpu_ms else None
        ),
        "snapshot_cache_batch_miss_ps_const_hash_share_pct": (
            batch_miss_ps_const_hash_cpu_ms / batch_miss_uniform_hash_cpu_ms * 100.0
            if batch_miss_uniform_hash_cpu_ms else None
        ),
        "snapshot_cache_batch_miss_nonconst_hash_share_pct": (
            batch_miss_nonconst_hash_cpu_ms / batch_miss_uniform_hash_cpu_ms * 100.0
            if batch_miss_uniform_hash_cpu_ms else None
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

    def metric_number(key: str) -> float | None:
        return number(counter(counters, key))

    def metric_per_present(key: str) -> float | None:
        value = metric_number(key)
        return value / present if value is not None and present else None

    for key in PE_RECORDER_PER_PRESENT_COUNTERS:
        metrics[f"{key}_per_present"] = metric_per_present(key)

    pe_commit_count = metric_number("pe_recorder_commit_count") or 0.0
    pe_record_count = metric_number("pe_recorder_record_count_total") or 0.0
    pe_payload_bytes = metric_number("pe_recorder_payload_bytes_total") or 0.0
    pe_record_append_calls = metric_number("pe_recorder_record_append_calls") or 0.0
    pe_record_append_no_flush_calls = (
        metric_number("pe_recorder_record_append_no_flush_calls") or 0.0
    )
    pe_record_append_cpu_ms = metric_number(
        "pe_recorder_record_append_cpu_ms"
    ) or 0.0
    pe_record_append_no_flush_cpu_ms = metric_number(
        "pe_recorder_record_append_no_flush_cpu_ms"
    ) or 0.0
    pe_const_flush_cpu_ms = metric_number(
        "pe_recorder_const_flush_cpu_ms"
    ) or 0.0
    pe_vs_const_setter_cpu_ms = metric_number(
        "pe_recorder_vs_const_f_setter_cpu_ms"
    ) or 0.0
    pe_ps_const_setter_cpu_ms = metric_number(
        "pe_recorder_ps_const_f_setter_cpu_ms"
    ) or 0.0
    metrics["pe_recorder_records_per_commit"] = (
        pe_record_count / pe_commit_count if pe_commit_count else None
    )
    metrics["pe_recorder_payload_bytes_per_commit"] = (
        pe_payload_bytes / pe_commit_count if pe_commit_count else None
    )
    metrics["pe_recorder_payload_bytes_per_record"] = (
        pe_payload_bytes / pe_record_count if pe_record_count else None
    )
    metrics["pe_recorder_record_append_cpu_us_per_call"] = (
        pe_record_append_cpu_ms * 1000.0 / pe_record_append_calls
        if pe_record_append_calls else None
    )
    metrics["pe_recorder_record_append_no_flush_cpu_us_per_call"] = (
        pe_record_append_no_flush_cpu_ms * 1000.0 /
        pe_record_append_no_flush_calls
        if pe_record_append_no_flush_calls else None
    )
    metrics["pe_recorder_const_flush_share_of_record_append_cpu_pct"] = (
        pe_const_flush_cpu_ms / pe_record_append_cpu_ms * 100.0
        if pe_record_append_cpu_ms else None
    )
    metrics["pe_recorder_vs_const_setter_share_of_record_append_cpu_pct"] = (
        pe_vs_const_setter_cpu_ms / pe_record_append_cpu_ms * 100.0
        if pe_record_append_cpu_ms else None
    )
    metrics["pe_recorder_ps_const_setter_share_of_record_append_cpu_pct"] = (
        pe_ps_const_setter_cpu_ms / pe_record_append_cpu_ms * 100.0
        if pe_record_append_cpu_ms else None
    )

    for rank in range(1, 5):
        base = f"pe_recorder_inter_append_top{rank}"
        metrics[f"{base}_samples_per_present"] = metric_per_present(
            f"{base}_samples"
        )
        metrics[f"{base}_ms_per_present"] = metric_per_present(f"{base}_ms")

    focused_between_calls_ms = 0.0
    focused_between_call_body_cpu_ms = 0.0
    focused_between_call_gap_residual_ms = 0.0
    has_focused_aggregate_body = False
    for _, target_prefix in PE_RECORDER_FOCUSED_GAP_PREFIXES:
        base = f"pe_recorder_gap_{target_prefix}"
        for rank in (1, 2):
            metrics[f"{base}_top{rank}_samples_per_present"] = (
                metric_per_present(f"{base}_top{rank}_samples")
            )
            metrics[f"{base}_top{rank}_ms_per_present"] = metric_per_present(
                f"{base}_top{rank}_ms"
            )
            metrics[f"{base}_between_top{rank}_entries_per_present"] = (
                metric_per_present(f"{base}_between_top{rank}_samples")
            )
            metrics[f"{base}_between_top{rank}_call_name_entries_per_present"] = (
                metric_per_present(f"{base}_between_top{rank}_call_name_samples")
            )
            metrics[f"{base}_between_top{rank}_call_name_cpu_ms_per_present"] = (
                metric_per_present(f"{base}_between_top{rank}_call_name_cpu_ms")
            )
            metrics[f"{base}_between_gap_top{rank}_samples_per_present"] = (
                metric_per_present(f"{base}_between_gap_top{rank}_samples")
            )
            metrics[f"{base}_between_gap_top{rank}_ms_per_present"] = (
                metric_per_present(f"{base}_between_gap_top{rank}_ms")
            )
            metrics[f"{base}_between_gap_top{rank}_name_samples_per_present"] = (
                metric_per_present(f"{base}_between_gap_top{rank}_name_samples")
            )
            metrics[f"{base}_between_gap_top{rank}_name_ms_per_present"] = (
                metric_per_present(f"{base}_between_gap_top{rank}_name_ms")
            )
            metrics[f"{base}_between_gap_site_top{rank}_samples_per_present"] = (
                metric_per_present(f"{base}_between_gap_site_top{rank}_samples")
            )
            metrics[f"{base}_between_gap_site_top{rank}_ms_per_present"] = (
                metric_per_present(f"{base}_between_gap_site_top{rank}_ms")
            )
        pre_call_ms = metric_number(f"{base}_pre_call_ms") or 0.0
        inside_call_ms = metric_number(f"{base}_inside_call_ms") or 0.0
        prev_call_tail_ms = metric_number(f"{base}_prev_call_tail_ms") or 0.0
        between_calls_raw = metric_number(f"{base}_between_calls_ms")
        between_calls_ms = between_calls_raw or 0.0
        phase_ms = pre_call_ms + inside_call_ms
        tail_split_ms = prev_call_tail_ms + between_calls_ms
        metrics[f"{base}_phase_samples_per_present"] = metric_per_present(
            f"{base}_phase_samples"
        )
        metrics[f"{base}_pre_call_ms_per_present"] = metric_per_present(
            f"{base}_pre_call_ms"
        )
        metrics[f"{base}_inside_call_ms_per_present"] = metric_per_present(
            f"{base}_inside_call_ms"
        )
        metrics[f"{base}_pre_call_share_pct"] = (
            pre_call_ms / phase_ms * 100.0 if phase_ms else None
        )
        metrics[f"{base}_inside_call_share_pct"] = (
            inside_call_ms / phase_ms * 100.0 if phase_ms else None
        )
        metrics[f"{base}_tail_split_samples_per_present"] = metric_per_present(
            f"{base}_tail_split_samples"
        )
        metrics[f"{base}_prev_call_tail_ms_per_present"] = metric_per_present(
            f"{base}_prev_call_tail_ms"
        )
        metrics[f"{base}_between_calls_ms_per_present"] = metric_per_present(
            f"{base}_between_calls_ms"
        )
        metrics[f"{base}_between_call_body_calls_per_present"] = (
            metric_per_present(f"{base}_between_call_body_calls")
        )
        metrics[f"{base}_between_call_body_cpu_ms_per_present"] = (
            metric_per_present(f"{base}_between_call_body_cpu_ms")
        )
        metrics[f"{base}_prev_call_tail_share_pct"] = (
            prev_call_tail_ms / tail_split_ms * 100.0 if tail_split_ms else None
        )
        metrics[f"{base}_between_calls_share_pct"] = (
            between_calls_ms / tail_split_ms * 100.0 if tail_split_ms else None
        )
        body_cpu_values = [
            metric_number(f"{base}_between_top{rank}_call_name_cpu_ms")
            for rank in (1, 2)
        ]
        has_body_cpu = any(value is not None for value in body_cpu_values)
        body_cpu_ms = sum(value or 0.0 for value in body_cpu_values)
        metrics[f"{base}_between_top_call_name_cpu_ms_per_present"] = (
            body_cpu_ms / present if has_body_cpu and present else None
        )
        metrics[f"{base}_between_body_residual_ms_per_present"] = (
            (between_calls_ms - body_cpu_ms) / present
            if has_body_cpu and between_calls_raw is not None and present else None
        )
        metrics[f"{base}_between_body_coverage_pct"] = (
            body_cpu_ms / between_calls_ms * 100.0
            if has_body_cpu and between_calls_raw is not None and between_calls_ms else None
        )
        metrics[f"{base}_between_body_residual_share_pct"] = (
            (between_calls_ms - body_cpu_ms) / between_calls_ms * 100.0
            if has_body_cpu and between_calls_raw is not None and between_calls_ms else None
        )
        aggregate_body_raw = metric_number(f"{base}_between_call_body_cpu_ms")
        if aggregate_body_raw is not None and between_calls_raw is not None:
            gap_residual_ms = between_calls_ms - aggregate_body_raw
            metrics[f"{base}_between_call_body_coverage_pct"] = (
                aggregate_body_raw / between_calls_ms * 100.0
                if between_calls_ms else None
            )
            metrics[f"{base}_between_call_gap_residual_ms_per_present"] = (
                gap_residual_ms / present if present else None
            )
            metrics[f"{base}_between_call_gap_residual_share_pct"] = (
                gap_residual_ms / between_calls_ms * 100.0
                if between_calls_ms else None
            )
            focused_between_calls_ms += between_calls_ms
            focused_between_call_body_cpu_ms += aggregate_body_raw
            focused_between_call_gap_residual_ms += gap_residual_ms
            has_focused_aggregate_body = True

    metrics["pe_recorder_focused_between_calls_ms_per_present"] = (
        focused_between_calls_ms / present
        if has_focused_aggregate_body and present else None
    )
    metrics["pe_recorder_focused_between_call_body_cpu_ms_per_present"] = (
        focused_between_call_body_cpu_ms / present
        if has_focused_aggregate_body and present else None
    )
    metrics["pe_recorder_focused_between_call_body_coverage_pct"] = (
        focused_between_call_body_cpu_ms / focused_between_calls_ms * 100.0
        if has_focused_aggregate_body and focused_between_calls_ms else None
    )
    metrics["pe_recorder_focused_between_call_gap_residual_ms_per_present"] = (
        focused_between_call_gap_residual_ms / present
        if has_focused_aggregate_body and present else None
    )
    metrics["pe_recorder_focused_between_call_gap_residual_share_pct"] = (
        focused_between_call_gap_residual_ms / focused_between_calls_ms * 100.0
        if has_focused_aggregate_body and focused_between_calls_ms else None
    )

    for _, target_prefix in PE_RECORDER_HOT_SETTERS:
        base = f"pe_recorder_hot_setter_{target_prefix}"
        calls = metric_number(f"{base}_calls") or 0.0
        dirty = metric_number(f"{base}_dirty") or 0.0
        cpu_ms = metric_number(f"{base}_cpu_ms")
        metrics[f"{base}_calls_per_present"] = metric_per_present(f"{base}_calls")
        metrics[f"{base}_dirty_per_present"] = metric_per_present(f"{base}_dirty")
        metrics[f"{base}_cpu_ms_per_present"] = metric_per_present(f"{base}_cpu_ms")
        metrics[f"{base}_dirty_share_pct"] = (
            dirty / calls * 100.0 if calls else None
        )
        metrics[f"{base}_cpu_us_per_call"] = (
            cpu_ms * 1000.0 / calls if cpu_ms is not None and calls else None
        )

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
    for site_key in uniform_backend_materialize_sites:
        site_count = number(counter(
            counters,
            f"draw_uniform_payload_materialized_{site_key}",
        )) or 0.0
        site_bytes = number(counter(
            counters,
            f"draw_uniform_payload_materialized_{site_key}_bytes",
        )) or 0.0
        site_cpu_ms = number(counter(
            counters,
            f"draw_uniform_payload_materialize_{site_key}_cpu_ms",
        )) or 0.0
        metrics[f"uniform_backend_materialize_{site_key}_share_pct"] = (
            site_count / uniform_backend_materialized * 100.0
            if uniform_backend_materialized else None
        )
        metrics[f"uniform_backend_materialize_{site_key}_bytes_per_present"] = (
            site_bytes / present if present else None
        )
        metrics[f"uniform_backend_materialize_{site_key}_cpu_ms_per_present"] = (
            site_cpu_ms / present if present else None
        )
    return metrics


def fmt_derived(value: float | None) -> str:
    if value is None:
        return "n/a"
    if abs(value) >= 1000.0:
        return f"{value:,.3f}"
    return f"{value:.3f}"


def pe_recorder_pair_label(counters: dict[str, Any], rank: int) -> str:
    prev = counter(counters, f"pe_recorder_inter_append_top{rank}_prev")
    next_ = counter(counters, f"pe_recorder_inter_append_top{rank}_next")
    if not prev and not next_:
        return "n/a"
    return f"{prev or 'unknown'} -> {next_ or 'unknown'}"


def append_pe_recorder_sections(
    lines: list[str],
    before: dict[str, Any],
    after: dict[str, Any],
    before_derived: dict[str, float | None],
    after_derived: dict[str, float | None],
) -> None:
    has_before = any(
        counter(before, f"pe_recorder_inter_append_top{rank}_samples") is not None
        for rank in range(1, 5)
    )
    has_after = any(
        counter(after, f"pe_recorder_inter_append_top{rank}_samples") is not None
        for rank in range(1, 5)
    )
    if not has_before and not has_after:
        return

    lines.append("## PE Recorder Top Inter-Append Pairs")
    lines.append("")
    lines.append("| Rank | Before pair | Before ms/present | Before samples/present | After pair | After ms/present | After samples/present | Delta ms/present | Delta % |")
    lines.append("|---:|---|---:|---:|---|---:|---:|---:|---:|")
    for rank in range(1, 5):
        ms_key = f"pe_recorder_inter_append_top{rank}_ms_per_present"
        samples_key = f"pe_recorder_inter_append_top{rank}_samples_per_present"
        before_ms = before_derived.get(ms_key)
        after_ms = after_derived.get(ms_key)
        diff, pct = delta(after_ms, before_ms)
        lines.append(
            f"| `{rank}` | `{pe_recorder_pair_label(before, rank)}` | "
            f"`{fmt_derived(before_ms)}` | "
            f"`{fmt_derived(before_derived.get(samples_key))}` | "
            f"`{pe_recorder_pair_label(after, rank)}` | "
            f"`{fmt_derived(after_ms)}` | "
            f"`{fmt_derived(after_derived.get(samples_key))}` | "
            f"`{diff}` | `{pct}` |"
        )
    lines.append("")

    lines.append("### Focused Gap Phase Split")
    lines.append("")
    lines.append("| Pair | Before pre-call ms/present | Before inside-call ms/present | After pre-call ms/present | After inside-call ms/present | Delta pre-call | Delta inside-call |")
    lines.append("|---|---:|---:|---:|---:|---:|---:|")
    for _, target_prefix in PE_RECORDER_FOCUSED_GAP_PREFIXES:
        base = f"pe_recorder_gap_{target_prefix}"
        before_pre = before_derived.get(f"{base}_pre_call_ms_per_present")
        before_inside = before_derived.get(f"{base}_inside_call_ms_per_present")
        after_pre = after_derived.get(f"{base}_pre_call_ms_per_present")
        after_inside = after_derived.get(f"{base}_inside_call_ms_per_present")
        pre_diff, _ = delta(after_pre, before_pre)
        inside_diff, _ = delta(after_inside, before_inside)
        lines.append(
            f"| `{target_prefix}` | `{fmt_derived(before_pre)}` | "
            f"`{fmt_derived(before_inside)}` | `{fmt_derived(after_pre)}` | "
            f"`{fmt_derived(after_inside)}` | `{pre_diff}` | `{inside_diff}` |"
        )
    lines.append("")

    lines.append("### Focused Between-Calls Entry Names")
    lines.append("")
    lines.append("| Pair | Rank | Before call name | Before entries/present | Before CPU ms/present | After call name | After entries/present | After CPU ms/present | Delta entries/present | Delta CPU ms/present |")
    lines.append("|---|---:|---|---:|---:|---|---:|---:|---:|---:|")
    for _, target_prefix in PE_RECORDER_FOCUSED_GAP_PREFIXES:
        base = f"pe_recorder_gap_{target_prefix}"
        for rank in (1, 2):
            before_key = f"{base}_between_top{rank}_call_name"
            after_key = f"{base}_between_top{rank}_call_name"
            metric_key = f"{base}_between_top{rank}_call_name_entries_per_present"
            cpu_metric_key = f"{base}_between_top{rank}_call_name_cpu_ms_per_present"
            before_value = before_derived.get(metric_key)
            after_value = after_derived.get(metric_key)
            before_cpu = before_derived.get(cpu_metric_key)
            after_cpu = after_derived.get(cpu_metric_key)
            if (
                before_value is None
                and after_value is None
                and before_cpu is None
                and after_cpu is None
            ):
                continue
            diff, _ = delta(after_value, before_value)
            cpu_diff, _ = delta(after_cpu, before_cpu)
            lines.append(
                f"| `{target_prefix}` | `{rank}` | "
                f"`{counter(before, before_key) or 'n/a'}` | "
                f"`{fmt_derived(before_value)}` | "
                f"`{fmt_derived(before_cpu)}` | "
                f"`{counter(after, after_key) or 'n/a'}` | "
                f"`{fmt_derived(after_value)}` | "
                f"`{fmt_derived(after_cpu)}` | `{diff}` | `{cpu_diff}` |"
            )
    lines.append("")

    lines.append("### Focused Between-Calls Return-To-Entry Gaps")
    lines.append("")
    lines.append("| Pair | Rank | Before transition | Before ms/present | Before samples/present | After transition | After ms/present | After samples/present | Delta ms/present |")
    lines.append("|---|---:|---|---:|---:|---|---:|---:|---:|")
    for _, target_prefix in PE_RECORDER_FOCUSED_GAP_PREFIXES:
        base = f"pe_recorder_gap_{target_prefix}"
        for rank in (1, 2):
            before_prev_key = f"{base}_between_gap_top{rank}_prev_family"
            before_next_key = f"{base}_between_gap_top{rank}_next_family"
            after_prev_key = f"{base}_between_gap_top{rank}_prev_family"
            after_next_key = f"{base}_between_gap_top{rank}_next_family"
            metric_key = f"{base}_between_gap_top{rank}_ms_per_present"
            samples_key = f"{base}_between_gap_top{rank}_samples_per_present"
            before_value = before_derived.get(metric_key)
            after_value = after_derived.get(metric_key)
            before_samples = before_derived.get(samples_key)
            after_samples = after_derived.get(samples_key)
            if (
                before_value is None
                and after_value is None
                and before_samples is None
                and after_samples is None
            ):
                continue
            before_transition = (
                f"{counter(before, before_prev_key) or 'n/a'} -> "
                f"{counter(before, before_next_key) or 'n/a'}"
            )
            after_transition = (
                f"{counter(after, after_prev_key) or 'n/a'} -> "
                f"{counter(after, after_next_key) or 'n/a'}"
            )
            diff, _ = delta(after_value, before_value)
            lines.append(
                f"| `{target_prefix}` | `{rank}` | "
                f"`{before_transition}` | `{fmt_derived(before_value)}` | "
                f"`{fmt_derived(before_samples)}` | `{after_transition}` | "
                f"`{fmt_derived(after_value)}` | "
                f"`{fmt_derived(after_samples)}` | `{diff}` |"
            )
    lines.append("")

    lines.append("### Focused Between-Calls Exact Return-To-Entry Gaps")
    lines.append("")
    lines.append("| Pair | Rank | Before transition | Before ms/present | Before samples/present | After transition | After ms/present | After samples/present | Delta ms/present |")
    lines.append("|---|---:|---|---:|---:|---|---:|---:|---:|")
    for _, target_prefix in PE_RECORDER_FOCUSED_GAP_PREFIXES:
        base = f"pe_recorder_gap_{target_prefix}"
        for rank in (1, 2):
            before_prev_key = f"{base}_between_gap_top{rank}_prev_call_name"
            before_next_key = f"{base}_between_gap_top{rank}_next_call_name"
            after_prev_key = f"{base}_between_gap_top{rank}_prev_call_name"
            after_next_key = f"{base}_between_gap_top{rank}_next_call_name"
            metric_key = f"{base}_between_gap_top{rank}_name_ms_per_present"
            samples_key = f"{base}_between_gap_top{rank}_name_samples_per_present"
            before_value = before_derived.get(metric_key)
            after_value = after_derived.get(metric_key)
            before_samples = before_derived.get(samples_key)
            after_samples = after_derived.get(samples_key)
            if (
                before_value is None
                and after_value is None
                and before_samples is None
                and after_samples is None
            ):
                continue
            before_transition = (
                f"{counter(before, before_prev_key) or 'n/a'} -> "
                f"{counter(before, before_next_key) or 'n/a'}"
            )
            after_transition = (
                f"{counter(after, after_prev_key) or 'n/a'} -> "
                f"{counter(after, after_next_key) or 'n/a'}"
            )
            diff, _ = delta(after_value, before_value)
            lines.append(
                f"| `{target_prefix}` | `{rank}` | "
                f"`{before_transition}` | `{fmt_derived(before_value)}` | "
                f"`{fmt_derived(before_samples)}` | `{after_transition}` | "
                f"`{fmt_derived(after_value)}` | "
                f"`{fmt_derived(after_samples)}` | `{diff}` |"
            )
    lines.append("")

    lines.append("### Focused Between-Calls Exact Return-To-Entry Call Sites")
    lines.append("")
    lines.append("| Pair | Rank | Before transition | Before caller | Before ms/present | Before samples/present | After transition | After caller | After ms/present | After samples/present | Delta ms/present |")
    lines.append("|---|---:|---|---|---:|---:|---|---|---:|---:|---:|")
    for _, target_prefix in PE_RECORDER_FOCUSED_GAP_PREFIXES:
        base = f"pe_recorder_gap_{target_prefix}"
        for rank in (1, 2):
            before_prev_key = (
                f"{base}_between_gap_site_top{rank}_prev_call_name")
            before_next_key = (
                f"{base}_between_gap_site_top{rank}_next_call_name")
            before_module_key = (
                f"{base}_between_gap_site_top{rank}_caller_module")
            before_rva_key = f"{base}_between_gap_site_top{rank}_caller_rva"
            after_prev_key = f"{base}_between_gap_site_top{rank}_prev_call_name"
            after_next_key = f"{base}_between_gap_site_top{rank}_next_call_name"
            after_module_key = f"{base}_between_gap_site_top{rank}_caller_module"
            after_rva_key = f"{base}_between_gap_site_top{rank}_caller_rva"
            metric_key = f"{base}_between_gap_site_top{rank}_ms_per_present"
            samples_key = (
                f"{base}_between_gap_site_top{rank}_samples_per_present")
            before_value = before_derived.get(metric_key)
            after_value = after_derived.get(metric_key)
            before_samples = before_derived.get(samples_key)
            after_samples = after_derived.get(samples_key)
            if (
                before_value is None
                and after_value is None
                and before_samples is None
                and after_samples is None
            ):
                continue
            before_transition = (
                f"{counter(before, before_prev_key) or 'n/a'} -> "
                f"{counter(before, before_next_key) or 'n/a'}"
            )
            after_transition = (
                f"{counter(after, after_prev_key) or 'n/a'} -> "
                f"{counter(after, after_next_key) or 'n/a'}"
            )
            before_caller = (
                f"{counter(before, before_module_key) or 'n/a'}+"
                f"{counter(before, before_rva_key) or '0x0'}"
            )
            after_caller = (
                f"{counter(after, after_module_key) or 'n/a'}+"
                f"{counter(after, after_rva_key) or '0x0'}"
            )
            diff, _ = delta(after_value, before_value)
            lines.append(
                f"| `{target_prefix}` | `{rank}` | "
                f"`{before_transition}` | `{before_caller}` | "
                f"`{fmt_derived(before_value)}` | "
                f"`{fmt_derived(before_samples)}` | `{after_transition}` | "
                f"`{after_caller}` | `{fmt_derived(after_value)}` | "
                f"`{fmt_derived(after_samples)}` | `{diff}` |"
            )
    lines.append("")

    lines.append("### Focused Between-Calls Body Coverage")
    lines.append("")
    lines.append("| Pair | Before between ms/present | Before top body CPU ms/present | Before all body CPU ms/present | Before all body coverage | Before call-gap residual ms/present | After between ms/present | After top body CPU ms/present | After all body CPU ms/present | After all body coverage | After call-gap residual ms/present | Delta call-gap residual |")
    lines.append("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
    for _, target_prefix in PE_RECORDER_FOCUSED_GAP_PREFIXES:
        base = f"pe_recorder_gap_{target_prefix}"
        before_between = before_derived.get(f"{base}_between_calls_ms_per_present")
        before_top_body = before_derived.get(
            f"{base}_between_top_call_name_cpu_ms_per_present")
        before_all_body = before_derived.get(
            f"{base}_between_call_body_cpu_ms_per_present")
        before_all_coverage = before_derived.get(
            f"{base}_between_call_body_coverage_pct")
        before_call_gap_residual = before_derived.get(
            f"{base}_between_call_gap_residual_ms_per_present")
        before_residual = before_call_gap_residual
        if before_residual is None:
            before_residual = before_derived.get(
                f"{base}_between_body_residual_ms_per_present")
        after_between = after_derived.get(f"{base}_between_calls_ms_per_present")
        after_top_body = after_derived.get(
            f"{base}_between_top_call_name_cpu_ms_per_present")
        after_all_body = after_derived.get(
            f"{base}_between_call_body_cpu_ms_per_present")
        after_all_coverage = after_derived.get(
            f"{base}_between_call_body_coverage_pct")
        after_call_gap_residual = after_derived.get(
            f"{base}_between_call_gap_residual_ms_per_present")
        after_residual = after_call_gap_residual
        if after_residual is None:
            after_residual = after_derived.get(
                f"{base}_between_body_residual_ms_per_present")
        if (
            before_between is None
            and before_top_body is None
            and before_all_body is None
            and before_residual is None
            and after_between is None
            and after_top_body is None
            and after_all_body is None
            and after_residual is None
        ):
            continue
        residual_diff, _ = delta(after_residual, before_residual)
        lines.append(
            f"| `{target_prefix}` | `{fmt_derived(before_between)}` | "
            f"`{fmt_derived(before_top_body)}` | "
            f"`{fmt_derived(before_all_body)}` | "
            f"`{fmt_derived(before_all_coverage)}` | "
            f"`{fmt_derived(before_residual)}` | "
            f"`{fmt_derived(after_between)}` | "
            f"`{fmt_derived(after_top_body)}` | "
            f"`{fmt_derived(after_all_body)}` | "
            f"`{fmt_derived(after_all_coverage)}` | "
            f"`{fmt_derived(after_residual)}` | `{residual_diff}` |"
        )
    lines.append("")


def write_report(
    output: Path,
    before_path: Path,
    after_path: Path,
    before_label: str,
    after_label: str,
    before: dict[str, Any],
    after: dict[str, Any],
) -> None:
    keys = tuple(dict.fromkeys((
        *FOCUS_COUNTERS,
        *RUN_COUNTERS,
        *EXTRA_COUNTERS,
        *PE_RECORDER_NUMERIC_FOCUS_COUNTERS,
    )))
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
    present_wait_delta, present_wait_pct = delta(
        after_derived["completion_present_wait_ms_per_present"],
        before_derived["completion_present_wait_ms_per_present"],
    )
    overlap_delta, overlap_pct = delta(
        after_derived["completion_wait_with_enqueue_ms_per_present"],
        before_derived["completion_wait_with_enqueue_ms_per_present"],
    )
    no_enqueue_delta, no_enqueue_pct = delta(
        after_derived["completion_wait_without_enqueue_ms_per_present"],
        before_derived["completion_wait_without_enqueue_ms_per_present"],
    )
    lines.append(
        f"- Present completion wait changed by `{present_wait_delta} ms/present` "
        f"(`{present_wait_pct}`)."
    )
    lines.append(
        f"- Completion overlap wait changed by `{overlap_delta} ms/present` "
        f"(`{overlap_pct}`)."
    )
    lines.append(
        f"- Completion no-enqueue wait changed by `{no_enqueue_delta} ms/present` "
        f"(`{no_enqueue_pct}`)."
    )
    replay_delta, replay_pct = delta(
        after_derived["commit_chunk_replay_cpu_ms_per_present"],
        before_derived["commit_chunk_replay_cpu_ms_per_present"],
    )
    encode_delta, encode_pct = delta(
        after_derived["encode_chunk_cpu_ms_per_present"],
        before_derived["encode_chunk_cpu_ms_per_present"],
    )
    stage_delta, stage_pct = delta(
        after_derived["no_enqueue_stage_encode_dequeue_to_command_buffer_commit_ms_per_present"],
        before_derived["no_enqueue_stage_encode_dequeue_to_command_buffer_commit_ms_per_present"],
    )
    lines.append(
        f"- Commit replay CPU changed by `{replay_delta} ms/present` "
        f"(`{replay_pct}`)."
    )
    lines.append(
        f"- Encode chunk CPU changed by `{encode_delta} ms/present` "
        f"(`{encode_pct}`)."
    )
    lines.append(
        f"- No-enqueue encode→commit stage changed by `{stage_delta} ms/present` "
        f"(`{stage_pct}`)."
    )
    uniform_build_delta, uniform_build_pct = delta(
        after_derived["snapshot_cache_uniform_build_cpu_ms_per_present"],
        before_derived["snapshot_cache_uniform_build_cpu_ms_per_present"],
    )
    uniform_hash_delta, uniform_hash_pct = delta(
        after_derived["snapshot_cache_uniform_hash_cpu_ms_per_present"],
        before_derived["snapshot_cache_uniform_hash_cpu_ms_per_present"],
    )
    uniform_append_delta, uniform_append_pct = delta(
        after_derived["submit_draw_run_batch_append_uniform_cpu_ms_per_present"],
        before_derived["submit_draw_run_batch_append_uniform_cpu_ms_per_present"],
    )
    lines.append(
        f"- Snapshot uniform build CPU changed by `{uniform_build_delta} ms/present` "
        f"(`{uniform_build_pct}`)."
    )
    lines.append(
        f"- Snapshot uniform hash CPU changed by `{uniform_hash_delta} ms/present` "
        f"(`{uniform_hash_pct}`)."
    )
    lines.append(
        f"- Backend uniform append CPU changed by `{uniform_append_delta} ms/present` "
        f"(`{uniform_append_pct}`)."
    )
    state_copy_delta, state_copy_pct = delta(
        after_derived["snapshot_state_copy_cpu_ms_per_present"],
        before_derived["snapshot_state_copy_cpu_ms_per_present"],
    )
    state_elided_delta, state_elided_pct = delta(
        after_derived["snapshot_state_elided_mib_per_present"],
        before_derived["snapshot_state_elided_mib_per_present"],
    )
    discarded_state_delta, discarded_state_pct = delta(
        after_derived["submit_draw_run_batch_discarded_state_mib_per_present"],
        before_derived["submit_draw_run_batch_discarded_state_mib_per_present"],
    )
    carrier_width_delta, carrier_width_pct = delta(
        after_derived["submission_carrier_bytes_per_record"],
        before_derived["submission_carrier_bytes_per_record"],
    )
    carrier_uniform_delta, carrier_uniform_pct = delta(
        after_derived["submission_carrier_uniform_storage_bytes_per_record"],
        before_derived["submission_carrier_uniform_storage_bytes_per_record"],
    )
    carrier_unused_uniform_delta, carrier_unused_uniform_pct = delta(
        after_derived[
            "submission_carrier_unused_uniform_storage_mib_per_present"
        ],
        before_derived[
            "submission_carrier_unused_uniform_storage_mib_per_present"
        ],
    )
    lines.append(
        f"- Snapshot state-copy CPU changed by `{state_copy_delta} ms/present` "
        f"(`{state_copy_pct}`)."
    )
    lines.append(
        f"- Snapshot state elided bytes changed by `{state_elided_delta} MiB/present` "
        f"(`{state_elided_pct}`)."
    )
    lines.append(
        f"- Discarded backend state changed by `{discarded_state_delta} MiB/present` "
        f"(`{discarded_state_pct}`)."
    )
    lines.append(
        f"- Submission carrier width changed by `{carrier_width_delta} bytes/record` "
        f"(`{carrier_width_pct}`)."
    )
    lines.append(
        f"- Submission carrier full-uniform storage changed by "
        f"`{carrier_uniform_delta} bytes/record` (`{carrier_uniform_pct}`)."
    )
    lines.append(
        f"- Submission carrier unused full-uniform storage changed by "
        f"`{carrier_unused_uniform_delta} MiB/present` "
        f"(`{carrier_unused_uniform_pct}`)."
    )
    compact_saved_delta, compact_saved_pct = delta(
        after_derived["uniform_compact_saved_bytes_per_present"],
        before_derived["uniform_compact_saved_bytes_per_present"],
    )
    lines.append(
        f"- Uniform compact-copy saved-byte opportunity changed by "
        f"`{compact_saved_delta} bytes/present` (`{compact_saved_pct}`)."
    )
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

    append_pe_recorder_sections(lines, before, after, before_derived, after_derived)

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

    def require_derived_decrease(key: str, label: str) -> None:
        before_value = before_derived[key] or 0.0
        after_value = after_derived[key] or 0.0
        if after_value >= before_value:
            failures.append(
                f"{label} did not decrease "
                f"({fmt_derived(before_value)} -> {fmt_derived(after_value)})"
            )

    def require_derived_not_increase(key: str, label: str) -> None:
        before_value = before_derived[key] or 0.0
        after_value = after_derived[key] or 0.0
        if after_value > before_value:
            failures.append(
                f"{label} increased "
                f"({fmt_derived(before_value)} -> {fmt_derived(after_value)})"
            )

    def require_available_derived_not_increase(key: str, label: str) -> None:
        before_value = before_derived[key]
        after_value = after_derived[key]
        if before_value is None or after_value is None:
            failures.append(
                f"{label} is missing "
                f"({fmt_derived(before_value)} -> {fmt_derived(after_value)})"
            )
            return
        if after_value > before_value:
            failures.append(
                f"{label} increased "
                f"({fmt_derived(before_value)} -> {fmt_derived(after_value)})"
            )

    def require_available_derived_decrease(key: str, label: str) -> None:
        before_value = before_derived[key]
        after_value = after_derived[key]
        if before_value is None or after_value is None:
            failures.append(
                f"{label} is missing "
                f"({fmt_derived(before_value)} -> {fmt_derived(after_value)})"
            )
            return
        if after_value >= before_value:
            failures.append(
                f"{label} did not decrease "
                f"({fmt_derived(before_value)} -> {fmt_derived(after_value)})"
            )

    def require_available_derived_increase(key: str, label: str) -> None:
        before_value = before_derived[key]
        after_value = after_derived[key]
        if before_value is None or after_value is None:
            failures.append(
                f"{label} is missing "
                f"({fmt_derived(before_value)} -> {fmt_derived(after_value)})"
            )
            return
        if after_value <= before_value:
            failures.append(
                f"{label} did not increase "
                f"({fmt_derived(before_value)} -> {fmt_derived(after_value)})"
            )

    def require_after_counter_zero(key: str, label: str) -> None:
        after_value = number(counter(after, key))
        if after_value is None:
            failures.append(f"{label} is missing")
            return
        if after_value != 0.0:
            failures.append(f"{label} is nonzero ({fmt_value(after_value)})")

    def require_render_pass_carry_promotion_gates() -> None:
        with_enqueue_before = before_derived[
            "completion_wait_with_enqueue_ms_per_present"
        ]
        with_enqueue_after = after_derived[
            "completion_wait_with_enqueue_ms_per_present"
        ]
        without_enqueue_before = before_derived[
            "completion_wait_without_enqueue_ms_per_present"
        ]
        without_enqueue_after = after_derived[
            "completion_wait_without_enqueue_ms_per_present"
        ]
        with_enqueue_ok = (
            with_enqueue_before is not None and
            with_enqueue_after is not None and
            with_enqueue_after > with_enqueue_before
        )
        without_enqueue_ok = (
            without_enqueue_before is not None and
            without_enqueue_after is not None and
            without_enqueue_after < without_enqueue_before
        )
        if not (with_enqueue_ok or without_enqueue_ok):
            failures.append(
                "render-pass-carry P4 gate did not move: "
                "completion_wait_with_enqueue_ms_per_present did not increase "
                f"({fmt_derived(with_enqueue_before)} -> "
                f"{fmt_derived(with_enqueue_after)}) and "
                "completion_wait_without_enqueue_ms_per_present did not decrease "
                f"({fmt_derived(without_enqueue_before)} -> "
                f"{fmt_derived(without_enqueue_after)})"
            )

        require_available_derived_increase(
            "encode_ready_depth_gt1_per_present",
            "encode_ready_depth_gt1_per_present",
        )
        require_derived_not_increase(
            "command_buffers_per_present",
            "command_buffers_per_present",
        )
        require_derived_not_increase(
            "passes_per_present",
            "passes_per_present",
        )
        require_available_derived_not_increase(
            "encoder_sidecar_rows_per_present",
            "encoder_sidecar_rows_per_present",
        )
        require_available_derived_not_increase(
            "tile_preservation_mib_per_present",
            "tile_preservation_mib_per_present",
        )
        require_available_derived_not_increase(
            "gpu_command_buffer_time_ms_per_present",
            "gpu_command_buffer_time_ms_per_present",
        )
        require_available_derived_not_increase(
            "encoder_sidecar_final_end_reason_per_present",
            "encoder_sidecar_final_end_reason_per_present",
        )
        require_available_derived_not_increase(
            "encoder_sidecar_final_same_key_reopen_per_present",
            "encoder_sidecar_final_same_key_reopen_per_present",
        )
        require_available_derived_not_increase(
            "encoder_sidecar_color_load_mib_per_present",
            "encoder_sidecar_color_load_mib_per_present",
        )
        require_available_derived_not_increase(
            "encoder_sidecar_depth_load_mib_per_present",
            "encoder_sidecar_depth_load_mib_per_present",
        )
        require_after_counter_zero(
            "gpu_command_buffer_errors",
            "gpu_command_buffer_errors",
        )
        require_after_counter_zero(
            "draw_skipped_no_pipeline",
            "draw_skipped_no_pipeline",
        )

    if args.require_render_pass_carry_promotion_gates:
        require_render_pass_carry_promotion_gates()

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

    if args.require_tile_preservation_not_increase:
        require_derived_not_increase(
            "tile_preservation_mib",
            "tile_preservation_mib",
        )

    if args.require_command_buffers_per_present_not_increase:
        require_derived_not_increase(
            "command_buffers_per_present",
            "command_buffers_per_present",
        )

    if args.require_render_passes_per_present_not_increase:
        require_derived_not_increase(
            "passes_per_present",
            "passes_per_present",
        )

    if args.require_encoder_final_end_reason_not_increase:
        require_available_derived_not_increase(
            "encoder_sidecar_final_end_reason_per_present",
            "encoder_sidecar_final_end_reason_per_present",
        )

    if args.require_encoder_final_same_key_reopen_not_increase:
        require_available_derived_not_increase(
            "encoder_sidecar_final_same_key_reopen_per_present",
            "encoder_sidecar_final_same_key_reopen_per_present",
        )

    if args.require_encoder_color_load_not_increase:
        require_available_derived_not_increase(
            "encoder_sidecar_color_load_mib_per_present",
            "encoder_sidecar_color_load_mib_per_present",
        )

    if args.require_encoder_depth_load_not_increase:
        require_available_derived_not_increase(
            "encoder_sidecar_depth_load_mib_per_present",
            "encoder_sidecar_depth_load_mib_per_present",
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

    if args.require_uniform_materialized_bytes_decrease:
        before_value = metric_value(before, "d3d9_snapshot_uniform_materialized_bytes")
        after_value = metric_value(after, "d3d9_snapshot_uniform_materialized_bytes")
        if after_value >= before_value:
            failures.append(
                "d3d9_snapshot_uniform_materialized_bytes did not decrease "
                f"({fmt_value(before_value)} -> {fmt_value(after_value)})"
            )

    if args.require_uniform_append_bytes_decrease:
        before_value = metric_value(before, "draw_uniform_payload_append_bytes")
        after_value = metric_value(after, "draw_uniform_payload_append_bytes")
        if after_value >= before_value:
            failures.append(
                "draw_uniform_payload_append_bytes did not decrease "
                f"({fmt_value(before_value)} -> {fmt_value(after_value)})"
            )

    if args.require_uniform_compact_saved_bytes_present:
        after_value = after_derived.get("uniform_compact_saved_bytes_per_present")
        if after_value is None or after_value <= 0.0:
            failures.append(
                "uniform_compact_saved_bytes_per_present stayed zero "
                f"({fmt_value(after_value)})"
            )

    if args.require_snapshot_state_elided_present:
        after_value = after_derived.get("snapshot_state_elided_per_present")
        if after_value is None or after_value <= 0.0:
            failures.append(
                "snapshot_state_elided_per_present stayed zero "
                f"({fmt_value(after_value)})"
            )

    if args.require_discarded_state_not_increase:
        require_derived_not_increase(
            "submit_draw_run_batch_discarded_state_records_per_present",
            "submit_draw_run_batch_discarded_state_records_per_present",
        )
        require_derived_not_increase(
            "submit_draw_run_batch_discarded_state_mib_per_present",
            "submit_draw_run_batch_discarded_state_mib_per_present",
        )

    if args.require_submission_carrier_bytes_per_record_decrease:
        require_available_derived_decrease(
            "submission_carrier_bytes_per_record",
            "submission_carrier_bytes_per_record",
        )

    if args.require_submission_carrier_uniform_storage_per_record_decrease:
        require_available_derived_decrease(
            "submission_carrier_uniform_storage_bytes_per_record",
            "submission_carrier_uniform_storage_bytes_per_record",
        )

    if args.require_completion_present_wait_decrease:
        before_value = metric_value(before, "completion_present_wait_ms")
        after_value = metric_value(after, "completion_present_wait_ms")
        if after_value >= before_value:
            failures.append(
                "completion_present_wait_ms did not decrease "
                f"({fmt_value(before_value)} -> {fmt_value(after_value)})"
            )

    if args.require_completion_wait_with_enqueue_increase:
        before_value = metric_value(before, "completion_wait_with_enqueue_ms")
        after_value = metric_value(after, "completion_wait_with_enqueue_ms")
        if after_value <= before_value:
            failures.append(
                "completion_wait_with_enqueue_ms did not increase "
                f"({fmt_value(before_value)} -> {fmt_value(after_value)})"
            )

    if args.require_completion_wait_without_enqueue_decrease:
        before_value = metric_value(before, "completion_wait_without_enqueue_ms")
        after_value = metric_value(after, "completion_wait_without_enqueue_ms")
        if after_value >= before_value:
            failures.append(
                "completion_wait_without_enqueue_ms did not decrease "
                f"({fmt_value(before_value)} -> {fmt_value(after_value)})"
            )

    if args.require_completion_present_wait_with_enqueue_increase:
        before_value = metric_value(before, "completion_present_wait_with_enqueue_ms")
        after_value = metric_value(after, "completion_present_wait_with_enqueue_ms")
        if after_value <= before_value:
            failures.append(
                "completion_present_wait_with_enqueue_ms did not increase "
                f"({fmt_value(before_value)} -> {fmt_value(after_value)})"
            )

    if args.require_completion_present_wait_without_enqueue_decrease:
        before_value = metric_value(before, "completion_present_wait_without_enqueue_ms")
        after_value = metric_value(after, "completion_present_wait_without_enqueue_ms")
        if after_value >= before_value:
            failures.append(
                "completion_present_wait_without_enqueue_ms did not decrease "
                f"({fmt_value(before_value)} -> {fmt_value(after_value)})"
            )

    if args.require_encode_ready_depth_gt1_increase:
        before_value = metric_value(before, "encode_dequeue_ready_depth_gt1")
        after_value = metric_value(after, "encode_dequeue_ready_depth_gt1")
        if after_value <= before_value:
            failures.append(
                "encode_dequeue_ready_depth_gt1 did not increase "
                f"({fmt_value(before_value)} -> {fmt_value(after_value)})"
            )

    if args.require_commit_chunk_replay_cpu_per_present_decrease:
        require_derived_decrease(
            "commit_chunk_replay_cpu_ms_per_present",
            "commit_chunk_replay_cpu_ms_per_present",
        )

    if args.require_queue_draw_submission_cpu_per_present_decrease:
        require_derived_decrease(
            "commit_chunk_queue_draw_submission_cpu_ms_per_present",
            "commit_chunk_queue_draw_submission_cpu_ms_per_present",
        )

    if args.require_snapshot_cpu_per_present_decrease:
        require_derived_decrease(
            "d3d9_snapshot_draw_submission_cpu_ms_per_present",
            "d3d9_snapshot_draw_submission_cpu_ms_per_present",
        )

    if args.require_snapshot_cache_lookup_cpu_per_present_decrease:
        require_derived_decrease(
            "d3d9_snapshot_cache_lookup_cpu_ms_per_present",
            "d3d9_snapshot_cache_lookup_cpu_ms_per_present",
        )

    if args.require_snapshot_cache_uniform_build_cpu_per_present_decrease:
        require_derived_decrease(
            "snapshot_cache_uniform_build_cpu_ms_per_present",
            "snapshot_cache_uniform_build_cpu_ms_per_present",
        )

    if args.require_snapshot_cache_uniform_hash_cpu_per_present_decrease:
        require_derived_decrease(
            "snapshot_cache_uniform_hash_cpu_ms_per_present",
            "snapshot_cache_uniform_hash_cpu_ms_per_present",
        )

    if args.require_batch_miss_uniform_build_cpu_per_present_decrease:
        require_derived_decrease(
            "snapshot_cache_batch_miss_uniform_build_cpu_ms_per_present",
            "snapshot_cache_batch_miss_uniform_build_cpu_ms_per_present",
        )

    if args.require_batch_miss_uniform_hash_cpu_per_present_decrease:
        require_derived_decrease(
            "snapshot_cache_batch_miss_uniform_hash_cpu_ms_per_present",
            "snapshot_cache_batch_miss_uniform_hash_cpu_ms_per_present",
        )

    if args.require_batch_miss_vs_const_hash_cpu_per_present_decrease:
        require_derived_decrease(
            "snapshot_cache_batch_miss_vs_const_hash_cpu_ms_per_present",
            "snapshot_cache_batch_miss_vs_const_hash_cpu_ms_per_present",
        )

    if args.require_batch_miss_ps_const_hash_cpu_per_present_decrease:
        require_derived_decrease(
            "snapshot_cache_batch_miss_ps_const_hash_cpu_ms_per_present",
            "snapshot_cache_batch_miss_ps_const_hash_cpu_ms_per_present",
        )

    if args.require_batch_miss_nonconst_hash_cpu_per_present_decrease:
        require_derived_decrease(
            "snapshot_cache_batch_miss_nonconst_hash_cpu_ms_per_present",
            "snapshot_cache_batch_miss_nonconst_hash_cpu_ms_per_present",
        )

    if args.require_snapshot_uniform_copy_cpu_per_present_decrease:
        require_derived_decrease(
            "snapshot_uniform_copy_cpu_ms_per_present",
            "snapshot_uniform_copy_cpu_ms_per_present",
        )

    if args.require_submit_draw_run_batch_append_uniform_cpu_per_present_decrease:
        require_derived_decrease(
            "submit_draw_run_batch_append_uniform_cpu_ms_per_present",
            "submit_draw_run_batch_append_uniform_cpu_ms_per_present",
        )

    if args.require_draw_uniform_payload_lookup_cpu_per_present_decrease:
        require_derived_decrease(
            "draw_uniform_payload_lookup_cpu_ms_per_present",
            "draw_uniform_payload_lookup_cpu_ms_per_present",
        )

    if args.require_draw_uniform_payload_append_copy_cpu_per_present_decrease:
        require_derived_decrease(
            "draw_uniform_payload_append_copy_cpu_ms_per_present",
            "draw_uniform_payload_append_copy_cpu_ms_per_present",
        )

    if args.require_encode_chunk_cpu_per_present_decrease:
        require_derived_decrease(
            "encode_chunk_cpu_ms_per_present",
            "encode_chunk_cpu_ms_per_present",
        )

    if args.require_argbuf_setup_cpu_per_present_decrease:
        require_derived_decrease(
            "argbuf_setup_cpu_ms_per_present",
            "argbuf_setup_cpu_ms_per_present",
        )

    if args.require_argbuf_open_cpu_per_present_decrease:
        require_derived_decrease(
            "argbuf_open_cpu_ms_per_present",
            "argbuf_open_cpu_ms_per_present",
        )

    if args.require_argbuf_cbuf_update_cpu_per_present_decrease:
        require_derived_decrease(
            "argbuf_cbuf_update_cpu_ms_per_present",
            "argbuf_cbuf_update_cpu_ms_per_present",
        )

    if args.require_argbuf_cbuf_update_vs_cpu_per_present_decrease:
        require_derived_decrease(
            "argbuf_cbuf_update_vs_cpu_ms_per_present",
            "argbuf_cbuf_update_vs_cpu_ms_per_present",
        )

    if args.require_no_enqueue_commit_entry_to_publish_decrease:
        require_derived_decrease(
            "no_enqueue_stage_commit_entry_to_publish_ms_per_present",
            "no_enqueue_stage_commit_entry_to_publish_ms_per_present",
        )

    if args.require_no_enqueue_publish_to_encode_dequeue_decrease:
        require_derived_decrease(
            "no_enqueue_stage_publish_to_encode_dequeue_ms_per_present",
            "no_enqueue_stage_publish_to_encode_dequeue_ms_per_present",
        )

    if args.require_no_enqueue_encode_dequeue_to_commit_decrease:
        require_derived_decrease(
            "no_enqueue_stage_encode_dequeue_to_command_buffer_commit_ms_per_present",
            "no_enqueue_stage_encode_dequeue_to_command_buffer_commit_ms_per_present",
        )

    if args.require_no_enqueue_wait_to_next_enqueue_decrease:
        require_derived_decrease(
            "no_enqueue_wait_to_next_enqueue_ms_per_present",
            "no_enqueue_wait_to_next_enqueue_ms_per_present",
        )

    if args.require_no_enqueue_before_publish_closure_decrease:
        require_derived_decrease(
            "no_enqueue_before_publish_closure_ms_per_present",
            "no_enqueue_before_publish_closure_ms_per_present",
        )

    if args.require_no_enqueue_before_publish_inter_replay_gap_decrease:
        require_derived_decrease(
            "no_enqueue_before_publish_inter_replay_gap_ms_per_present",
            "no_enqueue_before_publish_inter_replay_gap_ms_per_present",
        )

    if args.require_pe_focused_between_call_gap_residual_decrease:
        require_available_derived_decrease(
            "pe_recorder_focused_between_call_gap_residual_ms_per_present",
            "pe_recorder_focused_between_call_gap_residual_ms_per_present",
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
        "--require-tile-preservation-not-increase",
        action="store_true",
        help="exit nonzero if estimated tile-preservation MiB increases",
    )
    parser.add_argument(
        "--require-command-buffers-per-present-not-increase",
        action="store_true",
        help="exit nonzero if command_buffers per present increases",
    )
    parser.add_argument(
        "--require-render-passes-per-present-not-increase",
        action="store_true",
        help="exit nonzero if render_pass_begin per present increases",
    )
    parser.add_argument(
        "--require-render-pass-carry-promotion-gates",
        action="store_true",
        help=(
            "exit nonzero unless a render-pass-carry candidate moves P4 "
            "overlap/no-enqueue wait, increases ready depth, preserves "
            "CB/pass/tile/GPU/encoder-load locality, and keeps error counters zero"
        ),
    )
    parser.add_argument(
        "--require-encoder-final-end-reason-not-increase",
        action="store_true",
        help=(
            "exit nonzero if encoder-sidecar chunk-final render encoder "
            "closures per present increase"
        ),
    )
    parser.add_argument(
        "--require-encoder-final-same-key-reopen-not-increase",
        action="store_true",
        help=(
            "exit nonzero if encoder-sidecar chunk-final closures followed by "
            "the same RT/depth key per present increase"
        ),
    )
    parser.add_argument(
        "--require-encoder-color-load-not-increase",
        action="store_true",
        help="exit nonzero if encoder-sidecar color load MiB per present increases",
    )
    parser.add_argument(
        "--require-encoder-depth-load-not-increase",
        action="store_true",
        help="exit nonzero if encoder-sidecar depth load MiB per present increases",
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
    parser.add_argument(
        "--require-uniform-materialized-bytes-decrease",
        action="store_true",
        help="exit nonzero unless frontend uniform snapshot materialized bytes decrease",
    )
    parser.add_argument(
        "--require-uniform-append-bytes-decrease",
        action="store_true",
        help="exit nonzero unless backend uniform payload append bytes decrease",
    )
    parser.add_argument(
        "--require-uniform-compact-saved-bytes-present",
        action="store_true",
        help="exit nonzero unless compact uniform payload saved bytes per present is nonzero",
    )
    parser.add_argument(
        "--require-snapshot-state-elided-present",
        action="store_true",
        help="exit nonzero unless frontend state snapshot elisions per present are nonzero",
    )
    parser.add_argument(
        "--require-discarded-state-not-increase",
        action="store_true",
        help="exit nonzero if backend discarded state records or bytes per present increase",
    )
    parser.add_argument(
        "--require-submission-carrier-bytes-per-record-decrease",
        action="store_true",
        help="exit nonzero unless submission carrier bytes per record decrease",
    )
    parser.add_argument(
        "--require-submission-carrier-uniform-storage-per-record-decrease",
        action="store_true",
        help="exit nonzero unless submission carrier full-uniform storage bytes per record decrease",
    )
    parser.add_argument(
        "--require-completion-present-wait-decrease",
        action="store_true",
        help="exit nonzero unless completion_present_wait_ms decreases",
    )
    parser.add_argument(
        "--require-completion-wait-with-enqueue-increase",
        action="store_true",
        help="exit nonzero unless completion_wait_with_enqueue_ms increases",
    )
    parser.add_argument(
        "--require-completion-wait-without-enqueue-decrease",
        action="store_true",
        help="exit nonzero unless completion_wait_without_enqueue_ms decreases",
    )
    parser.add_argument(
        "--require-completion-present-wait-with-enqueue-increase",
        action="store_true",
        help="exit nonzero unless completion_present_wait_with_enqueue_ms increases",
    )
    parser.add_argument(
        "--require-completion-present-wait-without-enqueue-decrease",
        action="store_true",
        help="exit nonzero unless completion_present_wait_without_enqueue_ms decreases",
    )
    parser.add_argument(
        "--require-encode-ready-depth-gt1-increase",
        action="store_true",
        help="exit nonzero unless encode dequeue samples with ready depth > 1 increase",
    )
    parser.add_argument(
        "--require-commit-chunk-replay-cpu-per-present-decrease",
        action="store_true",
        help="exit nonzero unless commit_chunk_replay_cpu_ms per present decreases",
    )
    parser.add_argument(
        "--require-queue-draw-submission-cpu-per-present-decrease",
        action="store_true",
        help="exit nonzero unless queue draw-submission CPU per present decreases",
    )
    parser.add_argument(
        "--require-snapshot-cpu-per-present-decrease",
        action="store_true",
        help="exit nonzero unless draw-submission snapshot CPU per present decreases",
    )
    parser.add_argument(
        "--require-snapshot-cache-lookup-cpu-per-present-decrease",
        action="store_true",
        help="exit nonzero unless snapshot cache lookup CPU per present decreases",
    )
    parser.add_argument(
        "--require-snapshot-cache-uniform-build-cpu-per-present-decrease",
        action="store_true",
        help="exit nonzero unless snapshot cache uniform-build CPU per present decreases",
    )
    parser.add_argument(
        "--require-snapshot-cache-uniform-hash-cpu-per-present-decrease",
        action="store_true",
        help="exit nonzero unless snapshot cache uniform-hash CPU per present decreases",
    )
    parser.add_argument(
        "--require-batch-miss-uniform-build-cpu-per-present-decrease",
        action="store_true",
        help="exit nonzero unless batch-miss uniform-build CPU per present decreases",
    )
    parser.add_argument(
        "--require-batch-miss-uniform-hash-cpu-per-present-decrease",
        action="store_true",
        help="exit nonzero unless batch-miss uniform-hash CPU per present decreases",
    )
    parser.add_argument(
        "--require-batch-miss-vs-const-hash-cpu-per-present-decrease",
        action="store_true",
        help="exit nonzero unless batch-miss VS const hash CPU per present decreases",
    )
    parser.add_argument(
        "--require-batch-miss-ps-const-hash-cpu-per-present-decrease",
        action="store_true",
        help="exit nonzero unless batch-miss PS const hash CPU per present decreases",
    )
    parser.add_argument(
        "--require-batch-miss-nonconst-hash-cpu-per-present-decrease",
        action="store_true",
        help="exit nonzero unless batch-miss nonconst hash CPU per present decreases",
    )
    parser.add_argument(
        "--require-snapshot-uniform-copy-cpu-per-present-decrease",
        action="store_true",
        help="exit nonzero unless snapshot uniform copy CPU per present decreases",
    )
    parser.add_argument(
        "--require-submit-draw-run-batch-append-uniform-cpu-per-present-decrease",
        action="store_true",
        help="exit nonzero unless backend uniform append CPU per present decreases",
    )
    parser.add_argument(
        "--require-draw-uniform-payload-lookup-cpu-per-present-decrease",
        action="store_true",
        help="exit nonzero unless uniform payload lookup CPU per present decreases",
    )
    parser.add_argument(
        "--require-draw-uniform-payload-append-copy-cpu-per-present-decrease",
        action="store_true",
        help="exit nonzero unless uniform payload append-copy CPU per present decreases",
    )
    parser.add_argument(
        "--require-encode-chunk-cpu-per-present-decrease",
        action="store_true",
        help="exit nonzero unless encode_chunk_cpu_ms per present decreases",
    )
    parser.add_argument(
        "--require-argbuf-setup-cpu-per-present-decrease",
        action="store_true",
        help="exit nonzero unless argbuf setup CPU per present decreases",
    )
    parser.add_argument(
        "--require-argbuf-open-cpu-per-present-decrease",
        action="store_true",
        help="exit nonzero unless argbuf open/reopen CPU per present decreases",
    )
    parser.add_argument(
        "--require-argbuf-cbuf-update-cpu-per-present-decrease",
        action="store_true",
        help="exit nonzero unless argbuf cbuf update CPU per present decreases",
    )
    parser.add_argument(
        "--require-argbuf-cbuf-update-vs-cpu-per-present-decrease",
        action="store_true",
        help="exit nonzero unless argbuf VS cbuf update CPU per present decreases",
    )
    parser.add_argument(
        "--require-no-enqueue-commit-entry-to-publish-decrease",
        action="store_true",
        help="exit nonzero unless no-enqueue commit-entry to publish time decreases",
    )
    parser.add_argument(
        "--require-no-enqueue-publish-to-encode-dequeue-decrease",
        action="store_true",
        help="exit nonzero unless no-enqueue publish to encode-dequeue time decreases",
    )
    parser.add_argument(
        "--require-no-enqueue-encode-dequeue-to-commit-decrease",
        action="store_true",
        help="exit nonzero unless no-enqueue encode-dequeue to Metal commit time decreases",
    )
    parser.add_argument(
        "--require-no-enqueue-wait-to-next-enqueue-decrease",
        action="store_true",
        help="exit nonzero unless no-enqueue wait-to-next-enqueue time decreases",
    )
    parser.add_argument(
        "--require-no-enqueue-before-publish-closure-decrease",
        action="store_true",
        help="exit nonzero unless no-enqueue before-publish closure time decreases",
    )
    parser.add_argument(
        "--require-no-enqueue-before-publish-inter-replay-gap-decrease",
        action="store_true",
        help="exit nonzero unless no-enqueue before-publish inter-replay gap decreases",
    )
    parser.add_argument(
        "--require-pe-focused-between-call-gap-residual-decrease",
        action="store_true",
        help=(
            "exit nonzero unless aggregate focused PE between-call residual "
            "wall time per present decreases"
        ),
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
