---
domain: present-pacing
workload: STALKER Day high 1280x720, H502 late-scene knots 1..6
title: "Current Clear candidate: live removal and observer transfer remain unqualified"
type: evidence
status: live-transfer-unqualified
updated: 2026-10-16
source: traces/stkcop-current-clear-live-h574/{contract.md,inputs.json,build-receipt.json,preflight-verification.json,verification.json,comparison.json,raw-verification.json,verify_raw.py}; traces/stkcop-current-clear-live-h574/{A0,C0,C1,A1,A2,C2,D1,B1,B2,D2,C3,A3}
related: present-pacing-current-clear-self.573.md; present-pacing-default-owner.571.md; present-pacing-clear-failure-boundary.558.md
---

# Twelve valid episodes do not qualify live savings

H574 completes the predeclared current-image population without retries,
exclusions or extensions. All twelve games exit naturally with code 0, reach
seven spatial knots and restore all 14 shared files. Independent raw arithmetic
passes 108 self-CPU intervals and 72 scene intervals. Nevertheless, all three
primary screens fail: Encode CPU reduction, renderer-observer-OFF pacing
improvement, and ON/OFF practical pacing equivalence. This is an inconclusive
performance comparison, not a rendering failure or proof of zero benefit.

## Frozen comparison

The H572 images are reused unchanged. A/C use the canonical encoder; B/D use
the H558 failure-safe Clear fusion in exactly three runtime members. All four
include the H570 shader-count repair. A/B carry H514/H516 thin Replay/Encode
self checkpoints and H533 Present-boundary Primary checkpoints. C/D use
canonical PE and no private renderer observers or clock services. Product
perf/frame/supply/capture diagnostics are OFF in every arm; the external
spatial helper is common to all arms. OFF is not helper-free execution.

The fixed order is A0, C0, C1, A1, A2, C2, D1, B1, B2, D2, C3, A3. Every run
uses Sikarugir, Day/high 1280x720, max-threads2, snd_targets16 and default
commandlimit0/production-counter queries. The catalogue runner's only failure
is the predeclared `missing_capture`: delay600 exceeds the bounded execution.
Each run instead saves a post-tail screenshot. Startup focus and post-tail
capture are outside the selected interval. No builds, tests, policy changes
or UI input are introduced during the population.

All four original-input GPU/query/lifetime preflights and H573's local raw
verification are prerequisites, not new whole-live-scene pixel oracles.
Inspected A0/C1/D1/B2 post-tail images show coherent building geometry; this
does not establish exact pixels throughout the live scene.

## Full-window measurements

Intervals below are endpoint/accounting bounds, not statistical confidence
intervals. FPS covers knots 1..6, not the complete benchmark. CPU columns are
self CPU per observed completed frame; concurrent threads must not be added.
Primary includes game/Wine/renderer work and is not renderer-exclusive.

| Run | Frames | Scene FPS bounds | Replay ms/frame | Encode ms/frame | Mixed Primary ms/frame |
|---|---:|---:|---:|---:|---:|
| A0 | 127 | 18.637–18.644 | 28.517–28.865 | 28.956–29.187 | 47.836–48.572 |
| C0 | 123 | 18.385–18.392 | — | — | — |
| C1 | 122 | 17.957–17.963 | — | — | — |
| A1 | 120 | 17.531–17.539 | 30.448–30.800 | 31.191–31.561 | 50.422–51.272 |
| A2 | 115 | 16.613–16.620 | 30.223–30.664 | 31.221–31.416 | 51.932–52.859 |
| C2 | 114 | 16.732–16.739 | — | — | — |
| D1 | 124 | 18.161–18.169 | — | — | — |
| B1 | 108 | 14.890–14.895 | 30.663–31.122 | 29.983–30.133 | 53.665–54.593 |
| B2 | 117 | 17.194–17.201 | 30.554–30.891 | 29.578–29.766 | 51.541–52.636 |
| D2 | 115 | 16.558–16.565 | — | — | — |
| C3 | 115 | 16.514–16.521 | — | — | — |
| A3 | 116 | 17.292–17.301 | 31.232–31.535 | 31.882–32.268 | 52.867–53.794 |

## Prespecified screens

Each reduction screen requires **both** comparison lower bounds to exceed
the maximum absolute endpoint difference of its initial A/A control.

| Metric | A/A threshold | Forward reduction | Reverse reduction | Result |
|---|---:|---:|---:|---|
| Encode self CPU | 2.604682 ms/frame | A2−B1: 1.087880–1.432949 | A3−B2: 2.116370–2.690160 | Fail |
| OFF scene wall | 1.319594 ms/frame | C2−D1: 4.680203–4.728426 | C3−D2: 0.135339–0.188113 | Fail |

Replay and mixed Primary secondary screens also fail. None of the five
component intervals passes the Encode screen. OFF knots 2..3 pass their
component screen; the other four do not. That isolated component cannot
replace the predeclared full-window outcome.

The observer control requires the entire ON−OFF wall interval to lie within
±3% of the OFF midpoint. Only three of six full-window pairs pass:

| Pair | ON−OFF ms/frame | Allowed magnitude | Equivalent |
|---|---:|---:|---|
| A0/C0 | −0.754737..−0.712953 | 1.631423 | Yes |
| A1/C1 | 1.327488..1.373012 | 1.670399 | Yes |
| A2/C2 | 0.400168..0.450412 | 1.792639 | Yes |
| B1/D1 | 12.074060..12.122147 | 1.651510 | No |
| B2/D2 | −2.256073..−2.207042 | 1.811418 | No |
| A3/C3 | −2.755926..−2.699547 | 1.816270 | No |

Component controls also fail population-wide equivalence. Sign changes and
large differences do not isolate observer overhead from run-order/host/workload
variation. Before/after host censuses contain other system activity (for
example B1's post-run launchd and BiomeAgent); these snapshots do not locate
that work within the selected window or establish its causal contribution.
Neither thermal throttling nor a particular host process is proven causal.

## Meaning and next gate

Encode exceeds Replay in the full window of all four observed canonical runs;
Replay exceeds Encode in both observed candidate runs. Most component bounds
overlap. This strengthens the measured-worker ranking only: it does not prove
the largest default-path renderer owner, the pipeline critical path, or an
observer-free removable budget. H571's foreign dispatch/closure coverage gaps
remain, and its root/other-worker measurements are not part of H574.

H573's fixed-input source-5 paired-median 2.291 ms saving remains valid in its
own scope. H574 does not transfer it to live ms/frame or FPS, and does not
invalidate the local mechanism. No product promotion or legacy removal follows.

Do not extend this population until a preferred result appears. A subsequent
experiment needs a separately frozen design addressing run-order drift and
observer transfer. A within-process source-boundary crossover is a possible
design, not an implemented fix: pending Clear/pass state and cross-source carry
must first be proven independent of the policy switch. Foreign callback and
closure ownership coverage remains a separate prerequisite for the full goal.
