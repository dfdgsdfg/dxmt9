# experiments/launchers

Per-app or synthetic-probe shell launchers invoked by
`scripts/run_apps/run_experiment.py` per `experiments/CATALOGUE.toml`'s
`launcher` field. Filenames match `CATALOGUE.name` (kebab-case) so
`grep <app>` finds every artifact.

## Shared helpers (no CATALOGUE entry)

- `common.sh` — sourced by every launcher; provides `exp_stage_dxmt9`,
  `exp_run_wine_binary`, prefix discovery, capture/log hooks.
- `dx9_fast_sanity.sh` — bundle launcher for the 5 d9vk fast-sanity apps
  (`d9vk-d3d9-clear`, `d9vk-d3d9-buffer`, etc.), shared rather than
  per-app.

## App-bound launchers (1 per CATALOGUE entry)

D3D9 SDK / DXUT / third-party samples:

- `dx-sdk-basichlsl.sh` — DX SDK BasicHLSL.
- `dx-sdk-tutorial07.sh` — DX SDK Tutorial07 (skinned mesh).
- `dx-sdk-hdrformats.sh` — DX SDK HDRFormats (FP16/FP32 RT).
- `dxut-simple-sample.sh` — DXUT framework simple sample.
- `irrlicht-managed-lights.sh` — Irrlicht engine managed-lights demo.

Self-authored apps:

- `dxmt9-multitexture-terrain.sh` — multitexture terrain demo.
- `dxmt9-water-rt.sh` — water render-target / refraction demo.
- `dxmt9-wsi-present-local.sh` — minimal WSI present smoke (CI bootstrap).

Commercial / 3rd-party titles (require external prefix):

- `anno-1404-gold.sh` — Anno 1404 Gold (Heroic prefix).
- `street-fighter-iv-benchmark.sh` — SFIV benchmark (Heroic + CrossOver
  oracle lanes — see `scripts/run_apps/run_sfiv_benchmark_experiment.sh`
  for installer-extraction wrapper).

## Synthetic perf probes (no app source — shared probe binary, parameter-driven)

- `dxmt9-perf-bridge-empty.sh` — bridge round-trip baseline (no draw work).
- `dxmt9-perf-chain-parametric.sh` — parametric chain length probe.
- `dxmt9-perf-depth-heavy.sh` — depth/Z heavy workload.
- `dxmt9-perf-encode-replay.sh` — encode-then-replay throughput.
- `dxmt9-perf-ffp-only.sh` — fixed-function pipeline only.
- `dxmt9-perf-many-draw.sh` — many small draws / draw-call throughput.
- `dxmt9-perf-multi-rt.sh` — MRT (multiple render target) workload.
- `dxmt9-perf-offscreen-heavy.sh` — offscreen render-target heavy.
- `dxmt9-perf-present-loop.sh` — present-only inner loop.
- `dxmt9-perf-present-only.sh` — present without encode (drawable cycle).
- `dxmt9-perf-skeletal.sh` — skeletal / skinned mesh probe.

## Conventions

- Each launcher sources `common.sh` first, then calls `exp_stage_dxmt9`
  and `exp_run_wine_binary`. Per-app variation comes from CATALOGUE
  fields (`binary`, `window_title`, `capture_frame`, env vars).
- Launcher filename must exactly match `CATALOGUE.name`. Adding a new app:
  1. Add `[[app]]` entry to `experiments/CATALOGUE.toml` with `name`,
     `binary`, `launcher`, `reference` (optional), `features`, `status`.
  2. Create `experiments/launchers/<name>.sh` (sources `common.sh`).
  3. (Optional) Create `scripts/build_apps/build_<name>.sh` and add
     `build_script` field to CATALOGUE for `--build` support.
- Synthetic probes share the same probe binary
  (`apps/PerformanceProbe`, `apps/BridgeEmptyProbe`, etc.) — the launcher
  injects different env vars / args.
- Run via:
  ```
  python3 scripts/run_apps/run_experiment.py run <name>
  python3 scripts/run_apps/run_experiment.py run <name> --build
  ```
