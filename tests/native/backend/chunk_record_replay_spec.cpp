#include "device_c_chunk_replay.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using dxmt9::d3d9::NonDrawReplaySink;
using dxmt9::d3d9::ResolvedRecordView;
using dxmt9::d3d9::SparseDrawCall;
using dxmt9::d3d9::SparseReplaySink;
using dxmt9::d3d9::kCommandChunkDecodeFailure;
using dxmt9::d3d9::replayNonDrawRecord;
using dxmt9::d3d9::replaySparseRecord;

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void check(bool condition, std::string_view message) {
  if (!condition) {
    throw TestFailure(std::string(message));
  }
}

template <typename T>
std::vector<std::byte> bytesOf(const T& value) {
  std::vector<std::byte> bytes(sizeof(T));
  std::memcpy(bytes.data(), &value, sizeof(value));
  return bytes;
}

template <typename T>
std::vector<std::byte> bytesWithTail(const T& value,
                                     std::span<const std::byte> tail) {
  auto bytes = bytesOf(value);
  bytes.insert(bytes.end(), tail.begin(), tail.end());
  return bytes;
}

ResolvedRecordView makeRecord(std::uint32_t type,
                                std::span<const std::byte> payload,
                                std::span<void* const> objects = {},
                                std::uint32_t firstHandle = 0u) {
  return ResolvedRecordView{
      .wire = dxmt9::d3d9::ImportedRecordView{
          .header = D9CCommandChunkWireRecordHeader{
              .type = type,
              .flags = 0u,
              .payloadOffset = 0u,
              .payloadSize = static_cast<std::uint32_t>(payload.size()),
              .firstHandle = firstHandle,
              .handleCount = static_cast<std::uint32_t>(objects.size()),
          },
          .payload = payload,
      },
      .objects = objects,
  };
}

std::size_t alignUp(std::size_t value, std::size_t alignment) {
  return (value + alignment - 1u) & ~(alignment - 1u);
}

struct SparseSectionInput {
  std::uint16_t kind = 0u;
  std::uint32_t count = 0u;
  std::vector<std::byte> bytes;
};

template <typename T>
SparseSectionInput sparseSection(std::uint16_t kind,
                                 std::span<const T> values) {
  return SparseSectionInput{
      .kind = kind,
      .count = static_cast<std::uint32_t>(values.size()),
      .bytes = std::vector<std::byte>(std::as_bytes(values).begin(),
                                      std::as_bytes(values).end()),
  };
}

struct SparseRecordFixture {
  std::uint32_t type = 0u;
  std::uint32_t firstHandle = 0u;
  D9CCommandChunkWireDrawHeader draw{};
  std::vector<std::byte> payload;
  std::vector<void*> objects;

  ResolvedRecordView view() const {
    const auto* sections =
        reinterpret_cast<const D9CCommandChunkWireSectionDesc*>(
            payload.data() + draw.sectionTableOffset);
    return ResolvedRecordView{
        .wire = dxmt9::d3d9::ImportedRecordView{
            .header = D9CCommandChunkWireRecordHeader{
                .type = type,
                .payloadSize = static_cast<std::uint32_t>(payload.size()),
                .firstHandle = firstHandle,
                .handleCount = static_cast<std::uint32_t>(objects.size()),
            },
            .payload = payload,
            .drawHeader = draw,
            .sections = std::span<const D9CCommandChunkWireSectionDesc>(
                sections, draw.sectionCount),
        },
        .objects = std::span<void* const>(objects.data(), objects.size()),
    };
  }
};

SparseRecordFixture makeSparseRecord(
    std::uint32_t type, D9CCommandChunkWireDrawHeader draw,
    std::span<const SparseSectionInput> sections,
    std::span<void* const> objects = {},
    std::uint32_t firstHandle = 0u) {
  draw.sectionCount = static_cast<std::uint32_t>(sections.size());
  draw.sectionTableOffset = sizeof(draw);
  draw.sectionPayloadOffset = static_cast<std::uint32_t>(alignUp(
      sizeof(draw) + sections.size() *
                         sizeof(D9CCommandChunkWireSectionDesc),
      alignof(std::uint32_t)));
  SparseRecordFixture fixture{
      .type = type,
      .firstHandle = firstHandle,
      .draw = draw,
      .payload = std::vector<std::byte>(draw.sectionPayloadOffset),
      .objects = std::vector<void*>(objects.begin(), objects.end()),
  };
  std::vector<D9CCommandChunkWireSectionDesc> descriptors;
  descriptors.reserve(sections.size());
  for (const auto& section : sections) {
    const auto* rule = dxmt9::d3d9::sectionRule(section.kind);
    check(rule != nullptr, "sparse replay fixture section is known");
    fixture.payload.resize(
        alignUp(fixture.payload.size(), rule->payloadAlignment));
    descriptors.push_back(D9CCommandChunkWireSectionDesc{
        .kind = section.kind,
        .elementSize = rule->elementSize,
        .count = section.count,
        .payloadOffset = static_cast<std::uint32_t>(fixture.payload.size()),
        .byteSize = static_cast<std::uint32_t>(section.bytes.size()),
    });
    fixture.payload.insert(fixture.payload.end(), section.bytes.begin(),
                           section.bytes.end());
  }
  std::memcpy(fixture.payload.data(), &draw, sizeof(draw));
  if (!descriptors.empty()) {
    std::memcpy(fixture.payload.data() + draw.sectionTableOffset,
                descriptors.data(), descriptors.size() * sizeof(descriptors[0]));
  }
  return fixture;
}

