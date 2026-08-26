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
| `DXMT9_WINEMETAL_SO` | Explicit `winemetal_dxmt9.so` provider path for app-local bridge loading |
| `DXMT9_ALLOW_RUNTIME_PROVIDER_FALLBACK` | Allow legacy runtime-by-name provider fallback |
| `DXMT9_WINE_METAL_SURFACE_PROTOCOL` / `DXMT9_WINE_MANIFEST_ID` | Harness-owned Wine WSI declaration and resolved manifest identity. Only `legacy-macdrv-symbols:<runtime-id>` whose suffix equals `DXMT9_WINE_MANIFEST_ID` qualifies the legacy aggregate-table fallback; `extescape-v1` remains gated by `QUERYESCSUPPORT`. The harness clears inherited values when no manifest entry is resolved |
| `DXMT_EXPERIMENT_WINE_DLLOVERRIDES` | Wine `WINEDLLOVERRIDES` snippet for the experiment harness |
| `DXMT_EXPERIMENT_WINE_ID` | Select a Wine manifest entry for experiment runs |
| `DXMT_EXPERIMENT_CX_BOTTLE` | Select a CrossOver bottle for experiment launchers |
| `DXMT_EXPERIMENT_PROFILE` | Select the experiment runtime profile (`debug` / `perf`); `perf` selects the shared GT1 perf-path runtime defaults, which since the H195 promotion proof include the `DXMT9_OFFLOAD_COMMIT_REPLAY=1` + `DXMT9_OPTIMIZE_OPAQUE_DEPTH_INDEX_CACHE=1` pair (caller env always overrides; `run_3dmark05_perf_probe.sh` pins both to explicit values for recipe determinism, defaulting to the engine default — on — since 2026-07-12; export `0` to opt out). Falls back to `DXMT_PROFILE`, then **`perf`** (default since 2026-07-29). `debug` is the explicit opt-in for diagnosing a wild failure: it enables the Metal validation layer (`DXMT_VALIDATE=1`) and debug-level logging with `WINEDEBUG` unset, which costs `3.2x` on `sampled_avg_fps` (SFIV: `13.5` vs `43.0`) and `5.3x` on the steady-body median (`11.3` vs `59.7`), with a `619 MB`-to-`1.1 GB` log against `22 MB` — the older "roughly 4x (`11.3` vs `43.0`)" line compared a median against an average and understated the median cost; see `agents/rules/test_wild.rules.md` — it is **not** a performance-measurement configuration. The resolved profile, its source variable, and the effective validate/log-level/WINEDEBUG/perf-counter values are recorded by the launcher as `[experiment] profile: ...` and land in `result.json:profile` plus the `3dmark05-perf-summary.md` header, so any measurement states the configuration it ran under |
| `DXMT_3DMARK05_DIRECT` | Use the direct-prefix 3DMark05 launcher path instead of the supervised `run_experiment.py` flow |
| `DXMT_3DMARK05_LOG` | Log path for the direct 3DMark05 launcher path |
| `DXMT_3DMARK06_LANE` | Canonical reusable 3DMark06 selection: `gt1`, `gt2`, `sm2`, `hdr1`, `hdr2`, `hdr`, `graphics`, `cpu1`, `cpu2`, `cpu`, `score`, `feature`, `batch`, or `all`. The launcher appends the standard headless flags and records the resolved identity in `result.json:benchmark_lane`. Per-test selection still requires edition qualification | `gt1` |
| `DXMT_3DMARK06_ARGS` / `DXMT_3DMARK06_RESULT_FILE` | Override the complete 3DMark06 argument string and optionally append the final positional `.3dr` result path. A raw override takes precedence over `DXMT_3DMARK06_LANE`, receives no automatic headless suffix, and is recorded as lane `custom`. An explicit result path is preserved; when unset for a catalogue run, `run_experiment.py` injects a unique basename and records/copies any created or modified regular `.3dr` under the run artifact |
| `DXMT_3DMARK06_REQUIRE_UNLOCKED` / `DXMT_3DMARK06_AUTO_ENTER` / `DXMT_3DMARK06_ENTER_DELAY_SEC` / `DXMT_3DMARK06_ENTER_COUNT` / `DXMT_3DMARK06_ENTER_INTERVAL_SEC` / `DXMT_3DMARK06_FOCUS_KEEPALIVE_SEC` | 3DMark06 GUI-session guard and optional foreground/Enter fallback. The unlocked-session guard defaults on; the unqualified synthetic Enter fallback defaults off and is enabled with `DXMT_3DMARK06_AUTO_ENTER=1` |
| `DXMT_3DMARK06_DRY_RUN` | Print the resolved 3DMark06 binary, working directory, arguments, and Wine command without staging or launching |
| `DXMT_ASSERT` | Enable runtime assertions in experiment runs (also a log-failure marker scanned by mini-replay) |
| `DXMT9_CONFORMANCE_DLLOVERRIDES` / `DXMT9_CONFORMANCE_WINEMETAL_SO` | `run_d3d9_conformance.py` overrides for the Wine `WINEDLLOVERRIDES` snippet and the explicit `winemetal_dxmt9.so` provider path |
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
