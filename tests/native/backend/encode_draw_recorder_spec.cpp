// encodeDraw recorder seam coverage.
//
// These tests drive encoders::encodeDraw itself, with Metal calls suppressed
// after recording, so the checks include UP-vs-bound source selection,
// DrawVolatile bytes, and command ordering at the draw-issue boundary.

#include "../../../src/dxmt9/dxmt9_draw_encoder.hpp"
#include "../../../src/dxmt9/dxmt9_draw_encoder_internal.hpp"
#include "../../../src/dxmt9/dxmt9_draw_state.hpp"
#include "../../../src/dxmt9/dxmt9_ffp_shaders.hpp"
#include "../../../src/dxmt9/dxmt9_pipeline_cache.hpp"
#include "../../../src/dxmt9/dxmt9_resource_pool.hpp"
#include "../../../src/dxmt9/dxmt9_ring_arena.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using u32 = std::uint32_t;
using dxmt9::core::IndexType;
using dxmt9::core::PrimitiveType;
using dxmt9::encoders::EncodeDrawRecorder;
using dxmt9::encoders::PreUploadedDrawData;
using dxmt9::encoders::TextureSamplerBindShadow;
using dxmt9::state::DrawVolatile;

constexpr u32 kD3DDeclTypeFloat2 = 1u;
constexpr u32 kD3DDeclTypeFloat3 = 2u;
constexpr u32 kD3DDeclMethodDefault = 0u;
constexpr u32 kD3DDeclUsagePosition = dxmt9::ffp::kD3DDeclUsagePosition;
constexpr u32 kD3DDeclUsageTexcoord = dxmt9::ffp::kD3DDeclUsageTexcoord;

enum class RecordedKind {
  SetRenderPipelineState,
  SetDepthStencilState,
  SetBlendColorAndStencilRef,
  SetViewport,
  SetScissorRect,
  SetRasterizerState,
  SetFragmentTexture,
  SetFragmentSamplerState,
  SetVertexTexture,
  SetVertexSamplerState,
  SetVertexBuffer,
  SetVertexBytes,
  SetFragmentBytes,
  DrawPrimitives,
  DrawIndexedPrimitives,
};

struct RecordedCommand {
  RecordedKind kind = RecordedKind::SetVertexBuffer;
  obj_handle_t bufferHandle = 0;
  std::uint64_t offset = 0;
  std::uint8_t index = 0;
  std::array<std::uint8_t, 128> bytes{};
  std::uint64_t length = 0;
  WMTPrimitiveType primitiveType = WMTPrimitiveTypePoint;
  WMTIndexType indexType = WMTIndexTypeUInt16;
  std::uint64_t count = 0;
  std::uint64_t start = 0;
  std::uint32_t instanceCount = 0;
  std::int32_t baseVertex = 0;
  std::uint32_t baseInstance = 0;
  WMTViewport viewport{};
  WMTScissorRect scissor{};
  WMTTriangleFillMode fillMode = WMTTriangleFillModeFill;
  WMTCullMode cullMode = WMTCullModeNone;
  WMTDepthClipMode depthClipMode = WMTDepthClipModeClip;
  WMTWinding winding = WMTWindingClockwise;
  std::array<float, 4> color{};
};

struct Capture {
  std::vector<RecordedCommand> commands;
};

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

[[noreturn]] void fail(std::string message) {
  throw TestFailure(std::move(message));
}

template <typename A, typename B>
void checkEq(const A& left, const B& right, std::string_view message) {
  if (!(left == right)) {
    std::ostringstream out;
    out << message << " (" << left << " vs " << right << ")";
    fail(out.str());
  }
}

void check(bool value, std::string_view message) {
  if (!value) {
    fail(std::string(message));
  }
}

void checkNear(float actual, float expected, float epsilon, std::string_view message) {
  const float delta = actual > expected ? actual - expected : expected - actual;
  if (delta > epsilon) {
    std::ostringstream out;
    out << message << " (" << actual << " vs " << expected << ")";
    fail(out.str());
  }
}

const RecordedCommand& commandAt(const Capture& capture,
                                 std::size_t index,
                                 std::string_view message) {
  if (index >= capture.commands.size()) {
    fail(std::string(message));
  }
  return capture.commands[index];
}

std::size_t countCommands(const Capture& capture, RecordedKind kind) {
  std::size_t count = 0;
  for (const auto& command : capture.commands) {
    if (command.kind == kind) {
      ++count;
    }
  }
  return count;
}

DrawVolatile volatileBytes(const RecordedCommand& command) {
  checkEq(command.length, static_cast<std::uint64_t>(sizeof(DrawVolatile)),
          "DrawVolatile byte length");
  DrawVolatile value{};
  std::memcpy(&value, command.bytes.data(), sizeof(value));
  return value;
}

void recordSetRenderPipelineState(void* userdata,
                                  WMT::RenderPipelineState pipeline) {
  auto* capture = static_cast<Capture*>(userdata);
  RecordedCommand command{};
  command.kind = RecordedKind::SetRenderPipelineState;
  command.bufferHandle = pipeline.handle;
  capture->commands.push_back(command);
}

void recordSetDepthStencilState(void* userdata,
                                WMT::DepthStencilState depthStencil,
                                std::uint8_t stencilRef) {
  auto* capture = static_cast<Capture*>(userdata);
  RecordedCommand command{};
  command.kind = RecordedKind::SetDepthStencilState;
  command.bufferHandle = depthStencil.handle;
  command.index = stencilRef;
  capture->commands.push_back(command);
}

void recordSetBlendColorAndStencilRef(void* userdata,
                                      float red,
                                      float green,
                                      float blue,
                                      float alpha,
                                      std::uint8_t stencilRef) {
  auto* capture = static_cast<Capture*>(userdata);
  RecordedCommand command{};
  command.kind = RecordedKind::SetBlendColorAndStencilRef;
  command.color = {red, green, blue, alpha};
  command.index = stencilRef;
  capture->commands.push_back(command);
}

void recordSetViewport(void* userdata, WMTViewport viewport) {
  auto* capture = static_cast<Capture*>(userdata);
  RecordedCommand command{};
  command.kind = RecordedKind::SetViewport;
  command.viewport = viewport;
  capture->commands.push_back(command);
}

void recordSetScissorRect(void* userdata, WMTScissorRect rect) {
  auto* capture = static_cast<Capture*>(userdata);
  RecordedCommand command{};
  command.kind = RecordedKind::SetScissorRect;
  command.scissor = rect;
  capture->commands.push_back(command);
}

void recordSetRasterizerState(void* userdata,
                              WMTTriangleFillMode fillMode,
                              WMTCullMode cullMode,
                              WMTDepthClipMode depthClipMode,
                              WMTWinding winding,
                              float depthBias,
                              float slopeScale,
                              float depthBiasClamp) {
  (void)depthBias;
  (void)slopeScale;
  (void)depthBiasClamp;
  auto* capture = static_cast<Capture*>(userdata);
  RecordedCommand command{};
  command.kind = RecordedKind::SetRasterizerState;
  command.fillMode = fillMode;
  command.cullMode = cullMode;
  command.depthClipMode = depthClipMode;
  command.winding = winding;
  capture->commands.push_back(command);
}

void recordSetFragmentTexture(void* userdata,
                              WMT::Texture texture,
                              std::uint8_t index) {
  auto* capture = static_cast<Capture*>(userdata);
  RecordedCommand command{};
  command.kind = RecordedKind::SetFragmentTexture;
  command.bufferHandle = texture.handle;
  command.index = index;
  capture->commands.push_back(command);
}

void recordSetFragmentSamplerState(void* userdata,
                                   WMT::SamplerState sampler,
                                   std::uint8_t index) {
  auto* capture = static_cast<Capture*>(userdata);
  RecordedCommand command{};
  command.kind = RecordedKind::SetFragmentSamplerState;
  command.bufferHandle = sampler.handle;
  command.index = index;
  capture->commands.push_back(command);
}

void recordSetVertexTexture(void* userdata,
                            WMT::Texture texture,
                            std::uint8_t index) {
  auto* capture = static_cast<Capture*>(userdata);
  RecordedCommand command{};
  command.kind = RecordedKind::SetVertexTexture;
  command.bufferHandle = texture.handle;
  command.index = index;
  capture->commands.push_back(command);
}

void recordSetVertexSamplerState(void* userdata,
                                 WMT::SamplerState sampler,
                                 std::uint8_t index) {
  auto* capture = static_cast<Capture*>(userdata);
  RecordedCommand command{};
  command.kind = RecordedKind::SetVertexSamplerState;
  command.bufferHandle = sampler.handle;
  command.index = index;
  capture->commands.push_back(command);
}

void recordSetVertexBuffer(void* userdata,
                           WMT::Buffer buffer,
                           std::uint64_t offset,
                           std::uint8_t index) {
  auto* capture = static_cast<Capture*>(userdata);
  RecordedCommand command{};
  command.kind = RecordedKind::SetVertexBuffer;
  command.bufferHandle = buffer.handle;
  command.offset = offset;
  command.index = index;
  capture->commands.push_back(command);
}

void recordSetVertexBytes(void* userdata,
                          const void* bytes,
                          std::uint64_t length,
                          std::uint8_t index) {
  auto* capture = static_cast<Capture*>(userdata);
  RecordedCommand command{};
  command.kind = RecordedKind::SetVertexBytes;
  command.length = length;
  command.index = index;
  check(length <= command.bytes.size(), "recorded bytes fit capture storage");
  std::memcpy(command.bytes.data(), bytes, static_cast<std::size_t>(length));
  capture->commands.push_back(command);
}

void recordSetFragmentBytes(void* userdata,
                            const void* bytes,
                            std::uint64_t length,
                            std::uint8_t index) {
  auto* capture = static_cast<Capture*>(userdata);
  RecordedCommand command{};
  command.kind = RecordedKind::SetFragmentBytes;
  command.length = length;
  command.index = index;
  check(length <= command.bytes.size(), "recorded bytes fit capture storage");
  std::memcpy(command.bytes.data(), bytes, static_cast<std::size_t>(length));
  capture->commands.push_back(command);
}

void recordDrawPrimitives(void* userdata,
                          WMTPrimitiveType primitiveType,
                          std::uint64_t vertexStart,
                          std::uint64_t vertexCount,
                          std::uint32_t instanceCount,
                          std::uint32_t baseInstance) {
  auto* capture = static_cast<Capture*>(userdata);
  RecordedCommand command{};
  command.kind = RecordedKind::DrawPrimitives;
  command.primitiveType = primitiveType;
  command.start = vertexStart;
  command.count = vertexCount;
  command.instanceCount = instanceCount;
  command.baseInstance = baseInstance;
  capture->commands.push_back(command);
}

void recordDrawIndexedPrimitives(void* userdata,
                                 WMTPrimitiveType primitiveType,
                                 WMTIndexType indexType,
                                 std::uint64_t indexCount,
                                 WMT::Buffer indexBuffer,
                                 std::uint64_t indexBufferOffset,
                                 std::uint32_t instanceCount,
                                 std::int32_t baseVertex,
                                 std::uint32_t baseInstance) {
  auto* capture = static_cast<Capture*>(userdata);
  RecordedCommand command{};
  command.kind = RecordedKind::DrawIndexedPrimitives;
  command.primitiveType = primitiveType;
  command.indexType = indexType;
  command.count = indexCount;
  command.bufferHandle = indexBuffer.handle;
  command.offset = indexBufferOffset;
  command.instanceCount = instanceCount;
  command.baseVertex = baseVertex;
  command.baseInstance = baseInstance;
  capture->commands.push_back(command);
}

EncodeDrawRecorder makeRecorder(Capture& capture) {
  return EncodeDrawRecorder{
      .userdata = &capture,
      .suppressMetalCalls = true,
      .setRenderPipelineState = recordSetRenderPipelineState,
      .setDepthStencilState = recordSetDepthStencilState,
      .setBlendColorAndStencilRef = recordSetBlendColorAndStencilRef,
      .setViewport = recordSetViewport,
      .setScissorRect = recordSetScissorRect,
      .setRasterizerState = recordSetRasterizerState,
      .setFragmentTexture = recordSetFragmentTexture,
      .setFragmentSamplerState = recordSetFragmentSamplerState,
      .setVertexTexture = recordSetVertexTexture,
      .setVertexSamplerState = recordSetVertexSamplerState,
      .setVertexBuffer = recordSetVertexBuffer,
      .setVertexBytes = recordSetVertexBytes,
      .drawPrimitives = recordDrawPrimitives,
      .drawIndexedPrimitives = recordDrawIndexedPrimitives,
  };
}

struct Harness {
  dxmt9::core::BackendLimits limits{};
  dxmt9::resources::Pool pool{};
  dxmt9::pipeline::Cache cache{};
  dxmt9::scratch::FrameAllocators allocators{};
  dxmt9::CommandQueue queue;
  std::vector<std::pair<dxmt9::core::BufferHandle, obj_handle_t>> patchedBuffers;
  std::vector<std::pair<dxmt9::core::TextureHandle, obj_handle_t>> patchedTextures;
  std::vector<std::pair<dxmt9::core::SurfaceHandle, obj_handle_t>> patchedSurfaces;

  Harness() : queue(WMT::Device{}, limits, false) {}

  ~Harness() {
    for (const auto& [handle, metalHandle] : patchedBuffers) {
      (void)metalHandle;
      if (auto* record = pool.findBuffer(handle.value)) {
        record->buffer.handle = 0;
      }
    }
    for (const auto& [handle, metalHandle] : patchedTextures) {
      (void)metalHandle;
      if (auto* record = pool.findTexture(handle.value)) {
        record->texture.handle = 0;
        record->shaderReadTexture.handle = 0;
      }
    }
    for (const auto& [handle, metalHandle] : patchedSurfaces) {
      (void)metalHandle;
      if (auto* record = pool.findSurface(handle.value)) {
        record->texture.handle = 0;
        record->resolveTexture.handle = 0;
      }
    }
  }

  void clearPatchedResourceHandles() {
    for (const auto& [handle, metalHandle] : patchedBuffers) {
      (void)metalHandle;
      if (auto* record = pool.findBuffer(handle.value)) {
        record->buffer.handle = 0;
      }
    }
    for (const auto& [handle, metalHandle] : patchedTextures) {
      (void)metalHandle;
      if (auto* record = pool.findTexture(handle.value)) {
        record->texture.handle = 0;
        record->shaderReadTexture.handle = 0;
      }
    }
    for (const auto& [handle, metalHandle] : patchedSurfaces) {
      (void)metalHandle;
      if (auto* record = pool.findSurface(handle.value)) {
        record->texture.handle = 0;
        record->resolveTexture.handle = 0;
      }
    }
  }

  void restorePatchedResourceHandles() {
    for (const auto& [handle, metalHandle] : patchedBuffers) {
      auto* record = pool.findBuffer(handle.value);
      check(record != nullptr, "patched buffer record remains live");
      record->buffer.handle = metalHandle;
    }
    for (const auto& [handle, metalHandle] : patchedTextures) {
      auto* record = pool.findTexture(handle.value);
      check(record != nullptr, "patched texture record remains live");
      record->texture.handle = metalHandle;
    }
    for (const auto& [handle, metalHandle] : patchedSurfaces) {
      auto* record = pool.findSurface(handle.value);
      check(record != nullptr, "patched surface record remains live");
      record->texture.handle = metalHandle;
    }
  }

  dxmt9::core::BufferHandle createBoundBuffer(obj_handle_t metalHandle,
                                              std::uint64_t size) {
    clearPatchedResourceHandles();

    dxmt9::core::BufferDesc desc{};
    desc.size = size;
    desc.pool = dxmt9::core::Pool::Default;
    auto handle = pool.createBuffer(WMT::Device{}, desc);
    auto* record = pool.findBuffer(handle.value);
    check(record != nullptr, "pool creates a bound-buffer record");
    patchedBuffers.push_back({handle, metalHandle});
    restorePatchedResourceHandles();
    return handle;
  }

  dxmt9::core::TextureHandle createBoundTexture(obj_handle_t metalHandle,
                                                std::uint32_t width,
                                                std::uint32_t height) {
    clearPatchedResourceHandles();

    dxmt9::core::TextureDesc desc{};
    desc.width = width;
    desc.height = height;
    desc.levels = 1u;
    desc.pool = dxmt9::core::Pool::Default;
    auto handle = pool.createTexture(WMT::Device{}, limits, desc);
    auto* record = pool.findTexture(handle.value);
    check(record != nullptr, "pool creates a bound-texture record");
    patchedTextures.push_back({handle, metalHandle});
    restorePatchedResourceHandles();
    return handle;
  }

  dxmt9::core::SurfaceHandle createRenderTargetSurface(obj_handle_t metalHandle,
                                                       std::uint32_t width,
                                                       std::uint32_t height) {
    clearPatchedResourceHandles();

    dxmt9::core::SurfaceDesc desc{};
    desc.width = width;
    desc.height = height;
    desc.pool = dxmt9::core::Pool::Default;
    desc.renderTarget = true;
    auto handle = pool.createSurface(WMT::Device{}, limits, desc);
    auto* record = pool.findSurface(handle.value);
    check(record != nullptr, "pool creates a render-target surface record");
    patchedSurfaces.push_back({handle, metalHandle});
    restorePatchedResourceHandles();
    return handle;
  }
};

dxmt9::core::CanonicalDrawState makeProgrammableState(u32 stride) {
  dxmt9::core::CanonicalDrawState state{};
  state.hot.streamOffsets[0] = 0;
  state.hot.streamStrides[0] = stride;
  state.shaderLayout.vertexDecl.streams[0].stride = stride;
  state.shaderLayout.vertexShader.kind = dxmt9::core::ShaderRef::Kind::Bytecode;
  state.shaderLayout.pixelShader.kind = dxmt9::core::ShaderRef::Kind::Bytecode;
  return state;
}

void setFvfXyzTex1(dxmt9::core::CanonicalDrawState& state) {
  state.shaderLayout.vertexDecl.fvf =
      dxmt9::ffp::kFvfXyz | (1u << dxmt9::ffp::kFvfTexCountShift);
}

void setDeclPositionTexcoord(dxmt9::core::CanonicalDrawState& state,
                             u32 texcoordOffset,
                             u32 stride) {
  state.shaderLayout.vertexDecl.fvf = 0u;
  state.shaderLayout.vertexDecl.streams[0].stride = stride;
  state.hot.streamStrides[0] = stride;
  state.shaderLayout.vertexDecl.elements = {
      dxmt9::core::VertexElement{0, 0, kD3DDeclTypeFloat3,
                                 kD3DDeclMethodDefault,
                                 kD3DDeclUsagePosition, 0},
      dxmt9::core::VertexElement{0, static_cast<std::uint16_t>(texcoordOffset),
                                 kD3DDeclTypeFloat2,
                                 kD3DDeclMethodDefault,
                                 kD3DDeclUsageTexcoord, 0},
  };
}

