---
domain: present-pacing
workload: 3DMark05 GT2; 3DMark06 GT2
title: "Qualified compact settlement work removal"
type: evidence
status: verified-work-removal-gain-unproven
updated: 2026-09-08
source: experiments/output/pe-settlement-refinement-20260908/analysis; experiments/output/app-d3d9-3dmark{05,06}-pe-settlement-c1368a6e-gt2-{perf,sampler}-r1-20260908
related: present-pacing-pe-constant-copy.257.md; present-pacing-inline-constant-current.258.md; ../../../specs/d3d9/recorder/spec.md
---

# Qualified compact settlement work removal

## Scope and reference

At `97b0465e`, H257's current PE samples still locate compact settlement,
but its source-level pointer-array hypothesis does not survive inspection:
neither six-entry array exists in either linked DLL. The remaining OFF work
is six presence checks before validation and another six before clear. Empty
ranges already skip the actual dirty comparisons and `memset`; this change
must not claim to remove previously executed clears.

R-CORE-REC-7.61 adds an immutable producer-selected `hasConstantRanges` bit
in existing `SparseStatePlan::SourceFacts` padding. Reset clears it and
successful preparation publishes it once. Qualified const views consume it
only to gate the six-family validation and clear loops. Represented ranges
still require current dirty-extent validation; PendingDelta generation alone
does not prove constant freshness. Generic layout helpers remain range-based.

The private settlement interval has no PendingDelta writer or invalidating
callback. Remove its late duplicate current-ticket check and the recorder
caller's duplicate check; keep fresh source/domain, durable append, transaction
identity and current-ticket qualification before effects. No wire, transport,
queue, resource-lifetime, shader, default or Metal policy changes.

## Bounded verification

The actual build/finalize/owner/accept helpers cover all 64 six-family presence
masks with inline OFF/ON (128 cases), exact canonical bytes against the direct
builder, dirty membership, PendingDelta and scalar token settlement. Additional
traces cover ON/OFF/ON plan reuse, six families times changed/cleared dirty
ranges with retries, wrong transaction generation and token rejection/retry.
Existing owner tests retain wrong recorder/epoch, stale PendingDelta, partial
preparation, non-durable append and rollback coverage. These are bounded
production-bound native refinements, not exhaustive arbitrary D3D9 programs.

Two deliberate production mutations fail the native target: forcing false
constant presence and disabling the initial current-ticket comparison.
After restoration, full canonical builds pass, native focused 9/9 and x86_64
focused 4/4 pass. Logs retain both negative controls and final positive runs.
No new TLA transition or thread policy is introduced; unchanged recorder
models own ordering/lifetime, and native state/byte checks own this local
refinement. No separate shader/GPU oracle is claimed.

## Linked evidence

SourceFacts remains 240/304 bytes and the whole plan 288/368 bytes in PE32/64.
The new bit is at offset 42 before the unchanged PendingDelta offset 48;
native layout matches PE64. Existing enclosing recorder/device pins pass.
Exports, vtable/RTTI owners, options and 12 manifest hot-entry metrics match
H257. The manifest excludes the outlined functions audited separately below.

Both OFF paths now use one byte-load/branch per gated interval; ON preserves
all six validations. No pointer arrays, new allocation or external call appear.
`acceptSparseStatePlan` symbol extents shrink 0x980 to 0x900 (PE32) and 0x950
to 0x8f0 (PE64); these extents include alignment padding. Local stack changes
0x50 to 0x48 and 0x78 to 0x88 respectively, so PE64 has a 16-byte stack cost.
`buildSparseStatePlan` keeps its extent/frame, adding one reset byte store;
the ON preparation helper grows 16 bytes per width. The recorder settlement
caller also shrinks without adding calls. These are code-shape facts, not
measured CPU savings.

