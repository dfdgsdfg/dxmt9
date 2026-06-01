# dxmt9 Environment Variables

Master list of `DXMT*` environment variables honored by the dxmt9
runtime or repository harnesses. Kept in this file so a developer can
find every knob without grepping. Generated from source on demand:

```sh
rg -o '"DXMT9?_[A-Z0-9_]+|DXMT_[A-Z0-9_]+"' src scripts/run_apps scripts/run_suites scripts/tools | sort -u
```

Categories:

- **Capture / Debug** — frame capture, dump, validation.
- **Logging / Tracing** — log level, log path, trace switches.
- **Perf counters** — counter system + per-frame snapshot.
- **Adapter spoofing** — D3D9 driver ID overrides.
- **Present policy** — present-acquire / boundary / latency tuning.
- **Encoder debug toggles** — force-state knobs for triaging.
- **Pipeline cache** — archive prewarm and cache-root controls.
- **PE bridge / recorder** — PE-side chunk recorder diagnostics.
- **Compatibility** — opt-out / opt-in flags for known-issue surfaces.

A flag is "set" when its value is a non-empty string that is not `0`,
unless documented otherwise.

## Capture / Debug

| Var | Purpose | Default | Where |
|---|---|---|---|
| `MTL_CAPTURE_ENABLED` | Enable Apple's Metal capture layer for programmatic `.gputrace` capture; set to `1` whenever `DXMT_METAL_CAPTURE_FRAME` is used | unset | Metal runtime |
| `DXMT_METAL_CAPTURE_FRAME` | Capture this 1-based frame as `.gputrace` | unset | `dxmt9_capture.cpp` |
| `DXMT_METAL_CAPTURE_PATH` | Output path for the capture | tmp file | `dxmt9_capture.cpp` |
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
| `DXMT9_BRIDGE_VERBOSE` | Log rejected or suspicious winemetal bridge handles | `0` |

## Perf counters

| Var | Purpose | Default |
|---|---|---|
| `DXMT_PERF_COUNTERS` | Enable `[dxmt9-perf]` counter line at exit | `0` |
| `DXMT_PERF_COUNTERS_PERIODIC_PRESENTS` | Emit counters every N presents (numeric) | `0` |
| `DXMT9_PERF_FRAME_SAMPLING` | Per-frame counter delta snapshots | `0` |
| `DXMT9_PERF_ENCODER_BREAKDOWN` | Emit per-render-encoder `[dxmt9-perf-encoder]` summary and `[dxmt9-perf-encoder-stream]` stream breakdown lines for stream handle/offset/stride churn, stream Metal-bind first/handle/offset reasons, stream/IB unique handle bytes/usage/pool buckets, IB handle churn, primitive/vertex/FFP/pre-transformed geometry shape, PSO/shader-variant/VSOut-layout attribution including layout-cache hit/miss counts, VS/PS shader hash and, when `DXMT_DUMP_SHADER_DIR` is set, exact VS/PS source hash attribution, argbuf table/cbuf bytes including VS/FFPVS first/rewrite/field splits, setVertexBytes slot-5/other bytes, transient vertex/index bytes split by UP preupload, decl/shadow fallback, and indexed expansion, VS float upload-plan ranges, and write attribution | `0` |
| `DXMT_3DMARK05_REQUIRE_UNLOCKED` | 3DMark05 launcher guard: fail early when macOS reports `CGSSessionScreenIsLocked=Yes`, avoiding false black/factory-only perf captures. Set to `0` only for deliberate locked-session experiments | `1` |
| `DXMT_3DMARK05_RESULT_FILE` | Append a 3DMark05 result-file argument such as `dxmt9_gt1.3dr` after the selected command-line tests, enabling documented unattended result runs when the desktop is unlocked | unset |
| `DXMT_3DMARK05_MIN_TRACE_FREE_MB` | `run_3dmark05_perf_probe.sh` free-space guard before launching Wine/gputrace; defaults to `2048` with gputrace and `256` with `--no-gputrace` | derived |
| `DXMT_3DMARK05_MAX_TOP_UNEXPLAINED_BUFFER_WRITE_RATIO` | Default for the Xcode comparison gate `--max-top-unexplained-buffer-write-ratio`, failing candidates whose top encoder buffer-write traffic remains mostly unexplained by dxmt CPU-side writers | unset |
| `DXMT_3DMARK05_MAX_CONST_UPLOAD_BREAK_COUNT_RATIO` | Default for the run-level comparison gate `--max-const-upload-break-count-ratio`, failing sparse/coalesced constant-upload candidates that reduce bytes by creating too many const-upload draw-run breaks | unset |

## Present policy

