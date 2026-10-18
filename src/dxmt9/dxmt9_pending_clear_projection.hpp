#pragma once

#include "dxmt9_pending_clear_fusion.hpp"
#include "dxmt9_draw_encoder.hpp"
#include "dxmt9_format_convert.hpp"
#include "dxmt9_resource_pool.hpp"

namespace dxmt9::pending_clear {

// Candidate-only adapter. The caller owns retained surfaces and serialized
// encode state throughout this call. No pointer or ownership escapes in Facts.
inline SourceIdentity projectSource(
    std::uint64_t owner, core::metalqueue::PublishedCommandRef command) noexcept {
  if (!command.valid()) return {};
  return {.owner = owner, .generation = command.source.id.generation,
          .sequence = command.seqId, .sourceIndex = command.source.id.index,
          .storageGeneration = command.source.storage.generation,
          .firstPage = command.source.storage.firstPage,
          .pageCount = command.source.storage.pageCount};
}

inline Attachment projectAttachment(
    core::RenderTargetAttachment binding,
    const resources::SurfaceRecord* surface) noexcept {
  Attachment result{.resource = binding.handle.value, .boundLevel = binding.level};
  if (!binding.handle || !surface) return result;
  result.alias = surface->aliasTexture.value;
  result.width = surface->desc.width;
  result.height = surface->desc.height;
  // Only the proven single-sample envelope is represented as supported.
  result.samples = binding.sampleCount == 1 &&
      surface->desc.multiSampleType == core::MultiSampleType::None ? 1 : 0;
  result.viewLevel = surface->level;
  result.slice = surface->slice;
  const bool depth = convert::formatHasDepthAspect(surface->desc.format);
  const bool stencil = convert::formatHasStencilAspect(surface->desc.format);
  result.aspects = depth || stencil
      ? static_cast<std::uint8_t>((depth ? Depth : 0) | (stencil ? Stencil : 0))
      : static_cast<std::uint8_t>(Color);
  // Match the serial descriptor's physical participation, not only its logical
  // format: createSurface(NullRt) is storage-free, but a texture-backed NullRt
  // alias participates as color in prepareRenderPassWithStoreProofLookahead.
  // Keep storage-free identities unresolved in this initial envelope.
  result.resolved = static_cast<bool>(surface->texture);
  if (result.resolved) {
    // A failed mip-view creation may leave a parent texture in the record.
    // Descriptor dimensions alone do not prove the effective render area.
    WMT::Texture texture{surface->texture.handle};
    result.resolved = texture.width() == result.width &&
        texture.height() == result.height;
  }
  result.hasResolveTarget = static_cast<bool>(surface->resolveTexture);
  return result;
}

struct EncodeScope {
  bool activeEncoder;
  bool queryScope;
  bool exactReadHazard;
};

inline Facts projectFacts(
    CommandQueue& queue, const resources::Pool& pool,
    core::metalqueue::PublishedCommandRef clearCommand,
    core::metalqueue::PublishedCommandRef drawCommand,
    const core::ClearDesc& clear, const core::FlatDrawStateRecord& draw,
    EncodeScope scope) noexcept {
  Facts result;
  static_assert(result.sampledTextures.size() == core::kMaxTextures);
  static_assert(result.followingColorLoads.size() == core::kMaxRenderTargets);
  const auto owner = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&queue));
  result.clearSource = projectSource(owner, clearCommand);
  result.drawSource = projectSource(owner, drawCommand);
  result.clearIndex = clearCommand.commandIndex;
  result.drawIndex = drawCommand.commandIndex;
  result.fullRect = clear.rects.empty();
  result.requestedAspects = (clear.clearColor ? Color : 0) |
      (clear.clearDepth ? Depth : 0) | (clear.clearStencil ? Stencil : 0);
  result.activeEncoder = scope.activeEncoder;
  result.queryScope = scope.queryScope;
  result.exactReadHazard = scope.exactReadHazard;
  result.srgbWrite = core::flatStateOr(draw.renderStates, core::RS_SRGB_WRITE_ENABLE, 0u) != 0;
  result.backbufferDiscardPending = queue.backBufferDiscardAfterPresent_ &&
      draw.colorAttachments[0].handle == queue.currentBackBuffer_;
  for (std::size_t i = 0; i < core::kMaxRenderTargets; ++i) {
    const auto c = clear.colorAttachments[i];
    const auto d = draw.colorAttachments[i];
    result.draw[i] = projectAttachment(d, pool.findSurface(d.handle.value));
    result.clear[i] = c == d ? result.draw[i]
        : projectAttachment(c, pool.findSurface(c.handle.value));
    result.followingColorLoads[i] = encoders::resolveColorAttachmentLoadAction(
        false, result.backbufferDiscardPending && i == 0,
        queue.isColorHandleTouched(d.handle), queue.renderTapeExactAttachmentPreservation())
        == WMTLoadActionLoad ? Load::Preserve : Load::Discard;
  }
  result.draw[4] = projectAttachment(draw.depthStencil,
      pool.findSurface(draw.depthStencil.handle.value));
  result.clear[4] = clear.depthStencil == draw.depthStencil ? result.draw[4]
      : projectAttachment(clear.depthStencil,
          pool.findSurface(clear.depthStencil.handle.value));
  // Conservative: include every bound slot, even if the shader won't sample it.
  for (std::size_t i = 0; i < core::kMaxTextures; ++i)
    result.sampledTextures[i] = draw.textures[i].value;
  return result;
}

} // namespace dxmt9::pending_clear
