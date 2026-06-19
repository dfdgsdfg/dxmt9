#!/usr/bin/env python3
"""Regression tests for 3DMark05 perf counter comparison gates."""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "tools" / "compare_3dmark05_perf_counters.py"

UNIFORM_OWNER_COMPARE_FLAGS = (
    "--require-snapshot-cache-uniform-build-cpu-per-present-decrease",
    "--require-snapshot-cache-uniform-hash-cpu-per-present-decrease",
    "--require-batch-miss-uniform-build-cpu-per-present-decrease",
    "--require-batch-miss-uniform-hash-cpu-per-present-decrease",
    "--require-batch-miss-vs-const-hash-cpu-per-present-decrease",
    "--require-batch-miss-ps-const-hash-cpu-per-present-decrease",
    "--require-batch-miss-nonconst-hash-cpu-per-present-decrease",
    "--require-snapshot-uniform-copy-cpu-per-present-decrease",
    "--require-submit-draw-run-batch-append-uniform-cpu-per-present-decrease",
    "--require-draw-uniform-payload-lookup-cpu-per-present-decrease",
    "--require-draw-uniform-payload-append-copy-cpu-per-present-decrease",
)

UNIFORM_COMPACT_COMPARE_FLAGS = (
    "--require-uniform-compact-saved-bytes-present",
)

STATE_ELISION_COMPARE_FLAGS = (
    "--require-snapshot-state-elided-present",
    "--require-discarded-state-not-increase",
)

CARRIER_COMPARE_FLAGS = (
    "--require-submission-carrier-bytes-per-record-decrease",
    "--require-submission-carrier-uniform-storage-per-record-decrease",
)


def write_result(path: Path, counters: dict[str, int | float]) -> None:
    path.mkdir(parents=True, exist_ok=True)
    path.joinpath("result.json").write_text(
        json.dumps({"dxmt9_perf_counters": counters}, indent=2),
        encoding="utf-8",
    )


def write_encoder_csv(path: Path, rows: list[dict[str, str | int]]) -> None:
    fields = (
        "end_reason",
        "color_load_bytes",
        "color_store_bytes",
        "depth_load_bytes",
        "depth_store_bytes",
    )
    lines = [",".join(fields)]
    for row in rows:
        lines.append(",".join(str(row.get(field, 0)) for field in fields))
    path.joinpath("3dmark05-perf-encoders.csv").write_text(
        "\n".join(lines) + "\n",
        encoding="utf-8",
    )


