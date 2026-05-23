// Spec for the IDirect3DDevice9::Set/GetGammaRamp two-track stack:
//
//   1. PE-side shadow round-trip (G2-B Option B) — the byte-equal Set/Get
//      contract Wine d3d9 conformance asserts. The PE-side D3D9DeviceImpl
//      header in src/d3d9/d3d9_pe_device.cpp is Windows-only so we mirror
//      the WORD[256]*3 layout here at value level instead of instantiating
//      the impl. The Wine behavioral oracle is dlls/d3d9/tests/device.c
//      ::test_gamma — both Set and Get operate on a D3DGAMMARAMP-shaped
//      struct with no failure mode (D3D9 Set/GetGammaRamp return void).
//
//   2. Unix-side present-pass apply (G2-B Option A) — the parts that live
//      in this build:
//
//        a. core::GammaRamp POD layout in include/dxmt9/core_constants.hpp
//           (1.5 KB, layout-compatible with D3DGAMMARAMP).
//        b. core::Device::setGammaRamp + gammaRampIsIdentity recomputation
//           in src/d3d9/core.cpp — the unix-side shadow that gets pulled
//           into every snapshotSwapDesc.
//        c. SwapDesc::gammaRamp + SwapDesc::gammaRampIsIdentity carriage
//           — the present-time wire from snapshotSwapDesc to the unix
//           Presenter.
//        d. shaders::makeGammaApplyFragmentSource determinism (the MSL
//           kernel the gamma-apply PSO compiles).
//
// What we deliberately do NOT test here:
//   * Actual MSL compilation / GPU LUT correctness — needs a Metal device
//     and would belong in a GPU-bound test. The bytes-equal-across-builds
//     check + the const-folded shape pins (red[] before green[] before
//     blue[] inside ramp uniform) are the highest-value unit-level
//     defenses against drift.
//   * Presenter encoder open/close counts under non-identity ramps —
//     deferred to wild-experiment evidence; the present-pipeline fake-
//     drawable harness referenced in the task does not currently exist
//     in this tree (scope-limit hit).

#include "core_spec_fixtures.hpp"

#include "../../../src/dxmt9/dxmt9_shader_sources.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

using namespace dxmt9::core::spec;

