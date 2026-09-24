---
type: "Spec Design"
title: "PreparedEncodePlan foundation and Replay ownership boundary"
description: "Source-stable preparation, reuse of existing Replay planning, and a bounded publication contract before runtime integration."
tags: [specs, backend, replay, encode, ownership, verification]
---

# PreparedEncodePlan foundation

## First serial consumer: immutable read-hazard projection

The first production-shaped consumer of `R-BACK-2.71` replaces only the
DrawRun read-hazard reconstruction in serial Encode. It must preserve the
reference's full raw-handle read set (index buffer, every stream, every texture,
including inactive entries), exact overlap and Bloom bits. Canonical alias
projection is not interchangeable with that reference. The serial lane now
builds only raw reads through `makeDrawEntryReadSet`; it performs no canonical
alias projection. The general foundation's canonical rows remain eligible for
reference comparison only when their sequence equals the raw sequence.
Attachment hazards, pass actions, alias/backing resolution, resource pins,
GPU-use stamps, query scopes and execution order remain unchanged.

Prepared serial reads are the ordinary-Slot default. The former
`DXMT9_PREPARED_SERIAL_READS` selector is retired and ignored, including `0`. Final
ordinary Slot publication, under the existing queue mutex and after resource
marking, owns the optional immutable plan. The Slot retains it until reclaim;
Encode borrows it synchronously under the existing source residency protocol.
No diagnostic thread-local workspace may recycle a published region. The owner
is an immutable shared handle to break the backend-type dependency cycle;
Encode performs no shared-handle copies or reference-count increments per draw.
The demand-specific `PreparedSerialReads` allocates an owner/control block and
two bounded vectors once per candidate Slot: command rows and raw handles.
Rows reserve the source command count; handles reserve the smaller of the
source command count times the maximum draw read count and 8192. Only live
entries are initialized. No per-draw allocation, canonical dictionary, write
set, attachment range, global identity dedup or whole-plan seal scan is needed.
The 1024-command/8192-handle bounds reject the complete unpublished candidate,
never truncate it. The incremental builder appends the command view already
projected by `markSlotResourcesImpl`, after each command's captured stamp batch
is flushed. This removes the independent publication command walk, not the
per-command raw read-set extraction. It is not fused Slot emission or pooled
storage. The ineligible-source specialization has an empty callback and no builder.

`PreparedSerialReadsBuilder` owns the mutable unpublished prefix. Append must
match the next original command index; ordering or capacity failure is sticky
and discards the prefix. Seal requires complete coverage, consumes the builder
and returns only immutable ownership. Failure must not short-circuit resource
marking: every remaining command and snapshot stamp is visited normally.
No view, builder or callback escapes the synchronous queue-mutex scope.

The diagnostic `prepared_build_and_mark` scope now includes allocation,
the fused resource-mark/append loop and seal; `resource_mark` includes append
for eligible sources. Neither is comparable to H358's isolated build
timer. Use the unchanged parent prepare-slot scope for cost comparison;
do not subtract overlapping medians or add nested timings.

Admission must match the complete source/storage/sequence key and whole-source
command count. Lookup uses original command indices, never replay ordinals, so
reordered/subset serial traversal cannot select another command's row. Arena,
parallel execution, missing plan, stale identity, unsupported or unresolved row,
allocation failure and bounded capacity rejection use the original read-hazard
calculation before that draw's effects. No executed command is retried.
Terminal discard and reclaim release the Slot owner through normal destruction
or `clearCommands`; retaining CPU metadata does not extend or replace GPU pins.

Verification must compare the actual consumer's full exact list and Bloom bits
with the independent existing helper, exercise every key mutation, raw alias
identity preservation, clear/reuse and allocation/capacity failure, and preserve the existing
publication reducer's no-release-before-join contract. Same-capture GPU comparison
must prove nonzero prepared-row consumption, not merely unchanged pixels through
fallback. H356–H359 supply bounded GT2 exact-pixel evidence. No FPS
gain is implied by moving work onto the publication thread.

