# Capability Spec (D3DCAPS9)

This document defines what dxmt9 reports when an application calls
`IDirect3D9::GetDeviceCaps()` or `IDirect3DDevice9::GetDeviceCaps()`.

The values here are the **contract**: `CheckDeviceFormat()`, `CheckDeviceType()`,
`CheckDeviceMultiSampleType()`, and `CheckDeviceFormatConversion()` must be consistent
with what is reported here. Applications depend on these values to decide what
features to use.

---

## 1. Device Type and Presentation

| Field | Value | Notes |
|---|---|---|
| `DeviceType` | D3DDEVTYPE_HAL | Always hardware |
| `AdapterOrdinal` | adapter index | 0 for primary |
| `Caps` | D3DCAPS_READ_SCANLINE | Minimal base |
| `Caps2` | D3DCAPS2_CANAUTOGENMIPMAP \| D3DCAPS2_DYNAMICTEXTURES | |
| `Caps3` | D3DCAPS3_ALPHA_FULLSCREEN_FLIP_OR_DISCARD \| D3DCAPS3_LINEAR_TO_SRGB_PRESENTATION | |
| `PresentationIntervals` | D3DPRESENT_INTERVAL_ONE \| D3DPRESENT_INTERVAL_IMMEDIATE \| D3DPRESENT_INTERVAL_TWO | |

---

## 2. Rasterizer

| Field | Value | Notes |
|---|---|---|
| `RasterCaps` | DITHER \| ZTEST \| FOGRANGE \| FOGTABLE \| FOGVERTEX \| MIPMAPLODBIAS \| ZBIAS \| DEPTHBIAS \| WFOG \| ZFOG \| ANISOTROPY \| COLORPERSPECTIVE | |
| `ZCmpCaps` | All 8 D3DPCMPCAPS_* | All compare functions |
| `AlphaCmpCaps` | All 8 D3DPCMPCAPS_* | Alpha test always emulated in shader |
| `ShadeCaps` | D3DPSHADECAPS_COLORGOURAUDRGB \| D3DPSHADECAPS_SPECULARGOURAUDRGB \| D3DPSHADECAPS_ALPHAGOURAUDBLEND \| D3DPSHADECAPS_FOGGOURAUD | |
| `TextureCaps` | ALPHA \| ALPHAPALETTE \| CUBEMAP \| CUBEMAP_POW2 \| MIPMAP \| MIPCUBEMAP \| MIPVOLUMEMAP \| PERSPECTIVE \| POW2 \| PROJECTED \| VOLUMEMAP \| VOLUMEMAP_POW2 | **Not** SQUAREONLY — non-square textures are supported |
| `LinePatternCaps` | 0 | Line patterns not supported |
| `MaxAnisotropy` | 16 | Or query `[device maxAnisotropy]` |
| `MaxUserClipPlanes` | 6 | Hardware clip distances |
| `MaxVertexW` | 1e10f | |
| `GuardBandLeft/Right/Top/Bottom` | ±8192.0f | |
| `ExtentsAdjust` | 0.0f | |
| `StencilCaps` | KEEP \| ZERO \| REPLACE \| INCRSAT \| DECRSAT \| INVERT \| INCR \| DECR \| TWOSIDED | All stencil ops |

---

## 3. Blending

| Field | Value |
|---|---|
| `SrcBlendCaps` | All D3DPBLENDCAPS_* (16 values) |
| `DestBlendCaps` | All D3DPBLENDCAPS_* (16 values) |
| `AlphaBlendCaps` | SRCALPHA \| INVSRCALPHA \| DESTALPHA \| INVDESTALPHA \| SRCALPHASAT \| BOTHSRCALPHA \| BOTHINVSRCALPHA \| BLENDFACTOR |
| `TextureBlendCaps` | All D3DTEXBLENDCAPS_* |
| `TextureAddressCaps` | WRAP \| MIRROR \| CLAMP \| BORDER \| INDEPENDENTUV \| MIRRORONCE |
| `VolumeTextureAddressCaps` | Same as TextureAddressCaps |
| `LineCaps` | ALPHACMP \| BLEND \| FOG \| TEXTURE \| ZTEST |

---

## 4. Shader Models

| Field | Value | Notes |
|---|---|---|
| `VertexShaderVersion` | D3DVS_VERSION(3,0) | vs_1_1 through vs_3_0 all supported |
| `PixelShaderVersion` | D3DPS_VERSION(3,0) | ps_1_1 through ps_3_0 all supported |
| `MaxVertexShaderConst` | 256 | c0–c255 |
| `PixelShader1xMaxValue` | 8.0f | Max value clamped in ps_1_x |

### D3DPSHADERCAPS2_0 (PS 2.0 extended caps)

| Sub-field | Value | Notes |
|---|---|---|
| `Caps` | D3DPS20CAPS_ARBITRARYSWIZZLE \| D3DPS20CAPS_GRADIENTINSTRUCTIONS \| D3DPS20CAPS_PREDICATION \| D3DPS20CAPS_NODEPENDENTREADLIMIT \| D3DPS20CAPS_NOTEXINSTRUCTIONLIMIT | Advertise full ps_2_a / ps_2_b |
| `DynamicFlowControlDepth` | 24 | |
| `NumTemps` | 32 | |
| `StaticFlowControlDepth` | 4 | |
| `NumInstructionSlots` | 512 | |

### D3DVSHADERCAPS2_0 (VS 2.0 extended caps)

