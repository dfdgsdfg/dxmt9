---
domain: index-cache-locality
workload: 3DMark05 GT1
title: "Index-Cache Locality — the only accepted production GPU win - Current Overview"
type: domain-overview
status: current
updated: 2026-07-08
source: docs/perfomance/index-cache-locality/log.md; docs/perfomance/overview-3dmark05-gt1.md
related: docs/perfomance/index-cache-locality/index.md; docs/perfomance/index-cache-locality/log.md
---

# Index-Cache Locality — the only accepted production GPU win - Current Overview

> Current, compact view for this performance domain. Historical detail from the former
> top-level `index-cache-locality.md` overview is preserved in [log](log.md). Domain landing: [index](index.md).

## Scope

This domain owns the **one accepted production optimization** of the whole GT1
investigation: a cached, semantic-safe post-transform index-cache reorder that
lowers VS invocations — and therefore the hidden TVB / parameter-buffer write
bucket — for **opaque depth-writing triangle lists**. It covers the production
flag `DXMT9_OPTIMIZE_OPAQUE_DEPTH_INDEX_CACHE=1` (+ `_MIN_GAIN_PCT`), the
explicit-tolerance-only screen-blend variant `DXMT9_OPTIMIZE_SCREEN_BLEND_INDEX_CACHE`,
min-gain threshold tuning, the no-mutate identity scouts that fed candidate
selection, the CPU-cost optimization of the cache path, and the remaining `50/2`
bottleneck triage. The mechanism behind why this works is proven separately by
[tvb-mechanism-proof](../tvb-mechanism-proof/index.md): TVB write ≈ `VS invocations × per-vertex VSOut bytes`.

## Latest Conclusions

| # | Hypothesis | Verdict | Evidence |
|---|---|---|---|
| H22 | The current perf gate can keep the locality semantic ceiling attached to the next Xcode queue | accepted (gate) | [index-cache-locality-screenblend.10](index-cache-locality-screenblend.10.md) (`locality-semantic-ceiling=oracle-required`; color-exact/zero-sample buckets are too small, sample-visible bucket needs final-color/final-writer proof) |
| H23 | Current real-texture semantic replay summaries provide the missing final-writer oracle | rejected by gate | [hidden-backend-storage-shape.20](../hidden-backend-storage/hidden-backend-storage-shape.20.md) (`final-writer-replay-oracle=blocked-final-writer-hazard`; fail LRU32 `-14,593`, masked LRU32 `-9,113`, owner-safe LRU32 `0`) |
| H24 | Gate/class/primitive-shape telemetry can classify the remaining opaque-depth CPU side-effect before another Xcode spend | accepted; frame60 hot rows have `102/102` candidate gate-pass and `0` gate-fail, so the blocker is valid candidate construction/cache lookup, not hot-row failed-gate waste | [index-cache-locality-cpucost.18](index-cache-locality-cpucost.18.md) |
| H25 | The commit-replay offload absorbs the candidate/lookup CPU tax at FPS parity | accepted; with `DXMT9_OFFLOAD_COMMIT_REPLAY=1` the opt-in runs at `1999 -> 1980` presents (`-0.95%`, noise) while applying `333,283` reordered-buffer hits (`~168` draws/present, `67` buffers created) — the `~0.24ms/present` build/select/lookup cost lands on worker/encode threads with idle headroom, so the runtime promotion blocker is gone; remaining formal gate is a paired offload+opt-in `.gputrace` proof | [index-cache-locality-offload-synergy.19](index-cache-locality-offload-synergy.19.md) |
| H26 | The paired offload+opt-in `.gputrace` proof passes every promotion gate | accepted; frame60 finalizer verdict "all requested requirement gates were satisfied" vs the June baseline: target rows `60/0+60/1` GPU `-7.39%`, VS buffer write `-16.54%`, VS invocations `-14.12%` (identical to the historical proof), `175` candidate draws with miss32 `582,658 -> 450,807`; stable-frame, PSO-attribution, and coverage gates all pass, so the opt-in's evidence is complete — its default remains coupled to the offload because the CPU tax is only absorbed there | [index-cache-locality-offload-promotion-proof.20](index-cache-locality-offload-promotion-proof.20.md) |

## Current Navigation

- [Domain index](index.md)
- [Historical log](log.md)
- [Root 3DMark05 GT1 map](../overview-3dmark05-gt1.md)

## Recent Leaf Documents

- [index-cache-locality-offload-promotion-proof.20 - Offload+IndexCache Promotion Proof Passes Every Gate](index-cache-locality-offload-promotion-proof.20.md)
- [index-cache-locality-offload-synergy.19 - Offload Absorbs The Index-Cache CPU Tax At FPS Parity](index-cache-locality-offload-synergy.19.md)
- [index-cache-locality-cpucost.18 - Candidate Gate Shape Counters](index-cache-locality-cpucost.18.md)
- [index-cache-locality-cpucost.17 - Strict LRU Candidate Builder Diagnostic](index-cache-locality-cpucost.17.md)
- [index-cache-locality-cpucost.16 - Draw-Shape Prefilter Audit](index-cache-locality-cpucost.16.md)
- [index-cache-locality-cpucost.15 - Persistent Rejected Verdict Refresh](index-cache-locality-cpucost.15.md)
- [index-cache-locality-cpucost.14 - Candidate Upper-Bound Pre-Gate Rejection](index-cache-locality-cpucost.14.md)
- [index-cache-locality-cpucost.13 - Bucketed Candidate Selector Rejection](index-cache-locality-cpucost.13.md)
