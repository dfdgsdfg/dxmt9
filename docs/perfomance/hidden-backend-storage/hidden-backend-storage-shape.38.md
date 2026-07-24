---
domain: hidden-backend-storage
workload: 3DMark05 GT2
subcategory: shape
order: 38
title: GT2 R32F Alpha-Test Draws Are Already at the Index-Locality Floor
date: 2026-07-24
type: measurement
status: rejected-optimization-accepted-bound
source: experiments/output/app-d3d9-3dmark05-gt2-alpha-index-candidate-frame279-r1-20260724/3dmark05-perf-indexed-probe-draws.csv; experiments/output/app-d3d9-3dmark05-gt2-alpha-index-candidate-frame279-r1-20260724/3dmark05-perf-encoders.csv; traces/app-d3d9-3dmark05-gt2-passcoalesce-order-store-frame279-xcode-r1-20260724/analysis/frame279-xcode-dxmt-joined-summary.csv
related: docs/perfomance/overview-3dmark05-gt2.md; docs/perfomance/hidden-backend-storage/index.md; docs/perfomance/hidden-backend-storage/hidden-backend-storage-shape.37.md; docs/perfomance/backend-shape-classifiers/backend-shape-classifiers-alphatest.01.md
---

# GT2 R32F Alpha-Test Draws Are Already at the Index-Locality Floor

## Question

The two dominant frame279 `2048x2048 R32F` passes each contain `229`
alpha-tested indexed draws. The production LRU32 selector deliberately excludes
alpha test. Does extending the selector to this class provide enough additional
VS-invocation reduction to justify a semantic mini replay and Xcode A/B?

## Method

A no-gputrace GT2 scout ran with
`--frame 279 --measure-index-cache-opt-candidate --frame-sampling
--keep-frontmost`. The candidate flag builds and measures an LRU32 triangle
order on the CPU but never submits it to Metal.

The scout used the traditional pass topology, so its two complete R32F rows are
`279/1` and `279/3` instead of the passcoalesced capture's `279/1` and `279/2`.
The rows are otherwise an exact workload-shape match:

| Property, per pass | Value |
|---|---:|
| render target | format `16` (`R32F`), `2048x2048`, alias `0x20000010000003e` |
| depth | format `41` (`D24X8`), `2048x2048` |
| draws / primitives / submitted vertices | `367 / 343,514 / 1,030,542` |
| alpha-test draws / primitives | `229 / 238,694` |
| non-alpha draws / primitives | `138 / 104,820` |
| depth state | enabled, write enabled, `LessEqual` |
| alpha blend / stencil / clip / scissor | off / off / off / off |
| VS / PS / VSOut | `0x2a0219b55dccb965` / `0x8fd974b9f86d546b` / `0xfff` |

The output is not disposable black color. The R32F alias requires a shader-read
view, and the following main-color row binds the same alias at texture stage 0
for `280` indexed draws with `texture_mask=0x1ff`. This is a sampled
shadow/depth input, so primitive order would still require exact semantic proof
if it had material headroom.

## Result

The alpha-test class has no useful LRU32 headroom:

| Alpha-test subset, per pass | Draws | Primitives | Original LRU32 | Candidate LRU32 | Delta | Gate pass |
|---|---:|---:|---:|---:|---:|---:|
| candidate built | `160` | `184,324` | `238,571` | `238,484` | `-87` (`-0.0365%`) | `0` |
| upper-bound rejected | `69` | `54,370` | `146,390` | not built | no theoretical gain | `0` |
| total | `229` | `238,694` | `384,961` | not aggregatable | negligible | `0` |

For all `69` upper-bound rejections, original LRU32 misses already equal the
draw-local unique-index count. Of the `160` built candidates, `157` have zero
delta; only three save `29` misses each. Across both matching R32F passes, the
only measured alpha-test reduction is therefore `174` estimated invocations.

The non-alpha subset is a positive control for the same measurement path:

| Non-alpha subset, per pass | Draws | Original LRU32 | Candidate LRU32 | Delta | Gate pass |
|---|---:|---:|---:|---:|---:|
| opaque depth-writing | `138` | `174,601` | `104,743` | `-69,858` (`-40.01%`) | `125` |

That reproduces the already-active production selector: the capture reports
`125` reordered-cache hits and `13` rejected hits in each dominant R32F pass.
The scout therefore distinguishes the known useful opaque class from the
alpha-test no-op instead of failing to build useful candidates generally.

Per-draw effective locality also explains the captured hardware count. After
the existing opaque reorders, the scout estimates `489,809` LRU32 misses and
`486,697` LRU64 misses per matching R32F pass. Xcode reports `486,280` VS
invocations per captured pass, only `417` (`0.086%`) below the LRU64 estimate.
The draw-local unique-index sum is `477,864`, leaving only `8,416` invocations
between the captured count and that conservative floor. Even eliminating that
entire residual in both passes would remove only `16,832`, or `0.56%`, of the
frame's `3,012,831` VS invocations. Alpha test itself contributes only `33`
LRU64 misses above unique per pass.

## Verdict

Do not extend `shouldOptimizeOpaqueDepthIndexOrder()` to alpha-tested draws for
GT2. The target class is semantically sensitive because it produces a sampled
R32F shadow/depth input, yet its valid candidates save only `0.0365%` LRU32
misses and none clear the existing `10%` gate. A mini replay, unsafe apply
probe, or new `.gputrace` would test risk without a material performance
numerator.

This also closes the idea that the dominant R32F passes contain a large pool of
avoidable hidden VS invocations. Their Xcode invocation count already tracks
the effective LRU64 estimate. Further index-order work on these passes has a
sub-`0.6%` whole-frame upper bound; the remaining large cost is bytes and GPU
work per required transformed vertex, or a higher-level reduction in submitted
scene geometry.

Subsequent alias-liveness work found that the passcoalesced Xcode capture moved
the main texture consumer before its R32F producer because the DAG treated
surface writes and texture reads as unrelated resources. The per-pass
invocation and index-locality measurements above remain valid workload-shape
evidence, but the pre-fix whole-frame ordering is not parity evidence. See
[hidden-backend-storage-shape.39](hidden-backend-storage-shape.39.md).
