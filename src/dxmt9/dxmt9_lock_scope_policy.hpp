#pragma once

#include <cstdlib>

namespace dxmt9 {

// Experimental synchronization refinement, not a renderer/provider selector.
// Formal/native evidence precedes the separate GPU/wild promotion gate.
inline bool lockScopeRefinementEnabled() noexcept {
  static const bool enabled = [] {
    const char* value = std::getenv("DXMT9_LOCK_SCOPE_REFINEMENT");
    return value && value[0] != '\0' && value[0] != '0';
  }();
  return enabled;
}

// LockScopeRefinement!ValidateDestination: immutable preflight reserves no
// slot. The destination is checked only after acquiring the queue mutex.
constexpr bool preparedDrawBatchDestinationAvailable(
    bool stopped, bool arenaActive, bool directActive) noexcept {
  return !stopped && !arenaActive && !directActive;
}

}  // namespace dxmt9
