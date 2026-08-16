#include "device_c_render_tape_capture.hpp"
#include "device_c_render_tape_capture_layout.hpp"
#include "device_c_render_tape_provider.hpp"
#include "dxmt9/dxmt9_device.hpp"
#include "dxmt9/dxmt9_presenter.hpp"
#include "dxmt9/dxmt9_resource_pool.hpp"

// R-HARN-REPLAY-7.6/7.7/7.8: bounded provider identity grammar,
// pre-effect failure, independent evidence, and native Metal output readback.

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
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
  if (!condition) throw TestFailure(std::string(message));
}

std::size_t alignUp(std::size_t value, std::size_t alignment) {
  return (value + alignment - 1u) & ~(alignment - 1u);
}

struct Record {
  std::uint32_t type = 0u;
  std::vector<std::byte> payload{};
  std::vector<D9CCommandChunkWireHandleEntry> handles{};
};

template <typename T>
std::vector<std::byte> bytesOf(const T& value) {
  std::vector<std::byte> bytes(sizeof(value));
  std::memcpy(bytes.data(), &value, sizeof(value));
  return bytes;
}

template <typename T, std::size_t N>
std::span<const std::byte> spanBytes(const std::array<T, N>& values) {
  return {reinterpret_cast<const std::byte*>(values.data()), sizeof(values)};
}

template <typename T>
std::span<const std::byte> spanBytes(const T& value) {
  return {reinterpret_cast<const std::byte*>(&value), sizeof(value)};
}

RenderTapeSurfaceDescriptorV2 outputDescriptor(
    const D9CSurfaceDesc& surface) {
  return RenderTapeSurfaceDescriptorV2{
      .schemaVersion = kRenderTapeSurfaceDescriptorVersion2,
      .storage = static_cast<std::uint32_t>(
          RenderTapeSurfaceStorage::SwapchainBackbuffer),
      .initialContentDisposition = static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::ProducedPresentOutput),
      .surface = surface,
  };
}

std::vector<std::byte> texture2DDescriptor(
    const D9CSurfaceDesc& level0) {
  const RenderTapeTextureDescriptorV2 header{
      .schemaVersion = kRenderTapeTextureDescriptorVersion2,
      .dimension = static_cast<std::uint32_t>(
          RenderTapeTextureDimension::Texture2D),
      .mipLevelCount = 1u,
      .subresourceCount = 1u,
      .initialContentDisposition = static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::CompleteSeed),
  };
  std::vector<std::byte> bytes(sizeof(header) + sizeof(level0));
  std::memcpy(bytes.data(), &header, sizeof(header));
  std::memcpy(bytes.data() + sizeof(header), &level0, sizeof(level0));
  return bytes;
}

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
    check(rule != nullptr, "fixture record must be canonical");
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
  std::memcpy(bytes.data() + recordTableOffset, records.data(),
              records.size() * sizeof(records[0]));
  if (!handles.empty()) {
    std::memcpy(bytes.data() + handleTableOffset, handles.data(),
                handles.size() * sizeof(handles[0]));
  }
  std::memcpy(bytes.data() + payloadArenaOffset, payload.data(), payload.size());
  return bytes;
}

std::vector<std::byte> bootstrapChunkWithRenderTargets(
    std::span<const D9CCommandChunkWireRenderTargetBinding> renderTargets) {
  std::array<D9CCommandChunkWireTextureBinding, D9C_DRAW_PACKET_MAX_TEXTURES>
      textures{};
  for (std::uint32_t slot = 0u; slot < textures.size(); ++slot) {
    textures[slot] = {.slot = slot, .valid = 1u,
                      .handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX};
  }
  std::array<D9CCommandChunkWireStreamBinding, D9C_DRAW_PACKET_MAX_STREAMS>
      streams{};
  for (std::uint32_t slot = 0u; slot < streams.size(); ++slot) {
    streams[slot] = {.slot = slot, .valid = 1u,
                     .handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX};
  }
  constexpr std::uint32_t sectionCount = 3u;
  const auto sectionTableOffset = sizeof(D9CCommandChunkWireDrawHeader);
  const auto sectionPayloadOffset = alignUp(
      sectionTableOffset + sectionCount * sizeof(D9CCommandChunkWireSectionDesc),
      alignof(std::uint32_t));
  const auto streamOffset = sectionPayloadOffset + sizeof(textures);
  const auto renderTargetOffset = streamOffset + sizeof(streams);
  const auto renderTargetBytes = renderTargets.size() *
                                 sizeof(D9CCommandChunkWireRenderTargetBinding);
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
          .kind = D9C_COMMAND_CHUNK_SECTION_RENDER_TARGET,
          .elementSize = sizeof(D9CCommandChunkWireRenderTargetBinding),
          .count = static_cast<std::uint32_t>(renderTargets.size()),
          .payloadOffset = static_cast<std::uint32_t>(renderTargetOffset),
          .byteSize = static_cast<std::uint32_t>(renderTargetBytes),
      },
  };
  std::vector<std::byte> payload(renderTargetOffset + renderTargetBytes);
  std::memcpy(payload.data(), &draw, sizeof(draw));
  std::memcpy(payload.data() + sectionTableOffset, sections.data(), sizeof(sections));
  std::memcpy(payload.data() + sectionPayloadOffset, textures.data(), sizeof(textures));
  std::memcpy(payload.data() + streamOffset, streams.data(), sizeof(streams));
  if (!renderTargets.empty()) {
    std::memcpy(payload.data() + renderTargetOffset, renderTargets.data(),
                renderTargetBytes);
  }
  std::vector<D9CCommandChunkWireHandleEntry> handles(renderTargets.size(),
      D9CCommandChunkWireHandleEntry{
          .kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
          .generation = 7u,
          .objectId = 41u,
      });
  const std::array records{Record{
      .type = D9C_COMMAND_RECORD_APPLY_STATE,
      .payload = std::move(payload),
      .handles = std::move(handles),
  }};
  return makeChunk(records);
}

std::vector<std::byte> bootstrapChunk() {
  const std::array renderTargets{D9CCommandChunkWireRenderTargetBinding{
      .slot = 0u,
      .valid = 1u,
      .handleIndex = 0u,
  }};
  return bootstrapChunkWithRenderTargets(renderTargets);
}

std::vector<std::byte> implicitBootstrapChunk() {
  std::array<D9CCommandChunkWireTextureBinding, D9C_DRAW_PACKET_MAX_TEXTURES>
      textures{};
  for (std::uint32_t slot = 0u; slot < textures.size(); ++slot) {
    textures[slot] = {.slot = slot, .valid = 1u,
                      .handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX};
  }
  std::array<D9CCommandChunkWireStreamBinding, D9C_DRAW_PACKET_MAX_STREAMS>
      streams{};
  for (std::uint32_t slot = 0u; slot < streams.size(); ++slot) {
    streams[slot] = {.slot = slot, .valid = 1u,
                     .handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX};
  }
  constexpr std::uint32_t sectionCount = 2u;
  const auto sectionTableOffset = sizeof(D9CCommandChunkWireDrawHeader);
  const auto sectionPayloadOffset = alignUp(
      sectionTableOffset + sectionCount * sizeof(D9CCommandChunkWireSectionDesc),
      alignof(std::uint32_t));
  const auto streamOffset = sectionPayloadOffset + sizeof(textures);
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
  };
  std::vector<std::byte> payload(streamOffset + sizeof(streams));
  std::memcpy(payload.data(), &draw, sizeof(draw));
  std::memcpy(payload.data() + sectionTableOffset, sections.data(),
              sizeof(sections));
  std::memcpy(payload.data() + sectionPayloadOffset, textures.data(),
              sizeof(textures));
  std::memcpy(payload.data() + streamOffset, streams.data(), sizeof(streams));
  return makeChunk(std::array{Record{
      .type = D9C_COMMAND_RECORD_APPLY_STATE,
      .payload = std::move(payload),
  }});
}

constexpr D9CWireObjectIdentity kOutput{
    .kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
    .generation = 7u,
    .objectId = 41u,
};
constexpr D9CWireObjectIdentity kSeedBuffer{
    .kind = D9C_CHUNK_HANDLE_KIND_BUFFER,
    .generation = 3u,
    .objectId = 42u,
};
constexpr D9CWireObjectIdentity kTexture{
    .kind = D9C_CHUNK_HANDLE_KIND_TEXTURE,
    .generation = 9u,
    .objectId = 43u,
};
constexpr D9CWireObjectIdentity kTexturedVertexDeclaration{
    .kind = D9C_CHUNK_HANDLE_KIND_VERTEX_DECL,
    .generation = 2u,
    .objectId = 44u,
};
constexpr D9CWireObjectIdentity kSnapshotDepth{
    .kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
    .generation = 5u,
    .objectId = 45u,
};
constexpr D9CWireObjectIdentity kSnapshotTexture2D{
    .kind = D9C_CHUNK_HANDLE_KIND_TEXTURE,
    .generation = 6u,
    .objectId = 46u,
};
constexpr D9CWireObjectIdentity kSnapshotCube{
    .kind = D9C_CHUNK_HANDLE_KIND_TEXTURE,
    .generation = 8u,
    .objectId = 47u,
};
constexpr D9CWireObjectIdentity kSnapshotColorSurface{
    .kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
    .generation = 3u,
    .objectId = 48u,
};
constexpr D9CWireObjectIdentity kGeneralVertexBuffer{
    .kind = D9C_CHUNK_HANDLE_KIND_BUFFER,
    .generation = 4u,
    .objectId = 49u,
};
constexpr D9CWireObjectIdentity kGeneralIndexBuffer{
    .kind = D9C_CHUNK_HANDLE_KIND_BUFFER,
    .generation = 4u,
    .objectId = 50u,
};
constexpr D9CWireObjectIdentity kGeneralVertexShader{
    .kind = D9C_CHUNK_HANDLE_KIND_SHADER,
    .generation = 4u,
    .objectId = 51u,
};
constexpr D9CWireObjectIdentity kGeneralVertexDeclaration{
    .kind = D9C_CHUNK_HANDLE_KIND_VERTEX_DECL,
    .generation = 4u,
    .objectId = 52u,
};

constexpr RenderTapeDigest kGeneralExpectedDigest{
    std::byte{0x44}, std::byte{0x3b}, std::byte{0x93}, std::byte{0x18},
    std::byte{0xda}, std::byte{0x95}, std::byte{0x54}, std::byte{0xcf},
    std::byte{0x19}, std::byte{0x26}, std::byte{0xc3}, std::byte{0x01},
    std::byte{0x43}, std::byte{0xed}, std::byte{0x63}, std::byte{0xb4},
    std::byte{0xac}, std::byte{0x1b}, std::byte{0x61}, std::byte{0xfe},
    std::byte{0x83}, std::byte{0x3c}, std::byte{0x89}, std::byte{0xa7},
    std::byte{0xd7}, std::byte{0x50}, std::byte{0xda}, std::byte{0x2e},
    std::byte{0xb7}, std::byte{0xf3}, std::byte{0xf9}, std::byte{0x9c},
};

std::vector<std::byte> sparsePayload(
    D9CCommandChunkWireDrawHeader draw,
    std::span<const std::pair<std::uint16_t, std::span<const std::byte>>> inputs) {
  draw.sectionCount = static_cast<std::uint32_t>(inputs.size());
  draw.sectionTableOffset = sizeof(draw);
  draw.sectionPayloadOffset = static_cast<std::uint32_t>(alignUp(
      sizeof(draw) + inputs.size() * sizeof(D9CCommandChunkWireSectionDesc),
      alignof(std::uint32_t)));
  std::vector<D9CCommandChunkWireSectionDesc> sections;
  std::vector<std::byte> payload(draw.sectionPayloadOffset);
  for (const auto& [kind, bytes] : inputs) {
    const auto* rule = sectionRule(kind);
    check(rule != nullptr, "textured fixture section must be canonical");
    payload.resize(alignUp(payload.size(), rule->payloadAlignment));
    sections.push_back(D9CCommandChunkWireSectionDesc{
        .kind = kind,
        .elementSize = rule->elementSize,
        .count = static_cast<std::uint32_t>(bytes.size() / rule->elementSize),
        .payloadOffset = static_cast<std::uint32_t>(payload.size()),
        .byteSize = static_cast<std::uint32_t>(bytes.size()),
    });
    payload.insert(payload.end(), bytes.begin(), bytes.end());
  }
  std::memcpy(payload.data(), &draw, sizeof(draw));
  std::memcpy(payload.data() + draw.sectionTableOffset, sections.data(),
              sections.size() * sizeof(sections[0]));
  return payload;
}

std::vector<std::byte> texturedBootstrapChunk(
    bool productionVertexDeclaration = false) {
  std::array<D9CCommandChunkWireTextureBinding, D9C_DRAW_PACKET_MAX_TEXTURES>
      textures{};
  for (std::uint32_t slot = 0u; slot < textures.size(); ++slot) {
    textures[slot] = {
        .slot = slot,
        .valid = 1u,
        .handleIndex = slot == 0u ? 0u : D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX,
    };
  }
  std::array<D9CCommandChunkWireStreamBinding, D9C_DRAW_PACKET_MAX_STREAMS>
      streams{};
  for (std::uint32_t slot = 0u; slot < streams.size(); ++slot) {
    streams[slot] = {.slot = slot, .valid = 1u,
                     .handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX};
  }
  const D9CCommandChunkWireVertexInput input{
      .valid = 1u,
      .kind = productionVertexDeclaration
          ? D9C_COMMAND_CHUNK_VERTEX_INPUT_DECLARATION
          : D9C_COMMAND_CHUNK_VERTEX_INPUT_FVF,
      .value = 0x144u,
      .handleIndex = productionVertexDeclaration
          ? 1u
          : D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX,
  };
  const std::array sections{
      std::pair{static_cast<std::uint16_t>(D9C_COMMAND_CHUNK_SECTION_TEXTURE),
                spanBytes(textures)},
      std::pair{static_cast<std::uint16_t>(D9C_COMMAND_CHUNK_SECTION_STREAM),
                spanBytes(streams)},
      std::pair{static_cast<std::uint16_t>(D9C_COMMAND_CHUNK_SECTION_VERTEX_INPUT),
                spanBytes(input)},
  };
  const auto payload = sparsePayload(
      D9CCommandChunkWireDrawHeader{
          .flags = D9C_COMMAND_CHUNK_DRAW_FLAG_FULL_SNAPSHOT},
      sections);
  return makeChunk(std::array{Record{
      .type = D9C_COMMAND_RECORD_APPLY_STATE,
      .payload = payload,
      .handles = productionVertexDeclaration
          ? std::vector<D9CCommandChunkWireHandleEntry>{
                {.kind = kTexture.kind,
                 .generation = kTexture.generation,
                 .objectId = kTexture.objectId},
                {.kind = kTexturedVertexDeclaration.kind,
                 .generation = kTexturedVertexDeclaration.generation,
                 .objectId = kTexturedVertexDeclaration.objectId},
            }
          : std::vector<D9CCommandChunkWireHandleEntry>{
                {.kind = kTexture.kind,
                 .generation = kTexture.generation,
                 .objectId = kTexture.objectId},
            },
  }});
}

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

