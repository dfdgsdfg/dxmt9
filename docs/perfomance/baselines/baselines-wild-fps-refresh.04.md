---
domain: baselines
workload: 3DMark05 GT1/GT2/GT3 + SFIV
subcategory: wild-fps-refresh
order: 4
title: README FPS Refresh — Single-Run Current-Cap Sweep at e32da591
date: 2026-08-25
type: experiment-run
status: accepted-baseline
source: experiments/output/app-d3d9-3dmark05-current-cap-gt{1,2,3}-r1-20260825; experiments/output/app-d3d9-sfiv-benchmark-current-cap-sfiv-r1-20260825
related: docs/perfomance/baselines/baselines-wild-fps-refresh.03.md; docs/perfomance/present-pacing/present-pacing-current-bottleneck-pe-symbol.236.md
---

# README FPS Refresh — Single-Run Current-Cap Sweep at `e32da591`

## Question

What is the current absolute throughput and frame shape after the PE recorder,
typed-boundary, sparse-state-plan, and Render Tape lease refactors?

This is a one-run status refresh, not an A/B attribution. Deltas against the
2026-08-23 sweep remain inside the host's approximately `+/-3%` single-run
ambient band.

## Method

One supervised run per workload used Sikarugir-CX 24.0.7 Wine on the same
16 GB Apple M1 MacBook Air. The canonical x86 PE, x64 PE, and x86_64 unix
provider builds were staged together. All runs used the `perf` profile,
`DXMT9_PERF_FRAME_SAMPLING=1`, no gputrace, encoder breakdown off, and the
production identity render-partition provider. The three 3DMark05 scenes ran
to natural completion; SFIV used its catalogue observation window.

Primary FPS is positive frame count divided by positive sampled wall time.
The steady cross-check is the reciprocal of median positive wall time after
discarding startup samples and intervals above 200 ms.

## Result

| workload | sampled frames | sampled average | steady median | wall p50 / p95 | CB / Present | passes / Present |
|---|---:|---:|---:|---:|---:|---:|
| GT1 | `3,256` | **`30.646`** | `31.49` | `31.689 / 49.332ms` | `4.000` | `10.717` |
| GT2 | `1,840` | **`28.311`** | `30.93` | `32.326 / 44.567ms` | `3.999` | `15.778` |
| GT3 | `5,062` | **`64.875`** | `69.91` | `14.325 / 24.509ms` | `4.000` | `14.697` |
| SFIV | `2,859` | **`43.252`** | `59.49` | `16.802 / 51.525ms` | `3.939` | `22.904` |

Every run passed with a normal capture and zero GPU command-buffer errors.
Command-buffer and pass locality remains at the established production shape.

## Interpretation

The current refactors preserve the 2026-08-23 throughput level: GT1 and GT2
remain near 31 and 28 sampled FPS, GT3 remains near 65, and SFIV retains a
near-60-FPS steady body with average loss in its pacing tail. No causal gain or
regression is assigned to the intervening structural work.

The companion GT2 attribution in
[Present-Pacing #236](../present-pacing/present-pacing-current-bottleneck-pe-symbol.236.md)
shows that the current first ceiling is the app/PE producer thread, not Metal
GPU execution. The sweep itself remains the absolute status surface; it does
not replace matched A/B or repeated promotion evidence.
