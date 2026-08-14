#pragma once

#include "dxmt9/device_c.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>

namespace dxmt9::d3d9 {

// Bounded, pointer-free descriptor payloads shared by PE capture and unix
// provider replay. These are tape schema payloads, not bridge ABI structs.
struct RenderTapeTextureDescriptor {
  D9CSurfaceDesc level0{};
  std::uint32_t levelCount = 0u;
};

inline constexpr std::uint32_t kRenderTapeTextureDescriptorVersion2 = 2u;
inline constexpr std::uint32_t kRenderTapeSurfaceDescriptorVersion2 = 2u;

enum class RenderTapeTextureDimension : std::uint32_t {
  Texture2D = 1u,
  Cube = 2u,
  Volume = 3u,
};

enum class RenderTapeInitialContentDisposition : std::uint32_t {
  CompleteSeed = 1u,
  Unavailable = 2u,
  ProducedPresentOutput = 3u,
};

enum class RenderTapeSurfaceStorage : std::uint32_t {
  Standalone = 1u,
  TextureSubresource = 2u,
  SwapchainBackbuffer = 3u,
};

constexpr std::uint32_t renderTapeTextureDescriptorMipLevel(
    RenderTapeTextureDimension dimension, std::uint32_t mipLevelCount,
    std::uint32_t subresource) noexcept {
  return dimension == RenderTapeTextureDimension::Cube && mipLevelCount != 0u
             ? subresource % mipLevelCount
             : subresource;
}

// Versioned tape metadata. It is carried inside the existing ObjectDefine
// descriptor bytes and therefore changes neither canonical D9C v2 records nor
// the PE/unix bridge ABI. One D9CSurfaceDesc follows for each subresource.
struct RenderTapeTextureDescriptorV2 {
  std::uint32_t schemaVersion = kRenderTapeTextureDescriptorVersion2;
  std::uint32_t dimension = 0u;
  std::uint32_t mipLevelCount = 0u;
  std::uint32_t subresourceCount = 0u;
  std::uint32_t initialContentDisposition = 0u;
  std::uint32_t reserved0 = 0u;
};

// A surface view can name the exact generation-qualified texture storage it
// aliases. Unavailable means that this view supplies no independent seed; the
// parent's matching subresource owns the captured bytes.
struct RenderTapeSurfaceDescriptorV2 {
  std::uint32_t schemaVersion = kRenderTapeSurfaceDescriptorVersion2;
  std::uint32_t storage = 0u;
  std::uint32_t initialContentDisposition = 0u;
  std::uint32_t subresource = 0u;
  D9CWireObjectIdentity parentTexture{};
  D9CSurfaceDesc surface{};
};

// PE wrapper lifetime is separate from tape identity lifetime. A texture
// derived surface remains retained after its last wrapper is released because
// the parent texture still owns the aliased storage; rewrapping acquires the
// same identity, and parent retirement is the only final destroy transition.
struct RenderTapeSurfaceAliasLifetime {
  enum class Disposition : std::uint8_t {
    Live,
    RetainedAlias,
    Retired,
  };

  std::uint32_t wrapperRefs = 0u;
  bool textureAlias = false;
  Disposition disposition = Disposition::Live;

  bool acquire() noexcept {
    if (disposition == Disposition::Retired ||
        wrapperRefs == std::numeric_limits<std::uint32_t>::max())
      return false;
    ++wrapperRefs;
    disposition = Disposition::Live;
    return true;
  }

  bool releaseWrapper() noexcept {
    if (wrapperRefs == 0u)
      return false;
    if (wrapperRefs > 1u) {
      --wrapperRefs;
      return false;
    }
    wrapperRefs = 0u;
    if (textureAlias) {
      disposition = Disposition::RetainedAlias;
      return false;
    }
    disposition = Disposition::Retired;
    return true;
  }