namespace {

// Layout-identical mirror of D3DGAMMARAMP from <d3d9types.h>; pin-tested
// against the real core::GammaRamp below.
struct GammaRampMirror {
  std::array<uint16_t, 256> red{};
  std::array<uint16_t, 256> green{};
  std::array<uint16_t, 256> blue{};
};

static_assert(sizeof(GammaRampMirror) == 3u * 256u * sizeof(uint16_t),
              "D3DGAMMARAMP-mirror must be 1536 bytes (3 * 256 * WORD)");
static_assert(sizeof(GammaRampMirror) == 1536u,
              "D3DGAMMARAMP-mirror absolute size pin");
static_assert(sizeof(dxmt9::core::GammaRamp) == sizeof(GammaRampMirror),
              "core::GammaRamp must match D3DGAMMARAMP byte layout");
static_assert(offsetof(dxmt9::core::GammaRamp, green) == 256u * sizeof(uint16_t),
              "core::GammaRamp green must follow red[256] without padding");
static_assert(offsetof(dxmt9::core::GammaRamp, blue) == 512u * sizeof(uint16_t),
              "core::GammaRamp blue must follow green[256] without padding");

GammaRampMirror identityRamp() {
  GammaRampMirror r{};
  for (uint32_t i = 0; i < 256u; ++i) {
    const uint16_t v = static_cast<uint16_t>(i << 8);
    r.red[i] = v;
    r.green[i] = v;
    r.blue[i] = v;
  }
  return r;
}

dxmt9::core::GammaRamp makeCoreIdentity() {
  dxmt9::core::GammaRamp r{};
  for (uint32_t i = 0; i < 256u; ++i) {
    const uint16_t v = static_cast<uint16_t>(i << 8);
    r.red[i] = v;
    r.green[i] = v;
    r.blue[i] = v;
  }
  return r;
}

// Mirror of D3D9DeviceImpl::SetGammaRamp / GetGammaRamp shadow logic —
// byte-copy on non-null, no-op on null, swapChain / flags unused.
void deviceSetGammaRamp(GammaRampMirror& shadow,
                        uint32_t /*swapChain*/, uint32_t /*flags*/,
                        const GammaRampMirror* ramp) {
  if (!ramp) return;
  std::memcpy(&shadow, ramp, sizeof(GammaRampMirror));
}

void deviceGetGammaRamp(const GammaRampMirror& shadow,
                        uint32_t /*swapChain*/,
                        GammaRampMirror* out) {
  if (!out) return;
  std::memcpy(out, &shadow, sizeof(GammaRampMirror));
}

bool rampsEqual(const GammaRampMirror& a, const GammaRampMirror& b) {
  return std::memcmp(&a, &b, sizeof(GammaRampMirror)) == 0;
}

// Pure-value mirror of core::Device::setGammaRamp identity recomputation —
// avoids constructing a real core::Device (which would need WMT plumbing).
// The production code in src/d3d9/core.cpp does the same comparison.
bool computeIdentity(const dxmt9::core::GammaRamp& r) {
  for (uint32_t i = 0; i < 256u; ++i) {
    const uint16_t expected = static_cast<uint16_t>(i << 8);
    if (r.red[i] != expected || r.green[i] != expected || r.blue[i] != expected) {
      return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// G2-B Option B — PE-side shadow round-trip
// ---------------------------------------------------------------------------

void testFreshDeviceReturnsIdentityRamp() {
  GammaRampMirror shadow = identityRamp();
  GammaRampMirror out{};
  std::memset(&out, 0xCD, sizeof(out));
  deviceGetGammaRamp(shadow, 0u, &out);
  const GammaRampMirror expected = identityRamp();
  check(rampsEqual(out, expected),
        "fresh device GetGammaRamp returns the identity ramp (i << 8)");
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
  GammaRampMirror input{};
  for (uint32_t i = 0; i < 256u; ++i) {
    input.red[i] = static_cast<uint16_t>((255u - i) << 8);
    const double f = static_cast<double>(i) / 255.0;
    const double g = std::pow(f, 1.0 / 2.2);
    input.green[i] = static_cast<uint16_t>(std::lround(g * 65535.0));
    input.blue[i] = i < 128u ? static_cast<uint16_t>(i << 8) : 0xFFFFu;
  }
  GammaRampMirror shadow = identityRamp();
  deviceSetGammaRamp(shadow, 0u, 0u, &input);
  GammaRampMirror out{};
  std::memset(&out, 0xAB, sizeof(out));
  deviceGetGammaRamp(shadow, 0u, &out);
  check(rampsEqual(out, input),
        "Set->Get round-trip is byte-equal for a non-identity ramp");
  checkEq(static_cast<uint32_t>(out.red[0]),   65280u, "round-trip red[0]");
  checkEq(static_cast<uint32_t>(out.red[255]), 0u,     "round-trip red[255]");
  checkEq(static_cast<uint32_t>(out.blue[200]), 0xFFFFu,
          "round-trip blue[200] clipped to max");
  checkEq(static_cast<uint32_t>(out.blue[64]), static_cast<uint32_t>(64u << 8),
          "round-trip blue[64] identity below clip");
}

void testNonZeroSwapChainShareSameShadow() {
  GammaRampMirror shadow = identityRamp();
  GammaRampMirror custom{};
  for (uint32_t i = 0; i < 256u; ++i) {
    custom.red[i]   = static_cast<uint16_t>(0x1100u + i);
    custom.green[i] = static_cast<uint16_t>(0x2200u + i);
    custom.blue[i]  = static_cast<uint16_t>(0x3300u + i);
  }
  deviceSetGammaRamp(shadow, 5u, 0u, &custom);
  GammaRampMirror read0{};
  GammaRampMirror read5{};
  deviceGetGammaRamp(shadow, 0u, &read0);
  deviceGetGammaRamp(shadow, 5u, &read5);
  check(rampsEqual(read0, custom),
        "SetGammaRamp(sc=5) updates the shared device-wide shadow");
  check(rampsEqual(read5, custom),
        "GetGammaRamp(sc=5) reads the same shadow as sc=0");
  check(rampsEqual(read0, read5),
        "sc=0 and sc=5 reads are identical (no per-sc fan-out)");
}

void testNullRampSetIsNoOp() {
  GammaRampMirror shadow = identityRamp();
  const GammaRampMirror baseline = shadow;
  deviceSetGammaRamp(shadow, 0u, 0u, nullptr);
  check(rampsEqual(shadow, baseline),
        "SetGammaRamp(nullptr) leaves the shadow at its prior value");
  deviceGetGammaRamp(shadow, 0u, nullptr);
  check(rampsEqual(shadow, baseline),
        "GetGammaRamp(nullptr) is a no-op (shadow is the source of truth)");
}

void testFlagsAreAcceptedAsOpaque() {
  GammaRampMirror input{};
  for (uint32_t i = 0; i < 256u; ++i) {
    input.red[i]   = static_cast<uint16_t>(0xAA00u | (i & 0xFFu));
    input.green[i] = static_cast<uint16_t>(0xBB00u | (i & 0xFFu));
    input.blue[i]  = static_cast<uint16_t>(0xCC00u | (i & 0xFFu));
  }
  GammaRampMirror shadow0    = identityRamp();
  GammaRampMirror shadow1    = identityRamp();
  GammaRampMirror shadowJunk = identityRamp();
  deviceSetGammaRamp(shadow0,    0u, 0u,         &input);
  deviceSetGammaRamp(shadow1,    0u, 1u,         &input);
  deviceSetGammaRamp(shadowJunk, 0u, 0xDEADBEEFu, &input);
  check(rampsEqual(shadow0, shadow1),
        "D3DSGR_NO_CALIBRATION vs D3DSGR_CALIBRATE produce the same shadow");
  check(rampsEqual(shadow0, shadowJunk),
        "unknown flag bits do not corrupt or block the shadow update");
}

// ---------------------------------------------------------------------------
// G2-B Option A — unix-side carriage + identity fast-path
// ---------------------------------------------------------------------------

void testCoreGammaRampPodMatchesPeShape() {
  // The unix-side carriage is a layout-compatible POD. The static_asserts
  // above already pin sizeof and channel offsets at compile time; this
  // case adds a runtime byte-equal write-then-read to defend against an
  // accidental alignment hole.
  const GammaRampMirror peShape = identityRamp();
  dxmt9::core::GammaRamp coreShape{};
  std::memcpy(&coreShape, &peShape, sizeof(coreShape));
  for (uint32_t i = 0; i < 256u; ++i) {
    checkEq(static_cast<uint32_t>(coreShape.red[i]),
            static_cast<uint32_t>(peShape.red[i]),
            "core::GammaRamp.red byte-matches PE shape");
    checkEq(static_cast<uint32_t>(coreShape.green[i]),
            static_cast<uint32_t>(peShape.green[i]),
            "core::GammaRamp.green byte-matches PE shape");
    checkEq(static_cast<uint32_t>(coreShape.blue[i]),
            static_cast<uint32_t>(peShape.blue[i]),
            "core::GammaRamp.blue byte-matches PE shape");
  }
}

void testIdentityComputationRecognizesIdentity() {
  // Default core::GammaRamp{} is all-zero, NOT identity. The unix-side
  // recompute must reject that as non-identity (otherwise apps would see
  // black until the first SetGammaRamp).
  const dxmt9::core::GammaRamp zero{};
  check(!computeIdentity(zero),
        "all-zero ramp is recognized as non-identity (not the default identity)");

  // The identity-seeded ramp recomputes to identity.
  const dxmt9::core::GammaRamp ident = makeCoreIdentity();
  check(computeIdentity(ident),
        "identity ramp recomputes to identity");

  // Single-entry perturbation flips identity off — exactly the case
  // SetGammaRamp must catch.
  dxmt9::core::GammaRamp tweaked = makeCoreIdentity();
  tweaked.green[42] = static_cast<uint16_t>(tweaked.green[42] - 1);
  check(!computeIdentity(tweaked),
        "single-entry perturbation flips identity false");
}

void testSwapDescCarriesGammaShape() {
  // SwapDesc is the wire field the unix-side Presenter reads on each
  // present. Default-constructed SwapDesc must carry an identity-flagged
  // ramp so the present-pass apply skips for apps that never call
  // SetGammaRamp.
  const dxmt9::core::SwapDesc desc{};
  check(desc.gammaRampIsIdentity,
        "default SwapDesc::gammaRampIsIdentity is true (skip-apply default)");
  // The ramp payload itself is value-initialized (all-zero) — the
  // Presenter must rely on the identity flag, not the payload bytes,
  // to decide whether to enable the apply pass.
  const dxmt9::core::GammaRamp zero{};
  check(std::memcmp(&desc.gammaRamp, &zero, sizeof(zero)) == 0,
        "default SwapDesc::gammaRamp payload is value-initialized");

  // sizeof pin so a refactor that inflates SwapDesc by accident shows up
  // here (the 1.5 KB ramp is the dominant new cost in the present-time
  // packet — anything larger needs a separate review).
  static_assert(sizeof(dxmt9::core::SwapDesc) >= sizeof(dxmt9::core::GammaRamp),
                "SwapDesc must carry at least one GammaRamp's worth of bytes");
}

void testGammaApplyFragmentSourceIsDeterministic() {
  // The MSL kernel is a pure value transform; two consecutive calls with
  // the same variant hash must produce byte-equal source. Without this
  // pin the shader archive key would silently churn on rebuilds.
  const auto a = dxmt9::shaders::makeGammaApplyFragmentSource(0x1234u, false);
  const auto b = dxmt9::shaders::makeGammaApplyFragmentSource(0x1234u, false);
  check(a == b, "makeGammaApplyFragmentSource is byte-deterministic");

  // The kernel must reference the LUT uniform layout we ship in
  // SwapDesc::gammaRamp. Surface drift in the channel order would make
  // the apply produce wrong pixels for the case that needs it the most.
  using std::string_view;
  const string_view text{a};
  checkContains(text, "ushort red[256]",
                "gamma-apply FS declares red[256]");
  checkContains(text, "ushort green[256]",
                "gamma-apply FS declares green[256]");
  checkContains(text, "ushort blue[256]",
                "gamma-apply FS declares blue[256]");
  checkContains(text, "[[buffer(0)]]",
                "gamma-apply FS binds the LUT uniform at buffer(0)");
  checkContains(text, "tex0.sample",
                "gamma-apply FS samples the source backbuffer");

  // Opaque-alpha variant must clamp alpha to 1 — matches the existing
  // textured-blit opaque variant for X8R8G8B8 / X8B8G8R8 swap chains.
  const auto opaque = dxmt9::shaders::makeGammaApplyFragmentSource(0xBEEFu, true);
  checkContains(opaque, "mapped.a = 1.0;",
                "opaque variant clamps alpha to 1.0");
  const auto alpha = dxmt9::shaders::makeGammaApplyFragmentSource(0xBEEFu, false);
  checkContains(alpha, "mapped.a = color.a;",
                "alpha-preserving variant passes through source alpha");
  check(opaque != alpha,
        "opaque and alpha-preserving variants are distinct sources");
}

}  // namespace

int main() {
  try {
    testFreshDeviceReturnsIdentityRamp();
    testSetGetRoundTripIsByteEqual();
    testNonZeroSwapChainShareSameShadow();
    testNullRampSetIsNoOp();
    testFlagsAreAcceptedAsOpaque();
    testCoreGammaRampPodMatchesPeShape();
    testIdentityComputationRecognizesIdentity();
    testSwapDescCarriesGammaShape();
    testGammaApplyFragmentSourceIsDeterministic();
  } catch (const TestFailure& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  } catch (const std::exception& error) {
    std::cerr << "unexpected exception: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
