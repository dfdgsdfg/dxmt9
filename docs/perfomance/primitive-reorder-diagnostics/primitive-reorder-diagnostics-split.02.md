---
domain: primitive-reorder-diagnostics
workload: 3DMark05 GT1
subcategory: split
order: 02
title: Bounded Split Row 60/1 Opaque
date: 2026-06-02
type: experiment-run
status: rejected
source: specs/perfomance.plan.md#L10435-L10530
---

# Bounded Split Row 60/1 Opaque

**Question / hypothesis.** The symmetric check after the `60/3` negative: does
the other opaque 2048x2048 depth-write hot row, `60/1`, respond differently to
order-preserving draw partitioning?

**Method.** `run_3dmark05_perf_probe.sh ... --split-large-indexed-draws 4096
--split-large-indexed-draws-row 60/1 --split-large-indexed-draws-class
opaque-depth-write --measure-index-reuse` with the same strict top-row,
geometry, Xcode-counter, dxmt-join, and PSO-attribution gates as `60/3`.
No-gputrace smoke confirmed scope first; baseline `measure-index-cache-gputrace-r1`.

**Result.** Selector scoped correctly: only `60/1` reports `9` source draws →
`23` Metal draws over `72,305` primitives. Xcode Summary: `4` CBs, `12`
encoders, `742` draws, `3,122,697` vertices, `34.03ms` GPU. Total GPU
`34.391 → 34.026ms` (`-1.06%`); hot-set GPU `33.741 → 33.408ms` (`-0.99%`);
hot-set VS write `1472.747 → 1473.040MiB` (`+0.02%`); unexplained `+0.02%`; VS
bytes/inv `-0.01%`. Per-row, the *target* `60/1` regressed: GPU
`8.252 → 8.422ms` (`+2.07%`) and VS write `437.404 → 437.680MiB` (`+0.06%`).
Stream/IB handle churn rose slightly (`+1.08% / +0.81%`) from the extra draws.

**Verdict.** Rejected. The target row regresses in both GPU time and VS write
while the hot-set VS write stays effectively unchanged. Together with `60/3`,
this rejects order-preserving bounded draw partitioning for *both* opaque
depth-writing hot rows. Next GPU probes must change backend state shape,
material grouping, or primitive locality/order — not draw granularity.

**Related.** [primitive-reorder-diagnostics](index.md) · prior:
[primitive-reorder-diagnostics-split.01](primitive-reorder-diagnostics-split.01.md) · next: [primitive-reorder-diagnostics-split.03](primitive-reorder-diagnostics-split.03.md)
· [hidden-backend-storage](../hidden-backend-storage/index.md) (TVB bucket unmoved) ·
[index-cache-locality](../index-cache-locality/index.md) (semantic-safe order/locality successor) · [baselines](../baselines/index.md).
