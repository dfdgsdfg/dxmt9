#include "dxmt9_trace_gpu.hpp"

#include <cmath>
#include <algorithm>
#include <string>

namespace dxmt9::trace::gpu {
namespace {

bool supports(Level level, const Capabilities& capabilities) noexcept {
  switch (level) {
  case Level::CommandBuffer:
    return capabilities.commandBufferTime == Support::Supported;
  case Level::EncoderStage:
    return capabilities.stageBoundary == Support::Supported;
  case Level::DrawBoundary:
    // Draw boundary capability is reported for the device, but this API only
    // installs render-encoder start/end attachments.
    return false;
  }
  return false;
}

bool validCalibration(const Calibration& value) noexcept {
  return value.valid && value.mappingId != 0 && value.sourceClockId != 0 &&
      value.hostClockId != 0 &&
      std::isfinite(value.scale) && value.scale > 0.0 &&
      std::isfinite(value.offset) && value.validBegin < value.validEnd &&
      (value.observationCount > 0 || value.sourceClockId == value.hostClockId);
}

}  // namespace

StartResult Collector::start(const SessionConfig& config,
                             const Capabilities& capabilities) noexcept {
  if (sessionControl_.test_and_set(std::memory_order_acquire)) {
    return StartResult::Busy;
  }
  struct ControlGuard {
    std::atomic_flag& flag;
    ~ControlGuard() { flag.clear(std::memory_order_release); }
  } guard{sessionControl_};
  if (config.sessionGeneration == 0 || config.capacity == 0 ||
      config.capacity > kMaxSamples) {
    return StartResult::InvalidConfig;
  }
  if (!supports(config.level, capabilities)) {
    return StartResult::UnsupportedLevel;
  }
  if (open_.load(std::memory_order_acquire)) {
    return StartResult::Busy;
  }
  if (activeReservers_.load(std::memory_order_acquire) != 0) {
    return StartResult::Busy;
  }
  bool allFree = true;
  for (const auto& slot : slots_) {
    allFree &= slot.state.load(std::memory_order_acquire) ==
        static_cast<std::uint8_t>(SlotState::Free);
  }
  if (!retirement::canBeginSession(
          open_.load(std::memory_order_acquire),
          activeReservers_.load(std::memory_order_acquire), allFree)) {
    return StartResult::Busy;
  }

  selected_.store(0, std::memory_order_relaxed);
  completed_.store(0, std::memory_order_relaxed);
  lostCapacity_.store(0, std::memory_order_relaxed);
  invalid_.store(0, std::memory_order_relaxed);
  stale_.store(0, std::memory_order_relaxed);
  cursor_.store(0, std::memory_order_relaxed);
  level_ = config.level;
  calibration_ = validCalibration(config.calibration) ? config.calibration
                                                      : Calibration{};
  capacity_.store(config.capacity, std::memory_order_relaxed);
  sessionGeneration_.store(config.sessionGeneration, std::memory_order_relaxed);
  open_.store(true, std::memory_order_release);
  return StartResult::Started;
}

Ticket Collector::reserve(const WorkIdentity& work) noexcept {
  const std::uint64_t observedGeneration =
      sessionGeneration_.load(std::memory_order_acquire);
  if (!open_.load(std::memory_order_acquire)) {
    return {};
  }
  activeReservers_.fetch_add(1, std::memory_order_acq_rel);
  if (!open_.load(std::memory_order_acquire) ||
      sessionGeneration_.load(std::memory_order_acquire) != observedGeneration) {
    activeReservers_.fetch_sub(1, std::memory_order_release);
    return {};
  }
  const std::uint32_t capacity = capacity_.load(std::memory_order_relaxed);
  if (capacity == 0) {
    activeReservers_.fetch_sub(1, std::memory_order_release);
    return {};
  }
  const std::uint32_t first = cursor_.fetch_add(1, std::memory_order_relaxed);
  for (std::uint32_t offset = 0; offset < capacity; ++offset) {
    const std::uint32_t index = (first + offset) % capacity;
    auto& slot = slots_[index];
    std::uint8_t expected = static_cast<std::uint8_t>(SlotState::Free);
    if (!slot.state.compare_exchange_strong(
            expected, static_cast<std::uint8_t>(SlotState::Initializing),
                                             std::memory_order_acq_rel,
                                             std::memory_order_relaxed)) {
      continue;
    }
    if (!open_.load(std::memory_order_acquire)) {
      slot.state.store(static_cast<std::uint8_t>(SlotState::Free),
                       std::memory_order_release);
      activeReservers_.fetch_sub(1, std::memory_order_release);
      return {};
    }
    slot.work = work;
    slot.completion = {};
    std::uint64_t generation = slot.generation.fetch_add(
        1, std::memory_order_relaxed) + 1;
    if (generation == 0) {
      generation = slot.generation.fetch_add(1, std::memory_order_relaxed) + 1;
    }
    slot.state.store(static_cast<std::uint8_t>(SlotState::Reserved),
                     std::memory_order_release);
    selected_.fetch_add(1, std::memory_order_relaxed);
    activeReservers_.fetch_sub(1, std::memory_order_release);
    return Ticket{observedGeneration, index, generation};
  }
  lostCapacity_.fetch_add(1, std::memory_order_relaxed);
  activeReservers_.fetch_sub(1, std::memory_order_release);
  return {};
}

void Collector::recordLostCapacity() noexcept {
  lostCapacity_.fetch_add(1, std::memory_order_relaxed);
}

bool Collector::complete(Ticket ticket,
                         const Completion& completion) noexcept {
  if (!ticket || ticket.slot >= capacity_.load(std::memory_order_relaxed) ||
      ticket.sessionGeneration !=
          sessionGeneration_.load(std::memory_order_acquire)) {
    stale_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  auto& slot = slots_[ticket.slot];
  if (!retirement::completionTicketMatches(
          sessionGeneration_.load(std::memory_order_acquire), ticket.slot,
          slots_[ticket.slot].generation.load(std::memory_order_relaxed),
          ticket.sessionGeneration, ticket.slot, ticket.slotGeneration)) {
    stale_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  std::uint8_t expected = static_cast<std::uint8_t>(SlotState::Reserved);
  if (!slot.state.compare_exchange_strong(
          expected, static_cast<std::uint8_t>(SlotState::Completing),
          std::memory_order_acq_rel, std::memory_order_relaxed)) {
    if (expected == static_cast<std::uint8_t>(SlotState::Reserved)) {
      slot.state.store(static_cast<std::uint8_t>(SlotState::Reserved),
                       std::memory_order_release);
    }
    stale_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  if (!retirement::completionTicketMatches(
          sessionGeneration_.load(std::memory_order_acquire), ticket.slot,
          slot.generation.load(std::memory_order_relaxed),
          ticket.sessionGeneration, ticket.slot, ticket.slotGeneration)) {
    slot.state.store(static_cast<std::uint8_t>(SlotState::Reserved),
                     std::memory_order_release);
    stale_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  slot.completion = completion;
  slot.state.store(static_cast<std::uint8_t>(SlotState::Ready),
                   std::memory_order_release);
  return true;
}

bool Collector::drainOne(Sample& sample) noexcept {
  const std::uint32_t capacity = capacity_.load(std::memory_order_relaxed);
  for (std::uint32_t index = 0; index < capacity; ++index) {
    auto& slot = slots_[index];
    std::uint8_t expected = static_cast<std::uint8_t>(SlotState::Ready);
    if (!slot.state.compare_exchange_strong(
            expected, static_cast<std::uint8_t>(SlotState::Draining),
                                             std::memory_order_acq_rel,
                                             std::memory_order_relaxed)) {
      continue;
    }
    sample = {};
    sample.sessionGeneration = sessionGeneration_.load(std::memory_order_relaxed);
    sample.slotGeneration = slot.generation.load(std::memory_order_relaxed);
    sample.work = slot.work;
    sample.level = level_;
    sample.sourceClockId = level_ == Level::CommandBuffer
        ? kMachAbsoluteTimeClockId
        : kMetalGpuCounterClockId;
    sample.gpuStartNs = slot.completion.gpuStartNs;
    sample.gpuEndNs = slot.completion.gpuEndNs;
    sample.calibration = calibration_;
    if (slot.completion.status != CommandBufferStatus::Completed) {
      sample.validity = ResultValidity::CommandBufferFailed;
    } else if (sample.gpuStartNs == 0 || sample.gpuEndNs == 0) {
      sample.validity = ResultValidity::MissingTimestamp;
    } else if (sample.gpuEndNs <= sample.gpuStartNs) {
      sample.validity = ResultValidity::NonMonotonicTimestamp;
    } else {
      sample.validity = ResultValidity::Valid;
      sample.durationNs = sample.gpuEndNs - sample.gpuStartNs;
      sample.calibrationApplies = calibration_.valid &&
          (sample.gpuStartNs >= calibration_.validBegin &&
           sample.gpuEndNs <= calibration_.validEnd);
    }
    if (sample.validity == ResultValidity::Valid) {
      completed_.fetch_add(1, std::memory_order_relaxed);
    } else {
      invalid_.fetch_add(1, std::memory_order_relaxed);
    }
    slot.state.store(static_cast<std::uint8_t>(SlotState::Free),
                     std::memory_order_release);
    return true;
  }
  return false;
}

void Collector::close() noexcept {
  open_.store(false, std::memory_order_release);
}

bool Collector::isOpen() const noexcept {
  return open_.load(std::memory_order_acquire);
}

Counters Collector::counters() const noexcept {
  return Counters{
      .selected = selected_.load(std::memory_order_relaxed),
      .completed = completed_.load(std::memory_order_relaxed),
      .lostCapacity = lostCapacity_.load(std::memory_order_relaxed),
      .invalid = invalid_.load(std::memory_order_relaxed),
      .stale = stale_.load(std::memory_order_relaxed),
  };
}

bool calibrateFromAnchors(std::uint64_t mappingId,
                          std::uint64_t sourceClockId,
                          std::uint64_t hostClockId,
                          const ClockAnchor& first,
                          const ClockAnchor& second,
                          Calibration& calibration) noexcept {
  calibration = {};
  if (mappingId == 0 || sourceClockId == 0 || hostClockId == 0 ||
      first.hostBeforeNs == 0 || first.cpuTimestamp == 0 ||
      first.gpuTimestamp == 0 || first.hostAfterNs < first.hostBeforeNs ||
      second.hostBeforeNs == 0 || second.cpuTimestamp == 0 ||
      second.gpuTimestamp == 0 || second.hostAfterNs < second.hostBeforeNs ||
      second.cpuTimestamp <= first.cpuTimestamp ||
      second.gpuTimestamp <= first.gpuTimestamp ||
      second.hostBeforeNs <= first.hostAfterNs) {
    return false;
  }
  const std::uint64_t firstHost = first.hostBeforeNs +
      (first.hostAfterNs - first.hostBeforeNs) / 2u;
  const std::uint64_t secondHost = second.hostBeforeNs +
      (second.hostAfterNs - second.hostBeforeNs) / 2u;
  if (secondHost <= firstHost) {
    return false;
  }
  const double scale = static_cast<double>(secondHost - firstHost) /
      static_cast<double>(second.gpuTimestamp - first.gpuTimestamp);
  const double offset = static_cast<double>(firstHost) -
      scale * static_cast<double>(first.gpuTimestamp);
  if (!std::isfinite(scale) || scale <= 0.0 || !std::isfinite(offset)) {
    return false;
  }
  const auto firstUncertainty =
      (first.hostAfterNs - first.hostBeforeNs + 1u) / 2u;
  const auto secondUncertainty =
      (second.hostAfterNs - second.hostBeforeNs + 1u) / 2u;
  calibration = Calibration{
      .mappingId = mappingId,
      .sourceClockId = sourceClockId,
      .hostClockId = hostClockId,
      .scale = scale,
      .offset = offset,
      .validBegin = first.gpuTimestamp,
      .validEnd = second.gpuTimestamp,
      .firstSourceObservation = first.gpuTimestamp,
      .firstHostObservationNs = firstHost,
      .secondSourceObservation = second.gpuTimestamp,
      .secondHostObservationNs = secondHost,
      .uncertaintyNs = std::max(firstUncertainty, secondUncertainty),
      .observationCount = 2,
      .valid = true,
  };
  return true;
}

Calibration commandBufferHostTimeCalibration(std::uint64_t mappingId,
                                             std::uint64_t validBeginNs,
                                             std::uint64_t validEndNs,
                                             std::uint64_t uncertaintyNs) noexcept {
  if (mappingId == 0 || validBeginNs >= validEndNs) {
    return {};
  }
  return Calibration{
      .mappingId = mappingId,
      .sourceClockId = kMachAbsoluteTimeClockId,
      .hostClockId = kMachAbsoluteTimeClockId,
      .scale = 1.0,
      .offset = 0.0,
      .validBegin = validBeginNs,
      .validEnd = validEndNs,
      .uncertaintyNs = uncertaintyNs,
      .observationCount = 0,
      .valid = true,
  };
}

}  // namespace dxmt9::trace::gpu
