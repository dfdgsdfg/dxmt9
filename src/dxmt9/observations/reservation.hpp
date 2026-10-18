#pragma once
#include <atomic>
#include <cstdint>
namespace dxmt9::observations {
// Caller holds a trace writer lease until all reserved slots are initialized.
// The finalizer reads only after the admission gate has drained.
inline bool reserve(std::atomic<std::uint32_t>& cursor, std::uint32_t capacity,
                    std::uint32_t size, std::uint32_t& first) noexcept {
  first = cursor.load(std::memory_order_relaxed);
  for (std::uint32_t attempt = 0; attempt < 4 && first <= capacity &&
       size <= capacity - first; ++attempt) {
    if (cursor.compare_exchange_weak(first, first + size, std::memory_order_relaxed)) return true;
  }
  return false;
}
} // namespace dxmt9::observations
