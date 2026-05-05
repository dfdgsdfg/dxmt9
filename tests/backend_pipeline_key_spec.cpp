#include <array>
#include <cstddef>
#include <exception>
#include <iostream>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "dxmt9/core.hpp"
#include "../src/dxmt9/dxmt9_ffp_shaders.hpp"
#include "../src/dxmt9/dxmt9_format_convert.hpp"
#include "../src/dxmt9/dxmt9_pipeline_cache.hpp"

using namespace dxmt9::core;

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

template <typename A, typename B>
void checkEq(const A& left, const B& right, std::string_view message) {
  if (!(left == right)) {
    std::ostringstream out;
    out << message << " (left != right)";
    fail(out.str());
  }
}

auto makeBlendKeys(const DrawDesc& desc) {
  return dxmt9::pipeline::detail::makeBlendAttachmentKeys(desc);
}

auto makeFlatBlendKeys(const DrawDesc& desc) {
  const auto hot = makeFlatDrawStateRecord(desc);
  return dxmt9::pipeline::detail::makeBlendAttachmentKeys(
      FlatDrawStateView{.hot = &hot});
}

auto makeVariantKey(const DrawDesc& desc) {
  std::array<u32, kMaxRenderTargets> colorFormats{};
  colorFormats[0] = WMTPixelFormatBGRA8Unorm;
  auto blend = makeBlendKeys(desc);
  for (std::size_t i = 0; i < kMaxRenderTargets; ++i) {
    blend[i].pixelFormat = colorFormats[i];
  }
  return dxmt9::pipeline::makeShaderVariantKey(desc,
                                               std::span<const u32>(colorFormats),
                                               std::span<const dxmt9::pipeline::BlendAttachmentKey>(blend),
                                               0u,
                                               0u);
}

auto makeFlatVariantKey(const DrawDesc& desc) {
  const auto hot = makeFlatDrawStateRecord(desc);
  const auto shaderLayout = makeDrawShaderLayoutContext(desc);
  std::array<u32, kMaxRenderTargets> colorFormats{};
  colorFormats[0] = WMTPixelFormatBGRA8Unorm;
  auto blend = makeFlatBlendKeys(desc);
  for (std::size_t i = 0; i < kMaxRenderTargets; ++i) {
    blend[i].pixelFormat = colorFormats[i];
  }
  return dxmt9::pipeline::makeShaderVariantKey(
      FlatDrawStateView{.hot = &hot, .shaderLayout = &shaderLayout},
      std::span<const u32>(colorFormats),
      std::span<const dxmt9::pipeline::BlendAttachmentKey>(blend),
      0u,
      0u);
}

void testAlphaBlendEnableAndDisable() {
  DrawDesc desc{};

  auto keys = makeBlendKeys(desc);
  check(!keys[0].blendingEnabled, "absent alpha blend state disables blending");

  desc.rs.values[RS_ALPHABLEND_ENABLE] = 0u;
  keys = makeBlendKeys(desc);
  check(!keys[0].blendingEnabled, "zero alpha blend state disables blending");

  desc.rs.values[RS_ALPHABLEND_ENABLE] = 1u;
  keys = makeBlendKeys(desc);
  check(keys[0].blendingEnabled, "non-zero alpha blend state enables blending");

  keys = dxmt9::pipeline::detail::makeBlendAttachmentKeys(desc, true);
  check(!keys[0].blendingEnabled, "force-visible mode disables blending");
}

void testBlendOperationFallbacks() {
  DrawDesc desc{};
  desc.rs.values[RS_BLEND_OP] = static_cast<u32>(BlendOp::Subtract);

  auto keys = makeBlendKeys(desc);
  checkEq(keys[0].rgbBlendOperation,
          static_cast<u32>(BlendOp::Subtract),
          "RGB blend op reflects render state");
  checkEq(keys[0].alphaBlendOperation,
          static_cast<u32>(BlendOp::Subtract),
          "missing alpha blend op falls back to RGB blend op");

  desc.rs.values[RS_BLEND_OP_ALPHA] = static_cast<u32>(BlendOp::Max);
  keys = makeBlendKeys(desc);
  checkEq(keys[0].rgbBlendOperation,
          static_cast<u32>(BlendOp::Subtract),
          "RGB blend op remains independent from alpha override");
  checkEq(keys[0].alphaBlendOperation,
          static_cast<u32>(BlendOp::Max),
          "alpha blend op override is reflected");
}

