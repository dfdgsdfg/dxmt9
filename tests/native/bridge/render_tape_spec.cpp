#include "device_c_render_tape.hpp"
#include "device_c_chunk_registry.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace dxmt9::d3d9;

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void check(bool condition, std::string_view message) {
  if (!condition) {
    throw TestFailure(std::string(message));
  }
}

bool sameWireIdentity(const D9CWireObjectIdentity& lhs,
                      const D9CWireObjectIdentity& rhs) {
  return lhs.kind == rhs.kind && lhs.generation == rhs.generation &&
         lhs.objectId == rhs.objectId;
}

std::size_t alignUp(std::size_t value, std::size_t alignment) {
  return (value + alignment - 1u) & ~(alignment - 1u);
}

std::vector<std::byte> makeSingleRecordChunk(
    std::uint32_t type, std::span<const std::byte> payload,
    std::span<const D9CCommandChunkWireHandleEntry> handles = {}) {
  const auto recordTableOffset = sizeof(D9CCommandChunkWireHeader);
  const auto handleTableOffset = alignUp(
      recordTableOffset + sizeof(D9CCommandChunkWireRecordHeader),
      alignof(D9CCommandChunkWireHandleEntry));
  const auto payloadArenaOffset =
      alignUp(handleTableOffset +
                  handles.size() * sizeof(D9CCommandChunkWireHandleEntry),
              alignof(std::uint32_t));
  const D9CCommandChunkWireHeader header{
      .version = D9C_COMMAND_CHUNK_WIRE_VERSION,
      .headerSize = D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE,
      .recordHeaderSize = D9C_COMMAND_CHUNK_WIRE_RECORD_HEADER_SIZE,
      .handleEntrySize = D9C_COMMAND_CHUNK_WIRE_HANDLE_ENTRY_SIZE,
      .recordTableOffset = static_cast<std::uint32_t>(recordTableOffset),
      .recordCount = 1u,
      .handleTableOffset = static_cast<std::uint32_t>(handleTableOffset),
      .handleCount = static_cast<std::uint32_t>(handles.size()),
      .payloadArenaOffset = static_cast<std::uint32_t>(payloadArenaOffset),
      .payloadArenaSize = static_cast<std::uint32_t>(payload.size()),
  };
  const D9CCommandChunkWireRecordHeader record{
      .type = type,
      .payloadSize = static_cast<std::uint32_t>(payload.size()),
      .handleCount = static_cast<std::uint32_t>(handles.size()),
  };
  std::vector<std::byte> bytes(payloadArenaOffset + payload.size());
  std::memcpy(bytes.data(), &header, sizeof(header));
  std::memcpy(bytes.data() + recordTableOffset, &record, sizeof(record));
  if (!handles.empty()) {
    std::memcpy(bytes.data() + handleTableOffset, handles.data(),
                handles.size_bytes());
  }
  if (!payload.empty()) {
    std::memcpy(bytes.data() + payloadArenaOffset, payload.data(),
                payload.size());
  }
  return bytes;
}

std::vector<std::byte> makeApplyStateChunk(
    bool referenceHandle = false,
    D9CWireObjectIdentity referencedIdentity = {}) {
  std::array<D9CCommandChunkWireTextureBinding,
             D9C_DRAW_PACKET_MAX_TEXTURES>
      textures{};
  for (std::uint32_t slot = 0u; slot < textures.size(); ++slot) {
    textures[slot] = D9CCommandChunkWireTextureBinding{
        .slot = slot,
        .valid = 1u,
        .handleIndex = referenceHandle && slot == 0u
                           ? 0u
                           : D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX,
    };
  }
  std::array<D9CCommandChunkWireStreamBinding, D9C_DRAW_PACKET_MAX_STREAMS>
      streams{};
  for (std::uint32_t slot = 0u; slot < streams.size(); ++slot) {
    streams[slot] = D9CCommandChunkWireStreamBinding{
        .slot = slot,
        .valid = 1u,
        .handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX,
    };
  }

  constexpr std::uint32_t sectionCount = 2u;
  constexpr std::size_t textureBytes =
      sizeof(D9CCommandChunkWireTextureBinding) *
      D9C_DRAW_PACKET_MAX_TEXTURES;
  constexpr std::size_t streamBytes =
      sizeof(D9CCommandChunkWireStreamBinding) * D9C_DRAW_PACKET_MAX_STREAMS;
  const auto sectionTableOffset = sizeof(D9CCommandChunkWireDrawHeader);
  const auto sectionPayloadOffset =
      alignUp(sectionTableOffset +
                  sectionCount * sizeof(D9CCommandChunkWireSectionDesc),
              alignof(std::uint32_t));
  const auto streamOffset =
      sectionPayloadOffset + textureBytes;
  const D9CCommandChunkWireDrawHeader apply{
      .flags = D9C_COMMAND_CHUNK_DRAW_FLAG_FULL_SNAPSHOT,
      .sectionCount = sectionCount,
      .sectionTableOffset = static_cast<std::uint32_t>(sectionTableOffset),
      .sectionPayloadOffset = static_cast<std::uint32_t>(sectionPayloadOffset),
  };
  const std::array sections{
      D9CCommandChunkWireSectionDesc{
          .kind = D9C_COMMAND_CHUNK_SECTION_TEXTURE,
          .elementSize = sizeof(D9CCommandChunkWireTextureBinding),
          .count = static_cast<std::uint32_t>(textures.size()),
          .payloadOffset = static_cast<std::uint32_t>(sectionPayloadOffset),
          .byteSize = static_cast<std::uint32_t>(textureBytes),
      },
      D9CCommandChunkWireSectionDesc{
          .kind = D9C_COMMAND_CHUNK_SECTION_STREAM,
          .elementSize = sizeof(D9CCommandChunkWireStreamBinding),
          .count = static_cast<std::uint32_t>(streams.size()),
          .payloadOffset = static_cast<std::uint32_t>(streamOffset),
          .byteSize = static_cast<std::uint32_t>(streamBytes),
      },
  };
  std::vector<std::byte> payload(streamOffset + streamBytes);
  std::memcpy(payload.data(), &apply, sizeof(apply));
  std::memcpy(payload.data() + sectionTableOffset, sections.data(),
              sizeof(sections));
  std::memcpy(payload.data() + sectionPayloadOffset, textures.data(),
              textureBytes);
  std::memcpy(payload.data() + streamOffset, streams.data(),
              streamBytes);
  const D9CCommandChunkWireHandleEntry handle{
      .kind = referencedIdentity.kind,
      .generation = referencedIdentity.generation,
      .objectId = referencedIdentity.objectId,
  };
  return makeSingleRecordChunk(
      D9C_COMMAND_RECORD_APPLY_STATE, payload,
      referenceHandle ? std::span<const D9CCommandChunkWireHandleEntry>(&handle,
                                                                         1u)
                      : std::span<const D9CCommandChunkWireHandleEntry>{});
}

std::vector<std::byte> makePresentChunk() {
  const D9CCommandChunkWirePresent present{};
  return makeSingleRecordChunk(D9C_COMMAND_RECORD_PRESENT,
                               std::as_bytes(std::span(&present, 1u)));
}

std::vector<std::byte> makeMappedPresentChunk(
    D9CWireObjectIdentity sourceIdentity) {
  const D9CCommandChunkWirePresent present{
      .sourceHandleIndex = 0u,
  };
  const D9CCommandChunkWireHandleEntry source{
      .kind = sourceIdentity.kind,
      .generation = sourceIdentity.generation,
      .objectId = sourceIdentity.objectId,
  };
  return makeSingleRecordChunk(
      D9C_COMMAND_RECORD_PRESENT,
      std::as_bytes(std::span(&present, 1u)),
      std::span<const D9CCommandChunkWireHandleEntry>(&source, 1u));
}

std::vector<std::byte> makeUnboundDrawChunk() {
  auto chunk = makeApplyStateChunk();
  auto* wire = reinterpret_cast<D9CCommandChunkWireHeader*>(chunk.data());
  auto* record = reinterpret_cast<D9CCommandChunkWireRecordHeader*>(
      chunk.data() + wire->recordTableOffset);
  auto* draw = reinterpret_cast<D9CCommandChunkWireDrawHeader*>(
      chunk.data() + wire->payloadArenaOffset);
  record->type = D9C_COMMAND_RECORD_DRAW_PRIMITIVE;
  draw->flags = 0u;
  draw->primitiveType = 4u;
  draw->primitiveCount = 1u;
  return chunk;
}

std::vector<std::byte> makeDrawChunk(
    D9CWireObjectIdentity textureIdentity) {
  auto chunk = makeApplyStateChunk(true, textureIdentity);
  auto* wire = reinterpret_cast<D9CCommandChunkWireHeader*>(chunk.data());
  auto* record = reinterpret_cast<D9CCommandChunkWireRecordHeader*>(
      chunk.data() + wire->recordTableOffset);
  auto* draw = reinterpret_cast<D9CCommandChunkWireDrawHeader*>(
      chunk.data() + wire->payloadArenaOffset);
  record->type = D9C_COMMAND_RECORD_DRAW_PRIMITIVE;
  draw->flags = 0u;
  draw->primitiveType = 4u; // D3DPT_TRIANGLELIST
  draw->primitiveCount = 1u;
  return chunk;
}

constexpr D9CWireObjectIdentity kSurface{
    .kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
    .generation = 2u,
    .objectId = 17u,
};

constexpr D9CWireObjectIdentity kTexture{
    .kind = D9C_CHUNK_HANDLE_KIND_TEXTURE,
    .generation = 3u,
    .objectId = 29u,
};

constexpr D9CWireObjectIdentity kBuffer{
    .kind = D9C_CHUNK_HANDLE_KIND_BUFFER,
    .generation = 4u,
    .objectId = 31u,
};

constexpr std::uint32_t kSurfaceDescriptorKind = static_cast<std::uint32_t>(
    RenderTapeDescriptorKind::Surface);
constexpr std::uint32_t kTextureDescriptorKind = static_cast<std::uint32_t>(
    RenderTapeDescriptorKind::Texture);
constexpr std::uint32_t kShaderDescriptorKind = static_cast<std::uint32_t>(
    RenderTapeDescriptorKind::Shader);
constexpr std::uint32_t kBufferDescriptorKind = static_cast<std::uint32_t>(
    RenderTapeDescriptorKind::Buffer);

RenderTapeSurfaceDescriptorV2 outputSurfaceDescriptor() {
  return RenderTapeSurfaceDescriptorV2{
      .schemaVersion = kRenderTapeSurfaceDescriptorVersion2,
      .storage = static_cast<std::uint32_t>(
          RenderTapeSurfaceStorage::SwapchainBackbuffer),
      .initialContentDisposition = static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::ProducedPresentOutput),
      .surface = D9CSurfaceDesc{
          .format = 21u,
          .resourceType = 1u,
          .usage = 1u,
          .pool = 0u,
          .width = 4u,
          .height = 4u,
          .depth = 1u,
      },
  };
}

std::vector<std::byte> texture2DDescriptor(
    std::uint32_t mipLevelCount = 1u,
    RenderTapeInitialContentDisposition disposition =
        RenderTapeInitialContentDisposition::CompleteSeed) {
  const RenderTapeTextureDescriptorV2 header{
      .schemaVersion = kRenderTapeTextureDescriptorVersion2,
      .dimension = static_cast<std::uint32_t>(
          RenderTapeTextureDimension::Texture2D),
      .mipLevelCount = mipLevelCount,
      .subresourceCount = mipLevelCount,
      .initialContentDisposition = static_cast<std::uint32_t>(disposition),
  };
  std::vector<std::byte> descriptor(
      sizeof(header) + mipLevelCount * sizeof(D9CSurfaceDesc));
  std::memcpy(descriptor.data(), &header, sizeof(header));
  for (std::uint32_t mip = 0u; mip < mipLevelCount; ++mip) {
    const D9CSurfaceDesc level{
        .format = 21u,
        .resourceType = 3u,
        .pool = 0u,
        .width = std::max(1u, 4u >> mip),
        .height = std::max(1u, 4u >> mip),
        .depth = 1u,
    };
    std::memcpy(descriptor.data() + sizeof(header) +
                    mip * sizeof(D9CSurfaceDesc),
                &level, sizeof(level));
  }
  return descriptor;
}

RenderTapeDigest digest(std::byte seed) {
  RenderTapeDigest value{};
  for (std::size_t i = 0u; i < value.size(); ++i) {
    value[i] = static_cast<std::byte>(
        static_cast<unsigned>(seed) + static_cast<unsigned>(i));
  }
  return value;
}

RenderTapeDigest mutationDigest() {
  return RenderTapeDigest{
      std::byte{0x8d}, std::byte{0x70}, std::byte{0xd6}, std::byte{0x91},
      std::byte{0xc8}, std::byte{0x22}, std::byte{0xd5}, std::byte{0x56},
      std::byte{0x38}, std::byte{0xb6}, std::byte{0xe7}, std::byte{0xfd},
      std::byte{0x54}, std::byte{0xcd}, std::byte{0x94}, std::byte{0x17},
      std::byte{0x0c}, std::byte{0x87}, std::byte{0xd1}, std::byte{0x9e},
      std::byte{0xb1}, std::byte{0xf6}, std::byte{0x28}, std::byte{0xb7},
      std::byte{0x57}, std::byte{0x50}, std::byte{0x6e}, std::byte{0xde},
      std::byte{0x56}, std::byte{0x88}, std::byte{0xd2}, std::byte{0x97},
  };
}

std::vector<std::byte> makeCompleteTape(
    D9CWireObjectIdentity mutationIdentity = kSurface,
    std::uint64_t mutationOffset = 0u, std::uint64_t mutationBytes = 4u,
    std::uint32_t controlBytes = sizeof(RenderTapeFlushWaitControl),
    std::uint64_t controlCompletion = 10u,
    std::uint64_t presentCompletion = 10u,
    RenderTapeDigestValidity digestValidity =
        RenderTapeDigestValidity::NotCaptured,
    bool includeGammaRamp = false) {
  const auto bootstrap = makeApplyStateChunk();
  const auto present = makeMappedPresentChunk(kSurface);
  const auto descriptor = outputSurfaceDescriptor();
  const auto resourceDigest = mutationDigest();
  const RenderTapeFlushWaitControl wait{.waitedSeqId = 9u};
  const RenderTapeOrderedControlHeader control{
      .kind = static_cast<std::uint32_t>(RenderTapeControlKind::FlushWait),
      .disposition = static_cast<std::uint32_t>(
          RenderTapeControlDisposition::Completed),
      .controlBytes = controlBytes,
      .completionOrdinal = controlCompletion,
  };
  const RenderTapeOracleAttachment oracle{
      .identity = kSurface,
      .descriptorKind = static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
  };

  RenderTapeBuilder builder;
  std::array<std::byte, kRenderTapeGammaRampBytes> gammaRamp{};
  if (includeGammaRamp) {
    for (std::size_t i = 0u; i < gammaRamp.size() / sizeof(std::uint16_t);
         ++i) {
      const auto value = static_cast<std::uint16_t>((i % 256u) << 8u);
      std::memcpy(gammaRamp.data() + i * sizeof(value), &value, sizeof(value));
    }
  }
  builder.appendBootstrapState(
      bootstrap, includeGammaRamp ? std::span<const std::byte>(gammaRamp)
                                  : std::span<const std::byte>{});
  builder.appendObjectDefine(
      kSurface, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
      std::as_bytes(std::span(&descriptor, 1u)), 0u, {});
  builder.appendResourceMutation(mutationIdentity,
                                 RenderTapeMutationKind::CpuUnlock, 0u,
                                 mutationOffset, mutationBytes,
                                 resourceDigest);
  if (includeGammaRamp) {
    const RenderTapeOrderedControlHeader gammaControl{
        .kind = static_cast<std::uint32_t>(RenderTapeControlKind::GammaRampSet),
        .disposition = static_cast<std::uint32_t>(
            RenderTapeControlDisposition::Completed),
        .controlBytes = kRenderTapeGammaRampBytes,
        .completionOrdinal = controlCompletion,
    };
    builder.appendOrderedControl(gammaControl, gammaRamp);
  }
  builder.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 1u, .handleCount = 1u}, present);
  builder.appendOrderedControl(control, std::as_bytes(std::span(&wait, 1u)));
  builder.appendPresentComplete(
      includeGammaRamp ? 5u : 4u, presentCompletion, digestValidity,
      digestValidity == RenderTapeDigestValidity::Sha256
          ? digest(std::byte{0x40})
          : RenderTapeDigest{},
      std::as_bytes(std::span(&oracle, 1u)));
  return builder.seal();
}

RenderTapeBlobCatalogue completeCatalogue(std::uint64_t bytes = 4u,
                                          bool verified = true);

template <typename T>
T* eventPayload(std::vector<std::byte>& tape, std::size_t eventIndex);

void gammaRampBootstrapAndOrderedMutationAreTyped() {
  const auto tape = makeCompleteTape(
      kSurface, 0u, 4u, sizeof(RenderTapeFlushWaitControl), 10u, 12u,
      RenderTapeDigestValidity::NotCaptured, true);
  const auto catalogue = completeCatalogue();
  const auto valid = validateRenderTape(tape, catalogue);
  check(valid.valid(), "gamma bootstrap and ordered mutation must validate");

  auto bad = tape;
  auto *bootstrap = eventPayload<RenderTapeBootstrapHeader>(bad, 0u);
  bootstrap->gammaRampDigest[0] ^= std::byte{1u};
  check(validateRenderTape(bad, catalogue).status ==
            RenderTapeValidationStatus::InvalidBootstrapChunk,
        "gamma digest mismatch must fail closed");
}

void bufferMutationDispositionIsTypedAndBackwardCompatible() {
  const D9CBufferDesc bufferDescriptor{.size = 4u};
  const auto bufferDescriptorBytes =
      std::as_bytes(std::span(&bufferDescriptor, 1u));
  const RenderTapeOracleAttachment oracle{
      .identity = kSurface,
      .descriptorKind = kSurfaceDescriptorKind,
  };
  const auto seedDigest = mutationDigest();
  const RenderTapeBlobCatalogue catalogue{.blobs = {{
      .digest = seedDigest,
      .size = 4u,
      .verified = 1u,
  }}};

  RenderTapeBuilder builder;
  builder.appendBootstrapState(makeApplyStateChunk());
  const auto surfaceDescriptor = outputSurfaceDescriptor();
  builder.appendObjectDefine(
      kSurface, kSurfaceDescriptorKind,
      std::as_bytes(std::span(&surfaceDescriptor, 1u)), 0u, {});
  builder.appendObjectDefine(kBuffer, kBufferDescriptorKind,
                             bufferDescriptorBytes, 0u, {}, 4u, 1u);
  builder.appendResourceMutation(
      kBuffer, RenderTapeMutationKind::CpuUnlock, 0u, 0u, 4u, seedDigest,
      RenderTapeBufferMutationDisposition::NoOverwrite);
  builder.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 1u, .handleCount = 0u},
      makePresentChunk());
  builder.appendPresentComplete(
      5u, 1u, RenderTapeDigestValidity::NotCaptured, {},
      std::as_bytes(std::span(&oracle, 1u)));
  auto tape = builder.seal();
  check(validateRenderTape(tape, catalogue).valid(),
        "buffer lock disposition must validate for buffer CpuUnlock");
  check(eventPayload<RenderTapeResourceMutationHeader>(tape, 3u)
                ->bufferDisposition ==
            static_cast<std::uint32_t>(
                RenderTapeBufferMutationDisposition::NoOverwrite),
        "buffer lock disposition must occupy the fixed reserved field");

  auto oldBundle = tape;
  eventPayload<RenderTapeResourceMutationHeader>(oldBundle, 3u)
      ->bufferDisposition = static_cast<std::uint32_t>(
          RenderTapeBufferMutationDisposition::Plain);
  check(validateRenderTape(oldBundle, catalogue).valid(),
        "zero disposition must preserve old bundle compatibility");

  auto invalid = tape;
  eventPayload<RenderTapeResourceMutationHeader>(invalid, 3u)
      ->bufferDisposition = 99u;
  check(validateRenderTape(invalid, catalogue).status ==
            RenderTapeValidationStatus::InvalidMutationKind,
        "unknown buffer disposition must fail closed");

  auto nonBuffer = tape;
  auto *mutation = eventPayload<RenderTapeResourceMutationHeader>(nonBuffer, 3u);
  mutation->identity = kSurface;
  check(validateRenderTape(nonBuffer, catalogue).status ==
            RenderTapeValidationStatus::InvalidMutationKind,
        "non-buffer mutation must reject non-Plain disposition");
}

RenderTapeBlobCatalogue completeCatalogue(std::uint64_t bytes,
                                          bool verified) {
  return RenderTapeBlobCatalogue{.blobs = {{
                                      .digest = mutationDigest(),
                                      .size = bytes,
                                      .verified = verified ? 1u : 0u,
                                  }}};
}

void clearDrawPresentSourceMappingMatchesOracleExactly() {
  constexpr D9CWireObjectIdentity alternateOutput{
      .kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
      .generation = 9u,
      .objectId = 18u,
  };
  const auto bootstrap = makeApplyStateChunk();
  const D9CCommandChunkWireClear clear{
      .flags = 1u,
      .colorARGB = 0xff204060u,
      .z = 1.0f,
      .rectOffset = sizeof(D9CCommandChunkWireClear),
  };
  const auto clearChunk = makeSingleRecordChunk(
      D9C_COMMAND_RECORD_CLEAR, std::as_bytes(std::span(&clear, 1u)));
  const auto drawChunk = makeUnboundDrawChunk();
  const auto presentChunk = makeMappedPresentChunk(kSurface);
  const auto descriptor = outputSurfaceDescriptor();
  const RenderTapeOracleAttachment oracle{
      .identity = kSurface,
      .descriptorKind = kSurfaceDescriptorKind,
  };

  RenderTapeBuilder builder;
  builder.appendBootstrapState(bootstrap);
  builder.appendObjectDefine(kSurface, kSurfaceDescriptorKind,
                             std::as_bytes(std::span(&descriptor, 1u)), 0u,
                             {});
  builder.appendObjectDefine(alternateOutput, kSurfaceDescriptorKind,
                             std::as_bytes(std::span(&descriptor, 1u)), 0u,
                             {});
  builder.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 1u, .handleCount = 0u}, clearChunk);
  builder.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 1u, .handleCount = 0u}, drawChunk);
  builder.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 1u, .handleCount = 1u}, presentChunk);
  builder.appendPresentComplete(
      6u, 1u, RenderTapeDigestValidity::NotCaptured, {},
      std::as_bytes(std::span(&oracle, 1u)));
  const auto tape = builder.seal();
  check(validateRenderTape(tape, {}).valid(),
        "typed Clear-Draw-Present source mapping matches its oracle");

  RenderTapeBuilder mismatch;
  mismatch.appendBootstrapState(bootstrap);
  mismatch.appendObjectDefine(kSurface, kSurfaceDescriptorKind,
                              std::as_bytes(std::span(&descriptor, 1u)), 0u,
                              {});
  mismatch.appendObjectDefine(alternateOutput, kSurfaceDescriptorKind,
                              std::as_bytes(std::span(&descriptor, 1u)), 0u,
                              {});
  mismatch.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 1u, .handleCount = 0u}, clearChunk);
  mismatch.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 1u, .handleCount = 0u}, drawChunk);
  const auto wrongPresent = makeMappedPresentChunk(alternateOutput);
  mismatch.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 1u, .handleCount = 1u}, wrongPresent);
  mismatch.appendPresentComplete(
      6u, 1u, RenderTapeDigestValidity::NotCaptured, {},
      std::as_bytes(std::span(&oracle, 1u)));
  const auto rejected = validateRenderTape(mismatch.seal(), {});
  check(rejected.status == RenderTapeValidationStatus::InvalidPresentComplete,
        "generation-qualified Present source mismatch rejects oracle promotion");
}

