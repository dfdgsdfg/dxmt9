# Wine Bridge Phase 3 — End-to-End Smoke Test Results

## Result summary

**Reached:** PE `Direct3DCreate9Ex` → PE-side `dxmt9c_factory_create()`
generated client → PE winemetal.dll bridge initialization →
`__wine_unix_call_dispatcher` resolved successfully (`0x20869a690`).

**Blocked at:** `NtQueryVirtualMemory(MemoryWineUnixFuncs)` returns
`STATUS_DLL_NOT_FOUND` (`0xc0000135`). Wine cannot pair our PE
`winemetal.dll` with the ELF `winemetal.so` because the pairing
requires `wine_builtin_dll=true` (winebuild + winecrt0 link), which
Heroic's Wine bundles do not ship.

The exact failure path:

```
[winemetal-bridge] initialize(winemetal.so): ntdll=00006FFFFFD20000
[winemetal-bridge] initialize(winemetal.so): dispatcher export=...
[winemetal-bridge] initialize(winemetal.so): dispatcher=000000020869A690
[winemetal-bridge] dispatcher-only fallback: NtQueryVirtualMemory(1000)
                   status=0xc0000135 handle=0x0
[winemetal-bridge] dxmt9_winemetal_unix_call: bridge not ready status=0xc0000135
[dxmt9-pe]         create9_ex: factory create failed
FAIL: Direct3DCreate9Ex hr=0x8007000e (E_OUTOFMEMORY)
```

## Test environment

- Host: macOS aarch64
- Wine: Heroic Wine-11.6-DXMT (staging)
  `~/Library/Application Support/heroic/tools/wine/Wine-11.6-DXMT/`
- WINEPREFIX: `~/.dxmt9-wineprefix`
- App: `experiments/apps/D9VKD3D9Clear/d3d9-clear-x64.exe`
- WINEDLLOVERRIDES: `d3d9,winemetal=n`
- WINEDLLPATH: `$WINEPREFIX/dxmt9-unix`

Native overrides forced Wine to load our PE `d3d9.dll` and PE
`winemetal.dll` (visible as "Loaded ... : native" in the trace).
llvm-mingw runtime DLLs (`libc++.dll`, `libunwind.dll`) were copied
from `/Users/dididi/llvm-mingw/x86_64-w64-mingw32/bin/` into
`system32/` to satisfy import resolution.

## Why the pairing failed

Modern Wine pairs a PE DLL with its ELF unix module via one of:

1. **Builtin DLL convention**: the PE DLL was built with
   `winebuild --builtin <pe.dll>` (which embeds a magic section). Wine
   then auto-discovers the matching `<pe>.so` in the same path.
2. **Legacy `__wine_init_unix_call`**: deprecated, removed from
   modern ntdll exports. Our bridge code still tries this for backward
   compat (warning is benign).
3. **`NtQueryVirtualMemory(MemoryWineUnixFuncs=1000)`**: the modern
   path our `initializeDispatcherOnlyFallback` uses. **This call only
   returns a valid handle if Wine already knows about the pairing —
   i.e., the PE DLL is a builtin.**

Since `wine_builtin_dll=false` was used (Heroic Wine does not ship
`winebuild` / `libwinecrt0`), our PE winemetal.dll has no pairing
metadata. The dispatcher fallback fails at `NtQueryVirtualMemory`,
the bridge state stays in `STATUS_DLL_NOT_FOUND`, and every subsequent
`__wine_unix_call` returns immediately without reaching the .so.

## What's verified

| Layer | Status |
|---|---|
| PE `d3d9.dll` exports (`Direct3DCreate9Ex` @ ordinal 2) | ✅ Wine resolves and calls |
| PE-side `dxmt9_pe_create9_ex` → `dxmt9c_factory_create` | ✅ Logged via `[dxmt9-pe]` |
| Generated PE client (`dxmt9_device_c_bridge_exports.generated.cpp`) | ✅ Calls into bridge thunk |
| PE `winemetal.dll` bridge init (`initializeBridgeState`) | ✅ Resolves ntdll + dispatcher |
| PE→ELF `__wine_unix_call` actually crossing | ❌ Blocked at NtQueryVirtualMemory |
| ELF dispatch table slots (compile_shader … device_c thunks) | ✅ Built but unreached |

The PE↔ELF code path is **fully implemented** in `winemetal_bridge.cpp`
+ the generated client + the unified `__wine_unix_call_funcs[]` table.
The only missing piece is build-time pairing metadata.

## Path to green E2E

To complete Phase 3 we need `winebuild` + `libwinecrt0.a` +
`libntdll.a` + `libdbghelp.a` for `x86_64-windows`. Three options:

1. **Build a partial Wine SDK.** Clone wine-staging and run
   `./configure --enable-tools-only && make tools/winebuild libs/winecrt0
   dlls/ntdll dlls/dbghelp` against the same Wine version Heroic ships
   (11.6). Estimated 30-60 min on first run.

2. **Use the pre-built dxmt-deps repository** (the upstream dxmt
   project ships `dxmt-deps` with these binaries alongside an
   appropriate Wine). Mirrors what Heroic's Wine-11.6-DXMT was built
   against.

3. **Vendor `libwinecrt0.a` + import libs from a Wine devel install.**
   Some Linux distros ship `wine-devel` packages with these. Extract
   and place them under a path consumed via `-Dwine_install_path=`.

Once these are in place:

```sh
meson setup build-win32-x64-builtin --cross-file cross/x86_64-windows.ini \
  -Dwine_builtin_dll=true \
  -Dwine_install_path=/path/to/wine-sdk
ninja -C build-win32-x64-builtin
# winebuild postprocess will embed the pairing metadata into the PE DLLs;
# Wine's NtQueryVirtualMemory(1000) will then return a valid handle.
```

## Reproduction script

```sh
HEROIC_WINE="$HOME/Library/Application Support/heroic/tools/wine/Wine-11.6-DXMT/Contents/Resources/wine/bin/wine"
WP="$HOME/.dxmt9-wineprefix"
DXMT9=/Users/dididi/workspaces/dxmt9

WINEPREFIX="$WP" "$HEROIC_WINE" wineboot --init

# Install our DLLs (overwrites Wine-DXMT's bundled d3d9.dll)
cp "$DXMT9/build-win32-x64/src/win32/d3d9.dll" "$WP/drive_c/windows/system32/"
cp "$DXMT9/build-win32-x64/src/winemetal/winemetal.dll" "$WP/drive_c/windows/system32/"
cp /Users/dididi/llvm-mingw/x86_64-w64-mingw32/bin/{libc++,libunwind}.dll \
   "$WP/drive_c/windows/system32/"

# Stage the unix module so WINEDLLPATH can find it
mkdir -p "$WP/dxmt9-unix/x86_64-unix"
cp "$DXMT9/build/src/winemetal/unix/winemetal.so" "$WP/dxmt9-unix/x86_64-unix/"

# Run with debug
DXMT_LOG_LEVEL=debug \
WINEPREFIX="$WP" \
WINEDLLPATH="$WP/dxmt9-unix" \
WINEDLLOVERRIDES="d3d9,winemetal=n" \
"$HEROIC_WINE" "$DXMT9/experiments/apps/D9VKD3D9Clear/d3d9-clear-x64.exe"
```