class RecordingSink final : public NonDrawReplaySink {
 public:
  std::array<std::uint32_t, 31> calls{};
  std::uint32_t currentType = 0u;
  std::uint32_t start = 0u;
  std::uint32_t count = 0u;
  std::size_t byteCount = 0u;
  void* firstObject = nullptr;
  void* secondObject = nullptr;

  std::int32_t result(std::uint32_t type) {
    currentType = type;
    ++calls[type];
    return static_cast<std::int32_t>(1000u + type);
  }

  std::int32_t setConstants(
      std::uint32_t type, const D9CCommandChunkWireSetConst& fixed,
      std::span<const std::byte> registerBytes) override {
    start = fixed.startRegister;
    count = fixed.registerCount;
    byteCount = registerBytes.size();
    return result(type);
  }

  std::int32_t clear(
      const D9CCommandChunkWireClear&,
      std::span<const D9CRect> rects) override {
    count = static_cast<std::uint32_t>(rects.size());
    return result(D9C_COMMAND_RECORD_CLEAR);
  }

  std::int32_t present(
      const D9CCommandChunkWirePresent&) override {
    return result(D9C_COMMAND_RECORD_PRESENT);
  }

  std::int32_t stretchRect(
      const D9CCommandChunkWireStretchRect&, void* src,
      void* dst) override {
    firstObject = src;
    secondObject = dst;
    return result(D9C_COMMAND_RECORD_STRETCH_RECT);
  }

  std::int32_t colorFill(
      const D9CCommandChunkWireColorFill&, void* surface) override {
    firstObject = surface;
    return result(D9C_COMMAND_RECORD_COLOR_FILL);
  }

  std::int32_t updateTexture(
      const D9CCommandChunkWireUpdateTexture&, void* src,
      void* dst) override {
    firstObject = src;
    secondObject = dst;
    return result(D9C_COMMAND_RECORD_UPDATE_TEXTURE);
  }

  std::int32_t updateSurface(
      const D9CCommandChunkWireUpdateSurface&, void* src,
      void* dst) override {
    firstObject = src;
    secondObject = dst;
    return result(D9C_COMMAND_RECORD_UPDATE_SURFACE);
  }

  std::int32_t queryIssue(
      const D9CCommandChunkWireQueryIssue&, void* query) override {
    firstObject = query;
    return result(D9C_COMMAND_RECORD_QUERY_ISSUE);
  }

  std::int32_t readback(
      const D9CCommandChunkWireReadback&, void* src,
      void* dst) override {
    firstObject = src;
    secondObject = dst;
    return result(D9C_COMMAND_RECORD_READBACK);
  }

  std::int32_t reszDepthResolve(
      const D9CCommandChunkWireReszDepthResolve&, void* msaaDepth,
      void* intzDest) override {
    firstObject = msaaDepth;
    secondObject = intzDest;
    return result(D9C_COMMAND_RECORD_RESZ_DEPTH_RESOLVE);
  }

  std::int32_t generateMipmaps(
      const D9CCommandChunkWireGenerateMipmaps&, void* texture) override {
    firstObject = texture;
    return result(D9C_COMMAND_RECORD_GENERATE_MIPMAPS);
  }

  std::int32_t applyState(
      const ResolvedRecordView&) override {
    return result(D9C_COMMAND_RECORD_APPLY_STATE);
  }
};

class SparseRecordingSink final : public SparseReplaySink {
 public:
  std::vector<std::uint16_t> order;
  std::array<void*, D9C_DRAW_PACKET_MAX_TEXTURES> textures{};
  std::array<void*, D9C_DRAW_PACKET_MAX_STREAMS> streams{};
  std::array<void*, 2> shaders{};
  void* vertexDeclaration = nullptr;
  std::uint32_t fvf = 0u;
  void* indexBuffer = nullptr;
  std::array<void*, D9C_DRAW_PACKET_MAX_RENDER_TARGETS> renderTargets{};
  void* depthStencil = nullptr;
  std::uint32_t renderStateValue = 0u;
  std::uint16_t constantKind = 0u;
  std::uint32_t constantStart = 0u;
  std::size_t constantBytes = 0u;
  std::uint32_t applyCount = 0u;
  std::uint32_t drawCount = 0u;
  SparseDrawCall lastDraw{};

