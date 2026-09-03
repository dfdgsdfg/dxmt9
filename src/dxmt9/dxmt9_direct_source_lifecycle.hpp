#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>

namespace dxmt9::queue {

// Shared production/model vocabulary for the ordinary Direct-source path.
// The reducer is cold observer policy: callers invoke it only through the
// opt-in lifecycle sink, while native truth tables invoke the same function.
// The production projection begins once an admission witness is consumed. A
// raw rejected before that handoff has no direct destination identity and is
// intentionally outside this admitted-source ledger.
enum class DirectSourcePhase : std::uint8_t {
  Empty,
  RawOwned,
  Planned,
  Admitted,
  Effected,
  ReceiptOwned,
  Published,
  Encoded,
  Completed,
  Detached,
  Restored,
  PoisonAbandoned,
  Reclaimed,
  FailStopped,
};

enum class DirectSourceAction : std::uint8_t {
  ImportRaw,
  Plan,
  AdmitWitness,
  EffectCut,
  DestinationReceipt,
  Publish,
  Encode,
  Complete,
  Detach,
  Restore,
  PoisonAbandon,
  RollbackPreEffect,
  Reclaim,
  FailStop,
};

enum class DirectSourceControlMode : std::uint8_t {
  Ordinary,
  Separator,
  OrderedControl,
};

// Exact-fit admissions carry one qualitative credit; ledger-qualified
// admissions carry retained bytes.  They are deliberately distinct units so
// a byte total can never be mistaken for a qualitative receipt.
enum class DirectSourceCreditKind : std::uint8_t {
  Qualitative,
  RetainedBytes,
};

enum class DirectSourceLifecycleError : std::uint8_t {
  None,
  InvalidIdentity,
  BoundedOverflow,
  StaleOrDuplicateWitness,
  InvalidTransition,
  SourceReordered,
  DuplicateEmission,
  FallbackAfterEffect,
  CompletionRequired,
  CreditMismatch,
  MissingRestore,
  PartialAdoption,
};

struct DirectSourceIdentity {
  std::uint64_t rawOrdinal = 0;
  std::uint32_t spanOrdinal = 0;
  std::uint64_t sourceOrdinal = 0;
  std::uint64_t seqId = 0;
  std::uint64_t sourceGeneration = 0;
  std::uint64_t storageGeneration = 0;
  std::uint32_t destinationSlot =
      std::numeric_limits<std::uint32_t>::max();
  std::uint32_t sourceIndex = std::numeric_limits<std::uint32_t>::max();
  std::uint32_t firstPage = std::numeric_limits<std::uint32_t>::max();
  std::uint32_t pageCount = 0;

  constexpr bool valid() const noexcept {
    return rawOrdinal != 0 && sourceOrdinal != 0 && seqId != 0 &&
        sourceGeneration != 0 && storageGeneration != 0 &&
        destinationSlot != std::numeric_limits<std::uint32_t>::max() &&
        sourceIndex != std::numeric_limits<std::uint32_t>::max() &&
        firstPage != std::numeric_limits<std::uint32_t>::max() &&
        pageCount != 0;
  }

  friend constexpr bool operator==(const DirectSourceIdentity&,
                                   const DirectSourceIdentity&) = default;
};

struct DirectSourceLifecycleEvent {
  DirectSourceIdentity identity{};
  DirectSourceAction action = DirectSourceAction::ImportRaw;
  DirectSourceControlMode controlMode = DirectSourceControlMode::Ordinary;
  DirectSourceCreditKind creditKind = DirectSourceCreditKind::Qualitative;
  std::uint32_t schemaRevision = 0;
  std::uint64_t witnessGeneration = 0;
  std::uint64_t retainedCredit = 0;
  std::uint64_t stagedCredit = 0;
  std::uint64_t detachedCredit = 0;
  // One semantic source range may contain several spans backed by the same
  // physical payload.  Only the designated owner carries aggregate credit;
  // sibling span records remain semantic-only and must carry zero credit.
  bool physicalCreditOwner = false;
  bool hasPresent = false;
  bool partialAdoption = false;

