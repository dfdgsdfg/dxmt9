#pragma once

#include <atomic>
#include <cstddef>
#include <string_view>

namespace dxmt9::diagnostics {
// Synchronous fan-out of an already acquired report. The consumer must copy
// before returning; it never owns the producer or its compatibility output.
using Consumer = void (*)(const char*, std::string_view, bool) noexcept;
using Interest = bool (*)(const char*) noexcept;
inline std::atomic<Consumer> consumer{nullptr};
inline std::atomic<Interest> interest{nullptr};
inline bool interested(const char* schema) noexcept {
  if (auto predicate = interest.load(std::memory_order_relaxed))
    return predicate(schema);
  return false;
}
inline void report(const char* schema, std::string_view data,
                   bool truncated = false) noexcept {
  if (auto sink = consumer.load(std::memory_order_relaxed))
    sink(schema, data, truncated);
}
}  // namespace dxmt9::diagnostics
