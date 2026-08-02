---
type: "Spec Gap"
title: "D3D9 Gap"
description: "Implementation and evidence gaps for the D3D9 frontend."
tags: [specs, gap, d3d9]
---

# D3D9 Gap

Domain-owned implementation and evidence gap tracker. Use the [root gap index](../gap.md) for cross-domain rollup. Detailed API coverage lives in [gap_d3d9](gap_d3d9.md), and Wine-oracle mapping lives in [tests/gap_d3d9_wine_test](../tests/gap_d3d9_wine_test.md).

## D3D9 Layer

| Area | Status | Evidence / notes |
|---|---|---|
| Factory, adapter enum, `CheckDeviceFormat` / conversion | ⚠️ | Functional path exists. **T3 (2026-05-08)**: `CheckDepthStencilMatch` now does real bit-depth compatibility (not unconditional `S_OK`); `CheckDeviceFormatConversion` does real format-pair check (identity + A8R8G8B8↔X8R8G8B8) instead of unconditional `D3DERR_NOTAVAILABLE`; `CheckDeviceFormat` rejects VB/IB rtype with `D3DERR_INVALIDCALL` and downgrades AUTOGENMIPMAP success to `D3DOK_NOAUTOGEN`. Helpers in `core_format_utils.hpp`. PE factory still needs adapter index validation, fullscreen display format strictness, adapter format allowlist, resource type mapping parity. |
| `GetAdapterIdentifier` | ✅ | `Factory::getAdapterIdentifier()` |
| `EnumAdapterModes` | ✅ | `Factory::enumAdapterModes()` |
| `GetAdapterDisplayMode` | ✅ | `Factory::getAdapterDisplayMode()` |
| `GetAdapterMonitor` | ⚠️ | Method surface is wired, but the unix provider returns a stable stub `HMONITOR`; there is no WindowServer monitor identity mapping. |
| `CheckDeviceType` | ⚠️ | HAL path exists. **PE validation landed 2026-05-24 (`fc242c2`):** invalid adapter / unavailable type / invalid enum / fullscreen display-format HRESULT parity implemented + native-verified (`dxmt9-core-factory-validation-spec`). Remaining: Wine-conformance-run lane/arch breadth. |
| `CheckDeviceMultiSampleType` | ⚠️ | Factory + Device path exists. **PE validation landed 2026-05-24 (`fc242c2`):** `D3DMULTISAMPLE_NONE`→`D3D_OK`+quality=1, invalid-enum / unknown-msType→`INVALIDCALL` (`pQualityLevels` preserved), unsupported sample-count→`NOTAVAILABLE`; native-verified (`dxmt9-core-factory-validation-spec`). Remaining: Wine-conformance-run breadth. |
| PE `d3d9.dll` export surface | ⚠️ | The scoped auxiliary set is now implemented with DXVK/D9VK-compatible ordinals: `Direct3DCreate9On12`, `D3DPERF_*`, and `DebugSetMute`. Focused x64 app-local export/auxiliary tests pass. The row remains partial until the broader selected export profile, such as optional `PSGP*` / `DebugSetLevel` compatibility if adopted, is explicitly audited |
| Device lifecycle: Reset, `D3DPOOL_DEFAULT` invalidation | ⚠️ | Functional reset exists. **T2 (2026-05-08)**: `Reset()`/`ResetEx()` now mirror viewport+scissor to fullscreen `{0,0,W,H}` on success matching Wine `d3d9_device_Reset`; `releaseAllBound()` explicitly nullifies the C-side render-target slot 0 and depth-stencil so no stale Metal handle survives; lost-device gate now enforces `D3DERR_DEVICELOST` early-return on `Present`/`PresentEx`/`BeginScene`/`EndScene`/`Clear`/`DrawPrimitive`/`DrawIndexedPrimitive`/`DrawPrimitiveUP`/`DrawIndexedPrimitiveUP`/`ProcessVertices`. MANAGED preservation already correct in `invalidateDefaultPoolResources`. Still needed: present-parameter validation parity, full Wine `test_reset_resources` / `test_lost_device` matrix coverage. |
| Fullscreen `createDevice` | ⚠️ | `normalizePresentParameters()` exists. **PE present-parameter validation landed 2026-05-24 (`bfaf1fb`):** Reset/ResetEx/CreateAdditionalSwapChain validate SwapEffect / BackBufferCount (cap 3 / 30 extended) / PresentationInterval → `D3DERR_INVALIDCALL`; native-verified (`dxmt9-core-d3d9-device-validation-spec`). Remaining: exact creation HRESULT propagation + Wine-conformance-run breadth. |
| Device-lost: trigger + recovery | ✅ | `setDeviceLostObserver()` |
| `TestCooperativeLevel` | ✅ | Returns `D3DERR_DEVICELOST` when lost |
| `CreateAdditionalSwapChain` | ✅ | `Device::createAdditionalSwapChain()` |
| Device state shadow (render / texture / sampler / transform / lights / stencil) | ✅ | `DeviceState`, all `Set*`/`Get*` methods |
| BeginScene / EndScene | ✅ | Nested-call validation |
| StateBlock capture / restore | ⚠️ | Full-state copy exists. **T1 (2026-05-08)**: `D3DSBT_VERTEXSTATE` apply now copies the per-stage TSS slice (`D3DTSS_TEXCOORDINDEX`, `D3DTSS_TEXTURETRANSFORMFLAGS`) matching Wine `vertex_states_texture[]`; `kPixelStateRenderStates` and `kVertexStateRenderStates` already match Wine's `pixel_states_render[]` and `vertex_states_render[]`. **C5 (2026-05-10, `a4252db`)**: per-frequency dirty mask now invalidated on `IDirect3DStateBlock9::Apply` via new `CommandQueue::markPendingDirtyAll()` (uniform::DirtyState all 12 bits — VS/PS F/I/B + FFP_VS Transforms/Clip/Viewport + FFP_PS Fog/Alpha/TexFactor) so the next chunk re-uploads every uniform sub-block. The four `BeginStateBlock` / `EndStateBlock` / `CreateStateBlock` / Apply-during-record invalid-call cases already returned `INVALIDCALL` in master; commit added one missing test for EndStateBlock-without-Begin. Still needed: full Wine `test_state_block_savedstates` matrix coverage (per-state-byte equality across capture/apply cycles). |
| Hot-path CommandChunk recording and data normalizers | ⚠️ | V2 chunk records, sparse draw sections, `APPLY_STATE`, bulk retention, and draw-run paths exist. `makeCanonicalDrawStateFromState()` / draw-run helpers cover production state-to-flat-draw transforms, while `makeDrawDescFromState()` is fixture/offline coverage only. Current native evidence is `chunk_record_v2_{validation,replay,allocation}_spec`, `pe_chunk_record_v2_value_spec`, and `resource_hazard_v2_spec`; the retired V1 import/replay fixture family was removed on 2026-07-19. Remaining alignment is replacing the PE-local semantic-record adapter with direct typed V2 producers and auditing broader barrier/hazard behavior against R-CORE-11.14-R-CORE-11.18. |
| DrawPrimitive, DrawIndexedPrimitive, UP variants | ✅ | All four variants |
| TriangleFan decomposition | ✅ | `decomposeTriangleFanIndices()` |
| Half-pixel offset | ✅ | `halfPixelFixup()` |
| FfpVertexKey / FfpPixelKey generation | ✅ | `makeFfpVertexKey()`, `makeFfpPixelKey()` |
| Shader bytecode storage + hash | ✅ | `ShaderBytecode`, FNV-1a hash cache |
| Shader IR spec contracts (`R-CORE-SHADER-*`) | ⚠️ | New spec at `specs/d3d9/shader/{requirements,spec}.md` consolidates the IR data model, semantic translation (FFP key / half-pixel / alpha-test / SM 1.x output clamp), analysis-pass purity, MSL emission contract, variant-axis classification, cache hash composition, Wine/vkd3d oracle boundaries, and formal/property validation requirements. Required passes implemented (constant/sampler/VS-out/PS-in semantics, max temp index). The spec frames per-output VSOut precision (FP16) as the shader-layer's *next unproven candidate* — the proven control variable for `VS Buffer Device Memory Bytes Written` is post-transform VS invocation count, owned by the index-locality reorder path in `specs/backend/` (current ceiling: GPU −13.86%, VS write −13.19% on 3DMark05 GT1). The hidden backend bucket is documented as a native Apple vertex/tiler/parameter-storage attribution model, not a public Metal ABI object the IR can shrink directly. **Open items:** (a) IR-level precision-inference pass — the prior `DXMT9_FS_HALF_PRECISION` text-rewrite (~33% of SFIV FS compile) was removed as non-functional; requirement remains open for a future design at `specs/d3d9/shader/spec.md` §4 with conservative mixed-write handling and VS-first strategy. (b) Diagnostic VS-output half-precision opt-in is wired through `DXMT9_PROBE_HALF_VSOUT` / `ShaderSourceContext::enableHalfVSOut`; production precision inference, per-output plan emission, and correctness/counter oracle remain open. (c) Multi-axis half-precision correctness oracle (`R-CORE-SHADER-3.10`, `R-CORE-SHADER-8.3`, `spec.md` §9.3) — paired VS+FS replay across five state classes (depth-write opaque, depth-read no-blend, blend-off, alpha-blend, screen-blend), with active-pixel coverage gate, per-output tolerance (colour ≤ 1 LSB, depth/alpha exact), and a counter gate proving `VS Buffer Device Memory Bytes Written` moves at baseline scale. None of these axes are built. (d) Formal/property validation suite (`R-CORE-SHADER-8.5` through `R-CORE-SHADER-8.10`) for precision inference, VSOut liveness, cache-key completeness, D3DBC decoder safety, emitter determinism/purity, and SM 1.x output clamp version gating. (e) `SpirvModule` → `ShaderIR` rename (non-blocking cleanup; introduce `using ShaderIR = SpirvModule;` alias, then rename callers incrementally). (f) Backend-side shader-hash consumption contract — the shader spec now scopes `DXMT9_PERF_ENCODER_BREAKDOWN`'s shader-hash surfacing out (R-CORE-SHADER-7.3); the matching backend / perf-attribution contract is not yet specified anywhere. (g) vkd3d decoder-oracle coverage diff and drift-checked focused `.shader_test` imports for any SM1-SM3 edge cases missing from dxmt9's local corpus. |
| Format table | ⚠️ | `formatTable()` exists; central explicit classification for all FOURCC/pseudo-formats such as `RESZ` and `NULL` still needs audit |
| Vendor FOURCC / dependent-texture / mipmap upload policy | ⚠️ | scaffolded via 7 PE conformance entries (`tests/conformance/d3d9/d3d9_conformance_vendor_policy.c`) — explicit unsupported HRESULT documented for INTZ/FETCH4/RESZ; TEXBEM/TEXDEPTH/test_mipmap_upload deferred to NATIVE/SR follow-up; `test_miptree_layout_lock_pitch_policy` captures the public-ABI LockRect pitch + GetLevelDesc round-trip (the value-level math is in `tests/native/core/core_d3d9_miptree_layout_spec.cpp`). Policy decisions are now PE-captured even though runtime evidence remains deferred. |
| Shader-runner fixtures: SR-blocked `visual.c` entries | ⚠️ | Each entry now has a narrow PE scaffold in `tests/conformance/d3d9/d3d9_conformance_sr_blocked_policy.c` capturing the public-ABI portion (`bumpenvmap_tss_policy`, `pretransformed_vertex_declaration_policy`, `vface_pixel_shader_create_policy`, `fp_special_caps_policy`). The full rasterization aspect still requires SR runner DSL / oracle infrastructure (TEXBEM runner support, XYZRHW pretransformed-varying fixture, VFACE input fixture, NaN/Inf tolerance policy) and remains deferred. |
| `makeDefaultCaps()` | ✅ | R-CAPS-1 through R-CAPS-9; DITHER and hardware cursor caps are not advertised while their D3D9 state remains shadow-only. |
| Buffer / Texture / Surface lifecycle | ⚠️ | Pool-based reset behavior exists. D3D9Ex SYSTEMMEM user-memory policy is implemented for the Wine-observed 2D texture/offscreen-surface cases, with class-specific failures elsewhere. DEFAULT-pool texture/cube/volume/VB/IB/RT/DS/offscreen creation now returns an opaque 32-bit token for zero input and reopens a matching token onto the same retained Metal backing, including a distinct destination-device pool handle; mismatched class or description fails. Native pins: `dxmt9-core-device-com-spec` and `dxmt9-core-d3d9-shared-handle-spec`. Remaining Tier-2 gap: tokens are unix-provider process-local and are not real closable/cross-process Win32 handles; IOSurface or `MTLSharedTextureHandle` export/import plus handle-lifetime transport is still required. |
| `UpdateSurface`, `UpdateTexture`, `StretchRect`, `ColorFill`, `GetRenderTargetData`, `GetFrontBufferData` | ✅ | Core copy logic plus synchronous swap-chain present-source readback; front-buffer capture is limited to the dxmt9 swap-chain image and does not include WindowServer composition. |
| `DXMT9_LAYER_FRAMEBUFFER_ONLY` × backbuffer `Lock` / `GetRenderTargetData` semantics (R-CORE-WSI-6.1, R-CORE-WSI-6.2) | ⚠️ | Spec landed 2026-05-22 in `specs/d3d9/wsi/requirements.md` §6, classification **D** (no `framebufferOnly` branch on the D3D9 Lock / readback paths — the toggle is a pure present-side optimisation; the dxmt9 backbuffer `MTLTexture` is allocated independently from the `CAMetalLayer` drawable). Code inspection: `src/d3d9/d3d9_pe_device_child_surface.cpp:397` (`LockRect`), `src/d3d9/d3d9_pe_device.cpp:2426` (`GetRenderTargetData`), `src/d3d9/core_surface.cpp:354` (`Device::getRenderTargetData`). Missing evidence: Wine-oracle PE conformance case asserting that `Lock` + `GetRenderTargetData` on the swap-chain backbuffer succeed and return the rendered pixels with `DXMT9_LAYER_FRAMEBUFFER_ONLY=1` set; today the toggle is exercised only in performance probes (SFIV), not correctness probes. |
| Query: EVENT, OCCLUSION, TIMESTAMP, TIMESTAMPFREQ, TIMESTAMPDISJOINT | ⚠️ | `submittedSeqId_` / `completedSeqId_` waterline exists. **PE validation landed 2026-05-24 (`bfaf1fb`):** `CreateQuery` unsupported-type gate + per-type `GetDataSize` (EVENT=4, OCCLUSION=4, TIMESTAMP=8, TIMESTAMPDISJOINT=4, TIMESTAMPFREQ=8); native-verified (`dxmt9-core-d3d9-device-validation-spec`). Remaining: Windows-visible zero-initialisation behaviour + Wine-conformance-run breadth. |
| Clip planes | ✅ | Fixed-function world-space planes are converted with `inverse(View * Projection)` under the D3D row-vector convention; programmable vertex shaders preserve the app's clip-space coefficients. **2026-07-17:** this split fixes 3DMark05 GT3 ocean draws that were incorrectly clipped by the former inverse-transpose WVP transform. The FFP and metal_ir emitters retain the Apple-GPU-compatible single `[[clip_distance]]` slot using `min(dot(plane_i, position))` across enabled planes. **Regression coverage:** native fixed-function/programmatic coordinate-space assertions in `dxmt9-core-state-draw-transform-spec`, plus `dxmt9_clip_plane_halfspace_readback.shader_test`, `dxmt9_clip_plane_runtime_readback.shader_test`, and `dxmt9_clip_plane_programmable_space_readback.shader_test`. |
| MSAA | ✅ | `sampleCount()`, `RenderTargetAttachment.sampleCount` |
| 3DMark05 GT3 black-sea rendering (early scene and dusk waterline) | ✅ RESOLVED (2026-07-18) | The end-to-end GT3 heuristic run after the fixed-function contract pass (`2d1eedb3`) completed without a black-sea interval, including the previously reported early 15–20 s region and the dusk waterline window. The complete fix set is the clip-plane coordinate-space correction (`1ef1815a`), all 16 pixel-sampler bindings (`ebc069d2`), and the fixed-function state/lighting contracts (`2d1eedb3` plus the 2026-07-18 initial-state follow-up). Earlier two-frame residual and wave-detail hypotheses are superseded by the full-run result; their captures remain available in repository history for regression archaeology. |
| COM: `IDirect3D9` — full factory interface | ⚠️ | Method surface exists; PE code gates `Direct3DCreate9()` Ex QI, but Wine conformance coverage and stricter validation are still pending |
| COM: `IDirect3DDevice9` — full device method surface | ✅ | All 40+ methods |
| COM: `IDirect3DSwapChain9` | ✅ | Present, backBuffer, depthStencilSurface |
| COM: `Direct3DCreate9(sdkVersion)` | ✅ | Returns `nullptr` for wrong SDK version |
| TLA+ `SeqIdSafety` / `BoundedInflight` / `QueryResolutionSafety` assertions | ✅ | `DXMT_ASSERT` with `// TLA+:` comments |
| `IDirect3D9Ex` — `Direct3DCreate9Ex`, `GetAdapterModeCountEx`, `EnumAdapterModesEx`, `GetAdapterDisplayModeEx`, `GetAdapterLUID`, `CreateDeviceEx` | ⚠️ | Method surface exists; PE factory/device Ex QI is now gated by creation mode, while display-mode validation, `CreateDeviceEx` mode validation, exact HRESULT propagation, and Wine `d3d9ex.c` coverage remain pending |
| `IDirect3DDevice9Ex` — `CheckDeviceState`, `ResetEx`, `PresentEx`, `SetMaximumFrameLatency`, `GetMaximumFrameLatency`, `WaitForVBlank`, `CheckResourceResidency`, `GetGPUThreadPriority`, `SetGPUThreadPriority`, `SetConvolutionMonoKernel`, `ComposeRects`, `CreateRenderTargetEx`, `CreateOffscreenPlainSurfaceEx`, `CreateDepthStencilSurfaceEx`, `GetDisplayModeEx` | ⚠️ | Method surface exists; device and swap-chain Ex QI are gated by extended-device creation. **T2 (2026-05-08)**: `ResetEx` mirrors viewport/scissor reset and clears `deviceNotReset_` on success; `PresentEx` gates on `D3DERR_DEVICELOST`. **T4 (2026-05-08)**: shared-handle SYSTEMMEM 1-mip texture and offscreen surface paths land. Still needed: real DEFAULT-pool cross-process shared backing, full `CreateOffscreenPlainSurfaceEx` shared-handle coverage, convolution/`ComposeRects`, real present statistics, and broader Wine `d3d9ex.c` lane coverage. |

