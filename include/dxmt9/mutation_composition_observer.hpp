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
#include <mutex>
#include <optional>

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

enum class CompletionDisposition : std::uint8_t {
  Completed,
  Pending,
  Failed,
  Discarded,
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
  RenderTapeIdentity,
  Disposition,
  RangeOverlap,
  Barrier,
  Failure,
  SourceOrder,
  Capacity,
  Invalid,
  Completion,
  Count,
};

using ResourceMutationIdentity = dxmt9::resources::MutationSourceIdentity;

struct RenderTapeIdentity {
  std::uint32_t kind = 0u;
  std::uint32_t reserved = 0u;
  std::uint64_t generation = 0u;
  std::uint64_t objectId = 0u;

  constexpr bool valid() const noexcept {
    return kind != 0u && generation != 0u && objectId != 0u;
  }

  friend constexpr bool operator==(const RenderTapeIdentity&, const RenderTapeIdentity&) = default;
};

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
  // Filled by MutationCompositionObserver::recordMutation.  Keeping the
  // production source ordinal above preserves exact task settlement while
  // this identity supplies the one observer-wide ordering domain.
  dxmt9::resources::MutationOrderingIdentity orderingIdentity{};
  dxmt9::resources::MutationSourceKind sourceKind =
      dxmt9::resources::MutationSourceKind::SynchronousMutation;
  RenderTapeIdentity renderTapeIdentity{};
  Disposition disposition = Disposition::Plain;
  std::uint64_t byteOffset = 0u;
  std::uint64_t byteSize = 0u;
  MutationTiming timing{};
  bool successful = true;
  CompletionDisposition completion = CompletionDisposition::Completed;
  bool barrierBefore = false;
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
  // `rejectionCounts` is the record-time/provisional view.  Completion can
  // settle after the adjacent event was recorded, so finalization rebuilds
  // the exact terminal-state view below rather than mutating history.
  std::uint64_t finalRejectionCounts[
      static_cast<std::size_t>(RejectionReason::Count)]{};
  std::uint64_t invalidOrDroppedEvents = 0u;
  std::uint64_t pendingMutations = 0u;
  std::uint64_t completedMutations = 0u;
  std::uint64_t failedMutations = 0u;
  std::uint64_t discardedMutations = 0u;
};

enum class CompositionDecision : std::uint8_t {
  Candidate,
  DifferentResource,
  DifferentGeneration,
  RenderTapeIdentity,
  SourceOrder,
  Barrier,
  Failure,
  Completion,
  Disposition,
  RangeOverlap,
};

inline constexpr CompositionDecision classifyComposition(
    const MutationEvent& previous, const MutationEvent& current,
    bool barrierBeforeCurrent) noexcept {
  if (previous.identity.resource != current.identity.resource)
    return CompositionDecision::DifferentResource;
  if (previous.identity.backingGeneration !=
      current.identity.backingGeneration)
    return CompositionDecision::DifferentGeneration;
  // Both sides must carry a complete Render Tape identity.  Two missing
  // identities are not an exact match: allowing that pair would turn an
  // unsupported/legacy observation into a composition candidate.
  if (!previous.renderTapeIdentity.valid() ||
      !current.renderTapeIdentity.valid() ||
      previous.renderTapeIdentity != current.renderTapeIdentity)
    return CompositionDecision::RenderTapeIdentity;
  const bool previousHasOrdering = previous.orderingIdentity.valid();
  const bool currentHasOrdering = current.orderingIdentity.valid();
  if (previousHasOrdering || currentHasOrdering) {
    if (!previousHasOrdering || !currentHasOrdering ||
        !dxmt9::resources::mutationOrderingPrecedes(
            previous.orderingIdentity, current.orderingIdentity))
      return CompositionDecision::SourceOrder;
  } else if (!mutationSourceOrdinalPrecedes(previous.identity,
                                             current.identity)) {
    // Compatibility for direct value-only callers. Production observer
    // events always carry the generation-qualified ordering identity.
    return CompositionDecision::SourceOrder;
  }
  if (barrierBeforeCurrent) return CompositionDecision::Barrier;
  if (!previous.successful || !current.successful)
    return CompositionDecision::Failure;
  if (previous.completion != CompletionDisposition::Completed ||
      current.completion != CompletionDisposition::Completed)
    return CompositionDecision::Completion;
  if (previous.disposition == Disposition::Discard ||
      current.disposition == Disposition::Discard)
    return CompositionDecision::Disposition;
  const auto previousEnd = previous.byteOffset + previous.byteSize;
  const auto currentEnd = current.byteOffset + current.byteSize;
  const bool overlap = current.byteOffset < previousEnd &&
                       previous.byteOffset < currentEnd;
  if (previous.disposition == Disposition::NoOverwrite &&
      current.disposition == Disposition::NoOverwrite && overlap)
    return CompositionDecision::RangeOverlap;
  return CompositionDecision::Candidate;
}

