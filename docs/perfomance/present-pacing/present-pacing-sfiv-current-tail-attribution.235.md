---
domain: present-pacing
workload: SFIV Benchmark (D3D9)
title: "Present-Pacing #235 - Current SFIV Tail Is CPU/Producer Cadence, Not A GPU Hot Frame"
type: leaf
status: current
updated: 2026-08-23
source: experiments/output/app-d3d9-sfiv-benchmark-post-writing-frontier-frame-sampling-r1; experiments/output/app-d3d9-sfiv-benchmark-post-writing-frontier-systemtrace-r1; traces/app-d3d9-sfiv-post-writing-frontier-tail-r1/metal-system.trace; traces/app-d3d9-sfiv-post-writing-frontier-tail-r1/analysis/metal-gpu-intervals.xml; traces/app-d3d9-sfiv-post-writing-frontier-tail-r1/analysis/metal-command-buffers.xml; traces/app-d3d9-sfiv-post-writing-frontier-tail-r1/analysis/time-profile.xml
related: docs/perfomance/overview-sfiv.md; docs/perfomance/present-pacing/log.md
---

# Present-Pacing #235 - Current SFIV Tail Is CPU/Producer Cadence, Not A GPU Hot Frame

## Question

After removing the queue-frontier black-screen regression, does current SFIV
still have a particular GPU-hot frame, or does its residual wall-time tail come
from CPU production and pacing?

## Method

The first run used the normal `perf` profile with
`DXMT9_PERF_FRAME_SAMPLING=1` and `DXMT_PERF_COUNTERS=1` for 60 seconds. A
second independent run recorded a 10-second all-processes Metal System Trace
during the fight. The trace exported `metal-gpu-intervals`, application
command-buffer submissions, Metal errors, and the time profile. Trace overhead
is not used as an FPS baseline; it is only an independent GPU/CPU ownership
check.

Both runs used the catalogue-selected Sikarugir-CX 24.0.7 Wine and the rebuilt
`build-x86_64-builtin` provider. Both completed with a rendered benchmark
capture, normal scene progression, and zero GPU command-buffer or Metal errors.

## Production frame sample

The clean run encoded `3,180` Presents and sampled `3,190` positive frame
intervals over `76.480s`, for `41.710` sampled FPS. The in-application capture
reported `AVERAGE: 42.60` FPS.

| Metric | p50 | p95 | p99 | max |
|---|---:|---:|---:|---:|
| frame wall | `21.356ms` | `54.179ms` | `65.759ms` | `145.580ms` |
| encode chunk CPU | `5.871ms` | `12.094ms` | `15.106ms` | `66.923ms` |
| completion wait | `2.462ms` | `7.157ms` | `9.644ms` | `71.431ms` |
| drawable acquire wait | `0.053ms` | `6.989ms` | `9.721ms` | `13.252ms` |
| GPU command buffers | `2.135ms` | `6.483ms` | `8.811ms` | `15.028ms` |

There are eight frame samples at or above `100ms`. Their GPU command-buffer
times are `1.747-7.534ms`, and their drawable waits are `0.043-0.108ms`.
Seven also have only `0-3ms` of completion wait; the remaining sample has
`6.873ms`. None of the run's GPU samples exceeds the `16.667ms` display
period. The long wall intervals therefore cannot be attributed to a recurring
GPU-hot frame or drawable pacing.

There are isolated backend outliers, but they are not the dominant repeated
shape: one `69.409ms` wall sample contains `65.617ms` of draw-encode CPU, and
one `85.148ms` sample contains `71.431ms` of completion wait. They should stay
visible as regression sentinels rather than be generalized into the current
SFIV ceiling.

## Metal System Trace cross-check

The 10-second trace contains `14,150` SFIV depth-0 GPU intervals. Their total
active time is `1,684.312ms`; the longest individual interval is `7.174ms`.
The longest SFIV application command-buffer event is `6.09ms`, and the Metal
error table is empty. The former `88-96ms` scene-pass cluster is absent.

The time profile records `21,460` one-millisecond running samples for the SFIV
process across its CPU threads. The hottest unnamed game thread accounts for
`8,679` samples, while `dxmt9-encode` accounts for `3,328`; several other game
threads account for another `2,636`, `2,143`, `1,245`, `1,097`, and `1,062`
samples. This is a multi-core CPU-active workload, not a GPU channel held by a
single long render pass.

## Verdict

The current repeated SFIV tail is **CPU/producer cadence dominated**, with
smaller completion and drawable-pacing contributions. The unnamed hot threads
include the 32-bit game/Wine/Rosetta execution domain, so the exact split
between game simulation, Wine thunking, and PE recording requires a separate
non-throughput sampler if it becomes actionable. The available evidence is
already sufficient to reject a particular GPU-hot frame as the present
ceiling owner.

Future SFIV performance work should first attribute the producer-side gaps.
GPU shader or render-pass work should be reopened only if a fresh production
sample again shows command-buffer or labeled pass time near the frame wall.
