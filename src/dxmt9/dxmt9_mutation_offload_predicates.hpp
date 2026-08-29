#pragma once

// Shared pure predicates for the Managed buffer mutation offload protocol
// (R-BACK-44.1 / 44.2 / 44.3 / 44.4 / 44.5).
//
// These are the C++ half of the vocabulary that
// `specs/verification/tla/BufferMutationOffload.tla` uses for its transitions.
// The production code, the model, and
// `tests/native/backend/buffer_mutation_offload_spec.cpp` all name the same
// predicates, so a TLC counterexample trace is mechanically translatable into
// a deterministic native step sequence instead of being hand-reinterpreted.
// This follows the binding pattern `dxmt9_mark_reclaim_predicates.hpp` /
// `dxmt9-producer-mark-reclaim-spec` established for R-BACK-43.6, which
// R-BACK-44.6 requires this mode to repeat.
//
// Every function is a `constexpr` value transform with no dependency on Wine,
// Metal, the buffer arena, or the offload queue — which is the point: the
// admission decision must be identical whether it is evaluated on the producer
// thread inside `dxmt9c_buffer_unlock`, on the offload worker, or in a test
// with no device at all.
//
// The mode itself is not implemented yet (`specs/backend/buffer-mutation-
// offload/gap.md`); this header is step 1 of its evidence stack, so it
// currently has no production caller.

#include <cstdint>

#include "dxmt9/mutation_identity.hpp"

