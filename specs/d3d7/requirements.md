# D3D7 / DirectDraw 7 Layer Requirements

The D3D7 layer exposes `IDirectDraw7`, `IDirect3D7`, `IDirect3DDevice7`, and
`IDirectDrawSurface7` to Win32 applications. It ships as `ddraw.dll`. All 3D
rendering is translated into calls on the existing dxmt9 D3D9 layer. The 2D
DirectDraw surface model (`IDirectDrawSurface7`) doubles as the D3D7 texture
and render-target type; the wrapper classifies each surface based on its
`DDSCAPS2` flags at creation time and backs it with the appropriate D3D9 resource.

No new Metal backend work is required.

---

## 1. Entry Points

**R-D3D7-1.1** `ddraw.dll` must export `DirectDrawCreate(lpGUID, lplpDD, pUnk)`.
When `lpGUID` is NULL or `DDCREATE_HARDWAREONLY`, it must return an `IDirectDraw*`
(IDirectDraw1). Applications that subsequently `QueryInterface` for `IID_IDirectDraw7`
must receive a valid `IDirectDraw7*`.

**R-D3D7-1.2** `ddraw.dll` must export `DirectDrawCreateEx(lpGUID, lplpDD, iid, pUnk)`.
When `iid == IID_IDirectDraw7` it must return a valid `IDirectDraw7*` directly.
For any other IID it must return `DDERR_INVALIDPARAMS`.

**R-D3D7-1.3** `ddraw.dll` must export `DirectDrawEnumerate` and
`DirectDrawEnumerateEx` as stubs that enumerate a single "primary display" entry
backed by the Metal device's adapter identifier.

**R-D3D7-1.4** `ddraw.dll` must export `DirectDrawCreateClipper`. The returned
`IDirectDrawClipper*` must be a minimal stub sufficient to satisfy
`SetClipper` calls from windowed apps; no actual clipping is required.

---

## 2. IDirectDraw7

**R-D3D7-2.1** `SetCooperativeLevel(hwnd, flags)` must record `hwnd` as the
device window and accept both `DDSCL_NORMAL` (windowed) and `DDSCL_FULLSCREEN |
DDSCL_EXCLUSIVE` (fullscreen) flags. The window handle is used when creating the
implicit swap chain.

**R-D3D7-2.2** `SetDisplayMode(w, h, bpp, refresh, flags)` for fullscreen mode
must store the requested resolution and format. It must take effect when the
D3D device is created.

**R-D3D7-2.3** `RestoreDisplayMode()` must return `DD_OK` and restore windowed
behaviour.

**R-D3D7-2.4** `GetDisplayMode(pDesc)` must fill a `DDSURFACEDESC2` with the
current display resolution, bit depth, and refresh rate.

**R-D3D7-2.5** `GetDeviceIdentifier(pIdent, flags)` must populate
`DDDEVICEIDENTIFIER2` with the adapter name, driver version, and a unique GUID
derived from the Metal device's registry ID.

**R-D3D7-2.6** `CreateSurface(pDesc, ppSurface, pUnk)` must construct an
`IDirectDrawSurface7` wrapper classified by the `DDSCAPS2` flags in `pDesc`
(see section 4).

**R-D3D7-2.7** `CreatePalette(flags, pEntries, ppPalette, pUnk)` must return
a minimal `IDirectDrawPalette*` stub. Palettized textures are not required to
render correctly; the stub must not crash.

**R-D3D7-2.8** `QueryInterface(IID_IDirect3D7, ppD3D7)` must return a valid
`IDirect3D7*` associated with this `IDirectDraw7` instance.

**R-D3D7-2.9** `EnumDisplayModes(flags, pDesc, ctx, cb)` must enumerate the
same display modes as `IDirect3D9::EnumAdapterModes`, converting each to
`DDSURFACEDESC2` format.

**R-D3D7-2.10** `WaitForVerticalBlank(flags, event)` must return `DD_OK`. A
best-effort wait is acceptable.

**R-D3D7-2.11** `FlipToGDISurface()` must return `DD_OK` as a no-op.

---

## 3. IDirect3D7

**R-D3D7-3.1** `IDirect3D7` must be obtained by calling
`IDirectDraw7::QueryInterface(IID_IDirect3D7)`. It shares lifetime with its
parent `IDirectDraw7`.

**R-D3D7-3.2** `EnumDevices(callback, context)` must invoke the callback once
for the HAL device (`IID_IDirect3DHALDevice`). Reference and software devices
are not required.

**R-D3D7-3.3** `CreateDevice(REFCLSID, pBackBuffer, ppDevice)` must:
1. Derive present parameters from the `IDirectDrawSurface7*` render target
   (`pBackBuffer`): width, height, format, windowed/fullscreen from the
   surface descriptor.
