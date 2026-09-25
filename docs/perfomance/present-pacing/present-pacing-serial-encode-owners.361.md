---
domain: present-pacing
workload: STALKER CoP Day
title: "H361: Serial encodeDraw owners and conditional service ceiling"
type: evidence
status: partial-attribution
updated: 2026-09-25
source: experiments/output/app-d3d9-stkcop-bench-encode-owner-h361-control/{result.json,daybenchmark.result,scene.png}; experiments/output/app-d3d9-stkcop-bench-encode-owner-h361-diagnostic/{result.json,daybenchmark.result,actual.png}; experiments/output/app-d3d9-stkcop-bench-encode-owner-h361-tail/{result.json,daybenchmark.result,actual.png}; traces/app-d3d9-stkcop-encode-owner-h361/analysis/{analyze.py,costs.json,samples.py,sample-summary.json,middle.sample.txt,late.sample.txt,supply-tail.csv,waits-tail.csv,cpu-tail.json,progress-tail.json,encode_sensitivity.py,encode-sensitivity.json}
related: present-pacing-stalker-draw-phases.341.md; present-pacing-prepared-default.360.md
---

# H361: Serial encodeDraw owners and conditional service ceiling

HEAD `ed88a207`, no runtime changes. Day/high, renderer_r2.5, 1280x720,
Sikarugir cx24.0.7, perf profile, validation OFF. Canonical Unix and both PE
builds complete before measurement; no build/test overlaps a run. The PE build
initially lacked llvm-mingw on PATH; adding the documented toolchain resolved
that failure. Existing `g_ntAllocScanHint` unused-variable warnings remain.

## Qualification

Control and diagnostic load identical five staged artifacts: Unix SHA-256
prefix `7060d7fd61d4f279`, PE32 `47f77df1763c6282`, PE64
`0fd59afe183630bd`. Both runners PASS with zero GPU command-buffer errors.
Control's automatic 90-second image is loading, **not** scene evidence;
the separately inspected `scene.png` is an actual rendered scene. Diagnostic's
150-second image is also a scene. These are screenshot spot checks, not exact
GPU differential proof or continuous visual qualification.

Control has normal perf counters, no extra phase/supply/stack probes.
Diagnostic adds existing draw-phase and draw-issue split timers, per-frame
logging and two external 10-second `sample` sessions (requested 5ms interval).
It produces 511/537 actual stack samples, not the nominal 2000. The requested
2450–2470 supply/wait window is **not reached**: only 2364 Presents are encoded.
Its empty trace files establish no native CPU budget and are retained.

| Run | Encoded Presents | Benchmark frames | Native benchmark average |
|---|---:|---:|---:|
| Control | 2499 | 2410 | 18.815 FPS |
| Diagnostic | 2364 | 2275 | 14.918 FPS, excluded from performance verdict |
| Tail CPU trace | 2404 | 2315 | 10.846 FPS, excluded from performance verdict |

Diagnostic has conspicuous multi-second stalls in the sampling-era windows.
Do not call its FPS change a renderer regression or a calibrated observer cost:
live input differs, source counts differ, and order is not reversed. Control
is one new reference, not a speedup claim versus H355 or README.
The tail retry uses the same five images, normal phase timers (extra splits
OFF), frame logging and bounded supply/wait observation at Presents 2100–2120,
without external sampling. It also PASSes with zero GPU errors and an inspected
scene image. It develops severe late stalls even without external sampling;
therefore the earlier stalls cannot be assigned exclusively to `sample`.
This campaign does not establish stable absolute FPS or isolated observer cost.

## Cost owners

The following whole-process counters are normalized by **encoded Presents**,
including non-benchmark activity. They are not benchmark-frame costs.
Despite their `cpu_ms` spelling, `PerfScope` and `EncodeDrawPhaseTimer` use
`steady_clock` elapsed time, **not native thread CPU**. They include observer
work and descheduling. Parent/child rows overlap and must not be added.

| Scope | Control ms/encoded Present | Fraction of encodeChunk |
|---|---:|---:|
| encodeChunk | 25.100 | 100% |
| encodeDraw | 18.107 | 72.14% |
| Binding packet parent | 3.975 | 15.84% |
| Stream binding aggregate | 3.523 | 14.04% |
| Draw issue | 1.270 | 5.06% |
| Uniform build | 0.899 | 3.58% |

The diagnostic's sequential partition locates the largest regions inside
encodeDraw: remainder 6.350ms/Present (indexed setup/issue and early-return
paths, not pure overhead), argbuf/uniform region 6.024ms (also contains binding
packet construction), setup 3.811ms, stream preparation 1.826ms, vertex bind
1.712ms, base-state texture binding 1.351ms, FFP vertex 0.204ms. Argument-buffer
setup itself is zero in these runs. Region names must not be mistaken for
exclusive named child costs. Measured Metal draw-issue child is only
1.497ms/Present in the intrusive diagnostic; this does **not** bound all Metal
driver cost, which also occurs in binding, pass creation and submission.

## Concrete architecture candidate

`src/dxmt9/dxmt9_draw_encoder_draw.mm::encodeDraw` constructs a complete
`DrawBindingPacketPlan` **before** calling the 128-entry direct-mapped
`cacheDrawBindingPacket`. The cache then hashes and exact-compares that built
packet; a hit cannot avoid its construction. This block is outside
`effectiveSkipBaseStateBind`, although several consumers are gated by it.
Control records 6,349,353 hits and 712,648 misses: **89.91% hits**. That is
evidence of repeated equal results, not proof that any particular draw may
reuse a prior packet without checking its dependencies.

