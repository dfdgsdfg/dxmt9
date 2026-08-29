---
domain: index-cache-locality
workload: 3DMark05 GT1
title: "Index-Cache Locality — workload-gated GPU provider"
type: domain-index
status: current
updated: 2026-08-29
source: docs/perfomance/overview-3dmark05-gt1.md; docs/perfomance/overview-3dmark05-gt2.md
related: docs/perfomance/index-cache-locality/overview.md; docs/perfomance/index-cache-locality/log.md
---

# Index-Cache Locality — workload-gated GPU provider

Latest tracked row: `H29` - GT2 merge-rejection telemetry finds `575,523` adjacent indexed-triangle boundaries but no single-condition frontier. The dominant exact class is binding payload + non-contiguous IB (`361,143`, `62.75%`), so a joined-index buffer alone cannot provide a semantic merge.

Current status: explicit default-off. The GT1 GPU mechanism remains valid, but
the former offload coupling was retired on 2026-08-29 after STALKER Day exposed
`97.53 ms/present` of candidate CPU and `90.2%` post-build rejection. The probe
wrapper pins `0`; `--optimize-opaque-depth-index-cache` remains the opt-in.

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
- [index-cache-locality-cpucost.16 - Draw-Shape Prefilter Audit](index-cache-locality-cpucost.16.md)
