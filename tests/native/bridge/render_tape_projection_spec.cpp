#include "device_c_render_tape_projection.hpp"
#include "device_c_render_tape_policy.hpp"

// R-HARN-REPLAY-7.16/7.17: pure frame-tape draw-range projection, canonical
// locator conservation, exact object/blob closure, and pre-effect rejection.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
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
  if (!condition) throw TestFailure(std::string(message));
}

std::size_t alignUp(std::size_t value, std::size_t alignment) {
  return (value + alignment - 1u) & ~(alignment - 1u);
}

template <typename T>
std::vector<std::byte> bytesOf(const T& value) {
  std::vector<std::byte> bytes(sizeof(value));
  std::memcpy(bytes.data(), &value, sizeof(value));
  return bytes;
}

struct Record {
  std::uint32_t type = 0u;
  std::vector<std::byte> payload{};
  std::vector<D9CCommandChunkWireHandleEntry> handles{};
};

std::vector<std::byte> makeChunk(std::span<const Record> specs) {
  const auto recordTableOffset = sizeof(D9CCommandChunkWireHeader);
  std::size_t handleCount = 0u;
  for (const auto& spec : specs) handleCount += spec.handles.size();
  const auto handleTableOffset = alignUp(
      recordTableOffset + specs.size() * sizeof(D9CCommandChunkWireRecordHeader),
      alignof(D9CCommandChunkWireHandleEntry));
  const auto payloadArenaOffset = alignUp(
      handleTableOffset + handleCount * sizeof(D9CCommandChunkWireHandleEntry),
      alignof(std::uint32_t));

  std::vector<D9CCommandChunkWireRecordHeader> records;
  std::vector<D9CCommandChunkWireHandleEntry> handles;
  std::vector<std::byte> payload;
  for (const auto& spec : specs) {
    const auto* rule = recordRule(spec.type);
    check(rule != nullptr, "fixture record type must be canonical");
    payload.resize(alignUp(payload.size(), rule->payloadAlignment));
    records.push_back(D9CCommandChunkWireRecordHeader{
        .type = spec.type,
        .payloadOffset = static_cast<std::uint32_t>(payload.size()),
        .payloadSize = static_cast<std::uint32_t>(spec.payload.size()),
        .firstHandle = static_cast<std::uint32_t>(handles.size()),
        .handleCount = static_cast<std::uint32_t>(spec.handles.size()),
    });
    handles.insert(handles.end(), spec.handles.begin(), spec.handles.end());
    payload.insert(payload.end(), spec.payload.begin(), spec.payload.end());
  }
  const D9CCommandChunkWireHeader header{
      .version = D9C_COMMAND_CHUNK_WIRE_VERSION,
      .headerSize = D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE,
      .recordHeaderSize = D9C_COMMAND_CHUNK_WIRE_RECORD_HEADER_SIZE,
      .handleEntrySize = D9C_COMMAND_CHUNK_WIRE_HANDLE_ENTRY_SIZE,
      .recordTableOffset = static_cast<std::uint32_t>(recordTableOffset),
      .recordCount = static_cast<std::uint32_t>(records.size()),
      .handleTableOffset = static_cast<std::uint32_t>(handleTableOffset),
      .handleCount = static_cast<std::uint32_t>(handles.size()),
      .payloadArenaOffset = static_cast<std::uint32_t>(payloadArenaOffset),
      .payloadArenaSize = static_cast<std::uint32_t>(payload.size()),
  };
  std::vector<std::byte> bytes(payloadArenaOffset + payload.size());
  std::memcpy(bytes.data(), &header, sizeof(header));
  if (!records.empty()) {
    std::memcpy(bytes.data() + recordTableOffset, records.data(),
                records.size() * sizeof(records[0]));
  }
  if (!handles.empty()) {
    std::memcpy(bytes.data() + handleTableOffset, handles.data(),
                handles.size() * sizeof(handles[0]));
  }
  if (!payload.empty()) {
    std::memcpy(bytes.data() + payloadArenaOffset, payload.data(),
                payload.size());
  }
  return bytes;
}

constexpr D9CWireObjectIdentity kOutput{
    .kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
    .generation = 1u,
    .objectId = 11u,
};
constexpr D9CWireObjectIdentity kTexture{
    .kind = D9C_CHUNK_HANDLE_KIND_TEXTURE,
    .generation = 3u,
    .objectId = 29u,
};
constexpr D9CWireObjectIdentity kAlias{
    .kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
    .generation = 5u,
    .objectId = 31u,
};
constexpr D9CWireObjectIdentity kShader{
    .kind = D9C_CHUNK_HANDLE_KIND_SHADER,
    .generation = 7u,
    .objectId = 43u,
};

RenderTapeDigest digest(std::string_view text) {
  check(text.size() == kRenderTapeDigestSize * 2u,
        "fixture digest has canonical size");
  RenderTapeDigest value{};
  for (std::size_t i = 0u; i < value.size(); ++i) {
    const auto nibble = [](char digit) -> unsigned {
      if (digit >= '0' && digit <= '9') return digit - '0';
      return digit - 'a' + 10u;
    };
    value[i] = static_cast<std::byte>(
        (nibble(text[i * 2u]) << 4u) | nibble(text[i * 2u + 1u]));
  }
  return value;
}

const RenderTapeDigest kTextureDigest = digest(
    "8d70d691c822d55638b6e7fd54cd94170c87d19eb1f628b757506ede5688d297");
const RenderTapeDigest kShaderDigest = digest(
    "9f64a747e1b97f131fabb6b447296c9b6f0201e79fb3c5356e6c77e89b6a806a");

