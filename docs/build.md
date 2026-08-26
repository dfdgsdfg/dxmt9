# Building dxmt9 from Source

Developer guide: toolchain setup, the two build lanes, runtime layout,
manual installation, and formal verification. For a prebuilt package,
see the installation section of the top-level [README](../README.md).

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
| `mise` | Owns the exact Python and uv tool versions and project task environment |
| `llvm` | C++20 / ObjC++ compiler and `clang-tidy` |
| `clang-format` | Source formatter |
| `temurin`, `tla+-toolbox` (casks) | Java runtime and TLC model checker for formal verification |
| `msitools`, `winetricks` | Optional Wine experiment helpers |

**Python via mise + uv** (see `.mise.toml`, `.python-version`,
`pyproject.toml`, and the committed `uv.lock`):

```sh
mise trust
mise install
mise run python:check
```

`mise` pins Python and uv; the sync task binds uv's internal project environment
to that exact mise Python and installs only the committed lockfile resolution.
There is no activation step and no environment path is added to `PATH`. Run
repository Python commands through `scripts/run_python.sh`; it resolves the
pinned tools and invokes `uv run --locked`. Meson and repository shell entry
points use the same launcher and therefore never select a system, Homebrew,
Conda, or agent-specific Python. The launcher creates or synchronizes the
environment on demand; `mise run python:sync` remains available for CI and an
explicit eager sync.

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

## Build

dxmt9 supports two deployment lanes:

| Lane | Use case | PE DLLs | Unix provider |
|---|---|---|---|
| Wine runtime builtin | DXMT-style runtime integration | Wine-builtin `d3d9.dll` + `winemetal_dxmt9.dll` | runtime-installed `winemetal_dxmt9.so` |
| Native app-local | DXVK-style per-application package | native PE `d3d9.dll` + `winemetal_dxmt9.dll` beside the game | app-local `winemetal_dxmt9.so` |

Both lanes intentionally keep `winemetal_dxmt9.so` as a separate Wine unixlib
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

- `build/src/winemetal/unix/winemetal_dxmt9.so`

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

- `build-x86_64-builtin/src/winemetal/unix/winemetal_dxmt9.so`

The link against the Wine runtime matters: `winemetal_dxmt9.so` must carry
`@rpath/winemac.so` / `@rpath/ntdll.so` install-name dependencies for
Wine's unixlib lookup to accept it. The canonical build target runs an
install-name fixup post-link; a direct
`ninja src/winemetal/unix/winemetal_dxmt9.so` skips it. The audit
`scripts/check/audit_winemetal_install_names.py` (meson test
`dxmt9-winemetal-install-name-audit`) checks for this regression.

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
| x64 | `build-win32-x64-builtin/src/win32/d3d9.dll` | `build-win32-x64-builtin/src/winemetal/winemetal_dxmt9.dll` |
| x86 | `build-win32-x86-builtin/src/win32/d3d9.dll` | `build-win32-x86-builtin/src/winemetal/winemetal_dxmt9.dll` |

### Lane 2: Native App-Local

The app-local lane disables builtin post-processing. Native `d3d9.dll` does not
statically import `winemetal_dxmt9.dll`; it loads the sibling bridge DLL from the
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
scripts/run_python.sh scripts/tools/package_app_local.py --clean --output-dir dist/dxmt9-app-local
```

Package outputs:

```text
dist/dxmt9-app-local/
  dxmt9-deploy.json
  pe/x64/d3d9.dll
  pe/x64/winemetal_dxmt9.dll
  pe/x64/libc++.dll
  pe/x64/libunwind.dll
  pe/x86/d3d9.dll
  pe/x86/winemetal_dxmt9.dll
  pe/x86/libc++.dll
  pe/x86/libunwind.dll
  unix/x86_64-unix/winemetal_dxmt9.so
