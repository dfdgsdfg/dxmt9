// Byte-equal determinism + sensitivity regression guard for the MSL
// shader-source generators that feed the pipeline / shader-source cache.
//
// `makeDrawShaderSource()`, `makeTranslatedVertexSource()`,
// `makeTranslatedFragmentSource()`, `makeFfpVertexSource()`, and
// `makeFfpPixelSource()` are all documented as "same ShaderSourceContext
// in -> byte-identical MSL string out". The FFP variant-key determinism
// guard (`tests/native/backend/ffp_key_determinism_spec.cpp`) covers
// the upstream key normalization but stops short of the source-text
// boundary. Without a byte-equal guard at this layer, micro-drift in
// the emitter (whitespace, literal formatting, loop-iteration order
// inside a `std::unordered_set`, etc.) silently fragments the shader
// archive across processes: every fresh string hashes to a new variant
// and the PSO cache thrashes without an observable regression.
//
// The spec is value-only: it calls each generator N times on the same
// ShaderSourceContext and asserts `==` on the returned `std::string`.
// It also flips one bit on the context per shader class (`argbufHybridMode`
// on translator + FFP paths, `clipPlaneMask` / `sampleCount` /
// `textures[0]` on the built-in fallbacks, `key.hash` on the FFP paths)
// and asserts the emitted text changes — so the determinism check cannot
// be silently satisfied by an emitter that has degenerated into a constant
// string.
//
// Test-environment invariants:
//   - DXMT9_TRIM_UNUSED_VARYINGS is observed at first call inside the
//     generator and then cached process-wide. It is left unset; its
//     effect on the emitted text is workload-specific and out of scope
//     here.
//   - No Metal device is created.
//
// See: `src/dxmt9/dxmt9_shader_sources.hpp`,
// `src/dxmt9/dxmt9_draw_shader.hpp`, `src/dxmt9/dxmt9_ffp_shaders.hpp`,
// `src/dxmt9/dxmt9_shader_translator.hpp`,
// `tests/native/backend/argbuf_hybrid_msl_spec.cpp` (fixture pattern),
// `tests/native/backend/ffp_key_determinism_spec.cpp` (key-side guard).

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
#include "../../../src/dxmt9/dxmt9_draw_shader.hpp"
#include "../../../src/dxmt9/dxmt9_ffp_shaders.hpp"
#include "../../../src/dxmt9/dxmt9_shader_sources.hpp"
#include "../../../src/dxmt9/dxmt9_shader_translator.hpp"

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

// 100 is enough to surface nondeterminism that lives inside the inner
// loops (std::unordered_* iteration order, transient string allocator
// state, address-of-static) without making the spec slow.
constexpr int kRepeatN = 100;

dxmt9::drawshader::ShaderSourceContext makeContext(const DrawDesc& desc,
                                                    bool argbufHybridMode) {
  auto context = dxmt9::drawshader::makeShaderSourceContext(desc);
  context.argbufHybridMode = argbufHybridMode;
  return context;
}

// Run a generator N times and assert byte-equal across all returns.
template <typename Generator>
std::string assertByteStable(Generator&& generator, std::string_view label) {
  const std::string baseline = generator();
  check(!baseline.empty(), std::string(label) + ": baseline source is empty");
  for (int i = 0; i < kRepeatN; ++i) {
    const std::string next = generator();
    if (next != baseline) {
      std::ostringstream out;
      out << label << ": MSL source drifted on iteration " << i
          << " (baseline " << baseline.size() << " bytes, next "
          << next.size() << " bytes)";
      fail(out.str());
    }
  }
  return baseline;
}

void assertDiffers(const std::string& a, const std::string& b,
                   std::string_view label) {
  if (a == b) {
    std::ostringstream out;
    out << label << ": expected emitted MSL to change but it was identical ("
        << a.size() << " bytes)";
    fail(out.str());
  }
}

// ---------------------------------------------------------------------
// Representative shader fixtures.

// vs_3_0: dcl_position o0; mov o0, c0.
std::vector<u32> makeVs30Bytecode() {
  return {
      0xfffe0300u,
      0x0200001fu, 0x80000000u, 0xc00f0000u,  // dcl_position o0
      0x02000001u, 0xc00f0000u, 0xa0e40000u,  // mov o0, c0
      0x0000ffffu,
  };
}

