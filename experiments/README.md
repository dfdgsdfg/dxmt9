# Experiments

Experiments are end-to-end runs against real D3D9 applications under the
current `dxmt9` runtime.

## Directory layout

| Subdir | Committed? | Purpose |
|---|---|---|
| `apps/` | yes | Small fixture EXEs (D9VK, sample-d3d9-basic-hlsl). |
| `apps_3rd/` | no | External installs (SFIV, etc.). |
| `prefixs/` | no | Per-experiment Wine prefixes. |
| `wine/` | mixed | `manifest.toml` + README committed; bundles ignored. |
| `launchers/` | yes | Per-app launcher scripts. |
| `output/` | partial (`.gitkeep` only) | Run results. |
| `references/` | yes | Reference screenshots. |
| `CATALOGUE.toml` | yes | Manifest of every experiment. |

Spec: `specs/experiments/runtime/{requirements,spec}.md`.

Primary entrypoint:

```sh
scripts/run_python.sh scripts/run_apps/run_experiment.py list
scripts/run_python.sh scripts/run_apps/run_experiment.py run conf-d3d9-wsi-present
scripts/run_python.sh scripts/run_apps/run_experiment.py run sample-d3d9-basic-hlsl --wine-root "$WINE_ROOT"
scripts/run_python.sh scripts/run_apps/run_experiment.py run sample-d3d9-tutorial07 --wine-root "$WINE_ROOT"
scripts/run_python.sh scripts/run_apps/run_experiment.py run sample-d3d9-hdr-formats --wine-root "$WINE_ROOT"
scripts/run_python.sh scripts/run_apps/run_experiment.py run sample-d3d9-dxut-simple --wine-root "$WINE_ROOT"
scripts/run_python.sh scripts/run_apps/run_experiment.py run sample-d3d9-irrlicht-lights --wine-root "$WINE_ROOT"
scripts/run_python.sh scripts/run_apps/run_experiment.py run app-d3d9-3dmark06 --wine-root "$WINE_ROOT"
scripts/run_python.sh scripts/run_apps/run_experiment.py run app-d3d9-sfiv-benchmark --wine-root "$WINE_ROOT" --binary "/path/to/StreetFighterIV_Benchmark.exe"
```

DX9 regression suite:

```sh
bash scripts/run_suites/run_dx9_regression_suite.sh --wine-root "$WINE_ROOT"
```

DX9 fast sanity suite derived from the small `d9vk` D3D9 tests:

```sh
bash scripts/run_suites/run_dx9_fast_sanity_suite.sh --wine-root "$WINE_ROOT"
```

This suite:

- cross-builds the repo-local Win32 sanity apps
- runs them across `dxmt9-x64`, `builtin-x64`, and `builtin-x86`
- uses self-validating app exits plus captured frames, not builtin-oracle SSIM
- writes `experiments/output/dx9-fast-sanity/summary.json` and `summary.md`

Builtin-oracle compare suite for selected DX9 sample apps:

```sh
bash scripts/run_suites/run_dx9_oracle_compare_suite.sh --wine-root "$WINE_ROOT"
```

Stale temp-prefix cleanup for interrupted suite runs:

```sh
scripts/run_python.sh scripts/tools/cleanup_dxmt9_temp_prefixes.py --dry-run
scripts/run_python.sh scripts/tools/cleanup_dxmt9_temp_prefixes.py --all
```

Notes:

- `run_experiment.py` now cleans auto-created temp prefixes on normal exit and on
  `SIGINT`/`SIGTERM`/`SIGHUP`
- auto-created temp prefixes now live under `tmp/prefixes` in the repo root by
  default
- oracle/regression suite wrappers run stale temp-prefix cleanup automatically
  before starting
- use the cleanup script when earlier runs were killed hard or the machine was
  interrupted and old `dxmt9-exp-*` prefixes remain under that temp root

Build-then-run via the consolidated runner. Pass `--build` to invoke the
app's `build_script` (declared in `CATALOGUE.toml`) before launching:

```sh
scripts/run_python.sh scripts/run_apps/run_experiment.py run sample-d3d9-basic-hlsl --build --wine-root "$WINE_ROOT"
scripts/run_python.sh scripts/run_apps/run_experiment.py run sample-d3d9-tutorial07 --build --wine-root "$WINE_ROOT"
scripts/run_python.sh scripts/run_apps/run_experiment.py run sample-d3d9-hdr-formats --build --wine-root "$WINE_ROOT"
scripts/run_python.sh scripts/run_apps/run_experiment.py run sample-d3d9-dxut-simple --build --wine-root "$WINE_ROOT"
scripts/run_python.sh scripts/run_apps/run_experiment.py run sample-d3d9-irrlicht-lights --build --wine-root "$WINE_ROOT"
```

Wrappers that still need extra setup (default-prefix injection or installer
extraction) remain as shell scripts:

```sh
scripts/run_python.sh scripts/run_apps/run_experiment.py run app-d3d9-sfiv-benchmark --binary "~/Downloads/StreetFighterIV_Benchmark.exe"
```

3DMark06 is commercial and is not vendored. Place a complete installed payload
with `3DMark06.exe` at
`experiments/apps_3rd/app-d3d9-3dmark06/`, or pass its absolute POSIX path with
`--binary`. The first runtime qualification must verify that the installed
edition accepts the per-test command-line switches; those switches are normally
a Professional Edition feature. Examples:

