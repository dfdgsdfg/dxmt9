#pragma once

#include <cstdint>

namespace dxmt9::draw_parent_completion {

inline constexpr std::uint32_t kMinimumWindow = 1u;
inline constexpr std::uint32_t kMaximumWindow = 64u;

constexpr bool validWindow(std::uint32_t window) noexcept {
  return window >= kMinimumWindow && window <= kMaximumWindow;
}

// Ordinals are one-based and local to one phase. A zero-length phase has no
// groups and therefore no completion boundary. Invalid inputs have no
// scheduled boundary; callers validate the environment bounds separately.
constexpr bool shouldDrain(std::uint32_t ordinal, std::uint32_t phaseCount,
                           std::uint32_t window) noexcept {
  if (!validWindow(window) || ordinal == 0u || phaseCount == 0u ||
      ordinal > phaseCount)
    return false;
  return ordinal % window == 0u || ordinal == phaseCount;
}

}  // namespace dxmt9::draw_parent_completion
