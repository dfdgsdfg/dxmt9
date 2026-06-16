#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)

frame=${DXMT_3DMARK05_PROBE_FRAME:-60}
suffix=${DXMT_3DMARK05_PROBE_SUFFIX:-}
run_id=${DXMT_3DMARK05_RUN_ID:-}
output_dir=${DXMT_3DMARK05_OUTPUT_DIR:-}
trace_dir=${DXMT_3DMARK05_TRACE_DIR:-}
xcode_csv=${DXMT_3DMARK05_XCODE_COUNTERS_CSV:-}
baseline_output=${DXMT_3DMARK05_COMPARE_BASELINE_OUTPUT:-}
baseline_joined=${DXMT_3DMARK05_COMPARE_BASELINE_JOINED:-}
before_label=${DXMT_3DMARK05_COMPARE_BEFORE_LABEL:-baseline}
after_label=${DXMT_3DMARK05_COMPARE_AFTER_LABEL:-}
semantic_image_policy=${DXMT_3DMARK05_SEMANTIC_IMAGE_POLICY:-}
semantic_image_before=${DXMT_3DMARK05_SEMANTIC_IMAGE_BEFORE:-}
semantic_image_after=${DXMT_3DMARK05_SEMANTIC_IMAGE_AFTER:-}
semantic_image_output=${DXMT_3DMARK05_SEMANTIC_IMAGE_OUTPUT:-}
semantic_image_summary_output=${DXMT_3DMARK05_SEMANTIC_IMAGE_SUMMARY_OUTPUT:-}
semantic_image_diff_output=${DXMT_3DMARK05_SEMANTIC_IMAGE_DIFF_OUTPUT:-}
semantic_image_min_active_pct=${DXMT_3DMARK05_SEMANTIC_IMAGE_MIN_ACTIVE_PCT:-1}
dry_run=0
require_result_json=0
allow_partial_stable_frame_proof=0

require_color_dontcare_increase=0
require_depth_dontcare_increase=0
require_tile_preservation_decrease=0
require_tile_preservation_not_increase=0
require_command_buffers_per_present_not_increase=0
require_render_passes_per_present_not_increase=0
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
require_encode_chunk_cpu_per_present_decrease=0
require_no_enqueue_commit_entry_to_publish_decrease=0
require_no_enqueue_publish_to_encode_dequeue_decrease=0
require_no_enqueue_encode_dequeue_to_commit_decrease=0
require_no_enqueue_wait_to_next_enqueue_decrease=0
max_gpu_command_buffer_regression_ms=${DXMT_3DMARK05_MAX_GPU_COMMAND_BUFFER_REGRESSION_MS:-}
max_const_upload_break_count_ratio=${DXMT_3DMARK05_MAX_CONST_UPLOAD_BREAK_COUNT_RATIO:-}

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
require_cache_opt_apply_proof=0
require_opaque_depth_index_cache_proof=0
require_screen_blend_cache_proof=0
require_semantic_image_proof=0
require_tvb_mechanism_proof=0
require_target_index_cache_miss32_decrease=0
require_target_index_cache_opt_miss32_decrease=0
require_target_reordered_index_cache_hits=0
require_target_vs_buffer_write_decrease=0
require_target_vs_invocations_decrease=0
require_top_pso_attribution=0
require_xcode_counter_coverage=0
require_dxmt_join_coverage=0
require_shader_dump_matches=${DXMT_3DMARK05_REQUIRE_SHADER_DUMP_MATCHES:-0}
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
class_proxy_top=${DXMT_3DMARK05_CLASS_PROXY_TOP:-12}
target_row_keys=()

