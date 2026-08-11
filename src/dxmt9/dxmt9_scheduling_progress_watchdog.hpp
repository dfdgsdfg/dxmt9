#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>

namespace dxmt9 {

enum class SchedulingProgressPhase : std::uint8_t {
  Admission,
  LeaseWait,
  Published,
  EncodeOrOpenSession,
  Submitted,
  GpuSettled,
  CompletionExpanded,
  PresentRelease,
  Released,
};

enum SchedulingProgressFlag : std::uint32_t {
  SchedulingProgressAccepted = 1u << 0,
  SchedulingProgressHasPresent = 1u << 1,
  SchedulingProgressPresentPublished = 1u << 2,
  SchedulingProgressPresentSkipped = 1u << 3,
  SchedulingProgressPresentSettled = 1u << 4,
  SchedulingProgressReleased = 1u << 5,
  SchedulingProgressStop = 1u << 6,
  SchedulingProgressDeviceLoss = 1u << 7,
  SchedulingProgressCapture = 1u << 8,
};

// CommandQueue-owned, diagnostic-only progress ledger. The fixed slots are
// generation-stamped by seqId; collisions with live obligations are reported
// as tracking overflow rather than allocating or perturbing scheduling.
class SchedulingProgressWatchdog {
 public:
  static constexpr std::size_t kCapacity = 256;

  SchedulingProgressWatchdog();
  SchedulingProgressWatchdog(bool enabled, std::uint64_t thresholdMs,
                             bool startSamplerThread);
  ~SchedulingProgressWatchdog();

  SchedulingProgressWatchdog(const SchedulingProgressWatchdog&) = delete;
  SchedulingProgressWatchdog& operator=(
      const SchedulingProgressWatchdog&) = delete;

  bool enabled() const noexcept { return enabled_; }
  std::uint64_t trackingOverflowCount() const noexcept;

  void noteAccepted(std::uint64_t seqId, bool hasPresent) noexcept;
  void noteLeaseWait(std::uint64_t seqId) noexcept;
  void notePublished(std::uint64_t seqId, bool hasPresent) noexcept;
  void noteEncodeOrOpenSession(std::uint64_t seqId) noexcept;
  void noteSubmitted(std::uint64_t seqId, bool capture = false) noexcept;
  void noteGpuSettled(std::uint64_t seqId) noexcept;
  void noteCompletionExpanded(std::uint64_t seqId) noexcept;
  void notePresentDisposition(std::uint64_t seqId, bool published) noexcept;
  void noteReleased(std::uint64_t seqId, bool presentSettled) noexcept;
  void noteTerminal(bool deviceLoss) noexcept;
  void stop() noexcept;

  struct SlotSnapshotForTest {
    bool tracked = false;
    std::uint64_t identity = 0;
    std::uint64_t progressNs = 0;
    std::uint32_t flags = 0;
    SchedulingProgressPhase phase = SchedulingProgressPhase::Admission;
  };
  SlotSnapshotForTest slotSnapshotForTest(std::uint64_t seqId) noexcept;

  template <typename Clock>
  void noteAcceptedWithClockForTest(std::uint64_t seqId, bool hasPresent,
                                    Clock&& clock) noexcept {
    if (!enabled_) {
      return;
    }
    noteAt(seqId, SchedulingProgressPhase::Admission,
           SchedulingProgressAccepted |
               (hasPresent ? SchedulingProgressHasPresent : 0u),
           clock());
  }

 private:
  struct Slot {
    std::atomic_flag lock = ATOMIC_FLAG_INIT;
    std::uint64_t identity = 0;
    std::uint64_t acceptedNs = 0;
    std::uint64_t progressNs = 0;
    std::uint64_t lastReportNs = 0;
    std::uint32_t flags = 0;
    SchedulingProgressPhase phase = SchedulingProgressPhase::Admission;
  };

  static std::uint64_t steadyNowNs() noexcept;
  void note(std::uint64_t seqId, SchedulingProgressPhase phase,
            std::uint32_t flags) noexcept;
  void noteAt(std::uint64_t seqId, SchedulingProgressPhase phase,
              std::uint32_t flags, std::uint64_t nowNs) noexcept;
  static void lockSlot(Slot& slot) noexcept;
  static void unlockSlot(Slot& slot) noexcept;
  bool findOrClaimLocked(Slot& slot, std::uint64_t seqId,
                         std::uint64_t nowNs) noexcept;
  void sample() noexcept;
  void runSampler() noexcept;

  bool enabled_ = false;
  std::uint64_t thresholdNs_ = 0;
  std::array<Slot, kCapacity> slots_{};
  std::atomic<std::uint64_t> overflowCount_{0};
  std::atomic<std::uint64_t> lastOverflowReportNs_{0};
  std::atomic<std::uint32_t> terminalFlags_{0};
  std::atomic<bool> stopRequested_{false};
  std::mutex samplerMutex_{};
  std::condition_variable samplerCv_{};
  std::thread samplerThread_{};
};

}  // namespace dxmt9
