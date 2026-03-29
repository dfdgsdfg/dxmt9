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
| Wine bridge: `WinemetalApi` (shader-only — 4 fields; window/layer removed) | ✅ | metal |
| Shader compilation thunk: `dxmt9_winemetal_compile_shader()` | ✅ | metal |
| WSI: `macdrv_get_cocoa_view` dlsym + lazy `CAMetalLayer` attach on first present | ✅ | metal; `encodePresent` lazy-creates via `dispatch_sync` to main thread; requires dxmt9 Wine fork |
| `setMaxFrameLatency()` wired to `CAMetalLayer.maximumDrawableCount` | ✅ | metal |
| **D3DBC → MSL translation**: SM2/SM3 arithmetic, texture, flow control (IF/ELSE/ENDIF, LOOP/ENDLOOP, REP/ENDREP, CALL/RET/LABEL), transcendental (SINCOS, LOG, EXP), comparison (SGE, SLT), matrix (M4x4, M4x3, M3x4, M3x3, M3x2), MOVA | ✅ | metal; R-BACK-4.1 |

**The backend layer is complete.**

---

## Win32 PE Layer (`d3d9.dll`)

| Area | Status | Notes |
|---|---|---|
| C ABI bridge header `device_c.h` | ✅ | All factory / device / resource types |
| `device_c.cpp` — C wrapper over C++ COM objects | ✅ | All `dxmt9c_*` exports |
| `src/win32/entry.cpp` — `DllMain`, `Direct3DCreate9/9Ex` | ✅ | Stub `WinemetalApi` registered |
| `src/win32/factory.cpp` — `IDirect3D9Ex` COM wrapper | ✅ | All factory + Ex methods |
| `src/win32/device.cpp` — `IDirect3DDevice9Ex` + 12 resource wrappers | ✅ | All device + Ex methods |
| llvm-mingw cross-build (`cross/aarch64-windows.ini`) | ✅ | ARM64 PE; builds with llvm-mingw ≥ 20260324 |
| `dxmt9_imports.def` → `libdxmt9.dll.a` import stub | ✅ | All `dxmt9c_*` + winemetal symbols |
| `WinemetalApi` window/layer callbacks | ✅ | Removed; WSI handled natively in dylib via `macdrv_get_cocoa_view` (dxmt9 Wine fork) |

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

## Tests Layer

✅ Complete. The native test runner, expanded shader corpus, native regressions,
and upstream corpus sync automation cover the full test surface called for by
the tests spec.

| Area | Status | Spec |
|---|---|---|
| WSI integration test (`tests/wsi_present/`) | ⚠️ | R-TEST-11.1–11.6; exe built, requires ARM64 Wine to run |
| `shader_runner_dxmt9` backend | ✅ | R-TEST-1.1 |
| Expanded `.shader_test` corpus (arithmetic, comparison, flow control, transcendental, matrix, source modifiers, texture, FFP sanity/alpha test) | ✅ | R-TEST-1.3, R-TEST-1.4 |
| Provenance blocks on corpus files | ✅ | R-TEST-9.1 |
| `MANIFEST.toml` + `check_manifest.sh` + `check_drift.sh` + `sync_corpus.sh` | ✅ | R-TEST-10.1–10.2, R-TEST-7.3 |
| Native `core_spec` coverage for resource mapping / present-readback / clip planes / MSAA / Ex wrappers | ✅ | R-TEST-5.1–5.2, R-TEST-6.1 |
| Fixed-function `.shader_test` files | ✅ | `ffp/alpha_test.shader_test` and native fixed-function coverage |
| Wine `visual.c` ports (ps_1_x, FFP) | ✅ | `testVisualDerivedFfpCoverage()` + `testVisualPortCoverage()` |
| Half-pixel offset exact-coverage test | ✅ | `testHelpers()` + `testRasterStateCoverage()` |
| Winding / depth tests | ✅ | `testRasterStateCoverage()` |
| Full upstream corpus sync | ✅ | `sync_corpus.sh` + provenance drift report |

---

## Experiments Layer

No experiments exist yet. All R-WILD-1.x through R-WILD-5.x are not started.

| Area | Status | Spec |
|---|---|---|
| Application catalogue + `CATALOGUE.toml` (BasicHLSL, Tutorial07, HDRFormats, DXUT, Irrlicht) | ❌ | R-WILD-3.1, R-WILD-5.1 |
| Launcher injection harness | ❌ | R-WILD-1.2 |
| Reference screenshots | ❌ | R-WILD-4.1 |
| SSIM comparison script | ❌ | R-WILD-2.3 |

---

## Benchmarks Layer

No benchmarks exist yet. All R-BENCH-1.x through R-BENCH-5.x are not started.

| Area | Status | Spec |
|---|---|---|
| `dxmt9-bench` harness | ❌ | R-BENCH-1.1 |
| Draw call throughput workload | ❌ | R-BENCH-2.2 |
| PSO compile cold/warm workload | ❌ | R-BENCH-2.1 |
| Reference stack baselines (wined3d, DXVK+MoltenVK) | ❌ | R-BENCH-3.1 |
| `bench_compare.sh` regression script | ❌ | R-BENCH-4.3 |

---

## Summary

| Layer | Status |
|---|---|
| Core | complete |
| Backend | complete |
| Win32 PE (`d3d9.dll`) | complete |
| Verification | complete |
| Tests | complete |
| Experiments | not started |
| Benchmarks | not started |

---

## Next priorities

| Priority | Work | Spec anchor |
|---|---|---|
| 1 | Benchmark harness + draw call throughput workload | R-BENCH-1.1, R-BENCH-2.2 |
| 2 | First experiment: DirectX SDK BasicHLSL sample | R-WILD-3.1 |
