# Spec–Implementation Gap

Current state of the codebase vs. the specifications.
Updated against current working tree.

Legend: ✅ implemented · ⚠️ partial · ❌ not started

---

## D3D9 Layer

| Area | Status | Evidence / notes |
|---|---|---|
| Factory, adapter enum, `CheckDeviceFormat` / conversion | ⚠️ | Functional path exists. **T3 (2026-05-08)**: `CheckDepthStencilMatch` now does real bit-depth compatibility (not unconditional `S_OK`); `CheckDeviceFormatConversion` does real format-pair check (identity + A8R8G8B8↔X8R8G8B8) instead of unconditional `D3DERR_NOTAVAILABLE`; `CheckDeviceFormat` rejects VB/IB rtype with `D3DERR_INVALIDCALL` and downgrades AUTOGENMIPMAP success to `D3DOK_NOAUTOGEN`. Helpers in `core_format_utils.hpp`. PE factory still needs adapter index validation, fullscreen display format strictness, adapter format allowlist, resource type mapping parity. |
| `GetAdapterIdentifier` | ✅ | `Factory::getAdapterIdentifier()` |
| `EnumAdapterModes` | ✅ | `Factory::enumAdapterModes()` |
| `GetAdapterDisplayMode` | ✅ | `Factory::getAdapterDisplayMode()` |
| `GetAdapterMonitor` | ✅ | `Factory::getAdapterMonitor()` |
| `CheckDeviceType` | ⚠️ | HAL path exists; invalid adapter, valid but unavailable device type, invalid enum, and fullscreen display-format return-code parity still need Wine-oracle Windows D3D9 conformance coverage |
| `CheckDeviceMultiSampleType` | ⚠️ | Factory + Device path exists; invalid enum, `D3DMULTISAMPLE_NONE` quality level, unsupported sample-count `pQualityLevels`, and Windows D3D9 HRESULT parity still need Wine-oracle conformance coverage |
| PE `d3d9.dll` export surface | ⚠️ | The scoped auxiliary set is now implemented with DXVK/D9VK-compatible ordinals: `Direct3DCreate9On12`, `D3DPERF_*`, and `DebugSetMute`. Focused x64 app-local export/auxiliary tests pass. The row remains partial until the broader selected export profile, such as optional `PSGP*` / `DebugSetLevel` compatibility if adopted, is explicitly audited |
| Device lifecycle: Reset, `D3DPOOL_DEFAULT` invalidation | ⚠️ | Functional reset exists. **T2 (2026-05-08)**: `Reset()`/`ResetEx()` now mirror viewport+scissor to fullscreen `{0,0,W,H}` on success matching Wine `d3d9_device_Reset`; `releaseAllBound()` explicitly nullifies the C-side render-target slot 0 and depth-stencil so no stale Metal handle survives; lost-device gate now enforces `D3DERR_DEVICELOST` early-return on `Present`/`PresentEx`/`BeginScene`/`EndScene`/`Clear`/`DrawPrimitive`/`DrawIndexedPrimitive`/`DrawPrimitiveUP`/`DrawIndexedPrimitiveUP`/`ProcessVertices`. MANAGED preservation already correct in `invalidateDefaultPoolResources`. Still needed: present-parameter validation parity, full Wine `test_reset_resources` / `test_lost_device` matrix coverage. |
| Fullscreen `createDevice` | ⚠️ | `normalizePresentParameters()` exists; Windows D3D9-compatible `D3DPRESENT_PARAMETERS` validation and exact creation HRESULT propagation are still required |
| Device-lost: trigger + recovery | ✅ | `setDeviceLostObserver()` |
| `TestCooperativeLevel` | ✅ | Returns `D3DERR_DEVICELOST` when lost |
| `CreateAdditionalSwapChain` | ✅ | `Device::createAdditionalSwapChain()` |
| Device state shadow (render / texture / sampler / transform / lights / stencil) | ✅ | `DeviceState`, all `Set*`/`Get*` methods |
| BeginScene / EndScene | ✅ | Nested-call validation |
| StateBlock capture / restore | ⚠️ | Full-state copy exists. **T1 (2026-05-08)**: `D3DSBT_VERTEXSTATE` apply now copies the per-stage TSS slice (`D3DTSS_TEXCOORDINDEX`, `D3DTSS_TEXTURETRANSFORMFLAGS`) matching Wine `vertex_states_texture[]`; `kPixelStateRenderStates` and `kVertexStateRenderStates` already match Wine's `pixel_states_render[]` and `vertex_states_render[]`. Still needed: `BeginStateBlock` recording invalid-call cases, derived-cache invalidation after `Apply`, full Wine `test_state_block_savedstates` matrix. |
| Hot-path CommandChunk recording and data normalizers | ⚠️ | Chunk records, delta draw packets, `APPLY_STATE`, bulk retention, and draw-run paths exist. `makeCanonicalDrawStateFromState()` / draw-run helpers cover production state-to-flat-draw transforms, while `makeDrawDescFromState()` is fixture/offline coverage only. **Module structure (Round-2/3 splits, 2026-05-08/09):** `device_c_record_utils` split into `_validate.cpp` (612) + `_replay.cpp` (264) + `_hazard.cpp` (406) (T1 `aef2b0a`); `device_c_device_state_draw.cpp` split into `_state.cpp` + `_draw.cpp` (Round-2 `96770cd`); `device_c_common.cpp` split into `_marshal.cpp` + `_shader_dump.cpp` + `_format_utils.cpp` (Round-2 `140493b`); `device_c_draw.cpp` chunk-replay extracted to `device_c_chunk_replay.cpp` (T6 `0ec573f`). All splits cover the same code paths with native tests (`chunk_record_validation_spec`, `chunk_record_hazard_spec`, `chunk_record_replay_spec`, `chunk_record_import_spec`). Remaining alignment is auditing barrier/hazard behaviour with a fake backend or queue instrumentation against R-CORE-11.14-R-CORE-11.18 |
| DOD / DXMT ownership acceptance (`R-ARCH-1.*`, `R-ARCH-2.*`, `R-ARCH-5.*`) | ⚠️ | `specs/archicture/` now owns the whole-project architecture contract and the chunk wire path is data-oriented. Merge-readiness still needs explicit acceptance that PE state shadow, POD chunk construction, unix import, queue execution, presentation pacing, and deferred resource safety each have matching implementation owners and tests |
| DrawPrimitive, DrawIndexedPrimitive, UP variants | ✅ | All four variants |
| TriangleFan decomposition | ✅ | `decomposeTriangleFanIndices()` |
| Half-pixel offset | ✅ | `halfPixelFixup()` |
| FfpVertexKey / FfpPixelKey generation | ✅ | `makeFfpVertexKey()`, `makeFfpPixelKey()` |
| Shader bytecode storage + hash | ✅ | `ShaderBytecode`, FNV-1a hash cache |
| Format table | ⚠️ | `formatTable()` exists; central explicit classification for all FOURCC/pseudo-formats such as `RESZ` and `NULL` still needs audit |
| `makeDefaultCaps()` | ✅ | R-CAPS-1 through R-CAPS-7 |
| Buffer / Texture / Surface lifecycle | ⚠️ | Pool-based reset behavior exists. **T4 (2026-05-08)**: D3D9Ex SYSTEMMEM 1-mip 2D texture and `CreateOffscreenPlainSurface` w/ `pSharedHandle` now alias caller memory (`LockRect` returns user pointer with computed pitch); VB/IB SYSTEMMEM + handle returns `D3DERR_NOTAVAILABLE`; SCRATCH + handle returns `D3DERR_INVALIDCALL`; cube/volume + SYSTEMMEM + handle returns `D3DERR_INVALIDCALL`. DEFAULT-pool shared handles still `E_NOTIMPL` (deferred — needs IOSurface/MTLSharedTexture bridge). Still needed: public COM refcount/lifetime, common private-data semantics, full `test_user_memory` matrix including `CreateOffscreenPlainSurfaceEx`. |
| `UpdateSurface`, `UpdateTexture`, `StretchRect`, `ColorFill`, `GetRenderTargetData` | ✅ | Core-side logic |
| Query: EVENT, OCCLUSION, TIMESTAMP, TIMESTAMPFREQ, TIMESTAMPDISJOINT | ⚠️ | `submittedSeqId_` / `completedSeqId_` waterline exists; invalid query type, exact `GetDataSize`, and Windows D3D9-visible zero-initialisation behaviour still need Wine-oracle PE conformance coverage |
| Clip planes | ✅ | `transformClipPlane()`, `DrawUniforms.clipPlanes` |
| MSAA | ✅ | `sampleCount()`, `RenderTargetAttachment.sampleCount` |
| COM: `IDirect3D9` — full factory interface | ⚠️ | Method surface exists; PE code gates `Direct3DCreate9()` Ex QI, but Wine conformance coverage and stricter validation are still pending |
| COM: `IDirect3DDevice9` — full device method surface | ✅ | All 40+ methods |
| COM: `IDirect3DSwapChain9` | ✅ | Present, backBuffer, depthStencilSurface |
| COM: `Direct3DCreate9(sdkVersion)` | ✅ | Returns `nullptr` for wrong SDK version |
| TLA+ `SeqIdSafety` / `BoundedInflight` / `QueryResolutionSafety` assertions | ✅ | `DXMT_ASSERT` with `// TLA+:` comments |
| `IDirect3D9Ex` — `Direct3DCreate9Ex`, `GetAdapterModeCountEx`, `EnumAdapterModesEx`, `GetAdapterDisplayModeEx`, `GetAdapterLUID`, `CreateDeviceEx` | ⚠️ | Method surface exists; PE factory/device Ex QI is now gated by creation mode, while display-mode validation, `CreateDeviceEx` mode validation, exact HRESULT propagation, and Wine `d3d9ex.c` coverage remain pending |
| `IDirect3DDevice9Ex` — `CheckDeviceState`, `ResetEx`, `PresentEx`, `SetMaximumFrameLatency`, `GetMaximumFrameLatency`, `WaitForVBlank`, `CheckResourceResidency`, `GetGPUThreadPriority`, `SetGPUThreadPriority`, `SetConvolutionMonoKernel`, `ComposeRects`, `CreateRenderTargetEx`, `CreateOffscreenPlainSurfaceEx`, `CreateDepthStencilSurfaceEx`, `GetDisplayModeEx` | ⚠️ | Method surface exists; device Ex QI is now gated by parent factory. **T2 (2026-05-08)**: `ResetEx` now mirrors viewport/scissor reset and clears `deviceNotReset_` on success (previously success was ignored); `PresentEx` now gates on `D3DERR_DEVICELOST`. **T4 (2026-05-08)**: shared-handle SYSTEMMEM 1-mip texture and offscreen surface paths land. Still needed: swap-chain Ex exposure, full `CreateOffscreenPlainSurfaceEx` shared-handle path (todo_wine in Wine itself), DEFAULT-pool shared handle, broader Wine `d3d9ex.c` coverage. |

