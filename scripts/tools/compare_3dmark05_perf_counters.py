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
    batch_miss_ps_const_hash_cpu_ms = number(counter(
        counters,
        "d3d9_snapshot_cache_batch_miss_uniform_build_ps_const_hash_cpu_ms",
    )) or 0.0
    batch_miss_nonconst_hash_cpu_ms = number(counter(
        counters,
        "d3d9_snapshot_cache_batch_miss_uniform_build_nonconst_hash_cpu_ms",
    )) or 0.0
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
        "command_buffers_per_present": ratio(
            counters, "command_buffers", "present_encoded",
        ),
        "sub_command_buffers_per_present": ratio(
            counters, "sub_command_buffers", "present_encoded",
        ),
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
        "completion_enqueue_while_waiting_per_present": (
            completion_enqueue_while_waiting / present if present else None
        ),
        "completion_present_enqueue_while_waiting_per_present": (
            completion_enqueue_while_waiting_present / present if present else None
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
        "snapshot_cache_batch_miss_ps_const_hash_cpu_ms_per_present": (
            batch_miss_ps_const_hash_cpu_ms / present if present else None
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
