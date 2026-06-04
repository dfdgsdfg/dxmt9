---
domain: hidden-backend-storage
workload: 3DMark05 GT1
subcategory: scaling
order: 01
title: Multi-Capture VS Buffer Scaling
date: 2026-06-01
type: measurement
status: accepted
source: specs/perfomance.plan.md#L3189-L3313
---

# Multi-Capture VS Buffer Scaling

**Question / hypothesis.** Across many frame60 captures, which encoder-row
metric does the VS-buffer-write bucket correlate with — submitted geometry / VS
invocations (hidden-backend hypothesis) or explicit dxmt CPU writers / fragment
volume (rejected hypotheses)?

**Method.** `analyze_vs_buffer_scaling.py` over all `45` local frame60
captures that have joined Xcode/dxmt CSVs, baseline
`current-normal-gputrace-r1`. Baseline-delta triage requires top row-key
equality, draw-count delta <= `1%`, vertex/primitive delta <= `5%` before
calling an A/B geometry-stable.

**Result.** VS buffer write observed range `903.327`-`2917.457MiB`; same-row /
state-probe band `1627.233`-`1629.865MiB`. dxmt CPU writer / Xcode buffer write
`0.0003x`-`0.0296x`. VS/expected-VSOut `4.0x`-`88.4x`. VS bytes/invocation
`736.1`-`1449.9B`.

Encoder-row Pearson r vs VS buffer MiB (nonzero rows):

| Metric | r |
|---|---:|
| tiled vertex + primitive-block bytes | `0.797` |
| VS invocations | `0.718` |
| post-clipped primitives / stream0 bytes / dxmt vertices / primitives | `0.702` |
| pixels | `0.660` |
| expected VSOut bytes | `0.637` |
| dxmt CPU writer bytes | `0.409` |
| stream/IB state churn | `0.379` |
| FS invocations | `0.255` |

Triage classes: `force-fragment-color` VS write `-4.85%` (geometry unchanged) =
fragment/visible-source secondary; `force-expand-indexed` VS `+79.29%`/GPU
`+83.55%` = destructive proof reuse matters; `reverse-indexed-triangles` VS
`-44.49%`/GPU `-37.94%` but row keys drift = locality moves bucket but not a
legal same-frame win. Broad `DXMT9_PROBE_DISABLE_ALPHA_BLEND=1`,
`DXMT_DISABLE_CULL=1` (`1627.240`→`1627.233MiB`), and `DXMT_DISABLE_SCISSOR=1`
(`→1627.315MiB`) all leave the bucket at ~`1627MiB`.

**Verdict.** accepted (scaling confirms hidden owner). The bucket scales with
primitive / post-clip / VS-invocation shape (r `0.70`-`0.80`), not with dxmt
writers (`0.41`) or FS invocations (`0.26`). No single D3D9 state bit owns it.
The only strong VS-write movers are destructive or shape-drifting locality
classifiers — motivating row-key-preserving locality / backend-shape probes.

**Related.** [[hidden-backend-storage]] · [[hidden-backend-storage-attribution.01]] ·
[[hidden-backend-storage-scaling.02]] · [[hidden-backend-storage-shape.01]] ·
[[primitive-reorder-diagnostics]] · [[index-cache-locality]] ·
[[backend-shape-classifiers]] · [[overview-3dmark05-gt1]]