**The D3D9 layer is functionally broad but no longer classified as complete.**
The review of Wine's D3D9 tests is treated as a Windows D3D9 behavioural-oracle
review. It added stricter conformance requirements for Ex exposure, PE
auxiliary exports, state blocks, public COM refcounts, factory validation,
format conversion, multisample quality-level behaviour, presentation-parameter
validation, D3D9Ex user-memory resources, shared-handle policy, exact HRESULT
propagation, query validation, and lost-device/reset behaviour.

---

## Backend Layer

| Area | Status | Notes |
|---|---|---|
| `BackendDevice` interface + sim backend | ✅ | sim |
| `MTLDevice` init + `MTLCommandQueue` | ✅ | metal |
| Command queue ring: 32 slots, `kMaxQueuedChunks=31`, Wine/encode/finish threads | ✅ | metal; present frame-latency tokens enforce frame pacing separately |
| Architecture concurrency model (`R-ARCH-6.*`) | ⚠️ | Queue, encoder, resource lifetime, present latency, and query sequence safety have TLA+ coverage and implementation assertions. **Wave 1+2 closed (2026-05-08/09):** `EncoderLifecycle.tla` upgraded to exact `lastReadHandles`/`lastWriteHandles : SUBSET Handles` (T1 `79022c7`, R-VERIF-4.4); `ConcurrentProgressSignals.tla` proves `NoQueryWaitBlocksPresent` / `NoFrameLatencyBlocksQuery` / `NoRingPressureBlocksPresentCompletion` liveness with `PacingOrdering` invariant (T2 `9243627`, R-ARCH-6.8/6.9); cross-references in design/comparison docs (`49a33cf`). Remaining: end-to-end observer evidence for fire-and-forget submission classes, ring back-pressure, explicit wait classes, and sidecar worker publication still needs an integrated acceptance pass |
| Ring allocators: `RingArena` for argbuf, replayStore, staging, copyTemp | ✅ | metal |
| Clear-as-load-action folding | ✅ | metal |
| Render-target change → encoder split | ✅ | metal |
| Encoder merging with exact hazard tracking | ✅ | metal; Bloom overlap remains diagnostic only for false-positive measurement |
| PSO cache: `ShaderVariantKey`, async compile, `MTLBinaryArchive` disk cache | ✅ | metal |
| DSS cache: `DepthStencilKey` + `StencilFaceKey` | ✅ | metal |
| FFP shader generator: `makeFfpVertexSource()` + `makeFfpPixelSource()` | ✅ | metal |
| Half-pixel offset in VS | ✅ | metal |
| Pixel shader texture V orientation contract | ✅ | default programmable `texld` path preserves D3D V; `DXMT_DEBUG_FORCE_PIXEL_V_FLIP` is debug-only |
| Alpha test `discard_fragment()` | ✅ | metal |
| Clip plane `[[clip_distance]]` | ✅ | metal |
| Argument buffer layout: `DrawUniforms` | ✅ | metal |
| `MTLBuffer` / `MTLTexture` allocation | ✅ | metal |
| `mapBuffer` DISCARD / NOOVERWRITE / plain | ✅ | metal |
| Deferred destroy: `destroyPending` + `Pool::reclaimCompleted()` | ✅ | metal |
| Back buffer `DontCare` after present | ✅ | metal |
| `CAMetalLayer` swap chain, `nextDrawable`, blit, vsync | ✅ | metal |
| Encode + finish threads | ✅ | metal |
| Surface ops: SurfaceCopy, StretchRect, Readback, ColorFill | ✅ | metal |
| MSAA: multisample + resolve textures | ✅ | metal |
| Wine bridge: `WinemetalApi` (shader-only — 4 fields; window/layer removed) | ✅ | metal |
| Shader compilation thunk: `dxmt9_winemetal_compile_shader()` | ✅ | metal |
| WSI: `macdrv_get_cocoa_view` dlsym + lazy `CAMetalLayer` attach on first present | ✅ | metal; `encodePresent` lazy-creates via `dispatch_sync` to main thread; no custom Wine fork required for the DLL override path |
| `setMaxFrameLatency()` wired to `CAMetalLayer.maximumDrawableCount` | ✅ | metal |
| Imported-record replay data boundaries | ⚠️ | `commit_chunk` validates records through `device_c_record_utils`, bulk-retains resources, and coalesces draw runs through `ImportedChunkView` / `ImportedRecordView` helpers. `chunk_record_spec` pins POD/layout invariants and `chunk_record_import_spec` pins fixed/variable record validation, multi-record iteration, record-count mismatch, truncated tail, draw-run boundary scans, run-param conversion, resource-retention derivation, and replay-category classification for draw/apply/barrier/readback records. Remaining audit is GPU-side barrier/hazard ordering with fake backend or queue instrumentation against R-BACK-2.14-R-BACK-2.27 |
| Bridge op budget / hot-path batching | ⚠️ | `dxmt9-bridge-ops-spec` now pins generated bridge opcode count/range and keeps DOD chunk bridge ops distinct from legacy per-draw calls. Runtime benchmark counters by workload class are still required for R-BENCH-2.3-R-BENCH-2.5 |
| **D3DBC → MSL translation**: SM2/SM3 arithmetic, texture, flow control (IF/ELSE/ENDIF, LOOP/ENDLOOP, REP/ENDREP, CALL/RET/LABEL), transcendental (SINCOS, LOG, EXP), comparison (SGE, SLT), matrix (M4x4, M4x3, M3x4, M3x3, M3x2), MOVA | ✅ | metal; R-BACK-4.1 |
| Per-frequency draw-uniform split (`R-BACK-12.1`–`R-BACK-12.21`) | ✅ | Landed: `VsConsts`/`PsConsts`/`FfpVsConsts`/`FfpPsConsts`/`DrawVolatile` host structs + MSL prelude (A2), `setVertexBytes` bridge (A1), shader translator routing (B1), backend `DirtyMask` + range counters at chunk import (C1), encoder per-stage bind + `setVertexBytes` push (C2), `uniform_*` perf counters (C3), layout + dirty unit specs (D1, D2). Measured (offscreen-heavy@256, 3 reps): `transient_upload_bytes` 87 MB → 1.17 MB (−98.7%), `uniform_build_cpu_ms` 380 → 15 (−96%), pixel-identical output, fps within noise of baseline. **Cleanup closed (2026-05-08/09):** encoder `LocalDirtyState` replaced with `uniform::DirtyState` (E1 `e0ea2e2`); legacy `DrawUniforms` host struct + `buildDrawUniforms` removed (E2 `bf2ca5e`, `86131d6`). |
| Submission grain: 1 chunk → N MTLCommandBuffer chain (`R-BACK-2.29`–`R-BACK-2.32`) | ❌ | Specs landed 2026-05-10 after SFIV measurement showed encode_chunk_cpu (69ms mean) and completion_wait (69ms mean) both saturating frame budget at command_buffers=1/frame. Evidence: `docs/sfiv-benchmark-measurement.md`, `docs/perfomance-bottleneck.md` "Submission grain bottleneck" section, `docs/architecture-comparison.md §G`. **Required work:** mid-chunk split policy in `encodeChunk()` (deterministic per-N or per-render-pass trigger), queue tracker handling of sub-CB chain with single seqId, present-tail invariant enforcement (R-BACK-2.30), reclaim-on-last-sub-CB invariant (R-BACK-2.32). **Verification:** `CommandQueue.tla` extension to model sub-CB chain, A/B experiment varying mid-chunk commit frequency on SFIV with `gpu_command_buffer_time_*_ms` (M4) vs `completion_wait_ms` comparison. **Risk:** Apple Silicon TBDR penalty for small sub-CBs (tile flush per commit) — split threshold is itself a tuning question that empirical measurement must close. |
| Render-pass load/store action policy (`R-BACK-15.1`–`R-BACK-15.16`) | ⚠️ | `specs/backend/render-pass-actions/` (commit `ebd7bad`) adopted as the contract. **Landed (G+H batches, 2026-05-08/09):** `dxmt9_perf_counters` now emits `render_pass_load_action_*` / `_store_action_*` / `tile_preservation_bytes` (G1 `c3bd28b`, G3 `4b124aa`); `tests/native/backend/render_pass_actions_spec.cpp` filled with 8-case matrix (G2 `6a0ade4`, G4 `2da64ca`); `CommandQueue::touchedColorHandles_` set + API (H2 `4d77934`); depth look-ahead end-of-chunk + texture-sample (H1 `ba2ff2f`); color first-use DontCare-load + touched mark/invalidate (H3 `c9ba806`, R-BACK-15.4/5); B+C integration tests (H4 `a7a2aee`, R-BACK-15.4-7). **Still needed:** `R-BACK-15.6` cross-frame correctness for queue-local retention, `R-BACK-15.7`/`15.8` live-out depth-stencil DontCare-store, performance contract validation (≥30% Load reduction, ≥50% depth Store reduction, `completion_present_wait_ms` ≥20% on SFIV). |
| Argument-buffer hybrid Stage 2 (`R-BACK-12.22`–`R-BACK-12.26`) | ❌ | Stage 1 (per-frequency UBO + setVertexBytes) must land first. Stage 2 layers an Apple-Silicon argbuf path that consolidates stable per-frame regions (VsConsts, PsConsts, FfpVs/Ps, tex/sampler) into one argbuf at slot 30 while keeping `DrawVolatile` on `setVertexBytes` and vertex streams on direct binding. Missing: capability gate against `argumentBuffersTier ≥ 2` + `MTLGPUFamilyApple3`, per-encoder argbuf allocator, dirty-mask sub-region writer, MSL emitter variant for argbuf-aware bindings, conformance proof of bit-identity vs Stage 1 (shader-runner equality), `argbufHybrid*` counters. |
| Tile-shader FFP fast path (`R-BACK-13.1`–`R-BACK-13.6`) | ❌ | Apple-Silicon-only acceleration of FFP fog / alpha-test / A2C via `MTLTileRenderPipelineState` instead of fragment-stage discard. Missing: GPU-family capability gate, `makeFfpTilePixelSource()` generator, tile-mode bit in PSO key, per-pass selector that falls back to portable on precision/unsupported corners, `tileFfpPassCount` / `portableFfpPassCount` / `tileFfpFallbackByReason` counters, shader-runner readback equality between tile and portable variants. Conformance evidence taken from portable path; tile path judged by bit-identity. |
| `MTLHeap` small-resource pooling (`R-BACK-5.9`, `R-BACK-5.10`, `R-BACK-14.*`) | ❌ | D3D9 small-texture working set (lightmaps, decals, glyph atlases, particle sprites) currently hits direct `newTextureWithDescriptor` per resource with per-resource residency. Missing: heap families (`priv-tex` / `shared-tex-um` / `shared-buf`) with geometric capacity growth, eligibility threshold (≤64 KB + usage compatible), per-encoder `usedHeaps` set with `useHeap:` elision of per-resource residency, `heap.{family}.heapCount` / `bytesAllocated` / `directFallbackCount` counters, deferred-destroy gate by `completedSeqId`. |
| Pool×Usage → storage-mode unified-memory mapping (`R-BACK-5.7`, `R-BACK-5.8`) | ⚠️ | Existing `R-BACK-5.1` covers DEFAULT/DYNAMIC mapping; the matrix in `R-BACK-5.7` extends this to MANAGED/SYSTEMMEM/SCRATCH and adds a `hasUnifiedMemory` branch (`MANAGED` → Shared on Apple Silicon, no staging copy). Missing: explicit per-pool storage selection at create-time, removal of the staging copy on Apple Silicon for MANAGED, `D3DLOCK_DISCARD` fallback to fresh-allocate when ring is empty (per `R-BACK-5.8`), regression evidence that MANAGED upload bandwidth is bypassed on M1+. |
| Pipeline cache prewarming from `MTLBinaryArchive` (`R-BACK-3.7`, `R-BACK-3.8`, `R-BACK-4.8`) | ❌ | Today PSO/shader-function caches populate lazily on first draw, producing visible cold-start stutter. Missing: device-init full-archive load mode (default for shipping), prewarm-mode runtime config (full/lazy/disabled), cross-process archive identity (`${cache_root}/dxmt9-shaders.metallib-archive` per device family with file locking), `prewarmEntriesLoaded` / `prewarmLoadNs` / `coldCompileCountAfterWarm` counters, present-diagnostic exposure of archive path/version, conformance evidence that archive misses fall through to compile-and-write without blocking device creation. |

