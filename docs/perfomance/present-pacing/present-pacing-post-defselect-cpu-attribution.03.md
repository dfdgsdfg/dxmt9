---
domain: present-pacing
workload: 3DMark05 GT2
subcategory: post-defselect-cpu-attribution
order: 03
title: dxmt9 Owns 30.5% Of GT2's Critical Thread, And Half Of That Is The Constant Path
date: 2026-07-29
type: experiment-run
status: superseded-by-attribution.04
source: experiments/output/app-d3d9-3dmark05-gt2-pe-decim-64; experiments/output/app-d3d9-3dmark05-gt2-pe-decim-16
related: docs/perfomance/present-pacing/present-pacing-post-defselect-cpu-attribution.02.md; docs/perfomance/const-upload/overview.md
---

# dxmt9 Owns 30.5% Of GT2's Critical Thread, And Half Of That Is The Constant Path

> **Superseded on 2026-07-29 by [attribution.04](present-pacing-post-defselect-cpu-attribution.04.md).** Every figure below carries an uncalibrated `186 ns`-per-sample instrument bias. Calibrated, PE recording is `16.1%` of the frame, not `30.5%`, and the constant path is `0.8%`, not `13%`. The relative ranking of `appendRecordDirect` survives; nothing else here does.

**Question / hypothesis.**
[attribution.02](present-pacing-post-defselect-cpu-attribution.02.md) established
that GT2's producer thread never blocks — `24,935` of `24,936` samples Running —
so the residual is producer CPU. But the producer thread is the *caller* thread:
the D3D9 application calls in on it, and dxmt9's PE-side chunk recording runs
there too. attribution.01's `16.3%` figure covered only dxmt9's *separate*
threads. How much of the critical thread is dxmt9?

**Method.** `DXMT9_PE_STATS_DECIMATION` with `DXMT_LOG_LEVEL=info` on GT2, at
both `N=64` and `N=16`, the two values
`agents/rules/environment_variables_bridge.rules.md` records as verified
low-perturbation. Per-scope cost is `sampled_ms * N / presents`, per that rule.

**Result.** Per present, over `1,140` presents:

| Scope | `N=64` | `N=16` | events / present |
|---|---:|---:|---:|
| `appendRecordDirect` | `8.58 ms` | `8.24 ms` | `2,706` |
| `touchConstShadow` | `4.76 ms` | `4.63 ms` | **`21,570`** |
| `flushConstShadow` | `2.16 ms` | `2.10 ms` | `10,205` |
| `buildDrawPrimitivePacket` | `0.79 ms` | `0.81 ms` | `1,694` |
| **total** | **`16.29 ms`** | **`15.78 ms`** | |

The two decimation rates agree within `3.2%`, and neither perturbs throughput:
`18.73` and `18.56` fps against `18.58` for a plain `perf` run.

**Verdict.** At a `53.38 ms` frame, dxmt9's PE-side recording is **`30.5%` of
GT2's critical thread**. attribution.01's `16.3%` was half the picture — it
measured dxmt9's own threads and could not see the larger share sitting inside
the caller thread that actually sets frame time.

Frame composition after the codegen fix:

| | ms | share |
|---|---:|---:|
| dxmt9 PE recording (in the producer thread) | `16.29` | `30.5%` |
| GPU | `9.71` | `18.2%` |
| application CPU and everything else | `~27.4` | `~51%` |

**Where the cost is.** Two shapes, not one.

`appendRecordDirect` is the single largest scope at `8.58 ms`, and it is
cost-per-call: `2,706` calls per present at `~3.2 us` each.

The constant path is the opposite shape — cheap per call, enormous call count.
`touchConstShadow` runs **`21,570` times per present** at `~0.22 us`, and
`flushConstShadow` a further `10,205`. Together the constant scopes are
`6.92 ms`, `13%` of the frame, over `31,775` events per present. Whether those
calls carry distinct values or are dominated by redundant setter traffic is not
answered here and is the obvious next question;
`DXMT9_PERF_VS_CONST_SETTER_RANGE` exists to aggregate the app's actual setter
ranges against the flushed records.

**Scope.** One GT2 run per decimation rate. The estimator is a sampled
extrapolation, not a full trace, which is what keeps it non-perturbing; the
agreement between `N=64` and `N=16` is the evidence that the extrapolation is
stable, not a proof of absolute accuracy.

**Related.**
[attribution.01](present-pacing-post-defselect-cpu-attribution.01.md) ·
[attribution.02](present-pacing-post-defselect-cpu-attribution.02.md) ·
[const-upload](../const-upload/index.md) · [present-pacing](index.md)
