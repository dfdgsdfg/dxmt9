---
domain: primitive-reorder-diagnostics
workload: 3DMark05 GT1
subcategory: minindex
order: 03
title: Current-Head Geometry Payload Min-Index Rerun
date: undated
type: experiment-run
status: inconclusive
source: specs/perfomance.plan.md#L15191-L15321
---

# Current-Head Geometry Payload Min-Index Rerun

**Question / hypothesis.** Run min-index ordering through the row-local *mini
replay* harness (a reduced 3-draw `60/2` top-group bundle) under Xcode counters.
Does the locality axis reproduce the full-frame hidden VS Buffer Device Memory
Bytes Written bucket in the reduced replay?

**Method.** `run_3dmark05_mini_replay.py <frame60-mini-replay-manifest.json>
--primitive-order sort-min-index --draw-order original --run`, on the 3-draw
manifest (`1,752B` index, `12,672B` stream0; draws 234..236, VS
`0x7836c3b4c98a465b` / PS `0x11cc89f85cc54054`). Xcode capture/export of the
`mini-replay-passshape-sort-min-index-r1.gputrace`, compared against the
`mini-replay-passshape-r1` original-order replay.

**Result.** Original vs `sort-min-index`: GPU time `1147.851 → 1099.089us`
(`-48.762us`); VS invocations `18,362 → 19,519` (`+1,157`); VS buffer device
*writes* `0B → 0B` (unchanged, still zero); VS device bytes written
`859,648 → 822,848B` (`-36,800B`); tiled vertex bytes `262,144B` unchanged;
cull/clip limiters `70.18/67.95% → 78.71/76.38%` (≈+8.5%).

**Verdict.** Inconclusive (small replay). Primitive order is a real
backend-shape input even in the reduced replay — it moves GPU time, VS
invocations, visible VS device writes, and cull/clip limiters — but it still
reports **0B** for the named `VS Buffer Device Memory Bytes Written` counter, so
the 3-draw mini replay does not reach the full-frame condition that maps hidden
backend write into Xcode's named VS-buffer bucket. The next locality proof needs
a larger same-row/material window or a full-frame scoped probe, not another
3-draw mini replay.

**Related.** [primitive-reorder-diagnostics](index.md) · prior:
[primitive-reorder-diagnostics-minindex.02](primitive-reorder-diagnostics-minindex.02.md) · next:
[primitive-reorder-diagnostics-minindex.04](primitive-reorder-diagnostics-minindex.04.md) (16-draw geometry-locked rerun) ·
[mini-replay-bisection](../mini-replay-bisection/index.md) (harness, payload capture, manifest builder) ·
[hidden-backend-storage](../hidden-backend-storage/index.md) (TVB counter stays 0B at small replay scale) ·
[index-cache-locality](../index-cache-locality/index.md) (semantic-safe successor).