std::vector<std::byte> generalBootstrapChunk(
    const D9CWireObjectIdentity& vertexBuffer,
    const D9CWireObjectIdentity& indexBuffer,
    const D9CWireObjectIdentity& vertexShader,
    const D9CWireObjectIdentity& vertexDeclaration,
    const D9CWireObjectIdentity& output) {
  std::array<D9CCommandChunkWireTextureBinding, D9C_DRAW_PACKET_MAX_TEXTURES>
      textures{};
  for (std::uint32_t slot = 0u; slot < textures.size(); ++slot) {
    textures[slot] = {.slot = slot,
                      .valid = 1u,
                      .handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX};
  }
  std::array<D9CCommandChunkWireStreamBinding, D9C_DRAW_PACKET_MAX_STREAMS>
      streams{};
  for (std::uint32_t slot = 0u; slot < streams.size(); ++slot) {
    streams[slot] = {
        .slot = slot,
        .valid = 1u,
        .handleIndex = slot == 0u ? 0u : D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX,
    };
  }
  const std::array shaders{
      D9CCommandChunkWireShaderBinding{
          .stage = D9C_COMMAND_CHUNK_SHADER_STAGE_VERTEX,
          .valid = 1u,
          .handleIndex = 1u,
      },
  };
  const D9CCommandChunkWireVertexInput input{
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
  const std::array sections{
      std::pair{static_cast<std::uint16_t>(D9C_COMMAND_CHUNK_SECTION_TEXTURE),
                spanBytes(textures)},
      std::pair{static_cast<std::uint16_t>(D9C_COMMAND_CHUNK_SECTION_STREAM),
                spanBytes(streams)},
      std::pair{static_cast<std::uint16_t>(D9C_COMMAND_CHUNK_SECTION_SHADER),
                spanBytes(shaders)},
      std::pair{static_cast<std::uint16_t>(D9C_COMMAND_CHUNK_SECTION_VERTEX_INPUT),
                spanBytes(input)},
      std::pair{static_cast<std::uint16_t>(D9C_COMMAND_CHUNK_SECTION_INDEX_BUFFER),
                spanBytes(index)},
      std::pair{static_cast<std::uint16_t>(D9C_COMMAND_CHUNK_SECTION_RENDER_TARGET),
                spanBytes(renderTargets)},
  };
  const auto payload = sparsePayload(
      D9CCommandChunkWireDrawHeader{
          .flags = D9C_COMMAND_CHUNK_DRAW_FLAG_FULL_SNAPSHOT},
      sections);
  return makeChunk(std::array{Record{
      .type = D9C_COMMAND_RECORD_APPLY_STATE,
      .payload = payload,
      .handles = {
          {vertexBuffer.kind, vertexBuffer.generation, vertexBuffer.objectId},
          {vertexShader.kind, vertexShader.generation, vertexShader.objectId},
          {vertexDeclaration.kind, vertexDeclaration.generation,
           vertexDeclaration.objectId},
          {indexBuffer.kind, indexBuffer.generation, indexBuffer.objectId},
          {output.kind, output.generation, output.objectId},
      },
  }});
}

std::vector<std::byte> generalIndexedDrawChunk() {
  const D9CCommandChunkWireDrawHeader draw{
      .primitiveType = 4u,
      .minVertex = 0u,
      .numVertices = 3u,
      .startIndex = 0u,
      .primitiveCount = 1u,
      .sectionTableOffset = sizeof(D9CCommandChunkWireDrawHeader),
      .sectionPayloadOffset = sizeof(D9CCommandChunkWireDrawHeader),
  };
  return makeChunk(std::array{Record{
      .type = D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE,
      .payload = bytesOf(draw),
  }});
}

// Keep the production-parallel fixture in one canonical command chunk.  The
// first record is the same FULL_SNAPSHOT state anchor used by the production
// indexed path; the remainder is two indivisible indexed DrawRuns.  64 A
// draws use the baseline PSO/uniform identity.  64 B draws use a color-write
// topology/render-state transition and a pixel-inert VS constant range: the
// production economics identities change while the degenerate pixels do not.
std::vector<std::byte> indexedDrawPayload(std::uint32_t primitiveType,
                                          bool alternateUniform) {
  const D9CCommandChunkWireDrawHeader draw{
      .primitiveType = primitiveType,
      .minVertex = 0u,
      .numVertices = 3u,
      .startIndex = 0u,
      .primitiveCount = 1u,
      .sectionTableOffset = sizeof(D9CCommandChunkWireDrawHeader),
      .sectionPayloadOffset = sizeof(D9CCommandChunkWireDrawHeader),
  };
  if (!alternateUniform) return bytesOf(draw);
  const D9CCommandChunkWireConstantRange range{
      .startRegister = 0u,
      .registerCount = 1u,
  };
  // The fixture shader moves c0 to oPos. Keeping w=0 makes every B draw
  // clipped/degenerate like A while changing the production uniform identity.
  const std::array<float, 4u> values{1.0f, 1.0f, 1.0f, 0.0f};
  std::vector<std::byte> constants(sizeof(range) + sizeof(values));
  std::memcpy(constants.data(), &range, sizeof(range));
  std::memcpy(constants.data() + sizeof(range), values.data(), sizeof(values));
  const auto renderState = bytesOf(D9CCommandChunkWireRenderState{
      .state = 168u, // D3DRS_COLORWRITEENABLE
      .value = 0u,
  });
  const std::array sections{
      std::pair{static_cast<std::uint16_t>(D9C_COMMAND_CHUNK_SECTION_RENDER_STATE),
                std::span<const std::byte>(renderState)},
      std::pair{static_cast<std::uint16_t>(D9C_COMMAND_CHUNK_SECTION_VS_CONST_F),
                std::span<const std::byte>(constants)},
  };
  return sparsePayload(draw, sections);
}

std::vector<std::byte> generalIndexedDrawRunChunk(std::uint32_t drawCount) {
  check(drawCount >= 128u, "parallel fixture must retain two 64-draw children");
  const auto bootstrap = generalBootstrapChunk(
      kGeneralVertexBuffer, kGeneralIndexBuffer, kGeneralVertexShader,
      kGeneralVertexDeclaration, kOutput);
  ImportedChunkView bootstrapView;
  check(importPrevalidatedCommandChunk(
            bootstrap,
            CommandChunkEnvelope{.recordCount = 1u, .handleCount = 5u},
            bootstrapView),
        "parallel fixture imports its FULL_SNAPSHOT anchor");
  const auto anchor = bootstrapView.record(0u);
  std::vector<Record> records;
  records.reserve(static_cast<std::size_t>(drawCount) + 1u);
  records.push_back(Record{
      .type = anchor.header.type,
      .payload = std::vector<std::byte>(anchor.payload.begin(),
                                        anchor.payload.end()),
      .handles = std::vector<D9CCommandChunkWireHandleEntry>(
          bootstrapView.handles.begin(), bootstrapView.handles.end()),
  });
  for (std::uint32_t index = 0u; index < drawCount; ++index) {
    records.push_back(Record{
        .type = D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE,
        .payload = indexedDrawPayload(index < 64u ? 4u : 5u, index >= 64u),
    });
  }
  return makeChunk(records);
}

struct GeneralIndexedFixture {
  std::vector<std::byte> vertexBytes{};
  std::vector<std::byte> indexBytes{};
  std::vector<std::byte> shaderBytes{};
  std::vector<std::byte> declarationBytes{};
  std::vector<std::byte> outputSeed{};
  std::vector<RenderTapeProviderBlob> blobsValue{};
  std::vector<std::byte> tape{};

  explicit GeneralIndexedFixture(std::uint32_t drawCount = 1u,
                                 bool strictDigest = false) {
    const std::array vertices{
        TexturedVertex{-0.5f, -0.5f, 0.0f, 1.0f, 0xffff0000u, 0.0f, 0.0f},
        TexturedVertex{0.5f, -0.5f, 0.0f, 1.0f, 0xff00ff00u, 1.0f, 0.0f},
        TexturedVertex{0.0f, 0.5f, 0.0f, 1.0f, 0xff0000ffu, 0.5f, 1.0f},
    };
    vertexBytes.assign(reinterpret_cast<const std::byte*>(vertices.data()),
                       reinterpret_cast<const std::byte*>(vertices.data()) +
                           sizeof(vertices));
    const std::array<std::uint16_t, 3u> indices{0u, 1u, 2u};
    indexBytes.assign(reinterpret_cast<const std::byte*>(indices.data()),
                      reinterpret_cast<const std::byte*>(indices.data()) +
                          sizeof(indices));
    const std::array<std::uint32_t, 8u> shaderWords{
        0xfffe0300u, 0x0200001fu, 0x80000000u, 0xc00f0000u,
        0x02000001u, 0xc00f0000u, 0xa0e40000u, 0x0000ffffu,
    };
    shaderBytes.assign(reinterpret_cast<const std::byte*>(shaderWords.data()),
                       reinterpret_cast<const std::byte*>(shaderWords.data()) +
                           sizeof(shaderWords));
    const std::array declaration{
        D9CVertexElement{0u, 0u, 3u, 0u, 9u, 0u},
        D9CVertexElement{0u, 16u, 4u, 0u, 10u, 0u},
        D9CVertexElement{0u, 20u, 1u, 0u, 5u, 0u},
        D9CVertexElement{0xffu, 0u, 17u, 0u, 0u, 0u},
    };
    declarationBytes.assign(reinterpret_cast<const std::byte*>(declaration.data()),
                            reinterpret_cast<const std::byte*>(declaration.data()) +
                                sizeof(declaration));

    const auto vertexDigest = RenderTapeCaptureSession::sha256(vertexBytes);
    const auto indexDigest = RenderTapeCaptureSession::sha256(indexBytes);
    const auto shaderDigest = RenderTapeCaptureSession::sha256(shaderBytes);
    const auto declarationDigest =
        RenderTapeCaptureSession::sha256(declarationBytes);
    outputSeed.assign(16u * 16u * 4u, std::byte{0x3cu});
    const auto outputSeedDigest =
        RenderTapeCaptureSession::sha256(outputSeed);
    blobsValue = {
        {.digest = vertexDigest, .bytes = vertexBytes},
        {.digest = indexDigest, .bytes = indexBytes},
        {.digest = shaderDigest, .bytes = shaderBytes},
        {.digest = declarationDigest, .bytes = declarationBytes},
        {.digest = outputSeedDigest, .bytes = outputSeed},
    };

    const D9CSurfaceDesc outputSurface{
        .format = 22u,
        .resourceType = 1u,
        .usage = 1u,
        .pool = 0u,
        .width = 16u,
        .height = 16u,
        .depth = 1u,
    };
    auto output = outputDescriptor(outputSurface);
    output.initialContentDisposition = static_cast<std::uint32_t>(
        RenderTapeInitialContentDisposition::CompleteSeed);
    const D9CBufferDesc vertexBuffer{
        .size = static_cast<std::uint32_t>(vertexBytes.size()),
    };
    const D9CBufferDesc indexBuffer{
        .size = static_cast<std::uint32_t>(indexBytes.size()),
        .format = 101u,
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
    const auto clear = bytesOf(D9CCommandChunkWireClear{
        .flags = 1u,
        .colorARGB = 0xff102030u,
        .z = 1.0f,
        .rectOffset = sizeof(D9CCommandChunkWireClear),
    });
    const auto present = makeChunk(std::array{Record{
        .type = D9C_COMMAND_RECORD_PRESENT,
        .payload = bytesOf(D9CCommandChunkWirePresent{
            .sourceHandleIndex = 0u,
        }),
        .handles = {{kOutput.kind, kOutput.generation, kOutput.objectId}},
    }});
    RenderTapeBuilder builder;
    builder.appendBootstrapState(generalBootstrapChunk(
        kGeneralVertexBuffer, kGeneralIndexBuffer, kGeneralVertexShader,
        kGeneralVertexDeclaration, kOutput));
    builder.appendObjectDefine(
        kOutput, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
        std::as_bytes(std::span(&output, 1u)), 0u, {}, outputSeed.size(), 1u);
    builder.appendObjectDefine(
        kGeneralVertexBuffer,
        static_cast<std::uint32_t>(RenderTapeDescriptorKind::Buffer),
        std::as_bytes(std::span(&vertexBuffer, 1u)), 0u, {}, vertexBytes.size(),
        1u);
    builder.appendObjectDefine(
        kGeneralIndexBuffer,
        static_cast<std::uint32_t>(RenderTapeDescriptorKind::Buffer),
        std::as_bytes(std::span(&indexBuffer, 1u)), 0u, {}, indexBytes.size(),
        1u);
    builder.appendObjectDefine(
        kGeneralVertexShader,
        static_cast<std::uint32_t>(RenderTapeDescriptorKind::Shader),
        std::as_bytes(std::span(&shader, 1u)), shaderBytes.size(), shaderDigest);
    builder.appendObjectDefine(
        kGeneralVertexDeclaration,
        static_cast<std::uint32_t>(RenderTapeDescriptorKind::VertexDeclaration),
        std::as_bytes(std::span(&vertexDeclaration, 1u)),
        declarationBytes.size(), declarationDigest);
    builder.appendResourceMutation(
        kGeneralVertexBuffer, RenderTapeMutationKind::Upload, 0u, 0u,
        vertexBytes.size(), vertexDigest);
    builder.appendResourceMutation(
        kGeneralIndexBuffer, RenderTapeMutationKind::Upload, 0u, 0u,
        indexBytes.size(), indexDigest);
    builder.appendResourceMutation(
        kOutput, RenderTapeMutationKind::Upload, 0u, 0u,
        outputSeed.size(), outputSeedDigest);
    builder.appendCommandChunk(
        CommandChunkEnvelope{.recordCount = 1u}, makeChunk(std::array{Record{
            .type = D9C_COMMAND_RECORD_CLEAR,
            .payload = clear,
        }}));
    builder.appendCommandChunk(
        CommandChunkEnvelope{.recordCount = drawCount == 1u ? 1u
                                                             : drawCount + 1u,
                             .handleCount = drawCount == 1u ? 0u : 5u},
        drawCount == 1u ? generalIndexedDrawChunk()
                        : generalIndexedDrawRunChunk(drawCount));
    builder.appendCommandChunk(
        CommandChunkEnvelope{.recordCount = 1u, .handleCount = 1u}, present);
    builder.appendPresentComplete(
        12u, 1u,
        strictDigest ? RenderTapeDigestValidity::Sha256
                     : RenderTapeDigestValidity::NotCaptured,
        strictDigest ? kGeneralExpectedDigest : RenderTapeDigest{},
        std::as_bytes(std::span(&oracle, 1u)));
    tape = builder.seal();
  }
};

struct ParallelIndexedFixture : GeneralIndexedFixture {
  ParallelIndexedFixture() : GeneralIndexedFixture(128u, true) {}
};

std::vector<std::byte> texturedDrawChunk(std::uint16_t extraSection = 0u) {
  const std::array vertices{
      TexturedVertex{-0.5f, -0.5f, 0.0f, 1.0f, 0xffffffffu, 0.0f, 0.0f},
      TexturedVertex{31.5f, -0.5f, 0.0f, 1.0f, 0xffffffffu, 2.0f, 0.0f},
      TexturedVertex{-0.5f, 31.5f, 0.0f, 1.0f, 0xffffffffu, 0.0f, 2.0f},
  };
  const D9CCommandChunkWireStreamBinding stream{
      .slot = 0u, .valid = 1u,
      .handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX};
  const D9CCommandChunkWireShaderBinding shader{
      .stage = D9C_COMMAND_CHUNK_SHADER_STAGE_VERTEX, .valid = 0u,
      .handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX};
  const D9CCommandChunkWireVertexInput declaration{
      .valid = 0u, .kind = D9C_COMMAND_CHUNK_VERTEX_INPUT_DECLARATION,
      .handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX};
  const D9CCommandChunkWireIndexBinding index{
      .valid = 0u, .handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX};
  std::vector<std::pair<std::uint16_t, std::span<const std::byte>>> sections;
  if (extraSection == D9C_COMMAND_CHUNK_SECTION_STREAM)
    sections.emplace_back(extraSection, spanBytes(stream));
  if (extraSection == D9C_COMMAND_CHUNK_SECTION_SHADER)
    sections.emplace_back(extraSection, spanBytes(shader));
  if (extraSection == D9C_COMMAND_CHUNK_SECTION_VERTEX_INPUT)
    sections.emplace_back(extraSection, spanBytes(declaration));
  if (extraSection == D9C_COMMAND_CHUNK_SECTION_INDEX_BUFFER)
    sections.emplace_back(extraSection, spanBytes(index));
  sections.emplace_back(D9C_COMMAND_CHUNK_SECTION_UP_VERTEX_DATA,
                        spanBytes(vertices));
  const auto payload = sparsePayload(
      D9CCommandChunkWireDrawHeader{
          .primitiveType = 4u,
          .primitiveCount = 1u,
          .stride = sizeof(TexturedVertex),
      },
      sections);
  return makeChunk(std::array{Record{
      .type = D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP,
      .payload = payload,
  }});
}

struct ProductionFixture {
  std::vector<std::byte> tape{};
  std::vector<std::byte> depthSeed{};
  RenderTapeDigest depthSeedDigest{};

  explicit ProductionFixture(bool wrongExpectedDigest = false,
                             bool withDepthSeed = false) {
    const D9CCommandChunkWireClear clear{
        .flags = 1u,
        .colorARGB = 0xff204060u,
        .z = 1.0f,
        .rectCount = 0u,
        .rectOffset = sizeof(D9CCommandChunkWireClear),
    };
    const std::array frameRecords{
        Record{.type = D9C_COMMAND_RECORD_CLEAR, .payload = bytesOf(clear)},
        Record{.type = D9C_COMMAND_RECORD_PRESENT,
               .payload = bytesOf(D9CCommandChunkWirePresent{
                   .sourceHandleIndex = 0u,
               }),
               .handles = {{kOutput.kind, kOutput.generation,
                            kOutput.objectId}}},
    };
    const auto frame = makeChunk(frameRecords);
    const D9CSurfaceDesc outputDesc{
        .format = 21u,
        .resourceType = 1u,
        .usage = 1u,
        .pool = 0u,
        .multiSampleType = 0u,
        .multiSampleQuality = 0u,
        .width = 16u,
        .height = 16u,
        .depth = 1u,
    };
    const RenderTapeOracleAttachment oracle{
        .identity = kOutput,
        .descriptorKind = static_cast<std::uint32_t>(
            RenderTapeDescriptorKind::Surface),
    };
    const auto outputDescriptorV2 = outputDescriptor(outputDesc);
    const RenderTapeSurfaceDescriptorV2 depthDescriptor{
        .schemaVersion = kRenderTapeSurfaceDescriptorVersion2,
        .storage = static_cast<std::uint32_t>(
            RenderTapeSurfaceStorage::Standalone),
        .initialContentDisposition = static_cast<std::uint32_t>(
            RenderTapeInitialContentDisposition::CompleteDepthFloat32V1),
        .surface = D9CSurfaceDesc{
            .format = render_tape_d3d_format::D24X8,
            .resourceType = 1u,
            .usage = 2u,
            .pool = 0u,
            .multiSampleType = 0u,
            .multiSampleQuality = 0u,
            .width = 16u,
            .height = 16u,
            .depth = 1u,
        },
    };
    const RenderTapeDigest expectedDigest{
        std::byte{0x5f}, std::byte{0x73}, std::byte{0x22}, std::byte{0xd0},
        std::byte{0x5f}, std::byte{0x8b}, std::byte{0xa9}, std::byte{0x74},
        std::byte{0x08}, std::byte{0x93}, std::byte{0xa4}, std::byte{0x70},
        std::byte{0x42}, std::byte{0x3e}, std::byte{0x69}, std::byte{0x2f},
        std::byte{0x05}, std::byte{0x7c}, std::byte{0x05}, std::byte{0x4f},
        std::byte{0xd0}, std::byte{0xbd}, std::byte{0x92}, std::byte{0xaa},
        std::byte{0x60}, std::byte{0x6c}, std::byte{0x44}, std::byte{0x40},
        std::byte{0x10}, std::byte{0x9d}, std::byte{0xca}, std::byte{0x61},
    };
    auto expected = expectedDigest;
    if (wrongExpectedDigest) expected[0] ^= std::byte{1u};
    RenderTapeBuilder builder;
    builder.appendBootstrapState(implicitBootstrapChunk());
    builder.appendObjectDefine(
        kOutput, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
        std::as_bytes(std::span(&outputDescriptorV2, 1u)), 0u, {});
    if (withDepthSeed) {
      depthSeed.resize(16u * 16u * sizeof(float));
      const float depth = 0.375f;
      for (std::size_t offset = 0u; offset < depthSeed.size();
           offset += sizeof(depth)) {
        std::memcpy(depthSeed.data() + offset, &depth, sizeof(depth));
      }
      depthSeedDigest = RenderTapeCaptureSession::sha256(depthSeed);
      builder.appendObjectDefine(
          kSnapshotDepth,
          static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
          std::as_bytes(std::span(&depthDescriptor, 1u)), 0u, {},
          depthSeed.size(), 1u);
      builder.appendResourceMutation(
          kSnapshotDepth, RenderTapeMutationKind::Upload, 0u, 0u,
          depthSeed.size(), depthSeedDigest);
    }
    builder.appendCommandChunk(
        CommandChunkEnvelope{.recordCount = 2u, .handleCount = 1u}, frame);
    builder.appendPresentComplete(
        withDepthSeed ? 5u : 3u, 1u,
        RenderTapeDigestValidity::Sha256, expected,
        std::as_bytes(std::span(&oracle, 1u)));
    tape = builder.seal();
  }
};

struct Fixture {
  std::array<std::byte, 16u> seed{};
  RenderTapeDigest seedDigest{};
  std::vector<std::byte> tape{};

  explicit Fixture(std::uint32_t clearRectCount = 0u,
                   std::uint32_t clearFlags = 1u) {
    for (std::size_t i = 0u; i < seed.size(); ++i)
      seed[i] = static_cast<std::byte>(i + 1u);
    seedDigest = RenderTapeCaptureSession::sha256(seed);

    D9CCommandChunkWireClear clear{
        .flags = clearFlags,
        .colorARGB = 0xff204060u,
        .z = 1.0f,
        .rectCount = clearRectCount,
        .rectOffset = sizeof(D9CCommandChunkWireClear),
    };
    auto clearBytes = bytesOf(clear);
    if (clearRectCount) {
      const D9CRect rect{0, 0, 16, 16};
      const auto rectBytes = std::as_bytes(std::span(&rect, 1u));
      clearBytes.insert(clearBytes.end(), rectBytes.begin(), rectBytes.end());
    }
    const std::array frameRecords{
        Record{.type = D9C_COMMAND_RECORD_CLEAR, .payload = std::move(clearBytes)},
        Record{.type = D9C_COMMAND_RECORD_PRESENT,
               .payload = bytesOf(D9CCommandChunkWirePresent{
                   .sourceHandleIndex = 0u,
               }),
               .handles = {{kOutput.kind, kOutput.generation,
                            kOutput.objectId}}},
    };
    const auto frame = makeChunk(frameRecords);
    const D9CSurfaceDesc outputDesc{
        .format = 21u, // D3DFMT_A8R8G8B8
        .resourceType = 1u,
        .usage = 1u,
        .pool = 0u,
        .multiSampleType = 0u,
        .width = 16u,
        .height = 16u,
        .depth = 1u,
    };
    const D9CBufferDesc bufferDesc{
        .size = static_cast<std::uint32_t>(seed.size()),
        .pool = 0u,
    };
    const RenderTapeOracleAttachment oracle{
        .identity = kOutput,
        .descriptorKind = static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
    };
    const auto outputDescriptorV2 = outputDescriptor(outputDesc);
    RenderTapeBuilder builder;
    builder.appendBootstrapState(bootstrapChunk());
    builder.appendObjectDefine(
        kOutput, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
        std::as_bytes(std::span(&outputDescriptorV2, 1u)), 0u, {});
    builder.appendObjectDefine(
        kSeedBuffer, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Buffer),
        std::as_bytes(std::span(&bufferDesc, 1u)), 0u, {}, seed.size(), 1u);
    builder.appendResourceMutation(kSeedBuffer, RenderTapeMutationKind::Upload,
                                   0u, 0u, seed.size(), seedDigest);
    builder.appendCommandChunk(
        CommandChunkEnvelope{.recordCount = 2u, .handleCount = 1u}, frame);
    builder.appendPresentComplete(
        5u, 1u, RenderTapeDigestValidity::NotCaptured, {},
        std::as_bytes(std::span(&oracle, 1u)));
    tape = builder.seal();
  }

  RenderTapeProviderBlob blob() const {
    return {.digest = seedDigest, .bytes = seed};
  }
};

void testProducedByCapturedPassTape() {
  constexpr D9CWireObjectIdentity output{
      .kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
      .generation = 7u,
      .objectId = 40u,
  };
  constexpr D9CWireObjectIdentity alias{
      .kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
      .generation = 7u,
      .objectId = 41u,
  };
  constexpr D9CWireObjectIdentity texture{
      .kind = D9C_CHUNK_HANDLE_KIND_TEXTURE,
      .generation = 9u,
      .objectId = 43u,
  };
  auto level = D9CSurfaceDesc{
      .format = 22u,
      .resourceType = 3u,
      .usage = 1u,
      .pool = 0u,
      .width = 4u,
      .height = 4u,
      .depth = 1u,
  };
  auto producedTexture = texture2DDescriptor(
      D9CSurfaceDesc{.format = 22u, .resourceType = 3u, .pool = 0u,
                     .width = 4u, .height = 4u, .depth = 1u});
  RenderTapeTextureDescriptorV2 producedHeader{};
  std::memcpy(&producedHeader, producedTexture.data(), sizeof(producedHeader));
  producedHeader.initialContentDisposition = static_cast<std::uint32_t>(
      RenderTapeInitialContentDisposition::ProducedByCapturedPass);
  std::memcpy(producedTexture.data(), &producedHeader, sizeof(producedHeader));
  std::memcpy(producedTexture.data() + sizeof(RenderTapeTextureDescriptorV2),
              &level, sizeof(level));
  const RenderTapeSurfaceDescriptorV2 aliasDescriptor{
      .schemaVersion = kRenderTapeSurfaceDescriptorVersion2,
      .storage = static_cast<std::uint32_t>(
          RenderTapeSurfaceStorage::TextureSubresource),
      .initialContentDisposition = static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::Unavailable),
      .subresource = 0u,
      .parentTexture = texture,
      .surface = D9CSurfaceDesc{.format = 22u,
                                .resourceType = 1u,
                                .usage = 1u,
                                .pool = 0u,
                                .width = 4u,
                                .height = 4u,
                                .depth = 1u},
  };
  const auto clear = bytesOf(D9CCommandChunkWireClear{
      .flags = 1u,
      .colorARGB = 0xff204060u,
      .z = 1.0f,
      .rectCount = 0u,
      .rectOffset = sizeof(D9CCommandChunkWireClear),
  });
  const auto present = bytesOf(D9CCommandChunkWirePresent{});
  ImportedChunkView bootstrap;
  const auto applyBytes = bootstrapChunkWithRenderTargets(
      std::array{D9CCommandChunkWireRenderTargetBinding{
          .slot = 0u, .valid = 1u, .handleIndex = 0u}});
  check(importPrevalidatedCommandChunk(
            applyBytes,
            CommandChunkEnvelope{.recordCount = 1u, .handleCount = 1u},
            bootstrap),
        "produced-pass apply fixture imports");
  const auto applyRecord = bootstrap.record(0u);
  const std::array frameRecords{
      Record{.type = applyRecord.header.type,
             .payload = std::vector<std::byte>(applyRecord.payload.begin(),
                                               applyRecord.payload.end()),
             .handles = std::vector<D9CCommandChunkWireHandleEntry>(
                 bootstrap.handles.begin(), bootstrap.handles.end())},
      Record{.type = D9C_COMMAND_RECORD_CLEAR, .payload = clear},
      Record{.type = D9C_COMMAND_RECORD_PRESENT, .payload = present},
  };
  const auto outputDescriptorV2 = outputDescriptor(D9CSurfaceDesc{
      .format = 21u, .resourceType = 1u, .usage = 1u, .pool = 0u,
      .width = 4u, .height = 4u, .depth = 1u});
  const RenderTapeOracleAttachment oracle{
      .identity = output,
      .descriptorKind = static_cast<std::uint32_t>(
          RenderTapeDescriptorKind::Surface),
  };
  const auto buildProducedTape = [&](std::span<const std::byte> textureBytes,
                                     const auto& surfaceAlias) {
    RenderTapeBuilder builder;
    builder.appendBootstrapState(implicitBootstrapChunk());
    builder.appendObjectDefine(
        output, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
        std::as_bytes(std::span(&outputDescriptorV2, 1u)), 0u, {});
    builder.appendObjectDefine(
        texture, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Texture),
        textureBytes, 0u, {}, 0u, 0u);
    builder.appendObjectDefine(
        alias, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
        std::as_bytes(std::span(&surfaceAlias, 1u)), 0u, {}, 0u, 0u);
    builder.appendCommandChunk(
        CommandChunkEnvelope{.recordCount = 3u,
                             .handleCount = static_cast<std::uint32_t>(
                                 bootstrap.handles.size())},
        makeChunk(frameRecords));
    builder.appendPresentComplete(
        5u, 1u, RenderTapeDigestValidity::NotCaptured, {},
        std::as_bytes(std::span(&oracle, 1u)));
    return builder.seal();
  };
  const auto tape = buildProducedTape(producedTexture, aliasDescriptor);
  const RenderTapeBlobCatalogue catalogue;
  const auto validation = validateRenderTape(tape, catalogue);
  check(validation.valid(),
        "produced full-clear tape retains and validates its command chunk");
  const auto preflight = preflightFrameTapeIdentity(tape, {});
  check(preflight.complete() && preflight.coverage.seedMutations == 0u &&
            preflight.coverage.commandChunks == 1u &&
            preflight.coverage.commandRecords == 3u &&
            preflight.coverage.clearRecords == 1u &&
            preflight.coverage.presentRecords == 1u,
        "provider admits the bounded produced full-clear tape without a seed");

  auto multiMipTexture = producedTexture;
  RenderTapeTextureDescriptorV2 multiMipHeader{};
  std::memcpy(&multiMipHeader, multiMipTexture.data(), sizeof(multiMipHeader));
  multiMipHeader.mipLevelCount = 2u;
  multiMipHeader.subresourceCount = 2u;
  multiMipTexture.resize(sizeof(multiMipHeader) + 2u * sizeof(D9CSurfaceDesc));
  std::memcpy(multiMipTexture.data(), &multiMipHeader,
              sizeof(multiMipHeader));
  std::memcpy(multiMipTexture.data() + sizeof(multiMipHeader), &level,
              sizeof(level));
  auto level1 = level;
  level1.width = 2u;
  level1.height = 2u;
  std::memcpy(multiMipTexture.data() + sizeof(multiMipHeader) +
                  sizeof(level),
              &level1, sizeof(level1));
  const RenderTapeObjectDefineHeader multiMipFixed{
      .identity = texture,
      .descriptorKind = static_cast<std::uint32_t>(
          RenderTapeDescriptorKind::Texture),
      .descriptorBytes = static_cast<std::uint32_t>(multiMipTexture.size()),
  };
  const auto multiMipDetail = renderTapeClassifyObjectDefineValidation(
      multiMipFixed, multiMipTexture);
  check(multiMipDetail.subreason ==
            RenderTapeObjectDefineValidationSubreason::TextureDescriptorDisposition,
        "produced multi-mip texture remains fail-closed");

  RenderTapeTextureDescriptorV2 cubeHeader{
      .schemaVersion = kRenderTapeTextureDescriptorVersion2,
      .dimension = static_cast<std::uint32_t>(
          RenderTapeTextureDimension::Cube),
      .mipLevelCount = 1u,
      .subresourceCount = 6u,
      .initialContentDisposition = static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::ProducedByCapturedPass),
  };
  auto cubeFace = level;
  cubeFace.format = 114u;
  cubeFace.resourceType = 5u;
  std::vector<std::byte> cubeDescriptor(
      sizeof(cubeHeader) + 6u * sizeof(cubeFace));
  std::memcpy(cubeDescriptor.data(), &cubeHeader, sizeof(cubeHeader));
  for (std::uint32_t face = 0u; face < 6u; ++face) {
    std::memcpy(cubeDescriptor.data() + sizeof(cubeHeader) +
                    face * sizeof(cubeFace),
                &cubeFace, sizeof(cubeFace));
  }
  auto cubeAliasDescriptor = aliasDescriptor;
  cubeAliasDescriptor.surface.format = 114u;
  const auto cubeTape = buildProducedTape(cubeDescriptor,
                                          cubeAliasDescriptor);
  const auto cubeValidation = validateRenderTape(cubeTape, catalogue);
  const auto cubePreflight = preflightFrameTapeIdentity(cubeTape, {});
  check(cubeValidation.valid() && cubePreflight.complete(),
        std::string("provider preflight admits one-mip cube face-zero Produced proof: ") +
            renderTapeValidationStatusName(cubeValidation.status) + "/" +
            frameTapeReplayStatusName(cubePreflight.status));

  auto* factory = dxmt9c_factory_create();
  check(factory != nullptr, "cube Produced provider creates native factory");
  const D9CPresentParams params{
      .backBufferWidth = 4u,
      .backBufferHeight = 4u,
      .backBufferFormat = 21u,
      .backBufferCount = 1u,
      .swapEffect = 1u,
      .windowed = 1u,
      .presentationInterval = 0x80000000u,
  };
  auto* device = dxmt9c_factory_create_device(factory, 0u, &params, 0u,
                                               nullptr);
  check(device != nullptr, "cube Produced provider creates native device");
  const auto cubeReplay = replayFrameTapeIdentity(device, cubeTape, {});
  dxmt9c_device_release(device);
  dxmt9c_factory_release(factory);
  check(cubeReplay.complete() &&
            cubeReplay.conservation.objectsCreated == 3u &&
            cubeReplay.conservation.objectsReleased == 3u,
        "provider creates/releases cube parent, face alias, and output");
}

