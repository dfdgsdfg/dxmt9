#include "device_c_render_tape_capture_layout.hpp"

#include "device_c_render_tape.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>

namespace dxmt9::d3d9 {
namespace {

bool checkedAdd(std::uint64_t a, std::uint64_t b,
                std::uint64_t& out) noexcept {
  if (a > std::numeric_limits<std::uint64_t>::max() - b)
    return false;
  out = a + b;
  return true;
}

bool checkedMul(std::uint64_t a, std::uint64_t b,
                std::uint64_t& out) noexcept {
  if (a != 0u && b > std::numeric_limits<std::uint64_t>::max() / a)
    return false;
  out = a * b;
  return true;
}

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

RenderTapeExpectedContentContract expectedContentFailure(
    RenderTapeExpectedContentStatus status) noexcept {
  return RenderTapeExpectedContentContract{.status = status};
}

RenderTapeExpectedContentContract expectedContentForSurface(
    const D9CSurfaceDesc& desc) noexcept {
  if (desc.width == 0u || desc.height == 0u || desc.depth != 1u)
    return expectedContentFailure(RenderTapeExpectedContentStatus::InvalidExtent);

  const auto blockSize = blockBytes(desc.format);
  const auto bytesPerPixel = renderTapeLinearBytesPerPixel(desc.format);
  if (blockSize == 0u && bytesPerPixel == 0u)
    return expectedContentFailure(RenderTapeExpectedContentStatus::UnsupportedFormat);

  std::uint64_t fullRowBytes = 0u;
  if (blockSize != 0u) {
    if (!checkedMul((static_cast<std::uint64_t>(desc.width) + 3u) / 4u,
                    blockSize, fullRowBytes) ||
        fullRowBytes == 0u ||
        fullRowBytes > static_cast<std::uint64_t>(
                            std::numeric_limits<std::int32_t>::max()))
      return expectedContentFailure(RenderTapeExpectedContentStatus::Overflow);
    RenderTapeBlockLockLayout layout{};
    if (renderTapeBlockLockLayout(desc, static_cast<std::int32_t>(fullRowBytes),
                                  nullptr, layout) !=
        RenderTapeBlockLayoutStatus::Accepted)
      return expectedContentFailure(RenderTapeExpectedContentStatus::InvalidExtent);
    return {.status = RenderTapeExpectedContentStatus::Accepted,
            .bytes = layout.tightBytes,
            .count = 1u};
  }

  if (!checkedMul(desc.width, bytesPerPixel, fullRowBytes) ||
      fullRowBytes == 0u ||
      fullRowBytes > static_cast<std::uint64_t>(
                          std::numeric_limits<std::int32_t>::max()))
    return expectedContentFailure(RenderTapeExpectedContentStatus::Overflow);
  RenderTapeLinearLockLayout layout{};
  if (renderTapeLinearLockLayout(desc, static_cast<std::int32_t>(fullRowBytes),
                                 nullptr, layout) !=
      RenderTapeLinearLayoutStatus::Accepted)
    return expectedContentFailure(RenderTapeExpectedContentStatus::InvalidExtent);
  return {.status = RenderTapeExpectedContentStatus::Accepted,
          .bytes = layout.tightBytes,
          .count = 1u};
}

RenderTapeExpectedContentContract expectedContentForTexture(
    std::span<const std::byte> descriptor,
    std::span<std::uint64_t> subresourceBytes) noexcept {
  RenderTapeTextureDescriptorV2 texture{};
  if (!renderTapeLoadTextureDescriptorV2(descriptor, texture))
    return expectedContentFailure(RenderTapeExpectedContentStatus::InvalidDescriptor);
  const auto dimension = static_cast<RenderTapeTextureDimension>(texture.dimension);
  if (dimension == RenderTapeTextureDimension::Volume)
    return expectedContentFailure(RenderTapeExpectedContentStatus::UnsupportedDimension);
  if (texture.initialContentDisposition != static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::CompleteSeed))
    return expectedContentFailure(RenderTapeExpectedContentStatus::InvalidDescriptor);
  if (!subresourceBytes.empty() && subresourceBytes.size() < texture.subresourceCount)
    return expectedContentFailure(RenderTapeExpectedContentStatus::InvalidExtent);

