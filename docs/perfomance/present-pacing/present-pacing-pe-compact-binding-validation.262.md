---
domain: present-pacing
workload: 3DMark05 GT2; 3DMark06 GT2
title: "Compact binding emission reuses admission identity validity"
type: evidence
status: implementation-verification
updated: 2026-09-08
source: experiments/output/pe-compact-binding-validation-20260908/analysis
related: present-pacing-pe-binding-duplicates.261.md; present-pacing-pe-validated-constant-range.260.md; ../../../specs/d3d9/recorder/spec.md
---

# Compact binding emission reuses admission identity validity

## Mechanism and safety boundary

[H261](present-pacing-pe-binding-duplicates.261.md) rejected a within-record
duplicate cache because the sampled target populations had no duplicate pins.
This change instead removes repeated work on the first binding occurrence:
the compact writer no longer constructs a temporary typed reference and
rechecks its kind/nonzero identity before comparing it with the fully
validated admission mapping.

In `src/d3d9/d3d9_pe_semantic_owner.hpp`, `identityWitness.add` still proves
the expected family kind, nonzero generation/object ID and object presence.
Private `makeQualifiedBinding` still compares every current source identity
field and the object pointer with that mapping. Equality transports the
already-proved validity; it is not replaced by a cached result or a raw public
trusted constructor. `pinPreparedQualifiedBinding` is unchanged: it validates
the current physical pin or acquires the next pin in order under the existing
retention checkpoint.

Each binding section checks its active range and row count once, using
subtraction-form bounds. Its row visitor retains the occurrence bound and
locator ordinal, wire-valid and null/pinned checks. This removes repeated
family/range lookup in `locatorFor` without storing another array or cache.
Full source equality, wire bounds, pin validation, fault order, capacity,
rollback and settlement remain distinct authorities. Generic conversion is
unchanged. No ABI, scheduling, chunk boundary or shader/pixel algorithm changes.

R-CORE-REC-7.36 owns this refinement. Native exact-byte and retention/failure
differentials are its deterministic oracle. Existing TLA models continue to
own transaction ordering; this local check subsumption adds no thread, wake,
lease or lifetime transition. Native tests do not prove Wine or Metal behavior,
and linked code reduction is not a CPU-time or FPS measurement.

## Native evidence

`compactProducerBindingIdentityGuards` exercises the real compact producer:
seven families times wrong kind, zero generation, zero ID and a null pointer
with non-null identity give 28 pre-append rejection cases. Four additional
cases mutate a later depth-stencil source during a test-only texture AddRef:
different kind, valid nonzero generation/ID, and an alternate same-kind pointer.
Each verifies no settlement, balanced pins, unchanged committed prefix and
exact generic-builder bytes after restoring the source and retrying. This
hook is native-only and not evidence that production Wine reenters the owner.
Final refcounts prove balance, not absence of transient AddRef calls.

Removing full source identity/pointer equality as a negative control fails
the new late-source-mutation check. Restoring the candidate passes native
focused 9/9 and x86_64 focused 5/5, including existing maximum-family,
null/duplicate, aggregate-retainer, settlement rollback and final-family
retain-fault coverage. `native-source-equality-negative.log`, `native-final.log`
and `x86_64-final.log` preserve these verdicts.

## Linked evidence and remaining gates

The analysis root freezes both baseline PE images from `bb57d01c` and the
candidate pair, plus the public manifest-body audit. Main's twelve public
body metrics per width are unchanged; the outlined compact binding consumers
are inspected separately. The candidate DLLs shrink by 5,632 bytes for PE32
and 3,072 bytes for PE64. Inner acquire code is inlined into some larger row
bodies, so isolated row-body sizes are not an operation-cost comparison.
Linked consumer costs and actual final-image runs are being qualified before
a performance verdict.

The user-requested pre-change current-HEAD 06 GT2 rerun is retained separately
at `experiments/output/app-d3d9-3dmark06-pe-current-bb57d01c-gt2-perf-user-rerun-r1-20260908`.
It gives 18.302 FPS over 1,172 scene frames / 64.035 seconds; its eight reported
error counters are zero. That is a perf/frame-sampling spot, not observer-free
benchmark-owned FPS. The reported error snapshot ends at Present 2,160 while
the frame log reaches 2,171. Its partial all-thread process join is 95.157
CPU ms/frame, not PE-exclusive cost. README recovery and repeatable CPU savings
remain unproven.
