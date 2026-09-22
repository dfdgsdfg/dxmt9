---
type: "Spec Design"
title: "PreparedEncodePlan foundation and Replay ownership boundary"
description: "Source-stable preparation, reuse of existing Replay planning, and a bounded publication contract before runtime integration."
tags: [specs, backend, replay, encode, ownership, verification]
---

# PreparedEncodePlan foundation

Implements the value-layer foundation of `R-BACK-2.71`. The builder and pure
publication reducer live in `src/dxmt9/dxmt9_prepared_encode_plan.hpp`. An opt-in
ordinary publication shadow now exercises the builder; no Encode caller consumes
it and no plan is published asynchronously. A sealed plan is data,
not a parallel execution certificate. Actual joint publication remains the
next integration step.

## Publication shadow cost probe

`prepareSlotForPublish` in `src/dxmt9/dxmt9_command_queue.cpp` has a cached
default-OFF `DXMT9_CPU_SUPPLY_PREPARED_OBSERVER` gate. Inside the bounded
CPU-supply window it builds and destroys a private plan from the final ordinary
Slot, with the actual source/storage generations and sequence. There is no
plan field in the Slot, no async borrow, and no execution-policy change.

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
attempt, not RSS. Failed-plan cardinalities are unavailable. A successful
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
allocation. Initial ceilings are 256 canonical identities, 1024 command rows,
8192 access indices and 1024 candidate ranges. They are testable admission
limits, not workload maxima or changes to production pass capacity. The
source-wide 256 bound is independently conservative; a future production
budget must not assume that H347's per-pass cardinality is source-wide.
The initial fixed storage is 149,576 bytes on the tested 64-bit builds. This
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
Two children model prepared execution; one completion models serial execution.
`ReleasePlan` requires join, while source retirement additionally requires
submission completion. GPU completion events are trusted environment inputs
in this value model, not a synthesized GPU signal. An encoded failure remains
a failure even after its submitted work reaches completion.

This reducer does not perform atomic memory publication, retain a real Slot,
or implement cancellation/terminal queue recovery. The production adapter must
consume actual assembler/lease evidence, publish under the existing queue
lock or release/acquire protocol, keep owned storage through join, and bind
completion to actual submitted identity. Those obligations remain open.

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

No runtime behavior or default policy changes. Production shadow comparison,
non-degenerate GPU exact qualification and end-to-end timing belong to the
subsequent Replay/Encode integration stages.
