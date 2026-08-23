---
domain: baselines
workload: 3DMark05 GT1/GT2/GT3 + SFIV
subcategory: wild-fps-refresh
order: 3
title: README FPS Refresh — Single-Run Sweep at 675c2415; GT1 30.9, GT2 29.0, GT3 65.9, SFIV 44.2
date: 2026-08-23
type: experiment-run
status: accepted-baseline
source: experiments/output/app-d3d9-3dmark05-post-pe-boundaries-{gt1,gt2,gt3}-r1-20260823; experiments/output/app-d3d9-sfiv-benchmark-post-pe-boundaries-sfiv-r1-20260823
related: docs/perfomance/baselines/baselines-wild-fps-refresh.02.md
---

# README FPS Refresh — Single-Run Sweep at `675c2415`

**Question.** What are the current absolute wild-workload FPS values after the
PE recorder boundary hardening series? This is a single-run status refresh, not
an A/B and not evidence that the PE changes caused the difference from the
previous README table.

**Method.** One run per workload on a 16 GB MacBook Air with an Apple M1
8-core GPU and Sikarugir-CX 24.0.7 Wine. The canonical PE and unix-provider
builds were rebuilt together before the sweep. All runs used the `perf` profile,
`DXMT9_PERF_FRAME_SAMPLING=1`, no gputrace, encoder breakdown off, and
frontmost-window retention. Primary FPS is `positive frames * 1000 / sum of
positive wall_ms`. The steady cross-check is `1000 / median(wall_ms)` after
discarding frames before ordinal 30 and frames above 200 ms. The 3DMark scenes
ran to natural completion under a 120-second supervisor. SFIV used its
catalogue-defined 50-second observation timeout; that timeout is an accepted
completion disposition for this experiment.

## Result

| workload | sampled frames | sampled avg | steady median | CB/present | passes/present | status / GPU errors |
|---|---:|---:|---:|---:|---:|---|
| GT1 | 3,285 | **30.91** | 31.86 | 4.000 | 10.716 | pass / 0 |
| GT2 | 1,883 | **29.04** | 31.45 | 3.999 | 15.787 | pass / 0 |
| GT3 | 5,138 | **65.86** | 71.05 | 4.000 | 14.700 | pass / 0 |
| SFIV | 2,893 | **44.22** | 59.84 | 3.940 | 22.924 | pass / 0 |

Against the immediately preceding 2026-08-21 refresh, each delta is within or
at the edge of the host's approximately ±3% single-run ambient band, so this
sweep does not establish a new causal performance gain.

All four logs were free of GPU command-buffer errors and the captured frame from
each run was visually normal. The screenshot check is a smoke oracle only; it
does not replace a deterministic pixel comparison.

**Limitations.** These are one-run absolute values with no ABBA pairing. SFIV's
50-second observation is shorter than the prior refresh's 90-second matched
window, so the SFIV averages are useful as current smoke/performance status but
not as a precise cross-refresh delta. The steady median remains close to the
long-standing 60 FPS body while its sampled average remains sensitive to
pacing and hitch tails.
