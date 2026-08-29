#pragma once

// Observation-only ledger for the R-BACK-44.9..44.11 composition decision.
//
// This header deliberately has no dependency on the queue, pool, Wine, or
// Metal.  The identity tuple is the same tuple used by the managed mutation
// transport: the canonical Handle value (slot plus resource generation), the
// pool backing/content generation, and the FIFO/source ordinal.  Installing an
// observer is a cold diagnostic action; callers on the normal path only test
// the cached pointer before constructing an event.

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "dxmt9/mutation_identity.hpp"

namespace dxmt9::resources::mutation_observer {

enum class Disposition : std::uint8_t {
  Plain,
  NoOverwrite,
  Discard,
};

enum class ObserverKind : std::uint8_t {
  GpuUse,
  CpuObserver,
};

enum class BarrierReason : std::uint8_t {
  Draw,
  ProcessVertices,
  ReadLock,
  QueryReadback,
  UpdateCopy,
  CrossThread,
  DestroyReset,
  CaptureLease,
  Failure,
  Unknown,
  Count,
};

enum class RejectionReason : std::uint8_t {
  DifferentResource,
  DifferentGeneration,
  Disposition,
  RangeOverlap,
  Barrier,
  Failure,
  SourceOrder,
  Capacity,
  Invalid,
  Count,
};

using ResourceMutationIdentity = dxmt9::resources::MutationSourceIdentity;

struct MutationTiming {
  // Zero means that this phase was not separately sampled, not that it was
  // free. The total is the measured sum of known phases only.
  std::uint64_t wow64WritebackNs = 0u;
  std::uint64_t queueLockNs = 0u;
  std::uint64_t backingRotationNs = 0u;
  std::uint64_t arenaUpdateNs = 0u;
  std::uint64_t shadowCopyNs = 0u;
  std::uint64_t liveContentsCopyNs = 0u;

  constexpr std::uint64_t knownTotalNs() const noexcept {
    return wow64WritebackNs + queueLockNs + backingRotationNs +
           arenaUpdateNs + shadowCopyNs + liveContentsCopyNs;
  }
};

struct MutationEvent {
  ResourceMutationIdentity identity{};
  Disposition disposition = Disposition::Plain;
  std::uint64_t byteOffset = 0u;
  std::uint64_t byteSize = 0u;
  MutationTiming timing{};
  bool successful = true;
  std::uint64_t firstUseDistance =
      std::numeric_limits<std::uint64_t>::max();
  ObserverKind firstUseKind = ObserverKind::GpuUse;
};

struct ObserverSnapshot {
  std::uint64_t mutationCalls = 0u;
  std::uint64_t mutationBytes = 0u;
  std::uint64_t zeroUseGenerations = 0u;
  std::uint64_t discardToDiscardDeadChains = 0u;
  std::uint64_t firstUseGpuCount = 0u;
  std::uint64_t firstUseCpuCount = 0u;
  std::uint64_t firstUseDistanceTotal = 0u;
  std::uint64_t firstUseDistanceMax = 0u;
  std::uint64_t mergeableRangePairs = 0u;
  std::uint64_t mergeableUnionBytes = 0u;
  std::uint64_t mergeableOverlapBytes = 0u;
  std::uint64_t candidateCalls = 0u;
  std::uint64_t candidateBytesSaved = 0u;
  std::uint64_t candidateCpuTimeSavedNs = 0u;
  std::uint64_t wow64WritebackNs = 0u;
  std::uint64_t queueLockNs = 0u;
  std::uint64_t backingRotationNs = 0u;
  std::uint64_t arenaUpdateNs = 0u;
  std::uint64_t shadowCopyNs = 0u;
  std::uint64_t liveContentsCopyNs = 0u;
  std::uint64_t barrierCounts[static_cast<std::size_t>(BarrierReason::Count)]{};
  std::uint64_t rejectionCounts[
      static_cast<std::size_t>(RejectionReason::Count)]{};
  std::uint64_t invalidOrDroppedEvents = 0u;
};

class MutationCompositionObserver final {
 public:
  // The caps are diagnostic bounds. A full workload may be sampled in
  // windows; silently growing a container here would make the observer itself
  // part of the throughput experiment.
  static constexpr std::size_t kMaxGenerations = 1024u;
  static constexpr std::size_t kMaxEvents = 4096u;

