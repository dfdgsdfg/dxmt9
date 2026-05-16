// R-TEST-0.10 / B2 imported-record value boundary coverage.
//
// Observable blockers without a production test seam:
// - CommandQueue's SET_CONST dirty-range accumulator is private. This spec
//   verifies constant payloads at the core DeviceState and submitted
//   DrawUniformPayload boundaries, but it cannot assert the exact dirty-range
//   bitsets that commit_chunk ORs into the queue.
// - Imported RT/DS replay exposes RenderTargetAttachment metadata (handle,
//   level, sample count) at the draw boundary. Surface format/extent descriptor
//   preservation belongs to the resource-creation boundary lane.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "device_c_common.hpp"
#include "dxmt9/com.hpp"
#include "dxmt9/core.hpp"
#include "dxmt9/device_c.h"
#include "dxmt9/dxmt9_device.hpp"

namespace {

using namespace dxmt9::core;

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

[[noreturn]] void fail(std::string message) {
  throw TestFailure(std::move(message));
}

void check(bool condition, std::string_view message) {
  if (!condition) {
    fail(std::string(message));
  }
}

template <typename A, typename B>
void checkEq(const A& left, const B& right, std::string_view message) {
  if (!(left == right)) {
    fail(std::string(message));
  }
}

void checkNear(float left, float right, std::string_view message) {
  if (std::abs(left - right) > 0.0001f) {
    fail(std::string(message));
  }
}

void checkColor(const ColorRGBA& left, const ColorRGBA& right,
                std::string_view message) {
  checkNear(left.r, right.r, message);
  checkNear(left.g, right.g, message);
  checkNear(left.b, right.b, message);
  checkNear(left.a, right.a, message);
}

void checkMatrix(const Matrix4x4& left, const Matrix4x4& right,
                 std::string_view message) {
  for (std::size_t i = 0; i < left.m.size(); ++i) {
    checkNear(left.m[i], right.m[i], message);
  }
}

void checkClipPlane(const ClipPlane& left, const ClipPlane& right,
                    std::string_view message) {
  for (std::size_t i = 0; i < left.size(); ++i) {
    checkNear(left[i], right[i], message);
  }
}

enum class EventKind {
  MarkChunkResources,
  SetSkipDrawResourceMarking,
  SubmitDraw,
  Flush,
};

struct RecordedDrawRun {
  CanonicalDrawState state{};
  FlatDrawStateRecord hot{};
  DrawUniformPayload uniforms{};
  std::vector<DrawParam> draws;
  std::vector<u8> payloadArena;
};

struct RecordedEvent {
  EventKind kind = EventKind::Flush;
  bool skipDrawResourceMarking = false;
  std::vector<ChunkHandleEntry> chunkHandles;
  RecordedDrawRun drawRun;
};

struct RecordingDxmt9Device final : dxmt9::Device {
  RecordingDxmt9Device()
      : limits_{}, queue_(WMT::Device{NULL_OBJECT_HANDLE}, limits_) {}

  WMT::Device wmtDevice() override { return WMT::Device{NULL_OBJECT_HANDLE}; }
  dxmt9::CommandQueue& queue() override { return queue_; }
  const BackendLimits& limits() const override { return limits_; }
  std::shared_ptr<BackendDevice> backend() override { return {}; }

  void setDeviceLostObserver(BackendDevice::DeviceLostObserver observer) override {
    deviceLostObserver = std::move(observer);
  }

  void setPresentationStatusObserver(
      BackendDevice::PresentationStatusObserver observer) override {
    presentationStatusObserver = std::move(observer);
  }

  BufferHandle createBuffer(const BufferDesc&) override {
    return BufferHandle{nextHandle++};
  }

  TextureHandle createTexture(const TextureDesc&) override {
    return TextureHandle{nextHandle++};
  }

  SurfaceHandle createSurface(const SurfaceDesc&) override {
    return SurfaceHandle{nextHandle++};
  }

  SurfaceHandle createSurfaceForTexture(TextureHandle, std::uint32_t,
                                        const SurfaceDesc&) override {
    return SurfaceHandle{nextHandle++};
  }

  void markChunkResources(std::span<const ChunkHandleEntry> entries) override {
    RecordedEvent event;
    event.kind = EventKind::MarkChunkResources;
    event.chunkHandles.assign(entries.begin(), entries.end());
    events.push_back(std::move(event));
  }

  void setSkipDrawResourceMarking(bool skip) override {
    RecordedEvent event;
    event.kind = EventKind::SetSkipDrawResourceMarking;
    event.skipDrawResourceMarking = skip;
    events.push_back(std::move(event));
  }

  void submitDrawRun(CanonicalDrawState state, const DrawUniformPayload& uniforms,
                     std::span<const DrawParam> draws,
                     std::span<const DrawParamPayloadView> payloads) override {
    RecordedEvent event;
    event.kind = EventKind::SubmitDraw;
    event.drawRun.state = std::move(state);
    event.drawRun.hot = event.drawRun.state.hot;
    event.drawRun.uniforms = uniforms;

    auto appendPayload = [&](std::span<const u8> bytes) -> DrawPayloadRange {
      if (bytes.empty()) {
        return {};
      }
      const auto offset = static_cast<u32>(event.drawRun.payloadArena.size());
      event.drawRun.payloadArena.insert(event.drawRun.payloadArena.end(),
                                        bytes.begin(), bytes.end());
      return DrawPayloadRange{
          .offset = offset,
          .size = static_cast<u32>(bytes.size()),
      };
    };

    event.drawRun.draws.reserve(draws.size());
    for (std::size_t i = 0; i < draws.size(); ++i) {
      DrawParam param = draws[i];
      const DrawParamPayloadView payload =
          i < payloads.size() ? payloads[i] : DrawParamPayloadView{};
      param.userVertexRange = appendPayload(payload.userVertexData);
      param.userIndexRange = appendPayload(payload.userIndexData);
      event.drawRun.draws.push_back(param);
    }
    events.push_back(std::move(event));
  }

  void flush() override {
    RecordedEvent event;
    event.kind = EventKind::Flush;
    events.push_back(std::move(event));
  }

