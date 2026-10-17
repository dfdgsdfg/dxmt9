#include "dxmt9_trace_gpu.hpp"

#include "../winemetal/Metal.hpp"

#import <Metal/Metal.h>

#include <array>
#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <utility>

#import <mach/mach_time.h>

namespace dxmt9::trace::gpu {

namespace {

struct StageSlot {
  enum State : std::uint8_t { Free, Reserving, Reserved, Resolving, Resolved };
  std::atomic<std::uint8_t> state{Free};
  std::uint64_t generation = 1;
  WorkIdentity work{};
  StagePairResult result{};
};

struct StageImpl {
  WMT::Reference<WMT::CounterSampleBuffer> buffer{};
  std::array<StageSlot, kMaxStagePairs> slots{};
  std::atomic<std::uint32_t> cursor{0};
  std::atomic<std::uint64_t> admissionGate{0};
  std::atomic<bool> open{false};
  std::uint64_t sessionGeneration = 0;
  std::uint32_t capacity = 0;
  std::atomic<std::uint64_t> selected{0};
  std::atomic<std::uint64_t> resolved{0};
  std::atomic<std::uint64_t> lostCapacity{0};
  std::atomic<std::uint64_t> invalid{0};
  std::atomic<std::uint64_t> stale{0};
};

constexpr std::size_t kQuarantineCapacity = 32;
constexpr std::uint64_t kAdmissionClosed = std::uint64_t{1} << 63u;
constexpr std::uint64_t kAdmissionCountMask = ~kAdmissionClosed;
constexpr std::uint64_t kCallGateClosed = std::uint64_t{1} << 63u;
constexpr std::uint64_t kCallGateCountMask = ~kCallGateClosed;

class StageCallLease {
public:
  StageCallLease(std::atomic<std::uint64_t>& gate,
                 std::atomic<void*>& impl) noexcept : gate_(gate) {
    auto value = gate_.load(std::memory_order_acquire);
    while (!(value & kCallGateClosed)) {
      if (gate_.compare_exchange_weak(value, value + 1u,
                                     std::memory_order_acq_rel,
                                     std::memory_order_acquire)) {
        impl_ = static_cast<StageImpl*>(impl.load(std::memory_order_acquire));
        acquired_ = true;
        return;
      }
    }
  }
  ~StageCallLease() {
    if (acquired_) gate_.fetch_sub(1u, std::memory_order_release);
  }
  StageImpl* get() const noexcept { return impl_; }
  explicit operator bool() const noexcept { return acquired_; }

private:
  std::atomic<std::uint64_t>& gate_;
  StageImpl* impl_ = nullptr;
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
std::array<std::atomic<StageImpl*>, kQuarantineCapacity> gQuarantine{};
std::mutex gQuarantineMutex;
std::atomic<std::uint32_t> gStageImplCount{0};

bool canRetire(const StageImpl& impl) noexcept {
  if ((impl.admissionGate.load(std::memory_order_acquire) &
       kAdmissionCountMask) != 0) {
    return false;
  }
  const std::uint32_t count = std::min(
      impl.cursor.load(std::memory_order_acquire), impl.capacity);
  for (std::uint32_t index = 0; index < count; ++index) {
    const auto state = impl.slots[index].state.load(std::memory_order_acquire);
    if (state == StageSlot::Reserved || state == StageSlot::Reserving ||
        state == StageSlot::Resolving) {
      return false;
    }
  }
  return true;
}

void destroyOrQuarantine(StageImpl* impl) noexcept {
  if (!impl) {
    return;
  }
  impl->open.store(false, std::memory_order_release);
  impl->admissionGate.fetch_or(kAdmissionClosed, std::memory_order_acq_rel);
  if (canRetire(*impl)) {
    delete impl;
    gStageImplCount.fetch_sub(1, std::memory_order_relaxed);
    return;
  }
  std::lock_guard lock(gQuarantineMutex);
  for (auto& slot : gQuarantine) {
    StageImpl* expected = nullptr;
    if (slot.compare_exchange_strong(expected, impl,
                                     std::memory_order_release,
                                     std::memory_order_relaxed)) {
      return;
    }
  }
  // The global live-buffer limit equals the quarantine capacity, so this is
  // unreachable unless the registry is corrupted. Preserve the Metal handle.
}

bool acquireAdmission(StageImpl& impl) noexcept {
  auto gate = impl.admissionGate.load(std::memory_order_acquire);
  for (;;) {
    if (gate & kAdmissionClosed) return false;
    if ((gate & kAdmissionCountMask) == kAdmissionCountMask) return false;
    if (impl.admissionGate.compare_exchange_weak(
            gate, gate + 1u, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
      return true;
    }
  }
}

void releaseAdmission(StageImpl& impl) noexcept {
  impl.admissionGate.fetch_sub(1u, std::memory_order_release);
}

bool resolveStagePair(StageImpl& impl, StagePairTicket ticket,
                      const CompletionEvidence& evidence,
                      StagePairResult& result) noexcept {
  if (!ticket || ticket.sessionGeneration != impl.sessionGeneration ||
      ticket.pairIndex >= impl.capacity) {
    impl.stale.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  auto& slot = impl.slots[ticket.pairIndex];
  if (slot.generation != ticket.pairGeneration ||
      slot.work.deviceGeneration != evidence.work.deviceGeneration ||
      slot.work.queueGeneration != evidence.work.queueGeneration ||
      slot.work.commandBufferId != evidence.work.commandBufferId ||
      slot.work.sequenceId != evidence.work.sequenceId ||
      !evidence.observedAfterCompletion) {
    impl.stale.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  std::uint8_t expected = StageSlot::Reserved;
  if (!slot.state.compare_exchange_strong(expected, StageSlot::Resolving,
                                           std::memory_order_acq_rel,
                                           std::memory_order_relaxed)) {
    impl.stale.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  if (evidence.status != CommandBufferStatus::Completed) {
    slot.result.validity = ResultValidity::CommandBufferFailed;
  } else {
    std::uint64_t timestamps[2] = {};
    const std::uint32_t firstSample = ticket.pairIndex * 2u;
    impl.buffer.resolveCounterRange(firstSample, 2, timestamps,
                                     sizeof(timestamps));
    slot.result.startTimestamp = timestamps[0];
    slot.result.endTimestamp = timestamps[1];
    if (timestamps[0] == 0 || timestamps[1] == 0) {
      slot.result.validity = ResultValidity::MissingTimestamp;
    } else if (timestamps[1] <= timestamps[0]) {
      slot.result.validity = ResultValidity::NonMonotonicTimestamp;
    } else {
      slot.result.validity = ResultValidity::Valid;
    }
  }
  result = slot.result;
  impl.resolved.fetch_add(1, std::memory_order_relaxed);
  if (slot.result.validity != ResultValidity::Valid) {
    impl.invalid.fetch_add(1, std::memory_order_relaxed);
  }
  slot.state.store(StageSlot::Resolved, std::memory_order_release);
  return true;
}

std::uint64_t machToNs(std::uint64_t ticks) noexcept {
  mach_timebase_info_data_t timebase{};
  if (mach_timebase_info(&timebase) != KERN_SUCCESS || timebase.denom == 0) {
    return 0;
  }
  const long double ns = static_cast<long double>(ticks) * timebase.numer /
      timebase.denom;
  if (ns <= 0.0L || ns > static_cast<long double>(UINT64_MAX)) {
    return 0;
  }
  return static_cast<std::uint64_t>(ns);
}

}  // namespace

StageSampleBuffer::~StageSampleBuffer() {
  (void)closeCallGateAndWait(callGate_);
  destroyOrQuarantine(static_cast<StageImpl*>(
      impl_.exchange(nullptr, std::memory_order_acq_rel)));
}

StageSampleBuffer::StageSampleBuffer(StageSampleBuffer&& other) noexcept {
  (void)closeCallGateAndWait(other.callGate_);
  impl_.store(other.impl_.exchange(nullptr, std::memory_order_acq_rel),
              std::memory_order_release);
  other.callGate_.store(0, std::memory_order_release);
}

StageSampleBuffer& StageSampleBuffer::operator=(StageSampleBuffer&& other) noexcept {
  if (this != &other) {
    closeCallGateAndWait(callGate_);
    closeCallGateAndWait(other.callGate_);
    destroyOrQuarantine(static_cast<StageImpl*>(
        impl_.exchange(nullptr, std::memory_order_acq_rel)));
    impl_.store(other.impl_.exchange(nullptr, std::memory_order_acq_rel),
                std::memory_order_release);
    callGate_.store(0, std::memory_order_release);
    other.callGate_.store(0, std::memory_order_release);
  }
  return *this;
}

bool StageSampleBuffer::start(WMT::Device& device,
                              std::uint64_t sessionGeneration,
                              std::uint32_t pairCapacity) noexcept {
  if (sessionGeneration == 0 || pairCapacity == 0 ||
      pairCapacity > kMaxStagePairs ||
      !device.supportsCounterSampling(WMTCounterSamplingPointAtStageBoundary)) {
    return false;
  }
  if (!closeCallGateBounded(callGate_)) return false;
  auto reopen = [&]() { callGate_.store(0, std::memory_order_release); };
  auto* current = static_cast<StageImpl*>(impl_.load(std::memory_order_acquire));
  if (current) {
    if (current->open.load(std::memory_order_acquire) ||
        (current->admissionGate.load(std::memory_order_acquire) &
         kAdmissionCountMask) != 0) {
      reopen();
      return false;
    }
    for (const auto& slot : current->slots) {
      if (slot.state.load(std::memory_order_acquire) == StageSlot::Reserved ||
          slot.state.load(std::memory_order_acquire) == StageSlot::Resolving) {
        reopen();
        return false;
      }
    }
  }
  if (gStageImplCount.fetch_add(1, std::memory_order_acq_rel) >=
      kQuarantineCapacity) {
    gStageImplCount.fetch_sub(1, std::memory_order_release);
    reopen();
    return false;
  }
  std::unique_ptr<StageImpl> next(new (std::nothrow) StageImpl());
  if (!next) {
    gStageImplCount.fetch_sub(1, std::memory_order_release);
    reopen();
    return false;
  }
  next->buffer = device.newCounterSampleBuffer(pairCapacity * 2u, true);
  if (!next->buffer) {
    gStageImplCount.fetch_sub(1, std::memory_order_release);
    reopen();
    return false;
  }
  next->sessionGeneration = sessionGeneration;
  next->capacity = pairCapacity;
  next->open.store(true, std::memory_order_release);
  destroyOrQuarantine(current);
  impl_.store(next.release(), std::memory_order_release);
  reopen();
  return true;
}

StagePairReservation StageSampleBuffer::reserve(
    const WorkIdentity& work) noexcept {
  StageCallLease lease(callGate_, impl_);
  auto* impl = lease.get();
  if (!impl || !acquireAdmission(*impl)) {
    return {};
  }
  if (!impl->open.load(std::memory_order_acquire)) {
    releaseAdmission(*impl);
    return {};
  }
  const std::uint32_t index = impl->cursor.fetch_add(1, std::memory_order_relaxed);
  if (index >= impl->capacity ||
      !impl->open.load(std::memory_order_acquire)) {
    if (index >= impl->capacity) {
      impl->lostCapacity.fetch_add(1, std::memory_order_relaxed);
    }
    releaseAdmission(*impl);
    return {};
  }
  auto& slot = impl->slots[index];
  std::uint8_t expected = StageSlot::Free;
  if (!slot.state.compare_exchange_strong(expected, StageSlot::Reserving,
                                          std::memory_order_acq_rel,
                                          std::memory_order_relaxed)) {
    impl->lostCapacity.fetch_add(1, std::memory_order_relaxed);
    releaseAdmission(*impl);
    return {};
  }
  slot.work = work;
  slot.state.store(StageSlot::Reserved, std::memory_order_release);
  if (!impl->open.load(std::memory_order_acquire)) {
    slot.state.store(StageSlot::Free, std::memory_order_release);
    impl->lostCapacity.fetch_add(1, std::memory_order_relaxed);
    releaseAdmission(*impl);
    return {};
  }
  const std::uint32_t firstSample = index * 2u;
  StagePairReservation reservation{
      .ticket = StagePairTicket{impl->sessionGeneration, index, slot.generation},
      .attachment = WMTSampleBufferAttachmentInfo{
          .sample_buffer = impl->buffer.handle,
          .start_of_encoder_sample_index = firstSample,
          .end_of_encoder_sample_index = firstSample + 1u,
      },
  };
  impl->selected.fetch_add(1, std::memory_order_relaxed);
  releaseAdmission(*impl);
  return reservation;
}

bool StageSampleBuffer::resolve(StagePairTicket ticket,
                                const CompletionEvidence& evidence,
                                StagePairResult& result) noexcept {
  StageCallLease lease(callGate_, impl_);
  auto* impl = lease.get();
  return impl && resolveStagePair(*impl, ticket, evidence, result);
}

bool StageSampleBuffer::resolveQuarantined(
    StagePairTicket ticket, const CompletionEvidence& evidence,
    StagePairResult& result) noexcept {
  std::lock_guard lock(gQuarantineMutex);
  for (auto& entry : gQuarantine) {
    auto* impl = entry.load(std::memory_order_acquire);
    if (!impl || impl->sessionGeneration != ticket.sessionGeneration ||
        !resolveStagePair(*impl, ticket, evidence, result)) {
      continue;
    }
    if (!impl->open.load(std::memory_order_acquire) &&
        dxmt9::trace::gpu::canRetire(*impl)) {
      StageImpl* expected = impl;
      if (entry.compare_exchange_strong(expected, nullptr,
                                        std::memory_order_acq_rel,
                                        std::memory_order_relaxed)) {
        delete impl;
        gStageImplCount.fetch_sub(1, std::memory_order_release);
      }
    }
    return true;
  }
  return false;
}

void StageSampleBuffer::close() noexcept {
  StageCallLease lease(callGate_, impl_);
  if (auto* impl = lease.get()) {
    impl->open.store(false, std::memory_order_release);
    impl->admissionGate.fetch_or(kAdmissionClosed, std::memory_order_acq_rel);
  }
}

bool StageSampleBuffer::isOpen() const noexcept {
  StageCallLease lease(callGate_, impl_);
  const auto* impl = lease.get();
  return impl && impl->open.load(std::memory_order_acquire);
}

bool StageSampleBuffer::canRetire() const noexcept {
  StageCallLease lease(callGate_, impl_);
  const auto* impl = lease.get();
  return impl && !impl->open.load(std::memory_order_acquire) &&
      dxmt9::trace::gpu::canRetire(*impl);
}

StageCounters StageSampleBuffer::counters() const noexcept {
  StageCallLease lease(callGate_, impl_);
  const auto* impl = lease.get();
  return impl ? StageCounters{
      .selected = impl->selected.load(std::memory_order_relaxed),
      .resolved = impl->resolved.load(std::memory_order_relaxed),
      .lostCapacity = impl->lostCapacity.load(std::memory_order_relaxed),
      .invalid = impl->invalid.load(std::memory_order_relaxed),
      .stale = impl->stale.load(std::memory_order_relaxed),
  } : StageCounters{};
}

bool sampleClockAnchor(WMT::Device& device, ClockAnchor& anchor) noexcept {
  id<MTLDevice> metalDevice = (id<MTLDevice>)device.handle;
  if (![metalDevice respondsToSelector:@selector(sampleTimestamps:gpuTimestamp:)]) {
    return false;
  }
  const auto before = mach_absolute_time();
  MTLTimestamp cpuTimestamp = 0;
  MTLTimestamp gpuTimestamp = 0;
  [metalDevice sampleTimestamps:&cpuTimestamp gpuTimestamp:&gpuTimestamp];
  const auto after = mach_absolute_time();
  anchor = ClockAnchor{
      .hostBeforeNs = machToNs(before),
      .cpuTimestamp = cpuTimestamp,
      .gpuTimestamp = gpuTimestamp,
      .hostAfterNs = machToNs(after),
  };
  return anchor.hostBeforeNs != 0 && anchor.cpuTimestamp != 0 &&
      anchor.gpuTimestamp != 0 && anchor.hostAfterNs >= anchor.hostBeforeNs;
}

Capabilities queryCapabilities(WMT::Device& device) noexcept {
  const bool stageSamplingSupported =
      device.supportsCounterSampling(WMTCounterSamplingPointAtStageBoundary);
  bool hasTimestampBuffer = false;
  if (stageSamplingSupported) {
    auto probe = device.newCounterSampleBuffer(2, true);
    hasTimestampBuffer = static_cast<bool>(probe);
  }
  return Capabilities{
      .commandBufferTime = Support::Supported,
      .stageBoundary = hasTimestampBuffer
          ? Support::Supported
          : Support::Unsupported,
      .drawBoundary = device.supportsCounterSampling(
                          WMTCounterSamplingPointAtDrawBoundary)
          ? Support::Supported
          : Support::Unsupported,
      .blitBoundary = device.supportsCounterSampling(
                          WMTCounterSamplingPointAtBlitBoundary)
          ? Support::Supported
          : Support::Unsupported,
      .dispatchBoundary = device.supportsCounterSampling(
                              WMTCounterSamplingPointAtDispatchBoundary)
          ? Support::Supported
          : Support::Unsupported,
      .tileDispatchBoundary = device.supportsCounterSampling(
                                  WMTCounterSamplingPointAtTileDispatchBoundary)
          ? Support::Supported
          : Support::Unsupported,
  };
}

}  // namespace dxmt9::trace::gpu
