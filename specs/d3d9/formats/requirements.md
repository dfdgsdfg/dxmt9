---
type: "Spec Requirements"
title: "Format Requirements"
description: "D3D9 / Formats requirements and compatibility contracts."
tags: [specs, d3d9, formats, requirements]
---

# Format Requirements

This document defines the normative D3D9 format support contract for
`D3DFORMAT` classification, `CheckDeviceFormat()`, and format conversion checks.
Concrete D3D-to-Metal mapping tables and data-layout notes live in
`specs/d3d9/formats/spec.md`.

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

**R-FORMAT-6** Any format not listed in `specs/d3d9/formats/spec.md` must return
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

## Vendor Pseudo-Format and Hardware-Hack Contracts

These FOURCC values are not ordinary storage formats; they are command
triggers or capability probes that D3D9 titles drive through the format
and render-state surfaces. Concrete Metal mechanisms live in
`specs/d3d9/formats/spec.md` (§ Vendor Pseudo-Formats).

**R-FORMAT-11** `D3DFMT_RESZ` (`MAKEFOURCC('R','E','S','Z')`) is an
**Optional** *command* pseudo-format that resolves a multisampled depth
surface into a bound `INTZ` depth texture. `CheckDeviceFormat(RESZ, …,
D3DRTYPE_SURFACE)` must return `D3D_OK` only when the Metal device
supports multisample depth resolve. RESZ carries no storage: the resolve
is requested at runtime by binding the multisampled depth surface as a
texture and writing the sentinel value `0x7FA05000` to `D3DRS_POINTSIZE`.
The PE layer must detect this sentinel write and emit a depth-resolve
command; the backend resolves the multisampled depth into the bound INTZ
texture via a Metal render-pass depth-resolve attachment. A non-sentinel
`D3DRS_POINTSIZE` value must retain its ordinary point-size meaning.

**R-FORMAT-12** `D3DFMT_NULL` (`MAKEFOURCC('N','U','L','L')`) is a
null render-target pseudo-format with no color storage, used for
depth/stencil-only passes (shadow depth, stencil-shadow volumes).
`CheckDeviceFormat(NULL, D3DUSAGE_RENDERTARGET, D3DRTYPE_SURFACE)` must
return `D3D_OK`. A NULL render target must allocate no color backing;
when it is the bound render target the backend must configure a render
pass with no color writes (color attachment omitted or store action
`DontCare`), leaving the depth/stencil attachment as the effective
target. `LockRect` of a NULL surface must succeed (`D3D_OK`) and return
a lockable dummy buffer (`pBits != NULL`, non-zero `Pitch`) whose
contents are meaningless and discarded on `UnlockRect` (Wine
`dlls/d3d9/tests/device.c::test_surface_format_null`). `GetRenderTargetData`
of a NULL surface returns `D3DERR_INVALIDCALL`.

**R-FORMAT-13** Alpha-to-coverage is enabled through a cross-vendor
render-state hack, not a creatable format: writing
`MAKEFOURCC('A','T','O','C')` to `D3DRS_ADAPTIVETESS_Y` enables it and `0`
disables it (the ATI `A2M1`/`A2M0` tokens are accepted as aliases). The
PE layer must detect the enable/disable token, fold an alpha-to-coverage
bit into the pipeline-state key, and the backend must set Metal
`alphaToCoverageEnabled` accordingly. While disabled, the host render
state reverts to its ordinary meaning.

**R-FORMAT-14** `D3DFMT_NVDB` (`MAKEFOURCC('N','V','D','B')`) probes the
NVIDIA depth-bounds-test capability. Metal has no depth-bounds test, so
`CheckDeviceFormat(NVDB)` must return `D3DERR_NOTAVAILABLE`. The
depth-bounds test is a performance-only, correctness-neutral
optimization; reporting it unavailable causes conformant applications to
disable it and fall back to normal depth testing, so no rendering
regression results.

**R-FORMAT-15** A shader-visible `INTZ`, `DF16`, or `DF24` resource backed by
a Metal depth pixel format must use a typed `depth2d<float>` direct shader
binding for ordinary samples. The scalar Metal result is exposed to D3D9 as
`float4(depth, 0, 0, 1)`. The active depth-stage mask must participate in
shader/pipeline identity, and a native depth resource must never enter a
homogeneous `texture2d<float>` argument-buffer array. ATI FETCH4 is a separate
compatibility operation: validated GET4 stages retain the direct gather lane
and do not use `depth2d`, but they remain excluded from the resource array.

**R-FORMAT-16** A supported 2D or cube `D3DUSAGE_AUTOGENMIPMAP` query shall
return `D3D_OK`, not `D3DOK_NOAUTOGEN`; dynamic combinations and volume or
surface resource types remain unavailable. The D3D9 object exposes exactly one
public level while the backend owns a complete hidden mip pyramid. Every successful
level-0 write marks that pyramid dirty, including CPU unlock, clear,
render-target draw, `UpdateSurface`, `StretchRect`, and `ColorFill` paths;
`UpdateTexture` may instead generate the destination pyramid as part of the
ordered copy operation. Before the next draw samples a dirty texture, all
earlier replay effects shall precede generation, the full pyramid shall be
regenerated, and only then may the sampling draw execute. The physical
dirty-to-clean transition occurs only after successful generation. PE may
optimistically settle its recorder-local dirty bit once the exact retained
generation command is accepted, provided every post-accept failure is
fail-stop and therefore cannot expose stale mips to a successful draw.

Automatic generation required by a sampling draw shall be represented as an
ordered, texture-qualified command in the canonical chunk and backend command
streams. Production replay must encode it into the current Metal command
buffer after closing any writer render encoder and before the sampling draw;
it must not force a PE replay drain, a standalone command buffer, or a CPU
wait. Malformed or unresolved texture identity fails the containing chunk
before Metal side effects.

AUTOGEN creation accepts only `Levels=0` or `Levels=1`, rejects SYSTEMMEM, and
rejects volume textures. These are creation-time semantic errors and shall be
rejected on the PE side before a resource crosses the provider boundary.
