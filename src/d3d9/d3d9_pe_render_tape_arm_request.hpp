#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace dxmt9::d3d9 {

// Cold experiment control, not a PE/Unix ABI packet. Little-endian Windows
// writers publish exactly these 64 bytes; no pointer or game-state claim.
struct RenderTapeArmRequest {
  std::uint64_t magic = 0x31514d5241394444ull;
  std::uint32_t version = 1u;
  std::uint32_t bytes = 64u;
  std::uint32_t processId = 0u;
  std::uint32_t reserved = 0u;
  std::uint64_t processCreation = 0u;
  std::array<std::uint64_t, 2> nonce{};
  std::uint64_t issuedMs = 0u;
  std::uint64_t expiresMs = 0u;
};
static_assert(std::is_trivially_copyable_v<RenderTapeArmRequest>);
static_assert(sizeof(RenderTapeArmRequest) == 64u);
static_assert(offsetof(RenderTapeArmRequest, nonce) == 32u);
static_assert(offsetof(RenderTapeArmRequest, expiresMs) == 56u);

inline bool parseRenderTapeArmNonce(std::string_view text,
                                  std::array<std::uint64_t, 2>& nonce) noexcept {
  if (text.size() != 32u) return false;
  std::array<std::uint64_t, 2> parsed{};
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    parsed[i / 16u] = (parsed[i / 16u] << 4u) |
        static_cast<unsigned>(c <= '9' ? c - '0' : c - 'a' + 10);
  }
  if (!(parsed[0] | parsed[1])) return false;
  nonce = parsed;
  return true;
}

inline bool renderTapeArmRequestPathValid(std::string_view path) noexcept {
  return path.size() > 3u && path.size() < 260u &&
      ((path[0] >= 'A' && path[0] <= 'Z') ||
       (path[0] >= 'a' && path[0] <= 'z')) &&
      path[1] == ':' && path[2] == '\\' &&
      path.find('\0') == std::string_view::npos;
}

enum class RenderTapeArmRequestState : std::uint8_t {
  Waiting, Consumed, Rejected,
};
enum class RenderTapeArmRequestInput : std::uint8_t {
  Missing, WriterBusy, Ready, Invalid,
};
enum class RenderTapeArmRequestDecision : std::uint8_t {
  Wait, Accept, Reject, Terminal,
};

struct RenderTapeArmRequestGate {
  static constexpr std::uint64_t kWaitMs = 600000u;
  static constexpr std::uint64_t kMaxRequestMs = 30000u;
  RenderTapeArmRequestState state = RenderTapeArmRequestState::Waiting;
  std::uint32_t processId = 0u;
  std::uint64_t processCreation = 0u;
  std::array<std::uint64_t, 2> nonce{};
  std::uint64_t readyMs = 0u;
  std::uint64_t lastMs = 0u;
  std::uint64_t acceptedMs = 0u;
  std::uint64_t expiresMs = 0u;

  // Consume before bootstrap. Abort/reset/publication failure cannot restore
  // Waiting; retrying requires a fresh process and a new external request.
  RenderTapeArmRequestDecision observe(RenderTapeArmRequestInput input,
      const RenderTapeArmRequest& request, std::uint64_t now) noexcept {
    using Decision = RenderTapeArmRequestDecision;
    if (state != RenderTapeArmRequestState::Waiting) return Decision::Terminal;
    if (!processId || !processCreation || !(nonce[0] | nonce[1]) ||
        now < readyMs || now < lastMs || now - readyMs >= kWaitMs ||
        input == RenderTapeArmRequestInput::Invalid) {
      state = RenderTapeArmRequestState::Rejected;
      return Decision::Reject;
    }
    lastMs = now;
    if (input == RenderTapeArmRequestInput::Missing ||
        input == RenderTapeArmRequestInput::WriterBusy) return Decision::Wait;
    const RenderTapeArmRequest schema{};
    if (input != RenderTapeArmRequestInput::Ready ||
        request.magic != schema.magic || request.version != schema.version ||
        request.bytes != sizeof(request) || request.reserved ||
        request.processId != processId || request.processCreation != processCreation ||
        request.nonce != nonce || request.issuedMs < readyMs || request.issuedMs > now ||
        request.expiresMs <= request.issuedMs || now >= request.expiresMs ||
        request.expiresMs - request.issuedMs > kMaxRequestMs) {
      state = RenderTapeArmRequestState::Rejected;
      return Decision::Reject;
    }
    acceptedMs = now;
    expiresMs = request.expiresMs;
    state = RenderTapeArmRequestState::Consumed;
    return Decision::Accept;
  }

  // Bootstrap can be slow. Expiry is checked again before identity arm;
  // admission within the window alone must not arm a much later interval.
  bool mayFinishBootstrap(std::uint64_t now) const noexcept {
    return state == RenderTapeArmRequestState::Consumed &&
        now >= acceptedMs && now < expiresMs;
  }
};

// Allocated only as part of the already cold PeCaptureState owner.
struct RenderTapeArmRequestSelector {
  bool initialized = false;
  bool enabled = false;
  std::array<char, 260> path{};
  RenderTapeArmRequestGate gate{};
};

} // namespace dxmt9::d3d9
