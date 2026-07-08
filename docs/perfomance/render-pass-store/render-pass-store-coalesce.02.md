---
domain: render-pass-store
workload: 3DMark05 GT1
subcategory: coalesce
order: 02
title: passcoalesce Removes 100% of Distance-1 Re-entries on Real GT1 Frames (observe-time)
date: 2026-06-09
type: experiment-run
status: accepted-analysis
source: traces/app-d3d9-3dmark05-dagcheck-coalesce/analysis/dag/ (dag-frame40..60-chunk*-{pre,post}-opt.json), experiments/output/app-d3d9-3dmark05-dagcheck-coalesce/3dmark05-perf-summary.md, src/dxmt9/render/dag_observer.cpp, specs/d3d9-renderer/requirements.md (R-BACK-39.7 DXMT9_RENDERER_DUMP_DAG_OPTIMIZE)
---

# passcoalesce Removes 100% of Distance-1 Re-entries on Real GT1 Frames (observe-time)

**Question / hypothesis.** [render-pass-store-coalesce.01](render-pass-store-coalesce.01.md) showed the DAG makes
the H6 re-entry coalesce *machine-decidable* on one frame. Run on a **real
GT1 capture**: when `passcoalesce` is actually applied to these frames' DAGs, how
many of the H6 distance-1 `A→B→A` re-entries ([render-pass-store-reentry-distance.01](render-pass-store-reentry-distance.01.md))
does it coalesce, and how much pass/round-trip reduction does that represent?

**Method.** Device-gated run: 3DMark05 GT1 on the **default `traditional`
encode path** (`status: pass`, 1,680 presents) with the new observe-time
selector `DXMT9_RENDERER_DUMP_DAG_OPTIMIZE=passcoalesce` (R-BACK-39.7) and a
`DXMT9_RENDERER_DUMP_DAG` window of frame 50 ±10 (frames 40–60). The selector
runs `passcoalesce` only on the **post-opt DAG snapshot** — it is analysis-only
and **does not touch the Metal encode** (which stays byte-identical via
`encoders::encodeChunk`). Each frame's pre-opt JSON is the un-coalesced baseline;
post-opt is after `lifetime → passcoalesce → loadstore`. Compared pass counts and
`A→B→A` render-pass re-entry counts across the 21 frames.

**Result.**

| Metric (21 frames, 40–60) | pre-opt | post-opt | Δ |
|---|---:|---:|---|
| Render/Present passes | 212 | 190 | **−22 (−10.4%)** |
| `A→B→A` render-pass re-entries | 22 | **0** | **−22 (all removed)** |

A clean 1:1: every removed pass is one coalesced re-entry pair — `passcoalesce`
merged **exactly** the distance-1 re-entries and nothing else (no over-merge).

frame50 (10 → 9 passes):

```
PRE   P0 RT..08/D..01 draws 0..14    P1 RT..05/D..04 draws 14..56    P2 RT..08/D..01 draws 56..135
POST  P0 RT..08/D..01 draws 0..93 (Clear/Store)   P1 RT..05/D..04 draws 93..135   ...
```

P0 and P2 (same `AttachmentSet` RT `..08` + depth `..01`) coalesced into one pass
(draws `0..14` ⧺ `56..135` = `0..93`); the edge-free intervening P1 (RT `..05`)
relocated after. The merged pass does **one** `Clear` + all draws + **one**
`Store`, eliminating P0's color+depth `Store` and P2's color+depth `Load` — the
H14 `Clear+Store ↔ Load+Store` round-trip on both attachments.

**Ties to the measured budget.** This run's counters:
`render_pass_same_key_reentry = 3,792` (`distance_1 = 3,438`),
`render_pass_tile_preservation_bytes = 211.9 GB` over 1,680 presents. The 22
re-entries in the 21-frame sample (~1.05/frame) are the same distance-1 class the
counter measures run-wide (~3,438), and `passcoalesce` coalesces **100%** of
them — so the structural lever targets exactly the re-entry tile-preservation
budget that [render-pass-store-reentry-distance.01](render-pass-store-reentry-distance.01.md) (H8/H14) and
[render-pass-store-passchain.01](render-pass-store-passchain.01.md) (H5/H6) identified.

**Limits (honest — this is observe-time, not a delivered win).**
1. **No GPU saving yet.** The encode is byte-identical (`encodeChunk`); the
   coalesce exists only in the post-opt DAG snapshot. Delivering the bandwidth
   saving requires `fg_linearizer::executeLinearization` to drive the encode —
   the device-gated frontier.
2. **Byte-equal proof owed.** That all 22 are edge-safe (relocatable intervening
   pass, no intervening writer — consistent with H10/H11/H12 rejections) is a
   *structural* safety argument from the hazard DAG, not a rendered-output proof.
   The merged-pass output must still be shown byte-equal on device.
3. **Relocation reorders independent passes.** P1 moved after the merged pair;
   safe by the edge model (P1 edge-free), but its submission-order change is
   unproven on GPU until (1).
4. Per-chunk window only; full-frame/full-run coalesce projection needs the
   executor + the preservation-bytes counter, not the DAG alone.

**Verdict.** Accepted as analysis. On a real GT1 capture, `passcoalesce` over the
complete hazard DAG coalesces **100% of the dominant distance-1 re-entry class
(22/22 sampled), −10.4% render passes**, exactly the H6 lever the
render-pass-store track has been pointing at — confirming the opportunity is
large and structurally realizable. The remaining step is the device-gated
executor: drive the Metal encode from the coalesced linearization and prove
byte-equal output + measure the preservation-byte reduction against the
211.9 GB budget.

**Related.** [render-pass-store-coalesce.01](render-pass-store-coalesce.01.md) (DAG/WAW makes it decidable) ·
[render-pass-store-passchain.01](render-pass-store-passchain.01.md) (H5/H6) ·
[render-pass-store-reentry-distance.01](render-pass-store-reentry-distance.01.md) (H8/H14) · [render-pass-store](index.md) ·
[overview-3dmark05-gt1](../overview-3dmark05-gt1.md).
