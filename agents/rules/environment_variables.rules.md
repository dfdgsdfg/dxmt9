# dxmt9 Environment Variables

Master list of `DXMT*` environment variables honored by the dxmt9
runtime. Kept in this file so a developer can find every knob without
grepping. Generated from `src/` source-of-truth on demand:

```sh
grep -rEho '"DXMT[A-Z0-9_]+"' src/ | sort -u
```

Categories:

- **Capture / Debug** — frame capture, dump, validation.
- **Logging / Tracing** — log level, log path, trace switches.
- **Perf counters** — counter system + per-frame snapshot.
- **Adapter spoofing** — D3D9 driver ID overrides.
- **Present policy** — present-acquire / boundary / latency tuning.
- **Encoder debug toggles** — force-state knobs for triaging.
- **Compatibility** — opt-out / opt-in flags for known-issue surfaces.

A flag is "set" when its value is a non-empty string that is not `0`,
unless documented otherwise.

## Capture / Debug

| Var | Purpose | Default | Where |
|---|---|---|---|
| `DXMT_METAL_CAPTURE_FRAME` | Capture this 1-based frame as `.gputrace` | unset | `dxmt9_capture.cpp` |
| `DXMT_METAL_CAPTURE_PATH` | Output path for the capture | tmp file | `dxmt9_capture.cpp` |
| `DXMT_DUMP_GPU_TEXTURE_HANDLE` | Dump a specific texture (BMP) post-readback | unset | `dxmt9_capture.cpp` |
| `DXMT_DUMP_GPU_TEXTURE_PATH` | Output path for texture BMP | unset | `dxmt9_capture.cpp` |
| `DXMT_DUMP_TEXTURE_HANDLE` / `DXMT_DUMP_TEXTURE_DIR` | Texture dump (different stage) | unset | core/texture path |
| `DXMT_DUMP_SHADER_DIR` / `DXMT_DUMP_SHADER_BYTECODE_DIR` | Shader translation dumps | unset | shader transform |
| `DXMT_FORCE_PRESENT_TEXTURE_HANDLE` | Force a specific texture as present source | unset | presenter |
| `DXMT_SKIP_TEXTURE_HANDLE` | Suppress texture by handle | unset | presenter / encoder |
| `DXMT_CAPTURE_FRAME` | Legacy capture knob (use `DXMT_METAL_CAPTURE_FRAME`) | unset | core |
| `DXMT_EXPERIMENT_CAPTURE_PATH` | Experiment-harness capture path | unset | experiment runner |
| `DXMT_LEAK_STATEBLOCKS` | Skip stateblock destruction (leak-check triage) | unset | d3d9 |

## Logging / Tracing

| Var | Purpose | Default |
|---|---|---|
| `DXMT_LOG_LEVEL` | `Error` / `Warn` / `Info` / `Debug` / `Trace` | `Warn` |
| `DXMT_LOG_PATH` | Redirect log to file | stderr |
| `DXMT_TRACE_FILE` | Trace output file | unset |
| `DXMT_TRACE_RENDER` | Trace render encoder | unset |
| `DXMT_TRACE_QUEUE` / `DXMT_TRACE_QUEUE_FROM` | Trace queue events | unset |
| `DXMT_TRACE_ENCODE_SEQ` | Trace encode sequence ids | unset |
| `DXMT_TRACE_FVF` / `DXMT_TRACE_FVF_TEX0` / `DXMT_TRACE_FVF_EXPANDED` | FVF decode trace | unset |
| `DXMT_TRACE_SHADER_INPUTS` | Shader input binding trace | unset |
| `DXMT_TRACE_TEXTURE_HANDLE` | Trace per-handle texture events | unset |
| `DXMT9_TRACE_DRAW_GEOMETRY` / `DXMT9_TRACE_DRAW_GEOMETRY_LIMIT` | Draw geometry diagnostics | unset |

## Perf counters

| Var | Purpose | Default |
|---|---|---|
| `DXMT_PERF_COUNTERS` | Enable `[dxmt9-perf]` counter line at exit | `0` |
| `DXMT_PERF_COUNTERS_PERIODIC_PRESENTS` | Emit counters every N presents (numeric) | `0` |
| `DXMT9_PERF_FRAME_SAMPLING` | Per-frame counter delta snapshots | `0` |

## Present policy

| Var | Purpose | Default |
|---|---|---|
| `DXMT9_PRESENT_ASYNC_ACQUIRE` | Request drawable on the encode thread async | `0` |
| `DXMT9_PRESENT_PREACQUIRE` | Pre-acquire drawable before encode | `0` |
| `DXMT9_PRESENT_ACQUIRE_ON_SUBMIT` | Acquire drawable at submit time | `0` |
| `DXMT9_PRESENT_BOUNDARY_AFTER_ACQUIRE` | Defer present boundary | `0` |
| `DXMT9_PRESENT_BOUNDARY_COMPLETION` / `DXMT9_PRESENT_BOUNDARY_PRESENT_COMPLETION` | Boundary on completion | `0` |
| `DXMT9_PRESENT_REFRESH_HZ` | Override refresh rate (numeric Hz) | derived |
| `DXMT9_LAYER_DISPLAY_SYNC` | CAMetalLayer display sync flag | `1` |
| `DXMT9_DISABLE_PRESENT_BOUNDARY` | Disable explicit present boundary | `0` |
| `DXMT9_SPLIT_PRESENT_CHUNK` / `DXMT9_SPLIT_PRESENT_ACQUIRE` | Split present chunks | `0` |
| `DXMT9_SPLIT_STRETCH_CHUNK` | Split stretch-rect chunks | `0` |
| `DXMT9_DRAW_CHUNK_COMMAND_LIMIT` | Max commands per chunk (numeric) | derived |
| `DXMT9_CAP_FRAME_LATENCY_TO_BACKBUFFERS` | Limit max frame latency to backbuffer count | `0` |
| `DXMT9_MAX_FRAME_LATENCY` | Override max frame latency (numeric) | unset |

