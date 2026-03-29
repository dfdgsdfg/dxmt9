# Spec–Implementation Gap

Current state of the codebase vs. the specifications.
Updated against current working tree.

Legend: ✅ implemented · ⚠️ partial · ❌ not started

---

## Core Layer

| Area | Status | Evidence / notes |
|---|---|---|
| Factory, adapter enum, `CheckDeviceFormat` | ✅ | `Factory`, `formatSupportsUsage()` |
| `GetAdapterIdentifier` | ✅ | `Factory::getAdapterIdentifier()` |
| `EnumAdapterModes` | ✅ | `Factory::enumAdapterModes()` |
| `GetAdapterDisplayMode` | ✅ | `Factory::getAdapterDisplayMode()` |
| `GetAdapterMonitor` | ✅ | `Factory::getAdapterMonitor()` |
| `CheckDeviceType` | ✅ | HAL only |
| `CheckDeviceMultiSampleType` | ✅ | Factory + Device |
| Device lifecycle: Reset, `D3DPOOL_DEFAULT` invalidation | ✅ | `Device::reset()` |
| Fullscreen `createDevice` | ✅ | `normalizePresentParameters()` |
| Device-lost: trigger + recovery | ✅ | `setDeviceLostObserver()` |
| `TestCooperativeLevel` | ✅ | Returns `D3DERR_DEVICELOST` when lost |
| `CreateAdditionalSwapChain` | ✅ | `Device::createAdditionalSwapChain()` |
| Device state shadow (render / texture / sampler / transform / lights / stencil) | ✅ | `DeviceState`, all `Set*`/`Get*` methods |
| BeginScene / EndScene | ✅ | Nested-call validation |
| StateBlock capture / restore | ✅ | Full state copy |
| DrawPrimitive, DrawIndexedPrimitive, UP variants | ✅ | All four variants |
| TriangleFan decomposition | ✅ | `decomposeTriangleFanIndices()` |
| Half-pixel offset | ✅ | `halfPixelFixup()` |
| FfpVertexKey / FfpPixelKey generation | ✅ | `makeFfpVertexKey()`, `makeFfpPixelKey()` |
| Shader bytecode storage + hash | ✅ | `ShaderBytecode`, FNV-1a hash cache |
| Format table | ✅ | `formatTable()` |
| `makeDefaultCaps()` | ✅ | R-CAPS-1 through R-CAPS-5 |
| Buffer / Texture / Surface lifecycle | ✅ | Pool-based reset behavior |
| `UpdateSurface`, `UpdateTexture`, `StretchRect`, `ColorFill`, `GetRenderTargetData` | ✅ | Core-side logic |
| Query: EVENT, OCCLUSION, TIMESTAMP, TIMESTAMPFREQ, TIMESTAMPDISJOINT | ✅ | `submittedSeqId_` / `completedSeqId_` waterline |
| Clip planes | ✅ | `transformClipPlane()`, `DrawUniforms.clipPlanes` |
| MSAA | ✅ | `sampleCount()`, `RenderTargetAttachment.sampleCount` |
| COM: `IDirect3D9` — full factory interface | ✅ | All 8 factory methods |
| COM: `IDirect3DDevice9` — full device method surface | ✅ | All 40+ methods |
| COM: `IDirect3DSwapChain9` | ✅ | Present, backBuffer, depthStencilSurface |
| COM: `Direct3DCreate9(sdkVersion)` | ✅ | Returns `nullptr` for wrong SDK version |
| TLA+ `SeqIdSafety` / `BoundedInflight` / `QueryResolutionSafety` assertions | ✅ | `DXMT_ASSERT` with `// TLA+:` comments |
| `IDirect3D9Ex` — `Direct3DCreate9Ex`, `GetAdapterModeCountEx`, `EnumAdapterModesEx`, `GetAdapterDisplayModeEx`, `GetAdapterLUID`, `CreateDeviceEx` | ✅ | `Direct3D9Impl` inherits `IDirect3D9Ex` |
| `IDirect3DDevice9Ex` — `CheckDeviceState`, `ResetEx`, `PresentEx`, `SetMaximumFrameLatency`, `GetMaximumFrameLatency`, `WaitForVBlank`, `CheckResourceResidency`, `GetGPUThreadPriority`, `SetGPUThreadPriority`, `SetConvolutionMonoKernel`, `ComposeRects`, `CreateRenderTargetEx`, `CreateOffscreenPlainSurfaceEx`, `CreateDepthStencilSurfaceEx`, `GetDisplayModeEx` | ✅ | `Direct3DDevice9Impl` inherits `IDirect3DDevice9Ex` |

**The core layer is complete.** All R-CORE-1.x through R-CORE-10.x satisfied.

---

## Backend Layer

