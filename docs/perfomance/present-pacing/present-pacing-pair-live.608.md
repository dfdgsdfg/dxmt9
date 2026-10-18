---
domain: present-pacing
workload: STALKER Day high 1280x720 max-threads2 snd_targets16 late spatial interval
title: "Exact-prefix shortcut passes effects but does not certify live CPU removal"
type: evidence
status: fixed-live-contrast-complete-removal-unconfirmed
updated: 2026-10-16
source: traces/stkcop-pair-live-h608/{contract.md,live-contract.md,prepare.py,derivation.json,controller-prewarm.json,preflight.json,launch_guard.py,run.py,inputs.json,analyze.py,analyze_run.py,selected_identity.py,bounds.py,test_process_exit.py,process-exit-tests.json,compare.py,screen-tests.json,comparison.json,verify.py,verification.json}; traces/stkcop-pair-live-h608/{O0,N0,O1,N1,O2}/{receipt.json,self.bin,analysis.json,catalogue-result.json,post-tail.png,restore/restored.json}
related: present-pacing-pair-production.607.md; present-pacing-pair-shortcut.606.md; present-pacing-queue-partition.603.md; present-pacing-replay-cpu-owner.596.md; present-pacing-run-milestones.543.md
---

# Local preparation improvement does not establish a default-live removal budget

H608 completes the fixed O0,N0,O1,N1,O2 comparison with one H607 image. The
candidate changes only actual ordinary-batch state comparison. It does not
combine the prior clock-removal, owned-run or Clear candidates. Original
rendering effects are qualified in H607; live post-tail screenshots are coherent
but are not full-scene pixel oracles.

All five runs complete seven active spatial knots, natural game exit0 and
14-file restoration. No run is replaced or added. Only catalogue
`missing_capture` is waived. Product counters, detailed phase observers and
candidate witness logs are OFF. Generation-qualified caller-self CPU endpoints
occur only at knots1/6; the copied controller is prewarmed before the first run.
Computer-use supplies startup focus and post-selection screenshot only.

Unix image SHA-256:
`89dfb5cfd628521fc68577b92ed47db648bbf037e1609965d071d70caeba56c6`.
Canonical PE and default rendering policies remain; this is a private
diagnostic/treatment image, not an observer-free canonical-binary benchmark.

## Same-window worker CPU

Bounds retain endpoint read/dispatch delay and inherited 2us/read allowance.
These are finite accounting bounds, not statistical confidence intervals.
FPS is only the selected late spatial interval, not the whole benchmark score.

| Run | Candidate | Frames | Replay CPU, ms/frame | Encode CPU, ms/frame | Late FPS |
|---|---|---:|---:|---:|---:|
| O0 | OFF | 128 | 28.545–28.661 | 28.973–29.242 | 19.058–19.066 |
| N0 | ON | 120 | 28.851–28.973 | 29.295–29.636 | 17.278–17.284 |
| O1 | OFF | 119 | 30.442–30.616 | 30.590–30.779 | 17.242–17.249 |
| N1 | ON | 122 | 29.726–29.842 | 30.316–30.714 | 18.024–18.030 |
| O2 | OFF | 121 | 30.300–30.430 | 30.707–30.951 | 18.112–18.118 |

Both adjacent OFFs are compared against each ON. Worst lower Replay reductions
are **−0.428ms/frame for N0 and +0.458ms/frame for N1**, below the frozen OFF
variation screen of **2.071ms/frame**. Repeated improvement fails. Both Replay
and Encode pass the finite 3% CPU nonregression screen, but whole-window pacing
nonregression fails. Neither a causal FPS regression nor a benefit is isolated
by those changing baseline values. Do not discard O0, extend the population,
or use N1 alone as the improvement result.

Encode's CPU lower exceeds Replay's upper in O0,N0,N1,O2; O1 overlaps. The three
OFF runs therefore show two Encode-leading windows and one near tie, **not** a
stable Replay-dominant pipeline. Neither worker CPU is removable wholesale;
their overlapping execution times must not be summed as frame latency. This
also does not identify the largest operation inside either worker or price
the PE/game thread on this new image.

## Generation, termination and independent verification

H603's process-exit closure is declared prospectively in this contract. It
accepts an EOF-live diagnostic domain only with same birth identity, natural
exit0, no timeout/kill/failure, complete active knots and process-absence
restoration. It does not certify destructor effects or GPU resource retirement.
Closed/process-exit positive controls and nine terminal/identity negatives pass.

The independent verifier imports neither analyzer nor estimator. It decodes
Bind/Retire and sample rows, checks generation pairing, recomputes CPU bounds,
all six spatial timing groups, comparisons and role ordering, and validates
70 restored file hashes. All checks pass; no Wine process remains. H607's six
effect controls and both native architecture results remain hash-qualified.

## Decision toward the original objective

Stop live repetitions of this small candidate. Its 52–64ns/entry warmed local
effect remains valid, and actual-call-site correctness is stronger than H606,
but no default-live removable ms/frame or FPS gain is certified. No product
promotion, default change or commit occurs.

The next useful attribution target is the **default-live Encode whole draw
loop**, alongside Replay's larger cache/construction remainder. H543's same
original input identified roughly 14ms in the loop versus smaller individual
pass subcomponents, but that is captured-source CPU, not current live ms/frame.
Transfer its run-level boundaries with bounded/source-qualified sampling,
preserving whole-loop versus setup/pass accounting and avoiding per-draw CPU
syscalls. Do not revisit already-small queue reservation or extend broad FPS
tests for the prefix shortcut. The global largest single-thread cost and
correctness-qualified removable budget remain open.
