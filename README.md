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

This installs system and bootstrap tools:

| Package | Purpose |
|---|---|
| `meson` + `ninja` | Build system |
| `ripgrep` | Fast repository/script helper searches |
| `mise` | Project tool runtime manager for `uv` |
| `llvm` | C++20 / ObjC++ compiler and `clang-tidy` |
| `clang-format` | Source formatter |
| `temurin`, `tla+-toolbox` (casks) | Java runtime and TLC model checker for formal verification |
| `msitools`, `winetricks` | Optional Wine experiment helpers |

**Python via uv** (see `.mise.toml`, `.python-version`, and `pyproject.toml`):

```sh
mise trust
mise install
uv python install
uv sync
```

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

dxmt9 supports two deployment lanes:

| Lane | Use case | PE DLLs | Unix provider |
|---|---|---|---|
| Wine runtime builtin | DXMT-style runtime integration | Wine-builtin `d3d9.dll` + `winemetal.dll` | runtime-installed `winemetal.so` |
| Native app-local | DXVK-style per-application package | native PE `d3d9.dll` + `winemetal.dll` beside the game | app-local `winemetal.so` |

Both lanes intentionally keep `winemetal.so` as a separate Wine unixlib
provider. A `d3d9.dll`-only app-local package is not a supported artifact.

Set common paths first:

```sh
export PATH="$HOME/llvm-mingw/bin:$PATH"
export WINE_ROOT="/path/to/wine/runtime"
```

`WINE_ROOT` is the root that contains `bin/wine` or `bin/wine64` and
`lib/wine/`.

### Host Unit Build

```sh
meson setup build
meson compile -C build
meson test -C build
```

Output:

- `build/src/winemetal/unix/winemetal.so`

To use the Homebrew LLVM toolchain explicitly:

```sh
CC=$(brew --prefix llvm)/bin/clang \
CXX=$(brew --prefix llvm)/bin/clang++ \
meson setup build
```

### Shared x86_64 Unix Provider

On Apple Silicon, current macOS Wine hosts run Wine64 as x86_64 under Rosetta 2.
That includes Heroic Wine, CrossOver, and vanilla Wine builds packaged for
macOS. Build the unix provider against the target Wine runtime:

```sh
meson setup build-x86_64-builtin \
  --native-file cross/x86_64-macos.ini \
  -Dwine_install_path="$WINE_ROOT"
meson compile -C build-x86_64-builtin
```

Output:

- `build-x86_64-builtin/src/winemetal/unix/winemetal.so`

### Lane 1: Wine Runtime Builtin

The builtin lane requires a Wine toolchain/runtime root that provides
`winebuild`, `libwinecrt0.a`, `libntdll.a`, `libdbghelp.a`, `winemac.so`, and
`ntdll.so`.

Build the 64-bit PE DLLs:

```sh
meson setup build-win32-x64-builtin \
  --cross-file cross/x86_64-windows.ini \
  -Dwine_builtin_dll=true \
  -Dwine_install_path="$WINE_ROOT"
meson compile -C build-win32-x64-builtin
```

Build the 32-bit WoW64 PE DLLs:

```sh
meson setup build-win32-x86-builtin \
  --cross-file cross/i686-windows.ini \
  -Dwine_builtin_dll=true \
  -Dwine_install_path="$WINE_ROOT"
meson compile -C build-win32-x86-builtin
```

Outputs:

| Arch | D3D9 DLL | Bridge DLL |
|---|---|---|
| x64 | `build-win32-x64-builtin/src/win32/d3d9.dll` | `build-win32-x64-builtin/src/winemetal/winemetal.dll` |
| x86 | `build-win32-x86-builtin/src/win32/d3d9.dll` | `build-win32-x86-builtin/src/winemetal/winemetal.dll` |

### Lane 2: Native App-Local

The app-local lane disables builtin post-processing. Native `d3d9.dll` does not
statically import `winemetal.dll`; it loads the sibling bridge DLL from the
application directory at runtime.

Build the 64-bit app-local PE DLLs:

```sh
meson setup build-win32-x64 \
  --cross-file cross/x86_64-windows.ini \
  -Dwine_builtin_dll=false
meson compile -C build-win32-x64
```

Build the 32-bit app-local PE DLLs:

```sh
meson setup build-win32-x86 \
  --cross-file cross/i686-windows.ini \
  -Dwine_builtin_dll=false
meson compile -C build-win32-x86
```

Create a mixed x64/x86 app-local package:

```sh
python3 scripts/tools/package_app_local.py --clean --output-dir dist/dxmt9-app-local
```

Package outputs:

```text
dist/dxmt9-app-local/
  dxmt9-deploy.json
  pe/x64/d3d9.dll
  pe/x64/winemetal.dll
  pe/x64/libc++.dll
  pe/x64/libunwind.dll
  pe/x86/d3d9.dll
  pe/x86/winemetal.dll
  pe/x86/libc++.dll
  pe/x86/libunwind.dll
  unix/x86_64-unix/winemetal.so
```

---

## Runtime Layout

`dxmt9` is split into three runtime binaries:

| Binary | Kind | Role |
|---|---|---|
| `d3d9.dll` | PE DLL | User-facing D3D9 entry points loaded by the app |
| `winemetal.dll` | PE DLL | PE bridge that dispatches `dxmt9c_*` and shader/provider calls into `winemetal.so` |
| `winemetal.so` | Wine unixlib provider | Single unix-side root for generated dispatch, provider/runtime code, and shader-service handlers |

The two lanes differ only in how those binaries are discovered:

- Builtin: Wine loads builtin `d3d9.dll`; `d3d9.dll` imports builtin
  `winemetal.dll`; `winemetal.dll` uses Wine builtin unixlib metadata to find
  runtime-installed `winemetal.so`.
- App-local: Wine's normal DLL search loads `d3d9.dll` next to the game;
  `d3d9.dll` dynamically loads sibling `winemetal.dll`; `winemetal.dll` locates
  `winemetal.so` from `DXMT9_WINEMETAL_SO`, its own directory, or the process
  executable directory. Runtime provider fallback is disabled unless
  `DXMT9_ALLOW_RUNTIME_PROVIDER_FALLBACK=1` is set.

The PE bridge must not import a Mach-O `.dylib` or `.so` directly.

---

## Installing dxmt9

Choose one lane per prefix/application. Do not mix builtin and app-local
installations for the same game while debugging load-order issues.

You also need a recent macOS Wine build with:

- unixlib support, including `MemoryWineLoadUnixLibByName` for app-local mode;
- `winemac.drv`;
- a standard Wine runtime layout with `x86_64-windows`, `i386-windows`, and
  `x86_64-unix` when running both x64 and x86 games.

Confirmed host:

- Heroic Wine 11.6 on macOS, tested on 2026-04-17 with the builtin
  `d3d9.dll` + `winemetal.dll` + `winemetal.so` layout;
  `wsi_present_x64.exe` completes the full 180-frame present smoke

Intended hosts that follow the same Wine runtime model:

- recent CrossOver
- recent vanilla Wine on macOS

Known unsupported host:

- GPTK 1.1 / Wine 7.7, which is too old for this unixlib bridge model

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

### Install Lane 1: Wine Runtime Builtin

For Heroic's bundled Wine, use the installer helper:

```sh
bash scripts/install/install_heroic_wine.sh \
  --prefix "$WINEPREFIX" \
  --wine-root "$WINE_ROOT"
```

The script installs x64 artifacts by default and automatically installs the
WoW64 x86 lane when `build-win32-x86-builtin` exists. It creates one-time
`.dxmt9-backup` copies before overwriting existing files.

Manual install layout:

| Arch | Source | Destination |
|---|---|---|
| x64 | `build-win32-x64-builtin/src/win32/d3d9.dll` | `$WINE_ROOT/lib/wine/x86_64-windows/d3d9.dll` |
| x64 | `build-win32-x64-builtin/src/winemetal/winemetal.dll` | `$WINE_ROOT/lib/wine/x86_64-windows/winemetal.dll` |
| x64 | `~/llvm-mingw/x86_64-w64-mingw32/bin/libc++.dll` | `$WINEPREFIX/drive_c/windows/system32/libc++.dll` |
| x64 | `~/llvm-mingw/x86_64-w64-mingw32/bin/libunwind.dll` | `$WINEPREFIX/drive_c/windows/system32/libunwind.dll` |
| x86 | `build-win32-x86-builtin/src/win32/d3d9.dll` | `$WINE_ROOT/lib/wine/i386-windows/d3d9.dll` |
| x86 | `build-win32-x86-builtin/src/winemetal/winemetal.dll` | `$WINE_ROOT/lib/wine/i386-windows/winemetal.dll` |
| x86 | `~/llvm-mingw/i686-w64-mingw32/bin/libc++.dll` | `$WINEPREFIX/drive_c/windows/syswow64/libc++.dll` |
| x86 | `~/llvm-mingw/i686-w64-mingw32/bin/libunwind.dll` | `$WINEPREFIX/drive_c/windows/syswow64/libunwind.dll` |
| host | `build-x86_64-builtin/src/winemetal/unix/winemetal.so` | `$WINE_ROOT/lib/wine/x86_64-unix/winemetal.so` |

