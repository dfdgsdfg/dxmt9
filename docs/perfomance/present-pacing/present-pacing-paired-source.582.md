---
domain: present-pacing
workload: STALKER Day/high 1280x720 late-window paired eligible sources
title: "Paired live sources repeat a Clear-fusion CPU contrast"
type: evidence
status: mixed-policy-cpu-screen-passed-default-transfer-open
updated: 2026-10-16
source: traces/stkcop-paired-source-h582/{contract.md,live-contract.md,policy.hpp,policy_spec.cpp,observer.hpp,build.py,build.json,preflight.json,default-preflight.json,launch_guard.py,run.py,inputs.json,seeds.json,clocks.json,analyze_policy.py,analyze_run.py,estimate.py,test_estimator.py,estimator-tests.json,verify_results.py,raw-verification.json,comparison.json}; traces/stkcop-paired-source-h582/{S0,T0,T1,S1}/{receipt.json,analysis.json,paired-analysis.json,self.bin,policy.bin,post-tail.png,restore/restored.json,recovered.json}
related: present-pacing-source-shape.581.md; present-pacing-current-clear-self.573.md; present-pacing-current-clear-live.574.md; present-pacing-clear-default-route.578.md
---

# Clear fusion has a repeated within-run CPU effect

H582 completes the four-run population proposed by H581. Both treatment runs
pass the frozen finite CPU screen. This closes the paired **mixed-policy**
contrast, not observer-free default CPU removal, all-ON FPS, or the global
single-thread owner goal. No production path is promoted by this experiment.

## Image and correctness

One private image starts from H572 B: thin H514 self-CPU observers, H558's
failure-safe Clear candidate and H570's shader repair. Only the draw-encoder
archive member changes. There is no root-wall, worker-body, foreign-callback
or H581 shape observer in this timing image. H533 supplies only Present
boundaries for Primary. Shared images are restored after each run.

Eligibility is the immutable original legacy Slot having both draws and
Clears. Consecutive eligible sources form pairs; intervening ineligible sources
do not advance the ordinal. A frozen random seed selects one position per
pair. Ineligible sources stay OFF. The policy capability checks queue owner,
source sequence, slot index, generation, storage and page range. No command
ordering, queue waits, flushes or source boundaries are changed.

Five original-capture controls pass exact source/output BGRX hashes, seven
source mappings, query transcript and 670/670 wrapper lifetime accounting.
OFF/eligible-ON/sham/paired/inverse produce 0/65/0/0/65 fusions. This capture
has one fusion-bearing source, so it is correctness evidence, not a live
randomization sample. Native policy tests cover 163,840 rows per architecture
(ARM64 and x86_64).

Five real ordinary-queue controls additionally verify two sources, completion
sequence2 and exact color/depth/stencil readback. Fusion totals are 0/2/0/1/1;
paired and inverse select complementary sources. Tape and DCE are both OFF.
This fixture skips draws to isolate attachment actions; the original capture
provides the actual-draw oracle. Metal API validation is enabled only for the
correctness fixture, not live timing. Seven malformed receipts are rejected.

Policy receipt status records scope destruction, not successful encoding by
itself. Analysis also requires successful complete H514 Encode scopes and
normal game completion. Source joins verify the queue-to-lifecycle-controller
binding as well as TID, sequence, index, generation and storage.

## Frozen experiment and result

Order is S0 sham, T0 treatment, T1 treatment, S1 sham. The four seeds, binary,
supervisor and estimator are hash-frozen before S0. Sham assigns the same
kind of random labels but keeps both positions OFF. Treatment enables only
label1. No failed run is replaced and the population is not extended.

All four runs reach seven spatial knots and exit naturally with game code0.
Each restores fourteen shared files and recovers four clock services. The
only catalogue waiver is the predeclared `missing_capture`; a separate
post-tail image exists for every run and shows coherent building geometry.
Those images are visual integration checks, not exact whole-game oracles.

Both full Encode scopes must lie inside the conservative knot1–6 interval.
The primary difference is CPU(label0) minus CPU(label1). The normalized
contrast is `2 * sum(pair differences) / observed frames`. It estimates a
per-eligible-source effect under no interference; it is **not an observed
all-ON reduction** or a direct measurement of the mixed run's total saving.

| Run | Frames | Complete pairs | Mean difference, ms/source | Normalized contrast, ms/frame | Paired fusions |
|---|---:|---:|---:|---:|---:|
| S0, sham | 128 | 63 | +0.127 | +0.125 | 0 |
| T0, treatment | 119 | 58 | +1.333 | +1.300 | 3,505 |
| T1, treatment | 114 | 56 | +1.266 | +1.244 | 3,419 |
| S1, sham | 120 | 59 | −0.145 | −0.143 | 0 |

The frozen screen threshold, including sham endpoint allowance, is
**0.320 ms/frame**. Treatment contrasts after their endpoint allowances are
**0.925** and **1.057 ms/frame**, both above it. Descriptive normal-approximation
95% intervals are 0.998–1.602 and 0.824–1.663 ms/frame; both sham intervals
include zero. A fixed 65,536-draw sign-flip sharp-null test gives approximately
0.0000153 for each treatment versus 0.453/0.532 for sham.

These inferential calculations assume no carryover and fixed selected units.
GPU backlog and treatment-dependent timing/window membership can violate
those assumptions. Do not present the intervals as unconditional causal
bounds. Boundary-unmatched pairs are 2/2/1/1 and are not extrapolated.
An independent raw integer decoder reproduces all 3,007 selected source joins,
236 pair differences, fusion totals and normalized arithmetic.

## Whole-thread CPU and pacing are separate outcomes

| Run | Replay CPU, ms/frame | Encode CPU, ms/frame | Observed late-window FPS |
|---|---:|---:|---:|
| S0 | 28.603–28.978 | 29.562–29.689 | 18.994–19.003 |
| T0 | 29.276–29.671 | 29.526–29.900 | 17.195–17.202 |
| T1 | 29.822–30.172 | 29.573–29.760 | 16.392–16.396 |
| S1 | 30.252–30.621 | 30.674–30.851 | 17.585–17.591 |

The treatments have lower observed pacing than both sham runs. This is not an
FPS success. The fixed mixed-policy design does not isolate all-ON FPS or
attribute the movement to thermal behavior, host activity or the candidate.
Even the two sham runs differ substantially in whole-thread CPU and pacing.

Encode leads Replay in both sham full windows; T0 overlaps, and T1 has a small
Replay lead. The practical result remains two similarly expensive renderer
workers, not a stable Encode-only bottleneck. Primary is 48.470–52.900 ms/frame
across these runs but includes game/Wine; this image does not remeasure the
renderer-only Primary bound from H580/H581. Concurrent CPU scopes must not be
summed into frame latency.

## Decision and next gate

Stop repeating unstratified live A/B runs to decide whether the candidate has
any local CPU effect: H573's captured-input result now has a repeated live
paired counterpart. Keep the supported size explicit: about 1.27–1.33
ms/eligible source in this diagnostic mixed policy, not removal of the whole
Draw-plus-Clear source cost and not an explanation for a twofold backend gap.

The next decision is default transfer, not another broad owner census. A
separately frozen all-OFF/all-ON comparison without the private policy/self
observers is needed to assess whole-run regression and promotion. Preserve the
existing failure/lifetime and original GPU oracles for the exact images used.
If default CPU savings still do not improve pacing, examine the remaining
Replay/Primary supply constraint rather than declaring further Encode work
automatically critical. H582 itself changes only private experiment artifacts
and evidence documents; it does not close the active global-owner goal.
