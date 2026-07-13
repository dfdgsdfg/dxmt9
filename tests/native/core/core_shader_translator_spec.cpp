#include "core_spec_fixtures.hpp"

using namespace dxmt9::core;
using namespace dxmt9::core::fixture;
using namespace dxmt9::core::spec;

namespace {

void testShaderThunk() {
  const auto vertexWords = makeVertexBytecode();
  const auto pixelWords = makePixelBytecode();

  WinemetalShaderCompileRequest vertexRequest{};
  vertexRequest.kind = WinemetalShaderKind_D3DBytecodeVertex;
  vertexRequest.bytecode = vertexWords.data();
  vertexRequest.bytecodeSize = static_cast<dxmt9_u64>(vertexWords.size() * sizeof(u32));
  vertexRequest.bytecodeHash = hashBytes(std::as_bytes(std::span(vertexWords)));
  vertexRequest.clipPlaneMask = 1u;
  vertexRequest.sampleCount = 4u;
  const auto vertexHandle = dxmt9_winemetal_compile_shader(&vertexRequest);
  check(vertexHandle != 0, "vertex shader thunk");
  const auto vertexSource = shaderSourceToString(vertexHandle);
  checkEq(dxmt9_winemetal_shader_source_size(vertexHandle), static_cast<dxmt9_u64>(vertexSource.size()),
          "vertex shader source size");
  checkContains(vertexSource, "vertex VSOut dxmt9_vs", "vertex shader source");
  checkContains(vertexSource, "def c0", "vertex shader decode comment");
  checkContains(vertexSource, "const float4 dxmt9_cdef0 = float4(1.0f, 2.0f, 3.0f, 4.0f)", "vertex shader constant define");
  checkContains(vertexSource, "for (uint i = 0; i < 32u; ++i) { r[i] = float4(0.0f); }",
                "vertex shader temp initialization");
  checkContains(vertexSource, "outPosition = dxmt9_cdef0", "vertex shader mov translation");
  checkContains(vertexSource, "clip_distance", "vertex shader clip distance");
  dxmt9_winemetal_destroy_shader(vertexHandle);

  const auto vertexTexcoordWords = makeVertexTexcoordBytecode();
  WinemetalShaderCompileRequest vertexTexcoordRequest{};
  vertexTexcoordRequest.kind = WinemetalShaderKind_D3DBytecodeVertex;
  vertexTexcoordRequest.bytecode = vertexTexcoordWords.data();
  vertexTexcoordRequest.bytecodeSize = static_cast<dxmt9_u64>(vertexTexcoordWords.size() * sizeof(u32));
  vertexTexcoordRequest.bytecodeHash = hashBytes(std::as_bytes(std::span(vertexTexcoordWords)));
  const auto vertexTexcoordHandle = dxmt9_winemetal_compile_shader(&vertexTexcoordRequest);
  check(vertexTexcoordHandle != 0, "vertex texcoord shader thunk");
  const auto vertexTexcoordSource = shaderSourceToString(vertexTexcoordHandle);
  checkContains(vertexTexcoordSource, "outTexcoord[1] = dxmt9_cdef0", "vertex texcoord oT1 write");
  checkContains(vertexTexcoordSource, "out.texcoord1 = outTexcoord[1]", "vertex texcoord oT1 output");
  dxmt9_winemetal_destroy_shader(vertexTexcoordHandle);

  WinemetalShaderCompileRequest pixelRequest{};
  pixelRequest.kind = WinemetalShaderKind_D3DBytecodePixel;
  pixelRequest.bytecode = pixelWords.data();
  pixelRequest.bytecodeSize = static_cast<dxmt9_u64>(pixelWords.size() * sizeof(u32));
  pixelRequest.bytecodeHash = hashBytes(std::as_bytes(std::span(pixelWords)));
  pixelRequest.alphaTestEnable = 1u;
  pixelRequest.alphaTestFunc = static_cast<u32>(CompareFunc::GreaterEqual);
  pixelRequest.alphaRef = 0.5f;
  const auto pixelHandle = dxmt9_winemetal_compile_shader(&pixelRequest);
  check(pixelHandle != 0, "pixel shader thunk");
  const auto pixelSource = shaderSourceToString(pixelHandle);
  checkContains(pixelSource, "fragment float4 dxmt9_fs", "pixel shader source");
  checkContains(pixelSource, "def c0", "pixel shader decode comment");
  checkContains(pixelSource, "float4 r[1];",
                "pixel shader trims temp array to max-written register");
  checkContains(pixelSource, "for (uint i = 0; i < 1u; ++i) { r[i] = float4(0.0f); }",
                "pixel shader trimmed temp initialization");
  checkContains(pixelSource, "add r0, c0, c0", "pixel shader arithmetic comment");
  checkContains(pixelSource, "r[0] = (dxmt9_cdef0 + dxmt9_cdef0)", "pixel shader arithmetic translation");
  checkContains(pixelSource, "discard_fragment()", "pixel shader alpha test");
  dxmt9_winemetal_destroy_shader(pixelHandle);

  const auto pixelMrtWords = makePixelMrtBytecode();
  WinemetalShaderCompileRequest pixelMrtRequest{};
  pixelMrtRequest.kind = WinemetalShaderKind_D3DBytecodePixel;
  pixelMrtRequest.bytecode = pixelMrtWords.data();
  pixelMrtRequest.bytecodeSize = static_cast<dxmt9_u64>(pixelMrtWords.size() * sizeof(u32));
  pixelMrtRequest.bytecodeHash = hashBytes(std::as_bytes(std::span(pixelMrtWords)));
  const auto pixelMrtHandle = dxmt9_winemetal_compile_shader(&pixelMrtRequest);
  check(pixelMrtHandle != 0, "pixel mrt shader thunk");
  const auto pixelMrtSource = shaderSourceToString(pixelMrtHandle);
  checkContains(pixelMrtSource, "struct FSOut", "pixel mrt output struct");
  checkContains(pixelMrtSource, "float4 color1 [[color(1)]]", "pixel mrt color1 output");
  checkContains(pixelMrtSource, "outColor[1] = dxmt9_cdef1", "pixel mrt color1 write");
  dxmt9_winemetal_destroy_shader(pixelMrtHandle);

  DeviceState state;
  state.reset();
  state.renderStates[RS_LIGHTING] = 1;
  state.renderStates[RS_SPECULAR_ENABLE] = 1;
  state.renderStates[RS_ALPHA_TEST_ENABLE] = 1;
  state.renderStates[RS_ALPHA_FUNC] = static_cast<u32>(CompareFunc::GreaterEqual);
  state.renderStates[RS_ALPHA_REF] = 128;
  state.renderStates[RS_FOG_TABLE_MODE] = static_cast<u32>(FogMode::Exp2);
  state.renderStates[RS_CLIP_PLANE_ENABLE] = 1;
  state.lightEnabled[0] = true;
  state.lights[0].type = LightType::Point;
  state.textureStageStates[0][TSS_COLOR_OP] = static_cast<u32>(TextureOp::SelectArg1);
  state.textureStageStates[0][TSS_ALPHA_OP] = static_cast<u32>(TextureOp::Modulate);
  state.textureStageStates[0][TSS_TEXCOORD_INDEX] = 4;
  state.textureStageStates[0][TSS_TEXTURE_TRANSFORM_FLAGS] = 7;

  const auto vertexKey = makeFfpVertexKey(state);
  WinemetalShaderCompileRequest ffpVertexRequest{};
  ffpVertexRequest.kind = WinemetalShaderKind_FfpVertex;
  ffpVertexRequest.variantKey = &vertexKey;
  ffpVertexRequest.textured = true;
  ffpVertexRequest.clipPlaneMask = vertexKey.clipPlaneMask;
  const auto ffpVertexHandle = dxmt9_winemetal_compile_shader(&ffpVertexRequest);
  check(ffpVertexHandle != 0, "ffp vertex shader thunk");
  const auto ffpVertexSource = shaderSourceToString(ffpVertexHandle);
  checkContains(ffpVertexSource, "ffp vertex hash", "ffp vertex source");
  checkContains(ffpVertexSource, "halfPixelFixup", "ffp vertex half-pixel");
  dxmt9_winemetal_destroy_shader(ffpVertexHandle);

  const auto pixelKey = makeFfpPixelKey(state);
  WinemetalShaderCompileRequest ffpPixelRequest{};
  ffpPixelRequest.kind = WinemetalShaderKind_FfpPixel;
  ffpPixelRequest.variantKey = &pixelKey;
  ffpPixelRequest.textured = true;
  ffpPixelRequest.alphaTestEnable = pixelKey.alphaTestEnable ? 1u : 0u;
  ffpPixelRequest.alphaTestFunc = pixelKey.alphaTestFunc;
  ffpPixelRequest.alphaRef = 0.5f;
  const auto ffpPixelHandle = dxmt9_winemetal_compile_shader(&ffpPixelRequest);
  check(ffpPixelHandle != 0, "ffp pixel shader thunk");
  const auto ffpPixelSource = shaderSourceToString(ffpPixelHandle);
  checkContains(ffpPixelSource, "ffp pixel hash", "ffp pixel source");
  checkContains(ffpPixelSource, "discard_fragment()", "ffp pixel alpha test");
  dxmt9_winemetal_destroy_shader(ffpPixelHandle);

  const auto controlWords = makeControlFlowBytecode();
  WinemetalShaderCompileRequest controlRequest{};
  controlRequest.kind = WinemetalShaderKind_D3DBytecodeVertex;
  controlRequest.bytecode = controlWords.data();
  controlRequest.bytecodeSize = static_cast<dxmt9_u64>(controlWords.size() * sizeof(u32));
  controlRequest.bytecodeHash = hashBytes(std::as_bytes(std::span(controlWords)));
  controlRequest.clipPlaneMask = 1u;
  const auto controlHandle = dxmt9_winemetal_compile_shader(&controlRequest);
  check(controlHandle != 0, "control flow shader thunk");
  const auto controlSource = shaderSourceToString(controlHandle);
  checkContains(controlSource, "vertex VSOut dxmt9_vs", "control flow vertex shader source");
  checkContains(controlSource, "r[3] = float4(sin(", "control flow call body is inlined at call site");
  checkContains(controlSource, "if ((dxmt9_cdef0).x != 0.0f)", "control flow if translation");
  checkContains(controlSource, "} else {", "control flow else translation");
  checkContains(controlSource, "for (int dxmt9_loop_", "control flow loop translation");
  checkContains(controlSource, "for (int dxmt9_rep_", "control flow rep translation");
  checkContains(controlSource, "a0 = int4(round(", "control flow mova translation");
  checkContains(controlSource, "float4(sin(", "control flow sin translation");
  checkContains(controlSource, "cos(", "control flow cos translation");
  checkContains(controlSource, "float4(log2(", "control flow log translation");
  checkContains(controlSource, "float4(exp2(", "control flow exp translation");
  checkContains(controlSource, "pow(", "control flow pow translation");
  checkContains(controlSource, "select(float4(0.0f), float4(1.0f),", "control flow compare translation");
  checkContains(controlSource, "p[0] =", "control flow setp translation");
  checkContains(controlSource, "m4x4", "control flow matrix opcode");
  checkContains(controlSource, "m4x3", "control flow matrix opcode");
  checkContains(controlSource, "m3x2", "control flow matrix opcode");
  dxmt9_winemetal_destroy_shader(controlHandle);
}

void testPixelShaderDepthOutputTranslation() {
  const std::array<u32, 41> bytecode = {
      0xffff0300u, 0x0017fffeu, 0x42415443u, 0x0000001cu, 0x00000023u,
      0xffff0300u, 0x00000000u, 0x00000000u, 0x00000120u, 0x0000001cu,
      0x335f7370u, 0x4d00305fu, 0x6f726369u, 0x74666f73u, 0x29522820u,
      0x44334420u, 0x53203958u, 0x65646168u, 0x6f432072u, 0x6c69706du,
      0x39207265u, 0x2e35312eu, 0x2e393737u, 0x30303030u, 0xababab00u,
      0x05000051u, 0xa00f0000u, 0x3f800000u, 0x00000000u, 0x00000000u,
      0x00000000u, 0x02000001u, 0x802f0800u, 0xa0000000u, 0x02000001u,
      0x802f0801u, 0xa0000000u, 0x02000001u, 0x902f0800u, 0xa0000000u,
      0x0000ffffu,
  };

  ShaderRef shader{};
  shader.kind = ShaderRef::Kind::Bytecode;
  shader.bytecode.bytes.assign(reinterpret_cast<const u8*>(bytecode.data()),
                               reinterpret_cast<const u8*>(bytecode.data() + bytecode.size()));
  shader.bytecode.hash = hashBytes(std::as_bytes(std::span<const u32>(bytecode.data(), bytecode.size())));

  DrawDesc desc{};
  desc.pixelShader = shader;
  const auto source = dxmt9::translator::makeTranslatedFragmentSource(
      shader, dxmt9::drawshader::makeShaderSourceContext(desc));
  check(source.find("float depth [[depth(any)]]") != std::string::npos,
        "pixel shader oDepth emits Metal depth output");
  check(source.find("outDepth =") != std::string::npos,
        "pixel shader oDepth writes translated scalar depth");
  check(source.find("result.depth = outDepth") != std::string::npos,
        "pixel shader oDepth returns depth field");
}

void testPixelShaderSamplerRegisterTranslation() {
  const std::array<u32, 15> bytecode = {
      0xffff0200u,
      0x0200001fu, 0x80000000u, 0xb0030000u,
      0x0200001fu, 0x90000000u, 0xa00f0802u,
      0x03000042u, 0x800f0000u, 0xb0e40000u, 0xa0e40802u,
      0x02000001u, 0x800f0800u, 0x80e40000u,
      0x0000ffffu,
  };

  ShaderRef shader{};
  shader.kind = ShaderRef::Kind::Bytecode;
  shader.bytecode.bytes.assign(reinterpret_cast<const u8*>(bytecode.data()),
                               reinterpret_cast<const u8*>(bytecode.data() + bytecode.size()));
  shader.bytecode.hash = hashBytes(std::as_bytes(std::span<const u32>(bytecode.data(), bytecode.size())));

  DrawDesc desc{};
  desc.pixelShader = shader;
  const auto source = dxmt9::translator::makeTranslatedFragmentSource(
      shader, dxmt9::drawshader::makeShaderSourceContext(desc));
  checkContains(source, "texture2d<float> tex2 [[texture(2)]]", "pixel shader declares sampler texture slot");
  checkContains(source, "sampler samp2 [[sampler(2)]]", "pixel shader declares sampler state slot");
  checkContains(source, "tex2.sample(samp2", "pixel shader samples declared sampler register");
  auto x8Context = dxmt9::drawshader::makeShaderSourceContext(desc);
  x8Context.x8AlphaOneTextureMask = 1u << 2u;
  const auto x8Source = dxmt9::translator::makeTranslatedFragmentSource(shader, x8Context);
  checkContains(x8Source, "dxmt9_x8_alpha_one", "X8 alpha-fill helper is emitted for marked samplers");
  checkContains(x8Source, "dxmt9_x8_alpha_one(tex2.sample(samp2",
                "marked sampler sample is wrapped with alpha-one fixup");
  if (getenvFlag("DXMT_DEBUG_FORCE_PIXEL_V_FLIP")) {
    checkContains(source, "1.0f -", "pixel shader forced V flip source contract");
  } else {
    checkNotContains(source, "1.0f -", "pixel shader keeps D3D texture V coordinates by default");
  }
}

void testPixelShaderInputSemanticTranslation() {
  const std::array<u32, 15> bytecode = {
      0xffff0300u,
      0x0200001fu, 0x80000005u, 0x90230000u,
      0x0200001fu, 0x90000000u, 0xa00f0802u,
      0x03000042u, 0x800f0000u, 0x90e40000u, 0xa0e40802u,
      0x02000001u, 0x800f0800u, 0x80e40000u,
      0x0000ffffu,
  };

  ShaderRef shader{};
  shader.kind = ShaderRef::Kind::Bytecode;
  shader.bytecode.bytes.assign(reinterpret_cast<const u8*>(bytecode.data()),
                               reinterpret_cast<const u8*>(bytecode.data() + bytecode.size()));
  shader.bytecode.hash = hashBytes(std::as_bytes(std::span<const u32>(bytecode.data(), bytecode.size())));

  DrawDesc desc{};
  desc.pixelShader = shader;
  const auto source = dxmt9::translator::makeTranslatedFragmentSource(
      shader, dxmt9::drawshader::makeShaderSourceContext(desc));
  checkContains(source, "in.texcoord0", "ps_3_0 dcl_texcoord input maps to texcoord");
  checkContains(source, "tex2.sample(samp2", "ps_3_0 input semantic sample keeps sampler register");
  if (getenvFlag("DXMT_DEBUG_FORCE_PIXEL_V_FLIP")) {
    checkContains(source, "1.0f -", "ps_3_0 forced V flip source contract");
  } else {
    checkNotContains(source, "1.0f -", "ps_3_0 keeps D3D texture V coordinates by default");
  }
}

void testVertexShaderOutputSemanticTranslation() {
  std::vector<u32> words;
  words.push_back(makeVersionToken(true, 3, 0));
  words.push_back(makeInstructionToken(kD3DSIO_DCL, 2));
  words.push_back(makeDclSemanticToken(kD3DDeclUsagePosition, 0));
  words.push_back(makeDstToken(kD3DSPR_TEXCRDOUT, 0));
  words.push_back(makeInstructionToken(kD3DSIO_DCL, 2));
  words.push_back(makeDclSemanticToken(kD3DDeclUsageTexcoord, 0));
  words.push_back(makeDstToken(kD3DSPR_TEXCRDOUT, 1));
  words.push_back(makeInstructionToken(kD3DSIO_DCL, 2));
  words.push_back(makeDclSemanticToken(kD3DDeclUsageColor, 1));
  words.push_back(makeDstToken(kD3DSPR_TEXCRDOUT, 2));
  words.push_back(makeInstructionToken(kD3DSIO_MOV, 2));
  words.push_back(makeDstToken(kD3DSPR_TEXCRDOUT, 0));
  words.push_back(makeSrcToken(kD3DSPR_CONST, 0));
  words.push_back(makeInstructionToken(kD3DSIO_MOV, 2));
  words.push_back(makeDstToken(kD3DSPR_TEXCRDOUT, 1));
  words.push_back(makeSrcToken(kD3DSPR_CONST, 1));
  words.push_back(makeInstructionToken(kD3DSIO_MOV, 2));
  words.push_back(makeDstToken(kD3DSPR_TEXCRDOUT, 2));
  words.push_back(makeSrcToken(kD3DSPR_CONST, 1));
  words.push_back(kD3DSIO_END);

  ShaderRef shader{};
  shader.kind = ShaderRef::Kind::Bytecode;
  shader.bytecode.bytes.assign(reinterpret_cast<const u8*>(words.data()),
                               reinterpret_cast<const u8*>(words.data() + words.size()));
  shader.bytecode.hash = hashBytes(std::as_bytes(std::span<const u32>(words.data(), words.size())));

  DrawDesc desc{};
  desc.vertexShader = shader;
  const auto source = dxmt9::translator::makeTranslatedVertexSource(
      shader, dxmt9::drawshader::makeShaderSourceContext(desc));
  checkContains(source, "outPosition = vsConsts.vsFloatConst[0]", "vs_3_0 dcl_position o0 maps to Metal position");
  checkContains(source, "outTexcoord[0] = vsConsts.vsFloatConst[1]", "vs_3_0 dcl_texcoord0 o1 maps by semantic index");
  checkContains(source, "outSecondaryColor = vsConsts.vsFloatConst[1]", "vs_3_0 dcl_color1 o2 maps to secondary color");
  check(source.find("outTexcoord[1] = vsConsts.vsFloatConst[1]") == std::string::npos,
        "vs_3_0 output mapping ignores raw o-register index for texcoord semantic");
  if (getenvFlag("DXMT_DEBUG_FLIP_VERTEX_Y")) {
    checkContains(source, "out.position.y = -out.position.y", "vertex shader forced Y flip source contract");
  } else {
    checkNotContains(source, "out.position.y = -out.position.y",
                     "vertex shader keeps D3D clip Y by default");
  }
}

}  // namespace

int main() {
  try {
    if (getenvFlag("DXMT9_CORE_SPEC_SOURCE_CONTRACT_ONLY")) {
      testPixelShaderSamplerRegisterTranslation();
      testPixelShaderInputSemanticTranslation();
      testVertexShaderOutputSemanticTranslation();
      return EXIT_SUCCESS;
    }

    testShaderThunk();
    testPixelShaderDepthOutputTranslation();
    testPixelShaderSamplerRegisterTranslation();
    testPixelShaderInputSemanticTranslation();
    testVertexShaderOutputSemanticTranslation();
  } catch (const TestFailure& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  } catch (const std::exception& error) {
    std::cerr << "unexpected exception: " << error.what() << '\n';
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
