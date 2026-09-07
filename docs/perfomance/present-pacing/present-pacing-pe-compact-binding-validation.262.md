---
domain: present-pacing
workload: 3DMark05 GT2; 3DMark06 GT2
title: "Compact binding emission reuses admission identity validity"
type: evidence
status: measured-no-claimed-gain
updated: 2026-09-08
source: experiments/output/pe-compact-binding-validation-20260908/analysis; experiments/output/app-d3d9-3dmark{05,06}-pe-binding-validation-361a4797-gt2-{perf,sampler}-r1-20260908; experiments/output/app-d3d9-3dmark06-pe-current-bb57d01c-gt2-perf-user-rerun-r1-20260908
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

## Linked evidence

The analysis root freezes both baseline PE images from `bb57d01c` and the
candidate pair, plus the public manifest-body audit. Main's twelve public
body metrics per width are unchanged; the outlined compact binding consumers
are inspected separately. The candidate DLLs shrink by 5,632 bytes for PE32
and 3,072 bytes for PE64. Inner acquire code is inlined into some larger row
bodies, so isolated row-body sizes are not an operation-cost comparison.
`audit_linked_binding_paths.*` compares complete selected code-symbol ranges,
including directly reachable post-return blocks. It records unresolved indirect
branches rather than treating a first-return prefix as a full function. Row
and separately outlined acquire bodies are summed without overlapping ranges;
shared pin helpers and section drivers are not part of that sum. All seven
families shrink in this static comparison, but this is not a count of dynamic
instructions executed for every binding.

PE32 texture row plus acquire goes from 601 bytes / 191 instructions / two
direct calls to 496 bytes / 149 instructions / one call. Its source and
physical-pin guards remain. PE64 texture goes from 663 bytes / 160 instructions
to 429 bytes / 117 instructions. Stack is not free: PE32 texture's selected
baseline prologues subtract 8 and 64 bytes in two functions; the candidate
subtracts 88 in one. These amounts exclude saved registers and return addresses
and must not be read as peak stack usage. Section driver costs change too.
The audit does not convert these code metrics into runtime savings.

Final PE32 SHA-256 is
`cb32bf442f31fe46e843ea34b9178f30d8af695e1cd2afc1dc19ff0cc85b9d6b`;
PE64 is `0c0c0aacacd60532052e60466787c5c79db2eefb056371fe3490460662d0489b`.
The post-negative-control relink differs from the initial frozen candidate
only in the COFF/export timestamps; both copies remain in the analysis root.
Final audit receipts use the final images. The Unix provider and both bridge
DLLs retain H260/H261 identity. All four canonical builds pass.

## Sampler-OFF runtime spots

Commit `361a4797` uses the same Sikarugir cx-24.0.7 runtime, 1024x768 for 05
and 1280x720 for 06, perf/Warn, replay offload ON and frame sampling ON.
PE detailed phases, stats, ledger, module map and stopped-PC sampler are OFF;
inline constants, segmented transport and opaque index reorder remain OFF.
A read-only process observer samples CPU time/RSS at two-second intervals.
No concurrent build, native test or second Wine run executes during a scene.

| Product | Scene frames | Scene wall | FPS | CB/frame | Pass/frame | Partial process CPU ms/frame |
|---|---:|---:|---:|---:|---:|---:|
| 05 GT2 | 1,935 | 64.587 s | 29.960 | 4.001 | 15.787 | 69.648 |
| 06 GT2 | 1,169 | 64.123 s | 18.231 | 4.002 | 19.865 | 95.601 |

Positive-wall frames supply FPS; 06 additionally requires more than 100 draws
to exclude loading, not slow scene frames. CPU joins cover 1,847/1,148 frame
increments and 60.268/62.282 seconds respectively. These are approximate partial
all-thread process costs, not PE time or whole-scene CPU totals.
`perf-qualified.json` and each run's `measurement-qualification.json` preserve
the rules, exact denominators and five staged-image hashes.

Both runs pass with eight reported renderer error counters at zero. 05 exits
normally; 06's permitted timeout closes the completed result GUI. 05's last
counter reports 1,936 Presents after logged frame 1,935. 06's last counter
reports 2,160 while the log reaches frame 2,172; the error snapshot does not
prove the unreported tail. No whole-scene pixel oracle is claimed.

