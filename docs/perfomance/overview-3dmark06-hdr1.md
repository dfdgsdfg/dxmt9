---
domain: root
workload: 3DMark06 HDR1
title: "3DMark06 HDR1 Performance — Current Bottleneck"
type: root-overview
status: current
updated: 2026-08-26
source: experiments/output/app-d3d9-3dmark06-dxmt9-off-graphics-r1-20260826; experiments/output/app-d3d9-3dmark06-dxmt9-attribution-hdr1-ui-r4-20260826; traces/3dmark06-dxmt9-bottleneck-20260826/hdr1/frame1200.gputrace; traces/3dmark06-dxmt9-bottleneck-20260826/hdr1/analysis/frame1200-counters-xcode.csv
related: docs/perfomance/overview-3dmark06-hdr2.md; docs/perfomance/overview-3dmark06-gt1.md; docs/perfomance/overview-3dmark06-gt2.md; docs/perfomance/baselines/baselines-3dmark06-wined3d.05.md
---

# 3DMark06 HDR1 Performance — Current Bottleneck

> **Current verdict at `b4b77cb8`: PSO provider/cache churn is the first
> bottleneck.** Floating-point render-target transitions and attachment
> traffic are real secondary costs, but optimizing render passes before the
> PSO path would target the wrong owner.

## Measurement Contract

The 2026-08-26 measurement used the Apple M1 8-core GPU, Sikarugir-CX 24.0.7,
1280x720, no anti-aliasing, and the current dxmt9 defaults.

- The observer-free value is the benchmark Result Details value from one
  graphics-only run with all dxmt9 performance observers disabled. Advanced
  Edition did not emit the requested `.3dr` file.
- Runtime attribution is a separate counter-enabled HDR1 run. Steady rows keep
  draw-bearing presents, drop the first transition stall, and exclude rows
  above 200ms.
- The sequence-1200 Metal capture is one mechanism frame. Its encoder counts
  need not equal the runtime average.
- CPU phase counters can overlap or nest. PSO-prefetch accumulation is an owner
  signal, not an additive term to place beside encode wall.

## Current Measurements

| Metric | Result |
|---|---:|
| observer-free official FPS | **`22.066`** |
| observer-free frame time | `45.318ms` |
| attribution FPS / wall | `21.940` / `45.578ms` |
| producer `submit_draw` | `2.569ms/present` |
| replay | `7.981ms/present` |
| encode chunk | `11.141ms/present` |
| completion wait | `11.525ms/present` |
| GPU command buffers | `10.969ms/present` |

The counter-enabled FPS is only `0.6%` below observer-free throughput.

## PSO And Command Shape

| Metric | Per Present |
|---|---:|
| D3D draw calls | `796.84` |
| render passes | `67.04` |
| command buffers | `4.00` |
| PSO binds | `154.54` |
| PSO builds | **`21.034`** |
| PSO-prefetch CPU accumulation | **`22.584ms`** |
| prefetch draw lookup | `21.144ms` |
| prefetch key resolve | `0.845ms` |

GT1, GT2, and HDR2 stay below one PSO build per Present. HDR1 builds about 21
despite only about 155 binds, making PSO identity, cache reuse, archive reuse,
or duplicate provider work the uniquely strong CPU mechanism. The current
measurement proves the owner class, not which of those sub-causes is final.

## Floating-Point Render Targets

The sequence-1200 encoder breakdown records:

| Metric | Result |
|---|---:|
| draw-encoder passes | `74` |
| floating-point passes / draws | `64` / `729` |
| FP entries / exits | `8` / `8` |
| Metal resolve actions | **`0`** |
| FP attachment load / store | `97.607/121.757MB` |
| all attachment load / store | `164.208/196.746MB` |

Pass endings are 19 clears, 54 render-target changes, and one Present. Xcode
also exposes clear/blit work, producing 114 render and 20 blit encoders for the
captured frame. The cost is FP target ping-pong and load/store preservation,
not an MSAA resolve path.

## Metal Frame

The captured frame reports `12.38ms` GPU time, four command buffers, 1,309
Metal draws, 6,540,707 vertices, and a `96.38MiB` bandwidth footprint.

| Weighted limiter | Result |
|---|---:|
| ALU | `38.69%` |
| texture read / write | `39.70%` / `11.73%` |
| buffer read | `2.68%` |
| MMU / LLC | `6.01%` / `11.91%` |
| shaded-vertex read | `57.07%` |
| cull / clip | `60.26%` / `22.38%` |
| tile partial renders | **`0`** |

The texture and vertex/front-end activity makes FP-pass traffic a plausible
second GPU target, but there is no tile spill. Limiter percentages are
independent activity weights and do not sum to 100%.

## Bottleneck And Next Gate

The first experiment must classify PSO keys by frequency and prove whether the
21 builds per Present are unique variants, failed cache reuse, archive
non-residency, or duplicate concurrent creation. The acceptance gate is a
large fall in PSO builds and prefetch CPU with unchanged pixels, four command
buffers, and no added pass or tile traffic.

Only after that gate should HDR1 target FP attachment load/store or pass
coalescing. A render-pass-only change cannot claim to fix the current primary
bottleneck.