Implements the value-layer foundation of `R-BACK-2.71`. The builder and pure
publication reducer live in `src/dxmt9/dxmt9_prepared_encode_plan.hpp`. The
diagnostic shadow retains the general canonical projection; the default
serial consumer uses only the raw read projection in
`src/dxmt9/dxmt9_prepared_serial_reads.hpp`. Both reuse the existing Replay
read-set transform. Neither serial metadata nor a sealed general plan is a
parallel execution certificate.
General prepared scheduling and parallel consumption remain subsequent work.

## Publication shadow cost probe

`prepareSlotForPublish` in `src/dxmt9/dxmt9_command_queue.cpp` has a cached
default-OFF `DXMT9_CPU_SUPPLY_PREPARED_OBSERVER` gate. Inside the bounded
CPU-supply window it builds and destroys a private plan from the final ordinary
Slot, with the actual source/storage generations and sequence. There is no
async borrow or execution-policy change from this observer. The separate serial
consumer owns the optional Slot field; the observer never populates it.

Before allocating, `screenPreparedEncodeShadow` skips frozen nonempty query
scopes, empty sources, and single commands with no possible two-draw child
work. A single DrawRun with multiple draws remains a candidate: the existing
parallel producer can subdivide that one command. Screening acceptance is not
parallel admission. Skipped sources retain identity and total-cost events but
have no allocation, projection or seal events; their failure/cardinality fields
are unavailable, not fabricated successful plans. The analyzer also accepts
historical complete transcripts without a screen event.

Verification classification: diagnostic-only read-side preparation selection;
no resource lifetime, command order, publication or execution transition changes.
The native truth table binds the actual screening predicate, and live queue
OFF/ON fixtures compare publication sequence and command-count evidence. The
existing TLA regression is a regression check, not a new async-refinement or
GPU-equivalence proof. Future plan consumption still needs those proofs.

The shadow is intentionally a second command walk under the existing queue
mutex. It measures allocation/initialization, command/access projection,
seal/structural validation, and total including destruction/observer events.
It is not the eventual fused Replay implementation. Draw alias resolution is
synchronous only; non-draw commands or incomplete access require Encode
resolution and cannot certify a worker. Arena-only publications are not covered.

`scripts/tools/analyze_prepared_encode_shadow.py` qualifies exact source event
coverage, native thread CPU and monotonic wall endpoints, rejects clipped or
duplicate records, and retains rejected attempts. Allocation bytes are per
attempt, not RSS. Failed-plan cardinalities are unavailable in the ordinary
plan fields. Capacity rejections additionally invoke
`measurePreparedShadowCardinality`, sharing the exact diagnostic projection
visitor but walking the complete source without the dictionary admission bound.
The cold collector sorts full-width canonical read/write handles and reports
their exact union, access occurrences, visited commands and unresolved rows.
Non-draw/incomplete rows remain unresolved: this is the projected draw-resource
union, not proof of all source resource accesses. Allocation/source failure
discards all partial counts. The analyzer requires complete identity-qualified
cardinality endpoints and full command coverage; historical missing events
remain unavailable. The separate cardinality CPU interval is included in total
observer time and must not be mistaken for normal builder/Encode cost.
A successful
structural plan does not establish parallel eligibility or saved Encode work.

The native `dxmt9-prepared-encode-plan-spec --publication-shadow-probe` invokes
actual queue submission/publication without workers or a GPU: 18 sources of
64 resource-empty DrawRun commands, plus three publication fixtures covering a
single Clear, a single-draw DrawRun and a nonempty query scope. OFF/ON tests
require identical publication sequences and command counts, 18 preparation
attempts and three skips. Use the first two candidate sources as shadow warm-up
and the remaining 16 as a synthetic connection/cost probe. It does not model
STALKER resource cardinality, alias lookup contention, execution, or FPS.
Real-workload qualification must additionally compare identical input/binary
observer-OFF and ON runs and independently check the original Encode plan.