template <typename T>
T* eventPayload(std::vector<std::byte>& tape, std::size_t eventIndex) {
  auto* header = reinterpret_cast<RenderTapeHeader*>(tape.data());
  auto* events = reinterpret_cast<RenderTapeEventHeader*>(
      tape.data() + header->eventTableOffset);
  return reinterpret_cast<T*>(tape.data() + header->payloadArenaOffset +
                              events[eventIndex].payloadOffset);
}

D9CCommandChunkWireDrawHeader* bootstrapDrawHeader(
    std::vector<std::byte>& tape) {
  auto* bootstrap = eventPayload<RenderTapeBootstrapHeader>(tape, 0u);
  auto* overlay = reinterpret_cast<std::byte*>(bootstrap + 1u);
  auto* chunk = reinterpret_cast<D9CCommandChunkWireHeader*>(overlay);
  return reinterpret_cast<D9CCommandChunkWireDrawHeader*>(
      overlay + chunk->payloadArenaOffset);
}

class RecordingSink final : public RenderTapeReplaySink {
public:
  bool bootstrap(const RenderTapeBootstrapHeader&,
                 std::span<const std::byte>,
                 RenderTapeBootstrapReplayMode mode) override {
    check(mode == RenderTapeBootstrapReplayMode::JournalOnlyDeferredProvider,
          "bootstrap must defer provider application explicitly");
    calls.push_back(RenderTapeEventType::BootstrapState);
    return true;
  }
  bool objectDefine(const RenderTapeObjectDefineHeader&,
                    std::span<const std::byte>) override {
    calls.push_back(RenderTapeEventType::ObjectDefine);
    return true;
  }
  bool objectDestroy(const RenderTapeObjectDestroyHeader&) override {
    calls.push_back(RenderTapeEventType::ObjectDestroy);
    return true;
  }
  bool resourceMutation(const RenderTapeResourceMutationHeader&) override {
    calls.push_back(RenderTapeEventType::ResourceMutation);
    return true;
  }
  bool commandChunk(const CommandChunkEnvelope& envelope,
                    std::span<const std::byte> chunk) override {
    calls.push_back(RenderTapeEventType::CommandChunk);
    return validateCommandChunk(chunk, envelope).valid();
  }
  bool orderedControl(const RenderTapeOrderedControlHeader&,
                      std::span<const std::byte>) override {
    calls.push_back(RenderTapeEventType::OrderedControl);
    return true;
  }
  bool presentComplete(const RenderTapePresentCompleteHeader&,
                       std::span<const std::byte>) override {
    calls.push_back(RenderTapeEventType::PresentComplete);
    return true;
  }

  std::vector<RenderTapeEventType> calls;
};

class BoundedRefinementSink final : public RenderTapeReplaySink {
public:
  bool bootstrap(const RenderTapeBootstrapHeader&,
                 std::span<const std::byte>,
                 RenderTapeBootstrapReplayMode) override {
    return live.empty() && mutations == 0u && draws == 0u && presents == 0u;
  }

  bool objectDefine(const RenderTapeObjectDefineHeader& fixed,
                    std::span<const std::byte>) override {
    live.push_back(fixed.identity);
    return true;
  }

  bool objectDestroy(const RenderTapeObjectDestroyHeader& fixed) override {
    const auto found = std::find_if(live.begin(), live.end(), [&](const auto& id) {
      return id.kind == fixed.identity.kind &&
             id.generation == fixed.identity.generation &&
             id.objectId == fixed.identity.objectId;
    });
    if (found == live.end()) return false;
    live.erase(found);
    return true;
  }

  bool resourceMutation(const RenderTapeResourceMutationHeader& fixed) override {
    const auto found = std::find_if(live.begin(), live.end(), [&](const auto& id) {
      return id.kind == fixed.identity.kind &&
             id.generation == fixed.identity.generation &&
             id.objectId == fixed.identity.objectId;
    });
    if (found == live.end()) return false;
    lastMutationDigest = fixed.digest;
    ++mutations;
    return true;
  }

  bool commandChunk(const CommandChunkEnvelope& envelope,
                    std::span<const std::byte> bytes) override {
    ImportedChunkView chunk;
    if (!validateCommandChunk(bytes, envelope, &chunk).valid()) return false;
    for (const auto& handle : chunk.handles) {
      const auto found = std::find_if(live.begin(), live.end(), [&](const auto& id) {
        return id.kind == handle.kind && id.generation == handle.generation &&
               id.objectId == handle.objectId;
      });
      if (found == live.end()) return false;
    }
    for (const auto& record : chunk.records) {
      switch (record.type) {
      case D9C_COMMAND_RECORD_DRAW_PRIMITIVE:
      case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE:
      case D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP:
      case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP:
        if (mutations != draws + 1u) return false;
        ++draws;
        break;
      case D9C_COMMAND_RECORD_PRESENT:
        if (presents >= draws) return false;
        ++presents;
        break;
      default:
        break;
      }
    }
    return true;
  }

  bool orderedControl(const RenderTapeOrderedControlHeader&,
                      std::span<const std::byte>) override {
    return false;
  }

  bool presentComplete(const RenderTapePresentCompleteHeader&,
                       std::span<const std::byte>) override {
    if (completions >= presents) return false;
    ++completions;
    return true;
  }

  std::vector<D9CWireObjectIdentity> live;
  RenderTapeDigest lastMutationDigest{};
  std::uint32_t mutations = 0u;
  std::uint32_t draws = 0u;
  std::uint32_t presents = 0u;
  std::uint32_t completions = 0u;
};

struct RefinementRegistryObject {
  std::uint32_t retains = 0u;
};

void retainRefinementObject(std::uint32_t, void* object) noexcept {
  ++static_cast<RefinementRegistryObject*>(object)->retains;
}

void validTapeReplaysExactlyOnce() {
  const auto tape = makeCompleteTape();
  const auto catalogue = completeCatalogue();
  ImportedRenderTapeView imported;
  const auto validation = validateRenderTape(tape, catalogue, &imported);
  check(validation.valid(), renderTapeValidationStatusName(validation.status));
  RecordingSink sink;
  const auto replay = replayPrevalidatedRenderTape(imported, catalogue, sink);
  check(replay.complete, "valid tape replay must complete");
  const std::array expected{
      RenderTapeEventType::BootstrapState,
      RenderTapeEventType::ObjectDefine,
      RenderTapeEventType::ResourceMutation,
      RenderTapeEventType::CommandChunk,
      RenderTapeEventType::OrderedControl,
      RenderTapeEventType::PresentComplete,
  };
  check(std::equal(sink.calls.begin(), sink.calls.end(), expected.begin(),
                   expected.end()),
        "events must replay exactly once in serial order");
}

void immutableObjectsAndRetirementFailClosed() {
  constexpr D9CWireObjectIdentity shader{
      .kind = D9C_CHUNK_HANDLE_KIND_SHADER,
      .generation = 1u,
      .objectId = 23u,
  };
  constexpr std::array<std::byte, 4u> descriptor{};
  const auto surfaceDescriptor = outputSurfaceDescriptor();
  const auto surfaceDescriptorBytes =
      std::as_bytes(std::span(&surfaceDescriptor, 1u));
  const auto shaderDigest = digest(std::byte{0x70});
  const auto bootstrap = makeApplyStateChunk();
  const auto present = makePresentChunk();
  const RenderTapeOracleAttachment oracle{
      .identity = kSurface,
      .descriptorKind = static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
  };

  RenderTapeBuilder immutable;
  immutable.appendBootstrapState(bootstrap);
  immutable.appendObjectDefine(
      kSurface, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
      surfaceDescriptorBytes, 0u, {});
  immutable.appendObjectDefine(shader, kShaderDescriptorKind, descriptor, 12u,
                               shaderDigest);
  immutable.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 1u, .handleCount = 0u}, present);
  immutable.appendPresentComplete(
      4u, 1u, RenderTapeDigestValidity::NotCaptured, {},
      std::as_bytes(std::span(&oracle, 1u)));
  const auto immutableTape = immutable.seal();
  const RenderTapeBlobCatalogue immutableCatalogue{.blobs = {{
      .digest = shaderDigest,
      .size = 12u,
      .verified = 1u,
  }}};
  check(validateRenderTape(immutableTape, immutableCatalogue).valid(),
        "verified shader payload definition must validate");
  check(validateRenderTape(immutableTape, {}).status ==
            RenderTapeValidationStatus::UnknownBlob,
        "shader payload definition without its blob must fail");

  RenderTapeBuilder reused;
  reused.appendBootstrapState(bootstrap);
  reused.appendObjectDefine(kSurface, kSurfaceDescriptorKind, surfaceDescriptorBytes, 0u,
                            {});
  reused.appendObjectDestroy(kSurface);
  auto newerSurface = kSurface;
  ++newerSurface.generation;
  reused.appendObjectDefine(newerSurface, kSurfaceDescriptorKind, surfaceDescriptorBytes,
                            0u, {});
  reused.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 1u, .handleCount = 0u}, present);
  const RenderTapeOracleAttachment newerOracle{
      .identity = newerSurface,
      .descriptorKind =
          static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
  };
  reused.appendPresentComplete(
      5u, 1u, RenderTapeDigestValidity::NotCaptured, {},
      std::as_bytes(std::span(&newerOracle, 1u)));
  check(validateRenderTape(reused.seal(), {}).valid(),
        "a destroyed slot admits a strictly newer generation");

  RenderTapeBuilder overlapping;
  overlapping.appendBootstrapState(bootstrap);
  overlapping.appendObjectDefine(kSurface, kSurfaceDescriptorKind, surfaceDescriptorBytes,
                                  0u, {});
  overlapping.appendObjectDefine(newerSurface, kSurfaceDescriptorKind,
                                  surfaceDescriptorBytes, 0u, {});
  overlapping.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 1u, .handleCount = 0u}, present);
  overlapping.appendPresentComplete(
      4u, 1u, RenderTapeDigestValidity::NotCaptured, {},
      std::as_bytes(std::span(&oracle, 1u)));
  check(validateRenderTape(overlapping.seal(), {}).status ==
            RenderTapeValidationStatus::DuplicateGeneration,
        "overlapping live generations of one slot fail closed");

  RenderTapeBuilder stale;
  stale.appendBootstrapState(bootstrap);
  stale.appendObjectDefine(newerSurface, kSurfaceDescriptorKind, surfaceDescriptorBytes,
                           0u, {});
  stale.appendObjectDestroy(newerSurface);
  stale.appendObjectDefine(kSurface, kSurfaceDescriptorKind, surfaceDescriptorBytes, 0u,
                           {});
  stale.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 1u, .handleCount = 0u}, present);
  stale.appendPresentComplete(
      5u, 1u, RenderTapeDigestValidity::NotCaptured, {},
      std::as_bytes(std::span(&oracle, 1u)));
  check(validateRenderTape(stale.seal(), {}).status ==
            RenderTapeValidationStatus::RetainedSlotReuse,
        "destroyed slots reject stale generation reintroduction");

  RenderTapeBuilder equal;
  equal.appendBootstrapState(bootstrap);
  equal.appendObjectDefine(kSurface, kSurfaceDescriptorKind, surfaceDescriptorBytes, 0u,
                           {});
  equal.appendObjectDestroy(kSurface);
  equal.appendObjectDefine(kSurface, kSurfaceDescriptorKind, surfaceDescriptorBytes, 0u,
                           {});
  equal.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 1u, .handleCount = 0u}, present);
  equal.appendPresentComplete(
      5u, 1u, RenderTapeDigestValidity::NotCaptured, {},
      std::as_bytes(std::span(&oracle, 1u)));
  check(validateRenderTape(equal.seal(), {}).status ==
            RenderTapeValidationStatus::DuplicateGeneration,
        "an exact identity definition remains unique after destroy");

  RenderTapeBuilder mismatched;
  mismatched.appendBootstrapState(bootstrap);
  mismatched.appendObjectDefine(kSurface, kSurfaceDescriptorKind, surfaceDescriptorBytes,
                                0u, {});
  mismatched.appendObjectDefine(kSurface, kTextureDescriptorKind, descriptor,
                                0u, {});
  mismatched.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 1u, .handleCount = 0u}, present);
  mismatched.appendPresentComplete(
      4u, 1u, RenderTapeDigestValidity::NotCaptured, {},
      std::as_bytes(std::span(&oracle, 1u)));
  check(validateRenderTape(mismatched.seal(), {}).status ==
            RenderTapeValidationStatus::InvalidObjectDefine,
        "a reused identity with a mismatched schema tag must fail closed");
}

void descriptorKindIdentityMismatchFailsClosed() {
  constexpr std::array<std::byte, 8u> descriptor{};
  RenderTapeBuilder mismatch;
  mismatch.appendBootstrapState(makeApplyStateChunk());
  mismatch.appendObjectDefine(kSurface, kTextureDescriptorKind, descriptor, 0u,
                              {});
  mismatch.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 1u, .handleCount = 0u},
      makePresentChunk());
  const RenderTapeOracleAttachment oracle{
      .identity = kSurface, .descriptorKind = kSurfaceDescriptorKind};
  mismatch.appendPresentComplete(
      3u, 1u, RenderTapeDigestValidity::NotCaptured, {},
      std::as_bytes(std::span(&oracle, 1u)));
  check(validateRenderTape(mismatch.seal(), {}).status ==
            RenderTapeValidationStatus::InvalidObjectDefine,
        "identity and descriptor schema tags must match exactly");
}

void bootstrapAndCommandHandlesCloseBeforeReplay() {
  const auto surfaceDescriptor = outputSurfaceDescriptor();
  const auto surfaceDescriptorBytes =
      std::as_bytes(std::span(&surfaceDescriptor, 1u));
  const auto textureDescriptor = texture2DDescriptor();
  const auto seedDigest = mutationDigest();
  const RenderTapeBlobCatalogue catalogue{.blobs = {{
      .digest = seedDigest, .size = 4u, .verified = 1u,
  }}};
  const RenderTapeOracleAttachment oracle{
      .identity = kSurface,
      .descriptorKind = static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
  };

  RenderTapeBuilder deferred;
  deferred.appendBootstrapState(makeApplyStateChunk(true, kTexture));
  // The definition is intentionally journaled after BootstrapState. The
  // closure index makes this safe without applying the provider state early.
  deferred.appendObjectDefine(
      kSurface, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
      surfaceDescriptorBytes, 0u, {});
  deferred.appendObjectDefine(kTexture, kTextureDescriptorKind, textureDescriptor,
                              0u, {}, 4u, 1u);
  deferred.appendResourceMutation(kTexture, RenderTapeMutationKind::Upload,
                                  0u, 0u, 4u, seedDigest);
  deferred.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 1u, .handleCount = 0u},
      makePresentChunk());
  deferred.appendPresentComplete(
      5u, 1u, RenderTapeDigestValidity::NotCaptured, {},
      std::as_bytes(std::span(&oracle, 1u)));
  ImportedRenderTapeView imported;
  const auto deferredValidation =
      validateRenderTape(deferred.seal(), catalogue, &imported);
  check(deferredValidation.valid(),
        std::string(renderTapeValidationStatusName(deferredValidation.status)) +
            " chunk=" +
            std::to_string(static_cast<std::uint32_t>(
                deferredValidation.chunkStatus)));

  auto stale = kTexture;
  ++stale.generation;
  RenderTapeBuilder missingBootstrapDefinition;
  missingBootstrapDefinition.appendBootstrapState(
      makeApplyStateChunk(true, stale));
  missingBootstrapDefinition.appendObjectDefine(
      kTexture, kTextureDescriptorKind, textureDescriptor, 0u, {}, 4u, 1u);
  missingBootstrapDefinition.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 1u, .handleCount = 0u},
      makePresentChunk());
  missingBootstrapDefinition.appendPresentComplete(
      3u, 1u, RenderTapeDigestValidity::NotCaptured, {},
      std::as_bytes(std::span(&oracle, 1u)));
  check(validateRenderTape(missingBootstrapDefinition.seal(), {}).status ==
            RenderTapeValidationStatus::UnknownIdentity,
        "bootstrap stale handles must fail before callbacks");

  RenderTapeBuilder missingCommandDefinition;
  missingCommandDefinition.appendBootstrapState(makeApplyStateChunk());
  missingCommandDefinition.appendObjectDefine(
      kSurface, kSurfaceDescriptorKind, surfaceDescriptorBytes, 0u, {});
  missingCommandDefinition.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 1u, .handleCount = 1u},
      makeApplyStateChunk(true, stale));
  missingCommandDefinition.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 1u, .handleCount = 0u},
      makePresentChunk());
  missingCommandDefinition.appendPresentComplete(
      4u, 1u, RenderTapeDigestValidity::NotCaptured, {},
      std::as_bytes(std::span(&oracle, 1u)));
  check(validateRenderTape(missingCommandDefinition.seal(), {}).status ==
            RenderTapeValidationStatus::UnknownIdentity,
        "later command handles must resolve exact generations");

  RenderTapeBuilder retiredCommandDefinition;
  retiredCommandDefinition.appendBootstrapState(makeApplyStateChunk());
  retiredCommandDefinition.appendObjectDefine(
      kTexture, kTextureDescriptorKind, textureDescriptor, 0u, {}, 4u, 1u);
  retiredCommandDefinition.appendResourceMutation(
      kTexture, RenderTapeMutationKind::Upload, 0u, 0u, 4u, seedDigest);
  retiredCommandDefinition.appendObjectDestroy(kTexture);
  retiredCommandDefinition.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 1u, .handleCount = 1u},
      makeApplyStateChunk(true, kTexture));
  retiredCommandDefinition.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 1u, .handleCount = 0u},
      makePresentChunk());
  retiredCommandDefinition.appendPresentComplete(
      6u, 1u, RenderTapeDigestValidity::NotCaptured, {},
      std::as_bytes(std::span(&oracle, 1u)));
  check(validateRenderTape(retiredCommandDefinition.seal(), catalogue).status ==
            RenderTapeValidationStatus::UnknownIdentity,
        "retired command handles must fail closed");
}

void seedContentClosesByUniqueSubresourceAndSummedBytes() {
  const auto surfaceDescriptor = outputSurfaceDescriptor();
  const auto surfaceDescriptorBytes =
      std::as_bytes(std::span(&surfaceDescriptor, 1u));
  const auto textureDescriptor = texture2DDescriptor(
      2u, RenderTapeInitialContentDisposition::CompleteSeed);
  const auto firstDigest = digest(std::byte{0x10});
  const auto secondDigest = digest(std::byte{0x30});
  const RenderTapeBlobCatalogue catalogue{.blobs = {
      RenderTapeBlob{.digest = firstDigest, .size = 3u, .verified = 1u},
      RenderTapeBlob{.digest = secondDigest, .size = 4u, .verified = 1u},
  }};
  const RenderTapeOracleAttachment oracle{
      .identity = kSurface,
      .descriptorKind = static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
  };

  const auto appendPrefix = [&](RenderTapeBuilder& builder) {
    builder.appendBootstrapState(makeApplyStateChunk());
    builder.appendObjectDefine(
        kSurface, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
        surfaceDescriptorBytes, 0u, {});
    builder.appendObjectDefine(kTexture, kTextureDescriptorKind, textureDescriptor,
                               0u, {}, 7u, 2u);
  };
  const auto appendSuffix = [&](RenderTapeBuilder& builder,
                                std::uint64_t presentOrdinal) {
    builder.appendCommandChunk(
        CommandChunkEnvelope{.recordCount = 1u, .handleCount = 0u},
        makePresentChunk());
    builder.appendPresentComplete(
        presentOrdinal, 1u, RenderTapeDigestValidity::NotCaptured, {},
        std::as_bytes(std::span(&oracle, 1u)));
  };

  RenderTapeBuilder complete;
  appendPrefix(complete);
  complete.appendResourceMutation(kTexture, RenderTapeMutationKind::Upload,
                                  0u, 0u, 3u, firstDigest);
  complete.appendResourceMutation(kTexture, RenderTapeMutationKind::Upload,
                                  1u, 0u, 4u, secondDigest);
  appendSuffix(complete, 6u);
  check(validateRenderTape(complete.seal(), catalogue).valid(),
        "unique multi-subresource seeds close by summed bytes and count");

  RenderTapeBuilder postArmMutation;
  appendPrefix(postArmMutation);
  postArmMutation.appendResourceMutation(
      kTexture, RenderTapeMutationKind::Upload, 0u, 0u, 3u, firstDigest);
  postArmMutation.appendResourceMutation(
      kTexture, RenderTapeMutationKind::Upload, 1u, 0u, 4u, secondDigest);
  // Once the declared checkpoint closes, a mutation in the live interval is
  // ordinary even when no CommandChunk has appeared yet. It may target a
  // seeded subresource again and need not match the aggregate seed extent.
  postArmMutation.appendResourceMutation(
      kTexture, RenderTapeMutationKind::CpuUnlock, 0u, 1u, 4u, secondDigest);
  appendSuffix(postArmMutation, 7u);
  check(validateRenderTape(postArmMutation.seal(), catalogue).valid(),
        "post-arm pre-command mutation is not reclassified as a seed");

  RenderTapeBuilder duplicate;
  appendPrefix(duplicate);
  duplicate.appendResourceMutation(kTexture, RenderTapeMutationKind::Upload,
                                   0u, 0u, 3u, firstDigest);
  duplicate.appendResourceMutation(kTexture, RenderTapeMutationKind::Upload,
                                   0u, 0u, 4u, secondDigest);
  appendSuffix(duplicate, 6u);
  check(validateRenderTape(duplicate.seal(), catalogue).status ==
            RenderTapeValidationStatus::InvalidMutationRange,
        "duplicate seed subresources fail closed even when totals match");

  RenderTapeBuilder incomplete;
  appendPrefix(incomplete);
  incomplete.appendResourceMutation(kTexture, RenderTapeMutationKind::Upload,
                                    0u, 0u, 3u, firstDigest);
  appendSuffix(incomplete, 5u);
  const auto incompleteResult =
      validateRenderTape(incomplete.seal(), catalogue);
  check(incompleteResult.status == RenderTapeValidationStatus::IncompleteFrame &&
            incompleteResult.failedEventIndex ==
                std::numeric_limits<std::uint32_t>::max(),
        "incomplete seed remains pending until its exact first use or frame end");
}

