// Pure value-level spec for the IDirect3DDevice9::Set/GetGammaRamp
// PE-side shadow that lives in src/d3d9/d3d9_pe_device.cpp
// (D3D9DeviceImpl::gammaRamp_).
//
// Why a value-level spec?  d3d9_pe_device.cpp is built only on the PE
// (Windows) side (it includes <windows.h> / <d3d9.h>) so native
// dxmt9-core-* tests cannot instantiate D3D9DeviceImpl directly.  The
// shadow's behavior is small and self-contained — initialize to the
// identity ramp, byte-copy on Set, byte-copy on Get — so we mirror it
// here in a struct identical in layout to D3DGAMMARAMP
// (3 * 256 * WORD).  The Wine behavioral oracle for the round-trip
// shape is dlls/d3d9/tests/device.c::test_gamma — both Set and Get
// operate on a D3DGAMMARAMP-shaped struct with no failure mode (D3D9
// Set/GetGammaRamp return void).
//
// What this spec pins (matching d3d9_pe_device.cpp G1-4 Option B impl):
//   1.  A fresh device returns the identity ramp (ramp[i] = i << 8 per
//       channel) on GetGammaRamp.  This is the historical wined3d
//       "orig_gamma" baseline; apps that probe gamma without ever
//       calling Set still see a sensible value (and not the all-zero
//       memset that the prior stub returned).
//   2.  SetGammaRamp followed by GetGammaRamp produces a byte-equal
//       round-trip for any well-formed input ramp.
//   3.  SetGammaRamp(iSwapChain != 0) is non-fatal and shares the same
//       shadow — matches the void return signature and Wine's
//       wined3d_swapchain_set_gamma_ramp shape (the per-output
//       forwarding is invisible to the D3D9 caller, which gets no
//       HRESULT either way).
//   4.  SetGammaRamp(ramp = nullptr) does not crash and leaves the
//       prior shadow intact.  Wine d3d9.dll forwards a null ramp to
//       wined3d which dereferences it; dxmt9 chooses to no-op
//       defensively since the API has no error channel.
//   5.  The unused D3DSGR_NO_CALIBRATION / D3DSGR_CALIBRATE flag bits
//       are accepted without filtering — Wine wined3d logs a FIXME but
//       does not reject; we match.

#include "core_spec_fixtures.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

using namespace dxmt9::core::spec;

