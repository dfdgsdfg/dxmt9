#include "../../../src/dxmt9/render/backend_interface.hpp"
#include "../../../src/dxmt9/render/backend_factory.hpp"

#include <stdexcept>
#include <string>
#include <string_view>
#include <iostream>

namespace {
struct TestFailure : std::runtime_error { using std::runtime_error::runtime_error; };
void check(bool c, std::string_view m) { if (!c) throw TestFailure(std::string(m)); }

void testBackendModeAndCapsDefaults() {
  using namespace dxmt9::render;
  BackendCaps caps{};
  check(caps.supports_mesh == false, "caps default supports_mesh=false");
  check(caps.supports_icb == false, "caps default supports_icb=false");
  check(static_cast<int>(BackendMode::Traditional) == 0, "Traditional is 0");
}

void testFactoryModeResolution() {
  using namespace dxmt9::render;
  check(resolveBackendMode(nullptr) == BackendMode::Traditional, "unset → Traditional");
  check(resolveBackendMode("") == BackendMode::Traditional, "empty → Traditional");
  check(resolveBackendMode("0") == BackendMode::Traditional, "\"0\" → Traditional");
  check(resolveBackendMode("traditional") == BackendMode::Traditional, "traditional");
  check(resolveBackendMode("framegraph") == BackendMode::FrameGraph, "framegraph");
  check(resolveBackendMode("garbage") == BackendMode::Traditional, "unknown → Traditional");
}

void testCreateBackendByMode() {
  using namespace dxmt9::render;
  check(createBackend(BackendMode::Traditional)->mode() == BackendMode::Traditional, "createBackend Traditional");
  check(createBackend(BackendMode::FrameGraph)->mode() == BackendMode::FrameGraph, "createBackend FrameGraph");
}
}  // namespace

int main() {
  try {
    testBackendModeAndCapsDefaults();
    testFactoryModeResolution();
    testCreateBackendByMode();
  }
  catch (const std::exception& e) {
    std::cerr << "backend_interface_spec failed: " << e.what() << '\n'; return 1;
  }
  std::cout << "backend_interface_spec passed\n"; return 0;
}