void setInvDestColorAddBlend(dxmt9::core::CanonicalDrawState& state) {
  state.hot.renderStates.entries[0] = dxmt9::core::FlatStateEntry{
      dxmt9::core::RS_SRC_BLEND,
      static_cast<u32>(dxmt9::core::BlendFactor::InvDestColor)};
  state.hot.renderStates.entries[1] = dxmt9::core::FlatStateEntry{
      dxmt9::core::RS_DEST_BLEND,
      static_cast<u32>(dxmt9::core::BlendFactor::One)};
  state.hot.renderStates.entries[2] = dxmt9::core::FlatStateEntry{
      dxmt9::core::RS_ALPHABLEND_ENABLE,
      1u};
  state.hot.renderStates.count = 3u;
}

void setSortedRenderStates(
    dxmt9::core::CanonicalDrawState& state,
    std::initializer_list<dxmt9::core::FlatStateEntry> entries) {
  state.hot.renderStates = {};
  for (const auto& entry : entries) {
    if (state.hot.renderStates.count >=
        state.hot.renderStates.entries.size()) {
      break;
    }
    state.hot.renderStates.entries[state.hot.renderStates.count++] = entry;
  }
  std::sort(
      state.hot.renderStates.entries.begin(),
      state.hot.renderStates.entries.begin() + state.hot.renderStates.count,
      [](const dxmt9::core::FlatStateEntry& a,
         const dxmt9::core::FlatStateEntry& b) {
        return a.state < b.state;
      });
}

void setOpaqueDepthRenderStates(dxmt9::core::CanonicalDrawState& state) {
  setSortedRenderStates(
      state,
      {
          dxmt9::core::FlatStateEntry{
              dxmt9::core::RS_Z_ENABLE,
              1u},
          dxmt9::core::FlatStateEntry{
              dxmt9::core::RS_Z_WRITE_ENABLE,
              1u},
          dxmt9::core::FlatStateEntry{
              dxmt9::core::RS_Z_FUNC,
              static_cast<u32>(dxmt9::core::CompareFunc::LessEqual)},
      });
}

dxmt9::encoders::EncodeContext makeContext(Harness& harness,
                                           EncodeDrawRecorder& recorder) {
  return dxmt9::encoders::EncodeContext{
      WMT::Reference<WMT::Device>{},
      harness.limits,
      harness.pool,
      harness.cache,
      harness.allocators,
      nullptr,
      nullptr,
      harness.queue,
      dxmt9::uniform::DirtyState{},
      &recorder,
  };
}

void assertIndexedDrawCommand(const RecordedCommand& command,
                              obj_handle_t indexBuffer,
                              std::uint64_t indexOffset,
                              std::uint64_t indexCount,
                              WMTPrimitiveType primitiveType =
                                  WMTPrimitiveTypeTriangle,
                              WMTIndexType indexType = WMTIndexTypeUInt16) {
  check(command.kind == RecordedKind::DrawIndexedPrimitives,
        "third command is drawIndexedPrimitives");
  checkEq(static_cast<unsigned>(command.primitiveType),
          static_cast<unsigned>(primitiveType),
          "draw primitive type");
  checkEq(static_cast<unsigned>(command.indexType),
          static_cast<unsigned>(indexType),
          "draw index type");
  checkEq(command.count, indexCount, "draw index count");
  checkEq(command.bufferHandle, indexBuffer, "draw index buffer source");
  checkEq(command.offset, indexOffset, "draw index buffer offset");
  checkEq(command.instanceCount, std::uint32_t{1}, "draw instance count");
  checkEq(command.baseVertex, std::int32_t{0}, "Metal draw base vertex");
  checkEq(command.baseInstance, std::uint32_t{0}, "Metal draw base instance");
}

void assertDrawPrimitivesCommand(const RecordedCommand& command,
                                 WMTPrimitiveType primitiveType,
                                 std::uint64_t vertexStart,
                                 std::uint64_t vertexCount) {
  check(command.kind == RecordedKind::DrawPrimitives,
        "third command is drawPrimitives");
  checkEq(static_cast<unsigned>(command.primitiveType),
          static_cast<unsigned>(primitiveType),
          "drawPrimitives primitive type");
  checkEq(command.start, vertexStart, "drawPrimitives vertex start");
  checkEq(command.count, vertexCount, "drawPrimitives vertex count");
  checkEq(command.instanceCount, std::uint32_t{1},
          "drawPrimitives instance count");
  checkEq(command.baseInstance, std::uint32_t{0},
          "drawPrimitives base instance");
}

const RecordedCommand& firstVertexBufferBind(Capture& capture,
                                             std::uint8_t slot,
                                             std::string_view message) {
  for (const auto& command : capture.commands) {
    if (command.kind == RecordedKind::SetVertexBuffer &&
        command.index == slot) {
      return command;
    }
  }
  fail(std::string(message));
}

const RecordedCommand& firstIndexedDraw(Capture& capture,
                                        std::string_view message) {
  for (const auto& command : capture.commands) {
    if (command.kind == RecordedKind::DrawIndexedPrimitives) {
      return command;
    }
  }
  fail(std::string(message));
}

void runEncodeDraw(Harness& harness,
                   EncodeDrawRecorder& recorder,
                   dxmt9::core::CanonicalDrawState& state,
                   const dxmt9::core::DrawParam& param,
                   const PreUploadedDrawData& preUploaded,
	                   std::span<const std::uint8_t> arena,
	                   bool skipBaseStateBind = true,
	                   bool argbufHybridMode = false,
	                   TextureSamplerBindShadow* textureSamplerShadow = nullptr,
	                   const dxmt9::core::DrawBindingSnapshot* bindingSnapshot = nullptr,
	                   const dxmt9::core::DrawBindingOverride* paramBindingOverride = nullptr) {
  auto ctx = makeContext(harness, recorder);
  WMT::CommandBuffer commandBuffer{};
  WMT::RenderCommandEncoder encoder{};
  encoder.handle = static_cast<obj_handle_t>(0xE005u);
  dxmt9::uniform::DirtyState cleanDirty{};

  const bool encoded = dxmt9::encoders::encodeDraw(
      ctx,
      commandBuffer,
      encoder,
      state.view(),
      55u,
      skipBaseStateBind,
      &preUploaded,
      &param,
      arena,
      &cleanDirty,
      /*tileFfpMode=*/false,
      argbufHybridMode,
	      /*argbufResourceArray=*/false,
	      /*argbufDirectCbufMode=*/false,
	      /*reopenArgbufHybrid=*/true,
	      textureSamplerShadow,
	      std::numeric_limits<std::uint32_t>::max(),
	      bindingSnapshot,
	      paramBindingOverride);

  check(encoded, "encodeDraw emits a draw");
}

void testBaseStateRecorderCapturesRasterTextureSamplerOrdering() {
  Harness harness;
  Capture capture;
  auto recorder = makeRecorder(capture);

  constexpr obj_handle_t kPipeline = 0x700000700000701ull;
  constexpr obj_handle_t kDepthState = 0x700000700000702ull;
  constexpr obj_handle_t kSampler = 0x700000700000703ull;
  constexpr obj_handle_t kRenderTarget = 0x700000700000704ull;
  constexpr obj_handle_t kTexture0 = 0x700000700000705ull;
  constexpr obj_handle_t kTexture1 = 0x700000700000706ull;
  constexpr obj_handle_t kBoundVertex = 0x700000700000707ull;

  recorder.suppressBaseStateLookup = true;
  recorder.renderPipelineState.handle = kPipeline;
  recorder.depthStencilState.handle = kDepthState;
  recorder.fragmentSamplerState.handle = kSampler;

  auto state = makeProgrammableState(20u);
  state.hot.colorAttachments[0].handle =
      harness.createRenderTargetSurface(kRenderTarget, 640u, 480u);
  state.hot.viewport.viewport = dxmt9::core::Viewport{10u, 20u, 320u, 240u,
                                                      0.25f, 0.75f};
  state.hot.viewport.scissorEnabled = true;
  state.hot.viewport.scissor = dxmt9::core::Rect{11, 22, 111, 222};
  // FlatStateSet invariant: entries[0..count) sorted ascending by state.
  // RS_FILL_MODE (8) < RS_CULL_MODE (22).
  state.hot.renderStates.entries[0] = dxmt9::core::FlatStateEntry{
      dxmt9::core::RS_FILL_MODE,
      2u};
  state.hot.renderStates.entries[1] = dxmt9::core::FlatStateEntry{
      dxmt9::core::RS_CULL_MODE,
      static_cast<u32>(dxmt9::core::CullMode::Ccw)};
  state.hot.renderStates.count = 2u;
  state.hot.textures[0] = harness.createBoundTexture(kTexture0, 64u, 64u);
  state.hot.textures[1] = harness.createBoundTexture(kTexture1, 32u, 32u);
  state.hot.textureMask = 0x3u;
  state.hot.streamBuffers[0] = harness.createBoundBuffer(kBoundVertex, 4096u);
  state.hot.streamOffsets[0] = 48u;

  dxmt9::core::DrawParam param{};
  param.primitiveType = PrimitiveType::TriangleList;
  param.primitiveCount = 1u;
  param.indexed = false;

  PreUploadedDrawData preUploaded{};
  runEncodeDraw(harness, recorder, state, param, preUploaded, {},
                /*skipBaseStateBind=*/false);

  checkEq(capture.commands.size(), std::size_t{12},
          "base-state draw command count");

  const auto& depth = commandAt(capture, 0, "missing depth-state bind");
  check(depth.kind == RecordedKind::SetDepthStencilState,
        "first base-state command binds depth state");
  checkEq(depth.bufferHandle, kDepthState, "depth-state handle");
  checkEq(static_cast<unsigned>(depth.index), 0u, "depth-state stencil ref");

  const auto& pipeline = commandAt(capture, 1, "missing pipeline bind");
  check(pipeline.kind == RecordedKind::SetRenderPipelineState,
        "second base-state command binds pipeline");
  checkEq(pipeline.bufferHandle, kPipeline, "pipeline handle");

  const auto& viewport = commandAt(capture, 2, "missing viewport bind");
  check(viewport.kind == RecordedKind::SetViewport,
        "third base-state command sets viewport");
  checkEq(viewport.viewport.originX, 10.0, "viewport origin x");
  checkEq(viewport.viewport.originY, 20.0, "viewport origin y");
  checkEq(viewport.viewport.width, 320.0, "viewport width");
  checkEq(viewport.viewport.height, 240.0, "viewport height");
  checkEq(viewport.viewport.znear, 0.25, "viewport min z");
  checkEq(viewport.viewport.zfar, 0.75, "viewport max z");

  const auto& scissor = commandAt(capture, 3, "missing scissor bind");
  check(scissor.kind == RecordedKind::SetScissorRect,
        "fourth base-state command sets scissor");
  checkEq(scissor.scissor.x, std::uint64_t{11}, "scissor x");
  checkEq(scissor.scissor.y, std::uint64_t{22}, "scissor y");
  checkEq(scissor.scissor.width, std::uint64_t{100}, "scissor width");
  checkEq(scissor.scissor.height, std::uint64_t{200}, "scissor height");

  const auto& raster = commandAt(capture, 4, "missing rasterizer bind");
  check(raster.kind == RecordedKind::SetRasterizerState,
        "fifth base-state command sets rasterizer state");
  checkEq(static_cast<unsigned>(raster.fillMode),
          static_cast<unsigned>(WMTTriangleFillModeLines),
          "raster fill mode");
  checkEq(static_cast<unsigned>(raster.cullMode),
          static_cast<unsigned>(WMTCullModeBack),
          "raster cull mode");
  checkEq(static_cast<unsigned>(raster.depthClipMode),
          static_cast<unsigned>(WMTDepthClipModeClip),
          "raster depth clip mode");
  checkEq(static_cast<unsigned>(raster.winding),
          static_cast<unsigned>(WMTWindingClockwise),
          "raster front-face winding");

  const auto& stream = commandAt(capture, 5, "missing base-state stream bind");
  check(stream.kind == RecordedKind::SetVertexBuffer,
        "stream bind follows base raster state");
  checkEq(stream.bufferHandle, kBoundVertex, "base-state stream buffer");
  checkEq(stream.offset, std::uint64_t{48}, "base-state stream offset");
  checkEq(static_cast<unsigned>(stream.index), 1u, "base-state stream slot");

  const auto& texture0 = commandAt(capture, 6, "missing texture0 bind");
  check(texture0.kind == RecordedKind::SetFragmentTexture,
        "fragment texture0 binds before DrawVolatile");
  checkEq(texture0.bufferHandle, kTexture0, "fragment texture0 handle");
  checkEq(static_cast<unsigned>(texture0.index), 0u, "fragment texture0 stage");

  const auto& sampler0 = commandAt(capture, 7, "missing sampler0 bind");
  check(sampler0.kind == RecordedKind::SetFragmentSamplerState,
        "fragment sampler0 binds before DrawVolatile");
  checkEq(sampler0.bufferHandle, kSampler, "fragment sampler0 handle");
  checkEq(static_cast<unsigned>(sampler0.index), 0u, "fragment sampler0 stage");

  const auto& texture1 = commandAt(capture, 8, "missing texture1 bind");
  check(texture1.kind == RecordedKind::SetFragmentTexture,
        "fragment texture1 binds before DrawVolatile");
  checkEq(texture1.bufferHandle, kTexture1, "fragment texture1 handle");
  checkEq(static_cast<unsigned>(texture1.index), 1u, "fragment texture1 stage");

  const auto& sampler1 = commandAt(capture, 9, "missing sampler1 bind");
  check(sampler1.kind == RecordedKind::SetFragmentSamplerState,
        "fragment sampler1 binds before DrawVolatile");
  checkEq(sampler1.bufferHandle, kSampler, "fragment sampler1 handle");
  checkEq(static_cast<unsigned>(sampler1.index), 1u, "fragment sampler1 stage");

  const auto& volCommand =
      commandAt(capture, 10, "missing base-state DrawVolatile");
  check(volCommand.kind == RecordedKind::SetVertexBytes,
        "DrawVolatile follows base-state commands");
  const auto vol = volatileBytes(volCommand);
  checkEq(vol.vertexBaseIndex, std::int32_t{0},
          "base-state DrawVolatile base vertex");
  checkEq(vol.vertexStreamStride, std::uint32_t{20},
          "base-state DrawVolatile stride");

  assertDrawPrimitivesCommand(
      commandAt(capture, 11, "missing base-state draw"),
      WMTPrimitiveTypeTriangle,
      0u,
      3u);

  Capture clampCapture;
  auto clampRecorder = makeRecorder(clampCapture);
  clampRecorder.suppressBaseStateLookup = true;
  clampRecorder.renderPipelineState.handle = kPipeline;
  clampRecorder.depthStencilState.handle = kDepthState;
  clampRecorder.fragmentSamplerState.handle = kSampler;
  state.hot.renderStates.entries[2] = dxmt9::core::FlatStateEntry{
      dxmt9::core::RS_CLIPPING,
      0u};
  state.hot.renderStates.count = 3u;
  runEncodeDraw(harness, clampRecorder, state, param, preUploaded, {},
                /*skipBaseStateBind=*/false);
  const auto& clampRaster =
      commandAt(clampCapture, 4, "missing unclipped rasterizer bind");
  checkEq(static_cast<unsigned>(clampRaster.depthClipMode),
          static_cast<unsigned>(WMTDepthClipModeClamp),
          "D3DRS_CLIPPING=false maps to Metal depth clamp");
}

void testBlendFactorBindsMetalBlendColor() {
  Harness harness;
  Capture capture;
  auto recorder = makeRecorder(capture);

  constexpr obj_handle_t kPipeline = 0x710000710000701ull;
  constexpr obj_handle_t kDepthState = 0x710000710000702ull;
  constexpr obj_handle_t kBoundVertex = 0x710000710000703ull;

  recorder.suppressBaseStateLookup = true;
  recorder.renderPipelineState.handle = kPipeline;
  recorder.depthStencilState.handle = kDepthState;

  auto state = makeProgrammableState(20u);
  state.hot.streamBuffers[0] = harness.createBoundBuffer(kBoundVertex, 256u);
  state.hot.renderStates.entries[0] = dxmt9::core::FlatStateEntry{
      dxmt9::core::RS_SRC_BLEND,
      static_cast<u32>(dxmt9::core::BlendFactor::BlendFactor)};
  state.hot.renderStates.entries[1] = dxmt9::core::FlatStateEntry{
      dxmt9::core::RS_DEST_BLEND,
      static_cast<u32>(dxmt9::core::BlendFactor::InvBlendFactor)};
  state.hot.renderStates.entries[2] = dxmt9::core::FlatStateEntry{
      dxmt9::core::RS_ALPHABLEND_ENABLE,
      1u};
  state.hot.renderStates.entries[3] = dxmt9::core::FlatStateEntry{
      dxmt9::core::RS_STENCIL_REF,
      0x7au};
  state.hot.renderStates.entries[4] = dxmt9::core::FlatStateEntry{
      dxmt9::core::RS_BLEND_FACTOR,
      0x80402010u};
  state.hot.renderStates.count = 5u;

  dxmt9::core::DrawParam param{};
  param.primitiveType = PrimitiveType::TriangleList;
  param.primitiveCount = 1u;
  param.indexed = false;

  PreUploadedDrawData preUploaded{};
  runEncodeDraw(harness, recorder, state, param, preUploaded, {},
                /*skipBaseStateBind=*/false);

  const auto& blend = commandAt(capture, 1, "missing blend factor bind");
  check(blend.kind == RecordedKind::SetBlendColorAndStencilRef,
        "D3DRS_BLENDFACTOR binds Metal blend color");
  checkNear(blend.color[0], 64.0f / 255.0f, 0.0001f, "blend red");
  checkNear(blend.color[1], 32.0f / 255.0f, 0.0001f, "blend green");
  checkNear(blend.color[2], 16.0f / 255.0f, 0.0001f, "blend blue");
  checkNear(blend.color[3], 128.0f / 255.0f, 0.0001f, "blend alpha");
  checkEq(static_cast<unsigned>(blend.index), 0x7au,
          "blend bind preserves current stencil ref");
}

