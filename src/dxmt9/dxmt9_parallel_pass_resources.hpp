#pragma once

#include "dxmt9_source_semantics.hpp"
#include <array>
#include <bit>
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

// A synchronous borrow of one sealed pass's canonical identity table. The
// table is owned by the source-qualified snapshot, not by a child. No indexed
// coverage escapes certificate validation; the certificate retains its own
// value-owned snapshot. A bit has meaning only in the exact bound table.
class ParallelPassReadIndices {
 public:
  constexpr void bind(const ParallelPassReadSet& table) noexcept {
    table_ = &table;
    bits_ = {};
    valid_ = table.complete() && table.canonicalized();
  }
  void bind(ParallelPassReadSet&&) = delete;
  void bind(const ParallelPassReadSet&&) = delete;
  constexpr bool complete() const noexcept { return table_ && valid_; }
  constexpr bool validFor(const ParallelPassReadSet& table) const noexcept {
    return table_ == &table && complete();
  }
  constexpr bool add(std::uint64_t identity) noexcept {
    if (!complete()) return false;
    for (std::uint32_t i = 0; i < table_->count; ++i) {
      if (identity && table_->handles[i] == identity) {
        bits_[i / 64] |= std::uint64_t{1} << (i % 64);
        return true;
      }
    }
    // Never turn an unknown identity into an empty/disjoint set.
    valid_ = false;
    return false;
  }
  template<typename Set>
  constexpr bool include(const Set& reads) noexcept {
    if (!complete() || !reads.complete() || !reads.canonicalized() ||
        reads.count > reads.handles.size()) {
      valid_ = false;
      return false;
    }
    for (std::uint32_t i = 0; i < reads.count; ++i)
      if (!add(reads.handles[i])) return false;
    return true;
  }
  constexpr bool merge(const ParallelPassReadIndices& other) noexcept {
    if (!complete() || !other.complete() || table_ != other.table_) {
      valid_ = false;
      return false;
    }
    for (std::size_t i = 0; i < bits_.size(); ++i) bits_[i] |= other.bits_[i];
    return true;
  }
  constexpr std::uint32_t count() const noexcept {
    std::uint32_t result = 0;
    for (auto word : bits_) result += std::popcount(word);
    return result;
  }
  constexpr bool coversTable() const noexcept {
    return complete() && count() == table_->count;
  }
  constexpr bool contains(std::uint64_t identity) const noexcept {
    if (!complete() || !identity) return false;
    for (std::uint32_t i = 0; i < table_->count; ++i)
      if (table_->handles[i] == identity)
        return bits_[i / 64] & (std::uint64_t{1} << (i % 64));
    return false;
  }
  template<typename Set>
  constexpr bool containsAll(const Set& other) const noexcept {
    if (!complete() || !other.complete() || !other.canonicalized() ||
        other.count > other.handles.size()) return false;
    for (std::uint32_t i = 0; i < other.count; ++i)
      if (!contains(other.handles[i])) return false;
    return true;
  }
  template<typename Set>
  constexpr bool overlaps(const Set& other) const noexcept {
    if (!complete() || !other.complete() || !other.canonicalized() ||
        other.count > other.handles.size()) return true;
    for (std::uint32_t i = 0; i < other.count; ++i)
      if (contains(other.handles[i])) return true;
    return false;
  }

 private:
  const ParallelPassReadSet* table_ = nullptr;
  std::array<std::uint64_t, (kParallelPassReadCapacity + 63) / 64> bits_{};
  bool valid_ = false;
};

static_assert(std::is_trivially_copyable_v<ParallelPassReadIndices>);
static_assert(sizeof(ParallelPassReadIndices) < sizeof(ParallelPassReadSet));

} // namespace dxmt9::encoders