**The backend layer is functionally broad, but the newly specified
data-oriented replay boundary still needs an explicit audit and unit-test split.**

---

## Wine PE / `winemetal` Deployment Layer

The spec now matches upstream DXMT's deployment shape:
`d3d9.dll` imports `winemetal.dll`, and `winemetal.dll` dispatches to the
paired Wine unix module `winemetal.so`. The older `dxmt9.dll` / `dxmt9.so`
bridge naming is no longer part of the target spec.

| Area | Status | Notes |
|---|---|---|
| C ABI bridge header `device_c.h` | ✅ | All factory / device / resource types |
| Provider-side C ABI wrappers in `src/d3d9/` | ✅ | `dxmt9c_*` provider + bridge sources are present |
| `winemetal.so` unix module | ✅ | `src/winemetal/unix/meson.build` builds `winemetal.so` and links Wine `winemac.so` / `ntdll.so` when configured |
| `d3d9.dll` as user-facing PE DLL | ⚠️ | Source target exists in `src/win32/`; build/runtime validation still required |
| `winemetal.dll` as shared Wine builtin PE bridge | ⚠️ | Source target exists in `src/winemetal/`; Wine builtin postprocess/runtime validation still required |
| PE bridge ↔ unix module thunk mechanism | ⚠️ | Code-gen + bridge sources are present; needs end-to-end Wine smoke verification |
| `dxmt9.dll` / `dxmt9.so` legacy bridge naming | ❌ | Removed from target spec; stale references should be treated as documentation drift |