  std::int32_t note(std::uint16_t kind) {
    order.push_back(kind);
    return 0;
  }

  std::int32_t setRenderStates(
      std::span<const D9CCommandChunkWireRenderState> values) override {
    if (!values.empty()) {
      renderStateValue = values.back().value;
    }
    return note(D9C_COMMAND_CHUNK_SECTION_RENDER_STATE);
  }

  std::int32_t setTexture(std::uint32_t slot, void* texture) override {
    textures[slot] = texture;
    return note(D9C_COMMAND_CHUNK_SECTION_TEXTURE);
  }

  std::int32_t setStream(
      const D9CCommandChunkWireStreamBinding& value,
      void* buffer) override {
    streams[value.slot] = buffer;
    return note(D9C_COMMAND_CHUNK_SECTION_STREAM);
  }

  std::int32_t setShader(std::uint32_t stage, void* shader) override {
    shaders[stage] = shader;
    return note(D9C_COMMAND_CHUNK_SECTION_SHADER);
  }

  std::int32_t setVertexInput(
      std::uint32_t kind, std::uint32_t value,
      void* declaration) override {
    if (kind == D9C_COMMAND_CHUNK_VERTEX_INPUT_FVF) {
      fvf = value;
      vertexDeclaration = nullptr;
    } else {
      vertexDeclaration = declaration;
    }
    return note(D9C_COMMAND_CHUNK_SECTION_VERTEX_INPUT);
  }

  std::int32_t setIndexBuffer(void* buffer) override {
    indexBuffer = buffer;
    return note(D9C_COMMAND_CHUNK_SECTION_INDEX_BUFFER);
  }

  std::int32_t setRenderTarget(
      std::uint32_t slot, void* surface) override {
    renderTargets[slot] = surface;
    return note(D9C_COMMAND_CHUNK_SECTION_RENDER_TARGET);
  }

  std::int32_t setDepthStencil(void* surface) override {
    depthStencil = surface;
    return note(D9C_COMMAND_CHUNK_SECTION_DEPTH_STENCIL);
  }

  std::int32_t setViewport(const D9CViewport&) override {
    return note(D9C_COMMAND_CHUNK_SECTION_VIEWPORT);
  }

  std::int32_t setScissor(const D9CRect&) override {
    return note(D9C_COMMAND_CHUNK_SECTION_SCISSOR);
  }

  std::int32_t setMaterial(const D9CMaterial&) override {
    return note(D9C_COMMAND_CHUNK_SECTION_MATERIAL);
  }

  std::int32_t setClipPlane(
      const D9CCommandChunkWireClipPlane&) override {
    return note(D9C_COMMAND_CHUNK_SECTION_CLIP_PLANE);
  }

  std::int32_t setTextureStageStates(
      std::span<const D9CDrawPacketTextureStageState>) override {
    return note(D9C_COMMAND_CHUNK_SECTION_TEXTURE_STAGE_STATE);
  }

  std::int32_t setSamplerStates(
      std::span<const D9CDrawPacketSamplerState>) override {
    return note(D9C_COMMAND_CHUNK_SECTION_SAMPLER_STATE);
  }

  std::int32_t setTransforms(
      std::span<const D9CDrawPacketTransform>) override {
    return note(D9C_COMMAND_CHUNK_SECTION_TRANSFORM);
  }

  std::int32_t setLights(
      std::span<const D9CCommandChunkWireLight>) override {
    return note(D9C_COMMAND_CHUNK_SECTION_LIGHT);
  }

  std::int32_t setLightEnables(
      std::span<const D9CCommandChunkWireLightEnable>) override {
    return note(D9C_COMMAND_CHUNK_SECTION_LIGHT_ENABLE);
  }

  std::int32_t setConstants(
      std::uint16_t sectionKind,
      const D9CCommandChunkWireConstantRange& range,
      std::span<const std::byte> registerBytes) override {
    constantKind = sectionKind;
    constantStart = range.startRegister;
    constantBytes = registerBytes.size();
    return note(sectionKind);
  }

  std::int32_t finishApplyState(std::uint32_t) override {
    ++applyCount;
    return 71;
  }

  std::int32_t draw(const SparseDrawCall& call) override {
    ++drawCount;
    lastDraw = call;
    return 73;
  }
};

