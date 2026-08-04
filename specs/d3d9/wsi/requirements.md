---
type: "Spec Requirements"
title: "Window System Integration Requirements"
description: "D3D9 / WSI requirements and compatibility contracts."
tags: [specs, d3d9, wsi, requirements]
---

# Window System Integration Requirements

WSI maps the Win32 `HWND` supplied to D3D9 device and swap-chain creation onto a
Metal presentation surface. The normative behavior is the D3D9-visible
presentation contract; the Cocoa/Metal attachment mechanism is an implementation
detail.

Traceability: R-CORE-2.4-R-CORE-2.8, R-CORE-10.8-R-CORE-10.10,
R-CORE-11.4-R-CORE-11.5.

---

## 1. Swap Chain and Window Binding

`R-CORE-2.4` requires the device to track the active implicit swap chain created
from `D3DPRESENT_PARAMETERS` at `CreateDevice()` time. Additional swap chains
must be supported for windowed multi-window scenarios.

Each swap chain stores the caller-visible `HWND` used for presentation and an
opaque backend presentation handle when a host view/layer can be resolved.
Backend handles must remain opaque across the PE/unix bridge.

**R-CORE-WSI-1.1** A windowed, presenting swap chain must acquire its Wine
client surface and borrowed `CAMetalLayer` through the protocol defined by
`R-WMB-12.1`-`R-WMB-12.6`. If no supported protocol can resolve a presentation
layer for the `HWND`, device or swap-chain creation must fail cleanly with
`D3DERR_NOTAVAILABLE`. It must not create a device whose `Present` silently
becomes a no-window no-op. A future explicitly offscreen creation mode may
define different behavior under a separate requirement.

---

## 2. Presentation Parameters, `HRESULT`, and Resize

`R-CORE-2.6` requires `CreateDevice()`, `CreateDeviceEx()`, `Reset()`, and
`ResetEx()` to validate `D3DPRESENT_PARAMETERS` with Windows D3D9-compatible
rules before creating or rebuilding a swap chain. Invalid parameters fail with
`D3DERR_INVALIDCALL`; allocation or backend failures preserve their original
`HRESULT`.

Successful creation and reset must write normalized presentation parameters back
to the caller-visible structure, including resolved back-buffer dimensions,
normalized back-buffer count, and resolved back-buffer format.

`R-CORE-2.8` requires successful reset to rebuild the implicit swap chain and
restore Windows-compatible device state. Failed base-device reset may enter
lost-device state; Ex reset failures preserve Ex cooperative-level semantics.

Resize caused by reset updates the backend presentation layer and recreates the
back buffer after in-flight GPU work has drained. Windowed host-view resize does
not by itself make the device lost. Fullscreen mode changes may surface
device-lost behavior according to R-CORE-2.5 and R-CORE-2.8.

---

## 3. Present and Device State

`R-CORE-10.10` requires `PresentEx()` to present the full swap-chain back buffer.
`pSourceRect`, `pDestRect`, `pDirtyRegion`, and `dwFlags` are hints and may be
ignored. Base `Present()` follows the same full-buffer presentation behavior.

Presenting to a valid window commits the current command chunk and obtains a
backend drawable/frame token as required by the command queue and frame-latency
contracts. Loss of an already bound layer must follow the reset/rebind or
device-status path. It must return a Windows-compatible failure or device-status
`HRESULT` without dereferencing stale host-window state.

`R-CORE-10.8` requires `CheckDeviceState(hDestinationWindow)` to return `D3D_OK`
while the Ex device is operational, `S_PRESENT_OCCLUDED` when the destination
window is minimized or occluded, and `D3DERR_DEVICELOST` when the device is lost.

Base devices report lost/reset state through `TestCooperativeLevel()` as
specified by R-CORE-2.5.

---

## 4. Lifetime

Presentation-layer lifetime is owned by the swap chain. Reset and destruction
must drain or otherwise fence in-flight command buffers before destroying or
replacing backend presentation handles and back-buffer textures.

The PE side owns D3D9 COM lifetimes and stores only opaque backend handles for
WSI objects. Cocoa objects, Metal objects, and unix-side C++ objects remain
unix-side implementation details.

Bridge payload constraints follow `R-CORE-11.4`. Recorded chunks must contain
POD command records, scalar payloads, and opaque handles only. No Objective-C
object pointer, COM pointer, unix-side C++ object pointer, or executable payload
may enter a recorded chunk or hot-path replay schema.