std::vector<u32> makeVs20Bytecode() {
  auto words = makeVs30Bytecode();
  words.front() = 0xfffe0200u;
  return words;
}

// vs_2_0: dcl_position o0; dst r0, r1.xyzw, r2.xyzw; mov o0, r0.
// Exercises the new DST (opcode 17) lowering through the MSL emitter so
// the determinism guard covers it; before P0-1 this would have tripped
// invalid_opcode and the translator would have produced an empty module.
std::vector<u32> makeVs20DstBytecode() {
  return {
      0xfffe0200u,
      // dcl_position o0
      0x0200001fu, 0x80000000u, 0xc00f0000u,
      // dst r0, r1, r2 — opcode 17, op-count 3, dst TEMP r0 mask=0xf,
      // src TEMP r1 swizzle=0xe4, src TEMP r2 swizzle=0xe4.
      0x03000011u, 0x800f0000u, 0x80e40001u, 0x80e40002u,
      // mov o0, r0
      0x02000001u, 0xc00f0000u, 0x80e40000u,
      0x0000ffffu,
  };
}

// ps_2_0: dcl t0; dcl_2d s0; texld r0, t0, s0; mov oC0, r0.
const std::array<u32, 15> kPs20Bytecode = {
    0xffff0200u,
    0x0200001fu, 0x90000000u, 0xb0030000u,
    0x0200001fu, 0x90000000u, 0xa00f0800u,
    0x03000042u, 0x800f0000u, 0xb0e40000u, 0xa0e40800u,
    0x02000001u, 0x800f0800u, 0x80e40000u,
    0x0000ffffu,
};

std::array<u32, 15> makePs30Bytecode() {
  auto out = kPs20Bytecode;
  out[0] = 0xffff0300u;
  return out;
}

ShaderRef makeBytecodeShaderRef(std::span<const u32> words) {
  ShaderRef shader{};
  shader.kind = ShaderRef::Kind::Bytecode;
  const auto* base = reinterpret_cast<const u8*>(words.data());
  shader.bytecode.bytes.assign(base, base + words.size_bytes());
  shader.bytecode.hash =
      hashBytes(std::as_bytes(std::span<const u32>(words.data(), words.size())));
  return shader;
}

// Lighting + clip-plane + vertex blend + texcoord transform set.
FfpVertexKey makeRepresentativeFfpVertexKey() {
  FfpVertexKey key{};
  key.lightingEnabled = true;
  key.specularEnabled = true;
  key.normalizeNormals = true;
  key.lightEnabled[0] = true;
  key.lightType[0] = static_cast<u32>(LightType::Point);
  key.lightEnabled[1] = true;
  key.lightType[1] = static_cast<u32>(LightType::Directional);
  key.colorMaterialMode = {1u, 2u, 3u, 0u};
  key.fogMode = FogMode::Exp2;
  key.texCoordGen[0] = 4u;
  key.texTransformFlags[0] = 7u;
  key.vertexBlend = 2u;
  key.clipPlaneMask = 0b1011u;
  // The FFP source generator embeds `key.hash` as a textual comment, so
  // a stable value here is part of the byte-equal contract.
  key.hash = 0xdeadbeefcafebabeull;
  return key;
}

FfpVertexKey makeMinimalFfpVertexKey() {
  FfpVertexKey key{};
  key.hash = 0x0123456789abcdefull;
  return key;
}

// Two active stages, alpha test + fog — exercises the textured branch.
FfpPixelKey makeRepresentativeFfpPixelKey() {
  FfpPixelKey key{};
  key.stages[0].colorOp = static_cast<u32>(TextureOp::Modulate);
  key.stages[0].colorArg1 = 2u;
  key.stages[0].colorArg2 = 0u;
  key.stages[0].alphaOp = static_cast<u32>(TextureOp::Modulate);
  key.stages[0].alphaArg1 = 2u;
  key.stages[0].alphaArg2 = 0u;
  key.stages[0].texType = 2u;
  key.stages[1].colorOp = static_cast<u32>(TextureOp::SelectArg1);
  key.stages[1].alphaOp = static_cast<u32>(TextureOp::SelectArg1);
  key.fogMode = FogMode::Linear;
  key.alphaTestEnable = true;
  key.alphaTestFunc = static_cast<u32>(CompareFunc::GreaterEqual);
  key.hash = 0xfeedfacedeadc0deull;
  return key;
}

