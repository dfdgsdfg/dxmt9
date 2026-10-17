#include "../../../src/dxmt9/dxmt9_trace_gpu.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void check(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

dxmt9::trace::gpu::Capabilities commandBufferCapabilities() {
  using namespace dxmt9::trace::gpu;
  return Capabilities{.commandBufferTime = Support::Supported};
}

void asyncRetirementAndIdentity() {
  using namespace dxmt9::trace::gpu;
  Collector collector;
  check(collector.start(SessionConfig{.sessionGeneration = 41,
                                     .capacity = 2,
                                     .level = Level::CommandBuffer},
                        commandBufferCapabilities()) == StartResult::Started,
        "command-buffer collection should start");

  const Ticket first = collector.reserve(WorkIdentity{
      .deviceGeneration = 3, .queueGeneration = 7,
      .commandBufferId = 101, .sequenceId = 9001});
  const Ticket second = collector.reserve(WorkIdentity{
      .deviceGeneration = 3, .queueGeneration = 8,
      .commandBufferId = 102, .sequenceId = 9002});
  check(first && second && first.slot != second.slot,
        "reservations must own distinct bounded slots");
  check(!collector.reserve(WorkIdentity{}),
        "full capacity must drop without overwriting pending work");

  check(collector.complete(second, Completion{.status = CommandBufferStatus::Completed,
                                               .gpuStartNs = 500,
                                               .gpuEndNs = 575}),
        "out-of-order completion should publish readiness");
  Sample sample;
  check(collector.drainOne(sample), "ready completion should drain");
  check(sample.sessionGeneration == 41 && sample.work.sequenceId == 9002 &&
            sample.durationNs == 75 && sample.validity == ResultValidity::Valid,
        "sample must retain actual identity and GPU interval");
  check(!sample.calibration.valid,
        "missing clock calibration must remain explicitly absent");
  check(!sample.calibrationApplies,
        "uncalibrated GPU samples must not align to the host timeline");

  check(collector.complete(first, Completion{.status = CommandBufferStatus::Completed,
                                              .gpuStartNs = 600,
                                              .gpuEndNs = 600}),
        "second completion should publish");
  check(collector.drainOne(sample) &&
            sample.validity == ResultValidity::NonMonotonicTimestamp &&
            sample.durationNs == 0,
        "non-monotonic endpoints must remain invalid, not measured zero");
  const Counters counts = collector.counters();
  check(counts.selected == 2 && counts.completed == 1 && counts.invalid == 1 &&
            counts.lostCapacity == 1,
        "loss and valid/invalid completion counts must be distinct");
}

void generationsAndClose() {
  using namespace dxmt9::trace::gpu;
  Collector collector;
  const auto capabilities = commandBufferCapabilities();
  check(collector.start(SessionConfig{.sessionGeneration = 5,
                                     .capacity = 1}, capabilities) == StartResult::Started,
        "initial session should start");
  const Ticket old = collector.reserve(WorkIdentity{.commandBufferId = 1});
  check(collector.complete(old, Completion{.status = CommandBufferStatus::Completed,
                                            .gpuStartNs = 10, .gpuEndNs = 20}),
        "initial completion should publish");
  Sample sample;
  check(collector.drainOne(sample), "initial sample should drain");
  collector.close();
  check(!collector.isOpen() && !collector.reserve(WorkIdentity{}),
        "closed collection must reject new work");
  check(collector.start(SessionConfig{.sessionGeneration = 6,
                                     .capacity = 1}, capabilities) == StartResult::Started,
        "drained collector should accept a fresh session");
  const Ticket current = collector.reserve(WorkIdentity{.commandBufferId = 2});
  check(!collector.complete(old, Completion{.status = CommandBufferStatus::Completed,
                                             .gpuStartNs = 30, .gpuEndNs = 40}),
        "late prior-session callback must be rejected");
  check(collector.complete(current, Completion{.status = CommandBufferStatus::Failed,
                                               .gpuStartNs = 30, .gpuEndNs = 40}),
        "failed command buffer should publish an invalid result");
  check(collector.drainOne(sample) &&
            sample.validity == ResultValidity::CommandBufferFailed,
        "failed GPU work must not contribute a successful duration");
  check(collector.counters().stale == 1,
        "late callback rejection must be counted");
}