  BackendLimits limits_{};
  dxmt9::CommandQueue queue_;
  std::uint64_t nextHandle = 1;
  std::vector<RecordedEvent> events;
  BackendDevice::DeviceLostObserver deviceLostObserver;
  BackendDevice::PresentationStatusObserver presentationStatusObserver;
};

D9CWireHandle wireHandleFromValue(std::uint64_t value) {
  return D9CWireHandle{
      .lo = static_cast<std::uint32_t>(value),
      .hi = static_cast<std::uint32_t>(value >> 32),
  };
}

std::uint64_t wireValueFromPtr(const void* ptr) {
  return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(ptr));
}

D9CWireHandle wireHandleFromPtr(const void* ptr) {
  return wireHandleFromValue(wireValueFromPtr(ptr));
}

template <typename T>
void appendPod(std::vector<std::uint8_t>& bytes, const T& value) {
  const auto* begin = reinterpret_cast<const std::uint8_t*>(&value);
  bytes.insert(bytes.end(), begin, begin + sizeof(T));
}

template <typename T>
std::uint32_t appendRecord(std::vector<std::uint8_t>& bytes, const T& record) {
  const auto offset = static_cast<std::uint32_t>(bytes.size());
  appendPod(bytes, record);
  return offset;
}

template <typename T>
std::uint32_t appendConstRecord(std::vector<std::uint8_t>& bytes,
                                std::uint32_t type,
                                std::uint32_t start,
                                std::uint32_t count,
                                std::span<const T> payload) {
  D9CCommandRecordSetConst record{};
  record.header.type = type;
  record.header.size = static_cast<std::uint32_t>(
      sizeof(record) + payload.size_bytes());
  record.start = start;
  record.count = count;

  const auto offset = appendRecord(bytes, record);
  const auto* begin = reinterpret_cast<const std::uint8_t*>(payload.data());
  bytes.insert(bytes.end(), begin, begin + payload.size_bytes());
  return offset;
}

D9CCommandChunkWireHandleEntry wireHandleEntry(std::uint32_t kind,
                                               const void* ptr) {
  return D9CCommandChunkWireHandleEntry{
      .kind = kind,
      .generation = D9C_COMMAND_CHUNK_WIRE_HANDLE_GENERATION_NONE,
      .opaqueHandle = wireValueFromPtr(ptr),
      .reserved0 = 0u,
      .reserved1 = 0u,
  };
}

D9CCommandChunkWireRecordHeader wireRecordHeader(std::uint32_t type,
                                                 std::uint32_t payloadOffset,
                                                 std::uint32_t payloadSize,
                                                 std::uint32_t firstHandle = 0u,
                                                 std::uint32_t handleCount = 0u) {
  return D9CCommandChunkWireRecordHeader{
      .type = type,
      .flags = D9C_COMMAND_CHUNK_WIRE_RECORD_FLAG_NONE,
      .payloadOffset = payloadOffset,
      .payloadSize = payloadSize,
      .firstHandle = firstHandle,
      .handleCount = handleCount,
      .reserved0 = 0u,
      .reserved1 = 0u,
  };
}

std::vector<std::uint8_t> makeWireChunkBlob(
    std::span<const std::uint8_t> payload,
    std::span<const D9CCommandChunkWireRecordHeader> records,
    std::span<const D9CCommandChunkWireHandleEntry> handles) {
  D9CCommandChunkWireHeader header{};
  header.version = D9C_COMMAND_CHUNK_WIRE_VERSION;
  header.headerSize = D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE;
  header.recordHeaderSize = D9C_COMMAND_CHUNK_WIRE_RECORD_HEADER_SIZE;
  header.handleEntrySize = D9C_COMMAND_CHUNK_WIRE_HANDLE_ENTRY_SIZE;
  header.recordTableOffset = D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE;
  header.recordCount = static_cast<std::uint32_t>(records.size());
  header.handleTableOffset = header.recordTableOffset +
      header.recordCount * D9C_COMMAND_CHUNK_WIRE_RECORD_HEADER_SIZE;
  header.handleCount = static_cast<std::uint32_t>(handles.size());
  header.payloadArenaOffset = header.handleTableOffset +
      header.handleCount * D9C_COMMAND_CHUNK_WIRE_HANDLE_ENTRY_SIZE;
  header.payloadArenaSize = static_cast<std::uint32_t>(payload.size());

  std::vector<std::uint8_t> blob;
  blob.reserve(header.payloadArenaOffset + header.payloadArenaSize);
  appendPod(blob, header);
  for (const auto& record : records) appendPod(blob, record);
  for (const auto& handle : handles) appendPod(blob, handle);
  blob.insert(blob.end(), payload.begin(), payload.end());
  return blob;
}

int32_t commitWireChunk(D9CDevice& cDevice,
                        const std::vector<std::uint8_t>& wireBlob,
                        std::uint32_t recordCount,
                        std::uint32_t handleCount) {
  D9CCommandChunk chunk{};
  chunk.version = D9C_COMMAND_CHUNK_VERSION;
  chunk.recordCount = recordCount;
  chunk.recordBytes = static_cast<std::uint32_t>(wireBlob.size());
  chunk.records = wireHandleFromPtr(wireBlob.data());
  chunk.handleCount = handleCount;
  chunk.handles = {};
  return dxmt9c_device_commit_chunk(&cDevice, &chunk);
}

bool containsChunkHandle(const std::vector<ChunkHandleEntry>& entries,
                         ChunkHandleKind kind,
                         Handle handle) {
  return std::find_if(entries.begin(), entries.end(), [&](const auto& entry) {
           return entry.kind == kind && entry.handle == handle;
         }) != entries.end();
}

void checkEventKind(const std::vector<RecordedEvent>& events,
                    std::size_t index,
                    EventKind expected,
                    std::string_view message) {
  check(index < events.size(), message);
  check(events[index].kind == expected, message);
}

Matrix4x4 identityMatrix() {
  Matrix4x4 matrix{};
  matrix.m[0] = 1.0f;
  matrix.m[5] = 1.0f;
  matrix.m[10] = 1.0f;
  matrix.m[15] = 1.0f;
  return matrix;
}

