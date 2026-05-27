// gap_d3d9 §B.10#10 / §B.2 — FFP triadic texture-stage args.
//
// Covers the third argument (`D3DTSS_COLORARG0` / `D3DTSS_ALPHAARG0`)
// consumed by the triadic texture ops `D3DTOP_MULTIPLYADD` (25) and
// `D3DTOP_LERP` (26):
//
//   - Key sensitivity: perturbing colorArg0 / alphaArg0 in a DeviceState
//     flips both `operator==` and the cached hash (so the PSO + shader
//     source caches do not silently merge two variants that read a
//     different third argument).
//   - Source emission: makeFfpPixelSource() emits a `colorArg0_<stage>`
//     selector and feeds it to the triadic op helper, and the helper
//     itself emits the MULTIPLYADD (`arg1 * arg2 + arg0`) and LERP
//     (`mix(arg2, arg1, arg0.<channel>)`) bodies that read arg0 rather
//     than the previous-stage `current` fallback.
//
// Pure source-level / value assertions; no Metal device is created.
// Mirrors tile_ffp_msl_spec.cpp's grep-for-substring style.

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

// Build a ShaderSourceContext for a single-texture FFP pixel program from a
// pixel key. A bound texture on stage 0 is needed so the generator samples
// `texColor0` and runs the texop dispatch.
dxmt9::drawshader::ShaderSourceContext makeContext(const FfpPixelKey& key) {
  DrawDesc desc{};
  desc.pixelShader.kind = ShaderRef::Kind::FixedFunctionPixel;
  desc.pixelShader.pixelKey = key;
  auto layout = makeDrawShaderLayoutContext(desc);
  auto hot = makeFlatDrawStateRecord(desc);
  auto ctx = dxmt9::drawshader::makeShaderSourceContext(layout, hot);
  ctx.textures[0] = true;
  return ctx;
}

dxmt9::drawshader::ShaderSourceContext makeUnboundContext(const FfpPixelKey& key) {
  DrawDesc desc{};
  desc.pixelShader.kind = ShaderRef::Kind::FixedFunctionPixel;
  desc.pixelShader.pixelKey = key;
  auto layout = makeDrawShaderLayoutContext(desc);
  auto hot = makeFlatDrawStateRecord(desc);
  return dxmt9::drawshader::makeShaderSourceContext(layout, hot);
}

dxmt9::drawshader::ShaderSourceContext makeSingleStageContext(
    const FfpPixelKey& key, size_t stage, bool argbufHybrid) {
  DrawDesc desc{};
  desc.pixelShader.kind = ShaderRef::Kind::FixedFunctionPixel;
  desc.pixelShader.pixelKey = key;
  auto layout = makeDrawShaderLayoutContext(desc);
  auto hot = makeFlatDrawStateRecord(desc);
  auto ctx = dxmt9::drawshader::makeShaderSourceContext(layout, hot);
  ctx.textures[stage] = true;
  ctx.argbufHybridMode = argbufHybrid;
  return ctx;
}

// A DeviceState whose stage-0 color/alpha ops are the triadic MULTIPLYADD,
// with arg1/arg2/arg0 all set to distinct selectors.
DeviceState makeTriadicState() {
  DeviceState state;
  state.reset();
  state.textureStageStates[0][TSS_COLOR_OP] =
      static_cast<u32>(TextureOp::MultiplyAdd);
  state.textureStageStates[0][TSS_COLOR_ARG1] = 2u;   // D3DTA_TEXTURE
  state.textureStageStates[0][TSS_COLOR_ARG2] = 0u;   // D3DTA_DIFFUSE
  state.textureStageStates[0][TSS_COLOR_ARG0] = 1u;   // D3DTA_CURRENT
  state.textureStageStates[0][TSS_ALPHA_OP] =
      static_cast<u32>(TextureOp::MultiplyAdd);
  state.textureStageStates[0][TSS_ALPHA_ARG1] = 2u;
  state.textureStageStates[0][TSS_ALPHA_ARG2] = 0u;
  state.textureStageStates[0][TSS_ALPHA_ARG0] = 1u;
  return state;
}

// Case 1: colorArg0 / alphaArg0 are captured into the key and participate in
// both operator== and the cached hash. A single-bit perturbation of either
// must flip the key.
void testArg0KeysAreSensitive() {
  const auto base = makeTriadicState();
  const auto baseKey = makeFfpPixelKey(base);
  check(baseKey.hash != 0, "triadic base key hash is nonzero");
  check(baseKey.stages[0].colorArg0 == 1u,
        "colorArg0 captured into the pixel key stage");
  check(baseKey.stages[0].alphaArg0 == 1u,
        "alphaArg0 captured into the pixel key stage");

  auto colorArg0Changed = base;
  colorArg0Changed.textureStageStates[0][TSS_COLOR_ARG0] = 3u;  // D3DTA_TFACTOR
  const auto colorKey = makeFfpPixelKey(colorArg0Changed);
  check(!(colorKey == baseKey),
        "pixel key insensitive to TSS_COLOR_ARG0 change");
  check(colorKey.hash != baseKey.hash,
        "pixel key hash insensitive to TSS_COLOR_ARG0 change");

  auto alphaArg0Changed = base;
  alphaArg0Changed.textureStageStates[0][TSS_ALPHA_ARG0] = 3u;
  const auto alphaKey = makeFfpPixelKey(alphaArg0Changed);
  check(!(alphaKey == baseKey),
        "pixel key insensitive to TSS_ALPHA_ARG0 change");
  check(alphaKey.hash != baseKey.hash,
        "pixel key hash insensitive to TSS_ALPHA_ARG0 change");
}

