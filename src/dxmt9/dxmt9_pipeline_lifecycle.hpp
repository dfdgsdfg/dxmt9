#pragma once

#include "dxmt9/progress_predicates.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

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
  // Inline, state-only, and pre-effect terminal work with no GPU authority.
  NoGpuTerminal,
  PreEffectRollback,
  PreEffectReject,
  StateOnly,
  FailStop,
  Shutdown,
  // Control-plane terminal rows.  These are deliberately distinct from
  // ordinary queue completion: Reset advances the PE epoch and Teardown
  // drains the remaining owners without inventing a GPU completion.
  Reset,
  Teardown,
  BridgeReject,
  ReplayFailure,
  EncodeFailure,
  PresentSettled,
  // Finish-thread dequeue advances the public waterline but does not change
  // ownership.  Keep it explicit so GPU completion and reclaim can retain
  // their physical ordering in the production trace.
  FinishAdvance,
};

// Identifies the production owner that emitted a lifecycle edge.  This is
// diagnostic metadata only; it does not add an ABI or scheduling boundary.
enum class PipelineOwner : std::uint8_t {
  Queue,
  PeImport,
  Receipt,
  SelectedParallel,
  DeviceLoss,
  Replay,
  DirectPublication,
  LegacyPublication,
  SerialEncode,
  GpuSubmission,
  GpuCompletion,
  Reclaim,
  Reset,
  Teardown,
};

enum class PipelineControl : std::uint8_t {
  Normal,
  Present,
  Reset,
  Teardown,
  DeviceLoss,
  Exception,
};

