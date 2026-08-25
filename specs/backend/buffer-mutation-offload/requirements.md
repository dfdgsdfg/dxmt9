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
**plain** writable unlocks of Managed-pool buffers whose pool record
carries a versioned backing (`hasVersionedBacking()`): locks that carried
`D3DLOCK_DISCARD` or `D3DLOCK_NOOVERWRITE` are excluded and keep the
synchronous path, because the PE hazard seal skips `NOOVERWRITE`
(`bufferLockRequiresHazardFlush`) and `DISCARD` zero-fills the whole core
`storage_` beyond the locked span — both break the staged-dirty-span
premise (2026-08-25 design review, findings 1 and 3). The excluded classes
match the measured population: the #237 Managed upload wall is `1,223`
plain locks. Every other unlock class (Default dynamic, readonly,
non-versioned) keeps its existing path. The mode also requires the
commit-replay offload worker (`DXMT9_OFFLOAD_COMMIT_REPLAY`) to be active;
with inline replay the mode must resolve to off.

## R-BACK-44.2 (Synchronous unlock half)

In offload mode, the synchronous half of a Managed writable unlock is a
transaction with this order (2026-08-25 review finding 4):

1. **reserve** queue admission — a FIFO ordinal and the staged-byte budget
   (which counts against the existing `DXMT9_OFFLOAD_QUEUE_CHUNKS` /
   `DXMT9_OFFLOAD_QUEUE_BYTES` bounds, review finding 9) — with no
   externally visible side effect; then copy the exact dirty span
   (`lockedOffset_`, `lockedSize_` bytes of the core CPU `storage_`, after
   the wow64 shadow writeback) into task-owned storage and take the record
   half of the R-BACK-44.2a lease (core-buffer retention + drain-target
   ownership); the concrete ring-entry half is produced by step 2's
   rotation — both halves are task-owned before step 3 commits;
2. perform the logical backing rotation synchronously: backing selection
   under the existing R-BACK-5.8 / R-BACK-5.11 rules (idle-reuse check
   against `completedSeqId`, fresh allocation fallback, never blocking on
   GPU completion), `renameActiveIndex` / `record.buffer` /
   `record.contents` / `contentRevision` update under the buffer arena's
   unique lock;
3. **commit** the reserved task (canonical buffer identity, leased target
   backing, staged bytes, source ordinal) at its reserved FIFO position
   and publish it against the buffer's resource-scoped drain target. The
   commit step must not be fallible: every fallible operation happens
   before step 2.

Failure of step 1 is a retryable pre-effect rejection: no rotation, no
revision bump, and — because lock-state clearing across every layer (PE
`lastLock*`, wow64 shadow lock state, core `locked_` metadata) is deferred
until after step 3 — the unlock remains retryable with all layers
consistent (review finding 8). A concurrent queue stop/poison observed at
step 1 rejects the unlock with the existing fail-stop disposition.

**R-BACK-44.2a (Task lease.)** The committed task owns, until application
or terminal discard: a retention on the core buffer, the concrete rename
ring entry it targets (backing handle, contents pointer, generation and
`contentRevision` at rotation), and a replay-residency lease equivalent to
the chunk capture's `backingResidency`. The worker applies to the leased
entry — never to the record's then-current active backing — and destroy or
GC of the record must respect the lease (review finding 5).

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

**R-BACK-44.4a (Encode-side reader precondition.)** R-BACK-44.3's FIFO
order covers replay; encode of an earlier chunk may run after a later
mutation is applied. Therefore, before offload mode can be enabled, every
encode-side byte consumer of a versioned buffer record must source bytes
from the captured snapshot (or be keyed by the captured backing and
`contentRevision`), never from the live `record.shadow` / `record.contents`
(review finding 2). The known violator is the encode index staging path:
`StreamIbStagingCache::findOrStage` is called for index buffers without
the `!indexSnapshot` guard the vertex-stream path has
(`dxmt9_draw_encoder_draw.mm`), and diagnostic index readers read
`indexRecord->shadow` directly. This is a **pre-existing latent race** in
the synchronous path as well — the R-BACK-2.51(d) unlock drain waits on
`lastReplayedSeq`, which does not cover encode-time reads — and must be
fixed as an independent correctness change before this mode, not as part
of it.

## R-BACK-44.5 (Direct-call reader fence)

A direct (non-chunk) unix call that reads live buffer record bytes —
shared-buffer export/alias, any future readback consumer — must wait for
pending mutation tasks on that buffer through the existing resource-scoped
replay ledger (R-BACK-2.51(d)(i)), whose `lastQueuedSeq` publication is
extended to mutation tasks by R-BACK-44.2(3). Read locks of Managed
buffers remain served from the core CPU `storage_` and are unaffected.
In offload mode the Managed `NOOVERWRITE` unlock's unconditional replay
bypass is additionally conditional on the buffer's drain target having no
pending work: a Managed `NOOVERWRITE` unlock performs a full synchronous
upload (the exact-range path is Default-only), which would otherwise race
a queued mutation task and lose the ordering argument. Mode-off keeps the
bypass byte-identical. (Managed + `D3DUSAGE_DYNAMIC` is an app contract
violation, but it must not corrupt.)

In offload mode the Managed plain writable unlock itself no longer
performs the R-BACK-2.51(d) pre-mutation drain. This is not a local
exception: adopting this mode **amends R-BACK-2.51(d) with a fourth
admission form** — "(iv) a Managed plain writable unlock in offload mode
may substitute ordered FIFO-mutation admission (R-BACK-44.2's
reserve/rotate/commit transaction) for the pre-mutation wait" — and the
amendment must land in `specs/backend/requirements.md` R-BACK-2.51 itself
in the same change that implements the mode (review finding 6).

## R-BACK-44.6 (Synchronicity reclassification evidence)

`dxmt9c_buffer_unlock` retains its entry-wide `visibility-wait`
classification in the R-BACK-43.1 table — the closed one-class-per-entry
format is not extended, and the entry still returns the staging /
rotation / admission acknowledgement so it can never become `record-only`
(review finding 7). The offload mode is documented as a mode-conditional
non-waiting `state-mutation-ack` subpath under that ceiling, in the
producer-concurrency spec's §4 ordering protocols. The behavioral change
must still follow R-BACK-43.2's procedure with the full R-BACK-43.6
evidence stack: a bounded model covering the new producer/worker
interleavings including at least one counterexample configuration that
demonstrates the guarded failure when the ordering premise is removed,
shared pure predicates binding the model to the production code, and a
native spec exercising the predicate boundaries.

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
