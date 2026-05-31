// R-BACK-12.22..12.26 — Stage 2 argbuf-hybrid MSL routing.
//
// Pure source-level assertions on the MSL produced by the FFP and
// DXBC->MSL emitters. Verifies that ShaderSourceContext::argbufHybridMode
// flips the entry-point binding shape from dedicated slots 0/3 (plus
// per-stage texture/sampler slots) to a single ArgbufLayout argument
// buffer at slot 30, while keeping the vertex stream (slot 1) and
// DrawVolatile (slot 5) on direct binding (design.md §11.4).
//
// Stage 1 (argbufHybridMode=false) keeps emitting buffer(0)/buffer(3)
// declarations so existing PSOs are unaffected. Stage 2
// (argbufHybridMode=true) emits one buffer(30) argbuf parameter and
// re-aliases `vsConsts`/`psConsts`/`ffpVs`/`ffpPs`/`texN`/`sampN` off
// the argbuf so the body code is unchanged.
//
// No Metal device is created; the generators return std::string and the
// tests grep for stable substrings.

#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "dxmt9/core.hpp"
#include "../../../src/dxmt9/dxmt9_argbuf_hybrid.hpp"
#include "../../../src/dxmt9/dxmt9_draw_shader.hpp"
#include "../../../src/dxmt9/dxmt9_ffp_shaders.hpp"
#include "../../../src/dxmt9/dxmt9_shader_sources.hpp"
#include "../../../src/dxmt9/dxmt9_shader_translator.hpp"
#include "../../../src/winemetal/Metal.hpp"

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
void checkEq(A actual, B expected, std::string_view message) {
  if (!(actual == expected)) {
    std::ostringstream out;
    out << message << " (" << actual << " vs " << expected << ")";
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

// Build a ShaderSourceContext from a DrawDesc fixture, optionally
// flipping argbufHybridMode. The two-step sequence (build context, then
// stamp the bit) mirrors what the pipeline cache does at the call site:
// the variant key drives the bit, not the DrawDesc.
dxmt9::drawshader::ShaderSourceContext makeContext(const DrawDesc& desc,
                                                    bool argbufHybridMode) {
  auto context = dxmt9::drawshader::makeShaderSourceContext(desc);
  context.argbufHybridMode = argbufHybridMode;
  return context;
}

// R-BACK-12.22..12.26 (resource-array sub-mode) — build a context with the
// resource-array sub-bit set (always alongside argbufHybridMode, matching
// the pipeline-cache invariant key.argbufResourceArray = argbufHybridMode &&
// argbufResourceArray).
dxmt9::drawshader::ShaderSourceContext makeResourceArrayContext(const DrawDesc& desc) {
  auto context = dxmt9::drawshader::makeShaderSourceContext(desc);
  context.argbufHybridMode = true;
  context.argbufResourceArray = true;
  return context;
}

// ---------------------------------------------------------------------
// FFP vertex emitter

void testFfpVertexStage1Bindings() {
  DrawDesc desc{};
  // FFP-friendly FVF: position only (XYZ).
  desc.vertexDecl.fvf = 0x002u;
  desc.vertexShader.kind = ShaderRef::Kind::FixedFunctionVertex;
  desc.vertexShader.vertexKey = FfpVertexKey{};
  const auto src = dxmt9::ffp::makeFfpVertexSource(*desc.vertexShader.vertexKey,
                                                    makeContext(desc, /*argbufHybridMode=*/false));
  // Stage 1: dedicated slot 0 / slot 3 plus stream0 at slot 1, DrawVolatile at slot 5.
  checkContains(src, "[[buffer(0)]]", "Stage 1 FFP vertex declares VsConsts at slot 0");
  checkContains(src, "[[buffer(3)]]", "Stage 1 FFP vertex declares FfpVsConsts at slot 3");
  checkContains(src, "[[buffer(1)]]", "Stage 1 FFP vertex keeps stream0 at slot 1");
  checkContains(src, "[[buffer(5)]]", "Stage 1 FFP vertex keeps DrawVolatile at slot 5");
  checkNotContains(src, "[[buffer(30)]]",
                   "Stage 1 FFP vertex must not bind argbuf at slot 30");
  checkNotContains(src, "ArgbufLayout",
                   "Stage 1 FFP vertex must not reference ArgbufLayout");
  checkNotContains(src, "abuf.",
                   "Stage 1 FFP vertex must not dereference an argbuf pointer");
}

void testFfpVertexStage2Bindings() {
  DrawDesc desc{};
  desc.vertexDecl.fvf = 0x002u;
  desc.vertexShader.kind = ShaderRef::Kind::FixedFunctionVertex;
  desc.vertexShader.vertexKey = FfpVertexKey{};
  const auto src = dxmt9::ffp::makeFfpVertexSource(*desc.vertexShader.vertexKey,
                                                    makeContext(desc, /*argbufHybridMode=*/true));
  // Stage 2: one buffer(30) argbuf, no buffer(0) or buffer(3); stream0
  // and DrawVolatile stay at their direct slots (design.md §11.4).
  checkContains(src, "[[buffer(30)]]",
                "Stage 2 FFP vertex binds argbuf at slot 30");
  checkContains(src, "ArgbufLayout& abuf",
                "Stage 2 FFP vertex declares ArgbufLayout reference parameter");
  checkContains(src, "abuf.vsConsts",
                "Stage 2 FFP vertex aliases vsConsts off the argbuf");
  checkContains(src, "abuf.ffpVs",
                "Stage 2 FFP vertex aliases ffpVs off the argbuf");
  checkContains(src, "[[buffer(1)]]",
                "Stage 2 FFP vertex keeps stream0 direct at slot 1");
  checkContains(src, "[[buffer(5)]]",
                "Stage 2 FFP vertex keeps DrawVolatile direct at slot 5");
  checkNotContains(src, "[[buffer(0)]]",
                   "Stage 2 FFP vertex must not bind constants at slot 0");
  checkNotContains(src, "[[buffer(3)]]",
                   "Stage 2 FFP vertex must not bind FfpVsConsts at slot 3");
  // Body still references `ffpVs.halfPixelFixup` etc. by name — the
  // alias makes it work without per-call rewrites.
  checkContains(src, "ffpVs.halfPixelFixup",
                "Stage 2 FFP vertex body keeps named field reads");
}

// ---------------------------------------------------------------------
// FFP pixel emitter (portable fragment path)

void testFfpPixelStage1Bindings() {
  DrawDesc desc{};
  desc.pixelShader.kind = ShaderRef::Kind::FixedFunctionPixel;
  desc.pixelShader.pixelKey = FfpPixelKey{};
  const auto src = dxmt9::ffp::makeFfpPixelSource(*desc.pixelShader.pixelKey,
                                                    makeContext(desc, /*argbufHybridMode=*/false));
  checkContains(src, "[[buffer(0)]]", "Stage 1 FFP pixel declares PsConsts at slot 0");
  checkContains(src, "[[buffer(3)]]", "Stage 1 FFP pixel declares FfpPsConsts at slot 3");
  checkNotContains(src, "[[buffer(30)]]",
                   "Stage 1 FFP pixel must not bind argbuf at slot 30");
  checkNotContains(src, "abuf.",
                   "Stage 1 FFP pixel must not dereference an argbuf pointer");
}

void testFfpPixelStage2BindingsTextured() {
  DrawDesc desc{};
  desc.pixelShader.kind = ShaderRef::Kind::FixedFunctionPixel;
  // Enable stage 0 with a basic SelectArg1 / Modulate so the textured
  // branch is exercised. Without an active stage the emitter falls into
  // the no-texture branch which has fewer bindings to verify.
  FfpPixelKey psKey{};
  // colorOp = 4 (Modulate), alphaOp = 4 — anything != Disable (1) marks
  // the stage as active. The selector also requires textures[stage].
  psKey.stages[0].colorOp = 4;
  psKey.stages[0].alphaOp = 4;
  psKey.stages[0].colorArg1 = 2;  // texture
  psKey.stages[0].colorArg2 = 0;  // diffuse
  psKey.stages[0].alphaArg1 = 2;
  psKey.stages[0].alphaArg2 = 0;
  desc.pixelShader.pixelKey = psKey;
  // ShaderSourceContext::textures[0] is sourced from desc.textures[0]
  // when constructed via makeShaderSourceContext(DrawDesc).
  desc.textures[0].handle = Handle{1};

  const auto src = dxmt9::ffp::makeFfpPixelSource(psKey,
                                                    makeContext(desc, /*argbufHybridMode=*/true));
  checkContains(src, "[[buffer(30)]]",
                "Stage 2 FFP pixel binds argbuf at slot 30");
  checkContains(src, "ArgbufLayout& abuf",
                "Stage 2 FFP pixel declares ArgbufLayout reference parameter");
  checkContains(src, "abuf.psConsts",
                "Stage 2 FFP pixel aliases psConsts off the argbuf");
  checkContains(src, "abuf.ffpPs",
                "Stage 2 FFP pixel aliases ffpPs off the argbuf");
  checkContains(src, "texture2d<float> tex0 [[texture(0)]]",
                "Stage 2 FFP pixel keeps tex0 on the direct texture lane");
  checkContains(src, "sampler samp0 [[sampler(0)]]",
                "Stage 2 FFP pixel keeps samp0 on the direct sampler lane");
  checkNotContains(src, "[[buffer(0)]]",
                   "Stage 2 FFP pixel must not bind PsConsts at slot 0");
  checkNotContains(src, "[[buffer(3)]]",
                   "Stage 2 FFP pixel must not bind FfpPsConsts at slot 3");
  // Body still uses `tex0.sample(samp0, ...)` by name.
  checkContains(src, "tex0.sample(samp0",
                "Stage 2 FFP pixel body keeps tex0.sample(samp0, ...) form");
}

void testFfpPixelStage2BindingsNoTexture() {
  // Without active texture stages the emitter takes the no-texture
  // branch. Stage 2 still routes psConsts/ffpPs through the argbuf.
  DrawDesc desc{};
  desc.pixelShader.kind = ShaderRef::Kind::FixedFunctionPixel;
  desc.pixelShader.pixelKey = FfpPixelKey{};
  const auto src = dxmt9::ffp::makeFfpPixelSource(*desc.pixelShader.pixelKey,
                                                    makeContext(desc, /*argbufHybridMode=*/true));
  checkContains(src, "[[buffer(30)]]",
                "Stage 2 FFP pixel (no-texture) binds argbuf at slot 30");
  checkContains(src, "abuf.psConsts",
                "Stage 2 FFP pixel (no-texture) aliases psConsts");
  checkContains(src, "abuf.ffpPs",
                "Stage 2 FFP pixel (no-texture) aliases ffpPs");
  checkNotContains(src, "[[buffer(0)]]",
                   "Stage 2 FFP pixel (no-texture) must not bind slot 0");
}

// ---------------------------------------------------------------------
// FFP tile pixel emitter

void testFfpTileStage1Bindings() {
  DrawDesc desc{};
  desc.pixelShader.kind = ShaderRef::Kind::FixedFunctionPixel;
  desc.pixelShader.pixelKey = FfpPixelKey{};
  const auto src = dxmt9::ffp::makeFfpTilePixelSource(
      *desc.pixelShader.pixelKey,
      makeContext(desc, /*argbufHybridMode=*/false),
      static_cast<u32>(WMTPixelFormatBGRA8Unorm));
  checkContains(src, "constant FfpPsConsts& ffpPs [[buffer(3)]]",
                "Stage 1 tile FFP binds ffpPs at slot 3");
  checkNotContains(src, "[[buffer(30)]]",
                   "Stage 1 tile FFP must not bind argbuf at slot 30");
}

void testFfpTileStage2Bindings() {
  DrawDesc desc{};
  desc.pixelShader.kind = ShaderRef::Kind::FixedFunctionPixel;
  desc.pixelShader.pixelKey = FfpPixelKey{};
  const auto src = dxmt9::ffp::makeFfpTilePixelSource(
      *desc.pixelShader.pixelKey,
      makeContext(desc, /*argbufHybridMode=*/true),
      static_cast<u32>(WMTPixelFormatBGRA8Unorm));
  checkContains(src, "[[buffer(30)]]",
                "Stage 2 tile FFP binds argbuf at slot 30");
  checkContains(src, "abuf.ffpPs",
                "Stage 2 tile FFP aliases ffpPs off the argbuf");
  checkNotContains(src, "[[buffer(3)]]",
                   "Stage 2 tile FFP must not bind ffpPs directly at slot 3");
}

// ---------------------------------------------------------------------
// DXBC -> MSL translator

// Minimal pixel shader (ps_2_0) that samples texture stage 2 and writes
// the result to oC0. Pulled from the existing translator spec so the
// fixture lives outside this file's responsibilities.
const std::array<u32, 15> kPixelSamplerBytecode = {
    0xffff0200u,
    0x0200001fu, 0x80000000u, 0xb0030000u,
    0x0200001fu, 0x90000000u, 0xa00f0802u,
    0x03000042u, 0x800f0000u, 0xb0e40000u, 0xa0e40802u,
    0x02000001u, 0x800f0800u, 0x80e40000u,
    0x0000ffffu,
};

ShaderRef makePixelShaderRef() {
  ShaderRef shader{};
  shader.kind = ShaderRef::Kind::Bytecode;
  const auto* base = reinterpret_cast<const u8*>(kPixelSamplerBytecode.data());
  shader.bytecode.bytes.assign(base, base + kPixelSamplerBytecode.size() * sizeof(u32));
  shader.bytecode.hash = hashBytes(std::as_bytes(std::span<const u32>(
      kPixelSamplerBytecode.data(), kPixelSamplerBytecode.size())));
  return shader;
}

void testTranslatedPixelStage1Bindings() {
  ShaderRef shader = makePixelShaderRef();
  DrawDesc desc{};
  desc.pixelShader = shader;
  desc.textures[2].handle = Handle{3};
  const auto src = dxmt9::translator::makeTranslatedFragmentSource(
      shader, makeContext(desc, /*argbufHybridMode=*/false));
  checkContains(src, "constant PsConsts& psConsts [[buffer(0)]]",
                "Stage 1 translated pixel binds PsConsts at slot 0");
  checkContains(src, "constant FfpPsConsts& ffpPs [[buffer(3)]]",
                "Stage 1 translated pixel binds FfpPsConsts at slot 3");
  checkContains(src, "[[texture(2)]]",
                "Stage 1 translated pixel binds texture stage 2 directly");
  checkContains(src, "[[sampler(2)]]",
                "Stage 1 translated pixel binds sampler stage 2 directly");
  checkNotContains(src, "[[buffer(30)]]",
                   "Stage 1 translated pixel must not bind argbuf at slot 30");
  checkNotContains(src, "abuf.",
                   "Stage 1 translated pixel must not dereference an argbuf pointer");
}

void testTranslatedPixelStage2Bindings() {
  ShaderRef shader = makePixelShaderRef();
  DrawDesc desc{};
  desc.pixelShader = shader;
  desc.textures[2].handle = Handle{3};
  const auto src = dxmt9::translator::makeTranslatedFragmentSource(
      shader, makeContext(desc, /*argbufHybridMode=*/true));
  checkContains(src, "[[buffer(30)]]",
                "Stage 2 translated pixel binds argbuf at slot 30");
  checkContains(src, "ArgbufLayout& abuf",
                "Stage 2 translated pixel declares argbuf reference parameter");
  checkContains(src, "abuf.psConsts",
                "Stage 2 translated pixel aliases psConsts off the argbuf");
  checkContains(src, "abuf.ffpPs",
                "Stage 2 translated pixel aliases ffpPs off the argbuf");
  checkContains(src, "texture2d<float> tex2 [[texture(2)]]",
                "Stage 2 translated pixel keeps tex2 on the direct texture lane");
  checkContains(src, "sampler samp2 [[sampler(2)]]",
                "Stage 2 translated pixel keeps samp2 on the direct sampler lane");
  checkNotContains(src, "[[buffer(0)]]",
                   "Stage 2 translated pixel must not bind slot 0");
  checkNotContains(src, "[[buffer(3)]]",
                   "Stage 2 translated pixel must not bind slot 3");
  // Body still calls tex2.sample(samp2, ...) — only the constant binding
  // changes between Stage 1 and Stage 2; the texture sample form is
  // unchanged.
  checkContains(src, "tex2.sample(samp2",
                "Stage 2 translated pixel body keeps tex2.sample(samp2, ...) form");
}

// Vertex bytecode: vs_3_0 dcl_position o0; mov o0, c0. Pulled from the
// existing translator spec to keep test fixtures DRY.
std::vector<u32> makeVertexBytecode() {
  // Build directly with raw token shape. The decoder accepts any
  // version with valid DCL/MOV/END tokens.
  std::vector<u32> words;
  // version vs_3_0
  words.push_back(0xfffe0300u);
  // dcl_position o0  — semantic token + dst token
  words.push_back(0x0200001fu);
  words.push_back(0x80000000u);  // usage=position, index=0
  words.push_back(0xc00f0000u);  // o0 mask=xyzw
  // mov o0, c0
  words.push_back(0x02000001u);
  words.push_back(0xc00f0000u);  // o0
  words.push_back(0xa0e40000u);  // c0.xyzw
  // end
  words.push_back(0x0000ffffu);
  return words;
}

ShaderRef makeVertexShaderRef() {
  auto words = makeVertexBytecode();
  ShaderRef shader{};
  shader.kind = ShaderRef::Kind::Bytecode;
  const auto* base = reinterpret_cast<const u8*>(words.data());
  shader.bytecode.bytes.assign(base, base + words.size() * sizeof(u32));
  shader.bytecode.hash = hashBytes(std::as_bytes(std::span<const u32>(words.data(), words.size())));
  return shader;
}

void testTranslatedVertexStage1Bindings() {
  ShaderRef shader = makeVertexShaderRef();
  DrawDesc desc{};
  desc.vertexShader = shader;
  const auto src = dxmt9::translator::makeTranslatedVertexSource(
      shader, makeContext(desc, /*argbufHybridMode=*/false));
  checkContains(src, "constant VsConsts& vsConsts [[buffer(0)]]",
                "Stage 1 translated vertex binds VsConsts at slot 0");
  checkContains(src, "constant FfpVsConsts& ffpVs [[buffer(3)]]",
                "Stage 1 translated vertex binds FfpVsConsts at slot 3");
  checkContains(src, "device const uchar* stream0 [[buffer(1)]]",
                "Stage 1 translated vertex keeps stream0 at slot 1");
  checkContains(src, "constant DrawVolatile& drawVolatile [[buffer(5)]]",
                "Stage 1 translated vertex keeps DrawVolatile at slot 5");
  checkNotContains(src, "[[buffer(30)]]",
                   "Stage 1 translated vertex must not bind argbuf at slot 30");
}

void testTranslatedVertexStage2Bindings() {
  ShaderRef shader = makeVertexShaderRef();
  DrawDesc desc{};
  desc.vertexShader = shader;
  const auto src = dxmt9::translator::makeTranslatedVertexSource(
      shader, makeContext(desc, /*argbufHybridMode=*/true));
  checkContains(src, "[[buffer(30)]]",
                "Stage 2 translated vertex binds argbuf at slot 30");
  checkContains(src, "ArgbufLayout& abuf",
                "Stage 2 translated vertex declares argbuf pointer parameter");
  checkContains(src, "abuf.vsConsts",
                "Stage 2 translated vertex aliases vsConsts off the argbuf");
  checkContains(src, "abuf.ffpVs",
                "Stage 2 translated vertex aliases ffpVs off the argbuf");
  // Vertex stream + DrawVolatile remain on direct binding (design.md §11.4).
  checkContains(src, "device const uchar* stream0 [[buffer(1)]]",
                "Stage 2 translated vertex keeps stream0 direct at slot 1");
  checkContains(src, "constant DrawVolatile& drawVolatile [[buffer(5)]]",
                "Stage 2 translated vertex keeps DrawVolatile direct at slot 5");
  checkNotContains(src, "[[buffer(0)]]",
                   "Stage 2 translated vertex must not bind slot 0");
  checkNotContains(src, "[[buffer(3)]]",
                   "Stage 2 translated vertex must not bind slot 3");
}

// ---------------------------------------------------------------------
// Cross-cutting check: the prelude variant is emitted exactly when the
// argbufHybrid bit is set, so downstream MSL has the ArgbufLayout
// struct in scope.

void testArgbufLayoutStructPresentOnlyForStage2() {
  DrawDesc desc{};
  desc.vertexDecl.fvf = 0x002u;
  desc.vertexShader.kind = ShaderRef::Kind::FixedFunctionVertex;
  desc.vertexShader.vertexKey = FfpVertexKey{};
  const auto stage1 = dxmt9::ffp::makeFfpVertexSource(*desc.vertexShader.vertexKey,
                                                       makeContext(desc, /*argbufHybridMode=*/false));
  const auto stage2 = dxmt9::ffp::makeFfpVertexSource(*desc.vertexShader.vertexKey,
                                                       makeContext(desc, /*argbufHybridMode=*/true));
  checkNotContains(stage1, "struct ArgbufLayout",
                   "Stage 1 prelude must not include ArgbufLayout");
  checkContains(stage2, "struct ArgbufLayout",
                "Stage 2 prelude includes ArgbufLayout");
  // Prelude pin: bind slot 30 mirrors DXMT, designs.md §11.2.
  check(dxmt9::shaders::kArgbufHybridBindSlot == 30u,
        "argbuf bind slot constant is 30");
  // R-BACK-12.22..12.26 — argbuf is constants-only; texture/sampler resources
  // stay on the direct render-encoder lane.
  checkNotContains(stage2, "textures2d",
                   "Stage 2 prelude must not declare argbuf texture arrays");
  checkNotContains(stage2, "texturesCube",
                   "Stage 2 prelude must not declare argbuf cube texture arrays");
  checkNotContains(stage2, "textures3d",
                   "Stage 2 prelude must not declare argbuf 3D texture arrays");
  checkNotContains(stage2, "array<sampler",
                   "Stage 2 prelude must not declare an argbuf sampler array");
  check(dxmt9::shaders::kArgbufHybridDescriptorCount ==
            dxmt9::shaders::kArgbufHybridConstantBufferCount,
        "argbuf descriptor count equals the four constant-buffer pointers");
}

void testArgbufDescriptorTablesMirrorPinnedMslIds() {
  const auto constants = dxmt9::argbuf_hybrid::buildArgumentDescriptors();
  checkEq(constants.count(),
          static_cast<std::size_t>(dxmt9::shaders::kArgbufHybridDescriptorCount),
          "constants-only descriptor count matches public constant");
  for (u32 i = 0; i < dxmt9::shaders::kArgbufHybridConstantBufferCount; ++i) {
    const auto& d = constants.entries[i];
    checkEq(d.argumentType, static_cast<u32>(WMTArgumentTypeBuffer),
            "constants-only descriptor is a buffer");
    checkEq(d.index, i, "constants-only descriptor id mirrors MSL [[id]]");
    checkEq(d.arrayLength, 0u, "constants-only descriptor is not an array entry");
    checkEq(d.constantBlockAlignment, 16u,
            "constants-only descriptor keeps the 16-byte constant alignment");
  }

  const auto resources =
      dxmt9::argbuf_hybrid::buildResourceArrayArgumentDescriptors();
  checkEq(resources.count(),
          static_cast<std::size_t>(dxmt9::shaders::kArgbufResourceArrayDescriptorCount),
          "resource-array descriptor count matches public constant");
  for (u32 i = 0; i < dxmt9::shaders::kArgbufHybridConstantBufferCount; ++i) {
    const auto& d = resources.entries[i];
    checkEq(d.argumentType, static_cast<u32>(WMTArgumentTypeBuffer),
            "resource-array cbuf descriptor is a buffer");
    checkEq(d.index, i, "resource-array cbuf id mirrors MSL [[id]]");
  }
  for (u32 stage = 0; stage < dxmt9::shaders::kArgbufResourceArrayStageCount; ++stage) {
    const auto& texture =
        resources.entries[dxmt9::shaders::kArgbufHybridConstantBufferCount + stage];
    checkEq(texture.argumentType, static_cast<u32>(WMTArgumentTypeTexture),
            "resource-array texture descriptor is a texture");
    checkEq(texture.index, dxmt9::shaders::kArgbufResourceArrayTextureBaseId + stage,
            "resource-array texture id mirrors MSL texture [[id]]");
    checkEq(texture.textureType, static_cast<u32>(WMTTextureType2D),
            "resource-array texture descriptor uses texture2d");

    const auto& sampler =
        resources.entries[dxmt9::shaders::kArgbufHybridConstantBufferCount +
                          dxmt9::shaders::kArgbufResourceArrayStageCount + stage];
    checkEq(sampler.argumentType, static_cast<u32>(WMTArgumentTypeSampler),
            "resource-array sampler descriptor is a sampler");
    checkEq(sampler.index, dxmt9::shaders::kArgbufResourceArraySamplerBaseId + stage,
            "resource-array sampler id mirrors MSL sampler [[id]]");
  }
}

// ---------------------------------------------------------------------
// R-BACK-12.22..12.26 (resource-array sub-mode) — texture/sampler via argbuf

void testResourceArrayPreludeShape() {
  // The extended prelude declares the texture/sampler arrays at the pinned
  // [[id]] positions; the constants-only prelude does not.
  const auto prelude =
      dxmt9::shaders::makeShaderPreludeArgbufResourceArray(/*withClipDistances=*/false);
  checkContains(prelude, "struct ArgbufLayout",
                "resource-array prelude declares ArgbufLayout");
  checkContains(prelude, "array<texture2d<float>, 8> textures [[id(4)]]",
                "resource-array prelude declares the texture array at id 4");
  checkContains(prelude, "array<sampler, 8> samplers [[id(12)]]",
                "resource-array prelude declares the sampler array at id 12");
  // The four constant-buffer pointers keep id 0..3.
  for (const char* tag : {"[[id(0)]]", "[[id(1)]]", "[[id(2)]]", "[[id(3)]]"}) {
    checkContains(prelude, tag,
                  "resource-array prelude keeps the four cbuf pointer ids");
  }
  // Constants-only prelude must NOT grow the texture/sampler arrays — the
  // default Stage 2 lane stays byte-identical.
  const auto constantsOnly =
      dxmt9::shaders::makeShaderPreludeArgbufHybrid(/*withClipDistances=*/false);
  checkNotContains(constantsOnly, "array<texture2d<float>",
                   "constants-only prelude must not declare the texture array");
  checkNotContains(constantsOnly, "array<sampler",
                   "constants-only prelude must not declare the sampler array");
}

void testFfpPixelResourceArrayTextured() {
  // FFP textures are always texture2d<float>, so the resource-array lane is
  // always eligible for an active textured FFP stage.
  DrawDesc desc{};
  desc.pixelShader.kind = ShaderRef::Kind::FixedFunctionPixel;
  FfpPixelKey psKey{};
  psKey.stages[0].colorOp = 4;  // Modulate
  psKey.stages[0].alphaOp = 4;
  psKey.stages[0].colorArg1 = 2;  // texture
  psKey.stages[0].colorArg2 = 0;
  psKey.stages[0].alphaArg1 = 2;
  psKey.stages[0].alphaArg2 = 0;
  desc.pixelShader.pixelKey = psKey;
  desc.textures[0].handle = Handle{1};

  const auto src =
      dxmt9::ffp::makeFfpPixelSource(psKey, makeResourceArrayContext(desc));
  // Argbuf still carries the constants.
  checkContains(src, "[[buffer(30)]]",
                "resource-array FFP pixel binds argbuf at slot 30");
  checkContains(src, "abuf.psConsts",
                "resource-array FFP pixel aliases psConsts off the argbuf");
  // Texture/sampler now ride the argbuf arrays — NO direct texture/sampler
  // slot params.
  checkNotContains(src, "[[texture(0)]]",
                   "resource-array FFP pixel drops the direct texture(0) param");
  checkNotContains(src, "[[sampler(0)]]",
                   "resource-array FFP pixel drops the direct sampler(0) param");
  // Alias block rebinds tex0/samp0 off the argbuf arrays.
  checkContains(src, "texture2d<float> tex0 = abuf.textures[0];",
                "resource-array FFP pixel aliases tex0 off abuf.textures");
  checkContains(src, "sampler samp0 = abuf.samplers[0];",
                "resource-array FFP pixel aliases samp0 off abuf.samplers");
  // The body sample form is unchanged so every downstream sample site works.
  checkContains(src, "tex0.sample(samp0",
                "resource-array FFP pixel body keeps tex0.sample(samp0, ...) form");
}

void testTranslatedPixelResourceArray2D() {
  // ps_2_0 sampling stage 2 with a default 2D sampler type — eligible for
  // the resource-array lane.
  ShaderRef shader = makePixelShaderRef();
  DrawDesc desc{};
  desc.pixelShader = shader;
  desc.textures[2].handle = Handle{3};
  // Default textureTypes are TwoD; leave them so the eligibility predicate
  // holds.
  const auto src = dxmt9::translator::makeTranslatedFragmentSource(
      shader, makeResourceArrayContext(desc));
  checkContains(src, "[[buffer(30)]]",
                "resource-array translated pixel binds argbuf at slot 30");
  checkContains(src, "abuf.psConsts",
                "resource-array translated pixel aliases psConsts");
  checkNotContains(src, "[[texture(2)]]",
                   "resource-array translated pixel drops the direct texture(2) param");
  checkNotContains(src, "[[sampler(2)]]",
                   "resource-array translated pixel drops the direct sampler(2) param");
  checkContains(src, "texture2d<float> tex2 = abuf.textures[2];",
                "resource-array translated pixel aliases tex2 off abuf.textures");
  checkContains(src, "sampler samp2 = abuf.samplers[2];",
                "resource-array translated pixel aliases samp2 off abuf.samplers");
  checkContains(src, "tex2.sample(samp2",
                "resource-array translated pixel body keeps tex2.sample(samp2, ...) form");
}

void testResourceArrayDisabledStaysConstantsOnly() {
  // With the resource-array sub-bit OFF (constants-only Stage 2), the
  // textured FFP pixel MSL must be byte-identical to the pre-sub-mode
  // Stage 2 form: direct [[texture(N)]] / [[sampler(N)]] params, no argbuf
  // texture-array alias. This guards the "default OFF is byte-identical"
  // invariant the whole sub-mode is gated on.
  DrawDesc desc{};
  desc.pixelShader.kind = ShaderRef::Kind::FixedFunctionPixel;
  FfpPixelKey psKey{};
  psKey.stages[0].colorOp = 4;
  psKey.stages[0].alphaOp = 4;
  psKey.stages[0].colorArg1 = 2;
  psKey.stages[0].colorArg2 = 0;
  psKey.stages[0].alphaArg1 = 2;
  psKey.stages[0].alphaArg2 = 0;
  desc.pixelShader.pixelKey = psKey;
  desc.textures[0].handle = Handle{1};

  const auto constantsOnly =
      dxmt9::ffp::makeFfpPixelSource(psKey, makeContext(desc, /*argbufHybridMode=*/true));
  checkContains(constantsOnly, "texture2d<float> tex0 [[texture(0)]]",
                "constants-only Stage 2 keeps the direct texture(0) param");
  checkNotContains(constantsOnly, "abuf.textures[0]",
                   "constants-only Stage 2 must not alias texture off the argbuf");
}

}  // namespace

int main() {
  try {
    testResourceArrayPreludeShape();
    testFfpPixelResourceArrayTextured();
    testTranslatedPixelResourceArray2D();
    testResourceArrayDisabledStaysConstantsOnly();
    testFfpVertexStage1Bindings();
    testFfpVertexStage2Bindings();
    testFfpPixelStage1Bindings();
    testFfpPixelStage2BindingsTextured();
    testFfpPixelStage2BindingsNoTexture();
    testFfpTileStage1Bindings();
    testFfpTileStage2Bindings();
    testTranslatedPixelStage1Bindings();
    testTranslatedPixelStage2Bindings();
    testTranslatedVertexStage1Bindings();
    testTranslatedVertexStage2Bindings();
    testArgbufLayoutStructPresentOnlyForStage2();
    testArgbufDescriptorTablesMirrorPinnedMslIds();
  } catch (const TestFailure& failure) {
    std::cerr << "argbuf_hybrid_msl_spec failed: " << failure.what() << '\n';
    return 1;
  } catch (const std::exception& ex) {
    std::cerr << "argbuf_hybrid_msl_spec unexpected exception: " << ex.what() << '\n';
    return 1;
  }

  std::cout << "argbuf_hybrid_msl_spec passed\n";
  return 0;
}
