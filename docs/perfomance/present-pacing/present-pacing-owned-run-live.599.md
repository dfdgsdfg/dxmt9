---
domain: present-pacing
workload: STALKER Day high 1280x720 max-threads2 snd_targets16 late spatial interval
title: "Owned-run construction lowers Replay contrasts but does not clear the fixed variation screen"
type: evidence
status: fixed-live-comparison-closed-removal-unconfirmed
updated: 2026-10-16
source: traces/stkcop-owned-run-live-h599/{contract.md,live-contract.md,derivation.json,run.py,launch_guard.py,inputs.json,preflight.json,analyze_run.py,analyze.py,bounds.py,selected_identity.py,compare.py,screen-tests.json,comparison.json,verify.py,verify-derivation.json,verification.json}; traces/stkcop-owned-run-live-h599/{O0,N0,O1,N1,O2}/{receipt.json,self.bin,analysis.json,post-tail.png,restore/restored.json}; traces/stkcop-owned-run-live-h599/N0/supplemental-startup.png
related: present-pacing-owned-run-construction.597.md; present-pacing-owned-run-warm-replay.598.md; present-pacing-replay-cpu-owner.596.md; present-pacing-cache-clock.595.md
---

# Positive Replay contrasts, no certified removable amount

Both candidate runs have less Replay CPU than both adjacent reference runs.
Their worst-adjacent lower reductions are **1.262 and 0.860ms/frame**. Neither
exceeds the predeclared OFF variation screen of **1.389ms/frame**, so repeated
CPU improvement is not established by this population. This is not evidence
that the removed copies cost nothing.

Whole selected-window pacing passes the finite 3% nonregression screen, not
repeated improvement. Encode CPU fails nonregression. Do not promote the
candidate, add concurrent Replay/Encode CPU into frame latency, or turn the
static reduction from three hot-state copies to one into an FPS claim.

## Treatment and qualification

The unchanged [H597](present-pacing-owned-run-construction.597.md) image selects
reference construction when `H597_GATE` is absent and final-location owned
construction when it is `1`. All five runs use unix SHA-256
`173c18e35cfc3bb1727c7f27c543fed8190a2375c60f98a7c2c83208c5f28aa3`.
Canonical PE images, queue policy, resource ownership, cache clocks and source
ordering remain unchanged. Clear fusion and product counters are OFF. No
H596 phase observer or H595 clock suppression is combined with this candidate.
Remaining zero-fill, layout assignment and helper work are included.

H597's native value/lifetime/allocation-fault tests and eight exact-effect
controls qualify this image. [H598](present-pacing-owned-run-warm-replay.598.md)
failed on reference iteration1 before candidate timing; its same-Device warm
replay results are not substituted for live costs. H599 uses a separately
frozen live population, O0,N0,O1,N1,O2, with no replacements or extensions.

All five runs pass seven active camera knots, natural game exit0, binary
qualification, post-tail screenshots and fourteen-file restoration. Only the
catalogue's `missing_capture` is waived. The runner uses computer-use for
startup focus and post-tail screenshots. One additional read-only N0 startup
progress screenshot was taken before the selected interval; it sent no input.
No builds, tests or UI operations occur within the selected intervals.
Screenshots show coherent rendering, not whole-scene pixel equality.

## CPU and pacing

Two sparse caller-self samples per Replay/Encode worker bound CPU over
camera knots1–6. Values include endpoint acknowledgment uncertainty and the
inherited 2us allowance. Units are **ms/completed frame**, not elapsed stage
time. FPS is restricted to this late interval, not the benchmark average.

| Run | Frames | Replay CPU | Encode CPU | Late FPS |
|---|---:|---:|---:|---:|
| O0 | 124 | 28.435–28.561 | 28.777–29.162 | 17.546–17.553 |
| N0 | 129 | 27.019–27.173 | 29.755–30.057 | 19.166–19.173 |
| O1 | 120 | 29.688–29.823 | 29.650–29.930 | 17.437–17.444 |
| N1 | 115 | 28.703–28.828 | 30.417–30.773 | 17.031–17.038 |
| O2 | 115 | 30.734–30.863 | 30.915–31.073 | 16.672–16.677 |

| Replay reduction contrast | Bounds, ms/frame |
|---|---:|
| O0 minus N0 | 1.262–1.542 |
| O1 minus N0 | 2.515–2.804 |
| O1 minus N1 | 0.860–1.121 |
| O2 minus N1 | 1.906–2.161 |

| Metric | OFF variation | Worst lower reductions N0 / N1 | 3% nonregression | Repeated improvement |
|---|---:|---:|---|---|
| Replay CPU | 1.389 | 1.262 / 0.860 | Pass | Fail |
| Encode CPU | 1.424 | −1.279 / −1.123 | Fail | Fail |
| Whole-window elapsed/frame | 2.654 | 4.795 / −1.390 | Pass | Fail |

The OFF Replay and Encode costs rise in execution order. N0's faster pacing
does not repeat in N1 versus its preceding OFF. No thermal, host-load or
cross-stage causal mechanism is independently established here; retain the
drift rather than detrending or dropping trials. Subinterval pacing screens
also remain available: knots1–2, 2–3, 3–4 and 4–5 fail nonregression. The
whole-window pass is not a claim that every local segment is nonregressing.

Encode exceeds Replay in O0, N0, N1 and O2; the O1 bounds overlap. These two
observed workers do not establish a unique global/default thread maximum.
App-thread and foreign lifecycle-tail coverage is not added by this experiment.

## Verification and decision

`verify.py` imports neither the parser nor estimator. It decodes raw records,
joins process birth, queue/controller and thread identities, independently
recomputes CPU bounds and primary contrasts, rechecks the eight effect controls,
checks seventy restoration hashes and confirms no remaining Wine process.
All checks pass. The six frozen comparison fixtures also passed before launch.
The independent verifier covers primary screens, not every subsidiary pacing
screen produced by `compare.py`.

Explicit source audits for H597/H598/H599 and `git diff --check` pass. The
repository-wide default source audit still fails on eleven missing historical
build logs in three unrelated leaves; those records are not modified here.

Keep the effect-qualified private candidate and all failed/ambiguous evidence.
Do not extend this population to obtain a preferred verdict. The next useful
measurement is a bounded, operation-local constructor comparison on immutable
inputs with equivalent ownership and allocation state, including remaining
zero-fill/layout work; any later live transfer needs a new declared design.
H598 must not be treated as a usable warmed replay harness until its reference
output mismatch is understood.

No production code change, promotion, commit or certified CPU/FPS saving follows.
The largest default-live single-thread owner and a correctness-qualified
removable amount remain unresolved.
