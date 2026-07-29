---
domain: root
workload: 3DMark05 GT3
title: "3DMark05 GT3 Performance — Current Baseline"
type: root-overview
status: current
updated: 2026-07-25
source: experiments/output/app-d3d9-3dmark05-current-v2-gt3-r3-retry1-20260719; experiments/output/app-d3d9-3dmark05-release-default-gt3-r1-20260725
related: docs/perfomance/overview.md; docs/perfomance/overview-3dmark05-gt1.md; docs/perfomance/overview-3dmark05-gt2.md
---

# 3DMark05 GT3 Performance — Current Baseline

## Current Result

Three completed current-runtime runs used the V2-only command wire,
`-gt3 -nosplash -nosysteminfo -noscreens`, no Metal frame capture, frame
sampling, frontmost supervision, and the engine-default offload plus
opaque-depth index-cache policy. The successful captures show expected GT3
airship-scene rendering and effects at their capture points. They are normal
performance sanity captures, not dense screenshots of the former 66-68-second
quadrant window. All GPU error counters are zero.

| Metric | Run median | Run range |
|---|---:|---:|
| sampled average FPS | `27.858` | `27.809-27.998` |
| sampled frames | `2,191` | `2,187-2,202` |
| sampled wall time | `78.650s` | `78.644-78.650s` |
| wall p50 | `29.094ms` | `29.061-29.491ms` |
| wall p95 | `85.705ms` | `85.150-85.768ms` |
| GPU CB p50 | `11.275ms` | `11.002-11.390ms` |
| GPU CB p95 | `13.661ms` | `13.590-13.741ms` |
| encoded presents | `2,192` | `2,188-2,203` |

The sampled average varies by only `0.68%` from the slowest to fastest run.
One first `r3` launch stopped early with `missing_capture`; it is excluded and
the completed `r3-retry1` run is the third sample. The failure contained no
renderer/GPU error and is not mixed into either the performance or visual
verdict.

The 2026-07-25 release-default spot check rebuilt commit `5dc7ca01` as
release/O3 and records `2,210` positive samples over `78.982s`, or `27.981`
sampled FPS. Wall p50/p95 is `29.355/82.801ms` and GPU-CB p50/p95 is
`7.729/11.071ms`; FPS is `0.44%` above the repeated median. The ordinary
`0:54.44` capture shows the airship, water, lighting, and a coherent full frame
without a quadrant rectangle, but it does not replace the exact 66-68-second
gate. Chunk/V2 rejects, GPU errors, pipeline failures, missing-pipeline draws,
and DCE activity are all zero.

The 2026-07-20 direct-cbuf generality gate
measures sampled FPS `27.753 -> 27.893` (`+0.50%`) while draw/chunk CPU falls
`22.60%/18.27%`, argbuf setup/binds become zero, and all errors stay zero.
An additional ON capture lands at `1:07.66` in the former quadrant-noise
window and shows normal water, airship, smoke, and full-frame output without
the top-right filtered-noise rectangle. Phase-sampled GPU p50 rises `4.57%`,
so the result does not establish a GPU win. The cross-workload CPU reduction
and subsequent deterministic dirty-rebind regression nevertheless support the
default-on promotion; GT3 GPU-phase behavior remains a monitoring item.

## Preserved V1/V2 Comparisons

The five same-build `command-chunk-v2-final2` promotion pairs are the broadest
retained V1/V2 GT3 comparison. Pair-wise medians show V2 process throughput
`-1.1%` and offload replay CPU/present `-30.8%` relative to V1. The throughput
result passed the plan's no-more-than-3% regression gate. GPU CB p95 changed
`+4.1%`, but varied strongly with scene progress; independent V1 and V2 leg
medians are not a cleaner FPS comparison. All ten legs had zero GPU errors and
V2 rejects.

Those captures predate the later rendering fix and contain historical GT3
artifacts, including the top-right quadrant corruption. They prove the wire
performance gate only. Current HEAD includes the dynamic draw-buffer
dependency fix (`3f07df6c`); the 2026-07-19 captures above provide whole-run
sanity evidence, while the targeted 66-68-second heuristic remains the
quadrant-specific correctness gate.

The exact same-build quadrant-glitch pair provides a direct V1/V2 GT3
comparison. It predates V1 removal and uses process-wide presents/elapsed
rather than current frame sampling, so compare within that pair only. Both
legs contain the same then-open quadrant artifact; neither is a correctness
oracle.

| Metric | V1 | V2 | Delta |
|---|---:|---:|---:|
| process throughput | `22.972 presents/s` | `24.226 presents/s` | `+5.46%` |
| GPU CB p50 | `11.933ms` | `11.258ms` | `-5.66%` |
| GPU CB p95 | `13.684ms` | `14.192ms` | `+3.71%` |
| GPU errors | `0` | `0` | unchanged |

The current three-run median process throughput is `24.572 presents/s`, or
`+6.97%` over the preserved exact-window V1 leg and `+1.43%` over its V2 leg.
This is a cumulative current-versus-historical result, not a wire-only delta.
It supports V2 as non-regressing on GT3 while the frame-sampled table above
remains the authoritative baseline for future changes.

## Next Attribution Gate

Typical GPU CB time is far below wall p95, so a future GT3 performance change
must distinguish scene-phase wall tails from GPU execution. Preserve the prior
15/20-second sea checks and 66-68-second quadrant window as correctness gates;
do not promote a tail improvement if bloom, fire, water, or the fixed quadrant
regresses.
