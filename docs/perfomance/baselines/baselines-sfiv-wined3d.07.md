---
domain: baselines
workload: Street Fighter IV Benchmark
subcategory: renderer-comparison
order: 7
title: Street Fighter IV Sikarugir WineD3D Baseline
date: 2026-08-29
type: experiment-run
status: accepted-baseline
source: experiments/output/app-d3d9-sfiv-benchmark-wined3d-sikarugir-warm-20260829; experiments/output/app-d3d9-sfiv-benchmark-wined3d-sikarugir-final-r1-20260829
related: README.md; docs/perfomance/baselines/baselines-d9vk-moltenvk.06.md; docs/perfomance/baselines/baselines-wild-fps-refresh.04.md
---

# Street Fighter IV Sikarugir WineD3D Baseline

## Question

What throughput does Sikarugir-CX 24.0.7's builtin WineD3D path deliver in the
SFIV benchmark tracked for dxmt9 and D9VK/MoltenVK?

## Method

The run used Sikarugir-CX 24.0.7 with `d3d9=b`, no app-local `d3d9.dll`,
`--skip-stage`, and the repository's `perf` profile. A 75-second warm-up
preceded one recorded run. Both dxmt9 counters and diagnostic renderer HUDs
were disabled.

SFIV selected 1280x720 at 60 Hz. The recorded value is the benchmark overlay's
cumulative `AVERAGE` at the 240-second capture point. It is not the
instantaneous `FPS`, the runner's process-elapsed estimate, or a final result
screen. The process remained in the benchmark after the useful interval and
was terminated by the allowed 30-second post-capture timeout; `result.json`
records status `pass`, no reported failure, and a 270.7-second process elapsed
time.

## Result

| renderer | mode | 240-second overlay average |
|---|---|---:|
| Sikarugir WineD3D/OpenGL | 1280x720 | **`43.80 FPS`** |
| Sikarugir D9VK/MoltenVK | 1280x800 | `46.78 FPS` |
| current dxmt9/Metal | 1280x720 | `44.3 FPS` headline |

D9VK is numerically `6.8%` above WineD3D while rendering a different mode, so
this is not a matched A/B percentage. WineD3D and dxmt9 share the 1280x720
mode, but dxmt9's headline uses renderer frame sampling instead of SFIV's
240-second overlay observer. The durable evidence is the WineD3D baseline
itself and the renderer-specific mode selection, not a sub-percent ordering
between columns.

## Limits

This is one recorded run after one warm-up, not an interleaved trial. The
captured frame is visually complete, but no pixel-matched oracle was used.
Provider identity follows from builtin `d3d9=b`, skipped dxmt9 staging, and the
absence of an app-local D3D9 provider; the empty D9VK/dxmt9 application log is
consistent with that selection.
