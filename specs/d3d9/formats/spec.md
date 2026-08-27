---
type: "Spec"
title: "Format Mapping Spec"
description: "D3D9 / Formats spec, ownership, ordering, and evidence mapping."
tags: [specs, d3d9, formats, spec]
---

# Format Mapping Spec

This document defines the authoritative mapping between D3D9 surface formats
(`D3DFORMAT`) and Metal pixel formats (`MTLPixelFormat`), and classifies each
format as **Required**, **Optional**, or **Unsupported** for dxmt9.

## Color / Render Target Formats

| D3DFORMAT | MTLPixelFormat | Support | Notes |
|---|---|---|---|
| D3DFMT_A8R8G8B8 | BGRA8Unorm | **Required** | Primary backbuffer format |
| D3DFMT_X8R8G8B8 | BGRA8Unorm | **Required** | Alpha treated as 1.0 on read |
| D3DFMT_A8B8G8R8 | RGBA8Unorm | **Required** | |
| D3DFMT_X8B8G8R8 | RGBA8Unorm | **Required** | Alpha treated as 1.0 on read |
| D3DFMT_R5G6B5 | B5G6R5Unorm | **Required** | |
| D3DFMT_A1R5G5B5 | BGR5A1Unorm | **Required** | |
| D3DFMT_X1R5G5B5 | BGR5A1Unorm | **Required** | Alpha forced to 1 |
| D3DFMT_A4R4G4B4 | ABGR4Unorm | **Required** | |
| D3DFMT_A8 | A8Unorm | **Required** | Alpha-only |
| D3DFMT_R8G8B8 | none | **Unsupported** | No 24-bit Metal format; apps must use A8R8G8B8 |
| D3DFMT_A16B16G16R16F | RGBA16Float | **Required** | HDR render target |
| D3DFMT_A32B32G32R32F | RGBA32Float | **Required** | Full precision HDR |
| D3DFMT_G16R16F | RG16Float | **Required** | Two-component FP |
| D3DFMT_R16F | R16Float | **Required** | Single-channel FP |
| D3DFMT_G32R32F | RG32Float | **Required** | |
| D3DFMT_R32F | R32Float | **Required** | |
| D3DFMT_A16B16G16R16 | RGBA16Unorm | **Required** | 16-bit normalized |
| D3DFMT_G16R16 | RG16Unorm | **Required** | |
| D3DFMT_A2R10G10B10 | RGB10A2Unorm | **Required** | 10-bit HDR |
| D3DFMT_A2B10G10R10 | BGR10A2Unorm | **Optional** | Check `MTLPixelFormatBGR10A2Unorm` availability |
| D3DFMT_L8 | R8Unorm | **Required** | Single channel; shader must replicate to RGB |
| D3DFMT_L16 | R16Unorm | **Required** | |
| D3DFMT_A8L8 | RG8Unorm | **Required** | R=luminance, G=alpha; shader remaps |
| D3DFMT_V8U8 | RG8Snorm | **Required** | Signed two-channel (bump) |
| D3DFMT_Q8W8V8U8 | RGBA8Snorm | **Required** | Signed four-channel |
| D3DFMT_V16U16 | RG16Snorm | **Required** | |
| D3DFMT_Q16W16V16U16 | RGBA16Snorm | **Optional** | Signed four-channel 16-bit (bump). Color/sampling supported; FormatInfo currently mirrors `A16B16G16R16` including `renderTarget=true` — real D3D9 may not advertise RT for it (minor caps over-report, tracked in `specs/d3d9/gap_d3d9.md`) |
| D3DFMT_CxV8U8 | none | **Unsupported** | Computed normal map; no Metal equivalent |

### BGRA Byte-Order Note

D3D9's `D3DFMT_A8R8G8B8` stores bytes in memory as B, G, R, A order (little-endian
ARGB dword). Metal's `MTLPixelFormatBGRA8Unorm` matches this layout exactly. No
swizzle is needed for this format when used as a texture or render target.

For shader sampling, Metal presents BGRA textures with components in (B, G, R, A)
order in RGBA register slots. When translating D3D9 shaders, the translator must
not insert a swizzle; the D3D9 bytecode already references components by semantic
meaning (RGBA), and the hardware delivers them correctly via `BGRA8Unorm`.