**R-CORE-WSI-4.1** The dedicated cold WSI bootstrap/rebind call is the sole
exception for Wine-issued presentation tokens. It may transport the fixed-width
opaque client-surface and borrowed-layer values defined by `R-WMB-12.2`, but PE
code must never dereference or retain Objective-C ownership of them. Adoption,
quiescence, replacement, and release must follow `R-WMB-12.3`-`R-WMB-12.6`.

---

## 5. Fullscreen and Multi-Monitor

Fullscreen presentation is not required for the initial WSI target. Until
exclusive fullscreen is implemented, valid fullscreen creation may fail cleanly
with `D3DERR_NOTAVAILABLE` after parameter validation.

Adapter and monitor reporting must remain stable for the process lifetime.
Where multiple monitors are exposed, each adapter identifier must include a
stable display identity such as the host `CGDirectDisplayID`.

---

## 6. Backbuffer Readback And `framebufferOnly` Layer Mode

The D3D9-visible backbuffer surface returned by
`IDirect3DSwapChain9::GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, …)` and the
`CAMetalLayer` drawable used at present time are distinct Metal objects in
dxmt9. The backbuffer is a dxmt9-owned `MTLTexture` allocated through the
queue's resource pool (see `core_resources.cpp:319`); the drawable is acquired
from the layer's `nextDrawable` only inside the present encoder
(`dxmt9_presenter.cpp`) and is the destination of the present blit, not its
source.

The `DXMT9_LAYER_FRAMEBUFFER_ONLY` environment knob (see
`agents/rules/environment_variables.rules.md`) toggles
`CAMetalLayer.framebufferOnly` between `false` (default) and `true`. The
property describes the **drawable** backing, not the backbuffer texture.

**R-CORE-WSI-6.1** (Backbuffer Lock/Readback independence from layer mode)
`IDirect3DSurface9::LockRect`, `IDirect3DSurface9::UnlockRect`, and
`IDirect3DDevice9::GetRenderTargetData` invoked on the surface returned by
`IDirect3DSwapChain9::GetBackBuffer` must continue to behave per their normal
D3D9 contracts (Wine-oracle parity for invalid-rect / double-Lock /
Unlock-without-Lock / `D3DERR_INVALIDCALL` HRESULTs and successful pixel data
return on a valid lock) regardless of the value of
`DXMT9_LAYER_FRAMEBUFFER_ONLY`. The knob controls the
`CAMetalLayer.framebufferOnly` drawable property only; the dxmt9 backbuffer
`MTLTexture` is created with full read / blit usage flags through the queue
resource pool and is the source of any D3D9-side readback. The present-time
copy from the backbuffer texture into the drawable is the only operation
affected by the drawable's tile-only mode.

**R-CORE-WSI-6.2** (Configuration scope and no-op classification — D)
This requirement records the **current implementation behavior** as
unspecified at the D3D9-API surface: `DXMT9_LAYER_FRAMEBUFFER_ONLY=1` MUST
NOT silently change any D3D9 `Lock` / `GetRenderTargetData` HRESULT, MUST
NOT zero-fill caller buffers, and MUST NOT emit a per-call warning in any
log level. There is no `framebufferOnly` branch on the D3D9 Lock / readback
code paths (`src/d3d9/d3d9_pe_device_child_surface.cpp:397` for `LockRect`;
`src/d3d9/d3d9_pe_device.cpp:2426` and `src/d3d9/core_surface.cpp:354` for
`GetRenderTargetData`). Whether the present-side optimisation observably
changes any D3D9-visible behavior on a future Metal release is currently
not under test; the contract is "no D3D9-visible side effect today" and any
intentional future divergence must add a corresponding
`R-CORE-WSI-6.x` requirement and a Wine-oracle case before shipping.

**R-CORE-WSI-6.3** (`GetFrontBufferData` synchronous copy)
`IDirect3DDevice9::GetFrontBufferData` must delegate to the selected swap chain,
and the swap-chain method must validate a `D3DPOOL_SYSTEMMEM` destination in
`D3DFMT_A8R8G8B8`. The operation copies the swap chain's most recently rendered
present-source image synchronously through `GetRenderTargetData`; a multisampled
backbuffer is first resolved to a temporary single-sample render target. On
macOS this is a D3D9 swap-chain front-image approximation: it does not capture
WindowServer composition, occluding windows, or the full desktop outside the
swap-chain image.

Note: behavior **classification D (no framebufferOnly branch in the D3D9 API
surface; the toggle is a pure present-side optimisation)** applies. The
historical comment thread in `dxmt9_presenter.cpp:226-235` referring to
"D3D9 Lock() / GetRenderTargetData() on the backbuffer can read it" is
about the drawable's system-memory residency for the WindowServer
compositor path, not about the dxmt9 backbuffer `MTLTexture` read path.
