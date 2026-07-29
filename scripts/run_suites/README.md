# scripts/run_suites

Multi-app benchmark and regression suites. Each suite iterates over a fixed
set of apps under `run_apps/`, aggregates results, and writes summary
artifacts. None are wired to Meson tests.

- `run_dx9_builtin_oracle_suite.sh` — runs the builtin lane against the
  manifest and compares with oracle outputs.
- `run_dx9_fast_sanity_suite.sh` — fast-sanity suite runner that drives
  the fast-sanity bundle through `run_experiment.py`.
- `run_dx9_oracle_compare_suite.sh` — A/B compares dxmt9 vs reference oracles.
- `run_dx9_performance_suite.sh` — performance probe sweep.
- `run_dx9_regression_suite.sh` — regression sweep over the conformance
  manifest.
- `run_d3d9_conformance_render_modes.sh` — CI conformance gate that runs
  `scripts/tools/run_d3d9_conformance.py` once per renderer
  (`DXMT9_RENDER_MODE=traditional` and `=framegraph`), forces the explicit
  `strict`/feature-empty profile for byte-identical backend parity, records each
  leg's verdicts under a mode-tagged artifact, and exits non-zero on a
  mode-specific regression (R-BACK-39.4). This is the merge-blocking gate: the
  underlying conformance runner only records verdict JSON and always exits 0,
  so the pass/fail decision is computed here from the per-mode result files.
