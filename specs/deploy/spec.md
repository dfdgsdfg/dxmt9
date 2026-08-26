---
type: "Spec"
title: "Deployment Spec"
description: "Deploy spec, ownership, ordering, and evidence mapping."
tags: [specs, deploy, spec]
---

# Deployment Spec

---

## 1. Overview

Deployment is split into two lanes that share the same D3D9 frontend and Metal
runtime:

```mermaid
flowchart TD
    subgraph Runtime["Wine runtime install"]
        RD9["lib/wine/*-windows/d3d9.dll\nbuiltin PE"]
        RWM["lib/wine/*-windows/winemetal_dxmt9.dll\nbuiltin PE bridge"]
        RSO["lib/wine/*-unix/winemetal_dxmt9.so\nunix provider"]
        RD9 --> RWM --> RSO
    end

    subgraph Local["Application-local install"]
        EXE["Game.exe"]
        LD9["./d3d9.dll\nnative PE"]
        LWM["./winemetal_dxmt9.dll\nnative PE bridge"]
        LSO["./winemetal_dxmt9.so\nunix provider"]
        EXE --> LD9 --> LWM --> LSO
    end

    subgraph Shared["Shared implementation"]
        PECORE["D3D9 PE COM frontend"]
        BRIDGE["generated dxmt9c_* unix-call client"]
        PROVIDER["winemetal_dxmt9.so provider\nD3D9 core + Metal runtime"]
    end

    RD9 --> PECORE
    LD9 --> PECORE
    RWM --> BRIDGE
    LWM --> BRIDGE
    RSO --> PROVIDER
    LSO --> PROVIDER
```

The runtime lane keeps the current dxmt workflow. The app-local lane adds a
native PE `d3d9.dll` and native PE `winemetal_dxmt9.dll` so Wine can discover them
from the executable directory the same way DXVK-style DLL drops work.

---

## 2. Artifact Matrix

| Lane | Artifact | Build form | Install location |
|---|---|---|---|
| Runtime | `d3d9.dll` | PE DLL postprocessed as Wine builtin | `<wine-root>/lib/wine/x86_64-windows/` or `i386-windows/` |
| Runtime | `winemetal_dxmt9.dll` | PE DLL postprocessed as Wine builtin | matching `*-windows` runtime dir |
| Runtime | `winemetal_dxmt9.so` | Wine unixlib provider | `<wine-root>/lib/wine/<host>-unix/` |
| App-local | `d3d9.dll` | native PE DLL, no builtin postprocess | next to `Game.exe` |
| App-local | `winemetal_dxmt9.dll` | native PE DLL, no builtin postprocess | next to `Game.exe` |
| App-local | `winemetal_dxmt9.so` | Wine unixlib provider | next to `Game.exe` or explicit override path |

For a mixed package:

```text
package/
  pe/
    x64/
      d3d9.dll
      winemetal_dxmt9.dll
      libc++.dll        # only if dynamically required
      libunwind.dll     # only if dynamically required
    x86/
      d3d9.dll
      winemetal_dxmt9.dll
      libc++.dll
      libunwind.dll
  unix/
    x86_64-unix/
      winemetal_dxmt9.so
    aarch64-unix/
      winemetal_dxmt9.so
  dxmt9-deploy.json
```

A final app-local staging directory for one executable is flat:

```text
Game.exe
d3d9.dll
winemetal_dxmt9.dll
winemetal_dxmt9.so
```

The staging step selects exactly one PE architecture and one matching Wine host
unix provider from the manifest. A 32-bit game receives the 32-bit PE DLLs; a
64-bit game receives the 64-bit PE DLLs. The unix provider is selected from the
Wine host architecture, not from the Windows process bitness.

### 2.1 Module Namespace and DXMT Coexistence

The conventional API entry point remains `d3d9.dll`, because application DLL
discovery requires that name. The private bridge/provider pair is suffix-
qualified:

| Product | PE bridge | Unix provider |
|---|---|---|
| upstream DXMT | `winemetal.dll` | `winemetal.so` |
| dxmt9 | `winemetal_dxmt9.dll` | `winemetal_dxmt9.so` |

