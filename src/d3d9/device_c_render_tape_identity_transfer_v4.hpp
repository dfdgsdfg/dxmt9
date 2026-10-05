#pragma once

#include "device_c_render_tape_action_witness.hpp"
#include "device_c_render_tape_execution_topology_v1.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <vector>

namespace dxmt9::d3d9 {

// D9RTSNV3: one capture's neutral identity.v4 + execution_topology.v1 and
// explicit queue-observed action witness. Transport versioning is independent
// of either final sidecar. D9RTSNV2 remains an explicitly legacy-only input.
inline constexpr std::uint64_t kRenderTapeIdentityV4TransferMagic =
    0x33564e5354523944ull;
inline constexpr std::uint32_t kRenderTapeIdentityV4TransferVersion = 3u;
inline constexpr std::uint64_t kRenderTapeIdentityV4TransferLegacyMagic =
    0x32564e5354523944ull;
inline constexpr std::uint32_t kRenderTapeIdentityV4TransferLegacyVersion = 2u;

// Retained V2 grammar, with canonical tables starting at 448. No reserved
// byte or table data is reinterpreted as an action observation.
struct RenderTapeIdentityV4TransferLegacyHeader {
  std::uint64_t magic = kRenderTapeIdentityV4TransferLegacyMagic;
  std::uint64_t totalBytes = 0u;
  std::uint32_t version = kRenderTapeIdentityV4TransferLegacyVersion;
  std::uint32_t headerSize = sizeof(RenderTapeIdentityV4TransferLegacyHeader);
  std::uint32_t reserved0 = 0u;
  std::uint32_t reserved1 = 0u;
  RenderTapeIdentityV4Header identity{};
  RenderTapeExecutionTopologyV1Header topology{};
};

// 32-byte envelope + 264-byte identity + 152-byte topology + 64-byte witness.
// Descriptors address one canonical contiguous layout: seven identity tables,
// then accepted plans and placements. Only their extents are rebuilt; all
// other witnessed values are preserved, with byte/digest authority unbound.
struct RenderTapeIdentityV4TransferHeader {
  std::uint64_t magic = kRenderTapeIdentityV4TransferMagic;
  std::uint64_t totalBytes = 0u;
  std::uint32_t version = kRenderTapeIdentityV4TransferVersion;
  std::uint32_t headerSize = sizeof(RenderTapeIdentityV4TransferHeader);
  std::uint32_t reserved0 = 0u;
  std::uint32_t reserved1 = 0u;
  RenderTapeIdentityV4Header identity{};
  RenderTapeExecutionTopologyV1Header topology{};
  RenderTapeActionWitnessV1 actionWitness{};
};

static_assert(sizeof(RenderTapeIdentityV4TransferLegacyHeader) == 448u);
static_assert(sizeof(RenderTapeIdentityV4TransferHeader) == 512u);
static_assert(offsetof(RenderTapeIdentityV4TransferHeader, identity) == 32u);
static_assert(offsetof(RenderTapeIdentityV4TransferHeader, topology) == 296u);
static_assert(offsetof(RenderTapeIdentityV4TransferHeader, actionWitness) == 448u);
static_assert(offsetof(RenderTapeIdentityV4TransferLegacyHeader, identity) == 32u);
static_assert(offsetof(RenderTapeIdentityV4TransferLegacyHeader, topology) == 296u);
static_assert(std::is_trivially_copyable_v<RenderTapeIdentityV4TransferLegacyHeader>);
static_assert(std::is_standard_layout_v<RenderTapeIdentityV4TransferLegacyHeader>);
static_assert(std::is_trivially_copyable_v<RenderTapeIdentityV4TransferHeader>);
static_assert(std::is_standard_layout_v<RenderTapeIdentityV4TransferHeader>);
static_assert(kRenderTapeIdentityV4TransferMagic != kRenderTapeIdentityMagic);
static_assert(kRenderTapeIdentityV4TransferMagic != kRenderTapeExecutionTopologyV1Magic);
static_assert(kRenderTapeIdentityV4TransferMagic != kRenderTapeIdentityV4TransferLegacyMagic);

enum class RenderTapeIdentityV4TransferStatus : std::uint8_t {
  Valid,
  InvalidHeader,
  InvalidLayout,
  CaptureMismatch,
  CapacityExceeded,
  AllocationFailed,
};

// Structural transfer success is not provenance, lifecycle, query-result or
// Metal execution qualification. PE must validate both against its real sealed
// Tape before binding byte lengths/digests and publishing final sidecars.
// Owned vectors, not incoming sidecar descriptors, define export extents.
RenderTapeIdentityV4TransferStatus encodeRenderTapeIdentityV4SnapshotTransfer(
    const RenderTapeIdentityV4View& identity,
    const RenderTapeExecutionTopologyV1View& topology,
    std::vector<std::byte>& out) noexcept;

RenderTapeIdentityV4TransferStatus encodeRenderTapeIdentityV4SnapshotTransfer(
    const RenderTapeIdentityV4View& identity,
    const RenderTapeExecutionTopologyV1View& topology,
    const RenderTapeActionWitnessV1& actionWitness,
    std::vector<std::byte>& out) noexcept;

// The expected token/epoch and queue come from independent trusted capture
// state and bind a QueueObserved witness too. All outputs are cleared at entry;
// failures expose no partial pair/witness. Legacy V2 decodes as LegacyUnknown.
RenderTapeIdentityV4TransferStatus decodeRenderTapeIdentityV4SnapshotTransfer(
    std::span<const std::byte> bytes,
    RenderTapeIdentityV4CaptureKey expectedCapture,
    std::uint64_t expectedQueue,
    RenderTapeIdentityV4View& identityOut,
    RenderTapeExecutionTopologyV1View& topologyOut,
    RenderTapeActionWitnessV1* witnessOut = nullptr) noexcept;

const char* renderTapeIdentityV4TransferStatusName(
    RenderTapeIdentityV4TransferStatus status) noexcept;

} // namespace dxmt9::d3d9
