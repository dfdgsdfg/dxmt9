#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

namespace dxmt9::query {

enum class UploadStatus : std::uint8_t { Pending, Complete, Failed };

// Queue-mutex-owned, fixed storage. Metal handles are retained here; tests
// supply value handles and statuses to the very same admission/reaping code.
// QueryInitializerFence: event signaling and successful completion are
// independent observations. No allocation, callback ownership, or query ptr.
template <typename Buffer, std::size_t Capacity>
class UploadTracker {
  static_assert(Capacity > 0);
 public:
  UploadTracker() = default;
  UploadTracker(const UploadTracker&) = delete;
  UploadTracker& operator=(const UploadTracker&) = delete;

  bool admit(bool hasEvent, std::uint64_t next) noexcept {
    if (failed_ || count_ == Capacity || !hasEvent ||
        next == std::numeric_limits<std::uint64_t>::max() ||
        next == 0 || next != published_ + 1) {
      fail();
      return false;
    }
    return true;
  }

  bool append(Buffer buffer, std::uint64_t value) noexcept {
    if (!admit(true, value)) return false;
    entries_[(head_ + count_) % Capacity] = {std::move(buffer), value};
    ++count_;
    published_ = value;
    return true;
  }

  template <typename StatusOf>
  std::uint64_t poll(std::uint64_t signaled, StatusOf statusOf) noexcept {
    while (!failed_ && count_) {
      auto& head = entries_[head_];
      const auto status = statusOf(head.buffer);
      if (status == UploadStatus::Failed) {
        fail();
        break;
      }
      if (status != UploadStatus::Complete) break;
      completed_ = head.value;
      head = {};
      head_ = (head_ + 1) % Capacity;
      --count_;
    }
    return std::min(completed_, signaled);
  }

  void fail() noexcept { failed_ = true; }
  bool failed() const noexcept { return failed_; }
  std::size_t pending() const noexcept { return count_; }
  std::uint64_t published() const noexcept { return published_; }

 private:
  struct Entry { Buffer buffer{}; std::uint64_t value = 0; };
  std::array<Entry, Capacity> entries_{};
  std::size_t head_ = 0, count_ = 0;
  std::uint64_t published_ = 0, completed_ = 0;
  bool failed_ = false;
};
} // namespace dxmt9::query
