# scripts/run_apps

Per-app experiment runners. Most apps go through the consolidated Python
driver; a small number that need extra host-specific setup (default-prefix
injection, installer extraction) keep a thin shell wrapper. None of these are
wired to Meson tests; they are evidence-gathering tools.

- `run_experiment.py` — core runner: stages a Wine prefix, launches the app,
  collects logs and metrics. All other entries here call this.
  - `python3 run_experiment.py list` — list catalogue entries.
  - `python3 run_experiment.py run <name>` — run one app.
  - `python3 run_experiment.py run <name> --build` — first run the app's
    `build_script` field from `experiments/CATALOGUE.toml`, then run. Errors if
    the app has no `build_script` declared.
- `run_app-d3d9-anno-1404_experiment.sh` — Anno 1404 Gold launcher (injects default
  Heroic `--wine-root` and `--prefix`; no build step).
- `run_app-d3d9-sfiv-benchmark_experiment.sh` — Street Fighter IV benchmark (extracts
  the public installer's MSI, installs prefix-native `d3dx9_41`, picks the
  Heroic vs CrossOver host lane).
- `run_app-d3d9-3dmark05-verify_direct.sh` — direct foreground runner for the
  existing 3DMark05 verify prefix. It enables `DXMT_3DMARK05_DIRECT=1` on the
  regular 3DMark05 launcher and uses `DXMT_3DMARK05_ARGS` when a manual
  heuristic run needs different switches. The default run enables only the first
  benchmark, GT1:
  `-gt1 -nosplash -nosysteminfo -noscreens`. GT2/GT3/CPU/feature tests stay
  disabled unless requested explicitly. Use
  `DXMT_3DMARK05_ARGS='-gtall -batchall -featureall -cpuall -nosplash -nosysteminfo -noscreens'`
  for the full suite. By default it stages the current dxmt9 x64/x86 PE DLLs
  plus the unix provider into the verify prefix before launch; set
  `DXMT_3DMARK05_STAGE=0` only when intentionally testing the already-installed
  prefix binaries. Auto-Enter is controlled with
  `DXMT_3DMARK05_AUTO_ENTER`, `DXMT_3DMARK05_ENTER_DELAY_SEC`,
  `DXMT_3DMARK05_ENTER_COUNT`, and `DXMT_3DMARK05_ENTER_INTERVAL_SEC`.
