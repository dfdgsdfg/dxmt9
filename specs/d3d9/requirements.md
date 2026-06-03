# D3D9 Layer Requirements

The D3D9 layer is the API frontend for applications running under Wine. It
exposes the D3D9 COM interface surface and enforces Windows D3D9-compatible
behaviour before handing work to the Metal backend.

The implementation structure remains DXMT-compatible: PE frontend and
`winemetal` bridge/provider split, chunked command submission, and Metal work on
the unix side. Wine `dlls/d3d9/tests` are used as an oracle for public Windows
D3D9 API behaviour; they do not require dxmt9 to copy Wine's `dlls/d3d9` or
wined3d internal architecture.

Detailed subtopic contracts live in:

- `caps/requirements.md` for `D3DCAPS9` and capability consistency.
- `formats/requirements.md` for `D3DFORMAT` classification and validation.
- `queries/requirements.md` for `IDirect3DQuery9` behaviour and ordering.
- `wsi/requirements.md` for HWND, swap-chain, presentation, and no-window WSI.
- `ir/requirements.md` for shader IR, analysis passes, MSL emission, FFP key
  semantics, half-pixel and alpha-test rewrite contracts, and precision /
  VSOut policy (`R-CORE-IR-*`).

---

## 1. Factory and Adapter

**R-CORE-1.1** `Direct3DCreate9(SDKVersion)` must return a valid `IDirect3D9*` when
`SDKVersion == D3D_SDK_VERSION`, and must return `NULL` for any other value.

**R-CORE-1.2** The factory must enumerate exactly the physical GPU adapters present in
the system. Adapter ordering must be stable across calls within a process lifetime.

**R-CORE-1.3** `IDirect3D9::GetDeviceCaps()` must return a `D3DCAPS9` structure that
accurately reflects the capabilities of the underlying Metal device. Fields that have
no Metal equivalent must report the D3D9 minimum guarantee or the most restrictive
safe value — never a capability the backend cannot actually satisfy.

**R-CORE-1.4** `IDirect3D9::CheckDeviceFormat()` must return `D3D_OK` only for format
and usage combinations the Metal backend can support. It must return
`D3DERR_NOTAVAILABLE` for unsupported combinations. It must never silently lie.

**R-CORE-1.5** `IDirect3D9::CreateDevice()` must reject `D3DDEVTYPE_REF` and
`D3DDEVTYPE_NULLREF`. Only `D3DDEVTYPE_HAL` is supported.

**R-CORE-1.6** A factory created by `Direct3DCreate9()` must expose
`IUnknown` and `IDirect3D9`, but `QueryInterface(IID_IDirect3D9Ex)` must fail
with `E_NOINTERFACE` and set the output to `NULL`. A factory created by
`Direct3DCreate9Ex()` must expose both `IDirect3D9` and `IDirect3D9Ex`.

**R-CORE-1.7** Factory validation methods must respect the caller-provided
`D3DDEVTYPE`, adapter format, back-buffer format, usage, and resource type. The
implementation must not silently coerce unsupported device types to
`D3DDEVTYPE_HAL` or ignore format parameters when reporting support.

**R-CORE-1.8** Display-mode methods must expose Windows D3D9-compatible adapter
mode formats, validated by Wine D3D9 tests: `D3DFMT_X8R8G8B8` and
`D3DFMT_R5G6B5`. `D3DFMT_A8R8G8B8` is a required resource/back-buffer format,
but it must not be reported as an adapter mode format. If the internal backend
current mode or swap-chain back buffer uses `A8R8G8B8`,
`GetAdapterDisplayMode()`, `GetAdapterDisplayModeEx()`,
`IDirect3DDevice9::GetDisplayMode()`, and
`IDirect3DSwapChain9::GetDisplayMode()` must report `X8R8G8B8`.

**R-CORE-1.8.1** Ex display-mode methods must validate structure sizes and
filter fields according to D3D9Ex behaviour. `EnumAdapterModesEx()` must reject
an invalid `D3DDISPLAYMODEEX::Size` and must not return modes that fail the
provided `D3DDISPLAYMODEFILTER`.

**R-CORE-1.9** Factory validation return codes must match Windows D3D9-observed
behaviour, using Wine D3D9 tests as the portable oracle:

- invalid adapter index: `D3DERR_INVALIDCALL`;
- valid but unavailable `D3DDEVTYPE` values such as `REF`, `NULLREF`, or `SW`:
  `D3DERR_NOTAVAILABLE`;
- invalid non-D3D9 `D3DDEVTYPE` enum values: `D3DERR_INVALIDCALL`;
- fullscreen `CheckDeviceType()` display format other than `D3DFMT_X8R8G8B8`
  or `D3DFMT_R5G6B5`: `D3DERR_NOTAVAILABLE`;
