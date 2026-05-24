// Factory HRESULT / enum validation parity spec.
//
// Locks the shared dxmt9::com / dxmt9::core::Factory validation contract
// that backs the PE-side IDirect3D9 / IDirect3D9Ex wrapper
// (src/d3d9/d3d9_pe_factory.cpp). The Wine behavioral oracle for these
// outcomes is encoded by the PE conformance functions in
// tests/conformance/d3d9 (read, not modified by this spec):
//   - test_factory_validation_return_codes      (CheckDeviceType / MS-type)
//   - test_invalid_multisample_render_target_quality
//   - test_check_device_format_conversion_matrix
//   - test_ex_create_reset_mode_validation       (CreateDeviceEx mode)
//   - test_ex_get_adapter_display_mode_ex_policy
//   - test_ex_get_adapter_luid_policy
//
// The core C++ enums (DeviceType, MultiSampleType) cannot represent the
// out-of-domain values the PE oracle pokes (0xdead, 65536, 15_SAMPLES);
// those invalid-enum cases are covered by the PE conformance exe. This
// spec covers the in-domain shared contract: invalid adapter ordinal,
// valid-but-unavailable device type, the D3DMULTISAMPLE_NONE quality
// level, unsupported sample-count rejection, and Ex adapter-ordinal
// validation for EnumAdapterModesEx / GetAdapterDisplayModeEx /
// GetAdapterLUID.

#include "core_spec_fixtures.hpp"

#include <memory>

using namespace dxmt9::core;
using namespace dxmt9::core::fixture;
using namespace dxmt9::core::spec;

