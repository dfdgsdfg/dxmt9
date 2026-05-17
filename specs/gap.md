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
| StateBlock capture / restore | ⚠️ | Full-state copy exists. **T1 (2026-05-08)**: `D3DSBT_VERTEXSTATE` apply now copies the per-stage TSS slice (`D3DTSS_TEXCOORDINDEX`, `D3DTSS_TEXTURETRANSFORMFLAGS`) matching Wine `vertex_states_texture[]`; `kPixelStateRenderStates` and `kVertexStateRenderStates` already match Wine's `pixel_states_render[]` and `vertex_states_render[]`. **C5 (2026-05-10, `a4252db`)**: per-frequency dirty mask now invalidated on `IDirect3DStateBlock9::Apply` via new `CommandQueue::markPendingDirtyAll()` (uniform::DirtyState all 13 bits — VS/PS F/I/B + FFP_VS Transforms/Clip/Viewport + FFP_PS Fog/Alpha/TexFactor/TSSConstant) so the next chunk re-uploads every uniform sub-block. The four `BeginStateBlock` / `EndStateBlock` / `CreateStateBlock` / Apply-during-record invalid-call cases already returned `INVALIDCALL` in master; commit added one missing test for EndStateBlock-without-Begin. Still needed: full Wine `test_state_block_savedstates` matrix coverage (per-state-byte equality across capture/apply cycles). |
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
| **D3DBC → MSL translation**: SM1/SM2/SM3 arithmetic, texture, flow control, constants, matrix, and MOVA | ⚠️ | Core direct D3DBC→MSL paths exist for the broad SM2/SM3 set, and `shader_transform_spec` plus the shader corpus cover many opcodes. The 2026-05-16 3DMark06 audit in `specs/3dmark06.md` now has source/corpus coverage for `TEXKILL`, `BREAK`/`BREAKC`/`BREAKP`, `CALL`/`CALLNZ`/`LABEL`/`RET`, `DEFB`/`DEFI`, `DP2ADD`, `DP3`/`DP4`, `EXPP`/`LOGP`, `FRC`, `IFC`, `LRP`, and a passing `vs_2_0` corpus probe. SM1 pixel bytecode and legacy texture opcodes now have source-contract lowering and runtime evidence for coordinate, bump-env, register-remap, dot-product, matrix, specular, vspec, and depth families. Local Wine marks `TEXM3x3DIFF` with valid shader-model range `0.0..0.0`, so dxmt9 keeps it as a deterministic reserved/invalid opcode instead of a runtime support target. 3DMark06 `texkill-cf1`/`texkill-cf2` and `shader-complete-cf1` app re-validation show the prior `opcode_65` blocker no longer trips. Still needed: broaden model/opcode pair coverage and continue the 3DMark06 black internal-output investigation outside shader opcode support. |
| Per-frequency draw-uniform split (`R-BACK-12.1`–`R-BACK-12.21`) | ✅ | Landed: `VsConsts`/`PsConsts`/`FfpVsConsts`/`FfpPsConsts`/`DrawVolatile` host structs + MSL prelude (A2), `setVertexBytes` bridge (A1), shader translator routing (B1), backend `DirtyMask` + range counters at chunk import (C1), encoder per-stage bind + `setVertexBytes` push (C2), `uniform_*` perf counters (C3), layout + dirty unit specs (D1, D2). Measured (offscreen-heavy@256, 3 reps): `transient_upload_bytes` 87 MB → 1.17 MB (−98.7%), `uniform_build_cpu_ms` 380 → 15 (−96%), pixel-identical output, fps within noise of baseline. **Cleanup closed (2026-05-08/09):** encoder `LocalDirtyState` replaced with `uniform::DirtyState` (E1 `e0ea2e2`); legacy `DrawUniforms` host struct + `buildDrawUniforms` removed (E2 `bf2ca5e`, `86131d6`). |
| Submission grain: 1 chunk → N MTLCommandBuffer chain (`R-BACK-2.29`–`R-BACK-2.34`) | ⚠️ | **Production default is `per-render-pass + cap=4`** per R-BACK-2.34 — `dxmt9_draw_encoder.mm:267` returns `MidChunkCommitPolicy::PerRenderPass` for the no-env case, matching the spec text in `specs/backend/requirements.md`. The earlier "spec drift flagged" note in this row was itself based on a misread of `midChunkCommitPolicy()` and has been retracted; the architecture-comparison.md "Y1 default 1:1-to-4 cap=4" claim is correct. **Implementation history:** S2 (`encodeChunk` mid-chunk split via `splitMidChunk` lambda mirroring `splitBeforeBlockingPresent`, env-driven `DXMT9_MID_CHUNK_COMMIT_POLICY=off\|per-render-pass\|per-n-records`, originally default `off`). U1 (R-BACK-2.33 chain length cap via `splitMidChunkUnderCap` wrapper + `DXMT9_MID_CHUNK_COMMIT_CAP_PER_RENDER_PASS=4` default; cap=0 disables for diagnostic A/B). Y1 (R-BACK-2.34, commit `539b515`) flipped the no-env default from `off` → `per-render-pass`; `policy=off` remains reachable as the legacy 1-CB-per-chunk opt-out. S1 TLA: `CommandQueue.tla` extended with sub-CB chain (39,638 states pass). Counters `sub_command_buffers` + `chunk_subcb_count_max` in table + per-frame snapshot. **CC1 (2026-05-10):** earlier docs misclaimed dxmt9 borrowed DXMT's "submission slot chain"; verified DXMT actually uses strict 1 chunk = 1 CB. R-BACK-2.34 is a dxmt9-original divergence, more aggressive than DXMT. **BB1 SFIV +49% fps (13.25 → 19.77)** measured on synthetic SFIV fixture — this provided the chain-rich evidence U1 was missing and motivated the Y1 default flip. **S3 menu A/B (light scene):** `command_buffers / frame` 1 → 2 with `present_acquire_wait_ms` p99 **−93%**. **U1 heavy-scene A/B (RP≥10, ≈1500 frames):** `command_buffers` plateaus at 4 (cap holds), `gpu_command_buffer_time_ms` p99 **−44%** (290 → 163 ms — pipelining win measured) but fps unchanged within noise (11.89 → 11.24) because 4× tile-flush + commit overhead absorbs the gain on that profile (≈2.1 ms / frame per `docs/research/g-axis-tuning.md`); the cap from R-BACK-2.33 bounds that worst case. **Still missing:** wild-title fps measurement (BB1 SFIV is synthetic only); `per-n-records` split path implemented but not exercised by probes; R-BACK-2.32 formal evidence. |
| Render-pass load/store action policy (`R-BACK-15.1`–`R-BACK-15.16`) | ⚠️ | `specs/backend/render-pass-actions/` (commit `ebd7bad`) adopted as the contract. **Landed (G+H batches, 2026-05-08/09):** `dxmt9_perf_counters` now emits `render_pass_load_action_*` / `_store_action_*` / `tile_preservation_bytes` (G1 `c3bd28b`, G3 `4b124aa`); `tests/native/backend/render_pass_actions_spec.cpp` filled with 8-case matrix (G2 `6a0ade4`, G4 `2da64ca`); `CommandQueue::touchedColorHandles_` set + API (H2 `4d77934`); depth look-ahead end-of-chunk + texture-sample (H1 `ba2ff2f`); color first-use DontCare-load + touched mark/invalidate (H3 `c9ba806`, R-BACK-15.4/5); B+C integration tests (H4 `a7a2aee`, R-BACK-15.4-7). **Still needed:** `R-BACK-15.6` cross-frame correctness for queue-local retention, `R-BACK-15.7`/`15.8` live-out depth-stencil DontCare-store, performance contract validation (≥30% Load reduction, ≥50% depth Store reduction, `completion_present_wait_ms` ≥20% on SFIV). |
| Argument-buffer hybrid Stage 2 (`R-BACK-12.22`–`R-BACK-12.26`) | ⚠️ | Stage 1 ✅. **winemetal bridge surface landed 2026-05-10 (`182fc32`):** `WMTArgumentBuffersTier`, `WMTArgumentDescriptor`, `MTLDevice_argumentBuffersSupport`, `MTLDevice_newArgumentEncoder`, `MTLArgumentEncoder_*` (encodedLength/alignment/setArgumentBuffer/setBuffer/setTexture/setSamplerState), `MTL{Buffer,Texture,SamplerState}_gpuResourceID` for Tier-2 MTLResourceID encoding (11 funcs). **dxmt9-runtime adopter landed 2026-05-10:** capability gate (`Pool::argbufHybridEnabled_` cached once at `CommandQueue` init from `device.argumentBuffersSupport() >= Tier2 && supportsApple3()`); per-pass `selectArgbufHybridForPass` selector (Stage1 fallback when gate fails, no mid-pass switching); `ShaderVariantKey::argbufHybridMode` bit (Stage 1/Stage 2 PSOs hash to distinct cache entries, parallel to `tileFfpMode`); MSL prelude variant `makeShaderPreludeArgbufHybrid()` declaring `struct ArgbufLayout { constant Vs/Ps/FfpVs/FfpPs* + texture2d[8] + sampler[8] }` at `[[id(0..19)]]` for slot 30 binding; new `dxmt9_argbuf_hybrid` module owning `buildArgumentDescriptors()` (20-entry `WMTArgumentDescriptor` table for `MTLDevice::newArgumentEncoder`); counters `argbuf_hybrid_encoder_count` / `stage1_encoder_count` / `argbuf_hybrid_fallback_count` / `argbuf_hybrid_bytes_per_encoder` / `stage1_bytes_per_encoder` all wired in `startRenderPass`; native test `dxmt9-argbuf-hybrid-spec` covers capability gate, selector decision shape, variant-key bit independence from `tileFfpMode`, descriptor layout (4 + 8 + 8 entries at id 0..19), MSL prelude variant text. **MSL routing landed 2026-05-10 (`b9cfffb`):** `ShaderSourceContext::argbufHybridMode` flag propagated via `Cache::getOrBuildDrawPipelineForState` → FFP emitters (`makeFfpVertexSource` / `makeFfpPixelSource` / `makeFfpTilePixelSource`) and DXBC→MSL translator (`dxmt9_shader_metal_ir.cpp`) emit single `[[buffer(30)]] ArgbufLayout const* abuf` parameter + alias preamble (`constant VsConsts& vsConsts = *abuf->vsConsts;`, `texture2d<float> tex0 = abuf->textures[0];`, …) when bit is set; body MSL surface unchanged. Native test `dxmt9-argbuf-hybrid-msl-spec` (12 cases) covers Stage1/Stage2 emit symmetry + variant-key bit independence from `tileFfpMode`. **Per-encoder argbuf populator landed 2026-05-10 (`5aa7081`):** `dxmt9_argbuf_hybrid` extended with `ArgbufEncoderResource` (lazy-init on `CommandQueue` from `device.newArgumentEncoder(buildArgumentDescriptors(), 20)`), `PopulatedArgbuf`, `openArgbuf` (reserves storage via `CommandQueue::reserveTransientBuffer` keyed on chunk `seqId`), `populateConstantBuffers` / `populateResourceBindings` / `updateDirtyArgbufRegions`. `startRenderPass` opens argbuf in Stage 2 path, populates resource MTLResourceIDs, binds at `kArgbufHybridBindSlot=30` for vertex+fragment, bumps `argbuf_hybrid_bytes_per_encoder`. `encodeDraw` per-draw rewrites dirty cbuf entries via `updateDirtyArgbufRegions`. Native test `dxmt9-argbuf-populator-spec` covers ArgbufEncoderResource + `dirtyBytesEstimate` + PopulatedArgbuf default. **Stage 1 shadow drop landed 2026-05-10 (`ceb9583`)** ahead of `R-BACK-12.26` validation: encoder no longer issues slot 0/3 binds when `argbufHybridMode=true`; cbuf dirty bits cleared inside the argbuf populator branch; `bindFfpVsIfDirty` routes through new `pointFfpVsAtSlice` helper (argbuf [[id(1)]]) when in Stage 2 to handle FFP `preTransformed` viewport override. **🔴 KNOWN REGRESSION:** baseline 71/2 → after-shadow-drop 66/7 — five new visible-pixel failures on Apple Silicon (shader-corpus: viewport_vs_triangle, viewport_nonzero_origin, half_pixel_solid_rect, vs_color_triangle, texture_2x2 + resource-hazard `9363652620110249896 vs 2` + core-device-com swap-chain count mismatch). Cause: the Stage 2 GPU read path has silent bugs that the Stage 1 shadow was masking; the shadow drop made them visible. Still needed (urgent): `R-BACK-12.26` shader-runner readback equality on Apple Silicon to drive the failing test list to root cause; stable byte-counter accounting (today `argbuf_hybrid_bytes_per_encoder` no longer double-counts since Stage 1 binds are gated, but the FfpVs override path bumps both `argbuf_hybrid_bytes` and the inline `pointFfpVsAtSlice` repoint). |
| Tile-shader FFP fast path (`R-BACK-13.1`–`R-BACK-13.6`) | ⚠️ | Apple-Silicon-only acceleration of FFP fog / alpha-test / A2C via `MTLTileRenderPipelineState` instead of fragment-stage discard. **winemetal bridge surface landed 2026-05-10 (`d9c54eb`):** `WMTTileRenderPipelineDescriptor`, `MTLDevice_newRenderPipelineStateWithTileDescriptor`, tile-stage encoder ops (`setTileRenderPipelineState`, `dispatchThreadsPerTile`, `setTileBuffer`, `setTileTexture`, `setTileBytes`, `setTileSamplerState` — 7 funcs). `MTLDevice_supportsFamily(WMTGPUFamilyApple3)` already exposed. **dxmt9-runtime adopter landed 2026-05-10 (`84ea225`):** `makeFfpTilePixelSource` MSL emitter (imageblock half4 for 8-bpc attachments, float4 otherwise; FFP arithmetic always `float`); `selectTileFfpForPass(state, supportsApple3)` selector with reason taxonomy (GpuFamily / NotFfp / Precision / UnsupportedState); PSO key extended with `tileFfpMode` bit, separate cache entries, `device.newRenderPipelineState(WMTTileRenderPipelineDescriptor&, Error&)` overload wired; mid-pass demotion split treats `tileResplit` as a flush trigger alongside RT-change/hazard; counters `tile_ffp_pass_count`/`portable_ffp_pass_count`/`tile_ffp_fallback_{precision,unsupported_state,gpu_family,mid_pass_ineligible}`/`tile_ffp_mid_pass_resplit_count` all wired; native tests `dxmt9-tile-ffp-selector-spec` (12 cases, including FFP vertex-blend portable fallback) and `dxmt9-tile-ffp-msl-spec` (7 cases) pass. **Encoder wire landed 2026-05-10 (`523b66e`):** `encodeDraw` now branches on `tileFfpMode` and calls `encoder.setTileRenderPipelineState(pipeline)` + `encoder.dispatchThreadsPerTile` when selector chose tile path; mid-pass demotion path unchanged (`activePassUsesTileFfp` flag still drives `tileResplit` flush trigger). **W2 tile-size query landed 2026-05-10 (`9ba7c8e`):** new winemetal accessors `MTLRenderCommandEncoder_tileWidth` / `_tileHeight` query Metal's chosen tile size for the open encoder, replacing the 16×16 hardcode. Falls back to 16×16 when Metal returns 0 (older OS / unsupported attachment shape). Still needed: shader-runner readback equality between tile and portable variants on Apple Silicon hardware. |
| `MTLHeap` small-resource pooling (`R-BACK-5.9`, `R-BACK-5.10`, `R-BACK-14.*`) | ⚠️ | D3D9 small-texture working set (lightmaps, decals, glyph atlases, particle sprites) currently hits direct `newTextureWithDescriptor` per resource with per-resource residency. **winemetal bridge surface landed 2026-05-10 (`cb7c7a4`):** `WMTHeapDescriptor` + `WMTHeapType` + storage/cache/hazard-tracking enums, `MTLDevice_newHeapWithDescriptor`, `MTLHeap_{makeBuffer,makeTexture,size,usedSize,currentAllocatedSize,setLabel}`, `MTL{Render,Blit,Compute}CommandEncoder_useHeap` (10 funcs). **dxmt9-runtime adopter landed 2026-05-10 (`8b0635f`):** new `dxmt9_heap_manager.{hpp,cpp}` module with three families (`PrivateTexture` / `SharedTextureUm` / `SharedBuffer`), `kHeapEligibilityFootprintBytes = 64 KB`, geometric grow `4 MB` → `256 MB` cap; `Pool::createBuffer/createTexture` route through `HeapManager` before direct allocation; `BufferRecord/TextureRecord` carry `isHeapBacked` + `WMT::Heap heap`; `useHeap` issued at every render/blit encoder open; `retireFreedHeaps` gated on `completedSeqId`; counters `heap_alloc_count`/`heap_bytes_allocated`/`heap_instance_count`/`heap_direct_fallback_count`/`heap_fragmentation_failure_count`/`heap_compaction_count`/`heap_alloc_failure_count`/`use_heap_calls`/`use_resource_calls` all wired. Native test `dxmt9-heap-pooling-spec` covers eligibility classification + lifetime. **Render-encoder useHeap dedup landed 2026-05-10 (`523b66e`):** `beginRenderPass` now walks bound resources (`hot.indexBuffer`, `hot.streamBuffers[]`, `hot.textures[]`) and calls `encoder.useHeap` only for heap-backed bindings via a small `std::array` linear-scan dedup — Metal driver no longer sees `useHeap` for unrelated heap instances. **W1 blit/initializer dedup landed 2026-05-10 (`473218c`):** same pattern applied to all 5 blit-encoder open sites in `dxmt9_blit_encoders.cpp` (`encodeReadback` / `encodeStretchRect` direct-blit / `encodeSurfaceCopy` direct-blit / `readbackSurface` x2 / `Pool::uploadTextureLevel` synchronous blit) and the deferred-upload flush in `Initializer::flushToWaitUnlocked`. `StagingCopy` extended with `destIsHeapBacked` / `destHeap` so the flush dedupes without a reverse handle lookup. `HeapManager::forEachHeapInstance` removed (no callers remain). Compute encoders are not constructed in dxmt9 today — no-op for that path. Still needed: workload regression evidence on FFP-heavy scenes that small-texture allocations land in heap families. |
| Pool×Usage → storage-mode unified-memory mapping (`R-BACK-5.7`, `R-BACK-5.8`) | ⚠️ | **Implementation landed 2026-05-10 (`6b79657`):** `toResourceOptions(pool, usage, hasUnifiedMemory)` matrix in `dxmt9_format_convert`; `Pool::hasUnifiedMemory_` cached once at queue init via `setHasUnifiedMemory`; resource records carry `needsStagingBlit` / `isManagedDiscrete`; `stageTextureUpload` short-circuits to direct `replaceRegion` on Apple Silicon MANAGED; `perf::countManagedTextureUploadBlit(bytes)` advances only on the discrete branch (Apple Silicon must keep counter at 0). Buffer create's discrete-MANAGED branch annotated as TODO follow-up. **C1 rename ring landed 2026-05-10 (`4a9a8f0`):** `BufferRecord` carries `isDynamicRename` + `renameActiveIndex` + `std::vector<BufferRenameRingEntry> renameRing`; `Pool::finalizeBufferMap` extended to take `WMT::Device + completedSeqId` so the rotation runs inline. New file-local `rotateDynamicRename` stamps the rotated-out entry's `lastUsedSeqId`, fast-paths idle-active, walks the ring for any other idle entry, and on no idle entry calls `device.newBuffer(info)` with `MTLStorageModeShared` and appends — never blocks GPU completion. Grow-only ring (capacity persists for the buffer's lifetime). Native test `dxmt9-dynamic-rename-ring-spec` covers create-time tagging, ring seeding, in-place idle reuse, growth on full in-flight, idle rotation, no-shrink invariant, non-DISCARD bypass. Still needed: workload regression evidence that MANAGED upload bandwidth is bypassed on M1+. |
| Pipeline cache prewarming from `MTLBinaryArchive` (`R-BACK-3.7`, `R-BACK-3.8`, `R-BACK-4.8`) | ⚠️ | **Implementation landed 2026-05-10 (`fdadb82`):** new `dxmt9_archive_prewarm` module with `resolveMode()` (env `DXMT9_PREWARM=full|lazy|disabled`), `resolveArchivePath(device, mode)`, `run(device, path, mode)` executing the §6.1 failure-mode table with POSIX `flock(LOCK_SH\|LOCK_NB)` 100 ms bounded retry, magic-size + `.abi` stamp probes, rename-to-`.corrupt`/`.outdated` on mismatch, perf counters `prewarm_entries_loaded` / `prewarm_load_cpu_ns` / `prewarm_failure_{corrupt,schema,lock_busy,missing}` / `cold_compile_count_after_warm` / `archive_bytes`. `dxmt9_device` emits diagnostic log line. **C2/C3 (2026-05-10, `8fb69a5`):** build-default mode now branches on `NDEBUG` (release builds = `Full`, debug = `Lazy`); env `DXMT9_PREWARM` continues to override. NSError-domain routing landed: new `WMT::Error::code()` / `domain()` accessors + `MTLBinaryArchive` ctor probe inside the shared-flock scope inspects `NSError.code` and maps `MTLBinaryArchiveErrorInvalidFile (1)` → corrupt and `UnexpectedElement (2)` → schema; pre-load magic-size + `.abi` stamp probes retained as fallback for older Metal that nil-out without setting a domain code. Still needed: workload evidence that `coldCompileCountAfterWarm` drops after warm prewarm. |

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

### R-TEST-0.10 Boundary Value Coverage

⚠️ Partial. Existing native tests cover many helper-level value transforms, but
the exact value carried across several production boundaries is not fully
asserted. The rows below track the implementation goal for closing R-TEST-0.10:
each boundary needs tests that compare the semantic value immediately before and
after the boundary, not only layout, helper conversion, or final rendered output.

| Boundary lane | Status | Current evidence | Missing evidence / implementation goal |
|---|---|---|---|
| B1: PE setters -> D9C packet / variable records | ⚠️ | `dxmt9-pe-chunk-record-value-spec` asserts exact rich `D9CDrawPrimitivePacket` values, retained handle ranges, record ordering, and all six SET_CONST tail payloads; layout/validation suites still guard POD shape. | Real PE setter capture is still blocked because the Windows-only `D3D9DeviceImpl` packet/const appenders are private. A narrow native-safe producer capture seam is still needed to prove actual PE setters generate those exact records. |
| B2: imported records -> core `DeviceState` / replay inputs | ✅ | `dxmt9-imported-apply-state-value-spec` asserts rich APPLY_STATE and SET_CONST payload propagation into core state, submitted draw-run state, `DrawUniformPayload`, bulk retention, indexed draw policy, and malformed-record no-mutation behaviour; `state_draw_transform_spec` now also asserts shader constant values and hashes through production canonical draw uniforms. `resource_hazard_spec` and `dod_replay_observer_spec` cover ordering. | Queue dirty-range bitsets remain private, so this lane verifies constant values at `DeviceState` and draw uniform boundaries rather than directly asserting the internal dirty accumulator. |
| B3: public resource creation -> backend/Metal descriptors | ⚠️ | `dxmt9-resource-format-boundary-spec` routes public D9C texture/cube/volume/render-target/depth-stencil/offscreen creation through core resources and asserts D3DFORMAT identity, component/alpha policy, sRGB-compatible conversion, depth fallback policy, usage/pool/dimensions/levels/MSAA, NPOT mip-tree width/height/pitch/storage layout, invalid mip-level rejection, and BC row rounding. `dxmt9-core-format-caps-spec` now pins signed normal formats (`V8U8`, `Q8W8V8U8`, `V16U16`), `ATI2`/`BC5` FOURCC and block layout, `CxV8U8` unsupported classification, and unknown/null-like format rejection. | Final `resources::Pool` -> `WMTTextureInfo` descriptor values are still not directly observable from `RecordingBackend`; a resource-pool/Metal descriptor observer is needed for complete post-Pool evidence. |
| B4: generated bridge/unixcall marshalling -> C boundary args | ⚠️ | `dxmt9-bridge-marshalling-value-spec` asserts generated `Args_*` native/wow64 POD layout, commit-chunk pointer/status preservation, wire blob record/handle payload values, sampler/TSS/const/factory argument values, and wow64 pointer/handle decode helpers. | The PE client wrapper and unix dispatch TU are not directly linkable into this native test; a generated-call fake sink would be needed to observe the full wrapper-to-dispatch path. |
| B5: draw state -> shader arg-buffer texture/sampler bindings | ⚠️ | `dxmt9-shader-argbuf-binding-value-spec` asserts `FlatDrawStateRecord` texture/sampler slot values through shader context, Stage 1 `texN`/`sampN` MSL bindings, Stage 2 arg-buffer aliases, descriptor ids, null texture slots, and sampler/LOD defaults. `dxmt9-encode-draw-recorder-spec` covers representative Stage 1 texture/sampler encoder calls. `dxmt9-argbuf-populator-spec` now records Stage 2 `MTLArgumentEncoder::setTexture/setSamplerState` writes with concrete argbuf ids, texture handles, shader-read texture view selection, sampler handles, null-stage skip behavior, and texture/sampler ordering. | Remaining evidence gap is live GPU-visible equality for Stage 2 argument-buffer sampling on Apple Silicon, plus Stage 2 constant-buffer dirty rewrite interactions beyond the deterministic write seam. |
| B6: Wine PE conformance HRESULT / out-pointer / status values | ❌ | `tests/conformance/d3d9/MANIFEST.toml` currently reports 27 scaffolded, 6 failing, 2 partial, and 0 fully passing cases; manifest validation passes. | Fix or record evidence for the current failing public ABI lanes: factory validation return codes, present-parameter validation, Ex create/reset mode validation, private-data resource wrappers, Ex shared-handle policy, and creation-failure out pointers; promote cases only when every declared lane/architecture has passing evidence. |
| B7: core draw API -> `DrawParam` / topology payload | ✅ | `dxmt9-core-device-coverage-spec` now asserts non-UP `TriangleFan` conversion for `drawPrimitive`, `drawIndexedPrimitive`, and coalesced `drawPrimitiveRun`: canonical `TriangleList`, generated/re-written user index payload bytes, index type, base vertex, start index, stripped source index-buffer policy, and invalid indexed-fan rejection before backend recording when no index buffer is bound. `state_draw_transform_spec` now pins stream/index/vertex-declaration canonical values, D3DTA/TSS argument propagation, sparse render-target/depth-stencil attachment values, and shader-constant hashes at the canonical draw-state boundary. `core_device_lifecycle_spec` already covers UP fan vertex/index payload decomposition. | Keep this lane green by requiring concrete topology payload assertions for any future primitive conversion; enum normalization alone is not sufficient evidence. |
| B8: `DrawParam` / `FlatDrawStateView` -> Metal encoder draw command | ⚠️ | `dxmt9-metal-encoder-recorder-spec` overrides `MTLRenderCommandEncoder_encodeCommands` and records final WMT command payloads for `drawIndexedPrimitives`, including the 3DMark05 fan-fix shape: triangle primitive, `u16`, index count 6, generated index-buffer offset 0, instance count 1, base vertex 0, base instance 0, plus the stream-0 Metal vertex-buffer bind offset 3712 at slot 1. `dxmt9-encode-draw-recorder-spec` now drives the full `encodeDraw` path with Metal calls suppressed after recording: it asserts command order for base-state binds, stream binds, `DrawVolatile`, and final draw; concrete `DrawVolatile` bytes; non-indexed `drawPrimitives` start-vertex offset absorption; direct bound-index selection; bound-vertex/user-index selection; UP-vertex/user-index selection; multi-stream programmable VS binds; stream offsets/strides; base vertex; start-index offset; index count/type; final draw arguments; and representative `skipBaseStateBind=false` pipeline, depth-stencil, viewport, scissor, rasterizer, texture0/1, and sampler0/1 encoder writes. Focused 3DMark05 diagnostics also show the fixed bad draw as `api=DrawIndexedPrimitiveUP`, `indexType=u16`, `userIndexBytes=12`, and `indexed=1`. | Remaining evidence gap is runtime GPU-visible behavior for state combinations where correct encoder values do not by themselves prove Metal rasterization/sampling/depth results, plus broader variant coverage as new wild-app failures identify concrete combinations. Logs and screenshots remain useful evidence but not deterministic unit coverage. |

### R-TEST-0.11 Draw / Render Intent Coverage

⚠️ Partial. The 3DMark05 TriangleFan issue showed that enum normalization can
pass while draw intent is still wrong. The render-part audit now treats each
draw/render transform as incomplete unless tests prove the backend-visible
operation preserves the D3D9 intent: equivalent topology, source data selection,
raster footprint, render target effects, resource sampling, and ordering.

| Intent class | Status | Current evidence | Missing evidence / implementation goal |
|---|---|---|---|
| Geometry topology intent | ⚠️ | Core TriangleFan payload coverage is now green for non-UP, indexed, draw-run, and UP paths; encoder recorder covers final indexed and non-indexed Metal command shapes plus `DrawVolatile` ordering. | Add runtime raster/readback interactions for cases where the encoded primitive shape is correct but GPU-visible coverage, culling, or depth behavior could still diverge. |
| Draw source intent | ⚠️ | Core tests assert generated/re-written index payloads, base/start fields, and stripped source index-buffer policy for TriangleFan conversion. `dxmt9-encode-draw-recorder-spec` covers bound stream offset/stride, UP vertex/index slices, direct bound-index selection, multi-stream programmable VS source selection, and representative texture/sampler/depth/raster/pipeline encoder source selection at the encoder boundary. | Pair the deterministic encoder evidence with GPU readback probes for sampling/depth/raster combinations that cannot be proven from command values alone. |
| Raster intent | ⚠️ | State/draw transform tests and raster-plan checks cover viewport/scissor/half-pixel inputs; shader-runner has viewport, half-pixel, cull/front-face keep/reject, scissor left-edge, depth-range, fill-mode wireframe, and clip-plane runtime readback probes. | Add broader clip-plane partial-coverage and combined raster-state probes. |
| Attachment / render-state intent | ⚠️ | Pipeline-key/render-pass tests cover descriptors and load/store policy; shader-runner covers color-write, MRT basics, `oDepth` depth-test intent, alpha-test discard, alpha-blend RGB/alpha, FFP linear and EXP fog, sRGB write, depth compare rejection, cull-none orientation, and combined alpha/depth/color-mask behavior. | Add additional stencil, MRT interaction, multi-draw state-transition, and clear/load/store combined behavior probes. |
| Resource sampling intent | ⚠️ | Resource-format boundary and shader arg-buffer tests cover descriptors, texture/sampler slot values, Stage 1 and Stage 2 encoder write seams, several texture readback probes, sampler clamp overscan readback, sampler sRGB decode, and texture+alpha-blend readback. | Pair descriptor/encoder evidence with additional GPU readback where descriptor inspection cannot prove sampling behavior, especially Stage 2 argument-buffer sampling equality on Apple Silicon. |
| Ordering / synchronization intent | ⚠️ | Chunk import/replay, hazard, replay observer, and TLA+ models cover many ordering and lifetime rules. | Add machine-readable Metal/WSI result schemas and command-level recorder evidence for final ordering across draw, clear, copy, readback, and present. |

### Wine D3D9 Deferred Oracle Routes

| Wine route item | Status | Current evidence | Deferred reason / unblocker |
|---|---|---|---|
| `texbem_test` / `TEXBEM` / `TEXBEML` | ✅ | Native FFP key/source-contract coverage records bump-env texture-stage state and SM1 opcode identity/lowering seams. `shader_runner_dxmt9` now has passing `ps_1_3` `TEXBEM` and `TEXBEML` runtime readbacks with bump-env matrix/luminance TSS values exposed through the runner DSL. | Broaden only if a Wine row or wild app exposes an uncovered bump-env matrix/luminance edge. |
| `texdepth_test` / `TEXDEPTH` | ✅ | Shader transform source-contract coverage names and lowers the legacy opcode path. `legacy_sm1/dxmt9_ps14_texdepth_depth_write.shader_test` proves `TEXDEPTH` depth-test behavior, and `legacy_sm1/dxmt9_ps13_texm3x2depth_source.shader_test` pins the related `TEXM3x2DEPTH` source/depth-output contract. | Broaden only if Wine/app evidence exposes a new depth-output edge. |
| `intz_test`, `test_fetch4`, `resz_test` | ❌ | Format conversion and resource-boundary tests cover standard D3D9 formats plus selected pseudo-format classification, but these vendor FOURCC paths are not implemented as supported resource semantics. | Deferred because `INTZ`, `FETCH4`, and `RESZ` need an explicit compatibility policy first: accepted/rejected public HRESULTs, Metal storage/fallback strategy, shader sampling/depth resolve behavior, and runtime oracle coverage. |
| `test_null_format` / `D3DFMT_NULL` | ❌ | Native format caps now prove unknown/null-like formats are rejected and `CxV8U8` is an explicit unsupported vendor value, but dxmt9 has no first-class `D3DFMT_NULL` pseudo-format contract. | Deferred until a NULL-format policy is specified: whether public creation/check APIs accept the pseudo-format, which resource usages are legal, how the backend represents a no-storage render target, and which runtime readback oracle proves the behavior. |
| `test_mipmap_upload` | ⚠️ | Native resource tests now assert explicit NPOT mip-tree layout, per-level pitch/storage, invalid level rejection, and upload pitch for locked texture levels; core copy/update paths exist. | Wine upload parity remains deferred until PE conformance or runtime probes cover dirty-region propagation, managed/default pool interactions, cube/volume variants, and generated-vs-manual mip upload ordering. |
| `ps_1_1` / `ps_1_2` / `ps_1_3` / `ps_1_4` runtime shader lanes | ✅ | Native shader source-contract coverage decodes and lowers representative SM1 pixel opcodes, including legacy texture/dependent-texture families. Runtime readback now has passing `TEXCOORD` / `TEX` baselines for `ps_1_1`, `ps_1_2`, `ps_1_3`, and `ps_1_4`, plus dependent texture, matrix, specular/vspec, and depth rows for Wine-valid legacy opcodes. | Broaden only when a Wine row or wild app exposes a specific uncovered SM1 edge. |
| `vs_1_1` runtime shader lane | ✅ | Native shader source-contract coverage decodes and lowers representative `vs_1_1` fixed-output style bytecode. `legacy_sm1/dxmt9_vs11_const_color_readback.shader_test` proves the runtime declaration/constant/color handoff path. | Broaden only when a Wine row or wild app exposes a specific `vs_1_1` input/declaration edge. |
| SM1 dependent texture and depth op families | ✅ | Source-contract coverage records `TEXBEM*`, `TEXCOORD`, `TEXDEPTH`, `TEXDP3*`, `TEXM3x*`, and `TEXREG2*` opcode identity/lowering. Runtime readback now covers baseline SM1 texture-coordinate paths across `ps_1_1` through `ps_1_4`, `TEXBEM`, `TEXBEML`, `TEXREG2AR`, `TEXREG2GB`, `TEXREG2RGB`, `TEXDP3`, `TEXDP3TEX`, `TEXM3x2PAD`/`TEXM3x2TEX`, `TEXM3x2DEPTH`, `TEXM3x3PAD`/`TEXM3x3TEX`, `TEXM3x3`, `TEXM3x3SPEC`, `TEXM3x3VSPEC`, and `TEXDEPTH`. | `TEXM3x3DIFF` is intentionally not a passing runtime entry because local Wine marks it reserved/invalid (`0.0..0.0` valid model range). |
| `VFACE`, `POSITIONT`, and pretransformed-varying shader fixtures | ✅ | Source-contract and EXP probes cover fragment `POSITION`/`VPOS`, `XYZRHW` draw intent, undeclared/default varying intent, `vFace` token decode, Metal `[[front_facing]]` lowering, runtime front/back winding readback, `POSITIONT` diffuse-varying readback, and `POSITIONT` texture-varying readback. | Broaden only if a Wine row or wild app exposes additional face-orientation or pretransformed interpolation edges. |
| Shader constant and FFP TSS constant runtime matrices | ⚠️ | PE conformance scaffolds cover VS/PS constants and stateblock constant apply. Shader-runner runtime coverage now proves TFACTOR selection, `RESULTARG = TEMP` handoff, and true `D3DTSS_CONSTANT` / `D3DTA_CONSTANT` FFP readback. Native coverage pins the production `D3DTSS_CONSTANT` slot, distinct internal `TSS_TEXTURE_TYPE`, `FfpPsConsts::stageConstants[]` propagation, and mid-encoder dirty marking. | Broader runtime evidence still needs shader_runner or EXP hooks for float/int/bool shader constant edge cases and multi-stage constant interaction variants. |
| Current shader failing evidence | ⚠️ | The shader-runner manifest currently has no failing entries; `shader_corpus_tool.py gaps` reports 159 / 159 passing and all runtime shader models covered. Recently promoted evidence includes SM1 depth/specular/vspec/dependent texture op families, `VFACE` front/back readback, `POSITIONT` diffuse and texture-varying readbacks, TFACTOR, `RESULTARG = TEMP`, `D3DTA_CONSTANT`, non-projected and projected texture-transform count3/count4, camera-space position/normal generated texcoords, level-2 `TEXLDL`, generated mip `TEXLDL`, 2D `SetLOD`/maxmip, float/G16R16/V16U16-reduced sampling, 2D `UpdateTexture`, NPOT wrap, FFP linear/EXP/custom-color fog, shader-output table fog, depth compare rejection, zwrite overlap, cull-none orientation, alpha/depth/color-mask interaction, FFP vertex-blend outside probes, multistream texcoord1 offset, and programmable skinning edge probes. SH-D EXP diagnostics still record vertex texture fetch, uninitialized-varying, vertex/range fog, reflection/spheremap non-identity, cube/volume resource, and broader lighting/material gaps. | Keep failing diagnostic probes out of the passing manifest until direct runner/experiment evidence is green. |

### R-TEST-0.12 / R-TEST-1.11 Vertex And Skinning Intent Coverage

⚠️ Partial. Static shader-transform evidence covers several important pieces of
the D3D9 skinning shape: `BLENDWEIGHT` / `BLENDINDICES` declaration mapping,
multi-stream vertex input layout, stream-specific MSL loads, and indexed
constant access through `a0`. R-TEST-1.11 now also has a focused
`shader_runner_dxmt9` runtime fixture for programmable skinning intent. Remaining
gaps are broader weight/index conversion variants and fixed-function vertex
blending.

| Intent class | Status | Current evidence | Missing evidence / implementation goal |
|---|---|---|---|
| Vertex semantic mapping | ✅ | `state_draw_transform_spec` snapshots vertex declaration streams and offsets; `shader_transform_spec` covers multi-stream input layouts and DCL-bound semantic loads; `vs_specific/dxmt9_vs_skinned_triangle.shader_test` renders a multi-stream declaration with POSITION on stream 0 and `BLENDWEIGHT` / `BLENDINDICES` on stream 1; `vs_specific/dxmt9_vs_multistream_texcoord1_offset_readback.shader_test` covers a stream-1 texcoord1 offset runtime path. | Add more variants for moved semantics and additional declaration types. |
| Skinning constants / matrix palette | ✅ | `shader_transform_spec` includes indexed constant-read lowering for the canonical hardware-skinning shape; `dxmt9_vs_skinned_triangle.shader_test` uses VS constants and `MOVA` / indexed matrix access to move the rendered pose. | Add additional matrix slots and mixed-weight cases. |
| Blend weights / indices | ⚠️ | Static MSL source-contract tests verify `BLENDWEIGHT` and `BLENDINDICES` loads from the declared stream and offset; the runtime skinning fixture covers the basic stream/index path. | Add readback coverage that fails on wrong component order, normalization/raw conversion edge cases, or multiple bone-index interpretation. |
| Programmable skinned draw result | ✅ | `dxmt9-shader-corpus-vs_specific-dxmt9_vs_skinned_triangle` passes and proves a deterministic framebuffer mask from multi-stream declaration, exact VS constants, indexed matrix access, and matrix deformation. `vs_specific/dxmt9_vs_skinned_triangle_edge_regression.shader_test` adds edge probes that fail on overdraw or pose drift. | Broaden beyond the focused fixtures with additional matrix slots and mixed-weight cases. |
| Fixed-function vertex blending | ⚠️ | FFP vertex-blend runtime probes now pass for non-indexed one-weight declaration, 2/3-weight declarations, indexed declaration matrix selection, FVF `XYZB2` beta-weight decoding, and outside-probe variants for 3-weight and indexed paths. The FFP VS reads `BLENDWEIGHT` / `BLENDINDICES`, uses the selected world matrices, and produces the expected rendered pose. | Add last-beta indexed FVF variants, normal/lighting interactions, and broader matrix/weight edge cases. |

### R-TEST-13 Module-Boundary Harness

⚠️ Partial. `tests/module_boundary/` now exists as a first-class harness owner
with `MANIFEST.toml`, `run_module_boundary.py`, fixed failure categories,
deterministic result JSON, Meson-wired manifest/runner validation, a
project-authored PE probe, x64/x86 cross-build targets, app-local/builtin staging
logic, builtin-vs-app-local artifact role separation, Wine executable discovery,
timeout classification with PE-probe phase markers, and live provider-side
execution through `dxmt9-unix-chunk-injection-probe`. Still missing: clean
app-local unix provider load, Wine wrapper exit after completed PE probe,
bridge ABI/loader live pass evidence through Wine, and persistent lane/arch
evidence.

| Area | Status | Spec |
|---|---|---|
| WSI integration test (`tests/integration/wsi_present/`) | ✅ | Heroic Wine 11.5 builtin path passes the full 180-frame `wsi_present_x64.exe` smoke |
| Stateless shader transform unit suites | ⚠️ | `shader_transform_spec` covers D3DBC decode/classification fixtures, stage/version decode, comment skipping, fixed operand-count decode, texture-use classification, predicated/control token decode, register kind/index semantics, swizzle/source/dest/write-mask/relative-addressing tokens, sampler register slots, ps_3_0 texcoord semantics, vs_3_0 output semantics, default no-flip contracts, write masks/swizzles/all known source modifiers, source indexed constants, constant-destination indexed writes, IF/ELSE, LOOP/REP/CALL/CALLNZ/LABEL/RET/BREAKC lowering, `TEXKILL`, SM1 `TEXCOORD`/`TEX`/`TEXDEPTH`, bump-env, register-remap, dot-product, matrix, specular, vspec, and reserved `TEXM3x3DIFF`, plus a broad opcode source-contract matrix including MAD/DP/CMP/SLT/SGE/POW/SINCOS/LOG/EXP/matrix/TEXLDD/TEXLDL. `opcode_audit_spec` pins all known `D3DSIO` opcode names/classifications. Remaining evidence gap is runtime corpus breadth across model/opcode pairs rather than the earlier silent-no-op surface. |
| Stateless state-to-draw-data unit suites | ✅ | `state_draw_transform_spec` covers `makeCanonicalDrawStateFromState()` and fixture/offline `makeDrawDescFromState()` for draw args, viewport/scissor, render/sampler/TSS copy, FFP keys, transforms, clip planes, constants, bytecode shader refs, texture/resource handles, stream/index bindings, vertex decl/FVF, and RT/DS attachment variants |
| Stateless key/descriptor unit suites | ✅ | `backend_key_descriptor_spec` covers `buildDrawUniforms()`, depth/stencil keys, and pure `SamplerSnapshot` → `WMTSamplerInfo` descriptor mapping. `backend_pipeline_key_spec` covers blend enable, RGB/alpha op/factor fallback, MRT color-write defaults/overrides, force-visible override, sampler texture/filter flags, FVF vertex-layout hashing, PSO hash responsiveness, and sRGB-compatible pixel format conversion |
| `shader_runner_dxmt9` backend | ✅ | R-TEST-1.1 |
| `shader_runner_dxmt9` extended probe layer | ⚠️ | dxmt9-local runtime probes now cover texture setup/readback, SM1 `ps_1_1` through `ps_1_4` texture-coordinate baselines, Wine-valid SM1 dependent/depth/specular/vspec opcodes, `vs_1_1` constant color, dependent texture read, VS geometry, `VFACE` front/back readback, `POSITIONT` diffuse and texture-varying readback, multi-stream VS texturing including texcoord1 offset, programmable skinning pose plus edge regression, viewport-bounded and nonzero-origin rasterization, half-pixel edge masks, color-write/MRT render-state interactions, `oDepth` depth testing (`render_state/dxmt9_odepth_depth_test.shader_test`), depth compare rejection, zwrite overlap, cull-none orientation, alpha-test, alpha/depth/color-mask combination, alpha-blend alpha-channel, fill-mode wireframe, clip-plane runtime, FFP linear/EXP/custom-color fog, shader-output table fog, sRGB write, sampler sRGB decode, TFACTOR, `RESULTARG = TEMP`, `D3DTA_CONSTANT`, non-projected and projected texture-transform count3/count4, generated camera-space position/normal texcoords, level-2 `TEXLDL`, generated mip `TEXLDL`, 2D `SetLOD`/maxmip, float/G16R16/V16U16-reduced sampling, 2D `UpdateTexture`, NPOT wrap, and FFP vertex-blending intent across one-weight, 2/3-weight, indexed, FVF `XYZB2`, and outside-probe variants. | FFP last-beta indexed FVF variants, normal/lighting vertex-blend interactions, vertex/range fog, reflection/spheremap non-identity generated texcoords, cube/volume resource probes, and multi-draw state-transition probes remain open. |
| Module-boundary harness (`tests/module_boundary/`) | ⚠️ | `tests/module_boundary/MANIFEST.toml` enumerates artifact staging, provider-side probe, app-local loader smoke, builtin loader smoke, result schema, and boundary-separation cases. `scripts/check/check_module_boundary_manifest.py` validates R-TEST-13 coverage; `run_module_boundary.py` validates manifests/results, stages x64/x86 PE artifacts plus MinGW/Wine runtime dependencies, runs dry-run app-local results, auto-discovers configured Wine, separates app-local artifacts from builtin Wine-tree artifacts, records PE probe phase events in timeout JSON, classifies app-local unix-provider load failure from bridge diagnostics, executes the provider-side `dxmt9-unix-chunk-injection-probe` with perf counters, and emits the common `dxmt9.debug.result.v1` path via `--debug-output` with staged artifact manifests plus provider-locator and ABI-handshake sidecars. `dxmt9-module-boundary-probe_x64/_x86` cross-build successfully; short Heroic Wine x64 runs now identify app-local failure after `load_d3d9`/exports at unix-provider load and builtin completion through factory before Wine wrapper timeout. | Resolve app-local unix provider load under Heroic Wine, make the Wine wrapper exit cleanly after completed PE probe, and add passing bridge ABI live checks through Wine. |
| Expanded `.shader_test` corpus (arithmetic, comparison, flow control, transcendental, matrix, source modifiers, texture, FFP sanity/alpha test) | ✅ | Corpus manifest tracks 159 passing probes and no failing entries. All runtime shader models are represented. FFP vertex blending has passing one-weight, 2/3-weight, indexed declaration, FVF `XYZB2`, and outside-probe runtime coverage. Meson shards `status = "passing"` entries for the normal shader-corpus suite. |
| Provenance blocks on corpus files | ✅ | R-TEST-9.1 |
| `MANIFEST.toml` + `check_manifest.sh` + `check_drift.sh` + `sync_corpus.sh` | ✅ | R-TEST-10.1–10.2, R-TEST-7.3; corpus manifest now records `models`, `opcodes`, license provenance, and `shader_corpus_tool.py gaps` reports model/opcode coverage gaps |
| Native `core_spec` coverage for resource mapping / present-readback / clip planes / MSAA / Ex wrappers / programmable texture orientation | ✅ | R-TEST-4.3-R-TEST-4.4, R-TEST-5.1–5.2, R-TEST-6.1 |
| Fixed-function `.shader_test` files | ⚠️ | `ffp/alpha_test.shader_test`, native fixed-function coverage, FFP linear/EXP fog, TFACTOR, `RESULTARG = TEMP`, `D3DTA_CONSTANT`, projected texture-transform count3/count4, and the FFP vertex-blend runtime probe set pass for one-weight, 2/3-weight, indexed, FVF `XYZB2`, and outside-probe paths. |
| Wine `visual.c` oracle coverage (ps_1_x, FFP) | ✅ | native clean-room oracle coverage for lighting, fog, texture transform, texop, FFP varying, sanity, alpha, BEM, ps_1_4, and vs_1_1 source contracts |
| Wine `device.c` / `d3d9ex.c` / `stateblock.c` conformance subset | ⚠️ | `tests/conformance/d3d9/MANIFEST.toml` lists 35 Wine-oracle PE conformance cases with DoD/acceptance criteria and `dxmt9-d3d9-conformance-manifest-check` validates lane/arch evidence. Current manifest status is 27 scaffolded, 6 failing, and 2 partial. **Source organization (T8/T9, 2026-05-08/09):** `d3d9_conformance.c` (originally 1,777 LOC) split into a thin driver (73 LOC) + per-domain `d3d9_conformance_{device,resource,swapchain,query_stateblock}.c` linked to a single `dxmt9-d3d9-conformance.exe` (T8 `b2c4c75`); standalone per-test executables normalized to single `d3d9_*` prefix (drop `_x64` suffix from 6 files, add `d3d9_` prefix to 3 bare files; T9 `739a080`, target names preserved for external invocation compatibility). Focused x64 app-local export/auxiliary runtime evidence passes. The first device-backed app-local run now reaches the provider with 328 checks, 26 failures, and 0 skips; failing groups are factory validation, present-parameter validation, Ex create/reset, private-data resource wrappers, Ex shared-handle policy, and creation-failure out pointers |
| Half-pixel offset exact-coverage test | ✅ | `testHelpers()` + `testRasterStateCoverage()` |
| Winding / depth tests | ✅ | `testRasterStateCoverage()` |
| Full upstream corpus sync | ✅ | `sync_corpus.sh` + provenance drift report |

### R-TEST-14 Debugging Tooling Standard

⚠️ Partial. Metal diagnostics are mostly implemented and documented, but WSI /
window, Wine unix/provider, headless-host, and environment-registry diagnostics
are not yet standardized as a single machine-readable evidence contract. The
debugging surface must be treated as harness evidence when it diagnoses a module
boundary, and as experiment evidence only when it proves app-level behaviour.

| Area | Status | Current evidence | Missing evidence / implementation goal |
|---|---|---|---|
| Environment registry | ✅ | `agents/rules/environment_variables.rules.md` is the registry for runtime and harness `DXMT*` / `DXMT9*` knobs, including bridge verbosity, provider selection, PE recorder flags, prewarm/cache flags, and shader-corpus provenance variables. | Keep new consumers covered by the diagnostic cost audit. |
| Metal diagnostics | ⚠️ | Metal debug runbook covers `.gputrace`, validation-layer use, labels, debug groups, signposts, command-buffer GPU timing, and fault counters. | Link these artifacts into a standard result schema and keep per-stage GPU timing marked as external `xctrace` evidence until runtime `MTLCounterSampleBuffer` support exists. |
| WSI/window diagnostics | ⚠️ | `wsi_present_x64.exe`, queue/present traces, macOS capture helpers, capture-source classification, `check_debug_result_schema.py` coverage for layer acquisition, HWND/window-title identity, capture source, and full-screen fallback semantics, and the non-catalogue `run_wsi_present.py` debug-result runner exist. The runner now records PE stdout HWND/title identity, optional live `screencapture` window/fullscreen artifacts, presented-frame count, and caller-supplied layer-acquisition classification. | Run opt-in Wine WSI evidence with live macdrv layer-acquisition classification on patched Wine runtimes. |
| Wine unix/provider diagnostics | ⚠️ | Provider locator logs and ABI handshake logs exist; Wine manifest files already carry patch-related intent in places. | Implement read-only `scripts/wine/check_patch.py`; extend manifest resolution and experiment runs to pre-gate `requires_patch` / `patch_status` and record those fields in results. |
| Headless / non-Darwin diagnostics | ⚠️ | `wsi_platform_headless` provides a headless utility path; `check_debug_result_schema.py` rejects headless results that claim WSI layer acquisition or window-capture evidence. | Wire headless harness results to emit the checked schema. |
| Machine-readable debug results | ⚠️ | `check_debug_result_schema.py` defines and self-tests the `dxmt9.debug.result.v1` JSON contract for command, environment, artifacts, diagnostics, limits, and fixed failure categories. `run_experiment.py` now writes schema-compatible `debug_result.json` for single-frame, frame-list, interval-range, video metadata, and skipped-frame sidecar paths when capture artifacts exist. Non-catalogue WSI and module-boundary runners now emit schema-compatible debug results directly with live capture and provider/ABI sidecars. | Emit schema-compliant JSON from broader dump producers and add dedicated sidecar roles if the generic log role becomes insufficient. |
| Boundary data dumps | ⚠️ | Native tests assert many exact before/after boundary values, and logs/counters expose pieces of the runtime path; `check_debug_result_schema.py` validates before/after dump entries, schema names, sidecar metadata, and correlation keys. `debug_artifact_bundle.py` now writes manifest-backed dump bundles with sidecar hashes/budgets and has a Meson selftest. | Add opt-in producers for real schema-versioned dump payloads across PE, bridge, unix import, backend, Metal, and WSI. |
| Rendered frame/video capture | ⚠️ | Experiments already emit single-frame internal dumps or window captures for some runs; `check_debug_result_schema.py` validates frame-list / interval metadata, per-frame sources, hashes, sizes, limits, dropped frames, and bounded video segment metadata. `debug_artifact_bundle.py` writes frame sequence manifests and video segment metadata. `run_experiment.py` now accepts `--capture-frames`, `--capture-range`, `--capture-video`, bounds options, emits `DXMT_CAPTURE_FRAMES` / `DXMT_CAPTURE_RANGE`, schedules window captures, records dropped frames, imports renderer `*.skipped.json` sidecars, and wraps captures/video metadata in `dxmt9.debug.result.v1`. The renderer now parses `DXMT_CAPTURE_FRAMES` / `DXMT_CAPTURE_RANGE`, writes `DXMT_EXPERIMENT_CAPTURE_DIR/frameNNNNNN.bmp` internal backbuffer dumps on matching presents, and emits `dxmt9.render_capture.skip.v1` sidecars with reason/counters when internal capture was requested but unavailable. Non-catalogue WSI/module-boundary runners now share the debug-result path. | Drive opt-in live app evidence through the new skipped-frame sidecar path. |
| Diagnostic cost gating | ⚠️ | `tests/HARNESS_MANIFEST.toml` inventories the major harness layers and assigns cost class / release-default policy; `scripts/check/check_harness_manifest.py` is Meson-wired as `dxmt9-harness-manifest-check`; `scripts/check/audit_diagnostic_costs.py` is Meson-wired as a failing gate for harness cost invariants and source/script DXMT env registry drift while the self-test covers missing-env and release-default mismatch cases. | Add focused disabled-hook evidence for bridge counts, chunk counts, allocation growth, and artifact writes. |

### Unit-First DoD Checklist

| DoD item | Status | Evidence / remaining work |
|---|---|---|
| Shader transforms are testable from bytecode to deterministic source/IR without Wine, Metal, or GPU execution | ✅ | `shader_transform_spec` covers lower-level D3DBC decode/classification fixtures, register slots, semantic mapping, no-flip contracts, modifiers/masks, source and constant-destination relative addressing, invalid destination-addressing error contracts, flow control, and broad opcode lowering source contracts |
| Core draw data is built by pure state-to-value helpers | ✅ | `makeCanonicalDrawStateFromState()` plus draw-run helpers build production `CanonicalDrawState` / `DrawRunDesc` data, and large constants/matrices/clip planes now flow through `DrawUniformPayload` handles instead of hot draw-state records; `makeDrawDescFromState()` remains fixture/offline only. `state_draw_transform_spec` covers draw args, constants and canonical constant hashes, shader refs, resource bindings, sparse stream/index bindings, vertex decl/FVF hashes, RT/DS attachment masks/levels/sample counts, viewport/scissor, render/sampler/TSS including D3DTA modifier bits, FFP keys, transform multiply order, blend transform slots, and clip-plane boundary values |
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
| Interval frame sequence + bounded video capture | ⚠️ | R-WILD-4.4-R-WILD-4.6; bundle writer, schema selftest, experiment CLI parsing, scheduled window frame collection, dropped-frame metadata, bounded `screencapture -v -V` video collection, and renderer-side multi-frame internal BMP production are implemented. Non-catalogue WSI runner adoption and renderer failure sidecars/counters remain open. |
| Bootstrap verified entry: `dxmt9-wsi-present-local` on Heroic Wine 11.5 | ✅ | local workflow validation |
| Verified real application entries: `dx-sdk-basichlsl`, `dx-sdk-tutorial07`, `dx-sdk-hdrformats`, `dxut-simple-sample`, `irrlicht-managed-lights` | ✅ | Heroic Wine 11.5, direct capture, SSIM 1.0000 |
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
| 1 | Extend the `shader_runner_dxmt9` runtime probe layer beyond texture / dependent-read / VS color / viewport / half-pixel / color-write probes to mip/4x4/LOD and additional combined render-state cases; alpha-test, alpha-blend alpha-channel, oDepth, MRT, fill, clip-plane, fog, and sRGB now have initial readback coverage | R-TEST-1.7–R-TEST-1.10 |
| 2 | Promote the PE conformance scaffold inventory by fixing the newly exposed app-local conformance failures, then running app-local and builtin Wine lanes for all scaffolded cases | R-TEST-12.1, R-TEST-12.20 |
| 3 | Finish factory HRESULT parity and validation coverage, including `CheckDeviceFormatConversion`, multisample quality levels, device-type enum handling, and any selected optional export-profile stubs beyond the current auxiliary set | R-CORE-1.9, R-CORE-1.11-R-CORE-1.15, R-TEST-12.9-R-TEST-12.10, R-TEST-12.15-R-TEST-12.16 |
| 4 | Implement and verify D3D9Ex user-memory texture/offscreen-surface paths and shared-handle error policy | R-CORE-4.11-R-CORE-4.12, R-TEST-12.11 |
| 5 | Implement Wine-oracle D3D9Ex QI conformance and finish Ex/display/swap-chain validation | R-CORE-1.6, R-CORE-10.2-R-CORE-10.4, R-CORE-10.17, R-CORE-10.18, R-TEST-12.4 |
| 6 | Expand Wine stateblock conformance beyond the current compact scaffold and implement full `D3DSBT_*` masks/resource/reset interactions | R-CORE-3.7, R-CORE-3.8, R-TEST-12.5, R-TEST-12.19 |
| 7 | Expand compact reset/window scaffolds if runtime evidence exposes missing Wine-visible edge cases, then promote device lifetime/refcount, query validation, resource wrapper, and scene scaffolds with runtime evidence | R-CORE-2.6, R-CORE-2.8, R-CORE-4.8-R-CORE-4.10, R-CORE-8.3, R-TEST-12.3, R-TEST-12.12-R-TEST-12.14, R-TEST-12.17-R-TEST-12.18 |
| 8 | Build and verify the upstream-style PE targets: `d3d9.dll` + `winemetal.dll` + `winemetal.so` | d3d9/wsi §6, §9 |
| 9 | Run the Wine WSI smoke on the upstream-style deployment and promote the gap status if it passes | R-TEST-11.3 |
| 10 | D3D8 entry point + IDirect3D8 factory + resource wrappers | R-D3D8-1.1, R-D3D8-2.1 |
| 11 | Standardize debug-tool evidence across Metal, WSI/window, Wine unix/provider, headless lanes, boundary dumps, render frame sequences, video segments, and the environment-variable registry | R-TEST-14.1-R-TEST-14.18, R-WILD-4.4-R-WILD-4.6 |