// This is the production-owned lifecycle vocabulary. The same X-macro is
// parsed by gen_pipeline_lifecycle_table.py, so the C++ validator and the TLA+
// model cannot silently acquire different transition rows.
#define DXMT9_PIPELINE_LIFECYCLE_ROWS(X)                                      \
  X(SourceArrival, ProducerOwned, PeImport, Advance, Normal)                 \
  X(ProducerOwned, RawOwned, Replay, Advance, Normal)                        \
  X(RawOwned, ReplayBorrowed, Replay, Advance, Normal)                       \
  X(RawOwned, RawOwned, Queue, AdmissionWait, Normal)                        \
  X(RawOwned, ReplayBorrowed, Replay, AdmissionRetry, Normal)                 \
  X(ReplayBorrowed, ReplayBorrowed, Replay, BuildProgress, Normal)            \
  X(ReplayBorrowed, ReplayBorrowed, Replay, Advance, Normal)                   \
  X(ReplayBorrowed, FinalOwned, DirectPublication, Advance, Normal)            \
  X(ReplayBorrowed, FinalOwned, LegacyPublication, Advance, Normal)            \
  X(ReplayBorrowed, FinalOwned, Queue, Advance, Normal)                        \
  X(FinalOwned, Encoding, SerialEncode, Advance, Normal)                      \
  X(FinalOwned, Encoding, SelectedParallel, Advance, Normal)                  \
  X(FinalOwned, Encoding, Queue, Advance, Normal)                              \
  X(Encoding, Encoding, Queue, ChildJoin, Normal)                             \
  X(Encoding, GPUInFlight, Receipt, Advance, Normal)                          \
  X(Encoding, GPUInFlight, SelectedParallel, Advance, Normal)                \
  X(Encoding, GPUInFlight, GpuSubmission, Advance, Normal)                    \
  X(Encoding, GPUInFlight, Receipt, Advance, Present)                          \
  X(Encoding, GPUInFlight, SelectedParallel, Advance, Present)                \
  X(GPUInFlight, GPUInFlight, Receipt, PayloadRetired, Normal)                 \
  X(GPUInFlight, Completed, GpuCompletion, Completed, Normal)                  \
  X(GPUInFlight, Completed, Receipt, Completed, Normal)                        \
  X(GPUInFlight, Completed, GpuCompletion, Completed, Present)                 \
  X(GPUInFlight, Completed, Receipt, Completed, Present)                        \
  X(GPUInFlight, Completed, DeviceLoss, DeviceLost, DeviceLoss)               \
  X(Completed, Reclaimed, Reclaim, Completed, Normal)                         \
  X(Completed, Reclaimed, Reclaim, PresentSettled, Present)                   \
  X(Completed, Completed, Queue, FinishAdvance, Normal)                       \
  X(Encoding, Reclaimed, Queue, NoGpuTerminal, Normal)                        \
  X(RawOwned, Reclaimed, Queue, NoGpuTerminal, Normal)                         \
  X(FinalOwned, Reclaimed, Queue, NoGpuTerminal, Normal)                       \
  X(RawOwned, Reclaimed, DeviceLoss, FailStop, DeviceLoss)                    \
  X(Encoding, Reclaimed, DeviceLoss, FailStop, DeviceLoss)                    \
  X(RawOwned, Reclaimed, PeImport, BridgeReject, Exception)                   \
  X(ProducerOwned, Reclaimed, PeImport, BridgeReject, Exception)               \
  X(ReplayBorrowed, RawOwned, Queue, PreEffectRollback, Normal)               \
  X(ReplayBorrowed, Reclaimed, Replay, ReplayFailure, Exception)              \
  X(Encoding, Reclaimed, SerialEncode, EncodeFailure, Exception)               \
  X(Encoding, Reclaimed, SelectedParallel, EncodeFailure, Exception)           \
  X(ProducerOwned, Reclaimed, DeviceLoss, FailStop, DeviceLoss)                \
  X(ReplayBorrowed, Reclaimed, DeviceLoss, FailStop, DeviceLoss)               \
  X(FinalOwned, Reclaimed, DeviceLoss, FailStop, DeviceLoss)                   \
  X(Completed, Reclaimed, DeviceLoss, FailStop, DeviceLoss)                    \
  X(GPUInFlight, Reclaimed, DeviceLoss, FailStop, DeviceLoss)                  \
  X(RawOwned, Reclaimed, Reset, Reset, Reset)                                  \
  X(FinalOwned, Reclaimed, Reset, Reset, Reset)                                \
  X(Encoding, Reclaimed, Reset, Reset, Reset)                                  \
  X(RawOwned, Reclaimed, Teardown, Teardown, Teardown)                         \
  X(FinalOwned, Reclaimed, Teardown, Teardown, Teardown)                       \
  X(Encoding, Reclaimed, Teardown, Teardown, Teardown)                       \
  X(ProducerOwned, Reclaimed, Teardown, Teardown, Teardown)                  \
  X(ReplayBorrowed, Reclaimed, Teardown, Teardown, Teardown)                 \
  X(Completed, Reclaimed, Teardown, Teardown, Teardown)                       \
  X(GPUInFlight, Reclaimed, Teardown, Teardown, Teardown)

struct PipelineLifecycleRow {
  PipelineStage from = PipelineStage::SourceArrival;
  PipelineStage to = PipelineStage::SourceArrival;
  PipelineOwner owner = PipelineOwner::Queue;
  PipelineDisposition disposition = PipelineDisposition::Advance;
  PipelineControl control = PipelineControl::Normal;
};

inline constexpr auto kPipelineLifecycleRows = std::array{
#define DXMT9_PIPELINE_LIFECYCLE_ROW(from, to, owner, disposition, control) \
  PipelineLifecycleRow{PipelineStage::from, PipelineStage::to,              \
                       PipelineOwner::owner, PipelineDisposition::disposition, \
                       PipelineControl::control},
  DXMT9_PIPELINE_LIFECYCLE_ROWS(DXMT9_PIPELINE_LIFECYCLE_ROW)
#undef DXMT9_PIPELINE_LIFECYCLE_ROW
};

constexpr bool pipelineKnownLifecycleRow(
    PipelineStage from, PipelineStage to, PipelineOwner owner,
    PipelineDisposition disposition,
    PipelineControl control = PipelineControl::Normal) noexcept {
  for (const auto& row : kPipelineLifecycleRows) {
    if (row.from == from && row.to == to && row.owner == owner &&
        row.disposition == disposition && row.control == control) {
      return true;
    }
  }
  return false;
}