Matrix4x4 taggedMatrix(float base) {
  Matrix4x4 matrix{};
  for (std::size_t i = 0; i < matrix.m.size(); ++i) {
    matrix.m[i] = base + static_cast<float>(i);
  }
  return matrix;
}

D9CMatrix matrixToC(const Matrix4x4& matrix) {
  D9CMatrix out{};
  std::memcpy(out.m, matrix.m.data(), matrix.m.size() * sizeof(float));
  return out;
}

ShaderRef makeBytecodeShaderRef(std::uint64_t hash,
                                std::initializer_list<u8> bytes) {
  ShaderRef shader{};
  shader.kind = ShaderRef::Kind::Bytecode;
  shader.hash = hash;
  shader.bytecode.hash = hash ^ 0x9e3779b97f4a7c15ull;
  shader.bytecode.bytes.assign(bytes.begin(), bytes.end());
  return shader;
}

D9CCommandRecordApplyState makeApplyStateRecord() {
  D9CCommandRecordApplyState record{};
  record.header.type = D9C_COMMAND_RECORD_APPLY_STATE;
  record.header.size = sizeof(record);
  return record;
}

D9CCommandRecordDrawPrimitive makeDrawRecord(std::uint32_t startVertex,
                                             std::uint32_t primitiveCount) {
  D9CCommandRecordDrawPrimitive record{};
  record.header.type = D9C_COMMAND_RECORD_DRAW_PRIMITIVE;
  record.header.size = sizeof(record);
  record.packet.primitiveType = 4u;
  record.packet.startVertex = startVertex;
  record.packet.primitiveCount = primitiveCount;
  return record;
}

D9CCommandRecordDrawIndexedPrimitive makeIndexedDrawRecord(D9CBuffer* indexBuffer) {
  D9CCommandRecordDrawIndexedPrimitive record{};
  record.header.type = D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE;
  record.header.size = sizeof(record);
  record.packet.state.primitiveType = 5u;
  record.packet.primitiveCount = 3u;
  record.packet.baseVertex = -7;
  record.packet.minVertex = 4u;
  record.packet.numVertices = 44u;
  record.packet.startIndex = 11u;
  record.packet.ibValid = 1u;
  record.packet.ibHandle = wireHandleFromPtr(indexBuffer);
  return record;
}

D9CMaterial makeMaterialRecord() {
  D9CMaterial material{};
  material.diffuse = {0.10f, 0.20f, 0.30f, 0.40f};
  material.ambient = {0.50f, 0.60f, 0.70f, 0.80f};
  material.specular = {0.90f, 1.00f, 1.10f, 1.20f};
  material.emissive = {1.30f, 1.40f, 1.50f, 1.60f};
  material.power = 7.25f;
  return material;
}

Material expectedMaterial() {
  Material material{};
  material.diffuse = {0.10f, 0.20f, 0.30f, 0.40f};
  material.ambient = {0.50f, 0.60f, 0.70f, 0.80f};
  material.specular = {0.90f, 1.00f, 1.10f, 1.20f};
  material.emissive = {1.30f, 1.40f, 1.50f, 1.60f};
  material.power = 7.25f;
  return material;
}

D9CLight makeLightRecord() {
  D9CLight light{};
  light.type = static_cast<std::uint32_t>(LightType::Spot);
  light.diffuse = {0.11f, 0.12f, 0.13f, 0.14f};
  light.specular = {0.21f, 0.22f, 0.23f, 0.24f};
  light.ambient = {0.31f, 0.32f, 0.33f, 0.34f};
  light.position[0] = 4.0f;
  light.position[1] = 5.0f;
  light.position[2] = 6.0f;
  light.direction[0] = -1.0f;
  light.direction[1] = -2.0f;
  light.direction[2] = -3.0f;
  light.range = 99.0f;
  light.falloff = 0.75f;
  light.attenuation0 = 0.01f;
  light.attenuation1 = 0.02f;
  light.attenuation2 = 0.03f;
  light.theta = 0.40f;
  light.phi = 0.80f;
  return light;
}

Light expectedLight() {
  Light light{};
  light.type = LightType::Spot;
  light.enabled = true;
  light.diffuse = {0.11f, 0.12f, 0.13f, 0.14f};
  light.specular = {0.21f, 0.22f, 0.23f, 0.24f};
  light.ambient = {0.31f, 0.32f, 0.33f, 0.34f};
  light.position = {4.0f, 5.0f, 6.0f};
  light.direction = {-1.0f, -2.0f, -3.0f};
  light.range = 99.0f;
  light.falloff = 0.75f;
  light.attenuation0 = 0.01f;
  light.attenuation1 = 0.02f;
  light.attenuation2 = 0.03f;
  light.theta = 0.40f;
  light.phi = 0.80f;
  return light;
}

void assertMaterial(const Material& actual, const Material& expected,
                    std::string_view message) {
  checkColor(actual.diffuse, expected.diffuse, message);
  checkColor(actual.ambient, expected.ambient, message);
  checkColor(actual.specular, expected.specular, message);
  checkColor(actual.emissive, expected.emissive, message);
  checkNear(actual.power, expected.power, message);
}

void assertLight(const Light& actual, const Light& expected,
                 std::string_view message) {
  checkEq(actual.type, expected.type, message);
  checkEq(actual.enabled, expected.enabled, message);
  checkColor(actual.diffuse, expected.diffuse, message);
  checkColor(actual.specular, expected.specular, message);
  checkColor(actual.ambient, expected.ambient, message);
  for (std::size_t i = 0; i < actual.position.size(); ++i) {
    checkNear(actual.position[i], expected.position[i], message);
    checkNear(actual.direction[i], expected.direction[i], message);
  }
  checkNear(actual.range, expected.range, message);
  checkNear(actual.falloff, expected.falloff, message);
  checkNear(actual.attenuation0, expected.attenuation0, message);
  checkNear(actual.attenuation1, expected.attenuation1, message);
  checkNear(actual.attenuation2, expected.attenuation2, message);
  checkNear(actual.theta, expected.theta, message);
  checkNear(actual.phi, expected.phi, message);
}

