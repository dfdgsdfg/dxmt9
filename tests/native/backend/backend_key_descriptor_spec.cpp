#include <array>
#include <bit>
#include <cmath>
#include <exception>
#include <iostream>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

#include "dxmt9/core.hpp"
#include "../../../src/dxmt9/dxmt9_draw_encoder.hpp"
#include "../../../src/dxmt9/dxmt9_draw_encoder_internal.hpp"
#include "../../../src/dxmt9/dxmt9_draw_state.hpp"

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

void checkNear(float left, float right, float epsilon, std::string_view message) {
  if (std::fabs(left - right) > epsilon) {
    std::ostringstream out;
    out << message << " (" << left << " vs " << right << ")";
    fail(out.str());
  }
}

void testBuildPerStageConstsCopiesShaderConstants() {
  DrawDesc desc{};
  desc.vsConst.float4[3] = {1.0f, 2.0f, 3.0f, 4.0f};
  desc.vsConst.int4[2] = {-1, 0, 1, 2};
  desc.vsConst.bools[0] = true;
  desc.vsConst.bools[1] = false;
  desc.vsConst.bools[15] = true;

  desc.psConst.float4[7] = {5.0f, 6.0f, 7.0f, 8.0f};
  desc.psConst.int4[5] = {9, 10, 11, 12};
  desc.psConst.bools[0] = false;
  desc.psConst.bools[4] = true;
  desc.psConst.bools[15] = false;

  const auto hot = makeFlatDrawStateRecord(desc);
  const auto shaderLayout = makeDrawShaderLayoutContext(desc);
  const auto uniformPayload = makeDrawUniformPayload(desc);
  const FlatDrawStateView view{
      .hot = &hot, .shaderLayout = &shaderLayout, .uniforms = &uniformPayload};
  const auto vsConsts = dxmt9::state::buildVsConsts(view);
  const auto psConsts = dxmt9::state::buildPsConsts(view);

  checkEq(vsConsts.vsFloatConst[3], desc.vsConst.float4[3], "VS float constants copied");
  checkEq(vsConsts.vsIntConst[2], desc.vsConst.int4[2], "VS integer constants copied");
  checkEq(psConsts.psFloatConst[7], desc.psConst.float4[7], "PS float constants copied");
  checkEq(psConsts.psIntConst[5], desc.psConst.int4[5], "PS integer constants copied");

  checkEq(vsConsts.vsBoolConst[0], 1u, "VS true bool constant converted to u32 one");
  checkEq(vsConsts.vsBoolConst[1], 0u, "VS false bool constant converted to u32 zero");
  checkEq(vsConsts.vsBoolConst[15], 1u, "VS high bool constant converted to u32 one");
  checkEq(psConsts.psBoolConst[0], 0u, "PS false bool constant converted to u32 zero");
  checkEq(psConsts.psBoolConst[4], 1u, "PS true bool constant converted to u32 one");
  checkEq(psConsts.psBoolConst[15], 0u, "PS high bool constant converted to u32 zero");
}

