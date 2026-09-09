#pragma once

#include "dxmt9/core_snapshots.hpp"

#include <cstddef>
#include <limits>

namespace dxmt9::detail {

// Binding capture only replaces an empty snapshot field with one owned
// DrawBindingSnapshot. All other payload extents are preserved, so the final
// extent is the prepared extent plus this checked vector-size delta. A max
// input is the existing overflow sentinel and must remain visible.
constexpr std::size_t drawRunPayloadBytesAfterBindingSnapshotCapture(
    std::size_t preparedBytes, std::size_t snapshotsBefore,
    std::size_t snapshotsAfter) noexcept {
  constexpr std::size_t kMax = std::numeric_limits<std::size_t>::max();
  constexpr std::size_t kSnapshotBytes = sizeof(core::DrawBindingSnapshot);
  if (preparedBytes == kMax || snapshotsAfter < snapshotsBefore) {
    return kMax;
  }

  const std::size_t addedSnapshots = snapshotsAfter - snapshotsBefore;
  if (addedSnapshots > kMax / kSnapshotBytes) {
    return kMax;
  }
  const std::size_t addedBytes = addedSnapshots * kSnapshotBytes;
  if (addedBytes > kMax - preparedBytes) {
    return kMax;
  }
  return preparedBytes + addedBytes;
}

}  // namespace dxmt9::detail
