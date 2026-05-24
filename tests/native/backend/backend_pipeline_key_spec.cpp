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
#include "../../../src/dxmt9/dxmt9_ffp_shaders.hpp"
#include "../../../src/dxmt9/dxmt9_format_convert.hpp"
#include "../../../src/dxmt9/dxmt9_pipeline_cache.hpp"
#include "../../../src/dxmt9/dxmt9_shader_sources.hpp"

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

template <typename A, typename B>
void checkEq(const A& left, const B& right, std::string_view message) {
  if (!(left == right)) {
    std::ostringstream out;
    out << message << " (left != right)";
    fail(out.str());
  }
}

struct FlatDrawFixture {
  FlatDrawStateRecord hot{};
  DrawShaderLayoutContext shaderLayout{};

  FlatDrawStateView view() const {
    return FlatDrawStateView{.hot = &hot, .shaderLayout = &shaderLayout};
  }
};

FlatDrawFixture makeFlatDrawFixture(const DrawDesc& desc) {
  return FlatDrawFixture{
      .hot = makeFlatDrawStateRecord(desc),
      .shaderLayout = makeDrawShaderLayoutContext(desc),
  };
}

auto makeBlendKeys(const FlatDrawStateRecord& hot, bool forceVisibleDraw = false) {
  return dxmt9::pipeline::detail::makeBlendAttachmentKeys(
      FlatDrawStateView{.hot = &hot}, forceVisibleDraw);
}

auto makeVariantKey(const FlatDrawFixture& fixture) {
  std::array<u32, kMaxRenderTargets> colorFormats{};
  colorFormats[0] = WMTPixelFormatBGRA8Unorm;
  auto blend = makeBlendKeys(fixture.hot);
  for (std::size_t i = 0; i < kMaxRenderTargets; ++i) {
    blend[i].pixelFormat = colorFormats[i];
  }
  return dxmt9::pipeline::makeShaderVariantKey(
      fixture.view(),
      std::span<const u32>(colorFormats),
      std::span<const dxmt9::pipeline::BlendAttachmentKey>(blend),
      0u,
      0u);
}

u32 makeVersionToken(bool vertex, u32 major, u32 minor) {
  return ((vertex ? 0xfffeu : 0xffffu) << 16) | ((major & 0xffu) << 8) | (minor & 0xffu);
}

u32 makeInstructionToken(u32 opcode, u32 operandCount) {
  return (opcode & 0xffffu) | ((operandCount & 0xfu) << 24);
}

ShaderRef makeUnsupportedOpcodePixelShader() {
  constexpr u32 kUnsupportedOpcode = 0x1234u;
  constexpr u32 kD3DSIOEnd = 0xffffu;
  const std::array<u32, 4> bytecode = {
      makeVersionToken(/*vertex=*/false, 2u, 0u),
      makeInstructionToken(kUnsupportedOpcode, 1u),
      0x800f0000u,
      kD3DSIOEnd,
  };

  ShaderRef shader{};
  shader.kind = ShaderRef::Kind::Bytecode;
  shader.hash = 0x1200340056007800ull;
  shader.bytecode.hash = shader.hash;
  const auto* base = reinterpret_cast<const u8*>(bytecode.data());
  shader.bytecode.bytes.assign(base, base + bytecode.size() * sizeof(u32));
  return shader;
}

void testAlphaBlendEnableAndDisable() {
  DrawDesc desc{};

  auto keys = makeBlendKeys(makeFlatDrawFixture(desc).hot);
  check(!keys[0].blendingEnabled, "absent alpha blend state disables blending");

  desc.rs.values[RS_ALPHABLEND_ENABLE] = 0u;
  keys = makeBlendKeys(makeFlatDrawFixture(desc).hot);
  check(!keys[0].blendingEnabled, "zero alpha blend state disables blending");

  desc.rs.values[RS_ALPHABLEND_ENABLE] = 1u;
  keys = makeBlendKeys(makeFlatDrawFixture(desc).hot);
  check(keys[0].blendingEnabled, "non-zero alpha blend state enables blending");

  keys = makeBlendKeys(makeFlatDrawFixture(desc).hot, true);
  check(!keys[0].blendingEnabled, "force-visible mode disables blending");
}

