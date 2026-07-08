---
domain: primitive-reorder-diagnostics
workload: 3DMark05 GT1
subcategory: split
order: 03
title: Split Row 60/4 Large4096
date: 2026-06-02
type: experiment-run
status: rejected
source: specs/perfomance.plan.md#L11355-L11516
---

# Split Row 60/4 Large4096

**Question / hypothesis.** The positive `60/4 large4096 reverse` classifier moved
the VS-write bucket. Was that win due to "large draw split reduces backend
write", or does it require the order/locality/visibility perturbation of
reversal? This probe keeps original primitive order and splits only the `60/4`
`large4096` draws into bounded Metal draws.

**Method.** `run_3dmark05_perf_probe.sh --suffix split-row-60-4-large4096-gputrace-r1
--frame 60 --encoder-breakdown-seq 60 --split-large-indexed-draws 4096
--split-large-indexed-draws-row 60/4 --split-large-indexed-draws-class large4096
--measure-index-reuse --top 4 --hot-gpu-share 95 --baseline-joined
<measure-index-cache-gputrace-r1>` with the strict Xcode-counter, dxmt-join,
top-PSO, top-row, and 5% draw/vertex/triangle gates. Smoke confirmed scope.

**Result.** Only `60/4` split: `19` source draws → `38` Metal draws (`+19`),
`104,721` primitives; rows `60/0,60/1,60/3` unsplit. Total GPU
`34.391 → 33.681ms` (`-2.07%`); top hot-row GPU `33.741 → 33.097ms` (`-1.91%`);
top VS buffer write fixed `1472.747 → 1472.756MiB` (`+0.00%`); top buffer write
`1473.614 → 1473.604MiB` (`-0.00%`). Hidden backend estimate stays dominant at
`1455.866MiB` (`0.989x` of VS write). Matched-row VS-write delta only
`+0.009MiB`: invocation-count reduction (`-5.726MiB`) was cancelled by
bytes/invocation growth (`+5.735MiB`); target `60/4` went `588.7 → 594.2 B/inv`
(`+0.93%`).

**Verdict.** Rejected — order-preserving split is negative for the VS-write
bottleneck (split ≠ reorder). The earlier positive `60/4 large4096 reverse`
cannot be explained as "large-draw split"; it moved cost through bytes/invocation
(`588.7 → 476.8 B/inv`, `-19.02%`), which the split moved the *opposite* way.
The working hypothesis is order-dependent Apple vertex/tiler backend storage
shape, not primitive count per draw.

**Related.** [primitive-reorder-diagnostics](../primitive-reorder-diagnostics.md) · prior:
[primitive-reorder-diagnostics-split.02](primitive-reorder-diagnostics-split.02.md) · next: [primitive-reorder-diagnostics-split.04](primitive-reorder-diagnostics-split.04.md)
· [hidden-backend-storage](../hidden-backend-storage.md) (order-dependent TVB storage) ·
[vsout-layout](../vsout-layout.md) (rejects visible-VSOut trim: 184B source vs 594-1165B/inv) ·
[index-cache-locality](../index-cache-locality.md) (semantic-safe locality successor) · [baselines](../baselines.md).
