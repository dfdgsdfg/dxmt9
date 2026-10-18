---
domain: present-pacing
workload: STALKER Day high 1280x720 max-threads2 snd_targets16 late spatial interval
title: "Cache-clock removal has positive CPU contrasts but fails the repeatability screen"
type: evidence
status: fixed-live-comparison-closed-removal-unconfirmed
updated: 2026-10-16
source: traces/stkcop-cache-clock-h595/{contract.md,live-contract.md,gate.hpp,prepare.py,derivation.json,core_draw.cpp,native.cpp,build.py,build.json,preflight.py,preflight.json,run.py,inputs.json,compare.py,screen-tests.json,analyze_run.py,analyze.py,bounds.py,selected_identity.py,comparison.json,verify.py,verification.json}; traces/stkcop-cache-clock-h595/{O0,N0,O1,N1,O2}/{receipt.json,self.bin,analysis.json,post-tail.png,restore/restored.json}
related: present-pacing-replay-operations.594.md; present-pacing-replay-partition.593.md; present-pacing-sparse-cpu.584.md
---

# A real disabled-diagnostic defect, not yet a certified live saving

H595 tests the clock candidate found in [H594](present-pacing-replay-operations.594.md).
Both ON runs have lower Replay CPU than both adjacent OFF runs. However, the
worst-adjacent lower reductions, **0.590 and 0.888ms/frame**, do not exceed
the frozen OFF variation screen of **1.210ms/frame**. Repeated CPU improvement
is therefore **not established**. This is not evidence of zero clock cost.

Do not call the full H594 compatibility-preparation envelope of
11.819/12.700ms/frame removable. Do not extend this five-run population until
a preferred sign or threshold appears. The next priority is attribution of
the large preparation body, not more broad FPS trials for this smaller clock
candidate.

## Exact treatment and controls

The private image starts from H584 A, with Clear OFF and no H593/H594 detailed
phase observers. Only `core_draw.cpp.o` changes. Within
`Device::cachedBaseDrawStateForSubmissionBatch`, 18 existing `PerfScope`
callback sites pass through one keep-clock decision. The baseline retains
callbacks; the candidate suppresses them only when product counters are OFF.
State/cache decisions, resource identity, generations, queue routing and
synchronization remain unchanged. `H595_GATE` is a private process-wide
selector, absent in OFF and set to `1` in ON, not a product runtime knob.

The unchanged core source rebuilt with the canonical recipe is byte-identical
to the original archive member. Archive verification confirms only
`core_draw.cpp.o` differs in the treatment. All five runs use one image; the
unix SHA-256 is
`4f28f35ae628684813ce13e026f8ba8e90d02d0a57ff696f6a12a80909502b6b`.
The statically embedded capture core is updated as well.

ARM64/Rosetta fixtures test all gate states and the extracted actual
`PerfScope` null-callback path. Eight fixed effect controls pass:

- Original H502/r5 with candidate OFF/ON and product counters OFF/ON retains
  exact source/output pixels, seven source boundaries, query transcript,
  670 wrapper lifetimes and normal-state restoration. Enabled cache timing
  counters remain nonzero under the candidate.
- Ordinary queue candidate OFF/ON with sparse observer OFF/ON retains API
  completion/readbacks. This fixture skips draws and is not a live pixel oracle.

The same H584 sparse caller-self CPU mechanism supplies two samples per
Replay/Encode worker. There are no per-draw clocks added by this measurement.
Before live execution the copied controller is invoked without arguments,
expected exit2 with no file access, avoiding first-use executable latency.
Actual acknowledgment delays remain in every bound; none are discarded.

The five frozen runs O0,N0,O1,N1,O2 all pass seven active spatial knots, natural
game exit0 and 14-file restoration. The computer-use skill is used only for
startup focus and post-selection screenshots. Screenshots are coherent, not
whole-scene pixel equality. The only catalogue waiver is `missing_capture`.
The inherited receipt text says `H584-live-contract`; the actual frozen
H595 contract, runner, images and estimator hashes are checked independently.
There are no replacement runs or remaining Wine processes.

## Live CPU and pacing

CPU values are caller-self **ms/completed frame** bounds over knots1–6,
including the endpoint uncertainty and inherited 2us allowance. Concurrent
worker CPU must not be summed as frame latency. FPS is only for this selected
late interval, not a whole-benchmark average.

| Run | Frames | Replay CPU | Encode CPU | Late FPS |
|---|---:|---:|---:|---:|
| O0 | 131 | 28.918–29.030 | 28.974–29.246 | 19.545–19.553 |
| N0 | 126 | 28.202–28.328 | 29.327–29.539 | 18.447–18.455 |
| O1 | 121 | 29.974–30.127 | 29.813–30.011 | 17.587–17.593 |
| N1 | 116 | 28.906–29.086 | 29.616–30.113 | 16.565–16.571 |
| O2 | 114 | 30.719–30.858 | 30.756–31.039 | 15.298–15.303 |

| Replay CPU reduction contrast | Bound, ms/frame |
|---|---:|
| O0 minus N0 | 0.590–0.828 |
| O1 minus N0 | 1.646–1.925 |
| O1 minus N1 | 0.888–1.221 |
| O2 minus N1 | 1.633–1.952 |

All four contrasts are positive, but the predeclared repeated-improvement
condition fails. Replay CPU and Encode CPU each pass the finite 3%
nonregression screen; neither passes its repeated-improvement screen.
Encode worst-adjacent lower reductions are negative, −0.565/−0.300ms/frame.

Scene time increases monotonically in execution order, including OFF runs.
The whole-window pacing screen fails both nonregression and improvement;
adjacent OFF variation is 8.529ms/frame. This does not independently establish
that disabling clocks slows rendering. No causal thermal, host-load or
scheduling explanation was measured. Preserve the drift rather than removing
runs or detrending it after seeing the result.

Replay and Encode bounds overlap in every OFF. Encode exceeds Replay in
both ONs, under this sparse observer. There is still no uniquely established
default-live global CPU-thread owner.

## Independent verification and next gate

`verify.py` imports neither the parser nor estimator. It decodes raw records,
joins process birth and queue/controller/thread identity, recomputes endpoint
bounds and all primary comparison arithmetic, checks eight effect controls
and 70 restoration hashes, and confirms no remaining Wine. All checks pass.
The six finite-screen fixtures pass on the final comparison script too.

The code-level disabled-clock issue is real; a live recurring removal amount
is not yet certified. Keep the private candidate separate from product code.
Return to H594's largest body and separate:

1. Cache hit, uniform refresh and full state/layout rebuild.
2. Identity/admission and retained storage checks.
3. Owned canonical-state/uniform run construction versus flat draw append.

Use operation-local caller-self CPU coverage and explicit measurement bounds
to avoid another whole-run comparison dominated by unrelated drift. Preserve
the final queue-batch boundary and generation/lifetime oracle. Do not substitute
cold capture route proportions for live compatibility frequency, or replace
owned snapshots with unqualified borrowed cache pointers.

No production change, promotion, commit, FPS-gain claim or goal completion
follows. Largest individual default-live CPU operation and correctness-qualified
removable amount remain open.