void perIdentityLateSeedClosureIsExact() {
  const auto surfaceDescriptor = outputSurfaceDescriptor();
  const auto surfaceDescriptorBytes =
      std::as_bytes(std::span(&surfaceDescriptor, 1u));
  const auto textureDescriptor = texture2DDescriptor(
      1u, RenderTapeInitialContentDisposition::CompleteSeed);
  const RenderTapeOracleAttachment oracle{
      .identity = kSurface,
      .descriptorKind = kSurfaceDescriptorKind,
  };
  const auto seedDigest = mutationDigest();
  const RenderTapeBlobCatalogue catalogue{.blobs = {
      RenderTapeBlob{.digest = seedDigest, .size = 4u, .verified = 1u},
  }};

  RenderTapeBuilder late;
  late.appendBootstrapState(makeApplyStateChunk());
  late.appendObjectDefine(kSurface, kSurfaceDescriptorKind, surfaceDescriptorBytes, 0u,
                          {});
  late.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 1u, .handleCount = 0u},
      makeApplyStateChunk());
  late.appendObjectDefine(kTexture, kTextureDescriptorKind, textureDescriptor, 0u,
                          {}, 4u, 1u);
  late.appendResourceMutation(kTexture, RenderTapeMutationKind::Upload, 0u,
                              0u, 4u, seedDigest);
  late.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 1u, .handleCount = 1u},
      makeApplyStateChunk(true, kTexture));
  late.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 1u, .handleCount = 0u},
      makePresentChunk());
  late.appendPresentComplete(
      7u, 8u, RenderTapeDigestValidity::NotCaptured, {},
      std::as_bytes(std::span(&oracle, 1u)));
  check(validateRenderTape(late.seal(), catalogue).valid(),
        "an exact identity may define and complete its seed after unrelated traffic");

  RenderTapeBuilder firstUse;
  firstUse.appendBootstrapState(makeApplyStateChunk());
  firstUse.appendObjectDefine(kSurface, kSurfaceDescriptorKind, surfaceDescriptorBytes, 0u,
                              {});
  firstUse.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 1u, .handleCount = 0u},
      makeApplyStateChunk());
  firstUse.appendObjectDefine(kTexture, kTextureDescriptorKind, textureDescriptor, 0u,
                              {}, 4u, 1u);
  firstUse.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 1u, .handleCount = 1u},
      makeApplyStateChunk(true, kTexture));
  firstUse.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 1u, .handleCount = 0u},
      makePresentChunk());
  firstUse.appendPresentComplete(
      6u, 7u, RenderTapeDigestValidity::NotCaptured, {},
      std::as_bytes(std::span(&oracle, 1u)));
  const auto rejected = validateRenderTape(firstUse.seal(), catalogue);
  check(rejected.status == RenderTapeValidationStatus::IncompleteFrame &&
            rejected.failedEventIndex == 4u,
        "first use of an exact identity rejects before a missing seed can be bypassed");

  RenderTapeBuilder sequence(kRenderTapeProfileSequence);
  sequence.appendBootstrapState(makeApplyStateChunk());
  sequence.appendObjectDefine(kSurface, kSurfaceDescriptorKind, surfaceDescriptorBytes, 0u,
                               {});
  sequence.appendObjectDefine(kTexture, kTextureDescriptorKind, textureDescriptor, 0u,
                               {}, 4u, 1u);
  sequence.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 1u, .handleCount = 0u},
      makePresentChunk());
  sequence.appendPresentComplete(
      4u, 5u, RenderTapeDigestValidity::NotCaptured, {},
      std::as_bytes(std::span(&oracle, 1u)));
  sequence.appendResourceMutation(kTexture, RenderTapeMutationKind::Upload, 0u,
                                  0u, 4u, seedDigest);
  sequence.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 1u, .handleCount = 0u},
      makePresentChunk());
  sequence.appendPresentComplete(
      7u, 8u, RenderTapeDigestValidity::NotCaptured, {},
      std::as_bytes(std::span(&oracle, 1u)));
  const auto sequenceRejected =
      validateRenderTape(sequence.seal(), catalogue);
  check(sequenceRejected.status == RenderTapeValidationStatus::IncompleteFrame &&
            sequenceRejected.failedEventIndex == 6u,
        "late initial seed cannot satisfy the sequence between-Present mutation");
}

void versionedSubresourceDescriptorsFailClosed() {
  constexpr D9CWireObjectIdentity texture{
      .kind = D9C_CHUNK_HANDLE_KIND_TEXTURE,
      .generation = 4u,
      .objectId = 0x4400u,
  };
  constexpr D9CWireObjectIdentity staleTexture{
      .kind = D9C_CHUNK_HANDLE_KIND_TEXTURE,
      .generation = 3u,
      .objectId = 0x4400u,
  };
  constexpr D9CWireObjectIdentity surface{
      .kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
      .generation = 2u,
      .objectId = 0x5500u,
  };
  constexpr D9CWireObjectIdentity output{
      .kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
      .generation = 1u,
      .objectId = 0x6600u,
  };
  const auto outputDescriptor = outputSurfaceDescriptor();
  const RenderTapeOracleAttachment outputOracle{
      .identity = output,
      .descriptorKind = static_cast<std::uint32_t>(
          RenderTapeDescriptorKind::Surface),
  };
  const auto finish = [&](RenderTapeBuilder& builder,
                          std::uint64_t presentOrdinal) {
    builder.appendCommandChunk(
        CommandChunkEnvelope{.recordCount = 1u, .handleCount = 0u},
        makePresentChunk());
    builder.appendPresentComplete(
        presentOrdinal, 1u, RenderTapeDigestValidity::NotCaptured, {},
        std::as_bytes(std::span(&outputOracle, 1u)));
  };
  const RenderTapeTextureDescriptorV2 textureHeader{
      .schemaVersion = kRenderTapeTextureDescriptorVersion2,
      .dimension =
          static_cast<std::uint32_t>(RenderTapeTextureDimension::Cube),
      .mipLevelCount = 2u,
      .subresourceCount = 12u,
      .initialContentDisposition = static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::CompleteSeed),
  };
  std::vector<std::byte> textureDescriptor(
      sizeof(textureHeader) + 12u * sizeof(D9CSurfaceDesc));
  check(textureHeader.subresourceCount ==
                6u * textureHeader.mipLevelCount &&
            textureDescriptor.size() ==
                sizeof(textureHeader) +
                    static_cast<std::size_t>(textureHeader.subresourceCount) *
                        sizeof(D9CSurfaceDesc),
        "versioned cube descriptor contains exactly six complete mip chains");
  std::memcpy(textureDescriptor.data(), &textureHeader, sizeof(textureHeader));
  for (std::uint32_t subresource = 0u; subresource < 12u; ++subresource) {
    const auto mipLevel = renderTapeTextureDescriptorMipLevel(
        RenderTapeTextureDimension::Cube, textureHeader.mipLevelCount,
        subresource);
    const D9CSurfaceDesc desc{
        .format = 0x33545844u,
        .resourceType = 5u,
        .width = mipLevel == 0u ? 8u : 4u,
        .height = mipLevel == 0u ? 8u : 4u,
        .depth = 1u,
    };
    std::memcpy(textureDescriptor.data() + sizeof(textureHeader) +
                    static_cast<std::size_t>(subresource) * sizeof(desc),
                &desc, sizeof(desc));
  }
  for (std::uint32_t face = 0u; face < 6u; ++face) {
    for (std::uint32_t mipLevel = 0u;
         mipLevel < textureHeader.mipLevelCount; ++mipLevel) {
      const auto subresource = face * textureHeader.mipLevelCount + mipLevel;
      D9CSurfaceDesc desc{};
      std::memcpy(&desc,
                  textureDescriptor.data() + sizeof(textureHeader) +
                      static_cast<std::size_t>(subresource) * sizeof(desc),
                  sizeof(desc));
      const auto expectedExtent = mipLevel == 0u ? 8u : 4u;
      check(desc.width == expectedExtent && desc.height == expectedExtent,
            "all six cube faces repeat the complete mip descriptor chain");
    }
  }

  const RenderTapeTextureDescriptorV2 volumeHeader{
      .schemaVersion = kRenderTapeTextureDescriptorVersion2,
      .dimension = static_cast<std::uint32_t>(
          RenderTapeTextureDimension::Volume),
      .mipLevelCount = 3u,
      .subresourceCount = 3u,
      .initialContentDisposition = static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::CompleteSeed),
  };
  std::vector<std::byte> volumeDescriptor(
      sizeof(volumeHeader) + 3u * sizeof(D9CSurfaceDesc));
  std::memcpy(volumeDescriptor.data(), &volumeHeader, sizeof(volumeHeader));
  for (std::uint32_t mip = 0u; mip < 3u; ++mip) {
    const D9CSurfaceDesc level{
        .format = 21u,
        .resourceType = 4u,
        .width = std::max(1u, 8u >> mip),
        .height = std::max(1u, 4u >> mip),
        .depth = std::max(1u, 8u >> mip),
    };
    std::memcpy(volumeDescriptor.data() + sizeof(volumeHeader) +
                    mip * sizeof(level),
                &level, sizeof(level));
  }
  for (std::uint32_t mip = 0u; mip < 3u; ++mip) {
    D9CSurfaceDesc level{};
    check(renderTapeTextureSubresourceDescriptor(volumeDescriptor, mip,
                                                  level) &&
              level.depth == (8u >> mip),
          "volume V2 preserves exact depth for every mip descriptor");
  }
  const RenderTapeObjectDefineHeader volumeFixed{
      .identity = texture,
      .descriptorKind = static_cast<std::uint32_t>(
          RenderTapeDescriptorKind::Texture),
      .descriptorBytes =
          static_cast<std::uint32_t>(volumeDescriptor.size()),
      .expectedContentBytes = 3u,
      .expectedContentCount = 3u,
  };
  check(!renderTapeClassifyObjectDefineValidation(volumeFixed,
                                                   volumeDescriptor)
             .valid(),
        "canonical volume V2 descriptor validates as one descriptor per mip");

  RenderTapeBuilder incompleteSeed;
  incompleteSeed.appendBootstrapState(makeApplyStateChunk());
  incompleteSeed.appendObjectDefine(
      output, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
      std::as_bytes(std::span(&outputDescriptor, 1u)), 0u, {});
  incompleteSeed.appendObjectDefine(
      texture, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Texture),
      textureDescriptor, 0u, {}, 64u, 1u);
  finish(incompleteSeed, 4u);
  check(validateRenderTape(incompleteSeed.seal(), {}).status ==
            RenderTapeValidationStatus::InvalidObjectDefine,
        "versioned cube descriptor rejects an incomplete face/mip seed count");

  const RenderTapeSurfaceDescriptorV2 staleAlias{
      .schemaVersion = kRenderTapeSurfaceDescriptorVersion2,
      .storage = static_cast<std::uint32_t>(
          RenderTapeSurfaceStorage::TextureSubresource),
      .initialContentDisposition = static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::Unavailable),
      .subresource = 7u,
      .parentTexture = staleTexture,
      .surface = D9CSurfaceDesc{
          .format = 0x33545844u,
          .resourceType = 1u,
          .width = 4u,
          .height = 4u,
          .depth = 1u,
      },
  };
  RenderTapeBuilder stale;
  stale.appendBootstrapState(makeApplyStateChunk());
  stale.appendObjectDefine(
      output, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
      std::as_bytes(std::span(&outputDescriptor, 1u)), 0u, {});
  stale.appendObjectDefine(
      texture, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Texture),
      textureDescriptor, 0u, {}, 768u, 12u);
  stale.appendObjectDefine(
      surface, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
      std::as_bytes(std::span(&staleAlias, 1u)), 0u, {});
  finish(stale, 5u);
  check(validateRenderTape(stale.seal(), {}).status ==
            RenderTapeValidationStatus::InvalidObjectDefine,
        "surface alias rejects a stale parent texture generation");

  auto outOfRangeAlias = staleAlias;
  outOfRangeAlias.parentTexture = texture;
  outOfRangeAlias.subresource = 12u;
  RenderTapeBuilder outOfRange;
  outOfRange.appendBootstrapState(makeApplyStateChunk());
  outOfRange.appendObjectDefine(
      output, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
      std::as_bytes(std::span(&outputDescriptor, 1u)), 0u, {});
  outOfRange.appendObjectDefine(
      texture, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Texture),
      textureDescriptor, 0u, {}, 768u, 12u);
  outOfRange.appendObjectDefine(
      surface, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
      std::as_bytes(std::span(&outOfRangeAlias, 1u)), 0u, {});
  finish(outOfRange, 5u);
  check(validateRenderTape(outOfRange.seal(), {}).status ==
            RenderTapeValidationStatus::InvalidObjectDefine,
        "surface alias rejects a parent subresource outside the exact texture");

  auto exactAlias = staleAlias;
  exactAlias.parentTexture = texture;
  RenderTapeBuilder exact;
  exact.appendBootstrapState(makeApplyStateChunk());
  exact.appendObjectDefine(
      output, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
      std::as_bytes(std::span(&outputDescriptor, 1u)), 0u, {});
  exact.appendObjectDefine(
      texture, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Texture),
      textureDescriptor, 0u, {}, 768u, 12u);
  exact.appendObjectDefine(
      surface, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
      std::as_bytes(std::span(&exactAlias, 1u)), 0u, {});
  finish(exact, 5u);
  check(validateRenderTape(exact.seal(), {}).status !=
            RenderTapeValidationStatus::InvalidObjectDefine,
        "surface alias accepts the exact generation-qualified parent identity");
}

void textureDispositionDiagnosticsPreserveCanonicalLevel0() {
  auto descriptor = texture2DDescriptor(
      1u, RenderTapeInitialContentDisposition::ProducedByCapturedPass);
  const D9CSurfaceDesc depthLevel0{
      .format = 75u,
      .resourceType = 3u,
      .usage = 2u,
      .pool = 0u,
      .multiSampleType = 0u,
      .multiSampleQuality = 0u,
      .width = 37u,
      .height = 19u,
      .depth = 1u,
  };
  const auto writeLevel0 = [&](const D9CSurfaceDesc& level0) {
    std::memcpy(descriptor.data() + sizeof(RenderTapeTextureDescriptorV2),
                &level0, sizeof(level0));
  };
  const auto matchesLevel0 = [](const D9CSurfaceDesc& actual,
                                const D9CSurfaceDesc& expected) {
    return actual.format == expected.format &&
           actual.resourceType == expected.resourceType &&
           actual.usage == expected.usage && actual.pool == expected.pool &&
           actual.multiSampleType == expected.multiSampleType &&
           actual.multiSampleQuality == expected.multiSampleQuality &&
           actual.width == expected.width && actual.height == expected.height &&
           actual.depth == expected.depth;
  };
  const RenderTapeObjectDefineHeader fixed{
      .identity = kTexture,
      .descriptorKind = kTextureDescriptorKind,
      .descriptorBytes = static_cast<std::uint32_t>(descriptor.size()),
  };
  writeLevel0(depthLevel0);
  const auto detail = renderTapeClassifyObjectDefineValidation(fixed, descriptor);
  check(detail.subreason ==
            RenderTapeObjectDefineValidationSubreason::TextureDescriptorDisposition &&
            detail.hasTextureLevel0 &&
            matchesLevel0(detail.textureLevel0, depthLevel0) &&
            sameWireIdentity(detail.identity, kTexture) &&
            detail.expectedContentBytes == 0u &&
            detail.expectedContentCount == 0u,
        "depth-usage Produced rejects with the exact canonical level-zero descriptor");

  const auto makeTape = [&]() {
    const auto outputDescriptor = outputSurfaceDescriptor();
    const RenderTapeOracleAttachment oracle{
        .identity = kSurface,
        .descriptorKind = kSurfaceDescriptorKind,
    };
    RenderTapeBuilder builder;
    builder.appendBootstrapState(makeApplyStateChunk());
    builder.appendObjectDefine(
        kSurface, kSurfaceDescriptorKind,
        std::as_bytes(std::span(&outputDescriptor, 1u)), 0u, {});
    builder.appendObjectDefine(kTexture, kTextureDescriptorKind, descriptor, 0u,
                               {});
    builder.appendCommandChunk(
        CommandChunkEnvelope{.recordCount = 1u, .handleCount = 0u},
        makePresentChunk());
    builder.appendPresentComplete(
        4u, 1u, RenderTapeDigestValidity::NotCaptured, {},
        std::as_bytes(std::span(&oracle, 1u)));
    return builder.seal();
  };
  const auto rejected = validateRenderTape(makeTape(), {});
  check(rejected.status == RenderTapeValidationStatus::InvalidObjectDefine &&
            rejected.failedEventIndex == 2u &&
            rejected.objectDefine.subreason == detail.subreason &&
            rejected.objectDefine.hasTextureLevel0 &&
            matchesLevel0(rejected.objectDefine.textureLevel0, depthLevel0),
        "canonical validation preserves the rejected texture's actual descriptor");

  auto rtLevel0 = depthLevel0;
  rtLevel0.format = 21u;
  rtLevel0.usage = 1u;
  writeLevel0(rtLevel0);
  const auto rtDetail = renderTapeClassifyObjectDefineValidation(fixed, descriptor);
  check(!rtDetail.valid() && rtDetail.hasTextureLevel0 &&
            matchesLevel0(rtDetail.textureLevel0, rtLevel0),
        "RT-usage Produced keeps its existing descriptor admission unchanged");
  const auto unresolved = validateRenderTape(makeTape(), {});
  check(unresolved.status == RenderTapeValidationStatus::IncompleteFrame &&
            unresolved.incompleteFrameReason ==
                RenderTapeIncompleteFrameReason::ProducedProofUnresolvedAtPresent,
        "diagnostic level-zero data cannot replace the terminal-first produced proof");

  auto invalidLevel0 = depthLevel0;
  invalidLevel0.resourceType = 1u;
  writeLevel0(invalidLevel0);
  const auto malformed = renderTapeClassifyObjectDefineValidation(fixed, descriptor);
  check(malformed.subreason ==
            RenderTapeObjectDefineValidationSubreason::TextureDescriptorDimension &&
            !malformed.hasTextureLevel0 &&
            matchesLevel0(malformed.textureLevel0, D9CSurfaceDesc{}),
        "malformed texture topology cannot claim an authoritative level-zero diagnostic");
}

std::vector<std::byte> df24ProducedTextureDescriptor() {
  auto descriptor = texture2DDescriptor(
      1u, RenderTapeInitialContentDisposition::ProducedByCapturedPass);
  const D9CSurfaceDesc level0{
      .format = kRenderTapeDf24D3DFormat,
      .resourceType = 3u,
      .usage = 2u,
      .pool = 0u,
      .width = 2048u,
      .height = 2048u,
      .depth = 1u,
  };
  std::memcpy(descriptor.data() + sizeof(RenderTapeTextureDescriptorV2),
              &level0, sizeof(level0));
  return descriptor;
}

RenderTapeSurfaceDescriptorV2 df24ProducedAliasDescriptor(
    const D9CWireObjectIdentity& parent) {
  const auto texture = df24ProducedTextureDescriptor();
  D9CSurfaceDesc level0{};
  check(renderTapeTextureSubresourceDescriptor(texture, 0u, level0),
        "authentic DF24 level-zero descriptor loads");
  level0.resourceType = 1u;
  return RenderTapeSurfaceDescriptorV2{
      .schemaVersion = kRenderTapeSurfaceDescriptorVersion2,
      .storage = static_cast<std::uint32_t>(
          RenderTapeSurfaceStorage::TextureSubresource),
      .initialContentDisposition = static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::Unavailable),
      .parentTexture = parent,
      .surface = level0,
  };
}

// The binding owns the sole handle row; Clear and carried-attachment draws
// have empty canonical handle slices. This pins the real stream rather than
// constructing an ImportedChunkView that skips wire validation.
std::vector<std::byte> makeDf24AttachmentProofChunk(
    const D9CWireObjectIdentity& alias, std::uint32_t flags = 2u,
    std::uint32_t rectCount = 0u,
    std::uint32_t sectionKind = D9C_COMMAND_CHUNK_SECTION_DEPTH_STENCIL,
    bool drawBeforeClear = false, bool laterZClear = false) {
  const D9CCommandChunkWireDepthStencilBinding depthBinding{
      .valid = 1u, .handleIndex = 0u};
  const D9CCommandChunkWireRenderTargetBinding colorBinding{
      .valid = 1u, .handleIndex = 0u};
  const auto bindingBytes = sectionKind == D9C_COMMAND_CHUNK_SECTION_DEPTH_STENCIL
      ? std::as_bytes(std::span(&depthBinding, 1u))
      : std::as_bytes(std::span(&colorBinding, 1u));
  const auto bindingOffset = sizeof(D9CCommandChunkWireDrawHeader) +
                             sizeof(D9CCommandChunkWireSectionDesc);
  const D9CCommandChunkWireDrawHeader apply{
      .sectionCount = 1u,
      .sectionTableOffset = sizeof(D9CCommandChunkWireDrawHeader),
      .sectionPayloadOffset = static_cast<std::uint32_t>(bindingOffset),
  };
  const D9CCommandChunkWireSectionDesc section{
      .kind = static_cast<std::uint16_t>(sectionKind),
      .elementSize = static_cast<std::uint16_t>(bindingBytes.size()),
      .count = 1u,
      .payloadOffset = static_cast<std::uint32_t>(bindingOffset),
      .byteSize = static_cast<std::uint32_t>(bindingBytes.size()),
  };
  std::vector<D9CCommandChunkWireRecordHeader> records;
  std::vector<std::byte> payload(bindingOffset + bindingBytes.size());
  std::memcpy(payload.data(), &apply, sizeof(apply));
  std::memcpy(payload.data() + sizeof(apply), &section, sizeof(section));
  std::memcpy(payload.data() + bindingOffset, bindingBytes.data(),
              bindingBytes.size());
  records.push_back({
      .type = D9C_COMMAND_RECORD_APPLY_STATE,
      .payloadSize = static_cast<std::uint32_t>(payload.size()),
      .handleCount = 1u,
  });
  const auto append = [&](std::uint32_t type,
                          std::span<const std::byte> bytes) {
    records.push_back({
        .type = type,
        .payloadOffset = static_cast<std::uint32_t>(payload.size()),
        .payloadSize = static_cast<std::uint32_t>(bytes.size()),
        .firstHandle = 1u,
    });
    payload.insert(payload.end(), bytes.begin(), bytes.end());
  };
  if (drawBeforeClear) {
    const D9CCommandChunkWireDrawHeader draw{
        .primitiveType = 4u,
        .primitiveCount = 1u,
        .sectionTableOffset = sizeof(D9CCommandChunkWireDrawHeader),
        .sectionPayloadOffset = sizeof(D9CCommandChunkWireDrawHeader),
    };
    append(D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
           std::as_bytes(std::span(&draw, 1u)));
  }
  const auto appendClear = [&](std::uint32_t clearFlags,
                               std::uint32_t rectangles) {
    const D9CCommandChunkWireClear clear{
        .flags = clearFlags,
        .z = 0.5f,
        .rectCount = rectangles,
        .rectOffset = sizeof(D9CCommandChunkWireClear),
    };
    std::vector<std::byte> bytes(sizeof(clear) + rectangles * sizeof(D9CRect));
    std::memcpy(bytes.data(), &clear, sizeof(clear));
    if (rectangles != 0u) {
      const D9CRect rect{.right = 1024, .bottom = 1024};
      std::memcpy(bytes.data() + sizeof(clear), &rect, sizeof(rect));
    }
    append(D9C_COMMAND_RECORD_CLEAR, bytes);
  };
  appendClear(flags, rectCount);
  if (laterZClear) appendClear(2u, 0u);
  const auto handleTableOffset = alignUp(
      sizeof(D9CCommandChunkWireHeader) +
          records.size() * sizeof(D9CCommandChunkWireRecordHeader),
      alignof(D9CCommandChunkWireHandleEntry));
  const auto payloadOffset = alignUp(
      handleTableOffset + sizeof(D9CCommandChunkWireHandleEntry),
      alignof(std::uint32_t));
  const D9CCommandChunkWireHeader header{
      .version = D9C_COMMAND_CHUNK_WIRE_VERSION,
      .headerSize = D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE,
      .recordHeaderSize = D9C_COMMAND_CHUNK_WIRE_RECORD_HEADER_SIZE,
      .handleEntrySize = D9C_COMMAND_CHUNK_WIRE_HANDLE_ENTRY_SIZE,
      .recordTableOffset = sizeof(D9CCommandChunkWireHeader),
      .recordCount = static_cast<std::uint32_t>(records.size()),
      .handleTableOffset = static_cast<std::uint32_t>(handleTableOffset),
      .handleCount = 1u,
      .payloadArenaOffset = static_cast<std::uint32_t>(payloadOffset),
      .payloadArenaSize = static_cast<std::uint32_t>(payload.size()),
  };
  const D9CCommandChunkWireHandleEntry handle{
      .kind = alias.kind, .generation = alias.generation,
      .objectId = alias.objectId};
  std::vector<std::byte> bytes(payloadOffset + payload.size());
  std::memcpy(bytes.data(), &header, sizeof(header));
  std::memcpy(bytes.data() + header.recordTableOffset, records.data(),
              records.size() * sizeof(records[0]));
  std::memcpy(bytes.data() + handleTableOffset, &handle, sizeof(handle));
  std::memcpy(bytes.data() + payloadOffset, payload.data(), payload.size());
  return bytes;
}