constexpr bool pipelineEncodeOwnerValid(PipelineOwner owner) noexcept {
  return owner == PipelineOwner::SerialEncode ||
      owner == PipelineOwner::SelectedParallel;
}

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
  // `completedSeq` is retained as the historical finish-waterline alias for
  // existing model/native fixtures. Production snapshots also fill the
  // explicitly named frontiers below.
  std::uint64_t completedSeq = 0;
  std::uint64_t presentSeq = 0;
  std::uint64_t gpuCompletedTailSeq = 0;
  std::uint64_t gpuCompletedPresentSeq = 0;
  std::uint64_t finishWaterlineSeq = 0;
  std::uint32_t completedQueueDepth = 0;
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
  PipelineOwner owner = PipelineOwner::Queue;
  PipelineControl control = PipelineControl::Normal;
  // Present is a control-plane fact independent of payload representation:
  // Legacy/Arena sources may contain Draw records followed by Present.
  bool hasPresent = false;
  std::uint64_t ownedBytes = 0;
  std::uint32_t outstandingBorrows = 0;
  std::uint32_t constructedCount = 0;
  std::uint32_t requiredCount = 0;
  std::uint32_t joinedChildren = 0;
  std::uint32_t totalChildren = 0;
  bool completionAuthority = false;
  // Physical batch correlation is diagnostic only; it never creates a
  // scheduling boundary.  A source event remains independently
  // identity-qualified by PipelineIdentity.
  std::uint64_t physicalBatchId = 0;
  std::uint32_t batchIndex = 0;
  std::uint32_t batchCount = 1;
  PipelineQueueSnapshot before{};
  PipelineQueueSnapshot after{};
};

// Cold control-plane evidence is intentionally separate from per-source
// lifecycle transitions.  Reset/Teardown report the queue boundary and the
// number of identities still live; they never synthesize Reclaimed edges.
struct PipelineControlObservation {
  PipelineControl control = PipelineControl::Normal;
  PipelineDisposition disposition = PipelineDisposition::Advance;
  std::uint64_t epoch = 0;
  std::uint64_t completedSeq = 0;
  std::uint64_t capacityGeneration = 0;
  std::uint32_t liveSourceCount = 0;
  bool drained = false;
};

