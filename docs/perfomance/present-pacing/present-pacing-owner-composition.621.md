---
domain: present-pacing
workload: STALKER Day default-policy late camera interval
title: "H621 — simultaneous Replay, Encode and Primary ownership bounds"
type: experiment
status: bounded-attribution
updated: 2026-10-16
source: traces/stkcop-owner-composition-h621/{contract.md,live-contract.md,build.json,native-tests.json,preflight-verified.json,pe-fixture-verified.json,process-exit-tests.json,inputs.json,verification.json}; traces/stkcop-owner-composition-h621/{O0,N0,N1,O1}/{receipt.json,analysis.json,post-tail.png}; traces/stkcop-owner-composition-h621/{N0,N1}/joint.json
related: present-pacing-uniform-owner.620.md; present-pacing-replay-cache-live.618.md; present-pacing-encode-census.613.md; present-pacing-foreign-owner-live.580.md; present-pacing-process-freshness.565.md
---

# H621 — close the different-episode comparison, not the removal claim

Replay and Encode are now observed in the same late episode, together with
Primary renderer roots, auxiliary worker bodies and foreign callbacks.
Encode's draw loop is the largest **individual named disjoint operation**
among the observed Replay/Encode partitions in both ON runs: 18.15–19.78ms
CPU per completed frame. Every other individual partition and unassigned
worker-thread budget is smaller. This does not mean the loop is one indivisible
operation, or that an arbitrarily grouped set of Replay operations is smaller.

There is still no global maximum or removable-budget proof. Primary renderer
root upper bounds, 19.20 / 18.88ms/frame, overlap the loop bounds. Thread
wrapper/closure disposal, unobserved driver callbacks and transfer from
observation-ON to the default path remain exclusions. The combined observer
adds a finite +2.416 to +3.788ms/frame Replay contrast. No product optimization,
default-policy change or commit is made.

## Composition and qualification

The private image preserves H618's core/provider archives. Its runtime changes
exactly seven members relative to H618: the command queue gains H566 Finish/
Completion body scopes; two H613 members supply Encode phases; four H566
members observe pipeline/library compilation, archive load/save and presenter
acquire workers. H579's private API object observes foreign callback bodies.
H563 diagnostic PE bytes are identical between arms; only ON maps the root
observer and temporarily stages the H562 clock service.

Parent receipts, original auxiliary members and instrumented archive members
are authenticated. The queue transformation reverses exactly to H618. H566's
authoritative completion receipt is `link-verification.json`, not its retained
incomplete `build.json`; the latter records an old archive-index gate failure.
The initial H621 provenance check stopped on that distinction before compiling,
then used the authenticated completed receipt. Install-name checks pass.

ARM64 and Rosetta x86_64 each pass all 16 independent observer switch
combinations, including concurrent roots and exception unwind. Original H502
capture observer-OFF/ON preserves exact source/output digest
`1955a90a4d989708bcbd5a82aa2c3dcd425ed6f94170ec55075c6b2778d68e9b`,
670 resource wrappers, seven mapped sources and the 9/5/4/5 query transcript.
This is production-counter query semantics, not actual-Metal occlusion proof.
Both ordinary queue controls pass. Capture observation has 260 provider roots
and nine Encode roots; seven mapped sources must not be confused with either.

PE32 and PE64 actual-object/external-activation tests pass with the composed
Unix image. Foreign birth activation is rejected and eight corrupted map cases
are rejected. All five shared images are restored and three temporary services
recovered. Another 2,541 deterministic cases check common-camera window
clipping; sparse-self process-exit and identity-negative controls pass.

Xcode 27.0, Metal 32023.921 and Apple M1 are verified using the Metal debugging
workflow before GPU execution. Computer-use is limited to startup focus and
post-tail screenshots. No parallel workload/build/parser/UI operation occurs
during the selected intervals. No new rendering transition model is needed:
this is observation-only composition, not a semantic optimization.

## Fixed live population

Same-image O0, N0, N1, O1; no replacements. Sikarugir, Day/high, 1280x720,
`-max-threads 2`, `snd_targets=16`, product perf OFF. Default command-limit and
production-counter occlusion policy are retained. H584 self endpoints are on
in both arms; H618/H613/H566/H579 and PE root observation are ON only in N.
PE activation occurs at camera knot0; self epochs are requested after knots1
and6. All owners use the same knot1-to-knot6 camera window, with endpoint
uncertainty retained. Intermediate knots are progress evidence, not additional
whole-thread CPU samples.