void closeDrainsOutstandingAndRejectsStaleSlotGeneration() {
  using namespace dxmt9::trace::gpu;
  Collector collector;
  const auto caps = commandBufferCapabilities();
  check(collector.start(SessionConfig{.sessionGeneration = 17,
                                     .capacity = 1}, caps) == StartResult::Started,
        "session should start");
  const Ticket first = collector.reserve(WorkIdentity{.commandBufferId = 10});
  collector.close();
  check(collector.start(SessionConfig{.sessionGeneration = 18,
                                     .capacity = 1}, caps) == StartResult::Busy,
        "session close must retain outstanding reservations for natural retirement");
  check(collector.complete(first, Completion{.status = CommandBufferStatus::Completed,
                                              .gpuStartNs = 100,
                                              .gpuEndNs = 125}),
        "late completion after close must still publish");
  Sample sample;
  check(collector.drainOne(sample) && sample.sessionGeneration == 17,
        "late work remains attributed to its originating session");
  check(collector.start(SessionConfig{.sessionGeneration = 18,
                                     .capacity = 1}, caps) == StartResult::Started,
        "new session may start after all old readers retire");
  const Ticket second = collector.reserve(WorkIdentity{.commandBufferId = 11});
  check(second.slot == first.slot && second.slotGeneration != first.slotGeneration,
        "reused storage must get a fresh slot generation");
  check(!collector.complete(first, Completion{.status = CommandBufferStatus::Completed,
                                               .gpuStartNs = 1,
                                               .gpuEndNs = 2}),
        "late old slot generation must not complete a reused slot");
  check(collector.complete(second, Completion{.status = CommandBufferStatus::Completed,
                                               .gpuStartNs = 200,
                                               .gpuEndNs = 250}),
        "current slot generation should remain completable");
  check(collector.drainOne(sample) && sample.work.commandBufferId == 11,
        "stale callback must not overwrite new work");
}

void levelSelectionAndCalibration() {
  using namespace dxmt9::trace::gpu;
  Collector collector;
  Capabilities caps = commandBufferCapabilities();
  caps.stageBoundary = Support::Supported;
  check(collector.start(SessionConfig{.sessionGeneration = 1,
                                     .capacity = 1,
                                     .level = Level::EncoderStage}, caps) ==
            StartResult::Started,
        "supported stage-boundary sampling may be selected");
  collector.close();
  check(collector.start(SessionConfig{.sessionGeneration = 2,
                                     .capacity = 1,
                                     .level = Level::DrawBoundary}, caps) ==
            StartResult::UnsupportedLevel,
        "draw sampling must stay unsupported without per-draw attachment APIs");

  Calibration mapping{.mappingId = 8,
                      .sourceClockId = 10,
                      .hostClockId = 9,
                      .scale = 1.0,
                      .offset = 100.0,
                      .validBegin = 1000,
                      .validEnd = 2000,
                      .uncertaintyNs = 4,
                      .observationCount = 2,
                      .valid = true};
  check(collector.start(SessionConfig{.sessionGeneration = 3,
                                     .capacity = 1,
                                     .level = Level::CommandBuffer,
                                     .calibration = mapping}, caps) == StartResult::Started,
        "valid mapping may be attached to the session");
  const auto ticket = collector.reserve(WorkIdentity{});
  check(collector.complete(ticket, Completion{.status = CommandBufferStatus::Completed,
                                               .gpuStartNs = 1100,
                                               .gpuEndNs = 1200}),
        "calibrated sample should publish");
  Sample sample;
  check(collector.drainOne(sample) && sample.calibration.valid &&
            sample.calibration.mappingId == 8 && sample.calibration.uncertaintyNs == 4 &&
            sample.calibrationApplies,
        "qualified calibration metadata must survive retirement");
}

void calibrationRejectsInvalidAnchors() {
  using namespace dxmt9::trace::gpu;
  const ClockAnchor first{.hostBeforeNs = 100,
                          .cpuTimestamp = 1000,
                          .gpuTimestamp = 2000,
                          .hostAfterNs = 110};
  const ClockAnchor second{.hostBeforeNs = 200,
                           .cpuTimestamp = 1100,
                           .gpuTimestamp = 2200,
                           .hostAfterNs = 210};
  Calibration calibration;
  check(calibrateFromAnchors(7, 8, 9, first, second, calibration),
        "paired monotonic clock anchors should calibrate");
  check(calibration.valid && calibration.mappingId == 7 &&
            calibration.sourceClockId == 8 && calibration.hostClockId == 9 &&
            calibration.observationCount == 2 && calibration.uncertaintyNs == 5,
        "calibration must retain its domains, observations and bracket error");
  const ClockAnchor backward{.hostBeforeNs = 300,
                             .cpuTimestamp = 1200,
                             .gpuTimestamp = 2100,
                             .hostAfterNs = 310};
  check(!calibrateFromAnchors(7, 8, 9, second, backward, calibration) &&
            !calibration.valid,
        "nonmonotonic GPU clock anchors must not create a mapping");
  const Calibration sameClock =
      commandBufferHostTimeCalibration(11, 100, 200, 3);
  check(sameClock.valid && sameClock.sourceClockId == kMachAbsoluteTimeClockId &&
            sameClock.hostClockId == kMachAbsoluteTimeClockId &&
            sameClock.scale == 1.0 && sameClock.offset == 0.0,
        "documented command-buffer host-time domain should use an identity map");
}

}  // namespace

int main() {
  try {
    asyncRetirementAndIdentity();
    generationsAndClose();
    closeDrainsOutstandingAndRejectsStaleSlotGeneration();
    levelSelectionAndCalibration();
    calibrationRejectsInvalidAnchors();
  } catch (const std::exception& error) {
    std::cerr << "tracing GPU spec failed: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