2. Call `IDirect3D9::CreateDevice` to create an `IDirect3DDevice9`.
3. Return an `IDirect3DDevice7` wrapper backed by that device.

Only `IID_IDirect3DHALDevice` is required; any other CLSID must return
`DDERR_NODIRECTDRAWHW`.

**R-D3D7-3.4** `CreateVertexBuffer(pDesc, ppVB, flags)` must create an
`IDirect3DVertexBuffer7` wrapper backed by an `IDirect3DVertexBuffer9`.
The `D3DVERTEXBUFFERDESC` supplies FVF and vertex count.

**R-D3D7-3.5** `EnumZBufferFormats(REFCLSID, callback, context)` must enumerate
the depth/stencil formats supported by the D3D9 backend for the HAL device.

**R-D3D7-3.6** `EvictManagedTextures()` must call
`IDirect3DDevice9::ResourceManagerDiscardBytes(0)` or return `D3D_OK` as a
no-op if unavailable.

---

## 4. IDirectDrawSurface7 Classification

A single `IDirectDrawSurface7` wrapper may back different D3D9 resource types
depending on the caps passed to `IDirectDraw7::CreateSurface`. The wrapper must
be classified at creation time and must not change classification after creation.

**R-D3D7-4.1** A surface with `DDSCAPS_PRIMARYSURFACE` is the front buffer.
The wrapper represents the swap chain's implicit back buffer and delegates
`Flip()` / `Blt()` to `IDirect3DDevice9::Present`.

**R-D3D7-4.2** A surface with `DDSCAPS_BACKBUFFER` is the swap chain back buffer.
It maps to `IDirect3DDevice9::GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO)`.

**R-D3D7-4.3** A surface with `DDSCAPS_ZBUFFER` is a depth/stencil surface.
It maps to a `D3DPOOL_DEFAULT` `IDirect3DSurface9` created with
`CreateDepthStencilSurface`. The pixel format is derived from `ddpfPixelFormat`
using the format mapping table (see `docs/research/dx7-api.md`).

**R-D3D7-4.4** A surface with `DDSCAPS_TEXTURE` and `DDSCAPS2_D3DTEXTUREMANAGE`
or `DDSCAPS2_TEXTUREMANAGE` is a managed texture. It maps to an
`IDirect3DTexture9` in `D3DPOOL_MANAGED`.

**R-D3D7-4.5** A surface with `DDSCAPS_TEXTURE | DDSCAPS_3DDEVICE` (render
target texture) maps to an `IDirect3DTexture9` with `D3DUSAGE_RENDERTARGET` in
`D3DPOOL_DEFAULT`.

**R-D3D7-4.6** A surface with `DDSCAPS2_CUBEMAP` maps to an
`IDirect3DCubeTexture9`. `GetAttachedSurface` with a `DDSCAPS2_CUBEMAP_*FACE`
cap must return the wrapper for the corresponding `IDirect3DSurface9` face.

**R-D3D7-4.7** A surface with `DDSCAPS2_VOLUME` maps to a level of an
`IDirect3DVolumeTexture9`.

**R-D3D7-4.8** A mip chain (`DDSCAPS_MIPMAP | DDSCAPS_COMPLEX`) must be created
as a mipmapped texture. `GetAttachedSurface(DDSCAPS_MIPMAP)` must traverse the
mip chain to return the next smaller level.

**R-D3D7-4.9** If `DDSCAPS2_HINTDYNAMIC` is set, the equivalent D3D9 resource
must use `D3DUSAGE_DYNAMIC | D3DPOOL_DEFAULT`.

---

## 5. IDirectDrawSurface7 Operations

**R-D3D7-5.1** `Lock(pRect, pDesc, flags, event)` must map the underlying D3D9
resource and return a CPU-writable pointer in `pDesc->lpSurface`. The pitch must
be written to `pDesc->lPitch`.

**R-D3D7-5.2** `Unlock(pRect)` must unmap the D3D9 resource. Changes made while
locked must be visible to subsequent GPU draws.

**R-D3D7-5.3** `Blt(pDstRect, pSrcSurface, pSrcRect, flags, pFX)` must:
- When called on the primary surface with the back buffer as source: delegate to
  `IDirect3DDevice9::Present`. Destination rect and source rect may be ignored.
- When called between two non-primary surfaces: delegate to
  `IDirect3DDevice9::StretchRect` for GPU-resident surfaces or
  `IDirect3DDevice9::UpdateSurface` for system-memory source.
- `DDBLT_COLORFILL` must delegate to `IDirect3DDevice9::ColorFill`.

**R-D3D7-5.4** `BltFast(x, y, pSrc, pSrcRect, flags)` must delegate to
`IDirect3DDevice9::UpdateSurface` (system→default) or `StretchRect`
(default→default) with no scaling.

