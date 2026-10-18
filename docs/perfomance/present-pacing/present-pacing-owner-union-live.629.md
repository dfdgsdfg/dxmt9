---
domain: present-pacing
workload: STALKER Day default-policy late camera interval
title: "H629 — draw-loop separation using same-episode PE union bounds"
type: experiment
status: bounded-attribution-default-transfer-open
updated: 2026-10-16
source: traces/stkcop-owner-union-live-h629/{contract.md,live-contract.md,derivation.json,composition-preflight.json,process-exit-tests.json,inputs.json,verification.json,final-audit.json}; traces/stkcop-owner-union-live-h629/{O0,N0,N1,O1}/{receipt.json,analysis.json,post-tail.png}; traces/stkcop-owner-union-live-h629/{N0,N1}/joint.json
related: present-pacing-root-union.628.md; present-pacing-owner-composition.621.md; present-pacing-encode-setup-live.623.md; present-pacing-layout-reuse.626.md; present-pacing-override-projection.627.md
---

# H629 — an observed operation winner, not a default bottleneck or saving

Encode's draw loop has a CPU lower bound above every other **individual
observed disjoint partition**, Primary renderer-root upper bound, unassigned
Replay/Encode budget and observed auxiliary-thread bound in both ON episodes.
Its CPU bounds are 18.319–19.811 and 18.435–19.810ms per completed frame.
Primary's tighter union bounds are at most 18.146 and 17.635ms/frame.

This closes H621's overlapping Primary comparison in a new live population.
It does not establish a largest default whole-thread owner, eliminate CPU
work or prove an FPS improvement. OFF Replay/Encode totals overlap in both
episodes, and detailed observation materially raises Replay CPU. No product
source, default-policy, commit or push change is made.

## Frozen population and evidence

O0,N0,N1,O1, one episode each, no replacements or extension. Sikarugir,
Day/high, 1280x720, `-max-threads 2`, `snd_targets=16`, product perf OFF,
command limit0 and production-counter occlusion0. H584 sparse self endpoints
are active in both arms. Only N enables H628 PE root mapping, H618 Replay,
H613 Encode, H566 worker bodies and H579 foreign callbacks.

Both arms use the unchanged H621 Unix observer image, SHA-256
`a10e100abb72f28a7cbeb1ace770d89435375ee6c2e87fde3fbc06a5b56c9ef1`,
and identical H628 diagnostic PE images. The supervisor is mechanically
derived from H621; its Unix package is deliberately not renamed to H629.
Only the PE root package/controller, output paths and provenance gates change.
The sparse worker parser remains distinct from H628's Primary parser.

Preflight authenticates 424 files against actual H621 build/native/GPU/PE
receipts and H628 build/native/COM/clock-identity qualification. Sparse parser
process-exit, missing-witness and eight identity-negative controls are executed
again. No new H629 GPU or native-runtime result is claimed: those gates are
authenticated parent evidence, with H628's actual PE fixtures already using
the selected H621 Unix provider.

PE roots activate at camera knot0. H584 epochs are requested after knots1 and6;
all ownership comparisons use the enclosing knot1-to-knot6 window. All four
runs reach seven active camera knots and exit naturally. Computer-use is
limited to startup focus and post-tail capture; the four saved images were
viewed and show coherent building, actor and texture rendering. This is not
every-frame pixel equality. No concurrent build/parser/workload or UI operation
runs during the selected measurement windows.

The catalogue's only administrative waiver is predeclared `missing_capture`:
its capture delay exceeds the episode, while the supervisor saves post-tail
images. All 56 shared-file restorations and eight temporary-service recoveries
pass hash checks. No Wine process remains. Independent verification re-reads
134,599 Replay/Encode/PE rows, checks phase arithmetic and identity joins,
and reconstructs both original and union PE bounds. The final audit separately
reconstructs candidate rankings and rechecks provenance/restoration.

The scoped provenance audit for this leaf and the updated index/overview
passes, as does `git diff --check`. The default repository-wide provenance
audit still fails on 11 missing historical build-log paths in the unrelated
capacity-ownership recovery, GPU-position invariance and query-default
promotion leaves; those documents and missing artifacts are not modified here.

## Whole-thread CPU and observation burden

CPU milliseconds per completed frame. These are deterministic enclosures,
not confidence intervals. Concurrent thread CPU must not be summed as frame
latency; selected-window FPS is not whole-demo FPS or a README replacement.