void assertConstants(const DrawUniformPayload& uniforms,
                     std::string_view message) {
  checkNear(uniforms.vsConst.float4[3][0], 1.25f, message);
  checkNear(uniforms.vsConst.float4[3][1], 2.25f, message);
  checkNear(uniforms.vsConst.float4[3][2], 3.25f, message);
  checkNear(uniforms.vsConst.float4[3][3], 4.25f, message);
  checkNear(uniforms.vsConst.float4[4][0], 5.25f, message);
  checkNear(uniforms.vsConst.float4[4][3], 8.25f, message);
  checkNear(uniforms.psConst.float4[5][0], 9.5f, message);
  checkNear(uniforms.psConst.float4[5][3], 12.5f, message);

  checkEq(uniforms.vsConst.int4[2][0], -1, message);
  checkEq(uniforms.vsConst.int4[2][3], -4, message);
  checkEq(uniforms.vsConst.int4[3][0], 5, message);
  checkEq(uniforms.vsConst.int4[3][3], 8, message);
  checkEq(uniforms.psConst.int4[4][0], 9, message);
  checkEq(uniforms.psConst.int4[4][3], 12, message);

  check(uniforms.vsConst.bools[1], message);
  check(!uniforms.vsConst.bools[2], message);
  check(uniforms.vsConst.bools[3], message);
  check(!uniforms.psConst.bools[2], message);
  check(uniforms.psConst.bools[3], message);
}

void assertDeviceStateValues(const DeviceState& state,
                             const std::shared_ptr<Texture>& texture,
                             const std::shared_ptr<Buffer>& vertexBuffer,
                             const std::shared_ptr<Buffer>& indexBuffer,
                             const std::shared_ptr<Surface>& renderTarget,
                             const std::shared_ptr<Surface>& depthStencil,
                             const D9CVertexDecl& vertexDecl,
                             const D9CShader& vertexShader,
                             const D9CShader& pixelShader,
                             const Matrix4x4& textureTransform,
                             const ClipPlane& clipPlane) {
  checkEq(state.renderStates.at(RS_SCISSOR_TEST_ENABLE), 1u,
          "core state scissor render state value");
  checkEq(state.renderStates.at(RS_CLIP_PLANE_ENABLE), 1u << 2u,
          "core state clip-plane render state value");
  checkEq(state.renderStates.at(RS_TEXTURE_FACTOR), 0xa0b0c0d0u,
          "core state texture-factor render state value");

  checkEq(state.textureStageStates[3].at(TSS_COLOR_OP), 4u,
          "core state TSS color op");
  checkEq(state.textureStageStates[3].at(TSS_ALPHA_ARG1), 0x20u,
          "core state TSS alpha arg");
  checkEq(state.samplerStates[5].at(SAMP_ADDRESS_U), 3u,
          "core state sampler address U");
  checkEq(state.samplerStates[6].at(SAMP_MIN_FILTER), 2u,
          "core state sampler min filter");

  check(state.textures[2] == texture, "core state texture handle");
  check(state.streamBuffers[1] == vertexBuffer, "core state stream buffer");
  checkEq(state.streamOffsets[1], 17u, "core state stream offset");
  checkEq(state.streamStrides[1], 36u, "core state stream stride");
  check(state.indexBuffer == indexBuffer, "core state index buffer");
  checkEq(state.indexType, IndexType::UInt32, "core state index type");

  checkEq(state.renderTargets[0].handle.value, renderTarget->handle().value,
          "core state RT handle");
  checkEq(state.renderTargets[0].level, 0u, "core state RT level");
  checkEq(state.renderTargets[0].sampleCount, 4u, "core state RT sample count");
  checkEq(state.depthStencil.handle.value, depthStencil->handle().value,
          "core state DS handle");
  checkEq(state.depthStencil.sampleCount, 2u, "core state DS sample count");

  checkEq(state.viewport.x, 9u, "core state viewport x");
  checkEq(state.viewport.y, 10u, "core state viewport y");
  checkEq(state.viewport.width, 111u, "core state viewport width");
  checkEq(state.viewport.height, 77u, "core state viewport height");
  checkNear(state.viewport.minZ, 0.125f, "core state viewport minZ");
  checkNear(state.viewport.maxZ, 0.875f, "core state viewport maxZ");
  checkEq(state.scissorRect, Rect{2, 3, 54, 65}, "core state scissor rect");
  check(state.scissorEnabled, "core state scissor enabled cache");

  assertMaterial(state.material, expectedMaterial(), "core state material");
  assertLight(state.lights[4], expectedLight(), "core state light data");
  check(state.lightEnabled[4], "core state light enable table");
  checkClipPlane(state.clipPlanes[2], clipPlane, "core state clip plane");
  checkMatrix(state.transforms[XFORM_WORLD_BASE], identityMatrix(),
              "core state world transform");
  checkMatrix(state.transforms[XFORM_VIEW], identityMatrix(),
              "core state view transform");
  checkMatrix(state.transforms[XFORM_PROJECTION], identityMatrix(),
              "core state projection transform");
  checkMatrix(state.transforms[XFORM_TEXTURE_BASE], textureTransform,
              "core state texture transform");

  checkEq(state.fvf, 0x142u, "core state FVF preserved after vdecl");
  check(state.vertexDecl.elements == vertexDecl.elements,
        "core state vertex declaration elements");
  checkEq(state.vertexDecl.fvf, 0x142u, "core state vertex declaration FVF");
  checkEq(state.vertexShader, vertexShader.ref, "core state vertex shader ref");
  checkEq(state.pixelShader, pixelShader.ref, "core state pixel shader ref");

  DrawUniformPayload uniforms{};
  uniforms.vsConst = state.vsConst;
  uniforms.psConst = state.psConst;
  assertConstants(uniforms, "core state shader constants");
}

