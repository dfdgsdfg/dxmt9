// encodeDraw recorder seam coverage.
//
// These tests drive encoders::encodeDraw itself, with Metal calls suppressed
// after recording, so the checks include UP-vs-bound source selection,
// DrawVolatile bytes, and command ordering at the draw-issue boundary.

#include "../../../src/dxmt9/dxmt9_draw_encoder.hpp"
#include "../../../src/dxmt9/dxmt9_draw_state.hpp"
#include "../../../src/dxmt9/dxmt9_pipeline_cache.hpp"
#include "../../../src/dxmt9/dxmt9_resource_pool.hpp"
#include "../../../src/dxmt9/dxmt9_ring_arena.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
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
using dxmt9::state::DrawVolatile;

enum class RecordedKind {
  SetRenderPipelineState,
  SetDepthStencilState,
  SetViewport,
  SetScissorRect,
  SetRasterizerState,
  SetFragmentTexture,
  SetFragmentSamplerState,
  SetVertexBuffer,
  SetVertexBytes,
  DrawPrimitives,
  DrawIndexedPrimitives,
};

struct RecordedCommand {
  RecordedKind kind = RecordedKind::SetVertexBuffer;
  obj_handle_t bufferHandle = 0;
  std::uint64_t offset = 0;
  std::uint8_t index = 0;
  std::array<std::uint8_t, 64> bytes{};
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

const RecordedCommand& commandAt(const Capture& capture,
                                 std::size_t index,
                                 std::string_view message) {
  if (index >= capture.commands.size()) {
    fail(std::string(message));
  }
  return capture.commands[index];
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

void recordDrawPrimitives(void* userdata,
                          WMTPrimitiveType primitiveType,
                          std::uint64_t vertexStart,
                          std::uint64_t vertexCount) {
  auto* capture = static_cast<Capture*>(userdata);
  RecordedCommand command{};
  command.kind = RecordedKind::DrawPrimitives;
  command.primitiveType = primitiveType;
  command.start = vertexStart;
  command.count = vertexCount;
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
      .setViewport = recordSetViewport,
      .setScissorRect = recordSetScissorRect,
      .setRasterizerState = recordSetRasterizerState,
      .setFragmentTexture = recordSetFragmentTexture,
      .setFragmentSamplerState = recordSetFragmentSamplerState,
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

  Harness() : queue(WMT::Device{}, limits) {}

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
                              std::uint64_t indexCount) {
  check(command.kind == RecordedKind::DrawIndexedPrimitives,
        "third command is drawIndexedPrimitives");
  checkEq(static_cast<unsigned>(command.primitiveType),
          static_cast<unsigned>(WMTPrimitiveTypeTriangle),
          "draw primitive type");
  checkEq(static_cast<unsigned>(command.indexType),
          static_cast<unsigned>(WMTIndexTypeUInt16),
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
}

void runEncodeDraw(Harness& harness,
                   EncodeDrawRecorder& recorder,
                   dxmt9::core::CanonicalDrawState& state,
                   const dxmt9::core::DrawParam& param,
                   const PreUploadedDrawData& preUploaded,
                   std::span<const std::uint8_t> arena,
                   bool skipBaseStateBind = true) {
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
      &cleanDirty);

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
  state.hot.renderStates.entries[0] = dxmt9::core::FlatStateEntry{
      dxmt9::core::RS_CULL_MODE,
      static_cast<u32>(dxmt9::core::CullMode::Ccw)};
  state.hot.renderStates.count = 1u;
  state.hot.textures[0] = harness.createBoundTexture(kTexture0, 64u, 64u);
  state.hot.textures[1] = harness.createBoundTexture(kTexture1, 32u, 32u);
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
          static_cast<unsigned>(WMTTriangleFillModeFill),
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
  checkEq(stream1.offset, std::uint64_t{192},
          "stream1 offset folds non-indexed start vertex");
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

  assertDrawPrimitivesCommand(
      commandAt(capture, 3, "missing multi-stream drawPrimitives"),
      WMTPrimitiveTypeTriangle,
      0u,
      6u);
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

}  // namespace

int main() {
  try {
    testBaseStateRecorderCapturesRasterTextureSamplerOrdering();
    testNonIndexedDrawPrimitivesAbsorbsStartVertexIntoOffset();
    testProgrammableVsBindsExtraBoundStreamBeforeDraw();
    testBoundVertexAndUserIndexOrdering();
    testBoundVertexAndBoundIndexOrdering();
    testUserVertexAndUserIndexOrdering();
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