namespace {

// CheckDeviceType: adapter-ordinal + device-type availability parity.
// Oracle: test_factory_validation_return_codes (invalid adapter ->
// INVALIDCALL) and the HAL-only availability policy.
void testCheckDeviceTypeValidation() {
  using namespace dxmt9::com;

  auto* d3d = Direct3DCreate9(D3D_SDK_VERSION);
  check(d3d != nullptr, "Direct3DCreate9");
  const size_t adapterCount = d3d->GetAdapterCount();
  check(adapterCount > 0, "adapter count positive");

  // Valid HAL on the default adapter -> D3D_OK (windowed and fullscreen).
  checkEq(d3d->CheckDeviceType(0, DeviceType::Hal, Format::X8R8G8B8, Format::X8R8G8B8, true),
          D3D_OK, "CheckDeviceType HAL windowed default");
  checkEq(d3d->CheckDeviceType(0, DeviceType::Hal, Format::X8R8G8B8, Format::X8R8G8B8, false),
          D3D_OK, "CheckDeviceType HAL fullscreen default");

  // Invalid adapter ordinal -> D3DERR_INVALIDCALL (oracle line ~60).
  checkEq(d3d->CheckDeviceType(adapterCount, DeviceType::Hal, Format::X8R8G8B8, Format::X8R8G8B8, true),
          D3DERR_INVALIDCALL, "CheckDeviceType invalid adapter");

  // Valid-but-unavailable device type (REF / NULLREF) -> D3DERR_NOTAVAILABLE.
  // dxmt9 only exposes HAL; the PE layer maps the same policy after its
  // known-enum gate.
  checkEq(d3d->CheckDeviceType(0, DeviceType::Ref, Format::X8R8G8B8, Format::X8R8G8B8, true),
          D3DERR_NOTAVAILABLE, "CheckDeviceType REF unavailable");
  checkEq(d3d->CheckDeviceType(0, DeviceType::NullRef, Format::X8R8G8B8, Format::X8R8G8B8, true),
          D3DERR_NOTAVAILABLE, "CheckDeviceType NULLREF unavailable");

  // Fullscreen display-format parity: a non-display-mode adapter format
  // (compressed DXT1 is not a scan-out format) is not a valid fullscreen
  // mode -> D3DERR_NOTAVAILABLE. (The PE layer additionally narrows the
  // fullscreen scan-out set to X8R8G8B8 / R5G6B5; that tighter PE rule is
  // covered by test_check_device_type_display_format_policy in the PE
  // conformance exe.)
  checkEq(d3d->CheckDeviceType(0, DeviceType::Hal, Format::DXT1, Format::DXT1, false),
          D3DERR_NOTAVAILABLE, "CheckDeviceType fullscreen non-display format");

  checkEq(d3d->Release(), 0u, "factory release");
}

// CheckDeviceMultiSampleType: D3DMULTISAMPLE_NONE quality level + the
// unsupported sample-count rejection. Oracle:
// test_factory_validation_return_codes (NONE -> success, quality 1) and
// test_invalid_multisample_render_target_quality (unsupported count).
void testCheckDeviceMultiSampleTypeValidation() {
  using namespace dxmt9::com;

  // The stub factory uses the default BackendLimits, which deterministically
  // expose sample counts 2 and 4 but not 8 (supportsSampleCount8 == false).
  // This pins both the available and the NOTAVAILABLE branches without
  // depending on host GPU caps.
  auto* d3d = Direct3DCreate9Ex(D3D_SDK_VERSION);
  check(d3d != nullptr, "Direct3DCreate9Ex");
  const size_t adapterCount = d3d->GetAdapterCount();
  check(adapterCount > 0, "adapter count positive");

  // D3DMULTISAMPLE_NONE always reports a single quality level -> D3D_OK.
  checkEq(d3d->CheckDeviceMultiSampleType(0, Format::X8R8G8B8, MultiSampleType::None),
          D3D_OK, "MultiSampleType NONE always available");

  // Supported sample count on a render-target-capable format -> D3D_OK.
  checkEq(d3d->CheckDeviceMultiSampleType(0, Format::X8R8G8B8, MultiSampleType::Two),
          D3D_OK, "MultiSampleType 2x supported");
  checkEq(d3d->CheckDeviceMultiSampleType(0, Format::X8R8G8B8, MultiSampleType::Four),
          D3D_OK, "MultiSampleType 4x supported");

  // Unsupported sample count -> D3DERR_NOTAVAILABLE (oracle:
  // CreateRenderTarget with an unavailable MS type is rejected; the
  // factory cap query reports NOTAVAILABLE up front).
  checkEq(d3d->CheckDeviceMultiSampleType(0, Format::X8R8G8B8, MultiSampleType::Eight),
          D3DERR_NOTAVAILABLE, "MultiSampleType 8x unsupported");

  // Invalid adapter ordinal -> D3DERR_INVALIDCALL.
  checkEq(d3d->CheckDeviceMultiSampleType(adapterCount, Format::X8R8G8B8, MultiSampleType::Two),
          D3DERR_INVALIDCALL, "MultiSampleType invalid adapter");

  checkEq(d3d->Release(), 0u, "factory release");
}

// Ex adapter-ordinal validation for EnumAdapterModesEx /
// GetAdapterDisplayModeEx / GetAdapterLUID, plus the rotation/structure
// outcomes. Oracle: test_ex_get_adapter_display_mode_ex_policy and
// test_ex_get_adapter_luid_policy.
void testExFactoryValidation() {
  using namespace dxmt9::com;

  auto* d3d = Direct3DCreate9Ex(D3D_SDK_VERSION);
  check(d3d != nullptr, "Direct3DCreate9Ex");
  const size_t adapterCount = d3d->GetAdapterCount();
  check(adapterCount > 0, "adapter count positive");

  // GetAdapterDisplayModeEx default adapter: succeeds, rotation IDENTITY,
  // non-degenerate mode, NULL rotation is allowed.
  DisplayModeEx mode{};
  DisplayRotation rotation = DisplayRotation::Rotate90;
  check(d3d->GetAdapterDisplayModeEx(0, &mode, &rotation), "GetAdapterDisplayModeEx default");
  checkEq(rotation, DisplayRotation::Identity, "GetAdapterDisplayModeEx rotation IDENTITY");
  check(mode.width > 0, "GetAdapterDisplayModeEx width positive");
  check(mode.height > 0, "GetAdapterDisplayModeEx height positive");
  check(d3d->GetAdapterDisplayModeEx(0, &mode, nullptr), "GetAdapterDisplayModeEx NULL rotation");

  // Invalid adapter ordinal -> failure.
  DisplayModeEx badMode{};
  check(!d3d->GetAdapterDisplayModeEx(adapterCount, &badMode, &rotation),
        "GetAdapterDisplayModeEx invalid adapter rejected");
  // NULL out-pointer -> failure.
  check(!d3d->GetAdapterDisplayModeEx(0, nullptr, &rotation),
        "GetAdapterDisplayModeEx NULL mode rejected");

  // EnumAdapterModesEx: valid index on default adapter succeeds; invalid
  // adapter ordinal and NULL out-pointer are rejected.
  DisplayModeFilter filter{};
  filter.format = Format::X8R8G8B8;
  DisplayModeEx enumMode{};
  check(d3d->EnumAdapterModesEx(0, &filter, 0, &enumMode), "EnumAdapterModesEx default");
  checkEq(enumMode.scanLineOrdering, DisplayScanLineOrdering::Progressive,
          "EnumAdapterModesEx progressive scanline");
  check(!d3d->EnumAdapterModesEx(adapterCount, &filter, 0, &enumMode),
        "EnumAdapterModesEx invalid adapter rejected");
  check(!d3d->EnumAdapterModesEx(0, &filter, 0, nullptr),
        "EnumAdapterModesEx NULL mode rejected");

  // GetAdapterLUID: default adapter succeeds with a non-zero, stable LUID;
  // invalid adapter ordinal and NULL out-pointer are rejected.
  Luid luid0{};
  Luid luid1{};
  check(d3d->GetAdapterLUID(0, &luid0), "GetAdapterLUID default");
  check(luid0.lowPart != 0 || luid0.highPart != 0, "GetAdapterLUID non-zero");
  check(d3d->GetAdapterLUID(0, &luid1), "GetAdapterLUID stable call");
  checkEq(luid0.lowPart, luid1.lowPart, "GetAdapterLUID low stable");
  checkEq(luid0.highPart, luid1.highPart, "GetAdapterLUID high stable");
  check(!d3d->GetAdapterLUID(adapterCount, &luid0), "GetAdapterLUID invalid adapter rejected");
  check(!d3d->GetAdapterLUID(0, nullptr), "GetAdapterLUID NULL out rejected");

  checkEq(d3d->Release(), 0u, "factory ex release");
}

// CreateDeviceEx fullscreen-mode relation validation. Oracle:
// test_ex_create_reset_mode_validation — a windowed present-parameters
// block paired with a fullscreen mode, or a fullscreen block with no
// mode / a mode whose dimensions disagree with the back buffer, all fail.
void testCreateDeviceExModeValidation() {
  using namespace dxmt9::com;

  auto* d3d = Direct3DCreate9Ex(D3D_SDK_VERSION);
  check(d3d != nullptr, "Direct3DCreate9Ex");

  // Fullscreen params with a mode whose dimensions match the back buffer
  // succeed (control case).
  PresentParameters fsParams{};
  fsParams.windowed = false;
  fsParams.backBufferWidth = 1024;
  fsParams.backBufferHeight = 768;
  fsParams.backBufferFormat = Format::A8R8G8B8;
  fsParams.presentationInterval = PresentInterval::Default;
  fsParams.deviceWindow = Handle{404};
  DisplayModeEx fsMode{};
  fsMode.width = 1024;
  fsMode.height = 768;
  fsMode.format = Format::A8R8G8B8;
  auto* ok = d3d->CreateDeviceEx(0, fsParams, &fsMode);
  check(ok != nullptr, "CreateDeviceEx fullscreen matching mode");
  if (ok) {
    checkEq(ok->Release(), 0u, "CreateDeviceEx control device release");
  }

  // NB: the fullscreen-params-without-mode rejection (oracle
  // test_ex_create_reset_mode_validation line ~361) is enforced at the
  // PE factory layer (d3d9_pe_factory.cpp CreateDeviceEx), not in the
  // shared core fullscreen-mode-relation helper, so it is exercised by
  // the PE conformance exe rather than asserted here.

  // Fullscreen params with a mode whose dimensions disagree with the
  // back buffer -> rejected.
  DisplayModeEx mismatch = fsMode;
  mismatch.width = 800;
  check(d3d->CreateDeviceEx(0, fsParams, &mismatch) == nullptr,
        "CreateDeviceEx fullscreen mismatched mode rejected");

  // Windowed params paired with a fullscreen mode -> rejected.
  PresentParameters winParams{};
  winParams.windowed = true;
  winParams.backBufferWidth = 320;
  winParams.backBufferHeight = 240;
  winParams.backBufferFormat = Format::A8R8G8B8;
  winParams.presentationInterval = PresentInterval::Default;
  winParams.deviceWindow = Handle{405};
  check(d3d->CreateDeviceEx(0, winParams, &fsMode) == nullptr,
        "CreateDeviceEx windowed with fullscreen mode rejected");

  checkEq(d3d->Release(), 0u, "factory ex release");
}

}  // namespace

int main() {
  try {
    testCheckDeviceTypeValidation();
    testCheckDeviceMultiSampleTypeValidation();
    testExFactoryValidation();
    testCreateDeviceExModeValidation();
  } catch (const TestFailure& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  } catch (const std::exception& error) {
    std::cerr << "unexpected exception: " << error.what() << '\n';
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