```sh
DXMT_3DMARK06_LANE=gt1 \
DXMT_3DMARK06_RESULT_FILE=dxmt9_gt1.3dr \
  scripts/run_python.sh scripts/run_apps/run_experiment.py run app-d3d9-3dmark06

DXMT_3DMARK06_LANE=hdr1 \
DXMT_3DMARK06_RESULT_FILE=dxmt9_hdr1.3dr \
  scripts/run_python.sh scripts/run_apps/run_experiment.py run app-d3d9-3dmark06

DXMT_3DMARK06_LANE=cpu \
  scripts/run_python.sh scripts/run_apps/run_experiment.py run app-d3d9-3dmark06
```

3DMark05 uses the same convention through `DXMT_3DMARK05_LANE`. Named results
are preserved in `result.json:benchmark_lane`; reserve the product-specific
`*_ARGS` variables for custom selections that have no canonical preset.
Catalogue runs automatically supply a unique product-specific `.3dr` result
basename when `DXMT_3DMARK*_RESULT_FILE` is unset. Newly created or overwritten
regular result files are copied atomically to `<output>/benchmark-results/` and
listed with their source and digest in `result.json:benchmark_result_files`.
Editions that do not emit a file record `status=not_emitted`; stale files in the
benchmark directory are not reused.

Permanent-prefix installer for Heroic:

```sh
bash scripts/install/install_heroic_experiment_prefix.sh --prefix "$HOME/.wine-dxmt9-heroic" --wine-root "$WINE_ROOT"
```

The runner:

- reads [`CATALOGUE.toml`](./CATALOGUE.toml)
- stages `d3d9.dll`, `winemetal_dxmt9.dll`, and `winemetal_dxmt9.so` into a Wine runtime/prefix
- runs the selected launcher
- captures the presented back buffer directly from dxmt9 when `capture_frame` is set
- falls back to window capture only when an internal frame dump is unavailable
- writes `actual.png`, `diff.png`, `ssim.txt`, `dxmt9.log`, and `result.json`

Current note:

- the committed sample references are stale for several shader apps
- use the builtin-oracle compare suite to judge current renderer parity
- current builtin-vs-dxmt9 sample parity:
  - `sample-d3d9-basic-hlsl`: `0.9539`
  - `sample-d3d9-tutorial07`: `0.9094`
  - `sample-d3d9-dxut-simple`: `0.9426`
  - `sample-d3d9-irrlicht-lights`: `0.9979`

Current verified bootstrap entry:

- `conf-d3d9-wsi-present`

This is not a catalogue target from the experiments spec. It exists to validate
the launcher, frame-dump capture, and SSIM workflow locally before external
sample binaries are staged.

Current verified real application entry:

- `sample-d3d9-basic-hlsl`
  - Heroic Wine 11.6 builtin path
  - 240 frames
  - direct backbuffer capture
  - `ssim = 1.0000`
- `sample-d3d9-tutorial07`
  - Heroic Wine 11.6 builtin path
  - 180 frames
  - direct backbuffer capture
  - `ssim = 1.0000`
- `sample-d3d9-dxut-simple`
  - Heroic Wine 11.6 builtin path
  - 180 frames
  - direct backbuffer capture
  - `ssim = 1.0000`
- `sample-d3d9-hdr-formats`
  - Heroic Wine 11.6 builtin path
  - 180 frames
  - direct backbuffer capture
  - `ssim = 1.0000`
- `sample-d3d9-irrlicht-lights`
  - Heroic Wine 11.6 builtin path
  - 180 frames
  - direct backbuffer capture
  - `ssim = 1.0000`

Current verified host:

- Heroic Wine 11.6 builtin path

Current exploratory feature targets:

- `conf-d3d9-srgb-texture`
- `conf-d3d9-float-texture`
- `conf-d3d9-stream-source`
- `conf-d3d9-shademode-provoking`
- `conf-d3d9-pointsize`
- `conf-d3d9-yuv-format`
- `conf-d3d9-vendor-format`

conf-d3d9-intent-probe notes:

- These modes map to Wine `dlls/d3d9/tests/visual.c` behavior.
- Current tree only has the ignored local `conf-d3d9-intent-probe.exe`; the
  corresponding source is absent from `experiments/apps/conf-d3d9-intent-probe/`.
- Catalogue rows and launchers are kept as exploratory scaffolding while the
  source/build lane is restored or replaced by a clean project-owned probe.

- `sample-d3d9-water-rt`
  - repo-local DX9 sample
  - render-to-texture + projected UV + alpha blend
  - intended as the first focused proxy for render-to-texture water rendering bugs
  - builtin-oracle compare now matches and can be used as a regression gate
- `sample-d3d9-multitexture-terrain`
  - repo-local DX9 sample
  - multi-sampler terrain material blend
  - intended as the second focused proxy for outdoor terrain/material bugs
  - builtin-oracle compare currently matches and can be used as a regression gate
- `perf-d3d9-present-only`
  - repo-local DX9 micro-benchmark
  - clear + immediate present, no draw stress
  - intended to isolate present/compositor pacing against Wine builtin D3D9
- `perf-d3d9-offscreen-heavy`
  - repo-local DX9 micro-benchmark
  - many fixed-function draws into an offscreen render target, no per-frame present
  - intended to isolate draw/encode throughput without CAMetalLayer pacing
- `perf-d3d9-many-draw`
  - repo-local DX9 micro-benchmark
  - many fixed-function draw calls followed by immediate present
  - intended to expose whether draw-call submission or present pacing dominates

Current exploratory commercial-oracle candidate:

- `app-d3d9-sfiv-benchmark`
  - Heroic `11.6-DXMT` research lane for `dxmt9`
  - keep the Heroic prefix fresh; if the benchmark starts failing after local DLL
    or `winetricks` experiments, recreate the prefix instead of trying to
    salvage it
  - wrapper now installs prefix-native `d3dx9_41` and mirrors the 32-bit DLL next to the extracted benchmark binary