struct PipelineControlObserverSink {
  using Fn = void (*)(void*, const PipelineControlObservation&) noexcept;
  void* context = nullptr;
  Fn fn = nullptr;
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
  InvalidPhysicalBatch,
  IncompletePhysicalBatch,
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

// R-BACK-2.85 / R-VERIF-2.23: rollback is legal only while the exact raw can
// still be replayed through Legacy without duplicating a semantic or Metal
// effect. DirectChunkSlotReplayLease is the production owner of this cut.
constexpr bool pipelineReplayFailureMayRollback(
    bool semanticEffectsStarted) noexcept {
  return !semanticEffectsStarted;
}

constexpr bool pipelineNoGpuTerminalMayPublish(
    PipelineStage stage,
    PipelineDisposition disposition,
    std::uint32_t outstandingBorrows,
    bool completionAuthority) noexcept {
  return disposition == PipelineDisposition::NoGpuTerminal &&
      (stage == PipelineStage::RawOwned ||
       stage == PipelineStage::FinalOwned || stage == PipelineStage::Encoding) &&
      outstandingBorrows == 0 && !completionAuthority;
}

constexpr bool pipelineOwnerMatchesTransition(
    PipelineOwner owner,
    PipelineStage from,
    PipelineStage to,
    PipelineDisposition disposition) noexcept {
  for (const auto& row : kPipelineLifecycleRows) {
    if (row.from == from && row.to == to && row.owner == owner &&
        row.disposition == disposition) {
      return true;
    }
  }
  return false;
}

constexpr bool pipelineControlIsTerminal(PipelineControl control) noexcept {
  return control == PipelineControl::Reset ||
      control == PipelineControl::Teardown ||
      control == PipelineControl::DeviceLoss;
}

constexpr bool pipelineDispositionIsFailure(
    PipelineDisposition disposition) noexcept {
  return disposition == PipelineDisposition::BridgeReject ||
      disposition == PipelineDisposition::ReplayFailure ||
      disposition == PipelineDisposition::EncodeFailure ||
      disposition == PipelineDisposition::FailStop ||
      disposition == PipelineDisposition::DeviceLost;
}

constexpr bool pipelineControlMatches(
    PipelineControl control, PipelineDisposition disposition,
    PipelinePayloadKind payloadKind, bool hasPresent = false) noexcept {
  if (control == PipelineControl::Present &&
      !hasPresent && payloadKind != PipelinePayloadKind::PresentOnly) {
    return false;
  }
  if (control == PipelineControl::DeviceLoss &&
      disposition != PipelineDisposition::DeviceLost &&
      disposition != PipelineDisposition::FailStop) {
    return false;
  }
  if (control == PipelineControl::Reset &&
      disposition != PipelineDisposition::Reset) {
    return false;
  }
  if (control == PipelineControl::Teardown &&
      disposition != PipelineDisposition::Teardown &&
      disposition != PipelineDisposition::Shutdown) {
    return false;
  }
  return true;
}

// Legacy fixtures populate completedSeq/presentSeq only. Production
// snapshots populate explicit GPU and finish frontiers so completion-watcher
// publication is not confused with finish-thread dequeue.
constexpr std::uint64_t pipelineGpuCompletedSeq(
    const PipelineQueueSnapshot& snapshot) noexcept {
  return snapshot.gpuCompletedTailSeq != 0
      ? snapshot.gpuCompletedTailSeq : snapshot.completedSeq;
}

constexpr std::uint64_t pipelineGpuPresentSeq(
    const PipelineQueueSnapshot& snapshot) noexcept {
  return snapshot.gpuCompletedPresentSeq != 0
      ? snapshot.gpuCompletedPresentSeq : snapshot.presentSeq;
}

constexpr std::uint64_t pipelineFinishWaterlineSeq(
    const PipelineQueueSnapshot& snapshot) noexcept {
  return snapshot.finishWaterlineSeq != 0
      ? snapshot.finishWaterlineSeq : snapshot.completedSeq;
}

// Completion publication is a FIFO prefix: the GPU tail is the finish
// waterline plus the number of completed-but-not-yet-finished entries.  Keep
// this arithmetic explicit so a sparse or wrapped queue cannot be silently
// presented as a contiguous frontier to the observer/model.
constexpr bool pipelineCompletionFrontierContiguous(
    std::uint64_t finishWaterlineSeq, std::uint32_t completedQueueDepth,
    std::uint64_t gpuCompletedTailSeq) noexcept {
  return completedQueueDepth <=
             std::numeric_limits<std::uint64_t>::max() - finishWaterlineSeq &&
      gpuCompletedTailSeq == finishWaterlineSeq + completedQueueDepth;
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
         disposition == PipelineDisposition::DeviceLost ||
         disposition == PipelineDisposition::FailStop);
  }
  if (stage == PipelineStage::Completed) {
    return completionAuthority &&
        (disposition == PipelineDisposition::Completed ||
         disposition == PipelineDisposition::DeviceLost ||
         disposition == PipelineDisposition::FailStop ||
         disposition == PipelineDisposition::PresentSettled);
  }
  return pipelineNoGpuTerminalMayPublish(
             stage, disposition, outstandingBorrows, completionAuthority) ||
      disposition == PipelineDisposition::PreEffectReject ||
      disposition == PipelineDisposition::StateOnly ||
      disposition == PipelineDisposition::FailStop ||
      disposition == PipelineDisposition::Shutdown ||
      disposition == PipelineDisposition::Teardown ||
      disposition == PipelineDisposition::Reset ||
      disposition == PipelineDisposition::BridgeReject ||
      disposition == PipelineDisposition::ReplayFailure ||
      disposition == PipelineDisposition::EncodeFailure;
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

