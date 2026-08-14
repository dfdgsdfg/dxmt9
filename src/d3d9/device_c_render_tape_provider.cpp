#include "device_c_render_tape_provider.hpp"

#include "device_c_chunk_replay.hpp"
#include "device_c_common.hpp"
#include "device_c_render_tape_capture.hpp"
#include "device_c_render_tape_capture_layout.hpp"
#include "dxmt9/dxmt9_device.hpp"
#include "dxmt9/dxmt9_presenter.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>

namespace dxmt9::d3d9 {
namespace {

template <typename T>
bool load(std::span<const std::byte> bytes, std::size_t offset, T& out) {
  if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) return false;
  std::memcpy(&out, bytes.data() + offset, sizeof(T));
  return true;
}

bool sameIdentity(const D9CWireObjectIdentity& a,
                  const D9CWireObjectIdentity& b) {
  return a.kind == b.kind && a.generation == b.generation &&
         a.objectId == b.objectId;
}

bool sameDigest(const RenderTapeDigest& a, const RenderTapeDigest& b) {
  return std::equal(a.begin(), a.end(), b.begin());
}

bool checkedMul(std::uint64_t a, std::uint64_t b,
                std::uint64_t& out) noexcept {
  if (a != 0u && b > std::numeric_limits<std::uint64_t>::max() / a) {
    return false;
  }
  out = a * b;
  return true;
}

const RenderTapeProviderBlob* findBlob(
    std::span<const RenderTapeProviderBlob> blobs,
    const RenderTapeDigest& digest) {
  const auto it = std::find_if(blobs.begin(), blobs.end(), [&](const auto& blob) {
    return sameDigest(blob.digest, digest);
  });
  return it == blobs.end() ? nullptr : &*it;
}

RenderTapeBlobCatalogue makeCatalogue(
    std::span<const RenderTapeProviderBlob> blobs, bool& valid) {
  RenderTapeBlobCatalogue catalogue;
  valid = true;
  for (std::size_t i = 0; i < blobs.size(); ++i) {
    if (std::any_of(blobs.begin(), blobs.begin() + i, [&](const auto& prior) {
          return sameDigest(prior.digest, blobs[i].digest);
        })) {
      valid = false;
      return {};
    }
    const auto actual = RenderTapeCaptureSession::sha256(blobs[i].bytes);
    if (!sameDigest(actual, blobs[i].digest)) {
      valid = false;
      return {};
    }
    catalogue.blobs.push_back(RenderTapeBlob{
        .digest = blobs[i].digest,
        .size = blobs[i].bytes.size(),
        .verified = 1u,
    });
  }
  return catalogue;
}

bool fullRect(const D9CRect& rect, std::uint32_t width,
              std::uint32_t height) {
  return rect.left == 0 && rect.top == 0 &&
         rect.right == static_cast<std::int32_t>(width) &&
         rect.bottom == static_cast<std::int32_t>(height);
}

struct Definition {
  RenderTapeObjectDefineHeader fixed{};
  std::span<const std::byte> descriptor{};
};

struct Chunk {
  CommandChunkEnvelope envelope{};
  std::span<const std::byte> bytes{};
};

struct IntervalPlan {
  std::vector<RenderTapeResourceMutationHeader> mutationsBefore{};
  std::vector<Chunk> frame{};
  RenderTapeDigestValidity expectedDigestValidity =
      RenderTapeDigestValidity::NotCaptured;
  RenderTapeDigest expectedDigest{};
  std::uint64_t presentOrdinal = 0u;
  std::uint64_t completionOrdinal = 0u;
  bool sawTexturedDraw = false;
};

struct PreflightPlan {
  ImportedRenderTapeView tape{};
  RenderTapeBlobCatalogue catalogue{};
  std::vector<Definition> definitions{};
  std::vector<RenderTapeResourceMutationHeader> initialMutations{};
  std::vector<Chunk> bootstrap{};
  std::array<IntervalPlan, kRenderTapeMaxReplayIntervals> intervals{};
  std::uint32_t intervalCount = 0u;
  D9CWireObjectIdentity outputIdentity{};
  D9CSurfaceDesc outputDesc{};
  D9CWireObjectIdentity textureIdentity{};
  RenderTapeTextureDescriptorV2 textureDesc{};
  D9CSurfaceDesc textureLevel0{};
  bool textureProducedByCapturedPass = false;
  D9CWireObjectIdentity vertexDeclarationIdentity{};
};

enum class FrameRecordState : std::uint8_t {
  ExpectClear,
  ExpectDrawOrPresent,
  ExpectPresent,
  Complete,
};

struct BoundedDrawState {
  D9CWireObjectIdentity texture{};
  D9CWireObjectIdentity vertexDeclaration{};
  std::uint32_t fvf = 0u;
  bool unsupportedBinding = false;
};

bool nullHandle(std::uint32_t handleIndex) {
  return handleIndex == D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX;
}

bool handleIdentity(const ImportedChunkView& chunk, std::uint32_t handleIndex,
                    std::uint32_t kind, D9CWireObjectIdentity& identity) {
  if (nullHandle(handleIndex)) {
    identity = {};
    return true;
  }
  if (handleIndex >= chunk.handles.size() ||
      chunk.handles[handleIndex].kind != kind) return false;
  const auto& handle = chunk.handles[handleIndex];
  identity = D9CWireObjectIdentity{
      .kind = handle.kind,
      .generation = handle.generation,
      .objectId = handle.objectId,
  };
  return true;
}

bool applyBoundedBindings(const ImportedChunkView& chunk,
                          const ImportedRecordView& record,
                          BoundedDrawState& state,
                          std::span<const std::byte>* upVertices = nullptr) {
  for (std::size_t index = 0u; index < record.sections.size(); ++index) {
    const auto section = record.section(index);
    switch (section.descriptor.kind) {
    case D9C_COMMAND_CHUNK_SECTION_TEXTURE:
      for (std::uint32_t bindingIndex = 0u;
           bindingIndex < section.descriptor.count; ++bindingIndex) {
        D9CCommandChunkWireTextureBinding binding{};
        if (!load(section.payload, bindingIndex * sizeof(binding), binding))
          return false;
        if (!binding.valid) continue;
        D9CWireObjectIdentity identity{};
        if (!handleIdentity(chunk, binding.handleIndex,
                            D9C_CHUNK_HANDLE_KIND_TEXTURE, identity)) return false;
        if (binding.slot == 0u) {
          state.texture = identity;
        } else if (identity.objectId != 0u) {
          state.unsupportedBinding = true;
        }
      }
      break;
    case D9C_COMMAND_CHUNK_SECTION_STREAM:
      for (std::uint32_t bindingIndex = 0u;
           bindingIndex < section.descriptor.count; ++bindingIndex) {
        D9CCommandChunkWireStreamBinding binding{};
        if (!load(section.payload, bindingIndex * sizeof(binding), binding))
          return false;
        if (binding.valid && !nullHandle(binding.handleIndex))
          state.unsupportedBinding = true;
      }
      break;
    case D9C_COMMAND_CHUNK_SECTION_SHADER:
      for (std::uint32_t bindingIndex = 0u;
           bindingIndex < section.descriptor.count; ++bindingIndex) {
        D9CCommandChunkWireShaderBinding binding{};
        if (!load(section.payload, bindingIndex * sizeof(binding), binding))
          return false;
        if (binding.valid && !nullHandle(binding.handleIndex))
          state.unsupportedBinding = true;
      }
      break;
    case D9C_COMMAND_CHUNK_SECTION_VERTEX_INPUT: {
      D9CCommandChunkWireVertexInput input{};
      if (section.descriptor.count != 1u ||
          !load(section.payload, 0u, input)) return false;
      if (!input.valid ||
          (input.kind != D9C_COMMAND_CHUNK_VERTEX_INPUT_FVF &&
           input.kind != D9C_COMMAND_CHUNK_VERTEX_INPUT_DECLARATION)) {
        state.unsupportedBinding = true;
      } else if (input.kind == D9C_COMMAND_CHUNK_VERTEX_INPUT_FVF) {
        if (!nullHandle(input.handleIndex)) state.unsupportedBinding = true;
        state.fvf = input.value;
        state.vertexDeclaration = {};
      } else {
        D9CWireObjectIdentity identity{};
        if (!handleIdentity(chunk, input.handleIndex,
                            D9C_CHUNK_HANDLE_KIND_VERTEX_DECL, identity) ||
            identity.objectId == 0u) {
          return false;
        }
        state.vertexDeclaration = identity;
        state.fvf = 0u;
      }
      break;
    }
    case D9C_COMMAND_CHUNK_SECTION_INDEX_BUFFER:
      for (std::uint32_t bindingIndex = 0u;
           bindingIndex < section.descriptor.count; ++bindingIndex) {
        D9CCommandChunkWireIndexBinding binding{};
        if (!load(section.payload, bindingIndex * sizeof(binding), binding))
          return false;
        if (binding.valid && !nullHandle(binding.handleIndex))
          state.unsupportedBinding = true;
      }
      break;
    case D9C_COMMAND_CHUNK_SECTION_DEPTH_STENCIL: {
      D9CCommandChunkWireDepthStencilBinding binding{};
      if (section.descriptor.count != 1u ||
          !load(section.payload, 0u, binding)) return false;
      if (binding.valid && !nullHandle(binding.handleIndex))
        state.unsupportedBinding = true;
      break;
    }
    case D9C_COMMAND_CHUNK_SECTION_UP_INDEX_DATA:
      state.unsupportedBinding = true;
      break;
    case D9C_COMMAND_CHUNK_SECTION_UP_VERTEX_DATA:
      if (!upVertices || !upVertices->empty()) return false;
      *upVertices = section.payload;
      break;
    default:
      break;
    }
  }
  return true;
}

bool acceptedTexturedDraw(const ImportedChunkView& chunk,
                          const ImportedRecordView& record,
                          BoundedDrawState& state,
                          const D9CWireObjectIdentity& expectedVertexDeclaration) {
  constexpr std::uint32_t kTriangleList = 4u;
  constexpr std::uint32_t kXyzRhwDiffuseTex1Fvf = 0x144u;
  constexpr std::uint32_t kVertexStride = 28u;
  if (record.drawHeader.flags != 0u) return false;
  for (const auto& section : record.sections) {
    if (section.kind != D9C_COMMAND_CHUNK_SECTION_TEXTURE &&
        section.kind != D9C_COMMAND_CHUNK_SECTION_VERTEX_INPUT &&
        section.kind != D9C_COMMAND_CHUNK_SECTION_UP_VERTEX_DATA) {
      return false;
    }
  }
  std::span<const std::byte> vertices;
  if (!applyBoundedBindings(chunk, record, state, &vertices)) return false;
  const bool usesExactDeclaration =
      expectedVertexDeclaration.objectId != 0u &&
      sameIdentity(state.vertexDeclaration, expectedVertexDeclaration) &&
      state.fvf == 0u;
  const bool usesExactFvf = expectedVertexDeclaration.objectId == 0u &&
      state.vertexDeclaration.objectId == 0u &&
      state.fvf == kXyzRhwDiffuseTex1Fvf;
  if (state.unsupportedBinding || state.texture.objectId == 0u ||
      (!usesExactDeclaration && !usesExactFvf) ||
      record.drawHeader.primitiveType != kTriangleList ||
      record.drawHeader.primitiveCount != 1u ||
      record.drawHeader.stride != kVertexStride ||
      vertices.size() != 3u * kVertexStride) return false;
  return true;
}

bool mutationCatalogueMatches(
    const PreflightPlan& plan,
    std::span<const RenderTapeProviderBlob> blobs) {
  const auto matches = [&](const RenderTapeResourceMutationHeader& mutation) {
    const auto definition = std::find_if(
        plan.definitions.begin(), plan.definitions.end(),
        [&](const auto& value) {
          return sameIdentity(value.fixed.identity, mutation.identity);
        });
    const auto* blob = findBlob(blobs, mutation.digest);
    return definition != plan.definitions.end() && blob &&
           blob->bytes.size() == mutation.byteSize;
  };
  if (!std::all_of(plan.initialMutations.begin(),
                   plan.initialMutations.end(), matches)) {
    return false;
  }
  for (std::uint32_t i = 0u; i < plan.intervalCount; ++i) {
    if (!std::all_of(plan.intervals[i].mutationsBefore.begin(),
                     plan.intervals[i].mutationsBefore.end(), matches)) {
      return false;
    }
  }
  return true;
}

constexpr std::array<std::byte, 32u> kProductionTexturedVertexDeclaration{
    std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
    std::byte{0x03}, std::byte{0x00}, std::byte{0x09}, std::byte{0x00},
    std::byte{0x00}, std::byte{0x00}, std::byte{0x10}, std::byte{0x00},
    std::byte{0x04}, std::byte{0x00}, std::byte{0x0a}, std::byte{0x00},
    std::byte{0x00}, std::byte{0x00}, std::byte{0x14}, std::byte{0x00},
    std::byte{0x01}, std::byte{0x00}, std::byte{0x05}, std::byte{0x00},
    std::byte{0xff}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
    std::byte{0x11}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
};

bool acceptedProductionTexturedVertexDeclaration(
    const Definition& definition, std::span<const RenderTapeProviderBlob> blobs) {
  const auto& fixed = definition.fixed;
  if (fixed.identity.kind != D9C_CHUNK_HANDLE_KIND_VERTEX_DECL ||
      fixed.descriptorKind != static_cast<std::uint32_t>(
                                  RenderTapeDescriptorKind::VertexDeclaration) ||
      definition.descriptor.size() != sizeof(RenderTapeVertexDeclDescriptor) ||
      fixed.payloadValidity != static_cast<std::uint32_t>(
                                  RenderTapeDigestValidity::Sha256) ||
      fixed.immutablePayloadBytes != kProductionTexturedVertexDeclaration.size() ||
      fixed.expectedContentBytes != 0u || fixed.expectedContentCount != 0u) {
    return false;
  }
  RenderTapeVertexDeclDescriptor descriptor{};
  if (!load(definition.descriptor, 0u, descriptor) ||
      descriptor.elementCount != 4u || descriptor.elementBytes !=
                                             kProductionTexturedVertexDeclaration.size()) {
    return false;
  }
  const auto* blob = findBlob(blobs, fixed.immutablePayloadDigest);
  return blob && blob->bytes.size() == kProductionTexturedVertexDeclaration.size() &&
         std::equal(kProductionTexturedVertexDeclaration.begin(),
                    kProductionTexturedVertexDeclaration.end(), blob->bytes.begin());
}

bool parseChunks(std::span<const std::byte> bytes, std::uint32_t count,
                 std::vector<Chunk>& out) {
  std::size_t offset = 0u;
  for (std::uint32_t i = 0u; i < count; ++i) {
    D9CCommandChunkWireHeader header{};
    if (!load(bytes, offset, header) ||
        header.payloadArenaOffset > bytes.size() - offset ||
        header.payloadArenaSize >
            bytes.size() - offset - header.payloadArenaOffset) return false;
    const std::size_t totalBytes =
        static_cast<std::size_t>(header.payloadArenaOffset) +
        header.payloadArenaSize;
    out.push_back(Chunk{
        .envelope = CommandChunkEnvelope{
            .version = D9C_COMMAND_CHUNK_WIRE_VERSION,
            .recordCount = header.recordCount,
            .handleCount = header.handleCount,
        },
        .bytes = bytes.subspan(offset, totalBytes),
    });
    offset += totalBytes;
  }
  return offset == bytes.size();
}

bool acceptedSurface(const D9CSurfaceDesc& desc) {
  if (desc.resourceType != 1u || desc.width == 0u || desc.height == 0u ||
      desc.depth != 1u || desc.pool != 0u || desc.multiSampleType != 0u ||
      desc.multiSampleQuality != 0u || (desc.usage & 1u) == 0u ||
      (desc.usage & 2u) != 0u) return false;
  return desc.format == 21u || desc.format == 22u;
}

std::uint32_t mipExtent(std::uint32_t extent,
                        std::uint32_t mipLevel) noexcept {
  return mipLevel >= 32u ? 1u : std::max(1u, extent >> mipLevel);
}

bool acceptedTextureDescriptor(std::span<const std::byte> descriptor,
                               const RenderTapeTextureDescriptorV2& texture,
                               D9CSurfaceDesc& level0) {
  RenderTapeTextureDescriptorV2 canonical{};
  if (!renderTapeLoadTextureDescriptorV2(descriptor, canonical) ||
      canonical.dimension != texture.dimension ||
      canonical.mipLevelCount != texture.mipLevelCount ||
      canonical.subresourceCount != texture.subresourceCount ||
      canonical.initialContentDisposition !=
          texture.initialContentDisposition ||
      texture.dimension !=
          static_cast<std::uint32_t>(RenderTapeTextureDimension::Texture2D) ||
      texture.mipLevelCount == 0u ||
      texture.subresourceCount != texture.mipLevelCount ||
      (texture.initialContentDisposition != static_cast<std::uint32_t>(
           RenderTapeInitialContentDisposition::CompleteSeed) &&
       texture.initialContentDisposition != static_cast<std::uint32_t>(
           RenderTapeInitialContentDisposition::ProducedByCapturedPass)) ||
      texture.reserved0 != 0u ||
      descriptor.size() !=
          sizeof(texture) +
              static_cast<std::size_t>(texture.subresourceCount) *
                  sizeof(D9CSurfaceDesc) ||
      !load(descriptor, sizeof(texture), level0) ||
      level0.resourceType != 3u || level0.width == 0u ||
      level0.height == 0u || level0.depth != 1u || level0.pool != 0u ||
      level0.multiSampleType != 0u || level0.multiSampleQuality != 0u ||
      ((texture.initialContentDisposition == static_cast<std::uint32_t>(
            RenderTapeInitialContentDisposition::CompleteSeed) &&
        (level0.usage & 3u) != 0u) ||
       (texture.initialContentDisposition == static_cast<std::uint32_t>(
            RenderTapeInitialContentDisposition::ProducedByCapturedPass) &&
        (level0.usage & 1u) == 0u))) return false;
  const auto format = devicec::fmtFromD3D(level0.format);
  if (format == core::Format::Unknown || core::isCompressedFormat(format))
    return false;
  for (std::uint32_t level = 1u; level < texture.mipLevelCount; ++level) {
    D9CSurfaceDesc desc{};
    if (!load(descriptor, sizeof(texture) +
                              level * sizeof(D9CSurfaceDesc), desc) ||
        desc.format != level0.format || desc.resourceType != 3u ||
        desc.usage != level0.usage || desc.pool != level0.pool ||
        desc.multiSampleType != 0u || desc.multiSampleQuality != 0u ||
        desc.width != mipExtent(level0.width, level) ||
        desc.height != mipExtent(level0.height, level) ||
        desc.depth != 1u) return false;
  }
  return true;
}

FrameTapeReplayResult buildPlan(std::span<const std::byte> bytes,
                                std::span<const RenderTapeProviderBlob> blobs,
                                PreflightPlan* plan) {
  FrameTapeReplayResult result;
  result.conservation.inputBlobs = static_cast<std::uint32_t>(blobs.size());
  bool catalogueValid = false;
  auto catalogue = makeCatalogue(blobs, catalogueValid);
  if (!catalogueValid) {
    result.status = FrameTapeReplayStatus::InvalidBlobCatalogue;
    return result;
  }

  ImportedRenderTapeView tape;
  RenderTapeValidationScratch scratch;
  const auto validation = validateRenderTape(bytes, catalogue, &tape, scratch);
  if (!validation.valid()) {
    result.status = FrameTapeReplayStatus::InvalidTape;
    result.failedEventIndex = validation.failedEventIndex;
    return result;
  }
  result.validity.structurallyValid = true;
  result.validity.digestsValid = true;
  result.profile = tape.header.profile;
  result.intervalCount = tape.header.presentCount;
  result.coverage.eventCount = tape.header.eventCount;

  PreflightPlan candidate{.tape = tape, .catalogue = std::move(catalogue)};
  candidate.intervalCount = tape.header.presentCount;
  FrameTapeBootstrapOutputDisposition bootstrapDisposition =
      FrameTapeBootstrapOutputDisposition::Malformed;
  bool bootstrapAccepted = false;
  std::array<FrameRecordState, kRenderTapeMaxReplayIntervals> recordStates{
      FrameRecordState::ExpectClear, FrameRecordState::ExpectClear};
  std::array<bool, kRenderTapeMaxReplayIntervals> intervalStarted{};
  std::uint32_t completedIntervals = 0u;
  bool anyTexturedDraw = false;
  BoundedDrawState drawState{};
  std::vector<RenderTapeDigest> referencedBlobs;
  const auto referenceBlob = [&](const RenderTapeDigest& digest) {
    if (std::none_of(referencedBlobs.begin(), referencedBlobs.end(),
                     [&](const auto& prior) {
                       return sameDigest(prior, digest);
                     })) {
      referencedBlobs.push_back(digest);
    }
  };
  std::uint32_t currentEventIndex = 0xffffffffu;
  for (std::uint32_t index = 0u; index < tape.events.size(); ++index) {
    currentEventIndex = index;
    const auto event = tape.event(index);
    switch (static_cast<RenderTapeEventType>(event.header.type)) {
    case RenderTapeEventType::BootstrapState: {
      RenderTapeBootstrapHeader fixed{};
      if (!load(event.payload, 0u, fixed) || fixed.overlayCount != 1u ||
          !parseChunks(event.payload.subspan(sizeof(fixed)), fixed.overlayCount,
                       candidate.bootstrap)) goto unsupported;
      result.coverage.bootstrapChunks = fixed.overlayCount;
      break;
    }
    case RenderTapeEventType::ObjectDefine: {
      RenderTapeObjectDefineHeader fixed{};
      if (completedIntervals != 0u || intervalStarted[0] ||
          !load(event.payload, 0u, fixed)) goto unsupported;
      const auto descriptor = event.payload.subspan(sizeof(fixed));
      const bool noImmutablePayload =
          fixed.payloadValidity == static_cast<std::uint32_t>(
              RenderTapeDigestValidity::NotCaptured) &&
          fixed.immutablePayloadBytes == 0u;
      if (fixed.identity.kind == D9C_CHUNK_HANDLE_KIND_SURFACE) {
        RenderTapeSurfaceDescriptorV2 surface{};
        if (!noImmutablePayload || descriptor.size() != sizeof(surface) ||
            !load(descriptor, 0u, surface) ||
            surface.schemaVersion != kRenderTapeSurfaceDescriptorVersion2 ||
            !acceptedSurface(surface.surface)) goto unsupported;
        if (surface.storage == static_cast<std::uint32_t>(
                RenderTapeSurfaceStorage::SwapchainBackbuffer)) {
          if (surface.initialContentDisposition != static_cast<std::uint32_t>(
                  RenderTapeInitialContentDisposition::ProducedPresentOutput) ||
              surface.subresource != 0u ||
              !renderTapeZeroIdentity(surface.parentTexture) ||
              fixed.expectedContentBytes != 0u ||
              fixed.expectedContentCount != 0u ||
              candidate.outputIdentity.objectId != 0u) goto unsupported;
          candidate.outputIdentity = fixed.identity;
          candidate.outputDesc = surface.surface;
        } else if (surface.storage == static_cast<std::uint32_t>(
                       RenderTapeSurfaceStorage::TextureSubresource)) {
          if (surface.initialContentDisposition != static_cast<std::uint32_t>(
                  RenderTapeInitialContentDisposition::Unavailable) ||
              surface.subresource != 0u || fixed.expectedContentBytes != 0u ||
              fixed.expectedContentCount != 0u ||
              surface.parentTexture.kind != D9C_CHUNK_HANDLE_KIND_TEXTURE ||
              surface.parentTexture.generation == 0u ||
              surface.parentTexture.objectId == 0u) goto unsupported;
        } else {
          goto unsupported;
        }
      } else if (fixed.identity.kind == D9C_CHUNK_HANDLE_KIND_BUFFER) {
        D9CBufferDesc buffer{};
        if (!noImmutablePayload || descriptor.size() != sizeof(buffer) ||
            !load(descriptor, 0u, buffer) ||
            buffer.size == 0u || fixed.expectedContentCount != 1u ||
            fixed.expectedContentBytes != buffer.size) goto unsupported;
      } else if (fixed.identity.kind == D9C_CHUNK_HANDLE_KIND_TEXTURE) {
        RenderTapeTextureDescriptorV2 texture{};
        D9CSurfaceDesc level0{};
        if (!noImmutablePayload || descriptor.size() < sizeof(texture) ||
            !load(descriptor, 0u, texture) ||
            !acceptedTextureDescriptor(descriptor, texture, level0) ||
            fixed.expectedContentCount !=
                (texture.initialContentDisposition == static_cast<std::uint32_t>(
                     RenderTapeInitialContentDisposition::ProducedByCapturedPass)
                     ? 0u
                     : texture.subresourceCount) ||
            (texture.initialContentDisposition == static_cast<std::uint32_t>(
                 RenderTapeInitialContentDisposition::ProducedByCapturedPass)
                 ? fixed.expectedContentBytes != 0u
                 : fixed.expectedContentBytes == 0u) ||
            candidate.textureIdentity.objectId != 0u) goto unsupported;
        candidate.textureIdentity = fixed.identity;
        candidate.textureDesc = texture;
        candidate.textureLevel0 = level0;
        candidate.textureProducedByCapturedPass =
            texture.initialContentDisposition == static_cast<std::uint32_t>(
                RenderTapeInitialContentDisposition::ProducedByCapturedPass);
      } else if (fixed.identity.kind == D9C_CHUNK_HANDLE_KIND_VERTEX_DECL) {
        const Definition definition{.fixed = fixed, .descriptor = descriptor};
        if (candidate.vertexDeclarationIdentity.objectId != 0u ||
            !acceptedProductionTexturedVertexDeclaration(definition, blobs)) {
          goto unsupported;
        }
        candidate.vertexDeclarationIdentity = fixed.identity;
        referenceBlob(fixed.immutablePayloadDigest);
      } else {
        goto unsupported;
      }
      candidate.definitions.push_back({.fixed = fixed, .descriptor = descriptor});
      ++result.coverage.objectDefinitions;
      break;
    }
    case RenderTapeEventType::ResourceMutation: {
      RenderTapeResourceMutationHeader fixed{};
      if (!load(event.payload, 0u, fixed) || fixed.byteOffset != 0u ||
          (fixed.kind != static_cast<std::uint32_t>(RenderTapeMutationKind::Upload) &&
           fixed.kind != static_cast<std::uint32_t>(RenderTapeMutationKind::CpuUnlock)) ||
          !findBlob(blobs, fixed.digest)) goto unsupported;
      if (completedIntervals == 0u && !intervalStarted[0]) {
        candidate.initialMutations.push_back(fixed);
      } else if (tape.header.profile == kRenderTapeProfileSequence &&
                 completedIntervals == 1u && !intervalStarted[1]) {
        candidate.intervals[1].mutationsBefore.push_back(fixed);
      } else {
        goto unsupported;
      }
      ++result.coverage.seedMutations;
      referenceBlob(fixed.digest);
      break;
    }
    case RenderTapeEventType::CommandChunk: {
      RenderTapeCommandChunkHeader fixed{};
      if (completedIntervals >= candidate.intervalCount ||
          completedIntervals >= kRenderTapeMaxReplayIntervals ||
          (completedIntervals == 1u &&
           candidate.intervals[1].mutationsBefore.size() != 1u) ||
          !load(event.payload, 0u, fixed)) goto unsupported;
      const auto intervalIndex = completedIntervals;
      intervalStarted[intervalIndex] = true;
      candidate.intervals[intervalIndex].frame.push_back(Chunk{
          .envelope = CommandChunkEnvelope{fixed.wireVersion, fixed.recordCount,
                                            fixed.handleCount},
          .bytes = event.payload.subspan(sizeof(fixed)),
      });
      const auto& frame = candidate.intervals[intervalIndex].frame.back();
      ImportedChunkView chunk;
      if (!importPrevalidatedCommandChunk(frame.bytes, frame.envelope, chunk) ||
          chunk.records.empty()) goto unsupported;
      for (std::size_t recordIndex = 0u; recordIndex < chunk.records.size();
           ++recordIndex) {
        const auto record = chunk.record(recordIndex);
        ++result.coverage.commandRecords;
        auto& recordState = recordStates[intervalIndex];
        if (recordState == FrameRecordState::ExpectClear &&
            candidate.textureProducedByCapturedPass &&
            record.header.type == D9C_COMMAND_RECORD_APPLY_STATE) {
          if (!applyBoundedBindings(chunk, record, drawState)) goto unsupported;
        } else if (recordState == FrameRecordState::ExpectClear &&
            record.header.type == D9C_COMMAND_RECORD_CLEAR) {
          D9CCommandChunkWireClear clear{};
          if (!load(record.payload, 0u, clear) || clear.flags != 1u ||
              clear.rectCount != 0u) goto unsupported;
          ++result.coverage.clearRecords;
          recordState = FrameRecordState::ExpectDrawOrPresent;
        } else if (recordState == FrameRecordState::ExpectDrawOrPresent &&
                   record.header.type ==
                       D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP) {
          candidate.intervals[intervalIndex].sawTexturedDraw = true;
          ++result.coverage.drawPrimitiveUpRecords;
          recordState = FrameRecordState::ExpectPresent;
        } else if ((recordState == FrameRecordState::ExpectDrawOrPresent ||
                    recordState == FrameRecordState::ExpectPresent) &&
                   record.header.type == D9C_COMMAND_RECORD_PRESENT) {
          D9CCommandChunkWirePresent present{};
          if (!load(record.payload, 0u, present) || present.flags != 0u ||
              present.reserved0 != 0u || present.hasSrc != present.hasDst ||
              (present.hasSrc != 0u &&
               (!fullRect(present.src, candidate.outputDesc.width,
                          candidate.outputDesc.height) ||
                !fullRect(present.dst, candidate.outputDesc.width,
                          candidate.outputDesc.height)))) goto unsupported;
          ++result.coverage.presentRecords;
          recordState = FrameRecordState::Complete;
        } else {
          goto unsupported;
        }
      }
      ++result.coverage.commandChunks;
      break;
    }
    case RenderTapeEventType::PresentComplete: {
      RenderTapePresentCompleteHeader fixed{};
      RenderTapeOracleAttachment oracle{};
      if (completedIntervals >= candidate.intervalCount ||
          !intervalStarted[completedIntervals] ||
          recordStates[completedIntervals] != FrameRecordState::Complete ||
          !load(event.payload, 0u, fixed) || fixed.oracleCount != 1u ||
          !load(event.payload, sizeof(fixed), oracle) ||
          !sameIdentity(oracle.identity, candidate.outputIdentity)) goto unsupported;
      auto& interval = candidate.intervals[completedIntervals];
      interval.presentOrdinal = fixed.presentOrdinal;
      interval.completionOrdinal = fixed.completionOrdinal;
      interval.expectedDigestValidity =
          static_cast<RenderTapeDigestValidity>(fixed.digestValidity);
      interval.expectedDigest = fixed.expectedDigest;
      result.intervals[completedIntervals].presentOrdinal = fixed.presentOrdinal;
      result.intervals[completedIntervals].completionOrdinal =
          fixed.completionOrdinal;
      result.intervals[completedIntervals].validity.expectedDigestCaptured =
          interval.expectedDigestValidity == RenderTapeDigestValidity::Sha256;
      ++completedIntervals;
      ++result.coverage.presentOutputs;
      break;
    }
    case RenderTapeEventType::ObjectDestroy:
    case RenderTapeEventType::OrderedControl:
      goto unsupported;
    }
  }
  bootstrapDisposition = candidate.bootstrap.size() == 1u
      ? classifyFrameTapeBootstrapOutput(
            candidate.bootstrap.front().bytes,
            candidate.bootstrap.front().envelope,
            candidate.outputIdentity)
      : FrameTapeBootstrapOutputDisposition::Malformed;
  bootstrapAccepted =
      bootstrapDisposition == FrameTapeBootstrapOutputDisposition::ImplicitDefault ||
      bootstrapDisposition == FrameTapeBootstrapOutputDisposition::ExplicitExact;
  for (const auto& chunkBytes : candidate.bootstrap) {
    ImportedChunkView chunk;
    if (!importPrevalidatedCommandChunk(chunkBytes.bytes, chunkBytes.envelope,
                                        chunk)) goto unsupported;
    for (std::size_t recordIndex = 0u; recordIndex < chunk.records.size();
         ++recordIndex) {
      if (!applyBoundedBindings(chunk, chunk.record(recordIndex), drawState))
        goto unsupported;
    }
  }
  // Apply the effective draw state in exact interval order. Mutation events do
  // not alter bindings, so the state shadow intentionally survives Present 1.
  drawState = {};
  for (const auto& chunkBytes : candidate.bootstrap) {
    ImportedChunkView chunk;
    importPrevalidatedCommandChunk(chunkBytes.bytes, chunkBytes.envelope, chunk);
    for (std::size_t recordIndex = 0u; recordIndex < chunk.records.size();
         ++recordIndex) {
      if (!applyBoundedBindings(chunk, chunk.record(recordIndex), drawState))
        goto unsupported;
    }
  }
  for (std::uint32_t intervalIndex = 0u;
       intervalIndex < candidate.intervalCount; ++intervalIndex) {
    for (const auto& chunkBytes : candidate.intervals[intervalIndex].frame) {
      ImportedChunkView chunk;
      importPrevalidatedCommandChunk(chunkBytes.bytes, chunkBytes.envelope, chunk);
      for (std::size_t recordIndex = 0u; recordIndex < chunk.records.size();
           ++recordIndex) {
        const auto record = chunk.record(recordIndex);
        if (record.header.type == D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP) {
          if (!acceptedTexturedDraw(chunk, record, drawState,
                                    candidate.vertexDeclarationIdentity)) {
            goto unsupported;
          }
        } else if (record.sparseState() &&
                   !applyBoundedBindings(chunk, record, drawState)) {
          goto unsupported;
        }
      }
    }
  }
  if (completedIntervals != candidate.intervalCount ||
      candidate.outputIdentity.objectId == 0u ||
      !bootstrapAccepted ||
      referencedBlobs.size() != blobs.size() ||
      !mutationCatalogueMatches(candidate, blobs)) goto unsupported;
  anyTexturedDraw = std::any_of(
      candidate.intervals.begin(),
      candidate.intervals.begin() + candidate.intervalCount,
      [](const auto& interval) { return interval.sawTexturedDraw; });
  if (!anyTexturedDraw && !candidate.textureProducedByCapturedPass &&
      (candidate.textureIdentity.objectId != 0u ||
       candidate.vertexDeclarationIdentity.objectId != 0u))
    goto unsupported;
  if (tape.header.profile == kRenderTapeProfileSequence && !anyTexturedDraw)
    goto unsupported;
  if (anyTexturedDraw || candidate.textureProducedByCapturedPass) {
    std::uint64_t texturePixels = 0u;
    std::uint64_t tightTextureBytes = 0u;
    if (!checkedMul(candidate.textureLevel0.width,
                    candidate.textureLevel0.height, texturePixels) ||
        !checkedMul(texturePixels, 4u, tightTextureBytes)) goto unsupported;
    const auto textureDefinition = std::find_if(
        candidate.definitions.begin(), candidate.definitions.end(),
        [&](const auto& definition) {
          return sameIdentity(definition.fixed.identity,
                              candidate.textureIdentity);
        });
    const bool productionDeclaration =
        candidate.vertexDeclarationIdentity.objectId != 0u;
    const std::size_t expectedDefinitions = productionDeclaration ? 3u : 2u;
    const std::size_t expectedSurfaceAliases = std::count_if(
        candidate.definitions.begin(), candidate.definitions.end(),
        [&](const auto& definition) {
          if (definition.fixed.identity.kind !=
              D9C_CHUNK_HANDLE_KIND_SURFACE)
            return false;
          RenderTapeSurfaceDescriptorV2 surface{};
          return load(definition.descriptor, 0u, surface) &&
                 surface.storage == static_cast<std::uint32_t>(
                     RenderTapeSurfaceStorage::TextureSubresource);
        });
    const bool aliasesNameCandidateTexture = std::all_of(
        candidate.definitions.begin(), candidate.definitions.end(),
        [&](const auto& definition) {
          if (definition.fixed.identity.kind !=
              D9C_CHUNK_HANDLE_KIND_SURFACE)
            return true;
          RenderTapeSurfaceDescriptorV2 surface{};
          if (!load(definition.descriptor, 0u, surface) ||
              surface.storage != static_cast<std::uint32_t>(
                  RenderTapeSurfaceStorage::TextureSubresource))
            return true;
          return sameIdentity(surface.parentTexture,
                              candidate.textureIdentity);
        });
    const bool sequence = tape.header.profile == kRenderTapeProfileSequence;
    const std::size_t expectedBlobs =
        (productionDeclaration ? 2u : 1u) + (sequence ? 1u : 0u);
    const auto mutationShapeMatches = [&](const auto& mutation) {
      return sameIdentity(mutation.identity, candidate.textureIdentity) &&
             mutation.subresource == 0u && mutation.byteOffset == 0u &&
             mutation.byteSize == textureDefinition->fixed.expectedContentBytes;
    };
    if (candidate.definitions.size() !=
            expectedDefinitions + expectedSurfaceAliases ||
        candidate.textureIdentity.objectId == 0u ||
        !aliasesNameCandidateTexture ||
        textureDefinition == candidate.definitions.end() ||
        (!candidate.textureProducedByCapturedPass &&
         !sameIdentity(drawState.texture, candidate.textureIdentity)) ||
        candidate.textureDesc.mipLevelCount != 1u ||
        candidate.textureLevel0.format != 21u ||
        (!candidate.textureProducedByCapturedPass &&
         (textureDefinition->fixed.expectedContentBytes < tightTextureBytes ||
          textureDefinition->fixed.expectedContentBytes %
                  candidate.textureLevel0.height != 0u ||
          candidate.initialMutations.size() != 1u ||
          !mutationShapeMatches(candidate.initialMutations[0]) ||
          blobs.size() != expectedBlobs ||
          textureDefinition->fixed.expectedContentCount != 1u)) ||
        (candidate.textureProducedByCapturedPass &&
         (!candidate.initialMutations.empty() ||
          blobs.size() != (productionDeclaration ? 1u : 0u) +
                              (sequence ? 1u : 0u) ||
          textureDefinition->fixed.expectedContentCount != 0u))) {
      goto unsupported;
    }
    if (sequence) {
      if (!candidate.intervals[0].sawTexturedDraw ||
          !candidate.intervals[1].sawTexturedDraw ||
          !candidate.intervals[0].mutationsBefore.empty() ||
          candidate.intervals[1].mutationsBefore.size() != 1u ||
          !mutationShapeMatches(candidate.intervals[1].mutationsBefore[0]) ||
          sameDigest(candidate.initialMutations[0].digest,
                     candidate.intervals[1].mutationsBefore[0].digest) ||
          candidate.intervals[0].expectedDigestValidity !=
              RenderTapeDigestValidity::Sha256 ||
          candidate.intervals[1].expectedDigestValidity !=
              RenderTapeDigestValidity::Sha256 ||
          sameDigest(candidate.intervals[0].expectedDigest,
                     candidate.intervals[1].expectedDigest) ||
          candidate.intervals[0].completionOrdinal >=
              candidate.intervals[1].completionOrdinal) {
        goto unsupported;
      }
    }
  }
  result.conservation.referencedBlobs =
      static_cast<std::uint32_t>(referencedBlobs.size());
  result.requirements = FrameTapeReplayRequirements{
      .outputWidth = candidate.outputDesc.width,
      .outputHeight = candidate.outputDesc.height,
      .outputFormat = candidate.outputDesc.format,
  };
  result.conservation.presentOrdinal =
      candidate.intervals[candidate.intervalCount - 1u].presentOrdinal;
  result.conservation.completionOrdinal =
      candidate.intervals[candidate.intervalCount - 1u].completionOrdinal;
  result.validity.expectedDigestCaptured = std::all_of(
      candidate.intervals.begin(),
      candidate.intervals.begin() + candidate.intervalCount,
      [](const auto& interval) {
        return interval.expectedDigestValidity == RenderTapeDigestValidity::Sha256;
      });
  result.status = FrameTapeReplayStatus::Complete;
  if (plan) *plan = std::move(candidate);
  return result;

unsupported:
  result.status = FrameTapeReplayStatus::UnsupportedGrammar;
  result.failedEventIndex = currentEventIndex;
  return result;
}

struct OwnedObject {
  D9CWireObjectIdentity identity{};
  void* value = nullptr;
};

void releaseObject(OwnedObject& object) {
  switch (object.identity.kind) {
  case D9C_CHUNK_HANDLE_KIND_TEXTURE:
    dxmt9c_texture_release(static_cast<D9CTexture*>(object.value)); break;
  case D9C_CHUNK_HANDLE_KIND_SURFACE:
    dxmt9c_surface_release(static_cast<D9CSurface*>(object.value)); break;
  case D9C_CHUNK_HANDLE_KIND_BUFFER:
    dxmt9c_buffer_release(static_cast<D9CBuffer*>(object.value)); break;
  case D9C_CHUNK_HANDLE_KIND_VERTEX_DECL:
    dxmt9c_vdecl_release(static_cast<D9CVertexDecl*>(object.value)); break;
  default: break;
  }
  object.value = nullptr;
}

void* findObject(std::vector<OwnedObject>& objects,
                 const D9CWireObjectIdentity& identity) {
  const auto it = std::find_if(objects.begin(), objects.end(), [&](const auto& object) {
    return sameIdentity(object.identity, identity);
  });
  return it == objects.end() ? nullptr : it->value;
}

bool resolveChunk(const Chunk& chunk, std::vector<OwnedObject>& objects,
                  std::vector<void*>& resolved) {
  ImportedChunkView imported;
  if (!importPrevalidatedCommandChunk(chunk.bytes, chunk.envelope, imported)) return false;
  resolved.clear();
  for (const auto& handle : imported.handles) {
    void* object = findObject(objects, D9CWireObjectIdentity{
        .kind = handle.kind, .generation = handle.generation,
        .objectId = handle.objectId});
    if (!object) return false;
    resolved.push_back(object);
  }
  return true;
}

} // namespace

