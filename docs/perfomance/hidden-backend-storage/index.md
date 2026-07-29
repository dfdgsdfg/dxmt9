---
domain: hidden-backend-storage
workload: 3DMark05 GT1 and GT2
title: "Hidden Backend Storage — the central GPU explanation"
type: domain-index
status: current
updated: 2026-07-25
source: docs/perfomance/overview-3dmark05-gt1.md; docs/perfomance/hidden-backend-storage/hidden-backend-storage-shape.42.md
related: docs/perfomance/hidden-backend-storage/overview.md; docs/perfomance/hidden-backend-storage/log.md
---

# Hidden Backend Storage — the central GPU explanation

Latest tracked row: `H52` - cross-chunk DCE can remove GT2's final R32F pass,
but waiting for the successor proof cuts instantaneous FPS by `24.6%`. The
accepted no-wait design exposes `498` safe prefixes yet finds only one
already-ready successor in `536` frames and omits just `30` commands. DCE
therefore remains opt-in and provides no current GT2 performance win.

## Start Here

- [Current overview](overview.md) - latest conclusion and active gates only.
- [Historical log](log.md) - long-form chronology moved from the old domain root.
- [Root 3DMark05 GT1 map](../overview-3dmark05-gt1.md)

## Recent Leaf Documents

- [hidden-backend-storage-shape.42 - Cross-Chunk DCE Removes the R32F Pass but Cannot Wait for GT2 Proof](hidden-backend-storage-shape.42.md)
- [hidden-backend-storage-shape.41 - GT2 Final R32F Pass Is Observationally Dead but Needs Cross-Chunk Scheduling](hidden-backend-storage-shape.41.md)
- [hidden-backend-storage-shape.39 - GT2 R32F Liveness Exposes a Surface-Alias Hazard Gap in Pass Coalescing](hidden-backend-storage-shape.39.md)
- [hidden-backend-storage-shape.38 - GT2 R32F Alpha-Test Draws Are Already at the Index-Locality Floor](hidden-backend-storage-shape.38.md)
- [hidden-backend-storage-shape.37 - GT2 Black Draws Are a Depth Prepass, Not the Main Hidden-Write Owner](hidden-backend-storage-shape.37.md)
- [hidden-backend-storage-shape.36 - GT2 Full-Frame Native Replay Preserves the GPU Ceiling Without Partial Renders](hidden-backend-storage-shape.36.md)
- [hidden-backend-storage-shape.35 - Current Shader Dump Join Keeps the Hidden Owner Below Visible VSOut](hidden-backend-storage-shape.35.md)
- [hidden-backend-storage-shape.30 - GPU Efficiency Ceiling Is Separate From Wall-Clock FPS Ownership](hidden-backend-storage-shape.30.md)
