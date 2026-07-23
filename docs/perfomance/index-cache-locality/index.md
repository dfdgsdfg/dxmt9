---
domain: index-cache-locality
workload: 3DMark05 GT1
title: "Index-Cache Locality — the only accepted production GPU win"
type: domain-index
status: current
updated: 2026-07-21
source: docs/perfomance/overview-3dmark05-gt1.md; docs/perfomance/overview-3dmark05-gt2.md
related: docs/perfomance/index-cache-locality/overview.md; docs/perfomance/index-cache-locality/log.md
---

# Index-Cache Locality — the only accepted production GPU win

Latest tracked row: `H29` - GT2 merge-rejection telemetry finds `575,523` adjacent indexed-triangle boundaries but no single-condition frontier. The dominant exact class is binding payload + non-contiguous IB (`361,143`, `62.75%`), so a joined-index buffer alone cannot provide a semantic merge.

Current status: the coupled pair is engine-default ON since `d45af067` (2026-07-10, H216 in [present-pacing](../present-pacing/index.md)) — unset follows the offload, explicit `0` opts out — and the probe wrapper pins match the engine defaults since `e5129346` (2026-07-12, H221).

## Start Here

- [Current overview](overview.md) - latest conclusion and active gates only.
- [Historical log](log.md) - long-form chronology moved from the old domain root.
- [Root 3DMark05 GT1 map](../overview-3dmark05-gt1.md)

## Recent Leaf Documents

- [index-cache-locality-merge-rejection.23 - Strict Merge Rejections Require Multiple Preserved Draw Properties](index-cache-locality-merge-rejection.23.md)
- [index-cache-locality-scope-merge-gt2.22 - GT2 Confirms Extended Scope And Strict Merge Are No-Ops](index-cache-locality-scope-merge-gt2.22.md)
- [index-cache-locality-scope-merge.21 - Extended Scope And Strict Compatible Merge Have No GT1 Coverage](index-cache-locality-scope-merge.21.md)
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