---

## Verification Layer

| Area | Status | Evidence |
|---|---|---|
| TLA+ specs: CommandQueue, QueueLifecycleRefinement, PresentFrameLatency, ResourceLifetime, EncoderLifecycle, QuerySeqId, ConcurrentProgressSignals | ✅ | ConcurrentProgressSignals added 2026-05-09 (T2 closes G1: pacing-axis independence under R-ARCH-6.8/6.9) |
| All seven specs model-checked by TLC — zero errors | ✅ | EncoderLifecycle now uses exact `lastReadHandles` / `lastWriteHandles : SUBSET Handles` instead of Boolean `hazardFlag` (T1 closes G2 / R-VERIF-4.4); `AtMostOneEncoder` tautology removed (T1 closes G3 / R-VERIF-4.1); `BloomNeverForcesSplit` invariant proves Bloom signal cannot trigger an encoder split (T4 closes G4 / R-BACK-2.28). Distinct states post-changes: ConcurrentProgressSignals 6,172, EncoderLifecycle 3,403 (Bloom-extended). |
| Pacing-axis independence (`R-ARCH-6.8` / `R-ARCH-6.9`) | ✅ | `ConcurrentProgressSignals.tla` proves `NoQueryWaitBlocksPresent`, `NoFrameLatencyBlocksQuery`, `NoRingPressureBlocksPresentCompletion` liveness with `PacingOrdering` invariant. |
| Hazard model: exact handle sets, not Bloom (`R-BACK-2.28`) | ✅ | `EncoderLifecycle.tla` `MergeRenderDraw` checks RAW/WAR/WAW set intersection; `BloomNeverForcesSplit` invariant proves split path ignores Bloom signal; `bloomFalsePositiveCount` advances only when exact disagrees (diagnostic-only role formal). |
| `QueueLifecycleRefinement` concrete queue lifecycle model checked and asserted | ✅ | `QueueLifecycleController` debug invariants cover `readySlots`, `pendingCompletion`, `completedSeqQueue`, inline completion, empty commit, `waitForSequence`, and shutdown paths |
| `PresentFrameLatency` present-token model checked and asserted | ✅ | `completedPresentSeqQueue_` advances `presentCompletedSeqId` only after `completedSeqId`; `presentBoundary()` asserts `MAX_FRAME_LATENCY` wait return safety |
| `SeqIdSafety` asserted with `// TLA+:` label | ✅ | `Device` submitted/completed sequence guards and queue completion watermarks |
| `QueryResolutionSafety` asserted | ✅ | `Query::getData()` |
| `BoundedInflight` asserted | ✅ | sim + metal `QueueLifecycleController::commitCurrentChunk()` |
| `NoUseAfterFree` asserted with `// TLA+:` label | ✅ | `Pool::reclaimCompleted()` |
| `RingSafety` asserted with `// TLA+:` label | ✅ | `RingArena::allocateBytes()` + slot ring |
| `EncodeSafety` asserted with `// TLA+:` label | ✅ | encode loop |
| `WineCommit` action mapping comments | ✅ | `QueueLifecycleController::commitCurrentChunk()` and `CommandQueue::submit*()` paths |
| DOD wire-schema acceptance | ⚠️ | Existing chunk tests cover many POD/layout and import validation cases; R-VERIF-7.1 now tracks full wire-schema acceptance for size/alignment, command IDs, version constants, offsets, and variable-tail rules |
| Queue observer / fake-backend verification | ❌ | R-VERIF-7.3 requires deterministic queue-facing evidence for chunk seq IDs, retained handles, replay categories, barrier/readback boundaries, and encoded command order without relying on Metal timing |
| DXMT concept mapping acceptance | ⚠️ | README mapping exists; R-VERIF-7.4 requires explicit implementation-owner and test evidence for each hot-path concept before calling DXMT merge readiness complete |

