// LIGHT spec for FrameGraphBackend (Task A5, R-BACK-40.5).
//
// Asserts the L0 contract without building a device/ChunkSlot fixture
// (behavioral equivalence with the traditional path is Task A8's job):
//   - FrameGraphBackend is constructible
//   - mode() == BackendMode::FrameGraph
//   - usable through an IRenderBackend& reference
//   - the pure profile/feature resolvers select the promoted passcoalesce-only
//     default and retain explicit strict/empty rollback paths.
// Deliberately does NOT call onChunkReady.

#include "../../../src/dxmt9/render/framegraph_backend.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using dxmt9::render::BackendMode;
using dxmt9::render::FrameGraphBackend;
using dxmt9::render::IRenderBackend;
using dxmt9::render::RendererCompatProfile;
using dxmt9::render::resolveRendererCompatProfile;
using dxmt9::render::resolveRendererFeatures;

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void check(bool condition, std::string_view message) {
  if (!condition) {
    throw TestFailure(std::string(message));
  }
}

void testConstructibleAndMode() {
  FrameGraphBackend backend;
  check(backend.mode() == BackendMode::FrameGraph,
        "mode() returns BackendMode::FrameGraph");
}

void testUsableThroughInterface() {
  FrameGraphBackend backend;
  IRenderBackend& iface = backend;
  check(iface.mode() == BackendMode::FrameGraph,
        "IRenderBackend& mode() returns FrameGraph");
  // Lifecycle hooks inherit no-op defaults; calling them must be safe.
  iface.onDeviceCreated();
  iface.onFrameBegin(0);
  iface.onFrameEnd();
  iface.onDeviceDestroyed();
}

void testStrictResolverEmptyForAllTokens() {
  check(resolveRendererFeatures(nullptr, RendererCompatProfile::Strict).empty(),
        "strict null env yields empty feature set");
  check(resolveRendererFeatures("", RendererCompatProfile::Strict).empty(),
        "strict empty env yields empty feature set");
  check(resolveRendererFeatures("   , ;  ",
                                RendererCompatProfile::Strict)
            .empty(),
        "strict separator-only env yields empty feature set");
}

void testResolverEmptyForGarbageTokens() {
  // Garbage / unknown tokens are rejected under strict and the set stays empty.
  check(resolveRendererFeatures("garbage", RendererCompatProfile::Strict)
            .empty(),
        "garbage env yields empty feature set");
  check(resolveRendererFeatures("mesh,icb,not-a-feature",
                                RendererCompatProfile::Strict)
            .empty(),
        "multi-token env yields empty feature set");
}

void testProgressivePasscoalesceResolution() {
  check(resolveRendererCompatProfile(nullptr) ==
            RendererCompatProfile::Progressive,
        "unset compat profile resolves to progressive");
  check(resolveRendererCompatProfile("progressive") ==
            RendererCompatProfile::Progressive,
        "progressive compat profile is recognized");
  check(resolveRendererCompatProfile("strict") ==
            RendererCompatProfile::Strict,
        "strict compat profile is recognized");
  check(resolveRendererCompatProfile("unknown") ==
            RendererCompatProfile::Strict,
        "unknown compat profile resolves to strict");
  check(resolveRendererFeatures("passcoalesce",
                                RendererCompatProfile::Strict)
            .empty(),
        "strict rejects passcoalesce");
  const auto defaults =
      resolveRendererFeatures(nullptr, RendererCompatProfile::Progressive);
  check(defaults.passcoalesce,
        "unset progressive features enable promoted passcoalesce");
  check(!defaults.dce,
        "unset progressive features keep bounded DCE opt-in");
  check(resolveRendererFeatures("", RendererCompatProfile::Progressive).empty(),
        "empty progressive features explicitly disable passcoalesce");
  check(resolveRendererFeatures("0", RendererCompatProfile::Progressive).empty(),
        "zero progressive features explicitly disable passcoalesce");
  const auto progressive = resolveRendererFeatures(
      "passcoalesce,dce,unknown", RendererCompatProfile::Progressive);
  check(progressive.passcoalesce,
        "progressive accepts the implemented passcoalesce feature");
  check(progressive.dce,
        "progressive accepts the opt-in bounded DCE feature");
  check(resolveRendererFeatures("dce", RendererCompatProfile::Strict).empty(),
        "strict rejects the bounded DCE token");
}

}  // namespace

int main() {
  try {
    testConstructibleAndMode();
    testUsableThroughInterface();
    testStrictResolverEmptyForAllTokens();
    testResolverEmptyForGarbageTokens();
    testProgressivePasscoalesceResolution();
  } catch (const std::exception& e) {
    std::cerr << "framegraph_backend_spec failed: " << e.what() << '\n';
    return 1;
  }
  std::cout << "framegraph_backend_spec passed\n";
  return 0;
}