| Sub-field | Value | Notes |
|---|---|---|
| `Caps` | D3DVS20CAPS_PREDICATION | |
| `DynamicFlowControlDepth` | 24 | |
| `NumTemps` | 32 | |
| `StaticFlowControlDepth` | 4 | |

---

## 5. Textures

| Field | Value | Notes |
|---|---|---|
| `MaxTextureWidth` | 16384 | Query `[device maxTextureSize]`; min report 8192 |
| `MaxTextureHeight` | 16384 | |
| `MaxVolumeExtent` | 2048 | 3D texture max dimension |
| `MaxTextureRepeat` | 8192 | Max texture coordinate |
| `MaxTextureAspectRatio` | 0 | 0 = no restriction |
| `MaxTextureLODBias` | 16.0f | |
| `MaxSimultaneousTextures` | 16 | Stages 0–15 |
| `MaxActiveLights` | 8 | Fixed-function lights |

---

## 6. Render Targets

| Field | Value | Notes |
|---|---|---|
| `NumSimultaneousRTs` | 4 | MRT; Metal guarantees ≥4 color attachments |
| `MaxRenderTargetWidth/Height` | Same as MaxTextureWidth/Height | |

---

## 7. Vertex Processing

| Field | Value | Notes |
|---|---|---|
| `MaxStreams` | 16 | Stream sources 0–15 |
| `MaxStreamStride` | 255 | Bytes per vertex element |
| `MaxPrimitiveCount` | 16777215 | 2^24 − 1 |
| `MaxVertexIndex` | 16777215 | |
| `MaxVertexBlendMatrices` | 4 | D3DVBF_3WEIGHTS |
| `MaxVertexBlendMatrixIndex` | 255 | Indexed vertex blend |
| `FVFCaps` | D3DFVFCAPS_PSIZE \| (8 << D3DFVFCAPS_TEXCOORDCOUNTMASK) | Point size + 8 tex coord sets |
| `VertexProcessingCaps` | TEXGEN \| MATERIALSOURCE7 \| DIRECTIONALLIGHTS \| POSITIONALLIGHTS \| LOCALVIEWER \| TWEENING | |

---

## 8. Misc

| Field | Value | Notes |
|---|---|---|
| `DevCaps` | EXECUTESYSTEMMEMORY \| TLVERTEXSYSTEMMEMORY \| TLVERTEXVIDEOMEMORY \| TEXTURESYSTEMMEMORY \| TEXTUREVIDEOMEMORY \| DRAWPRIMTLVERTEX \| CANRENDERAFTERFLIP \| HWRASTERIZATION \| HWTRANSFORMANDLIGHT \| PUREDEVICE \| QUINTICRTPATCHES \| RTPATCHES | |
| `DevCaps2` | D3DDEVCAPS2_STREAMOFFSET \| D3DDEVCAPS2_DMAPNPATCH \| D3DDEVCAPS2_ADAPTIVETESSRTPATCH \| D3DDEVCAPS2_ADAPTIVETESSNPATCH \| D3DDEVCAPS2_CAN_STRETCHRECT_FROM_TEXTURES \| D3DDEVCAPS2_PRESAMPLEDDMAPNPATCH \| D3DDEVCAPS2_VERTEXELEMENTSCANSHARESTREAMOFFSET | |
| `MaxPointSize` | 256.0f | |
| `MasterAdapterOrdinal` | 0 | |
| `AdapterOrdinalInGroup` | 0 | |
| `NumberOfAdaptersInGroup` | 1 | |
| `DeclTypes` | All D3DDTCAPS_* matching the vertex format table in formats.md | |

---

## 9. Rules for GetDeviceCaps Consistency

**R-CAPS-1** Every feature advertised in `D3DCAPS9` must be implementable by the
backend on the target Metal device. dxmt9 must not advertise capabilities it cannot
satisfy.

**R-CAPS-2** `CheckDeviceFormat(format, usage)` must return `D3D_OK` if and only if
the format is classified as Required or Optional (and confirmed available) in
`specs/core/formats.md` for the given usage.

**R-CAPS-3** `MaxTextureWidth` / `MaxTextureHeight` must be the lesser of 16384 and
the value returned by `[MTLDevice maxTextureSize]` at device init time.

**R-CAPS-4** If a Metal device does not support `MTLPixelFormatDepth24Unorm_Stencil8`
(query `[device isDepth24Stencil8PixelFormatSupported]`), the caps must still report
`D3DFMT_D24S8` as available (it is remapped to `Depth32Float_Stencil8`), but
`DepthBias` in render state must report accurate behavior for 32-bit float depth.

**R-CAPS-5** `NumSimultaneousRTs` must not exceed `[MTLDevice maxColorRenderTargets]`
if that value is less than 4 (not expected on any supported Mac).

**R-CAPS-6** `CheckDeviceFormatConversion()` must be consistent with the reported
format table, but identical source and destination formats are always accepted
with `D3D_OK` after adapter/device-type validation. Unsupported well-formed
conversions return `D3DERR_NOTAVAILABLE`; they must not be reported as supported
only because both individual formats are renderable or texturable.

**R-CAPS-7** `CheckDeviceMultiSampleType()` must combine caps consistency with
Windows D3D9-compatible front-end validation, using Wine D3D9 tests as the
behavioural oracle. `D3DMULTISAMPLE_NONE` reports exactly one quality level.
Invalid multisample enum values return `D3DERR_INVALIDCALL`; unsupported but
well-formed sample counts return `D3DERR_NOTAVAILABLE` and handle
`pQualityLevels` according to Wine-test-observed behaviour. Caps must not
advertise a sample count that the backend cannot allocate for the requested
format/windowed combination.
