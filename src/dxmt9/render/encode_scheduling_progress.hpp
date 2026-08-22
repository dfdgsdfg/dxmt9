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
  // For a SegmentSerial batch this is the conjunction over every required
  // contiguous control slot, not only the slot at writeIndex.
  bool controlSlotsFree = false;
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
      gate.controlSlotsFree && !gate.reserveStillPressured) {
    return CpuReadyAdmissionAction::RetryAdmission;
  }
  return CpuReadyAdmissionAction::Wait;
}

enum class FirstLeaseCapacityWaitAction : std::uint8_t {
  Wait,
  RetryLease,
  ExecuteOneSourceSerial,
  Stop,
};

enum class FirstLeaseReadyHeadEligibility : std::uint8_t {
  Eligible,
  NonArena,
  Present,
  OrdinaryCapacity,
  HighWater,
};

struct FirstLeaseReadyHeadState {
  bool arena = false;
  bool present = false;
  bool fitsOrdinaryCapacity = false;
  bool fitsHighWater = false;
};

constexpr FirstLeaseReadyHeadEligibility
classifyFirstLeaseReadyHeadEligibility(
    FirstLeaseReadyHeadState state) noexcept {
  if (!state.arena) {
    return FirstLeaseReadyHeadEligibility::NonArena;
  }
  if (state.present) {
    return FirstLeaseReadyHeadEligibility::Present;
  }
  if (!state.fitsOrdinaryCapacity) {
    return FirstLeaseReadyHeadEligibility::OrdinaryCapacity;
  }
  if (!state.fitsHighWater) {
    return FirstLeaseReadyHeadEligibility::HighWater;
  }
  return FirstLeaseReadyHeadEligibility::Eligible;
}

struct FirstLeaseCapacityWaitState {
  bool stopped = false;
  bool admissionPressure = false;
  bool serialProgressAvailable = false;
  bool readyHeadOwnsOrdinaryDirectCapacity = false;
  std::uint64_t observedGeneration = 0;
  std::uint64_t currentGeneration = 0;
};

// A capacity generation always gets the first retry: it may make the complete
// fixed lease available without changing grouping. Admission pressure may use
// one exact already-resident ordinary Direct head only once before another
// capacity transition; this creates no SessionReleaseEvent and reserves no new
// Tape capacity.
constexpr FirstLeaseCapacityWaitAction classifyFirstLeaseCapacityWait(
    FirstLeaseCapacityWaitState state) noexcept {
  if (state.stopped) {
    return FirstLeaseCapacityWaitAction::Stop;
  }
  if (state.currentGeneration != state.observedGeneration) {
    return FirstLeaseCapacityWaitAction::RetryLease;
  }
  if (state.admissionPressure && state.serialProgressAvailable &&
      state.readyHeadOwnsOrdinaryDirectCapacity) {
    return FirstLeaseCapacityWaitAction::ExecuteOneSourceSerial;
  }
  return FirstLeaseCapacityWaitAction::Wait;
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