  friend constexpr bool operator==(const DirectSourceLifecycleEvent&,
                                   const DirectSourceLifecycleEvent&) = default;
};

struct DirectSourceLifecycleRecord {
  DirectSourceIdentity identity{};
  DirectSourcePhase phase = DirectSourcePhase::Empty;
  DirectSourceControlMode controlMode = DirectSourceControlMode::Ordinary;
  DirectSourceCreditKind creditKind = DirectSourceCreditKind::Qualitative;
  std::uint32_t schemaRevision = 0;
  std::uint64_t witnessGeneration = 0;
  std::uint64_t retainedCredit = 0;
  std::uint64_t stagedCredit = 0;
  std::uint64_t detachedCredit = 0;
  std::uint64_t aggregateCredit = 0;
  bool physicalCreditOwner = false;
  // Counts every accepted lifecycle transition, including ImportRaw.  Keep
  // publication cardinality separate: exactly-once is a semantic emission
  // property, not a count of cold observer transitions.
  std::uint32_t transitionCount = 0;
  std::uint32_t publicationCount = 0;
  bool hasPresent = false;
  bool effectStarted = false;
  bool witnessConsumed = false;
  bool completed = false;
  bool poisoned = false;

  friend constexpr bool operator==(const DirectSourceLifecycleRecord&,
                                   const DirectSourceLifecycleRecord&) = default;
};

inline constexpr std::size_t kMaxObservedDirectSources = 256;

struct DirectSourceLifecycleState {
  std::array<DirectSourceLifecycleRecord, kMaxObservedDirectSources> records{};
  std::size_t recordCount = 0;
  std::uint64_t lastImportedRawOrdinal = 0;
  std::uint32_t lastImportedSpanOrdinal = 0;
  std::uint64_t lastImportedSourceOrdinal = 0;
  std::uint64_t lastPublishedRawOrdinal = 0;
  std::uint32_t lastPublishedSpanOrdinal = 0;
  std::uint64_t lastPublishedSourceOrdinal = 0;
  std::uint64_t lastReclaimedRawOrdinal = 0;
  std::uint32_t lastReclaimedSpanOrdinal = 0;
  std::uint64_t lastReclaimedSourceOrdinal = 0;