void testBlendOperationFallbacks() {
  DrawDesc desc{};
  desc.rs.values[RS_BLEND_OP] = static_cast<u32>(BlendOp::Subtract);

  auto keys = makeBlendKeys(makeFlatDrawFixture(desc).hot);
  checkEq(keys[0].rgbBlendOperation,
          static_cast<u32>(BlendOp::Subtract),
          "RGB blend op reflects render state");
  checkEq(keys[0].alphaBlendOperation,
          static_cast<u32>(BlendOp::Subtract),
          "missing alpha blend op falls back to RGB blend op");

  desc.rs.values[RS_BLEND_OP_ALPHA] = static_cast<u32>(BlendOp::Max);
  keys = makeBlendKeys(makeFlatDrawFixture(desc).hot);
  checkEq(keys[0].alphaBlendOperation,
          static_cast<u32>(BlendOp::Subtract),
          "alpha blend op override is ignored when separate alpha blend is disabled");

  desc.rs.values[RS_SEPARATE_ALPHA_BLEND_ENABLE] = 1u;
  keys = makeBlendKeys(makeFlatDrawFixture(desc).hot);
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

  auto keys = makeBlendKeys(makeFlatDrawFixture(desc).hot);
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
  keys = makeBlendKeys(makeFlatDrawFixture(desc).hot);
  checkEq(keys[0].sourceAlphaBlendFactor,
          static_cast<u32>(BlendFactor::SrcAlpha),
          "source alpha blend factor override is ignored when separate alpha blend is disabled");
  checkEq(keys[0].destinationAlphaBlendFactor,
          static_cast<u32>(BlendFactor::InvDestAlpha),
          "destination alpha blend factor override is ignored when separate alpha blend is disabled");

  desc.rs.values[RS_SEPARATE_ALPHA_BLEND_ENABLE] = 1u;
  keys = makeBlendKeys(makeFlatDrawFixture(desc).hot);
  checkEq(keys[0].sourceAlphaBlendFactor,
          static_cast<u32>(BlendFactor::One),
          "source alpha blend factor override is reflected");
  checkEq(keys[0].destinationAlphaBlendFactor,
          static_cast<u32>(BlendFactor::Zero),
          "destination alpha blend factor override is reflected");
}

void testMrtColorWriteMaskDefaultAndOverride() {
  DrawDesc desc{};
  auto keys = makeBlendKeys(makeFlatDrawFixture(desc).hot);

  for (std::size_t i = 0; i < kMaxRenderTargets; ++i) {
    checkEq(keys[i].colorWriteMask, 0xfu, "missing color write mask defaults to all channels");
    checkEq(keys[i].pixelFormat, 0u, "render-state helper leaves pixel format unresolved");
  }

  desc.rs.values[RS_COLOR_WRITE_ENABLE] = 0x5u;
  keys = makeBlendKeys(makeFlatDrawFixture(desc).hot);
  for (std::size_t i = 0; i < kMaxRenderTargets; ++i) {
    checkEq(keys[i].colorWriteMask, 0x5u, "color write mask override applies to every MRT key");
  }

  keys = makeBlendKeys(makeFlatDrawFixture(desc).hot, true);
  for (std::size_t i = 0; i < kMaxRenderTargets; ++i) {
    checkEq(keys[i].colorWriteMask, 0xfu, "force-visible mode restores all-channel writes");
  }
}