- `CheckDeviceFormat()` adapter format must be one of `D3DFMT_X8R8G8B8`,
  `D3DFMT_R5G6B5`, or `D3DFMT_X1R5G5B5`; `D3DFMT_UNKNOWN` returns
  `D3DERR_INVALIDCALL`, other invalid non-zero formats return
  `D3DERR_NOTAVAILABLE`;
- unsupported `D3DRESOURCETYPE` values return `D3DERR_INVALIDCALL`.

**R-CORE-1.10** `CheckDeviceFormat()` must first apply the D3D9 front-end
validation above, then query backend capability for the requested
`CheckFormat`, `Usage`, and resource type. It must map texture/surface/cube
types, volume types, and vertex/index buffers as distinct resource classes, and
must not answer only from the raw format.

**R-CORE-1.11** `CheckDeviceMultiSampleType()` must preserve Windows
D3D9-compatible front-end semantics, validated by Wine D3D9 tests, in addition
to backend capability checks. Invalid adapter indices return
`D3DERR_INVALIDCALL`; invalid multisample enum values return
`D3DERR_INVALIDCALL`; `D3DMULTISAMPLE_NONE` succeeds for valid displayable
formats and reports one quality level when `pQualityLevels` is non-null.
Unsupported but well-formed multisample requests return `D3DERR_NOTAVAILABLE`
and must follow the Wine-test-observed `pQualityLevels` write behaviour for that
failure path.

**R-CORE-1.12** `CheckDeviceFormatConversion()` must validate adapter and
device type like the other factory checks. If source and destination formats
are identical, it must return `D3D_OK`. Unsupported conversions return
`D3DERR_NOTAVAILABLE`; invalid adapters return `D3DERR_INVALIDCALL`.

**R-CORE-1.13** The PE `d3d9.dll` entry-point surface is part of Windows D3D9
API compatibility. Both app-local and Wine-builtin variants must export the
D3D9 factory entry points, `Direct3DShaderValidatorCreate9`, D3DPERF helpers
observed in Wine/Windows export profiles (`D3DPERF_BeginEvent`,
`D3DPERF_EndEvent`, `D3DPERF_GetStatus`,
`D3DPERF_QueryRepeatFrame`, `D3DPERF_SetMarker`, `D3DPERF_SetOptions`,
`D3DPERF_SetRegion`), `DebugSetMute`, and a loader-safe
`Direct3DCreate9On12` stub. Unsupported auxiliary exports may be no-op stubs,
but they must not be missing from the PE export table.

**R-CORE-1.14** `Direct3DShaderValidatorCreate9()` may be a non-validating
stub, but it must return a stable, callable validator object. The validator's
`QueryInterface()` must fail unsupported interfaces with `E_NOINTERFACE` and a
cleared output pointer; `AddRef()` / `Release()` must be safe on the singleton;
`Begin()`, `Instruction()`, and `End()` must be callable and return a
Wine/Windows-compatible success or validation failure without dereferencing
invalid optional state.

**R-CORE-1.15** `Direct3DCreate9On12()` is required as a loader-compatibility
entry point, not as a D3D12 interop implementation. With unsupported or disabled
9On12 arguments it must return an ordinary Ex-capable D3D9 factory or a clean
`NULL` factory result on initialization failure. If dxmt9 exposes
`IDirect3DDevice9On12`, the interface must be query-safe: unsupported D3D12
device queries return `E_NOINTERFACE` with a cleared output pointer, null output
arguments return `E_INVALIDARG`, and unwrap/return methods fail without
touching Metal backend state.

---

## 2. Device Lifecycle

**R-CORE-2.1** The device must implement `IDirect3DDevice9`. `IDirect3DDevice9Ex`
is also required; see section 10.

**R-CORE-2.2** `TestCooperativeLevel()` must return `D3D_OK` while the device is
operational. Device-lost state (`D3DERR_DEVICELOST`, `D3DERR_DEVICENOTRESET`) is only
required when a GPU reset or mode change makes the device inoperable.

**R-CORE-2.3** `Reset()` must release all resources in `D3DPOOL_DEFAULT` and rebuild
the implicit swap chain. Resources in `D3DPOOL_MANAGED`, `D3DPOOL_SYSTEMMEM`, and
`D3DPOOL_SCRATCH` must survive `Reset()` intact.

**R-CORE-2.4** The device must track a single active swap chain created from
`D3DPRESENT_PARAMETERS` at `CreateDevice()` time. `CreateAdditionalSwapChain()` must
be supported for windowed multi-window scenarios.

**R-CORE-2.5** Device-lost and reset behaviour must distinguish base D3D9 from
D3D9Ex. Base devices may report `D3DERR_DEVICELOST` /
`D3DERR_DEVICENOTRESET` through `TestCooperativeLevel()`; Ex devices must keep
`TestCooperativeLevel()` successful and surface window/device status through
`CheckDeviceState()`.

**R-CORE-2.6** `CreateDevice()`, `CreateDeviceEx()`, `Reset()`, and `ResetEx()`
must validate `D3DPRESENT_PARAMETERS` using Windows D3D9-compatible rules
validated by Wine D3D9 tests before creating or rebuilding the swap chain:

