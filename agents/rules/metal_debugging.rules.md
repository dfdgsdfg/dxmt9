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

## 1. Programmatic frame capture (.gputrace)

`MetalCaptureController` (`src/dxmt9/dxmt9_capture.{hpp,cpp}`) triggers a
Metal frame capture on a target frame number, writing the result to a
`.gputrace` file you can open in Xcode.

```sh
export DXMT_METAL_CAPTURE_FRAME=120          # capture the 120th present
export DXMT_METAL_CAPTURE_PATH=traces/app-d3d9-3dmark05-20260531-gt1/frame120.gputrace
./your-app
# → traces/app-d3d9-3dmark05-20260531-gt1/frame120.gputrace, openable from Xcode
```

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