Final frozen PE32 SHA-256:
`16d0ad610afd6e9842ca4bccf676eb9050bdfa01d5d64c3df0709709745211dd`.
PE64: `e2d9ae0b3cd086cfc5c46a87e3d51e787b787a1b68abe4589393c4955c287a01`.
DLL sizes remain 1,905,664/1,771,520 bytes. Unix and both bridge DLLs are
byte-identical to H257. Rebuilding after the native-only negative controls
changed only timestamp bytes in the PE images; first-link images and final
images are both retained, and the final code gates were inspected again.

## Wine correctness

The final frozen images pass five actual Wine cases per PE width: vertex
constant, pixel shader get/set, pixel constant, shader constant Apply and
cross-stage constant StateBlock. All ten report no failure, skip, timeout or
missing case. `conformance-{x86,x64}-{36,68}` JSON/log/staged-build sidecars
retain exact selectors and images. X86 ran before the source commit, x64
after `c1368a6e`; the PE and provider hashes match in all four invocations.
This is getter/StateBlock evidence, not inline-constant draw-path execution.

## Sampler-OFF runtime spots

Committed `c1368a6e` uses Sikarugir cx-24.0.7, named GT2, 1024x768 for 05
and 1280x720 for 06. The perf/Warn recipe enables frame sampling and keeps
inline constants, segmented transport, index reorder, PE sampler/module map,
decimation/full recorder stats, owner phase split and copy ledger OFF.
Offload stays ON. A PID-bound process CPU/RSS observer polls every two seconds;
there is no concurrent build, test suite, other benchmark or GPU trace.
This is neither observer-free nor proof of host isolation.

| Product | Scene FPS | Rows / wall ms | CB / frame | Pass / frame | Draw / frame |
|---|---:|---:|---:|---:|---:|
| 05 GT2 | 29.682 | 1,918 / 64,617.525 | 4.001 | 15.785 | 1,681.726 |
| 06 GT2 | 18.194 | 1,165 / 64,033.464 | 4.002 | 19.863 | 1,880.198 |

05 includes all positive-wall frames 1–1,918; 06 includes positive-wall rows
with more than 100 draws, frames 990–2,154. Loading is excluded, not slow
scene frames. FPS comes from the frame log, not process-elapsed performance
or a decoded `.3dr`. All five staging receipts match the frozen images and
all eight retained chunk/GPU/pipeline/no-pipeline failure counters are zero.
05 exits normally; 06's permitted timeout closes its completed-scene result
GUI. Saved captures at 1:00.29 and 0:36.28 show forest rendering; neither
constitutes whole-scene pixel equivalence. Summed GPU CB times are
1.994/11.910 ms/frame, not GPU critical-path latency.

| Product | Partial CPU frame increments | Process CPU seconds / elapsed seconds | CPU ms / frame | Average cores | RSS peak KiB |
|---|---:|---:|---:|---:|---:|
| 05 GT2 | 1,826 | 128.10 / 60.284 | 70.153 | 2.125 | 3,233,272 |
| 06 GT2 | 1,111 | 106.02 / 60.292 | 95.428 | 1.758 | 3,298,592 |

These joins cover different partial intervals and all process threads, not
PE-exclusive CPU. `qualify_perf.py` and `perf-qualified.json` retain exact
arithmetic, populations, health and binary qualification.

H258's preceding default-OFF spots are 29.631 FPS for 05 and the requested
18.192 FPS repeat for 06. Current differences are +0.17%/+0.01%; approximate
process CPU/frame differences are -0.16%/-0.37%. These single sequential
spots have no controlled cooldown/order counterbalance and do not establish
gain, neutrality or reduced CPU consumption. H257's user-interfered 06 r1
remains excluded. Current 06 exceeds historical README 18.1 numerically;
05 remains 1.39% below 30.1. README's index-reorder-ON profile is a mismatched
reference, not a measured explanation for the residual. README is unchanged.

## Complete current PE profiles