**The D3D9 layer is functionally broad but no longer classified as complete.**
The review of Wine's D3D9 tests is treated as a Windows D3D9 behavioural-oracle
review. It added stricter conformance requirements for Ex exposure, PE
auxiliary exports, state blocks, public COM refcounts, factory validation,
format conversion, multisample quality-level behaviour, presentation-parameter
validation, D3D9Ex user-memory resources, shared-handle policy, exact HRESULT
propagation, query validation, and lost-device/reset behaviour.

**Wine-oracle test mapping is now complete.** All 268 Wine
`{visual,device,d3d9ex,stateblock}.c` tests are mapped to dxmt9 PE
conformance evidence (`../tests/gap_d3d9_wine_test.md`, 268/268 covered as of
HEAD `4a3aaa0`). The per-row "still need Wine-oracle conformance
coverage" notes above therefore now mean *lane / arch breadth promotion*
(app-local + x86 lanes; the builtin x64 lane is largely passing) rather
than a missing scaffold — see the Tests Layer rows. Separately, the
per-spec-item audit in `gap_d3d9.md` (2026-05-23) surfaced concrete
implementation gaps that were *not* lane-breadth issues. The 2026-05-24 pass
closed the then-unclassified silent fall-throughs; it did not make the full
D3D9 surface complete. Explicit failures, partial implementations, advertised
capability mismatches, and intentional default/no-op contracts remain tracked
below and in `gap_d3d9.md`.

