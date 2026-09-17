#pragma once

#include <cstdlib>

namespace dxmt9::query {
// One cached decision for the public fence and its initializer authority.
// An explicit zero retains the synchronous diagnostic/reference path.
inline bool asynchronousFencesEnabled() noexcept {
  static const bool enabled = [] {
    const char* value = std::getenv("DXMT9_ASYNC_QUERY_FENCES");
    return !value || (value[0] && value[0] != '0');
  }();
  return enabled;
}
} // namespace dxmt9::query