void testMrtPerRenderTargetColorWriteMask() {
  // (a) default: every attachment writes full RGBA.
  DrawDesc desc{};
  auto keys = makeBlendKeys(makeFlatDrawFixture(desc).hot);
  for (std::size_t i = 0; i < kMaxRenderTargets; ++i) {
    checkEq(keys[i].colorWriteMask, 0xfu, "default per-RT color write mask is all channels");
  }

  // (b) RS_COLOR_WRITE_ENABLE1 partial mask changes ONLY attachment 1.
  desc.rs.values[RS_COLOR_WRITE_ENABLE1] = 0x5u;
  keys = makeBlendKeys(makeFlatDrawFixture(desc).hot);
  checkEq(keys[0].colorWriteMask, 0xfu, "attachment 0 unaffected by RS_COLOR_WRITE_ENABLE1");
  checkEq(keys[1].colorWriteMask, 0x5u, "attachment 1 driven by RS_COLOR_WRITE_ENABLE1");
  checkEq(keys[2].colorWriteMask, 0xfu, "attachment 2 unaffected by RS_COLOR_WRITE_ENABLE1");
  checkEq(keys[3].colorWriteMask, 0xfu, "attachment 3 unaffected by RS_COLOR_WRITE_ENABLE1");

  // (c) attachment 0 still driven by RS_COLOR_WRITE_ENABLE; per-RT slots are
  // independent and 2/3 keep falling back to the all-channels default.
  desc.rs.values[RS_COLOR_WRITE_ENABLE] = 0x3u;
  keys = makeBlendKeys(makeFlatDrawFixture(desc).hot);
  checkEq(keys[0].colorWriteMask, 0x3u, "attachment 0 driven by RS_COLOR_WRITE_ENABLE");
  checkEq(keys[1].colorWriteMask, 0x5u, "attachment 1 still driven by RS_COLOR_WRITE_ENABLE1");
  checkEq(keys[2].colorWriteMask, 0xfu,
          "attachment 2 falls back to all channels when its per-RT slot is unset");
  checkEq(keys[3].colorWriteMask, 0xfu,
          "attachment 3 falls back to all channels when its per-RT slot is unset");

  // RS_COLOR_WRITE_ENABLE2 / ...ENABLE3 drive attachments 2 and 3 respectively.
  desc.rs.values[RS_COLOR_WRITE_ENABLE2] = 0x9u;
  desc.rs.values[RS_COLOR_WRITE_ENABLE3] = 0x6u;
  keys = makeBlendKeys(makeFlatDrawFixture(desc).hot);
  checkEq(keys[2].colorWriteMask, 0x9u, "attachment 2 driven by RS_COLOR_WRITE_ENABLE2");
  checkEq(keys[3].colorWriteMask, 0x6u, "attachment 3 driven by RS_COLOR_WRITE_ENABLE3");

  // force-visible mode overrides every per-RT mask back to all channels.
  keys = makeBlendKeys(makeFlatDrawFixture(desc).hot, true);
  for (std::size_t i = 0; i < kMaxRenderTargets; ++i) {
    checkEq(keys[i].colorWriteMask, 0xfu,
            "force-visible mode restores all-channel writes for every per-RT slot");
  }
}

void testShaderVariantKeyReflectsSamplerTextureAndFiltering() {
  DrawDesc desc{};
  desc.vertexShader.hash = 0x101u;
  desc.pixelShader.hash = 0x202u;

  const auto untextured = makeVariantKey(makeFlatDrawFixture(desc));
  check(!untextured.textured, "missing texture binding leaves variant untextured");
  check(!untextured.linear, "missing sampler filter state leaves variant nearest");

  desc.textures[0].handle = Handle{0x44u};
  desc.samplers[0].states[SAMP_MIN_FILTER] = 1u;
  desc.samplers[0].states[SAMP_MAG_FILTER] = 1u;
  const auto nearest = makeVariantKey(makeFlatDrawFixture(desc));
  check(nearest.textured, "texture binding marks variant textured");
  check(!nearest.linear, "nearest min/mag filters keep variant nearest");
  check(dxmt9::pipeline::ShaderVariantKeyHash{}(nearest) !=
            dxmt9::pipeline::ShaderVariantKeyHash{}(untextured),
        "textured sampler key changes the PSO hash");

  desc.samplers[0].states[SAMP_MIN_FILTER] = 2u;
  const auto linearMin = makeVariantKey(makeFlatDrawFixture(desc));
  check(linearMin.linear, "linear min filter marks variant linear");
  check(dxmt9::pipeline::ShaderVariantKeyHash{}(linearMin) !=
            dxmt9::pipeline::ShaderVariantKeyHash{}(nearest),
        "linear min filter changes the PSO hash");

  desc.samplers[0].states[SAMP_MIN_FILTER] = 1u;
  desc.samplers[0].states[SAMP_MAG_FILTER] = 2u;
  const auto linearMag = makeVariantKey(makeFlatDrawFixture(desc));
  check(linearMag.linear, "linear mag filter marks variant linear");
}

