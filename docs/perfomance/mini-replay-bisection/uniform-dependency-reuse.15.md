---
domain: mini-replay-bisection
workload: 3DMark05 GT2 fixed Render Tape
title: "Known RenderState dependencies: eliminate unchanged fixed-uniform rebuilds"
type: experiment
status: bounded-candidate-default-off
updated: 2026-09-16
source: experiments/output/gt2-uniform-dependency-reuse-20260916-r631.noindex/campaign.json; experiments/output/gt2-uniform-dependency-reuse-20260916-r631.noindex/campaign-complete.json; experiments/output/gt2-uniform-dependency-reuse-20260916-r631.noindex/audit.py; experiments/output/gt2-uniform-dependency-reuse-20260916-r631.noindex/audit.json; experiments/output/gt2-uniform-dependency-reuse-20260916-r631.noindex/frozen-probe-audit.json; experiments/output/gt2-uniform-dependency-reuse-20260916-r631.noindex/native-tests-final.log; experiments/output/gt2-uniform-dependency-reuse-20260916-r631.noindex/native-regressions-2.log; experiments/output/gt2-uniform-dependency-reuse-20260916-r631.noindex/native-candidate-regressions.log; experiments/output/gt2-uniform-dependency-reuse-20260916-r631.noindex/native-oracle-tests-2.log; experiments/output/gt2-uniform-dependency-reuse-20260916-r631.noindex/x86-tests.log; experiments/output/gt2-uniform-dependency-reuse-20260916-r631.noindex/x86-oracle-tests-2.log
related: docs/perfomance/mini-replay-bisection/uniform-dependencies-observer.14.md; docs/perfomance/mini-replay-bisection/replay-cost-owners.11.md; specs/backend/requirements.md; specs/backend/spec.md
---

# Known RenderState dependencies and uniform reuse

## Verdict

The GT2 full-rebuild work was largely **over-invalidation**, not unavoidable
uniform construction. Of 422 binding-agnostic batch full rebuilds per
iteration, all 421 with a prior full payload reproduced identical matrices,
material, lights, texture transforms and clip values. The opt-in exact-value
probe checks floating-point bits, excluding Light padding; it grants no
reuse authority.

The default-off `DXMT9_REPLAY_PRECISE_UNIFORM_INVALIDATION` candidate narrows
only known RenderState setters' nonconstant dependency. Full rebuilds fall
**422 → 1** while CPU semantics, resource/source settlement, final-Slot work
shape and GPU exact original pixels remain unchanged. The batch uniform-build
scope falls about **1.56 → 0.52ms**. Ordinary bounded CPU medians improve
**2.76–3.97%** in opposite orders. This is not an observer-off, asynchronous
service-time or Wine FPS result; promotion remains withheld.

## Mechanism and safety boundary

`src/d3d9/core_draw.cpp` (`makeDrawUniformPayloadFromState`) reads transforms,
material, lights and clip inputs, but among render states only clipping
enable and clip-plane enable affect its fixed values. Other render states
affect hot state/shader decisions, which still invalidate normally.

`src/d3d9/d3d9_uniform_dependencies.hpp`
(`renderStateAffectsNonconstantUniform`) is shared by the known integer/float
setters and native refinement. The change does **not** add a cache, generation,
per-draw equality scan, identity heuristic, allocation or wire field.

The existing witnesses remain mandatory:

- Stable-state, flat-render-state, dedicated shader-layout and aggregate
  uniform generations still advance on every RenderState setter.
- `RS_CLIPPING` and `RS_CLIP_PLANE_ENABLE` still invalidate nonconstant data.
- Generic `mutableState`, even with a RenderState reason, remains broad.
  StateBlock, reset, transform/material/light/clip and rollback remain broad.
- Programmable/FFP clip-coordinate compatibility, complete-payload validity,
  both stages' constant generation and usage bounds still gate reuse.
- No queue order, resource lifetime, Slot identity or completion rule changes.

## Same-image comparison

Base HEAD is `b0a5b221` plus this candidate. Both variants use the same frozen
x86_64/O3 image through Rosetta on Apple M1, ProductionSlot exact/zero headroom,
the same GT2 original and capture-source boundaries. The source-plan digest is
`cc6b8312ea6abfc0a571741faca56aec729d8dac57e5585a3589bce29aac34f6`.
Both GPU variants reproduce original RGBA digest
`d5a01daf5f2fcf66c90e08bf6e9064a5902eda58aa717d321ce4d11ee6701284`.

`DXMT9_PERF_UNIFORM_HASH_OBSERVER_MODE=parent` is common to both variants;
the exact-value dependency probe is **off during timing**. Other observers
remain enabled. Every timed process uses two warmups plus 16 retained
iterations; order is OFF/ON/ON/OFF, repeated separately for diagnostic phase
accounting. No builds or tests run concurrently with these measurements.