The helper also mirrors the PE DLLs into prefix `system32` and `syswow64`
because some launcher workflows inspect prefix DLLs before applying builtin
overrides.

Run with builtin override:

```sh
WINEPREFIX="$WINEPREFIX" \
WINEDLLOVERRIDES="d3d9=b" \
"$WINE_ROOT/bin/wine" game.exe
```

For 32-bit D3D9 games under Heroic, the game's config must also enable WoW64:

```json
{
  "enableWoW64": true
}
```

### Install Lane 2: Native App-Local

Copy one PE architecture plus the host unix provider into the game executable
directory.

For a 64-bit game:

```sh
GAME_DIR="/path/to/game"
cp dist/dxmt9-app-local/pe/x64/d3d9.dll "$GAME_DIR/"
cp dist/dxmt9-app-local/pe/x64/winemetal.dll "$GAME_DIR/"
cp dist/dxmt9-app-local/pe/x64/libc++.dll "$GAME_DIR/"
cp dist/dxmt9-app-local/pe/x64/libunwind.dll "$GAME_DIR/"
cp dist/dxmt9-app-local/unix/x86_64-unix/winemetal.so "$GAME_DIR/"
```

For a 32-bit game:

```sh
GAME_DIR="/path/to/game"
cp dist/dxmt9-app-local/pe/x86/d3d9.dll "$GAME_DIR/"
cp dist/dxmt9-app-local/pe/x86/winemetal.dll "$GAME_DIR/"
cp dist/dxmt9-app-local/pe/x86/libc++.dll "$GAME_DIR/"
cp dist/dxmt9-app-local/pe/x86/libunwind.dll "$GAME_DIR/"
cp dist/dxmt9-app-local/unix/x86_64-unix/winemetal.so "$GAME_DIR/"
```

Run without forcing Wine's builtin D3D9:

```sh
WINEPREFIX="$WINEPREFIX" \
"$WINE_ROOT/bin/wine" "$GAME_DIR/game.exe"
```

If the launcher needs an explicit override, use native-first:

```sh
WINEDLLOVERRIDES="d3d9=n,b"
```

Do not use `WINEDLLOVERRIDES="d3d9=b"` for app-local mode; it bypasses the
`d3d9.dll` next to the game.

---

## Verify TLA+ specs

The concurrent subsystems (command queue, resource lifetime, encoder lifecycle,
query resolution) are formally verified with TLC:

```sh
TLA2TOOLS_JAR="/Applications/TLA+ Toolbox.app/Contents/Eclipse/tla2tools.jar" \
  bash scripts/check/verify_tla.sh
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
cross/           Meson cross/native files for macOS unix and Windows PE builds
tests/           Regression and conformance suites
  native/        Fast native unit/spec tests by owner (core, shader, backend,
                 bridge, smoke)
  shader_runner/ shader_runner_dxmt9 plus manifest-driven .shader_test corpus
  conformance/   Wine-oracle PE D3D9 conformance tests
  integration/   Wine/WSI end-to-end smoke tests
  fixtures/      Static test fixtures
specs/           Specifications and formal verification
  core/          D3D9 COM requirements and design
  backend/       Metal translation requirements and design
  deploy/        Wine runtime and native app-local deployment specs
  verification/  TLA+ specs (CommandQueue, ResourceLifetime,
                 EncoderLifecycle, QuerySeqId) + .cfg model files
  gap.md         Spec–implementation gap tracker
scripts/         Purpose-organized helper scripts; see scripts/README.md
                 (codegen, check, build_apps, run_apps, run_suites, install,
                 tools)
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
