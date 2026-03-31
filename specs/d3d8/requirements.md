# D3D8 Layer Requirements

The D3D8 layer is a shim that translates the `IDirect3D8` / `IDirect3DDevice8`
COM surface into calls on the existing dxmt9 D3D9 layer. It ships as `d3d8.dll`.

No new Metal backend work is required — every D3D8 capability maps to something
already in the D3D9 layer. The D3D8 layer is pure translation.

---

## 1. Entry Point

**R-D3D8-1.1** `d3d8.dll` must export `Direct3DCreate8(SDKVersion)`. When
`SDKVersion == D3D_SDK_VERSION` (220) it must return a valid `IDirect3D8*`.
For any other value it must return `NULL`.

**R-D3D8-1.2** `d3d8.dll` must export `ValidatePixelShader` and
`ValidateVertexShader` as stubs returning `D3D_OK`. These are called by some
DX8-era applications at startup.

---

## 2. IDirect3D8 Factory

**R-D3D8-2.1** `IDirect3D8::GetAdapterCount`, `GetAdapterIdentifier`,
`GetAdapterModeCount`, `EnumAdapterModes`, `GetAdapterDisplayMode`,
`GetAdapterMonitor` must delegate to the underlying `IDirect3D9` methods and
return identical data.

**R-D3D8-2.2** `IDirect3D8::CheckDeviceFormat`, `CheckDeviceType`,
`CheckDeviceMultiSampleType`, `CheckDepthStencilMatch`, `GetDeviceCaps` must
delegate to the corresponding `IDirect3D9` methods and filter the returned
`D3DCAPS8` to clamp shader version fields to `D3DVS_VERSION(1,1)` and
`D3DPS_VERSION(1,4)`. Applications must never observe capabilities beyond what
D3D8 supports.

**R-D3D8-2.3** `IDirect3D8::CreateDevice` must create an `IDirect3DDevice8`
wrapper backed by an `IDirect3DDevice9`. The `D3DPRESENT_PARAMETERS` argument
uses the D3D8 layout (no `Flags` field, field name
`FullScreen_PresentationInterval`); it must be translated to
`D3DPRESENT_PARAMETERS9` before passing to `IDirect3D9::CreateDevice`.
`D3DSWAPEFFECT_COPY_VSYNC` must be translated to `D3DSWAPEFFECT_COPY` with
`PresentationInterval = D3DPRESENT_INTERVAL_ONE`.

---

## 3. Shader Handle Table

D3D8 identifies vertex and pixel shaders by opaque `DWORD` handles. The D3D8
device wrapper must maintain two handle tables that map these `DWORD` values to
D3D9 COM objects.

**R-D3D8-3.1** `CreateVertexShader(pDeclaration, pFunction, pHandle, Usage)`
must:
1. Parse the D3D8 vertex declaration token stream (`D3DVSD_*` tokens) into a
   `D3DVERTEXELEMENT9` array (see section 4).
2. Call `IDirect3DDevice9::CreateVertexDeclaration` with that array; store the
   resulting `IDirect3DVertexDeclaration9*` in the vertex declaration table.
3. If `pFunction` is non-NULL, call `IDirect3DDevice9::CreateVertexShader` with
   the bytecode; store the resulting `IDirect3DVertexShader9*` in the vertex
   shader table.
4. Write a unique non-zero handle (≥ `D3DVS_MAXTYPES` = 16) to `*pHandle`.

**R-D3D8-3.2** `DeleteVertexShader(handle)` must release the stored
`IDirect3DVertexShader9*` and `IDirect3DVertexDeclaration9*` and remove the
handle from both tables.

**R-D3D8-3.3** `SetVertexShader(handle)`:
- If `handle < D3DVS_MAXTYPES` (16): treat it as an FVF code. Call
  `IDirect3DDevice9::SetFVF(handle)` and `SetVertexShader(NULL)`.
- Otherwise: look up the handle in both tables. Call
  `IDirect3DDevice9::SetVertexDeclaration(pDecl9)` and
  `IDirect3DDevice9::SetVertexShader(pVS9)`.

**R-D3D8-3.4** `GetVertexShader(pHandle)` must return the currently active
handle. If the active state was set as an FVF, return the FVF value directly.

**R-D3D8-3.5** `GetVertexShaderDeclaration(handle, pData, pSizeOfData)` must
return the original D3D8 declaration token stream that was passed to
`CreateVertexShader`. The raw token stream must be stored alongside the table
entry.

**R-D3D8-3.6** `GetVertexShaderFunction(handle, pData, pSizeOfData)` must
return the original shader bytecode. It must be stored alongside the table entry.
If the shader was declaration-only (NULL function), return an empty buffer.

**R-D3D8-3.7** `CreatePixelShader(pFunction, pHandle)` must call
`IDirect3DDevice9::CreatePixelShader` with the bytecode, store the resulting
`IDirect3DPixelShader9*` in the pixel shader table, and write a unique handle
to `*pHandle`.