- `SwapEffect` must be non-zero and no greater than `D3DSWAPEFFECT_COPY` for
  base D3D9, or `D3DSWAPEFFECT_FLIPEX` for D3D9Ex;
- `BackBufferCount == 0` is accepted and normalized to one buffer after
  validation;
- base D3D9 accepts at most three back buffers; D3D9Ex accepts at most thirty;
- `D3DSWAPEFFECT_COPY` accepts at most one back buffer;
- `PresentationInterval` must be `DEFAULT`, `ONE`, `TWO`, `THREE`, `FOUR`, or
  `IMMEDIATE`;
- successful creation/reset must write the normalized presentation parameters
  back to the caller-visible `D3DPRESENT_PARAMETERS`, including derived
  back-buffer width/height, normalized back-buffer count, and resolved
  back-buffer format.

Invalid presentation parameters must fail with `D3DERR_INVALIDCALL`; allocation
or backend failures must preserve their original `HRESULT`.

**R-CORE-2.7** `ResetEx(pPresentationParameters, pFullscreenDisplayMode)` and
`CreateDeviceEx(..., pFullscreenDisplayMode, ...)` must accept a fullscreen
display mode if and only if `pPresentationParameters->Windowed == FALSE`. When
provided, `D3DDISPLAYMODEEX::Size` must be valid and the mode width/height must
match the back-buffer width/height. Mismatches return `D3DERR_INVALIDCALL`.

**R-CORE-2.8** Successful `Reset()` / `ResetEx()` must restore Windows
D3D9-compatible device state. Render target slot 0 becomes the implicit back
buffer, higher render target slots become unbound, the depth-stencil binding
follows the new auto-depth-stencil state, scene recording is cleared, and
viewport/scissor state is reset to the new back-buffer dimensions. Failed
base-device reset may place the device into `D3DERR_DEVICENOTRESET` /
lost-device state; Ex reset failures must preserve Ex cooperative-level
semantics and report status through
`CheckDeviceState()`.

---

## 3. Device State Machine

**R-CORE-3.1** The device must maintain a complete shadow of all D3D9 mutable state:
render states (`D3DRS_*`), texture stage states (`D3DTSS_*`), sampler states
(`D3DSAMP_*`), transform matrices, lights, material, viewport, scissor rect, stream
sources, index buffer, vertex declaration, shaders, shader constants, and render
targets.

**R-CORE-3.2** `GetRenderState` / `GetTextureStageState` / `GetSamplerState` must
return the last value set by the corresponding `Set*` call, regardless of whether
that value was applied to the GPU. The shadow must be authoritative.

**R-CORE-3.3** State changes made between `BeginScene()` and `EndScene()` must take
effect no later than the next draw call. State changes outside a scene may be deferred
until `BeginScene()`.

**R-CORE-3.4** `BeginScene()` must succeed when called once per frame. Nested
`BeginScene()` calls must return `D3DERR_INVALIDCALL`. `EndScene()` without a matching
`BeginScene()` must return `D3DERR_INVALIDCALL`.

**R-CORE-3.5** `BeginScene()` and `EndScene()` are permitted to be no-ops from the
GPU's perspective. Applications that omit them must still have their draw calls
submitted correctly.

**R-CORE-3.6** `IDirect3DStateBlock9` must capture and restore the full device state
as documented for `D3DSBT_ALL`, `D3DSBT_PIXELSTATE`, and `D3DSBT_VERTEXSTATE`.
`Apply()` must restore the captured state atomically from the application's viewpoint.

**R-CORE-3.7** State block creation must honor the requested type:
`D3DSBT_ALL`, `D3DSBT_PIXELSTATE`, and `D3DSBT_VERTEXSTATE` capture distinct
D3D9 state subsets, and unsupported types must return `D3DERR_INVALIDCALL`.
`CreateStateBlock(D3DSBT_ALL)` and a state block produced by
`BeginStateBlock()` / `EndStateBlock()` must follow the D3D9 compatibility
quirks validated by Wine's `dlls/d3d9/tests/stateblock.c` suite.

**R-CORE-3.8** While the device is recording a state block, nested
`BeginStateBlock()`, `IDirect3DStateBlock9::Capture()`, and
`IDirect3DStateBlock9::Apply()` must fail with `D3DERR_INVALIDCALL`.
`EndStateBlock()` without a matching active recording must also fail with
`D3DERR_INVALIDCALL`.

**R-CORE-3.9** The per-texture-coordinate wrap render states
`D3DRS_WRAP0`..`D3DRS_WRAP15` must be accepted, shadowed, and
stateblock-tracked like any other render state, so `GetRenderState`
round-trips them per R-CORE-3.2. Their default value (`0`, all
`D3DWRAPCOORD_*` bits clear) disables cylindrical texture-coordinate
wrapping and renders correctly. dxmt9 has no faithful Metal equivalent
for *enabled* cylindrical-wrap interpolation — a rasterizer-level
shortest-path wrap of texture coordinates across a primitive — so a
non-zero `D3DRS_WRAPn` is a **documented no-op**: geometry renders
without wrapping and may show seams on the rare titles that rely on it.
This is an accepted limitation, not a silent state drop; the value
remains visible through `GetRenderState`. (Same class of Metal-capability
limitation as the single-`[[clip_distance]]` clip-plane collapse.)

