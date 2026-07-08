---
domain: primitive-reorder-diagnostics
workload: 3DMark05 GT1
subcategory: reverse
order: 14
title: Opaque Large4096 Reverse
date: 2026-06-02
type: experiment-run
status: rejected
source: specs/perfomance.plan.md#L11219-L11516
---

# Opaque Large4096 Reverse

**Question / hypothesis.** The positive `60/4 large4096` target
([primitive-reorder-diagnostics-reverse.11](primitive-reorder-diagnostics-reverse.11.md)) is entirely depth-read/alpha/
textured — not safe to reorder. The cross-bucket baseline showed the
*correctness-preserving* large draws are opaque-depth-write: 9 in `60/1`, 9 in
`60/3`, 5 in `60/0` (23 total). Does reordering only that safe opaque-large set
move the VS-write bucket?

**Method.** `run_3dmark05_perf_probe.sh --suffix reverse-opaque-large4096-gputrace-r1
--probe-reverse-opaque-indexed-triangles --probe-reverse-indexed-triangles-rows 60/0,60/1,60/3
--probe-reverse-indexed-triangles-class large4096` + `finalize` with strict
gates. Smoke hit exactly 23 opaque-large draws (5/9/9), `60/4` untouched, drift
<1%. All gates PASSED.

**Result.** Negative. Total GPU `34.391 -> 34.257ms` (`-0.39%`); top hot GPU
`-0.31%`; top VS buffer write `1472.747 -> 1472.821MiB` (`+0.01%`); top buffer
write `-0.00%`. Hidden backend estimate `1454.945MiB` (`0.988x`). Matched-row
VS-write delta `+0.074MiB`: invocation decrease `-5.865MiB` cancelled by
bytes/inv increase `+5.939MiB`. Companion **order-preserving split** probe of the
same `60/4 large4096` draws (19 -> 38 Metal draws) was also negative: top VS
write `+0.00%`, GPU `-1.91%` — so pure draw-size split is not the owner either.

**Verdict.** Rejected. The production-safe opaque-large reorder does NOT reproduce
the positive `60/4` classifier; opaque-large alone carries no signal, and
order-preserving split confirms the prior win required the order/locality/
visibility perturbation, not partition or vertex-count change. The shader-dump
join (9/9 top rows matched, `184B` source VSOut vs `594-1165B` Xcode) again
confirms the owner is hidden Apple vertex/tiler/backend storage below
source-visible VSOut.

**Related.** [primitive-reorder-diagnostics](../primitive-reorder-diagnostics.md) · from: [primitive-reorder-diagnostics-reverse.11](primitive-reorder-diagnostics-reverse.11.md)
· tooling: [primitive-reorder-diagnostics-reverse.09](primitive-reorder-diagnostics-reverse.09.md) · split sibling: [primitive-reorder-diagnostics-split.04](primitive-reorder-diagnostics-split.04.md)
· [hidden-backend-storage](../hidden-backend-storage.md) · [vsout-layout](../vsout-layout.md) · [index-cache-locality](../index-cache-locality.md).
