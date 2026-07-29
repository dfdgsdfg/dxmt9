# Boundary-Isolated Benchmark Infrastructure: Readiness Audit (Z1)

**Verdict:** Fit for purpose. All 6 boundaries have probes + harness +
counter schema. Core infrastructure is complete. Three strategic gaps
remain for the next cycle. Post-implementation audit of W1-A..D, W2,
W3, W4, X1, Y1 against the V1 design (`docs/research/boundary-benchmarks.md`).

## Five-Boundary Verdict at a Glance

| Boundary | Coverage | Isolation | Counters | Spec | Reporting | Overall |
|---|---|---|---|---|---|---|
| B1 PE→Recorder | ✅ | ✅ Native micro | ✅ 3 keys | ⚠️ No runtime contract | ✅ | **⚠️** Micro-bench complete |
| B2 PE→bridge | ✅ BridgeEmpty | ✅ 100k ops | ✅ 7 keys (p50/95/99) | ✅ | ✅ | **✅** Tightest isolation |
| B3 Queue | ✅ Encode+Chain | ⚠️ Real PE | ✅ 7 keys | ✅ R-BACK-2.29-34 | ✅ | **⚠️** Coupled to PE |
| B4 Encode | ✅ 6 probes | ⚠️ Real PE | ✅ 21 keys | ✅ R-BACK-12/15 | ✅ | **⚠️** No encoder-only variant |
| B5 Metal→GPU | ✅ Implicit | ⚠️ Black-box | ✅ 7 keys (p-tiles) | ⚠️ No R-BENCH-GPU-* | ✅ | **⚠️** Measured, not isolatable |
| B6 Presenter | ✅ PresentLoop | ✅ Empty Presents | ✅ 14 keys | ✅ | ✅ | **✅** Clean isolation |

## What Landed (Code-Grounded)

**Probes:** 4 new binaries under `experiments/apps/`:

- `perf_d3d9_bridge_empty.cpp` (~350 LOC) — B2, zero-payload Clear loop.
- `perf_d3d9_encode_replay.cpp` (~300 LOC) — B3+B4, offscreen chunk replay, no Present.
- `perf_d3d9_chain_parametric.cpp` (~400 LOC) — B3+B4, parameterized RT chain, optional `CHAIN_PRESENT_INTERVAL` after X1 fixed the encode-thread anomaly.
- `perf_d3d9_present_loop.cpp` (~250 LOC) — B6, tiny Clear + empty Presents.
- `tests/native/core/chunk_record_micro_spec.cpp` — B1 native micro-bench, fake backend, no Wine.

**Harness:** `scripts/tools/run_dx9_present_policy_ab.py` defines
`BOUNDARY_COUNTER_FIELDS` (6 boundaries × 3-22 keys). `payload["boundary"] = args.boundary` carries the chosen boundary into `summary.json`.

**Suite:** `scripts/run_suites/run_boundary_audit_suite.sh` runs 8
boundary/probe pairs (B2 / B3×2 / B4×4 / B6) and parks per-probe
output under `experiments/output/dx9-present-policy-ab/<tag>-<B>-*/`.

**Counter families added:**

- `bridge_commit_latency_{ns,max_ns,p50_ns,p95_ns,p99_ns}` (B2).
- `sub_command_buffers`, `chunk_subcb_count_max` (R-BACK-2.29..2.32, B3).
- `gpu_command_buffer_time_{ms,max_ms,samples,p50_ms,p95_ms,p99_ms}`,
  `gpu_command_buffer_errors` (M4/M5, B5).
- `Kind::PercentileNs` variant in `kCounterTable`.

**Specs:** `specs/benchmarks/boundary-counter-schema.md` mirrors
`BOUNDARY_COUNTER_FIELDS`. `specs/backend/requirements.md` carries
R-BACK-2.29..2.34. R-BACK-2.34 default-flipped policy (Y1 / 2026-05-10)
based on X1 chain-probe evidence (`docs/boundary-baseline-measurements.md`).
`specs/backend/gap.md` row reflects ⚠️ partial with the Y1 flip rationale.

## Critical Gaps & ROI

### 1. Expected-counter gates (2h, HIGH)

ChainParametricProbe and PresentLoopProbe lack
`[apps.dxmt9-perf-*.expected_counters]` entries in
`experiments/CATALOGUE.toml` (BridgeEmpty + EncodeReplay have them).
Without these, the L3 expected-range gate cannot catch regressions
automatically on the chain or present probes.

### 2. Unix-side chunk injection (5d, MEDIUM)

EncodeReplayProbe + ChainParametricProbe still route through real
PE `CommandRecorder` (the chunks ARE real PE recordings, replayed N
times). True isolation would serialize chunks to disk and inject
directly into the unix queue, removing PE recording overhead from the
B3+B4 measurement. Acceptable workaround today — probes already
isolate from Present+GPU; PE overhead is secondary.

### 3. Cross-summary diff tool (3h, LOW-MEDIUM)

