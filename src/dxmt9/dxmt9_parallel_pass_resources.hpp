#pragma once

#include "dxmt9_source_semantics.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace dxmt9::encoders {

// Pass/child union storage, not the copied draw-entry continuation summary.
// Only canonical identities are accumulated here; source/generation/range
// qualification belongs to the enclosing sealed snapshot and certificate.
// The bound is an admission budget, not a claim about workload cardinality.
inline constexpr std::size_t kParallelPassReadCapacity = 256u;

template<std::size_t Capacity>
struct BoundedParallelPassReadSet {
  std::array<std::uint64_t, Capacity> handles{};
  std::uint32_t count = 0;
  std::uint32_t flags = core::ExactResourceSetComplete;

  constexpr bool complete() const noexcept {
    return count <= Capacity && (flags & core::ExactResourceSetComplete) &&
        !(flags & core::ExactResourceSetOverflow);
  }
  constexpr bool canonicalized() const noexcept {
    return flags & core::ExactResourceSetCanonicalized;
  }
  constexpr bool add(std::uint64_t identity) noexcept {
    if (!complete()) return false;
    if (!identity) return true;
    for (std::uint32_t i = 0; i < count; ++i)
      if (handles[i] == identity) return true;
    if (count == Capacity) {
      flags = (flags & ~core::ExactResourceSetComplete) |
          core::ExactResourceSetOverflow;
      return false;
    }
    handles[count++] = identity;
    return true;
  }
  template<typename Set>
  constexpr bool overlaps(const Set& other) const noexcept {
    // Invalid input is never a disjointness proof.
    if (!complete() || !other.complete() || other.count > other.handles.size())
      return true;
    for (std::uint32_t i = 0; i < count; ++i)
      for (std::uint32_t j = 0; j < other.count; ++j)
        if (handles[i] == other.handles[j]) return true;
    return false;
  }
  // Explicit small-to-wide import only; never silently narrow a pass to an entry.
  constexpr void importEntry(const core::ExactResourceSet& entry) noexcept {
    *this = {};
    flags = entry.flags;
    if (!entry.complete() || entry.count > entry.handles.size()) {
      flags &= ~core::ExactResourceSetComplete;
      return;
    }
    for (std::uint32_t i = 0; i < entry.count; ++i)
      if (!add(entry.handles[i])) return;
  }
  friend constexpr bool operator==(const BoundedParallelPassReadSet&,
                                   const BoundedParallelPassReadSet&) = default;
};

using ParallelPassReadSet = BoundedParallelPassReadSet<kParallelPassReadCapacity>;
static_assert(std::is_trivially_copyable_v<ParallelPassReadSet>);
static_assert(core::kSourceEntryResourceCapacity == 12u);

} // namespace dxmt9::encoders