### D3D9 API Coverage Inventory

Per-item inventory lives in [D3D9 API coverage inventory](gap_d3d9.md) (a regenerable
read-only ripgrep tracker, not a spec). Current gaps are summarized first; the
original **2026-05-23** four-agent audit totals are retained afterward as a
historical baseline.

Current implementation gaps, ordered by compatibility impact:

| Gap | Classification | Missing behavior |
|---|---|---|
| `ProcessVertices` / software vertex processing | partial implementation | The covered fixed-function and programmable subsets now include explicit `DP3` and `DP4` readback coverage; texture/sample opcodes beyond 2D `TEXLDL`, exotic declaration combinations, and remaining clipping/lighting edge cases remain deferred. |
| Legacy raster and sampler state | partial or capability-gated | The D3D9 line-AA toggle, `D3DSAMP_ELEMENTINDEX`, and `D3DSAMP_DMAPOFFSET` have no backend semantics. |
| D3D9Ex sharing and composition | explicit/partial implementation | DEFAULT-pool shared handles do not provide cross-process backing; `SetConvolutionMonoKernel` and `ComposeRects` return `E_NOTIMPL`; present statistics are default zero values. |
| Factory, reset, and display parity | validation/host-integration gap | Adapter/fullscreen/resource-type validation and exact HRESULT parity remain incomplete; monitor identity and raster status are synthetic host approximations. |

