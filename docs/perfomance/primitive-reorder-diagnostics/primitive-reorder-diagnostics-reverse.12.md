---
domain: primitive-reorder-diagnostics
workload: 3DMark05 GT1
subcategory: reverse
order: 12
title: Row/Class 60/4 Large4096 + Alpha Reverse
date: 2026-06-02
type: experiment-run
status: inconclusive
source: specs/perfomance.plan.md#L10798-L10872
---

# Row/Class 60/4 Large4096 + Alpha Reverse

**Question / hypothesis.** Can the AND-list intersection `large4096 && alpha-blend`
(16 of the 19 `large4096` draws) reproduce the full `60/4 large4096` signal
([primitive-reorder-diagnostics-reverse.11](primitive-reorder-diagnostics-reverse.11.md)) with fewer reordered draws? Asks
whether the 3 non-alpha large draws are required.

**Method.** `run_3dmark05_perf_probe.sh --suffix reverse-row-60-4-large4096-alpha-gputrace-r1
--probe-reverse-indexed-triangles --probe-reverse-indexed-triangles-row 60/4
--probe-reverse-indexed-triangles-classes large4096,alpha-blend` then
`finalize_3dmark05_perf_probe.sh` with the strict shape/coverage/PSO gate set.
Smoke first confirmed 16/237 draws (`534,258B`). All finalizer gates PASSED.

**Result.** 16-draw intersection reproduces the 19-draw signal almost exactly:
top VS write `-7.46%`, top GPU `-6.82%`, `60/4` VS write `-22.32%`, `60/4`
bytes/inv `-19.00%`. Matched hot rows moved `-109.821MiB` VS write, of which
`-91.996MiB` is bytes/invocation and only `-17.825MiB` invocation-count; target
`60/4` accounts for `-82.641MiB`. Contrast: broad `60/4 alpha-blend` (243 draws,
[primitive-reorder-diagnostics-reverse.10](primitive-reorder-diagnostics-reverse.10.md)) moved VS write only `+0.03%`.

**Verdict.** Inconclusive / positive classifier. The 3 non-alpha large draws are
NOT required. But broad alpha cancels the effect, so it is not "alpha is good" —
it is an order/locality interaction between the 16 large alpha/depth-read/textured
draws and surrounding small alpha work. Still depth-read/textured/alpha = not
production-safe by reorder alone. Later current-HEAD rerun
([primitive-reorder-diagnostics-reverse.17](primitive-reorder-diagnostics-reverse.17.md)) fails to reproduce it.

**Related.** [primitive-reorder-diagnostics](../primitive-reorder-diagnostics.md) · tooling: [primitive-reorder-diagnostics-reverse.09](primitive-reorder-diagnostics-reverse.09.md)
· from: [primitive-reorder-diagnostics-reverse.11](primitive-reorder-diagnostics-reverse.11.md) · contrasts: [primitive-reorder-diagnostics-reverse.10](primitive-reorder-diagnostics-reverse.10.md)
· narrows to: [primitive-reorder-diagnostics-reverse.13](primitive-reorder-diagnostics-reverse.13.md) · current rerun: [primitive-reorder-diagnostics-reverse.17](primitive-reorder-diagnostics-reverse.17.md)
· [hidden-backend-storage](../hidden-backend-storage.md).