void testConstantReplayMatrix() {
  RecordingSink sink;
  const std::array types = {
      D9C_COMMAND_RECORD_SET_VS_CONST_F,
      D9C_COMMAND_RECORD_SET_VS_CONST_I,
      D9C_COMMAND_RECORD_SET_VS_CONST_B,
      D9C_COMMAND_RECORD_SET_PS_CONST_F,
      D9C_COMMAND_RECORD_SET_PS_CONST_I,
      D9C_COMMAND_RECORD_SET_PS_CONST_B,
  };
  std::array<std::byte, 16> data{};
  for (const auto type : types) {
    const auto bytes =
        type == D9C_COMMAND_RECORD_SET_VS_CONST_B ||
                type == D9C_COMMAND_RECORD_SET_PS_CONST_B
            ? std::span<const std::byte>(data).first(4u)
            : std::span<const std::byte>(data);
    const D9CCommandChunkWireSetConst fixed{3u, 1u};
    const auto payload = bytesWithTail(fixed, bytes);
    const auto status = replayNonDrawRecord(
        makeRecord(type, payload), sink);
    check(status == static_cast<std::int32_t>(1000u + type) &&
              sink.start == 3u && sink.count == 1u &&
              sink.byteCount == bytes.size(),
          "constant replay preserves type, range, bytes, and sink HRESULT");
  }
}

void testOrderingAndResourceReplayMatrix() {
  RecordingSink sink;
  const std::array rects = {D9CRect{0, 0, 4, 4}, D9CRect{4, 4, 8, 8}};
  D9CCommandChunkWireClear clear{};
  clear.rectCount = static_cast<std::uint32_t>(rects.size());
  clear.rectOffset = sizeof(clear);
  const auto clearPayload = bytesWithTail(clear, std::as_bytes(std::span(rects)));
  check(replayNonDrawRecord(
            makeRecord(D9C_COMMAND_RECORD_CLEAR, clearPayload), sink) ==
            1000 + D9C_COMMAND_RECORD_CLEAR &&
            sink.count == rects.size(),
        "Clear replay exposes bounded rect span");

  const D9CCommandChunkWirePresent present{};
  const auto presentPayload = bytesOf(present);
  check(replayNonDrawRecord(
            makeRecord(D9C_COMMAND_RECORD_PRESENT, presentPayload), sink) ==
            1000 + D9C_COMMAND_RECORD_PRESENT,
        "Present replay preserves sink boundary result");

  int first = 1;
  int second = 2;
  std::array<void*, 2> objects{&first, &second};
  constexpr std::uint32_t firstHandle = 9u;

  const D9CCommandChunkWireStretchRect stretch{
      .srcHandleIndex = firstHandle,
      .dstHandleIndex = firstHandle + 1u,
  };
  auto payload = bytesOf(stretch);
  check(replayNonDrawRecord(
            makeRecord(D9C_COMMAND_RECORD_STRETCH_RECT, payload, objects,
                       firstHandle),
            sink) == 1000 + D9C_COMMAND_RECORD_STRETCH_RECT &&
            sink.firstObject == &first && sink.secondObject == &second,
        "StretchRect absolute indices resolve through record slice");

  const D9CCommandChunkWireColorFill color{
      .surfaceHandleIndex = firstHandle,
  };
  payload = bytesOf(color);
  check(replayNonDrawRecord(
            makeRecord(D9C_COMMAND_RECORD_COLOR_FILL, payload,
                       std::span<void* const>(objects).first(1u), firstHandle),
            sink) == 1000 + D9C_COMMAND_RECORD_COLOR_FILL &&
            sink.firstObject == &first,
        "ColorFill resolves its surface descriptor");

  const auto replayTwo = [&](std::uint32_t type, const auto& fixed) {
    const auto fixedBytes = bytesOf(fixed);
    const auto status = replayNonDrawRecord(
        makeRecord(type, fixedBytes, objects, firstHandle), sink);
    check(status == static_cast<std::int32_t>(1000u + type) &&
              sink.firstObject == &first && sink.secondObject == &second,
          "two-handle non-draw replay resolves in source order");
  };
  replayTwo(D9C_COMMAND_RECORD_UPDATE_TEXTURE,
            D9CCommandChunkWireUpdateTexture{firstHandle,
                                                firstHandle + 1u});
  replayTwo(D9C_COMMAND_RECORD_UPDATE_SURFACE,
            D9CCommandChunkWireUpdateSurface{
                .srcHandleIndex = firstHandle,
                .dstHandleIndex = firstHandle + 1u,
            });
  replayTwo(D9C_COMMAND_RECORD_READBACK,
            D9CCommandChunkWireReadback{firstHandle,
                                          firstHandle + 1u});
  replayTwo(D9C_COMMAND_RECORD_RESZ_DEPTH_RESOLVE,
            D9CCommandChunkWireReszDepthResolve{firstHandle,
                                                   firstHandle + 1u});

  const D9CCommandChunkWireGenerateMipmaps generate{firstHandle};
  payload = bytesOf(generate);
  check(replayNonDrawRecord(
            makeRecord(D9C_COMMAND_RECORD_GENERATE_MIPMAPS, payload,
                       std::span<void* const>(objects).first(1u), firstHandle),
            sink) == 1000 + D9C_COMMAND_RECORD_GENERATE_MIPMAPS &&
            sink.firstObject == &first,
        "GenerateMipmaps resolves its texture in source order");

  const D9CCommandChunkWireQueryIssue issue{firstHandle, 1u};
  payload = bytesOf(issue);
  check(replayNonDrawRecord(
            makeRecord(D9C_COMMAND_RECORD_QUERY_ISSUE, payload,
                       std::span<void* const>(objects).first(1u), firstHandle),
            sink) == 1000 + D9C_COMMAND_RECORD_QUERY_ISSUE &&
            sink.firstObject == &first,
        "Query::Issue resolves and preserves ordering");

  D9CCommandChunkWireDrawHeader apply{};
  apply.sectionTableOffset = sizeof(apply);
  apply.sectionPayloadOffset = sizeof(apply);
  payload = bytesOf(apply);
  check(replayNonDrawRecord(
            makeRecord(D9C_COMMAND_RECORD_APPLY_STATE, payload), sink) ==
            1000 + D9C_COMMAND_RECORD_APPLY_STATE,
        "APPLY_STATE routes through the sparse-state replay sink");
}

