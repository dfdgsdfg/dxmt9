---
domain: index-cache-locality
workload: 3DMark05 GT1
title: "Index-Cache Locality — the only accepted production GPU win"
type: domain-index
status: current
updated: 2026-07-12
source: docs/perfomance/overview-3dmark05-gt1.md
related: docs/perfomance/index-cache-locality/overview.md; docs/perfomance/index-cache-locality/log.md
---

# Index-Cache Locality — the only accepted production GPU win

Latest tracked row: `H26` - The paired offload+opt-in `.gputrace` proof passes every promotion gate (accepted; frame60 finalizer verdict "all requested requirement gates were satisfied" vs the June baseline: target rows `60/0+60/1` GPU `-7.39%`, VS buffer write `-16.54%`, VS invocations `-14.12%` (identical to the historical proof), `175` candidate draws with miss32 `582,658 -> 450,807`; stable-frame, PSO-attribution, and coverage gates all pass, so the opt-in's evidence is complete — its default remains coupled to the offload because the CPU tax is only absorbed there).

Current status: the coupled pair is engine-default ON since `d45af067` (2026-07-10, H216 in [present-pacing](../present-pacing/index.md)) — unset follows the offload, explicit `0` opts out — and the probe wrapper pins match the engine defaults since `e5129346` (2026-07-12, H221).

## Start Here

- [Current overview](overview.md) - latest conclusion and active gates only.
- [Historical log](log.md) - long-form chronology moved from the old domain root.
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
- [index-cache-locality-cpucost.12 - Lazy Priority Frontier Rejection](index-cache-locality-cpucost.12.md)
- [index-cache-locality-cpucost.11 - Candidate Frontier Cap Rejection](index-cache-locality-cpucost.11.md)
- [index-cache-locality-screenblend.10 - Semantic Ceiling Is Now an Automated Gate](index-cache-locality-screenblend.10.md)
- [index-cache-locality-cpucost.10 - Candidate Select Volume Counters](index-cache-locality-cpucost.10.md)
