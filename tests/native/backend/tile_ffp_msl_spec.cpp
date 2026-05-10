// R-BACK-13.* — focused tile-FFP MSL emitter spec.
//
// Pure source-level assertions on the output of makeFfpTilePixelSource().
// Verifies:
//   - 8-bpc attachments emit `imageblock<...>` carrying `half4` slot type
//     while wider attachments emit `float4` (R-BACK-13.7 fp boundary).
//   - The kernel signature is `[[kernel]] void ffp_tile(...)` (never a
//     fragment) and never calls `discard_fragment()` (R-BACK-13.5).
//   - FFP arithmetic carries `float`, never `half` (R-BACK-13.7).
//   - Fog blend / alpha-test code is emitted only when keyed on by the
//     FFPKeyPS.
//
// No Metal device is created; the generator returns std::string and the
// tests grep for stable substrings.

#include <cstdint>
#include <exception>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "dxmt9/core.hpp"
#include "../../../src/dxmt9/dxmt9_draw_shader.hpp"
#include "../../../src/dxmt9/dxmt9_ffp_shaders.hpp"
#include "../../../src/winemetal/Metal.hpp"

using namespace dxmt9::core;
using namespace dxmt9::core::fixture;

namespace {

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

void checkContains(const std::string& haystack, std::string_view needle,
                    std::string_view message) {
  if (haystack.find(needle) == std::string::npos) {
    std::ostringstream out;
    out << message << " (expected substring '" << needle << "' not found)";
    fail(out.str());
  }
}

void checkNotContains(const std::string& haystack, std::string_view needle,
                       std::string_view message) {
  if (haystack.find(needle) != std::string::npos) {
    std::ostringstream out;
    out << message << " (unexpected substring '" << needle << "' found)";
    fail(out.str());
  }
}

dxmt9::drawshader::ShaderSourceContext makeContext() {
  DrawDesc desc{};
  desc.pixelShader.kind = ShaderRef::Kind::FixedFunctionPixel;
  desc.pixelShader.pixelKey = FfpPixelKey{};
  auto layout = makeDrawShaderLayoutContext(desc);
  auto hot = makeFlatDrawStateRecord(desc);
  return dxmt9::drawshader::makeShaderSourceContext(layout, hot);
}

void testTileKernelSignature() {
  FfpPixelKey key{};
  auto src = dxmt9::ffp::makeFfpTilePixelSource(
      key, makeContext(), static_cast<std::uint32_t>(WMTPixelFormatBGRA8Unorm));
  // R-BACK-13.5: kernel signature, never a fragment.
  checkContains(src, "[[kernel]] void ffp_tile",
                "tile MSL emits a kernel function");
  checkNotContains(src, "fragment float4",
                    "tile MSL never emits a fragment function");
  // R-BACK-13.5: discard_fragment() is forbidden in the tile path.
  checkNotContains(src, "discard_fragment",
                    "tile MSL never calls discard_fragment");
  // imageblock declaration is the entry point for programmable blending.
  checkContains(src, "imageblock<TileColorData",
                "tile MSL uses imageblock<> for programmable blending");
}

void testImageblockHalf4OnEightBitFormats() {
  FfpPixelKey key{};
  // BGRA8Unorm — 8-bpc; R-BACK-13.7 permits half4 imageblock here.
  auto src = dxmt9::ffp::makeFfpTilePixelSource(
      key, makeContext(), static_cast<std::uint32_t>(WMTPixelFormatBGRA8Unorm));
  checkContains(src, "half4 color [[color(0)]]",
                "8-bpc BGRA8Unorm uses half4 imageblock slot");
  // Verify the attachment-classification helper agrees.
  check(dxmt9::ffp::tileFfpAttachmentAcceptsHalf(
            static_cast<std::uint32_t>(WMTPixelFormatBGRA8Unorm)),
        "BGRA8Unorm classified as half-friendly");
  check(dxmt9::ffp::tileFfpAttachmentAcceptsHalf(
            static_cast<std::uint32_t>(WMTPixelFormatRGBA8Unorm)),
        "RGBA8Unorm classified as half-friendly");
}

void testImageblockFloat4OnWideFormats() {
  FfpPixelKey key{};
  // RGBA16Float — wider than 8-bpc; R-BACK-13.7 requires float4.
  auto src = dxmt9::ffp::makeFfpTilePixelSource(
      key, makeContext(), static_cast<std::uint32_t>(WMTPixelFormatRGBA16Float));
  checkContains(src, "float4 color [[color(0)]]",
                "RGBA16Float uses float4 imageblock slot");
  checkNotContains(src, "half4 color",
                    "wide attachment never falls back to half4 slot");
  check(!dxmt9::ffp::tileFfpAttachmentAcceptsHalf(
            static_cast<std::uint32_t>(WMTPixelFormatRGBA16Float)),
        "RGBA16Float classified as float-only");
  check(!dxmt9::ffp::tileFfpAttachmentAcceptsHalf(
            static_cast<std::uint32_t>(WMTPixelFormatRGBA32Float)),
        "RGBA32Float classified as float-only");
}

void testFogArithmeticTypedFloat() {
  FfpPixelKey key{};
  key.fogMode = FogMode::Linear;
  auto src = dxmt9::ffp::makeFfpTilePixelSource(
      key, makeContext(), static_cast<std::uint32_t>(WMTPixelFormatBGRA8Unorm));
  // R-BACK-13.7: fog arithmetic must declare `float fog`, not `half`.
  checkContains(src, "float fog",
                "fog blend variable is typed float");
  checkContains(src, "float4 fogColor",
                "fog color literal is typed float4");
  checkNotContains(src, "half fog",
                    "fog variable is never typed half");
  // Fog blend must promote the imageblock-read color to float regardless
  // of the slot type, then demote on writeback.
  checkContains(src, "float4 color = float4(slot->color)",
                "imageblock value is promoted to float4 for FFP arithmetic");
}

void testAlphaTestEmitsBranchOnly() {
  FfpPixelKey key{};
  // Without alphaTestEnable the emitter must skip the branch entirely.
  auto plain = dxmt9::ffp::makeFfpTilePixelSource(
      key, makeContext(), static_cast<std::uint32_t>(WMTPixelFormatBGRA8Unorm));
  checkNotContains(plain, "ffpPs.alphaRef",
                    "no alpha-test branch when alphaTestEnable=false");

  // With alphaTestEnable, the branch is emitted and uses ffpPs.alphaRef.
  key.alphaTestEnable = true;
  auto enabled = dxmt9::ffp::makeFfpTilePixelSource(
      key, makeContext(), static_cast<std::uint32_t>(WMTPixelFormatBGRA8Unorm));
  checkContains(enabled, "ffpPs.alphaRef",
                "alpha-test branch reads ffpPs.alphaRef");
  // R-BACK-13.5: alpha-test rejection is `return` (skip imageblock
  // write) — never a discard_fragment call.
  checkNotContains(enabled, "discard_fragment",
                    "alpha-test rejection uses early return, not discard");
}

void testFogOmittedWhenFogModeNone() {
  FfpPixelKey key{};
  key.fogMode = FogMode::None;
  auto src = dxmt9::ffp::makeFfpTilePixelSource(
      key, makeContext(), static_cast<std::uint32_t>(WMTPixelFormatBGRA8Unorm));
  checkNotContains(src, "fogColor",
                    "no fog code emitted when fog mode is None");
}

void testKernelHasFfpPsConstsBuffer() {
  FfpPixelKey key{};
  auto src = dxmt9::ffp::makeFfpTilePixelSource(
      key, makeContext(), static_cast<std::uint32_t>(WMTPixelFormatBGRA8Unorm));
  // FfpPsConsts at buffer(3) mirrors the portable PSO binding so the
  // encoder can drive both pipelines from the same per-frequency UBO.
  checkContains(src, "constant FfpPsConsts& ffpPs [[buffer(3)]]",
                "tile kernel binds FfpPsConsts at buffer(3) like portable");
  checkContains(src, "struct FfpPsConsts",
                "FfpPsConsts struct is declared inline");
}

}  // namespace

int main() {
  try {
    testTileKernelSignature();
    testImageblockHalf4OnEightBitFormats();
    testImageblockFloat4OnWideFormats();
    testFogArithmeticTypedFloat();
    testAlphaTestEmitsBranchOnly();
    testFogOmittedWhenFogModeNone();
    testKernelHasFfpPsConstsBuffer();
  } catch (const TestFailure& failure) {
    std::cerr << "tile_ffp_msl_spec failed: " << failure.what() << '\n';
    return 1;
  } catch (const std::exception& ex) {
    std::cerr << "tile_ffp_msl_spec unexpected exception: " << ex.what() << '\n';
    return 1;
  }

  std::cout << "tile_ffp_msl_spec passed\n";
  return 0;
}
