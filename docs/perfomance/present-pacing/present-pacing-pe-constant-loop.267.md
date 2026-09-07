---
domain: present-pacing
workload: 3DMark05 GT2; 3DMark06 GT2
title: "Call-local constant backing borrows and one dirty reduction"
type: evidence
status: measured-no-claimed-gain
updated: 2026-09-08
source: experiments/output/pe-constant-loop-borrows-20260908/analysis; experiments/output/pe-inline-constant-pixels-20260908/analysis/loop-borrows-{x86,x64}-off-final; experiments/output/app-d3d9-3dmark{05,06}-pe-constant-loop-f928ba0c-gt2-off-{perf,sampler}-r1-20260908; experiments/output/app-d3d9-3dmark06-pe-constant-loop-f928ba0c-gt2-off-perf-r2-20260908
related: present-pacing-inline-constant-economy.266.md; present-pacing-pe-compact-binding-validation.262.md; present-pacing-pe-constant-copy.257.md
---

# One fixed-width Live loop reduction

## Decision and mechanism

Base `a6700464` retains H262's measured constant consumer: 66/61 unambiguous
stopped-PC samples in `ValidatedConstWrite<16>::applyNonEmpty` for 05/06 GT2.
Those are exclusive symbol buckets, not CPU milliseconds or copy-only costs.
The current PE32 image still reloads vector backing pointers after a changed
element and maintains both `changed` and `firstChanged` for the same outcome.

The Live kernel now borrows the two backing pointers after both materializing
resizes, for this call only. No resize, callback or publication occurs inside
the loop. `firstChanged == count` represents no changes because every visited
index is smaller than `count`; a second boolean is unnecessary. R-CORE-REC-7.40
and the existing compile-time state-write projection remain the policy owner.
Values, zero extension, dirty membership/range, input alignment, exact
self-alias, Recording, exception behavior and wire contracts are unchanged.
There is no new cache, field, diagnostic gate or PE/Unix crossing.

This is a local value refinement, not a new concurrency transition. The
independent old reducer is the differential oracle; no new TLA or universal
Wine/Metal proof is claimed. PE remains x86/x64 C++ under Wine/Rosetta: smaller
linked code is not evidence of native ARM64 PE execution.

The read-only retention audit did not justify deleting another pin check.
Record-unique identity admission and current owner-prefix validation answer
different questions. Capture-only pending-lease traversal is not a measured
steady-state GT2 hotspot and is not changed here.

## Prototype rejection and linked evidence

Pointer caching alone increased PE32's 16-byte kernel from 215 to 218 decoded
instructions and its prologue stack subtraction from 12 to 16 bytes. It was
not retained as the final candidate. Adding the sentinel reduction gives:

| PE / register bytes | Body bytes before / after | Instructions before / after | Prologue stack subtraction before / after |
|---|---:|---:|---:|
| 32 / 16 | 614 / 594 | 215 / 208 | 12 / 8 |
| 32 / 4 | 561 / 504 | 199 / 186 | 12 / 8 |
| 64 / 16 | 588 / 561 | 185 / 176 | 56 / 40 |
| 64 / 4 | 546 / 504 | 171 / 162 | 40 / 40 |

The prologue values exclude saved registers and return addresses. Full-symbol
inspection includes directly reachable blocks after the first textual return;
these totals are not dynamic instructions per setter. Existing cold growth
calls remain (11/10 direct calls in each PE32/PE64 body); the element loop adds
no call. The changed 16-byte source still reloads after equality, so that
separate H257 residual is not claimed removed.

`audit_constants.py`, `linked-constants.json` and frozen baseline, pointer-only
and final candidate images preserve the comparison. `audit_public.py` and
`public-linked.json` pin all twelve manifest hot-body metrics per width to
zero delta, equal export pairs and the declared vtable/RTTI/key-function
owners. The Unix provider remains byte-identical to H266.

## Verification

The new native Cartesian matrix independently seeds 784 rows per width:
16 changed masks for empty growth, plus 16 prior-dirty masks times 16 changed
masks for nonzero-start growth and both warm start positions. Both widths
give 1,568 rows and 3,136 actual factory/Live applications including repeats.
Every application compares exact bytes, extents and dirty effects with
`oldConstTransition`; source immutability and warm pointer stability are
checked. Growth cases do not assume stable addresses across resizing.
Existing null/empty/cap/overflow, unaligned, signed-zero/NaN-bit, self-alias
and A/B/A cases remain. This does not add allocation-fault or guard-page proof.

Canonical native, PE32, PE64 and x86_64 provider builds pass. Focused native
4/4 and x86_64 5/5 pass, including actual record replay and the provider
install-name audit. No whole-suite pass is inferred from these focused runs.
The final real PE32/PE64 pixel fixtures pass after the native gates: 144
sampled pixels, 18 signatures per process, standalone VS/PS counts 19/19,
and device Release zero. Both use inline OFF and worker replay; exact staged
image checks pass. These are bounded pixel receipts, not whole-scene evidence.
The preliminary PE32 run is retained
as `loop-borrows-x86-off-r1` but is not the final post-native receipt.

## Actual runtime economy spots

The original 06 perf `r1` (18.168 FPS) is excluded from performance evidence:
the user reported computer use during that run. Its raw artifacts remain
with `performance-exclusion.json`; its process CPU and RSS are excluded too.
The same-image, same-recipe `r2` passes independent qualification and replaces
it below. The two 06 runs are not averaged, and their difference is not
attributed to computer use quantitatively.

