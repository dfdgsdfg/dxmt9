---
domain: present-pacing
workload: STALKER CoP Day high max-threads 2 snd_targets 16
title: "L64 wild diagnostics: extra publications and visible geometry loss"
type: experiment
status: promotion-blocked-visible-corruption
updated: 2026-10-06
source: traces/stkcop-l64-h445/{run.py,analyze.py,summary.json}; traces/stkcop-l64-h445/{a,b}/{invocation.json,completion.json,supply.csv,supply-analysis.json,scene.png}; traces/stkcop-l64-h445/b/scene-later.png; experiments/output/app-d3d9-stkcop-bench-l64-h445-{a,b}/result.json
related: docs/perfomance/present-pacing/present-pacing-l64-wild-abba.444.md; docs/perfomance/present-pacing/present-pacing-v4-final-grouping.443.md
---

# L64 is blocked by a visible wild counterexample

Same staged images and game configuration as the two-arm diagnostic pair:
Day/high, 1280x720 windowed, two engine threads, 16 sound targets, limit 0/64.
Perf/frame counters and query-split census are enabled. Bounded CPU supply
traces select encoded Presents 1800–1820. No runtime policy changes beyond
the requested command limit. Both runs exit successfully with engine results;
diagnostic FPS is not performance evidence.

Computer-use restored the actual game windows and captured the scene. The
baseline scene shows buildings/ground. The L64 scene shows extensive absent
ground/buildings and exposed background, still present in a second screenshot
about 28 seconds later. The captures are not matched camera identities and
window restoration timing differs, so this does not independently isolate
L64 from focus/reset interaction. It nevertheless blocks promotion. A static
screenshot does not qualify the full baseline sequence or temporal stability.

## Actual publication effect

| Whole-run observation | Limit 0 | Limit 64 |
|---|---:|---:|
| Submitted Presents | 3,348 | 3,469 |
| DrawLimit publications | 0 | 22,399 |
| QueryEnd publications | 7,808 | 7,761 |
| All ordinary publications | 11,156 | 33,629 |
| Ordinary completion-bearing tail CBs | 11,156 | 33,629 |
| All CB creations | 25,149 | 73,936 |
| Render-pass begins | 101,364 | 122,288 |
| Encoded draws | 9,673,200 | 9,883,877 |
| GPU CB errors | 0 | 0 |

Publication conservation is exact: A = 3,348 Present + 7,808 QueryEnd;
B = 3,469 Present + 7,761 QueryEnd + 22,399 DrawLimit. All-CB creations per
submitted Present are about 7.51/21.31; pass begins are 30.28/35.25.
Different run lengths/camera work and corrupted output prohibit treating
these ratios as same-input causal cost differences. Zero GPU errors does not
establish correct pixels.

## Bounded CPU evidence, not an accepted speedup

Both traces pass existing CPU clock/schema, closed, no-overflow and
no-incomplete-writer checks. With two edge Presents trimmed, both analyzers
accept 16 scene-bearing frames (1802–1818 endpoints), one distinct app,
replay and encode thread each.

| Native thread envelope CPU ms/Present | Limit 0 | Limit 64 |
|---|---:|---:|
| App (game/Wine/PE/synchronous Unix) | 19.388 | 16.120 |
| Replay | 16.877 | 14.172 |
| Encode | 17.315 | 13.862 |

These are observer-inclusive, non-additive envelopes in unmatched scene
windows. Corrupted output invalidates equivalent-work CPU savings. The
fixed-arrival sensitivity projection is retained in the raw analysis but
does not establish a whole-pipeline bound or performance benefit.

## Decision and next gate

Keep production defaults unchanged. H444's small average-FPS signal remains
historical exploratory data and must not be promoted as a valid speedup.
H443's fixed capture GPU gate did not cover this full production sequence.

Next reproduce L64 observer-OFF with the window foreground before scene start,
and repeat the same restoration procedure at limit 0. If the fault follows
L64, capture the earliest divergent interval and reduce DrawLimit publication
into a native/refinement/GPU oracle. Inspect state/resource re-establishment
and attachment preservation across the new split; these are hypotheses, not
an established cause. Do not tune away the missing rendering or proceed with
further performance promotion while this counterexample is open.
