#include "device_c_render_tape_capture.hpp"
#include "device_c_chunk_replay.hpp"
#include "device_c_common.hpp"
#include "device_c_render_tape_capture_layout.hpp"
#include "device_c_render_tape_identity.hpp"
#include "device_c_render_tape_provider.hpp"
#include "device_c_render_tape_source_plan.hpp"
#include "device_c_render_tape_runtime_objects.hpp"
#include "dxmt9/dxmt9_command_queue.hpp"
#include "dxmt9/dxmt9_device.hpp"
#include "dxmt9/dxmt9_perf_counters.hpp"
#include "dxmt9/dxmt9_presenter.hpp"
#include "dxmt9/dxmt9_resource_pool.hpp"

// R-HARN-REPLAY-7.6/7.7/7.8: bounded provider identity grammar,
// pre-effect failure, independent evidence, and native Metal output readback.

#include <array>
#include <algorithm>
#include <bit>
#include <chrono>
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
#include <utility>
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
constexpr D9CWireObjectIdentity kGpuVisiblePixelShader{
    .kind = D9C_CHUNK_HANDLE_KIND_SHADER,
    .generation = 5u,
    .objectId = 53u,
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

// The GPU policy fixture uses a patterned clear to make the readback
// non-degenerate while keeping the indexed draws clipped.  Its RGBA oracle is
// generated from these two independent colors, never from replay output.
constexpr std::uint32_t kGpuPolicyBaseColorARGB = 0xff102030u;
constexpr std::uint32_t kGpuPolicyInsetColorARGB = 0xff506070u;
constexpr std::uint32_t kGpuVisibleBackgroundColorARGB = 0xff101820u;
constexpr std::uint32_t kGpuVisibleColorAARGB = 0xffe04040u;
constexpr std::uint32_t kGpuVisibleColorBARGB = 0xff40e080u;

std::vector<std::byte> gpuPolicyExpectedPixels();
std::vector<std::byte> gpuVisibleUniformExpectedPixels(
    std::uint32_t backgroundARGB = kGpuVisibleBackgroundColorARGB);

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
    const D9CWireObjectIdentity& output,
    const D9CWireObjectIdentity& pixelShader = {},
    bool extraStream = false, bool volumeTexture = false) {
  std::array<D9CCommandChunkWireTextureBinding, D9C_DRAW_PACKET_MAX_TEXTURES>
      textures{};
  for (std::uint32_t slot = 0u; slot < textures.size(); ++slot) {
    textures[slot] = {.slot = slot,
                      .valid = 1u,
                      .handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX};
  }
  if (volumeTexture) textures[0].handleIndex = 6u;
  std::array<D9CCommandChunkWireStreamBinding, D9C_DRAW_PACKET_MAX_STREAMS>
      streams{};
  for (std::uint32_t slot = 0u; slot < streams.size(); ++slot) {
    streams[slot] = {
        .slot = slot,
        .valid = 1u,
        .handleIndex = slot == 0u ? 0u : D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX,
    };
  }
  if (pixelShader.kind != 0u)
    streams[0].stride = static_cast<std::uint32_t>(sizeof(TexturedVertex));
  if (extraStream) {
    streams[1].handleIndex = 0u;
    streams[1].offset = 12u * sizeof(TexturedVertex);
    streams[1].stride = sizeof(TexturedVertex);
  }
  std::vector<D9CCommandChunkWireShaderBinding> shaders{
      D9CCommandChunkWireShaderBinding{
          .stage = D9C_COMMAND_CHUNK_SHADER_STAGE_VERTEX,
          .valid = 1u,
          .handleIndex = 1u,
      },
  };
  if (pixelShader.kind != 0u) {
    shaders.push_back(D9CCommandChunkWireShaderBinding{
        .stage = D9C_COMMAND_CHUNK_SHADER_STAGE_PIXEL,
        .valid = 1u,
        .handleIndex = 2u,
    });
  }
  const D9CCommandChunkWireVertexInput input{
      .valid = 1u,
      .kind = D9C_COMMAND_CHUNK_VERTEX_INPUT_DECLARATION,
      .value = 0u,
      .handleIndex = static_cast<std::uint32_t>(2u + (pixelShader.kind != 0u)),
  };
  const D9CCommandChunkWireIndexBinding index{
      .valid = 1u,
      .handleIndex = static_cast<std::uint32_t>(3u + (pixelShader.kind != 0u)),
  };
  const std::array renderTargets{D9CCommandChunkWireRenderTargetBinding{
      .slot = 0u,
      .valid = 1u,
      .handleIndex = static_cast<std::uint32_t>(4u + (pixelShader.kind != 0u)),
  }};
  const D9CViewport visibleViewport{.x = 0u, .y = 0u, .width = 16u,
                                    .height = 16u, .minZ = 0.f, .maxZ = 1.f};
  const std::array visibleRenderStates{
      D9CCommandChunkWireRenderState{.state = 7u, .value = 0u}, // ZENABLE
      D9CCommandChunkWireRenderState{.state = 14u, .value = 0u}, // ZWRITEENABLE
      D9CCommandChunkWireRenderState{.state = 15u, .value = 0u}, // ALPHATESTENABLE
      D9CCommandChunkWireRenderState{.state = 22u, .value = 1u}, // CULLMODE NONE
      D9CCommandChunkWireRenderState{.state = 27u, .value = 0u}, // ALPHABLENDENABLE
      D9CCommandChunkWireRenderState{.state = 168u, .value = 15u}, // COLORWRITEENABLE
      D9CCommandChunkWireRenderState{.state = 174u, .value = 0u}, // SCISSORTESTENABLE
  };
  const std::array volumeSampler{
      D9CDrawPacketSamplerState{0u, 1u, 3u}, // ADDRESSU CLAMP
      D9CDrawPacketSamplerState{0u, 2u, 3u}, // ADDRESSV CLAMP
      D9CDrawPacketSamplerState{0u, 3u, 3u}, // ADDRESSW CLAMP
      D9CDrawPacketSamplerState{0u, 5u, 1u}, // MAGFILTER POINT
      D9CDrawPacketSamplerState{0u, 6u, 1u}, // MINFILTER POINT
      D9CDrawPacketSamplerState{0u, 7u, 1u}, // MIPFILTER POINT
  };
  std::vector<std::pair<std::uint16_t, std::span<const std::byte>>> sections{
      std::pair{static_cast<std::uint16_t>(D9C_COMMAND_CHUNK_SECTION_TEXTURE),
                spanBytes(textures)},
      std::pair{static_cast<std::uint16_t>(D9C_COMMAND_CHUNK_SECTION_STREAM),
                spanBytes(streams)},
      std::pair{static_cast<std::uint16_t>(D9C_COMMAND_CHUNK_SECTION_SHADER),
                std::span<const std::byte>(reinterpret_cast<const std::byte*>(
                    shaders.data()), shaders.size() * sizeof(shaders[0]))},
      std::pair{static_cast<std::uint16_t>(D9C_COMMAND_CHUNK_SECTION_VERTEX_INPUT),
                spanBytes(input)},
      std::pair{static_cast<std::uint16_t>(D9C_COMMAND_CHUNK_SECTION_INDEX_BUFFER),
                spanBytes(index)},
      std::pair{static_cast<std::uint16_t>(D9C_COMMAND_CHUNK_SECTION_RENDER_TARGET),
                spanBytes(renderTargets)},
  };
  if (pixelShader.kind != 0u) {
    sections.emplace_back(D9C_COMMAND_CHUNK_SECTION_RENDER_STATE,
                          spanBytes(visibleRenderStates));
    sections.emplace_back(D9C_COMMAND_CHUNK_SECTION_VIEWPORT,
                          spanBytes(visibleViewport));
    if (volumeTexture)
      sections.emplace_back(D9C_COMMAND_CHUNK_SECTION_SAMPLER_STATE,
                            spanBytes(volumeSampler));
    std::sort(sections.begin(), sections.end(),
              [](const auto& left, const auto& right) {
                return left.first < right.first;
              });
  }
  const auto payload = sparsePayload(
      D9CCommandChunkWireDrawHeader{
          .flags = D9C_COMMAND_CHUNK_DRAW_FLAG_FULL_SNAPSHOT},
      sections);
  return makeChunk(std::array{Record{
      .type = D9C_COMMAND_RECORD_APPLY_STATE,
      .payload = payload,
      .handles = [&] {
        std::vector<D9CCommandChunkWireHandleEntry> handles{
            {vertexBuffer.kind, vertexBuffer.generation, vertexBuffer.objectId},
            {vertexShader.kind, vertexShader.generation, vertexShader.objectId},
        };
        if (pixelShader.kind != 0u)
          handles.push_back({pixelShader.kind, pixelShader.generation,
                             pixelShader.objectId});
        handles.push_back({vertexDeclaration.kind, vertexDeclaration.generation,
                           vertexDeclaration.objectId});
        handles.push_back({indexBuffer.kind, indexBuffer.generation,
                           indexBuffer.objectId});
        handles.push_back({output.kind, output.generation, output.objectId});
        if (volumeTexture)
          handles.push_back({kTexture.kind, kTexture.generation, kTexture.objectId});
        return handles;
      }(),
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

std::vector<std::byte> gpuVisibleVertexShader() {
  // vs_2_0: dcl_position v0; mul oPos, v0, c0.  DCL has both a semantic
  // token and a destination token; v0 is an input register (0x900f0000),
  // while the source operand carries the explicit xyzw swizzle (0x90e40000).
  // MUL has three operands, so its instruction token length is three.
  const std::array<std::uint32_t, 9u> words{
      0xfffe0200u, 0x0200001fu, 0x80000000u, 0x900f0000u,
      0x03000005u, 0xc00f0000u, 0x90e40000u, 0xa0e40000u,
      0x0000ffffu};
  return std::vector<std::byte>(
      reinterpret_cast<const std::byte*>(words.data()),
      reinterpret_cast<const std::byte*>(words.data()) + sizeof(words));
}

std::vector<std::byte> gpuVisiblePixelShader() {
  // ps_2_0: mov oC0, c0.
  const std::array<std::uint32_t, 5u> words{
      0xffff0200u, 0x02000001u, 0x800f0800u, 0xa0e40000u, 0x0000ffffu};
  return std::vector<std::byte>(
      reinterpret_cast<const std::byte*>(words.data()),
      reinterpret_cast<const std::byte*>(words.data()) + sizeof(words));
}

std::vector<std::byte> gpuVolumePixelShader() {
  // ps_3_0: dcl_volume s0; texldl r0, c0, s0; mov oC0, r0.
  // Per-draw c0 selects both the z slice and exact mip, independent of UVs.
  const std::array<std::uint32_t, 12u> words{
      0xffff0300u, 0x0200001fu, 0xa0000000u, 0xa00f0800u,
      0x0300005fu, 0x800f0000u, 0xa0e40000u, 0xa0e40800u,
      0x02000001u, 0x800f0800u, 0x80e40000u, 0x0000ffffu};
  return {reinterpret_cast<const std::byte*>(words.data()),
          reinterpret_cast<const std::byte*>(words.data()) + sizeof(words)};
}

std::vector<std::byte> gpuVisibleUniformDrawPayload(
    std::uint32_t startIndex, std::array<float, 4u> transform,
    std::array<float, 4u> color) {
  const D9CCommandChunkWireDrawHeader draw{
      .primitiveType = 4u,
      .minVertex = (startIndex / 6u) * 4u,
      .numVertices = 4u,
      .startIndex = startIndex,
      .primitiveCount = 2u,
  };
  const D9CCommandChunkWireConstantRange range{
      .startRegister = 0u,
      .registerCount = 1u,
  };
  std::array<std::byte, sizeof(range) + sizeof(transform)> vsConstants{};
  std::memcpy(vsConstants.data(), &range, sizeof(range));
  std::memcpy(vsConstants.data() + sizeof(range), transform.data(),
              sizeof(transform));
  std::array<std::byte, sizeof(range) + sizeof(color)> psConstants{};
  std::memcpy(psConstants.data(), &range, sizeof(range));
  std::memcpy(psConstants.data() + sizeof(range), color.data(), sizeof(color));
  const std::array sections{
      std::pair{static_cast<std::uint16_t>(D9C_COMMAND_CHUNK_SECTION_VS_CONST_F),
                std::span<const std::byte>(vsConstants)},
      std::pair{static_cast<std::uint16_t>(D9C_COMMAND_CHUNK_SECTION_PS_CONST_F),
                std::span<const std::byte>(psConstants)},
  };
  return sparsePayload(draw, sections);
}

std::vector<std::byte> gpuVisibleUniformDrawRunChunk(bool alteredMiddleColor,
                                                    bool alteredMiddleTransform,
                                                    bool extraStream = false,
                                                    bool volumeTexture = false) {
  const auto bootstrap = generalBootstrapChunk(
      kGeneralVertexBuffer, kGeneralIndexBuffer, kGeneralVertexShader,
      kGeneralVertexDeclaration, kOutput, kGpuVisiblePixelShader, extraStream,
      volumeTexture);
  ImportedChunkView bootstrapView;
  check(importPrevalidatedCommandChunk(
            bootstrap,
            CommandChunkEnvelope{.recordCount = 1u,
                                 .handleCount = volumeTexture ? 7u : 6u},
            bootstrapView),
        "visible-uniform fixture imports its FULL_SNAPSHOT anchor");
  const auto anchor = bootstrapView.record(0u);
  std::vector<Record> records;
  records.reserve(4u);
  records.push_back(Record{
      .type = anchor.header.type,
      .payload = std::vector<std::byte>(anchor.payload.begin(), anchor.payload.end()),
      .handles = std::vector<D9CCommandChunkWireHandleEntry>(
          bootstrapView.handles.begin(), bootstrapView.handles.end()),
  });
  constexpr std::array<float, 4u> transformA{1.f, 1.f, 1.f, 1.f};
  constexpr std::array<float, 4u> transformB{0.5f, 1.f, 1.f, 1.f};
  constexpr std::array<float, 4u> colorA{0.88f, 0.25f, 0.25f, 1.f};
  constexpr std::array<float, 4u> colorB{0.25f, 0.88f, 0.5f, 1.f};
  const auto middleTransform = alteredMiddleTransform ? transformA : transformB;
  const std::array transforms{transformA, middleTransform, transformA};
  const std::array colors{colorA, alteredMiddleColor ?
      std::array<float, 4u>{0.25f, 0.25f, 0.95f, 1.f} : colorB, colorA};
  const std::array volumeCoordinates{
      std::array<float, 4u>{.5f, .5f, 1.f / 6.f, 0.f},
      std::array<float, 4u>{.5f, .5f, 5.f / 6.f, 0.f},
      std::array<float, 4u>{.5f, .5f, .5f, 1.f},
  };
  for (std::uint32_t draw = 0u; draw < 3u; ++draw) {
    records.push_back(Record{
        .type = D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE,
        .payload = gpuVisibleUniformDrawPayload(draw * 6u, transforms[draw],
            volumeTexture ? volumeCoordinates[draw] : colors[draw]),
    });
  }
  return makeChunk(records);
}

// Keep the production-parallel fixture in one canonical command chunk.  The
// first record is the same FULL_SNAPSHOT state anchor used by the production
// indexed path; the remainder is two indivisible indexed DrawRuns.  The first
// half uses the baseline PSO/uniform identity.  The second half uses a
// color-write topology/render-state transition and a pixel-inert VS constant
// range: the production economics identities change while the degenerate
// pixels do not.
std::vector<std::byte> indexedDrawPayload(std::uint32_t primitiveType,
                                          bool alternateUniform,
                                          bool explicitUniform = false) {
  const D9CCommandChunkWireDrawHeader draw{
      .primitiveType = primitiveType,
      .minVertex = 0u,
      .numVertices = 3u,
      .startIndex = 0u,
      .primitiveCount = 1u,
      .sectionTableOffset = sizeof(D9CCommandChunkWireDrawHeader),
      .sectionPayloadOffset = sizeof(D9CCommandChunkWireDrawHeader),
  };
  if (!alternateUniform && !explicitUniform) return bytesOf(draw);
  const D9CCommandChunkWireConstantRange range{
      .startRegister = 0u,
      .registerCount = 1u,
  };
  // The fixture shader moves c0 to oPos. Keeping w=0 makes every B draw
  // clipped/degenerate like A while changing the production uniform identity.
  const std::array<float, 4u> values = alternateUniform
      ? std::array<float, 4u>{1.0f, 1.0f, 1.0f, 0.0f}
      : std::array<float, 4u>{0.0f, 0.0f, 0.0f, 0.0f};
  std::vector<std::byte> constants(sizeof(range) + sizeof(values));
  std::memcpy(constants.data(), &range, sizeof(range));
  std::memcpy(constants.data() + sizeof(range), values.data(), sizeof(values));
  const auto renderState = bytesOf(D9CCommandChunkWireRenderState{
      .state = 168u, // D3DRS_COLORWRITEENABLE
      .value = alternateUniform ? 0u : 0xfu,
  });
  const std::array sections{
      std::pair{static_cast<std::uint16_t>(D9C_COMMAND_CHUNK_SECTION_RENDER_STATE),
                std::span<const std::byte>(renderState)},
      std::pair{static_cast<std::uint16_t>(D9C_COMMAND_CHUNK_SECTION_VS_CONST_F),
                std::span<const std::byte>(constants)},
  };
  return sparsePayload(draw, sections);
}

std::vector<std::byte> generalIndexedDrawRunChunk(std::uint32_t drawCount,
                                                 bool alternating = false) {
  check(drawCount >= 2u && drawCount <= 128u && (drawCount % 2u) == 0u,
        "indexed draw run must contain two even bounded children");
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
  const auto midpoint = drawCount / 2u;
  for (std::uint32_t index = 0u; index < drawCount; ++index) {
    const bool alternate = alternating ? (index % 2u) != 0u : index >= midpoint;
    records.push_back(Record{
        .type = D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE,
        .payload = indexedDrawPayload(alternate ? 5u : 4u, alternate, alternating),
    });
  }
  return makeChunk(records);
}

struct GeneralIndexedFixture {
  std::vector<std::byte> vertexBytes{};
  std::vector<std::byte> indexBytes{};
  std::vector<std::byte> shaderBytes{};
  std::vector<std::byte> declarationBytes{};
  std::vector<std::byte> pixelShaderBytes{};
  std::vector<std::byte> outputSeed{};
  std::vector<std::byte> auditTextureSeed{};
  std::array<std::vector<std::byte>, 3u> volumeSeeds{};
  std::vector<RenderTapeProviderBlob> blobsValue{};
  std::vector<std::byte> tape{};

  explicit GeneralIndexedFixture(std::uint32_t drawCount = 1u,
                                 bool strictDigest = false,
                                 bool patternedClear = false,
                                 bool visibleUniform = false,
                                 bool alteredMiddleUniform = false,
                                 bool alteredMiddleTransform = false,
                                 std::uint32_t auditTextureFormat = 0u,
                                 bool extraStream = false,
                                 bool alteredExtraStream = false,
                                 bool queryAware = false,
                                 std::uint32_t backgroundARGB = kGpuVisibleBackgroundColorARGB,
                                 bool volumeTexture = false,
                                 bool queryPendingOnly = false) {
    check(!queryAware || (visibleUniform && drawCount == 3u && strictDigest),
          "query fixture uses three visible draws and an exact original pixel oracle");
    check(!volumeTexture || (visibleUniform && drawCount == 3u && strictDigest &&
                            auditTextureFormat == 0u),
          "volume fixture uses an exact visible shader oracle without a 2D audit seed");
    check(!queryPendingOnly || queryAware,
          "Pending-only query fixture carries an authenticated query transcript");
    // Overscan vertically so the uniform-binding oracle does not depend on
    // exact top/bottom edge ties after D3D9 half-pixel correction.
    const std::array visibleVertices{
        TexturedVertex{-1.0f, -1.25f, 0.0f, 1.0f, 0xffffffffu, 0.0f, 0.0f},
        TexturedVertex{-0.5f, -1.25f, 0.0f, 1.0f, 0xffffffffu, 1.0f, 0.0f},
        TexturedVertex{-0.5f, 1.25f, 0.0f, 1.0f, 0xffffffffu, 1.0f, 1.0f},
        TexturedVertex{-1.0f, 1.25f, 0.0f, 1.0f, 0xffffffffu, 0.0f, 1.0f},
        TexturedVertex{-0.5f, -1.25f, 0.0f, 1.0f, 0xffffffffu, 0.0f, 0.0f},
        TexturedVertex{0.5f, -1.25f, 0.0f, 1.0f, 0xffffffffu, 1.0f, 0.0f},
        TexturedVertex{0.5f, 1.25f, 0.0f, 1.0f, 0xffffffffu, 1.0f, 1.0f},
        TexturedVertex{-0.5f, 1.25f, 0.0f, 1.0f, 0xffffffffu, 0.0f, 1.0f},
        TexturedVertex{0.5f, -1.25f, 0.0f, 1.0f, 0xffffffffu, 0.0f, 0.0f},
        TexturedVertex{1.0f, -1.25f, 0.0f, 1.0f, 0xffffffffu, 1.0f, 0.0f},
        TexturedVertex{1.0f, 1.25f, 0.0f, 1.0f, 0xffffffffu, 1.0f, 1.0f},
        TexturedVertex{0.5f, 1.25f, 0.0f, 1.0f, 0xffffffffu, 0.0f, 1.0f},
    };
    const std::array indexedVertices{
        TexturedVertex{-0.5f, -0.5f, 0.0f, 1.0f, 0xffff0000u, 0.0f, 0.0f},
        TexturedVertex{0.5f, -0.5f, 0.0f, 1.0f, 0xff00ff00u, 1.0f, 0.0f},
        TexturedVertex{0.0f, 0.5f, 0.0f, 1.0f, 0xff0000ffu, 0.5f, 1.0f},
    };
    const auto selectedVertices = visibleUniform
        ? std::span<const TexturedVertex>(visibleVertices)
        : std::span<const TexturedVertex>(indexedVertices);
    vertexBytes.assign(reinterpret_cast<const std::byte*>(selectedVertices.data()),
                       reinterpret_cast<const std::byte*>(selectedVertices.data()) +
                           selectedVertices.size_bytes());
    if (extraStream) {
      check(visibleUniform, "extra stream requires visible uniform fixture");
      auto extra = visibleVertices;
      // Stream zero is unchanged even for the negative. POSITION comes from
      // the second half through actual Metal slot 6 at a nonzero bind offset.
      if (alteredExtraStream)
        for (auto& vertex : extra) vertex.x = 0.f;
      const auto bytes = std::as_bytes(std::span(extra));
      vertexBytes.insert(vertexBytes.end(), bytes.begin(), bytes.end());
    }
    const std::array<std::uint16_t, 18u> visibleIndices{
        0u, 1u, 2u, 0u, 2u, 3u, 4u, 5u, 6u,
        4u, 6u, 7u, 8u, 9u, 10u, 8u, 10u, 11u};
    const std::array<std::uint16_t, 3u> indexedIndices{0u, 1u, 2u};
    const auto selectedIndices = visibleUniform
        ? std::span<const std::uint16_t>(visibleIndices)
        : std::span<const std::uint16_t>(indexedIndices);
    indexBytes.assign(reinterpret_cast<const std::byte*>(selectedIndices.data()),
                      reinterpret_cast<const std::byte*>(selectedIndices.data()) +
                          selectedIndices.size_bytes());
    const std::array<std::uint32_t, 8u> shaderWords{
        0xfffe0300u, 0x0200001fu, 0x80000000u, 0xc00f0000u,
        0x02000001u, 0xc00f0000u, 0xa0e40000u, 0x0000ffffu,
    };
    shaderBytes = visibleUniform ? gpuVisibleVertexShader() : std::vector<std::byte>(
        reinterpret_cast<const std::byte*>(shaderWords.data()),
        reinterpret_cast<const std::byte*>(shaderWords.data()) + sizeof(shaderWords));
    if (visibleUniform) pixelShaderBytes = volumeTexture
        ? gpuVolumePixelShader() : gpuVisiblePixelShader();
    const std::array declaration{
        // The legacy clipped fixture intentionally uses POSITIONT.  The
        // visible shader binds POSITION, so keep that fixture's declaration
        // semantically aligned without changing the existing fixture.
        D9CVertexElement{static_cast<std::uint16_t>(extraStream ? 1u : 0u), 0u, 3u, 0u,
                         static_cast<std::uint8_t>(visibleUniform ? 0u : 9u),
                         0u},
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
    const auto pixelShaderDigest =
        RenderTapeCaptureSession::sha256(pixelShaderBytes);
    outputSeed.assign(16u * 16u * 4u, std::byte{0x3cu});
    if (visibleUniform) {
      const std::array background{
          std::byte(backgroundARGB & 0xffu), std::byte((backgroundARGB >> 8u) & 0xffu),
          std::byte((backgroundARGB >> 16u) & 0xffu), std::byte{0xff}};
      for (std::size_t offset = 0u; offset < outputSeed.size(); offset += 4u)
        std::copy(background.begin(), background.end(), outputSeed.begin() + offset);
    }
    const auto outputSeedDigest =
        RenderTapeCaptureSession::sha256(outputSeed);
    blobsValue = {
        {.digest = vertexDigest, .bytes = vertexBytes},
        {.digest = indexDigest, .bytes = indexBytes},
        {.digest = shaderDigest, .bytes = shaderBytes},
        {.digest = declarationDigest, .bytes = declarationBytes},
        {.digest = outputSeedDigest, .bytes = outputSeed},
    };
    if (visibleUniform) {
      const RenderTapeProviderBlob pixelBlob{
          .digest = pixelShaderDigest, .bytes = pixelShaderBytes};
      blobsValue.insert(blobsValue.begin() + 3, pixelBlob);
    }

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
    const RenderTapeShaderDescriptor pixelShader{
        .stage = D9C_COMMAND_CHUNK_SHADER_STAGE_PIXEL,
        .bytecodeBytes = static_cast<std::uint32_t>(pixelShaderBytes.size()),
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
    std::vector<Record> clearRecords;
    clearRecords.push_back(Record{
        .type = D9C_COMMAND_RECORD_CLEAR,
        .payload = bytesOf(D9CCommandChunkWireClear{
            .flags = 1u,
            .colorARGB = visibleUniform
                ? kGpuVisibleBackgroundColorARGB
                : patternedClear ? kGpuPolicyBaseColorARGB : 0xff102030u,
            .z = 1.0f,
            .rectOffset = sizeof(D9CCommandChunkWireClear),
        }),
    });
    if (patternedClear) {
      const D9CCommandChunkWireClear inset{
          .flags = 1u,
          .colorARGB = kGpuPolicyInsetColorARGB,
          .z = 1.0f,
          .rectCount = 1u,
          .rectOffset = sizeof(D9CCommandChunkWireClear),
      };
      std::vector<std::byte> insetBytes = bytesOf(inset);
      const D9CRect rect{4, 4, 12, 12};
      const auto rectBytes = std::as_bytes(std::span(&rect, 1u));
      insetBytes.insert(insetBytes.end(), rectBytes.begin(), rectBytes.end());
      clearRecords.push_back(Record{
          .type = D9C_COMMAND_RECORD_CLEAR,
          .payload = std::move(insetBytes),
      });
    }
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
        kGeneralVertexDeclaration, kOutput,
        visibleUniform ? kGpuVisiblePixelShader : D9CWireObjectIdentity{}, extraStream,
        volumeTexture));
    builder.appendObjectDefine(
        kOutput, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
        std::as_bytes(std::span(&output, 1u)), 0u, {}, outputSeed.size(), 1u);
    if (volumeTexture) {
      const RenderTapeTextureDescriptorV2 header{
          .dimension = static_cast<std::uint32_t>(RenderTapeTextureDimension::Volume),
          .mipLevelCount = 3u, .subresourceCount = 3u,
          .initialContentDisposition = static_cast<std::uint32_t>(
              RenderTapeInitialContentDisposition::CompleteSeed)};
      std::array<D9CSurfaceDesc, 3u> mips{};
      const std::array extents{std::array{7u, 5u, 3u}, std::array{3u, 2u, 1u},
                               std::array{1u, 1u, 1u}};
      constexpr std::array<std::uint32_t, 3u> sliceColors{
          kGpuVisibleColorAARGB, 0xff2040c0u, kGpuVisibleColorBARGB};
      std::uint64_t total = 0u;
      for (std::uint32_t mip = 0u; mip < mips.size(); ++mip) {
        mips[mip] = {.format = 21u, .resourceType = 4u,
            .width = extents[mip][0], .height = extents[mip][1],
            .depth = extents[mip][2]};
        auto& seed = volumeSeeds[mip];
        seed.resize(mips[mip].width * mips[mip].height * mips[mip].depth * 4u);
        for (std::uint32_t z = 0u; z < mips[mip].depth; ++z) {
          const auto color = mip == 0u ? sliceColors[z]
              : mip == 1u ? kGpuVisibleColorAARGB : 0xffc04020u;
          for (std::uint32_t texel = 0u; texel < mips[mip].width * mips[mip].height;
               ++texel) {
            const auto offset = (z * mips[mip].width * mips[mip].height + texel) * 4u;
            std::memcpy(seed.data() + offset, &color, sizeof(color));
          }
        }
        total += seed.size();
      }
      std::vector<std::byte> descriptor = bytesOf(header);
      const auto mipBytes = spanBytes(mips);
      descriptor.insert(descriptor.end(), mipBytes.begin(), mipBytes.end());
      builder.appendObjectDefine(kTexture,
          static_cast<std::uint32_t>(RenderTapeDescriptorKind::Texture),
          descriptor, 0u, {}, total, 3u);
      for (std::uint32_t mip = 0u; mip < mips.size(); ++mip) {
        const auto digest = RenderTapeCaptureSession::sha256(volumeSeeds[mip]);
        blobsValue.push_back({.digest = digest, .bytes = volumeSeeds[mip]});
        builder.appendResourceMutation(kTexture, RenderTapeMutationKind::Upload,
            mip, 0u, volumeSeeds[mip].size(), digest);
      }
    }
    if (auditTextureFormat != 0u) {
      const std::array<float, 4u> values{0.25f, 0.5f, 0.75f, 1.0f};
      const auto bytes = std::as_bytes(std::span(values));
      auditTextureSeed.assign(bytes.begin(), bytes.end());
      const auto digest = RenderTapeCaptureSession::sha256(auditTextureSeed);
      blobsValue.push_back({.digest = digest, .bytes = auditTextureSeed});
      const auto descriptor = texture2DDescriptor(D9CSurfaceDesc{
          .format = auditTextureFormat, .resourceType = 3u, .usage = 1u,
          .width = 2u, .height = 2u, .depth = 1u});
      builder.appendObjectDefine(kTexture,
          static_cast<std::uint32_t>(RenderTapeDescriptorKind::Texture),
          descriptor, 0u, {}, bytes.size(), 1u);
      builder.appendResourceMutation(kTexture, RenderTapeMutationKind::Upload,
          0u, 0u, bytes.size(), digest);
    }
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
    if (visibleUniform) {
      builder.appendObjectDefine(
          kGpuVisiblePixelShader,
          static_cast<std::uint32_t>(RenderTapeDescriptorKind::Shader),
          std::as_bytes(std::span(&pixelShader, 1u)), pixelShaderBytes.size(),
          pixelShaderDigest);
    }
    builder.appendObjectDefine(
        kGeneralVertexDeclaration,
        static_cast<std::uint32_t>(RenderTapeDescriptorKind::VertexDeclaration),
        std::as_bytes(std::span(&vertexDeclaration, 1u)),
        declarationBytes.size(), declarationDigest);
    constexpr D9CWireObjectIdentity queryIdentity{
        D9C_CHUNK_HANDLE_KIND_QUERY, 19u, 95u};
    if (queryAware) {
      const RenderTapeQueryDescriptorV2 descriptor{.type = 9u, .dataBytes = 4u};
      builder.appendObjectDefine(queryIdentity,
          static_cast<std::uint32_t>(RenderTapeDescriptorKind::Query),
          spanBytes(descriptor), 0u, {});
    }
    const auto appendIssue = [&](std::uint32_t flags) {
      const auto chunk = makeChunk(std::array{Record{
          .type = D9C_COMMAND_RECORD_QUERY_ISSUE,
          .payload = bytesOf(D9CCommandChunkWireQueryIssue{
              .queryHandleIndex = 0u, .flags = flags}),
          .handles = {{queryIdentity.kind, queryIdentity.generation,
                       queryIdentity.objectId}},
      }});
      builder.appendCommandChunk(
          CommandChunkEnvelope{.recordCount = 1u, .handleCount = 1u}, chunk);
    };
    const auto appendPoll = [&](std::uint64_t cycle, std::int32_t result,
                                std::uint32_t samples, std::uint32_t flags) {
      RenderTapeQueryGetDataControlV2 control{
          .flags = flags, .hasOutput = 1u,
          .definedBytes = result == 0 ? 4u : 0u,
          .dataSize = 4u, .issueCycle = cycle,
      };
      if (control.definedBytes) std::memcpy(control.result.data(), &samples, 4u);
      builder.appendOrderedControl(RenderTapeOrderedControlHeader{
          .identity = queryIdentity,
          .kind = static_cast<std::uint32_t>(RenderTapeControlKind::QueryGetData),
          .disposition = static_cast<std::uint32_t>(result == 0
              ? RenderTapeControlDisposition::Completed
              : RenderTapeControlDisposition::Pending),
          .resultCode = result, .controlBytes = sizeof(control),
      }, spanBytes(control));
    };
    builder.appendResourceMutation(
        kGeneralVertexBuffer, RenderTapeMutationKind::Upload, 0u, 0u,
        vertexBytes.size(), vertexDigest);
    builder.appendResourceMutation(
        kGeneralIndexBuffer, RenderTapeMutationKind::Upload, 0u, 0u,
        indexBytes.size(), indexDigest);
    builder.appendResourceMutation(
        kOutput, RenderTapeMutationKind::Upload, 0u, 0u,
        outputSeed.size(), outputSeedDigest);
    // The visible binding fixture starts from a complete background seed.
    // A preceding standalone Clear leaves a populated compatibility slot and
    // can reject Direct admission before this three-draw source starts. Clear
    // policy coverage belongs to the unchanged patterned-Clear fixture.
    if (!visibleUniform) {
      builder.appendCommandChunk(
          CommandChunkEnvelope{
              .recordCount = static_cast<std::uint32_t>(clearRecords.size())},
          makeChunk(clearRecords));
    }
    if (queryAware) appendIssue(2u);
    builder.appendCommandChunk(
        CommandChunkEnvelope{.recordCount = drawCount == 1u ? 1u
                                                             : drawCount + 1u,
                             .handleCount = drawCount == 1u ? 0u
                                                             : visibleUniform ?
                                                                 volumeTexture ? 7u : 6u : 5u},
        drawCount == 1u
            ? generalIndexedDrawChunk()
            : visibleUniform
                ? gpuVisibleUniformDrawRunChunk(alteredMiddleUniform,
                                                alteredMiddleTransform, extraStream,
                                                volumeTexture)
                : generalIndexedDrawRunChunk(drawCount, patternedClear));
    if (queryAware) {
      appendIssue(1u);
      appendPoll(1u, 1, 0u, 0u);
      // Three four-pixel-wide visible stripes cover 12 * 16 samples. The
      // middle VS transform intentionally leaves two background gaps.
      appendPoll(1u, queryPendingOnly ? 1 : 0, 192u, 1u);
      // The first result owner must survive reuse and the public wrapper's
      // retirement if a fresh replay observes Pending for captured S_OK.
      appendIssue(2u);
      appendIssue(1u);
      appendPoll(2u, queryPendingOnly ? 1 : 0, 0u, 0u);
      builder.appendObjectDestroy(queryIdentity);
    }
    builder.appendCommandChunk(
        CommandChunkEnvelope{.recordCount = 1u, .handleCount = 1u}, present);
    // Visible adds a pixel shader definition and omits the Clear event.
    builder.appendPresentComplete(
        queryAware || volumeTexture ? builder.eventCount()
                                   : auditTextureFormat != 0u ? 14u : 12u, 1u,
        strictDigest ? RenderTapeDigestValidity::Sha256
                     : RenderTapeDigestValidity::NotCaptured,
        strictDigest
            ? (visibleUniform
                   ? RenderTapeCaptureSession::sha256(
                         gpuVisibleUniformExpectedPixels(backgroundARGB))
                   : patternedClear
                   ? RenderTapeCaptureSession::sha256(gpuPolicyExpectedPixels())
                   : kGeneralExpectedDigest)
            : RenderTapeDigest{},
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
      .backBufferFormat = 22u,
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
                           bool wrongVertexDeclaration = false,
                           bool explicitPresentSource = false) {
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
        .handles = explicitPresentSource
            ? std::vector<D9CCommandChunkWireHandleEntry>{{
                  kOutput.kind, kOutput.generation, kOutput.objectId}}
            : std::vector<D9CCommandChunkWireHandleEntry>{},
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
          CommandChunkEnvelope{.recordCount = 1u,
                               .handleCount = explicitPresentSource ? 1u : 0u},
          presentChunk);
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
              .recordCount = static_cast<std::uint32_t>(records.size()),
              .handleCount = explicitPresentSource ? 1u : 0u},
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

// The first command deliberately precedes the texture definition.  The
// prestage discriminator must still admit the texture because no command
// occurs between its definition and its complete seed; ObjectDefine then
// transfers the hidden physical object into the active table before the draw.
struct PostCommandTexturePrestageFixture {
  std::array<std::byte, 16u> seed{
      std::byte{0x10}, std::byte{0x20}, std::byte{0xf0}, std::byte{0xff},
      std::byte{0xe0}, std::byte{0x30}, std::byte{0x20}, std::byte{0xff},
      std::byte{0x30}, std::byte{0xd0}, std::byte{0x40}, std::byte{0xff},
      std::byte{0xc0}, std::byte{0x50}, std::byte{0xb0}, std::byte{0xff},
  };
  std::vector<std::byte> vertexBytes{};
  std::vector<std::byte> indexBytes{};
  std::vector<std::byte> shaderBytes{};
  RenderTapeDigest seedDigest{};
  std::vector<RenderTapeProviderBlob> blobsValue{};
  std::vector<std::byte> tape{};

  explicit PostCommandTexturePrestageFixture(bool alternateSeed = false,
                                             std::uint32_t texturePool = 0u) {
    if (alternateSeed) {
      for (std::size_t pixel = 0u; pixel < seed.size(); pixel += 4u) {
        seed[pixel + 0u] = std::byte{0x70};
        seed[pixel + 1u] = std::byte{0x40};
        seed[pixel + 2u] = std::byte{0x20};
      }
    }
    seedDigest = RenderTapeCaptureSession::sha256(seed);
    const std::array vertices{
        TexturedVertex{-0.5f, -0.5f, 0.0f, 1.0f, 0xffffffffu, 0.0f, 0.0f},
        TexturedVertex{31.5f, -0.5f, 0.0f, 1.0f, 0xffffffffu, 2.0f, 0.0f},
        TexturedVertex{-0.5f, 31.5f, 0.0f, 1.0f, 0xffffffffu, 0.0f, 2.0f},
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
    const auto vertexDigest = RenderTapeCaptureSession::sha256(vertexBytes);
    const auto indexDigest = RenderTapeCaptureSession::sha256(indexBytes);
    const auto shaderDigest = RenderTapeCaptureSession::sha256(shaderBytes);
    blobsValue = {
        {.digest = vertexDigest, .bytes = vertexBytes},
        {.digest = indexDigest, .bytes = indexBytes},
        {.digest = shaderDigest, .bytes = shaderBytes},
        {.digest = seedDigest, .bytes = seed},
    };
    const D9CSurfaceDesc outputDesc{
        .format = 21u,
        .resourceType = 1u,
        .usage = 1u,
        .pool = 0u,
        .width = 16u,
        .height = 16u,
        .depth = 1u,
    };
    const auto outputDescriptorV2 = outputDescriptor(outputDesc);
    const auto textureDescriptor = texture2DDescriptor(D9CSurfaceDesc{
        .format = 21u,
        .resourceType = 3u,
        .usage = 0u,
        .pool = texturePool,
        .width = 2u,
        .height = 2u,
        .depth = 1u,
    });
    const D9CCommandChunkWireClear clear{
        .flags = 1u,
        .colorARGB = 0xff081018u,
        .z = 1.0f,
        .rectOffset = sizeof(D9CCommandChunkWireClear),
    };
    const D9CCommandChunkWirePresent present{};
    const RenderTapeOracleAttachment oracle{
        .identity = kOutput,
        .descriptorKind = static_cast<std::uint32_t>(
            RenderTapeDescriptorKind::Surface),
    };

    std::array<D9CCommandChunkWireTextureBinding,
               D9C_DRAW_PACKET_MAX_TEXTURES>
        textures{};
    for (std::uint32_t slot = 0u; slot < textures.size(); ++slot) {
      textures[slot] = {.slot = slot,
                        .valid = 1u,
                        .handleIndex = slot == 0u
                            ? 0u
                            : D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX};
    }
    std::array<D9CCommandChunkWireStreamBinding, D9C_DRAW_PACKET_MAX_STREAMS>
        streams{};
    for (std::uint32_t slot = 0u; slot < streams.size(); ++slot) {
      streams[slot] = {
          .slot = slot,
          .valid = 1u,
          .handleIndex = slot == 0u
              ? 1u
              : D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX,
          .stride = slot == 0u
              ? static_cast<std::uint32_t>(sizeof(TexturedVertex))
              : 0u,
          .frequency = 1u,
      };
    }
    const D9CCommandChunkWireVertexInput input{
        .valid = 1u,
        .kind = D9C_COMMAND_CHUNK_VERTEX_INPUT_FVF,
        .value = 0x144u,
        .handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX,
    };
    const D9CCommandChunkWireIndexBinding index{
        .valid = 1u,
        .handleIndex = 2u,
    };
    const std::array stateSections{
        std::pair{static_cast<std::uint16_t>(D9C_COMMAND_CHUNK_SECTION_TEXTURE),
                  spanBytes(textures)},
        std::pair{static_cast<std::uint16_t>(D9C_COMMAND_CHUNK_SECTION_STREAM),
                  spanBytes(streams)},
        std::pair{static_cast<std::uint16_t>(
                      D9C_COMMAND_CHUNK_SECTION_VERTEX_INPUT),
                  spanBytes(input)},
        std::pair{static_cast<std::uint16_t>(
                      D9C_COMMAND_CHUNK_SECTION_INDEX_BUFFER),
                  spanBytes(index)},
    };
    const auto statePayload = sparsePayload(
        D9CCommandChunkWireDrawHeader{
            .flags = D9C_COMMAND_CHUNK_DRAW_FLAG_FULL_SNAPSHOT},
        stateSections);
    const auto stateBytes = makeChunk(std::array{Record{
        .type = D9C_COMMAND_RECORD_APPLY_STATE,
        .payload = statePayload,
        .handles = {
            {kTexture.kind, kTexture.generation, kTexture.objectId},
            {kGeneralVertexBuffer.kind, kGeneralVertexBuffer.generation,
             kGeneralVertexBuffer.objectId},
            {kGeneralIndexBuffer.kind, kGeneralIndexBuffer.generation,
             kGeneralIndexBuffer.objectId},
        },
    }});
    const auto drawBytes = generalIndexedDrawChunk();
    ImportedChunkView stateView;
    check(importPrevalidatedCommandChunk(
              stateBytes,
              CommandChunkEnvelope{.recordCount = 1u, .handleCount = 3u},
              stateView),
          "post-command prestage state anchor imports");
    ImportedChunkView drawView;
    check(importPrevalidatedCommandChunk(
              drawBytes, CommandChunkEnvelope{.recordCount = 1u},
              drawView),
          "post-command prestage draw imports");
    const auto stateRecord = stateView.record(0u);
    const auto drawRecord = drawView.record(0u);
    const auto drawChunk = makeChunk(std::array{
        Record{
            .type = stateRecord.header.type,
            .payload = std::vector<std::byte>(stateRecord.payload.begin(),
                                              stateRecord.payload.end()),
            .handles = std::vector<D9CCommandChunkWireHandleEntry>(
                stateView.handles.begin(), stateView.handles.end()),
        },
        Record{
            .type = drawRecord.header.type,
            .payload = std::vector<std::byte>(drawRecord.payload.begin(),
                                              drawRecord.payload.end()),
        },
    });

    RenderTapeBuilder builder;
    builder.appendBootstrapState(implicitBootstrapChunk());
    builder.appendObjectDefine(
        kOutput, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
        std::as_bytes(std::span(&outputDescriptorV2, 1u)), 0u, {});
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
        std::as_bytes(std::span(&shader, 1u)), shaderBytes.size(),
        RenderTapeCaptureSession::sha256(shaderBytes));
    builder.appendResourceMutation(
        kGeneralVertexBuffer, RenderTapeMutationKind::Upload, 0u, 0u,
        vertexBytes.size(), RenderTapeCaptureSession::sha256(vertexBytes));
    builder.appendResourceMutation(
        kGeneralIndexBuffer, RenderTapeMutationKind::Upload, 0u, 0u,
        indexBytes.size(), RenderTapeCaptureSession::sha256(indexBytes));
    builder.appendCommandChunk(
        CommandChunkEnvelope{.recordCount = 1u},
        makeChunk(std::array{Record{
            .type = D9C_COMMAND_RECORD_CLEAR,
            .payload = bytesOf(clear),
        }}));
    builder.appendObjectDefine(
        kTexture, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Texture),
        textureDescriptor, 0u, {}, seed.size(), 1u);
    builder.appendResourceMutation(kTexture, RenderTapeMutationKind::Upload,
                                   0u, 0u, seed.size(), seedDigest);
    builder.appendCommandChunk(
        CommandChunkEnvelope{.recordCount = 2u, .handleCount = 3u},
        drawChunk);
    builder.appendCommandChunk(
        CommandChunkEnvelope{.recordCount = 1u},
        makeChunk(std::array{Record{
            .type = D9C_COMMAND_RECORD_PRESENT,
            .payload = bytesOf(present),
        }}));
    builder.appendPresentComplete(
        12u, 1u, RenderTapeDigestValidity::NotCaptured, {},
        std::as_bytes(std::span(&oracle, 1u)));
    tape = builder.seal();
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
      .backBufferFormat = 22u,
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
  auto prepared = prepareRenderTapeIdentity(fixture.tape,
                                             std::span(&blob, 1u));
  fixture.tape[0] ^= std::byte{0xff};
  fixture.seed[0] ^= std::byte{0xff};
  auto movedPrepared = std::move(prepared);
  const auto preparedFirst = replayPreparedRenderTapeIdentity(
      device, movedPrepared,
      RenderTapePreparedExecutionOptions{
          .route = RenderTapePreparedRoute::Compatibility,
          .reset = RenderTapePreparedReset::FreshDevice});
  const auto preparedSecond = replayPreparedRenderTapeIdentity(
      device, movedPrepared,
      RenderTapePreparedExecutionOptions{
          .route = RenderTapePreparedRoute::Compatibility,
          .reset = RenderTapePreparedReset::FreshDevice});
  SequenceFixture warmBFixture;
  const auto warmBBlobs = warmBFixture.blobs();
  auto preparedWarmB = prepareRenderTapeIdentity(warmBFixture.tape,
                                                   warmBBlobs);
  const auto warmA = replayPreparedRenderTapeIdentity(
      device, movedPrepared,
      RenderTapePreparedExecutionOptions{
          .route = RenderTapePreparedRoute::Compatibility,
          .reset = RenderTapePreparedReset::WarmDevice});
  const auto warmB = replayPreparedRenderTapeIdentity(
      device, preparedWarmB,
      RenderTapePreparedExecutionOptions{
          .route = RenderTapePreparedRoute::Compatibility,
          .reset = RenderTapePreparedReset::WarmDevice});
  const auto warmAAgain = replayPreparedRenderTapeIdentity(
      device, movedPrepared,
      RenderTapePreparedExecutionOptions{
          .route = RenderTapePreparedRoute::Compatibility,
          .reset = RenderTapePreparedReset::WarmDevice});
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
  check(preparedFirst.complete() && preparedSecond.complete() &&
            first.validity.outputDigest == preparedFirst.validity.outputDigest &&
            preparedFirst.validity.outputDigest ==
                preparedSecond.validity.outputDigest &&
            preparedFirst.prepared.planReused &&
            preparedSecond.prepared.planReused &&
            preparedFirst.prepared.timing.chunkReplayNanos != 0u,
        "moved prepared input must replay repeatedly with one immutable plan");
  check(warmA.complete() && warmB.complete() && warmAAgain.complete() &&
            warmA.prepared.resetPerformed && warmA.prepared.resetVerified &&
            warmB.prepared.resetPerformed && warmB.prepared.resetVerified &&
            warmAAgain.prepared.resetPerformed &&
            warmAAgain.prepared.resetVerified &&
            warmA.validity.outputDigest == warmAAgain.validity.outputDigest &&
            warmB.validity.outputDigest != warmA.validity.outputDigest,
        "same-device warm A-B-A replay must reset state and preserve A output");
  check(first.conservation.objectsCreated == 2u &&
            first.conservation.objectsReleased == 2u &&
            second.conservation.objectsCreated == 2u &&
            second.conservation.objectsReleased == 2u &&
            production.conservation.objectsCreated == 3u &&
            production.conservation.objectsReleased == 3u,
        "repeat replay conserves output, texture, and declaration wrappers independently");
}

void nativeMetalPostCommandTexturePrestageMatchesOrdinaryPixels(
    std::uint32_t texturePool) {
  PostCommandTexturePrestageFixture fixture(false, texturePool);
  const auto preflight = preflightRenderTapeIdentity(
      fixture.tape, fixture.blobsValue);
  check(preflight.complete(), frameTapeReplayStatusName(preflight.status));
  check(preflight.coverage.eventCount == 13u &&
            preflight.coverage.objectDefinitions == 5u &&
            preflight.coverage.seedMutations == 3u &&
            preflight.coverage.commandChunks == 3u &&
            preflight.coverage.commandRecords == 4u &&
            preflight.coverage.clearRecords == 1u &&
            preflight.coverage.applyStateRecords == 1u &&
            preflight.coverage.drawIndexedPrimitiveRecords == 1u &&
            preflight.coverage.presentRecords == 1u &&
            preflight.conservation.inputBlobs == 4u &&
            preflight.conservation.referencedBlobs == 4u,
        "post-command prestage fixture preserves the exact clear/define/seed/draw order");
  auto prepared = prepareRenderTapeIdentity(fixture.tape,
                                             fixture.blobsValue);
  check(prepared.valid() && prepared.evidence().immutableInputOwned,
        "post-command prestage fixture owns its prepared input");

  const D9CPresentParams params{
      .backBufferWidth = 16u,
      .backBufferHeight = 16u,
      .backBufferFormat = 21u,
      .backBufferCount = 1u,
      .swapEffect = 1u,
      .windowed = 1u,
      .presentationInterval = 0x80000000u,
  };
  const auto replay = [&](PreparedRenderTape& input,
                          RenderTapePreparedExecutionOptions options) {
    auto* factory = dxmt9c_factory_create();
    check(factory != nullptr, "post-command prestage factory is available");
    auto* device =
        dxmt9c_factory_create_device(factory, 0u, &params, 0u, nullptr);
    check(device != nullptr, "post-command prestage device constructs");
    const auto result = replayPreparedRenderTapeIdentity(device, input,
                                                         options);
    dxmt9c_device_release(device);
    dxmt9c_factory_release(factory);
    return result;
  };
  const auto ordinary = replay(prepared, {
      .route = RenderTapePreparedRoute::Compatibility,
      .reset = RenderTapePreparedReset::FreshDevice,
  });
  const auto prestaged = replay(prepared, {
      .route = RenderTapePreparedRoute::Compatibility,
      .reset = RenderTapePreparedReset::FreshDevice,
      .auditTexturePrestage = true,
  });
  check(ordinary.complete() && prestaged.complete(),
        std::string("post-command prestage replay completes: ") +
            frameTapeReplayStatusName(ordinary.status) + "/" +
            frameTapeReplayStatusName(prestaged.status));
  check(ordinary.validity.outputReadback && ordinary.validity.outputNonDegenerate &&
            prestaged.validity.outputReadback &&
            prestaged.validity.outputNonDegenerate &&
            ordinary.outputPixels.size() == 16u * 16u * 4u &&
            prestaged.outputPixels == ordinary.outputPixels &&
            prestaged.validity.outputDigest == ordinary.validity.outputDigest,
        "hidden prestage activation produces exactly the ordinary sampled pixels");
  check(ordinary.conservation.objectsCreated == 5u &&
            ordinary.conservation.objectsReleased == 5u &&
            prestaged.conservation.objectsCreated == 5u &&
            prestaged.conservation.objectsReleased == 5u,
        "ordinary and prestaged replays conserve all indexed fixture objects");

  PostCommandTexturePrestageFixture alternateFixture(true, texturePool);
  auto alternatePrepared = prepareRenderTapeIdentity(
      alternateFixture.tape, alternateFixture.blobsValue);
  const auto alternate = replay(alternatePrepared, {
      .route = RenderTapePreparedRoute::Compatibility,
      .reset = RenderTapePreparedReset::FreshDevice,
  });
  check(alternate.complete() &&
            alternate.validity.outputReadback &&
            alternate.validity.outputNonDegenerate &&
            alternate.outputPixels != ordinary.outputPixels &&
            alternate.validity.outputDigest != ordinary.validity.outputDigest,
        "alternate texture seed must change sampled pixels");

  const auto& evidence = prestaged.texturePrestageAudit;
  check(evidence.requested && evidence.attempted && evidence.complete &&
            evidence.candidateObjects == 1u &&
            evidence.candidateSubresources == 1u &&
            evidence.candidateBytes == fixture.seed.size() &&
            evidence.immediateUploadSubresources +
                    evidence.deferredUploadSubresources ==
                1u &&
            evidence.activatedObjects == 1u && evidence.consumedSeeds == 1u &&
            evidence.stagedObjectsRemaining == 0u &&
            evidence.stagedSubresourcesRemaining == 0u &&
            evidence.stagedBytesRemaining == 0u &&
            evidence.hiddenObjectsCreated == 1u &&
            evidence.hiddenObjectsReleased == 0u &&
            evidence.activeObjectsCreated == 4u &&
            evidence.activeObjectsReleased == 5u &&
            evidence.physicalObjectsCreated == 5u &&
            evidence.physicalObjectsReleased == 5u &&
            evidence.physicalConservation,
        "prestage evidence is non-vacuous, fully consumed, and physically conserved");
  if (evidence.deferredUploadSubresources != 0u) {
    check(evidence.initializerFlushes == 1u &&
              evidence.initializerWaits == 1u &&
              evidence.initializerUploadValue != 0u,
          "deferred prestage upload must perform and wait for initializer flush");
  } else {
    check(evidence.initializerFlushes == 0u &&
              evidence.initializerWaits == 0u,
          "immediate prestage upload must not claim initializer synchronization");
  }
}

void nativeMetalPostCommandTexturePrestageMatchesOrdinaryPixels() {
  nativeMetalPostCommandTexturePrestageMatchesOrdinaryPixels(0u);
  nativeMetalPostCommandTexturePrestageMatchesOrdinaryPixels(1u);
}

void sourceBoundaryExactCoverExhaustive() {
  // R-HARN-REPLAY-8.13: every partition of a bounded serial stream, and a
  // zero/gap/overlap counterexample at every source edge.
  for (std::uint32_t count = 1u; count <= 7u; ++count) {
    for (std::uint32_t mask = 0u; mask < (1u << (count - 1u)); ++mask) {
      std::vector<ProviderReplayRecordRange> ranges;
      std::uint32_t first = 0u;
      for (std::uint32_t end = 1u; end <= count; ++end) {
        if (end == count || (mask & (1u << (end - 1u)))) {
          ranges.push_back({first, end - first});
          first = end;
        }
      }
      check(validateProviderReplayRecordRanges(count, ranges),
            "every exact serial partition must admit");
      std::uint32_t ordinal = 0u;
      for (const auto& range : ranges) {
        for (std::uint32_t local = 0u; local < range.recordCount; ++local)
          check(range.firstRecord + local == ordinal++, "exactly-once serial coverage");
      }
      check(ordinal == count, "partition consumes whole stream");
      for (std::uint32_t cutsMask = 0u; cutsMask < (1u << (count - 1u)); ++cutsMask) {
        std::vector<std::uint32_t> cuts;
        bool internalOnly = true;
        for (std::uint32_t cut = 1u; cut < count; ++cut) {
          if (!(cutsMask & (1u << (cut - 1u)))) continue;
          cuts.push_back(cut);
          for (const auto& source : ranges)
            if (source.firstRecord == cut) internalOnly = false;
        }
        check(validateProviderReplayBatchCuts(count, ranges, cuts) == internalOnly,
              "batch cuts admit exactly the strict interior subset of every partition");
        if (!internalOnly) continue;
        std::uint32_t consumed = 0u, publications = 0u, flushes = 0u;
        for (const auto& source : ranges) {
          for (std::uint32_t record = source.firstRecord;
               record < source.firstRecord + source.recordCount; ++record) {
            if (std::find(cuts.begin(), cuts.end(), record) != cuts.end()) ++flushes;
            check(record == consumed++, "nested partition preserves serial exactly once");
          }
          ++publications;
        }
        check(consumed == count && publications == ranges.size() && flushes == cuts.size(),
              "batch partition does not introduce source publications");
        if (!cuts.empty()) {
          auto bad = cuts;
          bad.push_back(cuts.back());
          check(!validateProviderReplayBatchCuts(count, ranges, bad), "duplicate cut rejects");
          bad = cuts;
          bad.insert(bad.begin(), count);
          check(!validateProviderReplayBatchCuts(count, ranges, bad), "unordered cut rejects");
        }
      }
      for (std::size_t i = 0; i < ranges.size(); ++i) {
        auto corrupt = ranges;
        corrupt[i].recordCount = 0u;
        check(!validateProviderReplayRecordRanges(count, corrupt), "zero range rejects");
        corrupt = ranges;
        ++corrupt[i].firstRecord;
        check(!validateProviderReplayRecordRanges(count, corrupt), "range gap rejects");
        if (i != 0u) {
          corrupt = ranges;
          --corrupt[i].firstRecord;
          check(!validateProviderReplayRecordRanges(count, corrupt), "overlap rejects");
        }
      }
    }
  }
  check(!validateProviderReplayRecordRanges(0u, {}), "empty audit cover rejects");
  check(!validateProviderReplayRecordRanges(1u, {}), "missing cover rejects");
  const std::array overflow{
      ProviderReplayRecordRange{0u, 0xffffffffu},
      ProviderReplayRecordRange{0xffffffffu, 1u}};
  check(!validateProviderReplayRecordRanges(0xffffffffu, overflow), "overflow rejects");
}

std::vector<std::byte> sourceBoundaryFixtureIdentity(
    const GeneralIndexedFixture& fixture, bool wholeChunkSources = false) {
  render_tape_runtime::GeneralPreflightPlan plan;
  check(render_tape_runtime::buildGeneralReplayPlan(
            fixture.tape, fixture.blobsValue, plan).complete(), "source-cut fixture plan");
  std::vector<RenderTapeIdentitySource> sources;
  std::vector<RenderTapeIdentityRange> ranges;
  std::vector<RenderTapeIdentitySettlement> settlements;
  for (const auto& event : plan.events) {
    if (event.type != RenderTapeEventType::CommandChunk) continue;
    const auto ordinal = plan.tape.event(event.eventIndex).header.ordinal;
    const auto firstSource = sources.size() + 1u;
    const auto count = plan.commandChunks[event.index].envelope.recordCount;
    const auto stride = wholeChunkSources ? count : 1u;
    for (std::uint32_t record = 0u; record < count; record += stride) {
      const auto source = sources.size() + 1u;
      sources.push_back({.eventOrdinal = ordinal, .sourceOrdinal = source,
                         .seqId = source, .captureToken = 77u,
                         .firstRecord = record, .recordCount = stride,
                         .firstRange = static_cast<std::uint32_t>(ranges.size()),
                         .rangeCount = 1u});
      ranges.push_back({.eventOrdinal = ordinal, .sourceOrdinal = source,
                        .seqId = source, .logicalPassId = source,
                        .firstRecord = record, .recordCount = stride,
                        .dagPassIndex = 0u, .passKind = 1u});
    }
    settlements.push_back({.eventOrdinal = ordinal, .rawOrdinal = ordinal,
                            .buildGeneration = 1u, .firstSourceOrdinal = firstSource,
                            .tailSeqId = sources.size(),
                            .sourceCount = wholeChunkSources ? 1u : count});
  }
  auto identity = buildRenderTapeIdentity(fixture.tape, plan.catalogue, 42u,
      plan.presentOrdinal, 77u, RenderTapeIdentityAuthority::Capture,
      sources, ranges, nullptr, settlements);
  check(!identity.empty(), "source-cut fixture identity builds");
  return identity;
}

void nativeMetalSourceBoundaryAudit() {
  const GeneralIndexedFixture fixture(32u, true, true);
  const auto prepared = prepareRenderTapeIdentity(fixture.tape, fixture.blobsValue);
  check(prepared.valid(), "source-cut prepared fixture");
  const auto identity = sourceBoundaryFixtureIdentity(fixture);
  render_tape_runtime::GeneralPreflightPlan plan;
  check(render_tape_runtime::buildGeneralReplayPlan(
            fixture.tape, fixture.blobsValue, plan).complete(), "shared source plan fixture");
  CaptureSourceReplayPlan sharedPlan;
  check(buildCaptureSourceReplayPlan(fixture.tape, plan, identity, sharedPlan) ==
            CaptureSourceReplayPlanStatus::Valid && sharedPlan.valid() &&
            sharedPlan.rows().size() == 36u, "authenticated shared plan preserves all sources");
  CaptureSourceReplayPlan repeated;
  check(buildCaptureSourceReplayPlan(fixture.tape, plan, identity, repeated) ==
            CaptureSourceReplayPlanStatus::Valid && repeated.digest() == sharedPlan.digest(),
        "source-plan canonical digest is deterministic");
  const auto digest = sharedPlan.digest();
  auto moved = std::move(sharedPlan);
  check(moved.valid() && !sharedPlan.valid() && moved.digest() == digest,
        "moving a valid source plan invalidates old authority");
  check(buildCaptureSourceReplayPlan({}, plan, identity, moved) !=
            CaptureSourceReplayPlanStatus::Valid && moved.valid() && moved.digest() == digest,
        "failed build does not replace prior output; caller must check returned status");
  for (unsigned fault = 0u; fault < 4u; ++fault) {
    auto badPlan = plan;
    auto bytes = std::vector<std::byte>(plan.commandChunks[0].bytes.begin(),
                                        plan.commandChunks[0].bytes.end());
    if (fault == 0u) {
      bytes.back() ^= std::byte{1u};
      badPlan.commandChunks[0].bytes = bytes;
    } else if (fault == 1u) {
      std::swap(badPlan.events[0], badPlan.events[1]);
    } else if (fault == 2u) {
      badPlan.events.erase(badPlan.events.begin());
    } else {
      badPlan.events[1] = badPlan.events[0];
    }
    CaptureSourceReplayPlan rejected;
    check(buildCaptureSourceReplayPlan(fixture.tape, badPlan, identity, rejected) !=
              CaptureSourceReplayPlanStatus::Valid && !rejected.valid(),
          "stale same-size chunk and reordered/missing/duplicate events reject");
  }
  RenderTapePreparedExecutionOptions options{
      .auditSourceBoundaries = true, .sourceBoundaryIdentity = identity};

  // No device is needed to pin admission failures: the audit must reject
  // malformed provenance before any replay-owned object can be created.
  auto missing = options;
  missing.sourceBoundaryIdentity = {};
  auto result = replayPreparedRenderTapeIdentity(nullptr, prepared, missing);
  check(result.status == FrameTapeReplayStatus::InvalidTape &&
            !result.sourceBoundaryAudit.validated && result.conservation.objectsCreated == 0u,
        "missing cut authority rejects before effects");
  for (bool staleDigest : {false, true}) {
    auto corrupt = identity;
    RenderTapeIdentityHeader header{};
    std::memcpy(&header, corrupt.data(), sizeof(header));
    if (staleDigest) {
      header.eventsDigest[0] ^= std::byte{1u};
      std::memcpy(corrupt.data(), &header, sizeof(header));
    } else {
      RenderTapeIdentitySource source{};
      std::memcpy(&source, corrupt.data() + header.sourceTableOffset, sizeof(source));
      ++source.firstRecord;
      std::memcpy(corrupt.data() + header.sourceTableOffset, &source, sizeof(source));
    }
    auto bad = options;
    bad.sourceBoundaryIdentity = corrupt;
    result = replayPreparedRenderTapeIdentity(nullptr, prepared, bad);
    check(result.status == FrameTapeReplayStatus::InvalidTape &&
              !result.sourceBoundaryAudit.validated && result.conservation.objectsCreated == 0u,
          "stale digest or incomplete cover rejects before effects");
  }
  for (bool warm : {false, true}) {
    auto bad = options;
    if (warm) bad.reset = RenderTapePreparedReset::WarmDevice;
    else bad.route = RenderTapePreparedRoute::ProductionSlot;
    check(replayPreparedRenderTapeIdentity(nullptr, prepared, bad).status ==
              FrameTapeReplayStatus::UnsupportedGrammar, "audit route/reset must be explicit");
  }

  const D9CPresentParams params{.backBufferWidth = 16u, .backBufferHeight = 16u,
      .backBufferFormat = 22u, .backBufferCount = 1u, .swapEffect = 1u,
      .windowed = 1u, .presentationInterval = 0x80000000u};
  auto* factory = dxmt9c_factory_create();
  check(factory != nullptr, "source-cut factory");
  auto* device = dxmt9c_factory_create_device(factory, 0u, &params, 0u, nullptr);
  check(device != nullptr, "source-cut device");
  result = replayPreparedRenderTapeIdentity(device, prepared, options);
  dxmt9c_device_release(device);
  dxmt9c_factory_release(factory);
  check(result.complete(), frameTapeReplayStatusName(result.status));
  check(result.sourceBoundaryAudit.requested && result.sourceBoundaryAudit.diagnostic &&
            result.sourceBoundaryAudit.validated && result.sourceBoundaryAudit.complete &&
            result.sourceBoundaryAudit.sourceCount == 36u &&
            result.sourceBoundaryAudit.completedSourceCount == 36u,
        "all 36 exact-cover ranges execute and request a flush");
  check(result.validity.expectedDigestMatched &&
            result.outputPixels == gpuPolicyExpectedPixels(),
        "source cuts preserve independent clipped-Clear oracle");
  check(result.conservation.objectsCreated == result.conservation.objectsReleased,
        "source-cut resource ownership conserved");

  // A second execution preserves whole-chunk source cuts, adding only local
  // flushes. The fixture oracle proves clear/control order, not visible draws.
  const auto groupedIdentity = sourceBoundaryFixtureIdentity(fixture, true);
  std::vector<RenderTapeBatchCut> batchCuts;
  for (const auto& event : plan.events) {
    if (event.type != RenderTapeEventType::CommandChunk) continue;
    const auto& chunk = plan.commandChunks[event.index];
    ImportedChunkView imported;
    check(importPrevalidatedCommandChunk(chunk.bytes, chunk.envelope, imported),
          "batch-cut native chunk imports");
    for (std::uint32_t record = 2u; record < imported.records.size(); ++record) {
      if (imported.records[record].type == D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE)
        batchCuts.push_back({plan.tape.event(event.eventIndex).header.ordinal, record});
    }
  }
  check(batchCuts.size() == 31u, "batch-cut fixture contains 31 internal draw cuts");
  auto batched = options;
  batched.sourceBoundaryIdentity = groupedIdentity;
  batched.auditBatchCuts = batchCuts;
  for (unsigned fault = 0u; fault < 5u; ++fault) {
    auto bad = batched;
    auto cuts = batchCuts;
    if (fault == 0u) ++cuts[0].eventOrdinal;
    if (fault == 1u) cuts[0].beforeRecord = 0u;
    if (fault == 2u) cuts[1] = cuts[0];
    if (fault == 3u) cuts[0].beforeRecord = 0xffffffffu;
    if (fault == 4u) bad.auditSourceBoundaries = false;
    bad.auditBatchCuts = cuts;
    const auto rejected = replayPreparedRenderTapeIdentity(nullptr, prepared, bad);
    check(!rejected.complete() && !rejected.sourceBoundaryAudit.validated &&
              rejected.conservation.objectsCreated == 0u,
          "bad local cut is rejected before resource effects");
  }
  factory = dxmt9c_factory_create();
  check(factory != nullptr, "batch-cut factory");
  device = dxmt9c_factory_create_device(factory, 0u, &params, 0u, nullptr);
  check(device != nullptr, "batch-cut device");
  result = replayPreparedRenderTapeIdentity(device, prepared, batched);
  dxmt9c_device_release(device);
  dxmt9c_factory_release(factory);
  check(result.complete() && result.sourceBoundaryAudit.complete &&
            result.sourceBoundaryAudit.sourceCount == 3u &&
            result.sourceBoundaryAudit.completedSourceCount == 3u &&
            result.sourceBoundaryAudit.batchCutCount == 31u &&
            result.sourceBoundaryAudit.completedBatchCutCount == 31u &&
            result.outputPixels == gpuPolicyExpectedPixels() &&
            result.conservation.objectsCreated == result.conservation.objectsReleased,
        "31 local flushes preserve exactly 3 source publications and independent oracle");
}

void nativeMetalCaptureSourceReplayPolicy() {
  // The policy fixture deliberately clips its indexed draws.  This test
  // authenticates source coverage and compares the route output to the
  // fixture's independent patterned-clear oracle; it does not claim visible
  // draw fidelity for clipped geometry.
  const GeneralIndexedFixture fixture(32u, true, true);
  const auto prepared = prepareRenderTapeIdentity(fixture.tape,
                                                   fixture.blobsValue);
  check(prepared.valid() && prepared.evidence().immutableInputOwned,
        "capture-source policy owns its prepared fixture");
  render_tape_runtime::GeneralPreflightPlan plan;
  check(render_tape_runtime::buildGeneralReplayPlan(
            fixture.tape, fixture.blobsValue, plan)
            .complete(),
        "capture-source policy preflight completes");
  const auto identity = sourceBoundaryFixtureIdentity(fixture);
  CaptureSourceReplayPlan sourcePlan;
  check(buildCaptureSourceReplayPlan(fixture.tape, plan, identity, sourcePlan) ==
            CaptureSourceReplayPlanStatus::Valid && sourcePlan.valid(),
        "capture-source policy identity authenticates");

  const D9CPresentParams params{.backBufferWidth = 16u,
                                .backBufferHeight = 16u,
                                .backBufferFormat = 22u,
                                .backBufferCount = 1u,
                                .swapEffect = 1u,
                                .windowed = 1u,
                                .presentationInterval = 0x80000000u};
  struct RouteRun {
    FrameTapeReplayResult result{};
    dxmt9::perf::RenderTapeReplaySnapshot before{};
    dxmt9::perf::RenderTapeReplaySnapshot after{};
  };
  const auto replay = [&](RenderTapePreparedRoute route) {
    auto* factory = dxmt9c_factory_create();
    check(factory != nullptr, "capture-source policy factory constructs");
    auto* device =
        dxmt9c_factory_create_device(factory, 0u, &params, 0u, nullptr);
    check(device != nullptr, "capture-source policy device constructs");
    const auto before = dxmt9::perf::snapshotRenderTapeReplay();
    const auto result = replayPreparedRenderTapeIdentity(
        device, prepared,
        RenderTapePreparedExecutionOptions{
            .route = route,
            .reset = RenderTapePreparedReset::FreshDevice,
            .sourceBoundaryMode = RenderTapeSourceBoundaryMode::CaptureSource,
            .sourceBoundaryIdentity = identity,
            .expectedSourcePlanDigest = sourcePlan.digest()});
    const auto after = dxmt9::perf::snapshotRenderTapeReplay();
    dxmt9c_device_release(device);
    dxmt9c_factory_release(factory);
    return RouteRun{.result = result, .before = before, .after = after};
  };

  // The ordinary compatibility replay is the independent native-Metal
  // oracle. Both source-policy routes must retain its exact sampled bytes and
  // authenticated digest while exposing real source-settlement evidence.
  auto* oracleFactory = dxmt9c_factory_create();
  check(oracleFactory != nullptr, "capture-source oracle factory constructs");
  auto* oracleDevice =
      dxmt9c_factory_create_device(oracleFactory, 0u, &params, 0u, nullptr);
  check(oracleDevice != nullptr, "capture-source oracle device constructs");
  const auto oracle = replayPreparedRenderTapeIdentity(
      oracleDevice, prepared,
      RenderTapePreparedExecutionOptions{
          .route = RenderTapePreparedRoute::Compatibility,
          .reset = RenderTapePreparedReset::FreshDevice});
  dxmt9c_device_release(oracleDevice);
  dxmt9c_factory_release(oracleFactory);
  check(oracle.complete() && oracle.validity.outputReadback &&
            oracle.validity.expectedDigestMatched &&
            oracle.outputPixels == gpuPolicyExpectedPixels(),
        "capture-source policy has an independent patterned-clear oracle");

  for (const auto route : {RenderTapePreparedRoute::Compatibility,
                           RenderTapePreparedRoute::ProductionSlot}) {
    const auto run = replay(route);
    const auto& result = run.result;
    check(result.complete() && result.validity.outputReadback &&
              result.validity.expectedDigestMatched &&
              result.outputPixels == oracle.outputPixels &&
              result.validity.outputDigest == oracle.validity.outputDigest,
          "capture-source route preserves the independent oracle exactly");
    check(result.sourceReplay.requested && result.sourceReplay.validated &&
              result.sourceReplay.complete &&
              result.sourceReplay.planDigest == sourcePlan.digest() &&
              result.sourceReplay.sourceCount == sourcePlan.rows().size() &&
              result.sourceReplay.completedSourceCount ==
                  result.sourceReplay.sourceCount &&
              result.sourceReplay.publicationCount != 0u &&
              result.sourceReplay.presentSourceCount == 1u &&
              result.sourceReplay.publicationCount +
                      result.sourceReplay.emptySourceCount >=
                  result.sourceReplay.sourceCount &&
              result.prepared.requestedRoute == route,
          "capture-source route reports authenticated actual source coverage");
    bool rowsAuthenticated =
        result.sourceReplay.sources.size() == sourcePlan.rows().size();
    for (std::size_t index = 0u;
         rowsAuthenticated && index < sourcePlan.rows().size(); ++index) {
      const auto& captured = sourcePlan.rows()[index];
      const auto& actual = result.sourceReplay.sources[index];
      rowsAuthenticated = actual.eventOrdinal == captured.eventOrdinal &&
          actual.sourceOrdinal == captured.sourceOrdinal &&
          actual.capturedSeqId == captured.capturedSeqId &&
          actual.commandChunkIndex == captured.commandChunkIndex &&
          actual.firstRecord == captured.firstRecord &&
          actual.recordCount == captured.recordCount && actual.settled &&
          actual.actualRawOrdinal != 0u &&
          static_cast<std::size_t>(actual.publicationOffset) +
                  actual.publicationCount <=
              result.sourceReplay.publications.size();
    }
    check(rowsAuthenticated && !result.sourceReplay.publications.empty(),
          "capture-source evidence retains each captured row and local publication set");
    const auto compatibilityDraws =
        run.after.compatibilityDraws - run.before.compatibilityDraws;
    const auto directLeases = run.after.directLeases - run.before.directLeases;
    const auto directDraws =
        run.after.directLeaseDraws - run.before.directLeaseDraws;
    const auto gpuErrors = run.after.gpuErrors - run.before.gpuErrors;
    if (route == RenderTapePreparedRoute::Compatibility) {
      check(compatibilityDraws != 0u && directLeases == 0u &&
                directDraws == 0u && gpuErrors == 0u,
            "capture-source compatibility route has actual compatibility-only draw coverage");
    } else {
      check(directLeases != 0u && directDraws != 0u && gpuErrors == 0u,
            "capture-source ProductionSlot route has actual ordinary-Direct draw coverage");
    }
    check(!result.sourceBoundaryAudit.requested &&
              !result.sourceBoundaryAudit.validated,
          "capture-source evidence remains separate from diagnostic boundaries");
  }

  auto missingIdentity = replayPreparedRenderTapeIdentity(
      nullptr, prepared,
      RenderTapePreparedExecutionOptions{
          .route = RenderTapePreparedRoute::Compatibility,
          .reset = RenderTapePreparedReset::FreshDevice,
          .sourceBoundaryMode = RenderTapeSourceBoundaryMode::CaptureSource});
  check(missingIdentity.status == FrameTapeReplayStatus::InvalidTape &&
            missingIdentity.sourceReplay.requested &&
            !missingIdentity.sourceReplay.validated &&
            missingIdentity.conservation.objectsCreated == 0u,
        "capture-source policy rejects missing authenticated identity before effects");

  auto mixedMode = replayPreparedRenderTapeIdentity(
      nullptr, prepared,
      RenderTapePreparedExecutionOptions{
          .route = RenderTapePreparedRoute::Compatibility,
          .reset = RenderTapePreparedReset::FreshDevice,
          .auditSourceBoundaries = true,
          .sourceBoundaryMode = RenderTapeSourceBoundaryMode::CaptureSource,
          .sourceBoundaryIdentity = identity});
  check(mixedMode.status == FrameTapeReplayStatus::UnsupportedGrammar &&
            mixedMode.sourceReplay.requested &&
            !mixedMode.sourceReplay.validated &&
            mixedMode.sourceBoundaryAudit.requested &&
            !mixedMode.sourceBoundaryAudit.validated,
        "diagnostic source cuts and normal capture-source policy cannot mix");

  auto missingDigest = replayPreparedRenderTapeIdentity(
      nullptr, prepared,
      RenderTapePreparedExecutionOptions{
          .route = RenderTapePreparedRoute::Compatibility,
          .reset = RenderTapePreparedReset::FreshDevice,
          .sourceBoundaryMode = RenderTapeSourceBoundaryMode::CaptureSource,
          .sourceBoundaryIdentity = identity});
  check(missingDigest.status == FrameTapeReplayStatus::ObjectCreationFailed &&
            missingDigest.sourceReplay.validated &&
            missingDigest.conservation.objectsCreated == 0u,
        "capture-source policy preserves unqualified replay when expected digest is omitted");

  auto forgedDigest = sourcePlan.digest();
  forgedDigest[0] ^= std::byte{0x01};
  auto mismatchedDigest = replayPreparedRenderTapeIdentity(
      nullptr, prepared,
      RenderTapePreparedExecutionOptions{
          .route = RenderTapePreparedRoute::Compatibility,
          .reset = RenderTapePreparedReset::FreshDevice,
          .sourceBoundaryMode = RenderTapeSourceBoundaryMode::CaptureSource,
          .sourceBoundaryIdentity = identity,
          .expectedSourcePlanDigest = forgedDigest});
  check(mismatchedDigest.status == FrameTapeReplayStatus::InvalidTape &&
            !mismatchedDigest.sourceReplay.validated &&
            mismatchedDigest.conservation.objectsCreated == 0u,
        "capture-source policy rejects a stale expected plan digest before effects");
}

void nativeMetalTextureInputAudit() {
  // A bounded cold diagnostic, not a rendering policy: prove the real GPU
  // texture is replaced once at an authenticated cut, then replay continues.
  const GeneralIndexedFixture fixture(32u, true, true, false, false, false,
                                      render_tape_d3d_format::R32F);
  const auto prepared = prepareRenderTapeIdentity(fixture.tape, fixture.blobsValue);
  check(prepared.valid(), "R32F intervention fixture prepares");
  render_tape_runtime::GeneralPreflightPlan plan;
  check(render_tape_runtime::buildGeneralReplayPlan(
            fixture.tape, fixture.blobsValue, plan).complete(),
        "R32F intervention fixture preflights");
  const auto identity = sourceBoundaryFixtureIdentity(fixture, true);
  CaptureSourceReplayPlan sources;
  check(buildCaptureSourceReplayPlan(fixture.tape, plan, identity, sources) ==
            CaptureSourceReplayPlanStatus::Valid,
        "R32F intervention authenticates source identity");
  const std::array<float, 4u> input{1.0f, 0.75f, 0.5f, 0.25f};
  const auto inputBytes = std::as_bytes(std::span(input));
  const RenderTapeTextureInputAuditRequest request{
      .identity = kTexture, .afterCapturedSeqId = sources.rows().front().capturedSeqId,
      .digest = RenderTapeCaptureSession::sha256(inputBytes), .bytes = inputBytes};
  const RenderTapePreparedExecutionOptions options{
      .route = RenderTapePreparedRoute::ProductionSlot,
      .reset = RenderTapePreparedReset::FreshDevice,
      .sourceBoundaryMode = RenderTapeSourceBoundaryMode::CaptureSource,
      .sourceBoundaryIdentity = identity,
      .expectedSourcePlanDigest = sources.digest(),
      .hasExpectedSourcePlanDigest = true,
      .auditTextureInput = &request};
  for (unsigned fault = 0u; fault < 12u; ++fault) {
    auto bad = request;
    auto badOptions = options;
    badOptions.auditTextureInput = &bad;
    if (fault == 0u) ++bad.identity.generation;
    if (fault == 1u) ++bad.identity.objectId;
    if (fault == 2u) bad.identity.kind = D9C_CHUNK_HANDLE_KIND_BUFFER;
    if (fault == 3u) bad.afterCapturedSeqId = 0u;
    if (fault == 4u) bad.afterCapturedSeqId = sources.rows().back().capturedSeqId;
    if (fault == 5u) bad.digest[0] ^= std::byte{1u};
    if (fault == 6u) {
      bad.bytes = inputBytes.first(4u);
      bad.digest = RenderTapeCaptureSession::sha256(bad.bytes);
    }
    if (fault == 7u) bad.bytes = {};
    if (fault == 8u) badOptions.reset = RenderTapePreparedReset::WarmDevice;
    if (fault == 9u) badOptions.expectedSourcePlanDigest = {};
    if (fault == 10u) badOptions.sourceBoundaryMode = RenderTapeSourceBoundaryMode::Chunk;
    if (fault == 11u) badOptions.hasExpectedSourcePlanDigest = false;
    const auto result = replayPreparedRenderTapeIdentity(nullptr, prepared, badOptions);
    check(!result.complete() && !result.textureInputAudit.validated &&
              !result.textureInputAudit.attempted &&
              result.conservation.objectsCreated == 0u,
          "invalid texture input intervention fails before effects");
  }
  const GeneralIndexedFixture unsupported(32u, true, true, false, false, false, 21u);
  const auto unsupportedPrepared = prepareRenderTapeIdentity(unsupported.tape, unsupported.blobsValue);
  const auto unsupportedIdentity = sourceBoundaryFixtureIdentity(unsupported, true);
  render_tape_runtime::GeneralPreflightPlan unsupportedPlan;
  check(unsupportedPrepared.valid() && render_tape_runtime::buildGeneralReplayPlan(
            unsupported.tape, unsupported.blobsValue, unsupportedPlan).complete(),
        "unsupported audit format remains valid ordinary tape grammar");
  CaptureSourceReplayPlan unsupportedSources;
  check(buildCaptureSourceReplayPlan(unsupported.tape, unsupportedPlan,
            unsupportedIdentity, unsupportedSources) == CaptureSourceReplayPlanStatus::Valid,
        "unsupported audit format has valid source identity");
  auto unsupportedOptions = options;
  unsupportedOptions.sourceBoundaryIdentity = unsupportedIdentity;
  unsupportedOptions.expectedSourcePlanDigest = unsupportedSources.digest();
  const auto rejected = replayPreparedRenderTapeIdentity(nullptr, unsupportedPrepared, unsupportedOptions);
  check(rejected.status == FrameTapeReplayStatus::InvalidTape &&
            rejected.sourceReplay.validated && !rejected.textureInputAudit.validated &&
            rejected.conservation.objectsCreated == 0u,
        "BGRA is rejected by R32F intervention admission, not by unrelated source gates");
  const D9CPresentParams params{.backBufferWidth = 16u, .backBufferHeight = 16u,
      .backBufferFormat = 22u, .backBufferCount = 1u, .swapEffect = 1u,
      .windowed = 1u, .presentationInterval = 0x80000000u};
  auto* factory = dxmt9c_factory_create();
  check(factory != nullptr, "R32F intervention factory constructs");
  auto* device = dxmt9c_factory_create_device(factory, 0u, &params, 0u, nullptr);
  check(device != nullptr, "R32F intervention device constructs");
  const auto result = replayPreparedRenderTapeIdentity(device, prepared, options);
  dxmt9c_device_release(device);
  dxmt9c_factory_release(factory);
  const auto& audit = result.textureInputAudit;
  check(result.complete() && result.sourceReplay.complete && audit.requested &&
            audit.validated && audit.attempted && audit.uploaded && audit.readbackMatched &&
            audit.beforeDigest == RenderTapeCaptureSession::sha256(fixture.auditTextureSeed) &&
            audit.inputDigest == request.digest && audit.afterDigest == request.digest &&
            audit.byteCount == inputBytes.size() &&
            result.outputPixels == gpuPolicyExpectedPixels() &&
            result.conservation.objectsCreated == result.conservation.objectsReleased,
        "R32F intervention reads actual pre/post GPU bytes and preserves unrelated output");
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

void synchronousReferenceRejectsSeedBeforeEffects() {
  const GeneralIndexedFixture fixture(3u, true, false, true);
  const auto prepared = prepareRenderTapeIdentity(fixture.tape, fixture.blobsValue);
  check(prepared.valid(), "reference seed fixture prepares");
  const D9CPresentParams params{.backBufferWidth = 16u, .backBufferHeight = 16u,
      .backBufferFormat = 22u, .backBufferCount = 1u, .swapEffect = 1u,
      .windowed = 1u, .presentationInterval = 0x80000000u};
  std::unique_ptr<D9CFactory, decltype(&dxmt9c_factory_release)> factory(
      dxmt9c_factory_create(), dxmt9c_factory_release);
  check(bool(factory), "reference seed factory");
  std::unique_ptr<D9CDevice, decltype(&dxmt9c_device_release)> device(
      dxmt9c_factory_create_device(factory.get(), 0u, &params, 0u, nullptr),
      dxmt9c_device_release);
  check(device && device->dev().upperDevice() &&
            !device->dev().upperDevice()->usesAsyncQueryFences(),
        "isolated reference seed lane has signal-only initializer authority");
  const auto rejected = replayPreparedRenderTapeIdentity(device.get(), prepared, {});
  check(rejected.status == FrameTapeReplayStatus::UnsupportedGrammar &&
            rejected.conservation.objectsCreated == 0u &&
            rejected.queryReplay.failure == RenderTapeQueryReplayFailure::BackendUnavailable,
        "signal-only initializer reference rejects seed before tape effects");
}

void nativeMetalCapturedQueryTapeOracle() {
  const GeneralIndexedFixture fixture(3u, true, false, true,
                                      false, false, 0u, false, false, true);
  const auto prepared = prepareRenderTapeIdentity(fixture.tape, fixture.blobsValue);
  constexpr std::uint32_t alternateBackground = 0xff403020u;
  const GeneralIndexedFixture alternateFixture(3u, true, false, true,
      false, false, 0u, false, false, true, alternateBackground);
  const auto alternate = prepareRenderTapeIdentity(
      alternateFixture.tape, alternateFixture.blobsValue);
  check(prepared.valid() && alternate.valid() &&
            prepared.preflight().queryReplay.queryDefinitions == 1u &&
            prepared.preflight().queryReplay.issueRecords == 4u &&
            prepared.preflight().queryReplay.recordedPolls == 3u,
        "query-aware general admission preserves definitions/issues/original polls");
  const D9CPresentParams params{.backBufferWidth = 16u, .backBufferHeight = 16u,
      .backBufferFormat = 22u, .backBufferCount = 1u, .swapEffect = 1u,
      .windowed = 1u, .presentationInterval = 0x80000000u};
  for (const auto route : {RenderTapePreparedRoute::Compatibility,
                           RenderTapePreparedRoute::ProductionSlot}) {
    std::unique_ptr<D9CFactory, decltype(&dxmt9c_factory_release)> factory(
        dxmt9c_factory_create(), dxmt9c_factory_release);
    check(bool(factory), "query tape Metal factory");
    std::unique_ptr<D9CDevice, decltype(&dxmt9c_device_release)> device(
        dxmt9c_factory_create_device(factory.get(), 0u, &params, 0u, nullptr),
        dxmt9c_device_release);
    check(bool(device), "query tape Metal device");
    for (std::uint32_t iteration = 0u; iteration != 3u; ++iteration) {
      const bool changedSeed = iteration == 1u;
      const auto replay = replayPreparedRenderTapeIdentity(
          device.get(), changedSeed ? alternate : prepared, {
          .route = route,
          .reset = iteration == 0u ? RenderTapePreparedReset::FreshDevice
                                   : RenderTapePreparedReset::WarmDevice,
      });
      if (!replay.complete()) {
        std::cerr << "query failure=" << static_cast<unsigned>(replay.queryReplay.failure)
                  << " route=" << static_cast<unsigned>(route)
                  << " iteration=" << iteration
                  << '\n';
        for (const auto& row : replay.queryReplay.rows) {
          std::uint32_t captured = 0u, observed = 0u;
          std::memcpy(&captured, row.capturedBytes.data(), sizeof(captured));
          std::memcpy(&observed, row.replayBytes.data(), sizeof(observed));
          std::cerr << "query row event=" << row.eventIndex << " cycle=" << row.issueCycle
                    << " captured_hr=" << row.capturedResult << " replay_hr="
                    << row.replayResult << " oracle_hr=" << row.oracleResult
                    << " captured=" << captured << " observed=" << observed
                    << " local_seq=" << row.localSequence << " completed="
                    << row.completedSequence << " supplemental=" << row.oraclePolls << '\n';
        }
        if (replay.outputPixels.size() >= 16u * 4u) {
          std::cerr << "query output first row:";
          for (std::uint32_t x = 0u; x < 16u; ++x) {
            std::uint32_t pixel = 0u;
            std::memcpy(&pixel, replay.outputPixels.data() + x * 4u, sizeof(pixel));
            std::cerr << ' ' << std::hex << pixel << std::dec;
          }
          std::cerr << '\n';
        }
      }
      check(replay.complete(), std::string("query tape replay: ") +
          frameTapeReplayStatusName(replay.status) + " event=" +
          std::to_string(replay.failedEventIndex) +
          (replay.queryReplay.rows.empty() ? std::string{} :
              " samples-low-byte=" + std::to_string(std::to_integer<unsigned>(
                  replay.queryReplay.rows.back().replayBytes[0])) +
              " expected-low-byte=" + std::to_string(std::to_integer<unsigned>(
                  replay.queryReplay.rows.back().capturedBytes[0]))));
      const auto& query = replay.queryReplay;
      check(query.present && query.actualMetal && query.gpuResultOracle &&
                query.transcriptComplete && query.oracleComplete &&
                query.replayedIssueRecords == 4u && query.replayedPolls == 3u &&
                query.capturedPending == 1u && query.capturedReady == 2u &&
                query.readinessSatisfied == 2u && query.matchedResults == 2u &&
                query.rows.size() == 3u &&
                query.rows[1].issueCycle == 1u && query.rows[1].resultMatched &&
                query.rows[2].issueCycle == 2u && query.rows[2].resultMatched,
            "actual query results stay bound to both original issue cycles");
      check(replay.outputPixels == gpuVisibleUniformExpectedPixels(
                changedSeed ? alternateBackground : kGpuVisibleBackgroundColorARGB) &&
                replay.sourcePixels == replay.outputPixels &&
                replay.validity.expectedDigestMatched &&
                replay.conservation.objectsCreated == replay.conservation.objectsReleased,
            "query-aware replay preserves exact original source/output and wrapper conservation");
      check(query.rows[1].localQueryIdentity != 0u &&
                query.rows[1].localQueue != 0u &&
                query.rows[1].localSequence <= query.rows[1].completedSequence,
            "ready query oracle uses real local submission/completion");
    }
  }

  // A changed defined result still passes structural admission and must fail
  // the actual result oracle even though all original pixels are unchanged.
  auto corrupt = fixture.tape;
  RenderTapeHeader header{};
  std::memcpy(&header, corrupt.data(), sizeof(header));
  for (std::uint32_t i = 0u; i < header.eventCount; ++i) {
    RenderTapeEventHeader event{};
    std::memcpy(&event, corrupt.data() + header.eventTableOffset +
                           i * sizeof(event), sizeof(event));
    if (event.type != static_cast<std::uint32_t>(RenderTapeEventType::OrderedControl))
      continue;
    const auto offset = header.payloadArenaOffset + event.payloadOffset;
    RenderTapeOrderedControlHeader control{};
    std::memcpy(&control, corrupt.data() + offset, sizeof(control));
    if (control.resultCode != 0) continue;
    RenderTapeQueryGetDataControlV2 poll{};
    std::memcpy(&poll, corrupt.data() + offset + sizeof(control), sizeof(poll));
    if (poll.issueCycle != 1u) continue;
    poll.result[0] ^= std::byte{1u};
    std::memcpy(corrupt.data() + offset + sizeof(control), &poll, sizeof(poll));
    break;
  }
  const auto negative = prepareRenderTapeIdentity(corrupt, fixture.blobsValue);
  check(negative.valid(), "wrong query bytes remain a structurally valid negative");
  std::unique_ptr<D9CFactory, decltype(&dxmt9c_factory_release)> factory(
      dxmt9c_factory_create(), dxmt9c_factory_release);
  std::unique_ptr<D9CDevice, decltype(&dxmt9c_device_release)> device(
      dxmt9c_factory_create_device(factory.get(), 0u, &params, 0u, nullptr),
      dxmt9c_device_release);
  const auto mismatch = replayPreparedRenderTapeIdentity(device.get(), negative, {});
  check(mismatch.status == FrameTapeReplayStatus::QueryOracleFailed &&
            mismatch.queryReplay.failure == RenderTapeQueryReplayFailure::ResultMismatch,
        "actual query bytes reject an otherwise unchanged-pixel tape");

  const GeneralIndexedFixture pendingFixture(3u, true, false, true,
      false, false, 0u, false, false, true, kGpuVisibleBackgroundColorARGB,
      false, true);
  const auto pending = prepareRenderTapeIdentity(
      pendingFixture.tape, pendingFixture.blobsValue);
  check(pending.valid(), "Pending-only query fixture prepares");
  const auto pendingReplay = replayPreparedRenderTapeIdentity(device.get(), pending, {});
  const auto& pendingQuery = pendingReplay.queryReplay;
  check(pendingReplay.complete() && pendingQuery.present && pendingQuery.actualMetal &&
            pendingQuery.transcriptComplete && pendingQuery.oracleComplete &&
            !pendingQuery.gpuResultOracle && pendingQuery.capturedReady == 0u &&
            pendingQuery.capturedPending == 3u && pendingQuery.replayedPolls == 3u &&
            pendingQuery.readinessRequired == 0u && pendingQuery.requiredResults == 0u &&
            pendingReplay.outputPixels == gpuVisibleUniformExpectedPixels(),
        "Pending-only transcript preserves actual Metal/pixel evidence without a GPU query-result claim");
}

struct PagedBootstrapVolumeFixture {
  GeneralIndexedFixture source{3u, true, false, true, false, false,
      0u, false, false, true, kGpuVisibleBackgroundColorARGB, true};
  std::vector<std::byte> pixelShader{source.pixelShaderBytes};
  std::vector<RenderTapeProviderBlob> blobs{source.blobsValue};
  std::vector<std::byte> correct{};
  std::vector<std::byte> wrongTail{};

  PagedBootstrapVolumeFixture() {
    // Both sampler operands move from s0 to s5. Its state is beyond the first
    // sparse cap after six complete public sampler-state sets are sorted.
    const std::uint32_t declaration = 0xa00f0805u;
    const std::uint32_t operand = 0xa0e40805u;
    std::memcpy(pixelShader.data() + 3u * sizeof(declaration), &declaration, sizeof(declaration));
    std::memcpy(pixelShader.data() + 7u * sizeof(operand), &operand, sizeof(operand));
    const auto originalDigest = RenderTapeCaptureSession::sha256(source.pixelShaderBytes);
    const auto shaderDigest = RenderTapeCaptureSession::sha256(pixelShader);
    for (auto& blob : blobs)
      if (blob.digest == originalDigest) blob = {.digest = shaderDigest, .bytes = pixelShader};

    ImportedRenderTapeView tape;
    check(importPrevalidatedRenderTape(source.tape, tape), "paged Volume source imports");
    const auto rebuildState = [](const ImportedRecordView& record, std::uint32_t flags,
                                  bool completeConstants) {
      std::vector<std::vector<std::byte>> owned;
      owned.reserve(kRenderTapeStateCategoryCount);
      std::vector<std::pair<std::uint16_t, std::span<const std::byte>>> sections;
      for (std::size_t index = 0u; index < record.sections.size(); ++index) {
        const auto section = record.section(index);
        if (section.descriptor.kind == D9C_COMMAND_CHUNK_SECTION_SAMPLER_STATE) continue;
        owned.emplace_back(section.payload.begin(), section.payload.end());
        if (section.descriptor.kind == D9C_COMMAND_CHUNK_SECTION_TEXTURE) {
          auto* bindings = reinterpret_cast<D9CCommandChunkWireTextureBinding*>(owned.back().data());
          check(section.descriptor.count == D9C_DRAW_PACKET_MAX_TEXTURES,
                "paged shader retains the complete bound/null texture table");
          bindings[5u].handleIndex = bindings[0u].handleIndex;
          bindings[0u].handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX;
        }
        sections.emplace_back(section.descriptor.kind, owned.back());
      }
      if (completeConstants) {
        for (std::uint16_t kind = D9C_COMMAND_CHUNK_SECTION_VS_CONST_F;
             kind <= D9C_COMMAND_CHUNK_SECTION_PS_CONST_B; ++kind) {
          const auto* rule = sectionRule(kind);
          owned.emplace_back(sizeof(D9CCommandChunkWireConstantRange) +
                             rule->maxCount * rule->elementSize, std::byte{0});
          const D9CCommandChunkWireConstantRange range{
              .startRegister = 0u, .registerCount = rule->maxCount};
          std::memcpy(owned.back().data(), &range, sizeof(range));
          // A nonzero unreferenced final register catches truncated files;
          // c0 still comes from the original per-draw delta.
          owned.back().back() = std::byte{1u};
          sections.emplace_back(kind, owned.back());
        }
      }
      auto payload = sparsePayload(D9CCommandChunkWireDrawHeader{.flags = flags}, sections);
      auto* header = reinterpret_cast<D9CCommandChunkWireDrawHeader*>(payload.data());
      auto* descriptors = reinterpret_cast<D9CCommandChunkWireSectionDesc*>(
          payload.data() + header->sectionTableOffset);
      for (std::uint32_t index = 0u; index < header->sectionCount; ++index)
        if (descriptors[index].kind >= D9C_COMMAND_CHUNK_SECTION_VS_CONST_F &&
            descriptors[index].kind <= D9C_COMMAND_CHUNK_SECTION_PS_CONST_B)
          descriptors[index].count = sectionRule(descriptors[index].kind)->maxCount;
      return payload;
    };
    const auto build = [&](bool wrong) {
      RenderTapeBuilder builder;
      for (std::uint32_t index = 0u; index < tape.events.size(); ++index) {
        const auto event = tape.event(index);
        const auto type = static_cast<RenderTapeEventType>(event.header.type);
        if (type == RenderTapeEventType::BootstrapState) {
          RenderTapeBootstrapHeader fixed{};
          std::memcpy(&fixed, event.payload.data(), sizeof(fixed));
          D9CCommandChunkWireHeader header{};
          const auto bytes = event.payload.subspan(sizeof(fixed));
          std::memcpy(&header, bytes.data(), sizeof(header));
          ImportedChunkView chunk;
          check(importPrevalidatedCommandChunk(bytes, CommandChunkEnvelope{
              .recordCount = header.recordCount, .handleCount = header.handleCount}, chunk),
              "paged Volume bootstrap imports its original anchor");
          std::vector<Record> records{{.type = D9C_COMMAND_RECORD_APPLY_STATE,
              .payload = rebuildState(chunk.record(0u), D9C_COMMAND_CHUNK_DRAW_FLAG_FULL_SNAPSHOT, true),
              .handles = std::vector<D9CCommandChunkWireHandleEntry>(chunk.handles.begin(), chunk.handles.end())}};
          std::vector<D9CDrawPacketSamplerState> states;
          constexpr std::array values{3u, 3u, 3u, 0u, 1u, 1u, 1u, 0u, 0u, 1u, 0u, 0u, 0u};
          for (std::uint32_t sampler = 0u; sampler <= 5u; ++sampler)
            for (std::uint32_t state = 1u; state <= values.size(); ++state)
              states.push_back({sampler, state,
                  wrong && sampler == 5u && state == 3u ? 1u : values[state - 1u]});
          for (std::uint32_t pageIndex = 0u;; ++pageIndex) {
            const auto page = renderTapeBootstrapPageSpan(states.size(), D9C_DRAW_PACKET_MAX_SAMPLER, pageIndex);
            if (page.count == 0u) break;
            const std::array section{std::pair<std::uint16_t, std::span<const std::byte>>{
                D9C_COMMAND_CHUNK_SECTION_SAMPLER_STATE,
                std::as_bytes(std::span(states).subspan(page.first, page.count))}};
            records.push_back({.type = D9C_COMMAND_RECORD_APPLY_STATE,
                                .payload = sparsePayload({}, section)});
          }
          builder.appendBootstrapState(makeChunk(records));
        } else if (type == RenderTapeEventType::ObjectDefine) {
          RenderTapeObjectDefineHeader fixed{};
          std::memcpy(&fixed, event.payload.data(), sizeof(fixed));
          auto payload = std::vector<std::byte>(event.payload.begin(), event.payload.end());
          if (fixed.immutablePayloadDigest == originalDigest) {
            fixed.immutablePayloadDigest = shaderDigest;
            std::memcpy(payload.data(), &fixed, sizeof(fixed));
          }
          builder.appendRawEvent(type, payload);
        } else if (type == RenderTapeEventType::CommandChunk) {
          RenderTapeCommandChunkHeader fixed{};
          std::memcpy(&fixed, event.payload.data(), sizeof(fixed));
          ImportedChunkView chunk;
          check(importPrevalidatedCommandChunk(event.payload.subspan(sizeof(fixed)),
              CommandChunkEnvelope{.recordCount = fixed.recordCount, .handleCount = fixed.handleCount}, chunk),
              "paged Volume source commands import");
          std::vector<Record> records;
          std::uint32_t draw = 0u;
          for (std::uint32_t recordIndex = 0u; recordIndex < chunk.records.size(); ++recordIndex) {
            const auto record = chunk.record(recordIndex);
            Record copy{.type = record.header.type,
                .payload = std::vector<std::byte>(record.payload.begin(), record.payload.end()),
                .handles = std::vector<D9CCommandChunkWireHandleEntry>(record.handles.begin(), record.handles.end())};
            if (record.header.type == D9C_COMMAND_RECORD_APPLY_STATE)
              copy.payload = rebuildState(record, 0u, false);
            if (record.header.type == D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE) {
              auto* header = reinterpret_cast<D9CCommandChunkWireDrawHeader*>(copy.payload.data());
              auto* sections = reinterpret_cast<D9CCommandChunkWireSectionDesc*>(
                  copy.payload.data() + header->sectionTableOffset);
              for (std::uint32_t sectionIndex = 0u; sectionIndex < header->sectionCount; ++sectionIndex) {
                if (sections[sectionIndex].kind != D9C_COMMAND_CHUNK_SECTION_PS_CONST_F || draw >= 2u) continue;
                const float z = draw == 0u ? -1.f / 6.f : 7.f / 6.f;
                std::memcpy(copy.payload.data() + sections[sectionIndex].payloadOffset +
                    sizeof(D9CCommandChunkWireConstantRange) + 2u * sizeof(float), &z, sizeof(z));
              }
              ++draw;
            }
            records.push_back(std::move(copy));
          }
          builder.appendCommandChunk(CommandChunkEnvelope{
              .recordCount = fixed.recordCount, .handleCount = fixed.handleCount}, makeChunk(records));
        } else {
          builder.appendRawEvent(type, event.payload);
        }
      }
      return builder.seal();
    };
    correct = build(false);
    wrongTail = build(true);
  }
};

void nativeMetalPagedBootstrapTailControlsSampledPixels() {
  const PagedBootstrapVolumeFixture fixture;
  const auto correct = prepareRenderTapeIdentity(fixture.correct, fixture.blobs);
  const auto wrong = prepareRenderTapeIdentity(fixture.wrongTail, fixture.blobs);
  check(correct.valid() && wrong.valid(), "paged Volume correct/wrong-tail fixtures preflight");
  ImportedRenderTapeView tape;
  check(importPrevalidatedRenderTape(fixture.correct, tape), "paged Volume tape imports");
  const auto bootstrap = tape.event(0u).payload.subspan(sizeof(RenderTapeBootstrapHeader));
  D9CCommandChunkWireHeader header{};
  std::memcpy(&header, bootstrap.data(), sizeof(header));
  ImportedChunkView chunk;
  check(validateCommandChunk(bootstrap, CommandChunkEnvelope{
              .recordCount = header.recordCount, .handleCount = header.handleCount}, &chunk).valid() &&
            chunk.records.size() == 3u && chunk.record(1u).section(0u).descriptor.count == 64u &&
            chunk.record(2u).section(0u).descriptor.count == 14u,
        "real GPU fixture has one full anchor and exactly78 sampler rows in two capped deltas");
  const auto tail = chunk.record(2u).section(0u).payload;
  D9CDrawPacketSamplerState witness{};
  std::memcpy(&witness, tail.data() + 3u * sizeof(witness), sizeof(witness));
  check(witness.sampler == 5u && witness.type == 3u && witness.value == 3u,
        "sampled shader ADDRESSW CLAMP is deliberately in the last bootstrap page");
  std::uint32_t completeConstants = 0u;
  const auto anchor = chunk.record(0u);
  for (std::size_t index = 0u; index < anchor.sections.size(); ++index) {
    const auto section = anchor.section(index);
    if (section.descriptor.kind >= D9C_COMMAND_CHUNK_SECTION_VS_CONST_F &&
        section.descriptor.kind <= D9C_COMMAND_CHUNK_SECTION_PS_CONST_B) {
      check(section.descriptor.count == sectionRule(section.descriptor.kind)->maxCount &&
                section.payload.back() == std::byte{1u},
            "paged GPU anchor retains each full constant file and its final-register sentinel");
      ++completeConstants;
    }
    if (section.descriptor.kind == D9C_COMMAND_CHUNK_SECTION_TEXTURE) {
      D9CCommandChunkWireTextureBinding null{}, bound{};
      std::memcpy(&null, section.payload.data(), sizeof(null));
      std::memcpy(&bound, section.payload.data() + 5u * sizeof(bound), sizeof(bound));
      check(null.valid == 1u && null.handleIndex == D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX &&
                bound.valid == 1u && bound.handleIndex < chunk.handles.size() &&
                chunk.handles[bound.handleIndex].objectId == kTexture.objectId,
            "paged GPU anchor binds the actual sampled Volume while preserving explicit null slots");
    }
  }
  check(completeConstants == 6u, "paged GPU anchor preserves all six complete constant files");
  const D9CPresentParams params{.backBufferWidth = 16u, .backBufferHeight = 16u,
      .backBufferFormat = 22u, .backBufferCount = 1u, .swapEffect = 1u,
      .windowed = 1u, .presentationInterval = 0x80000000u};
  for (const auto route : {RenderTapePreparedRoute::Compatibility, RenderTapePreparedRoute::ProductionSlot}) {
    std::unique_ptr<D9CFactory, decltype(&dxmt9c_factory_release)> factory(dxmt9c_factory_create(), dxmt9c_factory_release);
    std::unique_ptr<D9CDevice, decltype(&dxmt9c_device_release)> device(
        dxmt9c_factory_create_device(factory.get(), 0u, &params, 0u, nullptr), dxmt9c_device_release);
    check(device != nullptr, "paged Volume actual Metal device constructs");
    for (std::uint32_t iteration = 0u; iteration < 3u; ++iteration) {
      const bool incorrect = iteration == 1u;
      const auto before = dxmt9::perf::snapshotRenderTapeReplay();
      const auto replay = replayPreparedRenderTapeIdentity(device.get(), incorrect ? wrong : correct, {
          .route = route,
          .reset = iteration == 0u ? RenderTapePreparedReset::FreshDevice : RenderTapePreparedReset::WarmDevice});
      const auto after = dxmt9::perf::snapshotRenderTapeReplay();
      check(after.gpuErrors == before.gpuErrors &&
            (route == RenderTapePreparedRoute::Compatibility
                ? after.compatibilityDraws - before.compatibilityDraws == 3u &&
                  after.directLeaseDraws == before.directLeaseDraws
                : after.directLeaseDraws - before.directLeaseDraws == 3u &&
                  after.compatibilityDraws == before.compatibilityDraws),
            "paged bootstrap GPU fixture executes all3 draws through its selected production route");
      const auto expected = gpuVisibleUniformExpectedPixels();
      if (incorrect) {
        auto swapped = expected;
        for (std::size_t y = 0u; y < 16u; ++y) {
          for (std::size_t x = 0u; x < 16u; ++x) {
            if (x >= 4u && (x < 6u || x >= 10u)) continue;
            const auto color = x < 4u ? kGpuVisibleColorBARGB : kGpuVisibleColorAARGB;
            std::memcpy(swapped.data() + (y * 16u + x) * sizeof(color), &color, sizeof(color));
          }
        }
        check(replay.status == FrameTapeReplayStatus::OutputMismatch &&
                  replay.validity.expectedDigestCaptured && !replay.validity.expectedDigestMatched &&
                  replay.outputPixels != expected && replay.outputPixels == swapped,
              "wrong last-page ADDRESSW WRAP swaps exact sampled stripes and fails the unchanged original oracle");
      } else {
        check(replay.complete() && replay.outputPixels == expected &&
                  replay.sourcePixels == expected && replay.validity.expectedDigestMatched &&
                  replay.queryReplay.actualMetal && replay.queryReplay.gpuResultOracle &&
                  replay.queryReplay.matchedResults == 2u,
              "cold/warm paged bootstrap preserves exact original mip/z pixels and real192/zero query results");
      }
      check(replay.conservation.objectsCreated == replay.conservation.objectsReleased,
            "paged correct/negative/warm recovery conserve every runtime wrapper");
    }
  }
}

void writePagedBootstrapVolumeFixture(const std::filesystem::path& directory) {
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  check(!error, "paged Volume fixture directory creates");
  const PagedBootstrapVolumeFixture fixture;
  const auto write = [&](std::string_view name, std::span<const std::byte> bytes) {
    std::ofstream output(directory / std::string(name), std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    check(output.good(), "paged Volume fixture artifact writes");
  };
  write("events.bin", fixture.correct);
  write("wrong-tail.events.bin", fixture.wrongTail);
  write("output.rgba", gpuVisibleUniformExpectedPixels());
  write("source.rgba", gpuVisibleUniformExpectedPixels());
  constexpr std::array names{
      "vertex.bin", "index.bin", "shader.bin", "pixel.bin", "declaration.bin",
      "output.bin", "volume0.bin", "volume1.bin", "volume2.bin"};
  check(fixture.blobs.size() == names.size(), "paged Volume artifact blob names match canonical catalogue");
  for (std::size_t index = 0u; index < names.size(); ++index) write(names[index], fixture.blobs[index].bytes);
}

void nativeMetalVolumeMipSliceOracle() {
  const GeneralIndexedFixture fixture(3u, true, false, true, false, false,
      0u, false, false, true, kGpuVisibleBackgroundColorARGB, true);
  const auto prepared = prepareRenderTapeIdentity(fixture.tape, fixture.blobsValue);
  check(prepared.valid(), std::string("volume mip/slice fixture prepares: ") +
      frameTapeReplayStatusName(prepared.preflight().status));
  const D9CPresentParams params{.backBufferWidth = 16u, .backBufferHeight = 16u,
      .backBufferFormat = 22u, .backBufferCount = 1u, .swapEffect = 1u,
      .windowed = 1u, .presentationInterval = 0x80000000u};
  for (const auto route : {RenderTapePreparedRoute::Compatibility,
                           RenderTapePreparedRoute::ProductionSlot}) {
    std::unique_ptr<D9CFactory, decltype(&dxmt9c_factory_release)> factory(
        dxmt9c_factory_create(), dxmt9c_factory_release);
    check(bool(factory), "volume oracle Metal factory");
    std::unique_ptr<D9CDevice, decltype(&dxmt9c_device_release)> device(
        dxmt9c_factory_create_device(factory.get(), 0u, &params, 0u, nullptr),
        dxmt9c_device_release);
    check(bool(device), "volume oracle Metal device");
    for (std::uint32_t iteration = 0u; iteration < 2u; ++iteration) {
      const auto before = dxmt9::perf::snapshotRenderTapeReplay();
      const auto replay = replayPreparedRenderTapeIdentity(device.get(), prepared, {
          .route = route,
          .reset = iteration == 0u ? RenderTapePreparedReset::FreshDevice
                                   : RenderTapePreparedReset::WarmDevice});
      const auto after = dxmt9::perf::snapshotRenderTapeReplay();
      check(replay.complete(), std::string("volume mip/slice replay: ") +
          frameTapeReplayStatusName(replay.status) + " event=" +
          std::to_string(replay.failedEventIndex));
      check(replay.outputPixels == gpuVisibleUniformExpectedPixels() &&
                replay.sourcePixels == replay.outputPixels &&
                replay.validity.expectedDigestMatched &&
                replay.queryReplay.actualMetal && replay.queryReplay.gpuResultOracle &&
                replay.queryReplay.matchedResults == 2u &&
                replay.conservation.objectsCreated == replay.conservation.objectsReleased,
            "real Metal samples mip0 z0/z2 and mip1 with exact original pixels/query results");
      const auto direct = after.directLeaseDraws - before.directLeaseDraws;
      const auto compatibility = after.compatibilityDraws - before.compatibilityDraws;
      check(after.gpuErrors == before.gpuErrors &&
                (route == RenderTapePreparedRoute::Compatibility
                     ? compatibility == 3u && direct == 0u
                     : direct == 3u && compatibility == 0u),
            "volume oracle executes the selected production route without GPU errors");
    }
  }

  for (const std::uint32_t mip : {0u, 1u}) {
    // Authenticate a wrong z-slice/mip seed while preserving the original
    // independent pixel oracle. A source/output agreement alone cannot pass.
    auto wrongSeed = fixture.volumeSeeds[mip];
    const auto oldDigest = RenderTapeCaptureSession::sha256(wrongSeed);
    const auto color = mip == 0u ? kGpuVisibleColorAARGB : kGpuVisibleColorBARGB;
    const auto begin = mip == 0u ? 2u * 7u * 5u * 4u : 0u;
    for (std::size_t offset = begin; offset < wrongSeed.size(); offset += 4u)
      std::memcpy(wrongSeed.data() + offset, &color, sizeof(color));
    const auto digest = RenderTapeCaptureSession::sha256(wrongSeed);
    auto blobs = fixture.blobsValue;
    for (auto& blob : blobs)
      if (blob.digest == oldDigest) blob = {.digest = digest, .bytes = wrongSeed};
    auto tape = fixture.tape;
    ImportedRenderTapeView imported;
    check(importPrevalidatedRenderTape(tape, imported), "wrong volume fixture imports");
    for (std::uint32_t index = 0u; index < imported.header.eventCount; ++index) {
      const auto event = imported.event(index);
      if (event.header.type != static_cast<std::uint32_t>(
                                    RenderTapeEventType::ResourceMutation)) continue;
      RenderTapeResourceMutationHeader mutation{};
      std::memcpy(&mutation, event.payload.data(), sizeof(mutation));
      if (mutation.identity.kind != kTexture.kind ||
          mutation.identity.objectId != kTexture.objectId || mutation.subresource != mip)
        continue;
      mutation.digest = digest;
      std::memcpy(tape.data() + imported.header.payloadArenaOffset +
                      event.header.payloadOffset, &mutation, sizeof(mutation));
    }
    const auto wrong = prepareRenderTapeIdentity(tape, blobs);
    check(wrong.valid(), "wrong volume sample seed remains fully authenticated");
    std::unique_ptr<D9CFactory, decltype(&dxmt9c_factory_release)> factory(
        dxmt9c_factory_create(), dxmt9c_factory_release);
    std::unique_ptr<D9CDevice, decltype(&dxmt9c_device_release)> device(
        dxmt9c_factory_create_device(factory.get(), 0u, &params, 0u, nullptr),
        dxmt9c_device_release);
    const auto first = replayPreparedRenderTapeIdentity(device.get(), prepared, {});
    check(first.complete(), "volume A starts from the exact authenticated seed");
    const auto rejected = replayPreparedRenderTapeIdentity(device.get(), wrong, {
        .reset = RenderTapePreparedReset::WarmDevice});
    check(rejected.status == FrameTapeReplayStatus::OutputMismatch &&
              rejected.validity.outputReadback && !rejected.validity.expectedDigestMatched &&
              rejected.outputPixels != gpuVisibleUniformExpectedPixels(),
          "wrong z-slice or mip bytes fail exact original GPU pixels");
    const auto restored = replayPreparedRenderTapeIdentity(device.get(), prepared, {
        .reset = RenderTapePreparedReset::WarmDevice});
    check(restored.complete() && restored.outputPixels == first.outputPixels &&
              restored.queryReplay.gpuResultOracle &&
              restored.conservation.objectsCreated == restored.conservation.objectsReleased,
          "volume A/B/A restores every mip/slice after changed-content failure");
  }
}

void nativeMetalRetainedQueryResultSurvivesReuseAndRelease(
    bool expectPollBudget = false) {
  using namespace dxmt9::core;
  const D9CPresentParams params{.backBufferWidth = 16u, .backBufferHeight = 16u,
      .backBufferFormat = 21u, .backBufferCount = 1u, .swapEffect = 1u,
      .windowed = 1u, .presentationInterval = 0x80000000u};
  std::unique_ptr<D9CFactory, decltype(&dxmt9c_factory_release)> factory(
      dxmt9c_factory_create(), dxmt9c_factory_release);
  std::unique_ptr<D9CDevice, decltype(&dxmt9c_device_release)> wire(
      dxmt9c_factory_create_device(factory.get(), 0u, &params, 0u, nullptr),
      dxmt9c_device_release);
  check(bool(wire), "retained query actual device");
  auto& device = wire->dev();
  auto upper = device.upperDevice();
  check(upper && upper->usesMetalOcclusion(), "retained query requires GPU visibility");
  auto& queue = upper->queue();
  check(device.setFVF(0x4002u | 0x040u) == D3D_OK &&
            device.setRenderState(RS_CULL_MODE, u32(CullMode::None)) == D3D_OK &&
            device.setRenderState(RS_LIGHTING, 0u) == D3D_OK &&
            device.setRenderState(RS_Z_ENABLE, 0u) == D3D_OK &&
            device.setViewport({0u, 0u, 16u, 16u, 0.f, 1.f}) == D3D_OK,
        "retained query fixed state");
  struct Vertex { float x, y, z, w; u32 color; };
  const std::array<Vertex, 6u> vertices{{
      {-2,-2,.5f,1,0xff00ff00}, {-2,2,.5f,1,0xff00ff00}, {2,-2,.5f,1,0xff00ff00},
      {2,-2,.5f,1,0xff00ff00}, {-2,2,.5f,1,0xff00ff00}, {2,2,.5f,1,0xff00ff00},
  }};
  auto vb = device.createBuffer({sizeof(vertices), Pool::Default, UsageVertexBuffer});
  check(bool(vb), "retained query vertex buffer");
  const auto mapped = vb->lock(0u, sizeof(vertices), 0u);
  check(mapped.data != nullptr, "retained query vertex map");
  std::memcpy(mapped.data, vertices.data(), sizeof(vertices));
  vb->unlock();
  check(device.setStreamSource(0u, vb, 0u, sizeof(Vertex)) == D3D_OK,
        "retained query stream");
  ClearDesc clear{};
  clear.clearColor = true;
  clear.color = {1.f, 0.f, 0.f, 1.f};
  check(device.clear(clear) == D3D_OK, "retained query original red clear");
  upper->flush();
  struct Gate {
    WMT::Reference<WMT::SharedEvent> event;
    ~Gate() { if (event) event.signalValue(1u); }
  } gate{queue.device().newSharedEvent()};
  check(bool(gate.event), "real completion gate");
  auto blocker = queue.newCommandBuffer();
  blocker.encodeWaitForEvent(WMT::Event{gate.event.handle}, 1u);
  blocker.commit();
  std::unique_ptr<D9CQuery, decltype(&dxmt9c_query_release)> query(
      dxmt9c_device_create_query(wire.get(), 9u), dxmt9c_query_release);
  check(bool(query), "retained query wrapper");
  check(dxmt9c_query_issue(query.get(), 2u) == D3D_OK &&
            device.drawPrimitive(PrimitiveType::TriangleList, 2u) == D3D_OK &&
            dxmt9c_query_issue(query.get(), 1u) == D3D_OK,
        "old query issue/draw/end");
  std::uint32_t pending = 0xdeadbeefu;
  check(dxmt9c_query_get_data(query.get(), &pending, sizeof(pending), 1u) == S_FALSE &&
            pending == 0xdeadbeefu,
        "actual shared-event gate deterministically preserves old Pending");
  const auto snapshot = device.captureQueryPollSnapshot(query->obj);
  RenderTapeQueryReplayRow row{
      .issueCycle = 1u, .hasOutput = true, .dataSize = 4u,
      .capturedResult = S_OK, .replayResult = S_FALSE, .definedBytes = 4u,
      .localQueryIdentity = snapshot.identity, .localIssueOrdinal = snapshot.issueOrdinal,
      .localDeviceEpoch = snapshot.issueDeviceEpoch, .resultRequired = true,
  };
  const std::uint32_t expected = 256u;
  std::memcpy(row.capturedBytes.data(), &expected, sizeof(expected));
  check(dxmt9c_query_issue(query.get(), 2u) == D3D_OK &&
            dxmt9c_query_issue(query.get(), 1u) == D3D_OK,
        "reissue replaces public result with empty cycle");
  query.reset();
  const auto oracleOnlyDrain = [&]() {
    const auto began = std::chrono::steady_clock::now();
    gate.event.signalValue(1u);
    upper->flush();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - began).count());
  };
  // Ordinary retention checks semantic ownership after actual completion, not
  // OS scheduling within a finite spin count. The deterministic Pending call
  // above remains before reissue/release; this drain is separate oracle work.
  auto oracleDrainNanos = expectPollBudget ? 0u : oracleOnlyDrain();
  RenderTapeQueryReplayEvidence evidence{.readinessRequired = 1u};
  auto settled = render_tape_runtime::settleQueryOracle(
      wire.get(), snapshot, row, evidence);
  if (settled != RenderTapeQueryReplayFailure::None || !row.resultMatched) {
    std::uint32_t observed = 0u;
    std::memcpy(&observed, row.replayBytes.data(), sizeof(observed));
    std::cerr << "query retained failure=" << static_cast<unsigned>(settled)
              << " samples=" << observed << " expected=" << expected
              << " local_seq=" << row.localSequence << " completed="
              << row.completedSequence << " oracle_hr=" << row.oracleResult
              << " polls=" << row.oraclePolls << " eligible=" << snapshot.eligible
              << " progress_limit=" << static_cast<unsigned>(evidence.progressLimit)
              << " aggregate_polls=" << evidence.supplementalPolls
              << " poll_budget=" << evidence.maxOraclePolls
              << " wall_ns=" << evidence.oracleWallNanos
              << " wall_budget_ns=" << evidence.maxOracleWallNanos
              << " fence_valid=" << snapshot.backendFence.valid()
              << " marker_seq=" << snapshot.backendFence.snapshot().sequence << '\n';
    const auto progress = snapshot.backend ? snapshot.backend->queryProgress()
                                          : dxmt9::query::Progress{};
    std::cerr << "query retained backend_failed=" << progress.failed
              << " marker_cancelled=" << (snapshot.backendFence.marker &&
                  snapshot.backendFence.marker->cancelled())
              << " blocker_status=" << blocker.status();
    if (snapshot.occlusionResult) {
      const auto& result = *snapshot.occlusionResult;
      std::cerr << " result_samples=" << result.samples.load(std::memory_order_acquire)
                << " result_pending=" << result.pending.load(std::memory_order_acquire)
                << " result_failed=" << result.failed.load(std::memory_order_acquire)
                << " result_abandoned=" << result.abandoned.load(std::memory_order_acquire)
                << " result_object=" << result.object << " result_issue=" << result.issue
                << " result_epoch=" << result.epoch;
    }
    std::cerr << '\n';
  }
  if (expectPollBudget) {
    // Keep the real event closed for the bounded probe, so this classifier
    // does not depend on how quickly the OS schedules its completion thread.
    check(settled == RenderTapeQueryReplayFailure::ProgressExhausted &&
              evidence.progressLimit == RenderTapeQueryProgressLimit::PollBudget &&
              evidence.supplementalPolls == evidence.maxOraclePolls &&
              !row.readinessSatisfied && !row.resultMatched &&
              snapshot.occlusionResult &&
              !snapshot.occlusionResult->failed.load(std::memory_order_acquire),
          "bounded real-event probe reports PollBudget without accepting a result");
    const auto initialPolls = evidence.supplementalPolls;
    const auto initialWallNanos = evidence.oracleWallNanos;
    oracleDrainNanos = oracleOnlyDrain();
    evidence = RenderTapeQueryReplayEvidence{.readinessRequired = 1u};
    row.oraclePolls = 0u;
    settled = render_tape_runtime::settleQueryOracle(wire.get(), snapshot, row, evidence);
    std::cerr << "query retained recovery initial_failure=poll-budget initial_accepted=0"
              << " initial_polls=" << initialPolls << " initial_wall_ns=" << initialWallNanos
              << " oracle_drain_ns=" << oracleDrainNanos
              << " recovery_polls=" << evidence.supplementalPolls
              << " recovery_issue=" << row.localIssueOrdinal
              << " recovery_identity=" << row.localQueryIdentity << '\n';
  }
  check(oracleDrainNanos != 0u, "retained query accounts for its separate real oracle drain");
  check(settled ==
            RenderTapeQueryReplayFailure::None && row.resultMatched &&
            row.readinessSatisfied && row.supplemental && row.oraclePolls > 0u &&
            row.localSequence <= row.completedSequence,
        "old immutable actual result settles after public reuse/release");
  auto wrong = row;
  wrong.capturedBytes[0] ^= std::byte{1u};
  check(render_tape_runtime::settleQueryOracle(wire.get(), snapshot, wrong, evidence) ==
            RenderTapeQueryReplayFailure::ResultMismatch,
        "same-cycle result negative detects wrong expected samples");
  auto stale = row;
  ++stale.localIssueOrdinal;
  check(render_tape_runtime::settleQueryOracle(wire.get(), snapshot, stale, evidence) ==
            RenderTapeQueryReplayFailure::SnapshotUnavailable,
        "stale local issue identity cannot poll a different cycle");
  RenderTapeQueryReplayEvidence exhausted{.maxOraclePolls = 0u};
  check(render_tape_runtime::settleQueryOracle(wire.get(), snapshot, row, exhausted) ==
            RenderTapeQueryReplayFailure::ProgressExhausted &&
            exhausted.progressLimit == RenderTapeQueryProgressLimit::PollBudget,
        "bounded final oracle exhaustion has a named failure");
  RenderTapeQueryReplayEvidence wallExhausted{.maxOracleWallNanos = 0u};
  check(render_tape_runtime::settleQueryOracle(wire.get(), snapshot, row, wallExhausted) ==
            RenderTapeQueryReplayFailure::ProgressExhausted &&
            wallExhausted.progressLimit == RenderTapeQueryProgressLimit::WallBudget,
        "wall-time oracle exhaustion stays distinct from poll-count exhaustion");
}

void nativeMetalVisibleUniformPolicyOracle() {
  const GeneralIndexedFixture fixture(3u, true, false, true);
  const auto prepared = prepareRenderTapeIdentity(fixture.tape,
                                                   fixture.blobsValue);
  const auto expected = gpuVisibleUniformExpectedPixels();
  check(prepared.valid() && prepared.evidence().immutableInputOwned,
        "visible-uniform policy owns its prepared fixture");
  render_tape_runtime::GeneralPreflightPlan plan;
  const auto preflight = render_tape_runtime::buildGeneralReplayPlan(
      fixture.tape, fixture.blobsValue, plan);
  check(preflight.complete() &&
            preflight.coverage.drawIndexedPrimitiveRecords == 3u,
        "visible-uniform policy preflight completes");

  const D9CPresentParams params{.backBufferWidth = 16u,
                                .backBufferHeight = 16u,
                                .backBufferFormat = 22u,
                                .backBufferCount = 1u,
                                .swapEffect = 1u,
                                .windowed = 1u,
                                .presentationInterval = 0x80000000u};
  for (const auto route : {RenderTapePreparedRoute::Compatibility,
                           RenderTapePreparedRoute::ProductionSlot}) {
    auto* factory = dxmt9c_factory_create();
    check(factory != nullptr, "visible-uniform policy factory constructs");
    auto* device =
        dxmt9c_factory_create_device(factory, 0u, &params, 0u, nullptr);
    check(device != nullptr, "visible-uniform policy device constructs");
    const auto before = dxmt9::perf::snapshotRenderTapeReplay();
    const auto result = replayPreparedRenderTapeIdentity(
        device, prepared,
        RenderTapePreparedExecutionOptions{
            .route = route, .reset = RenderTapePreparedReset::FreshDevice});
    const auto after = dxmt9::perf::snapshotRenderTapeReplay();
    dxmt9c_device_release(device);
    dxmt9c_factory_release(factory);
    check(result.complete() && result.validity.outputReadback &&
              result.validity.expectedDigestMatched && result.outputPixels == expected &&
              result.coverage.drawIndexedPrimitiveRecords == 3u &&
              result.conservation.objectsCreated == result.conservation.objectsReleased,
          "visible-uniform policy retains the independent three-draw pixel oracle");
    const auto compatibilityDraws =
        after.compatibilityDraws - before.compatibilityDraws;
    const auto directDraws = after.directLeaseDraws - before.directLeaseDraws;
    const auto gpuErrors = after.gpuErrors - before.gpuErrors;
    if (route == RenderTapePreparedRoute::Compatibility) {
      check(compatibilityDraws == 3u && directDraws == 0u && gpuErrors == 0u,
            "visible-uniform compatibility executes all three draws");
    } else {
      check(directDraws == 3u && compatibilityDraws == 0u && gpuErrors == 0u,
            "visible-uniform ProductionSlot executes all three draws");
    }
  }

  const GeneralIndexedFixture altered(3u, true, false, true, true);
  const auto alteredPrepared = prepareRenderTapeIdentity(altered.tape,
                                                          altered.blobsValue);
  auto* factory = dxmt9c_factory_create();
  check(factory != nullptr, "altered visible-uniform policy factory constructs");
  auto* device = dxmt9c_factory_create_device(factory, 0u, &params, 0u, nullptr);
  check(device != nullptr, "altered visible-uniform policy device constructs");
  const auto mismatch = replayPreparedRenderTapeIdentity(
      device, alteredPrepared,
      RenderTapePreparedExecutionOptions{
          .route = RenderTapePreparedRoute::Compatibility,
          .reset = RenderTapePreparedReset::FreshDevice});
  dxmt9c_device_release(device);
  dxmt9c_factory_release(factory);
  check(!mismatch.complete() && mismatch.validity.outputReadback &&
            !mismatch.validity.expectedDigestMatched &&
            mismatch.outputPixels != expected,
        "altered middle uniform fails against the original independent oracle");

  const GeneralIndexedFixture staleVs(3u, true, false, true, false, true);
  const auto staleVsPrepared = prepareRenderTapeIdentity(staleVs.tape,
                                                          staleVs.blobsValue);
  factory = dxmt9c_factory_create();
  check(factory != nullptr, "stale VS visible-uniform factory constructs");
  device = dxmt9c_factory_create_device(factory, 0u, &params, 0u, nullptr);
  check(device != nullptr, "stale VS visible-uniform device constructs");
  const auto staleVsMismatch = replayPreparedRenderTapeIdentity(
      device, staleVsPrepared,
      RenderTapePreparedExecutionOptions{
          .route = RenderTapePreparedRoute::Compatibility,
          .reset = RenderTapePreparedReset::FreshDevice});
  dxmt9c_device_release(device);
  dxmt9c_factory_release(factory);
  check(!staleVsMismatch.complete() && staleVsMismatch.validity.outputReadback &&
            !staleVsMismatch.validity.expectedDigestMatched &&
            staleVsMismatch.outputPixels != expected,
        "stale middle VS transform fails against the original independent oracle");
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

void pagedBootstrapOutputUsesFinalOrderedAttachment() {
  const auto implicit = implicitBootstrapChunk();
  ImportedChunkView imported;
  check(importPrevalidatedCommandChunk(implicit, CommandChunkEnvelope{.recordCount = 1u}, imported),
        "paged attachment anchor imports");
  const auto fullPayload = imported.record(0u).payload;
  const Record anchor{.type = D9C_COMMAND_RECORD_APPLY_STATE,
      .payload = std::vector<std::byte>(fullPayload.begin(), fullPayload.end())};
  const D9CCommandChunkWireRenderState state{.state = 7u, .value = 1u};
  const std::array stateSection{std::pair<std::uint16_t, std::span<const std::byte>>{
      D9C_COMMAND_CHUNK_SECTION_RENDER_STATE, std::as_bytes(std::span(&state, 1u))}};
  const Record preserve{.type = D9C_COMMAND_RECORD_APPLY_STATE,
      .payload = sparsePayload({}, stateSection)};
  const auto target = [&](std::uint32_t symbol, std::uint32_t handleIndex) {
    const D9CCommandChunkWireRenderTargetBinding binding{
        .slot = 0u, .valid = 1u,
        .handleIndex = symbol == 2u ? D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX : handleIndex};
    const std::array section{std::pair<std::uint16_t, std::span<const std::byte>>{
        D9C_COMMAND_CHUNK_SECTION_RENDER_TARGET, std::as_bytes(std::span(&binding, 1u))}};
    Record record{.type = D9C_COMMAND_RECORD_APPLY_STATE, .payload = sparsePayload({}, section)};
    if (symbol != 2u)
      record.handles.push_back({kOutput.kind,
          kOutput.generation + (symbol == 3u ? 1u : 0u), kOutput.objectId});
    return record;
  };
  for (std::uint32_t length = 1u, words = 4u; length <= 3u; ++length, words *= 4u) {
    for (std::uint32_t word = 0u; word < words; ++word) {
      std::vector<Record> records{anchor};
      auto expected = FrameTapeBootstrapOutputDisposition::ImplicitDefault;
      std::uint32_t handleCount = 0u;
      for (std::uint32_t index = 0u, digits = word; index < length; ++index, digits /= 4u) {
        const auto symbol = digits % 4u;
        if (symbol == 0u) {
          records.push_back(preserve);
          continue;
        }
        records.push_back(target(symbol, handleCount));
        handleCount += records.back().handles.size();
        expected = symbol == 1u ? FrameTapeBootstrapOutputDisposition::ExplicitExact
            : symbol == 2u ? FrameTapeBootstrapOutputDisposition::ExplicitNull
                           : FrameTapeBootstrapOutputDisposition::WrongIdentity;
      }
      const auto bytes = makeChunk(records);
      check(validateCommandChunk(bytes, CommandChunkEnvelope{
                  .recordCount = static_cast<std::uint32_t>(records.size()),
                  .handleCount = handleCount}).valid(),
            "ordered attachment checker uses canonical records and generation-qualified handle slices");
      check(classifyFrameTapeBootstrapOutput(bytes, CommandChunkEnvelope{
                  .recordCount = static_cast<std::uint32_t>(records.size()),
                  .handleCount = handleCount}, kOutput) == expected,
            "every small ordered RT0 trace classifies the final effective delta, not the first binding");
    }
  }
  const auto sparseOnly = makeChunk(std::array{preserve});
  check(classifyFrameTapeBootstrapOutput(sparseOnly,
            CommandChunkEnvelope{.recordCount = 1u}, kOutput) ==
            FrameTapeBootstrapOutputDisposition::Malformed,
        "paged attachment classifier requires the first full anchor");
  const auto explicitBytes = bootstrapChunk();
  check(importPrevalidatedCommandChunk(explicitBytes,
            CommandChunkEnvelope{.recordCount = 1u, .handleCount = 1u}, imported),
        "legacy explicit attachment imports");
  const auto explicitPayload = imported.record(0u).payload;
  const Record explicitAnchor{.type = D9C_COMMAND_RECORD_APPLY_STATE,
      .payload = std::vector<std::byte>(explicitPayload.begin(), explicitPayload.end()),
      .handles = {{kOutput.kind, kOutput.generation, kOutput.objectId}}};
  const auto reset = makeChunk(std::array{explicitAnchor, anchor});
  check(classifyFrameTapeBootstrapOutput(reset,
            CommandChunkEnvelope{.recordCount = 2u, .handleCount = 1u}, kOutput,
            kRenderTapeLegacyBaselineProfileVersion) ==
            FrameTapeBootstrapOutputDisposition::ImplicitDefault &&
            classifyFrameTapeBootstrapOutput(reset,
                CommandChunkEnvelope{.recordCount = 2u, .handleCount = 1u}, kOutput) ==
                FrameTapeBootstrapOutputDisposition::Malformed,
        "legacy later FULL resets implicit RT0 while profile3 rejects a repeated anchor");
  const Record emptyDelta{.type = D9C_COMMAND_RECORD_APPLY_STATE,
      .payload = sparsePayload({}, {})};
  check(classifyFrameTapeBootstrapOutput(makeChunk(std::array{anchor, emptyDelta}),
            CommandChunkEnvelope{.recordCount = 2u}, kOutput) ==
            FrameTapeBootstrapOutputDisposition::Malformed,
        "effect-empty bootstrap continuation fails attachment admission");

  // Exercise the actual compact provider planner, not only the classifier.
  ProductionFixture fixture;
  ImportedRenderTapeView original;
  check(importPrevalidatedRenderTape(fixture.tape, original), "compact paged fixture imports");
  const auto paged = makeChunk(std::array{anchor, preserve});
  const RenderTapeBootstrapHeader header{.overlayCount = 1u};
  auto payload = bytesOf(header);
  payload.insert(payload.end(), paged.begin(), paged.end());
  RenderTapeBuilder builder;
  builder.appendRawEvent(RenderTapeEventType::BootstrapState, payload);
  for (std::uint32_t index = 1u; index < original.events.size(); ++index) {
    const auto event = original.event(index);
    builder.appendRawEvent(static_cast<RenderTapeEventType>(event.header.type), event.payload);
  }
  const auto tape = builder.seal();
  const auto admitted = preflightFrameTapeIdentity(tape, {});
  check(admitted.complete() && admitted.coverage.bootstrapChunks == 1u,
        "compact provider preflight accepts one canonical chunk with ordered bootstrap pages");
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
  GeneralIndexedFixture fixture(1u, true);
  auto* tapeHeader = reinterpret_cast<RenderTapeHeader*>(fixture.tape.data());
  auto* events = reinterpret_cast<RenderTapeEventHeader*>(
      fixture.tape.data() + tapeHeader->eventTableOffset);
  bool digestCorrupted = false;
  for (std::uint32_t event = 0u; event < tapeHeader->eventCount; ++event) {
    if (events[event].type !=
        static_cast<std::uint32_t>(RenderTapeEventType::PresentComplete)) {
      continue;
    }
    auto* complete = reinterpret_cast<RenderTapePresentCompleteHeader*>(
        fixture.tape.data() + tapeHeader->payloadArenaOffset +
        events[event].payloadOffset);
    complete->expectedDigest[0] ^= std::byte{1u};
    digestCorrupted = true;
    break;
  }
  check(digestCorrupted, "general mismatch fixture must contain PresentComplete");
  const auto validation = preflightFrameTapeIdentity(
      fixture.tape, fixture.blobsValue);
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
  auto prepared = prepareRenderTapeIdentity(
      fixture.tape, fixture.blobsValue);
  const auto result = replayPreparedRenderTapeIdentity(
      device, prepared,
      RenderTapePreparedExecutionOptions{
          .route = RenderTapePreparedRoute::Compatibility,
          .reset = RenderTapePreparedReset::FreshDevice,
          .auditFreshSeedDrains = true});
  dxmt9c_device_release(device);
  dxmt9c_factory_release(factory);
  check(result.status == FrameTapeReplayStatus::OutputMismatch,
        "wrong expected digest must report output mismatch");
  check(result.validity.outputReadback && result.validity.expectedDigestCaptured &&
            !result.validity.expectedDigestMatched,
        "output mismatch must retain readback and digest comparison evidence");
  check(result.conservation.objectsCreated == 5u &&
            result.conservation.objectsReleased == 5u,
        "output mismatch must still clean up replay-owned objects");
  check(result.freshSeedDrainAudit.attempted &&
            result.freshSeedDrainAudit.mutationCount == 3u &&
            result.freshSeedDrainAudit.inspectedMutations == 3u &&
            result.freshSeedDrainAudit.complete,
        "fresh-seed audit remains complete when only the final pixel oracle fails");
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
  // X8 seeds use the same canonical opaque alpha as capture/readback.
  for (const auto index : {0u, 7u}) {
    for (std::size_t byte = 3u; byte < seeds[index].size(); byte += 4u) {
      seeds[index][byte] = std::byte{0xff};
    }
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
  auto prepared = prepareRenderTapeIdentity(tape, blobs);
  const auto result = replayPreparedRenderTapeIdentity(
      device, prepared,
      RenderTapePreparedExecutionOptions{
          .route = RenderTapePreparedRoute::Compatibility,
          .reset = RenderTapePreparedReset::FreshDevice,
          .auditInitialSeeds = true});
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
  const auto& audit = result.initialSeedAudit;
  const auto textureSeed = std::find_if(
      audit.entries.begin(), audit.entries.end(), [&](const auto& entry) {
        return entry.identity.kind == kSnapshotTexture2D.kind &&
            entry.identity.generation == kSnapshotTexture2D.generation &&
            entry.identity.objectId == kSnapshotTexture2D.objectId;
      });
  check(audit.attempted && audit.complete && audit.captureFailedCount == 0u &&
            textureSeed != audit.entries.end() && textureSeed->supported &&
            textureSeed->match,
        "private RT initial seed audit waits for deferred upload before readback");
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

void writeProjectionFixture(const std::filesystem::path& directory) {
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  check(!error, "projection provider fixture directory must be created");
  const TexturedFixture fixture(true, false, false, false, 0u, 21u, false,
                                false, true);
  const auto blob = fixture.blob();
  const RenderTapeBlobCatalogue catalogue{.blobs = {{
      .digest = blob.digest,
      .size = blob.bytes.size(),
      .verified = 1u,
  }}};
  const std::array sources{
      RenderTapeIdentitySource{.eventOrdinal = 5u,
                               .sourceOrdinal = 101u,
                               .seqId = 501u,
                               .captureToken = 77u,
                               .recordCount = 1u,
                               .firstRange = 0u,
                               .rangeCount = 1u},
      RenderTapeIdentitySource{.eventOrdinal = 6u,
                               .sourceOrdinal = 102u,
                               .seqId = 502u,
                               .captureToken = 77u,
                               .recordCount = 1u,
                               .firstRange = 1u,
                               .rangeCount = 1u},
      RenderTapeIdentitySource{.eventOrdinal = 7u,
                               .sourceOrdinal = 103u,
                               .seqId = 503u,
                               .captureToken = 77u,
                               .recordCount = 1u,
                               .firstRange = 2u,
                               .rangeCount = 1u},
  };
  const std::array ranges{
      RenderTapeIdentityRange{.eventOrdinal = 5u,
                              .sourceOrdinal = 101u,
                              .seqId = 501u,
                              .logicalPassId = 9000u,
                              .firstRecord = 0u,
                              .recordCount = 1u,
                              .dagPassIndex = 0u,
                              .passKind = 1u},
      RenderTapeIdentityRange{.eventOrdinal = 6u,
                              .sourceOrdinal = 102u,
                              .seqId = 502u,
                              .logicalPassId = 9001u,
                              .firstRecord = 0u,
                              .recordCount = 1u,
                              .dagPassIndex = 1u,
                              .passKind = 1u},
      RenderTapeIdentityRange{.eventOrdinal = 7u,
                              .sourceOrdinal = 103u,
                              .seqId = 503u,
                              .logicalPassId = 9002u,
                              .firstRecord = 0u,
                              .recordCount = 1u,
                              .dagPassIndex = 2u,
                              .passKind = 1u},
  };
  const std::array settlements{
      RenderTapeIdentitySettlement{
          .eventOrdinal = 5u,
          .rawOrdinal = 5u,
          .buildGeneration = 1u,
          .firstSourceOrdinal = 101u,
          .tailSeqId = 501u,
          .sourceCount = 1u,
      },
      RenderTapeIdentitySettlement{
          .eventOrdinal = 6u,
          .rawOrdinal = 6u,
          .buildGeneration = 1u,
          .firstSourceOrdinal = 102u,
          .tailSeqId = 502u,
          .sourceCount = 1u,
      },
      RenderTapeIdentitySettlement{
          .eventOrdinal = 7u,
          .rawOrdinal = 7u,
          .buildGeneration = 1u,
          .firstSourceOrdinal = 103u,
          .tailSeqId = 503u,
          .sourceCount = 1u,
      },
  };
  const auto identity = buildRenderTapeIdentity(
      fixture.tape, catalogue, 42u, 7u, 77u,
      RenderTapeIdentityAuthority::Capture, sources, ranges, nullptr,
      settlements);
  check(!identity.empty(), "projection provider identity must build");

  std::ofstream events(directory / "events.bin", std::ios::binary);
  std::ofstream sidecar(directory / "identity.bin", std::ios::binary);
  std::ofstream seed(directory / "seed.bin", std::ios::binary);
  check(events.good() && sidecar.good() && seed.good(),
        "projection provider fixture files must open");
  events.write(reinterpret_cast<const char*>(fixture.tape.data()),
               static_cast<std::streamsize>(fixture.tape.size()));
  sidecar.write(reinterpret_cast<const char*>(identity.data()),
                static_cast<std::streamsize>(identity.size()));
  seed.write(reinterpret_cast<const char*>(fixture.seed.data()),
             static_cast<std::streamsize>(fixture.seed.size()));
  check(events.good() && sidecar.good() && seed.good(),
        "projection provider fixture files must be written");
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

std::vector<std::byte> gpuPolicyExpectedPixels() {
  constexpr std::array basePixel{
      std::byte{0x30}, std::byte{0x20}, std::byte{0x10}, std::byte{0xff}};
  constexpr std::array insetPixel{
      std::byte{0x70}, std::byte{0x60}, std::byte{0x50}, std::byte{0xff}};
  std::vector<std::byte> pixels;
  pixels.reserve(16u * 16u * basePixel.size());
  for (std::size_t row = 0u; row < 16u; ++row) {
    for (std::size_t column = 0u; column < 16u; ++column) {
      const bool inset = row >= 4u && row < 12u && column >= 4u && column < 12u;
      const auto& pixel = inset ? insetPixel : basePixel;
      pixels.insert(pixels.end(), pixel.begin(), pixel.end());
    }
  }
  return pixels;
}

std::vector<std::byte> gpuVisibleUniformExpectedPixels(std::uint32_t backgroundARGB) {
  // The independent oracle models the three transformed viewport strips:
  // A (left, four pixels), B (middle, four pixels after its 0.5x VS scale),
  // and A (right, four pixels). The remaining columns retain the clear.
  const std::array background{
      std::byte(backgroundARGB & 0xffu), std::byte((backgroundARGB >> 8u) & 0xffu),
      std::byte((backgroundARGB >> 16u) & 0xffu), std::byte{0xff}};
  constexpr std::array colorA{
      std::byte{0x40}, std::byte{0x40}, std::byte{0xe0}, std::byte{0xff}};
  constexpr std::array colorB{
      std::byte{0x80}, std::byte{0xe0}, std::byte{0x40}, std::byte{0xff}};
  std::vector<std::byte> pixels;
  pixels.reserve(16u * 16u * background.size());
  for (std::size_t row = 0u; row < 16u; ++row) {
    for (std::size_t column = 0u; column < 16u; ++column) {
      const auto& pixel = column < 4u || column >= 12u
          ? colorA
          : column >= 6u && column < 10u ? colorB : background;
      pixels.insert(pixels.end(), pixel.begin(), pixel.end());
    }
  }
  return pixels;
}

void writeGpuPolicyFixture(const std::filesystem::path& directory,
                           bool textureInputAudit = false) {
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  check(!error, "GPU policy fixture directory must be created");
  const GeneralIndexedFixture fixture(32u, true, true, false, false, false,
      textureInputAudit ? render_tape_d3d_format::R32F : 0u);
  const auto preflight = preflightRenderTapeIdentity(fixture.tape,
                                                      fixture.blobsValue);
  check(preflight.complete() &&
            preflight.coverage.drawIndexedPrimitiveRecords == 32u,
        "GPU policy fixture must be a bounded 32-draw indexed tape");
  const auto oracle = gpuPolicyExpectedPixels();
  check(oracle.size() == 16u * 16u * 4u,
        "GPU policy fixture oracle dimensions are pinned");
  const auto expectedDigest = RenderTapeCaptureSession::sha256(oracle);
  check(expectedDigest != RenderTapeDigest{},
        "GPU policy fixture oracle digest must be authenticated");
  std::ofstream events(directory / "events.bin", std::ios::binary);
  check(events.good(), "GPU policy fixture must open events.bin");
  events.write(reinterpret_cast<const char*>(fixture.tape.data()),
               static_cast<std::streamsize>(fixture.tape.size()));
  check(events.good(), "GPU policy fixture must write events.bin");
  std::vector<std::string_view> names{
      std::string_view{"vertex.bin"}, std::string_view{"index.bin"},
      std::string_view{"shader.bin"}, std::string_view{"declaration.bin"},
      std::string_view{"output.bin"},
  };
  if (textureInputAudit) names.push_back("audit-texture.bin");
  check(fixture.blobsValue.size() == names.size(),
        "GPU policy fixture blob count is pinned");
  for (std::size_t index = 0u; index < names.size(); ++index) {
    std::ofstream blob(directory / std::string(names[index]), std::ios::binary);
    check(blob.good(), "GPU policy fixture must open a blob file");
    blob.write(reinterpret_cast<const char*>(fixture.blobsValue[index].bytes.data()),
               static_cast<std::streamsize>(fixture.blobsValue[index].bytes.size()));
    check(blob.good(), "GPU policy fixture must write every blob file");
  }
  for (const auto name : {std::string_view{"output.rgba"},
                          std::string_view{"source.rgba"}}) {
    std::ofstream rgba(directory / std::string(name), std::ios::binary);
    check(rgba.good(), "GPU policy fixture must open an RGBA oracle");
    rgba.write(reinterpret_cast<const char*>(oracle.data()),
               static_cast<std::streamsize>(oracle.size()));
    check(rgba.good(), "GPU policy fixture must write an RGBA oracle");
  }
  if (textureInputAudit) {
    // Authenticate the exact emitted tape, including its capture timestamp.
    const auto sidecar = sourceBoundaryFixtureIdentity(fixture, true);
    std::ofstream identity(directory / "identity.bin", std::ios::binary);
    identity.write(reinterpret_cast<const char*>(sidecar.data()), sidecar.size());
    check(identity.good(), "texture intervention fixture writes identity");
    const std::array<float, 4u> input{1.0f, 0.75f, 0.5f, 0.25f};
    std::ofstream data(directory / "intervention.r32f", std::ios::binary);
    data.write(reinterpret_cast<const char*>(input.data()), sizeof(input));
    check(data.good(), "texture intervention fixture writes owned input");
  }
}

void writeGpuVisibleUniformFixture(const std::filesystem::path& directory,
                                   bool alteredMiddleUniform = false,
                                   bool alteredMiddleTransform = false,
                                   bool extraStream = false,
                                   bool alteredExtraStream = false,
                                   bool queryAware = false) {
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  check(!error, "visible-uniform GPU fixture directory must be created");
  const GeneralIndexedFixture fixture(3u, true, false, true,
                                      alteredMiddleUniform,
                                      alteredMiddleTransform, 0u,
                                      extraStream, alteredExtraStream, queryAware);
  const auto preflight = preflightRenderTapeIdentity(fixture.tape,
                                                      fixture.blobsValue);
  if (!preflight.complete() ||
      preflight.coverage.drawIndexedPrimitiveRecords != 3u) {
    std::string diagnostic =
        "visible-uniform fixture preflight status=" +
        std::string(frameTapeReplayStatusName(preflight.status)) +
        " failed_event=" + std::to_string(preflight.failedEventIndex) +
        " events=" + std::to_string(preflight.coverage.eventCount) +
        " chunks=" + std::to_string(preflight.coverage.commandChunks) +
        " records=" + std::to_string(preflight.coverage.commandRecords) +
        " indexed=" +
        std::to_string(preflight.coverage.drawIndexedPrimitiveRecords) +
        " primitive=" +
        std::to_string(preflight.coverage.drawPrimitiveRecords) +
        " constants=" +
        std::to_string(preflight.coverage.stateConstantRecords) +
        " apply_state=" +
        std::to_string(preflight.coverage.applyStateRecords) +
        " clear=" + std::to_string(preflight.coverage.clearRecords) +
        " present=" + std::to_string(preflight.coverage.presentRecords) +
        " blobs=" + std::to_string(preflight.conservation.inputBlobs) +
        " referenced_blobs=" +
        std::to_string(preflight.conservation.referencedBlobs);
    RenderTapeHeader tapeHeader{};
    if (fixture.tape.size() >= sizeof(tapeHeader)) {
      std::memcpy(&tapeHeader, fixture.tape.data(), sizeof(tapeHeader));
      const auto eventBytes = static_cast<std::uint64_t>(
          tapeHeader.eventHeaderSize);
      const auto eventOffset = static_cast<std::uint64_t>(
          tapeHeader.eventTableOffset) +
          static_cast<std::uint64_t>(preflight.failedEventIndex) * eventBytes;
      if (tapeHeader.eventHeaderSize == sizeof(RenderTapeEventHeader) &&
          preflight.failedEventIndex < tapeHeader.eventCount &&
          eventOffset <= fixture.tape.size() &&
          sizeof(RenderTapeEventHeader) <= fixture.tape.size() - eventOffset) {
        RenderTapeEventHeader failedHeader{};
        std::memcpy(&failedHeader, fixture.tape.data() + eventOffset,
                    sizeof(failedHeader));
        diagnostic += " failed_type=" + std::to_string(failedHeader.type) +
            " failed_ordinal=" + std::to_string(failedHeader.ordinal) +
            " failed_payload_bytes=" +
            std::to_string(failedHeader.payloadSize);
        if (failedHeader.type ==
                static_cast<std::uint32_t>(RenderTapeEventType::PresentComplete) &&
            failedHeader.payloadOffset <= tapeHeader.payloadArenaSize &&
            sizeof(RenderTapePresentCompleteHeader) <=
                tapeHeader.payloadArenaSize - failedHeader.payloadOffset &&
            failedHeader.payloadSize >= sizeof(RenderTapePresentCompleteHeader)) {
          RenderTapePresentCompleteHeader complete{};
          const auto payloadOffset = static_cast<std::uint64_t>(
              tapeHeader.payloadArenaOffset) + failedHeader.payloadOffset;
          if (payloadOffset <= fixture.tape.size() &&
              sizeof(complete) <= fixture.tape.size() - payloadOffset) {
            std::memcpy(&complete, fixture.tape.data() + payloadOffset,
                        sizeof(complete));
            diagnostic += " present_ordinal=" +
                std::to_string(complete.presentOrdinal) +
                " completion_ordinal=" +
                std::to_string(complete.completionOrdinal) +
                " oracle_count=" + std::to_string(complete.oracleCount);
          }
        }
      }
    }
    check(false, diagnostic);
  }
  const auto oracle = gpuVisibleUniformExpectedPixels();
  check(oracle.size() == 16u * 16u * 4u,
        "visible-uniform fixture oracle dimensions are pinned");
  std::ofstream events(directory / "events.bin", std::ios::binary);
  check(events.good(), "visible-uniform fixture must open events.bin");
  events.write(reinterpret_cast<const char*>(fixture.tape.data()),
               static_cast<std::streamsize>(fixture.tape.size()));
  check(events.good(), "visible-uniform fixture must write events.bin");
  constexpr std::array names{
      std::string_view{"vertex.bin"}, std::string_view{"index.bin"},
      std::string_view{"shader.bin"}, std::string_view{"pixel.bin"},
      std::string_view{"declaration.bin"}, std::string_view{"output.bin"},
  };
  check(fixture.blobsValue.size() == names.size(),
        "visible-uniform fixture blob count is pinned");
  for (std::size_t index = 0u; index < names.size(); ++index) {
    std::ofstream blob(directory / std::string(names[index]), std::ios::binary);
    check(blob.good(), "visible-uniform fixture must open a blob file");
    blob.write(reinterpret_cast<const char*>(fixture.blobsValue[index].bytes.data()),
               static_cast<std::streamsize>(fixture.blobsValue[index].bytes.size()));
    check(blob.good(), "visible-uniform fixture must write every blob file");
  }
  for (const auto name : {std::string_view{"output.rgba"},
                          std::string_view{"source.rgba"}}) {
    std::ofstream rgba(directory / std::string(name), std::ios::binary);
    check(rgba.good(), "visible-uniform fixture must open an RGBA oracle");
    rgba.write(reinterpret_cast<const char*>(oracle.data()),
               static_cast<std::streamsize>(oracle.size()));
    check(rgba.good(), "visible-uniform fixture must write an RGBA oracle");
  }
  if (queryAware) {
    const auto sidecar = sourceBoundaryFixtureIdentity(fixture, true);
    std::ofstream identity(directory / "identity.bin", std::ios::binary);
    identity.write(reinterpret_cast<const char*>(sidecar.data()),
                   static_cast<std::streamsize>(sidecar.size()));
    check(identity.good(), "query fixture writes its exact capture-source sidecar");
  }
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

void completedBackbufferSeedAdmissionTruthTable() {
  using dxmt9::RenderTapeBackbufferSeedCompletionState;
  using dxmt9::renderTapeBackbufferSeedCompletionAllowed;
  for (unsigned builds = 0u; builds != 32u; ++builds) {
    // This same staged guard precedes every payload/encoder read in the cold
    // queue API. Model a borrowed append as an unreadable live payload.
    unsigned payloadReads = 0u;
    if (dxmt9::renderTapeBackbufferSeedBuildQuiescent(
            builds & 1u, builds & 2u, builds & 4u, builds & 8u, builds & 16u)) {
      ++payloadReads;
    }
    check(payloadReads == (builds == 0u ? 1u : 0u),
          "active Direct/Arena builder is rejected before any payload read");
  }
  // Distinct handle values include a same-slot/different-generation owner.
  // Their encoding is irrelevant to this pure identity/completion model;
  // validSeedSurface independently models the pool's generation lookup.
  constexpr std::array<dxmt9::core::Handle, 4u> owners{{
      {0u}, {1u}, {2u}, {0x100000001u},
  }};
  constexpr std::array<std::array<std::uint64_t, 2u>, 4u> initializers{{
      {0u, 0u}, {1u, 0u}, {1u, 1u}, {1u, 2u},
  }};
  std::size_t checked = 0u;
  for (const auto target : owners) {
    for (const auto current : owners) {
      for (std::uint64_t published = 0u; published != 3u; ++published) {
        for (std::uint64_t completed = 0u; completed != 4u; ++completed) {
          for (unsigned flags = 0u; flags != 1024u; ++flags) {
            const auto& initializer = initializers[flags >> 8u];
            const RenderTapeBackbufferSeedCompletionState state{
                .seedTarget = target,
                .currentBackbuffer = current,
                .publishedSequence = published,
                .completedSequence = completed,
                .initializerPublished = initializer[0],
                .initializerCompleted = initializer[1],
                .inflightCount = (flags & 32u) ? 1u : 0u,
                .exactAttachmentPreservation = bool(flags & 1u),
                .validSeedSurface = bool(flags & 2u),
                .producerQuiescent = bool(flags & 4u),
                .initializerQuiescent = bool(flags & 8u),
                .initializerTrackingAvailable = bool(flags & 128u),
                .initializerFailed = bool(flags & 64u),
                .terminal = bool(flags & 16u),
            };
            // Serial semantics: a full live-target seed may replace discard
            // permission only after all its real local work has completed,
            // with no concurrent writer, initializer or terminal failure.
            const bool sameLiveOwner = target.value != 0u &&
                target.value == current.value && (flags & 2u);
            const bool settled = published != 0u && completed >= published &&
                !(flags & 32u) && (flags & 4u) && (flags & 8u) &&
                (flags & 128u) && !(flags & 64u) && initializer[1] >= initializer[0];
            const bool expected = sameLiveOwner && settled &&
                (flags & 1u) && !(flags & 16u);
            const bool admitted = renderTapeBackbufferSeedCompletionAllowed(state);
            check(admitted == expected,
                  "full-seed completion binds exact owner, waterline and terminal truth table");
            for (const bool discard : {false, true}) {
              bool nextDiscard = discard;
              if (admitted) nextDiscard = false;
              check(nextDiscard == (expected ? false : discard),
                    "failed seed admission preserves discard; fresh accepted seed is a no-op");
              ++checked;
            }
          }
        }
      }
    }
  }
  check(checked == 393216u,
        "bounded full-seed checker includes submitted initializer error/pending callbacks");
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

void freshSeedDrainPredicateTruthTable() {
  constexpr D9CWireObjectIdentity textureA{
      .kind = D9C_CHUNK_HANDLE_KIND_TEXTURE, .generation = 4u,
      .objectId = 0x7200u};
  constexpr D9CWireObjectIdentity textureB{
      .kind = D9C_CHUNK_HANDLE_KIND_TEXTURE, .generation = 9u,
      .objectId = 0x7201u};
  constexpr D9CWireObjectIdentity surface{
      .kind = D9C_CHUNK_HANDLE_KIND_SURFACE, .generation = 4u,
      .objectId = 0x7202u};
  const D9CSurfaceDesc level0{
      .format = 22u, .resourceType = 3u, .usage = 0u, .pool = 0u,
      .width = 4u, .height = 4u, .depth = 1u};
  const auto textureDescriptor = texture2DDescriptor(level0);
  const auto expected = renderTapeDeriveExpectedSurfaceContent(level0);
  check(expected.status == RenderTapeExpectedContentStatus::Accepted,
        "fresh-seed truth table uses an accepted texture surface");
  std::vector<std::byte> seed(expected.bytes, std::byte{0x31});
  const RenderTapeObjectDefineHeader definition{
      .identity = textureA,
      .descriptorKind = static_cast<std::uint32_t>(
          RenderTapeDescriptorKind::Texture),
      .descriptorBytes = static_cast<std::uint32_t>(textureDescriptor.size()),
      .expectedContentBytes = expected.bytes,
      .expectedContentCount = 1u};
  RenderTapeResourceMutationHeader mutation{
      .identity = textureA,
      .kind = static_cast<std::uint32_t>(RenderTapeMutationKind::Upload),
      .subresource = 0u,
      .byteSize = expected.bytes};
  const auto admits = [&] {
    return renderTapeFreshSeedDrainBypassAllowed(
        definition, textureDescriptor, mutation, seed, true, false, false);
  };
  check(admits(), "fresh full first texture seed is bypass-eligible");

  mutation.identity.generation++;
  check(!admits(), "wrong generation is not fresh-owner eligible");
  mutation.identity = textureA;
  check(!renderTapeFreshSeedDrainBypassAllowed(
            definition, textureDescriptor, mutation, seed, false, false, false),
        "an intervening command epoch requires a drain");
  check(!renderTapeFreshSeedDrainBypassAllowed(
            definition, textureDescriptor, mutation, seed, true, true, false),
        "texture aliases conservatively require a drain");
  check(!renderTapeFreshSeedDrainBypassAllowed(
            definition, textureDescriptor, mutation,
            std::span<const std::byte>(seed.data(), seed.size() - 1u), true,
            false, false),
        "partial seed bytes require a drain");
  check(!renderTapeFreshSeedDrainBypassAllowed(
            definition, textureDescriptor, mutation, seed, true, false, true),
        "repeated subresource seed requires a drain");

  const RenderTapeObjectDefineHeader secondOwner{
      .identity = textureB,
      .descriptorKind = definition.descriptorKind,
      .descriptorBytes = definition.descriptorBytes,
      .expectedContentBytes = expected.bytes,
      .expectedContentCount = 1u};
  mutation.identity = textureB;
  check(renderTapeFreshSeedDrainBypassAllowed(
            secondOwner, textureDescriptor, mutation, seed, true, false, false),
        "an independently-owned second texture has its own fresh epoch");

  const D9CSurfaceDesc level1{level0.format, level0.resourceType, level0.usage,
                              level0.pool, level0.multiSampleType,
                              level0.multiSampleQuality, 2u, 2u, 1u};
  const RenderTapeTextureDescriptorV2 multiHeader{
      .schemaVersion = kRenderTapeTextureDescriptorVersion2,
      .dimension = static_cast<std::uint32_t>(RenderTapeTextureDimension::Texture2D),
      .mipLevelCount = 2u,
      .subresourceCount = 2u,
      .initialContentDisposition = static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::CompleteSeed)};
  std::vector<std::byte> multiDescriptor(sizeof(multiHeader) +
                                          2u * sizeof(D9CSurfaceDesc));
  std::memcpy(multiDescriptor.data(), &multiHeader, sizeof(multiHeader));
  std::memcpy(multiDescriptor.data() + sizeof(multiHeader), &level0,
              sizeof(level0));
  std::memcpy(multiDescriptor.data() + sizeof(multiHeader) + sizeof(level0),
              &level1, sizeof(level1));
  const auto expectedLevel1 = renderTapeDeriveExpectedSurfaceContent(level1);
  check(expectedLevel1.status == RenderTapeExpectedContentStatus::Accepted,
        "multi-mip truth table uses an accepted second surface");
  std::vector<std::byte> seedLevel1(expectedLevel1.bytes, std::byte{0x51});
  const RenderTapeObjectDefineHeader multiDefinition{
      .identity = textureA,
      .descriptorKind = definition.descriptorKind,
      .descriptorBytes = static_cast<std::uint32_t>(multiDescriptor.size()),
      .expectedContentBytes = expected.bytes + expectedLevel1.bytes,
      .expectedContentCount = 2u};
  auto multiMutation = mutation;
  multiMutation.identity = textureA;
  multiMutation.byteSize = expected.bytes;
  check(renderTapeFreshSeedDrainBypassAllowed(
            multiDefinition, multiDescriptor, multiMutation, seed, true, false,
            false),
        "first full seed of a multi-mip texture is eligible");
  multiMutation.subresource = 1u;
  multiMutation.byteSize = expectedLevel1.bytes;
  check(renderTapeFreshSeedDrainBypassAllowed(
            multiDefinition, multiDescriptor, multiMutation, seedLevel1, true,
            false, false),
        "each independently seeded texture subresource is checked");
  auto prestageMutation = mutation;
  prestageMutation.identity = textureA;
  prestageMutation.byteSize = expected.bytes;
  const std::array prestageMutations{prestageMutation};
  check(renderTapeTexturePrestageCandidate(
            definition, textureDescriptor, prestageMutations, false, false,
            false),
        "complete texture seed is a cold-prestage candidate");
  check(!renderTapeTexturePrestageCandidate(
            definition, textureDescriptor, prestageMutations, true, false,
            false),
        "texture alias excludes cold prestaging");
  check(!renderTapeTexturePrestageCandidate(
            definition, textureDescriptor, prestageMutations, false, true,
            false),
        "same wire slot with another generation excludes cold prestaging");
  check(!renderTapeTexturePrestageCandidate(
            definition, textureDescriptor, prestageMutations, false, false,
            true),
        "command between definition and final seed excludes cold prestaging");
  auto duplicateMutation = prestageMutation;
  const std::array duplicateMutations{prestageMutation, duplicateMutation};
  check(!renderTapeTexturePrestageCandidate(
            definition, textureDescriptor, duplicateMutations, false, false,
            false),
        "repeated texture subresource excludes cold prestaging");
  const std::array multiMutations{
      RenderTapeResourceMutationHeader{.identity = textureA,
                                       .kind = static_cast<std::uint32_t>(
                                           RenderTapeMutationKind::Upload),
                                       .subresource = 0u,
                                       .byteSize = expected.bytes},
      RenderTapeResourceMutationHeader{.identity = textureA,
                                       .kind = static_cast<std::uint32_t>(
                                           RenderTapeMutationKind::Upload),
                                       .subresource = 1u,
                                       .byteSize = expectedLevel1.bytes}};
  check(renderTapeTexturePrestageCandidate(
            multiDefinition, multiDescriptor, multiMutations, false, false,
            false),
        "all full texture subresources are required for cold prestaging");

  // Bind the predicate to a small chronological event model rather than only
  // testing independent flags: two owners are declared in both orders, a
  // command may occur before/between/after declarations or between mutations,
  // and mutation order is also permuted.  The expected admission is exactly
  // the reducer state (same epoch and not previously seeded).
  const std::array<D9CWireObjectIdentity, 2> owners{textureA, textureB};
  const std::array<RenderTapeObjectDefineHeader, 2> ownerDefinitions{
      definition, secondOwner};
  for (unsigned defineOrder = 0u; defineOrder < 2u; ++defineOrder) {
    for (unsigned mutationOrder = 0u; mutationOrder < 2u; ++mutationOrder) {
      for (unsigned commandPosition = 0u; commandPosition < 4u;
           ++commandPosition) {
        for (unsigned stale = 0u; stale < 2u; ++stale) {
          std::array<std::uint64_t, 2> definitionEpoch{};
          std::array<bool, 2> seeded{};
          std::array<bool, 2> declared{};
          std::array<bool, 2> possiblyUsed{};
          std::uint64_t commandEpoch = 0u;
          const auto command = [&] {
            ++commandEpoch;
            // Independent history oracle: any live owner may be bound by a
            // sparse command; an as-yet-undefined owner cannot be referenced.
            for (std::size_t owner = 0u; owner < declared.size(); ++owner) {
              possiblyUsed[owner] = possiblyUsed[owner] || declared[owner];
            }
          };
          const unsigned firstDefinition = defineOrder == 0u ? 0u : 1u;
          const unsigned secondDefinition = 1u - firstDefinition;
          const std::array<unsigned, 2> declarationOrder{
              firstDefinition, secondDefinition};
          for (unsigned event = 0u; event < declarationOrder.size(); ++event) {
            if (commandPosition == 0u && event == 0u) command();
            definitionEpoch[declarationOrder[event]] = commandEpoch;
            declared[declarationOrder[event]] = true;
            if (commandPosition == 1u && event == 0u) command();
          }
          if (commandPosition == 2u) command();
          const unsigned firstMutation = mutationOrder == 0u ? 0u : 1u;
          const unsigned secondMutation = 1u - firstMutation;
          const std::array<unsigned, 2> replayMutationOrder{
              firstMutation, secondMutation};
          for (unsigned event = 0u; event < replayMutationOrder.size();
               ++event) {
            if (commandPosition == 3u && event == 1u) command();
            const unsigned owner = replayMutationOrder[event];
            auto ownerMutation = mutation;
            ownerMutation.identity = owners[owner];
            ownerMutation.byteSize = expected.bytes;
            if (stale) ++ownerMutation.identity.generation;
            const bool admitted = renderTapeFreshSeedDrainBypassAllowed(
                ownerDefinitions[owner], textureDescriptor, ownerMutation,
                seed, definitionEpoch[owner] == commandEpoch, false,
                seeded[owner]);
            const bool expectedAdmission = !stale &&
                !possiblyUsed[owner] && !seeded[owner];
            check(admitted == expectedAdmission,
                  "chronological two-owner fresh-seed model agrees with predicate");
            seeded[owner] = true;
          }
        }
      }
    }
  }

  const D9CBufferDesc bufferDesc{.size = 32u, .pool = 0u};
  const auto bufferDescriptor = bytesOf(bufferDesc);
  const std::vector<std::byte> bufferSeed(bufferDesc.size, std::byte{0x42});
  const D9CWireObjectIdentity buffer{
      .kind = D9C_CHUNK_HANDLE_KIND_BUFFER, .generation = 3u,
      .objectId = 0x7203u};
  const RenderTapeObjectDefineHeader bufferDefinition{
      .identity = buffer,
      .descriptorKind = static_cast<std::uint32_t>(
          RenderTapeDescriptorKind::Buffer),
      .descriptorBytes = sizeof(bufferDesc),
      .expectedContentBytes = bufferDesc.size,
      .expectedContentCount = 1u};
  const RenderTapeResourceMutationHeader bufferMutation{
      .identity = buffer,
      .kind = static_cast<std::uint32_t>(RenderTapeMutationKind::CpuUnlock),
      .byteSize = bufferDesc.size};
  check(renderTapeFreshSeedDrainBypassAllowed(
            bufferDefinition, bufferDescriptor, bufferMutation, bufferSeed,
            true, false, false),
        "a full first buffer seed is bypass-eligible");
  auto nonzeroBufferSubresource = bufferMutation;
  nonzeroBufferSubresource.subresource = 1u;
  check(!renderTapeFreshSeedDrainBypassAllowed(
            bufferDefinition, bufferDescriptor, nonzeroBufferSubresource,
            bufferSeed, true, false, false),
        "buffer subresources outside zero are rejected");

  const auto output = outputDescriptor(level0);
  const RenderTapeObjectDefineHeader surfaceDefinition{
      .identity = surface,
      .descriptorKind = static_cast<std::uint32_t>(
          RenderTapeDescriptorKind::Surface),
      .descriptorBytes = sizeof(output),
      .expectedContentBytes = expected.bytes,
      .expectedContentCount = 1u};
  const RenderTapeResourceMutationHeader surfaceMutation{
      .identity = surface,
      .kind = static_cast<std::uint32_t>(RenderTapeMutationKind::Upload),
      .byteSize = expected.bytes};
  check(!renderTapeFreshSeedDrainBypassAllowed(
            surfaceDefinition, std::as_bytes(std::span(&output, 1u)),
            surfaceMutation, seed, true, false, false),
        "surface owners are outside the fresh-seed proof");
  check(!renderTapeFreshSeedDrainBypassAllowed(
            definition, {}, mutation, seed, true, false, false),
        "unknown descriptors are rejected");
}

void texturePrestageAdmissionExhaustiveEventOrder() {
  constexpr D9CWireObjectIdentity texture{
      .kind = D9C_CHUNK_HANDLE_KIND_TEXTURE, .generation = 11u,
      .objectId = 0x7300u};
  constexpr D9CWireObjectIdentity otherTexture{
      .kind = D9C_CHUNK_HANDLE_KIND_TEXTURE, .generation = 11u,
      .objectId = 0x7301u};
  const D9CSurfaceDesc level{
      .format = 21u, .resourceType = 3u, .pool = 0u,
      .width = 2u, .height = 2u, .depth = 1u};
  const auto descriptor = texture2DDescriptor(level);
  const auto expected = renderTapeDeriveExpectedSurfaceContent(level);
  check(expected.status == RenderTapeExpectedContentStatus::Accepted,
        "prestage exhaustive checker has an accepted 2D seed");
  const RenderTapeObjectDefineHeader definition{
      .identity = texture,
      .descriptorKind = static_cast<std::uint32_t>(
          RenderTapeDescriptorKind::Texture),
      .descriptorBytes = static_cast<std::uint32_t>(descriptor.size()),
      .expectedContentBytes = expected.bytes,
      .expectedContentCount = 1u,
  };
  const RenderTapeResourceMutationHeader mutation{
      .identity = texture,
      .kind = static_cast<std::uint32_t>(RenderTapeMutationKind::Upload),
      .subresource = 0u,
      .byteSize = expected.bytes,
  };
  const auto modelOrderAllowed = [](std::uint32_t definitionPosition,
                                    std::span<const std::uint32_t> seeds,
                                    std::span<const std::uint32_t> commands) {
    if (seeds.empty()) return false;
    // Execute an independent event state machine, rather than reproducing
    // the production min/max interval calculation.
    std::vector<std::pair<std::uint32_t, unsigned>> events{{definitionPosition, 0u}};
    for (const auto position : seeds) events.emplace_back(position, 1u);
    for (const auto position : commands) events.emplace_back(position, 2u);
    std::sort(events.begin(), events.end());
    bool defined = false;
    std::size_t remainingSeeds = seeds.size();
    for (std::size_t i = 0u; i < events.size(); ++i) {
      if (i && events[i - 1u].first == events[i].first) return false;
      switch (events[i].second) {
      case 0u: defined = true; break;
      case 1u:
        if (!defined) return false;
        --remainingSeeds;
        break;
      case 2u:
        if (defined && remainingSeeds != 0u) return false;
        break;
      }
    }
    return defined && remainingSeeds == 0u;
  };
  // Bind the production order reducer to the admission predicate for every
  // define/seed/command permutation, including invalid forward references.
  std::array<std::uint32_t, 3u> order{0u, 1u, 2u};
  std::uint32_t checkedOrders = 0u;
  do {
    std::uint32_t definitionPosition = 0u;
    std::uint32_t seedPosition = 0u;
    std::uint32_t commandPosition = 0u;
    for (std::uint32_t position = 0u; position < order.size(); ++position) {
      if (order[position] == 0u) definitionPosition = position;
      if (order[position] == 1u) seedPosition = position;
      if (order[position] == 2u) commandPosition = position;
    }
    const std::array seedPositions{seedPosition};
    const std::array commandPositions{commandPosition};
    const bool orderAllowed = renderTapeTexturePrestageOrderAllowed(
        definitionPosition, seedPositions, commandPositions);
    check(orderAllowed == modelOrderAllowed(definitionPosition, seedPositions,
                                             commandPositions),
          "production order reducer matches the independent event model");
    const bool admitted = renderTapeTexturePrestageCandidate(
        definition, descriptor, std::span(&mutation, 1u), false, false,
        !orderAllowed);
    check(admitted == orderAllowed,
          "every define/command/seed order agrees with production admission");
    ++checkedOrders;
  } while (std::next_permutation(order.begin(), order.end()));
  check(checkedOrders == 6u,
        "prestage checker exhausts all three-event chronological orders");

  const D9CSurfaceDesc level1{
      .format = 21u, .resourceType = 3u, .pool = 0u,
      .width = 1u, .height = 1u, .depth = 1u};
  const RenderTapeTextureDescriptorV2 multiHeader{
      .schemaVersion = kRenderTapeTextureDescriptorVersion2,
      .dimension = static_cast<std::uint32_t>(RenderTapeTextureDimension::Texture2D),
      .mipLevelCount = 2u,
      .subresourceCount = 2u,
      .initialContentDisposition = static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::CompleteSeed),
  };
  const auto expected1 = renderTapeDeriveExpectedSurfaceContent(level1);
  check(expected1.status == RenderTapeExpectedContentStatus::Accepted,
        "prestage exhaustive checker has an accepted second mip");
  std::vector<std::byte> multiDescriptor(sizeof(multiHeader) +
                                          2u * sizeof(D9CSurfaceDesc));
  std::memcpy(multiDescriptor.data(), &multiHeader, sizeof(multiHeader));
  std::memcpy(multiDescriptor.data() + sizeof(multiHeader), &level,
              sizeof(level));
  std::memcpy(multiDescriptor.data() + sizeof(multiHeader) + sizeof(level),
              &level1, sizeof(level1));
  const RenderTapeObjectDefineHeader multiDefinition{
      .identity = texture,
      .descriptorKind = definition.descriptorKind,
      .descriptorBytes = static_cast<std::uint32_t>(multiDescriptor.size()),
      .expectedContentBytes = expected.bytes + expected1.bytes,
      .expectedContentCount = 2u,
  };
  const std::array multiMutations{
      mutation,
      RenderTapeResourceMutationHeader{
          .identity = texture,
          .kind = static_cast<std::uint32_t>(RenderTapeMutationKind::Upload),
          .subresource = 1u,
          .byteSize = expected1.bytes,
      },
  };
  std::array<std::uint32_t, 4u> multiOrder{0u, 1u, 2u, 3u};
  checkedOrders = 0u;
  do {
    std::uint32_t definitionPosition = 0u;
    std::array<std::uint32_t, 2u> seedPositions{};
    std::array<std::uint32_t, 1u> commandPositions{};
    for (std::uint32_t position = 0u; position < multiOrder.size();
         ++position) {
      if (multiOrder[position] == 0u) definitionPosition = position;
      if (multiOrder[position] == 1u) seedPositions[0] = position;
      if (multiOrder[position] == 2u) seedPositions[1] = position;
      if (multiOrder[position] == 3u) commandPositions[0] = position;
    }
    const bool orderAllowed = renderTapeTexturePrestageOrderAllowed(
        definitionPosition, seedPositions, commandPositions);
    check(orderAllowed == modelOrderAllowed(definitionPosition, seedPositions,
                                             commandPositions),
          "two-mip production order matches the independent event model");
    const bool admitted = renderTapeTexturePrestageCandidate(
        multiDefinition, multiDescriptor, multiMutations, false, false,
        !orderAllowed);
    check(admitted == orderAllowed,
          "every two-mip define/seed/command order agrees with admission");
    ++checkedOrders;
  } while (std::next_permutation(multiOrder.begin(), multiOrder.end()));
  check(checkedOrders == 24u,
        "prestage checker exhausts all 24 two-mip chronological orders");

  auto duplicate = mutation;
  const std::array duplicateSeeds{mutation, duplicate};
  check(!renderTapeTexturePrestageCandidate(
            definition, descriptor, duplicateSeeds, false, false, false),
        "repeated subresource seed is excluded from prestaging");
  auto outOfRange = mutation;
  outOfRange.subresource = 1u;
  check(!renderTapeTexturePrestageCandidate(
            definition, descriptor, std::span(&outOfRange, 1u), false, false,
            false),
        "out-of-range subresource is excluded from prestaging");
  auto partial = mutation;
  partial.byteSize = expected.bytes - 1u;
  check(!renderTapeTexturePrestageCandidate(
            definition, descriptor, std::span(&partial, 1u), false, false,
            false),
        "partial seed bytes are excluded from prestaging");
  check(!renderTapeTexturePrestageCandidate(
            definition, descriptor, std::span(&mutation, 1u), true, false,
            false),
        "surface alias excludes prestaging");
  check(!renderTapeTexturePrestageCandidate(
            definition, descriptor, std::span(&mutation, 1u), false, true,
            false),
        "same-slot generation conflict excludes prestaging");

  auto wrongOwner = mutation;
  wrongOwner.identity = otherTexture;
  check(!renderTapeTexturePrestageCandidate(
            definition, descriptor, std::span(&wrongOwner, 1u), false, false,
            false),
        "mutation for another owner cannot satisfy prestaging coverage");

  const D9CBufferDesc bufferDesc{
      .size = static_cast<std::uint32_t>(expected.bytes), .pool = 0u};
  const RenderTapeObjectDefineHeader bufferDefinition{
      .identity = D9CWireObjectIdentity{
          .kind = D9C_CHUNK_HANDLE_KIND_BUFFER,
          .generation = texture.generation,
          .objectId = texture.objectId},
      .descriptorKind = static_cast<std::uint32_t>(
          RenderTapeDescriptorKind::Buffer),
      .descriptorBytes = sizeof(bufferDesc),
      .expectedContentBytes = expected.bytes,
      .expectedContentCount = 1u,
  };
  const auto bufferBytes = bytesOf(bufferDesc);
  auto bufferMutation = mutation;
  bufferMutation.identity = bufferDefinition.identity;
  check(!renderTapeTexturePrestageCandidate(
            bufferDefinition, bufferBytes, std::span(&bufferMutation, 1u),
            false, false, false),
        "buffers are excluded from the texture-only prestage lane");
}

void preparedTapeOwnsImmutableInputAndExposesColdEvidence() {
  SequenceFixture fixture;
  auto blobs = fixture.blobs();
  auto prepared = prepareRenderTapeIdentity(fixture.tape, blobs);
  check(prepared.valid(), "prepared sequence tape must complete preflight");
  check(prepared.prepareCount() == 1u && prepared.evidence().prepareCount == 1u &&
            prepared.evidence().immutableInputOwned &&
            prepared.evidence().preflightCompleted &&
            prepared.evidence().tapeBytes == fixture.tape.size() &&
            prepared.evidence().blobCount == blobs.size(),
        "prepared tape must expose one cold preflight and owned-input evidence");

  fixture.tape[0] ^= std::byte{0xff};
  fixture.firstSeed[0] ^= std::byte{0xff};
  check(prepared.preflightResult().complete() &&
            prepared.preflightResult().validity.digestsValid,
        "prepared plan must survive mutation of caller-owned tape and blobs");
  auto moved = std::move(prepared);
  check(moved.valid() && moved.prepareCount() == 1u,
        "prepared tape must remain valid after move");
  const auto unsupported = replayPreparedRenderTapeIdentity(
      nullptr, moved,
      RenderTapePreparedExecutionOptions{
          .route = RenderTapePreparedRoute::Compatibility,
          .reset = RenderTapePreparedReset::WarmDevice});
  check(unsupported.status == FrameTapeReplayStatus::WarmResetUnsupported &&
            unsupported.prepared.planReused &&
            unsupported.prepared.timing.replayWallNanos == 0u,
        "warm-device replay must fail explicitly before device effects");
}

void sharedGeneralPreflightPlanMatchesProviderAndIsTransactional() {
  GeneralIndexedFixture fixture;
  const auto providerResult =
      preflightRenderTapeIdentity(fixture.tape, fixture.blobsValue);
  check(providerResult.complete() &&
            providerResult.profile == kRenderTapeProfileFrame,
        "general fixture must complete the existing provider preflight");

  render_tape_runtime::GeneralPreflightPlan plan;
  const auto sharedResult = render_tape_runtime::buildGeneralReplayPlan(
      fixture.tape, fixture.blobsValue, plan);
  check(sharedResult.complete() &&
            sharedResult.status == providerResult.status &&
            sharedResult.profile == providerResult.profile &&
            sharedResult.intervalCount == providerResult.intervalCount &&
            sharedResult.requirements.outputWidth ==
                providerResult.requirements.outputWidth &&
            sharedResult.requirements.outputHeight ==
                providerResult.requirements.outputHeight &&
            sharedResult.requirements.outputFormat ==
                providerResult.requirements.outputFormat &&
            sharedResult.coverage.eventCount == providerResult.coverage.eventCount &&
            sharedResult.coverage.objectDefinitions ==
                providerResult.coverage.objectDefinitions &&
            sharedResult.coverage.seedMutations ==
                providerResult.coverage.seedMutations &&
            sharedResult.coverage.commandChunks ==
                providerResult.coverage.commandChunks &&
            sharedResult.coverage.commandRecords ==
                providerResult.coverage.commandRecords &&
            sharedResult.coverage.drawIndexedPrimitiveRecords ==
                providerResult.coverage.drawIndexedPrimitiveRecords &&
            sharedResult.coverage.presentRecords ==
                providerResult.coverage.presentRecords &&
            sharedResult.coverage.presentOutputs ==
                providerResult.coverage.presentOutputs &&
            sharedResult.conservation.inputBlobs ==
                providerResult.conservation.inputBlobs &&
            sharedResult.conservation.referencedBlobs ==
                providerResult.conservation.referencedBlobs,
        "shared general preflight must match provider admission evidence");
  check(plan.tape.events.data() ==
            reinterpret_cast<const RenderTapeEventHeader*>(
                fixture.tape.data() + plan.tape.header.eventTableOffset) &&
            plan.tape.payloadArena.data() ==
                fixture.tape.data() + plan.tape.header.payloadArenaOffset,
        "shared plan must borrow the validated tape views exactly");
  check(plan.definitions.size() ==
            static_cast<std::size_t>(providerResult.coverage.objectDefinitions) &&
            plan.commandChunks.size() ==
                static_cast<std::size_t>(providerResult.coverage.commandChunks),
        "shared plan must expose the provider's complete general structure");

  for (const auto& event : plan.events) {
    const auto source = plan.tape.event(event.eventIndex);
    check(!source.payload.empty() ||
              event.type == RenderTapeEventType::PresentComplete,
          "shared plan events must resolve to source tape payloads");
    if (event.type == RenderTapeEventType::ObjectDefine) {
      check(event.index < plan.definitions.size(),
            "shared definition event index must be bounded");
      const auto& definition = plan.definitions[event.index];
      check(definition.descriptor.data() ==
                source.payload.data() + sizeof(definition.fixed) &&
                definition.descriptor.size() + sizeof(definition.fixed) ==
                    source.payload.size(),
            "shared definitions must borrow exact descriptor subspans");
    } else if (event.type == RenderTapeEventType::CommandChunk) {
      RenderTapeCommandChunkHeader fixed{};
      check(source.payload.size() >= sizeof(fixed) &&
                event.index < plan.commandChunks.size(),
            "shared command event index must be bounded");
      std::memcpy(&fixed, source.payload.data(), sizeof(fixed));
      const auto& chunk = plan.commandChunks[event.index];
      check(chunk.bytes.data() ==
                source.payload.data() + sizeof(fixed) &&
                chunk.bytes.size() + sizeof(fixed) == source.payload.size() &&
                chunk.envelope.recordCount == fixed.recordCount &&
                chunk.envelope.handleCount == fixed.handleCount,
            "shared command chunks must borrow exact wire subspans");
    }
  }

  const auto priorTapeEvents = plan.tape.events.data();
  const auto priorDefinitions = plan.definitions.size();
  const auto priorFirstDescriptor = plan.definitions.front().descriptor.data();
  const auto priorEvents = plan.events.size();
  const std::array<std::byte, 1u> malformed{std::byte{0x7f}};
  const auto failed = render_tape_runtime::buildGeneralReplayPlan(
      malformed, fixture.blobsValue, plan);
  check(failed.status == FrameTapeReplayStatus::InvalidTape &&
            plan.tape.events.data() == priorTapeEvents &&
            plan.definitions.size() == priorDefinitions &&
            plan.definitions.front().descriptor.data() == priorFirstDescriptor &&
            plan.events.size() == priorEvents,
        "failed shared preflight must leave the existing plan unchanged");
}

void sharedRuntimeObjectHelpersPinIdentityAndOwnership() {
  using namespace render_tape_runtime;
  const D9CWireObjectIdentity identity{
      .kind = D9C_CHUNK_HANDLE_KIND_TEXTURE, .generation = 19u,
      .objectId = 0x9100u};
  const D9CWireObjectIdentity staleGeneration{
      .kind = identity.kind, .generation = identity.generation + 1u,
      .objectId = identity.objectId};
  const D9CWireObjectIdentity wrongKind{
      .kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
      .generation = identity.generation,
      .objectId = identity.objectId};
  std::array<OwnedObject, 1u> objects{{
      {.identity = identity, .value = reinterpret_cast<void*>(0x1u)}}};
  check(findExactObject(objects, identity) == objects[0].value,
        "runtime lookup must return an exact live identity");
  check(findExactObject(objects, staleGeneration) == nullptr &&
            findExactObject(objects, wrongKind) == nullptr,
        "runtime lookup must reject stale generation and kind mismatches");

  std::array<void*, 1u> resolved{};
  std::size_t resolvedCount = 99u;
  const auto chunkBytes = bootstrapChunk();
  const CommandChunkView chunk{
      .envelope = CommandChunkEnvelope{.version = D9C_COMMAND_CHUNK_VERSION,
                                       .recordCount = 1u,
                                       .handleCount = 1u},
      .bytes = chunkBytes};
  const D9CWireObjectIdentity chunkIdentity{
      .kind = D9C_CHUNK_HANDLE_KIND_SURFACE, .generation = 7u,
      .objectId = 41u};
  std::array<OwnedObject, 1u> chunkObjects{{
      {.identity = chunkIdentity, .value = reinterpret_cast<void*>(0x2u)}}};
  check(resolveCommandHandles(chunk, chunkObjects, resolved, resolvedCount) &&
            resolvedCount == 1u && resolved[0] == chunkObjects[0].value,
        "runtime command resolution must use the exact object table");
  chunkObjects[0].identity.generation++;
  check(!resolveCommandHandles(chunk, chunkObjects, resolved, resolvedCount) &&
            resolvedCount == 0u,
        "runtime command resolution must fail closed on stale generation");
  chunkObjects[0].identity.generation--;

  const std::array twoTargets{
      D9CCommandChunkWireRenderTargetBinding{.slot = 0u,
                                             .valid = 1u,
                                             .handleIndex = 0u},
      D9CCommandChunkWireRenderTargetBinding{.slot = 1u,
                                             .valid = 1u,
                                             .handleIndex = 1u}};
  auto missingSecondBytes = bootstrapChunkWithRenderTargets(twoTargets);
  D9CCommandChunkWireHeader missingSecondHeader{};
  std::memcpy(&missingSecondHeader, missingSecondBytes.data(),
              sizeof(missingSecondHeader));
  const D9CCommandChunkWireHandleEntry missingIdentity{
      .kind = D9C_CHUNK_HANDLE_KIND_SURFACE, .generation = 7u,
      .objectId = 0xdeadbeefu};
  std::memcpy(missingSecondBytes.data() + missingSecondHeader.handleTableOffset +
                  sizeof(D9CCommandChunkWireHandleEntry),
              &missingIdentity, sizeof(missingIdentity));
  const CommandChunkView missingSecondChunk{
      .envelope = CommandChunkEnvelope{.version = D9C_COMMAND_CHUNK_VERSION,
                                       .recordCount = 1u,
                                       .handleCount = 2u},
      .bytes = missingSecondBytes};
  std::array<void*, 2u> missingSecondResolved{
      {reinterpret_cast<void*>(0x31u), reinterpret_cast<void*>(0x32u)}};
  resolvedCount = 55u;
  check(!resolveCommandHandles(missingSecondChunk, chunkObjects,
                               missingSecondResolved, resolvedCount) &&
            resolvedCount == 0u &&
            missingSecondResolved[0] == reinterpret_cast<void*>(0x31u) &&
            missingSecondResolved[1] == reinterpret_cast<void*>(0x32u),
        "runtime resolution must publish no pointers when a later handle is missing");
  std::array<void*, 0u> undersizedResolved{};
  resolvedCount = 55u;
  check(!resolveCommandHandles(chunk, chunkObjects, undersizedResolved,
                               resolvedCount) &&
            resolvedCount == 0u,
        "runtime resolution must reject undersized scratch before publishing");

  const D9CWireObjectIdentity aliasIdentity{
      .kind = D9C_CHUNK_HANDLE_KIND_SURFACE, .generation = 23u,
      .objectId = 0x9101u};
  const RenderTapeSurfaceDescriptorV2 alias{
      .schemaVersion = kRenderTapeSurfaceDescriptorVersion2,
      .storage = static_cast<std::uint32_t>(
          RenderTapeSurfaceStorage::TextureSubresource),
      .subresource = 0u,
      .parentTexture = identity};
  const auto aliasBytes = bytesOf(alias);
  const RenderTapeObjectDefineHeader aliasDefinition{
      .identity = aliasIdentity,
      .descriptorKind = static_cast<std::uint32_t>(
          RenderTapeDescriptorKind::Surface),
      .descriptorBytes = static_cast<std::uint32_t>(aliasBytes.size()),
  };
  check(createRenderTapeObject(nullptr, aliasDefinition, aliasBytes, {}, {}) ==
            nullptr,
        "alias creation must reject a missing parent before any effect");

  const D9CPresentParams params{
      .backBufferWidth = 8u,
      .backBufferHeight = 8u,
      .backBufferFormat = 21u,
      .backBufferCount = 1u,
      .swapEffect = 1u,
      .windowed = 1u,
      .presentationInterval = 0x80000000u,
  };
  auto* factory = dxmt9c_factory_create();
  check(factory != nullptr, "runtime object fixture factory must be available");
  auto* device = dxmt9c_factory_create_device(factory, 0u, &params, 0u,
                                                nullptr);
  check(device != nullptr, "runtime object fixture device must construct");
  const D9CSurfaceDesc level0{.format = 22u,
                              .resourceType = 3u,
                              .usage = 1u,
                              .pool = 0u,
                              .width = 8u,
                              .height = 8u,
                              .depth = 1u};
  const auto descriptor = texture2DDescriptor(level0);
  const RenderTapeObjectDefineHeader definition{
      .identity = identity,
      .descriptorKind = static_cast<std::uint32_t>(
          RenderTapeDescriptorKind::Texture),
      .descriptorBytes = static_cast<std::uint32_t>(descriptor.size()),
  };
  void* value = createRenderTapeObject(device, definition, descriptor, {}, {});
  check(value != nullptr, "runtime object fixture must create one texture");
  OwnedObject owned{.identity = identity, .value = value};
  releaseOwnedObject(owned);
  check(owned.value == nullptr && findExactObject(std::span(&owned, 1u), identity) ==
            nullptr,
        "runtime release must null the owned wrapper and hide it from lookup");
  releaseOwnedObject(owned);
  check(owned.value == nullptr,
        "runtime release must be idempotent after the first release");
  dxmt9c_device_release(device);
  dxmt9c_factory_release(factory);
}

} // namespace

int main(int argc, char** argv) {
  try {
    check(::setenv("DXMT_PERF_COUNTERS", "1", 1) == 0,
          "native route evidence requires counters before first device");
    if (argc == 2 && std::string_view(argv[1]) == "--test-event-ordering") {
      completedBackbufferSeedAdmissionTruthTable();
      eventLevelMutationDrainOrderingIsPinned();
      return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--test-query-oracle") {
      check(::setenv("DXMT9_METAL_OCCLUSION", "1", 1) == 0,
            "query Metal oracle requires actual GPU visibility before first device");
      completedBackbufferSeedAdmissionTruthTable();
      nativeMetalCapturedQueryTapeOracle();
      nativeMetalRetainedQueryResultSurvivesReuseAndRelease();
      return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--test-seed-reference-rejection") {
      check(::setenv("DXMT9_ASYNC_QUERY_FENCES", "0", 1) == 0,
            "reference seed test selects signal-only lane before first device");
      completedBackbufferSeedAdmissionTruthTable();
      synchronousReferenceRejectsSeedBeforeEffects();
      return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--test-volume-oracle") {
      check(::setenv("DXMT9_METAL_OCCLUSION", "1", 1) == 0,
            "volume oracle retains actual GPU query-result checks");
      nativeMetalVolumeMipSliceOracle();
      nativeMetalPagedBootstrapTailControlsSampledPixels();
      return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--test-retained-query-oracle") {
      check(::setenv("DXMT9_METAL_OCCLUSION", "1", 1) == 0,
            "retained query oracle uses actual GPU visibility");
      nativeMetalRetainedQueryResultSurvivesReuseAndRelease();
      return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--test-retained-query-budget") {
      check(::setenv("DXMT9_METAL_OCCLUSION", "1", 1) == 0 &&
                ::setenv("DXMT9_PERF_COMPLETION_SIGNAL_DELAY_MS", "250", 1) == 0,
            "retained limit discriminator uses actual visibility and delayed publication");
      nativeMetalRetainedQueryResultSurvivesReuseAndRelease(true);
      return 0;
    }
    if (argc == 3 && std::string_view(argv[1]) == "--write-production-fixture") {
      writeProductionFixture(argv[2]);
      return 0;
    }
    if (argc == 3 && std::string_view(argv[1]) == "--write-projection-fixture") {
      writeProjectionFixture(argv[2]);
      return 0;
    }
    if (argc == 3 && std::string_view(argv[1]) == "--write-sequence-fixture") {
      writeSequenceFixture(argv[2]);
      return 0;
    }
    if (argc == 3 &&
        std::string_view(argv[1]) == "--write-gpu-policy-fixture") {
      writeGpuPolicyFixture(argv[2]);
      return 0;
    }
    if (argc == 3 && std::string_view(argv[1]) == "--write-texture-input-audit-fixture") {
      writeGpuPolicyFixture(argv[2], true);
      return 0;
    }
    if (argc == 3 && std::string_view(argv[1]) ==
                         "--write-gpu-paged-bootstrap-fixture") {
      writePagedBootstrapVolumeFixture(argv[2]);
      return 0;
    }
    if (argc == 3 && std::string_view(argv[1]) ==
                         "--write-gpu-query-fixture") {
      writeGpuVisibleUniformFixture(argv[2], false, false, false, false, true);
      return 0;
    }
    if (argc == 3 && std::string_view(argv[1]) ==
                         "--write-gpu-visible-extra-stream-fixture") {
      writeGpuVisibleUniformFixture(argv[2], false, false, true);
      return 0;
    }
    if (argc == 3 && std::string_view(argv[1]) ==
                         "--write-gpu-visible-extra-stream-altered-fixture") {
      writeGpuVisibleUniformFixture(argv[2], false, false, true, true);
      return 0;
    }
    if (argc == 3 && std::string_view(argv[1]) ==
                         "--write-gpu-visible-uniform-fixture") {
      writeGpuVisibleUniformFixture(argv[2]);
      return 0;
    }
    if (argc == 3 && std::string_view(argv[1]) ==
                         "--write-gpu-visible-uniform-altered-fixture") {
      writeGpuVisibleUniformFixture(argv[2], true);
      return 0;
    }
    if (argc == 3 && std::string_view(argv[1]) ==
                         "--write-gpu-visible-uniform-stale-vs-fixture") {
      writeGpuVisibleUniformFixture(argv[2], false, true);
      return 0;
    }
    if (argc == 3 && std::string_view(argv[1]) == "--write-parallel-fixture") {
      writeParallelFixture(argv[2]);
      return 0;
    }
    check(argc == 1,
          "usage: render_tape_provider_spec "
          "[--test-event-ordering|--test-query-oracle|--test-seed-reference-rejection|"
          "--test-volume-oracle|--test-retained-query-oracle|--test-retained-query-budget|"
          "--write-production-fixture dir|"
          "--write-projection-fixture dir|--write-sequence-fixture dir|"
          "--write-gpu-policy-fixture dir|"
          "--write-texture-input-audit-fixture dir|"
          "--write-gpu-visible-extra-stream-fixture dir|"
          "--write-gpu-visible-extra-stream-altered-fixture dir|"
          "--write-gpu-visible-uniform-fixture dir|"
          "--write-gpu-query-fixture dir|"
          "--write-gpu-paged-bootstrap-fixture dir|"
          "--write-gpu-visible-uniform-altered-fixture dir|"
          "--write-gpu-visible-uniform-stale-vs-fixture dir|"
          "--write-parallel-fixture dir]");
    completedBackbufferSeedAdmissionTruthTable();
    eventLevelMutationDrainOrderingIsPinned();
    bufferMutationReplayPlanPinsLockFlagsAndOffset();
    freshSeedDrainPredicateTruthTable();
    texturePrestageAdmissionExhaustiveEventOrder();
    preparedTapeOwnsImmutableInputAndExposesColdEvidence();
    sharedGeneralPreflightPlanMatchesProviderAndIsTransactional();
    sharedRuntimeObjectHelpersPinIdentityAndOwnership();
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
    nativeMetalPostCommandTexturePrestageMatchesOrdinaryPixels();
    generalIndexedReplayConservesAllObjects();
    sourceBoundaryExactCoverExhaustive();
    nativeMetalSourceBoundaryAudit();
    nativeMetalCaptureSourceReplayPolicy();
    nativeMetalTextureInputAudit();
    nativeMetalVisibleUniformPolicyOracle();
    productionParallelIndexedFixtureHasTwoChildren();
    boundedSequenceMutationIsVisibleAtSecondPresent();
    pagedBootstrapOutputUsesFinalOrderedAttachment();
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
