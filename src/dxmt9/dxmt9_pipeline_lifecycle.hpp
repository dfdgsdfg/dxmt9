#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace dxmt9::queue {

// R-BACK-2.88 / CpuPipelineOwnership.tla.  This is a cold, value-only
// observer contract.  Transition owners pass snapshots; the observer neither
// owns payload spans nor participates in the queue predicate it validates.

enum class PipelineStage : std::uint8_t {
  SourceArrival,
  ProducerOwned,
  RawOwned,
  ReplayBorrowed,
  FinalOwned,
  Encoding,
  GPUInFlight,
  Completed,
  Reclaimed,
};

enum class PipelinePayloadKind : std::uint8_t {
  Legacy,
  Arena,
  PresentOnly,
  StateOnly,
};

enum class PipelineDisposition : std::uint8_t {
  Advance,
  AdmissionWait,
  AdmissionRetry,
  BuildProgress,
  ChildJoin,
  PayloadRetired,
  Completed,
  DeviceLost,
  PreEffectRollback,
  PreEffectReject,
  StateOnly,
  FailStop,
  Shutdown,
};

struct PipelineIdentity {
  std::uint64_t workId = 0;
  std::uint64_t sourceOrdinal = 0;
  std::uint64_t seqId = 0;
  // This is the CpuReadyTape storage generation, not a control-slot index.
  // Keep the full 64-bit domain so an ABA check cannot truncate a wrapped
  // arena identity at the observer boundary.
  std::uint64_t generation = 0;

  constexpr bool valid() const noexcept {
    return workId != 0 && sourceOrdinal != 0 && seqId != 0 && generation != 0;
  }

  constexpr bool operator==(const PipelineIdentity&) const noexcept = default;
};

struct PipelineQueueSnapshot {
  std::uint64_t completedSeq = 0;
  std::uint64_t presentSeq = 0;
  std::uint64_t capacityGeneration = 0;
  std::uint64_t admissionWakeGeneration = 0;
  std::uint32_t occupancy = 0;
  std::uint32_t capacity = 0;
  std::uint32_t admissionWaiters = 0;
  bool stopped = false;
  bool failed = false;

  constexpr bool operator==(const PipelineQueueSnapshot&) const noexcept =
      default;
};

struct PipelineLifecycleEvent {
  PipelineIdentity identity{};
  PipelineStage from = PipelineStage::SourceArrival;
  PipelineStage to = PipelineStage::SourceArrival;
  PipelinePayloadKind payloadKind = PipelinePayloadKind::Legacy;
  PipelineDisposition disposition = PipelineDisposition::Advance;
  std::uint64_t ownedBytes = 0;
  std::uint32_t outstandingBorrows = 0;
  std::uint32_t constructedCount = 0;
  std::uint32_t requiredCount = 0;
  std::uint32_t joinedChildren = 0;
  std::uint32_t totalChildren = 0;
  bool completionAuthority = false;
  PipelineQueueSnapshot before{};
  PipelineQueueSnapshot after{};
};

enum class PipelineObservationError : std::uint8_t {
  None,
  InvalidIdentity,
  BoundedObserverOverflow,
  StaleGeneration,
  DuplicateOrRegressedStage,
  QueueSnapshotDiscontinuity,
  InvalidQueueSnapshot,
  OccupancyMismatch,
  MissingAdmissionWake,
  AdmissionRetryWithoutWake,
  IncompletePublication,
  OutstandingBorrow,
  CompletionBeforeJoin,
  MissingCompletionAuthority,
  CompletionOutOfOrder,
  PresentOutOfOrder,
  InvalidDisposition,
};

constexpr bool pipelineStageOwnsQueueCredit(PipelineStage stage) noexcept {
  return stage == PipelineStage::ReplayBorrowed ||
      stage == PipelineStage::FinalOwned || stage == PipelineStage::Encoding ||
      stage == PipelineStage::GPUInFlight || stage == PipelineStage::Completed;
}

constexpr bool pipelinePublicationMayCommit(
    std::uint32_t constructedCount,
    std::uint32_t requiredCount,
    std::uint32_t outstandingBorrows) noexcept {
  return requiredCount != 0 && constructedCount == requiredCount &&
      outstandingBorrows == 0;
}

constexpr bool pipelineCompletionMayPublish(
    std::uint64_t completedSeq,
    std::uint64_t sourceSeq,
    std::uint32_t outstandingBorrows,
    std::uint32_t joinedChildren,
    std::uint32_t totalChildren,
    bool completionAuthority) noexcept {
  return sourceSeq == completedSeq + 1 && totalChildren != 0 &&
      joinedChildren == totalChildren && outstandingBorrows == 0 &&
      completionAuthority;
}