std::vector<std::byte> drawPayload(bool fullSnapshot,
                                   std::uint32_t firstHandle = 0u,
                                   bool includeAlias = false) {
  std::array<D9CCommandChunkWireTextureBinding,
             D9C_DRAW_PACKET_MAX_TEXTURES>
      textures{};
  for (std::uint32_t slot = 0u; slot < textures.size(); ++slot) {
    textures[slot] = D9CCommandChunkWireTextureBinding{
        .slot = slot,
        .valid = 1u,
        .handleIndex = slot == 0u ? firstHandle
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
  const D9CCommandChunkWireShaderBinding shader{
      .stage = D9C_COMMAND_CHUNK_SHADER_STAGE_VERTEX,
      .valid = 1u,
      .handleIndex = firstHandle + 1u,
  };
  const D9CCommandChunkWireRenderTargetBinding renderTarget{
      .slot = 0u,
      .valid = includeAlias ? 1u : 0u,
      .handleIndex = includeAlias
          ? firstHandle + 2u
          : D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX,
  };

  constexpr std::uint32_t sectionCount = 4u;
  const auto sectionTableOffset = sizeof(D9CCommandChunkWireDrawHeader);
  const auto sectionPayloadOffset = alignUp(
      sectionTableOffset + sectionCount * sizeof(D9CCommandChunkWireSectionDesc),
      alignof(std::uint32_t));
  const auto streamOffset = sectionPayloadOffset + sizeof(textures);
  const auto shaderOffset = streamOffset + sizeof(streams);
  const auto renderTargetOffset = shaderOffset + sizeof(shader);
  const D9CCommandChunkWireDrawHeader draw{
      .flags = fullSnapshot ? D9C_COMMAND_CHUNK_DRAW_FLAG_FULL_SNAPSHOT : 0u,
      .primitiveType = 4u,
      .primitiveCount = 1u,
      .sectionCount = sectionCount,
      .sectionTableOffset = static_cast<std::uint32_t>(sectionTableOffset),
      .sectionPayloadOffset = static_cast<std::uint32_t>(sectionPayloadOffset),
  };
  const std::array sections{
      D9CCommandChunkWireSectionDesc{
          .kind = D9C_COMMAND_CHUNK_SECTION_TEXTURE,
          .elementSize = sizeof(textures[0]),
          .count = static_cast<std::uint32_t>(textures.size()),
          .payloadOffset = static_cast<std::uint32_t>(sectionPayloadOffset),
          .byteSize = sizeof(textures),
      },
      D9CCommandChunkWireSectionDesc{
          .kind = D9C_COMMAND_CHUNK_SECTION_STREAM,
          .elementSize = sizeof(streams[0]),
          .count = static_cast<std::uint32_t>(streams.size()),
          .payloadOffset = static_cast<std::uint32_t>(streamOffset),
          .byteSize = sizeof(streams),
      },
      D9CCommandChunkWireSectionDesc{
          .kind = D9C_COMMAND_CHUNK_SECTION_SHADER,
          .elementSize = sizeof(shader),
          .count = 1u,
          .payloadOffset = static_cast<std::uint32_t>(shaderOffset),
          .byteSize = sizeof(shader),
      },
      D9CCommandChunkWireSectionDesc{
          .kind = D9C_COMMAND_CHUNK_SECTION_RENDER_TARGET,
          .elementSize = sizeof(renderTarget),
          .count = 1u,
          .payloadOffset = static_cast<std::uint32_t>(renderTargetOffset),
          .byteSize = sizeof(renderTarget),
      },
  };
  std::vector<std::byte> payload(renderTargetOffset + sizeof(renderTarget));
  std::memcpy(payload.data(), &draw, sizeof(draw));
  std::memcpy(payload.data() + sectionTableOffset, sections.data(),
              sizeof(sections));
  std::memcpy(payload.data() + sectionPayloadOffset, textures.data(),
              sizeof(textures));
  std::memcpy(payload.data() + streamOffset, streams.data(), sizeof(streams));
  std::memcpy(payload.data() + shaderOffset, &shader, sizeof(shader));
  std::memcpy(payload.data() + renderTargetOffset, &renderTarget,
              sizeof(renderTarget));
  return payload;
}

std::vector<std::byte> applyStatePayload() {
  auto payload = drawPayload(true);
  auto* shaderSection = reinterpret_cast<D9CCommandChunkWireSectionDesc*>(
      payload.data() + sizeof(D9CCommandChunkWireDrawHeader) +
      2u * sizeof(D9CCommandChunkWireSectionDesc));
  auto* draw =
      reinterpret_cast<D9CCommandChunkWireDrawHeader*>(payload.data());
  draw->primitiveType = 0u;
  draw->primitiveCount = 0u;
  auto* shader = reinterpret_cast<D9CCommandChunkWireShaderBinding*>(
      payload.data() + shaderSection->payloadOffset);
  shader->valid = 0u;
  shader->handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX;
  auto* texture = reinterpret_cast<D9CCommandChunkWireTextureBinding*>(
      payload.data() + reinterpret_cast<D9CCommandChunkWireDrawHeader*>(
                           payload.data())
                           ->sectionPayloadOffset);
  texture->handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX;
  return payload;
}

std::vector<std::byte> bootstrapChunk() {
  auto payload = applyStatePayload();
  return makeChunk(std::array{Record{
      .type = D9C_COMMAND_RECORD_APPLY_STATE,
      .payload = std::move(payload),
  }});
}

RenderTapeSurfaceDescriptorV2 outputDescriptor();
std::vector<std::byte> textureDescriptor();

std::vector<std::byte> sparseTexturePayload(bool drawRecord,
                                            bool bindTexture,
                                            std::uint32_t handleIndex = 0u) {
  const auto sectionTableOffset = sizeof(D9CCommandChunkWireDrawHeader);
  const auto sectionPayloadOffset = alignUp(
      sectionTableOffset + sizeof(D9CCommandChunkWireSectionDesc),
      alignof(std::uint32_t));
  const D9CCommandChunkWireDrawHeader draw{
      .primitiveType = drawRecord ? 4u : 0u,
      .primitiveCount = drawRecord ? 1u : 0u,
      .sectionCount = 1u,
      .sectionTableOffset = static_cast<std::uint32_t>(sectionTableOffset),
      .sectionPayloadOffset = static_cast<std::uint32_t>(sectionPayloadOffset),
  };
  const D9CCommandChunkWireSectionDesc section{
      .kind = D9C_COMMAND_CHUNK_SECTION_TEXTURE,
      .elementSize = sizeof(D9CCommandChunkWireTextureBinding),
      .count = 1u,
      .payloadOffset = static_cast<std::uint32_t>(sectionPayloadOffset),
      .byteSize = sizeof(D9CCommandChunkWireTextureBinding),
  };
  const D9CCommandChunkWireTextureBinding texture{
      .slot = 0u,
      .valid = 1u,
      .handleIndex = bindTexture ? handleIndex
                                 : D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX,
  };
  std::vector<std::byte> payload(sectionPayloadOffset + sizeof(texture));
  std::memcpy(payload.data(), &draw, sizeof(draw));
  std::memcpy(payload.data() + sectionTableOffset, &section, sizeof(section));
  std::memcpy(payload.data() + sectionPayloadOffset, &texture,
              sizeof(texture));
  return payload;
}

std::vector<std::byte> setVsFloatPayload(
    std::uint32_t startRegister,
    std::span<const std::array<std::uint32_t, 4u>> registers) {
  const D9CCommandChunkWireSetConst fixed{
      .startRegister = startRegister,
      .registerCount = static_cast<std::uint32_t>(registers.size()),
  };
  std::vector<std::byte> payload(sizeof(fixed) +
                                 registers.size_bytes());
  std::memcpy(payload.data(), &fixed, sizeof(fixed));
  std::memcpy(payload.data() + sizeof(fixed), registers.data(),
              registers.size_bytes());
  return payload;
}

std::vector<std::byte> makeSparseFoldTape() {
  const auto output = outputDescriptor();
  const auto texture = textureDescriptor();
  const D9CCommandChunkWireClear clear{
      .flags = 1u,
      .colorARGB = 0xff204060u,
      .z = 1.0f,
      .rectOffset = sizeof(D9CCommandChunkWireClear),
  };
  const std::array firstConstants{
      std::array<std::uint32_t, 4u>{1u, 2u, 3u, 4u},
      std::array<std::uint32_t, 4u>{5u, 6u, 7u, 8u},
  };
  const std::array<std::array<std::uint32_t, 4u>, 1u> lastConstant{
      std::array<std::uint32_t, 4u>{9u, 10u, 11u, 12u},
  };
  const std::array records{
      Record{.type = D9C_COMMAND_RECORD_CLEAR, .payload = bytesOf(clear)},
      Record{
          .type = D9C_COMMAND_RECORD_APPLY_STATE,
          .payload = sparseTexturePayload(false, true),
          .handles = {D9CCommandChunkWireHandleEntry{
              .kind = kTexture.kind,
              .generation = kTexture.generation,
              .objectId = kTexture.objectId,
          }}},
      Record{.type = D9C_COMMAND_RECORD_SET_VS_CONST_F,
             .payload = setVsFloatPayload(0u, firstConstants)},
      Record{.type = D9C_COMMAND_RECORD_SET_VS_CONST_F,
             .payload = setVsFloatPayload(1u, lastConstant)},
      Record{.type = D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
             .payload = sparseTexturePayload(true, false)},
      Record{.type = D9C_COMMAND_RECORD_PRESENT,
             .payload = bytesOf(D9CCommandChunkWirePresent{})},
  };
  const auto chunk = makeChunk(records);
  const RenderTapeOracleAttachment oracle{
      .identity = kOutput,
      .descriptorKind =
          static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
  };
  std::array<std::byte, 1536u> gamma{};
  for (std::size_t index = 0u; index < gamma.size() / 2u; ++index) {
    const auto value = static_cast<std::uint16_t>((index % 256u) << 8u);
    std::memcpy(gamma.data() + index * 2u, &value, sizeof(value));
  }
  RenderTapeBuilder builder;
  builder.appendBootstrapState(bootstrapChunk(), gamma);
  builder.appendObjectDefine(
      kOutput, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
      std::as_bytes(std::span(&output, 1u)), 0u, {});
  builder.appendObjectDefine(
      kTexture, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Texture),
      texture, 0u, {}, 4u, 1u);
  builder.appendResourceMutation(kTexture, RenderTapeMutationKind::Upload, 0u,
                                 0u, 4u, kTextureDigest);
  builder.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 6u, .handleCount = 1u}, chunk);
  builder.appendPresentComplete(
      5u, 6u, RenderTapeDigestValidity::NotCaptured, {},
      std::as_bytes(std::span(&oracle, 1u)));
  return builder.seal();
}