**The verification layer is partial for DXMT merge readiness.** Existing
R-VERIF-1.x through R-VERIF-6.x evidence remains complete, while new R-VERIF-7.x
acceptance tracks wire-schema, fake-backend/queue-observer, bridge-budget, and
DXMT concept-mapping evidence.

---

## Tests Layer

⚠️ Partial. The native test runner, expanded shader corpus, native regressions,
upstream corpus sync automation, and unit-first stateless transform suites now
cover broad shader/backend behaviour. The layer remains partial because the
extended runtime probe layer and Wine runtime execution of the PE conformance
suite are still open.

| Area | Status | Spec |
|---|---|---|
| WSI integration test (`tests/integration/wsi_present/`) | ✅ | Heroic Wine 11.5 builtin path passes the full 180-frame `wsi_present_x64.exe` smoke |
| Stateless shader transform unit suites | ✅ | `shader_transform_spec` covers D3DBC decode/classification fixtures, stage/version decode, comment skipping, fixed operand-count decode, texture-use classification, predicated/control token decode, register kind/index semantics, swizzle/source/dest/write-mask/relative-addressing tokens, sampler register slots, ps_3_0 texcoord semantics, vs_3_0 output semantics, default no-flip contracts, write masks/swizzles/source modifiers, IF/ELSE, LOOP/REP/CALL/LABEL/RET lowering, deterministic unsupported relative-addressing errors, and a broad opcode source-contract matrix including MAD/DP/CMP/SLT/SGE/POW/SINCOS/LOG/EXP/matrix/TEXLDD/TEXLDL |
| Stateless state-to-draw-data unit suites | ✅ | `state_draw_transform_spec` covers `makeCanonicalDrawStateFromState()` and fixture/offline `makeDrawDescFromState()` for draw args, viewport/scissor, render/sampler/TSS copy, FFP keys, transforms, clip planes, constants, bytecode shader refs, texture/resource handles, stream/index bindings, vertex decl/FVF, and RT/DS attachment variants |
| Stateless key/descriptor unit suites | ✅ | `backend_key_descriptor_spec` covers `buildDrawUniforms()`, depth/stencil keys, and pure `SamplerSnapshot` → `WMTSamplerInfo` descriptor mapping. `backend_pipeline_key_spec` covers blend enable, RGB/alpha op/factor fallback, MRT color-write defaults/overrides, force-visible override, sampler texture/filter flags, FVF vertex-layout hashing, PSO hash responsiveness, and sRGB-compatible pixel format conversion |
| `shader_runner_dxmt9` backend | ✅ | R-TEST-1.1 |
| `shader_runner_dxmt9` extended probe layer | ⚠️ | dxmt9-local runtime probes now cover texture setup/readback, dependent texture read, one VS geometry path, viewport-bounded and nonzero-origin rasterization, half-pixel edge masks, and two color-write render-state interactions: `texture/dxmt9_texture_2x2.shader_test`, `texture/dxmt9_dependent_texture_read.shader_test`, `vs_specific/dxmt9_vs_color_triangle.shader_test`, `viewport/dxmt9_viewport_vs_triangle.shader_test`, `viewport/dxmt9_viewport_nonzero_origin.shader_test`, `viewport/dxmt9_half_pixel_solid_rect.shader_test`, `render_state/dxmt9_color_write_mask.shader_test`, and `render_state/dxmt9_color_write_rgb_preserves_alpha.shader_test`. Broader mip/4x4/LOD, alpha/oDepth/MRT/fog/sRGB coverage remains open; `render_state/dxmt9_alpha_test_readback.shader_test` is tracked as a failing regression probe |
| Expanded `.shader_test` corpus (arithmetic, comparison, flow control, transcendental, matrix, source modifiers, texture, FFP sanity/alpha test) | ✅ | 27 tracked shader tests: 26 passing tests, one failing alpha-test readback probe, and eight dxmt9-local runtime extended probes in the passing Meson shader-corpus suite |
| Provenance blocks on corpus files | ✅ | R-TEST-9.1 |
| `MANIFEST.toml` + `check_manifest.sh` + `check_drift.sh` + `sync_corpus.sh` | ✅ | R-TEST-10.1–10.2, R-TEST-7.3; corpus manifest now records `models`, `opcodes`, license provenance, and `shader_corpus_tool.py gaps` reports model/opcode coverage gaps |
| Native `core_spec` coverage for resource mapping / present-readback / clip planes / MSAA / Ex wrappers / programmable texture orientation | ✅ | R-TEST-4.3-R-TEST-4.4, R-TEST-5.1–5.2, R-TEST-6.1 |
| Fixed-function `.shader_test` files | ✅ | `ffp/alpha_test.shader_test` and native fixed-function coverage |
| Wine `visual.c` oracle coverage (ps_1_x, FFP) | ✅ | native clean-room oracle coverage for lighting, fog, texture transform, texop, FFP varying, sanity, alpha, BEM, ps_1_4, and vs_1_1 source contracts |
| Wine `device.c` / `d3d9ex.c` / `stateblock.c` conformance subset | ⚠️ | `tests/conformance/d3d9/MANIFEST.toml` lists 31 Wine-oracle PE conformance cases with DoD/acceptance criteria and `dxmt9-d3d9-conformance-manifest-check` validates lane/arch evidence. Current manifest status is 23 scaffolded, 6 failing, and 2 partial. **Source organization (T8/T9, 2026-05-08/09):** `d3d9_conformance.c` (originally 1,777 LOC) split into a thin driver (73 LOC) + per-domain `d3d9_conformance_{device,resource,swapchain,query_stateblock}.c` linked to a single `dxmt9-d3d9-conformance.exe` (T8 `b2c4c75`); standalone per-test executables normalized to single `d3d9_*` prefix (drop `_x64` suffix from 6 files, add `d3d9_` prefix to 3 bare files; T9 `739a080`, target names preserved for external invocation compatibility). Focused x64 app-local export/auxiliary runtime evidence passes. The first device-backed app-local run now reaches the provider with 328 checks, 26 failures, and 0 skips; failing groups are factory validation, present-parameter validation, Ex create/reset, private-data resource wrappers, Ex shared-handle policy, and creation-failure out pointers |
| Half-pixel offset exact-coverage test | ✅ | `testHelpers()` + `testRasterStateCoverage()` |
| Winding / depth tests | ✅ | `testRasterStateCoverage()` |
| Full upstream corpus sync | ✅ | `sync_corpus.sh` + provenance drift report |