FfpPixelKey makeMinimalFfpPixelKey() {
  FfpPixelKey key{};
  key.hash = 0xabad1deafe1edull;
  return key;
}

DrawDesc makeFfpDrawDesc() {
  DrawDesc desc{};
  desc.vertexDecl.fvf =
      dxmt9::ffp::kFvfXyz | dxmt9::ffp::kFvfDiffuse |
      (1u << dxmt9::ffp::kFvfTexCountShift);
  desc.textures[0].handle = Handle{1};
  return desc;
}

DrawDesc makeBytecodeDrawDesc(const ShaderRef& vs, const ShaderRef& ps) {
  DrawDesc desc{};
  desc.vertexShader = vs;
  desc.pixelShader = ps;
  desc.textures[0].handle = Handle{1};
  return desc;
}

// ---------------------------------------------------------------------
// Positive (determinism) cases.

void testTranslatedVertexIsDeterministic() {
  for (const auto& bytecode : {makeVs20Bytecode(), makeVs30Bytecode(),
                               makeVs20DstBytecode()}) {
    const auto vs = makeBytecodeShaderRef(bytecode);
    const auto desc = makeBytecodeDrawDesc(vs, ShaderRef{});
    for (const bool argbuf : {false, true}) {
      const auto context = makeContext(desc, argbuf);
      assertByteStable(
          [&] { return dxmt9::translator::makeTranslatedVertexSource(vs, context); },
          argbuf ? "translated vs (argbuf hybrid)" : "translated vs (stage 1)");
    }
  }
}

void testTranslatedFragmentIsDeterministic() {
  const auto ps20 = makePs30Bytecode();  // mutable buffer for span
  for (const auto* words : {&kPs20Bytecode, &ps20}) {
    const auto ps = makeBytecodeShaderRef(
        std::span<const u32>(words->data(), words->size()));
    const auto desc = makeBytecodeDrawDesc(ShaderRef{}, ps);
    for (const bool argbuf : {false, true}) {
      const auto context = makeContext(desc, argbuf);
      assertByteStable(
          [&] { return dxmt9::translator::makeTranslatedFragmentSource(ps, context); },
          argbuf ? "translated ps (argbuf hybrid)" : "translated ps (stage 1)");
    }
  }
}

void testFfpVertexIsDeterministic() {
  // Populated key (lighting on, two lights, vertex blend, clip planes).
  {
    const auto key = makeRepresentativeFfpVertexKey();
    const auto desc = makeFfpDrawDesc();
    for (const bool argbuf : {false, true}) {
      const auto context = makeContext(desc, argbuf);
      assertByteStable(
          [&] { return dxmt9::ffp::makeFfpVertexSource(key, context); },
          argbuf ? "FFP vs (lighting on, argbuf hybrid)"
                 : "FFP vs (lighting on, stage 1)");
    }
  }
  // Minimal key (lighting off, no lights, no clip planes).
  {
    const auto key = makeMinimalFfpVertexKey();
    const auto desc = makeFfpDrawDesc();
    const auto context = makeContext(desc, /*argbufHybridMode=*/false);
    assertByteStable(
        [&] { return dxmt9::ffp::makeFfpVertexSource(key, context); },
        "FFP vs (lighting off)");
  }
}

void testFfpPixelIsDeterministic() {
  // Textured + alpha test + fog (two active stages).
  {
    const auto key = makeRepresentativeFfpPixelKey();
    const auto desc = makeFfpDrawDesc();
    for (const bool argbuf : {false, true}) {
      const auto context = makeContext(desc, argbuf);
      assertByteStable(
          [&] { return dxmt9::ffp::makeFfpPixelSource(key, context); },
          argbuf ? "FFP ps (textured, argbuf hybrid)"
                 : "FFP ps (textured, stage 1)");
    }
  }
  // Minimal key + no textures — passthrough branch.
  {
    const auto key = makeMinimalFfpPixelKey();
    DrawDesc desc{};
    for (const bool argbuf : {false, true}) {
      const auto context = makeContext(desc, argbuf);
      assertByteStable(
          [&] { return dxmt9::ffp::makeFfpPixelSource(key, context); },
          argbuf ? "FFP ps (no texture, argbuf hybrid)"
                 : "FFP ps (no texture, stage 1)");
    }
  }
}

