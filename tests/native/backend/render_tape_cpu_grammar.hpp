#pragma once

// Bounded, no-GPU grammar admission for the CPU Render Tape sink.
//
// The caller must pass a view produced by importPrevalidatedRenderTape() after
// the normal Render Tape validator has accepted the immutable input.  This
// helper deliberately does not revalidate or reparse that input: it only
// projects the already-imported event/chunk views onto the smaller CPU
// grammar, before the sink is allowed to create or mutate anything.

#include "device_c_render_tape.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>

namespace dxmt9::d3d9 {

inline constexpr std::size_t kRenderTapeCpuGrammarMaxEvents = 32u;
inline constexpr std::size_t kRenderTapeCpuGrammarMaxSubmissions = 32u;
inline constexpr std::size_t kRenderTapeCpuGrammarMaxCommandRecords = 128u;
// This is deliberately a fixture-sized CPU sink admission bound.  It is not
// the Render Tape/provider limit and must not be read as support for arbitrary
// captured resource sizes.
inline constexpr std::size_t kRenderTapeCpuGrammarMaxBufferBytes =
    16u * 1024u * 1024u;

enum class RenderTapeCpuGrammarReason : std::uint8_t {
  Accepted,
  EventLimit,
  SubmissionLimit,
  CommandRecordLimit,
  DuplicateBootstrap,
  BootstrapGammaUnsupported,
  BootstrapOverlayUnsupported,
  BootstrapHandlesUnsupported,
  BootstrapRecordUnsupported,
  BufferDescriptorRequired,
  BufferDescriptorMalformed,
  DynamicBackendRequired,
  GroupedBufferUnsupported,
  ManagedAllocationFaultRequiresManagedBuffer,
  MultipleBufferDescriptors,
  BufferDestroyUnsupported,
  BufferOrderUnsupported,
  PresentSurfaceDescriptorRequired,
  PresentSurfaceDescriptorMalformed,
  BufferMutationUnsupported,
  BufferMutationRangeUnsupported,
  CommandChunkUnsupported,
  CommandRecordUnsupported,
  DrawShapeUnsupported,
  DrawOrderUnsupported,
  CommandHandlesUnsupported,
  BufferReferenceUnsupported,
  PresentMetadataUnsupported,
  IncompleteGrammar,
  EventAfterPresentComplete,
  UnsupportedEvent,
};

struct RenderTapeCpuGrammarResult {
  RenderTapeCpuGrammarReason reason = RenderTapeCpuGrammarReason::Accepted;
  std::uint32_t eventIndex = 0xffffffffu;
  std::uint32_t submissionIndex = 0xffffffffu;
  std::uint32_t recordIndex = 0xffffffffu;