void testFvfLayoutHashIsDeterministicAndResponsive() {
  DrawDesc desc{};
  desc.vertexDecl.fvf =
      dxmt9::ffp::kFvfXyzrhw | dxmt9::ffp::kFvfDiffuse | (1u << dxmt9::ffp::kFvfTexCountShift);

  const auto layout = dxmt9::ffp::decodeFixedFunctionVertexLayout(desc.vertexDecl);
  const auto layoutAgain = dxmt9::ffp::decodeFixedFunctionVertexLayout(desc.vertexDecl);
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
  const auto specularLayout = dxmt9::ffp::decodeFixedFunctionVertexLayout(withSpecular.vertexDecl);
  check(specularLayout.has_value(), "specular FVF layout decodes");
  checkEq(specularLayout->stride, 32u, "specular FVF increases stride");
  checkEq(specularLayout->texcoordOffset[0], 24u, "specular FVF shifts texcoord offset");
  check(specularLayout->hash != layout->hash, "FVF layout hash changes when vertex layout changes");

  DrawDesc beta = desc;
  beta.vertexDecl.fvf = dxmt9::ffp::kFvfXyzB2 | dxmt9::ffp::kFvfDiffuse;
  const auto betaLayout = dxmt9::ffp::decodeFixedFunctionVertexLayout(beta.vertexDecl);
  check(betaLayout.has_value(), "XYZB2 FVF layout decodes");
  check(betaLayout->hasBlendWeight, "XYZB2 FVF creates blend weights");
  checkEq(betaLayout->blendWeightOffset, 12u, "XYZB2 blend weight offset");
  checkEq(betaLayout->blendWeightComponents, 2u, "XYZB2 blend weight components");
  checkEq(betaLayout->diffuseOffset, 20u, "XYZB2 diffuse offset");
  checkEq(betaLayout->stride, 24u, "XYZB2 diffuse stride");
  check(betaLayout->hash != layout->hash, "XYZB2 layout hash differs from XYZRHW layout");
}

void testShaderVariantKeyHashRespondsToLayoutAndBlendState() {
  DrawDesc desc{};
  desc.vertexShader.hash = 0xaaaau;
  desc.pixelShader.hash = 0x5555u;
  desc.vertexDecl.fvf =
      dxmt9::ffp::kFvfXyz | dxmt9::ffp::kFvfDiffuse | (1u << dxmt9::ffp::kFvfTexCountShift);

  const auto base = makeVariantKey(makeFlatDrawFixture(desc));
  const auto baseHash = dxmt9::pipeline::ShaderVariantKeyHash{}(base);

  DrawDesc layoutChanged = desc;
  layoutChanged.vertexDecl.fvf |= dxmt9::ffp::kFvfSpecular;
  const auto layoutKey = makeVariantKey(makeFlatDrawFixture(layoutChanged));
  check(layoutKey.hash != base.hash, "layout changes alter the shader variant layout hash");
  check(dxmt9::pipeline::ShaderVariantKeyHash{}(layoutKey) != baseHash,
        "layout changes alter the PSO key hash");

  DrawDesc blendChanged = desc;
  blendChanged.rs.values[RS_ALPHABLEND_ENABLE] = 1u;
  blendChanged.rs.values[RS_COLOR_WRITE_ENABLE] = 0x7u;
  const auto blendKey = makeVariantKey(makeFlatDrawFixture(blendChanged));
  check(!(blendKey == base), "blend state changes alter the PSO key");
  check(dxmt9::pipeline::ShaderVariantKeyHash{}(blendKey) != baseHash,
        "blend state changes alter the PSO key hash");
}

