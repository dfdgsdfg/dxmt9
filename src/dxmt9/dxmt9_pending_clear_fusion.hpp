#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace dxmt9::pending_clear {

// Experimental call-local admission facts, not resource ownership or a wire ABI.
// No production call site yet: R-BACK-15.19 requires the adapter/GPU gates too.
enum Aspect : std::uint8_t { Color = 1, Depth = 2, Stencil = 4 };
enum class Load : std::uint8_t { Preserve, Discard };

struct SourceIdentity {
  // Owner is the encode queue/tape instance, never a dereferenceable capability.
  std::uint64_t owner = 0, generation = 0, sequence = 0;
  std::uint32_t sourceIndex = std::numeric_limits<std::uint32_t>::max();
  std::uint64_t storageGeneration = 0;
  std::uint32_t firstPage = std::numeric_limits<std::uint32_t>::max();
  std::uint32_t pageCount = 0;
  friend constexpr bool operator==(SourceIdentity, SourceIdentity) = default;
  constexpr bool valid() const noexcept {
    return owner && generation && sequence && storageGeneration && pageCount &&
        sourceIndex != std::numeric_limits<std::uint32_t>::max() &&
        firstPage != std::numeric_limits<std::uint32_t>::max();
  }
};

struct Attachment {
  std::uint64_t resource = 0, alias = 0;
  std::uint32_t width = 0, height = 0, samples = 0;
  std::uint32_t boundLevel = 0, viewLevel = 0, slice = 0;
  std::uint8_t aspects = 0;
  bool resolved = false;
  bool hasResolveTarget = false;
};

struct Facts {
  SourceIdentity clearSource{}, drawSource{};
  std::uint32_t clearIndex = 0, drawIndex = 0;
  // Four color attachments and one depth/stencil attachment.
  std::array<Attachment, 5> clear{}, draw{};
  std::array<Load, 4> followingColorLoads{};
  // D3D9 exposes 16 fragment and 4 vertex texture slots. The runtime adapter
  // statically binds this extent to core::kMaxTextures.
  std::array<std::uint64_t, 20> sampledTextures{};
  std::uint8_t requestedAspects = 0;
  bool fullRect = false;
  bool activeEncoder = false;
  bool queryScope = false;
  bool exactReadHazard = false;
  bool srgbWrite = false;
  bool backbufferDiscardPending = false;
};

enum class Decision : std::uint8_t {
  Admit, Order, Scope, EmptyOrUnknownAspects, PartialRect, ConversionOrDiscard,
  Unresolved, Identity, Extent, SamplesOrResolve, AspectMismatch, ReadHazard,
  FollowingLoadDiscardsClear,
};

constexpr Decision classify(const Facts& f) noexcept {
  if (!f.clearSource.valid() || f.clearSource != f.drawSource ||
      f.clearIndex == std::numeric_limits<std::uint32_t>::max() ||
      f.drawIndex != f.clearIndex + 1u) return Decision::Order;
  if (f.activeEncoder || f.queryScope) return Decision::Scope;
  if (!f.requestedAspects || (f.requestedAspects & ~(Color | Depth | Stencil)))
    return Decision::EmptyOrUnknownAspects;
  if (!f.fullRect) return Decision::PartialRect;
  // clear.has_value() currently changes the discard-after-Present branch.
  // Until that branch has an independent refinement, do not cross it here.
  if (f.srgbWrite || f.backbufferDiscardPending) return Decision::ConversionOrDiscard;
  if (f.exactReadHazard) return Decision::ReadHazard;
  std::uint32_t width = 0, height = 0;
  bool requestedColor = false;
  for (std::size_t i = 0; i < f.draw.size(); ++i) {
    const auto& d = f.draw[i];
    const auto& c = f.clear[i];
    const auto requested = i < 4 ? std::uint8_t(f.requestedAspects & Color)
                                : std::uint8_t(f.requestedAspects & (Depth | Stencil));
    const bool clearsSlot = requested && (i == 4 || c.resource != 0);
    if (d.resource) {
      if (!d.resolved) return Decision::Unresolved;
      if (!d.width || !d.height) return Decision::Extent;
      if (!width) { width = d.width; height = d.height; }
      if (width != d.width || height != d.height) return Decision::Extent;
      if (d.samples != 1 || d.hasResolveTarget) return Decision::SamplesOrResolve;
      if ((i < 4 && d.aspects != Color) ||
          (i == 4 && (!d.aspects || (d.aspects & ~(Depth | Stencil)))))
        return Decision::AspectMismatch;
    }
    if (!clearsSlot) continue;
    if (!c.resource || !c.resolved || !d.resource) return Decision::Unresolved;
    if (c.resource != d.resource || c.boundLevel != d.boundLevel ||
        c.viewLevel != d.viewLevel || c.slice != d.slice || c.alias != d.alias)
      return Decision::Identity;
    if (c.width != d.width || c.height != d.height) return Decision::Extent;
    if (c.samples != 1 || c.hasResolveTarget) return Decision::SamplesOrResolve;
    if (c.aspects != d.aspects || (c.aspects & requested) != requested ||
        (d.aspects & requested) != requested)
      return Decision::AspectMismatch;
    for (const auto texture : f.sampledTextures)
      if (texture && (texture == c.resource || (c.alias && texture == c.alias)))
        return Decision::ReadHazard;
    if (i < 4) {
      requestedColor = true;
      // The serial follow-up pass must actually load the standalone Clear.
      // Do not claim exact refinement of a first-use DontCare-load sequence.
      if (f.followingColorLoads[i] != Load::Preserve)
        return Decision::FollowingLoadDiscardsClear;
    }
  }
  if (!width || ((f.requestedAspects & Color) && !requestedColor))
    return Decision::EmptyOrUnknownAspects;
  return Decision::Admit;
}

} // namespace dxmt9::pending_clear
