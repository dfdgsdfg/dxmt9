#include "../../../src/dxmt9/render/encode_scheduling_progress.hpp"
#include "../../../src/dxmt9/dxmt9_command_queue.hpp"
#include "../../../src/dxmt9/dxmt9_resource_initializer.hpp"
#include "../../../src/dxmt9/dxmt9_scheduling_progress_watchdog.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace dxmt9 {

struct SchedulingProgressTestAccess {
  struct PendingWaitReady {
    std::mutex mutex;
    std::condition_variable cv;
    bool ready = false;
  };

  static std::condition_variable& conditionVariable(
      CommandQueue& queue, render::SchedulingWakeTarget target) {
    switch (target) {
    case render::SchedulingWakeWriter: return queue.writeCv_;
    case render::SchedulingWakeEncoder: return queue.encodeCv_;
    case render::SchedulingWakeFinish: return queue.finishCv_;
    case render::SchedulingWakePresentCompleted:
      return queue.presentCompletedCv_;
    case render::SchedulingWakePresentDequeued:
      return queue.presentDequeuedCv_;
    case render::SchedulingWakeSessionRelease:
      return queue.sessionReleaseCv_;
    default: std::terminate();
    }
  }

  static std::mutex& schedulingMutex(CommandQueue& queue) {
    return queue.mutex_;
  }

  static void enqueueInitializerPending(resources::Initializer& initializer) {
    // This fixture exercises only the pending-upload wake edge. A default
    // StagingCopy is not a valid initializer resource and is now correctly
    // rejected by the lifetime contract before it can enter the queue.
    initializer.queue_->noteInitializerPendingUploads();
  }

  static void requestTerminal(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    queue.requestSchedulingStopLocked();
  }

  static void poison(CommandQueue& queue) {
    queue.queueLifecycle_.poisonTapeFailure();
  }

  static core::metalqueue::QueueLifecycleController::PoisonOriginSnapshot
  poisonWithLocations(CommandQueue& queue,
                      std::uint_least32_t& firstLine,
                      std::uint_least32_t& firstColumn,
                      const char*& firstFile,
                      const char*& firstFunction) {
    const auto first = std::source_location::current();
    firstLine = first.line();
    firstColumn = first.column();
    firstFile = first.file_name();
    firstFunction = first.function_name();
    queue.queueLifecycle_.poisonTapeFailure(first);
    queue.queueLifecycle_.poisonTapeFailure(std::source_location::current());
    return queue.queueLifecycle_.firstPoisonOrigin();
  }

  static void abort(CommandQueue& queue) {
    queue.abortCpuReadyArenaSource({}, queue.slots_.size());
  }

  static bool processPendingCompletion(CommandQueue& queue) {
    return queue.queueLifecycle_.processOnePendingCompletion();
  }

  static void observePendingWait(void* context) noexcept {
    auto& ready = *static_cast<PendingWaitReady*>(context);
    {
      std::lock_guard lock(ready.mutex);
      ready.ready = true;
    }
    ready.cv.notify_one();
  }

  static void setPendingWaitObserver(
      CommandQueue& queue, PendingWaitReady& ready) {
    queue.queueLifecycle_.setPendingCompletionWaitObserverForTest(
        &ready, observePendingWait);
  }

  static void requestPendingCompletionStop(CommandQueue& queue) {
    queue.queueLifecycle_.requestPendingCompletionStop();
  }
};

}  // namespace dxmt9

