#pragma once

#include <cstdint>
#include <mutex>

namespace dxmt9::d3d9::pe {

// The PE recorder is single-producer by default.  This guard is the one
// production ownership witness used by both the append envelope and the
// cold state-block/tape intervals.  Keeping the branch in this small type is
// important: the unlocked path must not construct a lock operation or touch
// an atomic merely because a setter was entered.
class RecorderLockGuard final {
 public:
  RecorderLockGuard(std::recursive_mutex& mutex, bool required) noexcept
      : mutex_(mutex), required_(required) {
    if (required_)
      mutex_.lock();
  }

  ~RecorderLockGuard() {
    if (required_)
      mutex_.unlock();
  }

  RecorderLockGuard(const RecorderLockGuard&) = delete;
  RecorderLockGuard& operator=(const RecorderLockGuard&) = delete;

 private:
  std::recursive_mutex& mutex_;
  bool required_;
};

constexpr bool recorderLockRequired(std::uint32_t behaviorFlags,
                                    bool forceLock) noexcept {
  constexpr std::uint32_t kD3DCreateMultithreaded = 0x00000004u;
  return (behaviorFlags & kD3DCreateMultithreaded) != 0u || forceLock;
}

}  // namespace dxmt9::d3d9::pe