Separate 250-Hz sampler runs use the same five binaries and recipe, except
for Info logging, module mapping and full self-PC output. Detailed PE clocks
remain OFF. Stopping the game thread and reporting buckets perturbs execution;
neither sampler FPS nor its frame timings are performance evidence.

| Product | Last reported Present | Thread samples | PE samples | Stored / printed buckets | Cross-symbol samples |
|---|---:|---:|---:|---:|---:|
| 05 GT2 | 1,860 | 13,563 | 1,430 | 614 / 614 | 367 |
| 06 GT2 | 2,100 | 14,089 | 854 | 481 / 481 | 214 |

Every cumulative group (31/35) passes conservation and completeness checks:
no omitted or overflow samples, allocation fallback, suspend/context/resume
failure or target-thread mismatch. Module probes pass and all eight renderer
failure counters remain zero. Last reports are cumulative, not sums. The
approximate 06 scene-prefix subtraction removes the last pre-heavy report at
Present 960, retaining 849 PE samples out of 13,353 thread samples in 477
buckets; the first heavy frame is 1,011. 05 has no earlier report to subtract.
These are not exact scene edges, and the final unreported Present tail remains
outside the profile. Collection completeness does not mean whole-run coverage.

The frozen PE32 symbol join retains all buckets, marking any bucket crossing
a symbol start as ambiguous. Interior padding, inlining and asynchronous Wine
PC mapping still limit attribution. Narrow unambiguous symbol counts below
exclude nested lambdas; they are not inclusive CPU times or a cost ranking.

| Linked symbol | 05 samples | 06 samples |
|---|---:|---:|
| `applyConstShadowLiveFixedWidth<16>` | 93 | 50 |
| `SetVertexShaderConstantF` | 45 | 26 |
| `acceptSparseStatePlan` | 31 | 19 |
| `buildSparseStatePlan` | 27 | 21 |
| `preparePeSemanticSparsePlanLayout` | 21 | 18 |
| `PeSemanticPreparedIdentityWitnessT<45>::add` | 18 | 14 |
| `accumulateRetentionDelta<45>` | 64 | 35 |
| `writeSparsePlanPayloadQualified` | 43 | 18 |

H257 had 48/21 narrow acceptance samples versus 31/19 here. Small stopped-PC
populations, shifted symbol boundaries and different execution intervals do
not establish settlement CPU savings. `profiles-qualified.json`, both full
CSVs/Markdown reports, symbol JSONs and scene-prefix reports retain this join.
The existing `symbolicate_xctrace_pe.py` plus H255's `join_full_selfpc.py`
and `sampler_phase_delta.py` produce those artifacts; `verify_profiles.py`
checks current images, profile, module containment and coverage.

## Decision and next bounded gate

The change removes demonstrated linked work without widening state ownership
or altering canonical bytes. Native negative controls, Wine getter/StateBlock
checks and fresh wild health pass; the four runtime runs close collection for
this implementation. Repeatable README recovery, exact PE cost attribution,
reduced process CPU and whole-scene pixel equivalence are not established.
No provider or experimental default changes follow.

Current PE32 and PE64 disassembly confirms the next local candidate:
`D3D9DeviceImpl::SetVertexShaderConstantF` validates count, pointer and extent,
then its outlined `applyConstShadowLiveFixedWidth<16>` repeats those guards.
The compiler has not removed them across the call. The four retained
`next-guard-{pe32,pe64}-{vsf-entry,live16}.txt` listings locate those blocks;
the earlier bounded SIMD compare/copy remains present.

A call-local validated range may avoid only the duplicate Live-kernel checks.
It must preserve `applyConstStateWriteImpl`'s write-allowed, zero-count and
Recording routing, exact call identity, checked generic reference, growth,
zero extension, dirty tracking and error precedence. First prove all six
families with bounded reference tests and actual x86/x64 getter/StateBlock
coverage; inspect both linked callers and bodies for new spills, stack or
call costs before measuring. This is a conditional next implementation, not
an expected FPS budget or permission to weaken public validation.
