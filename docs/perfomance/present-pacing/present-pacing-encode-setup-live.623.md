---
domain: present-pacing
workload: STALKER Day default-policy late camera interval
title: "H623 — existing base-state reuse works; common setup remains"
type: experiment
status: bounded-attribution
updated: 2026-10-16
source: traces/stkcop-encode-setup-live-h623/{contract.md,live-contract.md,build.json,source-verified.json,native-tests.json,detail-tests.json,preflight-verified.json,process-exit-tests.json,inputs.json,verification.json}; traces/stkcop-encode-setup-live-h623/{O0,N0,N1,O1}/{receipt.json,analysis.json,attribution.json,post-tail.png}
related: present-pacing-encode-live-detail.622.md; present-pacing-owner-composition.621.md; present-pacing-encode-setup.611.md; present-pacing-encode-issue.612.md
---

# H623 — base-state lookup is not repeated on every draw

The conditional base-state lookup/bind block executes on only 18.15% / 18.26%
of observed live draws. Its CPU upper bound is 1.935 / 1.876ms per completed
frame. Actual recorded draw calls, including their wrappers, have upper bounds
1.764 / 1.781ms/frame. Neither is the entire large Encode-loop cost.

Common setup excluding the conditional block remains 1.757–3.135 and
1.829–3.270ms/frame. Its bounds overlap the conditional block; this is not a
separated CPU ranking or a removable budget. Existing base-state reuse should
not be replaced with another cache based on an assumption of per-draw PSO
lookup. No product optimization, default promotion or commit is made.

## Scope and qualification

Private H602-default runtime/provider semantics and canonical PE binaries are
preserved. H622's coarse queue/chunk instrumentation remains; detailed
observation is reduced to three lexical scopes in
`src/dxmt9/dxmt9_draw_encoder_draw.mm`, `encodeDraw`:

- `Setup`: function preparation through the existing setup phase mark.
- `BaseState`: only the setup block guarded by `!effectiveSkipBaseStateBind`,
  nested inside Setup. This name does not mean H622's later BaseState phase.
- `IndexIssue`: the four actual `recordedDraw*` call sites, not H622's broad
  index/issue region. Each scope ends immediately after its recorded call.

The existing 18-field schema is retained with distinct H623 magic; unused
detail fields must be zero. Setup exclusive subtracts BaseState, and the loop
contains Setup inclusive and recorded calls. CPU endpoints stay at roots;
per-scope wall is converted only into conservative CPU bounds using the root
complement. No instantaneous process-counter residual is used.

Source reversals, parent image hashes, exactly three changed runtime archive
members and install names pass. Native tests pass 12 controls, 22 corrupted
trace rejections and four independent detail-switch controls. The initial
derivation rejected three ambiguous textual matches for the base-state guard;
matching the setup-specific progress marker fixed this before compilation.

Original H502 capture OFF/ON and ordinary queue OFF/ON pass. Source/output
digest stays
`1955a90a4d989708bcbd5a82aa2c3dcd425ed6f94170ec55075c6b2778d68e9b`,
with 670 balanced wrappers, seven mapped sources and the 9/5/4/5 query
transcript. Original-input ON records 3,982 setup and recorded-call scopes,
with 754 conditional base-state scopes; OFF has no detailed entries. This
is production-counter query behavior, not actual-Metal occlusion validation.

Xcode 27.0 and Metal 32023.921 are rechecked. Metal debugging qualification
keeps GPU equality before timing. Computer-use is confined to startup focus
and post-tail screenshots. No new rendering-state transition exists, so this
observer requires native observer checks, not a new rendering-policy model.

## Fixed live results

Same-image O0,N0,N1,O1; no replacements. Sikarugir Day/high, 1280x720,
`-max-threads 2`, `snd_targets=16`, product perf OFF, command limit0 and
production-counter occlusion0. All arms have coarse observation; only N has
detail. The Unix SHA-256 is
`5a156c041afa66710360adb077333f806b4ec1c46f87190e1906eb319a92f72e`.

