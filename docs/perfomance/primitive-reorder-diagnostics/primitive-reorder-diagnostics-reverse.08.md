---
domain: primitive-reorder-diagnostics
workload: 3DMark05 GT1
subcategory: reverse
order: 08
title: Row 60/4 Reverse
date: 2026-06-02
type: experiment-run
status: rejected
source: specs/perfomance.plan.md#L10037-L10124
---

# Row 60/4 Reverse

**Question / hypothesis.** Test the remaining baseline hot row `60/4` (the
depth-read / textured / mostly-alpha-blended row) under strict shape gates.
Does reversing the whole `60/4` row reduce the hidden VS-write bucket?

**Method.** `run_3dmark05_perf_probe.sh --suffix reverse-row-60-4-gputrace-r1
--probe-reverse-indexed-triangles --probe-reverse-indexed-triangles-row 60/4`
with shape/coverage/PSO gates. Image visually normal; all gates PASSED
(cleanest single-row result after `60/3`). 277 reversed draws, `~2.276MiB`
transient IB.

**Result.** GPU `34.391 -> 37.260ms` (`+8.34%`); hot/top GPU `+8.40%`; hot/top
VS buffer write `1472.747 -> 1566.541MiB` (`+6.37%`); VS bytes/inv `+1.41%`;
draws `+3.09%`, vertices/triangles `+3.61%`. Target row `60/4` regresses most:
GPU `9.031 -> 10.171ms` (`+12.63%`), VS write `370.276 -> 444.367MiB` (`+20.01%`,
`+74.090MiB`), invocations `+8.85%`, bytes/inv `+10.25%`. Injected transient IB
is only `~2.255MiB`, far below the `+93.795MiB` matched-row VS-write regression
— the movement is GPU-side hidden VS/tiler/backend.

**Verdict.** Rejected (strong negative). Single-row reversal is now rejected for
both shape-gate-clean rows: `60/3` was flat/slightly negative, `60/4` is a clear
regression. The production fix cannot be "reverse this row." This pushed the
investigation toward narrower per-material/state-class axes
([primitive-reorder-diagnostics-reverse.09](primitive-reorder-diagnostics-reverse.09.md) tooling) that keep row and geometry
gates fixed.

**Related.** [primitive-reorder-diagnostics](index.md) · prev: [primitive-reorder-diagnostics-reverse.07](primitive-reorder-diagnostics-reverse.07.md)
· next (class tooling): [primitive-reorder-diagnostics-reverse.09](primitive-reorder-diagnostics-reverse.09.md) · contrasts: [primitive-reorder-diagnostics-reverse.05](primitive-reorder-diagnostics-reverse.05.md)
· [hidden-backend-storage](../hidden-backend-storage/index.md) · [index-reuse-measurement](../index-reuse-measurement/index.md).
