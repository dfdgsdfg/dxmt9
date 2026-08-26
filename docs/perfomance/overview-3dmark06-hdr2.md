---
domain: root
workload: 3DMark06 HDR2
title: "3DMark06 HDR2 Performance — Current Bottleneck"
type: root-overview
status: current
updated: 2026-08-26
source: experiments/output/app-d3d9-3dmark06-dxmt9-off-graphics-r1-20260826; experiments/output/app-d3d9-3dmark06-dxmt9-attribution-hdr2-ui-r1-20260826; traces/3dmark06-dxmt9-bottleneck-20260826/hdr2/frame1200.gputrace; traces/3dmark06-dxmt9-bottleneck-20260826/hdr2/analysis/frame1200-counters-xcode.csv
related: docs/perfomance/overview-3dmark06-hdr1.md; docs/perfomance/overview-3dmark06-gt1.md; docs/perfomance/overview-3dmark06-gt2.md; docs/perfomance/frame-lifecycle.md
---

# 3DMark06 HDR2 Performance — Current Bottleneck

> **Current verdict at `b4b77cb8`: the dominant time remains outside the named
> replay/encode and GPU scopes.** Application simulation, PE-side state work,
> scheduling, or pacing must be split before another Metal optimization is
> justified.

## Measurement Contract

The 2026-08-26 measurement used the Apple M1 8-core GPU, Sikarugir-CX 24.0.7,
1280x720, no anti-aliasing, and the current dxmt9 defaults.

- The observer-free value is the benchmark Result Details value from one
  graphics-only run with dxmt9 counters, frame sampling, and encoder breakdown
  disabled. Advanced Edition did not emit the requested `.3dr` file.
- Runtime attribution is a separate counter-enabled HDR2 run. Steady rows keep
  draw-bearing presents, drop the first transition stall, and exclude rows
  above 200ms.
- Metal counters come from one sequence-1200 frame and characterize its GPU
  mechanism, not every frame in the scene.
- CPU scopes may overlap or nest. `submit_draw` does not cover all app/PE work,
  so the unnamed wall residual is not idle time by definition.

## Current Measurements

| Metric | Result |
|---|---:|
| observer-free official FPS | **`16.270`** |
| observer-free frame time | `61.463ms` |
| attribution FPS / wall | `16.583` / `60.301ms` |
| producer `submit_draw` | `2.698ms/present` |
| replay | `8.593ms/present` |
| encode chunk | `12.258ms/present` |
| completion wait | `9.323ms/present` |
| GPU command buffers | `8.804ms/present` |

The attribution run is `1.9%` faster than the observer-free run, inside the
expected one-run noise band. It does not indicate negative observer cost.

## Command And Pass Shape

| Metric | Per Present |
|---|---:|
| D3D draw calls | `1,204.06` |
| render passes | `64.12` |
| command buffers | `4.00` |
| PSO binds | `97.49` |
| PSO builds | `0.580` |

Unlike HDR1, HDR2 has no PSO-build explosion.

## Floating-Point Render Targets

The sequence-1200 encoder breakdown records:

| Metric | Result |
|---|---:|
| draw-encoder passes | `64` |
| floating-point passes / draws | `58` / `1,036` |
| FP entries / exits | `6` / `6` |
| Metal resolve actions | **`0`** |
| FP attachment load / store | `56.679/71.425MB` |
| all attachment load / store | `102.309/117.054MB` |

Pass endings are ten clears, 53 render-target changes, and one Present. FP
target switching is frequent, but neither resolve work nor tile spill explains
the roughly 61ms observer-free frame.

## Metal Frame

The captured frame reports `5.92ms` GPU time, four command buffers, 75 render
encoders, 1,348 Metal draws, 7,254,590 vertices, and a `51.88MiB` bandwidth
footprint.

| Weighted limiter | Result |
|---|---:|
| ALU | `27.55%` |
| texture read / write | `19.33%` / `6.13%` |
| buffer read | `5.37%` |
| MMU / LLC | `5.21%` / `10.05%` |
| shaded-vertex read | `53.22%` |
| cull / clip | `51.61%` / `13.28%` |
| tile partial renders | **`0`** |

This is the lightest GPU frame of the four measured scenes. Limiter
percentages are independent activity weights and do not sum to 100%.

## Bottleneck And Next Gate

Average GPU command-buffer time is 8.8ms and the captured frame is 5.9ms,
leaving most of the 61ms observer-free frame outside the GPU. The named
producer, replay, and encode scopes also do not account for the wall ceiling.

The next gate is a frame-aligned application/PE/pacing attribution that covers
state setters, bridge crossings, thread runnable/wait time, and Present pacing.
Only a demonstrated GPU-critical FP pass should authorize attachment or
render-pass work. Parallel Metal encoding and tile optimization are not
supported as first actions by the current evidence.