Committed `f928ba0c` runs Sikarugir cx-24.0.7, named GT2, perf/Warn with frame
sampling and the same two-second process CPU observer as H266. Replay offload
is ON; inline constants, segmented transport, CPU-ready Tape, index reorder,
detailed PE stats/ledger and the stopped-PC sampler are OFF. Resolutions are
1024x768 and 1280x720. No build, test or second Wine benchmark runs during
either scene. Five-image receipts match the frozen candidate, and provider
and bridge hashes match H266. Recipes retain the exact identities; the private
`run_candidate.py` and `compare_perf.py` retain population/acceptance rules.

| Product | Scene FPS | Frames / wall ms | CB / frame | Pass / frame | Partial process CPU ms / frame |
|---|---:|---:|---:|---:|---:|
| 05 GT2 | 29.989 | 1,937 / 64,590.173 | 4.001 | 15.783 | 69.468 |
| 06 GT2 (`r2`) | 18.270 | 1,171 / 64,094.736 | 4.002 | 19.863 | 95.017 |

05 uses positive-wall frames 1-1,937; 06 also requires more than 100 draws,
excluding loading and retaining scene frames 970-2,140. These are renderer
scene averages, not benchmark-owned `.3dr` or process-elapsed FPS. CPU joins
cover 1,878/1,166 frame increments, 130.46/110.79 CPU seconds and
61.131/63.214 elapsed seconds. They measure approximate partial all-thread
process cost, not PE-exclusive or complete-scene CPU. RSS peaks are
3,301,156/3,662,984 KiB; one spot does not establish a memory delta.

Both results pass with eight reported renderer failure counters at zero.
05 exits normally and final counter Present 1,938 covers logged frame 1,937.
06 times out only in the completed result GUI, as allowed by the catalogue;
its last counter Present 2,100 does not prove the unreported tail to frame
2,140 error-free. Its retained capture shows the forest at 0:35.47/frame 622,
not whole-scene equivalence. Summed GPU CB time is 1.995/11.880 ms/frame,
not GPU critical-path latency.

Relative to H266's earlier OFF spots, FPS changes +0.68%/+0.02% and partial
CPU/frame -1.02%/-0.70%. These small single-run differences do not establish
improvement, regression or neutrality. The runs are not counterbalanced,
thermal/host isolation is not established, and CPU joins have different
endpoints. Do not attribute these differences to the loop or claim repeatable
README recovery. README and inline defaults remain unchanged.

## Separate current PE profiles

The same candidate images also run with Info logging, module mapping and
the 250-Hz full-self-PC sampler. These two runs suspend the target thread;
their FPS and process CPU are not economy evidence. `qualify_profiles.py`
checks each cumulative report independently, exact staged-image prefixes,
module-probe containment and the frozen-image symbol join.

| Product | Reports | Last report Present | Thread / PE samples | Stored / printed buckets | Last logged frame |
|---|---:|---:|---:|---:|---:|
| 05 GT2 | 31 | 1,860 | 13,677 / 1,322 | 592 / 592 | 1,871 |
| 06 GT2 | 34 | 2,040 | 13,879 / 825 | 456 / 456 | 2,097 |

All reports have zero suspend/context/resume failures, omitted or overflow
samples; neither run reports a target-thread mismatch. Both runs pass with
eight renderer error counters zero. Complete bucket output does not cover
the unreported scene tail. Subtracting 06's pre-heavy Present-960 report
leaves 13,140 thread / 820 PE samples before the last report; the first heavy
frame is 999. This is an approximate scene prefix, not exact phase isolation.

Selected unambiguous cumulative self-PC buckets are:

| Symbol | 05 | 06 |
|---|---:|---:|
| `ValidatedConstWrite<16>::applyNonEmpty` | 53 | 23 |
| `SetVertexShaderConstantF` | 55 | 32 |
| `accumulateRetentionDelta` | 35 | 26 |
| `buildSparseStatePlan` | 26 | 12 |
| `preparePeSemanticSparsePlanLayout` | 32 | 27 |
| `writeSparsePlanPayloadQualified` | 33 | 24 |
| Prepared identity witness `add` | 19 | 15 |
| `acceptSparseStatePlan` | 26 | 17 |

The join leaves 382/229 cross-symbol samples unassigned. Inlining, padding,
64-byte buckets, changed symbol alignment and asynchronous Wine suspension
limit attribution. A lower helper bucket count is not a measured CPU saving
or an inclusive ranking. The current evidence still locates constant entry,
materialization and first-occurrence admission/retention work; it does not
justify deleting ownership checks or adding a duplicate cache.

## Next measurement boundary

Further small code reductions need better producer CPU attribution. The
existing Unix `unix_commit_chunk_entry` Info marker supplies a native thread
ID without a new PE hot probe; it is not the Win32 PE thread-ID namespace.
An external Darwin `libproc` observer can query thread counters by that ID,
but no such reusable collector or live-target qualification was implemented
here. The local public SDK labels cumulative user/system fields without
stating units: calibrate against a controlled helper before converting to ms.
Bind PID/start identity and native TID, reject missing/short queries and reset
delta baselines across disappearance. Selected-thread CPU still includes
game/Wine/synchronous Unix work, so it must not be relabelled PE-exclusive.