constexpr bool pipelineOwnerMayReclaim(
    PipelineStage stage,
    PipelineDisposition disposition,
    std::uint32_t outstandingBorrows,
    bool completionAuthority) noexcept {
  if (outstandingBorrows != 0) {
    return false;
  }
  if (stage == PipelineStage::GPUInFlight) {
    return completionAuthority &&
        (disposition == PipelineDisposition::Completed ||
         disposition == PipelineDisposition::DeviceLost);
  }
  if (stage == PipelineStage::Completed) {
    return completionAuthority &&
        (disposition == PipelineDisposition::Completed ||
         disposition == PipelineDisposition::DeviceLost);
  }
  return disposition == PipelineDisposition::PreEffectReject ||
      disposition == PipelineDisposition::StateOnly ||
      disposition == PipelineDisposition::FailStop ||
      disposition == PipelineDisposition::Shutdown;
}

constexpr bool pipelineTransitionRequiresAdmissionWake(
    const PipelineQueueSnapshot& before,
    const PipelineQueueSnapshot& after) noexcept {
  const bool releasedCapacity = after.occupancy < before.occupancy;
  const bool terminalWake = (!before.stopped && after.stopped) ||
      (!before.failed && after.failed);
  return before.admissionWaiters != 0 && (releasedCapacity || terminalWake);
}

constexpr bool pipelineAdmissionWaitSatisfied(
    const PipelineQueueSnapshot& current,
    std::uint64_t observedWakeGeneration) noexcept {
  return current.stopped || current.failed ||
      (current.occupancy < current.capacity &&
       current.admissionWakeGeneration > observedWakeGeneration);
}

struct PipelineLifecycleRecord {
  PipelineIdentity identity{};
  PipelineStage stage = PipelineStage::SourceArrival;
  PipelinePayloadKind payloadKind = PipelinePayloadKind::Legacy;
  std::uint64_t ownedBytes = 0;
  std::uint64_t observedAdmissionWakeGeneration = 0;
  std::uint32_t outstandingBorrows = 0;
  std::uint32_t constructedCount = 0;
  std::uint32_t requiredCount = 0;
  std::uint32_t joinedChildren = 0;
  std::uint32_t totalChildren = 0;
  bool completionAuthority = false;
  bool payloadRetired = false;
  bool occupied = false;
  bool terminal = false;
};

inline constexpr std::size_t kMaxObservedPipelineSources = 8;
inline constexpr std::size_t kMaxObservedPipelineEvents = 256;

struct PipelineLifecycleObserverState {
  std::array<PipelineLifecycleRecord, kMaxObservedPipelineSources> records{};
  std::array<PipelineLifecycleEvent, kMaxObservedPipelineEvents> ownerEvents{};
  std::size_t recordCount = 0;
  std::size_t ownerEventCount = 0;
  PipelineQueueSnapshot queue{};
  bool hasQueueSnapshot = false;
  std::size_t eventCount = 0;
};

