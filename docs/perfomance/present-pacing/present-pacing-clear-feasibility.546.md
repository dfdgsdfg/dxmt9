---
domain: present-pacing
workload: STALKER CoP H502/r5 original Normal input
title: "Standalone pending Clear costs about 2 ms; all 65 calls pass necessary fusion-shape checks"
type: evidence
status: clear-owner-and-necessary-coverage-confirmed-refinement-open
updated: 2026-10-15
source: traces/stkcop-clear-feasibility-h546/{contract.md,pass_observer.hpp,dxmt9_draw_encoder_chunk.mm,dxmt9_render_pass_encoder.mm,dxmt9_render_pass_lifecycle.mm,build.py,build.json,build.log,run.py,analyze.py,summary.json,verify.py,verification.json,parser-tests.log,shape.py,shape.json,shape-initial-enum-error.json,verify_shape.py,shape-verification.json,shape.log}; traces/stkcop-clear-feasibility-h546/{image,A0,B0,B1,A1,shape}-receipt.json
related: present-pacing-pass-outer.545.md; present-pacing-pass-owner.544.md; present-pacing-encode-command-owner.542.md
---

# Pending Clear is now directly timed, not inferred from a gap

H545 selected the pre-close interval after aspect-only Clears. H546 adds a
private span **after** `LifecycleRuntime::flushPendingClear` confirms a pending
Clear exists. It includes `encodeClearPass` and pending-state settlement, not
empty-call overhead. No rendering policy changes. Shape collection is separate
from timing so extra resource-pool lookups and output cannot inflate the
selected Clear interval.

## Execution and checks

Frozen H537 A archive/provider/runner, H502/r5 original input, three private
runtime-object replacements. Fixed image-OFF/A0/B0/B1/A1/shape population;
fresh process/device for every execution. **All six** preserve original source
and output bytes, seven source mappings, 670 balanced wrappers and query
obligations. No canonical source, build object, Wine installation or runtime
default changes. No new TLA transition applies to observer-only copies.

Both B runs contain 65 nonempty Clear spans, all joined inside source 5's
DrawRun pass-entry scopes on the same native thread. No such span is outside
those scopes. The 531 total spans per run have no overflow, clock error or
unclosed endpoints. Independent reducers verify scope sums and original-image
pins; existing supply-parser tests pass 42/42. No per-draw clocks are added.

| Captured source-5 scope | B0 CPU | B1 CPU |
|---|---:|---:|
| Nonempty pending-Clear execution, 65 calls | **2.243 ms** | **2.071 ms** |
| Full DrawRun pass entry | 7.867 ms | 7.214 ms |
| Residual outside Clear, startRenderPass and endRender | 0.559 ms | 0.544 ms |
| Whole draw loop | 15.092 ms | 13.632 ms |

Clear wall sums are 2.287/2.114 ms. These are observer-inclusive captured-source
costs, not default ms/frame or a promised saving. Command-body CPU is
22.802/23.056 ms for A0/A1 versus 26.295/23.928 ms for B0/B1. The paired
ON-minus-OFF differences, +3.493/+0.872 ms, do not calibrate per-scope overhead.
No run is dropped or repeated to improve a comparison.

## Separate necessary-condition census

The shape-only execution has neither supply tracing nor private CPU timers.
At the existing pending-Clear branch it exports flags, full-rect status,
attachment identities, actual surface dimensions/formats/subresources,
sample counts, resolve/alias information, sampled texture identities, sRGB,
active-render/query scope and the existing exact hazard predicate.

All 65 Clears fail the existing `clearKey == drawKey` check, matching H545's
mechanism. Their 650 clear/draw attachment rows show:

- 63 depth-only Clears and two color-only Clears, all full-rect.
- Requested attachment identities are present unchanged in the following Draw.
- All participating physical targets have matching positive dimensions,
  single-sample storage and no resolve target.
- No exact Clear/read hazard, sampled requested alias, active render encoder,
  query scope or sRGB-write conversion is observed at these branch points.
- The 65 shape command ordinals equal both timing runs' 65 Clear-span ordinals.

Thus **65/65 pass the recorded necessary shape checks**. This is not 65/65
proven safe fusions: no changed-policy GPU oracle has run, and attachment
presence/geometry alone does not prove load/store equivalence.

The first analyzer mistakenly assumed the internal `MultiSampleType::None`
value was zero. It is one, unlike the D3D ABI spelling. The erroneous report
is preserved as `shape-initial-enum-error.json`; the corrected analyzer reads
the enum value from `core_constants.hpp`. An independent check confirms both
descriptor and attachment sample values are one. No experiment was rerun.

## Next implementation gate

There is now a concrete operation to remove, rather than another broad timing
bucket: the separate full-surface Clear encoder before a compatible Draw pass.
Use the existing full-equality path as a baseline, then extend admission only
through an explicit geometry/identity/hazard witness, not a bare subset test.

The reference is `Clear(C); Draw(A)`. The candidate is one pass on A with
Clear load actions only on C and the original load actions on A minus C.
Before implementation/promotion, close these obligations:

1. Requested aspects retain exact resource/subresource identity and clear
   value conversion. Effective render area must not under-clear a larger
   attachment. Partial rectangles and unqualified MSAA/resolve stay outside
   admission until separately proved.
2. Unrequested color/depth/stencil contents, first-use/discard-after-Present
   behavior and final store/resolve decisions remain semantically valid.
   `clear.has_value()` affects existing load selection, so copying the old
   equality branch is not by itself a proof for aspect-only Clears.
3. Pending-Clear identity, query/order boundaries, sampled aliases and
   failure-before-effect behavior remain intact. Model the distinguishing
   mismatched extent/identity/hazard and independent depth/stencil cases.
4. Bind the witness to production predicates and native counterexamples;
   compare independent color/depth/stencil GPU readbacks, then the original
   capture under the changed policy. Final color equality alone is insufficient.
5. Measure observer-thin source CPU with a fixed A/A and alternating A/B
   population before reporting a saved amount; qualify live transfer separately.

This investigation has not established the largest **default live** dxmt9
owner or a correctness-qualified removal budget. The larger draw-loop owner
and mixed live Primary attribution remain open. The capture oracle still has
`content_authority=unknown` and `timing_qualified=0`, and its production-counter
query contract is not an actual Metal visibility-query oracle.