// Dispatch wrapper: covers Bytecode VS/PS, FixedFunctionVertex,
// FixedFunctionPixel, and the no-shader built-in fallback (which also
// branches on `textures[0]` between generic and textured variants).
void testMakeDrawShaderSourceIsDeterministic() {
  {
    const auto vs = makeBytecodeShaderRef(makeVs30Bytecode());
    const auto context = makeContext(makeBytecodeDrawDesc(vs, ShaderRef{}), false);
    assertByteStable(
        [&] { return dxmt9::drawshader::makeDrawShaderSource(context, true); },
        "makeDrawShaderSource(bytecode VS)");
  }
  {
    const auto ps = makeBytecodeShaderRef(kPs20Bytecode);
    const auto context = makeContext(makeBytecodeDrawDesc(ShaderRef{}, ps), false);
    assertByteStable(
        [&] { return dxmt9::drawshader::makeDrawShaderSource(context, false); },
        "makeDrawShaderSource(bytecode PS)");
  }
  {
    auto desc = makeFfpDrawDesc();
    desc.vertexShader.kind = ShaderRef::Kind::FixedFunctionVertex;
    desc.vertexShader.vertexKey = makeRepresentativeFfpVertexKey();
    const auto context = makeContext(desc, false);
    assertByteStable(
        [&] { return dxmt9::drawshader::makeDrawShaderSource(context, true); },
        "makeDrawShaderSource(FFP VS)");
  }
  {
    DrawDesc desc{};
    desc.textures[0].handle = Handle{1};
    desc.pixelShader.kind = ShaderRef::Kind::FixedFunctionPixel;
    desc.pixelShader.pixelKey = makeRepresentativeFfpPixelKey();
    const auto context = makeContext(desc, false);
    assertByteStable(
        [&] { return dxmt9::drawshader::makeDrawShaderSource(context, false); },
        "makeDrawShaderSource(FFP PS)");
  }
  {
    DrawDesc desc{};
    const auto context = makeContext(desc, false);
    assertByteStable(
        [&] { return dxmt9::drawshader::makeDrawShaderSource(context, true); },
        "makeDrawShaderSource(builtin VS)");
    assertByteStable(
        [&] { return dxmt9::drawshader::makeDrawShaderSource(context, false); },
        "makeDrawShaderSource(builtin PS)");
  }
}

// ---------------------------------------------------------------------
// Negative (sensitivity) cases — flipping a single bit on the context
// MUST change the emitted MSL. A regression that collapses the emitter
// into a constant string would silently satisfy the determinism cases
// above; these guard that bypass.

void testArgbufHybridModeFlipsSource() {
  // Translator vs
  {
    const auto vs = makeBytecodeShaderRef(makeVs30Bytecode());
    const auto desc = makeBytecodeDrawDesc(vs, ShaderRef{});
    const auto s1 = dxmt9::translator::makeTranslatedVertexSource(vs, makeContext(desc, false));
    const auto s2 = dxmt9::translator::makeTranslatedVertexSource(vs, makeContext(desc, true));
    assertDiffers(s1, s2, "translated vs argbufHybridMode flip");
  }
  // Translator ps
  {
    const auto ps = makeBytecodeShaderRef(kPs20Bytecode);
    const auto desc = makeBytecodeDrawDesc(ShaderRef{}, ps);
    const auto s1 = dxmt9::translator::makeTranslatedFragmentSource(ps, makeContext(desc, false));
    const auto s2 = dxmt9::translator::makeTranslatedFragmentSource(ps, makeContext(desc, true));
    assertDiffers(s1, s2, "translated ps argbufHybridMode flip");
  }
  // FFP vs
  {
    const auto key = makeRepresentativeFfpVertexKey();
    const auto desc = makeFfpDrawDesc();
    const auto s1 = dxmt9::ffp::makeFfpVertexSource(key, makeContext(desc, false));
    const auto s2 = dxmt9::ffp::makeFfpVertexSource(key, makeContext(desc, true));
    assertDiffers(s1, s2, "FFP vs argbufHybridMode flip");
  }
  // FFP ps
  {
    const auto key = makeRepresentativeFfpPixelKey();
    const auto desc = makeFfpDrawDesc();
    const auto s1 = dxmt9::ffp::makeFfpPixelSource(key, makeContext(desc, false));
    const auto s2 = dxmt9::ffp::makeFfpPixelSource(key, makeContext(desc, true));
    assertDiffers(s1, s2, "FFP ps argbufHybridMode flip");
  }
}