  std::uint64_t totalBytes = 0u;
  for (std::uint32_t subresource = 0u;
       subresource < texture.subresourceCount; ++subresource) {
    D9CSurfaceDesc desc{};
    if (!renderTapeTextureSubresourceDescriptor(descriptor, subresource, desc))
      return expectedContentFailure(RenderTapeExpectedContentStatus::InvalidDescriptor);
    const auto content = expectedContentForSurface(desc);
    if (content.status != RenderTapeExpectedContentStatus::Accepted)
      return content;
    if (!subresourceBytes.empty())
      subresourceBytes[subresource] = content.bytes;
    if (!checkedAdd(totalBytes, content.bytes, totalBytes))
      return expectedContentFailure(RenderTapeExpectedContentStatus::Overflow);
  }
  return {.status = RenderTapeExpectedContentStatus::Accepted,
          .bytes = totalBytes,
          .count = texture.subresourceCount};
}

} // namespace

RenderTapeExpectedContentContract renderTapeDeriveExpectedSurfaceContent(
    const D9CSurfaceDesc& desc) noexcept {
  return expectedContentForSurface(desc);
}

const char* renderTapeExpectedContentStatusName(
    RenderTapeExpectedContentStatus status) noexcept {
  switch (status) {
  case RenderTapeExpectedContentStatus::NotRequired:
    return "not_required";
  case RenderTapeExpectedContentStatus::Accepted:
    return "accepted";
  case RenderTapeExpectedContentStatus::InvalidDescriptor:
    return "invalid_descriptor";
  case RenderTapeExpectedContentStatus::UnsupportedFormat:
    return "unsupported_format";
  case RenderTapeExpectedContentStatus::UnsupportedDimension:
    return "unsupported_dimension";
  case RenderTapeExpectedContentStatus::InvalidExtent:
    return "invalid_extent";
  case RenderTapeExpectedContentStatus::Overflow:
    return "overflow";
  }
  return "unknown";
}

const char* renderTapeMissingSeedDescriptorStatusName(
    RenderTapeMissingSeedDescriptorStatus status) noexcept {
  switch (status) {
  case RenderTapeMissingSeedDescriptorStatus::UnsupportedKind:
    return "unsupported_kind";
  case RenderTapeMissingSeedDescriptorStatus::Accepted:
    return "accepted";
  case RenderTapeMissingSeedDescriptorStatus::InvalidDescriptor:
    return "invalid_descriptor";
  case RenderTapeMissingSeedDescriptorStatus::MissingSubresource:
    return "missing_subresource";
  }
  return "unknown";
}

