---
domain: index-reuse-measurement
workload: 3DMark05 GT1
title: "Index-Reuse Measurement — instrumentation that established VS-inv = post-transform cache-miss - Current Overview"
type: domain-overview
status: current
updated: 2026-07-08
source: docs/perfomance/index-reuse-measurement/log.md; docs/perfomance/overview-3dmark05-gt1.md
related: docs/perfomance/index-reuse-measurement/index.md; docs/perfomance/index-reuse-measurement/log.md
---

# Index-Reuse Measurement — instrumentation that established VS-inv = post-transform cache-miss - Current Overview

> Current, compact view for this performance domain. Historical detail from the former
> top-level `index-reuse-measurement.md` overview is preserved in [log](log.md). Domain landing: [index](index.md).

## Scope

This domain owns the **measurement / instrumentation** that characterized the
*shape* of the hot indexed geometry without changing rendering: how Xcode
`VS Invocations` relates to submitted references vs draw-local unique vertices vs
finite post-transform vertex-cache misses; whether the ~1.6 GiB VS-buffer-write
bucket is caused by tiny-draw replay or redundant geometry replay; and how the
hot indexed triangle-list traffic splits by backend-relevant state class. Its
job was mostly to **rule out** cheap explanations (geometry expansion, dedup,
tiny draws, payload canonicalization) and to **pinpoint** the one correlation
that made a lever exist: VS invocations track the post-transform cache-miss
estimate, not raw references.

## Latest Conclusions

> **Every row below cites a leaf now marked `outdated: retired-journal`.** These
> measurements cannot be re-derived today; they are kept because they record
> which cheap explanations for the hidden bucket were already ruled out.

| # | Hypothesis | Verdict | Evidence |
|---|---|---|---|
| H5 | dxmt indexed-expansion is inflating GT1 geometry | rejected (`draw_expanded_indexed=0`) | [index-reuse-measurement-geometry.01](index-reuse-measurement-geometry.01.md) |
| H6 | Redundant replay of the same geometry shape owns the bucket | rejected (dup ratio `0.143x`) | [index-reuse-measurement-geometry.02](index-reuse-measurement-geometry.02.md) |
| H7 | Bucket is driven by many tiny repeated draws | rejected; real large indexed pressure (`22,622` prim/draw) | [index-reuse-measurement-geometry.03](index-reuse-measurement-geometry.03.md) |
| H8 | Hot frame is one homogeneous material class | rejected; splits opaque-dw / depth-read-textured / mixed | [index-reuse-measurement-classattr.01](index-reuse-measurement-classattr.01.md) |
| H9 | The positive `60/4` large-draw signal is production-safe | rejected; `60/4` large4096 is 0 opaque / all depth-read | [index-reuse-measurement-classattr.02](index-reuse-measurement-classattr.02.md) |

## Current Navigation

- [Domain index](index.md)
- [Historical log](log.md)
- [Root 3DMark05 GT1 map](../overview-3dmark05-gt1.md)

## Recent Leaf Documents

> 7 of the 7 leaves listed below are marked `outdated:` and open with a banner naming the ground. They are history, not re-checkable evidence.

- [index-reuse-measurement-geometry.03 - Indexed Draw-Size Histogram Probe](index-reuse-measurement-geometry.03.md)
- [index-reuse-measurement-reuse.02 - Order-Preserving Vertex Payload Canonicalization Check](index-reuse-measurement-reuse.02.md)
- [index-reuse-measurement-geometry.02 - Geometry Signature / Dedup Result](index-reuse-measurement-geometry.02.md)
- [index-reuse-measurement-classattr.02 - Large4096 Cross-Bucket Attribution](index-reuse-measurement-classattr.02.md)
- [index-reuse-measurement-reuse.01 - Indexed Unique Vertex / Index Reuse Probe](index-reuse-measurement-reuse.01.md)
- [index-reuse-measurement-geometry.01 - Geometry Amplification Audit + Draw Geometry Signature Instrumentation](index-reuse-measurement-geometry.01.md)
- [index-reuse-measurement-classattr.01 - Indexed Triangle State-Class Attribution](index-reuse-measurement-classattr.01.md)
