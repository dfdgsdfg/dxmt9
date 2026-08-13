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
- `audit_perf_counter_table.py` — text-based audit detecting fields added to
  the `Counters` struct that are never referenced from `kCounterTable`,
  preventing silent-miss regressions in `[dxmt9-perf]` output (test
  `dxmt9-perf-counter-table-audit`).
- `audit_perf_docs_sources.py` — checks newly added `docs/perfomance` leaf files
  so deleted/retired `specs/perfomance.plan.md` line ranges are not used as new
  provenance or maintenance state.
- `test_render_tape_cli.py` — produces a bounded v2 frame-tape fixture through
  the native builder and checks validator/inspect success with a verified
  mutation blob, digest-named bundle pack/validation, corrupted-header
  rejection, and component digest rejection (`dxmt9-render-tape-cli-spec`).
- `test_render_tape_provider_cli.py` — proves fresh-device warmup/repeat
  identity, the strict production output-oracle predicate, deterministic
  closure-aware whole-command reduction/bisection, content-addressed reduced
  bundles, bounded two-interval mutation visibility with distinct per-Present
  digests, and fail-before-output rejection
  (`dxmt9-render-tape-provider-cli-spec`).
