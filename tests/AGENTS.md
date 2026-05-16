# tests

Native unit suites, GPU shader corpus, end-to-end integration probes, and
Wine PE D3D9 conformance. All native suites run via `meson test`; the
conformance lane cross-builds PE executables that Wine runs on macOS.

| Directory | Role | Meson-wired |
|-----------|------|-------------|
| `native/smoke/` | Bootstrap smoke (`dxmt9-smoke`) | yes |
| `native/core/` | D3D9 frontend specs (device lifecycle, FFP keys, format caps, draw-state transforms, dod state format, draw-uniforms layout/dirty, shader translator) | yes (~12 specs) |
| `native/bridge/` | Wire chunk + replay specs (validation, hazard, replay, import) + bridge ops + WMT setBytes dispatch | yes (7 specs) |
| `native/backend/` | Metal backend specs (descriptor/pipeline keys, replay observer, resource hazard, render-pass actions, allocation counter, MTLHeap pooling, argbuf hybrid, tile FFP, dynamic rename ring, metalcapture) | yes (~15 specs) |
| `native/shader/` | Shader bytecode → MSL translator spec | yes (1 spec) |
| `shader_runner/` | GPU-visible `.shader_test` corpus runner (~28 corpus files) | yes (sharded `dxmt9-shader-corpus-*` per file) |
| `integration/wsi_present/` | End-to-end WSI present probe | partial (PE binary built; runs under Wine) |
| `fixtures/` | Shared corpus-sync fixture data | no (data only) |
| `conformance/d3d9/` | Wine PE D3D9 conformance — single shared `dxmt9-d3d9-conformance.exe` (T8 split: device/resource/swapchain/query_stateblock) + per-test x64 exes (window_cursor, queries, reset_lost, etc.) | yes (per-exe + manifest check) |

## Running

```sh
# Whole suite, excluding the slow GPU shader corpus
meson test -C build-x86_64-builtin --no-suite shader-corpus

# Single domain
meson test -C build-x86_64-builtin --suite '' dxmt9-state-draw-transform-spec

# Shader corpus (Metal-backed)
meson test -C build-x86_64-builtin --suite shader-corpus

# Wine D3D9 conformance manifest check
bash scripts/check/check_d3d9_conformance_manifest.sh
```

## Conventions

- Native specs are named `<area>_spec.cpp`; per-area `meson.build` declares
  one `executable()` + `test()` per spec. Shared fixtures live in
  `<area>/<group>_fixtures.hpp` (`static inline` for ODR safety in C, regular
  inline for C++).
- Test target names follow `dxmt9-<area>-<name>`. External invocations
  (CI, scripts) reference test names, not file paths.
- Wine PE conformance exes live under `conformance/d3d9/` with a per-test
  `MANIFEST.toml` entry (`scripts/check/check_d3d9_conformance_manifest.sh`
  validates each `source_file` and `executable` resolves).
- New specs default to `*_spec.cpp` + `dxmt9-<area>-<name>` test name. Add
  to the appropriate `tests/native/<area>/meson.build` file (not the root
  `tests/meson.build`).

## Verification scripts (Meson-wired)

| Test name | Script | Role |
|-----------|--------|------|
| `dxmt9-verify-tla` | `scripts/check/verify_tla.sh` | TLA+ model checker over queue/lifetime/encoder/query specs |
| `dxmt9-manifest-check` | `scripts/check/check_manifest.sh` | shader-corpus `MANIFEST.toml` filesystem parity |
| `dxmt9-shader-corpus-gaps` | `scripts/tools/shader_corpus_tool.py gaps` | shader-corpus model/opcode coverage gaps |
| `dxmt9-d3d9-conformance-manifest-check` | `scripts/check/check_d3d9_conformance_manifest.sh` | D3D9 conformance manifest source/exe validity |
| `dxmt9-d3d9-conformance-status-report` | `scripts/check/check_d3d9_conformance_status.py` | D3D9 conformance pass/fail report |
| `dxmt9-drift-report` | `scripts/check/check_drift.sh` | upstream vkd3d corpus drift |
| `dxmt9-perf-counter-table-audit` | `scripts/check/audit_perf_counter_table.py` | perf counter `Counters` field ↔ `kCounterTable` parity |
| `dxmt9-perf-counter-callsite-audit` | `scripts/check/audit_perf_counter_callsites.py` | perf counter `count*()` declaration ↔ production callsite parity |
| `dxmt9-corpus-sync-smoke` | `tests/shader_runner/corpus_sync_smoke.py` | corpus sync round-trip |
