# Xcode counter export and attribution

Run commands from the repository root. Read [Metal rules](../../../rules/metal_debugging.rules.md).

When a `.gputrace` is opened in Xcode for real performance debugging, do not
leave the useful data trapped in the Xcode UI. Export both the replayed capture
and encoder counters into the run's `analysis/` directory so later analysis can
be done from files.

Recommended file names:

```sh
TRACE_DIR="traces/<app-runid>"
ANALYSIS_DIR="$TRACE_DIR/analysis"
mkdir -p "$ANALYSIS_DIR"
df -h "$TRACE_DIR"

# Xcode GUI outputs:
#   $ANALYSIS_DIR/frame<N>-performance.gputrace
#   $ANALYSIS_DIR/frame<N>-counters-xcode.csv
# Optional derived files:
#   $ANALYSIS_DIR/frame<N>-counters-summary.csv
#   $ANALYSIS_DIR/frame<N>-xcode-dxmt-joined-summary.csv
#   $ANALYSIS_DIR/frame<N>-xcode-dxmt-bottleneck-report.md
```

Required Xcode GUI sequence:

1. Open `frame<N>.gputrace` in Xcode.
2. In **Summary**, use **Export** and enable **Embed Performance Data** before
   saving the replayed capture as `analysis/frame<N>-performance.gputrace`.
   Enable **Embed External Files** too if Xcode offers it and the capture may be
   inspected on another machine.
3. In **Summary**, click **Show Performance**.
4. Open **Counters**.
5. Wait until counter profiling is complete. The table must be populated and
   Xcode's activity/progress indicator must no longer be profiling counters.
   In practice, Xcode often spends about 1 minute in **Profiling Draw
   Counters...** after the first encoder rows are visible; allow that work to
   finish and confirm that draw-counter profiling has stopped before exporting.
   Elapsed time alone is not a completion signal.
6. Use **Export Encoder Counters** from the Counters view and save the CSV as
   `analysis/frame<N>-counters-xcode.csv`.
7. After export, parse the CSV from the terminal and join it with dxmt encoder
   attribution:
   `scripts/tools/finalize_3dmark05_perf_probe.sh --suffix <tag> --frame <N>`.
   This writes `analysis/frame<N>-counters-summary.csv` and
   `analysis/frame<N>-xcode-dxmt-joined-summary.csv`, plus a Markdown
   bottleneck report ranking the top encoders.

Xcode's save panel can retain a previous `analysis` folder even after a new
`frame<N>.gputrace` is opened. After every performance or counter export,
verify the actual landing path with `find traces -name ...` or `ls
$ANALYSIS_DIR`; if the file landed in an older run directory, move it into the
current `traces/<app-runid>/analysis/` before finalizing. Treat the current
run id, not Xcode's abbreviated `Where: analysis` label, as authoritative.