  bool recordMutation(const MutationEvent& event) noexcept {
    if (!event.identity.valid() || event.byteOffset >
            std::numeric_limits<std::uint64_t>::max() - event.byteSize) {
      ++snapshot_.invalidOrDroppedEvents;
      return false;
    }
    auto* generation = findGeneration(event.identity);
    if (generation == nullptr) {
      if (generationCount_ == kMaxGenerations) {
        ++snapshot_.invalidOrDroppedEvents;
        reject(RejectionReason::Capacity);
        return false;
      }
      auto& created = generations_[generationCount_++];
      created = {};
      created.resource = event.identity.resource;
      created.backingGeneration = event.identity.backingGeneration;
      created.firstEvent = eventCount_;
      generation = &created;
    }

    if (eventCount_ == kMaxEvents) {
      ++snapshot_.invalidOrDroppedEvents;
      reject(RejectionReason::Capacity);
      return false;
    }

    const MutationEvent* previous = nullptr;
    if (generation->mutationCount != 0u) {
      previous = &events_[generation->lastEvent];
    } else {
      previous = latestResourceEvent(event.identity.resource);
    }
    if (previous) {
      const bool sourceOrder =
          dxmt9::resources::mutationSourceOrdinalPrecedes(
              previous->identity, event.identity);
      const bool noBarrier = !generation->barrierSinceLast;
      const bool successful = previous->successful && event.successful;
      const bool dispositionComposable =
          previous->disposition != Disposition::Discard &&
          event.disposition != Disposition::Discard;
      const auto previousEnd = previous->byteOffset + previous->byteSize;
      const auto currentEnd = event.byteOffset + event.byteSize;
      const bool overlap = event.byteOffset < previousEnd &&
                           previous->byteOffset < currentEnd;
      const bool rangesComposable =
          !(previous->disposition == Disposition::NoOverwrite &&
            event.disposition == Disposition::NoOverwrite && overlap);
      if (generation->mutationCount != 0u && sourceOrder && noBarrier &&
          successful && dispositionComposable &&
          rangesComposable) {
        ++snapshot_.mergeableRangePairs;
        ++snapshot_.candidateCalls;
        snapshot_.candidateBytesSaved += event.byteSize;
        snapshot_.candidateCpuTimeSavedNs += event.timing.knownTotalNs();
        snapshot_.mergeableUnionBytes += rangeUnionBytes(
            previous->byteOffset, previousEnd, event.byteOffset, currentEnd);
        snapshot_.mergeableOverlapBytes +=
            overlap ? rangeOverlapBytes(previous->byteOffset, previousEnd,
                                       event.byteOffset, currentEnd)
                    : 0u;
      } else {
        if (!sourceOrder)
          reject(RejectionReason::SourceOrder);
        else if (generation->barrierSinceLast)
          reject(RejectionReason::Barrier);
        else if (!successful)
          reject(RejectionReason::Failure);
        else if (!dispositionComposable)
          reject(RejectionReason::Disposition);
        else if (!rangesComposable)
          reject(RejectionReason::RangeOverlap);
        else if (generation->mutationCount == 0u)
          reject(RejectionReason::DifferentGeneration);
      }
    }

    events_[eventCount_] = event;
    discardChainsCounted_ = false;
    if (generation->mutationCount == 0u) {
      generation->firstOrdinal = event.identity.sourceOrdinal;
      generation->firstDisposition = event.disposition;
    }
    generation->lastEvent = eventCount_++;
    ++generation->mutationCount;
    generation->lastDisposition = event.disposition;
    generation->barrierSinceLast = false;
    ++snapshot_.mutationCalls;
    snapshot_.mutationBytes += event.byteSize;
    snapshot_.wow64WritebackNs += event.timing.wow64WritebackNs;
    snapshot_.queueLockNs += event.timing.queueLockNs;
    snapshot_.backingRotationNs += event.timing.backingRotationNs;
    snapshot_.arenaUpdateNs += event.timing.arenaUpdateNs;
    snapshot_.shadowCopyNs += event.timing.shadowCopyNs;
    snapshot_.liveContentsCopyNs += event.timing.liveContentsCopyNs;
    return true;
  }

