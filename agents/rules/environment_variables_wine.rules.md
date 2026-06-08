# dxmt9 Environment Variables — Cross-process / Wine / Apple-side

Part of the [`environment_variables.rules.md`](environment_variables.rules.md)
index (Wine, PE/unix bridge, repository experiment harnesses, and macOS / Metal
toolchain knobs). A flag is "set" when its value is a non-empty string that is
not `0`, unless documented otherwise. See the index for global notes.

## Cross-process / Wine

These are used by Wine, the PE/unix bridge, or repository experiment
harnesses:

| Var | Purpose |
|---|---|
| `DXMT9_WINEMETAL_SO` | Explicit winemetal.so provider path for app-local bridge loading |
| `DXMT9_ALLOW_RUNTIME_PROVIDER_FALLBACK` | Allow legacy runtime-by-name provider fallback |
| `DXMT_EXPERIMENT_WINE_DLLOVERRIDES` | Wine `WINEDLLOVERRIDES` snippet for the experiment harness |
| `DXMT_EXPERIMENT_WINE_ID` | Select a Wine manifest entry for experiment runs |
| `DXMT_EXPERIMENT_CX_BOTTLE` | Select a CrossOver bottle for experiment launchers |
| `DXMT_EXPERIMENT_PROFILE` | Select the experiment runtime profile (`debug` / `perf`); `perf` selects the shared GT1 perf-path runtime defaults. Falls back to `DXMT_PROFILE`, then `debug` |
| `DXMT_3DMARK05_DIRECT` | Use the direct-prefix 3DMark05 launcher path instead of the supervised `run_experiment.py` flow |
| `DXMT_3DMARK05_LOG` | Log path for the direct 3DMark05 launcher path |
| `DXMT_ASSERT` | Enable runtime assertions in experiment runs (also a log-failure marker scanned by mini-replay) |
| `DXMT9_CONFORMANCE_DLLOVERRIDES` / `DXMT9_CONFORMANCE_WINEMETAL_SO` | `run_d3d9_conformance.py` overrides for the Wine `WINEDLLOVERRIDES` snippet and the explicit `winemetal.so` provider path |
| `DXMT_EXPERIMENT_BINARY` / `DXMT_EXPERIMENT_LOG` / `DXMT_EXPERIMENT_NAME` / `DXMT_EXPERIMENT_OUTPUT_DIR` / `DXMT_EXPERIMENT_PREFIX` / `DXMT_EXPERIMENT_PE_BUILD_DIR` / `DXMT_EXPERIMENT_RUNTIME_PE_BUILD_DIR` / `DXMT_EXPERIMENT_UNIX_BUILD_DIR` / `DXMT_EXPERIMENT_WOW64_PE_BUILD_DIR` / `DXMT_EXPERIMENT_WOW64_RUNTIME_PE_BUILD_DIR` / `DXMT_EXPERIMENT_WINE_BIN` / `DXMT_EXPERIMENT_WINE_ROOT` / `DXMT_EXPERIMENT_SKIP_STAGE` | Internal `run_experiment.py` → launcher plumbing (app name/binary/prefix, staged PE/unix/wow64 build dirs, Wine bin/root, output/log paths, stage-skip flag). Set by the harness, not user knobs |
| `DXMT_VALIDATE` | Enable validation-layer setup in experiment launchers |
| `DXMT_UPSTREAM_ROOT` | Upstream shader-corpus checkout used by sync/drift tools |
| `DXMT_UPSTREAM_COMMIT` | Upstream shader-corpus commit override used by sync/drift tools |
| `DXMT_UPSTREAM_URL` | Upstream shader-corpus provenance URL override |
| `DXMT_ORACLE_DATE` | Shader-corpus oracle provenance date override |

## Apple-side (not honored by dxmt9 directly)

These are macOS / Metal toolchain knobs you may want set for a debug
session even though dxmt9 itself does not read them:

| Var | Purpose |
|---|---|
| `MTL_DEBUG_LAYER` | Enable Metal Validation Layer |
| `MTL_HUD_ENABLED` | Built-in Metal HUD overlay |
| `MTL_SHADER_VALIDATION` | Shader validation pass |
