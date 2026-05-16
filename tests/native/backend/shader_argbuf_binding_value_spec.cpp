// R-TEST-0.10 B5 - draw state -> shader arg-buffer texture/sampler bindings.
//
// CPU-only value-boundary spec for dxmt9-shader-argbuf-binding-value-spec.
// This test intentionally stops at the strongest observable boundary in
// the native build:
//
//   FlatDrawStateRecord texture slot N
//     -> ShaderSourceContext::textures[N]
//     -> Stage 1 MSL texN/sampN [[texture(N)]]/[[sampler(N)]]
//     -> Stage 2 MSL texN/sampN aliases from abuf->textures2d[N]/samplers[N]
//     -> Stage 2 descriptor ids texture=4+N, sampler=12+N.
//     -> Encoder call-plan values consumed by setFragmentTexture/Sampler,
//        setViewport, setScissorRect, and setRasterizerCullMode.
//
// The final Metal object writes still require a live device/encoder, but
// the encoder-facing call plans are pure helpers so this spec can pin the
// exact stage/viewport/scissor/cull values before the live Metal seam.

#include <array>
#include <cmath>
#include <cstdint>
#include <exception>
#include <iostream>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "dxmt9/core.hpp"
#include "../../../src/dxmt9/dxmt9_argbuf_hybrid.hpp"
#include "../../../src/dxmt9/dxmt9_draw_encoder.hpp"
#include "../../../src/dxmt9/dxmt9_draw_encoder_internal.hpp"
#include "../../../src/dxmt9/dxmt9_draw_shader.hpp"
#include "../../../src/dxmt9/dxmt9_ffp_shaders.hpp"
#include "../../../src/dxmt9/dxmt9_shader_translator.hpp"
#include "../../../src/winemetal/winemetal.h"

using namespace dxmt9::core;
using namespace dxmt9::core::fixture;