### Unit-First DoD Checklist

| DoD item | Status | Evidence / remaining work |
|---|---|---|
| Shader transforms are testable from bytecode to deterministic source/IR without Wine, Metal, or GPU execution | ✅ | `shader_transform_spec` covers lower-level D3DBC decode/classification fixtures, register slots, semantic mapping, no-flip contracts, modifiers/masks, flow control, unsupported relative-addressing error contract, and broad opcode lowering source contracts |
| Core draw data is built by pure state-to-value helpers | ✅ | `makeCanonicalDrawStateFromState()` plus draw-run helpers build production `CanonicalDrawState` / `DrawRunDesc` data, and large constants/matrices/clip planes now flow through `DrawUniformPayload` handles instead of hot draw-state records; `makeDrawDescFromState()` remains fixture/offline only. `state_draw_transform_spec` covers draw args, constants, shader refs, resource bindings, stream/index bindings, vertex decl/FVF, RT/DS attachments, viewport/scissor, render/sampler/TSS, FFP keys, transforms, and clip planes |
| Backend cache inputs are verified as deterministic value descriptors | ✅ | `backend_key_descriptor_spec` covers uniforms/depth-stencil/sampler descriptors; `backend_pipeline_key_spec` covers blend/MRT color-write key mapping, PSO hash responsiveness, sampler texture/filter flags, FVF layout hashing, and sRGB format mapping |
| Chunk wire records have data-driven validation independent of replay side effects | ✅ | `device_c_record_utils` + `chunk_record_import_spec` validate fixed records, variable Clear/SetConst tails, invalid/truncated records, and draw-run parameter conversion |
| Imported chunk replay has explicit boundary types and unit-testable replay decisions | ✅ | `ImportedChunkView` / `ImportedRecordView`, draw-run scan helpers, replay-category classification, resource-retention derivation, exact read/write hazard extraction, barrier/synchronous-read boundary decisions, and deterministic hazard-scope reset/continuation are split and tested without Metal |
| Bridge-op count/order evidence exists for DOD bridge shape | ⚠️ | `dxmt9-bridge-ops-spec` pins the generated bridge opcode table and DOD chunk op placement. **Counter infrastructure landed (I1B `db8bdb9`, 2026-05-08):** perf counters refactored into a data-driven table enabling per-workload classification without code changes. Workload-level runtime bridge-op counters and benchmark JSON still required for R-BENCH-2.3-R-BENCH-2.5 |
| Allocation/capacity evidence exists for DOD hot paths | ⚠️ | `dxmt9-dod-replay-observer-spec` pins warmed ChunkSlot capacity reuse and uniform interning; `dxmt9-allocation-counter-spec` verifies real perf-counter emission for Metal buffer allocation counts/bytes. **Perf infrastructure expanded (I1–I5 wave, 2026-05-08):** chunk admit/reject + ring arena heap fallback counters (I2 `1fd2d7e`); P50/P95/P99 percentile rings for timing counters (I3 `8bcf2ef`); per-frame perf snapshot mode (I4 `08c4ac2`); 4 synthetic workload probes (I5 `f8547e1`). A general heap allocation interposer for arbitrary hot paths remains future work |

