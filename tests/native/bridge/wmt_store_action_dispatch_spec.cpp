// Pins the narrow WMT render-encoder Store-action setters used to resolve
// WMTStoreActionUnknown immediately before endEncoding. This is a PE-side
// bridge test: local C stubs capture the exact handle, action, and color slot.

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

#define WINEMETAL_API extern "C"

#include "winemetal/Metal.hpp"

namespace {

struct Capture {
  enum class Setter { None, Color, Depth, Stencil } setter = Setter::None;
  obj_handle_t encoder = 0;
  WMTStoreAction action = WMTStoreActionStore;
  std::uint32_t colorIndex = 0;
};

Capture g_capture;

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void check(bool condition, std::string_view message) {
  if (!condition) {
    throw TestFailure(std::string(message));
  }
}

void reset() {
  g_capture = {};
}

void testColorSetter() {
  reset();
  WMT::RenderCommandEncoder encoder{0xC010u};
  encoder.setColorStoreAction(WMTStoreActionDontCare, 3u);
  check(g_capture.setter == Capture::Setter::Color,
        "color wrapper selects the color Store-action setter");
  check(g_capture.encoder == 0xC010u,
        "color wrapper preserves encoder handle");
  check(g_capture.action == WMTStoreActionDontCare,
        "color wrapper preserves Store action");
  check(g_capture.colorIndex == 3u,
        "color wrapper preserves attachment slot");
}

void testDepthSetter() {
  reset();
  WMT::RenderCommandEncoder encoder{0xD020u};
  encoder.setDepthStoreAction(WMTStoreActionStore);
  check(g_capture.setter == Capture::Setter::Depth,
        "depth wrapper selects the depth Store-action setter");
  check(g_capture.encoder == 0xD020u,
        "depth wrapper preserves encoder handle");
  check(g_capture.action == WMTStoreActionStore,
        "depth wrapper preserves Store action");
}

void testStencilSetter() {
  reset();
  WMT::RenderCommandEncoder encoder{0x5700u};
  encoder.setStencilStoreAction(WMTStoreActionDontCare);
  check(g_capture.setter == Capture::Setter::Stencil,
        "stencil wrapper selects the stencil Store-action setter");
  check(g_capture.encoder == 0x5700u,
        "stencil wrapper preserves encoder handle");
  check(g_capture.action == WMTStoreActionDontCare,
        "stencil wrapper preserves Store action");
}

}  // namespace

extern "C" void MTLRenderCommandEncoder_setColorStoreAction(
    obj_handle_t encoder,
    WMTStoreAction action,
    std::uint32_t colorAttachmentIndex) {
  g_capture = {
      .setter = Capture::Setter::Color,
      .encoder = encoder,
      .action = action,
      .colorIndex = colorAttachmentIndex,
  };
}

extern "C" void MTLRenderCommandEncoder_setDepthStoreAction(
    obj_handle_t encoder, WMTStoreAction action) {
  g_capture = {
      .setter = Capture::Setter::Depth,
      .encoder = encoder,
      .action = action,
  };
}

extern "C" void MTLRenderCommandEncoder_setStencilStoreAction(
    obj_handle_t encoder, WMTStoreAction action) {
  g_capture = {
      .setter = Capture::Setter::Stencil,
      .encoder = encoder,
      .action = action,
  };
}

int main() {
  try {
    testColorSetter();
    testDepthSetter();
    testStencilSetter();
  } catch (const TestFailure& e) {
    std::cerr << "wmt_store_action_dispatch_spec failed: " << e.what()
              << '\n';
    return EXIT_FAILURE;
  } catch (const std::exception& e) {
    std::cerr << "wmt_store_action_dispatch_spec unexpected exception: "
              << e.what() << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "wmt_store_action_dispatch_spec passed\n";
  return EXIT_SUCCESS;
}
