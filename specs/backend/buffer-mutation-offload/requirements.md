---
type: requirements
title: Managed Buffer Mutation Offload — Requirements
description: Contracts for deferring Managed-pool unlock byte materialization to the replay offload worker (R-BACK-44.x).
tags: [backend, buffers, producer-concurrency, offload]
---

# Managed Buffer Mutation Offload — Requirements

Requirements for deferring the byte materialization of Managed-pool buffer
writable unlocks from the producer thread to the commit-replay offload
worker, while preserving the exact content each draw observes today.

Motivation and sizing: `docs/perfomance/present-pacing/present-pacing-bridge-crossing-decomposition.237.md`
(Managed rotation re-uploads own `1.19ms/present` of the GT2 producer wall,
`1.76ms/call` over two full-buffer copies plus rotation) and
`docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.38.md`
(mutation algebra, composition barriers, and the `>=0.5ms/Present` design
gate, which the #237 measurement meets).

## R-BACK-44.1 (Scope and mode)

The managed mutation offload is an opt-in provider mode selected by
`DXMT9_MANAGED_MUTATION_OFFLOAD` (default off; unset, empty, and `0` select
the current synchronous upload path byte-identically). It applies only to
writable unlocks of Managed-pool buffers whose pool record carries a
versioned backing (`hasVersionedBacking()`); every other unlock class
(Default dynamic, readonly, non-versioned) keeps its existing path. The
mode also requires the commit-replay offload worker
(`DXMT9_OFFLOAD_COMMIT_REPLAY`) to be active; with inline replay the mode
must resolve to off.

## R-BACK-44.2 (Synchronous unlock half)

In offload mode, the synchronous half of a Managed writable unlock must,
before returning to the application, in this order:

1. establish staging capacity and copy the exact dirty span
   (`lockedOffset_`, `lockedSize_` bytes of the core CPU `storage_`, after
   the wow64 shadow writeback) into task-owned storage — a staging or
   reservation failure is a pre-effect rejection that leaves every record,
   ring, and revision unchanged;
2. perform the logical backing rotation synchronously: backing selection
   under the existing R-BACK-5.8 / R-BACK-5.11 rules (idle-reuse check
   against `completedSeqId`, fresh allocation fallback, never blocking on
   GPU completion), `renameActiveIndex` / `record.buffer` /
   `record.contents` / `contentRevision` update under the buffer arena's
   unique lock;
3. enqueue the mutation task (canonical buffer identity, target backing
   generation, staged bytes, source ordinal) at its producer-order
   position in the replay FIFO and publish it against the buffer's
   resource-scoped drain target.

The unlock returns success only after all three steps complete
(`.38`'s "Unlock success may be returned only after immutable payload
capacity, resource retention, and any failure-visible reservation are
established").

## R-BACK-44.3 (Ordered application)

The offload worker must apply mutation tasks and replay chunks in one
strict FIFO order: a mutation task enqueued after chunk `A` and before
chunk `B` is applied after `A`'s replay completes and before `B`'s replay
begins. Application materializes the rotated backing: copy-forward of the
untouched region from the pool CPU shadow (which at application time holds
exactly the pre-mutation content by induction over this same order), the
dirty-span patch from the staged bytes, and then the pool shadow update.
V1 performs no coalescing, reordering, or elision of mutation tasks.

## R-BACK-44.4 (Snapshot and encode visibility)

Commit-time buffer binding capture continues to read the live pool record
synchronously at commit (`Pool::captureChunkBufferBinding`); because
R-BACK-44.2 rotates synchronously, a commit after the unlock captures the
post-rotation backing handle, contents address, and content revision. Any
consumer that reads buffer bytes on the replay/encode side (snapshot
`contentsAddress`, `record.shadow`, encode staging caches) for a chunk
enqueued after the mutation must observe the applied bytes; this is
discharged by R-BACK-44.3's ordering, not by any wait. Chunks enqueued
before the mutation captured the pre-rotation backing whose bytes the
mutation never touches (rotation preserves prior ring backings).

## R-BACK-44.5 (Direct-call reader fence)

A direct (non-chunk) unix call that reads live buffer record bytes —
shared-buffer export/alias, any future readback consumer — must wait for
pending mutation tasks on that buffer through the existing resource-scoped
replay ledger (R-BACK-2.51(d)(i)), whose `lastQueuedSeq` publication is
extended to mutation tasks by R-BACK-44.2(3). In offload mode the Managed
writable unlock itself no longer performs the R-BACK-2.51(d) pre-mutation
drain: the FIFO ordering of R-BACK-44.3 replaces the wait. Read locks of
Managed buffers remain served from the core CPU `storage_` and are
unaffected.

## R-BACK-44.6 (Synchronicity reclassification evidence)

Offload mode changes `dxmt9c_buffer_unlock`'s bridge-entry synchronicity
class for the Managed writable case (today `visibility-wait`). The change
must follow R-BACK-43.2's reclassification procedure with the full
R-BACK-43.6 evidence stack: a bounded model covering the new
producer/worker interleavings including at least one counterexample
configuration that demonstrates the guarded failure when the ordering
premise is removed, shared pure predicates binding the model to the
production code, and a native spec exercising the predicate boundaries.

## R-BACK-44.7 (Failure and teardown)

A worker-side application failure fail-stops the offload worker under the
existing poison discipline; it must never be reported as a recoverable
unlock result. Device Reset, buffer destroy, and device-lost teardown must
either apply or discard pending mutation tasks in FIFO order before the
targeted resource's backing is released; discard is permitted only on
paths where the corresponding draws are also discarded.

## R-BACK-44.8 (Promotion gates)

Default remains off until: the R-BACK-44.6 evidence stack is green; the
D3D9 conformance suite passes with the mode on; GT1/GT3/SFIV visual
anchors are clean; and a GT2 matched A/B shows the producer-wall reduction
with zero GPU errors and no locality regression. Rollback (`0`) must stay
byte-identical to the pre-mode path.