For `D3DFMT_A8B8G8R8` (RGBA layout), Metal's `RGBA8Unorm` is used with no swizzle.

For `D3DFMT_L8` (luminance), the shader for fixed-function or translated programs
must replicate the R channel to G and B: `color.rgb = texture.rrr`.

## Depth / Stencil Formats

| D3DFORMAT | MTLPixelFormat | Support | Notes |
|---|---|---|---|
| D3DFMT_D24S8 | Depth24Unorm_Stencil8 | **Required (macOS)** | Not available on iOS/tvOS; see note |
| D3DFMT_D24X8 | Depth24Unorm_Stencil8 | **Required (macOS)** | Stencil present but ignored by app |
| D3DFMT_D16 | Depth16Unorm | **Required** | |
| D3DFMT_D32 | Depth32Float | **Required** | Use float; D3D9 D32 = 32-bit fixed |
| D3DFMT_D32F_LOCKABLE | Depth32Float | **Required** | Lockable float depth; staging readback |
| D3DFMT_D32_LOCKABLE | Depth32Float | **Required** | Lockable 32-bit depth (D3D9b); Metal has no 32-bit-int depth, so mapped to Depth32Float like D32F_LOCKABLE |
| D3DFMT_D16_LOCKABLE | Depth16Unorm | **Required** | Staging readback |
| D3DFMT_D15S1 | none | **Unsupported** | No Metal equivalent |
| D3DFMT_D24X4S4 | none | **Unsupported** | No Metal equivalent |
| D3DFMT_D24FS8 | Depth32Float_Stencil8 | **Optional** | Floating-point depth with stencil |
| D3DFMT_S8_LOCKABLE | none | **Unsupported** | Stencil-only surface |

**`Depth24Unorm_Stencil8` availability:** This format is not universally available in
Metal. On Apple silicon Macs, query `[device supportsFamily:MTLGPUFamilyMac2]` or
check `MTLPixelFormatDepth24Unorm_Stencil8` via `[device isDepth24Stencil8PixelFormatSupported]`.
If unavailable, `D3DFMT_D24S8` must fall back to `Depth32Float_Stencil8`.
`CheckDeviceFormat(D3DFMT_D24S8)` must return `D3D_OK` in both cases because the
format is supported, just mapped differently.

## Compressed Texture Formats (BC / DXTn)

| D3DFORMAT | MTLPixelFormat | Support | Notes |
|---|---|---|---|
| D3DFMT_DXT1 | BC1_RGBA | **Required** | 4bpp, 1-bit alpha |
| D3DFMT_DXT2 | BC2_RGBA | **Required** | 8bpp, premul alpha (treat as DXT3) |
| D3DFMT_DXT3 | BC2_RGBA | **Required** | 8bpp, explicit alpha |
| D3DFMT_DXT4 | BC3_RGBA | **Required** | 8bpp, premul alpha (treat as DXT5) |
| D3DFMT_DXT5 | BC3_RGBA | **Required** | 8bpp, interpolated alpha |
| D3DFMT_ATI1 / D3DFMT_BC4 | BC4_RUnorm | **Required** | 1-channel compressed |
| D3DFMT_ATI2 / D3DFMT_BC5 | BC5_RGUnorm | **Required** | 2-channel compressed (normal maps) |

BC formats are supported on all Metal devices (Mac and Apple silicon).

## Index Buffer Formats

| D3DFORMAT | Metal index type | Support |
|---|---|---|
| D3DFMT_INDEX16 | MTLIndexTypeUInt16 | **Required** |
| D3DFMT_INDEX32 | MTLIndexTypeUInt32 | **Required** |

## Vertex Buffer Formats (D3DDECLTYPE to MTLVertexFormat)

| D3DDECLTYPE | MTLVertexFormat | Notes |
|---|---|---|
| FLOAT1 | Float | |
| FLOAT2 | Float2 | |
| FLOAT3 | Float3 | |
| FLOAT4 | Float4 | |
| D3DCOLOR | UChar4Normalized_BGRA | BGRA byte order to RGBA float [0,1] |
| UBYTE4 | UChar4 | Unsigned byte, no normalization |
| UBYTE4N | UChar4Normalized | |
| SHORT2 | Short2 | |
| SHORT4 | Short4 | |
| SHORT2N | Short2Normalized | |
| SHORT4N | Short4Normalized | |
| USHORT2N | UShort2Normalized | |
| USHORT4N | UShort4Normalized | |
| UDEC3 | UInt1010102Normalized | 10-10-10-2, unsigned |
| DEC3N | Int1010102Normalized | 10-10-10-2, signed normalized |
| FLOAT16_2 | Half2 | |
| FLOAT16_4 | Half4 | |

