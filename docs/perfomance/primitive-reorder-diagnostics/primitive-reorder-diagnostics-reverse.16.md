---
domain: primitive-reorder-diagnostics
workload: 3DMark05 GT1
subcategory: reverse
order: 16
title: Scissor Rectangle/Tiling Probe
date: 2026-06-02
type: experiment-run
status: rejected
source: specs/perfomance.plan.md#L11810-L11900
---

# Scissor Rectangle/Tiling Probe

**Question / hypothesis.** The draw-sample diff
([primitive-reorder-diagnostics-reverse.15](primitive-reorder-diagnostics-reverse.15.md)) showed the only difference between
the historical positive and current-HEAD reruns was the scissor rectangle shape.
Keep scissor enabled but override the four `60/4 large4096 && alpha-blend &&
scissor` rectangles to the reference `0,0,190,553`. Does rectangle/tile-coverage
shape own the historical win?

**Method.** Scissor-rect override probe (preserve scissor enablement, normalize
rectangle) on the same 4 draws, with strict finalizer gates against
`measure-index-cache-gputrace-r1`. Smoke confirmed 4 applied / 38 skipped,
`23,128 px` area-delta accumulator. All gates PASSED (same hot rows, identical
draw/vertex/triangle counts).

**Result.** Total GPU `34.391 -> 36.362ms` (`+5.73%`); hot GPU `+5.93%`; hot VS
buffer write `1472.747 -> 1472.874MiB` (`+0.01%`); VS bytes/inv `+0.01%`; hidden
backend estimate `1455.978MiB` (still dominant). Target `60/4` GPU `-1.01%`, VS
write `+0.03%`; unrelated rows `60/3 +8.85%`, `60/1 +8.67%`, `60/0 +7.44%` GPU
while their VS write stays flat.

**Verdict.** Rejected. The scissor rectangle/tile-coverage hypothesis is negative:
modifying the four rects exactly did not drop the hot hidden VS/tiler/backend
bucket. Together with broad `DXMT_DISABLE_SCISSOR`, targeted screen-blend index
order, and the current diagnostic reorder, every scissor angle leaves the
`~1.47GiB` bucket unchanged. Scissor is a row/material-shape classifier, not the
root cause.

**Related.** [primitive-reorder-diagnostics](index.md) · from: [primitive-reorder-diagnostics-reverse.15](primitive-reorder-diagnostics-reverse.15.md)
· next: [primitive-reorder-diagnostics-reverse.17](primitive-reorder-diagnostics-reverse.17.md) · [backend-shape-classifiers](../backend-shape-classifiers/index.md) (disable-scissor)
· [hidden-backend-storage](../hidden-backend-storage/index.md).