class Compare3DMark05PerfCountersTests(unittest.TestCase):
    def run_compare(self, root: Path, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [
                sys.executable,
                str(SCRIPT),
                str(root / "before"),
                str(root / "after"),
                "--output",
                str(root / "comparison.md"),
                *args,
            ],
            text=True,
            capture_output=True,
            check=False,
        )

    def test_mechanism_gates_pass_when_counters_move(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write_result(root / "before", {
                "present_encoded": 2,
                "draw_calls": 100,
                "commit_chunk_draw_run_submits": 10,
                "commit_chunk_draw_run_records": 20,
                "commit_chunk_draw_run_binding_override_records": 0,
                "commit_chunk_draw_batch_const_upload_passthrough": 0,
                "commit_chunk_draw_submission_batch_submits": 4,
                "commit_chunk_draw_submission_batch_records": 8,
                "commit_chunk_draw_submission_batch_max_records": 3,
                "commit_chunk_draw_run_break_type_const_upload": 50,
                "commit_chunk_draw_run_break_type_const_upload_bytes": 800,
                "commit_chunk_draw_run_break_type_const_upload_registers": 50,
                "commit_chunk_draw_run_break_type_const_vs_f": 40,
                "commit_chunk_draw_run_break_type_const_vs_f_bytes": 640,
                "commit_chunk_draw_run_break_type_const_vs_f_registers": 40,
                "commit_chunk_draw_run_break_type_const_vs_i": 5,
                "commit_chunk_draw_run_break_type_const_vs_i_bytes": 80,
                "commit_chunk_draw_run_break_type_const_vs_i_registers": 5,
                "commit_chunk_draw_run_break_type_const_vs_b": 0,
                "commit_chunk_draw_run_break_type_const_vs_b_bytes": 0,
                "commit_chunk_draw_run_break_type_const_vs_b_registers": 0,
                "commit_chunk_draw_run_break_type_const_ps_f": 5,
                "commit_chunk_draw_run_break_type_const_ps_f_bytes": 80,
                "commit_chunk_draw_run_break_type_const_ps_f_registers": 5,
                "commit_chunk_draw_run_break_type_const_ps_i": 0,
                "commit_chunk_draw_run_break_type_const_ps_i_bytes": 0,
                "commit_chunk_draw_run_break_type_const_ps_i_registers": 0,
                "commit_chunk_draw_run_break_type_const_ps_b": 0,
                "commit_chunk_draw_run_break_type_const_ps_b_bytes": 0,
                "commit_chunk_draw_run_break_type_const_ps_b_registers": 0,
                "commit_chunk_draw_run_break_state_delta": 25,
                "commit_chunk_draw_run_break_state_delta_stream_only": 5,
                "commit_chunk_draw_run_break_state_delta_mixed": 20,
                "commit_chunk_draw_run_break_state_delta_mixed_group2": 18,
                "commit_chunk_draw_run_break_state_delta_stream_ib_only": 15,
                "commit_chunk_draw_run_break_state_delta_mixed_pair_stream_ib": 18,
                "commit_chunk_draw_delta_stream": 50,
                "commit_chunk_draw_delta_ib": 45,
                "d3d9_snapshot_uniform_materialized": 4,
                "d3d9_snapshot_uniform_materialized_bytes": 40960,
                "d3d9_snapshot_uniform_materialized_compact_candidate_bytes": 20480,
                "d3d9_snapshot_uniform_materialized_compact_saved_bytes": 20480,
                "d3d9_snapshot_uniform_materialized_compact_fixed_bytes": 8192,
                "d3d9_snapshot_uniform_materialized_compact_vertex_bytes": 4096,
                "d3d9_snapshot_uniform_materialized_compact_pixel_bytes": 8192,
                "d3d9_snapshot_uniform_elided": 1,
                "d3d9_snapshot_uniform_elided_bytes": 10240,
                "d3d9_snapshot_uniform_adjacent_previous_payload": 10,
                "d3d9_snapshot_uniform_adjacent_same_fixed_payload_hash": 8,
                "d3d9_snapshot_uniform_adjacent_same_fixed_and_shader_const_hashes": 3,
                "draw_uniform_payload_appends": 2,
                "draw_uniform_payload_append_bytes": 20512,
                "draw_uniform_fixed_payload_appends": 1,
                "draw_uniform_fixed_payload_append_bytes": 8192,
                "draw_uniform_vertex_constants_appends": 1,
                "draw_uniform_vertex_constants_append_bytes": 4096,
                "draw_uniform_pixel_constants_appends": 1,
                "draw_uniform_pixel_constants_append_bytes": 2048,
                "draw_uniform_payload_materialized": 3,
                "draw_uniform_payload_materialized_bytes": 30720,
                "draw_uniform_payload_materialize_fallbacks": 1,
                "draw_uniform_payload_materialize_cpu_ms": 1.5,
                "draw_uniform_payload_materialized_draw_encoder_command": 1,
                "draw_uniform_payload_materialized_draw_encoder_command_bytes": 10240,
                "draw_uniform_payload_materialize_draw_encoder_command_cpu_ms": 0.5,
                "draw_uniform_payload_materialized_draw_encoder_param": 2,
                "draw_uniform_payload_materialized_draw_encoder_param_bytes": 20480,
                "draw_uniform_payload_materialize_draw_encoder_param_cpu_ms": 1.0,
                "completion_wait_ms": 100.0,
                "completion_present_wait_ms": 90.0,
                "completion_wait_with_enqueue_ms": 0.0,
                "completion_wait_without_enqueue_ms": 100.0,
                "completion_present_wait_with_enqueue_ms": 0.0,
                "completion_present_wait_without_enqueue_ms": 90.0,
                "completion_enqueue_while_waiting": 0,
                "completion_enqueue_while_waiting_present": 0,
                "completion_wait_commit_chunk_entries": 0,
                "completion_wait_commit_chunk_replay_starts": 0,
                "completion_wait_commit_chunk_replay_ends": 0,
                "completion_wait_commit_chunk_replay_cpu_ms": 0.0,
                "chunk_publish_slot_residency_samples": 2,
                "chunk_publish_slot_residency_ms": 20.0,
                "chunk_publish_slot_residency_p50_ms": 11.0,
                "chunk_publish_slot_residency_p95_ms": 19.0,
                "chunk_publish_slot_residency_present_samples": 2,
                "chunk_publish_slot_residency_present_ms": 18.0,
                "chunk_publish_slot_residency_present_p50_ms": 10.0,
                "chunk_publish_slot_residency_present_p95_ms": 17.0,
                "chunk_publish_slot_residency_nonpresent_samples": 1,
                "chunk_publish_slot_residency_nonpresent_ms": 2.0,
                "chunk_publish_slot_residency_nonpresent_p50_ms": 2.0,
                "chunk_publish_slot_residency_nonpresent_p95_ms": 2.0,
                "chunk_publish_reason_present_split_before": 4,
                "chunk_publish_present_split_before_tail_draw_run": 3,
                "chunk_publish_present_split_before_tail_clear": 1,
                "chunk_publish_present_split_before_draw_only": 3,
                "chunk_publish_present_pre_present_opportunity_slots": 2,
                "chunk_publish_present_pre_present_opportunity_tail_slots": 2,
                "chunk_publish_present_pre_present_opportunity_nontail_slots": 0,
                "chunk_publish_present_pre_present_opportunity_commands": 100,
                "chunk_publish_present_pre_present_opportunity_draw_runs": 80,
                "chunk_publish_present_pre_present_opportunity_draw_items": 160,
                "chunk_publish_present_pre_present_opportunity_non_draw_commands": 20,
                "chunk_publish_present_pre_present_opportunity_payload_bytes": 4096,
                "chunk_publish_present_pre_present_opportunity_residency_ms": 18.0,
                "chunk_publish_present_pre_present_opportunity_residency_p50_ms": 10.0,
                "chunk_publish_present_pre_present_opportunity_residency_p95_ms": 17.0,
                "chunk_publish_present_pre_present_opportunity_tail_draw_run": 2,
                "chunk_publish_present_pre_present_opportunity_tail_clear": 0,
                "chunk_publish_present_pre_present_opportunity_draw_only": 2,
                "completion_no_enqueue_first_publish_slot_samples": 2,
                "completion_no_enqueue_first_publish_slot_commands": 100,
                "completion_no_enqueue_first_publish_slot_commands_p50": 50,
                "completion_no_enqueue_first_publish_slot_commands_p95": 70,
                "completion_no_enqueue_first_publish_slot_draw_run_commands": 80,
                "completion_no_enqueue_first_publish_slot_draw_items": 160,
                "completion_no_enqueue_first_publish_slot_draw_items_p50": 80,
                "completion_no_enqueue_first_publish_slot_draw_items_p95": 120,
                "completion_no_enqueue_first_publish_slot_non_draw_commands": 20,
                "completion_no_enqueue_first_publish_slot_payload_bytes": 4096,
                "completion_no_enqueue_first_publish_slot_payload_bytes_p50": 2048,
                "completion_no_enqueue_first_publish_slot_payload_bytes_p95": 3072,
                "completion_no_enqueue_first_publish_slot_present_commands": 2,
                "encode_dequeue_ready_depth_samples": 10,
                "encode_dequeue_ready_depth_total": 10,
                "encode_dequeue_ready_depth_max": 1,
                "encode_dequeue_ready_depth_gt1": 0,
                "encode_dequeue_ready_depth_gt2": 0,
                "encode_dequeue_ready_depth_gt4": 0,
                "commit_chunk_replay_cpu_ms": 40.0,
                "commit_chunk_queue_draw_submission_cpu_ms": 20.0,
                "d3d9_snapshot_draw_submission_cpu_ms": 18.0,
                "d3d9_snapshot_cache_lookup_cpu_ms": 10.0,
                "d3d9_snapshot_cache_uniform_build_cpu_ms": 8.0,
                "d3d9_snapshot_cache_uniform_hash_cpu_ms": 4.0,
                "d3d9_snapshot_cache_batch_miss_uniform_build_cpu_ms": 6.0,
                "d3d9_snapshot_cache_batch_miss_uniform_build_hash_cpu_ms": 3.0,
                "d3d9_snapshot_cache_batch_miss_uniform_build_vs_const_hash_cpu_ms": 2.0,
                "d3d9_snapshot_cache_batch_miss_uniform_build_ps_const_hash_cpu_ms": 0.5,
                "d3d9_snapshot_cache_batch_miss_uniform_build_nonconst_hash_cpu_ms": 0.5,
                "d3d9_snapshot_uniform_copy_cpu_ms": 2.0,
                "submit_draw_run_batch_append_uniform_cpu_ms": 3.0,
                "draw_uniform_payload_lookup_cpu_ms": 1.0,
                "draw_uniform_payload_append_copy_cpu_ms": 2.0,
                "encode_chunk_cpu_ms": 50.0,
                "encode_draw_argbuf_setup_cpu_ms": 24.0,
                "encode_draw_argbuf_open_cpu_ms": 10.0,
                "encode_draw_argbuf_cbuf_update_cpu_ms": 12.0,
                "encode_draw_argbuf_cbuf_update_vs_cpu_ms": 8.0,
                "completion_no_enqueue_stage_commit_entry_to_publish_ms": 20.0,
                "completion_no_enqueue_stage_publish_to_encode_dequeue_ms": 8.0,
                "completion_no_enqueue_stage_encode_dequeue_to_command_buffer_commit_ms": 30.0,
                "completion_no_enqueue_wait_to_next_enqueue_ms": 60.0,
                "completion_no_enqueue_commit_chunk_completed_replay_cpu_before_publish_ms": 6.0,
                "completion_no_enqueue_commit_chunk_active_replay_cpu_before_publish_ms": 2.0,
                "completion_no_enqueue_commit_chunk_inter_replay_gap_before_publish_ms": 10.0,
                "completion_no_enqueue_commit_publish_wait_before_publish_ms": 1.0,
                "completion_no_enqueue_commit_publish_on_before_publish_cpu_ms": 4.0,
                "completion_no_enqueue_commit_chunk_entries_before_publish": 10,
                "completion_no_enqueue_commit_chunk_replay_starts_before_publish": 8,
                "completion_no_enqueue_commit_chunk_replay_ends_before_publish": 7,
                "completion_no_enqueue_commit_chunk_shape_samples_before_publish": 5,
                "completion_no_enqueue_commit_chunk_records_before_publish": 40,
                "completion_no_enqueue_commit_chunk_chunks_with_draw_before_publish": 4,
                "completion_no_enqueue_commit_chunk_chunks_with_present_before_publish": 2,
                "completion_no_enqueue_commit_chunk_chunks_state_const_only_before_publish": 1,
                "completion_no_enqueue_commit_chunk_chunks_no_draw_no_present_before_publish": 1,
                "completion_no_enqueue_commit_chunk_draw_records_before_publish": 20,
                "completion_no_enqueue_commit_chunk_const_records_before_publish": 12,
                "completion_no_enqueue_commit_chunk_apply_state_records_before_publish": 4,
                "completion_no_enqueue_commit_chunk_clear_records_before_publish": 2,
                "completion_no_enqueue_commit_chunk_present_records_before_publish": 2,
                "completion_no_enqueue_commit_chunk_surface_records_before_publish": 0,
                "completion_no_enqueue_commit_chunk_query_records_before_publish": 0,
                "completion_no_enqueue_commit_chunk_other_records_before_publish": 0,
                "encode_draw_cpu_ms": 100.0,
            })
            write_result(root / "after", {
                "present_encoded": 2,
                "draw_calls": 100,
                "commit_chunk_draw_run_submits": 10,
                "commit_chunk_draw_run_records": 40,
                "commit_chunk_draw_run_binding_override_records": 12,
                "commit_chunk_draw_batch_const_upload_passthrough": 5,
                "commit_chunk_draw_submission_batch_submits": 5,
                "commit_chunk_draw_submission_batch_records": 25,
                "commit_chunk_draw_submission_batch_max_records": 7,
                "commit_chunk_draw_run_break_type_const_upload": 40,
                "commit_chunk_draw_run_break_type_const_upload_bytes": 640,
                "commit_chunk_draw_run_break_type_const_upload_registers": 40,
                "commit_chunk_draw_run_break_type_const_vs_f": 30,
                "commit_chunk_draw_run_break_type_const_vs_f_bytes": 480,
                "commit_chunk_draw_run_break_type_const_vs_f_registers": 30,
                "commit_chunk_draw_run_break_type_const_vs_i": 4,
                "commit_chunk_draw_run_break_type_const_vs_i_bytes": 64,
                "commit_chunk_draw_run_break_type_const_vs_i_registers": 4,
                "commit_chunk_draw_run_break_type_const_vs_b": 0,
                "commit_chunk_draw_run_break_type_const_vs_b_bytes": 0,
                "commit_chunk_draw_run_break_type_const_vs_b_registers": 0,
                "commit_chunk_draw_run_break_type_const_ps_f": 6,
                "commit_chunk_draw_run_break_type_const_ps_f_bytes": 96,
                "commit_chunk_draw_run_break_type_const_ps_f_registers": 6,
                "commit_chunk_draw_run_break_type_const_ps_i": 0,
                "commit_chunk_draw_run_break_type_const_ps_i_bytes": 0,
                "commit_chunk_draw_run_break_type_const_ps_i_registers": 0,
                "commit_chunk_draw_run_break_type_const_ps_b": 0,
                "commit_chunk_draw_run_break_type_const_ps_b_bytes": 0,
                "commit_chunk_draw_run_break_type_const_ps_b_registers": 0,
                "commit_chunk_draw_run_break_state_delta": 20,
                "commit_chunk_draw_run_break_state_delta_stream_only": 4,
                "commit_chunk_draw_run_break_state_delta_mixed": 16,
                "commit_chunk_draw_run_break_state_delta_mixed_group2": 14,
                "commit_chunk_draw_run_break_state_delta_stream_ib_only": 12,
                "commit_chunk_draw_run_break_state_delta_mixed_pair_stream_ib": 14,
                "commit_chunk_draw_delta_stream": 40,
                "commit_chunk_draw_delta_ib": 35,
                "d3d9_snapshot_uniform_materialized": 4,
                "d3d9_snapshot_uniform_materialized_bytes": 20480,
                "d3d9_snapshot_uniform_materialized_compact_candidate_bytes": 10240,
                "d3d9_snapshot_uniform_materialized_compact_saved_bytes": 10240,
                "d3d9_snapshot_uniform_materialized_compact_fixed_bytes": 4096,
                "d3d9_snapshot_uniform_materialized_compact_vertex_bytes": 2048,
                "d3d9_snapshot_uniform_materialized_compact_pixel_bytes": 4096,
                "d3d9_snapshot_uniform_elided": 4,
                "d3d9_snapshot_uniform_elided_bytes": 40960,
                "d3d9_snapshot_uniform_adjacent_previous_payload": 5,
                "d3d9_snapshot_uniform_adjacent_same_fixed_payload_hash": 4,
                "d3d9_snapshot_uniform_adjacent_same_fixed_and_shader_const_hashes": 1,
                "draw_uniform_payload_appends": 1,
                "draw_uniform_payload_append_bytes": 10256,
                "draw_uniform_fixed_payload_appends": 1,
                "draw_uniform_fixed_payload_append_bytes": 4096,
                "draw_uniform_vertex_constants_appends": 1,
                "draw_uniform_vertex_constants_append_bytes": 2048,
                "draw_uniform_pixel_constants_appends": 1,
                "draw_uniform_pixel_constants_append_bytes": 1024,
                "draw_uniform_payload_materialized": 2,
                "draw_uniform_payload_materialized_bytes": 20480,
                "draw_uniform_payload_materialize_fallbacks": 0,
                "draw_uniform_payload_materialize_cpu_ms": 1.0,
                "completion_wait_ms": 80.0,
                "completion_present_wait_ms": 70.0,
                "completion_wait_with_enqueue_ms": 10.0,
                "completion_wait_without_enqueue_ms": 70.0,
                "completion_present_wait_with_enqueue_ms": 8.0,
                "completion_present_wait_without_enqueue_ms": 62.0,
                "completion_enqueue_while_waiting": 4,
                "completion_enqueue_while_waiting_present": 2,
                "completion_wait_commit_chunk_entries": 3,
                "completion_wait_commit_chunk_replay_starts": 2,
                "completion_wait_commit_chunk_replay_ends": 2,
                "completion_wait_commit_chunk_replay_cpu_ms": 6.0,
                "chunk_publish_slot_residency_samples": 3,
                "chunk_publish_slot_residency_ms": 10.0,
                "chunk_publish_slot_residency_p50_ms": 5.0,
                "chunk_publish_slot_residency_p95_ms": 9.0,
                "chunk_publish_slot_residency_present_samples": 2,
                "chunk_publish_slot_residency_present_ms": 6.0,
                "chunk_publish_slot_residency_present_p50_ms": 3.0,
                "chunk_publish_slot_residency_present_p95_ms": 5.0,
                "chunk_publish_slot_residency_nonpresent_samples": 1,
                "chunk_publish_slot_residency_nonpresent_ms": 4.0,
                "chunk_publish_slot_residency_nonpresent_p50_ms": 4.0,
                "chunk_publish_slot_residency_nonpresent_p95_ms": 4.0,
                "chunk_publish_present_pre_present_opportunity_slots": 2,
                "chunk_publish_present_pre_present_opportunity_tail_slots": 1,
                "chunk_publish_present_pre_present_opportunity_nontail_slots": 1,
                "chunk_publish_present_pre_present_opportunity_commands": 60,
                "chunk_publish_present_pre_present_opportunity_draw_runs": 50,
                "chunk_publish_present_pre_present_opportunity_draw_items": 120,
                "chunk_publish_present_pre_present_opportunity_non_draw_commands": 10,
                "chunk_publish_present_pre_present_opportunity_payload_bytes": 2048,
                "chunk_publish_present_pre_present_opportunity_residency_ms": 6.0,
                "chunk_publish_present_pre_present_opportunity_residency_p50_ms": 3.0,
                "chunk_publish_present_pre_present_opportunity_residency_p95_ms": 5.0,
                "chunk_publish_present_pre_present_opportunity_tail_draw_run": 1,
                "chunk_publish_present_pre_present_opportunity_tail_clear": 1,
                "chunk_publish_present_pre_present_opportunity_draw_only": 1,
                "completion_no_enqueue_first_publish_slot_samples": 2,
                "completion_no_enqueue_first_publish_slot_commands": 60,
                "completion_no_enqueue_first_publish_slot_commands_p50": 30,
                "completion_no_enqueue_first_publish_slot_commands_p95": 45,
                "completion_no_enqueue_first_publish_slot_draw_run_commands": 50,
                "completion_no_enqueue_first_publish_slot_draw_items": 120,
                "completion_no_enqueue_first_publish_slot_draw_items_p50": 60,
                "completion_no_enqueue_first_publish_slot_draw_items_p95": 90,
                "completion_no_enqueue_first_publish_slot_non_draw_commands": 10,
                "completion_no_enqueue_first_publish_slot_payload_bytes": 2048,
                "completion_no_enqueue_first_publish_slot_payload_bytes_p50": 1024,
                "completion_no_enqueue_first_publish_slot_payload_bytes_p95": 1536,
                "completion_no_enqueue_first_publish_slot_present_commands": 2,
                "encode_dequeue_ready_depth_samples": 10,
                "encode_dequeue_ready_depth_total": 25,
                "encode_dequeue_ready_depth_max": 5,
                "encode_dequeue_ready_depth_gt1": 4,
                "encode_dequeue_ready_depth_gt2": 2,
                "encode_dequeue_ready_depth_gt4": 1,
                "commit_chunk_replay_cpu_ms": 30.0,
                "commit_chunk_queue_draw_submission_cpu_ms": 14.0,
                "d3d9_snapshot_draw_submission_cpu_ms": 12.0,
                "d3d9_snapshot_cache_lookup_cpu_ms": 6.0,
                "d3d9_snapshot_cache_uniform_build_cpu_ms": 6.0,
                "d3d9_snapshot_cache_uniform_hash_cpu_ms": 2.5,
                "d3d9_snapshot_cache_batch_miss_uniform_build_cpu_ms": 4.0,
                "d3d9_snapshot_cache_batch_miss_uniform_build_hash_cpu_ms": 1.5,
                "d3d9_snapshot_cache_batch_miss_uniform_build_vs_const_hash_cpu_ms": 1.0,
                "d3d9_snapshot_cache_batch_miss_uniform_build_ps_const_hash_cpu_ms": 0.25,
                "d3d9_snapshot_cache_batch_miss_uniform_build_nonconst_hash_cpu_ms": 0.25,
                "d3d9_snapshot_uniform_copy_cpu_ms": 1.0,
                "submit_draw_run_batch_append_uniform_cpu_ms": 2.0,
                "draw_uniform_payload_lookup_cpu_ms": 0.5,
                "draw_uniform_payload_append_copy_cpu_ms": 1.0,
                "encode_chunk_cpu_ms": 40.0,
                "encode_draw_argbuf_setup_cpu_ms": 16.0,
                "encode_draw_argbuf_open_cpu_ms": 6.0,
                "encode_draw_argbuf_cbuf_update_cpu_ms": 8.0,
                "encode_draw_argbuf_cbuf_update_vs_cpu_ms": 4.0,
                "completion_no_enqueue_stage_commit_entry_to_publish_ms": 12.0,
                "completion_no_enqueue_stage_publish_to_encode_dequeue_ms": 4.0,
                "completion_no_enqueue_stage_encode_dequeue_to_command_buffer_commit_ms": 20.0,
                "completion_no_enqueue_wait_to_next_enqueue_ms": 42.0,
                "completion_no_enqueue_commit_chunk_completed_replay_cpu_before_publish_ms": 4.0,
                "completion_no_enqueue_commit_chunk_active_replay_cpu_before_publish_ms": 1.0,
                "completion_no_enqueue_commit_chunk_inter_replay_gap_before_publish_ms": 5.0,
                "completion_no_enqueue_commit_publish_wait_before_publish_ms": 1.0,
                "completion_no_enqueue_commit_publish_on_before_publish_cpu_ms": 2.0,
                "completion_no_enqueue_commit_chunk_entries_before_publish": 8,
                "completion_no_enqueue_commit_chunk_replay_starts_before_publish": 6,
                "completion_no_enqueue_commit_chunk_replay_ends_before_publish": 6,
                "completion_no_enqueue_commit_chunk_shape_samples_before_publish": 4,
                "completion_no_enqueue_commit_chunk_records_before_publish": 32,
                "completion_no_enqueue_commit_chunk_chunks_with_draw_before_publish": 3,
                "completion_no_enqueue_commit_chunk_chunks_with_present_before_publish": 2,
                "completion_no_enqueue_commit_chunk_chunks_state_const_only_before_publish": 1,
                "completion_no_enqueue_commit_chunk_chunks_no_draw_no_present_before_publish": 0,
                "completion_no_enqueue_commit_chunk_draw_records_before_publish": 16,
                "completion_no_enqueue_commit_chunk_const_records_before_publish": 8,
                "completion_no_enqueue_commit_chunk_apply_state_records_before_publish": 4,
                "completion_no_enqueue_commit_chunk_clear_records_before_publish": 2,
                "completion_no_enqueue_commit_chunk_present_records_before_publish": 2,
                "completion_no_enqueue_commit_chunk_surface_records_before_publish": 0,
                "completion_no_enqueue_commit_chunk_query_records_before_publish": 0,
                "completion_no_enqueue_commit_chunk_other_records_before_publish": 0,
                "encode_draw_cpu_ms": 90.0,
            })

            result = self.run_compare(
                root,
                "--require-draw-run-records-increase",
                "--require-draw-run-records-per-submit-increase",
                "--require-binding-overrides-present",
                "--require-const-upload-passthrough-present",
                "--require-const-upload-break-bytes-decrease",
                "--max-const-upload-break-count-ratio",
                "1.10",
                "--require-encode-draw-cpu-decrease",
                "--require-uniform-materialized-bytes-decrease",
                "--require-uniform-append-bytes-decrease",
                *UNIFORM_COMPACT_COMPARE_FLAGS,
                "--require-completion-present-wait-decrease",
                "--require-completion-wait-with-enqueue-increase",
                "--require-completion-wait-without-enqueue-decrease",
                "--require-completion-present-wait-with-enqueue-increase",
                "--require-completion-present-wait-without-enqueue-decrease",
                "--require-encode-ready-depth-gt1-increase",
                "--require-commit-chunk-replay-cpu-per-present-decrease",
                "--require-queue-draw-submission-cpu-per-present-decrease",
                "--require-snapshot-cpu-per-present-decrease",
                "--require-snapshot-cache-lookup-cpu-per-present-decrease",
                *UNIFORM_OWNER_COMPARE_FLAGS,
                "--require-encode-chunk-cpu-per-present-decrease",
                "--require-argbuf-setup-cpu-per-present-decrease",
                "--require-argbuf-open-cpu-per-present-decrease",
                "--require-argbuf-cbuf-update-cpu-per-present-decrease",
                "--require-argbuf-cbuf-update-vs-cpu-per-present-decrease",
                "--require-no-enqueue-commit-entry-to-publish-decrease",
                "--require-no-enqueue-publish-to-encode-dequeue-decrease",
                "--require-no-enqueue-encode-dequeue-to-commit-decrease",
                "--require-no-enqueue-wait-to-next-enqueue-decrease",
                "--require-no-enqueue-before-publish-closure-decrease",
                "--require-no-enqueue-before-publish-inter-replay-gap-decrease",
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            report = root.joinpath("comparison.md").read_text(encoding="utf-8")
            self.assertIn("binding_override_records_per_draw_run_record", report)
            self.assertIn("draw_submission_batch_records_per_submit", report)
            self.assertIn("const_upload_breaks_per_draw", report)
            self.assertIn("const_upload_passthrough_per_present", report)
            self.assertIn("const_upload_subtype_coverage_pct", report)
            self.assertIn("const_vs_f_share_pct", report)
            self.assertIn("const_vs_f_byte_share_pct", report)
            self.assertIn("state_delta_mixed_share_pct", report)
            self.assertIn("state_delta_mixed_pair_stream_ib_share_pct", report)
            self.assertIn("stream_deltas_per_draw", report)
            self.assertIn("ib_deltas_per_draw", report)
            self.assertIn("uniform_materialized_bytes_per_present", report)
            self.assertIn("uniform_compact_candidate_bytes_per_present", report)
            self.assertIn("uniform_compact_saved_bytes_per_present", report)
            self.assertIn("uniform_compact_fixed_bytes_per_present", report)
            self.assertIn("uniform_compact_vertex_bytes_per_present", report)
            self.assertIn("uniform_compact_pixel_bytes_per_present", report)
            self.assertIn("uniform_compact_candidate_share_of_materialized_bytes", report)
            self.assertIn("uniform_compact_fixed_share_of_candidate_bytes", report)
            self.assertIn("uniform_compact_vertex_share_of_candidate_bytes", report)
            self.assertIn("uniform_compact_pixel_share_of_candidate_bytes", report)
            self.assertIn("uniform_compact_saved_share_of_materialized_bytes", report)
            self.assertIn("uniform_adjacent_same_fixed_payload_hash_share", report)
            self.assertIn("uniform_adjacent_same_fixed_and_shader_const_hashes_share", report)
            self.assertIn("uniform_append_bytes_per_present", report)
            self.assertIn("uniform_fixed_append_bytes_per_present", report)
            self.assertIn("uniform_vertex_constants_append_bytes_per_present", report)
            self.assertIn("uniform_pixel_constants_append_bytes_per_present", report)
            self.assertIn("uniform_stage_constants_append_bytes_per_present", report)
            self.assertIn("uniform_vertex_append_amplification_vs_compact_vertex", report)
            self.assertIn("uniform_pixel_append_amplification_vs_compact_pixel", report)
            self.assertIn("uniform_stage_append_amplification_vs_compact_stage", report)
            self.assertIn("uniform_append_bytes_per_append", report)
            self.assertIn("uniform_payload_record_append_bytes_per_append", report)
            self.assertIn("uniform_fixed_append_records_per_payload_append", report)
            self.assertIn("uniform_vertex_constants_append_records_per_payload_append", report)
            self.assertIn("uniform_pixel_constants_append_records_per_payload_append", report)
            self.assertIn("uniform_backend_materialized_bytes_per_present", report)
            self.assertIn("uniform_backend_materialize_cpu_ms_per_present", report)
            self.assertIn("uniform_backend_materialized_bytes_per_call", report)
            self.assertIn("uniform_backend_materialize_fallbacks_per_present", report)
            self.assertIn("uniform_backend_materialize_draw_encoder_command_share_pct", report)
            self.assertIn("uniform_backend_materialize_draw_encoder_command_bytes_per_present", report)
            self.assertIn("uniform_backend_materialize_draw_encoder_command_cpu_ms_per_present", report)
            self.assertIn("uniform_backend_materialize_draw_encoder_param_share_pct", report)
            self.assertIn("uniform_backend_materialize_queue_observation_share_pct", report)
            self.assertIn("uniform_append_records_per_materialized_snapshot", report)
            self.assertIn("uniform_semantic_hash_misses_per_present", report)
            self.assertIn("uniform_semantic_hash_miss_bytes_per_present", report)
            self.assertIn("uniform_append_bytes_share_of_materialized_bytes", report)
            self.assertIn("uniform_fixed_append_bytes_share_of_append_bytes", report)
            self.assertIn("uniform_vertex_constants_append_bytes_share_of_append_bytes", report)
            self.assertIn("uniform_pixel_constants_append_bytes_share_of_append_bytes", report)
            self.assertIn("uniform_snapshot_elision_share", report)
            self.assertIn("completion_present_wait_ms_per_present", report)
            self.assertIn("completion_wait_with_enqueue_ms_per_present", report)
            self.assertIn("completion_wait_without_enqueue_ms_per_present", report)
            self.assertIn("completion_wait_overlap_share_pct", report)
            self.assertIn("completion_wait_no_enqueue_share_pct", report)
            self.assertIn("completion_present_wait_overlap_share_pct", report)
            self.assertIn("completion_present_enqueue_while_waiting_per_present", report)
            self.assertIn("completion_wait_commit_chunk_entries_per_present", report)
            self.assertIn("completion_wait_commit_chunk_replay_starts_per_present", report)
            self.assertIn("completion_wait_commit_chunk_replay_ends_per_present", report)
            self.assertIn("completion_wait_commit_chunk_replay_cpu_ms_per_present", report)
            self.assertIn("chunk_publish_slot_residency_ms_per_present", report)
            self.assertIn("chunk_publish_slot_residency_p50_ms", report)
            self.assertIn("chunk_publish_slot_residency_p95_ms", report)
            self.assertIn("chunk_publish_slot_residency_present_ms_per_present", report)
            self.assertIn("chunk_publish_slot_residency_present_p50_ms", report)
            self.assertIn("chunk_publish_slot_residency_present_p95_ms", report)
            self.assertIn("chunk_publish_slot_residency_nonpresent_ms_per_present", report)
            self.assertIn("chunk_publish_slot_residency_nonpresent_p50_ms", report)
            self.assertIn("chunk_publish_slot_residency_nonpresent_p95_ms", report)
            self.assertIn(
                "chunk_publish_present_split_before_tail_draw_run_per_present",
                report,
            )
            self.assertIn(
                "chunk_publish_present_split_before_tail_draw_run_share_pct",
                report,
            )
            self.assertIn(
                "chunk_publish_present_split_before_draw_only_share_pct",
                report,
            )
            self.assertIn("chunk_publish_present_pre_present_opportunity_slots_per_present", report)
            self.assertIn("chunk_publish_present_pre_present_opportunity_commands_per_slot", report)
            self.assertIn("chunk_publish_present_pre_present_opportunity_draw_runs_per_slot", report)
            self.assertIn("chunk_publish_present_pre_present_opportunity_draw_items_per_slot", report)
            self.assertIn("chunk_publish_present_pre_present_opportunity_non_draw_commands_per_slot", report)
            self.assertIn("chunk_publish_present_pre_present_opportunity_payload_bytes_per_slot", report)
            self.assertIn("chunk_publish_present_pre_present_opportunity_residency_ms_per_present", report)
            self.assertIn(
                "chunk_publish_present_pre_present_opportunity_tail_draw_run_per_present",
                report,
            )
            self.assertIn(
                "chunk_publish_present_pre_present_opportunity_tail_draw_run_share_pct",
                report,
            )
            self.assertIn(
                "chunk_publish_present_pre_present_opportunity_draw_only_share_pct",
                report,
            )
            self.assertIn(
                "chunk_publish_present_pre_present_opportunity_residency_p50_ms",
                report,
            )
            self.assertIn(
                "chunk_publish_present_pre_present_opportunity_residency_p95_ms",
                report,
            )
            self.assertIn("completion_no_enqueue_first_publish_slot_samples", report)
            self.assertIn("completion_no_enqueue_first_publish_slot_commands", report)
            self.assertIn("completion_no_enqueue_first_publish_slot_draw_items", report)
            self.assertIn("completion_no_enqueue_first_publish_slot_payload_bytes", report)
            self.assertIn("no_enqueue_first_publish_slot_samples_per_present", report)
            self.assertIn("no_enqueue_first_publish_slot_commands_per_slot", report)
            self.assertIn("no_enqueue_first_publish_slot_draw_items_per_slot", report)
            self.assertIn("no_enqueue_first_publish_slot_payload_bytes_per_slot", report)
            self.assertIn("encode_ready_depth_avg", report)
            self.assertIn("encode_ready_depth_gt1_per_present", report)
            self.assertIn("encode_ready_depth_gt2_per_present", report)
            self.assertIn("encode_ready_depth_gt4_per_present", report)
            self.assertIn("encode_ready_depth_gt1_share_pct", report)
            self.assertIn("encode_ready_depth_gt2_share_pct", report)
            self.assertIn("encode_ready_depth_gt4_share_pct", report)
            self.assertIn("commit_chunk_replay_cpu_ms_per_present", report)
            self.assertIn("commit_chunk_queue_draw_submission_cpu_ms_per_present", report)
            self.assertIn("d3d9_snapshot_draw_submission_cpu_ms_per_present", report)
            self.assertIn("d3d9_snapshot_cache_lookup_cpu_ms_per_present", report)
            self.assertIn("snapshot_cache_uniform_build_cpu_ms_per_present", report)
            self.assertIn("snapshot_cache_uniform_hash_cpu_ms_per_present", report)
            self.assertIn("snapshot_cache_batch_miss_uniform_build_cpu_ms_per_present", report)
            self.assertIn("snapshot_cache_batch_miss_uniform_hash_cpu_ms_per_present", report)
            self.assertIn("snapshot_cache_batch_miss_vs_const_hash_cpu_ms_per_present", report)
            self.assertIn("snapshot_cache_batch_miss_ps_const_hash_cpu_ms_per_present", report)
            self.assertIn("snapshot_cache_batch_miss_nonconst_hash_cpu_ms_per_present", report)
            self.assertIn("snapshot_uniform_copy_cpu_ms_per_present", report)
            self.assertIn("submit_draw_run_batch_append_uniform_cpu_ms_per_present", report)
            self.assertIn("draw_uniform_payload_lookup_cpu_ms_per_present", report)
            self.assertIn("draw_uniform_payload_append_copy_cpu_ms_per_present", report)
            self.assertIn("snapshot_cache_batch_miss_uniform_hash_share_pct", report)
            self.assertIn("encode_chunk_cpu_ms_per_present", report)
            self.assertIn("argbuf_setup_cpu_ms_per_present", report)
            self.assertIn("argbuf_open_cpu_ms_per_present", report)
            self.assertIn("argbuf_cbuf_update_cpu_ms_per_present", report)
            self.assertIn("argbuf_cbuf_update_vs_cpu_ms_per_present", report)
            self.assertIn("argbuf_open_share_of_setup_pct", report)
            self.assertIn("argbuf_cbuf_update_share_of_setup_pct", report)
            self.assertIn("no_enqueue_stage_commit_entry_to_publish_ms_per_present", report)
            self.assertIn("no_enqueue_stage_publish_to_encode_dequeue_ms_per_present", report)
            self.assertIn(
                "no_enqueue_stage_encode_dequeue_to_command_buffer_commit_ms_per_present",
                report,
            )
            self.assertIn("no_enqueue_wait_to_next_enqueue_ms_per_present", report)
            self.assertIn("no_enqueue_before_publish_completed_replay_cpu_ms_per_present", report)
            self.assertIn("no_enqueue_before_publish_active_replay_cpu_ms_per_present", report)
            self.assertIn("no_enqueue_before_publish_inter_replay_gap_ms_per_present", report)
            self.assertIn("no_enqueue_before_publish_commit_publish_wait_ms_per_present", report)
            self.assertIn("no_enqueue_before_publish_on_before_publish_cpu_ms_per_present", report)
            self.assertIn("no_enqueue_before_publish_closure_ms_per_present", report)
            self.assertIn("no_enqueue_before_publish_residual_ms_per_present", report)
            self.assertIn("no_enqueue_before_publish_inter_replay_gap_share_pct", report)
            self.assertIn("no_enqueue_before_publish_closure_share_pct", report)
            self.assertIn("no_enqueue_before_publish_entries_per_present", report)
            self.assertIn("no_enqueue_before_publish_records_per_scanned_chunk", report)
            self.assertIn("no_enqueue_before_publish_draw_records_per_present", report)
            self.assertIn("draw_uniform_payload_append_bytes", report)
            self.assertIn("| `const_uploads_per_state_delta_break` | `2.000` | `2.000`", report)
            self.assertIn("| `const_upload_break_bytes_per_break` | `16.000` | `16.000`", report)
            self.assertIn("| `const_upload_registers_per_break` | `1.000` | `1.000`", report)
            self.assertIn("| `const_upload_subtype_coverage_pct` | `100.000` | `100.000`", report)
            self.assertIn("| `draw_submission_batch_records_per_submit` | `2.000` | `5.000`", report)
            self.assertIn("| `uniform_materialized_bytes_per_present` | `20,480.000` | `10,240.000`", report)
            self.assertIn("| `uniform_compact_candidate_bytes_per_present` | `10,240.000` | `5,120.000`", report)
            self.assertIn("| `uniform_compact_saved_bytes_per_present` | `10,240.000` | `5,120.000`", report)
            self.assertIn("| `uniform_compact_fixed_bytes_per_present` | `4,096.000` | `2,048.000`", report)
            self.assertIn("| `uniform_compact_vertex_bytes_per_present` | `2,048.000` | `1,024.000`", report)
            self.assertIn("| `uniform_compact_pixel_bytes_per_present` | `4,096.000` | `2,048.000`", report)
            self.assertIn("| `uniform_compact_candidate_share_of_materialized_bytes` | `50.000` | `50.000`", report)
            self.assertIn("| `uniform_compact_fixed_share_of_candidate_bytes` | `40.000` | `40.000`", report)
            self.assertIn("| `uniform_compact_vertex_share_of_candidate_bytes` | `20.000` | `20.000`", report)
            self.assertIn("| `uniform_compact_pixel_share_of_candidate_bytes` | `40.000` | `40.000`", report)
            self.assertIn("| `uniform_compact_saved_share_of_materialized_bytes` | `50.000` | `50.000`", report)
            self.assertIn("| `uniform_adjacent_same_fixed_payload_hash_share` | `80.000` | `80.000`", report)
            self.assertIn("| `uniform_adjacent_same_fixed_and_shader_const_hashes_share` | `30.000` | `20.000`", report)
            self.assertIn("| `uniform_append_bytes_per_present` | `10,256.000` | `5,128.000`", report)
            self.assertIn("| `uniform_fixed_append_bytes_per_present` | `4,096.000` | `2,048.000`", report)
            self.assertIn("| `uniform_vertex_constants_append_bytes_per_present` | `2,048.000` | `1,024.000`", report)
            self.assertIn("| `uniform_pixel_constants_append_bytes_per_present` | `1,024.000` | `512.000`", report)
            self.assertIn("| `uniform_stage_constants_append_bytes_per_present` | `3,072.000` | `1,536.000`", report)
            self.assertIn("| `uniform_vertex_append_amplification_vs_compact_vertex` | `1.000` | `1.000`", report)
            self.assertIn("| `uniform_pixel_append_amplification_vs_compact_pixel` | `0.250` | `0.250`", report)
            self.assertIn("| `uniform_stage_append_amplification_vs_compact_stage` | `0.500` | `0.500`", report)
            self.assertIn("| `uniform_append_bytes_per_append` | `10,256.000` | `10,256.000`", report)
            self.assertIn("| `uniform_payload_record_append_bytes_per_append` | `3,088.000` | `3,088.000`", report)
            self.assertIn("| `uniform_fixed_append_records_per_payload_append` | `0.500` | `1.000`", report)
            self.assertIn("| `uniform_vertex_constants_append_records_per_payload_append` | `0.500` | `1.000`", report)
            self.assertIn("| `uniform_pixel_constants_append_records_per_payload_append` | `0.500` | `1.000`", report)
            self.assertIn("| `uniform_backend_materialized_bytes_per_present` | `15,360.000` | `10,240.000`", report)
            self.assertIn("| `uniform_backend_materialize_cpu_ms_per_present` | `0.750` | `0.500`", report)
            self.assertIn("| `uniform_backend_materialized_bytes_per_call` | `10,240.000` | `10,240.000`", report)
            self.assertIn("| `uniform_backend_materialize_fallbacks_per_present` | `0.500` | `0.000`", report)
            self.assertIn("| `uniform_backend_materialize_draw_encoder_command_share_pct` | `33.333` | `0.000`", report)
            self.assertIn("| `uniform_backend_materialize_draw_encoder_command_bytes_per_present` | `5,120.000` | `0.000`", report)
            self.assertIn("| `uniform_backend_materialize_draw_encoder_command_cpu_ms_per_present` | `0.250` | `0.000`", report)
            self.assertIn("| `uniform_backend_materialize_draw_encoder_param_share_pct` | `66.667` | `0.000`", report)
            self.assertIn("| `uniform_backend_materialize_queue_observation_share_pct` | `0.000` | `0.000`", report)
            self.assertIn("| `uniform_fixed_append_bytes_share_of_append_bytes` | `39.938` | `39.938`", report)
            self.assertIn("| `uniform_vertex_constants_append_bytes_share_of_append_bytes` | `19.969` | `19.969`", report)
            self.assertIn("| `uniform_pixel_constants_append_bytes_share_of_append_bytes` | `9.984` | `9.984`", report)
            self.assertIn("| `uniform_snapshot_elision_share` | `20.000` | `50.000`", report)
            self.assertIn("| `completion_present_wait_ms_per_present` | `45.000` | `35.000`", report)
            self.assertIn("| `completion_wait_with_enqueue_ms_per_present` | `0.000` | `5.000`", report)
            self.assertIn("| `completion_wait_without_enqueue_ms_per_present` | `50.000` | `35.000`", report)
            self.assertIn("| `completion_wait_overlap_share_pct` | `0.000` | `12.500`", report)
            self.assertIn("| `completion_present_wait_overlap_share_pct` | `0.000` | `11.429`", report)
            self.assertIn(
                "| `completion_wait_commit_chunk_entries_per_present` | `0.000` | `1.500`",
                report,
            )
            self.assertIn(
                "| `completion_wait_commit_chunk_replay_starts_per_present` | `0.000` | `1.000`",
                report,
            )
            self.assertIn(
                "| `completion_wait_commit_chunk_replay_cpu_ms_per_present` | `0.000` | `3.000`",
                report,
            )
            self.assertIn(
                "| `chunk_publish_slot_residency_ms_per_present` | `10.000` | `5.000`",
                report,
            )
            self.assertIn(
                "| `chunk_publish_slot_residency_p50_ms` | `11.000` | `5.000`",
                report,
            )
            self.assertIn(
                "| `chunk_publish_slot_residency_p95_ms` | `19.000` | `9.000`",
                report,
            )
            self.assertIn(
                "| `chunk_publish_slot_residency_present_ms_per_present` | `9.000` | `3.000`",
                report,
            )
            self.assertIn(
                "| `chunk_publish_slot_residency_present_p50_ms` | `10.000` | `3.000`",
                report,
            )
            self.assertIn(
                "| `chunk_publish_slot_residency_present_p95_ms` | `17.000` | `5.000`",
                report,
            )
            self.assertIn(
                "| `chunk_publish_slot_residency_nonpresent_ms_per_present` | `1.000` | `2.000`",
                report,
            )
            self.assertIn(
                "| `chunk_publish_slot_residency_nonpresent_p50_ms` | `2.000` | `4.000`",
                report,
            )
            self.assertIn(
                "| `chunk_publish_slot_residency_nonpresent_p95_ms` | `2.000` | `4.000`",
                report,
            )
            self.assertIn(
                "| `chunk_publish_present_pre_present_opportunity_slots_per_present` | `1.000` | `1.000`",
                report,
            )
            self.assertIn(
                "| `chunk_publish_present_pre_present_opportunity_tail_slot_share_pct` | `100.000` | `50.000`",
                report,
            )
            self.assertIn(
                "| `chunk_publish_present_pre_present_opportunity_commands_per_slot` | `50.000` | `30.000`",
                report,
            )
            self.assertIn(
                "| `chunk_publish_present_pre_present_opportunity_draw_runs_per_slot` | `40.000` | `25.000`",
                report,
            )
            self.assertIn(
                "| `chunk_publish_present_pre_present_opportunity_draw_items_per_slot` | `80.000` | `60.000`",
                report,
            )
            self.assertIn(
                "| `chunk_publish_present_pre_present_opportunity_non_draw_commands_per_slot` | `10.000` | `5.000`",
                report,
            )
            self.assertIn(
                "| `chunk_publish_present_pre_present_opportunity_payload_bytes_per_slot` | `2,048.000` | `1,024.000`",
                report,
            )
            self.assertIn(
                "| `chunk_publish_present_pre_present_opportunity_residency_ms_per_present` | `9.000` | `3.000`",
                report,
            )
            self.assertIn(
                "| `chunk_publish_present_pre_present_opportunity_residency_p50_ms` | `10.000` | `3.000`",
                report,
            )
            self.assertIn(
                "| `chunk_publish_present_pre_present_opportunity_residency_p95_ms` | `17.000` | `5.000`",
                report,
            )
            self.assertIn(
                "| `chunk_publish_present_pre_present_opportunity_tail_draw_run_share_pct` | `100.000` | `50.000`",
                report,
            )
            self.assertIn(
                "| `chunk_publish_present_pre_present_opportunity_tail_clear_share_pct` | `0.000` | `50.000`",
                report,
            )
            self.assertIn(
                "| `chunk_publish_present_pre_present_opportunity_draw_only_share_pct` | `100.000` | `50.000`",
                report,
            )
            self.assertIn(
                "| `no_enqueue_first_publish_slot_samples_per_present` | `1.000` | `1.000`",
                report,
            )
            self.assertIn(
                "| `no_enqueue_first_publish_slot_commands_per_slot` | `50.000` | `30.000`",
                report,
            )
            self.assertIn(
                "| `no_enqueue_first_publish_slot_draw_items_per_slot` | `80.000` | `60.000`",
                report,
            )
            self.assertIn(
                "| `no_enqueue_first_publish_slot_payload_bytes_per_slot` | `2,048.000` | `1,024.000`",
                report,
            )
            self.assertIn(
                "| `completion_no_enqueue_first_publish_slot_commands_p50` | `50` | `30`",
                report,
            )
            self.assertIn("| `encode_ready_depth_avg` | `1.000` | `2.500`", report)
            self.assertIn("| `encode_ready_depth_gt1_per_present` | `0.000` | `2.000`", report)
            self.assertIn("| `encode_ready_depth_gt2_per_present` | `0.000` | `1.000`", report)
            self.assertIn("| `encode_ready_depth_gt4_per_present` | `0.000` | `0.500`", report)
            self.assertIn("| `encode_ready_depth_gt1_share_pct` | `0.000` | `40.000`", report)
            self.assertIn("| `encode_ready_depth_gt2_share_pct` | `0.000` | `20.000`", report)
            self.assertIn("| `encode_ready_depth_gt4_share_pct` | `0.000` | `10.000`", report)
            self.assertIn("| `commit_chunk_replay_cpu_ms_per_present` | `20.000` | `15.000`", report)
            self.assertIn("| `snapshot_cache_uniform_build_cpu_ms_per_present` | `4.000` | `3.000`", report)
            self.assertIn("| `snapshot_cache_uniform_hash_cpu_ms_per_present` | `2.000` | `1.250`", report)
            self.assertIn("| `snapshot_cache_batch_miss_uniform_hash_share_pct` | `50.000` | `37.500`", report)
            self.assertIn("| `submit_draw_run_batch_append_uniform_cpu_ms_per_present` | `1.500` | `1.000`", report)
            self.assertIn("| `encode_chunk_cpu_ms_per_present` | `25.000` | `20.000`", report)
            self.assertIn("| `argbuf_setup_cpu_ms_per_present` | `12.000` | `8.000`", report)
            self.assertIn("| `argbuf_open_cpu_ms_per_present` | `5.000` | `3.000`", report)
            self.assertIn("| `argbuf_cbuf_update_cpu_ms_per_present` | `6.000` | `4.000`", report)
            self.assertIn("| `argbuf_cbuf_update_vs_cpu_ms_per_present` | `4.000` | `2.000`", report)
            self.assertIn(
                "| `no_enqueue_stage_encode_dequeue_to_command_buffer_commit_ms_per_present` | `15.000` | `10.000`",
                report,
            )
            self.assertIn(
                "| `no_enqueue_before_publish_completed_replay_cpu_ms_per_present` | `3.000` | `2.000`",
                report,
            )
            self.assertIn(
                "| `no_enqueue_before_publish_inter_replay_gap_ms_per_present` | `5.000` | `2.500`",
                report,
            )
            self.assertIn(
                "| `no_enqueue_before_publish_closure_ms_per_present` | `9.500` | `5.500`",
                report,
            )
            self.assertIn(
                "| `no_enqueue_before_publish_residual_ms_per_present` | `0.500` | `0.500`",
                report,
            )
            self.assertIn(
                "| `no_enqueue_before_publish_closure_share_pct` | `95.000` | `91.667`",
                report,
            )
            self.assertIn(
                "| `no_enqueue_before_publish_records_per_scanned_chunk` | `8.000` | `8.000`",
                report,
            )

    def test_const_upload_byte_and_count_gates_fail(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write_result(root / "before", {
                "commit_chunk_draw_run_break_type_const_upload": 50,
                "commit_chunk_draw_run_break_type_const_upload_bytes": 800,
            })
            write_result(root / "after", {
                "commit_chunk_draw_run_break_type_const_upload": 70,
                "commit_chunk_draw_run_break_type_const_upload_bytes": 900,
            })

            result = self.run_compare(
                root,
                "--require-const-upload-break-bytes-decrease",
                "--max-const-upload-break-count-ratio",
                "1.10",
            )

            self.assertNotEqual(result.returncode, 0)
            self.assertIn(
                "commit_chunk_draw_run_break_type_const_upload_bytes did not decrease",
                result.stderr,
            )
            self.assertIn(
                "commit_chunk_draw_run_break_type_const_upload exceeded count ratio",
                result.stderr,
            )

    def test_uniform_byte_gates_fail_when_width_does_not_decrease(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write_result(root / "before", {
                "d3d9_snapshot_uniform_materialized_bytes": 40960,
                "draw_uniform_payload_append_bytes": 20512,
            })
            write_result(root / "after", {
                "d3d9_snapshot_uniform_materialized_bytes": 40960,
                "draw_uniform_payload_append_bytes": 30768,
            })

            result = self.run_compare(
                root,
                "--require-uniform-materialized-bytes-decrease",
                "--require-uniform-append-bytes-decrease",
            )

            self.assertNotEqual(result.returncode, 0)
            self.assertIn(
                "d3d9_snapshot_uniform_materialized_bytes did not decrease",
                result.stderr,
            )
            self.assertIn(
                "draw_uniform_payload_append_bytes did not decrease",
                result.stderr,
            )

    def test_uniform_compact_saved_gate_fails_when_no_opportunity(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write_result(root / "before", {
                "present_encoded": 2,
                "d3d9_snapshot_uniform_materialized_compact_saved_bytes": 2048,
            })
            write_result(root / "after", {
                "present_encoded": 2,
                "d3d9_snapshot_uniform_materialized_compact_saved_bytes": 0,
            })

            result = self.run_compare(root, *UNIFORM_COMPACT_COMPARE_FLAGS)

            self.assertNotEqual(result.returncode, 0)
            self.assertIn(
                "uniform_compact_saved_bytes_per_present stayed zero",
                result.stderr,
            )

    def test_completion_overlap_gates_fail_when_p4_does_not_move(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write_result(root / "before", {
                "completion_present_wait_ms": 90.0,
                "completion_wait_with_enqueue_ms": 10.0,
                "completion_wait_without_enqueue_ms": 80.0,
                "completion_present_wait_with_enqueue_ms": 9.0,
                "completion_present_wait_without_enqueue_ms": 81.0,
                "encode_dequeue_ready_depth_gt1": 3,
            })
            write_result(root / "after", {
                "completion_present_wait_ms": 91.0,
                "completion_wait_with_enqueue_ms": 5.0,
                "completion_wait_without_enqueue_ms": 85.0,
                "completion_present_wait_with_enqueue_ms": 5.0,
                "completion_present_wait_without_enqueue_ms": 86.0,
                "encode_dequeue_ready_depth_gt1": 3,
            })

            result = self.run_compare(
                root,
                "--require-completion-present-wait-decrease",
                "--require-completion-wait-with-enqueue-increase",
                "--require-completion-wait-without-enqueue-decrease",
                "--require-completion-present-wait-with-enqueue-increase",
                "--require-completion-present-wait-without-enqueue-decrease",
                "--require-encode-ready-depth-gt1-increase",
            )

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("completion_present_wait_ms did not decrease", result.stderr)
            self.assertIn("completion_wait_with_enqueue_ms did not increase", result.stderr)
            self.assertIn("completion_wait_without_enqueue_ms did not decrease", result.stderr)
            self.assertIn(
                "completion_present_wait_with_enqueue_ms did not increase",
                result.stderr,
            )
            self.assertIn(
                "completion_present_wait_without_enqueue_ms did not decrease",
                result.stderr,
            )
            self.assertIn(
                "encode_dequeue_ready_depth_gt1 did not increase",
                result.stderr,
            )

    def test_serial_stage_gates_fail_when_per_present_cost_does_not_decrease(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write_result(root / "before", {
                "present_encoded": 2,
                "commit_chunk_replay_cpu_ms": 40.0,
                "commit_chunk_queue_draw_submission_cpu_ms": 20.0,
                "d3d9_snapshot_draw_submission_cpu_ms": 18.0,
                "d3d9_snapshot_cache_lookup_cpu_ms": 10.0,
                "encode_chunk_cpu_ms": 50.0,
                "completion_no_enqueue_stage_commit_entry_to_publish_ms": 20.0,
                "completion_no_enqueue_stage_publish_to_encode_dequeue_ms": 8.0,
                "completion_no_enqueue_stage_encode_dequeue_to_command_buffer_commit_ms": 30.0,
                "completion_no_enqueue_wait_to_next_enqueue_ms": 60.0,
                "completion_no_enqueue_commit_chunk_completed_replay_cpu_before_publish_ms": 6.0,
                "completion_no_enqueue_commit_chunk_active_replay_cpu_before_publish_ms": 2.0,
                "completion_no_enqueue_commit_chunk_inter_replay_gap_before_publish_ms": 10.0,
                "completion_no_enqueue_commit_publish_wait_before_publish_ms": 1.0,
            })
            write_result(root / "after", {
                "present_encoded": 2,
                "commit_chunk_replay_cpu_ms": 42.0,
                "commit_chunk_queue_draw_submission_cpu_ms": 20.0,
                "d3d9_snapshot_draw_submission_cpu_ms": 19.0,
                "d3d9_snapshot_cache_lookup_cpu_ms": 10.5,
                "encode_chunk_cpu_ms": 55.0,
                "completion_no_enqueue_stage_commit_entry_to_publish_ms": 22.0,
                "completion_no_enqueue_stage_publish_to_encode_dequeue_ms": 9.0,
                "completion_no_enqueue_stage_encode_dequeue_to_command_buffer_commit_ms": 30.0,
                "completion_no_enqueue_wait_to_next_enqueue_ms": 61.0,
                "completion_no_enqueue_commit_chunk_completed_replay_cpu_before_publish_ms": 8.0,
                "completion_no_enqueue_commit_chunk_active_replay_cpu_before_publish_ms": 2.0,
                "completion_no_enqueue_commit_chunk_inter_replay_gap_before_publish_ms": 11.0,
                "completion_no_enqueue_commit_publish_wait_before_publish_ms": 1.0,
            })

            result = self.run_compare(
                root,
                "--require-commit-chunk-replay-cpu-per-present-decrease",
                "--require-queue-draw-submission-cpu-per-present-decrease",
                "--require-snapshot-cpu-per-present-decrease",
                "--require-snapshot-cache-lookup-cpu-per-present-decrease",
                "--require-encode-chunk-cpu-per-present-decrease",
                "--require-no-enqueue-commit-entry-to-publish-decrease",
                "--require-no-enqueue-publish-to-encode-dequeue-decrease",
                "--require-no-enqueue-encode-dequeue-to-commit-decrease",
                "--require-no-enqueue-wait-to-next-enqueue-decrease",
                "--require-no-enqueue-before-publish-closure-decrease",
                "--require-no-enqueue-before-publish-inter-replay-gap-decrease",
            )

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("commit_chunk_replay_cpu_ms_per_present did not decrease", result.stderr)
            self.assertIn(
                "commit_chunk_queue_draw_submission_cpu_ms_per_present did not decrease",
                result.stderr,
            )
            self.assertIn(
                "d3d9_snapshot_draw_submission_cpu_ms_per_present did not decrease",
                result.stderr,
            )
            self.assertIn(
                "d3d9_snapshot_cache_lookup_cpu_ms_per_present did not decrease",
                result.stderr,
            )
            self.assertIn("encode_chunk_cpu_ms_per_present did not decrease", result.stderr)
            self.assertIn(
                "no_enqueue_stage_commit_entry_to_publish_ms_per_present did not decrease",
                result.stderr,
            )
            self.assertIn(
                "no_enqueue_stage_publish_to_encode_dequeue_ms_per_present did not decrease",
                result.stderr,
            )
            self.assertIn(
                "no_enqueue_stage_encode_dequeue_to_command_buffer_commit_ms_per_present did not decrease",
                result.stderr,
            )
            self.assertIn(
                "no_enqueue_wait_to_next_enqueue_ms_per_present did not decrease",
                result.stderr,
            )
            self.assertIn(
                "no_enqueue_before_publish_closure_ms_per_present did not decrease",
                result.stderr,
            )
            self.assertIn(
                "no_enqueue_before_publish_inter_replay_gap_ms_per_present did not decrease",
                result.stderr,
            )

    def test_argbuf_owner_gates_fail_when_per_present_cost_does_not_decrease(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write_result(root / "before", {
                "present_encoded": 2,
                "encode_draw_argbuf_setup_cpu_ms": 20.0,
                "encode_draw_argbuf_open_cpu_ms": 8.0,
                "encode_draw_argbuf_cbuf_update_cpu_ms": 10.0,
                "encode_draw_argbuf_cbuf_update_vs_cpu_ms": 6.0,
            })
            write_result(root / "after", {
                "present_encoded": 2,
                "encode_draw_argbuf_setup_cpu_ms": 20.0,
                "encode_draw_argbuf_open_cpu_ms": 9.0,
                "encode_draw_argbuf_cbuf_update_cpu_ms": 11.0,
                "encode_draw_argbuf_cbuf_update_vs_cpu_ms": 6.0,
            })

            result = self.run_compare(
                root,
                "--require-argbuf-setup-cpu-per-present-decrease",
                "--require-argbuf-open-cpu-per-present-decrease",
                "--require-argbuf-cbuf-update-cpu-per-present-decrease",
                "--require-argbuf-cbuf-update-vs-cpu-per-present-decrease",
            )

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("argbuf_setup_cpu_ms_per_present did not decrease", result.stderr)
            self.assertIn("argbuf_open_cpu_ms_per_present did not decrease", result.stderr)
            self.assertIn("argbuf_cbuf_update_cpu_ms_per_present did not decrease", result.stderr)
            self.assertIn(
                "argbuf_cbuf_update_vs_cpu_ms_per_present did not decrease",
                result.stderr,
            )

    def test_uniform_owner_gates_fail_when_per_present_cost_does_not_decrease(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write_result(root / "before", {
                "present_encoded": 2,
                "d3d9_snapshot_cache_uniform_build_cpu_ms": 8.0,
                "d3d9_snapshot_cache_uniform_hash_cpu_ms": 4.0,
                "d3d9_snapshot_cache_batch_miss_uniform_build_cpu_ms": 6.0,
                "d3d9_snapshot_cache_batch_miss_uniform_build_hash_cpu_ms": 3.0,
                "d3d9_snapshot_cache_batch_miss_uniform_build_vs_const_hash_cpu_ms": 2.0,
                "d3d9_snapshot_cache_batch_miss_uniform_build_ps_const_hash_cpu_ms": 0.5,
                "d3d9_snapshot_cache_batch_miss_uniform_build_nonconst_hash_cpu_ms": 0.5,
                "d3d9_snapshot_uniform_copy_cpu_ms": 2.0,
                "submit_draw_run_batch_append_uniform_cpu_ms": 3.0,
                "draw_uniform_payload_lookup_cpu_ms": 1.0,
                "draw_uniform_payload_append_copy_cpu_ms": 2.0,
            })
            write_result(root / "after", {
                "present_encoded": 2,
                "d3d9_snapshot_cache_uniform_build_cpu_ms": 8.0,
                "d3d9_snapshot_cache_uniform_hash_cpu_ms": 4.5,
                "d3d9_snapshot_cache_batch_miss_uniform_build_cpu_ms": 6.5,
                "d3d9_snapshot_cache_batch_miss_uniform_build_hash_cpu_ms": 3.0,
                "d3d9_snapshot_cache_batch_miss_uniform_build_vs_const_hash_cpu_ms": 2.5,
                "d3d9_snapshot_cache_batch_miss_uniform_build_ps_const_hash_cpu_ms": 0.5,
                "d3d9_snapshot_cache_batch_miss_uniform_build_nonconst_hash_cpu_ms": 0.75,
                "d3d9_snapshot_uniform_copy_cpu_ms": 2.0,
                "submit_draw_run_batch_append_uniform_cpu_ms": 3.2,
                "draw_uniform_payload_lookup_cpu_ms": 1.0,
                "draw_uniform_payload_append_copy_cpu_ms": 2.4,
            })

            result = self.run_compare(root, *UNIFORM_OWNER_COMPARE_FLAGS)

            self.assertNotEqual(result.returncode, 0)
            self.assertIn(
                "snapshot_cache_uniform_build_cpu_ms_per_present did not decrease",
                result.stderr,
            )
            self.assertIn(
                "snapshot_cache_uniform_hash_cpu_ms_per_present did not decrease",
                result.stderr,
            )
            self.assertIn(
                "snapshot_cache_batch_miss_uniform_build_cpu_ms_per_present did not decrease",
                result.stderr,
            )
            self.assertIn(
                "snapshot_cache_batch_miss_uniform_hash_cpu_ms_per_present did not decrease",
                result.stderr,
            )
            self.assertIn(
                "snapshot_cache_batch_miss_vs_const_hash_cpu_ms_per_present did not decrease",
                result.stderr,
            )
            self.assertIn(
                "snapshot_cache_batch_miss_ps_const_hash_cpu_ms_per_present did not decrease",
                result.stderr,
            )
            self.assertIn(
                "snapshot_cache_batch_miss_nonconst_hash_cpu_ms_per_present did not decrease",
                result.stderr,
            )
            self.assertIn(
                "snapshot_uniform_copy_cpu_ms_per_present did not decrease",
                result.stderr,
            )
            self.assertIn(
                "submit_draw_run_batch_append_uniform_cpu_ms_per_present did not decrease",
                result.stderr,
            )
            self.assertIn(
                "draw_uniform_payload_lookup_cpu_ms_per_present did not decrease",
                result.stderr,
            )
            self.assertIn(
                "draw_uniform_payload_append_copy_cpu_ms_per_present did not decrease",
                result.stderr,
            )

    def test_state_elision_metrics_are_reported(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write_result(root / "before", {
                "present_encoded": 2,
                "d3d9_snapshot_state_copy_cpu_ms": 4.0,
                "d3d9_snapshot_state_materialized": 10,
                "d3d9_snapshot_state_materialized_bytes": 10 * 1024 * 1024,
                "d3d9_snapshot_state_elided": 0,
                "d3d9_snapshot_state_elided_bytes": 0,
                "submit_draw_run_batch_discarded_state_records": 8,
                "submit_draw_run_batch_discarded_state_bytes": 8 * 1024 * 1024,
                "submit_draw_run_batch_submission_adjacent_same_generation_lane": 9,
                "submit_draw_run_batch_compat_same_generation_lane": 8,
                "submit_draw_run_batch_compat_same_generation_lane_compatible": 8,
                "submit_draw_run_batch_compat_same_generation_lane_incompatible": 0,
            })
            write_result(root / "after", {
                "present_encoded": 2,
                "d3d9_snapshot_state_copy_cpu_ms": 2.0,
                "d3d9_snapshot_state_materialized": 5,
                "d3d9_snapshot_state_materialized_bytes": 5 * 1024 * 1024,
                "d3d9_snapshot_state_elided": 5,
                "d3d9_snapshot_state_elided_bytes": 5 * 1024 * 1024,
                "submit_draw_run_batch_discarded_state_records": 2,
                "submit_draw_run_batch_discarded_state_bytes": 2 * 1024 * 1024,
                "submit_draw_run_batch_submission_adjacent_same_generation_lane": 6,
                "submit_draw_run_batch_compat_same_generation_lane": 5,
                "submit_draw_run_batch_compat_same_generation_lane_compatible": 5,
                "submit_draw_run_batch_compat_same_generation_lane_incompatible": 0,
            })

            result = self.run_compare(root)

            self.assertEqual(result.returncode, 0, result.stderr)
            report = root.joinpath("comparison.md").read_text(encoding="utf-8")
            self.assertIn("`snapshot_state_elided_mib_per_present`", report)
            self.assertIn("`snapshot_state_elision_share_pct`", report)
            self.assertIn(
                "`submit_draw_run_batch_discarded_state_mib_per_present`",
                report,
            )

    def test_state_elision_gates_fail_on_missing_elision_or_discarded_growth(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write_result(root / "before", {
                "present_encoded": 2,
                "d3d9_snapshot_state_elided": 8,
                "d3d9_snapshot_state_elided_bytes": 8 * 1024 * 1024,
                "submit_draw_run_batch_discarded_state_records": 2,
                "submit_draw_run_batch_discarded_state_bytes": 2 * 1024 * 1024,
            })
            write_result(root / "after", {
                "present_encoded": 2,
                "d3d9_snapshot_state_elided": 0,
                "d3d9_snapshot_state_elided_bytes": 0,
                "submit_draw_run_batch_discarded_state_records": 4,
                "submit_draw_run_batch_discarded_state_bytes": 4 * 1024 * 1024,
            })

            result = self.run_compare(root, *STATE_ELISION_COMPARE_FLAGS)

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("snapshot_state_elided_per_present stayed zero", result.stderr)
            self.assertIn(
                "submit_draw_run_batch_discarded_state_records_per_present increased",
                result.stderr,
            )
            self.assertIn(
                "submit_draw_run_batch_discarded_state_mib_per_present increased",
                result.stderr,
            )

    def test_submission_carrier_metrics_are_reported(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write_result(root / "before", {
                "present_encoded": 2,
                "d3d9_snapshot_submission_carrier_records": 4,
                "d3d9_snapshot_submission_carrier_bytes": 400,
                "d3d9_snapshot_submission_carrier_state_storage_bytes": 160,
                "d3d9_snapshot_submission_carrier_uniform_storage_bytes": 200,
                "d3d9_snapshot_submission_carrier_compact_uniform_storage_bytes": 40,
                "d3d9_snapshot_submission_carrier_unused_uniform_storage_records": 2,
                "d3d9_snapshot_submission_carrier_unused_uniform_storage_bytes": 100,
            })
            write_result(root / "after", {
                "present_encoded": 2,
                "d3d9_snapshot_submission_carrier_records": 4,
                "d3d9_snapshot_submission_carrier_bytes": 240,
                "d3d9_snapshot_submission_carrier_state_storage_bytes": 160,
                "d3d9_snapshot_submission_carrier_uniform_storage_bytes": 40,
                "d3d9_snapshot_submission_carrier_compact_uniform_storage_bytes": 40,
                "d3d9_snapshot_submission_carrier_unused_uniform_storage_records": 4,
                "d3d9_snapshot_submission_carrier_unused_uniform_storage_bytes": 40,
            })

            result = self.run_compare(root)

            self.assertEqual(result.returncode, 0, result.stderr)
            report = root.joinpath("comparison.md").read_text(encoding="utf-8")
            self.assertIn(
                "| `submission_carrier_bytes_per_record` | `100.000` | `60.000`",
                report,
            )
            self.assertIn(
                "| `submission_carrier_uniform_storage_bytes_per_record` | `50.000` | `10.000`",
                report,
            )
            self.assertIn(
                "| `submission_carrier_unused_uniform_storage_records_per_present` | `1.000` | `2.000`",
                report,
            )
            self.assertIn(
                "| `submission_carrier_unused_uniform_storage_bytes_per_record` | `25.000` | `10.000`",
                report,
            )
            self.assertIn(
                "| `submission_carrier_unused_uniform_storage_bytes_per_unused_record` | `50.000` | `10.000`",
                report,
            )
            self.assertIn(
                "| `submission_carrier_unused_uniform_storage_share_pct` | `50.000` | `100.000`",
                report,
            )
            self.assertIn(
                "Submission carrier width changed by `-40 bytes/record`",
                report,
            )
            self.assertIn(
                "Submission carrier unused full-uniform storage changed by",
                report,
            )

    def test_submission_carrier_gates_fail_when_width_does_not_decrease(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write_result(root / "before", {
                "present_encoded": 2,
                "d3d9_snapshot_submission_carrier_records": 4,
                "d3d9_snapshot_submission_carrier_bytes": 400,
                "d3d9_snapshot_submission_carrier_uniform_storage_bytes": 200,
            })
            write_result(root / "after", {
                "present_encoded": 2,
                "d3d9_snapshot_submission_carrier_records": 4,
                "d3d9_snapshot_submission_carrier_bytes": 400,
                "d3d9_snapshot_submission_carrier_uniform_storage_bytes": 220,
            })

            result = self.run_compare(root, *CARRIER_COMPARE_FLAGS)

            self.assertNotEqual(result.returncode, 0)
            self.assertIn(
                "submission_carrier_bytes_per_record did not decrease",
                result.stderr,
            )
            self.assertIn(
                "submission_carrier_uniform_storage_bytes_per_record did not decrease",
                result.stderr,
            )

    def test_submission_carrier_gates_fail_when_counters_are_missing(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write_result(root / "before", {"present_encoded": 2})
            write_result(root / "after", {"present_encoded": 2})

            result = self.run_compare(root, *CARRIER_COMPARE_FLAGS)

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("submission_carrier_bytes_per_record is missing", result.stderr)
            self.assertIn(
                "submission_carrier_uniform_storage_bytes_per_record is missing",
                result.stderr,
            )

    def test_overlap_locality_gates_fail_when_publish_fragmentation_increases(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write_result(root / "before", {
                "present_encoded": 2,
                "command_buffers": 10,
                "render_pass_begin": 20,
                "render_pass_tile_preservation_bytes": 8 * 1024 * 1024,
            })
            write_result(root / "after", {
                "present_encoded": 2,
                "command_buffers": 12,
                "render_pass_begin": 22,
                "render_pass_tile_preservation_bytes": 9 * 1024 * 1024,
            })

            result = self.run_compare(
                root,
                "--require-command-buffers-per-present-not-increase",
                "--require-render-passes-per-present-not-increase",
                "--require-tile-preservation-not-increase",
            )

            self.assertNotEqual(result.returncode, 0)
            self.assertIn(
                "command_buffers_per_present increased",
                result.stderr,
            )
            self.assertIn("passes_per_present increased", result.stderr)
            self.assertIn("tile_preservation_mib increased", result.stderr)

    def test_overlap_locality_gates_pass_when_publish_shape_is_preserved(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write_result(root / "before", {
                "present_encoded": 2,
                "command_buffers": 10,
                "render_pass_begin": 20,
                "render_pass_tile_preservation_bytes": 8 * 1024 * 1024,
            })
            write_result(root / "after", {
                "present_encoded": 4,
                "command_buffers": 20,
                "render_pass_begin": 40,
                "render_pass_tile_preservation_bytes": 8 * 1024 * 1024,
            })

            result = self.run_compare(
                root,
                "--require-command-buffers-per-present-not-increase",
                "--require-render-passes-per-present-not-increase",
                "--require-tile-preservation-not-increase",
            )

            self.assertEqual(result.returncode, 0, result.stderr)

    def test_encoder_sidecar_metrics_are_reported_when_csv_exists(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write_result(root / "before", {"present_encoded": 2})
            write_encoder_csv(root / "before", [
                {
                    "end_reason": "rt_change",
                    "color_load_bytes": 1024 * 1024,
                    "color_store_bytes": 2 * 1024 * 1024,
                    "depth_load_bytes": 3 * 1024 * 1024,
                    "depth_store_bytes": 4 * 1024 * 1024,
                },
                {
                    "end_reason": "present",
                    "color_load_bytes": 0,
                    "color_store_bytes": 2 * 1024 * 1024,
                    "depth_load_bytes": 0,
                    "depth_store_bytes": 4 * 1024 * 1024,
                },
            ])
            write_result(root / "after", {"present_encoded": 4})
            write_encoder_csv(root / "after", [
                {
                    "end_reason": "rt_change",
                    "color_load_bytes": 1024 * 1024,
                    "color_store_bytes": 1024 * 1024,
                    "depth_load_bytes": 1024 * 1024,
                    "depth_store_bytes": 1024 * 1024,
                },
                {
                    "end_reason": "final",
                    "color_load_bytes": 3 * 1024 * 1024,
                    "color_store_bytes": 1024 * 1024,
                    "depth_load_bytes": 5 * 1024 * 1024,
                    "depth_store_bytes": 1024 * 1024,
                },
            ])

            result = self.run_compare(root)

            self.assertEqual(result.returncode, 0, result.stderr)
            report = root.joinpath("comparison.md").read_text(encoding="utf-8")
            self.assertIn(
                "| `encoder_sidecar_final_end_reason_per_present` | `0.000` | `0.250`",
                report,
            )
            self.assertIn(
                "| `encoder_sidecar_color_load_mib_per_present` | `0.500` | `1.000`",
                report,
            )
            self.assertIn(
                "| `encoder_sidecar_depth_load_mib_per_present` | `1.500` | `1.500`",
                report,
            )

    def test_encoder_sidecar_metrics_are_reported_for_result_json_inputs(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write_result(root / "before", {"present_encoded": 2})
            write_encoder_csv(root / "before", [
                {
                    "end_reason": "rt_change",
                    "color_load_bytes": 1024 * 1024,
                    "depth_load_bytes": 2 * 1024 * 1024,
                },
            ])
            write_result(root / "after", {"present_encoded": 2})
            write_encoder_csv(root / "after", [
                {
                    "end_reason": "final",
                    "color_load_bytes": 2 * 1024 * 1024,
                    "depth_load_bytes": 3 * 1024 * 1024,
                },
            ])

            output = root / "comparison.md"
            result = self.run_compare(
                root,
                "--before",
                str(root / "before" / "result.json"),
                "--after",
                str(root / "after" / "result.json"),
                "--output",
                str(output),
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            report = output.read_text(encoding="utf-8")
            self.assertIn(
                "| `encoder_sidecar_final_end_reason_per_present` | `0.000` | `0.500`",
                report,
            )
            self.assertIn(
                "| `encoder_sidecar_color_load_mib_per_present` | `0.500` | `1.000`",
                report,
            )
            self.assertIn(
                "| `encoder_sidecar_depth_load_mib_per_present` | `1.000` | `1.500`",
                report,
            )

    def test_encoder_sidecar_gates_fail_when_csv_is_missing(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write_result(root / "before", {"present_encoded": 2})
            write_result(root / "after", {"present_encoded": 2})

            result = self.run_compare(
                root,
                "--require-encoder-final-end-reason-not-increase",
                "--require-encoder-color-load-not-increase",
                "--require-encoder-depth-load-not-increase",
            )

            self.assertNotEqual(result.returncode, 0)
            self.assertIn(
                "encoder_sidecar_final_end_reason_per_present is missing",
                result.stderr,
            )
            self.assertIn(
                "encoder_sidecar_color_load_mib_per_present is missing",
                result.stderr,
            )
            self.assertIn(
                "encoder_sidecar_depth_load_mib_per_present is missing",
                result.stderr,
            )

    def test_encoder_sidecar_gates_fail_when_csv_is_empty(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write_result(root / "before", {"present_encoded": 2})
            write_encoder_csv(root / "before", [])
            write_result(root / "after", {"present_encoded": 2})
            write_encoder_csv(root / "after", [])

            result = self.run_compare(
                root,
                "--require-encoder-final-end-reason-not-increase",
                "--require-encoder-color-load-not-increase",
                "--require-encoder-depth-load-not-increase",
            )

            self.assertNotEqual(result.returncode, 0)
            self.assertIn(
                "encoder_sidecar_final_end_reason_per_present is missing",
                result.stderr,
            )
            self.assertIn(
                "encoder_sidecar_color_load_mib_per_present is missing",
                result.stderr,
            )
            self.assertIn(
                "encoder_sidecar_depth_load_mib_per_present is missing",
                result.stderr,
            )

    def test_encoder_sidecar_gates_fail_when_final_or_load_increases(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write_result(root / "before", {"present_encoded": 2})
            write_encoder_csv(root / "before", [
                {
                    "end_reason": "rt_change",
                    "color_load_bytes": 1024 * 1024,
                    "depth_load_bytes": 1024 * 1024,
                },
            ])
            write_result(root / "after", {"present_encoded": 2})
            write_encoder_csv(root / "after", [
                {
                    "end_reason": "rt_change",
                    "color_load_bytes": 1024 * 1024,
                    "depth_load_bytes": 1024 * 1024,
                },
                {
                    "end_reason": "final",
                    "color_load_bytes": 2 * 1024 * 1024,
                    "depth_load_bytes": 3 * 1024 * 1024,
                },
            ])

            result = self.run_compare(
                root,
                "--require-encoder-final-end-reason-not-increase",
                "--require-encoder-color-load-not-increase",
                "--require-encoder-depth-load-not-increase",
            )

            self.assertNotEqual(result.returncode, 0)
            self.assertIn(
                "encoder_sidecar_final_end_reason_per_present increased",
                result.stderr,
            )
            self.assertIn(
                "encoder_sidecar_color_load_mib_per_present increased",
                result.stderr,
            )
            self.assertIn(
                "encoder_sidecar_depth_load_mib_per_present increased",
                result.stderr,
            )

    def test_output_parent_directory_is_created(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write_result(root / "before", {
                "commit_chunk_draw_run_submits": 1,
                "commit_chunk_draw_run_records": 1,
            })
            write_result(root / "after", {
                "commit_chunk_draw_run_submits": 1,
                "commit_chunk_draw_run_records": 2,
            })

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    str(root / "before"),
                    str(root / "after"),
                    "--output",
                    str(root / "nested" / "reports" / "comparison.md"),
                ],
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue(root.joinpath("nested", "reports", "comparison.md").exists())

    def test_mechanism_gates_fail_when_counters_do_not_move(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write_result(root / "before", {
                "commit_chunk_draw_run_submits": 10,
                "commit_chunk_draw_run_records": 20,
                "commit_chunk_draw_run_binding_override_records": 0,
                "commit_chunk_draw_batch_const_upload_passthrough": 0,
                "encode_draw_cpu_ms": 100.0,
            })
            write_result(root / "after", {
                "commit_chunk_draw_run_submits": 10,
                "commit_chunk_draw_run_records": 20,
                "commit_chunk_draw_run_binding_override_records": 0,
                "commit_chunk_draw_batch_const_upload_passthrough": 0,
                "encode_draw_cpu_ms": 100.0,
            })

            result = self.run_compare(
                root,
                "--require-draw-run-records-increase",
                "--require-draw-run-records-per-submit-increase",
                "--require-binding-overrides-present",
                "--require-const-upload-passthrough-present",
                "--require-draw-submission-batch-present",
                "--require-encode-draw-cpu-decrease",
            )

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("commit_chunk_draw_run_records did not increase", result.stderr)
            self.assertIn("draw_run_records_per_submit did not increase", result.stderr)
            self.assertIn("binding_override_records stayed zero", result.stderr)
            self.assertIn("const_upload_passthrough stayed zero", result.stderr)
            self.assertIn("commit_chunk_draw_submission_batch counters stayed zero", result.stderr)
            self.assertIn("encode_draw_cpu_ms did not decrease", result.stderr)

    def test_draw_submission_batch_gate_fails_when_counters_are_zero(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write_result(root / "before", {})
            write_result(root / "after", {
                "commit_chunk_draw_submission_batch_submits": 0,
                "commit_chunk_draw_submission_batch_records": 0,
                "commit_chunk_draw_submission_batch_max_records": 0,
            })

            result = self.run_compare(
                root,
                "--require-draw-submission-batch-present",
            )

            self.assertNotEqual(result.returncode, 0)
            self.assertIn(
                "commit_chunk_draw_submission_batch counters stayed zero",
                result.stderr,
            )


if __name__ == "__main__":
    unittest.main()