| Category | Total rows | ✅ full | ⚠️ partial | ❌ silent gap |
|---|---:|---:|---:|---:|
| A. Shader bytecode (SIO opcodes 0..95) | 96 | 96 | 0 | 0 |
| A. SPR register kinds (0..18) | 19 | 14 | 0 | 0 |
| A. DECLTYPE (0..17 + UNUSED) | 18 | 17 | 0 | 1 |
| A. DECLUSAGE (0..13) | 14 | 8 | 0 | 6 |
| A. DECLMETHOD | 7 | 1 | 0 | 6 |
| B. D3DRENDERSTATETYPE | 102 | ~52 | ~12 | ~38 |
| B. D3DTEXTURESTAGESTATETYPE | 22 | 17 | 0 | 5 |
| B. D3DSAMPLERSTATETYPE | 13 | 10 | 1 | 2 |
| B. D3DTRANSFORMSTATETYPE | 13 | 13 | 0 | 0 |
| B. Light / Material / Viewport / ClipStatus / Gamma | 32 | ~10 | ~10 | ~12 |
| C. D3DFORMAT (standard + FOURCC + vendor pseudo) | 72 | ~45 | ~5 | ~22 |
| C. D3DCAPS9 fields | 75 | 53 | 0 | 22 |
| C. PresentParameters / DisplayMode / AdapterIdentifier / RasterStatus | 33 | ~22 | ~5 | ~6 |
| D. COM methods (21 interfaces) | 225 | 176 | 13 | 33 |
| **Grand total** | **~803** | **~534** | **~46** | **~163** |