void assertRichDrawRunValues(const RecordedDrawRun& run,
                             const std::shared_ptr<Texture>& texture,
                             const std::shared_ptr<Buffer>& vertexBuffer,
                             const std::shared_ptr<Buffer>& indexBuffer,
                             const std::shared_ptr<Surface>& renderTarget,
                             const std::shared_ptr<Surface>& depthStencil,
                             const D9CVertexDecl& vertexDecl,
                             const D9CShader& vertexShader,
                             const D9CShader& pixelShader,
                             const Matrix4x4& textureTransform,
                             const ClipPlane& clipPlane) {
  checkEq(run.draws.size(), static_cast<std::size_t>(1),
          "rich draw run has one indexed draw");
  check(run.draws[0].indexed, "rich draw run indexed policy");
  check(run.draws[0].primitiveType == PrimitiveType::TriangleStrip,
        "rich draw run primitive type");
  checkEq(run.draws[0].primitiveCount, 3u,
          "rich draw run primitive count");
  checkEq(run.draws[0].baseVertexIndex, -7,
          "rich draw run base vertex");
  checkEq(run.draws[0].startIndex, 11u,
          "rich draw run start index");
  checkEq(run.draws[0].indexType, IndexType::UInt32,
          "rich draw run index type");
  check(run.draws[0].userVertexRange.empty(),
        "rich draw run uses stream buffer, not UP vertices");
  check(run.draws[0].userIndexRange.empty(),
        "rich draw run uses bound index buffer, not UP indices");

  checkEq(flatStateOr(run.hot.renderStates, RS_SCISSOR_TEST_ENABLE, 0u), 1u,
          "rich draw flat scissor render state");
  checkEq(flatStateOr(run.hot.renderStates, RS_TEXTURE_FACTOR, 0u),
          0xa0b0c0d0u, "rich draw flat texture factor");
  checkEq(flatStateOr(run.hot.textureStageStates[3], TSS_COLOR_OP, 0u), 4u,
          "rich draw flat TSS color op");
  checkEq(flatStateOr(run.hot.textureStageStates[3], TSS_ALPHA_ARG1, 0u), 0x20u,
          "rich draw flat TSS alpha arg");
  checkEq(flatStateOr(run.hot.samplerStates[5], SAMP_ADDRESS_U, 0u), 3u,
          "rich draw flat sampler address U");
  checkEq(flatStateOr(run.hot.samplerStates[6], SAMP_MIN_FILTER, 0u), 2u,
          "rich draw flat sampler min filter");
  check((run.hot.key.samplerStateMask & ((1u << 5u) | (1u << 6u))) ==
            ((1u << 5u) | (1u << 6u)),
        "rich draw sampler state mask includes imported samplers");

  checkEq(run.hot.textures[2].value, texture->handle().value,
          "rich draw texture handle");
  checkEq(run.hot.textureMask, 1u << 2u, "rich draw texture mask");
  checkEq(run.hot.streamBuffers[1].value, vertexBuffer->handle().value,
          "rich draw stream buffer");
  checkEq(run.hot.streamOffsets[1], 17u, "rich draw stream offset");
  checkEq(run.hot.streamStrides[1], 36u, "rich draw stream stride");
  checkEq(run.hot.streamMask, 1u << 1u, "rich draw stream mask");
  checkEq(run.hot.indexBuffer.value, indexBuffer->handle().value,
          "rich draw index buffer");

  checkEq(run.hot.colorAttachments[0].handle.value,
          renderTarget->handle().value, "rich draw RT handle");
  checkEq(run.hot.colorAttachments[0].level, 0u, "rich draw RT level");
  checkEq(run.hot.colorAttachments[0].sampleCount, 4u,
          "rich draw RT sample count");
  checkEq(run.hot.depthStencil.handle.value, depthStencil->handle().value,
          "rich draw DS handle");
  checkEq(run.hot.depthStencil.sampleCount, 2u,
          "rich draw DS sample count");
  checkEq(run.hot.renderTargetMask, 1u, "rich draw RT mask");

  checkEq(run.hot.viewport.viewport.x, 9u, "rich draw viewport x");
  checkEq(run.hot.viewport.viewport.y, 10u, "rich draw viewport y");
  checkEq(run.hot.viewport.viewport.width, 111u, "rich draw viewport width");
  checkEq(run.hot.viewport.viewport.height, 77u, "rich draw viewport height");
  checkNear(run.hot.viewport.viewport.minZ, 0.125f, "rich draw viewport minZ");
  checkNear(run.hot.viewport.viewport.maxZ, 0.875f, "rich draw viewport maxZ");
  checkEq(run.hot.viewport.scissor, Rect{2, 3, 54, 65},
          "rich draw scissor rect");
  check(run.hot.viewport.scissorEnabled, "rich draw scissor enabled");

  check(run.state.shaderLayout.vertexDecl.elements == vertexDecl.elements,
        "rich draw vertex declaration elements");
  checkEq(run.state.shaderLayout.vertexDecl.fvf, 0x142u,
          "rich draw vertex declaration inherited FVF");
  checkEq(run.state.shaderLayout.vertexDecl.streams[1].offset, 17u,
          "rich draw vertex declaration stream offset");
  checkEq(run.state.shaderLayout.vertexDecl.streams[1].stride, 36u,
          "rich draw vertex declaration stream stride");
  check(run.state.shaderLayout.vertexDecl.streams[1].buffer == vertexBuffer,
        "rich draw vertex declaration stream buffer");
  checkEq(run.state.shaderLayout.vertexShader, vertexShader.ref,
          "rich draw vertex shader ref");
  checkEq(run.state.shaderLayout.pixelShader, pixelShader.ref,
          "rich draw pixel shader ref");
  checkEq(run.state.shaderLayout.clipPlaneMask, 1u << 2u,
          "rich draw shader layout clip-plane mask");

  assertConstants(run.uniforms, "rich draw uniform constants");
  checkEq(run.uniforms.clipPlaneMask, 1u << 2u,
          "rich draw uniform clip-plane mask");
  checkClipPlane(run.uniforms.clipPlanes[2], clipPlane,
                 "rich draw transformed clip plane");
  checkMatrix(run.uniforms.worldViewProj, identityMatrix(),
              "rich draw uniform world-view-projection");
  checkMatrix(run.uniforms.textureTransforms[0], textureTransform,
              "rich draw uniform texture transform");
}