```

This is the same package the release workflow
(`.github/workflows/release.yml`) publishes on version tags.

## Runtime Layout

`dxmt9` is split into three runtime binaries:

| Binary | Kind | Role |
|---|---|---|
| `d3d9.dll` | PE DLL | User-facing D3D9 entry points loaded by the app |
| `winemetal_dxmt9.dll` | PE DLL | PE bridge that dispatches `dxmt9c_*` and shader/provider calls into `winemetal_dxmt9.so` |
| `winemetal_dxmt9.so` | Wine unixlib provider | Single unix-side root for generated dispatch, provider/runtime code, and shader-service handlers |

The two lanes differ only in how those binaries are discovered:

- Builtin: Wine loads builtin `d3d9.dll`; `d3d9.dll` imports builtin
  `winemetal_dxmt9.dll`; `winemetal_dxmt9.dll` uses Wine builtin unixlib metadata to find
  runtime-installed `winemetal_dxmt9.so`.
- App-local: Wine's normal DLL search loads `d3d9.dll` next to the game;
  `d3d9.dll` dynamically loads sibling `winemetal_dxmt9.dll`; `winemetal_dxmt9.dll` locates
  `winemetal_dxmt9.so` from `DXMT9_WINEMETAL_SO`, its own directory, or the process
  executable directory. Runtime provider fallback is disabled unless
  `DXMT9_ALLOW_RUNTIME_PROVIDER_FALLBACK=1` is set.

The PE bridge must not import a Mach-O `.dylib` or `.so` directly.

## Installing a Local Build

Choose one lane per prefix/application. Do not mix builtin and app-local
installations for the same game while debugging load-order issues.

You also need a recent macOS Wine build with:

- unixlib support, including `MemoryWineLoadUnixLibByName` for app-local mode;
- `winemac.drv`;
- a standard Wine runtime layout with `x86_64-windows`, `i386-windows`, and
  `x86_64-unix` when running both x64 and x86 games.

See the top-level README for which Wine runtimes are known to work with
real games (the `winemac.so` symbol-export requirement).

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

Known unsupported host:

- GPTK 1.1 / Wine 7.7, which is too old for this unixlib bridge model

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
| x64 | `build-win32-x64-builtin/src/winemetal/winemetal_dxmt9.dll` | `$WINE_ROOT/lib/wine/x86_64-windows/winemetal_dxmt9.dll` |
| x64 | `~/llvm-mingw/x86_64-w64-mingw32/bin/libc++.dll` | `$WINEPREFIX/drive_c/windows/system32/libc++.dll` |
| x64 | `~/llvm-mingw/x86_64-w64-mingw32/bin/libunwind.dll` | `$WINEPREFIX/drive_c/windows/system32/libunwind.dll` |
| x86 | `build-win32-x86-builtin/src/win32/d3d9.dll` | `$WINE_ROOT/lib/wine/i386-windows/d3d9.dll` |
| x86 | `build-win32-x86-builtin/src/winemetal/winemetal_dxmt9.dll` | `$WINE_ROOT/lib/wine/i386-windows/winemetal_dxmt9.dll` |
| x86 | `~/llvm-mingw/i686-w64-mingw32/bin/libc++.dll` | `$WINEPREFIX/drive_c/windows/syswow64/libc++.dll` |
| x86 | `~/llvm-mingw/i686-w64-mingw32/bin/libunwind.dll` | `$WINEPREFIX/drive_c/windows/syswow64/libunwind.dll` |
| host | `build-x86_64-builtin/src/winemetal/unix/winemetal_dxmt9.so` | `$WINE_ROOT/lib/wine/x86_64-unix/winemetal_dxmt9.so` |

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
cp dist/dxmt9-app-local/pe/x64/winemetal_dxmt9.dll "$GAME_DIR/"
cp dist/dxmt9-app-local/pe/x64/libc++.dll "$GAME_DIR/"
cp dist/dxmt9-app-local/pe/x64/libunwind.dll "$GAME_DIR/"
cp dist/dxmt9-app-local/unix/x86_64-unix/winemetal_dxmt9.so "$GAME_DIR/"
```

For a 32-bit game:

```sh
GAME_DIR="/path/to/game"
cp dist/dxmt9-app-local/pe/x86/d3d9.dll "$GAME_DIR/"
cp dist/dxmt9-app-local/pe/x86/winemetal_dxmt9.dll "$GAME_DIR/"
cp dist/dxmt9-app-local/pe/x86/libc++.dll "$GAME_DIR/"
cp dist/dxmt9-app-local/pe/x86/libunwind.dll "$GAME_DIR/"
cp dist/dxmt9-app-local/unix/x86_64-unix/winemetal_dxmt9.so "$GAME_DIR/"
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

## Verify TLA+ Specs

The concurrency-sensitive subsystems are formally verified with TLC —
20 modules under `specs/verification/tla/` (see the README's formal
verification section for the full list):

```sh
TLA2TOOLS_JAR="/Applications/TLA+ Toolbox.app/Contents/Eclipse/tla2tools.jar" \
  bash scripts/check/verify_tla.sh
```

Expected output: `Model checking completed. No error has been found.` for
every module. The same check runs as part of `meson test`. Without
`TLA2TOOLS_JAR` (or `tlc` on PATH), the script downloads `tla2tools.jar`
itself and only needs a Java runtime.

## Repository Layout

```
include/dxmt9/   Public headers (core.hpp, assert.hpp, winemetal.h, device_c.h)
src/             Implementation (D3D9 frontend, Metal runtime, util, bridge layers)
  win32/         Win32 PE forwarding layer (`d3d9.dll`)
  winemetal/     Unified bridge stack (`winemetal_dxmt9.dll` + `winemetal_dxmt9.so`)
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
  verification/  TLA+ specs (20 modules: command queue, resource
                 lifetime, present pacing, wire protocol, ...) + .cfg
                 model files
  gap.md         Spec–implementation gap tracker
scripts/         Purpose-organized helper scripts; see scripts/README.md
                 (codegen, check, build_apps, run_apps, run_suites, install,
                 tools)
```