**Historical closure status (2026-05-24):** the re-audit found ~13 audited
silent fall-throughs already implemented on `master`, and the implementation
pass classified or closed the rest of that audit batch. This statement applies
only to the historical silent-coverage batch, not to the current implementation
gaps listed above. The findings closed in that batch were:

| Finding (2026-05-23 audit) | Resolution |
|---|---|
| `SAMP_BORDER_COLOR` slot (was 15, Wine = 4) | ✅ fixed to 4 + regression `static_assert` |
| 23 silent-`S_OK` COM stubs | ✅ GammaRamp / RasterStatus real impl; NPatch / DeletePatch / ClipStatus / GPU-priority / CheckResidency contract-gated |
| 6 D3DDECLUSAGE + 6 D3DDECLMETHOD silent fall-through | ✅ `DecoderReject` + perf counter |
| Point/Spot FFP lighting (directional-only) | ✅ implemented (Position/Range/Atten/Theta/Phi) |
| Vendor formats `INTZ`/`DF16`/`DF24`/`RESZ`/`NULL`/`ATOC` | ✅ implemented (RESZ end-to-end); `NVDB`/`RAWZ` → explicit `NOTAVAILABLE` |
| `SAMP_MIPMAPLODBIAS` unplumbed | ✅ shader-side `sample(…, bias(b))` + PSO-variant gated |
| `AlphaCmpCaps` source / `RASTER ScanLine` / `AdapterIdentifier9` GUID+WHQL | ✅ all wired |
| `D3DCLIPSTATUS9` / `D3DGAMMARAMP` log-only | ✅ GammaRamp real impl; ClipStatus = Wine-matching stub + defined default |
| `D3DRS_TWOSIDEDSTENCILMODE` / `D3DRS_WRAP0..15` | ✅ implemented (per-face stencil ops / accepted no-op) |