**R-D3D7-5.5** `Flip(pTargetSurface, flags)` must call
`IDirect3DDevice9::Present(NULL, NULL, NULL, NULL)`.

**R-D3D7-5.6** `GetSurfaceDesc(pDesc)` must populate `DDSURFACEDESC2` with the
current surface dimensions, pixel format, and caps.

**R-D3D7-5.7** `GetDC(pHDC)` / `ReleaseDC(hDC)` must return `DDERR_UNSUPPORTED`.
GDI DC access to GPU surfaces is not required.

**R-D3D7-5.8** `SetPriority` / `GetPriority` and `SetLOD` / `GetLOD` must
delegate to the corresponding `IDirect3DBaseTexture9` methods for texture
surfaces, and return `DD_OK` / 0 for non-texture surfaces.

**R-D3D7-5.9** `IsLost()` must return `DD_OK` while the device is operational.
`Restore()` must return `DD_OK`.

---

## 6. IDirect3DDevice7

**R-D3D7-6.1** `BeginScene()` and `EndScene()` must delegate to the D3D9 device.

**R-D3D7-6.2** `DrawPrimitive(type, FVF, pVertices, vertexCount, flags)` must:
1. Compute primitive count from `vertexCount` and `type`.
2. Call `IDirect3DDevice9::DrawPrimitiveUP(type, primCount, pVertices, stride)`.
   The stride is derived from the FVF.

**R-D3D7-6.3** `DrawIndexedPrimitive(type, FVF, pVertices, vCount, pIndices,
iCount, flags)` must compute primitive count from `iCount` and call
`IDirect3DDevice9::DrawIndexedPrimitiveUP`.

**R-D3D7-6.4** `DrawPrimitiveStrided(type, FVF, pStrided, vCount, flags)` must
pack the strided vertex data into a temporary unified buffer and call
`DrawPrimitiveUP`. The temporary buffer must remain valid until the call returns.

**R-D3D7-6.5** `DrawIndexedPrimitiveStrided(type, FVF, pStrided, vCount, pIdx,
iCount, flags)` must pack strided data and call `DrawIndexedPrimitiveUP`.

**R-D3D7-6.6** `DrawPrimitiveVB(type, pVB7, startVertex, count, flags)` must:
1. Set the vertex buffer via `IDirect3DDevice9::SetStreamSource`.
2. Set the FVF via `IDirect3DDevice9::SetFVF`.
3. Call `IDirect3DDevice9::DrawPrimitive(type, startVertex, primCount)`.

**R-D3D7-6.7** `DrawIndexedPrimitiveVB(type, pVB7, start, count, pIdx, iCount,
flags)` must upload `pIdx` into a temporary index buffer and call
`DrawIndexedPrimitive`.

**R-D3D7-6.8** `SetRenderTarget(pSurface7, flags)` must call
`IDirect3DDevice9::SetRenderTarget(0, pSurf9)` using the inner surface pointer.

**R-D3D7-6.9** `GetRenderTarget(ppSurface7)` must return the current render
target wrapped as `IDirectDrawSurface7`.

**R-D3D7-6.10** `SetViewport(pViewport7)` must translate `D3DVIEWPORT7` to
`D3DVIEWPORT9` (identical fields, direct cast is valid) and call
`IDirect3DDevice9::SetViewport`.

**R-D3D7-6.11** `SetTransform(stateType, pMatrix)` must translate the D3D7
transform type constant to `D3DTRANSFORMSTATETYPE` and call
`IDirect3DDevice9::SetTransform`. The four D3D7 world matrix states
(`D3DTRANSFORMSTATE_WORLD` = 256, `WORLD1`–`WORLD3` = 257–259) map to
`D3DTS_WORLD` / `D3DTS_WORLD1`–`D3DTS_WORLD3`.

**R-D3D7-6.12** `SetRenderState(rsType, value)` must translate the D3D7
`D3DRENDERSTATE_*` enum value to the corresponding `D3DRS_*` value and call
`IDirect3DDevice9::SetRenderState`. States with no D3D9 equivalent
(`D3DRENDERSTATE_ANTIALIAS`, `D3DRENDERSTATE_TEXTUREPERSPECTIVE`, etc.) must
be silently ignored (return `DD_OK`).

**R-D3D7-6.13** `SetTextureStageState(stage, type, value)` for the ten sampler-
related D3D7 TSS indices must call `IDirect3DDevice9::SetSamplerState`, applying
the same remapping defined in R-D3D8-8.1. All other indices must call
`IDirect3DDevice9::SetTextureStageState`.

**R-D3D7-6.14** `SetTexture(stage, pSurface7)` must extract the inner D3D9
texture pointer from the `IDirectDrawSurface7` wrapper and call
`IDirect3DDevice9::SetTexture(stage, pTex9)`.