namespace detail {

constexpr PipelineLifecycleRecord* findExact(
    PipelineLifecycleObserverState& state,
    const PipelineIdentity& identity) noexcept {
  for (std::size_t i = 0; i < state.recordCount; ++i) {
    if (state.records[i].identity == identity) {
      return &state.records[i];
    }
  }
  return nullptr;
}

constexpr bool sameLogicalIdentity(const PipelineIdentity& a,
                                   const PipelineIdentity& b) noexcept {
  return a.workId == b.workId && a.sourceOrdinal == b.sourceOrdinal &&
      a.seqId == b.seqId;
}

constexpr bool staleGeneration(const PipelineLifecycleObserverState& state,
                               const PipelineIdentity& identity) noexcept {
  for (std::size_t i = 0; i < state.recordCount; ++i) {
    if (sameLogicalIdentity(state.records[i].identity, identity) &&
        state.records[i].identity.generation != identity.generation) {
      return true;
    }
  }
  return false;
}

constexpr PipelineObservationError validateQueueSnapshots(
    const PipelineLifecycleObserverState& state,
    const PipelineLifecycleEvent& event) noexcept {
  const auto& before = event.before;
  const auto& after = event.after;
  if (state.hasQueueSnapshot && state.queue != before) {
    return PipelineObservationError::QueueSnapshotDiscontinuity;
  }
  if (before.capacity == 0 || after.capacity != before.capacity ||
      before.occupancy > before.capacity || after.occupancy > after.capacity ||
      after.completedSeq < before.completedSeq ||
      after.presentSeq < before.presentSeq ||
      after.presentSeq > after.completedSeq ||
      after.capacityGeneration < before.capacityGeneration ||
      after.admissionWakeGeneration < before.admissionWakeGeneration ||
      (before.stopped && !after.stopped) || (before.failed && !after.failed)) {
    return PipelineObservationError::InvalidQueueSnapshot;
  }

  const int expectedOccupancyDelta =
      static_cast<int>(pipelineStageOwnsQueueCredit(event.to)) -
      static_cast<int>(pipelineStageOwnsQueueCredit(event.from));
  if (static_cast<std::int64_t>(after.occupancy) -
          static_cast<std::int64_t>(before.occupancy) !=
      expectedOccupancyDelta) {
    return PipelineObservationError::OccupancyMismatch;
  }

  if (pipelineTransitionRequiresAdmissionWake(before, after) &&
      after.admissionWakeGeneration == before.admissionWakeGeneration) {
    return PipelineObservationError::MissingAdmissionWake;
  }
  return PipelineObservationError::None;
}

constexpr PipelineObservationError validateTransition(
    const PipelineLifecycleRecord& record,
    const PipelineLifecycleEvent& event) noexcept {
  if (record.stage != event.from || record.terminal) {
    return PipelineObservationError::DuplicateOrRegressedStage;
  }

  if (event.disposition == PipelineDisposition::AdmissionWait) {
    if (event.from != PipelineStage::RawOwned ||
        event.to != PipelineStage::RawOwned ||
        event.after.admissionWaiters != event.before.admissionWaiters + 1) {
      return PipelineObservationError::InvalidDisposition;
    }
    return PipelineObservationError::None;
  }

  if (event.disposition == PipelineDisposition::AdmissionRetry) {
    if (event.from != PipelineStage::RawOwned ||
        event.to != PipelineStage::ReplayBorrowed ||
        event.before.admissionWaiters == 0 ||
        event.after.admissionWaiters + 1 != event.before.admissionWaiters ||
        !pipelineAdmissionWaitSatisfied(
            event.before, record.observedAdmissionWakeGeneration) ||
        event.outstandingBorrows != 1) {
      return PipelineObservationError::AdmissionRetryWithoutWake;
    }
    return PipelineObservationError::None;
  }

  if (event.disposition == PipelineDisposition::BuildProgress) {
    if (event.from != PipelineStage::ReplayBorrowed ||
        event.to != PipelineStage::ReplayBorrowed ||
        event.outstandingBorrows != 1 || event.requiredCount == 0 ||
        event.constructedCount <= record.constructedCount ||
        event.constructedCount > event.requiredCount) {
      return PipelineObservationError::InvalidDisposition;
    }
    return PipelineObservationError::None;
  }

  if (event.disposition == PipelineDisposition::ChildJoin) {
    if (event.from != PipelineStage::Encoding ||
        event.to != PipelineStage::Encoding ||
        event.totalChildren == 0 ||
        event.joinedChildren != record.joinedChildren + 1 ||
        event.joinedChildren > event.totalChildren ||
        event.outstandingBorrows + 1 != record.outstandingBorrows) {
      return PipelineObservationError::InvalidDisposition;
    }
    return PipelineObservationError::None;
  }

  if (event.disposition == PipelineDisposition::PayloadRetired) {
    if (event.from != PipelineStage::GPUInFlight ||
        event.to != PipelineStage::GPUInFlight ||
        !event.completionAuthority || event.outstandingBorrows != 0) {
      return PipelineObservationError::InvalidDisposition;
    }
    return PipelineObservationError::None;
  }

  if (event.from == PipelineStage::ProducerOwned &&
      event.to == PipelineStage::RawOwned &&
      event.disposition == PipelineDisposition::Advance) {
    return PipelineObservationError::None;
  }
  if (event.from == PipelineStage::RawOwned &&
      event.to == PipelineStage::ReplayBorrowed &&
      event.disposition == PipelineDisposition::Advance &&
      event.outstandingBorrows == 1) {
    return PipelineObservationError::None;
  }
  if (event.from == PipelineStage::ReplayBorrowed &&
      event.to == PipelineStage::FinalOwned &&
      event.disposition == PipelineDisposition::Advance) {
    return pipelinePublicationMayCommit(
               event.constructedCount, event.requiredCount,
               event.outstandingBorrows)
        ? PipelineObservationError::None
        : event.outstandingBorrows != 0
            ? PipelineObservationError::OutstandingBorrow
            : PipelineObservationError::IncompletePublication;
  }
  if (event.from == PipelineStage::FinalOwned &&
      event.to == PipelineStage::Encoding &&
      event.disposition == PipelineDisposition::Advance &&
      event.outstandingBorrows != 0 && event.totalChildren != 0 &&
      event.joinedChildren == 0) {
    return PipelineObservationError::None;
  }
  if (event.from == PipelineStage::Encoding &&
      event.to == PipelineStage::GPUInFlight &&
      event.disposition == PipelineDisposition::Advance) {
    if (event.totalChildren == 0 ||
        event.joinedChildren != event.totalChildren) {
      return PipelineObservationError::CompletionBeforeJoin;
    }
    if (event.outstandingBorrows != 0) {
      return PipelineObservationError::OutstandingBorrow;
    }
    return event.completionAuthority
        ? PipelineObservationError::None
        : PipelineObservationError::MissingCompletionAuthority;
  }
  if (event.from == PipelineStage::GPUInFlight &&
      event.to == PipelineStage::Completed &&
      (event.disposition == PipelineDisposition::Completed ||
       event.disposition == PipelineDisposition::DeviceLost)) {
    if (!pipelineOwnerMayReclaim(event.from, event.disposition,
                                 event.outstandingBorrows,
                                 event.completionAuthority)) {
      return event.outstandingBorrows != 0
          ? PipelineObservationError::OutstandingBorrow
          : PipelineObservationError::MissingCompletionAuthority;
    }
    if (!pipelineCompletionMayPublish(
            event.before.completedSeq, event.identity.seqId,
            event.outstandingBorrows, event.joinedChildren,
            event.totalChildren, event.completionAuthority) ||
        event.after.completedSeq != event.identity.seqId) {
      return PipelineObservationError::CompletionOutOfOrder;
    }
    const bool present = event.payloadKind == PipelinePayloadKind::PresentOnly;
    if ((present && event.after.presentSeq != event.identity.seqId) ||
        (!present && event.after.presentSeq != event.before.presentSeq)) {
      return PipelineObservationError::PresentOutOfOrder;
    }
    return PipelineObservationError::None;
  }
  if (event.from == PipelineStage::Completed &&
      event.to == PipelineStage::Reclaimed &&
      (event.disposition == PipelineDisposition::Completed ||
       event.disposition == PipelineDisposition::DeviceLost)) {
    if (!pipelineOwnerMayReclaim(event.from, event.disposition,
                                 event.outstandingBorrows,
                                 event.completionAuthority)) {
      return event.outstandingBorrows != 0
          ? PipelineObservationError::OutstandingBorrow
          : PipelineObservationError::MissingCompletionAuthority;
    }
    if (event.after.completedSeq != event.before.completedSeq ||
        event.after.presentSeq != event.before.presentSeq ||
        event.after.occupancy > event.before.occupancy) {
      return PipelineObservationError::CompletionOutOfOrder;
    }
    return PipelineObservationError::None;
  }
  if (event.from == PipelineStage::ReplayBorrowed &&
      event.to == PipelineStage::RawOwned &&
      event.disposition == PipelineDisposition::PreEffectRollback &&
      event.outstandingBorrows == 0) {
    return PipelineObservationError::None;
  }
  if (event.to == PipelineStage::Reclaimed) {
    const bool stageMatchesDisposition =
        (event.disposition == PipelineDisposition::PreEffectReject &&
         event.from == PipelineStage::RawOwned) ||
        (event.disposition == PipelineDisposition::StateOnly &&
         (event.from == PipelineStage::RawOwned ||
          event.from == PipelineStage::FinalOwned)) ||
        (event.disposition == PipelineDisposition::FailStop &&
         event.from == PipelineStage::Encoding) ||
        (event.disposition == PipelineDisposition::Shutdown &&
         event.from != PipelineStage::GPUInFlight);
    const bool terminalSnapshotMatches =
        event.disposition != PipelineDisposition::FailStop ||
        event.after.failed;
    if (stageMatchesDisposition && terminalSnapshotMatches &&
        pipelineOwnerMayReclaim(event.from, event.disposition,
                                event.outstandingBorrows,
                                event.completionAuthority)) {
      return PipelineObservationError::None;
    }
  }
  return PipelineObservationError::InvalidDisposition;
}

}  // namespace detail

