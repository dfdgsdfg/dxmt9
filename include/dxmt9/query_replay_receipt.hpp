#pragma once

#include <cstdint>

namespace dxmt9::query {

enum class ReplayReceiptState : std::uint8_t {
  Pending,
  Ready,
  Superseded,
  Failed,
};

// QueryReplayReceipt: capture fixes the CPU prefix, not a GPU completion.
// A newer admitted Issue (including BEGIN) invalidates this attempt instead
// of extending its wait or letting it read a newer mutable Query generation.
constexpr ReplayReceiptState classifyReplayReceipt(
    std::uint64_t captured, std::uint64_t admitted,
    std::uint64_t replayed, bool terminal) noexcept {
  if (terminal || captured > admitted || replayed > admitted)
    return ReplayReceiptState::Failed;
  if (captured != admitted) return ReplayReceiptState::Superseded;
  return replayed >= captured ? ReplayReceiptState::Ready
                              : ReplayReceiptState::Pending;
}

}  // namespace dxmt9::query
