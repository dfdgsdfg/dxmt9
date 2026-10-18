---
domain: present-pacing
workload: STALKER Day high 1280x720 max-threads2 snd_targets16 late spatial interval
title: "H613 — full-root census attributes 97% of late Encode CPU"
type: experiment
status: bounded-attribution
updated: 2026-10-16
source: traces/stkcop-encode-census-h613/contract.md; traces/stkcop-encode-census-h613/comparison.json; traces/stkcop-encode-census-h613/verification.json; traces/stkcop-encode-census-h613/preflight-verified.json; traces/stkcop-encode-census-h613/{O0,N0,N1,O1}/analysis.json; traces/stkcop-encode-census-h613/{N0,N1}/attribution.json
related: docs/perfomance/present-pacing/present-pacing-encode-owner.609.md; docs/perfomance/present-pacing/present-pacing-encode-issue.612.md; docs/perfomance/present-pacing/present-pacing-queue-partition.603.md
---

# H613 — full-root late Encode census

The draw loop is the largest measured **Encode** region in both complete
late-episode root populations. Root attribution covers97.15–97.43% of selected
Encode CPU, closing H609's0.8–2.1% direct sample-coverage gap. The loop owns
60.11–64.59% of whole-thread CPU in the observed interior windows.

This is a stronger ownership result, not a demonstrated removable budget.
Replay and Encode still consume similar whole-thread CPU/frame with observation
OFF. Do not claim that Encode is the permanent pipeline bottleneck or that its
whole loop can be removed.

## Protocol

Keep H602 default archives and H609 coarse scopes. The private binary changes
only queue and chunk encoder objects, selecting every root into a bounded
32768-row buffer. No per-draw detail scopes, candidate policies, product edits
or persistent Wine staging. CPU endpoints are at encodeFn entry/exit. Children
use wall clocks with conservative complementary-wall CPU bounds. The root
excludes dequeue and post-encode queue bookkeeping.

Fixed same-image order O0,N0,N1,O1: O disables the census; N selects all roots.
All runs retain H602 generation-qualified Replay/Encode endpoints at spatial
knots1/6. Settings are Day/high/1280x720, max-threads2, snd_targets16, Clear OFF,
product perf OFF, detailed H602 counters OFF. Source numbering and process
identity are not shared between runs. No replacement or extra repetition.

Four original/ordinary preflight controls pass, including exact original
source/output SHA, seven sources, query transcript and670 balanced wrappers.
Native checks cover12 controls,22 corrupt traces and process-exit closure with
nine identity/missing-witness negatives. Runtime image SHA-256 is
`c425a78129a92fb7cfd5c155940cf834f1f372491f96f43618d741702b617c26`.

All live runs complete seven active camera knots, naturally exit0 and restore
14 files each. Post-tail images are visually coherent, not exact live pixel
oracles. Only the predeclared catalogue `missing_capture` failure is waived.
No build, test, profiler or UI is executed inside the selected interval.
Startup focus and post-tail screenshots are outside it. Independent verification
reconstructs23,547 raw rows, checks56 restoration hashes and confirms no Wine
process remains.

## Whole-thread observer contrast

CPU intervals are milliseconds per completed camera-episode frame. They are
not additive frame-time components because Replay and Encode can overlap.

| Run | Frames | Replay CPU/frame | Encode CPU/frame | Episode FPS |
|---|---:|---:|---:|---:|
| O0 | 116 | 29.545–29.694 | 29.667–29.808 | 17.392–17.400 |
| N0 | 118 | 30.471–30.633 | 30.888–31.198 | 16.696–16.703 |
| N1 | 121 | 29.980–30.097 | 30.514–30.638 | 17.594–17.600 |
| O1 | 124 | 30.132–30.273 | 30.068–30.294 | 18.681–18.688 |

All-pair finite N-minus-O Encode contrasts are +0.220–1.531ms/frame; the OFF
envelope spans0.626ms/frame. Observation measurably changes CPU, so no zero-cost
observer claim is justified. Replay contrasts span −0.293 to +1.087ms/frame;
camera time/frame spans −0.680 to +6.385ms. Neither is a clean performance win.
Do not subtract the Encode contrast as a universal tax from individual phases.

The predeclared screen rejects default-cost transfer if both ON upper bounds
exceed both OFF upper bounds by more than5%. That rejection does not trigger.
This modest finite screen is not statistical significance, exact calibration,
or proof that phase costs are identical when observation is disabled. The
large within-Encode ranking is supported by both censuses; precise unobserved
per-phase cost remains uncertain.

## Complete root population

Join process birth, controller, queue lifetime domain and tid. Use the common
interior window `[first.after,last.before]`; count every overlapping root and
report boundary straddlers rather than silently clipping them.

| Quantity | N0 | N1 |
|---|---:|---:|
| Complete roots across entire run | 11704 | 11843 |
| Roots fully inside selected window | 746 | 742 |
| Boundary-straddling roots | 0 | 0 |
| Root CPU coverage of selected Encode CPU | 97.147–97.402% | 97.181–97.432% |
| Whole interior Encode CPU, ms | 3651.830–3651.851 | 3699.403–3699.414 |
| Uncovered Encode CPU, ms | 94.867–104.177 | 95.009–104.289 |

All roots are selected, published and completed; losses, faults and orphan
scopes are zero. The uncovered CPU includes outside-root queue work and
root-observer overhead. It is not idle wall time. No sampling multiplier is
used anywhere.

Disjoint CPU totals in those interior windows, milliseconds:

| Owner | N0 | N1 |
|---|---:|---:|
| Draw loop | 2194.978–2355.762 | 2225.828–2389.290 |
| Pass preparation | 440.625–596.009 | 449.314–607.358 |
| encodeFn outside chunk | 338.179–504.209 | 338.976–507.764 |
| Chunk remainder | 31.617–127.577 | 31.619–126.879 |
| Uniform/setup | 4.325–70.601 | 5.809–71.400 |
| DrawRun remainder | 0.540–33.339 | 1.145–34.651 |
| Hazard | 0.087–16.786 | 0.218–16.112 |
| UP preparation | 0–6.996 | 0.068–7.111 |
| Binding setup | 0–5.102 | 0.030–4.660 |

The loop lower bound exceeds every other individual upper, including uncovered
thread CPU, in both populations. Its lower bound also exceeds half the entire
Encode CPU upper bound. This is not merely a large wall region dominated by
sleeping. It includes outer draw preparation and encodeDraw, not pure driver
calls; H610–H612 remain useful captured-path decomposition, not live phase
percentages.

Independent conversion to camera frames explicitly subtracts possible
interior overhang and adds missing camera-boundary wall. It yields observed
loop CPU bounds18.542–20.214ms/frame (N0) and18.336–19.811ms/frame (N1).
Pass bounds are3.674–5.301 and3.654–5.084ms/frame. Do not obtain these by
blindly dividing a partial sample by frame count or treat them as savings.

## Remaining completion gap

Within Encode, ownership is now well localized. Across the full dxmt9 pipeline,
Replay remains comparable: OFF Replay/Encode intervals overlap in both runs.
Historical H603 also leaves a large Replay region outside queue submission.
Its numbers must not be combined numerically with this different population.

The next global-owner gate is the same source-qualified census on Replay's
outside-queue source processing, separating required semantic execution from
repeated projection/materialization. This can rule out a larger competing
operation before selecting a removal. Any subsequent candidate must preserve
original GPU/query/lifetime effects and reduce source-matched CPU under a frozen
comparison. A19ms loop is an opportunity boundary, not19ms of removable work.
No optimization is promoted; the full goal remains open.