---

## 4. Resource Lifetime and Ownership

**R-CORE-4.1** All COM objects returned from the device must implement correct
`AddRef` / `Release` reference counting. An object must not be destroyed until its
reference count reaches zero.

**R-CORE-4.2** Resources must track their owning device. `GetDevice()` on any resource
must return that device with an incremented reference count.

**R-CORE-4.3** Resources created with `D3DPOOL_DEFAULT` must be destroyed when
`Reset()` is called or the device is destroyed, whichever comes first.

**R-CORE-4.4** Resources created with `D3DPOOL_MANAGED` must maintain a system-memory
backup. The GPU copy may be invalidated and re-uploaded transparently; the
application-visible contents must not change as a result.

**R-CORE-4.5** `Lock()` on a vertex buffer or index buffer must return a CPU-writable
pointer. `Unlock()` must make those writes visible to subsequent GPU draws. The lock
semantics of `D3DLOCK_DISCARD` and `D3DLOCK_NOOVERWRITE` must be respected: DISCARD
allows the implementation to return a fresh allocation; NOOVERWRITE guarantees the
application will not overwrite in-use regions.

**R-CORE-4.6** `Lock()` on a texture surface must return a CPU-accessible pointer with
the correct pitch. Modifications made before `Unlock()` must be visible in subsequent
texture samples.

**R-CORE-4.7** `IDirect3DSurface9::GetContainer()` must return the owning texture when
the surface is a mip level, and the device when it is a standalone render target.

**R-CORE-4.8** Public COM reference counts must match D3D9 compatibility
behaviour, not merely internal lifetime needs. `Get*` methods that return COM
objects must add a public reference. State setters and generated helper objects
must follow the observed D3D9/Wine rules, including the `SetVertexDeclaration()`
no-public-AddRef behaviour and stable cached FVF-generated declarations.

**R-CORE-4.9** Implicit swap-chain, back-buffer, depth-stencil, texture-level
surface, and device-child lifetimes must match Wine's D3D9 reference-count
tests. The implementation may hold private backend handles for deferred
execution, but those private retains must not change application-visible COM
reference counts.

**R-CORE-4.10** Private data behaviour must be implemented by one common helper
used by surfaces, textures, cube textures, volume textures, volumes, vertex
buffers, and index buffers. `D3DSPD_IUNKNOWN` requires `SizeOfData ==
sizeof(IUnknown*)`; invalid sizes return `D3DERR_INVALIDCALL` and must not
replace existing data. `GetPrivateData()` must return `D3DERR_MOREDATA` with
the required size when the caller buffer is too small, AddRef stored
`IUnknown` values on successful retrieval, and leave the caller's size value
unchanged when the GUID is not found.

**R-CORE-4.11** Shared-handle and user-memory parameters must follow Windows
D3D9-compatible behaviour validated by Wine D3D9 tests even when resource
sharing is not implemented. A non-null
`pSharedHandle` on a device created from `Direct3DCreate9()` returns
`E_NOTIMPL`. On Ex-capable devices, unsupported shared resources must fail with
the Windows D3D9-compatible error for the resource class and pool rather than
silently ignoring the handle.

**R-CORE-4.12** D3D9Ex user-memory resources are supported only for the
Wine-test-observed Windows D3D9 `D3DPOOL_SYSTEMMEM` cases. `CreateTexture()` with
`pSharedHandle != NULL`, `D3DPOOL_SYSTEMMEM`, and exactly one mip level must
bind the caller-supplied memory so `LockRect()` returns the same pointer and a
Windows D3D9-compatible pitch. `CreateOffscreenPlainSurface()` with
`D3DPOOL_SYSTEMMEM` and `pSharedHandle != NULL` follows the same ownership
model. Invalid level counts, scratch pool, cube/volume textures, and
vertex/index buffers must return the Windows D3D9-compatible
`D3DERR_INVALIDCALL` or `D3DERR_NOTAVAILABLE` for that resource class.

---

## 5. Draw Calls

**R-CORE-5.1** `DrawPrimitive` and `DrawIndexedPrimitive` must submit geometry using
the currently bound vertex declaration (or FVF), stream sources, index buffer,
shaders, and all active render/texture/sampler state at the time of the call.

**R-CORE-5.2** `DrawPrimitiveUP` and `DrawIndexedPrimitiveUP` must copy caller-owned
vertex and index data into GPU-accessible memory before issuing the draw. The caller's
buffer may be freed immediately after the call returns.