constexpr PipelineObservationError reducePipelineLifecycleEvent(
    PipelineLifecycleObserverState& state,
    const PipelineLifecycleEvent& event) noexcept {
  if (!event.identity.valid()) {
    return PipelineObservationError::InvalidIdentity;
  }
  if (detail::staleGeneration(state, event.identity)) {
    return PipelineObservationError::StaleGeneration;
  }
  if (const auto queueError = detail::validateQueueSnapshots(state, event);
      queueError != PipelineObservationError::None) {
    return queueError;
  }

  auto* record = detail::findExact(state, event.identity);
  if (!record) {
    if (event.from != PipelineStage::SourceArrival ||
        event.to != PipelineStage::ProducerOwned ||
        event.disposition != PipelineDisposition::Advance ||
        event.outstandingBorrows != 0) {
      return PipelineObservationError::DuplicateOrRegressedStage;
    }
    if (state.recordCount == state.records.size()) {
      return PipelineObservationError::BoundedObserverOverflow;
    }
    record = &state.records[state.recordCount++];
    record->identity = event.identity;
  } else if (const auto transitionError =
                 detail::validateTransition(*record, event);
             transitionError != PipelineObservationError::None) {
    return transitionError;
  }

  record->stage = event.to;
  record->payloadKind = event.payloadKind;
  record->ownedBytes = event.ownedBytes;
  record->outstandingBorrows = event.outstandingBorrows;
  record->constructedCount = event.constructedCount;
  record->requiredCount = event.requiredCount;
  record->joinedChildren = event.joinedChildren;
  record->totalChildren = event.totalChildren;
  record->completionAuthority = event.completionAuthority;
  record->occupied = pipelineStageOwnsQueueCredit(event.to);
  record->terminal = event.to == PipelineStage::Reclaimed;
  if (event.disposition == PipelineDisposition::AdmissionWait) {
    record->observedAdmissionWakeGeneration =
        event.before.admissionWakeGeneration;
  }
  if (event.disposition == PipelineDisposition::PayloadRetired) {
    record->payloadRetired = true;
  }

  state.queue = event.after;
  state.hasQueueSnapshot = true;
  ++state.eventCount;
  return PipelineObservationError::None;
}

