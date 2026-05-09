# scripts/check

Validation and audit scripts. All entries here are wired into Meson tests
under `tests/meson.build`.

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
  missing from a run JSON; intended to extend draw-uniforms coverage.
