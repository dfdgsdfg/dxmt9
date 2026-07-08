---
domain: primitive-reorder-diagnostics
workload: 3DMark05 GT1
subcategory: reverse
order: 17
title: Current Full Large4096 + Alpha Reorder Rerun
date: 2026-06-02
type: validation
status: rejected
source: specs/perfomance.plan.md#L11901-L11984
---

# Current Full Large4096 + Alpha Reorder Rerun

**Question / hypothesis.** Re-run the broader 16-draw `60/4 large4096 &&
alpha-blend` reverse ([primitive-reorder-diagnostics-reverse.12](primitive-reorder-diagnostics-reverse.12.md)) on current
HEAD — every large alpha draw, not just the 4 scissored ones — to test whether
the historical positive was a wider alpha/material ordering effect.

**Method.** `run_3dmark05_perf_probe.sh --suffix
reverse-row-60-4-large4096-alpha-current-gputrace-r1
--probe-reverse-indexed-triangles --probe-reverse-indexed-triangles-row 60/4
--probe-reverse-indexed-triangles-classes large4096,alpha-blend` + `finalize`
with strict gates. Smoke: 16 applied draws, `534,258B`. All gates PASSED (same
hot rows, identical draw/vertex/triangle counts).

**Result.** Total GPU `34.391 -> 34.575ms` (`+0.53%`); hot GPU `+0.65%`; hot VS
buffer write `1472.747 -> 1472.866MiB` (`+0.01%`); VS bytes/inv `+0.01%`; hidden
backend estimate `1455.335MiB` (still dominant). Target `60/4` GPU `+0.48%`, VS
write `+0.04%`; `60/3 +2.88%` GPU, `60/1 -3.46%` GPU, all with flat VS write.

**Verdict.** Rejected. The current full `large4096 && alpha-blend` index-order
predicate is not a VS-write root fix. This removes the last direct support for
promoting the historical `60/4` index-order anomaly into production logic. The
surviving owner is hidden vertex/tiler/backend storage; next probes move away
from scissor/alpha membership toward row shape (opaque 2048 `60/1`/`60/3`).

**Related.** [primitive-reorder-diagnostics](../primitive-reorder-diagnostics.md) · reruns: [primitive-reorder-diagnostics-reverse.12](primitive-reorder-diagnostics-reverse.12.md)
· prev: [primitive-reorder-diagnostics-reverse.16](primitive-reorder-diagnostics-reverse.16.md) · next: [primitive-reorder-diagnostics-reverse.18](primitive-reorder-diagnostics-reverse.18.md)
· [hidden-backend-storage](../hidden-backend-storage.md).
