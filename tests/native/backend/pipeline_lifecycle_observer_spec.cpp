#include "../../../src/dxmt9/dxmt9_pipeline_lifecycle.hpp"

#include <condition_variable>
#include <cstdint>
#include <exception>
#include <iostream>
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
  PipelineStage stage = PipelineStage::SourceArrival;
  std::uint64_t ownedBytes = 0;
  std::uint64_t observedWakeGeneration = 0;
  std::uint32_t borrows = 0;
  std::uint32_t constructed = 0;
  std::uint32_t required = 0;
  std::uint32_t joined = 0;
  std::uint32_t children = 0;
  bool completionAuthority = false;
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
         PipelineDisposition::StateOnly, before);
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
         PipelineDisposition::Shutdown, before);
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
    if (source.payloadKind == PipelinePayloadKind::PresentOnly) {
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
    emit(source, source.stage, PipelineStage::Reclaimed, disposition,
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
        .ownedBytes = source.ownedBytes,
        .outstandingBorrows = source.borrows,
        .constructedCount = source.constructed,
        .requiredCount = source.required,
        .joinedChildren = source.joined,
        .totalChildren = source.children,
        .completionAuthority = source.completionAuthority,
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

void purePredicateTruthTables() {
  check(pipelinePublicationMayCommit(2, 2, 0),
        "complete assembler prefix publishes after borrow return");
  check(!pipelinePublicationMayCommit(1, 2, 0),
        "partial assembler prefix cannot publish");
  check(!pipelinePublicationMayCommit(2, 2, 1),
        "publication cannot escape its replay borrow");

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
  check(!pipelineOwnerMayReclaim(
            PipelineStage::GPUInFlight, PipelineDisposition::Completed, 1,
            true),
        "outstanding borrow blocks reclaim");

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
  event.from = PipelineStage::SourceArrival;
  event.to = PipelineStage::ProducerOwned;
  event.disposition = PipelineDisposition::Advance;
  for (std::size_t i = 0; i < kMaxObservedPipelineEvents; ++i) {
    event.identity = PipelineIdentity{
        .workId = i + 1u, .sourceOrdinal = i + 1u, .seqId = i + 1u,
        .generation = i + 1u};
    emitPipelineLifecycleEvent(sink, event);
  }
  check(observer.valid() &&
            observer.state().ownerEventCount == kMaxObservedPipelineEvents,
        "owner evidence accepts exactly its fixed event capacity");
  event.identity = PipelineIdentity{
      .workId = kMaxObservedPipelineEvents + 1u,
      .sourceOrdinal = kMaxObservedPipelineEvents + 1u,
      .seqId = kMaxObservedPipelineEvents + 1u,
      .generation = kMaxObservedPipelineEvents + 1u};
  emitPipelineLifecycleEvent(sink, event);
  check(observer.error() == PipelineObservationError::BoundedObserverOverflow &&
            observer.state().ownerEventCount == kMaxObservedPipelineEvents,
        "owner evidence overflow is a stable bounded disposition");
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
    presentOnlyAdmissionWedgeCompletes();
    composedThreeAxisWakeIsDeterministic();
    deliberateNativeCounterexamples();
    ownerEvidenceOverflowFailsClosed();
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
