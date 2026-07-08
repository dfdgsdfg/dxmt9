---
domain: primitive-reorder-diagnostics
workload: 3DMark05 GT1
subcategory: reverse
order: 02
title: Opaque Depth-Writing Reverse Subset
date: 2026-06-02
type: experiment-run
status: rejected
source: specs/perfomance.plan.md#L9384-L9518
---

# Opaque Depth-Writing Reverse Subset

**Question / hypothesis.** Does the correctness-preserving subset of the full
reverse classifier ([primitive-reorder-diagnostics-reverse.01](primitive-reorder-diagnostics-reverse.01.md)) reproduce the
win? The probe reverses triangle order only for solid, depth-enabled,
depth-writing, non-blended, non-alpha-tested, non-stencil, non-clip-plane
triangle-list draws with `Less`/`LessEqual` depth. Blended / depth-write-off
visibility passes stay in normal order.

**Method.** `run_3dmark05_perf_probe.sh --suffix reverse-opaque-indexed-triangles-gputrace-r1
--frame 60 --probe-reverse-opaque-indexed-triangles --measure-index-reuse
--top 4 --hot-gpu-share 95 --baseline-joined <measure-index-cache>
--require-xcode-counter-coverage --require-dxmt-join-coverage --require-top-pso-attribution`.

**Result.** GPU `34.391 -> 35.678ms` (`+3.74%`); hot/top GPU `+3.99%`; hot/top
VS buffer write `1472.747 -> 1474.268MiB` (`+0.10%`); hot/top unexplained write
`-0.13%`; FS tiles `+0.00%`; texture write `+0.00%`; stream handle changes
`+8.80%`, IB handle changes `+6.51%`. Gputrace coverage: 402 probe draws / 362
skipped, `3.58MiB` reorder bytes (278 blended draws skipped). Hidden backend
estimate `1452.555MiB` (`0.985x` VS write).

**Verdict.** Rejected. The production-safer opaque subset does NOT reproduce the
full-reverse win: VS write is flat and frame time regresses. The full-reverse
benefit therefore depended on a broader frame-shape change (blended/depth-off
passes, scissor, tile coverage, hot-row membership). New transient-IB path adds
CPU/state churn with no GPU payoff. Also fails the 5% shape gate
([primitive-reorder-diagnostics-reverse.04](primitive-reorder-diagnostics-reverse.04.md), top draws `711 -> 753`).

**Related.** [primitive-reorder-diagnostics](index.md) · prev: [primitive-reorder-diagnostics-reverse.01](primitive-reorder-diagnostics-reverse.01.md)
· next: [primitive-reorder-diagnostics-reverse.03](primitive-reorder-diagnostics-reverse.03.md) · [hidden-backend-storage](../hidden-backend-storage/index.md) (owner unchanged)
· [vsout-layout](../vsout-layout/index.md) (VS bytes/inv still ~4.5x visible 184B VSOut).