RenderTapeMissingSeedDescriptor renderTapeDescribeMissingSeed(
    const D9CWireObjectIdentity& identity,
    std::span<const std::byte> descriptor, std::uint32_t missingSubresource,
    RenderTapeReferenceProvenance provenance) noexcept {
  RenderTapeMissingSeedDescriptor result{
      .identity = identity,
      .provenance = provenance,
      .missingSubresource = missingSubresource,
  };

  if (identity.kind == D9C_CHUNK_HANDLE_KIND_TEXTURE) {
    RenderTapeTextureDescriptorV2 texture{};
    if (!renderTapeLoadTextureDescriptorV2(descriptor, texture)) {
      result.descriptorStatus =
          RenderTapeMissingSeedDescriptorStatus::InvalidDescriptor;
      result.expectedContentStatus =
          RenderTapeExpectedContentStatus::InvalidDescriptor;
      return result;
    }
    result.textureDimension =
        static_cast<RenderTapeTextureDimension>(texture.dimension);
    result.mipLevelCount = texture.mipLevelCount;
    result.subresourceCount = texture.subresourceCount;
    if (missingSubresource >= texture.subresourceCount ||
        !renderTapeTextureSubresourceDescriptor(
            descriptor, missingSubresource, result.missingSurface)) {
      result.descriptorStatus =
          RenderTapeMissingSeedDescriptorStatus::MissingSubresource;
      result.expectedContentStatus =
          RenderTapeExpectedContentStatus::InvalidExtent;
      return result;
    }
    result.descriptorStatus = RenderTapeMissingSeedDescriptorStatus::Accepted;
    const auto expected =
        renderTapeDeriveExpectedSurfaceContent(result.missingSurface);
    result.expectedContentStatus = expected.status;
    result.expectedTightBytes = expected.bytes;
    result.expectedTightBytesValid =
        expected.status == RenderTapeExpectedContentStatus::Accepted;
    return result;
  }

  if (identity.kind == D9C_CHUNK_HANDLE_KIND_SURFACE) {
    RenderTapeSurfaceDescriptorV2 surface{};
    if (!renderTapeLoadSurfaceDescriptorV2(descriptor, surface)) {
      result.descriptorStatus =
          RenderTapeMissingSeedDescriptorStatus::InvalidDescriptor;
      result.expectedContentStatus =
          RenderTapeExpectedContentStatus::InvalidDescriptor;
      return result;
    }
    result.subresourceCount = 1u;
    if (missingSubresource != 0u) {
      result.descriptorStatus =
          RenderTapeMissingSeedDescriptorStatus::MissingSubresource;
      result.expectedContentStatus =
          RenderTapeExpectedContentStatus::InvalidExtent;
      return result;
    }
    result.descriptorStatus = RenderTapeMissingSeedDescriptorStatus::Accepted;
    result.missingSurface = surface.surface;
    const auto expected =
        renderTapeDeriveExpectedSurfaceContent(result.missingSurface);
    result.expectedContentStatus = expected.status;
    result.expectedTightBytes = expected.bytes;
    result.expectedTightBytesValid =
        expected.status == RenderTapeExpectedContentStatus::Accepted;
    return result;
  }

  return result;
}

RenderTapeExpectedContentContract renderTapeDeriveExpectedContentContract(
    std::uint32_t identityKind,
    std::span<const std::byte> descriptor,
    std::span<std::uint64_t> subresourceBytes) noexcept {
  if (identityKind == D9C_CHUNK_HANDLE_KIND_SHADER ||
      identityKind == D9C_CHUNK_HANDLE_KIND_VERTEX_DECL ||
      identityKind == D9C_CHUNK_HANDLE_KIND_QUERY)
    return {};
  if (identityKind == D9C_CHUNK_HANDLE_KIND_BUFFER) {
    if (descriptor.size() != sizeof(D9CBufferDesc))
      return expectedContentFailure(
          RenderTapeExpectedContentStatus::InvalidDescriptor);
    D9CBufferDesc buffer{};
    std::memcpy(&buffer, descriptor.data(), sizeof(buffer));
    if (buffer.size == 0u)
      return expectedContentFailure(RenderTapeExpectedContentStatus::InvalidExtent);
    if (!subresourceBytes.empty())
      subresourceBytes[0] = buffer.size;
    return {.status = RenderTapeExpectedContentStatus::Accepted,
            .bytes = buffer.size,
            .count = 1u};
  }
  if (identityKind == D9C_CHUNK_HANDLE_KIND_TEXTURE)
    return expectedContentForTexture(descriptor, subresourceBytes);
  if (identityKind != D9C_CHUNK_HANDLE_KIND_SURFACE)
    return {};

  if (descriptor.size() == sizeof(RenderTapeSurfaceDescriptorV2)) {
    RenderTapeSurfaceDescriptorV2 surface{};
    if (!renderTapeLoadSurfaceDescriptorV2(descriptor, surface))
      return expectedContentFailure(
          RenderTapeExpectedContentStatus::InvalidDescriptor);
    const auto storage = static_cast<RenderTapeSurfaceStorage>(surface.storage);
    const auto disposition = static_cast<RenderTapeInitialContentDisposition>(
        surface.initialContentDisposition);
    if ((storage == RenderTapeSurfaceStorage::TextureSubresource &&
         disposition == RenderTapeInitialContentDisposition::Unavailable) ||
        (storage == RenderTapeSurfaceStorage::SwapchainBackbuffer &&
         disposition == RenderTapeInitialContentDisposition::ProducedPresentOutput))
      return {};
    if (storage != RenderTapeSurfaceStorage::Standalone ||
        disposition != RenderTapeInitialContentDisposition::CompleteSeed)
      return expectedContentFailure(
          RenderTapeExpectedContentStatus::InvalidDescriptor);
    auto content = expectedContentForSurface(surface.surface);
    if (content.status != RenderTapeExpectedContentStatus::Accepted)
      return content;
    if (!subresourceBytes.empty())
      subresourceBytes[0] = content.bytes;
    content.count = 1u;
    return content;
  }
  return expectedContentFailure(RenderTapeExpectedContentStatus::InvalidDescriptor);
}

