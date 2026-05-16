# scripts/check

Validation and audit scripts. Entries here are either direct Meson tests in
`tests/meson.build` or wrappers used by a Meson-registered spec.

- `check_drift.sh` — invokes `tools/shader_corpus_tool.py drift` against an
  upstream vkd3d tree (test `dxmt9-drift-report`).
- `check_manifest.sh` — verifies the shader corpus `MANIFEST.toml` matches the
  filesystem (test `dxmt9-manifest-check`).
- `check_d3d9_conformance_manifest.sh` — validates the D3D9 conformance
  manifest entries point at real apps and oracles (test
  `dxmt9-d3d9-conformance-manifest-check`).
- `check_d3d9_conformance_status.py` — reads the conformance manifest and
  reports current pass/fail status (test
  `dxmt9-d3d9-conformance-status-report`).
- `verify_tla.sh` — runs the TLA+ model checker over the queue, resource
  lifetime, encoder lifecycle, and query sequencing specs (test
  `dxmt9-verify-tla`).
- `assert_perf_counters.py` — fails when expected perf counter keys are
  missing from a wrapped executable's `[dxmt9-perf]` output (used by
  `dxmt9-allocation-counter-spec`).
- `audit_perf_counter_table.py` — text-based audit detecting fields added to
  the `Counters` struct that are never referenced from `kCounterTable`,
  preventing silent-miss regressions in `[dxmt9-perf]` output (test
  `dxmt9-perf-counter-table-audit`).
- `audit_perf_counter_callsites.py` — text-based audit detecting declared
  `count*()` perf-counter functions that have no production call site under
  `src/` (test `dxmt9-perf-counter-callsite-audit`).