| Var | Purpose | Default |
|---|---|---|
| `DXMT9_PRESENT_ASYNC_ACQUIRE` | Request drawable on the encode thread async | `0` |
| `DXMT9_PRESENT_PREACQUIRE` | Pre-acquire drawable before encode | `0` |
| `DXMT9_PRESENT_ACQUIRE_ON_SUBMIT` | Acquire drawable at submit time | `0` |

The three acquire-policy vars above are mutually exclusive in effect:
the runtime resolves them once at Presenter construction into a single
`AcquirePolicy` value with priority `Async > SyncOnSubmit > PreAcquire
> Sync`. With multiple vars set the highest-priority one wins; the
others are ignored. See `dxmt9::resolveAcquirePolicy` in
`src/dxmt9/dxmt9_presenter.hpp` and the matrix spec
`tests/native/backend/present_acquire_policy_spec.cpp`.
| `DXMT9_PRESENT_BOUNDARY_AFTER_ACQUIRE` | Move `notePresentDequeued` to after acquire (selects `BoundaryPolicy::AfterAcquire`) | `0` |
| `DXMT9_PRESENT_BOUNDARY_COMPLETION` | Wait on command-buffer `completedSeqId_` (selects `BoundaryPolicy::Completion`) | `0` |
| `DXMT9_PRESENT_BOUNDARY_PRESENT_COMPLETION` | Wait on `presentCompletedSeqId_` (selects `BoundaryPolicy::PresentCompletion`) — default on; explicit `0` opts out | `1` |
| `DXMT9_PRESENT_REFRESH_HZ` | Override refresh rate (numeric Hz) | derived |
| `DXMT9_LAYER_DISPLAY_SYNC` | CAMetalLayer display sync flag | `1` |
| `DXMT9_DISABLE_PRESENT_BOUNDARY` | Skip the present-boundary wait entirely (selects `BoundaryPolicy::Disabled`) | `0` |
| `DXMT9_SPLIT_PRESENT_CHUNK` / `DXMT9_SPLIT_PRESENT_ACQUIRE` | Split present chunks | `0` |
| `DXMT9_SPLIT_STRETCH_CHUNK` | Split stretch-rect chunks | `0` |
| `DXMT9_DRAW_CHUNK_COMMAND_LIMIT` | Max commands per chunk (numeric) | derived |
| `DXMT9_CAP_FRAME_LATENCY_TO_BACKBUFFERS` | Limit max frame latency to backbuffer count | `0` |
| `DXMT9_MAX_FRAME_LATENCY` | Override max frame latency (numeric) | unset |
| `DXMT9_SYNC_PRESENT_FLUSH` | Flush synchronously after present for present-path triage | `0` |

The four `DXMT9_*PRESENT_BOUNDARY*` vars above resolve once at
process init into a single `dxmt9::BoundaryPolicy` value with priority
`Disabled > PresentCompletion > Completion > AfterAcquire > Default`
— `Disabled` short-circuits the whole boundary; `PresentCompletion`
is the historical default-on branch (null / empty env counts as set,
only explicit `0` demotes). `AfterAcquire` is observationally a no-op
when a higher-precedence wait branch is selected (those branches do
not consult `presentDequeuedSeqId_`). See
`dxmt9::resolveBoundaryPolicy` in `src/dxmt9/dxmt9_presenter.hpp`,
the switch in `CommandQueue::presentBoundary`
(`src/dxmt9/dxmt9_command_queue.cpp`), the AfterAcquire site in
`src/dxmt9/dxmt9_draw_encoder.mm`, and the matrix spec
`tests/native/backend/present_boundary_policy_spec.cpp`.

## Pipeline cache

| Var | Purpose | Default |
|---|---|---|
| `DXMT9_PREWARM` | Override Metal binary archive prewarm mode: `full` / `lazy` / `disabled` | release=`full`, debug=`lazy` |
| `DXMT9_CACHE_DIR` | Override dxmt9 cache root for shader archives | platform cache dir |

## Encoder / state debug