std::vector<std::byte> frameChunk(bool firstFullSnapshot = true,
                                  bool includeAlias = false) {
  const D9CCommandChunkWireClear clear{
      .flags = 1u,
      .colorARGB = 0xff204060u,
      .z = 1.0f,
      .rectCount = 0u,
      .rectOffset = sizeof(D9CCommandChunkWireClear),
  };
  std::vector<D9CCommandChunkWireHandleEntry> handles{
      D9CCommandChunkWireHandleEntry{
          .kind = kTexture.kind,
          .generation = kTexture.generation,
          .objectId = kTexture.objectId,
      },
      D9CCommandChunkWireHandleEntry{
          .kind = kShader.kind,
          .generation = kShader.generation,
          .objectId = kShader.objectId,
      },
  };
  if (includeAlias) {
    handles.push_back(D9CCommandChunkWireHandleEntry{
        .kind = kAlias.kind,
        .generation = kAlias.generation,
        .objectId = kAlias.objectId,
    });
  }
  const auto firstDrawPayload = drawPayload(firstFullSnapshot, 0u, includeAlias);
  const auto secondDrawPayload =
      drawPayload(false, includeAlias ? 3u : 2u, includeAlias);
  const std::array records{
      Record{.type = D9C_COMMAND_RECORD_CLEAR, .payload = bytesOf(clear)},
      Record{.type = D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
             .payload = firstDrawPayload,
             .handles = handles},
      Record{.type = D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
             .payload = secondDrawPayload,
             .handles = handles},
      Record{.type = D9C_COMMAND_RECORD_PRESENT,
             .payload = bytesOf(D9CCommandChunkWirePresent{})},
  };
  return makeChunk(records);
}

RenderTapeBlobCatalogue catalogue() {
  return RenderTapeBlobCatalogue{.blobs = {
      RenderTapeBlob{.digest = kShaderDigest, .size = 4u, .verified = 1u},
      RenderTapeBlob{.digest = kTextureDigest, .size = 4u, .verified = 1u},
  }};
}

RenderTapeSurfaceDescriptorV2 outputDescriptor() {
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
          .width = 1u,
          .height = 1u,
          .depth = 1u,
      },
  };
}

std::vector<std::byte> textureDescriptor() {
  const RenderTapeTextureDescriptorV2 header{
      .schemaVersion = kRenderTapeTextureDescriptorVersion2,
      .dimension = static_cast<std::uint32_t>(
          RenderTapeTextureDimension::Texture2D),
      .mipLevelCount = 1u,
      .subresourceCount = 1u,
      .initialContentDisposition = static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::CompleteSeed),
  };
  const D9CSurfaceDesc level{
      .format = 21u,
      .resourceType = 3u,
      .pool = 0u,
      .width = 1u,
      .height = 1u,
      .depth = 1u,
  };
  std::vector<std::byte> descriptor(sizeof(header) + sizeof(level));
  std::memcpy(descriptor.data(), &header, sizeof(header));
  std::memcpy(descriptor.data() + sizeof(header), &level, sizeof(level));
  return descriptor;
}

std::vector<std::byte> makeFrameTape(bool firstFullSnapshot = true,
                                     bool includeTextureDefinition = true,
                                     std::uint64_t expectedTextureBytes = 4u,
                                     bool lateTextureSeed = false,
                                     bool includeAlias = false) {
  constexpr std::array<std::byte, 4u> shaderDescriptor{};
  const auto output = outputDescriptor();
  const auto texture = textureDescriptor();
  const auto frame = frameChunk(firstFullSnapshot, includeAlias);
  const RenderTapeOracleAttachment oracle{
      .identity = kOutput,
      .descriptorKind =
          static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
  };
  const RenderTapeFlushWaitControl wait{.waitedSeqId = 8u};
  const RenderTapeOrderedControlHeader control{
      .kind = static_cast<std::uint32_t>(RenderTapeControlKind::FlushWait),
      .disposition = static_cast<std::uint32_t>(
          RenderTapeControlDisposition::Completed),
      .controlBytes = sizeof(wait),
      .completionOrdinal = 8u,
  };

  RenderTapeBuilder builder;
  builder.appendBootstrapState(bootstrapChunk());
  builder.appendObjectDefine(
      kOutput, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
      std::as_bytes(std::span(&output, 1u)), 0u, {});
  if (includeTextureDefinition && !lateTextureSeed) {
    builder.appendObjectDefine(
        kTexture,
        static_cast<std::uint32_t>(RenderTapeDescriptorKind::Texture),
        texture, 0u, {}, expectedTextureBytes, 1u);
  }
  if (includeAlias) {
    const RenderTapeSurfaceDescriptorV2 alias{
        .schemaVersion = kRenderTapeSurfaceDescriptorVersion2,
        .storage = static_cast<std::uint32_t>(
            RenderTapeSurfaceStorage::TextureSubresource),
        .initialContentDisposition = static_cast<std::uint32_t>(
            RenderTapeInitialContentDisposition::Unavailable),
        .subresource = 0u,
        .parentTexture = kTexture,
        .surface = D9CSurfaceDesc{
            .format = 21u,
            .resourceType = 1u,
            .usage = 0u,
            .pool = 0u,
            .width = 1u,
            .height = 1u,
            .depth = 1u,
        },
    };
    builder.appendObjectDefine(
        kAlias, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
        std::as_bytes(std::span(&alias, 1u)), 0u, {}, 0u, 0u);
  }
  builder.appendObjectDefine(
      kShader, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Shader),
      shaderDescriptor, 4u, kShaderDigest);
  if (lateTextureSeed) {
    // An unrelated command must not close another identity's seed
    // expectation. The texture is defined and seeded immediately before its
    // first selected draw reference.
    builder.appendCommandChunk(
        CommandChunkEnvelope{.recordCount = 1u, .handleCount = 0u},
        bootstrapChunk());
    builder.appendObjectDefine(
        kTexture,
        static_cast<std::uint32_t>(RenderTapeDescriptorKind::Texture),
        texture, 0u, {}, expectedTextureBytes, 1u);
  }
  builder.appendResourceMutation(kTexture, RenderTapeMutationKind::Upload, 0u,
                                 0u, 4u, kTextureDigest);
  builder.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 4u,
                           .handleCount = includeAlias ? 6u : 4u},
      frame);
  builder.appendObjectDestroy(kTexture);
  builder.appendOrderedControl(control, std::as_bytes(std::span(&wait, 1u)));
  builder.appendPresentComplete(
      builder.eventCount() - 2u, 9u, RenderTapeDigestValidity::NotCaptured, {},
      std::as_bytes(std::span(&oracle, 1u)));
  return builder.seal();
}