The names are module identities, not packaging aliases. Installers must not
create cross-product symlinks or copy a dxmt9 binary under an upstream DXMT
basename. Each PE bridge must resolve only its matching unix provider and must
perform its own ABI handshake before dispatch. This keeps D3D9 and D3D10/11
translation independently loadable in one prefix and, when the application
uses both API families, in one process.

---

## 3. Build Lanes

### 3.1 Runtime Builtin Lane

The existing lane remains the default for Wine runtime installation:

```sh
meson setup build-x86_64-builtin \
  -Dwine_install_path="$WINE_ROOT"
meson compile -C build-x86_64-builtin

meson setup build-win32-x64-builtin \
  --cross-file cross/x86_64-windows.ini \
  -Dwine_builtin_dll=true \
  -Dwine_install_path="$WINE_ROOT"
meson compile -C build-win32-x64-builtin
```

Output consumed by the runtime installer:

```text
build-win32-x64-builtin/src/win32/d3d9.dll
build-win32-x64-builtin/src/winemetal/winemetal_dxmt9.dll
build-x86_64-builtin/src/winemetal/unix/winemetal_dxmt9.so
```

The installer copies those files into the Wine runtime and optionally mirrors
the PE DLLs into the prefix Windows directories.

### 3.2 Native App-Local Lane

The app-local lane uses the same Windows cross toolchain and disables builtin
post-processing, but that is not sufficient by itself. The native lane must
also select a `d3d9.dll` target that uses the dynamic bridge resolver described
in section 5. It must not link the D3D9 forwarder against an import dependency
that creates a mandatory PE static import of `winemetal_dxmt9.dll`.

```sh
meson setup build-win32-x64-native \
  --cross-file cross/x86_64-windows.ini \
  -Dwine_builtin_dll=false
meson compile -C build-win32-x64-native
```

The command above describes the native lane shape. The concrete Meson target
must produce an app-local `d3d9.dll` whose generated bridge client reaches
`dxmt9_winemetal_unix_call` through `LoadLibraryExW` and `GetProcAddress`, not
through `dxmt9_winemetal_import_dep` or another static import edge.

Required packaging outputs:

```text
build-win32-x64-native/src/win32/d3d9.dll
build-win32-x64-native/src/winemetal/winemetal_dxmt9.dll
build-x86_64-builtin/src/winemetal/unix/winemetal_dxmt9.so
```

The native `d3d9.dll` should dynamically resolve app-local `winemetal_dxmt9.dll`
instead of linking it as a mandatory static import. This keeps a missing
`winemetal_dxmt9.dll` or provider load failure in the `Direct3DCreate9` failure path,
where dxmt9 can log a useful diagnostic. Non-Wine runtime dependencies of
`d3d9.dll` itself must still be packaged or statically linked, because the
Windows loader can fail before dxmt9 code runs.

The Meson install directories do not need to match the final app-local package
layout. A packaging step should collect the artifacts into a flat app-local
directory and generate a manifest.

```sh
scripts/run_python.sh scripts/tools/package_app_local.py --clean --output-dir dist/dxmt9-app-local
```

---

## 4. Runtime Install Flow

```mermaid
sequenceDiagram
    participant App as Game
    participant Wine as Wine loader
    participant D9 as builtin d3d9.dll
    participant WM as builtin winemetal_dxmt9.dll
    participant SO as runtime winemetal_dxmt9.so

    App->>Wine: LoadLibrary("d3d9.dll") or implicit import
    Wine->>D9: load builtin d3d9.dll
    D9->>WM: generated bridge imports
    WM->>Wine: __wine_init_unix_call / builtin unixlib lookup
    Wine->>SO: load runtime winemetal_dxmt9.so
    D9->>WM: dxmt9c_factory_create()
    WM->>SO: __wine_unix_call(handle, opcode, args)
```

This path is compatible with `WINEDLLOVERRIDES="d3d9=b"` and with the existing
runtime installer.

---

## 5. Application-Local Flow

