---
domain: present-pacing
workload: STALKER Day high late camera corridor
title: "H636 — minimal observation does not separate default Replay and Encode"
type: evidence
status: minimal-population-complete-global-owner-and-removal-open
updated: 2026-10-16
source: traces/stkcop-minimal-owner-h636/{contract.md,inputs.json,derivation.json,compare.py,comparison.json,final_audit.py,final-audit.json}; traces/stkcop-minimal-owner-h636/{O0,I0,N0,N1,I1,O1}/{receipt.json,self.bin,analysis.json,progress.jsonl,post-tail.png,catalogue-result.json}; traces/stkcop-minimal-owner-h636/{N0,N1}/{guest.bin,gap.bin.196}
related: present-pacing-guest-live.635.md; present-pacing-owner-union-live.629.md; present-pacing-replay-cache-live.618.md
---

# H636 — retain both worker candidates, not an invented winner

Both original-game episodes have overlapping Replay and Encode CPU bounds.
The fixed six-run population therefore does not identify a unique default
worker maximum. In both actively observed episodes, the PE-root union upper
bound is below both worker lower bounds. Neither result establishes a global
maximum across auxiliary/driver threads or a positive removable CPU amount.

## Frozen population and qualification

Order is O0, I0, N0, N1, I1, O1, with no replacement or extension. O uses the
original game without guest markers; I uses the private game with inert
markers; N activates guest CPU markers and the PE-root union. All arms use
the same H628 diagnostic PE and H621 A Unix images and H584 sparse worker
endpoints. Detailed H618/H613/H566/H579 observers remain off. O is thus a
default-policy observation, not a zero-instrumentation shipping binary.

Sikarugir, Day/high, 1280x720, maxthreads2, snd_targets16, product perf off,
draw command limit0 and production-counter occlusion0 are fixed. Guest/root
observation arms at camera knot0; comparison uses knots1–6. Camera equality
does not imply identical AI, draw count, geometry or GPU work.

All six runs reach seven active camera knots and natural game exit0. The
catalogue's sole predeclared `missing_capture` failure reflects capture-delay600;
each run has a separately inspected post-tail screenshot. These show no
obvious defect, not pixel-exact equivalence. No builds, parsers or UI actions
occur in the selected corridor. All 14 shared staging files are restored per
run, temporary clock services recovered, and no Wine processes remain.

The final audit reconciles 42 raw camera endpoints and rejects 24 sparse-stream
corruptions. Original game bytes remain unchanged. H628 baseline PE32 matches
the current canonical build; the measured diagnostic PE is a different image.
H635 used a different staged image population, so its absolute costs must not
be combined with these values.

## Worker CPU and same-episode Primary composition

CPU milliseconds per completed camera-episode frame. Bounds enclose endpoint
uncertainty; they are not confidence intervals. Parallel thread CPU must not
be summed as frame latency. FPS is selected-episode FPS, not whole-Day FPS.

| Run | Frames | Replay CPU/frame | Encode CPU/frame | Episode FPS |
|---|---:|---:|---:|---:|
| O0 | 108 | 30.671–36.124 | 29.036–34.596 | 15.820–15.827 |
| I0 | 93 | 38.893–39.160 | 33.625–34.355 | 13.574–13.582 |
| N0 | 90 | 38.324–38.588 | 33.602–33.994 | 13.336–13.342 |
| N1 | 107 | 31.592–31.807 | 31.143–31.521 | 15.855–15.862 |
| I1 | 116 | 31.493–31.669 | 31.376–31.788 | 16.701–16.707 |
| O1 | 115 | 31.551–31.697 | 31.420–31.802 | 17.150–17.157 |

O0's first controller request takes 569.067ms. Its wide CPU enclosure retains
this cold delay; the run is neither discarded nor replaced. O1's much tighter
bounds still overlap. N0's Replay lead is at least4.330ms/frame; N1's is only
0.071ms/frame. Observed N ordering does not establish original O ordering.

| Run | Primary CPU/frame | PE root CPU upper/frame | Outside-root CPU lower/frame |
|---|---:|---:|---:|
| N0 | 66.666–67.872 | 21.844 | 44.822 |
| N1 | 56.451–57.115 | 17.838 | 38.613 |

The guest and root streams share qualified native PID, birth and TID. Their
same-window composition is valid; subtracting H629/H635 or O-run values is
not. Outside-root work includes game, Wine, observer work and coverage gaps;
it is not a measured pure-game CPU region.

Finite all-pair contrasts, milliseconds/frame:

| Contrast | Replay | Encode |
|---|---:|---:|
| I minus O | −4.632 to +8.489 | −3.220 to +5.319 |
| N minus I | −7.567 to +7.095 | −3.212 to +2.618 |
| N minus O | −4.532 to +7.917 | −3.453 to +4.958 |

These include workload, layout and order differences. N minus I also combines
guest clocks and root observation. None is a calibrated observer tax to
subtract from a phase or a statistical estimate of treatment effect.

## Consequence

The minimal-observer population closes collection and finite control reporting,
not the original default-owner/removal objective. Preserve Replay and Encode
as competing large worker costs. Repeating wild populations without a new
discriminant is not the next test. Select a concrete redundant semantic-work
mechanism, establish same-input correctness and CPU reduction, then qualify
its transfer to default live execution. Previously rejected H619/H626/H627
copy/reuse screens remain rejected; this population supplies no reopening
evidence. No product change, promotion, commit or FPS gain is claimed.