std::vector<std::byte> makeSequenceTape() {
  constexpr std::array<std::byte, 4u> shaderDescriptor{};
  const auto output = outputDescriptor();
  const auto texture = textureDescriptor();
  const RenderTapeOracleAttachment oracle{
      .identity = kOutput,
      .descriptorKind =
          static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
  };
  const auto frame = frameChunk();
  RenderTapeBuilder builder(kRenderTapeProfileSequence);
  builder.appendBootstrapState(bootstrapChunk());
  builder.appendObjectDefine(
      kOutput, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
      std::as_bytes(std::span(&output, 1u)), 0u, {});
  builder.appendObjectDefine(
      kTexture, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Texture),
      texture, 0u, {}, 4u, 1u);
  builder.appendObjectDefine(
      kShader, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Shader),
      shaderDescriptor, 4u, kShaderDigest);
  builder.appendResourceMutation(kTexture, RenderTapeMutationKind::Upload, 0u,
                                 0u, 4u, kTextureDigest);
  builder.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 4u, .handleCount = 4u}, frame);
  builder.appendPresentComplete(
      6u, 7u, RenderTapeDigestValidity::NotCaptured, {},
      std::as_bytes(std::span(&oracle, 1u)));
  builder.appendResourceMutation(kTexture, RenderTapeMutationKind::Upload, 0u,
                                 0u, 4u, kTextureDigest);
  builder.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 4u, .handleCount = 4u}, frame);
  builder.appendPresentComplete(
      9u, 10u, RenderTapeDigestValidity::NotCaptured, {},
      std::as_bytes(std::span(&oracle, 1u)));
  return builder.seal();
}

RenderTapeProjectionSelector selector() {
  return RenderTapeProjectionSelector{
      .commandEventOrdinal = 6u,
      .firstRecordIndex = 1u,
      .recordCount = 2u,
  };
}

std::vector<std::byte> makeIdentity(
    const std::vector<std::byte>& tape, bool splitPasses = false,
    RenderTapeIdentityAuthority authority = RenderTapeIdentityAuthority::Capture) {
  const RenderTapeIdentitySource source{
      .eventOrdinal = 6u,
      .sourceOrdinal = 101u,
      .seqId = 501u,
      .captureToken = 77u,
      .recordCount = 4u,
      .firstRange = 0u,
      .rangeCount = splitPasses ? 2u : 1u,
  };
  const std::array ranges{
      RenderTapeIdentityRange{
          .eventOrdinal = 6u,
          .sourceOrdinal = 101u,
          .seqId = 501u,
          .logicalPassId = 9001u,
          .firstRecord = 0u,
          .recordCount = splitPasses ? 2u : 4u,
          .dagPassIndex = 3u,
          .passKind = 1u,
      },
      RenderTapeIdentityRange{
          .eventOrdinal = 6u,
          .sourceOrdinal = 101u,
          .seqId = 501u,
          .logicalPassId = 9002u,
          .firstRecord = 2u,
          .recordCount = 2u,
          .dagPassIndex = 4u,
          .passKind = 1u,
      },
  };
  return buildRenderTapeIdentity(
      tape, catalogue(), 42u, 6u, 77u,
      authority, std::span(&source, 1u),
      std::span(ranges).first(splitPasses ? 2u : 1u));
}

std::vector<std::byte> makeSplitIdentity(const std::vector<std::byte>& tape) {
  const std::array sources{
      RenderTapeIdentitySource{.eventOrdinal = 6u,
                               .sourceOrdinal = 101u,
                               .seqId = 501u,
                               .captureToken = 77u,
                               .firstRecord = 0u,
                               .recordCount = 2u,
                               .firstRange = 0u,
                               .rangeCount = 1u},
      RenderTapeIdentitySource{.eventOrdinal = 6u,
                               .sourceOrdinal = 102u,
                               .seqId = 502u,
                               .captureToken = 77u,
                               .firstRecord = 2u,
                               .recordCount = 2u,
                               .firstRange = 1u,
                               .rangeCount = 1u},
  };
  const std::array ranges{
      RenderTapeIdentityRange{.eventOrdinal = 6u,
                              .sourceOrdinal = 101u,
                              .seqId = 501u,
                              .logicalPassId = 9001u,
                              .firstRecord = 0u,
                              .recordCount = 2u,
                              .dagPassIndex = 3u,
                              .passKind = 1u},
      RenderTapeIdentityRange{.eventOrdinal = 6u,
                              .sourceOrdinal = 102u,
                              .seqId = 502u,
                              .logicalPassId = 9001u,
                              .firstRecord = 2u,
                              .recordCount = 2u,
                              .dagPassIndex = 3u,
                              .passKind = 1u},
  };
  return buildRenderTapeIdentity(
      tape, catalogue(), 42u, 6u, 77u, RenderTapeIdentityAuthority::Capture,
      sources, ranges);
}

std::vector<std::byte> makePolicyTape() {
  constexpr std::array<std::byte, 4u> shaderDescriptor{};
  const auto output = outputDescriptor();
  const auto texture = textureDescriptor();
  const RenderTapeOracleAttachment oracle{
      .identity = kOutput,
      .descriptorKind =
          static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
  };
  std::vector<Record> draws;
  draws.reserve(16u);
  for (std::uint32_t index = 0u; index < 16u; ++index) {
    draws.push_back(Record{
        .type = D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
        .payload = drawPayload(index == 0u, index * 2u),
        .handles = {
            {kTexture.kind, kTexture.generation, kTexture.objectId},
            {kShader.kind, kShader.generation, kShader.objectId},
        },
    });
  }
  const auto drawChunk = makeChunk(draws);
  const auto clear = bytesOf(D9CCommandChunkWireClear{
      .flags = 1u,
      .colorARGB = 0xff204060u,
      .z = 1.0f,
      .rectCount = 0u,
      .rectOffset = sizeof(D9CCommandChunkWireClear),
  });
  const auto present = bytesOf(D9CCommandChunkWirePresent{});
  const std::array coordinator{
      Record{.type = D9C_COMMAND_RECORD_CLEAR, .payload = clear},
      Record{.type = D9C_COMMAND_RECORD_PRESENT, .payload = present},
  };
  const auto coordinatorChunk = makeChunk(coordinator);
  const auto nonDrawChunk = makeChunk(std::array{Record{
      .type = D9C_COMMAND_RECORD_APPLY_STATE,
      .payload = applyStatePayload(),
  }});
  RenderTapeBuilder builder;
  builder.appendBootstrapState(bootstrapChunk());
  builder.appendObjectDefine(
      kOutput, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
      std::as_bytes(std::span(&output, 1u)), 0u, {});
  builder.appendObjectDefine(
      kTexture, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Texture),
      texture, 0u, {}, 4u, 1u);
  builder.appendObjectDefine(
      kShader, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Shader),
      shaderDescriptor, 4u, kShaderDigest);
  builder.appendResourceMutation(kTexture, RenderTapeMutationKind::Upload, 0u,
                                 0u, 4u, kTextureDigest);
  builder.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 16u, .handleCount = 32u}, drawChunk);
  builder.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 2u, .handleCount = 0u},
      coordinatorChunk);
  builder.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 1u, .handleCount = 0u},
      nonDrawChunk);
  builder.appendPresentComplete(
      7u, 8u, RenderTapeDigestValidity::NotCaptured, {},
      std::as_bytes(std::span(&oracle, 1u)));
  return builder.seal();
}

