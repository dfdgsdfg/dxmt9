# Boundary-isolated counter schema

Reporting contract for the V1 boundary-isolation benchmarks. Each
pipeline boundary owns a fixed list of perf counters; A/B reports
emitted by `scripts/tools/run_dx9_present_policy_ab.py --boundary
<Bn>` surface only that boundary's keys.

A regression on the wild oracle (SFIV / Anno1404) is then attributable:
diff per-boundary summaries, the boundary whose keys moved is the
boundary that regressed.

See:

- `docs/research/boundary-benchmarks.md` — V1 audit + probe inventory.
- `specs/backend/requirements.md` — boundary ownership (R-BACK-2.x).
- `scripts/tools/run_dx9_present_policy_ab.py` — `BOUNDARY_COUNTER_FIELDS`
  is the source of truth for the per-boundary key lists; this doc
  mirrors that table.
- `scripts/run_suites/run_boundary_audit_suite.sh` — runs the matching
  probe(s) for every boundary in one pass.

## Boundary → counter set

| Boundary | Owner | Probe(s) | Counter keys reported |
|---|---|---|---|
| **B1** PE → CommandRecorder | PE D3D9 layer + `PeCommandChunkBuilder` | `dxmt9-chunk-record-micro-spec` (native), runtime probes echo it via volume sentinels | `chunk_admit`, `submit_draw_cpu_ms`, `draw_calls` |
| **B2** PE → unix bridge | `winemetal::commit_chunk`, importer | `dxmt9-perf-bridge-empty` | `chunk_admit`, `chunk_reject`, `bridge_commit_latency_ns`, `bridge_commit_latency_max_ns`, `bridge_commit_latency_p50_ns`, `bridge_commit_latency_p95_ns`, `bridge_commit_latency_p99_ns` |
| **B3** unix CommandQueue | `CommandQueue` lifecycle, sub-CB chain | `dxmt9-perf-encode-replay`, `dxmt9-perf-chain-parametric` | `command_buffers`, `sub_command_buffers`, `chunk_subcb_count_max`, `queue_writer_wait_ms`, `queue_commit_wait_ms`, `ring_arena_heap_fallback_count`, `ring_arena_heap_fallback_bytes` |
| **B4** encode thread → MTLCB | encoder lifecycle, render-pass actions, hazards | `dxmt9-perf-{ffp-only,multi-rt,depth-heavy,skeletal,encode-replay,chain-parametric}` | `encode_chunk_calls`, `encode_chunk_cpu_ms`, `encode_chunk_cpu_max_ms`, `encode_draw_*_cpu_ms` family, `render_pass_begin/end`, `render_pass_load/store_action_*`, `render_pass_tile_preservation_bytes`, `uniform_*_calls`, `uniform_volatile_pushes` |
| **B5** Metal driver → GPU | command-buffer GPU wall time, GPU faults | end-to-end runs (any probe; metric is GPU-side) | `gpu_command_buffer_time_ms`, `gpu_command_buffer_time_max_ms`, `gpu_command_buffer_time_samples`, `gpu_command_buffer_time_p50_ms`, `gpu_command_buffer_time_p95_ms`, `gpu_command_buffer_time_p99_ms`, `gpu_command_buffer_errors` |
| **B6** GPU → Presenter | `Presenter`, drawable acquire, frame-token | `dxmt9-perf-present-loop`, `run_dx9_present_policy_ab` modes | `present_encoded`, `present_skipped`, `present_acquire_wait_ms` (+ `_max`), `present_async_acquire_*`, `present_boundary_wait_ms` (+ `_max`), `present_token_wait_ms` (+ `_max`), `present_preacquire_*`, `command_buffers`, `completion_present_wait_ms`, `queue_writer_wait_ms`, `queue_commit_wait_ms` |

## Reporting rules

1. **Single source of truth** — `BOUNDARY_COUNTER_FIELDS` in the runner.
   This doc mirrors that constant; if the constant moves, this doc
   moves too.
2. **`--boundary <Bn>` restricts the summary** — `summary.json["summary"][i]["counters"]`
   contains only the boundary's keys. The full counter dump still
   lives in each per-run `result.json` for forensic dig-in.
3. **`payload["boundary"]`** is set to the chosen boundary string (e.g.
   `"B3"`) when `--boundary` is passed; absent / `null` otherwise.
4. **A/B regression attribution** — when a wild-app fps regresses,
   re-run the boundary suite (`scripts/run_suites/run_boundary_audit_suite.sh`).
   The boundary whose summary diff is non-trivial is the regression
   site; others are noise.
5. **Probe coverage is asymmetric by design** — B1 has no runtime
   probe (covered by the native micro-bench `dxmt9-chunk-record-micro-spec`);
   B5 has no dedicated probe (any probe that runs commits CBs surfaces
   the GPU wall-time counters); B3 + B4 share probes because encoder
   work always crosses the queue tracker.

## Anti-patterns

- **Don't add a key to multiple boundaries.** A counter belongs to one
  owner; cross-boundary leakage breaks attribution. If a counter
  observably belongs to two stages (e.g. `command_buffers` shows up
  under both B3 and B6 in the schema above), document the rationale
  inline in `BOUNDARY_COUNTER_FIELDS`.
- **Don't gate a probe on counters from another boundary.** L3
  expected-range gates in `experiments/CATALOGUE.toml` should match the
  probe's owning boundary only.
- **Don't drop wild-app benchmarks.** The boundary suite complements
  SFIV / Anno / SDK samples; it does not replace them. End-to-end fps
  remains the user-visible regression sentinel.

## Workflow

When a perf counter is added or renamed:

1. Pick a single boundary owner.
2. Add the key to the matching list in
   `BOUNDARY_COUNTER_FIELDS` (`scripts/tools/run_dx9_present_policy_ab.py`).
3. Update the table above to mirror.
4. If the counter is gauge-style (sentinel / regression-only),
   consider an `expected_counters` entry in
   `experiments/CATALOGUE.toml` for the matching probe.

## Related

- `docs/research/boundary-benchmarks.md` — design rationale.
- `docs/research/g-axis-tuning.md` — G-axis cost model that motivated
  the chain-parametric probe.
- `docs/sfiv-benchmark-measurement.md` — wild-app oracle for
  cross-checking regression attributions.
