#pragma once

#include <cstdlib>
#include <string_view>

namespace dxmt9::d3d9 {

// Diagnostic ablation only. The enclosing hash timer and all semantic work
// survive every mode; no mode grants permission to reuse a hash or payload.
enum class UniformHashObserverMode { Full, Counts, Parent };

constexpr UniformHashObserverMode parseUniformHashObserverMode(
    std::string_view value) noexcept {
  if (value == "counts") return UniformHashObserverMode::Counts;
  if (value == "parent") return UniformHashObserverMode::Parent;
  // Preserve existing observation for unset, "full", and unknown values.
  return UniformHashObserverMode::Full;
}

constexpr bool uniformHashObserverTimes(UniformHashObserverMode mode) noexcept {
  return mode == UniformHashObserverMode::Full;
}

constexpr bool uniformHashObserverCounts(UniformHashObserverMode mode) noexcept {
  return mode != UniformHashObserverMode::Parent;
}

inline UniformHashObserverMode uniformHashObserverMode() noexcept {
  static const auto mode = [] {
    const char* value = std::getenv("DXMT9_PERF_UNIFORM_HASH_OBSERVER_MODE");
    return parseUniformHashObserverMode(value ? value : "");
  }();
  return mode;
}

} // namespace dxmt9::d3d9
