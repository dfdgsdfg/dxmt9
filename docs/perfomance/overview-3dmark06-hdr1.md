---
domain: root
workload: 3DMark06 HDR1
title: "3DMark06 HDR1 Performance — Current Bottleneck"
type: root-overview
status: current
updated: 2026-08-28
source: experiments/output/app-d3d9-3dmark06-dxmt9-off-graphics-r1-20260826; experiments/output/app-d3d9-3dmark06-dxmt9-attribution-hdr1-ui-r4-20260826; experiments/output/app-d3d9-3dmark06-hdr1-autogen-fix-r115-20260827; experiments/output/app-d3d9-3dmark06-hdr1-pso-diag-r4-20260828; experiments/output/app-d3d9-3dmark06-hdr1-pso-identity-components-r5-20260828; experiments/output/app-d3d9-3dmark06-hdr1-pso-layout-components-r6-20260828; experiments/output/app-d3d9-3dmark06-hdr1-pso-optimized-perf-r1-20260828; experiments/output/app-d3d9-3dmark06-hdr1-stream-offset-canonical-observer-off-r7-20260828; traces/3dmark06-dxmt9-bottleneck-20260826/hdr1/frame1200.gputrace; traces/3dmark06-dxmt9-bottleneck-20260826/hdr1/analysis/frame1200-counters-xcode.csv
related: docs/perfomance/overview-3dmark06-hdr2.md; docs/perfomance/overview-3dmark06-gt1.md; docs/perfomance/overview-3dmark06-gt2.md; docs/perfomance/baselines/baselines-3dmark06-wined3d.05.md
---

# 3DMark06 HDR1 Performance — Current Bottleneck

> **Current verdict (2026-08-28): quadratic handle publication and runtime
> stream-offset PSO fanout are removed. HDR1 now sits on a narrow CPU encode
> versus GPU boundary rather than a PSO-build wall.** Floating-point
> render-target transitions and attachment traffic remain real secondary
> costs, but the observer-off run still does not establish a GPU-only
> bottleneck and therefore does not authorize a render-pass semantic change.

> **Correctness addendum (2026-08-27):** the former angle-dependent water
> crop/checker pattern was stale reflection mips, not a shader or sampler-LOD
> defect. The PE factory advertised `D3DOK_NOAUTOGEN`, so 3DMark created an
> explicit 11-level fallback texture but updated only level 0. R-FORMAT-16 now
> exposes the required one-level D3D9 object over a complete hidden Metal
> pyramid and regenerates it after ordered level-0 writes before sampling.
> The `hdr1-autogen-fix-r115` perf wild run completed 2,820 Presents with a
> clean scene capture and zero Metal command-buffer errors, chunk rejects, or
> missing-pipeline draws. The short black interval also occurs on the WineD3D
> control and is benchmark scene sequencing, not this defect.

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
These 2026-08-26 throughput values predate the R-FORMAT-16 correctness fix and
must not be used as a post-fix promotion baseline without a fresh official
result run.

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

## 2026-08-28 PSO Identity And Publication Update

The draw-handle table formerly cloned the complete published slot prefix for
every new PSO. At the diagnostic run's 65,535-slot ceiling, the old arithmetic
would copy 621,369,378 `PsoSlot` values, about 203.8GB of logical slot traffic.
The replacement publishes stable 64-slot segments with release/acquire
visibility. In the opt-in diagnostic run it allocated 1,024 segments
(22,020,096 bytes) and spent 10.965ms total publishing all 65,535 slots; the
maximum single publication was 0.110ms.

The same run separated 80,307 final misses/insertions from 309,249 probe
lookups and observed only 89 generated-source tuples. Its maximum final-key
fanout per source tuple was 22,091. Retained descriptor axes were much smaller
(52 vertex sources, 55 fragment sources, five texture masks, 14 texture-type
shapes, four color-format shapes, ten blend shapes, three depth/stencil shapes,
and four mode shapes). In contrast, the new pre-source backend-identity axis
filled its bounded 4,096-entry set and then failed closed, so 4,096 is a lower
bound rather than the exact cardinality; the diagnostic reported 76,091
saturated observations. This localizes the large fanout before generated MSL
source identity rather than in Metal attachment/sampler descriptor shape.

A follow-up GUI-qualified HDR1 diagnostic run isolated that opaque backend
identity into its construction components. It completed 3,480 Presents with
80,442 final insertions and observed 27 vertex-shader identities, 35 canonical
pixel-shader identities, two clip masks, five FVFs, three depth formats, and
two stencil formats. Only the vertex-layout component saturated its bounded
4,096-entry set, exactly matching the composite backend-identity lower bound.
The aggregate overflow counter cannot attribute each saturated observation to
one component, but every other component stayed far below the bound.

The subsequent layout-decomposition run completed 3,480 Presents with 80,512
final insertions. Stream-zero offset alone saturated at 4,096 distinct values;
declaration elements had 12 shapes, stream-zero stride five, extra-stream
offsets 23, and extra-stream strides three. This proves that the high-cardinality
input is a runtime binding value rather than declaration or source layout. The
production vertex-declaration identity now excludes stream offsets while
retaining all strides, and native tests pin both key equality and byte-identical
programmable vertex MSL under offset-only changes.

The observer-off r7 run then completed 3,900 Presents in the same 250-second
harness window. Draw PSO builds fell from 36,033 in the earlier 1,680-Present
observer-off run to 262, or from 21.45 to 0.067 per Present. PSO-prefetch p50
fell from 14.503ms to 1.451ms; encode-chunk p50 fell from 13.685ms to 12.343ms.
Completion-wait and GPU-command-buffer p50 were 12.086ms and 11.577ms,
respectively, preserving the CPU/GPU near tie after the PSO wall disappeared.
The run reported zero GPU command-buffer errors, pipeline-build failures,
missing-pipeline draws, and slot exhaustion. It produced no official `.3dr`
FPS result, so the Present-count increase and phase values are mechanism
evidence rather than a published benchmark score.

The observer-off GUI-qualified HDR1 run selected exactly one HDR test and
completed 1,680 Presents with zero GPU errors, chunk rejects, or skipped draws.
It did not emit a `.3dr`, so it is mechanism evidence rather than an official
FPS result. The decisive p50 values are encode chunk 13.685ms, PSO-prefetch
14.503ms (nested/overlapping), GPU command buffer 12.797ms, and completion wait
13.325ms. That near tie keeps FP pass coalescing behind its GPU-bottleneck gate.
The earlier observer-off run built 36,033 draw PSOs (21.45 per Present) without
reaching the slot limit. The longer diagnostic run reached the 16-bit
65,535-slot handle domain and recorded 67,026 subsequent exhaustion attempts,
but neither run recorded a missing-pipeline draw. The stream-offset
canonicalization removes that production saturation mechanism. Remaining work
must remeasure the post-fix CPU/GPU boundary; it is not a blind
attachment-store relaxation.