std::vector<std::byte> makePolicyIdentity(const std::vector<std::byte>& tape) {
  const std::array sources{
      RenderTapeIdentitySource{.eventOrdinal = 6u,
                               .sourceOrdinal = 101u,
                               .seqId = 501u,
                               .captureToken = 77u,
                               .recordCount = 16u,
                               .firstRange = 0u,
                               .rangeCount = 1u},
      RenderTapeIdentitySource{.eventOrdinal = 7u,
                               .sourceOrdinal = 102u,
                               .seqId = 502u,
                               .captureToken = 77u,
                               .recordCount = 2u,
                               .firstRange = 1u,
                               .rangeCount = 1u},
      RenderTapeIdentitySource{.eventOrdinal = 8u,
                               .sourceOrdinal = 103u,
                               .seqId = 503u,
                               .captureToken = 77u,
                               .recordCount = 1u,
                               .firstRange = 2u,
                               .rangeCount = 1u},
  };
  const std::array ranges{
      RenderTapeIdentityRange{.eventOrdinal = 6u,
                              .sourceOrdinal = 101u,
                              .seqId = 501u,
                              .logicalPassId = 9001u,
                              .firstRecord = 0u,
                              .recordCount = 16u,
                              .dagPassIndex = 3u,
                              .passKind = 1u},
      RenderTapeIdentityRange{.eventOrdinal = 7u,
                              .sourceOrdinal = 102u,
                              .seqId = 502u,
                              .logicalPassId = 9002u,
                              .firstRecord = 0u,
                              .recordCount = 2u,
                              .dagPassIndex = 4u,
                              .passKind = 1u},
      RenderTapeIdentityRange{.eventOrdinal = 8u,
                              .sourceOrdinal = 103u,
                              .seqId = 503u,
                              .logicalPassId = 9003u,
                              .firstRecord = 0u,
                              .recordCount = 1u,
                              .dagPassIndex = 5u,
                              .passKind = 1u},
  };
  RenderTapeIdentityValidationResult validation;
  auto result = buildRenderTapeIdentity(
      tape, catalogue(), 42u, 7u, 77u, RenderTapeIdentityAuthority::Capture,
      sources, ranges, &validation);
  if (result.empty()) {
    std::cerr << "policy fixture validation status "
              << renderTapeIdentityStatusName(validation.status)
              << " tape "
              << renderTapeValidationStatusName(
                     validation.tapeValidation.status)
              << " event " << validation.tapeValidation.failedEventIndex
              << " chunk "
              << static_cast<unsigned>(validation.tapeValidation.chunkStatus)
              << '\n';
  }
  return result;
}

void policyExploreTruthTable() {
  const auto tape = makePolicyTape();
  ImportedRenderTapeView imported;
  const auto tapeCheck = validateRenderTape(tape, catalogue(), &imported);
  if (!tapeCheck.valid()) {
    std::cerr << "policy tape validation event " << tapeCheck.failedEventIndex
              << " status " << renderTapeValidationStatusName(tapeCheck.status)
              << " chunk " << static_cast<unsigned>(tapeCheck.chunkStatus)
              << '\n';
    if (tapeCheck.failedEventIndex < imported.events.size()) {
      const auto event = imported.event(tapeCheck.failedEventIndex);
      RenderTapeCommandChunkHeader fixed{};
      std::memcpy(&fixed, event.payload.data(), sizeof(fixed));
      ImportedChunkView chunk;
      const auto detail = validateCommandChunk(
          event.payload.subspan(sizeof(fixed), fixed.chunkBytes),
          CommandChunkEnvelope{fixed.wireVersion, fixed.recordCount,
                               fixed.handleCount}, &chunk);
      std::cerr << "detail record " << detail.failedRecordIndex << " section "
                << detail.failedSectionIndex << " offset " << detail.byteOffset
                << '\n';
    }
  }
  const auto identity = makePolicyIdentity(tape);
  check(!identity.empty(), "policy fixture identity must validate");
  const auto result = exploreRenderTapeParallelPolicy(tape, catalogue(), identity);
  check(result.valid() && result.authenticatedInput && result.structuralOnly &&
            !result.proofCoreValidated,
        "policy analyzer reports authenticated structural-only status");
  check(result.candidates.size() == 4u,
        "policy analyzer enumerates exactly 2/4/8/16 children");
  check(renderTapePolicyCanonicalCandidateCount(1u) == 0u &&
            renderTapePolicyCanonicalCandidateCount(2u) == 1u &&
            renderTapePolicyCanonicalCandidateCount(4u) == 2u &&
            renderTapePolicyCanonicalCandidateCount(8u) == 3u &&
            renderTapePolicyCanonicalCandidateCount(16u) == 4u &&
            renderTapePolicyCandidateBudgetAccepts(
                kRenderTapePolicyMaxCandidates - 4u, 16u) &&
            !renderTapePolicyCandidateBudgetAccepts(
                kRenderTapePolicyMaxCandidates - 3u, 16u),
        "canonical policy enumeration fails closed before partial output");
  for (std::size_t index = 0u; index < result.candidates.size(); ++index) {
    const auto& candidate = result.candidates[index];
    check(candidate.children.size() == (2u << index) &&
              candidate.drawTotal == 16u && candidate.primitiveTotal == 16u &&
              candidate.firstRecord == 0u && candidate.recordCount == 16u &&
              candidate.vertexShaderFacts == 16u &&
              candidate.pixelShaderFacts == 0u &&
              candidate.vertexInputFacts == 0u &&
              candidate.uniformSectionFacts == 0u &&
              candidate.pipelineInputSectionFacts == 16u,
          "policy candidate has canonical bounded draw ranges");
    check(candidate.children.front().firstRecord == 0u &&
              candidate.children.back().firstRecord < 16u,
          "policy children stay within source draw records");
  }
  check(result.rejections.size() == 2u &&
            result.rejections[0].reason ==
                RenderTapePolicyRejectionReason::CoordinatorRecord &&
            result.rejections[1].reason ==
                RenderTapePolicyRejectionReason::NonDrawRecord,
        "coordinator and non-draw ranges remain outside children");
  auto permuted = identity;
  std::reverse(permuted.begin() + sizeof(RenderTapeIdentityHeader),
               permuted.end());
  const auto rejected = exploreRenderTapeParallelPolicy(
      tape, catalogue(), std::span<const std::byte>(permuted));
  check(!rejected.valid(), "malformed identity fails closed");
}

template <typename T>
T& sidecarAt(std::vector<std::byte>& bytes, std::size_t offset) {
  check(offset + sizeof(T) <= bytes.size(), "sidecar mutation is in bounds");
  return *reinterpret_cast<T*>(bytes.data() + offset);
}