void appendDf24TestChunk(RenderTapeBuilder& builder,
                         const std::vector<std::byte>& bytes) {
  D9CCommandChunkWireHeader header{};
  std::memcpy(&header, bytes.data(), sizeof(header));
  const CommandChunkEnvelope envelope{
      .recordCount = header.recordCount, .handleCount = header.handleCount};
  check(validateCommandChunk(bytes, envelope).valid(),
        "DF24 canonical fixture command chunk validates on the wire");
  builder.appendCommandChunk(envelope, bytes);
}

RenderTapeBuilder beginDf24TestTape() {
  RenderTapeBuilder builder;
  builder.appendBootstrapState(makeApplyStateChunk());
  const auto output = outputSurfaceDescriptor();
  builder.appendObjectDefine(kSurface, kSurfaceDescriptorKind,
                             std::as_bytes(std::span(&output, 1u)), 0u, {});
  return builder;
}

void appendDf24TestPair(RenderTapeBuilder& builder,
                        const D9CWireObjectIdentity& parent,
                        const D9CWireObjectIdentity& alias) {
  const auto descriptor = df24ProducedTextureDescriptor();
  const auto aliasDescriptor = df24ProducedAliasDescriptor(parent);
  builder.appendObjectDefine(parent, kTextureDescriptorKind, descriptor, 0u, {});
  builder.appendObjectDefine(alias, kSurfaceDescriptorKind,
      std::as_bytes(std::span(&aliasDescriptor, 1u)), 0u, {});
}

std::vector<std::byte> finishDf24TestTape(RenderTapeBuilder& builder) {
  appendDf24TestChunk(builder, makeMappedPresentChunk(kSurface));
  const RenderTapeOracleAttachment oracle{
      .identity = kSurface, .descriptorKind = kSurfaceDescriptorKind};
  builder.appendPresentComplete(0u, 1u,
      RenderTapeDigestValidity::NotCaptured, {},
      std::as_bytes(std::span(&oracle, 1u)));
  // Seal only after the required terminal event, then derive the immediately
  // preceding Present ordinal from the serialized event layout.
  auto tape = builder.seal();
  RenderTapeHeader header{};
  std::memcpy(&header, tape.data(), sizeof(header));
  eventPayload<RenderTapePresentCompleteHeader>(tape, header.eventCount - 1u)
      ->presentOrdinal = header.eventCount - 1u;
  return tape;
}

void producedDf24CanonicalObligationsAreTemporalAndPerParent() {
  constexpr D9CWireObjectIdentity alias{
      .kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
      .generation = 1u, .objectId = 4294967319ull};
  constexpr D9CWireObjectIdentity parent{
      .kind = D9C_CHUNK_HANDLE_KIND_TEXTURE,
      .generation = 1u, .objectId = 4294967318ull};
  constexpr D9CWireObjectIdentity otherParent{
      .kind = D9C_CHUNK_HANDLE_KIND_TEXTURE,
      .generation = 2u, .objectId = 4294967320ull};
  constexpr D9CWireObjectIdentity otherAlias{
      .kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
      .generation = 2u, .objectId = 4294967321ull};
  const auto proof = makeDf24AttachmentProofChunk(alias);
  const auto reject = [&](const std::vector<std::byte>& tape,
                          RenderTapeValidationStatus status,
                          RenderTapeIncompleteFrameReason reason =
                              RenderTapeIncompleteFrameReason::None,
                          const RenderTapeBlobCatalogue& catalogue =
                              RenderTapeBlobCatalogue{}) {
    ImportedRenderTapeView imported;
    const auto result = validateRenderTape(tape, catalogue, &imported);
    check(result.status == status && result.incompleteFrameReason == reason,
          std::string("DF24 canonical negative expected ") +
              renderTapeValidationStatusName(status) + "/" +
              renderTapeIncompleteFrameReasonName(reason) + ", got " +
              renderTapeValidationStatusName(result.status) + "/" +
              renderTapeIncompleteFrameReasonName(result.incompleteFrameReason) +
              " at event " + std::to_string(result.failedEventIndex));
    RecordingSink sink;
    if (result.valid())
      (void)replayPrevalidatedRenderTape(imported, catalogue, sink);
    check(sink.calls.empty(), "rejected DF24 tape has zero replay callbacks");
    return result;
  };
  const auto beginPair = [&]() {
    auto builder = beginDf24TestTape();
    appendDf24TestPair(builder, parent, alias);
    return builder;
  };
  const auto appendOutput = [&](RenderTapeBuilder& builder) {
    const auto output = outputSurfaceDescriptor();
    builder.appendObjectDefine(kSurface, kSurfaceDescriptorKind,
        std::as_bytes(std::span(&output, 1u)), 0u, {});
  };
  RenderTapeBuilder boundBootstrap;
  boundBootstrap.appendBootstrapState(makeApplyStateChunk(true, parent));
  appendOutput(boundBootstrap);
  appendDf24TestPair(boundBootstrap, parent, alias);
  appendDf24TestChunk(boundBootstrap, proof);
  check(validateRenderTape(finishDf24TestTape(boundBootstrap), {}).valid(),
        "binding-only bootstrap does not consume uninitialized DF24 content");
  RenderTapeBuilder drawBootstrap;
  drawBootstrap.appendBootstrapState(makeDrawChunk(parent));
  appendOutput(drawBootstrap);
  appendDf24TestPair(drawBootstrap, parent, alias);
  appendDf24TestChunk(drawBootstrap, proof);
  reject(finishDf24TestTape(drawBootstrap),
         RenderTapeValidationStatus::BootstrapForbiddenRecord);
  RenderTapeBuilder presentBootstrap;
  presentBootstrap.appendBootstrapState(makeMappedPresentChunk(alias));
  appendOutput(presentBootstrap);
  appendDf24TestPair(presentBootstrap, parent, alias);
  appendDf24TestChunk(presentBootstrap, proof);
  reject(finishDf24TestTape(presentBootstrap),
         RenderTapeValidationStatus::BootstrapForbiddenRecord);
  auto accepted = beginPair();
  appendDf24TestChunk(accepted, proof);
  appendDf24TestChunk(accepted, makeDrawChunk(parent));
  const auto positive = finishDf24TestTape(accepted);
  ImportedRenderTapeView imported;
  check(validateRenderTape(positive, {}, &imported).valid(),
        "authentic DF24 Z-only full clear needs no seed and allows later sampling");
  RecordingSink sink;
  check(replayPrevalidatedRenderTape(imported, {}, sink).complete &&
            sink.calls.size() == imported.events.size(),
        "DF24 canonical tape replays each admitted event exactly once");

  for (const auto flags : {0u, 1u, 3u, 4u, 5u, 6u, 7u, 0x82u}) {
    auto builder = beginPair();
    appendDf24TestChunk(builder, makeDf24AttachmentProofChunk(alias, flags));
    reject(finishDf24TestTape(builder), RenderTapeValidationStatus::IncompleteFrame,
           RenderTapeIncompleteFrameReason::ProducedProofRejected);
  }
  for (const auto flags : {3u, 4u, 6u}) {
    auto builder = beginPair();
    appendDf24TestChunk(builder,
        makeDf24AttachmentProofChunk(alias, flags, 0u,
            D9C_COMMAND_CHUNK_SECTION_DEPTH_STENCIL, false, true));
    reject(finishDf24TestTape(builder), RenderTapeValidationStatus::IncompleteFrame,
           RenderTapeIncompleteFrameReason::ProducedProofRejected);
  }
  for (const auto drawFirst : {false, true}) {
    auto builder = beginPair();
    appendDf24TestChunk(builder,
        makeDf24AttachmentProofChunk(alias, 2u, drawFirst ? 0u : 1u,
            D9C_COMMAND_CHUNK_SECTION_DEPTH_STENCIL, drawFirst, true));
    reject(finishDf24TestTape(builder), RenderTapeValidationStatus::IncompleteFrame,
           RenderTapeIncompleteFrameReason::ProducedProofRejected);
  }
  auto wrongBinding = beginPair();
  appendDf24TestChunk(wrongBinding, makeDf24AttachmentProofChunk(alias, 1u, 0u,
      D9C_COMMAND_CHUNK_SECTION_RENDER_TARGET));
  reject(finishDf24TestTape(wrongBinding), RenderTapeValidationStatus::IncompleteFrame,
         RenderTapeIncompleteFrameReason::ProducedProofRejected);

  auto directParent = beginPair();
  appendDf24TestChunk(directParent, makeDrawChunk(parent));
  appendDf24TestChunk(directParent, proof);
  reject(finishDf24TestTape(directParent), RenderTapeValidationStatus::IncompleteFrame,
         RenderTapeIncompleteFrameReason::ProducedProofRejected);

  auto presentFirst = beginPair();
  appendDf24TestChunk(presentFirst, makeMappedPresentChunk(alias));
  appendDf24TestChunk(presentFirst, proof);
  reject(finishDf24TestTape(presentFirst), RenderTapeValidationStatus::IncompleteFrame,
         RenderTapeIncompleteFrameReason::ProducedProofRejected);
  const D9CCommandChunkWireStretchRect copy{
      .srcHandleIndex = 0u, .dstHandleIndex = 1u};
  const std::array copyHandles{
      D9CCommandChunkWireHandleEntry{
          .kind = alias.kind, .generation = alias.generation,
          .objectId = alias.objectId},
      D9CCommandChunkWireHandleEntry{
          .kind = kSurface.kind, .generation = kSurface.generation,
          .objectId = kSurface.objectId},
  };
  auto copyFirst = beginPair();
  appendDf24TestChunk(copyFirst, makeSingleRecordChunk(
      D9C_COMMAND_RECORD_STRETCH_RECT, std::as_bytes(std::span(&copy, 1u)),
      copyHandles));
  appendDf24TestChunk(copyFirst, proof);
  reject(finishDf24TestTape(copyFirst), RenderTapeValidationStatus::IncompleteFrame,
         RenderTapeIncompleteFrameReason::ProducedProofRejected);

  auto forwardParent = beginDf24TestTape();
  appendDf24TestChunk(forwardParent, proof);
  appendDf24TestPair(forwardParent, parent, alias);
  reject(finishDf24TestTape(forwardParent), RenderTapeValidationStatus::UnknownIdentity);
  auto forwardAlias = beginDf24TestTape();
  const auto parentDescriptor = df24ProducedTextureDescriptor();
  const auto aliasDescriptor = df24ProducedAliasDescriptor(parent);
  forwardAlias.appendObjectDefine(parent, kTextureDescriptorKind,
                                  parentDescriptor, 0u, {});
  appendDf24TestChunk(forwardAlias, proof);
  forwardAlias.appendObjectDefine(alias, kSurfaceDescriptorKind,
      std::as_bytes(std::span(&aliasDescriptor, 1u)), 0u, {});
  reject(finishDf24TestTape(forwardAlias), RenderTapeValidationStatus::UnknownIdentity);

  for (const auto usage : {0u, 1u, 3u, 0x102u}) {
    auto builder = beginDf24TestTape();
    auto descriptor = parentDescriptor;
    D9CSurfaceDesc level0{};
    std::memcpy(&level0, descriptor.data() + sizeof(RenderTapeTextureDescriptorV2),
                sizeof(level0));
    level0.usage = usage;
    std::memcpy(descriptor.data() + sizeof(RenderTapeTextureDescriptorV2),
                &level0, sizeof(level0));
    builder.appendObjectDefine(parent, kTextureDescriptorKind, descriptor, 0u, {});
    auto badAlias = aliasDescriptor;
    badAlias.surface.usage = usage;
    builder.appendObjectDefine(alias, kSurfaceDescriptorKind,
        std::as_bytes(std::span(&badAlias, 1u)), 0u, {});
    appendDf24TestChunk(builder, proof);
    reject(finishDf24TestTape(builder), RenderTapeValidationStatus::InvalidObjectDefine);
  }
  for (const auto staleParent : {false, true}) {
    auto builder = beginDf24TestTape();
    builder.appendObjectDefine(parent, kTextureDescriptorKind,
                                parentDescriptor, 0u, {});
    auto badAlias = aliasDescriptor;
    if (staleParent) ++badAlias.parentTexture.generation;
    else --badAlias.surface.width;
    builder.appendObjectDefine(alias, kSurfaceDescriptorKind,
        std::as_bytes(std::span(&badAlias, 1u)), 0u, {});
    appendDf24TestChunk(builder, proof);
    reject(finishDf24TestTape(builder), RenderTapeValidationStatus::InvalidObjectDefine);
  }
  for (const auto expectedCount : {0u, 1u}) {
    auto seeded = positive;
    auto* fixed = eventPayload<RenderTapeObjectDefineHeader>(seeded, 2u);
    fixed->expectedContentBytes = 4u;
    fixed->expectedContentCount = expectedCount;
    reject(seeded, RenderTapeValidationStatus::InvalidObjectDefine);
  }

  auto noAlias = beginDf24TestTape();
  noAlias.appendObjectDefine(parent, kTextureDescriptorKind,
                             parentDescriptor, 0u, {});
  const auto noAliasResult = reject(finishDf24TestTape(noAlias),
      RenderTapeValidationStatus::InvalidObjectDefine);
  check(noAliasResult.objectDefine.subreason ==
            RenderTapeObjectDefineValidationSubreason::ProducedTextureAliasClosure,
        "missing DF24 alias cannot leave an ambiguous proof origin");

  auto unresolved = beginPair();
  reject(finishDf24TestTape(unresolved), RenderTapeValidationStatus::IncompleteFrame,
         RenderTapeIncompleteFrameReason::ProducedProofUnresolvedAtPresent);

  auto independent = beginPair();
  appendDf24TestPair(independent, otherParent, otherAlias);
  appendDf24TestChunk(independent, proof);
  appendDf24TestChunk(independent, makeDf24AttachmentProofChunk(otherAlias));
  appendDf24TestChunk(independent, makeDrawChunk(otherParent));
  check(validateRenderTape(finishDf24TestTape(independent), {}).valid(),
        "independent DF24 parents each resolve their own exact-alias obligation");
  auto crossIdentity = beginPair();
  appendDf24TestPair(crossIdentity, otherParent, otherAlias);
  appendDf24TestChunk(crossIdentity, proof);
  reject(finishDf24TestTape(crossIdentity), RenderTapeValidationStatus::IncompleteFrame,
         RenderTapeIncompleteFrameReason::ProducedProofUnresolvedAtPresent);

  auto ambiguous = beginPair();
  const auto alternate = df24ProducedAliasDescriptor(parent);
  ambiguous.appendObjectDefine(otherAlias, kSurfaceDescriptorKind,
      std::as_bytes(std::span(&alternate, 1u)), 0u, {});
  appendDf24TestChunk(ambiguous, proof);
  const auto ambiguousResult = reject(finishDf24TestTape(ambiguous),
      RenderTapeValidationStatus::InvalidObjectDefine);
  check(ambiguousResult.failedEventIndex == 2u &&
            ambiguousResult.objectDefine.subreason ==
                RenderTapeObjectDefineValidationSubreason::ProducedTextureAliasClosure,
        "two DF24 aliases reject at the parent's closure before terminal proof");

  for (const auto identity : {parent, alias}) {
    auto mutation = beginPair();
    appendDf24TestChunk(mutation, proof);
    mutation.appendResourceMutation(identity, RenderTapeMutationKind::Upload,
                                    0u, 0u, 4u, mutationDigest());
    reject(finishDf24TestTape(mutation), RenderTapeValidationStatus::InvalidMutationKind,
           RenderTapeIncompleteFrameReason::None, completeCatalogue());
  }

  const auto appendRead = [&](RenderTapeBuilder& builder,
                              const D9CWireObjectIdentity& identity) {
    const RenderTapeCpuReadControl read{.copyCount = 1u, .bytesRead = 4u};
    const RenderTapeOrderedControlHeader control{
        .identity = identity,
        .kind = static_cast<std::uint32_t>(RenderTapeControlKind::CpuRead),
        .disposition = static_cast<std::uint32_t>(
            RenderTapeControlDisposition::Completed),
        .controlBytes = sizeof(read),
        .completionOrdinal = 1u,
    };
    builder.appendOrderedControl(control, std::as_bytes(std::span(&read, 1u)));
  };
  for (const auto identity : {parent, alias}) {
    auto earlyRead = beginPair();
    appendRead(earlyRead, identity);
    appendDf24TestChunk(earlyRead, proof);
    const auto result = reject(finishDf24TestTape(earlyRead),
        RenderTapeValidationStatus::IncompleteFrame,
        RenderTapeIncompleteFrameReason::ProducedProofRejected);
    check(result.failedEventIndex == 4u &&
              result.hasOffendingIdentity &&
              sameWireIdentity(result.offendingIdentity, parent),
          "pre-proof CPU read reports the unresolved generation-qualified parent");
    auto lateRead = beginPair();
    appendDf24TestChunk(lateRead, proof);
    appendRead(lateRead, identity);
    check(validateRenderTape(finishDf24TestTape(lateRead), {}).valid(),
          "DF24 CPU read after its full depth initialization is canonically sound");
  }
}