void testImportedApplyStateAndSetConstValuePropagation() {
  auto upper = std::make_unique<RecordingDxmt9Device>();
  auto* recorder = upper.get();
  auto* d3d = dxmt9::com::Direct3DCreate9Ex(dxmt9::com::D3D_SDK_VERSION,
                                            std::move(upper));
  check(d3d != nullptr, "create recording d3d factory");

  PresentParameters params{};
  params.backBufferWidth = 16u;
  params.backBufferHeight = 16u;
  params.backBufferFormat = Format::A8R8G8B8;
  params.windowed = true;
  params.deviceWindow = Handle{991};
  params.presentationInterval = PresentInterval::Immediate;

  auto* device = d3d->CreateDeviceEx(0u, params, nullptr);
  check(device != nullptr, "create recording d3d device");
  device->AddRef();

  {
    D9CDevice cDevice(device);

    auto texture = device->CreateTexture(TextureDesc{
        .width = 32u,
        .height = 16u,
        .depth = 1u,
        .levels = 1u,
        .format = Format::A8B8G8R8,
        .type = TextureType::TwoD,
        .pool = Pool::Default,
        .usage = UsageTexture,
    });
    auto vertexBuffer = device->CreateBuffer(BufferDesc{
        .size = 256u,
        .pool = Pool::Default,
        .usage = UsageVertexBuffer,
    });
    auto indexBuffer = device->CreateBuffer(BufferDesc{
        .size = 128u,
        .pool = Pool::Default,
        .usage = UsageIndexBuffer,
    });
    auto renderTarget = device->CreateSurface(SurfaceDesc{
        .width = 64u,
        .height = 48u,
        .format = Format::A8R8G8B8,
        .pool = Pool::Default,
        .usage = UsageRenderTarget,
        .renderTarget = true,
        .depthStencil = false,
        .multiSampleType = MultiSampleType::Four,
    });
    auto depthStencil = device->CreateSurface(SurfaceDesc{
        .width = 64u,
        .height = 48u,
        .format = Format::D24S8,
        .pool = Pool::Default,
        .usage = UsageDepthStencil,
        .renderTarget = false,
        .depthStencil = true,
        .multiSampleType = MultiSampleType::Two,
    });
    check(texture != nullptr, "test texture");
    check(vertexBuffer != nullptr, "test vertex buffer");
    check(indexBuffer != nullptr, "test index buffer");
    check(renderTarget != nullptr, "test render target");
    check(depthStencil != nullptr, "test depth stencil");

    D9CTexture textureWire(texture, &cDevice);
    D9CBuffer vertexBufferWire(vertexBuffer);
    D9CBuffer indexBufferWire(indexBuffer);
    indexBufferWire.desc.format = 102u;
    D9CSurface renderTargetWire(renderTarget);
    D9CSurface depthStencilWire(depthStencil);

    D9CVertexDecl oldDecl;
    oldDecl.elements = {
        VertexElement{0, 0, 2, 0, 0, 0},
    };
    D9CVertexDecl vertexDecl;
    vertexDecl.elements = {
        VertexElement{1, 0, 2, 0, 0, 0},
        VertexElement{1, 12, 4, 0, 10, 0},
        VertexElement{1, 28, 3, 0, 5, 0},
    };

    D9CShader vertexShader;
    vertexShader.ref = makeBytecodeShaderRef(
        0x1111222233334444ull, {0x01, 0x02, 0x03, 0x04});
    D9CShader pixelShader;
    pixelShader.ref = makeBytecodeShaderRef(
        0x5555666677778888ull, {0xf0, 0xe1, 0xd2, 0xc3, 0xb4});

    auto seedDecl = makeApplyStateRecord();
    seedDecl.packet.fvfValid = 1u;
    seedDecl.packet.fvf = 0x002u;
    seedDecl.packet.vdeclValid = 1u;
    seedDecl.packet.vdeclHandle = wireHandleFromPtr(&oldDecl);

    auto fvfClear = makeApplyStateRecord();
    fvfClear.packet.fvfValid = 1u;
    fvfClear.packet.fvf = 0x142u;

    const auto fvfDraw = makeDrawRecord(0u, 1u);

    const Matrix4x4 textureTransform = taggedMatrix(20.0f);
    const ClipPlane clipPlane{0.25f, -0.50f, 0.75f, -1.00f};

    auto richApply = makeApplyStateRecord();
    richApply.packet.renderStateCount = 3u;
    richApply.packet.renderStates[0] = {RS_SCISSOR_TEST_ENABLE, 1u};
    richApply.packet.renderStates[1] = {RS_CLIP_PLANE_ENABLE, 1u << 2u};
    richApply.packet.renderStates[2] = {RS_TEXTURE_FACTOR, 0xa0b0c0d0u};
    richApply.packet.textureMask = 1u << 2u;
    richApply.packet.textures[2] = wireHandleFromPtr(&textureWire);
    richApply.packet.streamSourceMask = 1u << 1u;
    richApply.packet.streamSources[1].buffer = wireHandleFromPtr(&vertexBufferWire);
    richApply.packet.streamSources[1].offset = 17u;
    richApply.packet.streamSources[1].stride = 36u;
    richApply.packet.vsValid = 1u;
    richApply.packet.vsHandle = wireHandleFromPtr(&vertexShader);
    richApply.packet.psValid = 1u;
    richApply.packet.psHandle = wireHandleFromPtr(&pixelShader);
    richApply.packet.vdeclValid = 1u;
    richApply.packet.vdeclHandle = wireHandleFromPtr(&vertexDecl);
    richApply.packet.rtMask = 1u;
    richApply.packet.rtHandles[0] = wireHandleFromPtr(&renderTargetWire);
    richApply.packet.dsValid = 1u;
    richApply.packet.dsHandle = wireHandleFromPtr(&depthStencilWire);
    richApply.packet.viewportValid = 1u;
    richApply.packet.viewport = D9CViewport{9u, 10u, 111u, 77u, 0.125f, 0.875f};
    richApply.packet.scissorValid = 1u;
    richApply.packet.scissor = D9CRect{2, 3, 54, 65};
    richApply.packet.tssCount = 2u;
    richApply.packet.tss[0] = {3u, TSS_COLOR_OP, 4u};
    richApply.packet.tss[1] = {3u, TSS_ALPHA_ARG1, 0x20u};
    richApply.packet.samplerStateCount = 2u;
    richApply.packet.samplerStates[0] = {5u, SAMP_ADDRESS_U, 3u};
    richApply.packet.samplerStates[1] = {6u, SAMP_MIN_FILTER, 2u};
    richApply.packet.materialValid = 1u;
    richApply.packet.material = makeMaterialRecord();
    richApply.packet.clipPlaneMask = 1u << 2u;
    for (std::size_t i = 0; i < clipPlane.size(); ++i) {
      richApply.packet.clipPlanes[2u * 4u + i] = clipPlane[i];
    }
    richApply.packet.transformCount = 4u;
    richApply.packet.transforms[0].state = 256u;
    richApply.packet.transforms[0].matrix = matrixToC(identityMatrix());
    richApply.packet.transforms[1].state = 2u;
    richApply.packet.transforms[1].matrix = matrixToC(identityMatrix());
    richApply.packet.transforms[2].state = 3u;
    richApply.packet.transforms[2].matrix = matrixToC(identityMatrix());
    richApply.packet.transforms[3].state = 16u;
    richApply.packet.transforms[3].matrix = matrixToC(textureTransform);
    richApply.packet.lightSlotMask = 1u << 4u;
    richApply.packet.lights[4] = makeLightRecord();
    richApply.packet.lightEnableValidMask = 1u << 4u;
    richApply.packet.lightEnableMask = 1u << 4u;

    const std::array<float, 8> vsConstF{
        1.25f, 2.25f, 3.25f, 4.25f,
        5.25f, 6.25f, 7.25f, 8.25f,
    };
    const std::array<float, 4> psConstF{9.5f, 10.5f, 11.5f, 12.5f};
    const std::array<std::int32_t, 8> vsConstI{-1, -2, -3, -4, 5, 6, 7, 8};
    const std::array<std::int32_t, 4> psConstI{9, 10, 11, 12};
    const std::array<std::uint32_t, 3> vsConstB{1u, 0u, 7u};
    const std::array<std::uint32_t, 2> psConstB{0u, 42u};

    const auto indexedDraw = makeIndexedDrawRecord(&indexBufferWire);

    std::vector<std::uint8_t> payload;
    std::vector<D9CCommandChunkWireRecordHeader> records;
    auto appendFixedRecord = [&](std::uint32_t type, const auto& record,
                                 std::uint32_t firstHandle = 0u,
                                 std::uint32_t handleCount = 0u) {
      const auto offset = appendRecord(payload, record);
      records.push_back(wireRecordHeader(type, offset, sizeof(record),
                                         firstHandle, handleCount));
    };
    auto appendConst = [&](std::uint32_t type, std::uint32_t start,
                           std::uint32_t count, const auto& data) {
      const auto offset = appendConstRecord(
          payload, type, start, count,
          std::span<const typename std::decay_t<decltype(data)>::value_type>(
              data.data(), data.size()));
      records.push_back(wireRecordHeader(
          type, offset,
          static_cast<std::uint32_t>(sizeof(D9CCommandRecordSetConst) +
                                     data.size() * sizeof(data[0]))));
    };

    appendFixedRecord(D9C_COMMAND_RECORD_APPLY_STATE, seedDecl);
    appendFixedRecord(D9C_COMMAND_RECORD_APPLY_STATE, fvfClear);
    appendFixedRecord(D9C_COMMAND_RECORD_DRAW_PRIMITIVE, fvfDraw);
    appendConst(D9C_COMMAND_RECORD_SET_VS_CONST_F, 3u, 2u, vsConstF);
    appendConst(D9C_COMMAND_RECORD_SET_PS_CONST_F, 5u, 1u, psConstF);
    appendConst(D9C_COMMAND_RECORD_SET_VS_CONST_I, 2u, 2u, vsConstI);
    appendConst(D9C_COMMAND_RECORD_SET_PS_CONST_I, 4u, 1u, psConstI);
    appendConst(D9C_COMMAND_RECORD_SET_VS_CONST_B, 1u, 3u, vsConstB);
    appendConst(D9C_COMMAND_RECORD_SET_PS_CONST_B, 2u, 2u, psConstB);
    appendFixedRecord(D9C_COMMAND_RECORD_APPLY_STATE, richApply, 0u, 7u);
    appendFixedRecord(D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE, indexedDraw,
                      7u, 1u);

    const std::vector<D9CCommandChunkWireHandleEntry> handles{
        wireHandleEntry(D9C_CHUNK_HANDLE_KIND_SURFACE, &renderTargetWire),
        wireHandleEntry(D9C_CHUNK_HANDLE_KIND_SURFACE, &depthStencilWire),
        wireHandleEntry(D9C_CHUNK_HANDLE_KIND_TEXTURE, &textureWire),
        wireHandleEntry(D9C_CHUNK_HANDLE_KIND_BUFFER, &vertexBufferWire),
        wireHandleEntry(D9C_CHUNK_HANDLE_KIND_SHADER, &vertexShader),
        wireHandleEntry(D9C_CHUNK_HANDLE_KIND_SHADER, &pixelShader),
        wireHandleEntry(D9C_CHUNK_HANDLE_KIND_VERTEX_DECL, &vertexDecl),
        wireHandleEntry(D9C_CHUNK_HANDLE_KIND_BUFFER, &indexBufferWire),
    };

    const auto wireBlob = makeWireChunkBlob(payload, records, handles);
    recorder->events.clear();
    checkEq(commitWireChunk(cDevice, wireBlob,
                            static_cast<std::uint32_t>(records.size()),
                            static_cast<std::uint32_t>(handles.size())),
            D3D_OK, "commit imported value chunk");

    checkEq(recorder->events.size(), static_cast<std::size_t>(5),
            "imported value event count");
    checkEventKind(recorder->events, 0u, EventKind::MarkChunkResources,
                   "imported value bulk retention event");
    checkEventKind(recorder->events, 1u, EventKind::SetSkipDrawResourceMarking,
                   "imported value skip enabled event");
    check(recorder->events[1].skipDrawResourceMarking,
          "imported value skip enabled");
    checkEventKind(recorder->events, 2u, EventKind::SubmitDraw,
                   "FVF clearing draw event");
    checkEventKind(recorder->events, 3u, EventKind::SubmitDraw,
                   "rich indexed draw event");
    checkEventKind(recorder->events, 4u, EventKind::SetSkipDrawResourceMarking,
                   "imported value skip disabled event");
    check(!recorder->events[4].skipDrawResourceMarking,
          "imported value skip disabled");

    const auto& retained = recorder->events[0].chunkHandles;
    checkEq(retained.size(), static_cast<std::size_t>(5),
            "bulk retention resolves only pool-backed imported handles");
    check(containsChunkHandle(retained, ChunkHandleKind::Surface,
                              renderTarget->handle()),
          "retention includes render target");
    check(containsChunkHandle(retained, ChunkHandleKind::Surface,
                              depthStencil->handle()),
          "retention includes depth stencil");
    check(containsChunkHandle(retained, ChunkHandleKind::Texture,
                              texture->handle()),
          "retention includes texture");
    check(containsChunkHandle(retained, ChunkHandleKind::Buffer,
                              vertexBuffer->handle()),
          "retention includes vertex buffer");
    check(containsChunkHandle(retained, ChunkHandleKind::Buffer,
                              indexBuffer->handle()),
          "retention includes index buffer");

    const auto& fvfClearedRun = recorder->events[2].drawRun;
    checkEq(fvfClearedRun.draws.size(), static_cast<std::size_t>(1),
            "FVF clear draw count");
    check(!fvfClearedRun.draws[0].indexed, "FVF clear draw is non-indexed");
    checkEq(fvfClearedRun.state.shaderLayout.vertexDecl.fvf, 0x142u,
            "FVF clear draw keeps new FVF");
    check(fvfClearedRun.state.shaderLayout.vertexDecl.elements.empty(),
          "FVF clear draw cleared prior vertex declaration elements");
    checkEq(fvfClearedRun.hot.key.vertexElementCount, 0u,
            "FVF clear flat key has no vertex elements");
    checkEq(fvfClearedRun.hot.key.fvf, 0x142u,
            "FVF clear flat key keeps FVF");

    assertRichDrawRunValues(recorder->events[3].drawRun, texture, vertexBuffer,
                            indexBuffer, renderTarget, depthStencil, vertexDecl,
                            vertexShader, pixelShader, textureTransform,
                            clipPlane);
    assertDeviceStateValues(device->coreDevice().state(), texture, vertexBuffer,
                            indexBuffer, renderTarget, depthStencil, vertexDecl,
                            vertexShader, pixelShader, textureTransform,
                            clipPlane);
  }

  checkEq(device->Release(), 0u, "release recording d3d device");
  checkEq(d3d->Release(), 0u, "release recording d3d factory");
}

