#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>

namespace dxmt9::observations {

// Append-only, single publication per slot. Owners close their admission and
// account for active writers before snapshotting size(); incomplete slots are
// explicit, and storage remains alive until every admitted writer has retired.
// Each diagnostic keeps its own window, counters and consumer closure policy.
template <typename T, std::size_t Capacity> class BoundedRecords {
  struct Entry {
    std::atomic<bool> ready{false};
    T value{};
  };
public:
  bool allocate() noexcept {
    entries_.reset(new (std::nothrow) Entry[Capacity]);
    return entries_ != nullptr;
  }
  explicit operator bool() const noexcept { return entries_ != nullptr; }
  std::uint64_t reserve() noexcept {
    return next_.fetch_add(1, std::memory_order_relaxed);
  }
  T& writable(std::size_t index) noexcept { return entries_[index].value; }
  void publish(std::size_t index) noexcept {
    entries_[index].ready.store(true, std::memory_order_release);
  }
  const T* read(std::size_t index) const noexcept {
    return entries_ && index < Capacity &&
        entries_[index].ready.load(std::memory_order_acquire)
        ? &entries_[index].value : nullptr;
  }
  std::uint64_t size() const noexcept { return next_.load(); }
private:
  std::unique_ptr<Entry[]> entries_;
  std::atomic<std::uint64_t> next_{0};
};
}  // namespace dxmt9::observations
