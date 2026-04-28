# Core Layer Requirements

The core layer is the Wine-facing half of dxmt9. It exposes the D3D9 COM interface
surface to applications running under Wine and is responsible for enforcing D3D9
semantics before handing work to the Metal backend.

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

---

## 8. Queries

**R-CORE-8.1** `D3DQUERYTYPE_EVENT` (GPU completion fence) must be supported.
`Issue(D3DISSUE_END)` followed by `GetData()` must correctly indicate whether the GPU
has completed all prior commands.

**R-CORE-8.2** `D3DQUERYTYPE_OCCLUSION` must be supported if the Metal device supports
visibility result buffers. The returned sample count is an approximation and is
permitted to be clamped to a boolean (non-zero = visible).

---

## 9. Error Handling

**R-CORE-9.1** All API calls must return documented `HRESULT` codes. `D3D_OK`
(zero) on success, `D3DERR_INVALIDCALL` for bad arguments or illegal state
transitions, and `D3DERR_OUTOFVIDEOMEMORY` / `E_OUTOFMEMORY` for allocation failures.

**R-CORE-9.2** Null pointer arguments to methods that document them as invalid must
return `D3DERR_INVALIDCALL`, not crash.

**R-CORE-9.3** The device must never silently discard draw calls or state changes.
Either the operation succeeds or it returns an error.

---

## 10. IDirect3DDevice9Ex

The Ex interface is required for applications that probe `Direct3DCreate9Ex` or
call `ResetEx` / `PresentEx`. It extends the base device and factory interfaces
without changing existing semantics.

**R-CORE-10.1** `Direct3DCreate9Ex(SDKVersion, ppD3D)` must return a valid
`IDirect3D9Ex*` when `SDKVersion == D3D_SDK_VERSION`. `IDirect3D9Ex` inherits
all `IDirect3D9` methods; the existing factory implementation must be reused.

**R-CORE-10.2** `IDirect3D9Ex::GetAdapterModeCountEx(Adapter, pFilter)` must
return the count of display modes matching `pFilter`. If `pFilter` is `NULL`
or `Format == D3DFMT_UNKNOWN`, all modes must be returned. The underlying mode
list is the same as `EnumAdapterModes()`.

**R-CORE-10.3** `IDirect3D9Ex::EnumAdapterModesEx(Adapter, pFilter, Mode, pMode)`
must fill a `D3DDISPLAYMODEEX` for the matching mode.
`D3DDISPLAYMODEEX::ScanLineOrdering` must be `D3DSCANLINEORDERING_PROGRESSIVE`.

**R-CORE-10.4** `IDirect3D9Ex::GetAdapterDisplayModeEx(Adapter, pMode, pRotation)`
must return the current display mode as `D3DDISPLAYMODEEX`.
`*pRotation` must be set to `D3DDISPLAYROTATION_IDENTITY`.

**R-CORE-10.5** `IDirect3D9Ex::GetAdapterLUID(Adapter, pLUID)` must return a
non-zero `LUID` that is stable for the adapter within the process lifetime.
It may be synthesised from the Metal device registry ID.

**R-CORE-10.6** `IDirect3D9Ex::CreateDeviceEx()` must create an
`IDirect3DDevice9Ex`. The additional `D3DDISPLAYMODEEX*` parameter overrides
the fullscreen display mode; if `NULL`, behaviour is identical to `CreateDevice()`.

**R-CORE-10.7** `IDirect3DDevice9Ex` inherits all `IDirect3DDevice9` methods.
The existing device implementation must be reused without modification.

**R-CORE-10.8** `CheckDeviceState(hDestinationWindow)` must return `D3D_OK`
while the device is operational, `S_PRESENT_OCCLUDED` when the window is
minimised or occluded, and `D3DERR_DEVICELOST` when the device is lost. It
replaces `TestCooperativeLevel()` for Ex applications.

**R-CORE-10.9** `ResetEx(pPresentationParameters, pFullscreenDisplayMode)` must
behave identically to `Reset()` for `D3DPOOL_DEFAULT` invalidation and swap-chain
rebuild. When `pFullscreenDisplayMode` is non-`NULL` it must be forwarded through
`normalizePresentParameters()`.

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

**R-CORE-10.16** `CreateRenderTargetEx()`, `CreateOffscreenPlainSurfaceEx()`, and
`CreateDepthStencilSurfaceEx()` must delegate to their non-Ex counterparts.
The `pSharedHandle` output parameter must be set to `NULL` (shared surfaces are
not supported).

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
affect an already recorded draw.

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
