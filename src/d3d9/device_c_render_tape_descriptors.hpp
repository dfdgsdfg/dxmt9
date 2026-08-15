#pragma once

#include "dxmt9/device_c.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>

namespace dxmt9::d3d9 {

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
  ProducedByCapturedPass = 4u,
  // Standalone D24X8 only. Canonical bytes are tightly packed little-endian
  // float32 depth, encoding version 1; they are not physical Metal bytes.
  CompleteDepthFloat32V1 = 5u,
};

// Capture-only model/code pin for the hard Present boundary. The PE owner
// advances this value at the exact production call sites; it is never touched
// while capture is disabled.
enum class RenderTapeArmBoundaryPhase : std::uint8_t {
  Disabled,
  PresentFlushed,
  SnapshotComplete,
  Armed,
  FirstCapturedChunk,
};

struct RenderTapeArmBoundaryTransition {
  RenderTapeArmBoundaryPhase next = RenderTapeArmBoundaryPhase::Disabled;
  bool accepted = false;
};

constexpr RenderTapeArmBoundaryTransition renderTapeAdvanceArmBoundary(
    RenderTapeArmBoundaryPhase current,
    RenderTapeArmBoundaryPhase requested) noexcept {
  const bool accepted =
      (current == RenderTapeArmBoundaryPhase::Disabled &&
       requested == RenderTapeArmBoundaryPhase::PresentFlushed) ||
      (current == RenderTapeArmBoundaryPhase::PresentFlushed &&
       requested == RenderTapeArmBoundaryPhase::SnapshotComplete) ||
      (current == RenderTapeArmBoundaryPhase::SnapshotComplete &&
       requested == RenderTapeArmBoundaryPhase::Armed) ||
      (current == RenderTapeArmBoundaryPhase::Armed &&
       requested == RenderTapeArmBoundaryPhase::FirstCapturedChunk);
  return {.next = accepted ? requested : current, .accepted = accepted};
}

inline constexpr bool renderTapeSnapshotStandaloneD24X8Supported(
    const D9CSurfaceDesc &surface) noexcept {
  return surface.resourceType == 1u && surface.width != 0u &&
         surface.height != 0u && surface.depth == 1u && surface.pool == 0u &&
         surface.usage == 2u && surface.format == 77u &&
         surface.multiSampleType == 0u && surface.multiSampleQuality == 0u;
}

enum class RenderTapeSurfaceStorage : std::uint32_t {
  Standalone = 1u,
  TextureSubresource = 2u,
  SwapchainBackbuffer = 3u,
};

enum class RenderTapeSurfaceRegistrationRoute : std::uint8_t {
  Standalone,
  TextureParentAlias,
};

// Produced standalone surfaces are deliberately narrower than the general
// D3D9 surface descriptor grammar.  They must be single-sample DEFAULT-pool
// attachments whose complete logical contents can be established by one
// unrestricted Clear.  D24X8 has no stencil aspect; stencil-bearing and all
// other depth formats remain fail-closed until an aspect-complete proof is
// carried by the tape grammar.
inline constexpr bool renderTapeProducedStandaloneSurfaceSupported(
    const D9CSurfaceDesc &surface) noexcept {
  if (surface.resourceType != 1u || surface.width == 0u ||
      surface.height == 0u || surface.depth != 1u || surface.pool != 0u ||
      surface.multiSampleType != 0u || surface.multiSampleQuality != 0u)
    return false;
  if (surface.usage == 1u)
    return surface.format == 21u || surface.format == 22u;
  return surface.usage == 2u && surface.format == 77u;
}

inline constexpr RenderTapeSurfaceRegistrationRoute
renderTapeSurfaceRegistrationRoute(bool hasParentTexture) noexcept {
  return hasParentTexture
             ? RenderTapeSurfaceRegistrationRoute::TextureParentAlias
             : RenderTapeSurfaceRegistrationRoute::Standalone;
}

