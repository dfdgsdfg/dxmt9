#pragma once

// Shared pure predicates for the producer mark / reclaim protocol.
//
// These two functions are the C++ half of the vocabulary that
// `specs/verification/tla/ProducerMarkReclaim.tla` uses for its transitions.
// Production code, the model, and
// `tests/native/backend/producer_mark_reclaim_spec.cpp` all name the same two
// predicates, so a TLC counterexample trace is mechanically translatable into
// a deterministic native step sequence instead of being hand-reinterpreted.
// See `docs/superpowers/specs/2026-08-20-producer-queue-concurrency-design.md`
// §5 layer 1.
//
// Both are `constexpr` value transforms with no dependency on Wine, Metal, or
// the queue mutex — which is the point: the producer↔queue concurrency track
// (T2a / T2b) moves their call sites off `CommandQueue::mutex_`, and the
// decision they encode must stay identical when it does.

#include <cstdint>

namespace dxmt9::resources {

// TLA+: ProducerMarkReclaim!CanReclaimRecord, the enabling condition of
// ProducerMarkReclaim!Reclaim(r).
//
// The gate is exactly two facts and nothing else: the record's last unix
// reference has been dropped, and the GPU has completed past the last chunk
// that named it. Pins are deliberately absent here — a pinned record cannot
// reach `destroyPending` in the first place, which is the ordering premise
// the model checks and the counterexample configuration removes.
inline constexpr bool canReclaimRecord(bool destroyPending,
                                       std::uint64_t lastUsedSeqId,
                                       std::uint64_t completedSeqId) noexcept {
  return destroyPending && lastUsedSeqId <= completedSeqId;
}

// TLA+: ProducerMarkReclaim!MarkStampUpper, the value written by
// ProducerMarkReclaim!StampMark(r).
//
// A monotone max, so a stamp never moves a record's watermark backwards no
// matter what order concurrent marks land in. Ties keep the current value,
// matching `std::max(current, stamp)`.
inline constexpr std::uint64_t markStampUpper(std::uint64_t current,
                                              std::uint64_t stamp) noexcept {
  return stamp > current ? stamp : current;
}

}  // namespace dxmt9::resources
