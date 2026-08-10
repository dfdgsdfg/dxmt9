---
type: "Spec Gap"
title: "D3D9 API Coverage Inventory"
description: "Spec implementation and evidence gap tracker."
tags: [specs, gap, d3d9, api-coverage]
---

# D3D9 API Coverage Inventory

Comprehensive per-spec-item inventory of the D3D9 surface across:

- **A** — Shader bytecode opcodes + declaration types/usages/methods + token modifiers
- **B** — Render / texture-stage / sampler / transform / light / material / viewport / clip-status / gamma-ramp state
- **C** — Format + caps + present parameters + display mode + adapter identifier + raster status
- **D** — COM interface methods (21 interfaces, ~225 vtbl slots)

Authored 2026-05-23 from a four-agent parallel audit. Each row records:
defined? · backend-read / runtime-use? · tested? · source/test anchor ·
notes. **This file is a tracker, not a spec** — it is a committed
companion to the [D3D9 gap](gap.md) and [root gap index](../gap.md)
(tracked, not gitignored), and is regenerated on demand from the
current source tree via the methodology footers at the bottom of each
section.

Severity legend (used uniformly across the file):

| Symbol | Meaning |
|---|---|
| ✅ | implemented and tested |
| ⚠️ | implemented but partial / no test / round-trip only |
| 🟡 | intentionally aliased / acceptable-default (not a bug) |
| 🔵 | explicit safe-reject (e.g. perf-counter bucket) |
| 🚫 | E_NOTIMPL / D3DERR_INVALIDCALL (honest failure) |
| ❌ | silently dropped / undefined / dead-code |

## Current status summary (2026-07-18)

| Area | Current status |
|---|---|
| Unclassified silent D3D9 coverage gaps | **None in the current silent-coverage track.** This means known silent fall-throughs have been implemented or classified; it does not mean the full D3D9 feature surface is complete. |
| Shader declaration/modifier coverage | D3DDECLUSAGE values are accepted for programmable VS semantics; non-default D3DDECLMETHOD values safe-reject; `_PARTIALPRECISION` and `_MSAMPCENTROID` are lowered/tested. |
| Render/TSS/Sampler fixes | Depth bias, MRT color masks, two-sided stencil, COLORVERTEX, WRAP0..15 round-trip/no-op, TSS ARG0 triadic ops and initial getter values, sampler border color, mip LOD bias, and max mip level are closed. The complete non-dead render-state plus public TSS/sampler initial-value matrix is pinned by `dxmt9-state-draw-transform-spec`; the PE `test_texture_stage_states` scaffold now preserves the upstream initial-value checks instead of testing only Set/Get round-trips. |
| Remaining deferred/unsupported API surface | N-patch/adaptive tessellation, broad `ProcessVertices`/real SWVP shader execution beyond the covered fixed-function and programmable subsets, `D3DSAMP_ELEMENTINDEX`, `D3DSAMP_DMAPOFFSET`, AA line raster toggle, `ComposeRects`, and convolution kernel. |
| Remaining no-op/default stub surface | `SetDialogBoxMode`, `Set/GetClipStatus` clip-accumulation, `ValidateDevice`, `PreLoad`, non-AUTOGEN `GenerateMipSubLevels`, `AddDirtyBox`, `Set/GetNPatchMode`, `DeletePatch`, GPU thread priority, resource residency, and swapchain present stats use Wine-compatible no-op/default contracts rather than a real backend feature. |
| Remaining validation work | Broader conformance Wine-run sweeps remain ongoing; the `ProcessVertices` M4x4 subset passed targeted PE singleton validation, while the expanded programmable scaffold and the P8/A8P8 fixed-function + ps_2_0 sampler readback scaffold, including cube `samplerCUBE` and volume `sampler3D` coverage, are build-covered. The latest stable app-local `visual_mvp_software_vp_policy` singleton reaches D3D with the ABI handshake OK and passes the expanded SWVP draw sections. Focused P8/A8P8 visual rerun is pending after the pre-transformed FFP half-pixel fix because the current Sikarugir conformance prefix fails the winemetal ABI/unixlib attach path before D3D9 loads; native P8/A8P8 expansion, matching UpdateTexture destination-palette re-expansion, mixed FFP-VS/programmable-PS texture orientation, and real Metal programmable P8/A8P8 draw readback gates pass. |

### Current ignored / unimplemented inventory

| Class | Items | Current behavior |
|---|---|---|
| Explicit unsupported / failure | non-default `D3DDECLMETHOD` 1..6, `D3DSPR_TEMPFLOAT16`, `D3DSPR_LABEL`, N-patch patch draws, `SetConvolutionMonoKernel`, `ComposeRects`, unsupported vendor depth formats such as `RAWZ`/`NVDB` | Rejects with an explicit HRESULT or safe-reject path; not silently dropped. Ordinary UVs are still supported through `D3DDECLUSAGE_TEXCOORD` + `D3DDECLMETHOD_DEFAULT`. |
| Deferred partial feature surface | broad `ProcessVertices` / SWVP beyond the covered fixed-function and programmable VS paths, palette/P8 direct core storage, `D3DSAMP_ELEMENTINDEX`, `D3DSAMP_DMAPOFFSET`, D3D9 AA-line raster toggle | Implemented where currently needed/tested, but missing broader backend semantics or full D3D9 behavior. |
| Intentional no-op/default contracts | `SetDialogBoxMode`, `ValidateDevice`, `PreLoad`, non-AUTOGEN `GenerateMipSubLevels`, `AddDirtyBox`, `Set/GetClipStatus` accumulation, `Set/GetNPatchMode`, `DeletePatch`, swapchain Ex present stats, GPU thread priority, resource residency | Wine-compatible benign return/default behavior; documented as no-op/default rather than treated as an actionable silent gap. |

## Original top-level summary (historical 2026-05-23 roll-up)

> This table is preserved as the original audit snapshot. It is not the current
> triage source; use the current status summary and re-audit delta below.

| Category | Total rows | ✅ full | ⚠️ partial | ❌ silent gap | Source |
|---|---|---|---|---|---|
| A. Shader bytecode (SIO opcodes 0..95) | 96 | 96 | 0 | 0 | A.1 |
| A. SPR register kinds (0..18) | 19 | 14 | 0 | 0 | A.2 (3 🟡 + 2 🔵) |
| A. DECLTYPE (0..17 + UNUSED) | 18 | 17 | 0 | 1 | A.3 (UNUSED implicit) |
| A. DECLUSAGE (0..13) | 14 | 14 | 0 | 0 | A.4 |
| A. DECLMETHOD | 7 | 1 | 0 | 6 | A.5 (only DEFAULT direct vertex fetch is honored; ordinary UVs use `D3DDECLUSAGE_TEXCOORD`, not `D3DDECLMETHOD_UV`) |
| B. D3DRENDERSTATETYPE | 102 | ~52 | ~12 | ~38 | B.1 |
| B. D3DTEXTURESTAGESTATETYPE | 22 | 17 | 0 | 5 | B.2 |
| B. D3DSAMPLERSTATETYPE | 13 | 10 | 1 | 2 | B.3 (incl. **SAMP_BORDER_COLOR code-mismatch bug**) |
| B. D3DTRANSFORMSTATETYPE | 13 | 13 | 0 | 0 | B.4 (P0-2 closed `WORLDMATRIX(1..255)`) |
| B. Light / Material / Viewport / ClipStatus / Gamma | 32 | ~10 | ~10 | ~12 | B.5..B.9 |
| C. D3DFORMAT (standard + FOURCC + vendor pseudo) | 72 | ~45 | ~5 | ~22 | C.1..C.6 |
| C. D3DCAPS9 fields | 75 | 53 | 0 | 22 | C.7 |
| C. PresentParameters / DisplayMode / AdapterIdentifier / RasterStatus | 33 | ~22 | ~5 | ~6 | C.8..C.11 |
| D. COM methods (21 interfaces) | 225 | 176 | 13 | 33 | D.1..D.21 (10 explicit 🚫, 23 silent stub) |
| **Grand total** | **~803** | **~534** | **~46** | **~163** | |

> **The per-section detail tables (A/B/C/D) below predate the 2026-05-24
> re-audit and are stale where an item has since closed on `master`.**
> The authoritative current status is the re-audit delta immediately
> below; the detail tables are regenerated on demand via the methodology
> footers.

## Re-audit delta (2026-05-24, refreshed 2026-05-29, against `master`)

A read-only re-audit confirmed that a number of items authored as gaps on
2026-05-23 have since been implemented on `master`. Verdicts below carry
a `file:line` anchor checked against the live tree.

### Closed since authoring (verified done)