class MutationCompositionObserver final {
 public:
  // The caps are diagnostic bounds. A full workload may be sampled in
  // windows; silently growing a container here would make the observer itself
  // part of the throughput experiment.
  static constexpr std::size_t kMaxGenerations = 1024u;
  static constexpr std::size_t kMaxEvents = 4096u;

  bool recordMutation(const MutationEvent& event) noexcept {
    std::lock_guard lock(mutex_);
    if (!event.identity.valid() || event.byteOffset >
            std::numeric_limits<std::uint64_t>::max() - event.byteSize) {
      ++snapshot_.invalidOrDroppedEvents;
      return false;
    }
    MutationEvent observed = event;
    observed.orderingIdentity = orderingPolicy_.issue(event.sourceKind);
    if (!observed.orderingIdentity.valid()) {
      ++snapshot_.invalidOrDroppedEvents;
      return false;
    }
    if (eventCount_ == kMaxEvents) {
      ++snapshot_.invalidOrDroppedEvents;
      reject(RejectionReason::Capacity);
      return false;
    }
    auto* generation = findGeneration(observed.identity);
    if (generation == nullptr) {
      if (generationCount_ == kMaxGenerations) {
        ++snapshot_.invalidOrDroppedEvents;
        reject(RejectionReason::Capacity);
        return false;
      }
      auto& created = generations_[generationCount_++];
      created = {};
      created.resource = observed.identity.resource;
      created.backingGeneration = observed.identity.backingGeneration;
      created.firstEvent = eventCount_;
      generation = &created;
    }

    const MutationEvent* previous = nullptr;
    if (generation->mutationCount != 0u) {
      previous = &events_[generation->lastEvent];
    } else {
      previous = latestResourceEvent(observed.identity.resource);
    }
    if (previous) {
      const bool successful = previous->successful && event.successful;
      const bool dispositionComposable =
          previous->disposition != Disposition::Discard &&
          observed.disposition != Disposition::Discard;
      const auto previousEnd = previous->byteOffset + previous->byteSize;
      const auto currentEnd = observed.byteOffset + observed.byteSize;
      const bool overlap = observed.byteOffset < previousEnd &&
                           previous->byteOffset < currentEnd;
      const bool rangesComposable =
          !(previous->disposition == Disposition::NoOverwrite &&
            observed.disposition == Disposition::NoOverwrite && overlap);
      const auto decision = classifyComposition(
          *previous, observed, generation->barrierSinceLast);
      if (decision == CompositionDecision::Candidate && successful &&
          observed.completion == CompletionDisposition::Completed &&
          previous->completion == CompletionDisposition::Completed &&
          dispositionComposable && rangesComposable) {
        ++snapshot_.mergeableRangePairs;
        ++snapshot_.candidateCalls;
        snapshot_.candidateBytesSaved += observed.byteSize;
        snapshot_.candidateCpuTimeSavedNs += observed.timing.knownTotalNs();
        snapshot_.mergeableUnionBytes += rangeUnionBytes(
            previous->byteOffset, previousEnd, observed.byteOffset, currentEnd);
        snapshot_.mergeableOverlapBytes +=
            overlap ? rangeOverlapBytes(previous->byteOffset, previousEnd,
                                       observed.byteOffset, currentEnd)
                    : 0u;
      } else {
        // This is the provisional view: a Pending event may settle later and
        // therefore legitimately move from Completion to Candidate in the
        // exact final view rebuilt by finalize().
        if (decision != CompositionDecision::Candidate)
          reject(rejectionForDecision(decision));
      }
    }

    events_[eventCount_] = observed;
    events_[eventCount_].barrierBefore = generation->barrierSinceLast;
    discardChainsCounted_ = false;
    if (generation->mutationCount == 0u) {
      generation->firstOrdinal = observed.identity.sourceOrdinal;
      generation->firstOrdering = observed.orderingIdentity;
      generation->firstDisposition = observed.disposition;
    }
    generation->lastEvent = eventCount_++;
    ++generation->mutationCount;
    generation->lastDisposition = observed.disposition;
    generation->barrierSinceLast = false;
    ++snapshot_.mutationCalls;
    snapshot_.mutationBytes += event.byteSize;
    if (observed.completion == CompletionDisposition::Completed)
      ++snapshot_.completedMutations;
    else if (observed.completion == CompletionDisposition::Pending)
      ++snapshot_.pendingMutations;
    else if (observed.completion == CompletionDisposition::Failed)
      ++snapshot_.failedMutations;
    else if (observed.completion == CompletionDisposition::Discarded)
      ++snapshot_.discardedMutations;
    snapshot_.wow64WritebackNs += observed.timing.wow64WritebackNs;
    snapshot_.queueLockNs += observed.timing.queueLockNs;
    snapshot_.backingRotationNs += observed.timing.backingRotationNs;
    snapshot_.arenaUpdateNs += observed.timing.arenaUpdateNs;
    snapshot_.shadowCopyNs += observed.timing.shadowCopyNs;
    snapshot_.liveContentsCopyNs += observed.timing.liveContentsCopyNs;
    return true;
  }

