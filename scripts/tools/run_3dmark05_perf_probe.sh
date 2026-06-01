#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)

frame=${DXMT_3DMARK05_PROBE_FRAME:-60}
timeout=${DXMT_3DMARK05_PROBE_TIMEOUT:-180}
suffix=${DXMT_3DMARK05_PROBE_SUFFIX:-}
result_file=${DXMT_3DMARK05_RESULT_FILE:-dxmt9_gt1.3dr}
capture_gputrace=1
dry_run=0
dump_shaders=0
trim_unused_varyings=0
trim_vertex_temps=0
trim_vs_output_scratch=0
split_sparse_const_records=0
aggressive_color_dontcare=0
aggressive_depth_dontcare=0
disable_cull=0
disable_scissor=0
force_visible=0
compare_baseline_output=${DXMT_3DMARK05_COMPARE_BASELINE_OUTPUT:-}
compare_baseline_joined=${DXMT_3DMARK05_COMPARE_BASELINE_JOINED:-}
require_color_dontcare_increase=0
require_depth_dontcare_increase=0
require_tile_preservation_decrease=0
require_draw_run_records_increase=0
require_draw_run_records_per_submit_increase=0
require_binding_overrides_present=0
require_const_upload_passthrough_present=0
require_draw_submission_batch_present=0
require_const_upload_break_bytes_decrease=0
require_encode_draw_cpu_decrease=0
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
require_top_pso_attribution=0
require_xcode_counter_coverage=0
require_dxmt_join_coverage=0
require_shader_dump_matches=0
min_top_pso_samples_per_draw=${DXMT_3DMARK05_MIN_TOP_PSO_SAMPLES_PER_DRAW:-0.90}
min_top_dxmt_joined_fraction=${DXMT_3DMARK05_MIN_TOP_DXMT_JOINED_FRACTION:-1.0}
max_top_gpu_regression_ms=${DXMT_3DMARK05_MAX_TOP_GPU_REGRESSION_MS:-}
max_top_buffer_write_regression_mib=${DXMT_3DMARK05_MAX_TOP_BUFFER_WRITE_REGRESSION_MIB:-}
max_top_unexplained_buffer_write_ratio=${DXMT_3DMARK05_MAX_TOP_UNEXPLAINED_BUFFER_WRITE_RATIO:-}
min_free_mb=${DXMT_3DMARK05_MIN_TRACE_FREE_MB:-}

usage() {
  cat <<'USAGE'
Usage: scripts/tools/run_3dmark05_perf_probe.sh [options]

Run the standard 3DMark05 GT1 perf probe with dxmt9 encoder breakdown,
optional Metal frame capture, and post-run summary CSV generation.

Options:
  --suffix NAME       Output suffix (default: probe-<timestamp>-frame<N>)
  --frame N           1-based Metal capture frame (default: 60)
  --timeout SEC       run_experiment timeout seconds (default: 180)
  --result-file NAME  3DMark05 result filename argument (default: dxmt9_gt1.3dr)
  --no-gputrace       Do not set DXMT_METAL_CAPTURE_FRAME/PATH
  --dump-shaders      Dump translated MSL and D3D shader bytecode under
                      traces/<run-id>/analysis/shaders
  --trim-unused-varyings
                      Set DXMT9_TRIM_UNUSED_VARYINGS=1 for pair-local VSOut
                      liveness/VS buffer-write experiments
  --trim-vertex-temps
                      Set DXMT9_TRIM_VERTEX_TEMPS=1 for translated VS temp
                      register/spill experiments
  --trim-vs-output-scratch
                      Set DXMT9_TRIM_VS_OUTPUT_SCRATCH=1 to size translated
                      VS outTexcoord[] scratch to observed output usage
  --split-sparse-const-records
                      Set DXMT9_SPLIT_SPARSE_CONST_RECORDS=1 for sparse
                      constant-upload record splitting experiments
  --aggressive-color-dontcare
                      Set DXMT9_AGGRESSIVE_COLOR_DONTCARE=1 for the run
  --aggressive-depth-dontcare
                      Set DXMT9_AGGRESSIVE_DEPTH_DONTCARE=1 for the run
  --disable-cull      Set DXMT_DISABLE_CULL=1 for render-state shape A/B
  --disable-scissor   Set DXMT_DISABLE_SCISSOR=1 for render-state shape A/B
  --force-visible     Set DXMT_DEBUG_FORCE_VISIBLE=1 for visibility/state A/B
  --compare-baseline-output PATH
                      After the run, compare result.json counters against this baseline output dir/result.json
  --baseline-joined PATH
                      Include this Xcode+dxmt joined CSV in the printed finalizer command
                      for after-Xcode counter comparison
  --require-color-dontcare-increase
                      Compare gate: candidate color StoreActionDontCare count must increase
  --require-depth-dontcare-increase
                      Compare gate: candidate depth StoreActionDontCare count must increase
  --require-tile-preservation-decrease
                      Compare gate: candidate tile-preservation MiB must decrease
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
  --max-gpu-command-buffer-regression-ms N
                      Compare gate: max allowed gpu_command_buffer_time_ms regression
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
  --max-top-unexplained-buffer-write-ratio N
                      Finalizer Xcode gate: max allowed unexplained top-N
                      buffer write / buffer write ratio
  --min-free-mb N     Required free space before launch (default: 2048 with gputrace, 256 without)
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
    --result-file)
      result_file=${2:?missing value for --result-file}
      shift 2
      ;;
    --no-gputrace)
      capture_gputrace=0
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
    --force-visible)
      force_visible=1
      shift
      ;;
    --compare-baseline-output)
      compare_baseline_output=${2:?missing value for --compare-baseline-output}
      shift 2
      ;;
    --baseline-joined)
      compare_baseline_joined=${2:?missing value for --baseline-joined}
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
    --max-top-unexplained-buffer-write-ratio)
      max_top_unexplained_buffer_write_ratio=${2:?missing value for --max-top-unexplained-buffer-write-ratio}
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

