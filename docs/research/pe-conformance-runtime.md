# PE Conformance Runtime Evidence

This note records runtime evidence for the Wine-derived D3D9 PE conformance
suite. It is intentionally separate from `tests/d3d9_conformance/MANIFEST.toml`:
the manifest has one status field per case, while runtime evidence can differ by
lane, architecture, Wine runtime, and staged artifact set.

## 2026-05-04 x64 App-Local Attempt

Artifacts:

- PE tests: `build-win32-x64/tests/d3d9_conformance/`
- Unix provider: `build-x86_64-builtin/src/winemetal/unix/winemetal.so`
- Wine runtime: Heroic Wine 11.6
- Prefix: `tmp/pe-conformance-prefix-app-local-x64`
- Lane: app-local x64

Command shape:

```sh
env \
  WINEPREFIX=/Users/dididi/workspaces/dxmt9/tmp/pe-conformance-prefix-app-local-x64 \
  WINEDEBUG=-all \
  WINEDLLOVERRIDES=d3d9=n,b \
  DXMT9_WINEMETAL_SO=/Users/dididi/workspaces/dxmt9/build-x86_64-builtin/src/winemetal/unix/winemetal.so \
  MVK_CONFIG_LOG_LEVEL=0 \
  "$HOME/Library/Application Support/heroic/tools/wine/Wine-11.6/Contents/Resources/wine/bin/wine" \
  ./<test-exe>
```

Results:

| Executable | Result | Meaning |
|---|---|---|
| `d3d9_exports_x64.exe` | exit `1` | Missing `Direct3DCreate9On12`, `D3DPERF_*`, and `DebugSetMute` exports |
| `d3d9_auxiliary_x64.exe` | exit `1` | Missing `Direct3DCreate9On12` export |
| `dxmt9-d3d9-conformance.exe` | exit `0`, but `SUMMARY checks=0 failures=0 skips=14` | Not passing evidence; all cases skipped because `Direct3DCreate9` returned null or `Direct3DCreate9Ex` failed with `0x8007000e` |
| `dxmt9-d3d9-device-lifetime.exe` | hung | Killed after waiting with no stdout |
| `d3d9_queries_x64.exe` | timeout | Timed out after 20 seconds with no stdout |

Conclusion:

- No manifest row should be promoted to `passing` from this attempt.
- The result is useful blocker evidence: export coverage, factory/device
  creation, and query/lifetime runtime behavior must be fixed before scaffolded
  cases can become runtime evidence.
- The builtin lane was not run in this attempt because freshly built builtin
  artifacts did not match the currently installed Heroic DXMT Wine runtime
  hashes, and staging them would overwrite the local runtime.

## 2026-05-05 Export Pass and Provider-Loader Diagnosis

Artifacts:

- PE tests: `build-win32-x64/tests/d3d9_conformance/`
- PE `d3d9.dll`: `build-win32-x64/src/win32/d3d9.dll`
- PE `winemetal.dll`: `build-win32-x64/src/winemetal/winemetal.dll`
- Unix provider: `build-x86_64-builtin/src/winemetal/unix/winemetal.so`
- Wine runtime: Heroic Wine 11.6 / Wine-11.6-DXMT lanes used during local
  diagnosis

Export/auxiliary result:

| Executable | Result | Meaning |
|---|---|---|
| `d3d9_exports_x64.exe` | exit `0` | `Direct3DCreate9On12`, `D3DPERF_*`, and `DebugSetMute` are now exported and callable in the focused app-local lane |
| `d3d9_auxiliary_x64.exe` | exit `0` | Shader-validator stub and loader-safe `Direct3DCreate9On12` path pass the focused app-local probe |

The previous `Direct3DCreate9 == NULL` / `Direct3DCreate9Ex == 0x8007000e`
failure was isolated to provider loading before native factory creation:

1. With only `WINEDLLOVERRIDES=d3d9=n,b`, Wine can load an installed builtin
   `winemetal.dll` instead of the adjacent current app-local bridge. That old
   bridge reports `dispatcher-only fallback` and never reaches the provider.
2. With `WINEDLLOVERRIDES=d3d9,winemetal=n,b`, the current bridge is selected,
   but `MemoryWineLoadUnixLibByName` still fails with `0xc0000135` if the
   Mach-O loader cannot resolve `winemac.so` and `ntdll.so`.
3. Adding Wine's unix library directory to `DYLD_LIBRARY_PATH` was not reliable
   enough by itself. The durable fix is to rewrite `winemetal.so`'s Mach-O
   dependency install names from bare `winemac.so` / `ntdll.so` to
   `@rpath/winemac.so` / `@rpath/ntdll.so` and give the build artifact a Wine
   unix-lib rpath. Installed runtime artifacts can then resolve the same names
   through `@loader_path`.

Launcher change:

- `experiments/launchers/common.sh` now defaults to
  `WINEDLLOVERRIDES=d3d9,winemetal=n,b`.
- The same launcher injects `DXMT9_WINEMETAL_SO` from
  `DXMT_EXPERIMENT_UNIX_BUILD_DIR/winemetal/unix/winemetal.so` when present.
- If `DXMT_EXPERIMENT_WINE_ROOT` is known, the launcher prepends the matching
  `lib/wine/*-unix` directory to `DYLD_LIBRARY_PATH` so app-local provider
  loading no longer depends on an out-of-band shell export.
- `src/winemetal/unix/meson.build` now runs a Darwin install-name fixup for
  `winemetal.so`, converting Wine unix dependencies to `@rpath` and adding the
  Wine unix lib directory to the build rpath.

With the fixed provider artifact, the base device-backed app-local executable no
longer skips factory/device cases:

| Executable | Result | Meaning |
|---|---|---|
| `dxmt9-d3d9-conformance.exe` | `SUMMARY checks=328 failures=26 skips=0` | Provider loading is fixed for this lane; remaining issues are implementation conformance failures, not bridge bootstrap failures |

Failing groups from this run:

- `factory_validation_return_codes`
- `present_parameter_validation`
- `ex_create_reset_mode_validation`
- `private_data_resource_wrappers`
- `ex_shared_handle_policy`
- `creation_failure_out_pointers`

No device-backed manifest row is promoted by this note. It closes the
focused export/auxiliary blocker and moves the base app-local lane from
loader-failure evidence to concrete failing-case evidence.

## Next Acceptance Step

1. Fix or intentionally classify the six failing base app-local conformance
   groups listed above.
2. Run the remaining app-local x64 executables, especially lifetime, query,
   resource, stateblock, reset/lost, and window/cursor, and record exact stdout,
   exit status, Wine runtime, and artifact hashes for every executable.
3. Stage a deliberate builtin lane in an isolated Wine runtime or prefix, then
   run the same executable set before changing manifest statuses.