**R-CORE-5.3** `D3DPT_TRIANGLEFAN` must be supported. It has no Metal equivalent and
must be decomposed into `D3DPT_TRIANGLELIST` before submission.

**R-CORE-5.4** `Clear()` must support independent clearing of color, depth, and
stencil. It must support rectangular sub-region clears via the `pRects` / `Count`
parameters.

**R-CORE-5.5** `StretchRect()` must be supported for surface-to-surface copies,
including copies to and from render targets, with and without scaling. The `Filter`
parameter (`D3DTEXF_NONE`, `D3DTEXF_POINT`, `D3DTEXF_LINEAR`) must be respected.

**R-CORE-5.6** `UpdateSurface()` must be supported for copying from a `D3DPOOL_SYSTEMMEM`
surface to a `D3DPOOL_DEFAULT` surface of a compatible format.

**R-CORE-5.7** `UpdateTexture()` must be supported for uploading all mip levels of a
`D3DPOOL_SYSTEMMEM` texture to a `D3DPOOL_DEFAULT` texture.

**R-CORE-5.8** `GetRenderTargetData()` must be supported for reading back a render
target into a `D3DPOOL_SYSTEMMEM` or `D3DPOOL_SCRATCH` surface. This operation is
permitted to stall the CPU until GPU completion.

**R-CORE-5.9** `ColorFill()` must fill a render target surface or plain surface with
a solid color. Partial-rectangle fills must be supported.

**R-CORE-5.10** Ordinary `DrawIndexedPrimitive` calls must remain indexed through the
default production path and use the active index buffer or copied UP index payload.
Expansion into a non-indexed transient vertex stream is permitted only for explicit
diagnostics under `DXMT_FORCE_EXPAND_INDEXED=1` or required primitive conversion
such as `D3DPT_TRIANGLEFAN` decomposition.

---

## 6. Shaders

**R-CORE-6.1** `CreateVertexShader()` and `CreatePixelShader()` must accept D3D9
shader bytecode for all shader models the device reports as supported in `D3DCAPS9`.
The target shader model coverage is **SM 1.x through SM 3.0** (vs_1_1, ps_1_1
through ps_1_4, vs_2_0, ps_2_0, vs_2_x, ps_2_x / ps_2_a / ps_2_b, vs_3_0, ps_3_0).
SM 1.x must be supported because many DX8-era games use ps_1_4 / vs_1_1 and will not
accept a device that reports only SM 2.0.

**R-CORE-6.2** `SetVertexShaderConstantF/I/B` and `SetPixelShaderConstantF/I/B` must
store constants into the device shadow. Their values must be visible to shaders
dispatched by subsequent draw calls.

**R-CORE-6.3** When no vertex shader is set (`SetVertexShader(NULL)`), the fixed-
function vertex pipeline must be active. Its behavior must be governed by the
transform state, lighting state, material, active lights, FVF, and fog parameters.

**R-CORE-6.4** When no pixel shader is set (`SetPixelShader(NULL)`), the fixed-
function texture-and-lighting blending pipeline must be active. Its behavior must be
governed by the texture stage state chain (`D3DTSS_COLOROP`, `D3DTSS_ALPHAOP`, and
related arguments).

**R-CORE-6.5** Translated programmable pixel shaders must preserve D3D texture
coordinates by default. `texld` / `TEX` and related sampling instructions must
not globally invert the V coordinate in the pixel shader. Texture orientation
differences between D3D and Metal must be handled by resource upload/readback,
surface addressing, and vertex/raster coordinate mapping rather than by a
default pixel-shader V flip.

**R-CORE-6.6** `DXMT_DEBUG_FORCE_PIXEL_V_FLIP` is a diagnostic source-contract
override only. When set, translated programmable pixel shaders may emit an
explicit `1.0f - v` coordinate transform to bisect texture-orientation bugs.
This flag must be off by default, must not be required for correct rendering,
and must remain independent from vertex clip-space Y debugging.

**R-CORE-6.7** Programmable shader decode, validation/lowering, IR
normalization, and Metal Shading Language generation must be deterministic,
stateless functions over explicit bytecode, capability, fixed-function key, and
translation-option inputs. They must not read mutable `DeviceState`, COM object
identity, backend caches, or global debug state except through explicit
parameters, and their outputs must be unit-testable as values: IR, MSL text,
resource binding metadata, diagnostics, or errors.

---

## 7. Vertex Format

**R-CORE-7.1** Both `SetFVF()` (legacy flexible vertex format) and
`SetVertexDeclaration()` (explicit `D3DVERTEXELEMENT9` array) must be supported and
interoperable. Setting one must not require the other to be cleared.

**R-CORE-7.2** The `D3DFVF_XYZRHW` flag (pre-transformed, screen-space vertex) must
be handled. The backend must convert screen-space coordinates to Metal NDC, accounting
for D3D9's half-pixel offset convention.

**R-CORE-7.3** Multi-stream vertex binding (up to the device-reported
`MaxStreams` capability) must be supported.