## Reuse and placement audit

| Existing owner / symbol | Reuse | Must not infer |
|---|---|---|
| `src/d3d9/device_c_cpu_ready_plan.hpp`, `ReplayEmissionPlan` / `RecordCapacityDelta` | Raw record routing, Direct islands, state runs, coordinator locators, ordered-control cuts, lease spans and exact Slot capacity | Wire record ordinal is not final Metal command ordinal; state absorption and draw grouping change that mapping |
| `src/d3d9/device_c_cpu_ready_plan.cpp`, `planReplayEmissionRange` | Existing range/lease authority; do not introduce a second wire parser or capacity pass | A structurally partitioned raw is not a sealed render pass or parallel child plan |
| `src/dxmt9/dxmt9_source_payload.hpp`, `TransactionalChunkSlotAssembler::OuterBinding`, `prepare`, `bindCommitEvidence`, `commit` | Future attachment seam for plan storage: bind reserved source/storage identity and finalize alongside actual command emission | The new builder's `seal` does not mint `ArenaCommitEvidence` or make a Slot visible |
| `src/dxmt9/dxmt9_source_semantics.hpp`, `makeDrawEntryReadSet`, `makeRenderAttachmentWriteSet` | Reuse access extraction from final draw state, once available during Slot construction | Small entry sets alone do not cover the whole logical pass |
| `src/dxmt9/dxmt9_parallel_render_pass.hpp`, `classifyParallelPassCommandRole` | The builder directly calls the existing final-command role classifier | A draw candidate range does not prove pass actions, child binding, or economic eligibility |
| `src/dxmt9/dxmt9_draw_encoder_chunk.mm`, `resolveParallelPassResourceIdentity`, `produceSealedParallelPassSnapshots` call site | Independent current reference and execution-time admission boundary | Replay cannot pre-authorize current session, capture, initializer or query conditions |

The initial builder accepts consecutive final Slot commands, not imported wire
records or an arbitrary reordered/DCE stream. The future adapter must append
only after actual final command identity is known, or carry an explicit remap
and validate it before consumption. It must not duplicate `ReplayEmissionPlan`
or assume one wire record becomes one final command. Same-attachment DrawRun
ranges are structural candidates; every non-draw command remains a singleton
boundary, including partial Clear. Final pass/action and child subdivision
remain Encode-owned.

## Alias and backing stability

`Pool::createSurfaceForTexture` in `src/dxmt9/dxmt9_resource_pool.cpp` sets
`SurfaceRecord::aliasTexture` at creation. The current Encode resolver maps a
surface handle to that texture handle. The observed creation assignment does
not by itself prove both objects remain lookup-valid across an asynchronous
Replay/Encode handoff: owner retention, generation and backing relationships
must be qualified by the future adapter. Shared imported surfaces also require
separate scrutiny; they do not get an `aliasTexture` from this creation path.

`Pool` uses kind-tagged `HandleArena` instances for Buffer/Texture/Surface.
The table preserves full 64-bit canonical handle values, not low resource
indices. Logical handle identity is not physical backing identity: buffer
DISCARD/rename can rotate backing under one handle. Reuse the existing
`captureChunkBufferBinding`, backing residency and `markBufferSnapshotUse`
contracts instead of substituting a logical handle for a backing pin. This
plan neither stores Metal pointers nor replaces those contracts.

Inputs distinguish `CanonicalStable` from `RequiresEncodeResolution`.
`CanonicalStable` is a caller-supplied fact, not a lifetime proof minted by the
builder; a production owner that can substantiate it does not exist yet for
this new path. Unresolved commands retain their exact command position and
explicit disposition, with no guessed canonical access entries. Encode must
re-resolve them or reject the prepared lane. Unknown/malformed input rejects
the unpublished plan; incomplete source access is never silently declared safe.

