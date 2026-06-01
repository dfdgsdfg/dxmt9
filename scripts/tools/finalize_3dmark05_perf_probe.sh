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
dry_run=0

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
require_shader_dump_matches=${DXMT_3DMARK05_REQUIRE_SHADER_DUMP_MATCHES:-0}
min_top_pso_samples_per_draw=${DXMT_3DMARK05_MIN_TOP_PSO_SAMPLES_PER_DRAW:-0.90}
min_top_dxmt_joined_fraction=${DXMT_3DMARK05_MIN_TOP_DXMT_JOINED_FRACTION:-1.0}
max_top_gpu_regression_ms=${DXMT_3DMARK05_MAX_TOP_GPU_REGRESSION_MS:-}
max_top_buffer_write_regression_mib=${DXMT_3DMARK05_MAX_TOP_BUFFER_WRITE_REGRESSION_MIB:-}
max_top_unexplained_buffer_write_ratio=${DXMT_3DMARK05_MAX_TOP_UNEXPLAINED_BUFFER_WRITE_RATIO:-}

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
  --require-color-dontcare-increase
  --require-depth-dontcare-increase
  --require-tile-preservation-decrease
  --require-draw-run-records-increase
  --require-draw-run-records-per-submit-increase
  --require-binding-overrides-present
  --require-const-upload-passthrough-present
  --require-draw-submission-batch-present
  --require-const-upload-break-bytes-decrease
  --max-const-upload-break-count-ratio N
  --require-encode-draw-cpu-decrease
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
  --max-top-unexplained-buffer-write-ratio N
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
validate_optional_number "--max-top-unexplained-buffer-write-ratio" "$max_top_unexplained_buffer_write_ratio"

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
      require_top_gpu_share_increase )) ||
   [[ -n "$max_top_gpu_regression_ms" ||
      -n "$max_top_buffer_write_regression_mib" ||
      -n "$max_top_unexplained_buffer_write_ratio" ]]; then
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
xcode_summary_csv="$analysis_dir/frame${frame}-counters-summary.csv"
joined_csv="$analysis_dir/frame${frame}-xcode-dxmt-joined-summary.csv"
xcode_report="$analysis_dir/frame${frame}-xcode-dxmt-bottleneck-report.md"
shader_msl_dir="$analysis_dir/shaders/msl"
shader_dump_report="$analysis_dir/frame${frame}-shader-dump-report.md"
shader_dump_csv="$analysis_dir/frame${frame}-shader-dump-summary.csv"
perf_compare_report="$analysis_dir/frame${frame}-perf-counter-comparison.md"
xcode_compare_report="$analysis_dir/frame${frame}-xcode-dxmt-comparison.md"

summary_cmd=(
  python3 scripts/tools/summarize_3dmark05_perf.py
  "$output_dir"
  --output "$summary_path"
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
  if [[ -n "$max_top_gpu_regression_ms" ]]; then
    xcode_compare_cmd+=(--max-top-gpu-regression-ms "$max_top_gpu_regression_ms")
  fi
  if [[ -n "$max_top_buffer_write_regression_mib" ]]; then
    xcode_compare_cmd+=(
      --max-top-buffer-write-regression-mib
      "$max_top_buffer_write_regression_mib"
    )
  fi
  if [[ -n "$max_top_unexplained_buffer_write_ratio" ]]; then
    xcode_compare_cmd+=(
      --max-top-unexplained-buffer-write-ratio
      "$max_top_unexplained_buffer_write_ratio"
    )
  fi
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
echo "joined_csv: $joined_csv"
echo "xcode_report: $xcode_report"
echo "shader_msl_dir: $shader_msl_dir"
echo "shader_dump_report: $shader_dump_report"
echo "shader_dump_csv: $shader_dump_csv"
if ((${#perf_compare_cmd[@]})); then
  echo "perf_compare_report: $perf_compare_report"
fi
if ((${#xcode_compare_cmd[@]})); then
  echo "xcode_compare_report: $xcode_compare_report"
fi
print_cmd "summary_cmd" "${summary_cmd[@]}"
print_cmd "xcode_summary_cmd" "${xcode_summary_cmd[@]}"
print_cmd "shader_dump_cmd" "${shader_dump_cmd[@]}"
if ((${#perf_compare_cmd[@]})); then
  print_cmd "perf_compare_cmd" "${perf_compare_cmd[@]}"
fi
if ((${#xcode_compare_cmd[@]})); then
  print_cmd "xcode_compare_cmd" "${xcode_compare_cmd[@]}"
fi

if (( dry_run )); then
  exit 0
fi

if [[ ! -f "$output_dir/result.json" ]]; then
  echo "missing result.json: $output_dir/result.json" >&2
  exit 2
fi
if [[ ! -f "$xcode_csv" ]]; then
  echo "missing Xcode encoder counters CSV: $xcode_csv" >&2
  echo "export it from Xcode Counters > Export Encoder Counters first" >&2
  exit 2
fi

mkdir -p "$analysis_dir"
run_cmd "${summary_cmd[@]}"
run_cmd "${xcode_summary_cmd[@]}"
run_cmd "${shader_dump_cmd[@]}"
if ((${#perf_compare_cmd[@]})); then
  run_cmd "${perf_compare_cmd[@]}"
fi
if ((${#xcode_compare_cmd[@]})); then
  run_cmd "${xcode_compare_cmd[@]}"
fi

echo "wrote summary: $summary_path"
echo "wrote encoder csv: $encoders_csv"
echo "wrote stream csv: $stream_csv"
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
