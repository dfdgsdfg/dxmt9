#include "device_c_render_tape_provider.hpp"

#include "device_c_chunk_replay.hpp"
#include "device_c_common.hpp"
#include "device_c_render_tape_capture.hpp"
#include "dxmt9/dxmt9_device.hpp"
#include "dxmt9/dxmt9_presenter.hpp"

#include <algorithm>
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

struct PreflightPlan {
  ImportedRenderTapeView tape{};
  RenderTapeBlobCatalogue catalogue{};
  std::vector<Definition> definitions{};
  std::vector<RenderTapeResourceMutationHeader> mutations{};
  std::vector<Chunk> bootstrap{};
  Chunk frame{};
  D9CWireObjectIdentity outputIdentity{};
  D9CSurfaceDesc outputDesc{};
  RenderTapeDigestValidity expectedDigestValidity =
      RenderTapeDigestValidity::NotCaptured;
  RenderTapeDigest expectedDigest{};
};

bool mutationCatalogueMatches(
    const PreflightPlan& plan,
    std::span<const RenderTapeProviderBlob> blobs) {
  for (const auto& mutation : plan.mutations) {
    const auto definition = std::find_if(
        plan.definitions.begin(), plan.definitions.end(),
        [&](const auto& value) {
          return sameIdentity(value.fixed.identity, mutation.identity);
        });
    const auto* blob = findBlob(blobs, mutation.digest);
    if (definition == plan.definitions.end() || !blob ||
        blob->bytes.size() != mutation.byteSize) {
      return false;
    }
  }
  return true;
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

bool acceptedTextureDescriptor(std::span<const std::byte> descriptor,
                               const RenderTapeTextureDescriptor& texture) {
  if (texture.levelCount == 0u || texture.level0.resourceType != 3u ||
      texture.level0.width == 0u || texture.level0.height == 0u ||
      texture.level0.depth != 1u || texture.level0.pool != 0u ||
      texture.level0.multiSampleType != 0u ||
      texture.level0.multiSampleQuality != 0u ||
      (texture.level0.usage & 3u) != 0u) return false;
  const auto format = devicec::fmtFromD3D(texture.level0.format);
  if (format == core::Format::Unknown || core::isCompressedFormat(format))
    return false;
  const std::size_t expectedBytes = sizeof(RenderTapeTextureDescriptor) +
      static_cast<std::size_t>(texture.levelCount - 1u) *
          sizeof(D9CSurfaceDesc);
  if (descriptor.size() != expectedBytes) return false;
  for (std::uint32_t level = 1u; level < texture.levelCount; ++level) {
    D9CSurfaceDesc desc{};
    if (!load(descriptor, sizeof(RenderTapeTextureDescriptor) +
                              (level - 1u) * sizeof(D9CSurfaceDesc), desc) ||
        desc.format != texture.level0.format || desc.resourceType != 3u ||
        desc.usage != texture.level0.usage || desc.pool != texture.level0.pool ||
        desc.multiSampleType != 0u || desc.multiSampleQuality != 0u ||
        desc.width != std::max(1u, texture.level0.width >> level) ||
        desc.height != std::max(1u, texture.level0.height >> level) ||
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
  result.coverage.eventCount = tape.header.eventCount;

  PreflightPlan candidate{.tape = tape, .catalogue = std::move(catalogue)};
  FrameTapeBootstrapOutputDisposition bootstrapDisposition =
      FrameTapeBootstrapOutputDisposition::Malformed;
  bool bootstrapAccepted = false;
  bool sawFrame = false;
  bool sawComplete = false;
  std::vector<RenderTapeDigest> referencedBlobs;
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
      if (!load(event.payload, 0u, fixed)) goto unsupported;
      const auto descriptor = event.payload.subspan(sizeof(fixed));
      if (fixed.payloadValidity != static_cast<std::uint32_t>(
                                       RenderTapeDigestValidity::NotCaptured) ||
          fixed.immutablePayloadBytes != 0u) goto unsupported;
      if (fixed.identity.kind == D9C_CHUNK_HANDLE_KIND_SURFACE) {
        D9CSurfaceDesc surface{};
        if (descriptor.size() != sizeof(surface) ||
            !load(descriptor, 0u, surface) || !acceptedSurface(surface) ||
            fixed.expectedContentBytes != 0u ||
            fixed.expectedContentCount != 0u ||
            candidate.outputIdentity.objectId != 0u) goto unsupported;
        candidate.outputIdentity = fixed.identity;
        candidate.outputDesc = surface;
      } else if (fixed.identity.kind == D9C_CHUNK_HANDLE_KIND_BUFFER) {
        D9CBufferDesc buffer{};
        if (descriptor.size() != sizeof(buffer) || !load(descriptor, 0u, buffer) ||
            buffer.size == 0u || fixed.expectedContentCount != 1u ||
            fixed.expectedContentBytes != buffer.size) goto unsupported;
      } else if (fixed.identity.kind == D9C_CHUNK_HANDLE_KIND_TEXTURE) {
        RenderTapeTextureDescriptor texture{};
        if (descriptor.size() < sizeof(texture) || !load(descriptor, 0u, texture) ||
            !acceptedTextureDescriptor(descriptor, texture) ||
            fixed.expectedContentCount != texture.levelCount ||
            fixed.expectedContentBytes == 0u) goto unsupported;
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
      candidate.mutations.push_back(fixed);
      ++result.coverage.seedMutations;
      if (std::none_of(referencedBlobs.begin(), referencedBlobs.end(),
                       [&](const auto& digest) {
                         return sameDigest(digest, fixed.digest);
                       })) {
        referencedBlobs.push_back(fixed.digest);
      }
      break;
    }
    case RenderTapeEventType::CommandChunk: {
      RenderTapeCommandChunkHeader fixed{};
      if (sawFrame || !load(event.payload, 0u, fixed)) goto unsupported;
      candidate.frame = Chunk{
          .envelope = CommandChunkEnvelope{fixed.wireVersion, fixed.recordCount,
                                            fixed.handleCount},
          .bytes = event.payload.subspan(sizeof(fixed)),
      };
      ImportedChunkView chunk;
      if (!importPrevalidatedCommandChunk(candidate.frame.bytes,
                                           candidate.frame.envelope, chunk) ||
          chunk.records.size() != 2u) goto unsupported;
      const auto clearRecord = chunk.record(0u);
      const auto presentRecord = chunk.record(1u);
      D9CCommandChunkWireClear clear{};
      D9CCommandChunkWirePresent present{};
      if (clearRecord.header.type != D9C_COMMAND_RECORD_CLEAR ||
          presentRecord.header.type != D9C_COMMAND_RECORD_PRESENT ||
          !load(clearRecord.payload, 0u, clear) ||
          !load(presentRecord.payload, 0u, present) ||
          clear.flags != 1u || clear.rectCount != 0u ||
          present.flags != 0u || present.reserved0 != 0u ||
          present.hasSrc != present.hasDst ||
          (present.hasSrc != 0u &&
           (!fullRect(present.src, candidate.outputDesc.width,
                      candidate.outputDesc.height) ||
            !fullRect(present.dst, candidate.outputDesc.width,
                      candidate.outputDesc.height)))) goto unsupported;
      sawFrame = true;
      result.coverage.commandChunks = 1u;
      result.coverage.commandRecords = 2u;
      result.coverage.clearRecords = 1u;
      result.coverage.presentRecords = 1u;
      break;
    }
    case RenderTapeEventType::PresentComplete: {
      RenderTapePresentCompleteHeader fixed{};
      RenderTapeOracleAttachment oracle{};
      if (!load(event.payload, 0u, fixed) || fixed.oracleCount != 1u ||
          !load(event.payload, sizeof(fixed), oracle) ||
          !sameIdentity(oracle.identity, candidate.outputIdentity)) goto unsupported;
      result.conservation.presentOrdinal = fixed.presentOrdinal;
      result.conservation.completionOrdinal = fixed.completionOrdinal;
      candidate.expectedDigestValidity =
          static_cast<RenderTapeDigestValidity>(fixed.digestValidity);
      candidate.expectedDigest = fixed.expectedDigest;
      result.coverage.presentOutputs = 1u;
      sawComplete = true;
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
  if (!sawFrame || !sawComplete || candidate.outputIdentity.objectId == 0u ||
      !bootstrapAccepted ||
      referencedBlobs.size() != blobs.size() ||
      !mutationCatalogueMatches(candidate, blobs)) goto unsupported;
  result.conservation.referencedBlobs =
      static_cast<std::uint32_t>(referencedBlobs.size());
  result.requirements = FrameTapeReplayRequirements{
      .outputWidth = candidate.outputDesc.width,
      .outputHeight = candidate.outputDesc.height,
      .outputFormat = candidate.outputDesc.format,
  };
  result.validity.expectedDigestCaptured =
      candidate.expectedDigestValidity == RenderTapeDigestValidity::Sha256;
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

FrameTapeReplayResult preflightFrameTapeIdentity(
    std::span<const std::byte> tape,
    std::span<const RenderTapeProviderBlob> blobs) noexcept {
  return buildPlan(tape, blobs, nullptr);
}

FrameTapeReplayResult replayFrameTapeIdentity(
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

  for (const auto& definition : plan.definitions) {
    void* value = nullptr;
    if (definition.fixed.identity.kind == D9C_CHUNK_HANDLE_KIND_SURFACE) {
      auto* swap = device->iface->GetSwapChain(0u);
      if (swap) {
        value = new D9CSurface{swap->backBuffer(), nullptr, 0u, device};
        swap->Release();
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
      RenderTapeTextureDescriptor desc{};
      load(definition.descriptor, 0u, desc);
      value = dxmt9c_device_create_texture(
          device, desc.level0.width, desc.level0.height, desc.levelCount,
          desc.level0.usage, desc.level0.format, desc.level0.pool);
    }
    if (!value) {
      result.status = FrameTapeReplayStatus::ObjectCreationFailed;
      cleanup();
      return result;
    }
    objects.push_back({.identity = definition.fixed.identity, .value = value});
    ++result.conservation.objectsCreated;
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

  for (const auto& mutation : plan.mutations) {
    const auto* blob = findBlob(blobs, mutation.digest);
    void* object = findObject(objects, mutation.identity);
    bool ok = blob && object;
    if (ok && mutation.identity.kind == D9C_CHUNK_HANDLE_KIND_BUFFER) {
      void* mapped = nullptr;
      auto* buffer = static_cast<D9CBuffer*>(object);
      ok = mutation.subresource == 0u &&
           dxmt9c_buffer_lock(buffer, 0u, static_cast<std::uint32_t>(blob->bytes.size()),
                              &mapped, 0u) == core::D3D_OK && mapped;
      if (ok) {
        std::memcpy(mapped, blob->bytes.data(), blob->bytes.size());
        ok = dxmt9c_buffer_unlock(buffer) == core::D3D_OK;
      }
    } else if (ok && mutation.identity.kind == D9C_CHUNK_HANDLE_KIND_TEXTURE) {
      D9CSurfaceDesc levelDesc{};
      D9CLockedRect locked{};
      auto* texture = static_cast<D9CTexture*>(object);
      ok = dxmt9c_texture_get_level_desc(texture, mutation.subresource,
                                         &levelDesc) == core::D3D_OK &&
           dxmt9c_texture_lock_rect(texture, mutation.subresource, &locked,
                                    nullptr, 0u) == core::D3D_OK &&
           locked.bits;
      if (ok && !renderTapeTextureSeedExtentMatches(
                    blob->bytes.size(), locked.pitch, levelDesc.height)) {
        (void)dxmt9c_texture_unlock_rect(texture, mutation.subresource);
        ok = false;
      } else if (ok) {
        std::memcpy(locked.bits, blob->bytes.data(), blob->bytes.size());
        ok = dxmt9c_texture_unlock_rect(texture, mutation.subresource) ==
             core::D3D_OK;
      }
    } else {
      ok = false;
    }
    if (!ok) {
      result.status = FrameTapeReplayStatus::MutationFailed;
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

  if (!resolveChunk(plan.frame, objects, resolved) ||
      replayPrevalidatedResolvedCommandChunk(
          device, plan.frame.bytes, plan.frame.envelope, resolved) != core::D3D_OK) {
    result.status = FrameTapeReplayStatus::CommandReplayFailed;
    cleanup();
    return result;
  }

  if (auto upper = device->dev().upperDevice(); upper && readbackTarget) {
    upper->flush();
    core::ReadbackPixels pixels;
    if (!upper->readbackSurface(
            core::ReadbackDesc{.source = readbackTarget->obj->handle()}, pixels)) {
      result.status = FrameTapeReplayStatus::ReadbackFailed;
      cleanup();
      return result;
    }
    result.validity.outputReadback = true;
    result.validity.outputBytes = pixels.bytes.size();
    result.validity.outputDigest = RenderTapeCaptureSession::sha256(
        std::as_bytes(std::span(pixels.bytes)));
    if (plan.expectedDigestValidity == RenderTapeDigestValidity::Sha256) {
      result.validity.expectedDigestMatched =
          sameDigest(result.validity.outputDigest, plan.expectedDigest);
      if (!result.validity.expectedDigestMatched) {
        result.status = FrameTapeReplayStatus::OutputMismatch;
      }
    }
    const auto pixelSize = std::min<std::size_t>(4u, pixels.bytes.size());
    result.validity.outputNonDegenerate = false;
    for (std::size_t index = pixelSize; index < pixels.bytes.size(); ++index) {
      if (pixels.bytes[index] != pixels.bytes[index % pixelSize]) {
        result.validity.outputNonDegenerate = true;
        break;
      }
    }
    if (output && output->scheduledCount() != 1u) {
      result.status = FrameTapeReplayStatus::PresentOutputFailed;
    }
  }
  cleanup();
  return result;
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
    return imported.handles.empty()
               ? FrameTapeBootstrapOutputDisposition::ImplicitDefault
               : FrameTapeBootstrapOutputDisposition::Ambiguous;
  }
  return FrameTapeBootstrapOutputDisposition::ExplicitExact;
}

bool renderTapeTextureSeedExtentMatches(std::uint64_t blobBytes,
                                        std::int32_t pitch,
                                        std::uint32_t mipHeight) noexcept {
  if (pitch <= 0 || mipHeight == 0u) return false;
  const auto rowPitch = static_cast<std::uint64_t>(pitch);
  if (rowPitch > std::numeric_limits<std::uint64_t>::max() / mipHeight) {
    return false;
  }
  return blobBytes == rowPitch * mipHeight;
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
