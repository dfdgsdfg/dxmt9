#include "dxmt9_trace_gpu_bridge.hpp"

#include "../winemetal/Metal.hpp"

#import <Metal/Metal.h>

#include <array>
#include <atomic>
#include <mutex>
#include <new>
#include <cstdint>
#include <thread>
#include <utility>

namespace dxmt9::trace::gpu {
namespace {

constexpr std::size_t kMaxBridgeSessions = 4;
constexpr std::size_t kMaxProcessCommandBuffers = kMaxSamples;
constexpr std::size_t kCompletionInboxCapacity = kMaxSamples;

struct ProcessCommandBufferRecord {
  std::atomic<std::uint32_t> state{0};
  std::uintptr_t handle = 0;
  std::uintptr_t queueHandle = 0;
  std::uint64_t sequenceId = 0;
  std::uint64_t id = 0;
  std::atomic<std::uint64_t> deviceGeneration{0};
};

struct CompletionInboxRecord {
  enum State : std::uint8_t { Free, Initializing, Ready, Reading };
  std::atomic<std::uint8_t> state{Free};
  WorkIdentity work{};
  CommandBufferStatus status = CommandBufferStatus::Unknown;
};

struct ProcessStageRecord {
  enum State : std::uint8_t { Free, Initializing, Reserved, Resolving, Ready };
  std::atomic<std::uint8_t> state{Free};
  StagePairTicket ticket{};
  WorkIdentity work{};
  StageSample sample{};
};

struct SubmissionSlot {
  enum State : std::uint8_t { Free, Initializing, Pending, Polling };
  std::atomic<std::uint8_t> state{Free};
  std::atomic<std::uint8_t> completionHint{0};
  WorkIdentity work{};
  Ticket ticket{};
  WMT::Reference<WMT::CommandBuffer> commandBuffer{};
};

struct BridgeImpl {
  std::array<SubmissionSlot, kMaxSamples> slots{};
  Collector collector{};
  std::atomic<bool> open{false};
  std::atomic<std::uint64_t> observerGate{0};
  std::atomic<std::uint32_t> pendingSamples{0};
  std::atomic<std::uint32_t> quarantineReaders{0};
  std::uint32_t capacity = 0;
};

constexpr std::uint64_t kCallGateClosed = std::uint64_t{1} << 63u;
constexpr std::uint64_t kCallGateCountMask = ~kCallGateClosed;

class BridgeCallLease {
public:
  BridgeCallLease(std::atomic<std::uint64_t>& gate,
                  std::atomic<void*>& pointer) noexcept : gate_(gate) {
    auto value = gate_.load(std::memory_order_acquire);
    while (!(value & kCallGateClosed)) {
      if (gate_.compare_exchange_weak(value, value + 1u,
                                     std::memory_order_acq_rel,
                                     std::memory_order_acquire)) {
        impl_ = static_cast<BridgeImpl*>(
            pointer.load(std::memory_order_acquire));
        acquired_ = true;
        return;
      }
    }
  }
  ~BridgeCallLease() {
    if (acquired_) gate_.fetch_sub(1u, std::memory_order_release);
  }
  BridgeImpl* get() const noexcept { return impl_; }

private:
  std::atomic<std::uint64_t>& gate_;
  BridgeImpl* impl_ = nullptr;
  bool acquired_ = false;
};

bool closeCallGateAndWait(std::atomic<std::uint64_t>& gate) noexcept {
  const auto previous = gate.fetch_or(kCallGateClosed,
                                      std::memory_order_acq_rel);
  if (previous & kCallGateClosed) return false;
  while ((gate.load(std::memory_order_acquire) & kCallGateCountMask) != 0) {
    std::this_thread::yield();
  }
  return true;
}

bool closeCallGateBounded(std::atomic<std::uint64_t>& gate) noexcept {
  const auto previous = gate.fetch_or(kCallGateClosed,
                                      std::memory_order_acq_rel);
  if (previous & kCallGateClosed) return false;
  for (std::uint32_t attempt = 0; attempt < 4096; ++attempt) {
    if ((gate.load(std::memory_order_acquire) & kCallGateCountMask) == 0) {
      return true;
    }
    std::this_thread::yield();
  }
  auto value = gate.load(std::memory_order_acquire);
  while (value & kCallGateClosed) {
    if (gate.compare_exchange_weak(value, value & kCallGateCountMask,
                                   std::memory_order_release,
                                   std::memory_order_acquire)) break;
  }
  return false;
}

constexpr std::uint64_t kObserverGateClosed = std::uint64_t{1} << 63u;
constexpr std::uint64_t kObserverCountMask = ~kObserverGateClosed;
constexpr std::uint32_t kIdentityInitializing = 1u;
constexpr std::uint32_t kIdentityLive = 0x80000000u;
constexpr std::uint32_t kIdentityRetiring = 0x40000000u;
constexpr std::uint32_t kIdentityReaderMask = 0x3fffffffu;

bool pinIdentityRecord(ProcessCommandBufferRecord& record) noexcept {
  auto state = record.state.load(std::memory_order_acquire);
  for (;;) {
    if ((state & kIdentityLive) == 0 ||
        (state & kIdentityReaderMask) == kIdentityReaderMask) return false;
    if (record.state.compare_exchange_weak(state, state + 1u,
                                            std::memory_order_acq_rel,
                                            std::memory_order_acquire)) return true;
  }
}

void unpinIdentityRecord(ProcessCommandBufferRecord& record) noexcept {
  const auto previous = record.state.fetch_sub(1u, std::memory_order_release);
  if ((previous & kIdentityRetiring) &&
      (previous & kIdentityReaderMask) == 1u) {
    record.state.store(0, std::memory_order_release);
  }
}

bool acquireObserver(BridgeImpl& impl) noexcept {
  auto gate = impl.observerGate.load(std::memory_order_acquire);
  for (;;) {
    if (gate & kObserverGateClosed) return false;
    if ((gate & kObserverCountMask) == kObserverCountMask) return false;
    if (impl.observerGate.compare_exchange_weak(
            gate, gate + 1u, std::memory_order_acq_rel,
            std::memory_order_acquire)) return true;
  }
}

void releaseObserver(BridgeImpl& impl) noexcept {
  impl.observerGate.fetch_sub(1u, std::memory_order_release);
}

std::array<std::atomic<BridgeImpl*>, kMaxBridgeSessions> gQuarantine{};
std::mutex gQuarantineMutex;
std::atomic<std::uint32_t> gLiveBridges{0};
SubmissionBridge gProcessBridge{};
StageSampleBuffer gProcessStageBuffer{};
std::array<ProcessStageRecord, kMaxStagePairs> gProcessStageRecords{};
std::array<ProcessCommandBufferRecord, kMaxProcessCommandBuffers>
    gProcessCommandBuffers{};
std::array<CompletionInboxRecord, kCompletionInboxCapacity> gCompletionInbox{};
std::atomic<std::uint64_t> gCompletionInboxLosses{0};
std::atomic<std::uint64_t> gNextCommandBufferId{1};
std::atomic<std::uint64_t> gNextProcessDeviceGeneration{1};
std::atomic<std::uint64_t> gProcessDeviceGeneration{0};
std::atomic<std::uintptr_t> gProcessDeviceHandle{0};
std::atomic<std::uint64_t> gIdentityRegistryLosses{0};
std::atomic<bool> gProcessGpuEnabled{false};
Calibration gProcessStageCalibration{};
std::atomic<bool> gProcessStageActive{false};
std::atomic<std::uintptr_t> gProcessStageDeviceHandle{0};
std::atomic_flag gProcessStageCalibrationControl = ATOMIC_FLAG_INIT;
ClockAnchor gProcessStageFirstAnchor{};
ClockAnchor gProcessStageLatestAnchor{};
bool gProcessStageFirstAnchorValid = false;
std::atomic<std::uint64_t> gNextProcessStageCalibrationMappingId{1};
thread_local std::array<WorkIdentity, 8> gStageWorkStack{};
thread_local std::uint8_t gStageWorkDepth = 0;

const WorkIdentity* currentStageWork() noexcept {
  return gStageWorkDepth ? &gStageWorkStack[gStageWorkDepth - 1u] : nullptr;
}

bool slotsDrained(const BridgeImpl& impl) noexcept {
  bool allFree = true;
  for (std::uint32_t index = 0; index < impl.capacity; ++index) {
    allFree &= impl.slots[index].state.load(std::memory_order_acquire) ==
        SubmissionSlot::Free;
  }
  return retirement::mayRetireStorage(
      impl.open.load(std::memory_order_acquire),
      static_cast<std::uint32_t>(impl.observerGate.load(std::memory_order_acquire) &
                                 kObserverCountMask),
      impl.pendingSamples.load(std::memory_order_acquire), allFree, 0);
}

std::size_t consumeCompletionInbox() noexcept {
  std::size_t consumed = 0;
  for (auto& entry : gCompletionInbox) {
    std::uint8_t expected = CompletionInboxRecord::Ready;
    if (!entry.state.compare_exchange_strong(
            expected, CompletionInboxRecord::Reading,
            std::memory_order_acq_rel, std::memory_order_relaxed)) continue;
    const WorkIdentity work = entry.work;
    const CommandBufferStatus status = entry.status;
    (void)gProcessBridge.publishCompletionHint(work, status);
    entry.work = {};
    entry.status = CommandBufferStatus::Unknown;
    entry.state.store(CompletionInboxRecord::Free, std::memory_order_release);
    ++consumed;
  }
  return consumed;
}

void updateProcessStageCalibration() noexcept {
  if (!gProcessStageActive.load(std::memory_order_acquire)) return;
  const auto deviceHandle =
      gProcessStageDeviceHandle.load(std::memory_order_acquire);
  if (!deviceHandle) return;
  WMT::Device device{static_cast<obj_handle_t>(deviceHandle)};
  ClockAnchor observation{};
  if (!sampleClockAnchor(device, observation)) return;
  if (!gProcessStageFirstAnchorValid) {
    gProcessStageFirstAnchor = observation;
    gProcessStageLatestAnchor = observation;
    gProcessStageFirstAnchorValid = true;
    return;
  }
  if (observation.gpuTimestamp <= gProcessStageLatestAnchor.gpuTimestamp ||
      observation.hostBeforeNs <= gProcessStageLatestAnchor.hostAfterNs) {
    return;
  }
  std::uint64_t mappingId =
      gNextProcessStageCalibrationMappingId.fetch_add(
          1, std::memory_order_relaxed);
  if (mappingId == 0) {
    mappingId = gNextProcessStageCalibrationMappingId.fetch_add(
        1, std::memory_order_relaxed);
  }
  Calibration calibration{};
  if (calibrateFromAnchors(
          mappingId,
          kMetalGpuCounterClockId, kMachAbsoluteTimeClockId,
          gProcessStageFirstAnchor, observation, calibration)) {
    gProcessStageCalibration = calibration;
    gProcessStageLatestAnchor = observation;
  }
}

std::size_t poll(BridgeImpl& impl) noexcept {
  std::size_t resolved = 0;
  for (std::uint32_t index = 0; index < impl.capacity; ++index) {
    auto& slot = impl.slots[index];
    std::uint8_t expected = SubmissionSlot::Pending;
    if (!slot.state.compare_exchange_strong(expected, SubmissionSlot::Polling,
                                             std::memory_order_acq_rel,
                                             std::memory_order_relaxed)) {
      continue;
    }
    WMT::CommandBuffer commandBuffer{slot.commandBuffer.handle};
    const auto hint = slot.completionHint.exchange(0, std::memory_order_acq_rel);
    const auto status = hint == 1u
        ? WMTCommandBufferStatusCompleted
        : hint == 2u
            ? WMTCommandBufferStatusError
            : commandBuffer.status();
    if (status == WMTCommandBufferStatusCompleted) {
      const std::uint64_t start = commandBuffer.gpuStartTime();
      const std::uint64_t end = commandBuffer.gpuEndTime();
      const bool published = impl.collector.complete(slot.ticket, Completion{
          .status = CommandBufferStatus::Completed,
          .gpuStartNs = start,
          .gpuEndNs = end,
      });
      if (published) {
        impl.pendingSamples.fetch_add(1, std::memory_order_relaxed);
      }
      (void)resolveProcessGpuStageSamples(CompletionEvidence{
          .work = slot.work,
          .status = CommandBufferStatus::Completed,
          .observedAfterCompletion = true,
      });
    } else if (status == WMTCommandBufferStatusError) {
      const bool published = impl.collector.complete(slot.ticket, Completion{
          .status = CommandBufferStatus::Failed,
      });
      if (published) {
        impl.pendingSamples.fetch_add(1, std::memory_order_relaxed);
      }
      (void)resolveProcessGpuStageSamples(CompletionEvidence{
          .work = slot.work,
          .status = CommandBufferStatus::Failed,
          .observedAfterCompletion = true,
      });
    } else {
      slot.state.store(SubmissionSlot::Pending, std::memory_order_release);
      continue;
    }
    slot.commandBuffer = nullptr;
    slot.state.store(SubmissionSlot::Free, std::memory_order_release);
    ++resolved;
  }
  return resolved;
}

std::size_t drainReady(BridgeImpl& impl, SampleConsumer consumer,
                       void* context) noexcept {
  if (!consumer) {
    return 0;
  }
  std::size_t count = 0;
  Sample sample;
  while (impl.collector.drainOne(sample)) {
    consumer(context, sample);
    impl.pendingSamples.fetch_sub(1, std::memory_order_relaxed);
    ++count;
  }
  return count;
}

void destroyOrQuarantine(BridgeImpl* impl) noexcept {
  if (!impl) {
    return;
  }
  impl->open.store(false, std::memory_order_release);
  impl->observerGate.fetch_or(kObserverGateClosed, std::memory_order_acq_rel);
  impl->collector.close();
  (void)poll(*impl);
  if (slotsDrained(*impl)) {
    delete impl;
    gLiveBridges.fetch_sub(1, std::memory_order_release);
    return;
  }
  std::lock_guard lock(gQuarantineMutex);
  for (auto& entry : gQuarantine) {
    BridgeImpl* expected = nullptr;
    if (entry.compare_exchange_strong(expected, impl,
                                      std::memory_order_release,
                                      std::memory_order_relaxed)) {
      return;
    }
  }
  // start() bounds all live and quarantined bridge sessions to this registry.
}

}  // namespace

SubmissionBridge::~SubmissionBridge() {
  (void)closeCallGateAndWait(callGate_);
  destroyOrQuarantine(static_cast<BridgeImpl*>(
      impl_.exchange(nullptr, std::memory_order_acq_rel)));
}

SubmissionBridge::SubmissionBridge(SubmissionBridge&& other) noexcept {
  (void)closeCallGateAndWait(other.callGate_);
  impl_.store(other.impl_.exchange(nullptr, std::memory_order_acq_rel),
              std::memory_order_release);
  other.callGate_.store(0, std::memory_order_release);
}

SubmissionBridge& SubmissionBridge::operator=(SubmissionBridge&& other) noexcept {
  if (this != &other) {
    closeCallGateAndWait(callGate_);
    closeCallGateAndWait(other.callGate_);
    destroyOrQuarantine(static_cast<BridgeImpl*>(
        impl_.exchange(nullptr, std::memory_order_acq_rel)));
    impl_.store(other.impl_.exchange(nullptr, std::memory_order_acq_rel),
                std::memory_order_release);
    callGate_.store(0, std::memory_order_release);
    other.callGate_.store(0, std::memory_order_release);
  }
  return *this;
}

bool SubmissionBridge::start(const SessionConfig& config,
                             const Capabilities& capabilities) noexcept {
  if (config.capacity == 0 || config.capacity > kMaxSamples ||
      config.level != Level::CommandBuffer) {
    return false;
  }
  if (!closeCallGateBounded(callGate_)) return false;
  auto reopen = [&]() { callGate_.store(0, std::memory_order_release); };
  if (auto* old = static_cast<BridgeImpl*>(impl_.load(std::memory_order_acquire))) {
    if (old->open.load(std::memory_order_acquire) || !slotsDrained(*old)) {
      reopen();
      return false;
    }
    destroyOrQuarantine(old);
    impl_.store(nullptr, std::memory_order_release);
  }
  if (gLiveBridges.fetch_add(1, std::memory_order_acq_rel) >=
      kMaxBridgeSessions) {
    gLiveBridges.fetch_sub(1, std::memory_order_release);
    reopen();
    return false;
  }
  auto* impl = new (std::nothrow) BridgeImpl();
  if (!impl) {
    gLiveBridges.fetch_sub(1, std::memory_order_release);
    reopen();
    return false;
  }
  const auto started = impl->collector.start(config, capabilities);
  if (started != StartResult::Started) {
    delete impl;
    gLiveBridges.fetch_sub(1, std::memory_order_release);
    reopen();
    return false;
  }
  impl->capacity = config.capacity;
  impl->open.store(true, std::memory_order_release);
  impl_.store(impl, std::memory_order_release);
  reopen();
  return true;
}

std::uint64_t SubmissionBridge::observeSubmission(
    WorkIdentity identity, std::uintptr_t commandBufferHandle) noexcept {
  BridgeCallLease lease(callGate_, impl_);
  auto* impl = lease.get();
  if (!impl || !commandBufferHandle || identity.commandBufferId == 0 ||
      !impl->open.load(std::memory_order_acquire)) {
    return 0;
  }
  if (!acquireObserver(*impl)) return 0;
  if (!impl->open.load(std::memory_order_acquire)) {
    releaseObserver(*impl);
    return 0;
  }
  for (std::uint32_t index = 0; index < impl->capacity; ++index) {
    auto& slot = impl->slots[index];
    std::uint8_t expected = SubmissionSlot::Free;
    if (!slot.state.compare_exchange_strong(expected,
                                             SubmissionSlot::Initializing,
                                             std::memory_order_acq_rel,
                                             std::memory_order_relaxed)) {
      continue;
    }
    const Ticket ticket = impl->collector.reserve(identity);
    if (!ticket) {
      slot.state.store(SubmissionSlot::Free, std::memory_order_release);
      releaseObserver(*impl);
      return 0;
    }
    slot.work = identity;
    slot.ticket = ticket;
    slot.completionHint.store(0, std::memory_order_relaxed);
    slot.commandBuffer = WMT::CommandBuffer(
        static_cast<obj_handle_t>(commandBufferHandle));
    slot.state.store(SubmissionSlot::Pending, std::memory_order_release);
    releaseObserver(*impl);
    return identity.commandBufferId;
  }
  impl->collector.recordLostCapacity();
  releaseObserver(*impl);
  return 0;
}

std::size_t SubmissionBridge::pollCompleted() noexcept {
  BridgeCallLease lease(callGate_, impl_);
  auto* impl = lease.get();
  return impl ? poll(*impl) : 0;
}

bool SubmissionBridge::publishCompletionHint(
    const WorkIdentity& work, CommandBufferStatus status) noexcept {
  BridgeCallLease lease(callGate_, impl_);
  auto* impl = lease.get();
  if (!impl || status == CommandBufferStatus::Unknown ||
      work.commandBufferId == 0 || work.sequenceId == 0) return false;
  for (std::uint32_t index = 0; index < impl->capacity; ++index) {
    auto& slot = impl->slots[index];
    if (slot.state.load(std::memory_order_acquire) != SubmissionSlot::Pending)
      continue;
    if (slot.work.deviceGeneration != work.deviceGeneration ||
        slot.work.queueGeneration != work.queueGeneration ||
        slot.work.commandBufferId != work.commandBufferId ||
        slot.work.sequenceId != work.sequenceId) continue;
    std::uint8_t expected = 0;
    return slot.completionHint.compare_exchange_strong(
        expected, static_cast<std::uint8_t>(status) + 1u,
        std::memory_order_release, std::memory_order_relaxed) ||
        expected == static_cast<std::uint8_t>(status) + 1u;
  }
  return false;
}

std::size_t SubmissionBridge::drain(SampleConsumer consumer,
                                    void* context) noexcept {
  BridgeCallLease lease(callGate_, impl_);
  auto* impl = lease.get();
  if (!impl) {
    return 0;
  }
  return drainReady(*impl, consumer, context);
}

void SubmissionBridge::close() noexcept {
  BridgeCallLease lease(callGate_, impl_);
  if (auto* impl = lease.get()) {
    impl->open.store(false, std::memory_order_release);
    impl->observerGate.fetch_or(kObserverGateClosed, std::memory_order_acq_rel);
    impl->collector.close();
  }
}

bool SubmissionBridge::isDrained() const noexcept {
  BridgeCallLease lease(callGate_, impl_);
  const auto* impl = lease.get();
  return impl && slotsDrained(*impl);
}

Counters SubmissionBridge::counters() const noexcept {
  BridgeCallLease lease(callGate_, impl_);
  const auto* impl = lease.get();
  return impl ? impl->collector.counters() : Counters{};
}

std::size_t SubmissionBridge::pollQuarantined(SampleConsumer consumer,
                                               void* context) noexcept {
  std::size_t count = 0;
  for (auto& entry : gQuarantine) {
    BridgeImpl* impl = nullptr;
    {
      std::lock_guard lock(gQuarantineMutex);
      impl = entry.load(std::memory_order_acquire);
      if (impl) {
        impl->quarantineReaders.fetch_add(1, std::memory_order_acq_rel);
      }
    }
    if (!impl) continue;
    (void)poll(*impl);
    count += drainReady(*impl, consumer, context);
    if (slotsDrained(*impl)) {
      std::lock_guard lock(gQuarantineMutex);
      if (entry.load(std::memory_order_acquire) == impl &&
          impl->quarantineReaders.load(std::memory_order_acquire) == 1) {
        entry.store(nullptr, std::memory_order_release);
        impl->quarantineReaders.fetch_sub(1, std::memory_order_release);
        delete impl;
        gLiveBridges.fetch_sub(1, std::memory_order_release);
        continue;
      }
    }
    impl->quarantineReaders.fetch_sub(1, std::memory_order_release);
  }
  return count;
}

StageWorkScope::StageWorkScope(WorkIdentity work) noexcept
    : previousDepth_(gStageWorkDepth) {
  if (gStageWorkDepth >= gStageWorkStack.size()) return;
  gStageWorkStack[gStageWorkDepth++] = work;
  pushed_ = true;
}

StageWorkScope::StageWorkScope(StageWorkScope&& other) noexcept
    : previousDepth_(other.previousDepth_), pushed_(other.pushed_) {
  other.pushed_ = false;
}

void StageWorkScope::restore(std::uint8_t previousDepth) noexcept {
  gStageWorkDepth = previousDepth;
}

std::uint64_t registerProcessGpuDevice(std::uintptr_t deviceHandle) noexcept {
  if (!deviceHandle) return 0;
  if (gProcessDeviceHandle.load(std::memory_order_acquire) != deviceHandle) {
    std::uint64_t generation = gNextProcessDeviceGeneration.fetch_add(
        1, std::memory_order_relaxed);
    if (generation == 0) {
      generation = gNextProcessDeviceGeneration.fetch_add(
          1, std::memory_order_relaxed);
    }
    gProcessDeviceGeneration.store(generation, std::memory_order_relaxed);
    gProcessDeviceHandle.store(deviceHandle, std::memory_order_release);
  }
  return gProcessDeviceGeneration.load(std::memory_order_acquire);
}

std::uint64_t processGpuDeviceGeneration(
    std::uintptr_t deviceHandle) noexcept {
  if (!deviceHandle) return 0;
  if (deviceHandle != gProcessDeviceHandle.load(std::memory_order_acquire)) {
    return 0;
  }
  return gProcessDeviceGeneration.load(std::memory_order_acquire);
}

std::uint64_t processCommandBufferDeviceGeneration(
    std::uintptr_t commandBufferHandle) noexcept {
  if (!commandBufferHandle) return 0;
  id<MTLCommandBuffer> commandBuffer =
      (id<MTLCommandBuffer>)static_cast<obj_handle_t>(commandBufferHandle);
  return processGpuDeviceGeneration(
      reinterpret_cast<std::uintptr_t>(commandBuffer.device));
}

std::uint64_t findProcessCommandBufferId(
    std::uintptr_t commandBufferHandle,
    std::uintptr_t queueHandle) noexcept {
  if (!commandBufferHandle || !queueHandle) return 0;
  std::uint64_t hash = static_cast<std::uint64_t>(commandBufferHandle);
  hash ^= static_cast<std::uint64_t>(queueHandle) + 0x9e3779b97f4a7c15ull +
      (hash << 6u) + (hash >> 2u);
  const std::size_t first = static_cast<std::size_t>(
      hash % gProcessCommandBuffers.size());
  for (std::size_t offset = 0; offset < gProcessCommandBuffers.size(); ++offset) {
    auto& record = gProcessCommandBuffers[
        (first + offset) % gProcessCommandBuffers.size()];
    if (pinIdentityRecord(record)) {
      const bool matches = record.handle == commandBufferHandle &&
          record.queueHandle == queueHandle;
      const auto id = matches ? record.id : 0;
      unpinIdentityRecord(record);
      if (matches) return id;
      continue;
    }
    std::uint32_t state = record.state.load(std::memory_order_acquire);
    if (state == kIdentityInitializing) {
      return 0;
    }
    if (state != 0) continue;
    break;
  }
  return 0;
}

std::uint64_t processCommandBufferId(std::uintptr_t commandBufferHandle,
                                    std::uintptr_t queueHandle,
                                    std::uint64_t sequenceId) noexcept {
  if (!commandBufferHandle || !queueHandle) return 0;
  if (const auto found = findProcessCommandBufferId(commandBufferHandle,
                                                     queueHandle)) {
    return found;
  }
  std::uint64_t hash = static_cast<std::uint64_t>(commandBufferHandle);
  hash ^= static_cast<std::uint64_t>(queueHandle) + 0x9e3779b97f4a7c15ull +
      (hash << 6u) + (hash >> 2u);
  const std::size_t first = static_cast<std::size_t>(
      hash % gProcessCommandBuffers.size());
  for (std::size_t offset = 0; offset < gProcessCommandBuffers.size(); ++offset) {
    auto& record = gProcessCommandBuffers[
        (first + offset) % gProcessCommandBuffers.size()];
    if (pinIdentityRecord(record)) {
      const bool matches = record.handle == commandBufferHandle &&
          record.queueHandle == queueHandle;
      const auto id = matches ? record.id : 0;
      unpinIdentityRecord(record);
      if (matches) return id;
      continue;
    }
    const auto state = record.state.load(std::memory_order_acquire);
    if (state == kIdentityInitializing) {
      gIdentityRegistryLosses.fetch_add(1, std::memory_order_relaxed);
      return 0;
    }
    if (state != 0) continue;
    std::uint32_t expected = 0;
    if (!record.state.compare_exchange_strong(
            expected, kIdentityInitializing, std::memory_order_acq_rel,
            std::memory_order_relaxed)) {
      continue;
    }
    std::uint64_t id = gNextCommandBufferId.fetch_add(
        1, std::memory_order_relaxed);
    if (id == 0) id = gNextCommandBufferId.fetch_add(
        1, std::memory_order_relaxed);
    record.handle = commandBufferHandle;
    record.queueHandle = queueHandle;
    record.sequenceId = sequenceId;
    record.id = id;
    record.state.store(kIdentityLive, std::memory_order_release);
    return id;
  }
  gIdentityRegistryLosses.fetch_add(1, std::memory_order_relaxed);
  return 0;
}

void retireProcessCommandBufferId(std::uint64_t commandBufferId) noexcept {
  if (!commandBufferId) return;
  for (auto& record : gProcessCommandBuffers) {
    if (!pinIdentityRecord(record)) continue;
    const auto recordId = record.id;
    if (recordId != commandBufferId) {
      unpinIdentityRecord(record);
      continue;
    }
    auto state = record.state.load(std::memory_order_acquire);
    while (state & kIdentityLive) {
      const auto readers = state & kIdentityReaderMask;
      const auto next = kIdentityRetiring | (readers - 1u);
      if (record.state.compare_exchange_weak(
              state, next, std::memory_order_acq_rel,
              std::memory_order_acquire)) {
        if ((readers - 1u) == 0) {
          record.state.store(0, std::memory_order_release);
        }
        return;
      }
    }
    unpinIdentityRecord(record);
    return;
  }
}

void retireProcessCommandBufferIdsThrough(
    std::uintptr_t queueIdentity,
    std::uint64_t completedSequence) noexcept {
  if (!queueIdentity || completedSequence == 0) return;
  for (auto& record : gProcessCommandBuffers) {
    if (!pinIdentityRecord(record)) continue;
    const auto id = record.queueHandle == queueIdentity &&
            record.sequenceId <= completedSequence
        ? record.id
        : 0;
    unpinIdentityRecord(record);
    if (id) retireProcessCommandBufferId(id);
  }
}

bool setProcessCommandBufferDeviceGeneration(
    std::uintptr_t commandBufferHandle, std::uintptr_t queueHandle,
    std::uint64_t sequenceId, std::uint64_t deviceGeneration) noexcept {
  if (!commandBufferHandle || !queueHandle || !sequenceId || !deviceGeneration)
    return false;
  std::uint64_t hash = static_cast<std::uint64_t>(commandBufferHandle);
  hash ^= static_cast<std::uint64_t>(queueHandle) + 0x9e3779b97f4a7c15ull +
      (hash << 6u) + (hash >> 2u);
  const std::size_t first = static_cast<std::size_t>(
      hash % gProcessCommandBuffers.size());
  for (std::size_t offset = 0; offset < gProcessCommandBuffers.size(); ++offset) {
    auto& record = gProcessCommandBuffers[
        (first + offset) % gProcessCommandBuffers.size()];
    if (!pinIdentityRecord(record)) {
      const auto state = record.state.load(std::memory_order_acquire);
      if (state == 0 || state == kIdentityInitializing) return false;
      continue;
    }
    const bool matches = record.handle == commandBufferHandle &&
        record.queueHandle == queueHandle && record.sequenceId == sequenceId;
    if (matches) record.deviceGeneration.store(deviceGeneration,
                                                std::memory_order_release);
    unpinIdentityRecord(record);
    if (matches) return true;
  }
  return false;
}

bool publishCompletedProcessGpuCommandBuffer(
    std::uintptr_t commandBufferHandle, std::uintptr_t queueIdentity,
    std::uint64_t sequenceId, CommandBufferStatus status) noexcept {
  if (!commandBufferHandle || !queueIdentity || !sequenceId ||
      status == CommandBufferStatus::Unknown) return false;
  std::uint64_t hash = static_cast<std::uint64_t>(commandBufferHandle);
  hash ^= static_cast<std::uint64_t>(queueIdentity) + 0x9e3779b97f4a7c15ull +
      (hash << 6u) + (hash >> 2u);
  const std::size_t first = static_cast<std::size_t>(
      hash % gProcessCommandBuffers.size());
  WorkIdentity work{};
  bool found = false;
  for (std::size_t offset = 0; offset < gProcessCommandBuffers.size(); ++offset) {
    auto& record = gProcessCommandBuffers[
        (first + offset) % gProcessCommandBuffers.size()];
    if (!pinIdentityRecord(record)) {
      const auto state = record.state.load(std::memory_order_acquire);
      if (state == 0 || state == kIdentityInitializing) break;
      continue;
    }
    const bool matches = record.handle == commandBufferHandle &&
        record.queueHandle == queueIdentity && record.sequenceId == sequenceId;
    if (matches) {
      work = WorkIdentity{
          .deviceGeneration = record.deviceGeneration.load(std::memory_order_acquire),
          .queueGeneration = static_cast<std::uint64_t>(queueIdentity),
          .commandBufferId = record.id,
          .sequenceId = record.sequenceId,
      };
      found = work.deviceGeneration != 0;
    }
    unpinIdentityRecord(record);
    if (matches) break;
  }
  if (!found) {
    gCompletionInboxLosses.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  for (auto& entry : gCompletionInbox) {
    std::uint8_t expected = CompletionInboxRecord::Free;
    if (!entry.state.compare_exchange_strong(
            expected, CompletionInboxRecord::Initializing,
            std::memory_order_acq_rel, std::memory_order_relaxed)) continue;
    entry.work = work;
    entry.status = status;
    entry.state.store(CompletionInboxRecord::Ready, std::memory_order_release);
    return true;
  }
  gCompletionInboxLosses.fetch_add(1, std::memory_order_relaxed);
  return false;
}

StagePairReservation reserveCurrentStageAttachment(
    std::uintptr_t deviceHandle,
    std::uintptr_t commandBufferHandle) noexcept {
  const WorkIdentity* current = currentStageWork();
  if (!current || !deviceHandle || !commandBufferHandle ||
      current->deviceGeneration == 0 ||
      current->sequenceId == 0) {
    return {};
  }
  if (deviceHandle != gProcessDeviceHandle.load(std::memory_order_acquire) ||
      current->deviceGeneration !=
          gProcessDeviceGeneration.load(std::memory_order_acquire)) {
    return {};
  }
  WorkIdentity work = *current;
  work.commandBufferId = processCommandBufferId(
      commandBufferHandle, static_cast<std::uintptr_t>(work.queueGeneration),
      work.sequenceId);
  if (work.commandBufferId == 0) return {};
  const auto reservation = gProcessStageBuffer.reserve(work);
  if (!reservation || reservation.ticket.pairIndex >= gProcessStageRecords.size()) {
    return {};
  }
  auto& record = gProcessStageRecords[reservation.ticket.pairIndex];
  std::uint8_t expected = ProcessStageRecord::Free;
  if (!record.state.compare_exchange_strong(expected,
                                             ProcessStageRecord::Initializing,
                                             std::memory_order_acq_rel,
                                             std::memory_order_relaxed)) {
    return {};
  }
  record.ticket = reservation.ticket;
  record.work = work;
  record.sample = StageSample{
      .sessionGeneration = reservation.ticket.sessionGeneration,
      .slotGeneration = reservation.ticket.pairGeneration,
      .pairIndex = reservation.ticket.pairIndex,
      .work = work,
      .sourceClockId = kMetalGpuCounterClockId,
  };
  record.state.store(ProcessStageRecord::Reserved, std::memory_order_release);
  return reservation;
}

std::size_t resolveProcessGpuStageSamples(
    const CompletionEvidence& evidence) noexcept {
  if (!evidence.observedAfterCompletion) return 0;
  std::size_t resolved = 0;
  for (auto& record : gProcessStageRecords) {
    if (record.state.load(std::memory_order_acquire) !=
            ProcessStageRecord::Reserved) {
      continue;
    }
    std::uint8_t expected = ProcessStageRecord::Reserved;
    if (!record.state.compare_exchange_strong(expected,
                                               ProcessStageRecord::Resolving,
                                               std::memory_order_acq_rel,
                                               std::memory_order_relaxed)) {
      continue;
    }
    if (record.work.deviceGeneration != evidence.work.deviceGeneration ||
        record.work.queueGeneration != evidence.work.queueGeneration ||
        record.work.commandBufferId != evidence.work.commandBufferId) {
      record.state.store(ProcessStageRecord::Reserved,
                         std::memory_order_release);
      continue;
    }
    StagePairResult result{};
    CompletionEvidence stageEvidence = evidence;
    stageEvidence.work = record.work;
    if (!gProcessStageBuffer.resolve(record.ticket, stageEvidence, result)) {
      record.state.store(ProcessStageRecord::Free, std::memory_order_release);
      continue;
    }
    record.sample.result = result;
    record.sample.calibration = gProcessStageCalibration;
    record.sample.calibrationApplies =
        record.sample.calibration.valid &&
        result.validity == ResultValidity::Valid &&
        result.startTimestamp >= record.sample.calibration.validBegin &&
        result.endTimestamp <= record.sample.calibration.validEnd;
    record.state.store(ProcessStageRecord::Ready, std::memory_order_release);
    ++resolved;
  }
  return resolved;
}

std::size_t drainProcessGpuStageSamples(StageSampleConsumer consumer,
                                        void* context) noexcept {
  if (!consumer) return 0;
  std::size_t count = 0;
  for (auto& record : gProcessStageRecords) {
    std::uint8_t expected = ProcessStageRecord::Ready;
    if (!record.state.compare_exchange_strong(expected,
                                               ProcessStageRecord::Resolving,
                                               std::memory_order_acq_rel,
                                               std::memory_order_relaxed)) {
      continue;
    }
    consumer(context, record.sample);
    record.ticket = {};
    record.work = {};
    record.sample = {};
    record.state.store(ProcessStageRecord::Free, std::memory_order_release);
    ++count;
  }
  return count;
}

void closeProcessGpuStageSamples() noexcept {
  gProcessStageBuffer.close();
}

bool processGpuStageSamplesDrained() noexcept {
  if (!gProcessStageActive.load(std::memory_order_acquire)) return true;
  for (const auto& record : gProcessStageRecords) {
    if (record.state.load(std::memory_order_acquire) !=
        ProcessStageRecord::Free) return false;
  }
  return gProcessStageBuffer.canRetire();
}

StageCounters processGpuStageCounters() noexcept {
  return gProcessStageBuffer.counters();
}

bool initializeProcessGpuTrace(WMT::Device& device,
                               const SessionConfig& config) noexcept {
  const auto deviceGeneration = registerProcessGpuDevice(
      static_cast<std::uintptr_t>(device.handle));
  if (!deviceGeneration) return false;
  auto commandConfig = config;
  commandConfig.level = Level::CommandBuffer;
  if (!gProcessBridge.start(commandConfig, queryCapabilities(device))) {
    return false;
  }
  gIdentityRegistryLosses.store(0, std::memory_order_relaxed);
  gProcessStageActive.store(false, std::memory_order_release);
  gProcessStageDeviceHandle.store(0, std::memory_order_release);
  if (config.level == Level::EncoderStage) {
    if (!gProcessStageBuffer.start(device, config.sessionGeneration,
                                   config.capacity)) {
      gProcessBridge.close();
      return false;
    }
    gProcessStageCalibration = {};
    gProcessStageFirstAnchor = {};
    gProcessStageLatestAnchor = {};
    gProcessStageFirstAnchorValid = false;
    gProcessStageDeviceHandle.store(
        static_cast<std::uintptr_t>(device.handle), std::memory_order_release);
    gProcessStageFirstAnchorValid =
        sampleClockAnchor(device, gProcessStageFirstAnchor);
    if (gProcessStageFirstAnchorValid) {
      gProcessStageLatestAnchor = gProcessStageFirstAnchor;
    } else {
      gProcessStageFirstAnchor = {};
    }
    gProcessStageActive.store(true, std::memory_order_release);
  }
  gProcessGpuEnabled.store(true, std::memory_order_release);
  return true;
}

bool initializeProcessGpuTraceHandle(
    std::uintptr_t deviceHandle, const SessionConfig& config) noexcept {
  if (!deviceHandle) return false;
  WMT::Device device{static_cast<obj_handle_t>(deviceHandle)};
  return initializeProcessGpuTrace(device, config);
}

bool processGpuTraceEnabled() noexcept {
  return gProcessGpuEnabled.load(std::memory_order_acquire);
}

std::uint64_t observeProcessGpuSubmission(
    WorkIdentity identity, std::uintptr_t commandBufferHandle) noexcept {
  return gProcessBridge.observeSubmission(identity, commandBufferHandle);
}

std::size_t pollProcessGpuTrace() noexcept {
  if (gProcessStageActive.load(std::memory_order_acquire)) {
    if (gProcessStageCalibrationControl.test_and_set(
            std::memory_order_acquire)) return 0;
    updateProcessStageCalibration();
    (void)consumeCompletionInbox();
    const auto completed = gProcessBridge.pollCompleted();
    gProcessStageCalibrationControl.clear(std::memory_order_release);
    return completed;
  }
  (void)consumeCompletionInbox();
  return gProcessBridge.pollCompleted();
}

std::uint64_t processGpuCompletionLosses() noexcept {
  return gCompletionInboxLosses.load(std::memory_order_relaxed);
}

std::size_t drainProcessGpuTrace(SampleConsumer consumer,
                                 void* context) noexcept {
  return gProcessBridge.drain(consumer, context);
}

void closeProcessGpuTrace() noexcept {
  gProcessGpuEnabled.store(false, std::memory_order_release);
  gProcessBridge.close();
}

bool processGpuTraceDrained() noexcept {
  for (const auto& entry : gCompletionInbox) {
    if (entry.state.load(std::memory_order_acquire) !=
        CompletionInboxRecord::Free) return false;
  }
  return gProcessBridge.isDrained();
}

Counters processGpuTraceCounters() noexcept {
  auto result = gProcessBridge.counters();
  result.lostCapacity +=
      gIdentityRegistryLosses.load(std::memory_order_relaxed);
  result.lostCapacity +=
      gCompletionInboxLosses.load(std::memory_order_relaxed);
  return result;
}

}  // namespace dxmt9::trace::gpu