void testBuildFfpAndVolatileViewportAndRenderStateValues() {
  DrawDesc desc{};
  desc.viewport.viewport = Viewport{12, 34, 320, 240, 0.25f, 0.75f};
  desc.vertexDecl.streams[0].offset = 64;
  desc.vertexDecl.streams[0].stride = 28;
  desc.clipPlaneMask = 0x15u;
  desc.rs.values[RS_TEXTURE_FACTOR] = 0x80402010u;
  desc.rs.values[RS_ALPHA_TEST_ENABLE] = 1u;
  desc.rs.values[RS_ALPHA_FUNC] = static_cast<u32>(CompareFunc::GreaterEqual);
  desc.rs.values[RS_ALPHA_REF] = 128u;
  desc.rs.values[RS_FOG_ENABLE] = 1u;
  desc.rs.values[RS_FOG_COLOR] = 0x7f102040u;
  desc.rs.values[RS_FOG_TABLE_MODE] = static_cast<u32>(FogMode::Exp2);
  desc.rs.values[RS_FOG_START] = std::bit_cast<u32>(2.5f);
  desc.rs.values[RS_FOG_END] = std::bit_cast<u32>(9.75f);
  desc.rs.values[RS_FOG_DENSITY] = std::bit_cast<u32>(0.125f);
  desc.textures[1].stageStates[TSS_CONSTANT] = 0x7f102040u;
  desc.textures[2].stageStates[TSS_BUMPENVMAT00] = std::bit_cast<u32>(0.25f);
  desc.textures[2].stageStates[TSS_BUMPENVMAT01] = std::bit_cast<u32>(0.5f);
  desc.textures[2].stageStates[TSS_BUMPENVMAT10] = std::bit_cast<u32>(0.75f);
  desc.textures[2].stageStates[TSS_BUMPENVMAT11] = std::bit_cast<u32>(1.0f);
  desc.textures[2].stageStates[TSS_BUMPENVLSCALE] = std::bit_cast<u32>(1.5f);
  desc.textures[2].stageStates[TSS_BUMPENVLOFFSET] = std::bit_cast<u32>(0.125f);

  const auto hot = makeFlatDrawStateRecord(desc);
  const auto shaderLayout = makeDrawShaderLayoutContext(desc);
  const auto uniformPayload = makeDrawUniformPayload(desc);
  const FlatDrawStateView view{
      .hot = &hot, .shaderLayout = &shaderLayout, .uniforms = &uniformPayload};
  const auto ffpVs = dxmt9::state::buildFfpVsConsts(view);
  const auto ffpPs = dxmt9::state::buildFfpPsConsts(view);
  const auto volatileConsts = dxmt9::state::buildDrawVolatile(
      /*vertexBaseIndex=*/0, hot.streamOffsets[0], hot.streamStrides[0]);

  checkNear(ffpVs.halfPixelFixup[0], 1.0f / 320.0f, 1.0e-6f, "half-pixel X fixup");
  checkNear(ffpVs.halfPixelFixup[1], 1.0f / 240.0f, 1.0e-6f, "half-pixel Y fixup");
  checkEq(ffpVs.viewportOrigin[0], 12.0f, "viewport origin X copied");
  checkEq(ffpVs.viewportOrigin[1], 34.0f, "viewport origin Y copied");
  checkEq(ffpVs.viewportSize[0], 320.0f, "viewport width copied");
  checkEq(ffpVs.viewportSize[1], 240.0f, "viewport height copied");

  checkNear(ffpPs.textureFactor[0], 0x40 / 255.0f, 1.0e-6f, "texture factor red extracted");
  checkNear(ffpPs.textureFactor[1], 0x20 / 255.0f, 1.0e-6f, "texture factor green extracted");
  checkNear(ffpPs.textureFactor[2], 0x10 / 255.0f, 1.0e-6f, "texture factor blue extracted");
  checkNear(ffpPs.textureFactor[3], 0x80 / 255.0f, 1.0e-6f, "texture factor alpha extracted");
  checkNear(ffpPs.stageConstants[1][0], 0x10 / 255.0f, 1.0e-6f, "stage constant red extracted");
  checkNear(ffpPs.stageConstants[1][1], 0x20 / 255.0f, 1.0e-6f, "stage constant green extracted");
  checkNear(ffpPs.stageConstants[1][2], 0x40 / 255.0f, 1.0e-6f, "stage constant blue extracted");
  checkNear(ffpPs.stageConstants[1][3], 0x7f / 255.0f, 1.0e-6f, "stage constant alpha extracted");
  checkEq(ffpPs.stageConstants[0][0], 0.0f, "unset stage constant defaults to transparent black");
  checkNear(ffpPs.alphaRef, 128.0f / 255.0f, 1.0e-6f, "alpha ref normalized");
  checkEq(ffpPs.alphaTestEnable, 1u, "alpha test enable reflected");
  checkEq(ffpPs.alphaTestFunc, static_cast<u32>(CompareFunc::GreaterEqual), "alpha test func reflected");
  checkEq(ffpPs.fogMode, static_cast<u32>(FogMode::Exp2), "fog mode reflected");
  checkNear(ffpPs.fogColor[0], 0x10 / 255.0f, 1.0e-6f, "fog color red extracted");
  checkNear(ffpPs.fogColor[1], 0x20 / 255.0f, 1.0e-6f, "fog color green extracted");
  checkNear(ffpPs.fogColor[2], 0x40 / 255.0f, 1.0e-6f, "fog color blue extracted");
  checkNear(ffpPs.fogColor[3], 0x7f / 255.0f, 1.0e-6f, "fog color alpha extracted");
  checkEq(ffpPs.fogStart, 2.5f, "fog start bit-cast from render state");
  checkEq(ffpPs.fogEnd, 9.75f, "fog end bit-cast from render state");
  checkEq(ffpPs.fogDensity, 0.125f, "fog density bit-cast from render state");
  checkEq(ffpPs.bumpEnvMat[2][0], 0.25f, "bump env mat00 copied");
  checkEq(ffpPs.bumpEnvMat[2][1], 0.5f, "bump env mat01 copied");
  checkEq(ffpPs.bumpEnvMat[2][2], 0.75f, "bump env mat10 copied");
  checkEq(ffpPs.bumpEnvMat[2][3], 1.0f, "bump env mat11 copied");
  checkEq(ffpPs.bumpEnvLum[2][0], 1.5f, "bump env luminance scale copied");
  checkEq(ffpPs.bumpEnvLum[2][1], 0.125f, "bump env luminance offset copied");
  checkEq(volatileConsts.vertexStreamOffset, 64u, "stream zero offset copied");
  checkEq(volatileConsts.vertexStreamStride, 28u, "stream zero stride copied");
  checkEq(ffpVs.clipPlaneMask, 0x15u, "clip plane mask copied");
}