| Run | Selected frames | Replay CPU | Encode CPU | Selected-window FPS |
|---|---:|---:|---:|---:|
| O0 | 122 | 29.254–29.395 | 29.204–29.418 | 18.163–18.170 |
| N0 | 113 | 33.520–33.687 | 30.885–31.124 | 16.819–16.824 |
| N1 | 112 | 32.981–33.130 | 30.602–30.886 | 16.002–16.009 |
| O1 | 115 | 30.427–30.592 | 30.456–30.709 | 16.784–16.792 |

Replay and Encode overlap in each OFF episode. Finite N-minus-O cross-pair
contrasts are Replay **+2.389 to +4.433ms/frame** and Encode **−0.107 to
+1.920ms/frame**. OFF envelope widths are 1.338 and 1.505ms/frame respectively.
These ranges include episode variation; they are not an exact observer tax
to subtract from phases. The ON Replay total does not establish the default
throughput bottleneck. OFF FPS also varies despite comparable thread budgets.

## Same-call PE bound improvement

H628 preserves ±100ns per root endpoint and accumulates the union of expanded
intervals. Both original and union totals are recorded in the same ON calls.
Window subtraction uses qualified enclosing checkpoints, not arbitrary pairs.
Waits/callbacks remain inside the renderer-root bound.

| Primary renderer root | N0 ms/frame | N1 ms/frame |
|---|---:|---:|
| Original elapsed-plus-margin upper | 19.585 | 19.038 |
| Expanded-interval union upper | 18.146 | 17.635 |
| Overlapping margin removed from upper | 1.440 | 1.403 |

The last row is reduced overcounting in a proof bound, **not removed CPU**.
No smaller clock uncertainty or off-CPU subtraction is used. The retained
original upper still overlaps the draw-loop bounds in both episodes; the
union is what separates them. Old H621 aggregates remain unchanged and cannot
be retrospectively tightened using these new calls.

## Observed disjoint operation ranking

Lower bounds use wholly contained roots; boundary roots contribute clipped
conservative uppers. These are named partitions, not equally sized or
indivisible operations. Grouping several Replay partitions together can exceed
the loop; that does not contradict this ranking.

| Region | N0 CPU ms/frame | N1 CPU ms/frame |
|---|---:|---:|
| Encode draw loop | 18.319–19.811 | 18.435–19.810 |
| Replay queue batch, excluding nested mark | 7.219–9.664 | 7.068–9.445 |
| Replay owned-run construction | 4.368–6.014 | 4.370–5.987 |
| Replay record-loop remainder | 3.335–5.057 | 3.316–5.024 |
| Encode pass handling | 3.738–5.088 | 3.796–5.108 |
| Encode root remainder | 2.960–4.407 | 2.955–4.319 |
| Replay uniform build/refresh | 1.856–3.617 | 1.860–3.538 |
| Replay unassigned | 1.637–4.401 | 1.688–4.393 |
| Encode unassigned | 0.714–1.202 | 0.517–0.930 |
| Primary renderer root, upper only | ≤18.146 | ≤17.635 |
| Largest observed auxiliary body, upper only | ≤0.739 | ≤0.750 |

The nearest competitor is Primary in both runs. Draw-loop lower minus its
upper is **0.173 / 0.800ms/frame**. No observed foreign callback body overlaps
the selected windows and no foreign request remains pending. Those statements
do not cover every possible driver callback or closure destructor.

## Decision and remaining objective

The measured draw-loop region is a justified optimization investigation target,
not a demonstrated 18–20ms/frame saving. H622/H623 already split its internal
work; H626/H627's decoder/projection candidates failed their frozen economic
screens. Do not reopen them or attribute the whole caller budget to copies.

The next ownership gate should reduce observation to the comparisons still
needed, especially avoiding Replay's expensive detailed partition when testing
transfer toward the sparse/default regime. Freeze that population separately;
do not extend this one or subtract a presumed observer tax. Wrapper/closure
teardown and unobserved callback coverage remain explicit obligations for a
global maximum claim.

Separately, an actual removable amount requires a new concrete redundant
operation, correctness/refinement and original GPU evidence, a fixed captured
CPU screen, then observer-OFF live validation. No positive live removable
amount is qualified yet; this is not proof that the remaining cost is
irreducible. The full default-owner-plus-removal objective remains open.