namespace {

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void check(bool condition, std::string_view message) {
  if (!condition) {
    throw TestFailure(std::string(message));
  }
}

void admissionTruthTable() {
  using namespace dxmt9::render;
  for (std::uint32_t bits = 0; bits < 64u; ++bits) {
    const CpuReadyAdmissionGate gate{
        .stopped = (bits & 1u) != 0,
        .poisoned = (bits & 2u) != 0,
        .arenaBuildActive = (bits & 4u) != 0,
        .arenaBuildContextPresent = (bits & 8u) != 0,
        .controlSlotsFree = (bits & 16u) != 0,
        .reserveStillPressured = (bits & 32u) != 0,
    };
    const auto expected = gate.stopped || gate.poisoned
        ? CpuReadyAdmissionAction::Stop
        : !gate.arenaBuildActive && !gate.arenaBuildContextPresent &&
                gate.controlSlotsFree && !gate.reserveStillPressured
            ? CpuReadyAdmissionAction::RetryAdmission
            : CpuReadyAdmissionAction::Wait;
    check(classifyCpuReadyAdmissionGate(gate) == expected,
          "CPU-ready admission truth table drifted");
  }
}

void firstLeaseCapacityWaitTruthTable() {
  using namespace dxmt9::render;
  for (std::uint32_t bits = 0; bits < 16u; ++bits) {
    const FirstLeaseReadyHeadState head{
        .arena = (bits & 1u) != 0,
        .present = (bits & 2u) != 0,
        .fitsOrdinaryCapacity = (bits & 4u) != 0,
        .fitsHighWater = (bits & 8u) != 0,
    };
    const auto expected = !head.arena
        ? FirstLeaseReadyHeadEligibility::NonArena
        : head.present
            ? FirstLeaseReadyHeadEligibility::Present
            : !head.fitsOrdinaryCapacity
                ? FirstLeaseReadyHeadEligibility::OrdinaryCapacity
                : !head.fitsHighWater
                    ? FirstLeaseReadyHeadEligibility::HighWater
                    : FirstLeaseReadyHeadEligibility::Eligible;
    check(classifyFirstLeaseReadyHeadEligibility(head) == expected,
          "first-lease ready-head eligibility priority drifted");
  }
  for (std::uint32_t bits = 0; bits < 128u; ++bits) {
    const bool readyHeadValid = (bits & 4u) != 0;
    const bool sameConsumedHead = (bits & 8u) != 0;
    const FirstLeaseReadyHeadIdentity readyHead = readyHeadValid
        ? FirstLeaseReadyHeadIdentity{.seqId = 11u, .sourceOrdinal = 13u}
        : FirstLeaseReadyHeadIdentity{};
    const FirstLeaseCapacityWaitState state{
        .stopped = (bits & 1u) != 0,
        .admissionPressure = (bits & 2u) != 0,
        .producerSequenceWaitTargetSeqId =
            (bits & 64u) != 0 ? 11u : 0u,
        .readyHeadOwnsOrdinaryDirectCapacity = (bits & 16u) != 0,
        .readyHead = readyHead,
        .lastSerialProgressHead = sameConsumedHead
            ? readyHead
            : FirstLeaseReadyHeadIdentity{
                  .seqId = 10u, .sourceOrdinal = 12u},
        .observedGeneration = 7u,
        .currentGeneration = (bits & 32u) != 0 ? 8u : 7u,
    };
    const auto expected = state.stopped
        ? FirstLeaseCapacityWaitAction::Stop
        : state.currentGeneration != state.observedGeneration
            ? FirstLeaseCapacityWaitAction::RetryLease
            : state.admissionPressure &&
                    state.readyHeadOwnsOrdinaryDirectCapacity &&
                    state.readyHead.valid() &&
                    state.readyHead != state.lastSerialProgressHead
                ? FirstLeaseCapacityWaitAction::
                      ExecuteOneSourceSerialForAdmissionPressure
                : state.producerSequenceWaitTargetSeqId >=
                              state.readyHead.seqId &&
                          state.readyHeadOwnsOrdinaryDirectCapacity &&
                          state.readyHead.valid() &&
                          state.readyHead != state.lastSerialProgressHead
                    ? FirstLeaseCapacityWaitAction::
                          ExecuteOneSourceSerialForProducerSequenceWait
                    : FirstLeaseCapacityWaitAction::Wait;
    check(classifyFirstLeaseCapacityWait(state) == expected,
          "first-lease capacity-wait action truth table drifted");
  }

  FirstLeaseCapacityWaitState identity{
      .admissionPressure = true,
      .readyHeadOwnsOrdinaryDirectCapacity = true,
      .readyHead = {.seqId = 11u, .sourceOrdinal = 13u},
      .observedGeneration = 7u,
      .currentGeneration = 7u,
  };
  check(classifyFirstLeaseCapacityWait(identity) ==
            FirstLeaseCapacityWaitAction::
                ExecuteOneSourceSerialForAdmissionPressure,
        "one eligible denied head consumes its exact serial token");
  identity.lastSerialProgressHead = identity.readyHead;
  check(classifyFirstLeaseCapacityWait(identity) ==
            FirstLeaseCapacityWaitAction::Wait,
        "one denied Ready identity cannot consume a second serial token");
  identity.readyHead = {.seqId = 12u, .sourceOrdinal = 14u};
  check(classifyFirstLeaseCapacityWait(identity) ==
            FirstLeaseCapacityWaitAction::
                ExecuteOneSourceSerialForAdmissionPressure,
        "FIFO head advance rearms one exact serial token in the same "
        "capacity generation");
  identity.currentGeneration = 8u;
  check(classifyFirstLeaseCapacityWait(identity) ==
            FirstLeaseCapacityWaitAction::RetryLease,
        "an explicit generation transition retries before serial progress");
  identity.observedGeneration = 8u;
  check(classifyFirstLeaseCapacityWait(identity) ==
            FirstLeaseCapacityWaitAction::
                ExecuteOneSourceSerialForAdmissionPressure,
        "generation retry does not consume the changed head's serial token");

  identity.admissionPressure = false;
  identity.producerSequenceWaitTargetSeqId = 12u;
  check(classifyFirstLeaseCapacityWait(identity) ==
            FirstLeaseCapacityWaitAction::
                ExecuteOneSourceSerialForProducerSequenceWait,
        "an ordered producer fence covers one fresh eligible FIFO head");
  identity.producerSequenceWaitTargetSeqId = 11u;
  check(classifyFirstLeaseCapacityWait(identity) ==
            FirstLeaseCapacityWaitAction::Wait,
        "a producer fence fails closed beyond its exact ordered target");

  constexpr std::array ineligibleStates{
      FirstLeaseReadyHeadState{.arena = false,
                               .present = false,
                               .fitsOrdinaryCapacity = true,
                               .fitsHighWater = true},
      FirstLeaseReadyHeadState{.arena = true,
                               .present = true,
                               .fitsOrdinaryCapacity = true,
                               .fitsHighWater = true},
      FirstLeaseReadyHeadState{.arena = true,
                               .present = false,
                               .fitsOrdinaryCapacity = false,
                               .fitsHighWater = true},
      FirstLeaseReadyHeadState{.arena = true,
                               .present = false,
                               .fitsOrdinaryCapacity = true,
                               .fitsHighWater = false},
  };
  constexpr std::array expectedIneligible{
      FirstLeaseReadyHeadEligibility::NonArena,
      FirstLeaseReadyHeadEligibility::Present,
      FirstLeaseReadyHeadEligibility::OrdinaryCapacity,
      FirstLeaseReadyHeadEligibility::HighWater,
  };
  identity.producerSequenceWaitTargetSeqId = identity.readyHead.seqId;
  for (std::size_t i = 0; i < ineligibleStates.size(); ++i) {
    const auto sibling =
        classifyFirstLeaseReadyHeadEligibility(ineligibleStates[i]);
    check(sibling == expectedIneligible[i],
          "the exact ineligible sibling must retain its disposition");
    identity.readyHeadOwnsOrdinaryDirectCapacity =
        sibling == FirstLeaseReadyHeadEligibility::Eligible;
    check(classifyFirstLeaseCapacityWait(identity) ==
              FirstLeaseCapacityWaitAction::Wait,
          "each ineligible sibling preserves the exact-head token");
  }
}

void sessionWakeTruthTables() {
  using namespace dxmt9::render;
  for (std::uint32_t bits = 0; bits < 512u; ++bits) {
    const CpuReadySessionWakeState state{
        .stopped = (bits & 1u) != 0,
        .ready = (bits & 2u) != 0,
        .orderedRelease = (bits & 4u) != 0,
        .producerSequenceWait = (bits & 8u) != 0,
        .admissionPressure = (bits & 16u) != 0,
        .writerPressure = (bits & 32u) != 0,
        .initializerPending = (bits & 64u) != 0,
        .capacityProgress = (bits & 128u) != 0,
        .sourceIntentProgress = (bits & 256u) != 0,
    };
    const bool openExpected = (bits & 0x0fu) != 0;
    const bool retainedExpected = bits != 0;
    check(openSessionWaitDone(state) == openExpected,
          "open-session wake truth table drifted");
    check(retainedOrDeferredSessionWaitDone(state) == retainedExpected,
          "retained/deferred-session wake truth table drifted");
  }
}

void initializerTransitionTruthTable() {
  using dxmt9::render::initializerPendingTransitionNeedsWake;
  check(!initializerPendingTransitionNeedsWake(false, false),
        "nonempty-to-nonempty does not wake");
  check(!initializerPendingTransitionNeedsWake(false, true),
        "nonempty-to-empty does not wake");
  check(initializerPendingTransitionNeedsWake(true, false),
        "empty-to-nonempty wakes the encode/session predicate");
  check(!initializerPendingTransitionNeedsWake(true, true),
        "empty-to-empty does not wake");
}

void terminalFanoutTruthTable() {
  using namespace dxmt9::render;
  constexpr std::array allTargets{
      SchedulingWakeWriter,
      SchedulingWakeEncoder,
      SchedulingWakeFinish,
      SchedulingWakePresentCompleted,
      SchedulingWakePresentDequeued,
      SchedulingWakeSessionRelease,
      SchedulingWakePendingCompletion,
  };
  const auto running = planSchedulingTerminalWake(
      SchedulingTerminalDisposition::Running);
  const auto stop = planSchedulingTerminalWake(
      SchedulingTerminalDisposition::Stop);
  const auto loss = planSchedulingTerminalWake(
      SchedulingTerminalDisposition::DeviceLoss);
  for (const auto target : allTargets) {
    check(!running.wakes(target), "Running must not fan out terminal wakes");
    check(stop.wakes(target), "Stop must fan out to every waiter owner");
    check(loss.wakes(target), "DeviceLoss must fan out to every waiter owner");
  }
}

template <typename Notify>
void productionConditionVariableReleases(
    dxmt9::CommandQueue& queue,
    dxmt9::render::SchedulingWakeTarget target,
    Notify notify, std::string_view message) {
  std::mutex readyMutex;
  std::condition_variable readyCv;
  bool waiterReady = false;
  std::atomic<bool> predicate{false};
  std::atomic<bool> released{false};
  std::thread waiter([&] {
    std::unique_lock lock(
        dxmt9::SchedulingProgressTestAccess::schedulingMutex(queue));
    {
      std::lock_guard readyLock(readyMutex);
      waiterReady = true;
    }
    readyCv.notify_one();
    auto& cv = dxmt9::SchedulingProgressTestAccess::conditionVariable(
        queue, target);
    released.store(cv.wait_for(lock, std::chrono::seconds(2), [&] {
      return predicate.load(std::memory_order_acquire);
    }), std::memory_order_release);
  });
  {
    std::unique_lock lock(readyMutex);
    readyCv.wait(lock, [&] { return waiterReady; });
  }
  predicate.store(true, std::memory_order_release);
  notify();
  waiter.join();
  check(released.load(std::memory_order_acquire), message);
}

dxmt9::CommandQueue makeSchedulingQueue() {
  return dxmt9::CommandQueue(
      dxmt9::CommandQueue::ArenaLeaseTestQueueTag{},
      dxmt9::core::BackendLimits{});
}

void productionOwnerConditionVariableReleaseTests() {
  using namespace dxmt9::render;
  {
    auto queue = makeSchedulingQueue();
    dxmt9::resources::Pool pool;
    dxmt9::resources::Initializer initializer(queue, pool, {});
    productionConditionVariableReleases(
        queue, SchedulingWakeEncoder,
        [&] {
          dxmt9::SchedulingProgressTestAccess::enqueueInitializerPending(
              initializer);
        },
        "Initializer empty-to-nonempty hook wakes the production encode CV");
  }
  constexpr std::array nonCompletionTargets{
      SchedulingWakeWriter,
      SchedulingWakeEncoder,
      SchedulingWakeFinish,
      SchedulingWakePresentCompleted,
      SchedulingWakePresentDequeued,
      SchedulingWakeSessionRelease,
  };
  for (const auto target : nonCompletionTargets) {
    auto queue = makeSchedulingQueue();
    productionConditionVariableReleases(
        queue, target,
        [&] { dxmt9::SchedulingProgressTestAccess::requestTerminal(queue); },
        "CommandQueue terminal fanout wakes the selected production CV");
  }
  for (const auto target : nonCompletionTargets) {
    auto queue = makeSchedulingQueue();
    productionConditionVariableReleases(
        queue, target,
        [&] { dxmt9::SchedulingProgressTestAccess::poison(queue); },
        "QueueLifecycleController poison wakes the selected production CV");
  }
  for (const auto target : nonCompletionTargets) {
    auto queue = makeSchedulingQueue();
    productionConditionVariableReleases(
        queue, target,
        [&] { dxmt9::SchedulingProgressTestAccess::abort(queue); },
        "arena abort wakes the selected production CV");
  }
  for (const auto action : {0, 1, 2, 3}) {
    auto queue = makeSchedulingQueue();
    dxmt9::SchedulingProgressTestAccess::PendingWaitReady waitReady;
    dxmt9::SchedulingProgressTestAccess::setPendingWaitObserver(
        queue, waitReady);
    std::atomic<bool> returned{false};
    std::atomic<bool> processed{true};
    std::thread waiter([&] {
      processed.store(
          dxmt9::SchedulingProgressTestAccess::processPendingCompletion(queue),
          std::memory_order_release);
      returned.store(true, std::memory_order_release);
    });
    {
      std::unique_lock lock(waitReady.mutex);
      waitReady.cv.wait(lock, [&] { return waitReady.ready; });
    }
    switch (action) {
    case 0:
      dxmt9::SchedulingProgressTestAccess::requestTerminal(queue);
      break;
    case 1:
      dxmt9::SchedulingProgressTestAccess::poison(queue);
      break;
    case 2:
      dxmt9::SchedulingProgressTestAccess::abort(queue);
      break;
    default:
      dxmt9::SchedulingProgressTestAccess::requestPendingCompletionStop(queue);
      break;
    }
    waiter.join();
    check(returned.load(std::memory_order_acquire) &&
              !processed.load(std::memory_order_acquire),
          "production pending-completion stop predicate releases its waiter");
  }
}

void watchdogPerfOffDoesZeroWork() {
  dxmt9::SchedulingProgressWatchdog watchdog(
      /*enabled=*/false, /*thresholdMs=*/1, /*startSamplerThread=*/false);
  std::uint64_t clockReads = 0;
  watchdog.noteAcceptedWithClockForTest(1, true, [&] {
    ++clockReads;
    return 1ull;
  });
  check(clockReads == 0,
        "perf-off watchdog gates before its first clock read");
  check(watchdog.trackingOverflowCount() == 0,
        "perf-off watchdog performs no atomic tracking work");
}

void watchdogUsesBoundedGenerationSafeSlots() {
  {
    dxmt9::SchedulingProgressWatchdog watchdog(
        /*enabled=*/true, /*thresholdMs=*/1000,
        /*startSamplerThread=*/false);
    std::uint64_t now = 1;
    watchdog.noteAcceptedWithClockForTest(1, false, [&] { return now++; });
    watchdog.noteAcceptedWithClockForTest(
        1 + dxmt9::SchedulingProgressWatchdog::kCapacity, false,
        [&] { return now++; });
    check(watchdog.trackingOverflowCount() == 1,
          "a live generation collision reports bounded tracking overflow");
  }
  {
    dxmt9::SchedulingProgressWatchdog watchdog(
        /*enabled=*/true, /*thresholdMs=*/1000,
        /*startSamplerThread=*/false);
    watchdog.noteAccepted(1, true);
    watchdog.noteReleased(1, false);
    watchdog.noteAccepted(
        1 + dxmt9::SchedulingProgressWatchdog::kCapacity, false);
    check(watchdog.trackingOverflowCount() == 1,
          "an unsettled Present keeps its generation-stamped slot live");
  }
  {
    dxmt9::SchedulingProgressWatchdog watchdog(
        /*enabled=*/true, /*thresholdMs=*/1000,
        /*startSamplerThread=*/false);
    watchdog.noteAccepted(1, false);
    watchdog.noteReleased(1, false);
    watchdog.noteAccepted(
        1 + dxmt9::SchedulingProgressWatchdog::kCapacity, false);
    check(watchdog.trackingOverflowCount() == 0,
          "a fully released non-Present slot is generation-reusable");
  }
}

void watchdogRejectsStaleStoresAcrossCapacityReuse() {
  dxmt9::SchedulingProgressWatchdog watchdog(
      /*enabled=*/true, /*thresholdMs=*/1000,
      /*startSamplerThread=*/false);
  constexpr std::uint64_t iterations = 20000;
  std::atomic<std::uint64_t> staleSeq{0};
  std::atomic<std::uint64_t> currentSeq{0};
  std::atomic<std::uint64_t> epoch{0};
  std::atomic<std::uint64_t> writerDoneEpoch{0};
  std::atomic<bool> stop{false};
  std::thread staleWriter([&] {
    std::uint64_t observedEpoch = 0;
    while (!stop.load(std::memory_order_acquire)) {
      const auto nextEpoch = epoch.load(std::memory_order_acquire);
      if (nextEpoch == observedEpoch) {
        std::this_thread::yield();
        continue;
      }
      observedEpoch = nextEpoch;
      watchdog.noteSubmitted(staleSeq.load(std::memory_order_relaxed), true);
      writerDoneEpoch.store(nextEpoch, std::memory_order_release);
    }
  });
  for (std::uint64_t i = 0; i < iterations; ++i) {
    const std::uint64_t oldSeq = 1 + i * 2u *
        dxmt9::SchedulingProgressWatchdog::kCapacity;
    const std::uint64_t newSeq = oldSeq +
        dxmt9::SchedulingProgressWatchdog::kCapacity;
    watchdog.noteAccepted(oldSeq, false);
    watchdog.noteReleased(oldSeq, false);
    staleSeq.store(oldSeq, std::memory_order_relaxed);
    currentSeq.store(newSeq, std::memory_order_relaxed);
    watchdog.noteAccepted(newSeq, false);
    epoch.store(i + 1, std::memory_order_release);
    while (writerDoneEpoch.load(std::memory_order_acquire) != i + 1) {
      std::this_thread::yield();
    }
    const auto snapshot = watchdog.slotSnapshotForTest(newSeq);
    check(snapshot.tracked && snapshot.identity == newSeq,
          "reused watchdog slot keeps the new generation identity");
    check(snapshot.phase == dxmt9::SchedulingProgressPhase::Admission &&
              (snapshot.flags & dxmt9::SchedulingProgressCapture) == 0 &&
              (snapshot.flags & dxmt9::SchedulingProgressReleased) == 0,
          "old-generation writer cannot store into the reused generation");
    watchdog.noteReleased(newSeq, false);
  }
  stop.store(true, std::memory_order_release);
  staleWriter.join();
  check(currentSeq.load(std::memory_order_relaxed) != 0,
        "watchdog reuse stress exercised a current generation");
}

void watchdogConservesSegmentSerialProgressPerSource() {
  dxmt9::SchedulingProgressWatchdog watchdog(
      /*enabled=*/true, /*thresholdMs=*/1000,
      /*startSamplerThread=*/false);
  constexpr std::array seqIds{41ull, 42ull, 43ull};
  for (const auto seqId : seqIds) {
    watchdog.noteAccepted(seqId, false);
  }
  watchdog.notePublished(seqIds[0], false);
  watchdog.notePublished(seqIds[1], false);
  watchdog.notePublished(seqIds[2], true);

  for (std::size_t i = 0; i < seqIds.size(); ++i) {
    const auto snapshot = watchdog.slotSnapshotForTest(seqIds[i]);
    check(snapshot.tracked && snapshot.identity == seqIds[i] &&
              snapshot.phase == dxmt9::SchedulingProgressPhase::Published &&
              (snapshot.flags & dxmt9::SchedulingProgressAccepted) != 0,
          "SegmentSerial publishes progress for every contiguous source");
    const bool hasPresent =
        (snapshot.flags & dxmt9::SchedulingProgressHasPresent) != 0;
    check(hasPresent == (i + 1u == seqIds.size()),
          "SegmentSerial reserves Present progress for the final source");
  }
}

void poisonOriginPublishesFirstCallsiteOnce() {
  auto queue = makeSchedulingQueue();
  std::uint_least32_t firstLine = 0;
  std::uint_least32_t firstColumn = 0;
  const char* firstFile = nullptr;
  const char* firstFunction = nullptr;
  const auto origin = dxmt9::SchedulingProgressTestAccess::poisonWithLocations(
      queue, firstLine, firstColumn, firstFile, firstFunction);
  check(origin.valid() && origin.file == firstFile &&
            origin.function == firstFunction && origin.line == firstLine &&
            origin.column == firstColumn,
        "QueueLifecycleController retains the first typed poison origin");
}

}  // namespace

int main() {
  try {
    admissionTruthTable();
    firstLeaseCapacityWaitTruthTable();
    sessionWakeTruthTables();
    initializerTransitionTruthTable();
    terminalFanoutTruthTable();
    productionOwnerConditionVariableReleaseTests();
    watchdogPerfOffDoesZeroWork();
    watchdogUsesBoundedGenerationSafeSlots();
    watchdogRejectsStaleStoresAcrossCapacityReuse();
    watchdogConservesSegmentSerialProgressPerSource();
    poisonOriginPublishesFirstCallsiteOnce();
    std::cout << "encode scheduling progress spec: ok\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "encode scheduling progress spec: " << error.what() << '\n';
    return 1;
  }
}