| Area | Status | Notes |
|---|---|---|
| `BackendDevice` interface + sim backend | ✅ | sim |
| `MTLDevice` init + `MTLCommandQueue` | ✅ | metal |
| Command queue ring: 32 slots, `kMaxInflight=3`, Wine/encode/finish threads | ✅ | metal |
| Ring allocators: `RingArena` for argbuf, lambdaStore, staging, copyTemp | ✅ | metal |
| Clear-as-load-action folding | ✅ | metal |
| Render-target change → encoder split | ✅ | metal |
| Encoder merging with Bloom-filter hazard detection | ✅ | metal |
| PSO cache: `ShaderVariantKey`, async compile, `MTLBinaryArchive` disk cache | ✅ | metal |
| DSS cache: `DepthStencilKey` + `StencilFaceKey` | ✅ | metal |
| FFP shader generator: `makeFfpVertexSource()` + `makeFfpPixelSource()` | ✅ | metal |
| Half-pixel offset in VS | ✅ | metal |
| Alpha test `discard_fragment()` | ✅ | metal |
| Clip plane `[[clip_distance]]` | ✅ | metal |
| Argument buffer layout: `DrawUniforms` | ✅ | metal |
| `MTLBuffer` / `MTLTexture` allocation | ✅ | metal |
| `mapBuffer` DISCARD / NOOVERWRITE / plain | ✅ | metal |
| Deferred destroy: `destroyPending` + `tryGarbageCollectUnlocked()` | ✅ | metal |
| Back buffer `DontCare` after present | ✅ | metal |
| `CAMetalLayer` swap chain, `nextDrawable`, blit, vsync | ✅ | metal |
| Encode + finish threads | ✅ | metal |
| Surface ops: SurfaceCopy, StretchRect, Readback, ColorFill | ✅ | metal |
| MSAA: multisample + resolve textures | ✅ | metal |
| Wine bridge: `WinemetalApi` dispatch table | ✅ | metal |
| Shader compilation thunk: `dxmt9_winemetal_compile_shader()` | ✅ | metal |
| `setMaxFrameLatency()` wired to `CAMetalLayer.maximumDrawableCount` | ✅ | metal |
| **D3DBC → MSL translation**: SM2/SM3 arithmetic, texture, flow control (IF/ELSE/ENDIF, LOOP/ENDLOOP, REP/ENDREP, CALL/RET/LABEL), transcendental (SINCOS, LOG, EXP), comparison (SGE, SLT), matrix (M4x4, M4x3, M3x4, M3x3, M3x2), MOVA | ✅ | metal; R-BACK-4.1 |

**The backend layer is complete.** All R-BACK-2.x through R-BACK-11.x satisfied.

---

## Verification Layer

| Area | Status | Evidence |
|---|---|---|
| TLA+ specs: CommandQueue, ResourceLifetime, EncoderLifecycle, QuerySeqId | ✅ | |
| All four specs model-checked by TLC — zero errors | ✅ | TLC 2.19; 100 / 57 / 17,241 / 52,522 distinct states |
| `SeqIdSafety` asserted with `// TLA+:` label | ✅ | `reclaimCompletedSlotsUnlocked()` |
| `QueryResolutionSafety` asserted | ✅ | `Query::getData()` |
| `BoundedInflight` asserted | ✅ | sim + metal `commitCurrentChunkUnlocked()` |
| `NoUseAfterFree` asserted with `// TLA+:` label | ✅ | `tryGarbageCollectUnlocked()` |
| `RingSafety` asserted with `// TLA+:` label | ✅ | `RingArena::allocateBytes()` + slot ring |
| `EncodeSafety` asserted with `// TLA+:` label | ✅ | encode loop |
| `WineCommit` action mapping comments | ✅ | `MetalBackendDevice` Wine-facing methods |

**The verification layer is complete.** All R-VERIF-1.x through R-VERIF-6.x satisfied.

---

## Experiments Layer

No experiments exist yet. All R-EXP-1.x through R-EXP-10.x are not started.

| Area | Status | Spec |
|---|---|---|
| `shader_runner_dxmt9` backend | ❌ | R-EXP-1.1 |
| `.shader_test` corpus — arithmetic + texture opcodes | ❌ | R-EXP-1.3 |
| `.shader_test` corpus — flow control (IF/LOOP/REP/CALL) | ❌ | R-EXP-1.3 |
| `.shader_test` corpus — transcendental (SINCOS/LOG/EXP) | ❌ | R-EXP-1.3 |
| `.shader_test` corpus — matrix ops (M4x4…M3x2), MOVA | ❌ | R-EXP-1.3 |
| Fixed-function `.shader_test` files | ❌ | R-EXP-2.1 |
| Wine `visual.c` ports (ps_1_x, FFP) | ❌ | R-EXP-8.2 |
| Half-pixel offset test | ❌ | R-EXP-3.1 |
| Winding / coordinate system tests | ❌ | R-EXP-4.1–4.2 |
| Resource mapping tests (DISCARD, MANAGED) | ❌ | R-EXP-5.1–5.2 |
| Provenance blocks on all `.shader_test` files | ❌ | R-EXP-9.1 |
| `MANIFEST.toml` + `check_manifest.sh` | ❌ | R-EXP-10.1–10.2 |

---

## Summary

| Layer | ✅ | ⚠️ | ❌ |
|---|---|---|---|
| Core | 38 | 0 | 0 |
| Backend | 25 | 0 | 0 |
| Verification | 9 | 0 | 0 |
| Experiments | 0 | 0 | 12 |

---

## Next priorities

| Priority | Work | Spec anchor |
|---|---|---|
| 1 | `shader_runner_dxmt9` backend + first arithmetic `.shader_test` files | R-EXP-1.1, R-EXP-1.3 |
| 2 | Flow control + transcendental + matrix `.shader_test` files | R-EXP-1.3 |
| 3 | `MANIFEST.toml` + `check_manifest.sh` + provenance blocks | R-EXP-9.1, R-EXP-10.1–10.2 |
| 4 | Wine `visual.c` ports (ps_1_x + FFP) | R-EXP-8.2 |
| 5 | Half-pixel, winding, resource mapping tests | R-EXP-3.1, R-EXP-4.x, R-EXP-5.x |
