---
type: "Spec Design"
title: "PreparedEncodePlan foundation and Replay ownership boundary"
description: "Source-stable preparation, reuse of existing Replay planning, and a bounded publication contract before runtime integration."
tags: [specs, backend, replay, encode, ownership, verification]
---

# PreparedEncodePlan foundation

Implements the value-layer foundation of `R-BACK-2.71`. The builder and pure
publication reducer live in `src/dxmt9/dxmt9_prepared_encode_plan.hpp`; no live
Replay, Slot, queue or Encode call site uses them yet. A sealed plan is data,
not a parallel execution certificate. Actual joint publication remains the
next integration step.

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

The foundation owns one fixed storage allocation per builder, with no per-draw
allocation. Initial ceilings are 256 canonical identities, 1024 command rows,
8192 access indices and 1024 candidate ranges. They are testable admission
limits, not workload maxima or changes to production pass capacity. The
source-wide 256 bound is independently conservative; a future production
budget must not assume that H347's per-pass cardinality is source-wide.
The initial fixed storage is 149,576 bytes on the tested 64-bit builds. This
is an unconnected foundation, not a priced hot-path allocation policy. Before
runtime integration, account for this maximum allocation and initialization;
pooling or exact-sized regions must retain the same failure/publication contract.

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
failure discards the complete unpublished allocation. A failed builder cannot
seal or resume; successful `seal` transfers storage exactly once. Destruction
rolls back any unsealed allocation. `seal` does not publish the associated
Slot, acquire resource ownership, or provide a worker lifetime token.

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
