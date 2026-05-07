// R-BACK-12.16/12.18 layout asserts for the per-frequency draw-uniform
// host structs. Pins each struct's sizeof + every field offsetof to a
// constant so any drift between the C++ struct and the MSL prelude is
// caught at compile time. Also string-checks that makeShaderPrelude()
// emits a matching MSL declaration for each struct.

#include "dxmt9/core.hpp"
#include "../../../src/dxmt9/dxmt9_draw_state.hpp"
#include "../../../src/dxmt9/dxmt9_shader_sources.hpp"

#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

using dxmt9::state::DrawVolatile;
using dxmt9::state::FfpPsConsts;
using dxmt9::state::FfpVsConsts;
using dxmt9::state::PsConsts;
using dxmt9::state::VsConsts;

// --- R-BACK-12.18: static struct sizes --------------------------------------

static_assert(sizeof(VsConsts) == 4416, "VsConsts size pinned for MSL prelude parity");
static_assert(sizeof(PsConsts) == 3904, "PsConsts size pinned for MSL prelude parity");
static_assert(sizeof(FfpVsConsts) == 700, "FfpVsConsts size pinned for MSL prelude parity");
static_assert(sizeof(FfpPsConsts) == 44, "FfpPsConsts size pinned for MSL prelude parity");
static_assert(sizeof(DrawVolatile) == 16, "DrawVolatile size pinned for MSL prelude parity");

// --- R-BACK-12.16: per-field byte offsets -----------------------------------

// VsConsts: float4[256] | int4[16] | uint[16].
static_assert(offsetof(VsConsts, vsFloatConst) == 0);
static_assert(offsetof(VsConsts, vsIntConst) == 256 * 16);
static_assert(offsetof(VsConsts, vsBoolConst) == 256 * 16 + 16 * 16);

// PsConsts: float4[224] | int4[16] | uint[16].
static_assert(offsetof(PsConsts, psFloatConst) == 0);
static_assert(offsetof(PsConsts, psIntConst) == 224 * 16);
static_assert(offsetof(PsConsts, psBoolConst) == 224 * 16 + 16 * 16);

// FfpVsConsts: 16 B float4[4] | 16 B float4[8][4] | float4[6] (ClipPlane) |
// float2 x3 | uint clipPlaneMask. ClipPlane = std::array<f32, 4> with 4 B
// alignment, so float2 fields can pack tightly behind it.
static_assert(offsetof(FfpVsConsts, ffpWorldViewProj) == 0);
static_assert(offsetof(FfpVsConsts, ffpTextureTransforms) == 64);
static_assert(offsetof(FfpVsConsts, clipPlanes) == 64 + 512);
static_assert(offsetof(FfpVsConsts, halfPixelFixup) == 64 + 512 + 96);
static_assert(offsetof(FfpVsConsts, viewportOrigin) == 64 + 512 + 96 + 8);
static_assert(offsetof(FfpVsConsts, viewportSize) == 64 + 512 + 96 + 16);
static_assert(offsetof(FfpVsConsts, clipPlaneMask) == 64 + 512 + 96 + 24);

// FfpPsConsts: float4 textureFactor | 4x f32 | 3x u32.
static_assert(offsetof(FfpPsConsts, textureFactor) == 0);
static_assert(offsetof(FfpPsConsts, alphaRef) == 16);
static_assert(offsetof(FfpPsConsts, fogStart) == 20);
static_assert(offsetof(FfpPsConsts, fogEnd) == 24);
static_assert(offsetof(FfpPsConsts, fogDensity) == 28);
static_assert(offsetof(FfpPsConsts, alphaTestEnable) == 32);
static_assert(offsetof(FfpPsConsts, alphaTestFunc) == 36);
static_assert(offsetof(FfpPsConsts, fogMode) == 40);

// DrawVolatile: i32 | u32 | u32 | u32 _pad — 16 B push-constant footprint
// for setVertexBytes.
static_assert(offsetof(DrawVolatile, vertexBaseIndex) == 0);
static_assert(offsetof(DrawVolatile, vertexStreamOffset) == 4);
static_assert(offsetof(DrawVolatile, vertexStreamStride) == 8);
static_assert(offsetof(DrawVolatile, _pad) == 12);

