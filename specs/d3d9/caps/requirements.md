---
type: "Spec Requirements"
title: "Capability Requirements (D3DCAPS9)"
description: "D3D9 / Caps requirements and compatibility contracts."
tags: [specs, d3d9, caps, requirements]
---

# Capability Requirements (D3DCAPS9)

This document defines the normative `D3DCAPS9` contract for
`IDirect3D9::GetDeviceCaps()` and `IDirect3DDevice9::GetDeviceCaps()`.
The reported values must remain consistent with `CheckDeviceFormat()`,
`CheckDeviceType()`, `CheckDeviceMultiSampleType()`, and
`CheckDeviceFormatConversion()`.

## Required Contract

**R-CAPS-1** Every feature advertised in `D3DCAPS9` must be implementable by the
backend on the target Metal device. dxmt9 must not advertise capabilities it cannot
satisfy.

**R-CAPS-2** `CheckDeviceFormat(format, usage)` must return `D3D_OK` if and only if
the format is classified as Required or Optional (and confirmed available) in
`specs/d3d9/formats/requirements.md` for the given usage.

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
