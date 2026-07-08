---
domain: attachment-pixelformat
workload: 3DMark05 GT1
subcategory: x8
order: 02
title: X8 Sampler Binding Attribution
date: undated
type: measurement
status: tooling
source: specs/perfomance.plan.md#L6989-L7079
---

# X8 Sampler Binding Attribution

**Question / hypothesis.** The broad X8 suppression failed because
`UsageRenderTarget` is not an unsampled proof. Does the *hot* GT1 encoder
actually sample any X8 RT alias, or is the sampling confined to small
post/resolve passes?

**Method.** Extend `DXMT9_PERF_ENCODER_BREAKDOWN=1` with active fragment-texture
binding attribution after shader texture-mask filtering: `fragment_texture_binding_*`,
`x8_rt_texture_binding_*` (samples, mask, unique handles, shader-read-view samples,
active-RT alias self-sample samples), and `x8_shader_alpha_fill_*` /
`x8_rt_texture_binding_last_{stage,handle}`. Partial no-gputrace run
`app-d3d9-3dmark05-x8-sampler-binding-nogputrace-r1` (terminated, not a valid
perf datapoint); seq60 rows were fully emitted before termination.

**Result.**

| seq/enc | RT fmt | draws | texture mask | fragment tex bindings | X8 RT tex bindings |
|---|---:|---:|---:|---:|---:|
| `60/0` | `2` | `42` | `0x7f` | `0` | `0` |
| `60/1` | `16` | `156` | `0x0` | `0` | `0` |
| `60/2` | `2` | `187` | `0x7f` | `1268` | `0` |
| `60/3..60/8` | mostly `2` | small post/resolve | `0x7f` | `46` agg | `7` agg |

Aggregate: `10,416` X8 RT texture binding samples, all needing the shader-read
view; active-RT alias self-sample count `0` in seq60 and in the partial-run
aggregate.

**Verdict.** Tooling. The large hot `60/2` encoder (`1268` active fragment
texture bindings) samples **zero** X8 RT aliases — X8 RT sampling appears only in
the small post/resolve encoders `60/3..60/8` (`7` samples). With no same-pass
read/write aliasing (`0` self-samples), allocation-wide X8 view suppression is
unjustified; a correctness-preserving fix should target the actual sampling sites
(sampler/PSO variant for D3D X8 alpha-fill) or prove per-alias lifetime.

**Related.** [attachment-pixelformat](../attachment-pixelformat.md) · narrows the broad attempt [attachment-pixelformat-x8.01](attachment-pixelformat-x8.01.md), motivates the shader alpha-fill companion [attachment-pixelformat-x8.03](attachment-pixelformat-x8.03.md) · shares the encoder-breakdown surface with [state-churn-encode](../state-churn-encode.md).