// --- R-BACK-12.16: MSL prelude string parity --------------------------------

void requireSubstring(const std::string& haystack, std::string_view needle,
                      std::string_view context) {
  if (haystack.find(needle) == std::string::npos) {
    std::cerr << "FAIL: " << context << ": MSL prelude is missing substring \""
              << needle << "\"\n";
    std::exit(1);
  }
}

void checkMslPreludeContainsStructDecls() {
  const std::string prelude = dxmt9::shaders::makeShaderPrelude(/*withClipDistances=*/true);

  // Per-struct opening braces.
  requireSubstring(prelude, "struct VsConsts {", "VsConsts opener");
  requireSubstring(prelude, "struct PsConsts {", "PsConsts opener");
  requireSubstring(prelude, "struct FfpVsConsts {", "FfpVsConsts opener");
  requireSubstring(prelude, "struct FfpPsConsts {", "FfpPsConsts opener");
  requireSubstring(prelude, "struct DrawVolatile {", "DrawVolatile opener");

  // VsConsts fields. kMaxVertexConstants=256, kMaxIntegerConstants=16,
  // kMaxBoolConstants=16.
  requireSubstring(prelude, "float4 vsFloatConst[256]", "VsConsts.vsFloatConst");
  requireSubstring(prelude, "int4 vsIntConst[16]", "VsConsts.vsIntConst");
  requireSubstring(prelude, "uint vsBoolConst[16]", "VsConsts.vsBoolConst");

  // PsConsts fields. kMaxPixelConstants=224.
  requireSubstring(prelude, "float4 psFloatConst[224]", "PsConsts.psFloatConst");
  requireSubstring(prelude, "int4 psIntConst[16]", "PsConsts.psIntConst");
  requireSubstring(prelude, "uint psBoolConst[16]", "PsConsts.psBoolConst");

  // FfpVsConsts fields. kMaxTextureStages=8, kMaxClipPlanes=6.
  requireSubstring(prelude, "float4 ffpWorldViewProj[4]", "FfpVsConsts.ffpWorldViewProj");
  requireSubstring(prelude, "float4 ffpTextureTransforms[8][4]",
                   "FfpVsConsts.ffpTextureTransforms");
  requireSubstring(prelude, "float4 clipPlanes[6]", "FfpVsConsts.clipPlanes");
  requireSubstring(prelude, "float2 halfPixelFixup", "FfpVsConsts.halfPixelFixup");
  requireSubstring(prelude, "float2 viewportOrigin", "FfpVsConsts.viewportOrigin");
  requireSubstring(prelude, "float2 viewportSize", "FfpVsConsts.viewportSize");
  requireSubstring(prelude, "uint clipPlaneMask", "FfpVsConsts.clipPlaneMask");

  // FfpPsConsts fields.
  requireSubstring(prelude, "float4 textureFactor", "FfpPsConsts.textureFactor");
  requireSubstring(prelude, "float alphaRef", "FfpPsConsts.alphaRef");
  requireSubstring(prelude, "float fogStart", "FfpPsConsts.fogStart");
  requireSubstring(prelude, "float fogEnd", "FfpPsConsts.fogEnd");
  requireSubstring(prelude, "float fogDensity", "FfpPsConsts.fogDensity");
  requireSubstring(prelude, "uint alphaTestEnable", "FfpPsConsts.alphaTestEnable");
  requireSubstring(prelude, "uint alphaTestFunc", "FfpPsConsts.alphaTestFunc");
  requireSubstring(prelude, "uint fogMode", "FfpPsConsts.fogMode");

  // DrawVolatile fields.
  requireSubstring(prelude, "int vertexBaseIndex", "DrawVolatile.vertexBaseIndex");
  requireSubstring(prelude, "uint vertexStreamOffset", "DrawVolatile.vertexStreamOffset");
  requireSubstring(prelude, "uint vertexStreamStride", "DrawVolatile.vertexStreamStride");
  requireSubstring(prelude, "uint _pad", "DrawVolatile._pad");
}

}  // namespace

int main() {
  checkMslPreludeContainsStructDecls();
  return 0;
}
