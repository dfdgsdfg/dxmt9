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

## 2. HWND → NSView Resolution

Wine's macOS window driver (`winemac.drv`) exports `macdrv_get_cocoa_view` as a
global symbol in distributions that include it. **No custom Wine fork is required.**
GPTK 2.0+, current CrossOver, and recent vanilla Wine builds on macOS are all
valid host candidates as long as they expose the symbol:

```c
/* Exported from winemac.drv in GPTK 2.0+ and equivalent Wine distributions */
macdrv_view macdrv_get_cocoa_view(HWND hwnd);
/* macdrv_view is NSView* cast to an opaque type — safe to cast back */
```

`dxmt9.so` resolves this symbol at runtime via `dlsym(RTLD_DEFAULT, ...)`.
`RTLD_DEFAULT` searches all already-loaded images in the process; `winemac.drv.so`
is loaded by Wine before any D3D9 call reaches `dxmt9.so`:

```c
static NSView* get_nsview_for_hwnd(u64 hwnd) {
    using Fn = NSView* (*)(void*);
    static Fn fn = reinterpret_cast<Fn>(
        dlsym(RTLD_DEFAULT, "macdrv_get_cocoa_view"));
    if (!fn) return nil;
    return fn(reinterpret_cast<void*>(static_cast<uintptr_t>(hwnd)));
}
```

If the symbol is absent (non-Wine environment, unit tests, or an older Wine build)
the function returns `nullptr`; `Present` becomes a no-op and the device remains
usable for offscreen rendering and `GetRenderTargetData` readback.

---

## 3. Layer Lifecycle

### 3.1 Creation (at CreateDevice / CreateAdditionalSwapChain)

1. Resolve `HWND` to `NSView*` via `macdrv_get_cocoa_view(hwnd)`.
2. Create and attach a `CAMetalLayer` as the view's backing layer:
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
3. Store `CAMetalLayer*` in the `SwapChain` object.

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

- Heroic Wine 11.4 on macOS, validated on 2026-03-30

Known unsupported host:

- GPTK 1.1 / Wine 7.7, which does not provide the required unixlib bridge path

```sh
# Build x86_64 dxmt9.so unix module (requires meson native file for x86_64 target):
meson setup build-x86_64 --native-file cross/x86_64-macos.ini
meson compile -C build-x86_64

# Build x86_64 d3d9.dll and dxmt9.dll PE binaries:
meson setup build-win32-x64 --cross-file cross/x86_64-windows.ini
meson compile -C build-win32-x64

# Install the user-facing D3D9 override into the Wine prefix:
cp build-win32-x64/src/win32/d3d9.dll \
  <wine-prefix>/drive_c/windows/system32/d3d9.dll
cp build-win32-x64/src/win32/dxmt9.dll \
  <wine-prefix>/drive_c/windows/system32/dxmt9.dll

# Install the internal bridge and unix module into Wine's module directories:
cp build-win32-x64/src/win32/dxmt9.dll \
  <wine-root>/lib/wine/x86_64-windows/dxmt9.dll
cp build-x86_64/src/dxmt9.so \
  <wine-root>/lib/wine/x86_64-unix/dxmt9.so

WINEDLLOVERRIDES="d3d9=n,b" wine app.exe
```

No custom Wine fork is required. On a compatible macOS Wine host,
`winemac.drv.so` exports `macdrv_get_cocoa_view`, and `dxmt9.so` finds it via
`dlsym(RTLD_DEFAULT, ...)` at the first `Present` call.

Current implementation note: `dxmt9.dll` is installed in both
`<wine-prefix>/drive_c/windows/system32` and
`<wine-root>/lib/wine/x86_64-windows`. The `system32` copy satisfies PE import
resolution for `d3d9.dll`; the runtime copy remains the Wine bridge module.
