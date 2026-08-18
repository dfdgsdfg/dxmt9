---
domain: state-churn-encode
workload: 3DMark05 GT2
subcategory: append-decomposition
order: 21
title: Deciding Clean-Host ABBA — The Cadence Win Is Real: Median +2.0%, Non-Overlapping Distributions
date: 2026-08-18
type: experiment-run
status: accepted-attribution
source: experiments/output/app-d3d9-3dmark05--dab-base-1..3; experiments/output/app-d3d9-3dmark05--dab-cand-1..3
related: docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.20.md
---

# Deciding Clean-Host ABBA — The Cadence Win Is Real: Median +2.0%, Non-Overlapping Distributions

**Setup.** The deciding experiment [.20](state-churn-encode-append-decomposition.20.md)
called for: host cleaned (17 GiB freed; the storage daemons that burned ~2
cores in [.19] are gone from the process list), six supervised GT2 runs in
A-B-B-A-A-B order, no decimation, no xctrace, frame sampling only, single
build. Base = default cadence (64 records / 256 KiB), candidate = 4x
(256 / 1.25 MiB).

**Result.** Every run passed with zero GPU errors.

| run | mean fps | median fps |
|---|---|---|
| base-1/2/3 | 27.47 / 27.51 / 27.46 (spread 0.17%) | 27.28 / 27.37 / 27.29 |
| cand-1/2/3 | 27.68 / 27.67 / 27.40 | **27.97 / 27.99 / 27.61** |

The median distributions do not overlap: the worst candidate median (27.61)
beats the best base median (27.37). Median delta **+0.55 fps = +2.0%**; mean
delta +0.37% (the candidate carries slightly heavier tail frames, diluting
the mean). Re-reading [.18]'s original ABBA with this lens shows the same
signature was already present — candidate medians 27.08/26.92 against base
26.64/26.75 (+1.2%) — and was mis-called "flat" by looking only at means.
The steady-state frame body is genuinely ~2% faster; the [.20] per-thread
measurement (−1.14 ms/frame of game-thread CPU) is the mechanism.

**Verdict.** `DXMT9_PE_CHUNK_MAX_RECORDS=256` + `DXMT9_PE_CHUNK_MAX_BYTES=1310720`
graduates from diagnostic to **promotion candidate** for the GT2-class
producer path: reproducible median +1-2% across two days and two protocols,
mean +0.4%, mechanism attributed (fewer, larger chunk seals: 44→8
seals/present, ~1.2 ms/present of game-thread CPU returned). Promotion still
requires the standard evidence: visual gates against the v0.0.3 anchor,
locality conservation (chunk granularity is a behavioural contract — CB/pass
shape and encode pacing must be shown unchanged or bounded), tail-frame
attribution for the mean dilution, and GT1/GT3/SFIV coverage. Until that
bundle exists the default cadence stays 64/256 KiB and the knob remains an
env opt-in.
