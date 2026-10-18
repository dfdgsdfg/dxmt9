#pragma once

#include <cstdint>

namespace dxmt9::core {

// R-BACK-2.107. Capability absence retains reference validation; a present
// but stale capability rejects rather than silently widening its authority.
enum class PrefixPrepareDecision : std::uint8_t {
  Reject,
  Full,
  Suffix,
};

constexpr PrefixPrepareDecision decidePrefixPrepare(
    bool buildable, bool capabilityPresent, bool capabilityMatches,
    bool extentsPreserved) noexcept {
  if (!buildable) return PrefixPrepareDecision::Reject;
  if (!capabilityPresent) return PrefixPrepareDecision::Full;
  return capabilityMatches && extentsPreserved
      ? PrefixPrepareDecision::Suffix : PrefixPrepareDecision::Reject;
}

}  // namespace dxmt9::core
