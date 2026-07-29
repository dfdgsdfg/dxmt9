---
domain: vsout-layout
workload: 3DMark05 GT1
title: "VSOut Layout — visible varying-width attempts to explain the VS-write bucket - Current Overview"
type: domain-overview
status: current
updated: 2026-07-08
source: docs/perfomance/vsout-layout/log.md; docs/perfomance/overview-3dmark05-gt1.md
related: docs/perfomance/vsout-layout/index.md; docs/perfomance/vsout-layout/log.md
---

# VSOut Layout — visible varying-width attempts to explain the VS-write bucket - Current Overview

> Current, compact view for this performance domain. Historical detail from the former
> top-level `vsout-layout.md` overview is preserved in [log](log.md). Domain landing: [index](index.md).

## Scope

This domain owns every attempt to explain or reduce the dominant Xcode
"VS Buffer Device Memory Bytes Written" bucket (~1.6 GiB across the top-3 render
encoders) by changing the **visible** per-vertex shader-stage-out shape — the MSL
`VSOut` struct width / field set. The hypotheses span blanket varying trimming,
exact FS-read liveness, dropping a single field (point-size), the extreme
position-only lower bound (plus its fragment-only control), and half-precision
varyings. Almost every one was **rejected** as not the first-order owner; the one
useful result is a semantically-*safe* liveness trim that nonetheless does not move
the bucket.

## Latest Conclusions

> **Every row below cites a leaf now marked `outdated: retired-journal`.** These
> rejections cannot be re-checked today; they are kept because they are the
> reason nobody should re-open visible varying width as the hidden-write owner.

| # | Hypothesis | Verdict | Evidence |
|---|---|---|---|
| H3 | A liveness trim can at least be done safely (no pixel change) | rejected (perf), but **semantically safe** (0 changed px, SSIM 1.000) | vsout-layout-varying.03 |
| H4 | Dropping only `VSOut.pointSize` (184B→180B) moves the bucket | rejected | vsout-layout-pointsize.01 |
| H5 | Extreme position-only VSOut (184B→16B) drops the bucket proportionally | rejected (non-proportional; correctness-invalid diagnostic) | vsout-layout-position.01 |
| H6 | Control: constant-fragment alone (184B VSOut unchanged) reproduces the same delta → mover is fragment/raster, not width | rejected as width owner (control confirms) | vsout-layout-position.02 |
| H7 | Half-precision varyings reduce hidden TVB/parameter storage | rejected (fails GPU-time TVB mechanism gate) | vsout-layout-half.01 |

## Current Navigation

- [Domain index](index.md)
- [Historical log](log.md)
- [Root 3DMark05 GT1 map](../overview-3dmark05-gt1.md)

## Recent Leaf Documents

> 7 of the 7 leaves listed below are marked `outdated:` and open with a banner naming the ground. They are history, not re-checkable evidence.
