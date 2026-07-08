---
domain: attachment-pixelformat
workload: 3DMark05 GT1
subcategory: r32f
order: 01
title: R32F RT PixelFormatView Suppression Probe
date: undated
type: experiment-run
status: rejected
source: specs/perfomance.plan.md#L6792-L6858
---

# R32F RT PixelFormatView Suppression Probe

**Question / hypothesis.** Does the `PixelFormatView` usage on R32F render
targets (and its swizzled shader-read view) own the GT1 frame60 GPU cost?
Xcode's lossless-compression insight flags that a render target with
`PixelFormatView` usage is excluded from Metal compression even when the
captured frame uses it as a render target only.

**Method.** `DXMT9_SUPPRESS_RT_PIXEL_FORMAT_VIEW=1`, intentionally limited to
`Format::R32F` render targets (removing the swizzled shader-read view is
correctness-risky if the resource is later sampled through D3D9's expanded read
contract). Captured `app-d3d9-3dmark05-suppress-rt-pixel-format-view-gputrace-r1`,
exported `frame60-counters-xcode.csv`, finalized into the joined summary and
comparison reports.

**Result.**

| Metric | Baseline | R32F suppression | Delta |
|---|---:|---:|---:|
| Suppressed RT count / bytes | `0 / 0` | `2 / 17,825,792` | active |
| Total GPU time | `35.261ms` | `34.940ms` | `-0.91%` |
| Top-3 GPU time | `34.737ms` | `34.399ms` | `-0.97%` |
| Top-3 buffer write | `1628.095MiB` | `1628.047MiB` | `-0.00%` |
| Top-3 VS buffer write | `1627.395MiB` | `1627.314MiB` | `-0.00%` |
| Top unexplained buffer-write ratio | `1.000x` | `1.000x` | unchanged |
| Top texture write | `22.000MiB` | `11.074MiB` | `-49.66%` |
| Top device write | `1676.365MiB` | `1665.615MiB` | `-0.64%` |

**Verdict.** Rejected as the primary GT1 GPU bottleneck. The flag did exactly
what it claimed — two R32F RT shader-read views removed, texture-write halved
(`-49.66%`) — but the frame limiter did not move: top-3 VS buffer write stayed
`~1.627GiB` and `1.000x` unexplained by dxmt CPU writers. This is a
texture-write reduction, not a VS-write owner. During replay the remaining
compression insight pointed at `fmt2` BGRA-like RTs, not the R32F/fmt16
resources, motivating the X8 follow-ups.

**Related.** [attachment-pixelformat](index.md) · first in this domain, followed by the attachment-metadata instrumentation [attachment-pixelformat-metadata.01](attachment-pixelformat-metadata.01.md) · confirms the [hidden-backend-storage](../hidden-backend-storage/index.md) owner survives · sibling pass-traffic class [render-pass-store](../render-pass-store/index.md) · refutes pixel-format/lossless-compression as the [overview-3dmark05-gt1](../overview-3dmark05-gt1.md) first-order owner.