void testSparseStateSectionReplayWithoutLegacyPacket() {
  constexpr std::uint32_t firstHandle = 10u;
  int textureObject = 1;
  int bufferObject = 2;
  int shaderObject = 3;
  int declarationObject = 4;
  int surfaceObject = 5;
  std::array<void*, 5> objects{
      &textureObject,
      &bufferObject,
      &shaderObject,
      &declarationObject,
      &surfaceObject,
  };

  const std::array renderStates = {
      D9CCommandChunkWireRenderState{.state = 7u, .value = 91u},
  };
  const std::array textures = {
      D9CCommandChunkWireTextureBinding{
          .slot = 2u, .valid = 1u, .handleIndex = firstHandle},
  };
  const std::array streams = {
      D9CCommandChunkWireStreamBinding{
          .slot = 1u,
          .valid = 1u,
          .handleIndex = firstHandle + 1u,
          .offset = 4u,
          .stride = 16u,
          .frequency = 1u,
      },
  };
  const std::array shaders = {
      D9CCommandChunkWireShaderBinding{
          .stage = D9C_COMMAND_CHUNK_SHADER_STAGE_VERTEX,
          .valid = 1u,
          .handleIndex = firstHandle + 2u,
      },
  };
  const std::array vertexInputs = {
      D9CCommandChunkWireVertexInput{
          .valid = 1u,
          .kind = D9C_COMMAND_CHUNK_VERTEX_INPUT_DECLARATION,
          .handleIndex = firstHandle + 3u,
      },
  };
  const std::array indexBuffers = {
      D9CCommandChunkWireIndexBinding{
          .valid = 1u,
          .handleIndex = firstHandle + 1u,
      },
  };
  const std::array renderTargets = {
      D9CCommandChunkWireRenderTargetBinding{
          .slot = 0u,
          .valid = 1u,
          .handleIndex = firstHandle + 4u,
      },
  };
  const std::array depthStencils = {
      D9CCommandChunkWireDepthStencilBinding{
          .valid = 1u,
          .handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX,
      },
  };
  const std::array viewports = {
      D9CViewport{.width = 640u, .height = 480u, .maxZ = 1.0f},
  };
  const std::array scissors = {D9CRect{0, 0, 640, 480}};
  const std::array materials = {D9CMaterial{}};
  const std::array clipPlanes = {
      D9CCommandChunkWireClipPlane{.slot = 0u,
                                     .values = {0.0f, 1.0f, 0.0f, 0.0f}},
  };
  const std::array textureStageStates = {
      D9CDrawPacketTextureStageState{.stage = 0u, .type = 1u, .value = 2u},
  };
  const std::array samplerStates = {
      D9CDrawPacketSamplerState{.sampler = 0u, .type = 1u, .value = 2u},
  };
  const std::array transforms = {D9CDrawPacketTransform{.state = 2u}};
  const std::array lights = {D9CCommandChunkWireLight{.slot = 0u}};
  const std::array lightEnables = {
      D9CCommandChunkWireLightEnable{.slot = 0u, .enabled = 1u},
  };
  D9CCommandChunkWireConstantRange constantRange{3u, 1u};
  auto constantBytes = bytesOf(constantRange);
  constantBytes.resize(sizeof(constantRange) + 16u, std::byte{0x2a});

  std::vector<SparseSectionInput> sections;
  sections.push_back(sparseSection(
      D9C_COMMAND_CHUNK_SECTION_RENDER_STATE,
      std::span<const D9CCommandChunkWireRenderState>(renderStates)));
  sections.push_back(sparseSection(
      D9C_COMMAND_CHUNK_SECTION_TEXTURE,
      std::span<const D9CCommandChunkWireTextureBinding>(textures)));
  sections.push_back(sparseSection(
      D9C_COMMAND_CHUNK_SECTION_STREAM,
      std::span<const D9CCommandChunkWireStreamBinding>(streams)));
  sections.push_back(sparseSection(
      D9C_COMMAND_CHUNK_SECTION_SHADER,
      std::span<const D9CCommandChunkWireShaderBinding>(shaders)));
  sections.push_back(sparseSection(
      D9C_COMMAND_CHUNK_SECTION_VERTEX_INPUT,
      std::span<const D9CCommandChunkWireVertexInput>(vertexInputs)));
  sections.push_back(sparseSection(
      D9C_COMMAND_CHUNK_SECTION_INDEX_BUFFER,
      std::span<const D9CCommandChunkWireIndexBinding>(indexBuffers)));
  sections.push_back(sparseSection(
      D9C_COMMAND_CHUNK_SECTION_RENDER_TARGET,
      std::span<const D9CCommandChunkWireRenderTargetBinding>(
          renderTargets)));
  sections.push_back(sparseSection(
      D9C_COMMAND_CHUNK_SECTION_DEPTH_STENCIL,
      std::span<const D9CCommandChunkWireDepthStencilBinding>(
          depthStencils)));
  sections.push_back(sparseSection(
      D9C_COMMAND_CHUNK_SECTION_VIEWPORT,
      std::span<const D9CViewport>(viewports)));
  sections.push_back(sparseSection(
      D9C_COMMAND_CHUNK_SECTION_SCISSOR,
      std::span<const D9CRect>(scissors)));
  sections.push_back(sparseSection(
      D9C_COMMAND_CHUNK_SECTION_MATERIAL,
      std::span<const D9CMaterial>(materials)));
  sections.push_back(sparseSection(
      D9C_COMMAND_CHUNK_SECTION_CLIP_PLANE,
      std::span<const D9CCommandChunkWireClipPlane>(clipPlanes)));
  sections.push_back(sparseSection(
      D9C_COMMAND_CHUNK_SECTION_TEXTURE_STAGE_STATE,
      std::span<const D9CDrawPacketTextureStageState>(textureStageStates)));
  sections.push_back(sparseSection(
      D9C_COMMAND_CHUNK_SECTION_SAMPLER_STATE,
      std::span<const D9CDrawPacketSamplerState>(samplerStates)));
  sections.push_back(sparseSection(
      D9C_COMMAND_CHUNK_SECTION_TRANSFORM,
      std::span<const D9CDrawPacketTransform>(transforms)));
  sections.push_back(sparseSection(
      D9C_COMMAND_CHUNK_SECTION_LIGHT,
      std::span<const D9CCommandChunkWireLight>(lights)));
  sections.push_back(sparseSection(
      D9C_COMMAND_CHUNK_SECTION_LIGHT_ENABLE,
      std::span<const D9CCommandChunkWireLightEnable>(lightEnables)));
  sections.push_back(SparseSectionInput{
      .kind = D9C_COMMAND_CHUNK_SECTION_VS_CONST_F,
      .count = 1u,
      .bytes = constantBytes,
  });

  D9CCommandChunkWireDrawHeader draw{
      .primitiveType = 4u,
      .startVertex = 6u,
      .primitiveCount = 2u,
  };
  const auto fixture = makeSparseRecord(
      D9C_COMMAND_RECORD_DRAW_PRIMITIVE, draw, sections, objects,
      firstHandle);
  SparseRecordingSink sink;
  check(replaySparseRecord(fixture.view(), sink) == 73 &&
            sink.drawCount == 1u && sink.renderStateValue == 91u &&
            sink.textures[2] == &textureObject &&
            sink.streams[1] == &bufferObject &&
            sink.shaders[0] == &shaderObject &&
            sink.vertexDeclaration == &declarationObject &&
            sink.indexBuffer == &bufferObject &&
            sink.renderTargets[0] == &surfaceObject &&
            sink.depthStencil == nullptr,
        "sparse importer resolves canonical state operations without a legacy packet");
  check(std::is_sorted(sink.order.begin(), sink.order.end()) &&
            sink.constantKind == D9C_COMMAND_CHUNK_SECTION_VS_CONST_F &&
            sink.constantStart == 3u && sink.constantBytes == 16u &&
            sink.lastDraw.param.primitiveType ==
                dxmt9::core::PrimitiveType::TriangleList &&
            sink.lastDraw.param.startVertex == 6u &&
            sink.lastDraw.param.primitiveCount == 2u,
        "state sections and constants apply in canonical order before DrawParam");
}