struct TexturedFixture {
  std::array<std::byte, 16u> seed{
      std::byte{0x10}, std::byte{0x20}, std::byte{0xf0}, std::byte{0xff},
      std::byte{0xe0}, std::byte{0x30}, std::byte{0x20}, std::byte{0xff},
      std::byte{0x30}, std::byte{0xd0}, std::byte{0x40}, std::byte{0xff},
      std::byte{0xc0}, std::byte{0x50}, std::byte{0xb0}, std::byte{0xff},
  };
  std::array<D9CVertexElement, 4u> declaration{{
      {0u, 0u, 3u, 0u, 9u, 0u},
      {0u, 16u, 4u, 0u, 10u, 0u},
      {0u, 20u, 1u, 0u, 5u, 0u},
      {0xffu, 0u, 17u, 0u, 0u, 0u},
  }};
  RenderTapeDigest seedDigest{};
  RenderTapeDigest declarationDigest{};
  std::vector<std::byte> tape{};

  explicit TexturedFixture(bool splitChunks = true,
                           bool omitDraw = false,
                           bool presentExFlags = false,
                           bool partialSeed = false,
                           std::uint16_t extraDrawSection = 0u,
                           std::uint32_t textureFormat = 21u,
                           bool productionVertexDeclaration = false,
                           bool wrongVertexDeclaration = false) {
    if (partialSeed) seed.fill(std::byte{0x55});
    if (wrongVertexDeclaration) declaration[1].usage = 9u;
    const auto seedBytes = partialSeed ? seed.size() / 2u : seed.size();
    seedDigest = RenderTapeCaptureSession::sha256(
        std::span<const std::byte>(seed.data(), seedBytes));
    const auto declarationBytes = std::as_bytes(std::span(declaration));
    declarationDigest = RenderTapeCaptureSession::sha256(declarationBytes);
    const D9CCommandChunkWireClear clear{
        .flags = 1u,
        .colorARGB = 0xff081018u,
        .z = 1.0f,
        .rectCount = 0u,
        .rectOffset = sizeof(D9CCommandChunkWireClear),
    };
    const D9CCommandChunkWirePresent present{
        .flags = presentExFlags ? 1u : 0u,
    };
    const auto clearRecord = Record{
        .type = D9C_COMMAND_RECORD_CLEAR,
        .payload = bytesOf(clear),
    };
    const auto draw = texturedDrawChunk(extraDrawSection);
    const auto presentRecord = Record{
        .type = D9C_COMMAND_RECORD_PRESENT,
        .payload = bytesOf(present),
    };
    const D9CSurfaceDesc outputDesc{
        .format = 21u,
        .resourceType = 1u,
        .usage = 1u,
        .pool = 0u,
        .multiSampleType = 0u,
        .multiSampleQuality = 0u,
        .width = 16u,
        .height = 16u,
        .depth = 1u,
    };
    const auto outputDescriptorV2 = outputDescriptor(outputDesc);
    const auto textureDesc = texture2DDescriptor(D9CSurfaceDesc{
            .format = textureFormat,
            .resourceType = 3u,
            .usage = 0u,
            .pool = 0u,
            .multiSampleType = 0u,
            .multiSampleQuality = 0u,
            .width = 2u,
            .height = 2u,
            .depth = 1u,
        });
    const RenderTapeOracleAttachment oracle{
        .identity = kOutput,
        .descriptorKind = static_cast<std::uint32_t>(
            RenderTapeDescriptorKind::Surface),
    };
    RenderTapeBuilder builder;
    builder.appendBootstrapState(
        texturedBootstrapChunk(productionVertexDeclaration));
    builder.appendObjectDefine(
        kOutput, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
        std::as_bytes(std::span(&outputDescriptorV2, 1u)), 0u, {});
    builder.appendObjectDefine(
        kTexture, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Texture),
        textureDesc, 0u, {}, seedBytes, 1u);
    if (productionVertexDeclaration) {
      const RenderTapeVertexDeclDescriptor declarationDesc{
          .elementCount = static_cast<std::uint32_t>(declaration.size()),
          .elementBytes = static_cast<std::uint32_t>(declarationBytes.size()),
      };
      builder.appendObjectDefine(
          kTexturedVertexDeclaration,
          static_cast<std::uint32_t>(
              RenderTapeDescriptorKind::VertexDeclaration),
          std::as_bytes(std::span(&declarationDesc, 1u)),
          declarationBytes.size(), declarationDigest);
    }
    builder.appendResourceMutation(kTexture, RenderTapeMutationKind::CpuUnlock,
                                   0u, 0u, seedBytes, seedDigest);
    if (splitChunks) {
      const auto clearChunk = makeChunk(std::array{clearRecord});
      builder.appendCommandChunk(
          CommandChunkEnvelope{.recordCount = 1u}, clearChunk);
      if (!omitDraw) {
        builder.appendCommandChunk(
            CommandChunkEnvelope{.recordCount = 1u}, draw);
      }
      const auto presentChunk = makeChunk(std::array{presentRecord});
      builder.appendCommandChunk(
          CommandChunkEnvelope{.recordCount = 1u}, presentChunk);
    } else {
      std::vector<Record> records{clearRecord};
      if (!omitDraw) {
        ImportedChunkView imported;
        check(importPrevalidatedCommandChunk(
                  draw, CommandChunkEnvelope{.recordCount = 1u}, imported),
              "textured draw fixture imports for combined chunk");
        const auto record = imported.record(0u);
        records.push_back(Record{
            .type = record.header.type,
            .payload = std::vector<std::byte>(record.payload.begin(),
                                              record.payload.end()),
        });
      }
      records.push_back(presentRecord);
      const auto combined = makeChunk(records);
      builder.appendCommandChunk(
          CommandChunkEnvelope{
              .recordCount = static_cast<std::uint32_t>(records.size())},
          combined);
    }
    builder.appendPresentComplete(
        splitChunks
            ? (omitDraw ? 6u : 7u) + (productionVertexDeclaration ? 1u : 0u)
            : 5u + (productionVertexDeclaration ? 1u : 0u),
        1u,
        RenderTapeDigestValidity::NotCaptured, {},
        std::as_bytes(std::span(&oracle, 1u)));
    tape = builder.seal();
  }

  RenderTapeProviderBlob blob(bool partialSeed = false) const {
    return {
        .digest = seedDigest,
        .bytes = std::span<const std::byte>(
            seed.data(), partialSeed ? seed.size() / 2u : seed.size()),
    };
  }

  std::array<RenderTapeProviderBlob, 2u> productionBlobs() const {
    return {{
        blob(),
        {.digest = declarationDigest,
         .bytes = std::as_bytes(std::span(declaration))},
    }};
  }
};