## Bounded storage and sealing

The foundation owns one fixed storage region per active builder, with no per-draw
allocation. Current ceilings are 512 canonical identities, 1024 command rows,
8192 access indices and 1024 candidate ranges. They are testable admission
limits, not workload maxima or changes to production pass capacity. The
source-wide bound is independently conservative; a future production
budget must not assume that H347's per-pass cardinality is source-wide.
The fixed storage is 168,008 bytes on the tested 64-bit builds (151,624 before
the serial read Bloom projection, 149,576 before the identity increase), plus
2 KiB builder-only lookup scratch. This
is a fixed foundation, not a qualified hot-path allocation policy. Before
runtime integration, account for this maximum allocation and initialization;
pooling or exact-sized regions must retain the same failure/publication contract.

The ordinary diagnostic shadow now uses `PreparedEncodePlanWorkspace`: one
cold thread-local reusable allocation, created only for the first candidate.
It caches bytes, never source semantics or resource ownership. Acquire moves
exclusive storage into the builder and resets key, counts and resolution flag;
append overwrites every live array element before counted access. Array tails
are not read or cleared. Recycling consumes the sealed owner or failed builder
after synchronous views expire. No object retains a workspace back-pointer, so
an active owner may outlive its workspace. At most one idle region is retained
per observing thread until thread exit. This is not async Slot-plan pooling.

`prepared_allocated` reports new bytes (zero on reuse), while
`prepared_storage_capacity` reports available private bytes, including reused
storage retained after an early rejection. Historical traces lack the latter
and cannot establish reuse. Allocation phase now includes acquire/reset; append
starts after capacity metadata. Total includes recycling, not a per-source free.

Each access row owns read/write offsets and counts into a `uint16_t` index
array. The dictionary, commands, ranges and source/storage/sequence key are
flat value data. The heap owner is move-only and exposes only const data after
sealing; no borrowed span, callback or pointer is persisted in the payload.
Bounds, full identity equality, command order, exact range coverage and
offset closure are checked. Unused dictionary entries, duplicate access entries,
truncation and malformed offsets reject. Alias intersections are preserved
as data for final admission, not elided by the builder.

The single allocation point is injectable at construction for deterministic
failure tests; the allocator is not retained. Any append/coverage/capacity
failure invalidates the complete unpublished prefix. The ordinary builder frees
it immediately; a workspace builder can privately retain the bytes for recycling.
A failed builder cannot seal or resume; successful `seal` transfers storage
exactly once. Destruction frees any unrecycled allocation. `seal` does not publish the associated
Slot, acquire resource ownership, or provide a worker lifetime token.

Reuse is checked by an exhaustive 125-history native differential against fresh
builders over canonical, unresolved, partial-capacity, incomplete and invalid-key
outcomes. Tests also cover allocation failure, stale generation, consumed owners
and workspace destruction before sealed-owner destruction. This is synchronous
storage refinement only; the publication TLA model still does not certify an
async adapter. Live OFF/ON queue probes require one allocation plus 17 reuses
for their 18 candidates without changing publications or skip accounting.

## Measured identity-table implementation

H353 measures 21 complete rejected-source projections at 412–429 canonical
identities, 2357–2477 accesses and 263–275 commands, with zero unresolved rows.
The source dictionary now holds 512 full-width identities and unchanged
`uint16_t` access indices; exceeding the bound must still reject atomically.
The builder uses 1024 private open-address slots with index+1 sentinels, full-key
collision equality, deterministic first-occurrence dense indices and bounded
probing. This costs 4 KiB for the immutable dictionary and 2 KiB for construction
lookup, rather than copying keys into a node-based map. Do not increase unrelated
command/access/range limits or persist construction lookup in the Encode payload.
Reusable storage and failed-prefix invalidation are retained. Shared capacity
constants drive limit validation, physical identity storage and coverage arrays.
The lookup resets on each builder construction, including workspace acquire,
and does not follow the sealed owner. Standalone closure validation deliberately
retains its independent duplicate scan; faster construction is not evidence that
seal or the whole pipeline is faster.

