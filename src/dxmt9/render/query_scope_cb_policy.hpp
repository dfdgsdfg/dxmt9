#pragma once

namespace dxmt9::render {

// R-CORE-8.2(b): this grants FIFO CB sharing, never pass/replay coalescing.
constexpr bool queryScopeAllowsSession(bool scoped, bool joinEnabled) noexcept {
  return !scoped || joinEnabled;
}

constexpr bool queryScopeClosesSourceEncoders(bool scoped,
                                             bool deferred) noexcept {
  return scoped && deferred;
}

} // namespace dxmt9::render