// `clipPlaneMask` drives per-mask clip-distance writes in the
// translator. Flipping it must change the emitted text.
void testClipPlaneMaskFlipsTranslatedVertex() {
  const auto vs = makeBytecodeShaderRef(makeVs30Bytecode());
  const auto desc = makeBytecodeDrawDesc(vs, ShaderRef{});
  auto contextA = makeContext(desc, false);
  contextA.clipPlaneMask = 0u;
  auto contextB = contextA;
  contextB.clipPlaneMask = 0b1011u;
  assertDiffers(
      dxmt9::translator::makeTranslatedVertexSource(vs, contextA),
      dxmt9::translator::makeTranslatedVertexSource(vs, contextB),
      "translated vs clipPlaneMask change");
}

// `sampleCount` participates in the built-in fallback variant hash.
void testSampleCountFlipsBuiltinFragment() {
  DrawDesc desc{};
  auto contextA = makeContext(desc, false);
  contextA.sampleCount = 1u;
  auto contextB = contextA;
  contextB.sampleCount = 4u;
  assertDiffers(
      dxmt9::drawshader::makeDrawShaderSource(contextA, false),
      dxmt9::drawshader::makeDrawShaderSource(contextB, false),
      "builtin ps sampleCount change");
}

// `textures[0]` selects between generic-fullscreen and textured-fullscreen
// built-in variants. The vertex source must reflect that.
void testTextureBindToggleFlipsBuiltinVertex() {
  DrawDesc desc{};
  auto noTex = makeContext(desc, false);
  noTex.textures[0] = false;
  auto withTex = noTex;
  withTex.textures[0] = true;
  assertDiffers(
      dxmt9::drawshader::makeDrawShaderSource(noTex, true),
      dxmt9::drawshader::makeDrawShaderSource(withTex, true),
      "builtin vs textures[0] toggle");
}

// Both FFP source generators embed `key.hash` as a textual comment.
// Mutating the hash must therefore change the emitted text — guards
// against a regression that drops the hash comment.
void testFfpKeyHashEmbeddedInSource() {
  {
    auto key = makeRepresentativeFfpVertexKey();
    const auto context = makeContext(makeFfpDrawDesc(), false);
    const auto base = dxmt9::ffp::makeFfpVertexSource(key, context);
    auto mutated = key;
    mutated.hash ^= 1ull;
    assertDiffers(
        base, dxmt9::ffp::makeFfpVertexSource(mutated, context),
        "FFP vs key.hash change");
  }
  {
    auto key = makeRepresentativeFfpPixelKey();
    const auto context = makeContext(makeFfpDrawDesc(), false);
    const auto base = dxmt9::ffp::makeFfpPixelSource(key, context);
    auto mutated = key;
    mutated.hash ^= 1ull;
    assertDiffers(
        base, dxmt9::ffp::makeFfpPixelSource(mutated, context),
        "FFP ps key.hash change");
  }
}

}  // namespace

int main() {
  try {
    // Positive: byte-equal across N runs with the same context.
    testTranslatedVertexIsDeterministic();
    testTranslatedFragmentIsDeterministic();
    testFfpVertexIsDeterministic();
    testFfpPixelIsDeterministic();
    testMakeDrawShaderSourceIsDeterministic();
    // Negative: a single-bit context change MUST flip the emitted text.
    testArgbufHybridModeFlipsSource();
    testClipPlaneMaskFlipsTranslatedVertex();
    testSampleCountFlipsBuiltinFragment();
    testTextureBindToggleFlipsBuiltinVertex();
    testFfpKeyHashEmbeddedInSource();
  } catch (const TestFailure& failure) {
    std::cerr << "shader_source_determinism_spec failed: " << failure.what()
              << '\n';
    return 1;
  } catch (const std::exception& ex) {
    std::cerr << "shader_source_determinism_spec unexpected exception: "
              << ex.what() << '\n';
    return 1;
  }
  std::cout << "shader_source_determinism_spec passed\n";
  return 0;
}
