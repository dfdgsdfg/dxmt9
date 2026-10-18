---
domain: present-pacing
workload: STALKER Day/high 1280x720 late-window whole-policy comparison
title: "Same-image observer-free Clear fusion passes nonregression, not repeated improvement"
type: evidence
status: finite-nonregression-passed-cpu-transfer-open
updated: 2026-10-16
source: traces/stkcop-unobserved-transfer-h583/{contract.md,preflight.py,preflight.json,launch_guard.py,run.py,inputs.json,analyze_run.py,compare.py,screen-tests.json,comparison.json,verify_results.py,raw-verification.json}; traces/stkcop-unobserved-transfer-h583/{O0,N0,O1,N1,O2}/{receipt.json,analysis.json,post-tail.png,restore/restored.json,recovered.json}; traces/stkcop-clear-crossover-h575/report.json; traces/stkcop-current-clear-h572/build.json
related: present-pacing-paired-source.582.md; present-pacing-current-clear-live.574.md; present-pacing-current-clear-self.573.md
---

# A whole-policy nonregression screen, not a new CPU estimate

H582's mixed-policy source contrast does not prove all-ON behavior. H583 fixes
one additional variable from H574: every OFF and ON process loads the **same
H572 D binary**, with the cached H549_ENABLE decision absent/present. There is
no source-wise policy switching or image-layout difference between arms.

Private renderer observers are absent from this image, verified against the
build receipt and symbols. Canonical PE DLLs are used. No H514 source clocks,
H533 Present clocks, H582 receipt, clock service, runtime fusion witness or
product perf/frame diagnostics are enabled. The external scene helper remains
identical in both arms: this is not an assertion of zero measurement overhead.

## Correctness and fixed population

H575's original canonical-OFF/candidate-OFF/candidate-ON hashes, exact pixels,
query transcript and wrapper lifetime controls are revalidated without reruns.
The unchanged H578 real ordinary-queue fixture is relinked against H572 D.
OFF/ON both pass completion sequence2 and exact color/depth/stencil readback;
fusion counts are 0/2. Metal validation is used only for this fixture. Its
draw-skipping attachment isolation is complemented by H575's actual-draw oracle.

The contract and supervisor are frozen before O0. Five fresh processes run in
order O0,N0,O1,N1,O2, with ON bracketed by OFF. All seven camera knots, active
scene qualification and natural game exit0 pass. Each run restores fourteen
shared files; no clock services are staged. Only the predeclared catalogue
missing_capture waiver applies, and each post-tail image is inspected. No
replacement or extra run is added. The five screens show coherent geometry,
not exact whole-game pixel identity.

## Selected late-window result

Knots1–6 are the primary interval. Endpoint bounds reflect observation timing,
not confidence intervals. The five component intervals are exploratory.

| Run | Policy | Frames | Wall ms/frame | Selected-window FPS |
|---|---|---:|---:|---:|
| O0 | OFF | 122 | 56.947–56.973 | 17.552–17.560 |
| N0 | ON | 120 | 57.052–57.074 | 17.521–17.528 |
| O1 | OFF | 117 | 59.504–59.526 | 16.799–16.806 |
| N1 | ON | 118 | 56.214–56.239 | 17.781–17.789 |
| O2 | OFF | 113 | 59.475–59.500 | 16.807–16.814 |

Both ON runs meet the frozen practical nonregression condition against **both**
adjacent OFFs: ON upper wall/frame <= 1.03 times OFF lower wall/frame.
This is a finite screen, not a general noninferiority guarantee.

Repeated improvement does **not** pass. The OFF-to-OFF noise threshold is
2.579 ms/frame. N0 is 0.078–0.126 ms/frame slower than O0, while it is
2.430–2.474 ms/frame faster than O1. N1 improves by 3.265–3.312 versus O1 and
3.236–3.286 versus O2. Only the second ON clears the noise threshold against
both neighbors. Do not select that pair alone or extend this population.

Independent verification reconstructs all 30 interval frame counts, wall/FPS
bounds, the primary noise threshold and four directional comparisons. It also
checks identical staged image hashes, absent private trace files/environment,
70 restoration hashes and no remaining Wine/game processes.

## What this changes

- H582's repeated conditional source CPU reduction remains valid within its
  scope; H583 neither remeasures it nor converts it to all-ON CPU savings.
- An observer-free, same-image, whole-policy run does not show a >3% regression
  in these two bracketed comparisons. It also does not establish repeated FPS
  improvement. No host/thermal causal attribution follows from the drift.
- H574's failed comparison is not erased or merged with this population.
- Neither this timing screen nor a visually coherent tail authorizes automatic
  production promotion or closes the largest-default-CPU-owner objective.

Do not run another broad FPS population simply to seek a favorable result.
The remaining transfer question is whole-policy **CPU**, including possible
cost displacement to Replay or Primary. A suitable next measurement is sparse
request-qualified self-thread checkpoints at the existing scene boundaries,
without per-source clocks, source policy receipts or PE root entry timing.
Require process/thread identity, self-counter freshness, acknowledged request
generation and enclosing endpoint bounds; an absent response is missing
coverage, not zero CPU. Compare ordinary OFF/ON on one immutable image and
report Replay/Encode separately. Existing foreign/root coverage is scoped
evidence, not a reason to restart a full census or infer a permanent winner.
