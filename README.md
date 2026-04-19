# dxmt9

Wine / D3D9-to-Metal translation layer for macOS.

Translates Direct3D 9 API calls from applications running under Wine directly
into Metal with no Vulkan middle layer. Supports `IDirect3D9`,
`IDirect3D9Ex`, `IDirect3DDevice9`, and `IDirect3DDevice9Ex`.

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

**llvm-mingw** (required for the Win32 PE bridge DLLs):

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

### Native unix module root (`winemetal.so`)

```sh
meson setup build
meson compile -C build
meson test -C build
```

Output: `build/src/winemetal/unix/winemetal.so`

To use the Homebrew LLVM toolchain explicitly:

```sh
CC=$(brew --prefix llvm)/bin/clang \
CXX=$(brew --prefix llvm)/bin/clang++ \
meson setup build
```

### Wine builtin PE bridge set (`d3d9.dll` + `winemetal.dll`)

Requires llvm-mingw on PATH plus a Wine toolchain root that provides
`winebuild`, `libwinecrt0.a`, `libntdll.a`, `libdbghelp.a`, `winemac.so`, and
`ntdll.so`.

```sh
meson setup build-win32-x64-builtin \
  --cross-file cross/x86_64-windows.ini \
  -Dwine_builtin_dll=true \
  -Dwine_install_path=/path/to/wine/toolchain
meson compile -C build-win32-x64-builtin
```

Outputs:

- `build-win32-x64-builtin/src/win32/d3d9.dll`
- `build-win32-x64-builtin/src/winemetal/winemetal.dll`

### x86_64 unix module for Wine64 / Rosetta (`winemetal.so`)

On Apple Silicon, current macOS Wine hosts run Wine64 as x86_64 under Rosetta 2.
That includes GPTK, CrossOver, and vanilla Wine builds packaged for macOS.
Build an x86_64 `winemetal.so` using a meson native file:

```sh
meson setup build-x86_64-builtin \
  --native-file cross/x86_64-macos.ini \
  -Dwine_install_path=/path/to/wine/toolchain
meson compile -C build-x86_64-builtin
```

Output:

- `build-x86_64-builtin/src/winemetal/unix/winemetal.so`

---

## Runtime Layout

`dxmt9` is split into three runtime binaries:

| Binary | Kind | Role |
|---|---|---|
| `d3d9.dll` | PE DLL | User-facing D3D9 entry points loaded by the app |
| `winemetal.dll` | PE DLL | Internal PE bridge that dispatches `dxmt9c_*` and shader/provider calls into `winemetal.so` |
| `winemetal.so` | Wine unix module | Single unixlib root that hosts generated `dxmt9c_*` dispatch plus provider/runtime + shader-service handlers |

`d3d9.dll` imports `winemetal.dll`. `winemetal.dll` dispatches into the single
`winemetal.so` unixlib. The PE bridge must not import a Mach-O `.dylib`
directly.

---

## Installing dxmt9

### For users

You need these files from a release or local build:

- `d3d9.dll`
- `winemetal.dll`
- `winemetal.so`
- `libc++.dll` and `libunwind.dll` from `llvm-mingw` if your Wine prefix does
  not already have them

You also need a recent macOS Wine build with:

- unixlib support
- `winemac.drv`
- a standard Wine runtime layout with `x86_64-windows` and `x86_64-unix`

Confirmed host:

- Heroic Wine 11.6 on macOS, tested on 2026-04-17 with the builtin
  `d3d9.dll` + `winemetal.dll` + `winemetal.so` layout;
  `wsi_present_x64.exe` completes the full 180-frame present smoke

Intended hosts that follow the same Wine runtime model:

- recent CrossOver
- recent vanilla Wine on macOS

Known unsupported host:

- GPTK 1.1 / Wine 7.7, which is too old for this unixlib bridge model

Example install:

```sh
export WINEPREFIX="$HOME/.wine"
export WINE_ROOT="/path/to/your/wine/runtime"

cp build-win32-x64-builtin/src/win32/d3d9.dll \
  "$WINEPREFIX/drive_c/windows/system32/d3d9.dll"

cp build-win32-x64-builtin/src/winemetal/winemetal.dll \
  "$WINE_ROOT/lib/wine/x86_64-windows/winemetal.dll"

cp build-x86_64-builtin/src/winemetal/unix/winemetal.so \
  "$WINE_ROOT/lib/wine/x86_64-unix/winemetal.so"

# Required when the PE bridge was built with llvm-mingw's libc++ runtime
cp "$HOME/llvm-mingw/x86_64-w64-mingw32/bin/libc++.dll" \
  "$WINEPREFIX/drive_c/windows/system32/libc++.dll"
cp "$HOME/llvm-mingw/x86_64-w64-mingw32/bin/libunwind.dll" \
  "$WINEPREFIX/drive_c/windows/system32/libunwind.dll"

WINEDLLOVERRIDES="d3d9=n,b" \
  "$WINE_ROOT/bin/wine" game.exe
```