void testProgrammableVsExtraStreamBindingPlan() {
  constexpr u32 kD3DDeclTypeFloat2 = 1u;
  constexpr u32 kD3DDeclTypeFloat3 = 2u;
  constexpr u32 kD3DDeclTypeD3DColor = 4u;
  constexpr u32 kD3DDeclMethodDefault = 0u;
  constexpr u32 kD3DDeclUsagePosition = 0u;
  constexpr u32 kD3DDeclUsageTexcoord = 5u;
  constexpr u32 kD3DDeclUsageColor = 10u;

  DrawDesc desc{};
  desc.vertexDecl.elements = {
      VertexElement{0, 0, kD3DDeclTypeFloat3, kD3DDeclMethodDefault, kD3DDeclUsagePosition, 0},
      VertexElement{1, 12, kD3DDeclTypeFloat2, kD3DDeclMethodDefault, kD3DDeclUsageTexcoord, 0},
      VertexElement{2, 12, kD3DDeclTypeD3DColor, kD3DDeclMethodDefault, kD3DDeclUsageColor, 0},
  };
  desc.vertexDecl.streams[1].offset = 128u;
  desc.vertexDecl.streams[1].stride = 20u;
  desc.vertexDecl.streams[2].offset = 9u;
  const auto hot = makeFlatDrawStateRecord(desc);

  dxmt9::encoders::ParamView nonIndexed{
      .primitiveType = PrimitiveType::TriangleList,
      .primitiveCount = 1u,
      .startVertex = 3u,
      .baseVertexIndex = 0,
      .startIndex = 0u,
      .indexType = IndexType::UInt16,
      .indexed = false,
  };
  auto bindings =
      dxmt9::encoders::makeProgrammableVsExtraStreamBindings(desc.vertexDecl, hot, nonIndexed);
  checkEq(bindings.size(), std::size_t{2},
          "programmable VS binding plan includes each nonzero declared stream");
  checkEq(bindings[0].stream, 1u, "first extra stream is stream1");
  checkEq(bindings[0].metalSlot, 6u, "stream1 binds to the generated stream1 slot");
  checkEq(bindings[0].stride, 20u, "stream1 binding plan uses declared stride");
  checkEq(bindings[0].offset, 188u,
          "non-indexed stream1 binding folds startVertex into the Metal buffer offset");
  checkEq(bindings[1].stream, 2u, "second extra stream is stream2");
  checkEq(bindings[1].metalSlot, 7u, "stream2 binds to the generated stream2 slot");
  checkEq(bindings[1].stride, 16u, "stream2 binding plan computes stride from declaration data");
  checkEq(bindings[1].offset, 57u,
          "non-indexed stream2 binding uses computed stride at the boundary");

  auto indexed = nonIndexed;
  indexed.indexed = true;
  bindings =
      dxmt9::encoders::makeProgrammableVsExtraStreamBindings(desc.vertexDecl, hot, indexed);
  checkEq(bindings[0].offset, 128u,
          "indexed stream1 binding leaves baseVertexIndex for DrawVolatile/MSL");
  checkEq(bindings[1].offset, 9u,
          "indexed stream2 binding leaves baseVertexIndex for DrawVolatile/MSL");

  const std::array<u8, 4> userBytes{1u, 2u, 3u, 4u};
  nonIndexed.userVertexData = std::span<const u8>(userBytes);
  bindings =
      dxmt9::encoders::makeProgrammableVsExtraStreamBindings(desc.vertexDecl, hot, nonIndexed);
  check(bindings.empty(), "UP draws do not plan direct extra stream buffer binds");
}