void testArgbufModeKeepsDirectTextureSamplerBinds() {
  // R-BACK-12.22..12.26 — Stage 2 is constants-only. Texture and sampler
  // resources continue to travel on the direct render-encoder
  // setFragmentTexture / setFragmentSamplerState lane (the validated
  // Stage 1 path) even when argbuf hybrid mode is active.
  Harness harness;
  Capture capture;
  auto recorder = makeRecorder(capture);

  constexpr obj_handle_t kPipeline = 0x700000700000731ull;
  constexpr obj_handle_t kDepthState = 0x700000700000732ull;
  constexpr obj_handle_t kSampler = 0x700000700000733ull;
  constexpr obj_handle_t kRenderTarget = 0x700000700000734ull;
  constexpr obj_handle_t kTexture0 = 0x700000700000735ull;
  constexpr obj_handle_t kBoundVertex = 0x700000700000736ull;

  recorder.suppressBaseStateLookup = true;
  recorder.renderPipelineState.handle = kPipeline;
  recorder.depthStencilState.handle = kDepthState;
  recorder.fragmentSamplerState.handle = kSampler;

  auto state = makeProgrammableState(20u);
  state.hot.colorAttachments[0].handle =
      harness.createRenderTargetSurface(kRenderTarget, 640u, 480u);
  state.hot.textures[0] = harness.createBoundTexture(kTexture0, 64u, 64u);
  state.hot.textureMask = 0x1u;
  state.hot.streamBuffers[0] = harness.createBoundBuffer(kBoundVertex, 4096u);

  dxmt9::core::DrawParam param{};
  param.primitiveType = PrimitiveType::TriangleList;
  param.primitiveCount = 1u;
  param.indexed = false;

  PreUploadedDrawData preUploaded{};
  runEncodeDraw(harness, recorder, state, param, preUploaded, {},
                /*skipBaseStateBind=*/false,
                /*argbufHybridMode=*/true);

  bool sawFragmentTexture = false;
  bool sawFragmentSampler = false;
  for (const auto& command : capture.commands) {
    if (command.kind == RecordedKind::SetFragmentTexture) {
      sawFragmentTexture = true;
    } else if (command.kind == RecordedKind::SetFragmentSamplerState) {
      sawFragmentSampler = true;
    }
  }
  check(sawFragmentTexture,
        "Stage 2 argbuf mode keeps the direct setFragmentTexture bind");
  check(sawFragmentSampler,
        "Stage 2 argbuf mode keeps the direct setFragmentSamplerState bind");
}

void testArgbufModeKeepsDirectVertexTextureSamplerBinds() {
  // Vertex texture fetch uses the same direct Metal resource lane as
  // fragment sampling. Stage 2 argbuf mode may replace constants, but it
  // must still emit setVertexTexture / setVertexSamplerState before draw.
  Harness harness;
  Capture capture;
  auto recorder = makeRecorder(capture);

  constexpr obj_handle_t kPipeline = 0x700000700000741ull;
  constexpr obj_handle_t kDepthState = 0x700000700000742ull;
  constexpr obj_handle_t kSampler = 0x700000700000743ull;
  constexpr obj_handle_t kRenderTarget = 0x700000700000744ull;
  constexpr obj_handle_t kVertexTexture0 = 0x700000700000745ull;
  constexpr obj_handle_t kBoundVertex = 0x700000700000746ull;

  recorder.suppressBaseStateLookup = true;
  recorder.renderPipelineState.handle = kPipeline;
  recorder.depthStencilState.handle = kDepthState;
  recorder.fragmentSamplerState.handle = kSampler;

  auto state = makeProgrammableState(20u);
  state.hot.colorAttachments[0].handle =
      harness.createRenderTargetSurface(kRenderTarget, 640u, 480u);
  state.hot.textures[dxmt9::core::kVertexTextureSampler0] =
      harness.createBoundTexture(kVertexTexture0, 64u, 64u);
  state.hot.textureMask = 1u << dxmt9::core::kVertexTextureSampler0;
  state.hot.streamBuffers[0] = harness.createBoundBuffer(kBoundVertex, 4096u);

  dxmt9::core::DrawParam param{};
  param.primitiveType = PrimitiveType::TriangleList;
  param.primitiveCount = 1u;
  param.indexed = false;

  PreUploadedDrawData preUploaded{};
  runEncodeDraw(harness, recorder, state, param, preUploaded, {},
                /*skipBaseStateBind=*/false,
                /*argbufHybridMode=*/true);

  bool sawVertexTexture = false;
  bool sawVertexSampler = false;
  for (const auto& command : capture.commands) {
    if (command.kind == RecordedKind::SetVertexTexture) {
      checkEq(command.bufferHandle, kVertexTexture0,
              "vertex texture bind handle");
      checkEq(static_cast<unsigned>(command.index), 0u,
              "vertex texture binds sampler slot 0");
      sawVertexTexture = true;
    } else if (command.kind == RecordedKind::SetVertexSamplerState) {
      checkEq(command.bufferHandle, kSampler,
              "vertex sampler bind handle");
      checkEq(static_cast<unsigned>(command.index), 0u,
              "vertex sampler binds sampler slot 0");
      sawVertexSampler = true;
    }
  }
  check(sawVertexTexture,
        "Stage 2 argbuf mode keeps the direct setVertexTexture bind");
  check(sawVertexSampler,
        "Stage 2 argbuf mode keeps the direct setVertexSamplerState bind");
}

void testTextureSamplerShadowDedupsDirectFragmentAndVertexBinds() {
  Harness harness;
  Capture capture;
  auto recorder = makeRecorder(capture);

  constexpr obj_handle_t kPipeline = 0x700000700000751ull;
  constexpr obj_handle_t kDepthState = 0x700000700000752ull;
  constexpr obj_handle_t kSampler = 0x700000700000753ull;
  constexpr obj_handle_t kRenderTarget = 0x700000700000754ull;
  constexpr obj_handle_t kFragmentTexture0 = 0x700000700000755ull;
  constexpr obj_handle_t kVertexTexture0 = 0x700000700000756ull;
  constexpr obj_handle_t kBoundVertex = 0x700000700000757ull;

  recorder.suppressBaseStateLookup = true;
  recorder.renderPipelineState.handle = kPipeline;
  recorder.depthStencilState.handle = kDepthState;
  recorder.fragmentSamplerState.handle = kSampler;

  auto state = makeProgrammableState(20u);
  state.hot.colorAttachments[0].handle =
      harness.createRenderTargetSurface(kRenderTarget, 640u, 480u);
  state.hot.textures[0] = harness.createBoundTexture(kFragmentTexture0, 64u, 64u);
  state.hot.textures[dxmt9::core::kVertexTextureSampler0] =
      harness.createBoundTexture(kVertexTexture0, 32u, 32u);
  state.hot.textureMask =
      (1u << 0u) | (1u << dxmt9::core::kVertexTextureSampler0);
  state.hot.streamBuffers[0] = harness.createBoundBuffer(kBoundVertex, 4096u);

  dxmt9::core::DrawParam param{};
  param.primitiveType = PrimitiveType::TriangleList;
  param.primitiveCount = 1u;
  param.indexed = false;

  PreUploadedDrawData preUploaded{};
  TextureSamplerBindShadow shadow{};
  runEncodeDraw(harness, recorder, state, param, preUploaded, {},
                /*skipBaseStateBind=*/false,
                /*argbufHybridMode=*/false,
                &shadow);
  runEncodeDraw(harness, recorder, state, param, preUploaded, {},
                /*skipBaseStateBind=*/false,
                /*argbufHybridMode=*/false,
                &shadow);

  checkEq(countCommands(capture, RecordedKind::SetFragmentTexture),
          std::size_t{1},
          "fragment texture bind is deduped across base-state rebinds");
  checkEq(countCommands(capture, RecordedKind::SetFragmentSamplerState),
          std::size_t{1},
          "fragment sampler bind is deduped across base-state rebinds");
  checkEq(countCommands(capture, RecordedKind::SetVertexTexture),
          std::size_t{1},
          "vertex texture bind is deduped across base-state rebinds");
  checkEq(countCommands(capture, RecordedKind::SetVertexSamplerState),
          std::size_t{1},
          "vertex sampler bind is deduped across base-state rebinds");
  checkEq(countCommands(capture, RecordedKind::DrawPrimitives),
          std::size_t{2},
          "dedup keeps both draws");
}

void testTextureSamplerShadowResetForcesDirectRebind() {
  Harness harness;
  Capture capture;
  auto recorder = makeRecorder(capture);

  constexpr obj_handle_t kPipeline = 0x700000700000761ull;
  constexpr obj_handle_t kDepthState = 0x700000700000762ull;
  constexpr obj_handle_t kSampler = 0x700000700000763ull;
  constexpr obj_handle_t kRenderTarget = 0x700000700000764ull;
  constexpr obj_handle_t kTexture0 = 0x700000700000765ull;
  constexpr obj_handle_t kBoundVertex = 0x700000700000766ull;

  recorder.suppressBaseStateLookup = true;
  recorder.renderPipelineState.handle = kPipeline;
  recorder.depthStencilState.handle = kDepthState;
  recorder.fragmentSamplerState.handle = kSampler;

  auto state = makeProgrammableState(20u);
  state.hot.colorAttachments[0].handle =
      harness.createRenderTargetSurface(kRenderTarget, 640u, 480u);
  state.hot.textures[0] = harness.createBoundTexture(kTexture0, 64u, 64u);
  state.hot.textureMask = 0x1u;
  state.hot.streamBuffers[0] = harness.createBoundBuffer(kBoundVertex, 4096u);

  dxmt9::core::DrawParam param{};
  param.primitiveType = PrimitiveType::TriangleList;
  param.primitiveCount = 1u;
  param.indexed = false;

  PreUploadedDrawData preUploaded{};
  TextureSamplerBindShadow shadow{};
  runEncodeDraw(harness, recorder, state, param, preUploaded, {},
                /*skipBaseStateBind=*/false,
                /*argbufHybridMode=*/false,
                &shadow);
  shadow.reset();
  runEncodeDraw(harness, recorder, state, param, preUploaded, {},
                /*skipBaseStateBind=*/false,
                /*argbufHybridMode=*/false,
                &shadow);

  checkEq(countCommands(capture, RecordedKind::SetFragmentTexture),
          std::size_t{2},
          "shadow reset forces fragment texture rebind");
  checkEq(countCommands(capture, RecordedKind::SetFragmentSamplerState),
          std::size_t{2},
          "shadow reset forces fragment sampler rebind");
}

void testNonIndexedDrawPrimitivesAbsorbsStartVertexIntoOffset() {
  Harness harness;
  Capture capture;
  auto recorder = makeRecorder(capture);

  constexpr obj_handle_t kBoundVertex = 0x5000005000005fcull;
  auto state = makeProgrammableState(28u);
  state.hot.streamBuffers[0] = harness.createBoundBuffer(kBoundVertex, 8192u);
  state.hot.streamOffsets[0] = 64u;

  dxmt9::core::DrawParam param{};
  param.primitiveType = PrimitiveType::TriangleList;
  param.primitiveCount = 2u;
  param.startVertex = 3u;
  param.baseVertexIndex = 99;
  param.indexed = false;

  PreUploadedDrawData preUploaded{};
  runEncodeDraw(harness, recorder, state, param, preUploaded, {});

  checkEq(capture.commands.size(), std::size_t{3},
          "non-indexed draw command count");
  const auto& stream = commandAt(capture, 0, "missing non-indexed stream bind");
  check(stream.kind == RecordedKind::SetVertexBuffer,
        "first non-indexed command binds vertex stream");
  checkEq(stream.bufferHandle, kBoundVertex,
          "non-indexed draw uses the bound vertex buffer");
  checkEq(stream.offset, std::uint64_t{148},
          "non-indexed startVertex is absorbed into vertex buffer offset");
  checkEq(static_cast<unsigned>(stream.index), 1u,
          "non-indexed vertex stream binds to Metal slot 1");

  const auto& volCommand =
      commandAt(capture, 1, "missing non-indexed DrawVolatile");
  check(volCommand.kind == RecordedKind::SetVertexBytes,
        "second non-indexed command pushes DrawVolatile");
  checkEq(static_cast<unsigned>(volCommand.index), 5u,
          "non-indexed DrawVolatile binds to Metal slot 5");
  const auto vol = volatileBytes(volCommand);
  checkEq(vol.vertexBaseIndex, std::int32_t{0},
          "non-indexed DrawVolatile base vertex is zero after offset absorb");
  checkEq(vol.vertexStreamOffset, std::uint32_t{0},
          "non-indexed DrawVolatile stream offset");
  checkEq(vol.vertexStreamStride, std::uint32_t{28},
          "non-indexed DrawVolatile stream stride");
  checkEq(vol._pad, std::uint32_t{0}, "non-indexed DrawVolatile pad");

  assertDrawPrimitivesCommand(
      commandAt(capture, 2, "missing non-indexed drawPrimitives"),
      WMTPrimitiveTypeTriangle,
      0u,
      6u);
}

void testFsVolatileAlphaTestImmediatePushShadowAndOverride() {
  // H228 — every draw pushes the per-draw FsVolatile alpha-test immediate at
  // fragment buffer 5: canonical draws resolve the trio from the flat render
  // state, run/batch draws with a per-draw alpha override use its raw values,
  // and the per-encoder shadow collapses redundant pushes.
  Harness harness;
  Capture capture;
  auto recorder = makeRecorder(capture);
  recorder.setFragmentBytes = recordSetFragmentBytes;

  constexpr obj_handle_t kBoundVertex = 0x5600005600005fcull;
  auto state = makeProgrammableState(28u);
  state.hot.streamBuffers[0] = harness.createBoundBuffer(kBoundVertex, 8192u);
  state.hot.colorAttachments[0].sampleCount = 4u;
  setSortedRenderStates(state, {
      dxmt9::core::FlatStateEntry{dxmt9::core::RS_ALPHA_TEST_ENABLE, 1u},
      dxmt9::core::FlatStateEntry{dxmt9::core::RS_ALPHA_REF, 0x80u},
      dxmt9::core::FlatStateEntry{
          dxmt9::core::RS_ALPHA_FUNC,
          static_cast<u32>(dxmt9::core::CompareFunc::GreaterEqual)},
      dxmt9::core::FlatStateEntry{dxmt9::core::RS_MULTISAMPLE_MASK, 0x5u},
  });

  dxmt9::core::DrawParam param{};
  param.primitiveType = PrimitiveType::TriangleList;
  param.primitiveCount = 1u;
  param.indexed = false;

  PreUploadedDrawData preUploaded{};
  TextureSamplerBindShadow shadow{};
  runEncodeDraw(harness, recorder, state, param, preUploaded, {},
                /*skipBaseStateBind=*/true,
                /*argbufHybridMode=*/false,
                &shadow);

  auto fsVolatileAt = [&](std::size_t nth) {
    std::size_t seen = 0;
    for (const auto& command : capture.commands) {
      if (command.kind != RecordedKind::SetFragmentBytes) {
        continue;
      }
      if (seen++ == nth) {
        checkEq(static_cast<unsigned>(command.index), 5u,
                "FsVolatile binds fragment buffer slot 5");
        checkEq(command.length, std::uint64_t{sizeof(dxmt9::state::FsVolatile)},
                "FsVolatile push length matches the host struct");
        dxmt9::state::FsVolatile value{};
        std::memcpy(&value, command.bytes.data(), sizeof(value));
        return value;
      }
    }
    fail("missing expected FsVolatile push");
  };

  checkEq(countCommands(capture, RecordedKind::SetFragmentBytes),
          std::size_t{1}, "state-resolved draw pushes one FsVolatile");
  const auto fromState = fsVolatileAt(0);
  checkEq(fromState.alphaTest,
          static_cast<u32>(dxmt9::core::CompareFunc::GreaterEqual),
          "FsVolatile carries the alpha func when alpha test is enabled");
  checkNear(fromState.alphaRef, 128.0f / 255.0f, 1.0e-6f,
            "FsVolatile alphaRef uses the fillFfpPsConsts 1/255 conversion");
  checkEq(fromState.sampleMask, std::uint32_t{0x5u},
          "FsVolatile carries the per-draw multisample mask");
  auto singleSampleState = state;
  singleSampleState.hot.colorAttachments[0].sampleCount = 1u;
  checkEq(dxmt9::state::buildFsVolatile(singleSampleState.view()).sampleMask,
          std::uint32_t{0xffffffffu},
          "single-sample target forces the effective sample mask to all ones");

  // Same values + live shadow => the second draw skips the push.
  runEncodeDraw(harness, recorder, state, param, preUploaded, {},
                /*skipBaseStateBind=*/true,
                /*argbufHybridMode=*/false,
                &shadow);
  checkEq(countCommands(capture, RecordedKind::SetFragmentBytes),
          std::size_t{1}, "unchanged FsVolatile is deduped by the encoder shadow");

  // A per-draw alpha override wins over the shared state: alpha off here.
  dxmt9::core::DrawBindingOverride alphaOverride{};
  alphaOverride.alphaTestEnable = 0u;
  alphaOverride.alphaTestFunc =
      static_cast<u32>(dxmt9::core::CompareFunc::GreaterEqual);
  alphaOverride.alphaTestRef = 0x80u;
  alphaOverride.alphaTestStateValid = true;
  runEncodeDraw(harness, recorder, state, param, preUploaded, {},
                /*skipBaseStateBind=*/true,
                /*argbufHybridMode=*/false,
                &shadow,
                /*bindingSnapshot=*/nullptr,
                &alphaOverride);
  checkEq(countCommands(capture, RecordedKind::SetFragmentBytes),
          std::size_t{2}, "per-draw alpha override forces a new FsVolatile push");
  const auto fromOverride = fsVolatileAt(1);
  checkEq(fromOverride.alphaTest, 0u,
          "alpha-off override pushes alphaTest=0 despite alpha-on shared state");
  checkNear(fromOverride.alphaRef, 0.0f, 1.0e-6f,
            "alpha-off override zeroes the immediate ref");
  checkEq(fromOverride.sampleMask, std::uint32_t{0x5u},
          "alpha override preserves the shared sample mask");
}