All four episodes reach seven active camera knots and exit naturally. Their
post-tail images were inspected and are coherent. All 56 shared-file
restoration checks pass. The catalogue's only waived failure is
`missing_capture`: scheduled catalogue capture was deliberately outside the
run, and the supervisor retains its own post-tail screenshot.

| Run | Frames in selected window | Replay CPU ms/frame | Encode CPU ms/frame | Selected-window FPS |
|---|---:|---:|---:|---:|
| O0 | 124 | 29.132–29.263 | 28.776–29.069 | 18.072–18.080 |
| N0 | 112 | 32.763–32.920 | 30.332–30.546 | 16.621–16.629 |
| N1 | 119 | 32.706–32.872 | 30.428–30.635 | 17.299–17.305 |
| O1 | 123 | 30.165–30.290 | 30.166–30.525 | 18.565–18.572 |

These are interval bounds, not confidence intervals. They are not whole-demo
FPS or README replacements. Thread CPU is concurrent and must not be summed
as frame latency. O0 has narrowly greater Replay CPU; O1 overlaps. There is
no stable separated default whole-thread winner across these two OFF episodes.

Finite ON-minus-OFF cross-pair CPU contrasts are Replay +2.416 to +3.788ms/frame
and Encode -0.193 to +1.859. OFF envelope widths are 1.159 and 1.749ms/frame.
Do not subtract a universal observer tax, infer Encode observation is free,
or interpret ON's larger Replay total as an established default bottleneck.

## Same-episode operation comparison

Disjoint exclusive phase CPU bounds, not wall times. Camera-boundary roots
contribute only conservative clipped upper bounds; lower bounds use wholly
contained roots. Unassigned CPU retains work outside named complete roots.

| Region | N0 ms/frame | N1 ms/frame |
|---|---:|---:|
| Encode draw loop | 18.154–19.515 | 18.316–19.778 |
| Replay queue batch, excluding nested mark | 7.009–9.364 | 7.097–9.530 |
| Replay owned-run construction | 4.335–5.890 | 4.291–5.968 |
| Replay record-loop remainder | 3.301–4.976 | 3.170–4.876 |
| Encode pass handling | 3.767–4.985 | 3.671–4.972 |
| Encode root remainder | 2.983–4.263 | 2.852–4.275 |
| Replay uniform build/refresh | 1.802–3.478 | 1.784–3.463 |
| Replay unassigned | 1.644–4.350 | 1.628–4.332 |
| Encode unassigned | 0.640–1.125 | 0.642–1.005 |
| Primary renderer root, upper only | ≤19.196 | ≤18.879 |
| Largest observed auxiliary body, upper only | ≤0.805 | ≤0.768 |

Primary's bound includes waits, callbacks and a 200ns/root allowance; it is
not a pure CPU measurement. No foreign callback body overlaps the window and
no event request is left pending. This excludes only the observed callback
families, not every driver callback or closure destructor.

`verification.json` independently re-reads 135,665 raw Replay/Encode/PE rows
without importing the production analyzer. It reconstructs phase bounds,
clock offset, enclosing PE roots, active PE thread identity and restoration
hashes. No other observed controller overlaps either selected window. Process
CPU subtraction is never used; H565's freshness counterexample remains binding.

## Decision

The cross-episode comparison gap is closed. Do not restart the rejected fixed
copy, owned-run temporary-copy or broad uniform-reuse trials. Their failed
economic screens are not overturned by a large parent-region budget.

The next useful **optimization target to investigate** is the actual-live
Encode draw loop, now established as larger than any individual Replay
partition in the same population. H610–H612's captured subregion timings are
not a live decomposition and must not be transferred as removal estimates.
Choose a concrete repeated operation inside that loop and compare a
semantics-preserving removal, including preparation costs and GPU exactness.
In parallel conceptually, the remaining ownership-proof task is to tighten
Primary's conservative bound and cover lifecycle tails; simply choosing a
smaller allowance is not evidence. Global default-live dominance and actual
removable CPU remain open.