// Match the bounded carried-session source ledger. Production owner
// validation must be able to reduce one 128-source completion chain without
// silently dropping identities from its strict state.
inline constexpr std::size_t kMaxObservedPipelineSources = 256;
// A max-size EncodeSession can contain 128 sources and emit several owner
// edges per source. Keep this cold diagnostic storage large enough for one
// bounded physical batch rather than rejecting a valid batch as observer
// overflow. It is allocated only when the opt-in observer is enabled.
inline constexpr std::size_t kMaxObservedPipelineEvents = 4096;
inline constexpr std::size_t kMaxObservedPipelineControls = 16;

struct PipelineLifecycleObserverState {
  std::array<PipelineLifecycleRecord, kMaxObservedPipelineSources> records{};
  std::array<PipelineLifecycleEvent, kMaxObservedPipelineEvents> ownerEvents{};
  std::array<PipelineControlObservation, kMaxObservedPipelineControls>
      controlEvents{};
  std::size_t recordCount = 0;
  std::size_t ownerEventCount = 0;
  std::size_t controlEventCount = 0;
  PipelineQueueSnapshot queue{};
  bool hasQueueSnapshot = false;
  std::size_t eventCount = 0;
};

namespace detail {

// Physical batching is only a correlation envelope.  Every member still has
// an independent logical identity and must retain its stable zero-based slot
// in that envelope.  The reducer permits repeated lifecycle edges for the
// same member, but never permits an id/index pair to be rebound or an
// identity to move to another index.
constexpr PipelineObservationError validatePhysicalBatchMember(
    const PipelineLifecycleObserverState& state,
    const PipelineLifecycleEvent& event) noexcept {
  if (event.physicalBatchId == 0) {
    return event.batchCount == 1 && event.batchIndex == 0
        ? PipelineObservationError::None
        : PipelineObservationError::InvalidPhysicalBatch;
  }
  if (event.batchCount == 0 || event.batchIndex >= event.batchCount) {
    return PipelineObservationError::InvalidPhysicalBatch;
  }
  bool hasPriorMember = false;
  bool identityWasSeen = false;
  for (std::size_t i = 0; i < state.ownerEventCount; ++i) {
    const auto& prior = state.ownerEvents[i];
    if (prior.physicalBatchId == event.physicalBatchId &&
        prior.identity == event.identity) {
      identityWasSeen = true;
      break;
    }
  }
  for (std::size_t i = 0; i < state.ownerEventCount; ++i) {
    const auto& prior = state.ownerEvents[i];
    if (prior.physicalBatchId != event.physicalBatchId) continue;
    hasPriorMember = true;
    if (prior.batchCount != event.batchCount) {
      return PipelineObservationError::InvalidPhysicalBatch;
    }
    if (prior.batchIndex == event.batchIndex &&
        prior.identity != event.identity) {
      return PipelineObservationError::InvalidPhysicalBatch;
    }
    if (prior.identity == event.identity &&
        prior.batchIndex != event.batchIndex) {
      return PipelineObservationError::InvalidPhysicalBatch;
    }
    if (!identityWasSeen && prior.batchIndex > event.batchIndex &&
        prior.identity != event.identity) {
      return PipelineObservationError::InvalidPhysicalBatch;
    }
    // A physical batch is emitted in prefix order.  Repeated events for an
    // already-seen member are fine; a first event for a later member is not.
    if (!identityWasSeen && prior.batchIndex < event.batchIndex &&
        prior.identity != event.identity) {
      bool earlierMemberSeen = false;
      for (std::size_t j = 0; j < state.ownerEventCount; ++j) {
        if (state.ownerEvents[j].physicalBatchId == event.physicalBatchId &&
            state.ownerEvents[j].batchIndex + 1 == event.batchIndex) {
          earlierMemberSeen = true;
          break;
        }
      }
      if (!earlierMemberSeen) {
        return PipelineObservationError::InvalidPhysicalBatch;
      }
    }
  }
  if (!hasPriorMember && event.batchIndex != 0) {
    return PipelineObservationError::InvalidPhysicalBatch;
  }
  return PipelineObservationError::None;
}

constexpr bool pipelinePhysicalBatchComplete(
    std::span<const PipelineLifecycleEvent> events,
    std::uint64_t physicalBatchId, std::uint32_t batchCount) noexcept {
  if (physicalBatchId == 0 || batchCount == 0) return false;
  std::uint32_t seen = 0;
  for (std::uint32_t index = 0; index < batchCount; ++index) {
    bool memberSeen = false;
    for (const auto& event : events) {
      if (event.physicalBatchId == physicalBatchId &&
          event.batchCount == batchCount && event.batchIndex == index &&
          event.to == PipelineStage::Reclaimed) {
        memberSeen = true;
        break;
      }
    }
    if (!memberSeen) return false;
    ++seen;
  }
  return seen == batchCount;
}

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
      pipelineGpuCompletedSeq(after) < pipelineGpuCompletedSeq(before) ||
      pipelineGpuPresentSeq(after) < pipelineGpuPresentSeq(before) ||
      pipelineFinishWaterlineSeq(after) <
          pipelineFinishWaterlineSeq(before) ||
      after.completedQueueDepth > after.capacity ||
      after.capacityGeneration < before.capacityGeneration ||
      after.admissionWakeGeneration < before.admissionWakeGeneration ||
      (before.stopped && !after.stopped) || (before.failed && !after.failed)) {
    return PipelineObservationError::InvalidQueueSnapshot;
  }

  const bool beforeHasExplicitFrontier =
      before.gpuCompletedTailSeq != 0 || before.finishWaterlineSeq != 0 ||
      before.completedQueueDepth != 0;
  const bool afterHasExplicitFrontier =
      after.gpuCompletedTailSeq != 0 || after.finishWaterlineSeq != 0 ||
      after.completedQueueDepth != 0;
  if ((beforeHasExplicitFrontier &&
       !pipelineCompletionFrontierContiguous(
           pipelineFinishWaterlineSeq(before), before.completedQueueDepth,
           pipelineGpuCompletedSeq(before))) ||
      (afterHasExplicitFrontier &&
       !pipelineCompletionFrontierContiguous(
           pipelineFinishWaterlineSeq(after), after.completedQueueDepth,
           pipelineGpuCompletedSeq(after)))) {
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
  if (!pipelineControlMatches(event.control, event.disposition,
                              event.payloadKind, event.hasPresent)) {
    return PipelineObservationError::InvalidDisposition;
  }
  if (!pipelineKnownLifecycleRow(event.from, event.to, event.owner,
                                 event.disposition, event.control)) {
    return PipelineObservationError::InvalidDisposition;
  }
  if (!pipelineOwnerMatchesTransition(event.owner, event.from, event.to,
                                       event.disposition)) {
    return PipelineObservationError::InvalidDisposition;
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

  if (event.disposition == PipelineDisposition::FinishAdvance) {
    const auto beforeFinish = pipelineFinishWaterlineSeq(event.before);
    const auto afterFinish = pipelineFinishWaterlineSeq(event.after);
    const auto beforeGpu = pipelineGpuCompletedSeq(event.before);
    const auto afterGpu = pipelineGpuCompletedSeq(event.after);
    if (event.from != PipelineStage::Completed ||
        event.to != PipelineStage::Completed ||
        event.owner != PipelineOwner::Queue ||
        event.outstandingBorrows != 0 || !event.completionAuthority ||
        !pipelineCompletionFrontierContiguous(
            beforeFinish, event.before.completedQueueDepth, beforeGpu) ||
        !pipelineCompletionFrontierContiguous(
            afterFinish, event.after.completedQueueDepth, afterGpu) ||
        afterFinish != beforeFinish + 1u ||
        event.before.completedQueueDepth == 0u ||
        event.after.completedQueueDepth + 1u !=
            event.before.completedQueueDepth ||
        afterGpu != beforeGpu) {
      return PipelineObservationError::InvalidDisposition;
    }
    return PipelineObservationError::None;
  }

  if (event.disposition == PipelineDisposition::NoGpuTerminal) {
    return event.to == PipelineStage::Reclaimed &&
        pipelineNoGpuTerminalMayPublish(
            event.from, event.disposition, event.outstandingBorrows,
            event.completionAuthority)
        ? PipelineObservationError::None
        : event.outstandingBorrows != 0
            ? PipelineObservationError::OutstandingBorrow
            : PipelineObservationError::InvalidDisposition;
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
            pipelineGpuCompletedSeq(event.before), event.identity.seqId,
            event.outstandingBorrows, event.joinedChildren,
            event.totalChildren, event.completionAuthority) ||
        pipelineGpuCompletedSeq(event.after) != event.identity.seqId) {
      return PipelineObservationError::CompletionOutOfOrder;
    }
    const bool present = event.hasPresent ||
        event.payloadKind == PipelinePayloadKind::PresentOnly;
    if ((present && pipelineGpuPresentSeq(event.after) != event.identity.seqId) ||
        (!present && pipelineGpuPresentSeq(event.after) !=
             pipelineGpuPresentSeq(event.before))) {
      return PipelineObservationError::PresentOutOfOrder;
    }
    return PipelineObservationError::None;
  }
  if (event.from == PipelineStage::Completed &&
      event.to == PipelineStage::Reclaimed &&
      (event.disposition == PipelineDisposition::Completed ||
       event.disposition == PipelineDisposition::DeviceLost ||
       event.disposition == PipelineDisposition::PresentSettled)) {
    if (!pipelineOwnerMayReclaim(event.from, event.disposition,
                                 event.outstandingBorrows,
                                 event.completionAuthority)) {
      return event.outstandingBorrows != 0
          ? PipelineObservationError::OutstandingBorrow
          : PipelineObservationError::MissingCompletionAuthority;
    }
    if (pipelineFinishWaterlineSeq(event.after) !=
            pipelineFinishWaterlineSeq(event.before) ||
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
         (event.from == PipelineStage::RawOwned ||
          event.from == PipelineStage::Encoding ||
          event.from == PipelineStage::GPUInFlight ||
          event.from == PipelineStage::Completed)) ||
       (event.disposition == PipelineDisposition::Shutdown &&
         event.from != PipelineStage::GPUInFlight) ||
       (event.disposition == PipelineDisposition::Reset &&
        event.from != PipelineStage::GPUInFlight) ||
       (event.disposition == PipelineDisposition::Teardown &&
        event.from != PipelineStage::GPUInFlight) ||
       (event.disposition == PipelineDisposition::BridgeReject &&
        event.from == PipelineStage::RawOwned) ||
       (event.disposition == PipelineDisposition::ReplayFailure &&
        event.from == PipelineStage::ReplayBorrowed) ||
       (event.disposition == PipelineDisposition::EncodeFailure &&
        event.from == PipelineStage::Encoding) ||
       (event.disposition == PipelineDisposition::PresentSettled &&
        event.from == PipelineStage::Completed);
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

// Test/model hook for the explicit physical-batch boundary.  Production
// owners call this only when the last member reaches its terminal edge; it is
// intentionally value-only so malformed missing/skipped members are caught
// without retaining payloads or adding a scheduling dependency.
constexpr bool pipelinePhysicalBatchComplete(
    std::span<const PipelineLifecycleEvent> events,
    std::uint64_t physicalBatchId, std::uint32_t batchCount) noexcept {
  return detail::pipelinePhysicalBatchComplete(events, physicalBatchId,
                                               batchCount);
}

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
        event.owner != PipelineOwner::PeImport ||
        event.outstandingBorrows != 0 ||
        !pipelineKnownLifecycleRow(event.from, event.to, event.owner,
                                   event.disposition, event.control)) {
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
  using FinalizeFn = void (*)(void*, std::uint64_t,
                              std::uint32_t) noexcept;
  FinalizeFn finalizeFn = nullptr;

  explicit constexpr operator bool() const noexcept { return fn != nullptr; }
};

