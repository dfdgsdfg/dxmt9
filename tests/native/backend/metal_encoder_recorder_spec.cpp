// Metal encoder recorder seam for final WMT render command arguments.
//
// The test overrides MTLRenderCommandEncoder_encodeCommands, then drives the
// C++ RenderCommandEncoder wrapper directly. This captures the final
// wmtcmd_render_draw_indexed payload that the unix provider would translate
// into [MTLRenderCommandEncoder drawIndexedPrimitives:...].

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#define WINEMETAL_API extern "C"

#include "winemetal/Metal.hpp"

namespace {

struct CapturedRenderCommand {
  obj_handle_t encoder = 0;
  WMTRenderCommandType type = WMTRenderCommandNop;
  wmtcmd_render_setbuffer setBuffer{};
  wmtcmd_render_draw_indexed drawIndexed{};
};

std::vector<CapturedRenderCommand> g_commands;

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

void resetCapture() {
  g_commands.clear();
}

const CapturedRenderCommand& commandAt(std::size_t index,
                                       std::string_view message) {
  if (index >= g_commands.size()) {
    fail(std::string(message));
  }
  return g_commands[index];
}

void test3dmark05FanFixShapeReachesDrawIndexedPrimitives() {
  resetCapture();

  WMT::RenderCommandEncoder encoder;
  encoder.handle = static_cast<obj_handle_t>(0xE005u);

  WMT::Buffer vertexBuffer;
  vertexBuffer.handle = static_cast<obj_handle_t>(0x1000001000001b8ull);
  encoder.setVertexBuffer(vertexBuffer, 3712u, 1u);

  WMT::Buffer indexBuffer;
  indexBuffer.handle = static_cast<obj_handle_t>(0x1A2B3C4Dull);
  encoder.drawIndexedPrimitives(WMTPrimitiveTypeTriangle,
                                WMTIndexTypeUInt16,
                                6u,
                                indexBuffer,
                                0u,
                                1u,
                                0,
                                0u);

  checkEq(g_commands.size(), std::size_t{2},
          "recorder captures vertex buffer bind and indexed draw");

  const auto& streamBind =
      commandAt(0, "missing setVertexBuffer command");
  checkEq(streamBind.encoder, encoder.handle,
          "setVertexBuffer targets the active encoder");
  checkEq(static_cast<unsigned>(streamBind.type),
          static_cast<unsigned>(WMTRenderCommandSetVertexBuffer),
          "stream bind emits set vertex buffer command");
  checkEq(streamBind.setBuffer.buffer, vertexBuffer.handle,
          "stream bind forwards vertex buffer handle");
  checkEq(streamBind.setBuffer.offset, uint64_t{3712},
          "stream bind forwards 3DMark05 stream offset");
  checkEq(static_cast<unsigned>(streamBind.setBuffer.index), 1u,
          "stream bind forwards Metal vertex slot");

  const auto& draw = commandAt(1, "missing drawIndexedPrimitives command");
  checkEq(draw.encoder, encoder.handle,
          "drawIndexedPrimitives targets the active encoder");
  checkEq(static_cast<unsigned>(draw.type),
          static_cast<unsigned>(WMTRenderCommandDrawIndexed),
          "drawIndexedPrimitives emits indexed draw command");
  checkEq(static_cast<unsigned>(draw.drawIndexed.primitive_type),
          static_cast<unsigned>(WMTPrimitiveTypeTriangle),
          "3DMark05 fan fix reaches Metal as triangle primitives");
  checkEq(static_cast<unsigned>(draw.drawIndexed.index_type),
          static_cast<unsigned>(WMTIndexTypeUInt16),
          "3DMark05 fan fix reaches Metal with generated u16 indices");
  checkEq(draw.drawIndexed.index_count, uint64_t{6},
          "two triangle-list primitives issue six indices");
  checkEq(draw.drawIndexed.index_buffer, indexBuffer.handle,
          "drawIndexedPrimitives forwards transient/generated index buffer");
  checkEq(draw.drawIndexed.index_buffer_offset, uint64_t{0},
          "generated fan index payload starts at offset zero");
  checkEq(draw.drawIndexed.instance_count, uint32_t{1},
          "drawIndexedPrimitives uses one instance");
  checkEq(draw.drawIndexed.base_vertex, int32_t{0},
          "3DMark05 fan fix uses zero base vertex");
  checkEq(draw.drawIndexed.base_instance, uint32_t{0},
          "drawIndexedPrimitives uses zero base instance");
}

void testDrawIndexedPrimitivesPreservesAllArgumentValues() {
  resetCapture();

  WMT::RenderCommandEncoder encoder;
  encoder.handle = static_cast<obj_handle_t>(0xFACEu);

  WMT::Buffer indexBuffer;
  indexBuffer.handle = static_cast<obj_handle_t>(0xBEEFull);
  encoder.drawIndexedPrimitives(WMTPrimitiveTypeTriangleStrip,
                                WMTIndexTypeUInt32,
                                17u,
                                indexBuffer,
                                64u,
                                3u,
                                -5,
                                9u);

  checkEq(g_commands.size(), std::size_t{1},
          "recorder captures a single indexed draw");
  const auto& draw = commandAt(0, "missing indexed draw command");
  checkEq(static_cast<unsigned>(draw.drawIndexed.primitive_type),
          static_cast<unsigned>(WMTPrimitiveTypeTriangleStrip),
          "indexed draw preserves primitive type");
  checkEq(static_cast<unsigned>(draw.drawIndexed.index_type),
          static_cast<unsigned>(WMTIndexTypeUInt32),
          "indexed draw preserves index type");
  checkEq(draw.drawIndexed.index_count, uint64_t{17},
          "indexed draw preserves index count");
  checkEq(draw.drawIndexed.index_buffer, indexBuffer.handle,
          "indexed draw preserves index buffer handle");
  checkEq(draw.drawIndexed.index_buffer_offset, uint64_t{64},
          "indexed draw preserves index buffer offset");
  checkEq(draw.drawIndexed.instance_count, uint32_t{3},
          "indexed draw preserves instance count");
  checkEq(draw.drawIndexed.base_vertex, int32_t{-5},
          "indexed draw preserves base vertex");
  checkEq(draw.drawIndexed.base_instance, uint32_t{9},
          "indexed draw preserves base instance");
}

}  // namespace

