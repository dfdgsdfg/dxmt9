---
domain: root
workload: 3DMark05 GT2
title: "3DMark05 GT2 Performance — Current Baseline"
type: root-overview
status: current
updated: 2026-07-19
source: experiments/output/app-d3d9-3dmark05-current-v2-gt2-r{1,2,3}-20260719
related: docs/perfomance/overview.md; docs/perfomance/overview-3dmark05-gt1.md; docs/perfomance/overview-3dmark05-gt3.md
---

# 3DMark05 GT2 Performance — Current Baseline

## Current Result

Three completed current-runtime runs used the V2-only command wire,
`-gt2 -nosplash -nosysteminfo -noscreens`, no Metal frame capture, frame
sampling, frontmost supervision, and the engine-default offload plus
opaque-depth index-cache policy. All three captures show the expected forest
scene, every run completed the GT2 sequence, and all GPU error counters are
zero.

| Metric | Run median | Run range |
|---|---:|---:|
| sampled average FPS | `7.428` | `7.392-7.435` |
| sampled frames | `506` | `504-507` |
| sampled wall time | `68.179s` | `68.116-68.188s` |
| wall p50 | `101.918ms` | `101.561-102.303ms` |
| wall p95 | `337.659ms` | `323.787-340.876ms` |
| GPU CB p50 | `18.947ms` | `18.282-18.988ms` |
| GPU CB p95 | `27.043ms` | `26.862-27.821ms` |
| encoded presents | `507` | `505-508` |

The sampled average varies by only `0.58%` from the slowest to fastest run.
GT2 is therefore reproducible, but its tail is much less regular than GT1 or
GT3: wall p95 is `337.659ms` while GPU CB p95 is `27.043ms`. This numerical
gap prioritizes CPU, synchronization, or phase-specific attribution for the
next GT2 investigation; it does not by itself prove which of those owns the
tail.

## V1 Comparison Status

There is no completed, frame-sampled, same-build V1 GT2 reference. The
2026-07-14 V1-era `at-immediate-gt2` diagnostics timed out after only 480
presents and used a different observation window, so they are not a valid
performance denominator. Treat this three-run V2-only result as the first
defensible GT2 baseline and require a preserved-binary bisect if a V1 delta is
ever needed.

## Measurement Rule

Use per-frame `wall_ms` for GT2. Presents-at-process-exit is invalid when a run
times out or stops in a different phase. Future candidates should repeat three
times, keep the same result/scene selection, and report sampled FPS, wall
p50/p95, GPU CB p50/p95, completion status, capture sanity, and GPU errors.