void textureAliasLogicalSlotsUseParentSubresources() {
  constexpr D9CWireObjectIdentity output{
      .kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
      .generation = 1u,
      .objectId = 0x7700u,
  };
  constexpr D9CWireObjectIdentity texture{
      .kind = D9C_CHUNK_HANDLE_KIND_TEXTURE,
      .generation = 4u,
      .objectId = 0x8800u,
  };
  constexpr D9CWireObjectIdentity firstAlias{
      .kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
      .generation = 2u,
      .objectId = 0x9900u,
  };
  constexpr D9CWireObjectIdentity secondAlias{
      .kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
      .generation = 3u,
      .objectId = firstAlias.objectId,
  };
  const auto outputDescriptor = outputSurfaceDescriptor();
  const std::array<D9CSurfaceDesc, 2u> levels{{
      D9CSurfaceDesc{
          .format = 21u,
          .resourceType = 3u,
          .width = 8u,
          .height = 8u,
          .depth = 1u,
      },
      D9CSurfaceDesc{
          .format = 21u,
          .resourceType = 3u,
          .width = 4u,
          .height = 4u,
          .depth = 1u,
      },
  }};
  const RenderTapeTextureDescriptorV2 textureHeader{
      .schemaVersion = kRenderTapeTextureDescriptorVersion2,
      .dimension =
          static_cast<std::uint32_t>(RenderTapeTextureDimension::Texture2D),
      .mipLevelCount = 2u,
      .subresourceCount = 2u,
      .initialContentDisposition = static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::CompleteSeed),
  };
  std::vector<std::byte> textureDescriptor(sizeof(textureHeader) +
                                           sizeof(levels));
  std::memcpy(textureDescriptor.data(), &textureHeader, sizeof(textureHeader));
  std::memcpy(textureDescriptor.data() + sizeof(textureHeader), levels.data(),
              sizeof(levels));
  const RenderTapeSurfaceDescriptorV2 firstDescriptor{
      .schemaVersion = kRenderTapeSurfaceDescriptorVersion2,
      .storage = static_cast<std::uint32_t>(
          RenderTapeSurfaceStorage::TextureSubresource),
      .initialContentDisposition = static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::Unavailable),
      .subresource = 0u,
      .parentTexture = texture,
      .surface = D9CSurfaceDesc{
          .format = levels[0].format,
          .resourceType = 1u,
          .width = levels[0].width,
          .height = levels[0].height,
          .depth = levels[0].depth,
      },
  };
  const RenderTapeSurfaceDescriptorV2 secondDescriptor{
      .schemaVersion = kRenderTapeSurfaceDescriptorVersion2,
      .storage = static_cast<std::uint32_t>(
          RenderTapeSurfaceStorage::TextureSubresource),
      .initialContentDisposition = static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::Unavailable),
      .subresource = 1u,
      .parentTexture = texture,
      .surface = D9CSurfaceDesc{
          .format = levels[1].format,
          .resourceType = 1u,
          .width = levels[1].width,
          .height = levels[1].height,
          .depth = levels[1].depth,
      },
  };
  const RenderTapeOracleAttachment oracle{
      .identity = output,
      .descriptorKind = static_cast<std::uint32_t>(
          RenderTapeDescriptorKind::Surface),
  };
  const auto textureSeedDigest = mutationDigest();
  const RenderTapeBlobCatalogue catalogue{.blobs = {{
      .digest = textureSeedDigest, .size = 4u, .verified = 1u,
  }}};
  const auto finish = [&](RenderTapeBuilder& builder,
                          std::uint64_t) {
    builder.appendCommandChunk(
        CommandChunkEnvelope{.recordCount = 1u, .handleCount = 0u},
        makePresentChunk());
    builder.appendPresentComplete(
        builder.eventCount(), 1u, RenderTapeDigestValidity::NotCaptured, {},
        std::as_bytes(std::span(&oracle, 1u)));
  };
  const auto defineCommon = [&](RenderTapeBuilder& builder) {
    builder.appendBootstrapState(makeApplyStateChunk());
    builder.appendObjectDefine(
        output, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
        std::as_bytes(std::span(&outputDescriptor, 1u)), 0u, {});
    builder.appendObjectDefine(
        texture, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Texture),
        textureDescriptor, 0u, {}, 8u, 2u);
    builder.appendResourceMutation(
        texture, RenderTapeMutationKind::Upload, 0u, 0u, 4u,
        textureSeedDigest);
    builder.appendResourceMutation(
        texture, RenderTapeMutationKind::Upload, 1u, 0u, 4u,
        textureSeedDigest);
  };

  RenderTapeBuilder coexist;
  defineCommon(coexist);
  coexist.appendObjectDefine(
      firstAlias,
      static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
      std::as_bytes(std::span(&firstDescriptor, 1u)), 0u, {});
  coexist.appendObjectDefine(
      secondAlias,
      static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
      std::as_bytes(std::span(&secondDescriptor, 1u)), 0u, {});
  finish(coexist, 6u);
  const auto coexistResult = validateRenderTape(coexist.seal(), catalogue);
  if (!coexistResult.valid()) {
    throw TestFailure(
        "two live alias subresources may share one wire surface object id: " +
        std::to_string(static_cast<unsigned>(coexistResult.status)) + "/" +
        std::to_string(coexistResult.failedEventIndex));
  }

  auto ordinarySurface = firstAlias;
  ordinarySurface.generation = 20u;
  RenderTapeBuilder mixed;
  defineCommon(mixed);
  mixed.appendObjectDefine(
      firstAlias,
      static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
      std::as_bytes(std::span(&firstDescriptor, 1u)), 0u, {});
  mixed.appendObjectDefine(
      ordinarySurface,
      static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
      std::as_bytes(std::span(&outputDescriptor, 1u)), 0u, {});
  finish(mixed, 6u);
  check(validateRenderTape(mixed.seal(), catalogue).valid(),
        "an alias and ordinary surface may share one wire object id");

  RenderTapeBuilder overlapping;
  defineCommon(overlapping);
  overlapping.appendObjectDefine(
      firstAlias,
      static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
      std::as_bytes(std::span(&firstDescriptor, 1u)), 0u, {});
  overlapping.appendObjectDefine(
      secondAlias,
      static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
      std::as_bytes(std::span(&firstDescriptor, 1u)), 0u, {});
  finish(overlapping, 6u);
  check(validateRenderTape(overlapping.seal(), catalogue).status ==
            RenderTapeValidationStatus::DuplicateGeneration,
        "two live generations of one logical alias slot fail closed");

  RenderTapeBuilder wrongDestroy;
  defineCommon(wrongDestroy);
  wrongDestroy.appendObjectDefine(
      firstAlias,
      static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
      std::as_bytes(std::span(&firstDescriptor, 1u)), 0u, {});
  wrongDestroy.appendObjectDestroy(secondAlias);
  finish(wrongDestroy, 6u);
  check(validateRenderTape(wrongDestroy.seal(), catalogue).status ==
            RenderTapeValidationStatus::UnknownIdentity,
        "alias destruction requires the exact defined identity");

  RenderTapeBuilder replacement;
  defineCommon(replacement);
  replacement.appendObjectDefine(
      firstAlias,
      static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
      std::as_bytes(std::span(&firstDescriptor, 1u)), 0u, {});
  replacement.appendObjectDestroy(firstAlias);
  replacement.appendObjectDefine(
      secondAlias,
      static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
      std::as_bytes(std::span(&firstDescriptor, 1u)), 0u, {});
  finish(replacement, 7u);
  check(validateRenderTape(replacement.seal(), catalogue).valid(),
        "one wire object id admits a monotone alias generation replacement");

  auto highAlias = firstAlias;
  highAlias.generation = 28u;
  auto lowerCrossObject = firstAlias;
  ++lowerCrossObject.objectId;
  lowerCrossObject.generation = 3u;
  RenderTapeBuilder crossObjectReplacement;
  defineCommon(crossObjectReplacement);
  crossObjectReplacement.appendObjectDefine(
      highAlias,
      static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
      std::as_bytes(std::span(&firstDescriptor, 1u)), 0u, {});
  crossObjectReplacement.appendObjectDestroy(highAlias);
  crossObjectReplacement.appendObjectDefine(
      lowerCrossObject,
      static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
      std::as_bytes(std::span(&firstDescriptor, 1u)), 0u, {});
  finish(crossObjectReplacement, 7u);
  check(validateRenderTape(crossObjectReplacement.seal(), catalogue).valid(),
        "event order admits a lower generation from a new alias wire object");

  auto equalFirst = firstAlias;
  equalFirst.generation = 1u;
  auto equalSecond = equalFirst;
  ++equalSecond.objectId;
  RenderTapeBuilder equalCrossObject;
  defineCommon(equalCrossObject);
  equalCrossObject.appendObjectDefine(
      equalFirst,
      static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
      std::as_bytes(std::span(&firstDescriptor, 1u)), 0u, {});
  equalCrossObject.appendObjectDestroy(equalFirst);
  equalCrossObject.appendObjectDefine(
      equalSecond,
      static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
      std::as_bytes(std::span(&firstDescriptor, 1u)), 0u, {});
  finish(equalCrossObject, 7u);
  check(validateRenderTape(equalCrossObject.seal(), catalogue).valid(),
        "event order admits equal generations from distinct alias wire objects");

  RenderTapeBuilder fullHistoryStale;
  defineCommon(fullHistoryStale);
  fullHistoryStale.appendObjectDefine(
      highAlias,
      static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
      std::as_bytes(std::span(&firstDescriptor, 1u)), 0u, {});
  fullHistoryStale.appendObjectDestroy(highAlias);
  fullHistoryStale.appendObjectDefine(
      lowerCrossObject,
      static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
      std::as_bytes(std::span(&firstDescriptor, 1u)), 0u, {});
  fullHistoryStale.appendObjectDestroy(lowerCrossObject);
  auto staleOriginalObject = highAlias;
  --staleOriginalObject.generation;
  fullHistoryStale.appendObjectDefine(
      staleOriginalObject,
      static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
      std::as_bytes(std::span(&firstDescriptor, 1u)), 0u, {});
  finish(fullHistoryStale, 9u);
  check(validateRenderTape(fullHistoryStale.seal(), catalogue).status ==
            RenderTapeValidationStatus::RetainedSlotReuse,
        "same-object monotonicity scans alias history across other wire objects");

  auto historyOne = firstAlias;
  historyOne.generation = 1u;
  auto historyTwo = historyOne;
  historyTwo.generation = 2u;
  auto historyThree = historyOne;
  historyThree.generation = 3u;
  RenderTapeBuilder historyOverlap;
  defineCommon(historyOverlap);
  historyOverlap.appendObjectDefine(
      historyOne,
      static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
      std::as_bytes(std::span(&firstDescriptor, 1u)), 0u, {});
  historyOverlap.appendObjectDestroy(historyOne);
  historyOverlap.appendObjectDefine(
      historyTwo,
      static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
      std::as_bytes(std::span(&firstDescriptor, 1u)), 0u, {});
  historyOverlap.appendObjectDefine(
      historyThree,
      static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
      std::as_bytes(std::span(&firstDescriptor, 1u)), 0u, {});
  finish(historyOverlap, 8u);
  check(validateRenderTape(historyOverlap.seal(), catalogue).status ==
            RenderTapeValidationStatus::DuplicateGeneration,
        "gen1-retired gen2-live gen3 remains an overlapping alias generation");
}

void textureSurfaceAliasesCoverCanonical2DAndStandaloneSurfaces() {
  constexpr D9CWireObjectIdentity texture{
      .kind = D9C_CHUNK_HANDLE_KIND_TEXTURE,
      .generation = 8u,
      .objectId = 0x8800u,
  };
  constexpr D9CWireObjectIdentity aliasSurface{
      .kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
      .generation = 9u,
      .objectId = 0x9900u,
  };
  constexpr D9CWireObjectIdentity standaloneSurface{
      .kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
      .generation = 9u,
      .objectId = 0x9901u,
  };
  constexpr D9CWireObjectIdentity output{
      .kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
      .generation = 1u,
      .objectId = 0x9902u,
  };
  const D9CSurfaceDesc level0{
      .format = 21u,
      .resourceType = 3u,
      .width = 8u,
      .height = 4u,
      .depth = 1u,
  };
  const D9CSurfaceDesc level1{
      .format = 21u,
      .resourceType = 3u,
      .width = 4u,
      .height = 2u,
      .depth = 1u,
  };
  const RenderTapeTextureDescriptorV2 textureDescriptor{
      .schemaVersion = kRenderTapeTextureDescriptorVersion2,
      .dimension = static_cast<std::uint32_t>(
          RenderTapeTextureDimension::Texture2D),
      .mipLevelCount = 2u,
      .subresourceCount = 2u,
      .initialContentDisposition = static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::CompleteSeed),
  };
  std::vector<std::byte> textureDescriptorBytes(
      sizeof(textureDescriptor) + sizeof(level0) + sizeof(level1));
  std::memcpy(textureDescriptorBytes.data(), &textureDescriptor,
              sizeof(textureDescriptor));
  std::memcpy(textureDescriptorBytes.data() + sizeof(textureDescriptor),
              &level0, sizeof(level0));
  std::memcpy(textureDescriptorBytes.data() + sizeof(textureDescriptor) +
                  sizeof(level0),
              &level1, sizeof(level1));
  const RenderTapeSurfaceDescriptorV2 aliasDescriptor{
      .schemaVersion = kRenderTapeSurfaceDescriptorVersion2,
      .storage = static_cast<std::uint32_t>(
          RenderTapeSurfaceStorage::TextureSubresource),
      .initialContentDisposition = static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::Unavailable),
      .subresource = 1u,
      .parentTexture = texture,
      .surface = D9CSurfaceDesc{
          .format = 21u,
          .resourceType = 1u,
          .width = 4u,
          .height = 2u,
          .depth = 1u,
      },
  };
  D9CSurfaceDesc resolvedLevel1{};
  check(renderTapeTextureSubresourceDescriptor(
            textureDescriptorBytes, 1u,
            resolvedLevel1) &&
            resolvedLevel1.width == aliasDescriptor.surface.width &&
            resolvedLevel1.height == aliasDescriptor.surface.height,
        "canonical 2D descriptor resolves the aliased mip dimensions");
  const D9CSurfaceDesc standaloneDescriptor{
      .format = 21u,
      .resourceType = 1u,
      .usage = 1u,
      .width = 4u,
      .height = 4u,
      .depth = 1u,
  };
  const RenderTapeSurfaceDescriptorV2 standaloneDescriptorV2{
      .schemaVersion = kRenderTapeSurfaceDescriptorVersion2,
      .storage = static_cast<std::uint32_t>(
          RenderTapeSurfaceStorage::Standalone),
      .initialContentDisposition = static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::CompleteSeed),
      .surface = standaloneDescriptor,
  };
  const RenderTapeSurfaceDescriptorV2 outputDescriptor{
      .schemaVersion = kRenderTapeSurfaceDescriptorVersion2,
      .storage = static_cast<std::uint32_t>(
          RenderTapeSurfaceStorage::SwapchainBackbuffer),
      .initialContentDisposition = static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::ProducedPresentOutput),
      .surface = standaloneDescriptor,
  };
  const auto firstDigest = digest(std::byte{0x61});
  const auto secondDigest = digest(std::byte{0x71});
  const auto standaloneDigest = digest(std::byte{0x81});
  const RenderTapeBlobCatalogue catalogue{.blobs = {
      RenderTapeBlob{.digest = firstDigest, .size = 128u, .verified = 1u},
      RenderTapeBlob{.digest = secondDigest, .size = 32u, .verified = 1u},
      RenderTapeBlob{.digest = standaloneDigest, .size = 64u, .verified = 1u},
  }};

  RenderTapeBuilder builder;
  builder.appendBootstrapState(makeApplyStateChunk());
  builder.appendObjectDefine(
      output, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
      std::as_bytes(std::span(&outputDescriptor, 1u)), 0u, {});
  builder.appendObjectDefine(
      texture, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Texture),
      textureDescriptorBytes, 0u, {}, 160u, 2u);
  builder.appendObjectDefine(
      aliasSurface, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
      std::as_bytes(std::span(&aliasDescriptor, 1u)), 0u, {});
  builder.appendObjectDefine(
      standaloneSurface,
      static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
      std::as_bytes(std::span(&standaloneDescriptorV2, 1u)), 0u, {}, 64u, 1u);
  builder.appendResourceMutation(texture, RenderTapeMutationKind::Upload, 0u,
                                 0u, 128u, firstDigest);
  builder.appendResourceMutation(texture, RenderTapeMutationKind::Upload, 1u,
                                 0u, 32u, secondDigest);
  builder.appendResourceMutation(standaloneSurface,
                                 RenderTapeMutationKind::Upload, 0u, 0u, 64u,
                                 standaloneDigest);
  const RenderTapeOracleAttachment oracle{
      .identity = output,
      .descriptorKind = static_cast<std::uint32_t>(
          RenderTapeDescriptorKind::Surface),
  };
  builder.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 1u, .handleCount = 0u},
      makePresentChunk());
  builder.appendPresentComplete(
      9u, 1u, RenderTapeDigestValidity::NotCaptured, {},
      std::as_bytes(std::span(&oracle, 1u)));
  const auto aliasResult = validateRenderTape(builder.seal(), catalogue);
  if (!aliasResult.valid()) {
    throw TestFailure(
        "uncompressed texture-level alias resolves through a canonical V2 parent " +
        std::to_string(static_cast<unsigned>(aliasResult.status)) + "/" +
        std::to_string(aliasResult.failedEventIndex));
  }

  auto wrongSubresource = aliasDescriptor;
  wrongSubresource.subresource = 0u;
  wrongSubresource.surface = aliasDescriptor.surface;
  wrongSubresource.surface.width = level1.width;
  wrongSubresource.surface.height = level1.height;
  RenderTapeBuilder rejected;
  rejected.appendBootstrapState(makeApplyStateChunk());
  rejected.appendObjectDefine(
      output, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
      std::as_bytes(std::span(&outputDescriptor, 1u)), 0u, {});
  rejected.appendObjectDefine(
      texture, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Texture),
      textureDescriptorBytes, 0u, {}, 160u, 2u);
  rejected.appendObjectDefine(
      aliasSurface, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
      std::as_bytes(std::span(&wrongSubresource, 1u)), 0u, {});
  rejected.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 1u, .handleCount = 0u},
      makePresentChunk());
  rejected.appendPresentComplete(
      5u, 1u, RenderTapeDigestValidity::NotCaptured, {},
      std::as_bytes(std::span(&oracle, 1u)));
  check(validateRenderTape(rejected.seal(), catalogue).status ==
            RenderTapeValidationStatus::InvalidObjectDefine,
        "texture-level alias keeps exact parent generation and subresource");
}

void textureSurfaceWrapperLifetimeIsIdempotent() {
  RenderTapeSurfaceAliasLifetime alias;
  alias.textureAlias = true;
  check(alias.acquire(), "first texture-derived surface wrapper acquires");
  check(alias.acquire(), "duplicate texture-derived wrapper acquires idempotently");
  check(!alias.releaseWrapper() && alias.wrapperRefs == 1u,
        "releasing one duplicate wrapper keeps the alias live");
  check(!alias.releaseWrapper() && alias.wrapperRefs == 0u &&
            alias.disposition == RenderTapeSurfaceAliasLifetime::Disposition::RetainedAlias,
        "release-to-zero retains the alias until parent retirement");
  check(alias.acquire() && alias.wrapperRefs == 1u &&
            alias.disposition == RenderTapeSurfaceAliasLifetime::Disposition::Live,
        "rewrapping a retained alias does not redefine its identity");
  check(!alias.releaseWrapper() && alias.wrapperRefs == 0u,
        "rewrapped alias returns to retained state without destruction");
  check(alias.retireParent() &&
            alias.disposition == RenderTapeSurfaceAliasLifetime::Disposition::Retired,
        "parent retirement is the alias destroy transition");
  check(!alias.acquire(), "retired alias cannot be resurrected");

  RenderTapeSurfaceAliasLifetime standalone;
  check(standalone.acquire() && standalone.releaseWrapper() &&
            standalone.disposition ==
                RenderTapeSurfaceAliasLifetime::Disposition::Retired,
        "standalone surface remains independently wrapper-owned");
}

void bootstrapProfileShapesAndPagePartitionsAreExhaustive() {
  const auto stateChunk = makeApplyStateChunk();
  ImportedChunkView anchor;
  check(importPrevalidatedCommandChunk(stateChunk, CommandChunkEnvelope{.recordCount = 1u},
                                       anchor), "shape anchor imports");
  const auto anchorBytes = anchor.record(0u).payload;
  const std::vector<std::byte> full(anchorBytes.begin(), anchorBytes.end());
  const D9CCommandChunkWireDrawHeader deltaHeader{
      .sectionCount = 1u, .sectionTableOffset = sizeof(D9CCommandChunkWireDrawHeader),
      .sectionPayloadOffset = sizeof(D9CCommandChunkWireDrawHeader) +
                              sizeof(D9CCommandChunkWireSectionDesc)};
  const D9CCommandChunkWireSectionDesc deltaSection{
      .kind = D9C_COMMAND_CHUNK_SECTION_RENDER_STATE,
      .elementSize = sizeof(D9CCommandChunkWireRenderState), .count = 1u,
      .payloadOffset = deltaHeader.sectionPayloadOffset,
      .byteSize = sizeof(D9CCommandChunkWireRenderState)};
  const D9CCommandChunkWireRenderState state{.state = 7u, .value = 1u};
  std::vector<std::byte> delta(deltaHeader.sectionPayloadOffset + sizeof(state));
  std::memcpy(delta.data(), &deltaHeader, sizeof(deltaHeader));
  std::memcpy(delta.data() + deltaHeader.sectionTableOffset, &deltaSection,
              sizeof(deltaSection));
  std::memcpy(delta.data() + deltaHeader.sectionPayloadOffset, &state, sizeof(state));
  const D9CCommandChunkWireDrawHeader emptyHeader{
      .sectionTableOffset = sizeof(D9CCommandChunkWireDrawHeader),
      .sectionPayloadOffset = sizeof(D9CCommandChunkWireDrawHeader)};
  std::vector<std::byte> empty(sizeof(emptyHeader));
  std::memcpy(empty.data(), &emptyHeader, sizeof(emptyHeader));
  const std::array payloads{full, delta, empty};
  const auto pack = [](std::span<const std::vector<std::byte>> records) {
    const auto handlesOffset = alignUp(sizeof(D9CCommandChunkWireHeader) +
        records.size() * sizeof(D9CCommandChunkWireRecordHeader),
        alignof(D9CCommandChunkWireHandleEntry));
    const auto arenaOffset = alignUp(handlesOffset, alignof(std::uint32_t));
    std::vector<std::byte> bytes(arenaOffset);
    std::uint32_t arenaBytes = 0u;
    for (std::size_t index = 0u; index < records.size(); ++index) {
      const D9CCommandChunkWireRecordHeader record{
          .type = D9C_COMMAND_RECORD_APPLY_STATE, .payloadOffset = arenaBytes,
          .payloadSize = static_cast<std::uint32_t>(records[index].size())};
      std::memcpy(bytes.data() + sizeof(D9CCommandChunkWireHeader) +
                      index * sizeof(record), &record, sizeof(record));
      bytes.insert(bytes.end(), records[index].begin(), records[index].end());
      arenaBytes += record.payloadSize;
    }
    const D9CCommandChunkWireHeader header{
        .version = D9C_COMMAND_CHUNK_WIRE_VERSION,
        .headerSize = D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE,
        .recordHeaderSize = D9C_COMMAND_CHUNK_WIRE_RECORD_HEADER_SIZE,
        .handleEntrySize = D9C_COMMAND_CHUNK_WIRE_HANDLE_ENTRY_SIZE,
        .recordTableOffset = sizeof(D9CCommandChunkWireHeader),
        .recordCount = static_cast<std::uint32_t>(records.size()),
        .handleTableOffset = static_cast<std::uint32_t>(handlesOffset),
        .payloadArenaOffset = static_cast<std::uint32_t>(arenaOffset),
        .payloadArenaSize = arenaBytes};
    std::memcpy(bytes.data(), &header, sizeof(header));
    return bytes;
  };
  const auto tapeWithBootstrap = [&](std::span<const std::byte> overlays,
                                      std::uint32_t version) {
    RenderTapeBuilder builder;
    builder.appendBootstrapState(overlays, {}, version);
    const auto descriptor = outputSurfaceDescriptor();
    builder.appendObjectDefine(kSurface,
        static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
        std::as_bytes(std::span(&descriptor, 1u)), 0u, {});
    builder.appendCommandChunk(CommandChunkEnvelope{.recordCount = 1u}, makePresentChunk());
    const RenderTapeOracleAttachment oracle{
        .identity = kSurface,
        .descriptorKind = static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface)};
    builder.appendPresentComplete(3u, 1u, RenderTapeDigestValidity::NotCaptured, {},
                                  std::as_bytes(std::span(&oracle, 1u)));
    return builder.seal();
  };
  std::uint32_t cases = 0u;
  for (const auto version : {kRenderTapeLegacyBaselineProfileVersion,
                             kRenderTapeBaselineProfileVersion}) {
    for (std::uint32_t length = 1u, words = 3u; length <= 4u; ++length, words *= 3u) {
      for (std::uint32_t word = 0u; word < words; ++word) {
        std::vector<std::vector<std::byte>> records;
        auto digits = word;
        bool expected = true;
        bool predicateAccepted = true;
        for (std::uint32_t index = 0u; index < length; ++index, digits /= 3u) {
          const auto symbol = digits % 3u;
          records.push_back(payloads[symbol]);
          expected &= version == kRenderTapeLegacyBaselineProfileVersion
              ? symbol == 0u : symbol == (index == 0u ? 0u : 1u);
          predicateAccepted &= renderTapeBootstrapRecordShapeValid(version, index,
              symbol == 0u ? D9C_COMMAND_CHUNK_DRAW_FLAG_FULL_SNAPSHOT : 0u,
              symbol == 2u ? 0u : 1u);
        }
        check(predicateAccepted == expected,
              "shared shape predicate refines the independent full-anchor/delta language");
        for (std::uint32_t cuts = 0u; cuts < (1u << (length - 1u)); ++cuts) {
          std::vector<std::byte> overlays;
          std::size_t first = 0u;
          for (std::size_t index = 0u; index < records.size(); ++index) {
            if (index + 1u != records.size() && (cuts & (1u << index)) == 0u) continue;
            const auto chunk = pack(std::span(records).subspan(first, index + 1u - first));
            overlays.insert(overlays.end(), chunk.begin(), chunk.end());
            first = index + 1u;
          }
          const auto result = validateRenderTape(tapeWithBootstrap(overlays, version), {});
          check(result.valid() == expected &&
                    (expected || result.status == RenderTapeValidationStatus::InvalidBootstrapChunk),
                "shared flattened bootstrap predicate matches every small trace/chunk partition");
          ++cases;
        }
      }
    }
    const auto emptyChunk = pack({});
    auto overlays = stateChunk;
    overlays.insert(overlays.end(), emptyChunk.begin(), emptyChunk.end());
    check(validateRenderTape(tapeWithBootstrap(overlays, version), {}).valid() ==
              (version == kRenderTapeLegacyBaselineProfileVersion),
          "legacy empty overlay remains readable; profile3 empty overlay fails closed");
    check(validateRenderTape(tapeWithBootstrap(emptyChunk, version), {}).valid() ==
              (version == kRenderTapeLegacyBaselineProfileVersion),
          "legacy empty-only overlay remains readable; profile3 requires the anchor");
  }
  check(cases == 1554u, "flattened shape checker covers all 1554 bounded traces/partitions");
  for (const auto version : {kRenderTapeLegacyBaselineProfileVersion,
                             kRenderTapeBaselineProfileVersion, 4u}) {
    for (const auto flags : {2u, 3u, 0xffffffffu})
      check(!renderTapeBootstrapRecordShapeValid(version, 0u, flags, 1u),
            "shape predicate rejects unknown flags and unsupported profiles");
  }
  check(!renderTapeBootstrapRecordShapeValid(4u, 0u,
              D9C_COMMAND_CHUNK_DRAW_FLAG_FULL_SNAPSHOT, 1u),
        "unsupported baseline cannot acquire anchor authority");
  auto zeroCount = delta;
  auto* zeroSection = reinterpret_cast<D9CCommandChunkWireSectionDesc*>(
      zeroCount.data() + deltaHeader.sectionTableOffset);
  zeroSection->count = 0u;
  zeroSection->byteSize = 0u;
  zeroCount.resize(deltaHeader.sectionPayloadOffset);
  const std::array zeroRecords{full, zeroCount};
  const auto zeroResult = validateRenderTape(
      tapeWithBootstrap(pack(zeroRecords), kRenderTapeBaselineProfileVersion), {});
  check(zeroResult.status == RenderTapeValidationStatus::InvalidBootstrapChunk &&
            zeroResult.chunkStatus == CommandChunkValidationStatus::InvalidSectionSchema,
        "canonical validation rejects zero-count-only continuation before shape policy");

  for (std::uint32_t total = 0u; total <= 16u; ++total) {
    for (std::uint32_t cap = 1u; cap <= 5u; ++cap) {
      std::uint32_t next = 0u;
      std::array<std::uint32_t, 16u> visits{};
      for (std::uint32_t pageIndex = 0u;; ++pageIndex) {
        const auto page = renderTapeBootstrapPageSpan(total, cap, pageIndex);
        if (page.count == 0u) break;
        check(page.first == next && page.count <= cap && page.count <= total - next,
              "shared page spans are bounded and adjacent");
        for (std::uint32_t index = page.first; index < page.first + page.count; ++index)
          ++visits[index];
        next += page.count;
      }
      check(next == total, "shared page spans cover the entire source table");
      for (std::uint32_t index = 0u; index < total; ++index)
        check(visits[index] == 1u, "each source state row appears exactly once");
    }
  }
  check(renderTapeBootstrapPageSpan(16u, 0u, 0u).count == 0u &&
            renderTapeBootstrapPageSpan(0xffffffffu, 0xffffffffu, 2u).count == 0u &&
            renderTapeBootstrapPageSpan(0xffffffffu, 64u, 0x03ffffffu).count == 63u,
        "page span zero-cap and widened overflow edges remain bounded");
}