struct SequenceFixture {
  std::array<std::byte, 16u> firstSeed{
      std::byte{0x10}, std::byte{0x20}, std::byte{0xf0}, std::byte{0xff},
      std::byte{0xe0}, std::byte{0x30}, std::byte{0x20}, std::byte{0xff},
      std::byte{0x30}, std::byte{0xd0}, std::byte{0x40}, std::byte{0xff},
      std::byte{0xc0}, std::byte{0x50}, std::byte{0xb0}, std::byte{0xff},
  };
  std::array<std::byte, 16u> secondSeed{
      std::byte{0xf0}, std::byte{0x20}, std::byte{0x10}, std::byte{0xff},
      std::byte{0x20}, std::byte{0xd0}, std::byte{0xe0}, std::byte{0xff},
      std::byte{0x40}, std::byte{0x30}, std::byte{0xd0}, std::byte{0xff},
      std::byte{0xb0}, std::byte{0x50}, std::byte{0xc0}, std::byte{0xff},
  };
  RenderTapeDigest firstDigest{};
  RenderTapeDigest secondDigest{};
  std::vector<std::byte> tape{};

  explicit SequenceFixture(RenderTapeDigest secondExpected = {},
                           bool omitBoundaryMutation = false) {
    firstDigest = RenderTapeCaptureSession::sha256(firstSeed);
    secondDigest = RenderTapeCaptureSession::sha256(secondSeed);
    const D9CSurfaceDesc outputDesc{
        .format = 21u,
        .resourceType = 1u,
        .usage = 1u,
        .pool = 0u,
        .multiSampleType = 0u,
        .multiSampleQuality = 0u,
        .width = 16u,
        .height = 16u,
        .depth = 1u,
    };
    const auto outputDescriptorV2 = outputDescriptor(outputDesc);
    const auto textureDesc = texture2DDescriptor(D9CSurfaceDesc{
            .format = 21u,
            .resourceType = 3u,
            .usage = 0u,
            .pool = 0u,
            .multiSampleType = 0u,
            .multiSampleQuality = 0u,
            .width = 2u,
            .height = 2u,
            .depth = 1u,
        });
    const RenderTapeOracleAttachment oracle{
        .identity = kOutput,
        .descriptorKind = static_cast<std::uint32_t>(
            RenderTapeDescriptorKind::Surface),
    };
    const D9CCommandChunkWireClear clear{
        .flags = 1u,
        .colorARGB = 0xff081018u,
        .z = 1.0f,
        .rectCount = 0u,
        .rectOffset = sizeof(D9CCommandChunkWireClear),
    };
    const auto draw = texturedDrawChunk();
    ImportedChunkView importedDraw;
    check(importPrevalidatedCommandChunk(
              draw, CommandChunkEnvelope{.recordCount = 1u}, importedDraw),
          "sequence draw imports");
    const auto drawRecord = importedDraw.record(0u);
    const std::array frameRecords{
        Record{.type = D9C_COMMAND_RECORD_CLEAR, .payload = bytesOf(clear)},
        Record{.type = drawRecord.header.type,
               .payload = std::vector<std::byte>(drawRecord.payload.begin(),
                                                 drawRecord.payload.end())},
        Record{.type = D9C_COMMAND_RECORD_PRESENT,
               .payload = bytesOf(D9CCommandChunkWirePresent{
                   .sourceHandleIndex = 0u,
               }),
               .handles = {{kOutput.kind, kOutput.generation,
                            kOutput.objectId}}},
    };
    const auto frame = makeChunk(frameRecords);
    const RenderTapeDigest firstExpected{
        std::byte{0x3d}, std::byte{0xc6}, std::byte{0xca}, std::byte{0x27},
        std::byte{0x08}, std::byte{0xcc}, std::byte{0xbb}, std::byte{0x28},
        std::byte{0x51}, std::byte{0x06}, std::byte{0xde}, std::byte{0xa4},
        std::byte{0xb1}, std::byte{0xcb}, std::byte{0xa4}, std::byte{0x2e},
        std::byte{0x6d}, std::byte{0x67}, std::byte{0xdd}, std::byte{0x69},
        std::byte{0xff}, std::byte{0xe7}, std::byte{0x2a}, std::byte{0xb0},
        std::byte{0x03}, std::byte{0xd8}, std::byte{0x7d}, std::byte{0x7d},
        std::byte{0x82}, std::byte{0x50}, std::byte{0xb7}, std::byte{0x2e},
    };
    if (std::all_of(secondExpected.begin(), secondExpected.end(),
                    [](std::byte value) { return value == std::byte{}; })) {
      secondExpected = RenderTapeDigest{
          std::byte{0x50}, std::byte{0xd9}, std::byte{0xf7}, std::byte{0x82},
          std::byte{0xe3}, std::byte{0xbd}, std::byte{0x33}, std::byte{0x5e},
          std::byte{0x89}, std::byte{0xc3}, std::byte{0x0a}, std::byte{0x80},
          std::byte{0x2b}, std::byte{0xb0}, std::byte{0x8d}, std::byte{0x50},
          std::byte{0x02}, std::byte{0x0d}, std::byte{0x46}, std::byte{0xa4},
          std::byte{0xc4}, std::byte{0x5a}, std::byte{0xc9}, std::byte{0x1c},
          std::byte{0xd0}, std::byte{0x98}, std::byte{0x9c}, std::byte{0x2b},
          std::byte{0xb1}, std::byte{0xca}, std::byte{0x3c}, std::byte{0xbc},
      };
    }
    RenderTapeBuilder builder(kRenderTapeProfileSequence);
    builder.appendBootstrapState(texturedBootstrapChunk(false));
    builder.appendObjectDefine(
        kOutput, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
        std::as_bytes(std::span(&outputDescriptorV2, 1u)), 0u, {});
    builder.appendObjectDefine(
        kTexture, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Texture),
        textureDesc, 0u, {}, firstSeed.size(), 1u);
    builder.appendResourceMutation(kTexture, RenderTapeMutationKind::CpuUnlock,
                                   0u, 0u, firstSeed.size(), firstDigest);
    builder.appendCommandChunk(
        CommandChunkEnvelope{.recordCount = 3u, .handleCount = 1u}, frame);
    builder.appendPresentComplete(
        5u, 1u, RenderTapeDigestValidity::Sha256, firstExpected,
        std::as_bytes(std::span(&oracle, 1u)));
    if (!omitBoundaryMutation) {
      builder.appendResourceMutation(kTexture, RenderTapeMutationKind::CpuUnlock,
                                     0u, 0u, secondSeed.size(), secondDigest);
    }
    builder.appendCommandChunk(
        CommandChunkEnvelope{.recordCount = 3u, .handleCount = 1u}, frame);
    builder.appendPresentComplete(
        8u, 2u, RenderTapeDigestValidity::Sha256, secondExpected,
        std::as_bytes(std::span(&oracle, 1u)));
    tape = builder.seal();
  }

  std::array<RenderTapeProviderBlob, 2u> blobs() const {
    return {{{.digest = firstDigest, .bytes = firstSeed},
             {.digest = secondDigest, .bytes = secondSeed}}};
  }
};

void acceptsSplitTexturedUpGrammarAndRejectsNearMisses() {
  TexturedFixture split;
  const auto blob = split.blob();
  const RenderTapeBlobCatalogue catalogue{.blobs = {{
      .digest = blob.digest,
      .size = blob.bytes.size(),
      .verified = 1u,
  }}};
  const auto structural = validateRenderTape(split.tape, catalogue);
  check(structural.valid(),
        std::string(renderTapeValidationStatusName(structural.status)) + "/" +
            std::to_string(structural.failedEventIndex) + "/" +
            renderTapeObjectDefineValidationSubreasonName(
                structural.objectDefine.subreason));
  const auto result = preflightFrameTapeIdentity(split.tape,
                                                 std::span(&blob, 1u));
  check(result.complete(), frameTapeReplayStatusName(result.status));
  check(result.coverage.eventCount == 8u &&
            result.coverage.commandChunks == 3u &&
            result.coverage.commandRecords == 3u &&
            result.coverage.clearRecords == 1u &&
            result.coverage.drawPrimitiveUpRecords == 1u &&
            result.coverage.presentRecords == 1u,
        "split command events preserve one Clear -> textured UP draw -> Present order");

  TexturedFixture combined(false);
  const auto combinedBlob = combined.blob();
  check(preflightFrameTapeIdentity(combined.tape,
                                   std::span(&combinedBlob, 1u)).complete(),
        "one canonical chunk with the same total record order remains accepted");

  TexturedFixture productionDeclaration(
      true, false, false, false, 0u, 21u, true);
  const auto productionBlobs = productionDeclaration.productionBlobs();
  const auto productionResult = preflightFrameTapeIdentity(
      productionDeclaration.tape, productionBlobs);
  check(productionResult.complete() &&
            productionResult.coverage.objectDefinitions == 3u &&
            productionResult.conservation.referencedBlobs == 2u,
        "production FVF declaration and texture seed are admitted exactly");

  TexturedFixture wrongDeclaration(
      true, false, false, false, 0u, 21u, true, true);
  const auto wrongDeclarationBlobs = wrongDeclaration.productionBlobs();
  check(preflightFrameTapeIdentity(
            wrongDeclaration.tape, wrongDeclarationBlobs).status ==
            FrameTapeReplayStatus::UnsupportedGrammar,
        "a structurally valid but semantically different declaration fails closed");

  TexturedFixture missingDraw(true, true);
  const auto missingBlob = missingDraw.blob();
  check(preflightFrameTapeIdentity(missingDraw.tape,
                                   std::span(&missingBlob, 1u)).status ==
            FrameTapeReplayStatus::UnsupportedGrammar,
        "textured object/seed without the one UP draw fails closed");

  TexturedFixture presentEx(true, false, true);
  const auto presentExBlob = presentEx.blob();
  check(preflightFrameTapeIdentity(presentEx.tape,
                                   std::span(&presentExBlob, 1u)).status ==
            FrameTapeReplayStatus::UnsupportedGrammar,
        "PresentEx flags remain outside the standard-Present grammar");

  TexturedFixture partial(true, false, false, true);
  const auto partialBlob = partial.blob(true);
  check(preflightFrameTapeIdentity(partial.tape,
                                   std::span(&partialBlob, 1u)).status ==
            FrameTapeReplayStatus::UnsupportedGrammar,
        "partial A8R8G8B8 texture seeds fail before provider effects");

  for (const auto section : {
           D9C_COMMAND_CHUNK_SECTION_STREAM,
           D9C_COMMAND_CHUNK_SECTION_SHADER,
           D9C_COMMAND_CHUNK_SECTION_VERTEX_INPUT,
           D9C_COMMAND_CHUNK_SECTION_INDEX_BUFFER,
       }) {
    TexturedFixture unsupported(true, false, false, false,
                                static_cast<std::uint16_t>(section));
    const auto unsupportedBlob = unsupported.blob();
    check(preflightFrameTapeIdentity(
              unsupported.tape, std::span(&unsupportedBlob, 1u)).status ==
              FrameTapeReplayStatus::UnsupportedGrammar,
          "VB/shader/vdecl/IB draw sections remain fail-closed");
  }

  TexturedFixture compressed(true, false, false, false, 0u, 0x31545844u);
  const auto compressedBlob = compressed.blob();
  check(preflightFrameTapeIdentity(
            compressed.tape, std::span(&compressedBlob, 1u)).status ==
            FrameTapeReplayStatus::UnsupportedGrammar,
        "compressed texture definitions remain outside the provider grammar");
}

void acceptsBoundedIdentityGrammarAndReportsEvidence() {
  Fixture fixture;
  const auto blob = fixture.blob();
  const RenderTapeBlobCatalogue catalogue{.blobs = {{
      .digest = blob.digest,
      .size = blob.bytes.size(),
      .verified = 1u,
  }}};
  const auto validation = validateRenderTape(fixture.tape, catalogue);
  check(validation.valid(), renderTapeValidationStatusName(validation.status));
  const auto result = preflightFrameTapeIdentity(fixture.tape,
                                                  std::span(&blob, 1u));
  check(result.complete(), frameTapeReplayStatusName(result.status));
  check(result.validity.structurallyValid && result.validity.digestsValid,
        "validity evidence must be independent and affirmative");
  check(result.coverage.objectDefinitions == 2u &&
            result.coverage.seedMutations == 1u &&
            result.coverage.bootstrapChunks == 1u &&
            result.coverage.commandChunks == 1u &&
            result.coverage.commandRecords == 2u &&
            result.coverage.clearRecords == 1u &&
            result.coverage.presentRecords == 1u &&
            result.coverage.presentOutputs == 1u,
        "coverage counters must describe the admitted grammar exactly");
  check(result.conservation.inputBlobs == 1u &&
            result.conservation.referencedBlobs == 1u &&
            result.conservation.presentOrdinal == 5u &&
            result.conservation.completionOrdinal == 1u,
        "conservation evidence must close blob and ordinal identities");
}

void canonicalUnsupportedDimensionsReturnTypedGrammar() {
  const D9CSurfaceDesc outputSurface{
      .format = 21u,
      .resourceType = 1u,
      .usage = 1u,
      .pool = 0u,
      .width = 8u,
      .height = 8u,
      .depth = 1u,
  };
  const auto output = outputDescriptor(outputSurface);
  const RenderTapeOracleAttachment oracle{
      .identity = kOutput,
      .descriptorKind = static_cast<std::uint32_t>(
          RenderTapeDescriptorKind::Surface),
  };
  const D9CCommandChunkWireClear clear{
      .flags = 1u,
      .colorARGB = 0xff102030u,
      .z = 1.0f,
      .rectOffset = sizeof(D9CCommandChunkWireClear),
  };
  const std::array records{
      Record{.type = D9C_COMMAND_RECORD_CLEAR, .payload = bytesOf(clear)},
      Record{.type = D9C_COMMAND_RECORD_PRESENT,
             .payload = bytesOf(D9CCommandChunkWirePresent{})},
  };
  const auto frame = makeChunk(records);
  const std::array<std::byte, 4u> seed{
      std::byte{1u}, std::byte{2u}, std::byte{3u}, std::byte{4u}};
  const auto seedDigest = RenderTapeCaptureSession::sha256(seed);
  const RenderTapeBlobCatalogue catalogue{.blobs = {{
      .digest = seedDigest, .size = seed.size(), .verified = 1u,
  }}};
  const RenderTapeProviderBlob blob{.digest = seedDigest, .bytes = seed};
  for (const auto dimension : {RenderTapeTextureDimension::Cube,
                               RenderTapeTextureDimension::Volume}) {
    const std::uint32_t mipCount = 2u;
    const std::uint32_t subresourceCount =
        dimension == RenderTapeTextureDimension::Cube ? 12u : mipCount;
    const RenderTapeTextureDescriptorV2 header{
        .schemaVersion = kRenderTapeTextureDescriptorVersion2,
        .dimension = static_cast<std::uint32_t>(dimension),
        .mipLevelCount = mipCount,
        .subresourceCount = subresourceCount,
        .initialContentDisposition = static_cast<std::uint32_t>(
            RenderTapeInitialContentDisposition::CompleteSeed),
    };
    std::vector<std::byte> descriptor(
        sizeof(header) + subresourceCount * sizeof(D9CSurfaceDesc));
    std::memcpy(descriptor.data(), &header, sizeof(header));
    for (std::uint32_t subresource = 0u; subresource < subresourceCount;
         ++subresource) {
      const auto mip = renderTapeTextureDescriptorMipLevel(
          dimension, mipCount, subresource);
      const D9CSurfaceDesc level{
          .format = 21u,
          .resourceType = dimension == RenderTapeTextureDimension::Cube ? 5u
                                                                         : 4u,
          .pool = 0u,
          .width = 8u >> mip,
          .height = 8u >> mip,
          .depth = dimension == RenderTapeTextureDimension::Volume
                       ? 4u >> mip
                       : 1u,
      };
      std::memcpy(descriptor.data() + sizeof(header) +
                      subresource * sizeof(level),
                  &level, sizeof(level));
    }
    RenderTapeBuilder builder;
    builder.appendBootstrapState(implicitBootstrapChunk());
    builder.appendObjectDefine(
        kOutput, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
        std::as_bytes(std::span(&output, 1u)), 0u, {});
    builder.appendObjectDefine(
        kTexture, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Texture),
        descriptor, 0u, {},
        static_cast<std::uint64_t>(seed.size()) * subresourceCount,
        subresourceCount);
    for (std::uint32_t subresource = 0u;
         subresource < subresourceCount; ++subresource) {
      builder.appendResourceMutation(
          kTexture, RenderTapeMutationKind::Upload, subresource, 0u,
          seed.size(), seedDigest);
    }
    builder.appendCommandChunk(
        CommandChunkEnvelope{.recordCount = 2u, .handleCount = 0u}, frame);
    builder.appendPresentComplete(
        builder.eventCount(), 1u, RenderTapeDigestValidity::NotCaptured, {},
        std::as_bytes(std::span(&oracle, 1u)));
    const auto tape = builder.seal();
    check(validateRenderTape(tape, catalogue).valid(),
          "canonical unsupported texture dimension remains structurally valid");
    check(preflightFrameTapeIdentity(tape, std::span(&blob, 1u)).status ==
              FrameTapeReplayStatus::UnsupportedGrammar,
          "provider reports typed unsupported after canonical V2 inspection");
  }
}