extern "C" void
MTLRenderCommandEncoder_encodeCommands(obj_handle_t encoder,
                                       const struct wmtcmd_base* cmdHead) {
  const auto* next = reinterpret_cast<const wmtcmd_render_nop*>(cmdHead);
  while (next != nullptr) {
    CapturedRenderCommand captured{};
    captured.encoder = encoder;
    captured.type = next->type;
    if (next->type == WMTRenderCommandSetVertexBuffer) {
      captured.setBuffer =
          *reinterpret_cast<const wmtcmd_render_setbuffer*>(next);
    } else if (next->type == WMTRenderCommandDrawIndexed) {
      captured.drawIndexed =
          *reinterpret_cast<const wmtcmd_render_draw_indexed*>(next);
    }
    g_commands.push_back(captured);
    next = reinterpret_cast<const wmtcmd_render_nop*>(next->next.ptr);
  }
}

int main() {
  try {
    test3dmark05FanFixShapeReachesDrawIndexedPrimitives();
    testDrawIndexedPrimitivesPreservesAllArgumentValues();
  } catch (const TestFailure& e) {
    std::cerr << "metal_encoder_recorder_spec failed: " << e.what() << '\n';
    return EXIT_FAILURE;
  } catch (const std::exception& e) {
    std::cerr << "metal_encoder_recorder_spec unexpected exception: "
              << e.what() << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "metal_encoder_recorder_spec passed\n";
  return EXIT_SUCCESS;
}