Against H260's 05 spot, FPS changes by +0.50% and approximate CPU/frame by
-0.30%. Against the pre-change current-HEAD 06 repeat below, FPS changes by
-0.39% and CPU/frame by +0.47%. These small, mixed, single-run differences
prove neither improvement nor regression or neutrality. Historical README
30.1/18.1 also uses a different index-reorder setting. Repeatable recovery and
CPU-consumption reduction remain open; README is unchanged.

## Separate full stopped-PC profiles

Both profiles use the same five final images, perf/Info, module mapping and
the 250 Hz PE thread sampler with complete self-PC output. Detailed phases,
stats and ledger remain OFF. These runs suspend the target thread and are
not FPS or product CPU measurements. `qualify_profiles.py` validates every
cumulative report, module-probe containment and the frozen-image symbol join;
it does not add cumulative snapshots together.

| Product | Valid reports | Last report Present | Thread samples | PE self samples | Stored / printed PC buckets | Last logged frame |
|---|---:|---:|---:|---:|---:|---:|
| 05 GT2 | 31 | 1,860 | 13,528 | 1,366 | 616 / 616 | 1,864 |
| 06 GT2 | 35 | 2,100 | 13,958 | 851 | 463 / 463 | 2,155 |

All reports have zero suspend/context/resume failures, omitted samples and
overflow samples; all eight reported renderer error counters are zero. The
06 run passes with its completed result-GUI timeout. Full bucket output does
not mean full-scene coverage: the last report omits the logged tail. For 06,
subtracting the Present-960 snapshot before the first heavy frame at 1,015
leaves 13,229 thread samples and 846 PE samples through Present 2,100. This
is an approximate scene prefix, not exact isolation of loading and scene.

`05-pe-symbols.json`, `06-pe-symbols.json` and `profiles-qualified.json`
preserve full populations. Selected unambiguous symbol buckets from the
cumulative reports are:

| Symbol | 05 samples | 06 samples |
|---|---:|---:|
| `ValidatedConstWrite<16>::applyNonEmpty` Live kernel | 66 | 61 |
| `SetVertexShaderConstantF` | 45 | 25 |
| `accumulateRetentionDelta` | 58 | 25 |
| `buildSparseStatePlan` | 40 | 21 |
| `preparePeSemanticSparsePlanLayout` | 26 | 20 |
| `writeSparsePlanPayloadQualified` | 34 | 27 |
| Prepared identity witness `add` | 22 | 12 |
| `acceptSparseStatePlan` | 23 | 10 |

These are exclusive stopped-PC buckets, not stacks, inclusive phase costs or
CPU milliseconds. The join leaves 368/222 cross-symbol samples unassigned.
Interior padding, inlining, 64-byte bucket boundaries and asynchronous Wine
PC mapping still limit attribution. Lexical owner-family totals are not a
bottleneck ranking; nested template names can include the caller's context.
Absence of a symbol sample would not prove absence of work.

## Remaining optimization boundary

This closes collection and scoped linked-code inspection, not measured CPU
savings. Constant update/record construction and first-occurrence admission,
retention and settlement remain candidates; current-source and physical-pin
checks remain required. [H258](present-pacing-inline-constant-current.258.md)
shows that inline batching removes almost all standalone constant records,
but not their bytes, setter work or a demonstrated CPU cost. A bounded
six-family effective-state replay/fault oracle is the next useful prerequisite
for revisiting that policy. Its pixel evidence and repeatable observer-OFF
economics remain required before changing the default. More tiny validation
removals are not a substitute for establishing an actual remaining cost.

## Pre-change 06 repeat

The user-requested pre-change current-HEAD 06 GT2 rerun is retained separately
at `experiments/output/app-d3d9-3dmark06-pe-current-bb57d01c-gt2-perf-user-rerun-r1-20260908`.
It gives 18.302 FPS over 1,172 scene frames / 64.035 seconds; its eight reported
error counters are zero. That is a perf/frame-sampling spot, not observer-free
benchmark-owned FPS. The reported error snapshot ends at Present 2,160 while
the frame log reaches 2,171. Its partial all-thread process join is 95.157
CPU ms/frame, not PE-exclusive cost. README recovery and repeatable CPU savings
remain unproven.
