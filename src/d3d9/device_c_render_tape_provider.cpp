#include "device_c_render_tape_provider.hpp"

#include "device_c_chunk_replay.hpp"
#include "device_c_common.hpp"
#include "device_c_render_tape_capture.hpp"
#include "device_c_render_tape_capture_layout.hpp"
#include "dxmt9/dxmt9_format_convert.hpp"
#include "dxmt9/dxmt9_device.hpp"
#include "dxmt9/dxmt9_presenter.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
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

bool containsIdentity(std::span<const D9CWireObjectIdentity> identities,
                      const D9CWireObjectIdentity& identity) noexcept {
  return std::any_of(identities.begin(), identities.end(),
                     [&](const auto& value) { return sameIdentity(value, identity); });
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

bool checkedAdd(std::uint64_t a, std::uint64_t b,
                std::uint64_t& out) noexcept {
  if (a > std::numeric_limits<std::uint64_t>::max() - b) return false;
  out = a + b;
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

bool presentGeometrySupported(const D9CCommandChunkWirePresent& present,
                              const D9CSurfaceDesc& output) noexcept {
  return present.flags == 0u && present.hasSrc == present.hasDst &&
      (present.hasSrc == 0u ||
       (fullRect(present.src, output.width, output.height) &&
        fullRect(present.dst, output.width, output.height)));
}

bool applyGammaRamp(D9CDevice* device,
                    std::span<const std::byte> bytes) noexcept {
  if (!device || bytes.size() != kRenderTapeGammaRampBytes) return false;
  std::array<std::uint16_t, kRenderTapeGammaRampBytes / sizeof(std::uint16_t)>
      gamma{};
  std::memcpy(gamma.data(), bytes.data(), bytes.size());
  dxmt9c_device_set_gamma_ramp(device, gamma.data());
  return true;
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
  std::vector<D9CWireObjectIdentity> seededArmColorTextures{};
  std::vector<std::pair<D9CWireObjectIdentity, D9CSurfaceDesc>>
      producedStandaloneSurfaces{};
  std::vector<std::pair<D9CWireObjectIdentity, D9CSurfaceDesc>>
      seededStandaloneDepthSurfaces{};
  std::vector<std::pair<D9CWireObjectIdentity, D9CSurfaceDesc>>
      seededStandaloneColorSurfaces{};
  std::vector<D9CWireObjectIdentity> drawTextureIdentities{};
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
                 std::vector<Chunk>& out, std::size_t trailingBytes = 0u) {
  if (trailingBytes > bytes.size()) return false;
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
  return offset == bytes.size() - trailingBytes;
}

bool acceptedOutputSurface(const D9CSurfaceDesc& desc) {
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
  const auto dimension =
      static_cast<RenderTapeTextureDimension>(texture.dimension);
  const bool texture2d = dimension == RenderTapeTextureDimension::Texture2D;
  const bool cube = dimension == RenderTapeTextureDimension::Cube;
  const bool completeSeed =
      texture.initialContentDisposition == static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::CompleteSeed);
  const bool produced =
      texture.initialContentDisposition == static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::ProducedByCapturedPass);
  const bool armSnapshotCompleteSeed =
      completeSeed && renderTapeArmColorSnapshotTextureSupported(descriptor);
  const bool acceptedShape =
      (completeSeed &&
       ((texture2d && texture.mipLevelCount != 0u &&
         texture.subresourceCount == texture.mipLevelCount) ||
        armSnapshotCompleteSeed)) ||
      (produced && renderTapeProducedTextureShapeSupported(texture));
  if (!renderTapeLoadTextureDescriptorV2(descriptor, canonical) ||
      canonical.dimension != texture.dimension ||
      canonical.mipLevelCount != texture.mipLevelCount ||
      canonical.subresourceCount != texture.subresourceCount ||
      canonical.initialContentDisposition !=
          texture.initialContentDisposition ||
      !acceptedShape ||
      texture.reserved0 != 0u ||
      descriptor.size() !=
          sizeof(texture) +
              static_cast<std::size_t>(texture.subresourceCount) *
                  sizeof(D9CSurfaceDesc) ||
      !load(descriptor, sizeof(texture), level0) ||
      level0.resourceType != (cube ? 5u : 3u) || level0.width == 0u ||
      level0.height == 0u || level0.depth != 1u || level0.pool != 0u ||
      level0.multiSampleType != 0u || level0.multiSampleQuality != 0u ||
      ((completeSeed && !armSnapshotCompleteSeed &&
        (level0.usage & 3u) != 0u) ||
       (produced && (level0.usage & 1u) == 0u))) return false;
  const auto format = devicec::fmtFromD3D(level0.format);
  if (format == core::Format::Unknown || core::isCompressedFormat(format))
    return false;
  for (std::uint32_t subresource = 1u;
       subresource < texture.subresourceCount; ++subresource) {
    D9CSurfaceDesc desc{};
    if (!load(descriptor, sizeof(texture) +
                              subresource * sizeof(D9CSurfaceDesc), desc) ||
        desc.format != level0.format ||
        desc.resourceType != (cube ? 5u : 3u) ||
        desc.usage != level0.usage || desc.pool != level0.pool ||
        desc.multiSampleType != 0u || desc.multiSampleQuality != 0u ||
        desc.width != mipExtent(
                          level0.width,
                          cube ? subresource % texture.mipLevelCount
                               : subresource) ||
        desc.height != mipExtent(
                           level0.height,
                           cube ? subresource % texture.mipLevelCount
                                : subresource) ||
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
          fixed.gammaRampBytes != 0u ||
          !parseChunks(event.payload.subspan(sizeof(fixed)), fixed.overlayCount,
                       candidate.bootstrap, fixed.gammaRampBytes)) goto unsupported;
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
            surface.schemaVersion != kRenderTapeSurfaceDescriptorVersion2)
          goto unsupported;
        if (surface.storage == static_cast<std::uint32_t>(
                RenderTapeSurfaceStorage::SwapchainBackbuffer)) {
          if (surface.initialContentDisposition != static_cast<std::uint32_t>(
                  RenderTapeInitialContentDisposition::ProducedPresentOutput) ||
              surface.subresource != 0u ||
              !renderTapeZeroIdentity(surface.parentTexture) ||
              fixed.expectedContentBytes != 0u ||
              fixed.expectedContentCount != 0u ||
              candidate.outputIdentity.objectId != 0u ||
              !acceptedOutputSurface(surface.surface)) goto unsupported;
          candidate.outputIdentity = fixed.identity;
          candidate.outputDesc = surface.surface;
        } else if (surface.storage == static_cast<std::uint32_t>(
                       RenderTapeSurfaceStorage::TextureSubresource)) {
          if (surface.initialContentDisposition != static_cast<std::uint32_t>(
                  RenderTapeInitialContentDisposition::Unavailable) ||
              fixed.expectedContentBytes != 0u ||
              fixed.expectedContentCount != 0u ||
              surface.parentTexture.kind != D9C_CHUNK_HANDLE_KIND_TEXTURE ||
              surface.parentTexture.generation == 0u ||
              surface.parentTexture.objectId == 0u) goto unsupported;
        } else if (surface.storage == static_cast<std::uint32_t>(
                       RenderTapeSurfaceStorage::Standalone)) {
          const auto disposition =
              static_cast<RenderTapeInitialContentDisposition>(
                  surface.initialContentDisposition);
          std::uint64_t depthBytes = 0u;
          const bool depthLayout =
              renderTapeSnapshotStandaloneD24X8Supported(surface.surface) &&
              checkedMul(surface.surface.width, surface.surface.height,
                         depthBytes) && checkedMul(depthBytes, 4u, depthBytes);
          std::uint64_t colorBytes = 0u;
          const bool colorLayout =
              renderTapeArmColorSnapshotStandaloneSurfaceSupported(
                  surface.surface) &&
              checkedMul(surface.surface.width, surface.surface.height,
                         colorBytes) && checkedMul(colorBytes, 4u, colorBytes);
          const bool produced =
              disposition ==
                  RenderTapeInitialContentDisposition::ProducedByCapturedPass &&
              fixed.expectedContentBytes == 0u &&
              fixed.expectedContentCount == 0u &&
              renderTapeProducedStandaloneSurfaceSupported(surface.surface);
          const bool seededDepth =
              disposition == RenderTapeInitialContentDisposition::
                                 CompleteDepthFloat32V1 &&
              depthLayout && fixed.expectedContentBytes == depthBytes &&
              fixed.expectedContentCount == 1u;
          const bool seededColor =
              disposition ==
                  RenderTapeInitialContentDisposition::CompleteSeed &&
              colorLayout && fixed.expectedContentBytes == colorBytes &&
              fixed.expectedContentCount == 1u;
          if (surface.subresource != 0u ||
              !renderTapeZeroIdentity(surface.parentTexture) ||
              (!produced && !seededDepth && !seededColor)) goto unsupported;
          if (produced) {
            candidate.producedStandaloneSurfaces.push_back(
                {fixed.identity, surface.surface});
          } else if (seededDepth) {
            candidate.seededStandaloneDepthSurfaces.push_back(
                {fixed.identity, surface.surface});
          } else {
            candidate.seededStandaloneColorSurfaces.push_back(
                {fixed.identity, surface.surface});
          }
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
        bool producedByCapturedPass = false;
        bool expectedContentShape = false;
        if (!noImmutablePayload || descriptor.size() < sizeof(texture) ||
            !load(descriptor, 0u, texture) ||
            !acceptedTextureDescriptor(descriptor, texture, level0))
          goto unsupported;
        producedByCapturedPass =
            texture.initialContentDisposition == static_cast<std::uint32_t>(
                RenderTapeInitialContentDisposition::ProducedByCapturedPass);
        expectedContentShape = producedByCapturedPass
            ? fixed.expectedContentBytes == 0u &&
                  fixed.expectedContentCount == 0u
            : fixed.expectedContentBytes != 0u &&
                  fixed.expectedContentCount == texture.subresourceCount;
        if (!expectedContentShape) goto unsupported;
        if (!producedByCapturedPass &&
            renderTapeArmColorSnapshotTextureSupported(descriptor)) {
          candidate.seededArmColorTextures.push_back(fixed.identity);
        } else {
          if (candidate.textureIdentity.objectId != 0u) goto unsupported;
          candidate.textureIdentity = fixed.identity;
          candidate.textureDesc = texture;
          candidate.textureLevel0 = level0;
          candidate.textureProducedByCapturedPass = producedByCapturedPass;
        }
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
          if (!load(record.payload, 0u, clear) || (clear.flags & 1u) == 0u ||
              (clear.flags & ~7u) != 0u || clear.rectCount != 0u)
            goto unsupported;
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
              present.hasSrc != present.hasDst ||
              (present.hasSrc != 0u &&
               (!fullRect(present.src, candidate.outputDesc.width,
                          candidate.outputDesc.height) ||
                !fullRect(present.dst, candidate.outputDesc.width,
                          candidate.outputDesc.height)))) goto unsupported;
          if (record.header.handleCount == 1u) {
            if (present.sourceHandleIndex >= chunk.handles.size())
              goto unsupported;
            const auto& source = chunk.handles[present.sourceHandleIndex];
            const D9CWireObjectIdentity identity{
                .kind = source.kind,
                .generation = source.generation,
                .objectId = source.objectId,
            };
            if (!sameIdentity(identity, candidate.outputIdentity))
              goto unsupported;
            ++result.coverage.presentSourceMappings;
          }
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
      // The compact interval plan has no ordered-state stream. Force the
      // general provider, which preserves and applies GammaRampSet.
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
          const auto existing = std::find_if(
              candidate.drawTextureIdentities.begin(),
              candidate.drawTextureIdentities.end(), [&](const auto& value) {
                return sameIdentity(value, drawState.texture);
              });
          if (existing == candidate.drawTextureIdentities.end()) {
            candidate.drawTextureIdentities.push_back(drawState.texture);
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
  if (anyTexturedDraw) {
    if (candidate.drawTextureIdentities.size() != 1u) goto unsupported;
    const auto& drawTexture = candidate.drawTextureIdentities.front();
    if (candidate.textureIdentity.objectId == 0u) {
      const auto seeded = std::find_if(
          candidate.seededArmColorTextures.begin(),
          candidate.seededArmColorTextures.end(), [&](const auto& value) {
            return sameIdentity(value, drawTexture);
          });
      const auto duplicate = seeded == candidate.seededArmColorTextures.end()
          ? seeded
          : std::find_if(std::next(seeded),
                         candidate.seededArmColorTextures.end(),
                         [&](const auto& value) {
                           return sameIdentity(value, drawTexture);
                         });
      const auto definition = std::find_if(
          candidate.definitions.begin(), candidate.definitions.end(),
          [&](const auto& value) {
            return sameIdentity(value.fixed.identity, drawTexture);
          });
      if (seeded == candidate.seededArmColorTextures.end() ||
          duplicate != candidate.seededArmColorTextures.end() ||
          definition == candidate.definitions.end() ||
          !load(definition->descriptor, 0u, candidate.textureDesc) ||
          !load(definition->descriptor, sizeof(candidate.textureDesc),
                candidate.textureLevel0) ||
          !renderTapeArmColorSnapshotTextureSupported(
              definition->descriptor)) {
        goto unsupported;
      }
      candidate.textureIdentity = drawTexture;
      candidate.textureProducedByCapturedPass = false;
    } else if (!sameIdentity(candidate.textureIdentity, drawTexture)) {
      goto unsupported;
    }
  }
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
    const bool selectedArmSnapshot = std::any_of(
        candidate.seededArmColorTextures.begin(),
        candidate.seededArmColorTextures.end(), [&](const auto& value) {
          return sameIdentity(value, candidate.textureIdentity);
        });
    const bool productionDeclaration =
        candidate.vertexDeclarationIdentity.objectId != 0u;
    const std::size_t expectedDefinitions =
        (productionDeclaration ? 3u : 2u) +
        candidate.seededArmColorTextures.size() -
        (selectedArmSnapshot ? 1u : 0u) +
        candidate.producedStandaloneSurfaces.size() +
        candidate.seededStandaloneDepthSurfaces.size() +
        candidate.seededStandaloneColorSurfaces.size();
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
          const bool parentAllowed =
              sameIdentity(surface.parentTexture,
                           candidate.textureIdentity) ||
              std::any_of(candidate.seededArmColorTextures.begin(),
                          candidate.seededArmColorTextures.end(),
                          [&](const auto& value) {
                            return sameIdentity(value,
                                                surface.parentTexture);
                          });
          const auto parent = std::find_if(
              candidate.definitions.begin(), candidate.definitions.end(),
              [&](const auto& value) {
                return sameIdentity(value.fixed.identity,
                                    surface.parentTexture);
              });
          return parentAllowed && parent != candidate.definitions.end() &&
                 renderTapeSurfaceAliasMatchesTextureSubresource(
                     parent->descriptor, parent->fixed.identity, surface);
        });
    const bool sequence = tape.header.profile == kRenderTapeProfileSequence;
    const auto mutationShapeMatches = [&](const auto& mutation) {
      return textureDefinition != candidate.definitions.end() &&
             sameIdentity(mutation.identity, candidate.textureIdentity) &&
             mutation.subresource == 0u && mutation.byteOffset == 0u &&
             mutation.byteSize == textureDefinition->fixed.expectedContentBytes;
    };
    const auto depthMutationShapeMatches = [&](const auto& mutation) {
      const auto seeded = std::find_if(
          candidate.seededStandaloneDepthSurfaces.begin(),
          candidate.seededStandaloneDepthSurfaces.end(),
          [&](const auto& value) {
            return sameIdentity(value.first, mutation.identity);
          });
      if (seeded == candidate.seededStandaloneDepthSurfaces.end() ||
          mutation.subresource != 0u || mutation.byteOffset != 0u) {
        return false;
      }
      const auto definition = std::find_if(
          candidate.definitions.begin(), candidate.definitions.end(),
          [&](const auto& value) {
            return sameIdentity(value.fixed.identity, mutation.identity);
          });
      return definition != candidate.definitions.end() &&
             mutation.byteSize == definition->fixed.expectedContentBytes;
    };
    const auto colorSurfaceMutationShapeMatches = [&](const auto& mutation) {
      const auto seeded = std::find_if(
          candidate.seededStandaloneColorSurfaces.begin(),
          candidate.seededStandaloneColorSurfaces.end(),
          [&](const auto& value) {
            return sameIdentity(value.first, mutation.identity);
          });
      if (seeded == candidate.seededStandaloneColorSurfaces.end() ||
          mutation.subresource != 0u || mutation.byteOffset != 0u) {
        return false;
      }
      const auto definition = std::find_if(
          candidate.definitions.begin(), candidate.definitions.end(),
          [&](const auto& value) {
            return sameIdentity(value.fixed.identity, mutation.identity);
          });
      return definition != candidate.definitions.end() &&
             mutation.byteSize == definition->fixed.expectedContentBytes;
    };
    std::size_t armTextureMutationCount = 0u;
    bool armTextureMutationSetMatches = true;
    for (const auto& identity : candidate.seededArmColorTextures) {
      const auto definition = std::find_if(
          candidate.definitions.begin(), candidate.definitions.end(),
          [&](const auto& value) {
            return sameIdentity(value.fixed.identity, identity);
          });
      RenderTapeTextureDescriptorV2 descriptor{};
      if (definition == candidate.definitions.end() ||
          !load(definition->descriptor, 0u, descriptor) ||
          !renderTapeArmColorSnapshotTextureSupported(
              definition->descriptor)) {
        armTextureMutationSetMatches = false;
        break;
      }
      armTextureMutationCount += descriptor.subresourceCount;
      for (std::uint32_t subresource = 0u;
           subresource < descriptor.subresourceCount; ++subresource) {
        D9CSurfaceDesc surface{};
        const auto expected = renderTapeTextureSubresourceDescriptor(
                                  definition->descriptor, subresource,
                                  surface)
            ? renderTapeDeriveExpectedSurfaceContent(surface)
            : RenderTapeExpectedContentContract{};
        const auto matches = std::count_if(
            candidate.initialMutations.begin(),
            candidate.initialMutations.end(), [&](const auto& mutation) {
              return sameIdentity(mutation.identity, identity) &&
                     mutation.kind == static_cast<std::uint32_t>(
                         RenderTapeMutationKind::Upload) &&
                     mutation.subresource == subresource &&
                     mutation.byteOffset == 0u &&
                     expected.status ==
                         RenderTapeExpectedContentStatus::Accepted &&
                     mutation.byteSize == expected.bytes;
            });
        if (matches != 1) armTextureMutationSetMatches = false;
      }
    }
    const std::size_t textureMutationCount =
        candidate.textureProducedByCapturedPass || selectedArmSnapshot
            ? 0u
            : 1u;
    const bool initialMutationSetMatches =
        candidate.initialMutations.size() ==
            textureMutationCount +
                armTextureMutationCount +
                candidate.seededStandaloneDepthSurfaces.size() +
                candidate.seededStandaloneColorSurfaces.size() &&
        armTextureMutationSetMatches &&
        static_cast<std::size_t>(std::count_if(
            candidate.initialMutations.begin(),
            candidate.initialMutations.end(), mutationShapeMatches)) ==
            textureMutationCount &&
        static_cast<std::size_t>(std::count_if(
            candidate.initialMutations.begin(),
            candidate.initialMutations.end(), depthMutationShapeMatches)) ==
            candidate.seededStandaloneDepthSurfaces.size() &&
        static_cast<std::size_t>(std::count_if(
            candidate.initialMutations.begin(),
            candidate.initialMutations.end(),
            colorSurfaceMutationShapeMatches)) ==
            candidate.seededStandaloneColorSurfaces.size();
    if (candidate.definitions.size() !=
            expectedDefinitions + expectedSurfaceAliases ||
        candidate.textureIdentity.objectId == 0u ||
        !aliasesNameCandidateTexture ||
        textureDefinition == candidate.definitions.end() ||
        (!candidate.textureProducedByCapturedPass &&
         !sameIdentity(drawState.texture, candidate.textureIdentity)) ||
        candidate.textureDesc.mipLevelCount != 1u ||
        (sequence && selectedArmSnapshot) ||
        ((!candidate.textureProducedByCapturedPass && !selectedArmSnapshot &&
          candidate.textureLevel0.format != 21u) ||
         (selectedArmSnapshot &&
          !renderTapeArmColorSnapshotTextureSupported(
              textureDefinition->descriptor)) ||
         (candidate.textureProducedByCapturedPass &&
          candidate.textureLevel0.format != 22u &&
          candidate.textureLevel0.format != 114u)) ||
        (!candidate.textureProducedByCapturedPass &&
         (textureDefinition->fixed.expectedContentBytes < tightTextureBytes ||
          textureDefinition->fixed.expectedContentBytes %
                  candidate.textureLevel0.height != 0u ||
          !initialMutationSetMatches ||
          textureDefinition->fixed.expectedContentCount !=
              (selectedArmSnapshot
                   ? candidate.textureDesc.subresourceCount
                   : 1u))) ||
        (candidate.textureProducedByCapturedPass &&
         (!initialMutationSetMatches ||
          textureDefinition->fixed.expectedContentCount != 0u))) {
      goto unsupported;
    }
    if (sequence) {
      const auto initialTextureMutation = std::find_if(
          candidate.initialMutations.begin(), candidate.initialMutations.end(),
          mutationShapeMatches);
      if (!candidate.intervals[0].sawTexturedDraw ||
          !candidate.intervals[1].sawTexturedDraw ||
          !candidate.intervals[0].mutationsBefore.empty() ||
          candidate.intervals[1].mutationsBefore.size() != 1u ||
          initialTextureMutation == candidate.initialMutations.end() ||
          !mutationShapeMatches(candidate.intervals[1].mutationsBefore[0]) ||
          sameDigest(initialTextureMutation->digest,
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

struct GeneralPreflightPlan {
  ImportedRenderTapeView tape{};
  RenderTapeBlobCatalogue catalogue{};
  // Value-owned definitions are retained by the plan so all resource
  // mutation shape checks happen before the provider creates any Metal
  // object.  Keeping the descriptor span in the imported tape is safe for
  // the lifetime of the plan (the caller owns the tape bytes).
  std::vector<Definition> definitions{};
  std::vector<D9CWireObjectIdentity> bootstrapIdentities{};
  D9CWireObjectIdentity outputIdentity{};
  D9CSurfaceDesc outputDesc{};
  RenderTapeDigestValidity expectedDigestValidity =
      RenderTapeDigestValidity::NotCaptured;
  RenderTapeDigest expectedDigest{};
  std::uint64_t presentOrdinal = 0u;
  std::uint64_t completionOrdinal = 0u;
};

bool generalMutationShapeValid(
    const RenderTapeResourceMutationHeader& mutation,
    const Definition& definition,
    std::span<const std::byte> bytes) noexcept {
  if (mutation.byteSize == 0u || bytes.size() != mutation.byteSize ||
      (mutation.kind != static_cast<std::uint32_t>(RenderTapeMutationKind::Upload) &&
       mutation.kind != static_cast<std::uint32_t>(RenderTapeMutationKind::CpuUnlock))) {
    return false;
  }

  const auto kind = mutation.identity.kind;
  if (kind == D9C_CHUNK_HANDLE_KIND_BUFFER) {
    D9CBufferDesc buffer{};
    std::uint64_t end = 0u;
    return definition.descriptor.size() == sizeof(buffer) &&
           load(definition.descriptor, 0u, buffer) && buffer.size != 0u &&
           checkedAdd(mutation.byteOffset, mutation.byteSize, end) &&
           end <= buffer.size;
  }

  if (mutation.byteOffset != 0u) return false;
  if (kind == D9C_CHUNK_HANDLE_KIND_TEXTURE) {
    D9CSurfaceDesc surface{};
    if (!renderTapeTextureSubresourceDescriptor(
            definition.descriptor, mutation.subresource, surface)) {
      return false;
    }
    const auto expected = renderTapeDeriveExpectedSurfaceContent(surface);
    return expected.status == RenderTapeExpectedContentStatus::Accepted &&
           mutation.byteSize == expected.bytes;
  }

  if (kind != D9C_CHUNK_HANDLE_KIND_SURFACE || mutation.subresource != 0u)
    return false;
  RenderTapeSurfaceDescriptorV2 surface{};
  if (!renderTapeLoadSurfaceDescriptorV2(definition.descriptor, surface) ||
      (surface.storage != static_cast<std::uint32_t>(
           RenderTapeSurfaceStorage::Standalone) &&
       surface.storage != static_cast<std::uint32_t>(
           RenderTapeSurfaceStorage::SwapchainBackbuffer))) {
    return false;
  }
  const auto disposition = static_cast<RenderTapeInitialContentDisposition>(
      surface.initialContentDisposition);
  if (disposition == RenderTapeInitialContentDisposition::CompleteDepthFloat32V1) {
    std::uint64_t rowBytes = 0u;
    std::uint64_t expectedBytes = 0u;
    return renderTapeSnapshotStandaloneD24X8Supported(surface.surface) &&
           checkedMul(surface.surface.width, sizeof(float), rowBytes) &&
           checkedMul(rowBytes, surface.surface.height, expectedBytes) &&
           mutation.byteSize == expectedBytes;
  }
  if (disposition != RenderTapeInitialContentDisposition::CompleteSeed ||
      (static_cast<RenderTapeSurfaceStorage>(surface.storage) ==
           RenderTapeSurfaceStorage::SwapchainBackbuffer &&
       !renderTapeArmColorSnapshotSwapchainSurfaceSupported(surface.surface)))
    return false;
  const auto expected = renderTapeDeriveExpectedSurfaceContent(surface.surface);
  return expected.status == RenderTapeExpectedContentStatus::Accepted &&
         mutation.byteSize == expected.bytes;
}

bool generalRecordTypeSupported(std::uint32_t type) noexcept {
  switch (type) {
  case D9C_COMMAND_RECORD_DRAW_PRIMITIVE:
  case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE:
  case D9C_COMMAND_RECORD_SET_VS_CONST_F:
  case D9C_COMMAND_RECORD_SET_VS_CONST_I:
  case D9C_COMMAND_RECORD_SET_VS_CONST_B:
  case D9C_COMMAND_RECORD_SET_PS_CONST_F:
  case D9C_COMMAND_RECORD_SET_PS_CONST_I:
  case D9C_COMMAND_RECORD_SET_PS_CONST_B:
  case D9C_COMMAND_RECORD_CLEAR:
  case D9C_COMMAND_RECORD_PRESENT:
  case D9C_COMMAND_RECORD_APPLY_STATE:
    return true;
  default:
    return false;
  }
}

FrameTapeReplayResult buildGeneralPlan(
    std::span<const std::byte> bytes,
    std::span<const RenderTapeProviderBlob> blobs,
    GeneralPreflightPlan* plan) {
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

  GeneralPreflightPlan candidate{
      .tape = tape,
      .catalogue = std::move(catalogue),
  };
  std::vector<RenderTapeDigest> referencedBlobs;
  const auto referenceBlob = [&](const RenderTapeDigest& digest) {
    if (std::none_of(referencedBlobs.begin(), referencedBlobs.end(),
                     [&](const auto& prior) {
                       return sameDigest(prior, digest);
                     })) {
      referencedBlobs.push_back(digest);
    }
  };
  bool sawShader = false;
  bool sawBuffer = false;
  bool sawIndexedDraw = false;
  bool sawPresentComplete = false;
  std::uint32_t currentEventIndex = 0xffffffffu;
  std::size_t firstCommand = tape.events.size();

  for (std::uint32_t index = 0u; index < tape.events.size(); ++index) {
    currentEventIndex = index;
    const auto event = tape.event(index);
    switch (static_cast<RenderTapeEventType>(event.header.type)) {
    case RenderTapeEventType::BootstrapState: {
      RenderTapeBootstrapHeader fixed{};
      std::vector<Chunk> chunks;
      if (!load(event.payload, 0u, fixed) || fixed.overlayCount == 0u ||
          !parseChunks(event.payload.subspan(sizeof(fixed)), fixed.overlayCount,
                       chunks, fixed.gammaRampBytes)) {
        goto unsupported;
      }
      for (const auto& overlay : chunks) {
        ImportedChunkView imported;
        if (!importPrevalidatedCommandChunk(overlay.bytes, overlay.envelope,
                                            imported)) {
          goto unsupported;
        }
        for (const auto& handle : imported.handles) {
          if (handle.kind == 0u || handle.objectId == 0u) continue;
          candidate.bootstrapIdentities.push_back(D9CWireObjectIdentity{
              .kind = handle.kind,
              .generation = handle.generation,
              .objectId = handle.objectId,
          });
        }
      }
      result.coverage.bootstrapChunks += fixed.overlayCount;
      break;
    }
    case RenderTapeEventType::ObjectDefine: {
      RenderTapeObjectDefineHeader fixed{};
      if (!load(event.payload, 0u, fixed)) goto unsupported;
      const auto descriptor = event.payload.subspan(sizeof(fixed));
      if (fixed.identity.kind == D9C_CHUNK_HANDLE_KIND_TEXTURE) {
        RenderTapeTextureDescriptorV2 texture{};
        D9CSurfaceDesc level0{};
        if (!renderTapeLoadTextureDescriptorV2(descriptor, texture) ||
            !renderTapeTextureSubresourceDescriptor(descriptor, 0u, level0) ||
            (texture.dimension != static_cast<std::uint32_t>(
                                      RenderTapeTextureDimension::Texture2D) &&
             texture.dimension != static_cast<std::uint32_t>(
                                      RenderTapeTextureDimension::Cube)) ||
            (static_cast<RenderTapeInitialContentDisposition>(
                 texture.initialContentDisposition) !=
                 RenderTapeInitialContentDisposition::CompleteSeed &&
             static_cast<RenderTapeInitialContentDisposition>(
                 texture.initialContentDisposition) !=
                 RenderTapeInitialContentDisposition::ProducedByCapturedPass)) {
          goto unsupported;
        }
      } else if (fixed.identity.kind == D9C_CHUNK_HANDLE_KIND_SURFACE) {
        RenderTapeSurfaceDescriptorV2 surface{};
        if (!renderTapeLoadSurfaceDescriptorV2(descriptor, surface))
          goto unsupported;
        const auto storage =
            static_cast<RenderTapeSurfaceStorage>(surface.storage);
        const auto disposition = static_cast<RenderTapeInitialContentDisposition>(
            surface.initialContentDisposition);
        if (storage == RenderTapeSurfaceStorage::SwapchainBackbuffer) {
          const bool produced =
              disposition == RenderTapeInitialContentDisposition::ProducedPresentOutput;
          const bool seeded =
              disposition == RenderTapeInitialContentDisposition::CompleteSeed &&
              renderTapeArmColorSnapshotSwapchainSurfaceSupported(surface.surface);
          if ((!produced && !seeded) ||
              candidate.outputIdentity.objectId != 0u ||
              !acceptedOutputSurface(surface.surface) ||
              (seeded && (fixed.expectedContentBytes == 0u ||
                          fixed.expectedContentCount != 1u)) ||
              (produced && (fixed.expectedContentBytes != 0u ||
                            fixed.expectedContentCount != 0u))) {
            goto unsupported;
          }
          candidate.outputIdentity = fixed.identity;
          candidate.outputDesc = surface.surface;
        } else if (storage == RenderTapeSurfaceStorage::TextureSubresource) {
          if (disposition != RenderTapeInitialContentDisposition::Unavailable)
            goto unsupported;
        } else if (storage == RenderTapeSurfaceStorage::Standalone) {
          if (disposition != RenderTapeInitialContentDisposition::CompleteSeed &&
              disposition != RenderTapeInitialContentDisposition::
                                 CompleteDepthFloat32V1 &&
              disposition != RenderTapeInitialContentDisposition::
                                 ProducedByCapturedPass) {
            goto unsupported;
          }
        } else {
          goto unsupported;
        }
      } else if (fixed.identity.kind == D9C_CHUNK_HANDLE_KIND_BUFFER) {
        D9CBufferDesc buffer{};
        if (descriptor.size() != sizeof(buffer) ||
            !load(descriptor, 0u, buffer) || buffer.size == 0u) {
          goto unsupported;
        }
        sawBuffer = true;
      } else if (fixed.identity.kind == D9C_CHUNK_HANDLE_KIND_SHADER) {
        RenderTapeShaderDescriptor shader{};
        const auto* blob = findBlob(blobs, fixed.immutablePayloadDigest);
        if (descriptor.size() != sizeof(shader) ||
            !load(descriptor, 0u, shader) || shader.stage > 1u || !blob ||
            shader.bytecodeBytes != fixed.immutablePayloadBytes ||
            blob->bytes.size() != shader.bytecodeBytes ||
            shader.bytecodeBytes < sizeof(std::uint32_t) * 2u ||
            shader.bytecodeBytes % sizeof(std::uint32_t) != 0u) {
          goto unsupported;
        }
        referenceBlob(fixed.immutablePayloadDigest);
        sawShader = true;
      } else if (fixed.identity.kind == D9C_CHUNK_HANDLE_KIND_VERTEX_DECL) {
        RenderTapeVertexDeclDescriptor declaration{};
        const auto* blob = findBlob(blobs, fixed.immutablePayloadDigest);
        if (descriptor.size() != sizeof(declaration) ||
            !load(descriptor, 0u, declaration) || !blob ||
            declaration.elementBytes != fixed.immutablePayloadBytes ||
            blob->bytes.size() != declaration.elementBytes ||
            declaration.elementCount == 0u ||
            declaration.elementBytes % sizeof(D9CVertexElement) != 0u) {
          goto unsupported;
        }
        referenceBlob(fixed.immutablePayloadDigest);
      } else {
        goto unsupported;
      }
      candidate.definitions.push_back(Definition{
          .fixed = fixed,
          .descriptor = descriptor,
      });
      ++result.coverage.objectDefinitions;
      break;
    }
    case RenderTapeEventType::ObjectDestroy: {
      RenderTapeObjectDestroyHeader fixed{};
      if (!load(event.payload, 0u, fixed)) {
        goto unsupported;
      }
      ++result.coverage.objectDestroys;
      break;
    }
    case RenderTapeEventType::ResourceMutation: {
      RenderTapeResourceMutationHeader fixed{};
      const auto *mutationBlob = load(event.payload, 0u, fixed)
          ? findBlob(blobs, fixed.digest)
          : nullptr;
      const auto definition = std::find_if(
          candidate.definitions.begin(), candidate.definitions.end(),
          [&](const auto& value) {
            return sameIdentity(value.fixed.identity, fixed.identity);
          });
      if (!mutationBlob || definition == candidate.definitions.end() ||
          !generalMutationShapeValid(fixed, *definition, mutationBlob->bytes)) {
        goto unsupported;
      }
      referenceBlob(fixed.digest);
      ++result.coverage.seedMutations;
      break;
    }
    case RenderTapeEventType::CommandChunk: {
      RenderTapeCommandChunkHeader fixed{};
      ImportedChunkView chunk;
      if (!load(event.payload, 0u, fixed) ||
          !importPrevalidatedCommandChunk(
              event.payload.subspan(sizeof(fixed)),
              CommandChunkEnvelope{fixed.wireVersion, fixed.recordCount,
                                   fixed.handleCount},
              chunk)) {
        goto unsupported;
      }
      for (std::size_t recordIndex = 0u; recordIndex < chunk.records.size();
           ++recordIndex) {
        const auto record = chunk.record(recordIndex);
        if (!generalRecordTypeSupported(record.header.type)) goto unsupported;
        ++result.coverage.commandRecords;
        switch (record.header.type) {
        case D9C_COMMAND_RECORD_DRAW_PRIMITIVE:
          ++result.coverage.drawPrimitiveRecords;
          break;
        case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE:
          ++result.coverage.drawIndexedPrimitiveRecords;
          sawIndexedDraw = true;
          break;
        case D9C_COMMAND_RECORD_SET_VS_CONST_F:
        case D9C_COMMAND_RECORD_SET_VS_CONST_I:
        case D9C_COMMAND_RECORD_SET_VS_CONST_B:
        case D9C_COMMAND_RECORD_SET_PS_CONST_F:
        case D9C_COMMAND_RECORD_SET_PS_CONST_I:
        case D9C_COMMAND_RECORD_SET_PS_CONST_B:
          ++result.coverage.stateConstantRecords;
          break;
        case D9C_COMMAND_RECORD_CLEAR:
          ++result.coverage.clearRecords;
          break;
        case D9C_COMMAND_RECORD_APPLY_STATE:
          ++result.coverage.applyStateRecords;
          break;
        case D9C_COMMAND_RECORD_PRESENT:
          {
          D9CCommandChunkWirePresent present{};
          if (!load(record.payload, 0u, present) ||
              record.header.handleCount > 1u ||
              !presentGeometrySupported(present, candidate.outputDesc)) {
            goto unsupported;
          }
          if (record.header.handleCount == 1u) {
            if (present.sourceHandleIndex >= chunk.handles.size())
              goto unsupported;
            const auto& source = chunk.handles[present.sourceHandleIndex];
            const D9CWireObjectIdentity identity{
                .kind = source.kind,
                .generation = source.generation,
                .objectId = source.objectId,
            };
            if (!sameIdentity(identity, candidate.outputIdentity))
              goto unsupported;
            ++result.coverage.presentSourceMappings;
          }
          ++result.coverage.presentRecords;
          break;
          }
        default:
          break;
        }
      }
      ++result.coverage.commandChunks;
      break;
    }
    case RenderTapeEventType::OrderedControl: {
      RenderTapeOrderedControlHeader fixed{};
      if (!load(event.payload, 0u, fixed) ||
          fixed.kind != static_cast<std::uint32_t>(
              RenderTapeControlKind::GammaRampSet) ||
          fixed.controlBytes != kRenderTapeGammaRampBytes ||
          event.payload.size() != sizeof(fixed) + kRenderTapeGammaRampBytes) {
        goto unsupported;
      }
      break;
    }
    case RenderTapeEventType::PresentComplete: {
      RenderTapePresentCompleteHeader fixed{};
      RenderTapeOracleAttachment oracle{};
      if (sawPresentComplete || !load(event.payload, 0u, fixed) ||
          fixed.oracleCount != 1u ||
          !load(event.payload, sizeof(fixed), oracle) ||
          !sameIdentity(oracle.identity, candidate.outputIdentity) ||
          (fixed.digestValidity != static_cast<std::uint32_t>(
                                       RenderTapeDigestValidity::NotCaptured) &&
           fixed.digestValidity != static_cast<std::uint32_t>(
                                       RenderTapeDigestValidity::Sha256))) {
        goto unsupported;
      }
      candidate.expectedDigestValidity =
          static_cast<RenderTapeDigestValidity>(fixed.digestValidity);
      candidate.expectedDigest = fixed.expectedDigest;
      candidate.presentOrdinal = fixed.presentOrdinal;
      candidate.completionOrdinal = fixed.completionOrdinal;
      sawPresentComplete = true;
      ++result.coverage.presentOutputs;
      break;
    }
    }
  }

  if (!sawShader || !sawBuffer || !sawIndexedDraw || !sawPresentComplete ||
      candidate.outputIdentity.objectId == 0u || tape.header.presentCount != 1u ||
      result.coverage.presentRecords != 1u ||
      result.coverage.presentSourceMappings > result.coverage.presentRecords ||
      referencedBlobs.size() != blobs.size()) {
    goto unsupported;
  }
  // Bootstrap overlays are replayed at the first command boundary (or at an
  // early destroy).  Every referenced object must therefore already have a
  // value-owned definition before either trigger; otherwise replay could
  // materialize a partially populated bootstrap and mutate Metal state before
  // discovering the missing object.
  for (const auto& identity : candidate.bootstrapIdentities) {
    const auto definition = std::find_if(
        candidate.definitions.begin(), candidate.definitions.end(),
        [&](const auto& value) { return sameIdentity(value.fixed.identity, identity); });
    if (definition == candidate.definitions.end()) goto unsupported;
    std::size_t definitionIndex = tape.events.size();
    std::size_t firstTrigger = tape.events.size();
    for (std::size_t eventIndex = 0u; eventIndex < tape.events.size();
         ++eventIndex) {
      const auto value = tape.event(eventIndex);
      if (value.header.type == static_cast<std::uint32_t>(
                                  RenderTapeEventType::ObjectDefine) &&
          definitionIndex == tape.events.size()) {
        RenderTapeObjectDefineHeader fixed{};
        if (load(value.payload, 0u, fixed) &&
            sameIdentity(fixed.identity, identity)) {
          definitionIndex = eventIndex;
        }
      }
      if (firstTrigger == tape.events.size() &&
          (value.header.type == static_cast<std::uint32_t>(
                                    RenderTapeEventType::CommandChunk) ||
           value.header.type == static_cast<std::uint32_t>(
                                    RenderTapeEventType::ObjectDestroy))) {
        firstTrigger = eventIndex;
      }
    }
    if (definitionIndex == tape.events.size() ||
        (firstTrigger != tape.events.size() && definitionIndex >= firstTrigger)) {
      goto unsupported;
    }
  }
  // A destroy can be the first bootstrap trigger.  Do not allow it to retire
  // an object that the deferred overlay still needs; otherwise the later
  // overlay application would dereference a value that has already been
  // released even though every definition was present.
  for (std::size_t eventIndex = 0u; eventIndex < tape.events.size();
       ++eventIndex) {
    const auto value = tape.event(eventIndex);
    if (value.header.type == static_cast<std::uint32_t>(
                                RenderTapeEventType::CommandChunk)) {
      firstCommand = eventIndex;
      break;
    }
  }
  if (firstCommand != tape.events.size()) {
    for (std::size_t eventIndex = 0u; eventIndex < firstCommand;
         ++eventIndex) {
      const auto value = tape.event(eventIndex);
      if (value.header.type != static_cast<std::uint32_t>(
                                   RenderTapeEventType::ObjectDestroy)) {
        continue;
      }
      RenderTapeObjectDestroyHeader destroy{};
      if (!load(value.payload, 0u, destroy) ||
          containsIdentity(candidate.bootstrapIdentities, destroy.identity)) {
        goto unsupported;
      }
    }
  }
  result.intervalCount = 1u;
  result.requirements = FrameTapeReplayRequirements{
      .outputWidth = candidate.outputDesc.width,
      .outputHeight = candidate.outputDesc.height,
      .outputFormat = candidate.outputDesc.format,
  };
  result.conservation.referencedBlobs =
      static_cast<std::uint32_t>(referencedBlobs.size());
  result.conservation.presentOrdinal = candidate.presentOrdinal;
  result.conservation.completionOrdinal = candidate.completionOrdinal;
  result.intervals[0].presentOrdinal = candidate.presentOrdinal;
  result.intervals[0].completionOrdinal = candidate.completionOrdinal;
  result.validity.expectedDigestCaptured =
      candidate.expectedDigestValidity == RenderTapeDigestValidity::Sha256;
  result.intervals[0].validity.expectedDigestCaptured =
      result.validity.expectedDigestCaptured;
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
  case D9C_CHUNK_HANDLE_KIND_SHADER:
    dxmt9c_shader_release(static_cast<D9CShader*>(object.value)); break;
  case D9C_CHUNK_HANDLE_KIND_VERTEX_DECL:
    dxmt9c_vdecl_release(static_cast<D9CVertexDecl*>(object.value)); break;
  case D9C_CHUNK_HANDLE_KIND_QUERY:
    dxmt9c_query_release(static_cast<D9CQuery*>(object.value)); break;
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

bool writeBlockRows(std::span<const std::byte> bytes, void* bits,
                    const RenderTapeBlockLockLayout& layout) {
  if (!bits || bytes.size() != layout.tightBytes || layout.rowBytes == 0u ||
      layout.rows == 0u || layout.pitch < layout.rowBytes ||
      layout.tightBytes !=
          static_cast<std::uint64_t>(layout.rowBytes) * layout.rows) {
    return false;
  }
  auto* destination = static_cast<std::byte*>(bits);
  for (std::uint32_t row = 0u; row < layout.rows; ++row) {
    std::memcpy(destination + static_cast<std::size_t>(row) * layout.pitch,
                bytes.data() + static_cast<std::size_t>(row) * layout.rowBytes,
                layout.rowBytes);
  }
  return true;
}

void* createGeneralObject(
    D9CDevice* device, const RenderTapeObjectDefineHeader& fixed,
    std::span<const std::byte> descriptor,
    std::span<const RenderTapeProviderBlob> blobs,
    std::vector<OwnedObject>& objects) {
  if (fixed.identity.kind == D9C_CHUNK_HANDLE_KIND_SURFACE) {
    RenderTapeSurfaceDescriptorV2 surface{};
    if (!renderTapeLoadSurfaceDescriptorV2(descriptor, surface)) return nullptr;
    const auto storage = static_cast<RenderTapeSurfaceStorage>(surface.storage);
    if (storage == RenderTapeSurfaceStorage::TextureSubresource) {
      auto* parent = static_cast<D9CTexture*>(
          findObject(objects, surface.parentTexture));
      return parent
          ? static_cast<void*>(dxmt9c_texture_get_surface_level(
                parent, surface.subresource))
          : nullptr;
    }
    if (storage == RenderTapeSurfaceStorage::SwapchainBackbuffer) {
      auto* swap = device->iface->GetSwapChain(0u);
      if (!swap) return nullptr;
      auto* value = new D9CSurface{swap->backBuffer(), nullptr, 0u, device};
      swap->Release();
      return value;
    }
    if (storage != RenderTapeSurfaceStorage::Standalone) return nullptr;
    if ((surface.surface.usage & 1u) != 0u) {
      return dxmt9c_device_create_render_target(
          device, surface.surface.width, surface.surface.height,
          surface.surface.format, surface.surface.multiSampleType,
          surface.surface.multiSampleQuality, 0u, nullptr);
    }
    if ((surface.surface.usage & 2u) != 0u) {
      return dxmt9c_device_create_depth_stencil(
          device, surface.surface.width, surface.surface.height,
          surface.surface.format, surface.surface.multiSampleType,
          surface.surface.multiSampleQuality, 0u, nullptr);
    }
    return nullptr;
  }
  if (fixed.identity.kind == D9C_CHUNK_HANDLE_KIND_BUFFER) {
    D9CBufferDesc desc{};
    if (!load(descriptor, 0u, desc)) return nullptr;
    return desc.format
        ? static_cast<void*>(dxmt9c_device_create_index_buffer(
              device, desc.size, desc.usage, desc.format, desc.pool))
        : static_cast<void*>(dxmt9c_device_create_vertex_buffer(
              device, desc.size, desc.usage, desc.fvf, desc.pool));
  }
  if (fixed.identity.kind == D9C_CHUNK_HANDLE_KIND_TEXTURE) {
    RenderTapeTextureDescriptorV2 desc{};
    D9CSurfaceDesc level0{};
    if (!renderTapeLoadTextureDescriptorV2(descriptor, desc) ||
        !renderTapeTextureSubresourceDescriptor(descriptor, 0u, level0)) {
      return nullptr;
    }
    if (desc.dimension == static_cast<std::uint32_t>(
                              RenderTapeTextureDimension::Cube)) {
      return dxmt9c_device_create_cube_texture(
          device, level0.width, desc.mipLevelCount, level0.usage,
          level0.format, level0.pool);
    }
    if (desc.dimension == static_cast<std::uint32_t>(
                              RenderTapeTextureDimension::Texture2D)) {
      return dxmt9c_device_create_texture(
          device, level0.width, level0.height, desc.mipLevelCount,
          level0.usage, level0.format, level0.pool);
    }
    return nullptr;
  }
  if (fixed.identity.kind == D9C_CHUNK_HANDLE_KIND_SHADER) {
    RenderTapeShaderDescriptor desc{};
    const auto* blob = findBlob(blobs, fixed.immutablePayloadDigest);
    if (!load(descriptor, 0u, desc) || !blob) return nullptr;
    const auto* bytecode =
        reinterpret_cast<const std::uint32_t*>(blob->bytes.data());
    return desc.stage == 0u
        ? static_cast<void*>(dxmt9c_device_create_vertex_shader(device,
                                                                 bytecode))
        : static_cast<void*>(dxmt9c_device_create_pixel_shader(device,
                                                                bytecode));
  }
  if (fixed.identity.kind == D9C_CHUNK_HANDLE_KIND_VERTEX_DECL) {
    const auto* blob = findBlob(blobs, fixed.immutablePayloadDigest);
    return blob
        ? static_cast<void*>(dxmt9c_device_create_vertex_declaration(
              device,
              reinterpret_cast<const D9CVertexElement*>(blob->bytes.data())))
        : nullptr;
  }
  return nullptr;
}

bool writeGeneralLockedSurface(const D9CSurfaceDesc& desc,
                               const D9CLockedRect& locked,
                               std::span<const std::byte> bytes) {
  if (renderTapeLinearBytesPerPixel(desc.format) != 0u) {
    RenderTapeLinearLockLayout layout{};
    return renderTapeLinearLockLayout(desc, locked.pitch, nullptr, layout) ==
               RenderTapeLinearLayoutStatus::Accepted &&
           writeRenderTapeLinearRows(bytes, locked.bits, layout);
  }
  RenderTapeBlockLockLayout layout{};
  return renderTapeBlockLockLayout(desc, locked.pitch, nullptr, layout) ==
             RenderTapeBlockLayoutStatus::Accepted &&
         writeBlockRows(bytes, locked.bits, layout);
}

bool applyGeneralMutation(
    D9CDevice* device, const RenderTapeResourceMutationHeader& mutation,
    std::span<const RenderTapeProviderBlob> blobs,
    const std::vector<Definition>& definitions,
    std::vector<OwnedObject>& objects) {
  const auto* blob = findBlob(blobs, mutation.digest);
  void* object = findObject(objects, mutation.identity);
  if (!blob || !object || blob->bytes.size() != mutation.byteSize) return false;
  const auto definition = std::find_if(
      definitions.begin(), definitions.end(), [&](const auto& value) {
        return sameIdentity(value.fixed.identity, mutation.identity);
      });
  if (definition == definitions.end()) return false;

  if (mutation.identity.kind == D9C_CHUNK_HANDLE_KIND_BUFFER) {
    if (mutation.byteOffset > std::numeric_limits<std::uint32_t>::max() ||
        mutation.byteSize > std::numeric_limits<std::uint32_t>::max()) {
      return false;
    }
    void* mapped = nullptr;
    auto* buffer = static_cast<D9CBuffer*>(object);
    if (dxmt9c_buffer_lock(
            buffer, static_cast<std::uint32_t>(mutation.byteOffset),
            static_cast<std::uint32_t>(mutation.byteSize), &mapped, 0u) !=
            core::D3D_OK ||
        !mapped) {
      return false;
    }
    std::memcpy(mapped, blob->bytes.data(), blob->bytes.size());
    return dxmt9c_buffer_unlock(buffer) == core::D3D_OK;
  }

  if (mutation.identity.kind == D9C_CHUNK_HANDLE_KIND_TEXTURE) {
    if (mutation.byteOffset != 0u) return false;
    D9CSurfaceDesc desc{};
    if (!renderTapeTextureSubresourceDescriptor(
            definition->descriptor, mutation.subresource, desc)) {
      return false;
    }
    D9CLockedRect locked{};
    auto* texture = static_cast<D9CTexture*>(object);
    if (dxmt9c_texture_lock_rect(texture, mutation.subresource, &locked,
                                 nullptr, 0u) != core::D3D_OK ||
        !locked.bits) {
      return false;
    }
    const bool copied = writeGeneralLockedSurface(desc, locked, blob->bytes);
    const bool unlocked =
        dxmt9c_texture_unlock_rect(texture, mutation.subresource) ==
        core::D3D_OK;
    return copied && unlocked;
  }

  if (mutation.identity.kind != D9C_CHUNK_HANDLE_KIND_SURFACE ||
      mutation.subresource != 0u || mutation.byteOffset != 0u) {
    return false;
  }
  RenderTapeSurfaceDescriptorV2 surface{};
  if (!renderTapeLoadSurfaceDescriptorV2(definition->descriptor, surface) ||
      (surface.storage != static_cast<std::uint32_t>(
           RenderTapeSurfaceStorage::Standalone) &&
       surface.storage != static_cast<std::uint32_t>(
           RenderTapeSurfaceStorage::SwapchainBackbuffer))) {
    return false;
  }
  auto* wrapped = static_cast<D9CSurface*>(object);
  const auto disposition = static_cast<RenderTapeInitialContentDisposition>(
      surface.initialContentDisposition);
  if (disposition ==
      RenderTapeInitialContentDisposition::CompleteDepthFloat32V1) {
    auto upper = device->dev().upperDevice();
    if (!upper || !wrapped || !wrapped->obj) return false;
    core::CanonicalD24X8Depth canonical{
        .bytes = std::vector<core::u8>(blob->bytes.size()),
        .version = core::kCanonicalD24X8DepthVersion1,
        .width = surface.surface.width,
        .height = surface.surface.height,
        .pitch = surface.surface.width * 4u,
        .physicalFormat = static_cast<core::u32>(convert::toPixelFormat(
            core::Format::D24X8, upper->limits())),
    };
    std::memcpy(canonical.bytes.data(), blob->bytes.data(), blob->bytes.size());
    return upper->seedCanonicalD24X8Depth(wrapped->obj->handle(), canonical);
  }
  if (disposition != RenderTapeInitialContentDisposition::CompleteSeed ||
      (static_cast<RenderTapeSurfaceStorage>(surface.storage) ==
           RenderTapeSurfaceStorage::SwapchainBackbuffer &&
       !renderTapeArmColorSnapshotSwapchainSurfaceSupported(surface.surface)))
    return false;
  if (static_cast<RenderTapeSurfaceStorage>(surface.storage) ==
      RenderTapeSurfaceStorage::SwapchainBackbuffer) {
    auto upper = device ? device->dev().upperDevice() : nullptr;
    if (!upper || !wrapped || !wrapped->obj) return false;
    auto* stagingTexture = dxmt9c_device_create_texture(
        device, surface.surface.width, surface.surface.height, 1u, 0u,
        surface.surface.format, 0u);
    if (!stagingTexture || !stagingTexture->obj) {
      if (stagingTexture) dxmt9c_texture_release(stagingTexture);
      return false;
    }
    D9CLockedRect locked{};
    bool copied = dxmt9c_texture_lock_rect(
                      stagingTexture, 0u, &locked, nullptr, 0u) ==
                      core::D3D_OK &&
                  locked.bits &&
                  writeGeneralLockedSurface(surface.surface, locked,
                                             blob->bytes);
    if (locked.bits) {
      copied = (dxmt9c_texture_unlock_rect(stagingTexture, 0u) ==
                core::D3D_OK) && copied;
    }
    auto* staging = copied
        ? dxmt9c_texture_get_surface_level(stagingTexture, 0u)
        : nullptr;
    copied = copied && staging && staging->obj;
    if (copied) {
      const core::Rect extent{0, 0, static_cast<core::i32>(surface.surface.width),
                              static_cast<core::i32>(surface.surface.height)};
      upper->submitSurfaceCopy(core::SurfaceCopyDesc{
          .source = staging->obj->handle(),
          .destination = wrapped->obj->handle(),
          .sourceRect = extent,
          .destinationRect = extent,
      });
      upper->flush();
      upper->queue().markColorHandleTouched(wrapped->obj->handle());
    }
    if (staging) dxmt9c_surface_release(staging);
    dxmt9c_texture_release(stagingTexture);
    return copied;
  }
  D9CLockedRect locked{};
  if (dxmt9c_surface_lock_rect(wrapped, &locked, nullptr, 0u) != core::D3D_OK ||
      !locked.bits) {
    return false;
  }
  const bool copied =
      writeGeneralLockedSurface(surface.surface, locked, blob->bytes);
  const bool unlocked = dxmt9c_surface_unlock_rect(wrapped) == core::D3D_OK;
  return copied && unlocked;
}

struct SeededSubresource {
  D9CWireObjectIdentity identity{};
  std::uint32_t subresource = 0u;
};

void restoreSeededColorAttachmentTouches(
    D9CDevice* device, const std::vector<Definition>& definitions,
    std::vector<OwnedObject>& objects,
    std::span<const SeededSubresource> seeded) {
  auto upper = device ? device->dev().upperDevice() : nullptr;
  if (!upper) return;
  for (const auto& definition : definitions) {
    if (definition.fixed.identity.kind != D9C_CHUNK_HANDLE_KIND_SURFACE)
      continue;
    RenderTapeSurfaceDescriptorV2 surface{};
    if (!renderTapeLoadSurfaceDescriptorV2(definition.descriptor, surface) ||
        (surface.surface.usage & 1u) == 0u) {
      continue;
    }
    const auto storage = static_cast<RenderTapeSurfaceStorage>(surface.storage);
    const bool hasSeed = std::any_of(
        seeded.begin(), seeded.end(), [&](const auto& value) {
          if (storage == RenderTapeSurfaceStorage::Standalone) {
            return value.subresource == 0u &&
                   sameIdentity(value.identity, definition.fixed.identity);
          }
          if (storage == RenderTapeSurfaceStorage::SwapchainBackbuffer) {
            return value.subresource == 0u &&
                   sameIdentity(value.identity, definition.fixed.identity);
          }
          return storage == RenderTapeSurfaceStorage::TextureSubresource &&
                 value.subresource == surface.subresource &&
                 sameIdentity(value.identity, surface.parentTexture);
        });
    if (!hasSeed) continue;
    auto* wrapped = static_cast<D9CSurface*>(
        findObject(objects, definition.fixed.identity));
    if (wrapped && wrapped->obj) {
      upper->queue().markColorHandleTouched(wrapped->obj->handle());
    }
  }
}

bool chunkContainsPresent(const Chunk& chunk) {
  ImportedChunkView imported;
  if (!importPrevalidatedCommandChunk(chunk.bytes, chunk.envelope, imported))
    return false;
  return std::any_of(imported.records.begin(), imported.records.end(),
                     [](const auto& record) {
                       return record.type == D9C_COMMAND_RECORD_PRESENT;
                     });
}

FrameTapeReplayResult replayGeneralPlan(
    D9CDevice* device, GeneralPreflightPlan plan,
    FrameTapeReplayResult result,
    std::span<const RenderTapeProviderBlob> blobs) {
  if (!device) {
    result.status = FrameTapeReplayStatus::ObjectCreationFailed;
    return result;
  }
  std::vector<OwnedObject> objects;
  std::vector<Definition> definitions;
  std::vector<Chunk> bootstrap;
  std::vector<void*> resolved;
  std::shared_ptr<OffscreenPresentOutput> output;
  D9CSurface* readbackTarget = nullptr;
  bool bootstrapReplayed = false;
  bool outputInstalled = false;
  bool sawSubmittedCommandWork = false;
  std::vector<SeededSubresource> seededSubresources;
  auto replayUpper = device->dev().upperDevice();
  const bool priorExactAttachmentPreservation =
      replayUpper &&
      replayUpper->queue().renderTapeExactAttachmentPreservation();
  if (replayUpper) {
    replayUpper->queue().setRenderTapeExactAttachmentPreservation(true);
  }
  const auto cleanup = [&] {
    if (auto upper = device->dev().upperDevice()) upper->flush();
    if (replayUpper) {
      replayUpper->queue().setRenderTapeExactAttachmentPreservation(
          priorExactAttachmentPreservation);
    }
    if (outputInstalled) {
      if (auto* swap = device->iface->GetSwapChain(0u)) {
        swap->coreSwapChain().restoreWindowPresenter();
        swap->Release();
      }
      outputInstalled = false;
    }
    if (readbackTarget) {
      dxmt9c_surface_release(readbackTarget);
      readbackTarget = nullptr;
    }
    for (auto& object : objects) {
      if (!object.value) continue;
      releaseObject(object);
      ++result.conservation.objectsReleased;
    }
  };
  const auto fail = [&](FrameTapeReplayStatus status,
                        std::uint32_t eventIndex) {
    result.status = status;
    result.failedEventIndex = eventIndex;
    cleanup();
    return result;
  };
  const auto installOutput = [&] {
    if (outputInstalled) return true;
    auto upper = device->dev().upperDevice();
    if (!upper || !upper->pool()) return true;
    readbackTarget = dxmt9c_device_create_render_target(
        device, plan.outputDesc.width, plan.outputDesc.height,
        plan.outputDesc.format, 0u, 0u, 0u, nullptr);
    auto* record = readbackTarget
        ? upper->pool()->findSurface(readbackTarget->obj->handle().value)
        : nullptr;
    auto* swap = device->iface->GetSwapChain(0u);
    if (!record || !record->texture || !swap) {
      if (swap) swap->Release();
      return false;
    }
    output = std::make_shared<OffscreenPresentOutput>(
        WMT::Texture{record->texture.handle}, plan.outputDesc.width,
        plan.outputDesc.height);
    outputInstalled = swap->coreSwapChain().installPresentOutput(output);
    swap->Release();
    return outputInstalled;
  };
  const auto replayChunk = [&](const Chunk& chunk) {
    ImportedChunkView imported;
    if (!importPrevalidatedCommandChunk(chunk.bytes, chunk.envelope, imported) ||
        !resolveChunk(chunk, objects, resolved)) {
      return false;
    }
    for (std::size_t recordIndex = 0u;
         recordIndex < imported.records.size(); ++recordIndex) {
      const auto record = imported.record(recordIndex);
      if (record.header.type != D9C_COMMAND_RECORD_PRESENT) continue;
      D9CCommandChunkWirePresent present{};
      if (!load(record.payload, 0u, present) || record.header.handleCount > 1u ||
          !presentGeometrySupported(present, plan.outputDesc)) {
        return false;
      }
      if (record.header.handleCount == 1u) {
        if (present.sourceHandleIndex >= imported.handles.size() ||
            present.sourceHandleIndex >= resolved.size()) {
          return false;
        }
        const auto& handle = imported.handles[present.sourceHandleIndex];
        const D9CWireObjectIdentity identity{
            .kind = handle.kind, .generation = handle.generation,
            .objectId = handle.objectId};
        if (!sameIdentity(identity, plan.outputIdentity)) return false;
        auto* surface =
            static_cast<D9CSurface*>(resolved[present.sourceHandleIndex]);
        auto* swap = device->iface->GetSwapChain(0u);
        const bool same = surface && surface->obj && swap &&
                          surface->obj == swap->backBuffer();
        if (swap) swap->Release();
        if (!same) return false;
      }
    }
    return replayPrevalidatedResolvedCommandChunk(
               device, chunk.bytes, chunk.envelope, resolved) == core::D3D_OK;
  };

  for (std::uint32_t eventIndex = 0u;
       eventIndex < plan.tape.events.size(); ++eventIndex) {
    const auto event = plan.tape.event(eventIndex);
    switch (static_cast<RenderTapeEventType>(event.header.type)) {
    case RenderTapeEventType::BootstrapState: {
      RenderTapeBootstrapHeader fixed{};
      if (!load(event.payload, 0u, fixed) ||
          !parseChunks(event.payload.subspan(sizeof(fixed)), fixed.overlayCount,
                       bootstrap, fixed.gammaRampBytes)) {
        return fail(FrameTapeReplayStatus::BootstrapFailed, eventIndex);
      }
      if (fixed.gammaRampBytes != 0u) {
        const auto gamma = event.payload.subspan(
            event.payload.size() - fixed.gammaRampBytes, fixed.gammaRampBytes);
        if (!applyGammaRamp(device, gamma))
          return fail(FrameTapeReplayStatus::BootstrapFailed, eventIndex);
      }
      break;
    }
    case RenderTapeEventType::ObjectDefine: {
      RenderTapeObjectDefineHeader fixed{};
      if (!load(event.payload, 0u, fixed))
        return fail(FrameTapeReplayStatus::ObjectCreationFailed, eventIndex);
      const auto descriptor = event.payload.subspan(sizeof(fixed));
      void* value = createGeneralObject(device, fixed, descriptor, blobs, objects);
      if (!value)
        return fail(FrameTapeReplayStatus::ObjectCreationFailed, eventIndex);
      definitions.push_back({.fixed = fixed, .descriptor = descriptor});
      objects.push_back({.identity = fixed.identity, .value = value});
      ++result.conservation.objectsCreated;
      break;
    }
    case RenderTapeEventType::ObjectDestroy: {
      RenderTapeObjectDestroyHeader fixed{};
      if (!bootstrapReplayed) {
        // Bootstrap is a real state application, not metadata. Materialize it
        // before retiring any identity it may reference, even when the first
        // command chunk has not arrived yet.
        for (const auto& overlay : bootstrap) {
          if (!replayChunk(overlay))
            return fail(FrameTapeReplayStatus::BootstrapFailed, eventIndex);
        }
        bootstrapReplayed = true;
      }
      if (renderTapeProviderEventRequiresDrain(
              sawSubmittedCommandWork, RenderTapeEventType::ObjectDestroy)) {
        if (auto upper = device->dev().upperDevice()) upper->flush();
      }
      if (!load(event.payload, 0u, fixed))
        return fail(FrameTapeReplayStatus::ObjectCreationFailed, eventIndex);
      const auto object = std::find_if(
          objects.begin(), objects.end(), [&](const auto& value) {
            return value.value && sameIdentity(value.identity, fixed.identity);
          });
      if (object == objects.end())
        return fail(FrameTapeReplayStatus::ObjectCreationFailed, eventIndex);
      releaseObject(*object);
      ++result.conservation.objectsReleased;
      break;
    }
    case RenderTapeEventType::ResourceMutation: {
      RenderTapeResourceMutationHeader fixed{};
      if (renderTapeProviderEventRequiresDrain(
              sawSubmittedCommandWork, RenderTapeEventType::ResourceMutation)) {
        if (auto upper = device->dev().upperDevice()) upper->flush();
      }
      if (!load(event.payload, 0u, fixed) ||
          !applyGeneralMutation(device, fixed, blobs, definitions, objects)) {
        return fail(FrameTapeReplayStatus::MutationFailed, eventIndex);
      }
      seededSubresources.push_back(
          {.identity = fixed.identity, .subresource = fixed.subresource});
      restoreSeededColorAttachmentTouches(
          device, definitions, objects, seededSubresources);
      break;
    }
    case RenderTapeEventType::CommandChunk: {
      RenderTapeCommandChunkHeader fixed{};
      if (!load(event.payload, 0u, fixed))
        return fail(FrameTapeReplayStatus::CommandReplayFailed, eventIndex);
      const Chunk chunk{
          .envelope = CommandChunkEnvelope{fixed.wireVersion,
                                            fixed.recordCount,
                                            fixed.handleCount},
          .bytes = event.payload.subspan(sizeof(fixed)),
      };
      if (!bootstrapReplayed) {
        restoreSeededColorAttachmentTouches(
            device, definitions, objects, seededSubresources);
        for (const auto& overlay : bootstrap) {
          if (!replayChunk(overlay))
            return fail(FrameTapeReplayStatus::BootstrapFailed, eventIndex);
        }
        bootstrapReplayed = true;
      }
      if (chunkContainsPresent(chunk) && !installOutput())
        return fail(FrameTapeReplayStatus::PresentOutputFailed, eventIndex);
      if (!replayChunk(chunk))
        return fail(FrameTapeReplayStatus::CommandReplayFailed, eventIndex);
      sawSubmittedCommandWork = true;
      break;
    }
    case RenderTapeEventType::PresentComplete: {
      auto upper = device->dev().upperDevice();
      auto& evidence = result.intervals[0].validity;
      evidence.expectedDigestCaptured =
          plan.expectedDigestValidity == RenderTapeDigestValidity::Sha256;
      if (upper && readbackTarget) {
        upper->flush();
        core::ReadbackPixels pixels;
        if (!upper->readbackSurface(
                core::ReadbackDesc{.source = readbackTarget->obj->handle()},
                pixels)) {
          return fail(FrameTapeReplayStatus::ReadbackFailed, eventIndex);
        }
        constexpr std::uint64_t bytesPerPixel = 4u;
        std::uint64_t tightPitch = 0u;
        std::uint64_t tightBytes = 0u;
        std::uint64_t pitchedBytes = 0u;
        if (!checkedMul(plan.outputDesc.width, bytesPerPixel, tightPitch) ||
            !checkedMul(tightPitch, plan.outputDesc.height, tightBytes) ||
            !checkedMul(pixels.pitch, plan.outputDesc.height, pitchedBytes) ||
            pixels.pitch < tightPitch ||
            tightBytes > std::numeric_limits<std::size_t>::max() ||
            pixels.bytes.size() < pitchedBytes) {
          return fail(FrameTapeReplayStatus::ReadbackFailed, eventIndex);
        }
        std::vector<std::byte> tight(static_cast<std::size_t>(tightBytes));
        for (std::uint32_t row = 0u; row < plan.outputDesc.height; ++row) {
          std::memcpy(
              tight.data() + static_cast<std::size_t>(row * tightPitch),
              pixels.bytes.data() + static_cast<std::size_t>(row) * pixels.pitch,
              static_cast<std::size_t>(tightPitch));
        }
        evidence.outputReadback = true;
        evidence.outputBytes = tight.size();
        evidence.outputDigest = RenderTapeCaptureSession::sha256(tight);
        if (evidence.expectedDigestCaptured) {
          evidence.expectedOutputDigest = plan.expectedDigest;
        }
        evidence.expectedDigestMatched =
            evidence.expectedDigestCaptured &&
            sameDigest(evidence.outputDigest, plan.expectedDigest);
        const auto pixelSize = std::min<std::size_t>(4u, tight.size());
        for (std::size_t index = pixelSize; index < tight.size(); ++index) {
          if (tight[index] != tight[index % pixelSize]) {
            evidence.outputNonDegenerate = true;
            break;
          }
        }
        result.outputPixels = std::move(tight);
      }
      if (evidence.expectedDigestCaptured && !evidence.expectedDigestMatched) {
        result.validity = evidence;
        result.validity.structurallyValid = true;
        result.validity.digestsValid = true;
        return fail(FrameTapeReplayStatus::OutputMismatch, eventIndex);
      }
      break;
    }
    case RenderTapeEventType::OrderedControl: {
      RenderTapeOrderedControlHeader fixed{};
      if (!load(event.payload, 0u, fixed) ||
          fixed.kind != static_cast<std::uint32_t>(
              RenderTapeControlKind::GammaRampSet) ||
          fixed.controlBytes != kRenderTapeGammaRampBytes ||
          event.payload.size() != sizeof(fixed) + kRenderTapeGammaRampBytes) {
        return fail(FrameTapeReplayStatus::UnsupportedGrammar, eventIndex);
      }
      const auto gamma = event.payload.subspan(sizeof(fixed));
      if (!applyGammaRamp(device, gamma))
        return fail(FrameTapeReplayStatus::UnsupportedGrammar, eventIndex);
      break;
    }
    }
  }

  if (!bootstrapReplayed || (output && output->scheduledCount() != 1u)) {
    result.status = FrameTapeReplayStatus::PresentOutputFailed;
  }
  result.validity = result.intervals[0].validity;
  result.validity.structurallyValid = true;
  result.validity.digestsValid = true;
  cleanup();
  return result;
}

} // namespace

bool renderTapeProviderEventRequiresDrain(
    bool submittedCommandWork, RenderTapeEventType event) noexcept {
  return submittedCommandWork &&
      (event == RenderTapeEventType::ResourceMutation ||
       event == RenderTapeEventType::ObjectDestroy);
}

FrameTapeReplayResult preflightRenderTapeIdentity(
    std::span<const std::byte> tape,
    std::span<const RenderTapeProviderBlob> blobs) noexcept {
  auto bounded = buildPlan(tape, blobs, nullptr);
  if (bounded.complete() ||
      bounded.status != FrameTapeReplayStatus::UnsupportedGrammar) {
    return bounded;
  }
  return buildGeneralPlan(tape, blobs, nullptr);
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
  if (!result.complete()) {
    if (result.status != FrameTapeReplayStatus::UnsupportedGrammar)
      return result;
    GeneralPreflightPlan generalPlan;
    auto generalResult = buildGeneralPlan(tape, blobs, &generalPlan);
    if (!generalResult.complete()) return generalResult;
    return replayGeneralPlan(device, std::move(generalPlan), generalResult,
                             blobs);
  }
  if (!device) {
    result.status = FrameTapeReplayStatus::ObjectCreationFailed;
    return result;
  }

  std::vector<OwnedObject> objects;
  std::shared_ptr<OffscreenPresentOutput> output;
  D9CSurface* readbackTarget = nullptr;
  bool outputInstalled = false;
  auto replayUpper = device->dev().upperDevice();
  const bool priorExactAttachmentPreservation =
      replayUpper &&
      replayUpper->queue().renderTapeExactAttachmentPreservation();
  if (replayUpper) {
    replayUpper->queue().setRenderTapeExactAttachmentPreservation(true);
  }
  const auto cleanup = [&] {
    if (auto upper = device->dev().upperDevice()) upper->flush();
    if (replayUpper) {
      replayUpper->queue().setRenderTapeExactAttachmentPreservation(
          priorExactAttachmentPreservation);
    }
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
      } else if (surface.storage == static_cast<std::uint32_t>(
                     RenderTapeSurfaceStorage::SwapchainBackbuffer)) {
        auto* swap = device->iface->GetSwapChain(0u);
        if (swap) {
          value = new D9CSurface{swap->backBuffer(), nullptr, 0u, device};
          swap->Release();
        }
      } else if (surface.storage == static_cast<std::uint32_t>(
                     RenderTapeSurfaceStorage::Standalone) &&
                 (surface.initialContentDisposition ==
                      static_cast<std::uint32_t>(
                          RenderTapeInitialContentDisposition::
                              ProducedByCapturedPass) ||
                  surface.initialContentDisposition ==
                      static_cast<std::uint32_t>(
                          RenderTapeInitialContentDisposition::
                              CompleteDepthFloat32V1) ||
                  surface.initialContentDisposition ==
                      static_cast<std::uint32_t>(
                          RenderTapeInitialContentDisposition::
                              CompleteSeed))) {
        value = surface.surface.usage == 1u
            ? static_cast<void*>(dxmt9c_device_create_render_target(
                  device, surface.surface.width, surface.surface.height,
                  surface.surface.format, surface.surface.multiSampleType,
                  surface.surface.multiSampleQuality, 0u, nullptr))
            : static_cast<void*>(dxmt9c_device_create_depth_stencil(
                  device, surface.surface.width, surface.surface.height,
                  surface.surface.format, surface.surface.multiSampleType,
                  surface.surface.multiSampleQuality, 0u, nullptr));
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
      value = desc.dimension == static_cast<std::uint32_t>(
                                    RenderTapeTextureDimension::Cube)
          ? static_cast<void*>(dxmt9c_device_create_cube_texture(
                device, level0.width, desc.mipLevelCount, level0.usage,
                level0.format, level0.pool))
          : static_cast<void*>(dxmt9c_device_create_texture(
                device, level0.width, level0.height, desc.mipLevelCount,
                level0.usage, level0.format, level0.pool));
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
        const auto definition = std::find_if(
            plan.definitions.begin(), plan.definitions.end(),
            [&](const auto& value) {
              return sameIdentity(value.fixed.identity, mutation.identity);
            });
        ok = definition != plan.definitions.end() &&
             renderTapeTextureSubresourceDescriptor(
                 definition->descriptor, mutation.subresource, levelDesc) &&
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
      } else if (ok &&
                 mutation.identity.kind == D9C_CHUNK_HANDLE_KIND_SURFACE) {
        const auto definition = std::find_if(
            plan.definitions.begin(), plan.definitions.end(),
            [&](const auto& value) {
              return sameIdentity(value.fixed.identity, mutation.identity);
            });
        RenderTapeSurfaceDescriptorV2 surface{};
        std::uint64_t pixelCount = 0u;
        std::uint64_t tightBytes = 0u;
        auto* wrapped = static_cast<D9CSurface*>(object);
        auto upper = device->dev().upperDevice();
        ok = definition != plan.definitions.end() &&
             load(definition->descriptor, 0u, surface) &&
             surface.storage == static_cast<std::uint32_t>(
                 RenderTapeSurfaceStorage::Standalone) &&
             mutation.subresource == 0u && mutation.byteOffset == 0u &&
             checkedMul(surface.surface.width, surface.surface.height,
                        pixelCount) &&
             checkedMul(pixelCount, 4u, tightBytes) &&
             blob->bytes.size() == tightBytes && wrapped && wrapped->obj;
        const bool depthSnapshot = ok &&
            surface.initialContentDisposition ==
                static_cast<std::uint32_t>(
                    RenderTapeInitialContentDisposition::
                        CompleteDepthFloat32V1) &&
            renderTapeSnapshotStandaloneD24X8Supported(surface.surface);
        const bool colorSnapshot = ok &&
            surface.initialContentDisposition ==
                static_cast<std::uint32_t>(
                    RenderTapeInitialContentDisposition::CompleteSeed) &&
            renderTapeArmColorSnapshotStandaloneSurfaceSupported(
                surface.surface);
        if (depthSnapshot && upper) {
          core::CanonicalD24X8Depth canonical{
              .bytes = std::vector<core::u8>(blob->bytes.size()),
              .version = core::kCanonicalD24X8DepthVersion1,
              .width = surface.surface.width,
              .height = surface.surface.height,
              .pitch = surface.surface.width * 4u,
              .physicalFormat = static_cast<core::u32>(convert::toPixelFormat(
                  core::Format::D24X8, upper->limits())),
          };
          std::memcpy(canonical.bytes.data(), blob->bytes.data(),
                      blob->bytes.size());
          ok = upper->seedCanonicalD24X8Depth(wrapped->obj->handle(),
                                               canonical);
        } else if (colorSnapshot) {
          D9CLockedRect locked{};
          ok = dxmt9c_surface_lock_rect(wrapped, &locked, nullptr, 0u) ==
                   core::D3D_OK &&
               locked.bits;
          RenderTapeLinearLockLayout layout{};
          if (ok &&
              (renderTapeLinearLockLayout(surface.surface, locked.pitch,
                                          nullptr, layout) !=
                   RenderTapeLinearLayoutStatus::Accepted ||
               blob->bytes.size() != layout.tightBytes ||
               !writeRenderTapeLinearRows(blob->bytes, locked.bits, layout))) {
            (void)dxmt9c_surface_unlock_rect(wrapped);
            ok = false;
          } else if (ok) {
            ok = dxmt9c_surface_unlock_rect(wrapped) == core::D3D_OK;
          }
        } else {
          ok = false;
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

  // Seed all prior-boundary content before BootstrapState can bind it.
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
      evidence.expectedOutputDigest = interval.expectedDigest;
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
    result.outputPixels = std::move(tight);
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

bool applyRenderTapePixelOracleEnvelope(
    FrameTapeReplayResult& result,
    std::span<const std::byte> expectedPixels) noexcept {
  if (result.status != FrameTapeReplayStatus::OutputMismatch ||
      result.profile != kRenderTapeProfileFrame ||
      result.intervalCount != 1u || !result.validity.structurallyValid ||
      !result.validity.digestsValid || !result.validity.outputReadback ||
      !result.validity.expectedDigestCaptured ||
      result.validity.expectedDigestMatched || expectedPixels.empty() ||
      result.coverage.commandChunks == 0u ||
      result.coverage.commandRecords == 0u ||
      result.coverage.presentRecords != 1u ||
      result.coverage.presentSourceMappings != 1u ||
      result.coverage.presentOutputs != 1u ||
      result.conservation.inputBlobs !=
          result.conservation.referencedBlobs ||
      result.conservation.objectsCreated !=
          result.conservation.objectsReleased ||
      result.conservation.presentOrdinal == 0u ||
      result.conservation.completionOrdinal == 0u ||
      expectedPixels.size() != result.outputPixels.size() ||
      expectedPixels.size() != result.validity.outputBytes ||
      !sameDigest(RenderTapeCaptureSession::sha256(expectedPixels),
                  result.validity.expectedOutputDigest)) {
    return false;
  }

  std::uint64_t pixelCount = 0u;
  std::uint64_t expectedBytes = 0u;
  if (!checkedMul(result.requirements.outputWidth,
                  result.requirements.outputHeight, pixelCount) ||
      !checkedMul(pixelCount, 4u, expectedBytes) ||
      expectedBytes != expectedPixels.size()) {
    return false;
  }

  auto& validity = result.validity;
  validity.expectedPixelsCompared = true;
  for (std::uint64_t pixel = 0u; pixel < pixelCount; ++pixel) {
    const auto offset = static_cast<std::size_t>(pixel * 4u);
    bool differs = false;
    for (std::size_t channel = 0u; channel < 3u; ++channel) {
      const auto expected = std::to_integer<std::uint32_t>(
          expectedPixels[offset + channel]);
      const auto actual = std::to_integer<std::uint32_t>(
          result.outputPixels[offset + channel]);
      const auto delta = expected > actual ? expected - actual
                                           : actual - expected;
      differs = differs || delta != 0u;
      validity.totalRgbDelta += delta;
      validity.maxRgbDelta = std::max(validity.maxRgbDelta, delta);
    }
    if (expectedPixels[offset + 3u] != result.outputPixels[offset + 3u]) {
      differs = true;
      ++validity.differingAlphaPixels;
    }
    if (differs) {
      ++validity.differingPixels;
    }
  }

  // Independent executions may land on adjacent 8-bit quantization values at
  // a tiny number of filtered/rasterized edge pixels. Keep the envelope much
  // narrower than a visible or structural rendering change: at most 1/12288
  // pixels, RGB delta <= 2, no alpha delta, and average aggregate RGB delta
  // <= 2 across the entire differing-pixel allowance.
  const auto allowedPixels = std::min<std::uint64_t>(
      64u, std::max<std::uint64_t>(1u, pixelCount / 12288u));
  validity.allowedDifferingPixels = allowedPixels;
  validity.pixelEnvelopeMatched =
      validity.differingPixels <= allowedPixels &&
      validity.maxRgbDelta <= 2u && validity.differingAlphaPixels == 0u &&
      validity.totalRgbDelta <= allowedPixels * 2u;
  result.intervals[0].validity = validity;
  if (!validity.pixelEnvelopeMatched) {
    return false;
  }
  result.status = FrameTapeReplayStatus::Complete;
  result.failedEventIndex = 0xffffffffu;
  return true;
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
