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
    WINE["Wine winemac.drv\nHWND → NSView*\nmacdrv_get_cocoa_view()"]
    WSI["dxmt9 WSI\nNSView → CAMetalLayer\nattach / resize / detach"]
    MTL["Metal\nCAMetalLayer.nextDrawable\n→ present"]

    APP --> WINE --> WSI --> MTL
```

---

## 2. HWND → Cocoa View Resolution

`dxmt9.so` supports two Wine host paths:

1. Legacy direct symbol path:
   - `macdrv_get_cocoa_view(HWND)` returns the backing `NSView*`
2. Builtin-module fallback path used by Heroic Wine 11.5:
   - `macdrv_functions[3]` returns a `WineWindow*`
   - `dxmt9.so` resolves `-[WineWindow contentView]` on the Cocoa main queue
   - `macdrv_functions[6]` creates a `WineMetalView*` from that
     `WineContentView*`
   - `macdrv_functions[7]` returns the `CAMetalLayer*`

The resolution order is:

```c
// 1. Try legacy direct NSView export
macdrv_get_cocoa_view(hwnd)

// 2. Otherwise fall back to Heroic/DXMT-style function table
macdrv_functions[3] -> WineWindow*
WineWindow.contentView -> WineContentView*
macdrv_functions[6](WineContentView*, metal_device) -> WineMetalView*
macdrv_functions[7](WineMetalView*) -> CAMetalLayer*
```

`dxmt9.so` resolves the legacy symbol or function table at runtime via
`dlsym(RTLD_DEFAULT, ...)`. If neither path is available (non-Wine environment,
unit tests, or an older Wine build), the function returns `nullptr`; `Present`
becomes a no-op and the device remains usable for offscreen rendering and
`GetRenderTargetData` readback.

---

## 3. Layer Lifecycle

### 3.1 Creation (at CreateDevice / CreateAdditionalSwapChain)

1. Resolve `HWND` to either:
   - `NSView*` directly through `macdrv_get_cocoa_view(hwnd)`, or
   - `WineWindow* -> contentView -> WineMetalView -> CAMetalLayer*` through the
     `macdrv_functions` fallback path.
2. If using the legacy `NSView*` path, create and attach a `CAMetalLayer` as
   the view's backing layer:
   ```objc
   CAMetalLayer *layer = [CAMetalLayer layer];
   layer.device          = mtlDevice;
   layer.pixelFormat     = MTLPixelFormatBGRA8Unorm;
   layer.drawableSize    = CGSizeMake(pp.BackBufferWidth, pp.BackBufferHeight);
   layer.displaySyncEnabled = (pp.PresentationInterval != D3DPRESENT_INTERVAL_IMMEDIATE);
   layer.maximumDrawableCount = 3;
   layer.framebufferOnly = YES;
   nsView.wantsLayer     = YES;
   nsView.layer          = layer;
   ```
3. If using the `WineMetalView` fallback path, store the returned
   `WineMetalView*` for later release and use its `CAMetalLayer*` directly.
4. Store `CAMetalLayer*` in the `SwapChain` object.

### 3.2 Resize

When `Reset()` or `IDirect3DSwapChain9::Reset()` is called with new dimensions:

1. Drain all in-flight command buffers (wait for GPU completion).
2. Update `layer.drawableSize`.
3. Recreate the back-buffer `MTLTexture` at the new size.

Resize is also triggered by `NSViewFrameDidChangeNotification`. In windowed mode
this is handled silently (no device lost). In fullscreen mode it surfaces as
`D3DERR_DEVICELOST`.

### 3.3 Destruction

On `IDirect3DDevice9::Release()` or swap chain destruction:

1. Drain GPU.
2. `nsView.layer = nil` — removes and releases the `CAMetalLayer`.

---

## 4. Windowed vs Fullscreen

### Windowed Mode (`Windowed = TRUE`)

- `layer.drawableSize` = `D3DPRESENT_PARAMETERS.BackBufferWidth/Height`.
- `displaySyncEnabled` controlled by `PresentationInterval`.

### Fullscreen Mode (`Windowed = FALSE`)

Not required for Phase 1. When implemented:

- Exclusive fullscreen requires `NSScreen` mode switching (`CGDisplayCapture`).
- `D3DPRESENT_INTERVAL_ONE` → `displaySyncEnabled = YES`.
- Return `D3DERR_NOTAVAILABLE` from `CreateDevice` for fullscreen until implemented.

---

## 5. Multi-Monitor

`IDirect3D9::GetAdapterCount()` returns the number of `MTLDevice` + `NSScreen` pairs.
For a single GPU with multiple monitors, one `MTLDevice` drives all screens.

The `AdapterIdentifier` for each adapter must include the monitor's `CGDirectDisplayID`.

---

## 6. PE Bridge and Unix Module Split

The Wine-facing runtime is split into three binaries:

| Binary | Kind | Role |
|---|---|---|
| `d3d9.dll` | PE DLL | User-facing D3D9 COM exports (`Direct3DCreate9`, `Direct3DCreate9Ex`) |
| `dxmt9.dll` | PE DLL | Internal bridge layer; marshals plain C / POD requests from `d3d9.dll` into Wine unix calls |
| `dxmt9.so` | Wine unix module | Native Metal / ObjC implementation, WSI, shader translation, and backend execution |

The bridge and unix module communicate through Wine's PE↔unix thunk mechanism.
`d3d9.dll` and `dxmt9.dll` are both PE images; `dxmt9.so` is the unix-side module
loaded by Wine. On macOS, `dxmt9.so` is still a Mach-O binary, but it follows
Wine's `.so` naming and loader conventions. The PE bridge must never import a
Mach-O / `.dylib` binary as if it were a Windows DLL.

All window/layer functions (`get_view`, `create_layer`, `resize_layer`,
`set_sync`, `destroy_layer`, `next_drawable`, `present_drawable`) live in
`dxmt9.so` using `macdrv_get_cocoa_view` + ObjC Metal APIs. The PE bridge owns
only marshalling, opaque handles, and Wine-visible thunk entry points.

---

## 7. Pixel Format for Presentation

`CAMetalLayer.pixelFormat` is always `MTLPixelFormatBGRA8Unorm`. The back buffer is
an internal `MTLTexture`; it is blitted to the BGRA drawable during present.

If `BackBufferFormat == D3DFMT_A8R8G8B8` or `D3DFMT_X8R8G8B8`, the blit is a
direct copy. For other formats a format-conversion pass is required.

---

## 8. Device Lost due to Window Events

| Event | Required action |
|---|---|
| Window destroyed while device alive | `TestCooperativeLevel()` → `D3DERR_DEVICELOST` |
| Display mode change (fullscreen) | `D3DERR_DEVICELOST` → `D3DERR_DEVICENOTRESET` after drain |
| GPU reset / Metal device removal | `D3DERR_DEVICELOST` |

Windowed resize does **not** cause device lost.

---

## 9. PE DLL, Bridge DLL, and Unix Module Initialization

`d3d9.dll` is the user-facing PE32+ D3D9 module loaded by the application.
`dxmt9.dll` is a second PE32+ bridge DLL loaded by `d3d9.dll`. `dxmt9.so` is the
unix-side Wine module that contains the native Metal and WSI implementation.

On the common macOS Wine64 / Rosetta path:

- `d3d9.dll` target: `x86_64-w64-mingw32`
- `dxmt9.dll` target: `x86_64-w64-mingw32`
- `dxmt9.so` target: host-side x86_64 Mach-O module loaded through Wine's unix
  loader path

32-bit x86 is not the primary target. The architecture split is therefore:

1. `d3d9.dll` handles COM entry points and user-visible D3D9 ABI.
2. `dxmt9.dll` handles plain-C marshalling and Wine unix-call dispatch.
3. `dxmt9.so` handles ObjC, Metal, `macdrv_get_cocoa_view`, and GPU execution.

### 9.1 Initialization Sequence

`d3d9.dll`'s `DllMain(DLL_PROCESS_ATTACH)`:

1. Initializes the D3D9-side wrapper state.
2. Resolves and loads `dxmt9.dll` as the internal PE bridge.
3. Exports `Direct3DCreate9` / `Direct3DCreate9Ex`; each calls into `dxmt9.dll`
   for factory creation.

`dxmt9.dll` then:

1. Registers its unix-call table with Wine.
2. Marshals `dxmt9c_*`, WSI, and shader requests into `dxmt9.so`.
3. Never imports `dxmt9.so` as a normal PE dependency; the handoff is through
   Wine's unix-call / unixlib path only.

`dxmt9.so` then:

1. Resolves `macdrv_get_cocoa_view` from `RTLD_DEFAULT`.
2. Creates `CAMetalLayer` objects on demand.
3. Owns all Metal device, queue, shader, and presentation state.

### 9.2 Installation (x86_64 / Rosetta)

The primary supported path is x86_64 Wine running under Rosetta 2 on Apple
Silicon. The Wine host may be GPTK, CrossOver, or a recent vanilla Wine build
for macOS, as long as it provides:

- Wine unixlib support
- `winemac.drv`
- the standard `x86_64-windows` and `x86_64-unix` runtime directories

Confirmed host:

- Heroic Wine 11.5 on macOS, validated on 2026-03-31 with the builtin-module
  layout: `dxmt9.dll` loads as a Wine builtin PE bridge, `dxmt9.so` loads as
  the unix module, and the full 180-frame `wsi_present_x64.exe` smoke passes

Known unsupported host:

- GPTK 1.1 / Wine 7.7, which does not provide the required unixlib bridge path

```sh
# Build prerequisites for Wine builtin modules:
# - winebuild
# - libwinecrt0.a
# - libntdll.a
# - libdbghelp.a
# - x86_64-unix/winemac.so
# - x86_64-unix/ntdll.so
#
# These may come from a Wine build tree or from a separate Wine toolchain
# install tree. A runtime-only Wine app bundle is not sufficient unless it
# ships those build-time assets.