  constexpr bool accepted() const noexcept {
    return reason == RenderTapeCpuGrammarReason::Accepted;
  }
};

inline const char* renderTapeCpuGrammarReasonName(
    RenderTapeCpuGrammarReason reason) noexcept {
  switch (reason) {
  case RenderTapeCpuGrammarReason::Accepted:
    return "accepted";
  case RenderTapeCpuGrammarReason::EventLimit:
    return "event-limit";
  case RenderTapeCpuGrammarReason::SubmissionLimit:
    return "submission-limit";
  case RenderTapeCpuGrammarReason::CommandRecordLimit:
    return "command-record-limit";
  case RenderTapeCpuGrammarReason::DuplicateBootstrap:
    return "duplicate-bootstrap";
  case RenderTapeCpuGrammarReason::BootstrapGammaUnsupported:
    return "bootstrap-gamma-unsupported";
  case RenderTapeCpuGrammarReason::BootstrapOverlayUnsupported:
    return "bootstrap-overlay-unsupported";
  case RenderTapeCpuGrammarReason::BootstrapHandlesUnsupported:
    return "bootstrap-handles-unsupported";
  case RenderTapeCpuGrammarReason::BootstrapRecordUnsupported:
    return "bootstrap-record-unsupported";
  case RenderTapeCpuGrammarReason::BufferDescriptorRequired:
    return "buffer-descriptor-required";
  case RenderTapeCpuGrammarReason::BufferDescriptorMalformed:
    return "buffer-descriptor-malformed";
  case RenderTapeCpuGrammarReason::DynamicBackendRequired:
    return "dynamic-backend-required";
  case RenderTapeCpuGrammarReason::GroupedBufferUnsupported:
    return "grouped-buffer-unsupported";
  case RenderTapeCpuGrammarReason::ManagedAllocationFaultRequiresManagedBuffer:
    return "managed-allocation-fault-requires-managed-buffer";
  case RenderTapeCpuGrammarReason::MultipleBufferDescriptors:
    return "multiple-buffer-descriptors";
  case RenderTapeCpuGrammarReason::BufferDestroyUnsupported:
    return "buffer-destroy-unsupported";
  case RenderTapeCpuGrammarReason::BufferOrderUnsupported:
    return "buffer-order-unsupported";
  case RenderTapeCpuGrammarReason::PresentSurfaceDescriptorRequired:
    return "present-surface-descriptor-required";
  case RenderTapeCpuGrammarReason::PresentSurfaceDescriptorMalformed:
    return "present-surface-descriptor-malformed";
  case RenderTapeCpuGrammarReason::BufferMutationUnsupported:
    return "buffer-mutation-unsupported";
  case RenderTapeCpuGrammarReason::BufferMutationRangeUnsupported:
    return "buffer-mutation-range-unsupported";
  case RenderTapeCpuGrammarReason::CommandChunkUnsupported:
    return "command-chunk-unsupported";
  case RenderTapeCpuGrammarReason::CommandRecordUnsupported:
    return "command-record-unsupported";
  case RenderTapeCpuGrammarReason::DrawShapeUnsupported:
    return "draw-shape-unsupported";
  case RenderTapeCpuGrammarReason::DrawOrderUnsupported:
    return "draw-order-unsupported";
  case RenderTapeCpuGrammarReason::CommandHandlesUnsupported:
    return "command-handles-unsupported";
  case RenderTapeCpuGrammarReason::BufferReferenceUnsupported:
    return "buffer-reference-unsupported";
  case RenderTapeCpuGrammarReason::PresentMetadataUnsupported:
    return "present-metadata-unsupported";
  case RenderTapeCpuGrammarReason::IncompleteGrammar:
    return "incomplete-grammar";
  case RenderTapeCpuGrammarReason::EventAfterPresentComplete:
    return "event-after-present-complete";
  case RenderTapeCpuGrammarReason::UnsupportedEvent:
    return "unsupported-event";
  }
  return "unknown";
}

namespace detail {

template <typename T>
inline bool renderTapeCpuGrammarLoad(std::span<const std::byte> bytes,
                                     std::size_t offset, T& value) noexcept {
  if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) return false;
  std::memcpy(&value, bytes.data() + offset, sizeof(T));
  return true;
}

inline bool renderTapeCpuGrammarSameIdentity(
    const D9CWireObjectIdentity& left,
    const D9CWireObjectIdentity& right) noexcept {
  return left.kind == right.kind && left.generation == right.generation &&
         left.objectId == right.objectId;
}

inline D9CWireObjectIdentity renderTapeCpuGrammarIdentity(
    const D9CCommandChunkWireHandleEntry& handle) noexcept {
  return D9CWireObjectIdentity{
      .kind = handle.kind,
      .generation = handle.generation,
      .objectId = handle.objectId};
}

inline bool renderTapeCpuGrammarBasicDraw(
    const ImportedRecordView& record,
    const D9CWireObjectIdentity& bufferIdentity,
    std::uint64_t bufferBytes) noexcept {
  // The CPU fixture has a proof/oracle for the canonical Texture+Stream state
  // sections, but not for arbitrary draw binding overrides.  The texture
  // snapshot must be handle-free and stream slot zero is the sole buffer;
  // this admits the real fixture without admitting indexed/UP or other
  // resource-binding payloads.
  const auto& draw = record.drawHeader;
  if (record.header.type != D9C_COMMAND_RECORD_DRAW_PRIMITIVE ||
      record.handles.size() != 1u ||
      !renderTapeCpuGrammarSameIdentity(
          renderTapeCpuGrammarIdentity(record.handles.front()), bufferIdentity) ||
      draw.flags != 0u || draw.primitiveType < 1u ||
      draw.primitiveType > 5u || draw.primitiveCount == 0u ||
      draw.baseVertex != 0 || draw.minVertex != 0u ||
      draw.numVertices != 0u || draw.startIndex != 0u || draw.stride != 0u ||
      draw.indexFormat != 0u || draw.sectionCount != 2u ||
      draw.sectionTableOffset != sizeof(draw) || draw.reserved0 != 0u ||
      record.sections.size() != 2u ||
      record.sections[0].kind != D9C_COMMAND_CHUNK_SECTION_TEXTURE ||
      record.sections[1].kind != D9C_COMMAND_CHUNK_SECTION_STREAM) {
    return false;
  }

  const auto texture = record.section(0u);
  const auto stream = record.section(1u);
  if (texture.descriptor.elementSize != sizeof(D9CCommandChunkWireTextureBinding) ||
      texture.descriptor.count != D9C_DRAW_PACKET_MAX_TEXTURES ||
      texture.descriptor.byteSize !=
          texture.descriptor.count * texture.descriptor.elementSize ||
      stream.descriptor.elementSize != sizeof(D9CCommandChunkWireStreamBinding) ||
      stream.descriptor.count != D9C_DRAW_PACKET_MAX_STREAMS ||
      stream.descriptor.byteSize !=
          stream.descriptor.count * stream.descriptor.elementSize ||
      texture.payload.size() != texture.descriptor.byteSize ||
      stream.payload.size() != stream.descriptor.byteSize) {
    return false;
  }
  for (std::uint32_t index = 0u; index < texture.descriptor.count; ++index) {
    D9CCommandChunkWireTextureBinding value{};
    if (!renderTapeCpuGrammarLoad(
            texture.payload,
            static_cast<std::size_t>(index) * sizeof(value), value) ||
        value.slot != index || value.valid > 1u || value.handleIndex !=
            D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX || value.reserved0 != 0u) {
      return false;
    }
  }
  for (std::uint32_t index = 0u; index < stream.descriptor.count; ++index) {
    D9CCommandChunkWireStreamBinding value{};
    if (!renderTapeCpuGrammarLoad(
            stream.payload,
            static_cast<std::size_t>(index) * sizeof(value), value) ||
        value.slot != index || value.valid != 1u || value.offset != 0u ||
        value.reserved0 != 0u || value.frequency > 1u) {
      return false;
    }
    if (index == 0u) {
      if (value.handleIndex != record.header.firstHandle || value.stride == 0u)
        return false;
    } else if (value.handleIndex != D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX ||
               value.stride != 0u) {
      return false;
    }
  }
  const D9CCommandChunkWireStreamBinding firstStream = [&] {
    D9CCommandChunkWireStreamBinding value{};
    (void)renderTapeCpuGrammarLoad(stream.payload, 0u, value);
    return value;
  }();
  std::uint64_t vertexCount = 0u;
  switch (draw.primitiveType) {
  case 1u:
    vertexCount = draw.primitiveCount;
    break;
  case 2u:
    vertexCount = static_cast<std::uint64_t>(draw.primitiveCount) * 2u;
    break;
  case 3u:
    vertexCount = static_cast<std::uint64_t>(draw.primitiveCount) + 1u;
    break;
  case 4u:
    vertexCount = static_cast<std::uint64_t>(draw.primitiveCount) * 3u;
    break;
  case 5u:
    vertexCount = static_cast<std::uint64_t>(draw.primitiveCount) + 2u;
    break;
  default:
    return false;
  }
  const auto firstVertex = static_cast<std::uint64_t>(draw.startVertex);
  if (firstVertex > std::numeric_limits<std::uint64_t>::max() - vertexCount) {
    return false;
  }
  const auto vertexEnd = firstVertex + vertexCount;
  if (firstStream.stride != 0u &&
      vertexEnd > std::numeric_limits<std::uint64_t>::max() /
                       firstStream.stride) {
    return false;
  }
  const auto vertexBytes = vertexEnd * firstStream.stride;
  if (firstStream.offset >
          std::numeric_limits<std::uint64_t>::max() - vertexBytes ||
      static_cast<std::uint64_t>(firstStream.offset) + vertexBytes >
          bufferBytes) {
    return false;
  }
  return true;
}

inline bool renderTapeCpuGrammarPresentRecord(
    const ImportedRecordView& record) noexcept {
  return record.header.type == D9C_COMMAND_RECORD_PRESENT &&
         record.handles.empty() && record.payload.size() ==
             sizeof(D9CCommandChunkWirePresent);
}

inline bool renderTapeCpuGrammarCanonicalUnbind(
    const ImportedRecordView& record) noexcept {
  const auto& draw = record.drawHeader;
  if (record.header.type != D9C_COMMAND_RECORD_APPLY_STATE ||
      !record.handles.empty() ||
      draw.flags != D9C_COMMAND_CHUNK_DRAW_FLAG_FULL_SNAPSHOT ||
      draw.primitiveType != 0u || draw.baseVertex != 0 ||
      draw.minVertex != 0u || draw.numVertices != 0u ||
      draw.startVertex != 0u || draw.startIndex != 0u ||
      draw.primitiveCount != 0u || draw.stride != 0u ||
      draw.indexFormat != 0u || draw.sectionCount != 2u ||
      draw.sectionTableOffset != sizeof(draw) || draw.reserved0 != 0u ||
      record.sections.size() != 2u ||
      record.sections[0].kind != D9C_COMMAND_CHUNK_SECTION_TEXTURE ||
      record.sections[1].kind != D9C_COMMAND_CHUNK_SECTION_STREAM) {
    return false;
  }
  const auto texture = record.section(0u);
  const auto stream = record.section(1u);
  if (texture.descriptor.elementSize != sizeof(D9CCommandChunkWireTextureBinding) ||
      texture.descriptor.count != D9C_DRAW_PACKET_MAX_TEXTURES ||
      texture.descriptor.byteSize !=
          texture.descriptor.count * texture.descriptor.elementSize ||
      stream.descriptor.elementSize != sizeof(D9CCommandChunkWireStreamBinding) ||
      stream.descriptor.count != D9C_DRAW_PACKET_MAX_STREAMS ||
      stream.descriptor.byteSize !=
          stream.descriptor.count * stream.descriptor.elementSize ||
      texture.payload.size() != texture.descriptor.byteSize ||
      stream.payload.size() != stream.descriptor.byteSize) {
    return false;
  }
  for (std::uint32_t index = 0u; index < texture.descriptor.count; ++index) {
    D9CCommandChunkWireTextureBinding value{};
    if (!renderTapeCpuGrammarLoad(
            texture.payload,
            static_cast<std::size_t>(index) * sizeof(value), value) ||
        value.slot != index || value.valid != 1u || value.handleIndex !=
            D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX || value.reserved0 != 0u) {
      return false;
    }
  }
  for (std::uint32_t index = 0u; index < stream.descriptor.count; ++index) {
    D9CCommandChunkWireStreamBinding value{};
    if (!renderTapeCpuGrammarLoad(
            stream.payload,
            static_cast<std::size_t>(index) * sizeof(value), value) ||
        value.slot != index || value.valid != 1u || value.handleIndex !=
            D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX || value.offset != 0u ||
        value.stride != 0u || value.frequency != 0u || value.reserved0 != 0u) {
      return false;
    }
  }
  return true;
}

inline RenderTapeCpuGrammarResult renderTapeCpuGrammarReject(
    RenderTapeCpuGrammarReason reason, std::uint32_t eventIndex,
    std::uint32_t submissionIndex = 0xffffffffu,
    std::uint32_t recordIndex = 0xffffffffu) noexcept {
  return {reason, eventIndex, submissionIndex, recordIndex};
}

inline bool renderTapeCpuGrammarBufferDispositionValid(
    std::uint32_t disposition) noexcept {
  return disposition <= static_cast<std::uint32_t>(
                            RenderTapeBufferMutationDisposition::Discard);
}

inline RenderTapeCpuGrammarResult renderTapeCpuGrammarBootstrap(
    std::span<const std::byte> payload, std::uint32_t eventIndex,
    std::uint32_t overlayCount) noexcept {
  RenderTapeBootstrapHeader fixed{};
  if (!renderTapeCpuGrammarLoad(payload, 0u, fixed) ||
      fixed.overlayCount != overlayCount || fixed.overlayCount == 0u) {
    return renderTapeCpuGrammarReject(
        RenderTapeCpuGrammarReason::BootstrapOverlayUnsupported, eventIndex);
  }
  if (fixed.gammaRampBytes != 0u) {
    return renderTapeCpuGrammarReject(
        RenderTapeCpuGrammarReason::BootstrapGammaUnsupported, eventIndex);
  }

  const auto chunks = payload.subspan(sizeof(fixed));
  std::size_t offset = 0u;
  for (std::uint32_t chunkIndex = 0u; chunkIndex < fixed.overlayCount;
       ++chunkIndex) {
    D9CCommandChunkWireHeader header{};
    if (!renderTapeCpuGrammarLoad(chunks, offset, header)) {
      return renderTapeCpuGrammarReject(
          RenderTapeCpuGrammarReason::BootstrapOverlayUnsupported, eventIndex,
          chunkIndex);
    }
    const auto remaining = chunks.size() - offset;
    if (header.payloadArenaOffset > remaining ||
        header.payloadArenaSize > remaining - header.payloadArenaOffset) {
      return renderTapeCpuGrammarReject(
          RenderTapeCpuGrammarReason::BootstrapOverlayUnsupported, eventIndex,
          chunkIndex);
    }
    const auto totalBytes = static_cast<std::size_t>(header.payloadArenaOffset) +
                            header.payloadArenaSize;
    ImportedChunkView imported;
    const CommandChunkEnvelope envelope{
        .version = D9C_COMMAND_CHUNK_WIRE_VERSION,
        .recordCount = header.recordCount,
        .handleCount = header.handleCount,
    };
    if (!importPrevalidatedCommandChunk(
            chunks.subspan(offset, totalBytes), envelope, imported)) {
      return renderTapeCpuGrammarReject(
          RenderTapeCpuGrammarReason::BootstrapOverlayUnsupported, eventIndex,
          chunkIndex);
    }
    if (!imported.handles.empty()) {
      return renderTapeCpuGrammarReject(
          RenderTapeCpuGrammarReason::BootstrapHandlesUnsupported, eventIndex,
          chunkIndex);
    }
    if (imported.records.empty()) {
      return renderTapeCpuGrammarReject(
          RenderTapeCpuGrammarReason::BootstrapRecordUnsupported, eventIndex,
          chunkIndex);
    }
    for (std::size_t recordIndex = 0u;
         recordIndex < imported.records.size(); ++recordIndex) {
      if (imported.record(recordIndex).header.type !=
          D9C_COMMAND_RECORD_APPLY_STATE) {
        return renderTapeCpuGrammarReject(
            RenderTapeCpuGrammarReason::BootstrapRecordUnsupported, eventIndex,
            chunkIndex, static_cast<std::uint32_t>(recordIndex));
      }
    }
    offset += totalBytes;
  }
  if (offset != chunks.size()) {
    return renderTapeCpuGrammarReject(
        RenderTapeCpuGrammarReason::BootstrapOverlayUnsupported, eventIndex);
  }
  return {};
}

}  // namespace detail

