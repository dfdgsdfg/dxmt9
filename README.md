# dxmt9

Wine / D3D9-to-Metal translation layer for macOS.

Translates Direct3D 9 API calls from applications running under Wine directly
into Metal — no Vulkan middle layer. Targets plain `IDirect3DDevice9`;
`IDirect3DDevice9Ex` is next.

Related projects: [DXMT](https://github.com/3Shain/dxmt) (D3D11/D3D12 Metal),
[d3d9-webgl](https://github.com/LostMyCode/d3d9-webgl) (D3D9 reference).

---

## Prerequisites

**macOS with Xcode Command Line Tools:**

```sh
xcode-select --install
```

**Homebrew packages** (see `Brewfile`):

```sh
brew bundle
```

This installs:

| Package | Purpose |
|---|---|
| `meson` + `ninja` | Build system |
| `llvm` | C++20 / ObjC++ compiler |
| `tla+-toolbox` (cask) | TLC model checker for formal verification |

**llvm-mingw** (required for the Win32 PE `d3d9.dll` only):

`llvm-mingw` is not in Homebrew. Download the latest pre-built universal binary from GitHub and unpack it:

```sh
curl -L https://github.com/mstorsjo/llvm-mingw/releases/download/20260324/llvm-mingw-20260324-ucrt-macos-universal.tar.xz \
  | tar -xJ --strip-components=1 -C ~/llvm-mingw
```

Then add it to your PATH for the cross-compilation step:

```sh
export PATH="$HOME/llvm-mingw/bin:$PATH"
```

---

## Build

### `libdxmt9.dylib` (macOS native)

```sh
meson setup build
meson compile -C build
meson test -C build
```

To use the Homebrew LLVM toolchain explicitly:

```sh
CC=$(brew --prefix llvm)/bin/clang \
CXX=$(brew --prefix llvm)/bin/clang++ \
meson setup build
```

### `d3d9.dll` (Win32 PE, cross-compiled for ARM64 Windows / Wine)

Requires llvm-mingw on PATH (see Prerequisites above).

```sh
meson setup build-win32 --cross-file cross/aarch64-windows.ini
meson compile -C build-win32
```

Output: `build-win32/src/win32/d3d9.dll`

### Installing into a Wine prefix

`libdxmt9.dylib` looks up `macdrv_get_cocoa_view` at runtime via `dlsym`.
This function is present in the dxmt9 Wine fork — it is not in stock Wine.

```sh
# Build Wine from https://github.com/dfdgsdfg/wine (tracks wine-mirror/wine main;
# the only change is macdrv_get_cocoa_view in dlls/winemac.drv/window.c).
# Then install dxmt9 into the prefix:
cp build/src/libdxmt9.dylib ~/.wine/drive_c/windows/system32/dxmt9.dll
cp build-win32/src/win32/d3d9.dll ~/.wine/drive_c/windows/system32/d3d9.dll
WINEDLLOVERRIDES="d3d9=n,b" wine game.exe
```

---

## Verify TLA+ specs

The concurrent subsystems (command queue, resource lifetime, encoder lifecycle,
query resolution) are formally verified with TLC:

```sh
TLA2TOOLS_JAR="/Applications/TLA+ Toolbox.app/Contents/Eclipse/tla2tools.jar" \
  bash scripts/verify_tla.sh
```

Expected output: `Model checking completed. No error has been found.` for all
four modules. The same check runs as part of `meson test`.

---

## Repository layout

```
include/dxmt9/   Public headers (core.hpp, assert.hpp, winemetal.h, device_c.h)
src/             Implementation (core, backend sim + Metal, Wine bridge, C ABI)
  win32/         Win32 PE d3d9.dll (cross-compiled with llvm-mingw)
cross/           Meson cross-file for ARM64 Windows (aarch64-windows.ini)
tests/           Smoke tests and core spec tests
specs/           Specifications and formal verification
  core/          D3D9 COM requirements and design
  backend/       Metal translation requirements and design
  verification/  TLA+ specs (CommandQueue, ResourceLifetime,
                 EncoderLifecycle, QuerySeqId) + .cfg model files
  gap.md         Spec–implementation gap tracker
scripts/         verify_tla.sh
```

---

## Status

| Layer | Status |
|---|---|
| Core (D3D9 COM surface, device state, draw calls) | Complete |
| Metal backend (command queue, PSO cache, FFP shaders, D3DBC translation) | Complete |
| Formal verification (TLC, all 4 specs) | Complete |
| C ABI bridge (`dxmt9c_*` — `libdxmt9.dylib` exports) | Complete |
| Win32 PE wrapper (`d3d9.dll` — llvm-mingw cross-build) | Complete |
| WSI (`macdrv_get_cocoa_view` + `CAMetalLayer` — requires dxmt9 Wine fork) | Complete |