namespace {

// Layout-identical mirror of D3DGAMMARAMP from <d3d9types.h>.
// PE-side D3DGAMMARAMP is { WORD red[256]; WORD green[256]; WORD blue[256]; }.
// We do not pull in <d3d9types.h> here (the headers carry Win32 types
// that are not portable for native specs), so we reproduce the layout
// with explicit fixed-width types.  Size pins below catch drift.
struct GammaRamp {
  std::array<uint16_t, 256> red{};
  std::array<uint16_t, 256> green{};
  std::array<uint16_t, 256> blue{};
};

static_assert(sizeof(GammaRamp) == 3u * 256u * sizeof(uint16_t),
              "D3DGAMMARAMP-mirror must be 1536 bytes (3 * 256 * WORD)");
static_assert(sizeof(GammaRamp) == 1536u,
              "D3DGAMMARAMP-mirror size in absolute bytes");

// Mirrors D3D9DeviceImpl::initGammaRampIdentity() in
// src/d3d9/d3d9_pe_device.cpp.
GammaRamp identityRamp() {
  GammaRamp r{};
  for (uint32_t i = 0; i < 256u; ++i) {
    const uint16_t v = static_cast<uint16_t>(i << 8);
    r.red[i] = v;
    r.green[i] = v;
    r.blue[i] = v;
  }
  return r;
}

// Mirrors D3D9DeviceImpl::SetGammaRamp: byte-copy on non-null,
// no-op on null, swapChain / flags unused for the shadow.
void deviceSetGammaRamp(GammaRamp& shadow,
                        uint32_t /*swapChain*/, uint32_t /*flags*/,
                        const GammaRamp* ramp) {
  if (!ramp) return;
  std::memcpy(&shadow, ramp, sizeof(GammaRamp));
}

// Mirrors D3D9DeviceImpl::GetGammaRamp: byte-copy on non-null, no-op
// on null.
void deviceGetGammaRamp(const GammaRamp& shadow,
                        uint32_t /*swapChain*/,
                        GammaRamp* out) {
  if (!out) return;
  std::memcpy(out, &shadow, sizeof(GammaRamp));
}

bool ramps_equal(const GammaRamp& a, const GammaRamp& b) {
  return std::memcmp(&a, &b, sizeof(GammaRamp)) == 0;
}

// ---------------------------------------------------------------------------

void testFreshDeviceReturnsIdentityRamp() {
  // Mirror the PE-side constructor: the gamma ramp shadow is
  // initialized to identity via initGammaRampIdentity() before any
  // app-side Set call.  GetGammaRamp on a fresh device must therefore
  // return the identity ramp byte-for-byte, NOT the all-zero memset
  // that the prior stub returned.
  GammaRamp shadow = identityRamp();

  GammaRamp out{};
  // Pre-fill with a sentinel so an accidental no-op Get is caught.
  std::memset(&out, 0xCD, sizeof(out));
  deviceGetGammaRamp(shadow, 0u, &out);

  const GammaRamp expected = identityRamp();
  check(ramps_equal(out, expected),
        "fresh device GetGammaRamp returns the identity ramp (i << 8)");

  // Concrete value pins on the identity table — defends against a
  // future refactor that swaps the channel order or the WORD scale.
  checkEq(static_cast<uint32_t>(out.red[0]),    0u,     "identity red[0]");
  checkEq(static_cast<uint32_t>(out.red[1]),    256u,   "identity red[1]");
  checkEq(static_cast<uint32_t>(out.red[128]),  32768u, "identity red[128]");
  checkEq(static_cast<uint32_t>(out.red[255]),  65280u, "identity red[255]");
  checkEq(static_cast<uint32_t>(out.green[0]),    0u,     "identity green[0]");
  checkEq(static_cast<uint32_t>(out.green[255]),  65280u, "identity green[255]");
  checkEq(static_cast<uint32_t>(out.blue[0]),    0u,     "identity blue[0]");
  checkEq(static_cast<uint32_t>(out.blue[255]),  65280u, "identity blue[255]");
}

void testSetGetRoundTripIsByteEqual() {
  // Construct a non-identity ramp with three different per-channel
  // shapes (inverted red, gamma-corrected green, clipped blue) so a
  // channel-mixup regression shows up.
  GammaRamp input{};
  for (uint32_t i = 0; i < 256u; ++i) {
    // Inverted linear ramp on red.
    input.red[i] = static_cast<uint16_t>((255u - i) << 8);
    // sRGB-ish gamma 2.2 placeholder on green.
    const double f = static_cast<double>(i) / 255.0;
    const double g = std::pow(f, 1.0 / 2.2);
    input.green[i] = static_cast<uint16_t>(std::lround(g * 65535.0));
    // Hard-clip on blue: identity below 0x80, 0xFFFF above.
    input.blue[i] = i < 128u ? static_cast<uint16_t>(i << 8) : 0xFFFFu;
  }

  GammaRamp shadow = identityRamp();
  deviceSetGammaRamp(shadow, /*swapChain=*/0u, /*flags=*/0u, &input);

  GammaRamp out{};
  std::memset(&out, 0xAB, sizeof(out));
  deviceGetGammaRamp(shadow, /*swapChain=*/0u, &out);

  check(ramps_equal(out, input),
        "Set->Get round-trip is byte-equal for a non-identity ramp");

  // Pin a handful of representative values so a partial-copy bug
  // (e.g. only first channel) does not slip past memcmp.
  checkEq(static_cast<uint32_t>(out.red[0]),   65280u,
          "round-trip red[0] inverted");
  checkEq(static_cast<uint32_t>(out.red[255]), 0u,
          "round-trip red[255] inverted");
  checkEq(static_cast<uint32_t>(out.blue[200]), 0xFFFFu,
          "round-trip blue[200] clipped to max");
  checkEq(static_cast<uint32_t>(out.blue[64]),  static_cast<uint32_t>(64u << 8),
          "round-trip blue[64] identity below clip");
}

void testNonZeroSwapChainShareSameShadow() {
  // D3D9 SetGammaRamp returns void — there is no error channel for
  // "invalid swapchain index".  The PE-side impl therefore accepts
  // any swapChain value and stores into the single device-wide
  // shadow.  GetGammaRamp on any swapChain returns the same shadow.
  // This pins that behavior so a future refactor that adds
  // per-swapchain shadows explicitly opts in.
  GammaRamp shadow = identityRamp();
  GammaRamp custom{};
  for (uint32_t i = 0; i < 256u; ++i) {
    custom.red[i]   = static_cast<uint16_t>(0x1100u + i);
    custom.green[i] = static_cast<uint16_t>(0x2200u + i);
    custom.blue[i]  = static_cast<uint16_t>(0x3300u + i);
  }
  deviceSetGammaRamp(shadow, /*swapChain=*/5u, /*flags=*/0u, &custom);

  GammaRamp read0{};
  GammaRamp read5{};
  deviceGetGammaRamp(shadow, 0u, &read0);
  deviceGetGammaRamp(shadow, 5u, &read5);

  check(ramps_equal(read0, custom),
        "SetGammaRamp(sc=5) updates the shared device-wide shadow");
  check(ramps_equal(read5, custom),
        "GetGammaRamp(sc=5) reads the same shadow as sc=0");
  check(ramps_equal(read0, read5),
        "sc=0 and sc=5 reads are identical (no per-sc fan-out)");
}

void testNullRampSetIsNoOp() {
  // SetGammaRamp signature is void, callers cannot detect a null-ramp
  // rejection.  The PE-side impl chooses to no-op rather than crash;
  // the shadow must remain intact.
  GammaRamp shadow = identityRamp();
  const GammaRamp baseline = shadow;
  deviceSetGammaRamp(shadow, /*swapChain=*/0u, /*flags=*/0u, nullptr);
  check(ramps_equal(shadow, baseline),
        "SetGammaRamp(nullptr) leaves the shadow at its prior value");

  // GetGammaRamp with a null out must also no-op.
  deviceGetGammaRamp(shadow, /*swapChain=*/0u, nullptr);
  check(ramps_equal(shadow, baseline),
        "GetGammaRamp(nullptr) is a no-op (shadow is the source of truth)");
}

void testFlagsAreAcceptedAsOpaque() {
  // D3DSGR_NO_CALIBRATION (0) and D3DSGR_CALIBRATE (1) are the only
  // documented flag bits.  Wine wined3d ignores unknown bits with a
  // FIXME log; dxmt9 matches by accepting the flag value without
  // filtering, since macOS has no calibrator hookup either way.  The
  // value-level pin is: the shadow update is independent of `flags`.
  GammaRamp input{};
  for (uint32_t i = 0; i < 256u; ++i) {
    input.red[i]   = static_cast<uint16_t>(0xAA00u | (i & 0xFFu));
    input.green[i] = static_cast<uint16_t>(0xBB00u | (i & 0xFFu));
    input.blue[i]  = static_cast<uint16_t>(0xCC00u | (i & 0xFFu));
  }

  GammaRamp shadow0    = identityRamp();
  GammaRamp shadow1    = identityRamp();
  GammaRamp shadowJunk = identityRamp();
  deviceSetGammaRamp(shadow0,    0u, /*D3DSGR_NO_CALIBRATION=*/0u, &input);
  deviceSetGammaRamp(shadow1,    0u, /*D3DSGR_CALIBRATE     =*/1u, &input);
  deviceSetGammaRamp(shadowJunk, 0u, /*unknown              =*/0xDEADBEEFu, &input);

  check(ramps_equal(shadow0, shadow1),
        "D3DSGR_NO_CALIBRATION vs D3DSGR_CALIBRATE produce the same shadow");
  check(ramps_equal(shadow0, shadowJunk),
        "unknown flag bits do not corrupt or block the shadow update");
}

}  // namespace

int main() {
  try {
    testFreshDeviceReturnsIdentityRamp();
    testSetGetRoundTripIsByteEqual();
    testNonZeroSwapChainShareSameShadow();
    testNullRampSetIsNoOp();
    testFlagsAreAcceptedAsOpaque();
  } catch (const TestFailure& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  } catch (const std::exception& error) {
    std::cerr << "unexpected exception: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
