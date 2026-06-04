---
domain: primitive-reorder-diagnostics
workload: 3DMark05 GT1
subcategory: reverse
order: 10
title: Row/Class 60/4 Alpha Reverse
date: 2026-06-02
type: experiment-run
status: rejected
source: specs/perfomance.plan.md#L11010-L11082
---

# Row/Class 60/4 Alpha Reverse

**Question / hypothesis.** First class-scoped Xcode candidate. Is the `60/4`
alpha-blended subset the owner of the primitive-order signal? `60/4` is the
depth-read/textured/mostly-alpha row where whole-row reverse regressed
([[primitive-reorder-diagnostics-reverse.08]]).

**Method.** `run_3dmark05_perf_probe.sh --suffix reverse-row-60-4-alpha-gputrace-r1
--probe-reverse-indexed-triangles --probe-reverse-indexed-triangles-row 60/4
--probe-reverse-indexed-triangles-class alpha-blend` with the full strict shape +
coverage + PSO gate set. All gates PASSED (row set `60/0,60/1,60/3,60/4` stable,
drift <5%).

**Result.** GPU `34.391 -> 33.203ms` (`-3.45%`); hot/top GPU `-3.44%`; hot VS
buffer write `1472.747 -> 1473.132MiB` (`+0.03%`); hot unexplained write
`-0.10%`; VS bytes/inv `-0.01%`; draws `+0.70%`. Target row `60/4`: GPU
`9.031 -> 8.513ms` (`-5.73%`), VS write `370.276 -> 370.722MiB` (`+0.12%`),
invocations `+0.09%`, bytes/inv `+0.03%`. 243 alpha draws, `~1.95MiB` transient IB.

**Verdict.** Rejected. Clean same-frame run: a small GPU-time win, but the
first-order counter (hot VS write, VS bytes/inv, hidden backend estimate) is
flat. The broad `60/4` alpha-blended subset is NOT the owner of the hidden
VS-write bucket; the time win is likely secondary ordering/cache noise.

**Related.** [[primitive-reorder-diagnostics]] · tooling: [[primitive-reorder-diagnostics-reverse.09]]
· next: [[primitive-reorder-diagnostics-reverse.11]] · [[hidden-backend-storage]]
· [[vsout-layout]] (bytes/inv unchanged vs visible width).
