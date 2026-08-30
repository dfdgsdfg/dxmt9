#include "dxmt9_scheduling_progress_watchdog.hpp"

#include "dxmt9_perf_counters.hpp"
#include "../util/log/log.hpp"

#include <algorithm>
#include <cstdlib>
#include <limits>

namespace dxmt9 {
namespace {

std::uint64_t watchdogThresholdMs() noexcept {
  const char* env = std::getenv("DXMT9_SCHEDULING_PROGRESS_WATCHDOG_MS");
  if (!env || env[0] == '\0' || env[0] == '0') {
    return 0;
  }
  char* end = nullptr;
  const auto parsed = std::strtoull(env, &end, 10);
  return end != env && *end == '\0' ? parsed : 0;
}

std::uint64_t thresholdToNs(std::uint64_t thresholdMs) noexcept {
  return thresholdMs > std::numeric_limits<std::uint64_t>::max() / 1000000ull
      ? std::numeric_limits<std::uint64_t>::max()
      : thresholdMs * 1000000ull;
}

const char* phaseName(SchedulingProgressPhase phase) noexcept {
  switch (phase) {
  case SchedulingProgressPhase::Admission: return "admission";
  case SchedulingProgressPhase::LeaseWait: return "first-lease";
  case SchedulingProgressPhase::Published: return "publication";
  case SchedulingProgressPhase::EncodeOrOpenSession:
    return "encode-or-open-session";
  case SchedulingProgressPhase::Submitted: return "session-submit";
  case SchedulingProgressPhase::GpuSettled: return "gpu-settlement";
  case SchedulingProgressPhase::CompletionExpanded:
    return "completion-expansion";
  case SchedulingProgressPhase::PresentRelease: return "present-release";
  case SchedulingProgressPhase::Released: return "released";
  }
  return "unknown";
}

}  // namespace

SchedulingProgressWatchdog::SchedulingProgressWatchdog() {
  // No env parsing, clock read, atomic mutation, or thread creation occurs
  // until the outer perf gate is enabled.
  if (!perf::enabled()) {
    return;
  }
  const std::uint64_t thresholdMs = watchdogThresholdMs();
  if (thresholdMs == 0) {
    return;
  }
  enabled_ = true;
  thresholdNs_ = thresholdToNs(thresholdMs);
  samplerThread_ = std::thread([this] { runSampler(); });
}

SchedulingProgressWatchdog::SchedulingProgressWatchdog(
    bool enabled, std::uint64_t thresholdMs, bool startSamplerThread)
    : enabled_(enabled && thresholdMs != 0),
      thresholdNs_(enabled_
          ? thresholdToNs(thresholdMs)
          : 0) {
  if (enabled_ && startSamplerThread) {
    samplerThread_ = std::thread([this] { runSampler(); });
  }
}

SchedulingProgressWatchdog::~SchedulingProgressWatchdog() {
  stop();
}

std::uint64_t SchedulingProgressWatchdog::trackingOverflowCount() const
    noexcept {
  if (!enabled_) {
    return 0;
  }
  return overflowCount_.load(std::memory_order_relaxed);
}

std::uint64_t SchedulingProgressWatchdog::steadyNowNs() noexcept {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count());
}

