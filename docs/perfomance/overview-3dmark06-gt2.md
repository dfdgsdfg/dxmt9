---
domain: root
workload: 3DMark06 GT2
title: "3DMark06 GT2 Performance — Current Bottleneck"
type: root-overview
status: current
updated: 2026-08-26
source: experiments/output/app-d3d9-3dmark06-dxmt9-off-graphics-r1-20260826; experiments/output/app-d3d9-3dmark06-dxmt9-attribution-gt2-ui-r2-20260826; traces/3dmark06-dxmt9-bottleneck-20260826/gt2/frame1200.gputrace; traces/3dmark06-dxmt9-bottleneck-20260826/gt2/analysis/frame1200-counters-xcode.csv
related: docs/perfomance/overview-3dmark06-gt1.md; docs/perfomance/overview-3dmark06-hdr1.md; docs/perfomance/overview-3dmark06-hdr2.md; specs/backend/gap.md
---

# 3DMark06 GT2 Performance — Current Bottleneck

> **Current verdict at `b4b77cb8`: replay and encode serialization are the
> strongest measured owners.** GT2 is the best of these four scenes for a
> bounded replay/encode overlap or parallel-encode experiment; mixed GPU ALU,
> texture, and buffer pressure is secondary.

## Measurement Contract

The 2026-08-26 measurement used the Apple M1 8-core GPU, Sikarugir-CX 24.0.7,
1280x720, no anti-aliasing, and the current dxmt9 defaults.

- The observer-free value is the benchmark-owned Result Details value from one
  graphics-only run with dxmt9 counters, frame sampling, and encoder breakdown
  disabled. Advanced Edition did not emit the requested `.3dr` file.
- Runtime attribution is a separate counter-enabled GT2 run. The steady set
  keeps draw-bearing presents, drops the first transition stall, and excludes
  rows above 200ms.
- Metal counters come from one sequence-1200 frame and describe that frame's
  mechanism rather than the whole-run distribution.
- CPU phase counters may overlap or nest. `submit_draw` does not include every
  application or PE-side producer cost.

## Current Measurements

| Metric | Result |
|---|---:|
| observer-free official FPS | **`18.605`** |
| observer-free frame time | `53.749ms` |
| attribution FPS / wall | `18.234` / `54.843ms` |
| producer `submit_draw` | `5.850ms/present` |
| replay | `18.414ms/present` |
| encode chunk | `18.569ms/present` |
| completion wait | `12.332ms/present` |
| GPU command buffers | `11.776ms/present` |

The attribution run is `2.0%` below the observer-free value, small enough for
the phase ownership to be representative.

## Command And Pass Shape

| Metric | Per Present |
|---|---:|
| D3D draw calls | `1,880.63` |
| render passes | `19.86` |
| command buffers | `4.00` |
| PSO binds | `505.72` |
| PSO builds | `0.607` |

The sequence-1200 encoder breakdown contains 21 draw-encoder passes and no
floating-point render target or resolve action. Color attachment load/store
traffic is `93.741/105.816MB`; pass endings are eleven clears, nine
render-target changes, and one Present.

## Metal Frame

The captured frame reports `12.38ms` GPU time, four command buffers, 34 render
encoders, 1,571 Metal draws, 5,181,364 vertices, and a `61.52MiB` bandwidth
footprint.

| Weighted limiter | Result |
|---|---:|
| ALU | `49.79%` |
| texture read / write | `37.46%` / `9.76%` |
| buffer read | `23.22%` |
| MMU / LLC | `9.74%` / `20.57%` |
| shaded-vertex read | `48.04%` |
| cull / clip | `53.18%` / `19.73%` |
| tile partial renders | **`0`** |

The GPU workload is mixed rather than dominated by one bandwidth or tile
limiter. The percentages are independent activity weights and do not sum to
100%.

## Bottleneck And Next Gate

GT2 is CPU-led: replay and encode each consume about 18.5ms while average GPU
command-buffer time is about 11.8ms. This is the clearest current workload for
a policy that overlaps replay with encode or parallelizes only proven
independent pass partitions.

The gate is not FPS alone. A candidate must reduce replay/encode wall while
keeping `4.00` command buffers, pass count, render-target traffic, and tile
partial renders non-increasing. If that succeeds, the mixed ALU and texture
pressure becomes the next GPU target. FP RT, resolve, and tile-spill policies
are not supported by this frame.