FrameTapeReplayResult preflightRenderTapeIdentity(
    std::span<const std::byte> tape,
    std::span<const RenderTapeProviderBlob> blobs) noexcept {
  return buildPlan(tape, blobs, nullptr);
}

FrameTapeReplayResult preflightFrameTapeIdentity(
    std::span<const std::byte> tape,
    std::span<const RenderTapeProviderBlob> blobs) noexcept {
  auto result = preflightRenderTapeIdentity(tape, blobs);
  if (result.complete() && result.profile != kRenderTapeProfileFrame) {
    result.status = FrameTapeReplayStatus::UnsupportedGrammar;
  }
  return result;
}

FrameTapeReplayResult replayRenderTapeIdentity(
    D9CDevice* device, std::span<const std::byte> tape,
    std::span<const RenderTapeProviderBlob> blobs) noexcept {
  PreflightPlan plan;
  auto result = buildPlan(tape, blobs, &plan);
  if (!result.complete()) return result;
  if (!device) {
    result.status = FrameTapeReplayStatus::ObjectCreationFailed;
    return result;
  }

  std::vector<OwnedObject> objects;
  std::shared_ptr<OffscreenPresentOutput> output;
  D9CSurface* readbackTarget = nullptr;
  bool outputInstalled = false;
  const auto cleanup = [&] {
    if (auto upper = device->dev().upperDevice()) upper->flush();
    if (outputInstalled) {
      if (auto* swap = device->iface->GetSwapChain(0u)) {
        swap->coreSwapChain().restoreWindowPresenter();
        swap->Release();
      }
      outputInstalled = false;
    }
    if (readbackTarget) dxmt9c_surface_release(readbackTarget);
    for (auto& object : objects) {
      if (object.value) {
        releaseObject(object);
        ++result.conservation.objectsReleased;
      }
    }
  };

  // Texture-derived surfaces borrow their parent storage. Create all owning
  // objects first, then materialize aliases so replay is independent of
  // ObjectDefine event order while preserving every identity exactly.
  for (unsigned creationPass = 0u; creationPass != 2u; ++creationPass) {
    for (const auto& definition : plan.definitions) {
      bool textureAlias = false;
      if (definition.fixed.identity.kind == D9C_CHUNK_HANDLE_KIND_SURFACE) {
        RenderTapeSurfaceDescriptorV2 surface{};
        textureAlias = load(definition.descriptor, 0u, surface) &&
                       surface.storage == static_cast<std::uint32_t>(
                           RenderTapeSurfaceStorage::TextureSubresource);
      }
      if ((creationPass == 1u) != textureAlias)
        continue;
    void* value = nullptr;
    if (definition.fixed.identity.kind == D9C_CHUNK_HANDLE_KIND_SURFACE) {
      RenderTapeSurfaceDescriptorV2 surface{};
      if (!load(definition.descriptor, 0u, surface)) {
        result.status = FrameTapeReplayStatus::ObjectCreationFailed;
        cleanup();
        return result;
      }
      if (surface.storage == static_cast<std::uint32_t>(
              RenderTapeSurfaceStorage::TextureSubresource)) {
        void* parent = findObject(objects, surface.parentTexture);
        if (parent) {
          value = dxmt9c_texture_get_surface_level(
              static_cast<D9CTexture*>(parent), surface.subresource);
        }
      } else {
        auto* swap = device->iface->GetSwapChain(0u);
        if (swap) {
          value = new D9CSurface{swap->backBuffer(), nullptr, 0u, device};
          swap->Release();
        }
      }
    } else if (definition.fixed.identity.kind == D9C_CHUNK_HANDLE_KIND_BUFFER) {
      D9CBufferDesc desc{};
      load(definition.descriptor, 0u, desc);
      value = desc.format
          ? static_cast<void*>(dxmt9c_device_create_index_buffer(
                device, desc.size, desc.usage, desc.format, desc.pool))
          : static_cast<void*>(dxmt9c_device_create_vertex_buffer(
                device, desc.size, desc.usage, desc.fvf, desc.pool));
    } else if (definition.fixed.identity.kind == D9C_CHUNK_HANDLE_KIND_TEXTURE) {
      RenderTapeTextureDescriptorV2 desc{};
      D9CSurfaceDesc level0{};
      load(definition.descriptor, 0u, desc);
      load(definition.descriptor, sizeof(desc), level0);
      value = dxmt9c_device_create_texture(
          device, level0.width, level0.height, desc.mipLevelCount,
          level0.usage, level0.format, level0.pool);
    } else if (definition.fixed.identity.kind ==
               D9C_CHUNK_HANDLE_KIND_VERTEX_DECL) {
      const auto* blob = findBlob(blobs, definition.fixed.immutablePayloadDigest);
      std::array<D9CVertexElement, 4u> elements{};
      if (blob && blob->bytes.size() == sizeof(elements)) {
        std::memcpy(elements.data(), blob->bytes.data(), sizeof(elements));
        value = dxmt9c_device_create_vertex_declaration(device,
                                                         elements.data());
      }
    }
    if (!value) {
      result.status = FrameTapeReplayStatus::ObjectCreationFailed;
      cleanup();
      return result;
    }
    objects.push_back({.identity = definition.fixed.identity, .value = value});
    ++result.conservation.objectsCreated;
    }
  }

  // Definitions are fully indexed before bootstrap application.
  std::vector<void*> resolved;
  for (const auto& chunk : plan.bootstrap) {
    if (!resolveChunk(chunk, objects, resolved) ||
        replayPrevalidatedResolvedCommandChunk(
            device, chunk.bytes, chunk.envelope, resolved) != core::D3D_OK) {
      result.status = FrameTapeReplayStatus::BootstrapFailed;
      cleanup();
      return result;
    }
  }

  const auto applyMutations = [&](std::span<const RenderTapeResourceMutationHeader>
                                      mutations) {
    for (const auto& mutation : mutations) {
      const auto* blob = findBlob(blobs, mutation.digest);
      void* object = findObject(objects, mutation.identity);
      bool ok = blob && object;
      if (ok && mutation.identity.kind == D9C_CHUNK_HANDLE_KIND_BUFFER) {
        void* mapped = nullptr;
        auto* buffer = static_cast<D9CBuffer*>(object);
        ok = mutation.subresource == 0u &&
             dxmt9c_buffer_lock(
                 buffer, 0u, static_cast<std::uint32_t>(blob->bytes.size()),
                 &mapped, 0u) == core::D3D_OK &&
             mapped;
        if (ok) {
          std::memcpy(mapped, blob->bytes.data(), blob->bytes.size());
          ok = dxmt9c_buffer_unlock(buffer) == core::D3D_OK;
        }
      } else if (ok &&
                 mutation.identity.kind == D9C_CHUNK_HANDLE_KIND_TEXTURE) {
        D9CSurfaceDesc levelDesc{};
        D9CLockedRect locked{};
        auto* texture = static_cast<D9CTexture*>(object);
        ok = dxmt9c_texture_get_level_desc(texture, mutation.subresource,
                                           &levelDesc) == core::D3D_OK &&
             dxmt9c_texture_lock_rect(texture, mutation.subresource, &locked,
                                      nullptr, 0u) == core::D3D_OK &&
             locked.bits;
        RenderTapeLinearLockLayout layout{};
        if (ok &&
            (renderTapeLinearLockLayout(levelDesc, locked.pitch, nullptr,
                                        layout) !=
                 RenderTapeLinearLayoutStatus::Accepted ||
             blob->bytes.size() != layout.tightBytes ||
             !writeRenderTapeLinearRows(blob->bytes, locked.bits, layout))) {
          (void)dxmt9c_texture_unlock_rect(texture, mutation.subresource);
          ok = false;
        } else if (ok) {
          ok = dxmt9c_texture_unlock_rect(texture, mutation.subresource) ==
               core::D3D_OK;
        }
      } else {
        ok = false;
      }
      if (!ok) {
        return false;
      }
    }
    return true;
  };

  if (!applyMutations(plan.initialMutations)) {
    result.status = FrameTapeReplayStatus::MutationFailed;
    cleanup();
    return result;
  }

  if (auto upper = device->dev().upperDevice(); upper && upper->pool()) {
    readbackTarget = dxmt9c_device_create_render_target(
        device, plan.outputDesc.width, plan.outputDesc.height,
        plan.outputDesc.format, 0u, 0u, 0u, nullptr);
    auto* record = readbackTarget
        ? upper->pool()->findSurface(readbackTarget->obj->handle().value)
        : nullptr;
    auto* swap = device->iface->GetSwapChain(0u);
    if (!record || !record->texture || !swap) {
      if (swap) swap->Release();
      result.status = FrameTapeReplayStatus::PresentOutputFailed;
      cleanup();
      return result;
    }
    output = std::make_shared<OffscreenPresentOutput>(
        WMT::Texture{record->texture.handle}, plan.outputDesc.width,
        plan.outputDesc.height);
    const bool installed = swap->coreSwapChain().installPresentOutput(output);
    swap->Release();
    if (!installed) {
      result.status = FrameTapeReplayStatus::PresentOutputFailed;
      cleanup();
      return result;
    }
    outputInstalled = true;
  }

  const auto readbackInterval = [&](std::uint32_t intervalIndex) {
    auto& evidence = result.intervals[intervalIndex].validity;
    const auto& interval = plan.intervals[intervalIndex];
    auto upper = device->dev().upperDevice();
    if (!upper || !readbackTarget) {
      return true;
    }
    upper->flush();
    core::ReadbackPixels pixels;
    if (!upper->readbackSurface(
            core::ReadbackDesc{.source = readbackTarget->obj->handle()}, pixels)) {
      return false;
    }
    evidence.outputReadback = true;
    constexpr std::uint32_t bytesPerPixel = 4u;
    std::uint64_t tightPitch = 0u;
    std::uint64_t tightBytes = 0u;
    std::uint64_t pitchedBytes = 0u;
    if (!checkedMul(plan.outputDesc.width, bytesPerPixel, tightPitch) ||
        !checkedMul(tightPitch, plan.outputDesc.height, tightBytes) ||
        !checkedMul(pixels.pitch, plan.outputDesc.height, pitchedBytes) ||
        pixels.pitch < tightPitch ||
        tightBytes > std::numeric_limits<std::size_t>::max() ||
        pixels.bytes.size() < pitchedBytes) {
      return false;
    }
    std::vector<std::byte> tight(static_cast<std::size_t>(tightBytes));
    for (std::uint32_t row = 0u; row < plan.outputDesc.height; ++row) {
      std::memcpy(tight.data() + static_cast<std::size_t>(row * tightPitch),
                  pixels.bytes.data() + static_cast<std::size_t>(row) * pixels.pitch,
                  static_cast<std::size_t>(tightPitch));
    }
    evidence.outputBytes = tight.size();
    evidence.outputDigest = RenderTapeCaptureSession::sha256(tight);
    evidence.expectedDigestCaptured =
        interval.expectedDigestValidity == RenderTapeDigestValidity::Sha256;
    if (evidence.expectedDigestCaptured) {
      evidence.expectedDigestMatched =
          sameDigest(evidence.outputDigest, interval.expectedDigest);
    }
    const auto pixelSize = std::min<std::size_t>(4u, tight.size());
    evidence.outputNonDegenerate = false;
    for (std::size_t index = pixelSize; index < tight.size(); ++index) {
      if (tight[index] != tight[index % pixelSize]) {
        evidence.outputNonDegenerate = true;
        break;
      }
    }
    return true;
  };

  for (std::uint32_t intervalIndex = 0u;
       intervalIndex < plan.intervalCount; ++intervalIndex) {
    if (!applyMutations(plan.intervals[intervalIndex].mutationsBefore)) {
      result.status = FrameTapeReplayStatus::MutationFailed;
      cleanup();
      return result;
    }
    for (const auto& chunk : plan.intervals[intervalIndex].frame) {
      if (!resolveChunk(chunk, objects, resolved) ||
          replayPrevalidatedResolvedCommandChunk(
              device, chunk.bytes, chunk.envelope, resolved) != core::D3D_OK) {
        result.status = FrameTapeReplayStatus::CommandReplayFailed;
        cleanup();
        return result;
      }
    }
    if (!readbackInterval(intervalIndex)) {
      result.status = FrameTapeReplayStatus::ReadbackFailed;
      cleanup();
      return result;
    }
    if (result.intervals[intervalIndex].validity.expectedDigestCaptured &&
        !result.intervals[intervalIndex].validity.expectedDigestMatched) {
      result.validity = result.intervals[intervalIndex].validity;
      result.validity.structurallyValid = true;
      result.validity.digestsValid = true;
      result.status = FrameTapeReplayStatus::OutputMismatch;
      cleanup();
      return result;
    }
  }
  if (output && output->scheduledCount() != plan.intervalCount) {
    result.status = FrameTapeReplayStatus::PresentOutputFailed;
  }
  if (plan.intervalCount != 0u) {
    result.validity = result.intervals[plan.intervalCount - 1u].validity;
    result.validity.structurallyValid = true;
    result.validity.digestsValid = true;
    result.validity.outputReadback = std::all_of(
        result.intervals.begin(), result.intervals.begin() + plan.intervalCount,
        [](const auto& interval) { return interval.validity.outputReadback; });
    result.validity.expectedDigestCaptured = std::all_of(
        result.intervals.begin(), result.intervals.begin() + plan.intervalCount,
        [](const auto& interval) {
          return interval.validity.expectedDigestCaptured;
        });
    result.validity.expectedDigestMatched = std::all_of(
        result.intervals.begin(), result.intervals.begin() + plan.intervalCount,
        [](const auto& interval) {
          return interval.validity.expectedDigestMatched;
        });
    result.validity.outputNonDegenerate = std::all_of(
        result.intervals.begin(), result.intervals.begin() + plan.intervalCount,
        [](const auto& interval) {
          return interval.validity.outputNonDegenerate;
        });
  }
  cleanup();
  return result;
}