  // Settle the exact source-qualified event after an asynchronous mutation
  // task has applied or been discarded. The observer retains no task payload
  // or wrapper pointer; only this small disposition is updated.
  void settleMutation(ResourceMutationIdentity identity,
                      CompletionDisposition disposition) noexcept {
    std::lock_guard lock(mutex_);
    for (std::size_t i = eventCount_; i != 0u; --i) {
      auto& event = events_[i - 1u];
      if (event.identity.resource == identity.resource &&
          event.identity.backingGeneration == identity.backingGeneration &&
          event.identity.sourceOrdinal == identity.sourceOrdinal &&
          event.completion == CompletionDisposition::Pending) {
        event.completion = disposition;
        if (snapshot_.pendingMutations != 0u) --snapshot_.pendingMutations;
        if (disposition == CompletionDisposition::Failed)
          event.successful = false;
        if (disposition == CompletionDisposition::Completed)
          ++snapshot_.completedMutations;
        else if (disposition == CompletionDisposition::Failed)
          ++snapshot_.failedMutations;
        else if (disposition == CompletionDisposition::Discarded)
          ++snapshot_.discardedMutations;
        return;
      }
    }
    ++snapshot_.invalidOrDroppedEvents;
  }

  // Direct CPU readers have no queue ordinal. Allocate a diagnostic-only
  // source ordinal for the retained production identity. Composition ordering
  // never compares this value with replay sequencing; recordMutation assigns
  // the shared generation-qualified ordering identity below.
  void observeCpuUse(std::uint64_t resource, std::uint64_t generation,
                     ObserverKind kind) noexcept {
    std::lock_guard lock(mutex_);
    if (resource == 0u || generation == 0u) {
      ++snapshot_.invalidOrDroppedEvents;
      return;
    }
    observeUse({.resource = resource,
                .backingGeneration = generation,
                .sourceOrdinal = allocateSourceOrdinal()},
               kind);
  }

  void observeCpuUseForResource(std::uint64_t resource,
                                ObserverKind kind) noexcept {
    std::lock_guard lock(mutex_);
    for (std::size_t i = eventCount_; i != 0u; --i) {
      if (events_[i - 1u].identity.resource == resource) {
        observeCpuUse(resource, events_[i - 1u].identity.backingGeneration,
                      kind);
        return;
      }
    }
    // A read of a resource with no recorded mutation is outside this
    // mutation-focused ledger and must not perturb its candidate counts.
  }