inline void finalizePipelineLifecycleBatch(
    PipelineLifecycleObserverSink sink, std::uint64_t physicalBatchId,
    std::uint32_t batchCount) noexcept {
  if (sink.finalizeFn) {
    sink.finalizeFn(sink.context, physicalBatchId, batchCount);
  }
}

// The disabled path is one nullable function-pointer branch.  In particular it
// reads no clock, allocates no storage, and cannot retain a synchronous view.
inline void emitPipelineLifecycleEvent(
    PipelineLifecycleObserverSink sink,
    const PipelineLifecycleEvent& event) noexcept {
  if (sink.fn) {
    sink.fn(sink.context, event);
  }
}

inline void emitPipelineControlObservation(
    PipelineControlObserverSink sink,
    const PipelineControlObservation& observation) noexcept {
  if (sink.fn) {
    sink.fn(sink.context, observation);
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
  // lane, then run the same strict reducer used by deterministic/model tests.
  // This method intentionally performs no allocation, clock read, or atomic
  // operation and is reached only through the opt-in production sink.
  void observeOwnerEvent(const PipelineLifecycleEvent& event) noexcept {
    if (!event.identity.valid()) {
      if (error_ == PipelineObservationError::None) {
        error_ = PipelineObservationError::InvalidIdentity;
      }
      return;
    }
    if (const auto batchError = detail::validatePhysicalBatchMember(
            state_, event);
        batchError != PipelineObservationError::None) {
      if (error_ == PipelineObservationError::None) {
        error_ = batchError;
      }
      return;
    }
    if (state_.ownerEventCount == state_.ownerEvents.size()) {
      if (error_ == PipelineObservationError::None) {
        error_ = PipelineObservationError::BoundedObserverOverflow;
      }
      return;
    }
    state_.ownerEvents[state_.ownerEventCount++] = event;
    if (error_ == PipelineObservationError::None) {
      const auto validation = reducePipelineLifecycleEvent(state_, event);
      if (validation != PipelineObservationError::None) {
        error_ = validation;
      }
    }
  }

  PipelineLifecycleObserverSink productionSink() noexcept {
    return {.context = this,
            .fn = &observeOwnerFromSink,
            .finalizeFn = &finalizeFromSink};
  }

  // The queue owner calls this when the actual physical reclaim ledger is
  // empty, independently of which member index happened to reclaim last.
  // Completeness is Reclaimed-edge qualified: seeing every identity at an
  // earlier dequeue/submit stage cannot hide a dropped terminal projection.
  PipelineObservationError finalizePhysicalBatch(
      std::uint64_t physicalBatchId, std::uint32_t batchCount) noexcept {
    if (error_ != PipelineObservationError::None) {
      return error_;
    }
    const auto events = std::span<const PipelineLifecycleEvent>(
        state_.ownerEvents.data(), state_.ownerEventCount);
    if (!detail::pipelinePhysicalBatchComplete(events, physicalBatchId,
                                               batchCount)) {
      error_ = PipelineObservationError::IncompletePhysicalBatch;
    }
    return error_;
  }

  void observeControl(const PipelineControlObservation& observation) noexcept {
    if (error_ != PipelineObservationError::None) {
      return;
    }
    if (state_.controlEventCount == state_.controlEvents.size()) {
      error_ = PipelineObservationError::BoundedObserverOverflow;
      return;
    }
    state_.controlEvents[state_.controlEventCount++] = observation;
    ++state_.eventCount;
  }

  PipelineControlObserverSink productionControlSink() noexcept {
    return {.context = this, .fn = &observeControlFromSink};
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

  static void finalizeFromSink(void* context, std::uint64_t physicalBatchId,
                               std::uint32_t batchCount) noexcept {
    static_cast<PipelineLifecycleObserver*>(context)->finalizePhysicalBatch(
        physicalBatchId, batchCount);
  }

  static void observeControlFromSink(
      void* context, const PipelineControlObservation& observation) noexcept {
    static_cast<PipelineLifecycleObserver*>(context)->observeControl(observation);
  }

  PipelineLifecycleObserverState state_{};
  PipelineObservationError error_ = PipelineObservationError::None;
};

}  // namespace dxmt9::queue