void identityTruthTableAndMalformedInputs() {
  const auto tape = makeFrameTape();
  const auto blobs = catalogue();
  auto identity = makeIdentity(
      tape, false, RenderTapeIdentityAuthority::ProviderReplay);
  RenderTapeIdentityView view;
  const auto accepted = validateRenderTapeIdentity(tape, blobs, identity, &view);
  check(accepted.valid() && view.sources.size() == 1u &&
            view.ranges.size() == 1u &&
            view.header.authority == static_cast<std::uint32_t>(
                                         RenderTapeIdentityAuthority::ProviderReplay),
        "identity sidecar accepts exact provider-replay value identity");
  std::uint64_t pass = 0u;
  check(renderTapeIdentityOwnsSelection(view, 6u, 1u, 2u, &pass) &&
            pass == 9001u &&
            !renderTapeIdentityOwnsSelection(view, 6u, 3u, 2u),
        "identity sidecar owns only in-range record selections");

  auto malformed = identity;
  sidecarAt<RenderTapeIdentityHeader>(malformed, 0u).eventsDigest[0] ^=
      std::byte{1u};
  check(validateRenderTapeIdentity(tape, blobs, malformed).status ==
            RenderTapeIdentityStatus::EventsMismatch,
        "altered events binding fails closed");
  malformed = identity;
  auto& source = sidecarAt<RenderTapeIdentitySource>(
      malformed, sizeof(RenderTapeIdentityHeader));
  source.seqId = 0u;
  check(validateRenderTapeIdentity(tape, blobs, malformed).status ==
            RenderTapeIdentityStatus::NonMonotoneSource,
        "zero/non-monotone production sequence fails closed");
  malformed = identity;
  sidecarAt<RenderTapeIdentitySource>(
      malformed, sizeof(RenderTapeIdentityHeader)).captureToken = 78u;
  check(validateRenderTapeIdentity(tape, blobs, malformed).status ==
            RenderTapeIdentityStatus::SourceCoverageMismatch,
        "stale bounded capture token fails closed");
  malformed = identity;
  auto& range = sidecarAt<RenderTapeIdentityRange>(
      malformed, sizeof(RenderTapeIdentityHeader) +
                     sizeof(RenderTapeIdentitySource));
  range.recordCount = 3u;
  check(validateRenderTapeIdentity(tape, blobs, malformed).status ==
            RenderTapeIdentityStatus::RecordCoverageMismatch,
        "partial record coverage fails closed");

  malformed = makeIdentity(tape, true);
  const auto rangeOffset = sizeof(RenderTapeIdentityHeader) +
                           sizeof(RenderTapeIdentitySource);
  auto& second = sidecarAt<RenderTapeIdentityRange>(
      malformed, rangeOffset + sizeof(RenderTapeIdentityRange));
  second.logicalPassId = 9001u;
  second.passKind = 2u;
  check(validateRenderTapeIdentity(tape, blobs, malformed).status ==
            RenderTapeIdentityStatus::PassIdentityMismatch,
        "inconsistent repeated logical pass identity fails closed");

  const auto split = makeSplitIdentity(tape);
  RenderTapeIdentityView splitView;
  const auto splitAccepted =
      validateRenderTapeIdentity(tape, blobs, split, &splitView);
  check(splitAccepted.valid() && splitView.sources.size() == 2u &&
            splitView.sources[0].firstRecord == 0u &&
            splitView.sources[1].firstRecord == 2u &&
            splitView.ranges[1].firstRecord == 2u,
        "v2 identity joins contiguous source fragments under one event");
  std::uint64_t splitPass = 0u;
  check(renderTapeIdentityOwnsSelection(splitView, 6u, 2u, 2u, &splitPass) &&
            splitPass == 9001u &&
            !renderTapeIdentityOwnsSelection(splitView, 6u, 1u, 3u),
        "event-local selection resolves through the second source fragment");

  malformed = split;
  sidecarAt<RenderTapeIdentitySource>(
      malformed, sizeof(RenderTapeIdentityHeader) +
                     sizeof(RenderTapeIdentitySource)).firstRecord = 3u;
  check(validateRenderTapeIdentity(tape, blobs, malformed).status ==
            RenderTapeIdentityStatus::SourceCoverageMismatch,
        "a source gap across one event fails exact aggregate coverage");
  malformed = split;
  sidecarAt<RenderTapeIdentityRange>(
      malformed, sizeof(RenderTapeIdentityHeader) +
                     2u * sizeof(RenderTapeIdentitySource) +
                     sizeof(RenderTapeIdentityRange)).passKind = 2u;
  check(validateRenderTapeIdentity(tape, blobs, malformed).status ==
            RenderTapeIdentityStatus::PassIdentityMismatch,
        "cross-fragment logical pass metadata is authenticated");
}

void executableProjectionMaterializesCanonicalBundle() {
  const auto tape = makeFrameTape(false);
  const auto blobs = catalogue();
  const auto identity = makeIdentity(tape);
  const auto materialized = materializeRenderTapeProjectionBundle(
      tape, blobs, identity, selector());
  check(materialized.valid(),
        renderTapeProjectionBundleStatusName(materialized.status));
  const auto processLocalIdentity = makeIdentity(
      tape, false, RenderTapeIdentityAuthority::ProviderReplay);
  check(materializeRenderTapeProjectionBundle(
            tape, blobs, processLocalIdentity, selector()).status ==
            RenderTapeProjectionBundleStatus::InvalidIdentity,
        "persisted provider-replay identity cannot authorize offline projection");
  check(materialized.logicalPassId == 9001u &&
            materialized.referencedBlobDigests.size() == 2u &&
            validateRenderTape(materialized.bytes, blobs).valid(),
        "executable projection conserves pass and exact blob closure");
  ImportedRenderTapeView projected;
  check(importPrevalidatedRenderTape(materialized.bytes, projected) &&
            projected.header.profile == kRenderTapeProfileFrame,
        "executable projection emits canonical Render Tape v2");
  std::vector<std::uint32_t> recordTypes;
  for (std::uint32_t eventIndex = 0u; eventIndex < projected.events.size();
       ++eventIndex) {
    const auto event = projected.event(eventIndex);
    if (static_cast<RenderTapeEventType>(event.header.type) !=
        RenderTapeEventType::CommandChunk) {
      continue;
    }
    RenderTapeCommandChunkHeader fixed{};
    std::memcpy(&fixed, event.payload.data(), sizeof(fixed));
    ImportedChunkView chunk;
    check(importPrevalidatedCommandChunk(
              event.payload.subspan(sizeof(fixed), fixed.chunkBytes),
              CommandChunkEnvelope{fixed.wireVersion, fixed.recordCount,
                                   fixed.handleCount},
              chunk),
          "projected command chunk imports");
    for (const auto& record : chunk.records) recordTypes.push_back(record.type);
  }
  check(recordTypes == std::vector<std::uint32_t>{
                           D9C_COMMAND_RECORD_CLEAR,
                           D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
                           D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
                           D9C_COMMAND_RECORD_PRESENT},
        "projected command executes Clear, selected Draw range, then Present");

  const auto splitIdentity = makeIdentity(tape, true);
  check(materializeRenderTapeProjectionBundle(
            tape, blobs, splitIdentity, selector()).status ==
            RenderTapeProjectionBundleStatus::SelectionOutsidePass,
        "draw range crossing a frozen pass boundary fails before effects");
  auto mismatched = identity;
  sidecarAt<RenderTapeIdentityHeader>(mismatched, 0u).eventsBytes += 1u;
  check(materializeRenderTapeProjectionBundle(
            tape, blobs, mismatched, selector()).status ==
            RenderTapeProjectionBundleStatus::InvalidIdentity,
        "mismatched identity fails before materialization");
}