RenderTapeExpectedContentStatus renderTapeValidateExpectedContentExtents(
    std::uint32_t identityKind, std::span<const std::byte> descriptor,
    std::span<const std::uint64_t> actualSubresourceBytes) noexcept {
  try {
    std::vector<std::uint64_t> expected(actualSubresourceBytes.size());
    const auto contract = renderTapeDeriveExpectedContentContract(
        identityKind, descriptor, expected);
    if (contract.status == RenderTapeExpectedContentStatus::NotRequired)
      return actualSubresourceBytes.empty()
                 ? RenderTapeExpectedContentStatus::NotRequired
                 : RenderTapeExpectedContentStatus::InvalidExtent;
    if (contract.status != RenderTapeExpectedContentStatus::Accepted)
      return contract.status;
    if (contract.count != actualSubresourceBytes.size())
      return RenderTapeExpectedContentStatus::InvalidExtent;
    for (std::size_t index = 0u; index < expected.size(); ++index) {
      if (expected[index] != actualSubresourceBytes[index])
        return RenderTapeExpectedContentStatus::InvalidExtent;
    }
    return RenderTapeExpectedContentStatus::Accepted;
  } catch (...) {
    return RenderTapeExpectedContentStatus::InvalidExtent;
  }
}

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
  case RenderTapeCaptureRejectionReason::UnmaterializedPreArmObject:
    return "unmaterialized_pre_arm_object";
  case RenderTapeCaptureRejectionReason::ExpectedContentContract:
    return "expected_content_contract";
  }
  return "unknown_capture_rejection";
}

bool renderTapeBootstrapClosureContains(
    std::span<const D9CWireObjectIdentity> closure,
    const D9CWireObjectIdentity& identity) noexcept {
  return std::any_of(closure.begin(), closure.end(), [&](const auto& candidate) {
    return candidate.kind == identity.kind &&
           candidate.generation == identity.generation &&
           candidate.objectId == identity.objectId;
  });
}

bool renderTapeBootstrapRequiresAllLiveObjects(
    std::uint32_t profile) noexcept {
  return profile == kRenderTapeProfileSequence;
}