void invalidInputsFailBeforeCallbacks() {
  const auto validCatalogue = completeCatalogue();

  auto incomplete = makeCompleteTape();
  eventPayload<RenderTapeBootstrapHeader>(incomplete, 0u)
      ->requiredCategoryMask &= ~std::uint64_t{1};
  check(validateRenderTape(incomplete, validCatalogue).status ==
            RenderTapeValidationStatus::BootstrapCoverageIncomplete,
        "missing bootstrap category must fail");

  auto unknownBaseline = makeCompleteTape();
  eventPayload<RenderTapeBootstrapHeader>(unknownBaseline, 0u)
      ->baselineProfileVersion = kRenderTapeBaselineProfileVersion + 1u;
  check(validateRenderTape(unknownBaseline, validCatalogue).status ==
            RenderTapeValidationStatus::InvalidBootstrap,
        "unknown bootstrap baseline must fail");

  auto sparseBootstrap = makeCompleteTape();
  bootstrapDrawHeader(sparseBootstrap)->flags = 0u;
  check(validateRenderTape(sparseBootstrap, validCatalogue).status ==
            RenderTapeValidationStatus::InvalidBootstrapChunk,
        "bootstrap APPLY_STATE must retain FULL_SNAPSHOT semantics");

  RenderTapeBuilder forbidden;
  const auto present = makePresentChunk();
  forbidden.appendBootstrapState(present);
  const auto descriptor = outputSurfaceDescriptor();
  forbidden.appendObjectDefine(
      kSurface, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
      std::as_bytes(std::span(&descriptor, 1u)), 0u, {});
  forbidden.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 1u, .handleCount = 0u}, present);
  const RenderTapeOracleAttachment oracle{.identity = kSurface,
                                          .descriptorKind = static_cast<std::uint32_t>(
                                              RenderTapeDescriptorKind::Surface)};
  forbidden.appendPresentComplete(
      3u, 1u, RenderTapeDigestValidity::NotCaptured, {},
      std::as_bytes(std::span(&oracle, 1u)));
  check(validateRenderTape(forbidden.seal(), {}).status ==
            RenderTapeValidationStatus::BootstrapForbiddenRecord,
        "bootstrap must reject Present records");

  const auto tape = makeCompleteTape();
  check(validateRenderTape(tape, {}).status ==
            RenderTapeValidationStatus::UnknownBlob,
        "missing mutation blob must fail");
  check(validateRenderTape(tape, completeCatalogue(5u)).status ==
            RenderTapeValidationStatus::BlobSizeMismatch,
        "blob size mismatch must fail");
  check(validateRenderTape(tape, completeCatalogue(4u, false)).status ==
            RenderTapeValidationStatus::BlobDigestMismatch,
        "unverified blob must fail");

  auto stale = kSurface;
  ++stale.generation;
  check(validateRenderTape(makeCompleteTape(stale), validCatalogue).status ==
            RenderTapeValidationStatus::UnknownIdentity,
        "stale mutation generation must fail");

  auto shaderDefineWithoutPayload = makeCompleteTape();
  eventPayload<RenderTapeObjectDefineHeader>(shaderDefineWithoutPayload, 1u)
      ->identity.kind = D9C_CHUNK_HANDLE_KIND_SHADER;
  check(validateRenderTape(shaderDefineWithoutPayload, validCatalogue).status ==
            RenderTapeValidationStatus::InvalidObjectDefine,
        "shader definitions must carry immutable payload bytes and digest");

  auto invalidMutationObject = kSurface;
  invalidMutationObject.kind = D9C_CHUNK_HANDLE_KIND_QUERY;
  check(validateRenderTape(makeCompleteTape(invalidMutationObject),
                           validCatalogue)
            .status == RenderTapeValidationStatus::InvalidMutationKind,
        "queries cannot be resource-mutation targets");
  check(validateRenderTape(
            makeCompleteTape(kSurface,
                             std::numeric_limits<std::uint64_t>::max(), 2u),
            completeCatalogue(2u))
            .status == RenderTapeValidationStatus::InvalidMutationRange,
        "overflowed mutation range must fail");

  check(validateRenderTape(
            makeCompleteTape(kSurface, 0u, 4u,
                             sizeof(RenderTapeFlushWaitControl) - 1u),
            validCatalogue)
            .status == RenderTapeValidationStatus::InvalidControlSize,
        "wrong ordered-control size must fail");

  auto invalidControlDisposition = makeCompleteTape();
  eventPayload<RenderTapeOrderedControlHeader>(invalidControlDisposition, 4u)
      ->disposition = static_cast<std::uint32_t>(
          RenderTapeControlDisposition::Terminal);
  check(validateRenderTape(invalidControlDisposition, validCatalogue).status ==
            RenderTapeValidationStatus::InvalidControlKind,
        "flush/wait cannot carry a terminal disposition");
  check(validateRenderTape(
            makeCompleteTape(kSurface, 0u, 4u,
                             sizeof(RenderTapeFlushWaitControl), 10u, 9u),
            validCatalogue)
            .status == RenderTapeValidationStatus::InvalidPresentComplete,
        "completion regression must fail");

  auto malformedPresent = makeCompleteTape();
  eventPayload<RenderTapePresentCompleteHeader>(malformedPresent, 5u)
      ->digestValidity = 99u;
  check(validateRenderTape(malformedPresent, validCatalogue).status ==
            RenderTapeValidationStatus::InvalidPresentComplete,
        "unknown Present digest validity must fail");

  auto missingOracle = makeCompleteTape();
  eventPayload<RenderTapePresentCompleteHeader>(missingOracle, 5u)
      ->oracleCount = 0u;
  check(validateRenderTape(missingOracle, validCatalogue).status ==
            RenderTapeValidationStatus::InvalidPresentComplete,
        "PresentComplete must identify at least one oracle attachment");

  auto mismatchedOracle = makeCompleteTape();
  auto* mismatchedAttachment = reinterpret_cast<RenderTapeOracleAttachment*>(
      eventPayload<RenderTapePresentCompleteHeader>(mismatchedOracle, 5u) +
      1u);
  ++mismatchedAttachment->descriptorKind;
  check(validateRenderTape(mismatchedOracle, validCatalogue).status ==
            RenderTapeValidationStatus::InvalidPresentComplete,
        "oracle attachment descriptor must match its object definition");

  auto trailingPayload = makeCompleteTape();
  trailingPayload.push_back(std::byte{});
  ++reinterpret_cast<RenderTapeHeader*>(trailingPayload.data())
        ->payloadArenaSize;
  check(validateRenderTape(trailingPayload, validCatalogue).status ==
            RenderTapeValidationStatus::NonCanonicalEventLayout,
        "unreferenced trailing payload bytes must fail");

  RecordingSink sink;
  ImportedRenderTapeView imported;
  const auto rejected = validateRenderTape(incomplete, validCatalogue, &imported);
  if (rejected.valid()) {
    static_cast<void>(replayPrevalidatedRenderTape(imported, validCatalogue,
                                                   sink));
  }
  check(sink.calls.empty(), "validation failure must produce zero callbacks");
}

void boundedCaptureReplayRefinementIsExhaustive() {
  enum class SeedCase : std::uint8_t {
    Upload,
    CpuUnlock,
    Missing,
    Short,
    Stale,
  };
  enum class BoundaryCase : std::uint8_t {
    Upload,
    CpuUnlock,
    Missing,
    Stale,
    Duplicate,
  };

  constexpr std::array<std::byte, 8u> ordinaryDescriptor{};
  const auto surfaceDescriptor = outputSurfaceDescriptor();
  const auto surfaceDescriptorBytes =
      std::as_bytes(std::span(&surfaceDescriptor, 1u));
  const auto seededTextureDescriptor = texture2DDescriptor(
      1u, RenderTapeInitialContentDisposition::CompleteSeed);
  const auto canonicalTextureDescriptor = texture2DDescriptor();
  const auto initialDigest = mutationDigest();
  const auto boundaryDigest = digest(std::byte{0x50});
  const auto shortDigest = digest(std::byte{0x70});
  const RenderTapeBlobCatalogue catalogue{.blobs = {
      RenderTapeBlob{.digest = initialDigest, .size = 4u, .verified = 1u},
      RenderTapeBlob{.digest = boundaryDigest, .size = 4u, .verified = 1u},
      RenderTapeBlob{.digest = shortDigest, .size = 3u, .verified = 1u},
  }};
  auto staleTexture = kTexture;
  ++staleTexture.generation;
  auto staleSurface = kSurface;
  ++staleSurface.generation;
  std::uint32_t checkedCases = 0u;

  const auto expectStatus = [&](const std::vector<std::byte>& tape,
                                RenderTapeValidationStatus expected,
                                std::string_view label,
                                std::uint32_t expectedMutations,
                                std::uint32_t expectedDraws,
                                std::uint32_t expectedPresents,
                                RenderTapeDigest expectedLastDigest) {
    ++checkedCases;
    ImportedRenderTapeView imported;
    const auto result = validateRenderTape(tape, catalogue, &imported);
    check(result.status == expected,
          std::string(label) + ": expected " +
              renderTapeValidationStatusName(expected) + ", got " +
              renderTapeValidationStatusName(result.status));
    if (!result.valid()) return;

    BoundedRefinementSink sink;
    const auto replay = replayPrevalidatedRenderTape(imported, catalogue, sink);
    check(replay.complete && sink.mutations == expectedMutations &&
              sink.draws == expectedDraws &&
              sink.presents == expectedPresents &&
              sink.completions == expectedPresents &&
              sink.lastMutationDigest == expectedLastDigest,
          std::string(label) +
              ": accepted production replay must refine the serial state");
  };

  // Domain A (20 cases): five initial-content states x live/stale draw x
  // live/stale output. This includes create-write-draw-Present, both admitted
  // seed mutation kinds, missing/short initial bytes, and stale references.
  for (const auto seedCase : {SeedCase::Upload, SeedCase::CpuUnlock,
                              SeedCase::Missing, SeedCase::Short,
                              SeedCase::Stale}) {
    for (const bool staleDraw : {false, true}) {
      for (const bool staleOracle : {false, true}) {
        RenderTapeBuilder builder;
        builder.appendBootstrapState(makeApplyStateChunk());
        builder.appendObjectDefine(kSurface, kSurfaceDescriptorKind,
                                   surfaceDescriptorBytes, 0u, {});
        builder.appendObjectDefine(kTexture, kTextureDescriptorKind,
                                   seededTextureDescriptor, 0u, {}, 4u, 1u);
        switch (seedCase) {
        case SeedCase::Upload:
        case SeedCase::CpuUnlock:
          builder.appendResourceMutation(
              kTexture,
              seedCase == SeedCase::Upload
                  ? RenderTapeMutationKind::Upload
                  : RenderTapeMutationKind::CpuUnlock,
              0u, 0u, 4u, initialDigest);
          break;
        case SeedCase::Missing:
          break;
        case SeedCase::Short:
          builder.appendResourceMutation(kTexture,
                                         RenderTapeMutationKind::Upload, 0u,
                                         0u, 3u, shortDigest);
          break;
        case SeedCase::Stale:
          builder.appendResourceMutation(staleTexture,
                                         RenderTapeMutationKind::Upload, 0u,
                                         0u, 4u, initialDigest);
          break;
        }
        builder.appendCommandChunk(
            CommandChunkEnvelope{.recordCount = 1u, .handleCount = 1u},
            makeDrawChunk(staleDraw ? staleTexture : kTexture));
        builder.appendCommandChunk(
            CommandChunkEnvelope{.recordCount = 1u, .handleCount = 0u},
            makePresentChunk());
        const auto presentOrdinal = builder.eventCount();
        const RenderTapeOracleAttachment oracle{
            .identity = staleOracle ? staleSurface : kSurface,
            .descriptorKind = kSurfaceDescriptorKind,
        };
        builder.appendPresentComplete(
            presentOrdinal, presentOrdinal + 1u,
            RenderTapeDigestValidity::NotCaptured, {},
            std::as_bytes(std::span(&oracle, 1u)));

        auto expected = RenderTapeValidationStatus::Valid;
        if (seedCase == SeedCase::Stale || staleDraw) {
          expected = RenderTapeValidationStatus::UnknownIdentity;
        } else if (seedCase == SeedCase::Missing || seedCase == SeedCase::Short) {
          expected = RenderTapeValidationStatus::IncompleteFrame;
        } else if (staleOracle) {
          expected = RenderTapeValidationStatus::InvalidPresentComplete;
        }
        const auto tape = builder.seal();
        if (seedCase == SeedCase::Missing && !staleDraw && !staleOracle) {
          const auto detail = validateRenderTape(tape, catalogue);
          check(detail.status == RenderTapeValidationStatus::IncompleteFrame &&
                    detail.incompleteFrameReason ==
                        RenderTapeIncompleteFrameReason::
                            ReferencedSeedIncomplete &&
                    detail.failedEventType == static_cast<std::uint32_t>(
                        RenderTapeEventType::CommandChunk) &&
                    detail.hasOffendingIdentity &&
                    detail.offendingIdentity.kind == kTexture.kind &&
                    detail.offendingIdentity.generation ==
                        kTexture.generation &&
                    detail.offendingIdentity.objectId == kTexture.objectId,
                "IncompleteFrame attributes the exact command event and missing seed identity");
        }
        expectStatus(tape, expected, "bounded frame trace", 1u, 1u, 1u,
                     initialDigest);
      }
    }
  }

  // Domain B (10 cases): five between-Present mutation states x live/stale
  // second draw. Exactly one valid digest-backed mutation must separate the
  // two complete intervals.
  for (const auto boundaryCase : {
           BoundaryCase::Upload, BoundaryCase::CpuUnlock,
           BoundaryCase::Missing, BoundaryCase::Stale,
           BoundaryCase::Duplicate}) {
    for (const bool staleSecondDraw : {false, true}) {
      RenderTapeBuilder builder(kRenderTapeProfileSequence);
      builder.appendBootstrapState(makeApplyStateChunk());
      builder.appendObjectDefine(kSurface, kSurfaceDescriptorKind, surfaceDescriptorBytes,
                                 0u, {});
      builder.appendObjectDefine(kTexture, kTextureDescriptorKind, seededTextureDescriptor,
                                 0u, {}, 4u, 1u);
      builder.appendResourceMutation(kTexture, RenderTapeMutationKind::Upload,
                                     0u, 0u, 4u, initialDigest);
      builder.appendCommandChunk(
          CommandChunkEnvelope{.recordCount = 1u, .handleCount = 1u},
          makeDrawChunk(kTexture));
      builder.appendCommandChunk(
          CommandChunkEnvelope{.recordCount = 1u, .handleCount = 0u},
          makePresentChunk());
      auto presentOrdinal = builder.eventCount();
      const RenderTapeOracleAttachment oracle{
          .identity = kSurface,
          .descriptorKind = kSurfaceDescriptorKind,
      };
      builder.appendPresentComplete(
          presentOrdinal, presentOrdinal + 1u,
          RenderTapeDigestValidity::NotCaptured, {},
          std::as_bytes(std::span(&oracle, 1u)));

      if (boundaryCase != BoundaryCase::Missing) {
        builder.appendResourceMutation(
            boundaryCase == BoundaryCase::Stale ? staleTexture : kTexture,
            boundaryCase == BoundaryCase::CpuUnlock
                ? RenderTapeMutationKind::CpuUnlock
                : RenderTapeMutationKind::Upload,
            0u, 0u, 4u, boundaryDigest);
      }
      if (boundaryCase == BoundaryCase::Duplicate) {
        builder.appendResourceMutation(kTexture,
                                       RenderTapeMutationKind::CpuUnlock, 0u,
                                       0u, 4u, boundaryDigest);
      }
      builder.appendCommandChunk(
          CommandChunkEnvelope{.recordCount = 1u, .handleCount = 1u},
          makeDrawChunk(staleSecondDraw ? staleTexture : kTexture));
      builder.appendCommandChunk(
          CommandChunkEnvelope{.recordCount = 1u, .handleCount = 0u},
          makePresentChunk());
      presentOrdinal = builder.eventCount();
      builder.appendPresentComplete(
          presentOrdinal, presentOrdinal + 1u,
          RenderTapeDigestValidity::NotCaptured, {},
          std::as_bytes(std::span(&oracle, 1u)));

      auto expected = RenderTapeValidationStatus::Valid;
      if (boundaryCase == BoundaryCase::Missing ||
          boundaryCase == BoundaryCase::Duplicate) {
        expected = RenderTapeValidationStatus::IncompleteFrame;
      } else if (boundaryCase == BoundaryCase::Stale || staleSecondDraw) {
        expected = RenderTapeValidationStatus::UnknownIdentity;
      }
      expectStatus(builder.seal(), expected, "bounded sequence trace", 2u,
                   2u, 2u, boundaryDigest);
    }
  }

  // Domain C (12 registry decisions): every production identity kind across
  // stale-old/new-generation resolution after destroy/recreate.
  for (std::uint32_t kind = D9C_CHUNK_HANDLE_KIND_TEXTURE;
       kind <= D9C_CHUNK_HANDLE_KIND_QUERY; ++kind) {
    WireObjectRegistry registry;
    RefinementRegistryObject oldObject;
    RefinementRegistryObject replacement;
    const auto oldIdentity = registry.insert(kind, &oldObject);
    check(registry.erase(oldIdentity, &oldObject),
          "bounded registry trace must destroy the old generation");
    const auto newIdentity = registry.insert(kind, &replacement);
    check(newIdentity.objectId == oldIdentity.objectId &&
              newIdentity.generation == oldIdentity.generation + 1u,
          "bounded registry trace must reuse only with generation advance");
    for (const auto identity : {oldIdentity, newIdentity}) {
      const std::array entry{wireHandleEntry(identity)};
      std::array<void*, 1u> resolved{};
      const bool accepted = registry.resolveAndRetain(
          entry, resolved, retainRefinementObject);
      ++checkedCases;
      check(accepted == (identity.generation == newIdentity.generation),
            "bounded registry trace must reject stale and admit current generation");
    }
    check(oldObject.retains == 0u && replacement.retains == 1u,
          "stale registry rejection must happen before retain effects");
  }

  // Domain D (3 tape decisions): tape validation follows the same sequential
  // generation contract as the production registry for each mutable kind.
  for (const std::uint32_t kind : {
           std::uint32_t{D9C_CHUNK_HANDLE_KIND_TEXTURE},
           std::uint32_t{D9C_CHUNK_HANDLE_KIND_SURFACE},
           std::uint32_t{D9C_CHUNK_HANDLE_KIND_BUFFER}}) {
    const D9CWireObjectIdentity oldIdentity{
        .kind = kind,
        .generation = 1u,
        .objectId = 101u + kind,
    };
    auto newIdentity = oldIdentity;
    ++newIdentity.generation;
    RenderTapeBuilder builder;
    builder.appendBootstrapState(makeApplyStateChunk());
    builder.appendObjectDefine(kSurface, kSurfaceDescriptorKind, surfaceDescriptorBytes,
                               0u, {});
    const std::span<const std::byte> objectDescriptor =
        kind == D9C_CHUNK_HANDLE_KIND_TEXTURE
            ? std::span<const std::byte>(canonicalTextureDescriptor)
            : kind == D9C_CHUNK_HANDLE_KIND_SURFACE
                  ? surfaceDescriptorBytes
                  : std::span<const std::byte>(ordinaryDescriptor);
    builder.appendObjectDefine(
        oldIdentity,
        static_cast<std::uint32_t>(renderTapeDescriptorKindForObject(kind)),
        objectDescriptor, 0u, {},
        kind == D9C_CHUNK_HANDLE_KIND_TEXTURE ? 4u : 0u,
        kind == D9C_CHUNK_HANDLE_KIND_TEXTURE ? 1u : 0u);
    if (kind == D9C_CHUNK_HANDLE_KIND_TEXTURE) {
      builder.appendResourceMutation(oldIdentity,
                                     RenderTapeMutationKind::Upload, 0u, 0u,
                                     4u, initialDigest);
    }
    builder.appendObjectDestroy(oldIdentity);
    builder.appendObjectDefine(
        newIdentity,
        static_cast<std::uint32_t>(renderTapeDescriptorKindForObject(kind)),
        objectDescriptor, 0u, {},
        kind == D9C_CHUNK_HANDLE_KIND_TEXTURE ? 4u : 0u,
        kind == D9C_CHUNK_HANDLE_KIND_TEXTURE ? 1u : 0u);
    if (kind == D9C_CHUNK_HANDLE_KIND_TEXTURE) {
      builder.appendResourceMutation(newIdentity,
                                     RenderTapeMutationKind::Upload, 0u, 0u,
                                     4u, initialDigest);
    }
    builder.appendCommandChunk(
        CommandChunkEnvelope{.recordCount = 1u, .handleCount = 0u},
        makePresentChunk());
    const auto presentOrdinal = builder.eventCount();
    const RenderTapeOracleAttachment oracle{
        .identity = kSurface,
        .descriptorKind = kSurfaceDescriptorKind,
    };
    builder.appendPresentComplete(
        presentOrdinal, presentOrdinal + 1u,
        RenderTapeDigestValidity::NotCaptured, {},
        std::as_bytes(std::span(&oracle, 1u)));
    ++checkedCases;
    const auto validation = validateRenderTape(builder.seal(), catalogue);
    check(validation.valid(),
          std::string("bounded retained-tape generation trace: expected valid, got ") +
              renderTapeValidationStatusName(validation.status));
  }

  check(checkedCases == 45u,
        "bounded refinement domain must remain explicit and exhaustive");
}