namespace {

using u8 = std::uint8_t;
using u32 = std::uint32_t;

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

void checkNear(float left, float right, float epsilon, std::string_view message) {
  if (std::fabs(left - right) > epsilon) {
    std::ostringstream out;
    out << message << " (" << left << " vs " << right << ")";
    fail(out.str());
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

std::string slotName(std::string_view prefix, u32 stage) {
  return std::string(prefix) + std::to_string(stage);
}

std::string stage1TextureBinding(u32 stage) {
  std::ostringstream out;
  out << "texture2d<float> tex" << stage << " [[texture(" << stage << ")]]";
  return out.str();
}

std::string stage1SamplerBinding(u32 stage) {
  std::ostringstream out;
  out << "sampler samp" << stage << " [[sampler(" << stage << ")]]";
  return out.str();
}

std::string stage2TextureAlias(u32 stage) {
  std::ostringstream out;
  out << "texture2d<float> tex" << stage << " = abuf->textures2d[" << stage << "]";
  return out.str();
}

std::string stage2SamplerAlias(u32 stage) {
  std::ostringstream out;
  out << "sampler samp" << stage << " = abuf->samplers[" << stage << "]";
  return out.str();
}

ShaderRef makePixelShaderSamplingStage(u32 stage) {
  // ps_2_0:
  //   dcl t0.xy
  //   dcl_2d sN
  //   texld r0, t0, sN
  //   mov oC0, r0
  // The sampler index lives in the low bits of the sampler register
  // tokens. The existing translator spec uses the same token shape for
  // sampler 2; here we parameterize N so first/middle/last argbuf slots
  // are covered without pulling in a GPU runner.
  const std::array<u32, 15> bytecode = {
      0xffff0200u,
      0x0200001fu, 0x80000000u, 0xb0030000u,
      0x0200001fu, 0x90000000u, 0xa00f0800u | stage,
      0x03000042u, 0x800f0000u, 0xb0e40000u, 0xa0e40800u | stage,
      0x02000001u, 0x800f0800u, 0x80e40000u,
      0x0000ffffu,
  };

  ShaderRef shader{};
  shader.kind = ShaderRef::Kind::Bytecode;
  const auto* base = reinterpret_cast<const u8*>(bytecode.data());
  shader.bytecode.bytes.assign(base, base + bytecode.size() * sizeof(u32));
  shader.bytecode.hash = hashBytes(std::as_bytes(std::span<const u32>(
      bytecode.data(), bytecode.size())));
  return shader;
}

dxmt9::drawshader::ShaderSourceContext makeContextFromFlat(DrawDesc desc,
                                                            bool argbufHybridMode) {
  const auto hot = makeFlatDrawStateRecord(desc);
  const auto layout = makeDrawShaderLayoutContext(desc);
  auto context = dxmt9::drawshader::makeShaderSourceContext(layout, hot);
  context.argbufHybridMode = argbufHybridMode;
  return context;
}

FfpPixelKey makeSingleTextureStageKey(u32 stage) {
  FfpPixelKey key{};
  auto& s = key.stages[stage];
  s.colorOp = static_cast<u32>(TextureOp::Modulate);
  s.alphaOp = static_cast<u32>(TextureOp::Modulate);
  s.colorArg1 = 2u;  // texture
  s.colorArg2 = 0u;  // diffuse
  s.alphaArg1 = 2u;
  s.alphaArg2 = 0u;
  s.texCoordIndex = stage;
  return key;
}

std::uint32_t textureArgbufId(u32 stage) {
  return dxmt9::shaders::kArgbufHybridTexture2DBase + stage;
}

std::uint32_t samplerArgbufId(u32 stage) {
  return dxmt9::shaders::kArgbufHybridSamplerBase + stage;
}

// ---------------------------------------------------------------------
// B5.1 - flat draw state preserves slot values and null/default state.

void testFlatDrawStateTextureSlotsFeedShaderContext() {
  DrawDesc desc{};
  desc.textures[0].handle = Handle{0x1000u};
  desc.textures[7].handle = Handle{0x7000u};
  desc.samplers[7].states[SAMP_MIN_FILTER] = 2u;
  desc.samplers[7].states[SAMP_MIP_FILTER] = 2u;

  const auto hot = makeFlatDrawStateRecord(desc);
  const auto layout = makeDrawShaderLayoutContext(desc);
  const auto context = dxmt9::drawshader::makeShaderSourceContext(layout, hot);

  checkEq(dxmt9::shaders::kArgbufHybridTextureSlotCount, kMaxTextureStages,
          "argbuf texture array covers the 8 fixed-function texture stages");
  checkEq(dxmt9::shaders::kArgbufHybridSamplerSlotCount, kMaxTextureStages,
          "argbuf sampler array covers the same 8 stage slots");

  checkEq(hot.textures[0], Handle{0x1000u}, "slot 0 texture handle preserved");
  checkEq(hot.textures[7], Handle{0x7000u}, "slot 7 texture handle preserved");
  checkEq(hot.textures[3], Handle{}, "null slot 3 stays a zero handle");
  checkEq(hot.textureMask, (1u << 0) | (1u << 7),
          "textureMask contains only non-null slots");

  for (u32 stage = 0; stage < dxmt9::shaders::kArgbufHybridTextureSlotCount; ++stage) {
    checkEq(context.textures[stage], hot.textures[stage] != Handle{},
            "ShaderSourceContext texture bit mirrors FlatDrawStateRecord slot");
  }
}

// ---------------------------------------------------------------------
// B5.2 - Stage 1 / Stage 2 shader binding values and descriptor ids.

void assertShaderSlotMapsToSameStage(u32 stage) {
  DrawDesc desc{};
  desc.pixelShader = makePixelShaderSamplingStage(stage);
  desc.textures[stage].handle = Handle{0x5000u + stage};

  const auto hot = makeFlatDrawStateRecord(desc);
  const auto layout = makeDrawShaderLayoutContext(desc);

  auto stage1Context = dxmt9::drawshader::makeShaderSourceContext(layout, hot);
  stage1Context.argbufHybridMode = false;
  const auto stage1 = dxmt9::translator::makeTranslatedFragmentSource(
      desc.pixelShader, stage1Context);

  auto stage2Context = dxmt9::drawshader::makeShaderSourceContext(layout, hot);
  stage2Context.argbufHybridMode = true;
  const auto stage2 = dxmt9::translator::makeTranslatedFragmentSource(
      desc.pixelShader, stage2Context);

  checkContains(stage1, stage1TextureBinding(stage),
                "Stage 1 translated shader binds texN at texture(N)");
  checkContains(stage1, stage1SamplerBinding(stage),
                "Stage 1 translated shader binds sampN at sampler(N)");
  checkContains(stage1, slotName("tex", stage) + ".sample(" + slotName("samp", stage),
                "Stage 1 shader body samples texN with sampN");

  checkContains(stage2, "[[buffer(30)]]",
                "Stage 2 translated shader binds the argbuf at slot 30");
  checkContains(stage2, stage2TextureAlias(stage),
                "Stage 2 translated shader aliases texN from argbuf textures2d[N]");
  checkContains(stage2, stage2SamplerAlias(stage),
                "Stage 2 translated shader aliases sampN from argbuf samplers[N]");
  checkContains(stage2, slotName("tex", stage) + ".sample(" + slotName("samp", stage),
                "Stage 2 shader body still samples texN with sampN");
  checkNotContains(stage2, "[[texture(" + std::to_string(stage) + ")]]",
                   "Stage 2 shader does not keep a direct texture(N) bind");
  checkNotContains(stage2, "[[sampler(" + std::to_string(stage) + ")]]",
                   "Stage 2 shader does not keep a direct sampler(N) bind");

  const auto descriptors = dxmt9::argbuf_hybrid::buildArgumentDescriptors();
  const auto textureId = textureArgbufId(stage);
  const auto samplerId = samplerArgbufId(stage);
  const auto& textureDesc = descriptors.entries[textureId];
  const auto& samplerDesc = descriptors.entries[samplerId];

  checkEq(static_cast<std::uint32_t>(textureDesc.argumentType),
          static_cast<std::uint32_t>(WMTArgumentTypeTexture),
          "Stage 2 descriptor for textures2d[N] is a texture");
  checkEq(static_cast<std::uint32_t>(textureDesc.index), textureId,
          "Stage 2 2D texture descriptor index is 4 + N");
  checkEq(static_cast<std::uint32_t>(samplerDesc.argumentType),
          static_cast<std::uint32_t>(WMTArgumentTypeSampler),
          "Stage 2 descriptor for samplers[N] is a sampler");
  checkEq(static_cast<std::uint32_t>(samplerDesc.index), samplerId,
          "Stage 2 sampler descriptor index is 12 + N");
}

void testStageBindingsForFirstMiddleAndLastArgbufSlots() {
  assertShaderSlotMapsToSameStage(0u);
  assertShaderSlotMapsToSameStage(3u);
  assertShaderSlotMapsToSameStage(7u);
}

// ---------------------------------------------------------------------
// B5.3 - null/default texture slot behavior where source generation can
// observe it.

void testNullFfpTextureSlotDoesNotMaterializeTextureBinding() {
  constexpr u32 kStage = 5u;
  DrawDesc desc{};
  desc.pixelShader.kind = ShaderRef::Kind::FixedFunctionPixel;
  desc.pixelShader.pixelKey = makeSingleTextureStageKey(kStage);

  const auto hot = makeFlatDrawStateRecord(desc);
  checkEq(hot.textures[kStage], Handle{},
          "FFP null texture stage stays unbound in FlatDrawStateRecord");
  check((hot.textureMask & (1u << kStage)) == 0u,
        "FFP null texture stage does not set textureMask");

  auto context = makeContextFromFlat(desc, /*argbufHybridMode=*/true);
  const auto src = dxmt9::ffp::makeFfpPixelSource(
      *desc.pixelShader.pixelKey, context);
  checkContains(src, "[[buffer(30)]]",
                "Stage 2 FFP still binds argbuf for constants with no texture");
  checkNotContains(src, stage2TextureAlias(kStage),
                   "null FFP texture stage does not alias abuf textures2d[N]");
  checkNotContains(src, stage2SamplerAlias(kStage),
                   "null FFP texture stage does not alias abuf samplers[N]");
  checkNotContains(src, "[[texture(5)]]",
                   "null FFP texture stage does not direct-bind texture(5)");
  checkNotContains(src, "[[sampler(5)]]",
                   "null FFP texture stage does not direct-bind sampler(5)");
}

// ---------------------------------------------------------------------
// B5.4 - sampler defaults and LOD assumptions observable without Metal.

void testFlatSamplerInfoMatchesSnapshotAndPinsLodDefaults() {
  constexpr u32 kStage = 7u;
  SamplerSnapshot snapshot{};
  snapshot.states[SAMP_MIN_FILTER] = 2u;
  snapshot.states[SAMP_MAG_FILTER] = 1u;
  snapshot.states[SAMP_MIP_FILTER] = 2u;
  snapshot.states[SAMP_ADDRESS_U] = 3u;
  snapshot.states[SAMP_ADDRESS_V] = 2u;
  snapshot.states[SAMP_MAX_ANISOTROPY] = 8u;
  snapshot.states[SAMP_MIPMAP_LOD_BIAS] = 0x3fc00000u;  // 1.5f, intentionally ignored.

  DrawDesc desc{};
  desc.textures[kStage].handle = Handle{0x7000u};
  desc.samplers[kStage] = snapshot;
  const auto hot = makeFlatDrawStateRecord(desc);

  const auto snapshotInfo = dxmt9::encoders::makeSamplerInfo(snapshot);
  const auto flatInfo = dxmt9::encoders::makeSamplerInfo(hot.samplerStates[kStage]);

  checkEq(flatInfo.min_filter, snapshotInfo.min_filter,
          "flat sampler min filter matches snapshot at slot N");
  checkEq(flatInfo.mag_filter, snapshotInfo.mag_filter,
          "flat sampler mag filter matches snapshot at slot N");
  checkEq(flatInfo.mip_filter, WMTSamplerMipFilterLinear,
          "slot N mip filter maps to linear when SAMP_MIP_FILTER is linear");
  checkEq(flatInfo.s_address_mode, snapshotInfo.s_address_mode,
          "flat sampler address U matches snapshot at slot N");
  checkEq(flatInfo.t_address_mode, snapshotInfo.t_address_mode,
          "flat sampler address V matches snapshot at slot N");
  checkEq(flatInfo.max_anisotroy, 8u,
          "flat sampler max anisotropy carries through at slot N");
  checkNear(flatInfo.lod_min_clamp, 0.0f, 0.0f,
            "LOD min clamp remains 0 even when LOD bias is present");
  checkNear(flatInfo.lod_max_clamp, 1e9f, 0.0f,
            "LOD max clamp stays open for explicit mip sampling");
  check(flatInfo.normalized_coords,
        "sampler coordinates remain normalized");
  check(!flatInfo.lod_average,
        "LOD averaging remains disabled");

  // CPU-visible sampler info does not set support_argument_buffers.
  // Production Stage 2 passes the MTLSamplerState object to
  // MTLArgumentEncoder::setSamplerState rather than testing the bridge
  // gpu_resource_id output. Confirming whether Metal accepts that object
  // in an argument buffer requires a live device or an encoder recorder
  // seam, neither of which exists in this CPU-only test target.
  check(!flatInfo.support_argument_buffers,
        "sampler arg-buffer support flag is not set by the CPU mapper");
}

void testDefaultSamplerInfoForNullTextureSlotIsDeterministic() {
  constexpr u32 kStage = 2u;
  DrawDesc desc{};
  const auto hot = makeFlatDrawStateRecord(desc);
  const auto info = dxmt9::encoders::makeSamplerInfo(hot.samplerStates[kStage]);

  checkEq(hot.textures[kStage], Handle{},
          "default texture slot is null");
  checkEq(info.min_filter, WMTSamplerMinMagFilterNearest,
          "default sampler min filter is nearest");
  checkEq(info.mag_filter, WMTSamplerMinMagFilterNearest,
          "default sampler mag filter is nearest");
  checkEq(info.mip_filter, WMTSamplerMipFilterNotMipmapped,
          "default sampler mip filter is not-mipmapped");
  checkNear(info.lod_min_clamp, 0.0f, 0.0f,
            "default sampler LOD min clamp is 0");
  checkNear(info.lod_max_clamp, 1e9f, 0.0f,
            "default sampler LOD max clamp is open");
  check(info.normalized_coords,
        "default sampler uses normalized coordinates");
  check(!info.support_argument_buffers,
        "default sampler arg-buffer support flag is not set");
}

// ---------------------------------------------------------------------
// B5.5 - encoder-facing call plans preserve stage and raster values.

void testEncoderFragmentBindingPlanMatchesShaderSlots() {
  DrawDesc desc{};
  desc.textures[5].handle = Handle{0x5005u};
  desc.textures[7].handle = Handle{0x5007u};
  desc.samplers[5].states[SAMP_ADDRESS_U] = 3u;
  desc.samplers[5].states[SAMP_ADDRESS_V] = 3u;
  desc.samplers[5].states[SAMP_MIN_FILTER] = 2u;
  desc.samplers[7].states[SAMP_MIP_FILTER] = 2u;

  const auto hot = makeFlatDrawStateRecord(desc);
  const auto bindings = dxmt9::encoders::makeFragmentTextureSamplerBindings(hot);

  checkEq(bindings.size(), std::size_t{2},
          "encoder fragment binding plan contains only non-null texture slots");
  checkEq(bindings[0].stage, 5u,
          "first encoder fragment binding keeps shader sampler slot 5");
  checkEq(bindings[0].texture, Handle{0x5005u},
          "first encoder fragment binding carries texture handle for slot 5");
  checkEq(flatStateOr(bindings[0].samplerStates, SAMP_ADDRESS_U, 0u), 3u,
          "slot 5 encoder sampler plan carries address U");
  checkEq(flatStateOr(bindings[0].samplerStates, SAMP_MIN_FILTER, 0u), 2u,
          "slot 5 encoder sampler plan carries min filter");
  const auto slot5Info = dxmt9::encoders::makeSamplerInfo(bindings[0].samplerStates);
  checkEq(slot5Info.min_filter, WMTSamplerMinMagFilterLinear,
          "slot 5 encoder sampler plan maps linear min filter");
  checkEq(slot5Info.s_address_mode, WMTSamplerAddressModeClampToEdge,
          "slot 5 encoder sampler plan maps clamp address U");

  checkEq(bindings[1].stage, 7u,
          "second encoder fragment binding keeps shader sampler slot 7");
  checkEq(bindings[1].texture, Handle{0x5007u},
          "second encoder fragment binding carries texture handle for slot 7");
  checkEq(flatStateOr(bindings[1].samplerStates, SAMP_MIP_FILTER, 0u), 2u,
          "slot 7 encoder sampler plan carries mip filter");
  const auto slot7Info = dxmt9::encoders::makeSamplerInfo(bindings[1].samplerStates);
  checkEq(slot7Info.mip_filter, WMTSamplerMipFilterLinear,
          "slot 7 encoder sampler plan maps linear mip filter");
}

void testEncoderRasterStatePlanMatchesMetalViewportScissorCull() {
  DrawDesc desc{};
  desc.viewport.viewport = Viewport{8u, 9u, 40u, 30u, 0.25f, 0.75f};
  desc.viewport.scissor = Rect{10, 11, 28, 31};
  desc.viewport.scissorEnabled = true;
  desc.rs.values[RS_CULL_MODE] = static_cast<u32>(CullMode::Cw);

  const auto hot = makeFlatDrawStateRecord(desc);
  const auto plan = dxmt9::encoders::makeEncoderRasterStatePlan(
      hot, 128u, 64u, false, false, false);

  checkNear(static_cast<float>(plan.viewport.originX), 8.0f, 0.0f,
            "encoder viewport origin X carries draw viewport");
  checkNear(static_cast<float>(plan.viewport.originY), 9.0f, 0.0f,
            "encoder viewport origin Y carries draw viewport");
  checkNear(static_cast<float>(plan.viewport.width), 40.0f, 0.0f,
            "encoder viewport width carries draw viewport");
  checkNear(static_cast<float>(plan.viewport.height), 30.0f, 0.0f,
            "encoder viewport height carries draw viewport");
  checkNear(static_cast<float>(plan.viewport.znear), 0.25f, 0.0f,
            "encoder viewport znear carries draw viewport");
  checkNear(static_cast<float>(plan.viewport.zfar), 0.75f, 0.0f,
            "encoder viewport zfar carries draw viewport");
  checkEq(plan.scissor.x, std::uint64_t{10},
          "encoder scissor x carries draw scissor left");
  checkEq(plan.scissor.y, std::uint64_t{11},
          "encoder scissor y carries draw scissor top");
  checkEq(plan.scissor.width, std::uint64_t{18},
          "encoder scissor width carries right-left");
  checkEq(plan.scissor.height, std::uint64_t{20},
          "encoder scissor height carries bottom-top");
  checkEq(plan.cullMode, WMTCullModeFront,
          "D3DCULL_CW encoder plan maps to Metal front cull");

  const auto scissorDisabledPlan = dxmt9::encoders::makeEncoderRasterStatePlan(
      hot, 128u, 64u, false, true, false);
  checkEq(scissorDisabledPlan.scissor.x, std::uint64_t{0},
          "disabled scissor plan starts at surface origin");
  checkEq(scissorDisabledPlan.scissor.y, std::uint64_t{0},
          "disabled scissor plan starts at surface origin");
  checkEq(scissorDisabledPlan.scissor.width, std::uint64_t{128},
          "disabled scissor plan spans surface width");
  checkEq(scissorDisabledPlan.scissor.height, std::uint64_t{64},
          "disabled scissor plan spans surface height");

  const auto preTransformedPlan = dxmt9::encoders::makeEncoderRasterStatePlan(
      hot, 128u, 64u, true, false, false);
  checkNear(static_cast<float>(preTransformedPlan.viewport.originX), 0.0f, 0.0f,
            "pretransformed encoder viewport starts at target origin");
  checkNear(static_cast<float>(preTransformedPlan.viewport.originY), 0.0f, 0.0f,
            "pretransformed encoder viewport starts at target origin");
  checkNear(static_cast<float>(preTransformedPlan.viewport.width), 128.0f, 0.0f,
            "pretransformed encoder viewport spans target width");
  checkNear(static_cast<float>(preTransformedPlan.viewport.height), 64.0f, 0.0f,
            "pretransformed encoder viewport spans target height");
  checkEq(preTransformedPlan.cullMode, WMTCullModeNone,
          "pretransformed encoder plan disables culling");
}

}  // namespace

int main() {
  try {
    testFlatDrawStateTextureSlotsFeedShaderContext();
    testStageBindingsForFirstMiddleAndLastArgbufSlots();
    testNullFfpTextureSlotDoesNotMaterializeTextureBinding();
    testFlatSamplerInfoMatchesSnapshotAndPinsLodDefaults();
    testDefaultSamplerInfoForNullTextureSlotIsDeterministic();
    testEncoderFragmentBindingPlanMatchesShaderSlots();
    testEncoderRasterStatePlanMatchesMetalViewportScissorCull();
  } catch (const TestFailure& failure) {
    std::cerr << "shader_argbuf_binding_value_spec failed: "
              << failure.what() << '\n';
    return 1;
  } catch (const std::exception& ex) {
    std::cerr << "shader_argbuf_binding_value_spec unexpected exception: "
              << ex.what() << '\n';
    return 1;
  }

  std::cout << "shader_argbuf_binding_value_spec passed\n";
  return 0;
}
