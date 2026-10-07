---
domain: present-pacing
workload: STALKER Day high, correctness-only OFF/ON tail image sequence
title: "Tail samples retain gross geometry, but screenshot timing cannot qualify frame correspondence"
type: evidence
status: sampled-visual-no-obvious-loss-camera-qualification-indeterminate
updated: 2026-10-08
source: traces/stkcop-tail-visual-h469/{contract.md,run.py,analyze.py,summary.json,review.json}; traces/stkcop-tail-visual-h469/{dxmt9-r1,dxmt9-visibility-r1}/{receipt.json,executed-runner.py}; traces/stkcop-tail-visual-h469/pairs-{1,2,3,4,5}.png; experiments/output/app-d3d9-stkcop-bench-tail-visual-h469-{dxmt9-r1,dxmt9-visibility-r1}/result.json
related: present-pacing-visibility-budget.468.md; present-pacing-positiont-pixel-center.466.md
---

# No obvious sampled building loss; temporal qualification remains open

One correctness-only OFF run and one ON run complete normally using H468's
frozen renderer images and Day/high/1280x720 settings. Each retains 13 requested
tail images, at scene seconds 100 through 112, plus immediate external
camera/frame snapshots before and after each capture. Both runs restore all
five shared Wine renderer files. No CPU or FPS value from these intentionally
perturbed runs is used as performance evidence.

The first supervisor attempt fails before game launch because the generated
inner execution scope omits `images`. The scope is fixed; prelaunch artifacts
and successful restoration remain in the `*-bootstrap-failure` directories.
This is not a rendering failure or a replacement for a negative game result.

## Observed coverage and uncertainty

| Policy | Actual scene coverage, seconds | Maximum capture duration | Maximum midpoint gap |
|---|---:|---:|---:|
| OFF | 100.018–112.630 | 0.685 s | 1.028 s |
| ON | 100.015–112.806 | 0.798 s | 1.059 s |

The 102–110 s target is bracketed in both policies. All 13 ordinal matches are
monotone. However, the conservative before/after camera envelopes span
3.52–6.39 m and 5.07–8.03 degrees, outside the existing 2 m / 3 degree matching
requirement. Camera movement during a screenshot operation is substantial;
the request label or its midpoint does not identify the actual rendered GPU
frame. Coverage-duration/gap checks were added during collection, before
cross-policy image review; they do not repair this correspondence failure.

All 26 tail samples are inspected through the five retained contact sheets.
Both sequences show the facade, entrance, building corner, floors, columns and
later interior geometry. No obvious ON-only whole-building loss or black frame
is seen in these samples. Different perspective and animation remain visible.

This is a limited visual observation, not a pixel oracle: approximately 1 Hz
samples cannot exclude missing geometry or popping between captures. The
camera gate reports `false` and scene qualification remains **indeterminate**.
Do not relax the envelope or run another identical screenshot pair to call it
qualified. H466's deterministic edge oracle remains separately valid.

## Consequence

This closes the finite screenshot experiment, not the whole-scene correctness
gate. H468 independently fails its performance repeatability requirement, so
even a visual pass would not certify that experiment's removable CPU budget.
The largest default dxmt9-attributable CPU owner also remains unresolved.
No default promotion, README update or new renderer modification follows.
