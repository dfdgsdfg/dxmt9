---
domain: present-pacing
workload: 3DMark05 GT2
subcategory: post-defselect-cpu-attribution
order: 04
title: The Decimated Timer Was Measuring Itself — PE Recording Is 16.1%, Not 30.5%
date: 2026-07-29
type: experiment-run
status: accepted-attribution-correction
source: experiments/output/app-d3d9-3dmark05-gt2-pe-calibrated-r1; experiments/output/app-d3d9-3dmark05-gt2-const-calib-r1; experiments/output/app-d3d9-3dmark05-gt2-const-buckets-r1
related: docs/perfomance/present-pacing/present-pacing-post-defselect-cpu-attribution.03.md; docs/perfomance/const-upload/const-upload-sparse-records.01.md
---

# The Decimated Timer Was Measuring Itself — PE Recording Is 16.1%, Not 30.5%

**Question / hypothesis.**
[attribution.03](present-pacing-post-defselect-cpu-attribution.03.md) put
`touchConstShadow` at `~0.22 us` per call. That is implausible for a call that
averages `2.97` registers: three 16-byte `memcmp`s cannot cost that. Either the
function has large fixed entry cost, or the instrument does. Which?

**Method.** Two additions to `DXMT9_PE_STATS_DECIMATION`, both diagnostic-only
and behind the same gate:

1. **Per-call-count buckets** for the const-setter scope. If cost is flat across
   `count`, fixed overhead dominates; if it slopes, the per-element loop does.
2. **A shared instrument-cost calibration.** Every sampling site times one empty
   region with the identical clock pair. Its mean is what the instrument costs
   per sample, and `raw_mean - null_mean` is the scope's real cost.

The second is what the first made necessary. N-variation cannot expose this
bias: the extrapolation `sampled_ms * N / presents` reduces to
`events * (true + bias) / presents` and is **independent of N**, which is why
attribution.03's `N=64` and `N=16` agreeing within `3.2%` proved stability and
not accuracy.

**Result — the buckets.** Cost does not scale with work at all:

| registers in call | events / present | ns / call |
|---|---:|---:|
| `1` | `2,617` | `183` |
| `3-4` | `18,917` | `202` |

Three to four times the work for `10%` more time. Then the calibration explained
why: **the instrument costs `186 ns` per sample.** The const-setter scope's real
cost is `202 - 186 = 17 ns`, not `202`.

**Result — all four scopes, calibrated.** GT2, `53.30 ms` frame:

| Scope | events / present | raw ns | **corrected ns** | raw ms | **corrected ms** | **% frame** |
|---|---:|---:|---:|---:|---:|---:|
| `appendRecordDirect` | `2,705` | `3,037` | `2,851` | `8.21` | **`7.71`** | **`14.5%`** |
| `touchConstShadow` | `21,544` | `203` | `17` | `4.38` | `0.37` | `0.7%` |
| `flushConstShadow` | `10,199` | `192` | `6` | `1.95` | `0.06` | `0.1%` |
| `buildDrawPrimitivePacket` | `1,693` | `450` | `264` | `0.76` | `0.45` | `0.8%` |
| **total** | | | | `15.31` | **`8.59`** | **`16.1%`** |

**Verdict.** dxmt9's PE-side recording is **`16.1%`** of GT2's critical thread,
not the `30.5%` attribution.03 reported, and **`90%` of it is
`appendRecordDirect` alone** at `2,851 ns` per call over `2,705` calls. The
other three scopes together are `1.6%` of the frame.

The bias is not uniform, which is why this could not be corrected by a scale
factor: it is `186 ns` per *sample* regardless of scope, so it is `6%` of
`appendRecordDirect`'s reading and **`92%`** of `touchConstShadow`'s. The tool
degrades exactly where per-call cost approaches clock cost.

**What this retires.** attribution.03's claim that the constant path is `13%` of
the frame and the next target was entirely instrument bias — calibrated, it is
`0.8%`. That also explains why
[const-upload-sparse-records.01](../const-upload/const-upload-sparse-records.01.md)
measured a null for `DXMT9_SPLIT_SPARSE_CONST_RECORDS`: there was never more
than `0.8%` of frame there to win. Its `84.7%`-redundant-setter figure was also
misread as an opportunity; `touchConstShadow` already compares every register
against the shadow and skips unchanged ones, so that number was the existing
filter working, not work being wasted.

**The real target is `appendRecordDirect`**: `14.5%` of the frame at `2.85 us`
per call. Nothing else in the PE recorder is worth looking at until that is
understood.

**Scope.** One GT2 run per configuration. The calibration measures the clock
pair's cost, not any other systematic error; a scope whose body is dominated by
cache effects the empty region does not reproduce would still read high.

**Related.**
[attribution.03](present-pacing-post-defselect-cpu-attribution.03.md) ·
[const-upload-sparse-records.01](../const-upload/const-upload-sparse-records.01.md) ·
[present-pacing](index.md)
