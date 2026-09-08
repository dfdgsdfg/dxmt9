---
domain: present-pacing
workload: 3DMark05 GT2; 3DMark06 GT2
title: "Single category walk for compact sparse layout"
type: evidence
status: accepted-work-removal-economy-unproven
updated: 2026-09-08
source: experiments/output/pe-layout-category-rebase-20260908/analysis; experiments/output/pe-three-register-equality-20260908/analysis/h283-candidate-pixels-{x86,x64}
related: present-pacing-pe-scalar-writer-authority.282.md; present-pacing-pe-sparse-work.250.md
---

# Single category walk for compact sparse layout

H283 replaces the first 25-category predicate walk in
`detail::preparePeSemanticSparsePlanLayout`
(`src/d3d9/d3d9_pe_semantic_owner.hpp`). Previously the helper counted active
sections, then visited the categories again to construct their descriptors.
The candidate constructs payload-relative descriptors during the canonical
category walk, then adds the actual header/table bias to active descriptors.
It does not change the generic layout oracle or public section-count helper.

## Refinement and limitations

For every current section alignment `a`, both the 56-byte draw header and
16-byte section descriptor are multiples of `a`. Therefore the final bias
`56 + 16 * sectionCount` commutes with each alignment step. A constexpr
assertion over the complete schema makes this a build-time prerequisite,
not a presumed property of future sections. The active descriptor sequence,
offsets, byte sizes, quota deltas, semantic byte count and final payload size
must equal the independent generic layout.

All existing section/count/range/byte checks remain. The actual emitted count
prices the table; cursor plus bias and every shifted offset remain checked
before `valid` is published. Failure can leave different partial descriptors,
but they remain invalid and unusable. No reference, allocation, retention
callback, source mutation, wire ABI, scheduling or provider default changes.
This is bounded call-local arithmetic refinement under R-CORE-REC-7.48/7.54
and R-VERIF-1.5–1.8, not a new TLA lifecycle transition. No new TLC result
is claimed.

`compactSparsePlanLayoutDifferentialMatrix` in
`tests/native/bridge/pe_semantic_owner_spec.cpp` compares the production
compact helper with `preparePeSemanticSparseLayout` using a separately
constructed generic sparse input. Successful cases require both helpers to
accept; equal rejection is not counted as a successful differential.
The bounded domain includes:

- empty and full 25-category layouts, with typed/constant schema maxima;
- each scalar family at 0/1/max, binding masks at 0/each-bit/full;
- all 64 six-constant presence masks and each family's end-anchored 1/max;
- all 25 pairs of UP lengths from `{0,1,3,5,17}`, plus full-layout odd tails;
- malformed constant byte/count metadata, absent metadata and oversized
  scalar/constant counts; rejection keeps the witness invalid.

Backing spans are checked against real storage. This is not the Cartesian
product of all 25 categories and not an executed greater-than-4-GiB aggregate
overflow test. Size arithmetic remains explicitly checked and reviewed.
Compact constant-start validity still belongs to upstream admission; this
patch does not change the existing generic/compact helper distinction.
Existing owner and producer differentials separately cover final wire bytes,
retention, capacity dimensions, malformed input and rollback/retry.

The final focused suites pass native arm64 7/7 and x86_64 4/4, including the
Unix install-name audit. Actual PE32/PE64 candidate fixtures each pass 24
signature rows: 192 readback samples exactly match the current baseline.
`verified-pixels.json` also joins fixture, source and frozen DLL hashes.
The literal oracle's existing channel tolerance is unchanged; this fixture
does not prove pixel identity for the entire GT2 scene.

## Linked work and identity

| Layout main body | Baseline | Candidate |
|---|---:|---:|
| PE32 bytes / instructions | 2,231 / 593 | 2,212 / 570 |
| PE32 direct-call sites / prologue stack subtraction | 21 / 36 | 15 / 20 |
| PE64 bytes / instructions | 2,758 / 622 | 2,787 / 612 |
| PE64 direct-call sites / prologue stack subtraction | 21 / 144 | 15 / 80 |

Including all linked layout lambdas, instruction totals are 807→784 for
PE32 and 816→806 for PE64. PE64 grows 29 bytes despite fewer instructions.
Removed call sites reflect changed inlining as well as the removed initial
predicate walk. The new descriptor loop is real work proportional to active
sections. These whole-symbol metrics include cold paths and are not dynamic
instruction counts, CPU time or a net-economy proof.