void testBlendFactorFallbacks() {
  DrawDesc desc{};
  desc.rs.values[RS_SRC_BLEND] = static_cast<u32>(BlendFactor::SrcAlpha);
  desc.rs.values[RS_DEST_BLEND] = static_cast<u32>(BlendFactor::InvDestAlpha);

  auto keys = makeBlendKeys(desc);
  checkEq(keys[0].sourceRGBBlendFactor,
          static_cast<u32>(BlendFactor::SrcAlpha),
          "source RGB blend factor reflects render state");
  checkEq(keys[0].destinationRGBBlendFactor,
          static_cast<u32>(BlendFactor::InvDestAlpha),
          "destination RGB blend factor reflects render state");
  checkEq(keys[0].sourceAlphaBlendFactor,
          static_cast<u32>(BlendFactor::SrcAlpha),
          "missing source alpha blend factor falls back to source RGB factor");
  checkEq(keys[0].destinationAlphaBlendFactor,
          static_cast<u32>(BlendFactor::InvDestAlpha),
          "missing destination alpha blend factor falls back to destination RGB factor");

  desc.rs.values[RS_SRC_BLEND_ALPHA] = static_cast<u32>(BlendFactor::One);
  desc.rs.values[RS_DEST_BLEND_ALPHA] = static_cast<u32>(BlendFactor::Zero);
  keys = makeBlendKeys(desc);
  checkEq(keys[0].sourceAlphaBlendFactor,
          static_cast<u32>(BlendFactor::One),
          "source alpha blend factor override is reflected");
  checkEq(keys[0].destinationAlphaBlendFactor,
          static_cast<u32>(BlendFactor::Zero),
          "destination alpha blend factor override is reflected");
}

void testMrtColorWriteMaskDefaultAndOverride() {
  DrawDesc desc{};
  auto keys = makeBlendKeys(desc);

  for (std::size_t i = 0; i < kMaxRenderTargets; ++i) {
    checkEq(keys[i].colorWriteMask, 0xfu, "missing color write mask defaults to all channels");
    checkEq(keys[i].pixelFormat, 0u, "render-state helper leaves pixel format unresolved");
  }

  desc.rs.values[RS_COLOR_WRITE_ENABLE] = 0x5u;
  keys = makeBlendKeys(desc);
  for (std::size_t i = 0; i < kMaxRenderTargets; ++i) {
    checkEq(keys[i].colorWriteMask, 0x5u, "color write mask override applies to every MRT key");
  }

  keys = dxmt9::pipeline::detail::makeBlendAttachmentKeys(desc, true);
  for (std::size_t i = 0; i < kMaxRenderTargets; ++i) {
    checkEq(keys[i].colorWriteMask, 0xfu, "force-visible mode restores all-channel writes");
  }
}

void testShaderVariantKeyReflectsSamplerTextureAndFiltering() {
  DrawDesc desc{};
  desc.vertexShader.hash = 0x101u;
  desc.pixelShader.hash = 0x202u;

  const auto untextured = makeVariantKey(desc);
  check(!untextured.textured, "missing texture binding leaves variant untextured");
  check(!untextured.linear, "missing sampler filter state leaves variant nearest");

  desc.textures[0].handle = Handle{0x44u};
  desc.samplers[0].states[SAMP_MIN_FILTER] = 1u;
  desc.samplers[0].states[SAMP_MAG_FILTER] = 1u;
  const auto nearest = makeVariantKey(desc);
  check(nearest.textured, "texture binding marks variant textured");
  check(!nearest.linear, "nearest min/mag filters keep variant nearest");
  check(dxmt9::pipeline::ShaderVariantKeyHash{}(nearest) !=
            dxmt9::pipeline::ShaderVariantKeyHash{}(untextured),
        "textured sampler key changes the PSO hash");

  desc.samplers[0].states[SAMP_MIN_FILTER] = 2u;
  const auto linearMin = makeVariantKey(desc);
  check(linearMin.linear, "linear min filter marks variant linear");
  check(dxmt9::pipeline::ShaderVariantKeyHash{}(linearMin) !=
            dxmt9::pipeline::ShaderVariantKeyHash{}(nearest),
        "linear min filter changes the PSO hash");

  desc.samplers[0].states[SAMP_MIN_FILTER] = 1u;
  desc.samplers[0].states[SAMP_MAG_FILTER] = 2u;
  const auto linearMag = makeVariantKey(desc);
  check(linearMag.linear, "linear mag filter marks variant linear");
}