// Case 2: the triadic op helper emits MULTIPLYADD and LERP bodies that read
// an explicit arg0, and the per-stage code emits colorArg0_/alphaArg0_
// selectors threaded into the helper call.
void testTriadicSourceEmission() {
  const auto key = makeFfpPixelKey(makeTriadicState());
  const auto src = dxmt9::ffp::makeFfpPixelSource(key, makeContext(key));

  // The per-stage prelude resolves the third argument via the shared
  // arg selector, just like arg1/arg2.
  checkContains(src, "float4 colorArg0_0 = dxmt9_select_texture_arg(",
                "stage-0 emits a colorArg0 selector");
  checkContains(src, "float4 alphaArg0_0 = dxmt9_select_texture_arg(",
                "stage-0 emits an alphaArg0 selector");
  // The triadic op helper accepts an arg0 parameter.
  checkContains(src, "float4 arg0",
                "FFP texop helper takes an explicit arg0 parameter");
  // MULTIPLYADD (op 25): saturate(arg1 * arg2 + arg0) per wined3d.
  checkContains(src, "case 25u: return saturate(arg1 * arg2 + arg0);",
                "MULTIPLYADD body reads arg0 (arg1*arg2 + arg0)");
  // LERP (op 26): mix(arg2, arg1, arg0.<channel>) per wined3d
  // lerp(arg2, arg1, arg0).
  checkContains(src, "case 26u: return mix(arg2, arg1, arg0",
                "LERP body reads arg0 as the blend factor");
}

void testUnboundEnabledStageStillEmitsCombiner() {
  DeviceState state;
  state.reset();
  state.textureStageStates[0][TSS_COLOR_OP] =
      static_cast<u32>(TextureOp::SelectArg1);
  state.textureStageStates[0][TSS_COLOR_ARG1] = 3u;  // D3DTA_TFACTOR
  state.textureStageStates[0][TSS_ALPHA_OP] =
      static_cast<u32>(TextureOp::SelectArg1);
  state.textureStageStates[0][TSS_ALPHA_ARG1] = 3u;

  const auto key = makeFfpPixelKey(state);
  const auto src = dxmt9::ffp::makeFfpPixelSource(key, makeUnboundContext(key));

  checkContains(src, "float4 texColor0 = float4(1.0f);",
                "unbound enabled FFP stage emits the default texture color");
  checkContains(src, "float4 colorArg1_0 = dxmt9_select_texture_arg(3u",
                "unbound enabled FFP stage still resolves COLORARG1");
  checkContains(src, "float4 alphaArg1_0 = dxmt9_select_texture_arg(3u",
                "unbound enabled FFP stage still resolves ALPHAARG1");
  checkContains(src, "color = current;",
                "unbound enabled FFP stage writes the combiner result");
  checkContains(src, "return color;",
                "unbound enabled FFP stage returns the combiner result");
  checkNotContains(src, "[[texture(0)]]",
                   "unbound enabled FFP stage must not declare texture params");
  checkNotContains(src, "tex0.sample",
                   "unbound enabled FFP stage must not sample texture 0");
}

void testNonzeroTextureStageEmitsMatchingTextureParameter() {
  DeviceState state;
  state.reset();
  state.textureStageStates[0][TSS_COLOR_OP] =
      static_cast<u32>(TextureOp::Disable);
  state.textureStageStates[0][TSS_ALPHA_OP] =
      static_cast<u32>(TextureOp::Disable);
  state.textureStageStates[1][TSS_COLOR_OP] =
      static_cast<u32>(TextureOp::SelectArg1);
  state.textureStageStates[1][TSS_COLOR_ARG1] = 2u;  // D3DTA_TEXTURE
  state.textureStageStates[1][TSS_ALPHA_OP] =
      static_cast<u32>(TextureOp::SelectArg1);
  state.textureStageStates[1][TSS_ALPHA_ARG1] = 2u;

  const auto key = makeFfpPixelKey(state);
  const auto src = dxmt9::ffp::makeFfpPixelSource(
      key, makeSingleStageContext(key, 1u, true));

  checkContains(src, "[[buffer(30)]]",
                "Stage 2 FFP source keeps constants on the argument buffer");
  checkContains(src, "texture2d<float> tex1 [[texture(1)]], sampler samp1 [[sampler(1)]]",
                "nonzero FFP texture stage declares matching texture/sampler params");
  checkContains(src, "float4 texColor1 = tex1.sample(samp1",
                "nonzero FFP texture stage samples the matching texture slot");
  checkContains(src, "float4 colorArg1_1 = dxmt9_select_texture_arg(2u",
                "nonzero FFP texture stage resolves D3DTA_TEXTURE");
  checkNotContains(src, "[[texture(0)]]",
                   "nonzero-only FFP texture stage must not declare texture 0");
  checkNotContains(src, "tex0.sample",
                   "nonzero-only FFP texture stage must not sample texture 0");
}

}  // namespace

int main() {
  try {
    testArg0KeysAreSensitive();
    testTriadicSourceEmission();
    testUnboundEnabledStageStillEmitsCombiner();
    testNonzeroTextureStageEmitsMatchingTextureParameter();
  } catch (const TestFailure& failure) {
    std::cerr << "ffp_triadic_msl_spec failed: " << failure.what() << '\n';
    return 1;
  } catch (const std::exception& ex) {
    std::cerr << "ffp_triadic_msl_spec unexpected exception: " << ex.what()
              << '\n';
    return 1;
  }

  std::cout << "ffp_triadic_msl_spec passed\n";
  return 0;
}
