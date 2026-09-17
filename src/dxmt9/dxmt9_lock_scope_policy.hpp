#pragma once

namespace dxmt9 {

// LockScopeRefinement!ValidateDestination: immutable preflight reserves no
// slot. The destination is checked only after acquiring the queue mutex.
constexpr bool preparedDrawBatchDestinationAvailable(
    bool stopped, bool arenaActive, bool directActive) noexcept {
  return !stopped && !arenaActive && !directActive;
}

}  // namespace dxmt9
