# Deployment Requirements

dxmt9 must support two deployment modes:

- **Wine runtime install:** the current dxmt-style installation where
  `d3d9.dll`, `winemetal.dll`, and `winemetal.so` are installed into a Wine
  runtime/prefix.
- **Application-local install:** a DXVK-style layout where placing dxmt9's
  `d3d9.dll` next to a game's `.exe` makes Wine load that DLL through normal
  native DLL search rules, without modifying the Wine runtime.

The two modes must ship the same D3D9 frontend and Metal provider behavior.
They differ only in how Wine discovers the PE DLLs and the unix provider.

---

## 1. Deployment Modes

**R-DEPLOY-1.1** Runtime install must continue to support the current artifact
set:

- `d3d9.dll` in the Wine Windows runtime directory for the target PE
  architecture, e.g. `lib/wine/x86_64-windows/` or `lib/wine/i386-windows/`.
- `winemetal.dll` in the matching Wine Windows runtime directory.
- `winemetal.so` in the Wine unix runtime directory, e.g.
  `lib/wine/x86_64-unix/` or `lib/wine/aarch64-unix/`.

**R-DEPLOY-1.2** Runtime install may also mirror `d3d9.dll` and
`winemetal.dll` into the prefix Windows directories (`system32` and, for
WoW64, `syswow64`) to match the existing installer workflow.

**R-DEPLOY-1.3** Runtime install must work with the builtin Wine load order,
for example `WINEDLLOVERRIDES="d3d9=b"`. It must not require files next to the
application executable.

**R-DEPLOY-1.4** Application-local install must work when the correct PE
architecture artifacts are placed in the same directory as the application
executable:

```text
Game.exe
d3d9.dll
winemetal.dll
winemetal.so
```

Additional PE runtime dependency DLLs, if any, must also be staged in this
directory or linked away.

**R-DEPLOY-1.5** Application-local install must rely on Wine's normal native
DLL search behavior for `d3d9.dll`. A user must not need to edit the Wine
runtime, copy files into `system32`, or set a builtin `d3d9` override for the
normal app-local path.

**R-DEPLOY-1.6** Application-local install must be scoped to the application
directory. If `d3d9.dll` is removed from that directory, the same Wine prefix
must fall back to its previous D3D9 behavior.

---

## 2. Artifact Requirements

**R-DEPLOY-2.1** The PE `d3d9.dll` must be built in two variants:

- a Wine-builtin variant suitable for runtime installation and `winebuild
  --builtin` post-processing;
- a native PE variant suitable for application-local loading.

**R-DEPLOY-2.2** Both PE `d3d9.dll` variants must export a Windows
D3D9-compatible entry-point surface, validated against Wine/Windows export
profiles. The required exports include:

- `Direct3DCreate9`, `Direct3DCreate9Ex`, and
  `Direct3DShaderValidatorCreate9`;
- `D3DPERF_BeginEvent`, `D3DPERF_EndEvent`, `D3DPERF_GetStatus`,
  `D3DPERF_QueryRepeatFrame`, `D3DPERF_SetMarker`, `D3DPERF_SetOptions`,
  and `D3DPERF_SetRegion`;
- `DebugSetMute`;
- a loader-safe `Direct3DCreate9On12` stub.

If the selected import-library/export-table compatibility profile includes
`PSGPError` or `PSGPSampleTexture`, those exports may be loader-safe stubs. A
missing auxiliary export is a deployment failure because statically importing
applications can fail during PE import resolution before `Direct3DCreate9` is
called.

**R-DEPLOY-2.3** The native PE `d3d9.dll` must not depend on Wine builtin module
metadata. It must be loadable as an ordinary PE DLL from the application
directory.

**R-DEPLOY-2.4** `winemetal.dll` must also be built in two variants:

- a Wine-builtin variant paired with runtime `winemetal.so`;
- a native PE variant that can be loaded from the application directory.

**R-DEPLOY-2.5** In application-local mode, `d3d9.dll` must resolve
`winemetal.dll` from the application-local directory by controlled dynamic
loading (`LoadLibrary` / `GetProcAddress`) or an equivalent delay-load path with
a failure hook. It must not statically import `winemetal.dll` in a way that
turns a missing bridge DLL into a process loader failure before
`Direct3DCreate9`. The app-local bridge DLL path must be derived from the
loaded `d3d9.dll` module path (`GetModuleFileNameW(d3d9_hmodule)`), not from
the process current working directory. The load must use an absolute sibling
path with `LoadLibraryExW` and flags that allow dependent PE DLLs to resolve
from that sibling directory, such as `LOAD_WITH_ALTERED_SEARCH_PATH` or
`LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR` plus default directories.

**R-DEPLOY-2.6** `winemetal.so` must remain a Wine unixlib provider exporting
`__wine_unix_call_funcs` and, where supported, `__wine_unix_call_wow64_funcs`.

**R-DEPLOY-2.7** The same unix provider must be usable by both the runtime
install path and the application-local path. Runtime install may locate it
through Wine builtin metadata. Application-local install must locate it from an
explicit path or package-local path by default.

**R-DEPLOY-2.8** The package must distinguish PE architecture from unix provider
architecture:

| Application | PE DLLs | Unix provider |
|---|---|---|
| 64-bit Windows process | `x86_64-windows` | host Wine unix arch |
| 32-bit Windows process under WoW64 | `i386-windows` | host Wine unix arch with wow64 unix-call table |

