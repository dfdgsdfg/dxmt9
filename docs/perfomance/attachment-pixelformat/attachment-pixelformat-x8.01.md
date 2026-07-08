---
domain: attachment-pixelformat
workload: 3DMark05 GT1
subcategory: x8
order: 01
title: Broad X8 Suppression Attempt
date: undated
type: experiment-run
status: rejected
source: specs/perfomance.plan.md#L6929-L6988
---

# Broad X8 Suppression Attempt

**Question / hypothesis.** Can the Xcode `fmt2` lossless-compression hint be
removed by suppressing the shader-read view on all `X8R8G8B8`/`X8B8G8R8` render
targets, and does that move the bottleneck?

**Method.** New isolated flag `DXMT9_SUPPRESS_X8_RT_PIXEL_FORMAT_VIEW=1`
(wrapper `--suppress-x8-rt-pixel-format-view`), kept separate from
`DXMT9_SUPPRESS_RT_PIXEL_FORMAT_VIEW` so R32F and X8 can be compared
independently. It suppresses only X8-family RT shader-read views. No-gputrace run
`app-d3d9-3dmark05-suppress-x8-rt-pixel-format-view-nogputrace-r1`; retained
compact evidence `frame60-x8-suppression-aborted-summary.csv`,
`frame60-x8-suppression-sample-encoders.csv`,
`frame60-x8-suppression-aborted-report.md`.

**Result.** The run did **not** complete and produced no `result.json` (manually
terminated after the timeout window), so it is not a valid perf datapoint. The
extracted evidence still proves flag behavior:

| Metric | Value |
|---|---:|
| Encoder rows observed before termination | `17,321` |
| Max seq seen | `1,480` |
| X8 RT rows | `13,072` |
| X8 RT rows with view suppressed | `13,072` |
| X8 RT rows with textured draws in same encoder | `13,071` |
| R32F RT rows | `4,249` |
| R32F RT rows with view kept | `4,249` |

**Verdict.** Rejected. Broad X8 RT `PixelFormatView` suppression is too coarse
for GT1: almost every X8 RT row (`13,071 / 13,072`) sits in an encoder with
textured draws, and `usage=0x2` (`UsageRenderTarget`) is not an adequate
unsampled-resource proof. A correctness-preserving X8 optimization needs a
resource-lifetime proof, a per-texture sampled-channel proof, or a shader
variant that preserves the D3D X8 alpha-fill contract without a Metal
`PixelFormatView`.

**Related.** [attachment-pixelformat](../attachment-pixelformat.md) · follows the metadata probe [attachment-pixelformat-metadata.01](attachment-pixelformat-metadata.01.md), narrowed by the sampler-binding attribution [attachment-pixelformat-x8.02](attachment-pixelformat-x8.02.md) · does not move the [hidden-backend-storage](../hidden-backend-storage.md) owner.
