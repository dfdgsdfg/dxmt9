---
domain: present-pacing
workload: STALKER H502 r5 original captured input
title: "H617 — captured Replay cache components have overlapping CPU bounds"
type: experiment
status: bounded-attribution
updated: 2026-10-16
source: traces/stkcop-replay-cache-h617/{contract.md,baseline-note.md,build.json,runs.json,analysis.json,verification.json,native-tests.json,extra-tests.json,detail-tests.json,source-verified.json}; traces/stkcop-replay-phases-h616/decision.md
related: present-pacing-replay-owner-live.615.md; present-pacing-replay-operations.594.md; present-pacing-replay-cpu-owner.596.md; present-pacing-owned-run-local.600.md
---

# H617 — cache attribution, not a removable live budget

Four fixed original-input executions preserve pixels, query behavior and
resource balances. Shader-layout, uniform and hot-state reconstruction CPU
bounds overlap: no unique dominant cache component is established. Owned-run
construction is larger than those individual components in this capture, but
that is not new removal evidence and does not reopen H597–H600's failed screen.

H616's broader sparse/draw split was stopped before measured execution because
H594/H596 already answered much of that question. Its private build remains
preserved; it supplies no performance result. H617 extends the observer to
cache components instead of repeating that broad comparison.

## Protocol and qualification

Use one private image, the unchanged H502/r5 bundle, and fresh processes in
fixed order O0,D0,D1,O1. O retains coarse scopes and detail scaffolding; D enables
nested wall scopes. Root CPU endpoints bound child CPU conservatively. There
are no per-draw CPU syscalls, optimization treatments, replacement executions,
Wine staging or live game runs.

Four private translation units instrument offload, chunk replay, command queue
and `src/d3d9/core_draw.cpp`. Canonical core source rebuilds byte-identically to
its baseline archive object. Unlike H602's core object, this core does not carry
two dormant private cache/owned-run scopes. Both H617 arms share this choice;
absolute H615/H617 timing is not a treatment comparison. The remaining objects
retain H602 endpoint machinery. No shared class definition or product source
is changed; no H595 clock suppression or H597 constructor treatment is enabled.

Qualification passes 12 native, four extra and six detail-switch controls,
plus 32 parser negatives. Source reversal and archive membership checks pass.
All four executions preserve:

- Original source/output pixel SHA-256
  `1955a90a4d989708bcbd5a82aa2c3dcd425ed6f94170ec55075c6b2778d68e9b`.
- 670 created/released wrappers, seven mapped final sources, nine query issues,
  five polls, four results and five readiness checks.
- Normal-state restoration, input hashes and loaded private image identity.

Each execution contains 260 inline raw roots, including setup/teardown. These
are not seven live FIFO sources. A separate final verifier imports no observer
parser, decodes all 1,040 raw rows, reproduces phase/root arithmetic and checks
3,988 file hashes. No performance execution is added by verification.

## Timing and observation burden

All values below are milliseconds per complete captured execution, **not per
live frame**. Inclusive parent and nested child costs must not be added.

| Run | Root CPU bounds | Inclusive record-loop wall |
|---|---:|---:|
| O0 | 60.491–61.898 | 43.225 |
| D0 | 62.447–63.834 | 44.782 |
| D1 | 61.433–62.829 | 43.874 |
| O1 | 60.198–61.586 | 43.556 |

Finite detail-minus-coarse contrasts are −0.465..+3.637ms root CPU and
+0.318..+1.557ms loop wall. Detail changes timing; do not subtract a universal
observer tax or treat O as an uninstrumented shipping image.

Disjoint exclusive regions:

| Region | D0 wall | D0 CPU bounds | D1 wall | D1 CPU bounds |
|---|---:|---:|---:|---:|
| Record-loop remainder | 12.533 | 11.051–12.533 | 12.545 | 11.104–12.545 |
| Queue batch | 16.896 | 16.438–16.896 | 16.208 | 15.805–16.208 |
| Planned routing remainder | 11.738 | 10.365–11.738 | 12.049 | 10.651–12.049 |
| Sparse dispatch excluding draw | 1.283 | 0.564–1.283 | 1.308 | 0.570–1.308 |
| Draw sink remainder | 4.285 | 3.093–4.285 | 3.953 | 2.734–3.953 |
| Binding snapshot | 0.734 | 0.356–0.734 | 0.719 | 0.346–0.719 |
| Non-draw dispatch | 3.312 | 1.315–1.832 | 3.260 | 1.307–1.887 |
| Cache-hit remainder | 0.321 | 0.155–0.321 | 0.313 | 0.140–0.313 |
| Cache-miss remainder | 0.591 | 0.119–0.591 | 0.493 | 0.110–0.493 |
| Shader layout | 1.150 | 0.575–1.150 | 1.122 | 0.572–1.122 |
| Uniform build/refresh | 1.103 | 0.452–1.103 | 1.109 | 0.457–1.109 |
| Hot-state rebuild | 0.695 | 0.143–0.695 | 0.685 | 0.147–0.685 |
| Owned-run construction | 4.903 | 4.585–4.903 | 4.913 | 4.605–4.913 |
| Compatibility preparation remainder | 0.835 | 0.565–0.835 | 0.802 | 0.528–0.802 |

Both D executions have identical detail counts: 4,299 sparse dispatches,
3,982 draw sinks/binding snapshots, 912 non-draw dispatches, 1,101 cache hits,
779 cache misses, 777 shader-layout rebuilds, 1,112 uniform builds/refreshes,
779 hot-state rebuilds, 2,343 owned-run constructions and 2,828 compatibility
preparations. These describe actual scope calls, not independently disjoint
draw populations.

## Transfer boundary and next discriminant

The cold capture has 2,828 compatibility draws among 3,982 draws. H594's live
population had about 497k compatibility draws among 507k draws. Active Tape/V4
qualification and the different Direct route mix can also contribute to the
captured planning/loop remainder. Neither those residuals nor captured costs
multiplied by historical live frequencies establish default-live CPU costs.

H615 still establishes only the largest observed region within Replay; H613
establishes Encode's draw-loop region in a separate population. Their phase
times cannot be ranked as a matched experiment, and no correctness-qualified
removable live CPU amount is established here.

The next discriminant is source-qualified **live cache-component attribution**
inside compatibility preparation: layout, uniform and hot-state reconstruction,
with owned-run construction as a competing region. Retain whole-thread CPU
endpoints and qualify observer burden. H594/H596 already established broad
compatibility/cache/run ownership; repeating that ranking is not the question.
Determine whether the live route mix changes the component ranking seen here.
Test a distinct representation mechanism only after showing which
initialization/copy is redundant; do not rerun closed clock, scratch, prefix
or owned-construction candidates until they happen to look favorable.
