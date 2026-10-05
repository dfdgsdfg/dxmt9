#pragma once

#include "device_c_render_tape.hpp"
#include "device_c_render_tape_action_witness.hpp"

#include <bit>
#include <cstring>
#include <type_traits>

namespace dxmt9::d3d9 {

inline constexpr std::uint64_t kRenderTapeActionProfileV1Magic = 0x31504152544d5844ull;
inline constexpr std::uint32_t kRenderTapeActionProfileV1Version = 1u;
inline constexpr std::size_t kRenderTapeActionProfileV1Bytes = 200u;
inline constexpr char kRenderTapeActionProfileV1Schema[] =
    "dxmt9.render_tape.action_profile.v1";

// Fixed little-endian capture evidence. The component binds the observed
// queue policy to the exact published triple, not manifest qualification flags.
struct RenderTapeActionProfileV1ComponentHeader {
  std::uint64_t magic = kRenderTapeActionProfileV1Magic;
  std::uint32_t version = kRenderTapeActionProfileV1Version;
  std::uint32_t headerSize = kRenderTapeActionProfileV1Bytes;
  RenderTapeActionWitnessV1 witness{};
  std::uint64_t eventsBytes = 0u;
  std::uint64_t identityBytes = 0u;
  std::uint64_t executionTopologyBytes = 0u;
  RenderTapeDigest eventsDigest{};
  RenderTapeDigest identityDigest{};
  RenderTapeDigest executionTopologyDigest{};
};

static_assert(std::endian::native == std::endian::little);
static_assert(sizeof(RenderTapeActionProfileV1ComponentHeader) == kRenderTapeActionProfileV1Bytes);
static_assert(offsetof(RenderTapeActionProfileV1ComponentHeader, version) == 8u);
static_assert(offsetof(RenderTapeActionProfileV1ComponentHeader, headerSize) == 12u);
static_assert(offsetof(RenderTapeActionProfileV1ComponentHeader, witness) == 16u);
static_assert(offsetof(RenderTapeActionProfileV1ComponentHeader, eventsBytes) == 80u);
static_assert(offsetof(RenderTapeActionProfileV1ComponentHeader, identityBytes) == 88u);
static_assert(offsetof(RenderTapeActionProfileV1ComponentHeader, executionTopologyBytes) == 96u);
static_assert(offsetof(RenderTapeActionProfileV1ComponentHeader, eventsDigest) == 104u);
static_assert(offsetof(RenderTapeActionProfileV1ComponentHeader, identityDigest) == 136u);
static_assert(offsetof(RenderTapeActionProfileV1ComponentHeader, executionTopologyDigest) == 168u);
static_assert(std::is_trivially_copyable_v<RenderTapeActionProfileV1ComponentHeader>);
static_assert(std::is_standard_layout_v<RenderTapeActionProfileV1ComponentHeader>);

inline constexpr bool renderTapeActionProfileV1ComponentValid(
    const RenderTapeActionProfileV1ComponentHeader& value) noexcept {
  return value.magic == kRenderTapeActionProfileV1Magic &&
      value.version == kRenderTapeActionProfileV1Version &&
      value.headerSize == kRenderTapeActionProfileV1Bytes &&
      renderTapeActionWitnessV1Valid(value.witness) &&
      value.witness.evidence == static_cast<std::uint32_t>(
          RenderTapeActionWitnessEvidenceV1::QueueObserved) &&
      value.eventsBytes && value.identityBytes && value.executionTopologyBytes;
}

// Callers derive these sizes and SHA-256 values from actual component bytes.
// This pure join cannot authenticate a caller's manifest/environment claims.
inline constexpr bool renderTapeActionProfileV1ComponentMatchesBinding(
    const RenderTapeActionProfileV1ComponentHeader& value,
    std::uint64_t token, std::uint64_t epoch, std::uint64_t queue,
    std::uint64_t eventsBytes, std::uint64_t identityBytes,
    std::uint64_t executionTopologyBytes, const RenderTapeDigest& eventsDigest,
    const RenderTapeDigest& identityDigest,
    const RenderTapeDigest& executionTopologyDigest) noexcept {
  return renderTapeActionProfileV1ComponentValid(value) &&
      renderTapeActionWitnessV1MatchesBinding(value.witness, token, epoch, queue) &&
      value.eventsBytes == eventsBytes && value.identityBytes == identityBytes &&
      value.executionTopologyBytes == executionTopologyBytes &&
      value.eventsDigest == eventsDigest && value.identityDigest == identityDigest &&
      value.executionTopologyDigest == executionTopologyDigest;
}

inline bool importRenderTapeActionProfileV1Component(
    std::span<const std::byte> bytes,
    RenderTapeActionProfileV1ComponentHeader& out) noexcept {
  out = {};
  if (bytes.size() != kRenderTapeActionProfileV1Bytes) return false;
  RenderTapeActionProfileV1ComponentHeader candidate;
  std::memcpy(&candidate, bytes.data(), sizeof(candidate));
  if (!renderTapeActionProfileV1ComponentValid(candidate)) return false;
  out = candidate;
  return true;
}

inline bool encodeRenderTapeActionProfileV1Component(
    const RenderTapeActionProfileV1ComponentHeader& value,
    std::array<std::byte, kRenderTapeActionProfileV1Bytes>& out) noexcept {
  out = {};
  if (!renderTapeActionProfileV1ComponentValid(value)) return false;
  std::memcpy(out.data(), &value, sizeof(value));
  return true;
}

} // namespace dxmt9::d3d9
