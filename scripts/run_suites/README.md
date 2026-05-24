# scripts/run_suites

Multi-app benchmark and regression suites. Each suite iterates over a fixed
set of apps under `run_apps/`, aggregates results, and writes summary
artifacts. None are wired to Meson tests.

- `run_dx9_builtin_oracle_suite.sh` — runs the builtin lane against the
  manifest and compares with oracle outputs.
- `run_conf-d3d9-fast-sanity_suite.sh` — fast-sanity suite runner that drives
  the fast-sanity bundle through `run_experiment.py`.
- `run_dx9_oracle_compare_suite.sh` — A/B compares dxmt9 vs reference oracles.
- `run_dx9_performance_suite.sh` — performance probe sweep.
- `run_dx9_regression_suite.sh` — regression sweep over the conformance
  manifest.
- `run_sfiv_benchmark_crossover_oracle.sh` — Street Fighter IV under
  CrossOver, used as a reference oracle.
