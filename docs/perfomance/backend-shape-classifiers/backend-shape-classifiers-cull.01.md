---
domain: backend-shape-classifiers
workload: 3DMark05 GT1
subcategory: cull
order: 01
title: Probe Disable Cull
date: 2026-06-02
type: experiment-run
status: rejected
source: specs/perfomance.plan.md#L3293-L3297
---

# Probe Disable Cull

**Question / hypothesis.** Does the cull-state bit own the hidden top-three
VS-buffer-write bucket?

**Method.** `DXMT_DISABLE_CULL=1`. Top-three Xcode VS-write A/B vs the
normal baseline (this is the first, terse cull readout; the fuller capture is
[backend-shape-classifiers-cull.02](backend-shape-classifiers-cull.02.md)).

**Result.** Top-three VS buffer write `1627.240MiB -> 1627.233MiB` (`-0.00%`)
while top-three GPU time `34.837ms -> 35.478ms` (`+1.84%`). Draw count, vertex
count, stream/IB churn, PSO samples, and expected VSOut all unchanged.

**Verdict.** Rejected. The cull-state bit is not the first-order owner of the
hidden VS buffer-write traffic — only GPU time moves, within backend variation.

**Related.** [backend-shape-classifiers](index.md) · first in the cull sequence, detailed in [backend-shape-classifiers-cull.02](backend-shape-classifiers-cull.02.md) · paired with [backend-shape-classifiers-scissor.01](backend-shape-classifiers-scissor.01.md) in the same summary · confirms [hidden-backend-storage](../hidden-backend-storage/index.md).