void wholeEventReductionIsCanonicalAndFailClosed() {
  const auto surfaceDescriptor = outputSurfaceDescriptor();
  const auto surfaceDescriptorBytes =
      std::as_bytes(std::span(&surfaceDescriptor, 1u));
  const auto textureDescriptor = texture2DDescriptor(
      1u, RenderTapeInitialContentDisposition::CompleteSeed);
  const auto seedDigest = mutationDigest();
  const RenderTapeBlobCatalogue catalogue{.blobs = {{
      .digest = seedDigest,
      .size = 4u,
      .verified = 1u,
  }}};
  const RenderTapeOracleAttachment oracle{
      .identity = kSurface,
      .descriptorKind = kSurfaceDescriptorKind,
  };

  RenderTapeBuilder builder;
  builder.appendBootstrapState(makeApplyStateChunk());
  builder.appendObjectDefine(kSurface, kSurfaceDescriptorKind, surfaceDescriptorBytes, 0u,
                             {});
  builder.appendObjectDefine(kTexture, kTextureDescriptorKind, textureDescriptor, 0u,
                             {}, 4u, 1u);
  builder.appendResourceMutation(kTexture, RenderTapeMutationKind::Upload, 0u,
                                 0u, 4u, seedDigest);
  const auto textureState = makeApplyStateChunk(true, kTexture);
  builder.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 1u, .handleCount = 1u},
      textureState);
  builder.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 1u, .handleCount = 0u},
      makePresentChunk());
  builder.appendPresentComplete(
      6u, 7u, RenderTapeDigestValidity::NotCaptured, {},
      std::as_bytes(std::span(&oracle, 1u)));
  const auto source = builder.seal();
  check(validateRenderTape(source, catalogue).valid(),
        "reducer source fixture must validate");

  const std::array bothCommands{4u, 5u};
  const auto complete = reduceRenderTape(source, catalogue, bothCommands);
  check(complete.valid(), renderTapeReductionStatusName(complete.status));
  check(complete.retainedSourceEventIndices ==
            std::vector<std::uint32_t>({0u, 1u, 2u, 3u, 4u, 5u, 6u}),
        "selected command closure must retain exact definitions and seed mutation");
  check(complete.referencedBlobDigests ==
            std::vector<RenderTapeDigest>({seedDigest}),
        "selected resource closure must retain its exact seed blob");
  check(validateRenderTape(complete.bytes, catalogue).valid(),
        "complete reduced tape must remain canonical");

  const std::array presentOnly{5u};
  const auto pruned = reduceRenderTape(source, catalogue, presentOnly);
  const auto repeated = reduceRenderTape(source, catalogue, presentOnly);
  check(pruned.valid() && repeated.valid() && pruned.bytes == repeated.bytes,
        "identical whole-event selection must produce byte-identical output");
  check(pruned.retainedSourceEventIndices ==
            std::vector<std::uint32_t>({0u, 1u, 5u, 6u}) &&
            pruned.referencedBlobDigests.empty(),
        "unreachable resource definition, seed mutation, and blob must be pruned");
  ImportedRenderTapeView imported;
  check(validateRenderTape(pruned.bytes, {}, &imported).valid(),
        "pruned tape must validate against only its referenced catalogue");
  RenderTapePresentCompleteHeader fixed{};
  std::memcpy(&fixed, imported.event(3u).payload.data(), sizeof(fixed));
  check(fixed.presentOrdinal == 3u && fixed.completionOrdinal == 7u,
        "reduction must patch the Present event ordinal and preserve completion");

  const std::array duplicate{5u, 5u};
  check(reduceRenderTape(source, catalogue, duplicate).status ==
            RenderTapeReductionStatus::InvalidSelection,
        "duplicate selections must fail closed");
  const std::array nonCommand{0u};
  check(reduceRenderTape(source, catalogue, nonCommand).status ==
            RenderTapeReductionStatus::InvalidSelection,
        "non-command selections must fail closed");
  const std::array noPresent{4u};
  check(reduceRenderTape(source, catalogue, noPresent).status ==
            RenderTapeReductionStatus::MissingPresentSelection,
        "selection without the Present command must fail closed");
  const std::array outOfRange{999u};
  check(reduceRenderTape(source, catalogue, outOfRange).status ==
            RenderTapeReductionStatus::InvalidSelection,
        "out-of-range selections must fail closed");

  RenderTapeBuilder liveMutation;
  const D9CWireObjectIdentity unrelatedOldTexture{
      .kind = D9C_CHUNK_HANDLE_KIND_TEXTURE,
      .generation = 1u,
      .objectId = 0x9021u,
  };
  auto unrelatedNewTexture = unrelatedOldTexture;
  ++unrelatedNewTexture.generation;
  liveMutation.appendBootstrapState(makeApplyStateChunk());
  liveMutation.appendObjectDefine(kSurface, kSurfaceDescriptorKind, surfaceDescriptorBytes,
                                  0u, {});
  liveMutation.appendObjectDefine(kTexture, kTextureDescriptorKind, textureDescriptor,
                                  0u, {}, 4u, 1u);
  liveMutation.appendObjectDefine(unrelatedOldTexture, kTextureDescriptorKind,
                                  textureDescriptor, 0u, {}, 4u, 1u);
  liveMutation.appendResourceMutation(unrelatedOldTexture,
                                      RenderTapeMutationKind::Upload, 0u, 0u,
                                      4u, seedDigest);
  liveMutation.appendObjectDestroy(unrelatedOldTexture);
  liveMutation.appendObjectDefine(unrelatedNewTexture, kTextureDescriptorKind,
                                  textureDescriptor, 0u, {}, 4u, 1u);
  liveMutation.appendResourceMutation(unrelatedNewTexture,
                                      RenderTapeMutationKind::Upload, 0u, 0u,
                                      4u, seedDigest);
  liveMutation.appendResourceMutation(kTexture, RenderTapeMutationKind::Upload,
                                      0u, 0u, 4u, seedDigest);
  liveMutation.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 1u, .handleCount = 1u},
      textureState);
  liveMutation.appendResourceMutation(
      kTexture, RenderTapeMutationKind::CpuUnlock, 0u, 0u, 4u, seedDigest);
  liveMutation.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 1u, .handleCount = 0u},
      makePresentChunk());
  liveMutation.appendPresentComplete(
      12u, 8u, RenderTapeDigestValidity::NotCaptured, {},
      std::as_bytes(std::span(&oracle, 1u)));
  const auto liveMutationSource = liveMutation.seal();
  check(validateRenderTape(liveMutationSource, catalogue).valid(),
        "ordinary post-seed mutation fixture must validate");
  const std::array liveCommands{9u, 11u};
  const auto reducedLiveMutation =
      reduceRenderTape(liveMutationSource, catalogue, liveCommands);
  ImportedRenderTapeView liveSourceView;
  ImportedRenderTapeView reducedLiveView;
  check(validateRenderTape(liveMutationSource, catalogue, &liveSourceView).valid() &&
            validateRenderTape(reducedLiveMutation.bytes, catalogue,
                               &reducedLiveView)
                .valid(),
        "post-seed reduced tape must validate with its rewritten event layout");
  check(reducedLiveMutation.valid() &&
            reducedLiveMutation.retainedSourceEventIndices ==
                std::vector<std::uint32_t>({0u, 1u, 2u, 8u, 9u, 10u, 11u,
                                            12u}),
        "post-seed mutation must remain in the selected identity closure while unrelated generations are omitted");
  const std::array liveEventTypes{
      RenderTapeEventType::BootstrapState, RenderTapeEventType::ObjectDefine,
      RenderTapeEventType::ObjectDefine, RenderTapeEventType::ResourceMutation,
      RenderTapeEventType::CommandChunk, RenderTapeEventType::ResourceMutation,
      RenderTapeEventType::CommandChunk, RenderTapeEventType::PresentComplete};
  check(reducedLiveView.events.size() == liveEventTypes.size() &&
            std::equal(liveEventTypes.begin(), liveEventTypes.end(),
                       reducedLiveView.events.begin(),
                       [](RenderTapeEventType type,
                          const RenderTapeEventHeader& header) {
                         return static_cast<std::uint32_t>(type) == header.type;
                       }),
        "reduced mutation events must retain canonical source order");
  check(std::equal(liveSourceView.event(8u).payload.begin(),
                   liveSourceView.event(8u).payload.end(),
                   reducedLiveView.event(3u).payload.begin(),
                   reducedLiveView.event(3u).payload.end()) &&
            std::equal(liveSourceView.event(10u).payload.begin(),
                       liveSourceView.event(10u).payload.end(),
                       reducedLiveView.event(5u).payload.begin(),
                       reducedLiveView.event(5u).payload.end()),
        "retained mutation payload bytes must be unchanged");
  RenderTapeResourceMutationHeader liveSeed{};
  RenderTapeResourceMutationHeader liveUpdate{};
  check(reducedLiveView.event(3u).payload.size() == sizeof(liveSeed) &&
            reducedLiveView.event(5u).payload.size() == sizeof(liveUpdate),
        "retained mutation payloads must have their canonical fixed size");
  std::memcpy(&liveSeed, reducedLiveView.event(3u).payload.data(),
              sizeof(liveSeed));
  std::memcpy(&liveUpdate, reducedLiveView.event(5u).payload.data(),
              sizeof(liveUpdate));
  check(
            sameWireIdentity(liveSeed.identity, kTexture) &&
            sameWireIdentity(liveUpdate.identity, kTexture) &&
            liveSeed.digest == seedDigest && liveUpdate.digest == seedDigest &&
            reducedLiveMutation.referencedBlobDigests ==
                std::vector<RenderTapeDigest>({seedDigest}),
        "retained mutations must preserve generation-qualified identities and digests");

  RenderTapeBuilder preCommandMutation;
  preCommandMutation.appendBootstrapState(makeApplyStateChunk());
  preCommandMutation.appendObjectDefine(
      kSurface, kSurfaceDescriptorKind, surfaceDescriptorBytes, 0u, {});
  preCommandMutation.appendObjectDefine(
      kTexture, kTextureDescriptorKind, textureDescriptor, 0u, {}, 4u, 1u);
  preCommandMutation.appendResourceMutation(
      kTexture, RenderTapeMutationKind::Upload, 0u, 0u, 4u, seedDigest);
  preCommandMutation.appendResourceMutation(
      kTexture, RenderTapeMutationKind::CpuUnlock, 0u, 0u, 4u, seedDigest);
  preCommandMutation.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 1u, .handleCount = 1u},
      textureState);
  preCommandMutation.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 1u, .handleCount = 0u},
      makePresentChunk());
  preCommandMutation.appendPresentComplete(
      7u, 8u, RenderTapeDigestValidity::NotCaptured, {},
      std::as_bytes(std::span(&oracle, 1u)));
  const auto preCommandSource = preCommandMutation.seal();
  check(validateRenderTape(preCommandSource, catalogue).valid(),
        "ordinary pre-command mutation fixture must validate");
  const std::array preCommandCommands{5u, 6u};
  const auto reducedPreCommand =
      reduceRenderTape(preCommandSource, catalogue, preCommandCommands);
  check(reducedPreCommand.valid() &&
            reducedPreCommand.retainedSourceEventIndices ==
                std::vector<std::uint32_t>({0u, 1u, 2u, 3u, 4u, 5u, 6u,
                                            7u}),
        "duplicate pre-command identity mutations must preserve source order");
  ImportedRenderTapeView preSourceView;
  ImportedRenderTapeView reducedPreView;
  check(validateRenderTape(preCommandSource, catalogue, &preSourceView).valid() &&
            validateRenderTape(reducedPreCommand.bytes, catalogue,
                               &reducedPreView)
                .valid(),
        "pre-command reduced tape must validate with its rewritten event layout");
  check(std::equal(preSourceView.event(3u).payload.begin(),
                   preSourceView.event(3u).payload.end(),
                   reducedPreView.event(3u).payload.begin(),
                   reducedPreView.event(3u).payload.end()) &&
            std::equal(preSourceView.event(4u).payload.begin(),
                       preSourceView.event(4u).payload.end(),
                       reducedPreView.event(4u).payload.begin(),
                       reducedPreView.event(4u).payload.end()),
        "pre-command mutation payload bytes must remain source-identical");
  RenderTapeResourceMutationHeader preSeed{};
  RenderTapeResourceMutationHeader preUpdate{};
  check(reducedPreView.event(3u).payload.size() == sizeof(preSeed) &&
            reducedPreView.event(4u).payload.size() == sizeof(preUpdate),
        "pre-command mutation payloads must have their canonical fixed size");
  std::memcpy(&preSeed, reducedPreView.event(3u).payload.data(),
              sizeof(preSeed));
  std::memcpy(&preUpdate, reducedPreView.event(4u).payload.data(),
              sizeof(preUpdate));
  check(sameWireIdentity(preSeed.identity, kTexture) &&
            sameWireIdentity(preUpdate.identity, kTexture) &&
            preSeed.digest == seedDigest && preUpdate.digest == seedDigest &&
            reducedPreCommand.referencedBlobDigests ==
                std::vector<RenderTapeDigest>({seedDigest}),
        "pre-command mutations must preserve identity and digest order");

  // Ordered controls and a retirement in the selected identity closure are
  // reducible in their original journal order.  The destroyed texture is not
  // used by the Present command, so this exercises the exact safe case rather
  // than inventing a post-retirement handle use.
  RenderTapeBuilder ordered;
  constexpr D9CWireObjectIdentity unrelatedQuery{
      .kind = D9C_CHUNK_HANDLE_KIND_QUERY,
      .generation = 1u,
      .objectId = 0x9031u,
  };
  constexpr D9CWireObjectIdentity unrelatedBuffer{
      .kind = D9C_CHUNK_HANDLE_KIND_BUFFER,
      .generation = 1u,
      .objectId = 0x9032u,
  };
  constexpr std::array<std::byte, 1u> unrelatedQueryDescriptor{};
  constexpr std::array<std::byte, 8u> unrelatedBufferDescriptor{};
  ordered.appendBootstrapState(makeApplyStateChunk());
  ordered.appendObjectDefine(kSurface, kSurfaceDescriptorKind,
                              surfaceDescriptorBytes, 0u, {});
  ordered.appendObjectDefine(kTexture, kTextureDescriptorKind,
                              textureDescriptor, 0u, {}, 4u, 1u);
  ordered.appendObjectDefine(
      unrelatedQuery, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Query),
      unrelatedQueryDescriptor, 0u, {});
  ordered.appendObjectDefine(
      unrelatedBuffer, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Buffer),
      unrelatedBufferDescriptor, 0u, {});
  ordered.appendResourceMutation(kTexture, RenderTapeMutationKind::Upload, 0u,
                                 0u, 4u, seedDigest);
  const RenderTapeQueryGetDataControl queryPayload{
      .dataSize = 8u,
      .seqId = 21u,
  };
  const RenderTapeOrderedControlHeader queryControl{
      .identity = unrelatedQuery,
      .kind = static_cast<std::uint32_t>(RenderTapeControlKind::QueryGetData),
      .disposition = static_cast<std::uint32_t>(
          RenderTapeControlDisposition::Completed),
      .controlBytes = sizeof(queryPayload),
      .completionOrdinal = 2u,
  };
  ordered.appendOrderedControl(queryControl,
                               std::as_bytes(std::span(&queryPayload, 1u)));
  const RenderTapeCpuReadControl cpuReadPayload{
      .copyCount = 1u,
      .bytesRead = 16u,
  };
  const RenderTapeOrderedControlHeader cpuReadControl{
      .identity = unrelatedBuffer,
      .kind = static_cast<std::uint32_t>(RenderTapeControlKind::CpuRead),
      .disposition = static_cast<std::uint32_t>(
          RenderTapeControlDisposition::Completed),
      .controlBytes = sizeof(cpuReadPayload),
      .completionOrdinal = 3u,
  };
  ordered.appendOrderedControl(cpuReadControl,
                               std::as_bytes(std::span(&cpuReadPayload, 1u)));
  ordered.appendObjectDestroy(unrelatedBuffer);
  ordered.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 1u, .handleCount = 1u},
      textureState);
  std::array<std::byte, kRenderTapeGammaRampBytes> gamma{};
  const RenderTapeOrderedControlHeader gammaControl{
      .kind = static_cast<std::uint32_t>(RenderTapeControlKind::GammaRampSet),
      .disposition = static_cast<std::uint32_t>(
          RenderTapeControlDisposition::Completed),
      .controlBytes = kRenderTapeGammaRampBytes,
      .completionOrdinal = 4u,
  };
  ordered.appendOrderedControl(gammaControl, gamma);
  ordered.appendObjectDestroy(kTexture);
  ordered.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 1u, .handleCount = 0u},
      makePresentChunk());
  ordered.appendPresentComplete(
      13u, 9u, RenderTapeDigestValidity::NotCaptured, {},
      std::as_bytes(std::span(&oracle, 1u)));
  const auto orderedSource = ordered.seal();
  check(validateRenderTape(orderedSource, catalogue).valid(),
        "ordered control/destroy source fixture must validate");
  const std::array orderedSelection{9u, 12u};
  const auto reducedOrdered =
      reduceRenderTape(orderedSource, catalogue, orderedSelection);
  check(reducedOrdered.valid(),
        "reducer must retain selected control/destroy ordering");
  check(reducedOrdered.retainedSourceEventIndices ==
            std::vector<std::uint32_t>({0u, 1u, 2u, 5u, 9u, 10u, 11u, 12u,
                                        13u}),
        "selected control and destroy events must remain source ordered");
  ImportedRenderTapeView orderedSourceView;
  ImportedRenderTapeView reducedOrderedView;
  check(validateRenderTape(orderedSource, catalogue, &orderedSourceView).valid() &&
            validateRenderTape(reducedOrdered.bytes, catalogue,
                               &reducedOrderedView)
                .valid(),
        "ordered reduction must pass final structural validation");
  const std::array orderedEventTypes{
      RenderTapeEventType::BootstrapState, RenderTapeEventType::ObjectDefine,
      RenderTapeEventType::ObjectDefine, RenderTapeEventType::ResourceMutation,
      RenderTapeEventType::CommandChunk, RenderTapeEventType::OrderedControl,
      RenderTapeEventType::ObjectDestroy, RenderTapeEventType::CommandChunk,
      RenderTapeEventType::PresentComplete};
  check(reducedOrderedView.events.size() == orderedEventTypes.size() &&
            std::equal(orderedEventTypes.begin(), orderedEventTypes.end(),
                       reducedOrderedView.events.begin(),
                       [](RenderTapeEventType type,
                          const RenderTapeEventHeader& header) {
                         return static_cast<std::uint32_t>(type) == header.type;
                       }),
        "reduced controls and destruction must retain source event types/order");
  check(std::equal(orderedSourceView.event(5u).payload.begin(),
                   orderedSourceView.event(5u).payload.end(),
                   reducedOrderedView.event(3u).payload.begin(),
                   reducedOrderedView.event(3u).payload.end()) &&
            std::equal(orderedSourceView.event(10u).payload.begin(),
                       orderedSourceView.event(10u).payload.end(),
                       reducedOrderedView.event(5u).payload.begin(),
                       reducedOrderedView.event(5u).payload.end()) &&
            std::equal(orderedSourceView.event(11u).payload.begin(),
                       orderedSourceView.event(11u).payload.end(),
                       reducedOrderedView.event(6u).payload.begin(),
                       reducedOrderedView.event(6u).payload.end()) &&
            std::equal(orderedSourceView.event(12u).payload.begin(),
                       orderedSourceView.event(12u).payload.end(),
                       reducedOrderedView.event(7u).payload.begin(),
                       reducedOrderedView.event(7u).payload.end()),
        "retained control, destruction, mutation, and Present payload bytes must be unchanged");
  RenderTapeOrderedControlHeader reducedGamma{};
  RenderTapeObjectDestroyHeader reducedDestroy{};
  check(reducedOrderedView.event(5u).payload.size() >= sizeof(reducedGamma) &&
            reducedOrderedView.event(6u).payload.size() == sizeof(reducedDestroy),
        "retained control and destruction payloads must have canonical sizes");
  std::memcpy(&reducedGamma, reducedOrderedView.event(5u).payload.data(),
              sizeof(reducedGamma));
  std::memcpy(&reducedDestroy, reducedOrderedView.event(6u).payload.data(),
              sizeof(reducedDestroy));
  check(reducedGamma.kind ==
                static_cast<std::uint32_t>(RenderTapeControlKind::GammaRampSet) &&
            reducedGamma.controlBytes == kRenderTapeGammaRampBytes &&
            sameWireIdentity(reducedDestroy.identity, kTexture),
        "retained GammaRampSet and ObjectDestroy must preserve typed payloads");

  RenderTapeBuilder afterTerminal;
  for (std::size_t index = 0u; index < orderedSourceView.events.size(); ++index) {
    const auto event = orderedSourceView.event(index);
    afterTerminal.appendRawEvent(
        static_cast<RenderTapeEventType>(event.header.type), event.payload);
  }
  afterTerminal.appendObjectDestroy(unrelatedBuffer);
  bool rejectedAfterTerminal = false;
  try {
    static_cast<void>(afterTerminal.seal());
  } catch (const std::invalid_argument&) {
    rejectedAfterTerminal = true;
  }
  check(rejectedAfterTerminal,
        "events after terminal PresentComplete must fail before reduction effects");

  auto invalidSource = source;
  reinterpret_cast<RenderTapeHeader*>(invalidSource.data())->version += 1u;
  const auto rejected = reduceRenderTape(invalidSource, catalogue, presentOnly);
  check(rejected.status == RenderTapeReductionStatus::InvalidSource &&
            rejected.bytes.empty() &&
            rejected.retainedSourceEventIndices.empty() &&
            rejected.referencedBlobDigests.empty(),
        "invalid source must fail before producing any reduced output");
}

