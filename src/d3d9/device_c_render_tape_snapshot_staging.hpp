#pragma once

#include "device_c_render_tape_snapshot_budget.hpp"

#include <cstdint>
#include <limits>

namespace dxmt9::d3d9 {

enum class RenderTapeSnapshotStagingFamily : std::uint8_t {
  Color,
  D24X8,
  D24S8,
};

struct RenderTapeSnapshotStagingPlan {
  std::uint64_t logicalRetainedBytes = 0u;
  std::uint64_t extraHostPayloadBytes = 0u;
};

// Arm snapshot admission only. Color has simultaneously live native/tight
// vectors; depth currently has one canonical vector but reserves the same
// conservative 2*N host-payload allowance. Shared Metal buffers, texture
// storage/padding, allocator overhead and driver RSS are not host vectors.
// The PE owner separately admits retained/aggregate/transient leases before
// crossing the bridge; no such aggregate authority is inferred here.
[[nodiscard]] inline constexpr bool renderTapeDeriveSnapshotStagingPlan(
    RenderTapeSnapshotStagingFamily family, std::uint64_t logicalBytes,
    RenderTapeSnapshotStagingPlan& out) noexcept {
  out = {};
  if ((family != RenderTapeSnapshotStagingFamily::Color &&
       family != RenderTapeSnapshotStagingFamily::D24X8 &&
       family != RenderTapeSnapshotStagingFamily::D24S8) ||
      logicalBytes == 0u ||
      logicalBytes > std::numeric_limits<std::uint64_t>::max() / 2u) {
    return false;
  }
  const auto extraHostBytes = logicalBytes * 2u;
  if (logicalBytes > kRenderTapeSnapshotPerResourceHardLimitBytes ||
      extraHostBytes > kRenderTapeSnapshotTransientHardLimitBytes) {
    return false;
  }
  out = {.logicalRetainedBytes = logicalBytes,
         .extraHostPayloadBytes = extraHostBytes};
  return true;
}

} // namespace dxmt9::d3d9
