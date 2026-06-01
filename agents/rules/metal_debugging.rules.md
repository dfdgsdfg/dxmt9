# Debugging dxmt9 with Xcode and Instruments

This runbook covers Metal-side debugging workflows for dxmt9: programmatic
frame capture, validation layer, GPU performance counters, signposts in
Instruments, and resource label / debug-group inspection in Xcode.

The CPU-side measurement story is documented separately in
`docs/perfomance-bottleneck.md`. This file is the **GPU-side** companion.

## TL;DR — what you can do today

| Goal | Tool / knob | Where it lands |
|---|---|---|
| Capture one frame as `.gputrace` | `DXMT_METAL_CAPTURE_FRAME=N` env var | Open the file in Xcode |
| Read resource names in capture | Always-on (M1 labels) | `vb_h0xN`, `pso_h0xH`, etc. |
| See render-pass / draw narrative | Always-on (M2 debug groups) | Encoder timeline gets nested groups |
| Profile in Instruments | Run with `xcrun xctrace record --template "Metal System Trace"` | Signpost track shows `frame`, `commit`, `draw` |
| Per-CB GPU wall time as counter | `DXMT_PERF_COUNTERS=1` (M4) | `gpu_command_buffer_time_*_ms` rows |
| GPU fault count as counter | `DXMT_PERF_COUNTERS=1` (M5) | `gpu_command_buffer_errors` row |
| Device capabilities at startup | Always-on (M6) | One log line at process init |

## Artifact location

Keep manual trace artifacts under a dedicated ignored trace tree:

```
traces/<app-runid>/
```

For app catalogue runs, `<app-runid>` starts with the `run_experiment.py` app
id and may add a timestamp or short tag, for example
`app-d3d9-3dmark05-20260531-gt1`. Put every manual profiling artifact there:
`.gputrace`, `.trace`, exported XML/TSV/JSON, stderr logs, parsed summaries,
and Xcode screenshots if captured. Use subdirectories such as `analysis/`,
`cpu/`, and `screenshots/` inside that per-run directory.

`traces/` is intentionally gitignored. Do not put raw `.trace` / `.gputrace`
bundles in `experiments/output/` unless an experiment harness explicitly emits
them as part of its own contract.

## Xcode `.gputrace` performance export discipline

When a `.gputrace` is opened in Xcode for real performance debugging, do not
leave the useful data trapped in the Xcode UI. Export both the replayed capture
and encoder counters into the run's `analysis/` directory so later analysis can
be done from files.

Recommended file names:

```sh
TRACE_DIR=traces/<app-runid>
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
   Counters...** after the first encoder rows are visible; wait at least
   60 seconds after entering the Counters view and confirm that draw-counter
   profiling has stopped before exporting.
6. Use **Export Encoder Counters** from the Counters view and save the CSV as
   `analysis/frame<N>-counters-xcode.csv`.
7. After export, parse the CSV from the terminal and join it with dxmt encoder
   attribution:
   `scripts/tools/finalize_3dmark05_perf_probe.sh --suffix <tag> --frame <N>`.
   This writes `analysis/frame<N>-counters-summary.csv` and
   `analysis/frame<N>-xcode-dxmt-joined-summary.csv`, plus a Markdown
   bottleneck report ranking the top encoders.

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
finalizer command by hand. Xcode comparison gates such as
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
For the VS-buffer-write hypothesis, run a paired candidate with wrapper
`--trim-unused-varyings` and gate it with
`--baseline-joined <csv> --require-top-vs-buffer-write-decrease`; this proves
whether pair-local VSOut liveness moves Xcode's VS buffer-write counter.

Before starting a new 3DMark05 `.gputrace`, keep at least 2GiB free on the
repository volume. The standard wrapper enforces this with
`DXMT_3DMARK05_MIN_TRACE_FREE_MB` / `--min-free-mb`; do not bypass it unless
the run deliberately disables gputrace or the output volume has already been
cleared. Use `--dry-run` first; if free space is below the guard, dry-run and
guard failures print `space usage hints` plus `large trace/output files` to
choose cleanup targets. Do not delete raw trace artifacts automatically.

## 1. Programmatic frame capture (.gputrace)

`MetalCaptureController` (`src/dxmt9/dxmt9_capture.{hpp,cpp}`) triggers a
Metal frame capture on a target frame number, writing the result to a
`.gputrace` file you can open in Xcode.

```sh
export MTL_CAPTURE_ENABLED=1                 # required for MTLCaptureManager file capture
export DXMT_METAL_CAPTURE_FRAME=120          # capture the 120th present
export DXMT_METAL_CAPTURE_PATH=traces/app-d3d9-3dmark05-20260531-gt1/frame120.gputrace
./your-app
# → traces/app-d3d9-3dmark05-20260531-gt1/frame120.gputrace, openable from Xcode
```

If the log says `Capture layer is not inserted`, the process was launched
without `MTL_CAPTURE_ENABLED=1`; rerun the capture with that variable set.

Trigger sites: `dxmt9_command_queue.cpp:1083` (request) and
`dxmt9_queue.cpp:1083-1095` (start/stop around `commit()`).

## 2. Reading the capture (labels + debug groups)

When you open the `.gputrace`, captured resources and command encoders
are already labeled and grouped:

- **Buffers**: `vb_h0xN`, `ib_h0xN`, `argbuf_seqN`, `transient_seqN`
- **Textures**: `tex_h0xN_<format>_<w>x<h>`
- **CommandQueue**: `dxmt9-q-<deviceId>`
- **CommandBuffer**: `cb_seq_N`
- **Pipelines**: `pso_h<shader-hash>`
- **Render encoders**: label + nested debug groups
  `RenderPass[rt=0xH,depth=0xH]` → `Draw[idx=N,tri=M]`. The label is what
  `xctrace` (`metal-application-encoders-list` and `metal-gpu-intervals`)
  uses to identify the encoder in text traces — without it the encoder
  falls back to Metal's default "Render Command N".
- **Blit encoders**: `Blit[<reason>]`
- **Present encoder**: `Present[seq=N]`

If a capture shows opaque `Buffer 0x12345` instead of these names, an
upstream creation site is missing a `setLabel` call — check
`scripts/check/audit_perf_counter_callsites.py`-style audit on M1
sites is healthy.

## 3. Validation layer

Run with the standard Apple Metal Validation Layer:

```sh
export MTL_DEBUG_LAYER=1
export MTL_DEBUG_LAYER_VALIDATE_LOAD_ACTIONS=1
./your-app
```

Validation messages go to stderr. dxmt9 also surfaces command-buffer
errors itself: `WMTCommandBufferStatusError` is logged via the
`dxmt9-metal` logger and incremented as the
`gpu_command_buffer_errors` perf counter (M5,
`dxmt9_queue.cpp:1444-1448`).

For a regression-resistant gate, the perf probes set
`expected_counters{ gpu_command_buffer_errors = { max = 0 } }` — a
single GPU fault during a probe trips the L3 expected-range gate.

## 4. Instruments (Metal System Trace)

dxmt9 emits `os_signpost` intervals on three boundaries (M3,
`src/dxmt9/dxmt9_signposts.{hpp,cpp}`):

| Interval | Site | What it covers |
|---|---|---|
| `frame` | `dxmt9_draw_encoder.mm:~2290` | one Present to the next |
| `commit` | `dxmt9_queue.cpp:~1095` | command-buffer `commit()` call |
| `draw` | `dxmt9_draw_encoder.mm:~837` | per-draw encode |

Subsystem: `com.dxmt9.translator`, category: `metal`.

Run Instruments:

```sh
xcrun xctrace record --template "Metal System Trace" --launch -- ./your-app
# or for just the dxmt9 signposts:
xcrun xctrace record --template "Logging" --launch -- ./your-app
```

In the resulting trace, find the **dxmt9.translator** subsystem under
the Signposts track. Frame intervals will show up as paired
begin/end markers.

## 5. GPU performance counters

### Per-command-buffer GPU wall time (M4)

When `DXMT_PERF_COUNTERS=1`, dxmt9 samples
`MTLCommandBuffer.GPUStartTime` / `GPUEndTime` after each command
buffer reaches Completed (`dxmt9_queue.cpp:~1450`). Driver-returned
0 / non-monotonic values are filtered. Surfaced as:

```
gpu_command_buffer_time_ms          # sum
gpu_command_buffer_time_max_ms      # max single CB
gpu_command_buffer_time_samples     # number of CBs sampled
gpu_command_buffer_time_p50_ms      # P50 over a 256-sample ring
gpu_command_buffer_time_p95_ms
gpu_command_buffer_time_p99_ms
```

### GPU fault counter (M5)

```
gpu_command_buffer_errors
```

Single counter; pair with `expected_counters{ max = 0 }` on probes.

### Stage-boundary GPU counter sample buffers (path B — not yet implemented)

`MTLCounterSampleBuffer` is bridged in winemetal but not yet wired at
render-encoder boundaries (the blit variant
`MTLCommandBuffer_blitCommandEncoderWithSampleBuffers` exists at
`winemetal.h:2007` but no render-encoder twin). The full implementation
spans PE/unix ABI plus dxmt9 wire-up:

1. **winemetal ABI extension** — add either
   `MTLCommandBuffer_renderCommandEncoderWithSampleBuffers(cmdbuf,
   WMTRenderPassInfo*, WMTSampleBufferAttachmentInfo*, num)` as a new
   bridge function, OR extend `WMTRenderPassInfo` with
   `sample_buffer_attachments[N] + num_sample_buffer_attachments` and
   keep a single render-encoder entry point. New ABI entries require:
   - `winemetal.h` declaration
   - `winemetal/unix/winemetal_private_api.mm` impl mapping to
     `MTLRenderPassDescriptor.sampleBufferAttachments[i].*`
   - Slot registered in `winemetal/unix/winemetal_unix.cpp`'s dispatch
     table (next slot after the last `dxmt9_winemetal_*_unix_call`)
   - PE-side stub in `winemetal_bridge.cpp` + spec
   - `dxmt9_winemetal_abi_hash_unix_call` regen — every PE/unix lockstep
     build must rebuild together; otherwise the bridge ABI handshake at
     `DXMT9_WINEMETAL_CALL_ABI_HASH` (slot 4) refuses to attach.
2. **dxmt9 side** — in `CommandQueue` (`dxmt9_command_queue.cpp:~107`),
   gate on `device.supportsCounterSampling(WMTCounterSamplingPointAtStageBoundary)`,
   allocate one `MTLCounterSampleBuffer` per CB sized to
   `2 × encoders_per_cb_estimate` (start + end per encoder). At
   render-encoder open in `dxmt9_draw_encoder.mm:839` and the four blit
   sites in `dxmt9_blit_encoders.cpp` + `dxmt9_presenter.cpp:475`, set
   `sample_buffer_attachments[0]` to the queue's buffer with
   start/end indices bumped per encoder. On CB completion (queue
   completion handler at `dxmt9_queue.cpp:~1450` where
   `gpu_command_buffer_time_ms` is already sampled), call
   `resolveCounterRange` and accumulate deltas into a new
   `perf::countRenderEncoderGpuTime(rt_handle, ns_delta)` family —
   bucket by RT handle so output mirrors xctrace's per-`RenderPass[rt=…]`
   breakdown without xctrace.
3. **Trade-off** — counter sample buffers are not free; each pair adds
   a tiny GPU stall to drain the pipeline at the boundary. Gate on
   `DXMT_PERF_COUNTERS=1` + a new `DXMT9_PERF_ENCODER_GPU_TIME` flag
   so probe runs can opt out.

Until this lands, use `xcrun xctrace record --template 'Metal System
Trace' --all-processes` + the `metal-gpu-intervals` schema for
per-encoder GPU time (see §4 and the encoder labels set at
`dxmt9_draw_encoder.mm:2226`, `dxmt9_blit_encoders.cpp:200`,
`dxmt9_blit_encoders.cpp:302`, `dxmt9_blit_encoders.cpp:380`,
`dxmt9_presenter.cpp:480`).

