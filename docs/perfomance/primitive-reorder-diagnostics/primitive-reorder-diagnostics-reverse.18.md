---
domain: primitive-reorder-diagnostics
workload: 3DMark05 GT1
subcategory: reverse
order: 18
title: Current Row 60/1 Opaque Reverse Rerun
date: 2026-06-02
type: validation
status: rejected
outdated: retired-journal
source: specs/perfomance.plan.md#L11985-L12071
---

# Current Row 60/1 Opaque Reverse Rerun

> **Outdated — this leaf's only `source:` is the retired `specs/perfomance.plan.md` journal, which was deleted.** The numbers below cannot be re-derived or re-checked. Kept as history; do not cite it as current evidence.

**Question / hypothesis.** Re-run the older `60/1` row reverse
([primitive-reorder-diagnostics-reverse.06](primitive-reorder-diagnostics-reverse.06.md), which had failed shape gates via
hot-row substitution) on current HEAD under strict same-hot-row gates, reversing
all `60/1` opaque-depth-write draws. Is the older `60/1` aggregate win real once
the comparison is clean?

**Method.** `run_3dmark05_perf_probe.sh --suffix
reverse-row-60-1-opaque-current-gputrace-r1 --probe-reverse-indexed-triangles
--probe-reverse-indexed-triangles-row 60/1
--probe-reverse-indexed-triangles-class opaque-depth-write` + `finalize` with
strict gates. Smoke: 156 applied draws, `1,405,854B`, alpha/scissor/textured =
0/0/0. All gates PASSED (same hot rows, identical draw/vertex/triangle counts).

**Result.** Total GPU `34.391 -> 33.253ms` (`-3.31%`); hot GPU `-3.24%`; hot VS
buffer write `1472.747 -> 1473.267MiB` (`+0.04%`); VS bytes/inv `+0.05%`; hidden
backend estimate `1454.905MiB` (still dominant); transient CPU writer `1.341MiB`.
Target `60/1`: GPU `8.252 -> 7.994ms` (`-3.12%`), VS write `437.404 -> 437.877MiB`
(`+0.11%`, slightly up), invocations `393,529 -> 393,300`.

**Verdict.** Rejected (clean negative). `60/1` opaque primitive-order reversal is
not a VS-write root fix: the GPU-time improvement is real but is not paired with
reduced VS buffer write or hidden backend traffic (likely a scheduling/locality
time effect). The older `60/1` aggregate win was weaker evidence (failed shape
gates); this strict run is the better causal comparison. Next direction: backend
state-shape experiments, not order reversal.

**Related.** [primitive-reorder-diagnostics](index.md) · reruns: [primitive-reorder-diagnostics-reverse.06](primitive-reorder-diagnostics-reverse.06.md)
· prev: [primitive-reorder-diagnostics-reverse.17](primitive-reorder-diagnostics-reverse.17.md) · [hidden-backend-storage](../hidden-backend-storage/index.md)
· [backend-shape-classifiers](../backend-shape-classifiers/index.md) · [mini-replay-bisection](../mini-replay-bisection/index.md) (state-shape replay is the next lever).
