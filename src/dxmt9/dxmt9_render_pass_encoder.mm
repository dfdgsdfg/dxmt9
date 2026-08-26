#import <Metal/Metal.h>

#include "dxmt9_render_pass_internal.hpp"

#include "dxmt9/assert.hpp"
#include "dxmt9_debug_trace.hpp"
#include "dxmt9_draw_encoder_diagnostics.hpp"
#include "dxmt9_format_convert.hpp"
#include "dxmt9_perf_counters.hpp"
#include "dxmt9_resource_pool.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <limits>
#include <optional>
#include <span>

namespace dxmt9::encoders {

using core::ClearDesc;
using core::Handle;
using dxmt9::convert::formatHasDepthAspect;
using dxmt9::convert::formatHasStencilAspect;
using dxmt9::ffp::decodeFixedFunctionVertexLayout;

namespace {

bool clearMatchesColorAttachment(const std::optional<ClearDesc>& clear, std::size_t index,
                                 Handle attachment) {
  return clear.has_value() && clear->clearColor && attachment &&
         clear->colorAttachments[index].handle == attachment;
}

bool clearMatchesDepthStencilAttachment(const std::optional<ClearDesc>& clear, Handle attachment,
                                        bool clearStencil) {
  if (!clear.has_value() || !attachment) {
    return false;
  }
  const bool requested = clearStencil ? clear->clearStencil : clear->clearDepth;
  return requested && clear->depthStencil.handle == attachment;
}

}  // namespace

// R-BACK-15.7 / spec section 4.2: depth/stencil DontCare-store look-ahead.
// Walks the remaining records in the current chunk starting from
// `startCommandIndex + 1` and returns true when one of two proofs holds:
//
//   1. The next record that touches `depthHandle` is a Clear of that
//      handle (the original G3 simple-form shortcut).
//   2. The depth handle never reappears in the rest of the chunk AND
//      no Present was seen during the walk (H1 broadening, R-BACK-15.7
//      end-of-chunk fall-through). R-BACK-15.9 still applies — no
//      cross-chunk look-ahead — but within the current chunk the depth
//      is provably dead, so DontCare-store is safe.
//
// Any prior live read or surface op on the handle (Readback /
// SurfaceCopy / StretchRect / ColorFill source-or-dest, a DrawRun that
// re-binds the handle as depth target, or a DrawRun that samples it as
// a shadow-map texture) flips the proof to defensive Store.
//
// Public so the G4 render-pass-actions fixture can exercise the
// contract without a Metal device.
perf::RenderPassDepthStoreProof depthStoreProofForLookahead(
    std::span<const RenderPassStoreProofLookaheadSource> sources, core::Handle depthHandle,
    std::uint32_t* firstTouchCommandDistance, core::Handle attachmentAliasTexture,
    RenderPassStoreProofActivePass activePass, LateRenderPassStoreAspect aspect,
    perf::RenderPassNoLookaheadCause* noLookaheadCause) {
  using Proof = perf::RenderPassDepthStoreProof;
  const auto noTouch = std::numeric_limits<std::uint32_t>::max();
  if (firstTouchCommandDistance) {
    *firstTouchCommandDistance = noTouch;
  }
  auto finish = [&](Proof proof, std::size_t logicalDistance) {
    if (firstTouchCommandDistance) {
      *firstTouchCommandDistance =
          static_cast<std::uint32_t>(std::min<std::size_t>(logicalDistance, noTouch - 1u));
    }
    return proof;
  };
  if (!depthHandle) {
    return Proof::BlockNullDepth;
  }
  if (activePass.lookaheadInvalid) {
    if (noLookaheadCause) {
      *noLookaheadCause = perf::RenderPassNoLookaheadCause::Invalid;
    }
    return Proof::BlockNoLookahead;
  }
  if (sources.empty()) {
    if (noLookaheadCause) {
      *noLookaheadCause = perf::RenderPassNoLookaheadCause::Empty;
    }
    return Proof::BlockNoLookahead;
  }
  using Kind = core::MetalCommandKind;
  bool sawPresent = false;
  bool withinActivePass = activePass.hot != nullptr && activePass.allowSameAttachmentContinuation;
  const auto activePassKey = activePass.hot ? makeAttachmentKey(*activePass.hot) : AttachmentKey{};
  const auto activePassWriteHazard =
      activePass.hot ? makeAttachmentHazard(*activePass.hot) : HazardProbe{};
  std::size_t logicalDistance = 0;
  for (const auto& source : sources) {
    const core::SourcePayloadView sourcePayload = source.payload.valid() ? source.payload
                                                  : source.slot
                                                      ? core::SourcePayloadView(*source.slot)
                                                      : core::SourcePayloadView{};
    if (!sourcePayload.valid()) {
      if (noLookaheadCause) {
        *noLookaheadCause = perf::RenderPassNoLookaheadCause::Invalid;
      }
      return Proof::BlockNoLookahead;
    }
    const std::size_t traversalCount =
        source.commandOrder.empty() ? sourcePayload.commandCount() : source.commandOrder.size();
    const std::size_t firstCommandIndex = std::min(source.firstCommandIndex, traversalCount);
    const std::size_t commandEndIndex = std::min(source.commandEndIndex, traversalCount);
    for (std::size_t traversalIndex = firstCommandIndex; traversalIndex < commandEndIndex;
         ++traversalIndex) {
      const std::size_t commandIndex =
          source.commandOrder.empty()
              ? traversalIndex
              : static_cast<std::size_t>(source.commandOrder[traversalIndex]);
      if (commandIndex >= sourcePayload.commandCount()) {
        if (noLookaheadCause) {
          *noLookaheadCause = perf::RenderPassNoLookaheadCause::Invalid;
        }
        return Proof::BlockNoLookahead;
      }
      ++logicalDistance;
      const auto sourceCommand = sourcePayload.commandAt(commandIndex);
      const auto& next = sourceCommand.command;
      if (next.kind != Kind::DrawRun) {
        withinActivePass = false;
      }
      switch (next.kind) {
      case Kind::Clear:
        if (sourceCommand.clear) {
          if (sourceCommand.clear->depthStencil.handle != depthHandle) {
            break;
          }
          const bool matchingAspect = aspect == LateRenderPassStoreAspect::Stencil
                                          ? sourceCommand.clear->clearStencil
                                          : sourceCommand.clear->clearDepth;
          if (sourceCommand.clear->rects.empty() && matchingAspect) {
            // R-BACK-15.7: a full clear of this exact aspect discards the
            // stored contents. A partial or other-aspect clear does not.
            return finish(Proof::AllowNextClear, logicalDistance);
          }
          return finish(Proof::BlockClearMismatch, logicalDistance);
        }
        break;
      case Kind::DrawRun:
        if (next.drawState.hot) {
          const auto& hot = *next.drawState.hot;
          const auto samplesDepth = [&]() {
            const auto& textures = hot.textures;
            const std::uint32_t mask = hot.textureMask;
            for (std::size_t s = 0; s < textures.size(); ++s) {
              if ((mask & (1u << s)) == 0)
                continue;
              if (textures[s] == depthHandle ||
                  (attachmentAliasTexture && textures[s] == attachmentAliasTexture)) {
                return true;
              }
            }
            return false;
          };
          if (withinActivePass) {
            const bool sameAttachments = makeAttachmentKey(hot) == activePassKey;
            const bool exactHazard =
                activePassWriteHazard.exactOverlaps(makeDrawReadHazard(next.drawState));
            if (sameAttachments && !exactHazard && !samplesDepth()) {
              // This record extends the encoder whose Store action we are
              // deciding. It is not a live-out depth use.
              break;
            }
            withinActivePass = false;
          }
          if (next.drawState.hot->depthStencil.handle == depthHandle) {
            // Depth is read by a subsequent draw — must Store.
            return finish(Proof::BlockDrawDepth, logicalDistance);
          }
          // R-BACK-15.7 extension: depth-as-shadow-map sample. Walk the
          // active texture bindings and bail if any matches the depth
          // handle (the depth surface is sampled as a texture by this
          // later draw, so its tile contents must be preserved).
          if (samplesDepth()) {
            return finish(Proof::BlockShadowSample, logicalDistance);
          }
        }
        break;
      case Kind::SurfaceCopy:
        if (next.surfaceCopy && (next.surfaceCopy->source == depthHandle ||
                                 next.surfaceCopy->destination == depthHandle)) {
          return finish(Proof::BlockSurfaceCopy, logicalDistance);
        }
        break;
      case Kind::StretchRect:
        if (next.stretchRect && (next.stretchRect->source == depthHandle ||
                                 next.stretchRect->destination == depthHandle)) {
          return finish(Proof::BlockStretchRect, logicalDistance);
        }
        break;
      case Kind::Readback:
        // R-BACK-15.15: host-visible read of the depth surface must not
        // be served from a DontCare-stored tile.
        if (next.readback &&
            (next.readback->source == depthHandle || next.readback->destination == depthHandle)) {
          return finish(Proof::BlockReadback, logicalDistance);
        }
        break;
      case Kind::ColorFill:
        if (next.colorFill && next.colorFill->destination == depthHandle) {
          return finish(Proof::BlockColorFill, logicalDistance);
        }
        break;
      case Kind::DepthResolve:
        // R-FORMAT-11: a later RESZ resolve reads the MSAA depth surface as
        // its source (and writes the INTZ destination). If either endpoint is
        // this depth handle its tile contents must survive — force a Store
        // exactly like the StretchRect/Readback depth-touch cases above.
        if (next.depthResolve && (next.depthResolve->msaaDepth == depthHandle ||
                                  next.depthResolve->intzDest == depthHandle)) {
          return finish(Proof::BlockDepthResolve, logicalDistance);
        }
        break;
      case Kind::Present:
        // R-BACK-15.13: a Present in the selected logical suffix implies the
        // frame may persist depth state across that boundary. Don't return
        // early — a later Clear on the same handle still wins.
        sawPresent = true;
        break;
      }
    }
  }
  // A deferred EncodeSession may append an unobserved FIFO source. Exact
  // touches found above are conclusive, but exhausting only the retained
  // prefix cannot prove that the attachment is dead.
  if (activePass.lookaheadMayHaveFutureSources) {
    if (noLookaheadCause) {
      *noLookaheadCause =
          activePass.lookaheadStorageTruncated ? perf::RenderPassNoLookaheadCause::StorageTruncated
          : activePass.lookaheadInvalid        ? perf::RenderPassNoLookaheadCause::Invalid
                                               : perf::RenderPassNoLookaheadCause::SuffixExhausted;
    }
    return Proof::BlockNoLookahead;
  }
  // R-BACK-15.7 end-of-chunk fall-through. Default keeps the defensive
  // sawPresent guard: a Present in this chunk implies the frame may
  // persist depth state across the chunk boundary (cross-frame shadow
  // map / depth-test reuse), so we Store. Setting
  // DXMT9_AGGRESSIVE_DEPTH_DONTCARE=1 drops the guard so the look-ahead
  // returns DontCare whenever the depth handle does not reappear in
  // the chunk, even when a Present is present. Empirically this is the
  // SFIV win path (Present-per-chunk pattern zeroes the conservative
  // form). Use only on workloads known not to read depth across
  // frames; depth-as-shadow-map within the same chunk is still
  // protected by the texture-sample scan above.
  static const bool aggressive = []() {
    if (const char* v = std::getenv("DXMT9_AGGRESSIVE_DEPTH_DONTCARE")) {
      return v[0] != '\0' && v[0] != '0';
    }
    return false;
  }();
  if (aggressive) {
    return Proof::AllowDeadNoPresent;
  }
  return sawPresent ? Proof::BlockPresent : Proof::AllowDeadNoPresent;
}

perf::RenderPassDepthStoreProof depthStoreProofForLookahead(
    const core::ChunkSlot& slot, std::size_t startCommandIndex, core::Handle depthHandle,
    std::uint32_t* firstTouchCommandDistance, core::Handle attachmentAliasTexture,
    RenderPassStoreProofActivePass activePass, LateRenderPassStoreAspect aspect,
    perf::RenderPassNoLookaheadCause* noLookaheadCause) {
  const std::size_t firstCommandIndex =
      startCommandIndex < slot.commandCount() ? startCommandIndex + 1u : slot.commandCount();
  const RenderPassStoreProofLookaheadSource source{
      .slot = &slot,
      .firstCommandIndex = firstCommandIndex,
      .commandEndIndex = slot.commandCount(),
  };
  return depthStoreProofForLookahead(
      std::span<const RenderPassStoreProofLookaheadSource>(&source, 1u), depthHandle,
      firstTouchCommandDistance, attachmentAliasTexture, activePass, aspect, noLookaheadCause);
}

bool nextDepthOperationIsClear(const core::ChunkSlot& slot, std::size_t startCommandIndex,
                               core::Handle depthHandle) {
  const auto proof = depthStoreProofForLookahead(slot, startCommandIndex, depthHandle);
  return proof == perf::RenderPassDepthStoreProof::AllowNextClear ||
         proof == perf::RenderPassDepthStoreProof::AllowDeadNoPresent;
}

perf::RenderPassColorStoreProof colorStoreProofForLookahead(
    std::span<const RenderPassStoreProofLookaheadSource> sources, core::Handle colorHandle,
    std::uint32_t* firstTouchCommandDistance, core::Handle attachmentAliasTexture,
    RenderPassStoreProofActivePass activePass, std::uint8_t colorAttachmentIndex,
    perf::RenderPassNoLookaheadCause* noLookaheadCause) {
  using Proof = perf::RenderPassColorStoreProof;
  const auto noTouch = std::numeric_limits<std::uint32_t>::max();
  if (firstTouchCommandDistance) {
    *firstTouchCommandDistance = noTouch;
  }
  auto finish = [&](Proof proof, std::size_t logicalDistance) {
    if (firstTouchCommandDistance) {
      *firstTouchCommandDistance =
          static_cast<std::uint32_t>(std::min<std::size_t>(logicalDistance, noTouch - 1u));
    }
    return proof;
  };
  if (!colorHandle) {
    return Proof::BlockNullColor;
  }
  if (activePass.lookaheadInvalid) {
    if (noLookaheadCause) {
      *noLookaheadCause = perf::RenderPassNoLookaheadCause::Invalid;
    }
    return Proof::BlockNoLookahead;
  }
  if (sources.empty()) {
    if (noLookaheadCause) {
      *noLookaheadCause = perf::RenderPassNoLookaheadCause::Empty;
    }
    return Proof::BlockNoLookahead;
  }
  using Kind = core::MetalCommandKind;
  bool sawPresent = false;
  bool withinActivePass = activePass.hot != nullptr && activePass.allowSameAttachmentContinuation;
  const auto activePassKey = activePass.hot ? makeAttachmentKey(*activePass.hot) : AttachmentKey{};
  const auto activePassWriteHazard =
      activePass.hot ? makeAttachmentHazard(*activePass.hot) : HazardProbe{};
  std::size_t logicalDistance = 0;
  for (const auto& source : sources) {
    const core::SourcePayloadView sourcePayload = source.payload.valid() ? source.payload
                                                  : source.slot
                                                      ? core::SourcePayloadView(*source.slot)
                                                      : core::SourcePayloadView{};
    if (!sourcePayload.valid()) {
      if (noLookaheadCause) {
        *noLookaheadCause = perf::RenderPassNoLookaheadCause::Invalid;
      }
      return Proof::BlockNoLookahead;
    }
    const std::size_t traversalCount =
        source.commandOrder.empty() ? sourcePayload.commandCount() : source.commandOrder.size();
    const std::size_t firstCommandIndex = std::min(source.firstCommandIndex, traversalCount);
    const std::size_t commandEndIndex = std::min(source.commandEndIndex, traversalCount);
    for (std::size_t traversalIndex = firstCommandIndex; traversalIndex < commandEndIndex;
         ++traversalIndex) {
      const std::size_t commandIndex =
          source.commandOrder.empty()
              ? traversalIndex
              : static_cast<std::size_t>(source.commandOrder[traversalIndex]);
      if (commandIndex >= sourcePayload.commandCount()) {
        if (noLookaheadCause) {
          *noLookaheadCause = perf::RenderPassNoLookaheadCause::Invalid;
        }
        return Proof::BlockNoLookahead;
      }
      ++logicalDistance;
      const auto sourceCommand = sourcePayload.commandAt(commandIndex);
      const auto& next = sourceCommand.command;
      if (next.kind != Kind::DrawRun) {
        withinActivePass = false;
      }
      switch (next.kind) {
      case Kind::Clear:
        if (sourceCommand.clear) {
          bool handleAppears = false;
          for (const auto& attachment : sourceCommand.clear->colorAttachments) {
            handleAppears = handleAppears || attachment.handle == colorHandle;
          }
          if (!handleAppears) {
            break;
          }
          const bool matchingSlot =
              colorAttachmentIndex < sourceCommand.clear->colorAttachments.size() &&
              sourceCommand.clear->colorAttachments[colorAttachmentIndex].handle == colorHandle;
          return finish(sourceCommand.clear->rects.empty() && sourceCommand.clear->clearColor &&
                                matchingSlot
                            ? Proof::AllowNextClear
                            : Proof::BlockClearMismatch,
                        logicalDistance);
        }
        break;
      case Kind::DrawRun:
        if (next.drawState.hot) {
          const auto& hot = *next.drawState.hot;
          const auto samplesColor = [&]() {
            const auto& textures = hot.textures;
            const std::uint32_t mask = hot.textureMask;
            for (std::size_t s = 0; s < textures.size(); ++s) {
              if ((mask & (1u << s)) == 0)
                continue;
              if (textures[s] == colorHandle ||
                  (attachmentAliasTexture && textures[s] == attachmentAliasTexture)) {
                return true;
              }
            }
            return false;
          };
          if (withinActivePass) {
            const bool sameAttachments = makeAttachmentKey(hot) == activePassKey;
            const bool exactHazard =
                activePassWriteHazard.exactOverlaps(makeDrawReadHazard(next.drawState));
            if (sameAttachments && !exactHazard && !samplesColor()) {
              // Same-pass draws consume neither system memory nor a later
              // Load, so they cannot block a following next-clear proof.
              break;
            }
            withinActivePass = false;
          }
          for (const auto& attachment : hot.colorAttachments) {
            if (attachment.handle == colorHandle) {
              return finish(Proof::BlockDrawTarget, logicalDistance);
            }
          }
          if (samplesColor()) {
            return finish(Proof::BlockTextureSample, logicalDistance);
          }
        }
        break;
      case Kind::SurfaceCopy:
        if (next.surfaceCopy && (next.surfaceCopy->source == colorHandle ||
                                 next.surfaceCopy->destination == colorHandle)) {
          return finish(Proof::BlockSurfaceCopy, logicalDistance);
        }
        break;
      case Kind::StretchRect:
        if (next.stretchRect && (next.stretchRect->source == colorHandle ||
                                 next.stretchRect->destination == colorHandle)) {
          return finish(Proof::BlockStretchRect, logicalDistance);
        }
        break;
      case Kind::Readback:
        if (next.readback &&
            (next.readback->source == colorHandle || next.readback->destination == colorHandle)) {
          return finish(Proof::BlockReadback, logicalDistance);
        }
        break;
      case Kind::ColorFill:
        if (next.colorFill && next.colorFill->destination == colorHandle) {
          return finish(Proof::BlockColorFill, logicalDistance);
        }
        break;
      case Kind::Present:
        if (next.present && next.present->presentSource == colorHandle) {
          return finish(Proof::BlockPresent, logicalDistance);
        }
        sawPresent = true;
        break;
      case Kind::DepthResolve:
        break;
      }
    }
  }
  if (activePass.lookaheadMayHaveFutureSources) {
    if (noLookaheadCause) {
      *noLookaheadCause =
          activePass.lookaheadStorageTruncated ? perf::RenderPassNoLookaheadCause::StorageTruncated
          : activePass.lookaheadInvalid        ? perf::RenderPassNoLookaheadCause::Invalid
                                               : perf::RenderPassNoLookaheadCause::SuffixExhausted;
    }
    return Proof::BlockNoLookahead;
  }
  static const bool aggressive = []() {
    if (const char* v = std::getenv("DXMT9_AGGRESSIVE_COLOR_DONTCARE")) {
      return v[0] != '\0' && v[0] != '0';
    }
    return false;
  }();
  if (sawPresent) {
    return Proof::BlockPresent;
  }
  return aggressive ? Proof::AllowDeadNoPresent : Proof::BlockDeadNoPresentDisabled;
}

perf::RenderPassColorStoreProof colorStoreProofForLookahead(
    const core::ChunkSlot& slot, std::size_t startCommandIndex, core::Handle colorHandle,
    std::uint32_t* firstTouchCommandDistance, core::Handle attachmentAliasTexture,
    RenderPassStoreProofActivePass activePass, std::uint8_t colorAttachmentIndex,
    perf::RenderPassNoLookaheadCause* noLookaheadCause) {
  const std::size_t firstCommandIndex =
      startCommandIndex < slot.commandCount() ? startCommandIndex + 1u : slot.commandCount();
  const RenderPassStoreProofLookaheadSource source{
      .slot = &slot,
      .firstCommandIndex = firstCommandIndex,
      .commandEndIndex = slot.commandCount(),
  };
  return colorStoreProofForLookahead(
      std::span<const RenderPassStoreProofLookaheadSource>(&source, 1u), colorHandle,
      firstTouchCommandDistance, attachmentAliasTexture, activePass, colorAttachmentIndex,
      noLookaheadCause);
}

bool nextColorOperationIsClear(const core::ChunkSlot& slot, std::size_t startCommandIndex,
                               core::Handle colorHandle) {
  return colorStoreProofForLookahead(slot, startCommandIndex, colorHandle) ==
         perf::RenderPassColorStoreProof::AllowNextClear;
}

RenderPassStoreProofSummary renderPassStoreProofSummaryForLookahead(
    EncodeContext& ctx, std::span<const RenderPassStoreProofLookaheadSource> lookaheadSources,
    const core::FlatDrawStateRecord& hot, RenderPassStoreProofActivePass activePass) {
  RenderPassStoreProofSummary summary{};
  auto* colorSurface = ctx.pool.findSurface(hot.colorAttachments[0].handle.value);
  const bool colorIncluded =
      colorSurface && colorSurface->texture && colorSurface->desc.format != core::Format::NullRt;
  if (colorIncluded) {
    summary.color = !lookaheadSources.empty()
                        ? colorStoreProofForLookahead(
                              lookaheadSources, hot.colorAttachments[0].handle,
                              &summary.colorTouchDistance, colorSurface->aliasTexture, activePass)
                        : perf::RenderPassColorStoreProof::BlockNoLookahead;
    if (colorSurface->resolveTexture &&
        (summary.color == perf::RenderPassColorStoreProof::AllowNextClear ||
         summary.color == perf::RenderPassColorStoreProof::AllowDeadNoPresent)) {
      summary.color = perf::RenderPassColorStoreProof::BlockMsaaResolve;
    }
  }

  auto* depthSurface = ctx.pool.findSurface(hot.depthStencil.handle.value);
  if (depthSurface && depthSurface->texture && depthSurface->desc.depthStencil) {
    summary.depth = !lookaheadSources.empty()
                        ? depthStoreProofForLookahead(lookaheadSources, hot.depthStencil.handle,
                                                      &summary.depthTouchDistance,
                                                      depthSurface->aliasTexture, activePass)
                        : perf::RenderPassDepthStoreProof::BlockNoLookahead;
    if (depthSurface->resolveTexture &&
        (summary.depth == perf::RenderPassDepthStoreProof::AllowNextClear ||
         summary.depth == perf::RenderPassDepthStoreProof::AllowDeadNoPresent)) {
      summary.depth = perf::RenderPassDepthStoreProof::BlockMsaaResolve;
    }
  }
  return summary;
}

bool prepareRenderPassWithStoreProofLookahead(
    EncodeContext& ctx, core::FlatDrawStateView drawState,
    const std::optional<ClearDesc>& clear,
    std::span<const RenderPassStoreProofLookaheadSource> lookaheadSources,
    RenderPassStoreProofActivePass activePass,
    std::span<const WMTSampleBufferAttachmentInfo> sampleBufferAttachments,
    WMT::Buffer visibilityBuffer, PreparedRenderPass& prepared) {
  prepared = {};
  auto* actionSummary = &prepared.actions;
  auto* lateStoreState = &prepared.lateStore;
  const auto& hot = *drawState.hot;
  auto* primarySurface = ctx.pool.findSurface(hot.colorAttachments[0].handle.value);
  // R-FORMAT-12: a D3DFMT_NULL render target is colorless and has no Metal
  // color texture by design. When RT0 is a NULL render target the render
  // pass is depth/stencil-only — proceed (the per-attachment loop below
  // omits every color attachment that has no texture, so the NULL RT
  // contributes no color attachment and the bound depth/stencil becomes
  // the effective target). Only abort when RT0 is genuinely missing, or it
  // is a normal color RT that failed to allocate its texture.
  const ColorlessRenderPassRt0 rt0{
      .surfaceExists = primarySurface != nullptr,
      .hasTexture = primarySurface && static_cast<bool>(primarySurface->texture),
      .isNullRt = primarySurface && primarySurface->desc.format == core::Format::NullRt,
  };
  if (!renderPassAdmitsRt0(rt0)) {
    return false;
  }
  auto& passInfo = prepared.info;
  const bool discardAfterPresent = !clear.has_value() && ctx.queue.backBufferDiscardAfterPresent_ &&
                                   hot.colorAttachments[0].handle == ctx.queue.currentBackBuffer_;
  for (std::size_t i = 0; i < core::kMaxRenderTargets; ++i) {
    auto* surface = ctx.pool.findSurface(hot.colorAttachments[i].handle.value);
    // R-FORMAT-12: omit any color slot whose surface owns no backend texture
    // (a NULL render target is the colorless case). See colorAttachmentIncluded.
    if (!colorAttachmentIncluded(surface != nullptr,
                                 surface && static_cast<bool>(surface->texture))) {
      continue;
    }
    auto& attachment = passInfo.colors[i];
    const bool srgbWrite = core::flatStateOr(hot.renderStates, core::RS_SRGB_WRITE_ENABLE, 0u) != 0;
    attachment.texture =
        (srgbWrite && surface->srgbTexture) ? surface->srgbTexture.handle : surface->texture.handle;
    const bool discardAttachment = discardAfterPresent && i == 0;
    const bool clearAttachment =
        clearMatchesColorAttachment(clear, i, hot.colorAttachments[i].handle);
    // R-BACK-15.4: first-use of a color RT (handle not yet in the
    // queue-local touched set) may DontCare-load. Render Tape exact mode
    // preserves its captured seed instead. Precedence:
    // Clear > post-present DontCare > tape Load > first-use DontCare > Load.
    attachment.load_action = resolveColorAttachmentLoadAction(
        clearAttachment, discardAttachment,
        ctx.queue.isColorHandleTouched(hot.colorAttachments[i].handle),
        ctx.queue.renderTapeExactAttachmentPreservation());
    perf::RenderPassNoLookaheadCause colorNoLookaheadCause =
        perf::RenderPassNoLookaheadCause::Empty;
    auto colorStoreProof =
        !lookaheadSources.empty()
            ? colorStoreProofForLookahead(lookaheadSources, hot.colorAttachments[i].handle, nullptr,
                                          surface->aliasTexture, activePass,
                                          static_cast<std::uint8_t>(i), &colorNoLookaheadCause)
            : perf::RenderPassColorStoreProof::BlockNoLookahead;
    if (surface->resolveTexture &&
        (colorStoreProof == perf::RenderPassColorStoreProof::AllowNextClear ||
         colorStoreProof == perf::RenderPassColorStoreProof::AllowDeadNoPresent)) {
      colorStoreProof = perf::RenderPassColorStoreProof::BlockMsaaResolve;
    }
    perf::countRenderPassColorStoreProof(colorStoreProof);
    if (colorStoreProof == perf::RenderPassColorStoreProof::BlockNoLookahead) {
      perf::countRenderPassNoLookaheadCause(colorNoLookaheadCause);
    }
    const bool colorDontCareStore =
        !ctx.queue.renderTapeExactAttachmentPreservation() &&
        (colorStoreProof == perf::RenderPassColorStoreProof::AllowNextClear ||
         colorStoreProof == perf::RenderPassColorStoreProof::AllowDeadNoPresent);
    const bool colorLateStoreEligible = lateRenderPassStoreEligible(
        colorStoreProof == perf::RenderPassColorStoreProof::BlockNoLookahead, colorNoLookaheadCause,
        activePass.lookaheadMayHaveFutureSources, static_cast<bool>(surface->resolveTexture),
        hot.colorAttachments[i].handle == ctx.queue.currentBackBuffer_);
    attachment.store_action = colorDontCareStore       ? WMTStoreActionDontCare
                              : colorLateStoreEligible ? WMTStoreActionUnknown
                                                       : WMTStoreActionStore;
    if (surface->resolveTexture) {
      attachment.resolve_texture = surface->resolveTexture.handle;
      attachment.store_action = WMTStoreActionMultisampleResolve;
    }
    if (actionSummary) {
      ++actionSummary->colorAttachmentCount;
      if (i == 0) {
        actionSummary->color0Included = 1;
        actionSummary->color0LoadAction = static_cast<std::uint64_t>(attachment.load_action);
        actionSummary->color0StoreAction = static_cast<std::uint64_t>(attachment.store_action);
        actionSummary->color0Clear = clearAttachment ? 1u : 0u;
      }
    }
    if (clearAttachment) {
      attachment.clear_color =
          WMTClearColor{clear->color.r, clear->color.g, clear->color.b, clear->color.a};
    }
    if (lateStoreState) {
      const std::uint64_t pixelBytes =
          static_cast<std::uint64_t>(surface->desc.width) *
          static_cast<std::uint64_t>(surface->desc.height) *
          static_cast<std::uint64_t>(core::bytesPerPixel(surface->desc.format));
      if (!lateStoreState->append({
              .handle = hot.colorAttachments[i].handle,
              .aliasTexture = surface->aliasTexture,
              .pixelBytes = pixelBytes,
              .action = attachment.store_action,
              .aspect = LateRenderPassStoreAspect::Color,
              .colorIndex = static_cast<std::uint8_t>(i),
        })) {
        DXMT_ASSERT(false && "late Store attachment ledger overflow");
        return false;
      }
    }
  }

  if (auto* depthSurface = ctx.pool.findSurface(hot.depthStencil.handle.value);
      depthSurface && depthSurface->texture && depthSurface->desc.depthStencil) {
    const bool clearDepth =
        clearMatchesDepthStencilAttachment(clear, hot.depthStencil.handle, false);
    const bool clearStencil =
        clearMatchesDepthStencilAttachment(clear, hot.depthStencil.handle, true);
    // R-BACK-15.7 simple form (specs/backend/render-pass-actions/spec.md
    // section 4.2): in-chunk look-ahead — if the very next op on this
    // depth handle is a Clear, the about-to-be-stored tile contents are
    // immediately discarded, so we can DontCare-store. R-BACK-15.14:
    // never DontCare an MSAA depth target with an attached resolve.
    const bool hasResolveTarget = static_cast<bool>(depthSurface->resolveTexture);
    perf::RenderPassNoLookaheadCause depthNoLookaheadCause =
        perf::RenderPassNoLookaheadCause::Empty;
    perf::RenderPassNoLookaheadCause stencilNoLookaheadCause =
        perf::RenderPassNoLookaheadCause::Empty;
    const auto proofForAspect = [&](LateRenderPassStoreAspect aspect,
                                    perf::RenderPassNoLookaheadCause& cause) {
      auto proof =
          !lookaheadSources.empty()
              ? depthStoreProofForLookahead(lookaheadSources, hot.depthStencil.handle, nullptr,
                                            depthSurface->aliasTexture, activePass, aspect, &cause)
              : perf::RenderPassDepthStoreProof::BlockNoLookahead;
      if (hasResolveTarget && (proof == perf::RenderPassDepthStoreProof::AllowNextClear ||
                               proof == perf::RenderPassDepthStoreProof::AllowDeadNoPresent)) {
        proof = perf::RenderPassDepthStoreProof::BlockMsaaResolve;
      }
      return proof;
    };
    const bool hasDepthAspect = formatHasDepthAspect(depthSurface->desc.format);
    const bool hasStencilAspect = formatHasStencilAspect(depthSurface->desc.format);
    const auto depthStoreProof =
        hasDepthAspect ? proofForAspect(LateRenderPassStoreAspect::Depth, depthNoLookaheadCause)
                       : perf::RenderPassDepthStoreProof::BlockNullDepth;
    const auto stencilStoreProof =
        hasStencilAspect
            ? proofForAspect(LateRenderPassStoreAspect::Stencil, stencilNoLookaheadCause)
            : perf::RenderPassDepthStoreProof::BlockNullDepth;
    // Preserve the historical denominator: render_pass_depth_proof_* is one
    // observation per included depth/stencil pass, not one per DS aspect.
    const auto legacyDepthStoreProof =
        legacyDepthProofForPass(hasDepthAspect, depthStoreProof, stencilStoreProof);
    const auto legacyNoLookaheadCause =
        hasDepthAspect ? depthNoLookaheadCause : stencilNoLookaheadCause;
    perf::countRenderPassDepthStoreProof(legacyDepthStoreProof);
    if (legacyDepthStoreProof == perf::RenderPassDepthStoreProof::BlockNoLookahead) {
      perf::countRenderPassNoLookaheadCause(legacyNoLookaheadCause);
    }
    const auto allowsDontCare = [&](perf::RenderPassDepthStoreProof proof) {
      return !ctx.queue.renderTapeExactAttachmentPreservation() &&
             !hasResolveTarget &&
             (proof == perf::RenderPassDepthStoreProof::AllowNextClear ||
              proof == perf::RenderPassDepthStoreProof::AllowDeadNoPresent);
    };
    const auto allowsLateStore = [&](perf::RenderPassDepthStoreProof proof,
                                     perf::RenderPassNoLookaheadCause cause) {
      return lateRenderPassStoreEligible(proof == perf::RenderPassDepthStoreProof::BlockNoLookahead,
                                         cause, activePass.lookaheadMayHaveFutureSources,
                                         hasResolveTarget,
                                         /*presentSourceConstrained=*/false);
    };
    const std::uint64_t depthPixelBytes =
        static_cast<std::uint64_t>(depthSurface->desc.width) *
        static_cast<std::uint64_t>(depthSurface->desc.height) *
        static_cast<std::uint64_t>(core::bytesPerPixel(depthSurface->desc.format));
    if (hasDepthAspect) {
      passInfo.depth.texture = depthSurface->texture.handle;
      passInfo.depth.load_action = clearDepth ? WMTLoadActionClear : WMTLoadActionLoad;
      passInfo.depth.store_action = allowsDontCare(depthStoreProof) ? WMTStoreActionDontCare
                                    : allowsLateStore(depthStoreProof, depthNoLookaheadCause)
                                        ? WMTStoreActionUnknown
                                        : WMTStoreActionStore;
      if (actionSummary) {
        actionSummary->depthIncluded = 1;
        actionSummary->depthLoadAction = static_cast<std::uint64_t>(passInfo.depth.load_action);
        actionSummary->depthStoreAction = static_cast<std::uint64_t>(passInfo.depth.store_action);
        actionSummary->depthClear = clearDepth ? 1u : 0u;
      }
      if (clearDepth) {
        passInfo.depth.clear_depth = clear->depth;
      }
      if (lateStoreState) {
        if (!lateStoreState->append({
                .handle = hot.depthStencil.handle,
                .aliasTexture = depthSurface->aliasTexture,
                .pixelBytes = depthPixelBytes,
                .action = passInfo.depth.store_action,
                .aspect = LateRenderPassStoreAspect::Depth,
            })) {
          DXMT_ASSERT(false && "late Store attachment ledger overflow");
          return false;
        }
      }
    }
    if (hasStencilAspect) {
      passInfo.stencil.texture = depthSurface->texture.handle;
      passInfo.stencil.load_action = clearStencil ? WMTLoadActionClear : WMTLoadActionLoad;
      passInfo.stencil.store_action = allowsDontCare(stencilStoreProof) ? WMTStoreActionDontCare
                                      : allowsLateStore(stencilStoreProof, stencilNoLookaheadCause)
                                          ? WMTStoreActionUnknown
                                          : WMTStoreActionStore;
      if (actionSummary) {
        actionSummary->stencilIncluded = 1;
        actionSummary->stencilLoadAction = static_cast<std::uint64_t>(passInfo.stencil.load_action);
        actionSummary->stencilStoreAction =
            static_cast<std::uint64_t>(passInfo.stencil.store_action);
        actionSummary->stencilClear = clearStencil ? 1u : 0u;
      }
      if (clearStencil) {
        passInfo.stencil.clear_stencil = clear->stencil;
      }
      if (lateStoreState) {
        if (!lateStoreState->append({
                .handle = hot.depthStencil.handle,
                .aliasTexture = depthSurface->aliasTexture,
                .pixelBytes = depthPixelBytes,
                .action = passInfo.stencil.store_action,
                .aspect = LateRenderPassStoreAspect::Stencil,
            })) {
          DXMT_ASSERT(false && "late Store attachment ledger overflow");
          return false;
        }
      }
    }
  }

  constexpr std::size_t kMaxSampleBufferAttachments =
      sizeof(passInfo.sample_buffer_attachments) / sizeof(passInfo.sample_buffer_attachments[0]);
  const auto attachmentCount =
      std::min<std::size_t>(sampleBufferAttachments.size(), kMaxSampleBufferAttachments);
  for (std::size_t i = 0; i < attachmentCount; ++i) {
    passInfo.sample_buffer_attachments[i] = sampleBufferAttachments[i];
  }
  passInfo.num_sample_buffer_attachments = static_cast<std::uint8_t>(attachmentCount);
  passInfo.visibility_buffer = visibilityBuffer.handle;

  // Load actions are immutable at encoder open and can be accounted here.
  // Store actions and their tile-byte estimates are accounted exactly once
  // by LifecycleRuntime immediately before endEncoding, after every Unknown
  // action has been resolved.
  for (std::size_t i = 0; i < core::kMaxRenderTargets; ++i) {
    auto* surface = ctx.pool.findSurface(hot.colorAttachments[i].handle.value);
    if (!surface || !surface->texture)
      continue;
    const auto& att = passInfo.colors[i];
    perf::countRenderPassLoadActionColor(static_cast<std::uint32_t>(att.load_action));
    const std::uint64_t pixelBytes =
        static_cast<std::uint64_t>(surface->desc.width) *
        static_cast<std::uint64_t>(surface->desc.height) *
        static_cast<std::uint64_t>(core::bytesPerPixel(surface->desc.format));
    if (att.load_action == WMTLoadActionLoad) {
      perf::countRenderPassTilePreservationBytes(pixelBytes);
      if (actionSummary) {
        actionSummary->colorLoadBytes += pixelBytes;
      }
    }
  }
  if (auto* depthSurface = ctx.pool.findSurface(hot.depthStencil.handle.value);
      depthSurface && depthSurface->texture && depthSurface->desc.depthStencil) {
    const std::uint64_t depthPixelBytes =
        static_cast<std::uint64_t>(depthSurface->desc.width) *
        static_cast<std::uint64_t>(depthSurface->desc.height) *
        static_cast<std::uint64_t>(core::bytesPerPixel(depthSurface->desc.format));
    if (formatHasDepthAspect(depthSurface->desc.format)) {
      perf::countRenderPassLoadActionDepth(static_cast<std::uint32_t>(passInfo.depth.load_action));
      if (passInfo.depth.load_action == WMTLoadActionLoad) {
        perf::countRenderPassTilePreservationBytes(depthPixelBytes);
        if (actionSummary) {
          actionSummary->depthLoadBytes += depthPixelBytes;
        }
      }
    }
    if (formatHasStencilAspect(depthSurface->desc.format)) {
      perf::countRenderPassLoadActionStencil(
          static_cast<std::uint32_t>(passInfo.stencil.load_action));
      if (passInfo.stencil.load_action == WMTLoadActionLoad) {
        perf::countRenderPassTilePreservationBytes(depthPixelBytes);
        if (actionSummary) {
          actionSummary->stencilLoadBytes += depthPixelBytes;
        }
      }
    }
  }

  lateStoreState->summary = *actionSummary;
  prepared.primaryWidth = primarySurface->desc.width;
  prepared.primaryHeight = primarySurface->desc.height;
  prepared.discardAfterPresent = discardAfterPresent;
  prepared.valid = true;
  return true;
}

void configurePreparedRenderPassEncoder(
    EncodeContext& ctx, WMT::RenderCommandEncoder& encoder,
    core::FlatDrawStateView drawState, const PreparedRenderPass& prepared) {
  const auto& hot = *drawState.hot;
  // R-BACK-14.3 — issue `useHeap` once per heap instance that actually
  // backs a resource bound on this encoder. Walking the active draw
  // state's stream/index buffers + sampler textures and consulting each
  // record's `isHeapBacked` flag avoids the over-issue case where every
  // pool heap (including heaps holding resources unrelated to this
  // encoder) was made resident at encoder open. The dedup buffer is
  // sized to the static binding cap (kMaxStreams streams + 1 index
  // buffer + kMaxSamplers texture stages = 33 bindings, all of which
  // share the same handful of heap instances per family) so this stays
  // a fixed-size, allocation-free walk on the encoder-open hot path.
  {
    constexpr std::size_t kMaxBoundHeaps = core::kMaxStreams + 1u + core::kMaxSamplers;
    std::array<obj_handle_t, kMaxBoundHeaps> usedHeaps{};
    std::size_t usedHeapCount = 0;
    auto pushHeap = [&](WMT::Heap heap) {
      const obj_handle_t h = heap.handle;
      if (h == 0)
        return;
      for (std::size_t i = 0; i < usedHeapCount; ++i) {
        if (usedHeaps[i] == h)
          return;
      }
      if (usedHeapCount < usedHeaps.size()) {
        usedHeaps[usedHeapCount++] = h;
      }
    };
    // `isHeapBacked`/`heap` on BufferRecord/TextureRecord are set exactly
    // once, before the record is inserted into the arena (Pool::createBuffer
    // / Pool::createTexture) and are never reassigned afterward — no code
    // path outside record construction writes either field (verified by
    // grepping every assignment site). Reading them through a plain `find`
    // view is therefore `immutable-after-init`, not a live-view race.
    auto considerBuffer = [&](core::Handle handle) {
      if (!handle)
        return;
      if (auto* rec = ctx.pool.findBuffer(handle.value); rec && rec->isHeapBacked) {
        pushHeap(rec->heap);
      }
    };
    auto considerTexture = [&](core::Handle handle) {
      if (!handle)
        return;
      if (auto* rec = ctx.pool.findTexture(handle.value); rec && rec->isHeapBacked) {
        pushHeap(rec->heap);
      }
    };
    considerBuffer(hot.indexBuffer);
    for (const auto& streamHandle : hot.streamBuffers) {
      considerBuffer(streamHandle);
    }
    for (const auto& textureHandle : hot.textures) {
      considerTexture(textureHandle);
    }
    for (std::size_t i = 0; i < usedHeapCount; ++i) {
      encoder.useHeap(WMT::Heap{usedHeaps[i]});
      perf::countUseHeap();
    }
  }
  const auto ffLayout = drawState.hasShaderContext()
                            ? decodeFixedFunctionVertexLayout(drawState.shaderContext().vertexDecl)
                            : std::optional<dxmt9::ffp::FixedFunctionVertexLayout>{};
  double viewportWidth = static_cast<double>(std::max(1u, hot.viewport.viewport.width));
  double viewportHeight = static_cast<double>(std::max(1u, hot.viewport.viewport.height));
  double viewportOriginX = static_cast<double>(hot.viewport.viewport.x);
  double viewportOriginY = static_cast<double>(hot.viewport.viewport.y);
  if (ffLayout && ffLayout->preTransformed) {
    viewportOriginX = 0.0;
    viewportOriginY = 0.0;
    viewportWidth = static_cast<double>(std::max(1u, prepared.primaryWidth));
    viewportHeight = static_cast<double>(std::max(1u, prepared.primaryHeight));
  }
  WMTViewport vp{viewportOriginX,
                 viewportOriginY,
                 viewportWidth,
                 viewportHeight,
                 static_cast<double>(hot.viewport.viewport.minZ),
                 static_cast<double>(hot.viewport.viewport.maxZ)};
  encoder.setViewport(vp);
  countViewportBind();
  // Prologue value is the initial baseline; per-draw rebinds will override it.
  // Convert D3D9 normalized constant bias to the active Metal depth format.
  const float prologueDepthBias = metalDepthBiasForDrawState(ctx, hot);
  const float prologueSlopeScale = std::bit_cast<float>(
      core::flatStateOr(hot.renderStates, core::RS_SLOPE_SCALE_DEPTH_BIAS, 0u));
  encoder.setRasterizerState(WMTTriangleFillModeFill, WMTCullModeNone, WMTDepthClipModeClip,
                             frontFaceWinding(), prologueDepthBias, prologueSlopeScale, 0.0f);
  countRasterizerBind();
}

void commitPreparedRenderPassOpen(EncodeContext& ctx,
                                  const PreparedRenderPass& prepared) {
  DXMT_ASSERT(prepared.valid);
  perf::countRenderPassBegin();
  if (prepared.discardAfterPresent) {
    ctx.queue.backBufferDiscardAfterPresent_ = false;
  }
}

WMT::Reference<WMT::RenderCommandEncoder> beginRenderPassWithStoreProofLookahead(
    EncodeContext& ctx, WMT::CommandBuffer& commandBuffer, core::FlatDrawStateView drawState,
    const std::optional<ClearDesc>& clear,
    std::span<const RenderPassStoreProofLookaheadSource> lookaheadSources,
    RenderPassStoreProofActivePass activePass,
    std::span<const WMTSampleBufferAttachmentInfo> sampleBufferAttachments,
    WMT::Buffer visibilityBuffer, RenderPassActionSummary* actionSummary,
    LateRenderPassStoreState* lateStoreState) {
  PreparedRenderPass prepared{};
  if (!prepareRenderPassWithStoreProofLookahead(
          ctx, drawState, clear, lookaheadSources, activePass,
          sampleBufferAttachments, visibilityBuffer, prepared)) {
    if (actionSummary) {
      *actionSummary = {};
    }
    if (lateStoreState) {
      *lateStoreState = {};
    }
    return {};
  }
  auto encoder = commandBuffer.renderCommandEncoder(prepared.info);
  if (!encoder) {
    if (actionSummary) {
      *actionSummary = {};
    }
    if (lateStoreState) {
      *lateStoreState = {};
    }
    return {};
  }
  commitPreparedRenderPassOpen(ctx, prepared);
  configurePreparedRenderPassEncoder(ctx, encoder, drawState, prepared);
  if (actionSummary) {
    *actionSummary = prepared.actions;
  }
  if (lateStoreState) {
    *lateStoreState = prepared.lateStore;
  }
  return WMT::Reference<WMT::RenderCommandEncoder>(encoder);
}

WMT::Reference<WMT::RenderCommandEncoder>
beginRenderPass(EncodeContext& ctx, WMT::CommandBuffer& commandBuffer,
                core::FlatDrawStateView drawState, const std::optional<ClearDesc>& clear,
                const core::ChunkSlot* lookaheadSlot, std::size_t lookaheadStartIndex,
                std::span<const WMTSampleBufferAttachmentInfo> sampleBufferAttachments,
                WMT::Buffer visibilityBuffer, RenderPassActionSummary* actionSummary,
                LateRenderPassStoreState* lateStoreState) {
  RenderPassStoreProofLookaheadSource lookaheadSource{};
  std::span<const RenderPassStoreProofLookaheadSource> lookaheadSources{};
  if (lookaheadSlot) {
    const std::size_t firstCommandIndex = lookaheadStartIndex < lookaheadSlot->commandCount()
                                              ? lookaheadStartIndex + 1u
                                              : lookaheadSlot->commandCount();
    lookaheadSource = RenderPassStoreProofLookaheadSource{
        .slot = lookaheadSlot,
        .firstCommandIndex = firstCommandIndex,
        .commandEndIndex = lookaheadSlot->commandCount(),
    };
    lookaheadSources = std::span<const RenderPassStoreProofLookaheadSource>(&lookaheadSource, 1u);
  }
  return beginRenderPassWithStoreProofLookahead(
      ctx, commandBuffer, drawState, clear, lookaheadSources, RenderPassStoreProofActivePass{},
      sampleBufferAttachments, visibilityBuffer, actionSummary, lateStoreState);
}

}  // namespace dxmt9::encoders
