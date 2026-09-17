---
domain: present-pacing
workload: STALKER CoP Day
title: "GPU occlusion ownership, query timeline, and StateBlock rebuild evidence"
type: evidence
status: measured
updated: 2026-09-17
source: experiments/output/occlusion-timeline-r642-r643.noindex/analysis.json; experiments/output/occlusion-timeline-r642-r643.noindex/qualification.json; experiments/output/occlusion-timeline-r642-r643.noindex/occlusion-preserve-r644-day.analysis.json; experiments/output/occlusion-timeline-r642-r643.noindex/r644-qualification.json; experiments/output/app-d3d9-stkcop-bench-occlusion-preserve-r644-day/result.json
related: present-pacing-policy-consolidation-wild.319.md; present-pacing-query-default-promotion.316.md; overview.md
---

# Real visibility is separate from completion; source cuts must preserve depth

The default primitive-count OCCLUSION approximation is not GPU visibility.
`DXMT9_METAL_OCCLUSION=1` implements a bounded actual-result provider without
changing the PE wire ABI. It remains default OFF. Query result correctness,
whole-sequence visual qualification, and FPS are distinct gates.

## Implementation and bounded verification

BEGIN creates an immutable issue-qualified result generation. Up to 64 active
queries share an immutable scope frozen onto each published ChunkSlot. Each
actual render pass owns a separate visibility buffer; counting is enabled
once, not toggled per draw at the same offset. Completion retains and checks
the originating command buffer, credits every scope member once, and settles
pending contributions before the queue completion waterline. END completion,
initializer completion, epoch and failure checks remain authoritative.

Reissue does not reset live result storage. Released queries do not remain in
a strong device registry. Scoped Arena admission fails before effects and
uses compatibility; cross-source sessions, reorder/DCE, parallel encoding,
tile-FFP and diagnostic visibility scouting are excluded. Early-prefix
publication is suppressed provider-wide. These are conservative qualification
boundaries, not performance optimizations.

The 31,008-state `QueryOcclusionResults` safety model rejects read-before-fence,
stale-generation redirection and ignored failure. Release is terminal for an
object identity; it cannot reset the issue ordinal and recycle an old key.
The complete 214-configuration TLC inventory passes. Native core tests cover
reissue, failure, release/reset, saturated results and 64/65-query admission.
Metal-validation-enabled arm64 and x86_64 oracles cover depth-hidden zero,
1,024 exact passing samples, consecutive draws, indexed/nonindexed UP,
overlapping scopes across passes/sources, pending output preservation, alpha
rejection and exact green readback. The strengthened depth-continuation case
also checks actual Store accounting: undefined DontCare contents sometimes
survive by accident, so pixels alone are insufficient to pin this policy.

These are bounded lifecycle and concrete fixture results, not C++ memory-order,
unbounded-history, full MSAA or actual driver-failure/recovery proofs.
In particular, the lifecycle model does not prove attachment Store semantics;
the descriptor-accounting and pixel oracles own that separate obligation.

## Retained failed integration evidence: R642–643

Both diagnostic runs use Sikarugir cx24.0.7, Day High, `renderer_r2.5`,
1280×720 windowed, perf profile, Info logging, counters ON and frame sampling
OFF. Actual occlusion, the query observer and the StateBlock observer are ON;
END coalescing remains OFF. All five staged images match across the pair.
No build, test, TLC or profiler overlaps either benchmark interval.

R642 skips 1,000 ENDs and retains only EVENT queries. Its automatic capture
misses the app; `actual.png` is not scene evidence. The separately retained
`scene-restored.png` shows an actual scene after restoring the window.
R643 skips 5,000 ENDs and retains 118 OCCLUSION and 10 EVENT summaries. Every
retained row has all lifecycle stages, but **all 118 occlusion results are
zero**. R643's `actual.png` shows missing buildings and terrain, despite its
earlier restored scene looking normal. Both runner statuses are pass and all
eight error counters are zero; neither proves visual correctness.

The failed images report 18.234642 and 20.071062 benchmark FPS. These values
are retained only as rejected evidence: missing geometry is not a speedup.
They must not update README or establish provider promotion.

R643 has 27,239 semantic-boundary publications and exactly 27,239
`render_pass_depth_proof_allow_dead_no_present` decisions. New query cuts expose
the unsound assumption that no later depth use in a source means no later use
at all. A following query still Loads that attachment. Metal need not report
an API error for undefined discarded contents.

The correction preserves concrete color/depth/stencil stores provider-wide,
including the unqueried predecessor published by BEGIN. It excludes provisional
Unknown stores and preserves multisample storage alongside resolve. Load/Clear
selection is unchanged. Re-admitting store elision requires an independent
cross-boundary proof, not merely a source-local next-use scan.

## Attachment-preserving integration: R644

