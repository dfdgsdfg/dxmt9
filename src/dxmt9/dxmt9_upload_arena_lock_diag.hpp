#pragma once

// Opt-in, bounded timing for the DeviceImpl upload queue mutex and the buffer
// HandleArena.  This diagnostic deliberately has its own sink: the existing
// queue-mutex report takes a sink mutex while a queue lock may still be held.
// Upload timings are published only after the observed lock has been released.

#include <atomic>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <shared_mutex>

namespace dxmt9 {

enum class UploadLockSite : std::uint8_t {
  QueueMutex = 0,
  BufferArena = 1,
};

enum class UploadLockOperation : std::uint8_t {
  UploadData = 0,
  UploadRange = 1,
  Update = 2,
  UpdateMany = 3,
  Inspect = 4,
  Reclaim = 5,
};

enum class UploadLockMode : std::uint8_t {
  Exclusive = 0,
  Shared = 1,
};

// One cached environment read.  Callers must branch on this before creating a
// probe so the disabled upload path does not read a clock or write probe state.
inline bool uploadArenaLockSplitEnabled() noexcept {
  static const bool enabled = [] {
    const char* value = std::getenv("DXMT9_PERF_UPLOAD_ARENA_LOCK_SPLIT");
    return value && value[0] != '\0' && value[0] != '0';
  }();
  return enabled;
}

std::uint64_t uploadArenaNativeThreadId() noexcept;

// Must be called after the measured lock has been released.  The sink is
// fixed-size and atomic; it never allocates, logs, or takes a mutex.
void recordUploadArenaLock(UploadLockSite site,
                           UploadLockOperation operation,
                           UploadLockMode mode,
                           std::uint64_t waitNs,
                           std::uint64_t holdNs) noexcept;

// Emits cumulative rows sampled at a process-global Present ordinal.  This is
// called before submitPresent acquires the queue mutex.
void snapshotUploadArenaLockSites(std::uint64_t snapshotOrdinal) noexcept;

struct UploadLockBucketSnapshot {
  bool valid = false;
  std::uint64_t nativeThreadId = 0;
  UploadLockSite site = UploadLockSite::QueueMutex;
  UploadLockOperation operation = UploadLockOperation::UploadData;
  UploadLockMode mode = UploadLockMode::Exclusive;
  std::uint64_t count = 0;
  std::uint64_t waitNs = 0;
  std::uint64_t holdNs = 0;
  std::uint64_t maxHoldNs = 0;
};

struct UploadLockSnapshot {
  static constexpr std::size_t kBucketCount = 256;
  std::array<UploadLockBucketSnapshot, kBucketCount> buckets{};
  std::size_t validCount = 0;
  std::uint64_t overflow = 0;
  std::uint64_t dropped = 0;
  std::uint64_t inconsistent = 0;
  std::uint64_t skippedRows = 0;
};

// Test/inspection pull path. It performs a bounded, non-blocking seqlock
// sample and never resets cumulative counters.
void collectUploadArenaLockSnapshot(UploadLockSnapshot& out) noexcept;

template <typename Lock, typename Clock = std::chrono::steady_clock>
class UploadLockScope {
 public:
  template <typename Mutex>
  UploadLockScope(Mutex& mutex, UploadLockSite site,
                  UploadLockOperation operation, UploadLockMode mode)
      : lock_(mutex, std::defer_lock), site_(site), operation_(operation),
        mode_(mode), acquiredAt_(Clock::now()) {
    lock_.lock();
    const auto now = Clock::now();
    waitNs_ = toNanos(now - acquiredAt_);
    holdStartedAt_ = now;
  }

  UploadLockScope(const UploadLockScope&) = delete;
  UploadLockScope& operator=(const UploadLockScope&) = delete;

  ~UploadLockScope() noexcept {
    const auto holdNs = toNanos(Clock::now() - holdStartedAt_);
    // Release first.  In particular, buffer-arena observation runs while the
    // enclosing upload queue mutex is held, so no sink mutex is permissible.
    lock_.unlock();
    recordUploadArenaLock(site_, operation_, mode_, waitNs_, holdNs);
  }

 private:
  static std::uint64_t toNanos(typename Clock::duration duration) noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count());
  }

  Lock lock_;
  UploadLockSite site_;
  UploadLockOperation operation_;
  UploadLockMode mode_;
  typename Clock::time_point acquiredAt_;
  typename Clock::time_point holdStartedAt_{};
  std::uint64_t waitNs_ = 0;
};

using UploadQueueLockScope = UploadLockScope<std::unique_lock<std::mutex>>;
using UploadArenaUniqueLockScope =
    UploadLockScope<std::unique_lock<std::shared_mutex>>;
using UploadArenaSharedLockScope =
    UploadLockScope<std::shared_lock<std::shared_mutex>>;

}  // namespace dxmt9
