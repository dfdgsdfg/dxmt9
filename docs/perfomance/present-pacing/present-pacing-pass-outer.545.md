---
domain: present-pacing
workload: STALKER CoP H502/r5 original Normal input
title: "Outer pass residual selects aspect-only Clear before Draw, not encoder close"
type: evidence
status: local-clear-candidate-identified-removal-open
updated: 2026-10-15
source: traces/stkcop-pass-outer-h545/{contract.md,pass_observer.hpp,dxmt9_draw_encoder_chunk.mm,dxmt9_render_pass_encoder.mm,dxmt9_render_pass_lifecycle.mm,build.py,build.json,build.log,run.py,analyze.py,summary.json,verify.py,verification.json,parser-tests.log,gaps.py,gaps.json,clear_join.py,clear-join.json}; traces/stkcop-pass-outer-h545/{image,A0,B0,B1,A1}-receipt.json
related: present-pacing-pass-owner.544.md; present-pacing-run-milestones.543.md; present-pacing-encode-command-owner.542.md
---

# The next concrete candidate is aspect-only Clear/Draw pass fusion

H544's unassigned pass-entry time is not mostly encoder close, labels or
argument-buffer setup. H545 adds private bounded same-thread spans around
`LifecycleRuntime::endRender`, the full `startRenderPass` lambda, label/group
creation, argument-buffer setup and the nested Metal close calls. All helper
and proof endpoints from H544 remain present. Three objects replace their
frozen H537 A counterparts in private images; no policy or production changes.

## Verification

Fixed image-OFF/A0/B0/B1/A1 order, fresh process/device per execution, no
failed-sample replacement. All five match the original source/output pixels,
seven source maps, 670 balanced wrappers and query obligations. Both B runs
have 80 matching pass-start command indices, 466 additional spans and no
overflow, clock errors or nested draw clocks. Every start/helper/label/argbuf
scope is nested within its same-thread, same-command DrawRun pass phase.
Metal-close scopes nest inside endRender; spans outside DrawRun pass phases
are accounted separately, not silently charged to preparation.

Independent verification checks 160 helper rows, 1,280 CPU/wall differences,
both 466-span populations, original H537 image hashes and all three unchanged
canonical source files. Existing parser tests pass 42/42. The helper retains
H544 record tags to denote its unchanged schema; these are new H545 runs,
not a mixture of prior samples. No new TLA transition applies to this
observer-only copy. No Wine staging, GUI operation or live game run occurs.

## Exclusive source-5 pass-entry partition

Observer-inclusive CPU ms per captured source, not ms/frame or removable CPU:

| Component | B0 | B1 |
|---|---:|---:|
| Preparation/create/configure helper | 3.491 | 3.350 |
| Label and debug-group setup | 0.193 | 0.177 |
| Argument-buffer setup | 0.046 | 0.037 |
| Other work within startRenderPass | 0.803 | 0.818 |
| endRender within DrawRun pass entry | 0.132 | 0.119 |
| Outside startRenderPass and endRender | 2.469 | 2.434 |
| Total DrawRun pass entry | 7.134 | 6.935 |

Full startRenderPass is 4.533/4.382 ms, already represented by the first four
rows. Within it, encoder creation is 2.621/2.571 ms and nested store proof is
0.471/0.373 ms. Do not add these nested scopes to the table.

There are 80 endRender calls inside DrawRun pass entry, but only 14 execute
the actual popDebugGroup/endEncoding block (0.023/0.020 ms). Another 66 closes
outside those pass phases cost 0.211/0.216 ms in total; their nested Metal
close calls cost 0.076/0.068 ms. This excludes encoder close as the owner of
the roughly 2.45 ms remaining within DrawRun preparation.

## Residual localization

`gaps.py` reconstructs disjoint gaps around full start/close spans from the
existing trace, without another run:

| Gap | Count | B0 CPU | B1 CPU |
|---|---:|---:|---:|
| Phase entry to endRender entry | 80 | 2.059 ms | 2.015 ms |
| endRender return to startRenderPass entry | 80 | 0.109 ms | 0.109 ms |
| startRenderPass return to phase exit | 80 | 0.043 ms | 0.049 ms |
| Continuing DrawRuns with neither start nor close | 446 | 0.258 ms | 0.261 ms |

The between-close-and-start gap includes the caller's possible mid-chunk
split work. It is not the large residual. The earlier gap is the useful next
seam, not a reason to optimize driver endEncoding or CB splitting blindly.

H542's authenticated static transcript has 65 nondraw commands, all full-rect
Clears: **63 depth-only and two color-only**. H545's complete 591-command
draw/nondraw order matches that transcript. Joining the earlier gap by command
ordinal selects **2.047/2.002 ms in the 65 DrawRuns immediately after Clear**;
the other 15 gaps total only 0.012/0.013 ms. This is a static-shape candidate
join, not an observed timed `flushPendingClear` call or proof that its full
cost can be eliminated.

Code supplies a concrete mechanism. In
`dxmt9_draw_encoder_chunk.mm::encodeDrawRunCommand`, a pending Clear is fused
only when `clearKey == drawKey` and the cleared attachments do not overlap
the draw-read hazard. In `dxmt9_render_pass_internal.hpp::makeAttachmentKey`,
the Clear key contains only requested aspects, while the Draw key contains
the complete attachment set. A depth-only Clear therefore cannot equal an
otherwise compatible color+depth Draw key. The nonmatching branch calls
`LifecycleRuntime::flushPendingClear` before endRender/startRenderPass.

This does not prove all 65 Clears may fuse. The next gate must observe that
specific flush scope and classify attachment/subresource identities, dimensions,
sample counts, effective render area and read hazards. Merely relaxing equality
to a handle subset can under-clear a larger depth target, ignore a sampled
alias, change resolve/store behavior or cross a query/order boundary. A real
candidate needs the serial reference, bounded refinement/native counterexamples,
meaningful GPU readbacks and an observer-thin economy comparison before any
promotion or removal-budget claim.

## Limits and next action

Command-body CPU is 27.426/24.859 ms for A0/A1 versus 23.910/23.187 ms for
B0/B1; the observer-ON runs being faster does not establish negative overhead.
No samples are removed or repeated, and there is no calibrated subtraction.
The whole draw loop remains 13.588/13.209 ms in B, larger than the newly
selected Clear-related scope. This experiment does not establish Clear as
the largest default owner.

Prioritize the concrete Clear fusion feasibility/coverage check over more
label, argbuf-open or endEncoding micro-optimization. Keep the full draw-loop
and canonical-live root attribution obligations open. The original-input
oracle remains production-counter diagnostic with `content_authority=unknown`
and `timing_qualified=0`; no live FPS or correctness-qualified removable CPU
amount is established.