  friend constexpr bool operator==(const DirectSourceLifecycleState&,
                                   const DirectSourceLifecycleState&) = default;
};

namespace detail {

constexpr DirectSourceLifecycleRecord* findDirectSource(
    DirectSourceLifecycleState& state,
    const DirectSourceIdentity& identity) noexcept {
  for (std::size_t i = 0; i < state.recordCount; ++i) {
    if (state.records[i].identity == identity) return &state.records[i];
  }
  return nullptr;
}

constexpr bool directCreditConserved(
    const DirectSourceLifecycleRecord& record) noexcept {
  return record.retainedCredit <=
             std::numeric_limits<std::uint64_t>::max() - record.stagedCredit &&
      record.retainedCredit + record.stagedCredit <=
          std::numeric_limits<std::uint64_t>::max() - record.detachedCredit &&
      record.retainedCredit + record.stagedCredit + record.detachedCredit ==
          record.aggregateCredit;
}

constexpr bool directPhysicalCreditKeyEqual(
    const DirectSourceIdentity& lhs,
    const DirectSourceIdentity& rhs) noexcept {
  return lhs.seqId == rhs.seqId &&
      lhs.sourceGeneration == rhs.sourceGeneration &&
      lhs.storageGeneration == rhs.storageGeneration &&
      lhs.destinationSlot == rhs.destinationSlot &&
      lhs.sourceIndex == rhs.sourceIndex && lhs.firstPage == rhs.firstPage &&
      lhs.pageCount == rhs.pageCount;
}

constexpr bool directEventCreditValid(
    const DirectSourceLifecycleEvent& event) noexcept {
  return event.retainedCredit <=
             std::numeric_limits<std::uint64_t>::max() - event.stagedCredit &&
      event.retainedCredit + event.stagedCredit <=
          std::numeric_limits<std::uint64_t>::max() - event.detachedCredit;
}

constexpr bool directLogicalOrdinalAfter(
    std::uint64_t raw, std::uint32_t span,
    std::uint64_t source, std::uint64_t priorRaw, std::uint32_t priorSpan,
    std::uint64_t priorSource) noexcept {
  if (priorRaw == 0) return raw != 0 && span == 0 && source != 0;
  if (raw == priorRaw) {
    return priorSpan != std::numeric_limits<std::uint32_t>::max() &&
        span == priorSpan + 1 && source >= priorSource;
  }
  // A new raw may append to the same physical source/seq, while a rotated
  // source may advance it.  Only reversal is forbidden here; the raw/span
  // pair supplies the exact adjacency proof.
  return raw > priorRaw && span == 0 && source >= priorSource;
}

}  // namespace detail

constexpr DirectSourceLifecycleError reduceDirectSourceLifecycle(
    DirectSourceLifecycleState& state,
    const DirectSourceLifecycleEvent& event) noexcept {
  if (!event.identity.valid() || event.schemaRevision == 0 ||
      !detail::directEventCreditValid(event)) {
    return DirectSourceLifecycleError::InvalidIdentity;
  }

  auto* record = detail::findDirectSource(state, event.identity);
  // Production has no independently observable RawOwned/Planned handoff.
  // Attach the projection at the real admission witness instead. The
  // abstract ImportRaw -> Plan path below remains available to the bounded
  // model and native reducer truth tables, but production must not fabricate
  // either edge.
  if (event.action == DirectSourceAction::AdmitWitness && !record) {
    if (event.witnessGeneration == 0 ||
        state.recordCount == state.records.size()) {
      return event.witnessGeneration == 0
          ? DirectSourceLifecycleError::StaleOrDuplicateWitness
          : DirectSourceLifecycleError::BoundedOverflow;
    }
    if (!detail::directLogicalOrdinalAfter(
            event.identity.rawOrdinal, event.identity.spanOrdinal,
            event.identity.sourceOrdinal, state.lastImportedRawOrdinal,
            state.lastImportedSpanOrdinal, state.lastImportedSourceOrdinal)) {
      return DirectSourceLifecycleError::SourceReordered;
    }
    if (!event.physicalCreditOwner &&
        (event.retainedCredit != 0 || event.stagedCredit != 0 ||
         event.detachedCredit != 0)) {
      return DirectSourceLifecycleError::CreditMismatch;
    }
    bool physicalOwnerFound = false;
    for (std::size_t i = 0; i < state.recordCount; ++i) {
      const auto& prior = state.records[i];
      if (!detail::directPhysicalCreditKeyEqual(prior.identity,
                                                event.identity)) {
        continue;
      }
      if (event.physicalCreditOwner && prior.physicalCreditOwner) {
        return DirectSourceLifecycleError::DuplicateEmission;
      }
      physicalOwnerFound |= prior.physicalCreditOwner;
    }
    if (!event.physicalCreditOwner && !physicalOwnerFound) {
      return DirectSourceLifecycleError::InvalidIdentity;
    }
    record = &state.records[state.recordCount++];
    record->identity = event.identity;
    record->phase = DirectSourcePhase::Admitted;
    record->controlMode = event.controlMode;
    record->creditKind = event.creditKind;
    record->schemaRevision = event.schemaRevision;
    record->witnessGeneration = event.witnessGeneration;
    record->retainedCredit = event.retainedCredit;
    record->stagedCredit = event.stagedCredit;
    record->detachedCredit = event.detachedCredit;
    record->aggregateCredit = event.retainedCredit + event.stagedCredit +
        event.detachedCredit;
    record->physicalCreditOwner = event.physicalCreditOwner;
    record->hasPresent = event.hasPresent;
    record->transitionCount = 1;
    state.lastImportedRawOrdinal = event.identity.rawOrdinal;
    state.lastImportedSpanOrdinal = event.identity.spanOrdinal;
    state.lastImportedSourceOrdinal = event.identity.sourceOrdinal;
    return detail::directCreditConserved(*record)
        ? DirectSourceLifecycleError::None
        : DirectSourceLifecycleError::CreditMismatch;
  }
  if (event.action == DirectSourceAction::ImportRaw) {
    if (record) return DirectSourceLifecycleError::DuplicateEmission;
    if (!detail::directLogicalOrdinalAfter(
            event.identity.rawOrdinal, event.identity.spanOrdinal,
            event.identity.sourceOrdinal, state.lastImportedRawOrdinal,
            state.lastImportedSpanOrdinal, state.lastImportedSourceOrdinal)) {
      return DirectSourceLifecycleError::SourceReordered;
    }
    if (state.recordCount == state.records.size()) {
      return DirectSourceLifecycleError::BoundedOverflow;
    }
    if (!event.physicalCreditOwner &&
        (event.retainedCredit != 0 || event.stagedCredit != 0 ||
         event.detachedCredit != 0)) {
      return DirectSourceLifecycleError::CreditMismatch;
    }
    bool physicalOwnerFound = false;
    if (event.physicalCreditOwner) {
      for (std::size_t i = 0; i < state.recordCount; ++i) {
        const auto& prior = state.records[i];
        if (prior.physicalCreditOwner &&
            detail::directPhysicalCreditKeyEqual(prior.identity,
                                                 event.identity)) {
          return DirectSourceLifecycleError::DuplicateEmission;
        }
      }
    } else {
      for (std::size_t i = 0; i < state.recordCount; ++i) {
        const auto& prior = state.records[i];
        if (prior.physicalCreditOwner &&
            detail::directPhysicalCreditKeyEqual(prior.identity,
                                                 event.identity)) {
          physicalOwnerFound = true;
          break;
        }
      }
      if (!physicalOwnerFound) return DirectSourceLifecycleError::InvalidIdentity;
    }
    record = &state.records[state.recordCount++];
    record->identity = event.identity;
    record->phase = DirectSourcePhase::RawOwned;
    record->controlMode = event.controlMode;
    record->creditKind = event.creditKind;
    record->schemaRevision = event.schemaRevision;
    record->retainedCredit = event.retainedCredit;
    record->stagedCredit = event.stagedCredit;
    record->detachedCredit = event.detachedCredit;
    record->aggregateCredit = event.retainedCredit + event.stagedCredit +
        event.detachedCredit;
    record->physicalCreditOwner = event.physicalCreditOwner;
    record->hasPresent = event.hasPresent;
    record->transitionCount = 1;
    state.lastImportedRawOrdinal = event.identity.rawOrdinal;
    state.lastImportedSpanOrdinal = event.identity.spanOrdinal;
    state.lastImportedSourceOrdinal = event.identity.sourceOrdinal;
    return DirectSourceLifecycleError::None;
  }
  if (!record || record->schemaRevision != event.schemaRevision ||
      record->controlMode != event.controlMode ||
      record->creditKind != event.creditKind ||
      record->physicalCreditOwner != event.physicalCreditOwner ||
      event.partialAdoption) {
    return event.partialAdoption
        ? DirectSourceLifecycleError::PartialAdoption
        : DirectSourceLifecycleError::InvalidIdentity;
  }

  const auto requireCredit = [&]() constexpr {
    return record->retainedCredit == event.retainedCredit &&
        record->stagedCredit == event.stagedCredit &&
        record->detachedCredit == event.detachedCredit;
  };
  switch (event.action) {
  case DirectSourceAction::Plan:
    if (record->phase != DirectSourcePhase::RawOwned || !requireCredit()) {
      return DirectSourceLifecycleError::InvalidTransition;
    }
    record->phase = DirectSourcePhase::Planned;
    break;
  case DirectSourceAction::AdmitWitness:
    if (record->phase != DirectSourcePhase::Planned ||
        event.witnessGeneration == 0 || record->witnessGeneration != 0) {
      return DirectSourceLifecycleError::StaleOrDuplicateWitness;
    }
    if (!requireCredit()) return DirectSourceLifecycleError::CreditMismatch;
    record->witnessGeneration = event.witnessGeneration;
    record->phase = DirectSourcePhase::Admitted;
    break;
  case DirectSourceAction::EffectCut:
    if (record->phase != DirectSourcePhase::Admitted ||
        event.witnessGeneration != record->witnessGeneration ||
        record->witnessConsumed) {
      return DirectSourceLifecycleError::StaleOrDuplicateWitness;
    }
    if (!requireCredit()) return DirectSourceLifecycleError::CreditMismatch;
    record->witnessConsumed = true;
    record->effectStarted = true;
    record->phase = DirectSourcePhase::Effected;
    break;
  case DirectSourceAction::DestinationReceipt:
    if (record->phase != DirectSourcePhase::Effected || !requireCredit()) {
      return DirectSourceLifecycleError::InvalidTransition;
    }
    record->phase = DirectSourcePhase::ReceiptOwned;
    break;
  case DirectSourceAction::Publish:
    if (record->phase != DirectSourcePhase::ReceiptOwned || !requireCredit() ||
        !detail::directLogicalOrdinalAfter(
            event.identity.rawOrdinal, event.identity.spanOrdinal,
            event.identity.sourceOrdinal, state.lastPublishedRawOrdinal,
            state.lastPublishedSpanOrdinal, state.lastPublishedSourceOrdinal)) {
      return !detail::directLogicalOrdinalAfter(
                 event.identity.rawOrdinal, event.identity.spanOrdinal,
                 event.identity.sourceOrdinal, state.lastPublishedRawOrdinal,
                 state.lastPublishedSpanOrdinal,
                 state.lastPublishedSourceOrdinal)
          ? DirectSourceLifecycleError::SourceReordered
          : DirectSourceLifecycleError::InvalidTransition;
    }
    state.lastPublishedRawOrdinal = event.identity.rawOrdinal;
    state.lastPublishedSpanOrdinal = event.identity.spanOrdinal;
    state.lastPublishedSourceOrdinal = event.identity.sourceOrdinal;
    record->hasPresent = event.hasPresent;
    record->phase = DirectSourcePhase::Published;
    ++record->publicationCount;
    break;
  case DirectSourceAction::Encode:
    if (record->phase != DirectSourcePhase::Published || !requireCredit()) {
      return DirectSourceLifecycleError::InvalidTransition;
    }
    record->phase = DirectSourcePhase::Encoded;
    break;
  case DirectSourceAction::Complete:
    if (record->phase != DirectSourcePhase::Encoded || !requireCredit()) {
      return DirectSourceLifecycleError::InvalidTransition;
    }
    record->completed = true;
    record->phase = DirectSourcePhase::Completed;
    break;
  case DirectSourceAction::Detach:
    if (!record->completed || record->phase != DirectSourcePhase::Completed ||
        event.stagedCredit != 0 ||
        (record->physicalCreditOwner
             ? event.detachedCredit == 0 ||
                 event.retainedCredit + event.detachedCredit !=
                     record->aggregateCredit
             : event.retainedCredit != 0 || event.detachedCredit != 0)) {
      return !record->completed
          ? DirectSourceLifecycleError::CompletionRequired
          : DirectSourceLifecycleError::CreditMismatch;
    }
    record->retainedCredit = event.retainedCredit;
    record->stagedCredit = 0;
    record->detachedCredit = event.detachedCredit;
    record->phase = DirectSourcePhase::Detached;
    break;
  case DirectSourceAction::Restore:
    if (record->phase != DirectSourcePhase::Detached ||
        event.detachedCredit != 0 || event.stagedCredit != 0 ||
        (record->physicalCreditOwner
             ? event.retainedCredit != record->aggregateCredit
             : event.retainedCredit != 0)) {
      return DirectSourceLifecycleError::CreditMismatch;
    }
    record->retainedCredit = event.retainedCredit;
    record->detachedCredit = 0;
    record->phase = DirectSourcePhase::Restored;
    break;
  case DirectSourceAction::PoisonAbandon:
    if (record->phase != DirectSourcePhase::Detached ||
        event.detachedCredit != 0 || event.stagedCredit != 0 ||
        (record->physicalCreditOwner
             ? event.retainedCredit != record->aggregateCredit
             : event.retainedCredit != 0)) {
      return DirectSourceLifecycleError::CreditMismatch;
    }
    record->retainedCredit = event.retainedCredit;
    record->detachedCredit = 0;
    record->poisoned = true;
    record->phase = DirectSourcePhase::PoisonAbandoned;
    break;
  case DirectSourceAction::RollbackPreEffect:
    if (record->effectStarted || record->witnessConsumed ||
        (record->phase != DirectSourcePhase::Planned &&
         record->phase != DirectSourcePhase::Admitted)) {
      return DirectSourceLifecycleError::FallbackAfterEffect;
    }
    // Rollback cancels the admitted diagnostic transaction. Keeping a
    // RolledBack row would make the later compatibility Publish look like a
    // malformed Direct transition and would also consume the FIFO ordinal of
    // a source that never became visible. Remove the row and restore the
    // admission frontier as one reducer transaction.
    {
      const auto recordIndex = static_cast<std::size_t>(
          record - state.records.data());
      // The FIFO frontier can be restored only by removing its tail. A
      // non-tail erase could strand a later semantic sibling without its
      // physical-credit owner, or rewind the frontier behind a later source.
      // Callers that need to cancel several siblings must preflight and apply
      // the complete tail group atomically through the batch reducer.
      if (recordIndex + 1 != state.recordCount) {
        return DirectSourceLifecycleError::SourceReordered;
      }
      state.records[--state.recordCount] = {};
      if (state.recordCount == 0) {
        state.lastImportedRawOrdinal = 0;
        state.lastImportedSpanOrdinal = 0;
        state.lastImportedSourceOrdinal = 0;
      } else {
        const auto& prior = state.records[state.recordCount - 1].identity;
        state.lastImportedRawOrdinal = prior.rawOrdinal;
        state.lastImportedSpanOrdinal = prior.spanOrdinal;
        state.lastImportedSourceOrdinal = prior.sourceOrdinal;
      }
    }
    return DirectSourceLifecycleError::None;
  case DirectSourceAction::Reclaim:
    if (!record->completed) {
      return DirectSourceLifecycleError::CompletionRequired;
    }
    if (record->phase != DirectSourcePhase::Restored) {
      return DirectSourceLifecycleError::MissingRestore;
    }
    if (!requireCredit() || !detail::directLogicalOrdinalAfter(
            event.identity.rawOrdinal, event.identity.spanOrdinal,
            event.identity.sourceOrdinal, state.lastReclaimedRawOrdinal,
            state.lastReclaimedSpanOrdinal,
            state.lastReclaimedSourceOrdinal)) {
      return !requireCredit() ? DirectSourceLifecycleError::CreditMismatch
                              : DirectSourceLifecycleError::SourceReordered;
    }
    state.lastReclaimedRawOrdinal = event.identity.rawOrdinal;
    state.lastReclaimedSpanOrdinal = event.identity.spanOrdinal;
    state.lastReclaimedSourceOrdinal = event.identity.sourceOrdinal;
    record->phase = DirectSourcePhase::Reclaimed;
    break;
  case DirectSourceAction::FailStop:
    record->poisoned = true;
    record->phase = DirectSourcePhase::FailStopped;
    break;
  case DirectSourceAction::ImportRaw:
    return DirectSourceLifecycleError::DuplicateEmission;
  }
  ++record->transitionCount;
  if (!detail::directCreditConserved(*record)) {
    return DirectSourceLifecycleError::CreditMismatch;
  }
  return DirectSourceLifecycleError::None;
}

// Reduces an observer batch transactionally. The caller's state is updated
// only after every sibling transition accepts, so native counterexamples and
// the production terminal adapter share the same all-or-nothing policy.
constexpr DirectSourceLifecycleError reduceDirectSourceLifecycleBatch(
    DirectSourceLifecycleState& state,
    std::span<const DirectSourceLifecycleEvent> events,
    bool requireSinglePhysicalCreditOwner = false) noexcept {
  if (requireSinglePhysicalCreditOwner) {
    std::size_t ownerCount = 0;
    for (const auto& event : events) {
      ownerCount += event.physicalCreditOwner ? 1u : 0u;
      if (!events.empty() &&
          !detail::directPhysicalCreditKeyEqual(events.front().identity,
                                                event.identity)) {
        return DirectSourceLifecycleError::InvalidIdentity;
      }
    }
    if (events.empty() || ownerCount != 1u) {
      return DirectSourceLifecycleError::CreditMismatch;
    }
  }

  auto scratch = state;
  for (const auto& event : events) {
    const auto error = reduceDirectSourceLifecycle(scratch, event);
    if (error != DirectSourceLifecycleError::None) return error;
  }
  state = scratch;
  return DirectSourceLifecycleError::None;
}

using DirectSourceLifecycleObserverFn = void (*)(
    void*, const DirectSourceLifecycleEvent&) noexcept;

struct DirectSourceLifecycleObserverSink {
  void* context = nullptr;
  DirectSourceLifecycleObserverFn fn = nullptr;
  // Reports an adapter contract fault without fabricating a lifecycle event.
  // This is diagnostic-only; it never changes queue routing or ownership.
  void (*failFn)(void*) noexcept = nullptr;
};

inline void emitDirectSourceLifecycleEvent(
    DirectSourceLifecycleObserverSink sink,
    const DirectSourceLifecycleEvent& event) noexcept {
  if (sink.fn) sink.fn(sink.context, event);
}

inline void failDirectSourceLifecycleObserver(
    DirectSourceLifecycleObserverSink sink) noexcept {
  if (sink.failFn) sink.failFn(sink.context);
}

static_assert(std::is_trivially_copyable_v<DirectSourceLifecycleEvent>);
static_assert(std::is_standard_layout_v<DirectSourceLifecycleEvent>);

}  // namespace dxmt9::queue
