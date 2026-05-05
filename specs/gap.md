# Spec–Implementation Gap

Current state of the codebase vs. the specifications.
Updated against current working tree.

Legend: ✅ implemented · ⚠️ partial · ❌ not started

---

## D3D9 Layer

| Area | Status | Evidence / notes |
|---|---|---|
| Factory, adapter enum, `CheckDeviceFormat` / conversion | ⚠️ | Functional path exists, but PE factory currently needs Windows D3D9-compatible validation, checked by Wine D3D9 tests, for adapter index, valid-vs-invalid device type, fullscreen display format, adapter format allowlist, resource type mapping, `CheckDeviceFormatConversion`, and exact return codes |
| `GetAdapterIdentifier` | ✅ | `Factory::getAdapterIdentifier()` |
| `EnumAdapterModes` | ✅ | `Factory::enumAdapterModes()` |
| `GetAdapterDisplayMode` | ✅ | `Factory::getAdapterDisplayMode()` |
| `GetAdapterMonitor` | ✅ | `Factory::getAdapterMonitor()` |
| `CheckDeviceType` | ⚠️ | HAL path exists; invalid adapter, valid but unavailable device type, invalid enum, and fullscreen display-format return-code parity still need Wine-derived Windows D3D9 conformance coverage |
| `CheckDeviceMultiSampleType` | ⚠️ | Factory + Device path exists; invalid enum, `D3DMULTISAMPLE_NONE` quality level, unsupported sample-count `pQualityLevels`, and Windows D3D9 HRESULT parity still need Wine-derived conformance coverage |
| PE `d3d9.dll` export surface | ⚠️ | The scoped auxiliary set is now implemented with DXVK/D9VK-compatible ordinals: `Direct3DCreate9On12`, `D3DPERF_*`, and `DebugSetMute`. Focused x64 app-local export/auxiliary tests pass. The row remains partial until the broader selected export profile, such as optional `PSGP*` / `DebugSetLevel` compatibility if adopted, is explicitly audited |
| Device lifecycle: Reset, `D3DPOOL_DEFAULT` invalidation | ⚠️ | Functional reset exists; Wine `device.c` / `d3d9ex.c` reset rebinding, higher render-target unbind, viewport/scissor reset, present-parameter validation, `ResetEx` fullscreen-mode relation, and lost-device conformance subset still need verification |
| Fullscreen `createDevice` | ⚠️ | `normalizePresentParameters()` exists; Windows D3D9-compatible `D3DPRESENT_PARAMETERS` validation and exact creation HRESULT propagation are still required |
| Device-lost: trigger + recovery | ✅ | `setDeviceLostObserver()` |
| `TestCooperativeLevel` | ✅ | Returns `D3DERR_DEVICELOST` when lost |
| `CreateAdditionalSwapChain` | ✅ | `Device::createAdditionalSwapChain()` |
| Device state shadow (render / texture / sampler / transform / lights / stencil) | ✅ | `DeviceState`, all `Set*`/`Get*` methods |
| BeginScene / EndScene | ✅ | Nested-call validation |
| StateBlock capture / restore | ⚠️ | Full-state copy exists; `D3DSBT_*` masks, recording invalid-call cases, derived-cache invalidation after `Apply`, and Windows D3D9 behaviours captured by Wine `stateblock.c` need conformance work |
| Hot-path CommandChunk recording and data normalizers | ⚠️ | Chunk records, delta draw packets, `APPLY_STATE`, bulk retention, and draw-run paths exist. `makeDrawDescFromState()` covers state-to-draw pure transforms, and `device_c_record_utils` now covers record validation, `ImportedChunkView` / `ImportedRecordView`, chunk iteration, record-count validation, draw-run scans, replay-category classification, and resource-retention derivation with native tests. Remaining alignment is auditing barrier/hazard behaviour with a fake backend or queue instrumentation against R-CORE-11.14-R-CORE-11.18 |
| DOD / DXMT ownership acceptance | ⚠️ | README concept mapping exists and the chunk wire path is data-oriented. Merge-readiness still needs explicit acceptance that PE state shadow, POD chunk construction, unix import, queue execution, presentation pacing, and deferred resource safety each have matching implementation owners and tests |
| DrawPrimitive, DrawIndexedPrimitive, UP variants | ✅ | All four variants |
| TriangleFan decomposition | ✅ | `decomposeTriangleFanIndices()` |
| Half-pixel offset | ✅ | `halfPixelFixup()` |
| FfpVertexKey / FfpPixelKey generation | ✅ | `makeFfpVertexKey()`, `makeFfpPixelKey()` |
| Shader bytecode storage + hash | ✅ | `ShaderBytecode`, FNV-1a hash cache |
| Format table | ⚠️ | `formatTable()` exists; central explicit classification for all FOURCC/pseudo-formats such as `RESZ` and `NULL` still needs audit |
| `makeDefaultCaps()` | ✅ | R-CAPS-1 through R-CAPS-7 |
| Buffer / Texture / Surface lifecycle | ⚠️ | Pool-based reset behavior exists; public COM refcount/lifetime, common private-data semantics, D3D9Ex user-memory texture/offscreen-surface success paths, and shared-handle/user-memory error codes need Wine-derived `device.c` / `d3d9ex.c` conformance coverage |
| `UpdateSurface`, `UpdateTexture`, `StretchRect`, `ColorFill`, `GetRenderTargetData` | ✅ | Core-side logic |
| Query: EVENT, OCCLUSION, TIMESTAMP, TIMESTAMPFREQ, TIMESTAMPDISJOINT | ⚠️ | `submittedSeqId_` / `completedSeqId_` waterline exists; invalid query type, exact `GetDataSize`, and Windows D3D9-visible zero-initialisation behaviour still need Wine-derived PE conformance coverage |
| Clip planes | ✅ | `transformClipPlane()`, `DrawUniforms.clipPlanes` |
| MSAA | ✅ | `sampleCount()`, `RenderTargetAttachment.sampleCount` |
| COM: `IDirect3D9` — full factory interface | ⚠️ | Method surface exists; PE code gates `Direct3DCreate9()` Ex QI, but Wine conformance coverage and stricter validation are still pending |
| COM: `IDirect3DDevice9` — full device method surface | ✅ | All 40+ methods |
| COM: `IDirect3DSwapChain9` | ✅ | Present, backBuffer, depthStencilSurface |
| COM: `Direct3DCreate9(sdkVersion)` | ✅ | Returns `nullptr` for wrong SDK version |
| TLA+ `SeqIdSafety` / `BoundedInflight` / `QueryResolutionSafety` assertions | ✅ | `DXMT_ASSERT` with `// TLA+:` comments |
| `IDirect3D9Ex` — `Direct3DCreate9Ex`, `GetAdapterModeCountEx`, `EnumAdapterModesEx`, `GetAdapterDisplayModeEx`, `GetAdapterLUID`, `CreateDeviceEx` | ⚠️ | Method surface exists; PE factory/device Ex QI is now gated by creation mode, while display-mode validation, `CreateDeviceEx` mode validation, exact HRESULT propagation, and Wine `d3d9ex.c` coverage remain pending |
| `IDirect3DDevice9Ex` — `CheckDeviceState`, `ResetEx`, `PresentEx`, `SetMaximumFrameLatency`, `GetMaximumFrameLatency`, `WaitForVBlank`, `CheckResourceResidency`, `GetGPUThreadPriority`, `SetGPUThreadPriority`, `SetConvolutionMonoKernel`, `ComposeRects`, `CreateRenderTargetEx`, `CreateOffscreenPlainSurfaceEx`, `CreateDepthStencilSurfaceEx`, `GetDisplayModeEx` | ⚠️ | Method surface exists; device Ex QI is now gated by parent factory, while swap-chain Ex exposure, Ex usage/shared-handle validation, and reset/device-state behaviour need Wine `d3d9ex.c` coverage |

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
| Ring allocators: `RingArena` for argbuf, replayStore, staging, copyTemp | ✅ | metal |
| Clear-as-load-action folding | ✅ | metal |
| Render-target change → encoder split | ✅ | metal |
| Encoder merging with Bloom-filter hazard detection | ✅ | metal |
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
| Bridge op budget / hot-path batching | ❌ | R-BENCH-2.3-R-BENCH-2.5 now require bridge operation counts by class and acceptance that `Set*` / `Draw*` traffic does not regress to one PE/unix call per D3D9 operation |
| **D3DBC → MSL translation**: SM2/SM3 arithmetic, texture, flow control (IF/ELSE/ENDIF, LOOP/ENDLOOP, REP/ENDREP, CALL/RET/LABEL), transcendental (SINCOS, LOG, EXP), comparison (SGE, SLT), matrix (M4x4, M4x3, M3x4, M3x3, M3x2), MOVA | ✅ | metal; R-BACK-4.1 |

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
| TLA+ specs: CommandQueue, QueueLifecycleRefinement, PresentFrameLatency, ResourceLifetime, EncoderLifecycle, QuerySeqId | ✅ | |
| All six specs model-checked by TLC — zero errors | ✅ | TLC 2.19 distinct states: CommandQueue 100, EncoderLifecycle 57, PresentFrameLatency 2,822, QuerySeqId 17,241, QueueLifecycleRefinement 12,584, ResourceLifetime 52,522 |
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
| WSI integration test (`tests/wsi_present/`) | ✅ | Heroic Wine 11.5 builtin path passes the full 180-frame `wsi_present_x64.exe` smoke |
| Stateless shader transform unit suites | ✅ | `shader_transform_spec` covers D3DBC decode/classification fixtures, stage/version decode, comment skipping, fixed operand-count decode, texture-use classification, predicated/control token decode, register kind/index semantics, swizzle/source/dest/write-mask/relative-addressing tokens, sampler register slots, ps_3_0 texcoord semantics, vs_3_0 output semantics, default no-flip contracts, write masks/swizzles/source modifiers, IF/ELSE, LOOP/REP/CALL/LABEL/RET lowering, deterministic unsupported relative-addressing errors, and a broad opcode source-contract matrix including MAD/DP/CMP/SLT/SGE/POW/SINCOS/LOG/EXP/matrix/TEXLDD/TEXLDL |
| Stateless state-to-draw-data unit suites | ✅ | `state_draw_transform_spec` covers `makeDrawDescFromState()` for draw args, viewport/scissor, render/sampler/TSS copy, FFP keys, transforms, clip planes, constants, bytecode shader refs, texture/resource handles, stream/index bindings, vertex decl/FVF, and RT/DS attachment variants |
| Stateless key/descriptor unit suites | ✅ | `backend_key_descriptor_spec` covers `buildDrawUniforms()`, depth/stencil keys, and pure `SamplerSnapshot` → `WMTSamplerInfo` descriptor mapping. `backend_pipeline_key_spec` covers blend enable, RGB/alpha op/factor fallback, MRT color-write defaults/overrides, force-visible override, sampler texture/filter flags, FVF vertex-layout hashing, PSO hash responsiveness, and sRGB-compatible pixel format conversion |
| `shader_runner_dxmt9` backend | ✅ | R-TEST-1.1 |
| `shader_runner_dxmt9` extended probe layer | ⚠️ | dxmt9-local runtime probes now cover texture setup/readback, one VS geometry path, viewport-bounded rasterization, and two color-write render-state interactions: `texture/dxmt9_texture_2x2.shader_test`, `vs_specific/dxmt9_vs_color_triangle.shader_test`, `viewport/dxmt9_viewport_vs_triangle.shader_test`, `render_state/dxmt9_color_write_mask.shader_test`, and `render_state/dxmt9_color_write_rgb_preserves_alpha.shader_test`. Broader mip/dependent-read, nonzero-origin/half-pixel, alpha/oDepth/MRT/fog/sRGB coverage remains open |
| Expanded `.shader_test` corpus (arithmetic, comparison, flow control, transcendental, matrix, source modifiers, texture, FFP sanity/alpha test) | ✅ | 23 passing shader tests including five dxmt9-local runtime extended probes |
| Provenance blocks on corpus files | ✅ | R-TEST-9.1 |
| `MANIFEST.toml` + `check_manifest.sh` + `check_drift.sh` + `sync_corpus.sh` | ✅ | R-TEST-10.1–10.2, R-TEST-7.3 |
| Native `core_spec` coverage for resource mapping / present-readback / clip planes / MSAA / Ex wrappers / programmable texture orientation | ✅ | R-TEST-4.3-R-TEST-4.4, R-TEST-5.1–5.2, R-TEST-6.1 |
| Fixed-function `.shader_test` files | ✅ | `ffp/alpha_test.shader_test` and native fixed-function coverage |
| Wine `visual.c` ports (ps_1_x, FFP) | ✅ | `testVisualDerivedFfpCoverage()` + `testVisualPortCoverage()` |
| Wine `device.c` / `d3d9ex.c` / `stateblock.c` conformance subset | ⚠️ | `tests/d3d9_conformance/MANIFEST.toml` lists 31 Wine-derived PE conformance cases with DoD/acceptance criteria and `dxmt9-d3d9-conformance-manifest-check` validates the manifest. All 31 cases are now scaffolded, including export smoke, shader-validator, D3D9On12 loader-safe probes, query public API probes, resources, stateblock matrix, reset/lost, window/cursor, and device misc. Focused x64 app-local export/auxiliary runtime evidence passes. The first device-backed app-local run now reaches the provider with 328 checks, 26 failures, and 0 skips; failing groups are factory validation, present-parameter validation, Ex create/reset validation, private-data resource wrappers, Ex shared-handle policy, and creation-failure out pointers |
| Half-pixel offset exact-coverage test | ✅ | `testHelpers()` + `testRasterStateCoverage()` |
| Winding / depth tests | ✅ | `testRasterStateCoverage()` |
| Full upstream corpus sync | ✅ | `sync_corpus.sh` + provenance drift report |