void opaqueOracleAlphaIsCanonical() {
  std::array pixels{
      std::byte{1u}, std::byte{2u}, std::byte{3u}, std::byte{4u},
      std::byte{5u}, std::byte{6u}, std::byte{7u}, std::byte{8u},
  };
  const auto original = pixels;
  check(renderTapeCanonicalizeOpaqueAlpha(21u, pixels) && pixels == original,
        "A8 oracle pixels must preserve authored alpha");
  check(renderTapeCanonicalizeOpaqueAlpha(22u, pixels) &&
            pixels[3] == std::byte{0xffu} &&
            pixels[7] == std::byte{0xffu},
        "X8 oracle pixels must canonicalize ignored alpha to opaque");
  std::array malformed{std::byte{0u}, std::byte{0u}, std::byte{0u}};
  check(!renderTapeCanonicalizeOpaqueAlpha(22u, malformed),
        "malformed X8 oracle storage must fail closed");
}

void queryTranscriptV2PreservesCallsAndRejectsStaleCycles() {
  constexpr D9CWireObjectIdentity queryIdentity{
      .kind = D9C_CHUNK_HANDLE_KIND_QUERY, .generation = 7u, .objectId = 901u};
  const auto makeTape = [&](auto descriptor,
                            bool includeBegin, bool includeEnd,
                            RenderTapeQueryGetDataControlV2 observed,
                            std::int32_t hr = 0, bool reissue = false,
                            bool skipPoll = false) {
    RenderTapeBuilder builder;
    builder.appendBootstrapState(makeApplyStateChunk());
    const auto output = outputSurfaceDescriptor();
    builder.appendObjectDefine(kSurface, kSurfaceDescriptorKind,
        std::as_bytes(std::span(&output, 1u)), 0u, {});
    builder.appendResourceMutation(kSurface, RenderTapeMutationKind::CpuUnlock,
                                  0u, 0u, 4u, mutationDigest());
    builder.appendObjectDefine(queryIdentity,
        static_cast<std::uint32_t>(RenderTapeDescriptorKind::Query),
        std::as_bytes(std::span(&descriptor, 1u)), 0u, {});
    const auto issue = [&](std::uint32_t flags) {
      const D9CCommandChunkWireQueryIssue payload{
          .queryHandleIndex = 0u, .flags = flags};
      const D9CCommandChunkWireHandleEntry handle{
          .kind = queryIdentity.kind, .generation = queryIdentity.generation,
          .objectId = queryIdentity.objectId};
      const auto chunk = makeSingleRecordChunk(D9C_COMMAND_RECORD_QUERY_ISSUE,
          std::as_bytes(std::span(&payload, 1u)), std::span(&handle, 1u));
      builder.appendCommandChunk(
          CommandChunkEnvelope{.recordCount = 1u, .handleCount = 1u}, chunk);
    };
    if (includeBegin) issue(2u);
    if (includeEnd) issue(1u);
    if (reissue) {
      issue(2u);
      issue(1u);
    }
    const RenderTapeOrderedControlHeader control{
        .identity = queryIdentity,
        .kind = static_cast<std::uint32_t>(RenderTapeControlKind::QueryGetData),
        .disposition = static_cast<std::uint32_t>(hr == 1
            ? RenderTapeControlDisposition::Pending
            : hr == 0 ? RenderTapeControlDisposition::Completed
                      : RenderTapeControlDisposition::Failed),
        .resultCode = hr, .controlBytes = sizeof(observed),
        .completionOrdinal = 1u};
    if (!skipPoll)
      builder.appendOrderedControl(control, std::as_bytes(std::span(&observed, 1u)));
    builder.appendCommandChunk(
        CommandChunkEnvelope{.recordCount = 1u, .handleCount = 1u},
        makeMappedPresentChunk(kSurface));
    const RenderTapeOracleAttachment oracle{
        .identity = kSurface, .descriptorKind = kSurfaceDescriptorKind};
    builder.appendPresentComplete(6u + (includeBegin ? 1u : 0u) +
        (includeEnd ? 1u : 0u) + (reissue ? 2u : 0u) - (skipPoll ? 1u : 0u),
        2u, RenderTapeDigestValidity::NotCaptured, {},
        std::as_bytes(std::span(&oracle, 1u)));
    return builder.seal();
  };

  const RenderTapeQueryDescriptorV2 occlusion{.type = 9u, .dataBytes = 4u};
  RenderTapeQueryGetDataControlV2 ready{
      .flags = 1u, .hasOutput = 1u, .definedBytes = 4u,
      .dataSize = 4u, .issueCycle = 1u};
  const std::uint32_t samples = 1024u;
  std::memcpy(ready.result.data(), &samples, sizeof(samples));
  const auto catalogue = completeCatalogue();
  const auto tape = makeTape(occlusion, true, true, ready);
  ImportedRenderTapeView imported;
  check(validateRenderTape(tape, catalogue, &imported).valid(),
        "V2 BEGIN END and exact successful result validate");
  const auto controlEvent = imported.event(6u);
  RenderTapeQueryGetDataControlV2 decoded{};
  std::memcpy(&decoded, controlEvent.payload.data() +
              sizeof(RenderTapeOrderedControlHeader), sizeof(decoded));
  check(decoded.flags == 1u && decoded.hasOutput == 1u &&
        decoded.dataSize == 4u && decoded.issueCycle == 1u &&
        decoded.definedBytes == 4u && decoded.result == ready.result,
        "call flags output shape cycle and exact bytes survive serialization");
  ready.result[0] ^= std::byte{0xffu};
  check(decoded.result[0] != ready.result[0],
        "journal result bytes own a copy independent of caller storage");
  ready.result[0] ^= std::byte{0xffu};
  auto newest = ready;
  newest.issueCycle = 2u;
  check(validateRenderTape(makeTape(occlusion, true, true, newest, 0, true),
                           catalogue).valid(),
        "serialized reissue BEGIN and END replace a completed captured cycle");
  check(validateRenderTape(makeTape(occlusion, true, true, ready, 0, true),
                           catalogue).status == RenderTapeValidationStatus::QueryUncapturedCycle,
        "old result cannot redirect a poll after serialized reissue");

  const auto rejects = [&](RenderTapeQueryGetDataControlV2 control,
                           RenderTapeValidationStatus status,
                           std::string_view message) {
    const auto result = validateRenderTape(
        makeTape(occlusion, true, true, control), catalogue);
    check(result.status == status && result.hasOffendingIdentity &&
          sameWireIdentity(result.offendingIdentity, queryIdentity), message);
  };
  auto bad = ready;
  bad.issueCycle = 2u;
  rejects(bad, RenderTapeValidationStatus::QueryUncapturedCycle,
          "stale or future issue cycle rejects with exact lifetime identity");
  bad = ready;
  bad.flags = 2u;
  rejects(bad, RenderTapeValidationStatus::InvalidQueryTranscript,
          "unknown GetData flags reject");
  bad = ready;
  bad.hasOutput = 2u;
  rejects(bad, RenderTapeValidationStatus::InvalidQueryTranscript,
          "nonboolean output presence rejects");
  bad = ready;
  bad.definedBytes = 9u;
  rejects(bad, RenderTapeValidationStatus::InvalidQueryTranscript,
          "result extent over eight rejects before indexing result storage");
  bad = ready;
  bad.result[7] = std::byte{1u};
  rejects(bad, RenderTapeValidationStatus::InvalidQueryTranscript,
          "undefined padding cannot carry caller memory");
  bad = ready;
  bad.hasOutput = 0u;
  rejects(bad, RenderTapeValidationStatus::InvalidQueryTranscript,
          "null output cannot claim defined bytes");

  RenderTapeQueryGetDataControlV2 pending{
      .flags = 1u, .hasOutput = 1u, .dataSize = 4u, .issueCycle = 1u};
  check(validateRenderTape(makeTape(occlusion, true, false, pending, 1), catalogue)
            .valid(), "active Pending preserves FLUSH but defines no bytes");
  pending.result[0] = std::byte{0xa5u};
  check(validateRenderTape(makeTape(occlusion, true, false, pending, 1), catalogue)
            .status == RenderTapeValidationStatus::InvalidQueryTranscript,
        "Pending result caller bytes are forbidden");
  pending.result = {};
  check(validateRenderTape(makeTape(occlusion, true, true, pending,
                                   static_cast<std::int32_t>(0x88760868u)),
                           catalogue).valid(),
        "failed calls preserve their HRESULT without defining caller bytes");
  check(validateRenderTape(makeTape(occlusion, true, false, ready), catalogue)
            .status == RenderTapeValidationStatus::QueryUncapturedCycle,
        "successful readiness cannot precede END");
  check(validateRenderTape(makeTape(occlusion, false, true, ready), catalogue)
            .status == RenderTapeValidationStatus::QueryUncapturedCycle,
        "occlusion END cannot invent missing prefix work");

  const RenderTapeQueryDescriptorV2 old{
      .type = 9u, .dataBytes = 4u,
      .initialState = static_cast<std::uint32_t>(
          RenderTapeQueryInitialState::ClosedWithoutReplayResult),
      .initialIssueCycle = 41u};
  bad = ready;
  bad.issueCycle = 41u;
  check(validateRenderTape(makeTape(old, false, false, bad), catalogue).status ==
            RenderTapeValidationStatus::QueryUncapturedCycle,
        "closed bootstrap cycle conveys provenance without result authority");
  check(validateRenderTape(makeTape(old, false, false, bad, 0, false, true),
                           catalogue).valid(),
        "an unreferenced old closed query does not invalidate the tape");
  bad.issueCycle = 42u;
  check(validateRenderTape(makeTape(old, true, true, bad), catalogue).valid(),
        "captured replacement BEGIN closes over its new cycle");
  const RenderTapeQueryDescriptorV2 event{.type = 8u, .dataBytes = 4u};
  RenderTapeQueryGetDataControlV2 eventReady{
      .hasOutput = 1u, .dataSize = 4u, .issueCycle = 1u};
  check(validateRenderTape(makeTape(event, false, true, eventReady), catalogue).valid(),
        "Event END-only cycle preserves public size without undefined writes");
  eventReady.definedBytes = 4u;
  check(validateRenderTape(makeTape(event, false, true, eventReady), catalogue).status ==
            RenderTapeValidationStatus::InvalidQueryTranscript,
        "Event cannot claim a result write absent from production contract");

  RenderTapeQueryDescriptorV2 decodedDescriptor{};
  auto unsupported = occlusion;
  unsupported.type = 99u;
  check(!renderTapeLoadQueryDescriptorV2(std::as_bytes(std::span(&unsupported, 1u)),
                                        decodedDescriptor),
        "unsupported type cannot be truncated into an eight-byte transcript");
  RenderTapeQueryCycleState cycle{.type = 9u, .issueCycle = 41u, .active = true};
  const auto initial = cycle;
  check(!renderTapeAdvanceQueryCycle(cycle, 1u, true) &&
        cycle.issueCycle == initial.issueCycle && cycle.active,
        "captured END of prearm active scope rejects without changing state");
  check(renderTapeAdvanceQueryCycle(cycle, 2u, true) &&
        cycle.issueCycle == 42u && cycle.capturedCycle,
        "replacement BEGIN establishes captured scope authority");
  cycle.issueCycle = std::numeric_limits<std::uint64_t>::max();
  check(!renderTapeAdvanceQueryCycle(cycle, 2u, true),
        "issue-cycle overflow fails closed");

  const D9CRenderTapeEventQueryCheckpointRequest request{
      .identity = queryIdentity, .issueCycle = 589u, .issueOrdinal = 1000u,
      .checkpointVersion = D9C_RENDER_TAPE_EVENT_QUERY_CHECKPOINT_VERSION_1,
      .queryType = 8u,
  };
  const D9CRenderTapeEventQueryCheckpointResult proof{
      .checkpointVersion = D9C_RENDER_TAPE_EVENT_QUERY_CHECKPOINT_VERSION_1,
      .status = D9C_RENDER_TAPE_EVENT_QUERY_CHECKPOINT_COMPLETE,
      .queryType = 8u, .identity = queryIdentity, .issueCycle = 589u,
      .coreQueryIdentity = 71u, .deviceEpoch = 4u, .issueOrdinal = 1000u,
      .rawOrdinal = 901u, .queueIdentity = 27u, .endSequence = 50u,
      .initializerSequence = 3u, .completedSequence = 50u,
      .completedInitializer = 3u,
  };
  RenderTapeQueryDescriptorV3 checkpoint{
      .initialIssueCycle = 589u, .checkpoint = proof};
  RenderTapeQueryDescriptorDecoded normalized{};
  const auto checkpointBytes = std::as_bytes(std::span(&checkpoint, 1u));
  check(renderTapeEventQueryCheckpointResultMatches(request, proof) &&
            renderTapeLoadQueryDescriptor(checkpointBytes, normalized) &&
            renderTapeQueryDescriptorIdentityMatches(normalized, queryIdentity) &&
            !renderTapeLoadQueryDescriptorV2(checkpointBytes, decodedDescriptor),
        "V3 completed EVENT checkpoint decodes explicitly without widening V2");
  auto eventPoll = eventReady;
  eventPoll.issueCycle = 589u;
  eventPoll.definedBytes = 0u;
  check(validateRenderTape(makeTape(checkpoint, false, false, eventPoll), catalogue)
            .valid(),
        "V3 checkpoint authorizes exact old-cycle EVENT poll without fabricated Issue");
  auto checkpointCycle = renderTapeInitialQueryCycle(normalized);
  check(!checkpointCycle.capturedCycle && checkpointCycle.completedEventCheckpoint &&
            checkpointCycle.issueOrdinal == 1000u &&
            renderTapeQueryPollCycleAdmitted(checkpointCycle, 589u, 0) &&
            renderTapeEventQueryCheckpointMatchesCycle(
                normalized, queryIdentity, checkpointCycle),
        "checkpoint provenance is separate from captured Issue history");
  auto staleCheckpointCycle = checkpointCycle;
  ++staleCheckpointCycle.issueOrdinal;
  check(!renderTapeEventQueryCheckpointMatchesCycle(
            normalized, queryIdentity, staleCheckpointCycle),
        "arm checkpoint cannot authorize another public Issue ordinal");
  staleCheckpointCycle = checkpointCycle;
  staleCheckpointCycle.active = true;
  check(!renderTapeEventQueryCheckpointMatchesCycle(
            normalized, queryIdentity, staleCheckpointCycle),
        "completed EVENT checkpoint cannot authorize an open BEGIN");
  check(renderTapeAdvanceQueryCycle(checkpointCycle, 1u, true) &&
            checkpointCycle.issueCycle == 590u && checkpointCycle.issueOrdinal == 1001u &&
            checkpointCycle.capturedCycle && !checkpointCycle.completedEventCheckpoint &&
            !renderTapeQueryPollCycleAdmitted(checkpointCycle, 589u, 0),
        "END-only reissue replaces checkpoint authority transactionally");
  eventPoll.issueCycle = 590u;
  check(validateRenderTape(makeTape(checkpoint, false, true, eventPoll), catalogue)
            .valid(), "captured replacement EVENT END preserves absolute cycle");
  eventPoll.issueCycle = 589u;
  check(validateRenderTape(makeTape(checkpoint, false, true, eventPoll), catalogue)
            .status == RenderTapeValidationStatus::QueryUncapturedCycle,
        "old checkpoint poll is stale after captured replacement END");
  auto forgedV2 = event;
  forgedV2.initialState = static_cast<std::uint32_t>(
      RenderTapeQueryInitialState::CompletedEventCheckpoint);
  forgedV2.initialIssueCycle = 589u;
  check(!renderTapeLoadQueryDescriptorV2(
            std::as_bytes(std::span(&forgedV2, 1u)), decodedDescriptor) &&
            !renderTapeLoadQueryDescriptor(
                std::as_bytes(std::span(&forgedV2, 1u)), normalized),
        "V2 cannot reuse its initial state field for a checkpoint");
  for (unsigned mutation = 0u; mutation < 15u; ++mutation) {
    auto invalid = checkpoint;
    if (mutation == 0u) invalid.schemaVersion = 2u;
    if (mutation == 1u) invalid.type = 9u;
    if (mutation == 2u) invalid.dataBytes = 8u;
    if (mutation == 3u) invalid.initialState = 1u;
    if (mutation == 4u) --invalid.initialIssueCycle;
    if (mutation == 5u) invalid.checkpoint.active = 1u;
    if (mutation == 6u) invalid.checkpoint.status = D9C_RENDER_TAPE_EVENT_QUERY_CHECKPOINT_PENDING;
    if (mutation == 7u) invalid.checkpoint.resultCode = 1;
    if (mutation == 8u) invalid.checkpoint.deviceEpoch = 0u;
    if (mutation == 9u) invalid.checkpoint.coreQueryIdentity = 0u;
    if (mutation == 10u) invalid.checkpoint.rawOrdinal = 0u;
    if (mutation == 11u) invalid.checkpoint.queueIdentity = 0u;
    if (mutation == 12u) --invalid.checkpoint.completedSequence;
    if (mutation == 13u) --invalid.checkpoint.completedInitializer;
    if (mutation == 14u) invalid.checkpoint.reserved0 = 1u;
    check(!renderTapeLoadQueryDescriptor(
              std::as_bytes(std::span(&invalid, 1u)), normalized) &&
              normalized.schemaVersion == 0u &&
              validateRenderTape(makeTape(invalid, false, false, eventPoll), catalogue)
                  .status == RenderTapeValidationStatus::InvalidQueryTranscript,
          "malformed, active, Pending, early, or unauthenticated EVENT checkpoints reject");
  }
  auto foreign = checkpoint;
  ++foreign.checkpoint.identity.generation;
  check(validateRenderTape(makeTape(foreign, false, false, eventPoll), catalogue)
            .status == RenderTapeValidationStatus::InvalidQueryTranscript,
        "checkpoint wire identity must match exact ObjectDefine generation");
  auto staleProof = proof;
  --staleProof.issueOrdinal;
  check(!renderTapeEventQueryCheckpointResultMatches(request, staleProof),
        "capture result for a prior public Issue ordinal cannot seed the arm");
  auto emptyPrefix = checkpoint;
  emptyPrefix.checkpoint.endSequence = 0u;
  emptyPrefix.checkpoint.initializerSequence = 0u;
  emptyPrefix.checkpoint.completedSequence = 0u;
  emptyPrefix.checkpoint.completedInitializer = 0u;
  check(validateRenderTape(makeTape(emptyPrefix, false, false, eventPoll), catalogue)
            .valid(), "authenticated completed empty-prefix EVENT permits END sequence zero");
  RenderTapeQueryCycleState beginEnd{.type = 8u};
  check(renderTapeAdvanceQueryCycle(beginEnd, 2u, false) &&
            beginEnd.issueCycle == 1u && beginEnd.issueOrdinal == 1u && beginEnd.active &&
            renderTapeAdvanceQueryCycle(beginEnd, 1u, false) &&
            beginEnd.issueCycle == 1u && beginEnd.issueOrdinal == 2u && !beginEnd.active,
        "EVENT BEGIN+END advances two public ordinals but one cycle");
  RenderTapeQueryCycleState endOnly{.type = 8u};
  check(renderTapeAdvanceQueryCycle(endOnly, 1u, false) &&
            endOnly.issueCycle == 1u && endOnly.issueOrdinal == 1u,
        "EVENT END-only advances one public ordinal and one cycle");
  endOnly.issueOrdinal = std::numeric_limits<std::uint64_t>::max();
  const auto beforeOverflow = endOnly;
  check(!renderTapeAdvanceQueryCycle(endOnly, 1u, true) &&
            endOnly.issueCycle == beforeOverflow.issueCycle &&
            endOnly.issueOrdinal == beforeOverflow.issueOrdinal &&
            endOnly.capturedCycle == beforeOverflow.capturedCycle,
        "public Issue ordinal overflow rejects without changing cycle authority");
}

} // namespace

int main(int argc, char** argv) {
  try {
    if (argc == 3 && std::string_view(argv[1]) == "--write-fixture") {
      const auto tape = makeCompleteTape();
      std::ofstream output(argv[2], std::ios::binary);
      output.write(reinterpret_cast<const char*>(tape.data()),
                   static_cast<std::streamsize>(tape.size()));
      check(output.good(), "failed to write render tape fixture");
      return 0;
    }
    check(argc == 1, "usage: render_tape_spec [--write-fixture path]");
    clearDrawPresentSourceMappingMatchesOracleExactly();
    validTapeReplaysExactlyOnce();
    immutableObjectsAndRetirementFailClosed();
    descriptorKindIdentityMismatchFailsClosed();
    bootstrapAndCommandHandlesCloseBeforeReplay();
    seedContentClosesByUniqueSubresourceAndSummedBytes();
    perIdentityLateSeedClosureIsExact();
    versionedSubresourceDescriptorsFailClosed();
    textureDispositionDiagnosticsPreserveCanonicalLevel0();
    producedDf24CanonicalObligationsAreTemporalAndPerParent();
    textureAliasLogicalSlotsUseParentSubresources();
    textureSurfaceAliasesCoverCanonical2DAndStandaloneSurfaces();
    textureSurfaceWrapperLifetimeIsIdempotent();
    gammaRampBootstrapAndOrderedMutationAreTyped();
    bufferMutationDispositionIsTypedAndBackwardCompatible();
    queryTranscriptV2PreservesCallsAndRejectsStaleCycles();
    bootstrapProfileShapesAndPagePartitionsAreExhaustive();
    invalidInputsFailBeforeCallbacks();
    boundedCaptureReplayRefinementIsExhaustive();
    wholeEventReductionIsCanonicalAndFailClosed();
    opaqueOracleAlphaIsCanonical();
  } catch (const std::exception& error) {
    std::cerr << "render tape spec failed: " << error.what() << '\n';
    return 1;
  }
  std::cout << "render tape spec passed\n";
  return 0;
}
