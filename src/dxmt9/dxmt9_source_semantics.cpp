#include "dxmt9_source_semantics.hpp"

#include <algorithm>
#include <limits>

namespace dxmt9::core {

namespace {

SourceSemanticBoundaryKind boundaryForCommand(
    MetalCommandKind kind) noexcept {
  switch (kind) {
  case MetalCommandKind::DrawRun:
    return SourceSemanticBoundaryKind::None;
  case MetalCommandKind::Clear:
    return SourceSemanticBoundaryKind::Clear;
  case MetalCommandKind::Readback:
    return SourceSemanticBoundaryKind::Readback;
  case MetalCommandKind::Present:
    return SourceSemanticBoundaryKind::Present;
  case MetalCommandKind::SurfaceCopy:
  case MetalCommandKind::StretchRect:
  case MetalCommandKind::ColorFill:
  case MetalCommandKind::DepthResolve:
  case MetalCommandKind::GenerateMipmaps:
    return SourceSemanticBoundaryKind::Blit;
  }
  return SourceSemanticBoundaryKind::Invalid;
}

SourceEntryEncoderKind entryKindForCommand(MetalCommandKind kind) noexcept {
  switch (kind) {
  case MetalCommandKind::DrawRun:
  case MetalCommandKind::Clear:
    return SourceEntryEncoderKind::Render;
  case MetalCommandKind::SurfaceCopy:
  case MetalCommandKind::StretchRect:
  case MetalCommandKind::Readback:
  case MetalCommandKind::ColorFill:
  case MetalCommandKind::DepthResolve:
  case MetalCommandKind::GenerateMipmaps:
    return SourceEntryEncoderKind::Blit;
  case MetalCommandKind::Present:
    return SourceEntryEncoderKind::Present;
  }
  return SourceEntryEncoderKind::Invalid;
}

void noteBoundary(SourceSemanticSummary& summary,
                  std::size_t ordinal,
                  SourceSemanticBoundaryKind kind) noexcept {
  if (kind == SourceSemanticBoundaryKind::None ||
      summary.firstBoundary != SourceSemanticBoundaryKind::None) {
    return;
  }
  if (ordinal > std::numeric_limits<std::uint32_t>::max()) {
    summary.flags |= SourceSemanticInvalid;
    summary.firstBoundary = SourceSemanticBoundaryKind::Invalid;
    summary.firstBoundaryOrdinal = 0;
    return;
  }
  summary.firstBoundary = kind;
  summary.firstBoundaryOrdinal = static_cast<std::uint32_t>(ordinal);
}

void addLogicalExtent(std::size_t& total,
                      std::size_t count,
                      std::size_t elementSize) noexcept {
  const std::size_t maximum = std::numeric_limits<std::size_t>::max();
  if (elementSize != 0 && count > (maximum - total) / elementSize) {
    total = maximum;
    return;
  }
  total += count * elementSize;
}

}  // namespace

std::size_t measureSourcePayloadLogicalExtent(
    SourcePayloadView payload) noexcept {
  if (!payload.valid()) {
    return 0;
  }

  // This is the flattened logical footprint visible through the common
  // payload adapter, not vector capacity or Tape page residency. Per-command
  // state and variable spans are counted as replay sees them; shared uniform
  // tables are counted once per source segment.
  std::size_t bytes = 0;
  addLogicalExtent(bytes, payload.commandCount(),
                   sizeof(MetalCommandHeader));
  std::array<bool, kMaxArenaSourcePayloadSegments> drawTablesCounted{};
  for (std::size_t i = 0; i < payload.commandCount(); ++i) {
    const SourceCommandView source = payload.commandAt(i);
    const auto& command = source.command;
    switch (source.kind()) {
    case MetalCommandKind::DrawRun:
      if (command.drawRunRecord) {
        addLogicalExtent(bytes, 1, sizeof(DrawRunCommandRecord));
      }
      if (command.drawPsoSubview) {
        addLogicalExtent(bytes, 1, sizeof(DrawPsoSubview));
      }
      if (command.drawState.hot) {
        addLogicalExtent(bytes, 1, sizeof(FlatDrawStateRecord));
      }
      if (command.drawState.shaderLayout) {
        addLogicalExtent(bytes, 1, sizeof(DrawShaderLayoutContext));
      }
      if (command.drawState.debug) {
        addLogicalExtent(bytes, 1, sizeof(DrawDebugSnapshot));
      }
      addLogicalExtent(bytes, command.drawParams.size(), sizeof(DrawParam));
      addLogicalExtent(bytes, command.drawPayloadBytes.size(), sizeof(u8));
      if (source.segmentIndex < drawTablesCounted.size() &&
          !drawTablesCounted[source.segmentIndex]) {
        drawTablesCounted[source.segmentIndex] = true;
        addLogicalExtent(bytes,
                         command.drawUniformFixedPayloadRecords.size(),
                         sizeof(DrawUniformFixedPayloadRecord));
        addLogicalExtent(bytes,
                         command.drawUniformVertexConstantsRecords.size(),
                         sizeof(DrawUniformVertexConstantsRecord));
        addLogicalExtent(bytes,
                         command.drawUniformVertexConstantBytes.size(),
                         sizeof(u8));
        addLogicalExtent(bytes,
                         command.drawUniformPixelConstantsRecords.size(),
                         sizeof(DrawUniformPixelConstantsRecord));
        addLogicalExtent(bytes,
                         command.drawUniformPixelConstantBytes.size(),
                         sizeof(u8));
        addLogicalExtent(bytes, command.drawUniformPayloadRecords.size(),
                         sizeof(DrawUniformPayloadRecord));
      }
      break;
    case MetalCommandKind::Clear:
      if (source.clear) {
        addLogicalExtent(bytes, 1, sizeof(ArenaClearRecord));
        addLogicalExtent(bytes, source.clear->rects.size(), sizeof(Rect));
      }
      break;
    case MetalCommandKind::SurfaceCopy:
      if (command.surfaceCopy) {
        addLogicalExtent(bytes, 1, sizeof(SurfaceCopyDesc));
      }
      break;
    case MetalCommandKind::StretchRect:
      if (command.stretchRect) {
        addLogicalExtent(bytes, 1, sizeof(StretchRectDesc));
      }
      break;
    case MetalCommandKind::Readback:
      if (command.readback) {
        addLogicalExtent(bytes, 1, sizeof(ReadbackDesc));
      }
      break;
    case MetalCommandKind::ColorFill:
      if (command.colorFill) {
        addLogicalExtent(bytes, 1, sizeof(ColorFillDesc));
      }
      break;
    case MetalCommandKind::DepthResolve:
      if (command.depthResolve) {
        addLogicalExtent(bytes, 1, sizeof(DepthResolveDesc));
      }
      break;
    case MetalCommandKind::GenerateMipmaps:
      if (command.generateMipmaps) {
        addLogicalExtent(bytes, 1, sizeof(GenerateMipmapsDesc));
      }
      break;
    case MetalCommandKind::Present:
      if (command.present) {
        addLogicalExtent(bytes, 1, sizeof(PresentCommandRecord));
      }
      break;
    }
  }
  return bytes;
}

RenderAttachmentKey makeRenderAttachmentKey(
    const FlatDrawStateRecord& hot) noexcept {
  RenderAttachmentKey key{};
  key.color = hot.colorAttachments;
  key.depthStencil = hot.depthStencil;
  for (const auto& attachment : hot.colorAttachments) {
    key.sampleCount = std::max(key.sampleCount, attachment.sampleCount);
  }
  key.sampleCount = std::max(key.sampleCount,
                             hot.depthStencil.sampleCount);
  return key;
}

ExactResourceSet makeRenderAttachmentWriteSet(
    const FlatDrawStateRecord& hot,
    bool canonicalized) noexcept {
  ExactResourceSet resources{};
  if (canonicalized) {
    resources.flags |= ExactResourceSetCanonicalized;
  }
  for (const auto& attachment : hot.colorAttachments) {
    resources.add(attachment.handle.value);
  }
  resources.add(hot.depthStencil.handle.value);
  return resources;
}

ExactResourceSet makeDrawEntryReadSet(
    FlatDrawStateView state,
    bool canonicalized) noexcept {
  ExactResourceSet resources{};
  if (canonicalized) {
    resources.flags |= ExactResourceSetCanonicalized;
  }
  if (!state.hot) {
    resources.flags &= ~ExactResourceSetComplete;
    return resources;
  }
  const auto& hot = *state.hot;
  resources.add(hot.indexBuffer.value);
  for (const auto& buffer : hot.streamBuffers) {
    resources.add(buffer.value);
  }
  for (const auto& texture : hot.textures) {
    resources.add(texture.value);
  }
  return resources;
}

SourceSemanticSummary summarizeSourcePayload(
    SourcePayloadView payload,
    SourceSemanticSummaryContext context) noexcept {
  SourceSemanticSummary summary{};
  summary.byteCount = context.byteCount;
  summary.pageCount = context.pageCount;
  if (context.sealed) {
    summary.flags |= SourceSemanticSealed;
  }
  if (context.entryStable) {
    summary.flags |= SourceSemanticEntryStable;
  }
  if (context.captureIsolation) {
    summary.flags |= SourceSemanticCaptureIsolation;
  }
  if (context.globalObservation) {
    summary.flags |= SourceSemanticGlobalObservation;
  }
  if (context.initializerRequirement) {
    summary.flags |= SourceSemanticInitializerRequirement;
  }
  if (!payload.valid() ||
      payload.commandCount() > std::numeric_limits<std::uint32_t>::max()) {
    summary.entryKind = SourceEntryEncoderKind::Invalid;
    summary.firstBoundary = SourceSemanticBoundaryKind::Invalid;
    summary.firstBoundaryOrdinal = 0;
    summary.flags |= SourceSemanticInvalid;
    return summary;
  }

  summary.commandCount = static_cast<std::uint32_t>(payload.commandCount());
  if (payload.commandsEmpty()) {
    return summary;
  }

  const SourceCommandView first = payload.commandAt(0);
  summary.entryKind = entryKindForCommand(first.kind());
  if (summary.entryKind == SourceEntryEncoderKind::Invalid) {
    summary.flags |= SourceSemanticInvalid;
    noteBoundary(summary, 0, SourceSemanticBoundaryKind::Invalid);
    return summary;
  }

  RenderAttachmentKey firstAttachment{};
  ExactResourceSet firstWrites{};
  bool firstDrawValid = false;
  if (first.kind() == MetalCommandKind::DrawRun &&
      first.command.drawState.hot) {
    firstDrawValid = true;
    firstAttachment = makeRenderAttachmentKey(*first.command.drawState.hot);
    firstWrites = makeRenderAttachmentWriteSet(
        *first.command.drawState.hot, context.resourcesCanonicalized);
    summary.entryRender = RenderContinuationKey{
        .attachments = firstAttachment,
        .entryReads = makeDrawEntryReadSet(
            first.command.drawState, context.resourcesCanonicalized),
        .route = context.firstRenderRoute,
        .passActionEpoch = context.passActionEpoch,
        .flags = RenderContinuationKeyValid |
                 RenderContinuationEntryStateComplete,
    };
    if (!summary.entryRender.entryReads.complete()) {
      summary.entryRender.flags &= ~RenderContinuationEntryStateComplete;
    }
  } else if (first.kind() == MetalCommandKind::DrawRun) {
    summary.flags |= SourceSemanticInvalid;
    noteBoundary(summary, 0, SourceSemanticBoundaryKind::Invalid);
  } else {
    noteBoundary(summary, 0, boundaryForCommand(first.kind()));
    if (first.kind() == MetalCommandKind::Readback) {
      summary.flags |= SourceSemanticGlobalObservation;
    }
  }

  std::uint32_t presentCount = 0;
  for (std::size_t i = 0; i < payload.commandCount(); ++i) {
    const SourceCommandView source = payload.commandAt(i);
    const auto& command = source.command;
    if (command.kind == MetalCommandKind::Present) {
      summary.flags |= SourceSemanticHasPresent;
      ++presentCount;
    }
    if (command.kind == MetalCommandKind::Readback) {
      summary.flags |= SourceSemanticGlobalObservation;
    }
    if (command.kind == MetalCommandKind::DrawRun) {
      const std::size_t drawCount = command.drawParams.size();
      if (drawCount > std::numeric_limits<std::uint32_t>::max() -
                          summary.drawCount) {
        summary.flags |= SourceSemanticInvalid;
      } else {
        summary.drawCount += static_cast<std::uint32_t>(drawCount);
      }
    }

    if (i == 0 || summary.firstBoundary !=
                      SourceSemanticBoundaryKind::None) {
      continue;
    }
    if (command.kind != MetalCommandKind::DrawRun) {
      noteBoundary(summary, i, boundaryForCommand(command.kind));
      continue;
    }
    if (!firstDrawValid || !command.drawState.hot) {
      noteBoundary(summary, i, SourceSemanticBoundaryKind::Invalid);
      summary.flags |= SourceSemanticInvalid;
      continue;
    }
    if (makeRenderAttachmentKey(*command.drawState.hot) != firstAttachment) {
      noteBoundary(summary, i,
                   SourceSemanticBoundaryKind::AttachmentChange);
      continue;
    }
    const ExactResourceSet reads = makeDrawEntryReadSet(
        command.drawState, context.resourcesCanonicalized);
    if (!reads.complete()) {
      noteBoundary(summary, i, SourceSemanticBoundaryKind::Invalid);
      summary.flags |= SourceSemanticInvalid;
    } else if (firstWrites.overlaps(reads)) {
      noteBoundary(summary, i, SourceSemanticBoundaryKind::ResourceHazard);
    }
  }

  if (presentCount == 1u &&
      payload.commandAt(payload.commandCount() - 1u).kind() ==
          MetalCommandKind::Present) {
    summary.flags |= SourceSemanticFinalPresentTail;
  }

  return summary;
}

}  // namespace dxmt9::core
