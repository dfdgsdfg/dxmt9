#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "../winemetal/winemetal.h"
#include "tracing/gpu_retirement_state.hpp"

namespace WMT {
class Device;
}

namespace dxmt9::trace::gpu {

constexpr std::size_t kMaxSamples = 256;
constexpr std::size_t kMaxStagePairs = 256;

enum class Level : std::uint8_t {
  CommandBuffer,
  EncoderStage,
  DrawBoundary,
};

enum class Support : std::uint8_t {
  Supported,
  Unsupported,
};

enum class CommandBufferStatus : std::uint8_t {
  Completed,
  Failed,
  Unknown,
};

enum class ResultValidity : std::uint8_t {
  Valid,
  CommandBufferFailed,
  MissingTimestamp,
  NonMonotonicTimestamp,
  StaleTicket,
};

struct Capabilities {
  Support commandBufferTime = Support::Unsupported;
  Support stageBoundary = Support::Unsupported;
  Support drawBoundary = Support::Unsupported;
  Support blitBoundary = Support::Unsupported;
  Support dispatchBoundary = Support::Unsupported;
  Support tileDispatchBoundary = Support::Unsupported;
};

// Maps raw source ticks to host nanoseconds. Command-buffer API timestamps are
// nanoseconds after WMT conversion and use the documented mach absolute host
// clock. Counter samples keep their device timestamp ticks and use paired
// sampleTimestamps anchors; absent calibration prevents cross-clock placement.
struct Calibration {
  std::uint64_t mappingId = 0;
  std::uint64_t sourceClockId = 0;
  std::uint64_t hostClockId = 0;
  double scale = 0.0;
  double offset = 0.0;
  std::uint64_t validBegin = 0;
  std::uint64_t validEnd = 0;
  std::uint64_t firstSourceObservation = 0;
  std::uint64_t firstHostObservationNs = 0;
  std::uint64_t secondSourceObservation = 0;
  std::uint64_t secondHostObservationNs = 0;
  std::uint64_t uncertaintyNs = 0;
  std::uint32_t observationCount = 0;
  bool valid = false;
};

struct ClockAnchor {
  std::uint64_t hostBeforeNs = 0;
  std::uint64_t cpuTimestamp = 0;
  std::uint64_t gpuTimestamp = 0;
  std::uint64_t hostAfterNs = 0;
};

constexpr std::uint64_t kMachAbsoluteTimeClockId = 0x6d6163685f616273ull;
constexpr std::uint64_t kMetalGpuCounterClockId = 0x6d746c5f67707574ull;

struct SessionConfig {
  std::uint64_t sessionGeneration = 0;
  std::uint32_t capacity = 0;
  Level level = Level::CommandBuffer;
  bool required = false;
  Calibration calibration{};
};

struct WorkIdentity {
  std::uint64_t deviceGeneration = 0;
  std::uint64_t queueGeneration = 0;
  std::uint64_t commandBufferId = 0;
  std::uint64_t sequenceId = 0;
};

struct Ticket {
  std::uint64_t sessionGeneration = 0;
  std::uint32_t slot = 0;
  std::uint64_t slotGeneration = 0;

  explicit operator bool() const noexcept {
    return sessionGeneration != 0 && slotGeneration != 0;
  }
};

struct Completion {
  CommandBufferStatus status = CommandBufferStatus::Unknown;
  std::uint64_t gpuStartNs = 0;
  std::uint64_t gpuEndNs = 0;
};

struct Sample {
  std::uint64_t sessionGeneration = 0;
  std::uint64_t slotGeneration = 0;
  WorkIdentity work{};
  Level level = Level::CommandBuffer;
  std::uint64_t sourceClockId = 0;
  ResultValidity validity = ResultValidity::MissingTimestamp;
  std::uint64_t gpuStartNs = 0;
  std::uint64_t gpuEndNs = 0;
  std::uint64_t durationNs = 0;
  Calibration calibration{};
  bool calibrationApplies = false;
};

enum class StartResult : std::uint8_t {
  Started,
  InvalidConfig,
  Busy,
  UnsupportedLevel,
};

struct Counters {
  std::uint64_t selected = 0;
  std::uint64_t completed = 0;
  std::uint64_t lostCapacity = 0;
  std::uint64_t invalid = 0;
  std::uint64_t stale = 0;
};

struct StagePairTicket {
  std::uint64_t sessionGeneration = 0;
  std::uint32_t pairIndex = 0;
  std::uint64_t pairGeneration = 0;

  explicit operator bool() const noexcept {
    return sessionGeneration != 0 && pairGeneration != 0;
  }
};

struct StagePairReservation {
  StagePairTicket ticket{};
  WMTSampleBufferAttachmentInfo attachment{};