Outstanding work for the closed findings in this historical table is deferred
evidence: GPU-runtime pixel validation (RESZ MSAA→INTZ readback, NULL
color-attachment omission, MIPMAPLODBIAS mip selection, tile-FFP↔portable
equality) and conformance Wine-run validation of the new PE gates. This does not
supersede the live implementation gaps above.

**2026-05-25 — why these are still "deferred": the blocker is `shader_runner_dxmt9`
DSL expressiveness, NOT the dxmt9 implementation** (sub-agent investigation; each
feature's `src/` path was confirmed implemented + reachable). Actionable per-feature
unblocks (all are runner/`tests/shader_runner/shader_runner_dxmt9.cpp` extensions):

| Feature | Runner DSL gap | Smallest unblock | Recommended vehicle |
|---|---|---|---|
| **`SAMP_MIPMAPLODBIAS`** | `parseDxmt9Sampler` (~line 1221-1260) has no key for sampler-state 8; `setSamplerState(stage,8,value)` is otherwise reachable via `applyDxmt9SamplerSetups` | add one `lod_bias`/`mipmaplodbias` branch using the existing `bitCastFloatState` float→u32 helper (~5 lines) | **`shader_runner` (tractable now)** — mip/LOD corpus already exists to model on |
| **`D3DFMT_NULL` depth-only** | (a) `parseDxmt9Format` has no `null` token + RT slot 0 hardcoded to backbuffer; (b) no depth readback / depth-as-texture sampling | larger: NULL-as-effective-RT + a depth-readback directive | **native/fake-backend render-pass-attachment spec** (assert color attachment omitted, depth retained) is a better fit than the readback runner |
| **tile-FFP ↔ portable equality** | `selectTileFfpForPass` (`dxmt9_pipeline_cache.cpp:747`) + `setSupportsApple3` (`dxmt9_command_queue.cpp:144`) are pure functions with **no env escape hatch** (unlike the sibling `DXMT9_DISABLE_ARGBUF_HYBRID` 11 lines below); also every probe-bearing FFP fog test is textured → selector forces portable, so no tile-path readback exists | add a `DXMT9_TILE_FFP=force\|off\|auto` toggle at the selector / supportsApple3 override, then A/B the same draw | **`shader_runner` once the toggle lands** (small `src/` env addition) |
| **RESZ MSAA→INTZ** | all 4 steps unexpressible: no MSAA depth-stencil RT directive, no RESZ/`POINTSIZE`-sentinel trigger, no `INTZ` format token/sampling, no depth readback (`Device::reszDepthResolve`/`submitDepthResolve` exist but the runner drives the core `Device` with none of these exposed) | substantial: 4-area DSL extension | native/fake-backend depth-resolve spec or a Wine PE conformance exe; readback-runner is the highest-effort path |

**2026-05-25 — implementation pass (parallel worktree sub-agents; main session merge+verify):**

- **`SAMP_MIPMAPLODBIAS` ✅ CLOSED + GPU-VERIFIED** (`b43c541`): added the `lod_bias` key to `parseDxmt9Sampler` + corpus probes (`dxmt9_mipmaplodbias_readback` + zero-control), registered in MANIFEST. Readback confirms the bias monotonically shifts the sampled mip (control bias 0 → mip 0 red; +4 → mip 1 green; +5 → mip 2 blue), matching the LOD math. **The feature is correct.**
- **NULL + RESZ ✅ CLOSED via native unit tests** (`483365e`): `dxmt9-null-rt-attachment-spec` (R-FORMAT-12) extracts the `beginRenderPass` RT0-admission + color-attachment-inclusion decisions into pure transforms (`renderPassAdmitsRt0` / `colorAttachmentIncluded`) and asserts the colorless-NULL policy (color slot omitted, depth retained). `dxmt9-resz-depth-resolve-spec` (R-FORMAT-11) asserts POINTSIZE-sentinel detection + `reszDepthResolve` `DepthResolveDesc` (MSAA source + INTZ level-0 dest) via a recording backend. Both pass. (These verify the decision LOGIC deterministically; full GPU pixel readback still needs the runner-DSL work above, but the core contracts are now unit-covered.)
- **tile-FFP ✅ FIXED (two-stage encode landed 2026-05-25) — default stays `off` pending a perf benchmark** (`DXMT9_TILE_FFP` toggle landed `e165154`): the toggle works (force→tile, off→portable, proven by `tile_ffp_pass_count`/`portable_ffp_pass_count`), and the A/B it enabled uncovered that **the tile-FFP path never rasterizes the draw's base colour into the imageblock** — so any draw routed to tile renders the *cleared* imageblock post-processed by the tile kernel, i.e. **black**, regardless of fog/alpha-test. Evidence (M1/apple7): a plain white quad with NO fog and NO alpha-test reads `(255,255,255)` portable but `(0,0,0)` tile; the tile run shows `draw_geometry_samples=0` / `bind_pipeline=0` / no draw counters while `tile_ffp_pass_count=1`, vs portable `draw_geometry_ffp=1` / `draw_vertices=3`. **Root cause:** `src/dxmt9/dxmt9_draw_encoder.mm:~1435-1459` (wired by `523b66e`) does `setTileRenderPipelineState` + `dispatchThreadsPerTile` **instead of** the base-colour `setRenderPipelineState` + `drawPrimitives`; the tile kernel `makeFfpTilePixelSource` (`dxmt9_ffp_shaders.cpp:~1302` `float4 color = float4(slot->color);`) therefore reads the cleared imageblock, not the fragment colour. This contradicts the programmable-blend design (`specs/backend/spec.md §13.5`, R-BACK-13.1) where the fragment stage must write the base colour FIRST and the tile kernel only applies fog/alpha-test/A2C over it. **Impact:** tile-FFP is default-on for eligible draws on Apple Silicon (the `selectTileFfpForPass` eligibility: FFP + fog/alpha-test/A2C, non-textured), so a real app using non-textured FFP fog/alpha-test draws renders that geometry **black**. (Narrowly triggered — most FFP draws are textured → forced portable → correct — which is why it went unnoticed; SFIV etc. are shader-based, not FFP.) **Proper fix = two-stage tile encode:** bind a base-colour render PSO (portable FFP fragment, fog+alpha-test stripped) and issue `drawPrimitives` so the geometry lands in the imageblock, THEN bind the tile PSO + `dispatchThreadsPerTile` to apply fog/alpha-test/A2C — within one render encoder; spans variant key + a base-colour pixel-source variant + pipeline cache (two PSOs) + encoder reorder + GPU bit-identity validation. **Interim safety APPLIED (2026-05-25):** `DXMT9_TILE_FFP` default flipped `auto`→`off` (`tileFfpModeOverride` in `dxmt9_pipeline_cache.cpp`) so every FFP draw now takes the correct portable lane by default; verified an eligible FFP draw records `portable_ffp_pass_count=1` (tile=0) with no env, while `DXMT9_TILE_FFP=auto` still reaches the tile path (`tile_ffp_pass_count=1`) for the two-stage-encode fix to validate behind the flag. Portable is correct and was the original path; the only loss is the (unvalidated) tile perf optimisation. The default flips back to `auto` once the two-stage encode lands + GPU-readback equality holds. **Repro** (`.shader_test`, kept here since the corpus file was removed to keep `dxmt9-manifest-check` green): `clear rgba(0,0,0,1); dxmt9-render-state fog=on fogmode=linear fogstart=0 fogend=1 fogcolor=rgba(0.25,0.5,0.75,1); alpha-test enable greaterequal 0.5; draw quad; probe (32,32)` — run under `DXMT9_PREWARM=disabled DXMT9_TILE_FFP=force` (→ buggy `(0,0,0)`) vs `=off` (→ correct `(64,128,191)`), both with `DXMT_PERF_COUNTERS=1` to confirm the paths differ. **RESOLVED 2026-05-25 (6 commits `43ae1dd`..`7ed5fcb`, supervised mega-refactor):** implemented the two-stage encode (base-colour render PSO + `drawPrimitives` → then tile PSO + `dispatchThreadsPerTile`, one encoder). The fix exposed TWO further root causes beyond the missing base draw: (1) the tile kernel's imageblock access used `imageblock_data.data(tid)` (explicit-layout accessor) which **never compiled** → changed to `read(tid)`/`write(value,tid)` (implicit layout); (2) `setTileRenderPipelineState` is **not a valid selector on the M1 render encoder** (`AGXG13GFamilyRenderContext`, threw `NSInvalidArgumentException`) → the tile PSO is bound via `setRenderPipelineState` per §13.5, with `FfpPsConsts` on `setTileBuffer(3)`. Also runtime-gated the tile fog blend on `ffpPs.fogMode != 0`. **Verified (M1, supervisor re-ran):** the registered probe `tests/shader_runner/corpus/ffp/dxmt9_tile_ffp_fog_equality_readback.shader_test` PASSES under BOTH `=force` and `=off` (both `(64,128,191)`) with `draw_geometry_samples=1` (was 0); manifest-check ok (243 tests); `dxmt9-tile-ffp-selector-spec`/`-msl-spec` OK; FFP corpus no-regression. The `off` default is retained because the perf case is still unproven — single-draw `gpu_command_buffer_time_ms` was tile 0.039 ms vs portable 0.050 ms (noisy, not a workload benchmark); flipping the default back to `auto` should await a real benchmark proving tile beats portable.

`DXMT9_PREWARM=disabled` + isolated `DXMT9_CACHE_DIR` were used per run. The dxmt9
implementations themselves are unchanged and presumed correct — but **unverified on
GPU**; the argbuf precedent (a "landed" lane that was a GPU page fault) is the reason
these should not be assumed correct until a probe (via one of the vehicles above)
actually reads back the pixels.

---

---

**2026-08-02 — `visual_p8_texture_sampler_policy`: one test-harness bug (fixed) and one real dxmt9 bug (open).**

> **This entry replaces an earlier version that got the root cause backwards.**
> It claimed dxmt9 routed `mov oC0, r0` to a dead temp. It does not — dxmt9
> decoded exactly what the test emitted. The mis-encoding was in the test's own
> bytecode assembler macro. The white pixels were real; the attribution was not.

**Fixed — the test's register-type encoding.** `P8_PS_REGTYPE`
(`d3d9_conformance_visual_formats.c`) and `PROCESS_VS_REGTYPE`
(`d3d9_conformance_visual_misc.c`) placed the *high* two bits of the register
type at bit 8 instead of bits 11-12, i.e. inside the 11-bit register-index
field. mingw's `d3d9types.h` invites this: it defines
`D3DSP_REGTYPE_SHIFT2 = 8` while `D3DSP_REGTYPE_MASK2 = 0x1800`, which are
mutually inconsistent — the canonical D3D form is `(type << 8) & MASK2`, the
whole type shifted so its bits 3-4 land at 11-12.

Consequence: every register type `>= 8` decoded as `type & 7` with index `+256`.

| operand | emitted | decoded as |
|---|---|---|
| `oC0` (`COLOROUT`, 8) | `0x800F0100` | `TEMP` index **256** |
| `s0` (`SAMPLER`, 10) | `0xA00F0100` | `CONST` index 256 |

So `mov oC0, r0` became `r[256] = r[0]`, `outColor[0]` kept its `float4(1.0f)`
initialiser, and the shader emitted white — and the `float4 r[257]` local was
just the temp array sized to the bogus index. Types 0-7 are unaffected, which is
exactly why every fxc-compiled shader and the rest of the suite were fine. A
corrected sibling macro with a comment naming this same trap already existed at
`d3d9_conformance_visual_misc.c:689` (`SWVP_VS_REGTYPE`); the two others had not
been updated. Both now use the canonical `(type << 8) & D3DSP_REGTYPE_MASK2`.

Measured effect on case 155: **48 -> 20 FAIL lines.** The four white
`programmable_ps` invocations now read back the correct palette colours, and the
cube and volume palettized checks (which were also mis-typing their samplers, so
every variant emitted `texture2d<float>`) go to zero failures.

**Still open — a real dxmt9 mip-LOD bug, and it is not shader translation.** The
surviving 20 failures are `visual_formats.c:638-641` (16: the four `lod_texels`
invocations x four texels) and `:796-799` (4: the update-texture lane). They fail
identically on the fixed-function and programmable paths, returning level-0
texels where level-1 is expected. P8/A8P8 are expanded to RGBA per level PE-side
by `expandP8SubresourceToBackend` (`device_c_resources.cpp:258`), so the
suspicion is that only level 0 is expanded, or that the expanded levels are not
linked as mips. The case also still hangs past 300 s after those failures,
uninvestigated.

`visual_process_vertices_xyzhw_policy` (index 186) also used the buggy macro, but
still fails after the fix at `visual_misc.c:14142-14143` — ProcessVertices
coordinate assertions, a different failure. Whether the macro fix changed
anything there was not measured.

**Defence-in-depth note:** `validateRegisterIndex`
(`dxmt9_shader_decoder.cpp:173-215`) would have rejected a temp index of 256
against `kMaxTempIndex = 32`, but it is deliberately parked and uncalled since
`7abaa20e` (see the comment at 160-172). Instead the translator sized a 4 KB
local array and emitted silently-wrong output. Re-enabling it is its own project
— the parked comment records that it previously over-rejected valid SM3
control-flow operands — but this is the second time this session that a missing
range check turned a wrong input into wrong pixels rather than an error.
