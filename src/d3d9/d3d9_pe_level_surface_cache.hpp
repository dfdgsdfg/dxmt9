#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace dxmt9::d3d9::pe {

// Per-(container, subresource) memo for resolved level-surface handles.
//
// `dxmt9c_texture_get_surface_level` is not a getter: the unix side allocates a
// fresh `D9CSurface` handle, inserts a fresh wire-registry identity, and — when
// the owning `core::Texture`'s weak per-level cache has expired — builds a new
// backend surface for the subresource. An app that re-fetches a level every
// frame therefore pays that whole round trip per call. The resolved handle is
// otherwise immutable for the owning PE wrapper's lifetime, so the wrapper
// resolves it once and hands out borrowed references.
//
// This class is the pure, allocation-bounded part of that policy: it owns no
// handles and performs no bridge calls. The container does the crossing on a
// miss, stores the result here, and releases every stored entry exactly once
// from its own destructor via `releaseAll`.
template <typename Entry>
class LevelSurfaceCache {
 public:
  // Subresource indices at or above this bound are never memoized. D3D9 tops
  // out at 15 mip levels (6 * 15 for a cube), so the bound only exists so a
  // bogus index from the app cannot size the slot vector; such calls fall back
  // to the owning per-call path.
  static constexpr std::uint32_t kMaxCachedIndex = 1024u;

  static constexpr bool cacheable(std::uint32_t index) noexcept {
    return index < kMaxCachedIndex;
  }

  // Resolved entry for `index`, or nullptr when the index has not been resolved
  // yet or is outside the cacheable window.
  const Entry* find(std::uint32_t index) const noexcept {
    if (!cacheable(index) || index >= slots_.size()) {
      return nullptr;
    }
    const auto& slot = slots_[index];
    return slot.resolved ? &slot.entry : nullptr;
  }

  // Memoizes `entry` at `index`. Returns the stored entry, or nullptr when the
  // index is not cacheable or the slot vector could not grow — in which case
  // the caller still owns `entry` and must release it itself. An occupied slot
  // is never overwritten: the already-stored entry is returned and the caller
  // must release the duplicate it just resolved.
  const Entry* store(std::uint32_t index, Entry entry) {
    if (!cacheable(index)) {
      return nullptr;
    }
    if (index >= slots_.size()) {
      try {
        slots_.resize(static_cast<std::size_t>(index) + 1u);
      } catch (...) {
        return nullptr;
      }
    }
    auto& slot = slots_[index];
    if (slot.resolved) {
      return &slot.entry;
    }
    slot.entry = std::move(entry);
    slot.resolved = true;
    ++resolved_;
    return &slot.entry;
  }

  // Invokes `release(Entry&)` once for every resolved entry, then empties the
  // cache. Safe on an empty cache and safe to call twice.
  template <typename ReleaseFn>
  void releaseAll(ReleaseFn&& release) noexcept {
    for (auto& slot : slots_) {
      if (!slot.resolved) {
        continue;
      }
      slot.resolved = false;
      release(slot.entry);
    }
    slots_.clear();
    resolved_ = 0u;
  }

  std::size_t resolvedCount() const noexcept { return resolved_; }

 private:
  struct Slot {
    Entry entry{};
    bool resolved = false;
  };

  std::vector<Slot> slots_{};
  std::size_t resolved_ = 0u;
};

}  // namespace dxmt9::d3d9::pe