---

## Experiments Layer

⚠️ Partial. The runner, launcher harness, output layout, one verified local
bootstrap entry, and the full initial real-application catalogue from
R-WILD-3.1 exist. The layer remains partial because the current implementation
depends on the Wine builtin path rather than the native macOS injection path
described by R-WILD-1.2.

| Area | Status | Spec |
|---|---|---|
| `experiments/CATALOGUE.toml` + launcher tree scaffolded | ✅ | R-WILD-5.1 |
| Wine launcher injection harness (`run_experiment.py`, launcher scripts, Heroic staging) | ⚠️ | R-WILD-1.2 |
| Internal backbuffer frame dump + SSIM comparison + `result.json` output | ✅ | R-WILD-2.3, R-WILD-4.1 |
| Bootstrap verified entry: `dxmt9-wsi-present-local` on Heroic Wine 11.5 | ✅ | local workflow validation |
| Verified real application entries: `dx-sdk-basichlsl`, `dx-sdk-tutorial07`, `dx-sdk-hdrformats`, `dxut-simple-sample`, `irrlicht-managed-lights` | ✅ | Heroic Wine 11.5, direct capture, SSIM 1.0000 |
| Exploratory commercial entry: `anno-1404-gold` | ⚠️ | supported on Heroic `Wine-11.6-DXMT`; plain `Wine-11.6` is research-only due to Wine `d3dx10_43` aborts |
| Initial catalogue from R-WILD-3.1 staged and verified | ✅ | All five required feature groups covered |
| Reference screenshots for initial catalogue entries | ✅ | R-WILD-4.1 |

---

## Benchmarks Layer

No benchmarks exist yet. All R-BENCH-1.x through R-BENCH-5.x are not started,
and the architecture bottleneck/concurrency evidence for R-ARCH-2.6-R-ARCH-2.7
and R-ARCH-6.* is still missing.

| Area | Status | Spec |
|---|---|---|
| `dxmt9-bench` harness | ❌ | R-BENCH-1.1 |
| Draw call throughput workload | ❌ | R-BENCH-2.2 |
| Bridge operation budget counters | ⚠️ | `dxmt9-bridge-ops-spec` provides static opcode-budget evidence; workload-level runtime counters and benchmark JSON still required for R-BENCH-2.3-R-BENCH-2.5, R-BENCH-5.3 |
| Architecture bottleneck/concurrency counter baselines | ❌ | R-ARCH-2.6-R-ARCH-2.7, R-ARCH-6.*, R-BENCH-2.6 |
| PSO compile cold/warm workload | ❌ | R-BENCH-2.1 |
| Reference stack baselines (wined3d, DXVK+MoltenVK) | ❌ | R-BENCH-3.1 |
| `bench_compare.sh` regression script | ❌ | R-BENCH-4.3 |

---

## D3D8 Layer (`d3d8.dll`)

No implementation exists yet. All R-D3D8-1.x through R-D3D8-11.x are not started.

