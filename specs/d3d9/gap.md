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

**2026-08-02 — `SetLOD` / `D3DSAMP_MAXMIPLEVEL` are silently ignored when `D3DSAMP_MIPFILTER = D3DTEXF_NONE`. Not a P8 bug.**

The 16 surviving failures in `visual_p8_texture_sampler_policy` are this, and the
scope is every texture format, not palettized ones. Root cause in
`makeSamplerInfo` (`dxmt9_draw_encoder.mm`, both overloads — `:9466`/`:9527`):

```c
  switch (mipFilter) {
    case 2u: info.mip_filter = WMTSamplerMipFilterLinear;  break;
    case 1u: info.mip_filter = WMTSamplerMipFilterNearest; break;
    default: info.mip_filter = WMTSamplerMipFilterNotMipmapped; break;  // D3DTEXF_NONE
  }
  ...
  info.lod_min_clamp = std::max(lodMinClamp, static_cast<float>(maxMipLevel));
```

`D3DTEXF_NONE` maps to `MTLSamplerMipFilterNotMipmapped`, and **Metal ignores
`lodMinClamp` in that mode** — it always samples level 0. The clamp is computed
and handed across the bridge (`winemetal_private_api.mm:871`) but is inert. D3D9's
"do not filter between mips" and D3D9's "most-detailed level is N" are two
independent knobs; dxmt9 folds them into the one Metal mode that can only express
the first.

**Trigger:** `D3DSAMP_MIPFILTER == D3DTEXF_NONE` **and `SetLOD > 0`**. With
`MIPFILTER` set to `POINT` or `LINEAR` it behaves correctly.

> **Corrected after review — `D3DSAMP_MAXMIPLEVEL` is NOT part of the trigger.**
> The first version of this entry and of the fix treated the effective level as
> `max(SetLOD, MAXMIPLEVEL)` under `NONE` too. The cited oracle says the
> opposite, unconditionally: Wine `visual.c:4795`, *"with mipmapping disabled,
> the max mip level is ignored, only level 0 is used"*, asserted for
> `MAXMIPLEVEL` 0/1/2/3. `wined3d/stateblock.c:2933` implements exactly that:
> ```c
> else if (desc->mip_filter == WINED3D_TEXF_NONE)
>     desc->mip_base_level = texture->lod;                 /* MAXMIPLEVEL ignored */
> else
>     desc->mip_base_level = min(max(MAX_MIP_LEVEL, texture->lod), level_count - 1);
> ```
> So under `NONE` the base level is **`SetLOD` alone**. dxmt9's pre-existing
> `MAXMIPLEVEL`-under-`NONE` behaviour (level 0) was already correct, and the
> first fix broke it — verified: with the folded rule,
> `NONE + MAXMIPLEVEL=1` sampled level 1 (green) where the oracle demands level
> 0 (red).

**Verified on a non-palettized control**, which is what establishes the scope. The
repo's own passing corpus case
`tests/shader_runner/corpus/texture/dxmt9_setlod_maxmip_readback.shader_test`
(plain `A8R8G8B8`, `SetLOD 1`) was re-run with **only** `mip_filter=point` changed
to `none`:

| | result |
|---|---|
| `mip_filter=point` (as committed) | pass |
| `mip_filter=none` | **fail** — `actual_bgra=(0,0,255,255)`, the level-0 texel, where white (level 1) is expected |

The existing corpus misses it only because all three mip proxies use
`mip_filter=point`. `SAMP_MIPMAPLODBIAS`, recorded as closed and GPU-verified
elsewhere in this file, is a different knob and is unaffected.

**FIXED 2026-08-02.** Both `makeSamplerInfo` overloads now compute the base level
the way wined3d does — `SetLOD` alone under `NONE`, `max(MAXMIPLEVEL, SetLOD)`
otherwise — and, when the mode is `NotMipmapped` with a non-zero base level,
select `MipFilterNearest` and pin `lod_max_clamp` to that level. Nearest
selection with `min == max` cannot filter between mips, so both D3D9 semantics
hold. `SamplerKey` already keys on
`lodMinClampBits` (`dxmt9_pipeline_cache.hpp:280`), so no cache change was
needed. The blit/present helper `makeSampler` (`:9439`) is untouched; it has no
LOD.

