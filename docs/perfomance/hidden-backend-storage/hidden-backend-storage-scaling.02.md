---
domain: hidden-backend-storage
workload: 3DMark05 GT1
subcategory: scaling
order: 02
title: Cross-Run VS Buffer Scaling Refresh
date: undated
type: measurement
status: accepted
source: specs/perfomance.plan.md#L8769-L8850
---

# Cross-Run VS Buffer Scaling Refresh

**Question / hypothesis.** Re-run the scaling correlation on the curated
classifier corpus (after adding `current-head-gputrace-r1`) to sharpen which
dimension the hidden VS-buffer-write bucket tracks.

**Method.** `analyze_vs_buffer_scaling.py --top-n 3` over `12` joined frame60
captures: `current-normal`, `current-head`, `disable-alpha-test`,
`disable-cull`, `disable-fog`, `disable-scissor`, `force-fragment-color`,
`force-texture-white`, `probe-disable-alpha-blend`, `probe-disable-depth-write`,
`probe-position-only-vsout`, `x8-alpha-fill-r2`.

**Result.** Run-class bands: normal / current HEAD / state toggles top-three VS
write `1627.233`-`1627.331MiB` (VS/VSOut `7.9x`, VS/named-tiled `27.3-58.1x`,
CPU-writer/buffer `0.0003x`); force-texture-white `1574.470MiB`; force-fragment
or position-only `1548.218`-`1548.284MiB` (VS/VSOut up to `88.4x`,
VS/named-tiled `182.1-182.2x`).

Encoder-row correlations:

| Metric | r | Interpretation |
|---|---:|---|
| post-clipped primitives / primitives / dxmt vertices / stream0 bytes | `0.977` | bucket tracks submitted geometry scale |
| VS invocations | `0.971` | strong mover when a probe changes backend visibility |
| stream/IB state churn | `0.950` | tracks draw/geometry shape (not CPU writer bytes) |
| expected VSOut bytes | `0.845` | correlated but not causal; position-only still `88.4x` |
| tiled vertex+primitive bytes | `0.841` | named counters move some but stay far smaller |
| dxmt CPU writer bytes | `0.188` | not the owner |
| FS invocations | `0.034` | fragment volume is not the scaling dimension |

**Verdict.** accepted (strengthens hidden-owner classification). The remaining
first-order bucket scales like hidden Apple vertex-stage/backend storage
attached to submitted geometry and the VS-invocation path: post-clipped
primitives `r=0.977`, VS invocations `r=0.971`, dxmt CPU writers `r=0.188`,
FS invocations `r=0.034`. Rejects dxmt writers, visible `VSOut` width, fragment
volume, and named tiled counters alone as the owner.

**Related.** [[hidden-backend-storage-scaling.01]] · [[hidden-backend-storage]] ·
[[hidden-backend-storage-density.01]] · [[hidden-backend-storage-shape.01]] ·
[[vsout-layout]] · [[backend-shape-classifiers]] · [[index-cache-locality]] ·
[[overview]]