inline RenderTapeCpuGrammarResult preflightRenderTapeCpuGrammar(
    const ImportedRenderTapeView& tape,
    bool allowDynamicResources = false,
    bool requireManagedAllocationFault = false,
    bool allowDrawIsland = false) noexcept {
  if (tape.events.size() > kRenderTapeCpuGrammarMaxEvents) {
    return detail::renderTapeCpuGrammarReject(
        RenderTapeCpuGrammarReason::EventLimit, 0xffffffffu);
  }

  bool sawBootstrap = false;
  bool sawBufferDefine = false;
  D9CWireObjectIdentity bufferIdentity{};
  std::uint64_t bufferBytes = 0u;
  bool sawMutation = false;
  bool sawDraw = false;
  bool sawUnbind = false;
  bool sawPresentRecord = false;
  bool sawBufferDestroy = false;
  bool sawOutputSurface = false;
  D9CWireObjectIdentity outputSurface{};
  bool sawPresentComplete = false;
  std::size_t mutationCount = 0u;
  std::size_t drawCount = 0u;
  bool dynamicBuffer = false;
  std::size_t submissionCount = 0u;
  std::size_t commandRecordCount = 0u;

  for (std::size_t eventIndex = 0u; eventIndex < tape.events.size();
       ++eventIndex) {
    const auto event = tape.event(eventIndex);
    const auto type = static_cast<RenderTapeEventType>(event.header.type);
    if (sawPresentComplete) {
      return detail::renderTapeCpuGrammarReject(
          RenderTapeCpuGrammarReason::EventAfterPresentComplete,
          static_cast<std::uint32_t>(eventIndex));
    }
    switch (type) {
    case RenderTapeEventType::BootstrapState: {
      if (sawBootstrap) {
        return detail::renderTapeCpuGrammarReject(
            RenderTapeCpuGrammarReason::DuplicateBootstrap,
            static_cast<std::uint32_t>(eventIndex));
      }
      RenderTapeBootstrapHeader fixed{};
      if (!detail::renderTapeCpuGrammarLoad(event.payload, 0u, fixed)) {
        return detail::renderTapeCpuGrammarReject(
            RenderTapeCpuGrammarReason::BootstrapOverlayUnsupported,
            static_cast<std::uint32_t>(eventIndex));
      }
      const auto result = detail::renderTapeCpuGrammarBootstrap(
          event.payload, static_cast<std::uint32_t>(eventIndex),
          fixed.overlayCount);
      if (!result.accepted()) return result;
      sawBootstrap = true;
      break;
    }
    case RenderTapeEventType::ObjectDefine: {
      RenderTapeObjectDefineHeader fixed{};
      if (!detail::renderTapeCpuGrammarLoad(event.payload, 0u, fixed)) {
        return detail::renderTapeCpuGrammarReject(
            RenderTapeCpuGrammarReason::BufferDescriptorRequired,
            static_cast<std::uint32_t>(eventIndex));
      }
      if (fixed.identity.kind == D9C_CHUNK_HANDLE_KIND_SURFACE) {
        if (sawOutputSurface || sawPresentRecord || sawPresentComplete ||
            fixed.descriptorKind != static_cast<std::uint32_t>(
                                        RenderTapeDescriptorKind::Surface)) {
          return detail::renderTapeCpuGrammarReject(
              RenderTapeCpuGrammarReason::PresentSurfaceDescriptorRequired,
              static_cast<std::uint32_t>(eventIndex));
        }
        const auto descriptor = event.payload.subspan(sizeof(fixed));
        RenderTapeSurfaceDescriptorV2 surface{};
        if (fixed.descriptorBytes != sizeof(surface) ||
            descriptor.size() != fixed.descriptorBytes ||
            !renderTapeLoadSurfaceDescriptorV2(descriptor, surface) ||
            static_cast<RenderTapeSurfaceStorage>(surface.storage) !=
                RenderTapeSurfaceStorage::SwapchainBackbuffer ||
            static_cast<RenderTapeInitialContentDisposition>(
                surface.initialContentDisposition) !=
                RenderTapeInitialContentDisposition::ProducedPresentOutput ||
            surface.subresource != 0u ||
            !renderTapeZeroIdentity(surface.parentTexture) ||
            fixed.expectedContentBytes != 0u ||
            fixed.expectedContentCount != 0u || surface.surface.resourceType != 1u ||
            surface.surface.width == 0u || surface.surface.height == 0u ||
            surface.surface.depth != 1u || surface.surface.pool != 0u ||
            surface.surface.multiSampleType != 0u ||
            surface.surface.multiSampleQuality != 0u ||
            surface.surface.usage != 1u ||
            (surface.surface.format != 21u && surface.surface.format != 22u)) {
          return detail::renderTapeCpuGrammarReject(
              RenderTapeCpuGrammarReason::PresentSurfaceDescriptorMalformed,
              static_cast<std::uint32_t>(eventIndex));
        }
        outputSurface = fixed.identity;
        sawOutputSurface = true;
        break;
      }
      if (fixed.identity.kind != D9C_CHUNK_HANDLE_KIND_BUFFER ||
          fixed.descriptorKind !=
              static_cast<std::uint32_t>(RenderTapeDescriptorKind::Buffer)) {
        return detail::renderTapeCpuGrammarReject(
            RenderTapeCpuGrammarReason::BufferDescriptorRequired,
            static_cast<std::uint32_t>(eventIndex));
      }
      const auto descriptor = event.payload.subspan(sizeof(fixed));
      D9CBufferDesc buffer{};
      if (fixed.descriptorBytes != sizeof(buffer) ||
          descriptor.size() != fixed.descriptorBytes ||
          !detail::renderTapeCpuGrammarLoad(descriptor, 0u, buffer) ||
              buffer.size == 0u ||
              buffer.size > kRenderTapeCpuGrammarMaxBufferBytes) {
        return detail::renderTapeCpuGrammarReject(
            RenderTapeCpuGrammarReason::BufferDescriptorMalformed,
            static_cast<std::uint32_t>(eventIndex));
      }
      constexpr std::uint32_t kD3DUsageDynamic = 0x200u;
      dynamicBuffer = (buffer.usage & kD3DUsageDynamic) != 0u;
      if (allowDrawIsland && (dynamicBuffer || buffer.pool != 0u)) {
        return detail::renderTapeCpuGrammarReject(
            RenderTapeCpuGrammarReason::GroupedBufferUnsupported,
            static_cast<std::uint32_t>(eventIndex));
      }
      if (requireManagedAllocationFault &&
          (dynamicBuffer || buffer.pool != 1u)) {
        return detail::renderTapeCpuGrammarReject(
            RenderTapeCpuGrammarReason::ManagedAllocationFaultRequiresManagedBuffer,
            static_cast<std::uint32_t>(eventIndex));
      }
      if (dynamicBuffer &&
          (!allowDynamicResources || buffer.pool != 0u)) {
        return detail::renderTapeCpuGrammarReject(
            RenderTapeCpuGrammarReason::DynamicBackendRequired,
            static_cast<std::uint32_t>(eventIndex));
      }
      if (sawBufferDefine) {
        return detail::renderTapeCpuGrammarReject(
            RenderTapeCpuGrammarReason::MultipleBufferDescriptors,
            static_cast<std::uint32_t>(eventIndex));
      }
      if (sawMutation || sawDraw || sawBufferDestroy) {
        return detail::renderTapeCpuGrammarReject(
            RenderTapeCpuGrammarReason::BufferOrderUnsupported,
            static_cast<std::uint32_t>(eventIndex));
      }
      bufferIdentity = fixed.identity;
      bufferBytes = buffer.size;
      sawBufferDefine = true;
      break;
    }
    case RenderTapeEventType::ResourceMutation: {
      RenderTapeResourceMutationHeader fixed{};
      if (!detail::renderTapeCpuGrammarLoad(event.payload, 0u, fixed) ||
          !sawBufferDefine || sawBufferDestroy ||
          !detail::renderTapeCpuGrammarSameIdentity(fixed.identity,
                                                    bufferIdentity) ||
          fixed.identity.kind != D9C_CHUNK_HANDLE_KIND_BUFFER ||
          (fixed.kind != static_cast<std::uint32_t>(
               RenderTapeMutationKind::CpuUnlock) &&
           fixed.kind != static_cast<std::uint32_t>(
               RenderTapeMutationKind::Upload))) {
        return detail::renderTapeCpuGrammarReject(
            RenderTapeCpuGrammarReason::BufferMutationUnsupported,
            static_cast<std::uint32_t>(eventIndex));
      }
      if (fixed.subresource != 0u || fixed.byteOffset != 0u ||
          fixed.byteSize != bufferBytes || fixed.byteSize == 0u ||
          fixed.byteOffset > std::numeric_limits<std::uint32_t>::max() ||
          fixed.byteSize > std::numeric_limits<std::uint32_t>::max() ||
          (!dynamicBuffer && mutationCount != 0u) ||
          (dynamicBuffer && mutationCount >= 2u) ||
          (dynamicBuffer && mutationCount == 0u && sawDraw) ||
          (dynamicBuffer && mutationCount == 1u && !sawDraw) ||
          ((!dynamicBuffer || mutationCount == 0u) && sawDraw &&
           !dynamicBuffer) ||
          fixed.bufferDisposition != static_cast<std::uint32_t>(
              dynamicBuffer && mutationCount == 1u
                  ? RenderTapeBufferMutationDisposition::Discard
                  : RenderTapeBufferMutationDisposition::Plain) ||
          !detail::renderTapeCpuGrammarBufferDispositionValid(
              fixed.bufferDisposition)) {
        return detail::renderTapeCpuGrammarReject(
            RenderTapeCpuGrammarReason::BufferMutationRangeUnsupported,
            static_cast<std::uint32_t>(eventIndex));
      }
      if (dynamicBuffer && mutationCount == 1u && drawCount != 1u) {
        return detail::renderTapeCpuGrammarReject(
            RenderTapeCpuGrammarReason::BufferMutationUnsupported,
            static_cast<std::uint32_t>(eventIndex));
      }
      ++mutationCount;
      sawMutation = true;
      break;
    }
    case RenderTapeEventType::CommandChunk: {
      if (submissionCount == kRenderTapeCpuGrammarMaxSubmissions) {
        return detail::renderTapeCpuGrammarReject(
            RenderTapeCpuGrammarReason::SubmissionLimit,
            static_cast<std::uint32_t>(eventIndex),
            static_cast<std::uint32_t>(submissionCount));
      }
      RenderTapeCommandChunkHeader fixed{};
      if (!detail::renderTapeCpuGrammarLoad(event.payload, 0u, fixed)) {
        return detail::renderTapeCpuGrammarReject(
            RenderTapeCpuGrammarReason::CommandChunkUnsupported,
            static_cast<std::uint32_t>(eventIndex));
      }
      ImportedChunkView imported;
      const CommandChunkEnvelope envelope{
          .version = fixed.wireVersion,
          .recordCount = fixed.recordCount,
          .handleCount = fixed.handleCount,
      };
      if (!importPrevalidatedCommandChunk(
              event.payload.subspan(sizeof(fixed)), envelope, imported)) {
        return detail::renderTapeCpuGrammarReject(
            RenderTapeCpuGrammarReason::CommandChunkUnsupported,
            static_cast<std::uint32_t>(eventIndex));
      }
      if (!sawBufferDefine) {
        return detail::renderTapeCpuGrammarReject(
            RenderTapeCpuGrammarReason::BufferOrderUnsupported,
            static_cast<std::uint32_t>(eventIndex),
            static_cast<std::uint32_t>(submissionCount));
      }
      for (const auto& handle : imported.handles) {
        const auto identity = detail::renderTapeCpuGrammarIdentity(handle);
        if (handle.kind != D9C_CHUNK_HANDLE_KIND_BUFFER ||
            !detail::renderTapeCpuGrammarSameIdentity(identity, bufferIdentity) ||
            sawBufferDestroy) {
          return detail::renderTapeCpuGrammarReject(
              sawBufferDestroy
                  ? RenderTapeCpuGrammarReason::BufferReferenceUnsupported
                  : RenderTapeCpuGrammarReason::CommandHandlesUnsupported,
              static_cast<std::uint32_t>(eventIndex),
              static_cast<std::uint32_t>(submissionCount));
        }
      }
      if (commandRecordCount + imported.records.size() >
          kRenderTapeCpuGrammarMaxCommandRecords) {
        return detail::renderTapeCpuGrammarReject(
            RenderTapeCpuGrammarReason::CommandRecordLimit,
            static_cast<std::uint32_t>(eventIndex),
            static_cast<std::uint32_t>(submissionCount));
      }
      if (allowDrawIsland && !dynamicBuffer && imported.records.size() > 1u) {
        for (const auto& record : imported.records) {
          if (record.type != D9C_COMMAND_RECORD_DRAW_PRIMITIVE) {
            return detail::renderTapeCpuGrammarReject(
                RenderTapeCpuGrammarReason::DrawOrderUnsupported,
                static_cast<std::uint32_t>(eventIndex),
                static_cast<std::uint32_t>(submissionCount));
          }
        }
      }
      for (std::size_t recordIndex = 0u;
           recordIndex < imported.records.size(); ++recordIndex) {
        const auto record = imported.record(recordIndex);
        if (record.header.type != D9C_COMMAND_RECORD_APPLY_STATE &&
            record.header.type != D9C_COMMAND_RECORD_DRAW_PRIMITIVE &&
            record.header.type != D9C_COMMAND_RECORD_PRESENT) {
          return detail::renderTapeCpuGrammarReject(
              RenderTapeCpuGrammarReason::CommandRecordUnsupported,
              static_cast<std::uint32_t>(eventIndex),
              static_cast<std::uint32_t>(submissionCount),
              static_cast<std::uint32_t>(recordIndex));
        }
        if (sawPresentRecord && record.header.type != D9C_COMMAND_RECORD_PRESENT) {
          return detail::renderTapeCpuGrammarReject(
              RenderTapeCpuGrammarReason::DrawOrderUnsupported,
              static_cast<std::uint32_t>(eventIndex),
              static_cast<std::uint32_t>(submissionCount),
              static_cast<std::uint32_t>(recordIndex));
        }
        for (const auto& handle : record.handles) {
          const auto identity = detail::renderTapeCpuGrammarIdentity(handle);
          if (handle.kind != D9C_CHUNK_HANDLE_KIND_BUFFER ||
              !detail::renderTapeCpuGrammarSameIdentity(identity, bufferIdentity) ||
              sawBufferDestroy) {
            return detail::renderTapeCpuGrammarReject(
                sawBufferDestroy
                    ? RenderTapeCpuGrammarReason::BufferReferenceUnsupported
                    : RenderTapeCpuGrammarReason::CommandHandlesUnsupported,
                static_cast<std::uint32_t>(eventIndex),
                static_cast<std::uint32_t>(submissionCount),
                static_cast<std::uint32_t>(recordIndex));
          }
        }
        switch (record.header.type) {
        case D9C_COMMAND_RECORD_DRAW_PRIMITIVE: {
          const bool basicDraw = detail::renderTapeCpuGrammarBasicDraw(
              record, bufferIdentity, bufferBytes);
          if (!sawMutation || sawBufferDestroy ||
              (!dynamicBuffer && sawDraw && !allowDrawIsland) ||
              (dynamicBuffer && (drawCount >= 2u ||
                                 (drawCount == 1u && mutationCount != 2u))) ||
              !basicDraw) {
            return detail::renderTapeCpuGrammarReject(
                !basicDraw ? RenderTapeCpuGrammarReason::DrawShapeUnsupported
                           : RenderTapeCpuGrammarReason::DrawOrderUnsupported,
                static_cast<std::uint32_t>(eventIndex),
                static_cast<std::uint32_t>(submissionCount),
                static_cast<std::uint32_t>(recordIndex));
          }
          sawDraw = true;
          ++drawCount;
          break;
        }
        case D9C_COMMAND_RECORD_APPLY_STATE:
          if (!sawDraw || sawUnbind || sawBufferDestroy ||
              (dynamicBuffer && (mutationCount != 2u || drawCount != 2u)) ||
              !detail::renderTapeCpuGrammarCanonicalUnbind(record)) {
            return detail::renderTapeCpuGrammarReject(
                detail::renderTapeCpuGrammarCanonicalUnbind(record)
                    ? RenderTapeCpuGrammarReason::DrawOrderUnsupported
                    : RenderTapeCpuGrammarReason::CommandRecordUnsupported,
                static_cast<std::uint32_t>(eventIndex),
                static_cast<std::uint32_t>(submissionCount),
                static_cast<std::uint32_t>(recordIndex));
          }
          sawUnbind = true;
          break;
        case D9C_COMMAND_RECORD_PRESENT:
          if (!sawDraw || sawPresentRecord ||
              (dynamicBuffer && (mutationCount != 2u || drawCount != 2u)) ||
              !detail::renderTapeCpuGrammarPresentRecord(record)) {
            return detail::renderTapeCpuGrammarReject(
                !detail::renderTapeCpuGrammarPresentRecord(record)
                    ? RenderTapeCpuGrammarReason::CommandRecordUnsupported
                    : RenderTapeCpuGrammarReason::DrawOrderUnsupported,
                static_cast<std::uint32_t>(eventIndex),
                static_cast<std::uint32_t>(submissionCount),
                static_cast<std::uint32_t>(recordIndex));
          }
          sawPresentRecord = true;
          break;
        default:
          break;
        }
      }
      ++submissionCount;
      commandRecordCount += imported.records.size();
      break;
    }
    case RenderTapeEventType::PresentComplete: {
      RenderTapePresentCompleteHeader fixed{};
      RenderTapeOracleAttachment oracle{};
      if (!detail::renderTapeCpuGrammarLoad(event.payload, 0u, fixed) ||
          !sawOutputSurface || !sawDraw || !sawPresentRecord ||
          !sawBufferDestroy || !sawUnbind || fixed.oracleCount != 1u ||
          (dynamicBuffer && (mutationCount != 2u || drawCount != 2u)) ||
          fixed.oracleBytes != sizeof(oracle) ||
          event.payload.size() != sizeof(fixed) + sizeof(oracle) ||
          fixed.digestValidity !=
              static_cast<std::uint32_t>(RenderTapeDigestValidity::NotCaptured) ||
          !detail::renderTapeCpuGrammarLoad(event.payload, sizeof(fixed), oracle) ||
          !renderTapePresentOutputIdentityMatchesCommand(oracle.identity,
                                                         outputSurface) ||
          oracle.descriptorKind !=
              static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface) ||
          oracle.reserved0 != 0u) {
        return detail::renderTapeCpuGrammarReject(
            RenderTapeCpuGrammarReason::PresentMetadataUnsupported,
            static_cast<std::uint32_t>(eventIndex));
      }
      sawPresentComplete = true;
      break;
    }
    case RenderTapeEventType::ObjectDestroy: {
      RenderTapeObjectDestroyHeader fixed{};
      if (!detail::renderTapeCpuGrammarLoad(event.payload, 0u, fixed) ||
          fixed.identity.kind != D9C_CHUNK_HANDLE_KIND_BUFFER ||
          !sawBufferDefine || !sawMutation || !sawDraw || !sawUnbind ||
          sawBufferDestroy || sawPresentComplete ||
          (dynamicBuffer && (mutationCount != 2u || drawCount != 2u)) ||
          !detail::renderTapeCpuGrammarSameIdentity(fixed.identity,
                                                    bufferIdentity)) {
        return detail::renderTapeCpuGrammarReject(
            RenderTapeCpuGrammarReason::BufferDestroyUnsupported,
            static_cast<std::uint32_t>(eventIndex));
      }
      sawBufferDestroy = true;
      break;
    }
    case RenderTapeEventType::OrderedControl:
      return detail::renderTapeCpuGrammarReject(
          RenderTapeCpuGrammarReason::UnsupportedEvent,
          static_cast<std::uint32_t>(eventIndex));
    }
  }
  if (!sawBootstrap || !sawBufferDefine || !sawMutation || !sawDraw ||
      !sawUnbind || !sawPresentRecord || !sawBufferDestroy ||
      !sawOutputSurface || !sawPresentComplete ||
      (dynamicBuffer ? (mutationCount != 2u || drawCount != 2u)
                     : (mutationCount != 1u ||
                        (allowDrawIsland ? drawCount == 0u : drawCount != 1u)))) {
    return detail::renderTapeCpuGrammarReject(
        RenderTapeCpuGrammarReason::IncompleteGrammar, 0xffffffffu);
  }
  return {};
}

}  // namespace dxmt9::d3d9
