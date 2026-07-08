---
domain: primitive-reorder-diagnostics
workload: 3DMark05 GT1
subcategory: minindex
order: 01
title: Min-Index Triangle Reorder Scout
date: undated
type: scout
status: rejected
source: specs/perfomance.plan.md#L13215-L13323
---

# Min-Index Triangle Reorder Scout

**Question / hypothesis.** After contiguous span-split failed, does changing
triangle *order* inside hot draws — sorting triangle-list primitives by
`(minIndex, maxIndex, originalOrder)` — improve post-transform vertex-cache
locality (and so reduce hidden VS/backend write)? Diagnostic-only, same draw
count, same primitive count.

**Method.** `run_3dmark05_perf_probe.sh --suffix sort-hotrows-minindex-span600k-scout-r1
--frame 60 --no-gputrace --encoder-breakdown-seq 60
--probe-sort-indexed-triangles-by-min-index
(`DXMT9_PROBE_SORT_INDEXED_TRIANGLES_BY_MIN_INDEX`)
--probe-reverse-indexed-triangles-rows 60/0,60/1,60/2,60/3,60/4
--probe-reverse-indexed-triangles-classes large4096
--probe-reverse-indexed-triangles-stream0-span-min 600000 --measure-index-reuse`.
Cheap no-gputrace scout before spending Xcode time; emits the same
`dxmt9-perf-indexed-probe-draw` before/after locality fields as the reverse probe.

**Result.** `7` applied draws, `158,354` primitives (both unchanged); `950,124B`
reordered transient IB uploaded. Cache miss 16 `300,027 → 315,777` (`+5.25%`);
miss 32 `274,722 → 308,357` (`+12.24%`); miss 64 `255,598 → 300,769` (`+17.67%`).
Adjacent index-delta sum `-10.77%`; backward jumps `-4.16%`; triangle index-span
sum and stream0 span sum unchanged. Per-row cache64 worsened `+17.67%` uniformly
(`60/4`: 3 draws, `109,542 → 128,901`). Screenshot was a normal GT1 frame.

**Verdict.** Rejected — useful negative scout, not a VS-write candidate. Sorting
by index range reduces adjacent deltas but *worsens* estimated post-transform
cache behavior; the original 3DMark05 order is already more vertex-cache
friendly. Do not spend gputrace time here. The next primitive-order experiment
must be cache-aware if it changes order.

**Related.** [primitive-reorder-diagnostics](index.md) · prior reverse-class results:
[primitive-reorder-diagnostics-split.03](primitive-reorder-diagnostics-split.03.md) · next:
[primitive-reorder-diagnostics-minindex.02](primitive-reorder-diagnostics-minindex.02.md) (cache-aware successor) ·
[index-reuse-measurement](../index-reuse-measurement/index.md) (LRU16/32/64 miss model) ·
[index-cache-locality](../index-cache-locality/index.md) (the semantic-safe cache-aware path this motivates).