void testAllDrawFormsAndSpanBackedUpPayloads() {
  SparseRecordingSink sink;
  D9CCommandChunkWireDrawHeader indexed{
      .primitiveType = 5u,
      .baseVertex = -2,
      .numVertices = 8u,
      .startIndex = 4u,
      .primitiveCount = 3u,
  };
  const auto indexedFixture = makeSparseRecord(
      D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE, indexed, {});
  check(replaySparseRecord(indexedFixture.view(), sink) == 73 &&
            sink.lastDraw.param.indexed &&
            sink.lastDraw.param.baseVertexIndex == -2 &&
            sink.lastDraw.param.startIndex == 4u,
        "indexed draw normalizes fixed arguments to DrawParam");

  const std::array<std::byte, 12> vertices{};
  const std::array directUpSections = {
      sparseSection(D9C_COMMAND_CHUNK_SECTION_UP_VERTEX_DATA,
                    std::span<const std::byte>(vertices)),
  };
  D9CCommandChunkWireDrawHeader directUp{
      .primitiveType = 4u,
      .primitiveCount = 1u,
      .stride = 4u,
  };
  const auto directUpFixture = makeSparseRecord(
      D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP, directUp, directUpSections);
  check(replaySparseRecord(directUpFixture.view(), sink) == 73 &&
            !sink.lastDraw.param.indexed &&
            sink.lastDraw.payload.userVertexData.size() == vertices.size() &&
            sink.lastDraw.payload.userIndexData.empty(),
        "direct-UP keeps the validated vertex range span-backed");

  const std::array<std::byte, 6> indices{};
  const std::array indexedUpSections = {
      sparseSection(D9C_COMMAND_CHUNK_SECTION_UP_INDEX_DATA,
                    std::span<const std::byte>(indices)),
      sparseSection(D9C_COMMAND_CHUNK_SECTION_UP_VERTEX_DATA,
                    std::span<const std::byte>(vertices)),
  };
  D9CCommandChunkWireDrawHeader indexedUp{
      .primitiveType = 4u,
      .numVertices = 3u,
      .primitiveCount = 1u,
      .stride = 4u,
      .indexFormat = 101u,
  };
  const auto indexedUpFixture = makeSparseRecord(
      D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP, indexedUp,
      indexedUpSections);
  check(replaySparseRecord(indexedUpFixture.view(), sink) == 73 &&
            sink.lastDraw.param.indexed &&
            sink.lastDraw.param.indexType == dxmt9::core::IndexType::UInt16 &&
            sink.lastDraw.payload.userIndexData.size() == indices.size() &&
            sink.lastDraw.payload.userVertexData.size() == vertices.size(),
        "indexed-UP preserves ordered index and vertex payload spans");

  const std::array applyState = {
      D9CCommandChunkWireRenderState{.state = 1u, .value = 44u},
  };
  const std::array applySections = {
      sparseSection(D9C_COMMAND_CHUNK_SECTION_RENDER_STATE,
                    std::span<const D9CCommandChunkWireRenderState>(
                        applyState)),
  };
  const auto applyFixture = makeSparseRecord(
      D9C_COMMAND_RECORD_APPLY_STATE, {}, applySections);
  check(replaySparseRecord(applyFixture.view(), sink) == 71 &&
            sink.applyCount == 1u && sink.renderStateValue == 44u &&
            sink.drawCount == 3u,
        "APPLY_STATE uses identical sparse state operations without drawing");
}