void testDepthStencilKeyReflectsDepthAndStencilState() {
  DrawDesc desc{};
  desc.rs.values[RS_Z_ENABLE] = 1u;
  desc.rs.values[RS_Z_WRITE_ENABLE] = 1u;
  desc.rs.values[RS_Z_FUNC] = static_cast<u32>(CompareFunc::LessEqual);
  desc.rs.values[RS_STENCIL_ENABLE] = 1u;
  desc.rs.values[RS_STENCIL_FUNC] = static_cast<u32>(CompareFunc::Greater);
  desc.rs.values[RS_STENCIL_FAIL] = static_cast<u32>(StencilOp::Replace);
  desc.rs.values[RS_STENCIL_ZFAIL] = static_cast<u32>(StencilOp::IncrSat);
  desc.rs.values[RS_STENCIL_PASS] = static_cast<u32>(StencilOp::DecrSat);
  desc.rs.values[RS_STENCIL_MASK] = 0x3cu;
  desc.rs.values[RS_STENCIL_WRITEMASK] = 0xc3u;
  desc.rs.values[RS_STENCIL_CCW_FUNC] = static_cast<u32>(CompareFunc::NotEqual);
  desc.rs.values[RS_STENCIL_CCW_FAIL] = static_cast<u32>(StencilOp::Zero);
  desc.rs.values[RS_STENCIL_CCW_ZFAIL] = static_cast<u32>(StencilOp::Invert);
  desc.rs.values[RS_STENCIL_CCW_PASS] = static_cast<u32>(StencilOp::Incr);
  // CCW ops are only honored on the back face when D3DRS_TWOSIDEDSTENCILMODE is
  // enabled (gap_d3d9 B.10#7); without it, the back face mirrors the front.
  desc.rs.values[RS_TWO_SIDED_STENCIL_MODE] = 1u;

  const auto hot = makeFlatDrawStateRecord(desc);
  const auto key =
      dxmt9::state::makeDepthStencilKey(FlatDrawStateView{.hot = &hot});

  check(key.depthEnable, "depth enabled reflected");
  check(key.depthWrite, "depth write reflected when depth is enabled");
  checkEq(key.depthFunc, static_cast<u32>(CompareFunc::LessEqual), "depth func reflected");
  check(key.front.enabled, "front stencil enabled reflected");
  check(key.back.enabled, "back stencil enabled follows front enable");
  checkEq(key.front.compareFunction, static_cast<u32>(CompareFunc::Greater), "front stencil func reflected");
  checkEq(key.front.failureOperation, static_cast<u32>(StencilOp::Replace), "front stencil fail op reflected");
  checkEq(key.front.depthFailureOperation, static_cast<u32>(StencilOp::IncrSat), "front stencil zfail op reflected");
  checkEq(key.front.passOperation, static_cast<u32>(StencilOp::DecrSat), "front stencil pass op reflected");
  checkEq(key.front.readMask, 0x3cu, "front stencil read mask reflected");
  checkEq(key.front.writeMask, 0xc3u, "front stencil write mask reflected");
  checkEq(key.back.compareFunction, static_cast<u32>(CompareFunc::NotEqual), "CCW stencil func reflected");
  checkEq(key.back.failureOperation, static_cast<u32>(StencilOp::Zero), "CCW stencil fail op reflected");
  checkEq(key.back.depthFailureOperation, static_cast<u32>(StencilOp::Invert), "CCW stencil zfail op reflected");
  checkEq(key.back.passOperation, static_cast<u32>(StencilOp::Incr), "CCW stencil pass op reflected");
  checkEq(key.back.readMask, 0x3cu, "CCW read mask uses D3D9 shared stencil mask state");
  checkEq(key.back.writeMask, 0xc3u, "CCW write mask uses D3D9 shared stencil write-mask state");
}