```mermaid
sequenceDiagram
    participant App as Game.exe directory
    participant Wine as Wine loader
    participant D9 as ./d3d9.dll
    participant WM as ./winemetal_dxmt9.dll
    participant SO as ./winemetal_dxmt9.so

    App->>Wine: LoadLibrary("d3d9.dll") or implicit import
    Wine->>D9: load native PE d3d9.dll from app dir
    D9->>D9: derive sibling path from d3d9.dll HMODULE
    D9->>Wine: LoadLibraryExW("<d3d9-dir>\\winemetal_dxmt9.dll")
    Wine->>WM: load native PE winemetal_dxmt9.dll from app dir
    WM->>Wine: load unix provider by explicit path
    Wine->>SO: load ./winemetal_dxmt9.so as unixlib
    D9->>WM: dxmt9c_factory_create()
    WM->>SO: __wine_unix_call(provider_handle, opcode, args)
```

The key difference from the runtime path is that the app-local `winemetal_dxmt9.dll`
cannot depend on Wine builtin metadata to find `winemetal_dxmt9.so`. It needs its own
provider locator.

The `winemetal_dxmt9.dll` PE bridge must be loaded by absolute sibling path. The
native `d3d9.dll` records its own `HMODULE` in `DllMain`, calls
`GetModuleFileNameW(d3d9_hmodule)`, replaces the final component with
`winemetal_dxmt9.dll`, and loads that absolute path with `LoadLibraryExW`. The load
flags should allow dependent PE DLLs to resolve from the bridge DLL directory,
for example `LOAD_WITH_ALTERED_SEARCH_PATH` or
`LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS`. It must
not use `LoadLibraryW(L"./winemetal_dxmt9.dll")`, because `.` is the current working
directory rather than the module directory.

---

## 6. Provider Locator

`winemetal_dxmt9.dll` owns provider discovery, but the lookup mode differs by bridge
variant. The builtin bridge is allowed to use Wine builtin metadata first. The
native app-local bridge prioritizes package-local providers and only falls back
to a runtime provider when explicitly requested.

```mermaid
flowchart TD
    START["dxmt9_winemetal_unix_call() first use"]
    MODE{Bridge locator mode}
    BUILTIN["builtin mode:\ntry Wine builtin unixlib metadata"]
    ENV["DXMT9_WINEMETAL_SO set?"]
    MODDIR["winemetal_dxmt9.so next to loaded winemetal_dxmt9.dll"]
    EXEDIR["winemetal_dxmt9.so next to process image"]
    ALLOW{DXMT9_ALLOW_RUNTIME_PROVIDER_FALLBACK=1?}
    BYNAME["Wine unixlib by-name lookup:\nwinemetal_dxmt9.so"]
    OK["cache module + unix-call handle"]
    FAIL["cache failure status\nlog candidates"]

    START --> MODE
    MODE -->|builtin| BUILTIN
    MODE -->|app-local| ENV
    BUILTIN -->|success| OK
    BUILTIN -->|not found| ENV
    ENV -->|success| OK
    ENV -->|not set or failed| MODDIR
    MODDIR -->|success| OK
    MODDIR -->|failed| EXEDIR
    EXEDIR -->|success| OK
    EXEDIR -->|failed| ALLOW
    ALLOW -->|yes| BYNAME
    ALLOW -->|no| FAIL
    BYNAME -->|success| OK
    BYNAME -->|failed| FAIL
```

The PE bridge already dispatches generated calls through
`dxmt9_winemetal_unix_call(code, args)`. A cold loader adapter owns Wine-version
differences and initializes the handle used by that function:

```cpp
enum class BridgeLocatorMode {
    Builtin,
    AppLocal,
};

struct BridgeState {
    std::once_flag initialized;
    BridgeLocatorMode mode = BridgeLocatorMode::AppLocal;
    unixlib_module_t module = 0;
    unixlib_handle_t handle = 0;
    WineUnixCallDispatcherVar dispatcher = nullptr;
    WineUnloadUnixLibFn unload = nullptr;
    NTSTATUS status = DXMT9_STATUS_DLL_NOT_FOUND;
};

enum class UnixlibLoaderCapability : uint32_t {
    BuiltinPairV1 = 1u << 0,
    ByNameV1 = 1u << 1,
};

struct WineUnixlibLoader {
    uint32_t capabilities;
    NTSTATUS (*loadBuiltin)(HMODULE module, unixlib_handle_t* handle);
    NTSTATUS (*loadByName)(const UNICODE_STRING& name,
                           unixlib_module_t* module,
                           unixlib_handle_t* handle);
    NTSTATUS (*unload)(unixlib_module_t module);
};
```