void acceptedRangeConservesCanonicalIdentity() {
  const auto tape = makeFrameTape();
  const auto blobs = catalogue();
  check(validateRenderTape(tape, blobs).valid(),
        "projection fixture source must validate");
  const auto projected = projectRenderTapeDrawSlice(tape, blobs, selector());
  check(projected.valid(), renderTapeProjectionStatusName(projected.status));
  check(projected.selectedLocators.size() == 2u &&
            projected.selectedLocators[0].eventOrdinal == 6u &&
            projected.selectedLocators[0].recordIndex == 1u &&
            projected.selectedLocators[1].eventOrdinal == 6u &&
            projected.selectedLocators[1].recordIndex == 2u,
        "draw locators must preserve canonical event ordinal and record order");
  check(projected.clearLocator.eventOrdinal == 6u &&
            projected.clearLocator.recordIndex == 0u &&
            projected.presentLocator.eventOrdinal == 6u &&
            projected.presentLocator.recordIndex == 3u,
        "same-command Clear and immediately-following Present stay outside range");
  check(projected.objects.size() == 3u &&
            projected.objects[0].identity.objectId == kOutput.objectId &&
            projected.objects[1].identity.objectId == kTexture.objectId &&
            projected.objects[1].identity.generation == kTexture.generation &&
            projected.objects[2].identity.objectId == kShader.objectId &&
            projected.objects[2].identity.generation == kShader.generation,
        "projection must conserve exact generation-qualified selected objects");
  check(projected.blobReferences.size() == 2u &&
            projected.blobReferences[0].digest == kShaderDigest &&
            projected.blobReferences[1].digest == kTextureDigest &&
            projected.blobReferences[1].initialContent == 1u,
        "projection must conserve source-ordered immutable and seed digests");
  check(projected.objects[1].initialContentBytes == 4u &&
            projected.objects[1].initialContentCount == 1u &&
            projected.sourceRecordCount == 4u &&
            projected.selectedDrawCount == 2u &&
            projected.excludedRecordCount == 2u,
        "projection must conserve initial content and record counts");
  check(projected.excludedEvents.size() == 3u &&
            projected.excludedEvents[0].kind ==
                RenderTapeProjectionExcludedKind::ObjectDestroy &&
            projected.excludedEvents[1].kind ==
                RenderTapeProjectionExcludedKind::OrderedControl &&
            projected.excludedEvents[2].kind ==
                RenderTapeProjectionExcludedKind::PresentComplete,
        "destroy, control, and completion stay coordinator-owned and excluded");
}

void projectionAccountsLatePerIdentitySeed() {
  const auto tape = makeFrameTape(true, true, 4u, true);
  const auto blobs = catalogue();
  check(validateRenderTape(tape, blobs).valid(),
        "late-seed projection fixture source must validate");
  auto lateSelector = selector();
  lateSelector.commandEventOrdinal = 7u;
  const auto projected =
      projectRenderTapeDrawSlice(tape, blobs, lateSelector);
  check(projected.valid(), renderTapeProjectionStatusName(projected.status));
  const auto texture = std::find_if(
      projected.objects.begin(), projected.objects.end(), [](const auto& object) {
        return object.identity.objectId == kTexture.objectId;
      });
  check(projected.blobReferences.size() == 2u &&
            projected.blobReferences[1].identity.objectId == kTexture.objectId &&
            projected.blobReferences[1].initialContent == 1u &&
            texture != projected.objects.end() &&
            texture->initialContentBytes == 4u &&
            texture->initialContentCount == 1u,
        "projection must classify a late matching mutation as initial content");
}

void projectionPreservesDispositionAndAliasMetadata() {
  const auto tape = makeFrameTape(true, true, 4u, false, true);
  const auto blobs = catalogue();
  const auto validation = validateRenderTape(tape, blobs);
  check(validation.valid(),
        "alias projection fixture source must validate");
  auto aliasSelector = selector();
  aliasSelector.commandEventOrdinal = 7u;
  const auto projected =
      projectRenderTapeDrawSlice(tape, blobs, aliasSelector);
  check(projected.valid(), renderTapeProjectionStatusName(projected.status));
  const auto texture = std::find_if(
      projected.objects.begin(), projected.objects.end(), [](const auto& object) {
        return object.identity.objectId == kTexture.objectId;
      });
  const auto alias = std::find_if(
      projected.objects.begin(), projected.objects.end(), [](const auto& object) {
        return object.identity.objectId == kAlias.objectId;
      });
  check(texture != projected.objects.end() &&
            texture->initialContentDisposition == static_cast<std::uint32_t>(
                RenderTapeInitialContentDisposition::CompleteSeed) &&
            alias != projected.objects.end() &&
            alias->initialContentDisposition == static_cast<std::uint32_t>(
                RenderTapeInitialContentDisposition::Unavailable) &&
            alias->aliasParentTexture.objectId == kTexture.objectId &&
            alias->aliasParentTexture.generation == kTexture.generation &&
            alias->aliasSubresource == 0u,
        "projection preserves texture disposition and exact alias metadata");
}

void sparseStateFoldTruthTable() {
  const auto tape = makeSparseFoldTape();
  const auto blobs = catalogue();
  const auto sourceValidation = validateRenderTape(tape, blobs);
  check(sourceValidation.valid(),
        renderTapeValidationStatusName(sourceValidation.status));
  const auto folded = foldRenderTapeStateForDraw(tape, blobs, 5u, 4u);
  const auto repeated = foldRenderTapeStateForDraw(tape, blobs, 5u, 4u);
  check(folded.valid(), renderTapeStateFoldStatusName(folded.status));
  check(!folded.selectedRecordWasFullSnapshot &&
            folded.coverageMask == kRenderTapeRequiredCategoryMask &&
            folded.bootstrapChunk == repeated.bootstrapChunk &&
            folded.gammaRamp.size() == 1536u &&
            folded.gammaRamp == repeated.gammaRamp &&
            folded.referencedIdentities.empty(),
        "default sparse fold is complete, deterministic, and drops an "
        "overwritten binding");

  D9CCommandChunkWireHeader header{};
  std::memcpy(&header, folded.bootstrapChunk.data(), sizeof(header));
  ImportedChunkView chunk;
  check(importPrevalidatedCommandChunk(
            folded.bootstrapChunk,
            CommandChunkEnvelope{header.version, header.recordCount,
                                 header.handleCount},
            chunk) &&
            chunk.records.size() == 1u &&
            chunk.records[0].type == D9C_COMMAND_RECORD_APPLY_STATE &&
            (chunk.record(0u).drawHeader.flags &
             D9C_COMMAND_CHUNK_DRAW_FLAG_FULL_SNAPSHOT) != 0u,
        "fold emits one canonical APPLY_STATE|FULL_SNAPSHOT record");
  const auto record = chunk.record(0u);
  const auto findSection = [&](std::uint16_t kind) {
    for (std::size_t index = 0u; index < record.sections.size(); ++index) {
      const auto section = record.section(index);
      if (section.descriptor.kind == kind) return section;
    }
    throw TestFailure("folded bootstrap omitted required section");
  };
  const auto textureSection =
      findSection(D9C_COMMAND_CHUNK_SECTION_TEXTURE);
  D9CCommandChunkWireTextureBinding texture{};
  std::memcpy(&texture, textureSection.payload.data(), sizeof(texture));
  check(textureSection.descriptor.count == D9C_DRAW_PACKET_MAX_TEXTURES &&
            texture.slot == 0u && texture.valid == 1u &&
            texture.handleIndex == D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX,
        "last sparse texture binding wins and expands to complete slots");
  const auto constants = findSection(D9C_COMMAND_CHUNK_SECTION_VS_CONST_F);
  D9CCommandChunkWireConstantRange range{};
  std::memcpy(&range, constants.payload.data(), sizeof(range));
  std::array<std::uint32_t, 4u> registerOne{};
  std::memcpy(registerOne.data(),
              constants.payload.data() + sizeof(range) + 16u,
              sizeof(registerOne));
  check(range.startRegister == 0u &&
            range.registerCount == D9C_DRAW_PACKET_MAX_CONST_VS_F &&
            registerOne == std::array<std::uint32_t, 4u>{9u, 10u, 11u, 12u},
        "overlapping standalone constant ranges preserve exact last-write semantics");

  check(foldRenderTapeStateForDraw(tape, blobs, 5u, 3u).status ==
            RenderTapeStateFoldStatus::InvalidSelection,
        "non-Draw fold target rejects before producing a bootstrap");
  auto incomplete = tape;
  auto* tapeHeader = reinterpret_cast<RenderTapeHeader*>(incomplete.data());
  auto* events = reinterpret_cast<RenderTapeEventHeader*>(
      incomplete.data() + tapeHeader->eventTableOffset);
  auto* bootstrap = reinterpret_cast<RenderTapeBootstrapHeader*>(
      incomplete.data() + tapeHeader->payloadArenaOffset +
      events[0u].payloadOffset);
  bootstrap->requiredCategoryMask &= ~(std::uint64_t{1} << 4u);
  const auto rejected =
      foldRenderTapeStateForDraw(incomplete, blobs, 5u, 4u);
  check(rejected.status == RenderTapeStateFoldStatus::InvalidSource &&
            rejected.bootstrapChunk.empty(),
        "incomplete category manifest rejects before output construction");
}