| Area | Status | Spec |
|---|---|---|
| `Direct3DCreate8` entry point + `ValidatePixelShader` stub | ❌ | R-D3D8-1.1, R-D3D8-1.2 |
| `IDirect3D8` factory (delegate to D3D9) | ❌ | R-D3D8-2.1–2.3 |
| Vertex shader handle table + `CreateVertexShader` | ❌ | R-D3D8-3.1–3.6 |
| Pixel shader handle table + `CreatePixelShader` | ❌ | R-D3D8-3.7–3.10 |
| D3D8 vertex declaration parser (`D3DVSD_*` → `D3DVERTEXELEMENT9`) | ❌ | R-D3D8-4.1–4.6 |
| State block token table | ❌ | R-D3D8-5.1–5.6 |
| `SetRenderTarget(RT, DS)` split | ❌ | R-D3D8-6.1–6.3 |
| `CreateRenderTarget` / `CreateDepthStencilSurface` signature fix | ❌ | R-D3D8-7.1–7.2 |
| `CreateImageSurface` → `CreateOffscreenPlainSurface` | ❌ | R-D3D8-7.3 |
| `CopyRects` → loop of `UpdateSurface` | ❌ | R-D3D8-7.4 |
| TSS → sampler state remapping (10 states) | ❌ | R-D3D8-8.1–8.2 |
| `Present` / `GetBackBuffer` signature fix | ❌ | R-D3D8-9.1–9.2 |
| Capabilities clamping to D3D8 limits | ❌ | R-D3D8-10.1–10.2 |
| Pass-through methods + resource wrapper unwrapping | ❌ | R-D3D8-11.1 |

---

## D3D7 / DirectDraw 7 Layer (`ddraw.dll`)

No implementation exists yet. All R-D3D7-1.x through R-D3D7-10.x are not started.

| Area | Status | Spec |
|---|---|---|
| `DirectDrawCreate` / `DirectDrawCreateEx` entry points | ❌ | R-D3D7-1.1–1.4 |
| `IDirectDraw7` (SetCooperativeLevel, CreateSurface, etc.) | ❌ | R-D3D7-2.1–2.11 |
| `IDirect3D7` (EnumDevices, CreateDevice, CreateVertexBuffer) | ❌ | R-D3D7-3.1–3.6 |
| `IDirectDrawSurface7` classification (primary / depth / tex / RT / cube / mip) | ❌ | R-D3D7-4.1–4.9 |
| `IDirectDrawSurface7` ops (Lock, Unlock, Blt, BltFast, Flip) | ❌ | R-D3D7-5.1–5.9 |
| `IDirect3DDevice7` draw calls (DrawPrimitive, DrawPrimitiveVB, etc.) | ❌ | R-D3D7-6.1–6.7 |
| `IDirect3DDevice7` state (SetRenderTarget, SetTransform, SetRenderState, etc.) | ❌ | R-D3D7-6.8–6.18 |
| `IDirect3DVertexBuffer7` (Lock, Unlock, GetDesc) | ❌ | R-D3D7-7.1–7.5 |
| Vertex count → primitive count conversion | ❌ | R-D3D7-8.1 |
| `DDPIXELFORMAT` → `D3DFORMAT` parser | ❌ | R-D3D7-9.1 |
| D3D7 render state / transform mapping tables | ❌ | R-D3D7-6.12, R-D3D7-6.11 |
| Strided vertex packing (`DrawPrimitiveStrided`) | ❌ | R-D3D7-6.4–6.5 |
| Mip chain traversal (`GetAttachedSurface`) | ❌ | R-D3D7-4.8 |

---

## Summary

| Layer | Status |
|---|---|
| Core | partial |
| Backend | complete |
| Wine PE / `winemetal` deployment | partial |
| Verification | complete |
| Tests | partial |
| D3D8 (`d3d8.dll`) | not started |
| D3D7 / DirectDraw 7 (`ddraw.dll`) | not started |
| Experiments | partial |
| Benchmarks | not started |

---

## Next priorities

| Priority | Work | Spec anchor |
|---|---|---|
| 1 | Extend the `shader_runner_dxmt9` runtime probe layer beyond texture / dependent-read / VS color / viewport / half-pixel / color-write probes to mip/4x4/LOD, alpha/oDepth/MRT/fog/sRGB cases, and fix the tracked alpha-test readback regression | R-TEST-1.7–R-TEST-1.10 |
| 2 | Promote the PE conformance scaffold inventory by fixing the newly exposed app-local conformance failures, then running app-local and builtin Wine lanes for all scaffolded cases | R-TEST-12.1, R-TEST-12.20 |
| 3 | Finish factory HRESULT parity and validation coverage, including `CheckDeviceFormatConversion`, multisample quality levels, device-type enum handling, and any selected optional export-profile stubs beyond the current auxiliary set | R-CORE-1.9, R-CORE-1.11-R-CORE-1.15, R-TEST-12.9-R-TEST-12.10, R-TEST-12.15-R-TEST-12.16 |
| 4 | Implement and verify D3D9Ex user-memory texture/offscreen-surface paths and shared-handle error policy | R-CORE-4.11-R-CORE-4.12, R-TEST-12.11 |
| 5 | Implement Wine-oracle D3D9Ex QI conformance and finish Ex/display/swap-chain validation | R-CORE-1.6, R-CORE-10.2-R-CORE-10.4, R-CORE-10.17, R-CORE-10.18, R-TEST-12.4 |
| 6 | Expand Wine stateblock conformance beyond the current compact scaffold and implement full `D3DSBT_*` masks/resource/reset interactions | R-CORE-3.7, R-CORE-3.8, R-TEST-12.5, R-TEST-12.19 |
| 7 | Expand compact reset/window scaffolds if runtime evidence exposes missing Wine-visible edge cases, then promote device lifetime/refcount, query validation, resource wrapper, and scene scaffolds with runtime evidence | R-CORE-2.6, R-CORE-2.8, R-CORE-4.8-R-CORE-4.10, R-CORE-8.3, R-TEST-12.3, R-TEST-12.12-R-TEST-12.14, R-TEST-12.17-R-TEST-12.18 |
| 8 | Build and verify the upstream-style PE targets: `d3d9.dll` + `winemetal.dll` + `winemetal.so` | d3d9/wsi §6, §9 |
| 9 | Run the Wine WSI smoke on the upstream-style deployment and promote the gap status if it passes | R-TEST-11.3 |
| 10 | D3D8 entry point + IDirect3D8 factory + resource wrappers | R-D3D8-1.1, R-D3D8-2.1 |
