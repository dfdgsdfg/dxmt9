---
domain: vsout-layout
subcategory: varying
order: 01
title: Trim-Varyings Xcode Recheck
date: undated
type: experiment-run
status: rejected
source: specs/perfomance.plan.md#L5361-L5398
---

# Trim-Varyings Xcode Recheck

**Question / hypothesis.** Does collapsing the visible MSL `VSOut` width by trimming
unused varyings reduce the dominant Xcode "VS Buffer Device Memory Bytes Written"
bucket on 3DMark05 GT1 frame60?

**Method.** Re-captured the same `frame60` with `DXMT9_TRIM_UNUSED_VARYINGS=1`,
`DXMT9_PERF_ENCODER_BREAKDOWN=1`, and `DXMT_DISABLE_AUTO_EXPAND_INDEXED=1`. Xcode
encoder-counter export + `frame60-xcode-dxmt-joined-summary.csv` +
`frame60-trim-vs-baseline-comparison.csv` under
`traces/app-d3d9-3dmark05-20260601-trim-varyings-frame60/`.

**Result.**

| Scope | Metric | Baseline | Trim-varyings | Delta |
|---|---:|---:|---:|---:|
| all encoders | GPU time | `33.994ms` | `34.372ms` | `+0.378ms` |
| all encoders | VS buffer write | `1627.365MiB` | `1627.349MiB` | `-0.016MiB` |
| top 3 | unexplained buffer write | `1627.579MiB` | `1627.625MiB` | `+0.046MiB` |
| top 3 | unexplained / buffer write | `0.9997x` | `0.9997x` | unchanged |

Per-encoder dxmt attribution unchanged: top-3 draws `385`, stream handle changes
`436`, IB handle changes `325`, `setVertexBytes` `6160` B, transient vertex/index
`0` B. (Note: even with zero varyings the dominant encoder still writes ~225 MiB.)

**Verdict.** Rejected. GPU time slightly *regressed* and VS buffer write moved
only `-0.016MiB` — visible varying width is not the first-order owner of the
hidden VS-write bucket. Confirms the central finding that the cost is hidden GPU
vertex-stage / TVB backend storage, not visible MSL stage-out width.

**Related.** [[vsout-layout]] · [[hidden-backend-storage]] · [[tvb-mechanism-proof]] · followed by [[vsout-layout-varying.02]].
