// Pure value-level spec for the PE-side IDirect3DDevice9(Ex) validation
// helpers that live in src/d3d9/d3d9_pe_device.cpp:
//   * pePresentParamsHResult       — Reset / ResetEx / CreateAdditionalSwapChain
//                                     present-parameter validation
//   * peResetExModeHResult         — ResetEx windowed / fullscreen mode rules
//   * peNormalizeBackBufferCount   — BackBufferCount=0 -> 1 normalization
//   * peQueryDataSizeForType       — IDirect3DQuery9::GetDataSize per-type size
//   * peTextureLevelCountHResult   — CreateTexture / CreateVolumeTexture /
//                                     CreateCubeTexture dimension/Levels policy
//
// Why a value-level spec?  d3d9_pe_device.cpp is built only on the PE
// (Windows) side (it includes <windows.h> / <d3d9.h>) so native
// dxmt9-core-* tests cannot instantiate D3D9DeviceImpl directly.  The
// established pattern (core_d3d9_gamma_ramp_spec.cpp,
// core_d3d9_multiply_transform_spec.cpp) is to mirror the small,
// self-contained PE-side pure logic here and pin its observable contract.
//
// Behavioral oracle — the EXISTING PE conformance functions encode the
// Wine-oracle expectations (read, not modified):
//   * tests/conformance/d3d9/d3d9_conformance_swapchain.c:
//       test_present_parameter_validation       (CreateDevice present rules,
//                                                 mirrored at Reset/ResetEx/
//                                                 CreateAdditionalSwapChain)
//       test_present_parameter_normalization     (BackBufferCount 0 -> 1)
//   * tests/conformance/d3d9/d3d9_conformance_device.c:
//       test_ex_create_reset_mode_validation      (ResetEx mode rules)
//       test_query_get_data_size_policy            (GetDataSize > 0 per type)
//   * tests/conformance/d3d9/d3d9_queries.cpp:
//       occlusion_query_public_sizes  (OCCLUSION       -> sizeof(DWORD)  = 4)
//       timestamp_query_public_sizes  (TIMESTAMP/FREQ  -> sizeof(UINT64) = 8,
//                                      TIMESTAMPDISJOINT-> sizeof(BOOL)   = 4)
//   * tests/conformance/d3d9/d3d9_conformance_resource.c:
//       test_create_cube_texture_dim_policy (edge==0 and Levels beyond
//                                            floor(log2(edge))+1 -> INVALIDCALL)
//
// Wine commit 6e073d28dee3af7f4c965daec94644e0f9f92727.

#include "core_spec_fixtures.hpp"

#include <cstdint>

using namespace dxmt9::core::spec;