Regression-pinned by two new corpus cases, both `mip_filter=none` variants of
existing ones, pinning **opposite** halves of the rule:
`dxmt9_setlod_maxmip_nomipfilter_readback` asserts `SetLOD` still selects its
level, and `dxmt9_ffp_sampler_max_mip_level_clamp_nomipfilter_readback` asserts
`MAXMIPLEVEL` is *ignored* (level 0). Both verified to discriminate: the first
reads `(0,0,255,255)` without the fix, and the second reads `(0,255,0,255)`
under the first, folded version of the fix. All four pre-existing mip cases pass
in every state, which is also the demonstration that the old corpus could not
see either bug.

Conformance case 155 went `20 -> 4-8` FAIL lines; the whole `:638-641` group is
gone and every residual is the separate `:796-799` issue below. The count was
**not stable at the time this was written** — the case still hung past 300 s and
how far it got varied. The hang was fixed later the same day (use-after-free,
below), and the residual turned out to be flaky on its own account, so the
instruction not to key a comparison to an exact number stands for a second,
independent reason.

**`:796-799` was entirely a test bug — RESOLVED.** Both formats read back exactly
`updated_expected` while the helper asserted `expected`. Cause: the invocations
at `visual_formats.c:1337` and `:1365` pass `updated_palette_index = 0`, so
`:588-590` runs `SetPaletteEntries(device, 0, updated_palette)` **while palette 0
is the current palette**, and the restore at `:649-651` is gated on
`if (updated_palette_index)` and never fires. The current texture palette is
device-global state consulted at draw time (Wine
`dlls/d3d8/tests/visual.c:2993 p8_texture_test`), so the leak makes every later
base-palette expectation unreachable. dxmt9 was right throughout. Fixed by re-issuing
`SetPaletteEntries(device, 0, palette)` before each of the four `UpdateTexture`
invocations.

> **Corrected 2026-08-02 after review.** This first said the restore also needed
> `SetCurrentTexturePalette(device, 0)`, "because the helper itself ends with
> `SetCurrentTexturePalette(device, 1)` and never switches back". **That is
> false** — `done_texture_bind:` at `visual_formats.c:835` restores current to 0
> unconditionally, and it predates this whole investigation. Only slot 0's
> *contents* leak; which palette is current never does.
>
> The consequence is an attribution error, not just a wrong sentence. The 8 → 4
> improvement was credited to those four `SetCurrentTexturePalette` calls; they
> are no-ops. What actually moved it is that the first fix patched only the two
> **fixed-function** call sites, and the second added `SetPaletteEntries` at the
> two **programmable** ones — 2 invocations × 4 checks = exactly the residual 8.
> The four no-op calls have been removed.

> **A "zero FAIL lines" result was published for this and was an artifact.** The
> case was still hanging at that point, so it never reached the checks. Once the
> use-after-free below was fixed the case ran to completion — `1,084` checks —
> and the leaked-palette failures were real and had simply never been reached.
> A green result from a run that dies early is not a green result.