void failsClosedBeforeEffectsOnUnsupportedAndCorruptInputs() {
  Fixture depthOnlyClear(0u, 2u);
  const auto depthOnlyBlob = depthOnlyClear.blob();
  check(preflightFrameTapeIdentity(depthOnlyClear.tape,
                                   std::span(&depthOnlyBlob, 1u)).status ==
            FrameTapeReplayStatus::UnsupportedGrammar,
        "depth-only Clear must fail the production TARGET grammar");

  Fixture partialClear(1u);
  const auto partialBlob = partialClear.blob();
  check(preflightFrameTapeIdentity(partialClear.tape,
                                   std::span(&partialBlob, 1u)).status ==
            FrameTapeReplayStatus::UnsupportedGrammar,
        "partial Clear must fail the narrow full-surface grammar");

  Fixture wrongDigest;
  auto blob = wrongDigest.blob();
  blob.digest[0] ^= std::byte{1u};
  check(preflightFrameTapeIdentity(wrongDigest.tape, std::span(&blob, 1u)).status ==
            FrameTapeReplayStatus::InvalidBlobCatalogue,
        "blob digest mismatch must fail before tape callbacks or provider effects");

  Fixture stale;
  auto* header = reinterpret_cast<RenderTapeHeader*>(stale.tape.data());
  auto* events = reinterpret_cast<RenderTapeEventHeader*>(
      stale.tape.data() + header->eventTableOffset);
  auto* complete = reinterpret_cast<RenderTapePresentCompleteHeader*>(
      stale.tape.data() + header->payloadArenaOffset + events[5].payloadOffset);
  auto* oracle = reinterpret_cast<RenderTapeOracleAttachment*>(complete + 1u);
  ++oracle->identity.generation;
  const auto staleBlob = stale.blob();
  check(preflightFrameTapeIdentity(stale.tape, std::span(&staleBlob, 1u)).status ==
            FrameTapeReplayStatus::InvalidTape,
        "stale generation must fail structural validation before effects");
}

std::vector<std::byte> readbackTight(dxmt9::Device& device,
                                     dxmt9::core::SurfaceHandle surface,
                                     std::uint32_t width,
                                     std::uint32_t height) {
  dxmt9::core::ReadbackPixels pixels;
  check(device.readbackSurface(
            dxmt9::core::ReadbackDesc{.source = surface}, pixels),
        "production Presenter oracle readback succeeds");
  constexpr std::uint32_t bytesPerPixel = 4u;
  const auto tightPitch = width * bytesPerPixel;
  check(pixels.pitch >= tightPitch &&
            pixels.bytes.size() >= static_cast<std::size_t>(pixels.pitch) * height,
        "production Presenter oracle readback has a bounded source span");
  std::vector<std::byte> tight(static_cast<std::size_t>(tightPitch) * height);
  for (std::uint32_t row = 0u; row < height; ++row) {
    std::memcpy(tight.data() + static_cast<std::size_t>(row) * tightPitch,
                pixels.bytes.data() + static_cast<std::size_t>(row) * pixels.pitch,
                tightPitch);
  }
  return tight;
}

void productionPresenterMirrorGpuOracle() {
  check(dxmt9::resolvePresentLoadAction(
            true, dxmt9::PresentOutputLoadPolicy::DeterministicClear) ==
            WMTLoadActionClear,
        "cold offscreen present targets use a deterministic clear");
  check(dxmt9::resolvePresentLoadAction(
            true, dxmt9::PresentOutputLoadPolicy::DontCare) ==
            WMTLoadActionDontCare,
        "explicit offscreen DontCare remains available for specialized callers");
  check(dxmt9::resolvePresentLoadAction(
            false, dxmt9::PresentOutputLoadPolicy::DeterministicClear) ==
            WMTLoadActionDontCare,
        "drawable presents retain the production DontCare fast path");
  constexpr std::uint32_t width = 16u;
  constexpr std::uint32_t height = 16u;
  auto devices = WMT::CopyAllDevices();
  if (!devices || devices.count() == 0u) {
    return;
  }
  WMT::Device metalDevice = devices.object(0u);
  auto upper = dxmt9::CreateDXMT9Device(
      dxmt9::DEVICE_DESC{.device = metalDevice});
  check(upper != nullptr, "production Presenter oracle device constructs");
  const dxmt9::core::SurfaceDesc surfaceDesc{
      .width = width,
      .height = height,
      .format = dxmt9::core::Format::A8R8G8B8,
      .pool = dxmt9::core::Pool::Scratch,
      .usage = dxmt9::core::UsageRenderTarget,
      .renderTarget = true,
      .depthStencil = false,
      .multiSampleType = dxmt9::core::MultiSampleType::None,
  };
  const auto source = upper->createSurface(surfaceDesc);
  const auto primary = upper->createSurface(surfaceDesc);
  const auto mirror = upper->createSurface(surfaceDesc);
  check(source && primary && mirror,
        "production Presenter oracle creates source and output targets");
  auto* sourceRecord = upper->pool()->findSurface(source.value);
  auto* primaryRecord = upper->pool()->findSurface(primary.value);
  auto* mirrorRecord = upper->pool()->findSurface(mirror.value);
  check(sourceRecord && primaryRecord && mirrorRecord && sourceRecord->texture &&
            primaryRecord->texture && mirrorRecord->texture,
        "production Presenter oracle resolves Metal textures from the pool");

  std::vector<std::uint8_t> sourceBytes(width * height * 4u);
  for (std::uint32_t row = 0u; row < height; ++row) {
    for (std::uint32_t column = 0u; column < width; ++column) {
      const auto offset = (row * width + column) * 4u;
      sourceBytes[offset + 0u] = static_cast<std::uint8_t>(column * 13u + row);
      sourceBytes[offset + 1u] = static_cast<std::uint8_t>(row * 17u + column);
      sourceBytes[offset + 2u] = static_cast<std::uint8_t>(column ^ (row * 3u));
      sourceBytes[offset + 3u] = static_cast<std::uint8_t>(0x40u + row + column);
    }
  }
  sourceRecord->texture.replaceRegion(
      WMTOrigin{.x = 0u, .y = 0u, .z = 0u},
      WMTSize{.width = width, .height = height, .depth = 1u}, 0u, 0u,
      sourceBytes.data(), width * 4u, sourceBytes.size());

  auto output = std::make_shared<dxmt9::OffscreenPresentOutput>(
      WMT::Texture{primaryRecord->texture.handle}, width, height);
  dxmt9::Presenter presenter(metalDevice, std::move(output), nullptr, nullptr);
  check(presenter.valid(), "production Presenter oracle is valid");
  auto ticket = std::make_shared<dxmt9::PresentMirrorTicket>();
  const dxmt9::PresentOutputTarget mirrorTarget{
      .texture = WMT::Texture{mirrorRecord->texture.handle},
      .width = width,
      .height = height,
  };
  check(presenter.reservePresentMirror(mirrorTarget, ticket),
        "production Presenter reserves one mirror");
  auto competingTicket = std::make_shared<dxmt9::PresentMirrorTicket>();
  check(!presenter.reservePresentMirror(mirrorTarget, competingTicket),
        "production Presenter rejects a second outstanding mirror");

  auto commandBuffer = upper->queue().newCommandBuffer();
  check(commandBuffer, "production Presenter oracle creates a command buffer");
  const dxmt9::Presenter::EncodeParams params{
      .source = WMT::Texture{sourceRecord->texture.handle},
      .width = width,
      .height = height,
      .displaySyncEnabled = false,
      .contentsScale = 1.0,
      .minimumPresentDuration = 0.0,
      .maxDrawableCount = dxmt9::kDefaultMetalDrawableCount,
      .opaqueAlpha = false,
      .seqId = 1u,
  };
  const auto first = presenter.encodeCommands(commandBuffer, params);
  check(first.acquired && first.encoded && ticket->encoded(),
        "production Presenter encodes the primary and one-shot mirror passes");
  commandBuffer.commit();
  commandBuffer.waitUntilCompleted();
  const auto primaryBytes = readbackTight(*upper, primary, width, height);
  const auto mirrorBytes = readbackTight(*upper, mirror, width, height);
  check(primaryBytes == mirrorBytes,
        "production Presenter primary and mirror readback bytes match");
  check(RenderTapeCaptureSession::sha256(primaryBytes) ==
            RenderTapeCaptureSession::sha256(mirrorBytes),
        "production Presenter primary and mirror digests match");

  auto secondTicket = std::make_shared<dxmt9::PresentMirrorTicket>();
  check(presenter.reservePresentMirror(mirrorTarget, secondTicket),
        "production Presenter permits a new mirror after one-shot consume");
  presenter.cancelPresentMirror(secondTicket);
  for (std::size_t i = 0u; i < sourceBytes.size(); ++i) {
    sourceBytes[i] ^= static_cast<std::uint8_t>((i % 29u) + 1u);
  }
  sourceRecord->texture.replaceRegion(
      WMTOrigin{.x = 0u, .y = 0u, .z = 0u},
      WMTSize{.width = width, .height = height, .depth = 1u}, 0u, 0u,
      sourceBytes.data(), width * 4u, sourceBytes.size());
  auto secondCommandBuffer = upper->queue().newCommandBuffer();
  check(secondCommandBuffer, "production Presenter oracle creates its second command buffer");
  const auto second = presenter.encodeCommands(secondCommandBuffer, params);
  check(second.acquired && second.encoded && !secondTicket->encoded(),
        "cancelled mirror does not leak into a later Present");
  secondCommandBuffer.commit();
  secondCommandBuffer.waitUntilCompleted();
  const auto primaryBytesAfterCancel = readbackTight(*upper, primary, width, height);
  const auto mirrorBytesAfterCancel = readbackTight(*upper, mirror, width, height);
  check(primaryBytesAfterCancel != primaryBytes &&
            mirrorBytesAfterCancel == mirrorBytes,
        "cancelled mirror remains untouched while the later primary changes");
}

void nativeMetalOffscreenIdentityReplay() {
  Fixture fixture;
  auto* factory = dxmt9c_factory_create();
  check(factory != nullptr, "native Metal factory must be available");
  const D9CPresentParams params{
      .backBufferWidth = 16u,
      .backBufferHeight = 16u,
      .backBufferFormat = 21u,
      .backBufferCount = 1u,
      .swapEffect = 1u,
      .windowed = 1u,
      .presentationInterval = 0x80000000u,
  };
  auto* device = dxmt9c_factory_create_device(factory, 0u, &params, 0u, nullptr);
  check(device != nullptr, "native Metal replay device must construct");
  const auto blob = fixture.blob();
  const auto result = replayFrameTapeIdentity(device, fixture.tape,
                                               std::span(&blob, 1u));
  dxmt9c_device_release(device);
  dxmt9c_factory_release(factory);
  check(result.complete(), frameTapeReplayStatusName(result.status));
  check(result.validity.outputReadback && result.validity.outputBytes != 0u,
        "offscreen PresentOutput must use production readback after completion");
  check(result.conservation.objectsCreated == 2u &&
            result.conservation.objectsReleased == 2u,
        "replay-owned wrappers must be conserved through completion cleanup");
}

void nativeMetalTexturedUpDigestRepeats() {
  TexturedFixture fixture;
  const auto blob = fixture.blob();
  TexturedFixture productionFixture(
      true, false, false, false, 0u, 21u, true);
  const auto productionBlobs = productionFixture.productionBlobs();
  auto* factory = dxmt9c_factory_create();
  check(factory != nullptr, "textured native Metal factory must be available");
  const D9CPresentParams params{
      .backBufferWidth = 16u,
      .backBufferHeight = 16u,
      .backBufferFormat = 21u,
      .backBufferCount = 1u,
      .swapEffect = 1u,
      .windowed = 1u,
      .presentationInterval = 0x80000000u,
  };
  auto* device = dxmt9c_factory_create_device(factory, 0u, &params, 0u, nullptr);
  check(device != nullptr, "textured native Metal replay device must construct");
  const auto first = replayFrameTapeIdentity(device, fixture.tape,
                                              std::span(&blob, 1u));
  const auto second = replayFrameTapeIdentity(device, fixture.tape,
                                               std::span(&blob, 1u));
  const auto production = replayFrameTapeIdentity(
      device, productionFixture.tape, productionBlobs);
  dxmt9c_device_release(device);
  dxmt9c_factory_release(factory);
  check(first.complete() && second.complete() && production.complete(),
        "textured UP replay must complete through FVF and production declaration routing");
  check(first.validity.outputReadback && first.validity.outputNonDegenerate &&
            first.validity.outputBytes == 16u * 16u * 4u,
        "textured UP fixture must produce a tight non-uniform Metal output");
  const RenderTapeDigest expectedDigest{
      std::byte{0x3d}, std::byte{0xc6}, std::byte{0xca}, std::byte{0x27},
      std::byte{0x08}, std::byte{0xcc}, std::byte{0xbb}, std::byte{0x28},
      std::byte{0x51}, std::byte{0x06}, std::byte{0xde}, std::byte{0xa4},
      std::byte{0xb1}, std::byte{0xcb}, std::byte{0xa4}, std::byte{0x2e},
      std::byte{0x6d}, std::byte{0x67}, std::byte{0xdd}, std::byte{0x69},
      std::byte{0xff}, std::byte{0xe7}, std::byte{0x2a}, std::byte{0xb0},
      std::byte{0x03}, std::byte{0xd8}, std::byte{0x7d}, std::byte{0x7d},
      std::byte{0x82}, std::byte{0x50}, std::byte{0xb7}, std::byte{0x2e},
  };
  check(first.validity.outputDigest == expectedDigest,
        "textured UP fixture must retain its exact non-uniform Metal digest");
  check(first.validity.outputDigest == second.validity.outputDigest,
        "identical textured tape replay must repeat the exact Metal digest");
  check(first.validity.outputDigest == production.validity.outputDigest,
        "the production FVF declaration must preserve the exact Metal digest");
  check(first.conservation.objectsCreated == 2u &&
            first.conservation.objectsReleased == 2u &&
            second.conservation.objectsCreated == 2u &&
            second.conservation.objectsReleased == 2u &&
            production.conservation.objectsCreated == 3u &&
            production.conservation.objectsReleased == 3u,
        "repeat replay conserves output, texture, and declaration wrappers independently");
}

void generalIndexedReplayConservesAllObjects() {
  GeneralIndexedFixture fixture;
  RenderTapeBlobCatalogue catalogue;
  for (const auto& blob : fixture.blobsValue) {
    catalogue.blobs.push_back({.digest = blob.digest,
                               .size = blob.bytes.size(),
                               .verified = 1u});
  }
  const auto structural = validateRenderTape(fixture.tape, catalogue);
  check(structural.valid(),
        std::string("general fixture structural validation failed: ") +
            renderTapeValidationStatusName(structural.status) + " event=" +
            std::to_string(structural.failedEventIndex));
  const auto preflight = preflightRenderTapeIdentity(
      fixture.tape, fixture.blobsValue);
  check(preflight.complete(), frameTapeReplayStatusName(preflight.status));
  check(preflight.coverage.objectDefinitions == 5u &&
            preflight.coverage.seedMutations == 3u &&
            preflight.coverage.drawIndexedPrimitiveRecords == 1u &&
            preflight.coverage.presentRecords == 1u &&
            preflight.coverage.presentSourceMappings == 1u &&
            preflight.conservation.inputBlobs == 5u &&
            preflight.conservation.referencedBlobs == 5u,
        "indexed shader/VB/IB tape must report the general grammar evidence");

  const D9CPresentParams params{
      .backBufferWidth = 16u,
      .backBufferHeight = 16u,
      .backBufferFormat = 22u,
      .backBufferCount = 1u,
      .swapEffect = 1u,
      .windowed = 1u,
      .presentationInterval = 0x80000000u,
  };
  auto* factory = dxmt9c_factory_create();
  check(factory != nullptr, "general indexed factory must be available");
  auto* device =
      dxmt9c_factory_create_device(factory, 0u, &params, 0u, nullptr);
  check(device != nullptr, "general indexed replay device must construct");
  const auto replay = replayRenderTapeIdentity(
      device, fixture.tape, fixture.blobsValue);
  dxmt9c_device_release(device);
  dxmt9c_factory_release(factory);
  check(replay.complete(), frameTapeReplayStatusName(replay.status));
  check(replay.coverage.drawIndexedPrimitiveRecords == 1u &&
            replay.coverage.objectDefinitions == 5u &&
            replay.conservation.objectsCreated == 5u &&
            replay.conservation.objectsReleased == 5u &&
            replay.conservation.referencedBlobs == 5u,
        "general indexed replay must conserve output, VB, IB, shader, and declaration");
}

