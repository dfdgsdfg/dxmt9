---
domain: present-pacing
workload: 3DMark05 GT2; 3DMark06 GT2
title: "Scalar writer admission-proof reuse"
type: evidence
status: measured-net-economy-not-established
updated: 2026-09-08
source: experiments/output/pe-scalar-writer-authority-20260908/analysis; experiments/output/pe-three-register-equality-20260908/analysis/h282-v2-candidate-pixels-{x86,x64}
related: present-pacing-pe-post-lifetime-profile.276.md; present-pacing-pe-three-register-single-pass.281.md
---

# Scalar writer admission-proof reuse

H282 tests removal of four repeated scalar cardinality guards from
`PeSemanticBatchOwner::writeSparsePlanPayloadQualified` in
`src/d3d9/d3d9_pe_semantic_owner.hpp`. The measured candidate leaves the four
lexical typed views in place; an earlier lazy-view variant was inspected but
not run under Wine. Both runtime variants are withdrawn. The retained change
is additive native coverage, not a dormant optimized branch or a new flag.

## Authority and bounded correctness

`prepareAdmissionQualified` already checks render-state, texture-stage-state,
sampler-state and transform cardinalities, including zero counts, together
with mask validity. The private compact path reaches canonical emission
through field-only fixed staging and callback-free logical quota staging.
Observed append adds diagnostics, not an intervening source mutation.
The writer obtains fresh qualified source access. Reusing those admission
facts at writer entry is therefore a local R-CORE-REC-7.34–7.36 refinement,
not permission to remove the upstream zero-to-nonzero rejection.

Later binding retains can invalidate source facts. Active scalar visitors
retain their pre-copy occurrence bounds and exact final row counts; section
consumption retains descriptor order/count/byte checks, and binding emission
retains source identity and physical-pin checks. Views reference current
tables rather than snapshotting their values. Neither the original entry
guard nor this candidate proves that a previously absent scalar category
cannot become present during a later reentrant retain. Arbitrary reentrancy
is not newly closed by this experiment. Generic/control serialization,
settlement, wire ABI, lifetime, source order and provider defaults are unchanged.

`compactScalarActiveInventoryDrawDifferential` in
`tests/native/bridge/pe_semantic_owner_spec.cpp` adds all 16 subsets of the
four scalar categories for ordinary and indexed draws: 32 cases, one row per
active category, not every possible multi-row cardinality. Each compares
the real compact producer to the independent generic sparse builder, retains
a Present prefix, injects a settlement rejection, checks unchanged prefix
bytes and PendingDelta, retries the same plan, and compares final wire bytes
and balanced references. The existing 12 stale-cardinality rejection cases
remain. This is bounded value/failure refinement under R-VERIF-1.5–1.8;
existing TLA models still own lifecycle order. No new transition or TLC run
is claimed.

The initial new fixture failed because its prefix ordinal and capability epoch
were inconsistent, then because a `uint32_t` epoch produced distinct implicit
`uint64_t` temporaries at capability acquisition. The final fixture uses one
stable `const std::uint64_t` object and prefix/draw ordinals 1/1 and 2/2.
Failed logs remain; no production validation was weakened to pass the test.
Final candidate native suites pass arm64 7/7 and x86_64 4/4, including the
Unix install-name audit.

Actual PE32/PE64 candidate fixtures pass 24 signature rows each: 192 sampled
pixels exactly match the preserved baseline signatures. The existing literal
oracle's channel tolerance is unchanged. These small real-readback fixtures
are not full GT2 scene pixel proofs.

## Linked code and image identity

| Writer body | PE32 bytes / instructions / prologue stack subtraction | PE64 bytes / instructions / prologue stack subtraction |
|---|---:|---:|
| Baseline | 1,853 / 452 / 276 | 2,110 / 394 / 536 |
| Unmeasured lazy-view variant | 3,386 / 849 / 284 | 4,692 / 903 / 504 |
| Measured guards-only variant | 3,472 / 869 / 300 | 4,788 / 916 / 536 |

Removing the guards changes compiler inlining and outlined helper boundaries.
These whole-symbol counts include cold branches and are not dynamic work or
proof of a slowdown. Direct-call counts change from 24 to 25 on PE32 and
24 to 20 on PE64. The 12 audited public hot-entry metrics, exports and
archive vtable/RTTI ownership remain unchanged in both PE widths.

