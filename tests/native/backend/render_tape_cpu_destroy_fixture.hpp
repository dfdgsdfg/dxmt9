#pragma once

#include "device_c_render_tape.hpp"
#include "device_c_render_tape_capture.hpp"
#include "device_c_render_tape_descriptors.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

namespace dxmt9::d3d9::test {

struct GeneralCpuDestroyFixture {
  std::vector<std::byte> tape{};
  std::vector<std::vector<std::byte>> blobs{};
};

enum class GeneralCpuMutationFixture {
  None,
  UnrelatedPlain,
  ReferencedPlain,
  ReferencedDiscard,
};

enum class GeneralCpuResetFixture {
  A,
  B,
};

namespace general_cpu_destroy_fixture_detail {

inline std::size_t alignUp(std::size_t value, std::size_t alignment) {
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

inline std::vector<std::byte> makeChunk(std::span<const Record> records) {
  const auto recordTableOffset = sizeof(D9CCommandChunkWireHeader);
  std::size_t handleCount = 0u;
  for (const auto& record : records) handleCount += record.handles.size();
  const auto handleTableOffset = alignUp(
      recordTableOffset + records.size() * sizeof(D9CCommandChunkWireRecordHeader),
      alignof(D9CCommandChunkWireHandleEntry));
  const auto payloadArenaOffset = alignUp(
      handleTableOffset + handleCount * sizeof(D9CCommandChunkWireHandleEntry),
      alignof(std::uint32_t));

  std::vector<D9CCommandChunkWireRecordHeader> recordHeaders;
  recordHeaders.reserve(records.size());
  std::vector<D9CCommandChunkWireHandleEntry> handles;
  handles.reserve(handleCount);
  std::vector<std::byte> payload;
  for (const auto& record : records) {
    const auto payloadOffset = alignUp(payload.size(), alignof(std::uint32_t));
    payload.resize(payloadOffset);
    recordHeaders.push_back({
        .type = record.type,
        .payloadOffset = static_cast<std::uint32_t>(payloadOffset),
        .payloadSize = static_cast<std::uint32_t>(record.payload.size()),
        .firstHandle = static_cast<std::uint32_t>(handles.size()),
        .handleCount = static_cast<std::uint32_t>(record.handles.size()),
    });
    handles.insert(handles.end(), record.handles.begin(), record.handles.end());
    payload.insert(payload.end(), record.payload.begin(), record.payload.end());
  }

  const D9CCommandChunkWireHeader header{
      .version = D9C_COMMAND_CHUNK_WIRE_VERSION,
      .headerSize = D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE,
      .recordHeaderSize = D9C_COMMAND_CHUNK_WIRE_RECORD_HEADER_SIZE,
      .handleEntrySize = D9C_COMMAND_CHUNK_WIRE_HANDLE_ENTRY_SIZE,
      .recordTableOffset = static_cast<std::uint32_t>(recordTableOffset),
      .recordCount = static_cast<std::uint32_t>(recordHeaders.size()),
      .handleTableOffset = static_cast<std::uint32_t>(handleTableOffset),
      .handleCount = static_cast<std::uint32_t>(handles.size()),
      .payloadArenaOffset = static_cast<std::uint32_t>(payloadArenaOffset),
      .payloadArenaSize = static_cast<std::uint32_t>(payload.size()),
  };
  std::vector<std::byte> bytes(payloadArenaOffset + payload.size());
  std::memcpy(bytes.data(), &header, sizeof(header));
  std::memcpy(bytes.data() + recordTableOffset, recordHeaders.data(),
              recordHeaders.size() * sizeof(recordHeaders[0]));
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

inline std::vector<std::byte> singleRecordChunk(
    std::uint32_t type, std::span<const std::byte> payload,
    std::span<const D9CCommandChunkWireHandleEntry> handles = {}) {
  const Record record{
      .type = type,
      .payload = std::vector<std::byte>(payload.begin(), payload.end()),
      .handles = std::vector<D9CCommandChunkWireHandleEntry>(handles.begin(),
                                                              handles.end()),
  };
  return makeChunk(std::span<const Record>(&record, 1u));
}

inline constexpr D9CWireObjectIdentity kOutput{
    .kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
    .generation = 4u,
    .objectId = 53u,
};
inline constexpr D9CWireObjectIdentity kVertexBuffer{
    .kind = D9C_CHUNK_HANDLE_KIND_BUFFER,
    .generation = 4u,
    .objectId = 54u,
};
inline constexpr D9CWireObjectIdentity kIndexBuffer{
    .kind = D9C_CHUNK_HANDLE_KIND_BUFFER,
    .generation = 4u,
    .objectId = 55u,
};
inline constexpr D9CWireObjectIdentity kVertexShader{
    .kind = D9C_CHUNK_HANDLE_KIND_SHADER,
    .generation = 4u,
    .objectId = 56u,
};
inline constexpr D9CWireObjectIdentity kVertexDeclaration{
    .kind = D9C_CHUNK_HANDLE_KIND_VERTEX_DECL,
    .generation = 4u,
    .objectId = 57u,
};
inline constexpr D9CWireObjectIdentity kUnrelatedBuffer{
    .kind = D9C_CHUNK_HANDLE_KIND_BUFFER,
    .generation = 4u,
    .objectId = 58u,
};

inline std::vector<std::byte> bootstrapChunk() {
  std::array<D9CCommandChunkWireTextureBinding, D9C_DRAW_PACKET_MAX_TEXTURES>
      textures{};
  for (std::uint32_t slot = 0u; slot < textures.size(); ++slot) {
    textures[slot] = {
        .slot = slot,
        .valid = 1u,
        .handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX,
    };
  }
  std::array<D9CCommandChunkWireStreamBinding, D9C_DRAW_PACKET_MAX_STREAMS>
      streams{};
  for (std::uint32_t slot = 0u; slot < streams.size(); ++slot) {
    streams[slot] = {
        .slot = slot,
        .valid = 1u,
        .handleIndex = slot == 0u ? 0u : D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX,
        .stride = slot == 0u ? 28u : 0u,
    };
  }
  const std::array shaders{D9CCommandChunkWireShaderBinding{
      .stage = D9C_COMMAND_CHUNK_SHADER_STAGE_VERTEX,
      .valid = 1u,
      .handleIndex = 1u,
  }};
  const D9CCommandChunkWireVertexInput vertexInput{
      .valid = 1u,
      .kind = D9C_COMMAND_CHUNK_VERTEX_INPUT_DECLARATION,
      .value = 0u,
      .handleIndex = 2u,
  };
  const D9CCommandChunkWireIndexBinding index{
      .valid = 1u,
      .handleIndex = 3u,
  };
  const std::array renderTargets{D9CCommandChunkWireRenderTargetBinding{
      .slot = 0u,
      .valid = 1u,
      .handleIndex = 4u,
  }};

  constexpr std::size_t sectionCount = 6u;
  const auto sectionTableOffset = sizeof(D9CCommandChunkWireDrawHeader);
  const auto sectionPayloadOffset = alignUp(
      sectionTableOffset + sectionCount * sizeof(D9CCommandChunkWireSectionDesc),
      alignof(std::uint32_t));
  const auto textureOffset = sectionPayloadOffset;
  const auto streamOffset = textureOffset + sizeof(textures);
  const auto shaderOffset = streamOffset + sizeof(streams);
  const auto vertexInputOffset = shaderOffset + sizeof(shaders);
  const auto indexOffset = vertexInputOffset + sizeof(vertexInput);
  const auto renderTargetOffset = indexOffset + sizeof(index);
  const D9CCommandChunkWireDrawHeader draw{
      .flags = D9C_COMMAND_CHUNK_DRAW_FLAG_FULL_SNAPSHOT,
      .sectionCount = sectionCount,
      .sectionTableOffset = static_cast<std::uint32_t>(sectionTableOffset),
      .sectionPayloadOffset = static_cast<std::uint32_t>(sectionPayloadOffset),
  };
  const std::array sections{
      D9CCommandChunkWireSectionDesc{
          .kind = D9C_COMMAND_CHUNK_SECTION_TEXTURE,
          .elementSize = sizeof(textures[0]),
          .count = static_cast<std::uint32_t>(textures.size()),
          .payloadOffset = static_cast<std::uint32_t>(textureOffset),
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
          .elementSize = sizeof(shaders[0]),
          .count = static_cast<std::uint32_t>(shaders.size()),
          .payloadOffset = static_cast<std::uint32_t>(shaderOffset),
          .byteSize = sizeof(shaders),
      },
      D9CCommandChunkWireSectionDesc{
          .kind = D9C_COMMAND_CHUNK_SECTION_VERTEX_INPUT,
          .elementSize = sizeof(vertexInput),
          .count = 1u,
          .payloadOffset = static_cast<std::uint32_t>(vertexInputOffset),
          .byteSize = sizeof(vertexInput),
      },
      D9CCommandChunkWireSectionDesc{
          .kind = D9C_COMMAND_CHUNK_SECTION_INDEX_BUFFER,
          .elementSize = sizeof(index),
          .count = 1u,
          .payloadOffset = static_cast<std::uint32_t>(indexOffset),
          .byteSize = sizeof(index),
      },
      D9CCommandChunkWireSectionDesc{
          .kind = D9C_COMMAND_CHUNK_SECTION_RENDER_TARGET,
          .elementSize = sizeof(renderTargets[0]),
          .count = static_cast<std::uint32_t>(renderTargets.size()),
          .payloadOffset = static_cast<std::uint32_t>(renderTargetOffset),
          .byteSize = sizeof(renderTargets),
      },
  };
  std::vector<std::byte> payload(renderTargetOffset + sizeof(renderTargets));
  std::memcpy(payload.data(), &draw, sizeof(draw));
  std::memcpy(payload.data() + sectionTableOffset, sections.data(),
              sizeof(sections));
  std::memcpy(payload.data() + textureOffset, textures.data(), sizeof(textures));
  std::memcpy(payload.data() + streamOffset, streams.data(), sizeof(streams));
  std::memcpy(payload.data() + shaderOffset, shaders.data(), sizeof(shaders));
  std::memcpy(payload.data() + vertexInputOffset, &vertexInput,
              sizeof(vertexInput));
  std::memcpy(payload.data() + indexOffset, &index, sizeof(index));
  std::memcpy(payload.data() + renderTargetOffset, renderTargets.data(),
              sizeof(renderTargets));

  const std::array handles{
      D9CCommandChunkWireHandleEntry{
          .kind = kVertexBuffer.kind,
          .generation = kVertexBuffer.generation,
          .objectId = kVertexBuffer.objectId,
      },
      D9CCommandChunkWireHandleEntry{
          .kind = kVertexShader.kind,
          .generation = kVertexShader.generation,
          .objectId = kVertexShader.objectId,
      },
      D9CCommandChunkWireHandleEntry{
          .kind = kVertexDeclaration.kind,
          .generation = kVertexDeclaration.generation,
          .objectId = kVertexDeclaration.objectId,
      },
      D9CCommandChunkWireHandleEntry{
          .kind = kIndexBuffer.kind,
          .generation = kIndexBuffer.generation,
          .objectId = kIndexBuffer.objectId,
      },
      D9CCommandChunkWireHandleEntry{
          .kind = kOutput.kind,
          .generation = kOutput.generation,
          .objectId = kOutput.objectId,
      },
  };
  return singleRecordChunk(D9C_COMMAND_RECORD_APPLY_STATE, payload, handles);
}

inline std::vector<std::byte> indexedDrawChunk() {
  // Carry the draw's resource closure in the command source, not only in the
  // earlier bootstrap, so production capture can pin both buffer backings.
  auto bytes = bootstrapChunk();
  D9CCommandChunkWireHeader header{};
  std::memcpy(&header, bytes.data(), sizeof(header));
  D9CCommandChunkWireRecordHeader record{};
  std::memcpy(&record, bytes.data() + header.recordTableOffset, sizeof(record));
  record.type = D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE;
  std::memcpy(bytes.data() + header.recordTableOffset, &record, sizeof(record));
  D9CCommandChunkWireDrawHeader draw{};
  auto* payload = bytes.data() + header.payloadArenaOffset + record.payloadOffset;
  std::memcpy(&draw, payload, sizeof(draw));
  draw.primitiveType = 4u;
  draw.numVertices = 3u;
  draw.primitiveCount = 1u;
  std::memcpy(payload, &draw, sizeof(draw));
  return bytes;
}

inline std::vector<std::byte> presentChunk() {
  const D9CCommandChunkWirePresent present{
      .sourceHandleIndex = 0u,
  };
  const D9CCommandChunkWireHandleEntry output{
      .kind = kOutput.kind,
      .generation = kOutput.generation,
      .objectId = kOutput.objectId,
  };
  return singleRecordChunk(D9C_COMMAND_RECORD_PRESENT,
                           std::as_bytes(std::span(&present, 1u)),
                           std::span<const D9CCommandChunkWireHandleEntry>(
                               &output, 1u));
}

}  // namespace general_cpu_destroy_fixture_detail

inline GeneralCpuDestroyFixture makeGeneralCpuDestroyFixture(
    bool dynamicVertex = true, bool dynamicIndex = true,
    GeneralCpuMutationFixture mutation = GeneralCpuMutationFixture::None,
    GeneralCpuResetFixture reset = GeneralCpuResetFixture::A,
    std::uint32_t extraDrawSources = 0u, bool clearBeforeExtraDraws = false) {
  using namespace general_cpu_destroy_fixture_detail;

  struct TexturedVertex {
    float x;
    float y;
    float z;
    float rhw;
    std::uint32_t diffuse;
    float u;
    float v;
  };
  static_assert(sizeof(TexturedVertex) == 28u);

  const std::array vertices{
      TexturedVertex{-0.5f, -0.5f, 0.0f, 1.0f, 0xffff0000u, 0.0f, 0.0f},
      TexturedVertex{0.5f, -0.5f, 0.0f, 1.0f, 0xff00ff00u, 1.0f, 0.0f},
      TexturedVertex{0.0f, 0.5f, 0.0f, 1.0f, 0xff0000ffu, 0.5f, 1.0f},
  };
  auto vertexBytes = bytesOf(vertices);
  if (reset == GeneralCpuResetFixture::B) {
    for (std::size_t index = 0u; index < vertexBytes.size(); ++index)
      vertexBytes[index] ^= static_cast<std::byte>(0x5au + (index & 0x1fu));
  }
  const bool unrelatedMutation =
      mutation == GeneralCpuMutationFixture::UnrelatedPlain;
  const bool referencedMutation =
      mutation == GeneralCpuMutationFixture::ReferencedPlain ||
      mutation == GeneralCpuMutationFixture::ReferencedDiscard;
  std::vector<std::byte> unrelatedBytes;
  std::vector<std::byte> changedUnrelatedBytes;
  if (unrelatedMutation) {
    unrelatedBytes.resize(vertexBytes.size());
    changedUnrelatedBytes.resize(vertexBytes.size());
    for (std::size_t index = 0u; index < vertexBytes.size(); ++index) {
      unrelatedBytes[index] = static_cast<std::byte>((0x5au + index) & 0xffu);
      changedUnrelatedBytes[index] =
          static_cast<std::byte>((0xa5u + index) & 0xffu);
    }
  }
  auto changedVertexBytes = vertexBytes;
  if (referencedMutation) {
    changedVertexBytes[0] = static_cast<std::byte>(
        std::to_integer<unsigned int>(changedVertexBytes[0]) ^ 0xffu);
  }
  const auto unrelatedDigest =
      RenderTapeCaptureSession::sha256(unrelatedBytes);
  const auto changedUnrelatedDigest =
      RenderTapeCaptureSession::sha256(changedUnrelatedBytes);
  const auto changedVertexDigest =
      RenderTapeCaptureSession::sha256(changedVertexBytes);
  const std::array<std::uint16_t, 3u> indices{0u, 1u, 2u};
  auto indexBytes = bytesOf(indices);
  const std::array<std::uint32_t, 8u> shaderWords{
      0xfffe0300u, 0x0200001fu, 0x80000000u, 0xc00f0000u,
      0x02000001u, 0xc00f0000u, 0xa0e40000u, 0x0000ffffu,
  };
  auto shaderBytes = bytesOf(shaderWords);
  const std::array declaration{
      D9CVertexElement{0u, 0u, 3u, 0u, 9u, 0u},
      D9CVertexElement{0u, 16u, 4u, 0u, 10u, 0u},
      D9CVertexElement{0u, 20u, 1u, 0u, 5u, 0u},
      D9CVertexElement{0xffu, 0u, 17u, 0u, 0u, 0u},
  };
  auto declarationBytes = bytesOf(declaration);

  const auto vertexDigest = RenderTapeCaptureSession::sha256(vertexBytes);
  const auto indexDigest = RenderTapeCaptureSession::sha256(indexBytes);
  const auto shaderDigest = RenderTapeCaptureSession::sha256(shaderBytes);
  const auto declarationDigest =
      RenderTapeCaptureSession::sha256(declarationBytes);

  const D9CSurfaceDesc outputSurface{
      .format = 22u,
      .resourceType = 1u,
      .usage = 1u,
      .pool = 0u,
      .width = 16u,
      .height = 16u,
      .depth = 1u,
  };
  const RenderTapeSurfaceDescriptorV2 outputDescriptor{
      .schemaVersion = kRenderTapeSurfaceDescriptorVersion2,
      .storage = static_cast<std::uint32_t>(
          RenderTapeSurfaceStorage::SwapchainBackbuffer),
      .initialContentDisposition = static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::ProducedPresentOutput),
      .surface = outputSurface,
  };
  const D9CBufferDesc vertexBuffer{
      .size = static_cast<std::uint32_t>(vertexBytes.size()),
      .usage = dynamicVertex ? 0x200u : 0u,
  };
  const D9CBufferDesc indexBuffer{
      .size = static_cast<std::uint32_t>(indexBytes.size()),
      .usage = dynamicIndex ? 0x200u : 0u,
      .format = 101u,
  };
  const D9CBufferDesc unrelatedBuffer{
      .size = static_cast<std::uint32_t>(unrelatedBytes.size()),
      .usage = 0u,
  };
  const RenderTapeShaderDescriptor shader{
      .stage = D9C_COMMAND_CHUNK_SHADER_STAGE_VERTEX,
      .bytecodeBytes = static_cast<std::uint32_t>(shaderBytes.size()),
  };
  const RenderTapeVertexDeclDescriptor vertexDeclaration{
      .elementCount = 4u,
      .elementBytes = static_cast<std::uint32_t>(declarationBytes.size()),
  };
  const RenderTapeOracleAttachment oracle{
      .identity = kOutput,
      .descriptorKind = static_cast<std::uint32_t>(
          RenderTapeDescriptorKind::Surface),
  };

  RenderTapeBuilder builder;
  builder.appendBootstrapState(bootstrapChunk());
  if (unrelatedMutation) {
    builder.appendObjectDefine(
        kUnrelatedBuffer,
        static_cast<std::uint32_t>(RenderTapeDescriptorKind::Buffer),
        std::as_bytes(std::span(&unrelatedBuffer, 1u)), 0u, {},
        unrelatedBytes.size(), 1u);
  }
  builder.appendObjectDefine(
      kOutput, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
      std::as_bytes(std::span(&outputDescriptor, 1u)), 0u, {});
  builder.appendObjectDefine(
      kVertexBuffer, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Buffer),
      std::as_bytes(std::span(&vertexBuffer, 1u)), 0u, {}, vertexBytes.size(),
      1u);
  builder.appendObjectDefine(
      kIndexBuffer, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Buffer),
      std::as_bytes(std::span(&indexBuffer, 1u)), 0u, {}, indexBytes.size(),
      1u);
  builder.appendObjectDefine(
      kVertexShader, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Shader),
      std::as_bytes(std::span(&shader, 1u)), shaderBytes.size(), shaderDigest);
  builder.appendObjectDefine(
      kVertexDeclaration,
      static_cast<std::uint32_t>(RenderTapeDescriptorKind::VertexDeclaration),
      std::as_bytes(std::span(&vertexDeclaration, 1u)), declarationBytes.size(),
      declarationDigest);
  if (unrelatedMutation) {
    builder.appendResourceMutation(
        kUnrelatedBuffer, RenderTapeMutationKind::Upload, 0u, 0u,
        unrelatedBytes.size(), unrelatedDigest,
        RenderTapeBufferMutationDisposition::Plain);
  }
  builder.appendResourceMutation(
      kVertexBuffer, RenderTapeMutationKind::Upload, 0u, 0u,
      vertexBytes.size(), vertexDigest);
  builder.appendResourceMutation(
      kIndexBuffer, RenderTapeMutationKind::Upload, 0u, 0u,
      indexBytes.size(), indexDigest);
  builder.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 1u, .handleCount = 5u}, indexedDrawChunk());
  if (clearBeforeExtraDraws) {
    const D9CCommandChunkWireClear clear{
        .flags = 1u, .colorARGB = 0xff000000u, .z = 1.0f,
        .rectOffset = sizeof(D9CCommandChunkWireClear)};
    builder.appendCommandChunk(CommandChunkEnvelope{.recordCount = 1u},
        general_cpu_destroy_fixture_detail::singleRecordChunk(
            D9C_COMMAND_RECORD_CLEAR, std::as_bytes(std::span(&clear, 1u))));
  }
  for (std::uint32_t source = 0u; source < extraDrawSources; ++source) {
    builder.appendCommandChunk(
        CommandChunkEnvelope{.recordCount = 1u, .handleCount = 5u}, indexedDrawChunk());
  }
  if (mutation != GeneralCpuMutationFixture::None) {
    const auto target = unrelatedMutation ? kUnrelatedBuffer : kVertexBuffer;
    const auto& bytes = unrelatedMutation ? changedUnrelatedBytes
                                           : changedVertexBytes;
    const auto& digest = unrelatedMutation ? changedUnrelatedDigest
                                            : changedVertexDigest;
    const auto kind = mutation == GeneralCpuMutationFixture::ReferencedDiscard
        ? RenderTapeMutationKind::CpuUnlock
        : RenderTapeMutationKind::Upload;
    const auto disposition = mutation == GeneralCpuMutationFixture::ReferencedDiscard
        ? RenderTapeBufferMutationDisposition::Discard
        : RenderTapeBufferMutationDisposition::Plain;
    builder.appendResourceMutation(target, kind, 0u, 0u, bytes.size(), digest,
                                   disposition);
    builder.appendCommandChunk(
        CommandChunkEnvelope{.recordCount = 1u, .handleCount = 5u},
        indexedDrawChunk());
  }
  if (unrelatedMutation) builder.appendObjectDestroy(kUnrelatedBuffer);
  builder.appendObjectDestroy(kVertexBuffer);
  builder.appendObjectDestroy(kIndexBuffer);
  builder.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 1u, .handleCount = 1u},
      general_cpu_destroy_fixture_detail::presentChunk());
  builder.appendPresentComplete(
      12u + extraDrawSources + std::uint32_t(clearBeforeExtraDraws) + (unrelatedMutation ? 5u :
             (mutation == GeneralCpuMutationFixture::None ? 0u : 2u)),
      1u, RenderTapeDigestValidity::NotCaptured, {},
      std::as_bytes(std::span(&oracle, 1u)));

  std::vector<std::vector<std::byte>> blobs{
      std::move(vertexBytes), std::move(indexBytes), std::move(shaderBytes),
      std::move(declarationBytes)};
  if (unrelatedMutation) {
    blobs.push_back(std::move(unrelatedBytes));
    blobs.push_back(std::move(changedUnrelatedBytes));
  } else if (referencedMutation) {
    blobs.push_back(std::move(changedVertexBytes));
  }
  return GeneralCpuDestroyFixture{
      .tape = builder.seal(),
      .blobs = std::move(blobs),
  };
}

}  // namespace dxmt9::d3d9::test
