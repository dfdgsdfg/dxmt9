---
domain: vsout-layout
workload: 3DMark05 GT1
subcategory: pointsize
order: 01
title: Point-Size-Only Pipeline Probe
date: undated
type: experiment-run
status: rejected
source: specs/perfomance.plan.md#L7462-L7509
---

# Point-Size-Only Pipeline Probe

**Question / hypothesis.** Does removing only the Metal `VSOut.pointSize`
`point_size` field (the narrowest possible visible-width change) move the Xcode
VS-write bucket, isolating the point-size path from ordinary FS liveness?

**Method.** `DXMT9_PROBE_DROP_VSOUT_POINT_SIZE=1` via
`run_3dmark05_perf_probe.sh --drop-vsout-point-size --frame 60
--encoder-breakdown-seq 60 --dump-shaders`. Run manually terminated after capture
(`process_exit 143`); finalized with `--require-xcode-counter-coverage
--require-dxmt-join-coverage --require-top-pso-attribution
--require-shader-dump-matches` (all gates passed).

**Result.** Structurally it worked: all hot rows moved `VSOut key 0xfff -> 0x7ff`
and expected source-visible VSOut width fell `184B -> 180B`. The bottleneck did
not move:

| Metric | Baseline | Drop point_size | Delta |
|---|---:|---:|---:|
| Total GPU | `34.879ms` | `34.624ms` | `-0.255ms` |
| Top 3 GPU | `34.347ms` | `34.085ms` | `-0.262ms` |
| Top 3 VS buffer write | `1627.327MiB` | `1627.311MiB` | `-0.016MiB` |
| Expected VSOut B/vertex | `184B` | `180B` | `-4B` |
| VS buffer / expected VSOut | `7.9x` | `8.0x` | no improvement |

**Verdict.** Rejected. A `-4B` visible-width reduction produced only `-0.016MiB`
VS-write movement (and the per-byte ratio actually worsened to `8.0x`).
Reinforces that the hidden vertex-stage / TVB backend storage, not visible VSOut
width, owns the bucket.

**Related.** [vsout-layout](../vsout-layout.md) · narrower follow-up to [vsout-layout-varying.01](vsout-layout-varying.01.md) · motivated [vsout-layout-position.01](vsout-layout-position.01.md) · [hidden-backend-storage](../hidden-backend-storage.md) · [shader-codegen](../shader-codegen.md).
