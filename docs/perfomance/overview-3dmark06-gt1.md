---
domain: root
workload: 3DMark06 GT1
title: "3DMark06 GT1 Performance — Current Bottleneck"
type: root-overview
status: current
updated: 2026-08-26
source: experiments/output/app-d3d9-3dmark06-dxmt9-off-graphics-r1-20260826; experiments/output/app-d3d9-3dmark06-dxmt9-attribution-gt1-r1-20260826; traces/3dmark06-dxmt9-bottleneck-20260826/gt1/frame1200.gputrace; traces/3dmark06-dxmt9-bottleneck-20260826/gt1/analysis/frame1200-counters-xcode.csv
related: docs/perfomance/overview-3dmark06-gt2.md; docs/perfomance/overview-3dmark06-hdr1.md; docs/perfomance/overview-3dmark06-hdr2.md; docs/perfomance/baselines/baselines-3dmark06-wined3d.05.md
---

# 3DMark06 GT1 Performance — Current Bottleneck

> **Current verdict at `b4b77cb8`: CPU translation and application work own
> the frame ceiling.** The Metal frame has real vertex/front-end and ALU
> pressure, but its command-buffer time is too small to explain the full frame.

## Measurement Contract

The 2026-08-26 measurement used the Apple M1 8-core GPU, Sikarugir-CX 24.0.7,
1280x720, no anti-aliasing, and the current dxmt9 defaults.

- The observer-free value comes from one graphics-only Advanced Edition run
  with all dxmt9 counters, frame sampling, and encoder breakdown disabled. The
  benchmark Result Details dialog is authoritative because Advanced Edition
  did not emit the requested `.3dr` file.
- Runtime attribution is a separate counter-enabled GT1 run. Steady rows keep
  draw-bearing presents, discard the first transition stall, and exclude rows
  above 200ms.
- The Metal result is one captured frame at sequence 1200. It is a mechanism
  sample, not a substitute for the steady runtime average.
- CPU phase counters can overlap or nest and must not be summed as an exclusive
  frame-time decomposition. `submit_draw` is only the instrumented producer
  draw-submit scope, not all application or PE-side work.

## Current Measurements

| Metric | Result |
|---|---:|
| observer-free official FPS | **`16.109`** |
| observer-free frame time | `62.077ms` |
| attribution FPS / wall | `14.998` / `66.678ms` |
| producer `submit_draw` | `4.752ms/present` |
| replay | `13.103ms/present` |
| encode chunk | `15.778ms/present` |
| completion wait | `14.981ms/present` |
| GPU command buffers | `14.443ms/present` |

The counter-enabled FPS is `6.9%` below the observer-free result. Use the
official value for throughput and the counter run only for attribution.

## Command And Pass Shape

| Metric | Per Present |
|---|---:|
| D3D draw calls | `1,467.15` |
| render passes | `34.83` |
| command buffers | `4.00` |
| PSO binds | `254.44` |
| PSO builds | `0.192` |

The sequence-1200 encoder breakdown contains 21 draw-encoder passes and no
floating-point render target. It records no Metal resolve action. Color
attachment load/store traffic is `84.337/88.023MB`; pass endings are ten
clears, ten render-target changes, and one Present.

## Metal Frame

The captured frame reports `9.10ms` GPU time, four command buffers, 32 render
encoders, 584 Metal draws, 4,215,956 vertices, and a `61.52MiB` bandwidth
footprint.

| Weighted limiter | Result |
|---|---:|
| ALU | `51.08%` |
| texture read / write | `15.16%` / `12.64%` |
| buffer read | `4.13%` |
| MMU / LLC | `7.91%` / `14.56%` |
| shaded-vertex read | `55.17%` |
| cull / clip | `66.99%` / `41.91%` |
| tile partial renders | **`0`** |

These limiter percentages are independent activity weights and do not sum to
100%. The high cull, shaded-vertex, and ALU readings identify the next GPU
ceiling after CPU work is removed; they do not make the current 62ms frame
GPU-bound.

## Bottleneck And Next Gate

GT1 is CPU-led. Replay and encode are the largest named dxmt9 CPU scopes, while
the remaining wall residual includes application simulation, PE state work,
scheduling, and pacing. The GPU command-buffer interval occupies only about a
quarter of the observer-free frame.

The next GT1 candidate must first move replay/encode or the unowned CPU
residual without increasing four command buffers or pass count. Only after that
movement should a GPU experiment target vertex invocation/cull/ALU cost. FP
render-target, resolve, bandwidth, and tile-spill work are not supported by
this frame.