Baseline is `7cdb1377`; candidate is that source plus frozen
`analysis/measured-runtime.patch`, not a clean-HEAD image. Frozen DLLs,
full hashes, build-option parity and staged-image receipts are retained.
The Unix provider remains `98f16a64a310aba5`; both bridge DLLs are unchanged.
`codegen-v2.json` and `public-contracts.json` preserve the linked audits.

| PE image SHA-256 prefix | Baseline | Measured candidate |
|---|---|---|
| PE32 | `03fe9a63206dad3d` | `5e2af370ecaa0253` |
| PE64 | `27be8880e2573e6e` | `32d5b069b1e6275a` |

## Eight sequential GT2 spots

Both products run twice per image, with the second pair reversing the first
pair's order. Sikarugir cx-24.0.7 uses actual PE32 under Rosetta, not native
arm64 PE. Resolution is 1024x768 for 05 and 1280x720 for 06. The recipe is
perf/Warn with frame sampling, replay offload ON, inline constants/segmented
transport/CPU-ready Tape/index reorder OFF, and detailed PE observers OFF.
Only the external low-rate process CPU/RSS observer runs beside the workload;
no build, test or second benchmark overlaps a timed run.

| Product / image | Repeat 1 FPS / process CPU ms per frame | Repeat 2 FPS / process CPU ms per frame |
|---|---:|---:|
| 05 baseline | 29.711 / 69.635 | 29.766 / 69.247 |
| 05 candidate | 29.864 / 69.159 | 29.960 / 69.121 |
| 06 baseline | 18.167 / 95.294 | 18.154 / 94.757 |
| 06 candidate | 18.152 / 95.129 | 17.971 / 95.451 |

Arithmetic image means give FPS **+0.584% / −0.545%** and process CPU/frame
**−0.434% / +0.279%** for 05/06. These are descriptive small-sample changes,
not noise-qualified savings or regressions. CPU uses one same-PID cumulative
endpoint delta covering 96.56–97.45% of scene frame increments. It includes
game, Wine and all process threads, not PE-exclusive time. No favorable
subinterval is selected and no control value is subtracted.

All eight runs report zero in the eight checked renderer health counters.
CB/frame stays about 4.001–4.002; pass/frame is 15.785–15.807 for 05 and
19.856–19.863 for 06. 06's final health receipts precede its last logged
frame by 3/58 frames for baseline and 54/40 for candidate; those tails are
not error-counter-proven. Its canonical result-GUI timeout follows a complete
contiguous 60–70-second scene; it is not an accepted scene wedge. No `.3dr`
or whole-scene visual oracle is claimed. The prior desktop-interrupted
measurement is outside this cohort.

`verified-economy.json` rechecks raw artifact hashes, frozen runtime identities,
scene continuity, FPS arithmetic, CPU endpoint arithmetic and pixel signatures.
Analysis-only signature-filter and baseline-path mistakes were corrected;
their failed logs are preserved separately from the final passing report.

## Decision and restoration

Proof reuse is sound within the stated interval, but reducing four checks
does not guarantee cheaper compiled code. The mixed GT2 observations do not
establish a net benefit that justifies the larger writer and PE32 stack.
Withdraw the runtime candidate; do not call this a proven FPS regression.
README remains unchanged and CPU/FPS recovery remains open.

Restoration rebuilds all three canonical runtime directories. Focused native
suites pass again (arm64 7/7 and x86_64 4/4), and both final actual-PE fixtures
match all 192 baseline pixel samples. `restored-images.json` matches every
baseline PE byte except parsed COFF/debug-directory timestamp fields;
`final-audit.json` verifies all five Wine-root images were restaged. No
production `src/` or `include/` diff remains. The candidate DLLs and patch
remain separate evidence, never relabelled as the restored runtime.

The next bounded candidate is the duplicate 25-category walk in
`preparePeSemanticSparsePlanLayout`: count active sections, then visit the
categories again to construct descriptors. Payload-relative construction
followed by descriptor-only rebasing could remove the first predicate walk.
It needs checked-arithmetic/alignment and exact-layout differentials plus
actual linked PE32/PE64 economics; no implementation or benefit is claimed here.