**R-DEPLOY-2.9** Any non-Wine PE runtime dependency of `d3d9.dll` or
`winemetal.dll` must be either statically linked or included in the deployment
manifest. Hidden dependencies on a local developer toolchain directory are not
allowed.

**R-DEPLOY-2.10** Any unix-side dynamic dependency of `winemetal.so` must be
declared in the deployment manifest. Wine runtime dependencies such as
`winemac.so` and `ntdll.so` may be provided by the target Wine runtime, but the
provider must not retain absolute references to a developer build tree that are
not valid on the target machine.

---

## 3. Unix Provider Discovery

**R-DEPLOY-3.1** The PE `winemetal.dll` bridge must support the current builtin
unixlib discovery path for runtime install.

**R-DEPLOY-3.2** The PE `winemetal.dll` bridge must support application-local
unix provider discovery. It must not rely solely on the Wine builtin module's
stored unixlib path.

**R-DEPLOY-3.3** Runtime install and application-local install must use separate
locator modes. The builtin bridge mode may try Wine builtin unixlib metadata
first. The native app-local bridge mode must not try builtin unixlib metadata
before package-local candidates.

**R-DEPLOY-3.4** Application-local unix provider discovery must use a stable
default search order:

1. An explicit override path from `DXMT9_WINEMETAL_SO`.
2. `winemetal.so` next to the loaded `winemetal.dll`.
3. `winemetal.so` next to the process executable.

**R-DEPLOY-3.5** Wine runtime provider fallback by unixlib name is allowed only
when explicitly enabled, for example with
`DXMT9_ALLOW_RUNTIME_PROVIDER_FALLBACK=1`. In app-local mode this fallback must
run after all package-local candidates fail and must be logged as a fallback.

**R-DEPLOY-3.6** When a candidate `winemetal.so` is found, the bridge must load
it through Wine's unixlib loader, not by directly calling provider functions
from PE code.

**R-DEPLOY-3.7** The bridge must cache the loaded unixlib module and call handle
for the process lifetime. Every generated `dxmt9c_*` client call must dispatch
through that handle.

**R-DEPLOY-3.8** If no usable `winemetal.so` can be found, D3D9 creation must
fail cleanly with diagnostic logging that includes the candidate paths and
Wine status codes.

---

## 4. Loader and Override Behavior

**R-DEPLOY-4.1** Application-local mode must not require
`WINEDLLOVERRIDES="d3d9=b"`, because that would force Wine's builtin `d3d9`
path instead of the application-local native DLL.

**R-DEPLOY-4.2** `WINEDLLOVERRIDES="d3d9=n,b"` may be documented as a
diagnostic or compatibility override, but it must not be required for the
normal application-local path.

**R-DEPLOY-4.3** Runtime install and application-local install must be mutually
safe. An application-local `d3d9.dll` must take precedence for that one
application, while other applications in the same prefix continue using the
runtime-installed or Wine-provided D3D9 path.

**R-DEPLOY-4.4** The package must not require users to rename the D3D9 entry
DLL. The user-facing file name is always `d3d9.dll`.

---

## 5. Packaging and Installation

**R-DEPLOY-5.1** The build must produce packageable app-local artifacts with PE
architecture and unix provider architecture represented separately. A flat
directory is acceptable only for a final single-application staging directory.
Mixed-architecture distribution packages must keep PE DLLs and unix providers
in separate architecture-keyed paths, for example `pe/x64/`, `pe/x86/`, and
`unix/x86_64-unix/`.

**R-DEPLOY-5.2** The runtime installer must remain idempotent. Re-running it
with the same artifacts must not corrupt the Wine runtime or prefix.

**R-DEPLOY-5.3** Installers must preserve the current backup behavior for files
they overwrite in a Wine runtime or prefix.

**R-DEPLOY-5.4** A deployment manifest must record:

- artifact names and architectures;
- source build directory or version string;
- checksum of each staged binary;
- bridge ABI hash shared by `d3d9.dll`, `winemetal.dll`, and `winemetal.so`;
- provider schema/version identifier;
- whether `__wine_unix_call_wow64_funcs` is present;
- minimum supported Wine version or unixlib feature level;
- required Wine unix architecture;
- required PE runtime dependency DLLs;
- required unix-side dependencies and whether they are packaged or expected
  from the target Wine runtime.

---

## 6. Verification

**R-DEPLOY-6.1** Runtime install verification must run at least one D3D9 smoke
application through the builtin runtime path and confirm that
`Direct3DCreate9` returns a dxmt9-backed object.

**R-DEPLOY-6.2** Application-local verification must use a temporary prefix with
no dxmt9 runtime installation, stage the app-local artifacts next to a D3D9
smoke executable, and run without a builtin `d3d9` override.

**R-DEPLOY-6.3** Application-local verification must prove that the loaded
`d3d9.dll`, `winemetal.dll`, and `winemetal.so` came from the intended
application-local package.

**R-DEPLOY-6.4** Missing-provider verification must remove `winemetal.so` from
the app-local package and run with runtime provider fallback disabled. The
result must be an explicit diagnostic failure, not a crash or silent fallback
to an unrelated provider.

**R-DEPLOY-6.5** WoW64 verification must run a 32-bit D3D9 smoke executable
with the 32-bit PE app-local DLLs and the host unix provider when the target
Wine runtime supports WoW64.

**R-DEPLOY-6.6** Application-local verification must confirm that
`winemetal.so` resolves its unix-side dependencies from the intended package or
target Wine runtime. It must not resolve `winemac.so`, `ntdll.so`, or equivalent
Wine runtime libraries from a developer build directory unless that build
directory is explicitly the target Wine runtime under test.
