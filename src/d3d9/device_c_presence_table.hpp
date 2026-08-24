#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <limits>
#include <memory>
#include <vector>

namespace dxmt9::d3d9 {

// Allocation is a performance concern only: callers retain a complete linear
// truth source and consult it whenever reset cannot provision this accelerator
// or the bounded table overflows.
template <typename Key, typename Hash = std::hash<Key>,
          typename Equal = std::equal_to<Key>,
          typename Allocator = std::allocator<Key>>
class PresenceTable {
 private:
  struct Slot {
    bool occupied = false;
    Key key{};
  };
  using SlotAllocator = typename std::allocator_traits<Allocator>::
      template rebind_alloc<Slot>;

 public:
  explicit PresenceTable(const Allocator& allocator = Allocator{})
      : slots_(SlotAllocator(allocator)) {}

  void reset(std::size_t capacityHint) noexcept {
    occupied_ = 0u;
    setOverflowed(true);
    if (capacityHint > (std::numeric_limits<std::size_t>::max() / 2u)) {
      return;
    }
    const std::size_t target = std::max<std::size_t>(capacityHint * 2u, 64u);
    std::size_t capacity = slots_.empty() ? 64u : slots_.size();
    while (capacity < target) {
      if (capacity > (std::numeric_limits<std::size_t>::max() / 2u)) {
        return;
      }
      capacity <<= 1u;
    }
    try {
      if (capacity != slots_.size()) {
        slots_.assign(capacity, Slot{});
      } else {
        std::fill(slots_.begin(), slots_.end(), Slot{});
      }
    } catch (...) {
      return;
    }
    setOverflowed(false);
  }

  bool overflowed() const noexcept { return overflowed_; }

  bool contains(const Key& key) const noexcept {
    if (overflowed_ || slots_.empty()) return false;
    const auto mask = slots_.size() - 1u;
    auto index = hash_(key) & mask;
    for (std::size_t probes = 0u; probes < slots_.size(); ++probes) {
      const auto& slot = slots_[index];
      if (!slot.occupied) return false;
      if (equal_(slot.key, key)) return true;
      index = (index + 1u) & mask;
    }
    return false;
  }

  bool insert(const Key& key) noexcept {
    const std::size_t occupancyLimit =
        slots_.size() - (slots_.size() / 4u);
    if (overflowed_ || slots_.empty() || occupied_ >= occupancyLimit) {
      setOverflowed(true);
      return false;
    }
    const auto mask = slots_.size() - 1u;
    auto index = hash_(key) & mask;
    for (std::size_t probes = 0u; probes < slots_.size(); ++probes) {
      auto& slot = slots_[index];
      if (!slot.occupied) {
        slot.occupied = true;
        slot.key = key;
        ++occupied_;
        return true;
      }
      if (equal_(slot.key, key)) return true;
      index = (index + 1u) & mask;
    }
    setOverflowed(true);
    return false;
  }

 private:
  void setOverflowed(bool value) noexcept { overflowed_ = value; }

  std::vector<Slot, SlotAllocator> slots_;
  std::size_t occupied_ = 0u;
  bool overflowed_ = true;
  [[no_unique_address]] Hash hash_{};
  [[no_unique_address]] Equal equal_{};
};

}  // namespace dxmt9::d3d9
