#include "../../../src/dxmt9/dxmt9_pipeline_lifecycle.hpp"

#include <array>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace dxmt9::queue;

void check(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

struct FakeSource {
  PipelineIdentity identity{};
  PipelinePayloadKind payloadKind = PipelinePayloadKind::Arena;
  std::uint64_t physicalBatchId = 0;
  std::uint32_t physicalBatchIndex = 0;
  std::uint32_t physicalBatchCount = 1;
  bool hasPresent = false;
  PipelineStage stage = PipelineStage::SourceArrival;
  std::uint64_t ownedBytes = 0;
  std::uint64_t observedWakeGeneration = 0;
  std::uint32_t borrows = 0;
  std::uint32_t constructed = 0;
  std::uint32_t required = 0;
  std::uint32_t joined = 0;
  std::uint32_t children = 0;
  bool completionAuthority = false;
  bool selectedParallel = false;
  bool effectBoundaryCrossed = false;
};

// Deterministic queue/fake-backend owner.  It drives the same pure admission,
// publication, completion, and reclaim predicates used by the production
// observer.  The only blocking operation is a condition-variable wait whose
// entry and wake are themselves synchronized; no sleep or polling is used.
class FakePipelineQueue {
 public:
  explicit FakePipelineQueue(std::uint32_t capacity = 1,
                             bool omitAdmissionWake = false)
      : omitAdmissionWake_(omitAdmissionWake) {
    queue_.capacity = capacity;
  }

  FakeSource makeSource(std::uint64_t id,
                        PipelinePayloadKind kind,
                        std::uint32_t generation = 1) {
    return {
        .identity = {.workId = id,
                     .sourceOrdinal = id,
                     .seqId = id,
                     .generation = generation},
        .payloadKind = kind,
        .hasPresent = kind == PipelinePayloadKind::PresentOnly,
    };
  }

  void arrive(FakeSource& source) {
    emit(source, PipelineStage::SourceArrival, PipelineStage::ProducerOwned,
         PipelineDisposition::Advance, queue_);
  }

  void adoptRaw(FakeSource& source) {
    const auto before = queue_;
    emit(source, source.stage, PipelineStage::RawOwned,
         PipelineDisposition::Advance, before);
  }

  bool beginReplay(FakeSource& source) {
    if (queue_.occupancy == queue_.capacity) {
      return false;
    }
    const auto before = queue_;
    ++queue_.occupancy;
    source.borrows = 1;
    source.ownedBytes = 64;
    emit(source, source.stage, PipelineStage::ReplayBorrowed,
         PipelineDisposition::Advance, before);
    return true;
  }

  void buildPart(FakeSource& source, std::uint32_t required) {
    const auto before = queue_;
    source.required = required;
    ++source.constructed;
    emit(source, source.stage, source.stage,
         PipelineDisposition::BuildProgress, before);
  }

  void publishFinal(FakeSource& source) {
    const auto before = queue_;
    source.borrows = 0;
    source.ownedBytes = 128;
    emit(source, source.stage, PipelineStage::FinalOwned,
         PipelineDisposition::Advance, before);
  }

  void beginEncoding(FakeSource& source, std::uint32_t children) {
    const auto before = queue_;
    source.children = children;
    source.joined = 0;
    source.selectedParallel = children > 1u;
    source.effectBoundaryCrossed = source.selectedParallel;
    source.borrows = children;
    emit(source, source.stage, PipelineStage::Encoding,
         PipelineDisposition::Advance, before);
  }

  void joinChild(FakeSource& source) {
    const auto before = queue_;
    --source.borrows;
    ++source.joined;
    emit(source, source.stage, source.stage, PipelineDisposition::ChildJoin,
         before);
  }

  void submit(FakeSource& source) {
    const auto before = queue_;
    source.completionAuthority = true;
    emit(source, source.stage, PipelineStage::GPUInFlight,
         PipelineDisposition::Advance, before);
  }

  void retirePayload(FakeSource& source) {
    const auto before = queue_;
    source.ownedBytes = 0;
    emit(source, source.stage, source.stage,
         PipelineDisposition::PayloadRetired, before);
  }

  void waitForAdmission(FakeSource& source) {
    std::unique_lock lock(mutex_);
    parkAdmissionLocked(source);
    admissionEntered_ = true;
    admissionEnteredCv_.notify_all();
    admissionCv_.wait(lock, [&] {
      return pipelineAdmissionWaitSatisfied(
          queue_, source.observedWakeGeneration);
    });

    const auto before = queue_;
    --queue_.admissionWaiters;
    ++queue_.occupancy;
    source.borrows = 1;
    source.ownedBytes = 64;
    emit(source, source.stage, PipelineStage::ReplayBorrowed,
         PipelineDisposition::AdmissionRetry, before);
  }

  void waitUntilAdmissionParked() {
    std::unique_lock lock(mutex_);
    admissionEnteredCv_.wait(lock, [&] { return admissionEntered_; });
  }

  void completeAndNotify(FakeSource& source,
                         PipelineDisposition disposition =
                             PipelineDisposition::Completed) {
    {
      std::lock_guard lock(mutex_);
      completeLocked(source, disposition);
    }
    admissionCv_.notify_all();
  }

  void parkAdmission(FakeSource& source) {
    std::lock_guard lock(mutex_);
    parkAdmissionLocked(source);
  }

  void complete(FakeSource& source,
                PipelineDisposition disposition =
                    PipelineDisposition::Completed) {
    std::lock_guard lock(mutex_);
    completeLocked(source, disposition);
  }

  void rollbackReplay(FakeSource& source) {
    const auto before = queue_;
    source.borrows = 0;
    source.constructed = 0;
    source.required = 0;
    source.ownedBytes = 64;
    --queue_.occupancy;
    ++queue_.capacityGeneration;
    publishAdmissionWakeIfNeeded(before);
    emit(source, source.stage, PipelineStage::RawOwned,
         PipelineDisposition::PreEffectRollback, before);
  }

  void finishStateOnly(FakeSource& source) {
    const auto before = queue_;
    emit(source, source.stage, PipelineStage::Reclaimed,
         PipelineDisposition::NoGpuTerminal, before);
  }

  void failStop(FakeSource& source) {
    const auto before = queue_;
    queue_.failed = true;
    --queue_.occupancy;
    ++queue_.capacityGeneration;
    publishAdmissionWakeIfNeeded(before);
    emit(source, source.stage, PipelineStage::Reclaimed,
         PipelineDisposition::FailStop, before);
  }

  void shutdownWaitingSource(FakeSource& source) {
    const auto before = queue_;
    queue_.stopped = true;
    if (queue_.admissionWaiters != 0) {
      ++queue_.admissionWakeGeneration;
      --queue_.admissionWaiters;
    }
    emit(source, source.stage, PipelineStage::Reclaimed,
         PipelineDisposition::Teardown, before);
  }

  void injectStaleGeneration(const FakeSource& source) {
    auto stale = source;
    ++stale.identity.generation;
    const auto before = queue_;
    emit(stale, stale.stage, stale.stage, PipelineDisposition::AdmissionWait,
         before);
  }

  void injectPrematureReclaim(FakeSource& source) {
    const auto before = queue_;
    --queue_.occupancy;
    ++queue_.capacityGeneration;
    emit(source, source.stage, PipelineStage::Reclaimed,
         PipelineDisposition::Completed, before);
  }

  void injectFabricatedGpuMilestone(FakeSource& source) {
    const auto before = queue_;
    emit(source, PipelineStage::RawOwned, PipelineStage::GPUInFlight,
         PipelineDisposition::Advance, before);
  }

  const PipelineLifecycleObserver& observer() const noexcept {
    return observer_;
  }

  const PipelineLifecycleObserver& ownerObserver() const noexcept {
    return ownerObserver_;
  }

  const PipelineQueueSnapshot& snapshot() const noexcept { return queue_; }

  const std::vector<PipelineLifecycleEvent>& events() const noexcept {
    return events_;
  }

 private:
  void parkAdmissionLocked(FakeSource& source) {
    const auto before = queue_;
    source.observedWakeGeneration = queue_.admissionWakeGeneration;
    ++queue_.admissionWaiters;
    emit(source, source.stage, source.stage,
         PipelineDisposition::AdmissionWait, before);
  }

  void publishAdmissionWakeIfNeeded(const PipelineQueueSnapshot& before) {
    if (before.admissionWaiters != 0 && !omitAdmissionWake_) {
      ++queue_.admissionWakeGeneration;
    }
  }

  void completeLocked(FakeSource& source,
                      PipelineDisposition disposition) {
    const auto beforeCompletion = queue_;
    ++queue_.completedSeq;
    if (source.hasPresent) {
      queue_.presentSeq = source.identity.seqId;
    }
    if (disposition == PipelineDisposition::DeviceLost) {
      queue_.failed = true;
    }
    source.borrows = 0;
    emit(source, source.stage, PipelineStage::Completed, disposition,
         beforeCompletion);
    const auto beforeReclaim = queue_;
    --queue_.occupancy;
    ++queue_.capacityGeneration;
    publishAdmissionWakeIfNeeded(beforeReclaim);
    // Device-loss is the GPU completion disposition; reclaim remains owned by
    // the normal Reclaim owner with the production Completed/PresentSettled
    // terminal row.
    const auto reclaimDisposition = source.hasPresent
        ? PipelineDisposition::PresentSettled
        : PipelineDisposition::Completed;
    emit(source, source.stage, PipelineStage::Reclaimed, reclaimDisposition,
         beforeReclaim);
  }

  void emit(FakeSource& source,
            PipelineStage from,
            PipelineStage to,
            PipelineDisposition disposition,
            const PipelineQueueSnapshot& before) {
    const PipelineLifecycleEvent event{
        .identity = source.identity,
        .from = from,
        .to = to,
        .payloadKind = source.payloadKind,
        .disposition = disposition,
        .owner = disposition == PipelineDisposition::Teardown
            ? PipelineOwner::Teardown
            : from == PipelineStage::SourceArrival
            ? PipelineOwner::PeImport
            : from == PipelineStage::ProducerOwned &&
                    to == PipelineStage::RawOwned
                ? PipelineOwner::Replay
            : from == PipelineStage::RawOwned &&
                    to == PipelineStage::ReplayBorrowed
                ? PipelineOwner::Replay
            : from == PipelineStage::ReplayBorrowed &&
                    to == PipelineStage::FinalOwned
                ? (source.payloadKind == PipelinePayloadKind::Arena
                       ? PipelineOwner::DirectPublication
                       : PipelineOwner::LegacyPublication)
            : from == PipelineStage::ReplayBorrowed &&
                    to == PipelineStage::ReplayBorrowed
                ? PipelineOwner::Replay
            : from == PipelineStage::FinalOwned &&
                    to == PipelineStage::Encoding
                ? (source.children > 1u
                       ? PipelineOwner::SelectedParallel
                       : PipelineOwner::SerialEncode)
            : from == PipelineStage::Encoding &&
                    to == PipelineStage::Encoding &&
                    disposition == PipelineDisposition::ChildJoin
                ? (source.selectedParallel ? PipelineOwner::SelectedParallel
                                            : PipelineOwner::Queue)
            : from == PipelineStage::Encoding &&
                    to == PipelineStage::GPUInFlight
                ? (source.selectedParallel ? PipelineOwner::SelectedParallel
                                            : PipelineOwner::Receipt)
            : from == PipelineStage::GPUInFlight &&
                    to == PipelineStage::GPUInFlight &&
                    disposition == PipelineDisposition::PayloadRetired
                ? PipelineOwner::Receipt
            : from == PipelineStage::GPUInFlight &&
                    disposition == PipelineDisposition::DeviceLost
                ? PipelineOwner::DeviceLoss
            : to == PipelineStage::Reclaimed &&
                    disposition == PipelineDisposition::FailStop &&
                    (from == PipelineStage::RawOwned ||
                     from == PipelineStage::Encoding)
                ? PipelineOwner::DeviceLoss
            : from == PipelineStage::GPUInFlight &&
                        disposition == PipelineDisposition::Completed
                    ? PipelineOwner::GpuCompletion
                    : from == PipelineStage::Completed &&
                            to == PipelineStage::Reclaimed
                        ? PipelineOwner::Reclaim
                    : PipelineOwner::Queue,
        .control = disposition == PipelineDisposition::DeviceLost ||
                disposition == PipelineDisposition::FailStop
            ? PipelineControl::DeviceLoss
            : disposition == PipelineDisposition::Teardown
                ? PipelineControl::Teardown
            : source.hasPresent &&
                    (to == PipelineStage::Completed ||
                     to == PipelineStage::Reclaimed)
                ? PipelineControl::Present
                : PipelineControl::Normal,
        .hasPresent = source.hasPresent,
        .ownedBytes = source.ownedBytes,
        .outstandingBorrows = source.borrows,
        .constructedCount = source.constructed,
        .requiredCount = source.required,
        .joinedChildren = source.joined,
        .totalChildren = source.children,
        .completionAuthority = source.completionAuthority,
        .lifecycleFacts = source.selectedParallel
            ? PipelineLifecycleFacts{
                .valid = true,
                .selectedParallel = true,
                .effectBoundaryCrossed = source.effectBoundaryCrossed,
                .encodeOwner = PipelineOwner::SelectedParallel,
                .childTotal = source.children,
                .joinedChildren = source.joined,
              }
            : PipelineLifecycleFacts{},
        .physicalBatchId = source.physicalBatchId,
        .batchIndex = source.physicalBatchIndex,
        .batchCount = source.physicalBatchCount,
        .before = before,
        .after = queue_,
    };
    source.stage = to;
    events_.push_back(event);
    emitPipelineLifecycleEvent(observer_.sink(), event);
    emitPipelineLifecycleEvent(ownerObserver_.productionSink(), event);
  }

  bool omitAdmissionWake_ = false;
  PipelineQueueSnapshot queue_{};
  PipelineLifecycleObserver observer_{};
  PipelineLifecycleObserver ownerObserver_{};
  std::vector<PipelineLifecycleEvent> events_{};
  std::mutex mutex_{};
  std::condition_variable admissionCv_{};
  std::condition_variable admissionEnteredCv_{};
  bool admissionEntered_ = false;
};

void buildAndSubmit(FakePipelineQueue& queue,
                    FakeSource& source,
                    std::uint32_t children) {
  check(queue.beginReplay(source), "source acquires bounded queue credit");
  queue.buildPart(source, 2);
  queue.buildPart(source, 2);
  queue.publishFinal(source);
  queue.beginEncoding(source, children);
  for (std::uint32_t child = 0; child < children; ++child) {
    queue.joinChild(source);
  }
  queue.submit(source);
}

void selectedParallelRecordedChain() {
  FakePipelineQueue queue;
  auto source = queue.makeSource(1, PipelinePayloadKind::Arena);
  queue.arrive(source);
  queue.adoptRaw(source);
  buildAndSubmit(queue, source, 2);
  queue.complete(source);
  check(queue.ownerObserver().valid(),
        "recorded selected-parallel chain reduces through reclaim");
  std::uint32_t joins = 0;
  bool selectedSubmit = false;
  for (const auto& event : queue.events()) {
    if (event.disposition == PipelineDisposition::ChildJoin) {
      ++joins;
      check(event.owner == PipelineOwner::SelectedParallel,
            "child join retains selected-parallel ownership");
      check(event.lifecycleFacts.valid &&
                event.lifecycleFacts.selectedParallel &&
                event.lifecycleFacts.effectBoundaryCrossed,
            "child join carries the post-gate selected facts");
    }
    if (event.from == PipelineStage::Encoding &&
        event.to == PipelineStage::GPUInFlight) {
      selectedSubmit = event.owner == PipelineOwner::SelectedParallel &&
          event.totalChildren == 2u && event.joinedChildren == 2u;
    }
  }
  check(joins == 2u && selectedSubmit,
        "selected-parallel observer records both joins before submit");
}

void purePredicateTruthTables() {
  check(kPipelineLifecycleRows.size() >= 30u,
        "production lifecycle table retains the bounded transition vocabulary");
  check(pipelineKnownLifecycleRow(
            PipelineStage::FinalOwned, PipelineStage::Encoding,
            PipelineOwner::SerialEncode, PipelineDisposition::Advance),
        "serial encode row is shared by C++ and the model");
  check(pipelineKnownLifecycleRow(
            PipelineStage::FinalOwned, PipelineStage::Encoding,
            PipelineOwner::SelectedParallel, PipelineDisposition::Advance),
        "selected-parallel encode row is shared by C++ and the model");
  check(pipelineKnownLifecycleRow(
            PipelineStage::Encoding, PipelineStage::Reclaimed,
            PipelineOwner::SelectedParallel, PipelineDisposition::EncodeFailure,
            PipelineControl::Exception),
        "control/disposition failure row is shared by C++ and the model");
  check(!pipelineKnownLifecycleRow(
            PipelineStage::Encoding, PipelineStage::GPUInFlight,
            PipelineOwner::Queue, PipelineDisposition::Advance),
        "unknown owner transition is rejected by the production table");
  check(pipelineKnownLifecycleRow(
            PipelineStage::Completed, PipelineStage::Completed,
            PipelineOwner::Queue, PipelineDisposition::FinishAdvance),
        "finish-waterline advancement is an explicit shared lifecycle row");

  check(pipelinePublicationMayCommit(2, 2, 0),
        "complete assembler prefix publishes after borrow return");
  check(!pipelinePublicationMayCommit(1, 2, 0),
        "partial assembler prefix cannot publish");
  check(!pipelinePublicationMayCommit(2, 2, 1),
        "publication cannot escape its replay borrow");

  check(pipelineReplayFailureMayRollback(false),
        "pre-effect construction failure may restore the exact Raw owner");
  check(!pipelineReplayFailureMayRollback(true),
        "post-effect construction failure cannot retry through Legacy");

  check(pipelineCompletionMayPublish(3, 4, 0, 2, 2, true),
        "joined FIFO completion advances");
  check(!pipelineCompletionMayPublish(3, 5, 0, 2, 2, true),
        "completion sequence cannot skip");
  check(!pipelineCompletionMayPublish(3, 4, 0, 1, 2, true),
        "completion authority cannot precede join");

  check(pipelineOwnerMayReclaim(
            PipelineStage::GPUInFlight, PipelineDisposition::Completed, 0,
            true),
        "completed GPU owner can reclaim");
  check(pipelineNoGpuTerminalMayPublish(
            PipelineStage::Encoding, PipelineDisposition::NoGpuTerminal, 0,
            false),
        "zero-command-buffer encoding has an explicit terminal disposition");
  check(!pipelineNoGpuTerminalMayPublish(
             PipelineStage::Encoding, PipelineDisposition::NoGpuTerminal, 1,
             false),
        "a zero-GPU terminal cannot escape an outstanding borrow");
  check(pipelineOwnerMatchesTransition(
            PipelineOwner::PeImport, PipelineStage::SourceArrival,
            PipelineStage::ProducerOwned, PipelineDisposition::Advance),
        "PE/import owns source arrival");
  check(pipelineOwnerMatchesTransition(
            PipelineOwner::Receipt, PipelineStage::Encoding,
            PipelineStage::GPUInFlight, PipelineDisposition::Advance),
        "receipt owns GPU submission");
  check(pipelineOwnerMatchesTransition(
            PipelineOwner::Receipt, PipelineStage::GPUInFlight,
            PipelineStage::Completed, PipelineDisposition::Completed),
        "receipt owns ordinary GPU completion");
  check(pipelineOwnerMatchesTransition(
            PipelineOwner::DeviceLoss, PipelineStage::GPUInFlight,
            PipelineStage::Completed, PipelineDisposition::DeviceLost),
        "device-loss owner settles an in-flight source");
  check(pipelineOwnerMatchesTransition(
            PipelineOwner::DeviceLoss, PipelineStage::RawOwned,
            PipelineStage::Reclaimed, PipelineDisposition::FailStop),
        "device-loss owner fail-stops raw-owned poison work");
  check(pipelineOwnerMatchesTransition(
            PipelineOwner::DeviceLoss, PipelineStage::Encoding,
            PipelineStage::Reclaimed, PipelineDisposition::FailStop),
        "device-loss owner fail-stops encoding poison work");
  check(!pipelineOwnerMatchesTransition(
             PipelineOwner::Queue, PipelineStage::RawOwned,
             PipelineStage::Reclaimed, PipelineDisposition::FailStop),
        "queue cannot masquerade as poison device-loss owner");
  check(!pipelineOwnerMatchesTransition(
             PipelineOwner::Receipt, PipelineStage::GPUInFlight,
             PipelineStage::Completed, PipelineDisposition::DeviceLost),
        "ordinary receipt cannot masquerade as device loss");
  check(pipelineOwnerMatchesTransition(
            PipelineOwner::SelectedParallel, PipelineStage::Encoding,
            PipelineStage::GPUInFlight, PipelineDisposition::Advance),
        "selected-parallel completion is owner-qualified without new behavior");
  check(pipelineOwnerMatchesTransition(
            PipelineOwner::SelectedParallel, PipelineStage::Encoding,
            PipelineStage::Encoding, PipelineDisposition::ChildJoin),
        "selected-parallel child joins are owner-qualified lifecycle rows");
  const PipelineLifecycleFacts selectedFacts{
      .valid = true,
      .selectedParallel = true,
      .effectBoundaryCrossed = true,
      .encodeOwner = PipelineOwner::SelectedParallel,
      .childTotal = 2u,
      .joinedChildren = 1u,
  };
  check(selectedFacts.shapeValid() &&
            pipelineChildJoinMayPublish(selectedFacts, 1u, 1u),
        "selected-parallel facts enforce child total and join prefix");
  check(!pipelineChildJoinMayPublish(selectedFacts, 2u, 1u),
        "selected-parallel facts reject an inconsistent join borrow count");
  check(!pipelineOwnerMatchesTransition(
             PipelineOwner::Queue, PipelineStage::Encoding,
             PipelineStage::GPUInFlight, PipelineDisposition::Advance),
        "GPU submission cannot omit its receipt or selected owner");
  check(!pipelineOwnerMayReclaim(
            PipelineStage::GPUInFlight, PipelineDisposition::Completed, 1,
            true),
        "outstanding borrow blocks reclaim");
  check(pipelineOwnerMayReclaim(
            PipelineStage::GPUInFlight, PipelineDisposition::FailStop, 0,
            true),
        "pending completion poison may fail-stop a GPU-owned source");
  check(pipelineKnownLifecycleRow(
            PipelineStage::GPUInFlight, PipelineStage::Reclaimed,
            PipelineOwner::DeviceLoss, PipelineDisposition::FailStop,
            PipelineControl::DeviceLoss),
        "pending completion poison has an identity-qualified terminal row");

  PipelineQueueSnapshot waiting{
      .admissionWakeGeneration = 8,
      .occupancy = 0,
      .capacity = 1,
      .admissionWaiters = 1,
  };
  check(pipelineAdmissionWaitSatisfied(waiting, 7),
        "capacity release generation wakes admission");
  waiting.admissionWakeGeneration = 7;
  check(!pipelineAdmissionWaitSatisfied(waiting, 7),
        "capacity alone cannot hide a missing notify");
  waiting.stopped = true;
  check(pipelineAdmissionWaitSatisfied(waiting, 7),
        "shutdown releases an admission waiter");

  check(queryGetDataPollSatisfied(4, 4) &&
            !queryGetDataPollSatisfied(3, 4),
        "query poll predicate follows the completed-sequence watermark");
  check(presentTokenWaitSatisfied(3, 3, false, false) &&
            presentTokenWaitSatisfied(0, 3, true, false) &&
            presentTokenWaitSatisfied(0, 3, false, true) &&
            !presentTokenWaitSatisfied(2, 3, false, false),
        "Present-token CV predicate admits completion, stop, or abort only");
  check(ringAdmissionWaitSatisfied(false, false, false, false,
                                           true, false) &&
            ringAdmissionWaitSatisfied(true, false, true, true, false,
                                               true) &&
            !ringAdmissionWaitSatisfied(false, false, true, false,
                                                true, false),
        "ring/admission CV predicate preserves active-build pressure");
}

void explicitCompletionAndFinishFrontiers() {
  PipelineLifecycleObserver observer;
  const PipelineIdentity identity{.workId = 1, .sourceOrdinal = 1,
                                  .seqId = 1, .generation = 7};
  PipelineQueueSnapshot base{
      .completedSeq = 0,
      .presentSeq = 0,
      .gpuCompletedTailSeq = 0,
      .gpuCompletedPresentSeq = 0,
      .finishWaterlineSeq = 0,
      .completedQueueDepth = 0,
      .capacityGeneration = 0,
      .admissionWakeGeneration = 0,
      .occupancy = 0,
      .capacity = 2,
  };
  auto emit = [&](PipelineStage from, PipelineStage to,
                  PipelineDisposition disposition,
                  PipelineOwner owner, PipelineQueueSnapshot before,
                  PipelineQueueSnapshot after, std::uint32_t borrows = 0,
                  std::uint32_t joined = 0, bool authority = false) {
    emitPipelineLifecycleEvent(
        observer.productionSink(), PipelineLifecycleEvent{
            .identity = identity,
            .from = from,
            .to = to,
            .payloadKind = PipelinePayloadKind::Arena,
            .disposition = disposition,
            .owner = owner,
            .ownedBytes = 64,
            .outstandingBorrows = borrows,
            .constructedCount = 2,
            .requiredCount = 2,
            .joinedChildren = joined,
            .totalChildren = 1,
            .completionAuthority = authority,
            .before = before,
            .after = after});
  };
  auto owned = base;
  owned.occupancy = 1;
  emit(PipelineStage::SourceArrival, PipelineStage::ProducerOwned,
       PipelineDisposition::Advance, PipelineOwner::PeImport, base, base);
  emit(PipelineStage::ProducerOwned, PipelineStage::RawOwned,
       PipelineDisposition::Advance, PipelineOwner::Replay, base, base);
  emit(PipelineStage::RawOwned, PipelineStage::ReplayBorrowed,
       PipelineDisposition::Advance, PipelineOwner::Replay, base, owned, 1);
  emit(PipelineStage::ReplayBorrowed, PipelineStage::FinalOwned,
       PipelineDisposition::Advance, PipelineOwner::DirectPublication, owned,
       owned);
  emit(PipelineStage::FinalOwned, PipelineStage::Encoding,
       PipelineDisposition::Advance, PipelineOwner::SerialEncode, owned, owned,
       1);
  auto gpu = owned;
  gpu.gpuCompletedTailSeq = 1;
  gpu.completedQueueDepth = 1;
  emit(PipelineStage::Encoding, PipelineStage::GPUInFlight,
       PipelineDisposition::Advance, PipelineOwner::Receipt, owned, owned,
       0, 1, true);
  emit(PipelineStage::GPUInFlight, PipelineStage::Completed,
       PipelineDisposition::Completed, PipelineOwner::GpuCompletion, owned,
       gpu, 0, 1, true);
  auto finish = gpu;
  finish.finishWaterlineSeq = 1;
  finish.completedSeq = 1;
  finish.completedQueueDepth = 0;
  emit(PipelineStage::Completed, PipelineStage::Completed,
       PipelineDisposition::FinishAdvance, PipelineOwner::Queue, gpu, finish,
       0, 1, true);
  auto reclaimed = finish;
  reclaimed.occupancy = 0;
  emit(PipelineStage::Completed, PipelineStage::Reclaimed,
       PipelineDisposition::Completed, PipelineOwner::Reclaim, finish,
       reclaimed, 0, 1, true);
  check(observer.valid(),
        "explicit GPU completion and finish waterline frontiers refine");
}

void completionFrontierTruthTable() {
  check(pipelineCompletionFrontierContiguous(4, 3, 7),
        "completed FIFO tail is the finish waterline plus queue depth");
  check(!pipelineCompletionFrontierContiguous(4, 3, 8),
        "a sparse completed FIFO tail is rejected");
  check(!pipelineCompletionFrontierContiguous(
             std::numeric_limits<std::uint64_t>::max(), 1, 0),
        "completed FIFO frontier overflow is rejected");
}

void lifecycleArithmeticTruthTable() {
  std::uint64_t frontier = 0;
  check(pipelineCheckedFrontierAdd(7, 5, frontier) && frontier == 12,
        "completion frontier addition preserves an in-range sum");
  frontier = 41;
  check(!pipelineCheckedFrontierAdd(
            std::numeric_limits<std::uint64_t>::max(), 1, frontier) &&
            frontier == 41,
        "completion frontier overflow fails before mutating evidence");

  std::uint32_t occupancy = 0;
  check(pipelineCheckedOccupancyAdd(7, 5, occupancy) && occupancy == 12,
        "queue occupancy addition preserves an in-range sum");
  occupancy = 23;
  check(!pipelineCheckedOccupancyAdd(
            std::numeric_limits<std::uint32_t>::max(), 1, occupancy) &&
            occupancy == 23,
        "queue occupancy overflow fails before mutating evidence");
}

PipelineLifecycleEvent batchMemberEvent(PipelineIdentity identity,
                                        std::uint32_t index,
                                        std::uint32_t count) {
  PipelineQueueSnapshot queue{};
  queue.capacity = 2;
  return PipelineLifecycleEvent{
      .identity = identity,
      .from = PipelineStage::SourceArrival,
      .to = PipelineStage::ProducerOwned,
      .payloadKind = PipelinePayloadKind::Arena,
      .disposition = PipelineDisposition::Advance,
      .owner = PipelineOwner::PeImport,
      .physicalBatchId = 91,
      .batchIndex = index,
      .batchCount = count,
      .before = queue,
      .after = queue};
}

void physicalBatchMemberTruthTable() {
  const PipelineIdentity first{.workId = 1, .sourceOrdinal = 1,
                               .seqId = 1, .generation = 1};
  const PipelineIdentity second{.workId = 2, .sourceOrdinal = 2,
                                .seqId = 2, .generation = 1};
  {
    PipelineLifecycleObserver observer;
    emitPipelineLifecycleEvent(observer.productionSink(),
                               batchMemberEvent(first, 0, 2));
    emitPipelineLifecycleEvent(observer.productionSink(),
                               batchMemberEvent(second, 1, 2));
    check(observer.valid(), "two-source physical batch preserves member order");
  }
  {
    PipelineLifecycleObserver observer;
    emitPipelineLifecycleEvent(observer.productionSink(),
                               batchMemberEvent(first, 1, 2));
    check(observer.error() == PipelineObservationError::InvalidPhysicalBatch,
          "skipped physical batch member is rejected");
  }
  {
    PipelineLifecycleObserver observer;
    emitPipelineLifecycleEvent(observer.productionSink(),
                               batchMemberEvent(first, 0, 2));
    emitPipelineLifecycleEvent(observer.productionSink(),
                               batchMemberEvent(second, 0, 2));
    check(observer.error() == PipelineObservationError::InvalidPhysicalBatch,
          "duplicate physical batch index is rejected");
  }
  {
    PipelineLifecycleObserver observer;
    emitPipelineLifecycleEvent(observer.productionSink(),
                               batchMemberEvent(first, 0, 2));
    emitPipelineLifecycleEvent(observer.productionSink(),
                               batchMemberEvent(first, 1, 2));
    check(observer.error() == PipelineObservationError::InvalidPhysicalBatch,
          "moving one identity to another physical batch index is rejected");
  }
  const auto firstEvent = batchMemberEvent(first, 0, 2);
  const auto secondEvent = batchMemberEvent(second, 1, 2);
  auto firstReclaim = firstEvent;
  firstReclaim.to = PipelineStage::Reclaimed;
  auto secondReclaim = secondEvent;
  secondReclaim.to = PipelineStage::Reclaimed;
  const std::array<PipelineLifecycleEvent, 1> missing{firstReclaim};
  const std::array<PipelineLifecycleEvent, 2> complete{
      firstReclaim, secondReclaim};
  check(!pipelinePhysicalBatchComplete(missing, 91, 2),
        "physical batch closure rejects a missing member");
  check(pipelinePhysicalBatchComplete(complete, 91, 2),
        "physical batch closure accepts every ordered member");
}

void endToEndProducerIdentityTruthTable() {
  PipelineQueueSnapshot queue{};
  queue.capacity = 4u;
  const auto arrival = [&](PipelineIdentity identity) {
    return PipelineLifecycleEvent{
        .identity = identity,
        .from = PipelineStage::SourceArrival,
        .to = PipelineStage::ProducerOwned,
        .payloadKind = PipelinePayloadKind::Arena,
        .disposition = PipelineDisposition::Advance,
        .owner = PipelineOwner::PeImport,
        .before = queue,
        .after = queue};
  };
  const PipelineIdentity complete{
      .firstProducerEventOrdinal = 7u,
      .lastProducerEventOrdinal = 8u,
      .firstProducerSourceOrdinal = 11u,
      .lastProducerSourceOrdinal = 14u,
      .workId = 31u,
      .sourceOrdinal = 41u,
      .seqId = 51u,
      .generation = 61u,
  };
  {
    PipelineLifecycleObserver observer(true, true);
    emitPipelineLifecycleEvent(observer.productionSink(), arrival(complete));
    check(observer.valid(),
          "strict production observer accepts a complete PE-to-storage identity");
  }
  {
    PipelineLifecycleObserver observer(true, true);
    auto missing = complete;
    missing.firstProducerEventOrdinal = 0u;
    missing.lastProducerEventOrdinal = 0u;
    missing.firstProducerSourceOrdinal = 0u;
    missing.lastProducerSourceOrdinal = 0u;
    emitPipelineLifecycleEvent(observer.productionSink(), arrival(missing));
    check(observer.error() == PipelineObservationError::InvalidIdentity,
          "strict production observer rejects a missing PE identity");
  }
  {
    PipelineLifecycleObserver observer(true, true);
    auto partial = complete;
    partial.lastProducerEventOrdinal = 0u;
    emitPipelineLifecycleEvent(observer.productionSink(), arrival(partial));
    check(observer.error() == PipelineObservationError::InvalidIdentity,
          "strict production observer rejects a partial PE identity");
  }
  {
    PipelineLifecycleObserver observer(true, true);
    auto otherRaw = complete;
    otherRaw.firstProducerSourceOrdinal = 15u;
    otherRaw.lastProducerSourceOrdinal = 18u;
    otherRaw.workId = 32u;
    otherRaw.sourceOrdinal = 42u;
    otherRaw.seqId = 52u;
    emitPipelineLifecycleEvent(observer.productionSink(), arrival(complete));
    emitPipelineLifecycleEvent(observer.productionSink(), arrival(otherRaw));
    check(observer.error() ==
              PipelineObservationError::DuplicateOrRegressedStage,
          "a different raw source cannot reuse a producer event interval");
  }
  {
    PipelineLifecycleObserver observer(true, true);
    auto otherRaw = complete;
    otherRaw.firstProducerEventOrdinal = 9u;
    otherRaw.lastProducerEventOrdinal = 10u;
    otherRaw.workId = 32u;
    otherRaw.sourceOrdinal = 42u;
    otherRaw.seqId = 52u;
    emitPipelineLifecycleEvent(observer.productionSink(), arrival(complete));
    emitPipelineLifecycleEvent(observer.productionSink(), arrival(otherRaw));
    check(observer.error() ==
              PipelineObservationError::DuplicateOrRegressedStage,
          "a different raw source cannot reuse a producer source interval");
  }
  {
    PipelineLifecycleObserver observer(true, true);
    auto segmented = complete;
    segmented.sourceOrdinal = 42u;
    segmented.seqId = 52u;
    emitPipelineLifecycleEvent(observer.productionSink(), arrival(complete));
    emitPipelineLifecycleEvent(observer.productionSink(), arrival(segmented));
    check(observer.valid(),
          "one raw producer interval may qualify several SegmentSerial sources");
  }
}

void physicalBatchOwnerChainRetainsReclaimClosure() {
  FakePipelineQueue queue;
  auto first = queue.makeSource(1, PipelinePayloadKind::Arena);
  auto second = queue.makeSource(2, PipelinePayloadKind::Arena);
  first.physicalBatchId = second.physicalBatchId = 91;
  first.physicalBatchIndex = 0;
  second.physicalBatchIndex = 1;
  first.physicalBatchCount = second.physicalBatchCount = 2;

  for (auto* source : {&first, &second}) {
    queue.arrive(*source);
    queue.adoptRaw(*source);
    check(queue.beginReplay(*source),
          "physical batch source acquires one replay borrow");
    queue.buildPart(*source, 1);
    queue.publishFinal(*source);
    queue.beginEncoding(*source, 1);
    queue.joinChild(*source);
    queue.submit(*source);
    queue.complete(*source);
  }

  check(queue.ownerObserver().valid(),
        "physical batch owner chain reduces through both source reclaims");
  const auto& events = queue.ownerObserver().state();
  bool sawReclaim[2] = {false, false};
  for (std::size_t i = 0; i < events.ownerEventCount; ++i) {
    const auto& event = events.ownerEvents[i];
    if (event.to == PipelineStage::Reclaimed &&
        event.physicalBatchId == 91 && event.batchCount == 2 &&
        event.batchIndex < 2) {
      sawReclaim[event.batchIndex] = true;
    }
  }
  check(sawReclaim[0] && sawReclaim[1],
        "physical batch identity reaches both Completed-to-Reclaimed edges");
}

void physicalBatchOwnerFinalizeRejectsOmittedTail() {
  PipelineLifecycleObserver observer;
  const PipelineIdentity first{.workId = 1, .sourceOrdinal = 1,
                               .seqId = 1, .generation = 1};
  const PipelineIdentity second{.workId = 2, .sourceOrdinal = 2,
                                .seqId = 2, .generation = 1};
  const auto sink = observer.productionSink();
  emitPipelineLifecycleEvent(sink, batchMemberEvent(first, 0, 2));
  emitPipelineLifecycleEvent(sink, batchMemberEvent(second, 1, 2));
  finalizePipelineLifecycleBatch(sink, 91, 2);
  check(observer.error() == PipelineObservationError::IncompletePhysicalBatch,
        "observer boundary rejects an omitted terminal batch projection even "
        "when every member appeared at an earlier stage");
}

void mixedDrawPresentSourceCompletes() {
  FakePipelineQueue queue;
  auto mixed = queue.makeSource(1, PipelinePayloadKind::Arena);
  mixed.hasPresent = true;
  queue.arrive(mixed);
  queue.adoptRaw(mixed);
  buildAndSubmit(queue, mixed, 1);
  queue.complete(mixed);
  check(queue.snapshot().presentSeq == 1,
        "mixed Draw+Present source advances present settlement on Arena payload");
  check(queue.observer().valid(),
        "mixed Draw+Present source remains valid through completion/reclaim");
  check(pipelineControlMatches(PipelineControl::Present,
                               PipelineDisposition::Completed,
                               PipelinePayloadKind::Arena, true),
        "Present control accepts a mixed Arena payload with present evidence");
  check(pipelineControlMatches(PipelineControl::Present,
                               PipelineDisposition::PresentSettled,
                               PipelinePayloadKind::Arena, true),
        "Present control accepts mixed Arena present settlement");
  check(pipelineKnownLifecycleRow(
            PipelineStage::Encoding, PipelineStage::GPUInFlight,
            PipelineOwner::Receipt, PipelineDisposition::Advance,
            PipelineControl::Present),
        "mixed Draw+Present submit consumes the production Present row");
  check(pipelineKnownLifecycleRow(
            PipelineStage::GPUInFlight, PipelineStage::Completed,
            PipelineOwner::Receipt, PipelineDisposition::Completed,
            PipelineControl::Present),
        "mixed Draw+Present completion consumes the production Present row");
}

void presentOnlyAdmissionWedgeCompletes() {
  FakePipelineQueue queue;
  auto first = queue.makeSource(1, PipelinePayloadKind::Arena);
  auto presentOnly = queue.makeSource(2, PipelinePayloadKind::PresentOnly);

  queue.arrive(first);
  queue.adoptRaw(first);
  buildAndSubmit(queue, first, 2);

  queue.arrive(presentOnly);
  queue.adoptRaw(presentOnly);
  std::thread waiter([&] { queue.waitForAdmission(presentOnly); });
  queue.waitUntilAdmissionParked();
  queue.completeAndNotify(first);
  waiter.join();

  queue.buildPart(presentOnly, 2);
  queue.buildPart(presentOnly, 2);
  queue.publishFinal(presentOnly);
  queue.beginEncoding(presentOnly, 1);
  queue.joinChild(presentOnly);
  queue.submit(presentOnly);
  queue.retirePayload(presentOnly);
  queue.complete(presentOnly);
  check(queue.observer().valid(),
        "end-to-end production observer accepts the bounded pipeline");
  check(queue.snapshot().completedSeq == 2,
        "present-only wedge advances completion sequence");
  check(queue.snapshot().presentSeq == 2,
        "present-only wedge advances present sequence");
  check(queue.snapshot().occupancy == 0,
        "both final owners reclaim their queue credit");
  check(queue.snapshot().capacityGeneration == 2,
        "both reclaims advance capacity generation");
  check(queue.snapshot().admissionWakeGeneration == 1,
        "the occupied-head reclaim wakes one admission waiter");
  check(queue.observer().state().recordCount == 2,
        "observer conserves both exact identities");
  check(queue.observer().state().records[1].payloadRetired,
        "early payload retirement stutters inside GPUInFlight");
  check(queue.events().size() == queue.observer().state().eventCount,
        "every production-owner event reaches the observer once");
  check(queue.events().size() == queue.ownerObserver().state().ownerEventCount,
        "owner sink retains each CV-boundary event without allocation");
}

void composedThreeAxisWakeIsDeterministic() {
  PipelineQueueSnapshot before{
      .completedSeq = 4,
      .presentSeq = 3,
      .capacityGeneration = 9,
      .admissionWakeGeneration = 12,
      .occupancy = 1,
      .capacity = 1,
      .admissionWaiters = 1,
  };
  check(!queryGetDataPollSatisfied(before.completedSeq, 5) &&
            !presentTokenWaitSatisfied(before.presentSeq, 4, false,
                                               false) &&
            !ringAdmissionWaitSatisfied(
                false, false, false, false, false, true),
        "composed wake starts with all three production predicates closed");

  const PipelineQueueSnapshot after{
      .completedSeq = 5,
      .presentSeq = 4,
      .capacityGeneration = 10,
      .admissionWakeGeneration = 13,
      .occupancy = 0,
      .capacity = 1,
      .admissionWaiters = 1,
  };
  check(queryGetDataPollSatisfied(after.completedSeq, 5) &&
            presentTokenWaitSatisfied(after.presentSeq, 4, false,
                                               false) &&
            ringAdmissionWaitSatisfied(
                false, false, false, false, true, false) &&
            pipelineTransitionRequiresAdmissionWake(before, after),
        "one deterministic completion/reclaim event opens all three axes");
}

void deliberateNativeCounterexamples() {
  {
    FakePipelineQueue queue(1, true);
    auto first = queue.makeSource(1, PipelinePayloadKind::Arena);
    auto waiter = queue.makeSource(2, PipelinePayloadKind::PresentOnly);
    queue.arrive(first);
    queue.adoptRaw(first);
    buildAndSubmit(queue, first, 1);
    queue.arrive(waiter);
    queue.adoptRaw(waiter);
    queue.parkAdmission(waiter);
    queue.complete(first);
    check(queue.observer().error() ==
              PipelineObservationError::MissingAdmissionWake,
          "missing reclaim notify is rejected deterministically");
  }

  {
    FakePipelineQueue queue;
    auto source = queue.makeSource(1, PipelinePayloadKind::Arena);
    queue.arrive(source);
    queue.adoptRaw(source);
    check(queue.beginReplay(source), "partial publication fixture reserves");
    queue.buildPart(source, 2);
    queue.publishFinal(source);
    check(queue.observer().error() ==
              PipelineObservationError::IncompletePublication,
          "partial assembler publication is rejected");
  }

  {
    FakePipelineQueue queue;
    auto source = queue.makeSource(1, PipelinePayloadKind::Arena);
    queue.arrive(source);
    queue.adoptRaw(source);
    check(queue.beginReplay(source), "join fixture reserves");
    queue.buildPart(source, 2);
    queue.buildPart(source, 2);
    queue.publishFinal(source);
    queue.beginEncoding(source, 2);
    queue.submit(source);
    check(queue.observer().error() ==
              PipelineObservationError::CompletionBeforeJoin,
          "completion authority before child join is rejected");
  }

  {
    FakePipelineQueue queue;
    auto source = queue.makeSource(1, PipelinePayloadKind::Arena);
    queue.arrive(source);
    queue.adoptRaw(source);
    check(queue.beginReplay(source), "reclaim fixture reserves");
    queue.buildPart(source, 2);
    queue.buildPart(source, 2);
    queue.publishFinal(source);
    queue.beginEncoding(source, 1);
    queue.injectPrematureReclaim(source);
    check(!queue.observer().valid(),
          "owner reclaim without completion authority is rejected");
  }

  {
    FakePipelineQueue queue;
    auto source = queue.makeSource(1, PipelinePayloadKind::StateOnly);
    queue.arrive(source);
    queue.adoptRaw(source);
    queue.injectFabricatedGpuMilestone(source);
    check(queue.observer().error() != PipelineObservationError::None,
          "fabricated GPU milestones cannot stand in for no-GPU terminal work");
  }

  {
    FakePipelineQueue queue;
    auto source = queue.makeSource(1, PipelinePayloadKind::Arena);
    queue.arrive(source);
    queue.injectStaleGeneration(source);
    check(queue.observer().error() == PipelineObservationError::StaleGeneration,
          "stale storage generation is rejected");
  }
}

void ownerEvidenceOverflowFailsClosed() {
  PipelineLifecycleObserver observer;
  const auto sink = observer.productionSink();
  PipelineLifecycleEvent event{};
  PipelineQueueSnapshot queue{};
  queue.capacity = static_cast<std::uint32_t>(kMaxObservedPipelineSources);
  for (std::size_t i = 0; i < kMaxObservedPipelineSources; ++i) {
    event = {};
    event.identity = PipelineIdentity{
        .workId = i + 1u, .sourceOrdinal = i + 1u, .seqId = i + 1u,
        .generation = i + 1u};
    event.from = PipelineStage::SourceArrival;
    event.to = PipelineStage::ProducerOwned;
    event.disposition = PipelineDisposition::Advance;
    event.owner = PipelineOwner::PeImport;
    event.before = queue;
    event.after = queue;
    emitPipelineLifecycleEvent(sink, event);

    event.from = PipelineStage::ProducerOwned;
    event.to = PipelineStage::RawOwned;
    event.owner = PipelineOwner::Replay;
    emitPipelineLifecycleEvent(sink, event);

    auto owned = queue;
    ++owned.occupancy;
    event.from = PipelineStage::RawOwned;
    event.to = PipelineStage::ReplayBorrowed;
    event.owner = PipelineOwner::Replay;
    event.outstandingBorrows = 1;
    event.before = queue;
    event.after = owned;
    emitPipelineLifecycleEvent(sink, event);

    event.from = PipelineStage::ReplayBorrowed;
    event.to = PipelineStage::FinalOwned;
    event.owner = PipelineOwner::DirectPublication;
    event.outstandingBorrows = 0;
    event.constructedCount = 1;
    event.requiredCount = 1;
    event.before = owned;
    event.after = owned;
    emitPipelineLifecycleEvent(sink, event);
    queue = owned;
  }
  static_assert(kMaxObservedPipelineEvents >= 2048,
                "the observer must hold a bounded 128-source lifecycle trace");
  check(observer.valid() && observer.state().ownerEventCount ==
            kMaxObservedPipelineSources * 4u,
        "128-source lifecycle evidence fits below the fixed event capacity");
}

void lifecycleSidecarGenerationAndSaturation() {
  PipelineLifecycleSidecar sidecar;
  const PipelineLifecycleFacts facts{
      .valid = true,
      .selectedParallel = false,
      .effectBoundaryCrossed = false,
      .encodeOwner = PipelineOwner::SerialEncode,
      .childTotal = 1u,
      .joinedChildren = 1u,
  };
  for (std::uint64_t seq = 1u; seq <= 256u; ++seq) {
    check(sidecar.recordFacts(seq, 1u, facts) ==
              PipelineLifecycleSidecarResult::Stored,
          "sidecar accepts its bounded generation-qualified capacity");
  }
  check(sidecar.recordFacts(257u, 1u, facts) ==
            PipelineLifecycleSidecarResult::BoundedOverflow &&
            sidecar.overflowCount() == 1u,
        "sidecar exposes saturation instead of silently dropping evidence");

  const PipelineIdentity oldIdentity{
      .workId = 1u, .sourceOrdinal = 1u, .seqId = 1u, .generation = 1u};
  const PipelineIdentity newIdentity{
      .workId = 8u, .sourceOrdinal = 8u, .seqId = 257u, .generation = 2u};
  check(sidecar.erase(oldIdentity),
        "terminal identity erases its sidecar entry for reuse");
  check(sidecar.recordIdentity(newIdentity) ==
            PipelineLifecycleSidecarResult::Stored,
        "freed sidecar capacity is reusable");
  PipelineLifecycleSidecar identitySidecar;
  const PipelineIdentity reusedOld{
      .workId = 7u, .sourceOrdinal = 7u, .seqId = 7u, .generation = 1u};
  const PipelineIdentity reusedNew{
      .workId = 8u, .sourceOrdinal = 8u, .seqId = 7u, .generation = 2u};
  const PipelineLifecycleFacts newerFacts{
      .valid = true,
      .selectedParallel = false,
      .effectBoundaryCrossed = false,
      .encodeOwner = PipelineOwner::SerialEncode,
      .childTotal = 2u,
      .joinedChildren = 2u,
  };
  check(identitySidecar.erase(reusedOld) == false,
        "a missing terminal identity cannot erase another generation");
  check(identitySidecar.recordFacts(7u, 1u, facts) ==
            PipelineLifecycleSidecarResult::Stored &&
            identitySidecar.recordFacts(7u, 2u, newerFacts) ==
                PipelineLifecycleSidecarResult::StaleGeneration,
        "same seqId rejects a concurrent facts generation");
  check(identitySidecar.recordIdentity(reusedOld) ==
            PipelineLifecycleSidecarResult::Stored &&
            identitySidecar.recordIdentity(reusedNew) ==
                PipelineLifecycleSidecarResult::StaleGeneration,
        "same seqId rejects a concurrent identity generation");
  PipelineIdentity lookedUp{};
  PipelineLifecycleFacts lookedUpFacts{};
  check(identitySidecar.lookup(7u, 1u, lookedUp, lookedUpFacts) &&
            lookedUp == reusedOld && lookedUpFacts == facts,
        "rejected generation leaves exact old identity and facts intact");
  check(!identitySidecar.lookup(7u, 2u, lookedUp, lookedUpFacts),
        "rejected generation does not allocate a second sidecar entry");
  check(identitySidecar.lookup(7u, 0u, lookedUp, lookedUpFacts) &&
            lookedUp == reusedOld && lookedUpFacts == facts,
        "unqualified lookup remains unique with one live generation");
  check(identitySidecar.erase(reusedOld),
        "exact terminal identity erases its sidecar entry for reuse");
  check(identitySidecar.recordFacts(7u, 2u, newerFacts) ==
            PipelineLifecycleSidecarResult::Stored &&
            identitySidecar.recordIdentity(reusedNew) ==
                PipelineLifecycleSidecarResult::Stored,
        "exact erase permits same seqId generation reuse");
  check(identitySidecar.lookup(7u, 2u, lookedUp, lookedUpFacts) &&
            lookedUp == reusedNew && lookedUpFacts == newerFacts,
        "reused generation publishes only its new identity and facts");
  identitySidecar.clear();
  check(!identitySidecar.lookup(7u, 2u, lookedUp, lookedUpFacts) &&
            identitySidecar.clearCount() == 1u,
        "reset/device-loss clear fences stale sidecar facts");
  const PipelineIdentity clearedReuse{
      .workId = 9u, .sourceOrdinal = 9u, .seqId = 7u, .generation = 3u};
  check(identitySidecar.recordIdentity(clearedReuse) ==
            PipelineLifecycleSidecarResult::Stored &&
            identitySidecar.recordFacts(7u, 3u, facts) ==
                PipelineLifecycleSidecarResult::Stored,
        "reset/device-loss clear permits same seqId generation reuse");

  PipelineLifecycleObserver observer(true);
  const auto observerSink = observer.productionSink();
  observerSink.recordFactsFn(observerSink.context, 41u, 3u, facts);
  for (std::uint64_t seq = 42u; seq <= 297u; ++seq) {
    observerSink.recordFactsFn(observerSink.context, seq, 3u, facts);
  }
  check(observer.error() == PipelineObservationError::BoundedObserverOverflow,
        "production sink surfaces sidecar overflow as observer evidence");

  PipelineLifecycleObserver resetObserver(true);
  const auto resetSink = resetObserver.productionSink();
  resetSink.recordFactsFn(resetSink.context, 9u, 1u, facts);
  emitPipelineControlObservation(
      resetObserver.productionControlSink(),
      PipelineControlObservation{.control = PipelineControl::Reset,
                                 .disposition = PipelineDisposition::Reset,
                                 .epoch = 1u});
  resetSink.recordFactsFn(resetSink.context, 9u, 2u, facts);
  check(resetObserver.error() == PipelineObservationError::None,
        "reset clears the old sidecar generation before reuse");
}

void failureAndShutdownRows() {
  {
    FakePipelineQueue queue;
    auto source = queue.makeSource(1, PipelinePayloadKind::Arena);
    queue.arrive(source);
    queue.adoptRaw(source);
    check(queue.beginReplay(source), "rollback fixture reserves");
    queue.rollbackReplay(source);
    check(queue.observer().valid(),
          "pre-effect rollback returns the exact Raw owner");
    check(queue.snapshot().occupancy == 0,
          "pre-effect rollback restores queue credit");
  }

  {
    FakePipelineQueue queue;
    auto stateOnly = queue.makeSource(1, PipelinePayloadKind::StateOnly);
    queue.arrive(stateOnly);
    queue.adoptRaw(stateOnly);
    queue.finishStateOnly(stateOnly);
    check(queue.observer().valid(),
          "state-only source uses an explicit no-GPU terminal row");
    check(queue.snapshot().completedSeq == 0,
          "state-only terminal does not fabricate GPU completion");
  }

  {
    FakePipelineQueue queue;
    auto source = queue.makeSource(1, PipelinePayloadKind::Arena);
    queue.arrive(source);
    queue.adoptRaw(source);
    check(queue.beginReplay(source), "fail-stop fixture reserves");
    queue.buildPart(source, 2);
    queue.buildPart(source, 2);
    queue.publishFinal(source);
    queue.beginEncoding(source, 1);
    queue.joinChild(source);
    queue.failStop(source);
    check(queue.observer().valid() && queue.snapshot().failed,
          "post-effect failure fail-stops without a GPU milestone");
    check(queue.snapshot().completedSeq == 0,
          "fail-stop does not fabricate completion authority");
  }

  {
    FakePipelineQueue queue;
    auto source = queue.makeSource(1, PipelinePayloadKind::Arena);
    queue.arrive(source);
    queue.adoptRaw(source);
    buildAndSubmit(queue, source, 1);
    queue.complete(source, PipelineDisposition::DeviceLost);
    check(queue.observer().valid() && queue.snapshot().failed,
          "device loss settles exact completion authority");
    check(queue.snapshot().completedSeq == 1,
          "device-loss settlement advances the ordered completion sequence");
  }

  {
    FakePipelineQueue queue;
    auto owner = queue.makeSource(1, PipelinePayloadKind::Arena);
    auto waiter = queue.makeSource(2, PipelinePayloadKind::Arena);
    queue.arrive(owner);
    queue.adoptRaw(owner);
    check(queue.beginReplay(owner), "shutdown fixture occupies capacity");
    queue.arrive(waiter);
    queue.adoptRaw(waiter);
    queue.parkAdmission(waiter);
    queue.shutdownWaitingSource(waiter);
    check(queue.observer().valid(),
          "shutdown emits a terminal wake and explicit disposition");
    check(queue.snapshot().stopped &&
              queue.snapshot().admissionWakeGeneration == 1,
          "shutdown wakes the parked admission predicate");
  }

  {
    FakePipelineQueue queue;
    auto source = queue.makeSource(1, PipelinePayloadKind::Arena);
    queue.arrive(source);
    queue.arrive(source);
    check(queue.observer().error() ==
              PipelineObservationError::DuplicateOrRegressedStage,
          "duplicate lifecycle emission is rejected");
  }
}

}  // namespace

int main() {
  try {
    purePredicateTruthTables();
    selectedParallelRecordedChain();
    completionFrontierTruthTable();
    lifecycleArithmeticTruthTable();
    physicalBatchMemberTruthTable();
    endToEndProducerIdentityTruthTable();
    physicalBatchOwnerChainRetainsReclaimClosure();
    physicalBatchOwnerFinalizeRejectsOmittedTail();
    explicitCompletionAndFinishFrontiers();
    mixedDrawPresentSourceCompletes();
    presentOnlyAdmissionWedgeCompletes();
    composedThreeAxisWakeIsDeterministic();
    deliberateNativeCounterexamples();
    ownerEvidenceOverflowFailsClosed();
    lifecycleSidecarGenerationAndSaturation();
    failureAndShutdownRows();
  } catch (const std::exception& e) {
    std::cerr << "pipeline_lifecycle_observer_spec failed: " << e.what()
              << '\n';
    return 1;
  } catch (...) {
    std::cerr << "pipeline_lifecycle_observer_spec unexpected exception\n";
    return 1;
  }
  std::cout << "pipeline_lifecycle_observer_spec passed\n";
  return 0;
}
