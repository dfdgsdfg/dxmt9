---
domain: primitive-reorder-diagnostics
workload: 3DMark05 GT1
subcategory: reverse
order: 06
title: Row 60/1 Reverse
date: 2026-06-02
type: experiment-run
status: rejected
source: specs/perfomance.plan.md#L9858-L9940
---

# Row 60/1 Reverse

**Question / hypothesis.** Repeat the single-row reverse against hot row `60/1`
(opaque depth-writing). Does it reduce the hidden VS-write bucket?

**Method.** `run_3dmark05_perf_probe.sh --suffix reverse-row-60-1-gputrace-r1
--probe-reverse-indexed-triangles --probe-reverse-indexed-triangles-row 60/1`
with the standard shape + coverage gates. Captured image was visually correct.

**Result.** Shape gate **rejected** the run as an optimization proof: top row
set drifts to `60/0,60/1,60/3,60/11`; top dxmt vertices `3,121,680 -> 2,922,468`
(`-6.38%`, allowed `<=5%`); triangles `-6.38%`. Xcode reported 19 render
encoders, 865 draws, `3,443,010` vertices, `35.70ms` GPU. The attractive
aggregate (hot/top VS write `-9.04%`) compares a *different* hot-row set and
less geometry, so it is not causal. Shared-row deltas show no target win: target
`60/1` GPU `8.252 -> 8.610ms` (`+4.34%`), VS write `437.404 -> 437.878MiB`
(`+0.11%`). 156 reversed draws, `~1.341MiB` transient IB.

**Verdict.** Rejected (shape-gate fail + flat target). Combined with the clean
`60/3` negative ([[primitive-reorder-diagnostics-reverse.05]]), simple single-row
reversal is no longer a candidate fix path. The aggregate "win" is frame-shape
contamination, not VS-write reduction.

**Related.** [[primitive-reorder-diagnostics]] · prev: [[primitive-reorder-diagnostics-reverse.05]]
· next: [[primitive-reorder-diagnostics-reverse.07]] · gate source: [[primitive-reorder-diagnostics-reverse.04]]
· later clean rerun: [[primitive-reorder-diagnostics-reverse.18]] · [[hidden-backend-storage]].
