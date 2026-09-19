#pragma once

#include "dxmt9/core_constants.hpp"
#include <array>
#include <cstddef>

namespace dxmt9::resources {

// One instance per locked Slot publication, at one fixed sequence. Only
// texture/surface max-stamps are idempotent; logical buffer marks select a
// mutable backing and MUST pass through. A collision costs a redundant mark.
template<std::size_t Capacity = 64>
class PublicationMarkCache {
  static_assert(Capacity != 0);
 public:
  bool shouldMark(core::ChunkHandleEntry entry) noexcept {
    if (!entry.handle || (entry.kind != core::ChunkHandleKind::Texture &&
                          entry.kind != core::ChunkHandleKind::Surface))
      return true;
    const auto value = entry.handle.value;
    const auto hash = value ^ (value >> 32) ^ static_cast<unsigned>(entry.kind);
    auto& previous = entries_[hash % Capacity];
    if (previous.handle == entry.handle && previous.kind == entry.kind)
      return false;
    previous = entry;
    return true;
  }
 private:
  std::array<core::ChunkHandleEntry, Capacity> entries_{};
};
} // namespace dxmt9::resources
