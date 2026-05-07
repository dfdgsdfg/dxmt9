// Verifies that the winemetal C++ bridge wrappers around
// `[MTLRenderCommandEncoder set{Fragment,Vertex}Bytes:length:atIndex:]`
// emit a `wmtcmd_render_setbytes` struct tagged with the right
// `WMTRenderCommandType` discriminator.
//
// This test stays inside the PE-side bridge surface: it does not link the
// unix winemetal provider. Instead it overrides
// `MTLRenderCommandEncoder_encodeCommands` so the wrappers in `Metal.hpp`
// dispatch into a local capture buffer that the test can inspect.
//
// We override the symbol by defining `WINEMETAL_API` to plain `extern "C"`
// before including `winemetal.h`; on Darwin/Linux that header otherwise
// declares the symbol with no import attribute, so a strong local
// definition wins at link time.

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

#define WINEMETAL_API extern "C"

#include "winemetal/Metal.hpp"

namespace {

struct CapturedSetBytes {
  bool seen = false;
  WMTRenderCommandType type = WMTRenderCommandNop;
  obj_handle_t encoder = 0;
  const void *bytes = nullptr;
  uint64_t length = 0;
  uint8_t index = 0;
};

CapturedSetBytes g_captured;

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
void checkEq(const A &left, const B &right, std::string_view message) {
  if (!(left == right)) {
    std::ostringstream out;
    out << message << " (" << left << " vs " << right << ")";
    fail(out.str());
  }
}

void resetCapture() {
  g_captured = CapturedSetBytes{};
}

void testSetVertexBytesEmitsVertexBytesCommand() {
  resetCapture();
  WMT::RenderCommandEncoder enc;
  enc.handle = 0xDEADBEEF;

  alignas(uint64_t) uint8_t payload[16] = {
    0x10, 0x32, 0x54, 0x76,
    0x98, 0xBA, 0xDC, 0xFE,
    0x01, 0x23, 0x45, 0x67,
    0x89, 0xAB, 0xCD, 0xEF,
  };

  enc.setVertexBytes(payload, sizeof(payload), 5);

  check(g_captured.seen, "setVertexBytes dispatched into encodeCommands");
  checkEq(static_cast<unsigned int>(g_captured.type),
          static_cast<unsigned int>(WMTRenderCommandSetVertexBytes),
          "setVertexBytes emits WMTRenderCommandSetVertexBytes");
  checkEq(g_captured.encoder, static_cast<obj_handle_t>(0xDEADBEEF),
          "setVertexBytes targets the encoder it was called on");
  check(g_captured.bytes == static_cast<const void *>(payload),
        "setVertexBytes forwards the caller's byte pointer");
  checkEq(g_captured.length, static_cast<uint64_t>(sizeof(payload)),
          "setVertexBytes forwards the caller's byte length");
  checkEq(static_cast<unsigned>(g_captured.index), 5u,
          "setVertexBytes forwards the caller's slot index");
}

void testSetFragmentBytesStillEmitsFragmentBytesCommand() {
  // Regression guard: the new vertex variant must not have stolen the
  // fragment dispatch case.
  resetCapture();
  WMT::RenderCommandEncoder enc;
  enc.handle = 0xCAFEBABE;

  uint32_t payload = 0xA5A5A5A5;
  enc.setFragmentBytes(&payload, sizeof(payload), 2);

  check(g_captured.seen, "setFragmentBytes dispatched into encodeCommands");
  checkEq(static_cast<unsigned int>(g_captured.type),
          static_cast<unsigned int>(WMTRenderCommandSetFragmentBytes),
          "setFragmentBytes emits WMTRenderCommandSetFragmentBytes");
  checkEq(static_cast<unsigned>(g_captured.index), 2u,
          "setFragmentBytes forwards the caller's slot index");
}

void testVertexAndFragmentDiscriminatorsDiffer() {
  // Sanity: the two enum values must not alias, otherwise the dispatch
  // case in winemetal_private_api.mm would be ambiguous.
  check(WMTRenderCommandSetVertexBytes != WMTRenderCommandSetFragmentBytes,
        "vertex and fragment setBytes discriminators are distinct");
}

}  // namespace

extern "C" void
MTLRenderCommandEncoder_encodeCommands(obj_handle_t encoder,
                                       const struct wmtcmd_base *cmd_head) {
  // Mirror the unix-side dispatcher closely enough to confirm the wrapper
  // produced a `wmtcmd_render_setbytes` payload tagged for the right
  // shader stage.
  const auto *render = reinterpret_cast<const wmtcmd_render_nop *>(cmd_head);
  g_captured.seen = true;
  g_captured.encoder = encoder;
  g_captured.type = render->type;
  if (render->type == WMTRenderCommandSetVertexBytes ||
      render->type == WMTRenderCommandSetFragmentBytes) {
    const auto *b = reinterpret_cast<const wmtcmd_render_setbytes *>(cmd_head);
    g_captured.bytes = b->bytes.ptr;
    g_captured.length = b->length;
    g_captured.index = b->index;
  }
}

int main() {
  try {
    testVertexAndFragmentDiscriminatorsDiffer();
    testSetVertexBytesEmitsVertexBytesCommand();
    testSetFragmentBytesStillEmitsFragmentBytesCommand();
  } catch (const TestFailure &e) {
    std::cerr << "wmt_setbytes_dispatch_spec failed: " << e.what() << '\n';
    return EXIT_FAILURE;
  } catch (const std::exception &e) {
    std::cerr << "wmt_setbytes_dispatch_spec unexpected exception: "
              << e.what() << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "wmt_setbytes_dispatch_spec passed\n";
  return EXIT_SUCCESS;
}