usage() {
  cat <<'USAGE'
Usage: scripts/tools/finalize_3dmark05_perf_probe.sh [options]

Finalize a 3DMark05 GT1 perf probe after Xcode has exported encoder counters.
This regenerates dxmt summaries, joins Xcode encoder counters with dxmt encoder
attribution, and optionally compares the candidate against baseline outputs.

Options:
  --suffix NAME       Probe suffix used by run_3dmark05_perf_probe.sh
  --run-id ID         Full run id (default: app-d3d9-3dmark05-<suffix>)
  --frame N           Frame number used in frame<N>-counters-xcode.csv (default: 60)
  --output-dir PATH   Experiment output dir (default: experiments/output/<run-id>)
  --trace-dir PATH    Trace dir (default: traces/<run-id>)
  --xcode-csv PATH    Xcode Export Encoder Counters CSV
                      (default: <trace-dir>/analysis/frame<N>-counters-xcode.csv)
  --baseline-output PATH
                      Compare run-level result.json counters against this baseline output dir/result.json
  --baseline-joined PATH
                      Compare Xcode+dxmt joined summary against this baseline joined CSV
  --before-label NAME Baseline label for comparison reports (default: baseline)
  --after-label NAME  Candidate label for comparison reports (default: suffix or run id)
  --semantic-image-policy exact|lsb1
                      Also gate a same-input mini-replay image pair with the
                      named compare_experiment_images.py policy. Use exact for
                      correctness; use lsb1 only for explicit blend-rounding
                      tolerance decisions.
  --semantic-image-before PATH
                      Baseline image for --semantic-image-policy
  --semantic-image-after PATH
                      Candidate image for --semantic-image-policy
  --semantic-image-output PATH
                      Report output path (default:
                      <analysis-dir>/frame<N>-semantic-image-policy-<policy>-compare.md)
  --semantic-image-summary-output PATH
                      CSV summary output path (default: report path with .csv)
  --semantic-image-diff-output PATH
                      Diff image output path (default: report path with .png)
  --semantic-image-min-active-pct N
                      Min before/after active pixel percentage for semantic
                      image gates (default: 1)
  --require-result-json
                      Gate: fail instead of using dxmt9.log partial-run counters
  --allow-partial-stable-frame-proof
                      Do not let --require-stable-frame-proof imply
                      --require-result-json. Use only for timeout-finalized
                      captures where Xcode encoder counters and dxmt9.log are
                      complete enough for row-local GPU proof. An explicit
                      --require-result-json or --baseline-output still requires
                      result.json.
  --require-color-dontcare-increase
  --require-depth-dontcare-increase
  --require-tile-preservation-decrease
  --require-tile-preservation-not-increase
  --require-command-buffers-per-present-not-increase
  --require-render-passes-per-present-not-increase
  --require-draw-run-records-increase
  --require-draw-run-records-per-submit-increase
  --require-binding-overrides-present
  --require-const-upload-passthrough-present
  --require-draw-submission-batch-present
  --require-const-upload-break-bytes-decrease
  --max-const-upload-break-count-ratio N
  --require-encode-draw-cpu-decrease
  --require-completion-present-wait-decrease
  --require-completion-wait-with-enqueue-increase
  --require-completion-wait-without-enqueue-decrease
  --require-completion-present-wait-with-enqueue-increase
  --require-completion-present-wait-without-enqueue-decrease
  --require-commit-chunk-replay-cpu-per-present-decrease
  --require-queue-draw-submission-cpu-per-present-decrease
  --require-snapshot-cpu-per-present-decrease
  --require-snapshot-cache-lookup-cpu-per-present-decrease
  --require-snapshot-cache-uniform-build-cpu-per-present-decrease
  --require-snapshot-cache-uniform-hash-cpu-per-present-decrease
  --require-batch-miss-uniform-build-cpu-per-present-decrease
  --require-batch-miss-uniform-hash-cpu-per-present-decrease
  --require-batch-miss-vs-const-hash-cpu-per-present-decrease
  --require-batch-miss-ps-const-hash-cpu-per-present-decrease
  --require-batch-miss-nonconst-hash-cpu-per-present-decrease
  --require-snapshot-uniform-copy-cpu-per-present-decrease
  --require-submit-draw-run-batch-append-uniform-cpu-per-present-decrease
  --require-draw-uniform-payload-lookup-cpu-per-present-decrease
  --require-draw-uniform-payload-append-copy-cpu-per-present-decrease
  --require-argbuf-setup-cpu-per-present-decrease
  --require-argbuf-open-cpu-per-present-decrease
  --require-argbuf-cbuf-update-cpu-per-present-decrease
  --require-argbuf-cbuf-update-vs-cpu-per-present-decrease
  --require-uniform-compact-saved-bytes-present
  --require-current-uniform-compact-saved-bytes-present
  --require-encode-chunk-cpu-per-present-decrease
  --require-no-enqueue-commit-entry-to-publish-decrease
  --require-no-enqueue-publish-to-encode-dequeue-decrease
  --require-no-enqueue-encode-dequeue-to-commit-decrease
  --require-no-enqueue-wait-to-next-enqueue-decrease
  --max-gpu-command-buffer-regression-ms N
  --require-top-gpu-decrease
  --require-top-buffer-write-decrease
  --require-top-vs-buffer-write-decrease
  --require-top-unexplained-buffer-write-decrease
  --require-stream-handle-churn-decrease
  --require-ib-handle-churn-decrease
  --require-argbuf-cbuf-decrease
  --require-transient-decrease
  --require-top-gpu-share-increase
  --require-top-row-key-match
  --require-stable-frame-proof
                      Gate preset: require result.json, counter/join coverage,
                      PSO attribution, top row-key match, top GPU/VS/
                      unexplained write decrease, and <=5% top geometry drift
  --require-cache-opt-apply-proof
                      Gate preset for cache-opt apply runs: stable frame proof
                      plus target rows' actual LRU32 miss, VS buffer write, and
                      VS invocation decreases; requires --target-row-key
  --require-opaque-depth-index-cache-proof
                      Gate preset for production-shaped opaque depth cached-index
                      opt-in: stable-frame proof plus target cache-opt telemetry,
                      reordered-cache hits, and target VS write/invocation
                      decreases; requires --target-row-key. The run must have
                      captured index-reuse and cache-opt-candidate telemetry.
  --require-screen-blend-cache-proof
                      Gate preset for screen-blend cached-index opt-in:
                      stable-frame proof plus target cache-opt telemetry,
                      reordered-cache hits, target VS write/invocation
                      decreases, and a same-input semantic image gate; requires
                      --target-row-key and --semantic-image-policy with
                      before/after images. The run must have captured
                      index-reuse and cache-opt-candidate telemetry.
  --require-semantic-image-proof
                      Gate: require --semantic-image-policy with before/after
                      images and fail if that image comparison fails. Use this
                      for unsafe nonopaque cache-order proofs where Xcode
                      counter movement alone is only a mechanism proof.
  --require-tvb-mechanism-proof
                      Gate: top hidden backend write MiB, VS buffer write MiB,
                      VS invocations, and GPU time must all strictly decrease
                      (row-local TVB pressure mechanism proof; named tiled
                      counters can under-report hidden-expanded storage)
  --require-target-index-cache-miss32-decrease
                      Xcode compare gate: target rows' actual dxmt LRU32 miss
                      estimate must decrease, catching cache-opt apply no-ops
  --require-target-index-cache-opt-miss32-decrease
                      Xcode compare gate: after target rows' cache-opt
                      candidate/effective LRU32 estimate must decrease
  --require-target-reordered-index-cache-hits
                      Xcode compare gate: every after target row must have
                      positive reordered index cache hits
  --require-target-vs-buffer-write-decrease
                      Xcode compare gate: target rows' VS buffer write must decrease
  --require-target-vs-invocations-decrease
                      Xcode compare gate: target rows' VS invocation count must decrease
  --require-top-pso-attribution
  --require-xcode-counter-coverage
  --require-dxmt-join-coverage
  --require-shader-dump-matches
                      Gate: top render encoder shader hashes must match dumped
                      MSL files unambiguously
  --min-top-pso-samples-per-draw N
  --min-top-dxmt-joined-fraction N
  --max-top-gpu-regression-ms N
  --max-top-buffer-write-regression-mib N
  --target-row-key ROW
                      Xcode compare gate: target seq/enc row key, e.g. 50/1;
                      repeat to exclude all mutated rows from non-target guards
  --max-non-target-gpu-regression-ms N
                      Xcode compare gate: max matched GPU ms regression for
                      before top-N rows excluding --target-row-key rows
  --max-non-target-vs-buffer-write-regression-mib N
                      Xcode compare gate: max matched VS buffer write MiB
                      regression for before top-N non-target rows
  --max-non-target-vs-invocations-regression-ratio N
                      Xcode compare gate: max matched relative VS invocation
                      regression for before top-N non-target rows
  --max-non-target-draw-call-delta-ratio N
                      Xcode compare gate: max relative draw-count drift for
                      before top-N non-target rows
  --max-non-target-vertex-count-delta-ratio N
                      Xcode compare gate: max relative vertex-count drift for
                      before top-N non-target rows
  --max-non-target-triangle-delta-ratio N
                      Xcode compare gate: max relative triangle-count drift for
                      before top-N non-target rows
  --max-top-unexplained-buffer-write-ratio N
  --max-top-draw-call-delta-ratio N
  --max-top-vertex-count-delta-ratio N
  --max-top-triangle-delta-ratio N
  --top N             GPU-time-ranked encoder count for top-N gates and comparison
                      (default: 3)
  --hot-gpu-share PCT GPU share target for report-only Hot Set Aggregate
                      (default: 95.0)
  --class-proxy-top N Top indexed state/class proxy rows to emit after joining
                      Xcode counters (default: 12)
  --dry-run           Print derived paths and commands without running them
  -h, --help          Show this help
USAGE
}

