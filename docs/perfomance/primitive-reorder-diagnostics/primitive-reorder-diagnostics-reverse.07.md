---
domain: primitive-reorder-diagnostics
workload: 3DMark05 GT1
subcategory: reverse
order: 07
title: Row-Set Hotrows Reverse
date: 2026-06-02
type: experiment-run
status: rejected
source: specs/perfomance.plan.md#L9941-L10036
---

# Row-Set Hotrows Reverse

**Question / hypothesis.** Apply the row-set selector to the full baseline
top-four hot rows `60/0,60/1,60/3,60/4` at once. Does reversing the whole hot set
reduce the hidden VS-write bucket?

**Method.** `run_3dmark05_perf_probe.sh --suffix reverse-hotrows-gputrace-r1
--probe-reverse-indexed-triangles --probe-reverse-indexed-triangles-rows 60/0,60/1,60/3,60/4`
with shape/coverage gates. No-gputrace validated selector scope: 722 probed
draws over the four rows (`6,261,402B`), `60/2` and others untouched.

**Result.** Shape gate **rejected**: top rows collapse to `60/0,60/1,60/2,60/8`
(requested `60/3`/`60/4` shrink to one tiny draw each; unprobed `60/2` becomes
largest). Top draws `711 -> 593` (`-16.60%`); dxmt vertices `-19.66%`; triangles
`-19.66%`. Xcode: 10 encoders, 599 draws, `2,507,922` vertices, `25.73ms`.
Aggregate looks strong (hot/top GPU `-25.07%`, hot/top VS write `-29.28%`) but is
shape-contaminated. Only `60/0`/`60/1` are shared; mixed: `60/1` bytes/inv
`-11.12%` but `60/0` GPU `+17.22%`, VS write `+8.29%`.

**Verdict.** Rejected (shape-gate fail). Like full reverse
([primitive-reorder-diagnostics-reverse.01](primitive-reorder-diagnostics-reverse.01.md)), the big aggregate drop describes
a *different submitted frame*, not a legal optimization. Useful only as another
classifier showing the hidden bucket moves dramatically with row membership +
geometry. Any real candidate must preserve the hot-row set and geometry first.

**Related.** [primitive-reorder-diagnostics](../primitive-reorder-diagnostics.md) · prev: [primitive-reorder-diagnostics-reverse.06](primitive-reorder-diagnostics-reverse.06.md)
· next: [primitive-reorder-diagnostics-reverse.08](primitive-reorder-diagnostics-reverse.08.md) · gate source: [primitive-reorder-diagnostics-reverse.04](primitive-reorder-diagnostics-reverse.04.md)
· [hidden-backend-storage](../hidden-backend-storage.md) · [index-reuse-measurement](../index-reuse-measurement.md).