inline constexpr bool renderTapePresentOutputIdentityMatchesCommand(
    const D9CWireObjectIdentity &commandIdentity,
    const D9CWireObjectIdentity &presentOutputIdentity) noexcept {
  return commandIdentity.kind == presentOutputIdentity.kind &&
         commandIdentity.generation == presentOutputIdentity.generation &&
         commandIdentity.objectId == presentOutputIdentity.objectId;
}

inline constexpr bool renderTapeZeroIdentity(
    const D9CWireObjectIdentity &identity) noexcept {
  return identity.kind == 0u && identity.generation == 0u &&
         identity.objectId == 0u;
}

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

inline constexpr bool renderTapeProducedTextureShapeSupported(
    const RenderTapeTextureDescriptorV2 &texture) noexcept {
  if (texture.mipLevelCount != 1u)
    return false;
  const auto dimension =
      static_cast<RenderTapeTextureDimension>(texture.dimension);
  return (dimension == RenderTapeTextureDimension::Texture2D &&
          texture.subresourceCount == 1u) ||
         (dimension == RenderTapeTextureDimension::Cube &&
          texture.subresourceCount == 6u);
}

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
    RetainedChunk,
    RetainedAlias,
    Retired,
  };

  std::uint32_t wrapperRefs = 0u;
  std::uint32_t pendingChunkRefs = 0u;
  bool textureAlias = false;
  bool parentRetired = false;
  Disposition disposition = Disposition::Live;

  bool acquire() noexcept {
    if (disposition == Disposition::Retired || parentRetired ||
        wrapperRefs == std::numeric_limits<std::uint32_t>::max())
      return false;
    ++wrapperRefs;
    disposition = Disposition::Live;
    return true;
  }

  bool retainPendingChunk() noexcept {
    // One builder owns at most one logical pending ref per identity. A
    // wrapper may release, reacquire, and release again before that builder
    // drains; this is still one pending ownership interval.
    if (disposition == Disposition::Retired || pendingChunkRefs != 0u)
      return false;
    ++pendingChunkRefs;
    if (wrapperRefs == 0u)
      disposition = Disposition::RetainedChunk;
    return true;
  }

  bool releasePendingChunk() noexcept {
    if (pendingChunkRefs == 0u)
      return false;
    --pendingChunkRefs;
    if (wrapperRefs == 0u && (!textureAlias || parentRetired)) {
      disposition = Disposition::Retired;
      return true;
    }
    if (wrapperRefs == 0u)
      disposition = Disposition::RetainedAlias;
    return false;
  }

  bool releaseWrapper() noexcept {
    if (wrapperRefs == 0u)
      return false;
    if (wrapperRefs > 1u) {
      --wrapperRefs;
      return false;
    }
    wrapperRefs = 0u;
    if (pendingChunkRefs != 0u) {
      disposition = Disposition::RetainedChunk;
      return false;
    }
    if (textureAlias && !parentRetired) {
      disposition = Disposition::RetainedAlias;
      return false;
    }
    disposition = Disposition::Retired;
    return true;
  }

  bool retireParent() noexcept {
    if (!textureAlias || parentRetired || disposition == Disposition::Retired)
      return false;
    parentRetired = true;
    if (wrapperRefs == 0u && pendingChunkRefs == 0u) {
      disposition = Disposition::Retired;
      return true;
    }
    disposition = pendingChunkRefs != 0u ? Disposition::RetainedChunk
                                         : Disposition::RetainedAlias;
    return false;
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
         out.parentTexture.objectId != 0u && out.surface.resourceType == 1u &&
         out.surface.width != 0u && out.surface.height != 0u &&
         out.surface.depth != 0u;
}

