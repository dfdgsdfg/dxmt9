---
domain: state-churn-encode
workload: 3DMark05 GT1
subcategory: expand
order: 02
title: Same-Frame Xcode Validation (no-auto-expand)
date: 2026-06-01
type: validation
status: rejected
source: specs/perfomance.plan.md#L5219-L5283
---

# Same-Frame Xcode Validation (no-auto-expand)

**Question / hypothesis.** Does disabling auto-expand reduce the *Xcode-reported*
top-pass buffer/device writes at the same frame, or is its benefit only CPU-side?

**Method.** `frame60` gputrace with `DXMT_DISABLE_AUTO_EXPAND_INDEXED=1`,
performance data embedded, encoder counters exported from Xcode. Output:
`experiments/output/app-d3d9-3dmark05-no-auto-expand-gputrace-frame60/` and
`traces/app-d3d9-3dmark05-20260601-no-auto-expand-frame60/analysis/`. Compared to
the prior dirty-range-reset frame60 capture (same encoder/draw/vertex shape).

**Result.** Xcode GPU `36.577 -> 35.643 ms` (-0.934ms); top-3 encoder GPU
`36.026 -> 35.084 ms` (-0.942ms); top-3 device writes `1676.657 -> 1676.491 MiB`
(-0.166); top-3 **buffer writes `1628.005 -> 1628.074 MiB` (+0.069)**; top-3 LLC
`1677.465 -> 1677.354 MiB`; top-3 vertices unchanged (`2,146,185`). Whole-run
`transient_vertex_bytes ~1.056GB -> 0`, `transient_upload_bytes ~2.122GB -> 1.029GB`.
The three dominant encoders keep the same memory-write shape (top
`cb_seq_236 RenderPass[rt=0x300002a0000000d,...]` = `20.669ms`, `981.209MiB`
buffer write, LLC `36.99%` / MMU `34.02%` / Buffer Write `21.34%`).

**Verdict.** Rejected (as a GPU fix). Auto-expand removal cuts CPU transient
uploads and shaves GPU time slightly, but does not reduce top-pass buffer/device
writes — the same three encoders still own ~`98.43%` of frame GPU and ~`1.63GiB`
buffer writes. A secondary cleanup, not the primary GPU bottleneck. Visual
correctness still unproven. This capture also motivated the `seq/enc` encoder
labels for the later label-join.

**Related.** [state-churn-encode](index.md) · prev: [state-churn-encode-expand.01](state-churn-encode-expand.01.md) ·
[state-churn-encode-encoder.03](state-churn-encode-encoder.03.md) (label-join follow-up) ·
[hidden-backend-storage](../hidden-backend-storage/index.md) (the unmoved ~1.63GiB write owner).