namespace {

// ---------------------------------------------------------------------------
// Layout-identical mirrors of the D3D9 enums / HRESULTs used by the PE-side
// validators.  We do not pull in <d3d9.h> here (Win32-only header); the
// validators take primitive scalars precisely so they can be mirrored
// portably and pinned at value level.
// ---------------------------------------------------------------------------

constexpr int32_t kD3D_OK = 0;
constexpr int32_t kD3DERR_INVALIDCALL = static_cast<int32_t>(0x8876086cu);

// D3DSWAPEFFECT (<d3d9types.h>): DISCARD=1, FLIP=2, COPY=3, OVERLAY=4,
// FLIPEX=5.  0 is invalid.
constexpr uint32_t kSwapDiscard = 1u;
constexpr uint32_t kSwapFlip = 2u;
constexpr uint32_t kSwapCopy = 3u;
constexpr uint32_t kSwapFlipEx = 5u;

// D3DMULTISAMPLE_TYPE (<d3d9types.h>): NONE=0, NONMASKABLE=1, 2_SAMPLES=2,
// ... 16_SAMPLES=16.  The validator only needs NONE vs non-NONE.
constexpr uint32_t kMsNone = 0u;
constexpr uint32_t kMs2 = 2u;
constexpr uint32_t kMs4 = 4u;

// D3DPRESENT_INTERVAL_* (<d3d9.h>): DEFAULT=0, ONE=1, TWO=2, THREE=4,
// FOUR=8, IMMEDIATE=0x80000000.
constexpr uint32_t kIntervalDefault = 0u;
constexpr uint32_t kIntervalOne = 0x00000001u;
constexpr uint32_t kIntervalTwo = 0x00000002u;
constexpr uint32_t kIntervalThree = 0x00000004u;
constexpr uint32_t kIntervalFour = 0x00000008u;
constexpr uint32_t kIntervalImmediate = 0x80000000u;

// D3DQUERYTYPE (<d3d9types.h>): EVENT=8, OCCLUSION=9, TIMESTAMP=10,
// TIMESTAMPDISJOINT=11, TIMESTAMPFREQ=12.
constexpr uint32_t kQueryEvent = 8u;
constexpr uint32_t kQueryOcclusion = 9u;
constexpr uint32_t kQueryTimestamp = 10u;
constexpr uint32_t kQueryTimestampDisjoint = 11u;
constexpr uint32_t kQueryTimestampFreq = 12u;

// ---------------------------------------------------------------------------
// Mirrors of the PE-side pure validators (src/d3d9/d3d9_pe_device.cpp).
// Kept byte-for-byte equivalent in logic; the implementation file is the
// source of truth.  Any drift here or there is a regression.
// ---------------------------------------------------------------------------

bool mirrorIsValidPresentationIntervalRaw(uint32_t interval) {
  return interval == kIntervalDefault || interval == kIntervalOne ||
         interval == kIntervalTwo || interval == kIntervalThree ||
         interval == kIntervalFour || interval == kIntervalImmediate;
}

// Mirrors pePresentParamsHResult().  multiSampleType / multiSampleQuality
// encode the Windows D3D9 multisample-vs-swap-effect contract: a
// multisampled swap chain requires D3DSWAPEFFECT_DISCARD, and a non-zero
// MultiSampleQuality requires a non-NONE MultiSampleType.
int32_t mirrorPresentParamsHResult(uint32_t swapEffect, uint32_t backBufferCount,
                                   uint32_t presentationInterval,
                                   uint32_t multiSampleType,
                                   uint32_t multiSampleQuality, bool extended) {
  switch (swapEffect) {
  case kSwapDiscard:
  case kSwapFlip:
  case kSwapCopy:
    break;
  case kSwapFlipEx:
    if (extended) break;
    return kD3DERR_INVALIDCALL;
  default:
    return kD3DERR_INVALIDCALL;
  }

  const uint32_t maxBackBufferCount = extended ? 30u : 3u;
  if (backBufferCount > maxBackBufferCount) {
    return kD3DERR_INVALIDCALL;
  }
  if (swapEffect == kSwapCopy && backBufferCount > 1u) {
    return kD3DERR_INVALIDCALL;
  }
  if (!mirrorIsValidPresentationIntervalRaw(presentationInterval)) {
    return kD3DERR_INVALIDCALL;
  }
  if (multiSampleType != kMsNone && swapEffect != kSwapDiscard) {
    return kD3DERR_INVALIDCALL;
  }
  if (multiSampleType == kMsNone && multiSampleQuality != 0u) {
    return kD3DERR_INVALIDCALL;
  }
  return kD3D_OK;
}

// Mirrors peResetExModeHResult().
//   sizeofMode == 0 means "caller passed NULL mode pointer".
constexpr uint32_t kSizeofDisplayModeEx = 24u;  // pinned below in spec note

int32_t mirrorResetExModeHResult(bool windowed, bool hasMode, uint32_t modeSize,
                                 uint32_t modeW, uint32_t modeH, uint32_t ppW,
                                 uint32_t ppH) {
  if (hasMode && modeSize != kSizeofDisplayModeEx) {
    return kD3DERR_INVALIDCALL;
  }
  // Windowed must pass NULL mode; fullscreen must pass a non-NULL mode.
  if (windowed ? hasMode : !hasMode) {
    return kD3DERR_INVALIDCALL;
  }
  if (hasMode && (modeW != ppW || modeH != ppH)) {
    return kD3DERR_INVALIDCALL;
  }
  return kD3D_OK;
}

// Mirrors peNormalizeBackBufferCount().
uint32_t mirrorNormalizeBackBufferCount(uint32_t count) {
  return count == 0u ? 1u : count;
}

// Mirrors peQueryDataSizeForType().  Sizes from the Wine oracle:
//   EVENT             -> sizeof(BOOL)   = 4
//   OCCLUSION         -> sizeof(DWORD)  = 4
//   TIMESTAMP         -> sizeof(UINT64) = 8
//   TIMESTAMPDISJOINT -> sizeof(BOOL)   = 4
//   TIMESTAMPFREQ     -> sizeof(UINT64) = 8
uint32_t mirrorQueryDataSizeForType(uint32_t type) {
  switch (type) {
  case kQueryEvent:
    return 4u;  // BOOL
  case kQueryOcclusion:
    return 4u;  // DWORD
  case kQueryTimestamp:
    return 8u;  // UINT64
  case kQueryTimestampDisjoint:
    return 4u;  // BOOL
  case kQueryTimestampFreq:
    return 8u;  // UINT64
  default:
    return 0u;
  }
}

// Mirrors peFullMipLevelCount() / peTextureLevelCountHResult(): the D3D9
// mip-chain policy shared by CreateTexture / CreateVolumeTexture /
// CreateCubeTexture.  Levels == 0 means "full chain" and is always
// accepted; an explicit Levels beyond floor(log2(maxDimension))+1 is
// D3DERR_INVALIDCALL, and a 0 dimension is always invalid.
uint32_t mirrorFullMipLevelCount(uint32_t maxDimension) {
  uint32_t dimension = maxDimension;
  uint32_t levels = 1u;
  while (dimension > 1u) {
    dimension >>= 1u;
    ++levels;
  }
  return levels;
}

int32_t mirrorTextureLevelCountHResult(uint32_t minDimension, uint32_t maxDimension,
                                       uint32_t levels) {
  if (minDimension == 0u) return kD3DERR_INVALIDCALL;
  if (levels != 0u && levels > mirrorFullMipLevelCount(maxDimension)) {
    return kD3DERR_INVALIDCALL;
  }
  return kD3D_OK;
}

// ---------------------------------------------------------------------------

void testPresentParamsValidation() {
  // Exact cases from test_present_parameter_validation (windowed lane).
  // {windowed, count, swap, interval} -> expected HRESULT.
  // swap=0 is not a valid effect.
  checkEq(mirrorPresentParamsHResult(0u, 1u, kIntervalImmediate, kMsNone, 0u, false),
          kD3DERR_INVALIDCALL, "swap effect 0 is invalid");
  // COPY + count 2 -> INVALIDCALL.
  checkEq(mirrorPresentParamsHResult(kSwapCopy, 2u, kIntervalImmediate, kMsNone, 0u, false),
          kD3DERR_INVALIDCALL, "COPY allows only 1 back buffer");
  // DISCARD + count 4 -> INVALIDCALL (> 3 for non-extended).
  checkEq(mirrorPresentParamsHResult(kSwapDiscard, 4u, kIntervalImmediate, kMsNone, 0u, false),
          kD3DERR_INVALIDCALL, "non-extended back buffer count cap is 3");
  // FLIPEX on a non-extended device -> INVALIDCALL.
  checkEq(mirrorPresentParamsHResult(kSwapFlipEx, 1u, kIntervalImmediate, kMsNone, 0u, false),
          kD3DERR_INVALIDCALL, "FLIPEX requires an extended device");
  // DISCARD + bogus interval 5 -> INVALIDCALL.
  checkEq(mirrorPresentParamsHResult(kSwapDiscard, 1u, 5u, kMsNone, 0u, false),
          kD3DERR_INVALIDCALL, "interval 5 is not a valid presentation interval");
  // COPY + count 0 -> OK (0 is within COPY's <=1 allowance).
  checkEq(mirrorPresentParamsHResult(kSwapCopy, 0u, kIntervalImmediate, kMsNone, 0u, false),
          kD3D_OK, "COPY + count 0 is valid");
  // DISCARD + count 3 -> OK (at the non-extended cap).
  checkEq(mirrorPresentParamsHResult(kSwapDiscard, 3u, kIntervalImmediate, kMsNone, 0u, false),
          kD3D_OK, "DISCARD + count 3 is at the cap and valid");
}

void testPresentParamsExtendedLane() {
  // FLIPEX is allowed only on an extended device.
  checkEq(mirrorPresentParamsHResult(kSwapFlipEx, 1u, kIntervalImmediate, kMsNone, 0u, true),
          kD3D_OK, "FLIPEX is valid on an extended device");
  // Extended back buffer cap is 30.
  checkEq(mirrorPresentParamsHResult(kSwapDiscard, 30u, kIntervalDefault, kMsNone, 0u, true),
          kD3D_OK, "extended back buffer cap is 30");
  checkEq(mirrorPresentParamsHResult(kSwapDiscard, 31u, kIntervalDefault, kMsNone, 0u, true),
          kD3DERR_INVALIDCALL, "extended back buffer count 31 exceeds the cap");
  // COPY's <=1 rule still applies on an extended device.
  checkEq(mirrorPresentParamsHResult(kSwapCopy, 2u, kIntervalImmediate, kMsNone, 0u, true),
          kD3DERR_INVALIDCALL, "COPY allows only 1 back buffer even when extended");
}

void testPresentParamsValidIntervals() {
  // All six documented intervals are accepted; everything else is rejected.
  const uint32_t valid[] = {kIntervalDefault, kIntervalOne, kIntervalTwo,
                            kIntervalThree, kIntervalFour, kIntervalImmediate};
  for (uint32_t iv : valid) {
    checkEq(mirrorPresentParamsHResult(kSwapDiscard, 1u, iv, kMsNone, 0u, false), kD3D_OK,
            "documented presentation interval is accepted");
  }
  for (uint32_t iv : {3u, 5u, 7u, 16u, 0x40000000u}) {
    checkEq(mirrorPresentParamsHResult(kSwapDiscard, 1u, iv, kMsNone, 0u, false),
            kD3DERR_INVALIDCALL, "undocumented presentation interval is rejected");
  }
}

void testPresentParamsMultiSample() {
  // Windows D3D9 (wined3d_swapchain_state_init): multisampling requires
  // D3DSWAPEFFECT_DISCARD.  test_swapchain_multisample_reset resets with
  // DISCARD + 2_SAMPLES, the only legal combination.
  checkEq(mirrorPresentParamsHResult(kSwapDiscard, 1u, kIntervalDefault, kMs2, 0u, false),
          kD3D_OK, "DISCARD + 2x MSAA is valid");
  checkEq(mirrorPresentParamsHResult(kSwapDiscard, 1u, kIntervalDefault, kMs4, 0u, false),
          kD3D_OK, "DISCARD + 4x MSAA is valid");
  // Non-DISCARD swap effects reject any non-NONE multisample type.
  checkEq(mirrorPresentParamsHResult(kSwapCopy, 1u, kIntervalDefault, kMs2, 0u, false),
          kD3DERR_INVALIDCALL, "COPY + MSAA is rejected");
  checkEq(mirrorPresentParamsHResult(kSwapFlip, 2u, kIntervalDefault, kMs2, 0u, false),
          kD3DERR_INVALIDCALL, "FLIP + MSAA is rejected");
  checkEq(mirrorPresentParamsHResult(kSwapFlipEx, 1u, kIntervalDefault, kMs4, 0u, true),
          kD3DERR_INVALIDCALL, "FLIPEX + MSAA is rejected even on extended");
  // A quality level cannot be requested without a sample type.
  checkEq(mirrorPresentParamsHResult(kSwapDiscard, 1u, kIntervalDefault, kMsNone, 1u, false),
          kD3DERR_INVALIDCALL, "non-zero quality with NONE sample type is rejected");
  // NONE + zero quality is the default and stays valid.
  checkEq(mirrorPresentParamsHResult(kSwapDiscard, 1u, kIntervalDefault, kMsNone, 0u, false),
          kD3D_OK, "NONE + zero quality is valid");
}

void testResetExModeValidation() {
  // Exact cases from test_ex_create_reset_mode_validation (ResetEx lane).
  // (1) Windowed default pp (windowed=TRUE) + non-NULL mode -> INVALIDCALL.
  checkEq(mirrorResetExModeHResult(/*windowed=*/true, /*hasMode=*/true,
                                   kSizeofDisplayModeEx, 800u, 600u, 400u, 300u),
          kD3DERR_INVALIDCALL, "windowed ResetEx must pass a NULL mode");
  // (2) windowed=FALSE + NULL mode -> INVALIDCALL.
  checkEq(mirrorResetExModeHResult(/*windowed=*/false, /*hasMode=*/false, 0u,
                                   0u, 0u, 800u, 600u),
          kD3DERR_INVALIDCALL, "fullscreen ResetEx must pass a non-NULL mode");
  // (3) windowed=FALSE + mode dimensions mismatching the back buffer ->
  //     INVALIDCALL.
  checkEq(mirrorResetExModeHResult(/*windowed=*/false, /*hasMode=*/true,
                                   kSizeofDisplayModeEx, 800u, 600u, 799u, 600u),
          kD3DERR_INVALIDCALL, "fullscreen mode width must match the back buffer");
  // (4) bad_mode with Width=Height=0 (pp keeps the real back buffer size) ->
  //     INVALIDCALL via the dimension-mismatch rule.
  checkEq(mirrorResetExModeHResult(/*windowed=*/false, /*hasMode=*/true,
                                   kSizeofDisplayModeEx, 0u, 0u, 800u, 600u),
          kD3DERR_INVALIDCALL, "zero-dimension mode is rejected");
  // Wrong mode Size -> INVALIDCALL even when everything else is consistent.
  checkEq(mirrorResetExModeHResult(/*windowed=*/false, /*hasMode=*/true,
                                   /*modeSize=*/16u, 800u, 600u, 800u, 600u),
          kD3DERR_INVALIDCALL, "mode Size must equal sizeof(D3DDISPLAYMODEEX)");
  // The final successful ResetEx case: windowed=TRUE + NULL mode -> OK.
  checkEq(mirrorResetExModeHResult(/*windowed=*/true, /*hasMode=*/false, 0u, 0u,
                                   0u, 640u, 480u),
          kD3D_OK, "windowed ResetEx with a NULL mode is valid");
  // Fullscreen with matching mode dimensions -> OK.
  checkEq(mirrorResetExModeHResult(/*windowed=*/false, /*hasMode=*/true,
                                   kSizeofDisplayModeEx, 800u, 600u, 800u, 600u),
          kD3D_OK, "fullscreen ResetEx with a matching mode is valid");
}

void testBackBufferCountNormalization() {
  // test_present_parameter_normalization: BackBufferCount 0 -> 1; any
  // non-zero count is preserved (clamping to the cap is the validator's job).
  checkEq(mirrorNormalizeBackBufferCount(0u), 1u, "count 0 normalizes to 1");
  checkEq(mirrorNormalizeBackBufferCount(1u), 1u, "count 1 is preserved");
  checkEq(mirrorNormalizeBackBufferCount(2u), 2u, "count 2 is preserved");
  checkEq(mirrorNormalizeBackBufferCount(3u), 3u, "count 3 is preserved");
}

void testQueryDataSizePerType() {
  // GetDataSize per-type contract from the query conformance specs.
  checkEq(mirrorQueryDataSizeForType(kQueryEvent), 4u, "EVENT size = sizeof(BOOL)");
  checkEq(mirrorQueryDataSizeForType(kQueryOcclusion), 4u,
          "OCCLUSION size = sizeof(DWORD)");
  checkEq(mirrorQueryDataSizeForType(kQueryTimestamp), 8u,
          "TIMESTAMP size = sizeof(UINT64)");
  checkEq(mirrorQueryDataSizeForType(kQueryTimestampDisjoint), 4u,
          "TIMESTAMPDISJOINT size = sizeof(BOOL)");
  checkEq(mirrorQueryDataSizeForType(kQueryTimestampFreq), 8u,
          "TIMESTAMPFREQ size = sizeof(UINT64)");
  // Every supported type reports a strictly positive size (the oracle's
  // CHECK_TRUE(size > 0) invariant).
  for (uint32_t t : {kQueryEvent, kQueryOcclusion, kQueryTimestamp,
                     kQueryTimestampDisjoint, kQueryTimestampFreq}) {
    check(mirrorQueryDataSizeForType(t) > 0u, "supported query size is > 0");
  }
  // An unsupported type reports 0.
  checkEq(mirrorQueryDataSizeForType(0xdeadbeefu), 0u,
          "unsupported query type reports size 0");
}

void testTextureMipLevelCountPolicy() {
  // test_create_cube_texture_dim_policy: edge length 1 is the minimum legal
  // cube size, and levels=1 is always valid regardless of edge.
  checkEq(mirrorTextureLevelCountHResult(1u, 1u, 1u), kD3D_OK,
          "edge length 1 with levels 1 is valid");
  // Power-of-two edge length, single explicit level, is the canonical
  // happy path.
  checkEq(mirrorTextureLevelCountHResult(64u, 64u, 1u), kD3D_OK,
          "edge length 64 with levels 1 is valid");
  // Edge length == 0 must be rejected regardless of Levels.
  checkEq(mirrorTextureLevelCountHResult(0u, 0u, 1u), kD3DERR_INVALIDCALL,
          "edge length 0 is invalid");
  checkEq(mirrorTextureLevelCountHResult(0u, 0u, 0u), kD3DERR_INVALIDCALL,
          "edge length 0 is invalid even with Levels=0 (full chain)");
  // A single zero axis is invalid even when another axis is large — the
  // 2D/volume call sites pass (min, max), and Metal asserts on any zero axis.
  checkEq(mirrorTextureLevelCountHResult(0u, 64u, 1u), kD3DERR_INVALIDCALL,
          "one zero axis is invalid even beside a large axis");
  // Explicit Levels exceeding floor(log2(edge))+1 must be rejected.  For
  // edge=64 the cap is log2(64)+1=7, so Levels=8 is out of range.
  checkEq(mirrorFullMipLevelCount(64u), 7u, "edge 64 has a 7-level full chain");
  checkEq(mirrorTextureLevelCountHResult(64u, 64u, 7u), kD3D_OK,
          "edge 64 with the exact cap of 7 levels is valid");
  checkEq(mirrorTextureLevelCountHResult(64u, 64u, 8u), kD3DERR_INVALIDCALL,
          "edge 64 with 8 levels exceeds the 7-level cap");
  // The cap follows the largest axis: 16x1 still has a 5-level chain.
  checkEq(mirrorTextureLevelCountHResult(1u, 16u, 5u), kD3D_OK,
          "16x1 with the exact cap of 5 levels is valid");
  checkEq(mirrorTextureLevelCountHResult(1u, 16u, 6u), kD3DERR_INVALIDCALL,
          "16x1 with 6 levels exceeds the 5-level cap");
  // Levels == 0 means "full chain" and is always accepted for a valid
  // (non-zero) dimension.
  checkEq(mirrorTextureLevelCountHResult(64u, 64u, 0u), kD3D_OK,
          "Levels=0 (full chain) is always valid for a non-zero dimension");
  // Non-power-of-two dimensions still resolve a correct full chain and cap.
  checkEq(mirrorFullMipLevelCount(1u), 1u, "edge 1 has a 1-level full chain");
  checkEq(mirrorFullMipLevelCount(3u), 2u, "edge 3 has a 2-level full chain");
  checkEq(mirrorTextureLevelCountHResult(3u, 3u, 2u), kD3D_OK,
          "edge 3 with the exact cap of 2 levels is valid");
  checkEq(mirrorTextureLevelCountHResult(3u, 3u, 3u), kD3DERR_INVALIDCALL,
          "edge 3 with 3 levels exceeds the 2-level cap");
}

constexpr uint32_t kD3DUsageAutogenMipmap = 0x00000400u;
constexpr uint32_t kD3DPoolDefault = 0u;
constexpr uint32_t kD3DPoolManaged = 1u;
constexpr uint32_t kD3DPoolSystemMem = 2u;

constexpr int32_t mirrorAutogenTextureCreationHResult(
    uint32_t usage, uint32_t pool, uint32_t levels,
    bool volumeTexture) {
  if ((usage & kD3DUsageAutogenMipmap) == 0u) return kD3D_OK;
  if (volumeTexture || pool == kD3DPoolSystemMem) {
    return kD3DERR_INVALIDCALL;
  }
  if (levels != 0u && levels != 1u) return kD3DERR_INVALIDCALL;
  return kD3D_OK;
}

void testAutogenTextureCreationPolicy() {
  checkEq(mirrorAutogenTextureCreationHResult(
              kD3DUsageAutogenMipmap, kD3DPoolDefault, 0u, false),
          kD3D_OK, "AUTOGEN Levels=0 default texture is valid");
  checkEq(mirrorAutogenTextureCreationHResult(
              kD3DUsageAutogenMipmap, kD3DPoolManaged, 1u, false),
          kD3D_OK, "AUTOGEN Levels=1 managed texture is valid");
  checkEq(mirrorAutogenTextureCreationHResult(
              kD3DUsageAutogenMipmap, kD3DPoolManaged, 2u, false),
          kD3DERR_INVALIDCALL,
          "AUTOGEN explicit multi-level texture is invalid");
  checkEq(mirrorAutogenTextureCreationHResult(
              kD3DUsageAutogenMipmap, kD3DPoolSystemMem, 0u, false),
          kD3DERR_INVALIDCALL, "AUTOGEN SYSTEMMEM texture is invalid");
  checkEq(mirrorAutogenTextureCreationHResult(
              kD3DUsageAutogenMipmap, kD3DPoolDefault, 0u, true),
          kD3DERR_INVALIDCALL, "AUTOGEN volume texture is invalid");
  checkEq(mirrorAutogenTextureCreationHResult(
              0u, kD3DPoolSystemMem, 4u, true),
          kD3D_OK, "non-AUTOGEN creation is outside this validator");
}

}  // namespace

int main() {
  try {
    testPresentParamsValidation();
    testPresentParamsExtendedLane();
    testPresentParamsValidIntervals();
    testPresentParamsMultiSample();
    testResetExModeValidation();
    testBackBufferCountNormalization();
    testQueryDataSizePerType();
    testTextureMipLevelCountPolicy();
    testAutogenTextureCreationPolicy();
  } catch (const TestFailure& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  } catch (const std::exception& error) {
    std::cerr << "unexpected exception: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