if [[ ! "$timeout" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
  echo "--timeout must be numeric seconds" >&2
  exit 2
fi

if [[ -z "$suffix" ]]; then
  suffix="probe-$(date +%Y%m%d-%H%M%S)-frame${frame}"
fi

if [[ -z "$min_free_mb" ]]; then
  if (( capture_gputrace )); then
    min_free_mb=2048
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

xcode_compare_requested=0
if (( require_top_gpu_decrease ||
      require_top_buffer_write_decrease ||
      require_top_vs_buffer_write_decrease ||
      require_top_unexplained_buffer_write_decrease ||
      require_stream_handle_churn_decrease ||
      require_ib_handle_churn_decrease ||
      require_argbuf_cbuf_decrease ||
      require_transient_decrease ||
      require_top_gpu_share_increase )) ||
   [[ -n "$max_top_gpu_regression_ms" ||
      -n "$max_top_buffer_write_regression_mib" ||
      -n "$max_top_unexplained_buffer_write_ratio" ]]; then
  xcode_compare_requested=1
fi

run_level_compare_requested=0
if (( require_color_dontcare_increase ||
      require_depth_dontcare_increase ||
      require_tile_preservation_decrease ||
      require_draw_run_records_increase ||
      require_draw_run_records_per_submit_increase ||
      require_binding_overrides_present ||
      require_const_upload_passthrough_present ||
      require_draw_submission_batch_present ||
      require_const_upload_break_bytes_decrease ||
      require_encode_draw_cpu_decrease )) ||
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

print_space_hints() {
  local stream=${1:-2}
  echo "space usage hints:" >&"$stream"
  if command -v du >/dev/null 2>&1; then
    du -sh \
      "$repo_root/traces" \
      "$repo_root/experiments/output" \
      "$repo_root/build-x86_64-builtin" 2>/dev/null |
      sort -hr >&"$stream" || true
  fi
  if command -v find >/dev/null 2>&1; then
    echo "large trace/output files:" >&"$stream"
    find "$repo_root/traces" "$repo_root/experiments/output" \
      -type f -size +50M -exec ls -lh {} \; 2>/dev/null |
      sort -k5 -hr |
      head -20 >&"$stream" || true
  fi
}

run_id="app-d3d9-3dmark05-${suffix}"
output_dir="$repo_root/experiments/output/$run_id"
trace_dir="$repo_root/traces/$run_id"
analysis_dir="$trace_dir/analysis"
shader_dump_dir="$analysis_dir/shaders"
shader_msl_dump_dir="$shader_dump_dir/msl"
shader_bytecode_dump_dir="$shader_dump_dir/bytecode"
summary_path="$output_dir/3dmark05-perf-summary.md"
capture_path="$trace_dir/frame${frame}.gputrace"
counter_comparison_path="$analysis_dir/frame${frame}-perf-counter-comparison.md"
free_mb=unknown
if command -v df >/dev/null 2>&1; then
  free_kb=$(df -Pk "$repo_root" | awk 'NR==2 {print $4}')
  if [[ "$free_kb" =~ ^[0-9]+$ ]]; then
    free_mb=$(( free_kb / 1024 ))
  fi
fi

session_locked=unknown
if command -v ioreg >/dev/null 2>&1; then
  session_state=$(ioreg -n Root -d1 2>/dev/null || true)
  if [[ "$session_state" == *'"CGSSessionScreenIsLocked"=Yes'* ]]; then
    session_locked=yes
  else
    session_locked=no
  fi
fi

env_args=(
  "DXMT_EXPERIMENT_PROFILE=perf"
  "DXMT_3DMARK05_DIRECT=1"
  "DXMT_DISABLE_AUTO_EXPAND_INDEXED=1"
  "DXMT9_PERF_ENCODER_BREAKDOWN=1"
  "DXMT_3DMARK05_RESULT_FILE=$result_file"
  "DXMT_3DMARK05_LOG=$output_dir/3dmark05-direct.log"
)

if (( capture_gputrace )); then
  env_args+=(
    "MTL_CAPTURE_ENABLED=1"
    "DXMT_METAL_CAPTURE_FRAME=$frame"
    "DXMT_METAL_CAPTURE_PATH=$capture_path"
  )
fi

if (( aggressive_color_dontcare )); then
  env_args+=("DXMT9_AGGRESSIVE_COLOR_DONTCARE=1")
fi

if (( trim_unused_varyings )); then
  env_args+=("DXMT9_TRIM_UNUSED_VARYINGS=1")
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

if (( aggressive_depth_dontcare )); then
  env_args+=("DXMT9_AGGRESSIVE_DEPTH_DONTCARE=1")
fi

if (( disable_cull )); then
  env_args+=("DXMT_DISABLE_CULL=1")
fi

if (( disable_scissor )); then
  env_args+=("DXMT_DISABLE_SCISSOR=1")
fi

if (( force_visible )); then
  env_args+=("DXMT_DEBUG_FORCE_VISIBLE=1")
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
  )
  if [[ -n "$compare_baseline_output" ]]; then
    finalize_cmd+=(--baseline-output "$compare_baseline_output")
  fi
  if [[ -n "$compare_baseline_joined" ]]; then
    finalize_cmd+=(--baseline-joined "$compare_baseline_joined")
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
  if (( require_color_dontcare_increase )); then
    finalize_cmd+=(--require-color-dontcare-increase)
  fi
  if (( require_depth_dontcare_increase )); then
    finalize_cmd+=(--require-depth-dontcare-increase)
  fi
  if (( require_tile_preservation_decrease )); then
    finalize_cmd+=(--require-tile-preservation-decrease)
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
  if [[ -n "$max_top_unexplained_buffer_write_ratio" ]]; then
    finalize_cmd+=(
      --max-top-unexplained-buffer-write-ratio
      "$max_top_unexplained_buffer_write_ratio"
    )
  fi
fi

echo "run_id: $run_id"
echo "output_dir: $output_dir"
echo "trace_dir: $trace_dir"
echo "summary: $summary_path"
echo "session_locked: $session_locked"
echo "free_space_mb: $free_mb"
echo "min_free_space_mb: $min_free_mb"
if (( capture_gputrace )); then
  echo "gputrace: $capture_path"
else
  echo "gputrace: disabled"
fi
if (( dump_shaders )); then
  echo "shader_dump_dir: $shader_dump_dir"
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

if (( dry_run )); then
  if [[ "$free_mb" != unknown && "$min_free_mb" != 0 && "$free_mb" -lt "$min_free_mb" ]]; then
    echo "dry-run: free space is below the launch guard; cleanup candidates follow"
    print_space_hints 1
  fi
  exit 0
fi

if [[ "${DXMT_3DMARK05_REQUIRE_UNLOCKED:-1}" != "0" && "$session_locked" == yes ]]; then
  echo "macOS session is locked; unlock the desktop before running 3DMark05 perf/gputrace" >&2
  exit 2
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

(
  cd "$repo_root"
  env "${env_args[@]}" "${cmd[@]}"
)

python3 "$repo_root/scripts/tools/summarize_3dmark05_perf.py" "$output_dir" --output "$summary_path"

if (( capture_gputrace )) && [[ ! -e "$capture_path" ]]; then
  echo "Metal gputrace capture was requested but no capture was written: $capture_path" >&2
  echo "check the run log for MTLCaptureManager errors:" >&2
  echo "  $output_dir/dxmt9.log" >&2
  echo "  $output_dir/3dmark05-direct.log" >&2
  exit 2
fi

if ((${#counter_compare_cmd[@]})); then
  mkdir -p "$trace_dir/analysis"
  (
    cd "$repo_root"
    "${counter_compare_cmd[@]}"
  )
fi

echo "wrote summary: $summary_path"
echo "wrote encoder csv: $output_dir/3dmark05-perf-encoders.csv"
echo "wrote stream csv: $output_dir/3dmark05-perf-encoder-streams.csv"
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
