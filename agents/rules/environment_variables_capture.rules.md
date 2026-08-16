# dxmt9 Environment Variables — Capture / Debug

Part of the [`environment_variables.rules.md`](environment_variables.rules.md)
index (frame capture, dump, validation, and mini-replay payload capture).
A flag is "set" when its value is a non-empty string that is not `0`, unless
documented otherwise. See the index for global notes.

## Capture / Debug

| Var | Purpose | Default | Where |
|---|---|---|---|
| `MTL_CAPTURE_ENABLED` / `METAL_CAPTURE_ENABLED` | Apple's Metal capture-layer env spellings seen in local workflows. Do **not** set these by default for 3DMark05 perf probes: current Wine/3DMark05 startup can black-screen with capture-layer env present. `run_3dmark05_perf_probe.sh` uses dxmt9's own `DXMT_METAL_CAPTURE_FRAME/PATH` trigger and only adds `MTL_CAPTURE_ENABLED=1` when `DXMT_3DMARK05_SET_MTL_CAPTURE_ENABLED=1` is deliberately set; either env satisfies the file-capture-layer preflight if a caller sets it explicitly | unset | Metal runtime |
| `DXMT9_SYNC_PSO_COMPILE` | Force draw/library PSO compilation to run inline instead of on the async compile queues. `MTL_CAPTURE_ENABLED=1` or `METAL_CAPTURE_ENABLED=1` also enables this path automatically because the capture layer has reproduced a hang while the encode thread waits on an async prefetched PSO future | `0` | `dxmt9_pipeline_cache.cpp` |
| `DXMT_METAL_CAPTURE_FRAME` | Capture this 1-based frame as `.gputrace` | unset | `dxmt9_capture.cpp` |
| `DXMT9_PER_DRAW_DEBUG_GROUPS` | Force the per-draw `Draw[seq=,prim=]` Metal debug groups on (`1`) or off (`0`). Unset means **on only when a capture is configured** (`DXMT_METAL_CAPTURE_FRAME` / `MTL_CAPTURE_ENABLED` / `METAL_CAPTURE_ENABLED`), because they are read only inside a capture and cost a string format, a bridged `WMT::String` allocation and a push/pop pair per draw — `2.7 ms/present` of encode-thread time on GT2 (`state-churn-encode-append-decomposition.16`). Per-render-pass, blit and present groups are unaffected | capture-gated | `dxmt9_capture.cpp` |
| `DXMT_METAL_CAPTURE_PATH` | Output path for the capture | tmp file | `dxmt9_capture.cpp` |
| `DXMT_METAL_CAPTURE_DESTINATION` | MTLCaptureManager destination: `gpuTraceDocument` / `gputrace` / `file` for file output, or `developerTools` / `xcode` for an attached-Xcode capture route | `gpuTraceDocument` | `dxmt9_capture.cpp` |
| `DXMT_3DMARK05_METAL_CAPTURE_DESTINATION` | 3DMark05 wrapper-only forwarder for `DXMT_METAL_CAPTURE_DESTINATION`; use `developerTools` only when testing Xcode-attached capture without defaulting to `MTL_CAPTURE_ENABLED=1`. Preflight that Xcode can attach to the real Wine child and choose a reachable target frame; this route proves `destination=1` start/stop log lines and does not directly write `frame<N>.gputrace` | unset | `run_3dmark05_perf_probe.sh` |
| `DXMT_3DMARK05_ALLOW_NO_FILE_CAPTURE_LAYER` | Disable the 3DMark05 wrapper's file-capture-layer preflight. Use only for deliberate late-failure diagnostics where `Capture layer is not inserted` at frame capture time is the expected evidence; normal file `.gputrace` runs should fail before Wine launch when the Wine child lacks `MetalCaptureEnabled` and no explicit `MTL_CAPTURE_ENABLED` / `METAL_CAPTURE_ENABLED` env is set | `0` | `run_3dmark05_perf_probe.sh` |
| `DXMT_DUMP_GPU_TEXTURE_HANDLE` | Dump a specific texture (BMP) post-readback | unset | `dxmt9_capture.cpp` |
| `DXMT_DUMP_GPU_TEXTURE_PATH` | Output path for texture BMP | unset | `dxmt9_capture.cpp` |
| `DXMT_DUMP_TEXTURE_HANDLE` / `DXMT_DUMP_TEXTURE_DIR` | Texture dump (different stage) | unset | core/texture path |
| `DXMT_DUMP_SHADER_DIR` / `DXMT_DUMP_SHADER_BYTECODE_DIR` | Shader translation dumps | unset | shader transform |
| `DXMT_FORCE_PRESENT_TEXTURE_HANDLE` | Force a specific texture as present source | unset | presenter |
| `DXMT_SKIP_TEXTURE_HANDLE` | Suppress texture by handle | unset | presenter / encoder |
| `DXMT_CAPTURE_FRAME` | Internal backbuffer capture frame; also primary frame for multi-frame runs | unset | core / experiment runner |
| `DXMT_CAPTURE_FRAMES` | Comma-separated internal backbuffer capture frame list | unset | core / experiment runner |
| `DXMT_CAPTURE_RANGE` | Inclusive internal backbuffer capture range `start:end:interval` | unset | core / experiment runner |
| `DXMT_EXPERIMENT_CAPTURE_PATH` | Single-frame experiment-harness capture path | unset | experiment runner |
| `DXMT_EXPERIMENT_CAPTURE_DIR` | Multi-frame internal capture output directory (`frameNNNNNN.bmp`) | unset | core / experiment runner |
| `DXMT9_RENDER_TAPE_CAPTURE` | Enable the production Render Tape producer. With `DXMT9_RENDER_TAPE_OUTPUT_ROOT` set, the unix renderer selects both the tape-exact attachment policy and a capture-only streaming profile (8-MiB Arena, at most 512 pages in one segmented source), allowing capture-authority source/sequence and pre-reorder FrameGraph identity to be published. Captured first-use attachment seeds are loaded and live-out attachments are stored so capture and fresh-provider replay share deterministic action semantics. Capture without an output publisher and normal rendering when unset do not select this profile or change ordinary streaming bounds | `0` | `d3d9_pe_device.cpp`, `dxmt9_device.cpp`, `dxmt9_command_queue.cpp` |
| `DXMT9_RENDER_TAPE_SKIP_PRESENTS` | Skip this many successful idle Present boundaries before arming the one-interval Render Tape capture. This is a capture-boundary selector only; it does not change command recording or production batching | `0` | `d3d9_pe_device.cpp` |
| `DXMT_LEAK_STATEBLOCKS` | Skip stateblock destruction (leak-check triage) | unset | d3d9 |

