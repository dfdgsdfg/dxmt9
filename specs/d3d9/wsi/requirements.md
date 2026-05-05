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

If no host view or presentation layer can be resolved for the `HWND`, device
creation may still succeed when the rest of the presentation parameters are
valid. `Present` becomes a no-window no-op that preserves device usability for
offscreen rendering and readback.

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
contracts. Presenting with no resolved window must still perform required
ordering work, then return a Windows-compatible success or device-status
`HRESULT` without dereferencing null host-window state.

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

Bridge payload constraints follow `R-CORE-11.4`. Recorded chunks and WSI bridge
calls must contain POD command records, scalar payloads, and opaque handles only.
No Objective-C object pointer, COM pointer, unix-side C++ object pointer, or
executable payload may cross the PE/unix boundary.

---

## 5. Fullscreen and Multi-Monitor

Fullscreen presentation is not required for the initial WSI target. Until
exclusive fullscreen is implemented, valid fullscreen creation may fail cleanly
with `D3DERR_NOTAVAILABLE` after parameter validation.

Adapter and monitor reporting must remain stable for the process lifetime.
Where multiple monitors are exposed, each adapter identifier must include a
stable display identity such as the host `CGDirectDisplayID`.
