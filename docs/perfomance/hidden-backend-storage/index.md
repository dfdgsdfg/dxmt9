---
domain: hidden-backend-storage
workload: 3DMark05 GT1 and GT2
title: "Hidden Backend Storage — the central GPU explanation"
type: domain-index
status: current
updated: 2026-07-25
source: docs/perfomance/overview-3dmark05-gt1.md; docs/perfomance/hidden-backend-storage/hidden-backend-storage-shape.40.md
related: docs/perfomance/hidden-backend-storage/overview.md; docs/perfomance/hidden-backend-storage/log.md
---

# Hidden Backend Storage — the central GPU explanation

Latest tracked row: `H50` - alias-aware passcoalesce completes GT1/GT2/GT3
without observed GPU/pipeline failures, preserves the corrected GT2 order, and
does not reproduce the known GT3 quadrant glitch in exact-window captures. The
env-clean default SFIV run also reaches a valid rendered scene with clean
stability counters. The passcoalesce-only L1 policy is promoted; broader
production options remain off.

## Start Here

- [Current overview](overview.md) - latest conclusion and active gates only.
- [Historical log](log.md) - long-form chronology moved from the old domain root.
- [Root 3DMark05 GT1 map](../overview-3dmark05-gt1.md)

## Recent Leaf Documents

- [hidden-backend-storage-shape.40 - Alias-Aware Pass Coalescing Clears the Default-Promotion Wild Gate](hidden-backend-storage-shape.40.md)
- [hidden-backend-storage-shape.39 - GT2 R32F Liveness Exposes a Surface-Alias Hazard Gap in Pass Coalescing](hidden-backend-storage-shape.39.md)
- [hidden-backend-storage-shape.38 - GT2 R32F Alpha-Test Draws Are Already at the Index-Locality Floor](hidden-backend-storage-shape.38.md)
- [hidden-backend-storage-shape.37 - GT2 Black Draws Are a Depth Prepass, Not the Main Hidden-Write Owner](hidden-backend-storage-shape.37.md)
- [hidden-backend-storage-shape.36 - GT2 Full-Frame Native Replay Preserves the GPU Ceiling Without Partial Renders](hidden-backend-storage-shape.36.md)
- [hidden-backend-storage-shape.35 - Current Shader Dump Join Keeps the Hidden Owner Below Visible VSOut](hidden-backend-storage-shape.35.md)
- [hidden-backend-storage-shape.34 - Fragmentless Depth-Only Keep-VSOut Route Passes Equality but Fails Xcode Counter Gate](hidden-backend-storage-shape.34.md)
- [hidden-backend-storage-shape.33 - Current Xcode/DXMT Attribution Narrows The Next Backend Gate](hidden-backend-storage-shape.33.md)
- [hidden-backend-storage-shape.32 - Recovered Capture Layer Reconfirms Frame60 Hidden VS Write Dominance](hidden-backend-storage-shape.32.md)
- [hidden-backend-storage-shape.31 - Current System Trace Refresh Reconfirms Vertex-Heavy Programmable Routes While Gputrace Remains Layer-Blocked](hidden-backend-storage-shape.31.md)
- [hidden-backend-storage-shape.30 - GPU Efficiency Ceiling Is Separate From Wall-Clock FPS Ownership](hidden-backend-storage-shape.30.md)
- [hidden-backend-storage-shape.29 - Encoder-Summary Route Counters Remove Indexed Per-Draw Requirement From Sidecars](hidden-backend-storage-shape.29.md)
- [hidden-backend-storage-shape.28 - Seq-Range System Trace Sidecar Adds Route Verdicts Without Capture-Layer Startup Mutation](hidden-backend-storage-shape.28.md)
- [hidden-backend-storage-shape.27 - Metal System Trace Keeps the Post-Compact Bottleneck Vertex-Stage Dominated](hidden-backend-storage-shape.27.md)
- [hidden-backend-storage-shape.26 - Fragmentless Depth-Only Route Smoke Reaches the Full 60/0 Pass](hidden-backend-storage-shape.26.md)
- [hidden-backend-storage-shape.25 - Programmable Route Feasibility Splits Depth-Only from Textured Hot Rows](hidden-backend-storage-shape.25.md)
- [hidden-backend-storage-shape.24 - Tile-FFP Expansion Still Requires a Programmable Route](hidden-backend-storage-shape.24.md)