FrameTapeReplayResult replayFrameTapeIdentity(
    D9CDevice* device, std::span<const std::byte> tape,
    std::span<const RenderTapeProviderBlob> blobs) noexcept {
  const auto preflight = preflightFrameTapeIdentity(tape, blobs);
  if (!preflight.complete()) return preflight;
  return replayRenderTapeIdentity(device, tape, blobs);
}

FrameTapeBootstrapOutputDisposition classifyFrameTapeBootstrapOutput(
    std::span<const std::byte> bytes, const CommandChunkEnvelope& envelope,
    const D9CWireObjectIdentity& output) noexcept {
  ImportedChunkView imported;
  if (!importPrevalidatedCommandChunk(bytes, envelope, imported) ||
      imported.records.size() != 1u ||
      imported.record(0u).header.type != D9C_COMMAND_RECORD_APPLY_STATE) {
    return FrameTapeBootstrapOutputDisposition::Malformed;
  }

  bool sawRenderTargetSection = false;
  bool sawSlotZero = false;
  for (std::size_t index = 0u; index < imported.record(0u).sections.size();
       ++index) {
    const auto section = imported.record(0u).section(index);
    if (section.descriptor.kind != D9C_COMMAND_CHUNK_SECTION_RENDER_TARGET) {
      continue;
    }
    sawRenderTargetSection = true;
    if (section.descriptor.count == 0u) {
      if (section.descriptor.byteSize != 0u) {
        return FrameTapeBootstrapOutputDisposition::Malformed;
      }
      continue;
    }
    for (std::uint32_t bindingIndex = 0u;
         bindingIndex < section.descriptor.count; ++bindingIndex) {
      D9CCommandChunkWireRenderTargetBinding binding{};
      if (!load(section.payload, bindingIndex * sizeof(binding), binding)) {
        return FrameTapeBootstrapOutputDisposition::Malformed;
      }
      if (binding.slot != 0u) {
        return FrameTapeBootstrapOutputDisposition::SlotOutOfRange;
      }
      if (sawSlotZero) {
        return FrameTapeBootstrapOutputDisposition::Ambiguous;
      }
      sawSlotZero = true;
      if (binding.valid != 1u ||
          binding.handleIndex == D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX) {
        return FrameTapeBootstrapOutputDisposition::ExplicitNull;
      }
      if (binding.handleIndex >= imported.handles.size()) {
        return FrameTapeBootstrapOutputDisposition::Malformed;
      }
      const auto& handle = imported.handles[binding.handleIndex];
      if (!sameIdentity(output, D9CWireObjectIdentity{
                                 .kind = handle.kind,
                                 .generation = handle.generation,
                                 .objectId = handle.objectId,
                             })) {
        return FrameTapeBootstrapOutputDisposition::WrongIdentity;
      }
    }
  }
  if (!sawRenderTargetSection || !sawSlotZero) {
    return FrameTapeBootstrapOutputDisposition::ImplicitDefault;
  }
  return FrameTapeBootstrapOutputDisposition::ExplicitExact;
}

const char* frameTapeReplayStatusName(FrameTapeReplayStatus status) noexcept {
  switch (status) {
  case FrameTapeReplayStatus::Complete: return "complete";
  case FrameTapeReplayStatus::InvalidTape: return "invalid-tape";
  case FrameTapeReplayStatus::InvalidBlobCatalogue: return "invalid-blob-catalogue";
  case FrameTapeReplayStatus::UnsupportedGrammar: return "unsupported-grammar";
  case FrameTapeReplayStatus::ObjectCreationFailed: return "object-creation-failed";
  case FrameTapeReplayStatus::MutationFailed: return "mutation-failed";
  case FrameTapeReplayStatus::BootstrapFailed: return "bootstrap-failed";
  case FrameTapeReplayStatus::CommandReplayFailed: return "command-replay-failed";
  case FrameTapeReplayStatus::PresentOutputFailed: return "present-output-failed";
  case FrameTapeReplayStatus::ReadbackFailed: return "readback-failed";
  case FrameTapeReplayStatus::OutputMismatch: return "output-mismatch";
  }
  return "unknown";
}

} // namespace dxmt9::d3d9