  explicit operator bool() const noexcept { return static_cast<bool>(ticket); }
};

struct StagePairResult {
  std::uint64_t startTimestamp = 0;
  std::uint64_t endTimestamp = 0;
  ResultValidity validity = ResultValidity::MissingTimestamp;
};

struct StageSample {
  std::uint64_t sessionGeneration = 0;
  std::uint64_t slotGeneration = 0;
  std::uint32_t pairIndex = 0;
  WorkIdentity work{};
  std::uint64_t sourceClockId = kMetalGpuCounterClockId;
  StagePairResult result{};
  Calibration calibration{};
  bool calibrationApplies = false;
};

struct StageCounters {
  std::uint64_t selected = 0;
  std::uint64_t resolved = 0;
  std::uint64_t lostCapacity = 0;
  std::uint64_t invalid = 0;
  std::uint64_t stale = 0;
};

struct CompletionEvidence {
  WorkIdentity work{};
  CommandBufferStatus status = CommandBufferStatus::Unknown;
  bool observedAfterCompletion = false;
};

class StageSampleBuffer {
public:
  StageSampleBuffer() noexcept = default;
  ~StageSampleBuffer();
  StageSampleBuffer(const StageSampleBuffer&) = delete;
  StageSampleBuffer& operator=(const StageSampleBuffer&) = delete;
  StageSampleBuffer(StageSampleBuffer&& other) noexcept;
  StageSampleBuffer& operator=(StageSampleBuffer&& other) noexcept;

  bool start(WMT::Device& device, std::uint64_t sessionGeneration,
             std::uint32_t pairCapacity) noexcept;
  StagePairReservation reserve(const WorkIdentity& work,
      const WMTSampleBufferAttachmentInfo* sharedAttachment = nullptr) noexcept;
  bool resolve(StagePairTicket ticket, const CompletionEvidence& evidence,
               StagePairResult& result) noexcept;
  static bool resolveQuarantined(StagePairTicket ticket,
                                 const CompletionEvidence& evidence,
                                 StagePairResult& result) noexcept;
  void close() noexcept;
  bool isOpen() const noexcept;
  bool canRetire() const noexcept;
  StageCounters counters() const noexcept;

private:
  mutable std::atomic<void*> impl_{nullptr};
  mutable std::atomic<std::uint64_t> callGate_{0};
};

class Collector {
public:
  Collector() noexcept = default;
  Collector(const Collector&) = delete;
  Collector& operator=(const Collector&) = delete;

  StartResult start(const SessionConfig& config,
                    const Capabilities& capabilities) noexcept;
  Ticket reserve(const WorkIdentity& work) noexcept;
  bool complete(Ticket ticket, const Completion& completion) noexcept;
  bool drainOne(Sample& sample) noexcept;
  void recordLostCapacity() noexcept;
  void close() noexcept;
  bool isOpen() const noexcept;
  Counters counters() const noexcept;

private:
  using SlotState = retirement::SlotState;

  struct Slot {
    std::atomic<std::uint8_t> state{
        static_cast<std::uint8_t>(SlotState::Free)};
    std::atomic<std::uint64_t> generation{0};
    WorkIdentity work{};
    Completion completion{};
  };

  std::array<Slot, kMaxSamples> slots_{};
  std::atomic_flag sessionControl_ = ATOMIC_FLAG_INIT;
  std::atomic<std::uint64_t> sessionGeneration_{0};
  std::atomic<std::uint32_t> activeReservers_{0};
  std::atomic<std::uint32_t> capacity_{0};
  std::atomic<std::uint32_t> cursor_{0};
  std::atomic<bool> open_{false};
  Level level_ = Level::CommandBuffer;
  Calibration calibration_{};
  std::atomic<std::uint64_t> selected_{0};
  std::atomic<std::uint64_t> completed_{0};
  std::atomic<std::uint64_t> lostCapacity_{0};
  std::atomic<std::uint64_t> invalid_{0};
  std::atomic<std::uint64_t> stale_{0};
};

bool calibrateFromAnchors(std::uint64_t mappingId,
                          std::uint64_t sourceClockId,
                          std::uint64_t hostClockId,
                          const ClockAnchor& first,
                          const ClockAnchor& second,
                          Calibration& calibration) noexcept;
Calibration commandBufferHostTimeCalibration(std::uint64_t mappingId,
                                             std::uint64_t validBeginNs,
                                             std::uint64_t validEndNs,
                                             std::uint64_t uncertaintyNs) noexcept;
bool sampleClockAnchor(WMT::Device& device, ClockAnchor& anchor) noexcept;
Capabilities queryCapabilities(WMT::Device& device) noexcept;

}  // namespace dxmt9::trace::gpu
