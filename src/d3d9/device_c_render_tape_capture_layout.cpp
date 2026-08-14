#include "device_c_render_tape_capture_layout.hpp"

#include <cstring>
#include <limits>

namespace dxmt9::d3d9 {
namespace {

std::uint32_t blockBytes(std::uint32_t format) noexcept {
  switch (format) {
  case render_tape_d3d_format::DXT1:
    return 8u;
  case render_tape_d3d_format::DXT3:
  case render_tape_d3d_format::DXT5:
    return 16u;
  default:
    return 0u;
  }
}

} // namespace

std::uint32_t renderTapeLinearBytesPerPixel(std::uint32_t format) noexcept {
  switch (format) {
  case render_tape_d3d_format::A8R8G8B8:
  case render_tape_d3d_format::X8R8G8B8:
  case render_tape_d3d_format::A8B8G8R8:
  case render_tape_d3d_format::X8B8G8R8:
    return 4u;
  case render_tape_d3d_format::R8G8B8:
    return 3u;
  case render_tape_d3d_format::A1R5G5B5:
  case render_tape_d3d_format::X1R5G5B5:
  case render_tape_d3d_format::R5G6B5:
  case render_tape_d3d_format::A4R4G4B4:
  case render_tape_d3d_format::X4R4G4B4:
  case render_tape_d3d_format::A8L8:
  case render_tape_d3d_format::V8U8:
  case render_tape_d3d_format::A8P8:
  case render_tape_d3d_format::L16:
    return 2u;
  case render_tape_d3d_format::A8:
  case render_tape_d3d_format::L8:
  case render_tape_d3d_format::P8:
    return 1u;
  default:
    return 0u;
  }
}

namespace {

bool checkedMul(std::uint64_t a, std::uint64_t b,
                std::uint64_t& out) noexcept {
  if (a != 0u && b > std::numeric_limits<std::uint64_t>::max() / a)
    return false;
  out = a * b;
  return true;
}

} // namespace

const char* renderTapeCaptureRejectionReasonName(
    RenderTapeCaptureRejectionReason reason) noexcept {
  switch (reason) {
  case RenderTapeCaptureRejectionReason::InvalidLockIdentity:
    return "invalid_lock_identity";
  case RenderTapeCaptureRejectionReason::UnsupportedLockFormat:
    return "unsupported_lock_format";
  case RenderTapeCaptureRejectionReason::InvalidBlockAlignment:
    return "invalid_block_alignment";
  case RenderTapeCaptureRejectionReason::InvalidLockPitch:
    return "invalid_lock_pitch";
  case RenderTapeCaptureRejectionReason::LockLayoutOverflow:
    return "lock_layout_overflow";
  case RenderTapeCaptureRejectionReason::LockCopyFailed:
    return "lock_copy_failed";
  case RenderTapeCaptureRejectionReason::IncompleteSubresourceSeed:
    return "incomplete_subresource_seed";
  case RenderTapeCaptureRejectionReason::DescriptorMismatch:
    return "descriptor_mismatch";
  case RenderTapeCaptureRejectionReason::FullSnapshotLockFailed:
    return "full_snapshot_lock_failed";
  case RenderTapeCaptureRejectionReason::FullSnapshotCopyFailed:
    return "full_snapshot_copy_failed";
  case RenderTapeCaptureRejectionReason::FullSnapshotUnlockFailed:
    return "full_snapshot_unlock_failed";
  case RenderTapeCaptureRejectionReason::FullSnapshotIdentityMismatch:
    return "full_snapshot_identity_mismatch";
  case RenderTapeCaptureRejectionReason::FullSnapshotExtentMismatch:
    return "full_snapshot_extent_mismatch";
  }
  return "unknown_capture_rejection";
}

RenderTapeFullSnapshotStatus renderTapeClassifySnapshot(
    bool captureTrackingEnabled, bool identityMatches, bool extentMatches,
    bool partialLock, std::size_t existingContentBytes,
    std::uint64_t expectedBytes) noexcept {
  if (!captureTrackingEnabled)
    return RenderTapeFullSnapshotStatus::NotRequired;
  if (!identityMatches)
    return RenderTapeFullSnapshotStatus::InvalidIdentity;
  if (!extentMatches || expectedBytes == 0u)
    return RenderTapeFullSnapshotStatus::InvalidExtent;
  if (!partialLock || existingContentBytes == expectedBytes)
    return RenderTapeFullSnapshotStatus::NotRequired;
  if (existingContentBytes != 0u)
    return RenderTapeFullSnapshotStatus::InvalidExtent;
  return RenderTapeFullSnapshotStatus::Required;
}

RenderTapeFullSnapshotStatus renderTapeValidateFullSnapshot(
    bool fullSubresource, std::uint64_t expectedBytes,
    std::span<const std::byte> bytes) noexcept {
  if (!fullSubresource || expectedBytes == 0u)
    return RenderTapeFullSnapshotStatus::InvalidExtent;
  if (expectedBytes > std::numeric_limits<std::size_t>::max() ||
      bytes.size() != expectedBytes)
    return RenderTapeFullSnapshotStatus::InvalidBytes;
  return RenderTapeFullSnapshotStatus::Accepted;
}

RenderTapeBlockLayoutStatus renderTapeBlockLockLayout(
    const D9CSurfaceDesc& desc, std::int32_t pitch,
    const RenderTapeLockRect* rect, RenderTapeBlockLockLayout& out) noexcept {
  out = {};
  const std::uint32_t bytesPerBlock = blockBytes(desc.format);
  if (bytesPerBlock == 0u)
    return RenderTapeBlockLayoutStatus::UnsupportedFormat;
  if (desc.width == 0u || desc.height == 0u)
    return RenderTapeBlockLayoutStatus::InvalidExtent;
  if (desc.width > static_cast<std::uint32_t>(
                       std::numeric_limits<std::int32_t>::max()) ||
      desc.height > static_cast<std::uint32_t>(
                        std::numeric_limits<std::int32_t>::max()))
    return RenderTapeBlockLayoutStatus::Overflow;
  if (pitch <= 0)
    return RenderTapeBlockLayoutStatus::InvalidPitch;

  const std::int32_t left = rect ? rect->left : 0;
  const std::int32_t top = rect ? rect->top : 0;
  const std::int32_t right = rect ? rect->right
                                  : static_cast<std::int32_t>(desc.width);
  const std::int32_t bottom = rect ? rect->bottom
                                   : static_cast<std::int32_t>(desc.height);
  if (left < 0 || top < 0 || right <= left || bottom <= top ||
      static_cast<std::uint32_t>(right) > desc.width ||
      static_cast<std::uint32_t>(bottom) > desc.height) {
    return RenderTapeBlockLayoutStatus::InvalidExtent;
  }
  if ((left & 3) != 0 || (top & 3) != 0 ||
      ((right & 3) != 0 && static_cast<std::uint32_t>(right) != desc.width) ||
      ((bottom & 3) != 0 &&
       static_cast<std::uint32_t>(bottom) != desc.height)) {
    return RenderTapeBlockLayoutStatus::InvalidAlignment;
  }

  const std::uint64_t fullBlocksWide = (desc.width + 3u) / 4u;
  const std::uint64_t fullRows = (desc.height + 3u) / 4u;
  const std::uint64_t firstBlock = static_cast<std::uint32_t>(left) / 4u;
  const std::uint64_t firstRow = static_cast<std::uint32_t>(top) / 4u;
  const std::uint64_t lastBlock =
      (static_cast<std::uint32_t>(right) + 3u) / 4u;
  const std::uint64_t lastRow =
      (static_cast<std::uint32_t>(bottom) + 3u) / 4u;
  std::uint64_t fullRowBytes = 0u;
  std::uint64_t rowBytes = 0u;
  std::uint64_t tightBytes = 0u;
  std::uint64_t sourceBytes = 0u;
  if (!checkedMul(fullBlocksWide, bytesPerBlock, fullRowBytes) ||
      !checkedMul(lastBlock - firstBlock, bytesPerBlock, rowBytes) ||
      !checkedMul(rowBytes, lastRow - firstRow, tightBytes) ||
      !checkedMul(static_cast<std::uint64_t>(pitch),
                  lastRow - firstRow - 1u, sourceBytes) ||
      sourceBytes > std::numeric_limits<std::uint64_t>::max() - rowBytes ||
      fullRowBytes > std::numeric_limits<std::uint32_t>::max() ||
      fullRows > std::numeric_limits<std::uint32_t>::max() ||
      firstBlock > std::numeric_limits<std::uint32_t>::max() ||
      firstRow > std::numeric_limits<std::uint32_t>::max() ||
      rowBytes > std::numeric_limits<std::uint32_t>::max() ||
      lastRow - firstRow > std::numeric_limits<std::uint32_t>::max() ||
      tightBytes > std::numeric_limits<std::size_t>::max()) {
    return RenderTapeBlockLayoutStatus::Overflow;
  }
  sourceBytes += rowBytes;
  if (sourceBytes > std::numeric_limits<std::size_t>::max())
    return RenderTapeBlockLayoutStatus::Overflow;
  // D3D9 LockRect returns the full subresource row stride even for a
  // rectangle. Requiring that stride, rather than only the rectangle width,
  // keeps every subsequent source-row address inside the described layout.
  if (static_cast<std::uint64_t>(pitch) < fullRowBytes)
    return RenderTapeBlockLayoutStatus::InvalidPitch;

  out = RenderTapeBlockLockLayout{
      .blockBytes = bytesPerBlock,
      .fullRowBytes = static_cast<std::uint32_t>(fullRowBytes),
      .fullRows = static_cast<std::uint32_t>(fullRows),
      .blockLeft = static_cast<std::uint32_t>(firstBlock),
      .blockTop = static_cast<std::uint32_t>(firstRow),
      .rowBytes = static_cast<std::uint32_t>(rowBytes),
      .rows = static_cast<std::uint32_t>(lastRow - firstRow),
      .pitch = static_cast<std::uint32_t>(pitch),
      .tightBytes = tightBytes,
      .sourceBytes = sourceBytes,
      .fullSubresource = left == 0 && top == 0 &&
                         static_cast<std::uint32_t>(right) == desc.width &&
                         static_cast<std::uint32_t>(bottom) == desc.height,
  };
  return RenderTapeBlockLayoutStatus::Accepted;
}

bool copyRenderTapeBlockRows(const void* bits,
                             const RenderTapeBlockLockLayout& layout,
                             std::vector<std::byte>& out) noexcept {
  std::uint64_t sourceBytes = 0u;
  if (!bits || layout.rowBytes == 0u || layout.rows == 0u ||
      layout.pitch < layout.rowBytes ||
      layout.tightBytes !=
          static_cast<std::uint64_t>(layout.rowBytes) * layout.rows ||
      layout.tightBytes > std::numeric_limits<std::size_t>::max() ||
      !checkedMul(layout.pitch, layout.rows - 1u, sourceBytes) ||
      sourceBytes > std::numeric_limits<std::uint64_t>::max() -
                        layout.rowBytes ||
      sourceBytes + layout.rowBytes >
          std::numeric_limits<std::size_t>::max()) {
    return false;
  }
  sourceBytes += layout.rowBytes;
  if (layout.sourceBytes != sourceBytes)
    return false;
  try {
    out.resize(static_cast<std::size_t>(layout.tightBytes));
    const auto* source = static_cast<const std::byte*>(bits);
    for (std::uint32_t row = 0u; row < layout.rows; ++row) {
      std::memcpy(out.data() + static_cast<std::size_t>(row) * layout.rowBytes,
                  source + static_cast<std::size_t>(row) * layout.pitch,
                  layout.rowBytes);
    }
    return true;
  } catch (...) {
    out.clear();
    return false;
  }
}

RenderTapeBlockMutationStatus applyRenderTapeBlockMutation(
    const RenderTapeBlockLockLayout& layout, std::span<const std::byte> bytes,
    std::vector<std::byte>& completeContent) noexcept {
  std::uint64_t contentBytes = 0u;
  const std::uint64_t destinationColumn =
      static_cast<std::uint64_t>(layout.blockLeft) * layout.blockBytes;
  if (layout.blockBytes == 0u || layout.fullRowBytes == 0u ||
      layout.fullRows == 0u || layout.rowBytes == 0u || layout.rows == 0u ||
      destinationColumn > layout.fullRowBytes ||
      layout.blockTop > layout.fullRows ||
      layout.rows > layout.fullRows - layout.blockTop ||
      layout.rowBytes > layout.fullRowBytes - destinationColumn) {
    return RenderTapeBlockMutationStatus::InvalidLayout;
  }
  if (!checkedMul(layout.fullRowBytes, layout.fullRows, contentBytes) ||
      contentBytes > std::numeric_limits<std::size_t>::max()) {
    return RenderTapeBlockMutationStatus::Overflow;
  }
  if (bytes.size() != layout.tightBytes ||
      layout.tightBytes !=
          static_cast<std::uint64_t>(layout.rowBytes) * layout.rows) {
    return RenderTapeBlockMutationStatus::InvalidBytes;
  }

  if (layout.fullSubresource) {
    if (layout.blockLeft != 0u || layout.blockTop != 0u ||
        layout.rowBytes != layout.fullRowBytes ||
        layout.rows != layout.fullRows || bytes.size() != contentBytes) {
      return RenderTapeBlockMutationStatus::InvalidLayout;
    }
    try {
      completeContent.assign(bytes.begin(), bytes.end());
      return RenderTapeBlockMutationStatus::Accepted;
    } catch (...) {
      return RenderTapeBlockMutationStatus::AllocationFailed;
    }
  }

  if (completeContent.size() != contentBytes)
    return RenderTapeBlockMutationStatus::IncompleteSeed;
  for (std::uint32_t row = 0u; row < layout.rows; ++row) {
    const auto destinationOffset =
        static_cast<std::size_t>(layout.blockTop + row) * layout.fullRowBytes +
        static_cast<std::size_t>(layout.blockLeft) * layout.blockBytes;
    std::memcpy(completeContent.data() + destinationOffset,
                bytes.data() + static_cast<std::size_t>(row) * layout.rowBytes,
                layout.rowBytes);
  }
  return RenderTapeBlockMutationStatus::Accepted;
}

RenderTapeLinearLayoutStatus renderTapeLinearLockLayout(
    const D9CSurfaceDesc& desc, std::int32_t pitch,
    const RenderTapeLockRect* rect, RenderTapeLinearLockLayout& out) noexcept {
  out = {};
  const std::uint32_t bytesPerPixel =
      renderTapeLinearBytesPerPixel(desc.format);
  if (bytesPerPixel == 0u)
    return RenderTapeLinearLayoutStatus::UnsupportedFormat;
  if (desc.width == 0u || desc.height == 0u)
    return RenderTapeLinearLayoutStatus::InvalidExtent;
  if (desc.width > static_cast<std::uint32_t>(
                       std::numeric_limits<std::int32_t>::max()) ||
      desc.height > static_cast<std::uint32_t>(
                        std::numeric_limits<std::int32_t>::max()))
    return RenderTapeLinearLayoutStatus::Overflow;
  if (pitch <= 0)
    return RenderTapeLinearLayoutStatus::InvalidPitch;

  const std::int32_t left = rect ? rect->left : 0;
  const std::int32_t top = rect ? rect->top : 0;
  const std::int32_t right = rect ? rect->right
                                  : static_cast<std::int32_t>(desc.width);
  const std::int32_t bottom = rect ? rect->bottom
                                   : static_cast<std::int32_t>(desc.height);
  if (left < 0 || top < 0 || right <= left || bottom <= top ||
      static_cast<std::uint32_t>(right) > desc.width ||
      static_cast<std::uint32_t>(bottom) > desc.height)
    return RenderTapeLinearLayoutStatus::InvalidExtent;

  std::uint64_t fullRowBytes = 0u;
  std::uint64_t destinationByteOffset = 0u;
  std::uint64_t rowBytes = 0u;
  std::uint64_t tightBytes = 0u;
  std::uint64_t sourceBytes = 0u;
  std::uint64_t subresourceSourceOffset = 0u;
  std::uint64_t subresourceSourceBytes = 0u;
  const auto rows = static_cast<std::uint32_t>(bottom - top);
  if (!checkedMul(desc.width, bytesPerPixel, fullRowBytes) ||
      !checkedMul(static_cast<std::uint32_t>(left), bytesPerPixel,
                  destinationByteOffset) ||
      !checkedMul(static_cast<std::uint32_t>(right - left), bytesPerPixel,
                  rowBytes) ||
      !checkedMul(rowBytes, rows, tightBytes) ||
      !checkedMul(static_cast<std::uint64_t>(pitch), rows - 1u,
                  sourceBytes) ||
      sourceBytes > std::numeric_limits<std::uint64_t>::max() - rowBytes ||
      !checkedMul(static_cast<std::uint32_t>(top),
                  static_cast<std::uint64_t>(pitch),
                  subresourceSourceOffset) ||
      subresourceSourceOffset >
          std::numeric_limits<std::uint64_t>::max() - destinationByteOffset ||
      fullRowBytes > std::numeric_limits<std::uint32_t>::max() ||
      destinationByteOffset > std::numeric_limits<std::uint32_t>::max() ||
      rowBytes > std::numeric_limits<std::uint32_t>::max() ||
      tightBytes > std::numeric_limits<std::size_t>::max())
    return RenderTapeLinearLayoutStatus::Overflow;
  sourceBytes += rowBytes;
  subresourceSourceOffset += destinationByteOffset;
  if (subresourceSourceOffset >
      std::numeric_limits<std::uint64_t>::max() - sourceBytes)
    return RenderTapeLinearLayoutStatus::Overflow;
  subresourceSourceBytes = subresourceSourceOffset + sourceBytes;
  if (sourceBytes > std::numeric_limits<std::size_t>::max() ||
      subresourceSourceOffset > std::numeric_limits<std::size_t>::max() ||
      subresourceSourceBytes > std::numeric_limits<std::size_t>::max())
    return RenderTapeLinearLayoutStatus::Overflow;
  if (static_cast<std::uint64_t>(pitch) < fullRowBytes)
    return RenderTapeLinearLayoutStatus::InvalidPitch;

  out = RenderTapeLinearLockLayout{
      .bytesPerPixel = bytesPerPixel,
      .fullRowBytes = static_cast<std::uint32_t>(fullRowBytes),
      .fullRows = desc.height,
      .destinationByteOffset =
          static_cast<std::uint32_t>(destinationByteOffset),
      .top = static_cast<std::uint32_t>(top),
      .rowBytes = static_cast<std::uint32_t>(rowBytes),
      .rows = rows,
      .pitch = static_cast<std::uint32_t>(pitch),
      .tightBytes = tightBytes,
      .sourceBytes = sourceBytes,
      .subresourceSourceOffset = subresourceSourceOffset,
      .subresourceSourceBytes = subresourceSourceBytes,
      .fullSubresource = left == 0 && top == 0 &&
                         static_cast<std::uint32_t>(right) == desc.width &&
                         static_cast<std::uint32_t>(bottom) == desc.height,
  };
  return RenderTapeLinearLayoutStatus::Accepted;
}

bool copyRenderTapeLinearRows(const void* bits,
                              const RenderTapeLinearLockLayout& layout,
                              std::vector<std::byte>& out,
                              RenderTapeLockBitsOrigin origin) noexcept {
  const std::uint64_t sourceOffset =
      origin == RenderTapeLockBitsOrigin::Subresource
          ? layout.subresourceSourceOffset
          : 0u;
  const std::uint64_t expectedSourceBytes =
      origin == RenderTapeLockBitsOrigin::Subresource
          ? layout.subresourceSourceBytes
          : layout.sourceBytes;
  std::uint64_t sourceBytes = 0u;
  if (!bits || layout.rowBytes == 0u || layout.rows == 0u ||
      layout.pitch < layout.rowBytes ||
      layout.tightBytes !=
          static_cast<std::uint64_t>(layout.rowBytes) * layout.rows ||
      layout.tightBytes > std::numeric_limits<std::size_t>::max() ||
      !checkedMul(layout.pitch, layout.rows - 1u, sourceBytes) ||
      sourceBytes > std::numeric_limits<std::uint64_t>::max() -
                        layout.rowBytes) {
    return false;
  }
  sourceBytes += layout.rowBytes;
  if (layout.sourceBytes != sourceBytes ||
      sourceOffset > std::numeric_limits<std::uint64_t>::max() - sourceBytes ||
      sourceOffset + sourceBytes != expectedSourceBytes ||
      expectedSourceBytes > std::numeric_limits<std::size_t>::max())
    return false;
  try {
    out.resize(static_cast<std::size_t>(layout.tightBytes));
    const auto* source = static_cast<const std::byte*>(bits) +
                         static_cast<std::size_t>(sourceOffset);
    for (std::uint32_t row = 0u; row < layout.rows; ++row) {
      std::memcpy(out.data() + static_cast<std::size_t>(row) * layout.rowBytes,
                  source + static_cast<std::size_t>(row) * layout.pitch,
                  layout.rowBytes);
    }
    return true;
  } catch (...) {
    out.clear();
    return false;
  }
}

bool writeRenderTapeLinearRows(std::span<const std::byte> tightBytes,
                               void* bits,
                               const RenderTapeLinearLockLayout& layout) noexcept {
  std::uint64_t destinationBytes = 0u;
  if (!bits || layout.rowBytes == 0u || layout.rows == 0u ||
      layout.pitch < layout.rowBytes ||
      tightBytes.size() != layout.tightBytes ||
      layout.tightBytes !=
          static_cast<std::uint64_t>(layout.rowBytes) * layout.rows ||
      !checkedMul(layout.pitch, layout.rows - 1u, destinationBytes) ||
      destinationBytes > std::numeric_limits<std::uint64_t>::max() -
                             layout.rowBytes) {
    return false;
  }
  destinationBytes += layout.rowBytes;
  if (destinationBytes != layout.sourceBytes ||
      destinationBytes > std::numeric_limits<std::size_t>::max())
    return false;
  auto* destination = static_cast<std::byte*>(bits);
  for (std::uint32_t row = 0u; row < layout.rows; ++row) {
    std::memcpy(destination + static_cast<std::size_t>(row) * layout.pitch,
                tightBytes.data() +
                    static_cast<std::size_t>(row) * layout.rowBytes,
                layout.rowBytes);
  }
  return true;
}

RenderTapeBlockMutationStatus applyRenderTapeLinearMutation(
    const RenderTapeLinearLockLayout& layout, std::span<const std::byte> bytes,
    std::vector<std::byte>& completeContent) noexcept {
  std::uint64_t contentBytes = 0u;
  if (layout.bytesPerPixel == 0u || layout.fullRowBytes == 0u ||
      layout.fullRows == 0u || layout.rowBytes == 0u || layout.rows == 0u ||
      layout.destinationByteOffset > layout.fullRowBytes ||
      layout.top > layout.fullRows ||
      layout.rows > layout.fullRows - layout.top ||
      layout.rowBytes >
          layout.fullRowBytes - layout.destinationByteOffset ||
      !checkedMul(layout.fullRowBytes, layout.fullRows, contentBytes) ||
      contentBytes > std::numeric_limits<std::size_t>::max() ||
      bytes.size() != layout.tightBytes ||
      layout.tightBytes !=
          static_cast<std::uint64_t>(layout.rowBytes) * layout.rows) {
    return RenderTapeBlockMutationStatus::InvalidLayout;
  }
  if (layout.fullSubresource) {
    if (layout.destinationByteOffset != 0u || layout.top != 0u ||
        layout.rowBytes != layout.fullRowBytes ||
        layout.rows != layout.fullRows || bytes.size() != contentBytes) {
      return RenderTapeBlockMutationStatus::InvalidLayout;
    }
    try {
      completeContent.assign(bytes.begin(), bytes.end());
      return RenderTapeBlockMutationStatus::Accepted;
    } catch (...) {
      return RenderTapeBlockMutationStatus::AllocationFailed;
    }
  }
  if (completeContent.size() != contentBytes)
    return RenderTapeBlockMutationStatus::IncompleteSeed;
  for (std::uint32_t row = 0u; row < layout.rows; ++row) {
    const auto destinationOffset =
        static_cast<std::size_t>(layout.top + row) * layout.fullRowBytes +
        layout.destinationByteOffset;
    std::memcpy(completeContent.data() + destinationOffset,
                bytes.data() + static_cast<std::size_t>(row) * layout.rowBytes,
                layout.rowBytes);
  }
  return RenderTapeBlockMutationStatus::Accepted;
}

} // namespace dxmt9::d3d9
