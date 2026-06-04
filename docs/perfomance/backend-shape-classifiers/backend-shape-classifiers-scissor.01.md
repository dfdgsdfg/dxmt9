---
domain: backend-shape-classifiers
workload: 3DMark05 GT1
subcategory: scissor
order: 01
title: Probe Disable Scissor
date: 2026-06-02
type: experiment-run
status: rejected
source: specs/perfomance.plan.md#L3298-L3303
---

# Probe Disable Scissor

**Question / hypothesis.** Does scissor state own the hidden top-three
VS-buffer-write bucket?

**Method.** `DXMT_DISABLE_SCISSOR=1`. Top-three Xcode VS-write A/B vs the
normal baseline (terse summary readout; the full capture is
[[backend-shape-classifiers-scissor.02]]).

**Result.** Top-three VS buffer write `1627.240MiB -> 1627.315MiB` (`+0.06%`,
effectively unchanged) while top-three GPU time `34.837ms -> 36.295ms`
(`+4.19%`). Draw count, vertex count, stream/IB churn, PSO samples, expected
VSOut, and explicit dxmt writer bytes all unchanged.

**Verdict.** Rejected. Scissor state is not the first-order owner of the hidden
VS buffer-write traffic — only GPU time moves.

**Related.** [[backend-shape-classifiers]] · detailed in [[backend-shape-classifiers-scissor.02]] · paired with [[backend-shape-classifiers-cull.01]] in the same summary · confirms [[hidden-backend-storage]].
