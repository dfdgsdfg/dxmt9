---
domain: present-pacing
workload: STALKER CoP H502/r5 original captured input
title: "H626 — declaration reuse eliminates calls but fails the CPU screen"
type: experiment
status: rejected-screen
updated: 2026-10-16
source: traces/stkcop-layout-reuse-h624/preflight.json; traces/stkcop-layout-semantic-h625/preflight.json; traces/stkcop-layout-semantic-h626/{contract.md,derivation.json,build.json,values.json,mutants.json,native-tests.json,preflight.json,timing-runs.json,timing-analysis.json}
related: present-pacing-encode-setup-live.623.md; present-pacing-encode-setup.611.md; present-pacing-owner-composition.621.md
---

# H626 — fewer decoder calls do not establish an Encode CPU saving

Lexical DrawRun-local declaration reuse reduces actual decoder calls from
3,982 to 757 on the original captured input: 3,225 hits, or 81.0% of calls.
Original pixels, query transcript and resource-lifetime controls pass.
The frozen CPU screen does not advance: median OFF-minus-ON is +0.172ms
per nine-root captured execution, below the 1.814ms A/A bound. Only four of
eight block lower bounds are positive, and one block is entirely negative.

No product source changes, Wine staging, promotion, live FPS claim or
experimentally established live removable budget follow. This population is
closed without replacements or additional runs. A failed screen does not
prove the decoder has zero intrinsic cost.

## Candidate and dependency closure

`src/dxmt9/dxmt9_ffp_shaders.cpp`, `decodeFixedFunctionVertexLayout`, consumes
declaration elements/FVF and effective stream0 stride. The private candidate
owns a memo for one invocation of `encodeDrawRunCommand`. It compares
elements/FVF with the immutable source and re-decodes on stride changes.
Non-equivalent declarations use the original decoder without replacing the
canonical memo. An absent optional layout is cached as a result, not a miss.
Buffer identity and offset are not decoder inputs; shader-kind classification
remains per draw.

The original `DrawNativeShadowView` layout and `encodeDraw` ABI are preserved.
A private prepared overload receives the lexical memo; existing callers
without that authority use the original decoder. No shared cache, TLS,
cross-source reuse, scheduling change or resource ownership is introduced.
The memo and its borrowed reference do not escape the caller's run.

Two preliminary qualification failures are retained, not performance results:

| Attempt | Observation | Consequence |
|---|---|---|
| H624, source-pointer eligibility | All 3,982 draws use copied binding-override declarations; zero hits | Stop before timing; pointer equality is not semantic eligibility |
| H625, semantic eligibility | OFF unexpectedly has 3,225 hits | Stop before timing; observer and candidate define conflicting inline `enabled()` functions/static state in the same namespace |
| H626, isolated candidate namespace | OFF has zero hits; ON has 3,225 | Gate qualification passes; proceed with the frozen screen |

H626 separates `h626_candidate` from the `h626` observer. Combined-TU tests
include both headers, and explicit trace-ON/gate-OFF/ON controls catch the
prior aliasing failure. OFF still includes the private wrapper/memo shape
and eligibility work: it is not a byte-identical production-entry baseline.

## Correctness and provenance

- Real-decoder exhaustive value comparisons pass 559,880 checks on each of
  ARM64 and x86_64. Tests compare layout fields, not padding or hashes alone;
  they cover valid/absent layouts, equivalent copied declarations, changed
  FVF/elements, stride changes, ABA and fresh owners in the bounded domain.
- Dropping the stride, FVF or elements dependency independently produces a
  rejected native counterexample. This local sequential value refinement
  uses the exhaustive checker rather than a new thread-interleaving model;
  it does not prove all possible declarations or GPU pixels.
- Observer controls pass 12 native cases and 22 corrupt-trace rejections.
- Reversible source derivation, parent hashes, exactly three changed runtime
  archive members and install-name audit pass. Core/provider archives remain
  unchanged; all loaded private image hashes are checked.
- Original-input OFF/ON and ordinary-queue OFF/ON controls pass. Original
  source/output SHA-256 remains
  `1955a90a4d989708bcbd5a82aa2c3dcd425ed6f94170ec55075c6b2778d68e9b`.
  The original fixture retains seven mapped sources, nine Encode roots,
  670 created/released wrappers and the 9/5/4/5 query transcript.
- The ordinary-queue control uses skipped draws and API validation; the
  original fixture supplies the pixel oracle. Query evidence is the captured
  production-counter behavior, not an actual-Metal occlusion proof.

Metal qualification precedes timing. Witness counters are enabled only for
qualification, then disabled for timing. No per-draw timers are present.

## Frozen CPU screen

One private image, original H502/r5 input, fresh processes. Fixed schedule:
two OFF/ON warmups, eight OFF shams, then eight OFF/ON/ON/OFF blocks.
All 42 executions pass original pixel/query/lifetime checks. No concurrent
build, parser or workload runs during the timing population.

The primary metric is the sum of nine same-thread Encode-root CPU bounds,
not whole-harness CPU, selected sequence8, elapsed latency or live ms/frame.
Independent decoding verifies 378 raw roots and 4,213 hashes. Bounds include
the endpoint granularity and wall brackets; they are not confidence intervals.

Positive contrasts favor ON. Values below are milliseconds per captured
nine-root execution:

| ABBA block | OFF-minus-ON point | Conservative interval |
|---|---:|---:|
| 0 | +0.886 | +0.765 to +0.992 |
| 1 | +0.069 | −0.040 to +0.182 |
| 2 | +0.024 | −0.086 to +0.150 |
| 3 | +0.642 | +0.515 to +0.758 |
| 4 | +0.276 | +0.166 to +0.393 |
| 5 | +0.008 | −0.112 to +0.126 |
| 6 | −0.432 | −0.549 to −0.320 |
| 7 | +0.541 | +0.423 to +0.649 |

The median point contrast is +0.172ms; the median lower bound is +0.063ms.
The largest absolute paired-sham bound is 1.814ms. The predeclared rule
requires every block lower bound positive and the median lower bound above
that sham bound. Both conditions fail. Do not substitute a favorable root,
drop the unfavorable block, extend the population or translate decoder-count
reduction into FPS.

## Decision and remaining objective

Close this declaration-memo CPU screen. H611 already classified decoding as
secondary; H626 does not reclassify it as the large missing owner. H623's
common-setup budget must not be assigned wholesale to this operation.

H621–H623 still place substantial actual-live work in the Encode draw loop,
but do not separate a global default-path winner from Primary uncertainty or
prove a removable amount. The remaining useful investigation is the larger
per-draw caller/preparation and pass-management work, with an exact dependency
and reuse opportunity before another candidate experiment. It must distinguish
binding-override context construction from required per-draw Metal state work;
another broad census or another generic scratch/cache trial is not justified.
The full largest-default-owner/removable-budget objective remains open.
