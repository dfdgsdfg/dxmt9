#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)

wrapper="$repo_root/scripts/tools/run_3dmark05_perf_probe.sh"
record_delay_sec=${DXMT_3DMARK05_SYSTEM_TRACE_DELAY_SEC:-75}
record_time_limit_sec=${DXMT_3DMARK05_SYSTEM_TRACE_TIME_LIMIT_SEC:-25}
summary_top=${DXMT_3DMARK05_SYSTEM_TRACE_TOP:-30}
wait_unlocked_sec=${DXMT_3DMARK05_SYSTEM_TRACE_WAIT_UNLOCKED_SEC:-0}
wait_unlocked_interval_sec=${DXMT_3DMARK05_SYSTEM_TRACE_WAIT_UNLOCKED_INTERVAL_SEC:-5}
dry_run=0
allow_gputrace=0
skip_export_summary=0
encoder_breakdown_seq_range=
xctrace_prefix=(xcrun xctrace)

usage() {
  cat <<'EOF'
usage: run_3dmark05_system_trace_sidecar.sh [sidecar options] -- PROBE_ARGS...

Run a normal no-gputrace 3DMark05 perf probe and record an all-processes
Metal System Trace sidecar during GT1. The script first runs the perf wrapper
with --dry-run, refuses locked sessions before starting xctrace, and then joins
the exported metal-gpu-intervals table with dxmt9 encoder telemetry.

Sidecar options:
  --record-delay-sec SEC       Delay after launching the probe before xctrace
                               starts. Default: 75.
  --time-limit-sec SEC         xctrace record time limit. Default: 25.
  --summary-top N              Rows to include in the sidecar summary. Default: 30.
  --encoder-breakdown-seq-range MIN:MAX
                               Use a bounded dxmt encoder-breakdown seq window
                               instead of all-frame breakdown. The window must
                               cover the xctrace RenderPass seq range or the
                               join coverage gate will fail.
  --wait-unlocked-sec SEC      Poll dry-run preflight until the macOS session
                               unlocks, then launch. Default: 0 (no wait).
  --wait-unlocked-interval-sec SEC
                               Poll interval for --wait-unlocked-sec. Default: 5.
  --wrapper PATH               Probe wrapper path. Default:
                               scripts/tools/run_3dmark05_perf_probe.sh.
  --xctrace-bin PATH           xctrace-compatible command for tests.
                               Default: xcrun xctrace.
  --allow-gputrace             Allow probe args that request .gputrace capture.
                               Not recommended for 3DMark05.
  --skip-export-summary        Record only; do not export/summarize.
  --dry-run                    Print resolved plan without launching.
  -h, --help                   Show this help.

Example:
  bash scripts/tools/run_3dmark05_system_trace_sidecar.sh \
    --record-delay-sec 75 --time-limit-sec 25 -- \
    --suffix systemtrace-indexed-r1 --frame 60 --no-gputrace \
    --timeout 120 --measure-index-reuse --frame-sampling
EOF
}

fail() {
  printf 'run_3dmark05_system_trace_sidecar: %s\n' "$*" >&2
  exit 2
}

is_non_negative_number() {
  [[ "$1" =~ ^[0-9]+([.][0-9]+)?$ ]]
}

is_positive_number() {
  is_non_negative_number "$1" && awk -v value="$1" 'BEGIN { exit !(value > 0) }'
}

is_positive_int() {
  [[ "$1" =~ ^[1-9][0-9]*$ ]]
}

is_non_negative_int() {
  [[ "$1" =~ ^[0-9]+$ ]]
}