void testShaderVariantKeyCarriesSourceIdentity() {
  DrawDesc desc{};
  desc.vertexShader.hash = 0x4444u;
  desc.pixelShader.hash = 0x8888u;

  const auto base = makeVariantKey(makeFlatDrawFixture(desc));
  checkEq(base.emitterVersion,
          dxmt9::pipeline::kShaderEmitterVersion,
          "variant key stamps shader emitter version");
  checkEq(base.sourceLayoutVersion,
          dxmt9::pipeline::kShaderSourceLayoutVersion,
          "variant key stamps shader source layout version");
  checkEq(base.debugEnvSchemaVersion,
          dxmt9::pipeline::kShaderDebugEnvSchemaVersion,
          "variant key stamps debug-env schema version");
  checkEq(base.debugEnvKey,
          dxmt9::pipeline::currentShaderSourceDebugEnvKey(),
          "variant key stamps current source-affecting debug env key");

  const auto baseHash = dxmt9::pipeline::ShaderVariantKeyHash{}(base);

  auto emitterChanged = base;
  ++emitterChanged.emitterVersion;
  check(!(emitterChanged == base), "emitter version changes key equality");
  check(dxmt9::pipeline::ShaderVariantKeyHash{}(emitterChanged) != baseHash,
        "emitter version changes key hash");

  auto layoutChanged = base;
  ++layoutChanged.sourceLayoutVersion;
  check(!(layoutChanged == base), "source layout version changes key equality");
  check(dxmt9::pipeline::ShaderVariantKeyHash{}(layoutChanged) != baseHash,
        "source layout version changes key hash");

  auto debugSchemaChanged = base;
  ++debugSchemaChanged.debugEnvSchemaVersion;
  check(!(debugSchemaChanged == base), "debug env schema version changes key equality");
  check(dxmt9::pipeline::ShaderVariantKeyHash{}(debugSchemaChanged) != baseHash,
        "debug env schema version changes key hash");

  const auto debugOff = dxmt9::pipeline::makeShaderSourceDebugEnvKey(
      /*trimUnusedVaryings=*/false,
      /*fsHalfPrecision=*/false,
      /*forceFullscreenVertex=*/false,
      /*flipTranslatedVertexY=*/false,
      /*forceFragmentShaderColor=*/false,
      "",
      /*forcePixelVFlip=*/false,
      /*debugFfpUv=*/false,
      /*debugFfpTexture=*/false,
      /*debugFfpAlpha=*/false);
  const auto debugUv = dxmt9::pipeline::makeShaderSourceDebugEnvKey(
      /*trimUnusedVaryings=*/true,
      /*fsHalfPrecision=*/false,
      /*forceFullscreenVertex=*/false,
      /*flipTranslatedVertexY=*/false,
      /*forceFragmentShaderColor=*/false,
      "uv",
      /*forcePixelVFlip=*/false,
      /*debugFfpUv=*/false,
      /*debugFfpTexture=*/false,
      /*debugFfpAlpha=*/false);
  check(debugOff != debugUv, "pure debug env key responds to source-affecting values");

  auto debugEnvChanged = base;
  debugEnvChanged.debugEnvKey = base.debugEnvKey ^ 0x9e3779b97f4a7c15ull;
  check(!(debugEnvChanged == base), "source-affecting debug env changes key equality");
  check(dxmt9::pipeline::ShaderVariantKeyHash{}(debugEnvChanged) != baseHash,
        "source-affecting debug env changes key hash");

  auto vertexSourceChanged = base;
  vertexSourceChanged.vertexSourceHash = 0x1111222233334444ull;
  check(!(vertexSourceChanged == base), "actual VS source hash changes key equality");
  check(dxmt9::pipeline::ShaderVariantKeyHash{}(vertexSourceChanged) != baseHash,
        "actual VS source hash changes key hash");

  auto fragmentSourceChanged = base;
  fragmentSourceChanged.fragmentSourceHash = 0x5555666677778888ull;
  check(!(fragmentSourceChanged == base), "actual FS source hash changes key equality");
  check(dxmt9::pipeline::ShaderVariantKeyHash{}(fragmentSourceChanged) != baseHash,
        "actual FS source hash changes key hash");

  auto tileSourceChanged = base;
  tileSourceChanged.tileSourceHash = 0x9999aaaabbbbccccull;
  check(!(tileSourceChanged == base), "actual tile source hash changes key equality");
  check(dxmt9::pipeline::ShaderVariantKeyHash{}(tileSourceChanged) != baseHash,
        "actual tile source hash changes key hash");
}

