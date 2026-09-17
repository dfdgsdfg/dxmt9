#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>

namespace dxmt9::query {

// Unix-owned issue generation. No public Query/COM object is retained by GPU
// work. A new BEGIN allocates a new result rather than resetting live storage.
struct OcclusionResult {
  const std::uint64_t object;
  const std::uint64_t issue;
  const std::uint64_t epoch;
  std::atomic<std::uint64_t> samples{0};
  std::atomic<std::uint32_t> pending{0};
  std::atomic<bool> failed{false};
  std::atomic<bool> abandoned{false};

  OcclusionResult(std::uint64_t object, std::uint64_t issue,
                  std::uint64_t epoch) noexcept
      : object(object), issue(issue), epoch(epoch) {}

  static constexpr std::uint64_t addSaturated(std::uint64_t a,
                                             std::uint64_t b) noexcept {
    return b > std::numeric_limits<std::uint64_t>::max() - a
        ? std::numeric_limits<std::uint64_t>::max() : a + b;
  }
  void add(std::uint64_t value) noexcept {
    auto old = samples.load(std::memory_order_relaxed);
    while (!samples.compare_exchange_weak(old, addSaturated(old, value),
              std::memory_order_relaxed)) {}
  }
  // This is an additional condition, never a substitute for the actual END
  // fence. In particular pending==0 before encoding does not mean complete.
  bool readableAfterFence() const noexcept {
    return !failed.load(std::memory_order_acquire) &&
        pending.load(std::memory_order_acquire) == 0;
  }
};

// Membership changes are ordered source boundaries. One pass contribution is
// shared by all overlapping queries, without replaying any draw twice.
struct OcclusionScope {
  static constexpr std::size_t capacity = 64;
  std::array<std::shared_ptr<OcclusionResult>, capacity> results{};
  std::size_t count = 0;

  bool contains(const std::shared_ptr<OcclusionResult>& result) const noexcept {
    for (std::size_t i = 0; i < count; ++i)
      if (results[i] == result) return true;
    return false;
  }
  void fail() const noexcept {
    for (std::size_t i = 0; i < count; ++i)
      results[i]->failed.store(true, std::memory_order_release);
  }
};

using OcclusionScopeRef = std::shared_ptr<const OcclusionScope>;

} // namespace dxmt9::query
