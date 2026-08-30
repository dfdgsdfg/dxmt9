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

The managed mutation offload is a provider mode selected by
`DXMT9_MANAGED_MUTATION_OFFLOAD` (**engine default ON since 2026-08-25**,
after the R-BACK-44.8 gates passed; explicit `0` selects the synchronous
upload path byte-identically as the supported rollback lane). It applies only to
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
(review finding 2). At design time the encode index staging path and diagnostic
index readers violated this rule. The prerequisite is now implemented; its
exact snapshot-presence fix and evidence are recorded in `gap.md`. The
underlying rule remains normative because R-BACK-2.51(d)'s unlock drain waits
on `lastReplayedSeq`, which does not cover encode-time reads.

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

Promotion required, and on 2026-08-25 received: the R-BACK-44.6 evidence
stack green (`BufferMutationOffload.tla` + counterexamples, predicates,
native specs); the D3D9 conformance suite showing zero mode-caused delta (ON and OFF fail-identical at HEAD; the shared failures are owned by concurrent WSI work);
GT1/GT3/SFIV visual anchors clean with the mechanism active; and a GT2
matched A/B showing the producer-wall reduction (harmonic
`27.605 -> 28.864`, `+4.6%`; managed full uploads `1,227 -> 0`; exact
task conservation) with zero GPU errors and conserved CB/render-pass/
sub-CB locality shape. The engine default flipped on with that evidence;
rollback (`0`) must stay byte-identical to the pre-mode path.

## R-BACK-44.9 (Composition observer)

Any mutation-composition work must begin with a cold observation-only ledger;
the current one-task-per-Unlock FIFO behavior remains authoritative. For every
successful writable Unlock, the observer must bind the production resource
identity, backing generation, disposition, exact range and bytes, source
ordinal, failure/completion disposition, and first following GPU use or CPU
observer. It must report zero-use generations, `DISCARD -> DISCARD` chains,
conservatively mergeable `NOOVERWRITE` ranges and union/overlap bytes, a typed
rejection reason for every adjacent non-candidate, and the measured CPU-time
split named in the `.38` experiment. Totals must include candidate calls,
candidate bytes, and attributable time, not call counts alone. The observer
must share the production generation and barrier classifiers, retain no
payload, and obey the disabled-path contract in R-ARCH-7.7.

Its composition ordering identity is observer-owned and typed as
`(ordering-generation, ordering-ordinal, source-kind)`. Every accepted mutation
and use receives the next ordinal from one fixed-width policy, regardless of
whether its retained production `sourceOrdinal` came from synchronous CPU work
or deferred/replay `replaySeq`. Reset advances the ordering generation before
restarting ordinals; generation mismatch, policy exhaustion, and unsupported
reentry are not comparable and must reject closed. The retained production
ordinal remains available for exact deferred settlement and source-facing
diagnostics, but must never be compared directly with the observer ordering
ordinal.

## R-BACK-44.10 (Decision gate)

No mutation stream, merge, elision, delayed Unlock, or changed acknowledgement
may be designed from aggregate Lock/Unlock counts or byte totals. A matched wild
observer run must show at least `0.5ms/Present` of conservatively composable
Unlock time before a composition design may open. Below `0.2ms/Present`, the
lane closes. Between those thresholds, evidence is inconclusive and the
transport remains unchanged. The gate must be evaluated separately for each
workload and mutation class; unrelated bridge residence cannot be added to it.

## R-BACK-44.11 (Composition proof and promotion)

If R-BACK-44.10 opens the lane, composition requires a separate requirements
and design change. It must define the byte algebra and every barrier for exact
resource/backing generation, latest-preceding-generation draw visibility,
read-Lock and ordered-control visibility, `DISCARD` freshness,
`NOOVERWRITE` in-flight safety, HRESULT/Unlock acknowledgement, failure order,
capture identity, Reset/destroy/device-lost, and cross-thread observation.
Promotion then requires a counterexample-backed formal refinement, native
legacy/composed byte and observer equivalence, Render Tape replay equivalence,
GPU readback/visual/validation evidence, bounded Wine faults, and matched wild
performance/locality evidence. Until all of those pass, speculative merging is
forbidden and R-BACK-44.3's V1 FIFO task-per-Unlock rule is unchanged.
