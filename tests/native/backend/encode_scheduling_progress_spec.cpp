#include "../../../src/dxmt9/render/encode_scheduling_progress.hpp"
#include "../../../src/dxmt9/dxmt9_scheduling_progress_watchdog.hpp"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

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
        .controlSlotFree = (bits & 16u) != 0,
        .reserveStillPressured = (bits & 32u) != 0,
    };
    const auto expected = gate.stopped || gate.poisoned
        ? CpuReadyAdmissionAction::Stop
        : !gate.arenaBuildActive && !gate.arenaBuildContextPresent &&
                gate.controlSlotFree && !gate.reserveStillPressured
            ? CpuReadyAdmissionAction::RetryAdmission
            : CpuReadyAdmissionAction::Wait;
    check(classifyCpuReadyAdmissionGate(gate) == expected,
          "CPU-ready admission truth table drifted");
  }
}

void capacityGenerationTruthTable() {
  using dxmt9::render::firstLeaseCapacityWaitDone;
  for (bool stopped : {false, true}) {
    for (std::uint64_t observed : {0ull, 1ull, 7ull}) {
      for (std::uint64_t current : {0ull, 1ull, 7ull}) {
        check(firstLeaseCapacityWaitDone(stopped, observed, current) ==
                  (stopped || observed != current),
              "first-lease generation truth table drifted");
      }
    }
  }
}

void sessionWakeTruthTables() {
  using namespace dxmt9::render;
  for (std::uint32_t bits = 0; bits < 256u; ++bits) {
    const CpuReadySessionWakeState state{
        .stopped = (bits & 1u) != 0,
        .ready = (bits & 2u) != 0,
        .orderedRelease = (bits & 4u) != 0,
        .producerSequenceWait = (bits & 8u) != 0,
        .admissionPressure = (bits & 16u) != 0,
        .writerPressure = (bits & 32u) != 0,
        .initializerPending = (bits & 64u) != 0,
        .capacityProgress = (bits & 128u) != 0,
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

template <typename Predicate, typename Release>
void conditionVariableReleases(Predicate predicate, Release release,
                               std::string_view message) {
  std::mutex mutex;
  std::condition_variable cv;
  std::condition_variable readyCv;
  bool waiterReady = false;
  bool released = false;
  std::thread waiter([&] {
    std::unique_lock lock(mutex);
    waiterReady = true;
    readyCv.notify_one();
    cv.wait(lock, predicate);
    released = true;
  });
  {
    std::unique_lock lock(mutex);
    readyCv.wait(lock, [&] { return waiterReady; });
    check(!predicate(), "fixture must begin with a closed wait predicate");
    release();
  }
  cv.notify_all();
  waiter.join();
  check(released, message);
}

void realConditionVariableReleaseTests() {
  using namespace dxmt9::render;
  {
    CpuReadyAdmissionGate gate{.controlSlotFree = true};
    conditionVariableReleases(
        [&] {
          return classifyCpuReadyAdmissionGate(gate) !=
              CpuReadyAdmissionAction::Wait;
        },
        [&] { gate.reserveStillPressured = false; },
        "admission reserve release wakes a real condition variable");
  }
  {
    bool stopped = false;
    std::uint64_t current = 3;
    conditionVariableReleases(
        [&] { return firstLeaseCapacityWaitDone(stopped, 3, current); },
        [&] { current = 4; },
        "capacity generation release wakes a real condition variable");
  }
  {
    CpuReadySessionWakeState state{};
    conditionVariableReleases(
        [&] { return retainedOrDeferredSessionWaitDone(state); },
        [&] { state.initializerPending = true; },
        "initializer publication wakes a retained session wait");
  }
  {
    CpuReadySessionWakeState state{};
    conditionVariableReleases(
        [&] { return openSessionWaitDone(state); },
        [&] { state.orderedRelease = true; },
        "ordered release wakes an open session wait");
  }
  constexpr std::array terminalTargets{
      SchedulingWakeWriter,
      SchedulingWakeEncoder,
      SchedulingWakeFinish,
      SchedulingWakePresentCompleted,
      SchedulingWakePresentDequeued,
      SchedulingWakeSessionRelease,
      SchedulingWakePendingCompletion,
  };
  for (const auto disposition : {SchedulingTerminalDisposition::Stop,
                                 SchedulingTerminalDisposition::DeviceLoss}) {
    for (const auto target : terminalTargets) {
      auto current = SchedulingTerminalDisposition::Running;
      conditionVariableReleases(
          [&] { return planSchedulingTerminalWake(current).wakes(target); },
          [&] { current = disposition; },
          "terminal disposition releases every real condition variable");
    }
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

}  // namespace

int main() {
  try {
    admissionTruthTable();
    capacityGenerationTruthTable();
    sessionWakeTruthTables();
    initializerTransitionTruthTable();
    terminalFanoutTruthTable();
    realConditionVariableReleaseTests();
    watchdogPerfOffDoesZeroWork();
    watchdogUsesBoundedGenerationSafeSlots();
    std::cout << "encode scheduling progress spec: ok\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "encode scheduling progress spec: " << error.what() << '\n';
    return 1;
  }
}