using PipelineLifecycleObserverFn = void (*)(
    void* context, const PipelineLifecycleEvent& event) noexcept;

struct PipelineLifecycleObserverSink {
  void* context = nullptr;
  PipelineLifecycleObserverFn fn = nullptr;

  explicit constexpr operator bool() const noexcept { return fn != nullptr; }
};

// The disabled path is one nullable function-pointer branch.  In particular it
// reads no clock, allocates no storage, and cannot retain a synchronous view.
inline void emitPipelineLifecycleEvent(
    PipelineLifecycleObserverSink sink,
    const PipelineLifecycleEvent& event) noexcept {
  if (sink.fn) {
    sink.fn(sink.context, event);
  }
}

class PipelineLifecycleObserver {
 public:
  void observe(const PipelineLifecycleEvent& event) noexcept {
    if (error_ != PipelineObservationError::None) {
      return;
    }
    error_ = reducePipelineLifecycleEvent(state_, event);
  }

  PipelineLifecycleObserverSink sink() noexcept {
    return {.context = this, .fn = &observeFromSink};
  }

  // Production owners already have the queue's serialized transition record
  // and exact Tape locator. Preserve that evidence verbatim in a bounded cold
  // lane; the strict reducer remains available to deterministic/model tests.
  // This method intentionally performs no allocation, clock read, or atomic
  // operation and is reached only through the opt-in production sink.
  void observeOwnerEvent(const PipelineLifecycleEvent& event) noexcept {
    if (error_ != PipelineObservationError::None) {
      return;
    }
    if (!event.identity.valid()) {
      error_ = PipelineObservationError::InvalidIdentity;
      return;
    }
    if (state_.ownerEventCount == state_.ownerEvents.size()) {
      error_ = PipelineObservationError::BoundedObserverOverflow;
      return;
    }
    state_.ownerEvents[state_.ownerEventCount++] = event;
    ++state_.eventCount;
  }

  PipelineLifecycleObserverSink productionSink() noexcept {
    return {.context = this, .fn = &observeOwnerFromSink};
  }

  constexpr PipelineObservationError error() const noexcept { return error_; }
  constexpr bool valid() const noexcept {
    return error_ == PipelineObservationError::None;
  }
  constexpr const PipelineLifecycleObserverState& state() const noexcept {
    return state_;
  }

 private:
  static void observeFromSink(void* context,
                              const PipelineLifecycleEvent& event) noexcept {
    static_cast<PipelineLifecycleObserver*>(context)->observe(event);
  }

  static void observeOwnerFromSink(
      void* context, const PipelineLifecycleEvent& event) noexcept {
    static_cast<PipelineLifecycleObserver*>(context)->observeOwnerEvent(event);
  }

  PipelineLifecycleObserverState state_{};
  PipelineObservationError error_ = PipelineObservationError::None;
};

}  // namespace dxmt9::queue