All runs reach seven active camera knots, exit naturally and restore 14 files
each. The only catalogue failure is the predeclared missing capture scheduled
outside the experiment. Four post-tail images were viewed and are coherent;
this is not every-frame exact proof. Independent `verify.py` decodes 47,173
roots, verifies sparse self-CPU arithmetic, identity/thread binding, exclusive
conservation and H623 nesting, and checks 56 restorations. No concurrent
build/parser/workload runs in the selected knot1-to-knot6 intervals.

CPU milliseconds per completed frame; bounds are not confidence intervals.
Parent/child or concurrently executing thread values must not be summed.

| Run | Frames | Replay CPU | Encode CPU | Inclusive loop CPU | Tail FPS |
|---|---:|---:|---:|---:|---:|
| O0 | 116 | 29.809–29.982 | 30.865–31.082 | 18.436–20.144 | 17.260–17.268 |
| N0 | 122 | 29.864–30.001 | 31.360–31.616 | 19.266–20.892 | 17.715–17.721 |
| N1 | 123 | 30.081–30.222 | 31.848–32.048 | 19.483–21.120 | 18.121–18.128 |
| O1 | 113 | 30.207–30.350 | 31.040–31.211 | 18.753–20.023 | 15.908–15.915 |

| Detailed owner | N0 calls | N0 CPU ms/frame | N1 calls | N1 CPU ms/frame |
|---|---:|---:|---:|---:|
| Common setup, exclusive | 492,970 | 1.757–3.135 | 497,686 | 1.829–3.270 |
| Conditional base lookup/bind | 89,463 | 0.633–1.935 | 90,871 | 0.531–1.876 |
| Recorded draws, including wrappers | 492,970 | 0.495–1.764 | 497,686 | 0.467–1.781 |

The conditional block is skipped for 81.85% / 81.74% of draws. Count equality
between setup and recorded calls confirms this population reaches draw
emission; it does not establish equal geometry or state across runs.

Finite N-minus-O Encode contrasts span +0.149 to +1.183ms/frame; the OFF
envelope is 0.346ms/frame. Replay contrasts span −0.486 to +0.413, and loop
contrasts −0.878 to +2.684. Observation remains nonzero; do not subtract a
universal tax from individual phases. H622 and H623 have different binaries
and episodes, so their absolute values are not a paired optimization result.
The final OFF FPS is lower despite lower Encode CPU, reinforcing that these
CPU measurements alone do not establish frame throughput causality.

## Removal candidate and full-objective limits

The next bounded removal candidate is lexical DrawRun-local reuse of
`decodeFixedFunctionVertexLayout`, not another PSO cache or another broad
phase census. `encodeDraw` invokes it before the base-state skip, even for
programmable draws. The H623 object retains an external decoder call at
relocation offset `0x4a90`. This proves the call survives compilation, not
its standalone cost or inlining behavior at unrelated sites.

The function in `src/dxmt9/dxmt9_ffp_shaders.cpp` reads declaration elements
or FVF plus effective stream0 stride. The candidate must compute once within
an immutable DrawRun and reuse only while those inputs are unchanged. Per-draw
binding overrides can change stride; preserve the original decoder there or
qualify the exact overridden input. An absent decoded layout is a valid cached
result, not a miss. Classification by shader kind remains separate. No pointer
or result may outlive the caller's lexical run, and no cross-source identity
cache is warranted. This differs from the rejected scratch allocation trial.

H611 measured this operation as secondary on cold input. It is not being
reclassified as the large missing cost or promised a large gain. A candidate
must demonstrate actual eliminated calls, dependency/refinement correctness,
original GPU equality and a frozen same-input CPU reduction before live
validation. Stop that candidate if its screen fails; do not repeatedly extend
the population. Larger total loop ownership and Primary exclusions remain
separate open obligations. H487/H497/H499/H595/H597–600/H606–608/H619 failed
screens remain closed. The full largest-default-owner/removable-budget goal
is not complete.