`D3DCOLOR` in a vertex buffer is stored as BGRA.
`MTLVertexFormatUChar4Normalized_BGRA` reads bytes as B,G,R,A and presents them as
`float4(r,g,b,a)` in the shader. The swizzle is handled by the Metal vertex fetch
hardware, so the generated vertex shader sees the correct RGBA order without any
extra instructions.

## Format Conversion at Upload

Formats that have no exact Metal equivalent require CPU-side conversion when texture
data is uploaded via `Lock`/`Unlock`:

| D3DFORMAT | Conversion | Target MTLPixelFormat |
|---|---|---|
| D3DFMT_R8G8B8 (24bpp) | Expand to 32bpp: insert A=0xFF | BGRA8Unorm |
| D3DFMT_X8R8G8B8 | Set A=0xFF in-place | BGRA8Unorm |
| D3DFMT_X1R5G5B5 | Set A=1 | BGR5A1Unorm |
| D3DFMT_L8 | Copy R channel, pad G=B=R, A=0xFF | RGBA8Unorm (or keep as R8Unorm) |
| D3DFMT_A8L8 | R=L, G=A, expand to RGBA | RGBA8Unorm |

Conversion is performed by the core on the CPU during the Lock/Unlock cycle. The
backend receives converted data in the target Metal format.

## Vendor Pseudo-Formats and Hardware-Hack FOURCCs

Per R-FORMAT-7 every FOURCC pseudo-format must be explicitly classified.
Three are depth-as-texture sampler formats backed by real Metal depth
formats; the rest are command triggers or capability probes (normative
behaviour in R-FORMAT-11..14, R-CORE-3.9).

| D3DFORMAT (FOURCC) | Metal mapping / mechanism | Support | Notes |
|---|---|---|---|
| `INTZ` | Depth32Float, sampled as texture | **Optional** | Depth-as-texture; sample returns depth in `.r`. Implemented. |
| `DF16` | Depth16Unorm, sampled as texture | **Optional** | 16-bit depth-as-texture; mirrors INTZ. Implemented. |
| `DF24` | Depth32Float, sampled as texture | **Optional** | 24-bit nominal; Depth32Float over-provisioned (no 24-bit depth-only Metal format). Implemented. |
| `RESZ` | Command: MSAA depth-resolve trigger | **Optional** | R-FORMAT-11. Sentinel `D3DRS_POINTSIZE = 0x7FA05000` resolves the bound multisampled depth into an INTZ texture. |
| `NULL` | Colorless render pass (no color storage) | **Optional** | R-FORMAT-12. Depth/stencil-only passes; backend omits / `DontCare`s the color attachment. |
| `ATOC` | Render state: `alphaToCoverageEnabled` | n/a (state hack) | R-FORMAT-13. Enabled via `D3DRS_ADAPTIVETESS_Y`; ATI `A2M1`/`A2M0` aliases. Not a creatable surface. |
| `NVDB` | none — no Metal depth-bounds test | **Unsupported** | R-FORMAT-14. `CheckDeviceFormat` → `D3DERR_NOTAVAILABLE`; correctness-neutral perf optimization, apps fall back. |
| `RAWZ` | none | **Unsupported** | Legacy NVIDIA raw-depth read; no Metal path. |
| `ATI1` / `ATI2` | BC4_RUnorm / BC5_RGUnorm | **Required** | Compressed; see Compressed Texture Formats table. |

R-FORMAT-15 fixes the Metal shader ABI for the three depth-as-texture formats.
For an active ordinary fragment sample, pipeline resolution classifies the
bound resource from the pool, records the stage mask in `ShaderVariantKey`, and
emits a direct `depth2d<float>` parameter. Metal's scalar depth result is
widened to the D3D single-channel value `(depth, 0, 0, 1)`. The slot-30
resource-array lane remains color-`texture2d` only and fails closed for all
native depth resources. GET4 is deliberately orthogonal: it keeps the existing
direct gather compatibility lowering and is also excluded from the resource
array.

