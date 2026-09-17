#pragma once

#include <cstdlib>

namespace dxmt9::query {
inline bool metalOcclusionEnabled() noexcept {
  static const bool enabled = [] {
    const char* value = std::getenv("DXMT9_METAL_OCCLUSION");
    return value && value[0] && value[0] != '0';
  }();
  return enabled;
}

// One cached decision for the public fence and its initializer authority.
// An explicit zero retains the synchronous diagnostic/reference path.
inline bool asynchronousFencesEnabled() noexcept {
  static const bool enabled = [] {
    const char* value = std::getenv("DXMT9_ASYNC_QUERY_FENCES");
    return !value || (value[0] && value[0] != '0');
  }();
  return enabled;
}
inline bool coalesceEndsEnabled() noexcept {
  static const bool enabled = [] {
    const char* value = std::getenv("DXMT9_COALESCE_QUERY_ENDS");
    return value && value[0] && value[0] != '0';
  }();
  return enabled && asynchronousFencesEnabled();
}
} // namespace dxmt9::query