### Unit-First DoD Checklist

| DoD item | Status | Evidence / remaining work |
|---|---|---|
| Shader transforms are testable from bytecode to deterministic source/IR without Wine, Metal, or GPU execution | ✅ | `shader_transform_spec` covers lower-level D3DBC decode/classification fixtures, register slots, semantic mapping, no-flip contracts, modifiers/masks, flow control, unsupported relative-addressing error contract, and broad opcode lowering source contracts |
| Core draw data is built by pure state-to-value helpers | ✅ | `makeDrawDescFromState()` plus `state_draw_transform_spec` cover draw args, constants, shader refs, resource bindings, stream/index bindings, vertex decl/FVF, RT/DS attachments, viewport/scissor, render/sampler/TSS, FFP keys, transforms, and clip planes |
| Backend cache inputs are verified as deterministic value descriptors | ✅ | `backend_key_descriptor_spec` covers uniforms/depth-stencil/sampler descriptors; `backend_pipeline_key_spec` covers blend/MRT color-write key mapping, PSO hash responsiveness, sampler texture/filter flags, FVF layout hashing, and sRGB format mapping |
| Chunk wire records have data-driven validation independent of replay side effects | ✅ | `device_c_record_utils` + `chunk_record_import_spec` validate fixed records, variable Clear/SetConst tails, invalid/truncated records, and draw-run parameter conversion |
| Imported chunk replay has explicit boundary types and unit-testable replay decisions | ✅ | `ImportedChunkView` / `ImportedRecordView`, draw-run scan helpers, replay-category classification, resource-retention derivation, read/write hazard extraction, barrier/synchronous-read boundary decisions, and deterministic hazard-scope reset/continuation are split and tested without Metal |

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

