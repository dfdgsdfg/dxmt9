# scripts/run_apps

Per-app experiment runners. Each shell wrapper is a thin shim around
`run_experiment.py` for one entry in `experiments/CATALOGUE.toml`. None of
these are wired to Meson tests; they are evidence-gathering tools.

- `run_experiment.py` — core runner: stages a Wine prefix, launches the app,
  collects logs and metrics. All other entries here call this.
- `run_anno1404_experiment.sh` — Anno 1404 Gold launcher.
- `run_basic_experiment.sh` — DX SDK BasicHLSL.
- `run_hdrformats_experiment.sh` — DX SDK HDRFormats.
- `run_irrlicht_managed_lights_experiment.sh` — Irrlicht managed lights.
- `run_multitextureterrain_experiment.sh` — multitexture terrain.
- `run_sfiv_benchmark_experiment.sh` — Street Fighter IV benchmark.
- `run_simple_sample_experiment.sh` — DXUT simple sample.
- `run_tutorial07_experiment.sh` — DX SDK Tutorial07.
- `run_waterrt_experiment.sh` — water render-target demo.