**R-CORE-7.4** FVF and vertex declaration interop must match the Wine
`device.c` tests. Setting an FVF must create or reuse a stable generated vertex
declaration for equivalent FVF layouts; `GetVertexDeclaration()` must return the
same cached declaration object for repeated equivalent FVF conversions; switching
between explicit declarations and FVF must preserve the public refcount
behaviour observed by Wine.

**R-CORE-7.5** Vertex clip-space Y correction is a vertex/raster contract, not a
pixel texture-coordinate contract. The default programmable vertex-shader
translation must not inject a global debug Y inversion. `DXMT_DEBUG_FLIP_VERTEX_Y`
may force `out.position.y = -out.position.y` only as a diagnostic bisect path;
it must be controlled separately from `DXMT_DEBUG_FORCE_PIXEL_V_FLIP`.

---

## 8. Queries

**R-CORE-8.1** `D3DQUERYTYPE_EVENT` (GPU completion fence) must be supported.
`Issue(D3DISSUE_END)` followed by `GetData()` must correctly indicate whether the GPU
has completed all prior commands.

**R-CORE-8.2** `D3DQUERYTYPE_OCCLUSION` must be supported if the Metal device supports
visibility result buffers. The returned sample count is an approximation and is
permitted to be clamped to a boolean (non-zero = visible).

**R-CORE-8.3** Query object validation must follow Windows D3D9-visible
behaviour validated by Wine D3D9 tests. Invalid query types return
`D3DERR_NOTAVAILABLE`; `GetDataSize()` must match the type-specific public size,
including `sizeof(DWORD)` for occlusion and `sizeof(BOOL)` for
timestamp-disjoint; `CreateQuery(type, NULL)` must act as a support probe
without returning an object; failed query creation must preserve the caller's
out pointer when the Wine behavioural oracle observes preservation; pre-issue
`GetData()` and backend-unavailable query data must write deterministic
Windows-compatible bytes for the requested public data range and must not
overrun short caller buffers or leak uninitialised memory.

---

## 9. Error Handling

**R-CORE-9.1** All API calls must return documented `HRESULT` codes. `D3D_OK`
(zero) on success, `D3DERR_INVALIDCALL` for bad arguments or illegal state
transitions, and `D3DERR_OUTOFVIDEOMEMORY` / `E_OUTOFMEMORY` for allocation failures.

**R-CORE-9.2** Null pointer arguments to methods that document them as invalid must
return `D3DERR_INVALIDCALL`, not crash.

**R-CORE-9.3** The device must never silently discard draw calls or state changes.
Either the operation succeeds or it returns an error.

**R-CORE-9.4** The PE/unix bridge ABI must preserve `HRESULT` failure causes.
Object creation entry points must return a status and an out handle, or an
equivalent structure, so validation, allocation, provider-load, and backend
failures are not collapsed to a generic `NULL` object /
`D3DERR_INVALIDCALL` result.

**R-CORE-9.5** dxmt9 intentionally prefers clean failure over reproducing
Windows access violations for invalid pointers. Wine tests that only validate a
process crash on invalid input are not required conformance targets; documented
optional pointers and observable HRESULT/refcount behaviour remain required.

---

## 10. IDirect3DDevice9Ex

The Ex interface is required for applications that probe `Direct3DCreate9Ex` or
call `ResetEx` / `PresentEx`. It extends the base device and factory interfaces
without changing existing semantics.

**R-CORE-10.1** `Direct3DCreate9Ex(SDKVersion, ppD3D)` must return a valid
`IDirect3D9Ex*` when `SDKVersion == D3D_SDK_VERSION`. `IDirect3D9Ex` inherits
all `IDirect3D9` methods; the existing factory implementation must be reused.

**R-CORE-10.2** `IDirect3D9Ex::GetAdapterModeCountEx(Adapter, pFilter)` must
return the count of display modes matching `pFilter`. `pFilter` must be
validated before use; if dxmt9 intentionally accepts a `NULL` filter as a clean
extension, that behaviour must be documented and covered by a dxmt9-specific
test rather than attributed to Wine. A `D3DFMT_UNKNOWN` filter format matches
all supported D3D9 adapter mode formats. The underlying mode list is the same
as `EnumAdapterModes()` and must not include `D3DFMT_A8R8G8B8`.

**R-CORE-10.3** `IDirect3D9Ex::EnumAdapterModesEx(Adapter, pFilter, Mode, pMode)`
must fill a `D3DDISPLAYMODEEX` for the matching mode.
`D3DDISPLAYMODEEX::ScanLineOrdering` must report the mode's observable scanline
ordering when known; otherwise it may report
`D3DSCANLINEORDERING_PROGRESSIVE`. Tests must not require a hardcoded value
when the host display reports a different valid ordering.