> **This claim was published, retracted, and is now reinstated in a narrower
> form — the retraction was the error.** The original entry said the first draw
> after `UpdateTexture` onto a fresh `D3DPOOL_DEFAULT` palettized texture samples
> the identity palette from `initPalettizedTexture` (`device_c_resources.cpp:246`).
> A second investigation refuted it, having seen no identity values, and I
> published that refutation. Both were looking at a run that **hung before
> reaching the invocation that fails.** With the use-after-free fixed the case
> runs to completion, and instrumentation shows it plainly:
>
> ```
> DIAG upd#2 fmt=0x28 cur_pal=0
>   got=80010101,40020202,20030303,10040404
>  want=80112233,40445566,20778899,10aabbcc
> ```
>
> `0x010101, 0x020202, …` with the source alpha preserved is exactly the identity
> palette. **OPEN.**
>
> **Corrected again 2026-08-02, and this is the third correction to this entry.**
> It was reinstated as "**A8P8 only** — the P8 invocation passes with the same
> fixture", **on the strength of one instrumented run.** That is the same
> methodology error the two preceding paragraphs apologise for, committed a third
> time in the same block. Twenty runs say otherwise:
>
> | failing lane | occurrences |
> |---|---:|
> | `P8`, fixed-function | 6 |
> | `P8`, programmable | 1 |
> | `A8P8`, programmable | 2 |
>
> **Both formats fail, and both pixel-shader lanes.** That decisively kills
> "A8P8 only" — seven P8 occurrences cannot arise under it. It does **not**
> establish the reverse: `7`-vs-`2` at equal exposure is `p = 0.18` two-sided,
> consistent with equal rates, so an earlier revision of this line saying "P8
> fails more often than A8P8" was the same assert-a-rule-from-a-small-sample
> error in milder form. **No per-format rule is established in either
> direction.** The defect is **nondeterministic**: `9` of `20` runs fail (`45%`) —
> `9` lane occurrences, since one run failed on two invocations — and a failing
> invocation is always all four texels or none, always exactly the identity
> palette. It is not the commit-replay offload (reproduces with
> `DXMT9_OFFLOAD_COMMIT_REPLAY=0`), and it is not the heap-handle hazard fixed
> below — a second set after that change reads `8` of `18` (`44%`), unchanged.
> Combined `17` of `38`. Evidence:
> `tests/conformance/d3d9/evidence/p8-identity-palette-flake-20260802.json`.
>
> A ~`40-45%` rate with an all-four-or-none wrong 2×2 expansion is **the same
> signature and a statistically indistinguishable rate** compared with
> `visual_mvp_software_vp_policy`
> (`d3d9_conformance_visual_misc.c:6986-6989`, `~40%` across two independent
> sets). Treat them as one defect with two repros, not two.
>
> Corrected statement: **the RGBA expansion of a palettized `UpdateTexture`
> destination nondeterministically retains `initPalettizedTexture`'s identity
> palette instead of the current one.** The earlier "A8P8 first-use repaint miss"
> was a per-format rule read off a sample of one, and no per-format rule survives
> the data.