void productionParallelIndexedFixtureHasTwoChildren() {
  ParallelIndexedFixture fixture;
  RenderTapeBlobCatalogue catalogue;
  for (const auto& blob : fixture.blobsValue) {
    catalogue.blobs.push_back({.digest = blob.digest,
                               .size = blob.bytes.size(),
                               .verified = 1u});
  }
  const auto structural = validateRenderTape(fixture.tape, catalogue);
  check(structural.valid(),
        std::string("parallel indexed fixture is structurally valid: ") +
            renderTapeValidationStatusName(structural.status) +
            " event=" + std::to_string(structural.failedEventIndex) +
            " chunk=" +
            std::to_string(static_cast<unsigned>(structural.chunkStatus)));
  const auto preflight = preflightRenderTapeIdentity(fixture.tape,
                                                      fixture.blobsValue);
  check(preflight.complete(), frameTapeReplayStatusName(preflight.status));
  check(preflight.coverage.clearRecords == 1u &&
            preflight.coverage.applyStateRecords == 1u &&
            preflight.coverage.drawIndexedPrimitiveRecords == 128u &&
            preflight.coverage.presentRecords == 1u &&
            preflight.coverage.commandRecords == 131u,
        std::string("parallel fixture is Clear + FULL_SNAPSHOT + 128 indexed draws + Present: ") +
            std::to_string(preflight.coverage.clearRecords) + "," +
            std::to_string(preflight.coverage.applyStateRecords) + "," +
            std::to_string(preflight.coverage.drawIndexedPrimitiveRecords) + "," +
            std::to_string(preflight.coverage.stateConstantRecords) + "," +
            std::to_string(preflight.coverage.presentRecords) + "," +
            std::to_string(preflight.coverage.commandRecords));

  const D9CPresentParams params{
      .backBufferWidth = 16u,
      .backBufferHeight = 16u,
      .backBufferFormat = 22u,
      .backBufferCount = 1u,
      .swapEffect = 1u,
      .windowed = 1u,
      .presentationInterval = 0x80000000u,
  };
  const auto replayFresh = [&](const char* partitionMode) {
    check(::setenv("DXMT9_RENDER_PARTITION_MODE", partitionMode, 1) == 0,
          "parallel fixture sets its partition mode before device creation");
    check(::setenv("DXMT_DEBUG_DISABLE_SHADER_ARCHIVE", "1", 1) == 0 &&
              ::setenv("DXMT9_PREWARM", "disabled", 1) == 0,
          "parallel fixture establishes hermetic replay environment");
    auto* factory = dxmt9c_factory_create();
    check(factory != nullptr, "parallel fixture factory must be available");
    auto* device =
        dxmt9c_factory_create_device(factory, 0u, &params, 0u, nullptr);
    check(device != nullptr, "parallel fixture replay device must construct");
    const auto result = replayRenderTapeIdentity(
        device, fixture.tape, fixture.blobsValue);
    dxmt9c_device_release(device);
    dxmt9c_factory_release(factory);
    return result;
  };
  const auto identity = replayFresh("identity");
  const auto explicitParallel = replayFresh("parallel");
  check(identity.complete() && explicitParallel.complete(),
        std::string("parallel fixture identity and explicit replays complete: ") +
            frameTapeReplayStatusName(identity.status) + ", " +
            frameTapeReplayStatusName(explicitParallel.status));
  check(identity.validity.outputReadback &&
            identity.validity.expectedDigestCaptured &&
            identity.validity.expectedDigestMatched &&
            explicitParallel.validity.expectedDigestCaptured &&
            explicitParallel.validity.expectedDigestMatched &&
            identity.validity.outputDigest == explicitParallel.validity.outputDigest &&
            identity.validity.outputDigest != RenderTapeDigest{},
        "parallel fixture produces one deterministic non-empty digest");
  check(identity.coverage.eventCount == explicitParallel.coverage.eventCount &&
            identity.coverage.commandRecords == explicitParallel.coverage.commandRecords &&
            identity.conservation.objectsCreated ==
                explicitParallel.conservation.objectsCreated &&
            identity.conservation.objectsReleased ==
                explicitParallel.conservation.objectsReleased,
        "identity and explicit parallel replay evidence is conserved");
  const auto sameValidity = [](const FrameTapeValidityEvidence& left,
                               const FrameTapeValidityEvidence& right) {
    return left.structurallyValid == right.structurallyValid &&
           left.digestsValid == right.digestsValid &&
           left.outputReadback == right.outputReadback &&
           left.expectedDigestCaptured == right.expectedDigestCaptured &&
           left.expectedDigestMatched == right.expectedDigestMatched &&
           left.expectedPixelsCompared == right.expectedPixelsCompared &&
           left.pixelEnvelopeMatched == right.pixelEnvelopeMatched &&
           left.outputNonDegenerate == right.outputNonDegenerate &&
           left.outputBytes == right.outputBytes &&
           left.allowedDifferingPixels == right.allowedDifferingPixels &&
           left.differingPixels == right.differingPixels &&
           left.totalRgbDelta == right.totalRgbDelta &&
           left.differingAlphaPixels == right.differingAlphaPixels &&
           left.maxRgbDelta == right.maxRgbDelta &&
           left.expectedOutputDigest == right.expectedOutputDigest &&
           left.outputDigest == right.outputDigest;
  };
  check(sameValidity(identity.validity, explicitParallel.validity),
        "identity and explicit parallel validity evidence is exactly equal");
  check(identity.status == explicitParallel.status &&
            identity.failedEventIndex == explicitParallel.failedEventIndex &&
            identity.profile == explicitParallel.profile &&
            identity.requirements.outputWidth ==
                explicitParallel.requirements.outputWidth &&
            identity.requirements.outputHeight ==
                explicitParallel.requirements.outputHeight &&
            identity.requirements.outputFormat ==
                explicitParallel.requirements.outputFormat,
        "identity and explicit parallel replay status is exactly equal");
  const auto sameCoverage = [](const FrameTapeCoverageEvidence& left,
                               const FrameTapeCoverageEvidence& right) {
    return left.eventCount == right.eventCount &&
           left.objectDefinitions == right.objectDefinitions &&
           left.seedMutations == right.seedMutations &&
           left.bootstrapChunks == right.bootstrapChunks &&
           left.commandChunks == right.commandChunks &&
           left.commandRecords == right.commandRecords &&
           left.clearRecords == right.clearRecords &&
           left.drawPrimitiveRecords == right.drawPrimitiveRecords &&
           left.drawIndexedPrimitiveRecords == right.drawIndexedPrimitiveRecords &&
           left.drawPrimitiveUpRecords == right.drawPrimitiveUpRecords &&
           left.stateConstantRecords == right.stateConstantRecords &&
           left.applyStateRecords == right.applyStateRecords &&
           left.presentRecords == right.presentRecords &&
           left.presentSourceMappings == right.presentSourceMappings &&
           left.presentOutputs == right.presentOutputs &&
           left.objectDestroys == right.objectDestroys;
  };
  check(sameCoverage(identity.coverage, explicitParallel.coverage),
        "identity and explicit parallel coverage evidence is exactly equal");
  check(identity.conservation.inputBlobs == explicitParallel.conservation.inputBlobs &&
            identity.conservation.referencedBlobs ==
                explicitParallel.conservation.referencedBlobs &&
            identity.conservation.objectsCreated ==
                explicitParallel.conservation.objectsCreated &&
            identity.conservation.objectsReleased ==
                explicitParallel.conservation.objectsReleased &&
            identity.conservation.presentOrdinal ==
                explicitParallel.conservation.presentOrdinal &&
            identity.conservation.completionOrdinal ==
                explicitParallel.conservation.completionOrdinal,
        "identity and explicit parallel conservation evidence is exactly equal");
  check(identity.intervalCount == explicitParallel.intervalCount &&
            sameValidity(identity.intervals[0].validity,
                         explicitParallel.intervals[0].validity) &&
            identity.intervals[0].presentOrdinal ==
                explicitParallel.intervals[0].presentOrdinal &&
            identity.intervals[0].completionOrdinal ==
                explicitParallel.intervals[0].completionOrdinal,
        "identity and explicit parallel interval output identity is exact");
}

void boundedSequenceMutationIsVisibleAtSecondPresent() {
  SequenceFixture fixture;
  const auto blobs = fixture.blobs();
  const auto preflight = preflightRenderTapeIdentity(fixture.tape, blobs);
  check(preflight.complete(), frameTapeReplayStatusName(preflight.status));
  check(preflight.profile == kRenderTapeProfileSequence &&
            preflight.intervalCount == 2u &&
            preflight.coverage.presentRecords == 2u &&
            preflight.coverage.presentOutputs == 2u &&
            preflight.coverage.seedMutations == 2u &&
            preflight.intervals[0].presentOrdinal == 5u &&
            preflight.intervals[1].presentOrdinal == 8u &&
            preflight.intervals[0].completionOrdinal == 1u &&
            preflight.intervals[1].completionOrdinal == 2u,
        "sequence preflight conserves two ordered Present intervals");
  check(preflightFrameTapeIdentity(fixture.tape, blobs).status ==
            FrameTapeReplayStatus::UnsupportedGrammar,
        "the legacy frame entry point remains sequence-strict");
  SequenceFixture missingBoundary({}, true);
  const auto missingBlobs = missingBoundary.blobs();
  const auto missing = preflightRenderTapeIdentity(
      missingBoundary.tape, missingBlobs);
  check(missing.status == FrameTapeReplayStatus::InvalidTape &&
            missing.conservation.objectsCreated == 0u &&
            missing.conservation.objectsReleased == 0u,
        "a second interval without its boundary mutation fails before effects");

  const D9CPresentParams params{
      .backBufferWidth = 16u,
      .backBufferHeight = 16u,
      .backBufferFormat = 21u,
      .backBufferCount = 1u,
      .swapEffect = 1u,
      .windowed = 1u,
      .presentationInterval = 0x80000000u,
  };
  const auto replayFresh = [&] {
    auto* factory = dxmt9c_factory_create();
    check(factory != nullptr, "sequence native Metal factory must be available");
    auto* device =
        dxmt9c_factory_create_device(factory, 0u, &params, 0u, nullptr);
    check(device != nullptr, "sequence replay device must construct");
    const auto result = replayRenderTapeIdentity(device, fixture.tape, blobs);
    dxmt9c_device_release(device);
    dxmt9c_factory_release(factory);
    return result;
  };
  const auto first = replayFresh();
  const auto second = replayFresh();
  check(first.complete() && second.complete(),
        frameTapeReplayStatusName(first.complete() ? second.status
                                                   : first.status));
  check(first.validity.outputReadback && first.validity.expectedDigestCaptured &&
            first.validity.expectedDigestMatched &&
            first.intervals[0].validity.outputDigest !=
                first.intervals[1].validity.outputDigest,
        "the boundary mutation produces two distinct accepted outputs");
  check(first.intervals[0].validity.outputDigest ==
                second.intervals[0].validity.outputDigest &&
            first.intervals[1].validity.outputDigest ==
                second.intervals[1].validity.outputDigest,
        "fresh-device sequence replay repeats both interval digests exactly");
  check(first.conservation.objectsCreated == 2u &&
            first.conservation.objectsReleased == 2u &&
            second.conservation.objectsCreated == 2u &&
            second.conservation.objectsReleased == 2u,
        "each sequence replay conserves its output and texture wrappers");
}

void productionShapeUsesImplicitDefaultOutputAndExactDigest() {
  ProductionFixture fixture;
  check(fixture.tape.size() != 0u, "production fixture must seal");
  const auto validation = preflightFrameTapeIdentity(fixture.tape, {});
  check(validation.complete(), frameTapeReplayStatusName(validation.status));
  check(validation.coverage.eventCount == 4u &&
            validation.coverage.objectDefinitions == 1u &&
            validation.coverage.seedMutations == 0u &&
            validation.coverage.commandChunks == 1u &&
            validation.coverage.commandRecords == 2u,
        "production fixture must match the four-event implicit-output capture");
  check(validation.requirements.outputWidth == 16u &&
            validation.requirements.outputHeight == 16u &&
            validation.requirements.outputFormat == 21u,
        "preflight must expose the admitted output requirements");
  check(validation.validity.expectedDigestCaptured,
        "production fixture must carry an output digest oracle");
  check(classifyFrameTapeBootstrapOutput(
            implicitBootstrapChunk(), CommandChunkEnvelope{.recordCount = 1u},
            kOutput) == FrameTapeBootstrapOutputDisposition::ImplicitDefault,
        "production bootstrap must classify as implicit default RT0");
  const auto explicitBootstrap = bootstrapChunk();
  check(classifyFrameTapeBootstrapOutput(
            explicitBootstrap, CommandChunkEnvelope{.recordCount = 1u,
                                                     .handleCount = 1u},
            kOutput) == FrameTapeBootstrapOutputDisposition::ExplicitExact,
        "explicit exact RT0 must remain accepted");
  check(classifyFrameTapeBootstrapOutput(
            explicitBootstrap, CommandChunkEnvelope{.recordCount = 1u,
                                                     .handleCount = 1u},
            D9CWireObjectIdentity{.kind = kOutput.kind,
                                  .generation = kOutput.generation + 1u,
                                  .objectId = kOutput.objectId}) ==
            FrameTapeBootstrapOutputDisposition::WrongIdentity,
        "wrong-generation RT0 must fail closed");
  auto explicitNull = explicitBootstrap;
  ImportedChunkView explicitView;
  check(importPrevalidatedCommandChunk(
            explicitNull, CommandChunkEnvelope{.recordCount = 1u,
                                                .handleCount = 1u},
            explicitView),
        "explicit bootstrap must import for negative classification");
  const auto renderTarget = explicitView.record(0u).section(2u);
  D9CCommandChunkWireRenderTargetBinding nullBinding{};
  std::memcpy(&nullBinding, renderTarget.payload.data(), sizeof(nullBinding));
  nullBinding.valid = 0u;
  nullBinding.handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX;
  std::memcpy(const_cast<std::byte*>(renderTarget.payload.data()), &nullBinding,
              sizeof(nullBinding));
  check(classifyFrameTapeBootstrapOutput(
            explicitNull, CommandChunkEnvelope{.recordCount = 1u,
                                                .handleCount = 1u},
            kOutput) == FrameTapeBootstrapOutputDisposition::ExplicitNull,
        "explicit null RT0 must fail closed");
  const std::array ambiguousTargets{
      D9CCommandChunkWireRenderTargetBinding{
          .slot = 0u,
          .valid = 1u,
          .handleIndex = 0u,
      },
      D9CCommandChunkWireRenderTargetBinding{
          .slot = 0u,
          .valid = 1u,
          .handleIndex = 1u,
      },
  };
  check(classifyFrameTapeBootstrapOutput(
            bootstrapChunkWithRenderTargets(ambiguousTargets),
            CommandChunkEnvelope{.recordCount = 1u, .handleCount = 2u},
            kOutput) == FrameTapeBootstrapOutputDisposition::Ambiguous,
        "duplicate RT0 bindings must fail closed as ambiguous");
  auto outOfRange = explicitBootstrap;
  ImportedChunkView outOfRangeView;
  check(importPrevalidatedCommandChunk(
            outOfRange, CommandChunkEnvelope{.recordCount = 1u,
                                               .handleCount = 1u},
            outOfRangeView),
        "explicit bootstrap must import for slot-range classification");
  const auto outOfRangeRenderTarget = outOfRangeView.record(0u).section(2u);
  D9CCommandChunkWireRenderTargetBinding outOfRangeBinding{};
  std::memcpy(&outOfRangeBinding, outOfRangeRenderTarget.payload.data(),
              sizeof(outOfRangeBinding));
  outOfRangeBinding.slot = 1u;
  std::memcpy(const_cast<std::byte*>(outOfRangeRenderTarget.payload.data()),
              &outOfRangeBinding, sizeof(outOfRangeBinding));
  check(classifyFrameTapeBootstrapOutput(
            outOfRange, CommandChunkEnvelope{.recordCount = 1u,
                                               .handleCount = 1u},
            kOutput) == FrameTapeBootstrapOutputDisposition::SlotOutOfRange,
        "non-zero RT slot must fail closed");
  auto* factory = dxmt9c_factory_create();
  check(factory != nullptr, "production fixture factory must be available");
  const D9CPresentParams params{
      .backBufferWidth = validation.requirements.outputWidth,
      .backBufferHeight = validation.requirements.outputHeight,
      .backBufferFormat = validation.requirements.outputFormat,
      .backBufferCount = 1u,
      .swapEffect = 1u,
      .windowed = 1u,
      .presentationInterval = 0x80000000u,
  };
  auto* device = dxmt9c_factory_create_device(factory, 0u, &params, 0u, nullptr);
  check(device != nullptr, "production fixture device must construct");
  const auto result = replayFrameTapeIdentity(device, fixture.tape, {});
  dxmt9c_device_release(device);
  dxmt9c_factory_release(factory);
  check(result.validity.outputReadback && result.validity.expectedDigestCaptured &&
            result.validity.expectedDigestMatched,
        "production fixture must read back and compare its digest");
  check(result.complete(), frameTapeReplayStatusName(result.status));
}

void productionShapeReportsWrongExpectedDigest() {
  ProductionFixture fixture(true);
  const auto validation = preflightFrameTapeIdentity(fixture.tape, {});
  check(validation.complete(), frameTapeReplayStatusName(validation.status));
  const D9CPresentParams params{
      .backBufferWidth = validation.requirements.outputWidth,
      .backBufferHeight = validation.requirements.outputHeight,
      .backBufferFormat = validation.requirements.outputFormat,
      .backBufferCount = 1u,
      .swapEffect = 1u,
      .windowed = 1u,
      .presentationInterval = 0x80000000u,
  };
  auto* factory = dxmt9c_factory_create();
  check(factory != nullptr, "wrong-digest fixture factory must be available");
  auto* device = dxmt9c_factory_create_device(factory, 0u, &params, 0u, nullptr);
  check(device != nullptr, "wrong-digest fixture device must construct");
  const auto result = replayFrameTapeIdentity(device, fixture.tape, {});
  dxmt9c_device_release(device);
  dxmt9c_factory_release(factory);
  check(result.status == FrameTapeReplayStatus::OutputMismatch,
        "wrong expected digest must report output mismatch");
  check(result.validity.outputReadback && result.validity.expectedDigestCaptured &&
            !result.validity.expectedDigestMatched,
        "output mismatch must retain readback and digest comparison evidence");
  check(result.conservation.objectsCreated == 1u &&
            result.conservation.objectsReleased == 1u,
        "output mismatch must still clean up replay-owned objects");
}

