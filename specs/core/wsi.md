# Window System Integration Spec

WSI (Window System Integration) defines how a Win32 window handle (`HWND`) maps to
a Metal presentation surface (`CAMetalLayer`), and how the swap chain stays in sync
with the window through resize, minimize, and mode changes.

---

## 1. The Problem

D3D9's `CreateDevice()` and `CreateAdditionalSwapChain()` receive a Win32 `HWND`. On
macOS under Wine, an `HWND` is a Wine internal handle backed by a macOS `NSWindow` /
`NSView`. Metal presents to a `CAMetalLayer` attached to an `NSView`. The WSI layer
must bridge these.

```mermaid
graph LR
    APP["Application\nhDeviceWindow = HWND"]
    WINE["Wine\nHWND → NSWindow/NSView\nvia win32 window mapping"]
    WSI["dxmt9 WSI\nNSView → CAMetalLayer\nattach / resize / detach"]
    MTL["Metal\nCAMetalLayer.nextDrawable\n→ present"]

    APP --> WINE --> WSI --> MTL
```

---

## 2. Layer Lifecycle

### 2.1 Creation (at CreateDevice / CreateAdditionalSwapChain)

1. Resolve `HWND` to a macOS `NSView*` via Wine's `winemetal` window query API
   (`winemetal_get_view_for_hwnd(hwnd) → view_handle`).
2. Create a `CAMetalLayer` and attach it as the view's backing layer:
   ```objc
   CAMetalLayer *layer = [CAMetalLayer layer];
   layer.device          = mtlDevice;
   layer.pixelFormat     = MTLPixelFormatBGRA8Unorm;  // always BGRA for display
   layer.drawableSize    = CGSizeMake(pp.BackBufferWidth, pp.BackBufferHeight);
   layer.displaySyncEnabled = (pp.PresentationInterval != D3DPRESENT_INTERVAL_IMMEDIATE);
   layer.maximumDrawableCount = 3;
   layer.framebufferOnly = YES;  // no sampling from drawable; blit-only
   nsView.layer          = layer;
   nsView.wantsLayer     = YES;
   ```
3. Store the `CAMetalLayer*` in the `SwapChain` object as an opaque handle (crossed
   via `winemetal` for Wine builds).

### 2.2 Resize

When the application calls `Reset()` or `IDirect3DSwapChain9::Reset()` with new
`BackBufferWidth` / `BackBufferHeight`:

1. Drain all in-flight command buffers (wait for GPU completion).
2. Update `layer.drawableSize`.
3. Recreate the back-buffer `MTLTexture` at the new size.
4. The `CAMetalLayer` handles resizing internally; no layer detach/reattach is needed.

Resize must also be triggered when the underlying `NSView` bounds change (e.g., user
drags the window border in windowed mode). The WSI layer must register for
`NSViewFrameDidChangeNotification` and update `drawableSize` accordingly, queueing
a resize event that dxmt9 surfaces as `D3DERR_DEVICELOST` (or handles silently for
windowed mode).

### 2.3 Destruction

On `IDirect3DDevice9::Release()` or swap chain destruction:
1. Drain GPU.
2. Remove the `CAMetalLayer` from the view (`nsView.layer = nil`).
3. Release the `CAMetalLayer` reference.

---

## 3. Windowed vs Fullscreen

### Windowed Mode (`Windowed = TRUE`)

- `CAMetalLayer.drawableSize` = `D3DPRESENT_PARAMETERS.BackBufferWidth/Height`.
- The layer is sized to match the back buffer, which may differ from window client
  area (stretch-to-fit is handled by `CALayer` autoresizing or by Metal's present
  scaling).
- `displaySyncEnabled` is controlled by `PresentationInterval`.

### Fullscreen Mode (`Windowed = FALSE`)

D3D9 fullscreen is not required for Phase 1. When implemented:
- Exclusive fullscreen requires `NSScreen` mode switching (via `CGDisplayCapture`).
- The Metal layer must cover the entire screen.
- `D3DPRESENT_INTERVAL_ONE` → `displaySyncEnabled = YES` with refresh rate matching.
- Return `D3DERR_NOTAVAILABLE` from `CreateDevice` for fullscreen until implemented.

---

## 4. Multi-Monitor

`IDirect3D9::GetAdapterCount()` returns the number of `MTLDevice` + `NSScreen` pairs.
For the common single-GPU multi-monitor case, one `MTLDevice` drives all screens.
Each adapter reports one primary `NSScreen`.

The `AdapterIdentifier` for each adapter must include the monitor's `CGDirectDisplayID`
so applications can identify physical displays.

---

## 5. Wine Bridge Requirements

Under Wine, all `NSView` and `CAMetalLayer` manipulation must happen on the macOS
(unix lib) side via the `winemetal` thunk interface. The following calls must be
available:

| Function | Description |
|---|---|
| `winemetal_get_view_for_hwnd(hwnd)` | Returns a `view_handle` for the Win32 HWND |
| `winemetal_create_metal_layer(view_handle, device_handle, params)` | Creates and attaches `CAMetalLayer`, returns `layer_handle` |
| `winemetal_resize_metal_layer(layer_handle, width, height)` | Updates `drawableSize` |
| `winemetal_set_sync_enabled(layer_handle, enabled)` | Sets `displaySyncEnabled` |
| `winemetal_destroy_metal_layer(layer_handle)` | Removes layer and releases it |
| `winemetal_next_drawable(layer_handle)` | Returns `drawable_handle` (blocks on vsync if enabled) |
| `winemetal_present_drawable(cmdbuf_handle, drawable_handle)` | Encodes present into command buffer |

All handles are opaque integers crossing the Win32/unix boundary.

---

## 6. Pixel Format for Presentation

The `CAMetalLayer.pixelFormat` must always be `MTLPixelFormatBGRA8Unorm` for display
output, regardless of the `D3DPRESENT_PARAMETERS.BackBufferFormat`. The back buffer
is an internal `MTLTexture` in the format matching the requested format
(see `formats.md`); it is blitted to the BGRA drawable during present.

If `BackBufferFormat == D3DFMT_A8R8G8B8` or `D3DFMT_X8R8G8B8`, the back buffer is
`MTLPixelFormatBGRA8Unorm` and the blit is a direct copy with no format conversion.

If the back buffer format differs (e.g., `D3DFMT_A16B16G16R16F`), the present blit
must include a format conversion pass (fullscreen triangle with a sampler and tone-
mapping if applicable, or a simple format-conversion blit if no HDR → SDR mapping
is needed).

---

## 7. Device Lost due to Window Events

The following events must trigger device-lost behavior:

| Event | Required action |
|---|---|
| Window destroyed while device alive | `TestCooperativeLevel()` → `D3DERR_DEVICELOST` |
| Display mode change (fullscreen) | `TestCooperativeLevel()` → `D3DERR_DEVICELOST` → `D3DERR_DEVICENOTRESET` after drain |
| GPU reset / Metal device removal | `TestCooperativeLevel()` → `D3DERR_DEVICELOST` |

In windowed mode, window resize does **not** cause device lost. The swap chain
silently resizes.

---

## 8. Wine Integration and Initialization Protocol

`libdxmt9.dylib` is a native macOS shared library. The actual `d3d9.dll` loaded by
Wine is a thin PE wrapper (built with mingw-w64 or Wine's PE toolchain, in a
separate Wine fork repository) that links to `libdxmt9.dylib` via Wine's unix-call
mechanism. On Apple Silicon, Wine's PE and unix address spaces are unified, so the
dylib's symbols are directly callable from PE code.

### 8.1 Initialization Sequence

The PE wrapper must perform the following steps in `DllMain(DLL_PROCESS_ATTACH)`:

1. **Populate a `WinemetalApi` table** with Wine's own implementations of each
   bridge function (HWND→view lookup, `CAMetalLayer` lifecycle, drawable
   management, shader compilation thunk).

2. **Register the table** by calling:
   ```c
   dxmt9_winemetal_set_api(&wine_winemetal_api);
   ```
   This one call wires all 11 bridge functions into `libdxmt9.dylib` for the
   lifetime of the process.

3. **Export the Win32 entry points** with the standard signatures:
   ```c
   IDirect3D9* WINAPI Direct3DCreate9(UINT sdkVersion);
   HRESULT     WINAPI Direct3DCreate9Ex(UINT sdkVersion, IDirect3D9Ex** ppD3D);
   ```
   Each calls `dxmt9::com::Direct3DCreate9` / `Direct3DCreate9Ex` with a
   freshly constructed `MetalBackendDevice`.

### 8.2 ABI Contract

The `WinemetalApi` struct in `include/dxmt9/winemetal.h` is the complete ABI
contract between the PE wrapper and `libdxmt9.dylib`. All 11 function pointers
must be non-null when `dxmt9_winemetal_set_api` is called. The PE wrapper owns
the lifetime of any objects returned as opaque `dxmt9_u64` handles.

| Handle type | Owned by | Released by |
|---|---|---|
| `view_handle` | Wine window system | never (Wine owns NSView lifetime) |
| `layer_handle` | PE wrapper / swap chain | `winemetal_destroy_metal_layer` |
| `drawable_handle` | `CAMetalLayer` | `winemetal_present_drawable` (consumes it) |
| `shader_handle` | dxmt9 backend | `winemetal_destroy_shader` |

### 8.3 Installation

```sh
# Wine DLL directory (per-prefix):
cp build/src/libdxmt9.dylib ~/.wine/drive_c/windows/system32/
cp wine-fork/build/d3d9.dll  ~/.wine/drive_c/windows/system32/

# Override so Wine loads the native build, not its built-in d3d9:
WINEDLLOVERRIDES="d3d9=n,b" wine app.exe
# or permanently via winetricks / Wine registry:
# HKCU\Software\Wine\DllOverrides  d3d9 = "native,builtin"
```

The PE `d3d9.dll` build lives in the Wine fork repository, not here. This
repository's responsibility ends at `libdxmt9.dylib` and the `WinemetalApi`
ABI boundary.
