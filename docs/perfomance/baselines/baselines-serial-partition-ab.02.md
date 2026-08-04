---
domain: baselines
workload: 3DMark05 GT1/GT2/GT3 + SFIV
subcategory: serial-partition-ab
order: 2
title: Serial Encode Partition Replay Is Performance-Neutral
date: 2026-08-04
type: experiment-run
status: accepted-baseline
source: experiments/output/app-d3d9-3dmark05-partition-ab-gt1-base-r1-20260804b; experiments/output/app-d3d9-3dmark05-partition-ab-gt1-{base-r2,head-r1,head-r2}-20260804; experiments/output/app-d3d9-3dmark05-partition-ab-gt{2,3}-{base,head}-r{1,2}-20260804; experiments/output/app-d3d9-sfiv-benchmark-partition-ab-sfiv-{base,head}-r{1,2}-20260804; experiments/output/app-d3d9-sfiv-benchmark-partition-ab-sfiv-{base,head}-r3-cool-20260804
related: docs/perfomance/baselines/baselines-wild-fps-refresh.01.md; specs/backend/requirements.md
---

# Serial Encode Partition Replay Is Performance-Neutral

**Question.** Does routing the existing single encode thread through immutable
resolved partitions (`4aa245d8` through `445b568e`) change wild-workload
performance relative to the pre-partition encode-session baseline
`72ced034`?

**Method.** Baseline `72ced034` and HEAD `98541901` were rebuilt from detached
worktrees with the same Apple Clang and llvm-mingw versions and matching
release/O3 Meson options. Runs used Sikarugir-CX 24.0.7, the `perf` profile,
`DXMT9_PERF_FRAME_SAMPLING=1`, no gputrace, and no encoder breakdown. Each
3DMark scene used two runs per lane in baseline/HEAD/HEAD/baseline order with a
15-second equal gap. The primary FPS is the frame-sampled average
(`positive frame count * 1000 / total wall_ms`); the steady cross-check is
`1000 / median(wall_ms)` after excluding frames `< 30` and `wall_ms > 200`.

SFIV's initial four-run sequence exposed strong thermal drift, so the
regression verdict uses an additional reverse-order matched pair with a
120-second cooldown before each lane. SFIV uses the same positive-frame
average and steady-frame median over a matched 90-second window.

## Result

| Workload | Pre-partition FPS | HEAD FPS | Primary delta | Steady delta | Verdict |
|---|---:|---:|---:|---:|---|
| GT1 | `27.327` | `27.211` | `-0.42%` | `+0.09%` | neutral |
| GT2 | `23.539` | `23.801` | `+1.11%` | `-0.14%` | neutral |
| GT3 | `54.767` | `54.705` | `-0.11%` | `+0.14%` | neutral |
| SFIV, cooled pair | `43.884` | `44.178` | `+0.67%` | `+0.13%` | neutral |

The primary and steady metrics either move in opposite directions or stay
within `1.11%`. No workload shows a consistent regression or improvement
attributable to serial partition consumption.

## Current README Numbers

| Workload | HEAD median | HEAD run range |
|---|---:|---:|
| GT1 | `27.2` | `27.09-27.33` |
| GT2 | `23.8` | `23.79-23.81` |
| GT3 | `54.7` | `54.69-54.72` |
| SFIV | `44.2` | `42.56-44.18` |

The SFIV headline is the median of its three HEAD windows. Its wider range is
thermal, not an implementation signal: the initial uninterrupted sequence's
on-screen averages drifted `46.53 -> 42.18 -> 43.01 -> 39.64`, while the cooled
matched pair differed by only `+0.67%` and its frame medians by `+0.13%`.

## Correctness Gate

All 18 measured runs report `status=pass`, zero GPU command-buffer errors, and
zero bridge record rejects. GT1/GT2/GT3 completed naturally. SFIV reached the
expected benchmark/result sequence and was accepted through its catalogue
`allow_timeout` policy. Captured SFIV frames showed the expected scene without
new rendering corruption.

## Verdict And Limit

The serial partition refactor preserves current performance. It establishes a
typed, immutable replay boundary, but it does not raise the performance ceiling
by itself because the same single encode thread still consumes every partition
in order. This is a regression gate, not evidence for parallel Metal encoding.

Confidence is sufficient for a refactor-neutral verdict on this M1 machine,
not for sub-percent ranking: GT scenes have two samples per lane, and SFIV has
one fully cooled matched pair. Re-run with longer equal cooldowns before
claiming a future change smaller than roughly `2%`.
