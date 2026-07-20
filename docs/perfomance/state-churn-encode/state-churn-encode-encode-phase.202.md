---
domain: state-churn-encode
workload: 3DMark05 GT1/GT2/GT3 and SFIV Benchmark
subcategory: encode-phase
order: 202
title: Direct-Cbuf Cross-Workload Generality Gate
date: 2026-07-20
type: experiment
status: accepted-general-cpu-cleanup-default-off
source: experiments/output/app-d3d9-3dmark05-direct-cbuf-generality-gt{1,2,3}-{off,on}-r{1,2}-20260720; experiments/output/app-d3d9-sfiv-benchmark-direct-cbuf-generality-sfiv-{off,on}-r{1,2}-20260720; experiments/output/app-d3d9-3dmark05-direct-cbuf-generality-gt3-visual-67s-retry-20260720; traces/app-d3d9-3dmark05-direct-cbuf-generality-gt{1,2,3}-on-r1-20260720/analysis/direct-cbuf-vs-off-r1.md; traces/app-d3d9-sfiv-benchmark-direct-cbuf-generality-sfiv-on-r1-20260720/analysis/direct-cbuf-vs-off-r1.md
related: docs/perfomance/state-churn-encode/state-churn-encode-encode-phase.143.md; docs/perfomance/state-churn-encode/state-churn-encode-encode-phase.148.md; docs/perfomance/overview.md
---

# Encode Phase 202 - Direct-Cbuf Cross-Workload Generality Gate

## Question and Verdict

Does the Stage 2b `DXMT9_ARGBUF_DIRECT_CBUF=1` path remain a safe and useful
CPU cleanup outside the original GT1 scout?

Yes for the constants-only Stage 2 workload family tested here. GT1, GT2, GT3,
and SFIV all remove the slot-30 argument-table path, reduce draw/chunk encode
CPU, complete without renderer errors, and preserve sampled FPS. The targeted
GT3 capture at `1:07.66` is also free of the former top-right quadrant noise.

This is not a default-promotion result. FPS moves only `+0.32%` to `+1.15%`,
while phase-sampled GPU p50 increases in GT3 and SFIV. The path also does not
apply when the resource-array lane is active. Keep the environment switch
default-off as a validated CPU/ABI probe.

## Measurement Contract

- HEAD: `1aba99a0f25efe5a589ff9d542cae0f7e3e6e380`.
- Apple M1 8-core GPU, macOS 26.5.2, Sikarugir-CX 24.0.7 Wine.
- Same-build ABBA order per workload: OFF r1, ON r1, ON r2, OFF r2.
- Two completed samples per lane, `perf` profile, frame sampling, no
  `.gputrace`, engine-default commit-replay offload and opaque-depth index
  cache.
- 3DMark runs used frontmost supervision; SFIV ran without concurrent
  frontmost manipulation.
- Values below are medians of the two run-level values in each lane. GPU
  p50/p95 is the distribution of per-frame
  `gpu_command_buffer_time_ms / gpu_command_buffer_time_samples`, which keeps
  the comparison tied to sampled frame phase.

## Cross-Workload Result

| Workload | FPS OFF -> ON | draw CPU / present | chunk CPU / present | frame-GPU p50 / p95 OFF -> ON | argbuf setup / present | table binds | errors |
|---|---:|---:|---:|---:|---:|---:|---:|
| GT1 | `20.908 -> 20.974` (`+0.32%`) | `9.487 -> 7.521ms` (`-20.72%`) | `12.014 -> 9.965ms` (`-17.05%`) | `1.066/13.650 -> 1.051/13.534ms` | `1.941 -> 0ms` | `1,258,014 -> 0` | `0 -> 0` |
| GT2 | `8.087 -> 8.181` (`+1.15%`) | `24.510 -> 17.265ms` (`-29.56%`) | `29.831 -> 22.393ms` (`-24.93%`) | `24.965/66.647 -> 24.069/56.400ms` | `7.907 -> 0ms` | `504,018 -> 0` | `0 -> 0` |
| GT3 | `27.753 -> 27.893` (`+0.50%`) | `7.358 -> 5.695ms` (`-22.60%`) | `9.470 -> 7.739ms` (`-18.27%`) | `9.300/58.050 -> 9.724/58.156ms` | `1.481 -> 0ms` | `767,701 -> 0` | `0 -> 0` |
| SFIV | `45.544 -> 45.694` (`+0.33%`) | `4.998 -> 3.370ms` (`-32.57%`) | `8.686 -> 7.594ms` (`-12.57%`) | `2.844/7.021 -> 3.099/7.325ms` | `1.513 -> 0ms` | `993,016 -> 0` | `0 -> 0` |

Every first-pair comparison report passed the required mechanism gates:

- `encode_draw_cpu_ms` decreases;
- `encode_chunk_cpu_ms` per present decreases;
- argbuf setup, open, and cbuf-update CPU per present decrease.

The stable cross-workload invariant is therefore CPU-path removal, not an FPS
claim. GT2 benefits on both CPU and sampled GPU tails, GT1 is neutral, and
GT3/SFIV expose small GPU-side tradeoffs that the current wall is able to hide.

## Correctness and ABI Coverage

The ON captures show expected GT1 bloom/muzzle effects, the GT2 forest scene,
the GT3 airship/water path, and SFIV character, lighting, glyph, and
post-effect output. The additional GT3 capture lands at `1:07.66`; the whole
top-right quadrant is normal and no filtered-noise rectangle is present. All
ON runs report `status=pass`, empty failure lists, zero GPU errors, and no
nonzero error/failure counters.

Focused native tests cover the shader/PSO ABI beyond these applications:

| Coverage | Evidence |
|---|---|
| FFP vertex, pixel, and tile Stage 2b source | `dxmt9-argbuf-hybrid-msl-spec` |
| Translated programmable vertex and pixel Stage 2b source | `dxmt9-argbuf-hybrid-msl-spec` |
| Direct slots `0/3` and host-slot contract | `dxmt9-argbuf-hybrid-msl-spec`, `dxmt9-shader-argbuf-binding-value-spec` |
| PSO/source identity and resource-array dominance | `dxmt9-backend-pipeline-key-spec`, `dxmt9-argbuf-hybrid-spec` |

All four focused targets pass. The resource-array dominance test is an
intentional limit: texture/sampler arrays retain the mutable slot-30 argument
table and suppress the direct-cbuf variant bit.

## Decision and Remaining Gate

Keep `DXMT9_ARGBUF_DIRECT_CBUF` default `0`:

1. Accept the path as broadly effective CPU cleanup for constants-only Stage
   2 passes.
2. Do not advertise an average-FPS win; all four results are noise-flat to
   modest.
3. Before default promotion, add a deterministic regression for the phase-148
   uniform-payload-source dirty rebind and repeat SFIV/GT3 GPU-phase sampling
   with at least three samples per lane.
4. Treat resource-array mode and non-Apple-Silicon Stage 1 fallback as outside
   this optimization's applicability, not failed direct-cbuf coverage.