RenderTapeBootstrapClosureStatus renderTapeBuildBootstrapClosure(
    std::span<const D9CWireObjectIdentity> bootstrapHandles,
    const D9CWireObjectIdentity& presentOutput,
    std::span<const RenderTapeBootstrapClosureObject> objects,
    std::vector<D9CWireObjectIdentity>& closure) noexcept {
  closure.clear();
  const auto append = [&](const D9CWireObjectIdentity& identity) {
    if (!renderTapeBootstrapClosureContains(closure, identity))
      closure.push_back(identity);
  };
  try {
    for (std::size_t index = 0u; index < objects.size(); ++index) {
      const auto duplicate = std::find_if(
          objects.begin() + index + 1u, objects.end(), [&](const auto& candidate) {
            return candidate.identity.kind == objects[index].identity.kind &&
                   candidate.identity.generation == objects[index].identity.generation &&
                   candidate.identity.objectId == objects[index].identity.objectId;
          });
      if (duplicate != objects.end())
        return RenderTapeBootstrapClosureStatus::DuplicateObjectIdentity;
    }
    for (const auto& identity : bootstrapHandles) append(identity);
    append(presentOutput);
    for (std::size_t index = 0u; index < closure.size(); ++index) {
      const auto& identity = closure[index];
      const auto object = std::find_if(
          objects.begin(), objects.end(), [&](const auto& candidate) {
            return candidate.identity.kind == identity.kind &&
                   candidate.identity.generation == identity.generation &&
                   candidate.identity.objectId == identity.objectId;
          });
      if (object == objects.end())
        return RenderTapeBootstrapClosureStatus::ReferencedObjectMissing;
      if (!object->complete)
        return RenderTapeBootstrapClosureStatus::ReferencedObjectIncomplete;
      if (!object->hasDescriptorDependency) continue;
      const auto dependency = object->descriptorDependency;
      if (dependency.kind == identity.kind &&
          dependency.generation == identity.generation &&
          dependency.objectId == identity.objectId)
        return RenderTapeBootstrapClosureStatus::InvalidDescriptorDependency;
      const auto dependencyObject = std::find_if(
          objects.begin(), objects.end(), [&](const auto& candidate) {
            return candidate.identity.kind == dependency.kind &&
                   candidate.identity.generation == dependency.generation &&
                   candidate.identity.objectId == dependency.objectId;
          });
      if (dependencyObject == objects.end())
        return RenderTapeBootstrapClosureStatus::DescriptorDependencyMissing;
      if (!dependencyObject->complete)
        return RenderTapeBootstrapClosureStatus::DescriptorDependencyIncomplete;
      append(dependency);
    }
  } catch (...) {
    closure.clear();
    return RenderTapeBootstrapClosureStatus::ReferencedObjectMissing;
  }
  return RenderTapeBootstrapClosureStatus::Accepted;
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

RenderTapeFullSnapshotStatus renderTapeClassifyBufferSnapshot(
    bool captureTrackingEnabled, bool identityMatches, bool extentMatches,
    bool partialWritableLock, std::size_t existingContentBytes,
    std::uint64_t expectedBytes) noexcept {
  return renderTapeClassifySnapshot(
      captureTrackingEnabled, identityMatches, extentMatches,
      partialWritableLock, existingContentBytes, expectedBytes);
}

RenderTapeSurfaceSnapshotRoute renderTapeClassifySurfaceSnapshotRoute(
    bool captureTrackingEnabled, bool ownerIsTexture2D,
    bool mutationIdentityIsTexture, bool partialMutation,
    bool mutationBytesPresent) noexcept {
  if (!captureTrackingEnabled || !partialMutation || !mutationBytesPresent)
    return RenderTapeSurfaceSnapshotRoute::NotRequired;
  if (!ownerIsTexture2D)
    return RenderTapeSurfaceSnapshotRoute::StandaloneSurface;
  if (!mutationIdentityIsTexture)
    return RenderTapeSurfaceSnapshotRoute::InvalidIdentity;
  return RenderTapeSurfaceSnapshotRoute::TextureDerived;
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

RenderTapeUserMemorySeedRoute renderTapeClassifyUserMemorySeedRoute(
    RenderTapeFullSnapshotStatus status) noexcept {
  switch (status) {
  case RenderTapeFullSnapshotStatus::Required:
    return RenderTapeUserMemorySeedRoute::FullOnly;
  case RenderTapeFullSnapshotStatus::NotRequired:
    return RenderTapeUserMemorySeedRoute::PartialOnly;
  case RenderTapeFullSnapshotStatus::Accepted:
  case RenderTapeFullSnapshotStatus::InvalidIdentity:
  case RenderTapeFullSnapshotStatus::InvalidExtent:
  case RenderTapeFullSnapshotStatus::InvalidBytes:
    return RenderTapeUserMemorySeedRoute::Reject;
  }
  return RenderTapeUserMemorySeedRoute::Reject;
}

bool renderTapeUserMemoryLockRequiresFlush(bool captureTrackingEnabled) noexcept {
  return captureTrackingEnabled;
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

RenderTapeBlockMutationStatus applyRenderTapeBufferMutation(
    std::uint64_t expectedBytes, std::uint64_t byteOffset,
    std::span<const std::byte> bytes,
    std::vector<std::byte>& completeContent) noexcept {
  if (expectedBytes == 0u || bytes.empty() ||
      expectedBytes > std::numeric_limits<std::size_t>::max()) {
    return RenderTapeBlockMutationStatus::InvalidBytes;
  }
  if (byteOffset > expectedBytes || bytes.size() > expectedBytes - byteOffset)
    return RenderTapeBlockMutationStatus::InvalidLayout;
  if (byteOffset == 0u && bytes.size() == expectedBytes) {
    try {
      completeContent.assign(bytes.begin(), bytes.end());
      return RenderTapeBlockMutationStatus::Accepted;
    } catch (...) {
      return RenderTapeBlockMutationStatus::AllocationFailed;
    }
  }
  if (completeContent.size() != expectedBytes)
    return RenderTapeBlockMutationStatus::IncompleteSeed;
  std::memcpy(completeContent.data() + static_cast<std::size_t>(byteOffset),
              bytes.data(), bytes.size());
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

RenderTapeLinearLayoutStatus renderTapeUserMemoryFullSeedLayout(
    const D9CSurfaceDesc& desc, std::int32_t pitch,
    RenderTapeLinearLockLayout& out) noexcept {
  const auto status = renderTapeLinearLockLayout(desc, pitch, nullptr, out);
  if (status != RenderTapeLinearLayoutStatus::Accepted)
    return status;
  if (!out.fullSubresource || out.destinationByteOffset != 0u ||
      out.top != 0u || out.rowBytes != out.fullRowBytes ||
      out.rows != out.fullRows)
    return RenderTapeLinearLayoutStatus::InvalidExtent;
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

RenderTapeUpdateTextureStatus applyRenderTapeUpdateTextureClosure(
    std::span<const std::byte> sourceDescriptor,
    std::span<const std::vector<std::byte>> sourceContent,
    std::span<const std::byte> destinationDescriptor,
    std::span<std::vector<std::byte>> destinationContent) noexcept {
  try {
    RenderTapeTextureDescriptorV2 source{};
    RenderTapeTextureDescriptorV2 destination{};
    if (!renderTapeLoadTextureDescriptorV2(sourceDescriptor, source) ||
        !renderTapeLoadTextureDescriptorV2(destinationDescriptor, destination))
      return RenderTapeUpdateTextureStatus::InvalidDescriptor;
    const auto sourceDimension =
        static_cast<RenderTapeTextureDimension>(source.dimension);
    const auto destinationDimension =
        static_cast<RenderTapeTextureDimension>(destination.dimension);
    if ((sourceDimension != RenderTapeTextureDimension::Texture2D &&
         sourceDimension != RenderTapeTextureDimension::Cube) ||
        (destinationDimension != RenderTapeTextureDimension::Texture2D &&
         destinationDimension != RenderTapeTextureDimension::Cube))
      return RenderTapeUpdateTextureStatus::UnsupportedDimension;
    if (sourceDimension != destinationDimension ||
        source.mipLevelCount == 0u ||
        source.mipLevelCount != destination.mipLevelCount ||
        source.subresourceCount == 0u ||
        source.subresourceCount != destination.subresourceCount ||
        sourceContent.size() != source.subresourceCount ||
        destinationContent.size() != destination.subresourceCount ||
        source.initialContentDisposition != static_cast<std::uint32_t>(
            RenderTapeInitialContentDisposition::CompleteSeed) ||
        destination.initialContentDisposition != static_cast<std::uint32_t>(
            RenderTapeInitialContentDisposition::CompleteSeed))
      return RenderTapeUpdateTextureStatus::DescriptorMismatch;

    std::vector<std::uint64_t> expected(source.subresourceCount);
    const auto sourceContract = renderTapeDeriveExpectedContentContract(
        D9C_CHUNK_HANDLE_KIND_TEXTURE, sourceDescriptor, expected);
    if (sourceContract.status == RenderTapeExpectedContentStatus::UnsupportedFormat)
      return RenderTapeUpdateTextureStatus::UnsupportedFormat;
    if (sourceContract.status != RenderTapeExpectedContentStatus::Accepted ||
        sourceContract.count != source.subresourceCount)
      return RenderTapeUpdateTextureStatus::InvalidDescriptor;
    std::vector<std::uint64_t> destinationExpected(source.subresourceCount);
    const auto destinationContract = renderTapeDeriveExpectedContentContract(
        D9C_CHUNK_HANDLE_KIND_TEXTURE, destinationDescriptor,
        destinationExpected);
    if (destinationContract.status ==
        RenderTapeExpectedContentStatus::UnsupportedFormat)
      return RenderTapeUpdateTextureStatus::UnsupportedFormat;
    if (destinationContract.status != RenderTapeExpectedContentStatus::Accepted ||
        destinationContract.count != destination.subresourceCount)
      return RenderTapeUpdateTextureStatus::InvalidDescriptor;

    for (std::uint32_t subresource = 0u;
         subresource < source.subresourceCount; ++subresource) {
      D9CSurfaceDesc sourceDesc{};
      D9CSurfaceDesc destinationDesc{};
      if (!renderTapeTextureSubresourceDescriptor(
              sourceDescriptor, subresource, sourceDesc) ||
          !renderTapeTextureSubresourceDescriptor(
              destinationDescriptor, subresource, destinationDesc))
        return RenderTapeUpdateTextureStatus::InvalidDescriptor;
      // Usage need not match: UpdateTexture is a legal SYSTEMMEM -> DEFAULT
      // transition. Pool legality and all storage/layout identity fields must
      // nevertheless be proven before bytes are copied.
      if (sourceDesc.format != destinationDesc.format ||
          sourceDesc.resourceType != destinationDesc.resourceType ||
          sourceDesc.width != destinationDesc.width ||
          sourceDesc.height != destinationDesc.height ||
          sourceDesc.depth != destinationDesc.depth ||
          sourceDesc.multiSampleType != destinationDesc.multiSampleType ||
          sourceDesc.multiSampleQuality != destinationDesc.multiSampleQuality)
        return RenderTapeUpdateTextureStatus::DescriptorMismatch;
      // D3DPOOL values are part of the API precondition even though pool does
      // not affect the copied byte layout: source must be SYSTEMMEM, while
      // destination must be neither SYSTEMMEM nor SCRATCH.
      constexpr std::uint32_t kD3DPoolSystemMem = 2u;
      constexpr std::uint32_t kD3DPoolScratch = 3u;
      if (sourceDesc.pool != kD3DPoolSystemMem ||
          destinationDesc.pool == kD3DPoolSystemMem ||
          destinationDesc.pool == kD3DPoolScratch)
        return RenderTapeUpdateTextureStatus::DescriptorMismatch;
      constexpr std::uint32_t kD3DUsageAutogenMipmap = 0x00000400u;
      if ((sourceDesc.usage & kD3DUsageAutogenMipmap) != 0u ||
          (destinationDesc.usage & kD3DUsageAutogenMipmap) != 0u ||
          sourceDesc.format == render_tape_d3d_format::P8 ||
          sourceDesc.format == render_tape_d3d_format::A8P8)
        return RenderTapeUpdateTextureStatus::UnsupportedFormat;
      if (expected[subresource] == 0u ||
          sourceContent[subresource].size() != expected[subresource] ||
          destinationExpected[subresource] != expected[subresource])
        return RenderTapeUpdateTextureStatus::IncompleteSource;
    }
    std::vector<std::vector<std::byte>> replacement;
    replacement.reserve(sourceContent.size());
    for (const auto& bytes : sourceContent)
      replacement.emplace_back(bytes);
    for (std::uint32_t subresource = 0u;
         subresource < source.subresourceCount; ++subresource)
      destinationContent[subresource].swap(replacement[subresource]);
    return RenderTapeUpdateTextureStatus::Accepted;
  } catch (const std::bad_alloc&) {
    return RenderTapeUpdateTextureStatus::AllocationFailed;
  } catch (...) {
    return RenderTapeUpdateTextureStatus::InvalidDescriptor;
  }
}

} // namespace dxmt9::d3d9
