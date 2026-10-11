---
domain: present-pacing
workload: STALKER CoP H481 original Normal source, x86 release
title: "DrawRun scratch initialization is real; its small CPU delta misses the frozen noise gate"
type: evidence
status: bounded-removal-candidate-no-promotion
updated: 2026-10-11
source: traces/stkcop-draw-loop-h487/{contract.md,run.py,analyze.py,matrix.json,matrix-images.json,summary.json,baseline-sha256.txt,candidate-sha256.txt,restored-sha256.txt,build-options.json,candidate.patch,baseline-loop-disassembly.txt,candidate-loop-disassembly.txt,baseline-indirect-symbols.txt,candidate-build.log,candidate-tests.log,restored-build.log,restored-tests.log,image-A.stderr,image-B.stderr}; traces/stkcop-normal-finish-h481/tape/frame-10699344711800-1/manifest.json
related: present-pacing-normal-source-cost.486.md; present-pacing-root-game.477.md; present-pacing-run-construction-removal.451.md
---

# Conditional DrawRun scratch construction: a real operation, a bounded result

H486 identified DrawRun as the original input's command-body owner. Release
disassembly now shows a concrete redundant operation: every valid draw
initializes full hot-state and shader-layout scratch before discovering whether
it needs a binding override. A small conditional-construction candidate removes
that operation and preserves the original GPU output in every execution.

The candidate is faster in all eight measured A/B pairs, with a paired body-CPU
delta median of 0.907 ms. But the frozen acceptance gate fails: that median is
smaller than the largest A/A difference, 1.123 ms, and execution-order groups
differ. This is not a qualified default-tail removal budget or the explanation
for the large backend performance gap. The candidate is archived, not promoted;
production source and x86 build images are restored to baseline.
Both restored runtime images are byte-identical to H486, and the same six
focused tests pass again after restoration.

## Mechanism verified in the release binary

`encodeChunkImpl`'s serial `encodeDrawSubrange` in
`src/dxmt9/dxmt9_draw_encoder_chunk.mm` creates `overrideHot{}` and
`overrideShaderLayout{}` before `drawParamBindingOverride` and
`drawParamBindingSnapshot`. With no binding override they are unused. With a
binding override the hot record is immediately overwritten with `hot`, and
the shader layout, when present, is copied from the existing command layout.
Alpha-only overrides do not require either scratch value.

The x86 baseline retains nine bzero calls before the binding decoder; the
candidate retains two. The seven removed calls clear 8,360 bytes in total,
besides removed inline default-field stores. The baseline also destroys the
default shader-layout stream array on the no-override path. This is actual
generated work, not an inference from C++ braces or an inline annotation.
Indirect-symbol inspection resolves bzero and memcpy stubs; the disassembler's
fallback `_wmemchr+...` annotations are not the callee names.

The experimental patch uses disengaged local `std::optional` storage, then
`emplace(hot)` and, if needed, `emplace(*drawStateView.shaderLayout)` inside
the existing binding-override predicate. No heap or cross-draw cache is added.
The 8,112-byte hot copy remains when the override needs it; only its prior
default initialization disappears. Optional shader destruction checks engagement;
copied stream references still live until the same draw iteration ends.

This is local value-construction elision. It introduces no new source order,
pass action, shared cache, dirty-bit transition, retained-resource identity or
completion rule, so no new TLA transition is needed. Missing layout, alpha-only
and absent overrides retain the same borrowed command view; present binding
overrides apply the same helper to the same copied inputs. The archived patch
does not change sibling parallel/ordinary draw paths. Broader promotion and
coverage of unusual override inputs are not claimed by this experiment.

## Frozen experiment and verification

Baseline revision is `99534a58`, whose runtime is identical to H485/H486.
Only `dxmt9_draw_encoder_chunk.mm` is recompiled for the candidate; the runtime
archive is linked into both the native library and Unix provider, so both
per-arm images change together. The runner is byte-identical. Isolated image
directories and actual dyld rows authenticate both arms; no Wine staging occurs.
Both use the existing x86_64 release configuration, ProductionSlot, serial
partitioning, disabled archive/prewarm and the unchanged H481 bundle and source
cuts. The toolchain remains Apple M1 / Xcode 27 / Metal 4 under Rosetta.

Builds and tests finish before the matrix. Each child uses a fresh process and
device. The fixed sequence is two non-timing image/oracle admissions, two
warm-ups per arm, four A/A pairs and eight A/B pairs alternating AB/BA. There
are no retries, outlier exclusions or command/draw-level clocks. The existing
coarse source trace remains enabled for CPU measurements; costs include that
observer. Setup sequences are excluded using V4 source mapping, not time cuts.

All 30 children complete. Every one matches both original 3,686,400-byte source
and output readbacks exactly, restores 16 surfaces, activates 12 aliases,
balances 779/779 wrappers and preserves all three original source cuts. All
28 CPU traces close without overflow/incomplete writers. The large source
remains captured index 1 / local sequence 4 with 556 commands. An independent
reducer rechecks hashes, receipts, source maps, clock availability, absence of
fine-grained clocks, chronology and image hashes after the complete matrix.

Focused x86 tests pass 6/6: state/draw transform, uniform dirty transitions,
encodeDraw recorder, V4 provider, attachment restore and install-name audit.
These are regression checks, not a new exhaustive override-lifetime proof.
GPU evidence is the original-source oracle in each matrix child. Counter-OFF
runs do not supply a measured zero GPU-error counter. First-read content
authority remains unknown; original final-output equality does not prove every
intermediate resource byte.

## CPU result and failed acceptance gate

Values below are native Encode-thread CPU for the original large source, not
whole-process CPU, replay elapsed time, GPU time or per-Present latency.

| Metric | Baseline | Candidate |
|---|---:|---:|
| Command-body median | 18.797 ms | 17.616 ms |
| Whole Encode median, including cold prefetch | 54.453 ms | 52.983 ms |
| Prefetch median | 33.840 ms | 33.574 ms |

The median of **paired body differences** is 0.907 ms; it is not the difference
of the two marginal medians. All 8/8 pairs favor the candidate, spanning
0.457–2.587 ms. AB-order savings median is 0.593 ms, BA-order 1.540 ms.
Whole Encode occasionally increases despite the lower body because cold
prefetch varies; it cannot be used as a steady-state savings figure.

A/A body differences are +0.171, -1.123, -0.087 and -0.336 ms. Before the
matrix, acceptance required positive savings medians in both orders, at least
7/8 faster pairs, and a paired median greater than the largest absolute A/A
difference. The first two conditions pass; the last fails. This conservative
finite gate is not a confidence interval. Do not relax it after seeing the
result, label the A/A variation pure instrumentation overhead or repeat until
an acceptable pair appears.

## Consequence for the full objective

The redundant initialization is a credible small optimization candidate, not
a demonstrated dominant bottleneck. Do not reinterpret the 6.620 ms intrusive
loop residual from H486 as removable work: removing one real part did not
produce a noise-qualified removal of that size. The remaining body still
includes required Metal encoding, binding semantics and pass transitions.

The next owner decision still needs current source-matched Replay CPU and
default-tail owner attribution, including Primary's renderer share and the
unnamed high-CPU thread from H477. Another small scratch or initialization
change must not substitute for those larger unresolved ownership questions.
No FPS, default promotion, globally largest owner or maximum-removal claim is
made. Archived candidate/source/images preserve the result without leaving
an unqualified runtime branch or toggle in production.
