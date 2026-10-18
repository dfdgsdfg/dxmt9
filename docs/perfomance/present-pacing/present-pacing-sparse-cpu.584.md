---
domain: present-pacing
workload: STALKER Day/high 1280x720 late-window whole-policy CPU
title: "Sparse whole-thread CPU: Clear lowers Encode contrasts, not a repeated saving beyond drift"
type: evidence
status: amended-identity-qualified-cpu-transfer-inconclusive
updated: 2026-10-16
source: traces/stkcop-sparse-cpu-h584/{contract.md,live-contract.md,build.json,native-verification.json,bounds-tests.json,preflight.json,inputs.json,qualification-amendment.md,selected_identity.py,identity-tests.json,analyze_amended.py,compare.py,comparison.json,verify_results.py,raw-verification.json}; traces/stkcop-sparse-cpu-h584/{O0,N0,O1,N1,O2}/{receipt.json,self.bin,analysis.json,post-tail.png,restore/restored.json}; traces/stkcop-current-clear-h572/build.json
related: present-pacing-unobserved-transfer.583.md; present-pacing-paired-source.582.md; present-pacing-self-counter-boundary.513.md
---

# Replay and Encode are near peers; no repeated whole-policy saving is certified

H584 closes the five-process sparse CPU population without replacements.
Both ON Encode contrasts are lower than both adjacent OFFs, but neither
worst-adjacent reduction exceeds the OFF-to-OFF variation screen. Replay
and Encode remain near peers, rather than one permanently dominant thread.
This is not evidence that Clear fusion has no local benefit: H582's conditional
source contrast and H573's exact-input saving remain separate results.

## Mechanism and correctness qualification

All five processes load the same H584 A image, derived from observer-free
H572 D. Only three objects change for queue identity binding and existing
Replay/ordinary Encode boundary polls. H549_ENABLE is process-wide OFF/ON;
there is no mixed source policy. Canonical PE, default queue, Day/high1280x720,
max-threads2 and snd_targets16 are fixed. No H514 per-source clocks, PE
observer, clock service, fusion witness or product perf/frame diagnostics
are active. The external scene helper remains in both arms.

Each worker reads its own libproc CPU counter once per requested epoch at an
existing boundary. Epoch1/2 are requested after spatial knots1/6. Between
requests there are cached-enabled and epoch checks, not timers or row writes.
The requests never wake or wait for a renderer thread. This reduces diagnostic
activity; it does not prove zero observer overhead or observer-free CPU transfer.

CPU bounds include the full scene-observation-to-sample delay and H513's fixed
2us self-counter allowance. Observed delays range from 6.863 to 27.863ms;
self-counter calls take 2.584–27.333us. The resulting CPU interval widths are
0.128–0.328ms/frame. These are endpoint bounds, not statistical confidence
intervals. No external stale thread-counter reads are used.

Qualification includes 6,154,240 finite CPU-trajectory/bound checks, four invalid
bound inputs, ARM64/Rosetta epoch schedules, actual external-controller tests,
and eight invalid receipt cases. Native steady polling does not create rows.
Eight exact-image correctness controls cover observer OFF/ON crossed with
candidate OFF/ON for both the original captured input and ordinary queue.
The original seven sources preserve exact source/output BGRX hashes, query
transcript and all 670 wrapper lifetimes. The real ordinary queue completes
sequence2 with exact color/depth/stencil readbacks under API validation.
The latter skips draws to isolate attachment actions; the former covers draws.

Two build-generation assertion failures are preserved with their resumers:
a nested lambda was incorrectly counted as the final function return, and
Replay has six success/failure completion sites, not three. They occurred
before live use and do not represent renderer failures.

## Explicit identity amendment

The original live contract required one process-lifetime binding. O0's original
analyzer failed at this check, before calculating or displaying CPU/FPS.
Identity-only inspection found two bindings, while all four sampled endpoints
belonged to one queue/controller/domain tuple. The unused binding is not
evidence of another measured owner; its creation cause was not investigated.

The original contract and failing analyzer remain intact. Before N0 and before
CPU-value inspection, a separate amendment and analyzer were frozen. They
require both stages and epochs to join the same selected binding, while allowing
other unsampled bindings. Two cross-queue counterexamples are rejected in
addition to positive single/extra-binding fixtures. Numerical estimators and
comparison screens are unchanged. No run is replaced or added.