while (($#)); do
  case "$1" in
    --suffix)
      suffix=${2:?missing value for --suffix}
      shift 2
      ;;
    --run-id)
      run_id=${2:?missing value for --run-id}
      shift 2
      ;;
    --frame)
      frame=${2:?missing value for --frame}
      shift 2
      ;;
    --output-dir)
      output_dir=${2:?missing value for --output-dir}
      shift 2
      ;;
    --trace-dir)
      trace_dir=${2:?missing value for --trace-dir}
      shift 2
      ;;
    --xcode-csv)
      xcode_csv=${2:?missing value for --xcode-csv}
      shift 2
      ;;
    --baseline-output)
      baseline_output=${2:?missing value for --baseline-output}
      shift 2
      ;;
    --baseline-joined)
      baseline_joined=${2:?missing value for --baseline-joined}
      shift 2
      ;;
    --before-label)
      before_label=${2:?missing value for --before-label}
      shift 2
      ;;
    --after-label)
      after_label=${2:?missing value for --after-label}
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
    --require-result-json)
      require_result_json=1
      shift
      ;;
    --allow-partial-stable-frame-proof)
      allow_partial_stable_frame_proof=1
      shift
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
    --max-gpu-command-buffer-regression-ms)
      max_gpu_command_buffer_regression_ms=${2:?missing value for --max-gpu-command-buffer-regression-ms}
      shift 2
      ;;
    --max-const-upload-break-count-ratio)
      max_const_upload_break_count_ratio=${2:?missing value for --max-const-upload-break-count-ratio}
      shift 2
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
    --require-tvb-mechanism-proof)
      require_tvb_mechanism_proof=1
      shift
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
    --class-proxy-top)
      class_proxy_top=${2:?missing value for --class-proxy-top}
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