void testNonIndexedDrawIgnoresStaleIndexIntent() {
  Harness harness;
  Capture capture;
  auto recorder = makeRecorder(capture);

  constexpr obj_handle_t kBoundVertex = 0x5500005500005fcull;
  constexpr obj_handle_t kIgnoredBoundIndex = 0x6500006500006bdull;
  constexpr obj_handle_t kIgnoredUploadedIndex = 0x7500007500007ceull;
  auto state = makeProgrammableState(24u);
  state.hot.streamBuffers[0] = harness.createBoundBuffer(kBoundVertex, 8192u);
  state.hot.streamOffsets[0] = 96u;
  state.hot.indexBuffer = harness.createBoundBuffer(kIgnoredBoundIndex, 4096u);

  std::array<std::uint8_t, 16> arena{};
  dxmt9::core::DrawParam param{};
  param.primitiveType = PrimitiveType::LineStrip;
  param.primitiveCount = 4u;
  param.startVertex = 6u;
  param.baseVertexIndex = -11;
  param.startIndex = 9u;
  param.indexType = IndexType::UInt32;
  param.indexed = false;
  param.userIndexRange = dxmt9::core::DrawPayloadRange{0u, arena.size()};

  PreUploadedDrawData preUploaded{};
  preUploaded.index.buffer.handle = kIgnoredUploadedIndex;
  preUploaded.index.offset = 512u;
  preUploaded.index.size = arena.size();

  runEncodeDraw(harness, recorder, state, param, preUploaded, arena);

  checkEq(capture.commands.size(), std::size_t{3},
          "non-indexed stale-index draw command count");

  const auto& stream = commandAt(capture, 0,
                                 "missing non-indexed stale-index stream bind");
  check(stream.kind == RecordedKind::SetVertexBuffer,
        "non-indexed stale-index draw binds vertex stream");
  checkEq(stream.bufferHandle, kBoundVertex,
          "non-indexed stale-index draw uses vertex buffer");
  checkEq(stream.offset, std::uint64_t{240},
          "non-indexed stale-index draw folds startVertex into stream offset");

  const auto& volCommand =
      commandAt(capture, 1, "missing non-indexed stale-index DrawVolatile");
  check(volCommand.kind == RecordedKind::SetVertexBytes,
        "non-indexed stale-index draw pushes DrawVolatile");
  const auto vol = volatileBytes(volCommand);
  checkEq(vol.vertexBaseIndex, std::int32_t{0},
          "non-indexed stale-index DrawVolatile clears base vertex");
  checkEq(vol.vertexStreamOffset, std::uint32_t{0},
          "non-indexed stale-index DrawVolatile stream offset");
  checkEq(vol.vertexStreamStride, std::uint32_t{24},
          "non-indexed stale-index DrawVolatile stride");

  const auto& draw = commandAt(capture, 2,
                               "missing non-indexed stale-index draw");
  assertDrawPrimitivesCommand(draw, WMTPrimitiveTypeLineStrip, 0u, 5u);
  check(draw.kind != RecordedKind::DrawIndexedPrimitives,
        "non-indexed stale-index draw never emits indexed Metal command");

  for (const auto& command : capture.commands) {
    check(command.kind != RecordedKind::DrawIndexedPrimitives,
          "stale bound/user index data is ignored when DrawParam is non-indexed");
    check(command.bufferHandle != kIgnoredBoundIndex,
          "stale bound index buffer is not consumed");
    check(command.bufferHandle != kIgnoredUploadedIndex,
          "stale uploaded index buffer is not consumed");
  }
}

void testIndexedPrimitiveTopologyAndIndexWidthBoundaries() {
  struct Case {
    PrimitiveType primitiveType = PrimitiveType::PointList;
    u32 primitiveCount = 0;
    IndexType indexType = IndexType::UInt16;
    WMTPrimitiveType metalPrimitiveType = WMTPrimitiveTypePoint;
    WMTIndexType metalIndexType = WMTIndexTypeUInt16;
    std::uint64_t indexCount = 0;
    std::uint64_t indexStride = 0;
    const char* label = "";
  };

  const std::array<Case, 5> cases{{
      {PrimitiveType::PointList, 5u, IndexType::UInt16,
       WMTPrimitiveTypePoint, WMTIndexTypeUInt16, 5u, sizeof(std::uint16_t),
       "indexed point list"},
      {PrimitiveType::LineList, 3u, IndexType::UInt16,
       WMTPrimitiveTypeLine, WMTIndexTypeUInt16, 6u, sizeof(std::uint16_t),
       "indexed line list"},
      {PrimitiveType::LineStrip, 3u, IndexType::UInt32,
       WMTPrimitiveTypeLineStrip, WMTIndexTypeUInt32, 4u, sizeof(std::uint32_t),
       "indexed line strip"},
      {PrimitiveType::TriangleList, 2u, IndexType::UInt32,
       WMTPrimitiveTypeTriangle, WMTIndexTypeUInt32, 6u, sizeof(std::uint32_t),
       "indexed triangle list"},
      {PrimitiveType::TriangleStrip, 4u, IndexType::UInt16,
       WMTPrimitiveTypeTriangleStrip, WMTIndexTypeUInt16, 6u,
       sizeof(std::uint16_t), "indexed triangle strip"},
  }};

  constexpr std::uint32_t kStartIndex = 7u;

  for (const auto& testCase : cases) {
    Harness harness;
    Capture capture;
    auto recorder = makeRecorder(capture);

    constexpr obj_handle_t kBoundVertex = 0x5300005300005acull;
    constexpr obj_handle_t kBoundIndex = 0x6300006300006bdull;
    auto state = makeProgrammableState(28u);
    state.hot.streamBuffers[0] =
        harness.createBoundBuffer(kBoundVertex, 16384u);
    state.hot.streamOffsets[0] = 160u;
    state.hot.indexBuffer = harness.createBoundBuffer(kBoundIndex, 4096u);

    dxmt9::core::DrawParam param{};
    param.primitiveType = testCase.primitiveType;
    param.primitiveCount = testCase.primitiveCount;
    param.baseVertexIndex = -5;
    param.startIndex = kStartIndex;
    param.indexType = testCase.indexType;
    param.indexed = true;

    PreUploadedDrawData preUploaded{};
    std::array<std::uint8_t, 1> arena{};
    runEncodeDraw(harness, recorder, state, param, preUploaded, arena);

    checkEq(capture.commands.size(), std::size_t{3},
            std::string(testCase.label) + " command count");
    const auto& stream = commandAt(capture, 0,
                                   std::string(testCase.label) + " stream bind");
    check(stream.kind == RecordedKind::SetVertexBuffer,
          std::string(testCase.label) + " first command binds vertex stream");
    checkEq(stream.bufferHandle, kBoundVertex,
            std::string(testCase.label) + " vertex buffer source");
    checkEq(stream.offset, std::uint64_t{160},
            std::string(testCase.label) + " indexed draw keeps stream offset");

    const auto& volCommand = commandAt(
        capture, 1, std::string(testCase.label) + " DrawVolatile");
    const auto vol = volatileBytes(volCommand);
    checkEq(vol.vertexBaseIndex, std::int32_t{-5},
            std::string(testCase.label) + " DrawVolatile base vertex");
    checkEq(vol.vertexStreamStride, std::uint32_t{28},
            std::string(testCase.label) + " DrawVolatile stride");

    assertIndexedDrawCommand(
        commandAt(capture, 2, std::string(testCase.label) + " draw"),
        kBoundIndex,
        static_cast<std::uint64_t>(kStartIndex) * testCase.indexStride,
        testCase.indexCount,
        testCase.metalPrimitiveType,
        testCase.metalIndexType);
  }
}

void testNonIndexedPrimitiveTopologyVertexCounts() {
  struct Case {
    PrimitiveType primitiveType = PrimitiveType::PointList;
    u32 primitiveCount = 0;
    WMTPrimitiveType metalType = WMTPrimitiveTypePoint;
    std::uint64_t vertexCount = 0;
    const char* label = "";
  };

  const std::array<Case, 5> cases{{
      {PrimitiveType::PointList, 5u, WMTPrimitiveTypePoint, 5u,
       "point list"},
      {PrimitiveType::LineList, 3u, WMTPrimitiveTypeLine, 6u,
       "line list"},
      {PrimitiveType::LineStrip, 3u, WMTPrimitiveTypeLineStrip, 4u,
       "line strip"},
      {PrimitiveType::TriangleList, 4u, WMTPrimitiveTypeTriangle, 12u,
       "triangle list"},
      {PrimitiveType::TriangleStrip, 4u, WMTPrimitiveTypeTriangleStrip, 6u,
       "triangle strip"},
  }};

  for (const auto& testCase : cases) {
    Harness harness;
    Capture capture;
    auto recorder = makeRecorder(capture);

    constexpr obj_handle_t kBoundVertex = 0x5100005100005fcull;
    auto state = makeProgrammableState(20u);
    state.hot.streamBuffers[0] = harness.createBoundBuffer(kBoundVertex, 8192u);
    state.hot.streamOffsets[0] = 80u;

    dxmt9::core::DrawParam param{};
    param.primitiveType = testCase.primitiveType;
    param.primitiveCount = testCase.primitiveCount;
    param.startVertex = 2u;
    param.indexed = false;

    PreUploadedDrawData preUploaded{};
    runEncodeDraw(harness, recorder, state, param, preUploaded, {});

    checkEq(capture.commands.size(), std::size_t{3},
            std::string(testCase.label) + " command count");
    const auto& stream = commandAt(capture, 0,
                                   std::string(testCase.label) + " stream bind");
    checkEq(stream.offset, std::uint64_t{120},
            std::string(testCase.label) + " startVertex folded into stream offset");
    assertDrawPrimitivesCommand(
        commandAt(capture, 2, std::string(testCase.label) + " draw command"),
        testCase.metalType,
        0u,
        testCase.vertexCount);
  }
}

void testProgrammableVsBindsExtraBoundStreamBeforeDraw() {
  Harness harness;
  Capture capture;
  auto recorder = makeRecorder(capture);

  constexpr obj_handle_t kStream0 = 0x5000005000007a0ull;
  constexpr obj_handle_t kStream1 = 0x6000006000008b1ull;
  constexpr u32 kD3DDeclTypeFloat2 = 1u;
  constexpr u32 kD3DDeclTypeFloat3 = 2u;
  constexpr u32 kD3DDeclMethodDefault = 0u;
  constexpr u32 kD3DDeclUsagePosition = 0u;
  constexpr u32 kD3DDeclUsageTexcoord = 5u;

  auto state = makeProgrammableState(12u);
  state.hot.streamBuffers[0] = harness.createBoundBuffer(kStream0, 8192u);
  state.hot.streamBuffers[1] = harness.createBoundBuffer(kStream1, 8192u);
  state.hot.streamOffsets[0] = 32u;
  state.hot.streamOffsets[1] = 144u;
  state.hot.streamStrides[1] = 16u;
  state.hot.streamFrequencies[1] =
      dxmt9::core::kStreamSourceInstanceData | 2u;
  state.shaderLayout.vertexDecl.elements = {
      dxmt9::core::VertexElement{0, 0, kD3DDeclTypeFloat3,
                                 kD3DDeclMethodDefault,
                                 kD3DDeclUsagePosition, 0},
      dxmt9::core::VertexElement{1, 0, kD3DDeclTypeFloat2,
                                 kD3DDeclMethodDefault,
                                 kD3DDeclUsageTexcoord, 0},
  };
  state.shaderLayout.vertexDecl.streams[1].stride = 16u;

  std::array<std::uint8_t, 1> arena{};
  dxmt9::core::DrawParam param{};
  param.primitiveType = PrimitiveType::TriangleList;
  param.primitiveCount = 2u;
  param.startVertex = 3u;
  param.indexType = IndexType::UInt16;
  param.indexed = false;
  param.instanceCount = 4u;

  PreUploadedDrawData preUploaded{};
  runEncodeDraw(harness, recorder, state, param, preUploaded, arena);

  checkEq(capture.commands.size(), std::size_t{4},
          "multi-stream programmable draw command count");

  const auto& stream0 = commandAt(capture, 0, "missing stream0 bind");
  check(stream0.kind == RecordedKind::SetVertexBuffer,
        "first command binds stream0");
  checkEq(stream0.bufferHandle, kStream0, "stream0 buffer source");
  checkEq(stream0.offset, std::uint64_t{68},
          "stream0 offset folds non-indexed start vertex");
  checkEq(static_cast<unsigned>(stream0.index), 1u,
          "stream0 binds to Metal slot 1");

  const auto& stream1 = commandAt(capture, 1, "missing stream1 bind");
  check(stream1.kind == RecordedKind::SetVertexBuffer,
        "second command binds stream1");
  checkEq(stream1.bufferHandle, kStream1, "stream1 buffer source");
  checkEq(stream1.offset, std::uint64_t{144},
          "instanced stream1 offset does not fold non-indexed start vertex");
  checkEq(static_cast<unsigned>(stream1.index), 6u,
          "stream1 binds to generated extra Metal slot");

  const auto& volCommand =
      commandAt(capture, 2, "missing multi-stream DrawVolatile");
  check(volCommand.kind == RecordedKind::SetVertexBytes,
        "third command pushes DrawVolatile");
  checkEq(static_cast<unsigned>(volCommand.index), 5u,
          "DrawVolatile binds after extra stream and before draw");
  const auto vol = volatileBytes(volCommand);
  checkEq(vol.vertexBaseIndex, std::int32_t{0},
          "multi-stream DrawVolatile base vertex");
  checkEq(vol.vertexStreamOffset, std::uint32_t{0},
          "multi-stream DrawVolatile stream offset");
  checkEq(vol.vertexStreamStride, std::uint32_t{12},
          "multi-stream DrawVolatile stream stride");
  checkEq(vol._pad, std::uint32_t{0}, "multi-stream DrawVolatile pad");
  checkEq(vol.streamInstanceDivisors[1], std::uint32_t{2},
          "multi-stream DrawVolatile carries stream1 instance divisor");

  const auto& draw =
      commandAt(capture, 3, "missing multi-stream drawPrimitives");
  check(draw.kind == RecordedKind::DrawPrimitives,
        "multi-stream draw uses drawPrimitives");
  checkEq(draw.count, std::uint64_t{6}, "multi-stream vertex count");
  checkEq(draw.instanceCount, std::uint32_t{4},
          "multi-stream Metal draw preserves instance count");
  checkEq(draw.baseInstance, std::uint32_t{0},
          "multi-stream Metal draw starts at instance zero");
}

void testProgrammableVsSkipsMissingExtraStreamWithoutStaleBind() {
  Harness harness;
  Capture capture;
  auto recorder = makeRecorder(capture);

  constexpr obj_handle_t kStream0 = 0x5300005300007a0ull;
  constexpr u32 kD3DDeclTypeFloat2 = 1u;
  constexpr u32 kD3DDeclTypeFloat3 = 2u;
  constexpr u32 kD3DDeclMethodDefault = 0u;
  constexpr u32 kD3DDeclUsagePosition = 0u;
  constexpr u32 kD3DDeclUsageTexcoord = 5u;

  auto state = makeProgrammableState(20u);
  state.hot.streamBuffers[0] = harness.createBoundBuffer(kStream0, 8192u);
  state.hot.streamOffsets[0] = 40u;
  state.hot.streamOffsets[3] = 256u;
  state.hot.streamStrides[3] = 12u;
  state.shaderLayout.vertexDecl.elements = {
      dxmt9::core::VertexElement{0, 0, kD3DDeclTypeFloat3,
                                 kD3DDeclMethodDefault,
                                 kD3DDeclUsagePosition, 0},
      dxmt9::core::VertexElement{3, 0, kD3DDeclTypeFloat2,
                                 kD3DDeclMethodDefault,
                                 kD3DDeclUsageTexcoord, 0},
  };
  state.shaderLayout.vertexDecl.streams[3].stride = 12u;

  dxmt9::core::DrawParam param{};
  param.primitiveType = PrimitiveType::TriangleStrip;
  param.primitiveCount = 2u;
  param.startVertex = 5u;
  param.indexed = false;

  PreUploadedDrawData preUploaded{};
  runEncodeDraw(harness, recorder, state, param, preUploaded, {});

  checkEq(capture.commands.size(), std::size_t{3},
          "missing extra stream draw command count");

  const auto& stream0 = commandAt(capture, 0, "missing primary stream bind");
  check(stream0.kind == RecordedKind::SetVertexBuffer,
        "missing-extra-stream draw binds primary stream");
  checkEq(stream0.bufferHandle, kStream0,
          "missing-extra-stream draw uses primary stream buffer");
  checkEq(stream0.offset, std::uint64_t{140},
          "missing-extra-stream draw folds startVertex into primary stream");
  checkEq(static_cast<unsigned>(stream0.index), 1u,
          "missing-extra-stream draw binds primary Metal slot");

  const auto& volCommand =
      commandAt(capture, 1, "missing missing-extra-stream DrawVolatile");
  check(volCommand.kind == RecordedKind::SetVertexBytes,
        "missing-extra-stream draw pushes DrawVolatile after primary stream");
  const auto vol = volatileBytes(volCommand);
  checkEq(vol.vertexBaseIndex, std::int32_t{0},
          "missing-extra-stream DrawVolatile clears base vertex");
  checkEq(vol.vertexStreamOffset, std::uint32_t{0},
          "missing-extra-stream DrawVolatile stream offset");
  checkEq(vol.vertexStreamStride, std::uint32_t{20},
          "missing-extra-stream DrawVolatile primary stride");
  checkEq(vol._pad, std::uint32_t{0},
          "missing-extra-stream DrawVolatile pad");

  assertDrawPrimitivesCommand(
      commandAt(capture, 2, "missing missing-extra-stream drawPrimitives"),
      WMTPrimitiveTypeTriangleStrip,
      0u,
      4u);

  for (const auto& command : capture.commands) {
    check(command.kind != RecordedKind::SetVertexBuffer ||
              static_cast<unsigned>(command.index) != 8u,
          "missing extra stream does not bind stale Metal slot 8");
  }
}

