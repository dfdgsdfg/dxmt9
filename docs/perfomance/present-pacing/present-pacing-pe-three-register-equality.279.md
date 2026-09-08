---
domain: present-pacing
workload: 3DMark05 GT2; 3DMark06 GT2
title: "PE three-register equality candidate"
type: evidence
status: rejected-runtime-candidate-tests-retained
updated: 2026-09-08
source: experiments/output/pe-three-register-equality-20260908/analysis
related: present-pacing-pe-vs-constant-call-shape.278.md; present-pacing-pe-held-constant-value.269.md
---

# PE three-register equality candidate

The runtime candidate at `6ffdf357` is withdrawn after the measurements below.
The H278 runtime is restored; the new native/PE pixel pins and generic
short-range acceptance contract remain. This is insufficient economy evidence,
not a demonstrated rendering failure or a proven performance regression.

## Boundary and decision gate

H278 measures three-register calls as 85.05%/79.73% of live VS-F calls in
05/06 GT2; 87.89%/93.05% of those are already equal. H279 tests one bounded
shortcut in `ValidatedConstWrite<16>::applyNonEmpty`: after both value and
dirty-store materializations, compare exactly 48 bytes using unaligned-safe
integer loads and XOR/OR reduction. An equal range returns without entering
the register loop. Prior dirty membership/bounds remain untouched. Changed
ranges retain the original loop; other lengths retain their original update
semantics. Four-byte bool handling is unchanged.

The specialization applies to all 16-byte register families, not just VS-F.
It adds no storage, persistent equality cache, ABI field or runtime flag.
Validation, allocation failure, Recording routing and lifetime policy remain
unchanged. The measured workload population selects the candidate, but does
not prove that its precomparison is cheaper. Mismatching three-register calls
pay both the precomparison and the original loop.

## Bounded refinement and actual pixels

The production fixed-width operation is compared with the independent serial
reference for 64 prior dirty masks, eight changed masks and 16 unaligned input
offsets: 8,192 new combinations. Nonzero start, outside-range bytes/dirty
obligations, exact bits including NaN/signed-zero patterns, source immutability,
all-zero/nonzero growth, stable warm storage and exact self-alias are covered.
Middle-only and last-only A-A-B-A sequences preserve earlier dirty work. Old
matrix and guard-page tests remain enabled. Arbitrary partially overlapping
caller/private-shadow storage is not a new supported API contract.

Native arm64 passes four focused suites; native x86_64 passes those four and
the install-name audit. This local deterministic shadow refinement uses the
production-bound exhaustive native layer; it introduces no interleaving,
queue or lifetime transition requiring a new TLA model. These host tests do
not establish native arm64 PE execution.

The actual PE conformance fixture adds six three-register VS-F/PS-F rows,
each making public A-A-B-A setter calls and four ordinary VB draws. Separate
unchanged, middle-only and last-only rows make stale constants observable.
Both PE widths pass using the ordinary default path with inline constants and
decimated diagnostics OFF. Frozen H278 baseline and candidate produce exactly
matching 96 sampled pixels per width (72 old, 24 new), bound to the actual
staged images in `pixel-differential.json`. The fixture's literal tolerance
remains three channel units: one VS last-only red channel differs by one unit
from its literal in both images. This is exact image-to-image agreement, not
exact agreement with every literal or whole-workload pixel proof.

The first baseline x86 attempt stopped before launch because the frozen DLL
layout lacked builtin postprocess stamps. The actual builtin postprocessor
was run before adding stamps; full DLL hashes remained identical. Only the
qualified `baseline-pixels-x86-r2` attempt is used.

## Linked PE cost and rejected variants

An initial 48-byte `memcmp` emitted an external call on PE32 and was rejected
before Wine. Short-circuit element compares added more branches. Boolean-AND
and the chosen word-XOR form produce the same inspected reduction shape.
Intermediate DLLs, source patches and full linked-symbol reports are retained
under the analysis root.

| Complete 16-byte kernel | H278 bytes / instructions | Candidate bytes / instructions |
|---|---:|---:|
| PE32 | 594 / 208 | 660 / 224 |
| PE64 | 561 / 176 | 655 / 194 |

No new equality/copy library call is introduced. Public setter metrics,
exports, TU ownership and the four-byte kernel remain unchanged. Local stack
subtraction stays 8/40 bytes, **but that is not a no-spill proof**: PE32's
fallback loop now reloads the source argument and compares a stack-held count
per iteration. The equal-three path returns before that loop. Both bridges
and the Unix provider remain byte-identical. The actual GT2 programs execute
PE32 under Sikarugir, not arm64 PE.

