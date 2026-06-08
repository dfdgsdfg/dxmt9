#include "../../../src/dxmt9/render/backend_interface.hpp"

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
}  // namespace

int main() {
  try { testBackendModeAndCapsDefaults(); }
  catch (const std::exception& e) {
    std::cerr << "backend_interface_spec failed: " << e.what() << '\n'; return 1;
  }
  std::cout << "backend_interface_spec passed\n"; return 0;
}
