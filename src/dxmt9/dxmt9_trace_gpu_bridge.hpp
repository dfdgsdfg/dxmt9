#pragma once

#include "dxmt9_trace_gpu.hpp"

#include <cstddef>
#include <cstdint>

namespace dxmt9::trace::gpu {

using SampleConsumer = void (*)(void* context, const Sample& sample) noexcept;
using StageSampleConsumer = void (*)(void* context,
                                     const StageSample& sample) noexcept;

// Thread-local encode context. Construct only behind the trace-enabled gate;
// this object has no Metal or queue state and restores nested scopes.
class StageWorkScope {
public:
  constexpr StageWorkScope() noexcept = default;
  explicit StageWorkScope(WorkIdentity work) noexcept;
  StageWorkScope(StageWorkScope&& other) noexcept;
  ~StageWorkScope() {
    if (pushed_) restore(previousDepth_);
  }
  StageWorkScope(const StageWorkScope&) = delete;
  StageWorkScope& operator=(const StageWorkScope&) = delete;

private:
  static void restore(std::uint8_t previousDepth) noexcept;
  std::uint8_t previousDepth_ = 0;
  bool pushed_ = false;
};

class SubmissionBridge {
public:
  SubmissionBridge() noexcept = default;
  ~SubmissionBridge();
  SubmissionBridge(const SubmissionBridge&) = delete;
  SubmissionBridge& operator=(const SubmissionBridge&) = delete;
  SubmissionBridge(SubmissionBridge&& other) noexcept;
  SubmissionBridge& operator=(SubmissionBridge&& other) noexcept;

  bool start(const SessionConfig& config,
             const Capabilities& capabilities) noexcept;

  // Called before commit. The caller owns commandBufferId uniqueness and
  // supplies the actual device/queue/sequence identity. Zero means admission
  // or capacity failed; rendering continues without a sample.
  std::uint64_t observeSubmission(WorkIdentity identity,
                                 std::uintptr_t commandBufferHandle) noexcept;

  // Diagnostic control path only: polls status without waiting and publishes
  // completed GPU timestamps to bounded collector storage.
  std::size_t pollCompleted() noexcept;
  bool publishCompletionHint(const WorkIdentity& work,
                             CommandBufferStatus status) noexcept;
  std::size_t drain(SampleConsumer consumer, void* context) noexcept;
  void close() noexcept;
  bool isDrained() const noexcept;
  Counters counters() const noexcept;

  // Late completions for a closed bridge are kept in a bounded process-local
  // quarantine until this diagnostic poll resolves them naturally.
  static std::size_t pollQuarantined(SampleConsumer consumer,
                                     void* context) noexcept;

private:
  mutable std::atomic<void*> impl_{nullptr};
  mutable std::atomic<std::uint64_t> callGate_{0};
};

// Runtime process facade. It is initialized on the trace control path after
// the Unix Metal device exists; queue objects carry no tracing state.
bool initializeProcessGpuTrace(WMT::Device& device,
                               const SessionConfig& config) noexcept;
bool initializeProcessGpuTraceHandle(std::uintptr_t deviceHandle,
                                     const SessionConfig& config) noexcept;
bool processGpuTraceEnabled() noexcept;
std::uint64_t registerProcessGpuDevice(std::uintptr_t deviceHandle) noexcept;
std::uint64_t processGpuDeviceGeneration(
    std::uintptr_t deviceHandle) noexcept;
std::uint64_t processCommandBufferDeviceGeneration(
    std::uintptr_t commandBufferHandle) noexcept;
std::uint64_t processCommandBufferId(std::uintptr_t commandBufferHandle,
                                     std::uintptr_t queueHandle,
                                     std::uint64_t sequenceId) noexcept;
std::uint64_t findProcessCommandBufferId(
    std::uintptr_t commandBufferHandle,
    std::uintptr_t queueHandle) noexcept;
bool setProcessCommandBufferDeviceGeneration(
    std::uintptr_t commandBufferHandle, std::uintptr_t queueHandle,
    std::uint64_t sequenceId, std::uint64_t deviceGeneration) noexcept;
void retireProcessCommandBufferId(std::uint64_t commandBufferId) noexcept;
void retireProcessCommandBufferIdsThrough(
    std::uintptr_t queueIdentity, std::uint64_t completedSequence) noexcept;
void publishProcessGpuCompletion(const CompletionEvidence& evidence) noexcept;
bool publishCompletedProcessGpuCommandBuffer(
    std::uintptr_t commandBufferHandle, std::uintptr_t queueIdentity,
    std::uint64_t sequenceId, CommandBufferStatus status) noexcept;
std::uint64_t processGpuCompletionLosses() noexcept;
StagePairReservation reserveCurrentStageAttachment(
    std::uintptr_t deviceHandle,
    std::uintptr_t commandBufferHandle) noexcept;
std::size_t resolveProcessGpuStageSamples(
    const CompletionEvidence& evidence) noexcept;
std::size_t drainProcessGpuStageSamples(StageSampleConsumer consumer,
                                       void* context) noexcept;
void closeProcessGpuStageSamples() noexcept;
bool processGpuStageSamplesDrained() noexcept;
StageCounters processGpuStageCounters() noexcept;
std::uint64_t observeProcessGpuSubmission(
    WorkIdentity identity, std::uintptr_t commandBufferHandle) noexcept;
std::size_t pollProcessGpuTrace() noexcept;
std::size_t drainProcessGpuTrace(SampleConsumer consumer,
                                 void* context) noexcept;
void closeProcessGpuTrace() noexcept;
bool processGpuTraceDrained() noexcept;
Counters processGpuTraceCounters() noexcept;

}  // namespace dxmt9::trace::gpu
