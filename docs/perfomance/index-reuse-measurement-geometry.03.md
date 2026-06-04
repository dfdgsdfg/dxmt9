---
domain: index-reuse-measurement
subcategory: geometry
order: 03
title: Indexed Draw-Size Histogram Probe
date: 2026-06-01
type: measurement
status: accepted
source: specs/perfomance.plan.md#L3432-L3476
---

# Indexed Draw-Size Histogram Probe

**Question / hypothesis.** After the dedup probe, the last ambiguity: is the hot
VS bucket driven by many *tiny repeated* draws, or by *large indexed* draws that
make Apple GPU internal vertex/parameter storage scale badly?

**Method.** `DXMT9_PERF_ENCODER_BREAKDOWN=1` extended with per-encoder draw-size
fields: `draw_primitive_min/max`, `draw_vertex_min/max`, primitive buckets
`1_63 / 64_255 / 256_1023 / 1024_4095 / 4096_plus`, and vertex buckets
`1_255 / 256_1023 / 1024_4095 / 4096_16383 / 16384_plus`. Run
`app-d3d9-3dmark05-draw-size-gputrace-r1` with same-`seq/enc` Xcode join.

**Result.** Run-level neutral vs regenerated baseline (`draws_per_present +0.10%`,
`tile_preservation_mib +0.52%`, `gpu_command_buffer_time_ms +0.36%`). For
`seq=60` the hot encoders are NOT tiny-draw dominated:

| seq/enc | draws | prim/draw | prim min/max | vert/draw | vert min/max | large prim/vert share |
|---|---:|---:|---:|---:|---:|---:|
| `60/2` | `187` | `2082.2` | `2 / 22622` | `6246.7` | `6 / 67866` | `0.56 / 0.36` |
| `60/1` | `156` | `1466.2` | `2 / 22622` | `4398.6` | `6 / 67866` | `0.46 / 0.33` |
| `60/0` | `42` | `2316.5` | `12 / 22622` | `6949.6` | `36 / 67866` | `0.62 / 0.40` |

Top-3 VS buffer traffic `1627.395MiB` vs explicit dxmt writer bytes `0.444MiB`
(unexplained ratio `1.000x`). Top-3 signatures `330` unique / `55` duplicate.
`385` draws span `715,395` triangles (top-3 from reuse probe).

**Verdict.** Tiny-draw replay rejected; real large indexed primitive pressure
accepted. Each hot row reaches `22,622` primitives / `67,866` vertices in a
single draw with large-primitive draw share `0.46–0.62`. Confirms the hot frame
is large indexed primitive pressure plus a hidden Apple GPU
vertex/tiler/parameter-storage bucket.

**Related.** [[index-reuse-measurement]] · follows
[[index-reuse-measurement-geometry.02]] · pairs with reuse counts
[[index-reuse-measurement-reuse.01]] · confirms [[hidden-backend-storage]] ·
large draws feed [[index-cache-locality]] selection.