void testDepthStencilKeyDefaultsAndCcwFallback() {
  DrawDesc desc{};
  desc.rs.values[RS_Z_ENABLE] = 0u;
  desc.rs.values[RS_Z_WRITE_ENABLE] = 1u;
  desc.rs.values[RS_Z_FUNC] = static_cast<u32>(CompareFunc::Less);
  desc.rs.values[RS_STENCIL_FUNC] = static_cast<u32>(CompareFunc::Equal);
  desc.rs.values[RS_STENCIL_FAIL] = static_cast<u32>(StencilOp::Replace);
  desc.rs.values[RS_STENCIL_ZFAIL] = static_cast<u32>(StencilOp::Incr);
  desc.rs.values[RS_STENCIL_PASS] = static_cast<u32>(StencilOp::Decr);
  desc.rs.values[RS_STENCIL_MASK] = 0x55u;
  desc.rs.values[RS_STENCIL_WRITEMASK] = 0xaau;

  const auto hot = makeFlatDrawStateRecord(desc);
  const auto key = dxmt9::state::makeDepthStencilKey(FlatDrawStateView{.hot = &hot});

  check(!key.depthEnable, "disabled Z state disables depth");
  check(!key.depthWrite, "disabled Z state suppresses depth writes");
  checkEq(key.depthFunc, static_cast<u32>(CompareFunc::Always), "disabled Z state defaults depth func to always");
  check(!key.front.enabled, "missing stencil enable defaults front stencil off");
  check(!key.back.enabled, "missing stencil enable defaults back stencil off");
  checkEq(key.back.compareFunction, key.front.compareFunction, "missing CCW func falls back to front");
  checkEq(key.back.failureOperation, key.front.failureOperation, "missing CCW fail op falls back to front");
  checkEq(key.back.depthFailureOperation, key.front.depthFailureOperation, "missing CCW zfail op falls back to front");
  checkEq(key.back.passOperation, key.front.passOperation, "missing CCW pass op falls back to front");
  checkEq(key.back.readMask, key.front.readMask, "missing CCW read mask falls back to front");
  checkEq(key.back.writeMask, key.front.writeMask, "missing CCW write mask falls back to front");

  DrawDesc defaults{};
  const auto defaultHot = makeFlatDrawStateRecord(defaults);
  const auto defaultKey = dxmt9::state::makeDepthStencilKey(FlatDrawStateView{.hot = &defaultHot});
  check(!defaultKey.depthEnable, "absent Z enable defaults depth off");
  check(!defaultKey.depthWrite, "absent Z write defaults depth writes off");
  checkEq(defaultKey.depthFunc, static_cast<u32>(CompareFunc::Always), "absent Z func defaults to always");
  checkEq(defaultKey.front.compareFunction, static_cast<u32>(CompareFunc::Always), "front stencil func defaults to always");
  checkEq(defaultKey.front.failureOperation, static_cast<u32>(StencilOp::Keep), "front stencil fail op defaults to keep");
  checkEq(defaultKey.front.depthFailureOperation, static_cast<u32>(StencilOp::Keep),
          "front stencil zfail op defaults to keep");
  checkEq(defaultKey.front.passOperation, static_cast<u32>(StencilOp::Keep), "front stencil pass op defaults to keep");
  checkEq(defaultKey.front.readMask, 0xffu, "front stencil read mask defaults to all bits");
  checkEq(defaultKey.front.writeMask, 0xffu, "front stencil write mask defaults to all bits");
  checkEq(defaultKey.back, defaultKey.front, "default back stencil face matches front");
}