**R-D3D7-6.15** `SetLight(index, pLight7)` must cast `D3DLIGHT7*` to `D3DLIGHT9*`
(structures are binary-identical) and call `IDirect3DDevice9::SetLight`.

**R-D3D7-6.16** `SetMaterial(pMaterial7)` must cast `D3DMATERIAL7*` to
`D3DMATERIAL9*` (structures are binary-identical) and call
`IDirect3DDevice9::SetMaterial`.

**R-D3D7-6.17** `LightEnable`, `GetLightEnable`, `SetClipPlane`, `GetClipPlane`,
`MultiplyTransform`, `ValidateDevice`, `GetCaps`, `Clear` must each delegate to
the corresponding D3D9 method. `Clear` parameters are identical.

**R-D3D7-6.18** `BeginStateBlock()` / `EndStateBlock(pToken)` /
`ApplyStateBlock(token)` / `CaptureStateBlock(token)` / `DeleteStateBlock(token)` /
`CreateStateBlock(type, pToken)` must be implemented using the same token table
design as the D3D8 layer (R-D3D8-5.1 through R-D3D8-5.6).

---

## 7. IDirect3DVertexBuffer7

**R-D3D7-7.1** `Lock(flags, ppData, pSize)` must map the underlying
`IDirect3DVertexBuffer9` and return a CPU-writable pointer. `pSize` must receive
the buffer size in bytes.

**R-D3D7-7.2** `Unlock()` must unmap the D3D9 vertex buffer.

**R-D3D7-7.3** `GetVertexBufferDesc(pDesc)` must populate `D3DVERTEXBUFFERDESC`
from the underlying buffer's D3D9 description.

**R-D3D7-7.4** `ProcessVertices` and `ProcessVerticesStrided` must return
`D3DERR_INVALIDCALL`. Hardware T&L via `ProcessVertices` is not required; apps
that need it must use `DrawPrimitive` directly.

**R-D3D7-7.5** `Optimize(pDevice, flags)` must return `D3D_OK` as a no-op.

---

## 8. Vertex Count to Primitive Count

**R-D3D7-8.1** All draw calls that accept a vertex count must convert to primitive
count before calling D3D9, using:

| D3DPRIMITIVETYPE | primCount formula |
|---|---|
| D3DPT_POINTLIST | vertexCount |
| D3DPT_LINELIST | vertexCount / 2 |
| D3DPT_LINESTRIP | vertexCount − 1 |
| D3DPT_TRIANGLELIST | vertexCount / 3 |
| D3DPT_TRIANGLESTRIP | vertexCount − 2 |
| D3DPT_TRIANGLEFAN | vertexCount − 2 |

`D3DPT_TRIANGLEFAN` is supported by D3D9 at the API level but decomposed to a
triangle list by the existing core layer before submission to Metal.

---

## 9. DDPIXELFORMAT Parsing

**R-D3D7-9.1** `CreateSurface` must parse `DDSURFACEDESC2::ddpfPixelFormat` to
determine the D3D9 `D3DFORMAT`. The mapping must cover at minimum:

| DDPF flags + bit masks | D3DFORMAT |
|---|---|
| RGB 32bpp A=FF000000 R=FF0000 G=FF00 B=FF | D3DFMT_A8R8G8B8 |
| RGB 32bpp A=0 R=FF0000 G=FF00 B=FF | D3DFMT_X8R8G8B8 |
| RGB 16bpp R=F800 G=7E0 B=1F | D3DFMT_R5G6B5 |
| RGB 16bpp R=7C00 G=3E0 B=1F A=8000 | D3DFMT_A1R5G5B5 |
| RGB 16bpp R=F00 G=F0 B=F A=F000 | D3DFMT_A4R4G4B4 |
| FourCC 'DXT1' | D3DFMT_DXT1 |
| FourCC 'DXT3' | D3DFMT_DXT3 |
| FourCC 'DXT5' | D3DFMT_DXT5 |
| ZBUFFER 16bpp | D3DFMT_D16 |
| ZBUFFER 32bpp | D3DFMT_D32 |
| ZBUFFER+STENCIL 32bpp | D3DFMT_D24S8 |

Unrecognised formats must return `DDERR_INVALIDPIXELFORMAT`.

---

## 10. Error Handling

**R-D3D7-10.1** All methods must return documented DirectDraw and Direct3D
HRESULT codes. `DD_OK` (0) on success. `DDERR_INVALIDPARAMS` for bad arguments.
`DDERR_OUTOFVIDEOMEMORY` for allocation failures.

**R-D3D7-10.2** `IDirectDrawSurface7` methods called on a surface whose
underlying D3D9 resource has been destroyed by a device Reset must return
`DDERR_SURFACELOST`. `Restore()` must attempt to recreate it.
