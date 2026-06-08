// LIGHT spec for TraditionalBackend (Task A4). Behavioral equivalence with the
// traditional encode path is proven by the A8 parity harness; here we only lock
// in construction, mode(), and that the type is usable through both the
// IRenderBackend and IExternalDrawEmitter base references (polymorphism
// compiles). onChunkReady/emitDraw need device fixtures and are not exercised.

#include "../../../src/dxmt9/render/traditional_backend.hpp"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using dxmt9::render::BackendMode;
using dxmt9::render::IExternalDrawEmitter;
using dxmt9::render::IRenderBackend;
using dxmt9::render::TraditionalBackend;

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

void testConstructibleAndMode() {
  TraditionalBackend backend;
  check(backend.mode() == BackendMode::Traditional,
        "TraditionalBackend::mode() returns Traditional");
}

void testUsableThroughRenderBackendRef() {
  TraditionalBackend backend;
  IRenderBackend& base = backend;
  check(base.mode() == BackendMode::Traditional,
        "mode() through IRenderBackend& returns Traditional");
  // Default caps are all-false / zero for the stateless traditional backend.
  check(!base.caps().supports_mesh,
        "default caps report no mesh support");
}

void testUsableThroughExternalDrawEmitterRef() {
  TraditionalBackend backend;
  IExternalDrawEmitter& emitter = backend;
  // Only that the reference binds (polymorphism compiles); no emit call here.
  check(&emitter == static_cast<IExternalDrawEmitter*>(&backend),
        "IExternalDrawEmitter& binds to the same TraditionalBackend");
}

}  // namespace

int main() {
  try {
    testConstructibleAndMode();
    testUsableThroughRenderBackendRef();
    testUsableThroughExternalDrawEmitterRef();
  } catch (const TestFailure& failure) {
    std::cerr << "traditional_backend_spec failed: " << failure.what() << '\n';
    return 1;
  } catch (const std::exception& ex) {
    std::cerr << "traditional_backend_spec unexpected exception: " << ex.what()
              << '\n';
    return 1;
  }
  return 0;
}
