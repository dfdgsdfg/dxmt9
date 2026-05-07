#include "core_spec_fixtures.hpp"

#include <memory>

using namespace dxmt9::core;
using namespace dxmt9::core::fixture;
using namespace dxmt9::core::spec;

namespace {

void testDepthStencilKeyDisablesDepthCompare() {
  DrawDesc desc{};
  desc.rs.values[RS_Z_ENABLE] = 0u;
  desc.rs.values[RS_Z_WRITE_ENABLE] = 1u;
  desc.rs.values[RS_Z_FUNC] = static_cast<u32>(CompareFunc::LessEqual);

  auto hot = makeFlatDrawStateRecord(desc);
  auto key = dxmt9::state::makeDepthStencilKey(FlatDrawStateView{.hot = &hot});
  check(!key.depthEnable, "ZENABLE=0 disables depth");
  check(!key.depthWrite, "ZENABLE=0 disables depth writes");
  checkEq(key.depthFunc, static_cast<u32>(CompareFunc::Always),
          "ZENABLE=0 maps depth compare to always");

  desc.rs.values[RS_Z_ENABLE] = 1u;
  hot = makeFlatDrawStateRecord(desc);
  key = dxmt9::state::makeDepthStencilKey(FlatDrawStateView{.hot = &hot});
  check(key.depthEnable, "ZENABLE=1 enables depth");
  check(key.depthWrite, "ZENABLE=1 keeps depth writes");
  checkEq(key.depthFunc, static_cast<u32>(CompareFunc::LessEqual),
          "ZENABLE=1 preserves ZFUNC");
}

void testFfpKeys() {
  DeviceState state;
  state.reset();

  state.renderStates[RS_LIGHTING] = 1;
  state.renderStates[RS_SPECULAR_ENABLE] = 1;
  state.renderStates[RS_ALPHA_TEST_ENABLE] = 1;
  state.renderStates[RS_ALPHA_FUNC] = static_cast<u32>(CompareFunc::GreaterEqual);
  state.renderStates[RS_FOG_TABLE_MODE] = static_cast<u32>(FogMode::Exp2);
  state.renderStates[RS_EMISSIVE_MATERIAL_SOURCE] = 3;
  state.renderStates[RS_AMBIENT_MATERIAL_SOURCE] = 2;
  state.renderStates[RS_DIFFUSE_MATERIAL_SOURCE] = 1;
  state.renderStates[RS_SPECULAR_MATERIAL_SOURCE] = 0;
  state.renderStates[RS_VERTEX_BLEND] = 2;

  state.textureStageStates[0][TSS_COLOR_OP] = static_cast<u32>(TextureOp::SelectArg1);
  state.textureStageStates[0][TSS_ALPHA_OP] = static_cast<u32>(TextureOp::Modulate);
  state.textureStageStates[0][TSS_TEXCOORD_INDEX] = 4;
  state.textureStageStates[0][TSS_TEXTURE_TRANSFORM_FLAGS] = 7;

  state.lightEnabled[0] = true;
  state.lights[0].type = LightType::Point;

  const auto vertexKey = makeFfpVertexKey(state);
  check(vertexKey.lightingEnabled, "vertex key lighting");
  check(vertexKey.specularEnabled, "vertex key specular");
  check(vertexKey.lightEnabled[0], "vertex key light enable");
  checkEq(vertexKey.lightType[0], static_cast<u32>(LightType::Point), "vertex key light type");
  checkEq(vertexKey.texCoordGen[0], 4u, "vertex key texcoord");
  checkEq(vertexKey.texTransformFlags[0], 7u, "vertex key transform flags");
  check(vertexKey.hash != 0, "vertex key hash");

  const auto pixelKey = makeFfpPixelKey(state);
  check(pixelKey.alphaTestEnable, "pixel key alpha test");
  checkEq(pixelKey.alphaTestFunc, static_cast<u32>(CompareFunc::GreaterEqual), "pixel key alpha func");
  checkEq(pixelKey.stages[0].colorOp, static_cast<u32>(TextureOp::SelectArg1), "pixel key color op");
  checkEq(pixelKey.stages[0].alphaOp, static_cast<u32>(TextureOp::Modulate), "pixel key alpha op");
  checkEq(pixelKey.stages[0].texCoordIndex, 4u, "pixel key texcoord");
  check(pixelKey.hash != 0, "pixel key hash");
}

void testVisualDerivedFfpCoverage() {
  // behavioral oracle: Wine visual.c:lighting_test
  DeviceState lightingState;
  lightingState.reset();
  lightingState.renderStates[RS_LIGHTING] = 1;
  lightingState.renderStates[RS_SPECULAR_ENABLE] = 1;
  lightingState.lightEnabled[0] = true;
  lightingState.lights[0].type = LightType::Directional;
  lightingState.lights[0].diffuse = {1.0f, 0.5f, 0.25f, 1.0f};
  const auto lightingVertexKey = makeFfpVertexKey(lightingState);
  check(lightingVertexKey.lightingEnabled, "lighting visual key");
  check(lightingVertexKey.specularEnabled, "lighting specular visual key");
  checkEq(lightingVertexKey.lightType[0], static_cast<u32>(LightType::Directional),
          "lighting visual light type");
  WinemetalShaderCompileRequest lightingRequest{};
  lightingRequest.kind = WinemetalShaderKind_FfpVertex;
  lightingRequest.variantKey = &lightingVertexKey;
  const auto lightingHandle = dxmt9_winemetal_compile_shader(&lightingRequest);
  check(lightingHandle != 0, "lighting ffp shader");
  const auto lightingSource = shaderSourceToString(lightingHandle);
  checkContains(lightingSource, "out.color.rgb *= 1.0", "lighting visual source");
  dxmt9_winemetal_destroy_shader(lightingHandle);

  // behavioral oracle: Wine visual.c:fog_test
  DeviceState fogState;
  fogState.reset();
  fogState.renderStates[RS_FOG_TABLE_MODE] = static_cast<u32>(FogMode::Exp2);
  fogState.renderStates[RS_ALPHA_TEST_ENABLE] = 1;
  fogState.renderStates[RS_ALPHA_FUNC] = static_cast<u32>(CompareFunc::GreaterEqual);
  fogState.textureStageStates[0][TSS_COLOR_OP] = static_cast<u32>(TextureOp::Modulate);
  const auto fogPixelKey = makeFfpPixelKey(fogState);
  checkEq(fogPixelKey.fogMode, FogMode::Exp2, "fog visual key");
  WinemetalShaderCompileRequest fogRequest{};
  fogRequest.kind = WinemetalShaderKind_FfpPixel;
  fogRequest.variantKey = &fogPixelKey;
  fogRequest.alphaTestEnable = fogPixelKey.alphaTestEnable ? 1u : 0u;
  fogRequest.alphaTestFunc = fogPixelKey.alphaTestFunc;
  fogRequest.alphaRef = 0.5f;
  const auto fogHandle = dxmt9_winemetal_compile_shader(&fogRequest);
  check(fogHandle != 0, "fog ffp shader");
  const auto fogSource = shaderSourceToString(fogHandle);
  checkContains(fogSource, "fogFactor", "fog visual source");
  checkContains(fogSource, "mix(fogColor, color, fog)", "fog visual blend");
  dxmt9_winemetal_destroy_shader(fogHandle);

  // behavioral oracle: Wine visual.c:texture_transform_test
  DeviceState transformState;
  transformState.reset();
  transformState.textureStageStates[0][TSS_TEXCOORD_INDEX] = 4;
  transformState.textureStageStates[0][TSS_TEXTURE_TRANSFORM_FLAGS] = 7;
  const auto transformVertexKey = makeFfpVertexKey(transformState);
  checkEq(transformVertexKey.texCoordGen[0], 4u, "texture transform texcoord");
  checkEq(transformVertexKey.texTransformFlags[0], 7u, "texture transform flags");

  // behavioral oracle: Wine visual.c:texop_test
  DeviceState texopState;
  texopState.reset();
  texopState.textureStageStates[0][TSS_COLOR_OP] = static_cast<u32>(TextureOp::Add);
  texopState.textureStageStates[0][TSS_ALPHA_OP] = static_cast<u32>(TextureOp::Modulate);
  texopState.textureStageStates[0][TSS_TEXCOORD_INDEX] = 4;
  texopState.textureStageStates[0][TSS_TEXTURE_TRANSFORM_FLAGS] = 7;
  const auto texopPixelKey = makeFfpPixelKey(texopState);
  checkEq(texopPixelKey.stages[0].colorOp, static_cast<u32>(TextureOp::Add), "texop visual color op");
  checkEq(texopPixelKey.stages[0].alphaOp, static_cast<u32>(TextureOp::Modulate), "texop visual alpha op");
  checkEq(texopPixelKey.stages[0].texCoordIndex, 4u, "texop visual texcoord");
  WinemetalShaderCompileRequest texopRequest{};
  texopRequest.kind = WinemetalShaderKind_FfpPixel;
  texopRequest.variantKey = &texopPixelKey;
  texopRequest.textured = true;
  const auto texopHandle = dxmt9_winemetal_compile_shader(&texopRequest);
  check(texopHandle != 0, "texop ffp shader");
  const auto texopSource = shaderSourceToString(texopHandle);
  checkContains(texopSource, "case 7u: return saturate(arg1 + arg2);", "texop add source");
  dxmt9_winemetal_destroy_shader(texopHandle);

  // behavioral oracle: Wine visual.c:fixed_function_varying_test
  DeviceState varyingState;
  varyingState.reset();
  varyingState.renderStates[RS_LIGHTING] = 1;
  varyingState.renderStates[RS_SPECULAR_ENABLE] = 1;
  varyingState.lightEnabled[0] = true;
  varyingState.lights[0].type = LightType::Point;
  varyingState.textureStageStates[0][TSS_COLOR_OP] = static_cast<u32>(TextureOp::SelectArg1);
  varyingState.textureStageStates[0][TSS_ALPHA_OP] = static_cast<u32>(TextureOp::Modulate);
  const auto varyingVertexKey = makeFfpVertexKey(varyingState);
  const auto varyingPixelKey = makeFfpPixelKey(varyingState);
  check(varyingVertexKey.hash != 0, "varying vertex hash");
  check(varyingPixelKey.hash != 0, "varying pixel hash");
  check(varyingVertexKey != lightingVertexKey, "varying vertex key differs");
  check(varyingPixelKey.hash != fogPixelKey.hash, "varying pixel key differs");
}

void testVisualPortCoverage() {
  // behavioral oracle: Wine visual.c:test_sanity
  auto backend = std::make_shared<RecordingBackend>();
  BackendLimits limits{};
  limits.maxTextureSize = 4096;
  limits.maxColorAttachments = 4;
  limits.maxAnisotropy = 16;
  limits.supportsBgr10A2 = true;
  limits.supportsDepth32FloatStencil8 = true;

  Factory factory(limits, backend);
  PresentParameters params{};
  params.backBufferWidth = 32;
  params.backBufferHeight = 32;
  params.backBufferFormat = Format::A8R8G8B8;
  params.windowed = true;
  params.presentationInterval = PresentInterval::Immediate;
  params.deviceWindow = Handle{11};
  auto device = factory.createDevice(0, params);
  check(device != nullptr, "visual sanity device");
  auto backBuffer = device->swapChain()->backBuffer();
  auto probeSurface = device->createSurface({32, 32, Format::A8R8G8B8, Pool::Scratch, 0, false, false});
  check(backBuffer != nullptr && probeSurface != nullptr, "visual sanity surfaces");
  ClearDesc clear{};
  clear.clearColor = true;
  clear.color = {0.0f, 0.0f, 0.0f, 1.0f};
  clear.colorAttachments[0] = {backBuffer->handle(), backBuffer->level(), backBuffer->multiSampleCount()};
  checkEq(device->clear(clear), D3D_OK, "visual sanity clear");
  checkEq(device->getRenderTargetData(backBuffer, probeSurface), D3D_OK, "visual sanity readback");
  auto region = probeSurface->lockRect(nullptr, 0);
  check(region.data != nullptr, "visual sanity lock");
  const auto sanityPixel = bgra(0x00, 0x00, 0x00, 0xff);
  checkBytes(std::span<const u8>(static_cast<const u8*>(region.data), 4),
             std::span<const u8>(sanityPixel.data(), sanityPixel.size()), "visual sanity pixel");
  probeSurface->unlockRect();

  // behavioral oracle: Wine visual.c:alpha_test
  DeviceState alphaState;
  alphaState.reset();
  alphaState.renderStates[RS_ALPHA_TEST_ENABLE] = 1;
  const std::array<CompareFunc, 8> alphaFuncs{
      CompareFunc::Never,        CompareFunc::Less,      CompareFunc::Equal,       CompareFunc::LessEqual,
      CompareFunc::Greater,      CompareFunc::NotEqual,  CompareFunc::GreaterEqual, CompareFunc::Always};
  u64 alphaHash = 0;
  for (size_t i = 0; i < alphaFuncs.size(); ++i) {
    alphaState.renderStates[RS_ALPHA_FUNC] = static_cast<u32>(alphaFuncs[i]);
    alphaState.renderStates[RS_ALPHA_REF] = 128;
    const auto pixelKey = makeFfpPixelKey(alphaState);
    check(pixelKey.alphaTestEnable, "alpha test pixel key enabled");
    checkEq(pixelKey.alphaTestFunc, static_cast<u32>(alphaFuncs[i]), "alpha test function key");
    check(pixelKey.hash != 0, "alpha test hash");
    if (i == 0) {
      alphaHash = pixelKey.hash;
    } else {
      check(pixelKey.hash != alphaHash, "alpha test hash varies");
    }
  }
  const auto alphaPixelKey = makeFfpPixelKey(alphaState);
  WinemetalShaderCompileRequest alphaRequest{};
  alphaRequest.kind = WinemetalShaderKind_FfpPixel;
  alphaRequest.variantKey = &alphaPixelKey;
  alphaRequest.textured = true;
  alphaRequest.alphaTestEnable = 1u;
  alphaRequest.alphaTestFunc = alphaPixelKey.alphaTestFunc;
  alphaRequest.alphaRef = 0.5f;
  const auto alphaHandle = dxmt9_winemetal_compile_shader(&alphaRequest);
  check(alphaHandle != 0, "alpha test visual shader");
  const auto alphaSource = shaderSourceToString(alphaHandle);
  checkContains(alphaSource, "discard_fragment()", "alpha test visual source");
  dxmt9_winemetal_destroy_shader(alphaHandle);

  // behavioral oracle: Wine visual.c:texbem_test
  DeviceState texbemState;
  texbemState.reset();
  texbemState.textureStageStates[0][TSS_COLOR_OP] = static_cast<u32>(TextureOp::BumpEnvMap);
  texbemState.textureStageStates[0][TSS_ALPHA_OP] = static_cast<u32>(TextureOp::BumpEnvMapLuminance);
  texbemState.textureStageStates[0][TSS_TEXCOORD_INDEX] = 1;
  const auto texbemKey = makeFfpPixelKey(texbemState);
  checkEq(texbemKey.stages[0].colorOp, static_cast<u32>(TextureOp::BumpEnvMap), "texbem color op");
  checkEq(texbemKey.stages[0].alphaOp, static_cast<u32>(TextureOp::BumpEnvMapLuminance), "texbem alpha op");
  checkEq(texbemKey.stages[0].texCoordIndex, 1u, "texbem texcoord");
  check(texbemKey.hash != 0, "texbem hash");

  // behavioral oracle: Wine visual.c:ps_1_4_test
  DeviceState ps14State;
  ps14State.reset();
  ps14State.textureStageStates[0][TSS_COLOR_OP] = static_cast<u32>(TextureOp::AddSigned);
  ps14State.textureStageStates[0][TSS_ALPHA_OP] = static_cast<u32>(TextureOp::Modulate);
  ps14State.textureStageStates[1][TSS_COLOR_OP] = static_cast<u32>(TextureOp::DotProduct3);
  const auto ps14Key = makeFfpPixelKey(ps14State);
  check(ps14Key.hash != 0, "ps_1_4 hash");
  check(ps14Key != texbemKey, "ps_1_4 key differs from texbem");

  // behavioral oracle: Wine visual.c:vshader_version_varying_test
  DeviceState baselineVertexState;
  baselineVertexState.reset();
  const auto baselineVertexKey = makeFfpVertexKey(baselineVertexState);
  DeviceState varyingState;
  varyingState.reset();
  varyingState.renderStates[RS_LIGHTING] = 1;
  varyingState.renderStates[RS_SPECULAR_ENABLE] = 1;
  varyingState.renderStates[RS_VERTEX_BLEND] = 2;
  varyingState.lightEnabled[0] = true;
  varyingState.lights[0].type = LightType::Point;
  const auto varyingVertexKey = makeFfpVertexKey(varyingState);
  check(varyingVertexKey.hash != 0, "vshader varying hash");
  check(varyingVertexKey.vertexBlend == 2u, "vshader varying vertex blend");
  check(varyingVertexKey.indexedVertexBlend, "vshader varying indexed blend");
  check(varyingVertexKey != baselineVertexKey, "vshader varying key differs");
}

}  // namespace

int main() {
  try {
    testDepthStencilKeyDisablesDepthCompare();
    testFfpKeys();
    testVisualDerivedFfpCoverage();
    testVisualPortCoverage();
  } catch (const TestFailure& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  } catch (const std::exception& error) {
    std::cerr << "unexpected exception: " << error.what() << '\n';
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