## Fresh economy evidence

Eight sequential runs compare the frozen H278 runtime with `6ffdf357`: two
baseline then two candidate runs per product (06 first, then 05). These are
fresh runs, separate from the earlier user-interrupted 06 measurement. There
are no overlapping builds or tests. Sikarugir cx-24.0.7, 1024x768 (05) and
1280x720 (06), perf/Warn, frame sampling and two-second process CPU observation
are fixed. Offload replay is ON; detailed PE diagnostics, stopped-PC sampling,
inline constants, segmented transport, CPU-ready Tape and index reorder are
OFF. 06 uses the focus-free single-GT2 GUI selector and completes the scene
before the allowed result-GUI timeout. All five actual staged image identities
match each launch recipe.

| Product / image | Scene FPS, r1 / r2 | Process CPU ms/frame, r1 / r2 | Scene frames, r1 / r2 |
|---|---:|---:|---:|
| 05 H278 | 29.774 / 29.821 | 69.592 / 69.514 | 1,922 / 1,925 |
| 05 candidate | 29.686 / 29.827 | 69.369 / 69.312 | 1,917 / 1,927 |
| 06 H278 | 18.142 / 18.047 | 94.982 / 95.631 | 1,162 / 1,155 |
| 06 candidate | 18.163 / 18.183 | 95.115 / 95.146 | 1,163 / 1,164 |

Mean candidate-minus-baseline changes are -0.139%/+0.434% FPS and
-0.306%/-0.185% process CPU per frame for 05/06. That is approximately
0.213/0.176 ms less process CPU per frame, not an isolated PE saving.
The cumulative same-PID CPU endpoints cover 96.7–97.5% of scene frames;
their phase coverage is not identical. No selected-interval CPU sums are
used. Two observations per image, fixed A-A-B-B ordering and whole-process
CPU cannot establish such a small attributed saving. The 06 baseline repeat
spread is already about 0.5% FPS. No confidence interval, neutrality or
material improvement is claimed.

All eight complete useful scene intervals qualify independently from GUI
process duration. Each has zero reported chunk/GPU/pipeline errors. 05 health
snapshots cover the scene end; 06 leaves 43/31 baseline and 56/2 candidate
tail frames after the last reported health snapshot. These tails are not
claimed error-verified. CB/frame stays about 4.001 (05) / 4.002 (06), with
passes/frame 15.785–15.789 / 19.850–19.862. Process RSS peaks also vary:
05 baseline 3,284,804–3,293,560 KiB versus candidate 3,305,832–3,313,972 KiB;
06 baseline 3,046,676–3,302,088 KiB versus candidate 3,381,516–3,833,740 KiB.
No memory saving or causal attribution of that variation is established.

`run_economy.py` records exact commands, environments and full hashes.
`validate_measurements.py` independently re-derives every scene and cumulative
CPU envelope, checks preserved raw artifact hashes and records
`measurements-verified.json`. Run directories are
`app-d3d9-3dmark{05,06}-pe-three-register-6ffdf357-{baseline,candidate}-gt2-off-perf-r{1,2}-20260908`.
FPS is renderer scene sampling, not benchmark-owned `.3dr` scoring.

## Disposition and restoration

The data suggest a small CPU reduction but do not price it robustly against
the mismatch precomparison and PE32 fallback reloads. Keep those extra costs
out of the default path pending stronger evidence. Restore only the runtime
shortcut, retaining all additive tests and the spec's acceptance conditions.
README remains unchanged; the 05 30.1 FPS recovery target is still open.

Both PE builds and the full Unix build are refreshed after restoration.
Runtime source is identical to H278. `restored-images.json` verifies every
PE image byte except two parsed COFF/debug-directory timestamps is identical
to its frozen H278 image; full SHA-256 hashes remain distinct and preserved.
The initial verifier fails when only COFF/export fields are masked: the second
changing field is the parsed debug-directory timestamp, not an export field.
It is corrected to inspect the actual PE directory; no arbitrary changed-byte
masking is used. The bridges and Unix provider remain byte-identical. Focused
native tests and actual PE pixel tests are rerun against the restored,
restaged images.

A future retry needs a controlled **actual PE public-setter** economy fixture
that separates warm count-three A-A calls from changed calls and other lengths.
The current three-register fixture is a pixel oracle, not a timer. The existing
`perf_d3d9_probe.cpp` skeletal path uses 184 registers and includes payload
generation/draw work, so it cannot price this shortcut without a new bounded
mode. Do not infer that cost from host-only helper timing, aggregate setter
diagnostics or call-population counts.
