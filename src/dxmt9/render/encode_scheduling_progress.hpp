#pragma once

#include <cstdint>

namespace dxmt9::render {

// Pure wait/wake projections for R-BACK-2.67. Production condition-variable
// predicates delegate to these values so the native truth tables and the TLA+
// interface transitions cannot silently drift apart.

struct CpuReadyAdmissionGate {
  bool stopped = false;
  bool poisoned = false;
  bool arenaBuildActive = false;
  bool arenaBuildContextPresent = false;
  bool controlSlotFree = false;
  bool reserveStillPressured = true;
};

enum class CpuReadyAdmissionAction : std::uint8_t {
  Wait,
  RetryAdmission,
  Stop,
};

constexpr CpuReadyAdmissionAction classifyCpuReadyAdmissionGate(
    CpuReadyAdmissionGate gate) noexcept {
  if (gate.stopped || gate.poisoned) {
    return CpuReadyAdmissionAction::Stop;
  }
  if (!gate.arenaBuildActive && !gate.arenaBuildContextPresent &&
      gate.controlSlotFree && !gate.reserveStillPressured) {
    return CpuReadyAdmissionAction::RetryAdmission;
  }
  return CpuReadyAdmissionAction::Wait;
}

constexpr bool firstLeaseCapacityWaitDone(
    bool stopped, std::uint64_t observedGeneration,
    std::uint64_t currentGeneration) noexcept {
  return stopped || currentGeneration != observedGeneration;
}

struct CpuReadySessionWakeState {
  bool stopped = false;
  bool ready = false;
  bool orderedRelease = false;
  bool producerSequenceWait = false;
  bool admissionPressure = false;
  bool writerPressure = false;
  bool initializerPending = false;
  bool capacityProgress = false;
};

constexpr bool openSessionWaitDone(CpuReadySessionWakeState state) noexcept {
  return state.stopped || state.ready || state.orderedRelease ||
      state.producerSequenceWait;
}

constexpr bool retainedOrDeferredSessionWaitDone(
    CpuReadySessionWakeState state) noexcept {
  return openSessionWaitDone(state) || state.admissionPressure ||
      state.writerPressure || state.initializerPending ||
      state.capacityProgress;
}

constexpr bool initializerPendingTransitionNeedsWake(
    bool wasEmpty, bool isEmpty) noexcept {
  return wasEmpty && !isEmpty;
}

enum class SchedulingTerminalDisposition : std::uint8_t {
  Running,
  Stop,
  DeviceLoss,
};

enum SchedulingWakeTarget : std::uint32_t {
  SchedulingWakeNone = 0,
  SchedulingWakeWriter = 1u << 0,
  SchedulingWakeEncoder = 1u << 1,
  SchedulingWakeFinish = 1u << 2,
  SchedulingWakePresentCompleted = 1u << 3,
  SchedulingWakePresentDequeued = 1u << 4,
  SchedulingWakeSessionRelease = 1u << 5,
  SchedulingWakePendingCompletion = 1u << 6,
};

struct SchedulingTerminalWakePlan {
  SchedulingTerminalDisposition disposition =
      SchedulingTerminalDisposition::Running;
  std::uint32_t targets = SchedulingWakeNone;

  constexpr bool wakes(SchedulingWakeTarget target) const noexcept {
    return (targets & static_cast<std::uint32_t>(target)) != 0;
  }
};

constexpr SchedulingTerminalWakePlan planSchedulingTerminalWake(
    SchedulingTerminalDisposition disposition) noexcept {
  if (disposition == SchedulingTerminalDisposition::Running) {
    return {disposition, SchedulingWakeNone};
  }
  return {
      disposition,
      SchedulingWakeWriter | SchedulingWakeEncoder | SchedulingWakeFinish |
          SchedulingWakePresentCompleted | SchedulingWakePresentDequeued |
          SchedulingWakeSessionRelease | SchedulingWakePendingCompletion,
  };
}

}  // namespace dxmt9::render
