#pragma once

#include "device_c_render_tape_identity_v3.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <vector>

namespace dxmt9::d3d9 {

// D9RTSNV1: a value-snapshot transport, never an authenticated identity.bin.
inline constexpr std::uint64_t kRenderTapeIdentityV3TransferMagic =
    0x31564e5354523944ull;
inline constexpr std::uint32_t kRenderTapeIdentityV3TransferVersion = 1u;

struct RenderTapeIdentityV3TransferHeader {
  std::uint64_t magic = kRenderTapeIdentityV3TransferMagic;
  std::uint64_t totalBytes = 0u;
  std::uint32_t version = kRenderTapeIdentityV3TransferVersion;
  std::uint32_t headerSize = sizeof(RenderTapeIdentityV3TransferHeader);
  std::uint32_t reserved0 = 0u;
  std::uint32_t reserved1 = 0u;
  // Semantic values remain witnessed exactly. Only the six table descriptors
  // are rebuilt from owned vectors; event digest/length must remain unbound.
  RenderTapeIdentityV3Header identity{};
};

static_assert(sizeof(RenderTapeIdentityV3TransferHeader) == 272u);
static_assert(offsetof(RenderTapeIdentityV3TransferHeader, identity) == 32u);
static_assert(std::is_trivially_copyable_v<RenderTapeIdentityV3TransferHeader>);
static_assert(std::is_standard_layout_v<RenderTapeIdentityV3TransferHeader>);
static_assert(kRenderTapeIdentityV3TransferMagic != kRenderTapeIdentityMagic);

enum class RenderTapeIdentityV3TransferStatus : std::uint8_t {
  Valid,
  InvalidHeader,
  InvalidLayout,
  CaptureMismatch,
  CapacityExceeded,
  AllocationFailed,
};

// Both calls invalidate out at entry and expose no partial output on failure.
// Incoming View table descriptors are not trusted; owned table extents are.
// Structural transfer admission grants no publication/completion authority.
RenderTapeIdentityV3TransferStatus encodeRenderTapeIdentityV3SnapshotTransfer(
    const RenderTapeIdentityV3View& snapshot,
    std::vector<std::byte>& out) noexcept;

// expectedCapture comes from the capture's independent trusted start receipt.
// Full tape/source/lifecycle validation belongs to attachCaptureIdentityV3.
RenderTapeIdentityV3TransferStatus decodeRenderTapeIdentityV3SnapshotTransfer(
    std::span<const std::byte> bytes,
    RenderTapeIdentityV3CaptureKey expectedCapture,
    RenderTapeIdentityV3View& out) noexcept;

const char* renderTapeIdentityV3TransferStatusName(
    RenderTapeIdentityV3TransferStatus status) noexcept;

} // namespace dxmt9::d3d9
