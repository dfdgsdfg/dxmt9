---
domain: root
workload: dxmt9 performance
title: "DXMT9 Performance Documentation Index"
type: root-index
status: current
updated: 2026-07-25
source: docs/perfomance/overview.md; docs/perfomance/overview-3dmark05-gt1.md; docs/perfomance/overview-3dmark05-gt2.md; docs/perfomance/overview-3dmark05-gt3.md; docs/perfomance/overview-sfiv.md; docs/perfomance/hidden-backend-storage/hidden-backend-storage-shape.42.md
related: docs/perfomance/log.md
---

# DXMT9 Performance Documentation Index

This is the entry point for the `docs/perfomance/` tree.

## Root Documents

- [DXMT9 performance bottleneck model](overview.md) - general CPU/GPU/sync
  model for dxmt9.
- [One frame, end to end](frame-lifecycle.md) - stages, state hand-off,
  thread concurrency, and measured per-stage cost joined in one place.
- [3DMark05 GT1 investigation map](overview-3dmark05-gt1.md) - current
  experiment knowledge graph and domain map.
- [3DMark05 GT2 current baseline](overview-3dmark05-gt2.md) - completed
  frame-sampled baseline and comparison limits.
- [3DMark05 GT3 current baseline](overview-3dmark05-gt3.md) - completed
  frame-sampled baseline and preserved exact V1/V2 comparison.
- [SFIV Benchmark investigation map](overview-sfiv.md) - D3D9 validation,
  current performance improvement, flicker triage, and historical scene-pass
  GPU stall track.
- [Shared performance documentation log](log.md) - root-level structure and
  maintenance history shared by the root overview documents.

## Domain Landings

- [attachment-pixelformat](attachment-pixelformat/index.md)
- [backend-shape-classifiers](backend-shape-classifiers/index.md)
- [baselines](baselines/index.md)
- [const-upload](const-upload/index.md)
- [hidden-backend-storage](hidden-backend-storage/index.md)
- [index-cache-locality](index-cache-locality/index.md)
- [index-reuse-measurement](index-reuse-measurement/index.md)
- [mini-replay-bisection](mini-replay-bisection/index.md)
- [present-pacing](present-pacing/index.md)
- [primitive-reorder-diagnostics](primitive-reorder-diagnostics/index.md)
- [render-pass-store](render-pass-store/index.md)
- [shader-codegen](shader-codegen/index.md)
- [snapshot-cache](snapshot-cache/index.md)
- [state-churn-encode](state-churn-encode/index.md)
- [tvb-mechanism-proof](tvb-mechanism-proof/index.md)
- [vsout-layout](vsout-layout/index.md)