| Item | Section | Evidence |
|---|---|---|
| `SAMP_BORDER_COLOR` slot fixed to 4 (+ regression `static_assert`) | B.3 | `include/dxmt9/core_constants.hpp:443,472` |
| 6 formerly-unhandled D3DDECLUSAGE values → generic programmable VS semantics; 6 non-default D3DDECLMETHOD values → explicit safe-reject + perf counter | A.4/A.5 | `src/dxmt9/dxmt9_shader_decoder.cpp:83,89,121-126,134-158,1335`; `shader_bytecode_validation_spec.cpp:testGenericDeclUsagesDecodeCleanly`, `testNonDefaultDeclMethodsRejectCleanly` |
| `AlphaCmpCaps` sourced from `alphaCmpCaps` | C.7 | `src/d3d9/device_c_format_utils.cpp:295`; `src/d3d9/d3d9_pe_factory.cpp:268` |
| `AdapterIdentifier9` DeviceIdentifier GUID + WHQLLevel populated (stable FNV-1a) | C.9 | `src/d3d9/core_factory.cpp:276-299,303-327` |
| `D3DRASTER_STATUS::ScanLine` synthesized (monotonic) | C.10/C.11 | `src/d3d9/d3d9_pe_device.cpp:2203-2226` |
| `D3DRS_DEPTHBIAS` + `SLOPESCALEDEPTHBIAS` → Metal `setRasterizerState` | B.10#1 | `src/dxmt9/dxmt9_draw_encoder.mm:556-564` |
| `D3DLIGHT_POINT` / `D3DLIGHT_SPOT` FFP lighting (Position/Range/Atten/Theta/Phi) | B.5/B.10#5 | `src/dxmt9/dxmt9_ffp_shaders.cpp:168-284` |
| `D3DGAMMARAMP` real impl + PE shadow + unix bridge | B.9/D.* | `src/d3d9/d3d9_pe_device.cpp:578-612,2244-2261` |
| `INTZ` vendor format accepted | C.5 | `src/d3d9/device_c_format_utils.cpp:85` |
| `R8G8B8` mapped (not Unsupported) | C.7 | `src/d3d9/device_c_format_utils.cpp:50` |
| `D9CCaps::adapterOrdinal` assigned from adapter index | C.12#8 | `src/d3d9/device_c_factory.cpp:217` |
| `GenerateMipSubLevels` conditional real work (AUTOGENMIPMAP) | D.* | `src/d3d9/d3d9_pe_device_child_surface.cpp:917+` |
| `AddDirtyRect` (2D texture) validates pool/bounds | D.* | `src/d3d9/d3d9_pe_device_child_surface.cpp:1058` |
| MRT `COLORWRITEENABLE1..3` per-RT independent masks (closed 2026-05-24) | B.10#2 | `src/dxmt9/dxmt9_pipeline_cache.cpp` `makeBlendAttachmentKeys`; gate `dxmt9-backend-pipeline-key-spec` |
| `PresentParameters` MultiSampleQuality/Flags/FullScreen_RefreshRateInHz plumbed (closed 2026-05-24) | C.12#1,#9 | `include/dxmt9/core_constants.hpp` + `ppFromC`; gate `dxmt9-core-format-caps-spec` |
| COM stub regression gates: CheckResourceResidency, Get/SetGPUThreadPriority (native, closed 2026-05-24) | D.* | `tests/native/core/core_device_com_spec.cpp`; gate `dxmt9-core-device-com-spec` |
| `DF16`/`DF24` vendor depth-as-texture accepted + wired to Metal depth (closed 2026-05-24) | C.5 | `device_c_format_utils.cpp`/`core_format.cpp`/`dxmt9_format_convert.cpp`/`core_format_utils.cpp`; gate `dxmt9-core-format-caps-spec` |
| TSS `COLORARG0`/`ALPHAARG0` + MULTIPLYADD/LERP triadic ops, arg0 default `D3DTA_CURRENT` (closed 2026-05-24) | B.10#10 | `dxmt9_ffp_shaders.cpp`/`core_draw.cpp`; gates `dxmt9-ffp-triadic-msl-spec` + tss_multiplyadd/lerp/alpha_lerp GPU readbacks |
| `D32_LOCKABLE` (84) + `Q16W16V16U16` (110) formats end-to-end (closed 2026-05-24) | C.12#7 | mirror D32F_LOCKABLE→Depth32Float / A16B16G16R16→RGBA16Snorm; gate `dxmt9-core-format-caps-spec`. Caveat: Q16W16V16U16 FormatInfo mirrors its analog's `renderTarget=true` — real D3D9 may not advertise RT for it (minor caps over-report) |
| `ATOC` alpha-to-coverage — classification + behavioral (closed 2026-05-24) | C.5 | R-FORMAT-13; `RS_ADAPTIVETESS_Y`(181) ATOC/A2M1/A2M0 token → PSO `alphaToCoverage` bit → bridge `alphaToCoverageEnabled` (already exposed). `3293e39`, gate `dxmt9-backend-pipeline-key-spec` |
| `NULL` colorless render target — classification + runtime depth-only probe (closed 2026-05-29) | C.5 | R-FORMAT-12; no color backing, `beginRenderPass` omits color attachment, Lock/readback→INVALIDCALL. Gates: `dxmt9-resource-format-boundary-spec`, `dxmt9-null-rt-attachment-spec`, `dxmt9-shader-corpus-render_state-dxmt9_null_rt_depth_occlusion_readback`. |
| `RESZ` MSAA depth-resolve — **end-to-end + runtime readback** (closed 2026-05-29) | C.5 | R-FORMAT-11. Full pipeline: PE sentinel detect (`82c89fc`) → canonical record validate/classify → recorder retention + replay-execute + `MetalCommandKind::DepthResolve` + encoder dispatch (`d5572fc`) → `encodeDepthResolve` primitive + winemetal depth-resolve ABI (`8568fab`). Gates: `dxmt9-wmt-depth-resolve-abi-spec`, `dxmt9-resource-hazard-spec`, `dxmt9-chunk-record-replay-spec`, `dxmt9-state-draw-transform-spec`, `dxmt9-resz-depth-resolve-spec`, `dxmt9-shader-corpus-render_state-dxmt9_resz_intz_sample_readback`. |
| `SAMP_MIPMAPLODBIAS` per-sampler mip LOD bias — shader-side + readback validation (closed 2026-05-29) | B.3/B.10#4 | `bfb8a2d`; `SamplerLodBias` uniform (fragment slot 4) + `sample(…, bias(b))` at FFP (4) + translated-texld (1) sites; `bias(0)` no-op. Gates: `dxmt9-shader-transform-spec`, `dxmt9-shader-corpus-texture-dxmt9_mipmaplodbias_readback`, `dxmt9-shader-corpus-texture-dxmt9_mipmaplodbias_zero_control_readback`. NOT an ABI change. |
| `D3DSAMP_MAXMIPLEVEL` → sampler `lod_min_clamp` | B.3 | `include/dxmt9/core_constants.hpp:SAMP_MAX_MIP_LEVEL`; `src/dxmt9/dxmt9_draw_encoder.mm:824-829,883-885`; gate `backend_key_descriptor_spec.cpp` |
| `D3DRS_TWOSIDEDSTENCILMODE` (185) per-face stencil ops (closed 2026-05-24) | B.10#7 | `19f4274`+`fc10a5b`; mode-on → back-face ops from CCW render states (`MTLDepthStencilDescriptor.backFaceStencil`); mode-off mirrors front (byte-identical default). Also fixed a latent CCW-state leak. Gate `dxmt9-stencil-ref-spec`. |
| `D3DCLIPSTATUS9` Set/GetClipStatus — Wine-matching stub + defined default (closed 2026-05-24) | B.8/D.* | `6f190b9`+`fbfd917` (A'); reject null, accept-without-store, GetClipStatus returns the defined all-visible default. No HW clip-flag accumulation exists (matches wined3d). Gate `dxmt9-core-device-com-spec` (native) + `d3d9_device_misc.cpp` (conformance, compiles; Wine-run deferred). |
| COM silent-`S_OK` stub gates — SetNPatchMode/GetNPatchMode, DeletePatch, Set/GetClipStatus (closed 2026-05-24) | D.* | `6f190b9`; conformance gates in `d3d9_device_misc.cpp` pin the documented no-op/INVALIDCALL contracts (compiles in the win32 PE build; Wine-run validation deferred). |
| Shader dst modifiers `_SATURATE`, `_PARTIALPRECISION`, and `_MSAMPCENTROID` | A.6 | `_SATURATE` clamps destination values, `_PARTIALPRECISION` lowers through half precision, and `_MSAMPCENTROID` marks matching PS inputs as `[[centroid_perspective]]`; gate `shader_transform_spec.cpp:testD3DBCDestModifierPartialPrecisionLowering`, `testPs30CentroidInputModifierLowersToMslInterpolation`, and `test_visual_process_vertices_xyzhw_policy` |
| `D3DRS_COLORVERTEX` FFP material-source gate | B.1 | `src/d3d9/core_draw.cpp:2169-2170`; gate `ffp_key_determinism_spec.cpp` |
| D3D9 initial state defaults | B.1/B.2/B.3 | `DeviceState::reset()` now uses `FOGVERTEXMODE=NONE`, `FOGSTART=0.0f`, and full-width stencil masks; device construction/reset derives `ZENABLE` from `EnableAutoDepthStencil`; getter fallbacks expose FILLMODE, TEXTUREFACTOR, point/tessellation, and TSS ARG0 defaults without expanding hot draw snapshots. Full native matrix: `state_draw_transform_spec:testInitialD3D9StateMatrix`; PE TSS defaults: `test_texture_stage_states`. |
| Missing NORMAL FFP lighting contract | B.1/B.5 | FFP MSL emits a zero lighting normal when the declaration has no NORMAL, including POSITIONT and no-layout fallback paths, so all light dot products are zero as required; gate `core_ffp_state_key_spec:testFixedFunctionDeclarationMissingInputsEmitD3DDefaults`. |
| DITHER capability consistency | C.7 | `RasterCaps` no longer advertises `D3DPRASTERCAPS_DITHER` while `D3DRS_DITHERENABLE` remains shadow-only; `core_format_caps_spec` pins the R-CAPS-1 contract. |
| Cursor capability consistency | C.7 | `CursorCaps` is zero while cursor methods remain validation/shadow-only; R-CAPS-9 and `core_format_caps_spec` pin the contract. |

### Remaining actionable gaps (verified still open)

There are no known **unclassified silent fall-throughs** in the current audit
track. The following items remain real compatibility gaps: advertised behavior
without a backend implementation, partial feature subsets, explicit unsupported
paths, or intentional no-op/default contracts.

Current deferred/unsupported surface to keep visible:

| Item | Current decision |
|---|---|
| `D3DDECLMETHOD` 1..6 | Explicit safe-reject. These are declaration **methods** for fixed-function tessellator/N-patch/displacement-map evaluation. `D3DDECLMETHOD_UV` is not ordinary texture coordinate input; ordinary UVs are `D3DDECLUSAGE_TEXCOORD` with `D3DDECLMETHOD_DEFAULT`, which is supported. |
| `D3DSPR_TEMPFLOAT16`, `D3DSPR_LABEL` | Explicit safe-reject. |
| N-patch / adaptive tessellation render states and patch draws | Deferred/unsupported: `PATCHEDGESTYLE`, `TWEENFACTOR`, `POSITIONDEGREE`, `NORMALDEGREE`, tessellation levels, `SetNPatchMode`, `DrawRectPatch`, `DrawTriPatch`, `DeletePatch` no-op contract. |
| `ProcessVertices` / real software vertex processing | Partial. Device-lost gating remains first; fixed-function `D3DFVF_XYZ` or simple POSITION/COLOR/TEXCOORD source declaration input, including SHORT4N POSITION decode, through WORLD/VIEW/PROJECTION to `D3DFVF_XYZRHW` or simple POSITIONT/COLOR/TEXCOORD destination declaration CPU transform is implemented, including matching diffuse/specular/texture-coordinate passthrough. Fixed-function lighting now covers FLOAT3/raw/normalized NORMAL input plus material/global-ambient and enabled directional/point/spot-light diffuse into destination diffuse color, including point-light `Range` cutoff and `Attenuation0`/`Attenuation1`/`Attenuation2` readbacks plus spot cone rejection and falloff readback, `D3DRS_COLORVERTEX` `D3DMCS_COLOR1` diffuse, `D3DMCS_COLOR2` specular, `D3DMCS_COLOR1` ambient, and `D3DMCS_COLOR2` emissive material sources, plus specular secondary color when `D3DRS_SPECULARENABLE` is set. Destination `D3DFVF_PSIZE` and `D3DDECLUSAGE_PSIZE` readbacks are covered for fixed-function passthrough, and programmable VS `PSIZE` output can populate destination declarations. Source declarations may read supported attributes from multiple bound streams and feed FLOAT3/raw integer/normalized NORMAL/TANGENT/BINORMAL, FLOAT1-4 or normalized UBYTE4N BLENDWEIGHT, UBYTE4 or D3DCOLOR BLENDINDICES, sparse TEXCOORD1 and FLOAT1 TEXCOORD7 slots, and generic PSIZE/TESSFACTOR/FOG/DEPTH/SAMPLE slots, including packed D3DCOLOR, raw SHORT2, normalized UBYTE4N, and UDEC3 generic SAMPLE inputs to simple programmable shaders; FVF `D3DFVF_NORMAL`, `D3DFVF_SPECULAR` COLOR1, `D3DFVF_XYZB4` BLENDWEIGHT, and `D3DFVF_XYZB5 | D3DFVF_LASTBETA_UBYTE4` and `D3DFVF_LASTBETA_D3DCOLOR` BLENDINDICES are covered as programmable input sources, and programmable TEXCOORD input now decodes raw integer, normalized, half, D3DCOLOR, and UDEC3/DEC3N declaration data (`SHORT2`, `SHORT4`, `UBYTE4`, `SHORT2N`, `SHORT4N`, `USHORT2N`, `USHORT4N`, `UBYTE4N`, `UDEC3`, `DEC3N`, `FLOAT16_2`, `FLOAT16_4`, `D3DCOLOR`) into float registers. `SrcStart`/`DestIndex` placement, `D3DPV_DONOTCOPYDATA` acceptance, and `D3DRS_CLIPPING=FALSE` depth clamp are covered. A limited programmable VS path now executes simple DCL/DEFI/DEFB/DEF/MOV/basic arithmetic/comparison/DP/MAD/LRP/NRM/RCP/RSQ/FRC/ABS/DP2ADD/POW/CRS/SGN/SINCOS/EXP/LOG/LIT/DST/EXPP/LOGP/M3x2/M3x3/M3x4/M4x3/M4x4 bytecode plus 2D TEXLDL vertex texture sampling with sampler ADDRESSU/V WRAP/CLAMP/MIRROR/MIRRORONCE/BORDER, BORDERCOLOR, MAXMIPLEVEL/SetLOD mip-clamp handling, and P8/A8P8 palette-expanded backing over PE vertex shader float/integer/bool constants for `ProcessVertices`, including `_PARTIALPRECISION` destination-modifier acceptance and `_SATURATE` output clamping, D3DSPSM source modifiers, simple IF/IFC/ELSE/ENDIF over bool constants, REP/LOOP counts, LOOP `aL` initial/step source reads, MOVA per-component address-register source reads, source relative addressing through `a0`/`aL` including INPUT-register relative reads and matrix constant-base reads plus address-register component swizzles, temp and `D3DSPR_OUTPUT` destination relative addressing, BREAK/BREAKC, and CALL/CALLNZ/LABEL/RET and SETP/BREAKP predicate plus predicated ordinary/flow-control instruction guards, and programmable source `FLOAT4 POSITION` / `D3DFVF_XYZW` reads that preserve clip-space w before viewport projection. The PE conformance scaffold covers vs_1_1 implicit input and RASTOUT/ATTROUT/TEXCRDOUT output readback plus vs_3_0 DCL + M4x4/MOV, MAD/MOV, NRM/DP3/DP4/MUL/ADD vector math, SLT/SGE/MIN/MAX/LRP/CND/CMP compare-select math, RCP/RSQ/FRC/ABS/DP2ADD/POW/CRS/SGN/SINCOS/EXP/LOG/LIT/DST/EXPP/LOGP scalar-cross/transcendent math and M3x2 matrix math, `_PARTIALPRECISION`, `_SATURATE` output clamping, D3DSPSM source modifiers, DEFI/CONSTINT REP/LOOP counts, DEFB/CONSTBOOL IF branch selection through SetVertexShaderConstantB, LOOP `aL` initial/step source reads, MOVA per-component address-register source reads, source relative addressing through `a0`/`aL` including INPUT-register relative reads and matrix constant-base reads plus address-register component swizzles, temp and `D3DSPR_OUTPUT` destination relative addressing, directional/point (including range/attenuation)/spot fixed-function lighting with cone rejection/falloff plus normalized declaration NORMAL decode, `D3DRS_COLORVERTEX` `D3DMCS_COLOR1` diffuse, `D3DMCS_COLOR2` specular, `D3DMCS_COLOR1` ambient, and `D3DMCS_COLOR2` emissive material-source readbacks, and specular secondary-color readback, destination PSIZE FVF/declaration readbacks, fixed-function D3DVBF_3WEIGHTS D3DFVF_XYZB4 unindexed and D3DFVF_XYZB5 LASTBETA_UBYTE4 indexed vertex-blend readbacks, FVF TEX2 mixed TEXCOORDSIZE1/TEXCOORDSIZE3 fixed-function and programmable ProcessVertices readbacks, sparse TEXCOORD1 and FLOAT1 TEXCOORD7 declaration readbacks, split-stream PSIZE/TESSFACTOR/FOG/DEPTH/SAMPLE generic declaration readbacks plus packed D3DCOLOR, raw SHORT2, normalized UBYTE4N, and UDEC3 generic SAMPLE readbacks, `SHORT2`/`SHORT2N`/`USHORT2N`/`FLOAT16_2` and D3DCOLOR TEXCOORD declaration decode readbacks plus D3DCOLOR and packed SHORT2N/UBYTE4N/FLOAT16_2/UDEC3/DEC3N destination TEXCOORD packing readbacks plus `SHORT4`/`SHORT4N`/`USHORT4N`/`FLOAT16_4`/`UBYTE4`/`UBYTE4N`/`UDEC3`/`DEC3N` TEXCOORD FLOAT4 destination readbacks, `SHORT4`/`UBYTE4`/`UBYTE4N`/`UDEC3`/`DEC3N` NORMAL declaration decode readbacks, BREAK/BREAKC, CALL/CALLNZ/LABEL/RET, SETP/BREAKP predicate plus predicated ordinary/flow-control instruction guards, and simple IF/IFC/ELSE/ENDIF flow-control, NORMAL/TANGENT/BINORMAL/BLENDWEIGHT/BLENDINDICES-input arithmetic, including D3DCOLOR BLENDINDICES and normalized UBYTE4N BLENDWEIGHT declaration readbacks, FVF NORMAL/SPECULAR/XYZB programmable input arithmetic, FLOAT4 POSITION MOV paths for both declaration and FVF XYZW source layouts plus fixed-function and programmable SHORT4N POSITION declaration decode, and ProcessVertices stream-frequency readback with stream 0 `D3DSTREAMSOURCE_INDEXEDDATA` plus non-1 `D3DSTREAMSOURCE_INSTANCEDATA` source addressing. Remaining texture/sample opcodes beyond 2D TEXLDL vertex texture sampling with sampler ADDRESSU/V WRAP/CLAMP/MIRROR/MIRRORONCE/BORDER, BORDERCOLOR, MAXMIPLEVEL/SetLOD mip-clamp handling, and P8/A8P8 palette-expanded backing, remaining lighting edge cases beyond the current range/attenuation/cone/falloff/COLORVERTEX material-source set, broader partial-clipping edge cases beyond transformed point-drop plus line-list/strip and triangle-list/strip/fan clipping, broader multi-instance stream-frequency behavior beyond ProcessVertices' single logical invocation, and exotic generic declaration format/index combinations beyond the current float-vector/SHORT2/UBYTE4N/UDEC3/UBYTE4/D3DCOLOR and nonzero TESSFACTOR/FOG/DEPTH/SAMPLE usage-index readbacks remain deferred. `SetSoftwareVertexProcessing` now tracks the mutable PE shadow and a mixed-VP runtime SetSoftwareVertexProcessing(TRUE) FFP and programmable-VS DrawPrimitiveUP draws are pinned by render-target readback, and lighting-disabled fixed-function FVF XYZ/XYZ+PSIZE, FVF XYZB4 unindexed, XYZB5 LASTBETA_UBYTE4 indexed, and stream-0 declaration BLENDWEIGHT/BLENDINDICES vertex-blend DrawPrimitiveUP/DrawIndexedPrimitiveUP plus bound DrawPrimitive/DrawIndexedPrimitive, and split stream0 POSITION/COLOR + stream1 BLENDWEIGHT/BLENDINDICES bound DrawPrimitive/DrawIndexedPrimitive, lighting-enabled FVF XYZ+NORMAL, fixed-function stream-0 POSITION/COLOR declaration DrawPrimitiveUP/DrawIndexedPrimitiveUP/bound DrawPrimitive/bound DrawIndexedPrimitive plus fixed-function SHORT4N POSITION/COLOR declaration DrawPrimitiveUP, and simple programmable VS FVF, stream-0 POSITION/COLOR declaration UP plus SHORT4N POSITION/COLOR declaration UP, stream-0 POSITION/COLOR/TANGENT declaration fixed-function DrawPrimitiveUP plus programmable DrawPrimitiveUP and bound draw, split stream0 POSITION/COLOR + stream2 TANGENT bound draw/indexed draw, and split POSITION/COLOR declaration-stream bound draw plus stream1 INSTANCEDATA color draw and stream0 INDEXEDDATA two-instance, non-1 INSTANCEDATA divider, and fixed-function/programmable line-strip and strip/fan expansion DrawPrimitive/DrawIndexedPrimitive expansion calls can use the same CPU transform path by submitting transformed XYZRHW UP vertices with the vertex shader bypassed for the packet while preserving the user-visible FVF/declaration and VS binding for later draws, with GetFVF/GetVertexShader/GetPixelShader/GetStreamSource readback covering programmable fallback state restoration, pixel-shader state preservation, bidirectional FFP/programmable SWVP state-transition readbacks, and UP fallback stream0 restoration; indexed coverage includes nonzero `minVertex` and `BaseVertexIndex` ranges, and fixed-function UP non-indexed/indexed (INDEX16/INDEX32 for the simple indexed readback) and bound non-indexed/indexed (INDEX16/INDEX32 for the simple indexed readback) plus programmable VS UP non-indexed/indexed (INDEX16/INDEX32 for the simple indexed readback) and bound non-indexed/indexed (INDEX16/INDEX32 for the simple indexed readback) SWVP draws, including point, line-list/strip, and triangle-list/strip/fan primitive forms, are checked by A8R8G8B8 render-target covered probe-pixel readback, with bound ps_2_0 constant-color readbacks for FFP and programmable-VS SWVP DrawPrimitiveUP plus bound DrawPrimitive/DrawIndexedPrimitive. |
| `D3DSAMP_ELEMENTINDEX`, `D3DSAMP_DMAPOFFSET` | Deferred. Texture-array/displacement-map sampler semantics are not represented in the backend. |
| `D3DRS_MULTISAMPLEANTIALIAS` | Shadow/default only. MSAA attachment selection and per-draw `D3DRS_MULTISAMPLEMASK` are implemented; the legacy per-draw MSAA enable toggle is not. |
| `D3DRS_ANTIALIASEDLINEENABLE` | Accepted no-op/deferred; Metal has no D3D9-style AA line raster toggle. |
| Legacy fixed-function raster states | `D3DRS_SHADEMODE=FLAT`, `D3DRS_LASTPIXEL`, and enabled `D3DRS_WRAP0..15` cylindrical wrapping round-trip through state shadowing but do not change raster output. The corresponding unsupported caps remain clear where D3D9 exposes one. |
| Palette/P8 texture binding | Partial. PE palette methods still round-trip; 2D/cube/volume `D3DFMT_P8` and `D3DFMT_A8P8` texture caps are exposed; 2D/cube/volume locks keep palette-index texels while expanding through the current palette into an A8R8G8B8 backend texture for sampling. Palette changes skip locked subresources and unlock expands those index texels through the latest palette; `UpdateTexture` between matching palettized textures copies index/alpha shadow data and preserves the destination palette before re-expanding the destination backing. PE matching palettized `UpdateTexture` records are committed immediately and then re-expanded through the device-current palette, so CPU-visible ProcessVertices vertex-texture sampling and the following fixed-function/programmable draw observe the copied shadow without waiting for a later chunk boundary. A native Metal integration gate now expands D9C P8/A8P8 shadow texels, including matching UpdateTexture destination-palette re-expansion, and reads them back through a programmable ps_2_0 draw. A PE visual scaffold samples 2D P8 and A8P8 textures through fixed-function texture stage state and ps_2_0 `texld`, plus 1x1 P8/A8P8 cube textures through ps_2_0 `samplerCUBE` and 1x1x4 P8/A8P8 volume textures through ps_2_0 `sampler3D`, then checks the palette-expanded A8R8G8B8 pixels, including A8P8 texel alpha, level-1 `SetLOD` sampling, same-slot palette updates, current-palette index switches while the texture is bound, current-palette-before-bind sampling, SYSTEMMEM-to-DEFAULT palettized `UpdateTexture` destination sampling before and after a destination palette switch, all six cube faces, and four volume slices by render-target readback; `test_visual_process_vertices_xyzhw_policy` also checks P8/A8P8 palette-expanded backing through vertex-texture TEXLDL destination-vertex readback, including same-slot and current-index P8/A8P8 bound palette updates plus P8 current-palette-before-bind vertex sampler readback. `Format::A8P8` / `Format::P8` now map explicitly for metadata, pitch, and D3DFORMAT round-trip; direct core storage remains unsupported so all runtime sampling continues through the A8R8G8B8 backing path. |
| `SetConvolutionMonoKernel`, `ComposeRects` | Explicit `E_NOTIMPL`. |
| D3D9Ex DEFAULT-pool shared resources | Same-process create/open-existing uses opaque 32-bit provider tokens and imports the retained Metal backing into each destination device's pool. Real closable and cross-process Win32 handles still need IOSurface or `MTLSharedTextureHandle` export/import plus handle-lifetime transport. |
| Factory, reset, and host-display parity | Adapter/fullscreen/resource-type validation and exact HRESULT propagation remain incomplete. `GetAdapterMonitor` returns a stub monitor identity and raster status is a synthetic estimate rather than a WindowServer scanline signal; `D3DCAPS_READ_SCANLINE` remains clear. |
| Stub/default contracts | `SetDialogBoxMode`, `ValidateDevice`, `PreLoad`, non-AUTOGEN `GenerateMipSubLevels`, `AddDirtyBox`, `Set/GetClipStatus`, `Set/GetNPatchMode`, `DeletePatch`, SwapChain Ex present stats, GPU thread priority, and resource residency intentionally return Wine-compatible defaults/no-ops where dxmt9 has no backend feature to drive. |

Additional SWVP draw coverage now pins fixed-function
`D3DFVF_XYZ|D3DFVF_DIFFUSE|D3DFVF_TEX1` `DrawPrimitiveUP`,
`DrawIndexedPrimitiveUP`, bound `DrawPrimitive`, and bound
`DrawIndexedPrimitive` plus the same programmable VS TEXCOORD passthrough
matrix through fixed-function texture-stage sampling, with four 2D A8R8G8B8
quadrant texels checked by render-target readback after the CPU transform path.
The basic colored-triangle SWVP readback also covers `D3DFMT_INDEX32` for
fixed-function and programmable VS `DrawIndexedPrimitiveUP` and bound
`DrawIndexedPrimitive`, in addition to the existing `D3DFMT_INDEX16` indexed
paths.
A split stream0 POSITION + stream1 COLOR SWVP `DrawPrimitive`
readback also sets stream1 `D3DSTREAMSOURCE_INSTANCEDATA` and verifies that the
CPU transform path uses the first instance color in the rendered pixel; follow-up
`DrawPrimitive` and `DrawIndexedPrimitive` readbacks set stream0
`D3DSTREAMSOURCE_INDEXEDDATA | 2` to verify two-instance CPU expansion,
then `D3DSTREAMSOURCE_INDEXEDDATA | 3` with stream1
`D3DSTREAMSOURCE_INSTANCEDATA | 2` to verify non-1 instance-data divider
addressing and the third logical instance color; additional 2-triangle `D3DPT_TRIANGLESTRIP` and `D3DPT_TRIANGLEFAN`
readbacks verify instanced line-strip and strip/fan-style primitives are expanded to
independent line-list or triangle-list packets before submit.
The same SWVP readback block also samples D3DFMT_P8 and D3DFMT_A8P8 textures
through the active palette for fixed-function and programmable VS TEXCOORD
`DrawPrimitiveUP`, `DrawIndexedPrimitiveUP`, bound `DrawPrimitive`, and bound
`DrawIndexedPrimitive` paths, including A8P8 texel alpha in A8R8G8B8
render-target readback, and the fixed-function plus programmable VS SWVP
`DrawPrimitiveUP` paths also sample P8/A8P8 `SYSTEMMEM` sources copied into
`DEFAULT` textures by `UpdateTexture`, with fixed-function and programmable VS
SWVP `DrawPrimitiveUP` plus fixed-function and programmable VS bound indexed
SWVP re-sampling after a same-slot
destination palette update and current palette index switch.
Programmable SWVP draw readback also covers a `D3DFVF_XYZW|D3DFVF_DIFFUSE`
clip-space source through `DrawPrimitiveUP`, plus a partially viewport-clipped
clip-space triangle through bound `DrawPrimitive` and `DrawIndexedPrimitive`, so
source `w` preservation reaches the draw fallback, not only `ProcessVertices`
destination-buffer readback.
The SWVP draw fallback now clips transformed line-list/strip and triangle-list/strip/fan primitives against active viewport/depth/user clip planes while `D3DRS_CLIPPING` is enabled, generating `XYZRHW` vertices with interpolated float/color attributes before submit. Readbacks cover all-outside behind-eye, far-plane, and user clip-plane `D3DFVF_XYZW|D3DFVF_DIFFUSE` programmable `DrawPrimitive`, `DrawIndexedPrimitive`, `DrawPrimitiveUP`, and `DrawIndexedPrimitiveUP` no-op cases plus partially user-clip-plane-clipped triangle-list and line-list `DrawPrimitive`, `DrawIndexedPrimitive`, `DrawPrimitiveUP`, and `DrawIndexedPrimitiveUP`; broader exotic-attribute clipping edge cases remain deferred.

Broad conformance Wine-run sweeps remain ongoing. The tracked
`ProcessVertices` PE singleton (`visual_process_vertices_xyzhw_policy`) passed
the M4x4/MOV subset with 146 checks before the MAD/FLOAT4-POSITION expansion;
the latest stable app-local `visual_mvp_software_vp_policy` replay reaches
D3D with the Sikarugir ABI handshake OK and passes the expanded SWVP draw,
clipping, stream-frequency, pixel-shader, and texture readback sections.
Focused P8/A8P8 visual rerun is still pending after the pre-transformed
`D3DFVF_XYZRHW` half-pixel correction because the current conformance prefix
now fails `winemetal.dll`/`winemetal.so` attach before `d3d9.dll` loads. The
conformance runner stages `winemetal.so` next to the app-local builtin PE and
passes `DXMT9_WINEMETAL_SO`, but this Sikarugir build reports
`MemoryWineLoadUnixLibByName` as unsupported and the app-local builtin load does
not register a unixlib path for the base `MemoryWineLoadUnixLib` lookup.
Forcing `winemetal=b` still hits the copied prefix `system32/winemetal.dll`
without a unixlib path, and removing both the app-local and prefix copies leaves
Wine unable to resolve `winemetal.dll` from the packaged builtin directory. Do
not treat the PE P8 visual scaffold as a fresh Wine-run pass until that runtime
path is repaired. Native Metal integration now covers P8/A8P8 D9C palette
expansion and matching UpdateTexture destination-palette re-expansion through
programmable ps_2_0 draw/readback while that Wine path is blocked.
The scaffold now also covers `D3DFVF_PSIZE` as a programmable source input,
separate from declaration PSIZE generic input and destination PSIZE writeback.
Destination declaration `COLOR1`/specular readback is covered for both
fixed-function passthrough and programmable shader output.
The formerly
deferred GPU-runtime pixel validations for RESZ MSAA→INTZ readback, NULL RT
depth-only rendering, and MIPMAPLODBIAS mip selection are covered by
shader-corpus readback probes.

### Resolved as documented-unsupported / non-gap (no implementation planned)

| Item | Section | Decision |
|---|---|---|
| `D3DFMT_NVDB` depth-bounds test | C.5 | **Unsupported — implemented + tested 2026-05-24.** Metal has no depth-bounds test; classified `Format::Nvdb`, `CheckDeviceFormat`→`NOTAVAILABLE`. R-FORMAT-14; gate `dxmt9-core-format-caps-spec`. |
| `D3DRS_WRAP0..15` texcoord wrap | B.10#6 | **Accepted no-op — implemented + tested 2026-05-24.** `RS_WRAP0..15` constants defined + GetRenderState round-trip; enabled cylindrical-wrap is the documented limitation. R-CORE-3.9; gate `dxmt9-state-draw-transform-spec`. |
| `D3DFMT_RAWZ` legacy raw depth | C.5 | **Unsupported — implemented + tested 2026-05-24.** Classified `Format::Rawz`, `CheckDeviceFormat`→`NOTAVAILABLE`; gate `dxmt9-core-format-caps-spec`. |
| `D3DRS_DEBUGMONITORTOKEN` (165) | B.10#9 | **Non-gap** — PE shadow already stores all RS slots ≤255 generically; a named constant adds nothing functional. |

## Cross-cutting current findings

> This section is the current short list. The detailed A/B/C/D matrices below
> still preserve original audit rows and may show stale `❌` markers; prefer
> this list plus the re-audit delta above when triaging new work.

Items below are surfaced from the four parts as deserving a dedicated track
after the silent-coverage fixes landed.

| Finding | Section | Current status / suggested track |
|---|---|---|
| N-patch / adaptive tessellation family | A.5/B.1/D.* | Deferred/unsupported. Non-default declaration methods safe-reject because they describe fixed-function tessellator/N-patch/displacement-map evaluation, not ordinary vertex attributes; patch draw calls return `D3DERR_INVALIDCALL`; N-patch mode is a documented no-op/default contract. |
| `ProcessVertices` / SWVP execution | D.4 | Partial. Fixed-function WORLD/VIEW/PROJECTION XYZ→XYZRHW CPU transform/readback is implemented for FVF and simple POSITION/COLOR/TEXCOORD source declarations, including SHORT4N POSITION decode, with matching diffuse/specular/texcoord passthrough into FVF or POSITIONT/COLOR/TEXCOORD declaration destinations plus destination `D3DFVF_PSIZE`/`D3DDECLUSAGE_PSIZE` readbacks; fixed-function `D3DVBF_3WEIGHTS` `D3DFVF_XYZB4` unindexed and `D3DFVF_XYZB5 | D3DFVF_LASTBETA_UBYTE4` indexed vertex-blend readbacks plus FVF TEX2 mixed `TEXCOORDSIZE1`/`TEXCOORDSIZE3` readbacks are covered for fixed-function and programmable `ProcessVertices`; fixed-function directional/point (including Range and Attenuation0/1/2)/spot-light diffuse output with spot cone rejection, `D3DRS_COLORVERTEX` `D3DMCS_COLOR1` diffuse, `D3DMCS_COLOR2` specular, `D3DMCS_COLOR1` ambient, and `D3DMCS_COLOR2` emissive material-source output, and specular secondary-color output are covered for FLOAT3/raw/normalized NORMAL input and material-driven lighting; source declarations may split supported attributes across bound streams and feed FLOAT3/raw integer/normalized NORMAL/TANGENT/BINORMAL, BLENDWEIGHT, BLENDINDICES, sparse TEXCOORD1 and FLOAT1 TEXCOORD7 slots, and PSIZE/TESSFACTOR/FOG/DEPTH/SAMPLE generic slots plus packed D3DCOLOR, raw SHORT2, normalized UBYTE4N, and UDEC3 generic SAMPLE inputs into simple programmable shaders, FVF `D3DFVF_NORMAL`, `D3DFVF_SPECULAR` COLOR1, `D3DFVF_XYZB4` BLENDWEIGHT, and `D3DFVF_XYZB5 | D3DFVF_LASTBETA_UBYTE4` and `D3DFVF_LASTBETA_D3DCOLOR` BLENDINDICES are covered as programmable input sources, and raw integer, normalized, half, D3DCOLOR, UDEC3, and DEC3N TEXCOORD declaration data decodes into programmable float inputs, including FLOAT4 TEXCOORD destination readback for 4-component raw integer, normalized/half, D3DCOLOR, UDEC3, and DEC3N inputs. `SrcStart`/`DestIndex` placement, `D3DPV_DONOTCOPYDATA` acceptance, `D3DRS_CLIPPING=FALSE` depth clamp, and source stream-frequency readback for stream 0 `D3DSTREAMSOURCE_INDEXEDDATA` plus non-1 `D3DSTREAMSOURCE_INSTANCEDATA` are covered. Limited real SWVP draw execution now covers lighting-disabled fixed-function FVF XYZ/XYZ+PSIZE, FVF XYZB4 unindexed, XYZB5 LASTBETA_UBYTE4 indexed, and stream-0 declaration BLENDWEIGHT/BLENDINDICES vertex-blend DrawPrimitiveUP/DrawIndexedPrimitiveUP plus bound DrawPrimitive/DrawIndexedPrimitive, and split stream0 POSITION/COLOR + stream1 BLENDWEIGHT/BLENDINDICES bound DrawPrimitive/DrawIndexedPrimitive, lighting-enabled FVF XYZ+NORMAL, fixed-function stream-0 POSITION/COLOR declaration DrawPrimitiveUP/DrawIndexedPrimitiveUP/bound DrawPrimitive/bound DrawIndexedPrimitive plus fixed-function SHORT4N POSITION/COLOR declaration DrawPrimitiveUP, and simple programmable VS FVF, stream-0 POSITION/COLOR declaration UP plus SHORT4N POSITION/COLOR declaration UP, stream-0 POSITION/COLOR/TANGENT declaration fixed-function DrawPrimitiveUP plus programmable DrawPrimitiveUP and bound draw, split stream0 POSITION/COLOR + stream2 TANGENT bound draw/indexed draws, and split POSITION/COLOR declaration-stream bound draw plus stream1 INSTANCEDATA color draw and stream0 INDEXEDDATA two-instance, non-1 INSTANCEDATA divider, and fixed-function/programmable line-strip and strip/fan expansion DrawPrimitive/DrawIndexedPrimitive expansions by CPU-transforming through the ProcessVertices path into transformed XYZRHW UP vertices before submit and bypassing the packet vertex shader so the VS is not executed twice; indexed coverage includes nonzero `minVertex` and `BaseVertexIndex` ranges, and fixed-function UP non-indexed/indexed (INDEX16/INDEX32 for the simple indexed readback) and bound non-indexed/indexed (INDEX16/INDEX32 for the simple indexed readback) plus programmable VS UP non-indexed/indexed (INDEX16/INDEX32 for the simple indexed readback) and bound non-indexed/indexed (INDEX16/INDEX32 for the simple indexed readback) SWVP draws, including point, line-list/strip, and triangle-list/strip/fan primitive forms, are checked by A8R8G8B8 render-target covered probe-pixel readback, with bound ps_2_0 constant-color readbacks for FFP and programmable-VS SWVP DrawPrimitiveUP plus bound DrawPrimitive/DrawIndexedPrimitive. Limited programmable VS execution covers simple DCL/DEFI/DEFB/DEF/MOV/basic arithmetic/comparison/DP/MAD/LRP/NRM/RCP/RSQ/FRC/ABS/DP2ADD/POW/CRS/SGN/SINCOS/EXP/LOG/LIT/DST/EXPP/LOGP/M3x2/M3x3/M3x4/M4x3/M4x4 bytecode plus 2D TEXLDL vertex texture sampling with sampler ADDRESSU/V WRAP/CLAMP/MIRROR/MIRRORONCE/BORDER, BORDERCOLOR, MAXMIPLEVEL/SetLOD mip-clamp handling, and P8/A8P8 palette-expanded backing and `_PARTIALPRECISION` destination-modifier acceptance and `_SATURATE` output clamping over PE vertex shader float/integer/bool constants, including programmable PSIZE output, plus programmable `FLOAT4 POSITION` / `D3DFVF_XYZW` source w preservation and SHORT4N POSITION declaration decode on fixed-function and programmable paths, LOOP `aL` initial/step source reads, MOVA per-component address-register source reads, source relative addressing through `a0`/`aL` including INPUT-register relative reads and matrix constant-base reads plus address-register component swizzles, and temp and `D3DSPR_OUTPUT` destination relative addressing. The targeted PE Wine singleton passed before the MAD/FLOAT4-POSITION/NORMAL/TANGENT/BINORMAL/BLENDWEIGHT/BLENDINDICES-input expansion (`visual_process_vertices_xyzhw_policy`, 146 checks); the latest stable app-local `visual_mvp_software_vp_policy` singleton reaches D3D with the ABI handshake OK and passes the expanded SWVP draw, clipping, stream-frequency, pixel-shader, and texture readback sections. Remaining texture/sample opcodes beyond 2D TEXLDL vertex texture sampling with sampler ADDRESSU/V WRAP/CLAMP/MIRROR/MIRRORONCE/BORDER, BORDERCOLOR, MAXMIPLEVEL/SetLOD mip-clamp handling, and P8/A8P8 palette-expanded backing, remaining lighting edge cases beyond the current range/attenuation/cone/falloff/COLORVERTEX material-source set, broader partial-clipping edge cases beyond transformed point-drop plus line-list/strip and triangle-list/strip/fan clipping, broader multi-instance stream-frequency behavior beyond ProcessVertices' single logical invocation, and exotic generic declaration format/index combinations beyond the current float-vector/SHORT2/UBYTE4N/UDEC3/UBYTE4/D3DCOLOR and nonzero TESSFACTOR/FOG/DEPTH/SAMPLE usage-index readbacks remain deferred. |
| `D3DSAMP_ELEMENTINDEX`, `D3DSAMP_DMAPOFFSET` | B.3 | Deferred. Texture-array index and displacement-map sampler semantics are not wired. |
| `D3DRS_MULTISAMPLEANTIALIAS` | B.1 | Shadow/default only. Attachment MSAA and `D3DRS_MULTISAMPLEMASK` fragment output are implemented. |
| `D3DRS_ANTIALIASEDLINEENABLE` | B.1 | Accepted no-op/deferred; Metal exposes no equivalent D3D9 AA-line raster mode. |
| Palette/P8 sampling | D.4/C.2 | Partial. 2D/cube/volume P8/A8P8 texture caps are exposed; 2D/cube/volume locks keep index data (plus texel alpha for A8P8) and expand through the current palette into an A8R8G8B8 backing texture; palette switches during a lock defer backend upload until unlock and then use the latest palette; `UpdateTexture` copies matching palettized source index/alpha shadow data while preserving the destination palette, and PE commits matching palettized `UpdateTexture` records immediately before re-expanding the destination through the device-current palette for immediate CPU/draw visibility; Native `dxmt9-core-device-com-spec` now directly samples P8/A8P8 palette-expanded backing through `dxmt9c_texture_sample_2d`, including locked-palette-switch and UpdateTexture destination palette-switch samples, and checks native cube/volume P8/A8P8 backend expansion uploads with A8P8 texel alpha preserved; 2D P8 and A8P8 fixed-function and ps_2_0 `texld` sampling have PE render-target readback scaffolds, including A8P8 texel alpha, level-1 `SetLOD`, same-slot palette updates, current-palette index switches while the texture is bound, current-palette-before-bind sampling, and SYSTEMMEM-to-DEFAULT `UpdateTexture` destination sampling before and after a destination palette switch, and P8/A8P8 ProcessVertices vertex-texture TEXLDL has destination-vertex readback coverage, including same-slot and current-index P8/A8P8 bound palette updates plus P8 current-palette-before-bind vertex sampler readback; P8/A8P8 cube textures have ps_2_0 `samplerCUBE` readback coverage across all six faces, and P8/A8P8 volume textures have ps_2_0 `sampler3D` readback coverage across four slices; `Format::A8P8` / `Format::P8` are explicit metadata formats but direct core storage is unsupported. |
| `SetConvolutionMonoKernel`, `ComposeRects` | D.5 | Explicit `E_NOTIMPL`. |
| Stub/default COM contracts | D.* | `SetDialogBoxMode`, `ValidateDevice`, `PreLoad`, non-AUTOGEN `GenerateMipSubLevels`, `AddDirtyBox`, `Set/GetClipStatus`, `Set/GetNPatchMode`, `DeletePatch`, SwapChain Ex present stats, GPU thread priority, and resource residency intentionally expose Wine-compatible defaults/no-ops instead of backend behavior. |
| GPU-runtime validations | C.5/B.3 | Closed for the tracked cases: RESZ MSAA→INTZ readback, NULL RT depth-only rendering, and MIPMAPLODBIAS mip selection are covered by shader-corpus readback probes. |

Additional SWVP draw coverage now pins fixed-function
`D3DFVF_XYZ|D3DFVF_DIFFUSE|D3DFVF_TEX1` `DrawPrimitiveUP`,
`DrawIndexedPrimitiveUP`, bound `DrawPrimitive`, and bound
`DrawIndexedPrimitive` plus the same programmable VS TEXCOORD passthrough
matrix through fixed-function texture-stage sampling, with four 2D A8R8G8B8
quadrant texels checked by render-target readback after the CPU transform path.
The basic colored-triangle SWVP readback also covers `D3DFMT_INDEX32` for
fixed-function and programmable VS `DrawIndexedPrimitiveUP` and bound
`DrawIndexedPrimitive`, in addition to the existing `D3DFMT_INDEX16` indexed
paths.
A split stream0 POSITION + stream1 COLOR SWVP `DrawPrimitive`
readback also sets stream1 `D3DSTREAMSOURCE_INSTANCEDATA` and verifies that the
CPU transform path uses the first instance color in the rendered pixel; follow-up
`DrawPrimitive` and `DrawIndexedPrimitive` readbacks set stream0
`D3DSTREAMSOURCE_INDEXEDDATA | 2` to verify two-instance CPU expansion,
then `D3DSTREAMSOURCE_INDEXEDDATA | 3` with stream1
`D3DSTREAMSOURCE_INSTANCEDATA | 2` to verify non-1 instance-data divider
addressing and the third logical instance color; additional 2-triangle `D3DPT_TRIANGLESTRIP` and `D3DPT_TRIANGLEFAN`
readbacks verify instanced line-strip and strip/fan-style primitives are expanded to
independent line-list or triangle-list packets before submit.
The same SWVP readback block also samples D3DFMT_P8 and D3DFMT_A8P8 textures
through the active palette for fixed-function and programmable VS TEXCOORD
`DrawPrimitiveUP`, `DrawIndexedPrimitiveUP`, bound `DrawPrimitive`, and bound
`DrawIndexedPrimitive` paths, including A8P8 texel alpha in A8R8G8B8
render-target readback.
Programmable SWVP draw readback also covers a `D3DFVF_XYZW|D3DFVF_DIFFUSE`
clip-space source through `DrawPrimitiveUP`, plus partially clipped bound
`DrawPrimitive`/`DrawIndexedPrimitive` cases, so source `w` preservation and
generated-vertex clipping reach the draw fallback, not only `ProcessVertices`
destination-buffer readback.
The SWVP draw fallback now clips transformed line-list/strip and triangle-list/strip/fan primitives
against viewport/depth/user clip planes and drops fully outside primitives while
`D3DRS_CLIPPING` is enabled, including behind-eye (`rhw <= 0`), far-plane,
user clip-plane `D3DFVF_XYZW|D3DFVF_DIFFUSE` no-op readbacks, partially
user-clip-plane-clipped triangle-list/line-list bound and UP readbacks, and
point-list outside-drop readbacks across bound and UP paths; broader partial-clipping edge cases remain
deferred.

---

## A. Shader Bytecode + Vertex Declaration

### A.1 D3DSIO opcodes

| Opcode | Code | Status | Source anchor | Test anchor | Notes |
|---|---|---|---|---|---|
| NOP | 0 | ✅ | dxmt9_d3d9_bytecode.hpp:79; dxmt9_shader_metal_ir.cpp:1349, 2391 | opcode_audit_spec.cpp:62 | emitted as comment |
| MOV | 1 | ✅ | dxmt9_d3d9_bytecode.hpp:80; dxmt9_shader_metal_ir.cpp:1351, 2435 | opcode_audit_spec.cpp:63 | |
| ADD | 2 | ✅ | dxmt9_d3d9_bytecode.hpp:81; dxmt9_shader_metal_ir.cpp:1443, 2521 | opcode_audit_spec.cpp:64 | |
| SUB | 3 | ✅ | dxmt9_d3d9_bytecode.hpp:82; dxmt9_shader_metal_ir.cpp:1444, 2522 | opcode_audit_spec.cpp:65 | |
| MAD | 4 | ✅ | dxmt9_d3d9_bytecode.hpp:83; dxmt9_shader_metal_ir.cpp:1446, 2524 | opcode_audit_spec.cpp:66 | |
| MUL | 5 | ✅ | dxmt9_d3d9_bytecode.hpp:84; dxmt9_shader_metal_ir.cpp:1445, 2523 | opcode_audit_spec.cpp:67 | |
| RCP | 6 | ✅ | dxmt9_d3d9_bytecode.hpp:85; dxmt9_shader_metal_ir.cpp:1463, 2541 | opcode_audit_spec.cpp:68 | |
| RSQ | 7 | ✅ | dxmt9_d3d9_bytecode.hpp:86; dxmt9_shader_metal_ir.cpp:1464, 2542 | opcode_audit_spec.cpp:69 | |
| DP3 | 8 | ✅ | dxmt9_d3d9_bytecode.hpp:87; dxmt9_shader_metal_ir.cpp:1467, 2545 | opcode_audit_spec.cpp:70 | |
| DP4 | 9 | ✅ | dxmt9_d3d9_bytecode.hpp:88; dxmt9_shader_metal_ir.cpp:1468, 2546 | opcode_audit_spec.cpp:71 | |
| MIN | 10 | ✅ | dxmt9_d3d9_bytecode.hpp:89; dxmt9_shader_metal_ir.cpp:1447, 2525 | opcode_audit_spec.cpp:72 | |
| MAX | 11 | ✅ | dxmt9_d3d9_bytecode.hpp:90; dxmt9_shader_metal_ir.cpp:1448, 2526 | opcode_audit_spec.cpp:73 | |
| SLT | 12 | ✅ | dxmt9_d3d9_bytecode.hpp:91; dxmt9_shader_metal_ir.cpp:1449, 2527 | opcode_audit_spec.cpp:74 | |
| SGE | 13 | ✅ | dxmt9_d3d9_bytecode.hpp:92; dxmt9_shader_metal_ir.cpp:1450, 2528 | opcode_audit_spec.cpp:75 | |
| EXP | 14 | ✅ | dxmt9_d3d9_bytecode.hpp:93; dxmt9_shader_metal_ir.cpp:1451, 2529 | opcode_audit_spec.cpp:76 | |
| LOG | 15 | ✅ | dxmt9_d3d9_bytecode.hpp:94; dxmt9_shader_metal_ir.cpp:1452, 2530 | opcode_audit_spec.cpp:77 | |
| LIT | 16 | ✅ | dxmt9_d3d9_bytecode.hpp:95; dxmt9_shader_metal_ir.cpp:1453, 2531 | shader_transform_spec.cpp (via opcode_audit) | covered indirectly |
| DST | 17 | ✅ | dxmt9_d3d9_bytecode.hpp:96; dxmt9_shader_metal_ir.cpp:1454, 2532 | shader_source_determinism_spec.cpp / opcode_audit_spec.cpp (table) | dedicated lowering case |
| LRP | 18 | ✅ | dxmt9_d3d9_bytecode.hpp:97; dxmt9_shader_metal_ir.cpp:1466, 2544 | opcode_audit_spec.cpp:78 | |
| FRC | 19 | ✅ | dxmt9_d3d9_bytecode.hpp:98; dxmt9_shader_metal_ir.cpp:1465, 2543 | opcode_audit_spec.cpp:79 | |
| M4x4 | 20 | ✅ | dxmt9_d3d9_bytecode.hpp:99; dxmt9_shader_metal_ir.cpp:1458, 2536 | opcode_audit_spec.cpp:80 | |
| M4x3 | 21 | ✅ | dxmt9_d3d9_bytecode.hpp:100; dxmt9_shader_metal_ir.cpp:1459, 2537 | opcode_audit_spec.cpp:81 | |
| M3x4 | 22 | ✅ | dxmt9_d3d9_bytecode.hpp:101; dxmt9_shader_metal_ir.cpp:1460, 2538 | opcode_audit_spec.cpp:82 | |
| M3x3 | 23 | ✅ | dxmt9_d3d9_bytecode.hpp:102; dxmt9_shader_metal_ir.cpp:1461, 2539 | opcode_audit_spec.cpp:83 | |
| M3x2 | 24 | ✅ | dxmt9_d3d9_bytecode.hpp:103; dxmt9_shader_metal_ir.cpp:1462, 2540 | opcode_audit_spec.cpp:84 | |
| CALL | 25 | ✅ | dxmt9_d3d9_bytecode.hpp:104; dxmt9_shader_metal_ir.cpp:1195, 2241 | opcode_audit_spec.cpp:85 | inlined via internal markers |
| CALLNZ | 26 | ✅ | dxmt9_d3d9_bytecode.hpp:105; dxmt9_shader_metal_ir.cpp:1204, 2250 | opcode_audit_spec.cpp:86 | |
| LOOP | 27 | ⚠️ | dxmt9_d3d9_bytecode.hpp:106; dxmt9_shader_metal_ir.cpp:1280, 2322 | opcode_audit_spec.cpp:87 | audit marks operand count `kUnsupportedFixedOperandCount` |
| RET | 28 | ✅ | dxmt9_d3d9_bytecode.hpp:107; dxmt9_shader_metal_ir.cpp:1214, 2260 | opcode_audit_spec.cpp:88 | |
| ENDLOOP | 29 | ✅ | dxmt9_d3d9_bytecode.hpp:108; dxmt9_shader_metal_ir.cpp:1303, 2345 | opcode_audit_spec.cpp:89 | |
| LABEL | 30 | 🔵 | dxmt9_d3d9_bytecode.hpp:109; dxmt9_shader_metal_ir.cpp:1188, 2234 | opcode_audit_spec.cpp:90; shader_bytecode_validation_spec.cpp:254 | label opcode is inlined, but `D3DSPR_LABEL` reg kind is rejected (DecoderRejectReason::LabelUnsupported) |
| DCL | 31 | ✅ | dxmt9_d3d9_bytecode.hpp:110; dxmt9_shader_metal_ir.cpp:786, 832, 1775 | opcode_audit_spec.cpp:91; core_shader_translator_spec.cpp:250 | |
| POW | 32 | ✅ | dxmt9_d3d9_bytecode.hpp:111; dxmt9_shader_metal_ir.cpp:1472, 2550 | opcode_audit_spec.cpp:92 | |
| CRS | 33 | ✅ | dxmt9_d3d9_bytecode.hpp:112; dxmt9_shader_metal_ir.cpp:1473, 2551 | opcode_audit_spec.cpp:93 | |
| SGN | 34 | ✅ | dxmt9_d3d9_bytecode.hpp:113; dxmt9_shader_metal_ir.cpp:1474, 2552 | opcode_audit_spec.cpp:94 | |
| ABS | 35 | ✅ | dxmt9_d3d9_bytecode.hpp:114; dxmt9_shader_metal_ir.cpp:1475, 2553 | opcode_audit_spec.cpp:95 | |
| NRM | 36 | ✅ | dxmt9_d3d9_bytecode.hpp:115; dxmt9_shader_metal_ir.cpp:1476, 2554 | opcode_audit_spec.cpp:96 | |
| SINCOS | 37 | ⚠️ | dxmt9_d3d9_bytecode.hpp:116; dxmt9_shader_metal_ir.cpp:1457, 2535 | opcode_audit_spec.cpp:97 | audit marks operand count `kUnsupportedFixedOperandCount` |
| REP | 38 | ⚠️ | dxmt9_d3d9_bytecode.hpp:117; dxmt9_shader_metal_ir.cpp:1280, 2322 | opcode_audit_spec.cpp:98 | shares loop control flow, operand count marked unsupported in audit |
| ENDREP | 39 | ✅ | dxmt9_d3d9_bytecode.hpp:118; dxmt9_shader_metal_ir.cpp:1303, 2345 | opcode_audit_spec.cpp:99 | |
| IF | 40 | ✅ | dxmt9_d3d9_bytecode.hpp:119; dxmt9_shader_metal_ir.cpp:1228, 2274 | opcode_audit_spec.cpp:100 | |
| IFC | 41 | ✅ | dxmt9_d3d9_bytecode.hpp:120; dxmt9_shader_metal_ir.cpp:1237, 2283 | opcode_audit_spec.cpp:101 | |
| ELSE | 42 | ✅ | dxmt9_d3d9_bytecode.hpp:121; dxmt9_shader_metal_ir.cpp:1260, 2302 | opcode_audit_spec.cpp:102 | |
| ENDIF | 43 | ✅ | dxmt9_d3d9_bytecode.hpp:122; dxmt9_shader_metal_ir.cpp:1272, 2314 | opcode_audit_spec.cpp:103 | |
| BREAK | 44 | ✅ | dxmt9_d3d9_bytecode.hpp:123; dxmt9_shader_metal_ir.cpp:1313, 2355 | opcode_audit_spec.cpp:104 | |
| BREAKC | 45 | ✅ | dxmt9_d3d9_bytecode.hpp:124; dxmt9_shader_metal_ir.cpp:1329, 2371 | opcode_audit_spec.cpp:105 | |
| MOVA | 46 | ✅ | dxmt9_d3d9_bytecode.hpp:125; dxmt9_shader_metal_ir.cpp:1410, 2468 | opcode_audit_spec.cpp:106 | |
| DEFB | 47 | ✅ | dxmt9_d3d9_bytecode.hpp:126; dxmt9_shader_metal_ir.cpp:1762, 2425 | opcode_audit_spec.cpp:107 | |
| DEFI | 48 | ✅ | dxmt9_d3d9_bytecode.hpp:127; dxmt9_shader_metal_ir.cpp:1746, 2412 | opcode_audit_spec.cpp:108 | |
| (49..63) | — | — | — | — | unused in D3D9 spec |
| TEXCOORD | 64 | ✅ | dxmt9_d3d9_bytecode.hpp:128; dxmt9_shader_metal_ir.cpp:1778, 2813 | opcode_audit_spec.cpp:109 | SM1 legacy lowering |
| TEXKILL | 65 | ✅ | dxmt9_d3d9_bytecode.hpp:129; dxmt9_shader_metal_ir.cpp:1441, 2499 | opcode_audit_spec.cpp:110 | |
| TEX | 66 | ✅ | dxmt9_d3d9_bytecode.hpp:130; dxmt9_shader_metal_ir.cpp:1477, 2555 | opcode_audit_spec.cpp:111 | |
| TEXBEM | 67 | ✅ | dxmt9_d3d9_bytecode.hpp:131; dxmt9_shader_metal_ir.cpp:1779, 2825 | opcode_audit_spec.cpp:112 | |
| TEXBEML | 68 | ✅ | dxmt9_d3d9_bytecode.hpp:132; dxmt9_shader_metal_ir.cpp:1780, 2826 | opcode_audit_spec.cpp:113 | |
| TEXREG2AR | 69 | ✅ | dxmt9_d3d9_bytecode.hpp:133; dxmt9_shader_metal_ir.cpp:1781, 2837 | opcode_audit_spec.cpp:114 | |
| TEXREG2GB | 70 | ✅ | dxmt9_d3d9_bytecode.hpp:134; dxmt9_shader_metal_ir.cpp:1782, 2843 | opcode_audit_spec.cpp:115 | |
| TEXM3x2PAD | 71 | ✅ | dxmt9_d3d9_bytecode.hpp:135; dxmt9_shader_metal_ir.cpp:1783, 2849 | opcode_audit_spec.cpp:116 | |
| TEXM3x2TEX | 72 | ✅ | dxmt9_d3d9_bytecode.hpp:136; dxmt9_shader_metal_ir.cpp:1784, 2852 | opcode_audit_spec.cpp:117 | |
| TEXM3x3PAD | 73 | ✅ | dxmt9_d3d9_bytecode.hpp:137; dxmt9_shader_metal_ir.cpp:1785, 2858 | opcode_audit_spec.cpp:118 | |
| TEXM3x3TEX | 74 | ✅ | dxmt9_d3d9_bytecode.hpp:138; dxmt9_shader_metal_ir.cpp:1786, 2863 | opcode_audit_spec.cpp:119 | |
| TEXM3x3DIFF | 75 | ✅ | dxmt9_d3d9_bytecode.hpp:139; dxmt9_shader_metal_ir.cpp:1787, 2870 | opcode_audit_spec.cpp:120 (+ dedicated unsupported-pair case at :217) | |
| TEXM3x3SPEC | 76 | ✅ | dxmt9_d3d9_bytecode.hpp:140; dxmt9_shader_metal_ir.cpp:1788, 2872 | opcode_audit_spec.cpp:121 | |
| TEXM3x3VSPEC | 77 | ✅ | dxmt9_d3d9_bytecode.hpp:141; dxmt9_shader_metal_ir.cpp:1789, 2881 | opcode_audit_spec.cpp:122 | |
| EXPP | 78 | ✅ | dxmt9_d3d9_bytecode.hpp:142; dxmt9_shader_metal_ir.cpp:1455, 2533 | opcode_audit_spec.cpp:123 | |
| LOGP | 79 | ✅ | dxmt9_d3d9_bytecode.hpp:143; dxmt9_shader_metal_ir.cpp:1456, 2534 | opcode_audit_spec.cpp:124 | |
| CND | 80 | ✅ | dxmt9_d3d9_bytecode.hpp:144; dxmt9_shader_metal_ir.cpp:1469, 2547 | opcode_audit_spec.cpp:125 | |
| DEF | 81 | ✅ | dxmt9_d3d9_bytecode.hpp:145; dxmt9_shader_metal_ir.cpp:1724, 2393 | opcode_audit_spec.cpp:126 | |
| TEXREG2RGB | 82 | ✅ | dxmt9_d3d9_bytecode.hpp:146; dxmt9_shader_metal_ir.cpp:1792, 2902 | opcode_audit_spec.cpp:127 | |
| TEXDP3TEX | 83 | ✅ | dxmt9_d3d9_bytecode.hpp:147; dxmt9_shader_metal_ir.cpp:1793, 2907 | opcode_audit_spec.cpp:128 | |
| TEXM3x2DEPTH | 84 | ✅ | dxmt9_d3d9_bytecode.hpp:148; dxmt9_shader_metal_ir.cpp:1794, 2912 | opcode_audit_spec.cpp:129 | |
| TEXDP3 | 85 | ✅ | dxmt9_d3d9_bytecode.hpp:149; dxmt9_shader_metal_ir.cpp:1795, 2916 | opcode_audit_spec.cpp:130 | |
| TEXM3x3 | 86 | ✅ | dxmt9_d3d9_bytecode.hpp:150; dxmt9_shader_metal_ir.cpp:1796, 2919 | opcode_audit_spec.cpp:131 | |
| TEXDEPTH | 87 | ✅ | dxmt9_d3d9_bytecode.hpp:151; dxmt9_shader_metal_ir.cpp:1791, 2898 | opcode_audit_spec.cpp:132 | |
| CMP | 88 | ✅ | dxmt9_d3d9_bytecode.hpp:152; dxmt9_shader_metal_ir.cpp:1470, 2548 | opcode_audit_spec.cpp:133 | |
| BEM | 89 | ✅ | dxmt9_d3d9_bytecode.hpp:153; dxmt9_shader_metal_ir.cpp:1790, 2893 | opcode_audit_spec.cpp:134 | |
| DP2ADD | 90 | ✅ | dxmt9_d3d9_bytecode.hpp:154; dxmt9_shader_metal_ir.cpp:1471, 2549 | opcode_audit_spec.cpp:135 | |
| DSX | 91 | ✅ | dxmt9_d3d9_bytecode.hpp:155; dxmt9_shader_metal_ir.cpp:1478, 2556 | opcode_audit_spec.cpp:136 | |
| DSY | 92 | ✅ | dxmt9_d3d9_bytecode.hpp:156; dxmt9_shader_metal_ir.cpp:1479, 2557 | opcode_audit_spec.cpp:137 | |
| TEXLDD | 93 | ✅ | dxmt9_d3d9_bytecode.hpp:157; dxmt9_shader_metal_ir.cpp:1480, 2558 | opcode_audit_spec.cpp:138; shader_transform_spec.cpp:1284 | |
| SETP | 94 | ✅ | dxmt9_d3d9_bytecode.hpp:158; dxmt9_shader_metal_ir.cpp:1429, 2487 | opcode_audit_spec.cpp:139 | |
| TEXLDL | 95 | ✅ | dxmt9_d3d9_bytecode.hpp:159; dxmt9_shader_metal_ir.cpp:1481, 2559 | opcode_audit_spec.cpp:140 | |
| BREAKP | 96 | ✅ | dxmt9_d3d9_bytecode.hpp:160; dxmt9_shader_metal_ir.cpp:1322, 2364 | opcode_audit_spec.cpp:141 | |
| PHASE | 0xFFFD | ✅ | dxmt9_d3d9_bytecode.hpp:161; dxmt9_shader_metal_ir.cpp:1037, 2031 | opcode_audit_spec.cpp:142 | filtered out (legacy PS1.4 phase boundary) |
| COMMENT | 0xFFFE | ✅ | dxmt9_d3d9_bytecode.hpp:162; dxmt9_shader_metal_ir.cpp:1037, 2031 | opcode_audit_spec.cpp:143 | filtered out |
| END | 0xFFFF | ✅ | dxmt9_d3d9_bytecode.hpp:163; dxmt9_shader_decoder.cpp:440 | opcode_audit_spec.cpp:144; shader_bytecode_validation_spec.cpp:119 | terminator |

### A.2 D3DSPR register kinds

| Kind | Code | Status | Source anchor | Test anchor | Notes |
|---|---|---|---|---|---|
| TEMP | 0 | ✅ | dxmt9_d3d9_bytecode.hpp:172; dxmt9_shader_decoder.cpp:204 | core_spec_fixtures.hpp:196; shader_bytecode_validation_spec.cpp:180 | bounded to 32 (kMaxTempIndex) |
| INPUT | 1 | ✅ | dxmt9_d3d9_bytecode.hpp:173; dxmt9_shader_decoder.cpp:206 | shader_transform_spec.cpp:308 | |
| CONST | 2 | ✅ | dxmt9_d3d9_bytecode.hpp:174; dxmt9_shader_decoder.cpp:208 | core_shader_translator_spec.cpp:261; shader_bytecode_validation_spec.cpp:163 | OOB bounded to kMaxVertexConstants |
| ADDR / TEXTURE | 3 | ✅ | dxmt9_d3d9_bytecode.hpp:175; dxmt9_shader_decoder.cpp:210-211 | core_spec_fixtures.hpp:198; shader_transform_spec.cpp:263 | stage-dependent: ADDR in VS, INPUT in PS (legacy texture reg) |
| RASTOUT | 4 | ✅ | dxmt9_d3d9_bytecode.hpp:176; dxmt9_shader_decoder.cpp:212 | core_spec_fixtures.hpp:199; shader_bytecode_validation_spec.cpp:145 | |
| ATTROUT | 5 | ✅ | dxmt9_d3d9_bytecode.hpp:177; dxmt9_shader_decoder.cpp:214 | shader_transform_spec.cpp:1112 | |
| TEXCRDOUT / OUTPUT | 6 | ✅ | dxmt9_d3d9_bytecode.hpp:178; dxmt9_shader_decoder.cpp:216 | core_shader_translator_spec.cpp:252 | OUTPUT alias = TEXCRDOUT (same code 6) |
| CONSTINT | 7 | ✅ | dxmt9_d3d9_bytecode.hpp:179; dxmt9_shader_decoder.cpp:218 | shader_transform_spec.cpp:testPs30ConstIntSourceLowering | defined + decoded; OOB bounded to kMaxIntegerConstants; DEFI + cInt source lowering covered |
| COLOROUT | 8 | ✅ | dxmt9_d3d9_bytecode.hpp:180; dxmt9_shader_decoder.cpp:220 | core_spec_fixtures.hpp:201; shader_transform_spec.cpp:272 | |
| DEPTHOUT | 9 | ✅ | dxmt9_d3d9_bytecode.hpp:181; dxmt9_shader_decoder.cpp:222 | shader_transform_spec.cpp:372 | |
| SAMPLER | 10 | ✅ | dxmt9_d3d9_bytecode.hpp:182; dxmt9_shader_decoder.cpp:224 | shader_transform_spec.cpp:266 | OOB bounded to kMaxSamplers |
| CONST2 | 11 | 🟡 | dxmt9_d3d9_bytecode.hpp:190; dxmt9_shader_decoder.cpp:233-236 | shader_bytecode_validation_spec.cpp:199, 211 | aliased to ConstFloat (Wine parity) |
| CONST3 | 12 | 🟡 | dxmt9_d3d9_bytecode.hpp:191; dxmt9_shader_decoder.cpp:234-236 | ❌ none | aliased to ConstFloat (Wine parity) |
| CONST4 | 13 | 🟡 | dxmt9_d3d9_bytecode.hpp:192; dxmt9_shader_decoder.cpp:235-236 | ❌ none | aliased to ConstFloat (Wine parity) |
| CONSTBOOL | 14 | ✅ | dxmt9_d3d9_bytecode.hpp:193; dxmt9_shader_decoder.cpp:237 | shader_transform_spec.cpp:296; shader_transform_spec.cpp:1314 | OOB bounded to kMaxBoolConstants |
| LOOP | 15 | ✅ | dxmt9_d3d9_bytecode.hpp:194; dxmt9_shader_decoder.cpp:239 | shader_transform_spec.cpp:testPs30LoopRegisterConstIntLowering | defined + decoded; LOOP aL, i# lowering covered |
| TEMPFLOAT16 | 16 | 🔵 | dxmt9_d3d9_bytecode.hpp:202; dxmt9_shader_decoder.cpp:241-243 | shader_bytecode_validation_spec.cpp:200, 237 | safe-reject via DecoderRejectReason::TempFloat16Unsupported |
| MISCTYPE | 17 | ✅ | dxmt9_d3d9_bytecode.hpp:203; dxmt9_shader_decoder.cpp:244; dxmt9_shader_metal_ir.cpp:802, 2087 | shader_transform_spec.cpp:750 | vPos / vFace SM3 inputs |
| LABEL | 18 | 🔵 | dxmt9_d3d9_bytecode.hpp:210; dxmt9_shader_decoder.cpp:246-248 | shader_bytecode_validation_spec.cpp:201, 261 | safe-reject via DecoderRejectReason::LabelUnsupported (post b9a99d1) |
| PREDICATE | 19 | ✅ | dxmt9_d3d9_bytecode.hpp:211; dxmt9_shader_decoder.cpp:249 | core_spec_fixtures.hpp:202 | predicate-bit gating in dxmt9_shader_metal_ir.cpp:1142, 2188 |

### A.3 D3DDECLTYPE

| Type | Code | Status | Source anchor | Test anchor | Notes |
|---|---|---|---|---|---|
| FLOAT1 | 0 | ✅ | dxmt9_ffp_shaders.hpp:43; dxmt9_ffp_shaders.cpp:20, 115; d3d9_pe_device.cpp:276 | d3d9_conformance_visual_misc.c | ProcessVertices FLOAT1 TEXCOORD7 readback covers PE declaration decode |
| FLOAT2 | 1 | ✅ | dxmt9_ffp_shaders.hpp:44; dxmt9_ffp_shaders.cpp:22, 117; d3d9_pe_device.cpp:277 | shader_argbuf_binding_value_spec.cpp:608; d3d9_conformance_query_stateblock.c:385 | |
| FLOAT3 | 2 | ✅ | dxmt9_ffp_shaders.hpp:45; dxmt9_ffp_shaders.cpp:24, 119; d3d9_pe_device.cpp:278 | backend_pipeline_key_spec.cpp:450; d3d9_conformance_query_stateblock.c:375 | |
| FLOAT4 | 3 | ✅ | dxmt9_ffp_shaders.hpp:46; dxmt9_ffp_shaders.cpp:26, 121; d3d9_pe_device.cpp:279 | core_ffp_state_key_spec.cpp:182; d3d9_conformance_device.c:750 | |
| D3DCOLOR | 4 | ✅ | dxmt9_ffp_shaders.hpp:47; dxmt9_ffp_shaders.cpp:28; d3d9_pe_device.cpp:280 | backend_key_descriptor_spec.cpp:163; d3d9_conformance_device.c:615 | swizzled BGRA→RGBA on load |
| UBYTE4 | 5 | ✅ | dxmt9_ffp_shaders.hpp:48; dxmt9_ffp_shaders.cpp:29; d3d9_pe_device.cpp:281 | shader_transform_spec.cpp:1559; d3d9_conformance_visual_misc.c | ProcessVertices TEXCOORD4 and NORMAL readbacks cover raw integer PE decode; BLENDINDICES path covers programmable generic input |
| SHORT2 | 6 | ✅ | dxmt9_ffp_shaders.hpp:49; dxmt9_ffp_shaders.cpp:31; d3d9_pe_device.cpp:282 | d3d9_conformance_visual_misc.c | ProcessVertices TEXCOORD readback covers raw integer PE decode |
| SHORT4 | 7 | ✅ | dxmt9_ffp_shaders.hpp:50; dxmt9_ffp_shaders.cpp:38; d3d9_pe_device.cpp:283 | d3d9_conformance_visual_misc.c | ProcessVertices TEXCOORD4 and NORMAL readbacks cover raw integer PE decode |
| UBYTE4N | 8 | ✅ | dxmt9_ffp_shaders.hpp:51; dxmt9_ffp_shaders.cpp:30; d3d9_pe_device.cpp:284 | d3d9_conformance_visual_misc.c:828,853 | ProcessVertices TEXCOORD4 and NORMAL readbacks cover PE decode |
| SHORT2N | 9 | ✅ | dxmt9_ffp_shaders.hpp:52; dxmt9_ffp_shaders.cpp:32; d3d9_pe_device.cpp:285 | d3d9_conformance_visual_misc.c:786 | ProcessVertices TEXCOORD readback covers PE decode |
| SHORT4N | 10 | ✅ | dxmt9_ffp_shaders.hpp:53; dxmt9_ffp_shaders.cpp:39; d3d9_pe_device.cpp:286 | shader_transform_spec.cpp:1560, 1572; d3d9_conformance_visual_misc.c:807 | ProcessVertices TEXCOORD4 readback covers PE decode |
| USHORT2N | 11 | ✅ | dxmt9_ffp_shaders.hpp:54; dxmt9_ffp_shaders.cpp:33; d3d9_pe_device.cpp:287 | d3d9_conformance_visual_misc.c:793 | ProcessVertices TEXCOORD readback covers PE decode |
| USHORT4N | 12 | ✅ | dxmt9_ffp_shaders.hpp:55; dxmt9_ffp_shaders.cpp:40; d3d9_pe_device.cpp:288 | d3d9_conformance_visual_misc.c:814 | ProcessVertices TEXCOORD4 readback covers PE decode |
| UDEC3 | 13 | ✅ | dxmt9_ffp_shaders.hpp:56; dxmt9_ffp_shaders.cpp:34; d3d9_pe_device.cpp:289 | shader_transform_spec.cpp:testVs30VertexDeclarationUDec3Load; d3d9_conformance_visual_misc.c | size known; programmable VS declaration load path and ProcessVertices TEXCOORD4/NORMAL readbacks covered |
| DEC3N | 14 | ✅ | dxmt9_ffp_shaders.hpp:57; dxmt9_ffp_shaders.cpp:35; d3d9_pe_device.cpp:290 | d3d9_conformance_visual_misc.c | ProcessVertices TEXCOORD4 and NORMAL readbacks cover PE decode |
| FLOAT16_2 | 15 | ✅ | dxmt9_ffp_shaders.hpp:58; dxmt9_ffp_shaders.cpp:36; d3d9_pe_device.cpp:291 | shader_transform_spec.cpp:1561; d3d9_conformance_visual_misc.c:800 | ProcessVertices TEXCOORD readback covers PE decode |
| FLOAT16_4 | 16 | ✅ | dxmt9_ffp_shaders.hpp:59; dxmt9_ffp_shaders.cpp:41; d3d9_pe_device.cpp:292 | d3d9_conformance_visual_misc.c:821 | ProcessVertices TEXCOORD4 readback covers PE decode |
| UNUSED | 17 | ✅ | d3d9_pe_device.cpp:293, 311, 314, 423 | d3d9_conformance_device.c:848 | sentinel; size=0 |

### A.4 D3DDECLUSAGE

| Usage | Code | Status | Source anchor | Test anchor | Notes |
|---|---|---|---|---|---|
| POSITION | 0 | ✅ | dxmt9_ffp_shaders.hpp:60; dxmt9_ffp_shaders.cpp:260; d3d9_pe_device.cpp:343, 353 | core_spec_fixtures.hpp:207; core_shader_translator_spec.cpp:251 | |
| BLENDWEIGHT | 1 | ✅ | dxmt9_ffp_shaders.hpp:61; dxmt9_ffp_shaders.cpp:282; d3d9_pe_device.cpp:371 | shader_transform_spec.cpp:418 | |
| BLENDINDICES | 2 | ✅ | dxmt9_ffp_shaders.hpp:62; dxmt9_ffp_shaders.cpp:287 | shader_transform_spec.cpp:419 | |
| NORMAL | 3 | ✅ | dxmt9_ffp_shaders.hpp:63; dxmt9_ffp_shaders.cpp:274; d3d9_pe_device.cpp:378 | shader_argbuf_binding_value_spec.cpp:482 | |
| PSIZE | 4 | ✅ | dxmt9_ffp_shaders.hpp:64; dxmt9_ffp_shaders.cpp:278; d3d9_pe_device.cpp:384 | d3d9_conformance_visual_misc.c | ProcessVertices declaration and FVF programmable source readbacks cover PE decode; destination FVF/declaration readbacks cover fixed-function passthrough and programmable PSIZE output |
| TEXCOORD | 5 | ✅ | dxmt9_ffp_shaders.hpp:65; dxmt9_ffp_shaders.cpp:291; d3d9_pe_device.cpp:413 | core_shader_translator_spec.cpp:254 | |
| TANGENT | 6 | ✅ | dxmt9_ffp_shaders.hpp:66; dxmt9_shader_decoder.cpp:isSupportedDeclUsage; decodeVertexShaderInputLayout | shader_bytecode_validation_spec.cpp:testGenericDeclUsagesDecodeCleanly | programmable VS accepts as a generic vin[] semantic; FFP treats it as an extra non-FFP element |
| BINORMAL | 7 | ✅ | dxmt9_ffp_shaders.hpp:67; dxmt9_shader_decoder.cpp:isSupportedDeclUsage; decodeVertexShaderInputLayout | shader_bytecode_validation_spec.cpp:testGenericDeclUsagesDecodeCleanly | programmable VS accepts as a generic vin[] semantic; FFP treats it as an extra non-FFP element |
| TESSFACTOR | 8 | ✅ | dxmt9_ffp_shaders.hpp:68; dxmt9_shader_decoder.cpp:isSupportedDeclUsage; decodeVertexShaderInputLayout | shader_bytecode_validation_spec.cpp:testGenericDeclUsagesDecodeCleanly; d3d9_conformance_visual_misc.c | programmable VS accepts as a generic vin[] semantic; ProcessVertices reads it as a generic programmable source, with no fixed-function tessellation lowering |
| POSITIONT | 9 | ✅ | dxmt9_ffp_shaders.hpp:69; dxmt9_ffp_shaders.cpp:254; d3d9_pe_device.cpp:348 | core_ffp_state_key_spec.cpp:183; d3d9_conformance_device.c:751 | XYZRHW pre-transformed |
| COLOR | 10 | ✅ | dxmt9_ffp_shaders.hpp:70; dxmt9_ffp_shaders.cpp:266; d3d9_pe_device.cpp:390 | core_spec_fixtures.hpp:209; backend_key_descriptor_spec.cpp:167 | usage_index 0=diffuse, 1=specular |
| FOG | 11 | ✅ | dxmt9_ffp_shaders.hpp:71; dxmt9_shader_decoder.cpp:isSupportedDeclUsage; decodeVertexShaderInputLayout | shader_bytecode_validation_spec.cpp:testGenericDeclUsagesDecodeCleanly; d3d9_conformance_visual_misc.c | programmable VS accepts as a generic vin[] semantic; ProcessVertices reads it as a generic programmable source, while FFP fog still uses render-state/uniform path |
| DEPTH | 12 | ✅ | dxmt9_ffp_shaders.hpp:72; dxmt9_shader_decoder.cpp:isSupportedDeclUsage; decodeVertexShaderInputLayout | shader_bytecode_validation_spec.cpp:testGenericDeclUsagesDecodeCleanly; d3d9_conformance_visual_misc.c | programmable VS accepts as a generic vin[] semantic; ProcessVertices reads it as a generic programmable source |
| SAMPLE | 13 | ✅ | dxmt9_ffp_shaders.hpp:73; dxmt9_shader_decoder.cpp:isSupportedDeclUsage; decodeVertexShaderInputLayout | shader_bytecode_validation_spec.cpp:testGenericDeclUsagesDecodeCleanly; d3d9_conformance_visual_misc.c | programmable VS accepts as a generic vin[] semantic; ProcessVertices reads it as a generic programmable source |

### A.5 D3DDECLMETHOD

These are not semantic usages. Ordinary texture coordinates are represented by
`D3DDECLUSAGE_TEXCOORD` with `D3DDECLMETHOD_DEFAULT`; that path is supported.
The non-default methods below belong to the legacy fixed-function
tessellator/N-patch/displacement-map evaluation path, so they are rejected
explicitly instead of being silently treated as direct vertex fetches.

| Method | Code | Status | Source anchor | Test anchor | Notes |
|---|---|---|---|---|---|
| DEFAULT | 0 | ✅ | d3d9_pe_device.cpp:342, 347, 352, 359, 370, 412, 420 | backend_key_descriptor_spec.cpp:164 (kD3DDeclMethodDefault); d3d9_conformance_query_stateblock.c:375 | only method ever consumed |
| PARTIALU | 1 | 🔵 | dxmt9_shader_decoder.cpp:isSupportedDeclMethod; translateD3DBytecodeToSpirv | shader_bytecode_validation_spec.cpp:testNonDefaultDeclMethodsRejectCleanly | explicit safe-reject; N-patch/tessellator method has no Metal lowering |
| PARTIALV | 2 | 🔵 | dxmt9_shader_decoder.cpp:isSupportedDeclMethod; translateD3DBytecodeToSpirv | shader_bytecode_validation_spec.cpp:testNonDefaultDeclMethodsRejectCleanly | explicit safe-reject |
| CROSSUV | 3 | 🔵 | dxmt9_shader_decoder.cpp:isSupportedDeclMethod; translateD3DBytecodeToSpirv | shader_bytecode_validation_spec.cpp:testNonDefaultDeclMethodsRejectCleanly | explicit safe-reject |
| UV | 4 | 🔵 | dxmt9_shader_decoder.cpp:isSupportedDeclMethod; translateD3DBytecodeToSpirv | shader_bytecode_validation_spec.cpp:testNonDefaultDeclMethodsRejectCleanly | explicit safe-reject |
| LOOKUP | 5 | 🔵 | dxmt9_shader_decoder.cpp:isSupportedDeclMethod; translateD3DBytecodeToSpirv | shader_bytecode_validation_spec.cpp:testNonDefaultDeclMethodsRejectCleanly | explicit safe-reject |
| LOOKUPPRESAMPLED | 6 | 🔵 | dxmt9_shader_decoder.cpp:isSupportedDeclMethod; translateD3DBytecodeToSpirv | shader_bytecode_validation_spec.cpp:testNonDefaultDeclMethodsRejectCleanly | explicit safe-reject |

### A.6 Token modifiers

#### A.6.1 Dst token modifiers (`_dst` token, bits 20..23)

| Modifier | D3DSPDM | Mask | Status | Source anchor | Test anchor | Notes |
|---|---|---|---|---|---|---|
| NONE | _NONE | 0 | ✅ | dxmt9_shader_decoder.cpp:decodeDestModifier; dxmt9_shader_metal_ir.cpp:applyDestModifier | shader_transform_spec.cpp:testD3DBCDestModifierPartialPrecisionLowering | default; emitter leaves the value unchanged |
| SATURATE | _SATURATE | 0x1 | ✅ | dxmt9_shader_metal_ir.cpp:applyDestModifier | shader_transform_spec.cpp:testD3DBCDestModifierPartialPrecisionLowering; `test_visual_process_vertices_xyzhw_policy` | applied as `clamp(..., 0, 1)` and bitmask-composable with `_pp`; ProcessVertices programmable VS readback covers saturated COLOR0 output |
| PARTIAL_PRECISION | _PARTIALPRECISION | 0x2 | ✅ | dxmt9_shader_metal_ir.cpp:applyDestModifier | shader_transform_spec.cpp:testD3DBCDestModifierPartialPrecisionLowering | lowers the destination value through `float4(half4(...))`; the separate whole-fragment-source `DXMT9_FS_HALF_PRECISION` experiment has been removed (non-functional) |
| CENTROID | _MSAMPCENTROID | 0x4 | ✅ | dxmt9_shader_decoder.cpp:collectPixelInputSemantics; dxmt9_shader_metal_ir.cpp:makePreludeOptions; dxmt9_shader_sources.cpp:makeShaderPrelude | shader_transform_spec.cpp:testPs30CentroidInputModifierLowersToMslInterpolation | pixel-input DCL modifier lowers to `[[centroid_perspective]]` on matching VSOut varyings |
| PREDICATED-instr bit | n/a | bit 28 of instr token | ✅ | dxmt9_shader_decoder.cpp:1387; dxmt9_shader_metal_ir.cpp:1142, 2188 | shader_transform_spec.cpp:1289 (`predicated == true`) | per-instruction predicate guard |

#### A.6.2 Src token swizzle + source modifiers (`_src` token, bits 24..27)

| Modifier | D3DSPSM | Code | Status | Source anchor | Test anchor | Notes |
|---|---|---|---|---|---|---|
| Swizzle | n/a | bits 16..23 | ✅ | dxmt9_shader_decoder.cpp:173 (decodeSwizzle); dxmt9_shader_metal_ir.cpp:applySwizzle | shader_transform_spec.cpp:1295, 1304 | 4×2-bit component selector |
| NONE | _NONE | 0 | ✅ | dxmt9_shader_metal_ir.cpp:216 | shader_transform_spec.cpp:testPs30WriteMaskSwizzleAndSourceModifiers | passthrough |
| NEG | _NEG | 1 | ✅ | dxmt9_shader_metal_ir.cpp:218 | shader_transform_spec.cpp:testPs30WriteMaskSwizzleAndSourceModifiers | unary minus |
| BIAS | _BIAS | 2 | ✅ | dxmt9_shader_metal_ir.cpp:220 | shader_transform_spec.cpp:testPs30MissingSourceModifierCoverage | (x - 0.5) |
| BIASNEG | _BIASNEG | 3 | ✅ | dxmt9_shader_metal_ir.cpp:222 | shader_transform_spec.cpp:testPs30MissingSourceModifierCoverage | -(x - 0.5) |
| SIGN | _SIGN | 4 | ✅ | dxmt9_shader_metal_ir.cpp:224 | shader_transform_spec.cpp:testPs30MissingSourceModifierCoverage | (x*2 - 1) |
| SIGNNEG | _SIGNNEG | 5 | ✅ | dxmt9_shader_metal_ir.cpp:226 | shader_transform_spec.cpp:testPs30MissingSourceModifierCoverage | -(x*2 - 1) |
| COMP | _COMP | 6 | ✅ | dxmt9_shader_metal_ir.cpp:228 | shader_transform_spec.cpp:testPs30MissingSourceModifierCoverage | (1 - x) |
| X2 | _X2 | 7 | ✅ | dxmt9_shader_metal_ir.cpp:230 | shader_transform_spec.cpp:testPs30MissingSourceModifierCoverage | (x*2) |
| X2NEG | _X2NEG | 8 | ✅ | dxmt9_shader_metal_ir.cpp:232 | shader_transform_spec.cpp:testPs30MissingSourceModifierCoverage | -(x*2) |
| DZ | _DZ | 9 | ✅ | dxmt9_shader_metal_ir.cpp:234 | shader_transform_spec.cpp:testPs30MissingSourceModifierCoverage | (x / x.z) |
| DW | _DW | 10 | ✅ | dxmt9_shader_metal_ir.cpp:236 | shader_transform_spec.cpp:testPs30MissingSourceModifierCoverage | (x / x.w) |
| ABS | _ABS | 11 | ✅ | dxmt9_shader_metal_ir.cpp:238 | shader_transform_spec.cpp:1308 (`modifier == 11u`) | abs(x) |
| ABSNEG | _ABSNEG | 12 | ✅ | dxmt9_shader_metal_ir.cpp:240 | shader_transform_spec.cpp:testPs30MissingSourceModifierCoverage | -abs(x) |
| NOT | _NOT | 13 | ✅ | dxmt9_shader_metal_ir.cpp:242 | shader_transform_spec.cpp:testPs30MissingSourceModifierCoverage | predicate-style select |
| Relative addressing | n/a | bit 13 | ✅ | dxmt9_shader_decoder.cpp:198 (tokenHasRelativeAddressing); 1404-1422 (per-opcode register-vs-literal probe) | shader_transform_spec.cpp:1324 (tokenHasRelativeAddressingForTest) | extra DWORD per rel-addr operand |
| WriteMask (dst) | n/a | bits 16..19 | ✅ | dxmt9_shader_decoder.cpp:194 (decodeWriteMask) | shader_transform_spec.cpp:1298 | 4-bit per-component write mask |
| Coissue (instr) | n/a | bit 30 | ✅ | dxmt9_shader_decoder.cpp:decoded `coissue`; dxmt9_shader_metal_ir.cpp:CND PS1.x branch | shader_runner/corpus/legacy_sm1/dxmt9_ps13_cnd_coissue_readback.shader_test | PS1.x coissued CND alpha path is lowered; other coissue uses are scheduling hints and execute serially |

### A.7 Section summary

- **D3DSIO opcodes**: 96 named + 3 sentinels (PHASE/COMMENT/END) defined; 93 fully lowered (✅), 3 with `kUnsupportedFixedOperandCount` audit-marked but control-flow-emitted (⚠️ LOOP, SINCOS, REP — they execute via shared paths; the audit row simply notes the decoder uses a variable operand-count path). 0 missing. All 99 covered by `opcode_audit_spec`.
- **D3DSPR register kinds**: 20 codes (0..19, with code 6 being TEXCRDOUT/OUTPUT alias and code 3 being ADDR/TEXTURE stage-dependent). 15 fully tested (✅), 3 aliased to ConstFloat (🟡 CONST2/3/4 — Wine parity), 2 safe-rejected (🔵 TEMPFLOAT16, LABEL). CONSTINT and LOOP now have dedicated lowering tests. 0 missing.
- **D3DDECLTYPE**: 18 codes (0..17 incl. UNUSED). 18 fully tested or trivially covered (✅), 0 missing test coverage from the inventory tests. UDEC3 now has a programmable VS declaration load test plus ProcessVertices TEXCOORD4 and NORMAL readbacks, and DEC3N is covered by ProcessVertices NORMAL readback. 0 missing constants.
- **D3DDECLUSAGE**: 14 codes (0..13). All are accepted by the programmable VS declaration matcher; the uncommon usages (TANGENT, BINORMAL, TESSFACTOR, FOG, DEPTH, SAMPLE) are covered as generic input semantics. FFP still consumes only the fixed-function subset.
- **D3DDECLMETHOD**: 7 codes (0..6). DEFAULT is consumed; methods 1..6 explicitly safe-reject because dxmt9 has no N-patch/tessellator lowering.
- **Dst token modifiers**: 3 D3DSPDM bits + predicated-instr bit. SATURATE, PARTIALPRECISION, and MSAMPCENTROID are covered; predicate-bit ✅.
- **Src token modifiers**: 14 D3DSPSM codes (0..13) + swizzle + rel-addr + writeMask + coissue. 14/14 source modifiers implemented and directly exercised by `shader_transform_spec`. Swizzle/writeMask/rel-addr all ✅. Coissue has PS1.x CND readback coverage; non-CND coissue remains a serial scheduling hint.

**Totals**: D3DSIO: 99 defined / 99 tested / 0 missing. D3DSPR: 20 defined / 15 directly tested + 3 alias + 2 safe-reject / 0 missing constants. D3DDECLTYPE: 18 defined / 18 directly tested or trivially covered / 0 missing. D3DDECLUSAGE: 14 defined / 14 accepted (fixed-function subset plus generic programmable VS semantics) / 0 missing. D3DDECLMETHOD: 7 defined / 1 consumed / 6 explicit safe-reject. Dst-mods: SATURATE, PARTIALPRECISION, MSAMPCENTROID plus predicate-bit covered / 0 missing. Src-mods: 14 / 14 directly tested / 0 missing impl.

## Methodology

Inventory built by ripgrep against the dxmt9 tree (read-only audit):

```sh
# Spec oracle (Wine d3d9types.h):
rg -n 'D3DSIO_|D3DSPR_|D3DDECLTYPE_|D3DDECLUSAGE_|D3DDECLMETHOD_|D3DSPDM_|D3DSPSM_' \
  ~/workspaces/wine/include/d3d9types.h

# dxmt9 source constants:
rg -n 'kD3DSIO_|kD3DSPR_' src/dxmt9/dxmt9_d3d9_bytecode.hpp
rg -n 'kD3DDeclType|kD3DDeclUsage|kD3DDeclMethod' src/dxmt9/dxmt9_ffp_shaders.hpp

# dxmt9 register-kind + opcode emission:
rg -n 'kD3DSIO_|kD3DSPR_' src/dxmt9/dxmt9_shader_decoder.cpp src/dxmt9/dxmt9_shader_metal_ir.cpp

# Dst/src modifier handling:
rg -nE 'decodeSourceModifier|decodeDestModifier|decodeWriteMask|applySourceModifier|decodeSwizzle' \
  src/dxmt9/

# Decl-type/usage/method in PE-side conversion and FFP layout:
rg -n 'D3DDECLTYPE_|D3DDECLUSAGE_|D3DDECLMETHOD_|kD3DDeclType|kD3DDeclUsage|kD3DDeclMethod' \
  src/d3d9 src/dxmt9 include

# Test coverage:
rg -n 'kD3DSIO_|kD3DSPR_|kD3DDeclType|kD3DDeclUsage|kD3DDeclMethod|D3DDECLTYPE_|D3DDECLUSAGE_|D3DDECLMETHOD_' \
  tests/native tests/conformance

# Per-opcode first-line lookup against the audit table:
rg -nE 'kD3DSIO_[A-Za-z0-9_]+' tests/native/shader/opcode_audit_spec.cpp
```

Anchors are the **first defining or emitting** line; for opcodes the audit table at `tests/native/shader/opcode_audit_spec.cpp:62-144` is the canonical per-opcode test row. Status downgrades from ✅ to ⚠️/🟡/🔵/❌ follow the legend in the task description.
## B. Render / Texture / Sampler / Transform / Light / Material State

> Audit basis: Wine `~/workspaces/wine/include/d3d9types.h` lines
> 948-1054 (D3DRENDERSTATETYPE), 1166-1187 (D3DTSS), 1232-1248 (D3DSAMP),
> 1200-1213 (D3DTS), 873-879 (D3DLIGHTTYPE), 1287-1290 (D3DCLIPSTATUS9),
> 1385-1389 (D3DGAMMARAMP), 1408-1422 (D3DLIGHT9), 1440-1446
> (D3DMATERIAL9), 1541-1548 (D3DVIEWPORT9). dxmt9 anchors are at HEAD
> (worktree `agent-a8cadf7009bfdcb14`).
>
> PE-shadow column: the PE shadow stores RS in
> `FixedStateTable<kPeRenderStateSlots=256>` (anchor
> `src/d3d9/d3d9_pe_state_shadow.hpp:425`), so every RS slot ≤ 255 gets
> shadow storage for free — Set/Get goes through `renderStateShadow`
> uniformly. The same is true for TSS (64-slot table per stage,
> `:454`) and SAMP (64-slot table per sampler, `:456`). The interesting
> question is whether *the value is also forwarded through the wire
> packet and read by the backend*. Generic-delta chunk coverage is
> uniform: `D9CDrawPacketRenderState{state, value}` in
> `D9CDrawPrimitivePacket.renderStates[64]` (`include/dxmt9/device_c.h:221`,
> `:288-348`) carries any RS pair as opaque DWORD, so chunk is ⚠️
> (generic-delta only) unless there is a dedicated field (viewport,
> material, lights, clipPlanes, transforms — those get ✅).
>
> Status legend: ✅ defined / used / tested. ⚠️ partial / generic. ❌
> not present. 🟡 intentionally mirrored. 🚫 explicitly deferred.

### B.1 D3DRENDERSTATETYPE

| RS name | Code | defined | PE-shadow | chunk | backend-read | test | Notes |
|---|---|---|---|---|---|---|---|
| ZENABLE | 7 | ✅ `core::RS_Z_ENABLE` | ✅ | ⚠️ | ✅ depth key / encoder | ✅ state_draw_transform_spec:testInitialD3D9StateMatrix | Initial FALSE without auto depth-stencil, TRUE with it |
| FILLMODE | 8 | ✅ `core::RS_FILL_MODE` | ✅ | ⚠️ | ✅ encoder triangleFillMode | ✅ state_draw_transform_spec:testInitialD3D9StateMatrix | Initial SOLID (3) |
| SHADEMODE | 9 | ⚠️ inline literal `9u /*RS_SHADEMODE*/` (core_state.cpp:133) | ✅ | ⚠️ | accepted no-op | ✅ state_draw_transform_spec:testAcceptedRenderStateRoundTripPolicies | Default Gouraud (2). Shadowed/readable; Metal lacks per-primitive flat shading without code-gen change. |
| (10 dead) | 10 | n/a | n/a | n/a | n/a | n/a | D3D9 unused |
| (11 dead) | 11 | n/a | n/a | n/a | n/a | n/a | |
| (12 dead) | 12 | n/a | n/a | n/a | n/a | n/a | |
| (13 dead) | 13 | n/a | n/a | n/a | n/a | n/a | |
| ZWRITEENABLE | 14 | ✅ `core::RS_Z_WRITE_ENABLE` (core_constants.hpp:378) | ✅ | ⚠️ | ✅ `dxmt9_draw_state.cpp:232` | ✅ state_draw_transform_spec | |
| ALPHATESTENABLE | 15 | ✅ `core::RS_ALPHA_TEST_ENABLE` (core_constants.hpp:352) | ✅ | ⚠️ | ✅ `dxmt9_draw_state.cpp:176`, `dxmt9_pipeline_cache.cpp:780` | ✅ ffp_key_determinism_spec:85, state_draw_transform_spec | |
| LASTPIXEL | 16 | ⚠️ `kRsLastPixel` (core_state.cpp:347) file-static | ✅ | ⚠️ | accepted no-op | ✅ state_draw_transform_spec:testAcceptedRenderStateRoundTripPolicies | Default TRUE; line-raster last-pixel policy has no Metal toggle |
| (17 dead) | 17 | n/a | n/a | n/a | n/a | n/a | |
| (18 dead) | 18 | n/a | n/a | n/a | n/a | n/a | |
| SRCBLEND | 19 | ✅ `core::RS_SRC_BLEND` (core_constants.hpp:380) | ✅ | ⚠️ | ✅ `dxmt9_pipeline_cache.cpp:152` (makeBlendAttachmentKeys) | ✅ blend_op_family_spec | |
| DESTBLEND | 20 | ✅ `core::RS_DEST_BLEND` (core_constants.hpp:381) | ✅ | ⚠️ | ✅ `dxmt9_pipeline_cache.cpp:154` | ✅ blend_op_family_spec | |
| (21 dead) | 21 | n/a | n/a | n/a | n/a | n/a | |
| CULLMODE | 22 | ✅ `core::RS_CULL_MODE` (core_constants.hpp:376) | ✅ | ⚠️ | ✅ `dxmt9_draw_encoder_internal.hpp:552` | ⚠️ encoder consumer only | |
| ZFUNC | 23 | ✅ `core::RS_Z_FUNC` (core_constants.hpp:379) | ✅ | ⚠️ | ✅ `dxmt9_draw_state.cpp:234` | ✅ state_draw_transform_spec | |
| ALPHAREF | 24 | ✅ `core::RS_ALPHA_REF` (core_constants.hpp:354) | ✅ | ⚠️ | ✅ `dxmt9_draw_state.cpp:191`, `dxmt9_pipeline_cache.cpp:716` | ⚠️ implicit via ffp key | Wire 0..255, FS receives /255.0f normalized |
| ALPHAFUNC | 25 | ✅ `core::RS_ALPHA_FUNC` (core_constants.hpp:353) | ✅ | ⚠️ | ✅ `dxmt9_draw_state.cpp:178` | ✅ ffp_key_determinism_spec:86 | |
| DITHERENABLE | 26 | ⚠️ `kRsDitherEnable` | ✅ | ⚠️ | accepted no-op | ✅ state_draw_transform_spec initial matrix + round-trip policy | Default FALSE; `RasterCaps` deliberately clears DITHER so this no-op is not advertised as implemented |
| ALPHABLENDENABLE | 27 | ✅ `core::RS_ALPHABLEND_ENABLE` (core_constants.hpp:386) | ✅ | ⚠️ | ✅ `dxmt9_pipeline_cache.cpp:143` | ✅ blend_op_family_spec | |
| FOGENABLE | 28 | ✅ `core::RS_FOG_ENABLE` (core_constants.hpp:355) | ✅ | ⚠️ | ✅ `dxmt9_draw_state.cpp:179` | ⚠️ via ffp key | |
| SPECULARENABLE | 29 | ✅ `core::RS_SPECULAR_ENABLE` (core_constants.hpp:347) | ✅ | ⚠️ | ✅ `core_draw.cpp:2152` (FFP key.specularEnabled) | ✅ ffp_key_determinism_spec:66 | |
| (30 dead) | 30 | n/a | n/a | n/a | n/a | n/a | |
| (31 dead) | 31 | n/a | n/a | n/a | n/a | n/a | |
| (32 dead) | 32 | n/a | n/a | n/a | n/a | n/a | |
| (33 dead) | 33 | n/a | n/a | n/a | n/a | n/a | |
| FOGCOLOR | 34 | ✅ `core::RS_FOG_COLOR` (core_constants.hpp:356) | ✅ | ⚠️ | ✅ `dxmt9_draw_state.cpp:189` | ⚠️ ffp_key | |
| FOGTABLEMODE | 35 | ✅ `core::RS_FOG_TABLE_MODE` (core_constants.hpp:349) | ✅ | ⚠️ | ✅ `dxmt9_draw_state.cpp:181` | ✅ ffp_key_determinism_spec:72 | |
| FOGSTART | 36 | ✅ `core::RS_FOG_START` | ✅ | ⚠️ | ✅ fog uniforms | ✅ state_draw_transform_spec:testInitialD3D9StateMatrix | Default 0.0f |
| FOGEND | 37 | ✅ `core::RS_FOG_END` (core_constants.hpp:358) | ✅ | ⚠️ | ✅ `dxmt9_draw_state.cpp:145,195` | ⚠️ | |
| FOGDENSITY | 38 | ✅ `core::RS_FOG_DENSITY` (core_constants.hpp:359) | ✅ | ⚠️ | ✅ `dxmt9_draw_state.cpp:147,197` | ⚠️ | |
| (39..47 dead) | 39-47 | n/a | n/a | n/a | n/a | n/a | nine unused slots |
| RANGEFOGENABLE | 48 | ✅ `core::RS_RANGE_FOG` (core_constants.hpp:351) | ✅ | ⚠️ | ✅ `dxmt9_draw_state.cpp:150` | ✅ ffp_key_determinism_spec:74 | |
| (49 dead) | 49 | n/a | n/a | n/a | n/a | n/a | |
| (50 dead) | 50 | n/a | n/a | n/a | n/a | n/a | |
| (51 dead) | 51 | n/a | n/a | n/a | n/a | n/a | |
| STENCILENABLE | 52 | ✅ `core::RS_STENCIL_ENABLE` (core_constants.hpp:393) | ✅ | ⚠️ | ✅ `dxmt9_draw_state.cpp:236` | ✅ stencil_ref_spec | |
| STENCILFAIL | 53 | ✅ `core::RS_STENCIL_FAIL` (core_constants.hpp:395) | ✅ | ⚠️ | ✅ `dxmt9_draw_state.cpp:240` | ⚠️ generic stencil cov | |
| STENCILZFAIL | 54 | ✅ `core::RS_STENCIL_ZFAIL` (core_constants.hpp:396) | ✅ | ⚠️ | ✅ `dxmt9_draw_state.cpp:242` | ⚠️ | |
| STENCILPASS | 55 | ✅ `core::RS_STENCIL_PASS` (core_constants.hpp:397) | ✅ | ⚠️ | ✅ `dxmt9_draw_state.cpp:244` | ⚠️ | |
| STENCILFUNC | 56 | ✅ `core::RS_STENCIL_FUNC` (core_constants.hpp:394) | ✅ | ⚠️ | ✅ `dxmt9_draw_state.cpp:238` | ⚠️ | |
| STENCILREF | 57 | ✅ `core::RS_STENCIL_REF` (core_constants.hpp:398) | ✅ | ⚠️ | ✅ `dxmt9_draw_state.cpp:270` (`computeStencilRef`) | ✅ stencil_ref_spec (b8d31a9) | wired in b8d31a9 |
| STENCILMASK | 58 | ✅ `core::RS_STENCIL_MASK` | ✅ | ⚠️ | ✅ depth/stencil key | ✅ state_draw_transform_spec:testInitialD3D9StateMatrix | Default 0xffffffff |
| STENCILWRITEMASK | 59 | ✅ `core::RS_STENCIL_WRITEMASK` | ✅ | ⚠️ | ✅ depth/stencil key | ✅ state_draw_transform_spec:testInitialD3D9StateMatrix | Default 0xffffffff |
| TEXTUREFACTOR | 60 | ✅ `core::RS_TEXTURE_FACTOR` | ✅ | ⚠️ | ✅ FFP PS uniform | ✅ state_draw_transform_spec:testInitialD3D9StateMatrix | Default opaque white (0xffffffff) |
| (61..127 dead) | 61-127 | n/a | n/a | n/a | n/a | n/a | 67 unused slots |
| WRAP0 | 128 | ⚠️ `kRsWrap0` (core_state.cpp:349) | ✅ | ⚠️ | accepted no-op | ✅ state_draw_transform_spec:testWrapRenderStateRoundTrip | Shadowed/readable; no UV cylindrical-wrap lowering in Metal sampler path |
| WRAP1 | 129 | ⚠️ `kRsWrap1` | ✅ | ⚠️ | accepted no-op | ✅ state_draw_transform_spec:testWrapRenderStateRoundTrip | |
| WRAP2 | 130 | ⚠️ `kRsWrap2` | ✅ | ⚠️ | accepted no-op | ✅ state_draw_transform_spec:testWrapRenderStateRoundTrip | |
| WRAP3 | 131 | ⚠️ `kRsWrap3` | ✅ | ⚠️ | accepted no-op | ✅ state_draw_transform_spec:testWrapRenderStateRoundTrip | |
| WRAP4 | 132 | ⚠️ `kRsWrap4` | ✅ | ⚠️ | accepted no-op | ✅ state_draw_transform_spec:testWrapRenderStateRoundTrip | |
| WRAP5 | 133 | ⚠️ `kRsWrap5` | ✅ | ⚠️ | accepted no-op | ✅ state_draw_transform_spec:testWrapRenderStateRoundTrip | |
| WRAP6 | 134 | ⚠️ `kRsWrap6` | ✅ | ⚠️ | accepted no-op | ✅ state_draw_transform_spec:testWrapRenderStateRoundTrip | |
| WRAP7 | 135 | ⚠️ `kRsWrap7` | ✅ | ⚠️ | accepted no-op | ✅ state_draw_transform_spec:testWrapRenderStateRoundTrip | |
| CLIPPING | 136 | ✅ `core::RS_CLIPPING` | ✅ | ⚠️ | normal draws map `FALSE` to Metal depth clamp and suppress user clip-distance output; `ProcessVertices` also applies `FALSE` depth clamp; transformed SWVP line-list/strip and triangle-list/strip/fan fallback clips against viewport/depth/user planes and drops all-outside primitives; broader clipping edge cases remain deferred | ✅ state_draw_transform_spec:testClippingFalseSuppressesUserClipPlanes + encode_draw_recorder_spec + `test_visual_process_vertices_xyzhw_policy` | Default TRUE |
| LIGHTING | 137 | ✅ `core::RS_LIGHTING` (core_constants.hpp:346) | ✅ | ⚠️ | ✅ `core_draw.cpp:1916,2150` (FFP key.lightingEnabled) | ✅ ffp_key_determinism_spec:65 | |
| (138 dead) | 138 | n/a | n/a | n/a | n/a | n/a | |
| AMBIENT | 139 | ✅ `core::RS_AMBIENT` (core_constants.hpp:360) | ✅ | ⚠️ | ✅ `dxmt9_draw_state.cpp:108` (uniform) | ⚠️ | |
| FOGVERTEXMODE | 140 | ✅ `core::RS_FOG_FROM_VERTEX` | ✅ | ⚠️ | ✅ fog resolution/key | ✅ state_draw_transform_spec:testInitialD3D9StateMatrix + ffp_key_determinism_spec | Default D3DFOG_NONE |
| COLORVERTEX | 141 | ⚠️ `kRsColorVertex` (core_state.cpp:358) | ✅ | ⚠️ | ✅ `core_draw.cpp:makeFfpVertexKey` gates color material sources | ✅ ffp_key_determinism_spec; state_draw_transform_spec:testAcceptedRenderStateRoundTripPolicies | Default TRUE; FALSE forces FFP material sources to material constants |
| LOCALVIEWER | 142 | ✅ `core::RS_LOCAL_VIEWER` | ✅ | ⚠️ | ✅ FFP specular and camera-space reflection texcoord generation select per-vertex or infinite-viewer formulas | ✅ core_ffp_state_key_spec:testFfpVertexCoordinateAndLightingContracts + ffp_key_determinism_spec | Default TRUE |
| NORMALIZENORMALS | 143 | ✅ `core::RS_NORMALIZE_NORMALS` | ✅ | ⚠️ | ✅ transformed camera normal is normalized only for `TRUE`; lighting and generated normal/reflection texcoords share the result | ✅ core_ffp_state_key_spec:testFfpVertexCoordinateAndLightingContracts + ffp_key_determinism_spec | Default FALSE |
| (144 dead) | 144 | n/a | n/a | n/a | n/a | n/a | |
| DIFFUSEMATERIALSOURCE | 145 | ✅ `core::RS_DIFFUSE_MATERIAL_SOURCE` (core_constants.hpp:361) | ✅ | ⚠️ | ✅ `core_draw.cpp:2169` (FFP key.colorMaterialMode[2]) | ✅ ffp_key_determinism_spec:70 | |
| SPECULARMATERIALSOURCE | 146 | ✅ `core::RS_SPECULAR_MATERIAL_SOURCE` (core_constants.hpp:362) | ✅ | ⚠️ | ✅ `core_draw.cpp:2173` (key.colorMaterialMode[3]) | ✅ ffp_key_determinism_spec:71 | |
| AMBIENTMATERIALSOURCE | 147 | ✅ `core::RS_AMBIENT_MATERIAL_SOURCE` (core_constants.hpp:363) | ✅ | ⚠️ | ✅ `core_draw.cpp:2165` (key.colorMaterialMode[1]) | ✅ ffp_key_determinism_spec:69 | |
| EMISSIVEMATERIALSOURCE | 148 | ✅ `core::RS_EMISSIVE_MATERIAL_SOURCE` (core_constants.hpp:364) | ✅ | ⚠️ | ✅ `core_draw.cpp:2161` (key.colorMaterialMode[0]) | ✅ ffp_key_determinism_spec:68 | |
| (149 dead) | 149 | n/a | n/a | n/a | n/a | n/a | |
| (150 dead) | 150 | n/a | n/a | n/a | n/a | n/a | |
| VERTEXBLEND | 151 | ✅ `core::RS_VERTEX_BLEND` | ✅ | ⚠️ | ✅ per-matrix WorldViewProjection, WorldView, and inverse-transpose normal transforms blend clip position, camera position, and camera normal | ✅ state_draw_transform_spec:testTransformMultiplicationOrderAndBlendSlots + core_ffp_state_key_spec + shader-runner vertex-blend corpus | |
| CLIPPLANEENABLE | 152 | ✅ `core::RS_CLIP_PLANE_ENABLE` | ✅ | ⚠️ | ✅ drives clip-distance output while `D3DRS_CLIPPING=TRUE`; suppressed when clipping is disabled | ✅ ffp_key_determinism_spec + state_draw_transform_spec:testClippingFalseSuppressesUserClipPlanes | |
| (153 dead) | 153 | n/a | n/a | n/a | n/a | n/a | |
| POINTSIZE | 154 | ✅ `core::RS_POINTSIZE` | ✅ | ⚠️ | ✅ FFP and programmable VS without PSIZE/oPts use the uniform default | ✅ core_ffp_state_key_spec + core_shader_translator_spec + shader-runner FFP point readback | |
| POINTSIZE_MIN | 155 | ✅ `core::RS_POINTSIZE_MIN` | ✅ | ⚠️ | ✅ clamps both FFP and programmable VS point output | ✅ initial-state matrix + core_ffp_state_key_spec + core_shader_translator_spec | Default 1.0f |
| POINTSPRITEENABLE | 156 | ✅ `core::RS_POINT_SPRITE_ENABLE` (core_constants.hpp:369) | ✅ | ⚠️ | ✅ `core_draw.cpp:2209,2253` (FFP key.pointSpriteEnabled), `ffp_shaders.cpp:736,812` | ⚠️ ef3ec91 (P1-4) | |
| POINTSCALEENABLE | 157 | ✅ `core::RS_POINT_SCALE_ENABLE` (core_constants.hpp:370) | ✅ | ⚠️ | ✅ `core_draw.cpp:2212` (key.pointScaleEnabled) | ⚠️ ef3ec91 (P1-4) | |
| POINTSCALE_A | 158 | ✅ `core::RS_POINTSCALE_A` | ✅ | ⚠️ | ✅ point uniform | ✅ initial-state matrix + core_ffp_state_key_spec | Default 1.0f |
| POINTSCALE_B | 159 | ✅ `core::RS_POINTSCALE_B` (core_constants.hpp:372) | ✅ | ⚠️ | ✅ `dxmt9_draw_state.cpp:163` | ⚠️ ef3ec91 | |
| POINTSCALE_C | 160 | ✅ `core::RS_POINTSCALE_C` (core_constants.hpp:373) | ✅ | ⚠️ | ✅ `dxmt9_draw_state.cpp:165` | ⚠️ ef3ec91 | |
| MULTISAMPLEANTIALIAS | 161 | ⚠️ inline literal `161u` (core_state.cpp:134) | ✅ | ⚠️ | ❌ not consumed (Metal MSAA is per-attachment) | ❌ | Stateblock + default only |
| MULTISAMPLEMASK | 162 | ✅ `core::RS_MULTISAMPLE_MASK` | ✅ | ✅ draw volatile | ✅ FFP and programmable `[[sample_mask]]` output | ✅ draw_uniforms_layout, encode_draw_recorder, shader_transform, tile_ffp_selector | Defaults to `0xffffffff`; nontrivial masks on maskable multisampled targets route tile FFP to the portable fragment path, while single-sample targets use an all-ones effective mask. |
| PATCHEDGESTYLE | 163 | ⚠️ `kRsPatchEdgeStyle` (core_state.cpp:362) | ✅ | ⚠️ | ❌ not consumed (no N-patch) | 🚫 | D3D9 N-patch — explicitly deferred |
| (164 dead) | 164 | n/a | n/a | n/a | n/a | n/a | |
| DEBUGMONITORTOKEN | 165 | ❌ not defined | ✅ shadow generic | ⚠️ | ❌ | ❌ | DEBUG-time only; no D3D9 behavior on retail |
| POINTSIZE_MAX | 166 | ✅ `core::RS_POINTSIZE_MAX` | ✅ | ⚠️ | ✅ clamps both FFP and programmable VS point output | ✅ initial-state matrix + core_ffp_state_key_spec + core_shader_translator_spec | Default `caps.MaxPointSize` (64.0f) |
| INDEXEDVERTEXBLENDENABLE | 167 | ✅ `core::RS_INDEXED_VERTEX_BLEND_ENABLE` (core_constants.hpp:375); also `kRsIndexedVertexBlendEnable` alias | ✅ | ⚠️ | ✅ `core_draw.cpp:2203` (key.indexedVertexBlend) | ✅ ffp_key_determinism_spec:76,226 | |
| COLORWRITEENABLE | 168 | ✅ `core::RS_COLOR_WRITE_ENABLE` (core_constants.hpp:384) | ✅ | ⚠️ | ✅ `dxmt9_pipeline_cache.cpp:162` | ⚠️ implicit via blend key | |
| (169 dead) | 169 | n/a | n/a | n/a | n/a | n/a | |
| TWEENFACTOR | 170 | ⚠️ `kRsTweenFactor` (core_state.cpp:364) | ✅ | ⚠️ | ❌ not consumed (no FFP tween path) | 🚫 | |
| BLENDOP | 171 | ✅ `core::RS_BLEND_OP` (core_constants.hpp:382) | ✅ | ⚠️ | ✅ `dxmt9_pipeline_cache.cpp:147` | ✅ blend_op_family_spec (c53284d) | |
| POSITIONDEGREE | 172 | ⚠️ `kRsPositionDegree` (core_state.cpp:365) | ✅ | ⚠️ | ❌ | 🚫 | N-patch — deferred |
| NORMALDEGREE | 173 | ⚠️ `kRsNormalDegree` (core_state.cpp:366) | ✅ | ⚠️ | ❌ | 🚫 | N-patch — deferred |
| SCISSORTESTENABLE | 174 | ✅ `core::RS_SCISSOR_TEST_ENABLE` (core_constants.hpp:383) | ✅ | ⚠️ | ✅ `chunk_replay.cpp:458`, `core_state.cpp:541` | ⚠️ no dedicated spec | Forwarded to encoder scissorRect logic |
| SLOPESCALEDEPTHBIAS | 175 | ✅ `core::RS_SLOPE_SCALE_DEPTH_BIAS` (core_constants.hpp) | ✅ | ⚠️ | ✅ `dxmt9_draw_encoder.mm` feeds Metal depth bias slope scale | ✅ backend depth-bias gate | Closed 2026-05-24; paired with `DEPTHBIAS` |
| ANTIALIASEDLINEENABLE | 176 | ⚠️ `kRsAntialiasedLineEnable` (core_state.cpp:368) | ✅ | ⚠️ | ❌ | ❌ | Metal has no AA-line mode |
| (177 dead) | 177 | n/a | n/a | n/a | n/a | n/a | |
| MINTESSELLATIONLEVEL | 178 | ⚠️ `kRsMinTessellationLevel` (core_state.cpp:369) | ✅ | ⚠️ | ❌ | 🚫 | N-patch tessellation — deferred |
| MAXTESSELLATIONLEVEL | 179 | ⚠️ `kRsMaxTessellationLevel` (core_state.cpp:370) | ✅ | ⚠️ | ❌ | 🚫 | |
| ADAPTIVETESS_X | 180 | ⚠️ `kRsAdaptiveTessX` (core_state.cpp:371) | ✅ | ⚠️ | ❌ | 🚫 | |
| ADAPTIVETESS_Y | 181 | ⚠️ `kRsAdaptiveTessY` (core_state.cpp:372) | ✅ | ⚠️ | ⚠️ ATOC token path implemented; N-patch tessellation still deferred | ✅ backend pipeline-key ATOC gate | D3D9 adaptive tessellation remains unsupported; vendor ATOC/A2M token behavior is wired |
| ADAPTIVETESS_Z | 182 | ⚠️ `kRsAdaptiveTessZ` (core_state.cpp:373) | ✅ | ⚠️ | ❌ | 🚫 | |
| ADAPTIVETESS_W | 183 | ⚠️ `kRsAdaptiveTessW` (core_state.cpp:374) | ✅ | ⚠️ | ❌ | 🚫 | |
| ENABLEADAPTIVETESSELLATION | 184 | ⚠️ `kRsEnableAdaptiveTessellation` (core_state.cpp:375) | ✅ | ⚠️ | ❌ | 🚫 | |
| TWOSIDEDSTENCILMODE | 185 | ✅ `core::RS_TWO_SIDED_STENCIL_MODE` (core_constants.hpp) | ✅ | ⚠️ | ✅ mode gates front/back stencil descriptors | ✅ stencil_ref_spec (twoSided branch) | Closed 2026-05-24; mode-off mirrors front face |
| CCW_STENCILFAIL | 186 | ✅ `core::RS_STENCIL_CCW_FAIL` (core_constants.hpp:402) | ✅ | ⚠️ | ✅ `dxmt9_draw_state.cpp:250` | ⚠️ | |
| CCW_STENCILZFAIL | 187 | ✅ `core::RS_STENCIL_CCW_ZFAIL` (core_constants.hpp:403) | ✅ | ⚠️ | ✅ `dxmt9_draw_state.cpp:252` | ⚠️ | |
| CCW_STENCILPASS | 188 | ✅ `core::RS_STENCIL_CCW_PASS` (core_constants.hpp:404) | ✅ | ⚠️ | ✅ `dxmt9_draw_state.cpp:254` | ⚠️ | |
| CCW_STENCILFUNC | 189 | ✅ `core::RS_STENCIL_CCW_FUNC` (core_constants.hpp:401) | ✅ | ⚠️ | ✅ `dxmt9_draw_state.cpp:248` | ⚠️ | |
| COLORWRITEENABLE1 | 190 | ✅ `core::RS_COLOR_WRITE_ENABLE1` | ✅ | ⚠️ | ✅ per-RT blend attachment mask | ✅ backend pipeline-key gate | Closed 2026-05-24 |
| COLORWRITEENABLE2 | 191 | ✅ `core::RS_COLOR_WRITE_ENABLE2` | ✅ | ⚠️ | ✅ per-RT blend attachment mask | ✅ backend pipeline-key gate | Closed 2026-05-24 |
| COLORWRITEENABLE3 | 192 | ✅ `core::RS_COLOR_WRITE_ENABLE3` | ✅ | ⚠️ | ✅ per-RT blend attachment mask | ✅ backend pipeline-key gate | Closed 2026-05-24 |
| BLENDFACTOR | 193 | ✅ `core::RS_BLEND_FACTOR` (core_constants.hpp:387) | ✅ | ⚠️ | ✅ `dxmt9_draw_encoder.mm:62` (using-decl); fed into setBlendColor | ⚠️ no spec | RGBA u32 split to 4 floats |
| SRGBWRITEENABLE | 194 | ✅ `core::RS_SRGB_WRITE_ENABLE` (core_constants.hpp:388) | ✅ | ⚠️ | ✅ `dxmt9_pipeline_cache.cpp:629`, `dxmt9_draw_encoder.mm:951` | ⚠️ implicit via pipeline key | |
| DEPTHBIAS | 195 | ✅ `core::RS_DEPTH_BIAS` (core_constants.hpp) | ✅ | ⚠️ | ✅ `dxmt9_draw_encoder.mm` feeds Metal depth bias | ✅ backend depth-bias gate | Closed 2026-05-24 |
| (196 dead) | 196 | n/a | n/a | n/a | n/a | n/a | |
| (197 dead) | 197 | n/a | n/a | n/a | n/a | n/a | |
| WRAP8 | 198 | ✅ `core::RS_WRAP8` | ✅ | ⚠️ | accepted no-op | ✅ state_draw_transform_spec:testWrapRenderStateRoundTrip | Shadowed/readable; representative high-range wrap coverage |
| WRAP9 | 199 | ✅ `core::RS_WRAP9` | ✅ | ⚠️ | accepted no-op | ✅ state_draw_transform_spec:testWrapRenderStateRoundTrip | |
| WRAP10 | 200 | ✅ `core::RS_WRAP10` | ✅ | ⚠️ | accepted no-op | ✅ state_draw_transform_spec:testWrapRenderStateRoundTrip | |
| WRAP11 | 201 | ✅ `core::RS_WRAP11` | ✅ | ⚠️ | accepted no-op | ✅ state_draw_transform_spec:testWrapRenderStateRoundTrip | |
| WRAP12 | 202 | ✅ `core::RS_WRAP12` | ✅ | ⚠️ | accepted no-op | ✅ state_draw_transform_spec:testWrapRenderStateRoundTrip | |
| WRAP13 | 203 | ✅ `core::RS_WRAP13` | ✅ | ⚠️ | accepted no-op | ✅ state_draw_transform_spec:testWrapRenderStateRoundTrip | |
| WRAP14 | 204 | ✅ `core::RS_WRAP14` | ✅ | ⚠️ | accepted no-op | ✅ state_draw_transform_spec:testWrapRenderStateRoundTrip | |
| WRAP15 | 205 | ✅ `core::RS_WRAP15` | ✅ | ⚠️ | accepted no-op | ✅ state_draw_transform_spec:testWrapRenderStateRoundTrip | |
| SEPARATEALPHABLENDENABLE | 206 | ✅ `core::RS_SEPARATE_ALPHA_BLEND_ENABLE` (core_constants.hpp:389) | ✅ | ⚠️ | ✅ `dxmt9_pipeline_cache.cpp:145` | ✅ blend_op_family_spec | |
| SRCBLENDALPHA | 207 | ✅ `core::RS_SRC_BLEND_ALPHA` (core_constants.hpp:390) | ✅ | ⚠️ | ✅ `dxmt9_pipeline_cache.cpp:156` | ✅ blend_op_family_spec | |
| DESTBLENDALPHA | 208 | ✅ `core::RS_DEST_BLEND_ALPHA` (core_constants.hpp:391) | ✅ | ⚠️ | ✅ `dxmt9_pipeline_cache.cpp:159` | ✅ blend_op_family_spec | |
| BLENDOPALPHA | 209 | ✅ `core::RS_BLEND_OP_ALPHA` (core_constants.hpp:392) | ✅ | ⚠️ | ✅ `dxmt9_pipeline_cache.cpp:149` | ✅ blend_op_family_spec | |

### B.2 D3DTEXTURESTAGESTATETYPE (per-stage, 8 stages)

Each entry below is replicated across stage 0..7 (8 stages × table). Backend-read columns assume stage 0 is consumed at minimum; pipeline key spans active stages.

| TSS name | Code | defined | PE-shadow | chunk | backend-read | test | Notes |
|---|---|---|---|---|---|---|---|
| COLOROP | 1 | ✅ `core::TSS_COLOR_OP` (core_constants.hpp:410) | ✅ per-stage table | ⚠️ via `D9CDrawPacketTextureStageState` (`device_c.h:230`) | ✅ `dxmt9_draw_encoder.mm:80`; pipeline key | ✅ ffp_key_determinism_spec:156 | |
| COLORARG1 | 2 | ✅ `core::TSS_COLOR_ARG1` (core_constants.hpp:411) | ✅ | ⚠️ | ✅ `dxmt9_draw_encoder.mm:78` | ✅ ffp_key_determinism_spec:157 | |
| COLORARG2 | 3 | ✅ `core::TSS_COLOR_ARG2` (core_constants.hpp:412) | ✅ | ⚠️ | ✅ `dxmt9_draw_encoder.mm:79` | ✅ ffp_key_determinism_spec:158 | |
| ALPHAOP | 4 | ✅ `core::TSS_ALPHA_OP` (core_constants.hpp:413) | ✅ | ⚠️ | ✅ `dxmt9_draw_encoder.mm:77` | ✅ ffp_key_determinism_spec:159 | |
| ALPHAARG1 | 5 | ✅ `core::TSS_ALPHA_ARG1` (core_constants.hpp:414) | ✅ | ⚠️ | ✅ `dxmt9_draw_encoder.mm:75` | ✅ ffp_key_determinism_spec:160 | |
| ALPHAARG2 | 6 | ✅ `core::TSS_ALPHA_ARG2` (core_constants.hpp:415) | ✅ | ⚠️ | ✅ `dxmt9_draw_encoder.mm:76` | ⚠️ | |
| BUMPENVMAT00 | 7 | ✅ `core::TSS_BUMPENVMAT00` (core_constants.hpp:416) | ✅ | ⚠️ | ✅ `dxmt9_draw_state.cpp:203` (uniform) | ⚠️ ebde43e ran probe | |
| BUMPENVMAT01 | 8 | ✅ `core::TSS_BUMPENVMAT01` (core_constants.hpp:417) | ✅ | ⚠️ | ✅ `dxmt9_draw_state.cpp:204` | ⚠️ ebde43e | |
| BUMPENVMAT10 | 9 | ✅ `core::TSS_BUMPENVMAT10` (core_constants.hpp:418) | ✅ | ⚠️ | ✅ `dxmt9_draw_state.cpp:205` | ⚠️ ebde43e | |
| BUMPENVMAT11 | 10 | ✅ `core::TSS_BUMPENVMAT11` (core_constants.hpp:419) | ✅ | ⚠️ | ✅ `dxmt9_draw_state.cpp:206` | ⚠️ ebde43e | |
| TEXCOORDINDEX | 11 | ✅ `core::TSS_TEXCOORD_INDEX` (core_constants.hpp:421) | ✅ | ⚠️ | ✅ `dxmt9_draw_encoder.mm:81` | ✅ ffp_key_determinism_spec:163, state_draw_transform_spec:619 | |
| (12..21 dead) | 12-21 | n/a | n/a | n/a | n/a | n/a | 10 unused slots |
| BUMPENVLSCALE | 22 | ✅ `core::TSS_BUMPENVLSCALE` (core_constants.hpp:422) | ✅ | ⚠️ | ✅ `dxmt9_draw_state.cpp:209` | ⚠️ ebde43e | |
| BUMPENVLOFFSET | 23 | ✅ `core::TSS_BUMPENVLOFFSET` (core_constants.hpp:423) | ✅ | ⚠️ | ✅ `dxmt9_draw_state.cpp:210` | ⚠️ ebde43e | |
| TEXTURETRANSFORMFLAGS | 24 | ✅ `core::TSS_TEXTURE_TRANSFORM_FLAGS` (core_constants.hpp:424) | ✅ | ⚠️ | ✅ `dxmt9_draw_encoder.mm:82` | ✅ ffp_key_determinism_spec:238, state_draw_transform_spec:620 | |
| (25 dead) | 25 | n/a | n/a | n/a | n/a | n/a | |
| COLORARG0 | 26 | ✅ `core::TSS_COLOR_ARG0` | ✅ | ⚠️ | ✅ consumed by FFP triadic ops | ✅ initial-state matrix + PE defaults + ffp_triadic_msl_spec + GPU readbacks | Default `D3DTA_CURRENT` |
| ALPHAARG0 | 27 | ✅ `core::TSS_ALPHA_ARG0` | ✅ | ⚠️ | ✅ consumed by FFP triadic ops | ✅ initial-state matrix + PE defaults + ffp_triadic_msl_spec + GPU readbacks | Default `D3DTA_CURRENT` |
| RESULTARG | 28 | ✅ `core::TSS_RESULT_ARG` (core_constants.hpp:420) | ✅ | ✅ | ✅ FFP shader routes `D3DTA_TEMP` results through the stage TEMP handoff | ✅ `dxmt9_ffp_tss_resultarg_temp_readback.shader_test` | |
| (29..31 dead) | 29-31 | n/a | n/a | n/a | n/a | n/a | |
| CONSTANT | 32 | ✅ `core::TSS_CONSTANT` (core_constants.hpp:425) | ✅ | ⚠️ | ✅ `dxmt9_draw_state.cpp:201` (uniform), `chunk_replay.cpp:143` | ⚠️ | |

### B.3 D3DSAMPLERSTATETYPE (per-sampler, 16 fragment + 4 vertex)

| SAMP name | Code | defined | PE-shadow | chunk | backend-read | test | Notes |
|---|---|---|---|---|---|---|---|
| ADDRESSU | 1 | ✅ `core::SAMP_ADDRESS_U` (core_constants.hpp:428) | ✅ | ⚠️ via `D9CDrawPacketSamplerState` (`device_c.h:236`) | ✅ `dxmt9_draw_encoder.mm:48` | ✅ state_draw_transform_spec:621 | |
| ADDRESSV | 2 | ✅ `core::SAMP_ADDRESS_V` (core_constants.hpp:429) | ✅ | ⚠️ | ✅ `dxmt9_draw_encoder.mm:49` | ⚠️ | |
| ADDRESSW | 3 | ✅ `core::SAMP_ADDRESS_W` (core_constants.hpp:430) | ✅ | ⚠️ | ✅ `dxmt9_draw_encoder.mm:50` | ⚠️ | |
| BORDERCOLOR | 4 | ✅ `core::SAMP_BORDER_COLOR = 4` + static_assert | ✅ | ⚠️ | ✅ encoder reads slot 4 | ✅ backend_key_descriptor_spec + lifecycle/coverage gates | Closed 2026-05-24 |
| MAGFILTER | 5 | ✅ `core::SAMP_MAG_FILTER` (core_constants.hpp:431) | ✅ | ⚠️ | ✅ `dxmt9_pipeline_cache.cpp:778`, `dxmt9_draw_encoder.mm:52` | ⚠️ | |
| MINFILTER | 6 | ✅ `core::SAMP_MIN_FILTER` (core_constants.hpp:432) | ✅ | ⚠️ | ✅ `dxmt9_pipeline_cache.cpp:777`, `dxmt9_draw_encoder.mm:54` | ⚠️ | |
| MIPFILTER | 7 | ✅ `core::SAMP_MIP_FILTER` (core_constants.hpp:433) | ✅ | ⚠️ | ✅ `dxmt9_draw_encoder.mm:55,714,765` | ⚠️ | |
| MIPMAPLODBIAS | 8 | ✅ `core::SAMP_MIPMAP_LOD_BIAS` | ✅ | ⚠️ | ✅ shader-side `sample(..., bias(b))` at FFP/translated texld sites | ✅ shader_transform_spec + readback mip-selection/zero-control gates | Closed; shader-corpus pixel readbacks validate biased mip selection and zero-bias control |
| MAXMIPLEVEL | 9 | ✅ `core::SAMP_MAX_MIP_LEVEL` | ✅ | ⚠️ | ✅ sampler `lod_min_clamp = max(SetLOD, MAXMIPLEVEL)` | ✅ backend_key_descriptor_spec | Closed; D3D9 SetLOD interaction is modeled as max/coarser clamp |
| MAXANISOTROPY | 10 | ✅ `core::SAMP_MAX_ANISOTROPY` (core_constants.hpp:435) | ✅ | ⚠️ | ✅ `dxmt9_draw_encoder.mm:53` | ✅ state_draw_transform_spec:622,648 | |
| SRGBTEXTURE | 11 | ✅ `core::SAMP_SRGB_TEXTURE` (core_constants.hpp:436) | ✅ | ⚠️ | ✅ `dxmt9_draw_encoder.mm:2077,2139` | ⚠️ | |
| ELEMENTINDEX | 12 | ❌ not defined | ✅ generic | ⚠️ | ❌ | ❌ | Texture-array index; deferred |
| DMAPOFFSET | 13 | ❌ not defined | ✅ generic | ⚠️ | ❌ | ❌ | D3D9 displacement maps; deferred |

### B.4 D3DTRANSFORMSTATETYPE

| TS name | Code | defined | PE-shadow | chunk | backend-read | test | Notes |
|---|---|---|---|---|---|---|---|
| VIEW | 2 | ✅ `core::XFORM_VIEW` (core_constants.hpp:452); `transformStateFromD3D` maps 2→XFORM_VIEW (device_c_format_utils.cpp:355) | ✅ `transformShadow`, slot derived via `FixedTransformTable::slotForState` (d3d9_pe_state_shadow.hpp:189) | ✅ dedicated field `D9CDrawPacketTransform` (`device_c.h:245`, packet array `:336-337`) | ✅ `core_draw.cpp:2185+` derives WVP; `state_draw_transform_spec:789,869` | ✅ state_draw_transform_spec, core_d3d9_multiply_transform_spec | |
| PROJECTION | 3 | ✅ `core::XFORM_PROJECTION` (core_constants.hpp:453); maps 3→XFORM_PROJECTION (device_c_format_utils.cpp:356) | ✅ | ✅ dedicated | ✅ | ✅ state_draw_transform_spec:790,870 | |
| TEXTURE0 | 16 | ✅ `core::XFORM_TEXTURE_BASE+0` (core_constants.hpp:454); maps via `XFORM_TEXTURE_BASE + (state-16)` (device_c_format_utils.cpp:361) | ✅ | ✅ dedicated | ✅ `state_draw_transform_spec:624,663` exercises XFORM_TEXTURE_BASE | ✅ state_draw_transform_spec:624 | |
| TEXTURE1 | 17 | ✅ XFORM_TEXTURE_BASE+1 | ✅ | ✅ | ✅ | ⚠️ | |
| TEXTURE2 | 18 | ✅ +2 | ✅ | ✅ | ✅ | ⚠️ | |
| TEXTURE3 | 19 | ✅ +3 | ✅ | ✅ | ✅ | ⚠️ | |
| TEXTURE4 | 20 | ✅ +4 | ✅ | ✅ | ✅ | ⚠️ | |
| TEXTURE5 | 21 | ✅ +5 | ✅ | ✅ | ✅ | ⚠️ | |
| TEXTURE6 | 22 | ✅ +6 | ✅ | ✅ | ✅ | ⚠️ | |
| TEXTURE7 | 23 | ✅ +7 | ✅ | ✅ | ✅ | ⚠️ | |
| WORLDMATRIX(0) | 256 | ✅ `core::XFORM_WORLD_BASE+0` (core_constants.hpp:450); maps 256→XFORM_WORLD_BASE (device_c_format_utils.cpp:371) | ✅ | ✅ dedicated | ✅ `state_draw_transform_spec:699,868` | ✅ state_draw_transform_spec (9980d5c — P0-2) | aka D3DTS_WORLD |
| WORLDMATRIX(1..3) | 257-259 | ✅ +1..+3 | ✅ | ✅ | ✅ FFP vertex-blend uses 0..3 | ✅ ffp_key_determinism_spec | Vertex blend rigs |
| WORLDMATRIX(4..255) | 260-511 | ✅ +4..+255 (`core::kMaxWorldMatrices = 256`) | ✅ | ✅ | ⚠️ wired through transformStateFromD3D but FFP key only uses up to vertex-blend count | ✅ state_draw_transform_spec:750-778 (9980d5c — P0-2) | Extended skinning range — wired by 9980d5c |

### B.5 D3DLIGHT9 + D3DLIGHTTYPE

D3DLIGHTTYPE (`d3d9types.h:873-879`):

| Type | Code | defined | PE-shadow | chunk | backend-read | test | Notes |
|---|---|---|---|---|---|---|---|
| D3DLIGHT_POINT | 1 | ✅ `D9CLight.type` u32 (device_c.h:169) | ✅ via `lightShadow[]` (d3d9_pe_state_shadow.hpp:469) | ✅ slot mask + array (`packet.lightSlotMask`, `packet.lights[8]`, device_c.h:341-342) | ✅ FFP point-light branch uses position/range/attenuation | ✅ FFP lighting gates | Closed 2026-05-24 |
| D3DLIGHT_SPOT | 2 | ✅ wire | ✅ | ✅ | ✅ FFP spot-light branch uses direction/theta/phi/falloff | ✅ FFP lighting gates | Closed 2026-05-24 |
| D3DLIGHT_DIRECTIONAL | 3 | ✅ wire | ✅ | ✅ | ✅ `ffp_shaders.cpp:164,188` (only supported type) | ⚠️ ffp_key_determinism_spec light fields | |

D3DLIGHT9 struct fields (`d3d9types.h:1408-1422`):

| Field | defined | PE-shadow | chunk | backend-read | test | Notes |
|---|---|---|---|---|---|---|
| Type | ✅ `D9CLight.type` (device_c.h:169) | ✅ | ✅ | ✅ directional/point/spot branches | ⚠️ | |
| Diffuse | ✅ `diffuse` D9CColorRGBA | ✅ | ✅ | ✅ `dxmt9_draw_state.cpp:112` (`lightDiffuse[i]`) | ⚠️ | |
| Specular | ✅ `specular` | ✅ | ✅ | ✅ `dxmt9_draw_state.cpp:113` | ⚠️ | |
| Ambient | ✅ `ambient` | ✅ | ✅ | ✅ `dxmt9_draw_state.cpp:114` | ⚠️ | |
| Position | ✅ `position[3]` | ✅ | ✅ | ✅ transformed from D3D world space by View before point/spot evaluation | ✅ backend_key_descriptor_spec | |
| Direction | ✅ `direction[3]` | ✅ | ✅ | ✅ transformed from D3D world space by View and normalized before directional/spot evaluation | ✅ backend_key_descriptor_spec + ProcessVertices spot cone readback | |
| Range | ✅ float | ✅ | ✅ | ✅ point/spot branch | ✅ ProcessVertices point range cutoff readback | |
| Falloff | ✅ | ✅ | ✅ | ✅ spot branch | ✅ ProcessVertices spot falloff readback | |
| Attenuation0 | ✅ | ✅ | ✅ | ✅ point/spot branch | ✅ ProcessVertices point-light readback | |
| Attenuation1 | ✅ | ✅ | ✅ | ✅ point/spot branch | ✅ ProcessVertices point-light readback | |
| Attenuation2 | ✅ | ✅ | ✅ | ✅ point/spot branch | ✅ ProcessVertices point-light readback | |
| Theta | ✅ | ✅ | ✅ | ✅ spot branch | ✅ ProcessVertices spot cone readback | |
| Phi | ✅ | ✅ | ✅ | ✅ spot branch | ✅ ProcessVertices spot cone readback | |

LightEnable: ✅ `packet.lightEnableValidMask` + `lightEnableMask` (device_c.h:343-344); shadow `lightEnableShadow` (d3d9_pe_state_shadow.hpp:472); read into `key.lightEnabled[]` in FFP key.

### B.6 D3DMATERIAL9 (`d3d9types.h:1440-1446`)

| Field | defined | PE-shadow | chunk | backend-read | test | Notes |
|---|---|---|---|---|---|---|
| Diffuse | ✅ `D9CMaterial.diffuse` (device_c.h:164) | ✅ `materialShadow` (d3d9_pe_state_shadow.hpp:463) | ✅ dedicated `packet.material` + `materialValid` (device_c.h:328-329) | ✅ `dxmt9_draw_state.cpp:105` (`materialDiffuse`) | ⚠️ no dedicated material spec | |
| Ambient | ✅ `ambient` | ✅ | ✅ | ✅ `dxmt9_draw_state.cpp:104` | ⚠️ | |
| Specular | ✅ `specular` | ✅ | ✅ | ✅ `dxmt9_draw_state.cpp:106` | ⚠️ | |
| Emissive | ✅ `emissive` | ✅ | ✅ | ✅ `dxmt9_draw_state.cpp:103` | ⚠️ | |
| Power | ✅ `power` float (device_c.h:165) | ✅ | ✅ | ✅ `dxmt9_draw_state.cpp:109` (`materialPower`) | ⚠️ | |

### B.7 D3DVIEWPORT9 (`d3d9types.h:1541-1548`)

| Field | defined | PE-shadow | chunk | backend-read | test | Notes |
|---|---|---|---|---|---|---|
| X | ✅ `D9CViewport.x` (device_c.h:53) | ✅ `viewportShadow` (d3d9_pe_state_shadow.hpp:452) | ✅ dedicated `packet.viewport` + `viewportValid` (device_c.h:318-319) | ✅ encoder `setViewport` consumer | ⚠️ | |
| Y | ✅ `y` | ✅ | ✅ | ✅ | ⚠️ | |
| Width | ✅ `width` | ✅ | ✅ | ✅ | ⚠️ | |
| Height | ✅ `height` | ✅ | ✅ | ✅ | ⚠️ | |
| MinZ | ✅ `minZ` float | ✅ | ✅ | ✅ | ⚠️ | |
| MaxZ | ✅ `maxZ` float | ✅ | ✅ | ✅ | ⚠️ | |

### B.8 D3DCLIPSTATUS9 (`d3d9types.h:1287-1290`)

| Field | defined | PE-shadow | chunk | backend-read | test | Notes |
|---|---|---|---|---|---|---|
| ClipUnion | 🟡 no hardware clip accumulation | 🟡 Wine-matching default policy | n/a | 🟡 `SetClipStatus` accepts without storing; `GetClipStatus` returns defined all-visible default | ✅ core_device_com_spec + conformance gate | Closed as Wine-matching stub/default; not a rendering backend feature |
| ClipIntersection | 🟡 | 🟡 | n/a | 🟡 same policy | ✅ | |

### B.9 D3DGAMMARAMP (`d3d9types.h:1385-1389`)

| Field | defined | PE-shadow | chunk | backend-read | test | Notes |
|---|---|---|---|---|---|---|
| red[256] | ✅ `D9CGammaRamp`/gamma shadow | ✅ | ✅ unix bridge | ✅ Set/Get round-trip and present-path gamma application | ✅ core_d3d9_gamma_ramp_spec | Closed; identity ramp is default |
| green[256] | ✅ | ✅ | ✅ | ✅ | ✅ | |
| blue[256] | ✅ | ✅ | ✅ | ✅ | ✅ | |

### B.10 Section summary

| Category | Total slots | ✅ defined | ✅ backend-read | ✅ tested (named-spec) | ❌ gaps (worst 5) |
|---|---|---|---|---|---|
| RS (named D3D9 slots) | 102 | Named/default/shadow coverage now includes depth bias, MRT color-write masks, WRAP0..15, COLORVERTEX, two-sided stencil, and MULTISAMPLEMASK | Core rendering states wired into draw_state / pipeline_cache / encoder; accepted no-op states are documented | Covered by ffp_key_determinism, blend_op_family, stencil_ref, state_draw_transform, draw_uniforms_layout, encode_draw_recorder, and backend pipeline-key gates | Remaining deferred/no-op: MULTISAMPLEANTIALIAS, ANTIALIASEDLINEENABLE, N-patch/adaptive tessellation states |
| TSS | 21 named (1..28 minus 13 dead) | Triadic ARG0 states and RESULTARG are defined | COLOR/ALPHA ops, bump-env uniforms, transform flags, constants, triadic ARG0, and RESULTARG TEMP handoff are consumed | ffp_key_determinism_spec, ffp_triadic_msl_spec, RESULTARG GPU readback | BUMPENVMAP retains partial per-stage evidence coverage |
| SAMP | 13 named | `BORDERCOLOR`, `MIPMAPLODBIAS`, `MAXMIPLEVEL` are defined and consumed | Address/filter/border/aniso/sRGB/lod-bias/max-mip wired | state_draw_transform_spec, backend_key_descriptor_spec, shader_transform_spec/readbacks | Deferred: ELEMENTINDEX, DMAPOFFSET |
| Transform | 266 unique D3D9 codes (2, 3, 16-23, 256-511) | All 266 mapped via `transformStateFromD3D`; widened to (0..255) by 9980d5c | All read by core_draw + draw_state | Covered by state_draw_transform_spec (VIEW, PROJECTION, WORLD(0,5,254), TEXTURE0) | WORLD(4..253) not individually tested (only 5 and 254 sampled) |
| D3DLIGHT9 | 13 fields × 8 slots + Type + LightEnable | All fields defined in `D9CLight` | Directional, point, and spot FFP branches consume the relevant fields | Light fields exercised by ffp_key_determinism/lighting gates | No current point/spot implementation gap; residual risk is visual edge coverage |
| D3DMATERIAL9 | 5 fields | All 5 in `D9CMaterial` | All 5 read | No dedicated material-roundtrip spec | No isolated material spec — implicitly covered via ffp probes |
| D3DVIEWPORT9 | 6 fields | All in `D9CViewport` | All read | ⚠️ no dedicated viewport-roundtrip spec | Same — implicitly covered by encoder layer |
| D3DCLIPSTATUS9 | 2 fields | Wine-matching default policy | Stub/default by design | core_device_com_spec + conformance gate | No hardware clip-status accumulation planned |
| D3DGAMMARAMP | 3 × 256 fields | Gamma ramp shadow + unix bridge | Present path applies non-identity ramp | core_d3d9_gamma_ramp_spec | Closed; identity ramp remains fast path |

**Current remaining deferred/no-op list, ordered by likely user-visible impact:**

1. **N-patch / adaptive tessellation / patch draw path** — unsupported by design; declaration methods safe-reject and patch draws return invalid-call/no-op contracts.
2. **Broad `ProcessVertices` / real SWVP execution** — the covered fixed-function and programmable subsets are extensive and now include explicit `DP3` and `DP4` readback coverage; texture/sample opcodes beyond 2D TEXLDL plus remaining clipping, lighting, and exotic declaration edge cases remain deferred. See the live `ProcessVertices` row above for the exact implemented subset.
3. **Palette/P8 sampling breadth** — 2D/cube/volume sampling is implemented through palette-expanded A8R8G8B8 backing; direct core P8/A8P8 storage remains unsupported.
4. **`D3DSAMP_ELEMENTINDEX` / `DMAPOFFSET`** — texture-array/displacement-map semantics deferred.
5. **`D3DRS_ANTIALIASEDLINEENABLE`** — accepted no-op/deferred due Metal rasterizer mismatch.
6. **`SetConvolutionMonoKernel` / `ComposeRects`** — explicit `E_NOTIMPL`.
7. **Runtime validation** — tracked GPU probes for RESZ, NULL RT, and MIPMAPLODBIAS are covered; remaining validation work is Wine-run coverage of PE gates.

## Methodology

Commands used to build this inventory (run from
`/Users/dididi/workspaces/dxmt9`):

```sh
# 1. Wine reference — RS / TSS / SAMP / TS / LIGHTTYPE / structs.
grep -n "D3DRS_\|D3DRENDERSTATETYPE" ~/workspaces/wine/include/d3d9types.h
grep -n "D3DTSS_\|D3DSAMP_\|D3DTRANSFORMSTATETYPE\|D3DTS_\|D3DLIGHTTYPE" \
  ~/workspaces/wine/include/d3d9types.h
grep -n "D3DLIGHT9\|D3DMATERIAL9\|D3DVIEWPORT9\|D3DCLIPSTATUS9\|D3DGAMMARAMP" \
  ~/workspaces/wine/include/d3d9types.h

# 2. dxmt9 constant defines.
grep -n "RS_\|TSS_\|SAMP_\|XFORM_" include/dxmt9/core_constants.hpp
grep -n "constexpr u32 kRs\|renderStates.set\|RS_\|TSS_\|SAMP_" \
  src/d3d9/core_state.cpp

# 3. PE shadow surface.
grep -n "renderStateShadow\|tssShadow\|samplerStateShadow\|transformShadow\|\
materialShadow\|viewportShadow\|lightShadow\|lightEnableShadow\|\
clipPlaneShadow" src/d3d9/d3d9_pe_state_shadow.hpp

# 4. Chunk packet wire fields.
# D9CDrawPrimitivePacket and D9CDrawPacketRenderState were deleted with the
# legacy record format; render states now ride D9CCommandChunkWireRenderState.
grep -n "D9CCommandChunkWireRenderState\|\
D9CDrawPacketTextureStageState\|D9CDrawPacketSamplerState\|\
D9CDrawPacketTransform\|D9CMaterial\|D9CLight\|D9CViewport" \
  include/dxmt9/device_c.h

# 5. Backend consumers.
rg -n "core::RS_|core::TSS_|core::SAMP_|core::XFORM_" \
  src/dxmt9 src/d3d9 | grep -v core_constants
rg -n "renderState|samplerState|textureStageState" \
  src/dxmt9/dxmt9_draw_state.cpp \
  src/dxmt9/dxmt9_pipeline_cache.cpp \
  src/dxmt9/dxmt9_ffp_shaders.cpp \
  src/dxmt9/dxmt9_draw_encoder.mm

# 6. Tests.
rg -ln "RS_|TSS_|SAMP_|XFORM_|D3DRS_|D3DTSS_|D3DSAMP_|D3DTS_" \
  tests/native
rg -n "RS_|TSS_|SAMP_|XFORM_" \
  tests/native/backend/ffp_key_determinism_spec.cpp \
  tests/native/backend/stencil_ref_spec.cpp \
  tests/native/backend/blend_op_family_spec.cpp \
  tests/native/core/state_draw_transform_spec.cpp \
  tests/native/core/core_ffp_state_key_spec.cpp

# 7. Stub-only API surface.
rg -n "SetClipStatus|GetClipStatus|SetGammaRamp|GetGammaRamp" \
  src/d3d9

# 8. Commit anchors for recent fixes.
git log --oneline --grep="STENCIL\|POINTSPRITE\|WORLDMATRIX\|stencil_ref"
```

Anchors are file-relative to repository root (`/Users/dididi/workspaces/dxmt9`).
Commit refs verified against `git log --oneline -50` at HEAD.
## C. Format + Caps + Present Parameters + Display Mode

Audit anchored against the D3D9 SDK headers `~/workspaces/wine/include/d3d9types.h` and
`~/workspaces/wine/include/d3d9caps.h`. Source under `/Users/dididi/workspaces/dxmt9/`.

Source anchors used throughout:
- `src/d3d9/core_format.cpp` — `formatTable()` (lines 34-134), `makeDefaultCaps()` (lines 487-574).
- `include/dxmt9/core_constants.hpp:242-292` — `enum class Format` (PE-side / core mirror).
- `src/d3d9/device_c_format_utils.cpp:38-145` — `fmtFromD3D` / `fmtToD3D` (D3DFMT_* code <-> core enum).
- `src/dxmt9/dxmt9_format_convert.cpp:21-180` — `toPixelFormat(...)` (core -> WMTPixelFormat).
- `src/d3d9/d3d9_pe_factory.cpp:46-58` — `isSupportedAdapterModeFormat` / `isValidCheckDeviceAdapterFormat`.
- `src/d3d9/d3d9_pe_factory.cpp:249-327` — `fillD3DCaps9` (D3DCAPS9 <- D9CCaps).
- `src/d3d9/device_c_format_utils.cpp:275-351` — `fillCCaps` (D9CCaps <- core DeviceCaps).
- `src/d3d9/core_present.cpp:10-140` — present-parameter validation + normalization.
- `src/d3d9/core_factory.cpp:251-285` — `getAdapterIdentifier` / `getAdapterDisplayMode` / `enumAdapterModes`.
- `src/d3d9/d3d9_pe_device.cpp:4725-4747` — `IDirect3DDevice9::GetRasterStatus` synthetic scanline impl.

### C.1 D3DFORMAT — Standard color/alpha (UNKNOWN .. A16B16G16R16)

| Name | Code | defined | mapped | runtime-use | test | Notes |
|---|---|---|---|---|---|---|
| UNKNOWN | 0 | OK | — | OK | OK | sentinel; `formatTable()` returns no entry; `fmtFromD3D(0) -> Unknown`; `Reset` validates rejection (`d3d9_conformance_device.c:68`). |
| R8G8B8 | 20 | OK | NO | NO | OK | `Format::R8G8B8` exists with `BackendPixelFormat::Unknown`, `FormatClass::Unsupported`; `formatSupportsUsage` rejects any non-zero usage (`core_format.cpp:450-452`). Tested via `core_format_caps_spec.cpp:21` for empty backend. |
| A8R8G8B8 | 21 | OK | OK | OK | OK | -> `BGRA8Unorm`; renderTarget capable; default adapter display format; the workhorse of every conformance test. |
| X8R8G8B8 | 22 | OK | OK | OK | OK | -> `BGRA8Unorm` (alpha bit ignored); exposed as primary "windowed display" adapter format. |
| R5G6B5 | 23 | OK | OK | OK | OK | -> `B5G6R5Unorm`; allowed adapter mode format (`isSupportedAdapterModeFormat`). |
| X1R5G5B5 | 24 | OK | OK | OK | warn | -> `BGR5A1Unorm` (treated as A1R5G5B5; alpha forced opaque); allowed CheckDeviceFormat adapter format. |
| A1R5G5B5 | 25 | OK | OK | OK | warn | -> `BGR5A1Unorm`. |
| A4R4G4B4 | 26 | OK | OK | OK | warn | -> `ABGR4Unorm`. |
| R3G3B2 | 27 | NO | NO | NO | NO | absent from `Format` enum; `fmtFromD3D` returns Unknown; only referenced as a fallback case in `userMemoryBytesPerPixel` (`d3d9_pe_device.cpp:203` returns 1). |
| A8 | 28 | OK | OK | OK | warn | -> `A8Unorm`. |
| A8R3G3B2 | 29 | NO | NO | NO | NO | absent from `Format` enum and `fmtFromD3D`. |
| X4R4G4B4 | 30 | NO | NO | NO | NO | absent from `Format` enum; referenced only by `userMemoryBytesPerPixel` -> 2 (`d3d9_pe_device.cpp:195`, `d3d9_pe_device_child_surface.cpp:211`). No core mapping; CheckDeviceFormat returns NOTAVAILABLE. |
| A2B10G10R10 | 31 | OK | maybe | OK | OK | -> `BGR10A2Unorm`, `FormatClass::Optional` gated on `BackendLimits::supportsBgr10A2`; falsy default Mac8 limits route to NOTAVAILABLE (`core_format_caps_spec.cpp:72`). |
| A8B8G8R8 | 32 | OK | OK | OK | OK | -> `RGBA8Unorm`. |
| X8B8G8R8 | 33 | OK | OK | OK | warn | -> `RGBA8Unorm`. |
| G16R16 | 34 | OK | OK | OK | warn | -> `RG16Unorm`. |
| A2R10G10B10 | 35 | OK | OK | OK | OK | -> `RGB10A2Unorm` (Required). |
| A16B16G16R16 | 36 | OK | OK | OK | warn | -> `RGBA16Unorm`. |

### C.2 D3DFORMAT — Palette / Luminance / Bumpmap / etc.

| Name | Code | defined | mapped | runtime-use | test | Notes |
|---|---|---|---|---|---|---|
| A8P8 | 40 | PE | PE | partial | native + PE visual scaffold | 2D/cube/volume PE `CheckDeviceFormat` texture queries report support; texture/cube/volume creation uses an A8R8G8B8 core backing, exposes 2-byte index+alpha locks, and expands palette RGB plus texel alpha before sampling, including matching palettized `UpdateTexture` index/alpha shadow copies that preserve the destination palette (`dxmt9-core-device-com-spec`). `dxmt9-core-device-com-spec` covers A8P8 palette-expanded `dxmt9c_texture_sample_2d` readback including texel alpha locked-palette-switch samples and destination palette-switch samples plus cube/volume A8P8 backend expansion with texel alpha preserved, and `test_visual_p8_texture_sampler_policy` covers 2D A8P8 fixed-function and ps_2_0 `texld` sampling into a render target with pixel readback, including texel alpha, level-1 `SetLOD` sampling, same-slot palette updates, current-palette index switches while the texture is bound, current-palette-before-bind sampling, SYSTEMMEM-to-DEFAULT `UpdateTexture` destination sampling before and after a destination palette switch, ps_2_0 `samplerCUBE` sampling across all six cube faces, and ps_2_0 `sampler3D` sampling across four volume slices; `test_visual_process_vertices_xyzhw_policy` covers A8P8 ProcessVertices vertex-texture TEXLDL destination-vertex readback plus fixed-function and programmable VS SWVP DrawPrimitiveUP sampling of an A8P8 SYSTEMMEM-to-DEFAULT `UpdateTexture` destination with texel alpha, including re-sampling after a same-slot destination palette update and current palette index switch, plus fixed-function and programmable VS bound indexed SWVP sampling after the current palette switch. `Format::A8P8` / `fmtFromD3D` exposure is explicit for metadata and pitch; direct core storage and non-texture caps remain unsupported. |
| P8 | 41 | PE | PE | partial | native + PE visual scaffold | 2D/cube/volume PE `CheckDeviceFormat` texture queries report support; texture/cube/volume creation uses an A8R8G8B8 core backing, exposes 1-byte index locks, and expands through the current palette before sampling, including palette changes deferred while the subresource is locked and matching palettized `UpdateTexture` index shadow copies that preserve the destination palette (`dxmt9-core-device-com-spec`). `dxmt9-core-device-com-spec` covers P8/A8P8 palette-expanded `dxmt9c_texture_sample_2d` readback, including locked-palette-switch and UpdateTexture destination palette-switch samples plus cube/volume P8 backend expansion, and `test_visual_p8_texture_sampler_policy` covers 2D P8 fixed-function and ps_2_0 `texld` sampling into a render target with pixel readback, including level-1 `SetLOD` sampling, same-slot palette updates, current-palette index switches while the texture is bound, current-palette-before-bind sampling, SYSTEMMEM-to-DEFAULT `UpdateTexture` destination sampling before and after a destination palette switch, ps_2_0 `samplerCUBE` sampling across all six cube faces, and ps_2_0 `sampler3D` sampling across four volume slices; `test_visual_process_vertices_xyzhw_policy` covers P8/A8P8 ProcessVertices vertex-texture TEXLDL destination-vertex readback including same-slot and current-index P8/A8P8 bound palette updates plus P8 current-palette-before-bind vertex sampler readback, and fixed-function and programmable VS SWVP DrawPrimitiveUP sampling of a P8 SYSTEMMEM-to-DEFAULT `UpdateTexture` destination, including re-sampling after a same-slot destination palette update and current palette index switch, plus fixed-function and programmable VS bound indexed SWVP sampling after the current palette switch. `Format::P8` / `fmtFromD3D` exposure is explicit for metadata and pitch; direct core storage and non-texture caps remain unsupported. |
| L8 | 50 | OK | OK | OK | OK | -> `R8Unorm`; CheckDeviceFormat denies `UsageRenderTarget` (test `core_format_caps_spec.cpp:70`). |
| A8L8 | 51 | OK | OK | OK | warn | -> `RG8Unorm`. |
| A4L4 | 52 | NO | NO | NO | NO | absent; referenced only by `userMemoryBytesPerPixel` -> 1. |
| V8U8 | 60 | OK | OK | OK | OK | -> `RG8Snorm`. Bumpmap covered by `core_format_caps_spec.cpp:89-97`. |
| L6V5U5 | 61 | NO | NO | NO | NO | absent. |
| X8L8V8U8 | 62 | NO | NO | NO | NO | absent. |
| Q8W8V8U8 | 63 | OK | OK | OK | OK | -> `RGBA8Snorm`; `core_format_caps_spec.cpp:100-105`. |
| V16U16 | 64 | OK | OK | OK | OK | -> `RG16Snorm`; `core_format_caps_spec.cpp:109-114`. |
| A2W10V10U10 | 67 | NO | NO | NO | NO | absent. |
| L16 | 81 | OK | OK | OK | warn | -> `R16Unorm` (entry in `formatTable`); `fmtFromD3D(81) -> L16`. (Code 81 is between depth codes — note the D3D code is correct: `D3DFMT_L16 = 81`.) |
| Q16W16V16U16 | 110 | NO | NO | NO | NO | absent from `Format` enum. |
| CxV8U8 | 117 | OK | NO | NO | OK | declared `Format::CxV8U8`, backend `Unknown`, `FormatClass::Unsupported`. CheckDeviceFormat returns NOTAVAILABLE (`core_format_caps_spec.cpp:138-148`). |

### C.3 D3DFORMAT — Depth/stencil

| Name | Code | defined | mapped | runtime-use | test | Notes |
|---|---|---|---|---|---|---|
| D16_LOCKABLE | 70 | OK | OK | OK | warn | -> `Depth16Unorm`. |
| D32 | 71 | OK | OK | OK | warn | -> `Depth32Float`. |
| D15S1 | 73 | OK | NO | NO | NO | declared but backend `Unknown`, `FormatClass::Unsupported`. |
| D24S8 | 75 | OK | OK | OK | OK | -> `Depth24Unorm_Stencil8` (Mac usually unsupported; on Apple-silicon it returns `Depth32Float_Stencil8` via `toPixelFormat` Mac fallback). Heavily tested in conformance. |
| D24X8 | 77 | OK | OK | OK | OK | -> `Depth24Unorm_Stencil8` (stencil masked). |
| D24X4S4 | 79 | OK | NO | NO | NO | declared but backend `Unknown`, `FormatClass::Unsupported`. |
| D16 | 80 | OK | OK | OK | OK | -> `Depth16Unorm`. |
| D32F_LOCKABLE | 82 | OK | OK | OK | warn | -> `Depth32Float`. |
| D24FS8 | 83 | OK | maybe | OK | warn | -> `Depth32Float_Stencil8` gated on `BackendLimits::supportsDepth32FloatStencil8` (`FormatClass::Optional`). |
| D32_LOCKABLE | 84 | NO | NO | NO | NO | absent from `Format` enum and `fmtFromD3D`; called out by `CheckDeviceFormat`'s sampleable-depth NOTAVAILABLE rejection (`d3d9_pe_factory.cpp:540`). |
| S8_LOCKABLE | 85 | OK | NO | NO | warn | declared but backend `Unknown`; bytes-per-pixel 1. |

### C.4 D3DFORMAT — FOURCC standard (video + DXT)

| Name | FOURCC | defined | mapped | runtime-use | test | Notes |
|---|---|---|---|---|---|---|
| UYVY | 'UYVY' | NO | NO | NO | warn | absent from `Format` enum; conformance harness queries it (`d3d9_conformance_*`) only to test rejection. |
| R8G8_B8G8 | 'RGBG' | NO | NO | NO | NO | absent. |
| YUY2 | 'YUY2' | NO | NO | NO | warn | absent from `Format`; conformance queries for rejection only. |
| G8R8_G8B8 | 'GRGB' | NO | NO | NO | NO | absent. |
| DXT1 | 'DXT1' (827611204) | OK | OK | OK | OK | -> `BC1_RGBA`. Tested in `core_format_caps_spec.cpp:25-28` + conformance. |
| DXT2 | 'DXT2' (844388420) | OK | OK | OK | warn | -> `BC2_RGBA` (alpha pre-multiplied treated as BC2 for now). |
| DXT3 | 'DXT3' (861165636) | OK | OK | OK | warn | -> `BC2_RGBA`. |
| DXT4 | 'DXT4' (877942852) | OK | OK | OK | warn | -> `BC3_RGBA`. |
| DXT5 | 'DXT5' (894720068) | OK | OK | OK | OK | -> `BC3_RGBA`. |
| MULTI2_ARGB8 | 'MET1' | NO | NO | NO | NO | absent. |

### C.5 D3DFORMAT — Index buffer + reserved

| Name | Code | defined | mapped | runtime-use | test | Notes |
|---|---|---|---|---|---|---|
| INDEX16 | 101 | OK | — | OK | OK | logical index format; no Metal pixel format mapping required (index buffers use buffer indexType). |
| INDEX32 | 102 | OK | — | OK | OK | as above. |
| Float R16F | 111 | OK | OK | OK | warn | -> `R16Float`. |
| Float G16R16F | 112 | OK | OK | OK | warn | -> `RG16Float`. |
| Float A16B16G16R16F | 113 | OK | OK | OK | OK | -> `RGBA16Float`. |
| Float R32F | 114 | OK | OK | OK | OK | -> `R32Float`. |
| Float G32R32F | 115 | OK | OK | OK | warn | -> `RG32Float`. |
| Float A32B32G32R32F | 116 | OK | OK | OK | warn | -> `RGBA32Float`. |

### C.6 D3DFORMAT — FOURCC vendor pseudo (INTZ, DF16, etc.)

These are vendor-defined pseudo-formats not in `d3d9types.h`. Current status
is summarized here; the re-audit delta above remains the authoritative
closure list for behavioral gates.

| Name | FOURCC | defined | mapped | runtime-use | test | Notes |
|---|---|---|---|---|---|---|
| INTZ | 'INTZ' (0x5a544e49) | OK | OK | OK | OK | depth-as-texture pseudo-format backed by Depth32Float; shader read + depth usage covered. |
| DF16 | 'DF16' (0x36314644) | OK | OK | OK | OK | DF16 vendor depth-as-texture path mirrors INTZ policy with Depth16Unorm storage. |
| DF24 | 'DF24' (0x34324644) | OK | OK | OK | OK | DF24 vendor depth-as-texture path mirrors INTZ/Depth32Float policy. |
| RAWZ | 'RAWZ' (0x5a574152) | OK | OK | NO | OK | explicitly classified unsupported; CheckDeviceFormat returns NOTAVAILABLE. |
| RESZ | 'RESZ' (0x5a534552) | OK | OK | OK | OK | write-only sentinel command; MSAA depth resolve into INTZ has shader-corpus readback coverage. |
| NULL | 'NULL' (0x4c4c554e) | OK | OK | OK | OK | colorless render target with no color backing; NULL RT depth-only runtime probe covered. |
| ATOC | 'ATOC' (0x434f5441) | OK | OK | OK | OK | alpha-to-coverage token path wired through pipeline key. |
| NVDB | 'NVDB' (0x4244564e) | OK | OK | NO | OK | explicitly classified unsupported; Metal has no depth-bounds equivalent. |
| GET4 | 'GET4' (0x34544547) | NO | NO | partial | warn | absent from `Format` enum, but explicitly short-circuited as NOTAVAILABLE in `device_c_factory.cpp:179, 186` (CheckDeviceFormat path) with a Wine-policy comment. |
| GET1 | 'GET1' (0x31544547) | NO | NO | NO | NO | absent. |
| R2VB | 'R2VB' | NO | NO | NO | NO | absent. |
| ATI1 | 'ATI1' (826889281) | OK | OK | OK | OK | -> `BC4_RUnorm` (single-channel BC4). `core_format_caps_spec.cpp:122`. |
| ATI2 | 'ATI2' (843666497) | OK | OK | OK | OK | -> `BC5_RGUnorm` (two-channel BC5). `core_format_caps_spec.cpp:118-135`. |

### C.7 D3DCAPS9 fields

Anchor: `include/dxmt9/core_constants.hpp:866-944` (struct), `src/d3d9/core_format.cpp:487-574` (`makeDefaultCaps`),
`src/d3d9/device_c_format_utils.cpp:275-351` (`fillCCaps`), `src/d3d9/d3d9_pe_factory.cpp:249-327` (`fillD3DCaps9`).

Legend column "Filled by makeDefaultCaps?": OK = explicit assignment in `makeDefaultCaps`; "default" = struct in-class
initializer carries an acceptable non-zero value; NO = zero-init only (i.e. the field reaches Windows apps as 0 / FALSE).

| Field | Filled by makeDefaultCaps? | Value | runtime-use | test | Notes |
|---|---|---|---|---|---|
| DeviceType | NO (caller sets) | `DeviceType::Hal` (struct default) | OK | OK | not in `makeDefaultCaps`; comes from the Factory bootstrap and conformance asserts `D3DDEVTYPE_HAL`. |
| AdapterOrdinal | NO | `0` (D9CCaps zero-init in `fillCCaps`) | warn | OK | tests check `caps.AdapterOrdinal` is `D3DADAPTER_DEFAULT (0)`. Acceptable on single-adapter Macs. |
| Caps | OK | `0x00000000` (no `D3DCAPS_READ_SCANLINE`) | OK | NO | dxmt9 declines scanline-read. |
| Caps2 | OK | `0x60020000` (FULLSCREENGAMMA \| CANAUTOGENMIPMAP \| DYNAMICTEXTURES) | OK | NO | |
| Caps3 | OK | `0x320` (ALPHA_FULLSCREEN_FLIP_OR_DISCARD \| COPY_TO_VIDMEM \| COPY_TO_SYSTEMMEM) | OK | NO | |
| PresentationIntervals | OK | `0x80000001` (IMMEDIATE | ONE) | OK | NO | matches the `core_present.cpp` validator's TWO/THREE/FOUR acceptance only at validation level. |
| CursorCaps | OK | `0` | OK | OK | R-CAPS-9 keeps hardware cursor caps clear while cursor methods remain validation/shadow-only; `core_format_caps_spec` pins core and C-ABI values. |
| DevCaps | OK | `0x0019aff0` | OK | NO | |
| PrimitiveMiscCaps | OK | `0x002ecff2` | OK | OK | tested in `d3d9_conformance_device.c` cap-presence checks. |
| RasterCaps | OK | `0x07332190` | OK | OK | DITHER deliberately clear under R-CAPS-8 while DITHERENABLE is shadow-only. |
| ZCmpCaps | OK | `0xff` (all 8 comparison ops) | OK | NO | |
| SrcBlendCaps | OK | `0x3fff` (full DX9 blend factor set) | OK | NO | |
| DestBlendCaps | OK | `0x27ff` | OK | NO | |
| AlphaCmpCaps | OK (named `alphaCmpCaps`) | `0xff` | OK | OK | `fillCCaps` and `fillD3DCaps9` source the dedicated `alphaCmpCaps` field; `core_format_caps_spec` pins the public mask. |
| ShadeCaps | OK | `0x84208` (COLORGOURAUDRGB | SPECULARGOURAUDRGB | ALPHAGOURAUDBLEND | FOGGOURAUD) | OK | NO | |
| TextureCaps | OK | `0x0001ec85` | OK | OK | |
| TextureFilterCaps | OK | `0x07030700` | OK | NO | |
| CubeTextureFilterCaps | OK | `0x07030700` | OK | NO | |
| VolumeTextureFilterCaps | OK | `0x03030300` | OK | NO | |
| TextureAddressCaps | OK | `0x1f` (WRAP/MIRROR/CLAMP/BORDER/INDEPENDENTUV) | OK | NO | MIRRORONCE (`0x20`) is not advertised. |
| VolumeTextureAddressCaps | OK | `0x1f` | OK | NO | |
| LineCaps | OK | `0x1f` | OK | OK | tested via `caps.LineCaps`. |
| MaxTextureWidth | OK | `min(16384, limits.maxTextureSize)` | OK | OK | tested as >= 1. |
| MaxTextureHeight | OK | `min(16384, limits.maxTextureSize)` | OK | OK | tested as >= 1. |
| MaxVolumeExtent | default | `2048` | OK | OK | not set in `makeDefaultCaps`; struct default. |
| MaxTextureRepeat | OK | `32768` | OK | OK | tested as >= 1. |
| MaxTextureAspectRatio | OK | `16384` | OK | NO | |
| MaxAnisotropy | OK | `limits.maxAnisotropy` | OK | OK | tested == 8 in `core_format_caps_spec.cpp:47`. |
| MaxVertexW | default | `1.0e10f` (struct default) | OK | NO | |
| GuardBandLeft / Top / Right / Bottom | default | `-8192 / -8192 / 8192 / 8192` | OK | NO | |
| ExtentsAdjust | default | `0.0f` | OK | NO | |
| StencilCaps | OK | `0x1ff` (KEEP|ZERO|REPLACE|INCRSAT|DECRSAT|INVERT|INCR|DECR|TWOSIDED) | OK | NO | |
| FVFCaps | OK | `0x00100008` (DONOTSTRIPELEMENTS | TEXCOORDCOUNTMASK=8) | OK | NO | |
| TextureOpCaps | OK (named `textureBlendCaps`) | `0x03feffff` | OK | OK | tested in conformance. |
| MaxTextureBlendStages | NO in `makeDefaultCaps`; hard-coded in `fillCCaps` | `8` | OK | OK | `device_c_format_utils.cpp:343` always writes `8`; tested as >= 1. |
| MaxSimultaneousTextures | OK | `8` (kMaxRenderTargets-ish const) | OK | OK | |
| VertexProcessingCaps | OK | `0x0000013b` | OK | NO | |
| MaxActiveLights | OK | `kMaxLights` | OK | OK | tested. |
| MaxUserClipPlanes | OK | `8` | OK | OK | Explicitly assigned by `makeDefaultCaps`; matches core clip-plane storage. |
| MaxVertexBlendMatrices | default | `4` | OK | NO | |
| MaxVertexBlendMatrixIndex | OK | `0` (writes the same value as default) | OK | NO | |
| MaxPointSize | OK | `64.0f` | OK | NO | |
| MaxPrimitiveCount | OK | `5592405` (=0x555555) | OK | OK | tested >= 1. |
| MaxVertexIndex | default | `16777215` | OK | OK | tested >= 1. |
| MaxStreams | OK | `kMaxStreams` | OK | OK | tested. |
| MaxStreamStride | OK | `1024` | OK | NO | |
| VertexShaderVersion | OK | `0xfffe0300` (vs_3_0) | OK | NO | |
| MaxVertexShaderConst | OK | `kMaxVertexConstants` (256) | OK | OK | tested via SetVertexShaderConstantF bounds check. |
| PixelShaderVersion | OK | `0xffff0300` (ps_3_0) | OK | OK | tested. |
| PixelShader1xMaxValue | OK | `1024.0f` | OK | NO | |
| DevCaps2 | OK | `0x51` (STREAMOFFSET \| CAN_STRETCHRECT_FROM_TEXTURES \| VERTEXELEMENTSCANSHARESTREAMOFFSET) | OK | NO | No displacement-map or adaptive-tessellation bits. |
| MaxNpatchTessellationLevel | NO | `0.0f` (D9CCaps zero-init, never set) | warn | NO | dxmt9 has no n-patch path; acceptable zero. |
| Reserved5 | NO | `0` (zero-init) | OK | NO | reserved field — D3D9 ignores. |
| MasterAdapterOrdinal | NO | `0` (struct default, never overridden) | OK | NO | acceptable on single-adapter Macs. |
| AdapterOrdinalInGroup | NO | `0` (struct default) | OK | NO | acceptable. |
| NumberOfAdaptersInGroup | default | `1` (struct in-class init) | OK | NO | acceptable. |
| DeclTypes | OK | `0x3ff` (all D3DDTCAPS_* bits) | OK | NO | |
| NumSimultaneousRTs | OK | `min(kMaxRenderTargets, limits.maxColorAttachments)` | OK | OK | tested. |
| StretchRectFilterCaps | OK | `0x03000300` (POINT | LINEAR src+dst) | OK | NO | |
| VS20Caps.Caps | OK (`vs20Caps`) | `0x00000001` (PREDICATION) | OK | NO | |
| VS20Caps.DynamicFlowControlDepth | default | `24` | OK | NO | |
| VS20Caps.NumTemps | default | `32` | OK | NO | |
| VS20Caps.StaticFlowControlDepth | default | `4` | OK | NO | |
| PS20Caps.Caps | OK (`ps20Caps`) | `0x0000001f` | OK | OK | tested in `d3d9_conformance_device.c` via `caps.PS20Caps`. |
| PS20Caps.DynamicFlowControlDepth | default | `24` | OK | NO | |
| PS20Caps.NumTemps | default | `32` | OK | NO | |
| PS20Caps.StaticFlowControlDepth | default | `4` | OK | NO | |
| PS20Caps.NumInstructionSlots | default | `512` | OK | NO | |
| VertexTextureFilterCaps | OK | `0x01000100` (POINT | MIN/MAG POINT) | OK | NO | |
| MaxVShaderInstructionsExecuted | OK | `65535` | OK | NO | |
| MaxPShaderInstructionsExecuted | OK | `65535` | OK | NO | |
| MaxVertexShader30InstructionSlots | OK | `512` | OK | NO | |
| MaxPixelShader30InstructionSlots | OK | `512` | OK | OK | tested via `caps.MaxPixelShader30InstructionSlots`. |
| (extra) `linePatternCaps` | NO | `0` | OK | NO | DX8 leftover field on `core::DeviceCaps`; not exposed in either `D9CCaps` or `D3DCAPS9` (D3D9 dropped LinePatternCaps). Dead field in the struct. |

### C.8 D3DPRESENT_PARAMETERS

Anchor: `include/dxmt9/core_constants.hpp:751-765` (core), `include/dxmt9/device_c.h:57-72` (`D9CPresentParams`),
`src/d3d9/core_present.cpp:10-140` (validate / normalize), `src/d3d9/d3d9_pe_factory.cpp:73-98, 117-134`
(D3D <-> D9C), `src/d3d9/d3d9_pe_device.cpp:1995-2010, 4197-4214` (Reset / ResetEx round-trip).

| Field | defined in `core::PresentParameters` | defined in `D9CPresentParams` | runtime-use | test | Notes |
|---|---|---|---|---|---|
| BackBufferWidth | OK | OK | OK | OK | validated >= 1 in windowed mode; defaults to adapter mode in fullscreen. |
| BackBufferHeight | OK | OK | OK | OK | as above. |
| BackBufferFormat | OK | OK | OK | OK | Unknown defaults to adapter display format (`core_present.cpp:18`). |
| BackBufferCount | OK | OK | OK | OK | clamped 1..3 (Ex: 1..30), COPY restricts to 1. |
| MultiSampleType | OK | OK | OK | OK | `msTypeFromD3D` accepts 0/2/4/8 (`device_c_format_utils.cpp:147-156`). |
| MultiSampleQuality | OK | OK (`multiSampleQuality`) | OK | OK | Copied by `ppFromC`; validation and resource descriptors preserve the selected quality index. Metal sample-count mapping does not expose vendor-specific quality modes. |
| SwapEffect | OK | OK | OK | OK | validated 1..3 (non-Ex), 1..5 (Ex). `core::PresentParameters::swapEffect` is raw u32. |
| hDeviceWindow | OK (`deviceWindow` Handle) | OK (`deviceWindow` u64) | OK | OK | |
| Windowed | OK | OK | OK | OK | `DXMT_FORCE_WINDOWED` env override (`core_present.cpp:12`). |
| EnableAutoDepthStencil | OK | OK | OK | OK | |
| AutoDepthStencilFormat | OK | OK | OK | OK | defaults to D24S8. |
| Flags | OK | OK (`flags`) | partial | warn | Copied by `ppFromC` and reported by the swap chain; individual `D3DPRESENTFLAG_*` backend effects still require per-flag audit. |
| FullScreen_RefreshRateInHz | OK | OK (`fullScreenRefreshRateHz`) | partial | warn | Copied and reported, but not used to constrain host present cadence. |
| PresentationInterval | OK (parsed as `PresentInterval` + raw u32) | OK | OK | OK | `core_present.cpp:55-67` validates the raw value; only `Immediate / Default / Two` are honored as semantic, but DEFAULT / ONE / TWO / THREE / FOUR / IMMEDIATE all validate. |

### C.9 D3DDISPLAYMODE / D3DDISPLAYMODEEX

Anchor: `include/dxmt9/core_constants.hpp:809-835`, `src/d3d9/d3d9_pe_factory.cpp:137-145`,
`src/d3d9/core_factory.cpp:271-285`, `src/d3d9/core_format.cpp:462-482` (`makeAdapterModes`).

| Field | defined | mapped | runtime-use | test | Notes |
|---|---|---|---|---|---|
| `D3DDISPLAYMODE.Width` | OK | OK | OK | OK | enumerated from `kCommonModes` (`core_format.cpp:469-472`). |
| `D3DDISPLAYMODE.Height` | OK | OK | OK | OK | as above. |
| `D3DDISPLAYMODE.RefreshRate` | OK | OK | warn | OK | hard-coded to `60` in `makeAdapterModes` and `adapter.displayMode.refreshRate`. No platform query. |
| `D3DDISPLAYMODE.Format` | OK | OK | OK | OK | only `X8R8G8B8` and `R5G6B5` exposed (`isSupportedAdapterModeFormat`); `exposeAdapterDisplayFormat` rewrites A8R8G8B8 -> X8R8G8B8 to mimic Windows. |
| `D3DDISPLAYMODEEX.Size` | n/a (fixed by D3D struct) | OK | OK | OK | written as `sizeof(D3DDISPLAYMODEEX)` in `d3d9_pe_factory.cpp:202`. |
| `D3DDISPLAYMODEEX.Width/Height/RefreshRate/Format` | OK | OK | OK | OK | shared path with `D3DDISPLAYMODE`. |
| `D3DDISPLAYMODEEX.ScanLineOrdering` | OK | OK | OK | OK | always `D3DSCANLINEORDERING_PROGRESSIVE` (`d3d9_pe_factory.cpp:207`, `core.cpp:335`). `core_device_com_spec.cpp:100, 295` asserts. |

### C.10 D3DADAPTER_IDENTIFIER9

Anchor: `include/dxmt9/core_constants.hpp:854-864`, `include/dxmt9/device_c.h:80-88` (D9CAdapterIdentifier),
`src/d3d9/core_factory.cpp:251-269` (core fill), `src/d3d9/device_c_factory.cpp:79-99` (D9C fill),
`src/d3d9/d3d9_pe_factory.cpp:382-410` (D3D fill).

| Field | defined | mapped | runtime-use | test | Notes |
|---|---|---|---|---|---|
| Driver | OK | OK | OK | warn | env override `DXMT_ADAPTER_DRIVER`, default `"nvd3dum.dll"`. |
| Description | OK | OK | OK | warn | env override `DXMT_ADAPTER_NAME`, default `"NVIDIA GeForce 6800"`. |
| DeviceName | OK | OK | OK | warn | hard-coded `"\\.\DISPLAY1"`. |
| DriverVersion | OK | OK | OK | warn | derived from `adapter.registryId` (default `1`); a `LARGE_INTEGER` in D3D, written as `LONGLONG`. |
| VendorId | OK | OK | OK | OK | env override `DXMT_ADAPTER_VENDOR_ID`, default `0x10de` (NVIDIA). |
| DeviceId | OK | OK | OK | OK | env override `DXMT_ADAPTER_DEVICE_ID`, default `0x0041` (GeForce 6800 PCI id). |
| SubSysId | OK | OK | OK | warn | hard-coded `0`. |
| Revision | OK | OK | OK | warn | hard-coded `0`. |
| DeviceIdentifier | OK | OK | OK | OK | Core derives a stable per-adapter 16-byte identifier from vendor, device, and description; D9C and PE copy it into the public GUID. |
| WHQLLevel | OK | OK | OK | OK | Explicitly returns `0`; Apple Silicon adapters are not WHQL-certified and D3D9 permits zero. |

### C.11 D3DRASTER_STATUS

Anchor: `src/d3d9/d3d9_pe_device.cpp:10686`, no core-side model; `tests/conformance/d3d9/d3d9_conformance_device.c:3147-3175`,
`tests/conformance/d3d9/d3d9_device_misc.cpp:174-175`.

| Field | defined | mapped | runtime-use | test | Notes |
|---|---|---|---|---|---|
| InVBlank | synthetic | PE estimate | PE-only | OK | `GetRasterStatus` synthesizes a monotonically advancing scanline/vblank estimate because dxmt9 has no real per-line Metal signal. Conformance accepts either `D3D_OK` or `E_FAIL` (`d3d9_device_misc.cpp:175`). |
| ScanLine | synthetic | PE estimate | PE-only | OK | `computeRasterStatusEstimate()` advances a synthetic scanline/vblank cycle; it is not queried from the WindowServer. `D3DCAPS_READ_SCANLINE` remains clear. |

(The conformance test accepts either success or failure for the host-dependent status, and the swapchain >0 path returns `D3DERR_INVALIDCALL` per `d3d9_conformance_device.c:3175`.)

### C.12 Section summary

| Category | Total rows | defined (OK) | runtime-use (OK) | tested (OK or warn) |
|---|---|---|---|---|
| C.1 Standard color/alpha (UNKNOWN..A16B16G16R16) | 17 | 13 | 13 | 13 |
| C.2 Palette / Luminance / Bumpmap / etc. | 12 | 6 | 6 | 6 |
| C.3 Depth/stencil | 11 | 9 | 7 | 7 |
| C.4 FOURCC standard (video + DXT) | 11 | 5 | 5 | 7 |
| C.5 Index + Float | 8 | 8 | 8 | 7 |
| C.6 FOURCC vendor pseudo | 13 | 2 | 2 (+1 partial GET4) | 5 |
| **D3DFORMAT subtotal** | **72** | **43** | **41** | **45** |
| C.7 D3DCAPS9 fields | 75 | 65 (53 explicit + 12 acceptable defaults) | 74 | 21 |
| C.8 D3DPRESENT_PARAMETERS | 14 | 11 (3 PE-shadow-only) | 11 | 11 |
| C.9 D3DDISPLAYMODE/EX | 7 | 7 | 7 | 7 |
| C.10 D3DADAPTER_IDENTIFIER9 | 10 | 8 | 8 | 10 |
| C.11 D3DRASTER_STATUS | 2 | 0 | 0 | 2 |
| **Caps + present + display + adapter + raster subtotal** | **108** | **91** | **100** | **51** |
| **Grand total** | **180** | **134** | **141** | **96** |

Top defects worth flagging:

1. `MultiSampleQuality`, `Flags`, and `FullScreen_RefreshRateInHz` are now represented and copied into `core::PresentParameters`; remaining work is per-flag backend semantics and host refresh-cadence enforcement.
2. `AlphaCmpCaps` now uses the dedicated `alphaCmpCaps` field in both C and PE caps fills.
3. `D3DADAPTER_IDENTIFIER9::DeviceIdentifier` now carries a stable derived GUID; `WHQLLevel` is explicitly zero for a non-WHQL Apple adapter.
4. `D3DRASTER_STATUS` has no real scanline source. The current PE path synthesizes a monotonically advancing scanline/vblank estimate so polling applications do not spin forever; `IDirect3D9::GetAdapterMonitor` still returns a stub monitor identity.
5. Vendor pseudo-formats have been reclassified since the original audit:
   `INTZ`/`DF16`/`DF24` are depth-as-texture formats, `RESZ` is a write-only
   MSAA-depth resolve command, `NULL` is a colorless render target, `ATOC` is
   wired as alpha-to-coverage, and `RAWZ`/`NVDB` are explicit unsupported
   classifications rather than silent `Unknown` fallthroughs.
6. `R8G8B8` (24-bit, code 20) is declared but `FormatClass::Unsupported` with no Metal mapping — Wine's CheckDeviceFormat tests sometimes expect Windows to NOTAVAILABLE this, which matches dxmt9.
7. `D32_LOCKABLE` (code 84) and `Q16W16V16U16` (code 110) are not represented at all (no `Format::` enum entry, no `fmtFromD3D` case). They were added in DX9b and are rarely used; mention them here for completeness.
8. `D9CCaps::adapterOrdinal` is zero-init in `fillCCaps` (never explicitly assigned from `adapterIndex`), but the value `0` happens to be correct for the single-adapter case dxmt9 supports today.
9. `D9CPresentParams.multiSampleQuality`, `flags`, and `fullScreenRefreshRateHz` are copied by `ppFromC`; capability-specific backend behavior remains separately audited.

## Methodology

Search commands used during this audit (all run under `/Users/dididi/workspaces/dxmt9/`):

```sh
# Core format table + mapping
rg -n "Format::|formatTable|findFormatInfo|makeDefaultCaps" src/d3d9/core_format.cpp include/dxmt9/core_constants.hpp

# D3DFMT_* code <-> core Format
rg -n "case [0-9]+: return F::|case F::" src/d3d9/device_c_format_utils.cpp

# Metal pixel format mapping
rg -n "toPixelFormat|WMTPixelFormat" src/dxmt9/dxmt9_format_convert.cpp

# Caps wiring (core -> D9C -> D3D9)
rg -n "fillCCaps|fillD3DCaps9|D9CCaps" src/d3d9 include/dxmt9

# Adapter identifier / display mode / raster status
rg -n "AdapterIdentifier|GetRasterStatus|isSupportedAdapterModeFormat|isValidCheckDeviceAdapterFormat|exposeAdapterDisplayFormat" src/d3d9

# Present parameters validation / normalization
rg -n "validatePresentParameters|normalizePresentParameters|applyFullscreenMode|D9CPresentParams" src/d3d9 include/dxmt9

# Vendor pseudo FOURCC
rg -n "INTZ|DF16|DF24|RAWZ|RESZ|NULL\b|ATOC|NVDB|GET4|GET1|R2VB|MAKEFOURCC|MULTI2_ARGB8|UYVY|YUY2|R8G8_B8G8|G8R8_G8B8" src/d3d9 src/dxmt9 include/dxmt9

# Test coverage (counts per Format)
rg -no "Format::[A-Z][A-Za-z0-9_]*" tests/native tests/conformance | awk -F: '{print $NF}' | sort | uniq -c | sort -rn

rg -no "D3DFMT_[A-Z0-9_]+" tests/conformance/d3d9 | awk -F: '{print $NF}' | sort | uniq -c | sort -rn

# Caps fields tested
rg -on "caps\.[A-Za-z][A-Za-z0-9_]*" tests/conformance/d3d9 | awk -F: '{print $NF}' | sort -u
```

Reference D3D9 SDK headers consulted (read-only):
- `~/workspaces/wine/include/d3d9types.h` — `D3DFORMAT`, `D3DPRESENT_PARAMETERS`, `D3DDISPLAYMODE`, `D3DDISPLAYMODEEX`, `D3DRASTER_STATUS`.
- `~/workspaces/wine/include/d3d9caps.h` — `D3DCAPS9` (~80 fields including the `D3DVSHADERCAPS2_0` / `D3DPSHADERCAPS2_0` sub-structs).

No source files were modified during this audit.
## D. COM Interface Methods

Generated 2026-05-23. Audit of every method on every D3D9 / D3D9Ex COM
interface implemented by dxmt9.

**Status legend** (per cell):
- **PE-side**: `✅` full / `⚠️` partial / `🟡` stub (returns S_OK without side effect) / `❌` `E_NOTIMPL` / `🚫` unimplemented (missing vtbl slot — none observed; every Wine vtbl slot is filled).
- **unix-side**: `✅` full backend wired to a `dxmt9c_*` C ABI entrypoint / `⚠️` partial / `🟡` stub / `❌` none (PE-only).
- **conformance test**: `✅` covered by at least one `tests/conformance/d3d9/*.c` function / `⚠️` partial / `❌` none.
- **native test**: `✅` covered by at least one `tests/native/core/*.cpp` spec / `⚠️` partial / `❌` none.

Source anchors are absolute paths in this repo. Source files referenced:
- `/Users/dididi/workspaces/dxmt9/src/d3d9/d3d9_pe_factory.cpp` (factory)
- `/Users/dididi/workspaces/dxmt9/src/d3d9/d3d9_pe_device.cpp` (device)
- `/Users/dididi/workspaces/dxmt9/src/d3d9/d3d9_pe_device_child_surface.cpp` (surface/texture/cube/volumetex/volume)
- `/Users/dididi/workspaces/dxmt9/src/d3d9/d3d9_pe_device_child_buffer.cpp` (VB/IB)
- `/Users/dididi/workspaces/dxmt9/src/d3d9/d3d9_pe_device_child_shader.cpp` (VS/PS)
- `/Users/dididi/workspaces/dxmt9/src/d3d9/d3d9_pe_device_child_misc.cpp` (VertexDecl, Query, StateBlock, SwapChain9Ex)

### D.1 IUnknown (3 methods)

| Method | PE-side | unix-side | conformance | native | Source | Notes |
|---|---|---|---|---|---|---|
| QueryInterface | ✅ | ❌ | ✅ | ✅ | `d3d9_pe_factory.cpp:355`, `d3d9_pe_device.cpp:1763`, `d3d9_pe_device_child_surface.cpp:433/796/1060/1270/1381` | tested via `test_factory_base_vs_ex_qi`, `test_ex_created_normal_device_qi`, `test_base_texture_metadata_iface_policy`, `test_cube_texture_face_desc_parity`, `core_device_com_spec.cpp` |
| AddRef | ✅ | ❌ | ✅ | ✅ | `d3d9_pe_factory.cpp:347`, `d3d9_pe_device.cpp:1759`, `d3d9_pe_device_child_surface.cpp:426/789/1053/1263/1374`, `d3d9_pe_device_child_buffer.cpp:131/282`, `d3d9_pe_device_child_misc.cpp:75/148/334/458`, `d3d9_pe_device_child_shader.cpp:32/93` | Reference counting verified by `test_private_data_iunknown_ownership_smoke`, `test_resource_get_device_wrapper_policy` |
| Release | ✅ | ❌ | ✅ | ✅ | same files | matches AddRef sites; pair-tested in resource lifecycle suite |

### D.2 IDirect3D9 (14 methods)

| Method | PE-side | unix-side | conformance | native | Source | Notes |
|---|---|---|---|---|---|---|
| RegisterSoftwareDevice | ❌ | ❌ | ❌ | ❌ | `d3d9_pe_factory.cpp:373` | returns `D3DERR_INVALIDCALL` (unsupported); Wine pattern |
| GetAdapterCount | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_factory.cpp:377` | `dxmt9c_factory_get_adapter_count`; covered by `test_multi_adapter` |
| GetAdapterIdentifier | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_factory.cpp:382` | `test_multi_adapter`, `test_device_parent_caps_getter_policy` |
| GetAdapterModeCount | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_factory.cpp:412` | `test_factory_caps_edge_matrix`, `test_ex_adapter_mode_enum_bounds` |
| EnumAdapterModes | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_factory.cpp:422` | `test_ex_adapter_mode_enum_bounds` |
| GetAdapterDisplayMode | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_factory.cpp:442` | `test_device_display_mode_adapter_format` |
| CheckDeviceType | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_factory.cpp:459` | `test_factory_validation_return_codes` |
| CheckDeviceFormat | ⚠️ | ✅ | ✅ | ⚠️ | `d3d9_pe_factory.cpp:489` | T3 (2026-05-08, `9980d5c` lineage): rejects VB/IB rtype with INVALIDCALL, downgrades AUTOGENMIPMAP success to `D3DOK_NOAUTOGEN`; `test_factory_caps_edge_matrix` |
| CheckDeviceMultiSampleType | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_factory.cpp:560` | `test_invalid_multisample_render_target_quality` |
| CheckDepthStencilMatch | ⚠️ | ✅ | ✅ | ⚠️ | `d3d9_pe_factory.cpp:595` | T3: real bit-depth compatibility via `dxmt9FormatPair_isDepthStencilCompatible` (not unconditional S_OK) |
| CheckDeviceFormatConversion | ⚠️ | ✅ | ✅ | ⚠️ | `d3d9_pe_factory.cpp:615` | T3: real pair check (identity + A8R8G8B8↔X8R8G8B8); `test_check_device_format_conversion_matrix` |
| GetDeviceCaps | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_factory.cpp:630` | `test_limits` |
| GetAdapterMonitor | ⚠️ | ✅ | ❌ | ❌ | `d3d9_pe_factory.cpp:661` | `dxmt9c_factory_get_adapter_monitor` returns a stub HMONITOR |
| CreateDevice | ✅ | ✅ | ✅ | ✅ | `d3d9_pe_factory.cpp:667` | `test_present_parameter_validation`, `core_device_lifecycle_spec` |

### D.3 IDirect3D9Ex (5 additional methods)

| Method | PE-side | unix-side | conformance | native | Source | Notes |
|---|---|---|---|---|---|---|
| GetAdapterModeCountEx | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_factory.cpp:712` | `test_ex_adapter_mode_enum_bounds` |
| EnumAdapterModesEx | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_factory.cpp:726` | `test_ex_adapter_mode_enum_bounds` |
| GetAdapterDisplayModeEx | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_factory.cpp:743` | `test_ex_get_adapter_display_mode_ex_policy`, `test_ex_adapter_display_mode_null_rotation` |
| CreateDeviceEx | ✅ | ✅ | ✅ | ✅ | `d3d9_pe_factory.cpp:767` | `test_ex_create_reset_mode_validation`, `test_ex_created_normal_device_qi` |
| GetAdapterLUID | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_factory.cpp:834` | `test_ex_adapter_luid_display_mode`, `test_ex_get_adapter_luid_policy` |

### D.4 IDirect3DDevice9 (119 methods)

| Method | PE-side | unix-side | conformance | native | Source | Notes |
|---|---|---|---|---|---|---|
| QueryInterface | ✅ | ❌ | ✅ | ✅ | `d3d9_pe_device.cpp:1763` | `test_ex_created_normal_device_qi` |
| AddRef | ✅ | ❌ | ✅ | ✅ | `d3d9_pe_device.cpp:1759` | shared lifecycle |
| Release | ✅ | ❌ | ✅ | ✅ | `d3d9_pe_device.cpp:1760` | shared lifecycle |
| TestCooperativeLevel | ✅ | ✅ | ✅ | ⚠️ | `d3d9_pe_device.cpp:1787` | `dxmt9c_device_test_cooperative_level`; T2 device-lost gate (2026-05-08); `test_reset_lockable_backbuffer_policy` |
| GetAvailableTextureMem | ⚠️ | ❌ | ✅ | ❌ | `d3d9_pe_device.cpp:1797` | PE-side pseudo-budget shadow (2 GiB sentinel, strictly-decreasing); `test_base_vidmem_accounting_policy`, `test_ex_vidmem_accounting_policy` |
| EvictManagedResources | 🟡 | ❌ | ✅ | ❌ | `d3d9_pe_device.cpp:1811` | returns S_OK; `test_visual_evict_managed_resources_policy` |
| GetDirect3D | ✅ | ❌ | ✅ | ❌ | `d3d9_pe_device.cpp:1813` | factory AddRef; `test_device_parent_caps_getter_policy` |
| GetDeviceCaps | ✅ | ✅ | ✅ | ⚠️ | `d3d9_pe_device.cpp:1821` | `dxmt9c_device_get_caps`; `test_limits`, `core_format_caps_spec` |
| GetDisplayMode | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:1848` | via swapchain; `test_device_display_mode_adapter_format` |
| GetCreationParameters | ✅ | ❌ | ✅ | ❌ | `d3d9_pe_device.cpp:1871` | PE shadow; `test_device_creation_parameters_policy` |
| SetCursorProperties | 🟡 | ❌ | ❌ | ❌ | `d3d9_pe_device.cpp:10332` | validates surface (A8R8G8B8 + POT); cursor behavior remains shadow-only and `CursorCaps` is clear |
| SetCursorPosition | 🟡 | ❌ | ❌ | ❌ | `d3d9_pe_device.cpp:10354` | logs only; no WindowServer integration |
| ShowCursor | 🟡 | ❌ | ❌ | ❌ | `d3d9_pe_device.cpp:10360` | gated on `cursorSurfaceSet_`; toggles shadow only and `CursorCaps` is clear |
| CreateAdditionalSwapChain | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:1920` | `test_additional_swapchain_backbuffer_bounds` |
| GetSwapChain | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:1962` | `dxmt9c_device_get_swap_chain`; `test_swapchain_backbuffer_getter_policy` |
| GetNumberOfSwapChains | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:1987` | `test_additional_swapchain_backbuffer_bounds` |
| Reset | ⚠️ | ✅ | ✅ | ⚠️ | `d3d9_pe_device.cpp:1991` | T2 (2026-05-08): viewport/scissor reset, releaseAllBound, deviceNotReset gate; `test_reset_lockable_backbuffer_policy`, `test_reset_fullscreen_focus_window_policy`, `test_swapchain_multisample_reset`, `d3d9_reset_lost.cpp` |
| Present | ✅ | ✅ | ✅ | ⚠️ | `d3d9_pe_device.cpp:2051` | T2 device-lost gate; chunk barrier flush + `D9C_COMMAND_RECORD_PRESENT`; many visual tests |
| GetBackBuffer | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:2095` | bounds-check matches Wine; `test_swapchain_backbuffer_getter_policy` |
| GetRasterStatus | ⚠️ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:4725` | `test_device_raster_status_bounds` |
| SetDialogBoxMode | 🟡 | ❌ | ❌ | ❌ | `d3d9_pe_device.cpp:2156` | logs only, returns S_OK |
| SetGammaRamp | ✅ | ✅ | ✅ | ✅ | `d3d9_pe_device.cpp:10728` | updates PE shadow state and forwards the ramp through D9C for presenter application; `core_d3d9_gamma_ramp_spec` |
| GetGammaRamp | ✅ | ✅ | ✅ | ✅ | `d3d9_pe_device.cpp:4778` | PE gamma shadow readback plus unix presenter application; `core_d3d9_gamma_ramp_spec` |
| CreateTexture | ✅ | ✅ | ✅ | ✅ | `d3d9_pe_device.cpp:2170` | T4 (2026-05-08): D3D9Ex SYSTEMMEM 1-mip alias; `test_ex_user_memory_lock_identity`, `core_d3d9_miptree_layout_spec` |
| CreateVolumeTexture | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:2216` | `test_volume_mipmap_level_desc_policy` |
| CreateCubeTexture | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:2237` | `test_cube_texture_face_desc_parity` |
| CreateVertexBuffer | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:2258` | `test_vertex_buffer_alignment`, `test_vertex_buffer_desc_binding_policy`, `test_vb_lock_flags` |
| CreateIndexBuffer | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:2280` | `test_index_buffer_desc_binding_policy` |
| CreateRenderTarget | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:2303` | `test_create_rt_ds_failure_policy`, `test_invalid_multisample_render_target_quality` |
| CreateDepthStencilSurface | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:2357` | `test_create_rt_ds_failure_policy`, `test_visual_depth_stencil_init_policy` |
| UpdateSurface | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:2391` | `test_visual_update_surface_policy`, `test_mipmap_surface_update_lock_policy` |
| UpdateTexture | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:2473` | `test_update_texture_pool_copy_2d`; P8/A8P8 shadow copy in `dxmt9-core-device-com-spec` |
| GetRenderTargetData | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:2504` | `test_get_render_target_data_policy` |
| GetFrontBufferData | ✅ | ✅ | ⚠️ | ❌ | `d3d9_pe_device.cpp` | delegates to the selected swap chain and synchronously copies its present-source image into SYSTEMMEM; multisampled sources resolve first. WindowServer-composited desktop capture remains out of scope; Wine visual re-run still needed. |
| StretchRect | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:2543` | `test_visual_blit_format_conversion_policy` |
| ColorFill | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:2573` | `test_visual_colorfill_format_policy` |
| CreateOffscreenPlainSurface | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:2606` | T4 shared-handle SYSTEMMEM 1-mip alias; `test_visual_offscreen_surface_creation_policy`, `test_ex_user_memory_*` |
| SetRenderTarget | ✅ | ✅ | ✅ | ✅ | `d3d9_pe_device.cpp:2651` | `core_device_com_spec` validates index bound + mismatch |
| GetRenderTarget | ✅ | ✅ | ✅ | ✅ | `d3d9_pe_device.cpp:2682` | shared with `core_device_com_spec` |
| SetDepthStencilSurface | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:2714` | `test_visual_depth_stencil_init_policy` |
| GetDepthStencilSurface | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:2753` | |
| BeginScene | ✅ | ✅ | ✅ | ⚠️ | `d3d9_pe_device.cpp:2777` | T2 device-lost gate; `test_scene_invalid_transitions` |
| EndScene | ✅ | ✅ | ✅ | ⚠️ | `d3d9_pe_device.cpp:2785` | T2 device-lost gate; `test_scene_invalid_transitions` |
| Clear | ✅ | ✅ | ✅ | ⚠️ | `d3d9_pe_device.cpp:2797` | T2 device-lost gate; `test_visual_clear_*`, `test_visual_depth_buffer_clear_policy` |
| SetTransform | ✅ | ✅ | ✅ | ✅ | `d3d9_pe_device.cpp:2848` | T1: stateblock record path; `test_stateblock_transform_capture_apply`, `core_d3d9_multiply_transform_spec`, `state_draw_transform_spec` |
| GetTransform | ✅ | ✅ | ✅ | ✅ | `d3d9_pe_device.cpp:2920` | `test_stateblock_transform_capture_apply` |
| MultiplyTransform | ✅ | ✅ | ✅ | ✅ | `d3d9_pe_device.cpp:2933` | T1: tracks under Begin/End; `test_stateblock_multiply_transform_capture`, `core_d3d9_multiply_transform_spec` |
| SetViewport | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:2966` | `test_viewport_scissor_state_getters` |
| GetViewport | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:2983` | `test_viewport_scissor_state_getters` |
| SetMaterial | ✅ | ✅ | ❌ | ❌ | `d3d9_pe_device.cpp:3019` | exercised via visual lighting tests indirectly |
| GetMaterial | ✅ | ✅ | ❌ | ❌ | `d3d9_pe_device.cpp:3029` | |
| SetLight | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:3038` | `test_light_enable_state`, `test_visual_lighting_render_state_policy` |
| GetLight | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:3075` | `test_light_enable_state` |
| LightEnable | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:3102` | `test_light_enable_state` |
| GetLightEnable | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:3127` | `test_light_enable_state` |
| SetClipPlane | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:3146` | `test_clip_plane_state_getters` |
| GetClipPlane | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:3159` | `test_clip_plane_state_getters` |
| SetRenderState | ✅ | ✅ | ✅ | ✅ | `d3d9_pe_device.cpp:3181` | `dxmt9c_device_set_render_state`; `test_visual_*_render_state_policy`, `core_state.cpp` |
| GetRenderState | ✅ | ✅ | ✅ | ✅ | `d3d9_pe_device.cpp:3213` | |
| CreateStateBlock | ✅ | ✅ | ✅ | ✅ | `d3d9_pe_device.cpp:3226` | `test_stateblock_invalid_type_recording_invalid_calls`, `core_stateblock_restore_spec` |
| BeginStateBlock | ✅ | ✅ | ✅ | ✅ | `d3d9_pe_device.cpp:3242` | T1 + C5 (2026-05-10 `a4252db`); `core_device_com_spec` |
| EndStateBlock | ✅ | ✅ | ✅ | ✅ | `d3d9_pe_device.cpp:3260` | T1; `core_device_com_spec` |
| SetClipStatus | 🟡 | ❌ | ✅ | ✅ | `d3d9_pe_device.cpp:5897` | accepts non-null without storing; no hardware clip-status accumulation |
| GetClipStatus | 🟡 | ❌ | ✅ | ✅ | `d3d9_pe_device.cpp:5907` | returns defined all-visible default |
| GetTexture | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:3557` | `test_get_set_texture` |
| SetTexture | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:3532` | `test_get_set_texture` |
| GetTextureStageState | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:3364` | `test_texture_stage_states` |
| SetTextureStageState | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:3335` | `test_texture_stage_states`, `test_visual_bumpenvmap_tss_policy` |
| GetSamplerState | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:3408` | `test_sampler_state_edges` |
| SetSamplerState | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:3380` | `test_sampler_state_edges` |
| ValidateDevice | 🟡 | ❌ | ❌ | ❌ | `d3d9_pe_device.cpp:6260` | writes `*pPasses=1`, returns S_OK without real validation |
| SetPaletteEntries | ⚠️ | ✅ | ✅ | ✅ | `d3d9_pe_device.cpp:5623` | PE shadow plus active-palette upload to bound palettized textures; `test_set_palette_roundtrip`, `test_palette_*_policy`, `test_visual_p8_texture_sampler_policy`, `dxmt9-core-device-com-spec` |
| GetPaletteEntries | ⚠️ | ❌ | ✅ | ❌ | `d3d9_pe_device.cpp:4000` | mirrors shadow |
| SetCurrentTexturePalette | ⚠️ | ✅ | ✅ | ✅ | `d3d9_pe_device.cpp:5660` | PE shadow plus bound palettized texture re-expansion; `test_palette_current_entry_isolation`, `test_visual_p8_texture_sampler_policy`, `dxmt9-core-device-com-spec` |
| GetCurrentTexturePalette | ✅ | ❌ | ✅ | ❌ | `d3d9_pe_device.cpp:4019` | |
| SetScissorRect | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:2996` | `test_viewport_scissor_state_getters` |
| GetScissorRect | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:3009` | |
| SetSoftwareVertexProcessing | ⚠️ | ❌ | ✅ | ❌ | `d3d9_pe_device.cpp:6922` | mutable PE shadow plus mixed-VP runtime SetSoftwareVertexProcessing(TRUE) FFP and programmable-VS render-target readbacks and limited lighting-disabled fixed-function FVF XYZ/XYZ+PSIZE, FVF XYZB4 unindexed, XYZB5 LASTBETA_UBYTE4 indexed, and stream-0 declaration BLENDWEIGHT/BLENDINDICES vertex-blend DrawPrimitiveUP/DrawIndexedPrimitiveUP plus bound DrawPrimitive/DrawIndexedPrimitive, and split stream0 POSITION/COLOR + stream1 BLENDWEIGHT/BLENDINDICES bound DrawPrimitive/DrawIndexedPrimitive, lighting-enabled FVF XYZ+NORMAL, simple programmable VS FVF, stream-0 POSITION/COLOR declaration UP plus SHORT4N POSITION/COLOR declaration UP, stream-0 POSITION/COLOR/TANGENT declaration fixed-function DrawPrimitiveUP plus programmable DrawPrimitiveUP and bound draw, split stream0 POSITION/COLOR + stream2 TANGENT bound draw/indexed draw, split POSITION/COLOR declaration-stream bound draw plus stream1 INSTANCEDATA color draw and stream0 INDEXEDDATA two-instance, non-1 INSTANCEDATA divider, and fixed-function/programmable line-strip and strip/fan expansion DrawPrimitive/DrawIndexedPrimitive expansion, nonzero-minVertex/BaseVertex indexed CPU transform paths, fixed-function UP non-indexed/indexed (including INDEX16/INDEX32 simple indexed readbacks) and bound non-indexed/indexed (including INDEX16/INDEX32 simple indexed readbacks) plus programmable VS UP non-indexed/indexed (including INDEX16/INDEX32 simple indexed readbacks) and bound non-indexed/indexed (including INDEX16/INDEX32 simple indexed readbacks) render-target readbacks, and bound ps_2_0 constant-color pixel-shader readbacks for FFP and programmable-VS SWVP DrawPrimitiveUP plus bound DrawPrimitive/DrawIndexedPrimitive, with stream0 restoration pinned after SWVP DrawPrimitiveUP and bidirectional FFP/programmable SWVP state-transition readbacks; `test_visual_mvp_software_vp_policy` |
| GetSoftwareVertexProcessing | ✅ | ❌ | ✅ | ❌ | `d3d9_pe_device.cpp:6927` | |
| SetNPatchMode | 🟡 | ❌ | ❌ | ❌ | `d3d9_pe_device.cpp:6931` | returns S_OK; N-patch unsupported |
| GetNPatchMode | 🟡 | ❌ | ❌ | ❌ | `d3d9_pe_device.cpp:6937` | returns 0.0 |
| DrawPrimitive | ✅ | ✅ | ✅ | ✅ | `d3d9_pe_device.cpp:7427` | T2 device-lost gate; many visual tests + `state_draw_transform_spec`, `core_d3d9_multiply_transform_spec` |
| DrawIndexedPrimitive | ✅ | ✅ | ✅ | ✅ | `d3d9_pe_device.cpp:7467` | `test_visual_max_index16_draw_policy`, `test_null_stream_shader_draw_policy` |
| DrawPrimitiveUP | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:7521` | many visual tests |
| DrawIndexedPrimitiveUP | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:7559` | many visual tests |
| ProcessVertices | ⚠️ | ✅ | ✅ | PE singleton | `d3d9_pe_device.cpp:7606` | device-lost gate plus fixed-function WORLD/VIEW/PROJECTION XYZ→XYZRHW CPU transform with matching diffuse/specular/texcoord/PSIZE passthrough for FVF and simple source/destination declarations, directional/point (including range/attenuation)/spot fixed-function lighting with cone rejection/falloff plus normalized declaration NORMAL decode into destination diffuse, `D3DRS_COLORVERTEX` `D3DMCS_COLOR1` diffuse, `D3DMCS_COLOR2` specular, `D3DMCS_COLOR1` ambient, and `D3DMCS_COLOR2` emissive material-source lighting, and spot specular into destination specular, supported source attributes split across bound streams, sparse TEXCOORD1 and FLOAT1 TEXCOORD7 declaration slots, and FLOAT3/raw integer/normalized NORMAL/TANGENT/BINORMAL plus BLENDWEIGHT/BLENDINDICES inputs for simple programmable shaders plus FVF NORMAL/SPECULAR/XYZB input plus fixed-function D3DVBF_3WEIGHTS D3DFVF_XYZB4 unindexed and D3DFVF_XYZB5 LASTBETA_UBYTE4 indexed vertex-blend readbacks and FVF TEX2 mixed TEXCOORDSIZE1/TEXCOORDSIZE3 fixed-function and programmable ProcessVertices readback and raw integer/normalized/half/D3DCOLOR/UDEC3/DEC3N TEXCOORD decode; `SrcStart`/`DestIndex` placement, `D3DPV_DONOTCOPYDATA` flag acceptance, and `D3DRS_CLIPPING=FALSE` depth clamp are covered; limited programmable VS path executes DCL/DEFI/DEFB/DEF/MOV/basic arithmetic/comparison/DP/MAD/LRP/NRM/RCP/RSQ/FRC/ABS/DP2ADD/POW/CRS/SGN/SINCOS/EXP/LOG/LIT/DST/EXPP/LOGP/M3x2/M3x3/M3x4/M4x3/M4x4 plus 2D TEXLDL vertex texture sampling with sampler ADDRESSU/V WRAP/CLAMP/MIRROR/MIRRORONCE/BORDER, BORDERCOLOR, MAXMIPLEVEL/SetLOD mip-clamp handling, and P8/A8P8 palette-expanded backing over PE vertex shader float/integer/bool constants, accepts `_PARTIALPRECISION` destination modifiers and `_SATURATE` output clamping, covers programmable PSIZE output, and covers D3DSPSM source modifiers and simple IF/IFC/ELSE/ENDIF over bool constants, DEFB/CONSTBOOL branch selection through SetVertexShaderConstantB, REP/LOOP counts with LOOP `aL` initial/step source reads, MOVA per-component address-register source reads, source relative addressing through `a0`/`aL` including INPUT-register relative reads and matrix constant-base reads plus address-register component swizzles, temp and `D3DSPR_OUTPUT` destination relative addressing, BREAK/BREAKC, and CALL/CALLNZ/LABEL/RET and SETP/BREAKP predicate plus predicated ordinary/flow-control instruction guards while preserving source FLOAT4 POSITION / XYZW w and decoding SHORT4N POSITION declarations on fixed-function and programmable ProcessVertices paths plus fixed-function and programmable SWVP DrawPrimitiveUP readbacks; unsupported shader/broader declaration variants return `D3DERR_INVALIDCALL`; `test_visual_process_vertices_xyzhw_policy` covers vs_1_1 implicit input and RASTOUT/ATTROUT/TEXCRDOUT output readback, M4x4/MOV, fixed-function directional/point (including range/attenuation)/spot lighting with cone rejection/falloff plus normalized declaration NORMAL decode, `D3DRS_COLORVERTEX` `D3DMCS_COLOR1` diffuse, `D3DMCS_COLOR2` specular, `D3DMCS_COLOR1` ambient, and `D3DMCS_COLOR2` emissive material-source lighting, plus spot specular, destination PSIZE FVF/declaration readbacks, fixed-function D3DVBF_3WEIGHTS D3DFVF_XYZB4 unindexed and D3DFVF_XYZB5 LASTBETA_UBYTE4 indexed vertex-blend readbacks, FVF TEX2 mixed TEXCOORDSIZE1/TEXCOORDSIZE3 readbacks, programmable COLOR1/specular source and destination readback, MAD/MOV, NRM/DP3/DP4/MUL/ADD vector math, SLT/SGE/MIN/MAX/LRP/CND/CMP compare-select math, RCP/RSQ/FRC/ABS/DP2ADD/POW/CRS/SGN/SINCOS/EXP/LOG/LIT/DST/EXPP/LOGP scalar-cross/transcendent math and M3x2 matrix math, 2D TEXLDL vertex texture sampler readback with ADDRESSU/V WRAP/CLAMP/MIRROR/MIRRORONCE/BORDER, BORDERCOLOR, MAXMIPLEVEL, and P8/A8P8 palette-expanded vertex texture sampling including same-slot and current-index P8/A8P8 bound palette updates plus P8 current-palette-before-bind vertex sampler readback, `_PARTIALPRECISION`, `_SATURATE` output clamping, D3DSPSM source modifiers, DEFI/CONSTINT REP/LOOP counts, DEFB/CONSTBOOL IF branch selection through SetVertexShaderConstantB, LOOP `aL` initial/step source reads, MOVA per-component address-register source reads, source relative addressing through `a0`/`aL` including INPUT-register relative reads and matrix constant-base reads plus address-register component swizzles, temp and `D3DSPR_OUTPUT` destination relative addressing, sparse TEXCOORD1 and FLOAT1 TEXCOORD7 declaration readbacks, `SHORT2`/`SHORT2N`/`USHORT2N`/`FLOAT16_2` and D3DCOLOR TEXCOORD decode readbacks plus `SHORT4`/`SHORT4N`/`USHORT4N`/`FLOAT16_4`/`UBYTE4`/`UBYTE4N`/`UDEC3`/`DEC3N` TEXCOORD FLOAT4 destination readbacks, `SHORT4`/`UBYTE4`/`UBYTE4N`/`UDEC3`/`DEC3N` NORMAL decode readbacks, BREAK/BREAKC, CALL/CALLNZ/LABEL/RET, SETP/BREAKP predicate plus predicated ordinary/flow-control instruction guards, and simple IF/IFC/ELSE/ENDIF flow-control, NORMAL/TANGENT/BINORMAL/BLENDWEIGHT/BLENDINDICES input arithmetic, including D3DCOLOR BLENDINDICES and normalized UBYTE4N BLENDWEIGHT declaration readbacks, FVF NORMAL/SPECULAR/XYZB input arithmetic, FLOAT4 POSITION MOV, flag/offset handling, and extra source-attribute paths |
| CreateVertexDeclaration | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:3629` | `test_vertex_declaration_fvf_policy`, `test_unused_declaration_type` |
| SetVertexDeclaration | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:3661` | `test_fvf_decl_management`, `test_vdecl_apply` |
| GetVertexDeclaration | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:3682` | `test_fvf_decl_management` |
| SetFVF | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:3607` | `test_fvf_decl_management`, `test_vertex_declaration_fvf_policy` |
| GetFVF | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:3624` | |
| CreateVertexShader | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:3690` | `test_get_set_vertex_shader`, `test_unsupported_shaders`, `test_shader_unsupported_stage_variants`, `test_ex_shader_validation_policy` |
| SetVertexShader | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:3709` | `test_get_set_vertex_shader` |
| GetVertexShader | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:3720` | `test_get_set_vertex_shader` |
| SetVertexShaderConstantF | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:3765` | `test_vertex_shader_constant`, `test_shader_constant_apply`, `test_shader_constant_stateblock_cross_stage` |
| GetVertexShaderConstantF | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:3776` | |
| SetVertexShaderConstantI | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:3782` | |
| GetVertexShaderConstantI | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:3791` | |
| SetVertexShaderConstantB | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:3797` | |
| GetVertexShaderConstantB | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:3806` | |
| SetStreamSource | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:3814` | `test_set_stream_source_state`, `test_stream_source_vb_offset_alignment_policy`, `test_stream_source_null_layout_policy`, `test_stream_source_zero_stride_policy` |
| GetStreamSource | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:3845` | `test_set_stream_source_state` |
| SetStreamSourceFreq | ✅ | ✅ | ✅ | ✅ | `d3d9_pe_device.cpp`; `core_state.cpp`; `dxmt9_draw_encoder.mm` | All 16 frequencies reach canonical draw state and state blocks; stream-0 INDEXEDDATA sets Metal instance count and INSTANCEDATA streams use `instance_id / divider`. Native gates: core_device_lifecycle, state_draw_transform, core_stateblock_restore, shader_transform, encode_draw_recorder. |
| GetStreamSourceFreq | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:3887` | `test_stream_source_frequency_state` |
| SetIndices | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:3898` | |
| GetIndices | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:3905` | |
| CreatePixelShader | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:3911` | `test_get_set_pixel_shader`, `test_unsupported_shaders`, `test_visual_vface_pixel_shader_create_policy` |
| SetPixelShader | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:3929` | `test_get_set_pixel_shader` |
| GetPixelShader | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:3936` | `test_get_set_pixel_shader` |
| SetPixelShaderConstantF | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:3940` | `test_pixel_shader_constant`, `test_shader_constant_apply` |
| GetPixelShaderConstantF | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:3949` | |
| SetPixelShaderConstantI | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:3955` | |
| GetPixelShaderConstantI | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:3964` | |
| SetPixelShaderConstantB | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:3970` | |
| GetPixelShaderConstantB | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:3979` | |
| DrawRectPatch | ❌ | ❌ | ❌ | ❌ | `d3d9_pe_device.cpp:8580` | returns `D3DERR_INVALIDCALL` (patch tessellation unsupported) |
| DrawTriPatch | ❌ | ❌ | ❌ | ❌ | `d3d9_pe_device.cpp:8581` | returns `D3DERR_INVALIDCALL` |
| DeletePatch | 🟡 | ❌ | ❌ | ❌ | `d3d9_pe_device.cpp:8582` | returns S_OK |
| CreateQuery | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:8588` | `test_query_get_data_size_policy`, `d3d9_queries.cpp` |

### D.5 IDirect3DDevice9Ex (15 additional methods)

| Method | PE-side | unix-side | conformance | native | Source | Notes |
|---|---|---|---|---|---|---|
| SetConvolutionMonoKernel | ❌ | ❌ | ❌ | ❌ | `d3d9_pe_device.cpp:8612` | `return E_NOTIMPL` |
| ComposeRects | ❌ | ❌ | ❌ | ❌ | `d3d9_pe_device.cpp:8613` | `return E_NOTIMPL` |
| PresentEx | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:8339` | T2 device-lost gate; `test_visual_swapchain_flip_present_policy` indirect |
| GetGPUThreadPriority | 🟡 | ❌ | ❌ | ❌ | `d3d9_pe_device.cpp:8357` | writes `*p=0` + S_OK |
| SetGPUThreadPriority | 🟡 | ❌ | ❌ | ❌ | `d3d9_pe_device.cpp:8361` | S_OK |
| WaitForVBlank | ✅ | ✅ | ❌ | ❌ | `d3d9_pe_device.cpp:8366` | `dxmt9c_device_wait_for_vblank` |
| CheckResourceResidency | 🟡 | ❌ | ❌ | ❌ | `d3d9_pe_device.cpp:8370` | S_OK |
| SetMaximumFrameLatency | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:8377` | range [1..30] validated; `test_ex_frame_latency_state` |
| GetMaximumFrameLatency | ✅ | ❌ | ✅ | ❌ | `d3d9_pe_device.cpp:8385` | PE shadow read; `test_ex_frame_latency_state` |
| CheckDeviceState | ✅ | ✅ | ❌ | ❌ | `d3d9_pe_device.cpp:8392` | `dxmt9c_device_check_device_state` |
| CreateRenderTargetEx | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:8397` | delegates to CreateRenderTarget; `test_create_rt_ds_failure_policy` |
| CreateOffscreenPlainSurfaceEx | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:8409` | T4 shared-handle SYSTEMMEM 1-mip alias; `test_ex_user_memory_*` |
| CreateDepthStencilSurfaceEx | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:8420` | `test_create_depth_stencil_surface_ex` |
| ResetEx | ⚠️ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:8434` | T2 (2026-05-08): viewport/scissor reset, `deviceNotReset_=false`; `test_ex_create_reset_mode_validation` |
| GetDisplayModeEx | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device.cpp:8504` | `test_display_mode_ex_size_filter_smoke` |

### D.6 IDirect3DResource9 (base — 8 methods per derived class)

Every IDirect3DResource9-derived implementation (surface, texture variants, buffers, volume) implements the 8 inherited slots independently. Status applies per slot family.

| Method | PE-side | unix-side | conformance | native | Source | Notes |
|---|---|---|---|---|---|---|
| GetDevice | ✅ | ❌ | ✅ | ✅ | surface `:447`; texture `:811`; cube `:1075`; volume `:1283`; voltex `:1396`; VB `:152`; IB `:303`; vdecl `:95`; query `:168`; sb `:361`; sc `:592`; vs `:52`; ps `:113` | `test_resource_get_device_wrapper_policy` |
| SetPrivateData | ✅ | ❌ | ✅ | ❌ | surface `:462`; texture `:826`; cube `:1090`; volume `:1294`; voltex `:1411`; VB `:163`; IB `:314` | `test_private_data_*` (4 suites) |
| GetPrivateData | ✅ | ❌ | ✅ | ❌ | same files | `test_private_data_resource_wrappers` |
| FreePrivateData | ✅ | ❌ | ✅ | ❌ | same files | `test_private_data_replace_and_size_policy` |
| SetPriority | ✅ | ❌ | ✅ | ❌ | surface `:475`; texture `:839`; cube `:1102`; voltex `:1424`; VB `:175`; IB `:326` | `test_resource_priority_pool_policy`, `test_resource_priority_roundtrip` |
| GetPriority | ✅ | ❌ | ✅ | ❌ | same files | |
| PreLoad | 🟡 | ❌ | ❌ | ❌ | surface `:488`; texture `:848`; cube `:1111`; voltex `:1433`; VB `:184`; IB `:335` | no-op |
| GetType | ✅ | ❌ | ✅ | ❌ | surface `:489`; texture `:849`; cube `:1112`; volume `:1283`; voltex `:1434`; VB `:185`; IB `:336` | `test_resource_type` |

### D.7 IDirect3DBaseTexture9 (6 additional methods — apply per texture variant)

| Method | PE-side | unix-side | conformance | native | Source | Notes |
|---|---|---|---|---|---|---|
| SetLOD | ✅ | ✅ | ✅ | ❌ | texture `:852`; cube `:1115`; voltex `:1437` | `test_texture_lod_policy` |
| GetLOD | ✅ | ✅ | ✅ | ❌ | texture `:855`; cube `:1118`; voltex `:1440` | `test_texture_lod_policy` |
| GetLevelCount | ✅ | ✅ | ✅ | ❌ | texture `:858`; cube `:1121`; voltex `:1443` | `test_texture_auto_mipmap_level_count` |
| SetAutoGenFilterType | ✅ | ✅ | ✅ | ❌ | texture `:861`; cube `:1124`; voltex `:1446` | `test_texture_autogen_filter_level_policy` |
| GetAutoGenFilterType | ✅ | ✅ | ✅ | ❌ | texture `:865`; cube `:1128`; voltex `:1450` | `test_texture_autogen_filter_level_policy` |
| GenerateMipSubLevels | 🟡 | ✅ | ✅ | ❌ | texture `:917`; cube `:1204`; voltex `:1538` | non-AUTOGEN no-op; AUTOGEN forwards to backend mip generation |

### D.8 IDirect3DTexture9 (5 additional methods)

| Method | PE-side | unix-side | conformance | native | Source | Notes |
|---|---|---|---|---|---|---|
| GetLevelDesc | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device_child_surface.cpp:875` | `test_texture_level_surface_desc_parity` |
| GetSurfaceLevel | ✅ | ✅ | ✅ | ✅ | `d3d9_pe_device_child_surface.cpp:895` | `test_texture_level_surface_unlock_policy`, `core_device_com_spec` |
| LockRect | ✅ | ✅ | ✅ | ✅ | `d3d9_pe_device_child_surface.cpp:914` | `test_texture_reentrant_lock_preserves_output`, `test_compressed_surface_lockrect_block_offset`, `core_device_com_spec` |
| UnlockRect | ✅ | ✅ | ✅ | ✅ | `d3d9_pe_device_child_surface.cpp:969` | `test_texture_level_surface_unlock_policy`, `core_device_com_spec` |
| AddDirtyRect | ⚠️ | ✅ | ✅ | ❌ | `d3d9_pe_device_child_surface.cpp:989` | `test_visual_add_dirty_rect_policy` |

### D.9 IDirect3DCubeTexture9 (5 additional methods)

| Method | PE-side | unix-side | conformance | native | Source | Notes |
|---|---|---|---|---|---|---|
| GetLevelDesc | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device_child_surface.cpp:1138` | `test_cube_texture_face_desc_parity` |
| GetCubeMapSurface | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device_child_surface.cpp:1159` | `test_cube_texture_level_surface_policy`, `test_cube_texture_face_desc_parity` |
| LockRect | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device_child_surface.cpp:1187` | shared `test_compressed_surface_lockrect_block_offset` |
| UnlockRect | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device_child_surface.cpp:1210` | |
| AddDirtyRect | ⚠️ | ✅ | ✅ | ❌ | `d3d9_pe_device_child_surface.cpp:1220` | covered by `test_visual_add_dirty_rect_policy` |

### D.10 IDirect3DVolumeTexture9 (5 additional methods)

| Method | PE-side | unix-side | conformance | native | Source | Notes |
|---|---|---|---|---|---|---|
| GetLevelDesc | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device_child_surface.cpp:1460` | `test_volume_mipmap_level_desc_policy` |
| GetVolumeLevel | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device_child_surface.cpp:1478` | `test_volume_resource_container_desc`, `test_volume_container_interface_policy` |
| LockBox | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device_child_surface.cpp:1489` | `test_volume_lockbox_bounds_offset_policy`, `test_volume_block_lock_layout` (recent commit `9076984` block alignment) |
| UnlockBox | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device_child_surface.cpp:1515` | |
| AddDirtyBox | 🟡 | ❌ | ❌ | ❌ | `d3d9_pe_device_child_surface.cpp:1613` | returns S_OK; upload is driven by unlock/recorder paths |

### D.11 IDirect3DVertexBuffer9 (3 additional methods)

| Method | PE-side | unix-side | conformance | native | Source | Notes |
|---|---|---|---|---|---|---|
| Lock | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device_child_buffer.cpp:188` | `test_vb_lock_flags`, `test_writeonly_vertex_buffer_readback_policy`, `test_pinned_buffers_d3dusage_policy` |
| Unlock | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device_child_buffer.cpp:218` | |
| GetDesc | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device_child_buffer.cpp:229` | `test_vertex_buffer_desc_binding_policy` |

### D.12 IDirect3DIndexBuffer9 (3 additional methods)

| Method | PE-side | unix-side | conformance | native | Source | Notes |
|---|---|---|---|---|---|---|
| Lock | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device_child_buffer.cpp:339` | shared with VB suite |
| Unlock | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device_child_buffer.cpp:346` | |
| GetDesc | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device_child_buffer.cpp:353` | `test_index_buffer_desc_binding_policy` |

### D.13 IDirect3DSurface9 (6 additional methods)

| Method | PE-side | unix-side | conformance | native | Source | Notes |
|---|---|---|---|---|---|---|
| GetContainer | ✅ | ❌ | ✅ | ❌ | `d3d9_pe_device_child_surface.cpp:492` | `test_texture_surface_container_policy`, `test_volume_container_interface_policy` |
| GetDesc | ✅ | ✅ | ✅ | ✅ | `d3d9_pe_device_child_surface.cpp:502` | `test_surface_dimensions`, `test_surface_format_null_policy`, `core_device_com_spec` |
| LockRect | ✅ | ✅ | ✅ | ✅ | `d3d9_pe_device_child_surface.cpp:531` | T4 SYSTEMMEM aliasing; `test_surface_lockrect_subrect_offset_policy`, `test_surface_reentrant_lock_preserves_output`, `test_resource_lock_error_policy`, `core_device_com_spec` |
| UnlockRect | ✅ | ✅ | ✅ | ✅ | `d3d9_pe_device_child_surface.cpp:604` | `test_surface_double_unlock_pool_policy`, `core_device_com_spec` |
| GetDC | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device_child_surface.cpp:641` | recent commit `5329219` (D3DKMTCreateDCFromMemory for user-memory DIB identity); `test_ex_user_memory_getdc_dib_identity`, `test_ex_user_memory_getdc_format_policy`, `test_nonlockable_backbuffer_getdc_policy` |
| ReleaseDC | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device_child_surface.cpp:717` | shared GetDC suite |

### D.14 IDirect3DVolume9 (4 additional methods)

| Method | PE-side | unix-side | conformance | native | Source | Notes |
|---|---|---|---|---|---|---|
| GetContainer | ✅ | ❌ | ✅ | ❌ | `d3d9_pe_device_child_surface.cpp:1307` | `test_volume_container_interface_policy` |
| GetDesc | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device_child_surface.cpp:1317` | `test_volume_resource_container_desc` |
| LockBox | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device_child_surface.cpp:1333` | delegates to parent volume-texture; covered by `test_volume_block_lock_layout` |
| UnlockBox | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device_child_surface.cpp:1337` | |

### D.15 IDirect3DSwapChain9 (7 additional methods, on `D3D9SwapChainImpl` which also is the Ex object)

| Method | PE-side | unix-side | conformance | native | Source | Notes |
|---|---|---|---|---|---|---|
| Present | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device_child_misc.cpp:487` | recorder-flush then `dxmt9c_swapchain_present` |
| GetFrontBufferData | ✅ | ✅ | ⚠️ | ❌ | `d3d9_pe_device_child_misc.cpp` | validates SYSTEMMEM A8R8G8B8 destination, resolves MSAA when needed, then performs synchronous GetRenderTargetData from the swap-chain present source; composited-desktop capture remains a host-integration gap. |
| GetBackBuffer | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device_child_misc.cpp:531` | identity-caches surfaces per idx; `test_swapchain_backbuffer_getter_policy`, `test_additional_swapchain_backbuffer_bounds` |
| GetRasterStatus | ⚠️ | ✅ | ❌ | ❌ | `d3d9_pe_device_child_misc.cpp:568` | synthetic scanline/vblank estimate + S_OK |
| GetDisplayMode | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device_child_misc.cpp:570` | derives from PresentParameters; `test_ex_swapchain_display_mode` |
| GetDevice | ✅ | ❌ | ✅ | ❌ | `d3d9_pe_device_child_misc.cpp:592` | |
| GetPresentParameters | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device_child_misc.cpp:603` | flags-shadow merged; `test_present_parameter_normalization`, `test_backbuffer_resize_present_parameter_policy` |

### D.16 IDirect3DSwapChain9Ex (3 additional methods)

| Method | PE-side | unix-side | conformance | native | Source | Notes |
|---|---|---|---|---|---|---|
| GetLastPresentCount | 🟡 | ❌ | ❌ | ❌ | `d3d9_pe_device_child_misc.cpp:646` | writes 0, returns S_OK |
| GetPresentStats | 🟡 | ❌ | ❌ | ❌ | `d3d9_pe_device_child_misc.cpp:654` | memset 0, returns S_OK |
| GetDisplayModeEx | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device_child_misc.cpp:643` | `test_ex_swapchain_display_mode`, `test_ex_swapchain_display_mode_null_rotation`, `test_swapchain_get_display_mode_ex_policy` |

### D.17 IDirect3DQuery9 (5 methods, all beyond IUnknown)

| Method | PE-side | unix-side | conformance | native | Source | Notes |
|---|---|---|---|---|---|---|
| GetDevice | ✅ | ❌ | ✅ | ❌ | `d3d9_pe_device_child_misc.cpp:168` | |
| GetType | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device_child_misc.cpp:179` | `test_query_get_data_size_policy` |
| GetDataSize | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device_child_misc.cpp:182` | `test_query_get_data_size_policy` |
| Issue | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device_child_misc.cpp:185` | exercised under occlusion suite (`d3d9_queries.cpp`) |
| GetData | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device_child_misc.cpp:204` | shared with `d3d9_queries.cpp` |

### D.18 IDirect3DStateBlock9 (3 methods beyond IUnknown)

| Method | PE-side | unix-side | conformance | native | Source | Notes |
|---|---|---|---|---|---|---|
| GetDevice | ✅ | ❌ | ✅ | ❌ | `d3d9_pe_device_child_misc.cpp:361` | |
| Capture | ✅ | ✅ | ✅ | ✅ | `d3d9_pe_device_child_misc.cpp:372` | T1 (2026-05-08) per-stage TSS slice; `test_stateblock_transform_capture_apply`, `test_state_management_*`, `core_stateblock_restore_spec`, `core_device_com_spec` |
| Apply | ✅ | ✅ | ✅ | ✅ | `d3d9_pe_device_child_misc.cpp:391` | C5 (2026-05-10 `a4252db`): `markPendingDirtyAll()` invalidation; `test_state_management_*`, `test_shader_constant_apply`, `core_stateblock_restore_spec` |

### D.19 IDirect3DVertexDeclaration9 (2 methods beyond IUnknown)

| Method | PE-side | unix-side | conformance | native | Source | Notes |
|---|---|---|---|---|---|---|
| GetDevice | ✅ | ❌ | ✅ | ❌ | `d3d9_pe_device_child_misc.cpp:95` | |
| GetDeclaration | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device_child_misc.cpp:106` | `test_vertex_declaration_fvf_policy`, `test_unused_declaration_type` |

### D.20 IDirect3DVertexShader9 (2 methods beyond IUnknown)

| Method | PE-side | unix-side | conformance | native | Source | Notes |
|---|---|---|---|---|---|---|
| GetDevice | ✅ | ❌ | ✅ | ❌ | `d3d9_pe_device_child_shader.cpp:52` | |
| GetFunction | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device_child_shader.cpp:63` | `dxmt9c_shader_get_bytecode`; covered by `test_get_set_vertex_shader` |

### D.21 IDirect3DPixelShader9 (2 methods beyond IUnknown)

| Method | PE-side | unix-side | conformance | native | Source | Notes |
|---|---|---|---|---|---|---|
| GetDevice | ✅ | ❌ | ✅ | ❌ | `d3d9_pe_device_child_shader.cpp:113` | |
| GetFunction | ✅ | ✅ | ✅ | ❌ | `d3d9_pe_device_child_shader.cpp:124` | shared `dxmt9c_shader_get_bytecode`; covered by `test_get_set_pixel_shader` |

### D.22 Section Summary

Row counts per interface (counting each method on each derived class once for the inherited-base sections):

| Interface | Methods | ✅ full PE | ⚠️ partial PE | 🟡 stub PE | ❌ E_NOTIMPL / unimpl | conformance tested | native tested |
|---|---|---|---|---|---|---|---|
| D.1 IUnknown | 3 | 3 | 0 | 0 | 0 | 3 | 3 |
| D.2 IDirect3D9 | 14 | 10 | 3 | 1 | 1 | 13 | 1 |
| D.3 IDirect3D9Ex | 5 | 5 | 0 | 0 | 0 | 5 | 1 |
| D.4 IDirect3DDevice9 | 119 | 89 | 7 | 13 | 6 | 102 | 16 |
| D.5 IDirect3DDevice9Ex | 15 | 9 | 1 | 3 | 2 | 9 | 0 |
| D.6 IDirect3DResource9 (base, multiplied across 7 derived classes that store full resource interface — counts the 8 slots once per category) | 8 | 7 | 0 | 1 | 0 | 7 | 1 |
| D.7 IDirect3DBaseTexture9 | 6 | 5 | 0 | 1 | 0 | 5 | 0 |
| D.8 IDirect3DTexture9 | 5 | 4 | 1 | 0 | 0 | 5 | 3 |
| D.9 IDirect3DCubeTexture9 | 5 | 4 | 1 | 0 | 0 | 5 | 0 |
| D.10 IDirect3DVolumeTexture9 | 5 | 4 | 0 | 1 | 0 | 4 | 0 |
| D.11 IDirect3DVertexBuffer9 | 3 | 3 | 0 | 0 | 0 | 3 | 0 |
| D.12 IDirect3DIndexBuffer9 | 3 | 3 | 0 | 0 | 0 | 3 | 0 |
| D.13 IDirect3DSurface9 | 6 | 6 | 0 | 0 | 0 | 6 | 3 |
| D.14 IDirect3DVolume9 | 4 | 4 | 0 | 0 | 0 | 4 | 0 |
| D.15 IDirect3DSwapChain9 | 7 | 5 | 0 | 1 | 1 | 5 | 0 |
| D.16 IDirect3DSwapChain9Ex | 3 | 1 | 0 | 2 | 0 | 1 | 0 |
| D.17 IDirect3DQuery9 | 5 | 5 | 0 | 0 | 0 | 5 | 0 |
| D.18 IDirect3DStateBlock9 | 3 | 3 | 0 | 0 | 0 | 3 | 1 |
| D.19 IDirect3DVertexDeclaration9 | 2 | 2 | 0 | 0 | 0 | 2 | 0 |
| D.20 IDirect3DVertexShader9 | 2 | 2 | 0 | 0 | 0 | 2 | 0 |
| D.21 IDirect3DPixelShader9 | 2 | 2 | 0 | 0 | 0 | 2 | 0 |
| **TOTAL (unique slots)** | **225** | **176** | **13** | **23** | **10** | **194** | **29** |

Historical coverage snapshot:
- **78%** of the 225 enumerated vtbl slots were fully (`✅`) implemented PE-side at the original audit point.
- **6%** were partial (`⚠️`) implementations.
- **10%** were silent stubs (`🟡`) returning S_OK or a benign default without backend coupling.
- **4%** explicitly returned `E_NOTIMPL` / `D3DERR_INVALIDCALL`.
- The table is retained for audit history; use the 2026-07-18 current status summary and live-gap table above for triage.

Highest-leverage live gaps:

1. **`ProcessVertices` breadth** — the covered fixed-function and programmable subsets are extensive and include explicit `DP3` and `DP4` readback coverage; texture/sample opcodes beyond 2D TEXLDL plus remaining clipping, lighting, and exotic declaration edge cases remain deferred.
2. **D3D9Ex cross-process sharing and composition** — same-process DEFAULT-pool create/open-existing shares Metal backing, but real closable/cross-process Win32 handles are absent; `SetConvolutionMonoKernel` and `ComposeRects` return `E_NOTIMPL`; present statistics are default values.
3. **Legacy FFP and displacement semantics** — flat shade, last-pixel, cylindrical wrap, `D3DSAMP_ELEMENTINDEX`, and `D3DSAMP_DMAPOFFSET` lack backend behavior.
4. **N-patch / adaptive tessellation / patch draw path** — unsupported by design; declaration methods safe-reject and patch draws return invalid-call/no-op contracts.
5. **Front-buffer host composition** — `GetFrontBufferData` copies the dxmt9 swap-chain present source, not a WindowServer-composited desktop image including occluding windows.
6. **Legacy line AA** — `D3DRS_ANTIALIASEDLINEENABLE` is accepted no-op/deferred because Metal has no matching D3D9 line-raster toggle.
7. **Stub/default COM contracts** — `SetDialogBoxMode`, `ValidateDevice`, `PreLoad`, non-AUTOGEN `GenerateMipSubLevels`, `AddDirtyBox`, `Set/GetClipStatus`, `Set/GetNPatchMode`, `DeletePatch`, SwapChain Ex present stats, GPU thread priority, and resource residency are intentional Wine-compatible default/no-op behavior, not silent missing work.

## Methodology

Every method row was verified by ripgrep against the source and test trees. Representative commands:

```sh
# All vtbl method declarations on a single PE implementation
rg -n "STDMETHODCALLTYPE" src/d3d9/d3d9_pe_device.cpp
rg -n "STDMETHODCALLTYPE" src/d3d9/d3d9_pe_factory.cpp
rg -n "STDMETHODCALLTYPE" src/d3d9/d3d9_pe_device_child_surface.cpp
rg -n "STDMETHODCALLTYPE" src/d3d9/d3d9_pe_device_child_buffer.cpp
rg -n "STDMETHODCALLTYPE" src/d3d9/d3d9_pe_device_child_misc.cpp
rg -n "STDMETHODCALLTYPE" src/d3d9/d3d9_pe_device_child_shader.cpp

# Wine reference vtbl ground-truth (every interface)
grep -n "#define INTERFACE IDirect3D" ~/workspaces/wine/include/d3d9.h
grep -n "_lpVtbl->" ~/workspaces/wine/include/d3d9.h | sort -u

# unix-side ABI surface (C bridge)
grep -n "dxmt9c_device_\|dxmt9c_swapchain_\|dxmt9c_surface_\|dxmt9c_texture_\|dxmt9c_buffer_\|dxmt9c_query_\|dxmt9c_stateblock_\|dxmt9c_shader_\|dxmt9c_factory_" include/dxmt9/device_c.h

# Conformance test function names
rg -n "^void test_|^static void test_" tests/conformance/d3d9/*.c

# Native spec coverage
rg -n "dxmt9c_device_\|dxmt9c_state\|dxmt9c_texture_\|dxmt9c_surface_" tests/native/core/*.cpp

# Stub-vs-E_NOTIMPL classification
rg -n "return E_NOTIMPL|return D3DERR_INVALIDCALL.*noexcept override|return S_OK.*noexcept override" src/d3d9/d3d9_pe_*.cpp
```

Status classification rules used:
- `🟡` (silent stub) = method body is `return S_OK;` (or trivially writes a zero / default-out value then returns S_OK) with no recorder, no C ABI call, and no shadow side effect that downstream code reads.
- `⚠️` (partial) = method body has real validation / state shadow / Wine-parity checks but is missing a documented sub-case (e.g., palette/P8 covers texture/cube/volume index expansion while direct core P8/A8P8 storage remains explicitly unsupported).
- `❌` (explicit failure) = `return E_NOTIMPL` or `return D3DERR_INVALIDCALL` as the entire body, with the intent documented inline that the method is unsupported (vs. partial).
- `✅` (full) = method goes through to `dxmt9c_*` C ABI (or PE shadow that fully captures Wine semantics) and has at least one Wine-parity comment or test cross-reference.