  // Call this at the first known GPU/CPU observer. The generation must be the
  // generation named by that observer, not the record's current generation.
  void observeUse(ResourceMutationIdentity identity, ObserverKind kind) noexcept {
    if (!identity.valid()) {
      ++snapshot_.invalidOrDroppedEvents;
      return;
    }
    auto* generation = findGeneration(identity);
    if (!generation) {
      reject(RejectionReason::Invalid);
      return;
    }
    generation->barrierSinceLast = true;
    if (generation->used) return;
    generation->used = true;
    const auto distance = identity.sourceOrdinal >= generation->firstOrdinal
                              ? identity.sourceOrdinal - generation->firstOrdinal
                              : 0u;
    for (std::size_t i = 0u; i < eventCount_; ++i) {
      auto& event = events_[i];
      if (event.identity.resource == identity.resource &&
          event.identity.backingGeneration == identity.backingGeneration) {
        event.firstUseDistance = distance;
        event.firstUseKind = kind;
      }
    }
    snapshot_.firstUseDistanceTotal += distance;
    if (distance > snapshot_.firstUseDistanceMax)
      snapshot_.firstUseDistanceMax = distance;
    if (kind == ObserverKind::GpuUse)
      ++snapshot_.firstUseGpuCount;
    else
      ++snapshot_.firstUseCpuCount;
  }

  void observeBarrier(ResourceMutationIdentity identity,
                      BarrierReason reason) noexcept {
    if (!identity.valid()) {
      ++snapshot_.invalidOrDroppedEvents;
      return;
    }
    auto* generation = findGeneration(identity);
    if (!generation) {
      reject(RejectionReason::Invalid);
      return;
    }
    generation->barrierSinceLast = true;
    ++snapshot_.barrierCounts[static_cast<std::size_t>(reason)];
  }

  // Close the sampling window. A generation with no GPU/CPU observer is the
  // only kind that can safely be called zero-use; merely being followed by a
  // newer generation is not itself evidence of dead bytes until this point.
  void finalize() noexcept {
    for (std::size_t i = 0u; i < generationCount_; ++i) {
      auto& generation = generations_[i];
      if (generation.mutationCount == 0u || generation.finalized) continue;
      generation.finalized = true;
      if (!generation.used) ++snapshot_.zeroUseGenerations;
    }
    if (!discardChainsCounted_) {
      for (std::size_t i = 0u; i < generationCount_; ++i) {
        const auto& current = generations_[i];
        const auto* prior = previousGeneration(current);
        if (prior && prior->lastDisposition == Disposition::Discard &&
            current.firstDisposition == Disposition::Discard && !prior->used &&
            !current.used) {
          ++snapshot_.discardToDiscardDeadChains;
        }
      }
      discardChainsCounted_ = true;
    }
  }

  [[nodiscard]] ObserverSnapshot snapshot() const noexcept { return snapshot_; }

  [[nodiscard]] std::size_t eventCount() const noexcept { return eventCount_; }

  [[nodiscard]] const MutationEvent* eventAt(std::size_t index) const noexcept {
    return index < eventCount_ ? &events_[index] : nullptr;
  }