What goes where:

- `d3d9.dll` goes into the Wine prefix because applications load it directly.
- `winemetal.dll` goes into Wine's `x86_64-windows` runtime directory as the builtin
  PE bridge for both the main D3D9 C ABI and the public shader ABI. It is not copied
  into `system32` on the verified builtin path.
- `winemetal.so` goes into Wine's `x86_64-unix` runtime directory because it is
  the single unix-side service root paired with `winemetal.dll`.

Typical `WINE_ROOT` examples:

- GPTK:
  `/Applications/Game Porting Toolkit.app/Contents/Resources/wine`
- CrossOver:
  `/Applications/CrossOver.app/Contents/SharedSupport/CrossOver`
- Vanilla Wine:
  the root that contains `bin/wine64` and `lib/wine/`

Typical `WINEPREFIX` examples:

- GPTK or vanilla Wine:
  `~/.wine` or a custom prefix path
- CrossOver:
  the bottle prefix path managed by CrossOver

For Heroic's bundled Wine, the repo includes an installer helper that stages
the files into the current Heroic runtime and a target prefix:

```sh
bash scripts/install_heroic_wine.sh --prefix "/path/to/heroic/prefix"
```

The script auto-detects the latest `Wine-*` runtime under Heroic, installs the
latest plain Heroic Wine runtime by default, skips Heroic `*-DXMT` variants
unless you pass `--wine-root`, installs `winemetal.dll` and `winemetal.so`,
copies `libc++.dll` / `libunwind.dll`, and creates `.dxmt9-backup` copies
before overwriting an existing file. This is the verified install path for
Heroic Wine 11.6.

### For developers

Build the unix module and PE DLLs locally, then install them into the same
locations as above:

```sh
meson setup build
meson compile -C build

meson setup build-x86_64-builtin \
  --native-file cross/x86_64-macos.ini \
  -Dwine_install_path=/path/to/wine/toolchain
meson compile -C build-x86_64-builtin

export PATH="$HOME/llvm-mingw/bin:$PATH"
meson setup build-win32-x64-builtin \
  --cross-file cross/x86_64-windows.ini \
  -Dwine_builtin_dll=true \
  -Dwine_install_path=/path/to/wine/toolchain
meson compile -C build-win32-x64-builtin
```

If you are on Intel macOS and your Wine runtime is also x86_64, you can use the
normal `build/src/winemetal/unix/winemetal.so` output directly instead of
`build-x86_64-builtin/src/winemetal/unix/winemetal.so`.

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
src/             Implementation (D3D9 frontend, Metal runtime, util, bridge layers)
  win32/         Win32 PE forwarding layer (`d3d9.dll`)
  winemetal/     Unified bridge stack (`winemetal.dll` + `winemetal.so`)
cross/           Meson cross/native files for Wine x86_64 and Windows cross-builds
tests/           Smoke tests and core spec tests
specs/           Specifications and formal verification
  core/          D3D9 COM requirements and design
  backend/       Metal translation requirements and design
  verification/  TLA+ specs (CommandQueue, ResourceLifetime,
                 EncoderLifecycle, QuerySeqId) + .cfg model files
  gap.md         Spec–implementation gap tracker
scripts/         verify_tla.sh, install_heroic_wine.sh, corpus tooling
```

---

## Status

| Layer | Status |
|---|---|
| Core (D3D9 COM surface, device state, draw calls) | Complete |
| Metal backend (command queue, PSO cache, FFP shaders, D3DBC translation) | Complete |
| Formal verification (TLC, all 4 specs) | Complete |
| Main bridge ABI (`dxmt9c_*` through `winemetal.dll` / `winemetal.so`) | Complete |
| PE forwarding layer (`d3d9.dll`) | Complete |
| Unified bridge stack (`winemetal.dll` + `winemetal.so`) | Complete |
| WSI (`winemac` legacy + fallback resolution, `CAMetalLayer`) | Complete |