R-FORMAT-16 separates the two mip topologies that D3D9 AUTOGEN requires.
`TextureDesc::levels` remains the public topology (`1`), while
`TextureRecord::physicalMipLevels` is the full base-extent pyramid used to
create the Metal texture and its shader views. PE texture objects own the
recorder-thread-confined `Clean`/`Dirty` semantic state. Successful level-0
surface writes enter `Dirty` regardless of whether they came from CPU unlock,
render-target clear/draw, `UpdateSurface`, `StretchRect`, or `ColorFill`; an
ordered `UpdateTexture` generates its AUTOGEN destination within the copy
operation. A sampling draw first publishes the preceding command chunk,
crosses the replay drain fence, generates the hidden pyramid through the WMT
blit encoder, settles `Clean`, and then records the draw. Generation failure
preserves `Dirty` and fails the draw. `AutogenMipGeneration.tla` owns the
bounded write/publish/generate/sample refinement. Native evidence consists of
the model-bound dirty transition truth table and a real RGBA16Float 1024x1024,
11-level Metal reduction oracle; Wine conformance pins the capability result
and one-level public API.

### FETCH4 sampler control

ATI FETCH4 is a sampler-state control rather than a creatable format. Writing
`GET4` (`MAKEFOURCC('G','E','T','4')`) to `D3DSAMP_MIPMAPLODBIAS` enables a
four-neighbour red-channel fetch for a compatible 2D texture while
`D3DSAMP_MAGFILTER` is `D3DTEXF_POINT`; writing `GET1` disables it. The runtime
accepts `INTZ`, `DF16`, `DF24`, `R16F`, `R32F`, `A8`, `L8`, and `L16` for this
compatibility path. GET4/GET1 are never interpreted as floating-point LOD
biases.

The Metal lowering uses `gather(..., component::x)` with the compatibility
offset `0.498046875 / textureSize` and reorders the result to D3D9 FETCH4
channel order. `TEX`, `TEXLDL`, and `TEXLDD` all select that gather while
FETCH4 is active; explicit level and gradient operands do not alter the
vendor operation. The per-sampler mask is part of `ShaderVariantKey` and
`ShaderSourceContext`, so ordinary sampling and FETCH4 cannot share a stale
pipeline or source. The public `CheckDeviceFormat(GET4)` capability probe
remains fail-closed with `D3DERR_NOTAVAILABLE`; this is an unadvertised runtime
compatibility path for applications that use the raw sampler token.

**RESZ resolve mechanism.** RESZ is not storage. The runtime trigger is
`SetRenderState(D3DRS_POINTSIZE, 0x7FA05000)` while the multisampled
depth surface is bound at texture stage 0. The PE layer recognises the
sentinel (distinct from any plausible point size), records a
depth-resolve command referencing the bound MSAA depth source and the
INTZ destination, and the backend performs the resolve as a Metal
render-pass depth-resolve attachment (`MTLMultisampleDepthResolveFilter`,
typically `Sample`). A non-sentinel `D3DRS_POINTSIZE` keeps its normal
point-size meaning and must not trigger a resolve.

**NULL render target.** A `NULL` surface allocates no color backing. When
bound as the active render target the backend configures the render pass
with no color writes — the color attachment is omitted, or present with
`MTLStoreActionDontCare` and no allocated texture — so the depth/stencil
attachment is the effective target. This supports the common depth-only
shadow pass and stencil-shadow-volume pass without wasting color
bandwidth. Locking or reading back a NULL surface returns
`D3DERR_INVALIDCALL`.

**ATOC alpha-to-coverage.** Alpha-to-coverage is a render-state hack, not
a creatable format. The PE layer detects `D3DRS_ADAPTIVETESS_Y ==
MAKEFOURCC('A','T','O','C')` (enable) or `0` (disable), folds an
alpha-to-coverage bit into the pipeline-state key so enabled and disabled
draws hash to distinct PSOs, and the backend sets
`MTLRenderPipelineDescriptor.alphaToCoverageEnabled`. The ATI
`A2M1`/`A2M0` tokens are accepted as aliases.

**Implementation status.** INTZ/DF16/DF24 are implemented. RESZ, NULL,
ATOC are specified here but not yet implemented; NVDB and RAWZ are
specified as Unsupported. See `specs/d3d9/gap_d3d9.md` for the live status.