void pixelOracleEnvelopeIsNarrowAndDigestAuthenticated() {
  constexpr std::uint32_t width = 1024u;
  constexpr std::uint32_t height = 768u;
  std::vector<std::byte> expected(
      static_cast<std::size_t>(width) * height * 4u,
                                  std::byte{0x40});
  for (std::size_t offset = 3u; offset < expected.size(); offset += 4u) {
    expected[offset] = std::byte{0xff};
  }
  const auto expectedDigest = RenderTapeCaptureSession::sha256(expected);
  const auto mismatch = [&](std::span<const std::byte> actual) {
    FrameTapeReplayResult result{};
    result.status = FrameTapeReplayStatus::OutputMismatch;
    result.profile = kRenderTapeProfileFrame;
    result.intervalCount = 1u;
    result.failedEventIndex = 7u;
    result.requirements = {
        .outputWidth = width,
        .outputHeight = height,
        .outputFormat = 22u,
    };
    result.validity.structurallyValid = true;
    result.validity.digestsValid = true;
    result.validity.outputReadback = true;
    result.validity.expectedDigestCaptured = true;
    result.validity.outputBytes = actual.size();
    result.validity.expectedOutputDigest = expectedDigest;
    result.validity.outputDigest = RenderTapeCaptureSession::sha256(actual);
    result.outputPixels.assign(actual.begin(), actual.end());
    result.intervals[0].validity = result.validity;
    result.coverage.commandChunks = 1u;
    result.coverage.commandRecords = 1u;
    result.coverage.presentRecords = 1u;
    result.coverage.presentSourceMappings = 1u;
    result.coverage.presentOutputs = 1u;
    result.conservation.inputBlobs = 1u;
    result.conservation.referencedBlobs = 1u;
    result.conservation.objectsCreated = 1u;
    result.conservation.objectsReleased = 1u;
    result.conservation.presentOrdinal = 1u;
    result.conservation.completionOrdinal = 1u;
    return result;
  };

  auto adjacent = expected;
  adjacent[0] = std::byte{0x43};
  auto accepted = mismatch(adjacent);
  check(applyRenderTapePixelOracleEnvelope(accepted, expected) &&
            accepted.complete() &&
            !accepted.validity.expectedDigestMatched &&
            accepted.validity.expectedPixelsCompared &&
            accepted.validity.pixelEnvelopeMatched &&
            accepted.validity.allowedDifferingPixels == 96u &&
            accepted.validity.differingPixels == 1u &&
            accepted.validity.maxRgbDelta == 3u &&
            accepted.validity.totalRgbDelta == 3u &&
            accepted.validity.differingAlphaPixels == 0u,
        "one bounded RGB quantization pixel is accepted with exact evidence");

  auto tooMany = adjacent;
  for (std::size_t pixel = 0u; pixel < 97u; ++pixel) {
    tooMany[pixel * 4u] = std::byte{0x41};
  }
  auto rejectedCount = mismatch(tooMany);
  check(!applyRenderTapePixelOracleEnvelope(rejectedCount, expected) &&
            rejectedCount.status == FrameTapeReplayStatus::OutputMismatch &&
            rejectedCount.validity.differingPixels == 97u,
        "pixel envelope rejects the 97th differing pixel");

  auto excessiveAggregate = expected;
  for (std::size_t pixel = 0u; pixel < 96u; ++pixel) {
    excessiveAggregate[pixel * 4u] = std::byte{0x43};
  }
  auto rejectedAggregate = mismatch(excessiveAggregate);
  check(!applyRenderTapePixelOracleEnvelope(rejectedAggregate, expected) &&
            rejectedAggregate.validity.differingPixels == 96u &&
            rejectedAggregate.validity.maxRgbDelta == 3u &&
            rejectedAggregate.validity.totalRgbDelta == 288u,
        "pixel envelope rejects excessive aggregate RGB drift");

  auto deltaFour = expected;
  deltaFour[0] = std::byte{0x44};
  auto rejectedDelta = mismatch(deltaFour);
  check(!applyRenderTapePixelOracleEnvelope(rejectedDelta, expected) &&
            rejectedDelta.validity.maxRgbDelta == 4u,
        "pixel envelope rejects RGB delta four");

  auto alpha = expected;
  alpha[3] = std::byte{0xfe};
  auto rejectedAlpha = mismatch(alpha);
  check(!applyRenderTapePixelOracleEnvelope(rejectedAlpha, expected) &&
            rejectedAlpha.validity.differingAlphaPixels == 1u,
        "pixel envelope rejects any alpha difference");

  auto wrongSidecar = mismatch(adjacent);
  auto unauthenticated = expected;
  unauthenticated[8] = std::byte{0x41};
  check(!applyRenderTapePixelOracleEnvelope(wrongSidecar, unauthenticated) &&
            !wrongSidecar.validity.expectedPixelsCompared,
        "pixel envelope rejects a sidecar that does not match the tape digest");

  auto invalidEvidence = mismatch(adjacent);
  invalidEvidence.validity.structurallyValid = false;
  check(!applyRenderTapePixelOracleEnvelope(invalidEvidence, expected) &&
            !invalidEvidence.validity.expectedPixelsCompared,
        "pixel envelope cannot rescue invalid structural evidence");
  auto brokenConservation = mismatch(adjacent);
  ++brokenConservation.conservation.objectsCreated;
  check(!applyRenderTapePixelOracleEnvelope(brokenConservation, expected) &&
            !brokenConservation.validity.expectedPixelsCompared,
        "pixel envelope cannot rescue object conservation mismatch");

  check(renderTapeSourceOracleMatchesOutputEvidence(
            expected, expected, {}, {}, false),
        "an exact source oracle remains strict evidence");
  check(renderTapeSourceOracleMatchesOutputEvidence(
            expected, adjacent, expected, adjacent,
            accepted.validity.pixelEnvelopeMatched),
        "an authenticated output envelope transfers only across exact source/output equivalence");
  check(!renderTapeSourceOracleMatchesOutputEvidence(
            expected, adjacent, expected, adjacent, false),
        "source mismatch cannot relax without an accepted output envelope");
  auto differentExpectedSource = expected;
  differentExpectedSource[4] = std::byte{0x41};
  check(!renderTapeSourceOracleMatchesOutputEvidence(
            differentExpectedSource, adjacent, expected, adjacent, true),
        "different captured source/output domains cannot share an envelope");
  auto differentActualSource = adjacent;
  differentActualSource[4] = std::byte{0x41};
  check(!renderTapeSourceOracleMatchesOutputEvidence(
            expected, differentActualSource, expected, adjacent, true),
        "different replay source/output domains cannot share an envelope");
}

void standaloneD24X8SeedIsCreatedBeforeReplayAndConserved() {
  ProductionFixture fixture(false, true);
  const RenderTapeProviderBlob depthBlob{
      .digest = fixture.depthSeedDigest,
      .bytes = fixture.depthSeed,
  };
  const auto validation = preflightFrameTapeIdentity(
      fixture.tape, std::span(&depthBlob, 1u));
  check(validation.complete(),
        std::string(frameTapeReplayStatusName(validation.status)) +
            " failed_event=" + std::to_string(validation.failedEventIndex));
  check(validation.coverage.objectDefinitions == 2u &&
            validation.coverage.seedMutations == 1u &&
            validation.conservation.referencedBlobs == 1u,
        "D24X8 provider fixture preflights one exact canonical depth seed");
  const D9CPresentParams params{
      .backBufferWidth = validation.requirements.outputWidth,
      .backBufferHeight = validation.requirements.outputHeight,
      .backBufferFormat = validation.requirements.outputFormat,
      .backBufferCount = 1u,
      .swapEffect = 1u,
      .windowed = 1u,
      .presentationInterval = 0x80000000u,
  };
  auto* factory = dxmt9c_factory_create();
  check(factory != nullptr, "D24X8 seed fixture factory must be available");
  auto* device = dxmt9c_factory_create_device(factory, 0u, &params, 0u,
                                                nullptr);
  check(device != nullptr, "D24X8 seed fixture device must construct");
  const auto result = replayFrameTapeIdentity(
      device, fixture.tape, std::span(&depthBlob, 1u));
  dxmt9c_device_release(device);
  dxmt9c_factory_release(factory);
  check(result.complete(), frameTapeReplayStatusName(result.status));
  check(result.validity.outputReadback &&
            result.validity.expectedDigestCaptured &&
            result.validity.expectedDigestMatched,
        "D24X8 seeded provider replay preserves the deterministic output oracle");
  check(result.conservation.objectsCreated == 2u &&
            result.conservation.objectsReleased == 2u,
        "D24X8 provider replay conserves output and standalone depth wrappers");
}

void colorSnapshotCapturesExact2DAndAllCubeFaces() {
  const D9CPresentParams params{
      .backBufferWidth = 16u,
      .backBufferHeight = 16u,
      .backBufferFormat = 21u,
      .backBufferCount = 1u,
      .swapEffect = 1u,
      .windowed = 1u,
      .presentationInterval = 0x80000000u,
  };
  auto* factory = dxmt9c_factory_create();
  check(factory != nullptr, "color snapshot factory must be available");
  auto* device = dxmt9c_factory_create_device(factory, 0u, &params, 0u,
                                                nullptr);
  check(device != nullptr, "color snapshot device must construct");

  const auto exercise = [&](D9CTexture* texture, std::uint32_t format,
                            std::uint32_t resourceType,
                            std::uint32_t subresourceCount) {
    check(texture != nullptr, "snapshot texture must construct");
    D9CWireObjectIdentity identity{};
    check(dxmt9c_texture_get_wire_identity(texture, &identity) ==
              dxmt9::core::D3D_OK,
          "snapshot texture must expose exact wire identity");
    const D9CSurfaceDesc surface{
        .format = format,
        .resourceType = resourceType,
        .usage = 1u,
        .pool = 0u,
        .multiSampleType = 0u,
        .multiSampleQuality = 0u,
        .width = 16u,
        .height = 16u,
        .depth = 1u,
    };
    std::vector<std::vector<std::byte>> expected(
        subresourceCount, std::vector<std::byte>(16u * 16u * 4u));
    for (std::uint32_t subresource = 0u; subresource < subresourceCount;
         ++subresource) {
      const std::uint32_t red = 0x20u + subresource * 0x10u;
      const std::uint32_t color =
          0xff000000u | (red << 16u) | 0x00004060u;
      for (std::size_t offset = 0u; offset < expected[subresource].size();
           offset += 4u) {
        const std::uint32_t word = format == 114u
            ? std::bit_cast<std::uint32_t>(
                  static_cast<float>(red) / 255.0f)
            : color;
        std::memcpy(expected[subresource].data() + offset, &word,
                    sizeof(word));
      }
      auto* face = dxmt9c_texture_get_surface_level(texture, subresource);
      check(face != nullptr &&
                dxmt9c_device_color_fill(device, face, nullptr, color) ==
                    dxmt9::core::D3D_OK,
            "snapshot source face must receive a nontrivial GPU fill");
      dxmt9c_surface_release(face);

      std::vector<std::byte> captured(expected[subresource].size());
      const D9CRenderTapeColorSnapshotRequest request{
          .identity = identity,
          .surface = surface,
          .subresource = subresource,
          .encodingVersion = D9C_RENDER_TAPE_COLOR_ENCODING_TIGHT_V1,
      };
      D9CRenderTapeColorSnapshotResult result{};
      const auto snapshotHr = dxmt9c_device_capture_render_tape_color_snapshot(
          device, &request, &result, captured.data(), captured.size());
      const auto mismatch = std::mismatch(
          captured.begin(), captured.end(), expected[subresource].begin());
      const auto mismatchOffset = static_cast<std::size_t>(
          mismatch.first - captured.begin());
      check(snapshotHr == dxmt9::core::D3D_OK &&
                result.status == D9C_RENDER_TAPE_COLOR_SNAPSHOT_COMPLETE &&
                result.subresource == subresource && result.format == format &&
                result.pitch == 16u * 4u &&
                result.byteCount == captured.size() &&
                captured == expected[subresource],
            "snapshot readback mismatch format=" + std::to_string(format) +
                " subresource=" + std::to_string(subresource) +
                " hr=" + std::to_string(snapshotHr) +
                " status=" + std::to_string(result.status) +
                " offset=" + std::to_string(mismatchOffset) +
                " actual=" +
                std::to_string(mismatchOffset < captured.size()
                    ? std::to_integer<unsigned>(captured[mismatchOffset]) : 0u) +
                " expected=" +
                std::to_string(mismatchOffset < expected[subresource].size()
                    ? std::to_integer<unsigned>(
                          expected[subresource][mismatchOffset]) : 0u));
    }

    auto face6Request = D9CRenderTapeColorSnapshotRequest{
        .identity = identity,
        .surface = surface,
        .subresource = subresourceCount,
        .encodingVersion = D9C_RENDER_TAPE_COLOR_ENCODING_TIGHT_V1,
    };
    std::vector<std::byte> rejectedBytes(16u * 16u * 4u);
    D9CRenderTapeColorSnapshotResult rejected{};
    check(dxmt9c_device_capture_render_tape_color_snapshot(
              device, &face6Request, &rejected, rejectedBytes.data(),
              rejectedBytes.size()) != dxmt9::core::D3D_OK &&
              rejected.status != D9C_RENDER_TAPE_COLOR_SNAPSHOT_COMPLETE,
          "first out-of-range face rejects before readback");
    ++face6Request.identity.generation;
    face6Request.subresource = 0u;
    rejected = {};
    check(dxmt9c_device_capture_render_tape_color_snapshot(
              device, &face6Request, &rejected, rejectedBytes.data(),
              rejectedBytes.size()) != dxmt9::core::D3D_OK &&
              rejected.status ==
                  D9C_RENDER_TAPE_COLOR_SNAPSHOT_STALE_GENERATION,
          "stale generation rejects before texture access");
  };

  auto* texture2d = dxmt9c_device_create_texture(
      device, 16u, 16u, 1u, 1u, 22u, 0u);
  exercise(texture2d, 22u, 3u, 1u);
  dxmt9c_texture_release(texture2d);

  // GT2's atlas is a single-level MANAGED shader texture, not a render
  // target. The arm snapshot must read the GPU-visible bytes and preserve the
  // meaningful alpha byte rather than reusing a divergent PE seed.
  auto* managedTexture = dxmt9c_device_create_texture(
      device, 128u, 32u, 1u, 0u, 21u, 1u);
  check(managedTexture != nullptr,
        "managed atlas snapshot texture must construct");
  D9CWireObjectIdentity managedIdentity{};
  check(dxmt9c_texture_get_wire_identity(managedTexture, &managedIdentity) ==
            dxmt9::core::D3D_OK,
        "managed atlas exposes exact wire identity");
  auto* managedFace = dxmt9c_texture_get_surface_level(managedTexture, 0u);
  constexpr std::uint32_t managedColor = 0x7f406080u;
  check(managedFace != nullptr &&
            dxmt9c_device_color_fill(device, managedFace, nullptr,
                                     managedColor) == dxmt9::core::D3D_OK,
        "managed atlas receives a nontrivial GPU fill");
  dxmt9c_surface_release(managedFace);
  std::vector<std::byte> managedExpected(128u * 32u * 4u);
  for (std::size_t offset = 0u; offset < managedExpected.size(); offset += 4u)
    std::memcpy(managedExpected.data() + offset, &managedColor,
                sizeof(managedColor));
  std::vector<std::byte> managedPeSeed(managedExpected.size(),
                                       std::byte{0x11u});
  D9CRenderTapeColorSnapshotRequest managedRequest{
      .identity = managedIdentity,
      .surface = D9CSurfaceDesc{
          .format = 21u,
          .resourceType = 3u,
          .usage = 0u,
          .pool = 1u,
          .multiSampleType = 0u,
          .multiSampleQuality = 0u,
          .width = 128u,
          .height = 32u,
          .depth = 1u,
      },
      .subresource = 0u,
      .encodingVersion = D9C_RENDER_TAPE_COLOR_ENCODING_TIGHT_V1,
  };
  D9CRenderTapeColorSnapshotResult managedResult{};
  const auto managedHr = dxmt9c_device_capture_render_tape_color_snapshot(
      device, &managedRequest, &managedResult, managedPeSeed.data(),
      managedPeSeed.size());
  check(managedHr == dxmt9::core::D3D_OK &&
            managedResult.status ==
                D9C_RENDER_TAPE_COLOR_SNAPSHOT_COMPLETE &&
            managedResult.format == 21u &&
            managedResult.byteCount == managedPeSeed.size() &&
            managedPeSeed == managedExpected &&
            managedPeSeed[3] == std::byte{0x7fu},
        "managed arm snapshot overrides divergent PE seed and preserves alpha");
  dxmt9c_texture_release(managedTexture);

  auto* standalone = dxmt9c_device_create_render_target(
      device, 16u, 16u, 22u, 0u, 0u, 0u, nullptr);
  check(standalone != nullptr,
        "standalone color snapshot surface must construct");
  D9CWireObjectIdentity standaloneIdentity{};
  check(dxmt9c_surface_get_wire_identity(standalone, &standaloneIdentity) ==
            dxmt9::core::D3D_OK,
        "standalone color snapshot exposes exact wire identity");
  constexpr std::uint32_t standaloneColor = 0xff406080u;
  check(dxmt9c_device_color_fill(device, standalone, nullptr,
                                 standaloneColor) == dxmt9::core::D3D_OK,
        "standalone color snapshot source receives a nontrivial GPU fill");
  std::vector<std::byte> standaloneExpected(16u * 16u * 4u);
  for (std::size_t offset = 0u; offset < standaloneExpected.size();
       offset += 4u) {
    std::memcpy(standaloneExpected.data() + offset, &standaloneColor,
                sizeof(standaloneColor));
  }
  std::vector<std::byte> standaloneCaptured(standaloneExpected.size());
  D9CRenderTapeColorSnapshotRequest standaloneRequest{
      .identity = standaloneIdentity,
      .surface = D9CSurfaceDesc{
          .format = 22u,
          .resourceType = 1u,
          .usage = 1u,
          .pool = 0u,
          .multiSampleType = 0u,
          .multiSampleQuality = 0u,
          .width = 16u,
          .height = 16u,
          .depth = 1u,
      },
      .subresource = 0u,
      .encodingVersion = D9C_RENDER_TAPE_COLOR_ENCODING_TIGHT_V1,
  };
  D9CRenderTapeColorSnapshotResult standaloneResult{};
  const auto standaloneHr =
      dxmt9c_device_capture_render_tape_color_snapshot(
          device, &standaloneRequest, &standaloneResult,
          standaloneCaptured.data(), standaloneCaptured.size());
  check(standaloneHr == dxmt9::core::D3D_OK &&
            standaloneResult.status ==
                D9C_RENDER_TAPE_COLOR_SNAPSHOT_COMPLETE &&
            standaloneCaptured == standaloneExpected,
        "standalone X8R8G8B8 render target snapshots exact canonical bytes hr=" +
            std::to_string(standaloneHr) + " status=" +
            std::to_string(standaloneResult.status) + " format=" +
            std::to_string(standaloneResult.format));
  ++standaloneRequest.identity.generation;
  standaloneResult = {};
  check(dxmt9c_device_capture_render_tape_color_snapshot(
            device, &standaloneRequest, &standaloneResult,
            standaloneCaptured.data(), standaloneCaptured.size()) !=
            dxmt9::core::D3D_OK &&
            standaloneResult.status ==
                D9C_RENDER_TAPE_COLOR_SNAPSHOT_STALE_GENERATION,
        "stale standalone color generation rejects before surface access");
  dxmt9c_surface_release(standalone);

  auto* cube = dxmt9c_device_create_cube_texture(
      device, 16u, 1u, 1u, 114u, 0u);
  exercise(cube, 114u, 5u, 6u);
  dxmt9c_texture_release(cube);

  dxmt9c_device_release(device);
  dxmt9c_factory_release(factory);
}

