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
drop_vsout_point_size=0
probe_position_only_vsout=0
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
split_large_indexed_draws=
split_large_indexed_draws_row=
split_large_indexed_draws_rows=
split_large_indexed_draws_class=
split_large_indexed_draws_classes=
force_expand_indexed=0
probe_reverse_indexed_triangles=0
probe_reverse_opaque_indexed_triangles=0
probe_reverse_nonopaque_indexed_triangles=0
probe_reverse_indexed_triangles_row=
probe_reverse_indexed_triangles_rows=
probe_reverse_indexed_triangles_class=
probe_reverse_indexed_triangles_classes=
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
aggressive_color_dontcare=0
aggressive_depth_dontcare=0
disable_cull=0
disable_scissor=0
disable_alpha_test=0
disable_fog=0
force_texture_white=0
probe_disable_alpha_blend=0
probe_disable_depth_write=0
probe_depth_func_always=0
force_visible=0
compare_baseline_output=${DXMT_3DMARK05_COMPARE_BASELINE_OUTPUT:-}
compare_baseline_joined=${DXMT_3DMARK05_COMPARE_BASELINE_JOINED:-}
encoder_breakdown_seq=${DXMT_3DMARK05_ENCODER_BREAKDOWN_SEQ:-}
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
require_top_row_key_match=0
require_top_pso_attribution=0
require_xcode_counter_coverage=0
require_dxmt_join_coverage=0
require_shader_dump_matches=0
min_top_pso_samples_per_draw=${DXMT_3DMARK05_MIN_TOP_PSO_SAMPLES_PER_DRAW:-0.90}
min_top_dxmt_joined_fraction=${DXMT_3DMARK05_MIN_TOP_DXMT_JOINED_FRACTION:-1.0}
max_top_gpu_regression_ms=${DXMT_3DMARK05_MAX_TOP_GPU_REGRESSION_MS:-}
max_top_buffer_write_regression_mib=${DXMT_3DMARK05_MAX_TOP_BUFFER_WRITE_REGRESSION_MIB:-}
max_top_unexplained_buffer_write_ratio=${DXMT_3DMARK05_MAX_TOP_UNEXPLAINED_BUFFER_WRITE_RATIO:-}
max_top_draw_call_delta_ratio=${DXMT_3DMARK05_MAX_TOP_DRAW_CALL_DELTA_RATIO:-}
max_top_vertex_count_delta_ratio=${DXMT_3DMARK05_MAX_TOP_VERTEX_COUNT_DELTA_RATIO:-}
max_top_triangle_delta_ratio=${DXMT_3DMARK05_MAX_TOP_TRIANGLE_DELTA_RATIO:-}
top_n=${DXMT_3DMARK05_TOP_N:-3}
hot_gpu_share=${DXMT_3DMARK05_HOT_GPU_SHARE:-95.0}
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
  --encoder-breakdown-seq N
                      Set DXMT9_PERF_ENCODER_BREAKDOWN_SEQ=N to emit only one
                      RenderPass[seq=N,...] frame's encoder breakdown
  --dump-shaders      Dump translated MSL and D3D shader bytecode under
                      traces/<run-id>/analysis/shaders
  --trim-unused-varyings
                      Set DXMT9_TRIM_UNUSED_VARYINGS=1 for pair-local VSOut
                      liveness/VS buffer-write experiments
  --drop-vsout-point-size
                      Set DXMT9_PROBE_DROP_VSOUT_POINT_SIZE=1 to remove only
                      VSOut [[point_size]] for pipeline-shape A/B probes
  --probe-position-only-vsout
                      Set DXMT9_PROBE_POSITION_ONLY_VSOUT=1 to force
                      position-only VSOut and constant fragment output for a
                      correctness-invalid hidden VS-write lower-bound probe
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
                      order-independent screen-blend indexed triangle-list
                      primitive order through a transient IB
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
                      separated, e.g. large4096,alpha-blend,scissor
  --split-large-indexed-draws N
                      Set DXMT9_SPLIT_LARGE_INDEXED_DRAWS=N to split indexed
                      triangle-list draws above N primitives
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
                      depth-read, alpha-blend, scissor, textured, large4096
  --split-large-indexed-draws-classes CLASSES
                      Set DXMT9_SPLIT_LARGE_INDEXED_DRAWS_CLASSES=CLASSES.
                      Values are ANDed and may be comma/semicolon/space/+ or
                      & separated, e.g. large4096,alpha-blend
  --force-expand-indexed
                      Set DXMT_FORCE_EXPAND_INDEXED=1 to expand indexed draws
                      into flat vertex lists for primitive/backend pressure
                      classification
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
                      Accepted values match split-large indexed filters:
                      any, opaque-depth-write, nonopaque, depth-read,
                      alpha-blend, scissor, textured, large4096
  --probe-reverse-indexed-triangles-classes CLASSES
                      Set DXMT9_PROBE_REVERSE_INDEXED_TRIANGLES_CLASSES=CLASSES.
                      Values are ANDed and may be comma/semicolon/space/+ or
                      & separated, e.g. large4096,scissor
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
                      ANDed, e.g. large4096,alpha-blend,scissor
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
  --probe-disable-alpha-blend
                      Set DXMT9_PROBE_DISABLE_ALPHA_BLEND=1 for blend state A/B
  --probe-disable-depth-write
                      Set DXMT9_PROBE_DISABLE_DEPTH_WRITE=1 for depth-write A/B
  --probe-depth-func-always
                      Set DXMT9_PROBE_DEPTH_FUNC_ALWAYS=1 to keep depth writes
                      but force the depth compare function to Always
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
  --require-top-row-key-match
                      Finalizer Xcode gate: top-N seq/enc row key sets must match
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
    --encoder-breakdown-seq)
      encoder_breakdown_seq=${2:?missing value for --encoder-breakdown-seq}
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
    --drop-vsout-point-size)
      drop_vsout_point_size=1
      shift
      ;;
    --probe-position-only-vsout)
      probe_position_only_vsout=1
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
    --split-large-indexed-draws)
      split_large_indexed_draws=${2:?missing value for --split-large-indexed-draws}
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
    --probe-disable-alpha-blend)
      probe_disable_alpha_blend=1
      shift
      ;;
    --probe-disable-depth-write)
      probe_disable_depth_write=1
      shift
      ;;
    --probe-depth-func-always)
      probe_depth_func_always=1
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
    --require-top-row-key-match)
      require_top_row_key_match=1
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