| Ordinary CPU measurement | OFF | ON | Change |
|---|---:|---:|---:|
| Forward order, whole-iteration median | 69.292ms | 66.540ms | −3.97% |
| Reversed order, whole-iteration median | 68.554ms | 66.660ms | −2.76% |
| Batch uniform-build scope, forward | 1.562ms | 0.524ms | −1.038ms |
| Batch uniform-build scope, reverse | 1.571ms | 0.521ms | −1.049ms |
| All-path uniform-hash parent, forward | 1.229ms | 0.732ms | −0.497ms |
| All-path uniform-hash parent, reverse | 1.232ms | 0.734ms | −0.497ms |

Scoped elapsed times include warmups and surviving observation; they are
not additive process-CPU ownership. The whole-iteration improvement is not
entirely attributable to the ~1.04ms local scope. OFF repeated medians drift
1.07%, versus 0.18% for ON; neither pair establishes a universal speedup.

Separate diagnostic replay CPU means fall **14.102 → 13.085ms** and
**14.056 → 13.265ms**. Diagnostic whole-iteration medians improve only
0.70%/0.60%, with variable object-setup tails; do not mix these diagnostic
receipts into the ordinary measurement cohort. Setup/admission/mutation and
publication remain outside the replay-local optimization.

## Work conservation and changed work

| Per iteration | OFF | ON |
|---|---:|---:|
| Batch snapshot misses | 514 | 514 |
| Full uniform payload reuse | 4 | 30 |
| Nonconstant payload reuse | 88 | 483 |
| Full uniform rebuild | 422 | 1 |
| All-path uniform build/refresh calls | 883 | 857 |
| Draws / Direct / compatibility | 1,866 / 1,856 / 10 | unchanged |
| Sources submitted/completed/reclaimed | 185 / 185 / 185 | unchanged |
| Leases / SoA growth after reserve | 182 / 1 | unchanged |
| Uniform records | 1,862 | unchanged |
| Fixed / VS / PS components | 184 / 870 / 259 | unchanged |

Thus fewer CPU reconstructions do not mean fewer shader constants or Slot
resources. Parent-only hash counters intentionally suppress byte/reason rows;
their zeros cannot be interpreted as absence of hashing.

## Evidence and residuals

R-BACK-2.15.5 is bound to these layers:

- Native fresh-value tests cover all 256 render-state keys × four values ×
  FFP/programmable modes × regular/singular matrices (4,096 cells). Both
  clipping dependencies have distinguishing values. Signed zero, NaN bits,
  light padding and each probe group have separate pins.
- Exhaustive finite-state refinement covers two cache lanes and all 262,144
  length-six traces (299,592 prefixes) over irrelevant/clipping setters,
  broad mutation, reset, shader-mode change and draws. Omitting either
  clipping key yields a three-step stale-value counterexample. This shared
  predicate binding is not generated whole-program isomorphism or a proof
  of thread interleavings, arbitrary values, driver behavior or pixels.
- Actual Device setters/cache borrows are checked against fresh values,
  fixed-payload and same-domain hot component hashes, shader layout and
  constant values/usage. Incremental stage hashes intentionally use a different
  domain from fresh full/prefix hashes: their bytes/usage and in-snapshot hash
  consistency are checked instead of invalid cross-domain hash equality.
  A/A/B/A, float setters, broad mutation with a RenderState reason,
  StateBlock, reset, FFP/programmable switching and constant mutation pass
  in separate OFF/ON processes on native and x86_64 builds.
- Native focused 6/6, extra regressions 6/6, candidate-enabled regressions
  7/7 and x86 focused/install-name 7/7 pass. Strengthened fresh-hash oracle
  reruns pass 2/2 on each architecture. No new TLA+ model or queue transition
  is introduced; this lane uses the finite-domain alternative in R-VERIF-1.5.
- The independently reauthenticated campaign contains 156 iterations:
  eight CPU verification, four GPU exact, 64 retained ordinary measurements,
  64 retained phase diagnostics and 16 warmups. A further four diagnostic
  iterations bind the dependency counts to the same frozen image.
- CPU/GPU receipts bind environment, input bytes, source plan and loaded
  image hashes. Cross-environment measurement bindings and the public
  policy-pair gate still reject mismatches; this explicitly scoped ablation
  does not bypass them or publish a general policy ranking.

Broader rendered workloads, FFP/clipping pixel transitions, globally
observer-off CPU service, asynchronous pipeline behavior and supervised wild
FPS/correctness qualification remain open. These are the next promotion
gates; finer component generations are not justified by this already-eliminated
GT2 rebuild population. The remaining ~0.73ms hash parent is a new residual
envelope, not proof that all of it can be removed.