**R-CORE-10.4** `IDirect3D9Ex::GetAdapterDisplayModeEx(Adapter, pMode, pRotation)`
must return the current display mode as `D3DDISPLAYMODEEX`.
When `pRotation` is non-null, `*pRotation` must reflect the host display
rotation if available; otherwise it may report
`D3DDISPLAYROTATION_IDENTITY`. A null `pRotation` is valid.

**R-CORE-10.5** `IDirect3D9Ex::GetAdapterLUID(Adapter, pLUID)` must return a
non-zero `LUID` that is stable for the adapter within the process lifetime.
It may be synthesised from the Metal device registry ID.

**R-CORE-10.6** `IDirect3D9Ex::CreateDeviceEx()` must create an
`IDirect3DDevice9Ex`. The additional `D3DDISPLAYMODEEX*` parameter overrides
the fullscreen display mode; if `NULL`, behaviour is identical to `CreateDevice()`.
Validation of the fullscreen mode follows R-CORE-2.7.

**R-CORE-10.7** `IDirect3DDevice9Ex` inherits all `IDirect3DDevice9` methods.
The existing device implementation must be reused where behaviour is identical;
Ex-only validation and status reporting remain explicit at the PE COM boundary.

**R-CORE-10.8** `CheckDeviceState(hDestinationWindow)` must return `D3D_OK`
while the device is operational, `S_PRESENT_OCCLUDED` when the window is
minimised or occluded, and `D3DERR_DEVICELOST` when the device is lost. It
replaces `TestCooperativeLevel()` for Ex applications.

**R-CORE-10.9** `ResetEx(pPresentationParameters, pFullscreenDisplayMode)` must
behave identically to `Reset()` for `D3DPOOL_DEFAULT` invalidation and swap-chain
rebuild. When `pFullscreenDisplayMode` is non-`NULL` it must be forwarded through
`normalizePresentParameters()` after the R-CORE-2.7 windowed/fullscreen and
size/mode checks pass.

**R-CORE-10.10** `PresentEx(pSourceRect, pDestRect, hDestWindowOverride,
pDirtyRegion, dwFlags)` must present the swap chain. `pSourceRect`,
`pDestRect`, `pDirtyRegion`, and `dwFlags` are hints and may be ignored; the
full back buffer must always be presented.

**R-CORE-10.11** `SetMaximumFrameLatency(MaxLatency)` must accept values in the
range `0..30`. A value of `0` selects the implementation default of `4`; values
greater than `30` must return `D3DERR_INVALIDCALL`.
`GetMaximumFrameLatency()` must return the current effective value.

**R-CORE-10.12** `WaitForVBlank(SwapChainIndex)` must return `D3D_OK`. A
best-effort vblank wait is acceptable; a busy-sleep no-op is not.

**R-CORE-10.13** `CheckResourceResidency(pResourceArray, NumResources)` must
return `S_OK`. Metal manages residency transparently.

**R-CORE-10.14** `GetGPUThreadPriority(pPriority)` must return `D3D_OK` with
`*pPriority = 0`. `SetGPUThreadPriority(Priority)` must return `D3D_OK` and
ignore `Priority`. Metal does not expose GPU thread priority.

**R-CORE-10.15** `SetConvolutionMonoKernel()` and `ComposeRects()` must return
`E_NOTIMPL`.

**R-CORE-10.16** `CreateRenderTargetEx()` and
`CreateDepthStencilSurfaceEx()` must validate the Ex-only `Usage` argument
before delegating. `Usage` must not contain `D3DUSAGE_RENDERTARGET` or
`D3DUSAGE_DEPTHSTENCIL`; invalid usage returns `D3DERR_INVALIDCALL`.
`CreateOffscreenPlainSurfaceEx()` may return `E_NOTIMPL` until implemented; if
implemented, it must apply the same usage and shared-handle policy before
delegating. `pSharedHandle` handling follows R-CORE-4.11.

**R-CORE-10.17** Ex interface exposure is inherited from the creating factory,
matching Windows D3D9 behaviour captured by Wine D3D9 tests. A device created
from a `Direct3DCreate9()` factory must return `E_NOINTERFACE` for
`QueryInterface(IID_IDirect3DDevice9Ex)`. A device created from a
`Direct3DCreate9Ex()` factory must expose
`IDirect3DDevice9Ex`, even if it was created through the inherited
`IDirect3D9::CreateDevice()` method.

**R-CORE-10.18** D3D9Ex swap-chain exposure must follow the same rule:
swap chains produced by an Ex-created device may expose
`IDirect3DSwapChain9Ex`; swap chains produced by a base-created device must not.

---

## 11. Hot-Path Command Recording and Frame Pacing

The core must follow the upstream DXMT performance shape: D3D API calls record work
on the PE side, while unix-side Metal execution happens from committed command
chunks.

**R-CORE-11.1** D3D9 hot-path calls must not cross the Wine PE/unix boundary one
call at a time. `Set*`, `Draw*`, `Clear`, and ordinary surface operations must be
handled by PE-side state tracking and command recording.