**FIXED 2026-08-02 — two writers, one texture, no ordering between them.**
`dxmt9c_device_update_texture` routed palettized `UpdateTexture` through
`core::Device::updateTexture`, which queues a GPU
`submitSurfaceCopy(srcSurface → dstSurface)` (`core_texture.cpp:397`). For P8 /
A8P8 **that Metal backing is not app data**: it is a derived A8R8G8B8 expansion
of (index bytes × *that texture's own* palette), written by
`expandP8SubresourceToBackend`. A SYSTEMMEM staging source is never bound through
`SetTexture`, so `applyCurrentPaletteToTexture` never reaches it and its palette
is still the identity ramp — which is why the blit carries exactly
`ff010101, ff020202, …`.

The *correct* expansion reached the same destination subresource by a **different
submission path** — `expandP8SubresourceToBackend` → `Texture::unlockRect` →
`syncLevelToBackend` → `Initializer::uploadTextureLevel`, staged and flushed as
its own command buffer at the head of `encodeChunk`. So a single `UpdateTexture`
issued **two conflicting full-subresource writes to one Metal texture on two
submission paths, with nothing *guaranteeing* the correct one last.** Note the
looser phrasing is deliberate: `encodeChunk` does encode an `encodeWaitForEvent`
on the initializer's `SharedEvent` when the initializer flushes at that chunk's
head, and in *that* interleaving the identity copy would land second every time,
which would be a `100%` failure rather than `45%`. The observed rate implies the
corrective write often rides a different flush window — a preceding chunk's
encode, or the PE-side post-flush `applyCurrentPaletteToTexture`
(`d3d9_pe_device.cpp:11319`), a third corrective writer. The fix removes the race
in every interleaving, so this does not change the decision.

The palettized branch now copies the index shadow and re-expands with the
**destination's** palette, so the expansion is the only writer — which is also
the only semantically correct source for those bytes. The non-palettized path is
untouched, and autogen-mipmap survives because `unlockRect` triggers it
(`core_texture.cpp:130`) *after* the correct expansion rather than before.

**Sufficiency was established by control, not inferred from the fix working.**
Re-adding only the queued `submitSurfaceCopy` on top of the fixed function
reproduces at `9/10`; restoring the whole legacy call, `8/10`.

| | case 155 | case 185 (`visual_mvp_software_vp_policy`) |
|---|---|---|
| before | `17/38` fail (`45%`) | `~40%` |
| after | **`0/60`** | **`0/34`** |

Case 185 had no change aimed at it and moved anyway, which settles "one defect,
two repros" — it was a hunch when this block was written and is now measured.
Non-palettized `UpdateTexture` coverage (cases 83, 117) passes.

**Not established: why the identity write won ~45% of the time.** Instrumented
event streams from passing and failing runs are byte-identical through dxmt9's
encode layer, Metal texture identities included, so the winner is settled below
it. Bounded, though: intra-blit-encoder ordering is reliable here — post-fix the
initializer still issues three same-subresource writes back-to-back in one
encoder and the last wins in `40/40`. The unordered relationship was between the
chunk command buffer's SurfaceCopy and the initializer's command buffer. The fix
removes the race rather than ordering it, so this is a known unknown rather than
residual risk on this path.

> **The native spec that was supposed to cover this has never run.**
> `testProgrammablePalettizedTextureDrawSmoke`
> (`tests/native/core/core_device_coverage_spec.cpp`) returns early unless
> `DXMT9_CORE_SPEC_METAL_INTEGRATION` is set, and **nothing in this repository
> sets it** — not `meson.build`, not CI, not any script. It returned *silently*,
> which reads exactly like coverage, and the regression shipped under it. It now
> announces the skip, and its hand-rolled imitation of the old PE sequence — under
> a comment claiming to "match the PE path ordering", i.e. describing the buggy
> shape as the specification — has been replaced by a call to the production
> bridge function with a **null `iface`**, so reintroducing the routing faults
> instead of degrading to a flake only conformance can see. Verified to fire:
> re-adding the routing makes it `SIGSEGV`, and it passes with the fix.

**The same shape in `UpdateSurface` — FIXED 2026-08-02, and it was worse than
predicted.** `dxmt9c_device_update_surface` → `Device::updateSurface`
(`core_surface.cpp:338`) moves the source's derived expansion **twice**: a queued
`submitSurfaceCopy` *and* a CPU `lockRect`/`memcpy`. `D9CSurface` carries no p8
shadow, so unlike the `UpdateTexture` case nothing re-expanded the destination
behind it. Both consequences measured with the destination bound and sampled:

| | P8 | A8P8 |
|---|---|---|
| first draw | `ff010101 ff020202 ff030303 ff040404` | `80010101 40020202 20030303 10040404` |
| after a palette switch | `ff000000` ×4 | `00000000` ×4 |
| expected | `ff112233 …` → `ff224466 …` | `80112233 …` → `80224466 …` |

The first row is the identity ramp, as predicted — deterministic here rather than
a race, since no corrective writer competes. **The second row was not predicted
and is the worse half:** the destination *texture*'s `p8Levels` index shadow was
never written, so the next `SetPaletteEntries` / `SetCurrentTexturePalette`
re-expanded from a still-zero shadow and every texel collapsed to `palette[0]`.
The copy is not mis-coloured, it is destroyed.

Fixed by `dxmt9c_copy_palettized_subresource` (`device_c_resources.cpp:336`),
reached through `D9CSurface::ownerTex` / `ownerLevel` — the `UpdateTexture` fix
one level down. A palettized pair never falls through to the routed path; an
unservable one is `D3DERR_INVALIDCALL` rather than an excuse to run the path
being fixed.

**Coverage was written first, and made to fail first.** There was none, which is
why this was flagged rather than fixed blind on 2026-08-02 morning. New case
`visual_p8_update_surface_policy` (index 156) fails `32` of `213` checks
deterministically on the unfixed routing — reproduced `3/3` by the supervising
session with an identical count each time — and passes with the fix. Provenance
is `dxmt9-policy`, not an oracle: **no Wine test combines a palettized format
with `UpdateSurface`** — every upstream palettized test copies with
`UpdateTexture`, and every upstream `UpdateSurface` test uses an unpalettized
format. Pinned ungated in `core_device_com_spec.cpp` on three separate
assertions, because any one of them passes while the bug is live: no surface copy
queued, expansion used the destination's palette, and the destination's index
shadow was written. Verified to discriminate.

> **The first version of this fix introduced a device wedge, caught in review.**
> It returned `D3DERR_INVALIDCALL` for an unservable palettized pair, reasoning
> that a mismatched `P8`/`A8P8` pair is "already rejected by PE-side validation,
> so unreachable from a real app". **Both halves were wrong.** PE's format check
> (`d3d9_pe_device.cpp:11208`) compares `Surface::GetDesc` formats, and
> `dxmt9c_surface_get_desc` reports the **core backing** format — `A8R8G8B8` for
> a level of *either* a P8 or an A8P8 texture, since both back onto the
> expansion — so `21 == 21` and the pair sails through.
>
> And reaching it is far worse than an `INVALIDCALL`, because `UpdateSurface` is
> a **fire-and-forget chunk record** (`D9C_COMMAND_RECORD_UPDATE_SURFACE`)
> replayed asynchronously. A failed record never becomes an HRESULT the app
> sees: it is `commitChunkFail`, which on the engine-default offload lane
> fail-stops the worker and **poisons every later commit**. That converts a call
> real D3D9 merely rejects into a process-lifetime wedge — and D3D9-era apps do
> probe by calling and checking the HRESULT. An unservable pair now falls
> through to the routed path, which is exactly the pre-fix behaviour for it
> (wrong pixels, app survives), so the change is strictly no worse than before
> for every input.
>
> The generalisable error: **"unreachable" was asserted from a validation check
> whose inputs I never read.**

**The D3D9 parity break underneath all of this — FIXED 2026-08-02.**
`dxmt9c_surface_get_desc` reported `fmtToD3D(core format)`, so `GetSurfaceLevel`
on a `P8` texture described itself as `D3DFMT_A8R8G8B8` — the *derived
expansion* — where real D3D9 says `D3DFMT_P8`. Beyond being wrong for apps, it is
what made PE's `UpdateSurface` format check unable to separate `P8` from `A8P8`.

**Fixing the getter alone would have been worse than the bug**, and a negative
control proved it rather than an argument. `IDirect3DSurface9::LockRect` on a
palettized level went to the core surface — the expansion, 4 bytes per texel —
while `IDirect3DTexture9::LockRect` on the *same subresource* yields the 1-byte
index shadow. Real D3D9 gives index bytes either way. So a getter-only fix leaves
an app that does the ordinary thing — `GetDesc` for the format, then `LockRect`
and walk rows by `Pitch` — holding two coherent-looking answers that disagree
about texel size. Today's behaviour is at least a *self-consistent* lie. The pin
fails that variant with `P8 surface lock pitch is one byte per texel`.

So the fix is both halves: report `ownerTex->d3dFormat`, **and** route
`Surface::LockRect` / `UnlockRect` through the texture path, which already owns
`lockedLevels` and re-expands on unlock. Surface and texture interfaces to one
subresource now agree on format, bytes, pitch, lock state, and expansion refresh.

That also closes the adjacent hole the `UpdateSurface` fix left open: PE's check
now compares `41` vs `40` for a `P8`↔`A8P8` pair and `41` vs `21` for
`P8`↔plain-`A8R8G8B8`, rejecting both before the routed copy path — where before
they passed as `21 == 21` and copied where real D3D9 rejects.

New case `visual_p8_surface_level_contract_policy` (index 156, `dxmt9-policy` —
Wine `6e073d28` has no oracle pairing a palettized `GetSurfaceLevel` with
`GetDesc` or surface `LockRect`) fails `19` assertions without the provider
change, first at `desc.Format == format`, and passes with it.

> **Implemented by `codex` (`gpt-5.6-sol`, reasoning high) under a sandbox where
> `wineserver` could not bind.** It therefore produced **no** Wine runtime
> evidence and said so plainly rather than reporting the runs it could not make;
> its `meson test` figure (`210 Ok / 463 Fail`) was sandbox artifacts — GPU
> shader-render cases returning black, and `verify_tla` unable to fetch
> `tla2tools`. Every runtime number above was produced outside that sandbox by
> the supervising session, which also reverted two unrelated edits it found in
> the tree adding links to `docs/research/metal-render-pass-lifecycle.md`, a file
> that does not exist.

**Two adjacent gaps deliberately left open**, so neither is fixed under cover of
this one: `srcRect` / `dstPoint` are still ignored by
`dxmt9c_device_update_surface` on *both* paths (a partial-rect palettized copy
now moves full-surface *correct* indices where it previously moved full-surface
*wrong* colours — worse to pin than to leave), and standalone
`CreateOffscreenPlainSurface(D3DFMT_P8)` surfaces, which have nowhere to store a
palette, keep the old path.

> **Corrected after review — the earlier description of that second gap was
> wrong**, and it was flagged at the time as a code-read inference with no test
> behind it, which is exactly how it went wrong. It claimed a standalone pair is
> rejected with `D3DERR_NOTAVAILABLE` where real D3D9 returns `D3D_OK`. Measured:
> a **pure** standalone pair is `Format::P8` on *both* sides, so there is no
> mismatch, the copy proceeds over raw 1-byte texels, and it already returns
> **`D3D_OK`** — the defect does not exist. The pair that does hit core's
> `NOTAVAILABLE` is the **mixed** one (standalone `P8` ↔ a `P8` *texture* level,
> core `P8` vs `A8R8G8B8`), and from a real app that never reaches core at all:
> PE `GetDesc` reports `41` vs `21` and rejects it with `INVALIDCALL` — still not
> D3D9's `D3D_OK`, but a different wrong answer than was recorded here.

**The hang is a real use-after-free, and it is not P8-specific — OPEN.**
`StagingCopy` (`dxmt9_resource_pool.hpp:618-621`) retains its `stagingTexture`
as a `WMT::Reference` but holds `destTexture` as a **bare, unretained handle**.
Staging an upload never bumps the destination's `lastUsedSeqId`, so when the
texture is released, `gcArena`'s gate

```cpp
DXMT_ASSERT(record.lastUsedSeqId <= completedSeqId);   // dxmt9_resource_pool.cpp:172
```

is trivially satisfied and the Metal texture is freed **while
`Initializer::pendingUploads_` still references it**.
`Initializer::flushToWaitUnlocked` (`dxmt9_resource_initializer.mm:373-377`)
then blits into freed memory. The gate's own comment scopes its safety argument
to encoders — *"no in-flight encoder can still be dereferencing this record's
pointer"* — which is exactly the hole: pending initializer uploads are not
encoders and are not covered by R-VERIF-3.1's C++ realisation.

The crash is unrecoverable rather than fatal: AGX faults on `dxmt9-encode`, a
native pthread created inside `winemetal.so` with no Wine TEB, so Wine's ntdll
handler dereferences a NULL `NtCurrentTeb()` and faults again. The thread wedges,
and the app thread waits forever in
`commit_chunk -> getRenderTargetData -> submitFlush -> waitForSequence`.

**This also explains the "4 vs 8" instability**, which was never failure
flakiness: the assertions failed deterministically per invocation and the count
was simply how many invocations completed before the crash landed.

This test is only the repro, not the cause: its teardown
`SetCurrentTexturePalette(device, 0)` stages an upload for a bound texture
immediately before release, and a preceding `GetRenderTargetData` has just
forced `completedSeqId` current, so the reclaim fires at once.

**FIXED 2026-08-02** by retaining the destination: `StagingCopy::destTexture` is
now `WMT::Reference<WMT::Texture>`, symmetric with `stagingTexture`, so a pending
upload holds the Metal texture alive across the staging→flush window regardless
of when the record is reclaimed. Bumping `lastUsedSeqId` on staging was the
alternative and was rejected: the initializer completes against its own
`SharedEvent` value, not a queue seq id, so the two are different domains.

> **Corrected 2026-08-02: "R-VERIF-3.1's model is unchanged, `verify_tla.sh`
> re-run clean" was true and worthless, and stating it implied assurance it did
> not carry.** `ResourceLifetime.tla` models in-flight GPU work *only* as
> seq-id-marked chunks, so a pending initializer upload — a GPU command
> referencing the texture — is outside the model entirely. That is precisely why
> `NoUseAfterFree` was green while this use-after-free was live, and it is green
> after the fix for the same reason. The fix satisfies R-VERIF-3.1 through
> refcounting, **a mechanism the model does not represent**. Recorded as a known
> incompleteness in `specs/verification/gap.md`. A model that cannot see the bug
> class cannot certify the fix, and re-running it is not evidence.

**A second, lower hazard one level up, also fixed.** `StagingCopy::destHeap` was
a bare `obj_handle_t`, and `HeapManager::retireFreedHeaps` erases an `Instance` —
releasing the pool's sole `Reference<Heap>` — as soon as `liveMembers == 0 &&
lastUsedSeqId <= completedSeqId`, neither of which a pending upload satisfies.
The flush's `blit.useHeap(destHeap)` would then pass a handle the pool no longer
owns. It survived only because a heap-placed `MTLTexture` retains its `MTLHeap`
internally, so the retained `destTexture` transitively pinned it — an
undocumented Apple implementation detail load-bearing for a correctness argument,
with nothing at the `useHeap` walk saying so. `destHeap` is now a
`WMT::Reference<WMT::Heap>`, symmetric with `destTexture`, at the cost of one
retain/release on the cold texture-initialization path. **No behavioural change
measured, and none claimed**: `18` runs of case 155 after it read the same flake
rate as the `20` before (`44%` vs `45%`).

**Measured effect:** case 155 stopped hanging and now completes `1,084` checks
with a verdict instead of timing out at 300 s. Separately, a fault on a
winemetal-owned pthread being unrecoverable under Wine turns any such bug into a
silent 300 s hang, which is worth a guard independent of this defect.

**Not yet re-measured:** whether the retain also affects the ~40% flaky wrong
expansion in `visual_mvp_software_vp_policy`. Its rate was established on builds
where this use-after-free was live. The case-155 defect above is now measured at
`45%` with the same all-four-texels-or-none wrong-2×2-expansion signature, which
makes "same defect, two repros" the leading reading rather than a hunch — so the
useful re-measurement is of both cases together, against the identity-palette
mechanism, not of the retain.




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

**~~Still open — a real dxmt9 mip-LOD bug, and it is not shader translation.~~
SUPERSEDED — every claim in this paragraph was resolved later the same day; it is
kept only as the record of what was believed at this point.** The surviving 20
failures were `visual_formats.c:638-641` (16: the four `lod_texels` invocations x
four texels) and `:796-799` (4: the update-texture lane). They failed identically
on the fixed-function and programmable paths, returning level-0 texels where
level-1 was expected. The suspicion recorded here — that only level 0 is
expanded, or that the expanded levels are not linked as mips — was **wrong**, and
so was reading the two groups as one bug. They were three unrelated causes:

| then | actually |
|---|---|
| `:638-641`, "a mip-LOD bug in the palettized expansion" | a **sampler** bug, nothing to do with palettes: `SetLOD` was discarded under `MIPFILTER=NONE`, because `D3DTEXF_NONE` mapped to `MTLSamplerMipFilterNotMipmapped`, in which Metal ignores `lodMinClamp` — fixed above |
| `:796-799`, same bug as `:638-641` | two separate things: a **test** bug (slot 0's palette contents clobbered by earlier invocations) plus the palettized-`UpdateTexture` **SurfaceCopy race** — both fixed above |
| "also still hangs past 300 s, uninvestigated" | a **use-after-free** (`StagingCopy::destTexture` unretained) — fixed above |

Four causes behind what this paragraph called one, which is why the shared
symptom was a bad grouping heuristic.

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