void testIndexedProgrammableDrawPreservesSparseStreamOffsets() {
  Harness harness;
  Capture capture;
  auto recorder = makeRecorder(capture);

  constexpr obj_handle_t kStream0 = 0x5200005200007a0ull;
  constexpr obj_handle_t kStream2 = 0x6200006200008b2ull;
  constexpr obj_handle_t kIndexBuffer = 0x7200007200009c3ull;
  constexpr u32 kD3DDeclTypeFloat2 = 1u;
  constexpr u32 kD3DDeclTypeFloat3 = 2u;
  constexpr u32 kD3DDeclMethodDefault = 0u;
  constexpr u32 kD3DDeclUsagePosition = 0u;
  constexpr u32 kD3DDeclUsageTexcoord = 5u;

  auto state = makeProgrammableState(20u);
  state.hot.streamBuffers[0] = harness.createBoundBuffer(kStream0, 8192u);
  state.hot.streamBuffers[2] = harness.createBoundBuffer(kStream2, 8192u);
  state.hot.streamOffsets[0] = 64u;
  state.hot.streamOffsets[2] = 192u;
  state.hot.streamStrides[2] = 12u;
  state.hot.indexBuffer = harness.createBoundBuffer(kIndexBuffer, 4096u);
  state.shaderLayout.vertexDecl.elements = {
      dxmt9::core::VertexElement{0, 0, kD3DDeclTypeFloat3,
                                 kD3DDeclMethodDefault,
                                 kD3DDeclUsagePosition, 0},
      dxmt9::core::VertexElement{2, 0, kD3DDeclTypeFloat2,
                                 kD3DDeclMethodDefault,
                                 kD3DDeclUsageTexcoord, 0},
  };
  state.shaderLayout.vertexDecl.streams[2].stride = 12u;

  std::array<std::uint8_t, 1> arena{};
  dxmt9::core::DrawParam param{};
  param.primitiveType = PrimitiveType::TriangleList;
  param.primitiveCount = 2u;
  param.startVertex = 9u;
  param.baseVertexIndex = -2;
  param.startIndex = 5u;
  param.indexType = IndexType::UInt32;
  param.indexed = true;

  PreUploadedDrawData preUploaded{};
  runEncodeDraw(harness, recorder, state, param, preUploaded, arena);

  checkEq(capture.commands.size(), std::size_t{4},
          "indexed sparse-stream draw command count");

  const auto& stream0 = commandAt(capture, 0,
                                  "missing indexed stream0 bind");
  check(stream0.kind == RecordedKind::SetVertexBuffer,
        "first indexed command binds stream0");
  checkEq(stream0.bufferHandle, kStream0, "indexed stream0 buffer source");
  checkEq(stream0.offset, std::uint64_t{64},
          "indexed stream0 keeps bound offset");
  checkEq(static_cast<unsigned>(stream0.index), 1u,
          "indexed stream0 binds to Metal slot 1");

  const auto& stream2 = commandAt(capture, 1,
                                  "missing indexed sparse stream2 bind");
  check(stream2.kind == RecordedKind::SetVertexBuffer,
        "second indexed command binds sparse stream2");
  checkEq(stream2.bufferHandle, kStream2,
          "indexed sparse stream2 buffer source");
  checkEq(stream2.offset, std::uint64_t{192},
          "indexed sparse stream2 keeps bound offset");
  checkEq(static_cast<unsigned>(stream2.index), 7u,
          "indexed sparse stream2 binds to generated extra Metal slot");

  const auto& volCommand =
      commandAt(capture, 2, "missing indexed sparse-stream DrawVolatile");
  check(volCommand.kind == RecordedKind::SetVertexBytes,
        "third indexed command pushes DrawVolatile");
  checkEq(static_cast<unsigned>(volCommand.index), 5u,
          "indexed DrawVolatile binds before draw");
  const auto vol = volatileBytes(volCommand);
  checkEq(vol.vertexBaseIndex, std::int32_t{-2},
          "indexed DrawVolatile carries base vertex");
  checkEq(vol.vertexStreamOffset, std::uint32_t{0},
          "indexed DrawVolatile stream offset");
  checkEq(vol.vertexStreamStride, std::uint32_t{20},
          "indexed DrawVolatile primary stream stride");
  checkEq(vol._pad, std::uint32_t{0}, "indexed DrawVolatile pad");

  const auto& draw = commandAt(capture, 3,
                               "missing indexed sparse-stream draw");
  check(draw.kind == RecordedKind::DrawIndexedPrimitives,
        "fourth indexed command is drawIndexedPrimitives");
  checkEq(static_cast<unsigned>(draw.primitiveType),
          static_cast<unsigned>(WMTPrimitiveTypeTriangle),
          "indexed sparse-stream primitive type");
  checkEq(static_cast<unsigned>(draw.indexType),
          static_cast<unsigned>(WMTIndexTypeUInt32),
          "indexed sparse-stream index type");
  checkEq(draw.count, std::uint64_t{6}, "indexed sparse-stream index count");
  checkEq(draw.bufferHandle, kIndexBuffer,
          "indexed sparse-stream index buffer source");
  checkEq(draw.offset, std::uint64_t{20},
          "indexed sparse-stream startIndex becomes UInt32 byte offset");
  checkEq(draw.instanceCount, std::uint32_t{1},
          "indexed sparse-stream instance count");
  checkEq(draw.baseVertex, std::int32_t{0},
          "Metal draw base vertex stays zero; shader uses DrawVolatile");
  checkEq(draw.baseInstance, std::uint32_t{0},
          "indexed sparse-stream base instance");
}

void testProgrammableIndexedBlendHeuristicStaysDirect() {
  Harness harness;
  Capture capture;
  auto recorder = makeRecorder(capture);

  constexpr obj_handle_t kUploadedVertex = 0x5200005200005acull;
  constexpr obj_handle_t kUploadedIndex = 0x6200006200006bdull;
  constexpr u32 kStride = 20u;
  constexpr u32 kIndexOffset = 6u * kStride;

  auto state = makeProgrammableState(kStride);
  state.shaderLayout.vertexDecl.fvf =
      dxmt9::ffp::kFvfXyz | (1u << dxmt9::ffp::kFvfTexCountShift);
  state.hot.textureMask = 0x3fu;
  setInvDestColorAddBlend(state);

  std::array<std::uint8_t, kIndexOffset + 12u> arena{};
  const std::array<std::uint16_t, 6> indices{{0u, 1u, 2u, 3u, 4u, 5u}};
  std::memcpy(arena.data() + kIndexOffset, indices.data(),
              indices.size() * sizeof(indices[0]));

  dxmt9::core::DrawParam param{};
  param.primitiveType = PrimitiveType::TriangleList;
  param.primitiveCount = 2u;
  param.baseVertexIndex = 0;
  param.startIndex = 0u;
  param.indexType = IndexType::UInt16;
  param.indexed = true;
  param.userVertexRange = dxmt9::core::DrawPayloadRange{0u, kIndexOffset};
  param.userIndexRange = dxmt9::core::DrawPayloadRange{kIndexOffset, 12u};

  PreUploadedDrawData preUploaded{};
  preUploaded.vertex.buffer.handle = kUploadedVertex;
  preUploaded.vertex.offset = 320u;
  preUploaded.vertex.size = kIndexOffset;
  preUploaded.index.buffer.handle = kUploadedIndex;
  preUploaded.index.offset = 640u;
  preUploaded.index.size = 12u;

  runEncodeDraw(harness, recorder, state, param, preUploaded, arena);

  const RecordedCommand* indexedDraw = nullptr;
  for (const auto& command : capture.commands) {
    check(command.kind != RecordedKind::DrawPrimitives,
          "programmable indexed draw must not be expanded to drawPrimitives");
    if (command.kind == RecordedKind::DrawIndexedPrimitives) {
      indexedDraw = &command;
    }
  }

  check(indexedDraw != nullptr,
        "programmable indexed blend heuristic emits direct indexed draw");
  assertIndexedDrawCommand(*indexedDraw, kUploadedIndex, 640u, 6u);
}

void testAutoExpandIndexedDrawHeuristicCoversProgrammableR32FCube() {
  auto state = makeProgrammableState(20u);
  setFvfXyzTex1(state);
  setInvDestColorAddBlend(state);

  check(!dxmt9::encoders::shouldAutoExpandIndexedDraw(
            state.hot.renderStates,
            0x3fu,
            /*fixedFunctionPath=*/false,
            /*ffpDecodableLayout=*/true,
            /*texture0R32FCube=*/false),
        "generic programmable indexed blend stays direct");
  check(!dxmt9::encoders::shouldAutoExpandIndexedDraw(
            state.hot.renderStates,
            0x3fu,
            /*fixedFunctionPath=*/false,
            /*ffpDecodableLayout=*/false,
            /*texture0R32FCube=*/false),
        "generic programmable indexed blend without FFP-decodable layout stays direct");
  check(dxmt9::encoders::shouldAutoExpandIndexedDraw(
            state.hot.renderStates,
            0x3fu,
            /*fixedFunctionPath=*/true,
            /*ffpDecodableLayout=*/true,
            /*texture0R32FCube=*/false),
        "existing FFP indexed blend heuristic still expands");
  check(dxmt9::encoders::shouldAutoExpandIndexedDraw(
            state.hot.renderStates,
            0x1fu,
            /*fixedFunctionPath=*/false,
            /*ffpDecodableLayout=*/true,
            /*texture0R32FCube=*/true),
        "programmable R32F cube shadow indexed blend expands");
  check(!dxmt9::encoders::shouldAutoExpandIndexedDraw(
            state.hot.renderStates,
            0x1fu,
            /*fixedFunctionPath=*/false,
            /*ffpDecodableLayout=*/false,
            /*texture0R32FCube=*/true),
        "programmable R32F cube requires FFP-decodable layout");
  check(!dxmt9::encoders::shouldAutoExpandIndexedDraw(
            state.hot.renderStates,
            0x0fu,
            /*fixedFunctionPath=*/false,
            /*ffpDecodableLayout=*/true,
            /*texture0R32FCube=*/true),
        "programmable R32F cube requires observed shadow sampler mask");
}

void testScreenBlendIndexOrderOptimizationPredicateIsStrict() {
  auto state = makeProgrammableState(20u);
  state.hot.renderStates.entries[0] = dxmt9::core::FlatStateEntry{
      dxmt9::core::RS_Z_ENABLE,
      1u};
  state.hot.renderStates.entries[1] = dxmt9::core::FlatStateEntry{
      dxmt9::core::RS_Z_WRITE_ENABLE,
      0u};
  state.hot.renderStates.entries[2] = dxmt9::core::FlatStateEntry{
      dxmt9::core::RS_SRC_BLEND,
      static_cast<u32>(dxmt9::core::BlendFactor::InvDestColor)};
  state.hot.renderStates.entries[3] = dxmt9::core::FlatStateEntry{
      dxmt9::core::RS_DEST_BLEND,
      static_cast<u32>(dxmt9::core::BlendFactor::One)};
  state.hot.renderStates.entries[4] = dxmt9::core::FlatStateEntry{
      dxmt9::core::RS_ALPHABLEND_ENABLE,
      1u};
  state.hot.renderStates.count = 5u;

  check(dxmt9::encoders::shouldOptimizeScreenBlendIndexOrder(
            state.hot.renderStates),
        "screen blend with read-only depth is order-optimization eligible");

  auto depthDisabled = state;
  depthDisabled.hot.renderStates.entries[0].value = 0u;
  check(!dxmt9::encoders::shouldOptimizeScreenBlendIndexOrder(
            depthDisabled.hot.renderStates),
        "disabled depth rejects screen-blend index-order optimization");

  auto depthWrite = state;
  depthWrite.hot.renderStates.entries[1].value = 1u;
  check(!dxmt9::encoders::shouldOptimizeScreenBlendIndexOrder(
            depthWrite.hot.renderStates),
        "depth writes disable screen-blend index-order optimization");

  auto alphaTest = state;
  alphaTest.hot.renderStates.entries[2] = dxmt9::core::FlatStateEntry{
      dxmt9::core::RS_ALPHA_TEST_ENABLE,
      1u};
  alphaTest.hot.renderStates.entries[3] = state.hot.renderStates.entries[2];
  alphaTest.hot.renderStates.entries[4] = state.hot.renderStates.entries[3];
  alphaTest.hot.renderStates.entries[5] = state.hot.renderStates.entries[4];
  alphaTest.hot.renderStates.count = 6u;
  check(!dxmt9::encoders::shouldOptimizeScreenBlendIndexOrder(
            alphaTest.hot.renderStates),
        "alpha test disables screen-blend index-order optimization");

  auto separateAlpha = state;
  separateAlpha.hot.renderStates.entries[5] = dxmt9::core::FlatStateEntry{
      dxmt9::core::RS_SEPARATE_ALPHA_BLEND_ENABLE,
      1u};
  separateAlpha.hot.renderStates.count = 6u;
  check(!dxmt9::encoders::shouldOptimizeScreenBlendIndexOrder(
            separateAlpha.hot.renderStates),
        "separate alpha disables screen-blend index-order optimization");

  auto stencil = state;
  stencil.hot.renderStates.entries[5] = dxmt9::core::FlatStateEntry{
      dxmt9::core::RS_STENCIL_ENABLE,
      1u};
  stencil.hot.renderStates.count = 6u;
  check(!dxmt9::encoders::shouldOptimizeScreenBlendIndexOrder(
            stencil.hot.renderStates),
        "stencil disables screen-blend index-order optimization");

  auto clipPlane = state;
  clipPlane.hot.renderStates.entries[5] = dxmt9::core::FlatStateEntry{
      dxmt9::core::RS_CLIP_PLANE_ENABLE,
      1u};
  clipPlane.hot.renderStates.count = 6u;
  check(!dxmt9::encoders::shouldOptimizeScreenBlendIndexOrder(
            clipPlane.hot.renderStates),
        "clip planes disable screen-blend index-order optimization");

  auto differentBlendOp = state;
  differentBlendOp.hot.renderStates.entries[5] = dxmt9::core::FlatStateEntry{
      dxmt9::core::RS_BLEND_OP,
      static_cast<u32>(dxmt9::core::BlendOp::Subtract)};
  differentBlendOp.hot.renderStates.count = 6u;
  check(!dxmt9::encoders::shouldOptimizeScreenBlendIndexOrder(
            differentBlendOp.hot.renderStates),
        "non-add blend op disables screen-blend index-order optimization");

  auto differentBlend = state;
  differentBlend.hot.renderStates.entries[2].value =
      static_cast<u32>(dxmt9::core::BlendFactor::SrcAlpha);
  check(!dxmt9::encoders::shouldOptimizeScreenBlendIndexOrder(
            differentBlend.hot.renderStates),
        "non-screen blend disables index-order optimization");
}

void testOpaqueDepthIndexOrderOptimizationPredicateIsStrict() {
  auto state = makeProgrammableState(20u);
  setOpaqueDepthRenderStates(state);

  check(dxmt9::encoders::shouldOptimizeOpaqueDepthIndexOrder(
            state.hot.renderStates,
            WMTTriangleFillModeFill),
        "opaque depth-writing triangle fill is order-optimization eligible");
  check(!dxmt9::encoders::shouldOptimizeOpaqueDepthIndexOrder(
            state.hot.renderStates,
            WMTTriangleFillModeFill,
            /*depthWriteGloballyDisabled=*/true),
        "globally disabled depth write rejects opaque-depth index-order optimization");
  check(!dxmt9::encoders::shouldOptimizeOpaqueDepthIndexOrder(
            state.hot.renderStates,
            WMTTriangleFillModeLines),
        "wireframe fill rejects opaque-depth index-order optimization");

  auto depthDisabled = state;
  depthDisabled.hot.renderStates.entries[0].value = 0u;
  check(!dxmt9::encoders::shouldOptimizeOpaqueDepthIndexOrder(
            depthDisabled.hot.renderStates,
            WMTTriangleFillModeFill),
        "disabled depth rejects opaque-depth index-order optimization");

  auto depthRead = state;
  depthRead.hot.renderStates.entries[1].value = 0u;
  check(!dxmt9::encoders::shouldOptimizeOpaqueDepthIndexOrder(
            depthRead.hot.renderStates,
            WMTTriangleFillModeFill),
        "depth-read state rejects opaque-depth index-order optimization");

  auto greaterDepth = state;
  greaterDepth.hot.renderStates.entries[2].value =
      static_cast<u32>(dxmt9::core::CompareFunc::Greater);
  check(!dxmt9::encoders::shouldOptimizeOpaqueDepthIndexOrder(
            greaterDepth.hot.renderStates,
            WMTTriangleFillModeFill),
        "non-order-preserving depth func rejects opaque-depth index-order optimization");
  check(dxmt9::encoders::shouldOptimizeOpaqueDepthIndexOrder(
            greaterDepth.hot.renderStates,
            WMTTriangleFillModeFill,
            /*depthWriteGloballyDisabled=*/false,
            /*extendedScope=*/true),
        "extended scope accepts reverse-depth comparison");

  auto alphaBlend = state;
  setSortedRenderStates(
      alphaBlend,
      {
          dxmt9::core::FlatStateEntry{
              dxmt9::core::RS_ALPHABLEND_ENABLE,
              1u},
          dxmt9::core::FlatStateEntry{
              dxmt9::core::RS_Z_ENABLE,
              1u},
          dxmt9::core::FlatStateEntry{
              dxmt9::core::RS_Z_WRITE_ENABLE,
              1u},
          dxmt9::core::FlatStateEntry{
              dxmt9::core::RS_Z_FUNC,
              static_cast<u32>(dxmt9::core::CompareFunc::LessEqual)},
      });
  check(!dxmt9::encoders::shouldOptimizeOpaqueDepthIndexOrder(
            alphaBlend.hot.renderStates,
            WMTTriangleFillModeFill),
        "alpha blend rejects opaque-depth index-order optimization");

  auto sourceReplacementBlend = state;
  setSortedRenderStates(
      sourceReplacementBlend,
      {
          dxmt9::core::FlatStateEntry{
              dxmt9::core::RS_ALPHABLEND_ENABLE,
              1u},
          dxmt9::core::FlatStateEntry{
              dxmt9::core::RS_Z_ENABLE,
              1u},
          dxmt9::core::FlatStateEntry{
              dxmt9::core::RS_SRC_BLEND,
              static_cast<u32>(dxmt9::core::BlendFactor::One)},
          dxmt9::core::FlatStateEntry{
              dxmt9::core::RS_DEST_BLEND,
              static_cast<u32>(dxmt9::core::BlendFactor::Zero)},
          dxmt9::core::FlatStateEntry{
              dxmt9::core::RS_Z_WRITE_ENABLE,
              1u},
          dxmt9::core::FlatStateEntry{
              dxmt9::core::RS_Z_FUNC,
              static_cast<u32>(dxmt9::core::CompareFunc::LessEqual)},
          dxmt9::core::FlatStateEntry{
              dxmt9::core::RS_BLEND_OP,
              static_cast<u32>(dxmt9::core::BlendOp::Add)},
      });
  check(dxmt9::encoders::shouldOptimizeOpaqueDepthIndexOrder(
            sourceReplacementBlend.hot.renderStates,
            WMTTriangleFillModeFill,
            /*depthWriteGloballyDisabled=*/false,
            /*extendedScope=*/true),
        "extended scope accepts source-replacement blending");

  auto separateAlpha = sourceReplacementBlend;
  setSortedRenderStates(
      separateAlpha,
      {
          dxmt9::core::FlatStateEntry{
              dxmt9::core::RS_ALPHABLEND_ENABLE,
              1u},
          dxmt9::core::FlatStateEntry{
              dxmt9::core::RS_Z_ENABLE,
              1u},
          dxmt9::core::FlatStateEntry{
              dxmt9::core::RS_SRC_BLEND,
              static_cast<u32>(dxmt9::core::BlendFactor::One)},
          dxmt9::core::FlatStateEntry{
              dxmt9::core::RS_DEST_BLEND,
              static_cast<u32>(dxmt9::core::BlendFactor::Zero)},
          dxmt9::core::FlatStateEntry{
              dxmt9::core::RS_Z_WRITE_ENABLE,
              1u},
          dxmt9::core::FlatStateEntry{
              dxmt9::core::RS_Z_FUNC,
              static_cast<u32>(dxmt9::core::CompareFunc::LessEqual)},
          dxmt9::core::FlatStateEntry{
              dxmt9::core::RS_BLEND_OP,
              static_cast<u32>(dxmt9::core::BlendOp::Add)},
          dxmt9::core::FlatStateEntry{
              dxmt9::core::RS_SEPARATE_ALPHA_BLEND_ENABLE,
              1u},
      });
  check(!dxmt9::encoders::shouldOptimizeOpaqueDepthIndexOrder(
             separateAlpha.hot.renderStates,
             WMTTriangleFillModeFill,
             /*depthWriteGloballyDisabled=*/false,
             /*extendedScope=*/true),
        "extended scope rejects separate-alpha replacement ambiguity");

  auto alphaTest = state;
  setSortedRenderStates(
      alphaTest,
      {
          dxmt9::core::FlatStateEntry{
              dxmt9::core::RS_ALPHA_TEST_ENABLE,
              1u},
          dxmt9::core::FlatStateEntry{
              dxmt9::core::RS_Z_ENABLE,
              1u},
          dxmt9::core::FlatStateEntry{
              dxmt9::core::RS_Z_WRITE_ENABLE,
              1u},
          dxmt9::core::FlatStateEntry{
              dxmt9::core::RS_Z_FUNC,
              static_cast<u32>(dxmt9::core::CompareFunc::LessEqual)},
      });
  check(!dxmt9::encoders::shouldOptimizeOpaqueDepthIndexOrder(
            alphaTest.hot.renderStates,
            WMTTriangleFillModeFill),
        "alpha test rejects opaque-depth index-order optimization");

  auto stencil = state;
  setSortedRenderStates(
      stencil,
      {
          dxmt9::core::FlatStateEntry{
              dxmt9::core::RS_STENCIL_ENABLE,
              1u},
          dxmt9::core::FlatStateEntry{
              dxmt9::core::RS_Z_ENABLE,
              1u},
          dxmt9::core::FlatStateEntry{
              dxmt9::core::RS_Z_WRITE_ENABLE,
              1u},
          dxmt9::core::FlatStateEntry{
              dxmt9::core::RS_Z_FUNC,
              static_cast<u32>(dxmt9::core::CompareFunc::LessEqual)},
      });
  check(!dxmt9::encoders::shouldOptimizeOpaqueDepthIndexOrder(
            stencil.hot.renderStates,
            WMTTriangleFillModeFill),
        "stencil rejects opaque-depth index-order optimization");

  auto clipPlane = state;
  setSortedRenderStates(
      clipPlane,
      {
          dxmt9::core::FlatStateEntry{
              dxmt9::core::RS_CLIP_PLANE_ENABLE,
              1u},
          dxmt9::core::FlatStateEntry{
              dxmt9::core::RS_Z_ENABLE,
              1u},
          dxmt9::core::FlatStateEntry{
              dxmt9::core::RS_Z_WRITE_ENABLE,
              1u},
          dxmt9::core::FlatStateEntry{
              dxmt9::core::RS_Z_FUNC,
              static_cast<u32>(dxmt9::core::CompareFunc::LessEqual)},
      });
  check(!dxmt9::encoders::shouldOptimizeOpaqueDepthIndexOrder(
            clipPlane.hot.renderStates,
            WMTTriangleFillModeFill),
        "clip planes reject opaque-depth index-order optimization");
}

