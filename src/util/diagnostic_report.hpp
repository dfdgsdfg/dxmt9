#pragma once

#include <atomic>
#include <cstddef>
#include <string_view>

namespace dxmt9::diagnostics {
// Synchronous fan-out of an already acquired report. The consumer must copy
// before returning; it never owns the producer or its compatibility output.
using Consumer = void (*)(const char*, std::string_view, bool) noexcept;
inline std::atomic<Consumer> consumer{nullptr};
inline void report(const char* schema, std::string_view data,
                   bool truncated = false) noexcept {
  if (auto sink = consumer.load(std::memory_order_relaxed))
    sink(schema, data, truncated);
}
}  // namespace dxmt9::diagnostics
