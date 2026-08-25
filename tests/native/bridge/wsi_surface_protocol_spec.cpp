#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

#include "dxmt9/device_c.h"
#include "dxmt9/wsi_surface_protocol.hpp"

namespace {

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void check(bool condition, std::string_view message) {
  if (!condition) {
    throw TestFailure(std::string(message));
  }
}

using dxmt9::wsi::SurfaceBindingState;
using dxmt9::wsi::SurfaceProtocol;

SurfaceBindingState escapeBinding() {
  return SurfaceBindingState{
      .protocol = SurfaceProtocol::ExtEscapeV1,
      .hwnd = 0x100u,
      .surfaceToken = 0x200u,
      .layerToken = 0x300u,
  };
}

void testWireLayoutAndEscapeValidation() {
  static_assert(MACDRV_ESCAPE_GET_SURFACE == 6790);
  static_assert(MACDRV_ESCAPE_RELEASE_SURFACE == 6791);
  static_assert(sizeof(macdrv_escape_surface) == 2u * sizeof(std::uint64_t));
  static_assert(sizeof(D9CWsiSurfaceBinding) == 32u);

  const macdrv_escape_surface valid{0x200u, 0x300u};
  check(dxmt9::wsi::validEscapeResponse(1, sizeof(valid), valid),
        "positive exact-width response with both tokens is valid");
  check(!dxmt9::wsi::validEscapeResponse(0, sizeof(valid), valid),
        "failed get escape is rejected");
  check(!dxmt9::wsi::validEscapeResponse(1, sizeof(valid) - 1u, valid),
        "malformed response width is rejected");
  check(!dxmt9::wsi::validEscapeResponse(
            1, sizeof(valid), macdrv_escape_surface{0u, valid.layer}),
        "zero surface token is rejected");
  check(!dxmt9::wsi::validEscapeResponse(
            1, sizeof(valid), macdrv_escape_surface{valid.surface, 0u}),
        "zero layer token is rejected");
}

void testUnsupportedQueryAndLegacySelection() {
  check(dxmt9::wsi::selectSurfaceProtocol(false, false) ==
            SurfaceProtocol::Unsupported,
        "unsupported query without qualification fails closed");
  check(dxmt9::wsi::selectSurfaceProtocol(false, true) ==
            SurfaceProtocol::LegacyMacdrvSymbols,
        "unsupported query selects the explicitly qualified legacy path");
  check(dxmt9::wsi::selectSurfaceProtocol(true, true) ==
            SurfaceProtocol::ExtEscapeV1,
        "ExtEscape takes precedence over legacy qualification");
  check(dxmt9::wsi::isLegacyProtocolDeclaration("legacy-macdrv-symbols"),
        "exact legacy manifest value is accepted");
  check(!dxmt9::wsi::isLegacyProtocolDeclaration("macdrv_functions"),
        "generic aggregate-table discovery is not manifest qualification");
  check(!dxmt9::wsi::isLegacyProtocolDeclaration("legacy-macdrv-symbols-v2"),
        "near-match legacy declaration is rejected");
}

void testAdoptionRollbackPredicate() {
  SurfaceBindingState current = escapeBinding();
  current.unixAdopted = true;
  SurfaceBindingState candidate = escapeBinding();
  candidate.surfaceToken = 0x400u;
  candidate.layerToken = 0x500u;

  check(dxmt9::wsi::preserveCurrentBindingOnAdoptionFailure(
            current, candidate, false),
        "failed candidate adoption preserves the current valid binding");
  check(!dxmt9::wsi::preserveCurrentBindingOnAdoptionFailure(
            current, candidate, true),
        "successful adoption does not request rollback");
  candidate.layerToken = 0u;
  check(!dxmt9::wsi::preserveCurrentBindingOnAdoptionFailure(
            current, candidate, false),
        "malformed candidate cannot enter adoption rollback");
}

void testTypedOutputRestoreKeepsExtEscapeLayer() {
  SurfaceBindingState binding = escapeBinding();
  binding.unixAdopted = true;
  check(dxmt9::wsi::validPresenterRestoreBinding(
            binding.protocol, binding.hwnd, binding.layerToken),
        "typed PresentOutput restore accepts the persisted ExtEscape layer");

  binding.surfaceToken = 0u;
  check(dxmt9::wsi::validPresenterRestoreBinding(
            binding.protocol, binding.hwnd, binding.layerToken),
        "unix restore does not revalidate the PE-owned surface token");
}

void testReleaseIsExactlyOnceAfterQuiescence() {
  SurfaceBindingState binding = escapeBinding();
  check(!binding.unixAdopted,
        "fresh Wine acquisition models a failed unix adoption candidate");
  check(!dxmt9::wsi::canAttemptWineRelease(binding, false),
        "failed-adoption release is forbidden before unix acknowledgement");
  check(dxmt9::wsi::canAttemptWineRelease(binding, true),
        "acquired surface remains releasable when unix adoption fails");
  binding.releaseAttempted = true;
  check(!dxmt9::wsi::canAttemptWineRelease(binding, true),
        "failed-adoption release consumes the obligation exactly once");

  binding = escapeBinding();
  binding.unixAdopted = true;
  check(dxmt9::wsi::canAttemptWineRelease(binding, true),
        "adopted surface is releasable after presenter quiescence");

  SurfaceBindingState legacy{
      .protocol = SurfaceProtocol::LegacyMacdrvSymbols,
      .hwnd = 0x100u,
      .unixAdopted = true,
  };
  check(!dxmt9::wsi::hasWineReleaseObligation(legacy),
        "legacy provider-owned acquisition has no ExtEscape release token");
}

}  // namespace

int main() {
  try {
    testWireLayoutAndEscapeValidation();
    testUnsupportedQueryAndLegacySelection();
    testAdoptionRollbackPredicate();
    testTypedOutputRestoreKeepsExtEscapeLayer();
    testReleaseIsExactlyOnceAfterQuiescence();
  } catch (const std::exception& error) {
    std::cerr << "wsi_surface_protocol_spec failed: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "wsi_surface_protocol_spec passed\n";
  return EXIT_SUCCESS;
}
