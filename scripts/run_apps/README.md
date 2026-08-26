# scripts/run_apps

Per-app experiment runners. Most apps go through the consolidated Python
driver; a small number that need extra host-specific setup (default-prefix
injection, installer extraction) keep a thin shell wrapper. None of these are
wired to Meson tests; they are evidence-gathering tools.

- `run_experiment.py` — core runner: stages a Wine prefix, launches the app,
  collects logs and metrics. All other entries here call this.
  - `scripts/run_python.sh scripts/run_apps/run_experiment.py list` — list
    catalogue entries.
  - `scripts/run_python.sh scripts/run_apps/run_experiment.py run <name>` — run
    one app.
  - `scripts/run_python.sh scripts/run_apps/run_experiment.py run <name>
    --build` — first run the app's
    `build_script` field from `experiments/CATALOGUE.toml`, then run. Errors if
    the app has no `build_script` declared.
Per-app shell wrappers are legacy. Three were removed on 2026-07-29:
`run_app-d3d9-sfiv-benchmark_experiment.sh` forwarded every argument unchanged
and added nothing; `scripts/run_suites/run_sfiv_benchmark_crossover_oracle.sh`
passed a `--host` flag `run_experiment.py` does not accept, so it had been
failing at argument parsing; and a third hardcoded a Heroic Wine root and
prefix — the exact pattern `agents/rules/test_wild.rules.md` names as bypassing
the manifest — for an app that has since left the catalogue. Run these apps the
same way every other catalogue entry is run:

```sh
scripts/run_python.sh scripts/run_apps/run_experiment.py run app-d3d9-sfiv-benchmark
```

The Wine runtime comes from the entry's `wine_id` in
`experiments/CATALOGUE.toml`; override it per run with `--wine-id`, never by
reintroducing a hardcoded `--wine-root` default in a wrapper.

`app-d3d9-3dmark06` likewise has no `scripts/run_apps/` wrapper. Stage its
external payload under `experiments/apps_3rd/app-d3d9-3dmark06/` and invoke it
through `run_experiment.py`; its per-test selection and `.3dr` output inputs
are documented in `experiments/launchers/README.md`.

- `run_app-d3d9-3dmark05-verify_direct.sh` — direct foreground runner for the
  existing 3DMark05 verify prefix. It enables `DXMT_3DMARK05_DIRECT=1` on the
  regular 3DMark05 launcher and uses the same `DXMT_3DMARK05_LANE` presets as
  catalogue runs. `DXMT_3DMARK05_ARGS` remains the raw custom escape hatch.
  The default run enables only the first benchmark, GT1:
  `-gt1 -nosplash -nosysteminfo -noscreens`. GT2/GT3/CPU/feature tests stay
  disabled unless requested explicitly. Use `DXMT_3DMARK05_LANE=all` for the
  full suite or `DXMT_3DMARK05_LANE=cpu` for the CPU diagnostics. By default it
  stages the current dxmt9 x64/x86 PE DLLs
  plus the unix provider into the verify prefix before launch; set
  `DXMT_3DMARK05_STAGE=0` only when intentionally testing the already-installed
  prefix binaries. The direct wrapper supervises the launcher with
  `DXMT_3DMARK05_DIRECT_TIMEOUT=180` by default because 3DMark05 can hang on
  the final frame after useful output is written; set a different positive
  timeout for longer manual suites. The catalogue entry also requires a
  positive timeout and rejects `run_experiment.py --timeout 0`. Use
  `DXMT_3DMARK05_DIRECT_DRY_RUN=1` to
  verify the resolved timeout and launcher command without starting Wine.
  The launcher also has a direct-shell fallback timeout
  (`DXMT_3DMARK05_LAUNCHER_TIMEOUT=180` by default) when it is not already
  supervised by `run_experiment.py` or this wrapper. It traps `TERM`/`INT` and
  kills the verify-prefix wineserver on exit by default
  (`DXMT_3DMARK05_KILL_SERVER_ON_EXIT=1`), which prevents a final-frame hang
  from leaving detached Wine processes after a timeout.
  Auto-Enter is controlled with
  `DXMT_3DMARK05_AUTO_ENTER`, `DXMT_3DMARK05_ENTER_DELAY_SEC`,
  `DXMT_3DMARK05_ENTER_COUNT`, and `DXMT_3DMARK05_ENTER_INTERVAL_SEC`.
