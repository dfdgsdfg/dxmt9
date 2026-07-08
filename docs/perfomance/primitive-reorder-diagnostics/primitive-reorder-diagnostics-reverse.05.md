---
domain: primitive-reorder-diagnostics
workload: 3DMark05 GT1
subcategory: reverse
order: 05
title: Row 60/3 Reverse
date: 2026-06-02
type: experiment-run
status: rejected
source: specs/perfomance.plan.md#L9791-L9857
---

# Row 60/3 Reverse

**Question / hypothesis.** First single-row reverse clean enough for same-frame
interpretation. Does reversing only hot row `60/3` (an opaque depth-writing
triangle-list row) reduce the hidden VS-buffer-write bucket?

**Method.** `run_3dmark05_perf_probe.sh --suffix reverse-row-60-3-gputrace-r1
--frame 60 --probe-reverse-indexed-triangles --probe-reverse-indexed-triangles-row 60/3
--measure-index-reuse --top 4 --hot-gpu-share 95 --baseline-joined <measure-index-cache>
--require-top-row-key-match --max-top-draw-call-delta-ratio 0.05
--max-top-vertex-count-delta-ratio 0.05 --max-top-triangle-delta-ratio 0.05`
(plus Xcode/dxmt coverage + PSO gates). All gates PASSED.

**Result.** GPU `34.391 -> 35.370ms` (`+2.85%`); hot/top GPU `+3.02%`; hot/top
VS buffer write `1472.747 -> 1473.157MiB` (`+0.03%`); VS bytes/inv `+0.02%`;
draws `+0.70%`, vertices/triangles `+0.02%`. Target row `60/3`: GPU
`10.662 -> 10.746ms` (`+0.79%`), VS write `437.402 -> 437.873MiB` (`+0.11%`),
invocations `-0.06%`. 169 reversed draws, `~1.46MiB` transient IB.

**Verdict.** Rejected (clean negative). The first shape-gate-clean reverse run:
reversing `60/3` alone does NOT reduce the hidden VS-write bucket and slightly
regresses GPU time. The full-reverse win cannot be explained as a simple per-row
order improvement for `60/3` — it must be a broader visibility / tile-coverage /
hot-row-membership / multi-row effect.

**Related.** [primitive-reorder-diagnostics](../primitive-reorder-diagnostics.md) · prev (tooling): [primitive-reorder-diagnostics-reverse.04](primitive-reorder-diagnostics-reverse.04.md)
· next: [primitive-reorder-diagnostics-reverse.06](primitive-reorder-diagnostics-reverse.06.md) · contrasts: [primitive-reorder-diagnostics-reverse.01](primitive-reorder-diagnostics-reverse.01.md)
· [hidden-backend-storage](../hidden-backend-storage.md).
