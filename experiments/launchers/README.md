# experiments/launchers

Per-app or synthetic-probe shell launchers invoked by
`scripts/run_apps/run_experiment.py` per `experiments/CATALOGUE.toml`'s
`launcher` field. Filenames match `CATALOGUE.name` (kebab-case) so
`grep <app>` finds every artifact.

## Shared helpers (no CATALOGUE entry)

- `common.sh` — sourced by every launcher; provides `exp_stage_dxmt9`,
  `exp_run_wine_binary`, prefix discovery, capture/log hooks.
- `conf-d3d9-fast-sanity.sh` — bundle launcher for the 5 d9vk fast-sanity apps
  (`conf-d3d9-clear`, `conf-d3d9-buffer`, etc.), shared rather than
  per-app.

## App-bound launchers (1 per CATALOGUE entry)

D3D9 SDK / DXUT / third-party samples:

- `sample-d3d9-basic-hlsl.sh` — DX SDK sample-d3d9-basic-hlsl.
- `sample-d3d9-tutorial07.sh` — DX SDK sample-d3d9-tutorial07 (skinned mesh).
- `sample-d3d9-hdr-formats.sh` — DX SDK sample-d3d9-hdr-formats (FP16/FP32 RT).
- `sample-d3d9-dxut-simple.sh` — DXUT framework simple sample.
- `sample-d3d9-irrlicht-lights.sh` — Irrlicht engine managed-lights demo.

Self-authored apps:

- `conf-d3d9-srgb-texture.sh` — conf-d3d9-intent-probe `srgbtexture` mode.
- `conf-d3d9-float-texture.sh` — conf-d3d9-intent-probe `float-texture` mode.
- `conf-d3d9-stream-source.sh` — conf-d3d9-intent-probe `stream-source` mode.
- `conf-d3d9-shademode-provoking.sh` — conf-d3d9-intent-probe `shademode-provoking` mode.
- `conf-d3d9-pointsize.sh` — conf-d3d9-intent-probe `pointsize-policy` mode.
- `conf-d3d9-yuv-format.sh` — conf-d3d9-intent-probe `yuv-format-policy` mode.
- `conf-d3d9-vendor-format.sh` — conf-d3d9-intent-probe `vendor-format-policy` mode.
- `sample-d3d9-multitexture-terrain.sh` — multitexture terrain demo.
- `sample-d3d9-water-rt.sh` — water render-target / refraction demo.
- `conf-d3d9-wsi-present.sh` — minimal WSI present smoke (CI bootstrap).

Commercial / 3rd-party titles (require external prefix):

- `app-d3d9-anno-1404.sh` — Anno 1404 Gold (Heroic prefix).
- `app-d3d9-sfiv-benchmark.sh` — SFIV benchmark (Heroic + CrossOver
  oracle lanes — see `scripts/run_apps/run_app-d3d9-sfiv-benchmark_experiment.sh`
  for installer-extraction wrapper).

## Synthetic perf probes (no app source — shared probe binary, parameter-driven)

- `perf-d3d9-bridge-empty.sh` — bridge round-trip baseline (no draw work).
- `perf-d3d9-chain-parametric.sh` — parametric chain length probe.
- `perf-d3d9-depth-heavy.sh` — depth/Z heavy workload.
- `perf-d3d9-encode-replay.sh` — encode-then-replay throughput.
- `perf-d3d9-ffp-only.sh` — fixed-function pipeline only.
- `perf-d3d9-many-draw.sh` — many small draws / draw-call throughput.
- `perf-d3d9-multi-rt.sh` — MRT (multiple render target) workload.
- `perf-d3d9-offscreen-heavy.sh` — offscreen render-target heavy.
- `perf-d3d9-present-loop.sh` — present-only inner loop.
- `perf-d3d9-present-only.sh` — present without encode (drawable cycle).
- `perf-d3d9-skeletal.sh` — skeletal / skinned mesh probe.

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
  (`apps/perf-d3d9-probe`, `apps/perf-d3d9-bridge-empty`, etc.) — the launcher
  injects different env vars / args.
- Run via:
  ```
  python3 scripts/run_apps/run_experiment.py run <name>
  python3 scripts/run_apps/run_experiment.py run <name> --build
  ```