`WineUnixlibLoader` is not a new hot-path abstraction. It is initialized once
before the first provider call and stores plain function pointers and capability
bits. Raw Wine information classes and Wine CRT/helper differences stay inside
its implementation.

Pseudo-code:

```cpp
static NTSTATUS initializeProvider(BridgeState& state) {
    WineUnixlibLoader loader = probeWineUnixlibLoader();

    if (state.mode == BridgeLocatorMode::Builtin) {
        require loader.capabilities contains BuiltinPairV1;
        if (loader.loadBuiltin(currentBridgeModule(), &state.handle) == STATUS_SUCCESS)
            return STATUS_SUCCESS;
    }

    require loader.capabilities contains ByNameV1;
    for (const auto& candidate : appLocalProviderCandidates()) {
        NTSTATUS status = loader.loadByName(candidate.name,
                                            &state.module,
                                            &state.handle);
        log candidate and status;
        if (status == STATUS_SUCCESS)
            return STATUS_SUCCESS;
    }

    if (runtimeProviderFallbackAllowed()) {
        NTSTATUS status = loader.loadByName(makeUnicodeString(L"winemetal_dxmt9.so"),
                                            &state.module,
                                            &state.handle);
        log by-name fallback and status;
        if (status == STATUS_SUCCESS)
            return STATUS_SUCCESS;
    }

    return DXMT9_STATUS_DLL_NOT_FOUND;
}
```

The adapter may use Wine's builtin module query for `builtin-pair-v1` and the
by-name unixlib query for `by-name-v1`. It may call `__wine_load_unix_lib` only
when that helper is actually linked or exported. In particular, the design does
not require `GetProcAddress(ntdll, "__wine_load_unix_lib")`: upstream Wine
declares the helper through its unixlib support library, but that does not make
an `ntdll.dll` export a stable native-PE contract. Native compatibility code may
issue the Wine information-class queries directly, but the numeric values,
WoW64 translation, probing, and error normalization remain private to the
adapter.

App-local candidate path construction:

- `DXMT9_WINEMETAL_SO`: explicit override, useful for debugging and package
  layouts that keep the provider outside the executable directory.
- Module directory: `GetModuleFileNameW(winemetal_dxmt9.dll)` with the file name
  replaced by `winemetal_dxmt9.so`.
- Executable directory: `GetModuleFileNameW(NULL)` with the file name replaced
  by `winemetal_dxmt9.so`.
- By-name fallback: `winemetal_dxmt9.so`, only when
  `DXMT9_ALLOW_RUNTIME_PROVIDER_FALLBACK=1` is set.

When a Windows path is used for an explicit local file, convert it to the NT
path form accepted by Wine's unixlib loader before calling
the selected `WineUnixlibLoader` adapter.

---

## 7. Capability Composition

Provider loading and window-system integration are orthogonal compatibility
axes:

| Axis | Initial values | Proves |
|---|---|---|
| `unixlib_loader_capabilities` | `builtin-pair-v1`, `by-name-v1` | The qualified unix provider can be found and called |
| `metal_surface_protocol` | `extescape-v1`, `legacy-macdrv-symbols:<runtime-id>`, `unsupported` | A Wine-owned window surface and Metal layer can be acquired and released |

A supported deployment is the intersection of one deployment-mode loader
requirement and one qualified Metal-surface protocol:

```text
runtime install   = builtin-pair-v1 AND qualified metal_surface_protocol
app-local install = by-name-v1      AND qualified metal_surface_protocol
```

This prevents two recurring false conclusions:

- loading `winemetal_dxmt9.so` on a new stock Wine does not prove that stock
  Wine exposes a supported Metal surface;
- finding legacy macdrv symbols does not prove that an app-local provider can
  be loaded or that the private ABI is qualified.

The package manifest declares required capabilities. The runtime probe records
observed capabilities and the selected acquisition path. A Wine version is
diagnostic provenance, not the compatibility decision key.

### Compatibility Classes