void testShaderVariantProbeKeyDropsOnlyActualSourceHashes() {
  DrawDesc desc{};
  desc.vertexShader.hash = 0x4444u;
  desc.pixelShader.hash = 0x8888u;

  auto sourceBacked = makeVariantKey(makeFlatDrawFixture(desc));
  sourceBacked.vertexSourceHash = 0x1111222233334444ull;
  sourceBacked.fragmentSourceHash = 0x5555666677778888ull;
  sourceBacked.tileSourceHash = 0x9999aaaabbbbccccull;
  sourceBacked.argbufHybridMode = true;
  sourceBacked.sampleCount = 4u;

  auto expectedProbe = sourceBacked;
  expectedProbe.vertexSourceHash = 0;
  expectedProbe.fragmentSourceHash = 0;
  expectedProbe.tileSourceHash = 0;

  const auto probe = dxmt9::pipeline::makeShaderVariantProbeKey(sourceBacked);
  check(probe == expectedProbe,
        "probe key preserves canonical variant fields and drops only actual source hashes");
  check(dxmt9::pipeline::ShaderVariantKeyHash{}(probe) !=
            dxmt9::pipeline::ShaderVariantKeyHash{}(sourceBacked),
        "probe and source-backed keys occupy distinct hash identities");
}

void testContainedDrawShaderSourcesCarryActualSourceHashes() {
  DrawDesc desc{};
  desc.vertexShader.hash = 0x1001u;
  desc.pixelShader.hash = 0x2002u;
  desc.vertexDecl.fvf =
      dxmt9::ffp::kFvfXyzrhw | dxmt9::ffp::kFvfDiffuse | (1u << dxmt9::ffp::kFvfTexCountShift);

  const auto context = dxmt9::drawshader::makeShaderSourceContext(desc);
  const auto sources = dxmt9::pipeline::detail::makeContainedDrawShaderSources(
      context, 0xabcdef1234567890ull);

  check(sources.has_value(), "default FFP draw sources are generated");
  check(!sources->vertex.empty(), "generated VS source is non-empty");
  check(!sources->fragment.empty(), "generated FS source is non-empty");
  checkEq(sources->vertexHash,
          dxmt9::shaders::makeHash(sources->vertex),
          "VS source hash matches the actual generated MSL text");
  checkEq(sources->fragmentHash,
          dxmt9::shaders::makeHash(sources->fragment),
          "FS source hash matches the actual generated MSL text");
}

void testProgrammableShaderVariantKeyUsesFullVertexDeclHash() {
  constexpr u32 kD3DDeclTypeFloat2 = 1u;
  constexpr u32 kD3DDeclTypeFloat3 = 2u;
  constexpr u32 kD3DDeclMethodDefault = 0u;
  constexpr u32 kD3DDeclUsagePosition = 0u;
  constexpr u32 kD3DDeclUsageTexcoord = 5u;

  DrawDesc desc{};
  desc.vertexShader.kind = ShaderRef::Kind::Bytecode;
  desc.vertexShader.hash = 0xabcdu;
  desc.pixelShader.hash = 0x1234u;
  desc.vertexDecl.elements = {
      VertexElement{0, 0, kD3DDeclTypeFloat3, kD3DDeclMethodDefault, kD3DDeclUsagePosition, 0},
      VertexElement{1, 12, kD3DDeclTypeFloat2, kD3DDeclMethodDefault, kD3DDeclUsageTexcoord, 0},
  };
  desc.vertexDecl.streams[0].stride = 12u;
  desc.vertexDecl.streams[1].stride = 20u;

  const auto base = makeVariantKey(makeFlatDrawFixture(desc));

  auto changedStreamStride = desc;
  changedStreamStride.vertexDecl.streams[1].stride = 24u;
  const auto streamStrideKey = makeVariantKey(makeFlatDrawFixture(changedStreamStride));
  check(streamStrideKey.hash != base.hash,
        "programmable VS variant key changes when a nonzero stream stride changes");
  check(dxmt9::pipeline::ShaderVariantKeyHash{}(streamStrideKey) !=
            dxmt9::pipeline::ShaderVariantKeyHash{}(base),
        "programmable VS PSO cache hash includes nonzero stream stride");

  auto movedSemantic = desc;
  movedSemantic.vertexDecl.elements[1].stream = 0u;
  movedSemantic.vertexDecl.elements[1].offset = 12u;
  const auto movedSemanticKey = makeVariantKey(makeFlatDrawFixture(movedSemantic));
  check(movedSemanticKey.hash != base.hash,
        "programmable VS variant key changes when a semantic crosses stream boundaries");
}