The CSV is the authoritative source for encoder cost, limiter, bandwidth,
vertex, primitive, and fragment counters. Prefer citing values from that file
over transcribing Xcode UI text. The UI remains useful for navigation and
resource inspection, but final bottleneck notes should be backed by exported
files. When render encoder labels include `RenderPass[seq=...,enc=...]`, use
the joined summary as the authoritative cross-reference from Xcode GPU cost to
dxmt stream/IB churn and upload/write attribution. Inspect the Markdown report
for the VS/FS buffer-write split, VS-write bytes per VS invocation, and
VS L1/LLC write, dxmt CPU-writer-to-Xcode-buffer-write ratios,
per-draw stream/IB churn rates, `dxmt_vs_buffer_bytes_per_dxmt_vertex`,
decoded `dxmt_vsout_*` layout fields, VS-buffer-to-expected-stage-out ratio,
`dxmt_pso_state_samples_per_draw`, VSOut layout-cache hit/miss counts, and
`dxmt_gpu_write_hint` / `dxmt_write_owner_confidence`. The joined CSV also
derives `dxmt_vs_buffer_write_share`,
`dxmt_unexplained_buffer_write_mib`, and
`dxmt_unexplained_buffer_write_ratio`; use these before blaming dxmt
CPU-side argbuf/transient writers for an Xcode buffer-write bucket. A large
unexplained ratio with a high VS buffer-write share points at GPU-side
vertex-stage output/spill-like traffic until proven otherwise. Also inspect
PSO/shader-variant/VSOut-layout attribution before concluding that the
bottleneck is fragment, texture, depth, attachment traffic, VS ALU, simple
varying width, or dxmt CPU upload traffic. A current dxmt log should have
PSO samples near draw frequency even for DrawRun iterations that skip
base-state binding.
For candidate validation, pass `--baseline-output <baseline-output-dir>` and
`--baseline-joined <baseline-joined.csv>` to
`scripts/tools/finalize_3dmark05_perf_probe.sh`; cite the generated Markdown
reports instead of manual spreadsheet comparisons. For run-level draw batching
or CPU encode candidates, use gates such as
`--require-draw-run-records-increase`,
`--require-draw-run-records-per-submit-increase`,
`--require-binding-overrides-present`,
`--require-const-upload-passthrough-present`,
`--require-draw-submission-batch-present`, and
`--require-encode-draw-cpu-decrease` so the intended mechanism is proven by
`result.json` counters before interpreting Xcode frame counters. These
run-level gates require `--baseline-output`; do not run them as standalone
flags, and make sure the path resolves to an existing `result.json`. When the run is meant to confirm a shader/VSOut-layout root cause, also pass
`--require-xcode-counter-coverage --require-dxmt-join-coverage
--require-top-pso-attribution`; incomplete Xcode counter exports, failed
Xcode/dxmt joins, and old/incomplete dxmt logs then fail instead of silently
producing joined rows with missing counter columns, empty dxmt attribution, or
`dxmt_pso_state_samples_per_draw == 0`.
The standard `run_3dmark05_perf_probe.sh` wrapper can carry
`--baseline-joined` plus Xcode comparison gates through to the printed
`finalize_cmd_after_xcode_export`; prefer that over reconstructing the
finalizer command by hand. For production-shaped
`--optimize-opaque-depth-index-cache` captures, use
`--require-opaque-depth-index-cache-proof`; it expands to stable-frame,
target cache-opt/effective LRU32, reordered-cache-hit, and target
VS-write/invocation gates without requiring generic actual LRU32 telemetry.
Reserve `--require-cache-opt-apply-proof` for diagnostic apply probes where
generic actual indexed telemetry is the intended target-row proof. Xcode
comparison gates such as
`--require-top-gpu-decrease`, `--require-top-buffer-write-decrease`,
`--require-top-vs-buffer-write-decrease`, and
`--require-top-unexplained-buffer-write-decrease` require `--baseline-joined`;
do not run them as standalone flags, and make sure the CSV exists before
starting a long capture. Use `--max-top-unexplained-buffer-write-ratio N` when
the candidate should make top Xcode buffer writes explainable by dxmt CPU-side
writers; high residual ratios indicate the bottleneck is still GPU-side or
unattributed. For captures intended to inspect the top
shader/VSOut rows, add wrapper `--dump-shaders`; this stores translated MSL and
D3D bytecode under `traces/<app-runid>/analysis/shaders/` next to the Xcode
counter export and joined report. After Xcode exports encoder counters, run
the standard finalizer; it writes `frame<N>-shader-dump-report.md` and
`frame<N>-shader-dump-summary.csv` by matching joined-summary
`dxmt_vertex_shader_last` / `dxmt_pixel_shader_last` and, on current logs,
`dxmt_vertex_shader_source_last` / `dxmt_pixel_shader_source_last` to
`analysis/shaders/msl/*-shader-<hash>-source-<source>.metal`. Use that report
before making manual shader-source claims about top GPU encoders. If the report
flags `ambiguous_*_dump`, the same shader hash produced multiple source hashes
but the log did not carry a disambiguating source hash; treat the selected row
as a candidate and inspect the source-hash/candidate count before making exact
source-level claims. For root-cause captures where shader source attribution is
required, add `--require-shader-dump-matches` together with wrapper
`--dump-shaders`; the finalizer then fails if top render rows have zero shader
hashes, cannot match dumped MSL files, or only match ambiguously.
For the VS-buffer-write hypothesis, pair-local VSOut liveness is exposed through
wrapper `--trim-unused-varyings`, but 3DMark05 GT1 evidence currently rejects
visible VSOut width as the first-order owner: trim-varyings, direct-texcoord,
point-size-only, position-only, and half-VSOut probes leave the dominant Xcode
VS-buffer-write bucket mostly hidden or fail the GPU-time gate. In the frame50
half-VSOut proof, top VS writes dropped only `-2.44%` while top GPU time
regressed `+3.40%`. Use
`--baseline-joined <csv> --require-top-vs-buffer-write-decrease` only when
revalidating that path on a new baseline; otherwise prioritize hidden
vertex/backend storage probes that move VS invocations or primitive/locality
shape, plus semantic-safe cache-locality selectors.