while (($#)); do
  case "$1" in
    --record-delay-sec)
      (($# >= 2)) || fail "--record-delay-sec requires a value"
      record_delay_sec=$2
      shift 2
      ;;
    --time-limit-sec)
      (($# >= 2)) || fail "--time-limit-sec requires a value"
      record_time_limit_sec=$2
      shift 2
      ;;
    --summary-top)
      (($# >= 2)) || fail "--summary-top requires a value"
      summary_top=$2
      shift 2
      ;;
    --encoder-breakdown-seq-range)
      (($# >= 2)) || fail "--encoder-breakdown-seq-range requires a value"
      encoder_breakdown_seq_range=$2
      shift 2
      ;;
    --wait-unlocked-sec)
      (($# >= 2)) || fail "--wait-unlocked-sec requires a value"
      wait_unlocked_sec=$2
      shift 2
      ;;
    --wait-unlocked-interval-sec)
      (($# >= 2)) || fail "--wait-unlocked-interval-sec requires a value"
      wait_unlocked_interval_sec=$2
      shift 2
      ;;
    --wrapper)
      (($# >= 2)) || fail "--wrapper requires a path"
      wrapper=$2
      shift 2
      ;;
    --xctrace-bin)
      (($# >= 2)) || fail "--xctrace-bin requires a path"
      xctrace_prefix=("$2")
      shift 2
      ;;
    --allow-gputrace)
      allow_gputrace=1
      shift
      ;;
    --skip-export-summary)
      skip_export_summary=1
      shift
      ;;
    --dry-run)
      dry_run=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    --)
      shift
      break
      ;;
    *)
      fail "unknown option: $1"
      ;;
  esac
done

(($# > 0)) || fail "missing PROBE_ARGS after --"
[[ -x "$wrapper" || -f "$wrapper" ]] || fail "missing wrapper: $wrapper"
is_non_negative_number "$record_delay_sec" ||
  fail "--record-delay-sec must be non-negative seconds"
is_positive_number "$record_time_limit_sec" ||
  fail "--time-limit-sec must be positive seconds"
is_positive_int "$summary_top" ||
  fail "--summary-top must be a positive integer"
if [[ -n "$encoder_breakdown_seq_range" &&
      ! "$encoder_breakdown_seq_range" =~ ^[0-9]+:[0-9]+$ ]]; then
  fail "--encoder-breakdown-seq-range expects MIN:MAX"
fi
if [[ -n "$encoder_breakdown_seq_range" ]]; then
  encoder_breakdown_seq_min=${encoder_breakdown_seq_range%%:*}
  encoder_breakdown_seq_max=${encoder_breakdown_seq_range#*:}
  (( encoder_breakdown_seq_min <= encoder_breakdown_seq_max )) ||
    fail "--encoder-breakdown-seq-range min must be <= max"
fi
is_non_negative_int "$wait_unlocked_sec" ||
  fail "--wait-unlocked-sec must be non-negative integer seconds"
is_positive_int "$wait_unlocked_interval_sec" ||
  fail "--wait-unlocked-interval-sec must be a positive integer"

probe_args=("$@")

probe_scopes_encoder_breakdown=0
probe_uses_all_frame_encoder_breakdown=0
probe_uses_range_encoder_breakdown=0
probe_disables_encoder_breakdown=0
for arg in "${probe_args[@]}"; do
  case "$arg" in
    --encoder-breakdown-seq|--encoder-breakdown-seq=*)
      probe_scopes_encoder_breakdown=1
      ;;
    --encoder-breakdown-seq-range|--encoder-breakdown-seq-range=*)
      probe_uses_range_encoder_breakdown=1
      ;;
    --encoder-breakdown-all-frames)
      probe_uses_all_frame_encoder_breakdown=1
      ;;
    --no-encoder-breakdown)
      probe_disables_encoder_breakdown=1
      ;;
  esac
done

if (( probe_disables_encoder_breakdown )); then
  fail "system trace sidecar requires encoder breakdown rows; remove --no-encoder-breakdown"
fi
if (( probe_scopes_encoder_breakdown )) ||
   [[ -n "${DXMT_3DMARK05_ENCODER_BREAKDOWN_SEQ:-}" ||
      -n "${DXMT9_PERF_ENCODER_BREAKDOWN_SEQ:-}" ]]; then
  fail "system trace sidecar requires all-frame or range encoder breakdown; remove scoped encoder-breakdown seq settings"
fi
if [[ -n "$encoder_breakdown_seq_range" ]] && (( probe_uses_range_encoder_breakdown )); then
  fail "pass --encoder-breakdown-seq-range either to the sidecar or the probe args, not both"
fi
if [[ -n "$encoder_breakdown_seq_range" ]] && (( probe_uses_all_frame_encoder_breakdown )); then
  fail "--encoder-breakdown-seq-range conflicts with --encoder-breakdown-all-frames"
fi
if (( probe_uses_range_encoder_breakdown && probe_uses_all_frame_encoder_breakdown )); then
  fail "--encoder-breakdown-seq-range conflicts with --encoder-breakdown-all-frames"
fi
if [[ -n "$encoder_breakdown_seq_range" ]]; then
  probe_args+=(--encoder-breakdown-seq-range "$encoder_breakdown_seq_range")
  probe_uses_range_encoder_breakdown=1
fi
system_trace_encoder_breakdown=all_frames
if (( probe_uses_range_encoder_breakdown )); then
  system_trace_encoder_breakdown=range
fi
if (( ! probe_uses_all_frame_encoder_breakdown && ! probe_uses_range_encoder_breakdown )); then
  probe_args+=(--encoder-breakdown-all-frames)
  probe_uses_all_frame_encoder_breakdown=1
fi

tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/dxmt9-system-trace-sidecar.XXXXXX")
dry_stdout="$tmp_dir/probe-dry-run.stdout"
dry_stderr="$tmp_dir/probe-dry-run.stderr"

cleanup_tmp() {
  /bin/rm -rf "$tmp_dir"
}
trap cleanup_tmp EXIT

run_probe_dry_run() {
  if ! bash "$wrapper" "${probe_args[@]}" --dry-run >"$dry_stdout" 2>"$dry_stderr"; then
    cat "$dry_stdout"
    cat "$dry_stderr" >&2
    fail "probe dry-run failed"
  fi
}

field() {
  awk -v key="$1" '
    index($0, key ": ") == 1 {
      sub("^[^:]+: ", "")
      print
      exit
    }
  ' "$dry_stdout"
}

load_preflight_fields() {
  run_id=$(field run_id)
  output_dir=$(field output_dir)
  trace_dir=$(field trace_dir)
  metal_system_trace=$(field metal_system_trace)
  metal_gpu_intervals_xml=$(field metal_gpu_intervals_xml)
  session_locked=$(field session_locked)
  gputrace=$(field gputrace)
  measure_index_reuse=$(field measure_index_reuse)

  [[ -n "$run_id" ]] || fail "dry-run output is missing run_id"
  [[ -n "$output_dir" ]] || fail "dry-run output is missing output_dir"
  [[ -n "$trace_dir" ]] || fail "dry-run output is missing trace_dir"
  [[ -n "$metal_system_trace" ]] || fail "dry-run output is missing metal_system_trace"
  [[ -n "$metal_gpu_intervals_xml" ]] || fail "dry-run output is missing metal_gpu_intervals_xml"
}

run_probe_dry_run
load_preflight_fields

if [[ "${DXMT_3DMARK05_REQUIRE_UNLOCKED:-1}" != "0" &&
      "$session_locked" == yes &&
      "$dry_run" -eq 0 &&
      "$wait_unlocked_sec" -gt 0 ]]; then
  waited_sec=0
  while [[ "$session_locked" == yes && "$waited_sec" -lt "$wait_unlocked_sec" ]]; do
    remaining_sec=$((wait_unlocked_sec - waited_sec))
    sleep_sec=$wait_unlocked_interval_sec
    if ((sleep_sec > remaining_sec)); then
      sleep_sec=$remaining_sec
    fi
    printf 'waiting for macOS session unlock: %ss/%ss\n' \
      "$waited_sec" "$wait_unlocked_sec" >&2
    sleep "$sleep_sec"
    waited_sec=$((waited_sec + sleep_sec))
    run_probe_dry_run
    load_preflight_fields
  done
fi

analysis_dir="$trace_dir/analysis"
encoders_csv="$output_dir/3dmark05-perf-encoders.csv"
probe_draws_csv="$output_dir/3dmark05-perf-indexed-probe-draws.csv"
xctrace_summary_csv="$analysis_dir/xctrace-metal-gpu-intervals-summary.csv"
xctrace_summary_md="$analysis_dir/xctrace-metal-gpu-intervals-summary.md"
preflight_log="$analysis_dir/system-trace-preflight.log"
preflight_err="$analysis_dir/system-trace-preflight.err"
wrapper_log="$analysis_dir/system-trace-wrapper.log"
xctrace_record_log="$analysis_dir/system-trace-record.log"
xctrace_export_log="$analysis_dir/system-trace-export.log"
xctrace_summary_log="$analysis_dir/system-trace-summary.log"
xctrace_time_limit_arg="${record_time_limit_sec}s"

cat "$dry_stdout"
printf 'system_trace_record_delay_sec: %s\n' "$record_delay_sec"
printf 'system_trace_time_limit_sec: %s\n' "$record_time_limit_sec"
if [[ "$system_trace_encoder_breakdown" == range ]]; then
  if [[ -n "$encoder_breakdown_seq_range" ]]; then
    printf 'system_trace_encoder_breakdown: range %s\n' "$encoder_breakdown_seq_range"
  else
    printf 'system_trace_encoder_breakdown: range probe_args\n'
  fi
else
  printf 'system_trace_encoder_breakdown: all_frames\n'
fi
printf 'system_trace_summary_top: %s\n' "$summary_top"
printf 'system_trace_wait_unlocked_sec: %s\n' "$wait_unlocked_sec"
printf 'system_trace_wait_unlocked_interval_sec: %s\n' "$wait_unlocked_interval_sec"
printf 'system_trace_wrapper_log: %s\n' "$wrapper_log"
printf 'system_trace_record_log: %s\n' "$xctrace_record_log"

if [[ "${DXMT_3DMARK05_REQUIRE_UNLOCKED:-1}" != "0" && "$session_locked" == yes ]]; then
  if [[ "$wait_unlocked_sec" -gt 0 && "$dry_run" -eq 0 ]]; then
    fail "macOS session is locked after waiting ${wait_unlocked_sec}s; unlock the desktop before running 3DMark05 system trace sidecar"
  fi
  fail "macOS session is locked; unlock the desktop before running 3DMark05 system trace sidecar"
fi

if (( ! allow_gputrace )) && [[ "$gputrace" != disabled ]]; then
  fail "system trace sidecar expects --no-gputrace; pass --allow-gputrace only for deliberate capture experiments"
fi

if [[ "$measure_index_reuse" != 1 ]]; then
  echo "note: --measure-index-reuse is not enabled; route verdicts will use encoder-summary route_* fields when the active provider supports them" >&2
fi

summary_cmd=(
  python3 "$repo_root/scripts/tools/summarize_xctrace_metal_intervals.py"
  --gpu-intervals "$metal_gpu_intervals_xml"
  --dxmt-encoders "$encoders_csv"
  --indexed-probe-draws "$probe_draws_csv"
  --output-csv "$xctrace_summary_csv"
  --output-md "$xctrace_summary_md"
  --run-label "$run_id"
  --trace "$metal_system_trace"
  --top "$summary_top"
  --require-xctrace-render-rows
  --min-dxmt-join-coverage 0.99
  --require-route-verdicts
)

printf 'system_trace_record_cmd:'
printf ' %q' "${xctrace_prefix[@]}" record \
  --template 'Metal System Trace' \
  --all-processes \
  --time-limit "$xctrace_time_limit_arg" \
  --no-prompt \
  --output "$metal_system_trace"
printf '\n'
printf 'system_trace_summary_cmd:'
printf ' %q' "${summary_cmd[@]}"
printf '\n'

if (( dry_run )); then
  exit 0
fi

mkdir -p "$analysis_dir"
cp "$dry_stdout" "$preflight_log"
cp "$dry_stderr" "$preflight_err"

/bin/rm -rf "$metal_system_trace"

wrapper_pid=
cleanup_children() {
  if [[ -n "$wrapper_pid" ]] && kill -0 "$wrapper_pid" 2>/dev/null; then
    kill "$wrapper_pid" 2>/dev/null || true
    wait "$wrapper_pid" 2>/dev/null || true
  fi
}
trap 'status=$?; cleanup_children; cleanup_tmp; exit "$status"' EXIT INT TERM

bash "$wrapper" "${probe_args[@]}" >"$wrapper_log" 2>&1 &
wrapper_pid=$!
printf 'system_trace_wrapper_pid: %s\n' "$wrapper_pid"

sleep "$record_delay_sec"
if ! kill -0 "$wrapper_pid" 2>/dev/null; then
  wait "$wrapper_pid" || wrapper_status=$?
  wrapper_status=${wrapper_status:-0}
  echo "probe exited before xctrace record started; see $wrapper_log" >&2
  exit "$wrapper_status"
fi

set +e
"${xctrace_prefix[@]}" record \
  --template 'Metal System Trace' \
  --all-processes \
  --time-limit "$xctrace_time_limit_arg" \
  --no-prompt \
  --output "$metal_system_trace" \
  >"$xctrace_record_log" 2>&1
xctrace_status=$?
set -e
printf 'system_trace_xctrace_status: %s\n' "$xctrace_status"

wait "$wrapper_pid" || wrapper_status=$?
wrapper_status=${wrapper_status:-0}
wrapper_pid=
printf 'system_trace_wrapper_status: %s\n' "$wrapper_status"

if (( wrapper_status != 0 )); then
  echo "probe failed; see $wrapper_log" >&2
  exit "$wrapper_status"
fi
if (( xctrace_status != 0 )); then
  echo "xctrace record failed; see $xctrace_record_log" >&2
  exit "$xctrace_status"
fi
if [[ ! -e "$metal_system_trace" ]]; then
  echo "xctrace record reported success but no trace was written: $metal_system_trace" >&2
  echo "see $xctrace_record_log" >&2
  exit 2
fi

if (( skip_export_summary )); then
  echo "wrote metal system trace: $metal_system_trace"
  exit 0
fi

"${xctrace_prefix[@]}" export \
  --input "$metal_system_trace" \
  --output "$metal_gpu_intervals_xml" \
  --xpath '/trace-toc/run[@number="1"]/data/table[@schema="metal-gpu-intervals"]' \
  >"$xctrace_export_log" 2>&1

"${summary_cmd[@]}" >"$xctrace_summary_log" 2>&1

echo "wrote metal system trace: $metal_system_trace"
echo "wrote metal gpu intervals xml: $metal_gpu_intervals_xml"
echo "wrote xctrace summary csv: $xctrace_summary_csv"
echo "wrote xctrace summary md: $xctrace_summary_md"