| Var | Purpose | Default |
|---|---|---|
| `DXMT_DEBUG_FORCE_CULL_MODE` | Force a specific cull mode | unset |
| `DXMT_DEBUG_FORCE_TRANSIENT_DEDICATED` | Force dedicated transient buffer | `0` |
| `DXMT_DEBUG_FORCE_VISIBLE` / `DXMT9_DEBUG_FORCE_VISIBLE_DRAW` | Force draw visible | `0` |
| `DXMT_DEBUG_FORCE_FULLSCREEN_VERTEX` | Force fullscreen-quad vertices | `0` |
| `DXMT_DEBUG_FORCE_FRAGMENT_COLOR` | Force fragment output color | unset |
| `DXMT9_DRAW_SEQ_MIN` / `DXMT9_DRAW_SEQ_MAX` | Drop draws outside an inclusive submission seq-id range for visual bisection | unset |
| `DXMT9_DRAW_ORDINAL_MIN` / `DXMT9_DRAW_ORDINAL_MAX` | Drop draws outside an inclusive draw-call ordinal range after any seq-id filter has been applied | unset |
| `DXMT_DEBUG_FORCE_PIXEL_V_FLIP` | Flip v-coordinate in pixel shader | `0` |
| `DXMT_DEBUG_FLIP_VERTEX_Y` | Flip y-coordinate in vertex shader | `0` |
| `DXMT_DEBUG_FRONT_FACE_CCW` | Force CCW front-face | `0` |
| `DXMT_DEBUG_FRAGMENT_MODE` | Override fragment shader mode | unset |
| `DXMT_DEBUG_FFP_ALPHA` / `DXMT_DEBUG_FFP_TEXTURE` / `DXMT_DEBUG_FFP_UV` | FFP pipeline debug toggles | `0` |
| `DXMT_DEBUG_NO_PER_DRAW_ALLOC` | Trip per-draw alloc invariant | `0` |
| `DXMT_DEBUG_DISABLE_SHADER_ARCHIVE` | Skip shader archive load/save | `0` |
| `DXMT9_DISABLE_ARGBUF_HYBRID` | Force Stage 2 argument-buffer hybrid binding path off | `0` |
| `DXMT9_TILE_FFP` | Tile-FFP path selector override: `off` / `auto` / `force`. **`off` is the DEFAULT** (unset/empty/unrecognized) — interim safety: the tile encode wire (`523b66e`) issues `setTileRenderPipelineState`+`dispatchThreadsPerTile` instead of the base-colour draw, so any tile-routed draw renders the cleared imageblock = **black**; until the two-stage encode lands + is GPU-validated, tile-FFP is off and every FFP draw takes the correct portable lane. `auto` explicitly opts into the `selectTileFfpForPass` heuristic (the path the two-stage fix will validate behind this flag before the default flips back). `force` takes the tile lane for any draw that passes every eligibility gate (FFP shape, precision, A2C) — it does **not** force a genuinely-ineligible draw (non-Apple3 / non-FFP / textured / vertex-blended / precision-unsafe / A2C-with-alpha-test). Use `force` vs `off` to A/B the tile and portable readbacks (R-BACK-13.*). Read once at first use (`dxmt9_pipeline_cache.cpp::selectTileFfpForPass`). | `off` |
| `DXMT_DISABLE_ALPHA_TEST` / `DXMT_DISABLE_CULL` / `DXMT_DISABLE_SCISSOR` | Disable specific state | `0` |
| `DXMT_FORCE_EXPAND_INDEXED` | Force indexed-→-non-indexed expansion | `0` |
| `DXMT_DISABLE_AUTO_EXPAND_INDEXED` | Disable compatibility heuristic that auto-expands selected indexed draws | `0` |
| `DXMT_FORCE_WINDOWED` | Force windowed mode | `0` |
| `DXMT_SKIP_ALL_DRAWS` | Discard every draw at submit | `0` |
| `DXMT9_AGGRESSIVE_DEPTH_DONTCARE` | Aggressive Store=DontCare for depth | `0` |
| `DXMT9_AGGRESSIVE_COLOR_DONTCARE` | Opt in to color Store=DontCare when the color handle does not reappear in the rest of the current chunk and no Present is seen; default remains conservative and only next-clear can DontCare-store color | `0` |
| `DXMT9_SPLIT_SPARSE_CONST_RECORDS` | Opt-in PE recorder experiment: split sparse dirty shader-constant ranges into actual changed-register runs instead of one merged min/max const-upload record. Use only in paired perf probes because it may trade fewer const bytes for more const records | `0` |
| `DXMT9_TRIM_UNUSED_VARYINGS` | Trim VSOut to drop fields no SFIV FS reads (texcoord5/6/7 + fogFactor + pointSize). -56 B/vertex, eliminates Apple TBDR "Out of parameter buffer memory" annotations (SFIV: 95→0 events in 20 s). Workload-specific — apps that sample texcoord ≥ 5 or read fogFactor/pointSize will render wrong | `0` |
| `DXMT9_TRIM_VERTEX_TEMPS` | Opt-in translated vertex-shader temp-array experiment: size `float4 r[]` from observed temp source/dest usage instead of the conservative 32-slot array. Use only in paired perf/gputrace probes until broader shader-corpus coverage clears the prior VS trim regression risk | `0` |
| `DXMT9_TRIM_VS_OUTPUT_SCRATCH` | Opt-in translated vertex-shader output-scratch experiment: size local `float4 outTexcoord[]` from emitted VSOut/declared output usage instead of the conservative 8-slot array. Use only in paired perf/gputrace probes; relative texcoord output access still promotes to all 8 slots | `0` |
| `DXMT9_FS_HALF_PRECISION` | **EXPERIMENTAL — NOT FUNCTIONAL.** Rewrites translated FS bodies to half (fp16). Only ~33% of SFIV's FS sources compile under the current text-rewrite implementation; the remainder fail at MSL boundary type checks (sample-coord float2 requirement, helper-call dispatch, half4 ctor matching). Proper fix requires IR-level type propagation — see dxmt9_shader_sources.hpp header for the full status note. | `0` |
| `DXMT9_LAYER_FRAMEBUFFER_ONLY` | Flip `CAMetalLayer.framebufferOnly` to `true` (Apple's tile-only fast path). Scope is the `CAMetalLayer` **drawable** only — the dxmt9 backbuffer `MTLTexture` returned by `IDirect3DSwapChain9::GetBackBuffer` is a separate pool-allocated texture with full read/blit usage, so D3D9 `IDirect3DSurface9::Lock()` and `IDirect3DDevice9::GetRenderTargetData()` on the backbuffer continue to follow their normal D3D9 contracts regardless of this flag (no silent no-op, no HRESULT change, no zero-fill, no per-call warning). The trade-off is therefore present-side only: tile-only mode lets Apple keep the drawable in tile memory and skip the system-memory round-trip used by the WindowServer compositor. Apps that read the **presented** drawable through a non-D3D9 path (e.g. screen-capture frameworks) may render wrong; D3D9-side readback is unaffected. See `specs/d3d9/wsi/requirements.md` §6 (R-CORE-WSI-6.1, R-CORE-WSI-6.2). Tested no-op on SFIV mean GPU time (compositor cost is parallel to SFIV's own fragment work, not stealing from it). | `0` |
| `DXMT9_MID_CHUNK_COMMIT_POLICY` | Sub-CB chain split policy: `off` / `per-render-pass` / `per-n-records` (R-BACK-2.29-2.31, R-BACK-2.34 default-flip 2026-05-10) | `per-render-pass` |
| `DXMT9_MID_CHUNK_COMMIT_RECORDS` | Records per sub-CB when policy=`per-n-records` | `64` |
| `DXMT9_MID_CHUNK_COMMIT_CAP_PER_RENDER_PASS` | Max sub-CBs per chunk; `0` disables (R-BACK-2.33) | `4` |