The corrected image repeats the Day lane and skip-5,000 observer window. All
five staged image hashes match the corresponding canonical build outputs;
the Unix image changes from R642–643. The runner exits normally, and all eight
required error counters remain zero. Both `scene-restored.png` and the later
`actual.png` show buildings and terrain rather than the missing geometry in
R643. These are spot inspections, not aligned whole-sequence pixel equivalence.

The retained window contains 49 OCCLUSION and 79 EVENT summaries, all with
complete lifecycle stages. Of the occlusion results, 27 are zero and 22 are
positive, with a maximum of 35,811 samples. Actual color/depth/stencil DontCare
store counters are all zero. The source-local depth-proof counter still reports
26,128 allow decisions, but the candidate guard prevents those decisions from
selecting discard stores. Together with the bounded oracle, this supports the
attachment-loss diagnosis; nonzero results alone are not a visibility oracle.

**Timing qualification is rejected.** A worker's native build/Metal verification
overlapped this diagnostic run. Exact invocation timestamps were not retained;
the worker confirmed the overlap after its 21:12:33 KST executable rebuild.
The benchmark's 21.149416 FPS over 2,611 rows (2,700 encoded Presents) is retained
for provenance only, not as a speedup, non-regression or README value. The
observer also perturbs polling. A quiet observer-off repeat remains required.

## What the observers establish

The timeline joins queue/query/issue/epoch identities with real publication,
submit and completion events. A recent-128 sequence ring supports late END
joins; skipped, dropped and evicted evidence is explicit. Each run retains
128 END summaries, not the entire history. END time means core END-return
observation; submit-to-completion includes host finish scheduling, not just GPU
execution. Arena publication and independent initializer-stage timing remain
outside this observer.

Before the attachment fix, R643's occlusion-window medians are 7.972 ms from
publication to submit, 6.202 ms from submit to completion and 0.409 ms from
completion to first S_OK. Their separate medians must not be added as a
per-query critical path. These timestamps demonstrate the separation of
stages but do not qualify the result bytes in the failed image.

R644's 49-query occlusion window reports these medians:

| Interval | Observed median |
|---|---:|
| Publication → actual submit | 14.323 ms |
| Submit → completion observation | 4.201 ms |
| Completion observation → first S_OK | 0.199 ms |
| Core END-return observation → first S_OK | 18.397 ms |

These are overlap-confounded diagnostic timings, not a clean bottleneck
ranking. Separate medians are not additive. The interval before submission is
a concrete next measurement target, not proof of pure encode cost. The 49
occlusion rows include 28,273 pending polls. Across the whole run the core
observer sees 305,854 occlusion polls, including 282,462 S_FALSE, 23,392 S_OK
and zero errors. Core occlusion polling takes 360.468 ms cumulatively, whereas
bridge query_get_data takes 11,620.342 ms across all query types. These scopes
differ: the core clock excludes PE flush and bridge replay drain, so neither
number alone measures the application's entire polling stall.

StateBlock-attributed binding-agnostic cache rebuilds compare old/new canonical
layout, key, resource identity and uniform payload hash after the existing
rebuild. The observer has no reuse authority; disabled mode performs no
baseline copy. R642 finds only 12 same-canonical cases in 139,303 samples;
R643 finds 7 in 150,404. Almost every key/resource identity changes; roughly
93% of shader layouts and uniform payloads change too. The broad hypothesis
"StateBlock usually rebuilds an unchanged snapshot" is not supported by these
failed-image diagnostics.

The attachment-preserving R644 census also rejects broad whole-snapshot reuse:
only 2 of 169,252 samples are same-canonical (0.00118%). Keys change in 169,250
samples, resource identity in 169,229, shader layout in 159,491 and uniform
payload in 158,640. Uniform generation changes in every sample, but generation
alone is deliberately insufficient for the changed classification. These
categories overlap and must not be summed. The observer compares the previous
cached snapshot with the next StateBlock-attributed rebuild; intervening
setters/draw selection can contribute, so this is not a controlled proof of
StateBlock-only causality or the cost of each changed component.

Whole-snapshot bypass is not implemented. A narrower component-reuse proposal
must first isolate stable components and their actual rebuild CPU cost. The
current evidence supports prioritizing query submission/polling over an
assumed redundant whole-snapshot rebuild.

## Remaining acceptance gates

R644 supplies mixed real results, attachment-store accounting, spot scene
recovery and a corrected-image StateBlock census. It does not supply clean
performance evidence. Next: a quiet observer-off repetition and broader
whole-sequence visual qualification, followed by a separate bounded timeline
window to localize END/publication-to-submit and bridge polling cost. Broader
PE conformance, MSAA/capacity interactions and real allocation/driver-fault
injection remain separate gates. No default promotion or README change is
made here.