  // Legacy replay may expose only the canonical logical buffer handle (no
  // captured rename-ring snapshot). Bind the use to the newest observed
  // generation rather than fabricating generation one; this remains
  // conservative while preserving the exact replay source ordinal.
  void observeUseForResource(std::uint64_t resource, std::uint64_t ordinal,
                             ObserverKind kind) noexcept {
    std::lock_guard lock(mutex_);
    for (std::size_t i = eventCount_; i != 0u; --i) {
      if (events_[i - 1u].identity.resource == resource) {
        observeUse({.resource = resource,
                    .backingGeneration =
                        events_[i - 1u].identity.backingGeneration,
                    .sourceOrdinal = ordinal},
                   kind);
        return;
      }
    }
    // Likewise, an unsupported/legacy use with no mutation in this window is
    // not an observer error and must not manufacture a global barrier.
  }

  void observeGlobalBarrier(BarrierReason reason) noexcept {
    std::lock_guard lock(mutex_);
    ++snapshot_.barrierCounts[static_cast<std::size_t>(reason)];
    for (std::size_t i = 0u; i < generationCount_; ++i)
      generations_[i].barrierSinceLast = true;
  }

  // Call this at the first known GPU/CPU observer. The generation must be the
  // generation named by that observer, not the record's current generation.
  void observeUse(ResourceMutationIdentity identity, ObserverKind kind) noexcept {
    std::lock_guard lock(mutex_);
    if (!identity.valid()) {
      ++snapshot_.invalidOrDroppedEvents;
      return;
    }
    auto* generation = findGeneration(identity);
    if (!generation) return;
    const auto ordering = orderingPolicy_.issue(
        dxmt9::resources::MutationSourceKind::ReplayUse);
    if (!ordering.valid()) {
      ++snapshot_.invalidOrDroppedEvents;
      return;
    }
    generation->barrierSinceLast = true;
    if (generation->used) return;
    generation->used = true;
    // Keep this metric in the production replay/source domain.  Candidate
    // ordering itself uses `orderingIdentity`; the distance is intentionally
    // a source-facing diagnostic and can be zero when a replay source is
    // outside the retained window.
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
    std::lock_guard lock(mutex_);
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
    std::lock_guard lock(mutex_);
    for (std::size_t i = 0u; i < generationCount_; ++i) {
      auto& generation = generations_[i];
      if (generation.mutationCount == 0u || generation.finalized) continue;
      generation.finalized = true;
      if (!generation.used) ++snapshot_.zeroUseGenerations;
    }
    recomputeCandidates();
    recomputeFinalRejections();
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

  void reset() noexcept {
    std::lock_guard lock(mutex_);
    generations_ = {};
    events_ = {};
    generationCount_ = 0u;
    eventCount_ = 0u;
    discardChainsCounted_ = false;
    (void)orderingPolicy_.reset();
    snapshot_ = {};
    windowPresents_ = 0u;
  }

  void notePresent() noexcept {
    std::lock_guard lock(mutex_);
    ++windowPresents_;
  }
  [[nodiscard]] std::uint64_t windowPresents() const noexcept {
    std::lock_guard lock(mutex_);
    return windowPresents_;
  }
  [[nodiscard]] std::uint64_t allocateSourceOrdinal() noexcept {
    std::lock_guard lock(mutex_);
    return orderingPolicy_.nextOrdinal();
  }

  [[nodiscard]] ObserverSnapshot snapshot() const noexcept {
    std::lock_guard lock(mutex_);
    return snapshot_;
  }

  [[nodiscard]] std::size_t eventCount() const noexcept {
    std::lock_guard lock(mutex_);
    return eventCount_;
  }

  // Return an owned snapshot. Returning an internal pointer after releasing
  // the mutex would let a concurrent recorder/settler race the caller.
  [[nodiscard]] std::optional<MutationEvent> eventAt(
      std::size_t index) const noexcept {
    std::lock_guard lock(mutex_);
    if (index >= eventCount_) return std::nullopt;
    return events_[index];
  }

 private:
  struct GenerationState {
    std::uint64_t resource = 0u;
    std::uint64_t backingGeneration = 0u;
    std::size_t firstEvent = 0u;
    std::size_t lastEvent = 0u;
    std::uint64_t firstOrdinal = 0u;
    dxmt9::resources::MutationOrderingIdentity firstOrdering{};
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

  static RejectionReason rejectionForDecision(
      CompositionDecision decision) noexcept {
    switch (decision) {
      case CompositionDecision::DifferentResource:
        return RejectionReason::DifferentResource;
      case CompositionDecision::DifferentGeneration:
        return RejectionReason::DifferentGeneration;
      case CompositionDecision::RenderTapeIdentity:
        return RejectionReason::RenderTapeIdentity;
      case CompositionDecision::SourceOrder:
        return RejectionReason::SourceOrder;
      case CompositionDecision::Barrier:
        return RejectionReason::Barrier;
      case CompositionDecision::Failure:
        return RejectionReason::Failure;
      case CompositionDecision::Completion:
        return RejectionReason::Completion;
      case CompositionDecision::Disposition:
        return RejectionReason::Disposition;
      case CompositionDecision::RangeOverlap:
        return RejectionReason::RangeOverlap;
      case CompositionDecision::Candidate:
        break;
    }
    return RejectionReason::Invalid;
  }

  const MutationEvent* previousForEvent(std::size_t index) const noexcept {
    const auto& current = events_[index];
    for (std::size_t i = index; i != 0u; --i) {
      const auto& prior = events_[i - 1u];
      if (prior.identity.resource == current.identity.resource &&
          prior.identity.backingGeneration ==
              current.identity.backingGeneration)
        return &prior;
    }
    // This is the first event for the current generation. Match
    // recordMutation(): only then compare against the latest event for the
    // same logical resource so the generation change is observable.
    for (std::size_t i = index; i != 0u; --i) {
      const auto& prior = events_[i - 1u];
      if (prior.identity.resource == current.identity.resource) return &prior;
    }
    return nullptr;
  }

  void recomputeFinalRejections() noexcept {
    for (auto& count : snapshot_.finalRejectionCounts) count = 0u;
    for (std::size_t i = 0u; i < eventCount_; ++i) {
      const auto* previous = previousForEvent(i);
      if (!previous) continue;
      const auto decision = classifyComposition(
          *previous, events_[i], events_[i].barrierBefore);
      if (decision != CompositionDecision::Candidate) {
        ++snapshot_.finalRejectionCounts[
            static_cast<std::size_t>(rejectionForDecision(decision))];
      }
    }
  }

  void recomputeCandidates() noexcept {
    snapshot_.mergeableRangePairs = 0u;
    snapshot_.mergeableUnionBytes = 0u;
    snapshot_.mergeableOverlapBytes = 0u;
    snapshot_.candidateCalls = 0u;
    snapshot_.candidateBytesSaved = 0u;
    snapshot_.candidateCpuTimeSavedNs = 0u;
    for (std::size_t generationIndex = 0u;
         generationIndex < generationCount_; ++generationIndex) {
      const auto& generation = generations_[generationIndex];
      const MutationEvent* previous = nullptr;
      for (std::size_t i = generation.firstEvent; i < eventCount_; ++i) {
        const auto& current = events_[i];
        if (current.identity.resource != generation.resource ||
            current.identity.backingGeneration != generation.backingGeneration)
          continue;
        if (previous) {
          const auto previousEnd = previous->byteOffset + previous->byteSize;
          const auto currentEnd = current.byteOffset + current.byteSize;
          const bool overlap = current.byteOffset < previousEnd &&
                               previous->byteOffset < currentEnd;
          const bool rangesComposable =
              !(previous->disposition == Disposition::NoOverwrite &&
                current.disposition == Disposition::NoOverwrite && overlap);
          if (classifyComposition(*previous, current,
                                  current.barrierBefore) ==
              CompositionDecision::Candidate &&
              rangesComposable) {
            ++snapshot_.mergeableRangePairs;
            ++snapshot_.candidateCalls;
            snapshot_.candidateBytesSaved += current.byteSize;
            snapshot_.candidateCpuTimeSavedNs += current.timing.knownTotalNs();
            snapshot_.mergeableUnionBytes += rangeUnionBytes(
                previous->byteOffset, previousEnd, current.byteOffset,
                currentEnd);
            snapshot_.mergeableOverlapBytes +=
                overlap ? rangeOverlapBytes(previous->byteOffset, previousEnd,
                                            current.byteOffset, currentEnd)
                        : 0u;
          }
        }
        previous = &current;
      }
    }
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
  dxmt9::resources::MutationOrderingPolicy orderingPolicy_{};
  std::uint64_t windowPresents_ = 0u;
  ObserverSnapshot snapshot_{};
  mutable std::recursive_mutex mutex_;
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