void testPipelineHelpersUseExplicitFlatInputs() {
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

  const auto flat = makeFlatDrawFixture(desc);
  const auto blend = makeBlendKeys(flat.hot);
  const auto variant = makeVariantKey(flat);

  check(blend[0].blendingEnabled, "flat blend helper reads alpha blend state");
  checkEq(blend[0].sourceRGBBlendFactor,
          static_cast<u32>(BlendFactor::SrcAlpha),
          "flat blend helper reads source blend factor");
  checkEq(blend[0].destinationRGBBlendFactor,
          static_cast<u32>(BlendFactor::InvSrcAlpha),
          "flat blend helper reads destination blend factor");
  checkEq(blend[0].colorWriteMask, 0x7u, "flat blend helper reads color write mask");
  check(variant.textured, "flat variant helper reads texture bindings");
  check(variant.linear, "flat variant helper reads sampler filtering");
  check(variant.clipPlanes, "flat variant helper reads clip plane mask");
  check(variant.alphaTest, "flat variant helper reads alpha test state");
  checkEq(variant.sampleCount, 4u, "flat variant helper reads render target sample count");
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

void testUnsupportedDrawTranslatorFailureReturnsEmptyPipelineFuture() {
  DrawDesc desc{};
  desc.pixelShader = makeUnsupportedOpcodePixelShader();
  auto context = dxmt9::drawshader::makeShaderSourceContext(desc);

  dxmt9::pipeline::ShaderVariantKey key{};
  key.hash = 0xfeed1234ull;
  key.colorFormats[0] = WMTPixelFormatBGRA8Unorm;
  key.blend[0].pixelFormat = WMTPixelFormatBGRA8Unorm;

  const auto contained =
      dxmt9::pipeline::detail::makeContainedDrawShaderSources(context, key.hash);
  check(!contained.has_value(),
        "unsupported draw shader bytecode is contained during source generation");

  dxmt9::pipeline::Cache cache;
  auto future = cache.getOrBuildDrawPipeline(
      WMT::Reference<WMT::Device>{}, key, std::move(context), nullptr, nullptr);

  try {
    const auto& pso = future.get();
    check(!pso, "unsupported draw shader bytecode resolves to an empty pipeline");
  } catch (const std::exception& ex) {
    fail(std::string("unsupported draw shader bytecode escaped the pipeline future: ") + ex.what());
  }
}

}  // namespace

int main() {
  try {
    testAlphaBlendEnableAndDisable();
    testBlendOperationFallbacks();
    testBlendFactorFallbacks();
    testMrtColorWriteMaskDefaultAndOverride();
    testMrtPerRenderTargetColorWriteMask();
    testShaderVariantKeyReflectsSamplerTextureAndFiltering();
    testFvfLayoutHashIsDeterministicAndResponsive();
    testShaderVariantKeyHashRespondsToLayoutAndBlendState();
    testShaderVariantKeyCarriesSourceIdentity();
    testShaderVariantProbeKeyDropsOnlyActualSourceHashes();
    testContainedDrawShaderSourcesCarryActualSourceHashes();
    testProgrammableShaderVariantKeyUsesFullVertexDeclHash();
    testPipelineHelpersUseExplicitFlatInputs();
    testSrgbCompatiblePixelFormatConversion();
    testUnsupportedDrawTranslatorFailureReturnsEmptyPipelineFuture();
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