## PE bridge / recorder

| Var | Purpose | Default |
|---|---|---|
| `DXMT9_PE_RECORDER_STATS` | Emit PE recorder aggregate stats | `0` |
| `DXMT9_PE_RECORDER_CHUNK_LOG` | Emit PE recorder chunk boundary logs | `0` |
| `DXMT9_PE_DRAW_FULL_SNAPSHOT` | Force every draw packet to ride a full PE state snapshot instead of the default delta. Applied in `src/d3d9/d3d9_pe_device.cpp::buildDrawPrimitivePacket` (lines ~497-583 under `if (dxmt9PeFullSnapshotEnabled())`). Trade-off: wire size grows ~10x (typical packet ~100 B → ~1 KB), the importer's run-coalescer (`packetHasNoStateDelta`) sees no empty packets so every draw breaks the coalesced run, but each packet is replayable in isolation — debug / stress / out-of-order-replay only. Equivalence with the default delta path is regression-guarded by `tests/native/bridge/pe_full_snapshot_equivalence_spec.cpp`. | `0` |
| `DXMT9_PE_CHUNK_MAX_RECORDS` | Override max pending PE chunk records before commit | `64` |
| `DXMT9_PE_CHUNK_MAX_BYTES` | Override max pending PE chunk bytes before commit | `262144` |

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

These are used by Wine, the PE/unix bridge, or repository experiment
harnesses:

| Var | Purpose |
|---|---|
| `DXMT9_WINEMETAL_SO` | Explicit winemetal.so provider path for app-local bridge loading |
| `DXMT9_ALLOW_RUNTIME_PROVIDER_FALLBACK` | Allow legacy runtime-by-name provider fallback |
| `DXMT_EXPERIMENT_WINE_DLLOVERRIDES` | Wine `WINEDLLOVERRIDES` snippet for the experiment harness |
| `DXMT_EXPERIMENT_WINE_ID` | Select a Wine manifest entry for experiment runs |
| `DXMT_EXPERIMENT_CX_BOTTLE` | Select a CrossOver bottle for experiment launchers |
| `DXMT_VALIDATE` | Enable validation-layer setup in experiment launchers |
| `DXMT_UPSTREAM_ROOT` | Upstream shader-corpus checkout used by sync/drift tools |
| `DXMT_UPSTREAM_COMMIT` | Upstream shader-corpus commit override used by sync/drift tools |
| `DXMT_UPSTREAM_URL` | Upstream shader-corpus provenance URL override |
| `DXMT_ORACLE_DATE` | Shader-corpus oracle provenance date override |

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
