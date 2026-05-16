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
- `audit_diagnostic_costs.py` — source-level R-TEST-14.19..14.23 audit for
  harness cost-class invariants and undocumented consumed `DXMT*` / `DXMT9*`
  diagnostic environment variables. It runs without build artifacts and is a
  failing Meson gate; `--dry-run` remains available for local preview.
- `check_debug_result_schema.py` — validates the machine-readable
  `dxmt9.debug.result.v1` contract for WSI visible-output evidence, headless
  results, boundary dumps, frame sequences, and bounded video segments (test
  `dxmt9-debug-result-schema-selftest`).
- `scripts/tools/debug_artifact_bundle.py` — writes schema-compatible boundary
  dump bundles, frame sequence manifests, and video segment metadata for
  harnesses that already collected the underlying values or captures (test
  `dxmt9-debug-artifact-bundle-selftest`).