void testSparseCarryUnbindFullSnapshotAndPreflight() {
  SparseRecordingSink sink;
  int textureObject = 1;
  std::array<void*, 1> object{&textureObject};
  const std::array bind = {
      D9CCommandChunkWireTextureBinding{
          .slot = 0u, .valid = 1u, .handleIndex = 0u},
  };
  const std::array bindSection = {
      sparseSection(D9C_COMMAND_CHUNK_SECTION_TEXTURE,
                    std::span<const D9CCommandChunkWireTextureBinding>(bind)),
  };
  D9CCommandChunkWireDrawHeader draw{
      .primitiveType = 4u,
      .primitiveCount = 1u,
  };
  const auto bindFixture = makeSparseRecord(
      D9C_COMMAND_RECORD_DRAW_PRIMITIVE, draw, bindSection, object);
  const auto emptyFixture = makeSparseRecord(
      D9C_COMMAND_RECORD_DRAW_PRIMITIVE, draw, {});
  check(replaySparseRecord(bindFixture.view(), sink) == 73 &&
            replaySparseRecord(emptyFixture.view(), sink) == 73 &&
            sink.textures[0] == &textureObject,
        "an absent section carries canonical state across records");

  const std::array unbind = {
      D9CCommandChunkWireTextureBinding{
          .slot = 0u,
          .valid = 1u,
          .handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX,
      },
  };
  const std::array unbindSection = {
      sparseSection(
          D9C_COMMAND_CHUNK_SECTION_TEXTURE,
          std::span<const D9CCommandChunkWireTextureBinding>(unbind)),
  };
  const auto unbindFixture = makeSparseRecord(
      D9C_COMMAND_RECORD_DRAW_PRIMITIVE, draw, unbindSection);
  check(replaySparseRecord(unbindFixture.view(), sink) == 73 &&
            sink.textures[0] == nullptr,
        "valid plus null index performs an explicit unbind");

  std::array<D9CCommandChunkWireTextureBinding,
             D9C_DRAW_PACKET_MAX_TEXTURES>
      textures{};
  for (std::uint32_t slot = 0u; slot < textures.size(); ++slot) {
    textures[slot].slot = slot;
    textures[slot].valid = 1u;
    textures[slot].handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX;
    sink.textures[slot] = &textureObject;
  }
  std::array<D9CCommandChunkWireStreamBinding,
             D9C_DRAW_PACKET_MAX_STREAMS>
      streams{};
  for (std::uint32_t slot = 0u; slot < streams.size(); ++slot) {
    streams[slot].slot = slot;
    streams[slot].valid = 1u;
    streams[slot].handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX;
    sink.streams[slot] = &textureObject;
  }
  const std::array fullSections = {
      sparseSection(
          D9C_COMMAND_CHUNK_SECTION_TEXTURE,
          std::span<const D9CCommandChunkWireTextureBinding>(textures)),
      sparseSection(
          D9C_COMMAND_CHUNK_SECTION_STREAM,
          std::span<const D9CCommandChunkWireStreamBinding>(streams)),
  };
  draw.flags = D9C_COMMAND_CHUNK_DRAW_FLAG_FULL_SNAPSHOT;
  const auto fullFixture = makeSparseRecord(
      D9C_COMMAND_RECORD_DRAW_PRIMITIVE, draw, fullSections);
  check(replaySparseRecord(fullFixture.view(), sink) == 73 &&
            std::all_of(sink.textures.begin(), sink.textures.end(),
                        [](const void* value) { return value == nullptr; }) &&
            std::all_of(sink.streams.begin(), sink.streams.end(),
                        [](const void* value) { return value == nullptr; }) &&
            sink.lastDraw.flags ==
                D9C_COMMAND_CHUNK_DRAW_FLAG_FULL_SNAPSHOT,
        "full snapshot replays every null texture and stream slot");

  std::array<void*, 1> unresolved{nullptr};
  const auto unresolvedFixture = makeSparseRecord(
      D9C_COMMAND_RECORD_DRAW_PRIMITIVE, draw, bindSection, unresolved);
  SparseRecordingSink untouched;
  check(replaySparseRecord(unresolvedFixture.view(), untouched) ==
            kCommandChunkDecodeFailure &&
            untouched.order.empty() && untouched.drawCount == 0u,
        "all registry objects resolve before the first state mutation");
}