void failClosedSelectionAndClosureCases() {
  const auto blobs = catalogue();
  check(projectRenderTapeDrawSlice(makeSequenceTape(), blobs, selector()).status ==
            RenderTapeProjectionStatus::UnsupportedProfile,
        "sequence tape must not enter the frame projection lane");

  auto nonDraw = selector();
  nonDraw.firstRecordIndex = 0u;
  check(projectRenderTapeDrawSlice(makeFrameTape(), blobs, nonDraw).status ==
            RenderTapeProjectionStatus::NonDrawRecord,
        "Clear in selected record range must fail closed");
  auto coordinator = selector();
  coordinator.commandEventOrdinal = 8u;
  coordinator.firstRecordIndex = 0u;
  coordinator.recordCount = 1u;
  check(projectRenderTapeDrawSlice(makeFrameTape(), blobs, coordinator).status ==
            RenderTapeProjectionStatus::InvalidSelection,
        "coordinator event cannot masquerade as a command record range");
  const auto defaultDelta =
      projectRenderTapeDrawSlice(makeFrameTape(false), blobs, selector());
  check(defaultDelta.valid() && defaultDelta.stateFold.valid() &&
            !defaultDelta.stateFold.selectedRecordWasFullSnapshot &&
            defaultDelta.stateFold.referencedIdentities.size() == 2u &&
            defaultDelta.stateFold.referencedIdentities[0].generation ==
                kTexture.generation &&
            defaultDelta.stateFold.referencedIdentities[1].generation ==
                kShader.generation,
        "first default sparse Draw derives a complete bootstrap without the diagnostic flag");
  auto outOfRange = selector();
  outOfRange.firstRecordIndex = 3u;
  outOfRange.recordCount = 2u;
  check(projectRenderTapeDrawSlice(makeFrameTape(), blobs, outOfRange).status ==
            RenderTapeProjectionStatus::InvalidSelection,
        "out-of-range record selector must fail closed");

  check(projectRenderTapeDrawSlice(makeFrameTape(true, false), blobs,
                                   selector())
                .status == RenderTapeProjectionStatus::InvalidSource,
        "missing exact definition must fail during source validation");
  auto wrongGeneration = makeFrameTape();
  auto* header = reinterpret_cast<RenderTapeHeader*>(wrongGeneration.data());
  auto* events = reinterpret_cast<RenderTapeEventHeader*>(
      wrongGeneration.data() + header->eventTableOffset);
  auto* command = reinterpret_cast<RenderTapeCommandChunkHeader*>(
      wrongGeneration.data() + header->payloadArenaOffset +
      events[5u].payloadOffset);
  auto* chunk = reinterpret_cast<D9CCommandChunkWireHeader*>(command + 1u);
  auto* handles = reinterpret_cast<D9CCommandChunkWireHandleEntry*>(
      reinterpret_cast<std::byte*>(chunk) + chunk->handleTableOffset);
  handles[0].generation += 1u;
  check(projectRenderTapeDrawSlice(wrongGeneration, blobs, selector()).status ==
            RenderTapeProjectionStatus::InvalidSource,
        "wrong-generation selected identity must fail before projection");
  check(projectRenderTapeDrawSlice(makeFrameTape(true, true, 8u), blobs,
                                   selector())
                .status == RenderTapeProjectionStatus::InvalidSource,
        "incomplete initial seed must fail before projection");
}

} // namespace

int main(int argc, char** argv) {
  try {
    if (argc == 4 &&
        std::string_view(argv[1]) == "--write-policy-fixture") {
      const auto tape = makePolicyTape();
      const auto identity = makePolicyIdentity(tape);
      std::ofstream output(argv[2], std::ios::binary);
      output.write(reinterpret_cast<const char*>(tape.data()),
                   static_cast<std::streamsize>(tape.size()));
      check(output.good(), "failed to write policy CLI fixture");
      std::ofstream identityOutput(argv[3], std::ios::binary);
      identityOutput.write(
          reinterpret_cast<const char*>(identity.data()),
          static_cast<std::streamsize>(identity.size()));
      check(identityOutput.good(), "failed to write policy identity fixture");
      return 0;
    }
    if ((argc == 3 || argc == 4) &&
        std::string_view(argv[1]) == "--write-fixture") {
      const auto tape = makeFrameTape(false);
      std::ofstream output(argv[2], std::ios::binary);
      output.write(reinterpret_cast<const char*>(tape.data()),
                   static_cast<std::streamsize>(tape.size()));
      check(output.good(), "failed to write projection CLI fixture");
      if (argc == 4) {
        const auto identity = makeIdentity(tape);
        std::ofstream identityOutput(argv[3], std::ios::binary);
        identityOutput.write(
            reinterpret_cast<const char*>(identity.data()),
            static_cast<std::streamsize>(identity.size()));
        check(identityOutput.good(), "failed to write identity CLI fixture");
      }
      return 0;
    }
    check(argc == 1,
          "usage: render_tape_projection_spec "
          "[--write-fixture path [identity]] "
          "[--write-policy-fixture tape identity]");
    acceptedRangeConservesCanonicalIdentity();
    projectionAccountsLatePerIdentitySeed();
    projectionPreservesDispositionAndAliasMetadata();
    sparseStateFoldTruthTable();
    failClosedSelectionAndClosureCases();
    identityTruthTableAndMalformedInputs();
    policyExploreTruthTable();
    executableProjectionMaterializesCanonicalBundle();
  } catch (const std::exception& error) {
    std::cerr << "render_tape_projection_spec failed: " << error.what() << '\n';
    return 1;
  }
  std::cout << "render_tape_projection_spec passed\n";
  return 0;
}