void testSamplerInfoDefaultsAreDeterministic() {
  SamplerSnapshot snapshot{};

  const auto info = dxmt9::encoders::makeSamplerInfo(snapshot);

  checkEq(info.min_filter, WMTSamplerMinMagFilterNearest, "missing min filter defaults to nearest");
  checkEq(info.mag_filter, WMTSamplerMinMagFilterNearest, "missing mag filter defaults to nearest");
  checkEq(info.mip_filter, WMTSamplerMipFilterNotMipmapped, "missing mip filter defaults to not-mipmapped");
  checkEq(info.s_address_mode, WMTSamplerAddressModeRepeat, "missing address U defaults to repeat");
  checkEq(info.t_address_mode, WMTSamplerAddressModeRepeat, "missing address V defaults to repeat");
  checkEq(info.r_address_mode, WMTSamplerAddressModeRepeat, "missing address W defaults to repeat");
  checkEq(info.border_color, WMTSamplerBorderColorTransparentBlack, "missing border color defaults transparent black");
  checkEq(info.max_anisotroy, 0u, "missing max anisotropy preserves descriptor zero default");
  checkEq(info.lod_min_clamp, 0.0f, "sampler descriptor min LOD clamp default");
  checkEq(info.lod_max_clamp, 1e9f, "sampler descriptor max LOD clamp allows explicit mip sampling");
  check(info.normalized_coords, "sampler coordinates are normalized");
  check(!info.lod_average, "LOD averaging remains disabled");
  check(!info.support_argument_buffers, "argument-buffer support remains disabled");
}