All 12 public hot-entry metrics, exports and vtable/RTTI ownership remain
unchanged in both PE widths. `codegen.json` includes every layout component,
not a first-return prefix. Baseline is `10672a25`; the measured candidate is
that source plus frozen `measured-runtime.patch`, not a clean-HEAD binary.
`build-identity.json` checks all canonical build options and the unchanged
bridge DLLs and Unix provider. Each wild receipt checks the five staged images
before and after its run.

| PE image SHA-256 prefix | Baseline | Candidate |
|---|---|---|
| PE32 | `ff67e24a7f87c38f` | `56da1dc0f414f7e0` |
| PE64 | `4706b26eded57841` | `652e9b47745802e8` |

## Actual PE economy

The cohort uses Sikarugir cx-24.0.7, actual PE32 under Rosetta, 1024x768
(05) / 1280x720 (06), perf/Warn and frame sampling. Replay offload is ON;
inline constants, segmented transport, CPU-ready Tape, index reorder and
detailed PE probes are OFF. A low-rate external process observer records
CPU/RSS; no build, test or second benchmark overlaps a timed process.
The second image pair reverses the first pair's order per product.

| Product / image | Repeat 1: frames / seconds / FPS / CPU ms per frame | Repeat 2: frames / seconds / FPS / CPU ms per frame |
|---|---:|---:|
| 05 baseline | 1,919 / 64.598 / 29.707 / 69.533 | 1,918 / 64.590 / 29.695 / 69.597 |
| 05 candidate | 1,923 / 64.547 / 29.792 / 69.529 | 1,924 / 64.536 / 29.813 / 69.432 |
| 06 baseline | 1,161 / 64.030 / 18.132 / 95.433 | 1,159 / 64.076 / 18.088 / 95.378 |
| 06 candidate | 1,156 / 64.043 / 18.050 / 95.389 | 1,158 / 64.056 / 18.078 / 95.365 |

Arithmetic image means give 05 FPS 29.701→29.802 (**+0.342%**) and process
CPU/frame 69.565→69.481 ms (**−0.121%**). For 06 they are 18.110→18.064
FPS (**−0.253%**) and 95.406→95.377 ms (**−0.030%**). All observed changes
are small. They establish neither an attributed saving nor statistical
neutrality/regression, and do not establish README recovery.

CPU uses one same-PID cumulative endpoint delta, covering 96.55–97.24% of
scene frame increments. It is all-thread process time including game/Wine,
not PE-exclusive time. The qualifier reselects the endpoints from the raw
CPU observation log rather than accepting a derived report's endpoints.

Eight reported renderer health counters are zero for all eight runs.
CB/frame remains about 4.001–4.002; pass/frame spans 15.785–15.792 (05)
and 19.856–19.864 (06). 06's final health counters precede the last logged
frame by 3/24 frames for baseline and 20/21 for candidate, so those tails
are not error-counter-proven. 06's permitted result-GUI timeout occurs after
a complete contiguous scene. No `.3dr`, whole-scene pixel proof or inference
from process-elapsed FPS is claimed.

`verified-economy.json` rechecks all raw artifact hashes, source patch and
five-image identities, scene continuity and FPS/CPU arithmetic. `final-audit.json`
joins those eight runs with the final native and pixel receipts, verifies the
runtime diff still equals the measured patch, and verifies the Wine root holds
all five candidate images. README is unchanged.

## Decision and next gate

Retain the duplicate category-discovery prepass removal, not a claimed net
CPU/FPS gain. Unlike H282's substantially larger writer and PE32 stack,
H283 reduces linked instructions, call sites and stack state without a large
companion code-size tax. This is a qualified local implementation change:
replacing a fixed source-fact pass with an active-descriptor pass is not a
proof that every sparse/dense case costs less. Independent clean-context
review reached the same bounded acceptance decision.

The precise open gate is whether rebasing is dynamically cheaper under the
workload's actual active-section count/shape distribution. An observer-only
histogram, followed by counterbalanced actual-PE32/PE64 measurements of the
complete helper across empty/representative/dense cases, would answer that
more directly than additional unqualified single FPS spots. Such a fixture
must qualify its linked helper against the production DLL rather than assume
that separately compiled test code has the same call shape. Repeatable
whole-workload CPU/FPS savings and README recovery remain separate gates.