**R-D3D8-3.8** `DeletePixelShader(handle)` must release the stored
`IDirect3DPixelShader9*` and remove the entry.

**R-D3D8-3.9** `SetPixelShader(handle)`:
- Handle 0 means no pixel shader: call `IDirect3DDevice9::SetPixelShader(NULL)`.
- Otherwise: look up and call `IDirect3DDevice9::SetPixelShader(pPS9)`.

**R-D3D8-3.10** `SetVertexShaderConstant(reg, pData, count)` and
`SetPixelShaderConstant(reg, pData, count)` must delegate to
`IDirect3DDevice9::SetVertexShaderConstantF` and `SetPixelShaderConstantF`
respectively. D3D8 constants are always float4 arrays.

---

## 4. Vertex Declaration Parser

**R-D3D8-4.1** The D3D8 vertex declaration token stream (`DWORD[]` terminated
by `0xFFFFFFFF`) must be parsed into a `D3DVERTEXELEMENT9[]` array.

**R-D3D8-4.2** `D3DVSD_STREAM(n)` tokens must set the active stream index for
subsequent `D3DVSD_REG` tokens and reset the running byte offset to zero.

**R-D3D8-4.3** `D3DVSD_REG(vsRegister, dataType)` tokens must produce one
`D3DVERTEXELEMENT9` entry:
- `Stream` = current stream index
- `Offset` = current running byte offset
- `Type` = `D3DVSDT_*` → `D3DDECLTYPE_*` (exact 1:1 mapping, same numeric values
  0–7)
- `Method` = `D3DDECLMETHOD_DEFAULT`
- `Usage` / `UsageIndex` = from `D3DVSDE_*` register mapping table (see
  research doc `docs/research/dx8-api.md` section "Vertex Shader Declaration")
- Advance running offset by the byte size of `dataType`.

**R-D3D8-4.4** `D3DVSD_SKIP(count)` tokens must advance the running byte offset
by `(count + 1) * sizeof(DWORD)` without emitting an element.

**R-D3D8-4.5** `D3DVSD_CONST(reg, count)` tokens must be ignored during
declaration parsing. Constant data embedded in declarations is not supported
by the D3D9 backend and is not used by any known D3D8 game title.

**R-D3D8-4.6** The parsed array must be terminated with `D3DDECL_END()`.

---

## 5. State Block Token Table

**R-D3D8-5.1** `CreateStateBlock(type, pToken)` must create an
`IDirect3DStateBlock9*` via `IDirect3DDevice9::CreateStateBlock`, store it in
a token table, and write a unique non-zero token to `*pToken`.

**R-D3D8-5.2** `BeginStateBlock()` must call
`IDirect3DDevice9::BeginStateBlock()`.

**R-D3D8-5.3** `EndStateBlock(pToken)` must call
`IDirect3DDevice9::EndStateBlock(&pSB9)`, store the result, and write a unique
token to `*pToken`.

**R-D3D8-5.4** `ApplyStateBlock(token)` must look up the token and call
`IDirect3DStateBlock9::Apply()`.

**R-D3D8-5.5** `CaptureStateBlock(token)` must look up the token and call
`IDirect3DStateBlock9::Capture()`.

**R-D3D8-5.6** `DeleteStateBlock(token)` must look up the token, call
`Release()` on the `IDirect3DStateBlock9*`, and remove the entry.

---

## 6. Render Target and Depth Stencil

**R-D3D8-6.1** `SetRenderTarget(pRenderTarget, pDepthStencil)` must call both
`IDirect3DDevice9::SetRenderTarget(0, pRT9)` and
`IDirect3DDevice9::SetDepthStencilSurface(pDS9)` in that order. Both arguments
may be NULL independently.

**R-D3D8-6.2** `GetRenderTarget(ppRenderTarget)` must return the surface at
render target index 0.

**R-D3D8-6.3** `GetDepthStencilSurface(ppZStencil)` must delegate directly to
`IDirect3DDevice9::GetDepthStencilSurface`.

---

## 7. Resource Creation Signature Differences

**R-D3D8-7.1** `CreateRenderTarget(w, h, Format, MultiSample, Lockable, ppSurface)`
must delegate to `IDirect3DDevice9::CreateRenderTarget(w, h, Format, MultiSample,
0, Lockable, ppSurface9, NULL)`. D3D8 has no `MultiSampleQuality` parameter;
inject 0.

**R-D3D8-7.2** `CreateDepthStencilSurface(w, h, Format, MultiSample, ppSurface)`
must delegate to `IDirect3DDevice9::CreateDepthStencilSurface(w, h, Format,
MultiSample, 0, FALSE, ppSurface9, NULL)`. Inject `MultiSampleQuality=0` and
`Discard=FALSE`.

**R-D3D8-7.3** `CreateImageSurface(w, h, Format, ppSurface)` must delegate to
`IDirect3DDevice9::CreateOffscreenPlainSurface(w, h, Format, D3DPOOL_SYSTEMMEM,
ppSurface9, NULL)`.