namespace dxmt9::resources::mutation_offload {

// `D3DLOCK_*` bits, mirrored from `src/d3d9/d3d9_pe_buffer_hazard.hpp` rather
// than pulled in from `d3d9types.h`, so this header stays free of the Windows
// SDK and usable from unix-side and test translation units alike. The three
// that matter to admission are exactly the three
// `bufferLockRequiresHazardFlush` distinguishes.
inline constexpr std::uint32_t kLockReadOnly = 0x00000010u;
inline constexpr std::uint32_t kLockNoOverwrite = 0x00001000u;
inline constexpr std::uint32_t kLockDiscard = 0x00002000u;

// `D3DPOOL_MANAGED`. Mirrored for the same reason as the lock bits.
inline constexpr std::uint32_t kPoolManaged = 1u;

// A FIFO ordinal of zero means "holds no reservation": either the task has not
// reached R-BACK-44.2 step 1 yet, or its reservation was released by the
// pre-effect rejection path. TLA+: the `# 0` conjunct of
// `BufferMutationOffload!FifoOrdinalPrecedes`.
inline constexpr std::uint64_t kUnreservedOrdinal = 0u;

// TLA+: the lock-class half of `BufferMutationOffload!Reserve`'s enabling
// condition — R-BACK-44.1's "plain writable unlock".
//
// A plain writable lock is one that carries none of READONLY / NOOVERWRITE /
// DISCARD. The three exclusions are not stylistic:
//   * READONLY never mutates, so there is nothing to offload;
//   * NOOVERWRITE is skipped by the PE hazard seal
//     (`bufferLockRequiresHazardFlush` returns false), so pre-mutation draws
//     are not guaranteed to have captured the pre-rotation backing — the
//     premise the whole ordering argument rests on;
//   * DISCARD zero-fills the core `storage_` beyond the locked span, so the
//     staged dirty span is not the whole of the change.
// (2026-08-25 design review, findings 1 and 3.)
inline constexpr bool isPlainWritableLock(std::uint32_t lockFlags) noexcept {
  return (lockFlags & (kLockReadOnly | kLockNoOverwrite | kLockDiscard)) == 0u;
}

// TLA+: `BufferMutationOffload!Reserve`'s enabling condition in full —
// R-BACK-44.1's scope gate.
//
// All five conjuncts are load-bearing, and the two mode conjuncts come first
// because they are the rollback contract: with the mode off, or with inline
// replay selected (`DXMT9_OFFLOAD_COMMIT_REPLAY=0`), there is no worker to
// carry the task and the unlock must stay byte-identical to the synchronous
// path. `hasVersionedBacking` is the pool record's own rename-ring test: a
// non-versioned record has no second entry to rotate to, so R-BACK-44.2 step 2
// has nothing to do.
inline constexpr bool admitsManagedMutationOffload(
    bool modeEnabled, bool offloadReplayActive, std::uint32_t pool,
    std::uint32_t lockFlags, bool hasVersionedBacking) noexcept {
  return modeEnabled && offloadReplayActive && pool == kPoolManaged &&
         hasVersionedBacking && isPlainWritableLock(lockFlags);
}

// TLA+: `BufferMutationOffload!FifoOrdinalPrecedes`.
//
// Strict, and false whenever either side holds no reservation: an item with no
// position in the queue can impose no order, and must not be mistaken for one
// that sits at the head. Reserving is what fixes the position (R-BACK-44.2
// step 1), which is why the ordinal is taken before anything visible happens.
inline constexpr bool fifoOrdinalPrecedes(std::uint64_t earlier,
                                          std::uint64_t later) noexcept {
  return earlier != kUnreservedOrdinal && later != kUnreservedOrdinal &&
         earlier < later;
}

// TLA+: `BufferMutationOffload!MutationBlocksChunkReplay` — the `ReplayChunk`
// half of R-BACK-44.3's ordering premise.
//
// True when the worker must apply this mutation task before it may replay this
// chunk. Deleting this predicate from the worker loop is exactly what
// `BufferMutationOffload.counterexample.cfg` models, and the model reports the
// damage as `EncodeReadsAppliedBytes`: the chunk is encoded reading a backing
// that still holds pre-mutation bytes.
inline constexpr bool mutationBlocksChunkReplay(std::uint64_t mutationOrdinal,
                                                bool mutationApplied,
                                                std::uint64_t chunkOrdinal) noexcept {
  return fifoOrdinalPrecedes(mutationOrdinal, chunkOrdinal) && !mutationApplied;
}

// TLA+: `BufferMutationOffload!ChunkBlocksMutationApply` — the `ApplyMutation`
// half of the same premise.
//
// True when the worker must replay this chunk before it may apply this
// mutation task. The two halves are separate functions rather than one
// symmetric helper because they are separate obligations: this one keeps the
// mutation from overwriting bytes a not-yet-replayed chunk is about to read,
// the other keeps a chunk from reading bytes that have not landed yet.
inline constexpr bool chunkBlocksMutationApply(std::uint64_t chunkOrdinal,
                                               bool chunkReplayed,
                                               std::uint64_t mutationOrdinal) noexcept {
  return fifoOrdinalPrecedes(chunkOrdinal, mutationOrdinal) && !chunkReplayed;
}

// TLA+: `BufferMutationOffload!CaptureRevisionIsCurrent`, the predicate behind
// the `SnapshotRevisionIsCurrent` invariant — R-BACK-44.2 step 2.
//
// `capturedRevision` is what `Pool::captureChunkBufferBinding` froze;
// `publishedRevision` is the `contentRevision` every unlock that has already
// returned to the app has published. Synchronous rotation makes them equal at
// every commit point. Deferring the logical rotation to the worker does not,
// and the resulting stale snapshot is internally consistent — the old backing
// really does still hold the old revision — so nothing downstream can detect
// it. That is why this is a separate invariant with its own counterexample
// configuration (`BufferMutationOffload.rotation.counterexample.cfg`) rather
// than a consequence of the visibility one.
inline constexpr bool captureRevisionIsCurrent(
    std::uint64_t capturedRevision, std::uint64_t publishedRevision) noexcept {
  return capturedRevision == publishedRevision;
}

// TLA+: `ReplayScopedDrain!ScopedReturnSafe` (R-BACK-2.51(d)(i)), which
// R-BACK-44.5 extends to mutation tasks by publishing them against the same
// per-buffer `ReplayDrainTarget`.
//
// A direct (non-chunk) unix call that reads live record bytes — shared-buffer
// export/alias today, any future readback — must wait while the buffer's
// queued watermark runs ahead of its replayed watermark. Read locks of Managed
// buffers are served from the core CPU `storage_` and never reach this gate.
//
// This one is bound to `ReplayScopedDrain.tla`, not to
// `BufferMutationOffload.tla`: the latter deliberately does not model the
// direct-reader fence (see its scope note), so claiming a binding it does not
// have would be worse than naming the model that does.
inline constexpr bool directReaderMustFence(std::uint64_t lastQueuedSeq,
                                            std::uint64_t lastReplayedSeq) noexcept {
  return lastQueuedSeq > lastReplayedSeq;
}

}  // namespace dxmt9::resources::mutation_offload