inline bool renderTapeLoadSurfaceDescriptorV2(
    std::span<const std::byte> descriptor,
    RenderTapeSurfaceDescriptorV2 &out) noexcept {
  out = {};
  if (descriptor.size() != sizeof(out))
    return false;
  std::memcpy(&out, descriptor.data(), sizeof(out));
  if (out.schemaVersion != kRenderTapeSurfaceDescriptorVersion2 ||
      out.surface.resourceType != 1u ||
      out.surface.width == 0u || out.surface.height == 0u ||
      out.surface.depth == 0u) {
    return false;
  }
  const auto storage = static_cast<RenderTapeSurfaceStorage>(out.storage);
  const auto disposition = static_cast<RenderTapeInitialContentDisposition>(
      out.initialContentDisposition);
  switch (storage) {
  case RenderTapeSurfaceStorage::Standalone:
    return (disposition == RenderTapeInitialContentDisposition::CompleteSeed ||
            (disposition == RenderTapeInitialContentDisposition::
                                CompleteDepthFloat32V1 &&
             renderTapeSnapshotStandaloneD24X8Supported(out.surface)) ||
            (disposition ==
                 RenderTapeInitialContentDisposition::ProducedByCapturedPass &&
             renderTapeProducedStandaloneSurfaceSupported(out.surface))) &&
           out.subresource == 0u && renderTapeZeroIdentity(out.parentTexture);
  case RenderTapeSurfaceStorage::TextureSubresource:
    return disposition == RenderTapeInitialContentDisposition::Unavailable &&
           out.parentTexture.kind == D9C_CHUNK_HANDLE_KIND_TEXTURE &&
           out.parentTexture.generation != 0u &&
           out.parentTexture.objectId != 0u;
  case RenderTapeSurfaceStorage::SwapchainBackbuffer:
    return disposition ==
               RenderTapeInitialContentDisposition::ProducedPresentOutput &&
           out.subresource == 0u && renderTapeZeroIdentity(out.parentTexture);
  }
  return false;
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
  if (descriptor.size() != sizeof(RenderTapeSurfaceDescriptorV2)) {
    slot.malformedSurfaceDescriptor = true;
    return slot;
  }
  RenderTapeSurfaceDescriptorV2 alias{};
  if (!renderTapeLoadSurfaceDescriptorV2(descriptor, alias)) {
    slot.malformedSurfaceDescriptor = true;
    return slot;
  }
  const auto storage = static_cast<RenderTapeSurfaceStorage>(alias.storage);
  if (storage != RenderTapeSurfaceStorage::TextureSubresource)
    return slot;
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

// The present output is a capture-owned role, not a property of the D3D9
// object. Exactly one live registry entry may hold it, and the admission that
// names a new holder must first hand the role back. The PE wrapper an
// admission opens is released before the arm returns, so a retained holder
// both multiplies the role across retries and keeps a stale entry alive for a
// wire object id the C-side registry is free to recycle at a newer generation.
enum class RenderTapePresentOutputRoleTransition : std::uint32_t {
  // No prior holder, or the prior holder already left the live registry.
  None = 0u,
  // The same exact identity is being re-admitted; the role does not move.
  Retained,
  // The holder is an app-owned object the capture only re-roled. Restore its
  // ordinary initial-content state and leave the object registered.
  Demote,
  // The holder exists only because the admission registered it. Release the
  // admission's wrapper reference and retire the identity.
  Retire,
};

inline const char* renderTapePresentOutputRoleTransitionName(
    RenderTapePresentOutputRoleTransition transition) noexcept {
  switch (transition) {
  case RenderTapePresentOutputRoleTransition::None:
    return "none";
  case RenderTapePresentOutputRoleTransition::Retained:
    return "retained";
  case RenderTapePresentOutputRoleTransition::Demote:
    return "demote";
  case RenderTapePresentOutputRoleTransition::Retire:
    return "retire";
  }
  return "unknown";
}

struct RenderTapePresentOutputRole {
  D9CWireObjectIdentity identity{};
  bool held = false;
  // True when the admission registered the holder itself, so the capture owns
  // the single wrapper reference recorded for it.
  bool captureOwned = false;
};

// `priorFound` reports whether the holder is still in the live registry, and
// `priorWrapperRefs` its wrapper reference count there. Both are ignored when
// no role is held. The transition never inspects generations: identity
// monotonicity stays owned by registration, so handing the role back cannot
// admit an identity that registration would reject.
inline RenderTapePresentOutputRoleTransition
renderTapePresentOutputRoleTransition(
    const RenderTapePresentOutputRole& role,
    const D9CWireObjectIdentity* next, bool priorFound,
    std::uint32_t priorWrapperRefs) noexcept {
  // A holder that already left the live registry is checked before identity,
  // so a re-admission that has to register the object again is classified as a
  // fresh admission rather than as a retained role.
  if (!role.held || !priorFound)
    return RenderTapePresentOutputRoleTransition::None;
  if (next != nullptr && role.identity.kind == next->kind &&
      role.identity.generation == next->generation &&
      role.identity.objectId == next->objectId) {
    return RenderTapePresentOutputRoleTransition::Retained;
  }
  // Retirement is scoped to the proven swap-chain output handoff: a surface
  // the admission itself registered that still carries only the admission's
  // own wrapper reference, and therefore has no remaining app wrapper. That is
  // the same transition the ordinary child destroy path would have produced.
  // Every other holder — including any generic standalone or texture-derived
  // alias the capture merely re-roled — is demoted and stays registered, so
  // this policy never removes an entry the alias rules still own.
  if (role.captureOwned && priorWrapperRefs == 1u &&
      role.identity.kind == D9C_CHUNK_HANDLE_KIND_SURFACE)
    return RenderTapePresentOutputRoleTransition::Retire;
  return RenderTapePresentOutputRoleTransition::Demote;
}

enum class RenderTapeSurfaceAliasReplacementStatus : std::uint32_t {
  Accepted = 0u,
  InvalidIdentity,
  DifferentLogicalSlot,
  NonMonotoneGeneration,
  PriorNotRetainedAlias,
  LiveWrapper,
  PendingChunkRequiresFlush,
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
  case RenderTapeSurfaceAliasReplacementStatus::PendingChunkRequiresFlush:
    return "pending_chunk_requires_flush";
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
  if (priorLifetime.pendingChunkRefs != 0u)
    return RenderTapeSurfaceAliasReplacementStatus::PendingChunkRequiresFlush;
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
  if (descriptor.size() < sizeof(RenderTapeTextureDescriptorV2))
    return false;
  RenderTapeTextureDescriptorV2 texture{};
  std::memcpy(&texture, descriptor.data(), sizeof(texture));
  if (texture.schemaVersion != kRenderTapeTextureDescriptorVersion2 ||
      subresource >= texture.subresourceCount ||
      !renderTapeDescriptorSubresourceCountFits(texture.subresourceCount,
                                                sizeof(texture)) ||
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

inline std::uint32_t renderTapeTextureMipExtent(
    std::uint32_t extent, std::uint32_t mipLevel) noexcept {
  if (mipLevel >= 32u)
    return 1u;
  const auto value = extent >> mipLevel;
  return value == 0u ? 1u : value;
}

inline bool renderTapeLoadTextureDescriptorV2(
    std::span<const std::byte> descriptor,
    RenderTapeTextureDescriptorV2 &out) noexcept {
  out = {};
  if (descriptor.size() < sizeof(out))
    return false;
  std::memcpy(&out, descriptor.data(), sizeof(out));
  const auto dimension = static_cast<RenderTapeTextureDimension>(out.dimension);
  std::uint64_t expectedSubresources = out.mipLevelCount;
  if (dimension == RenderTapeTextureDimension::Cube)
    expectedSubresources *= 6u;
  else if (dimension != RenderTapeTextureDimension::Texture2D &&
           dimension != RenderTapeTextureDimension::Volume)
    return false;
  if (out.schemaVersion != kRenderTapeTextureDescriptorVersion2 ||
      out.mipLevelCount == 0u ||
      out.subresourceCount != expectedSubresources || out.reserved0 != 0u ||
      !renderTapeDescriptorSubresourceCountFits(out.subresourceCount,
                                                sizeof(out)) ||
      descriptor.size() !=
          sizeof(out) + static_cast<std::size_t>(out.subresourceCount) *
                            sizeof(D9CSurfaceDesc)) {
    return false;
  }
  const std::uint32_t resourceType =
      dimension == RenderTapeTextureDimension::Texture2D
          ? 3u
          : dimension == RenderTapeTextureDimension::Volume ? 4u : 5u;
  D9CSurfaceDesc base{};
  if (!renderTapeTextureSubresourceDescriptor(descriptor, 0u, base) ||
      base.resourceType != resourceType || base.width == 0u ||
      base.height == 0u || base.depth == 0u ||
      (dimension != RenderTapeTextureDimension::Volume && base.depth != 1u)) {
    return false;
  }
  for (std::uint32_t subresource = 0u; subresource < out.subresourceCount;
       ++subresource) {
    const auto mipLevel = renderTapeTextureDescriptorMipLevel(
        dimension, out.mipLevelCount, subresource);
    D9CSurfaceDesc current{};
    if (!renderTapeTextureSubresourceDescriptor(descriptor, subresource,
                                                current) ||
        current.format != base.format ||
        current.resourceType != resourceType || current.usage != base.usage ||
        current.pool != base.pool ||
        current.multiSampleType != base.multiSampleType ||
        current.multiSampleQuality != base.multiSampleQuality ||
        current.width != renderTapeTextureMipExtent(base.width, mipLevel) ||
        current.height != renderTapeTextureMipExtent(base.height, mipLevel) ||
        current.depth !=
            (dimension == RenderTapeTextureDimension::Volume
                 ? renderTapeTextureMipExtent(base.depth, mipLevel)
                 : 1u)) {
      return false;
    }
  }
  return true;
}

// A Produced texture is admitted through one exact surface alias.  The alias
// has D3DRTYPE_SURFACE while its parent descriptor has D3DRTYPE_TEXTURE, so
// compare the physical subresource shape field-by-field and keep the expected
// resource-type difference explicit.
inline bool renderTapeSurfaceAliasMatchesTextureSubresource(
    std::span<const std::byte> textureDescriptor,
    const D9CWireObjectIdentity &textureIdentity,
    const RenderTapeSurfaceDescriptorV2 &alias) noexcept {
  RenderTapeTextureDescriptorV2 texture{};
  D9CSurfaceDesc subresource{};
  return renderTapeLoadTextureDescriptorV2(textureDescriptor, texture) &&
         alias.schemaVersion == kRenderTapeSurfaceDescriptorVersion2 &&
         alias.storage == static_cast<std::uint32_t>(
                              RenderTapeSurfaceStorage::TextureSubresource) &&
         alias.initialContentDisposition == static_cast<std::uint32_t>(
             RenderTapeInitialContentDisposition::Unavailable) &&
         renderTapeSameWireObject(alias.parentTexture, textureIdentity) &&
         alias.subresource < texture.subresourceCount &&
         renderTapeTextureSubresourceDescriptor(
             textureDescriptor, alias.subresource, subresource) &&
         alias.surface.resourceType == 1u &&
         alias.surface.format == subresource.format &&
         alias.surface.usage == subresource.usage &&
         alias.surface.pool == subresource.pool &&
         alias.surface.multiSampleType == subresource.multiSampleType &&
         alias.surface.multiSampleQuality == subresource.multiSampleQuality &&
         alias.surface.width == subresource.width &&
         alias.surface.height == subresource.height &&
         alias.surface.depth == subresource.depth;
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

static_assert(sizeof(RenderTapeTextureDescriptorV2) == 24u);
static_assert(sizeof(RenderTapeSurfaceDescriptorV2) == 72u);
static_assert(offsetof(RenderTapeSurfaceDescriptorV2, parentTexture) == 16u);
static_assert(offsetof(RenderTapeSurfaceDescriptorV2, surface) == 32u);
static_assert(sizeof(RenderTapeVertexDeclDescriptor) == 8u);
static_assert(sizeof(RenderTapeShaderDescriptor) == 8u);
static_assert(sizeof(RenderTapeQueryDescriptor) == 8u);

} // namespace dxmt9::d3d9