No benchmarks exist yet. All R-BENCH-1.x through R-BENCH-5.x are not started.

| Area | Status | Spec |
|---|---|---|
| `dxmt9-bench` harness | ❌ | R-BENCH-1.1 |
| Draw call throughput workload | ❌ | R-BENCH-2.2 |
| Bridge operation budget counters | ❌ | R-BENCH-2.3-R-BENCH-2.5, R-BENCH-5.3 |
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
| 1 | Extend the `shader_runner_dxmt9` runtime probe layer beyond texture / VS color / viewport / color-write probes to mip/dependent-read, nonzero-origin/half-pixel, alpha/oDepth/MRT/fog/sRGB cases | R-TEST-1.7–R-TEST-1.10 |
| 2 | Promote the PE conformance scaffold inventory by fixing the newly exposed app-local conformance failures, then running app-local and builtin Wine lanes for all scaffolded cases | R-TEST-12.1, R-TEST-12.20 |
| 3 | Finish factory HRESULT parity and validation coverage, including `CheckDeviceFormatConversion`, multisample quality levels, device-type enum handling, and any selected optional export-profile stubs beyond the current auxiliary set | R-CORE-1.9, R-CORE-1.11-R-CORE-1.15, R-TEST-12.9-R-TEST-12.10, R-TEST-12.15-R-TEST-12.16 |
| 4 | Implement and verify D3D9Ex user-memory texture/offscreen-surface paths and shared-handle error policy | R-CORE-4.11-R-CORE-4.12, R-TEST-12.11 |
| 5 | Port Wine-derived D3D9Ex QI conformance and finish Ex/display/swap-chain validation | R-CORE-1.6, R-CORE-10.2-R-CORE-10.4, R-CORE-10.17, R-CORE-10.18, R-TEST-12.4 |
| 6 | Expand Wine stateblock conformance beyond the current compact scaffold and implement full `D3DSBT_*` masks/resource/reset interactions | R-CORE-3.7, R-CORE-3.8, R-TEST-12.5, R-TEST-12.19 |
| 7 | Expand compact reset/window scaffolds if runtime evidence exposes missing Wine-visible edge cases, then promote device lifetime/refcount, query validation, resource wrapper, and scene scaffolds with runtime evidence | R-CORE-2.6, R-CORE-2.8, R-CORE-4.8-R-CORE-4.10, R-CORE-8.3, R-TEST-12.3, R-TEST-12.12-R-TEST-12.14, R-TEST-12.17-R-TEST-12.18 |
| 8 | Build and verify the upstream-style PE targets: `d3d9.dll` + `winemetal.dll` + `winemetal.so` | d3d9/wsi §6, §9 |
| 9 | Run the Wine WSI smoke on the upstream-style deployment and promote the gap status if it passes | R-TEST-11.3 |
| 10 | D3D8 entry point + IDirect3D8 factory + resource wrappers | R-D3D8-1.1, R-D3D8-2.1 |
