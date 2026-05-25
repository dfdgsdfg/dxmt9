# Spec–Implementation Gap

Current state of the codebase vs. the specifications.
Updated against current working tree (2026-05-24, HEAD `4a3aaa0`).

Two companion per-item inventories feed this tracker and hold the
authoritative detail behind the D3D9 and Tests rows below:

- [`specs/gap_d3d9.md`](gap_d3d9.md) — per-spec-item D3D9 API coverage
  (shader opcodes, render / texture-stage / sampler / transform state,
  formats, caps, ~225 COM vtbl slots; ~803 rows). Its concrete
  implementation gaps are rolled up under
  [D3D9 API Coverage Inventory](#d3d9-api-coverage-inventory) below.
- [`specs/gap_d3d9_wine_test.md`](gap_d3d9_wine_test.md) — every Wine
  `d3d9/tests/{visual,device,d3d9ex,stateblock}.c` `START_TEST` entry
  (268 total) mapped to its dxmt9 PE conformance evidence.

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
| Hot-path CommandChunk recording and data normalizers | ⚠️ | Chunk records, delta draw packets, `APPLY_STATE`, bulk retention, and draw-run paths exist. `makeCanonicalDrawStateFromState()` / draw-run helpers cover production state-to-flat-draw transforms, while `makeDrawDescFromState()` is fixture/offline coverage only. **Module structure (Round-2/3 splits, 2026-05-08/09):** `device_c_record_utils` split into `_validate.cpp` (612) + `_replay.cpp` (264) + `_hazard.cpp` (406) (T1 `aef2b0a`); `device_c_device_state_draw.cpp` split into `_state.cpp` + `_draw.cpp` (Round-2 `96770cd`); `device_c_common.cpp` split into `_marshal.cpp` + `_shader_dump.cpp` + `_format_utils.cpp` (Round-2 `140493b`); `device_c_draw.cpp` chunk-replay extracted to `device_c_chunk_replay.cpp` (T6 `0ec573f`). All splits cover the same code paths with native tests (`chunk_record_validation_spec`, `chunk_record_hazard_spec`, `chunk_record_replay_spec`, `chunk_record_import_spec`). Remaining alignment is auditing barrier/hazard behaviour with a fake backend or queue instrumentation against R-CORE-11.14-R-CORE-11.18 |
| DOD / DXMT ownership acceptance (`R-ARCH-1.*`, `R-ARCH-2.*`, `R-ARCH-5.*`) | ⚠️ | `specs/archicture/` now owns the whole-project architecture contract and the chunk wire path is data-oriented. Merge-readiness still needs explicit acceptance that PE state shadow, POD chunk construction, unix import, queue execution, presentation pacing, and deferred resource safety each have matching implementation owners and tests |
| DrawPrimitive, DrawIndexedPrimitive, UP variants | ✅ | All four variants |
| TriangleFan decomposition | ✅ | `decomposeTriangleFanIndices()` |
| Half-pixel offset | ✅ | `halfPixelFixup()` |
| FfpVertexKey / FfpPixelKey generation | ✅ | `makeFfpVertexKey()`, `makeFfpPixelKey()` |
| Shader bytecode storage + hash | ✅ | `ShaderBytecode`, FNV-1a hash cache |
| Format table | ⚠️ | `formatTable()` exists; central explicit classification for all FOURCC/pseudo-formats such as `RESZ` and `NULL` still needs audit |
| Vendor FOURCC / dependent-texture / mipmap upload policy | ⚠️ | scaffolded via 7 PE conformance entries (`tests/conformance/d3d9/d3d9_conformance_vendor_policy.c`) — explicit unsupported HRESULT documented for INTZ/FETCH4/RESZ; TEXBEM/TEXDEPTH/test_mipmap_upload deferred to NATIVE/SR follow-up; `test_miptree_layout_lock_pitch_policy` captures the public-ABI LockRect pitch + GetLevelDesc round-trip (the value-level math is in `tests/native/core/core_d3d9_miptree_layout_spec.cpp`). Policy decisions are now PE-captured even though runtime evidence remains deferred. |
| Shader-runner fixtures: SR-blocked `visual.c` entries | ⚠️ | Each entry now has a narrow PE scaffold in `tests/conformance/d3d9/d3d9_conformance_sr_blocked_policy.c` capturing the public-ABI portion (`bumpenvmap_tss_policy`, `pretransformed_vertex_declaration_policy`, `vface_pixel_shader_create_policy`, `fp_special_caps_policy`). The full rasterization aspect still requires SR runner DSL / oracle infrastructure (TEXBEM runner support, XYZRHW pretransformed-varying fixture, VFACE input fixture, NaN/Inf tolerance policy) and remains deferred. |
| `makeDefaultCaps()` | ✅ | R-CAPS-1 through R-CAPS-7 |
| Buffer / Texture / Surface lifecycle | ⚠️ | Pool-based reset behavior exists. **T4 (2026-05-08)**: D3D9Ex SYSTEMMEM 1-mip 2D texture and `CreateOffscreenPlainSurface` w/ `pSharedHandle` now alias caller memory (`LockRect` returns user pointer with computed pitch); VB/IB SYSTEMMEM + handle returns `D3DERR_NOTAVAILABLE`; SCRATCH + handle returns `D3DERR_INVALIDCALL`; cube/volume + SYSTEMMEM + handle returns `D3DERR_INVALIDCALL`. DEFAULT-pool shared handles still `E_NOTIMPL` (deferred — needs IOSurface/MTLSharedTexture bridge). Still needed: public COM refcount/lifetime, common private-data semantics, full `test_user_memory` matrix including `CreateOffscreenPlainSurfaceEx`. |
| `UpdateSurface`, `UpdateTexture`, `StretchRect`, `ColorFill`, `GetRenderTargetData` | ✅ | Core-side logic |
| `DXMT9_LAYER_FRAMEBUFFER_ONLY` × backbuffer `Lock` / `GetRenderTargetData` semantics (R-CORE-WSI-6.1, R-CORE-WSI-6.2) | ⚠️ | Spec landed 2026-05-22 in `specs/d3d9/wsi/requirements.md` §6, classification **D** (no `framebufferOnly` branch on the D3D9 Lock / readback paths — the toggle is a pure present-side optimisation; the dxmt9 backbuffer `MTLTexture` is allocated independently from the `CAMetalLayer` drawable). Code inspection: `src/d3d9/d3d9_pe_device_child_surface.cpp:397` (`LockRect`), `src/d3d9/d3d9_pe_device.cpp:2426` (`GetRenderTargetData`), `src/d3d9/core_surface.cpp:354` (`Device::getRenderTargetData`). Missing evidence: Wine-oracle PE conformance case asserting that `Lock` + `GetRenderTargetData` on the swap-chain backbuffer succeed and return the rendered pixels with `DXMT9_LAYER_FRAMEBUFFER_ONLY=1` set; today the toggle is exercised only in performance probes (SFIV), not correctness probes. |
| Query: EVENT, OCCLUSION, TIMESTAMP, TIMESTAMPFREQ, TIMESTAMPDISJOINT | ⚠️ | `submittedSeqId_` / `completedSeqId_` waterline exists. **PE validation landed 2026-05-24 (`bfaf1fb`):** `CreateQuery` unsupported-type gate + per-type `GetDataSize` (EVENT=4, OCCLUSION=4, TIMESTAMP=8, TIMESTAMPDISJOINT=4, TIMESTAMPFREQ=8); native-verified (`dxmt9-core-d3d9-device-validation-spec`). Remaining: Windows-visible zero-initialisation behaviour + Wine-conformance-run breadth. |
| Clip planes | ✅ | `transformClipPlane()` (inverse-transpose WVP) + `FfpVsConsts.clipPlanes` + single-slot `[[clip_distance]]` emitter. **2026-05-24 (`c0749c0`):** Apple Metal on Apple7+ honours only ONE `[[clip_distance]]` declaration per VS output (array form and multiple-scalar forms both silently clip every fragment regardless of value); the FFP and metal_ir vs_3_0 emitters now collapse D3D9's six clip planes into one slot via `min(dot(plane_i, position))` over the runtime mask — equivalent to "any plane discards" (`min < 0 ⇔ any d_i < 0`). Limitation: per-plane scissoring through geometry cannot be exposed (Metal cannot pass individual d_i to the rasterizer), but every D3D9 contract that depends on the half-space test passes. **Regression coverage:** `tests/shader_runner/corpus/render_state/dxmt9_clip_plane_halfspace_readback.shader_test` (plane (0,1,0,0) ⇒ top half renders, bottom is clipped) plus `dxmt9_clip_plane_runtime_readback.shader_test` (plane (0,0,0,-1) ⇒ everything clipped). |
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

**Wine-oracle test mapping is now complete.** All 268 Wine
`{visual,device,d3d9ex,stateblock}.c` tests are mapped to dxmt9 PE
conformance evidence (`gap_d3d9_wine_test.md`, 268/268 covered as of
HEAD `4a3aaa0`). The per-row "still need Wine-oracle conformance
coverage" notes above therefore now mean *lane / arch breadth promotion*
(app-local + x86 lanes; the builtin x64 lane is largely passing) rather
than a missing scaffold — see the Tests Layer rows. Separately, the
per-spec-item audit in `gap_d3d9.md` (2026-05-23) surfaced concrete
implementation gaps that were *not* lane-breadth issues; **those were all
closed in the 2026-05-24 implementation pass** — `gap_d3d9.md`'s
"Remaining actionable gaps" is now **None**. See the next subsection.

### D3D9 API Coverage Inventory

Per-item inventory lives in [`specs/gap_d3d9.md`](gap_d3d9.md) (a regenerable
read-only ripgrep tracker, not a spec). The original **2026-05-23** four-agent
audit baseline rolled up as (current status follows the table):

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

**Current status (2026-05-24): `gap_d3d9.md` "Remaining actionable gaps" =
None.** The 2026-05-24 re-audit found ~13 audited gaps were already implemented
on `master`, and the implementation pass closed the rest. The high-priority
findings the audit called out are all resolved (see `gap_d3d9.md`'s Closed
table for per-item commits + gate test targets):

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

Outstanding work here is **not implementation** but **deferred evidence**:
GPU-runtime pixel validation (RESZ MSAA→INTZ readback, NULL color-attachment
omission, MIPMAPLODBIAS mip selection, tile-FFP↔portable equality) and
conformance Wine-run validation of the new PE gates.

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
- **tile-FFP ✅ FIXED (two-stage encode landed 2026-05-25) — default stays `off` pending a perf benchmark** (`DXMT9_TILE_FFP` toggle landed `e165154`): the toggle works (force→tile, off→portable, proven by `tile_ffp_pass_count`/`portable_ffp_pass_count`), and the A/B it enabled uncovered that **the tile-FFP path never rasterizes the draw's base colour into the imageblock** — so any draw routed to tile renders the *cleared* imageblock post-processed by the tile kernel, i.e. **black**, regardless of fog/alpha-test. Evidence (M1/apple7): a plain white quad with NO fog and NO alpha-test reads `(255,255,255)` portable but `(0,0,0)` tile; the tile run shows `draw_geometry_samples=0` / `bind_pipeline=0` / no draw counters while `tile_ffp_pass_count=1`, vs portable `draw_geometry_ffp=1` / `draw_vertices=3`. **Root cause:** `src/dxmt9/dxmt9_draw_encoder.mm:~1435-1459` (wired by `523b66e`) does `setTileRenderPipelineState` + `dispatchThreadsPerTile` **instead of** the base-colour `setRenderPipelineState` + `drawPrimitives`; the tile kernel `makeFfpTilePixelSource` (`dxmt9_ffp_shaders.cpp:~1302` `float4 color = float4(slot->color);`) therefore reads the cleared imageblock, not the fragment colour. This contradicts the programmable-blend design (`specs/backend/design.md §13.5`, R-BACK-13.1) where the fragment stage must write the base colour FIRST and the tile kernel only applies fog/alpha-test/A2C over it. **Impact:** tile-FFP is default-on for eligible draws on Apple Silicon (the `selectTileFfpForPass` eligibility: FFP + fog/alpha-test/A2C, non-textured), so a real app using non-textured FFP fog/alpha-test draws renders that geometry **black**. (Narrowly triggered — most FFP draws are textured → forced portable → correct — which is why it went unnoticed; SFIV etc. are shader-based, not FFP.) **Proper fix = two-stage tile encode:** bind a base-colour render PSO (portable FFP fragment, fog+alpha-test stripped) and issue `drawPrimitives` so the geometry lands in the imageblock, THEN bind the tile PSO + `dispatchThreadsPerTile` to apply fog/alpha-test/A2C — within one render encoder; spans variant key + a base-colour pixel-source variant + pipeline cache (two PSOs) + encoder reorder + GPU bit-identity validation. **Interim safety APPLIED (2026-05-25):** `DXMT9_TILE_FFP` default flipped `auto`→`off` (`tileFfpModeOverride` in `dxmt9_pipeline_cache.cpp`) so every FFP draw now takes the correct portable lane by default; verified an eligible FFP draw records `portable_ffp_pass_count=1` (tile=0) with no env, while `DXMT9_TILE_FFP=auto` still reaches the tile path (`tile_ffp_pass_count=1`) for the two-stage-encode fix to validate behind the flag. Portable is correct and was the original path; the only loss is the (unvalidated) tile perf optimisation. The default flips back to `auto` once the two-stage encode lands + GPU-readback equality holds. **Repro** (`.shader_test`, kept here since the corpus file was removed to keep `dxmt9-manifest-check` green): `clear rgba(0,0,0,1); dxmt9-render-state fog=on fogmode=linear fogstart=0 fogend=1 fogcolor=rgba(0.25,0.5,0.75,1); alpha-test enable greaterequal 0.5; draw quad; probe (32,32)` — run under `DXMT9_PREWARM=disabled DXMT9_TILE_FFP=force` (→ buggy `(0,0,0)`) vs `=off` (→ correct `(64,128,191)`), both with `DXMT_PERF_COUNTERS=1` to confirm the paths differ. **RESOLVED 2026-05-25 (6 commits `43ae1dd`..`7ed5fcb`, supervised mega-refactor):** implemented the two-stage encode (base-colour render PSO + `drawPrimitives` → then tile PSO + `dispatchThreadsPerTile`, one encoder). The fix exposed TWO further root causes beyond the missing base draw: (1) the tile kernel's imageblock access used `imageblock_data.data(tid)` (explicit-layout accessor) which **never compiled** → changed to `read(tid)`/`write(value,tid)` (implicit layout); (2) `setTileRenderPipelineState` is **not a valid selector on the M1 render encoder** (`AGXG13GFamilyRenderContext`, threw `NSInvalidArgumentException`) → the tile PSO is bound via `setRenderPipelineState` per §13.5, with `FfpPsConsts` on `setTileBuffer(3)`. Also runtime-gated the tile fog blend on `ffpPs.fogMode != 0`. **Verified (M1, supervisor re-ran):** the registered probe `tests/shader_runner/corpus/ffp/dxmt9_tile_ffp_fog_equality_readback.shader_test` PASSES under BOTH `=force` and `=off` (both `(64,128,191)`) with `draw_geometry_samples=1` (was 0); manifest-check ok (243 tests); `dxmt9-tile-ffp-selector-spec`/`-msl-spec` OK; FFP corpus no-regression. The `off` default is retained because the perf case is still unproven — single-draw `gpu_command_buffer_time_ms` was tile 0.039 ms vs portable 0.050 ms (noisy, not a workload benchmark); flipping the default back to `auto` should await a real benchmark proving tile beats portable.

`DXMT9_PREWARM=disabled` + isolated `DXMT9_CACHE_DIR` were used per run. The dxmt9
implementations themselves are unchanged and presumed correct — but **unverified on
GPU**; the argbuf precedent (a "landed" lane that was a GPU page fault) is the reason
these should not be assumed correct until a probe (via one of the vehicles above)
actually reads back the pixels.

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
| Submission grain: 1 chunk → N MTLCommandBuffer chain (`R-BACK-2.29`–`R-BACK-2.34`) | ⚠️ | **Production default is `per-render-pass + cap=4`** per R-BACK-2.34 — `dxmt9_draw_encoder.mm:267` returns `MidChunkCommitPolicy::PerRenderPass` for the no-env case, matching the spec text in `specs/backend/requirements.md`. The earlier "spec drift flagged" note in this row was itself based on a misread of `midChunkCommitPolicy()` and has been retracted; the architecture-comparison.md "Y1 default 1:1-to-4 cap=4" claim is correct. **Implementation history:** S2 (`encodeChunk` mid-chunk split via `splitMidChunk` lambda mirroring `splitBeforeBlockingPresent`, env-driven `DXMT9_MID_CHUNK_COMMIT_POLICY=off\|per-render-pass\|per-n-records`, originally default `off`). U1 (R-BACK-2.33 chain length cap via `splitMidChunkUnderCap` wrapper + `DXMT9_MID_CHUNK_COMMIT_CAP_PER_RENDER_PASS=4` default; cap=0 disables for diagnostic A/B). Y1 (R-BACK-2.34, commit `539b515`) flipped the no-env default from `off` → `per-render-pass`; `policy=off` remains reachable as the legacy 1-CB-per-chunk opt-out. S1 TLA: `CommandQueue.tla` extended with sub-CB chain (39,638 states pass). Counters `sub_command_buffers` + `chunk_subcb_count_max` in table + per-frame snapshot. **CC1 (2026-05-10):** earlier docs misclaimed dxmt9 borrowed DXMT's "submission slot chain"; verified DXMT actually uses strict 1 chunk = 1 CB. R-BACK-2.34 is a dxmt9-original divergence, more aggressive than DXMT. **BB1 SFIV +49% fps (13.25 → 19.77)** measured on synthetic SFIV fixture — this provided the chain-rich evidence U1 was missing and motivated the Y1 default flip. **S3 menu A/B (light scene):** `command_buffers / frame` 1 → 2 with `present_acquire_wait_ms` p99 **−93%**. **U1 heavy-scene A/B (RP≥10, ≈1500 frames):** `command_buffers` plateaus at 4 (cap holds), `gpu_command_buffer_time_ms` p99 **−44%** (290 → 163 ms — pipelining win measured) but fps unchanged within noise (11.89 → 11.24) because 4× tile-flush + commit overhead absorbs the gain on that profile (≈2.1 ms / frame per `docs/research/g-axis-tuning.md`); the cap from R-BACK-2.33 bounds that worst case. **Still missing:** wild-title fps measurement (BB1 SFIV is synthetic only); `per-n-records` split path implemented but not exercised by probes; R-BACK-2.32 formal evidence. |
| Render-pass load/store action policy (`R-BACK-15.1`–`R-BACK-15.16`) | ⚠️ | `specs/backend/render-pass-actions/` (commit `ebd7bad`) adopted as the contract. **Landed (G+H batches, 2026-05-08/09):** `dxmt9_perf_counters` now emits `render_pass_load_action_*` / `_store_action_*` / `tile_preservation_bytes` (G1 `c3bd28b`, G3 `4b124aa`); `tests/native/backend/render_pass_actions_spec.cpp` filled with 8-case matrix (G2 `6a0ade4`, G4 `2da64ca`); `CommandQueue::touchedColorHandles_` set + API (H2 `4d77934`); depth look-ahead end-of-chunk + texture-sample (H1 `ba2ff2f`); color first-use DontCare-load + touched mark/invalidate (H3 `c9ba806`, R-BACK-15.4/5); B+C integration tests (H4 `a7a2aee`, R-BACK-15.4-7). **Still needed:** `R-BACK-15.6` cross-frame correctness for queue-local retention, `R-BACK-15.7`/`15.8` live-out depth-stencil DontCare-store, performance contract validation (≥30% Load reduction, ≥50% depth Store reduction, `completion_present_wait_ms` ≥20% on SFIV). |
| Argument-buffer hybrid Stage 2 (`R-BACK-12.22`–`R-BACK-12.26`) | ✅ | Stage 1 ✅; constants-only Stage 2 default-on + readback-validated (the 2026-05-10 regression is resolved — see end). **winemetal bridge surface landed 2026-05-10 (`182fc32`):** `WMTArgumentBuffersTier`, `WMTArgumentDescriptor`, `MTLDevice_argumentBuffersSupport`, `MTLDevice_newArgumentEncoder`, `MTLArgumentEncoder_*` (encodedLength/alignment/setArgumentBuffer/setBuffer/setTexture/setSamplerState), `MTL{Buffer,Texture,SamplerState}_gpuResourceID` for Tier-2 MTLResourceID encoding (11 funcs). **dxmt9-runtime adopter landed 2026-05-10:** capability gate (`Pool::argbufHybridEnabled_` cached once at `CommandQueue` init from `device.argumentBuffersSupport() >= Tier2 && supportsApple3()`); per-pass `selectArgbufHybridForPass` selector (Stage1 fallback when gate fails, no mid-pass switching); `ShaderVariantKey::argbufHybridMode` bit (Stage 1/Stage 2 PSOs hash to distinct cache entries, parallel to `tileFfpMode`); MSL prelude variant `makeShaderPreludeArgbufHybrid()` declaring `struct ArgbufLayout { constant Vs/Ps/FfpVs/FfpPs* + texture2d[8] + sampler[8] }` at `[[id(0..19)]]` for slot 30 binding; new `dxmt9_argbuf_hybrid` module owning `buildArgumentDescriptors()` (20-entry `WMTArgumentDescriptor` table for `MTLDevice::newArgumentEncoder`); counters `argbuf_hybrid_encoder_count` / `stage1_encoder_count` / `argbuf_hybrid_fallback_count` / `argbuf_hybrid_bytes_per_encoder` / `stage1_bytes_per_encoder` all wired in `startRenderPass`; native test `dxmt9-argbuf-hybrid-spec` covers capability gate, selector decision shape, variant-key bit independence from `tileFfpMode`, descriptor layout (4 + 8 + 8 entries at id 0..19), MSL prelude variant text. **MSL routing landed 2026-05-10 (`b9cfffb`):** `ShaderSourceContext::argbufHybridMode` flag propagated via `Cache::getOrBuildDrawPipelineForState` → FFP emitters (`makeFfpVertexSource` / `makeFfpPixelSource` / `makeFfpTilePixelSource`) and DXBC→MSL translator (`dxmt9_shader_metal_ir.cpp`) emit single `[[buffer(30)]] ArgbufLayout const* abuf` parameter + alias preamble (`constant VsConsts& vsConsts = *abuf->vsConsts;`, `texture2d<float> tex0 = abuf->textures[0];`, …) when bit is set; body MSL surface unchanged. Native test `dxmt9-argbuf-hybrid-msl-spec` (12 cases) covers Stage1/Stage2 emit symmetry + variant-key bit independence from `tileFfpMode`. **Per-encoder argbuf populator landed 2026-05-10 (`5aa7081`):** `dxmt9_argbuf_hybrid` extended with `ArgbufEncoderResource` (lazy-init on `CommandQueue` from `device.newArgumentEncoder(buildArgumentDescriptors(), 20)`), `PopulatedArgbuf`, `openArgbuf` (reserves storage via `CommandQueue::reserveTransientBuffer` keyed on chunk `seqId`), `populateConstantBuffers` / `populateResourceBindings` / `updateDirtyArgbufRegions`. `startRenderPass` opens argbuf in Stage 2 path, populates resource MTLResourceIDs, binds at `kArgbufHybridBindSlot=30` for vertex+fragment, bumps `argbuf_hybrid_bytes_per_encoder`. `encodeDraw` per-draw rewrites dirty cbuf entries via `updateDirtyArgbufRegions`. Native test `dxmt9-argbuf-populator-spec` covers ArgbufEncoderResource + `dirtyBytesEstimate` + PopulatedArgbuf default. **Stage 1 shadow drop landed 2026-05-10 (`ceb9583`)** ahead of `R-BACK-12.26` validation: encoder no longer issues slot 0/3 binds when `argbufHybridMode=true`; cbuf dirty bits cleared inside the argbuf populator branch; `bindFfpVsIfDirty` routes through new `pointFfpVsAtSlice` helper (argbuf [[id(1)]]) when in Stage 2 to handle FFP `preTransformed` viewport override. **✅ Regression RESOLVED (verified 2026-05-25):** the five formerly-failing shader-corpus tests (viewport_vs_triangle, viewport_nonzero_origin, half_pixel_solid_rect, vs_color_triangle, texture_2x2) all pass with Stage 2 constants-only hybrid **default-on** on Apple Silicon, and the full native suite is green — the Stage-1-shadow-drop read-path bugs were fixed and `R-BACK-12.26` readback equality holds for the constants-only path. The only remaining open argbuf work is the texture/sampler resource-array sub-lane, tracked in the next row. |
| Texture/sampler argbuf resource-array lane (`R-BACK-12.22`–`R-BACK-12.26`) | ✅ (opt-in) | Opt-in lane (`DXMT9_ARGBUF_RESOURCE_ARRAY`, default-OFF) wiring fragment-stage textures/samplers (FFP s0..s7) into the slot-30 argbuf as MSL `array<texture2d<float>,8> textures [[id(4)]]` / `array<sampler,8> samplers [[id(12)]]`, with `useResource(Read\|Sample)` residency for argbuf-pointed textures (heap-backed textures already covered by the encoder-open `useHeap` walk, `dxmt9_draw_encoder.mm:1130`). Cube/volume/stage≥8 draws correctly fall back to the direct Stage-1 lane. **Default constants-only Stage 2 hot path is unchanged** — this lane is gated off. **2026-05-25 ROOT CAUSE FOUND + FIXED:** the historical "texture corpus readback fault" was a **GPU page fault** (`kIOGPUCommandBufferCallbackErrorPageFault`), and the cause was the **sampler**, not the texture or the descriptor shape. dxmt9 created every `MTLSamplerState` without `MTLSamplerDescriptor.supportArgumentBuffers=YES` (the default direct-bind lane never needs it), so the sampler had no valid `gpuResourceID`; writing it into the argbuf via `setSamplerState` left `samplers[0]` pointing at garbage, and `tex0.sample(samp0, …)` faulted at dereference time. The Metal API validation layer reported no error (the encoding is structurally valid CPU-side), and two descriptor shapes (N×`arrayLength=0` vs one×`arrayLength=8`) page-faulted identically — both ruled out, isolating the sampler. Fix: set `WMTSamplerInfo.support_argument_buffers = shaders::argbufResourceArrayEnabled()` in `makeSampler` / `makeSamplerInfo` (`dxmt9_draw_encoder.mm`), gated on the opt-in lane so the default path is byte-identical. **Verification (Apple Silicon M1, apple7):** the previously-faulting `texture_2x2` and the full 90-test texture/sampler/cube/volume/mip corpus pass **both lane-ON and lane-OFF** (zero regressions); `dxmt9-argbuf-hybrid-spec` + `dxmt9-argbuf-hybrid-msl-spec` green. Remaining (optional) follow-up: per-draw argbuf-reopen is the safe floor — a "only reopen when a texture/sampler changed" optimisation and a microbenchmark proving argbuf patch + residency cost beats direct binding for target workloads are not yet done; the lane stays opt-in until that perf case is made. |
| Tile-shader FFP fast path (`R-BACK-13.1`–`R-BACK-13.6`) | ✅ (correct; default-off pending perf) | Apple-Silicon-only acceleration of FFP fog / alpha-test / A2C via `MTLTileRenderPipelineState` instead of fragment-stage discard. **winemetal bridge surface landed 2026-05-10 (`d9c54eb`):** `WMTTileRenderPipelineDescriptor`, `MTLDevice_newRenderPipelineStateWithTileDescriptor`, tile-stage encoder ops (`setTileRenderPipelineState`, `dispatchThreadsPerTile`, `setTileBuffer`, `setTileTexture`, `setTileBytes`, `setTileSamplerState` — 7 funcs). `MTLDevice_supportsFamily(WMTGPUFamilyApple3)` already exposed. **dxmt9-runtime adopter landed 2026-05-10 (`84ea225`):** `makeFfpTilePixelSource` MSL emitter (imageblock half4 for 8-bpc attachments, float4 otherwise; FFP arithmetic always `float`); `selectTileFfpForPass(state, supportsApple3)` selector with reason taxonomy (GpuFamily / NotFfp / Precision / UnsupportedState); PSO key extended with `tileFfpMode` bit, separate cache entries, `device.newRenderPipelineState(WMTTileRenderPipelineDescriptor&, Error&)` overload wired; mid-pass demotion split treats `tileResplit` as a flush trigger alongside RT-change/hazard; counters `tile_ffp_pass_count`/`portable_ffp_pass_count`/`tile_ffp_fallback_{precision,unsupported_state,gpu_family,mid_pass_ineligible}`/`tile_ffp_mid_pass_resplit_count` all wired; native tests `dxmt9-tile-ffp-selector-spec` (11 cases) and `dxmt9-tile-ffp-msl-spec` (7 cases) pass. **Encoder wire landed 2026-05-10 (`523b66e`):** `encodeDraw` now branches on `tileFfpMode` and calls `encoder.setTileRenderPipelineState(pipeline)` + `encoder.dispatchThreadsPerTile` when selector chose tile path; mid-pass demotion path unchanged (`activePassUsesTileFfp` flag still drives `tileResplit` flush trigger). **W2 tile-size query landed 2026-05-10 (`9ba7c8e`):** new winemetal accessors `MTLRenderCommandEncoder_tileWidth` / `_tileHeight` query Metal's chosen tile size for the open encoder, replacing the 16×16 hardcode. Falls back to 16×16 when Metal returns 0 (older OS / unsupported attachment shape). **🔴 2026-05-25: tile↔portable readback equality FAILS — the encoder wire (`523b66e`) does `setTileRenderPipelineState`+`dispatchThreadsPerTile` INSTEAD OF the base-colour `setRenderPipelineState`+`drawPrimitives`, so the tile kernel post-processes the cleared imageblock and any tile-routed draw renders BLACK (geometry never rasterised; `draw_geometry_samples=0`). Default-on for eligible (non-textured FFP fog/alpha-test/A2C) draws on Apple Silicon → those render wrong. Needs a two-stage encode (base-colour draw → then tile dispatch). The `dxmt9-tile-ffp-selector-spec` / `-msl-spec` pass because they assert selector/emit shape, NOT GPU output — they did not catch this.** **✅ RESOLVED 2026-05-25 (`43ae1dd`..`7ed5fcb`):** two-stage encode implemented + readback-verified (tile == portable == `(64,128,191)`, `draw_geometry_samples=1`). Also fixed the tile kernel's never-compiling `imageblock_data.data()` accessor (→ `read`/`write`) and the invalid-on-M1 `setTileRenderPipelineState` bind (→ `setRenderPipelineState` per §13.5). Default `DXMT9_TILE_FFP=off` retained until a workload benchmark proves tile beats portable (single-draw signal tile 0.039 ms vs portable 0.050 ms is inconclusive). See the D3D9 API Coverage Inventory tile-FFP bullet. |
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
| `d3d9.dll` as user-facing PE DLL | ✅ (builtin x64 + WoW64 x86) | Built via `build-win32-x64-builtin` / `build-win32-x86-builtin` (llvm-mingw cross + `winebuild --builtin` postprocess), staged to `lib/wine/{x86_64-windows,i386-windows}/d3d9.dll` + prefix `system32`/`syswow64`. **2026-05-25 end-to-end Wine WSI smoke PASS on BOTH x64 and WoW64/x86** — see thunk row. Native (app-local) variant: PE side validated, runtime-gated — see thunk row. |
| `winemetal.dll` as shared Wine builtin PE bridge | ✅ (builtin x64 + WoW64 x86) | Built + `winebuild --builtin` postprocessed, staged to `lib/wine/{x86_64-windows,i386-windows}/winemetal.dll`. Loaded under `WINEDLLOVERRIDES="d3d9,winemetal=n,b"`; `abi-hash handshake OK` logged for both the 64-bit and 32-bit bridge (32-bit dispatcher proven via 32-bit pointer widths, e.g. `dispatcher=7BF2801D`). |
| PE bridge ↔ unix module thunk mechanism | ✅ (builtin, x64 + WoW64 x86); ⚠️ (app-local runtime-gated) | **2026-05-25 end-to-end Wine WSI smoke PASS (builtin, Sikarugir `sikarugir-cx-24.0.7`).** Two binaries exercised both arch lanes: (a) `conf-d3d9-triangle` x64 + x86 (`--binary <abs path to ...-x86.exe>`), (b) the dedicated **`conf-d3d9-wsi-present`** (its missing `build/wsi_present/wsi_present_x64.exe` was built from `tests/integration/wsi_present/main.cpp` via `x86_64-w64-mingw32-clang++ -O2 ... -ld3d9 -luser32 -lgdi32`; **180/180 `device_present hr=0x0`, ssim 1.0**). Evidence (all lanes): `[winemetal-abi] abi-hash handshake OK (0x29886309da4f648d)` in BOTH PE + unix logs (64-bit) and the 32-bit equivalent (PE↔unix lockstep at runtime); `builtin unixlib lookup: info=1000 status=0x0` (the supported path); `[dxmt9-wsi] layer_acquisition=macdrv_functions`; `CreateDeviceEx → non-null`; `device_present hr=0x0`; `returncode=0`. **App-local (native) lane — PARTIAL, host-runtime-gated (NOT a dxmt9 defect):** native non-builtin DLLs built (`build-win32-x64`, `wine_builtin_dll=false`, plain `PE32+`, no static `winemetal.dll` import), packaged via `scripts/tools/package_app_local.py`, and **the dxmt9 PE chain is correct end-to-end** — `+loaddll` confirms the app-dir native `d3d9.dll`/`winemetal.dll` load (not the runtime builtins), the provider locator runs the spec R-DEPLOY-3.4 order (`env → module-dir → exe-dir → runtime-by-name`), NT-path conversion succeeds, and a missing provider surfaces as a clean `Direct3DCreate9` failure (R-DEPLOY-3.8). It is blocked at the unix provider load because the app-local locator uses `NtQueryVirtualMemory(info=1002 = MemoryWineLoadUnixLibByName)` (`winemetal_bridge.cpp:648`) and **Sikarugir CX 24.0.7 (wine-9.0 base) does not implement info class 1002** → `status=0xc0000003` → `abi-hash unix-call failed; refusing to attach`. This is exactly the `min_wine_unixlib_feature: MemoryWineLoadUnixLibByName` precondition the deploy manifest pre-declares and the `test_wild.rules.md` checklist-5 / CrossOver "class 1002" note. App-local end-to-end validation (R-DEPLOY-6.2/6.3) needs a newer Wine that implements info=1002 with a non-stripped `winemac.so`; the builtin lane works on Sikarugir via info=1000. |
| `dxmt9.dll` / `dxmt9.so` legacy bridge naming | ❌ | Removed from target spec; stale references should be treated as documentation drift |

---

## Verification Layer

| Area | Status | Evidence |
|---|---|---|
| TLA+ specs: CommandQueue, QueueLifecycleRefinement, PresentFrameLatency, ResourceLifetime, EncoderLifecycle, QuerySeqId, ConcurrentProgressSignals, DrawableToken, WireHandleGeneration, PresentIdAba | ✅ | ConcurrentProgressSignals added 2026-05-09 (T2 closes G1: pacing-axis independence under R-ARCH-6.8/6.9). DrawableToken / WireHandleGeneration / PresentIdAba added 2026-05-18 (close drawable-token handoff race, PE→unix generation-stamp zombie, and (slot, generation) ABA-safety respectively). Traceability matrix in `specs/verification/design.md` §7. |
| All ten specs model-checked by TLC — zero errors | ✅ | EncoderLifecycle now uses exact `lastReadHandles` / `lastWriteHandles : SUBSET Handles` instead of Boolean `hazardFlag` (T1 closes G2 / R-VERIF-4.4); `AtMostOneEncoder` tautology removed (T1 closes G3 / R-VERIF-4.1); `BloomNeverForcesSplit` invariant proves Bloom signal cannot trigger an encoder split (T4 closes G4 / R-BACK-2.28). Distinct states post-changes: ConcurrentProgressSignals 6,172, EncoderLifecycle 3,403 (Bloom-extended). |
| Companion native spec for `DrawableToken.tla` stash/take/wait state machine | ❌ | `tests/native/backend/present_acquire_policy_spec.cpp` covers env-var → `AcquirePolicy` resolution only. The token interleaving (stash → wait → take → complete/fail) is exercised by the TLA+ model and at runtime but has no deterministic native spec. |
| Companion native spec for `ConcurrentProgressSignals.tla` cross-axis non-blocking | ❌ | Pacing independence is observable only at the queue, not as a pure-data transform. A queue-observer / fake-backend probe covering all three axes (`completedSeqId` / `presentCompletedSeqId` / `ringSlotOccupancy`) simultaneously would close this. Same evidence gap as the queue-observer row below. |
| Companion native spec for `PresentIdAba.tla` slot-reuse ABA-safety | ❌ | `HandleArena` slot recycle is exercised end-to-end only through `chunk_record_validation_spec.cpp` (the generation-reject path). A focused `HandleArena` slot-reuse spec asserting `StaleResolvesNull` / `NoCrossSlotAlias` directly is missing. |
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
| Wine d3d9 test inventory (visual.c / device.c / d3d9ex.c / stateblock.c) | ✅ | [`specs/gap_d3d9_wine_test.md`](gap_d3d9_wine_test.md) inventories every Wine `START_TEST` entry across the four files and links each to the PE conformance function(s) carrying the evidence. **268/268 covered** as of HEAD `4a3aaa0` (visual.c 135, device.c 105, d3d9ex.c 27, stateblock.c 1; 0 scaffolded / partial / failing / untracked). Oracle pinned to Wine `wine-11.6` (`6e073d2`). Regenerated from Wine source + the gitignored `specs/wine_test.plan.md` via `scripts/tools/gen_wine_d3d9_test_inventory.py`. |
| Wine `device.c` / `d3d9ex.c` / `stateblock.c` conformance subset | ⚠️ | `tests/conformance/d3d9/MANIFEST.toml` now lists 248 Wine-oracle PE conformance cases with DoD/acceptance criteria and `dxmt9-d3d9-conformance-manifest-check` validates lane/arch evidence. Current case-level manifest status (HEAD `4a3aaa0`): 4 passing, 237 partial, 6 failing, 1 skipped. The builtin x64 lane is largely passing per the chunked-runner evidence snapshot (Sikarugir-CX 24.0.7, 2026-05-24); cases sit at `partial` because the app-local and x86 lanes are not yet fully evidenced — that lane/arch breadth is the open promotion work (the Wine-test *mapping* itself is complete, `gap_d3d9_wine_test.md` 268/268). **2026-05-19 extension wave (V5):** added 13 more clean-room scaffolds closing the remaining `planned` rows in `specs/wine_test.plan.md` — `test_buffer_no_dirty_update`, `yuv_color_test`, `yuv_layout_test`, `test_3dc_formats`, `test_position_index`, `test_mvp_software_vertex_shaders`, `shadow_test` from visual.c; `D3DSBT_ALL` / `D3DSBT_PIXELSTATE` / `D3DSBT_VERTEXSTATE` capture-apply matrix slices from stateblock.c; and `test_query_get_data_size_policy`, `test_check_device_format_conversion_matrix`, `test_multithreaded_device_creation_policy` for advanced device.c surfaces (query GetDataSize, CheckDeviceFormatConversion matrix, D3DCREATE_MULTITHREADED). **Wine migration wave (2026-05-19):** added 43 new clean-room scaffolds across `device.c`, `d3d9ex.c`, and `visual.c` oracles. From `device.c` / `d3d9ex.c`: `test_multi_device_independent_state`, `test_mode_change_focus_swap_policy`, `test_reset_fullscreen_focus_window_policy`, `test_window_position_present_parameter_policy`, `test_pinned_buffers_d3dusage_policy`, `test_volume_blocks_compressed_layout_policy`, `test_ex_user_memory_getdc_format_policy`, `test_swapchain_get_display_mode_ex_policy`, `test_ex_get_adapter_luid_policy`, `test_ex_get_adapter_display_mode_ex_policy`, `test_backbuffer_resize_present_parameter_policy` (11). From `visual.c`, four new bucket files cover formats (`float_texture_test`, `g16r16_texture_test`, `volume_v16u16_test`, `srgbtexture_test`, `srgbwrite_format_test`, `volume_srgb_test`, `volume_dxtn_test`, `test_signed_formats`), depth-stencil (`z_range_test`, `ds_size_test`, `depth_buffer_test`, `depth_buffer2_test`, `depth_bounds_test`, `zenable_test`, `zwriteenable_test`, `multisampled_depth_buffer_test`), render-target / clear / surface (`depth_clamp_test`, `clear_test`, `test_clear_different_size_surfaces`, `color_fill_test`, `offscreen_test`, `stencil_cull_test`, `update_surface_test`, `test_flip`), and shading / lighting / resource lifetime (`test_shademode`, `lighting_test`, `test_lighting_matrices`, `release_buffer_test`, `test_evict_bound_resources`, `add_dirty_rect_test`, `test_multisample_get_front_buffer_data`, `test_multisample_mismatch`) — 32 total. All declare `lanes=[app-local,builtin]` × `arches=[x64,x86]`; x64 + x86 `dxmt9-d3d9-conformance.exe` builds clean. **Source organization (T8/T9, 2026-05-08/09):** `d3d9_conformance.c` (originally 1,777 LOC) split into a thin driver (73 LOC) + per-domain `d3d9_conformance_{device,resource,swapchain,query_stateblock}.c` linked to a single `dxmt9-d3d9-conformance.exe` (T8 `b2c4c75`); standalone per-test executables normalized to single `d3d9_*` prefix (drop `_x64` suffix from 6 files, add `d3d9_` prefix to 3 bare files; T9 `739a080`, target names preserved for external invocation compatibility). Focused x64 app-local export/auxiliary runtime evidence passes. The first device-backed app-local run now reaches the provider with 328 checks, 26 failures, and 0 skips; failing groups are factory validation, present-parameter validation, Ex create/reset, private-data resource wrappers, Ex shared-handle policy, and creation-failure out pointers |
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
| Bootstrap verified entry: `conf-d3d9-wsi-present` on Heroic Wine 11.5 | ✅ | local workflow validation |
| Verified real application entries: `sample-d3d9-basic-hlsl`, `sample-d3d9-tutorial07`, `sample-d3d9-hdr-formats`, `sample-d3d9-dxut-simple`, `sample-d3d9-irrlicht-lights` | ✅ | Heroic Wine 11.5, direct capture, SSIM 1.0000 |
| Exploratory commercial entry: `app-d3d9-anno-1404` | ⚠️ | supported on Heroic `Wine-11.6-DXMT`; plain `Wine-11.6` is research-only due to Wine `d3dx10_43` aborts |
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
| Wine PE / `winemetal` deployment | partial (builtin lane end-to-end validated 2026-05-25 on BOTH x64 + WoW64/x86, via `conf-d3d9-triangle` + dedicated `conf-d3d9-wsi-present`; app-local native lane PE-chain validated but blocked at unix provider load by Sikarugir wine-9.0 lacking `NtQueryVirtualMemory` info=1002 — needs a newer Wine) |
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
| 2 | GPU-runtime validation for the landed-but-unverified features (RESZ MSAA→INTZ resolve, NULL depth-only pass, `SAMP_MIPMAPLODBIAS`, tile-FFP↔portable equality). **2026-05-25: confirmed BLOCKED on `shader_runner_dxmt9` DSL expressiveness, not implementation** — see the per-feature unblock table in the D3D9 API Coverage Inventory section. Order by tractability: (a) MIPMAPLODBIAS = ~5-line DSL sampler-key → run now; (b) tile-FFP = add `DXMT9_TILE_FFP` force-toggle then A/B; (c) NULL = native render-pass-attachment spec; (d) RESZ = native depth-resolve spec or Wine PE exe | `gap_d3d9.md` deferred-evidence rows, R-FORMAT-11/12, R-BACK-13.* | 
| 3 | Promote PE conformance lane/arch breadth: the builtin x64 lane is largely passing, so drive the app-local and x86 lanes to passing across the 237 `partial` manifest cases and fix the 6 `failing` cases (factory validation, present-parameter validation, Ex create/reset, private-data resource wrappers, Ex shared-handle, creation-failure out pointers); includes the Wine-run validation of the new COM stub gates | R-TEST-12.1, R-TEST-12.20 |
| 4 | Finish factory HRESULT parity and validation coverage, including `CheckDeviceFormatConversion`, multisample quality levels, device-type enum handling, and any selected optional export-profile stubs beyond the current auxiliary set | R-CORE-1.9, R-CORE-1.11-R-CORE-1.15, R-TEST-12.9-R-TEST-12.10, R-TEST-12.15-R-TEST-12.16 |
| 5 | Implement and verify D3D9Ex user-memory texture/offscreen-surface paths and shared-handle error policy | R-CORE-4.11-R-CORE-4.12, R-TEST-12.11 |
| 6 | Implement Wine-oracle D3D9Ex QI conformance and finish Ex/display/swap-chain validation | R-CORE-1.6, R-CORE-10.2-R-CORE-10.4, R-CORE-10.17, R-CORE-10.18, R-TEST-12.4 |
| 7 | Expand Wine stateblock conformance beyond the current compact scaffold and implement full `D3DSBT_*` masks/resource/reset interactions | R-CORE-3.7, R-CORE-3.8, R-TEST-12.5, R-TEST-12.19 |
| 8 | Expand compact reset/window scaffolds if runtime evidence exposes missing Wine-visible edge cases, then promote device lifetime/refcount, query validation, resource wrapper, and scene scaffolds with runtime evidence | R-CORE-2.6, R-CORE-2.8, R-CORE-4.8-R-CORE-4.10, R-CORE-8.3, R-TEST-12.3, R-TEST-12.12-R-TEST-12.14, R-TEST-12.17-R-TEST-12.18 |
| 9 | ✅ **Done (builtin x64 + WoW64/x86, 2026-05-25).** Built + verified the upstream-style PE targets `d3d9.dll` + `winemetal.dll` + `winemetal.so` on both arch lanes (builtin postprocess, staged to `lib/wine/{x86_64-windows,i386-windows,x86_64-unix}`). Missing `conf-d3d9-wsi-present` binary also built. Residual: **native app-local lane** — dxmt9 PE chain validated but end-to-end blocked by the host runtime (Sikarugir wine-9.0 lacks `NtQueryVirtualMemory` info=1002); needs a newer Wine, not a dxmt9 change. | d3d9/wsi §6, §9 |
| 10 | ✅ **Done (builtin, x64 + WoW64/x86, 2026-05-25).** Wine WSI smoke PASSED on the upstream-style builtin deployment — both `conf-d3d9-triangle` (x64 + x86) and the dedicated `conf-d3d9-wsi-present` (180/180 presents, ssim 1.0): abi-hash handshake OK (64- and 32-bit), `macdrv_functions` layer attach, device + present + exit 0. Gap status promoted (Wine PE layer rows). | R-TEST-11.3 |
| 11 | D3D8 entry point + IDirect3D8 factory + resource wrappers | R-D3D8-1.1, R-D3D8-2.1 |
