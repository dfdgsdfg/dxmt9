---
type: "Spec"
title: "Capability Spec (D3DCAPS9)"
description: "D3D9 / Caps spec, ownership, ordering, and evidence mapping."
tags: [specs, d3d9, caps, spec]
---

# Capability Spec (D3DCAPS9)

This document defines the concrete `D3DCAPS9` values dxmt9 reports. These tables
are implementation guidance for the normative rules in
`specs/d3d9/caps/requirements.md`.

## Device Type and Presentation

| Field | Value | Notes |
|---|---|---|
| `DeviceType` | D3DDEVTYPE_HAL | Always hardware |
| `AdapterOrdinal` | adapter index | 0 for primary |
| `Caps` | 0 | Scanline reads are not advertised |
| `Caps2` | FULLSCREENGAMMA \| CANAUTOGENMIPMAP \| DYNAMICTEXTURES (`0x60020000`) | |
| `Caps3` | ALPHA_FULLSCREEN_FLIP_OR_DISCARD \| COPY_TO_VIDMEM \| COPY_TO_SYSTEMMEM (`0x320`) | |
| `PresentationIntervals` | D3DPRESENT_INTERVAL_ONE \| D3DPRESENT_INTERVAL_IMMEDIATE (`0x80000001`) | |
| `CursorCaps` | 0 | Color/low-resolution cursor support is not advertised while device cursor methods are PE shadow-only |

## Rasterizer

| Field | Value | Notes |
|---|---|---|
| `RasterCaps` | ZTEST \| FOGRANGE \| FOGTABLE \| FOGVERTEX \| MIPMAPLODBIAS \| DEPTHBIAS \| SLOPESCALEDEPTHBIAS \| WFOG \| ZFOG \| ANISOTROPY \| COLORPERSPECTIVE \| SCISSORTEST (`0x07332190`) | DITHER is intentionally absent while `D3DRS_DITHERENABLE` is shadow-only; MULTISAMPLE_TOGGLE is also absent |
| `ZCmpCaps` | All 8 D3DPCMPCAPS_* | All compare functions |
| `AlphaCmpCaps` | All 8 D3DPCMPCAPS_* | Alpha test always emulated in shader |
| `ShadeCaps` | D3DPSHADECAPS_COLORGOURAUDRGB \| D3DPSHADECAPS_SPECULARGOURAUDRGB \| D3DPSHADECAPS_ALPHAGOURAUDBLEND \| D3DPSHADECAPS_FOGGOURAUD | |
| `TextureCaps` | ALPHA \| ALPHAPALETTE \| CUBEMAP \| CUBEMAP_POW2 \| MIPMAP \| MIPCUBEMAP \| MIPVOLUMEMAP \| PERSPECTIVE \| POW2 \| PROJECTED \| VOLUMEMAP \| VOLUMEMAP_POW2 | **Not** SQUAREONLY; non-square textures are supported |
| `LinePatternCaps` | 0 | Line patterns not supported |
| `MaxAnisotropy` | `limits.maxAnisotropy` | Backend-device limit |
| `MaxUserClipPlanes` | 8 | dxmt9 public cap; the core clip-plane storage uses the same limit |
| `MaxVertexW` | 1e10f | |
| `GuardBandLeft/Right/Top/Bottom` | +/-8192.0f | |
| `ExtentsAdjust` | 0.0f | |
| `StencilCaps` | KEEP \| ZERO \| REPLACE \| INCRSAT \| DECRSAT \| INVERT \| INCR \| DECR \| TWOSIDED | All stencil ops |

### Depth-bias mapping

`D3DRS_DEPTHBIAS` is a normalized depth offset, while Metal's constant
`depthBias` is expressed in units of the active depth format. The backend must
scale the D3D9 value from the actual Metal depth attachment format: `2^16` for
`Depth16Unorm`, `2^24` for `Depth24Unorm_Stencil8`, and `2^23` for
`Depth32Float` or `Depth32Float_Stencil8`. This includes the D24S8-to-D32FS8
fallback required by `R-CAPS-4`. `D3DRS_SLOPESCALEDEPTHBIAS` is passed through
without this conversion.

## Blending