This is a bounded preparation implementation, not a production promotion. Existing
pass/certificate limits remain separate. Native tests cover 256/257/512/513
entries, 513 full-generation keys colliding into the last bucket, probe wrap,
read/write alias reuse, deterministic dense ordering and failed-prefix reuse.
Fixed-input builder timing remains open; joint async
publication and independent Encode/GPU validation still precede consumption.
Evidence and exclusions: [H353](../../../docs/perfomance/present-pacing/present-pacing-prepared-cardinality.353.md).

## Publication value model

`reducePreparedPublication` defines the future transaction adapter contract:

```text
Preparing -- SlotReady + PlanReady --> Published(prepared)
Preparing -- SlotReady + PlanFailed -> Published(serial)
Preparing --------------------------> Aborted
Published -- final admission denial -> Published(serial)
Published -> Encoding -> child completion(s) -> Joined
Joined -> Submitted -> Completed -> Retired
```

Every event is qualified by all source/storage/sequence/range key fields.
Only `Preparing` can publish, and it needs a complete Slot plus either a
complete plan or an explicit preparation failure. Post-effect failure remains
sticky and drains child ownership; it cannot retry through serial execution.
Two children model parallel prepared execution. `SelectPreparedSerial` retains
the plan but requires only one consumer completion; `SelectSerial` discards
prepared authority and models reference fallback.
`ReleasePlan` requires join, while source retirement additionally requires
submission completion. GPU completion events are trusted environment inputs
in this value model, not a synthesized GPU signal. An encoded failure remains
a failure even after its submitted work reaches completion.

This reducer does not perform atomic memory publication or implement terminal
queue recovery. The default serial adapter uses the existing ordinary Slot
queue-lock publication and residency/reclaim protocol, retaining plan ownership
until Slot clear/destruction. Its consumer uses the complete source key from
that protocol. It does not introduce a new queue state machine. General Arena
publication and parallel consumer/recovery binding remain open.

## Verification mapping

`dxmt9-prepared-encode-plan-spec` checks the actual builder against independent
source-derived access/range sets over all 1024 five-command traces. It covers
the single allocation failure, every capacity dimension, exact capacities,
partial prefix disposal, missing/duplicate command positions, unknown kinds,
unresolved aliases, malformed offsets, full generations and moved/sealed
ownership.

The same native target exhaustively explores the pure reducer's finite state
graph (two workers), with invariants checked on every accepted transition and
every rejected transition required to preserve state. Every key-field mutation
is rejected for every state/action. It also checks terminal reachability from
every state when the environment makes progress, and explicit partial-publish,
duplicate-publish, early-reclaim and post-effect retry counterexamples. This is
an executable bounded state checker over the shared C++ transition, not a
separately reimplemented test policy. It does not prove scheduler fairness,
C++ memory ordering, actual queue binding or GPU bytes. Existing TLA regression
is complementary; no new TLA model is claimed for this foundation.

Current evidence: native focused 4/4 and the x86_64/Rosetta foundation test pass;
the reducer graph has 61 reachable states and 75 accepted edges. Existing TLA
regression passes in 195.96 seconds. Logs are preserved under
`experiments/output/prepared-encode-plan-foundation/{native,x86,tla}.log`.

The serial consumer is default following the explicit user promotion decision.
The selectable reference-only lane and runtime full-plan consumer are removed.
Allocation/capacity rejection, stale identity, Arena and non-identity execution
retain the necessary raw-hazard fallback; it is not a selectable legacy policy.
Parallel policies are unchanged. Broader wild qualification and end-to-end
performance remain evidence gaps, not claims implied by this promotion.
