#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)

frame=${DXMT_3DMARK05_PROBE_FRAME:-60}
timeout=${DXMT_3DMARK05_PROBE_TIMEOUT:-}
timeout_slack=${DXMT_3DMARK05_PROBE_TIMEOUT_SLACK:-45}
wait_unlocked_sec=${DXMT_3DMARK05_WAIT_UNLOCKED_SEC:-0}
wait_unlocked_interval_sec=${DXMT_3DMARK05_WAIT_UNLOCKED_INTERVAL_SEC:-5}
keep_frontmost=${DXMT_3DMARK05_KEEP_FRONTMOST:-0}
keep_frontmost_interval_sec=${DXMT_3DMARK05_KEEP_FRONTMOST_INTERVAL_SEC:-1}
keep_frontmost_process=${DXMT_3DMARK05_KEEP_FRONTMOST_PROCESS:-3DMark05.exe}
capture_delay_sec=${DXMT_3DMARK05_CAPTURE_DELAY_SEC:-}
catalogue_capture_delay_sec=$(python3 - "$repo_root/experiments/CATALOGUE.toml" <<'PY'
import sys
import tomllib
from pathlib import Path

with Path(sys.argv[1]).open("rb") as handle:
    catalogue = tomllib.load(handle)
for app in catalogue.get("app", []):
    if app.get("name") == "app-d3d9-3dmark05":
        print(f"{float(app.get('capture_delay_sec', 70.0)):g}")
        break
else:
    print("70")
PY
)
capture_frames=${DXMT_3DMARK05_CAPTURE_FRAMES:-}
capture_range=${DXMT_3DMARK05_CAPTURE_RANGE:-}
capture_dir=${DXMT_3DMARK05_CAPTURE_DIR:-}
suffix=${DXMT_3DMARK05_PROBE_SUFFIX:-}
result_file=${DXMT_3DMARK05_RESULT_FILE:-dxmt9_gt1.3dr}
capture_gputrace=1
dry_run=0
dump_shaders=0
frame_sampling=${DXMT9_PERF_FRAME_SAMPLING:-0}
draw_packet_actual_change=${DXMT9_PERF_DRAW_PACKET_ACTUAL_CHANGE:-0}
vs_const_setter_range=${DXMT9_PERF_VS_CONST_SETTER_RANGE:-0}
pe_recorder_stats=${DXMT9_PE_RECORDER_STATS:-0}
pe_recorder_chunk_log=${DXMT9_PE_RECORDER_CHUNK_LOG:-0}
pe_flush_after_clear=${DXMT9_PE_FLUSH_AFTER_CLEAR:-0}
pe_flush_after_draw=${DXMT9_PE_FLUSH_AFTER_DRAW:-0}
pe_draw_full_snapshot=${DXMT9_PE_DRAW_FULL_SNAPSHOT:-0}
pe_chunk_max_records=${DXMT9_PE_CHUNK_MAX_RECORDS:-}
pe_chunk_max_bytes=${DXMT9_PE_CHUNK_MAX_BYTES:-}
dxmt_log_level=${DXMT_LOG_LEVEL:-}
open_cb_preencode_tail_present=${DXMT9_OPEN_CB_PREENCODE_TAIL_PRESENT:-0}
open_cb_carry_render_session=${DXMT9_OPEN_CB_CARRY_RENDER_SESSION:-0}
open_cb_semantic_boundary_publish=${DXMT9_OPEN_CB_SEMANTIC_BOUNDARY_PUBLISH:-0}
open_cb_cpu_ready_command_limit=${DXMT9_OPEN_CB_CPU_READY_COMMAND_LIMIT:-}
open_cb_writer_active_cpu_ready_publish=${DXMT9_OPEN_CB_WRITER_ACTIVE_CPU_READY_PUBLISH:-0}
open_cb_active_wait_cpu_ready_append=${DXMT9_OPEN_CB_ACTIVE_WAIT_CPU_READY_APPEND:-0}
open_cb_wait_start_cpu_ready_publish=${DXMT9_OPEN_CB_WAIT_START_CPU_READY_PUBLISH:-0}
open_cb_semantic_boundary_release_mode=${DXMT9_OPEN_CB_SEMANTIC_BOUNDARY_RELEASE_MODE:-}
open_cb_pending_tail_wait_us=${DXMT9_OPEN_CB_PENDING_TAIL_WAIT_US:-}
stage_pre_present_command_limit=${DXMT9_STAGE_PRE_PRESENT_COMMAND_LIMIT:-}
draw_chunk_command_limit=${DXMT9_DRAW_CHUNK_COMMAND_LIMIT:-}
enable_chunk_end_carry=${DXMT9_ENABLE_CHUNK_END_CARRY:-0}
recommended_gputrace_min_free_mb=2048
trim_unused_varyings=0
trim_unused_varyings_vs_hashes=
trim_unused_varyings_ps_hashes=
drop_vsout_point_size=0
probe_position_only_vsout=0
probe_half_vsout=0
force_fragment_color=0
trim_vertex_temps=0
trim_vs_output_scratch=0
split_sparse_const_records=0
suppress_rt_pixel_format_view=0
suppress_x8_rt_pixel_format_view=0
x8_shader_alpha_fill=0
native_metal_base_vertex=0
optimize_screen_blend_index_order=0
optimize_screen_blend_index_order_row=
optimize_screen_blend_index_order_rows=
optimize_screen_blend_index_order_class=
optimize_screen_blend_index_order_classes=
optimize_screen_blend_index_order_stream0_span_min=
optimize_screen_blend_index_cache=0
optimize_screen_blend_index_cache_min_gain_pct=
split_large_indexed_draws=
split_large_indexed_draws_stream0_span_max=
split_large_indexed_draws_max_chunks_per_draw=
split_large_indexed_draws_row=
split_large_indexed_draws_rows=
split_large_indexed_draws_class=
split_large_indexed_draws_classes=
force_expand_indexed=0
probe_force_expand_indexed=0
probe_force_expand_indexed_row=
probe_force_expand_indexed_rows=
probe_force_expand_indexed_class=
probe_force_expand_indexed_classes=
probe_reverse_indexed_triangles=0
probe_reverse_opaque_indexed_triangles=0
probe_reverse_nonopaque_indexed_triangles=0
probe_sort_indexed_triangles_by_min_index=0
probe_optimize_indexed_triangles_vertex_cache=0
optimize_opaque_depth_index_cache=0
optimize_opaque_depth_index_cache_min_gain_pct=
index_cache_candidate_frontier_cap=
index_cache_candidate_lazy_frontier=0
index_cache_candidate_bucketed_select=0
index_cache_candidate_strict_lru=0
index_cache_candidate_upper_bound_gate=0
probe_apply_index_cache_opt_candidate=0
probe_apply_index_cache_opt_candidate_unsafe_nonopaque=0
probe_apply_index_cache_opt_candidate_min_gain_pct=
probe_reverse_indexed_triangles_row=
probe_reverse_indexed_triangles_rows=
probe_reverse_indexed_triangles_class=
probe_reverse_indexed_triangles_classes=
probe_reverse_indexed_triangles_stream0_span_min=
probe_indexed_triangle_encoder_draw_min=
probe_indexed_triangle_encoder_draw_max=
probe_indexed_triangle_encoder_draw_exclude=
probe_scissor_rect=
probe_scissor_rect_row=
probe_scissor_rect_rows=
probe_scissor_rect_class=
probe_scissor_rect_classes=
probe_force_cull_mode=
probe_force_cull_mode_row=
probe_force_cull_mode_rows=
probe_force_cull_mode_class=
probe_force_cull_mode_classes=
force_cull_mode=
measure_index_reuse=0
measure_index_cache_opt_candidate=0
dump_indexed_geometry=0
dump_indexed_geometry_cbufs=0
dump_indexed_geometry_max_draws=${DXMT9_DUMP_INDEXED_GEOMETRY_MAX_DRAWS:-16}
dump_indexed_geometry_vs=
dump_indexed_geometry_ps=
dump_indexed_geometry_texture0=
dump_indexed_geometry_texture0_width=
dump_indexed_geometry_texture0_height=
dump_indexed_geometry_texture0_format=
dump_depth_attachment_handle=${DXMT9_DUMP_DEPTH_ATTACHMENT_HANDLE:-}
dump_depth_attachment_seq=${DXMT9_DUMP_DEPTH_ATTACHMENT_SEQ:-}
dump_depth_attachment_enc=${DXMT9_DUMP_DEPTH_ATTACHMENT_ENC:-}
dump_depth_attachment_path=${DXMT9_DUMP_DEPTH_ATTACHMENT_PATH:-}
dump_color_attachment_after_draw=${DXMT9_DUMP_COLOR_ATTACHMENT_AFTER_DRAW:-0}
dump_color_attachment_handle=${DXMT9_DUMP_COLOR_ATTACHMENT_HANDLE:-}
dump_color_attachment_index=${DXMT9_DUMP_COLOR_ATTACHMENT_INDEX:-}
dump_color_attachment_seq=${DXMT9_DUMP_COLOR_ATTACHMENT_SEQ:-}
dump_color_attachment_enc=${DXMT9_DUMP_COLOR_ATTACHMENT_ENC:-}
dump_color_attachment_draw=${DXMT9_DUMP_COLOR_ATTACHMENT_DRAW:-}
dump_color_attachment_draws=${DXMT9_DUMP_COLOR_ATTACHMENT_DRAWS:-}
dump_color_attachment_command_index=${DXMT9_DUMP_COLOR_ATTACHMENT_COMMAND_INDEX:-}
dump_color_attachment_command_index_min=${DXMT9_DUMP_COLOR_ATTACHMENT_COMMAND_INDEX_MIN:-}
dump_color_attachment_command_index_max=${DXMT9_DUMP_COLOR_ATTACHMENT_COMMAND_INDEX_MAX:-}
dump_color_attachment_texture0=${DXMT9_DUMP_COLOR_ATTACHMENT_TEXTURE0:-}
dump_color_attachment_texture0s=${DXMT9_DUMP_COLOR_ATTACHMENT_TEXTURE0S:-}
dump_color_attachment_path=${DXMT9_DUMP_COLOR_ATTACHMENT_PATH:-}
dump_color_attachment_dir=${DXMT9_DUMP_COLOR_ATTACHMENT_DIR:-}
dump_color_attachment_roi_summary_path=${DXMT9_DUMP_COLOR_ATTACHMENT_ROI_SUMMARY_PATH:-}
dump_color_attachment_rois=${DXMT9_DUMP_COLOR_ATTACHMENT_ROIS:-}
dump_color_attachment_bright_threshold=${DXMT9_DUMP_COLOR_ATTACHMENT_BRIGHT_THRESHOLD:-}
dump_color_attachment_white_threshold=${DXMT9_DUMP_COLOR_ATTACHMENT_WHITE_THRESHOLD:-}
dump_color_attachment_warm_red_threshold=${DXMT9_DUMP_COLOR_ATTACHMENT_WARM_RED_THRESHOLD:-}
dump_color_attachment_warm_green_threshold=${DXMT9_DUMP_COLOR_ATTACHMENT_WARM_GREEN_THRESHOLD:-}
dump_color_attachment_warm_blue_margin=${DXMT9_DUMP_COLOR_ATTACHMENT_WARM_BLUE_MARGIN:-}
dump_draw_texture_handles=${DXMT9_DUMP_DRAW_TEXTURE_HANDLES:-}
dump_draw_texture0_any=0
dump_draw_texture0_width=${DXMT9_DUMP_DRAW_TEXTURE0_WIDTH:-}
dump_draw_texture0_height=${DXMT9_DUMP_DRAW_TEXTURE0_HEIGHT:-}
dump_draw_texture0_format=${DXMT9_DUMP_DRAW_TEXTURE0_FORMAT:-}
dump_draw_texture_seq=${DXMT9_DUMP_DRAW_TEXTURE_SEQ:-}
dump_draw_texture_enc=${DXMT9_DUMP_DRAW_TEXTURE_ENC:-}
dump_draw_texture_dir=${DXMT9_DUMP_DRAW_TEXTURE_DIR:-}
aggressive_color_dontcare=0
aggressive_depth_dontcare=0
disable_cull=0
disable_scissor=0
disable_alpha_test=0
disable_fog=0
force_texture_white=0
probe_force_texture_white=0
probe_force_texture_white_row=
probe_force_texture_white_rows=
probe_force_texture_white_class=
probe_force_texture_white_classes=
probe_force_texture_white_texture0=
probe_force_texture_white_texture0_width=
probe_force_texture_white_texture0_height=
probe_force_texture_white_texture0_format=
probe_force_texture_white_draw_ordinal=
probe_force_texture_white_draw_ordinals=
probe_force_texture_white_draw_ordinal_min=
probe_force_texture_white_draw_ordinal_max=
probe_force_texture_white_command_index=
probe_force_texture_white_command_indexes=
probe_force_texture_white_command_index_min=
probe_force_texture_white_command_index_max=
probe_force_texture_white_command_draw_index=
probe_force_texture_white_command_draw_indexes=
probe_force_texture_white_command_draw_index_min=
probe_force_texture_white_command_draw_index_max=
probe_disable_alpha_blend=0
probe_disable_alpha_blend_row=
probe_disable_alpha_blend_rows=
probe_disable_alpha_blend_class=
probe_disable_alpha_blend_classes=
probe_disable_alpha_blend_texture0=
probe_disable_alpha_blend_texture0_width=
probe_disable_alpha_blend_texture0_height=
probe_disable_alpha_blend_texture0_format=
probe_disable_depth_write=0
probe_disable_depth_write_row=
probe_disable_depth_write_rows=
probe_disable_depth_write_class=
probe_disable_depth_write_classes=
probe_depth_func_always=0
probe_depth_func_always_row=
probe_depth_func_always_rows=
probe_depth_func_always_class=
probe_depth_func_always_classes=
probe_depth_func_always_texture0=
probe_depth_func_always_texture0_width=
probe_depth_func_always_texture0_height=
probe_depth_func_always_texture0_format=
probe_fragmentless_depth_only=0
probe_fragmentless_depth_only_row=
probe_fragmentless_depth_only_rows=
probe_fragmentless_depth_only_class=
probe_fragmentless_depth_only_classes=
probe_fragmentless_depth_only_keep_vsout=0
force_visible=0
effect_draw_trace=0
effect_draw_trace_seq=
effect_draw_trace_seq_min=
effect_draw_trace_seq_max=
effect_draw_trace_enc=
effect_draw_trace_texture0=
effect_draw_trace_texture0_width=
effect_draw_trace_texture0_height=
effect_draw_trace_texture0_format=
effect_draw_trace_primitive_type=
effect_draw_trace_point_sprite=0
effect_draw_trace_include_non_alpha=0
effect_draw_trace_include_untextured=0
effect_draw_trace_geometry=0
effect_draw_trace_geometry_max_refs=
visibility_scout=0
visibility_scout_row=
visibility_scout_rows=
visibility_scout_path=
visibility_scout_draw_indices=
visibility_scout_summary_output=
visibility_scout_summary_csv_output=
visibility_scout_summary_limit=${DXMT_3DMARK05_VISIBILITY_SCOUT_SUMMARY_LIMIT:-12}
compare_baseline_output=${DXMT_3DMARK05_COMPARE_BASELINE_OUTPUT:-}
compare_baseline_joined=${DXMT_3DMARK05_COMPARE_BASELINE_JOINED:-}
semantic_image_policy=${DXMT_3DMARK05_SEMANTIC_IMAGE_POLICY:-}
semantic_image_before=${DXMT_3DMARK05_SEMANTIC_IMAGE_BEFORE:-}
semantic_image_after=${DXMT_3DMARK05_SEMANTIC_IMAGE_AFTER:-}
semantic_image_output=${DXMT_3DMARK05_SEMANTIC_IMAGE_OUTPUT:-}
semantic_image_summary_output=${DXMT_3DMARK05_SEMANTIC_IMAGE_SUMMARY_OUTPUT:-}
semantic_image_diff_output=${DXMT_3DMARK05_SEMANTIC_IMAGE_DIFF_OUTPUT:-}
semantic_image_min_active_pct=${DXMT_3DMARK05_SEMANTIC_IMAGE_MIN_ACTIVE_PCT:-1}
encoder_breakdown_seq=${DXMT_3DMARK05_ENCODER_BREAKDOWN_SEQ:-}
encoder_breakdown_seq_min=${DXMT_3DMARK05_ENCODER_BREAKDOWN_SEQ_MIN:-}
encoder_breakdown_seq_max=${DXMT_3DMARK05_ENCODER_BREAKDOWN_SEQ_MAX:-}
encoder_breakdown_all_frames=0
encoder_breakdown_enabled=1
render_pass_reentry_top=${DXMT_3DMARK05_RENDER_PASS_REENTRY_TOP:-}
dump_framegraph_dag=${DXMT_3DMARK05_DUMP_FRAMEGRAPH_DAG:-0}
framegraph_dag_frame=${DXMT_3DMARK05_DUMP_FRAMEGRAPH_DAG_FRAME:-}
framegraph_dag_frame_radius=${DXMT_3DMARK05_DUMP_FRAMEGRAPH_DAG_FRAME_RADIUS:-}
framegraph_dag_formats=${DXMT_3DMARK05_DUMP_FRAMEGRAPH_DAG_FORMATS:-json,mermaid}
framegraph_dag_optimize=${DXMT_3DMARK05_DUMP_FRAMEGRAPH_DAG_OPTIMIZE:-}
framegraph_dag_draws=${DXMT_3DMARK05_DUMP_FRAMEGRAPH_DAG_DRAWS:-0}
require_color_dontcare_increase=0
require_depth_dontcare_increase=0
require_tile_preservation_decrease=0
require_tile_preservation_not_increase=0
require_command_buffers_per_present_not_increase=0
require_render_passes_per_present_not_increase=0
require_render_pass_carry_promotion_gates=0
require_encoder_final_end_reason_not_increase=0
require_encoder_final_same_key_reopen_not_increase=0
require_encoder_color_load_not_increase=0
require_encoder_depth_load_not_increase=0
require_draw_run_records_increase=0
require_draw_run_records_per_submit_increase=0
require_binding_overrides_present=0
require_const_upload_passthrough_present=0
require_draw_submission_batch_present=0
require_const_upload_break_bytes_decrease=0
require_encode_draw_cpu_decrease=0
require_completion_present_wait_decrease=0
require_completion_wait_with_enqueue_increase=0
require_completion_wait_without_enqueue_decrease=0
require_completion_present_wait_with_enqueue_increase=0
require_completion_present_wait_without_enqueue_decrease=0
require_encode_ready_depth_gt1_increase=0
require_commit_chunk_replay_cpu_per_present_decrease=0
require_queue_draw_submission_cpu_per_present_decrease=0
require_snapshot_cpu_per_present_decrease=0
require_snapshot_cache_lookup_cpu_per_present_decrease=0
require_snapshot_cache_uniform_build_cpu_per_present_decrease=0
require_snapshot_cache_uniform_hash_cpu_per_present_decrease=0
require_batch_miss_uniform_build_cpu_per_present_decrease=0
require_batch_miss_uniform_hash_cpu_per_present_decrease=0
require_batch_miss_vs_const_hash_cpu_per_present_decrease=0
require_batch_miss_ps_const_hash_cpu_per_present_decrease=0
require_batch_miss_nonconst_hash_cpu_per_present_decrease=0
require_snapshot_uniform_copy_cpu_per_present_decrease=0
require_submit_draw_run_batch_append_uniform_cpu_per_present_decrease=0
require_draw_uniform_payload_lookup_cpu_per_present_decrease=0
require_draw_uniform_payload_append_copy_cpu_per_present_decrease=0
require_argbuf_setup_cpu_per_present_decrease=0
require_argbuf_open_cpu_per_present_decrease=0
require_argbuf_cbuf_update_cpu_per_present_decrease=0
require_argbuf_cbuf_update_vs_cpu_per_present_decrease=0
require_uniform_compact_saved_bytes_present=0
require_current_uniform_compact_saved_bytes_present=0
require_snapshot_state_elided_present=0
require_discarded_state_not_increase=0
require_submission_carrier_bytes_per_record_decrease=0
require_submission_carrier_uniform_storage_per_record_decrease=0
require_encode_chunk_cpu_per_present_decrease=0
require_no_enqueue_commit_entry_to_publish_decrease=0
require_no_enqueue_publish_to_encode_dequeue_decrease=0
require_no_enqueue_encode_dequeue_to_commit_decrease=0
require_no_enqueue_wait_to_next_enqueue_decrease=0
require_no_enqueue_before_publish_closure_decrease=0
require_no_enqueue_before_publish_inter_replay_gap_decrease=0
require_pe_focused_between_call_gap_residual_decrease=0
max_gpu_command_buffer_regression_ms=${DXMT_3DMARK05_MAX_GPU_COMMAND_BUFFER_REGRESSION_MS:-}
max_const_upload_break_count_ratio=${DXMT_3DMARK05_MAX_CONST_UPLOAD_BREAK_COUNT_RATIO:-}
require_result_json=0
allow_partial_stable_frame_proof=0
require_top_gpu_decrease=0
require_top_buffer_write_decrease=0
require_top_vs_buffer_write_decrease=0
require_top_unexplained_buffer_write_decrease=0
require_stream_handle_churn_decrease=0
require_ib_handle_churn_decrease=0
require_argbuf_cbuf_decrease=0
require_transient_decrease=0
require_top_gpu_share_increase=0
require_top_row_key_match=0
require_stable_frame_proof=0
require_tvb_mechanism_proof=0
require_cache_opt_apply_proof=0
require_opaque_depth_index_cache_proof=0
require_screen_blend_cache_proof=0
require_semantic_image_proof=0
require_target_index_cache_miss32_decrease=0
require_target_index_cache_opt_miss32_decrease=0
require_target_reordered_index_cache_hits=0
require_target_vs_buffer_write_decrease=0
require_target_vs_invocations_decrease=0
require_top_pso_attribution=0
require_xcode_counter_coverage=0
require_dxmt_join_coverage=0
require_shader_dump_matches=0
min_top_pso_samples_per_draw=${DXMT_3DMARK05_MIN_TOP_PSO_SAMPLES_PER_DRAW:-0.90}
min_top_dxmt_joined_fraction=${DXMT_3DMARK05_MIN_TOP_DXMT_JOINED_FRACTION:-1.0}
max_top_gpu_regression_ms=${DXMT_3DMARK05_MAX_TOP_GPU_REGRESSION_MS:-}
max_top_buffer_write_regression_mib=${DXMT_3DMARK05_MAX_TOP_BUFFER_WRITE_REGRESSION_MIB:-}
max_non_target_gpu_regression_ms=${DXMT_3DMARK05_MAX_NON_TARGET_GPU_REGRESSION_MS:-}
max_non_target_vs_buffer_write_regression_mib=${DXMT_3DMARK05_MAX_NON_TARGET_VS_BUFFER_WRITE_REGRESSION_MIB:-}
max_non_target_vs_invocations_regression_ratio=${DXMT_3DMARK05_MAX_NON_TARGET_VS_INVOCATIONS_REGRESSION_RATIO:-}
max_non_target_draw_call_delta_ratio=${DXMT_3DMARK05_MAX_NON_TARGET_DRAW_CALL_DELTA_RATIO:-}
max_non_target_vertex_count_delta_ratio=${DXMT_3DMARK05_MAX_NON_TARGET_VERTEX_COUNT_DELTA_RATIO:-}
max_non_target_triangle_delta_ratio=${DXMT_3DMARK05_MAX_NON_TARGET_TRIANGLE_DELTA_RATIO:-}
max_top_unexplained_buffer_write_ratio=${DXMT_3DMARK05_MAX_TOP_UNEXPLAINED_BUFFER_WRITE_RATIO:-}
max_top_draw_call_delta_ratio=${DXMT_3DMARK05_MAX_TOP_DRAW_CALL_DELTA_RATIO:-}
max_top_vertex_count_delta_ratio=${DXMT_3DMARK05_MAX_TOP_VERTEX_COUNT_DELTA_RATIO:-}
max_top_triangle_delta_ratio=${DXMT_3DMARK05_MAX_TOP_TRIANGLE_DELTA_RATIO:-}
top_n=${DXMT_3DMARK05_TOP_N:-3}
hot_gpu_share=${DXMT_3DMARK05_HOT_GPU_SHARE:-95.0}
min_free_mb=${DXMT_3DMARK05_MIN_TRACE_FREE_MB:-}
metal_capture_destination=${DXMT_3DMARK05_METAL_CAPTURE_DESTINATION:-${DXMT_METAL_CAPTURE_DESTINATION:-}}
metal_capture_destination_explicit=0
require_xcode_attach_preflight=${DXMT_3DMARK05_REQUIRE_XCODE_ATTACH_PREFLIGHT:-0}
xcode_attach_preflight_only=0
with_wine_capture_layer=${DXMT_3DMARK05_WITH_WINE_CAPTURE_LAYER:-0}
osascript_bin=${DXMT_3DMARK05_OSASCRIPT_BIN:-osascript}
devtools_security_bin=${DXMT_3DMARK05_DEVTOOLS_SECURITY_BIN:-/usr/sbin/DevToolsSecurity}
target_row_keys=()
frontmost_loop_pid=

capture_destination_is_developer_tools() {
  [[ "$1" == "developerTools" || "$1" == "xcode" ]]
}

capture_destination_is_file() {
  [[ -z "$1" || "$1" == "gpuTraceDocument" ||
     "$1" == "gputrace" || "$1" == "file" ]]
}

capture_destination_is_valid() {
  case "$1" in
    ""|gpuTraceDocument|gputrace|file|developerTools|xcode)
      return 0
      ;;
    *)
      return 1
      ;;
  esac
}

binary_has_metal_capture_enabled() {
  local path=$1
  [[ -f "$path" ]] && strings "$path" 2>/dev/null | grep -q 'MetalCaptureEnabled'
}

run_file_capture_layer_preflight() {
  local wine_root=$1
  local wine_real="$wine_root/bin/wine.real"
  local wine_preloader="$wine_root/bin/wine-preloader"
  local wine_real_has=0
  local wine_preloader_has=0
  if binary_has_metal_capture_enabled "$wine_real"; then
    wine_real_has=1
  fi
  if binary_has_metal_capture_enabled "$wine_preloader"; then
    wine_preloader_has=1
  fi

  if [[ "${DXMT_3DMARK05_SET_MTL_CAPTURE_ENABLED:-0}" != "0" ||
        "${MTL_CAPTURE_ENABLED:-0}" != "0" ||
        "${METAL_CAPTURE_ENABLED:-0}" != "0" ]]; then
    echo "file_capture_layer_preflight: status=pass reason=metal-capture-enabled-env"
    return 0
  fi
  if (( wine_real_has && wine_preloader_has )); then
    echo "file_capture_layer_preflight: status=pass reason=wine-binaries-metal-capture-enabled"
    return 0
  fi

  echo "file_capture_layer_preflight: status=fail reason=wine-binaries-lack-metal-capture-enabled wine_real_has=${wine_real_has} wine_preloader_has=${wine_preloader_has}"
  return 2
}

run_wine_capture_layer_wrapper_preflight() {
  local wine_root=$1
  local wine_real="$wine_root/bin/wine.real"
  local wine_preloader="$wine_root/bin/wine-preloader"
  local capture_real="$wine_root/bin/wine.capture.real"
  local capture_preloader="$wine_root/bin/wine.capture.real-preloader"

  if [[ ! -f "$wine_real" || ! -f "$wine_preloader" ]]; then
    echo "file_capture_layer_preflight: status=fail reason=wine-target-missing wine_real=$wine_real wine_preloader=$wine_preloader"
    return 2
  fi
  if ! binary_has_metal_capture_enabled "$capture_real"; then
    echo "file_capture_layer_preflight: status=fail reason=capture-real-lacks-metal-capture-enabled capture_real=$capture_real"
    return 2
  fi
  if ! binary_has_metal_capture_enabled "$capture_preloader"; then
    echo "file_capture_layer_preflight: status=fail reason=capture-preloader-lacks-metal-capture-enabled capture_preloader=$capture_preloader"
    return 2
  fi

  echo "file_capture_layer_preflight: status=pass reason=wine-capture-layer-wrapper capture_real=$capture_real capture_preloader=$capture_preloader"
  return 0
}

detect_session_locked() {
  local locked=unknown
  if command -v ioreg >/dev/null 2>&1; then
    local session_state
    session_state=$(ioreg -n Root -d1 2>/dev/null || true)
    if [[ "$session_state" == *'"CGSSessionScreenIsLocked"=Yes'* ]]; then
      locked=yes
    else
      locked=no
    fi
  fi
  printf '%s\n' "$locked"
}

run_xcode_attach_preflight() {
  local output
  local devtools_output
  if [[ -x "$devtools_security_bin" ]]; then
    devtools_output=$("$devtools_security_bin" -status 2>&1 || true)
    if [[ "$devtools_output" == *"disabled"* ]]; then
      echo "xcode_attach_preflight: status=fail reason=developer-mode-disabled devtools_security=${devtools_output}"
      return 2
    fi
  fi
  if ! output=$("$osascript_bin" <<'APPLESCRIPT' 2>&1
on boolText(value)
  if value then
    return "true"
  end if
  return "false"
end boolText

tell application "System Events"
  if not (exists process "Xcode") then
    return "status=fail reason=xcode-not-running"
  end if
  tell process "Xcode"
    if not (exists menu bar 1) then
      return "status=fail reason=no-menu-bar"
    end if
    if not (exists menu bar item "Debug" of menu bar 1) then
      return "status=fail reason=no-debug-menu"
    end if
    tell menu "Debug" of menu bar item "Debug" of menu bar 1
      set attachByPidFound to false
      set attachByPidEnabled to false
      set attachProcessFound to false
      set attachProcessEnabled to false
      set attachProcessFirstItem to ""
      repeat with itemIndex from 1 to count menu items
        set itemName to name of menu item itemIndex
        if itemName starts with "Attach to Process by PID or Name" then
          set attachByPidFound to true
          set attachByPidEnabled to enabled of menu item itemIndex
        end if
        if itemName is "Attach to Process" then
          set attachProcessFound to true
          set attachProcessEnabled to enabled of menu item itemIndex
          try
            set attachProcessFirstItem to name of menu item 1 of menu 1 of menu item itemIndex
          end try
        end if
      end repeat
      set statusText to "fail"
      set reasonText to "attach-unavailable"
      if attachByPidFound and attachByPidEnabled then
        set statusText to "pass"
        set reasonText to "attach-by-pid-enabled"
      else if attachProcessFound and attachProcessEnabled and attachProcessFirstItem is not "" and attachProcessFirstItem does not start with "Getting Process List" then
        set statusText to "pass"
        set reasonText to "process-list-populated"
      else if attachProcessFound and attachProcessEnabled and attachProcessFirstItem starts with "Getting Process List" then
        set reasonText to "process-list-loading"
      else if attachByPidFound and not attachByPidEnabled then
        set reasonText to "attach-by-pid-disabled"
      end if
      return "status=" & statusText & " reason=" & reasonText & " attach_by_pid_found=" & my boolText(attachByPidFound) & " attach_by_pid_enabled=" & my boolText(attachByPidEnabled) & " attach_process_found=" & my boolText(attachProcessFound) & " attach_process_enabled=" & my boolText(attachProcessEnabled) & " attach_process_first_item=" & attachProcessFirstItem
    end tell
  end tell
end tell
APPLESCRIPT
); then
    echo "xcode_attach_preflight: status=fail reason=osascript-failed output=$output" >&2
    return 2
  fi
  echo "xcode_attach_preflight: $output"
  if [[ "$output" == status=pass* ]]; then
    return 0
  fi
  return 2
}

run_3dmark05_frontmost_once() {
  local process_name=$1
  "$osascript_bin" -e "tell application \"System Events\" to if exists process \"$process_name\" then set frontmost of first process whose name is \"$process_name\" to true" >/dev/null 2>&1 || true
}

start_3dmark05_frontmost_loop() {
  if [[ "$keep_frontmost" == "0" || -z "$keep_frontmost" ]]; then
    return 0
  fi
  (
    while :; do
      run_3dmark05_frontmost_once "$keep_frontmost_process"
      sleep "$keep_frontmost_interval_sec" || exit 0
    done
  ) &
  frontmost_loop_pid=$!
  echo "frontmost_loop_started: pid=$frontmost_loop_pid process=$keep_frontmost_process interval_sec=$keep_frontmost_interval_sec"
}

stop_3dmark05_frontmost_loop() {
  if [[ -z "$frontmost_loop_pid" ]]; then
    return 0
  fi
  kill "$frontmost_loop_pid" >/dev/null 2>&1 || true
  wait "$frontmost_loop_pid" >/dev/null 2>&1 || true
  echo "frontmost_loop_stopped: pid=$frontmost_loop_pid"
  frontmost_loop_pid=
}

usage() {
  cat <<'USAGE'
Usage: scripts/tools/run_3dmark05_perf_probe.sh [options]

Run the standard 3DMark05 GT1 perf probe with dxmt9 perf counters, optional
encoder breakdown, optional Metal frame capture, and post-run summary CSV
generation.

Options:
  --suffix NAME       Output suffix (default: probe-<timestamp>-frame<N>)
  --frame N           1-based Metal capture frame (default: 60)
  --timeout SEC       run_experiment timeout seconds (default: 420 with gputrace,
                      120 with --no-gputrace; DXMT_3DMARK05_PROBE_TIMEOUT overrides)
                      The wrapper watchdog uses SEC + DXMT_3DMARK05_PROBE_TIMEOUT_SLACK
                      (default: 45) to clean up detached final-frame hangs.
  --wait-unlocked-sec SEC
                      Poll the macOS lock state before launch until unlocked.
                      Default: DXMT_3DMARK05_WAIT_UNLOCKED_SEC or 0.
  --wait-unlocked-interval-sec SEC
                      Poll interval for --wait-unlocked-sec. Default:
                      DXMT_3DMARK05_WAIT_UNLOCKED_INTERVAL_SEC or 5.
  --keep-frontmost    While the probe is running, periodically make the
                      3DMark05 process frontmost using System Events. Use this
                      for no-gputrace frame-sampling A/B runs where losing
                      focus changes frame progress or FPS.
  --keep-frontmost-interval-sec SEC
                      Poll interval for --keep-frontmost. Default:
                      DXMT_3DMARK05_KEEP_FRONTMOST_INTERVAL_SEC or 1.
  --keep-frontmost-process NAME
                      Process name made frontmost by --keep-frontmost.
                      Default: DXMT_3DMARK05_KEEP_FRONTMOST_PROCESS or
                      3DMark05.exe.
  --capture-delay-sec SEC
                      Override run_experiment capture delay seconds. Use this
                      for frame-window visual probes; the catalogue default is
                      70s for 3DMark05 and may miss rifle close-up frames.
  --capture-frames LIST
                      Set DXMT_CAPTURE_FRAMES for deterministic internal
                      backbuffer captures, e.g. 820,840,860. Writes BMP files
                      under --capture-dir or traces/<run-id>/analysis/captures.
  --capture-range START:END[:STEP]
                      Set DXMT_CAPTURE_RANGE for deterministic internal
                      backbuffer captures, e.g. 820:900:20.
  --capture-dir DIR   Set DXMT_EXPERIMENT_CAPTURE_DIR for --capture-frames or
                      --capture-range. Relative paths resolve under repo root.
  --result-file NAME  3DMark05 result filename argument (default: dxmt9_gt1.3dr)
  --no-gputrace       Do not set DXMT_METAL_CAPTURE_FRAME/PATH
                      The wrapper does not set MTL_CAPTURE_ENABLED by default:
                      current 3DMark05/Wine startup can black-screen with that
                      Apple capture-layer env present. Set
                      DXMT_3DMARK05_SET_MTL_CAPTURE_ENABLED=1 only for
                      deliberate capture-layer experiments. File capture
                      preflights the Wine child for MetalCaptureEnabled and
                      fails before launch when no capture layer is available;
                      set DXMT_3DMARK05_ALLOW_NO_FILE_CAPTURE_LAYER=1 only for
                      a deliberate late-failure diagnostic.
  --metal-capture-destination DEST
                      Set DXMT_METAL_CAPTURE_DESTINATION for gputrace runs.
                      Accepted values: gpuTraceDocument, gputrace, file,
                      developerTools, xcode. developerTools/xcode expects
                      Xcode to be attached to the real Wine child and does not
                      write frame<N>.gputrace directly.
  --xcode-developer-tools-capture
                      Shorthand for --metal-capture-destination developerTools.
  --with-wine-capture-layer
                      For a deliberate file .gputrace diagnostic, wrap the
                      launch in run_with_wine_metal_capture_layer.sh so
                      wine.real and wine-preloader are temporarily replaced by
                      MetalCaptureEnabled copies. This is not a normal FPS
                      sample path.
  --require-xcode-attach-preflight
                      Before launching a developerTools capture run, query
                      Xcode's Debug attach menu and fail if attach-by-PID is
                      disabled and the process list is not populated.
  --xcode-attach-preflight-only
                      Run the Xcode attach preflight and exit without launching
                      3DMark05. Useful before a manual attach-after-normal-start
                      capture attempt.
  --encoder-breakdown-seq N
                      Set DXMT9_PERF_ENCODER_BREAKDOWN_SEQ=N to emit only one
                      RenderPass[seq=N,...] frame's encoder breakdown
  --encoder-breakdown-seq-range MIN:MAX
                      Set DXMT9_PERF_ENCODER_BREAKDOWN_SEQ_MIN/MAX to emit a
                      bounded RenderPass seq window. Useful for xctrace
                      sidecars where a full all-frame breakdown is too heavy.
  --encoder-breakdown-all-frames
                      Do not auto-scope encoder/index diagnostics to --frame
                      when gputrace capture is enabled
  --no-encoder-breakdown
                      Do not set DXMT9_PERF_ENCODER_BREAKDOWN. Use only for
                      no-gputrace run-level/default-policy smokes; Xcode proof
                      and per-row diagnostics need encoder breakdown.
  --render-pass-reentry-top N
                      Set DXMT9_PERF_RENDER_PASS_REENTRY_TOP=N to emit the
                      top same-key render-pass re-entry rows per frame. Use
                      with encoder breakdown when pass action shape needs to
                      be joined to A/B role pairs.
  --dump-framegraph-dag
                      Set DXMT9_RENDERER_DUMP_DAG under
                      traces/<run-id>/analysis/dag and summarize combined,
                      pre-opt, and post-opt same-attachment re-entry
                      candidates after the run.
  --framegraph-dag-frame N
                      Set DXMT9_RENDERER_DUMP_DAG_FRAME=N. Implies
                      --dump-framegraph-dag. Defaults to --frame when the DAG
                      dump is enabled.
  --framegraph-dag-frame-radius N
                      Set DXMT9_RENDERER_DUMP_DAG_FRAME_RADIUS=N. Implies
                      --dump-framegraph-dag. Defaults to 0.
  --framegraph-dag-formats LIST
                      Set DXMT9_RENDERER_DUMP_DAG_FORMATS, e.g. json,mermaid.
                      JSON is needed for the wrapper's post-run summary.
  --framegraph-dag-optimize LIST
                      Set DXMT9_RENDERER_DUMP_DAG_OPTIMIZE for analysis-only
                      post-opt dumps, e.g. passcoalesce.
  --framegraph-dag-draws
                      Set DXMT9_RENDERER_DUMP_DAG_DRAWS=1 to include
                      draw-level JSON detail in DAG dumps.
  --frame-sampling    Set DXMT9_PERF_FRAME_SAMPLING=1 and emit per-Present
                      wall_ms/fps plus counter deltas. Use for visual/perf
                      coupling probes such as bloom, glow, and muzzle effects.
  --open-cb-preencode-tail-present
                      Set DXMT9_OPEN_CB_PREENCODE_TAIL_PRESENT=1 for the
                      open-command-buffer P4 carrier.
  --open-cb-carry-render-session
                      Set DXMT9_OPEN_CB_CARRY_RENDER_SESSION=1 so the open-CB
                      carrier keeps EncodeChunkSession state alive until the
                      Present tail finalizes the shared command buffer.
  --open-cb-semantic-boundary-publish
                      Set DXMT9_OPEN_CB_SEMANTIC_BOUNDARY_PUBLISH=1 so
                      semantic non-draw commands publish the preceding
                      non-present source for the open-CB carrier.
  --open-cb-cpu-ready-command-limit N
                      Set DXMT9_OPEN_CB_CPU_READY_COMMAND_LIMIT=N so draw
                      submit publishes non-present SemanticBoundary sources
                      deterministically when the writing slot reaches N commands.
  --open-cb-writer-active-cpu-ready-publish
                      Set DXMT9_OPEN_CB_WRITER_ACTIVE_CPU_READY_PUBLISH=1 so
                      H161 writer-active non-present writing-slot misses are
                      cut as CPU-ready semantic sources for the open-CB carrier.
  --open-cb-active-wait-cpu-ready-append
                      Set DXMT9_OPEN_CB_ACTIVE_WAIT_CPU_READY_APPEND=1 so an
                      active completion wait may cut current writer work as a
                      semantic CPU-ready source and append compatible ready
                      sources to the pending EncodeSession before release.
  --open-cb-wait-start-cpu-ready-publish
                      Set DXMT9_OPEN_CB_WAIT_START_CPU_READY_PUBLISH=1 so an
                      active completion wait may cut current writer work as a
                      semantic CPU-ready source before a pending EncodeSession
                      exists, including producer-side draw appends that happen
                      after the wait-start wakeup.
  --open-cb-semantic-boundary-release-mode MODE
                      Set DXMT9_OPEN_CB_SEMANTIC_BOUNDARY_RELEASE_MODE=MODE.
                      Accepted values follow the runtime: completion_wait or
                      deterministic.
  --open-cb-pending-tail-wait-us N
                      Set DXMT9_OPEN_CB_PENDING_TAIL_WAIT_US=N. When paired
                      with open-CB carry, a tail-less pending head waits up to
                      N microseconds for a Present tail before finalizing and
                      submitting the head alone.
  --stage-pre-present-command-limit N
                      Set DXMT9_STAGE_PRE_PRESENT_COMMAND_LIMIT=N. Required
                      for pre-Present head staging/open-CB split candidates.
  --draw-chunk-command-limit N
                      Set DXMT9_DRAW_CHUNK_COMMAND_LIMIT=N for ordinary
                      draw-limit source-boundary probes.
  --enable-chunk-end-carry
                      Set DXMT9_ENABLE_CHUNK_END_CARRY=1 for the default-off
                      cross-chunk pending draw submission carry experiment.
  --probe-draw-packet-actual-change
                      Set DXMT9_PERF_DRAW_PACKET_ACTUAL_CHANGE=1. Counts
                      declared draw-packet state deltas whose values do or do
                      not actually change the unix-side DeviceState before
                      snapshot invalidation.
  --probe-vs-const-setter-range
                      Set DXMT9_PERF_VS_CONST_SETTER_RANGE=1. Aggregates
                      SetVertexShaderConstantF app-call ranges and flushed VS
                      float constant record ranges by current VS/PS shader hash.
  --pe-recorder-stats
                      Set DXMT9_PE_RECORDER_STATS=1 and default
                      DXMT_LOG_LEVEL=info so recorder timing lines reach logs.
  --pe-recorder-chunk-log
                      Set DXMT9_PE_RECORDER_CHUNK_LOG=1.
  --pe-flush-after-clear
                      Set DXMT9_PE_FLUSH_AFTER_CLEAR=1 for PE chunk pacing
                      probes.
  --pe-flush-after-draw
                      Set DXMT9_PE_FLUSH_AFTER_DRAW=1 for PE chunk pacing
                      probes.
  --pe-draw-full-snapshot
                      Set DXMT9_PE_DRAW_FULL_SNAPSHOT=1 for bridge debug
                      probes.
  --pe-chunk-max-records N
                      Set DXMT9_PE_CHUNK_MAX_RECORDS=N.
  --pe-chunk-max-bytes N
                      Set DXMT9_PE_CHUNK_MAX_BYTES=N.
  --dxmt-log-level LEVEL
                      Set DXMT_LOG_LEVEL for the wrapped run.
  --dump-shaders      Dump translated MSL and D3D shader bytecode under
                      traces/<run-id>/analysis/shaders
  --trim-unused-varyings
                      Set DXMT9_TRIM_UNUSED_VARYINGS=1 for pair-local VSOut
                      liveness/VS buffer-write experiments
  --trim-unused-varyings-vs-hashes HASHES
                      Set DXMT9_TRIM_UNUSED_VARYINGS_VS_HASHES to a
                      comma/semicolon/space-separated allowlist of D3D vertex
                      shader hashes. Requires --trim-unused-varyings.
  --trim-unused-varyings-ps-hashes HASHES
                      Set DXMT9_TRIM_UNUSED_VARYINGS_PS_HASHES to a
                      comma/semicolon/space-separated allowlist of D3D pixel
                      shader hashes. Requires --trim-unused-varyings.
  --drop-vsout-point-size
                      Set DXMT9_PROBE_DROP_VSOUT_POINT_SIZE=1 to remove only
                      VSOut [[point_size]] for pipeline-shape A/B probes
  --probe-position-only-vsout
                      Set DXMT9_PROBE_POSITION_ONLY_VSOUT=1 to force
                      position-only VSOut and constant fragment output for a
                      correctness-invalid hidden VS-write lower-bound probe
  --probe-half-vsout
                      Set DXMT9_PROBE_HALF_VSOUT=1 to request half-precision
                      user varyings in VSOut while keeping position,
                      point_size, and clip_distance float
  --force-fragment-color
                      Set DXMT_DEBUG_FORCE_FRAGMENT_COLOR=1 to keep the
                      current VSOut layout while forcing translated/FFP
                      fragment shaders to a constant color
  --trim-vertex-temps
                      Set DXMT9_TRIM_VERTEX_TEMPS=1 for translated VS temp
                      register/spill experiments
  --trim-vs-output-scratch
                      Set DXMT9_TRIM_VS_OUTPUT_SCRATCH=1 to size translated
                      VS outTexcoord[] scratch to observed output usage
  --split-sparse-const-records
                      Set DXMT9_SPLIT_SPARSE_CONST_RECORDS=1 for sparse
                      constant-upload record splitting experiments
  --suppress-rt-pixel-format-view
                      Set DXMT9_SUPPRESS_RT_PIXEL_FORMAT_VIEW=1 to test
                      RT-only swizzle formats without PixelFormatView usage
  --suppress-x8-rt-pixel-format-view
                      Set DXMT9_SUPPRESS_X8_RT_PIXEL_FORMAT_VIEW=1 to test
                      X8 RT-only swizzle formats without PixelFormatView usage
  --x8-shader-alpha-fill
                      Set DXMT9_X8_SHADER_ALPHA_FILL=1 to force sampled X8
                      texture alpha to 1.0 in shader variants
  --native-metal-base-vertex
                      Set DXMT9_USE_NATIVE_METAL_BASE_VERTEX=1 for indexed
                      draw baseVertex / VS buffer-write experiments
  --optimize-screen-blend-index-order
                      Set DXMT9_OPTIMIZE_SCREEN_BLEND_INDEX_ORDER=1 to reverse
                      strict screen-blend indexed triangle-list primitive order
                      through a transient IB. Diagnostic/profiling only:
                      screen-blend output is destination-dependent.
  --optimize-screen-blend-index-order-row SEQ/ENC
                      Set DXMT9_OPTIMIZE_SCREEN_BLEND_INDEX_ORDER_ROW=SEQ/ENC
                      to constrain screen-blend index-order optimization
  --optimize-screen-blend-index-order-rows ROWS
                      Set DXMT9_OPTIMIZE_SCREEN_BLEND_INDEX_ORDER_ROWS=ROWS to
                      constrain screen-blend index-order optimization
  --optimize-screen-blend-index-order-class CLASS
                      Set DXMT9_OPTIMIZE_SCREEN_BLEND_INDEX_ORDER_CLASS=CLASS.
                      Accepted values match split-large indexed filters
  --optimize-screen-blend-index-order-classes CLASSES
                      Set DXMT9_OPTIMIZE_SCREEN_BLEND_INDEX_ORDER_CLASSES=CLASSES.
                      Values are ANDed and may be comma/semicolon/space/+ or &
                      separated, e.g. large4096,screen-blend,scissor
  --optimize-screen-blend-index-order-stream0-span-min BYTES
                      Set DXMT9_OPTIMIZE_SCREEN_BLEND_INDEX_ORDER_STREAM0_SPAN_MIN
                      to require a minimum original stream0 byte span
  --optimize-screen-blend-index-cache
                      Set DXMT9_OPTIMIZE_SCREEN_BLEND_INDEX_CACHE=1 to submit
                      cached LRU32 reordered IBs for strict screen-blend
                      triangle-list draws. Explicit-tolerance-only opt-in: it
                      never bypasses the screen-blend/depth-read safety
                      predicate, and proof runs must carry
                      --require-screen-blend-cache-proof plus
                      --semantic-image-policy exact|lsb1.
  --optimize-screen-blend-index-cache-min-gain-pct PCT
                      Set DXMT9_OPTIMIZE_SCREEN_BLEND_INDEX_CACHE_MIN_GAIN_PCT
                      (default in dxmt9: 10)
  --index-cache-candidate-frontier-cap N
                      Diagnostic-only: set
                      DXMT9_INDEX_CACHE_CANDIDATE_FRONTIER_CAP to limit the
                      LRU32 candidate frontier width during index-cache
                      reorder construction. 0 disables the cap.
  --index-cache-candidate-lazy-frontier
                      Diagnostic-only: set
                      DXMT9_INDEX_CACHE_CANDIDATE_LAZY_FRONTIER=1 to use a
                      lazily refreshed priority frontier instead of full
                      candidate-vector rescans. Can change primitive order.
  --index-cache-candidate-bucketed-select
                      Diagnostic-only: set
                      DXMT9_INDEX_CACHE_CANDIDATE_BUCKETED_SELECT=1 to keep
                      active candidates in cached-vertex-count buckets and
                      update only touched-vertex neighbors. Can change
                      primitive order.
  --index-cache-candidate-strict-lru
                      Diagnostic-only: set
                      DXMT9_INDEX_CACHE_CANDIDATE_STRICT_LRU=1 to update the
                      candidate builder's simulated LRU cache with the same
                      no-duplicate miss path as the LRU32 measurement helper.
                      Can change primitive order/candidate quality.
  --index-cache-candidate-upper-bound-gate
                      Diagnostic-only: set
                      DXMT9_INDEX_CACHE_CANDIDATE_UPPER_BOUND_GATE=1 to skip
                      candidate construction when original unique index count
                      proves the configured min-gain gate cannot be reached.
  --split-large-indexed-draws N
                      Set DXMT9_SPLIT_LARGE_INDEXED_DRAWS=N to split indexed
                      triangle-list draws above N primitives
  --split-large-indexed-draws-stream0-span-max BYTES
                      Set DXMT9_SPLIT_LARGE_INDEXED_DRAWS_STREAM0_SPAN_MAX
                      to split selected indexed triangle-list draws when the
                      next chunk would exceed this stream0 index byte span
  --split-large-indexed-draws-max-chunks-per-draw N
                      Set DXMT9_SPLIT_LARGE_INDEXED_DRAWS_MAX_CHUNKS_PER_DRAW
                      to skip source draws that would exceed this split count
  --split-large-indexed-draws-row SEQ/ENC
                      Set DXMT9_SPLIT_LARGE_INDEXED_DRAWS_ROW=SEQ/ENC to
                      constrain split-large-indexed-draws to one
                      Xcode/DXMT RenderPass row, e.g. 60/3
  --split-large-indexed-draws-rows ROWS
                      Set DXMT9_SPLIT_LARGE_INDEXED_DRAWS_ROWS=ROWS to
                      constrain split-large-indexed-draws to a
                      comma/semicolon/space separated row set
  --split-large-indexed-draws-class CLASS
                      Set DXMT9_SPLIT_LARGE_INDEXED_DRAWS_CLASS=CLASS.
                      Accepted values: any, opaque-depth-write, nonopaque,
                      depth-read, alpha-blend, no-alpha-blend, screen-blend,
                      standard-alpha, additive-alpha, scissor, no-scissor,
                      textured, large4096
  --split-large-indexed-draws-classes CLASSES
                      Set DXMT9_SPLIT_LARGE_INDEXED_DRAWS_CLASSES=CLASSES.
                      Values are ANDed and may be comma/semicolon/space/+ or
                      & separated, e.g. large4096,standard-alpha
  --force-expand-indexed
                      Set DXMT_FORCE_EXPAND_INDEXED=1 to expand indexed draws
                      into flat vertex lists for primitive/backend pressure
                      classification
  --probe-force-expand-indexed
                      Set DXMT9_PROBE_FORCE_EXPAND_INDEXED=1 to expand only
                      selected indexed triangle-list draws into flat vertex
                      lists for row/class-scoped backend-pressure diagnosis
  --probe-force-expand-indexed-row SEQ/ENC
                      Set DXMT9_PROBE_FORCE_EXPAND_INDEXED_ROW=SEQ/ENC
  --probe-force-expand-indexed-rows ROWS
                      Set DXMT9_PROBE_FORCE_EXPAND_INDEXED_ROWS=ROWS
  --probe-force-expand-indexed-class CLASS
                      Set DXMT9_PROBE_FORCE_EXPAND_INDEXED_CLASS=CLASS.
                      Accepted values match split-large indexed filters
  --probe-force-expand-indexed-classes CLASSES
                      Set DXMT9_PROBE_FORCE_EXPAND_INDEXED_CLASSES=CLASSES.
                      Values are ANDed, e.g. depth-read,screen-blend,textured
  --probe-reverse-indexed-triangles
                      Set DXMT9_PROBE_REVERSE_INDEXED_TRIANGLES=1 to keep
                      indexed draws but reverse triangle-list primitive order
                      through a transient IB for index locality/backend probes
  --probe-reverse-opaque-indexed-triangles
                      Set DXMT9_PROBE_REVERSE_OPAQUE_INDEXED_TRIANGLES=1 to
                      reverse only opaque depth-writing triangle-list indexed
                      draws, leaving blended/alpha-test/stencil draws intact
  --probe-reverse-nonopaque-indexed-triangles
                      Set DXMT9_PROBE_REVERSE_NONOPAQUE_INDEXED_TRIANGLES=1
                      to reverse only triangle-list indexed draws outside the
                      opaque depth-writing subset for visibility/tile probes
  --probe-sort-indexed-triangles-by-min-index
                      Set DXMT9_PROBE_SORT_INDEXED_TRIANGLES_BY_MIN_INDEX=1
                      to sort triangle-list primitive order by triangle
                      min/max index through a transient IB. Uses the same
                      row/class/span filters as reverse-indexed-triangles
  --probe-optimize-indexed-triangles-vertex-cache
                      Set DXMT9_PROBE_OPTIMIZE_INDEXED_TRIANGLES_VERTEX_CACHE=1
                      to greedily reorder triangle-list primitive order around
                      a small vertex cache through a transient IB. Uses the
                      same row/class/span filters as reverse-indexed-triangles
  --optimize-opaque-depth-index-cache
                      Set DXMT9_OPTIMIZE_OPAQUE_DEPTH_INDEX_CACHE=1 to submit
                      cached LRU32 reordered IBs for opaque depth-writing
                      triangle-list draws. This is opt-in, never bypasses the
                      opaque-depth-write safety gate, and is not row-scoped.
  --optimize-opaque-depth-index-cache-min-gain-pct PCT
                      Set DXMT9_OPTIMIZE_OPAQUE_DEPTH_INDEX_CACHE_MIN_GAIN_PCT
                      (default in dxmt9: 10)
  --probe-apply-index-cache-opt-candidate
                      Set DXMT9_PROBE_APPLY_INDEX_CACHE_OPT_CANDIDATE=1 to
                      submit the LRU32 cache-aware candidate only when its
                      measured miss reduction passes the min-gain gate. Uses
                      the same row/class/span filters plus opaque-depth-write
                      safety and a source-IB keyed reordered-index cache;
                      implies --measure-index-reuse and
                      --measure-index-cache-opt-candidate
  --probe-apply-index-cache-opt-candidate-unsafe-nonopaque
                      Set DXMT9_PROBE_APPLY_INDEX_CACHE_OPT_CANDIDATE_UNSAFE_NONOPAQUE=1
                      to bypass the opaque-depth-write safety gate for
                      targeted depth-read/blended diagnostic A/B probes.
                      Requires --probe-apply-index-cache-opt-candidate to
                      mutate draw order
  --probe-apply-index-cache-opt-candidate-min-gain-pct PCT
                      Set DXMT9_PROBE_APPLY_INDEX_CACHE_OPT_CANDIDATE_MIN_GAIN_PCT
                      (default in dxmt9: 10)
  --probe-reverse-indexed-triangles-row SEQ/ENC
                      Set DXMT9_PROBE_REVERSE_INDEXED_TRIANGLES_ROW=SEQ/ENC to
                      constrain any reverse-indexed-triangles probe to one
                      Xcode/DXMT RenderPass row, e.g. 60/3
  --probe-reverse-indexed-triangles-rows ROWS
                      Set DXMT9_PROBE_REVERSE_INDEXED_TRIANGLES_ROWS=ROWS to
                      constrain reverse probes to a comma/semicolon/space
                      separated row set, e.g. 60/0,60/1,60/3,60/4
  --probe-reverse-indexed-triangles-class CLASS
                      Set DXMT9_PROBE_REVERSE_INDEXED_TRIANGLES_CLASS=CLASS.
                      Accepted values match split-large indexed filters
  --probe-reverse-indexed-triangles-classes CLASSES
                      Set DXMT9_PROBE_REVERSE_INDEXED_TRIANGLES_CLASSES=CLASSES.
                      Values are ANDed and may be comma/semicolon/space/+ or
                      & separated, e.g. large4096,screen-blend or
                      depth-read,no-alpha-blend,no-scissor
  --probe-reverse-indexed-triangles-stream0-span-min BYTES
                      Set DXMT9_PROBE_REVERSE_INDEXED_TRIANGLES_STREAM0_SPAN_MIN
                      to require a minimum original stream0 byte span
  --probe-indexed-triangle-encoder-draw-min N
                      Set DXMT9_PROBE_INDEXED_TRIANGLE_ENCODER_DRAW_MIN=N.
                      Applies to reverse/sort/vertex-cache, screen-blend
                      index-order, split-large-indexed primitive probes, and
                      row-scoped force-texture-white, disable-alpha-blend, and
                      depth-func-always state probes
  --probe-indexed-triangle-encoder-draw-max N
                      Set DXMT9_PROBE_INDEXED_TRIANGLE_ENCODER_DRAW_MAX=N.
                      Use with a row selector to target a material window such
                      as 60/2 draw 71..188 without changing other rows
  --probe-indexed-triangle-encoder-draw-exclude LIST
                      Set DXMT9_PROBE_INDEXED_TRIANGLE_ENCODER_DRAW_EXCLUDE.
                      Comma/semicolon/space separated encoder draw indexes to
                      leave unmodified inside the selected row/range; debug
                      proof aid for exact-safe sub-window experiments only
  --probe-scissor-rect L,T,R,B
                      Set DXMT9_PROBE_SCISSOR_RECT=L,T,R,B to preserve scissor
                      enablement but override the scissor rectangle for
                      selected indexed triangle-list draws
  --probe-scissor-rect-row SEQ/ENC
                      Set DXMT9_PROBE_SCISSOR_RECT_ROW=SEQ/ENC to constrain
                      the scissor-rect probe to one Xcode/DXMT row
  --probe-scissor-rect-rows ROWS
                      Set DXMT9_PROBE_SCISSOR_RECT_ROWS=ROWS to constrain the
                      scissor-rect probe to a comma/semicolon/space row set
  --probe-scissor-rect-class CLASS
                      Set DXMT9_PROBE_SCISSOR_RECT_CLASS=CLASS. Accepted
                      values match split-large indexed filters
  --probe-scissor-rect-classes CLASSES
                      Set DXMT9_PROBE_SCISSOR_RECT_CLASSES=CLASSES. Values are
                      ANDed, e.g. large4096,screen-blend,scissor
  --probe-force-cull-mode MODE
                      Set DXMT9_PROBE_FORCE_CULL_MODE=MODE where MODE is one
                      of none, front, or back. Unlike --force-cull-mode, this
                      applies only to selected indexed triangle-list draws
  --probe-force-cull-mode-row SEQ/ENC
                      Set DXMT9_PROBE_FORCE_CULL_MODE_ROW=SEQ/ENC
  --probe-force-cull-mode-rows ROWS
                      Set DXMT9_PROBE_FORCE_CULL_MODE_ROWS=ROWS
  --probe-force-cull-mode-class CLASS
                      Set DXMT9_PROBE_FORCE_CULL_MODE_CLASS=CLASS. Accepted
                      values match split-large indexed filters
  --probe-force-cull-mode-classes CLASSES
                      Set DXMT9_PROBE_FORCE_CULL_MODE_CLASSES=CLASSES. Values
                      are ANDed, e.g. opaque-depth-write,large4096
  --force-cull-mode MODE
                      Set DXMT_DEBUG_FORCE_CULL_MODE=MODE where MODE is one of
                      none, front, or back for cull/backend shape A/B probes
  --measure-index-reuse
                      Set DXMT9_MEASURE_INDEX_REUSE=1 to scan accessible
                      index buffers and report per-encoder unique index counts
  --measure-index-cache-opt-candidate
                      Set DXMT9_MEASURE_INDEX_CACHE_OPT_CANDIDATE=1 to build
                      a cache-aware candidate index order without submitting it
                      and report original-vs-candidate cache-miss estimates;
                      implies --measure-index-reuse
  --dump-indexed-geometry
                      Dump selected indexed triangle raw index/stream0 payloads
                      under traces/<run-id>/analysis/geometry. Uses the
                      reverse-indexed row/class/span filters and encoder draw
                      range; implies --measure-index-reuse
  --dump-indexed-geometry-cbufs
                      Also dump real VsConsts/PsConsts/FfpVsConsts/FfpPsConsts
                      bytes beside each selected geometry payload
  --dump-indexed-geometry-max-draws N
                      Set DXMT9_DUMP_INDEXED_GEOMETRY_MAX_DRAWS=N
                      (default: 16)
  --dump-indexed-geometry-vs HASH
                      Set DXMT9_DUMP_INDEXED_GEOMETRY_VS=HASH to dump only
                      draws using this vertex shader hash
  --dump-indexed-geometry-ps HASH
                      Set DXMT9_DUMP_INDEXED_GEOMETRY_PS=HASH to dump only
                      draws using this pixel shader hash
  --dump-indexed-geometry-texture0 HANDLE
                      Set DXMT9_DUMP_INDEXED_GEOMETRY_TEXTURE0=HANDLE to dump
                      only draws with this texture bound at fragment stage 0
  --dump-indexed-geometry-texture0-width N
                      Set DXMT9_DUMP_INDEXED_GEOMETRY_TEXTURE0_WIDTH=N
  --dump-indexed-geometry-texture0-height N
                      Set DXMT9_DUMP_INDEXED_GEOMETRY_TEXTURE0_HEIGHT=N
  --dump-indexed-geometry-texture0-format N
                      Set DXMT9_DUMP_INDEXED_GEOMETRY_TEXTURE0_FORMAT=N
  --dump-depth-attachment-handle HANDLE
                      Dump one active depth attachment by handle for mini
                      replay --depth-input. Pairs with optional seq/enc gates.
  --dump-depth-attachment-seq N
                      Set DXMT9_DUMP_DEPTH_ATTACHMENT_SEQ=N
  --dump-depth-attachment-enc N
                      Set DXMT9_DUMP_DEPTH_ATTACHMENT_ENC=N
  --dump-depth-attachment-path PATH
                      Output path for the raw depth sidecar. Relative paths
                      are resolved under the repository root; default is
                      traces/<run-id>/analysis/frame<N>-depth.bin
  --dump-color-attachment-handle HANDLE
                      Dump one active color attachment by handle for pass
                      color-history/final-writer diagnostics. If omitted,
                      seq/enc gates dump color attachment index 0 by default.
  --dump-color-attachment-index N
                      Set DXMT9_DUMP_COLOR_ATTACHMENT_INDEX=N. Useful when
                      color RT handles are not stable across runs.
  --dump-color-attachment-after-draw
                      Set DXMT9_DUMP_COLOR_ATTACHMENT_AFTER_DRAW=1. Diagnostic
                      only: ends the render encoder after a selected draw,
                      dumps color, and resumes the pass with Load.
  --dump-color-attachment-draw N
                      Set DXMT9_DUMP_COLOR_ATTACHMENT_DRAW=N. Uses the same
                      0-based encoder draw index convention as
                      --probe-indexed-triangle-encoder-draw-min/max.
  --dump-color-attachment-draws LIST
                      Set DXMT9_DUMP_COLOR_ATTACHMENT_DRAWS=LIST and write
                      one color sidecar per matching draw. Requires
                      --dump-color-attachment-dir.
  --dump-color-attachment-command-index N
                      Set DXMT9_DUMP_COLOR_ATTACHMENT_COMMAND_INDEX=N. Uses
                      the original replay command index, so it remains useful
                      after diagnostic render-encoder splits.
  --dump-color-attachment-command-index-min N
                      Set DXMT9_DUMP_COLOR_ATTACHMENT_COMMAND_INDEX_MIN=N
  --dump-color-attachment-command-index-max N
                      Set DXMT9_DUMP_COLOR_ATTACHMENT_COMMAND_INDEX_MAX=N
  --dump-color-attachment-texture0 HANDLE
                      Set DXMT9_DUMP_COLOR_ATTACHMENT_TEXTURE0=HANDLE so the
                      after-draw dump only triggers on matching texture0.
  --dump-color-attachment-texture0s LIST
                      Set DXMT9_DUMP_COLOR_ATTACHMENT_TEXTURE0S=LIST and write
                      one color sidecar per matching texture0. Requires
                      --dump-color-attachment-dir.
  --dump-color-attachment-seq N
                      Set DXMT9_DUMP_COLOR_ATTACHMENT_SEQ=N
  --dump-color-attachment-enc N
                      Set DXMT9_DUMP_COLOR_ATTACHMENT_ENC=N
  --dump-color-attachment-path PATH
                      Output path for the raw color sidecar. Relative paths
                      are resolved under the repository root; default is
                      traces/<run-id>/analysis/frame<N>-color.bin
  --dump-color-attachment-dir DIR
                      Output directory for multi-draw color sidecars. Relative
                      paths are resolved under the repository root; generated
                      files are named color-s<seq>-e<enc>-after-draw-d<N>.bin.
  --dump-color-attachment-roi-summary-path PATH
                      Set DXMT9_DUMP_COLOR_ATTACHMENT_ROI_SUMMARY_PATH. Writes
                      a compact CSV with ROI max/bright/white stats instead of
                      requiring full attachment sidecar files.
  --dump-color-attachment-roi L,T,R,B[:NAME]
                      Append one DXMT9_DUMP_COLOR_ATTACHMENT_ROIS entry.
                      Multiple options are separated with semicolons.
  --dump-color-attachment-bright-threshold N
                      Set DXMT9_DUMP_COLOR_ATTACHMENT_BRIGHT_THRESHOLD
                      (default in dxmt9: 220)
  --dump-color-attachment-white-threshold N
                      Set DXMT9_DUMP_COLOR_ATTACHMENT_WHITE_THRESHOLD
                      (default in dxmt9: 240)
  --dump-color-attachment-warm-red-threshold N
                      Set DXMT9_DUMP_COLOR_ATTACHMENT_WARM_RED_THRESHOLD
                      (default in dxmt9: 180)
  --dump-color-attachment-warm-green-threshold N
                      Set DXMT9_DUMP_COLOR_ATTACHMENT_WARM_GREEN_THRESHOLD
                      (default in dxmt9: 110)
  --dump-color-attachment-warm-blue-margin N
                      Set DXMT9_DUMP_COLOR_ATTACHMENT_WARM_BLUE_MARGIN
                      (default in dxmt9: 32). Warm pixels require
                      blue <= red + margin, which rejects cyan beam hits.
  --dump-draw-texture-handles LIST
                      Dump live shader-read texture sidecars for a comma/space
                      separated handle list. Pairs with optional seq/enc gates.
  --dump-draw-texture0-any
                      Dump every unique live shader-read fragment texture0
                      sidecar in the selected seq/enc gate
  --dump-draw-texture0-width N
                      Dump live shader-read fragment texture0 sidecars whose
                      D3D descriptor width matches N
  --dump-draw-texture0-height N
                      Dump live shader-read fragment texture0 sidecars whose
                      D3D descriptor height matches N
  --dump-draw-texture0-format N
                      Dump live shader-read fragment texture0 sidecars whose
                      D3D descriptor format matches N
  --dump-draw-texture-seq N
                      Set DXMT9_DUMP_DRAW_TEXTURE_SEQ=N
  --dump-draw-texture-enc N
                      Set DXMT9_DUMP_DRAW_TEXTURE_ENC=N
  --dump-draw-texture-dir DIR
                      Output directory for raw texture sidecars. Relative paths
                      are resolved under the repository root; default is
                      traces/<run-id>/analysis/textures
  --aggressive-color-dontcare
                      Set DXMT9_AGGRESSIVE_COLOR_DONTCARE=1 for the run
  --aggressive-depth-dontcare
                      Set DXMT9_AGGRESSIVE_DEPTH_DONTCARE=1 for the run
  --disable-cull      Set DXMT_DISABLE_CULL=1 for render-state shape A/B
  --disable-scissor   Set DXMT_DISABLE_SCISSOR=1 for render-state shape A/B
  --disable-alpha-test
                      Set DXMT_DISABLE_ALPHA_TEST=1 to strip shader alpha-test
                      discard and isolate it from force-fragment-color
  --disable-fog       Set DXMT_DISABLE_FOG=1 to strip shader fog blend and
                      isolate fog source shape from force-fragment-color
  --force-texture-white
                      Set DXMT_FORCE_TEXTURE_WHITE=1 to replace fragment
                      texture samples with float4(1) while keeping shader body
  --probe-force-texture-white
                      Set DXMT9_PROBE_FORCE_TEXTURE_WHITE=1 to replace
                      fragment texture samples with float4(1) only for
                      selected indexed triangle-list draws
  --probe-force-texture-white-row SEQ/ENC
                      Set DXMT9_PROBE_FORCE_TEXTURE_WHITE_ROW=SEQ/ENC
  --probe-force-texture-white-rows ROWS
                      Set DXMT9_PROBE_FORCE_TEXTURE_WHITE_ROWS=ROWS
  --probe-force-texture-white-class CLASS
                      Set DXMT9_PROBE_FORCE_TEXTURE_WHITE_CLASS=CLASS.
                      Accepted values match split-large indexed filters
  --probe-force-texture-white-classes CLASSES
                      Set DXMT9_PROBE_FORCE_TEXTURE_WHITE_CLASSES=CLASSES.
                      Values are ANDed, e.g. depth-read,screen-blend,textured
  --probe-force-texture-white-texture0 HANDLE
                      Set DXMT9_PROBE_FORCE_TEXTURE_WHITE_TEXTURE0=HANDLE
  --probe-force-texture-white-texture0-width N
                      Set DXMT9_PROBE_FORCE_TEXTURE_WHITE_TEXTURE0_WIDTH=N
  --probe-force-texture-white-texture0-height N
                      Set DXMT9_PROBE_FORCE_TEXTURE_WHITE_TEXTURE0_HEIGHT=N
  --probe-force-texture-white-texture0-format N
                      Set DXMT9_PROBE_FORCE_TEXTURE_WHITE_TEXTURE0_FORMAT=N
  --probe-force-texture-white-draw-ordinal N
                      Set DXMT9_PROBE_FORCE_TEXTURE_WHITE_DRAW_ORDINALS=N
  --probe-force-texture-white-draw-ordinals LIST
                      Set DXMT9_PROBE_FORCE_TEXTURE_WHITE_DRAW_ORDINALS=LIST
  --probe-force-texture-white-draw-ordinal-min N
                      Set DXMT9_PROBE_FORCE_TEXTURE_WHITE_DRAW_ORDINAL_MIN=N
  --probe-force-texture-white-draw-ordinal-max N
                      Set DXMT9_PROBE_FORCE_TEXTURE_WHITE_DRAW_ORDINAL_MAX=N
  --probe-force-texture-white-command-index N
                      Set DXMT9_PROBE_FORCE_TEXTURE_WHITE_COMMAND_INDEXES=N
  --probe-force-texture-white-command-indexes LIST
                      Set DXMT9_PROBE_FORCE_TEXTURE_WHITE_COMMAND_INDEXES=LIST
  --probe-force-texture-white-command-index-min N
                      Set DXMT9_PROBE_FORCE_TEXTURE_WHITE_COMMAND_INDEX_MIN=N
  --probe-force-texture-white-command-index-max N
                      Set DXMT9_PROBE_FORCE_TEXTURE_WHITE_COMMAND_INDEX_MAX=N
  --probe-force-texture-white-command-draw-index N
                      Set DXMT9_PROBE_FORCE_TEXTURE_WHITE_COMMAND_DRAW_INDEXES=N
  --probe-force-texture-white-command-draw-indexes LIST
                      Set DXMT9_PROBE_FORCE_TEXTURE_WHITE_COMMAND_DRAW_INDEXES=LIST
  --probe-force-texture-white-command-draw-index-min N
                      Set DXMT9_PROBE_FORCE_TEXTURE_WHITE_COMMAND_DRAW_INDEX_MIN=N
  --probe-force-texture-white-command-draw-index-max N
                      Set DXMT9_PROBE_FORCE_TEXTURE_WHITE_COMMAND_DRAW_INDEX_MAX=N
  --probe-disable-alpha-blend
                      Set DXMT9_PROBE_DISABLE_ALPHA_BLEND=1 for blend state A/B
  --probe-disable-alpha-blend-row SEQ/ENC
                      Set DXMT9_PROBE_DISABLE_ALPHA_BLEND_ROW=SEQ/ENC and
                      constrain blend-off to selected indexed triangle draws
  --probe-disable-alpha-blend-rows ROWS
                      Set DXMT9_PROBE_DISABLE_ALPHA_BLEND_ROWS=ROWS
  --probe-disable-alpha-blend-class CLASS
                      Set DXMT9_PROBE_DISABLE_ALPHA_BLEND_CLASS=CLASS.
                      Accepted values match split-large indexed filters
  --probe-disable-alpha-blend-classes CLASSES
                      Set DXMT9_PROBE_DISABLE_ALPHA_BLEND_CLASSES=CLASSES.
                      Values are ANDed, e.g. large4096,screen-blend
  --probe-disable-alpha-blend-texture0 HANDLE
                      Set DXMT9_PROBE_DISABLE_ALPHA_BLEND_TEXTURE0=HANDLE
  --probe-disable-alpha-blend-texture0-width N
                      Set DXMT9_PROBE_DISABLE_ALPHA_BLEND_TEXTURE0_WIDTH=N
  --probe-disable-alpha-blend-texture0-height N
                      Set DXMT9_PROBE_DISABLE_ALPHA_BLEND_TEXTURE0_HEIGHT=N
  --probe-disable-alpha-blend-texture0-format N
                      Set DXMT9_PROBE_DISABLE_ALPHA_BLEND_TEXTURE0_FORMAT=N
  --probe-disable-depth-write
                      Set DXMT9_PROBE_DISABLE_DEPTH_WRITE=1 for depth-write A/B
  --probe-disable-depth-write-row SEQ/ENC
                      Set DXMT9_PROBE_DISABLE_DEPTH_WRITE_ROW=SEQ/ENC and
                      constrain depth-write-off to selected indexed triangle draws
  --probe-disable-depth-write-rows ROWS
                      Set DXMT9_PROBE_DISABLE_DEPTH_WRITE_ROWS=ROWS
  --probe-disable-depth-write-class CLASS
                      Set DXMT9_PROBE_DISABLE_DEPTH_WRITE_CLASS=CLASS.
                      Accepted values match split-large indexed filters
  --probe-disable-depth-write-classes CLASSES
                      Set DXMT9_PROBE_DISABLE_DEPTH_WRITE_CLASSES=CLASSES.
                      Values are ANDed, e.g. large4096,screen-blend
  --probe-depth-func-always
                      Set DXMT9_PROBE_DEPTH_FUNC_ALWAYS=1 to keep depth writes
                      but force the depth compare function to Always
  --probe-depth-func-always-row SEQ/ENC
                      Set DXMT9_PROBE_DEPTH_FUNC_ALWAYS_ROW=SEQ/ENC and
                      constrain depth-func-always to selected indexed triangle draws
  --probe-depth-func-always-rows ROWS
                      Set DXMT9_PROBE_DEPTH_FUNC_ALWAYS_ROWS=ROWS
  --probe-depth-func-always-class CLASS
                      Set DXMT9_PROBE_DEPTH_FUNC_ALWAYS_CLASS=CLASS. Accepted
                      values match split-large indexed filters
  --probe-depth-func-always-classes CLASSES
                      Set DXMT9_PROBE_DEPTH_FUNC_ALWAYS_CLASSES=CLASSES.
                      Values are ANDed, e.g. opaque-depth-write,large4096
  --probe-depth-func-always-texture0 HANDLE
                      Set DXMT9_PROBE_DEPTH_FUNC_ALWAYS_TEXTURE0=HANDLE
  --probe-depth-func-always-texture0-width N
                      Set DXMT9_PROBE_DEPTH_FUNC_ALWAYS_TEXTURE0_WIDTH=N
  --probe-depth-func-always-texture0-height N
                      Set DXMT9_PROBE_DEPTH_FUNC_ALWAYS_TEXTURE0_HEIGHT=N
  --probe-depth-func-always-texture0-format N
                      Set DXMT9_PROBE_DEPTH_FUNC_ALWAYS_TEXTURE0_FORMAT=N
  --probe-fragmentless-depth-only
                      Set DXMT9_PROBE_FRAGMENTLESS_DEPTH_ONLY=1 for scoped
                      depth-only backend-shape A/B. The encoder still gates
                      color-write=0, depth-write, no alpha/stencil/clip/A2C,
                      and the pipeline cache rejects discard/depth-output FS.
  --probe-fragmentless-depth-only-row SEQ/ENC
                      Set DXMT9_PROBE_FRAGMENTLESS_DEPTH_ONLY_ROW=SEQ/ENC
  --probe-fragmentless-depth-only-rows ROWS
                      Set DXMT9_PROBE_FRAGMENTLESS_DEPTH_ONLY_ROWS=ROWS
  --probe-fragmentless-depth-only-class CLASS
                      Set DXMT9_PROBE_FRAGMENTLESS_DEPTH_ONLY_CLASS=CLASS.
                      Accepted values match split-large indexed filters
  --probe-fragmentless-depth-only-classes CLASSES
                      Set DXMT9_PROBE_FRAGMENTLESS_DEPTH_ONLY_CLASSES=CLASSES.
                      Values are ANDed, e.g. large4096,no-alpha-blend
  --probe-fragmentless-depth-only-keep-vsout
                      Set DXMT9_PROBE_FRAGMENTLESS_DEPTH_ONLY_KEEP_VSOUT=1.
                      Keeps the ordinary VSOut layout while omitting the
                      fragment function, isolating VSOut-shape effects from
                      fragmentless depth-only routing.
  --force-visible     Set DXMT_DEBUG_FORCE_VISIBLE=1 for visibility/state A/B
  --effect-draw-trace
                      Set DXMT9_EFFECT_DRAW_TRACE=1 and log alpha-blended
                      textured draw state to dxmt9.log
  --effect-draw-trace-seq N
                      Set DXMT9_EFFECT_DRAW_TRACE_SEQ=N and enable effect trace
  --effect-draw-trace-seq-min N
                      Set DXMT9_EFFECT_DRAW_TRACE_SEQ_MIN=N and enable
                      effect trace
  --effect-draw-trace-seq-max N
                      Set DXMT9_EFFECT_DRAW_TRACE_SEQ_MAX=N and enable
                      effect trace
  --effect-draw-trace-enc N
                      Set DXMT9_EFFECT_DRAW_TRACE_ENC=N and enable effect trace
  --effect-draw-trace-texture0 HANDLE
                      Set DXMT9_EFFECT_DRAW_TRACE_TEXTURE0=HANDLE and enable
                      effect trace
  --effect-draw-trace-texture0-width N
                      Set DXMT9_EFFECT_DRAW_TRACE_TEXTURE0_WIDTH=N and enable
                      effect trace
  --effect-draw-trace-texture0-height N
                      Set DXMT9_EFFECT_DRAW_TRACE_TEXTURE0_HEIGHT=N and enable
                      effect trace
  --effect-draw-trace-texture0-format N
                      Set DXMT9_EFFECT_DRAW_TRACE_TEXTURE0_FORMAT=N and enable
                      effect trace
  --effect-draw-trace-primitive-type N
                      Set DXMT9_EFFECT_DRAW_TRACE_PRIMITIVE_TYPE=N and enable
                      effect trace. PrimitiveType enum: PointList=0,
                      TriangleList=3
  --effect-draw-trace-point-sprite
                      Set DXMT9_EFFECT_DRAW_TRACE_POINT_SPRITE=1 and log only
                      POINTSPRITEENABLE + PointList effect draw candidates
  --effect-draw-trace-include-non-alpha
                      Set DXMT9_EFFECT_DRAW_TRACE_INCLUDE_NON_ALPHA=1 so
                      trace rows are not limited to alpha-blended draws
  --effect-draw-trace-include-untextured
                      Set DXMT9_EFFECT_DRAW_TRACE_INCLUDE_UNTEXTURED=1 so
                      trace rows are not limited to texture-bound draws
  --effect-draw-trace-geometry
                      Set DXMT9_EFFECT_DRAW_TRACE_GEOMETRY=1 and add
                      stream0/index bbox, UV, and diffuse-alpha summaries
                      for matching indexed effect draws
  --effect-draw-trace-geometry-max-refs N
                      Set DXMT9_EFFECT_DRAW_TRACE_GEOMETRY_MAX_REFS=N
  --visibility-scout  Set DXMT9_VISIBILITY_SCOUT=1 and write Metal
                      visibility counts under traces/<run-id>/analysis
  --visibility-scout-row SEQ/ENC
                      Restrict visibility scout to one render encoder
  --visibility-scout-rows ROWS
                      Restrict visibility scout to comma-separated SEQ/ENC rows
  --visibility-scout-path PATH
                      Override DXMT9_VISIBILITY_SCOUT_PATH
  --visibility-scout-draw-indices LIST
                      Include an optional metal_draw_index window/range such as
                      36..37 in the post-run visibility summary
  --visibility-scout-summary-output PATH
                      Override the post-run visibility Markdown summary path
  --visibility-scout-summary-csv-output PATH
                      Override the post-run visibility CSV summary path
  --visibility-scout-summary-limit N
                      Number of visibility class rows to print in Markdown
  --compare-baseline-output PATH
                      After the run, compare result.json counters against this baseline output dir/result.json
  --baseline-joined PATH
                      Include this Xcode+dxmt joined CSV in the printed finalizer command
                      for after-Xcode counter comparison
  --semantic-image-policy exact|lsb1
                      Include a same-input mini-replay image gate in the
                      printed finalizer command. exact is the default
                      correctness policy; lsb1 is only for explicit
                      blend-rounding tolerance decisions.
  --semantic-image-before PATH
                      Baseline image for --semantic-image-policy
  --semantic-image-after PATH
                      Candidate image for --semantic-image-policy
  --semantic-image-output PATH
                      Finalizer image comparison report path
  --semantic-image-summary-output PATH
                      Finalizer image comparison CSV path
  --semantic-image-diff-output PATH
                      Finalizer image comparison diff path
  --semantic-image-min-active-pct N
                      Min before/after active pixel percentage for semantic
                      image gates (default: 1)
  --require-color-dontcare-increase
                      Compare gate: candidate color StoreActionDontCare count must increase
  --require-depth-dontcare-increase
                      Compare gate: candidate depth StoreActionDontCare count must increase
  --require-tile-preservation-decrease
                      Compare gate: candidate tile-preservation MiB must decrease
  --require-tile-preservation-not-increase
                      Compare gate: candidate tile-preservation MiB must not increase
  --require-command-buffers-per-present-not-increase
                      Compare gate: command buffers per present must not increase
  --require-render-passes-per-present-not-increase
                      Compare gate: render passes per present must not increase
  --require-render-pass-carry-promotion-gates
                      Compare gate: H128 P4/locality/error promotion bundle must pass
  --require-encoder-final-end-reason-not-increase
                      Compare gate: encoder sidecar final end-reason per present must not increase
  --require-encoder-final-same-key-reopen-not-increase
                      Compare gate: encoder sidecar final same-key reopen per present must not increase
  --require-encoder-color-load-not-increase
                      Compare gate: encoder sidecar color load MiB per present must not increase
  --require-encoder-depth-load-not-increase
                      Compare gate: encoder sidecar depth load MiB per present must not increase
  --require-draw-run-records-increase
                      Compare gate: commit_chunk_draw_run_records must increase
  --require-draw-run-records-per-submit-increase
                      Compare gate: draw-run records per submit must increase
  --require-binding-overrides-present
                      Compare gate: stream/IB draw-run binding override records must be nonzero
  --require-const-upload-passthrough-present
                      Compare gate: fallback draw batching must cross const-upload records
  --require-draw-submission-batch-present
                      Compare gate: fallback draw submission batch counters must be nonzero
  --require-const-upload-break-bytes-decrease
                      Compare gate: const-upload draw-run break bytes must decrease
  --max-const-upload-break-count-ratio N
                      Compare gate: max allowed after/before const-upload break count ratio
  --require-encode-draw-cpu-decrease
                      Compare gate: encode_draw_cpu_ms must decrease
  --require-completion-present-wait-decrease
                      Compare gate: completion_present_wait_ms must decrease
  --require-completion-wait-with-enqueue-increase
                      Compare gate: completion_wait_with_enqueue_ms must increase
  --require-completion-wait-without-enqueue-decrease
                      Compare gate: completion_wait_without_enqueue_ms must decrease
  --require-completion-present-wait-with-enqueue-increase
                      Compare gate: Present wait-with-enqueue ms must increase
  --require-completion-present-wait-without-enqueue-decrease
                      Compare gate: Present wait-without-enqueue ms must decrease
  --require-encode-ready-depth-gt1-increase
                      Compare gate: encode ready-depth >1 samples must increase
  --require-commit-chunk-replay-cpu-per-present-decrease
                      Compare gate: commit replay CPU per present must decrease
  --require-queue-draw-submission-cpu-per-present-decrease
                      Compare gate: queue draw-submission CPU per present must decrease
  --require-snapshot-cpu-per-present-decrease
                      Compare gate: snapshot CPU per present must decrease
  --require-snapshot-cache-lookup-cpu-per-present-decrease
                      Compare gate: snapshot cache lookup CPU per present must decrease
  --require-snapshot-cache-uniform-build-cpu-per-present-decrease
                      Compare gate: snapshot uniform-build CPU per present must decrease
  --require-snapshot-cache-uniform-hash-cpu-per-present-decrease
                      Compare gate: snapshot uniform-hash CPU per present must decrease
  --require-batch-miss-uniform-build-cpu-per-present-decrease
                      Compare gate: batch-miss uniform-build CPU per present must decrease
  --require-batch-miss-uniform-hash-cpu-per-present-decrease
                      Compare gate: batch-miss uniform-hash CPU per present must decrease
  --require-batch-miss-vs-const-hash-cpu-per-present-decrease
                      Compare gate: batch-miss VS const hash CPU per present must decrease
  --require-batch-miss-ps-const-hash-cpu-per-present-decrease
                      Compare gate: batch-miss PS const hash CPU per present must decrease
  --require-batch-miss-nonconst-hash-cpu-per-present-decrease
                      Compare gate: batch-miss nonconst hash CPU per present must decrease
  --require-snapshot-uniform-copy-cpu-per-present-decrease
                      Compare gate: snapshot uniform copy CPU per present must decrease
  --require-submit-draw-run-batch-append-uniform-cpu-per-present-decrease
                      Compare gate: backend uniform append CPU per present must decrease
  --require-draw-uniform-payload-lookup-cpu-per-present-decrease
                      Compare gate: uniform payload lookup CPU per present must decrease
  --require-draw-uniform-payload-append-copy-cpu-per-present-decrease
                      Compare gate: uniform payload append-copy CPU per present must decrease
  --require-argbuf-setup-cpu-per-present-decrease
                      Compare gate: argbuf setup CPU per present must decrease
  --require-argbuf-open-cpu-per-present-decrease
                      Compare gate: argbuf open CPU per present must decrease
  --require-argbuf-cbuf-update-cpu-per-present-decrease
                      Compare gate: argbuf cbuf update CPU per present must decrease
  --require-argbuf-cbuf-update-vs-cpu-per-present-decrease
                      Compare gate: argbuf VS cbuf update CPU per present must decrease
  --require-uniform-compact-saved-bytes-present
                      Compare gate: compact uniform payload saved bytes per present must be nonzero
  --require-current-uniform-compact-saved-bytes-present
                      Current-run gate: compact uniform payload saved bytes
                      per present must be nonzero; does not require a baseline.
  --require-snapshot-state-elided-present
                      Compare gate: state snapshot elisions per present must be nonzero
  --require-discarded-state-not-increase
                      Compare gate: discarded backend state per present must not increase
  --require-submission-carrier-bytes-per-record-decrease
                      Compare gate: submission carrier bytes per record must decrease
  --require-submission-carrier-uniform-storage-per-record-decrease
                      Compare gate: submission carrier full-uniform storage bytes per record must decrease
  --require-encode-chunk-cpu-per-present-decrease
                      Compare gate: encode_chunk_cpu_ms per present must decrease
  --require-no-enqueue-commit-entry-to-publish-decrease
                      Compare gate: no-enqueue commit-entry to publish ms must decrease
  --require-no-enqueue-publish-to-encode-dequeue-decrease
                      Compare gate: no-enqueue publish to encode-dequeue ms must decrease
  --require-no-enqueue-encode-dequeue-to-commit-decrease
                      Compare gate: no-enqueue encode-dequeue to Metal commit ms must decrease
  --require-no-enqueue-wait-to-next-enqueue-decrease
                      Compare gate: no-enqueue wait-to-next-enqueue ms must decrease
  --require-no-enqueue-before-publish-closure-decrease
                      Compare gate: no-enqueue before-publish closure ms must decrease
  --require-no-enqueue-before-publish-inter-replay-gap-decrease
                      Compare gate: no-enqueue before-publish inter-replay gap ms must decrease
  --require-pe-focused-between-call-gap-residual-decrease
                      Compare gate: focused PE between-call residual ms per present must decrease
  --max-gpu-command-buffer-regression-ms N
                      Compare gate: max allowed gpu_command_buffer_time_ms regression
  --require-result-json
                      Finalizer gate: fail instead of using dxmt9.log partial-run counters
  --allow-partial-stable-frame-proof
                      Finalizer gate: keep stable-frame Xcode/geometry gates
                      but allow timeout-finalized runs without result.json.
                      Explicit --require-result-json or --baseline-output still
                      requires result.json.
  --require-top-gpu-decrease
                      Finalizer Xcode gate: top-N GPU time must decrease
  --require-top-buffer-write-decrease
                      Finalizer Xcode gate: top-N buffer write MiB must decrease
  --require-top-vs-buffer-write-decrease
                      Finalizer Xcode gate: top-N VS buffer write MiB must decrease
  --require-top-unexplained-buffer-write-decrease
                      Finalizer Xcode gate: top-N buffer write not explained
                      by dxmt CPU writers must decrease
  --require-stream-handle-churn-decrease
                      Finalizer Xcode gate: top-N stream handle churn must decrease
  --require-ib-handle-churn-decrease
                      Finalizer Xcode gate: top-N IB handle churn must decrease
  --require-argbuf-cbuf-decrease
                      Finalizer Xcode gate: top-N dxmt argbuf cbuf MiB must decrease
  --require-transient-decrease
                      Finalizer Xcode gate: top-N dxmt transient MiB must decrease
  --require-top-gpu-share-increase
                      Finalizer Xcode gate: top-N GPU share must increase
  --require-top-row-key-match
                      Finalizer Xcode gate: top-N seq/enc row key sets must match
  --require-stable-frame-proof
                      Finalizer proof preset: require result.json, counter/join
                      coverage, PSO attribution, top row-key match, top GPU/VS/
                      unexplained write decrease, and <=5% top geometry drift
  --require-tvb-mechanism-proof
                      Finalizer gate: top hidden backend write, VS buffer
                      write, VS invocations, and GPU time must all strictly
                      decrease; use for row-local backend-shape mechanism proofs
  --require-cache-opt-apply-proof
                      Finalizer proof preset for cache-opt apply runs: stable
                      frame proof plus target rows' actual LRU32 miss, VS
                      buffer write, and VS invocation decreases; requires
                      --target-row-key
  --require-opaque-depth-index-cache-proof
                      Finalizer proof preset for production-shaped opaque
                      depth cached-index opt-in: stable-frame proof plus target
                      cache-opt telemetry, reordered-cache hits, target VS
                      write/invocation decreases, and --target-row-key.
                      Also requires --optimize-opaque-depth-index-cache and
                      enables index-reuse/cache-opt-candidate telemetry.
  --require-screen-blend-cache-proof
                      Finalizer proof preset for screen-blend cached-index
                      opt-in: stable-frame proof plus target cache-opt
                      telemetry, reordered-cache hits, target VS write/
                      invocation decreases, and a same-input semantic image
                      gate; requires --target-row-key and --semantic-image-
                      policy with before/after images.
                      Also requires --optimize-screen-blend-index-cache and
                      enables index-reuse/cache-opt-candidate telemetry.
  --require-semantic-image-proof
                      Finalizer gate: require --semantic-image-policy with
                      before/after images and fail if that image comparison
                      fails. Automatically required when unsafe nonopaque
                      cache-opt apply uses --require-cache-opt-apply-proof.
  --require-top-pso-attribution
                      Finalizer gate: top Xcode encoder rows must have PSO/VSOut
                      attribution near draw frequency
  --require-xcode-counter-coverage
                      Finalizer gate: Xcode CSV must include required bottleneck counters
                      and RenderPass[seq=...,enc=...] labels
  --require-dxmt-join-coverage
                      Finalizer gate: top Xcode encoder rows must join to dxmt
                      encoder attribution
  --require-shader-dump-matches
                      Finalizer gate: top render encoder shader hashes must
                      match dumped MSL files unambiguously (use with --dump-shaders)
  --min-top-pso-samples-per-draw N
                      Finalizer gate threshold for PSO samples/draw (default: 0.90)
  --min-top-dxmt-joined-fraction N
                      Finalizer gate threshold for top dxmt joined row fraction
                      (default: 1.0)
  --max-top-gpu-regression-ms N
                      Finalizer Xcode gate: max allowed top-N GPU time regression
  --max-top-buffer-write-regression-mib N
                      Finalizer Xcode gate: max allowed top-N buffer write regression
  --target-row-key ROW
                      Finalizer Xcode gate: target seq/enc row key, e.g. 50/1;
                      repeat to exclude all mutated rows from non-target guards
  --require-target-index-cache-miss32-decrease
                      Finalizer Xcode gate: target rows' actual dxmt LRU32 miss
                      estimate must decrease, catching cache-opt apply no-ops
  --require-target-index-cache-opt-miss32-decrease
                      Finalizer Xcode gate: after target rows' cache-opt
                      candidate/effective LRU32 estimate must decrease
  --require-target-reordered-index-cache-hits
                      Finalizer Xcode gate: every after target row must have
                      positive reordered index cache hits
  --require-target-vs-buffer-write-decrease
                      Finalizer Xcode gate: target rows' VS buffer write must decrease
  --require-target-vs-invocations-decrease
                      Finalizer Xcode gate: target rows' VS invocation count must decrease
  --max-non-target-gpu-regression-ms N
                      Finalizer Xcode gate: max matched GPU ms regression for
                      before top-N rows excluding --target-row-key rows
  --max-non-target-vs-buffer-write-regression-mib N
                      Finalizer Xcode gate: max matched VS buffer write MiB
                      regression for before top-N non-target rows
  --max-non-target-vs-invocations-regression-ratio N
                      Finalizer Xcode gate: max matched relative VS invocation
                      regression for before top-N non-target rows
  --max-non-target-draw-call-delta-ratio N
                      Finalizer Xcode gate: max relative draw-count drift for
                      before top-N non-target rows
  --max-non-target-vertex-count-delta-ratio N
                      Finalizer Xcode gate: max relative vertex-count drift for
                      before top-N non-target rows
  --max-non-target-triangle-delta-ratio N
                      Finalizer Xcode gate: max relative triangle-count drift for
                      before top-N non-target rows
  --max-top-unexplained-buffer-write-ratio N
                      Finalizer Xcode gate: max allowed unexplained top-N
                      buffer write / buffer write ratio
  --max-top-draw-call-delta-ratio N
                      Finalizer Xcode gate: max allowed relative top-N draw-count drift
  --max-top-vertex-count-delta-ratio N
                      Finalizer Xcode gate: max allowed relative top-N vertex-count drift
  --max-top-triangle-delta-ratio N
                      Finalizer Xcode gate: max allowed relative top-N triangle-count drift
  --top N             GPU-time-ranked encoder count for finalizer top-N gates
                      and Xcode comparison (default: 3)
  --hot-gpu-share PCT GPU share target for finalizer report-only Hot Set
                      Aggregate (default: 95.0)
  --min-free-mb N     Required free space before launch (default: 2048 with gputrace, 256 without).
                      Gputrace runs refuse values below 2048 unless
                      DXMT_3DMARK05_ALLOW_LOW_TRACE_FREE_MB=1 is set.
  --dry-run           Print paths, env, and command without launching
  -h, --help          Show this help
USAGE
}

while (($#)); do
  case "$1" in
    --suffix)
      suffix=${2:?missing value for --suffix}
      shift 2
      ;;
    --frame)
      frame=${2:?missing value for --frame}
      shift 2
      ;;
    --timeout)
      timeout=${2:?missing value for --timeout}
      shift 2
      ;;
    --wait-unlocked-sec)
      wait_unlocked_sec=${2:?missing value for --wait-unlocked-sec}
      shift 2
      ;;
    --wait-unlocked-interval-sec)
      wait_unlocked_interval_sec=${2:?missing value for --wait-unlocked-interval-sec}
      shift 2
      ;;
    --keep-frontmost)
      keep_frontmost=1
      shift
      ;;
    --keep-frontmost-interval-sec)
      keep_frontmost_interval_sec=${2:?missing value for --keep-frontmost-interval-sec}
      shift 2
      ;;
    --keep-frontmost-process)
      keep_frontmost_process=${2:?missing value for --keep-frontmost-process}
      shift 2
      ;;
    --capture-delay-sec)
      capture_delay_sec=${2:?missing value for --capture-delay-sec}
      shift 2
      ;;
    --capture-frames)
      capture_frames=${2:?missing value for --capture-frames}
      shift 2
      ;;
    --capture-range)
      capture_range=${2:?missing value for --capture-range}
      shift 2
      ;;
    --capture-dir)
      capture_dir=${2:?missing value for --capture-dir}
      shift 2
      ;;
    --result-file)
      result_file=${2:?missing value for --result-file}
      shift 2
      ;;
    --encoder-breakdown-seq)
      encoder_breakdown_seq=${2:?missing value for --encoder-breakdown-seq}
      shift 2
      ;;
    --encoder-breakdown-seq-range)
      range=${2:?missing value for --encoder-breakdown-seq-range}
      if [[ ! "$range" =~ ^[0-9]+:[0-9]+$ ]]; then
        echo "--encoder-breakdown-seq-range expects MIN:MAX" >&2
        exit 2
      fi
      encoder_breakdown_seq_min=${range%%:*}
      encoder_breakdown_seq_max=${range#*:}
      shift 2
      ;;
    --encoder-breakdown-all-frames)
      encoder_breakdown_all_frames=1
      shift
      ;;
    --no-encoder-breakdown)
      encoder_breakdown_enabled=0
      shift
      ;;
    --render-pass-reentry-top)
      render_pass_reentry_top=${2:?missing value for --render-pass-reentry-top}
      shift 2
      ;;
    --dump-framegraph-dag)
      dump_framegraph_dag=1
      shift
      ;;
    --framegraph-dag-frame)
      dump_framegraph_dag=1
      framegraph_dag_frame=${2:?missing value for --framegraph-dag-frame}
      shift 2
      ;;
    --framegraph-dag-frame-radius)
      dump_framegraph_dag=1
      framegraph_dag_frame_radius=${2:?missing value for --framegraph-dag-frame-radius}
      shift 2
      ;;
    --framegraph-dag-formats)
      dump_framegraph_dag=1
      framegraph_dag_formats=${2:?missing value for --framegraph-dag-formats}
      shift 2
      ;;
    --framegraph-dag-optimize)
      dump_framegraph_dag=1
      framegraph_dag_optimize=${2:?missing value for --framegraph-dag-optimize}
      shift 2
      ;;
    --framegraph-dag-draws)
      dump_framegraph_dag=1
      framegraph_dag_draws=1
      shift
      ;;
    --frame-sampling)
      frame_sampling=1
      shift
      ;;
    --open-cb-preencode-tail-present)
      open_cb_preencode_tail_present=1
      shift
      ;;
    --open-cb-carry-render-session)
      open_cb_carry_render_session=1
      shift
      ;;
    --open-cb-semantic-boundary-publish)
      open_cb_semantic_boundary_publish=1
      shift
      ;;
    --open-cb-cpu-ready-command-limit)
      open_cb_cpu_ready_command_limit=${2:?missing value for --open-cb-cpu-ready-command-limit}
      shift 2
      ;;
    --open-cb-writer-active-cpu-ready-publish)
      open_cb_writer_active_cpu_ready_publish=1
      shift
      ;;
    --open-cb-active-wait-cpu-ready-append)
      open_cb_active_wait_cpu_ready_append=1
      shift
      ;;
    --open-cb-wait-start-cpu-ready-publish)
      open_cb_wait_start_cpu_ready_publish=1
      shift
      ;;
    --open-cb-semantic-boundary-release-mode)
      open_cb_semantic_boundary_release_mode=${2:?missing value for --open-cb-semantic-boundary-release-mode}
      shift 2
      ;;
    --open-cb-pending-tail-wait-us)
      open_cb_pending_tail_wait_us=${2:?missing value for --open-cb-pending-tail-wait-us}
      shift 2
      ;;
    --stage-pre-present-command-limit)
      stage_pre_present_command_limit=${2:?missing value for --stage-pre-present-command-limit}
      shift 2
      ;;
    --draw-chunk-command-limit)
      draw_chunk_command_limit=${2:?missing value for --draw-chunk-command-limit}
      shift 2
      ;;
    --enable-chunk-end-carry)
      enable_chunk_end_carry=1
      shift
      ;;
    --probe-draw-packet-actual-change)
      draw_packet_actual_change=1
      shift
      ;;
    --probe-vs-const-setter-range)
      vs_const_setter_range=1
      shift
      ;;
    --pe-recorder-stats)
      pe_recorder_stats=1
      if [[ -z "$dxmt_log_level" ]]; then
        dxmt_log_level=info
      fi
      shift
      ;;
    --pe-recorder-chunk-log)
      pe_recorder_chunk_log=1
      shift
      ;;
    --pe-flush-after-clear)
      pe_flush_after_clear=1
      shift
      ;;
    --pe-flush-after-draw)
      pe_flush_after_draw=1
      shift
      ;;
    --pe-draw-full-snapshot)
      pe_draw_full_snapshot=1
      shift
      ;;
    --pe-chunk-max-records)
      pe_chunk_max_records=${2:?missing value for --pe-chunk-max-records}
      shift 2
      ;;
    --pe-chunk-max-bytes)
      pe_chunk_max_bytes=${2:?missing value for --pe-chunk-max-bytes}
      shift 2
      ;;
    --dxmt-log-level)
      dxmt_log_level=${2:?missing value for --dxmt-log-level}
      shift 2
      ;;
    --no-gputrace)
      capture_gputrace=0
      shift
      ;;
    --metal-capture-destination)
      metal_capture_destination=${2:?missing value for --metal-capture-destination}
      metal_capture_destination_explicit=1
      shift 2
      ;;
    --xcode-developer-tools-capture)
      metal_capture_destination=developerTools
      metal_capture_destination_explicit=1
      shift
      ;;
    --with-wine-capture-layer)
      with_wine_capture_layer=1
      shift
      ;;
    --require-xcode-attach-preflight)
      require_xcode_attach_preflight=1
      shift
      ;;
    --xcode-attach-preflight-only)
      xcode_attach_preflight_only=1
      shift
      ;;
    --dump-shaders)
      dump_shaders=1
      shift
      ;;
    --trim-unused-varyings)
      trim_unused_varyings=1
      shift
      ;;
    --trim-unused-varyings-vs-hashes)
      trim_unused_varyings_vs_hashes=${2:?--trim-unused-varyings-vs-hashes requires a value}
      shift 2
      ;;
    --trim-unused-varyings-ps-hashes)
      trim_unused_varyings_ps_hashes=${2:?--trim-unused-varyings-ps-hashes requires a value}
      shift 2
      ;;
    --drop-vsout-point-size)
      drop_vsout_point_size=1
      shift
      ;;
    --probe-position-only-vsout)
      probe_position_only_vsout=1
      shift
      ;;
    --probe-half-vsout)
      probe_half_vsout=1
      shift
      ;;
    --force-fragment-color)
      force_fragment_color=1
      shift
      ;;
    --trim-vertex-temps)
      trim_vertex_temps=1
      shift
      ;;
    --trim-vs-output-scratch)
      trim_vs_output_scratch=1
      shift
      ;;
    --split-sparse-const-records)
      split_sparse_const_records=1
      shift
      ;;
    --suppress-rt-pixel-format-view)
      suppress_rt_pixel_format_view=1
      shift
      ;;
    --suppress-x8-rt-pixel-format-view)
      suppress_x8_rt_pixel_format_view=1
      shift
      ;;
    --x8-shader-alpha-fill)
      x8_shader_alpha_fill=1
      shift
      ;;
    --native-metal-base-vertex)
      native_metal_base_vertex=1
      shift
      ;;
    --optimize-screen-blend-index-order)
      optimize_screen_blend_index_order=1
      shift
      ;;
    --optimize-screen-blend-index-order-row)
      optimize_screen_blend_index_order_row=${2:?missing value for --optimize-screen-blend-index-order-row}
      shift 2
      ;;
    --optimize-screen-blend-index-order-rows)
      optimize_screen_blend_index_order_rows=${2:?missing value for --optimize-screen-blend-index-order-rows}
      shift 2
      ;;
    --optimize-screen-blend-index-order-class)
      optimize_screen_blend_index_order_class=${2:?missing value for --optimize-screen-blend-index-order-class}
      shift 2
      ;;
    --optimize-screen-blend-index-order-classes)
      optimize_screen_blend_index_order_classes=${2:?missing value for --optimize-screen-blend-index-order-classes}
      shift 2
      ;;
    --optimize-screen-blend-index-order-stream0-span-min)
      optimize_screen_blend_index_order_stream0_span_min=${2:?missing value for --optimize-screen-blend-index-order-stream0-span-min}
      shift 2
      ;;
    --optimize-screen-blend-index-cache)
      optimize_screen_blend_index_cache=1
      shift
      ;;
    --optimize-screen-blend-index-cache-min-gain-pct)
      optimize_screen_blend_index_cache_min_gain_pct=${2:?missing value for --optimize-screen-blend-index-cache-min-gain-pct}
      shift 2
      ;;
    --split-large-indexed-draws)
      split_large_indexed_draws=${2:?missing value for --split-large-indexed-draws}
      shift 2
      ;;
    --split-large-indexed-draws-stream0-span-max)
      split_large_indexed_draws_stream0_span_max=${2:?missing value for --split-large-indexed-draws-stream0-span-max}
      shift 2
      ;;
    --split-large-indexed-draws-max-chunks-per-draw)
      split_large_indexed_draws_max_chunks_per_draw=${2:?missing value for --split-large-indexed-draws-max-chunks-per-draw}
      shift 2
      ;;
    --split-large-indexed-draws-row)
      split_large_indexed_draws_row=${2:?missing value for --split-large-indexed-draws-row}
      shift 2
      ;;
    --split-large-indexed-draws-rows)
      split_large_indexed_draws_rows=${2:?missing value for --split-large-indexed-draws-rows}
      shift 2
      ;;
    --split-large-indexed-draws-class)
      split_large_indexed_draws_class=${2:?missing value for --split-large-indexed-draws-class}
      shift 2
      ;;
    --split-large-indexed-draws-classes)
      split_large_indexed_draws_classes=${2:?missing value for --split-large-indexed-draws-classes}
      shift 2
      ;;
    --force-expand-indexed)
      force_expand_indexed=1
      shift
      ;;
    --probe-force-expand-indexed)
      probe_force_expand_indexed=1
      shift
      ;;
    --probe-force-expand-indexed-row)
      probe_force_expand_indexed_row=${2:?missing value for --probe-force-expand-indexed-row}
      shift 2
      ;;
    --probe-force-expand-indexed-rows)
      probe_force_expand_indexed_rows=${2:?missing value for --probe-force-expand-indexed-rows}
      shift 2
      ;;
    --probe-force-expand-indexed-class)
      probe_force_expand_indexed_class=${2:?missing value for --probe-force-expand-indexed-class}
      shift 2
      ;;
    --probe-force-expand-indexed-classes)
      probe_force_expand_indexed_classes=${2:?missing value for --probe-force-expand-indexed-classes}
      shift 2
      ;;
    --probe-reverse-indexed-triangles)
      probe_reverse_indexed_triangles=1
      shift
      ;;
    --probe-reverse-opaque-indexed-triangles)
      probe_reverse_opaque_indexed_triangles=1
      shift
      ;;
    --probe-reverse-nonopaque-indexed-triangles)
      probe_reverse_nonopaque_indexed_triangles=1
      shift
      ;;
    --probe-sort-indexed-triangles-by-min-index)
      probe_sort_indexed_triangles_by_min_index=1
      shift
      ;;
    --probe-optimize-indexed-triangles-vertex-cache)
      probe_optimize_indexed_triangles_vertex_cache=1
      shift
      ;;
    --optimize-opaque-depth-index-cache)
      optimize_opaque_depth_index_cache=1
      shift
      ;;
    --optimize-opaque-depth-index-cache-min-gain-pct)
      optimize_opaque_depth_index_cache_min_gain_pct=${2:?missing value for --optimize-opaque-depth-index-cache-min-gain-pct}
      shift 2
      ;;
    --index-cache-candidate-frontier-cap)
      index_cache_candidate_frontier_cap=${2:?missing value for --index-cache-candidate-frontier-cap}
      shift 2
      ;;
    --index-cache-candidate-lazy-frontier)
      index_cache_candidate_lazy_frontier=1
      shift
      ;;
    --index-cache-candidate-bucketed-select)
      index_cache_candidate_bucketed_select=1
      shift
      ;;
    --index-cache-candidate-strict-lru)
      index_cache_candidate_strict_lru=1
      shift
      ;;
    --index-cache-candidate-upper-bound-gate)
      index_cache_candidate_upper_bound_gate=1
      shift
      ;;
    --probe-apply-index-cache-opt-candidate)
      probe_apply_index_cache_opt_candidate=1
      measure_index_reuse=1
      measure_index_cache_opt_candidate=1
      shift
      ;;
    --probe-apply-index-cache-opt-candidate-unsafe-nonopaque)
      probe_apply_index_cache_opt_candidate_unsafe_nonopaque=1
      shift
      ;;
    --probe-apply-index-cache-opt-candidate-min-gain-pct)
      probe_apply_index_cache_opt_candidate_min_gain_pct=${2:?missing value for --probe-apply-index-cache-opt-candidate-min-gain-pct}
      shift 2
      ;;
    --probe-reverse-indexed-triangles-row)
      probe_reverse_indexed_triangles_row=${2:?missing value for --probe-reverse-indexed-triangles-row}
      shift 2
      ;;
    --probe-reverse-indexed-triangles-rows)
      probe_reverse_indexed_triangles_rows=${2:?missing value for --probe-reverse-indexed-triangles-rows}
      shift 2
      ;;
    --probe-reverse-indexed-triangles-class)
      probe_reverse_indexed_triangles_class=${2:?missing value for --probe-reverse-indexed-triangles-class}
      shift 2
      ;;
    --probe-reverse-indexed-triangles-classes)
      probe_reverse_indexed_triangles_classes=${2:?missing value for --probe-reverse-indexed-triangles-classes}
      shift 2
      ;;
    --probe-reverse-indexed-triangles-stream0-span-min)
      probe_reverse_indexed_triangles_stream0_span_min=${2:?missing value for --probe-reverse-indexed-triangles-stream0-span-min}
      shift 2
      ;;
    --probe-indexed-triangle-encoder-draw-min)
      probe_indexed_triangle_encoder_draw_min=${2:?missing value for --probe-indexed-triangle-encoder-draw-min}
      shift 2
      ;;
    --probe-indexed-triangle-encoder-draw-max)
      probe_indexed_triangle_encoder_draw_max=${2:?missing value for --probe-indexed-triangle-encoder-draw-max}
      shift 2
      ;;
    --probe-indexed-triangle-encoder-draw-exclude)
      probe_indexed_triangle_encoder_draw_exclude=${2:?missing value for --probe-indexed-triangle-encoder-draw-exclude}
      shift 2
      ;;
    --probe-scissor-rect)
      probe_scissor_rect=${2:?missing value for --probe-scissor-rect}
      shift 2
      ;;
    --probe-scissor-rect-row)
      probe_scissor_rect_row=${2:?missing value for --probe-scissor-rect-row}
      shift 2
      ;;
    --probe-scissor-rect-rows)
      probe_scissor_rect_rows=${2:?missing value for --probe-scissor-rect-rows}
      shift 2
      ;;
    --probe-scissor-rect-class)
      probe_scissor_rect_class=${2:?missing value for --probe-scissor-rect-class}
      shift 2
      ;;
    --probe-scissor-rect-classes)
      probe_scissor_rect_classes=${2:?missing value for --probe-scissor-rect-classes}
      shift 2
      ;;
    --probe-force-cull-mode)
      probe_force_cull_mode=${2:?missing value for --probe-force-cull-mode}
      shift 2
      ;;
    --probe-force-cull-mode-row)
      probe_force_cull_mode_row=${2:?missing value for --probe-force-cull-mode-row}
      shift 2
      ;;
    --probe-force-cull-mode-rows)
      probe_force_cull_mode_rows=${2:?missing value for --probe-force-cull-mode-rows}
      shift 2
      ;;
    --probe-force-cull-mode-class)
      probe_force_cull_mode_class=${2:?missing value for --probe-force-cull-mode-class}
      shift 2
      ;;
    --probe-force-cull-mode-classes)
      probe_force_cull_mode_classes=${2:?missing value for --probe-force-cull-mode-classes}
      shift 2
      ;;
    --force-cull-mode)
      force_cull_mode=${2:?missing value for --force-cull-mode}
      shift 2
      ;;
    --measure-index-reuse)
      measure_index_reuse=1
      shift
      ;;
    --measure-index-cache-opt-candidate)
      measure_index_reuse=1
      measure_index_cache_opt_candidate=1
      shift
      ;;
    --dump-indexed-geometry)
      dump_indexed_geometry=1
      measure_index_reuse=1
      shift
      ;;
    --dump-indexed-geometry-cbufs)
      dump_indexed_geometry=1
      dump_indexed_geometry_cbufs=1
      measure_index_reuse=1
      shift
      ;;
    --dump-indexed-geometry-max-draws)
      dump_indexed_geometry_max_draws=${2:?missing value for --dump-indexed-geometry-max-draws}
      shift 2
      ;;
    --dump-indexed-geometry-vs)
      dump_indexed_geometry_vs=${2:?missing value for --dump-indexed-geometry-vs}
      shift 2
      ;;
    --dump-indexed-geometry-ps)
      dump_indexed_geometry_ps=${2:?missing value for --dump-indexed-geometry-ps}
      shift 2
      ;;
    --dump-indexed-geometry-texture0)
      dump_indexed_geometry_texture0=${2:?missing value for --dump-indexed-geometry-texture0}
      shift 2
      ;;
    --dump-indexed-geometry-texture0-width)
      dump_indexed_geometry_texture0_width=${2:?missing value for --dump-indexed-geometry-texture0-width}
      shift 2
      ;;
    --dump-indexed-geometry-texture0-height)
      dump_indexed_geometry_texture0_height=${2:?missing value for --dump-indexed-geometry-texture0-height}
      shift 2
      ;;
    --dump-indexed-geometry-texture0-format)
      dump_indexed_geometry_texture0_format=${2:?missing value for --dump-indexed-geometry-texture0-format}
      shift 2
      ;;
    --dump-depth-attachment-handle)
      dump_depth_attachment_handle=${2:?missing value for --dump-depth-attachment-handle}
      shift 2
      ;;
    --dump-depth-attachment-seq)
      dump_depth_attachment_seq=${2:?missing value for --dump-depth-attachment-seq}
      shift 2
      ;;
    --dump-depth-attachment-enc)
      dump_depth_attachment_enc=${2:?missing value for --dump-depth-attachment-enc}
      shift 2
      ;;
    --dump-depth-attachment-path)
      dump_depth_attachment_path=${2:?missing value for --dump-depth-attachment-path}
      shift 2
      ;;
    --dump-color-attachment-handle)
      dump_color_attachment_handle=${2:?missing value for --dump-color-attachment-handle}
      shift 2
      ;;
    --dump-color-attachment-index)
      dump_color_attachment_index=${2:?missing value for --dump-color-attachment-index}
      shift 2
      ;;
    --dump-color-attachment-after-draw)
      dump_color_attachment_after_draw=1
      shift
      ;;
    --dump-color-attachment-draw)
      dump_color_attachment_draw=${2:?missing value for --dump-color-attachment-draw}
      shift 2
      ;;
    --dump-color-attachment-draws)
      dump_color_attachment_draws=${2:?missing value for --dump-color-attachment-draws}
      shift 2
      ;;
    --dump-color-attachment-command-index)
      dump_color_attachment_command_index=${2:?missing value for --dump-color-attachment-command-index}
      shift 2
      ;;
    --dump-color-attachment-command-index-min)
      dump_color_attachment_command_index_min=${2:?missing value for --dump-color-attachment-command-index-min}
      shift 2
      ;;
    --dump-color-attachment-command-index-max)
      dump_color_attachment_command_index_max=${2:?missing value for --dump-color-attachment-command-index-max}
      shift 2
      ;;
    --dump-color-attachment-texture0)
      dump_color_attachment_texture0=${2:?missing value for --dump-color-attachment-texture0}
      shift 2
      ;;
    --dump-color-attachment-texture0s)
      dump_color_attachment_texture0s=${2:?missing value for --dump-color-attachment-texture0s}
      shift 2
      ;;
    --dump-color-attachment-seq)
      dump_color_attachment_seq=${2:?missing value for --dump-color-attachment-seq}
      shift 2
      ;;
    --dump-color-attachment-enc)
      dump_color_attachment_enc=${2:?missing value for --dump-color-attachment-enc}
      shift 2
      ;;
    --dump-color-attachment-path)
      dump_color_attachment_path=${2:?missing value for --dump-color-attachment-path}
      shift 2
      ;;
    --dump-color-attachment-dir)
      dump_color_attachment_dir=${2:?missing value for --dump-color-attachment-dir}
      shift 2
      ;;
    --dump-color-attachment-roi-summary-path)
      dump_color_attachment_roi_summary_path=${2:?missing value for --dump-color-attachment-roi-summary-path}
      shift 2
      ;;
    --dump-color-attachment-roi)
      if [[ -n "$dump_color_attachment_rois" ]]; then
        dump_color_attachment_rois+=";"
      fi
      dump_color_attachment_rois+="${2:?missing value for --dump-color-attachment-roi}"
      shift 2
      ;;
    --dump-color-attachment-bright-threshold)
      dump_color_attachment_bright_threshold=${2:?missing value for --dump-color-attachment-bright-threshold}
      shift 2
      ;;
    --dump-color-attachment-white-threshold)
      dump_color_attachment_white_threshold=${2:?missing value for --dump-color-attachment-white-threshold}
      shift 2
      ;;
    --dump-color-attachment-warm-red-threshold)
      dump_color_attachment_warm_red_threshold=${2:?missing value for --dump-color-attachment-warm-red-threshold}
      shift 2
      ;;
    --dump-color-attachment-warm-green-threshold)
      dump_color_attachment_warm_green_threshold=${2:?missing value for --dump-color-attachment-warm-green-threshold}
      shift 2
      ;;
    --dump-color-attachment-warm-blue-margin)
      dump_color_attachment_warm_blue_margin=${2:?missing value for --dump-color-attachment-warm-blue-margin}
      shift 2
      ;;
    --dump-draw-texture-handles)
      dump_draw_texture_handles=${2:?missing value for --dump-draw-texture-handles}
      shift 2
      ;;
    --dump-draw-texture0-any)
      dump_draw_texture0_any=1
      shift
      ;;
    --dump-draw-texture0-width)
      dump_draw_texture0_width=${2:?missing value for --dump-draw-texture0-width}
      shift 2
      ;;
    --dump-draw-texture0-height)
      dump_draw_texture0_height=${2:?missing value for --dump-draw-texture0-height}
      shift 2
      ;;
    --dump-draw-texture0-format)
      dump_draw_texture0_format=${2:?missing value for --dump-draw-texture0-format}
      shift 2
      ;;
    --dump-draw-texture-seq)
      dump_draw_texture_seq=${2:?missing value for --dump-draw-texture-seq}
      shift 2
      ;;
    --dump-draw-texture-enc)
      dump_draw_texture_enc=${2:?missing value for --dump-draw-texture-enc}
      shift 2
      ;;
    --dump-draw-texture-dir)
      dump_draw_texture_dir=${2:?missing value for --dump-draw-texture-dir}
      shift 2
      ;;
    --aggressive-color-dontcare)
      aggressive_color_dontcare=1
      shift
      ;;
    --aggressive-depth-dontcare)
      aggressive_depth_dontcare=1
      shift
      ;;
    --disable-cull)
      disable_cull=1
      shift
      ;;
    --disable-scissor)
      disable_scissor=1
      shift
      ;;
    --disable-alpha-test)
      disable_alpha_test=1
      shift
      ;;
    --disable-fog)
      disable_fog=1
      shift
      ;;
    --force-texture-white)
      force_texture_white=1
      shift
      ;;
    --probe-force-texture-white)
      probe_force_texture_white=1
      shift
      ;;
    --probe-force-texture-white-row)
      probe_force_texture_white_row=${2:?missing value for --probe-force-texture-white-row}
      shift 2
      ;;
    --probe-force-texture-white-rows)
      probe_force_texture_white_rows=${2:?missing value for --probe-force-texture-white-rows}
      shift 2
      ;;
    --probe-force-texture-white-class)
      probe_force_texture_white_class=${2:?missing value for --probe-force-texture-white-class}
      shift 2
      ;;
    --probe-force-texture-white-classes)
      probe_force_texture_white_classes=${2:?missing value for --probe-force-texture-white-classes}
      shift 2
      ;;
    --probe-force-texture-white-texture0)
      probe_force_texture_white=1
      probe_force_texture_white_texture0=${2:?missing value for --probe-force-texture-white-texture0}
      shift 2
      ;;
    --probe-force-texture-white-texture0-width)
      probe_force_texture_white=1
      probe_force_texture_white_texture0_width=${2:?missing value for --probe-force-texture-white-texture0-width}
      shift 2
      ;;
    --probe-force-texture-white-texture0-height)
      probe_force_texture_white=1
      probe_force_texture_white_texture0_height=${2:?missing value for --probe-force-texture-white-texture0-height}
      shift 2
      ;;
    --probe-force-texture-white-texture0-format)
      probe_force_texture_white=1
      probe_force_texture_white_texture0_format=${2:?missing value for --probe-force-texture-white-texture0-format}
      shift 2
      ;;
    --probe-force-texture-white-draw-ordinal)
      probe_force_texture_white=1
      probe_force_texture_white_draw_ordinal=${2:?missing value for --probe-force-texture-white-draw-ordinal}
      shift 2
      ;;
    --probe-force-texture-white-draw-ordinals)
      probe_force_texture_white=1
      probe_force_texture_white_draw_ordinals=${2:?missing value for --probe-force-texture-white-draw-ordinals}
      shift 2
      ;;
    --probe-force-texture-white-draw-ordinal-min)
      probe_force_texture_white=1
      probe_force_texture_white_draw_ordinal_min=${2:?missing value for --probe-force-texture-white-draw-ordinal-min}
      shift 2
      ;;
    --probe-force-texture-white-draw-ordinal-max)
      probe_force_texture_white=1
      probe_force_texture_white_draw_ordinal_max=${2:?missing value for --probe-force-texture-white-draw-ordinal-max}
      shift 2
      ;;
    --probe-force-texture-white-command-index)
      probe_force_texture_white=1
      probe_force_texture_white_command_index=${2:?missing value for --probe-force-texture-white-command-index}
      shift 2
      ;;
    --probe-force-texture-white-command-indexes)
      probe_force_texture_white=1
      probe_force_texture_white_command_indexes=${2:?missing value for --probe-force-texture-white-command-indexes}
      shift 2
      ;;
    --probe-force-texture-white-command-index-min)
      probe_force_texture_white=1
      probe_force_texture_white_command_index_min=${2:?missing value for --probe-force-texture-white-command-index-min}
      shift 2
      ;;
    --probe-force-texture-white-command-index-max)
      probe_force_texture_white=1
      probe_force_texture_white_command_index_max=${2:?missing value for --probe-force-texture-white-command-index-max}
      shift 2
      ;;
    --probe-force-texture-white-command-draw-index)
      probe_force_texture_white=1
      probe_force_texture_white_command_draw_index=${2:?missing value for --probe-force-texture-white-command-draw-index}
      shift 2
      ;;
    --probe-force-texture-white-command-draw-indexes)
      probe_force_texture_white=1
      probe_force_texture_white_command_draw_indexes=${2:?missing value for --probe-force-texture-white-command-draw-indexes}
      shift 2
      ;;
    --probe-force-texture-white-command-draw-index-min)
      probe_force_texture_white=1
      probe_force_texture_white_command_draw_index_min=${2:?missing value for --probe-force-texture-white-command-draw-index-min}
      shift 2
      ;;
    --probe-force-texture-white-command-draw-index-max)
      probe_force_texture_white=1
      probe_force_texture_white_command_draw_index_max=${2:?missing value for --probe-force-texture-white-command-draw-index-max}
      shift 2
      ;;
    --probe-disable-alpha-blend)
      probe_disable_alpha_blend=1
      shift
      ;;
    --probe-disable-alpha-blend-row)
      probe_disable_alpha_blend_row=${2:?missing value for --probe-disable-alpha-blend-row}
      shift 2
      ;;
    --probe-disable-alpha-blend-rows)
      probe_disable_alpha_blend_rows=${2:?missing value for --probe-disable-alpha-blend-rows}
      shift 2
      ;;
    --probe-disable-alpha-blend-class)
      probe_disable_alpha_blend_class=${2:?missing value for --probe-disable-alpha-blend-class}
      shift 2
      ;;
    --probe-disable-alpha-blend-classes)
      probe_disable_alpha_blend_classes=${2:?missing value for --probe-disable-alpha-blend-classes}
      shift 2
      ;;
    --probe-disable-alpha-blend-texture0)
      probe_disable_alpha_blend=1
      probe_disable_alpha_blend_texture0=${2:?missing value for --probe-disable-alpha-blend-texture0}
      shift 2
      ;;
    --probe-disable-alpha-blend-texture0-width)
      probe_disable_alpha_blend=1
      probe_disable_alpha_blend_texture0_width=${2:?missing value for --probe-disable-alpha-blend-texture0-width}
      shift 2
      ;;
    --probe-disable-alpha-blend-texture0-height)
      probe_disable_alpha_blend=1
      probe_disable_alpha_blend_texture0_height=${2:?missing value for --probe-disable-alpha-blend-texture0-height}
      shift 2
      ;;
    --probe-disable-alpha-blend-texture0-format)
      probe_disable_alpha_blend=1
      probe_disable_alpha_blend_texture0_format=${2:?missing value for --probe-disable-alpha-blend-texture0-format}
      shift 2
      ;;
    --probe-disable-depth-write)
      probe_disable_depth_write=1
      shift
      ;;
    --probe-disable-depth-write-row)
      probe_disable_depth_write=1
      probe_disable_depth_write_row=${2:?missing value for --probe-disable-depth-write-row}
      shift 2
      ;;
    --probe-disable-depth-write-rows)
      probe_disable_depth_write=1
      probe_disable_depth_write_rows=${2:?missing value for --probe-disable-depth-write-rows}
      shift 2
      ;;
    --probe-disable-depth-write-class)
      probe_disable_depth_write=1
      probe_disable_depth_write_class=${2:?missing value for --probe-disable-depth-write-class}
      shift 2
      ;;
    --probe-disable-depth-write-classes)
      probe_disable_depth_write=1
      probe_disable_depth_write_classes=${2:?missing value for --probe-disable-depth-write-classes}
      shift 2
      ;;
    --probe-depth-func-always)
      probe_depth_func_always=1
      shift
      ;;
    --probe-depth-func-always-row)
      probe_depth_func_always=1
      probe_depth_func_always_row=${2:?missing value for --probe-depth-func-always-row}
      shift 2
      ;;
    --probe-depth-func-always-rows)
      probe_depth_func_always=1
      probe_depth_func_always_rows=${2:?missing value for --probe-depth-func-always-rows}
      shift 2
      ;;
    --probe-depth-func-always-class)
      probe_depth_func_always=1
      probe_depth_func_always_class=${2:?missing value for --probe-depth-func-always-class}
      shift 2
      ;;
    --probe-depth-func-always-classes)
      probe_depth_func_always=1
      probe_depth_func_always_classes=${2:?missing value for --probe-depth-func-always-classes}
      shift 2
      ;;
    --probe-depth-func-always-texture0)
      probe_depth_func_always=1
      probe_depth_func_always_texture0=${2:?missing value for --probe-depth-func-always-texture0}
      shift 2
      ;;
    --probe-depth-func-always-texture0-width)
      probe_depth_func_always=1
      probe_depth_func_always_texture0_width=${2:?missing value for --probe-depth-func-always-texture0-width}
      shift 2
      ;;
    --probe-depth-func-always-texture0-height)
      probe_depth_func_always=1
      probe_depth_func_always_texture0_height=${2:?missing value for --probe-depth-func-always-texture0-height}
      shift 2
      ;;
    --probe-depth-func-always-texture0-format)
      probe_depth_func_always=1
      probe_depth_func_always_texture0_format=${2:?missing value for --probe-depth-func-always-texture0-format}
      shift 2
      ;;
    --probe-fragmentless-depth-only)
      probe_fragmentless_depth_only=1
      shift
      ;;
    --probe-fragmentless-depth-only-row)
      probe_fragmentless_depth_only=1
      probe_fragmentless_depth_only_row=${2:?missing value for --probe-fragmentless-depth-only-row}
      shift 2
      ;;
    --probe-fragmentless-depth-only-rows)
      probe_fragmentless_depth_only=1
      probe_fragmentless_depth_only_rows=${2:?missing value for --probe-fragmentless-depth-only-rows}
      shift 2
      ;;
    --probe-fragmentless-depth-only-class)
      probe_fragmentless_depth_only=1
      probe_fragmentless_depth_only_class=${2:?missing value for --probe-fragmentless-depth-only-class}
      shift 2
      ;;
    --probe-fragmentless-depth-only-classes)
      probe_fragmentless_depth_only=1
      probe_fragmentless_depth_only_classes=${2:?missing value for --probe-fragmentless-depth-only-classes}
      shift 2
      ;;
    --probe-fragmentless-depth-only-keep-vsout)
      probe_fragmentless_depth_only=1
      probe_fragmentless_depth_only_keep_vsout=1
      shift
      ;;
    --force-visible)
      force_visible=1
      shift
      ;;
    --effect-draw-trace)
      effect_draw_trace=1
      shift
      ;;
    --effect-draw-trace-seq)
      effect_draw_trace=1
      effect_draw_trace_seq=${2:?missing value for --effect-draw-trace-seq}
      shift 2
      ;;
    --effect-draw-trace-seq-min)
      effect_draw_trace=1
      effect_draw_trace_seq_min=${2:?missing value for --effect-draw-trace-seq-min}
      shift 2
      ;;
    --effect-draw-trace-seq-max)
      effect_draw_trace=1
      effect_draw_trace_seq_max=${2:?missing value for --effect-draw-trace-seq-max}
      shift 2
      ;;
    --effect-draw-trace-enc)
      effect_draw_trace=1
      effect_draw_trace_enc=${2:?missing value for --effect-draw-trace-enc}
      shift 2
      ;;
    --effect-draw-trace-texture0)
      effect_draw_trace=1
      effect_draw_trace_texture0=${2:?missing value for --effect-draw-trace-texture0}
      shift 2
      ;;
    --effect-draw-trace-texture0-width)
      effect_draw_trace=1
      effect_draw_trace_texture0_width=${2:?missing value for --effect-draw-trace-texture0-width}
      shift 2
      ;;
    --effect-draw-trace-texture0-height)
      effect_draw_trace=1
      effect_draw_trace_texture0_height=${2:?missing value for --effect-draw-trace-texture0-height}
      shift 2
      ;;
    --effect-draw-trace-texture0-format)
      effect_draw_trace=1
      effect_draw_trace_texture0_format=${2:?missing value for --effect-draw-trace-texture0-format}
      shift 2
      ;;
    --effect-draw-trace-primitive-type)
      effect_draw_trace=1
      effect_draw_trace_primitive_type=${2:?missing value for --effect-draw-trace-primitive-type}
      shift 2
      ;;
    --effect-draw-trace-point-sprite)
      effect_draw_trace=1
      effect_draw_trace_point_sprite=1
      shift
      ;;
    --effect-draw-trace-include-non-alpha)
      effect_draw_trace=1
      effect_draw_trace_include_non_alpha=1
      shift
      ;;
    --effect-draw-trace-include-untextured)
      effect_draw_trace=1
      effect_draw_trace_include_untextured=1
      shift
      ;;
    --effect-draw-trace-geometry)
      effect_draw_trace=1
      effect_draw_trace_geometry=1
      shift
      ;;
    --effect-draw-trace-geometry-max-refs)
      effect_draw_trace=1
      effect_draw_trace_geometry=1
      effect_draw_trace_geometry_max_refs=${2:?missing value for --effect-draw-trace-geometry-max-refs}
      shift 2
      ;;
    --visibility-scout)
      visibility_scout=1
      shift
      ;;
    --visibility-scout-row)
      visibility_scout=1
      visibility_scout_row=${2:?missing value for --visibility-scout-row}
      shift 2
      ;;
    --visibility-scout-rows)
      visibility_scout=1
      visibility_scout_rows=${2:?missing value for --visibility-scout-rows}
      shift 2
      ;;
    --visibility-scout-path)
      visibility_scout=1
      visibility_scout_path=${2:?missing value for --visibility-scout-path}
      shift 2
      ;;
    --visibility-scout-draw-indices)
      visibility_scout=1
      visibility_scout_draw_indices=${2:?missing value for --visibility-scout-draw-indices}
      shift 2
      ;;
    --visibility-scout-summary-output)
      visibility_scout=1
      visibility_scout_summary_output=${2:?missing value for --visibility-scout-summary-output}
      shift 2
      ;;
    --visibility-scout-summary-csv-output)
      visibility_scout=1
      visibility_scout_summary_csv_output=${2:?missing value for --visibility-scout-summary-csv-output}
      shift 2
      ;;
    --visibility-scout-summary-limit)
      visibility_scout=1
      visibility_scout_summary_limit=${2:?missing value for --visibility-scout-summary-limit}
      shift 2
      ;;
    --compare-baseline-output)
      compare_baseline_output=${2:?missing value for --compare-baseline-output}
      shift 2
      ;;
    --baseline-joined)
      compare_baseline_joined=${2:?missing value for --baseline-joined}
      shift 2
      ;;
    --semantic-image-policy)
      semantic_image_policy=${2:?missing value for --semantic-image-policy}
      shift 2
      ;;
    --semantic-image-before)
      semantic_image_before=${2:?missing value for --semantic-image-before}
      shift 2
      ;;
    --semantic-image-after)
      semantic_image_after=${2:?missing value for --semantic-image-after}
      shift 2
      ;;
    --semantic-image-output)
      semantic_image_output=${2:?missing value for --semantic-image-output}
      shift 2
      ;;
    --semantic-image-summary-output)
      semantic_image_summary_output=${2:?missing value for --semantic-image-summary-output}
      shift 2
      ;;
    --semantic-image-diff-output)
      semantic_image_diff_output=${2:?missing value for --semantic-image-diff-output}
      shift 2
      ;;
    --semantic-image-min-active-pct)
      semantic_image_min_active_pct=${2:?missing value for --semantic-image-min-active-pct}
      shift 2
      ;;
    --require-color-dontcare-increase)
      require_color_dontcare_increase=1
      shift
      ;;
    --require-depth-dontcare-increase)
      require_depth_dontcare_increase=1
      shift
      ;;
    --require-tile-preservation-decrease)
      require_tile_preservation_decrease=1
      shift
      ;;
    --require-tile-preservation-not-increase)
      require_tile_preservation_not_increase=1
      shift
      ;;
    --require-command-buffers-per-present-not-increase)
      require_command_buffers_per_present_not_increase=1
      shift
      ;;
    --require-render-passes-per-present-not-increase)
      require_render_passes_per_present_not_increase=1
      shift
      ;;
    --require-render-pass-carry-promotion-gates)
      require_render_pass_carry_promotion_gates=1
      shift
      ;;
    --require-encoder-final-end-reason-not-increase)
      require_encoder_final_end_reason_not_increase=1
      shift
      ;;
    --require-encoder-final-same-key-reopen-not-increase)
      require_encoder_final_same_key_reopen_not_increase=1
      shift
      ;;
    --require-encoder-color-load-not-increase)
      require_encoder_color_load_not_increase=1
      shift
      ;;
    --require-encoder-depth-load-not-increase)
      require_encoder_depth_load_not_increase=1
      shift
      ;;
    --require-draw-run-records-increase)
      require_draw_run_records_increase=1
      shift
      ;;
    --require-draw-run-records-per-submit-increase)
      require_draw_run_records_per_submit_increase=1
      shift
      ;;
    --require-binding-overrides-present)
      require_binding_overrides_present=1
      shift
      ;;
    --require-const-upload-passthrough-present)
      require_const_upload_passthrough_present=1
      shift
      ;;
    --require-draw-submission-batch-present)
      require_draw_submission_batch_present=1
      shift
      ;;
    --require-const-upload-break-bytes-decrease)
      require_const_upload_break_bytes_decrease=1
      shift
      ;;
    --require-encode-draw-cpu-decrease)
      require_encode_draw_cpu_decrease=1
      shift
      ;;
    --require-completion-present-wait-decrease)
      require_completion_present_wait_decrease=1
      shift
      ;;
    --require-completion-wait-with-enqueue-increase)
      require_completion_wait_with_enqueue_increase=1
      shift
      ;;
    --require-completion-wait-without-enqueue-decrease)
      require_completion_wait_without_enqueue_decrease=1
      shift
      ;;
    --require-completion-present-wait-with-enqueue-increase)
      require_completion_present_wait_with_enqueue_increase=1
      shift
      ;;
    --require-completion-present-wait-without-enqueue-decrease)
      require_completion_present_wait_without_enqueue_decrease=1
      shift
      ;;
    --require-encode-ready-depth-gt1-increase)
      require_encode_ready_depth_gt1_increase=1
      shift
      ;;
    --require-commit-chunk-replay-cpu-per-present-decrease)
      require_commit_chunk_replay_cpu_per_present_decrease=1
      shift
      ;;
    --require-queue-draw-submission-cpu-per-present-decrease)
      require_queue_draw_submission_cpu_per_present_decrease=1
      shift
      ;;
    --require-snapshot-cpu-per-present-decrease)
      require_snapshot_cpu_per_present_decrease=1
      shift
      ;;
    --require-snapshot-cache-lookup-cpu-per-present-decrease)
      require_snapshot_cache_lookup_cpu_per_present_decrease=1
      shift
      ;;
    --require-snapshot-cache-uniform-build-cpu-per-present-decrease)
      require_snapshot_cache_uniform_build_cpu_per_present_decrease=1
      shift
      ;;
    --require-snapshot-cache-uniform-hash-cpu-per-present-decrease)
      require_snapshot_cache_uniform_hash_cpu_per_present_decrease=1
      shift
      ;;
    --require-batch-miss-uniform-build-cpu-per-present-decrease)
      require_batch_miss_uniform_build_cpu_per_present_decrease=1
      shift
      ;;
    --require-batch-miss-uniform-hash-cpu-per-present-decrease)
      require_batch_miss_uniform_hash_cpu_per_present_decrease=1
      shift
      ;;
    --require-batch-miss-vs-const-hash-cpu-per-present-decrease)
      require_batch_miss_vs_const_hash_cpu_per_present_decrease=1
      shift
      ;;
    --require-batch-miss-ps-const-hash-cpu-per-present-decrease)
      require_batch_miss_ps_const_hash_cpu_per_present_decrease=1
      shift
      ;;
    --require-batch-miss-nonconst-hash-cpu-per-present-decrease)
      require_batch_miss_nonconst_hash_cpu_per_present_decrease=1
      shift
      ;;
    --require-snapshot-uniform-copy-cpu-per-present-decrease)
      require_snapshot_uniform_copy_cpu_per_present_decrease=1
      shift
      ;;
    --require-submit-draw-run-batch-append-uniform-cpu-per-present-decrease)
      require_submit_draw_run_batch_append_uniform_cpu_per_present_decrease=1
      shift
      ;;
    --require-draw-uniform-payload-lookup-cpu-per-present-decrease)
      require_draw_uniform_payload_lookup_cpu_per_present_decrease=1
      shift
      ;;
    --require-draw-uniform-payload-append-copy-cpu-per-present-decrease)
      require_draw_uniform_payload_append_copy_cpu_per_present_decrease=1
      shift
      ;;
    --require-argbuf-setup-cpu-per-present-decrease)
      require_argbuf_setup_cpu_per_present_decrease=1
      shift
      ;;
    --require-argbuf-open-cpu-per-present-decrease)
      require_argbuf_open_cpu_per_present_decrease=1
      shift
      ;;
    --require-argbuf-cbuf-update-cpu-per-present-decrease)
      require_argbuf_cbuf_update_cpu_per_present_decrease=1
      shift
      ;;
    --require-argbuf-cbuf-update-vs-cpu-per-present-decrease)
      require_argbuf_cbuf_update_vs_cpu_per_present_decrease=1
      shift
      ;;
    --require-uniform-compact-saved-bytes-present)
      require_uniform_compact_saved_bytes_present=1
      shift
      ;;
    --require-current-uniform-compact-saved-bytes-present)
      require_current_uniform_compact_saved_bytes_present=1
      shift
      ;;
    --require-snapshot-state-elided-present)
      require_snapshot_state_elided_present=1
      shift
      ;;
    --require-discarded-state-not-increase)
      require_discarded_state_not_increase=1
      shift
      ;;
    --require-submission-carrier-bytes-per-record-decrease)
      require_submission_carrier_bytes_per_record_decrease=1
      shift
      ;;
    --require-submission-carrier-uniform-storage-per-record-decrease)
      require_submission_carrier_uniform_storage_per_record_decrease=1
      shift
      ;;
    --require-encode-chunk-cpu-per-present-decrease)
      require_encode_chunk_cpu_per_present_decrease=1
      shift
      ;;
    --require-no-enqueue-commit-entry-to-publish-decrease)
      require_no_enqueue_commit_entry_to_publish_decrease=1
      shift
      ;;
    --require-no-enqueue-publish-to-encode-dequeue-decrease)
      require_no_enqueue_publish_to_encode_dequeue_decrease=1
      shift
      ;;
    --require-no-enqueue-encode-dequeue-to-commit-decrease)
      require_no_enqueue_encode_dequeue_to_commit_decrease=1
      shift
      ;;
    --require-no-enqueue-wait-to-next-enqueue-decrease)
      require_no_enqueue_wait_to_next_enqueue_decrease=1
      shift
      ;;
    --require-no-enqueue-before-publish-closure-decrease)
      require_no_enqueue_before_publish_closure_decrease=1
      shift
      ;;
    --require-no-enqueue-before-publish-inter-replay-gap-decrease)
      require_no_enqueue_before_publish_inter_replay_gap_decrease=1
      shift
      ;;
    --require-pe-focused-between-call-gap-residual-decrease)
      require_pe_focused_between_call_gap_residual_decrease=1
      shift
      ;;
    --max-gpu-command-buffer-regression-ms)
      max_gpu_command_buffer_regression_ms=${2:?missing value for --max-gpu-command-buffer-regression-ms}
      shift 2
      ;;
    --max-const-upload-break-count-ratio)
      max_const_upload_break_count_ratio=${2:?missing value for --max-const-upload-break-count-ratio}
      shift 2
      ;;
    --require-result-json)
      require_result_json=1
      shift
      ;;
    --allow-partial-stable-frame-proof)
      allow_partial_stable_frame_proof=1
      shift
      ;;
    --require-top-gpu-decrease)
      require_top_gpu_decrease=1
      shift
      ;;
    --require-top-buffer-write-decrease)
      require_top_buffer_write_decrease=1
      shift
      ;;
    --require-top-vs-buffer-write-decrease)
      require_top_vs_buffer_write_decrease=1
      shift
      ;;
    --require-top-unexplained-buffer-write-decrease)
      require_top_unexplained_buffer_write_decrease=1
      shift
      ;;
    --require-stream-handle-churn-decrease)
      require_stream_handle_churn_decrease=1
      shift
      ;;
    --require-ib-handle-churn-decrease)
      require_ib_handle_churn_decrease=1
      shift
      ;;
    --require-argbuf-cbuf-decrease)
      require_argbuf_cbuf_decrease=1
      shift
      ;;
    --require-transient-decrease)
      require_transient_decrease=1
      shift
      ;;
    --require-top-gpu-share-increase)
      require_top_gpu_share_increase=1
      shift
      ;;
    --require-top-row-key-match)
      require_top_row_key_match=1
      shift
      ;;
    --require-stable-frame-proof)
      require_stable_frame_proof=1
      shift
      ;;
    --require-tvb-mechanism-proof)
      require_tvb_mechanism_proof=1
      shift
      ;;
    --require-cache-opt-apply-proof)
      require_cache_opt_apply_proof=1
      shift
      ;;
    --require-opaque-depth-index-cache-proof)
      require_opaque_depth_index_cache_proof=1
      shift
      ;;
    --require-screen-blend-cache-proof)
      require_screen_blend_cache_proof=1
      shift
      ;;
    --require-semantic-image-proof)
      require_semantic_image_proof=1
      shift
      ;;
    --require-top-pso-attribution)
      require_top_pso_attribution=1
      shift
      ;;
    --require-xcode-counter-coverage)
      require_xcode_counter_coverage=1
      shift
      ;;
    --require-dxmt-join-coverage)
      require_dxmt_join_coverage=1
      shift
      ;;
    --require-shader-dump-matches)
      require_shader_dump_matches=1
      shift
      ;;
    --min-top-pso-samples-per-draw)
      min_top_pso_samples_per_draw=${2:?missing value for --min-top-pso-samples-per-draw}
      shift 2
      ;;
    --min-top-dxmt-joined-fraction)
      min_top_dxmt_joined_fraction=${2:?missing value for --min-top-dxmt-joined-fraction}
      shift 2
      ;;
    --max-top-gpu-regression-ms)
      max_top_gpu_regression_ms=${2:?missing value for --max-top-gpu-regression-ms}
      shift 2
      ;;
    --max-top-buffer-write-regression-mib)
      max_top_buffer_write_regression_mib=${2:?missing value for --max-top-buffer-write-regression-mib}
      shift 2
      ;;
    --target-row-key)
      target_row_keys+=("${2:?missing value for --target-row-key}")
      shift 2
      ;;
    --require-target-index-cache-miss32-decrease)
      require_target_index_cache_miss32_decrease=1
      shift
      ;;
    --require-target-index-cache-opt-miss32-decrease)
      require_target_index_cache_opt_miss32_decrease=1
      shift
      ;;
    --require-target-reordered-index-cache-hits)
      require_target_reordered_index_cache_hits=1
      shift
      ;;
    --require-target-vs-buffer-write-decrease)
      require_target_vs_buffer_write_decrease=1
      shift
      ;;
    --require-target-vs-invocations-decrease)
      require_target_vs_invocations_decrease=1
      shift
      ;;
    --max-non-target-gpu-regression-ms)
      max_non_target_gpu_regression_ms=${2:?missing value for --max-non-target-gpu-regression-ms}
      shift 2
      ;;
    --max-non-target-vs-buffer-write-regression-mib)
      max_non_target_vs_buffer_write_regression_mib=${2:?missing value for --max-non-target-vs-buffer-write-regression-mib}
      shift 2
      ;;
    --max-non-target-vs-invocations-regression-ratio)
      max_non_target_vs_invocations_regression_ratio=${2:?missing value for --max-non-target-vs-invocations-regression-ratio}
      shift 2
      ;;
    --max-non-target-draw-call-delta-ratio)
      max_non_target_draw_call_delta_ratio=${2:?missing value for --max-non-target-draw-call-delta-ratio}
      shift 2
      ;;
    --max-non-target-vertex-count-delta-ratio)
      max_non_target_vertex_count_delta_ratio=${2:?missing value for --max-non-target-vertex-count-delta-ratio}
      shift 2
      ;;
    --max-non-target-triangle-delta-ratio)
      max_non_target_triangle_delta_ratio=${2:?missing value for --max-non-target-triangle-delta-ratio}
      shift 2
      ;;
    --max-top-unexplained-buffer-write-ratio)
      max_top_unexplained_buffer_write_ratio=${2:?missing value for --max-top-unexplained-buffer-write-ratio}
      shift 2
      ;;
    --max-top-draw-call-delta-ratio)
      max_top_draw_call_delta_ratio=${2:?missing value for --max-top-draw-call-delta-ratio}
      shift 2
      ;;
    --max-top-vertex-count-delta-ratio)
      max_top_vertex_count_delta_ratio=${2:?missing value for --max-top-vertex-count-delta-ratio}
      shift 2
      ;;
    --max-top-triangle-delta-ratio)
      max_top_triangle_delta_ratio=${2:?missing value for --max-top-triangle-delta-ratio}
      shift 2
      ;;
    --top)
      top_n=${2:?missing value for --top}
      shift 2
      ;;
    --hot-gpu-share)
      hot_gpu_share=${2:?missing value for --hot-gpu-share}
      shift 2
      ;;
    --min-free-mb)
      min_free_mb=${2:?missing value for --min-free-mb}
      shift 2
      ;;
    --dry-run)
      dry_run=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if [[ ! "$frame" =~ ^[0-9]+$ ]] || (( frame == 0 )); then
  echo "--frame must be a positive integer" >&2
  exit 2
fi

if (( capture_gputrace || metal_capture_destination_explicit )) &&
   ! capture_destination_is_valid "$metal_capture_destination"; then
  echo "--metal-capture-destination must be one of gpuTraceDocument, gputrace, file, developerTools, xcode" >&2
  exit 2
fi

if (( require_xcode_attach_preflight )) &&
   ! capture_destination_is_developer_tools "$metal_capture_destination"; then
  echo "--require-xcode-attach-preflight requires --xcode-developer-tools-capture or --metal-capture-destination developerTools" >&2
  exit 2
fi

if [[ -n "$open_cb_semantic_boundary_release_mode" &&
      "$open_cb_semantic_boundary_release_mode" != "completion_wait" &&
      "$open_cb_semantic_boundary_release_mode" != "deterministic" ]]; then
  echo "--open-cb-semantic-boundary-release-mode must be completion_wait or deterministic" >&2
  exit 2
fi

if (( with_wine_capture_layer )) && (( ! capture_gputrace )); then
  echo "--with-wine-capture-layer requires a file .gputrace capture; remove --no-gputrace" >&2
  exit 2
fi

if (( with_wine_capture_layer )) &&
   ! capture_destination_is_file "$metal_capture_destination"; then
  echo "--with-wine-capture-layer is only for file .gputrace capture; do not combine it with developerTools/xcode capture" >&2
  exit 2
fi

if (( xcode_attach_preflight_only )); then
  run_xcode_attach_preflight
  exit $?
fi

if [[ -z "$timeout" ]]; then
  if (( capture_gputrace )); then
    timeout=420
  else
    timeout=120
  fi
fi

if [[ ! "$timeout" =~ ^[0-9]+([.][0-9]+)?$ ||
      "$timeout" =~ ^0+([.]0+)?$ ]]; then
  echo "--timeout must be positive numeric seconds" >&2
  exit 2
fi

if [[ ! "$timeout_slack" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
  echo "DXMT_3DMARK05_PROBE_TIMEOUT_SLACK must be non-negative numeric seconds" >&2
  exit 2
fi

if [[ ! "$wait_unlocked_sec" =~ ^[0-9]+$ ]]; then
  echo "--wait-unlocked-sec must be non-negative integer seconds" >&2
  exit 2
fi

if [[ ! "$wait_unlocked_interval_sec" =~ ^[1-9][0-9]*$ ]]; then
  echo "--wait-unlocked-interval-sec must be a positive integer" >&2
  exit 2
fi

if [[ "$keep_frontmost" != "0" && -n "$keep_frontmost" ]]; then
  keep_frontmost=1
else
  keep_frontmost=0
fi

if [[ ! "$keep_frontmost_interval_sec" =~ ^[0-9]+([.][0-9]+)?$ ||
      "$keep_frontmost_interval_sec" =~ ^0+([.]0+)?$ ]]; then
  echo "--keep-frontmost-interval-sec must be positive numeric seconds" >&2
  exit 2
fi

keep_frontmost_process_regex='^[A-Za-z0-9_. -]+$'
if [[ -z "$keep_frontmost_process" ||
      ! "$keep_frontmost_process" =~ $keep_frontmost_process_regex ]]; then
  echo "--keep-frontmost-process must be a non-empty process name without shell or AppleScript metacharacters" >&2
  exit 2
fi

if [[ -n "$capture_delay_sec" && ! "$capture_delay_sec" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
  echo "--capture-delay-sec must be non-negative numeric seconds" >&2
  exit 2
fi
effective_capture_delay_sec=${capture_delay_sec:-$catalogue_capture_delay_sec}
watchdog_base_sec=$(python3 - "$timeout" "$effective_capture_delay_sec" <<'PY'
import sys

timeout = float(sys.argv[1])
capture_delay = float(sys.argv[2])
print(f"{timeout + capture_delay:g}")
PY
)

if [[ -n "$capture_frames" && ! "$capture_frames" =~ ^[0-9]+([,;[:space:]][0-9]+)*$ ]]; then
  echo "--capture-frames must be a comma/semicolon/space separated positive integer list" >&2
  exit 2
fi

if [[ -n "$capture_range" && ! "$capture_range" =~ ^[0-9]+:[0-9]+(:[0-9]+)?$ ]]; then
  echo "--capture-range must be START:END[:STEP] with non-negative integers" >&2
  exit 2
fi

if [[ -n "$dump_framegraph_dag" && ! "$dump_framegraph_dag" =~ ^[0-9]+$ ]]; then
  echo "DXMT_3DMARK05_DUMP_FRAMEGRAPH_DAG must be a non-negative integer flag" >&2
  exit 2
fi

if [[ -n "$framegraph_dag_draws" && ! "$framegraph_dag_draws" =~ ^[0-9]+$ ]]; then
  echo "DXMT_3DMARK05_DUMP_FRAMEGRAPH_DAG_DRAWS must be a non-negative integer flag" >&2
  exit 2
fi

if (( dump_framegraph_dag )); then
  if [[ -z "$framegraph_dag_frame" ]]; then
    framegraph_dag_frame=$frame
  fi
  if [[ -z "$framegraph_dag_frame_radius" ]]; then
    framegraph_dag_frame_radius=0
  fi
  if [[ ! "$framegraph_dag_frame" =~ ^[0-9]+$ ]] || (( framegraph_dag_frame == 0 )); then
    echo "--framegraph-dag-frame must be a positive integer" >&2
    exit 2
  fi
  if [[ ! "$framegraph_dag_frame_radius" =~ ^[0-9]+$ ]]; then
    echo "--framegraph-dag-frame-radius must be a non-negative integer" >&2
    exit 2
  fi
  if [[ -z "$framegraph_dag_formats" ]]; then
    echo "--framegraph-dag-formats must not be empty" >&2
    exit 2
  fi
  if [[ ! "$framegraph_dag_formats" =~ ^[A-Za-z0-9_,.-]+$ ]]; then
    echo "--framegraph-dag-formats must be a comma-separated token list" >&2
    exit 2
  fi
  if [[ -n "$framegraph_dag_optimize" &&
        ! "$framegraph_dag_optimize" =~ ^[A-Za-z0-9_,.-]+$ ]]; then
    echo "--framegraph-dag-optimize must be a comma-separated token list" >&2
    exit 2
  fi
fi

if [[ -z "$suffix" ]]; then
  suffix="probe-$(date +%Y%m%d-%H%M%S)-frame${frame}"
fi

if [[ -z "$min_free_mb" ]]; then
  if (( capture_gputrace )); then
    min_free_mb=$recommended_gputrace_min_free_mb
  else
    min_free_mb=256
  fi
fi

if [[ ! "$min_free_mb" =~ ^[0-9]+$ ]]; then
  echo "--min-free-mb must be a non-negative integer" >&2
  exit 2
fi

if [[ -n "$max_gpu_command_buffer_regression_ms" &&
      ! "$max_gpu_command_buffer_regression_ms" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
  echo "--max-gpu-command-buffer-regression-ms must be numeric milliseconds" >&2
  exit 2
fi

if [[ -n "$max_const_upload_break_count_ratio" &&
      ( ! "$max_const_upload_break_count_ratio" =~ ^[0-9]+([.][0-9]+)?$ ||
        "$max_const_upload_break_count_ratio" =~ ^0+([.]0+)?$ ) ]]; then
  echo "--max-const-upload-break-count-ratio must be positive numeric" >&2
  exit 2
fi

if [[ ! "$min_top_pso_samples_per_draw" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
  echo "--min-top-pso-samples-per-draw must be numeric" >&2
  exit 2
fi

if [[ ! "$min_top_dxmt_joined_fraction" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
  echo "--min-top-dxmt-joined-fraction must be numeric" >&2
  exit 2
fi

if [[ ! "$top_n" =~ ^[0-9]+$ ]] || (( top_n == 0 )); then
  echo "--top must be a positive integer" >&2
  exit 2
fi

if [[ ! "$hot_gpu_share" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
  echo "--hot-gpu-share must be numeric" >&2
  exit 2
fi

if [[ ! "$semantic_image_min_active_pct" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
  echo "--semantic-image-min-active-pct must be numeric" >&2
  exit 2
fi

semantic_image_compare_requested=0
if [[ -n "$semantic_image_policy$semantic_image_before$semantic_image_after$semantic_image_output$semantic_image_summary_output$semantic_image_diff_output" ]]; then
  semantic_image_compare_requested=1
fi
if (( semantic_image_compare_requested )); then
  if [[ "$semantic_image_policy" != exact && "$semantic_image_policy" != lsb1 ]]; then
    echo "--semantic-image-policy must be one of: exact, lsb1" >&2
    exit 2
  fi
  if [[ -z "$semantic_image_before" || -z "$semantic_image_after" ]]; then
    echo "--semantic-image-policy requires --semantic-image-before and --semantic-image-after" >&2
    exit 2
  fi
fi

if [[ -n "$force_cull_mode" &&
      "$force_cull_mode" != none &&
      "$force_cull_mode" != front &&
      "$force_cull_mode" != back ]]; then
  echo "--force-cull-mode must be one of: none, front, back" >&2
  exit 2
fi

if [[ -n "$probe_force_cull_mode" &&
      "$probe_force_cull_mode" != none &&
      "$probe_force_cull_mode" != front &&
      "$probe_force_cull_mode" != back ]]; then
  echo "--probe-force-cull-mode must be one of: none, front, back" >&2
  exit 2
fi

if [[ -n "$optimize_opaque_depth_index_cache_min_gain_pct" &&
      ! "$optimize_opaque_depth_index_cache_min_gain_pct" =~ ^[0-9]+$ ]]; then
  echo "--optimize-opaque-depth-index-cache-min-gain-pct must be a non-negative integer" >&2
  exit 2
fi

if [[ -n "$optimize_screen_blend_index_cache_min_gain_pct" &&
      ! "$optimize_screen_blend_index_cache_min_gain_pct" =~ ^[0-9]+$ ]]; then
  echo "--optimize-screen-blend-index-cache-min-gain-pct must be a non-negative integer" >&2
  exit 2
fi

if [[ -n "$probe_apply_index_cache_opt_candidate_min_gain_pct" &&
      ! "$probe_apply_index_cache_opt_candidate_min_gain_pct" =~ ^[0-9]+$ ]]; then
  echo "--probe-apply-index-cache-opt-candidate-min-gain-pct must be a non-negative integer" >&2
  exit 2
fi

if [[ -n "$index_cache_candidate_frontier_cap" &&
      ! "$index_cache_candidate_frontier_cap" =~ ^[0-9]+$ ]]; then
  echo "--index-cache-candidate-frontier-cap must be a non-negative integer" >&2
  exit 2
fi
if [[ -n "$dump_indexed_geometry_texture0_width" &&
      ! "$dump_indexed_geometry_texture0_width" =~ ^[0-9]+$ ]]; then
  echo "--dump-indexed-geometry-texture0-width must be a non-negative integer" >&2
  exit 2
fi
if [[ -n "$dump_indexed_geometry_texture0_height" &&
      ! "$dump_indexed_geometry_texture0_height" =~ ^[0-9]+$ ]]; then
  echo "--dump-indexed-geometry-texture0-height must be a non-negative integer" >&2
  exit 2
fi
if [[ -n "$dump_indexed_geometry_texture0_format" &&
      ! "$dump_indexed_geometry_texture0_format" =~ ^(0[xX][0-9a-fA-F]+|[0-9]+)$ ]]; then
  echo "--dump-indexed-geometry-texture0-format must be an integer format value" >&2
  exit 2
fi
if [[ -n "$probe_force_texture_white_texture0_width" &&
      ! "$probe_force_texture_white_texture0_width" =~ ^[0-9]+$ ]]; then
  echo "--probe-force-texture-white-texture0-width must be a non-negative integer" >&2
  exit 2
fi
if [[ -n "$probe_force_texture_white_texture0" &&
      ! "$probe_force_texture_white_texture0" =~ ^(0[xX][0-9a-fA-F]+|[0-9]+)$ ]]; then
  echo "--probe-force-texture-white-texture0 must be an integer handle" >&2
  exit 2
fi
if [[ -n "$probe_force_texture_white_texture0_height" &&
      ! "$probe_force_texture_white_texture0_height" =~ ^[0-9]+$ ]]; then
  echo "--probe-force-texture-white-texture0-height must be a non-negative integer" >&2
  exit 2
fi
if [[ -n "$probe_force_texture_white_texture0_format" &&
      ! "$probe_force_texture_white_texture0_format" =~ ^(0[xX][0-9a-fA-F]+|[0-9]+)$ ]]; then
  echo "--probe-force-texture-white-texture0-format must be an integer format value" >&2
  exit 2
fi
if [[ -n "$probe_force_texture_white_draw_ordinal" &&
      ! "$probe_force_texture_white_draw_ordinal" =~ ^[0-9]+$ ]]; then
  echo "--probe-force-texture-white-draw-ordinal must be a non-negative integer" >&2
  exit 2
fi
if [[ -n "$probe_force_texture_white_draw_ordinals" &&
      ! "$probe_force_texture_white_draw_ordinals" =~ ^[0-9,[:space:]]+$ ]]; then
  echo "--probe-force-texture-white-draw-ordinals must be a comma/space separated non-negative integer list" >&2
  exit 2
fi
if [[ -n "$probe_force_texture_white_draw_ordinal_min" &&
      ! "$probe_force_texture_white_draw_ordinal_min" =~ ^[0-9]+$ ]]; then
  echo "--probe-force-texture-white-draw-ordinal-min must be a non-negative integer" >&2
  exit 2
fi
if [[ -n "$probe_force_texture_white_draw_ordinal_max" &&
      ! "$probe_force_texture_white_draw_ordinal_max" =~ ^[0-9]+$ ]]; then
  echo "--probe-force-texture-white-draw-ordinal-max must be a non-negative integer" >&2
  exit 2
fi
if [[ -n "$probe_force_texture_white_command_index" &&
      ! "$probe_force_texture_white_command_index" =~ ^[0-9]+$ ]]; then
  echo "--probe-force-texture-white-command-index must be a non-negative integer" >&2
  exit 2
fi
if [[ -n "$probe_force_texture_white_command_indexes" &&
      ! "$probe_force_texture_white_command_indexes" =~ ^[0-9,[:space:]]+$ ]]; then
  echo "--probe-force-texture-white-command-indexes must be a comma/space separated non-negative integer list" >&2
  exit 2
fi
if [[ -n "$probe_force_texture_white_command_index_min" &&
      ! "$probe_force_texture_white_command_index_min" =~ ^[0-9]+$ ]]; then
  echo "--probe-force-texture-white-command-index-min must be a non-negative integer" >&2
  exit 2
fi
if [[ -n "$probe_force_texture_white_command_index_max" &&
      ! "$probe_force_texture_white_command_index_max" =~ ^[0-9]+$ ]]; then
  echo "--probe-force-texture-white-command-index-max must be a non-negative integer" >&2
  exit 2
fi
if [[ -n "$probe_force_texture_white_command_draw_index" &&
      ! "$probe_force_texture_white_command_draw_index" =~ ^[0-9]+$ ]]; then
  echo "--probe-force-texture-white-command-draw-index must be a non-negative integer" >&2
  exit 2
fi
if [[ -n "$probe_force_texture_white_command_draw_indexes" &&
      ! "$probe_force_texture_white_command_draw_indexes" =~ ^[0-9,[:space:]]+$ ]]; then
  echo "--probe-force-texture-white-command-draw-indexes must be a comma/space separated non-negative integer list" >&2
  exit 2
fi
if [[ -n "$probe_force_texture_white_command_draw_index_min" &&
      ! "$probe_force_texture_white_command_draw_index_min" =~ ^[0-9]+$ ]]; then
  echo "--probe-force-texture-white-command-draw-index-min must be a non-negative integer" >&2
  exit 2
fi
if [[ -n "$probe_force_texture_white_command_draw_index_max" &&
      ! "$probe_force_texture_white_command_draw_index_max" =~ ^[0-9]+$ ]]; then
  echo "--probe-force-texture-white-command-draw-index-max must be a non-negative integer" >&2
  exit 2
fi
if [[ -n "$probe_disable_alpha_blend_texture0_width" &&
      ! "$probe_disable_alpha_blend_texture0_width" =~ ^[0-9]+$ ]]; then
  echo "--probe-disable-alpha-blend-texture0-width must be a non-negative integer" >&2
  exit 2
fi
if [[ -n "$probe_disable_alpha_blend_texture0" &&
      ! "$probe_disable_alpha_blend_texture0" =~ ^(0[xX][0-9a-fA-F]+|[0-9]+)$ ]]; then
  echo "--probe-disable-alpha-blend-texture0 must be an integer handle" >&2
  exit 2
fi
if [[ -n "$probe_disable_alpha_blend_texture0_height" &&
      ! "$probe_disable_alpha_blend_texture0_height" =~ ^[0-9]+$ ]]; then
  echo "--probe-disable-alpha-blend-texture0-height must be a non-negative integer" >&2
  exit 2
fi
if [[ -n "$probe_disable_alpha_blend_texture0_format" &&
      ! "$probe_disable_alpha_blend_texture0_format" =~ ^(0[xX][0-9a-fA-F]+|[0-9]+)$ ]]; then
  echo "--probe-disable-alpha-blend-texture0-format must be an integer format value" >&2
  exit 2
fi
if [[ -n "$probe_depth_func_always_texture0_width" &&
      ! "$probe_depth_func_always_texture0_width" =~ ^[0-9]+$ ]]; then
  echo "--probe-depth-func-always-texture0-width must be a non-negative integer" >&2
  exit 2
fi
if [[ -n "$probe_depth_func_always_texture0" &&
      ! "$probe_depth_func_always_texture0" =~ ^(0[xX][0-9a-fA-F]+|[0-9]+)$ ]]; then
  echo "--probe-depth-func-always-texture0 must be an integer handle" >&2
  exit 2
fi
if [[ -n "$probe_depth_func_always_texture0_height" &&
      ! "$probe_depth_func_always_texture0_height" =~ ^[0-9]+$ ]]; then
  echo "--probe-depth-func-always-texture0-height must be a non-negative integer" >&2
  exit 2
fi
if [[ -n "$probe_depth_func_always_texture0_format" &&
      ! "$probe_depth_func_always_texture0_format" =~ ^(0[xX][0-9a-fA-F]+|[0-9]+)$ ]]; then
  echo "--probe-depth-func-always-texture0-format must be an integer format value" >&2
  exit 2
fi
if [[ -n "$effect_draw_trace_texture0" &&
      ! "$effect_draw_trace_texture0" =~ ^(0[xX][0-9a-fA-F]+|[0-9]+)$ ]]; then
  echo "--effect-draw-trace-texture0 must be an integer handle" >&2
  exit 2
fi
if [[ -n "$effect_draw_trace_seq_min" &&
      ! "$effect_draw_trace_seq_min" =~ ^[0-9]+$ ]]; then
  echo "--effect-draw-trace-seq-min must be a non-negative integer" >&2
  exit 2
fi
if [[ -n "$effect_draw_trace_seq_max" &&
      ! "$effect_draw_trace_seq_max" =~ ^[0-9]+$ ]]; then
  echo "--effect-draw-trace-seq-max must be a non-negative integer" >&2
  exit 2
fi
if [[ -n "$effect_draw_trace_texture0_width" &&
      ! "$effect_draw_trace_texture0_width" =~ ^[0-9]+$ ]]; then
  echo "--effect-draw-trace-texture0-width must be a non-negative integer" >&2
  exit 2
fi
if [[ -n "$effect_draw_trace_texture0_height" &&
      ! "$effect_draw_trace_texture0_height" =~ ^[0-9]+$ ]]; then
  echo "--effect-draw-trace-texture0-height must be a non-negative integer" >&2
  exit 2
fi
if [[ -n "$effect_draw_trace_texture0_format" &&
      ! "$effect_draw_trace_texture0_format" =~ ^(0[xX][0-9a-fA-F]+|[0-9]+)$ ]]; then
  echo "--effect-draw-trace-texture0-format must be an integer format value" >&2
  exit 2
fi
if [[ -n "$effect_draw_trace_primitive_type" &&
      ! "$effect_draw_trace_primitive_type" =~ ^[0-9]+$ ]]; then
  echo "--effect-draw-trace-primitive-type must be a non-negative integer" >&2
  exit 2
fi
if [[ -n "$effect_draw_trace_geometry_max_refs" &&
      ! "$effect_draw_trace_geometry_max_refs" =~ ^[0-9]+$ ]]; then
  echo "--effect-draw-trace-geometry-max-refs must be a non-negative integer" >&2
  exit 2
fi
if (( index_cache_candidate_lazy_frontier && index_cache_candidate_bucketed_select )); then
  echo "--index-cache-candidate-lazy-frontier and --index-cache-candidate-bucketed-select are mutually exclusive" >&2
  exit 2
fi
if [[ -n "$dump_depth_attachment_handle" &&
      ! "$dump_depth_attachment_handle" =~ ^(0[xX][0-9a-fA-F]+|[0-9]+)$ ]]; then
  echo "--dump-depth-attachment-handle must be an integer handle, e.g. 0x300000100000001" >&2
  exit 2
fi
if [[ -n "$dump_depth_attachment_seq" &&
      ! "$dump_depth_attachment_seq" =~ ^[0-9]+$ ]]; then
  echo "--dump-depth-attachment-seq must be a non-negative integer" >&2
  exit 2
fi
if [[ -n "$dump_depth_attachment_enc" &&
      ! "$dump_depth_attachment_enc" =~ ^[0-9]+$ ]]; then
  echo "--dump-depth-attachment-enc must be a non-negative integer" >&2
  exit 2
fi
if [[ -z "$dump_depth_attachment_handle" &&
      ( -n "$dump_depth_attachment_seq" ||
        -n "$dump_depth_attachment_enc" ||
        -n "$dump_depth_attachment_path" ) ]]; then
  echo "--dump-depth-attachment-seq/enc/path require --dump-depth-attachment-handle" >&2
  exit 2
fi
if [[ -n "$dump_color_attachment_handle" &&
      ! "$dump_color_attachment_handle" =~ ^(0[xX][0-9a-fA-F]+|[0-9]+)$ ]]; then
  echo "--dump-color-attachment-handle must be an integer handle, e.g. 0x30000900000000b" >&2
  exit 2
fi
if [[ -n "$dump_color_attachment_index" &&
      ! "$dump_color_attachment_index" =~ ^[0-9]+$ ]]; then
  echo "--dump-color-attachment-index must be a non-negative integer" >&2
  exit 2
fi
if [[ -n "$dump_color_attachment_draw" &&
      ! "$dump_color_attachment_draw" =~ ^[0-9]+$ ]]; then
  echo "--dump-color-attachment-draw must be a non-negative integer" >&2
  exit 2
fi
if [[ -n "$dump_color_attachment_draws" &&
      ! "$dump_color_attachment_draws" =~ ^[0-9,:\;[:space:]]+$ ]]; then
  echo "--dump-color-attachment-draws must be a comma/space separated non-negative integer list" >&2
  exit 2
fi
if [[ -n "$dump_color_attachment_draws" &&
      -z "$dump_color_attachment_dir$dump_color_attachment_roi_summary_path" ]]; then
  echo "--dump-color-attachment-draws requires --dump-color-attachment-dir or --dump-color-attachment-roi-summary-path" >&2
  exit 2
fi
if [[ -n "$dump_color_attachment_command_index" &&
      ! "$dump_color_attachment_command_index" =~ ^[0-9]+$ ]]; then
  echo "--dump-color-attachment-command-index must be a non-negative integer" >&2
  exit 2
fi
if [[ -n "$dump_color_attachment_command_index_min" &&
      ! "$dump_color_attachment_command_index_min" =~ ^[0-9]+$ ]]; then
  echo "--dump-color-attachment-command-index-min must be a non-negative integer" >&2
  exit 2
fi
if [[ -n "$dump_color_attachment_command_index_max" &&
      ! "$dump_color_attachment_command_index_max" =~ ^[0-9]+$ ]]; then
  echo "--dump-color-attachment-command-index-max must be a non-negative integer" >&2
  exit 2
fi
if [[ -n "$dump_color_attachment_command_index_min$dump_color_attachment_command_index_max" &&
      -z "$dump_color_attachment_dir$dump_color_attachment_roi_summary_path" ]]; then
  echo "--dump-color-attachment-command-index-min/max requires --dump-color-attachment-dir or --dump-color-attachment-roi-summary-path" >&2
  exit 2
fi
if [[ -n "$dump_color_attachment_texture0" &&
      ! "$dump_color_attachment_texture0" =~ ^(0[xX][0-9a-fA-F]+|[0-9]+)$ ]]; then
  echo "--dump-color-attachment-texture0 must be an integer handle, e.g. 0x200000100000077" >&2
  exit 2
fi
if [[ -n "$dump_color_attachment_texture0s" &&
      ! "$dump_color_attachment_texture0s" =~ ^[0-9a-fA-FxX,:\;\ ]+$ ]]; then
  echo "--dump-color-attachment-texture0s must be a comma/space separated integer handle list" >&2
  exit 2
fi
if [[ -n "$dump_color_attachment_texture0s" &&
      -z "$dump_color_attachment_dir$dump_color_attachment_roi_summary_path" ]]; then
  echo "--dump-color-attachment-texture0s requires --dump-color-attachment-dir or --dump-color-attachment-roi-summary-path" >&2
  exit 2
fi
if [[ -n "$dump_color_attachment_seq" &&
      ! "$dump_color_attachment_seq" =~ ^[0-9]+$ ]]; then
  echo "--dump-color-attachment-seq must be a non-negative integer" >&2
  exit 2
fi
if [[ -n "$dump_color_attachment_enc" &&
      ! "$dump_color_attachment_enc" =~ ^[0-9]+$ ]]; then
  echo "--dump-color-attachment-enc must be a non-negative integer" >&2
  exit 2
fi
if [[ -n "$dump_color_attachment_path" &&
      -z "$dump_color_attachment_handle$dump_color_attachment_index$dump_color_attachment_seq$dump_color_attachment_enc$dump_color_attachment_draw$dump_color_attachment_draws$dump_color_attachment_command_index$dump_color_attachment_command_index_min$dump_color_attachment_command_index_max$dump_color_attachment_texture0$dump_color_attachment_texture0s" ]]; then
  echo "--dump-color-attachment-path requires --dump-color-attachment-handle, --dump-color-attachment-index, --dump-color-attachment-seq/enc, --dump-color-attachment-draw, --dump-color-attachment-draws, --dump-color-attachment-command-index, --dump-color-attachment-command-index-min/max, --dump-color-attachment-texture0, or --dump-color-attachment-texture0s" >&2
  exit 2
fi
if [[ -n "$dump_color_attachment_dir" &&
      -z "$dump_color_attachment_handle$dump_color_attachment_index$dump_color_attachment_seq$dump_color_attachment_enc$dump_color_attachment_draw$dump_color_attachment_draws$dump_color_attachment_command_index$dump_color_attachment_command_index_min$dump_color_attachment_command_index_max$dump_color_attachment_texture0$dump_color_attachment_texture0s" ]]; then
  echo "--dump-color-attachment-dir requires --dump-color-attachment-handle, --dump-color-attachment-index, --dump-color-attachment-seq/enc, --dump-color-attachment-draw, --dump-color-attachment-draws, --dump-color-attachment-command-index, --dump-color-attachment-command-index-min/max, --dump-color-attachment-texture0, or --dump-color-attachment-texture0s" >&2
  exit 2
fi
if [[ -n "$dump_color_attachment_roi_summary_path" &&
      -z "$dump_color_attachment_handle$dump_color_attachment_index$dump_color_attachment_seq$dump_color_attachment_enc$dump_color_attachment_draw$dump_color_attachment_draws$dump_color_attachment_command_index$dump_color_attachment_command_index_min$dump_color_attachment_command_index_max$dump_color_attachment_texture0$dump_color_attachment_texture0s" ]]; then
  echo "--dump-color-attachment-roi-summary-path requires --dump-color-attachment-handle, --dump-color-attachment-index, --dump-color-attachment-seq/enc, --dump-color-attachment-draw, --dump-color-attachment-draws, --dump-color-attachment-command-index, --dump-color-attachment-command-index-min/max, --dump-color-attachment-texture0, or --dump-color-attachment-texture0s" >&2
  exit 2
fi
if [[ -n "$dump_color_attachment_roi_summary_path" &&
      -z "$dump_color_attachment_rois" ]]; then
  echo "--dump-color-attachment-roi-summary-path requires at least one --dump-color-attachment-roi" >&2
  exit 2
fi
if [[ -n "$dump_color_attachment_rois" &&
      ! "$dump_color_attachment_rois" =~ ^[0-9]+,[0-9]+,[0-9]+,[0-9]+(:[A-Za-z0-9_.-]+)?(\;[0-9]+,[0-9]+,[0-9]+,[0-9]+(:[A-Za-z0-9_.-]+)?)*$ ]]; then
  echo "--dump-color-attachment-roi must be L,T,R,B[:NAME]; use the option multiple times for multiple ROIs" >&2
  exit 2
fi
if [[ -n "$dump_color_attachment_bright_threshold" &&
      ! "$dump_color_attachment_bright_threshold" =~ ^[0-9]+$ ]]; then
  echo "--dump-color-attachment-bright-threshold must be a non-negative integer" >&2
  exit 2
fi
if [[ -n "$dump_color_attachment_white_threshold" &&
      ! "$dump_color_attachment_white_threshold" =~ ^[0-9]+$ ]]; then
  echo "--dump-color-attachment-white-threshold must be a non-negative integer" >&2
  exit 2
fi
if [[ -n "$dump_color_attachment_warm_red_threshold" &&
      ! "$dump_color_attachment_warm_red_threshold" =~ ^[0-9]+$ ]]; then
  echo "--dump-color-attachment-warm-red-threshold must be a non-negative integer" >&2
  exit 2
fi
if [[ -n "$dump_color_attachment_warm_green_threshold" &&
      ! "$dump_color_attachment_warm_green_threshold" =~ ^[0-9]+$ ]]; then
  echo "--dump-color-attachment-warm-green-threshold must be a non-negative integer" >&2
  exit 2
fi
if [[ -n "$dump_color_attachment_warm_blue_margin" &&
      ! "$dump_color_attachment_warm_blue_margin" =~ ^[0-9]+$ ]]; then
  echo "--dump-color-attachment-warm-blue-margin must be a non-negative integer" >&2
  exit 2
fi
if (( dump_color_attachment_after_draw )) &&
   [[ -z "$dump_color_attachment_draw$dump_color_attachment_draws$dump_color_attachment_command_index$dump_color_attachment_command_index_min$dump_color_attachment_command_index_max$dump_color_attachment_texture0$dump_color_attachment_texture0s" ]]; then
  echo "--dump-color-attachment-after-draw requires --dump-color-attachment-draw, --dump-color-attachment-draws, --dump-color-attachment-command-index, --dump-color-attachment-command-index-min/max, --dump-color-attachment-texture0, or --dump-color-attachment-texture0s" >&2
  exit 2
fi
if [[ -n "$dump_draw_texture_handles" &&
      ! "$dump_draw_texture_handles" =~ ^[0-9a-fA-FxX,:\;\ ]+$ ]]; then
  echo "--dump-draw-texture-handles must be a comma/space separated integer handle list" >&2
  exit 2
fi
if [[ -n "$dump_draw_texture0_width" &&
      ! "$dump_draw_texture0_width" =~ ^[0-9]+$ ]]; then
  echo "--dump-draw-texture0-width must be a non-negative integer" >&2
  exit 2
fi
if [[ -n "$dump_draw_texture0_height" &&
      ! "$dump_draw_texture0_height" =~ ^[0-9]+$ ]]; then
  echo "--dump-draw-texture0-height must be a non-negative integer" >&2
  exit 2
fi
if [[ -n "$dump_draw_texture0_format" &&
      ! "$dump_draw_texture0_format" =~ ^(0[xX][0-9a-fA-F]+|[0-9]+)$ ]]; then
  echo "--dump-draw-texture0-format must be an integer format value" >&2
  exit 2
fi
if [[ -n "$dump_draw_texture_seq" &&
      ! "$dump_draw_texture_seq" =~ ^[0-9]+$ ]]; then
  echo "--dump-draw-texture-seq must be a non-negative integer" >&2
  exit 2
fi
if [[ -n "$dump_draw_texture_enc" &&
      ! "$dump_draw_texture_enc" =~ ^[0-9]+$ ]]; then
  echo "--dump-draw-texture-enc must be a non-negative integer" >&2
  exit 2
fi
if [[ -z "$dump_draw_texture_handles$dump_draw_texture0_width$dump_draw_texture0_height$dump_draw_texture0_format" &&
      "$dump_draw_texture0_any" -eq 0 &&
      ( -n "$dump_draw_texture_seq" ||
        -n "$dump_draw_texture_enc" ||
        -n "$dump_draw_texture_dir" ) ]]; then
  echo "--dump-draw-texture-seq/enc/dir require --dump-draw-texture-handles or --dump-draw-texture0-* filter" >&2
  exit 2
fi

if (( require_screen_blend_cache_proof )); then
  require_semantic_image_proof=1
  require_stable_frame_proof=1
  require_target_index_cache_opt_miss32_decrease=1
  require_target_reordered_index_cache_hits=1
  require_target_vs_buffer_write_decrease=1
  require_target_vs_invocations_decrease=1
  if (( ! semantic_image_compare_requested )); then
    echo "--require-screen-blend-cache-proof requires --semantic-image-policy with --semantic-image-before and --semantic-image-after" >&2
    exit 2
  fi
  if (( ! optimize_screen_blend_index_cache )); then
    echo "--require-screen-blend-cache-proof requires --optimize-screen-blend-index-cache" >&2
    exit 2
  fi
  if (( ${#target_row_keys[@]} == 0 )); then
    echo "--require-screen-blend-cache-proof requires at least one --target-row-key" >&2
    exit 2
  fi
  measure_index_reuse=1
  measure_index_cache_opt_candidate=1
fi

if (( require_opaque_depth_index_cache_proof )); then
  require_stable_frame_proof=1
  require_target_index_cache_opt_miss32_decrease=1
  require_target_reordered_index_cache_hits=1
  require_target_vs_buffer_write_decrease=1
  require_target_vs_invocations_decrease=1
  if (( ! optimize_opaque_depth_index_cache )); then
    echo "--require-opaque-depth-index-cache-proof requires --optimize-opaque-depth-index-cache" >&2
    exit 2
  fi
  if (( ${#target_row_keys[@]} == 0 )); then
    echo "--require-opaque-depth-index-cache-proof requires at least one --target-row-key" >&2
    exit 2
  fi
  measure_index_reuse=1
  measure_index_cache_opt_candidate=1
fi

if (( require_cache_opt_apply_proof )); then
  require_stable_frame_proof=1
  require_target_index_cache_miss32_decrease=1
  require_target_vs_buffer_write_decrease=1
  require_target_vs_invocations_decrease=1
  if (( probe_apply_index_cache_opt_candidate_unsafe_nonopaque )); then
    require_semantic_image_proof=1
  fi
  if (( ${#target_row_keys[@]} == 0 )); then
    echo "--require-cache-opt-apply-proof requires at least one --target-row-key" >&2
    exit 2
  fi
fi

if (( require_semantic_image_proof && ! semantic_image_compare_requested )); then
  echo "--require-semantic-image-proof requires --semantic-image-policy with --semantic-image-before and --semantic-image-after" >&2
  exit 2
fi

if (( require_stable_frame_proof )); then
  if (( ! allow_partial_stable_frame_proof )); then
    require_result_json=1
  fi
  require_top_gpu_decrease=1
  require_top_vs_buffer_write_decrease=1
  require_top_unexplained_buffer_write_decrease=1
  require_top_row_key_match=1
  require_top_pso_attribution=1
  require_xcode_counter_coverage=1
  require_dxmt_join_coverage=1
  if [[ -z "$max_top_draw_call_delta_ratio" ]]; then
    max_top_draw_call_delta_ratio=0.05
  fi
  if [[ -z "$max_top_vertex_count_delta_ratio" ]]; then
    max_top_vertex_count_delta_ratio=0.05
  fi
  if [[ -z "$max_top_triangle_delta_ratio" ]]; then
    max_top_triangle_delta_ratio=0.05
  fi
fi

if [[ -n "$max_top_gpu_regression_ms" &&
      ! "$max_top_gpu_regression_ms" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
  echo "--max-top-gpu-regression-ms must be numeric milliseconds" >&2
  exit 2
fi

if [[ -n "$max_top_buffer_write_regression_mib" &&
      ! "$max_top_buffer_write_regression_mib" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
  echo "--max-top-buffer-write-regression-mib must be numeric MiB" >&2
  exit 2
fi

if [[ -n "$max_top_unexplained_buffer_write_ratio" &&
      ! "$max_top_unexplained_buffer_write_ratio" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
  echo "--max-top-unexplained-buffer-write-ratio must be numeric" >&2
  exit 2
fi
if [[ -n "$max_top_draw_call_delta_ratio" &&
      ! "$max_top_draw_call_delta_ratio" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
  echo "--max-top-draw-call-delta-ratio must be numeric" >&2
  exit 2
fi
if [[ -n "$max_top_vertex_count_delta_ratio" &&
      ! "$max_top_vertex_count_delta_ratio" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
  echo "--max-top-vertex-count-delta-ratio must be numeric" >&2
  exit 2
fi
if [[ -n "$max_top_triangle_delta_ratio" &&
      ! "$max_top_triangle_delta_ratio" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
  echo "--max-top-triangle-delta-ratio must be numeric" >&2
  exit 2
fi
if [[ -n "$max_non_target_gpu_regression_ms" &&
      ! "$max_non_target_gpu_regression_ms" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
  echo "--max-non-target-gpu-regression-ms must be numeric" >&2
  exit 2
fi
if [[ -n "$max_non_target_vs_buffer_write_regression_mib" &&
      ! "$max_non_target_vs_buffer_write_regression_mib" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
  echo "--max-non-target-vs-buffer-write-regression-mib must be numeric" >&2
  exit 2
fi
if [[ -n "$max_non_target_vs_invocations_regression_ratio" &&
      ! "$max_non_target_vs_invocations_regression_ratio" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
  echo "--max-non-target-vs-invocations-regression-ratio must be numeric" >&2
  exit 2
fi
if [[ -n "$max_non_target_draw_call_delta_ratio" &&
      ! "$max_non_target_draw_call_delta_ratio" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
  echo "--max-non-target-draw-call-delta-ratio must be numeric" >&2
  exit 2
fi
if [[ -n "$max_non_target_vertex_count_delta_ratio" &&
      ! "$max_non_target_vertex_count_delta_ratio" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
  echo "--max-non-target-vertex-count-delta-ratio must be numeric" >&2
  exit 2
fi
if [[ -n "$max_non_target_triangle_delta_ratio" &&
      ! "$max_non_target_triangle_delta_ratio" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
  echo "--max-non-target-triangle-delta-ratio must be numeric" >&2
  exit 2
fi

xcode_compare_requested=0
if (( require_top_gpu_decrease ||
      require_top_buffer_write_decrease ||
      require_top_vs_buffer_write_decrease ||
      require_top_unexplained_buffer_write_decrease ||
      require_stream_handle_churn_decrease ||
      require_ib_handle_churn_decrease ||
      require_argbuf_cbuf_decrease ||
      require_transient_decrease ||
      require_top_gpu_share_increase ||
      require_top_row_key_match ||
      require_tvb_mechanism_proof ||
      require_target_index_cache_miss32_decrease ||
      require_target_index_cache_opt_miss32_decrease ||
      require_target_reordered_index_cache_hits ||
      require_target_vs_buffer_write_decrease ||
      require_target_vs_invocations_decrease )) ||
   [[ -n "$max_top_gpu_regression_ms" ||
      -n "$max_top_buffer_write_regression_mib" ||
      ${#target_row_keys[@]} -gt 0 ||
      -n "$max_non_target_gpu_regression_ms" ||
      -n "$max_non_target_vs_buffer_write_regression_mib" ||
      -n "$max_non_target_vs_invocations_regression_ratio" ||
      -n "$max_non_target_draw_call_delta_ratio" ||
      -n "$max_non_target_vertex_count_delta_ratio" ||
      -n "$max_non_target_triangle_delta_ratio" ||
      -n "$max_top_unexplained_buffer_write_ratio" ||
      -n "$max_top_draw_call_delta_ratio" ||
      -n "$max_top_vertex_count_delta_ratio" ||
      -n "$max_top_triangle_delta_ratio" ]]; then
  xcode_compare_requested=1
fi

run_level_compare_requested=0
if (( require_color_dontcare_increase ||
      require_depth_dontcare_increase ||
      require_tile_preservation_decrease ||
      require_tile_preservation_not_increase ||
      require_command_buffers_per_present_not_increase ||
      require_render_passes_per_present_not_increase ||
      require_render_pass_carry_promotion_gates ||
      require_encoder_final_end_reason_not_increase ||
      require_encoder_final_same_key_reopen_not_increase ||
      require_encoder_color_load_not_increase ||
      require_encoder_depth_load_not_increase ||
      require_draw_run_records_increase ||
      require_draw_run_records_per_submit_increase ||
      require_binding_overrides_present ||
      require_const_upload_passthrough_present ||
      require_draw_submission_batch_present ||
      require_const_upload_break_bytes_decrease ||
      require_encode_draw_cpu_decrease ||
      require_completion_present_wait_decrease ||
      require_completion_wait_with_enqueue_increase ||
      require_completion_wait_without_enqueue_decrease ||
      require_completion_present_wait_with_enqueue_increase ||
      require_completion_present_wait_without_enqueue_decrease ||
      require_encode_ready_depth_gt1_increase ||
      require_commit_chunk_replay_cpu_per_present_decrease ||
      require_queue_draw_submission_cpu_per_present_decrease ||
      require_snapshot_cpu_per_present_decrease ||
      require_snapshot_cache_lookup_cpu_per_present_decrease ||
      require_snapshot_cache_uniform_build_cpu_per_present_decrease ||
      require_snapshot_cache_uniform_hash_cpu_per_present_decrease ||
      require_batch_miss_uniform_build_cpu_per_present_decrease ||
      require_batch_miss_uniform_hash_cpu_per_present_decrease ||
      require_batch_miss_vs_const_hash_cpu_per_present_decrease ||
      require_batch_miss_ps_const_hash_cpu_per_present_decrease ||
      require_batch_miss_nonconst_hash_cpu_per_present_decrease ||
      require_snapshot_uniform_copy_cpu_per_present_decrease ||
      require_submit_draw_run_batch_append_uniform_cpu_per_present_decrease ||
      require_draw_uniform_payload_lookup_cpu_per_present_decrease ||
      require_draw_uniform_payload_append_copy_cpu_per_present_decrease ||
      require_argbuf_setup_cpu_per_present_decrease ||
      require_argbuf_open_cpu_per_present_decrease ||
      require_argbuf_cbuf_update_cpu_per_present_decrease ||
      require_argbuf_cbuf_update_vs_cpu_per_present_decrease ||
      require_uniform_compact_saved_bytes_present ||
      require_snapshot_state_elided_present ||
      require_discarded_state_not_increase ||
      require_submission_carrier_bytes_per_record_decrease ||
      require_submission_carrier_uniform_storage_per_record_decrease ||
      require_encode_chunk_cpu_per_present_decrease ||
      require_no_enqueue_commit_entry_to_publish_decrease ||
      require_no_enqueue_publish_to_encode_dequeue_decrease ||
      require_no_enqueue_encode_dequeue_to_commit_decrease ||
      require_no_enqueue_wait_to_next_enqueue_decrease ||
      require_no_enqueue_before_publish_closure_decrease ||
      require_no_enqueue_before_publish_inter_replay_gap_decrease ||
      require_pe_focused_between_call_gap_residual_decrease )) ||
   [[ -n "$max_gpu_command_buffer_regression_ms" ||
      -n "$max_const_upload_break_count_ratio" ]]; then
  run_level_compare_requested=1
fi

if (( xcode_compare_requested )) && [[ -z "$compare_baseline_joined" ]]; then
  echo "Xcode comparison gates require --baseline-joined <joined.csv>" >&2
  exit 2
fi

if (( run_level_compare_requested )) && [[ -z "$compare_baseline_output" ]]; then
  echo "run-level comparison gates require --compare-baseline-output <output-dir>" >&2
  exit 2
fi

if [[ -n "$compare_baseline_output" ]]; then
  baseline_result="$compare_baseline_output"
  if [[ -d "$baseline_result" ]]; then
    baseline_result="$baseline_result/result.json"
  fi
  if [[ ! -f "$baseline_result" ]]; then
    echo "missing baseline result.json: $baseline_result" >&2
    exit 2
  fi
fi

if [[ -n "$compare_baseline_joined" && ! -f "$compare_baseline_joined" ]]; then
  echo "missing baseline joined CSV: $compare_baseline_joined" >&2
  exit 2
fi

if (( require_shader_dump_matches && ! dump_shaders )); then
  echo "--require-shader-dump-matches requires --dump-shaders" >&2
  exit 2
fi

frame_local_index_diagnostics_requested=0
if (( measure_index_reuse ||
      measure_index_cache_opt_candidate ||
      dump_indexed_geometry ||
      force_expand_indexed ||
      probe_force_expand_indexed ||
      probe_reverse_indexed_triangles ||
      probe_reverse_opaque_indexed_triangles ||
      probe_reverse_nonopaque_indexed_triangles ||
      probe_sort_indexed_triangles_by_min_index ||
      probe_optimize_indexed_triangles_vertex_cache ||
      probe_apply_index_cache_opt_candidate ||
      probe_apply_index_cache_opt_candidate_unsafe_nonopaque ||
      optimize_opaque_depth_index_cache ||
      optimize_screen_blend_index_order ||
      optimize_screen_blend_index_cache )) ||
   [[ -n "$split_large_indexed_draws$split_large_indexed_draws_stream0_span_max$split_large_indexed_draws_max_chunks_per_draw$split_large_indexed_draws_row$split_large_indexed_draws_rows$split_large_indexed_draws_class$split_large_indexed_draws_classes$probe_force_expand_indexed_row$probe_force_expand_indexed_rows$probe_force_expand_indexed_class$probe_force_expand_indexed_classes$probe_reverse_indexed_triangles_row$probe_reverse_indexed_triangles_rows$probe_reverse_indexed_triangles_class$probe_reverse_indexed_triangles_classes$probe_reverse_indexed_triangles_stream0_span_min$probe_indexed_triangle_encoder_draw_min$probe_indexed_triangle_encoder_draw_max$probe_indexed_triangle_encoder_draw_exclude" ]]; then
  frame_local_index_diagnostics_requested=1
fi

if (( ! encoder_breakdown_enabled )) && (( capture_gputrace )); then
  echo "--no-encoder-breakdown requires --no-gputrace; Xcode/gputrace proof needs encoder rows" >&2
  exit 2
fi

if [[ -n "$encoder_breakdown_seq" &&
      -n "$encoder_breakdown_seq_min$encoder_breakdown_seq_max" ]]; then
  echo "--encoder-breakdown-seq and --encoder-breakdown-seq-range are mutually exclusive" >&2
  exit 2
fi

if [[ -n "$encoder_breakdown_seq_min$encoder_breakdown_seq_max" ]]; then
  if [[ -z "$encoder_breakdown_seq_min" || -z "$encoder_breakdown_seq_max" ]]; then
    echo "encoder breakdown seq range requires both min and max" >&2
    exit 2
  fi
  if (( encoder_breakdown_seq_min > encoder_breakdown_seq_max )); then
    echo "encoder breakdown seq range min must be <= max" >&2
    exit 2
  fi
fi

if (( encoder_breakdown_enabled )) &&
   (( ! encoder_breakdown_all_frames )) &&
   [[ -z "$encoder_breakdown_seq$encoder_breakdown_seq_min$encoder_breakdown_seq_max" && -n "$dump_color_attachment_seq" ]]; then
  encoder_breakdown_seq=$dump_color_attachment_seq
fi

if (( encoder_breakdown_enabled )) &&
   (( capture_gputrace || frame_local_index_diagnostics_requested )) &&
   (( ! encoder_breakdown_all_frames )) &&
   [[ -z "$encoder_breakdown_seq$encoder_breakdown_seq_min$encoder_breakdown_seq_max" ]]; then
  encoder_breakdown_seq=$frame
fi

print_space_hints() {
  local stream=${1:-2}
  echo "space usage hints:" >&"$stream"
  if command -v du >/dev/null 2>&1; then
    du -sh \
      "$repo_root/traces" \
      "$repo_root/experiments/output" \
      "$repo_root/build-x86_64-builtin" 2>/dev/null |
      sort -hr >&"$stream" || true
    echo "large trace run directories:" >&"$stream"
    du -sh "$repo_root/traces"/app-d3d9-3dmark05-* 2>/dev/null |
      sort -hr |
      head -12 >&"$stream" || true
    echo "large output run directories:" >&"$stream"
    du -sh "$repo_root/experiments/output"/app-d3d9-3dmark05-* 2>/dev/null |
      sort -hr |
      head -12 >&"$stream" || true
    echo "large ignored/manual-review candidates:" >&"$stream"
    du -sh \
      "$repo_root/experiments/prefixs"/* \
      "$repo_root/experiments/apps_3rd" \
      "$repo_root/experiments/wine/vendor" \
      "$repo_root/experiments/wine/sikarugir-cx-24.0.7" 2>/dev/null |
      sort -hr |
      head -20 >&"$stream" || true
  fi
  if command -v find >/dev/null 2>&1; then
    echo "large trace/output files:" >&"$stream"
    find "$repo_root/traces" "$repo_root/experiments/output" \
      -type f -size +50M -exec ls -lh {} \; 2>/dev/null |
      sort -k5 -hr |
      head -20 >&"$stream" || true
  fi
  echo "cleanup note: remove only obsolete run ids after preserving needed analysis artifacts; do not delete active prefixes blindly." >&"$stream"
}

cleanup_3dmark05_probe_wineserver() {
  if [[ "${DXMT_3DMARK05_KILL_SERVER_ON_EXIT:-1}" == "0" ]]; then
    return 0
  fi
  if [[ ! -x "$probe_wineserver" ]]; then
    echo "warning: cannot run 3DMark05 wineserver cleanup; missing $probe_wineserver" >&2
    return 0
  fi
  WINEPREFIX="$probe_prefix" "$probe_wineserver" -k >/dev/null 2>&1 || true
}

run_id="app-d3d9-3dmark05-${suffix}"
output_dir="$repo_root/experiments/output/$run_id"
trace_dir="$repo_root/traces/$run_id"
analysis_dir="$trace_dir/analysis"
probe_prefix="$repo_root/experiments/prefixs/app-d3d9-3dmark05"
probe_wine_root="${DXMT_3DMARK05_WINE_ROOT:-$repo_root/experiments/wine/sikarugir-cx-24.0.7}"
probe_wineserver="${DXMT_3DMARK05_WINESERVER:-$probe_wine_root/bin/wineserver}"
shader_dump_dir="$analysis_dir/shaders"
shader_msl_dump_dir="$shader_dump_dir/msl"
shader_bytecode_dump_dir="$shader_dump_dir/bytecode"
geometry_dump_dir="$analysis_dir/geometry"
framegraph_dag_dir="$analysis_dir/dag"
framegraph_dag_summary="$analysis_dir/framegraph-dag-summary.md"
framegraph_dag_summary_csv="$analysis_dir/framegraph-dag-summary.csv"
framegraph_dag_candidates_csv="$analysis_dir/framegraph-dag-candidates.csv"
framegraph_dag_preopt_summary="$analysis_dir/framegraph-dag-preopt-summary.md"
framegraph_dag_preopt_summary_csv="$analysis_dir/framegraph-dag-preopt-summary.csv"
framegraph_dag_preopt_candidates_csv="$analysis_dir/framegraph-dag-preopt-candidates.csv"
framegraph_dag_postopt_summary="$analysis_dir/framegraph-dag-postopt-summary.md"
framegraph_dag_postopt_summary_csv="$analysis_dir/framegraph-dag-postopt-summary.csv"
framegraph_dag_postopt_candidates_csv="$analysis_dir/framegraph-dag-postopt-candidates.csv"
visibility_scout_default_path="$analysis_dir/frame${frame}-visibility-scout.csv"
visibility_scout_summary_default_path="$analysis_dir/frame${frame}-visibility-scout-summary.md"
visibility_scout_summary_csv_default_path="$analysis_dir/frame${frame}-visibility-scout-summary.csv"
if [[ -n "$capture_frames$capture_range" ]]; then
  if [[ -z "$capture_dir" ]]; then
    capture_dir="$analysis_dir/captures"
  elif [[ "$capture_dir" != /* ]]; then
    capture_dir="$repo_root/$capture_dir"
  fi
fi
if [[ -n "$dump_depth_attachment_handle" ]]; then
  if [[ -z "$dump_depth_attachment_path" ]]; then
    dump_depth_attachment_path="$analysis_dir/frame${frame}-depth.bin"
  elif [[ "$dump_depth_attachment_path" != /* ]]; then
    dump_depth_attachment_path="$repo_root/$dump_depth_attachment_path"
  fi
fi
if [[ -n "$dump_color_attachment_handle$dump_color_attachment_index$dump_color_attachment_seq$dump_color_attachment_enc$dump_color_attachment_draw$dump_color_attachment_draws$dump_color_attachment_command_index$dump_color_attachment_command_index_min$dump_color_attachment_command_index_max$dump_color_attachment_texture0$dump_color_attachment_texture0s$dump_color_attachment_path$dump_color_attachment_dir$dump_color_attachment_roi_summary_path" ]]; then
  if [[ -z "$dump_color_attachment_handle$dump_color_attachment_index" ]]; then
    dump_color_attachment_index=0
  fi
  if [[ -n "$dump_color_attachment_dir" ]]; then
    if [[ "$dump_color_attachment_dir" != /* ]]; then
      dump_color_attachment_dir="$repo_root/$dump_color_attachment_dir"
    fi
  elif [[ -z "$dump_color_attachment_path" && -z "$dump_color_attachment_roi_summary_path" ]]; then
    dump_color_attachment_path="$analysis_dir/frame${frame}-color.bin"
  elif [[ -n "$dump_color_attachment_path" &&
          "$dump_color_attachment_path" != /* ]]; then
    dump_color_attachment_path="$repo_root/$dump_color_attachment_path"
  fi
  if [[ -n "$dump_color_attachment_roi_summary_path" &&
        "$dump_color_attachment_roi_summary_path" != /* ]]; then
    dump_color_attachment_roi_summary_path="$repo_root/$dump_color_attachment_roi_summary_path"
  fi
fi
if [[ -n "$dump_draw_texture_handles$dump_draw_texture0_width$dump_draw_texture0_height$dump_draw_texture0_format" ||
      "$dump_draw_texture0_any" -eq 1 ]]; then
  if [[ -z "$dump_draw_texture_dir" ]]; then
    dump_draw_texture_dir="$analysis_dir/textures"
  elif [[ "$dump_draw_texture_dir" != /* ]]; then
    dump_draw_texture_dir="$repo_root/$dump_draw_texture_dir"
  fi
fi
summary_path="$output_dir/3dmark05-perf-summary.md"
encoders_csv="$output_dir/3dmark05-perf-encoders.csv"
stream_csv="$output_dir/3dmark05-perf-encoder-streams.csv"
probe_draws_csv="$output_dir/3dmark05-perf-indexed-probe-draws.csv"
index_cache_runtime_report="$output_dir/3dmark05-index-cache-runtime-summary.md"
index_cache_runtime_csv="$output_dir/3dmark05-index-cache-runtime-summary.csv"
capture_path="$trace_dir/frame${frame}.gputrace"
trace_artifacts_json="$output_dir/3dmark05-trace-artifacts.json"
xcode_performance_gputrace="$analysis_dir/frame${frame}-performance.gputrace"
xcode_encoder_counters_csv="$analysis_dir/frame${frame}-counters-xcode.csv"
xcode_counters_summary_csv="$analysis_dir/frame${frame}-counters-summary.csv"
xcode_joined_summary_csv="$analysis_dir/frame${frame}-xcode-dxmt-joined-summary.csv"
xcode_bottleneck_report="$analysis_dir/frame${frame}-xcode-dxmt-bottleneck-report.md"
metal_system_trace="$trace_dir/metal-system.trace"
metal_gpu_intervals_xml="$analysis_dir/metal-gpu-intervals.xml"
xctrace_gpu_intervals_summary_csv="$analysis_dir/xctrace-metal-gpu-intervals-summary.csv"
xctrace_gpu_intervals_summary_md="$analysis_dir/xctrace-metal-gpu-intervals-summary.md"
counter_comparison_path="$analysis_dir/frame${frame}-perf-counter-comparison.md"
free_mb=unknown
if command -v df >/dev/null 2>&1; then
  free_kb=$(df -Pk "$repo_root" | awk 'NR==2 {print $4}')
  if [[ "$free_kb" =~ ^[0-9]+$ ]]; then
    free_mb=$(( free_kb / 1024 ))
  fi
fi

session_locked=$(detect_session_locked)

env_args=(
  "DXMT_EXPERIMENT_PROFILE=perf"
  "DXMT_3DMARK05_DIRECT=1"
  "DXMT_3DMARK05_PREFIX=$probe_prefix"
  "DXMT_3DMARK05_WINE_ROOT=$probe_wine_root"
  "DXMT_3DMARK05_WINESERVER=$probe_wineserver"
  "DXMT_DISABLE_AUTO_EXPAND_INDEXED=1"
  "DXMT_3DMARK05_RESULT_FILE=$result_file"
  "DXMT_3DMARK05_LOG=$output_dir/3dmark05-direct.log"
)

if (( encoder_breakdown_enabled )); then
  env_args+=("DXMT9_PERF_ENCODER_BREAKDOWN=1")
fi

if (( encoder_breakdown_enabled )) && [[ -n "$encoder_breakdown_seq" ]]; then
  env_args+=("DXMT9_PERF_ENCODER_BREAKDOWN_SEQ=$encoder_breakdown_seq")
fi
if (( encoder_breakdown_enabled )) && [[ -n "$encoder_breakdown_seq_min" ]]; then
  env_args+=("DXMT9_PERF_ENCODER_BREAKDOWN_SEQ_MIN=$encoder_breakdown_seq_min")
fi
if (( encoder_breakdown_enabled )) && [[ -n "$encoder_breakdown_seq_max" ]]; then
  env_args+=("DXMT9_PERF_ENCODER_BREAKDOWN_SEQ_MAX=$encoder_breakdown_seq_max")
fi

if [[ -n "$render_pass_reentry_top" ]]; then
  env_args+=("DXMT9_PERF_RENDER_PASS_REENTRY_TOP=$render_pass_reentry_top")
fi

if [[ "$frame_sampling" != "0" && -n "$frame_sampling" ]]; then
  env_args+=("DXMT9_PERF_FRAME_SAMPLING=$frame_sampling")
fi

if [[ "$open_cb_preencode_tail_present" != "0" && -n "$open_cb_preencode_tail_present" ]]; then
  env_args+=("DXMT9_OPEN_CB_PREENCODE_TAIL_PRESENT=$open_cb_preencode_tail_present")
fi

if [[ "$open_cb_carry_render_session" != "0" && -n "$open_cb_carry_render_session" ]]; then
  env_args+=("DXMT9_OPEN_CB_CARRY_RENDER_SESSION=$open_cb_carry_render_session")
fi

if [[ "$open_cb_semantic_boundary_publish" != "0" && -n "$open_cb_semantic_boundary_publish" ]]; then
  env_args+=("DXMT9_OPEN_CB_SEMANTIC_BOUNDARY_PUBLISH=$open_cb_semantic_boundary_publish")
fi

if [[ -n "$open_cb_cpu_ready_command_limit" ]]; then
  env_args+=("DXMT9_OPEN_CB_CPU_READY_COMMAND_LIMIT=$open_cb_cpu_ready_command_limit")
fi

if [[ "$open_cb_writer_active_cpu_ready_publish" != "0" && -n "$open_cb_writer_active_cpu_ready_publish" ]]; then
  env_args+=("DXMT9_OPEN_CB_WRITER_ACTIVE_CPU_READY_PUBLISH=$open_cb_writer_active_cpu_ready_publish")
fi

if [[ "$open_cb_active_wait_cpu_ready_append" != "0" && -n "$open_cb_active_wait_cpu_ready_append" ]]; then
  env_args+=("DXMT9_OPEN_CB_ACTIVE_WAIT_CPU_READY_APPEND=$open_cb_active_wait_cpu_ready_append")
fi

if [[ "$open_cb_wait_start_cpu_ready_publish" != "0" && -n "$open_cb_wait_start_cpu_ready_publish" ]]; then
  env_args+=("DXMT9_OPEN_CB_WAIT_START_CPU_READY_PUBLISH=$open_cb_wait_start_cpu_ready_publish")
fi

if [[ -n "$open_cb_semantic_boundary_release_mode" ]]; then
  env_args+=("DXMT9_OPEN_CB_SEMANTIC_BOUNDARY_RELEASE_MODE=$open_cb_semantic_boundary_release_mode")
fi

if [[ -n "$open_cb_pending_tail_wait_us" ]]; then
  env_args+=("DXMT9_OPEN_CB_PENDING_TAIL_WAIT_US=$open_cb_pending_tail_wait_us")
fi

if [[ -n "$stage_pre_present_command_limit" ]]; then
  env_args+=("DXMT9_STAGE_PRE_PRESENT_COMMAND_LIMIT=$stage_pre_present_command_limit")
fi

if [[ -n "$draw_chunk_command_limit" ]]; then
  env_args+=("DXMT9_DRAW_CHUNK_COMMAND_LIMIT=$draw_chunk_command_limit")
fi

if [[ "$enable_chunk_end_carry" != "0" && -n "$enable_chunk_end_carry" ]]; then
  env_args+=("DXMT9_ENABLE_CHUNK_END_CARRY=$enable_chunk_end_carry")
fi

if [[ "$draw_packet_actual_change" != "0" && -n "$draw_packet_actual_change" ]]; then
  env_args+=("DXMT9_PERF_DRAW_PACKET_ACTUAL_CHANGE=$draw_packet_actual_change")
fi

if [[ "$vs_const_setter_range" != "0" && -n "$vs_const_setter_range" ]]; then
  env_args+=("DXMT9_PERF_VS_CONST_SETTER_RANGE=$vs_const_setter_range")
fi

if [[ "$pe_recorder_stats" != "0" && -n "$pe_recorder_stats" ]]; then
  env_args+=("DXMT9_PE_RECORDER_STATS=$pe_recorder_stats")
  if [[ -z "$dxmt_log_level" ]]; then
    dxmt_log_level=info
  fi
fi

if [[ "$pe_recorder_chunk_log" != "0" && -n "$pe_recorder_chunk_log" ]]; then
  env_args+=("DXMT9_PE_RECORDER_CHUNK_LOG=$pe_recorder_chunk_log")
fi

if [[ "$pe_flush_after_clear" != "0" && -n "$pe_flush_after_clear" ]]; then
  env_args+=("DXMT9_PE_FLUSH_AFTER_CLEAR=$pe_flush_after_clear")
fi

if [[ "$pe_flush_after_draw" != "0" && -n "$pe_flush_after_draw" ]]; then
  env_args+=("DXMT9_PE_FLUSH_AFTER_DRAW=$pe_flush_after_draw")
fi

if [[ "$pe_draw_full_snapshot" != "0" && -n "$pe_draw_full_snapshot" ]]; then
  env_args+=("DXMT9_PE_DRAW_FULL_SNAPSHOT=$pe_draw_full_snapshot")
fi

if [[ -n "$pe_chunk_max_records" ]]; then
  env_args+=("DXMT9_PE_CHUNK_MAX_RECORDS=$pe_chunk_max_records")
fi

if [[ -n "$pe_chunk_max_bytes" ]]; then
  env_args+=("DXMT9_PE_CHUNK_MAX_BYTES=$pe_chunk_max_bytes")
fi

if [[ -n "$dxmt_log_level" ]]; then
  env_args+=("DXMT_LOG_LEVEL=$dxmt_log_level")
fi

if (( dump_framegraph_dag )); then
  env_args+=(
    "DXMT9_RENDERER_DUMP_DAG=$framegraph_dag_dir"
    "DXMT9_RENDERER_DUMP_DAG_FRAME=$framegraph_dag_frame"
    "DXMT9_RENDERER_DUMP_DAG_FRAME_RADIUS=$framegraph_dag_frame_radius"
    "DXMT9_RENDERER_DUMP_DAG_FORMATS=$framegraph_dag_formats"
  )
  if [[ -n "$framegraph_dag_optimize" ]]; then
    env_args+=("DXMT9_RENDERER_DUMP_DAG_OPTIMIZE=$framegraph_dag_optimize")
  fi
  if (( framegraph_dag_draws )); then
    env_args+=("DXMT9_RENDERER_DUMP_DAG_DRAWS=1")
  fi
fi

if (( capture_gputrace )); then
  if [[ "${DXMT_3DMARK05_SET_MTL_CAPTURE_ENABLED:-0}" != "0" ]]; then
    env_args+=("MTL_CAPTURE_ENABLED=1")
  fi
  env_args+=(
    "DXMT_METAL_CAPTURE_FRAME=$frame"
    "DXMT_METAL_CAPTURE_PATH=$capture_path"
  )
  if [[ -n "$metal_capture_destination" ]]; then
    env_args+=("DXMT_METAL_CAPTURE_DESTINATION=$metal_capture_destination")
  fi
fi

if (( aggressive_color_dontcare )); then
  env_args+=("DXMT9_AGGRESSIVE_COLOR_DONTCARE=1")
fi

if (( trim_unused_varyings )); then
  env_args+=("DXMT9_TRIM_UNUSED_VARYINGS=1")
  if [[ -n "$trim_unused_varyings_vs_hashes" ]]; then
    env_args+=("DXMT9_TRIM_UNUSED_VARYINGS_VS_HASHES=$trim_unused_varyings_vs_hashes")
  fi
  if [[ -n "$trim_unused_varyings_ps_hashes" ]]; then
    env_args+=("DXMT9_TRIM_UNUSED_VARYINGS_PS_HASHES=$trim_unused_varyings_ps_hashes")
  fi
elif [[ -n "$trim_unused_varyings_vs_hashes$trim_unused_varyings_ps_hashes" ]]; then
  echo "--trim-unused-varyings-vs-hashes/--trim-unused-varyings-ps-hashes require --trim-unused-varyings" >&2
  exit 2
fi

if (( drop_vsout_point_size )); then
  env_args+=("DXMT9_PROBE_DROP_VSOUT_POINT_SIZE=1")
fi

if (( probe_position_only_vsout )); then
  env_args+=("DXMT9_PROBE_POSITION_ONLY_VSOUT=1")
fi

if (( probe_half_vsout )); then
  env_args+=("DXMT9_PROBE_HALF_VSOUT=1")
fi

if (( force_fragment_color )); then
  env_args+=("DXMT_DEBUG_FORCE_FRAGMENT_COLOR=1")
fi

if (( trim_vertex_temps )); then
  env_args+=("DXMT9_TRIM_VERTEX_TEMPS=1")
fi

if (( trim_vs_output_scratch )); then
  env_args+=("DXMT9_TRIM_VS_OUTPUT_SCRATCH=1")
fi

if (( split_sparse_const_records )); then
  env_args+=("DXMT9_SPLIT_SPARSE_CONST_RECORDS=1")
fi

if (( suppress_rt_pixel_format_view )); then
  env_args+=("DXMT9_SUPPRESS_RT_PIXEL_FORMAT_VIEW=1")
fi

if (( suppress_x8_rt_pixel_format_view )); then
  env_args+=("DXMT9_SUPPRESS_X8_RT_PIXEL_FORMAT_VIEW=1")
fi

if (( x8_shader_alpha_fill )); then
  env_args+=("DXMT9_X8_SHADER_ALPHA_FILL=1")
fi

if (( native_metal_base_vertex )); then
  env_args+=("DXMT9_USE_NATIVE_METAL_BASE_VERTEX=1")
fi

if (( optimize_screen_blend_index_order )); then
  env_args+=("DXMT9_OPTIMIZE_SCREEN_BLEND_INDEX_ORDER=1")
fi

if [[ -n "$optimize_screen_blend_index_order_row" ]]; then
  env_args+=("DXMT9_OPTIMIZE_SCREEN_BLEND_INDEX_ORDER_ROW=$optimize_screen_blend_index_order_row")
fi

if [[ -n "$optimize_screen_blend_index_order_rows" ]]; then
  env_args+=("DXMT9_OPTIMIZE_SCREEN_BLEND_INDEX_ORDER_ROWS=$optimize_screen_blend_index_order_rows")
fi

if [[ -n "$optimize_screen_blend_index_order_class" ]]; then
  env_args+=("DXMT9_OPTIMIZE_SCREEN_BLEND_INDEX_ORDER_CLASS=$optimize_screen_blend_index_order_class")
fi

if [[ -n "$optimize_screen_blend_index_order_classes" ]]; then
  env_args+=("DXMT9_OPTIMIZE_SCREEN_BLEND_INDEX_ORDER_CLASSES=$optimize_screen_blend_index_order_classes")
fi

if [[ -n "$optimize_screen_blend_index_order_stream0_span_min" ]]; then
  env_args+=("DXMT9_OPTIMIZE_SCREEN_BLEND_INDEX_ORDER_STREAM0_SPAN_MIN=$optimize_screen_blend_index_order_stream0_span_min")
fi

if [[ -n "$split_large_indexed_draws" ]]; then
  env_args+=("DXMT9_SPLIT_LARGE_INDEXED_DRAWS=$split_large_indexed_draws")
fi

if [[ -n "$split_large_indexed_draws_stream0_span_max" ]]; then
  env_args+=("DXMT9_SPLIT_LARGE_INDEXED_DRAWS_STREAM0_SPAN_MAX=$split_large_indexed_draws_stream0_span_max")
fi

if [[ -n "$split_large_indexed_draws_max_chunks_per_draw" ]]; then
  env_args+=("DXMT9_SPLIT_LARGE_INDEXED_DRAWS_MAX_CHUNKS_PER_DRAW=$split_large_indexed_draws_max_chunks_per_draw")
fi

if [[ -n "$split_large_indexed_draws_row" ]]; then
  env_args+=("DXMT9_SPLIT_LARGE_INDEXED_DRAWS_ROW=$split_large_indexed_draws_row")
fi

if [[ -n "$split_large_indexed_draws_rows" ]]; then
  env_args+=("DXMT9_SPLIT_LARGE_INDEXED_DRAWS_ROWS=$split_large_indexed_draws_rows")
fi

if [[ -n "$split_large_indexed_draws_class" ]]; then
  env_args+=("DXMT9_SPLIT_LARGE_INDEXED_DRAWS_CLASS=$split_large_indexed_draws_class")
fi

if [[ -n "$split_large_indexed_draws_classes" ]]; then
  env_args+=("DXMT9_SPLIT_LARGE_INDEXED_DRAWS_CLASSES=$split_large_indexed_draws_classes")
fi

if (( force_expand_indexed )); then
  env_args+=("DXMT_FORCE_EXPAND_INDEXED=1")
fi

if (( probe_force_expand_indexed )); then
  env_args+=("DXMT9_PROBE_FORCE_EXPAND_INDEXED=1")
fi

if [[ -n "$probe_force_expand_indexed_row" ]]; then
  env_args+=("DXMT9_PROBE_FORCE_EXPAND_INDEXED_ROW=$probe_force_expand_indexed_row")
fi

if [[ -n "$probe_force_expand_indexed_rows" ]]; then
  env_args+=("DXMT9_PROBE_FORCE_EXPAND_INDEXED_ROWS=$probe_force_expand_indexed_rows")
fi

if [[ -n "$probe_force_expand_indexed_class" ]]; then
  env_args+=("DXMT9_PROBE_FORCE_EXPAND_INDEXED_CLASS=$probe_force_expand_indexed_class")
fi

if [[ -n "$probe_force_expand_indexed_classes" ]]; then
  env_args+=("DXMT9_PROBE_FORCE_EXPAND_INDEXED_CLASSES=$probe_force_expand_indexed_classes")
fi

if (( probe_reverse_indexed_triangles )); then
  env_args+=("DXMT9_PROBE_REVERSE_INDEXED_TRIANGLES=1")
fi

if (( probe_reverse_opaque_indexed_triangles )); then
  env_args+=("DXMT9_PROBE_REVERSE_OPAQUE_INDEXED_TRIANGLES=1")
fi

if (( probe_reverse_nonopaque_indexed_triangles )); then
  env_args+=("DXMT9_PROBE_REVERSE_NONOPAQUE_INDEXED_TRIANGLES=1")
fi

if (( probe_sort_indexed_triangles_by_min_index )); then
  env_args+=("DXMT9_PROBE_SORT_INDEXED_TRIANGLES_BY_MIN_INDEX=1")
fi

if (( probe_optimize_indexed_triangles_vertex_cache )); then
  env_args+=("DXMT9_PROBE_OPTIMIZE_INDEXED_TRIANGLES_VERTEX_CACHE=1")
fi

if (( optimize_opaque_depth_index_cache )); then
  env_args+=("DXMT9_OPTIMIZE_OPAQUE_DEPTH_INDEX_CACHE=1")
fi

if [[ -n "$optimize_opaque_depth_index_cache_min_gain_pct" ]]; then
  env_args+=("DXMT9_OPTIMIZE_OPAQUE_DEPTH_INDEX_CACHE_MIN_GAIN_PCT=$optimize_opaque_depth_index_cache_min_gain_pct")
fi

if [[ -n "$index_cache_candidate_frontier_cap" ]]; then
  env_args+=("DXMT9_INDEX_CACHE_CANDIDATE_FRONTIER_CAP=$index_cache_candidate_frontier_cap")
fi

if (( index_cache_candidate_lazy_frontier )); then
  env_args+=("DXMT9_INDEX_CACHE_CANDIDATE_LAZY_FRONTIER=1")
fi

if (( index_cache_candidate_bucketed_select )); then
  env_args+=("DXMT9_INDEX_CACHE_CANDIDATE_BUCKETED_SELECT=1")
fi

if (( index_cache_candidate_strict_lru )); then
  env_args+=("DXMT9_INDEX_CACHE_CANDIDATE_STRICT_LRU=1")
fi

if (( index_cache_candidate_upper_bound_gate )); then
  env_args+=("DXMT9_INDEX_CACHE_CANDIDATE_UPPER_BOUND_GATE=1")
fi

if (( optimize_screen_blend_index_cache )); then
  env_args+=("DXMT9_OPTIMIZE_SCREEN_BLEND_INDEX_CACHE=1")
fi

if [[ -n "$optimize_screen_blend_index_cache_min_gain_pct" ]]; then
  env_args+=("DXMT9_OPTIMIZE_SCREEN_BLEND_INDEX_CACHE_MIN_GAIN_PCT=$optimize_screen_blend_index_cache_min_gain_pct")
fi

if (( probe_apply_index_cache_opt_candidate )); then
  env_args+=("DXMT9_PROBE_APPLY_INDEX_CACHE_OPT_CANDIDATE=1")
fi

if (( probe_apply_index_cache_opt_candidate_unsafe_nonopaque )); then
  env_args+=("DXMT9_PROBE_APPLY_INDEX_CACHE_OPT_CANDIDATE_UNSAFE_NONOPAQUE=1")
fi

if [[ -n "$probe_apply_index_cache_opt_candidate_min_gain_pct" ]]; then
  env_args+=("DXMT9_PROBE_APPLY_INDEX_CACHE_OPT_CANDIDATE_MIN_GAIN_PCT=$probe_apply_index_cache_opt_candidate_min_gain_pct")
fi

if [[ -n "$probe_reverse_indexed_triangles_row" ]]; then
  env_args+=("DXMT9_PROBE_REVERSE_INDEXED_TRIANGLES_ROW=$probe_reverse_indexed_triangles_row")
fi

if [[ -n "$probe_reverse_indexed_triangles_rows" ]]; then
  env_args+=("DXMT9_PROBE_REVERSE_INDEXED_TRIANGLES_ROWS=$probe_reverse_indexed_triangles_rows")
fi

if [[ -n "$probe_reverse_indexed_triangles_class" ]]; then
  env_args+=("DXMT9_PROBE_REVERSE_INDEXED_TRIANGLES_CLASS=$probe_reverse_indexed_triangles_class")
fi

if [[ -n "$probe_reverse_indexed_triangles_classes" ]]; then
  env_args+=("DXMT9_PROBE_REVERSE_INDEXED_TRIANGLES_CLASSES=$probe_reverse_indexed_triangles_classes")
fi

if [[ -n "$probe_reverse_indexed_triangles_stream0_span_min" ]]; then
  env_args+=("DXMT9_PROBE_REVERSE_INDEXED_TRIANGLES_STREAM0_SPAN_MIN=$probe_reverse_indexed_triangles_stream0_span_min")
fi

if [[ -n "$probe_indexed_triangle_encoder_draw_min" ]]; then
  env_args+=("DXMT9_PROBE_INDEXED_TRIANGLE_ENCODER_DRAW_MIN=$probe_indexed_triangle_encoder_draw_min")
fi

if [[ -n "$probe_indexed_triangle_encoder_draw_max" ]]; then
  env_args+=("DXMT9_PROBE_INDEXED_TRIANGLE_ENCODER_DRAW_MAX=$probe_indexed_triangle_encoder_draw_max")
fi

if [[ -n "$probe_indexed_triangle_encoder_draw_exclude" ]]; then
  env_args+=("DXMT9_PROBE_INDEXED_TRIANGLE_ENCODER_DRAW_EXCLUDE=$probe_indexed_triangle_encoder_draw_exclude")
fi

if [[ -n "$probe_scissor_rect" ]]; then
  env_args+=("DXMT9_PROBE_SCISSOR_RECT=$probe_scissor_rect")
fi

if [[ -n "$probe_scissor_rect_row" ]]; then
  env_args+=("DXMT9_PROBE_SCISSOR_RECT_ROW=$probe_scissor_rect_row")
fi

if [[ -n "$probe_scissor_rect_rows" ]]; then
  env_args+=("DXMT9_PROBE_SCISSOR_RECT_ROWS=$probe_scissor_rect_rows")
fi

if [[ -n "$probe_scissor_rect_class" ]]; then
  env_args+=("DXMT9_PROBE_SCISSOR_RECT_CLASS=$probe_scissor_rect_class")
fi

if [[ -n "$probe_scissor_rect_classes" ]]; then
  env_args+=("DXMT9_PROBE_SCISSOR_RECT_CLASSES=$probe_scissor_rect_classes")
fi

if [[ -n "$probe_force_cull_mode" ]]; then
  env_args+=("DXMT9_PROBE_FORCE_CULL_MODE=$probe_force_cull_mode")
fi

if [[ -n "$probe_force_cull_mode_row" ]]; then
  env_args+=("DXMT9_PROBE_FORCE_CULL_MODE_ROW=$probe_force_cull_mode_row")
fi

if [[ -n "$probe_force_cull_mode_rows" ]]; then
  env_args+=("DXMT9_PROBE_FORCE_CULL_MODE_ROWS=$probe_force_cull_mode_rows")
fi

if [[ -n "$probe_force_cull_mode_class" ]]; then
  env_args+=("DXMT9_PROBE_FORCE_CULL_MODE_CLASS=$probe_force_cull_mode_class")
fi

if [[ -n "$probe_force_cull_mode_classes" ]]; then
  env_args+=("DXMT9_PROBE_FORCE_CULL_MODE_CLASSES=$probe_force_cull_mode_classes")
fi

if [[ -n "$force_cull_mode" ]]; then
  env_args+=("DXMT_DEBUG_FORCE_CULL_MODE=$force_cull_mode")
fi

if (( measure_index_reuse )); then
  env_args+=("DXMT9_MEASURE_INDEX_REUSE=1")
fi

if (( measure_index_cache_opt_candidate )); then
  env_args+=("DXMT9_MEASURE_INDEX_CACHE_OPT_CANDIDATE=1")
fi

if (( dump_indexed_geometry )); then
  env_args+=(
    "DXMT9_DUMP_INDEXED_GEOMETRY_DIR=$geometry_dump_dir"
    "DXMT9_DUMP_INDEXED_GEOMETRY_MAX_DRAWS=$dump_indexed_geometry_max_draws"
  )
  if (( dump_indexed_geometry_cbufs )); then
    env_args+=("DXMT9_DUMP_INDEXED_GEOMETRY_CBUFS=1")
  fi
  if [[ -n "$dump_indexed_geometry_vs" ]]; then
    env_args+=("DXMT9_DUMP_INDEXED_GEOMETRY_VS=$dump_indexed_geometry_vs")
  fi
  if [[ -n "$dump_indexed_geometry_ps" ]]; then
    env_args+=("DXMT9_DUMP_INDEXED_GEOMETRY_PS=$dump_indexed_geometry_ps")
  fi
  if [[ -n "$dump_indexed_geometry_texture0" ]]; then
    env_args+=("DXMT9_DUMP_INDEXED_GEOMETRY_TEXTURE0=$dump_indexed_geometry_texture0")
  fi
  if [[ -n "$dump_indexed_geometry_texture0_width" ]]; then
    env_args+=("DXMT9_DUMP_INDEXED_GEOMETRY_TEXTURE0_WIDTH=$dump_indexed_geometry_texture0_width")
  fi
  if [[ -n "$dump_indexed_geometry_texture0_height" ]]; then
    env_args+=("DXMT9_DUMP_INDEXED_GEOMETRY_TEXTURE0_HEIGHT=$dump_indexed_geometry_texture0_height")
  fi
  if [[ -n "$dump_indexed_geometry_texture0_format" ]]; then
    env_args+=("DXMT9_DUMP_INDEXED_GEOMETRY_TEXTURE0_FORMAT=$dump_indexed_geometry_texture0_format")
  fi
fi

if [[ -n "$dump_depth_attachment_handle" ]]; then
  env_args+=(
    "DXMT9_DUMP_DEPTH_ATTACHMENT_HANDLE=$dump_depth_attachment_handle"
    "DXMT9_DUMP_DEPTH_ATTACHMENT_PATH=$dump_depth_attachment_path"
  )
  if [[ -n "$dump_depth_attachment_seq" ]]; then
    env_args+=("DXMT9_DUMP_DEPTH_ATTACHMENT_SEQ=$dump_depth_attachment_seq")
  fi
  if [[ -n "$dump_depth_attachment_enc" ]]; then
    env_args+=("DXMT9_DUMP_DEPTH_ATTACHMENT_ENC=$dump_depth_attachment_enc")
  fi
fi

if [[ -n "$dump_color_attachment_handle$dump_color_attachment_index$dump_color_attachment_seq$dump_color_attachment_enc$dump_color_attachment_draw$dump_color_attachment_draws$dump_color_attachment_command_index$dump_color_attachment_command_index_min$dump_color_attachment_command_index_max$dump_color_attachment_texture0$dump_color_attachment_texture0s$dump_color_attachment_path$dump_color_attachment_dir$dump_color_attachment_roi_summary_path" ]]; then
  if [[ -n "$dump_color_attachment_path" ]]; then
    env_args+=(
      "DXMT9_DUMP_COLOR_ATTACHMENT_PATH=$dump_color_attachment_path"
    )
  fi
  if [[ -n "$dump_color_attachment_dir" ]]; then
    env_args+=(
      "DXMT9_DUMP_COLOR_ATTACHMENT_DIR=$dump_color_attachment_dir"
    )
  fi
  if [[ -n "$dump_color_attachment_roi_summary_path" ]]; then
    env_args+=("DXMT9_DUMP_COLOR_ATTACHMENT_ROI_SUMMARY_PATH=$dump_color_attachment_roi_summary_path")
  fi
  if [[ -n "$dump_color_attachment_rois" ]]; then
    env_args+=("DXMT9_DUMP_COLOR_ATTACHMENT_ROIS=$dump_color_attachment_rois")
  fi
  if [[ -n "$dump_color_attachment_bright_threshold" ]]; then
    env_args+=("DXMT9_DUMP_COLOR_ATTACHMENT_BRIGHT_THRESHOLD=$dump_color_attachment_bright_threshold")
  fi
  if [[ -n "$dump_color_attachment_white_threshold" ]]; then
    env_args+=("DXMT9_DUMP_COLOR_ATTACHMENT_WHITE_THRESHOLD=$dump_color_attachment_white_threshold")
  fi
  if [[ -n "$dump_color_attachment_warm_red_threshold" ]]; then
    env_args+=("DXMT9_DUMP_COLOR_ATTACHMENT_WARM_RED_THRESHOLD=$dump_color_attachment_warm_red_threshold")
  fi
  if [[ -n "$dump_color_attachment_warm_green_threshold" ]]; then
    env_args+=("DXMT9_DUMP_COLOR_ATTACHMENT_WARM_GREEN_THRESHOLD=$dump_color_attachment_warm_green_threshold")
  fi
  if [[ -n "$dump_color_attachment_warm_blue_margin" ]]; then
    env_args+=("DXMT9_DUMP_COLOR_ATTACHMENT_WARM_BLUE_MARGIN=$dump_color_attachment_warm_blue_margin")
  fi
  if [[ -n "$dump_color_attachment_handle" ]]; then
    env_args+=("DXMT9_DUMP_COLOR_ATTACHMENT_HANDLE=$dump_color_attachment_handle")
  fi
  if [[ -n "$dump_color_attachment_index" ]]; then
    env_args+=("DXMT9_DUMP_COLOR_ATTACHMENT_INDEX=$dump_color_attachment_index")
  fi
  if [[ -n "$dump_color_attachment_seq" ]]; then
    env_args+=("DXMT9_DUMP_COLOR_ATTACHMENT_SEQ=$dump_color_attachment_seq")
  fi
  if [[ -n "$dump_color_attachment_enc" ]]; then
    env_args+=("DXMT9_DUMP_COLOR_ATTACHMENT_ENC=$dump_color_attachment_enc")
  fi
  if (( dump_color_attachment_after_draw )); then
    env_args+=("DXMT9_DUMP_COLOR_ATTACHMENT_AFTER_DRAW=1")
  fi
  if [[ -n "$dump_color_attachment_draw" ]]; then
    env_args+=("DXMT9_DUMP_COLOR_ATTACHMENT_DRAW=$dump_color_attachment_draw")
  fi
  if [[ -n "$dump_color_attachment_draws" ]]; then
    env_args+=("DXMT9_DUMP_COLOR_ATTACHMENT_DRAWS=$dump_color_attachment_draws")
  fi
  if [[ -n "$dump_color_attachment_command_index" ]]; then
    env_args+=("DXMT9_DUMP_COLOR_ATTACHMENT_COMMAND_INDEX=$dump_color_attachment_command_index")
  fi
  if [[ -n "$dump_color_attachment_command_index_min" ]]; then
    env_args+=("DXMT9_DUMP_COLOR_ATTACHMENT_COMMAND_INDEX_MIN=$dump_color_attachment_command_index_min")
  fi
  if [[ -n "$dump_color_attachment_command_index_max" ]]; then
    env_args+=("DXMT9_DUMP_COLOR_ATTACHMENT_COMMAND_INDEX_MAX=$dump_color_attachment_command_index_max")
  fi
  if [[ -n "$dump_color_attachment_texture0" ]]; then
    env_args+=("DXMT9_DUMP_COLOR_ATTACHMENT_TEXTURE0=$dump_color_attachment_texture0")
  fi
  if [[ -n "$dump_color_attachment_texture0s" ]]; then
    env_args+=("DXMT9_DUMP_COLOR_ATTACHMENT_TEXTURE0S=$dump_color_attachment_texture0s")
  fi
fi

if [[ -n "$dump_draw_texture_handles$dump_draw_texture0_width$dump_draw_texture0_height$dump_draw_texture0_format" ||
      "$dump_draw_texture0_any" -eq 1 ]]; then
  env_args+=(
    "DXMT9_DUMP_DRAW_TEXTURE_DIR=$dump_draw_texture_dir"
  )
  if [[ -n "$dump_draw_texture_handles" ]]; then
    env_args+=("DXMT9_DUMP_DRAW_TEXTURE_HANDLES=$dump_draw_texture_handles")
  fi
  if (( dump_draw_texture0_any )); then
    env_args+=("DXMT9_DUMP_DRAW_TEXTURE0_ANY=1")
  fi
  if [[ -n "$dump_draw_texture0_width" ]]; then
    env_args+=("DXMT9_DUMP_DRAW_TEXTURE0_WIDTH=$dump_draw_texture0_width")
  fi
  if [[ -n "$dump_draw_texture0_height" ]]; then
    env_args+=("DXMT9_DUMP_DRAW_TEXTURE0_HEIGHT=$dump_draw_texture0_height")
  fi
  if [[ -n "$dump_draw_texture0_format" ]]; then
    env_args+=("DXMT9_DUMP_DRAW_TEXTURE0_FORMAT=$dump_draw_texture0_format")
  fi
  if [[ -n "$dump_draw_texture_seq" ]]; then
    env_args+=("DXMT9_DUMP_DRAW_TEXTURE_SEQ=$dump_draw_texture_seq")
  fi
  if [[ -n "$dump_draw_texture_enc" ]]; then
    env_args+=("DXMT9_DUMP_DRAW_TEXTURE_ENC=$dump_draw_texture_enc")
  fi
fi

if (( aggressive_depth_dontcare )); then
  env_args+=("DXMT9_AGGRESSIVE_DEPTH_DONTCARE=1")
fi

if (( disable_cull )); then
  env_args+=("DXMT_DISABLE_CULL=1")
fi

if (( disable_scissor )); then
  env_args+=("DXMT_DISABLE_SCISSOR=1")
fi

if (( disable_alpha_test )); then
  env_args+=("DXMT_DISABLE_ALPHA_TEST=1")
fi

if (( disable_fog )); then
  env_args+=("DXMT_DISABLE_FOG=1")
fi

if (( force_texture_white )); then
  env_args+=("DXMT_FORCE_TEXTURE_WHITE=1")
fi

if (( probe_force_texture_white )); then
  env_args+=("DXMT9_PROBE_FORCE_TEXTURE_WHITE=1")
fi

if [[ -n "$probe_force_texture_white_row" ]]; then
  env_args+=("DXMT9_PROBE_FORCE_TEXTURE_WHITE_ROW=$probe_force_texture_white_row")
fi

if [[ -n "$probe_force_texture_white_rows" ]]; then
  env_args+=("DXMT9_PROBE_FORCE_TEXTURE_WHITE_ROWS=$probe_force_texture_white_rows")
fi

if [[ -n "$probe_force_texture_white_class" ]]; then
  env_args+=("DXMT9_PROBE_FORCE_TEXTURE_WHITE_CLASS=$probe_force_texture_white_class")
fi

if [[ -n "$probe_force_texture_white_classes" ]]; then
  env_args+=("DXMT9_PROBE_FORCE_TEXTURE_WHITE_CLASSES=$probe_force_texture_white_classes")
fi

if [[ -n "$probe_force_texture_white_texture0" ]]; then
  env_args+=("DXMT9_PROBE_FORCE_TEXTURE_WHITE_TEXTURE0=$probe_force_texture_white_texture0")
fi

if [[ -n "$probe_force_texture_white_texture0_width" ]]; then
  env_args+=("DXMT9_PROBE_FORCE_TEXTURE_WHITE_TEXTURE0_WIDTH=$probe_force_texture_white_texture0_width")
fi

if [[ -n "$probe_force_texture_white_texture0_height" ]]; then
  env_args+=("DXMT9_PROBE_FORCE_TEXTURE_WHITE_TEXTURE0_HEIGHT=$probe_force_texture_white_texture0_height")
fi

if [[ -n "$probe_force_texture_white_texture0_format" ]]; then
  env_args+=("DXMT9_PROBE_FORCE_TEXTURE_WHITE_TEXTURE0_FORMAT=$probe_force_texture_white_texture0_format")
fi

if [[ -n "$probe_force_texture_white_draw_ordinal" ]]; then
  env_args+=("DXMT9_PROBE_FORCE_TEXTURE_WHITE_DRAW_ORDINALS=$probe_force_texture_white_draw_ordinal")
fi

if [[ -n "$probe_force_texture_white_draw_ordinals" ]]; then
  env_args+=("DXMT9_PROBE_FORCE_TEXTURE_WHITE_DRAW_ORDINALS=$probe_force_texture_white_draw_ordinals")
fi

if [[ -n "$probe_force_texture_white_draw_ordinal_min" ]]; then
  env_args+=("DXMT9_PROBE_FORCE_TEXTURE_WHITE_DRAW_ORDINAL_MIN=$probe_force_texture_white_draw_ordinal_min")
fi

if [[ -n "$probe_force_texture_white_draw_ordinal_max" ]]; then
  env_args+=("DXMT9_PROBE_FORCE_TEXTURE_WHITE_DRAW_ORDINAL_MAX=$probe_force_texture_white_draw_ordinal_max")
fi

if [[ -n "$probe_force_texture_white_command_index" ]]; then
  env_args+=("DXMT9_PROBE_FORCE_TEXTURE_WHITE_COMMAND_INDEXES=$probe_force_texture_white_command_index")
fi

if [[ -n "$probe_force_texture_white_command_indexes" ]]; then
  env_args+=("DXMT9_PROBE_FORCE_TEXTURE_WHITE_COMMAND_INDEXES=$probe_force_texture_white_command_indexes")
fi

if [[ -n "$probe_force_texture_white_command_index_min" ]]; then
  env_args+=("DXMT9_PROBE_FORCE_TEXTURE_WHITE_COMMAND_INDEX_MIN=$probe_force_texture_white_command_index_min")
fi

if [[ -n "$probe_force_texture_white_command_index_max" ]]; then
  env_args+=("DXMT9_PROBE_FORCE_TEXTURE_WHITE_COMMAND_INDEX_MAX=$probe_force_texture_white_command_index_max")
fi

if [[ -n "$probe_force_texture_white_command_draw_index" ]]; then
  env_args+=("DXMT9_PROBE_FORCE_TEXTURE_WHITE_COMMAND_DRAW_INDEXES=$probe_force_texture_white_command_draw_index")
fi

if [[ -n "$probe_force_texture_white_command_draw_indexes" ]]; then
  env_args+=("DXMT9_PROBE_FORCE_TEXTURE_WHITE_COMMAND_DRAW_INDEXES=$probe_force_texture_white_command_draw_indexes")
fi

if [[ -n "$probe_force_texture_white_command_draw_index_min" ]]; then
  env_args+=("DXMT9_PROBE_FORCE_TEXTURE_WHITE_COMMAND_DRAW_INDEX_MIN=$probe_force_texture_white_command_draw_index_min")
fi

if [[ -n "$probe_force_texture_white_command_draw_index_max" ]]; then
  env_args+=("DXMT9_PROBE_FORCE_TEXTURE_WHITE_COMMAND_DRAW_INDEX_MAX=$probe_force_texture_white_command_draw_index_max")
fi

if (( probe_disable_alpha_blend )); then
  env_args+=("DXMT9_PROBE_DISABLE_ALPHA_BLEND=1")
fi

if [[ -n "$probe_disable_alpha_blend_row" ]]; then
  env_args+=("DXMT9_PROBE_DISABLE_ALPHA_BLEND_ROW=$probe_disable_alpha_blend_row")
fi

if [[ -n "$probe_disable_alpha_blend_rows" ]]; then
  env_args+=("DXMT9_PROBE_DISABLE_ALPHA_BLEND_ROWS=$probe_disable_alpha_blend_rows")
fi

if [[ -n "$probe_disable_alpha_blend_class" ]]; then
  env_args+=("DXMT9_PROBE_DISABLE_ALPHA_BLEND_CLASS=$probe_disable_alpha_blend_class")
fi

if [[ -n "$probe_disable_alpha_blend_classes" ]]; then
  env_args+=("DXMT9_PROBE_DISABLE_ALPHA_BLEND_CLASSES=$probe_disable_alpha_blend_classes")
fi

if [[ -n "$probe_disable_alpha_blend_texture0" ]]; then
  env_args+=("DXMT9_PROBE_DISABLE_ALPHA_BLEND_TEXTURE0=$probe_disable_alpha_blend_texture0")
fi

if [[ -n "$probe_disable_alpha_blend_texture0_width" ]]; then
  env_args+=("DXMT9_PROBE_DISABLE_ALPHA_BLEND_TEXTURE0_WIDTH=$probe_disable_alpha_blend_texture0_width")
fi

if [[ -n "$probe_disable_alpha_blend_texture0_height" ]]; then
  env_args+=("DXMT9_PROBE_DISABLE_ALPHA_BLEND_TEXTURE0_HEIGHT=$probe_disable_alpha_blend_texture0_height")
fi

if [[ -n "$probe_disable_alpha_blend_texture0_format" ]]; then
  env_args+=("DXMT9_PROBE_DISABLE_ALPHA_BLEND_TEXTURE0_FORMAT=$probe_disable_alpha_blend_texture0_format")
fi

if (( probe_disable_depth_write )); then
  env_args+=("DXMT9_PROBE_DISABLE_DEPTH_WRITE=1")
fi

if [[ -n "$probe_disable_depth_write_row" ]]; then
  env_args+=("DXMT9_PROBE_DISABLE_DEPTH_WRITE_ROW=$probe_disable_depth_write_row")
fi

if [[ -n "$probe_disable_depth_write_rows" ]]; then
  env_args+=("DXMT9_PROBE_DISABLE_DEPTH_WRITE_ROWS=$probe_disable_depth_write_rows")
fi

if [[ -n "$probe_disable_depth_write_class" ]]; then
  env_args+=("DXMT9_PROBE_DISABLE_DEPTH_WRITE_CLASS=$probe_disable_depth_write_class")
fi

if [[ -n "$probe_disable_depth_write_classes" ]]; then
  env_args+=("DXMT9_PROBE_DISABLE_DEPTH_WRITE_CLASSES=$probe_disable_depth_write_classes")
fi

if (( probe_depth_func_always )); then
  env_args+=("DXMT9_PROBE_DEPTH_FUNC_ALWAYS=1")
fi

if [[ -n "$probe_depth_func_always_row" ]]; then
  env_args+=("DXMT9_PROBE_DEPTH_FUNC_ALWAYS_ROW=$probe_depth_func_always_row")
fi

if [[ -n "$probe_depth_func_always_rows" ]]; then
  env_args+=("DXMT9_PROBE_DEPTH_FUNC_ALWAYS_ROWS=$probe_depth_func_always_rows")
fi

if [[ -n "$probe_depth_func_always_class" ]]; then
  env_args+=("DXMT9_PROBE_DEPTH_FUNC_ALWAYS_CLASS=$probe_depth_func_always_class")
fi

if [[ -n "$probe_depth_func_always_classes" ]]; then
  env_args+=("DXMT9_PROBE_DEPTH_FUNC_ALWAYS_CLASSES=$probe_depth_func_always_classes")
fi

if [[ -n "$probe_depth_func_always_texture0" ]]; then
  env_args+=("DXMT9_PROBE_DEPTH_FUNC_ALWAYS_TEXTURE0=$probe_depth_func_always_texture0")
fi

if [[ -n "$probe_depth_func_always_texture0_width" ]]; then
  env_args+=("DXMT9_PROBE_DEPTH_FUNC_ALWAYS_TEXTURE0_WIDTH=$probe_depth_func_always_texture0_width")
fi

if [[ -n "$probe_depth_func_always_texture0_height" ]]; then
  env_args+=("DXMT9_PROBE_DEPTH_FUNC_ALWAYS_TEXTURE0_HEIGHT=$probe_depth_func_always_texture0_height")
fi

if [[ -n "$probe_depth_func_always_texture0_format" ]]; then
  env_args+=("DXMT9_PROBE_DEPTH_FUNC_ALWAYS_TEXTURE0_FORMAT=$probe_depth_func_always_texture0_format")
fi

if (( probe_fragmentless_depth_only )); then
  env_args+=("DXMT9_PROBE_FRAGMENTLESS_DEPTH_ONLY=1")
fi

if [[ -n "$probe_fragmentless_depth_only_row" ]]; then
  env_args+=("DXMT9_PROBE_FRAGMENTLESS_DEPTH_ONLY_ROW=$probe_fragmentless_depth_only_row")
fi

if [[ -n "$probe_fragmentless_depth_only_rows" ]]; then
  env_args+=("DXMT9_PROBE_FRAGMENTLESS_DEPTH_ONLY_ROWS=$probe_fragmentless_depth_only_rows")
fi

if [[ -n "$probe_fragmentless_depth_only_class" ]]; then
  env_args+=("DXMT9_PROBE_FRAGMENTLESS_DEPTH_ONLY_CLASS=$probe_fragmentless_depth_only_class")
fi

if [[ -n "$probe_fragmentless_depth_only_classes" ]]; then
  env_args+=("DXMT9_PROBE_FRAGMENTLESS_DEPTH_ONLY_CLASSES=$probe_fragmentless_depth_only_classes")
fi
if (( probe_fragmentless_depth_only_keep_vsout )); then
  env_args+=("DXMT9_PROBE_FRAGMENTLESS_DEPTH_ONLY_KEEP_VSOUT=1")
fi

if (( force_visible )); then
  env_args+=("DXMT_DEBUG_FORCE_VISIBLE=1")
fi

if (( effect_draw_trace )); then
  env_args+=("DXMT9_EFFECT_DRAW_TRACE=1")
fi

if [[ -n "$effect_draw_trace_seq" ]]; then
  env_args+=("DXMT9_EFFECT_DRAW_TRACE_SEQ=$effect_draw_trace_seq")
fi

if [[ -n "$effect_draw_trace_seq_min" ]]; then
  env_args+=("DXMT9_EFFECT_DRAW_TRACE_SEQ_MIN=$effect_draw_trace_seq_min")
fi

if [[ -n "$effect_draw_trace_seq_max" ]]; then
  env_args+=("DXMT9_EFFECT_DRAW_TRACE_SEQ_MAX=$effect_draw_trace_seq_max")
fi

if [[ -n "$effect_draw_trace_enc" ]]; then
  env_args+=("DXMT9_EFFECT_DRAW_TRACE_ENC=$effect_draw_trace_enc")
fi

if [[ -n "$effect_draw_trace_texture0" ]]; then
  env_args+=("DXMT9_EFFECT_DRAW_TRACE_TEXTURE0=$effect_draw_trace_texture0")
fi

if [[ -n "$effect_draw_trace_texture0_width" ]]; then
  env_args+=("DXMT9_EFFECT_DRAW_TRACE_TEXTURE0_WIDTH=$effect_draw_trace_texture0_width")
fi

if [[ -n "$effect_draw_trace_texture0_height" ]]; then
  env_args+=("DXMT9_EFFECT_DRAW_TRACE_TEXTURE0_HEIGHT=$effect_draw_trace_texture0_height")
fi

if [[ -n "$effect_draw_trace_texture0_format" ]]; then
  env_args+=("DXMT9_EFFECT_DRAW_TRACE_TEXTURE0_FORMAT=$effect_draw_trace_texture0_format")
fi

if [[ -n "$effect_draw_trace_primitive_type" ]]; then
  env_args+=("DXMT9_EFFECT_DRAW_TRACE_PRIMITIVE_TYPE=$effect_draw_trace_primitive_type")
fi

if (( effect_draw_trace_point_sprite )); then
  env_args+=("DXMT9_EFFECT_DRAW_TRACE_POINT_SPRITE=1")
fi

if (( effect_draw_trace_include_non_alpha )); then
  env_args+=("DXMT9_EFFECT_DRAW_TRACE_INCLUDE_NON_ALPHA=1")
fi

if (( effect_draw_trace_include_untextured )); then
  env_args+=("DXMT9_EFFECT_DRAW_TRACE_INCLUDE_UNTEXTURED=1")
fi

if (( effect_draw_trace_geometry )); then
  env_args+=("DXMT9_EFFECT_DRAW_TRACE_GEOMETRY=1")
fi

if [[ -n "$effect_draw_trace_geometry_max_refs" ]]; then
  env_args+=("DXMT9_EFFECT_DRAW_TRACE_GEOMETRY_MAX_REFS=$effect_draw_trace_geometry_max_refs")
fi

if (( visibility_scout )); then
  if [[ -z "$visibility_scout_path" ]]; then
    visibility_scout_path="$visibility_scout_default_path"
  elif [[ "$visibility_scout_path" != /* ]]; then
    visibility_scout_path="$repo_root/$visibility_scout_path"
  fi
  if [[ -z "$visibility_scout_summary_output" ]]; then
    visibility_scout_summary_output="$visibility_scout_summary_default_path"
  elif [[ "$visibility_scout_summary_output" != /* ]]; then
    visibility_scout_summary_output="$repo_root/$visibility_scout_summary_output"
  fi
  if [[ -z "$visibility_scout_summary_csv_output" ]]; then
    visibility_scout_summary_csv_output="$visibility_scout_summary_csv_default_path"
  elif [[ "$visibility_scout_summary_csv_output" != /* ]]; then
    visibility_scout_summary_csv_output="$repo_root/$visibility_scout_summary_csv_output"
  fi
  env_args+=(
    "DXMT9_VISIBILITY_SCOUT=1"
    "DXMT9_VISIBILITY_SCOUT_PATH=$visibility_scout_path"
  )
fi

if [[ -n "$visibility_scout_row" ]]; then
  env_args+=("DXMT9_VISIBILITY_SCOUT_ROW=$visibility_scout_row")
fi

if [[ -n "$visibility_scout_rows" ]]; then
  env_args+=("DXMT9_VISIBILITY_SCOUT_ROWS=$visibility_scout_rows")
fi

if [[ -n "$capture_frames" ]]; then
  env_args+=("DXMT_CAPTURE_FRAMES=$capture_frames")
fi

if [[ -n "$capture_range" ]]; then
  env_args+=("DXMT_CAPTURE_RANGE=$capture_range")
fi

if [[ -n "$capture_dir" ]]; then
  env_args+=("DXMT_EXPERIMENT_CAPTURE_DIR=$capture_dir")
fi

if (( dump_shaders )); then
  env_args+=(
    "DXMT_DUMP_SHADER_DIR=$shader_msl_dump_dir"
    "DXMT_DUMP_SHADER_BYTECODE_DIR=$shader_bytecode_dump_dir"
  )
fi

cmd=(
  caffeinate -dimsu
  python3 scripts/run_apps/run_experiment.py
  run app-d3d9-3dmark05
  --output-suffix "$suffix"
  --timeout "$timeout"
)
if [[ -n "$capture_delay_sec" ]]; then
  cmd+=(--capture-delay-sec "$capture_delay_sec")
fi
if (( with_wine_capture_layer )); then
  cmd=(
    bash scripts/tools/run_with_wine_metal_capture_layer.sh
    --wine-root "$probe_wine_root"
    --allow-3dmark05
    --
    "${cmd[@]}"
  )
fi

counter_compare_cmd=()
if [[ -n "$compare_baseline_output" ]]; then
  counter_compare_cmd=(
    python3 scripts/tools/compare_3dmark05_perf_counters.py
    "$compare_baseline_output"
    "$output_dir"
    --before-label baseline
    --after-label "$suffix"
    --output "$counter_comparison_path"
  )
  if (( require_color_dontcare_increase )); then
    counter_compare_cmd+=(--require-color-dontcare-increase)
  fi
  if (( require_depth_dontcare_increase )); then
    counter_compare_cmd+=(--require-depth-dontcare-increase)
  fi
  if (( require_tile_preservation_decrease )); then
    counter_compare_cmd+=(--require-tile-preservation-decrease)
  fi
  if (( require_tile_preservation_not_increase )); then
    counter_compare_cmd+=(--require-tile-preservation-not-increase)
  fi
  if (( require_command_buffers_per_present_not_increase )); then
    counter_compare_cmd+=(--require-command-buffers-per-present-not-increase)
  fi
  if (( require_render_passes_per_present_not_increase )); then
    counter_compare_cmd+=(--require-render-passes-per-present-not-increase)
  fi
  if (( require_render_pass_carry_promotion_gates )); then
    counter_compare_cmd+=(--require-render-pass-carry-promotion-gates)
  fi
  if (( require_encoder_final_end_reason_not_increase )); then
    counter_compare_cmd+=(--require-encoder-final-end-reason-not-increase)
  fi
  if (( require_encoder_final_same_key_reopen_not_increase )); then
    counter_compare_cmd+=(--require-encoder-final-same-key-reopen-not-increase)
  fi
  if (( require_encoder_color_load_not_increase )); then
    counter_compare_cmd+=(--require-encoder-color-load-not-increase)
  fi
  if (( require_encoder_depth_load_not_increase )); then
    counter_compare_cmd+=(--require-encoder-depth-load-not-increase)
  fi
  if (( require_draw_run_records_increase )); then
    counter_compare_cmd+=(--require-draw-run-records-increase)
  fi
  if (( require_draw_run_records_per_submit_increase )); then
    counter_compare_cmd+=(--require-draw-run-records-per-submit-increase)
  fi
  if (( require_binding_overrides_present )); then
    counter_compare_cmd+=(--require-binding-overrides-present)
  fi
  if (( require_const_upload_passthrough_present )); then
    counter_compare_cmd+=(--require-const-upload-passthrough-present)
  fi
  if (( require_draw_submission_batch_present )); then
    counter_compare_cmd+=(--require-draw-submission-batch-present)
  fi
  if (( require_const_upload_break_bytes_decrease )); then
    counter_compare_cmd+=(--require-const-upload-break-bytes-decrease)
  fi
  if (( require_encode_draw_cpu_decrease )); then
    counter_compare_cmd+=(--require-encode-draw-cpu-decrease)
  fi
  if (( require_completion_present_wait_decrease )); then
    counter_compare_cmd+=(--require-completion-present-wait-decrease)
  fi
  if (( require_completion_wait_with_enqueue_increase )); then
    counter_compare_cmd+=(--require-completion-wait-with-enqueue-increase)
  fi
  if (( require_completion_wait_without_enqueue_decrease )); then
    counter_compare_cmd+=(--require-completion-wait-without-enqueue-decrease)
  fi
  if (( require_completion_present_wait_with_enqueue_increase )); then
    counter_compare_cmd+=(--require-completion-present-wait-with-enqueue-increase)
  fi
  if (( require_completion_present_wait_without_enqueue_decrease )); then
    counter_compare_cmd+=(--require-completion-present-wait-without-enqueue-decrease)
  fi
  if (( require_encode_ready_depth_gt1_increase )); then
    counter_compare_cmd+=(--require-encode-ready-depth-gt1-increase)
  fi
  if (( require_commit_chunk_replay_cpu_per_present_decrease )); then
    counter_compare_cmd+=(--require-commit-chunk-replay-cpu-per-present-decrease)
  fi
  if (( require_queue_draw_submission_cpu_per_present_decrease )); then
    counter_compare_cmd+=(--require-queue-draw-submission-cpu-per-present-decrease)
  fi
  if (( require_snapshot_cpu_per_present_decrease )); then
    counter_compare_cmd+=(--require-snapshot-cpu-per-present-decrease)
  fi
  if (( require_snapshot_cache_lookup_cpu_per_present_decrease )); then
    counter_compare_cmd+=(--require-snapshot-cache-lookup-cpu-per-present-decrease)
  fi
  if (( require_snapshot_cache_uniform_build_cpu_per_present_decrease )); then
    counter_compare_cmd+=(--require-snapshot-cache-uniform-build-cpu-per-present-decrease)
  fi
  if (( require_snapshot_cache_uniform_hash_cpu_per_present_decrease )); then
    counter_compare_cmd+=(--require-snapshot-cache-uniform-hash-cpu-per-present-decrease)
  fi
  if (( require_batch_miss_uniform_build_cpu_per_present_decrease )); then
    counter_compare_cmd+=(--require-batch-miss-uniform-build-cpu-per-present-decrease)
  fi
  if (( require_batch_miss_uniform_hash_cpu_per_present_decrease )); then
    counter_compare_cmd+=(--require-batch-miss-uniform-hash-cpu-per-present-decrease)
  fi
  if (( require_batch_miss_vs_const_hash_cpu_per_present_decrease )); then
    counter_compare_cmd+=(--require-batch-miss-vs-const-hash-cpu-per-present-decrease)
  fi
  if (( require_batch_miss_ps_const_hash_cpu_per_present_decrease )); then
    counter_compare_cmd+=(--require-batch-miss-ps-const-hash-cpu-per-present-decrease)
  fi
  if (( require_batch_miss_nonconst_hash_cpu_per_present_decrease )); then
    counter_compare_cmd+=(--require-batch-miss-nonconst-hash-cpu-per-present-decrease)
  fi
  if (( require_snapshot_uniform_copy_cpu_per_present_decrease )); then
    counter_compare_cmd+=(--require-snapshot-uniform-copy-cpu-per-present-decrease)
  fi
  if (( require_submit_draw_run_batch_append_uniform_cpu_per_present_decrease )); then
    counter_compare_cmd+=(--require-submit-draw-run-batch-append-uniform-cpu-per-present-decrease)
  fi
  if (( require_draw_uniform_payload_lookup_cpu_per_present_decrease )); then
    counter_compare_cmd+=(--require-draw-uniform-payload-lookup-cpu-per-present-decrease)
  fi
  if (( require_draw_uniform_payload_append_copy_cpu_per_present_decrease )); then
    counter_compare_cmd+=(--require-draw-uniform-payload-append-copy-cpu-per-present-decrease)
  fi
  if (( require_argbuf_setup_cpu_per_present_decrease )); then
    counter_compare_cmd+=(--require-argbuf-setup-cpu-per-present-decrease)
  fi
  if (( require_argbuf_open_cpu_per_present_decrease )); then
    counter_compare_cmd+=(--require-argbuf-open-cpu-per-present-decrease)
  fi
  if (( require_argbuf_cbuf_update_cpu_per_present_decrease )); then
    counter_compare_cmd+=(--require-argbuf-cbuf-update-cpu-per-present-decrease)
  fi
  if (( require_argbuf_cbuf_update_vs_cpu_per_present_decrease )); then
    counter_compare_cmd+=(--require-argbuf-cbuf-update-vs-cpu-per-present-decrease)
  fi
  if (( require_uniform_compact_saved_bytes_present )); then
    counter_compare_cmd+=(--require-uniform-compact-saved-bytes-present)
  fi
  if (( require_snapshot_state_elided_present )); then
    counter_compare_cmd+=(--require-snapshot-state-elided-present)
  fi
  if (( require_discarded_state_not_increase )); then
    counter_compare_cmd+=(--require-discarded-state-not-increase)
  fi
  if (( require_submission_carrier_bytes_per_record_decrease )); then
    counter_compare_cmd+=(--require-submission-carrier-bytes-per-record-decrease)
  fi
  if (( require_submission_carrier_uniform_storage_per_record_decrease )); then
    counter_compare_cmd+=(--require-submission-carrier-uniform-storage-per-record-decrease)
  fi
  if (( require_encode_chunk_cpu_per_present_decrease )); then
    counter_compare_cmd+=(--require-encode-chunk-cpu-per-present-decrease)
  fi
  if (( require_no_enqueue_commit_entry_to_publish_decrease )); then
    counter_compare_cmd+=(--require-no-enqueue-commit-entry-to-publish-decrease)
  fi
  if (( require_no_enqueue_publish_to_encode_dequeue_decrease )); then
    counter_compare_cmd+=(--require-no-enqueue-publish-to-encode-dequeue-decrease)
  fi
  if (( require_no_enqueue_encode_dequeue_to_commit_decrease )); then
    counter_compare_cmd+=(--require-no-enqueue-encode-dequeue-to-commit-decrease)
  fi
  if (( require_no_enqueue_wait_to_next_enqueue_decrease )); then
    counter_compare_cmd+=(--require-no-enqueue-wait-to-next-enqueue-decrease)
  fi
  if (( require_no_enqueue_before_publish_closure_decrease )); then
    counter_compare_cmd+=(--require-no-enqueue-before-publish-closure-decrease)
  fi
  if (( require_no_enqueue_before_publish_inter_replay_gap_decrease )); then
    counter_compare_cmd+=(--require-no-enqueue-before-publish-inter-replay-gap-decrease)
  fi
  if (( require_pe_focused_between_call_gap_residual_decrease )); then
    counter_compare_cmd+=(--require-pe-focused-between-call-gap-residual-decrease)
  fi
  if [[ -n "$max_gpu_command_buffer_regression_ms" ]]; then
    counter_compare_cmd+=(
      --max-gpu-command-buffer-regression-ms
      "$max_gpu_command_buffer_regression_ms"
    )
  fi
  if [[ -n "$max_const_upload_break_count_ratio" ]]; then
    counter_compare_cmd+=(
      --max-const-upload-break-count-ratio
      "$max_const_upload_break_count_ratio"
    )
  fi
fi

finalize_cmd=()
if (( capture_gputrace )); then
  finalize_cmd=(
    scripts/tools/finalize_3dmark05_perf_probe.sh
    --suffix "$suffix"
    --frame "$frame"
    --top "$top_n"
    --hot-gpu-share "$hot_gpu_share"
  )
  if [[ -n "$compare_baseline_output" ]]; then
    finalize_cmd+=(--baseline-output "$compare_baseline_output")
  fi
  if [[ -n "$compare_baseline_joined" ]]; then
    finalize_cmd+=(--baseline-joined "$compare_baseline_joined")
  fi
  if (( semantic_image_compare_requested )); then
    finalize_cmd+=(
      --semantic-image-policy "$semantic_image_policy"
      --semantic-image-before "$semantic_image_before"
      --semantic-image-after "$semantic_image_after"
      --semantic-image-min-active-pct "$semantic_image_min_active_pct"
    )
    if [[ -n "$semantic_image_output" ]]; then
      finalize_cmd+=(--semantic-image-output "$semantic_image_output")
    fi
    if [[ -n "$semantic_image_summary_output" ]]; then
      finalize_cmd+=(--semantic-image-summary-output "$semantic_image_summary_output")
    fi
    if [[ -n "$semantic_image_diff_output" ]]; then
      finalize_cmd+=(--semantic-image-diff-output "$semantic_image_diff_output")
    fi
  fi
  if (( require_result_json )); then
    finalize_cmd+=(--require-result-json)
  fi
  if (( allow_partial_stable_frame_proof )); then
    finalize_cmd+=(--allow-partial-stable-frame-proof)
  fi
  if (( require_top_gpu_decrease )); then
    finalize_cmd+=(--require-top-gpu-decrease)
  fi
  if (( require_top_buffer_write_decrease )); then
    finalize_cmd+=(--require-top-buffer-write-decrease)
  fi
  if (( require_top_vs_buffer_write_decrease )); then
    finalize_cmd+=(--require-top-vs-buffer-write-decrease)
  fi
  if (( require_top_unexplained_buffer_write_decrease )); then
    finalize_cmd+=(--require-top-unexplained-buffer-write-decrease)
  fi
  if (( require_stream_handle_churn_decrease )); then
    finalize_cmd+=(--require-stream-handle-churn-decrease)
  fi
  if (( require_ib_handle_churn_decrease )); then
    finalize_cmd+=(--require-ib-handle-churn-decrease)
  fi
  if (( require_argbuf_cbuf_decrease )); then
    finalize_cmd+=(--require-argbuf-cbuf-decrease)
  fi
  if (( require_transient_decrease )); then
    finalize_cmd+=(--require-transient-decrease)
  fi
  if (( require_top_gpu_share_increase )); then
    finalize_cmd+=(--require-top-gpu-share-increase)
  fi
  if (( require_top_row_key_match )); then
    finalize_cmd+=(--require-top-row-key-match)
  fi
  if (( require_stable_frame_proof )); then
    finalize_cmd+=(--require-stable-frame-proof)
  fi
  if (( require_tvb_mechanism_proof )); then
    finalize_cmd+=(--require-tvb-mechanism-proof)
  fi
  if (( require_opaque_depth_index_cache_proof )); then
    finalize_cmd+=(--require-opaque-depth-index-cache-proof)
  fi
  if (( require_screen_blend_cache_proof )); then
    finalize_cmd+=(--require-screen-blend-cache-proof)
  fi
  if (( require_semantic_image_proof )); then
    finalize_cmd+=(--require-semantic-image-proof)
  fi
  if (( require_color_dontcare_increase )); then
    finalize_cmd+=(--require-color-dontcare-increase)
  fi
  if (( require_depth_dontcare_increase )); then
    finalize_cmd+=(--require-depth-dontcare-increase)
  fi
  if (( require_tile_preservation_decrease )); then
    finalize_cmd+=(--require-tile-preservation-decrease)
  fi
  if (( require_tile_preservation_not_increase )); then
    finalize_cmd+=(--require-tile-preservation-not-increase)
  fi
  if (( require_command_buffers_per_present_not_increase )); then
    finalize_cmd+=(--require-command-buffers-per-present-not-increase)
  fi
  if (( require_render_passes_per_present_not_increase )); then
    finalize_cmd+=(--require-render-passes-per-present-not-increase)
  fi
  if (( require_render_pass_carry_promotion_gates )); then
    finalize_cmd+=(--require-render-pass-carry-promotion-gates)
  fi
  if (( require_encoder_final_end_reason_not_increase )); then
    finalize_cmd+=(--require-encoder-final-end-reason-not-increase)
  fi
  if (( require_encoder_final_same_key_reopen_not_increase )); then
    finalize_cmd+=(--require-encoder-final-same-key-reopen-not-increase)
  fi
  if (( require_encoder_color_load_not_increase )); then
    finalize_cmd+=(--require-encoder-color-load-not-increase)
  fi
  if (( require_encoder_depth_load_not_increase )); then
    finalize_cmd+=(--require-encoder-depth-load-not-increase)
  fi
  if (( require_draw_run_records_increase )); then
    finalize_cmd+=(--require-draw-run-records-increase)
  fi
  if (( require_draw_run_records_per_submit_increase )); then
    finalize_cmd+=(--require-draw-run-records-per-submit-increase)
  fi
  if (( require_binding_overrides_present )); then
    finalize_cmd+=(--require-binding-overrides-present)
  fi
  if (( require_const_upload_passthrough_present )); then
    finalize_cmd+=(--require-const-upload-passthrough-present)
  fi
  if (( require_draw_submission_batch_present )); then
    finalize_cmd+=(--require-draw-submission-batch-present)
  fi
  if (( require_const_upload_break_bytes_decrease )); then
    finalize_cmd+=(--require-const-upload-break-bytes-decrease)
  fi
  if (( require_encode_draw_cpu_decrease )); then
    finalize_cmd+=(--require-encode-draw-cpu-decrease)
  fi
  if (( require_completion_present_wait_decrease )); then
    finalize_cmd+=(--require-completion-present-wait-decrease)
  fi
  if (( require_completion_wait_with_enqueue_increase )); then
    finalize_cmd+=(--require-completion-wait-with-enqueue-increase)
  fi
  if (( require_completion_wait_without_enqueue_decrease )); then
    finalize_cmd+=(--require-completion-wait-without-enqueue-decrease)
  fi
  if (( require_completion_present_wait_with_enqueue_increase )); then
    finalize_cmd+=(--require-completion-present-wait-with-enqueue-increase)
  fi
  if (( require_completion_present_wait_without_enqueue_decrease )); then
    finalize_cmd+=(--require-completion-present-wait-without-enqueue-decrease)
  fi
  if (( require_encode_ready_depth_gt1_increase )); then
    finalize_cmd+=(--require-encode-ready-depth-gt1-increase)
  fi
  if (( require_commit_chunk_replay_cpu_per_present_decrease )); then
    finalize_cmd+=(--require-commit-chunk-replay-cpu-per-present-decrease)
  fi
  if (( require_queue_draw_submission_cpu_per_present_decrease )); then
    finalize_cmd+=(--require-queue-draw-submission-cpu-per-present-decrease)
  fi
  if (( require_snapshot_cpu_per_present_decrease )); then
    finalize_cmd+=(--require-snapshot-cpu-per-present-decrease)
  fi
  if (( require_snapshot_cache_lookup_cpu_per_present_decrease )); then
    finalize_cmd+=(--require-snapshot-cache-lookup-cpu-per-present-decrease)
  fi
  if (( require_snapshot_cache_uniform_build_cpu_per_present_decrease )); then
    finalize_cmd+=(--require-snapshot-cache-uniform-build-cpu-per-present-decrease)
  fi
  if (( require_snapshot_cache_uniform_hash_cpu_per_present_decrease )); then
    finalize_cmd+=(--require-snapshot-cache-uniform-hash-cpu-per-present-decrease)
  fi
  if (( require_batch_miss_uniform_build_cpu_per_present_decrease )); then
    finalize_cmd+=(--require-batch-miss-uniform-build-cpu-per-present-decrease)
  fi
  if (( require_batch_miss_uniform_hash_cpu_per_present_decrease )); then
    finalize_cmd+=(--require-batch-miss-uniform-hash-cpu-per-present-decrease)
  fi
  if (( require_batch_miss_vs_const_hash_cpu_per_present_decrease )); then
    finalize_cmd+=(--require-batch-miss-vs-const-hash-cpu-per-present-decrease)
  fi
  if (( require_batch_miss_ps_const_hash_cpu_per_present_decrease )); then
    finalize_cmd+=(--require-batch-miss-ps-const-hash-cpu-per-present-decrease)
  fi
  if (( require_batch_miss_nonconst_hash_cpu_per_present_decrease )); then
    finalize_cmd+=(--require-batch-miss-nonconst-hash-cpu-per-present-decrease)
  fi
  if (( require_snapshot_uniform_copy_cpu_per_present_decrease )); then
    finalize_cmd+=(--require-snapshot-uniform-copy-cpu-per-present-decrease)
  fi
  if (( require_submit_draw_run_batch_append_uniform_cpu_per_present_decrease )); then
    finalize_cmd+=(--require-submit-draw-run-batch-append-uniform-cpu-per-present-decrease)
  fi
  if (( require_draw_uniform_payload_lookup_cpu_per_present_decrease )); then
    finalize_cmd+=(--require-draw-uniform-payload-lookup-cpu-per-present-decrease)
  fi
  if (( require_draw_uniform_payload_append_copy_cpu_per_present_decrease )); then
    finalize_cmd+=(--require-draw-uniform-payload-append-copy-cpu-per-present-decrease)
  fi
  if (( require_argbuf_setup_cpu_per_present_decrease )); then
    finalize_cmd+=(--require-argbuf-setup-cpu-per-present-decrease)
  fi
  if (( require_argbuf_open_cpu_per_present_decrease )); then
    finalize_cmd+=(--require-argbuf-open-cpu-per-present-decrease)
  fi
  if (( require_argbuf_cbuf_update_cpu_per_present_decrease )); then
    finalize_cmd+=(--require-argbuf-cbuf-update-cpu-per-present-decrease)
  fi
  if (( require_argbuf_cbuf_update_vs_cpu_per_present_decrease )); then
    finalize_cmd+=(--require-argbuf-cbuf-update-vs-cpu-per-present-decrease)
  fi
  if (( require_uniform_compact_saved_bytes_present )); then
    finalize_cmd+=(--require-uniform-compact-saved-bytes-present)
  fi
  if (( require_current_uniform_compact_saved_bytes_present )); then
    finalize_cmd+=(--require-current-uniform-compact-saved-bytes-present)
  fi
  if (( require_snapshot_state_elided_present )); then
    finalize_cmd+=(--require-snapshot-state-elided-present)
  fi
  if (( require_discarded_state_not_increase )); then
    finalize_cmd+=(--require-discarded-state-not-increase)
  fi
  if (( require_submission_carrier_bytes_per_record_decrease )); then
    finalize_cmd+=(--require-submission-carrier-bytes-per-record-decrease)
  fi
  if (( require_submission_carrier_uniform_storage_per_record_decrease )); then
    finalize_cmd+=(--require-submission-carrier-uniform-storage-per-record-decrease)
  fi
  if (( require_encode_chunk_cpu_per_present_decrease )); then
    finalize_cmd+=(--require-encode-chunk-cpu-per-present-decrease)
  fi
  if (( require_no_enqueue_commit_entry_to_publish_decrease )); then
    finalize_cmd+=(--require-no-enqueue-commit-entry-to-publish-decrease)
  fi
  if (( require_no_enqueue_publish_to_encode_dequeue_decrease )); then
    finalize_cmd+=(--require-no-enqueue-publish-to-encode-dequeue-decrease)
  fi
  if (( require_no_enqueue_encode_dequeue_to_commit_decrease )); then
    finalize_cmd+=(--require-no-enqueue-encode-dequeue-to-commit-decrease)
  fi
  if (( require_no_enqueue_wait_to_next_enqueue_decrease )); then
    finalize_cmd+=(--require-no-enqueue-wait-to-next-enqueue-decrease)
  fi
  if (( require_no_enqueue_before_publish_closure_decrease )); then
    finalize_cmd+=(--require-no-enqueue-before-publish-closure-decrease)
  fi
  if (( require_no_enqueue_before_publish_inter_replay_gap_decrease )); then
    finalize_cmd+=(--require-no-enqueue-before-publish-inter-replay-gap-decrease)
  fi
  if (( require_pe_focused_between_call_gap_residual_decrease )); then
    finalize_cmd+=(--require-pe-focused-between-call-gap-residual-decrease)
  fi
  if [[ -n "$max_gpu_command_buffer_regression_ms" ]]; then
    finalize_cmd+=(
      --max-gpu-command-buffer-regression-ms
      "$max_gpu_command_buffer_regression_ms"
    )
  fi
  if [[ -n "$max_const_upload_break_count_ratio" ]]; then
    finalize_cmd+=(
      --max-const-upload-break-count-ratio
      "$max_const_upload_break_count_ratio"
    )
  fi
  if (( require_top_pso_attribution )); then
    finalize_cmd+=(
      --require-top-pso-attribution
      --min-top-pso-samples-per-draw "$min_top_pso_samples_per_draw"
    )
  fi
  if (( require_xcode_counter_coverage )); then
    finalize_cmd+=(--require-xcode-counter-coverage)
  fi
  if (( require_dxmt_join_coverage )); then
    finalize_cmd+=(
      --require-dxmt-join-coverage
      --min-top-dxmt-joined-fraction "$min_top_dxmt_joined_fraction"
    )
  fi
  if (( require_shader_dump_matches )); then
    finalize_cmd+=(--require-shader-dump-matches)
  fi
  if [[ -n "$max_top_gpu_regression_ms" ]]; then
    finalize_cmd+=(--max-top-gpu-regression-ms "$max_top_gpu_regression_ms")
  fi
  if [[ -n "$max_top_buffer_write_regression_mib" ]]; then
    finalize_cmd+=(
      --max-top-buffer-write-regression-mib
      "$max_top_buffer_write_regression_mib"
    )
  fi
  for row_key in "${target_row_keys[@]}"; do
    finalize_cmd+=(--target-row-key "$row_key")
  done
  if (( require_target_index_cache_miss32_decrease )); then
    finalize_cmd+=(--require-target-index-cache-miss32-decrease)
  fi
  if (( require_target_index_cache_opt_miss32_decrease )); then
    finalize_cmd+=(--require-target-index-cache-opt-miss32-decrease)
  fi
  if (( require_target_reordered_index_cache_hits )); then
    finalize_cmd+=(--require-target-reordered-index-cache-hits)
  fi
  if (( require_target_vs_buffer_write_decrease )); then
    finalize_cmd+=(--require-target-vs-buffer-write-decrease)
  fi
  if (( require_target_vs_invocations_decrease )); then
    finalize_cmd+=(--require-target-vs-invocations-decrease)
  fi
  if [[ -n "$max_non_target_gpu_regression_ms" ]]; then
    finalize_cmd+=(
      --max-non-target-gpu-regression-ms
      "$max_non_target_gpu_regression_ms"
    )
  fi
  if [[ -n "$max_non_target_vs_buffer_write_regression_mib" ]]; then
    finalize_cmd+=(
      --max-non-target-vs-buffer-write-regression-mib
      "$max_non_target_vs_buffer_write_regression_mib"
    )
  fi
  if [[ -n "$max_non_target_vs_invocations_regression_ratio" ]]; then
    finalize_cmd+=(
      --max-non-target-vs-invocations-regression-ratio
      "$max_non_target_vs_invocations_regression_ratio"
    )
  fi
  if [[ -n "$max_non_target_draw_call_delta_ratio" ]]; then
    finalize_cmd+=(
      --max-non-target-draw-call-delta-ratio
      "$max_non_target_draw_call_delta_ratio"
    )
  fi
  if [[ -n "$max_non_target_vertex_count_delta_ratio" ]]; then
    finalize_cmd+=(
      --max-non-target-vertex-count-delta-ratio
      "$max_non_target_vertex_count_delta_ratio"
    )
  fi
  if [[ -n "$max_non_target_triangle_delta_ratio" ]]; then
    finalize_cmd+=(
      --max-non-target-triangle-delta-ratio
      "$max_non_target_triangle_delta_ratio"
    )
  fi
  if [[ -n "$max_top_unexplained_buffer_write_ratio" ]]; then
    finalize_cmd+=(
      --max-top-unexplained-buffer-write-ratio
      "$max_top_unexplained_buffer_write_ratio"
    )
  fi
  if [[ -n "$max_top_draw_call_delta_ratio" ]]; then
    finalize_cmd+=(
      --max-top-draw-call-delta-ratio
      "$max_top_draw_call_delta_ratio"
    )
  fi
  if [[ -n "$max_top_vertex_count_delta_ratio" ]]; then
    finalize_cmd+=(
      --max-top-vertex-count-delta-ratio
      "$max_top_vertex_count_delta_ratio"
    )
  fi
  if [[ -n "$max_top_triangle_delta_ratio" ]]; then
    finalize_cmd+=(
      --max-top-triangle-delta-ratio
      "$max_top_triangle_delta_ratio"
    )
  fi
fi

xctrace_export_cmd=(
  xcrun xctrace export
  --input "$metal_system_trace"
  --output "$metal_gpu_intervals_xml"
  --xpath '/trace-toc/run[@number="1"]/data/table[@schema="metal-gpu-intervals"]'
)
xctrace_summary_cmd=(
  python3 scripts/tools/summarize_xctrace_metal_intervals.py
  --gpu-intervals "$metal_gpu_intervals_xml"
  --dxmt-encoders "$encoders_csv"
  --indexed-probe-draws "$probe_draws_csv"
  --output-csv "$xctrace_gpu_intervals_summary_csv"
  --output-md "$xctrace_gpu_intervals_summary_md"
  --run-label "$run_id"
  --trace "$metal_system_trace"
  --top "$top_n"
  --require-xctrace-render-rows
  --min-dxmt-join-coverage 0.99
  --require-route-verdicts
)
if (( measure_index_reuse )); then
  xctrace_summary_cmd+=(--require-indexed-probe-routes)
fi

echo "run_id: $run_id"
echo "output_dir: $output_dir"
echo "trace_dir: $trace_dir"
echo "summary: $summary_path"
echo "index_cache_runtime_report: $index_cache_runtime_report"
echo "indexed_probe_draws: $probe_draws_csv"
echo "trace_artifacts_json: $trace_artifacts_json"
echo "metal_system_trace: $metal_system_trace"
echo "metal_gpu_intervals_xml: $metal_gpu_intervals_xml"
echo "xctrace_gpu_intervals_summary: $xctrace_gpu_intervals_summary_md"
echo "measure_index_reuse: $measure_index_reuse"
echo "session_locked: $session_locked"
echo "wait_unlocked_sec: $wait_unlocked_sec"
echo "wait_unlocked_interval_sec: $wait_unlocked_interval_sec"
echo "keep_frontmost: $keep_frontmost"
echo "keep_frontmost_process: $keep_frontmost_process"
echo "keep_frontmost_interval_sec: $keep_frontmost_interval_sec"
echo "require_current_uniform_compact_saved_bytes_present: $require_current_uniform_compact_saved_bytes_present"
echo "free_space_mb: $free_mb"
echo "min_free_space_mb: $min_free_mb"
echo "runner_timeout_sec: $timeout"
echo "watchdog_timeout_sec: ${timeout}+${effective_capture_delay_sec}+${timeout_slack}"
if (( with_wine_capture_layer )); then
  echo "wine_capture_layer_wrapper: enabled wine_root=$probe_wine_root"
else
  echo "wine_capture_layer_wrapper: disabled"
fi
if [[ -n "$capture_delay_sec" ]]; then
  echo "capture_delay_sec: $capture_delay_sec"
else
  echo "capture_delay_sec: ${effective_capture_delay_sec} (catalogue default)"
fi
if [[ -n "$capture_frames" ]]; then
  echo "capture_frames: $capture_frames"
fi
if [[ -n "$capture_range" ]]; then
  echo "capture_range: $capture_range"
fi
if [[ -n "$capture_dir" ]]; then
  echo "capture_dir: $capture_dir"
fi
if (( capture_gputrace )) && (( min_free_mb < recommended_gputrace_min_free_mb )); then
  echo "warning: gputrace min_free_space_mb is below the recommended ${recommended_gputrace_min_free_mb}MiB launch guard; set DXMT_3DMARK05_ALLOW_LOW_TRACE_FREE_MB=1 only for deliberate partial-run risk."
fi
if (( capture_gputrace )); then
  echo "metal_capture_destination: ${metal_capture_destination:-gpuTraceDocument}"
  if capture_destination_is_developer_tools "$metal_capture_destination"; then
    echo "gputrace: developerTools (Xcode export required; no direct file expected at $capture_path)"
    echo "xcode_developer_tools_capture_preflight: attach Xcode to the real Wine child before frame $frame; do not run if attach-by-PID is disabled or Attach to Process is stuck at Getting Process List"
    echo "xcode_developer_tools_capture_target: choose a frame known to be reached; missing frame $frame means no capture start/stop log is expected"
    echo "xcode_developer_tools_capture_preflight_required: $require_xcode_attach_preflight"
  else
    echo "gputrace: $capture_path"
  fi
else
  echo "gputrace: disabled"
fi
if (( dump_shaders )); then
  echo "shader_dump_dir: $shader_dump_dir"
fi
if (( dump_indexed_geometry )); then
  echo "geometry_dump_dir: $geometry_dump_dir"
fi
if (( dump_framegraph_dag )); then
  echo "framegraph_dag_dir: $framegraph_dag_dir"
  echo "framegraph_dag_frame: $framegraph_dag_frame"
  echo "framegraph_dag_frame_radius: $framegraph_dag_frame_radius"
  echo "framegraph_dag_formats: $framegraph_dag_formats"
  if [[ -n "$framegraph_dag_optimize" ]]; then
    echo "framegraph_dag_optimize: $framegraph_dag_optimize"
  fi
  if (( framegraph_dag_draws )); then
    echo "framegraph_dag_draws: 1"
  fi
  echo "framegraph_dag_summary: $framegraph_dag_summary"
  echo "framegraph_dag_summary_csv: $framegraph_dag_summary_csv"
  echo "framegraph_dag_candidates_csv: $framegraph_dag_candidates_csv"
  echo "framegraph_dag_preopt_summary: $framegraph_dag_preopt_summary"
  echo "framegraph_dag_preopt_summary_csv: $framegraph_dag_preopt_summary_csv"
  echo "framegraph_dag_preopt_candidates_csv: $framegraph_dag_preopt_candidates_csv"
  echo "framegraph_dag_postopt_summary: $framegraph_dag_postopt_summary"
  echo "framegraph_dag_postopt_summary_csv: $framegraph_dag_postopt_summary_csv"
  echo "framegraph_dag_postopt_candidates_csv: $framegraph_dag_postopt_candidates_csv"
fi
if (( visibility_scout )); then
  echo "visibility_scout_csv: $visibility_scout_path"
  echo "visibility_scout_summary: $visibility_scout_summary_output"
  echo "visibility_scout_summary_csv: $visibility_scout_summary_csv_output"
  if [[ -n "$visibility_scout_draw_indices" ]]; then
    echo "visibility_scout_draw_indices: $visibility_scout_draw_indices"
  fi
fi
if [[ -n "$dump_depth_attachment_handle" ]]; then
  echo "depth_attachment_dump: $dump_depth_attachment_path"
fi
if [[ -n "$dump_color_attachment_handle$dump_color_attachment_index$dump_color_attachment_seq$dump_color_attachment_enc$dump_color_attachment_draw$dump_color_attachment_draws$dump_color_attachment_command_index$dump_color_attachment_command_index_min$dump_color_attachment_command_index_max$dump_color_attachment_texture0$dump_color_attachment_texture0s$dump_color_attachment_path$dump_color_attachment_dir$dump_color_attachment_roi_summary_path" ]]; then
  if [[ -n "$dump_color_attachment_path" ]]; then
    echo "color_attachment_dump: $dump_color_attachment_path"
  fi
  if [[ -n "$dump_color_attachment_dir" ]]; then
    echo "color_attachment_dump_dir: $dump_color_attachment_dir"
  fi
  if [[ -n "$dump_color_attachment_roi_summary_path" ]]; then
    echo "color_attachment_roi_summary: $dump_color_attachment_roi_summary_path"
  fi
fi
if [[ -n "$dump_draw_texture_handles$dump_draw_texture0_width$dump_draw_texture0_height$dump_draw_texture0_format" ||
      "$dump_draw_texture0_any" -eq 1 ]]; then
  echo "draw_texture_dump_dir: $dump_draw_texture_dir"
fi
printf 'env:'
printf ' %q' "${env_args[@]}"
printf '\n'
printf 'cmd:'
printf ' %q' "${cmd[@]}"
printf '\n'
if ((${#counter_compare_cmd[@]})); then
  printf 'counter_compare_cmd:'
  printf ' %q' "${counter_compare_cmd[@]}"
  printf '\n'
fi
if ((${#finalize_cmd[@]})); then
  printf 'finalize_cmd_after_xcode_export:'
  printf ' %q' "${finalize_cmd[@]}"
  printf '\n'
fi
printf 'xctrace_system_trace_export_cmd:'
printf ' %q' "${xctrace_export_cmd[@]}"
printf '\n'
printf 'xctrace_system_trace_summary_cmd:'
printf ' %q' "${xctrace_summary_cmd[@]}"
printf '\n'
if (( probe_disable_alpha_blend )); then
  echo "warning: --probe-disable-alpha-blend is diagnostic only and can corrupt frame output (for example solid yellow/clear-like GT1 output); do not treat it as correctness-preserving."
fi
if (( disable_alpha_test )); then
  echo "warning: --disable-alpha-test is diagnostic only and can corrupt alpha-tested geometry; use it only to isolate discard/raster backend effects."
fi
if (( disable_fog )); then
  echo "warning: --disable-fog is diagnostic only and can corrupt fogged geometry; use it only to isolate fog/raster backend effects."
fi
if (( force_texture_white )); then
  echo "warning: --force-texture-white is diagnostic only and corrupts textured output; use it only to isolate texture-source backend effects."
fi
if (( probe_force_texture_white )) || [[ -n "$probe_force_texture_white_row$probe_force_texture_white_rows$probe_force_texture_white_class$probe_force_texture_white_classes" ]]; then
  echo "warning: --probe-force-texture-white is diagnostic only and corrupts selected textured output; use it only to isolate scoped texture-source backend effects."
fi
if (( probe_disable_depth_write )); then
  echo "warning: --probe-disable-depth-write is diagnostic only and can corrupt depth-dependent frame output; do not treat it as correctness-preserving."
fi
if (( probe_depth_func_always )); then
  echo "warning: --probe-depth-func-always is diagnostic only and can corrupt depth-dependent frame output; use it only to isolate depth-compare backend effects."
fi
if (( probe_fragmentless_depth_only )); then
  echo "warning: --probe-fragmentless-depth-only is diagnostic only; it is scoped to depth-only draws and still needs image/counter equality before any production route."
fi
if (( probe_fragmentless_depth_only_keep_vsout )); then
  echo "warning: --probe-fragmentless-depth-only-keep-vsout isolates fragmentless routing from position-only VSOut; it is still diagnostic and needs equality before Xcode counters."
fi
if (( force_expand_indexed )); then
  echo "warning: --force-expand-indexed is diagnostic only; it preserves indexed geometry intent but changes vertex submission/cache behavior and can heavily regress GPU/CPU cost."
fi
if (( probe_force_expand_indexed )) || [[ -n "$probe_force_expand_indexed_row$probe_force_expand_indexed_rows$probe_force_expand_indexed_class$probe_force_expand_indexed_classes" ]]; then
  echo "warning: --probe-force-expand-indexed is diagnostic only; it preserves indexed geometry intent but changes selected draw vertex submission/cache behavior and can heavily regress GPU/CPU cost."
fi
if (( probe_reverse_indexed_triangles )); then
  echo "warning: --probe-reverse-indexed-triangles is diagnostic only; it preserves indexed draw count/render state but changes primitive order and can alter depth/blend results."
fi
if (( probe_reverse_nonopaque_indexed_triangles )); then
  echo "warning: --probe-reverse-nonopaque-indexed-triangles is diagnostic only; it targets visibility-sensitive draws and can corrupt depth/blend results."
fi
if (( optimize_screen_blend_index_order )); then
  echo "warning: --optimize-screen-blend-index-order is diagnostic/profiling only; screen-blend output is destination-dependent and primitive order can change blended pixels."
fi
if (( optimize_screen_blend_index_cache )); then
  if [[ -z "$semantic_image_policy" ]]; then
    echo "warning: --optimize-screen-blend-index-cache is mechanism/profiling-only until a same-input semantic proof is attached."
    echo "warning: add --semantic-image-policy exact|lsb1 with same-input mini-replay images before treating screen-blend cache results as explicit proof."
  else
    echo "warning: --optimize-screen-blend-index-cache uses explicit ${semantic_image_policy} semantic policy; do not generalize this proof to broad depth-read reorder."
  fi
fi
if (( probe_apply_index_cache_opt_candidate_unsafe_nonopaque )); then
  echo "warning: --probe-apply-index-cache-opt-candidate-unsafe-nonopaque is diagnostic only; it bypasses the opaque-depth-write safety gate and can change depth/blend/scissor final writers. Use tight row/class filters plus same-input semantic images before treating the result as proof."
fi
if [[ -n "$probe_scissor_rect" ]]; then
  echo "warning: --probe-scissor-rect is diagnostic only; it changes raster coverage and can corrupt frame output. Use it only to classify scissor/tile-coverage backend effects."
fi
if [[ -n "$force_cull_mode" ]]; then
  echo "warning: --force-cull-mode is diagnostic only and can corrupt visibility; use it only to classify cull/backend state-shape effects."
fi

if (( dry_run )); then
  if (( capture_gputrace )) &&
     capture_destination_is_file "$metal_capture_destination" &&
     [[ "${DXMT_3DMARK05_ALLOW_NO_FILE_CAPTURE_LAYER:-0}" != "1" ]]; then
    if (( with_wine_capture_layer )); then
      if ! run_wine_capture_layer_wrapper_preflight "$probe_wine_root"; then
        echo "dry-run: file capture layer preflight would fail for --with-wine-capture-layer"
      fi
    elif ! run_file_capture_layer_preflight "$probe_wine_root"; then
      echo "dry-run: file capture layer preflight would fail without --with-wine-capture-layer"
    fi
  fi
  if [[ "$free_mb" != unknown && "$min_free_mb" != 0 && "$free_mb" -lt "$min_free_mb" ]]; then
    echo "dry-run: free space is below the launch guard; cleanup candidates follow"
    print_space_hints 1
  fi
  exit 0
fi

if [[ "${DXMT_3DMARK05_REQUIRE_UNLOCKED:-1}" != "0" &&
      "$session_locked" == yes &&
      "$wait_unlocked_sec" -gt 0 ]]; then
  waited_sec=0
  while [[ "$session_locked" == yes && "$waited_sec" -lt "$wait_unlocked_sec" ]]; do
    remaining_sec=$((wait_unlocked_sec - waited_sec))
    sleep_sec=$wait_unlocked_interval_sec
    if (( sleep_sec > remaining_sec )); then
      sleep_sec=$remaining_sec
    fi
    printf 'waiting for macOS session unlock: %ss/%ss\n' \
      "$waited_sec" "$wait_unlocked_sec" >&2
    sleep "$sleep_sec"
    waited_sec=$((waited_sec + sleep_sec))
    session_locked=$(detect_session_locked)
  done
  echo "session_locked_after_wait: $session_locked"
fi

if [[ "${DXMT_3DMARK05_REQUIRE_UNLOCKED:-1}" != "0" && "$session_locked" == yes ]]; then
  if [[ "$wait_unlocked_sec" -gt 0 ]]; then
    echo "macOS session is locked after waiting ${wait_unlocked_sec}s; unlock the desktop before running 3DMark05 perf/gputrace" >&2
  else
    echo "macOS session is locked; unlock the desktop before running 3DMark05 perf/gputrace" >&2
  fi
  exit 2
fi

if (( capture_gputrace )) &&
   (( min_free_mb < recommended_gputrace_min_free_mb )) &&
   [[ "${DXMT_3DMARK05_ALLOW_LOW_TRACE_FREE_MB:-0}" != "1" ]]; then
  echo "refusing low free-space gputrace launch guard: --min-free-mb ${min_free_mb} is below the recommended ${recommended_gputrace_min_free_mb}MiB" >&2
  echo "raise --min-free-mb, free disk space, or set DXMT_3DMARK05_ALLOW_LOW_TRACE_FREE_MB=1 after deliberately accepting partial-run/no-result.json risk" >&2
  exit 2
fi

if (( capture_gputrace )) &&
   capture_destination_is_file "$metal_capture_destination" &&
   [[ "${DXMT_3DMARK05_ALLOW_NO_FILE_CAPTURE_LAYER:-0}" != "1" ]]; then
  if (( with_wine_capture_layer )); then
    if ! run_wine_capture_layer_wrapper_preflight "$probe_wine_root"; then
      echo "Metal file capture was requested through the Wine capture-layer wrapper, but the capture-enabled Wine copies are unavailable." >&2
      echo "Expected MetalCaptureEnabled in:" >&2
      echo "  $probe_wine_root/bin/wine.capture.real" >&2
      echo "  $probe_wine_root/bin/wine.capture.real-preloader" >&2
      exit 2
    fi
  elif ! run_file_capture_layer_preflight "$probe_wine_root"; then
    echo "Metal file capture requires Apple's capture layer in the Wine child." >&2
    echo "The current Wine launcher lacks MetalCaptureEnabled and no Metal capture-layer env was requested." >&2
    echo "Use --with-wine-capture-layer for a deliberate file .gputrace diagnostic, --xcode-developer-tools-capture with a passing Xcode attach preflight, or set DXMT_3DMARK05_ALLOW_NO_FILE_CAPTURE_LAYER=1 for a deliberate late-failure diagnostic." >&2
    echo "Do not use DXMT_3DMARK05_SET_MTL_CAPTURE_ENABLED=1 as a normal 3DMark05 perf path; it can SIGKILL/black-screen before draw/present." >&2
    exit 2
  fi
fi

if (( require_xcode_attach_preflight )) &&
   capture_destination_is_developer_tools "$metal_capture_destination"; then
  if ! run_xcode_attach_preflight; then
    echo "Xcode attach preflight failed; fix Xcode attach state before starting a developerTools capture run" >&2
    exit 2
  fi
fi

if [[ "$free_mb" != unknown && "$min_free_mb" != 0 && "$free_mb" -lt "$min_free_mb" ]]; then
  echo "insufficient free space for 3DMark05 perf probe: ${free_mb}MiB available, ${min_free_mb}MiB required" >&2
  echo "free space under the repository volume or rerun with --min-free-mb N / DXMT_3DMARK05_MIN_TRACE_FREE_MB=N after deliberately accepting the risk" >&2
  print_space_hints 2
  exit 2
fi

mkdir -p "$output_dir" "$trace_dir"
if (( dump_shaders )); then
  mkdir -p "$shader_msl_dump_dir" "$shader_bytecode_dump_dir"
fi
if (( dump_indexed_geometry )); then
  mkdir -p "$geometry_dump_dir"
fi
if (( dump_framegraph_dag )); then
  mkdir -p "$framegraph_dag_dir"
fi
if [[ -n "$dump_depth_attachment_handle" ]]; then
  mkdir -p "$(dirname -- "$dump_depth_attachment_path")"
fi
if [[ -n "$dump_color_attachment_handle$dump_color_attachment_index$dump_color_attachment_seq$dump_color_attachment_enc$dump_color_attachment_draw$dump_color_attachment_draws$dump_color_attachment_command_index$dump_color_attachment_command_index_min$dump_color_attachment_command_index_max$dump_color_attachment_texture0$dump_color_attachment_texture0s$dump_color_attachment_path$dump_color_attachment_dir$dump_color_attachment_roi_summary_path" ]]; then
  if [[ -n "$dump_color_attachment_dir" ]]; then
    mkdir -p "$dump_color_attachment_dir"
  elif [[ -n "$dump_color_attachment_path" ]]; then
    mkdir -p "$(dirname -- "$dump_color_attachment_path")"
  fi
  if [[ -n "$dump_color_attachment_roi_summary_path" ]]; then
    mkdir -p "$(dirname -- "$dump_color_attachment_roi_summary_path")"
  fi
fi
if [[ -n "$dump_draw_texture_handles$dump_draw_texture0_width$dump_draw_texture0_height$dump_draw_texture0_format" ||
      "$dump_draw_texture0_any" -eq 1 ]]; then
  mkdir -p "$dump_draw_texture_dir"
fi
if [[ -n "$capture_dir" ]]; then
  mkdir -p "$capture_dir"
fi

run_status=0
trap 'stop_3dmark05_frontmost_loop' EXIT
start_3dmark05_frontmost_loop
(
  cd "$repo_root"
  python3 scripts/tools/run_with_timeout.py \
    --timeout "$watchdog_base_sec" \
    --slack "$timeout_slack" \
    --label 3dmark05-perf-wrapper \
    -- \
    env "${env_args[@]}" "${cmd[@]}"
) || run_status=$?
stop_3dmark05_frontmost_loop
trap - EXIT
cleanup_3dmark05_probe_wineserver

summary_cmd=(
  python3 "$repo_root/scripts/tools/summarize_3dmark05_perf.py"
  "$output_dir"
  --output "$summary_path"
)
if (( require_current_uniform_compact_saved_bytes_present )); then
  summary_cmd+=(--require-uniform-compact-saved-bytes-present)
fi
"${summary_cmd[@]}"
python3 "$repo_root/scripts/tools/summarize_index_cache_runtime.py" \
  --run "$suffix=$encoders_csv,$probe_draws_csv" \
  --output "$index_cache_runtime_report" \
  --csv-output "$index_cache_runtime_csv"

if (( visibility_scout )); then
  if [[ -f "$visibility_scout_path" ]]; then
    visibility_summary_cmd=(
      python3 "$repo_root/scripts/tools/summarize_visibility_scout.py"
      "$visibility_scout_path"
      --probe-draws "$probe_draws_csv"
      --output "$visibility_scout_summary_output"
      --csv-output "$visibility_scout_summary_csv_output"
      --limit "$visibility_scout_summary_limit"
      --no-sample-limit "$visibility_scout_summary_limit"
    )
    if [[ -n "$visibility_scout_row" ]]; then
      visibility_summary_cmd+=(--row "$visibility_scout_row")
    fi
    if [[ -n "$visibility_scout_draw_indices" ]]; then
      visibility_summary_cmd+=(--draw-indices "$visibility_scout_draw_indices")
    fi
    "${visibility_summary_cmd[@]}"
  else
    echo "warning: visibility scout was enabled but no CSV was written: $visibility_scout_path" >&2
  fi
fi

if (( dump_framegraph_dag )); then
  if find "$framegraph_dag_dir" -maxdepth 1 -name 'dag-frame*-chunk*-*.json' -print -quit | grep -q .; then
    python3 "$repo_root/scripts/tools/summarize_framegraph_dag.py" "$framegraph_dag_dir" \
      --summary-csv "$framegraph_dag_summary_csv" \
      --csv "$framegraph_dag_candidates_csv" \
      --markdown "$framegraph_dag_summary"
    python3 "$repo_root/scripts/tools/summarize_framegraph_dag.py" "$framegraph_dag_dir" \
      --stage pre-opt \
      --summary-csv "$framegraph_dag_preopt_summary_csv" \
      --csv "$framegraph_dag_preopt_candidates_csv" \
      --markdown "$framegraph_dag_preopt_summary"
    python3 "$repo_root/scripts/tools/summarize_framegraph_dag.py" "$framegraph_dag_dir" \
      --stage post-opt \
      --summary-csv "$framegraph_dag_postopt_summary_csv" \
      --csv "$framegraph_dag_postopt_candidates_csv" \
      --markdown "$framegraph_dag_postopt_summary"
  else
    echo "warning: framegraph DAG dump was enabled but no JSON files were written: $framegraph_dag_dir" >&2
  fi
fi

if (( capture_gputrace )); then
  if capture_destination_is_developer_tools "$metal_capture_destination"; then
    capture_started=0
    capture_stopped=0
    for log_path in "$output_dir/dxmt9.log" "$output_dir/3dmark05-direct.log"; do
      if [[ -f "$log_path" ]]; then
        if grep -F "Metal capture frame=${frame} " "$log_path" | grep -F "destination=1 started" >/dev/null; then
          capture_started=1
        fi
        if grep -F "Metal capture frame=${frame} " "$log_path" | grep -F "destination=1 stopped" >/dev/null; then
          capture_stopped=1
        fi
      fi
    done
    if (( ! capture_started || ! capture_stopped )); then
      echo "Metal developerTools capture was requested but start/stop was not proven in logs" >&2
      echo "expected Xcode-attached capture log lines for frame $frame with destination=1" >&2
      echo "classify as attach-preflight failure if Xcode could not attach to the Wine child" >&2
      echo "classify as frame-target miss if the run did not reach frame $frame" >&2
      echo "check the run log for MTLCaptureManager errors:" >&2
      echo "  $output_dir/dxmt9.log" >&2
      echo "  $output_dir/3dmark05-direct.log" >&2
      exit 2
    fi
  elif [[ ! -e "$capture_path" ]]; then
    echo "Metal gputrace capture was requested but no capture was written: $capture_path" >&2
    echo "check the run log for MTLCaptureManager errors:" >&2
    echo "  $output_dir/dxmt9.log" >&2
    echo "  $output_dir/3dmark05-direct.log" >&2
    exit 2
  fi
fi

python3 - "$trace_artifacts_json" \
  "$run_id" \
  "$output_dir" \
  "$trace_dir" \
  "$analysis_dir" \
  "$frame" \
  "$capture_gputrace" \
  "${metal_capture_destination:-gpuTraceDocument}" \
  "$capture_path" \
  "$xcode_performance_gputrace" \
  "$xcode_encoder_counters_csv" \
  "$xcode_counters_summary_csv" \
  "$xcode_joined_summary_csv" \
  "$xcode_bottleneck_report" <<'PY'
import json
import pathlib
import sys

(
    out,
    run_id,
    output_dir,
    trace_dir,
    analysis_dir,
    frame,
    capture_gputrace,
    destination,
    capture_path,
    performance_gputrace,
    counters_csv,
    counters_summary_csv,
    joined_summary_csv,
    bottleneck_report,
) = sys.argv[1:]

capture_enabled = capture_gputrace != "0"
direct_file_expected = capture_enabled and destination not in {"developerTools", "xcode"}
paths = {
    "output_dir": output_dir,
    "trace_dir": trace_dir,
    "analysis_dir": analysis_dir,
    "gputrace": capture_path if direct_file_expected else None,
    "xcode_performance_gputrace": performance_gputrace,
    "xcode_encoder_counters_csv": counters_csv,
    "xcode_counters_summary_csv": counters_summary_csv,
    "xcode_dxmt_joined_summary_csv": joined_summary_csv,
    "xcode_dxmt_bottleneck_report": bottleneck_report,
}
exists = {name: pathlib.Path(value).exists() for name, value in paths.items() if value}
payload = {
    "run_id": run_id,
    "frame": int(frame),
    "capture_gputrace": capture_enabled,
    "metal_capture_destination": destination,
    "direct_gputrace_file_expected": direct_file_expected,
    "paths": paths,
    "exists": exists,
}
pathlib.Path(out).write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
PY

if ((${#counter_compare_cmd[@]})); then
  mkdir -p "$trace_dir/analysis"
  (
    cd "$repo_root"
    "${counter_compare_cmd[@]}"
  )
fi

echo "wrote summary: $summary_path"
echo "wrote encoder csv: $encoders_csv"
echo "wrote stream csv: $stream_csv"
echo "wrote probe draw csv: $probe_draws_csv"
echo "wrote index cache runtime report: $index_cache_runtime_report"
echo "wrote index cache runtime csv: $index_cache_runtime_csv"
echo "wrote trace artifacts manifest: $trace_artifacts_json"
if (( visibility_scout )); then
  echo "wrote visibility scout csv: $visibility_scout_path"
  echo "wrote visibility scout summary: $visibility_scout_summary_output"
  echo "wrote visibility scout summary csv: $visibility_scout_summary_csv_output"
fi
if (( dump_framegraph_dag )); then
  echo "wrote framegraph DAG dir: $framegraph_dag_dir"
  if [[ -e "$framegraph_dag_summary" ]]; then
    echo "wrote framegraph DAG summary: $framegraph_dag_summary"
  fi
  if [[ -e "$framegraph_dag_summary_csv" ]]; then
    echo "wrote framegraph DAG summary csv: $framegraph_dag_summary_csv"
  fi
  if [[ -e "$framegraph_dag_candidates_csv" ]]; then
    echo "wrote framegraph DAG candidates csv: $framegraph_dag_candidates_csv"
  fi
  if [[ -e "$framegraph_dag_preopt_summary" ]]; then
    echo "wrote framegraph DAG preopt summary: $framegraph_dag_preopt_summary"
  fi
  if [[ -e "$framegraph_dag_preopt_summary_csv" ]]; then
    echo "wrote framegraph DAG preopt summary csv: $framegraph_dag_preopt_summary_csv"
  fi
  if [[ -e "$framegraph_dag_preopt_candidates_csv" ]]; then
    echo "wrote framegraph DAG preopt candidates csv: $framegraph_dag_preopt_candidates_csv"
  fi
  if [[ -e "$framegraph_dag_postopt_summary" ]]; then
    echo "wrote framegraph DAG postopt summary: $framegraph_dag_postopt_summary"
  fi
  if [[ -e "$framegraph_dag_postopt_summary_csv" ]]; then
    echo "wrote framegraph DAG postopt summary csv: $framegraph_dag_postopt_summary_csv"
  fi
  if [[ -e "$framegraph_dag_postopt_candidates_csv" ]]; then
    echo "wrote framegraph DAG postopt candidates csv: $framegraph_dag_postopt_candidates_csv"
  fi
fi
if ((${#counter_compare_cmd[@]})); then
  echo "wrote perf counter comparison: $counter_comparison_path"
fi
if (( capture_gputrace )); then
  echo "after Xcode exports encoder counters:"
  printf '  '
  printf '%q ' "${finalize_cmd[@]}"
  printf '\n'
fi
if (( dump_shaders )); then
  echo "wrote shader dump dir: $shader_dump_dir"
fi
if [[ -n "$dump_depth_attachment_handle" ]]; then
  if [[ -e "$dump_depth_attachment_path" ]]; then
    echo "wrote depth attachment dump: $dump_depth_attachment_path"
  else
    echo "warning: requested depth attachment dump was not written: $dump_depth_attachment_path" >&2
  fi
fi
if [[ -n "$dump_color_attachment_handle$dump_color_attachment_index$dump_color_attachment_seq$dump_color_attachment_enc$dump_color_attachment_draw$dump_color_attachment_draws$dump_color_attachment_command_index$dump_color_attachment_command_index_min$dump_color_attachment_command_index_max$dump_color_attachment_texture0$dump_color_attachment_texture0s$dump_color_attachment_path$dump_color_attachment_dir$dump_color_attachment_roi_summary_path" ]]; then
  if [[ -n "$dump_color_attachment_dir" ]]; then
    shopt -s nullglob
    color_attachment_dump_files=("$dump_color_attachment_dir"/*.bin)
    shopt -u nullglob
    if ((${#color_attachment_dump_files[@]})); then
      echo "wrote color attachment dumps: $dump_color_attachment_dir (${#color_attachment_dump_files[@]} files)"
    else
      echo "warning: requested color attachment dumps were not written: $dump_color_attachment_dir" >&2
    fi
  elif [[ -e "$dump_color_attachment_path" ]]; then
    echo "wrote color attachment dump: $dump_color_attachment_path"
  elif [[ -n "$dump_color_attachment_path" ]]; then
    echo "warning: requested color attachment dump was not written: $dump_color_attachment_path" >&2
  fi
  if [[ -n "$dump_color_attachment_roi_summary_path" ]]; then
    if [[ -e "$dump_color_attachment_roi_summary_path" ]]; then
      echo "wrote color attachment ROI summary: $dump_color_attachment_roi_summary_path"
    else
      echo "warning: requested color attachment ROI summary was not written: $dump_color_attachment_roi_summary_path" >&2
    fi
  fi
fi
if [[ -n "$dump_draw_texture_handles$dump_draw_texture0_width$dump_draw_texture0_height$dump_draw_texture0_format" ||
      "$dump_draw_texture0_any" -eq 1 ]]; then
  if find "$dump_draw_texture_dir" -maxdepth 1 -name 'texture-*.json' -print -quit | grep -q .; then
    echo "wrote draw texture dump dir: $dump_draw_texture_dir"
  else
    echo "warning: requested draw texture dumps were not written: $dump_draw_texture_dir" >&2
  fi
fi

if (( run_status != 0 )); then
  echo "3DMark05 perf run exited with status $run_status after writing available postprocess artifacts" >&2
  exit "$run_status"
fi