void testSamplerInfoReflectsSamplerSnapshot() {
  SamplerSnapshot snapshot{};
  snapshot.states[SAMP_MIN_FILTER] = 2u;
  snapshot.states[SAMP_MAG_FILTER] = 1u;
  snapshot.states[SAMP_MIP_FILTER] = 2u;
  snapshot.states[SAMP_ADDRESS_U] = 5u;
  snapshot.states[SAMP_ADDRESS_V] = 2u;
  snapshot.states[SAMP_ADDRESS_W] = 4u;
  snapshot.states[SAMP_BORDER_COLOR] = 0xffffffffu;
  snapshot.states[SAMP_MAX_ANISOTROPY] = 8u;
  snapshot.states[SAMP_MIPMAP_LOD_BIAS] = std::bit_cast<u32>(2.5f);

  const auto info = dxmt9::encoders::makeSamplerInfo(snapshot);
  DrawDesc desc{};
  desc.samplers[0] = snapshot;
  const auto hot = makeFlatDrawStateRecord(desc);
  const auto flatInfo = dxmt9::encoders::makeSamplerInfo(hot.samplerStates[0]);

  checkEq(info.min_filter, WMTSamplerMinMagFilterLinear, "linear min filter maps to Metal linear");
  checkEq(info.mag_filter, WMTSamplerMinMagFilterNearest, "point mag filter maps to Metal nearest");
  checkEq(info.mip_filter, WMTSamplerMipFilterLinear, "linear mip filter maps to Metal linear");
  checkEq(info.s_address_mode, WMTSamplerAddressModeMirrorClampToEdge,
          "address U mirror-once maps to Metal mirror-clamp-to-edge");
  checkEq(info.t_address_mode, WMTSamplerAddressModeMirrorRepeat, "address V mirror maps to Metal mirror-repeat");
  checkEq(info.r_address_mode, WMTSamplerAddressModeClampToBorderColor,
          "address W border maps to Metal clamp-to-border");
  checkEq(info.border_color, WMTSamplerBorderColorOpaqueWhite, "opaque white border color maps deterministically");
  checkEq(info.max_anisotroy, 8u, "max anisotropy maps into WMTSamplerInfo");
  checkEq(info.lod_min_clamp, 0.0f, "LOD bias is clamped out of the sampler descriptor min clamp");
  checkEq(info.lod_max_clamp, 1e9f, "LOD max clamp allows explicit mip sampling");
  check(info.normalized_coords, "snapshot sampler coordinates are normalized");
  checkEq(flatInfo.min_filter, info.min_filter,
          "flat sampler helper matches snapshot min filter mapping");
  checkEq(flatInfo.mag_filter, info.mag_filter,
          "flat sampler helper matches snapshot mag filter mapping");
  checkEq(flatInfo.mip_filter, info.mip_filter,
          "flat sampler helper matches snapshot mip filter mapping");
  checkEq(flatInfo.s_address_mode, info.s_address_mode,
          "flat sampler helper matches snapshot address U mapping");
  checkEq(flatInfo.r_address_mode, info.r_address_mode,
          "flat sampler helper matches snapshot address W mapping");
  checkEq(flatInfo.border_color, info.border_color,
          "flat sampler helper matches snapshot border color mapping");

  const auto lodInfo = dxmt9::encoders::makeSamplerInfo(hot.samplerStates[0], 2.0f);
  checkEq(lodInfo.lod_min_clamp, 2.0f,
          "texture SetLOD/maxmip feeds sampler min LOD clamp");
  checkEq(lodInfo.lod_max_clamp, 1e9f,
          "texture SetLOD keeps explicit LOD sampling upper clamp open");
}

void testSamplerInfoBorderColorFallbacks() {
  SamplerSnapshot transparent{};
  transparent.states[SAMP_ADDRESS_U] = 4u;
  transparent.states[SAMP_BORDER_COLOR] = 0x00123456u;
  const auto transparentInfo = dxmt9::encoders::makeSamplerInfo(transparent);
  checkEq(transparentInfo.border_color,
          WMTSamplerBorderColorTransparentBlack,
          "border colors with zero alpha map to transparent black");

  SamplerSnapshot opaque{};
  opaque.states[SAMP_ADDRESS_V] = 4u;
  opaque.states[SAMP_BORDER_COLOR] = 0x7f123456u;
  const auto opaqueInfo = dxmt9::encoders::makeSamplerInfo(opaque);
  checkEq(opaqueInfo.border_color,
          WMTSamplerBorderColorOpaqueBlack,
          "unsupported non-transparent border colors fall back to opaque black");
}

}  // namespace

int main() {
  try {
    testBuildPerStageConstsCopiesShaderConstants();
    testBuildFfpAndVolatileViewportAndRenderStateValues();
    testProgrammableVsExtraStreamBindingPlan();
    testDepthStencilKeyReflectsDepthAndStencilState();
    testDepthStencilKeyDefaultsAndCcwFallback();
    testSamplerInfoDefaultsAreDeterministic();
    testSamplerInfoReflectsSamplerSnapshot();
    testSamplerInfoBorderColorFallbacks();
  } catch (const TestFailure& failure) {
    std::cerr << "backend_key_descriptor_spec failed: " << failure.what() << '\n';
    return 1;
  } catch (const std::exception& ex) {
    std::cerr << "backend_key_descriptor_spec unexpected exception: " << ex.what() << '\n';
    return 1;
  }

  std::cout << "backend_key_descriptor_spec passed\n";
  return 0;
}