void SchedulingProgressWatchdog::lockSlot(Slot& slot) noexcept {
  while (slot.lock.test_and_set(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
}

void SchedulingProgressWatchdog::unlockSlot(Slot& slot) noexcept {
  slot.lock.clear(std::memory_order_release);
}

bool SchedulingProgressWatchdog::findOrClaimLocked(
    Slot& slot, std::uint64_t seqId, std::uint64_t nowNs) noexcept {
  if (seqId == 0) {
    return false;
  }
  if (slot.identity == seqId) {
    return true;
  }
  const std::uint32_t occupiedFlags = slot.flags;
  const bool occupiedPresentPending =
      (occupiedFlags & SchedulingProgressHasPresent) != 0 &&
      (occupiedFlags & SchedulingProgressPresentSettled) == 0;
  const bool occupiedComplete =
      (occupiedFlags & SchedulingProgressReleased) != 0 &&
      !occupiedPresentPending;
  if (slot.identity != 0 && !occupiedComplete) {
    overflowCount_.fetch_add(1, std::memory_order_relaxed);
    std::uint64_t last =
        lastOverflowReportNs_.load(std::memory_order_relaxed);
    if (nowNs - last >= thresholdNs_ &&
        lastOverflowReportNs_.compare_exchange_strong(
            last, nowNs, std::memory_order_relaxed)) {
      util::logf(util::LogLevel::Warn, "scheduling-watchdog",
                 "tracking-overflow seq=%llu occupied=%llu capacity=%zu",
                 static_cast<unsigned long long>(seqId),
                 static_cast<unsigned long long>(slot.identity), kCapacity);
    }
    return false;
  }
  // All old-generation fields are finalized before identity publishes the
  // reusable generation. Every writer and sampler holds this same lock and
  // revalidates identity, so a delayed old-generation writer cannot retain a
  // pointer and store into the new generation after kCapacity reuse.
  slot.acceptedNs = nowNs;
  slot.progressNs = nowNs;
  slot.lastReportNs = 0;
  slot.generation = 0;
  slot.sourceOrdinal = 0;
  slot.flags = 0;
  slot.phase = SchedulingProgressPhase::Admission;
  slot.owner = queue::PipelineOwner::Queue;
  slot.disposition = queue::PipelineDisposition::Advance;
  slot.identity = seqId;
  return true;
}

void SchedulingProgressWatchdog::note(
    std::uint64_t seqId, SchedulingProgressPhase phase,
    std::uint32_t flags) noexcept {
  if (!enabled_) {
    return;
  }
  noteAt(seqId, phase, flags, steadyNowNs());
}

void SchedulingProgressWatchdog::noteAt(
    std::uint64_t seqId, SchedulingProgressPhase phase,
    std::uint32_t flags, std::uint64_t nowNs,
    queue::PipelineOwner owner, queue::PipelineDisposition disposition,
    std::uint64_t generation, std::uint64_t sourceOrdinal) noexcept {
  if (seqId == 0) {
    return;
  }
  auto& slot = slots_[seqId % kCapacity];
  lockSlot(slot);
  if (findOrClaimLocked(slot, seqId, nowNs)) {
    slot.flags |= flags;
    slot.phase = phase;
    slot.progressNs = nowNs;
    slot.owner = owner;
    slot.disposition = disposition;
    if (generation != 0) {
      slot.generation = generation;
    }
    if (sourceOrdinal != 0) {
      slot.sourceOrdinal = sourceOrdinal;
    }
  }
  unlockSlot(slot);
}

void SchedulingProgressWatchdog::noteAccepted(std::uint64_t seqId,
                                               bool hasPresent) noexcept {
  note(seqId, SchedulingProgressPhase::Admission,
       SchedulingProgressAccepted |
           (hasPresent ? SchedulingProgressHasPresent : 0u));
}

void SchedulingProgressWatchdog::noteLeaseWait(std::uint64_t seqId) noexcept {
  note(seqId, SchedulingProgressPhase::LeaseWait, 0);
}

void SchedulingProgressWatchdog::notePublished(std::uint64_t seqId,
                                                bool hasPresent) noexcept {
  note(seqId, SchedulingProgressPhase::Published,
       SchedulingProgressAccepted |
           (hasPresent ? SchedulingProgressHasPresent : 0u));
}

void SchedulingProgressWatchdog::noteEncodeOrOpenSession(
    std::uint64_t seqId) noexcept {
  note(seqId, SchedulingProgressPhase::EncodeOrOpenSession, 0);
}

void SchedulingProgressWatchdog::noteSubmitted(
    std::uint64_t seqId, bool capture) noexcept {
  note(seqId, SchedulingProgressPhase::Submitted,
       capture ? SchedulingProgressCapture : 0u);
}

void SchedulingProgressWatchdog::noteGpuSettled(std::uint64_t seqId) noexcept {
  note(seqId, SchedulingProgressPhase::GpuSettled, 0);
}

void SchedulingProgressWatchdog::noteCompletionExpanded(
    std::uint64_t seqId) noexcept {
  note(seqId, SchedulingProgressPhase::CompletionExpanded, 0);
}

void SchedulingProgressWatchdog::notePresentDisposition(
    std::uint64_t seqId, bool published) noexcept {
  note(seqId, SchedulingProgressPhase::EncodeOrOpenSession,
       SchedulingProgressHasPresent |
           (published ? SchedulingProgressPresentPublished
                      : SchedulingProgressPresentSkipped));
}

void SchedulingProgressWatchdog::noteReleased(
    std::uint64_t seqId, bool presentSettled) noexcept {
  if (!enabled_) {
    return;
  }
  const std::uint64_t nowNs = steadyNowNs();
  if (seqId == 0) {
    return;
  }
  auto& slot = slots_[seqId % kCapacity];
  lockSlot(slot);
  if (!findOrClaimLocked(slot, seqId, nowNs)) {
    unlockSlot(slot);
    return;
  }
  const std::uint32_t existing = slot.flags;
  const bool hasPresent =
      (existing & SchedulingProgressHasPresent) != 0;
  slot.flags |= SchedulingProgressReleased |
      (presentSettled ? SchedulingProgressPresentSettled : 0u);
  slot.phase = hasPresent ? SchedulingProgressPhase::PresentRelease
                          : SchedulingProgressPhase::Released;
  slot.progressNs = nowNs;
  unlockSlot(slot);
}

void SchedulingProgressWatchdog::notePipelineEvent(
    const queue::PipelineLifecycleEvent& event) noexcept {
  if (!enabled_ || !event.identity.valid()) {
    return;
  }
  SchedulingProgressPhase phase = SchedulingProgressPhase::Admission;
  switch (event.to) {
  case queue::PipelineStage::SourceArrival:
  case queue::PipelineStage::ProducerOwned:
    phase = SchedulingProgressPhase::Admission;
    break;
  case queue::PipelineStage::RawOwned:
  case queue::PipelineStage::ReplayBorrowed:
  case queue::PipelineStage::FinalOwned:
  case queue::PipelineStage::Encoding:
    phase = SchedulingProgressPhase::EncodeOrOpenSession;
    break;
  case queue::PipelineStage::GPUInFlight:
    phase = SchedulingProgressPhase::Submitted;
    break;
  case queue::PipelineStage::Completed:
    phase = SchedulingProgressPhase::CompletionExpanded;
    break;
  case queue::PipelineStage::Reclaimed:
    phase = event.control == queue::PipelineControl::Present
        ? SchedulingProgressPhase::PresentRelease
        : SchedulingProgressPhase::Released;
    break;
  }
  std::uint32_t flags = 0;
  if (event.hasPresent ||
      event.payloadKind == queue::PipelinePayloadKind::PresentOnly ||
      event.control == queue::PipelineControl::Present) {
    flags |= SchedulingProgressHasPresent;
  }
  if (event.to == queue::PipelineStage::GPUInFlight) {
    flags |= SchedulingProgressAccepted;
  }
  if (event.to == queue::PipelineStage::Reclaimed) {
    flags |= SchedulingProgressReleased;
    if (event.control == queue::PipelineControl::Present) {
      flags |= SchedulingProgressPresentSettled;
    }
  }
  noteAt(event.identity.seqId, phase, flags, steadyNowNs(), event.owner,
         event.disposition, event.identity.generation,
         event.identity.sourceOrdinal);
}

SchedulingProgressWatchdog::SlotSnapshotForTest
SchedulingProgressWatchdog::slotSnapshotForTest(
    std::uint64_t seqId) noexcept {
  SlotSnapshotForTest result{};
  if (!enabled_ || seqId == 0) {
    return result;
  }
  auto& slot = slots_[seqId % kCapacity];
  lockSlot(slot);
  result.tracked = slot.identity == seqId;
  result.identity = slot.identity;
  result.progressNs = slot.progressNs;
  result.generation = slot.generation;
  result.sourceOrdinal = slot.sourceOrdinal;
  result.flags = slot.flags;
  result.phase = slot.phase;
  result.owner = slot.owner;
  result.disposition = slot.disposition;
  unlockSlot(slot);
  return result;
}

void SchedulingProgressWatchdog::noteTerminal(bool deviceLoss) noexcept {
  if (!enabled_) {
    return;
  }
  terminalFlags_.fetch_or(deviceLoss ? SchedulingProgressDeviceLoss
                                     : SchedulingProgressStop,
                          std::memory_order_relaxed);
}

void SchedulingProgressWatchdog::sample() noexcept {
  if (!enabled_) {
    return;
  }
  const std::uint64_t nowNs = steadyNowNs();
  bool frontierReported = false;
  for (auto& slot : slots_) {
    lockSlot(slot);
    const std::uint64_t identity = slot.identity;
    if (identity == 0) {
      unlockSlot(slot);
      continue;
    }
    const std::uint32_t flags = slot.flags;
    const bool sourceReleased =
        (flags & SchedulingProgressReleased) != 0;
    const bool presentPending =
        (flags & SchedulingProgressHasPresent) != 0 &&
        (flags & SchedulingProgressPresentSettled) == 0;
    if (sourceReleased && !presentPending) {
      unlockSlot(slot);
      continue;
    }
    const std::uint64_t progressNs = slot.progressNs;
    const std::uint64_t acceptedNs = slot.acceptedNs;
    if (nowNs - progressNs < thresholdNs_) {
      unlockSlot(slot);
      continue;
    }
    const std::uint64_t lastReport = slot.lastReportNs;
    if (lastReport != 0 && nowNs - lastReport < thresholdNs_) {
      unlockSlot(slot);
      continue;
    }
    slot.lastReportNs = nowNs;
    const auto phase = slot.phase;
    unlockSlot(slot);
    const std::uint32_t terminal =
        terminalFlags_.load(std::memory_order_relaxed);
    if (!frontierReported) {
      perf::reportSchedulingProgressThreshold();
      frontierReported = true;
    }
    util::logf(
        util::LogLevel::Warn, "scheduling-watchdog",
        "pending seq=%llu phase=%s age_ms=%llu total_age_ms=%llu "
        "present=%u published=%u "
        "skipped=%u settled=%u stop=%u loss=%u capture=%u",
        static_cast<unsigned long long>(identity), phaseName(phase),
        static_cast<unsigned long long>((nowNs - progressNs) / 1000000ull),
        static_cast<unsigned long long>((nowNs - acceptedNs) / 1000000ull),
        (flags & SchedulingProgressHasPresent) != 0,
        (flags & SchedulingProgressPresentPublished) != 0,
        (flags & SchedulingProgressPresentSkipped) != 0,
        (flags & SchedulingProgressPresentSettled) != 0,
        (terminal & SchedulingProgressStop) != 0,
        (terminal & SchedulingProgressDeviceLoss) != 0,
        (flags & SchedulingProgressCapture) != 0);
  }
}

void SchedulingProgressWatchdog::runSampler() noexcept {
  const auto interval = std::chrono::nanoseconds(
      std::max<std::uint64_t>(1000000ull,
                              std::min<std::uint64_t>(thresholdNs_ / 2u,
                                                      1000000000ull)));
  std::unique_lock lock(samplerMutex_);
  while (!stopRequested_.load(std::memory_order_relaxed)) {
    if (samplerCv_.wait_for(lock, interval, [this] {
          return stopRequested_.load(std::memory_order_relaxed);
        })) {
      break;
    }
    lock.unlock();
    sample();
    lock.lock();
  }
}

void SchedulingProgressWatchdog::stop() noexcept {
  if (!enabled_ || !samplerThread_.joinable()) {
    return;
  }
  stopRequested_.store(true, std::memory_order_relaxed);
  samplerCv_.notify_all();
  samplerThread_.join();
}

}  // namespace dxmt9