## Mini-replay / payload capture

Diagnostic-only knobs for dumping replayable per-draw payloads and for the
`scripts/tools/run_3dmark05_mini_replay.py` standalone replay harness. The dump
vars reuse the reverse-indexed-triangle row/class/span filters and the indexed
triangle encoder draw range; they read geometry/cbuf bytes on the draw path, so
use only in paired diagnostic probes.

| Var | Purpose | Default |
|---|---|---|
| `DXMT9_DUMP_INDEXED_GEOMETRY_DIR` | Directory for dumping per-draw indexed triangle geometry payloads (index + stream0/stream1 bytes). Exposed by `run_3dmark05_perf_probe.sh --dump-indexed-geometry`. Filtered by the reverse-indexed row/class/span filters and the encoder draw range | unset |
| `DXMT9_DUMP_INDEXED_GEOMETRY_CBUFS` | Also dump real per-draw uniform (cbuf) payloads beside the geometry dumps. Exposed by `run_3dmark05_perf_probe.sh --dump-indexed-geometry-cbufs` | `0` |
| `DXMT9_DUMP_INDEXED_GEOMETRY_MAX_DRAWS` | Cap on the number of indexed draw geometry payloads dumped. Exposed by `run_3dmark05_perf_probe.sh --dump-indexed-geometry-max-draws` | `16` |
| `DXMT9_DUMP_INDEXED_GEOMETRY_VS` / `DXMT9_DUMP_INDEXED_GEOMETRY_PS` | Filter the geometry dump by VS/PS shader hash (decimal or `0x`-prefixed, matching `3dmark05-perf-indexed-probe-draws.csv`). Exposed by `run_3dmark05_perf_probe.sh --dump-indexed-geometry-vs/-ps` | unset |
| `DXMT9_DUMP_DEPTH_ATTACHMENT_HANDLE` / `_PATH` / `_SEQ` / `_ENC` | Dump a render pass's raw D24X8 depth attachment sidecar for mini-replay `--depth-input`. `_HANDLE` selects the depth texture handle and `_PATH` the output file (both required to enable); `_SEQ`/`_ENC` optionally scope to one render-pass seq/encoder. Exposed by `run_3dmark05_perf_probe.sh --dump-depth-attachment-handle/-path/-seq/-enc` | unset |
| `DXMT9_DUMP_DRAW_TEXTURE_HANDLE(S)` / `_TEXTURE0_ANY` / `_TEXTURE0_WIDTH` / `_TEXTURE0_HEIGHT` / `_TEXTURE0_FORMAT` / `_SEQ` / `_SEQ_MIN` / `_ENC` / `_DIR` | Read back a shader-visible texture after a matching draw for resource-state diagnostics. `_DIR` plus at least one handle or texture-0 classifier enables the dump. `_SEQ` selects one exact source sequence; `_SEQ_MIN` selects the first matching draw at or after a lower bound, which is useful when capture warm-up makes the exact production sequence nondeterministic. `_ENC` may further restrict the encoder | unset |
| `DXMT9_MINI_REPLAY_CAPTURE_PATH` | `run_3dmark05_mini_replay.py` `.gputrace` capture output path | unset |
| `DXMT9_MINI_REPLAY_COLOR_OUTPUT_PATH` | `run_3dmark05_mini_replay.py` replay color output image path | unset |
| `DXMT9_MINI_REPLAY_REPEAT` | `run_3dmark05_mini_replay.py` replay repeat count | `1` |
| `DXMT9_MINI_REPLAY_MIN_CAPTURE_FREE_MB` | Free-space guard (MiB) before a mini-replay capture; overridable with `--min-capture-free-mb` | `2048` |
