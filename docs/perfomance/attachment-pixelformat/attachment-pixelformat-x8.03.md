---
domain: attachment-pixelformat
workload: 3DMark05 GT1
subcategory: x8
order: 03
title: X8 Shader Alpha-Fill Companion Probe
date: undated
type: experiment-run
status: rejected
source: specs/perfomance.plan.md#L7080-L7256
---

# X8 Shader Alpha-Fill Companion Probe

**Question / hypothesis.** Move the D3D X8 alpha-fill contract out of the Metal
`PixelFormatView` and into shader code, so an X8 RT can drop its shader-read
view safely. Combined with X8 view suppression, does the texture/store or
VS-write bucket move?

**Method.** `DXMT9_X8_SHADER_ALPHA_FILL=1` as an opt-in companion to
`DXMT9_SUPPRESS_X8_RT_PIXEL_FORMAT_VIEW=1`: when an active fragment sampler binds
an X8 texture, the shader variant forces sampled alpha to `1.0` in MSL.
`ShaderVariantKey::x8AlphaOneTextureMask` participates in equality/hash;
`ShaderSourceContext::x8AlphaOneTextureMask` drives `dxmt9_x8_alpha_one(...)`
wrapping in translated PS and FFP fragment source; breakdown records
`x8_shader_alpha_fill_{samples,mask_or}`. Local no-gputrace breakdown
`app-d3d9-3dmark05-x8-alpha-fill-breakdown-r1`, then Xcode gputrace
`app-d3d9-3dmark05-x8-alpha-fill-gputrace-r2` finalized with
`--require-xcode-counter-coverage --require-dxmt-join-coverage --require-top-pso-attribution`.

**Result.** Xcode gputrace r2 frame60: total GPU `34.641ms`, top-3 `34.194ms`
(`98.71%`), top-3 VS buffer write `1627.246MiB`, unexplained buffer write
`1627.601MiB` / `1.000x`.

| seq/enc | GPU share | VS buffer write | X8 RT samples | alpha-fill samples | mask |
|---|---:|---:|---:|---:|---:|
| `60/2` | `56.83%` | `981.192MiB` | `0` | `0` | `0x0` |
| `60/1` | `24.99%` | `421.109MiB` | `0` | `0` | `0x0` |
| `60/0` | `16.90%` | `224.945MiB` | `0` | `0` | `0x0` |
| `60/8` | `0.35%` | `0.000MiB` | `2` | `2` | `0x3` |
| `60/3..7` | `<0.22%` ea | `0.000MiB` | `1` ea | `1` ea | `0x1` |

Offline Metal codegen on the matched top shaders: VS IR return aggregate `184B`,
single `128B` local scratch, vs Xcode `1150.8B`–`1602.6B` VS write per
invocation (`6.25x`–`8.71x` of IR return).

**Verdict.** Rejected as the primary GT1 GPU fix. The companion path is active
exactly where attribution predicted (the small post/resolve passes), but the hot
encoders `60/0/1/2` have zero X8 sampling and zero alpha-fill — and the top-3 VS
buffer write is unchanged at `~1627.25MiB`. The remaining Summary insight still
points at the fmt16/R32F `PixelFormatView` resource, not the X8 alias path. The
surviving owner is below MSL-visible structure: hidden Apple GPU
vertex/tiler/parameter backend storage.

**Related.** [[attachment-pixelformat]] · last in the X8 sequence, follows the sampler-binding attribution [[attachment-pixelformat-x8.02]] · confirms the [[hidden-backend-storage]] owner survives · offline codegen echoes [[shader-codegen]] (visible temp/scratch too small) · refutes pixel-format as the [[overview]] first-order owner.