void testRejectsUnresolvedAndDrawRecords() {
  RecordingSink sink;
  const D9CCommandChunkWireUpdateTexture update{0u, 1u};
  const auto payload = bytesOf(update);
  std::array<void*, 2> unresolved{reinterpret_cast<void*>(1u), nullptr};
  check(replayNonDrawRecord(
            makeRecord(D9C_COMMAND_RECORD_UPDATE_TEXTURE, payload, unresolved),
            sink) == kCommandChunkDecodeFailure,
        "unresolved registry object rejects before sink entry");

  D9CCommandChunkWireDrawHeader draw{};
  const auto drawPayload = bytesOf(draw);
  check(replayNonDrawRecord(
            makeRecord(D9C_COMMAND_RECORD_DRAW_PRIMITIVE, drawPayload), sink) ==
            kCommandChunkDecodeFailure,
        "draw opcode cannot enter non-draw replay path");
}

}  // namespace

int main() {
  try {
    testConstantReplayMatrix();
    testOrderingAndResourceReplayMatrix();
    testSparseStateSectionReplayWithoutLegacyPacket();
    testAllDrawFormsAndSpanBackedUpPayloads();
    testSparseCarryUnbindFullSnapshotAndPreflight();
    testRejectsUnresolvedAndDrawRecords();
  } catch (const TestFailure& error) {
    std::cerr << "chunk_record_replay_spec failed: " << error.what()
              << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "chunk_record_replay_spec passed\n";
  return EXIT_SUCCESS;
}
