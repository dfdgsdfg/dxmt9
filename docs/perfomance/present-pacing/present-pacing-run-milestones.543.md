---
domain: present-pacing
workload: STALKER CoP H502/r5 original Normal input
title: "Run-level milestones separate the whole draw loop from pass entry"
type: evidence
status: local-loop-and-pass-owners-separated-removal-open
updated: 2026-10-15
source: traces/stkcop-run-milestones-h543/{contract.md,dxmt9_draw_encoder_chunk.mm,build.py,build.log,build.json,run.py,analyze.py,summary.json,pass_join.py,pass-join.json,parser-tests.log}; traces/stkcop-run-milestones-h543/{image,A0,B0,B1,A1}-receipt.json
related: present-pacing-encode-command-owner.542.md; present-pacing-drawrun-preparation.426.md; present-pacing-query-proof-wild.429.md
---

# Separate whole-loop CPU without another timer on every draw

H542 selected DrawRun execution, not nondraw dispatch. A private diagnostic
copy of `dxmt9_draw_encoder_chunk.mm` keeps its five existing run milestones,
removes the two nested encodeDraw record calls and clears the nested-draw
selection bit. The build pins exactly those three edits. No rendering,
source-order, queue, lifetime or shader-policy transition changes; no new TLA
model is required for this observer-only copy.

Only that object is replaced in a copy of H537 A's runtime archive. Both private
native/provider images are relinked with install-name fixup/audit; the original
runner is byte-identical. An initial private link-order/dependency selection
error is corrected before any run. Canonical objects, source, Wine installation
and original H537 images remain untouched. A separate OFF image/oracle check
authenticates the private dyld paths before the fixed A0/B0/B1/A1 population.

All five fresh-device executions match the original source/output pixels,
seven source mappings, 670 balanced wrappers and production-counter query
results. A has command-level tracing; B adds only the run milestones. All
traces close with no overflow/incomplete writers. There are **zero nested draw
events** and no nested-draw authority bit. The existing supply analyzer tests
pass 42/42; an independent integer-endpoint reducer verifies 9,048 phase pairs
over both observed sources in the two B runs.

## Source 5: whole loop and pass entry dominate

| DrawRun component, ms per captured source | B0 | B1 |
|---|---:|---:|
| Uniform/initial command setup | 0.680 | 0.669 |
| Flush-blit/read-hazard setup | 0.345 | 0.364 |
| Pass entry/transition | 6.816 | 6.600 |
| UP prescan/setup | 0.357 | 0.429 |
| Binding-plan construction setup | 0.286 | 0.268 |
| **Whole draw loop, including encodeDraw** | **14.943** | **14.268** |
| Inclusive 526 DrawRuns | 23.427 | 22.598 |

The inherited parser calls the last field `loop_residual`. Here it means the
**entire loop** because no nested encodeDraw time is recorded or subtracted.
It must not be compared as the same bucket as H426/H486's per-draw-subtracted
residual. Binding-plan updates inside encodeDraw also belong to this loop,
not solely the small command-level construction bucket.

Source 1's whole loop is 5.877/5.694 ms and pass entry is 1.184/1.113 ms.
Scopes remain observer-inclusive self CPU, not default ms/frame or removable
cost. The current split is consistent with H426's historical two-owner
hypothesis; it supplies a matched original-input decomposition with fewer clocks,
not a new proof that all preparation is redundant.

## The pass-start candidate is narrower than generic hazard checking

H542's separate unpriced progress transcript records 80 successful
`startRenderPass` invocations at distinct source-5 DrawRun command indices.
The transcript has all 591 command indices in exact order; both H543 B traces
have the same 526 DrawRun positions. `pass_join.py` partitions their pass-entry
scopes using that external command-index set:

| Command-index group | Commands | B0 pass-entry CPU | B1 pass-entry CPU |
|---|---:|---:|---:|
| Indices with pass starts in the structural witness | 80 | 6.548 ms | 6.353 ms |
| Other DrawRun indices | 446 | 0.268 ms | 0.247 ms |

This cross-run join is **candidate discovery**, not an observed pass-decision
transcript inside the timed runs. Original pixels and source counts alone do
not prove identical internal decisions. Do not divide these values by 80 and
call the result the Metal driver's encoder-creation cost. Pass entry includes
closing an old pass, attachment-store decisions, actual creation/configuration,
and surrounding work.

Code review identifies a concrete next distinction. In
`dxmt9_render_pass_encoder.mm`, `prepareRenderPassWithStoreProofLookahead`
can call color/depth/stencil lookahead proof routines before
`beginRenderPassWithStoreProofLookahead` creates the Metal encoder and configures
it. The proof routines traverse future source commands. H428 already elides
proof work when actual Metal occlusion forces preservation; the current frozen
production-counter diagnostic has Metal occlusion disabled. That earlier
elision is not a reason to assume this current proof path costs zero, nor is a
loop in source code evidence that it dominates the measured pass bucket.

## Interference and next decision

Source-5 A bodies are 22.486/24.013 ms, versus B 24.867/24.010 ms.
B0−A0 is +2.381 ms; B1−A1 is −0.003 ms. Source-1 differences are
+0.732/+0.750 ms. No negligible-overhead claim, statistical bound or calibrated
subtraction is supported. Do not compare this private image's absolute costs
against H542 as a product regression or improvement.

The next useful pass measurement is **preparation/proof versus actual Metal
creation/configuration**, with only pass-level clocks and source identity.
The larger whole-loop owner remains open as well. Do not remove a pass/hazard
boundary or add a cache from these scope totals. A correctness-qualified removal
experiment and canonical live ownership/transfer are still required; the global
goal is not complete. No default, FPS or README change follows.