**R-CORE-11.2** `SetRenderState`, `SetTextureStageState`, `SetSamplerState`,
`SetTexture`, `SetStreamSource`, `SetIndices`, `SetFVF`, `SetVertexDeclaration`,
`SetVertexShader`, `SetPixelShader`, shader constant setters, transform setters,
and render-target binding calls must update the core shadow state and dirty bits.
They must not emit a backend bridge call solely because state changed.

**R-CORE-11.3** Each recorded draw must resolve to a complete effective state before
encoding. The canonical bridge packet may carry only a state delta plus draw
payload, but ordered replay against the server-side shadow must produce the same
effective state the D3D9 device had at record time. Later state changes must not
affect an already recorded draw. The recorded draw owns this effective state by
record ordering and replay semantics; it is not required to contain a full
self-contained state snapshot in the canonical packet.

**R-CORE-11.4** Recorded command chunks must contain only POD command records and
opaque backend handles. They must not contain D3D9 COM pointers, Objective-C object
pointers, unix-side C++ object pointers, or lambdas.

**R-CORE-11.5** The command recorder must commit the current chunk when any of the
following occurs: `Present` / `PresentEx`, synchronous readback, event query
ordering, explicit flush, chunk command-count limit, chunk byte-size limit, or a
resource lifetime hazard that cannot be represented inside the current chunk.

**R-CORE-11.6** Command-count and byte-size limits must be configurable and must
have bounded defaults. The defaults must prevent unbounded draw-only chunks from
creating multi-frame tail latency even when the application does not call `Present`.

**R-CORE-11.7** `Present` / `PresentEx` must obtain a frame token from the backend
queue or presenter for the committed present-bearing chunk. The core must not pace
frames by waiting on an encode-dequeued sequence ID.

**R-CORE-11.8** `SetMaximumFrameLatency(MaxLatency)` must configure the maximum
number of present-bearing frame tokens that may remain incomplete. The wait target
is GPU/presenter completion of an older frame token, not backend encode progress.

**R-CORE-11.9** A `MaxLatency` value of 1 must mean the application can be at most
one present-bearing frame ahead of queue/presenter completion. It must not force a
full device drain after every draw or state call.

**R-CORE-11.10** D3D9 getters and state blocks must read and write the PE-side
shadow state. `GetRenderState`, `GetTexture`, `GetStreamSource`, and
`IDirect3DStateBlock9::Capture/Apply` must not require a backend round trip solely
to observe ordinary mutable state.

**R-CORE-11.11** `DrawPrimitiveUP` and `DrawIndexedPrimitiveUP` must copy
caller-owned vertex/index memory into recorder-owned chunk payload or staging memory
before returning to the application. Recorded UP draws must never retain pointers to
application memory.

**R-CORE-11.12** Resource handles referenced by a recorded chunk must remain valid
until the backend reports completion for that chunk. Application `Release()` calls may
drop COM references, but they must not invalidate handles already captured by
in-flight command records.

**R-CORE-11.13** Developer builds must expose diagnostics that count PE/unix bridge
operations by class: committed chunks, coarse resource operations, frame-token waits,
and any compatibility per-call fallback. This is required so the hot path can be
verified not to regress to one bridge call per D3D9 state or draw call.

**R-CORE-11.14** The D3D9 hot-path architecture must preserve the DXMT-shaped
ownership boundary: PE `DeviceImpl` owns COM validation and the authoritative
`DeviceState`; `CommandRecorder` / `CommandChunk` own POD packet construction;
and the unix-side `CommandQueue` owns ordered import, backend shadow replay,
Metal encoding, and presentation pacing. Data-oriented helper functions must not
move those ownership responsibilities across the boundary.

**R-CORE-11.15** `DeviceState` in the PE frontend remains the authoritative source
for D3D9 mutable state. Backend shadows, dirty masks, packet-local normalized
state, and cache keys are derived representations. They must be reconstructible
from ordered PE state updates and draw packets, and must not become the source
queried by D3D9 getters or state blocks.

**R-CORE-11.16** State-delta normalization, draw packet normalization, fixed-
function key construction, vertex declaration/FVF packet normalization, and
barrier packet insertion must be implemented as deterministic stateless
transforms wherever possible. Given explicit input values and options, these
transforms must produce the same packets, derived keys, diagnostics, and errors
without reading live COM objects, mutating `DeviceState`, issuing bridge calls,
or depending on backend execution state.

**R-CORE-11.17** Resource retention for a `CommandChunk` must be derived from the
serialized packet data and inline payload metadata. The retention list may be
cached for efficiency, but it must not carry independent semantics or require
walking live D3D9 COM bindings after the packet stream is built.

**R-CORE-11.18** Stateless shader and packet transforms must have unit-testable
inputs and outputs. Tests must be able to exercise them with synthetic
`DeviceState` values, draw arguments, shader bytecode, backend capability
descriptors, and expected packet/IR/MSL/resource-reference outputs without
creating a real D3D9 device, loading `winemetal.so`, or submitting work to Metal.