| Runtime class | Loader result | Surface result | Support result |
|---|---|---|---|
| Stock Wine with by-name unixlib loading but no accepted Metal-surface protocol | pass | unsupported | fail closed at WSI creation |
| Wine carrying the accepted ExtEscape protocol | mode-dependent pass | `extescape-v1` after runtime query | supported after integration gates |
| Exact audited legacy runtime | usually `builtin-pair-v1`; app-local only if separately proven | `legacy-macdrv-symbols:<runtime-id>` | supported only for the evidenced deployment modes |
| Unknown downstream Wine | probe | probe | unsupported until both axes are qualified |

---

## 8. Package Manifest

The root mixed-architecture package step should emit `dxmt9-deploy.json`:

```json
{
  "schema": 2,
  "mode": "app-local",
  "version": "0.1.0",
  "bridge_abi_hash": "<hex>",
  "provider_schema": "dxmt9-winemetal-v1",
  "required_capabilities": {
    "unixlib_loader": "by-name-v1",
    "metal_surface_protocol": "extescape-v1"
  },
  "d3d9_export_profile": "windows-d3d9-by-wine-tests",
  "has_wow64_unix_call_table": true,
  "variants": [
    {
      "name": "x64-on-x86_64-unix",
      "pe_arch": "x86_64-windows",
      "unix_arch": "x86_64-unix",
      "artifacts": [
        { "path": "pe/x64/d3d9.dll", "sha256": "<hex>" },
        { "path": "pe/x64/winemetal_dxmt9.dll", "sha256": "<hex>" },
        { "path": "pe/x64/libc++.dll", "sha256": "<hex>" },
        { "path": "pe/x64/libunwind.dll", "sha256": "<hex>" },
        { "path": "unix/x86_64-unix/winemetal_dxmt9.so", "sha256": "<hex>" }
      ],
      "pe_dependencies": [
        "libc++.dll",
        "libunwind.dll"
      ],
      "unix_dependencies": [
        { "name": "winemac.so", "source": "wine-runtime" },
        { "name": "ntdll.so", "source": "wine-runtime" }
      ]
    },
    {
      "name": "x86-wow64-on-x86_64-unix",
      "pe_arch": "i386-windows",
      "unix_arch": "x86_64-unix",
      "artifacts": [
        { "path": "pe/x86/d3d9.dll", "sha256": "<hex>" },
        { "path": "pe/x86/winemetal_dxmt9.dll", "sha256": "<hex>" },
        { "path": "pe/x86/libc++.dll", "sha256": "<hex>" },
        { "path": "pe/x86/libunwind.dll", "sha256": "<hex>" },
        { "path": "unix/x86_64-unix/winemetal_dxmt9.so", "sha256": "<hex>" }
      ],
      "pe_dependencies": [
        "libc++.dll",
        "libunwind.dll"
      ],
      "unix_dependencies": [
        { "name": "winemac.so", "source": "wine-runtime" },
        { "name": "ntdll.so", "source": "wine-runtime" }
      ]
    }
  ]
}
```

The final flat staging directory for one executable may also contain a reduced
manifest with one selected variant, but that file must identify itself as a
single staged package, for example with `"mode": "app-local-staged"`. The
runtime installer can use the same schema with `"mode": "wine-runtime"` and
runtime destination paths.

For an exact legacy package, `metal_surface_protocol` contains the qualified
value `legacy-macdrv-symbols:<runtime-id>`. An unqualified
`legacy-macdrv-symbols` value is invalid. The package manifest records required
capabilities; a run result separately records observed loader capabilities,
the observed WSI acquisition path, and exact Wine provenance.

Every staged binary, including optional PE runtime dependency DLLs, must appear
in `artifacts` with a checksum. The `pe_dependencies` list remains the compact
copy list for final staging. The `d3d9_export_profile` field records the export
table contract validated for the packaged `d3d9.dll`, including factory entries,
`Direct3DShaderValidatorCreate9`, Windows D3D9-compatible `D3DPERF_*`,
`DebugSetMute`, and loader-safe auxiliary stubs such as
`Direct3DCreate9On12`.

---

## 9. Failure Behavior

Provider discovery failures must be actionable. At debug log level,
`winemetal_dxmt9.dll` should print:

- whether builtin lookup was attempted;
- the loader capabilities observed by the cold adapter;
- every provider candidate path;
- the `NTSTATUS` returned for each candidate;
- the selected provider path and unix-call handle on success;
- the unix-side dependencies recorded for `winemetal_dxmt9.so` and whether they were
  resolved from the package or target Wine runtime;
- whether a dynamically resolved bridge dependency was missing before provider
  discovery began.

Provider-load failure and Metal-surface acquisition failure are different
stages and must have different machine-readable dispositions. In particular, a
stock Wine runtime may load `winemetal_dxmt9.so` successfully and then reject
windowed device creation because no qualified surface protocol exists. That is
a WSI compatibility result, not a loader result.

The D3D9 frontend should translate bridge initialization failure into a normal
D3D9 creation failure. App-local `d3d9.dll` therefore uses dynamic bridge
resolution so missing `winemetal_dxmt9.dll` and missing `winemetal_dxmt9.so` can be reported
from `Direct3DCreate9`. Missing dependencies of `d3d9.dll` itself are still
Windows loader failures and must be caught by packaging validation.

---

## 10. Verification Design

Runtime lane:

```sh
bash scripts/install/install_heroic_wine.sh --prefix "$PREFIX" --wine-root "$WINE_ROOT"
WINEPREFIX="$PREFIX" WINEDLLOVERRIDES="d3d9=b" "$WINE_ROOT/bin/wine" d3d9-smoke.exe
```

App-local lane:

```sh
tmp=$(mktemp -d)
cp d3d9-smoke.exe "$tmp/"
cp package/pe/x64/d3d9.dll "$tmp/"
cp package/pe/x64/winemetal_dxmt9.dll "$tmp/"
cp package/unix/x86_64-unix/winemetal_dxmt9.so "$tmp/"
for dep in libc++.dll libunwind.dll; do
  if [ -f "package/pe/x64/$dep" ]; then
    cp "package/pe/x64/$dep" "$tmp/"
  fi
done

WINEPREFIX="$TEMP_PREFIX" DXMT_LOG_LEVEL=debug "$WINE_ROOT/bin/wine" "$tmp/d3d9-smoke.exe"
```

The dependency copy loop represents the selected manifest variant's
`pe_dependencies`. A package that statically links those dependencies should
have an empty `pe_dependencies` list and the loop copies nothing.

Expected debug evidence:

- Wine loaded `d3d9.dll` from the smoke executable directory.
- The staged `d3d9.dll` export table matches the manifest's
  `d3d9_export_profile`.
- `d3d9.dll` loaded `winemetal_dxmt9.dll` from the `d3d9.dll` sibling absolute path.
- `winemetal_dxmt9.dll` selected the app-local `winemetal_dxmt9.so` candidate.
- `winemetal_dxmt9.so` resolved Wine runtime dependencies such as `winemac.so` and
  `ntdll.so` from the target Wine runtime under test.
- `Direct3DCreate9` returned a dxmt9-backed factory and a device can present a
  frame.

Negative app-local test:

```sh
rm "$tmp/winemetal_dxmt9.so"
WINEPREFIX="$TEMP_PREFIX" \
DXMT_LOG_LEVEL=debug \
DXMT9_ALLOW_RUNTIME_PROVIDER_FALLBACK=0 \
"$WINE_ROOT/bin/wine" "$tmp/d3d9-smoke.exe"
```

Expected result: `Direct3DCreate9` fails cleanly and logs that no candidate
provider was available.

Capability-composition negative test:

1. Use a stock Wine runtime that exposes `by-name-v1` but does not expose an
   accepted Metal-surface protocol.
2. Keep `winemetal_dxmt9.so` present and require its ABI handshake to pass.
3. Attempt windowed device or swap-chain creation.

Expected result: the run records successful provider loading,
`layer_acquisition=unavailable`, and a WSI creation failure. It must not classify
the run as a unixlib-loader failure or accept a black/no-op window.

Coexistence verification snapshots the checksums of upstream DXMT's
`winemetal.dll` and `winemetal.so`, installs dxmt9, and confirms that both
checksums are unchanged. It then records the loaded paths and independent ABI
handshakes for DXMT D3D10/11 and dxmt9 D3D9, first sequentially in one prefix
and then in one process when the combined fixture is available.