## 6. Device capabilities

At process init, `dxmt9_device.cpp` logs (M6):

```
[Info] dxmt9-device capabilities: family={apple7=1 apple8=1 apple9=0} \
       counter_sampling={stage=1 draw=0 blit=1}
```

If a feature you expect isn't enabled, this is the first place to look —
typical causes are older macOS / unsupported hardware family.

## 7. Common workflows

### "I'm seeing a wrong pixel — capture the frame"

```sh
DXMT_METAL_CAPTURE_FRAME=$frame_num \
DXMT_METAL_CAPTURE_PATH=traces/<app-runid>/bug.gputrace \
DXMT_PERF_COUNTERS=1 \
./repro-app
```

Open the `.gputrace` in Xcode. The render-pass debug groups + draw labels
narrate what happened. Cross-reference with the `[dxmt9-perf]` line in stderr
or the matching run's `experiments/output/<app-id>/result.json` counters.

### "GPU appears slower in policy X — quantify"

Run the policy A/B harness:

```sh
python3 scripts/tools/run_dx9_present_policy_ab.py \
  --mode default --mode async --runs 5 \
  --cv-tolerance 0.02
```

`summary.md` gets the new `gpu_command_buffer_time_*_ms` columns under
the Backend metrics table. Compare P50/P95/P99 across modes; CV gate
catches noisy runs.

### "Command buffer is failing — what does Metal say?"

The runtime already logs the error description on
`WMTCommandBufferStatusError`. Check stderr for `dxmt9-metal` lines.
For deeper inspection, add `MTL_DEBUG_LAYER=1` to surface the validation
diagnostic as well.

## 8. Audit gates that protect this surface

| Audit | Script | What it catches |
|---|---|---|
| Counter ↔ table | `scripts/check/audit_perf_counter_table.py` | New `Counters` field with no `kCounterTable` row |
| Counter ↔ callsite | `scripts/check/audit_perf_counter_callsites.py` | Declared `count*()` with no production callsite |
| Probe expected ranges | L3 gate in `run_experiment.py` | Probe counter outside `[min,max]` (e.g. `gpu_command_buffer_errors=1` in a healthy run) |
| Determinism CV | `--cv-tolerance` in `run_dx9_present_policy_ab.py` | fps spread > tolerance across runs |

All four run as Meson tests where applicable.

## 9. See also

- `agents/rules/environment_variables.rules.md` — master environment-variable reference
- `docs/perfomance-bottleneck.md` — CPU-side counter design
- `src/dxmt9/dxmt9_capture.{hpp,cpp}` — capture controller
- `src/dxmt9/dxmt9_signposts.{hpp,cpp}` — Instruments signposts
- `src/dxmt9/dxmt9_perf_counters.{hpp,cpp}` — counter table + ring
