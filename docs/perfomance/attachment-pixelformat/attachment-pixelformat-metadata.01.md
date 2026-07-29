---
domain: attachment-pixelformat
workload: 3DMark05 GT1
subcategory: metadata
order: 01
title: Attachment Metadata Probe
date: 2026-06-01
type: measurement
status: tooling
outdated: retired-journal
source: specs/perfomance.plan.md#L6859-L6928
---

# Attachment Metadata Probe

> **Outdated — this leaf's only `source:` is the retired `specs/perfomance.plan.md` journal, which was deleted.** The numbers below cannot be re-derived or re-checked. Kept as history; do not cite it as current evidence.

**Question / hypothesis.** The R32F suppression left a specific unresolved
hint: Xcode's remaining lossless-compression warning was attached to `fmt2`
BGRA-like render targets, while the implemented flag only touched R32F/fmt16
resources. This instrumentation maps the Xcode compression hint to concrete
hot-encoder RT shapes.

**Method.** Extend `DXMT9_PERF_ENCODER_BREAKDOWN=1` with per-render-encoder
attachment metadata: RT/depth format, size, bytes-per-pixel, alias texture
handle, backing texture `desc.usage`, whether the surface needs a shader-read
swizzle, and whether it currently requests a shader-read view (and therefore
`PixelFormatView` usage). No-gputrace run
`app-d3d9-3dmark05-attachment-metadata-nogputrace-r1`; retained compact artifact
`frame60-attachment-metadata-summary.csv`. The no-gputrace comparison was neutral
(`gpu_command_buffer_time_ms -0.41%`), as expected for a pure instrumentation run.

**Result.** Key seq `60` rows:

| seq/enc | RT format | RT size | RT alias texture | RT usage | swizzle/view | Depth fmt | Depth size |
|---|---:|---:|---|---:|---:|---:|---:|
| `60/0` | `2` (`X8R8G8B8`) | `1024x768` | `0x...08c` | `0x2` | `1 / 1` | `41` | `1024x768` |
| `60/1` | `16` (`R32F`) | `2048x2048` | `0x...08d` | `0x2` | `1 / 1` | `41` | `1024x768` |
| `60/2` | `2` (`X8R8G8B8`) | `1024x768` | `0x...08c` | `0x2` | `1 / 1` | `41` | `1024x768` |

**Verdict.** Tooling. The Xcode `fmt2` compression hint now maps to the hot
`X8R8G8B8` RT0 in encoders `60/0` and `60/2`, both swizzled with a shader-read
view requested. `rt_texture_usage=0x2` is the D3D `UsageRenderTarget` bitset,
**not** proof the object can never be sampled — removing the X8 shader-read view
would break the D3D X8 alpha-fill sampling contract if the app later reads alpha.
Therefore any X8-family suppression must stay an opt-in diagnostic.

**Related.** [attachment-pixelformat](index.md) · follows the R32F probe [attachment-pixelformat-r32f.01](attachment-pixelformat-r32f.01.md) and motivates the broad X8 attempt [attachment-pixelformat-x8.01](attachment-pixelformat-x8.01.md) · shares the `DXMT9_PERF_ENCODER_BREAKDOWN` instrumentation surface with [state-churn-encode](../state-churn-encode/index.md) · informs [render-pass-store](../render-pass-store/index.md).