  bool retireParent() noexcept {
    if (!textureAlias || disposition == Disposition::Retired)
      return false;
    disposition = Disposition::Retired;
    return true;
  }
};

inline bool renderTapeLoadSurfaceAliasDescriptor(
    std::span<const std::byte> descriptor,
    RenderTapeSurfaceDescriptorV2 &out) noexcept {
  out = {};
  if (descriptor.size() != sizeof(out))
    return false;
  std::memcpy(&out, descriptor.data(), sizeof(out));
  return out.schemaVersion == kRenderTapeSurfaceDescriptorVersion2 &&
         out.storage == static_cast<std::uint32_t>(
                            RenderTapeSurfaceStorage::TextureSubresource) &&
         out.initialContentDisposition == static_cast<std::uint32_t>(
                                              RenderTapeInitialContentDisposition::
                                                  Unavailable) &&
         out.parentTexture.kind == D9C_CHUNK_HANDLE_KIND_TEXTURE &&
         out.parentTexture.generation != 0u &&
         out.parentTexture.objectId != 0u;
}

inline bool renderTapeSurfaceDescriptorsEqual(const D9CSurfaceDesc &a,
                                              const D9CSurfaceDesc &b) noexcept {
  return a.format == b.format && a.usage == b.usage && a.pool == b.pool &&
         a.multiSampleType == b.multiSampleType &&
         a.multiSampleQuality == b.multiSampleQuality && a.width == b.width &&
         a.height == b.height && a.depth == b.depth;
}

inline bool renderTapeSurfaceAliasDescriptorsEqual(
    std::span<const std::byte> a, std::span<const std::byte> b) noexcept {
  RenderTapeSurfaceDescriptorV2 left{};
  RenderTapeSurfaceDescriptorV2 right{};
  return renderTapeLoadSurfaceAliasDescriptor(a, left) &&
         renderTapeLoadSurfaceAliasDescriptor(b, right) &&
         left.schemaVersion == right.schemaVersion &&
         left.storage == right.storage &&
         left.initialContentDisposition == right.initialContentDisposition &&
         left.subresource == right.subresource &&
         left.parentTexture.kind == right.parentTexture.kind &&
         left.parentTexture.generation == right.parentTexture.generation &&
         left.parentTexture.objectId == right.parentTexture.objectId &&
         renderTapeSurfaceDescriptorsEqual(left.surface, right.surface);
}

struct RenderTapeLogicalObjectSlot {
  D9CWireObjectIdentity identity{};
  bool textureSubresourceAlias = false;
  bool malformedSurfaceDescriptor = false;
  D9CWireObjectIdentity parentTexture{};
  std::uint32_t subresource = 0u;
  D9CSurfaceDesc surface{};
};

inline RenderTapeLogicalObjectSlot renderTapeLogicalObjectSlot(
    const D9CWireObjectIdentity &identity,
    std::span<const std::byte> descriptor) noexcept {
  RenderTapeLogicalObjectSlot slot{.identity = identity};
  if (identity.kind != D9C_CHUNK_HANDLE_KIND_SURFACE)
    return slot;
  if (descriptor.size() != sizeof(RenderTapeSurfaceDescriptorV2))
    return slot;
  RenderTapeSurfaceDescriptorV2 alias{};
  std::memcpy(&alias, descriptor.data(), sizeof(alias));
  if (alias.schemaVersion != kRenderTapeSurfaceDescriptorVersion2) {
    slot.malformedSurfaceDescriptor = true;
    return slot;
  }
  const auto storage = static_cast<RenderTapeSurfaceStorage>(alias.storage);
  if (storage != RenderTapeSurfaceStorage::TextureSubresource) {
    slot.malformedSurfaceDescriptor =
        storage != RenderTapeSurfaceStorage::Standalone &&
        storage != RenderTapeSurfaceStorage::SwapchainBackbuffer;
    return slot;
  }
  if (!renderTapeLoadSurfaceAliasDescriptor(descriptor, alias)) {
    slot.malformedSurfaceDescriptor = true;
    return slot;
  }
  slot.textureSubresourceAlias = true;
  slot.parentTexture = alias.parentTexture;
  slot.subresource = alias.subresource;
  slot.surface = alias.surface;
  return slot;
}

enum class RenderTapeLogicalSlotRelation : std::uint8_t {
  Different,
  Same,
  AliasDescriptorMismatch,
};

inline bool renderTapeSameWireObject(const D9CWireObjectIdentity &a,
                                     const D9CWireObjectIdentity &b) noexcept {
  return a.kind == b.kind && a.objectId == b.objectId;
}

inline bool renderTapeWireGenerationAdvances(
    const D9CWireObjectIdentity &prior,
    const D9CWireObjectIdentity &next) noexcept {
  return !renderTapeSameWireObject(prior, next) ||
         next.generation > prior.generation;
}

inline RenderTapeLogicalSlotRelation renderTapeLogicalSlotRelation(
    const RenderTapeLogicalObjectSlot &a,
    const RenderTapeLogicalObjectSlot &b) noexcept {
  if (a.malformedSurfaceDescriptor || b.malformedSurfaceDescriptor)
    return RenderTapeLogicalSlotRelation::AliasDescriptorMismatch;
  if (a.textureSubresourceAlias != b.textureSubresourceAlias)
    return RenderTapeLogicalSlotRelation::Different;
  if (a.textureSubresourceAlias && b.textureSubresourceAlias) {
    if (a.parentTexture.kind != b.parentTexture.kind ||
        a.parentTexture.generation != b.parentTexture.generation ||
        a.parentTexture.objectId != b.parentTexture.objectId ||
        a.subresource != b.subresource) {
      return RenderTapeLogicalSlotRelation::Different;
    }
    return renderTapeSurfaceDescriptorsEqual(a.surface, b.surface)
               ? RenderTapeLogicalSlotRelation::Same
               : RenderTapeLogicalSlotRelation::AliasDescriptorMismatch;
  }
  return renderTapeSameWireObject(a.identity, b.identity)
             ? RenderTapeLogicalSlotRelation::Same
             : RenderTapeLogicalSlotRelation::Different;
}

enum class RenderTapeSurfaceAliasReplacementStatus : std::uint32_t {
  Accepted = 0u,
  InvalidIdentity,
  DifferentLogicalSlot,
  NonMonotoneGeneration,
  PriorNotRetainedAlias,
  LiveWrapper,
  InvalidDescriptor,
  SurfaceMismatch,
};

inline const char* renderTapeSurfaceAliasReplacementStatusName(
    RenderTapeSurfaceAliasReplacementStatus status) noexcept {
  switch (status) {
  case RenderTapeSurfaceAliasReplacementStatus::Accepted:
    return "accepted";
  case RenderTapeSurfaceAliasReplacementStatus::InvalidIdentity:
    return "invalid_identity";
  case RenderTapeSurfaceAliasReplacementStatus::DifferentLogicalSlot:
    return "different_logical_slot";
  case RenderTapeSurfaceAliasReplacementStatus::NonMonotoneGeneration:
    return "non_monotone_generation";
  case RenderTapeSurfaceAliasReplacementStatus::PriorNotRetainedAlias:
    return "prior_not_retained_alias";
  case RenderTapeSurfaceAliasReplacementStatus::LiveWrapper:
    return "live_wrapper";
  case RenderTapeSurfaceAliasReplacementStatus::InvalidDescriptor:
    return "invalid_descriptor";
  case RenderTapeSurfaceAliasReplacementStatus::SurfaceMismatch:
    return "surface_mismatch";
  }
  return "unknown";
}

inline RenderTapeSurfaceAliasReplacementStatus
renderTapeSurfaceAliasReplacementStatus(
    const D9CWireObjectIdentity &priorIdentity,
    const RenderTapeSurfaceAliasLifetime &priorLifetime,
    std::span<const std::byte> priorDescriptor,
    const D9CWireObjectIdentity &nextIdentity,
    std::span<const std::byte> nextDescriptor) noexcept {
  if (priorIdentity.kind != D9C_CHUNK_HANDLE_KIND_SURFACE ||
      nextIdentity.kind != D9C_CHUNK_HANDLE_KIND_SURFACE ||
      priorIdentity.generation == 0u || nextIdentity.generation == 0u ||
      priorIdentity.objectId == 0u || nextIdentity.objectId == 0u) {
    return RenderTapeSurfaceAliasReplacementStatus::InvalidIdentity;
  }
  const auto priorSlot =
      renderTapeLogicalObjectSlot(priorIdentity, priorDescriptor);
  const auto nextSlot = renderTapeLogicalObjectSlot(nextIdentity, nextDescriptor);
  if (priorSlot.malformedSurfaceDescriptor ||
      nextSlot.malformedSurfaceDescriptor) {
    return RenderTapeSurfaceAliasReplacementStatus::InvalidDescriptor;
  }
  const auto relation = renderTapeLogicalSlotRelation(priorSlot, nextSlot);
  if (relation == RenderTapeLogicalSlotRelation::Different)
    return RenderTapeSurfaceAliasReplacementStatus::DifferentLogicalSlot;
  if (relation == RenderTapeLogicalSlotRelation::AliasDescriptorMismatch)
    return RenderTapeSurfaceAliasReplacementStatus::SurfaceMismatch;
  if (!renderTapeWireGenerationAdvances(priorIdentity, nextIdentity)) {
    return RenderTapeSurfaceAliasReplacementStatus::NonMonotoneGeneration;
  }
  if (!priorLifetime.textureAlias)
    return RenderTapeSurfaceAliasReplacementStatus::PriorNotRetainedAlias;
  if (priorLifetime.wrapperRefs != 0u)
    return RenderTapeSurfaceAliasReplacementStatus::LiveWrapper;
  if (priorLifetime.disposition !=
      RenderTapeSurfaceAliasLifetime::Disposition::RetainedAlias) {
    return RenderTapeSurfaceAliasReplacementStatus::PriorNotRetainedAlias;
  }
  if (!priorSlot.textureSubresourceAlias ||
      !nextSlot.textureSubresourceAlias) {
    return RenderTapeSurfaceAliasReplacementStatus::InvalidDescriptor;
  }
  return RenderTapeSurfaceAliasReplacementStatus::Accepted;
}

inline bool renderTapeDescriptorSubresourceCountFits(
    std::uint32_t count, std::size_t headerBytes) noexcept {
  if constexpr (sizeof(std::size_t) >= sizeof(std::uint64_t)) {
    return true;
  } else {
    return count <=
           (std::numeric_limits<std::size_t>::max() - headerBytes) /
               sizeof(D9CSurfaceDesc);
  }
}

inline bool renderTapeTextureSubresourceDescriptor(
    std::span<const std::byte> descriptor, std::uint32_t subresource,
    D9CSurfaceDesc &out) noexcept {
  if (descriptor.size() >= sizeof(RenderTapeTextureDescriptorV2)) {
    RenderTapeTextureDescriptorV2 texture{};
    std::memcpy(&texture, descriptor.data(), sizeof(texture));
    if (texture.schemaVersion == kRenderTapeTextureDescriptorVersion2) {
      if (subresource >= texture.subresourceCount ||
          !renderTapeDescriptorSubresourceCountFits(
              texture.subresourceCount, sizeof(texture)) ||
          descriptor.size() !=
              sizeof(texture) +
                  static_cast<std::size_t>(texture.subresourceCount) *
                      sizeof(D9CSurfaceDesc)) {
        return false;
      }
      std::memcpy(&out,
                  descriptor.data() + sizeof(texture) +
                      static_cast<std::size_t>(subresource) *
                          sizeof(D9CSurfaceDesc),
                  sizeof(out));
      return true;
    }
  }
  if (descriptor.size() < sizeof(RenderTapeTextureDescriptor))
    return false;
  RenderTapeTextureDescriptor texture{};
  std::memcpy(&texture, descriptor.data(), sizeof(texture));
  if (subresource >= texture.levelCount)
    return false;
  if (subresource == 0u) {
    out = texture.level0;
    return true;
  }
  const auto offset = sizeof(texture) +
      (static_cast<std::size_t>(subresource) - 1u) * sizeof(D9CSurfaceDesc);
  if (offset > descriptor.size() ||
      descriptor.size() - offset < sizeof(out)) {
    return false;
  }
  std::memcpy(&out, descriptor.data() + offset, sizeof(out));
  return true;
}

struct RenderTapeVertexDeclDescriptor {
  std::uint32_t elementCount = 0u;
  std::uint32_t elementBytes = 0u;
};

struct RenderTapeShaderDescriptor {
  std::uint32_t stage = 0u;
  std::uint32_t bytecodeBytes = 0u;
};

struct RenderTapeQueryDescriptor {
  std::uint32_t type = 0u;
  std::uint32_t dataBytes = 0u;
};

static_assert(sizeof(RenderTapeTextureDescriptor) == sizeof(D9CSurfaceDesc) + 4u);
static_assert(sizeof(RenderTapeTextureDescriptorV2) == 24u);
static_assert(sizeof(RenderTapeSurfaceDescriptorV2) == 72u);
static_assert(offsetof(RenderTapeSurfaceDescriptorV2, parentTexture) == 16u);
static_assert(offsetof(RenderTapeSurfaceDescriptorV2, surface) == 32u);
static_assert(sizeof(RenderTapeVertexDeclDescriptor) == 8u);
static_assert(sizeof(RenderTapeShaderDescriptor) == 8u);
static_assert(sizeof(RenderTapeQueryDescriptor) == 8u);

} // namespace dxmt9::d3d9