void colorCompleteSeedsReplayAllSubresourcesAndConserve() {
  const auto descriptor = [](RenderTapeTextureDimension dimension,
                             std::uint32_t format) {
    const std::uint32_t count =
        dimension == RenderTapeTextureDimension::Cube ? 6u : 1u;
    const RenderTapeTextureDescriptorV2 header{
        .schemaVersion = kRenderTapeTextureDescriptorVersion2,
        .dimension = static_cast<std::uint32_t>(dimension),
        .mipLevelCount = 1u,
        .subresourceCount = count,
        .initialContentDisposition = static_cast<std::uint32_t>(
            RenderTapeInitialContentDisposition::CompleteSeed),
    };
    std::vector<std::byte> bytes(sizeof(header) +
                                 count * sizeof(D9CSurfaceDesc));
    std::memcpy(bytes.data(), &header, sizeof(header));
    for (std::uint32_t face = 0u; face < count; ++face) {
      const D9CSurfaceDesc surface{
          .format = format,
          .resourceType = dimension == RenderTapeTextureDimension::Cube ? 5u
                                                                         : 3u,
          .usage = 1u,
          .pool = 0u,
          .width = 16u,
          .height = 16u,
          .depth = 1u,
      };
      std::memcpy(bytes.data() + sizeof(header) + face * sizeof(surface),
                  &surface, sizeof(surface));
    }
    return bytes;
  };
  const auto texture2dDescriptor = descriptor(
      RenderTapeTextureDimension::Texture2D, 22u);
  const auto cubeDescriptor = descriptor(RenderTapeTextureDimension::Cube,
                                         114u);
  const RenderTapeSurfaceDescriptorV2 standaloneColorDescriptor{
      .schemaVersion = kRenderTapeSurfaceDescriptorVersion2,
      .storage = static_cast<std::uint32_t>(
          RenderTapeSurfaceStorage::Standalone),
      .initialContentDisposition = static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::CompleteSeed),
      .surface = D9CSurfaceDesc{
          .format = 22u,
          .resourceType = 1u,
          .usage = 1u,
          .pool = 0u,
          .width = 16u,
          .height = 16u,
          .depth = 1u,
      },
  };
  std::vector<std::vector<std::byte>> seeds;
  seeds.reserve(8u);
  for (std::uint32_t subresource = 0u; subresource < 8u; ++subresource) {
    seeds.emplace_back(16u * 16u * 4u,
                       static_cast<std::byte>(0x10u + subresource));
  }
  std::vector<RenderTapeProviderBlob> blobs;
  blobs.reserve(seeds.size());
  for (const auto& seed : seeds) {
    blobs.push_back({.digest = RenderTapeCaptureSession::sha256(seed),
                     .bytes = seed});
  }

  const D9CSurfaceDesc outputSurface{
      .format = 21u,
      .resourceType = 1u,
      .usage = 1u,
      .pool = 0u,
      .width = 16u,
      .height = 16u,
      .depth = 1u,
  };
  const auto output = outputDescriptor(outputSurface);
  const RenderTapeOracleAttachment oracle{
      .identity = kOutput,
      .descriptorKind = static_cast<std::uint32_t>(
          RenderTapeDescriptorKind::Surface),
  };
  const D9CCommandChunkWireClear clear{
      .flags = 1u,
      .colorARGB = 0xff204060u,
      .z = 1.0f,
      .rectOffset = sizeof(D9CCommandChunkWireClear),
  };
  const std::array records{
      Record{.type = D9C_COMMAND_RECORD_CLEAR, .payload = bytesOf(clear)},
      Record{.type = D9C_COMMAND_RECORD_PRESENT,
             .payload = bytesOf(D9CCommandChunkWirePresent{})},
  };
  const auto frame = makeChunk(records);
  const RenderTapeDigest expectedDigest{
      std::byte{0x5f}, std::byte{0x73}, std::byte{0x22}, std::byte{0xd0},
      std::byte{0x5f}, std::byte{0x8b}, std::byte{0xa9}, std::byte{0x74},
      std::byte{0x08}, std::byte{0x93}, std::byte{0xa4}, std::byte{0x70},
      std::byte{0x42}, std::byte{0x3e}, std::byte{0x69}, std::byte{0x2f},
      std::byte{0x05}, std::byte{0x7c}, std::byte{0x05}, std::byte{0x4f},
      std::byte{0xd0}, std::byte{0xbd}, std::byte{0x92}, std::byte{0xaa},
      std::byte{0x60}, std::byte{0x6c}, std::byte{0x44}, std::byte{0x40},
      std::byte{0x10}, std::byte{0x9d}, std::byte{0xca}, std::byte{0x61},
  };
  RenderTapeBuilder builder;
  builder.appendBootstrapState(implicitBootstrapChunk());
  builder.appendObjectDefine(
      kOutput, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
      std::as_bytes(std::span(&output, 1u)), 0u, {});
  builder.appendObjectDefine(
      kSnapshotTexture2D,
      static_cast<std::uint32_t>(RenderTapeDescriptorKind::Texture),
      texture2dDescriptor, 0u, {}, seeds[0].size(), 1u);
  builder.appendObjectDefine(
      kSnapshotCube,
      static_cast<std::uint32_t>(RenderTapeDescriptorKind::Texture),
      cubeDescriptor, 0u, {}, seeds[0].size() * 6u, 6u);
  builder.appendObjectDefine(
      kSnapshotColorSurface,
      static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
      std::as_bytes(std::span(&standaloneColorDescriptor, 1u)), 0u, {},
      seeds[7].size(), 1u);
  builder.appendResourceMutation(
      kSnapshotTexture2D, RenderTapeMutationKind::Upload, 0u, 0u,
      seeds[0].size(), blobs[0].digest);
  for (std::uint32_t face = 0u; face < 6u; ++face) {
    builder.appendResourceMutation(
        kSnapshotCube, RenderTapeMutationKind::Upload, face, 0u,
        seeds[face + 1u].size(), blobs[face + 1u].digest);
  }
  builder.appendResourceMutation(
      kSnapshotColorSurface, RenderTapeMutationKind::Upload, 0u, 0u,
      seeds[7].size(), blobs[7].digest);
  builder.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 2u, .handleCount = 0u}, frame);
  builder.appendPresentComplete(
      14u, 1u, RenderTapeDigestValidity::Sha256, expectedDigest,
      std::as_bytes(std::span(&oracle, 1u)));
  const auto tape = builder.seal();
  const auto preflight = preflightFrameTapeIdentity(tape, blobs);
  check(preflight.complete() && preflight.coverage.objectDefinitions == 4u &&
            preflight.coverage.seedMutations == 8u &&
            preflight.conservation.referencedBlobs == 8u,
        "provider preflight color seeds status=" +
            std::string(frameTapeReplayStatusName(preflight.status)) +
            " failed_event=" + std::to_string(preflight.failedEventIndex) +
            " definitions=" +
            std::to_string(preflight.coverage.objectDefinitions) +
            " mutations=" +
            std::to_string(preflight.coverage.seedMutations) +
            " blobs=" +
            std::to_string(preflight.conservation.referencedBlobs));

  const D9CPresentParams params{
      .backBufferWidth = 16u,
      .backBufferHeight = 16u,
      .backBufferFormat = 21u,
      .backBufferCount = 1u,
      .swapEffect = 1u,
      .windowed = 1u,
      .presentationInterval = 0x80000000u,
  };
  auto* factory = dxmt9c_factory_create();
  check(factory != nullptr, "color seed replay factory must be available");
  auto* device = dxmt9c_factory_create_device(factory, 0u, &params, 0u,
                                                nullptr);
  check(device != nullptr, "color seed replay device must construct");
  const auto result = replayFrameTapeIdentity(device, tape, blobs);
  dxmt9c_device_release(device);
  dxmt9c_factory_release(factory);
  check(result.complete(), frameTapeReplayStatusName(result.status));
  check(result.validity.outputReadback &&
            result.validity.expectedDigestCaptured &&
            result.validity.expectedDigestMatched,
        "color seed replay preserves the deterministic output oracle");
  check(result.conservation.objectsCreated == 4u &&
            result.conservation.objectsReleased == 4u,
        "color seed replay conserves output, 2D, cube, and standalone objects");
}

void writeProductionFixture(const std::filesystem::path& directory) {
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  check(!error, "provider fixture directory must be created");
  const ProductionFixture fixture;
  std::ofstream output(directory / "events.bin", std::ios::binary);
  check(output.good(), "provider fixture must open events.bin");
  output.write(reinterpret_cast<const char*>(fixture.tape.data()),
               static_cast<std::streamsize>(fixture.tape.size()));
  check(output.good(), "provider fixture must write events.bin");
}

void writeSequenceFixture(const std::filesystem::path& directory) {
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  check(!error, "sequence fixture directory must be created");
  const SequenceFixture fixture;
  std::ofstream events(directory / "events.bin", std::ios::binary);
  check(events.good(), "sequence fixture must open events.bin");
  events.write(reinterpret_cast<const char*>(fixture.tape.data()),
               static_cast<std::streamsize>(fixture.tape.size()));
  check(events.good(), "sequence fixture must write events.bin");
  std::ofstream first(directory / "first.bin", std::ios::binary);
  std::ofstream second(directory / "second.bin", std::ios::binary);
  first.write(reinterpret_cast<const char*>(fixture.firstSeed.data()),
              static_cast<std::streamsize>(fixture.firstSeed.size()));
  second.write(reinterpret_cast<const char*>(fixture.secondSeed.data()),
               static_cast<std::streamsize>(fixture.secondSeed.size()));
  check(first.good() && second.good(), "sequence fixture must write both blobs");
}

void writeParallelFixture(const std::filesystem::path& directory) {
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  check(!error, "parallel fixture directory must be created");
  const ParallelIndexedFixture fixture;
  std::ofstream events(directory / "events.bin", std::ios::binary);
  check(events.good(), "parallel fixture must open events.bin");
  events.write(reinterpret_cast<const char*>(fixture.tape.data()),
               static_cast<std::streamsize>(fixture.tape.size()));
  check(events.good(), "parallel fixture must write events.bin");
  constexpr std::array names{
      std::string_view{"vertex.bin"}, std::string_view{"index.bin"},
      std::string_view{"shader.bin"}, std::string_view{"declaration.bin"},
      std::string_view{"output.bin"},
  };
  check(fixture.blobsValue.size() == names.size(),
        "parallel fixture blob count is pinned");
  for (std::size_t index = 0u; index < names.size(); ++index) {
    std::ofstream blob(directory / std::string(names[index]), std::ios::binary);
    check(blob.good(), "parallel fixture must open a blob file");
    blob.write(reinterpret_cast<const char*>(fixture.blobsValue[index].bytes.data()),
               static_cast<std::streamsize>(fixture.blobsValue[index].bytes.size()));
    check(blob.good(), "parallel fixture must write every blob file");
  }
}

void eventLevelMutationDrainOrderingIsPinned() {
  check(!renderTapeProviderEventRequiresDrain(
            false, RenderTapeEventType::ResourceMutation) &&
            !renderTapeProviderEventRequiresDrain(
                true, RenderTapeEventType::CommandChunk) &&
            !renderTapeProviderEventRequiresDrain(
                true, RenderTapeEventType::ObjectDefine) &&
            renderTapeProviderEventRequiresDrain(
                true, RenderTapeEventType::ResourceMutation) &&
            renderTapeProviderEventRequiresDrain(
                true, RenderTapeEventType::ObjectDestroy),
        "mutation and retirement after submitted command work require a drain");
}

void bufferMutationReplayPlanPinsLockFlagsAndOffset() {
  constexpr D9CWireObjectIdentity buffer{
      .kind = D9C_CHUNK_HANDLE_KIND_BUFFER,
      .generation = 2u,
      .objectId = 0x7100u,
  };
  RenderTapeResourceMutationHeader mutation{
      .identity = buffer,
      .kind = static_cast<std::uint32_t>(RenderTapeMutationKind::CpuUnlock),
      .subresource = 0u,
      .byteOffset = 16u,
      .byteSize = 32u,
  };
  RenderTapeBufferMutationReplayPlan plan{};
  check(renderTapeBufferMutationReplayPlan(mutation, plan) &&
            plan.byteOffset == 16u && plan.byteSize == 32u &&
            plan.lockFlags == 0u,
        "plain buffer replay must preserve the exact byte range");

  mutation.bufferDisposition = static_cast<std::uint32_t>(
      RenderTapeBufferMutationDisposition::NoOverwrite);
  check(renderTapeBufferMutationReplayPlan(mutation, plan) &&
            plan.byteOffset == 16u && plan.byteSize == 32u &&
            plan.lockFlags == 0x1000u,
        "NoOverwrite replay must preserve offset and lock flag");

  mutation.byteOffset = 0u;
  mutation.bufferDisposition = static_cast<std::uint32_t>(
      RenderTapeBufferMutationDisposition::Discard);
  check(renderTapeBufferMutationReplayPlan(mutation, plan) &&
            plan.byteOffset == 0u && plan.byteSize == 32u &&
            plan.lockFlags == 0x2000u,
        "Discard replay must select the dynamic backing disposition");

  mutation.bufferDisposition = 99u;
  check(!renderTapeBufferMutationReplayPlan(mutation, plan),
        "unknown buffer disposition must not reach provider effects");
  mutation.bufferDisposition = static_cast<std::uint32_t>(
      RenderTapeBufferMutationDisposition::Plain);
  mutation.byteOffset = std::numeric_limits<std::uint64_t>::max();
  check(!renderTapeBufferMutationReplayPlan(mutation, plan),
        "unrepresentable buffer offset must fail closed");

  mutation.byteOffset = 16u;
  mutation.kind =
      static_cast<std::uint32_t>(RenderTapeMutationKind::Palette);
  check(!renderTapeBufferMutationReplayPlan(mutation, plan),
        "non-upload/unlock mutation must not reach buffer lock replay");
}

} // namespace

int main(int argc, char** argv) {
  try {
    if (argc == 2 && std::string_view(argv[1]) == "--test-event-ordering") {
      eventLevelMutationDrainOrderingIsPinned();
      return 0;
    }
    if (argc == 3 && std::string_view(argv[1]) == "--write-production-fixture") {
      writeProductionFixture(argv[2]);
      return 0;
    }
    if (argc == 3 && std::string_view(argv[1]) == "--write-sequence-fixture") {
      writeSequenceFixture(argv[2]);
      return 0;
    }
    if (argc == 3 && std::string_view(argv[1]) == "--write-parallel-fixture") {
      writeParallelFixture(argv[2]);
      return 0;
    }
    check(argc == 1,
          "usage: render_tape_provider_spec "
          "[--test-event-ordering|--write-production-fixture dir|"
          "--write-sequence-fixture dir|--write-parallel-fixture dir]");
    eventLevelMutationDrainOrderingIsPinned();
    bufferMutationReplayPlanPinsLockFlagsAndOffset();
    standaloneD24X8SeedIsCreatedBeforeReplayAndConserved();
    colorSnapshotCapturesExact2DAndAllCubeFaces();
    colorCompleteSeedsReplayAllSubresourcesAndConserve();
    acceptsBoundedIdentityGrammarAndReportsEvidence();
    canonicalUnsupportedDimensionsReturnTypedGrammar();
    failsClosedBeforeEffectsOnUnsupportedAndCorruptInputs();
    testProducedByCapturedPassTape();
    acceptsSplitTexturedUpGrammarAndRejectsNearMisses();
    productionPresenterMirrorGpuOracle();
    nativeMetalOffscreenIdentityReplay();
    nativeMetalTexturedUpDigestRepeats();
    generalIndexedReplayConservesAllObjects();
    productionParallelIndexedFixtureHasTwoChildren();
    boundedSequenceMutationIsVisibleAtSecondPresent();
    productionShapeUsesImplicitDefaultOutputAndExactDigest();
    productionShapeReportsWrongExpectedDigest();
    pixelOracleEnvelopeIsNarrowAndDigestAuthenticated();
  } catch (const TestFailure& error) {
    std::cerr << "render_tape_provider_spec failed: " << error.what() << '\n';
    return 1;
  }
  std::cout << "render_tape_provider_spec passed\n";
  return 0;
}