void testFvfLayoutHashIsDeterministicAndResponsive() {
  DrawDesc desc{};
  desc.vertexDecl.fvf =
      dxmt9::ffp::kFvfXyzrhw | dxmt9::ffp::kFvfDiffuse | (1u << dxmt9::ffp::kFvfTexCountShift);

  const auto layout = dxmt9::ffp::decodeFixedFunctionVertexLayout(desc);
  const auto layoutAgain = dxmt9::ffp::decodeFixedFunctionVertexLayout(desc);
  check(layout.has_value(), "FVF layout decodes");
  check(layoutAgain.has_value(), "repeat FVF layout decodes");
  check(layout->valid, "FVF layout is valid");
  check(layout->preTransformed, "XYZRHW FVF is pre-transformed");
  checkEq(layout->positionComponents, 4u, "XYZRHW FVF uses four position components");
  check(layout->hasDiffuse, "diffuse FVF bit creates color input");
  check(layout->hasTexcoord[0], "texcount FVF creates texcoord input");
  checkEq(layout->stride, 28u, "FVF stride accounts for XYZRHW diffuse texcoord");
  checkEq(layout->positionOffset, 0u, "FVF position offset");
  checkEq(layout->diffuseOffset, 16u, "FVF diffuse offset");
  checkEq(layout->texcoordOffset[0], 20u, "FVF texcoord offset");
  checkEq(layout->hash, layoutAgain->hash, "FVF layout hash is deterministic");

  DrawDesc withSpecular = desc;
  withSpecular.vertexDecl.fvf |= dxmt9::ffp::kFvfSpecular;
  const auto specularLayout = dxmt9::ffp::decodeFixedFunctionVertexLayout(withSpecular);
  check(specularLayout.has_value(), "specular FVF layout decodes");
  checkEq(specularLayout->stride, 32u, "specular FVF increases stride");
  checkEq(specularLayout->texcoordOffset[0], 24u, "specular FVF shifts texcoord offset");
  check(specularLayout->hash != layout->hash, "FVF layout hash changes when vertex layout changes");
}

void testShaderVariantKeyHashRespondsToLayoutAndBlendState() {
  DrawDesc desc{};
  desc.vertexShader.hash = 0xaaaau;
  desc.pixelShader.hash = 0x5555u;
  desc.vertexDecl.fvf =
      dxmt9::ffp::kFvfXyz | dxmt9::ffp::kFvfDiffuse | (1u << dxmt9::ffp::kFvfTexCountShift);

  const auto base = makeVariantKey(desc);
  const auto baseHash = dxmt9::pipeline::ShaderVariantKeyHash{}(base);

  DrawDesc layoutChanged = desc;
  layoutChanged.vertexDecl.fvf |= dxmt9::ffp::kFvfSpecular;
  const auto layoutKey = makeVariantKey(layoutChanged);
  check(layoutKey.hash != base.hash, "layout changes alter the shader variant layout hash");
  check(dxmt9::pipeline::ShaderVariantKeyHash{}(layoutKey) != baseHash,
        "layout changes alter the PSO key hash");

  DrawDesc blendChanged = desc;
  blendChanged.rs.values[RS_ALPHABLEND_ENABLE] = 1u;
  blendChanged.rs.values[RS_COLOR_WRITE_ENABLE] = 0x7u;
  const auto blendKey = makeVariantKey(blendChanged);
  check(!(blendKey == base), "blend state changes alter the PSO key");
  check(dxmt9::pipeline::ShaderVariantKeyHash{}(blendKey) != baseHash,
        "blend state changes alter the PSO key hash");
}