The measured binding parent is 9.934s total: plan 2.952s, cache 3.405s,
texture-record diagnostic scope 0.970s, plus 2.608s uncovered parent work and
observer overhead. Do not label that residual memcpy or removable work.
The cache-hit fraction does not mean 89.91% of the parent can be eliminated.

There is also a concrete instrumentation concern:
`dxmt9_draw_encoder_internal.hpp::cacheDrawBindingPacket` receives a non-null
stats pointer on every draw, so its hash/probe/store clocks execute regardless
of whether the counter publication is wanted. The encoder's `PerfScope` reads
clocks whenever its callback is non-null; disabled global counters alone do
not remove those clocks. `encoderBreakdown` pointer presence also admits the
texture-record loop; pointer presence is not the same as diagnostic enablement.
These are code facts, not a measured no-observer speedup.

The safe follow-up is first cold-observer gating, then a run/state-qualified
immutable binding-plan reuse contract. Keep texture LOD/generation, shader
usage, per-draw stream overrides, attachment dimensions, viewport/scissor and
debug overrides in the dependency proof. Do not simply cache by the address of
the mutable overlaid draw view or enlarge the existing table.

## Conditional ceiling, not FPS prediction

For observed encodeChunk service `E`, component `C` and fractional removal `r`,
the hypothetical service speedup is `E / (E - r*C)`, assuming unchanged other
costs, no replacement overhead and valid nested non-overlapping scope coverage.

| Hypothetical reduction | Encode service speedup from control |
|---|---:|
| Half of all encodeDraw | 1.564x |
| Half of binding-packet parent | 1.086x |
| Entire binding-packet parent | 1.188x |
| Half of draw issue | 1.026x |

Deleting the entire binding parent is an unattainable cost envelope, not an
implementation proposal. Even the half-parent case is a sensitivity, not a
forecast. None is a whole-pipeline FPS bound: producer/replay/query/GPU
dependencies and the next limiting stage remain outside this calculation.

External sampled stacks reinforce this limitation: the encode thread is in
ready-prefix condition-variable wait in 451/511 and 387/537 samples. Only
36/44 samples have encodeDraw ancestry; 16/15 of those include clock/scope
reporting. These are sampled **wall-state** proportions, not native CPU
percentages, and the small perturbed populations cannot rank product subphases
reliably. Opaque Rosetta frames remain unresolved rather than assigned to PE or
driver code. A ready-prefix wait is not by itself a GPU wait diagnosis.

## Next gate

The tail retry supplies native CPU evidence. Its transcript closes with zero
overflow/incomplete writers, the wait trace has zero unknown identities and
zero blocking CV entries. The scene-qualified interior 2102–2118 contains
16 frames over 4122.812ms. No claim about query-54 ownership follows.

| Thread envelope | Native CPU ms/Present | Fraction of one CPU core |
|---|---:|---:|
| App (game/Wine/PE/synchronous Unix together) | 112.604 | 44.39% |
| Replay | 61.626 | 24.29% |
| Encode | 71.127 | 27.60% |

These overlapping thread envelopes are not additive frame costs and have
slightly different endpoints. Completed Encode scopes consume 67.872ms native
CPU / 73.537ms elapsed per Present. Encode gaps attributed to not-yet-published
input total 181.118ms/Present; ready dispatch/tail gaps are 2.518ms/Present.
Replay input queue is empty for 71.79% of the observed interval. This rejects
**sustained Encode-worker saturation in this diagnostic window**, not burst
criticality, query/GPU feedback or saturation in another scene. It does not
identify why the app stops publishing work.

The independent fixed-publication sensitivity reconstructs 904 complete
ordinary Encode sources, checks baseline endpoint equality and preserves
publication arrivals, dispatch gaps and wall-minus-native-CPU. Removing half
of all native Encode CPU advances source completion by median 12.464ms,
p95 19.482ms; the final source advances only 8.028ms over the 5056.002ms source
envelope. Removing all native Encode CPU advances the final source 10.812ms.
These are **latency sensitivities, not predicted FPS**, because producer/query/
GPU feedback is deliberately fixed. Forty-one 1-microsecond CPU>wall clock
quantization cases cap removable CPU at elapsed (41us total); larger inversions
are rejected. No unqualified clock row is silently accepted.

One new non-draw owner must not be hidden by the aggregate result: sources
5130/5133/5139 spend 45.173/44.078/40.763ms native CPU in the framegraph
planning/setup parent. Across all 904 sources that parent totals 151.795ms;
encodeChunk totals 1189.679ms and prefetch 127.043ms. The largest source has
814 commands, 100.697ms outer Encode CPU, of which 49.608ms is encodeChunk.
The marker includes graph construction/optimization and wrapper preparation,
so no individual optimizer is blamed without a finer owner split. Historical
H341's sub-millisecond planning example is not a current universal upper bound.

Prioritize cold diagnostic gating and run-qualified binding-plan reuse, then
price the three large planning spikes separately. Keep immutable state/resource
dependencies and exact output as implementation gates. Do not promote parallel
Encode on the assumption that the worker is continuously CPU-saturated.

Only this evidence leaf and the overview change in the tracked worktree.
No runtime behavior changes, no commit, no TLA/GPU oracle rerun are implied.