| Field | Value |
|---|---|
| `SrcBlendCaps` | `0x00003fff` |
| `DestBlendCaps` | `0x000027ff` |
| `TextureBlendCaps` | `0x03feffff` |
| `TextureAddressCaps` | WRAP \| MIRROR \| CLAMP \| BORDER \| INDEPENDENTUV (`0x1f`) |
| `VolumeTextureAddressCaps` | Same as TextureAddressCaps |
| `LineCaps` | ALPHACMP \| BLEND \| FOG \| TEXTURE \| ZTEST |

## Shader Models

| Field | Value | Notes |
|---|---|---|
| `VertexShaderVersion` | D3DVS_VERSION(3,0) | vs_1_1 through vs_3_0 all supported |
| `PixelShaderVersion` | D3DPS_VERSION(3,0) | ps_1_1 through ps_3_0 all supported |
| `MaxVertexShaderConst` | 256 | c0-c255 |
| `PixelShader1xMaxValue` | 1024.0f | Value reported by `makeDefaultCaps()` |

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

## Textures

| Field | Value | Notes |
|---|---|---|
| `MaxTextureWidth` | 16384 | Query `[device maxTextureSize]`; min report 8192 |
| `MaxTextureHeight` | 16384 | |
| `MaxVolumeExtent` | 2048 | 3D texture max dimension |
| `MaxTextureRepeat` | 32768 | Max texture coordinate |
| `MaxTextureAspectRatio` | 16384 | |
| `MaxTextureLODBias` | 16.0f | |
| `MaxSimultaneousTextures` | 8 | Fixed-function texture blend stages; the 16 pixel-shader sampler slots are a separate limit |
| `MaxActiveLights` | 8 | Fixed-function lights |

## Render Targets

| Field | Value | Notes |
|---|---|---|
| `NumSimultaneousRTs` | 4 | MRT; Metal guarantees at least 4 color attachments |
| `MaxRenderTargetWidth/Height` | Same as MaxTextureWidth/Height | |

## Vertex Processing

| Field | Value | Notes |
|---|---|---|
| `MaxStreams` | `kMaxStreams` (16) | Stream sources 0-15 |
| `MaxStreamStride` | 1024 | Bytes per vertex element |
| `MaxPrimitiveCount` | 5592405 | `0x555555` |
| `MaxVertexIndex` | 16777215 | |
| `MaxVertexBlendMatrices` | 4 | D3DVBF_3WEIGHTS |
| `MaxVertexBlendMatrixIndex` | 0 | Indexed matrix-palette range is not advertised |
| `FVFCaps` | D3DFVFCAPS_PSIZE \| (8 << D3DFVFCAPS_TEXCOORDCOUNTMASK) | Point size + 8 tex coord sets |
| `VertexProcessingCaps` | TEXGEN \| MATERIALSOURCE7 \| DIRECTIONALLIGHTS \| POSITIONALLIGHTS \| LOCALVIEWER \| TEXGEN_SPHEREMAP (`0x13b`) | TWEENING is not advertised |

## Misc

| Field | Value | Notes |
|---|---|---|
| `DevCaps` | EXECUTESYSTEMMEMORY \| EXECUTEVIDEOMEMORY \| TLVERTEXSYSTEMMEMORY \| TLVERTEXVIDEOMEMORY \| TEXTURESYSTEMMEMORY \| TEXTUREVIDEOMEMORY \| DRAWPRIMTLVERTEX \| CANRENDERAFTERFLIP \| DRAWPRIMITIVES2 \| DRAWPRIMITIVES2EX \| HWRASTERIZATION \| HWTRANSFORMANDLIGHT \| PUREDEVICE (`0x0019aff0`) | Patch/N-patch capability bits are not advertised |
| `DevCaps2` | STREAMOFFSET \| CAN_STRETCHRECT_FROM_TEXTURES \| VERTEXELEMENTSCANSHARESTREAMOFFSET (`0x51`) | Displacement-map and adaptive-tessellation bits are not advertised |
| `MaxPointSize` | 64.0f | Also used as the `D3DRS_POINTSIZE_MAX` initial value |
| `MasterAdapterOrdinal` | 0 | |
| `AdapterOrdinalInGroup` | 0 | |
| `NumberOfAdaptersInGroup` | 1 | |
| `DeclTypes` | All D3DDTCAPS_* matching the vertex format table in `specs/d3d9/formats/spec.md` | |