void testCompatibleIndexedDrawMergePreservesContiguousIndexOrder() {
  std::array<dxmt9::core::DrawParam, 4> draws{};
  for (auto& draw : draws) {
    draw.indexed = true;
    draw.primitiveType = PrimitiveType::TriangleList;
    draw.indexType = IndexType::UInt16;
    draw.instanceCount = 1u;
    draw.baseVertexIndex = -3;
    draw.startVertex = 7u;
    draw.uniformHandle = dxmt9::core::DrawUniformHandle{
        .index = 2u,
        .generation = 4u,
        .hash = 0x55u,
    };
  }
  draws[0].startIndex = 3u;
  draws[0].primitiveCount = 2u;
  draws[1].startIndex = 9u;
  draws[1].primitiveCount = 4u;
  draws[2].startIndex = 21u;
  draws[2].primitiveCount = 1u;
  draws[3].startIndex = 30u;
  draws[3].primitiveCount = 8u;

  const auto merged = dxmt9::encoders::makeCompatibleIndexedDrawMerge(
      draws, {});
  checkEq(merged.drawCount, std::size_t{3},
          "three contiguous indexed draws merge");
  checkEq(merged.param.startIndex, 3u,
          "merged draw keeps first source index");
  checkEq(merged.param.primitiveCount, 7u,
          "merged draw sums primitive counts");
}

void testCompatibleIndexedDrawMergeRejectsObservableDifferences() {
  std::array<dxmt9::core::DrawParam, 2> draws{};
  for (auto& draw : draws) {
    draw.indexed = true;
    draw.primitiveType = PrimitiveType::TriangleList;
    draw.primitiveCount = 1u;
    draw.instanceCount = 1u;
    draw.uniformHandle = dxmt9::core::DrawUniformHandle{
        .index = 1u,
        .generation = 1u,
        .hash = 0x44u,
    };
  }
  draws[1].startIndex = 3u;

  auto changedUniform = draws;
  changedUniform[1].uniformHandle.hash = 0x45u;
  checkEq(dxmt9::encoders::makeCompatibleIndexedDrawMerge(
              changedUniform, {}).drawCount,
          std::size_t{1},
          "uniform change stops indexed draw merge");

  auto instanced = draws;
  instanced[0].instanceCount = 2u;
  instanced[1].instanceCount = 2u;
  checkEq(dxmt9::encoders::makeCompatibleIndexedDrawMerge(
              instanced, {}).drawCount,
          std::size_t{1},
          "instancing keeps original cross-draw primitive order");

  auto nonContiguous = draws;
  nonContiguous[1].startIndex = 6u;
  checkEq(dxmt9::encoders::makeCompatibleIndexedDrawMerge(
              nonContiguous, {}).drawCount,
          std::size_t{1},
          "non-contiguous index ranges do not merge without a joined buffer");

  auto overflowingIndexCount = draws;
  overflowingIndexCount[0].primitiveCount =
      std::numeric_limits<u32>::max() / 3u;
  overflowingIndexCount[1].startIndex =
      overflowingIndexCount[0].primitiveCount * 3u;
  checkEq(dxmt9::encoders::makeCompatibleIndexedDrawMerge(
              overflowingIndexCount, {}).drawCount,
          std::size_t{1},
          "combined Metal index count must remain representable");

  std::array<std::uint8_t, 8> payload{};
  payload[0] = 1u;
  payload[4] = 2u;
  auto changedBinding = draws;
  changedBinding[0].bindingOverrideRange = {0u, 4u};
  changedBinding[1].bindingOverrideRange = {4u, 4u};
  checkEq(dxmt9::encoders::makeCompatibleIndexedDrawMerge(
              changedBinding, payload).drawCount,
          std::size_t{1},
          "binding change stops indexed draw merge");
}

void testCompatibleIndexedDrawMergeTelemetryClassifiesRelaxationSets() {
  using dxmt9::encoders::CompatibleIndexedDrawMergeReject;
  using dxmt9::encoders::CompatibleIndexedDrawMergeRelaxation;

  std::array<dxmt9::core::DrawParam, 2> draws{};
  for (auto& draw : draws) {
    draw.indexed = true;
    draw.primitiveType = PrimitiveType::TriangleList;
    draw.primitiveCount = 1u;
    draw.instanceCount = 1u;
    draw.uniformHandle = dxmt9::core::DrawUniformHandle{
        .index = 1u,
        .generation = 1u,
        .hash = 0x44u,
    };
  }
  draws[1].startIndex = 3u;

  auto uniformOnly = draws;
  uniformOnly[1].uniformHandle.hash = 0x45u;
  const auto uniformTelemetry =
      dxmt9::encoders::measureCompatibleIndexedDrawMergePairs(
          uniformOnly, {});
  const auto uniformIndex =
      dxmt9::encoders::compatibleIndexedDrawMergeRejectIndex(
          CompatibleIndexedDrawMergeReject::Uniform);
  checkEq(uniformTelemetry.pairAttempts, std::uint64_t{1},
          "merge telemetry visits each adjacent draw boundary once");
  checkEq(uniformTelemetry.compatiblePairs, std::uint64_t{0},
          "uniform-only mismatch is not strictly compatible");
  checkEq(uniformTelemetry.rejectPairs[uniformIndex], std::uint64_t{1},
          "uniform mismatch contributes to rejection volume");
  checkEq(uniformTelemetry.onlyRejectPairs[uniformIndex], std::uint64_t{1},
          "uniform-only mismatch contributes to its relaxation frontier");
  checkEq(uniformTelemetry.exactRelaxationSetPairs[static_cast<u32>(
              CompatibleIndexedDrawMergeRelaxation::Uniform)],
          std::uint64_t{1},
          "uniform-only mismatch contributes to logical relaxation set");

  auto uniformAndRange = uniformOnly;
  uniformAndRange[1].startIndex = 6u;
  const auto mixedTelemetry =
      dxmt9::encoders::measureCompatibleIndexedDrawMergePairs(
          uniformAndRange, {});
  const auto rangeIndex =
      dxmt9::encoders::compatibleIndexedDrawMergeRejectIndex(
          CompatibleIndexedDrawMergeReject::NonContiguousIndexRange);
  checkEq(mixedTelemetry.rejectPairs[uniformIndex], std::uint64_t{1},
          "mixed mismatch retains uniform cause attribution");
  checkEq(mixedTelemetry.rejectPairs[rangeIndex], std::uint64_t{1},
          "mixed mismatch retains range cause attribution");
  checkEq(mixedTelemetry.onlyRejectPairs[uniformIndex], std::uint64_t{0},
          "mixed mismatch does not overstate uniform-only frontier");
  checkEq(mixedTelemetry.onlyRejectPairs[rangeIndex], std::uint64_t{0},
          "mixed mismatch does not overstate joined-index-only frontier");
  checkEq(mixedTelemetry.multipleRejectPairs, std::uint64_t{1},
          "multi-condition rejections remain separately visible");
  const auto uniformAndRangeSet =
      static_cast<u32>(CompatibleIndexedDrawMergeRelaxation::Uniform) |
      static_cast<u32>(
          CompatibleIndexedDrawMergeRelaxation::NonContiguousIndexRange);
  checkEq(mixedTelemetry.exactRelaxationSetPairs[uniformAndRangeSet],
          std::uint64_t{1},
          "logical relaxation sets preserve exact multi-condition volume");

  std::array<std::uint8_t, 8> payload{};
  payload[0] = 1u;
  payload[1] = 2u;
  payload[4] = 3u;
  payload[5] = 4u;
  auto bindingPayloadOnly = draws;
  bindingPayloadOnly[0].bindingOverrideRange = {0u, 1u};
  bindingPayloadOnly[1].bindingOverrideRange = {4u, 1u};
  bindingPayloadOnly[0].bindingSnapshotRange = {1u, 1u};
  bindingPayloadOnly[1].bindingSnapshotRange = {5u, 1u};
  const auto bindingTelemetry =
      dxmt9::encoders::measureCompatibleIndexedDrawMergePairs(
          bindingPayloadOnly, payload);
  checkEq(bindingTelemetry.multipleRejectPairs, std::uint64_t{1},
          "two serialized binding ranges remain two raw reject causes");
  checkEq(bindingTelemetry.exactRelaxationSetPairs[static_cast<u32>(
              CompatibleIndexedDrawMergeRelaxation::BindingPayload)],
          std::uint64_t{1},
          "serialized binding ranges form one logical relaxation condition");
}

void testVersionedIndexSnapshotIsStableCacheSource() {
  using dxmt9::core::DrawBufferBindingSnapshot;

  check(dxmt9::encoders::isStableIndexCacheSource(
            /*userIndexDataEmpty=*/true,
            /*sourceRecordExists=*/true,
            /*sourceRecordHasBuffer=*/true,
            /*snapshot=*/nullptr),
        "non-versioned bound index buffer is a stable cache source");

  DrawBufferBindingSnapshot snapshot{
      .metalHandle = 0x7001u,
      .contentsAddress = 0x8000u,
      .byteSize = 4096u,
      .contentRevision = 9u,
  };
  check(dxmt9::encoders::isStableIndexCacheSource(
            /*userIndexDataEmpty=*/true,
            /*sourceRecordExists=*/true,
            /*sourceRecordHasBuffer=*/false,
            &snapshot),
        "versioned snapshot pins an immutable cache source revision");

  check(!dxmt9::encoders::isStableIndexCacheSource(
             /*userIndexDataEmpty=*/false,
             /*sourceRecordExists=*/true,
             /*sourceRecordHasBuffer=*/true,
             &snapshot),
        "UP index data never aliases the logical-buffer cache");

  auto missingBytes = snapshot;
  missingBytes.contentsAddress = 0u;
  check(!dxmt9::encoders::isStableIndexCacheSource(
             /*userIndexDataEmpty=*/true,
             /*sourceRecordExists=*/true,
             /*sourceRecordHasBuffer=*/true,
             &missingBytes),
        "versioned snapshot without readable bytes cannot build a candidate");

  auto missingRevision = snapshot;
  missingRevision.contentRevision = 0u;
  check(!dxmt9::encoders::isStableIndexCacheSource(
             /*userIndexDataEmpty=*/true,
             /*sourceRecordExists=*/true,
             /*sourceRecordHasBuffer=*/true,
             &missingRevision),
        "versioned snapshot without revision cannot key derived bytes");
}

void testExpandedIndexedProgrammableDrawExpandsExtraStreamBytes() {
  constexpr std::size_t kStream0Base = 24u;
  constexpr std::size_t kStream0Stride = 12u;
  constexpr std::size_t kStream1Base = 40u;
  constexpr std::size_t kStream1Stride = 8u;
  const std::array<std::uint16_t, 6> indices{{2u, 0u, 3u, 3u, 1u, 2u}};

  std::array<std::uint8_t, 128> stream0Bytes{};
  std::array<std::uint8_t, 128> stream1Bytes{};
  for (std::size_t i = 0; i < stream0Bytes.size(); ++i) {
    stream0Bytes[i] = static_cast<std::uint8_t>(i & 0xffu);
  }
  for (std::size_t i = 0; i < stream1Bytes.size(); ++i) {
    stream1Bytes[i] = static_cast<std::uint8_t>((0x80u + i) & 0xffu);
  }
  const std::span<const std::uint8_t> indexBytes{
      reinterpret_cast<const std::uint8_t*>(indices.data()),
      indices.size() * sizeof(indices[0])};

  std::vector<std::uint8_t> expanded0;
  std::vector<std::uint8_t> expanded1;
  check(dxmt9::encoders::expandIndexedStreamToFlatVertexBytes(
            stream0Bytes,
            indexBytes,
            IndexType::UInt16,
            0u,
            0,
            indices.size(),
            kStream0Base,
            kStream0Stride,
            expanded0),
        "stream0 indexed expansion succeeds");
  check(dxmt9::encoders::expandIndexedStreamToFlatVertexBytes(
            stream1Bytes,
            indexBytes,
            IndexType::UInt16,
            0u,
            0,
            indices.size(),
            kStream1Base,
            kStream1Stride,
            expanded1),
        "stream1 indexed expansion succeeds");

  checkEq(expanded0.size(), indices.size() * kStream0Stride,
          "expanded stream0 byte count");
  checkEq(expanded1.size(), indices.size() * kStream1Stride,
          "expanded stream1 byte count");
  for (std::size_t i = 0; i < indices.size(); ++i) {
    const std::size_t src0 = kStream0Base + indices[i] * kStream0Stride;
    const std::size_t src1 = kStream1Base + indices[i] * kStream1Stride;
    check(std::memcmp(expanded0.data() + i * kStream0Stride,
                      stream0Bytes.data() + src0,
                      kStream0Stride) == 0,
          "stream0 expanded vertex matches indexed source");
    check(std::memcmp(expanded1.data() + i * kStream1Stride,
                      stream1Bytes.data() + src1,
                      kStream1Stride) == 0,
          "stream1 expanded vertex matches indexed source");
  }
}

