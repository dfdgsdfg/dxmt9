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
#include <vector>

namespace {

using u32 = std::uint32_t;
using dxmt9::core::IndexType;
using dxmt9::core::PrimitiveType;
using dxmt9::encoders::EncodeDrawRecorder;
using dxmt9::encoders::PreUploadedDrawData;
using dxmt9::state::DrawVolatile;

enum class RecordedKind {
  SetVertexBuffer,
  SetVertexBytes,
  DrawPrimitives,
  DrawIndexedPrimitives,
};

struct RecordedCommand {
  RecordedKind kind = RecordedKind::SetVertexBuffer;
  WMT::Buffer buffer{};
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

void recordSetVertexBuffer(void* userdata,
                           WMT::Buffer buffer,
                           std::uint64_t offset,
                           std::uint8_t index) {
  auto* capture = static_cast<Capture*>(userdata);
  RecordedCommand command{};
  command.kind = RecordedKind::SetVertexBuffer;
  command.buffer = buffer;
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
  command.buffer = indexBuffer;
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
  std::vector<dxmt9::core::BufferHandle> patchedBuffers;

  Harness() : queue(WMT::Device{}, limits) {}

  ~Harness() {
    for (const auto handle : patchedBuffers) {
      if (auto* record = pool.findBuffer(handle.value)) {
        record->buffer.handle = 0;
      }
    }
  }

  dxmt9::core::BufferHandle createBoundBuffer(obj_handle_t metalHandle,
                                              std::uint64_t size) {
    dxmt9::core::BufferDesc desc{};
    desc.size = size;
    desc.pool = dxmt9::core::Pool::Default;
    auto handle = pool.createBuffer(WMT::Device{}, desc);
    auto* record = pool.findBuffer(handle.value);
    check(record != nullptr, "pool creates a bound-buffer record");
    record->buffer.handle = metalHandle;
    patchedBuffers.push_back(handle);
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
  checkEq(command.buffer.handle, indexBuffer, "draw index buffer source");
  checkEq(command.offset, indexOffset, "draw index buffer offset");
  checkEq(command.instanceCount, std::uint32_t{1}, "draw instance count");
  checkEq(command.baseVertex, std::int32_t{0}, "Metal draw base vertex");
  checkEq(command.baseInstance, std::uint32_t{0}, "Metal draw base instance");
}

void runEncodeDraw(Harness& harness,
                   EncodeDrawRecorder& recorder,
                   dxmt9::core::CanonicalDrawState& state,
                   const dxmt9::core::DrawParam& param,
                   const PreUploadedDrawData& preUploaded,
                   std::span<const std::uint8_t> arena) {
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
      true,
      &preUploaded,
      &param,
      arena,
      &cleanDirty);

  check(encoded, "encodeDraw emits a draw");
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
  checkEq(stream.buffer.handle, kBoundVertex,
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
  checkEq(stream.buffer.handle, kUploadedVertex,
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
    testBoundVertexAndUserIndexOrdering();
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