**R-D3D8-7.4** `CopyRects(pSrc, pSrcRects, numRects, pDst, pDstPts)` must call
`IDirect3DDevice9::UpdateSurface` once per rect, passing `pSrcRects[i]` and
`pDstPts[i]` (or NULL/NULL if the arrays are NULL). All rects must be processed
before returning.

---

## 8. Texture Stage State: Sampler State Remapping

**R-D3D8-8.1** `SetTextureStageState(stage, type, value)` must intercept the
ten TSS indices that became sampler states in D3D9 and redirect them to
`IDirect3DDevice9::SetSamplerState(stage, D3DSAMP_*, value)`:

| D3D8 D3DTSS index | D3D9 D3DSAMP |
|---|---|
| 13 (ADDRESSU) | D3DSAMP_ADDRESSU |
| 14 (ADDRESSV) | D3DSAMP_ADDRESSV |
| 15 (BORDERCOLOR) | D3DSAMP_BORDERCOLOR |
| 16 (MAGFILTER) | D3DSAMP_MAGFILTER |
| 17 (MINFILTER) | D3DSAMP_MINFILTER |
| 18 (MIPFILTER) | D3DSAMP_MIPFILTER |
| 19 (MIPMAPLODBIAS) | D3DSAMP_MIPMAPLODBIAS |
| 20 (MAXMIPLEVEL) | D3DSAMP_MAXMIPLEVEL |
| 21 (MAXANISOTROPY) | D3DSAMP_MAXANISOTROPY |
| 25 (ADDRESSW) | D3DSAMP_ADDRESSW |

All other TSS indices must be forwarded to
`IDirect3DDevice9::SetTextureStageState` unchanged.

**R-D3D8-8.2** `GetTextureStageState` must apply the same remapping in reverse,
calling `IDirect3DDevice9::GetSamplerState` for the ten redirected indices.

---

## 9. Present and Back Buffer

**R-D3D8-9.1** `IDirect3DDevice8::Present(pSrcRect, pDstRect, hDest, pDirtyRgn)`
must call `IDirect3DDevice9::Present(pSrcRect, pDstRect, hDest, pDirtyRgn)`.
D3D8 has no `dwFlags` parameter; pass 0.

**R-D3D8-9.2** `GetBackBuffer(BackBuffer, Type, ppSurface)` must call
`IDirect3DDevice9::GetBackBuffer(0, BackBuffer, Type, ppSurface9)`. D3D8 has
no swap chain index; always use index 0.

---

## 10. Capabilities Clamping

**R-D3D8-10.1** `GetDeviceCaps(D3DCAPS8*)` must populate all D3D8 capability
fields. Fields that do not exist in D3D8 (e.g. MRT count) must be omitted.
Shader version fields must be clamped: `VertexShaderVersion = D3DVS_VERSION(1,1)`,
`PixelShaderVersion = D3DPS_VERSION(1,4)`.

**R-D3D8-10.2** `NumSimultaneousRTs` is implicitly 1 in D3D8. The wrapper must
never expose MRT support.

---

## 11. Pass-through Methods

All remaining `IDirect3DDevice8` methods that have an exact equivalent in
`IDirect3DDevice9` must delegate with direct parameter forwarding:

`TestCooperativeLevel`, `GetAvailableTextureMem`, `GetDirect3D`,
`GetDisplayMode`, `GetCreationParameters`, `Reset`, `GetRasterStatus`,
`SetGammaRamp`, `GetGammaRamp`, `CreateTexture`, `CreateVolumeTexture`,
`CreateCubeTexture`, `CreateVertexBuffer`, `CreateIndexBuffer`,
`UpdateTexture`, `GetFrontBuffer`, `BeginScene`, `EndScene`, `Clear`,
`SetTransform`, `GetTransform`, `MultiplyTransform`, `SetViewport`,
`GetViewport`, `SetMaterial`, `GetMaterial`, `SetLight`, `GetLight`,
`LightEnable`, `GetLightEnable`, `SetClipPlane`, `GetClipPlane`,
`SetRenderState`, `GetRenderState`, `SetClipStatus`, `GetClipStatus`,
`GetTexture`, `SetTexture`, `ValidateDevice`, `SetPaletteEntries`,
`GetPaletteEntries`, `SetCurrentTexturePalette`, `GetCurrentTexturePalette`,
`DrawPrimitive`, `DrawIndexedPrimitive`, `DrawPrimitiveUP`,
`DrawIndexedPrimitiveUP`, `ProcessVertices`, `SetStreamSource`,
`GetStreamSource`, `SetIndices`, `GetIndices`.

**R-D3D8-11.1** Each pass-through method must translate the resource wrapper
types (e.g. `IDirect3DSurface8*` → `IDirect3DSurface9*`) by extracting the
underlying D3D9 interface from the D3D8 wrapper before the call.
