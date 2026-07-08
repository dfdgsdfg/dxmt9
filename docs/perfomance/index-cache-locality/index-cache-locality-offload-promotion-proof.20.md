---
domain: index-cache-locality
workload: 3DMark05 GT1
subcategory: promotion
order: 20
title: Offload+IndexCache Promotion Proof Passes Every Gate
date: 2026-07-08
type: gputrace-xcode
status: accepted-promotion-proof
source: traces/app-d3d9-3dmark05-consol-gputrace-proof-20260707/analysis/frame60-xcode-dxmt-comparison.md; traces/app-d3d9-3dmark05-consol-gputrace-proof-20260707/analysis/frame60-xcode-dxmt-joined-summary.csv; traces/app-d3d9-3dmark05-consol-gputrace-proof-20260707/analysis/frame60-counters-xcode.csv; experiments/output/app-d3d9-3dmark05-consol-gputrace-proof-20260707/result.json; docs/perfomance/present-pacing/present-pacing-consolidation-long-confirm.194.md
related: docs/perfomance/index-cache-locality/index.md; docs/perfomance/index-cache-locality/index-cache-locality-offload-synergy.19.md; specs/backend/requirements.md
---

# Index-Cache Locality 20 - Offload+opt-in promotion proof

## Question

H194 finished the runtime side (full-demo `+10.0%` FPS, visual pass). Does
the combined `DXMT9_OFFLOAD_COMMIT_REPLAY=1` +
`DXMT9_OPTIMIZE_OPAQUE_DEPTH_INDEX_CACHE=1` configuration reproduce the
accepted frame60 GPU win through the full
`--require-opaque-depth-index-cache-proof` gate set?

## Run

`--frame 60` `.gputrace` probe (`--with-wine-capture-layer`), healthy
capture run (`1920` presents, `325,848` reordered hits during capture,
zero GPU errors), manual Xcode encoder-counter export, finalizer against
the freshest existing baseline joined CSV
(`app-d3d9-3dmark05-capture-layer-current-r3`, June HEAD — code-drift
caveat acknowledged; the deltas below therefore bundle offload + index
cache + five weeks of other landings, but the target-row mechanism gates
are candidate-side absolute checks unaffected by the baseline's age).

## Verdict

**Passed: all requested requirement gates were satisfied** (finalizer
verdict) — including `--require-stable-frame-proof`,
`--require-top-row-key-match`, PSO attribution, Xcode counter and dxmt
join coverage, and the draw/vertex/triangle stability ratios (`<=5%`).

Target rows `60/0 + 60/1`:

| Metric | baseline | offload+opt-in | delta |
|---|---:|---:|---:|
| `target_gpu_ms` | `15.604` | `14.451` | **`-7.39%`** |
| `target_vs_buffer_write_mib` | `798.110` | `666.139` | **`-16.54%`** |
| `target_vs_invocations` | `536,583` | `460,839` | **`-14.12%`** |
| `target_cache_opt_candidate_draws` | `0` | `175` | mechanism live |
| candidate miss32 `original -> effective` | — | `582,658 -> 450,807` | `-22.6%` |

The VS-invocation delta matches the historical accepted proof exactly
(`-14.12%`), confirming the reorder mechanism is unchanged under offload.

## Promotion state

Evidence complete for the pair: runtime FPS (`+10-12%`, H190/H191/H194),
correctness (suites, TLA, visual anchors, R-BACK-2.51 hardening), and this
GPU-side proof. Default-flip readiness differs per flag:

- `DXMT9_OPTIMIZE_OPAQUE_DEPTH_INDEX_CACHE`: FPS-neutral only when the
  offload absorbs its CPU tax — its default should be coupled to (or
  conditioned on) the offload, not flipped alone.
- `DXMT9_OFFLOAD_COMMIT_REPLAY`: blocked from an unconditional default by
  the documented caveat that non-PE (direct COM) clients of the same
  process would lose the inline present boundary; a default flip first
  needs the `submitPresent` suppression to become per-present-context
  aware instead of process-global env.

Recommended promotion vehicle until then: enable both in the shared
`DXMT_EXPERIMENT_PROFILE=perf` runtime profile so every perf-path run
carries them by default while engine defaults stay conservative.
