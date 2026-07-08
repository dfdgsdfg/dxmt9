---
domain: render-pass-store
workload: 3DMark05 GT1
title: "Render-Pass Store — the P1 GPU-memory (tile preservation) track"
type: domain-index
status: current
updated: 2026-07-08
source: docs/perfomance/overview-3dmark05-gt1.md
related: docs/perfomance/render-pass-store/overview.md; docs/perfomance/render-pass-store/log.md
---

# Render-Pass Store — the P1 GPU-memory (tile preservation) track

Latest tracked row: `H14` - The stable role ping-pong also has stable pass-action shape (accepted-counter-sample (depth-read side is `color/depth Load+Store`; opaque depth-write side is `color/depth Clear+Store`)).

## Start Here

- [Current overview](overview.md) - latest conclusion and active gates only.
- [Historical log](log.md) - long-form chronology moved from the old domain root.
- [Root 3DMark05 GT1 map](../overview-3dmark05-gt1.md)

## Recent Leaf Documents

- [render-pass-store-coalesce.05 - Current Frame60 DAG Refresh Keeps H6 Coalesce Candidate Alive](render-pass-store-coalesce.05.md)
- [render-pass-store-coalesce.04 - H6 Benefit Ceiling — 38% of Tile Preservation Eliminable, ~3% of VS-write, FPS Conversion Unsettled](render-pass-store-coalesce.04.md)
- [render-pass-store-coalesce.03 - Per-draw D3D9 Detail Confirms the Re-entry Role Pair from the DAG Dump](render-pass-store-coalesce.03.md)
- [render-pass-store-dontcare.02 - Color Next-Clear StoreActionDontCare Run](render-pass-store-dontcare.02.md)
- [render-pass-store-coalesce.02 - passcoalesce Removes 100% of Distance-1 Re-entries on Real GT1 Frames (observe-time)](render-pass-store-coalesce.02.md)
- [render-pass-store-reentry.01 - Same RT/Depth Re-entry Measurement Run](render-pass-store-reentry.01.md)
- [render-pass-store-reentry-distance.01 - Same-Key Re-entry Distance Distribution](render-pass-store-reentry-distance.01.md)
- [render-pass-store-passchain.01 - Pass-Chain Split Measurement Run](render-pass-store-passchain.01.md)
- [render-pass-store-memoryless.01 - Transient D3D9 RT Memoryless Promotion (design)](render-pass-store-memoryless.01.md)
- [render-pass-store-dontcare.01 - Render-Pass Store Action DontCare Opt-In (design)](render-pass-store-dontcare.01.md)
- [render-pass-store-coalesce.01 - DAG WAR/WAW Edges Make the H6 Re-entry Coalesce Machine-Decidable (frame50)](render-pass-store-coalesce.01.md)
