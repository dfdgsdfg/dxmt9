# Capability Design (D3DCAPS9)

This document defines the concrete `D3DCAPS9` values dxmt9 reports. These tables
are implementation guidance for the normative rules in
`specs/d3d9/caps/requirements.md`.

## Device Type and Presentation

| Field | Value | Notes |
|---|---|---|
| `DeviceType` | D3DDEVTYPE_HAL | Always hardware |
| `AdapterOrdinal` | adapter index | 0 for primary |
| `Caps` | D3DCAPS_READ_SCANLINE | Minimal base |
| `Caps2` | D3DCAPS2_CANAUTOGENMIPMAP \| D3DCAPS2_DYNAMICTEXTURES | |
| `Caps3` | D3DCAPS3_ALPHA_FULLSCREEN_FLIP_OR_DISCARD \| D3DCAPS3_LINEAR_TO_SRGB_PRESENTATION | |
| `PresentationIntervals` | D3DPRESENT_INTERVAL_ONE \| D3DPRESENT_INTERVAL_IMMEDIATE \| D3DPRESENT_INTERVAL_TWO | |

## Rasterizer

| Field | Value | Notes |
|---|---|---|
| `RasterCaps` | DITHER \| ZTEST \| FOGRANGE \| FOGTABLE \| FOGVERTEX \| MIPMAPLODBIAS \| ZBIAS \| DEPTHBIAS \| WFOG \| ZFOG \| ANISOTROPY \| COLORPERSPECTIVE | |
| `ZCmpCaps` | All 8 D3DPCMPCAPS_* | All compare functions |
| `AlphaCmpCaps` | All 8 D3DPCMPCAPS_* | Alpha test always emulated in shader |
| `ShadeCaps` | D3DPSHADECAPS_COLORGOURAUDRGB \| D3DPSHADECAPS_SPECULARGOURAUDRGB \| D3DPSHADECAPS_ALPHAGOURAUDBLEND \| D3DPSHADECAPS_FOGGOURAUD | |
| `TextureCaps` | ALPHA \| ALPHAPALETTE \| CUBEMAP \| CUBEMAP_POW2 \| MIPMAP \| MIPCUBEMAP \| MIPVOLUMEMAP \| PERSPECTIVE \| POW2 \| PROJECTED \| VOLUMEMAP \| VOLUMEMAP_POW2 | **Not** SQUAREONLY; non-square textures are supported |
| `LinePatternCaps` | 0 | Line patterns not supported |
| `MaxAnisotropy` | 16 | Or query `[device maxAnisotropy]` |
| `MaxUserClipPlanes` | 6 | Hardware clip distances |
| `MaxVertexW` | 1e10f | |
| `GuardBandLeft/Right/Top/Bottom` | +/-8192.0f | |
| `ExtentsAdjust` | 0.0f | |
| `StencilCaps` | KEEP \| ZERO \| REPLACE \| INCRSAT \| DECRSAT \| INVERT \| INCR \| DECR \| TWOSIDED | All stencil ops |

## Blending

| Field | Value |
|---|---|
| `SrcBlendCaps` | All D3DPBLENDCAPS_* (16 values) |
| `DestBlendCaps` | All D3DPBLENDCAPS_* (16 values) |
| `AlphaBlendCaps` | SRCALPHA \| INVSRCALPHA \| DESTALPHA \| INVDESTALPHA \| SRCALPHASAT \| BOTHSRCALPHA \| BOTHINVSRCALPHA \| BLENDFACTOR |
| `TextureBlendCaps` | All D3DTEXBLENDCAPS_* |
| `TextureAddressCaps` | WRAP \| MIRROR \| CLAMP \| BORDER \| INDEPENDENTUV \| MIRRORONCE |
| `VolumeTextureAddressCaps` | Same as TextureAddressCaps |
| `LineCaps` | ALPHACMP \| BLEND \| FOG \| TEXTURE \| ZTEST |

## Shader Models

| Field | Value | Notes |
|---|---|---|
| `VertexShaderVersion` | D3DVS_VERSION(3,0) | vs_1_1 through vs_3_0 all supported |
| `PixelShaderVersion` | D3DPS_VERSION(3,0) | ps_1_1 through ps_3_0 all supported |
| `MaxVertexShaderConst` | 256 | c0-c255 |
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

## Textures

| Field | Value | Notes |
|---|---|---|
| `MaxTextureWidth` | 16384 | Query `[device maxTextureSize]`; min report 8192 |
| `MaxTextureHeight` | 16384 | |
| `MaxVolumeExtent` | 2048 | 3D texture max dimension |
| `MaxTextureRepeat` | 8192 | Max texture coordinate |
| `MaxTextureAspectRatio` | 0 | 0 = no restriction |
| `MaxTextureLODBias` | 16.0f | |
| `MaxSimultaneousTextures` | 16 | Stages 0-15 |
| `MaxActiveLights` | 8 | Fixed-function lights |

## Render Targets

| Field | Value | Notes |
|---|---|---|
| `NumSimultaneousRTs` | 4 | MRT; Metal guarantees at least 4 color attachments |
| `MaxRenderTargetWidth/Height` | Same as MaxTextureWidth/Height | |

## Vertex Processing

| Field | Value | Notes |
|---|---|---|
| `MaxStreams` | 16 | Stream sources 0-15 |
| `MaxStreamStride` | 255 | Bytes per vertex element |
| `MaxPrimitiveCount` | 16777215 | 2^24 - 1 |
| `MaxVertexIndex` | 16777215 | |
| `MaxVertexBlendMatrices` | 4 | D3DVBF_3WEIGHTS |
| `MaxVertexBlendMatrixIndex` | 255 | Indexed vertex blend |
| `FVFCaps` | D3DFVFCAPS_PSIZE \| (8 << D3DFVFCAPS_TEXCOORDCOUNTMASK) | Point size + 8 tex coord sets |
| `VertexProcessingCaps` | TEXGEN \| MATERIALSOURCE7 \| DIRECTIONALLIGHTS \| POSITIONALLIGHTS \| LOCALVIEWER \| TWEENING | |

## Misc

| Field | Value | Notes |
|---|---|---|
| `DevCaps` | EXECUTESYSTEMMEMORY \| TLVERTEXSYSTEMMEMORY \| TLVERTEXVIDEOMEMORY \| TEXTURESYSTEMMEMORY \| TEXTUREVIDEOMEMORY \| DRAWPRIMTLVERTEX \| CANRENDERAFTERFLIP \| HWRASTERIZATION \| HWTRANSFORMANDLIGHT \| PUREDEVICE \| QUINTICRTPATCHES \| RTPATCHES | |
| `DevCaps2` | D3DDEVCAPS2_STREAMOFFSET \| D3DDEVCAPS2_DMAPNPATCH \| D3DDEVCAPS2_ADAPTIVETESSRTPATCH \| D3DDEVCAPS2_ADAPTIVETESSNPATCH \| D3DDEVCAPS2_CAN_STRETCHRECT_FROM_TEXTURES \| D3DDEVCAPS2_PRESAMPLEDDMAPNPATCH \| D3DDEVCAPS2_VERTEXELEMENTSCANSHARESTREAMOFFSET | |
| `MaxPointSize` | 256.0f | |
| `MasterAdapterOrdinal` | 0 | |
| `AdapterOrdinalInGroup` | 0 | |
| `NumberOfAdaptersInGroup` | 1 | |
| `DeclTypes` | All D3DDTCAPS_* matching the vertex format table in `specs/d3d9/formats/design.md` | |
