---
domain: render-pass-store
workload: 3DMark05 GT1
title: "Render-Pass Store — the P1 GPU-memory (tile preservation) track - Current Overview"
type: domain-overview
status: current
updated: 2026-07-08
source: docs/perfomance/render-pass-store/log.md; docs/perfomance/overview-3dmark05-gt1.md
related: docs/perfomance/render-pass-store/index.md; docs/perfomance/render-pass-store/log.md
---

# Render-Pass Store — the P1 GPU-memory (tile preservation) track - Current Overview

> Current, compact view for this performance domain. Historical detail from the former
> top-level `render-pass-store.md` overview is preserved in [log](log.md). Domain landing: [index](index.md).

## Scope

This domain owns the **P1 GPU-memory track**: the repeated tile Store/Load
preservation caused by re-entering the same render target / depth attachment after
an intervening different pass. It covers the run-level measurement of that
re-entry budget, the `StoreActionDontCare` live-out proofs that *should* avoid the
stores, and the pass-chain split that explains why those cheap proofs do not fire
on GT1. The central GT1 cost owner is still the P0 hidden vertex-stage/TVB write
bucket ([hidden-backend-storage](../hidden-backend-storage/index.md)); this track is large in absolute GPU-memory
terms (~62 GB tile preservation) but secondary in the priority DAG, and its only
real lever — dependency-aware pass reordering/coalescing — is still **open**.

## Latest Conclusions

> **Every row below cites the single leaf
> [render-pass-store-reentry-distance.01](render-pass-store-reentry-distance.01.md),
> now marked `outdated: evidence-missing`.** The counter samples quoted here are
> last measurements; the artifacts they came from are gone, so the row counts
> cannot be re-derived. They are kept because they record which ping-pong
> explanations were already eliminated.

| # | Hypothesis | Verdict | Evidence |
|---|---|---|---|
| H10 | Top one-hop ping-pong is blocked by direct attachment-as-texture reads between B and A | rejected-counter-sample (`3561/3561` raw top rows have `B reads A=none`, `A reads B=none`) | [render-pass-store-reentry-distance.01](render-pass-store-reentry-distance.01.md) |
| H11 | Top one-hop ping-pong is kept live by present/clear/helper ops | rejected-counter-sample (`3569/3569` raw top rows are `BlockDrawTarget` + `BlockDrawDepth`, not present/clear/helper) | [render-pass-store-reentry-distance.01](render-pass-store-reentry-distance.01.md) |
| H12 | Top one-hop ping-pong is blocked by distant live-out reuse rather than immediate target reuse | rejected-counter-sample (dominant top patterns report `B next touch=color/depth 1`, `A next touch=color/depth 1`) | [render-pass-store-reentry-distance.01](render-pass-store-reentry-distance.01.md) |
| H13 | The immediate ping-pong is role-random and needs a global scheduler | rejected-counter-sample (encoder join shows stable role pairs: textured-depth-read <-> opaque-depth-write and screen-blend-depth-read <-> opaque-depth-write) | [render-pass-store-reentry-distance.01](render-pass-store-reentry-distance.01.md) |
| H14 | The stable role ping-pong also has stable pass-action shape | accepted-counter-sample (depth-read side is `color/depth Load+Store`; opaque depth-write side is `color/depth Clear+Store`) | [render-pass-store-reentry-distance.01](render-pass-store-reentry-distance.01.md) |

## Current Navigation

- [Domain index](index.md)
- [Historical log](log.md)
- [Root 3DMark05 GT1 map](../overview-3dmark05-gt1.md)

## Recent Leaf Documents

> 7 of the 8 leaves listed below are marked `outdated:` and open with a banner naming the ground. They are history, not re-checkable evidence.

- [render-pass-store-coalesce.05 - Current Frame60 DAG Refresh Keeps H6 Coalesce Candidate Alive](render-pass-store-coalesce.05.md)
- [render-pass-store-coalesce.04 - H6 Benefit Ceiling — 38% of Tile Preservation Eliminable, ~3% of VS-write, FPS Conversion Unsettled](render-pass-store-coalesce.04.md)
- [render-pass-store-coalesce.03 - Per-draw D3D9 Detail Confirms the Re-entry Role Pair from the DAG Dump](render-pass-store-coalesce.03.md)
- [render-pass-store-dontcare.02 - Color Next-Clear StoreActionDontCare Run](render-pass-store-dontcare.02.md)
- [render-pass-store-coalesce.02 - passcoalesce Removes 100% of Distance-1 Re-entries on Real GT1 Frames (observe-time)](render-pass-store-coalesce.02.md)
- [render-pass-store-reentry.01 - Same RT/Depth Re-entry Measurement Run](render-pass-store-reentry.01.md)
- [render-pass-store-reentry-distance.01 - Same-Key Re-entry Distance Distribution](render-pass-store-reentry-distance.01.md)
- [render-pass-store-passchain.01 - Pass-Chain Split Measurement Run](render-pass-store-passchain.01.md)