void testFlatPipelineHelpersMatchDrawDescCompatibilityWrappers() {
  DrawDesc desc{};
  desc.vertexShader.hash = 0xabcdefu;
  desc.pixelShader.hash = 0x102030u;
  desc.vertexDecl.fvf =
      dxmt9::ffp::kFvfXyz | dxmt9::ffp::kFvfDiffuse | (1u << dxmt9::ffp::kFvfTexCountShift);
  desc.textures[0].handle = Handle{0x1200u};
  desc.samplers[0].states[SAMP_MIN_FILTER] = 2u;
  desc.samplers[0].states[SAMP_MAG_FILTER] = 1u;
  desc.rs.values[RS_ALPHABLEND_ENABLE] = 1u;
  desc.rs.values[RS_SRC_BLEND] = static_cast<u32>(BlendFactor::SrcAlpha);
  desc.rs.values[RS_DEST_BLEND] = static_cast<u32>(BlendFactor::InvSrcAlpha);
  desc.rs.values[RS_COLOR_WRITE_ENABLE] = 0x7u;
  desc.rs.values[RS_ALPHA_TEST_ENABLE] = 1u;
  desc.clipPlaneMask = 0x3u;
  desc.rts.color[0].sampleCount = 4u;

  checkEq(makeFlatBlendKeys(desc), makeBlendKeys(desc),
          "flat blend helper matches DrawDesc compatibility wrapper");
  checkEq(makeFlatVariantKey(desc), makeVariantKey(desc),
          "flat shader variant helper matches DrawDesc compatibility wrapper");
}

void testSrgbCompatiblePixelFormatConversion() {
  BackendLimits limits{};

  checkEq(dxmt9::convert::toPixelFormat(Format::A8R8G8B8, limits, false),
          WMTPixelFormatBGRA8Unorm,
          "A8R8G8B8 linear pixel format");
  checkEq(dxmt9::convert::toPixelFormat(Format::A8R8G8B8, limits, true),
          WMTPixelFormatBGRA8Unorm_sRGB,
          "A8R8G8B8 sRGB pixel format");
  checkEq(dxmt9::convert::toPixelFormat(Format::A8B8G8R8, limits, true),
          WMTPixelFormatRGBA8Unorm_sRGB,
          "A8B8G8R8 sRGB pixel format");
  checkEq(dxmt9::convert::toPixelFormat(Format::DXT1, limits, true),
          WMTPixelFormatBC1_RGBA_sRGB,
          "DXT1 sRGB pixel format");
  checkEq(dxmt9::convert::toPixelFormat(Format::DXT5, limits, true),
          WMTPixelFormatBC3_RGBA_sRGB,
          "DXT5 sRGB pixel format");
  checkEq(dxmt9::convert::toPixelFormat(Format::R5G6B5, limits, true),
          WMTPixelFormatB5G6R5Unorm,
          "non-sRGB-compatible color format stays unchanged");
  checkEq(dxmt9::convert::toPixelFormat(Format::D24S8, limits, true),
          WMTPixelFormatDepth24Unorm_Stencil8,
          "depth-stencil format stays unchanged for sRGB request");
}

}  // namespace

int main() {
  try {
    testAlphaBlendEnableAndDisable();
    testBlendOperationFallbacks();
    testBlendFactorFallbacks();
    testMrtColorWriteMaskDefaultAndOverride();
    testShaderVariantKeyReflectsSamplerTextureAndFiltering();
    testFvfLayoutHashIsDeterministicAndResponsive();
    testShaderVariantKeyHashRespondsToLayoutAndBlendState();
    testFlatPipelineHelpersMatchDrawDescCompatibilityWrappers();
    testSrgbCompatiblePixelFormatConversion();
  } catch (const TestFailure& failure) {
    std::cerr << "backend_pipeline_key_spec failed: " << failure.what() << '\n';
    return 1;
  } catch (const std::exception& ex) {
    std::cerr << "backend_pipeline_key_spec unexpected exception: " << ex.what() << '\n';
    return 1;
  }

  std::cout << "backend_pipeline_key_spec passed\n";
  return 0;
}