void testMixedShaderPathsBindProgrammableDrawInputs() {
  struct Case {
    dxmt9::core::ShaderRef::Kind vertexKind;
    dxmt9::core::ShaderRef::Kind pixelKind;
    const char* label;
  };

  const std::array<Case, 4> cases{{
      {dxmt9::core::ShaderRef::Kind::Bytecode,
       dxmt9::core::ShaderRef::Kind::FixedFunctionPixel,
       "programmable VS plus FFP PS"},
      {dxmt9::core::ShaderRef::Kind::FixedFunctionVertex,
       dxmt9::core::ShaderRef::Kind::Bytecode,
       "FFP VS plus programmable PS"},
      {dxmt9::core::ShaderRef::Kind::None,
       dxmt9::core::ShaderRef::Kind::Bytecode,
       "null VS plus programmable PS"},
      {dxmt9::core::ShaderRef::Kind::Bytecode,
       dxmt9::core::ShaderRef::Kind::None,
       "programmable VS plus null PS"},
  }};

  for (const auto& testCase : cases) {
    Harness harness;
    Capture capture;
    auto recorder = makeRecorder(capture);

    constexpr obj_handle_t kBoundVertex = 0x5200005200007d0ull;
    auto state = makeProgrammableState(20u);
    state.shaderLayout.vertexShader.kind = testCase.vertexKind;
    state.shaderLayout.pixelShader.kind = testCase.pixelKind;
    setFvfXyzTex1(state);
    state.hot.streamBuffers[0] =
        harness.createBoundBuffer(kBoundVertex, 4096u);
    state.hot.streamOffsets[0] = 40u;

    dxmt9::core::DrawParam param{};
    param.primitiveType = PrimitiveType::TriangleList;
    param.primitiveCount = 1u;
    param.startVertex = 2u;
    param.baseVertexIndex = 17;
    param.indexed = false;

    PreUploadedDrawData preUploaded{};
    runEncodeDraw(harness, recorder, state, param, preUploaded, {});

    checkEq(capture.commands.size(), std::size_t{3},
            std::string(testCase.label) + " command count");

    const auto& stream = commandAt(
        capture, 0, std::string(testCase.label) + " stream bind");
    check(stream.kind == RecordedKind::SetVertexBuffer,
          std::string(testCase.label) + " binds vertex stream");
    checkEq(stream.bufferHandle, kBoundVertex,
            std::string(testCase.label) + " stream source");
    checkEq(stream.offset, std::uint64_t{80},
            std::string(testCase.label) + " folds non-indexed startVertex");
    checkEq(static_cast<unsigned>(stream.index), 1u,
            std::string(testCase.label) + " stream slot");

    const auto& volCommand = commandAt(
        capture, 1, std::string(testCase.label) + " DrawVolatile");
    check(volCommand.kind == RecordedKind::SetVertexBytes,
          std::string(testCase.label) + " pushes DrawVolatile");
    const auto vol = volatileBytes(volCommand);
    checkEq(vol.vertexBaseIndex, std::int32_t{0},
            std::string(testCase.label) + " clears non-indexed base vertex");
    checkEq(vol.vertexStreamOffset, std::uint32_t{0},
            std::string(testCase.label) + " stream offset");
    checkEq(vol.vertexStreamStride, std::uint32_t{20},
            std::string(testCase.label) + " stream stride");
    checkEq(vol._pad, std::uint32_t{0},
            std::string(testCase.label) + " DrawVolatile pad");

    assertDrawPrimitivesCommand(
        commandAt(capture, 2, std::string(testCase.label) + " draw"),
        WMTPrimitiveTypeTriangle,
        0u,
        3u);
  }
}

void testProgrammableDrawFvfAndDeclTransitionsDoNotReuseLayout() {
  struct Case {
    bool firstUsesFvf = false;
    const char* label = "";
  };

  const std::array<Case, 2> cases{{
      {true, "FVF then vertex declaration"},
      {false, "vertex declaration then FVF"},
  }};

  for (const auto& testCase : cases) {
    Harness harness;
    Capture capture;
    auto recorder = makeRecorder(capture);

    constexpr obj_handle_t kFirstVertex = 0x5300005300007d1ull;
    constexpr obj_handle_t kSecondVertex = 0x6300006300008d2ull;
    auto first = makeProgrammableState(testCase.firstUsesFvf ? 20u : 28u);
    auto second = makeProgrammableState(testCase.firstUsesFvf ? 28u : 20u);

    if (testCase.firstUsesFvf) {
      setFvfXyzTex1(first);
      setDeclPositionTexcoord(second, 20u, 28u);
    } else {
      setDeclPositionTexcoord(first, 20u, 28u);
      setFvfXyzTex1(second);
    }

    first.hot.streamBuffers[0] = harness.createBoundBuffer(kFirstVertex, 4096u);
    first.hot.streamOffsets[0] = 32u;
    second.hot.streamBuffers[0] =
        harness.createBoundBuffer(kSecondVertex, 4096u);
    second.hot.streamOffsets[0] = 96u;

    dxmt9::core::DrawParam param{};
    param.primitiveType = PrimitiveType::TriangleList;
    param.primitiveCount = 1u;
    param.startVertex = 1u;
    param.indexed = false;

    PreUploadedDrawData preUploaded{};
    runEncodeDraw(harness, recorder, first, param, preUploaded, {});
    runEncodeDraw(harness, recorder, second, param, preUploaded, {});

    checkEq(capture.commands.size(), std::size_t{6},
            std::string(testCase.label) + " command count");

    const u32 firstStride = testCase.firstUsesFvf ? 20u : 28u;
    const u32 secondStride = testCase.firstUsesFvf ? 28u : 20u;

    const auto& firstStream = commandAt(
        capture, 0, std::string(testCase.label) + " first stream");
    check(firstStream.kind == RecordedKind::SetVertexBuffer,
          std::string(testCase.label) + " first draw binds stream");
    checkEq(firstStream.bufferHandle, kFirstVertex,
            std::string(testCase.label) + " first stream source");
    checkEq(firstStream.offset, static_cast<std::uint64_t>(32u + firstStride),
            std::string(testCase.label) + " first layout stride");
    const auto firstVol = volatileBytes(
        commandAt(capture, 1, std::string(testCase.label) + " first volatile"));
    checkEq(firstVol.vertexStreamStride, firstStride,
            std::string(testCase.label) + " first volatile stride");
    assertDrawPrimitivesCommand(
        commandAt(capture, 2, std::string(testCase.label) + " first draw"),
        WMTPrimitiveTypeTriangle,
        0u,
        3u);

    const auto& secondStream = commandAt(
        capture, 3, std::string(testCase.label) + " second stream");
    check(secondStream.kind == RecordedKind::SetVertexBuffer,
          std::string(testCase.label) + " second draw binds stream");
    checkEq(secondStream.bufferHandle, kSecondVertex,
            std::string(testCase.label) + " second stream source");
    checkEq(secondStream.offset,
            static_cast<std::uint64_t>(96u + secondStride),
            std::string(testCase.label) + " second layout stride");
    const auto secondVol = volatileBytes(commandAt(
        capture, 4, std::string(testCase.label) + " second volatile"));
    checkEq(secondVol.vertexStreamStride, secondStride,
            std::string(testCase.label) + " second volatile stride");
    assertDrawPrimitivesCommand(
        commandAt(capture, 5, std::string(testCase.label) + " second draw"),
        WMTPrimitiveTypeTriangle,
        0u,
        3u);
  }
}

void testProgrammableArgbufIndexedDrawKeepsDirectResourceLanes() {
  Harness harness;
  Capture capture;
  auto recorder = makeRecorder(capture);

  constexpr obj_handle_t kVertexBuffer = 0x5400005400007e0ull;
  constexpr obj_handle_t kIndexBuffer = 0x6400006400008f1ull;
  auto state = makeProgrammableState(24u);
  state.hot.streamBuffers[0] =
      harness.createBoundBuffer(kVertexBuffer, 8192u);
  state.hot.streamOffsets[0] = 128u;
  state.hot.indexBuffer = harness.createBoundBuffer(kIndexBuffer, 4096u);

  dxmt9::core::DrawParam param{};
  param.primitiveType = PrimitiveType::TriangleList;
  param.primitiveCount = 2u;
  param.startVertex = 4u;
  param.baseVertexIndex = 3;
  param.startIndex = 5u;
  param.indexType = IndexType::UInt16;
  param.indexed = true;

  PreUploadedDrawData preUploaded{};
  runEncodeDraw(harness, recorder, state, param, preUploaded, {},
                /*skipBaseStateBind=*/true,
                /*argbufHybridMode=*/true);

  checkEq(capture.commands.size(), std::size_t{3},
          "argbuf indexed programmable draw command count");

  const auto& stream = commandAt(capture, 0,
                                 "missing argbuf indexed stream bind");
  check(stream.kind == RecordedKind::SetVertexBuffer,
        "argbuf indexed draw binds vertex stream directly");
  checkEq(stream.bufferHandle, kVertexBuffer,
          "argbuf indexed draw vertex buffer source");
  checkEq(stream.offset, std::uint64_t{128},
          "argbuf indexed draw keeps stream offset");
  checkEq(static_cast<unsigned>(stream.index), 1u,
          "argbuf indexed draw stream slot");

  const auto& volCommand =
      commandAt(capture, 1, "missing argbuf indexed DrawVolatile");
  check(volCommand.kind == RecordedKind::SetVertexBytes,
        "argbuf indexed draw still pushes DrawVolatile bytes");
  checkEq(static_cast<unsigned>(volCommand.index), 5u,
          "argbuf indexed DrawVolatile slot");
  const auto vol = volatileBytes(volCommand);
  checkEq(vol.vertexBaseIndex, std::int32_t{3},
          "argbuf indexed DrawVolatile carries base vertex");
  checkEq(vol.vertexStreamStride, std::uint32_t{24},
          "argbuf indexed DrawVolatile stride");

  assertIndexedDrawCommand(
      commandAt(capture, 2, "missing argbuf indexed draw"),
      kIndexBuffer,
      10u,
      6u);
}

void testProgrammableArgbufMultistreamDrawKeepsDirectResourceLanes() {
  Harness harness;
  Capture capture;
  auto recorder = makeRecorder(capture);

  constexpr obj_handle_t kStream0 = 0x5500005500007e0ull;
  constexpr obj_handle_t kStream1 = 0x6500006500008f1ull;
  auto state = makeProgrammableState(12u);
  state.hot.streamBuffers[0] = harness.createBoundBuffer(kStream0, 8192u);
  state.hot.streamBuffers[1] = harness.createBoundBuffer(kStream1, 8192u);
  state.hot.streamOffsets[0] = 48u;
  state.hot.streamOffsets[1] = 208u;
  state.hot.streamStrides[1] = 16u;
  state.shaderLayout.vertexDecl.elements = {
      dxmt9::core::VertexElement{0, 0, kD3DDeclTypeFloat3,
                                 kD3DDeclMethodDefault,
                                 kD3DDeclUsagePosition, 0},
      dxmt9::core::VertexElement{1, 0, kD3DDeclTypeFloat2,
                                 kD3DDeclMethodDefault,
                                 kD3DDeclUsageTexcoord, 0},
  };
  state.shaderLayout.vertexDecl.streams[1].stride = 16u;

  dxmt9::core::DrawParam param{};
  param.primitiveType = PrimitiveType::TriangleList;
  param.primitiveCount = 1u;
  param.startVertex = 2u;
  param.indexed = false;

  PreUploadedDrawData preUploaded{};
  runEncodeDraw(harness, recorder, state, param, preUploaded, {},
                /*skipBaseStateBind=*/true,
                /*argbufHybridMode=*/true);

  checkEq(capture.commands.size(), std::size_t{4},
          "argbuf multistream programmable draw command count");

  const auto& stream0 = commandAt(capture, 0,
                                  "missing argbuf multistream stream0 bind");
  check(stream0.kind == RecordedKind::SetVertexBuffer,
        "argbuf multistream draw binds stream0 directly");
  checkEq(stream0.bufferHandle, kStream0,
          "argbuf multistream stream0 source");
  checkEq(stream0.offset, std::uint64_t{72},
          "argbuf multistream stream0 folds start vertex");
  checkEq(static_cast<unsigned>(stream0.index), 1u,
          "argbuf multistream stream0 slot");

  const auto& stream1 = commandAt(capture, 1,
                                  "missing argbuf multistream stream1 bind");
  check(stream1.kind == RecordedKind::SetVertexBuffer,
        "argbuf multistream draw binds stream1 directly");
  checkEq(stream1.bufferHandle, kStream1,
          "argbuf multistream stream1 source");
  checkEq(stream1.offset, std::uint64_t{240},
          "argbuf multistream stream1 folds start vertex");
  checkEq(static_cast<unsigned>(stream1.index), 6u,
          "argbuf multistream stream1 generated slot");

  const auto& volCommand =
      commandAt(capture, 2, "missing argbuf multistream DrawVolatile");
  check(volCommand.kind == RecordedKind::SetVertexBytes,
        "argbuf multistream draw still pushes DrawVolatile bytes");
  checkEq(static_cast<unsigned>(volCommand.index), 5u,
          "argbuf multistream DrawVolatile slot");
  const auto vol = volatileBytes(volCommand);
  checkEq(vol.vertexBaseIndex, std::int32_t{0},
          "argbuf multistream DrawVolatile clears base vertex");
  checkEq(vol.vertexStreamStride, std::uint32_t{12},
          "argbuf multistream DrawVolatile primary stride");

  assertDrawPrimitivesCommand(
      commandAt(capture, 3, "missing argbuf multistream draw"),
      WMTPrimitiveTypeTriangle,
      0u,
      3u);
}

void testBoundVertexAndUserIndexOrdering() {
  Harness harness;
  Capture capture;
  auto recorder = makeRecorder(capture);

  constexpr obj_handle_t kBoundVertex = 0x1000001000001b8ull;
  constexpr obj_handle_t kIgnoredBoundIndex = 0x1000001000001c9ull;
  constexpr obj_handle_t kUploadedIndex = 0x2000002000002c9ull;
  auto state = makeProgrammableState(24u);
  state.hot.indexBuffer.value = kIgnoredBoundIndex;
  state.hot.streamBuffers[0] = harness.createBoundBuffer(kBoundVertex, 8192u);
  state.hot.streamOffsets[0] = 3712u;

  std::array<std::uint8_t, 12> arena{};
  dxmt9::core::DrawParam param{};
  param.primitiveType = PrimitiveType::TriangleList;
  param.primitiveCount = 2u;
  param.baseVertexIndex = -3;
  param.startIndex = 2u;
  param.indexType = IndexType::UInt16;
  param.indexed = true;
  param.userIndexRange = dxmt9::core::DrawPayloadRange{0u, arena.size()};

  PreUploadedDrawData preUploaded{};
  preUploaded.index.buffer.handle = kUploadedIndex;
  preUploaded.index.offset = 96u;
  preUploaded.index.size = arena.size();

  runEncodeDraw(harness, recorder, state, param, preUploaded, arena);

  checkEq(capture.commands.size(), std::size_t{3},
          "bound-vertex/user-index draw command count");
  const auto& stream = commandAt(capture, 0, "missing stream bind");
  check(stream.kind == RecordedKind::SetVertexBuffer,
        "first command binds vertex stream");
  checkEq(stream.bufferHandle, kBoundVertex,
          "bound vertex buffer is selected over UP source");
  checkEq(stream.offset, std::uint64_t{3712},
          "bound vertex stream offset is preserved");
  checkEq(static_cast<unsigned>(stream.index), 1u,
          "vertex stream binds to Metal slot 1");

  const auto& volCommand = commandAt(capture, 1, "missing DrawVolatile");
  check(volCommand.kind == RecordedKind::SetVertexBytes,
        "second command pushes DrawVolatile");
  checkEq(static_cast<unsigned>(volCommand.index), 5u,
          "DrawVolatile binds to Metal slot 5");
  const auto vol = volatileBytes(volCommand);
  checkEq(vol.vertexBaseIndex, std::int32_t{-3},
          "DrawVolatile carries indexed base vertex");
  checkEq(vol.vertexStreamOffset, std::uint32_t{0},
          "DrawVolatile stream offset");
  checkEq(vol.vertexStreamStride, std::uint32_t{24},
          "DrawVolatile stream stride");
  checkEq(vol._pad, std::uint32_t{0}, "DrawVolatile pad");

  assertIndexedDrawCommand(commandAt(capture, 2, "missing indexed draw"),
                           kUploadedIndex,
                           100u,
                           6u);
}

void testBoundVertexAndBoundIndexOrdering() {
  Harness harness;
  Capture capture;
  auto recorder = makeRecorder(capture);

  constexpr obj_handle_t kBoundVertex = 0x5000005000005acull;
  constexpr obj_handle_t kBoundIndex = 0x6000006000006bdull;
  constexpr std::uint32_t kStartIndex = 7u;
  auto state = makeProgrammableState(28u);
  state.hot.streamBuffers[0] = harness.createBoundBuffer(kBoundVertex, 16384u);
  state.hot.streamOffsets[0] = 128u;
  state.hot.indexBuffer = harness.createBoundBuffer(kBoundIndex, 4096u);

  std::array<std::uint8_t, 1> arena{};
  dxmt9::core::DrawParam param{};
  param.primitiveType = PrimitiveType::TriangleList;
  param.primitiveCount = 3u;
  param.baseVertexIndex = 4;
  param.startIndex = kStartIndex;
  param.indexType = IndexType::UInt16;
  param.indexed = true;

  PreUploadedDrawData preUploaded{};

  runEncodeDraw(harness, recorder, state, param, preUploaded, arena);

  checkEq(capture.commands.size(), std::size_t{3},
          "bound-vertex/bound-index draw command count");
  const auto& stream = commandAt(capture, 0, "missing bound stream bind");
  check(stream.kind == RecordedKind::SetVertexBuffer,
        "first command binds bound vertex stream");
  checkEq(stream.bufferHandle, kBoundVertex,
          "bound vertex buffer remains stream source");
  checkEq(stream.offset, std::uint64_t{128},
          "bound vertex stream offset is preserved");
  checkEq(static_cast<unsigned>(stream.index), 1u,
          "bound vertex stream binds to Metal slot 1");

  const auto& volCommand = commandAt(capture, 1,
                                     "missing bound-index DrawVolatile");
  check(volCommand.kind == RecordedKind::SetVertexBytes,
        "second command pushes bound-index DrawVolatile");
  checkEq(static_cast<unsigned>(volCommand.index), 5u,
          "bound-index DrawVolatile binds to Metal slot 5");
  const auto vol = volatileBytes(volCommand);
  checkEq(vol.vertexBaseIndex, std::int32_t{4},
          "bound-index DrawVolatile carries base vertex");
  checkEq(vol.vertexStreamOffset, std::uint32_t{0},
          "bound-index DrawVolatile stream offset");
  checkEq(vol.vertexStreamStride, std::uint32_t{28},
          "bound-index DrawVolatile stream stride");
  checkEq(vol._pad, std::uint32_t{0}, "bound-index DrawVolatile pad");

  assertIndexedDrawCommand(commandAt(capture, 2,
                                     "missing bound-index indexed draw"),
                           kBoundIndex,
                           static_cast<std::uint64_t>(kStartIndex) *
                               sizeof(std::uint16_t),
                           9u);
}