 private:
  struct GenerationState {
    std::uint64_t resource = 0u;
    std::uint64_t backingGeneration = 0u;
    std::size_t firstEvent = 0u;
    std::size_t lastEvent = 0u;
    std::uint64_t firstOrdinal = 0u;
    std::uint64_t mutationCount = 0u;
    Disposition firstDisposition = Disposition::Plain;
    Disposition lastDisposition = Disposition::Plain;
    bool used = false;
    bool barrierSinceLast = false;
    bool finalized = false;
  };

  GenerationState* findGeneration(ResourceMutationIdentity identity) noexcept {
    for (std::size_t i = 0u; i < generationCount_; ++i) {
      auto& generation = generations_[i];
      if (dxmt9::resources::sameMutationResourceGeneration(
              {.resource = generation.resource,
               .backingGeneration = generation.backingGeneration},
              identity))
        return &generation;
    }
    return nullptr;
  }

  MutationEvent* latestResourceEvent(std::uint64_t resource) noexcept {
    for (std::size_t i = eventCount_; i != 0u; --i) {
      if (events_[i - 1u].identity.resource == resource)
        return &events_[i - 1u];
    }
    return nullptr;
  }

  const GenerationState* previousGeneration(
      const GenerationState& current) const noexcept {
    const GenerationState* prior = nullptr;
    for (std::size_t i = 0u; i < generationCount_; ++i) {
      const auto& candidate = generations_[i];
      if (candidate.resource != current.resource ||
          candidate.firstEvent >= current.firstEvent)
        continue;
      if (!prior || candidate.firstEvent > prior->firstEvent)
        prior = &candidate;
    }
    return prior;
  }

  void reject(RejectionReason reason) noexcept {
    ++snapshot_.rejectionCounts[static_cast<std::size_t>(reason)];
  }

  static std::uint64_t rangeUnionBytes(std::uint64_t aStart,
                                       std::uint64_t aEnd,
                                       std::uint64_t bStart,
                                       std::uint64_t bEnd) noexcept {
    const auto start = aStart < bStart ? aStart : bStart;
    const auto end = aEnd > bEnd ? aEnd : bEnd;
    return end - start;
  }

  static std::uint64_t rangeOverlapBytes(std::uint64_t aStart,
                                          std::uint64_t aEnd,
                                          std::uint64_t bStart,
                                          std::uint64_t bEnd) noexcept {
    const auto start = aStart > bStart ? aStart : bStart;
    const auto end = aEnd < bEnd ? aEnd : bEnd;
    return end > start ? end - start : 0u;
  }

  std::array<GenerationState, kMaxGenerations> generations_{};
  std::array<MutationEvent, kMaxEvents> events_{};
  std::size_t generationCount_ = 0u;
  std::size_t eventCount_ = 0u;
  bool discardChainsCounted_ = false;
  ObserverSnapshot snapshot_{};
};

// Install/uninstall only at a quiescent boundary (the same lifecycle rule as
// the other diagnostic ledgers). Disabled production code pays one pointer
// load and branch at an instrumented cold handoff, and no event is built.
inline MutationCompositionObserver* gMutationCompositionObserver = nullptr;

[[nodiscard]] inline MutationCompositionObserver*
activeMutationCompositionObserver() noexcept {
  return gMutationCompositionObserver;
}

class ScopedMutationCompositionObserver final {
 public:
  explicit ScopedMutationCompositionObserver(
      MutationCompositionObserver& observer) noexcept
      : previous_(gMutationCompositionObserver) {
    gMutationCompositionObserver = &observer;
  }
  ~ScopedMutationCompositionObserver() { gMutationCompositionObserver = previous_; }

  ScopedMutationCompositionObserver(const ScopedMutationCompositionObserver&) = delete;
  ScopedMutationCompositionObserver& operator=(
      const ScopedMutationCompositionObserver&) = delete;

 private:
  MutationCompositionObserver* previous_ = nullptr;
};

}  // namespace dxmt9::resources::mutation_observer