void testMalformedImportedRecordDoesNotMutateState() {
  auto upper = std::make_unique<RecordingDxmt9Device>();
  auto* recorder = upper.get();
  auto* d3d = dxmt9::com::Direct3DCreate9Ex(dxmt9::com::D3D_SDK_VERSION,
                                            std::move(upper));
  check(d3d != nullptr, "create malformed recording d3d factory");

  PresentParameters params{};
  params.backBufferWidth = 16u;
  params.backBufferHeight = 16u;
  params.backBufferFormat = Format::A8R8G8B8;
  params.windowed = true;
  params.deviceWindow = Handle{992};
  params.presentationInterval = PresentInterval::Immediate;

  auto* device = d3d->CreateDeviceEx(0u, params, nullptr);
  check(device != nullptr, "create malformed recording d3d device");
  device->AddRef();

  {
    D9CDevice cDevice(device);

    auto mutatingApply = makeApplyStateRecord();
    mutatingApply.packet.renderStateCount = 1u;
    mutatingApply.packet.renderStates[0] = {RS_TEXTURE_FACTOR, 0x11223344u};

    D9CCommandRecordSetConst truncatedConst{};
    truncatedConst.header.type = D9C_COMMAND_RECORD_SET_VS_CONST_F;
    truncatedConst.header.size =
        sizeof(D9CCommandRecordSetConst) + sizeof(float) * 4u;
    truncatedConst.start = 0u;
    truncatedConst.count = 1u;

    std::vector<std::uint8_t> payload;
    const auto applyOffset = appendRecord(payload, mutatingApply);
    const auto badOffset = appendRecord(payload, truncatedConst);
    const std::vector<D9CCommandChunkWireRecordHeader> records{
        wireRecordHeader(D9C_COMMAND_RECORD_APPLY_STATE, applyOffset,
                         sizeof(mutatingApply)),
        wireRecordHeader(D9C_COMMAND_RECORD_SET_VS_CONST_F, badOffset,
                         sizeof(truncatedConst)),
    };
    const auto wireBlob = makeWireChunkBlob(payload, records, {});

    recorder->events.clear();
    checkEq(commitWireChunk(cDevice, wireBlob,
                            static_cast<std::uint32_t>(records.size()), 0u),
            D3DERR_INVALIDCALL,
            "malformed imported const record rejects whole chunk");
    check(!device->coreDevice().state().renderStates.contains(RS_TEXTURE_FACTOR),
          "malformed chunk rejects before applying earlier records");
    check(recorder->events.empty(),
          "malformed chunk does not reach fake backend");
  }

  checkEq(device->Release(), 0u, "release malformed d3d device");
  checkEq(d3d->Release(), 0u, "release malformed d3d factory");
}

}  // namespace

int main() {
  try {
    testImportedApplyStateAndSetConstValuePropagation();
    testMalformedImportedRecordDoesNotMutateState();
  } catch (const TestFailure& e) {
    std::cerr << "imported_apply_state_value_spec failed: " << e.what()
              << '\n';
    return EXIT_FAILURE;
  } catch (const std::exception& e) {
    std::cerr << "imported_apply_state_value_spec unexpected failure: "
              << e.what() << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