# Build x86_64 dxmt9.so unix module as a Wine unix module:
meson setup build-x86_64-builtin \
  --native-file cross/x86_64-macos.ini \
  -Dwine_install_path=<wine-toolchain-root>
meson compile -C build-x86_64-builtin

# Build x86_64 d3d9.dll and dxmt9.dll as Wine builtin PE modules:
meson setup build-win32-x64-builtin \
  --cross-file cross/x86_64-windows.ini \
  -Dwine_builtin_dll=true \
  -Dwine_install_path=<wine-toolchain-root>
meson compile -C build-win32-x64-builtin

# Install the user-facing D3D9 override into the Wine prefix:
cp build-win32-x64-builtin/src/win32/d3d9.dll \
  <wine-prefix>/drive_c/windows/system32/d3d9.dll

# Install the internal bridge and unix module into Wine's module directories:
cp build-win32-x64-builtin/src/win32/dxmt9.dll \
  <wine-root>/lib/wine/x86_64-windows/dxmt9.dll
cp build-x86_64-builtin/src/dxmt9.so \
  <wine-root>/lib/wine/x86_64-unix/dxmt9.so

WINEDLLOVERRIDES="d3d9=n,b" wine app.exe
```

No custom Wine fork is required. On a compatible macOS Wine host, `dxmt9.so`
finds either the legacy `macdrv_get_cocoa_view` export or the `macdrv_functions`
fallback table via `dlsym(RTLD_DEFAULT, ...)` at the first `Present` call.

Current implementation note: the default installation layout is now:

- `d3d9.dll` in `<wine-prefix>/drive_c/windows/system32`
- `dxmt9.dll` in `<wine-root>/lib/wine/x86_64-windows`
- `dxmt9.so` in `<wine-root>/lib/wine/x86_64-unix`

`dxmt9.dll` is no longer copied into `system32` by default. A legacy
`system32/dxmt9.dll` staging mode still exists only for the old native-bridge
layout and should not be used for builtin-module hosts such as Heroic Wine
11.5.