These results use **amended identity qualification**, not a pass of the original
primary contract. The independent raw verification records this distinction.

## Selected late interval

Fixed order is O0,N0,O1,N1,O2. All runs reach seven active spatial knots, exit
normally, restore fourteen shared files and produce coherent post-tail images.
Only the predeclared catalogue missing_capture waiver applies. A screenshot
inspection is not exact whole-game pixel equivalence.

| Run | Clear policy | Frames | Replay CPU ms/frame | Encode CPU ms/frame | Selected FPS |
|---|---|---:|---:|---:|---:|
| O0 | OFF | 125 | 28.514–28.648 | 28.876–29.158 | 18.602–18.609 |
| N0 | ON | 123 | 29.426–29.553 | 28.369–28.697 | 18.250–18.258 |
| O1 | OFF | 120 | 29.426–29.577 | 29.405–29.649 | 17.057–17.063 |
| N1 | ON | 125 | 29.893–30.027 | 28.844–29.056 | 18.347–18.354 |
| O2 | OFF | 116 | 30.418–30.587 | 30.652–30.851 | 16.687–16.692 |

Thread CPU overlaps in wall time. Do not add the two columns to obtain frame
latency, or interpret either as an exclusive renderer critical-path fraction.
Driver work executing on these threads is included. Primary and other workers
are not simultaneously sampled here; prior bounds are separate episodes.

## All directional comparisons

Positive values below mean lower ON Encode CPU. Both adjacent OFF comparisons
are retained, without linear-drift subtraction or treating frames as trials.

| Contrast | Encode reduction ms/frame |
|---|---:|
| O0 minus N0 | 0.179–0.789 |
| O1 minus N0 | 0.708–1.280 |
| O1 minus N1 | 0.349–0.805 |
| O2 minus N1 | 1.596–2.008 |

The Encode OFF-to-OFF variation threshold is **1.446ms/frame**. The two
worst-adjacent lower reductions, **0.179/0.349**, do not exceed it. The finite
Encode nonregression screen passes, but repeated improvement does not. The
comparison artifact's `cpu_removal_measured` indicates that CPU contrasts
were measured; it does not certify a repeatable removable budget.

Replay's variation threshold is 1.161ms/frame. N0 is 0.778–1.039ms/frame
above O0 and overlaps O1. N1 is 0.317–0.602 above O1 but 0.391–0.693 below O2.
Replay therefore fails its finite 3% nonregression screen on one contrast.
Do not label these differences as policy-caused cost displacement: semantic
work per observed frame and execution conditions can vary, and were not
independently matched by this measurement.

Encode is distinguishably larger in O0/O2, while O1 overlaps. Both ONs have
larger Replay CPU. The predeclared repeated-OFF-owner criterion is not met.
An Encode-only optimization cannot be assumed to remove an equivalent amount
from total CPU supply latency.

Pacing passes the whole-window finite 3% nonregression screen against all
adjacent OFFs, but not repeated improvement: the OFF variation threshold is
4.891ms/frame. Exploratory subintervals are mixed and some fail nonregression;
the whole-window result must not be generalized to every part of the tail.
No thermal/host-load causal explanation follows from the observed drift.

Independent raw verification recomputes 30 frame/wall intervals, ten CPU
bounds and all primary directional screens without calling the endpoint
estimator. It checks selected queue identity, identical staged images,
70 restoration hashes and no remaining Wine/game processes.

## Decision and remaining scope

- Close this population. Do not add more broad FPS runs to seek a preferred sign.
- Retain Clear fusion as qualified local CPU evidence, not a certified
  repeatable all-ON CPU/FPS saving. No production promotion is made.
- Treat Replay and Encode as comparable approximately 29–31ms/frame worker
  costs, not a fixed Encode-only global maximum. This does not establish
  simultaneous all-thread ownership or exclude game/Primary limits.
- The next useful attribution must hold semantic work fixed: use existing
  captured source identities to separate Replay-to-Slot work from Encode body
  work, and identify a removable operation shared across repeated inputs.
  First review existing attribution; do not automatically add another observer
  or restart a live census. Validate whole-stage reduction before another wild
  transfer attempt. A source census fraction alone is not removable CPU.

The largest-default-single-thread/removable-budget goal remains open. This
experiment narrows it by rejecting a permanent Encode-only interpretation and
by placing explicit limits on the Clear candidate's whole-policy evidence.
