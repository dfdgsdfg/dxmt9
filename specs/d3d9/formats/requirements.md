# Format Requirements

This document defines the normative D3D9 format support contract for
`D3DFORMAT` classification, `CheckDeviceFormat()`, and format conversion checks.
Concrete D3D-to-Metal mapping tables and data-layout notes live in
`specs/d3d9/formats/design.md`.

## Classification Contract

- **Required** formats must be supported; `CheckDeviceFormat()` returns `D3D_OK`
  for valid usage/resource combinations unless a rule below restricts that usage.
- **Optional** formats are supported only when the Metal device capability allows
  them; `CheckDeviceFormat()` returns `D3D_OK` only if confirmed at init time.
- **Unsupported** formats are never supported; `CheckDeviceFormat()` returns
  `D3DERR_NOTAVAILABLE`.

**R-FORMAT-1** Display-mode enumeration is separate from resource format support.
`D3DFMT_A8R8G8B8` is required for textures, render targets, and back buffers, but
D3D9 adapter mode enumeration must expose only `D3DFMT_X8R8G8B8` and
`D3DFMT_R5G6B5`; an internal `A8R8G8B8` current mode or swap-chain back buffer is
reported as `X8R8G8B8` by display-mode query methods.

**R-FORMAT-2** `CheckDeviceFormat(Adapter, DeviceType, AdapterFormat, Usage, RType,
CheckFormat)` must apply Windows D3D9-compatible front-end validation, using Wine
D3D9 tests as the behavioural oracle, before consulting the format tables. Valid
adapter formats are `D3DFMT_X8R8G8B8`, `D3DFMT_R5G6B5`, and `D3DFMT_X1R5G5B5`;
valid resource types are `SURFACE`, `TEXTURE`, `CUBETEXTURE`, `VOLUME`,
`VOLUMETEXTURE`, `VERTEXBUFFER`, and `INDEXBUFFER`.

**R-FORMAT-3** If `CheckFormat` is in the Unsupported table,
`CheckDeviceFormat()` must return `D3DERR_NOTAVAILABLE`.

**R-FORMAT-4** If `CheckFormat` is Required, `CheckDeviceFormat()` must return
`D3D_OK` for all valid `Usage`/`RType` combinations, subject to these restrictions:
compressed formats combined with `D3DUSAGE_RENDERTARGET` or
`D3DUSAGE_DEPTHSTENCIL` return `D3DERR_NOTAVAILABLE`; depth formats combined with
`D3DUSAGE_RENDERTARGET` return `D3DERR_NOTAVAILABLE`; `D3DFMT_L8`, `D3DFMT_L16`,
and `D3DFMT_A8L8` combined with `D3DUSAGE_RENDERTARGET` return
`D3DERR_NOTAVAILABLE`.

**R-FORMAT-5** If `CheckFormat` is Optional, dxmt9 must query device capability at
init time and return `D3D_OK` only when the capability is confirmed.

**R-FORMAT-6** Any format not listed in `specs/d3d9/formats/design.md` must return
`D3DERR_NOTAVAILABLE`.

**R-FORMAT-7** The format classifier must be centralised. FOURCC and
pseudo-formats such as `D3DFMT_ATI1`, `D3DFMT_ATI2`, `D3DFMT_RESZ`, and
`D3DFMT_NULL` must be explicitly classified; unsupported FOURCC values must not
slip through as ordinary color formats.

**R-FORMAT-8** Compressed formats are not valid as render targets.
`CheckDeviceFormat()` must return `D3DERR_NOTAVAILABLE` for any compressed format
combined with `D3DUSAGE_RENDERTARGET` or `D3DUSAGE_DEPTHSTENCIL`.

**R-FORMAT-9** `D3DFMT_D24S8` support must be reported with `D3D_OK` even when
`MTLPixelFormatDepth24Unorm_Stencil8` is unavailable, because the implementation
falls back to `Depth32Float_Stencil8`.

**R-FORMAT-10** Formats that have no exact Metal equivalent but are accepted for
upload must be converted during the core `Lock`/`Unlock` cycle before the backend
receives data.