validate_optional_number() {
  local name=$1
  local value=$2
  if [[ -n "$value" && ! "$value" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
    echo "$name must be numeric" >&2
    exit 2
  fi
}

validate_optional_positive_number() {
  local name=$1
  local value=$2
  if [[ -n "$value" &&
        ( ! "$value" =~ ^[0-9]+([.][0-9]+)?$ ||
          "$value" =~ ^0+([.]0+)?$ ) ]]; then
    echo "$name must be positive numeric" >&2
    exit 2
  fi
}

validate_optional_number "--max-gpu-command-buffer-regression-ms" "$max_gpu_command_buffer_regression_ms"
validate_optional_positive_number "--max-const-upload-break-count-ratio" "$max_const_upload_break_count_ratio"
validate_optional_number "--min-top-pso-samples-per-draw" "$min_top_pso_samples_per_draw"
validate_optional_number "--min-top-dxmt-joined-fraction" "$min_top_dxmt_joined_fraction"
validate_optional_number "--max-top-gpu-regression-ms" "$max_top_gpu_regression_ms"
validate_optional_number "--max-top-buffer-write-regression-mib" "$max_top_buffer_write_regression_mib"
validate_optional_number "--max-non-target-gpu-regression-ms" "$max_non_target_gpu_regression_ms"
validate_optional_number "--max-non-target-vs-buffer-write-regression-mib" "$max_non_target_vs_buffer_write_regression_mib"
validate_optional_number "--max-non-target-vs-invocations-regression-ratio" "$max_non_target_vs_invocations_regression_ratio"
validate_optional_number "--max-non-target-draw-call-delta-ratio" "$max_non_target_draw_call_delta_ratio"
validate_optional_number "--max-non-target-vertex-count-delta-ratio" "$max_non_target_vertex_count_delta_ratio"
validate_optional_number "--max-non-target-triangle-delta-ratio" "$max_non_target_triangle_delta_ratio"
validate_optional_number "--max-top-unexplained-buffer-write-ratio" "$max_top_unexplained_buffer_write_ratio"
validate_optional_number "--max-top-draw-call-delta-ratio" "$max_top_draw_call_delta_ratio"
validate_optional_number "--max-top-vertex-count-delta-ratio" "$max_top_vertex_count_delta_ratio"
validate_optional_number "--max-top-triangle-delta-ratio" "$max_top_triangle_delta_ratio"
validate_optional_number "--semantic-image-min-active-pct" "$semantic_image_min_active_pct"
if [[ ! "$top_n" =~ ^[0-9]+$ ]] || (( top_n == 0 )); then
  echo "--top must be a positive integer" >&2
  exit 2
fi
validate_optional_number "--hot-gpu-share" "$hot_gpu_share"
if [[ ! "$class_proxy_top" =~ ^[0-9]+$ ]] || (( class_proxy_top == 0 )); then
  echo "--class-proxy-top must be a positive integer" >&2
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
  if (( ${#target_row_keys[@]} == 0 )); then
    echo "--require-screen-blend-cache-proof requires at least one --target-row-key" >&2
    exit 2
  fi
fi

if (( require_opaque_depth_index_cache_proof )); then
  require_stable_frame_proof=1
  require_target_index_cache_opt_miss32_decrease=1
  require_target_reordered_index_cache_hits=1
  require_target_vs_buffer_write_decrease=1
  require_target_vs_invocations_decrease=1
  if (( ${#target_row_keys[@]} == 0 )); then
    echo "--require-opaque-depth-index-cache-proof requires at least one --target-row-key" >&2
    exit 2
  fi
fi

if (( require_semantic_image_proof && ! semantic_image_compare_requested )); then
  echo "--require-semantic-image-proof requires --semantic-image-policy with --semantic-image-before and --semantic-image-after" >&2
  exit 2
fi

if (( require_cache_opt_apply_proof )); then
  require_stable_frame_proof=1
  require_target_index_cache_miss32_decrease=1
  require_target_vs_buffer_write_decrease=1
  require_target_vs_invocations_decrease=1
  if (( ${#target_row_keys[@]} == 0 )); then
    echo "--require-cache-opt-apply-proof requires at least one --target-row-key" >&2
    exit 2
  fi
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

run_level_compare_requested=0
if (( require_color_dontcare_increase ||
      require_depth_dontcare_increase ||
      require_tile_preservation_decrease ||
      require_tile_preservation_not_increase ||
      require_command_buffers_per_present_not_increase ||
      require_render_passes_per_present_not_increase ||
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
      require_encode_chunk_cpu_per_present_decrease ||
      require_no_enqueue_commit_entry_to_publish_decrease ||
      require_no_enqueue_publish_to_encode_dequeue_decrease ||
      require_no_enqueue_encode_dequeue_to_commit_decrease ||
      require_no_enqueue_wait_to_next_enqueue_decrease )) ||
   [[ -n "$max_gpu_command_buffer_regression_ms" ||
      -n "$max_const_upload_break_count_ratio" ]]; then
  run_level_compare_requested=1
fi

if (( run_level_compare_requested )) && [[ -z "$baseline_output" ]]; then
  echo "run-level comparison gates require --baseline-output <output-dir>" >&2
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

if (( xcode_compare_requested )) && [[ -z "$baseline_joined" ]]; then
  echo "Xcode comparison gates require --baseline-joined <joined.csv>" >&2
  exit 2
fi

if [[ -n "$baseline_output" ]]; then
  baseline_result="$baseline_output"
  if [[ -d "$baseline_result" ]]; then
    baseline_result="$baseline_result/result.json"
  fi
  if [[ ! -f "$baseline_result" ]]; then
    echo "missing baseline result.json: $baseline_result" >&2
    exit 2
  fi
fi

if [[ -n "$baseline_joined" && ! -f "$baseline_joined" ]]; then
  echo "missing baseline joined CSV: $baseline_joined" >&2
  exit 2
fi

if [[ -z "$run_id" && -n "$suffix" ]]; then
  run_id="app-d3d9-3dmark05-${suffix}"
fi

if [[ -z "$trace_dir" && -n "$xcode_csv" ]]; then
  xcode_parent=$(cd -- "$(dirname -- "$xcode_csv")" 2>/dev/null && pwd || true)
  if [[ -n "$xcode_parent" && "$(basename -- "$xcode_parent")" == analysis ]]; then
    trace_dir=$(dirname -- "$xcode_parent")
  fi
fi

if [[ -z "$run_id" && -n "$trace_dir" ]]; then
  run_id=$(basename -- "$trace_dir")
fi

if [[ -z "$suffix" && "$run_id" == app-d3d9-3dmark05-* ]]; then
  suffix=${run_id#app-d3d9-3dmark05-}
fi

if [[ -z "$run_id" ]]; then
  echo "provide --suffix, --run-id, --trace-dir, or --xcode-csv" >&2
  exit 2
fi

if [[ -z "$output_dir" ]]; then
  output_dir="$repo_root/experiments/output/$run_id"
fi
if [[ -z "$trace_dir" ]]; then
  trace_dir="$repo_root/traces/$run_id"
fi
analysis_dir="$trace_dir/analysis"
if [[ -z "$xcode_csv" ]]; then
  xcode_csv="$analysis_dir/frame${frame}-counters-xcode.csv"
fi
if [[ -z "$after_label" ]]; then
  after_label=${suffix:-$run_id}
fi

summary_path="$output_dir/3dmark05-perf-summary.md"
encoders_csv="$output_dir/3dmark05-perf-encoders.csv"
stream_csv="$output_dir/3dmark05-perf-encoder-streams.csv"
probe_draws_csv="$output_dir/3dmark05-perf-indexed-probe-draws.csv"
trace_artifacts_json="$output_dir/3dmark05-trace-artifacts.json"
capture_path="$trace_dir/frame${frame}.gputrace"
xcode_performance_gputrace="$analysis_dir/frame${frame}-performance.gputrace"
xcode_summary_csv="$analysis_dir/frame${frame}-counters-summary.csv"
joined_csv="$analysis_dir/frame${frame}-xcode-dxmt-joined-summary.csv"
xcode_report="$analysis_dir/frame${frame}-xcode-dxmt-bottleneck-report.md"
index_cache_runtime_report="$analysis_dir/frame${frame}-index-cache-runtime-summary.md"
index_cache_runtime_csv="$analysis_dir/frame${frame}-index-cache-runtime-summary.csv"
class_proxy_report="$analysis_dir/frame${frame}-indexed-state-class-xcode-proxy.md"
class_proxy_csv="$analysis_dir/frame${frame}-indexed-state-class-xcode-proxy.csv"
shader_msl_dir="$analysis_dir/shaders/msl"
shader_dump_report="$analysis_dir/frame${frame}-shader-dump-report.md"
shader_dump_csv="$analysis_dir/frame${frame}-shader-dump-summary.csv"
perf_compare_report="$analysis_dir/frame${frame}-perf-counter-comparison.md"
xcode_compare_report="$analysis_dir/frame${frame}-xcode-dxmt-comparison.md"
if (( semantic_image_compare_requested )); then
  if [[ -z "$semantic_image_output" ]]; then
    semantic_image_output="$analysis_dir/frame${frame}-semantic-image-policy-${semantic_image_policy}-compare.md"
  fi
  if [[ -z "$semantic_image_summary_output" ]]; then
    semantic_image_summary_output="${semantic_image_output%.*}.csv"
  fi
  if [[ -z "$semantic_image_diff_output" ]]; then
    semantic_image_diff_output="${semantic_image_output%.*}.png"
  fi
fi

summary_cmd=(
  python3 scripts/tools/summarize_3dmark05_perf.py
  "$output_dir"
  --output "$summary_path"
)
if (( require_current_uniform_compact_saved_bytes_present )); then
  summary_cmd+=(--require-uniform-compact-saved-bytes-present)
fi

index_cache_runtime_cmd=(
  python3 scripts/tools/summarize_index_cache_runtime.py
  --run "$after_label=$encoders_csv,$probe_draws_csv"
  --output "$index_cache_runtime_report"
  --csv-output "$index_cache_runtime_csv"
)

xcode_summary_cmd=(
  python3 scripts/tools/summarize_xcode_encoder_counters.py
  "$xcode_csv"
  --dxmt-encoders-csv "$encoders_csv"
  --dxmt-streams-csv "$stream_csv"
  --run-label "$after_label"
  --summary-output "$xcode_summary_csv"
  --joined-output "$joined_csv"
  --report-output "$xcode_report"
  --top "$top_n"
  --hot-gpu-share "$hot_gpu_share"
)
if (( require_top_pso_attribution )); then
  xcode_summary_cmd+=(
    --require-top-pso-attribution
    --min-top-pso-samples-per-draw "$min_top_pso_samples_per_draw"
  )
fi
if (( require_xcode_counter_coverage )); then
  xcode_summary_cmd+=(--require-xcode-counter-coverage)
fi
if (( require_dxmt_join_coverage )); then
  xcode_summary_cmd+=(
    --require-dxmt-join-coverage
    --min-top-dxmt-joined-fraction "$min_top_dxmt_joined_fraction"
  )
fi

class_proxy_cmd=(
  python3 scripts/tools/analyze_indexed_probe_classes.py
  "$probe_draws_csv"
  --group row-state-class
  --joined-summary "$joined_csv"
  --top "$class_proxy_top"
  --output "$class_proxy_report"
  --csv-output "$class_proxy_csv"
)

shader_dump_cmd=(
  python3 scripts/tools/analyze_shader_dumps.py
  "$joined_csv"
  --shader-dir "$shader_msl_dir"
  --output "$shader_dump_report"
  --csv-output "$shader_dump_csv"
)
if (( require_shader_dump_matches )); then
  shader_dump_cmd+=(--require-matches)
fi

perf_compare_cmd=()
if [[ -n "$baseline_output" ]]; then
  perf_compare_cmd=(
    python3 scripts/tools/compare_3dmark05_perf_counters.py
    "$baseline_output"
    "$output_dir"
    --before-label "$before_label"
    --after-label "$after_label"
    --output "$perf_compare_report"
  )
  if (( require_color_dontcare_increase )); then
    perf_compare_cmd+=(--require-color-dontcare-increase)
  fi
  if (( require_depth_dontcare_increase )); then
    perf_compare_cmd+=(--require-depth-dontcare-increase)
  fi
  if (( require_tile_preservation_decrease )); then
    perf_compare_cmd+=(--require-tile-preservation-decrease)
  fi
  if (( require_tile_preservation_not_increase )); then
    perf_compare_cmd+=(--require-tile-preservation-not-increase)
  fi
  if (( require_command_buffers_per_present_not_increase )); then
    perf_compare_cmd+=(--require-command-buffers-per-present-not-increase)
  fi
  if (( require_render_passes_per_present_not_increase )); then
    perf_compare_cmd+=(--require-render-passes-per-present-not-increase)
  fi
  if (( require_draw_run_records_increase )); then
    perf_compare_cmd+=(--require-draw-run-records-increase)
  fi
  if (( require_draw_run_records_per_submit_increase )); then
    perf_compare_cmd+=(--require-draw-run-records-per-submit-increase)
  fi
  if (( require_binding_overrides_present )); then
    perf_compare_cmd+=(--require-binding-overrides-present)
  fi
  if (( require_const_upload_passthrough_present )); then
    perf_compare_cmd+=(--require-const-upload-passthrough-present)
  fi
  if (( require_draw_submission_batch_present )); then
    perf_compare_cmd+=(--require-draw-submission-batch-present)
  fi
  if (( require_const_upload_break_bytes_decrease )); then
    perf_compare_cmd+=(--require-const-upload-break-bytes-decrease)
  fi
  if (( require_encode_draw_cpu_decrease )); then
    perf_compare_cmd+=(--require-encode-draw-cpu-decrease)
  fi
  if (( require_completion_present_wait_decrease )); then
    perf_compare_cmd+=(--require-completion-present-wait-decrease)
  fi
  if (( require_completion_wait_with_enqueue_increase )); then
    perf_compare_cmd+=(--require-completion-wait-with-enqueue-increase)
  fi
  if (( require_completion_wait_without_enqueue_decrease )); then
    perf_compare_cmd+=(--require-completion-wait-without-enqueue-decrease)
  fi
  if (( require_completion_present_wait_with_enqueue_increase )); then
    perf_compare_cmd+=(--require-completion-present-wait-with-enqueue-increase)
  fi
  if (( require_completion_present_wait_without_enqueue_decrease )); then
    perf_compare_cmd+=(--require-completion-present-wait-without-enqueue-decrease)
  fi
  if (( require_commit_chunk_replay_cpu_per_present_decrease )); then
    perf_compare_cmd+=(--require-commit-chunk-replay-cpu-per-present-decrease)
  fi
  if (( require_queue_draw_submission_cpu_per_present_decrease )); then
    perf_compare_cmd+=(--require-queue-draw-submission-cpu-per-present-decrease)
  fi
  if (( require_snapshot_cpu_per_present_decrease )); then
    perf_compare_cmd+=(--require-snapshot-cpu-per-present-decrease)
  fi
  if (( require_snapshot_cache_lookup_cpu_per_present_decrease )); then
    perf_compare_cmd+=(--require-snapshot-cache-lookup-cpu-per-present-decrease)
  fi
  if (( require_snapshot_cache_uniform_build_cpu_per_present_decrease )); then
    perf_compare_cmd+=(--require-snapshot-cache-uniform-build-cpu-per-present-decrease)
  fi
  if (( require_snapshot_cache_uniform_hash_cpu_per_present_decrease )); then
    perf_compare_cmd+=(--require-snapshot-cache-uniform-hash-cpu-per-present-decrease)
  fi
  if (( require_batch_miss_uniform_build_cpu_per_present_decrease )); then
    perf_compare_cmd+=(--require-batch-miss-uniform-build-cpu-per-present-decrease)
  fi
  if (( require_batch_miss_uniform_hash_cpu_per_present_decrease )); then
    perf_compare_cmd+=(--require-batch-miss-uniform-hash-cpu-per-present-decrease)
  fi
  if (( require_batch_miss_vs_const_hash_cpu_per_present_decrease )); then
    perf_compare_cmd+=(--require-batch-miss-vs-const-hash-cpu-per-present-decrease)
  fi
  if (( require_batch_miss_ps_const_hash_cpu_per_present_decrease )); then
    perf_compare_cmd+=(--require-batch-miss-ps-const-hash-cpu-per-present-decrease)
  fi
  if (( require_batch_miss_nonconst_hash_cpu_per_present_decrease )); then
    perf_compare_cmd+=(--require-batch-miss-nonconst-hash-cpu-per-present-decrease)
  fi
  if (( require_snapshot_uniform_copy_cpu_per_present_decrease )); then
    perf_compare_cmd+=(--require-snapshot-uniform-copy-cpu-per-present-decrease)
  fi
  if (( require_submit_draw_run_batch_append_uniform_cpu_per_present_decrease )); then
    perf_compare_cmd+=(--require-submit-draw-run-batch-append-uniform-cpu-per-present-decrease)
  fi
  if (( require_draw_uniform_payload_lookup_cpu_per_present_decrease )); then
    perf_compare_cmd+=(--require-draw-uniform-payload-lookup-cpu-per-present-decrease)
  fi
  if (( require_draw_uniform_payload_append_copy_cpu_per_present_decrease )); then
    perf_compare_cmd+=(--require-draw-uniform-payload-append-copy-cpu-per-present-decrease)
  fi
  if (( require_argbuf_setup_cpu_per_present_decrease )); then
    perf_compare_cmd+=(--require-argbuf-setup-cpu-per-present-decrease)
  fi
  if (( require_argbuf_open_cpu_per_present_decrease )); then
    perf_compare_cmd+=(--require-argbuf-open-cpu-per-present-decrease)
  fi
  if (( require_argbuf_cbuf_update_cpu_per_present_decrease )); then
    perf_compare_cmd+=(--require-argbuf-cbuf-update-cpu-per-present-decrease)
  fi
  if (( require_argbuf_cbuf_update_vs_cpu_per_present_decrease )); then
    perf_compare_cmd+=(--require-argbuf-cbuf-update-vs-cpu-per-present-decrease)
  fi
  if (( require_uniform_compact_saved_bytes_present )); then
    perf_compare_cmd+=(--require-uniform-compact-saved-bytes-present)
  fi
  if (( require_encode_chunk_cpu_per_present_decrease )); then
    perf_compare_cmd+=(--require-encode-chunk-cpu-per-present-decrease)
  fi
  if (( require_no_enqueue_commit_entry_to_publish_decrease )); then
    perf_compare_cmd+=(--require-no-enqueue-commit-entry-to-publish-decrease)
  fi
  if (( require_no_enqueue_publish_to_encode_dequeue_decrease )); then
    perf_compare_cmd+=(--require-no-enqueue-publish-to-encode-dequeue-decrease)
  fi
  if (( require_no_enqueue_encode_dequeue_to_commit_decrease )); then
    perf_compare_cmd+=(--require-no-enqueue-encode-dequeue-to-commit-decrease)
  fi
  if (( require_no_enqueue_wait_to_next_enqueue_decrease )); then
    perf_compare_cmd+=(--require-no-enqueue-wait-to-next-enqueue-decrease)
  fi
  if [[ -n "$max_gpu_command_buffer_regression_ms" ]]; then
    perf_compare_cmd+=(--max-gpu-command-buffer-regression-ms "$max_gpu_command_buffer_regression_ms")
  fi
  if [[ -n "$max_const_upload_break_count_ratio" ]]; then
    perf_compare_cmd+=(--max-const-upload-break-count-ratio "$max_const_upload_break_count_ratio")
  fi
fi

xcode_compare_cmd=()
if [[ -n "$baseline_joined" ]]; then
  xcode_compare_cmd=(
    python3 scripts/tools/compare_xcode_dxmt_bottlenecks.py
    "$baseline_joined"
    "$joined_csv"
    --before-label "$before_label"
    --after-label "$after_label"
    --output "$xcode_compare_report"
    --top "$top_n"
  )
  if (( require_top_gpu_decrease )); then
    xcode_compare_cmd+=(--require-top-gpu-decrease)
  fi
  if (( require_top_buffer_write_decrease )); then
    xcode_compare_cmd+=(--require-top-buffer-write-decrease)
  fi
  if (( require_top_vs_buffer_write_decrease )); then
    xcode_compare_cmd+=(--require-top-vs-buffer-write-decrease)
  fi
  if (( require_top_unexplained_buffer_write_decrease )); then
    xcode_compare_cmd+=(--require-top-unexplained-buffer-write-decrease)
  fi
  if (( require_stream_handle_churn_decrease )); then
    xcode_compare_cmd+=(--require-stream-handle-churn-decrease)
  fi
  if (( require_ib_handle_churn_decrease )); then
    xcode_compare_cmd+=(--require-ib-handle-churn-decrease)
  fi
  if (( require_argbuf_cbuf_decrease )); then
    xcode_compare_cmd+=(--require-argbuf-cbuf-decrease)
  fi
  if (( require_transient_decrease )); then
    xcode_compare_cmd+=(--require-transient-decrease)
  fi
  if (( require_top_gpu_share_increase )); then
    xcode_compare_cmd+=(--require-top-gpu-share-increase)
  fi
  if (( require_top_row_key_match )); then
    xcode_compare_cmd+=(--require-top-row-key-match)
  fi
  if (( require_stable_frame_proof )); then
    xcode_compare_cmd+=(--require-stable-frame-proof)
  fi
  if (( require_tvb_mechanism_proof )); then
    xcode_compare_cmd+=(--require-tvb-mechanism-proof)
  fi
  if (( require_target_index_cache_miss32_decrease )); then
    xcode_compare_cmd+=(--require-target-index-cache-miss32-decrease)
  fi
  if (( require_target_index_cache_opt_miss32_decrease )); then
    xcode_compare_cmd+=(--require-target-index-cache-opt-miss32-decrease)
  fi
  if (( require_target_reordered_index_cache_hits )); then
    xcode_compare_cmd+=(--require-target-reordered-index-cache-hits)
  fi
  if (( require_target_vs_buffer_write_decrease )); then
    xcode_compare_cmd+=(--require-target-vs-buffer-write-decrease)
  fi
  if (( require_target_vs_invocations_decrease )); then
    xcode_compare_cmd+=(--require-target-vs-invocations-decrease)
  fi
  if [[ -n "$max_top_gpu_regression_ms" ]]; then
    xcode_compare_cmd+=(--max-top-gpu-regression-ms "$max_top_gpu_regression_ms")
  fi
  if [[ -n "$max_top_buffer_write_regression_mib" ]]; then
    xcode_compare_cmd+=(
      --max-top-buffer-write-regression-mib
      "$max_top_buffer_write_regression_mib"
    )
  fi
  for row_key in "${target_row_keys[@]}"; do
    xcode_compare_cmd+=(--target-row-key "$row_key")
  done
  if [[ -n "$max_non_target_gpu_regression_ms" ]]; then
    xcode_compare_cmd+=(
      --max-non-target-gpu-regression-ms
      "$max_non_target_gpu_regression_ms"
    )
  fi
  if [[ -n "$max_non_target_vs_buffer_write_regression_mib" ]]; then
    xcode_compare_cmd+=(
      --max-non-target-vs-buffer-write-regression-mib
      "$max_non_target_vs_buffer_write_regression_mib"
    )
  fi
  if [[ -n "$max_non_target_vs_invocations_regression_ratio" ]]; then
    xcode_compare_cmd+=(
      --max-non-target-vs-invocations-regression-ratio
      "$max_non_target_vs_invocations_regression_ratio"
    )
  fi
  if [[ -n "$max_non_target_draw_call_delta_ratio" ]]; then
    xcode_compare_cmd+=(
      --max-non-target-draw-call-delta-ratio
      "$max_non_target_draw_call_delta_ratio"
    )
  fi
  if [[ -n "$max_non_target_vertex_count_delta_ratio" ]]; then
    xcode_compare_cmd+=(
      --max-non-target-vertex-count-delta-ratio
      "$max_non_target_vertex_count_delta_ratio"
    )
  fi
  if [[ -n "$max_non_target_triangle_delta_ratio" ]]; then
    xcode_compare_cmd+=(
      --max-non-target-triangle-delta-ratio
      "$max_non_target_triangle_delta_ratio"
    )
  fi
  if [[ -n "$max_top_unexplained_buffer_write_ratio" ]]; then
    xcode_compare_cmd+=(
      --max-top-unexplained-buffer-write-ratio
      "$max_top_unexplained_buffer_write_ratio"
    )
  fi
  if [[ -n "$max_top_draw_call_delta_ratio" ]]; then
    xcode_compare_cmd+=(
      --max-top-draw-call-delta-ratio
      "$max_top_draw_call_delta_ratio"
    )
  fi
  if [[ -n "$max_top_vertex_count_delta_ratio" ]]; then
    xcode_compare_cmd+=(
      --max-top-vertex-count-delta-ratio
      "$max_top_vertex_count_delta_ratio"
    )
  fi
  if [[ -n "$max_top_triangle_delta_ratio" ]]; then
    xcode_compare_cmd+=(
      --max-top-triangle-delta-ratio
      "$max_top_triangle_delta_ratio"
    )
  fi
fi

semantic_image_compare_cmd=()
if (( semantic_image_compare_requested )); then
  semantic_image_compare_cmd=(
    python3 scripts/tools/compare_experiment_images.py
    --before "$semantic_image_before"
    --after "$semantic_image_after"
    --label-before "$before_label"
    --label-after "$after_label"
    --policy "$semantic_image_policy"
    --min-before-active-pct "$semantic_image_min_active_pct"
    --min-after-active-pct "$semantic_image_min_active_pct"
    --output "$semantic_image_output"
    --summary-output "$semantic_image_summary_output"
    --diff-output "$semantic_image_diff_output"
  )
fi

print_cmd() {
  local label=$1
  shift
  printf '%s:' "$label"
  printf ' %q' "$@"
  printf '\n'
}

run_cmd() {
  print_cmd "run" "$@"
  (
    cd "$repo_root"
    "$@"
  )
}

echo "run_id: $run_id"
echo "output_dir: $output_dir"
echo "trace_dir: $trace_dir"
echo "analysis_dir: $analysis_dir"
echo "xcode_csv: $xcode_csv"
echo "summary: $summary_path"
echo "encoder_csv: $encoders_csv"
echo "stream_csv: $stream_csv"
echo "probe_draws_csv: $probe_draws_csv"
echo "trace_artifacts_json: $trace_artifacts_json"
echo "index_cache_runtime_report: $index_cache_runtime_report"
echo "index_cache_runtime_csv: $index_cache_runtime_csv"
echo "class_proxy_report: $class_proxy_report"
echo "class_proxy_csv: $class_proxy_csv"
echo "joined_csv: $joined_csv"
echo "xcode_report: $xcode_report"
echo "top_n: $top_n"
echo "hot_gpu_share: $hot_gpu_share"
echo "class_proxy_top: $class_proxy_top"
echo "shader_msl_dir: $shader_msl_dir"
echo "shader_dump_report: $shader_dump_report"
echo "shader_dump_csv: $shader_dump_csv"
if ((${#perf_compare_cmd[@]})); then
  echo "perf_compare_report: $perf_compare_report"
fi
if ((${#xcode_compare_cmd[@]})); then
  echo "xcode_compare_report: $xcode_compare_report"
fi
if ((${#semantic_image_compare_cmd[@]})); then
  echo "semantic_image_report: $semantic_image_output"
  echo "semantic_image_csv: $semantic_image_summary_output"
  echo "semantic_image_diff: $semantic_image_diff_output"
fi
print_cmd "summary_cmd" "${summary_cmd[@]}"
print_cmd "index_cache_runtime_cmd" "${index_cache_runtime_cmd[@]}"
print_cmd "xcode_summary_cmd" "${xcode_summary_cmd[@]}"
print_cmd "class_proxy_cmd" "${class_proxy_cmd[@]}"
print_cmd "shader_dump_cmd" "${shader_dump_cmd[@]}"
if ((${#perf_compare_cmd[@]})); then
  print_cmd "perf_compare_cmd" "${perf_compare_cmd[@]}"
fi
if ((${#xcode_compare_cmd[@]})); then
  print_cmd "xcode_compare_cmd" "${xcode_compare_cmd[@]}"
fi
if ((${#semantic_image_compare_cmd[@]})); then
  print_cmd "semantic_image_compare_cmd" "${semantic_image_compare_cmd[@]}"
fi

if (( dry_run )); then
  exit 0
fi

if [[ ! -f "$output_dir/result.json" ]]; then
  if (( require_result_json )); then
    echo "missing required result.json: $output_dir/result.json" >&2
    exit 2
  fi
  if [[ -n "$baseline_output" ]]; then
    echo "missing result.json for run-level comparison: $output_dir/result.json" >&2
    exit 2
  fi
  if [[ ! -f "$output_dir/dxmt9.log" ]]; then
    echo "missing result.json and dxmt9.log: $output_dir/result.json" >&2
    exit 2
  fi
  echo "warning: missing result.json; using dxmt9.log partial-run counters" >&2
fi
if [[ ! -f "$xcode_csv" ]]; then
  echo "missing Xcode encoder counters CSV: $xcode_csv" >&2
  echo "export it from Xcode Counters > Export Encoder Counters first" >&2
  exit 2
fi

mkdir -p "$analysis_dir"
run_cmd "${summary_cmd[@]}"
run_cmd "${index_cache_runtime_cmd[@]}"
run_cmd "${xcode_summary_cmd[@]}"
if [[ -f "$probe_draws_csv" ]]; then
  run_cmd "${class_proxy_cmd[@]}"
else
  echo "warning: missing probe draw CSV; skipping class proxy report: $probe_draws_csv" >&2
fi
run_cmd "${shader_dump_cmd[@]}"
if ((${#perf_compare_cmd[@]})); then
  run_cmd "${perf_compare_cmd[@]}"
fi
if ((${#xcode_compare_cmd[@]})); then
  run_cmd "${xcode_compare_cmd[@]}"
fi
if ((${#semantic_image_compare_cmd[@]})); then
  run_cmd "${semantic_image_compare_cmd[@]}"
fi

python3 - "$trace_artifacts_json" \
  "$run_id" \
  "$output_dir" \
  "$trace_dir" \
  "$analysis_dir" \
  "$frame" \
  "$capture_path" \
  "$xcode_performance_gputrace" \
  "$xcode_csv" \
  "$xcode_summary_csv" \
  "$joined_csv" \
  "$xcode_report" <<'PY'
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
    capture_path,
    performance_gputrace,
    counters_csv,
    counters_summary_csv,
    joined_summary_csv,
    bottleneck_report,
) = sys.argv[1:]

out_path = pathlib.Path(out)
if out_path.exists():
    try:
        payload = json.loads(out_path.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        payload = {}
else:
    payload = {}

destination = payload.get("metal_capture_destination") or "gpuTraceDocument"
capture_enabled = bool(payload.get("capture_gputrace", pathlib.Path(capture_path).exists()))
direct_file_expected = payload.get(
    "direct_gputrace_file_expected",
    capture_enabled and destination not in {"developerTools", "xcode"},
)
paths = dict(payload.get("paths") or {})
paths.update({
    "output_dir": output_dir,
    "trace_dir": trace_dir,
    "analysis_dir": analysis_dir,
    "gputrace": capture_path if direct_file_expected else paths.get("gputrace"),
    "xcode_performance_gputrace": performance_gputrace,
    "xcode_encoder_counters_csv": counters_csv,
    "xcode_counters_summary_csv": counters_summary_csv,
    "xcode_dxmt_joined_summary_csv": joined_summary_csv,
    "xcode_dxmt_bottleneck_report": bottleneck_report,
})
paths = {key: value for key, value in paths.items() if value}
payload.update({
    "run_id": payload.get("run_id") or run_id,
    "frame": int(frame),
    "capture_gputrace": capture_enabled,
    "metal_capture_destination": destination,
    "direct_gputrace_file_expected": bool(direct_file_expected),
    "paths": paths,
    "exists": {key: pathlib.Path(value).exists() for key, value in paths.items()},
})

out_path.parent.mkdir(parents=True, exist_ok=True)
out_path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
print(out)
PY

echo "wrote summary: $summary_path"
echo "wrote encoder csv: $encoders_csv"
echo "wrote stream csv: $stream_csv"
echo "wrote probe draw csv: $probe_draws_csv"
echo "wrote trace artifacts manifest: $trace_artifacts_json"
echo "wrote index cache runtime report: $index_cache_runtime_report"
echo "wrote index cache runtime csv: $index_cache_runtime_csv"
if [[ -f "$class_proxy_csv" ]]; then
  echo "wrote class proxy report: $class_proxy_report"
  echo "wrote class proxy csv: $class_proxy_csv"
fi
echo "wrote xcode summary csv: $xcode_summary_csv"
echo "wrote joined csv: $joined_csv"
echo "wrote xcode report: $xcode_report"
echo "wrote shader dump report: $shader_dump_report"
echo "wrote shader dump csv: $shader_dump_csv"
if ((${#perf_compare_cmd[@]})); then
  echo "wrote perf comparison: $perf_compare_report"
fi
if ((${#xcode_compare_cmd[@]})); then
  echo "wrote xcode comparison: $xcode_compare_report"
fi
if ((${#semantic_image_compare_cmd[@]})); then
  echo "wrote semantic image comparison: $semantic_image_output"
  echo "wrote semantic image summary csv: $semantic_image_summary_output"
  echo "wrote semantic image diff: $semantic_image_diff_output"
fi