## Encoder / state debug

| Var | Purpose | Default |
|---|---|---|
| `DXMT_DEBUG_FORCE_CULL_MODE` | Force a specific cull mode | unset |
| `DXMT_DEBUG_FORCE_TRANSIENT_DEDICATED` | Force dedicated transient buffer | `0` |
| `DXMT_DEBUG_FORCE_VISIBLE` / `DXMT9_DEBUG_FORCE_VISIBLE_DRAW` | Force draw visible | `0` |
| `DXMT_DEBUG_FORCE_FULLSCREEN_VERTEX` | Force fullscreen-quad vertices | `0` |
| `DXMT_DEBUG_FORCE_FRAGMENT_COLOR` | Force fragment output color | unset |
| `DXMT_DEBUG_FORCE_PIXEL_V_FLIP` | Flip v-coordinate in pixel shader | `0` |
| `DXMT_DEBUG_FLIP_VERTEX_Y` | Flip y-coordinate in vertex shader | `0` |
| `DXMT_DEBUG_FRONT_FACE_CCW` | Force CCW front-face | `0` |
| `DXMT_DEBUG_FRAGMENT_MODE` | Override fragment shader mode | unset |
| `DXMT_DEBUG_FFP_ALPHA` / `DXMT_DEBUG_FFP_TEXTURE` / `DXMT_DEBUG_FFP_UV` | FFP pipeline debug toggles | `0` |
| `DXMT_DEBUG_NO_PER_DRAW_ALLOC` | Trip per-draw alloc invariant | `0` |
| `DXMT_DEBUG_DISABLE_SHADER_ARCHIVE` | Skip shader archive load/save | `0` |
| `DXMT_DISABLE_ALPHA_TEST` / `DXMT_DISABLE_CULL` / `DXMT_DISABLE_SCISSOR` | Disable specific state | `0` |
| `DXMT_FORCE_EXPAND_INDEXED` | Force indexed-→-non-indexed expansion | `0` |
| `DXMT_FORCE_WINDOWED` | Force windowed mode | `0` |
| `DXMT_SKIP_ALL_DRAWS` | Discard every draw at submit | `0` |
| `DXMT9_AGGRESSIVE_DEPTH_DONTCARE` | Aggressive Store=DontCare for depth | `0` |
| `DXMT9_ALLOW_RUNTIME_PROVIDER_FALLBACK` | Allow legacy provider fallback | `0` |
| `DXMT9_MID_CHUNK_COMMIT_POLICY` | Sub-CB chain split policy: `off` / `per-render-pass` / `per-n-records` (R-BACK-2.29-2.31, R-BACK-2.34 default-flip 2026-05-10) | `per-render-pass` |
| `DXMT9_MID_CHUNK_COMMIT_RECORDS` | Records per sub-CB when policy=`per-n-records` | `64` |
| `DXMT9_MID_CHUNK_COMMIT_CAP_PER_RENDER_PASS` | Max sub-CBs per chunk; `0` disables (R-BACK-2.33) | `4` |

## Adapter spoofing

| Var | Purpose | Default |
|---|---|---|
| `DXMT_ADAPTER_NAME` | Override `Description` | system |
| `DXMT_ADAPTER_VENDOR_ID` | Override vendor-id (numeric) | system |
| `DXMT_ADAPTER_DEVICE_ID` | Override device-id (numeric) | system |
| `DXMT_ADAPTER_DRIVER` | Override driver string | system |

## Compatibility

| Var | Purpose | Default |
|---|---|---|
| `DXMT_COMPAT_HUD` | Enable Compat HUD overlay | `0` |

## Cross-process / Wine

These are inherited by the Wine prefix and reach dxmt9 via the bridge:

| Var | Purpose |
|---|---|
| `DXMT_EXPERIMENT_WINE_DLLOVERRIDES` | Wine `WINEDLLOVERRIDES` snippet for the experiment harness |

## Apple-side (not honored by dxmt9 directly)

These are macOS / Metal toolchain knobs you may want set for a debug
session even though dxmt9 itself does not read them:

| Var | Purpose |
|---|---|
| `MTL_DEBUG_LAYER` | Enable Metal Validation Layer |
| `MTL_HUD_ENABLED` | Built-in Metal HUD overlay |
| `MTL_SHADER_VALIDATION` | Shader validation pass |

## Notes

- Values are read once at process start (typically inside a static
  initializer) — changing them after dxmt9 has loaded does not take
  effect.
- A zero string (`""`) and `"0"` mean "off" for boolean flags.
- For tunable numerics, an unparseable value falls back to the default.
- This file is **descriptive**, not a behavioral spec — for that, see
  `specs/`.