No script to compare 8 per-boundary `summary.json` files. Regression
attribution requires manual `diff`. A
`scripts/analyze/compare_boundary_summaries.py <tag>` that emits a
delta table (e.g., "B5: `gpu_command_buffer_time_p99_ms` +22%") would
remove friction. Becomes load-bearing when boundary regressions show
up weekly.

## Isolation Quality

**B2 (Bridge): Tightest.** 100k Clear operations with zero GPU/encode
cost. `WINE_UNIX_CALL` + importer are real (by design — they own the
boundary), but payload is minimal. ✅

**B3+B4 (Queue+Encode): Coupled to PE.** Both probes consume the real
`CommandRecorder` path. Could skip PE via unix-side injection (gap #2),
but current approach is honest — PE recording IS a real cost. ⚠️

**B5 (Metal→GPU): Black-box.** Measures GPU wall time via
`MTLCommandBuffer.GPUStartTime/End`. Cannot distinguish compute work
from driver scheduling from synchronization. Workaround: Xcode
Instruments / Metal System Trace, plus the `os_signpost` intervals
(M3) under `com.dxmt9.translator/metal`. ⚠️

**B6 (Presenter): Very tight.** Empty Presents + tiny Clear. Isolates
drawable acquisition + compositor pacing from encode work. ✅

## Attribution Scenario

Hypothetical: a wild-app fps regressed 15%.

1. Confirm regression — `run_dx9_present_policy_ab.py` against the
   wild app, 3 runs.
2. Run suite — `bash scripts/run_suites/run_boundary_audit_suite.sh`
   → 8 per-boundary summaries.
3. **User manually diffs** the 8 summaries (no automated tool yet):
   - B2 bridge: `chunk_admit`, `bridge_commit_latency_*` — unchanged ✓
   - B3 queue: `command_buffers`, `sub_command_buffers`, waits — stable ✓
   - B4 encode: `encode_chunk_cpu_ms`, `render_pass_*` — **+8%** suspect
   - B5 GPU: `gpu_command_buffer_time_p99_ms` — **+22%** primary regression
   - B6 present: `present_acquire_wait_ms` — unchanged ✓
4. **Diagnosis:** B5 (driver/GPU) + B4 (encode CPU spike).
   Investigate recent encoder changes.

The workflow is sound; gap #3 (diff tool) would speed it up.

## Anti-Goals (Skip)

- **GPU-only fixed-CB replay probe.** Xcode Instruments + the M3
  signposts already do this better. Document the workflow in
  `agents/rules/metal_debugging.rules.md`; do not build an in-tree probe.
- **Consolidate PerformanceProbe 4 mode-variants.** Env-var modes are
  fine; per-launcher granularity is correct for reproducibility.
- **Replace wild-app benchmarks.** Boundary probes complement
  SFIV / SDK samples. End-to-end fps remains the user-visible
  oracle.
- ~~Flip sub-CB chain default to per-render-pass~~ — **already done by Y1
  (R-BACK-2.34, 2026-05-10)** based on X1 chain-probe evidence
  (wall-time -5%, encode CPU -63%, present_acquire_wait -20%).
  Continued real-app monitoring is the open follow-up, not a re-decision.

## Go-Live Readiness

✅ **Harness works.** `--boundary <B1..B6>` filters counters correctly.
`summary.json` + `summary.md` per probe. Audit suite runs all 8
boundary/probe pairs in one invocation.

✅ **Specs aligned.** R-BACK-2.29..2.34 contracted. Counter schema
mirrored at `specs/benchmarks/boundary-counter-schema.md`. `gap.md`
updated with Y1 evidence.

✅ **Real-scene regression attribution feasible.** Run suite, diff
per-boundary counters, identify the regressed boundary. Friction is
manageable for occasional regressions.

⚠️ **Open gaps are nice-to-have, not blockers.** Expected-counter
gates incomplete (2h fix). Chunk injection would tighten B3+B4 (5d,
medium ROI). Diff tool would reduce manual burden (3h, low-medium ROI).

**Conclusion:** The boundary-audit infrastructure is
**production-ready for weekly A/B runs and incident diagnosis**. The
V1 hypothesis (boundary-isolated probes complement, not replace, wild-
app oracle) holds. Next-cycle priorities, in ROI order:

1. Add expected-counter gates to Chain + PresentLoop probes (2h).
2. Decide whether unix-side chunk injection is worth the 5-day cost
   given that B3+B4 isolation is already adequate.
3. Cross-summary diff tool when boundary regressions become routine.

## Cross-references

- `docs/research/boundary-benchmarks.md` — V1 design.
- `docs/boundary-baseline-measurements.md` — W4 + X1 measurements.
- `docs/research/g-axis-tuning.md` — cap=4 cost model.
- `docs/sfiv-benchmark-measurement.md` — SFIV oracle, U1 + S3 A/Bs.
- `specs/benchmarks/boundary-counter-schema.md` — counter contract.
- `specs/backend/requirements.md` — R-BACK-2.29..2.34.
- `specs/backend/gap.md` — submission grain row.