void testUserVertexAndUserIndexOrdering() {
  Harness harness;
  Capture capture;
  auto recorder = makeRecorder(capture);

  constexpr obj_handle_t kIgnoredBoundVertex = 0x3000003000003c9ull;
  constexpr obj_handle_t kUploadedVertex = 0x3000003000003daull;
  constexpr obj_handle_t kIgnoredBoundIndex = 0x4000004000004daull;
  constexpr obj_handle_t kUploadedIndex = 0x4000004000004ebull;
  auto state = makeProgrammableState(32u);
  state.hot.streamBuffers[0].value = kIgnoredBoundVertex;
  state.hot.streamOffsets[0] = 12u;
  state.hot.indexBuffer.value = kIgnoredBoundIndex;

  std::array<std::uint8_t, 102> arena{};
  dxmt9::core::DrawParam param{};
  param.primitiveType = PrimitiveType::TriangleList;
  param.primitiveCount = 1u;
  param.baseVertexIndex = 5;
  param.startIndex = 1u;
  param.indexType = IndexType::UInt16;
  param.indexed = true;
  param.userVertexRange = dxmt9::core::DrawPayloadRange{0u, 96u};
  param.userIndexRange = dxmt9::core::DrawPayloadRange{96u, 6u};

  PreUploadedDrawData preUploaded{};
  preUploaded.vertex.buffer.handle = kUploadedVertex;
  preUploaded.vertex.offset = 256u;
  preUploaded.vertex.size = 96u;
  preUploaded.index.buffer.handle = kUploadedIndex;
  preUploaded.index.offset = 512u;
  preUploaded.index.size = 6u;

  runEncodeDraw(harness, recorder, state, param, preUploaded, arena);

  checkEq(capture.commands.size(), std::size_t{3},
          "UP vertex/user-index draw command count");
  const auto& stream = commandAt(capture, 0, "missing UP stream bind");
  check(stream.kind == RecordedKind::SetVertexBuffer,
        "first command binds uploaded UP vertex stream");
  checkEq(stream.bufferHandle, kUploadedVertex,
          "UP vertex upload slice is selected as stream source");
  checkEq(stream.offset, std::uint64_t{268},
          "UP vertex offset includes stream offset");
  checkEq(static_cast<unsigned>(stream.index), 1u,
          "UP vertex stream binds to Metal slot 1");

  const auto& volCommand = commandAt(capture, 1, "missing UP DrawVolatile");
  check(volCommand.kind == RecordedKind::SetVertexBytes,
        "second command pushes UP DrawVolatile");
  const auto vol = volatileBytes(volCommand);
  checkEq(vol.vertexBaseIndex, std::int32_t{5},
          "UP DrawVolatile carries base vertex");
  checkEq(vol.vertexStreamOffset, std::uint32_t{0},
          "UP DrawVolatile stream offset");
  checkEq(vol.vertexStreamStride, std::uint32_t{32},
          "UP DrawVolatile stream stride");
  checkEq(vol._pad, std::uint32_t{0}, "UP DrawVolatile pad");

  assertIndexedDrawCommand(commandAt(capture, 2, "missing UP indexed draw"),
                           kUploadedIndex,
                           514u,
                           3u);
}

void testPreUploadedIndexedUserVertexIgnoresStartVertex() {
  Harness harness;
  Capture capture;
  auto recorder = makeRecorder(capture);

  constexpr obj_handle_t kIgnoredBoundVertex = 0x5600005600005acull;
  constexpr obj_handle_t kUploadedVertex = 0x6600006600006bdull;
  constexpr obj_handle_t kUploadedIndex = 0x7600007600007ceull;
  auto state = makeProgrammableState(20u);
  state.hot.streamBuffers[0] =
      harness.createBoundBuffer(kIgnoredBoundVertex, 4096u);
  state.hot.streamOffsets[0] = 36u;

  std::array<std::uint8_t, 86> arena{};
  dxmt9::core::DrawParam param{};
  param.primitiveType = PrimitiveType::TriangleList;
  param.primitiveCount = 2u;
  param.startVertex = 13u;
  param.baseVertexIndex = -7;
  param.startIndex = 4u;
  param.indexType = IndexType::UInt16;
  param.indexed = true;
  param.userVertexRange = dxmt9::core::DrawPayloadRange{0u, 80u};
  param.userIndexRange = dxmt9::core::DrawPayloadRange{80u, 6u};

  PreUploadedDrawData preUploaded{};
  preUploaded.vertex.buffer.handle = kUploadedVertex;
  preUploaded.vertex.offset = 300u;
  preUploaded.vertex.size = 80u;
  preUploaded.index.buffer.handle = kUploadedIndex;
  preUploaded.index.offset = 900u;
  preUploaded.index.size = 6u;

  runEncodeDraw(harness, recorder, state, param, preUploaded, arena);

  checkEq(capture.commands.size(), std::size_t{3},
          "preuploaded indexed UP draw command count");

  const auto& stream =
      commandAt(capture, 0, "missing preuploaded indexed UP stream bind");
  check(stream.kind == RecordedKind::SetVertexBuffer,
        "first command binds preuploaded indexed UP vertex stream");
  checkEq(stream.bufferHandle, kUploadedVertex,
          "indexed UP vertex upload slice wins over stale bound stream0");
  checkEq(stream.offset, std::uint64_t{336},
          "indexed UP startVertex does not fold into vertex buffer offset");
  checkEq(static_cast<unsigned>(stream.index), 1u,
          "indexed UP vertex stream binds to Metal slot 1");

  const auto& volCommand =
      commandAt(capture, 1, "missing preuploaded indexed UP DrawVolatile");
  check(volCommand.kind == RecordedKind::SetVertexBytes,
        "second command pushes indexed UP DrawVolatile");
  const auto vol = volatileBytes(volCommand);
  checkEq(vol.vertexBaseIndex, std::int32_t{-7},
          "indexed UP DrawVolatile carries base vertex");
  checkEq(vol.vertexStreamOffset, std::uint32_t{0},
          "indexed UP DrawVolatile stream offset");
  checkEq(vol.vertexStreamStride, std::uint32_t{20},
          "indexed UP DrawVolatile primary stride");
  checkEq(vol._pad, std::uint32_t{0}, "indexed UP DrawVolatile pad");

  assertIndexedDrawCommand(
      commandAt(capture, 2, "missing preuploaded indexed UP draw"),
      kUploadedIndex,
      908u,
      6u);
}

void testPreUploadedNonIndexedUserVertexFoldsStartVertex() {
  Harness harness;
  Capture capture;
  auto recorder = makeRecorder(capture);

  constexpr obj_handle_t kIgnoredBoundVertex = 0x5400005400005acull;
  constexpr obj_handle_t kUploadedVertex = 0x6400006400006bdull;
  auto state = makeProgrammableState(16u);
  state.hot.streamBuffers[0] = harness.createBoundBuffer(kIgnoredBoundVertex,
                                                         4096u);
  state.hot.streamOffsets[0] = 24u;

  std::array<std::uint8_t, 128> arena{};
  dxmt9::core::DrawParam param{};
  param.primitiveType = PrimitiveType::TriangleStrip;
  param.primitiveCount = 3u;
  param.startVertex = 4u;
  param.baseVertexIndex = 77;
  param.indexed = false;
  param.userVertexRange = dxmt9::core::DrawPayloadRange{0u, arena.size()};

  PreUploadedDrawData preUploaded{};
  preUploaded.vertex.buffer.handle = kUploadedVertex;
  preUploaded.vertex.offset = 320u;
  preUploaded.vertex.size = arena.size();

  runEncodeDraw(harness, recorder, state, param, preUploaded, arena);

  checkEq(capture.commands.size(), std::size_t{3},
          "preuploaded non-indexed UP draw command count");

  const auto& stream = commandAt(capture, 0,
                                 "missing preuploaded UP stream bind");
  check(stream.kind == RecordedKind::SetVertexBuffer,
        "first command binds preuploaded UP vertex stream");
  checkEq(stream.bufferHandle, kUploadedVertex,
          "preuploaded UP vertex slice is selected over bound stream0");
  checkEq(stream.offset, std::uint64_t{408},
          "non-indexed UP startVertex folds into uploaded vertex offset");
  checkEq(static_cast<unsigned>(stream.index), 1u,
          "preuploaded UP vertex stream binds to Metal slot 1");

  const auto& volCommand =
      commandAt(capture, 1, "missing preuploaded UP DrawVolatile");
  check(volCommand.kind == RecordedKind::SetVertexBytes,
        "second command pushes preuploaded UP DrawVolatile");
  const auto vol = volatileBytes(volCommand);
  checkEq(vol.vertexBaseIndex, std::int32_t{0},
          "non-indexed UP DrawVolatile clears stale base vertex");
  checkEq(vol.vertexStreamOffset, std::uint32_t{0},
          "non-indexed UP DrawVolatile stream offset");
  checkEq(vol.vertexStreamStride, std::uint32_t{16},
          "non-indexed UP DrawVolatile primary stride");
  checkEq(vol._pad, std::uint32_t{0}, "non-indexed UP DrawVolatile pad");

  assertDrawPrimitivesCommand(
      commandAt(capture, 2, "missing preuploaded UP drawPrimitives"),
      WMTPrimitiveTypeTriangleStrip,
      0u,
      5u);
}

void testStreamSnapshotOverridesLiveBufferHandle() {
  Harness harness;
  Capture capture;
  auto recorder = makeRecorder(capture);

  constexpr obj_handle_t kLiveVertex = 0x7d0000000000101ull;
  constexpr obj_handle_t kSnapshotVertex = 0x7d0000000000102ull;

  auto state = makeProgrammableState(20u);
  setDeclPositionTexcoord(state, 12u, 20u);
  state.hot.streamBuffers[0] = harness.createBoundBuffer(kLiveVertex, 4096u);
  state.hot.streamOffsets[0] = 32u;
  state.hot.streamStrides[0] = 20u;

  std::array<std::uint8_t, 128> vertexBytes{};
  dxmt9::core::DrawBindingSnapshot binding{};
  binding.streamMask = 1u << 0u;
  binding.streams[0].buffer = state.hot.streamBuffers[0];
  binding.streams[0].offset = state.hot.streamOffsets[0];
  binding.streams[0].stride = state.hot.streamStrides[0];
  binding.streams[0].snapshot = dxmt9::core::DrawBufferBindingSnapshot{
      .metalHandle = kSnapshotVertex,
      .contentsAddress = static_cast<std::uint64_t>(
          reinterpret_cast<std::uintptr_t>(vertexBytes.data())),
      .byteSize = vertexBytes.size(),
      .contentRevision = 3u,
  };

  dxmt9::core::DrawParam param{};
  param.primitiveType = PrimitiveType::TriangleList;
  param.primitiveCount = 1u;
  param.indexed = false;

  PreUploadedDrawData preUploaded{};
  std::array<std::uint8_t, 1> arena{};
  runEncodeDraw(harness, recorder, state, param, preUploaded, arena,
                /*skipBaseStateBind=*/true,
                /*argbufHybridMode=*/false,
                /*textureSamplerShadow=*/nullptr,
                &binding);

  const auto& stream =
      firstVertexBufferBind(capture, 1u, "missing snapshot stream0 bind");
  checkEq(stream.bufferHandle, kSnapshotVertex,
          "stream snapshot overrides live BufferRecord handle");
  checkEq(stream.offset, std::uint64_t{32},
          "stream snapshot preserves logical stream offset");
}

void testIndexSnapshotOverridesLiveBufferHandle() {
  Harness harness;
  Capture capture;
  auto recorder = makeRecorder(capture);

  constexpr obj_handle_t kLiveVertex = 0x7d0000000000201ull;
  constexpr obj_handle_t kLiveIndex = 0x7d0000000000202ull;
  constexpr obj_handle_t kSnapshotIndex = 0x7d0000000000203ull;

  auto state = makeProgrammableState(20u);
  setDeclPositionTexcoord(state, 12u, 20u);
  state.hot.streamBuffers[0] = harness.createBoundBuffer(kLiveVertex, 4096u);
  state.hot.streamOffsets[0] = 0u;
  state.hot.streamStrides[0] = 20u;
  state.hot.indexBuffer = harness.createBoundBuffer(kLiveIndex, 4096u);

  std::array<std::uint8_t, 64> indexBytes{};
  dxmt9::core::DrawBindingSnapshot binding{};
  binding.indexBuffer = state.hot.indexBuffer;
  binding.indexType = IndexType::UInt16;
  binding.indexSnapshot = dxmt9::core::DrawBufferBindingSnapshot{
      .metalHandle = kSnapshotIndex,
      .contentsAddress = static_cast<std::uint64_t>(
          reinterpret_cast<std::uintptr_t>(indexBytes.data())),
      .byteSize = indexBytes.size(),
      .contentRevision = 5u,
  };
  binding.indexSnapshotValid = true;

  dxmt9::core::DrawParam param{};
  param.primitiveType = PrimitiveType::TriangleList;
  param.primitiveCount = 1u;
  param.startIndex = 2u;
  param.indexType = IndexType::UInt16;
  param.indexed = true;

  PreUploadedDrawData preUploaded{};
  std::array<std::uint8_t, 1> arena{};
  runEncodeDraw(harness, recorder, state, param, preUploaded, arena,
                /*skipBaseStateBind=*/true,
                /*argbufHybridMode=*/false,
                /*textureSamplerShadow=*/nullptr,
                &binding);

  const auto& draw = firstIndexedDraw(capture, "missing snapshot indexed draw");
  checkEq(draw.bufferHandle, kSnapshotIndex,
          "index snapshot overrides live BufferRecord handle");
  checkEq(draw.offset, std::uint64_t{4},
          "index snapshot preserves startIndex byte offset");
}

void testBindingOverrideBaseStateRebindPolicy() {
  dxmt9::core::DrawShaderLayoutContext layout{};
  layout.vertexDecl.streams[0].stride = 24u;
  layout.vertexDecl.streams[1].stride = 32u;
  layout.vertexDecl.streams[2].stride = 16u;

  dxmt9::core::DrawBindingOverride binding{};
  binding.streamMask = 1u << 0u;
  binding.streams[0].buffer = dxmt9::core::Handle{0x4100u};
  binding.streams[0].offset = 256u;
  binding.streams[0].stride = 48u;
  check(!dxmt9::encoders::drawBindingOverrideRequiresBaseStateBind(
            binding, &layout),
        "stream0 override does not require base-state rebind");

  binding = {};
  binding.indexBufferValid = true;
  binding.indexBuffer = dxmt9::core::Handle{0x5100u};
  binding.indexType = IndexType::UInt32;
  check(!dxmt9::encoders::drawBindingOverrideRequiresBaseStateBind(
            binding, &layout),
        "IB-only override does not require base-state rebind");

  binding = {};
  binding.streamMask = 1u << 1u;
  binding.streams[1].buffer = dxmt9::core::Handle{0x4200u};
  binding.streams[1].offset = 512u;
  binding.streams[1].stride = 32u;
  check(!dxmt9::encoders::drawBindingOverrideRequiresBaseStateBind(
            binding, &layout),
        "extra-stream handle/offset override with same stride skips base state");

  binding.streams[1].stride = 40u;
  check(dxmt9::encoders::drawBindingOverrideRequiresBaseStateBind(
            binding, &layout),
        "extra-stream stride override requires base-state rebind");

  binding = {};
  binding.streamMask = 1u << 2u;
  binding.streams[2].stride = 16u;
  check(!dxmt9::encoders::drawBindingOverrideRequiresBaseStateBind(
            binding, nullptr),
        "missing shader layout does not force base-state rebind");

  dxmt9::core::DrawShaderLayoutContext bindingAgnosticLayout{};
  binding = {};
  binding.streamMask = 1u << 0u;
  binding.streams[0].buffer = dxmt9::core::Handle{0x4300u};
  binding.streams[0].offset = 128u;
  binding.streams[0].stride = 64u;
  check(!dxmt9::encoders::drawBindingOverrideRequiresBaseStateBind(
            binding, &bindingAgnosticLayout),
        "binding-agnostic stream0 override can reuse prefetched PSO");

  binding = {};
  binding.streamMask = 1u << 1u;
  binding.streams[1].buffer = dxmt9::core::Handle{0x4400u};
  binding.streams[1].offset = 256u;
  binding.streams[1].stride = 32u;
  check(dxmt9::encoders::drawBindingOverrideRequiresBaseStateBind(
            binding, &bindingAgnosticLayout),
        "binding-agnostic extra-stream stride override still needs live PSO lookup");
}

}  // namespace

int main() {
  try {
    testBaseStateRecorderCapturesRasterTextureSamplerOrdering();
    testBlendFactorBindsMetalBlendColor();
    testArgbufModeKeepsDirectTextureSamplerBinds();
    testArgbufModeKeepsDirectVertexTextureSamplerBinds();
    testTextureSamplerShadowDedupsDirectFragmentAndVertexBinds();
    testTextureSamplerShadowResetForcesDirectRebind();
    testNonIndexedDrawPrimitivesAbsorbsStartVertexIntoOffset();
    testFsVolatileAlphaTestImmediatePushShadowAndOverride();
    testNonIndexedDrawIgnoresStaleIndexIntent();
    testIndexedPrimitiveTopologyAndIndexWidthBoundaries();
    testNonIndexedPrimitiveTopologyVertexCounts();
    testProgrammableVsBindsExtraBoundStreamBeforeDraw();
    testProgrammableVsSkipsMissingExtraStreamWithoutStaleBind();
    testIndexedProgrammableDrawPreservesSparseStreamOffsets();
    testProgrammableIndexedBlendHeuristicStaysDirect();
    testAutoExpandIndexedDrawHeuristicCoversProgrammableR32FCube();
    testScreenBlendIndexOrderOptimizationPredicateIsStrict();
    testOpaqueDepthIndexOrderOptimizationPredicateIsStrict();
    testCompatibleIndexedDrawMergePreservesContiguousIndexOrder();
    testCompatibleIndexedDrawMergeRejectsObservableDifferences();
    testCompatibleIndexedDrawMergeTelemetryClassifiesRelaxationSets();
    testVersionedIndexSnapshotIsStableCacheSource();
    testExpandedIndexedProgrammableDrawExpandsExtraStreamBytes();
    testMixedShaderPathsBindProgrammableDrawInputs();
    testProgrammableDrawFvfAndDeclTransitionsDoNotReuseLayout();
    testProgrammableArgbufIndexedDrawKeepsDirectResourceLanes();
    testProgrammableArgbufMultistreamDrawKeepsDirectResourceLanes();
    testBoundVertexAndUserIndexOrdering();
    testBoundVertexAndBoundIndexOrdering();
    testUserVertexAndUserIndexOrdering();
    testPreUploadedIndexedUserVertexIgnoresStartVertex();
    testPreUploadedNonIndexedUserVertexFoldsStartVertex();
    testStreamSnapshotOverridesLiveBufferHandle();
    testIndexSnapshotOverridesLiveBufferHandle();
    testBindingOverrideBaseStateRebindPolicy();
  } catch (const TestFailure& e) {
    std::cerr << "encode_draw_recorder_spec failed: " << e.what() << '\n';
    return EXIT_FAILURE;
  } catch (const std::exception& e) {
    std::cerr << "encode_draw_recorder_spec unexpected exception: "
              << e.what() << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "encode_draw_recorder_spec passed\n";
  return EXIT_SUCCESS;
}
