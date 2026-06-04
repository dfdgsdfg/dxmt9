---
domain: primitive-reorder-diagnostics
workload: 3DMark05 GT1
subcategory: reverse
order: 11
title: Row/Class 60/4 Large4096 Reverse
date: 2026-06-02
type: experiment-run
status: inconclusive
source: specs/perfomance.plan.md#L11083-L11218
---

# Row/Class 60/4 Large4096 Reverse

**Question / hypothesis.** Does the primitive-size/locality component inside the
same `60/4` depth-read/textured/mostly-alpha row own the broad primitive-order
signal? Reverse only the 19 `large4096` (>=4096 primitive) draws in `60/4`.

**Method.** `run_3dmark05_perf_probe.sh --suffix reverse-row-60-4-large4096-gputrace-r1
--probe-reverse-indexed-triangles --probe-reverse-indexed-triangles-row 60/4
--probe-reverse-indexed-triangles-class large4096` with strict shape + coverage +
PSO gates. All gates PASSED.

**Result.** First clean narrow probe that **moves the first-order counter**. GPU
`34.391 -> 32.177ms` (`-6.44%`); hot/top GPU `-6.49%`; hot VS buffer write
`1472.747 -> 1362.858MiB` (`-7.46%`); hot unexplained write `-7.52%`; VS bytes/inv
`856.265 -> 809.005B` (`-5.52%`). Target row `60/4`: GPU `-15.20%`, VS write
`370.276 -> 287.596MiB` (`-22.33%`), bytes/inv `588.7 -> 476.8B` (`-19.02%`).
Matched-row VS-write delta `-109.888MiB`: only `-17.821MiB` from fewer
invocations, `-92.067MiB` from lower bytes/invocation. Only 19 draws reordered,
`~0.599MiB` transient IB, yet `60/0` also improved and `60/3` regressed.

**Verdict.** Inconclusive / strongest positive classifier. The signal is NOT
"fewer vertices" — order changes per-invocation backend storage shape. But
cross-bucket analysis showed the `60/4 large4096` target is entirely
depth-read/alpha/textured (0 opaque draws), so it is not production-safe for
reordering, and the cross-row interactions (`60/0` up, `60/3` down) keep it a
diagnostic. The shader-dump join confirmed ~80% of visible VSOut is unread yet
trimming it does not move the bucket — owner is below source-visible stage-out.

**Related.** [[primitive-reorder-diagnostics]] · tooling: [[primitive-reorder-diagnostics-reverse.09]]
· prev: [[primitive-reorder-diagnostics-reverse.10]] · narrows to: [[primitive-reorder-diagnostics-reverse.12]],
[[primitive-reorder-diagnostics-reverse.13]] · safe-subset attempt: [[primitive-reorder-diagnostics-reverse.14]]
· current rerun: [[primitive-reorder-diagnostics-reverse.15]] · [[hidden-backend-storage]] · [[vsout-layout]].