if [[ ! "$top_n" =~ ^[0-9]+$ ]] || (( top_n == 0 )); then
  echo "--top must be a positive integer" >&2
  exit 2
fi

if [[ ! "$hot_gpu_share" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
  echo "--hot-gpu-share must be numeric" >&2
  exit 2
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
      require_top_row_key_match )) ||
   [[ -n "$max_top_gpu_regression_ms" ||
      -n "$max_top_buffer_write_regression_mib" ||
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

if [[ -n "$encoder_breakdown_seq" ]]; then
  env_args+=("DXMT9_PERF_ENCODER_BREAKDOWN_SEQ=$encoder_breakdown_seq")
fi

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

if (( drop_vsout_point_size )); then
  env_args+=("DXMT9_PROBE_DROP_VSOUT_POINT_SIZE=1")
fi

if (( probe_position_only_vsout )); then
  env_args+=("DXMT9_PROBE_POSITION_ONLY_VSOUT=1")
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

if [[ -n "$split_large_indexed_draws" ]]; then
  env_args+=("DXMT9_SPLIT_LARGE_INDEXED_DRAWS=$split_large_indexed_draws")
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

if (( probe_reverse_indexed_triangles )); then
  env_args+=("DXMT9_PROBE_REVERSE_INDEXED_TRIANGLES=1")
fi

if (( probe_reverse_opaque_indexed_triangles )); then
  env_args+=("DXMT9_PROBE_REVERSE_OPAQUE_INDEXED_TRIANGLES=1")
fi

if (( probe_reverse_nonopaque_indexed_triangles )); then
  env_args+=("DXMT9_PROBE_REVERSE_NONOPAQUE_INDEXED_TRIANGLES=1")
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

if (( probe_disable_alpha_blend )); then
  env_args+=("DXMT9_PROBE_DISABLE_ALPHA_BLEND=1")
fi

if (( probe_disable_depth_write )); then
  env_args+=("DXMT9_PROBE_DISABLE_DEPTH_WRITE=1")
fi

if (( probe_depth_func_always )); then
  env_args+=("DXMT9_PROBE_DEPTH_FUNC_ALWAYS=1")
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
    --top "$top_n"
    --hot-gpu-share "$hot_gpu_share"
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
  if (( require_top_row_key_match )); then
    finalize_cmd+=(--require-top-row-key-match)
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
if (( probe_disable_depth_write )); then
  echo "warning: --probe-disable-depth-write is diagnostic only and can corrupt depth-dependent frame output; do not treat it as correctness-preserving."
fi
if (( probe_depth_func_always )); then
  echo "warning: --probe-depth-func-always is diagnostic only and can corrupt depth-dependent frame output; use it only to isolate depth-compare backend effects."
fi
if (( force_expand_indexed )); then
  echo "warning: --force-expand-indexed is diagnostic only; it preserves indexed geometry intent but changes vertex submission/cache behavior and can heavily regress GPU/CPU cost."
fi
if (( probe_reverse_indexed_triangles )); then
  echo "warning: --probe-reverse-indexed-triangles is diagnostic only; it preserves indexed draw count/render state but changes primitive order and can alter depth/blend results."
fi
if (( probe_reverse_nonopaque_indexed_triangles )); then
  echo "warning: --probe-reverse-nonopaque-indexed-triangles is diagnostic only; it targets visibility-sensitive draws and can corrupt depth/blend results."
fi
if [[ -n "$probe_scissor_rect" ]]; then
  echo "warning: --probe-scissor-rect is diagnostic only; it changes raster coverage and can corrupt frame output. Use it only to classify scissor/tile-coverage backend effects."
fi
if [[ -n "$force_cull_mode" ]]; then
  echo "warning: --force-cull-mode is diagnostic only and can corrupt visibility; use it only to classify cull/backend state-shape effects."
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

run_status=0
(
  cd "$repo_root"
  env "${env_args[@]}" "${cmd[@]}"
) || run_status=$?

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

if (( run_status != 0 )); then
  echo "3DMark05 perf run exited with status $run_status after writing available postprocess artifacts" >&2
  exit "$run_status"
fi
