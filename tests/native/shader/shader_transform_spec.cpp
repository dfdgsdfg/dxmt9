#include "dxmt9/core.hpp"
#include "../../../src/dxmt9/dxmt9_d3d9_bytecode.hpp"
#include "../../../src/dxmt9/dxmt9_draw_shader.hpp"
#include "../../../src/dxmt9/dxmt9_ffp_shaders.hpp"
#include "../../../src/dxmt9/dxmt9_shader_decoder.hpp"
#include "../../../src/dxmt9/dxmt9_shader_translator.hpp"

#include <array>
#include <cstdlib>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

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

void checkContains(std::string_view haystack, std::string_view needle, std::string_view message) {
  if (haystack.find(needle) == std::string_view::npos) {
    std::ostringstream out;
    out << message << " (missing '" << needle << "')";
    fail(out.str());
  }
}

void checkNotContains(std::string_view haystack, std::string_view needle, std::string_view message) {
  if (haystack.find(needle) != std::string_view::npos) {
    std::ostringstream out;
    out << message << " (unexpected '" << needle << "')";
    fail(out.str());
  }
}

template <typename Actual, typename Expected>
void checkEqual(const Actual& actual, const Expected& expected, std::string_view message) {
  if (!(actual == expected)) {
    std::ostringstream out;
    out << message << " (expected ";
    if constexpr (std::is_enum_v<Expected>) {
      out << static_cast<u32>(expected);
    } else {
      out << expected;
    }
    out << ", got ";
    if constexpr (std::is_enum_v<Actual>) {
      out << static_cast<u32>(actual);
    } else {
      out << actual;
    }
    out << ")";
    fail(out.str());
  }
}

void checkRegister(dxmt9::d3d9bc::D3DRegisterRef reg,
                   dxmt9::d3d9bc::D3DRegisterKind kind,
                   u32 index,
                   std::string_view message) {
  if (reg.kind != kind || reg.index != index) {
    std::ostringstream out;
    out << message << " (expected kind " << static_cast<u32>(kind) << " index " << index
        << ", got kind " << static_cast<u32>(reg.kind) << " index " << reg.index << ")";
    fail(out.str());
  }
}

template <typename Fn>
void checkThrowsContains(Fn&& fn, std::string_view needle, std::string_view message) {
  try {
    std::forward<Fn>(fn)();
  } catch (const std::runtime_error& error) {
    if (std::string_view(error.what()).find(needle) == std::string_view::npos) {
      std::ostringstream out;
      out << message << " (exception '" << error.what() << "' missing '" << needle << "')";
      fail(out.str());
    }
    return;
  }

  fail(std::string(message) + " (no exception)");
}

class ScopedUnsetEnv {
public:
  explicit ScopedUnsetEnv(const char* name) : name_(name) {
    if (const char* value = std::getenv(name)) {
      previous_ = value;
    }
    unsetenv(name);
  }

  ~ScopedUnsetEnv() {
    if (previous_) {
      setenv(name_, previous_->c_str(), 1);
    } else {
      unsetenv(name_);
    }
  }

  ScopedUnsetEnv(const ScopedUnsetEnv&) = delete;
  ScopedUnsetEnv& operator=(const ScopedUnsetEnv&) = delete;

private:
  const char* name_ = nullptr;
  std::optional<std::string> previous_;
};

class ScopedSetEnv {
public:
  ScopedSetEnv(const char* name, const char* value) : name_(name) {
    if (const char* previous = std::getenv(name)) {
      previous_ = previous;
    }
    setenv(name_, value, 1);
  }

  ~ScopedSetEnv() {
    if (previous_) {
      setenv(name_, previous_->c_str(), 1);
    } else {
      unsetenv(name_);
    }
  }

  ScopedSetEnv(const ScopedSetEnv&) = delete;
  ScopedSetEnv& operator=(const ScopedSetEnv&) = delete;

private:
  const char* name_ = nullptr;
  std::optional<std::string> previous_;
};

u32 encodeRegisterType(u32 regType) {
  return ((regType & 0x7u) << 28) | (((regType >> 3) & 0x3u) << 11);
}

u32 makeVersionToken(bool vertex, u32 major, u32 minor) {
  return ((vertex ? 0xfffeu : 0xffffu) << 16) | ((major & 0xffu) << 8) | (minor & 0xffu);
}

u32 makeInstructionToken(u32 opcode, u32 operandCount) {
  return (opcode & 0xffffu) | ((operandCount & 0xfu) << 24);
}

u32 makePredicatedInstructionToken(u32 opcode, u32 operandCount, u32 controls) {
  return makeInstructionToken(opcode, operandCount) | ((controls & 0xffu) << 16) | (1u << 28);
}

u32 makeControlledInstructionToken(u32 opcode, u32 operandCount, u32 controls) {
  return makeInstructionToken(opcode, operandCount) | ((controls & 0xffu) << 16);
}

u32 makeCommentToken(u32 wordCount) {
  return dxmt9::d3d9bc::kD3DSIO_COMMENT | ((wordCount & 0x7fffu) << 16);
}

u32 makeDstToken(u32 regType, u32 regIndex, u32 writeMask = 0xfu, u32 modifier = 0u) {
  return (1u << 31) | encodeRegisterType(regType) | ((modifier & 0xfu) << 20) | ((writeMask & 0xfu) << 16) |
         (regIndex & 0x7ffu);
}

u32 makeRelativeDstToken(u32 regType, u32 regIndex, u32 writeMask = 0xfu, u32 modifier = 0u) {
  return makeDstToken(regType, regIndex, writeMask, modifier) | (1u << 13);
}

u32 makeSrcToken(u32 regType, u32 regIndex, u32 swizzle = 0xe4u, u32 modifier = 0u) {
  return (1u << 31) | encodeRegisterType(regType) | ((modifier & 0xfu) << 24) | ((swizzle & 0xffu) << 16) |
         (regIndex & 0x7ffu);
}

u32 makeRelativeSrcToken(u32 regType, u32 regIndex, u32 swizzle = 0xe4u, u32 modifier = 0u) {
  return makeSrcToken(regType, regIndex, swizzle, modifier) | (1u << 13);
}

u32 makeSwizzle(u32 x, u32 y, u32 z, u32 w) {
  return (x & 0x3u) | ((y & 0x3u) << 2) | ((z & 0x3u) << 4) | ((w & 0x3u) << 6);
}

u32 makeDclSemanticToken(u32 usage, u32 usageIndex = 0u) {
  return (1u << 31) | (usage & 0xfu) | ((usageIndex & 0xfu) << 16);
}

u32 makeDclTextureCoordToken(u32 writeMask = 0xfu) {
  return (1u << 31) | ((writeMask & 0xfu) << 16);
}

u32 makeDclSamplerTypeToken(u32 textureType = 2u) {
  return (1u << 31) | ((textureType & 0xfu) << 27);
}

u32 makeLabelToken(u32 label) {
  return (1u << 31) | (label & 0x7ffu);
}

ShaderRef makeShader(std::span<const u32> words) {
  ShaderRef shader{};
  shader.kind = ShaderRef::Kind::Bytecode;
  const auto* first = reinterpret_cast<const u8*>(words.data());
  const auto* last = reinterpret_cast<const u8*>(words.data() + words.size());
  shader.bytecode.bytes.assign(first, last);
  shader.bytecode.hash = hashBytes(std::as_bytes(words));
  return shader;
}

std::string translatePixel(std::span<const u32> words) {
  const auto shader = makeShader(words);
  DrawDesc desc{};
  desc.pixelShader = shader;
  return dxmt9::translator::makeTranslatedFragmentSource(
      shader, dxmt9::drawshader::makeShaderSourceContext(desc));
}

// D3DSAMP_MIPMAPLODBIAS (specs/d3d9/gap_d3d9.md B.3) is gated behind the
// ShaderSourceContext::samplerLodBias variant flag. This overload threads the
// flag so the spec can assert both the bias-on emit and the byte-identical
// bias-off emit (no slot-4 param, plain sample()).
std::string translatePixel(std::span<const u32> words, bool samplerLodBias) {
  const auto shader = makeShader(words);
  DrawDesc desc{};
  desc.pixelShader = shader;
  auto context = dxmt9::drawshader::makeShaderSourceContext(desc);
  context.samplerLodBias = samplerLodBias;
  return dxmt9::translator::makeTranslatedFragmentSource(shader, context);
}

std::string translatePixelWithAlphaTestStrip(std::span<const u32> words) {
  const auto shader = makeShader(words);
  DrawDesc desc{};
  desc.pixelShader = shader;
  // Enable alpha test in the draw state so the debug strip is proven to win
  // over an alpha-test-active draw (H224 variant gating composes with the
  // DXMT_DISABLE_ALPHA_TEST strip; strip wins).
  desc.rs.values[RS_ALPHA_TEST_ENABLE] = 1u;
  auto context = dxmt9::drawshader::makeShaderSourceContext(desc);
  context.stripAlphaTestForDebug = true;
  return dxmt9::translator::makeTranslatedFragmentSource(shader, context);
}

// H224 — compile-time alpha-test/fog fragment tails. Build the fixture with
// explicit render states so makeShaderSourceContext(DrawDesc) resolves the
// fogActive gate exactly like the production flat-state
// overload does.
std::string translatePixelWithRenderStates(
    std::span<const u32> words,
    std::initializer_list<std::pair<u32, u32>> renderStates) {
  const auto shader = makeShader(words);
  DrawDesc desc{};
  desc.pixelShader = shader;
  for (const auto& [state, value] : renderStates) {
    desc.rs.values[state] = value;
  }
  return dxmt9::translator::makeTranslatedFragmentSource(
      shader, dxmt9::drawshader::makeShaderSourceContext(desc));
}

std::string translateVertex(std::span<const u32> words) {
  const auto shader = makeShader(words);
  DrawDesc desc{};
  desc.vertexShader = shader;
  return dxmt9::translator::makeTranslatedVertexSource(
      shader, dxmt9::drawshader::makeShaderSourceContext(desc));
}

std::string translateVertexWithVSOutLayout(
    std::span<const u32> words,
    dxmt9::shaders::VSOutLayout layout) {
  const auto shader = makeShader(words);
  DrawDesc desc{};
  desc.vertexShader = shader;
  auto context = dxmt9::drawshader::makeShaderSourceContext(desc);
  context.vsOutLayout = layout;
  return dxmt9::translator::makeTranslatedVertexSource(shader, context);
}

std::string translateVertex(std::span<const u32> words, std::vector<VertexElement> elements,
                            std::array<u32, kMaxStreams> strides = {},
                            bool argbufHybridMode = false) {
  const auto shader = makeShader(words);
  DrawDesc desc{};
  desc.vertexShader = shader;
  desc.vertexDecl.elements = std::move(elements);
  for (size_t i = 0; i < strides.size(); ++i) {
    desc.vertexDecl.streams[i].stride = strides[i];
  }
  auto context = dxmt9::drawshader::makeShaderSourceContext(desc);
  context.argbufHybridMode = argbufHybridMode;
  return dxmt9::translator::makeTranslatedVertexSource(
      shader, context);
}

dxmt9::ffp::VertexShaderInputLayout decodeVertexInputLayout(
    std::span<const u32> words,
    std::vector<VertexElement> elements,
    std::array<u32, kMaxStreams> strides = {}) {
  const auto shader = makeShader(words);
  DrawDesc desc{};
  desc.vertexShader = shader;
  desc.vertexDecl.elements = std::move(elements);
  for (size_t i = 0; i < strides.size(); ++i) {
    desc.vertexDecl.streams[i].stride = strides[i];
  }
  const auto context = dxmt9::drawshader::makeShaderSourceContext(desc);
  const auto module =
      dxmt9::translator::detail_::translateD3DBytecodeToSpirv(shader, true, context);
  const auto layout =
      dxmt9::translator::detail_::decodeVertexShaderInputLayout(module, context);
  if (!layout) {
    fail("vs_3_0 input layout did not decode");
  }
  return *layout;
}

std::vector<u32> makePs20TexturedBytecode(u32 samplerIndex) {
  using namespace dxmt9::d3d9bc;
  return {
      makeVersionToken(false, 2, 0),
      makeInstructionToken(kD3DSIO_DCL, 2),
      makeDclTextureCoordToken(0x3u),
      makeDstToken(kD3DSPR_ADDR, 0, 0x3u),
      makeInstructionToken(kD3DSIO_DCL, 2),
      makeDclSamplerTypeToken(),
      makeDstToken(kD3DSPR_SAMPLER, samplerIndex),
      makeInstructionToken(kD3DSIO_TEX, 3),
      makeDstToken(kD3DSPR_TEMP, 0),
      makeSrcToken(kD3DSPR_ADDR, 0),
      makeSrcToken(kD3DSPR_SAMPLER, samplerIndex),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_COLOROUT, 0),
      makeSrcToken(kD3DSPR_TEMP, 0),
      kD3DSIO_END,
  };
}

std::vector<u32> makePs30DecodeFixtureBytecode() {
  using namespace dxmt9::d3d9bc;
  return {
      makeVersionToken(false, 3, 0),
      makeCommentToken(2),
      0xfeedfaceu,
      0xc001d00du,
      makeInstructionToken(kD3DSIO_DCL, 2),
      makeDclSamplerTypeToken(),
      makeDstToken(kD3DSPR_SAMPLER, 5),
      makeInstructionToken(kD3DSIO_TEXLDD, 0),
      makeDstToken(kD3DSPR_TEMP, 3, 0x7u, 1u),
      makeSrcToken(kD3DSPR_CONST, 9, makeSwizzle(2u, 1u, 0u, 3u), 11u),
      makeSrcToken(kD3DSPR_SAMPLER, 5),
      makeSrcToken(kD3DSPR_CONST, 10),
      makeSrcToken(kD3DSPR_CONST, 11),
      makePredicatedInstructionToken(kD3DSIO_MOV, 2, 0x5au),
      makeDstToken(kD3DSPR_COLOROUT, 1),
      makeSrcToken(kD3DSPR_CONSTBOOL, 2),
      kD3DSIO_END,
  };
}

std::vector<u32> makePs30InputSemanticBytecode() {
  using namespace dxmt9::d3d9bc;
  constexpr u32 kD3DDeclUsageTexcoord = 5u;
  return {
      makeVersionToken(false, 3, 0),
      makeInstructionToken(kD3DSIO_DCL, 2),
      makeDclSemanticToken(kD3DDeclUsageTexcoord, 3u),
      makeDstToken(kD3DSPR_INPUT, 0),
      makeInstructionToken(kD3DSIO_DCL, 2),
      makeDclSamplerTypeToken(),
      makeDstToken(kD3DSPR_SAMPLER, 2),
      makeInstructionToken(kD3DSIO_TEX, 3),
      makeDstToken(kD3DSPR_TEMP, 0),
      makeSrcToken(kD3DSPR_INPUT, 0),
      makeSrcToken(kD3DSPR_SAMPLER, 2),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_COLOROUT, 0),
      makeSrcToken(kD3DSPR_TEMP, 0),
      kD3DSIO_END,
  };
}

std::vector<u32> makePs30HighTexcoordInputBytecode() {
  using namespace dxmt9::d3d9bc;
  constexpr u32 kD3DDeclUsageTexcoord = 5u;
  return {
      makeVersionToken(false, 3, 0),
      makeInstructionToken(kD3DSIO_DCL, 2),
      makeDclSemanticToken(kD3DDeclUsageTexcoord, 7u),
      makeDstToken(kD3DSPR_INPUT, 0),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_COLOROUT, 0),
      makeSrcToken(kD3DSPR_INPUT, 0),
      kD3DSIO_END,
  };
}

std::vector<u32> makePs20ColorInputBytecode(u32 inputIndex = 0) {
  using namespace dxmt9::d3d9bc;
  constexpr u32 kD3DDeclUsagePosition = 0u;
  return {
      makeVersionToken(false, 2, 0),
      makeInstructionToken(kD3DSIO_DCL, 2),
      makeDclSemanticToken(kD3DDeclUsagePosition, 0u),
      makeDstToken(kD3DSPR_INPUT, inputIndex),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_COLOROUT, 0),
      makeSrcToken(kD3DSPR_INPUT, inputIndex),
      kD3DSIO_END,
  };
}

std::vector<u32> makeVs30OutputSemanticBytecode() {
  using namespace dxmt9::d3d9bc;
  constexpr u32 kD3DDeclUsagePosition = 0u;
  constexpr u32 kD3DDeclUsageTexcoord = 5u;
  constexpr u32 kD3DDeclUsageColor = 10u;
  return {
      makeVersionToken(true, 3, 0),
      makeInstructionToken(kD3DSIO_DCL, 2),
      makeDclSemanticToken(kD3DDeclUsagePosition, 0u),
      makeDstToken(kD3DSPR_TEXCRDOUT, 0),
      makeInstructionToken(kD3DSIO_DCL, 2),
      makeDclSemanticToken(kD3DDeclUsageTexcoord, 2u),
      makeDstToken(kD3DSPR_TEXCRDOUT, 1),
      makeInstructionToken(kD3DSIO_DCL, 2),
      makeDclSemanticToken(kD3DDeclUsageColor, 1u),
      makeDstToken(kD3DSPR_TEXCRDOUT, 2),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_TEXCRDOUT, 0),
      makeSrcToken(kD3DSPR_CONST, 0),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_TEXCRDOUT, 1),
      makeSrcToken(kD3DSPR_CONST, 1),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_TEXCRDOUT, 2),
      makeSrcToken(kD3DSPR_CONST, 2),
      kD3DSIO_END,
  };
}

std::vector<u32> makeVs30TempFiveOutputBytecode() {
  using namespace dxmt9::d3d9bc;
  constexpr u32 kD3DDeclUsagePosition = 0u;
  return {
      makeVersionToken(true, 3, 0),
      makeInstructionToken(kD3DSIO_DCL, 2),
      makeDclSemanticToken(kD3DDeclUsagePosition, 0u),
      makeDstToken(kD3DSPR_TEXCRDOUT, 0),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_TEMP, 5),
      makeSrcToken(kD3DSPR_CONST, 0),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_TEXCRDOUT, 0),
      makeSrcToken(kD3DSPR_TEMP, 5),
      kD3DSIO_END,
  };
}

std::vector<u32> makeVs20DepthOutBytecode() {
  using namespace dxmt9::d3d9bc;
  return {
      makeVersionToken(true, 2, 0),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_DEPTHOUT, 0),
      makeSrcToken(kD3DSPR_CONST, 0),
      kD3DSIO_END,
  };
}

std::vector<u32> makeVs30HighRegisterOutputSemanticBytecode() {
  using namespace dxmt9::d3d9bc;
  constexpr u32 kD3DDeclUsagePosition = 0u;
  constexpr u32 kD3DDeclUsageTexcoord = 5u;
  return {
      makeVersionToken(true, 3, 0),
      makeInstructionToken(kD3DSIO_DCL, 2),
      makeDclSemanticToken(kD3DDeclUsagePosition, 0u),
      makeDstToken(kD3DSPR_INPUT, 0),
      makeInstructionToken(kD3DSIO_DCL, 2),
      makeDclSemanticToken(kD3DDeclUsageTexcoord, 0u),
      makeDstToken(kD3DSPR_INPUT, 1),
      makeInstructionToken(kD3DSIO_DCL, 2),
      makeDclSemanticToken(kD3DDeclUsageTexcoord, 1u),
      makeDstToken(kD3DSPR_INPUT, 2),
      makeInstructionToken(kD3DSIO_DCL, 2),
      makeDclSemanticToken(kD3DDeclUsagePosition, 0u),
      makeDstToken(kD3DSPR_TEXCRDOUT, 7),
      makeInstructionToken(kD3DSIO_DCL, 2),
      makeDclSemanticToken(kD3DDeclUsageTexcoord, 0u),
      makeDstToken(kD3DSPR_TEXCRDOUT, 3),
      makeInstructionToken(kD3DSIO_DCL, 2),
      makeDclSemanticToken(kD3DDeclUsageTexcoord, 1u),
      makeDstToken(kD3DSPR_TEXCRDOUT, 4),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_TEXCRDOUT, 7),
      makeSrcToken(kD3DSPR_INPUT, 0),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_TEXCRDOUT, 3),
      makeSrcToken(kD3DSPR_INPUT, 1),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_TEXCRDOUT, 4),
      makeSrcToken(kD3DSPR_INPUT, 2),
      kD3DSIO_END,
  };
}

std::vector<u32> makeVs30InputSemanticBytecode() {
  using namespace dxmt9::d3d9bc;
  constexpr u32 kD3DDeclUsagePosition = 0u;
  constexpr u32 kD3DDeclUsageBlendWeight = 1u;
  constexpr u32 kD3DDeclUsageBlendIndices = 2u;
  constexpr u32 kD3DDeclUsageTexcoord = 5u;
  return {
      makeVersionToken(true, 3, 0),
      makeInstructionToken(kD3DSIO_DCL, 2),
      makeDclSemanticToken(kD3DDeclUsagePosition, 0u),
      makeDstToken(kD3DSPR_INPUT, 0),
      makeInstructionToken(kD3DSIO_DCL, 2),
      makeDclSemanticToken(kD3DDeclUsageBlendWeight, 0u),
      makeDstToken(kD3DSPR_INPUT, 1),
      makeInstructionToken(kD3DSIO_DCL, 2),
      makeDclSemanticToken(kD3DDeclUsageBlendIndices, 0u),
      makeDstToken(kD3DSPR_INPUT, 2),
      makeInstructionToken(kD3DSIO_DCL, 2),
      makeDclSemanticToken(kD3DDeclUsageTexcoord, 0u),
      makeDstToken(kD3DSPR_INPUT, 3),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_RASTOUT, 0),
      makeSrcToken(kD3DSPR_INPUT, 0),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_TEXCRDOUT, 0),
      makeSrcToken(kD3DSPR_INPUT, 1),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_TEXCRDOUT, 1),
      makeSrcToken(kD3DSPR_INPUT, 2),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_TEXCRDOUT, 2),
      makeSrcToken(kD3DSPR_INPUT, 3),
      kD3DSIO_END,
  };
}

std::vector<u32> makeVs30SparseInputReadBytecode() {
  using namespace dxmt9::d3d9bc;
  constexpr u32 kD3DDeclUsagePosition = 0u;
  constexpr u32 kD3DDeclUsageTexcoord = 5u;
  return {
      makeVersionToken(true, 3, 0),
      makeInstructionToken(kD3DSIO_DCL, 2),
      makeDclSemanticToken(kD3DDeclUsagePosition, 0u),
      makeDstToken(kD3DSPR_INPUT, 0),
      makeInstructionToken(kD3DSIO_DCL, 2),
      makeDclSemanticToken(kD3DDeclUsageTexcoord, 0u),
      makeDstToken(kD3DSPR_INPUT, 7),
      makeInstructionToken(kD3DSIO_DCL, 2),
      makeDclSemanticToken(kD3DDeclUsagePosition, 0u),
      makeDstToken(kD3DSPR_TEXCRDOUT, 0),
      makeInstructionToken(kD3DSIO_DCL, 2),
      makeDclSemanticToken(kD3DDeclUsageTexcoord, 0u),
      makeDstToken(kD3DSPR_TEXCRDOUT, 1),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_TEXCRDOUT, 0),
      makeSrcToken(kD3DSPR_CONST, 0),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_TEXCRDOUT, 1),
      makeSrcToken(kD3DSPR_INPUT, 7),
      kD3DSIO_END,
  };
}

std::vector<u32> makePs30WriteMaskSwizzleModifierBytecode() {
  using namespace dxmt9::d3d9bc;
  return {
      makeVersionToken(false, 3, 0),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_TEMP, 0),
      makeSrcToken(kD3DSPR_CONST, 0, makeSwizzle(3u, 2u, 1u, 0u)),
      makeInstructionToken(kD3DSIO_ADD, 3),
      makeDstToken(kD3DSPR_TEMP, 1, 0x5u),
      makeSrcToken(kD3DSPR_TEMP, 0, makeSwizzle(1u, 0u, 3u, 2u), 1u),
      makeSrcToken(kD3DSPR_CONST, 1, 0xe4u, 11u),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_COLOROUT, 0),
      makeSrcToken(kD3DSPR_TEMP, 1),
      kD3DSIO_END,
  };
}

std::vector<u32> makePs30SourceModifierCoverageBytecode() {
  using namespace dxmt9::d3d9bc;
  return {
      makeVersionToken(false, 3, 0),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_TEMP, 0),
      makeSrcToken(kD3DSPR_CONST, 0, 0xe4u, 5u),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_TEMP, 1),
      makeSrcToken(kD3DSPR_CONST, 1, 0xe4u, 6u),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_TEMP, 2),
      makeSrcToken(kD3DSPR_CONST, 2, 0xe4u, 7u),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_TEMP, 3),
      makeSrcToken(kD3DSPR_CONST, 3, 0xe4u, 8u),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_TEMP, 4),
      makeSrcToken(kD3DSPR_CONST, 4, 0xe4u, 9u),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_TEMP, 5),
      makeSrcToken(kD3DSPR_CONST, 5, 0xe4u, 10u),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_TEMP, 6),
      makeSrcToken(kD3DSPR_CONSTBOOL, 0, 0xe4u, 13u),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_TEMP, 8),
      makeSrcToken(kD3DSPR_CONST, 8, 0xe4u, 2u),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_TEMP, 9),
      makeSrcToken(kD3DSPR_CONST, 9, 0xe4u, 3u),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_TEMP, 10),
      makeSrcToken(kD3DSPR_CONST, 10, 0xe4u, 4u),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_TEMP, 11),
      makeSrcToken(kD3DSPR_CONST, 11, 0xe4u, 12u),
      makeInstructionToken(kD3DSIO_ADD, 3),
      makeDstToken(kD3DSPR_TEMP, 7),
      makeSrcToken(kD3DSPR_TEMP, 0),
      makeSrcToken(kD3DSPR_TEMP, 6),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_COLOROUT, 0),
      makeSrcToken(kD3DSPR_TEMP, 7),
      kD3DSIO_END,
  };
}

std::vector<u32> makePs30ConstIntSourceBytecode() {
  using namespace dxmt9::d3d9bc;
  return {
      makeVersionToken(false, 3, 0),
      makeInstructionToken(kD3DSIO_DEFI, 5),
      makeDstToken(kD3DSPR_CONSTINT, 0),
      1u,
      2u,
      3u,
      4u,
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_TEMP, 0),
      makeSrcToken(kD3DSPR_CONSTINT, 0),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_COLOROUT, 0),
      makeSrcToken(kD3DSPR_TEMP, 0),
      kD3DSIO_END,
  };
}

std::vector<u32> makePs30LoopRegisterConstIntBytecode() {
  using namespace dxmt9::d3d9bc;
  return {
      makeVersionToken(false, 3, 0),
      makeInstructionToken(kD3DSIO_DEFI, 5),
      makeDstToken(kD3DSPR_CONSTINT, 0),
      2u,
      0u,
      0u,
      0u,
      makeInstructionToken(kD3DSIO_LOOP, 2),
      makeSrcToken(kD3DSPR_LOOP, 0),
      makeSrcToken(kD3DSPR_CONSTINT, 0),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_TEMP, 0),
      makeSrcToken(kD3DSPR_CONST, 1),
      makeInstructionToken(kD3DSIO_ENDLOOP, 0),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_COLOROUT, 0),
      makeSrcToken(kD3DSPR_TEMP, 0),
      kD3DSIO_END,
  };
}

std::vector<u32> makePs30DestModifierCoverageBytecode() {
  using namespace dxmt9::d3d9bc;
  constexpr u32 kD3DSPDMSaturate = 1u;
  constexpr u32 kD3DSPDMPartialPrecision = 2u;
  return {
      makeVersionToken(false, 3, 0),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_TEMP, 0, 0xfu, kD3DSPDMPartialPrecision),
      makeSrcToken(kD3DSPR_CONST, 0),
      makeInstructionToken(kD3DSIO_ADD, 3),
      makeDstToken(kD3DSPR_TEMP, 1, 0xfu, kD3DSPDMSaturate | kD3DSPDMPartialPrecision),
      makeSrcToken(kD3DSPR_CONST, 1),
      makeSrcToken(kD3DSPR_CONST, 2),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_COLOROUT, 0),
      makeSrcToken(kD3DSPR_TEMP, 1),
      kD3DSIO_END,
  };
}

std::vector<u32> makePs30CentroidInputBytecode() {
  using namespace dxmt9::d3d9bc;
  constexpr u32 kD3DSPDMMsampCentroid = 4u;
  constexpr u32 kD3DDeclUsageTexcoord = 5u;
  constexpr u32 kD3DDeclUsageColor = 10u;
  constexpr u32 kD3DDeclUsageFog = 11u;
  return {
      makeVersionToken(false, 3, 0),
      makeInstructionToken(kD3DSIO_DCL, 2),
      makeDclSemanticToken(kD3DDeclUsageColor, 0u),
      makeDstToken(kD3DSPR_INPUT, 0, 0xfu, kD3DSPDMMsampCentroid),
      makeInstructionToken(kD3DSIO_DCL, 2),
      makeDclSemanticToken(kD3DDeclUsageTexcoord, 3u),
      makeDstToken(kD3DSPR_INPUT, 2, 0xfu, kD3DSPDMMsampCentroid),
      makeInstructionToken(kD3DSIO_DCL, 2),
      makeDclSemanticToken(kD3DDeclUsageFog, 0u),
      makeDstToken(kD3DSPR_INPUT, 3, 0xfu, kD3DSPDMMsampCentroid),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_COLOROUT, 0),
      makeSrcToken(kD3DSPR_INPUT, 0),
      kD3DSIO_END,
  };
}

std::vector<u32> makeVs20DestModifierCoverageBytecode() {
  using namespace dxmt9::d3d9bc;
  constexpr u32 kD3DSPDMSaturate = 1u;
  constexpr u32 kD3DSPDMPartialPrecision = 2u;
  return {
      makeVersionToken(true, 2, 0),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_TEMP, 0, 0xfu, kD3DSPDMPartialPrecision),
      makeSrcToken(kD3DSPR_CONST, 0),
      makeInstructionToken(kD3DSIO_ADD, 3),
      makeDstToken(kD3DSPR_TEMP, 1, 0xfu, kD3DSPDMSaturate | kD3DSPDMPartialPrecision),
      makeSrcToken(kD3DSPR_CONST, 1),
      makeSrcToken(kD3DSPR_CONST, 2),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_RASTOUT, 0),
      makeSrcToken(kD3DSPR_TEMP, 1),
      kD3DSIO_END,
  };
}

std::vector<u32> makePs30IfElseBytecode() {
  using namespace dxmt9::d3d9bc;
  return {
      makeVersionToken(false, 3, 0),
      makeInstructionToken(kD3DSIO_IF, 1),
      makeSrcToken(kD3DSPR_CONST, 0),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_TEMP, 0),
      makeSrcToken(kD3DSPR_CONST, 1),
      makeInstructionToken(kD3DSIO_ELSE, 0),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_TEMP, 0),
      makeSrcToken(kD3DSPR_CONST, 2),
      makeInstructionToken(kD3DSIO_ENDIF, 0),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_COLOROUT, 0),
      makeSrcToken(kD3DSPR_TEMP, 0),
      kD3DSIO_END,
  };
}

std::vector<u32> makePs30LoopBytecode() {
  using namespace dxmt9::d3d9bc;
  return {
      makeVersionToken(false, 3, 0),
      makeInstructionToken(kD3DSIO_LOOP, 1),
      makeSrcToken(kD3DSPR_CONST, 0),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_TEMP, 0),
      makeSrcToken(kD3DSPR_CONST, 1),
      makeInstructionToken(kD3DSIO_ENDLOOP, 0),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_COLOROUT, 0),
      makeSrcToken(kD3DSPR_TEMP, 0),
      kD3DSIO_END,
  };
}

std::vector<u32> makePs30NestedLoopBytecode() {
  using namespace dxmt9::d3d9bc;
  return {
      makeVersionToken(false, 3, 0),
      makeInstructionToken(kD3DSIO_LOOP, 1),
      makeSrcToken(kD3DSPR_CONST, 0),
      makeInstructionToken(kD3DSIO_REP, 1),
      makeSrcToken(kD3DSPR_CONST, 1),
      makeInstructionToken(kD3DSIO_ADD, 3),
      makeDstToken(kD3DSPR_TEMP, 0),
      makeSrcToken(kD3DSPR_TEMP, 0),
      makeSrcToken(kD3DSPR_CONST, 2),
      makeInstructionToken(kD3DSIO_ENDREP, 0),
      makeInstructionToken(kD3DSIO_ENDLOOP, 0),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_COLOROUT, 0),
      makeSrcToken(kD3DSPR_TEMP, 0),
      kD3DSIO_END,
  };
}

std::vector<u32> makePs30RepBytecode() {
  using namespace dxmt9::d3d9bc;
  return {
      makeVersionToken(false, 3, 0),
      makeInstructionToken(kD3DSIO_REP, 1),
      makeSrcToken(kD3DSPR_CONST, 2),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_TEMP, 1),
      makeSrcToken(kD3DSPR_CONST, 3),
      makeInstructionToken(kD3DSIO_ENDREP, 0),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_COLOROUT, 0),
      makeSrcToken(kD3DSPR_TEMP, 1),
      kD3DSIO_END,
  };
}

std::vector<u32> makePs30BreakcBytecode() {
  using namespace dxmt9::d3d9bc;
  constexpr u32 kD3DSPCGt = 1u;
  return {
      makeVersionToken(false, 3, 0),
      makeInstructionToken(kD3DSIO_REP, 1),
      makeSrcToken(kD3DSPR_CONST, 2),
      makeControlledInstructionToken(kD3DSIO_BREAKC, 2, kD3DSPCGt),
      makeSrcToken(kD3DSPR_CONST, 0),
      makeSrcToken(kD3DSPR_CONST, 1),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_TEMP, 0),
      makeSrcToken(kD3DSPR_CONST, 3),
      makeInstructionToken(kD3DSIO_ENDREP, 0),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_COLOROUT, 0),
      makeSrcToken(kD3DSPR_TEMP, 0),
      kD3DSIO_END,
  };
}

std::vector<u32> makePs30CallLabelRetBytecode() {
  using namespace dxmt9::d3d9bc;
  return {
      makeVersionToken(false, 3, 0),
      makeInstructionToken(kD3DSIO_CALL, 1),
      makeLabelToken(7),
      makeInstructionToken(kD3DSIO_LABEL, 1),
      makeLabelToken(7),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_TEMP, 0),
      makeSrcToken(kD3DSPR_CONST, 4),
      makeInstructionToken(kD3DSIO_RET, 0),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_COLOROUT, 0),
      makeSrcToken(kD3DSPR_TEMP, 0),
      kD3DSIO_END,
  };
}

std::vector<u32> makePs30CallnzLabelRetBytecode() {
  using namespace dxmt9::d3d9bc;
  return {
      makeVersionToken(false, 3, 0),
      makeInstructionToken(kD3DSIO_CALLNZ, 0),
      makeLabelToken(9),
      makeSrcToken(kD3DSPR_CONSTBOOL, 0),
      makeInstructionToken(kD3DSIO_LABEL, 1),
      makeLabelToken(9),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_TEMP, 0),
      makeSrcToken(kD3DSPR_CONST, 4),
      makeInstructionToken(kD3DSIO_RET, 0),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_COLOROUT, 0),
      makeSrcToken(kD3DSPR_TEMP, 0),
      kD3DSIO_END,
  };
}

std::vector<u32> makePs30CallLabelNestedRetBytecode() {
  using namespace dxmt9::d3d9bc;
  return {
      makeVersionToken(false, 3, 0),
      makeInstructionToken(kD3DSIO_CALL, 1),
      makeLabelToken(7),
      makeInstructionToken(kD3DSIO_LABEL, 1),
      makeLabelToken(7),
      makeInstructionToken(kD3DSIO_IF, 1),
      makeSrcToken(kD3DSPR_CONST, 0),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_TEMP, 0),
      makeSrcToken(kD3DSPR_CONST, 1),
      makeInstructionToken(kD3DSIO_RET, 0),
      makeInstructionToken(kD3DSIO_ENDIF, 0),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_TEMP, 0),
      makeSrcToken(kD3DSPR_CONST, 2),
      makeInstructionToken(kD3DSIO_RET, 0),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_COLOROUT, 0),
      makeSrcToken(kD3DSPR_TEMP, 0),
      kD3DSIO_END,
  };
}

std::vector<u32> makePs30PredicatedIfBytecode() {
  using namespace dxmt9::d3d9bc;
  return {
      makeVersionToken(false, 3, 0),
      makePredicatedInstructionToken(kD3DSIO_IF, 1, 0u),
      makeSrcToken(kD3DSPR_CONST, 0),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_TEMP, 0),
      makeSrcToken(kD3DSPR_CONST, 1),
      makeInstructionToken(kD3DSIO_ELSE, 0),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_TEMP, 0),
      makeSrcToken(kD3DSPR_CONST, 2),
      makeInstructionToken(kD3DSIO_ENDIF, 0),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_COLOROUT, 0),
      makeSrcToken(kD3DSPR_TEMP, 0),
      kD3DSIO_END,
  };
}

std::vector<u32> makePs30PredicatedBreakcBytecode() {
  using namespace dxmt9::d3d9bc;
  constexpr u32 kD3DSPCGt = 1u;
  return {
      makeVersionToken(false, 3, 0),
      makeInstructionToken(kD3DSIO_REP, 1),
      makeSrcToken(kD3DSPR_CONST, 2),
      makePredicatedInstructionToken(kD3DSIO_BREAKC, 2, kD3DSPCGt),
      makeSrcToken(kD3DSPR_CONST, 0),
      makeSrcToken(kD3DSPR_CONST, 1),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_TEMP, 0),
      makeSrcToken(kD3DSPR_CONST, 3),
      makeInstructionToken(kD3DSIO_ENDREP, 0),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_COLOROUT, 0),
      makeSrcToken(kD3DSPR_TEMP, 0),
      kD3DSIO_END,
  };
}

std::vector<u32> makePs30PredicatedCallnzLabelRetBytecode() {
  using namespace dxmt9::d3d9bc;
  return {
      makeVersionToken(false, 3, 0),
      makePredicatedInstructionToken(kD3DSIO_CALLNZ, 0, 0u),
      makeLabelToken(9),
      makeSrcToken(kD3DSPR_CONSTBOOL, 0),
      makeInstructionToken(kD3DSIO_LABEL, 1),
      makeLabelToken(9),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_TEMP, 0),
      makeSrcToken(kD3DSPR_CONST, 4),
      makeInstructionToken(kD3DSIO_RET, 0),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_COLOROUT, 0),
      makeSrcToken(kD3DSPR_TEMP, 0),
      kD3DSIO_END,
  };
}

std::vector<u32> makePs30PositionInputBytecode() {
  using namespace dxmt9::d3d9bc;
  constexpr u32 kD3DDeclUsagePosition = 0u;
  return {
      makeVersionToken(false, 3, 0),
      makeInstructionToken(kD3DSIO_DCL, 2),
      makeDclSemanticToken(kD3DDeclUsagePosition, 0u),
      makeDstToken(kD3DSPR_INPUT, 0),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_COLOROUT, 0),
      makeSrcToken(kD3DSPR_INPUT, 0),
      kD3DSIO_END,
  };
}

std::vector<u32> makePs30VFaceBytecode() {
  using namespace dxmt9::d3d9bc;
  return {
      makeVersionToken(false, 3, 0),
      makeInstructionToken(kD3DSIO_DCL, 2),
      makeDclSemanticToken(0u),
      makeDstToken(kD3DSPR_MISCTYPE, 1),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_COLOROUT, 0),
      makeSrcToken(kD3DSPR_MISCTYPE, 1),
      kD3DSIO_END,
  };
}

std::vector<u32> makePs30MissingInputBytecode() {
  using namespace dxmt9::d3d9bc;
  return {
      makeVersionToken(false, 3, 0),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_COLOROUT, 0),
      makeSrcToken(kD3DSPR_INPUT, 5),
      kD3DSIO_END,
  };
}

std::vector<u32> makePs30IndexedConstSourceBytecode() {
  using namespace dxmt9::d3d9bc;
  return {
      makeVersionToken(false, 3, 0),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_TEMP, 0),
      makeRelativeSrcToken(kD3DSPR_CONST, 7),
      makeSrcToken(kD3DSPR_ADDR, 0),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_COLOROUT, 0),
      makeSrcToken(kD3DSPR_TEMP, 0),
      kD3DSIO_END,
  };
}

std::vector<u32> makePs30IndexedConstDestinationBytecode() {
  using namespace dxmt9::d3d9bc;
  return {
      makeVersionToken(false, 3, 0),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeRelativeDstToken(kD3DSPR_CONST, 7),
      makeSrcToken(kD3DSPR_ADDR, 0),
      makeSrcToken(kD3DSPR_CONST, 1),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_COLOROUT, 0),
      makeSrcToken(kD3DSPR_CONST, 1),
      kD3DSIO_END,
  };
}

std::vector<u32> makePs11TexcoordTexBytecode() {
  using namespace dxmt9::d3d9bc;
  return {
      makeVersionToken(false, 1, 1),
      makeInstructionToken(kD3DSIO_TEXCOORD, 0),
      makeDstToken(kD3DSPR_ADDR, 0),
      makeInstructionToken(kD3DSIO_TEX, 0),
      makeDstToken(kD3DSPR_ADDR, 0),
      makeInstructionToken(kD3DSIO_MOV, 0),
      makeDstToken(kD3DSPR_COLOROUT, 0),
      makeSrcToken(kD3DSPR_ADDR, 0),
      kD3DSIO_END,
  };
}

std::vector<u32> makePs14TexcrdTexldDepthBytecode() {
  using namespace dxmt9::d3d9bc;
  return {
      makeVersionToken(false, 1, 4),
      makeInstructionToken(kD3DSIO_TEXCOORD, 0),
      makeDstToken(kD3DSPR_TEMP, 0),
      makeSrcToken(kD3DSPR_ADDR, 0),
      makeInstructionToken(kD3DSIO_TEX, 0),
      makeDstToken(kD3DSPR_TEMP, 1),
      makeSrcToken(kD3DSPR_ADDR, 1),
      makeInstructionToken(kD3DSIO_TEXDEPTH, 0),
      makeSrcToken(kD3DSPR_TEMP, 5),
      makeInstructionToken(kD3DSIO_MOV, 0),
      makeDstToken(kD3DSPR_COLOROUT, 0),
      makeSrcToken(kD3DSPR_TEMP, 1),
      kD3DSIO_END,
  };
}

std::vector<u32> makePs13LegacyTextureFamilyBytecode() {
  using namespace dxmt9::d3d9bc;
  return {
      makeVersionToken(false, 1, 3),
      makeInstructionToken(kD3DSIO_TEXCOORD, 0), makeDstToken(kD3DSPR_ADDR, 0),
      makeInstructionToken(kD3DSIO_TEXCOORD, 0), makeDstToken(kD3DSPR_ADDR, 1),
      makeInstructionToken(kD3DSIO_TEXBEM, 0), makeDstToken(kD3DSPR_ADDR, 0), makeSrcToken(kD3DSPR_ADDR, 1),
      makeInstructionToken(kD3DSIO_TEXBEML, 0), makeDstToken(kD3DSPR_ADDR, 1), makeSrcToken(kD3DSPR_ADDR, 0),
      makeInstructionToken(kD3DSIO_TEXREG2AR, 0), makeDstToken(kD3DSPR_ADDR, 2), makeSrcToken(kD3DSPR_ADDR, 0),
      makeInstructionToken(kD3DSIO_TEXREG2GB, 0), makeDstToken(kD3DSPR_ADDR, 3), makeSrcToken(kD3DSPR_ADDR, 1),
      makeInstructionToken(kD3DSIO_TEXREG2RGB, 0), makeDstToken(kD3DSPR_ADDR, 4), makeSrcToken(kD3DSPR_ADDR, 2),
      makeInstructionToken(kD3DSIO_TEXDP3, 0), makeDstToken(kD3DSPR_ADDR, 5), makeSrcToken(kD3DSPR_ADDR, 4),
      makeInstructionToken(kD3DSIO_TEXDP3TEX, 0), makeDstToken(kD3DSPR_ADDR, 5), makeSrcToken(kD3DSPR_ADDR, 4),
      makeInstructionToken(kD3DSIO_TEXM3x2PAD, 0), makeDstToken(kD3DSPR_ADDR, 0), makeSrcToken(kD3DSPR_ADDR, 1),
      makeInstructionToken(kD3DSIO_TEXM3x2TEX, 0), makeDstToken(kD3DSPR_ADDR, 1), makeSrcToken(kD3DSPR_ADDR, 0),
      makeInstructionToken(kD3DSIO_TEXM3x3PAD, 0), makeDstToken(kD3DSPR_ADDR, 0), makeSrcToken(kD3DSPR_ADDR, 1),
      makeInstructionToken(kD3DSIO_TEXM3x3PAD, 0), makeDstToken(kD3DSPR_ADDR, 1), makeSrcToken(kD3DSPR_ADDR, 0),
      makeInstructionToken(kD3DSIO_TEXM3x3TEX, 0), makeDstToken(kD3DSPR_ADDR, 2), makeSrcToken(kD3DSPR_ADDR, 1),
      makeInstructionToken(kD3DSIO_TEXM3x3SPEC, 0), makeDstToken(kD3DSPR_ADDR, 3), makeSrcToken(kD3DSPR_ADDR, 2),
      makeSrcToken(kD3DSPR_CONST, 0),
      makeInstructionToken(kD3DSIO_TEXM3x3VSPEC, 0), makeDstToken(kD3DSPR_ADDR, 4), makeSrcToken(kD3DSPR_ADDR, 3),
      makeInstructionToken(kD3DSIO_TEXM3x3, 0), makeDstToken(kD3DSPR_ADDR, 5), makeSrcToken(kD3DSPR_ADDR, 4),
      makeInstructionToken(kD3DSIO_TEXM3x2PAD, 0), makeDstToken(kD3DSPR_ADDR, 0), makeSrcToken(kD3DSPR_ADDR, 1),
      makeInstructionToken(kD3DSIO_TEXM3x2DEPTH, 0), makeDstToken(kD3DSPR_ADDR, 1), makeSrcToken(kD3DSPR_ADDR, 0),
      makeInstructionToken(kD3DSIO_MOV, 0), makeDstToken(kD3DSPR_COLOROUT, 0), makeSrcToken(kD3DSPR_ADDR, 5),
      kD3DSIO_END,
  };
}

std::vector<u32> makePs14BemBytecode() {
  using namespace dxmt9::d3d9bc;
  return {
      makeVersionToken(false, 1, 4),
      makeInstructionToken(kD3DSIO_BEM, 0),
      makeDstToken(kD3DSPR_TEMP, 0),
      makeSrcToken(kD3DSPR_ADDR, 0),
      makeSrcToken(kD3DSPR_TEMP, 1),
      makeInstructionToken(kD3DSIO_MOV, 0),
      makeDstToken(kD3DSPR_COLOROUT, 0),
      makeSrcToken(kD3DSPR_TEMP, 0),
      kD3DSIO_END,
  };
}

std::vector<u32> makePs13ReservedTexm3x3DiffBytecode() {
  using namespace dxmt9::d3d9bc;
  return {
      makeVersionToken(false, 1, 3),
      makeInstructionToken(kD3DSIO_TEXM3x3DIFF, 0),
      makeDstToken(kD3DSPR_ADDR, 0),
      makeSrcToken(kD3DSPR_ADDR, 1),
      kD3DSIO_END,
  };
}

std::vector<u32> makePs30CallnzFixedOperandCountBytecode() {
  using namespace dxmt9::d3d9bc;
  return {
      makeVersionToken(false, 3, 0),
      makeInstructionToken(kD3DSIO_CALLNZ, 0),
      makeLabelToken(3),
      makeSrcToken(kD3DSPR_CONSTBOOL, 0),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_COLOROUT, 0),
      makeSrcToken(kD3DSPR_CONST, 0),
      kD3DSIO_END,
  };
}

std::vector<u32> makePs30ArithmeticOpcodeMatrixBytecode() {
  using namespace dxmt9::d3d9bc;
  return {
      makeVersionToken(false, 3, 0),
      makeInstructionToken(kD3DSIO_MAD, 4),
      makeDstToken(kD3DSPR_TEMP, 0),
      makeSrcToken(kD3DSPR_CONST, 0),
      makeSrcToken(kD3DSPR_CONST, 1),
      makeSrcToken(kD3DSPR_CONST, 2),
      makeInstructionToken(kD3DSIO_DP3, 3),
      makeDstToken(kD3DSPR_TEMP, 1),
      makeSrcToken(kD3DSPR_TEMP, 0),
      makeSrcToken(kD3DSPR_CONST, 3),
      makeInstructionToken(kD3DSIO_DP4, 3),
      makeDstToken(kD3DSPR_TEMP, 2),
      makeSrcToken(kD3DSPR_TEMP, 0),
      makeSrcToken(kD3DSPR_CONST, 4),
      makeInstructionToken(kD3DSIO_CMP, 4),
      makeDstToken(kD3DSPR_TEMP, 3),
      makeSrcToken(kD3DSPR_CONST, 5),
      makeSrcToken(kD3DSPR_CONST, 6),
      makeSrcToken(kD3DSPR_CONST, 7),
      makeInstructionToken(kD3DSIO_SLT, 3),
      makeDstToken(kD3DSPR_TEMP, 4),
      makeSrcToken(kD3DSPR_CONST, 8),
      makeSrcToken(kD3DSPR_CONST, 9),
      makeInstructionToken(kD3DSIO_SGE, 3),
      makeDstToken(kD3DSPR_TEMP, 5),
      makeSrcToken(kD3DSPR_CONST, 10),
      makeSrcToken(kD3DSPR_CONST, 11),
      makeInstructionToken(kD3DSIO_POW, 3),
      makeDstToken(kD3DSPR_TEMP, 6),
      makeSrcToken(kD3DSPR_CONST, 12),
      makeSrcToken(kD3DSPR_CONST, 13),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_COLOROUT, 0),
      makeSrcToken(kD3DSPR_TEMP, 6),
      kD3DSIO_END,
  };
}

std::vector<u32> makePs30TranscendentalOpcodeBytecode() {
  using namespace dxmt9::d3d9bc;
  return {
      makeVersionToken(false, 3, 0),
      makeInstructionToken(kD3DSIO_SINCOS, 2),
      makeDstToken(kD3DSPR_TEMP, 0),
      makeSrcToken(kD3DSPR_CONST, 0),
      makeInstructionToken(kD3DSIO_LOG, 2),
      makeDstToken(kD3DSPR_TEMP, 1),
      makeSrcToken(kD3DSPR_CONST, 1),
      makeInstructionToken(kD3DSIO_EXP, 2),
      makeDstToken(kD3DSPR_TEMP, 2),
      makeSrcToken(kD3DSPR_CONST, 2),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_COLOROUT, 0),
      makeSrcToken(kD3DSPR_TEMP, 2),
      kD3DSIO_END,
  };
}

std::vector<u32> makePs30MatrixOpcodeBytecode() {
  using namespace dxmt9::d3d9bc;
  return {
      makeVersionToken(false, 3, 0),
      makeInstructionToken(kD3DSIO_M4x4, 3),
      makeDstToken(kD3DSPR_TEMP, 0),
      makeSrcToken(kD3DSPR_CONST, 0),
      makeSrcToken(kD3DSPR_CONST, 4),
      makeInstructionToken(kD3DSIO_M3x3, 3),
      makeDstToken(kD3DSPR_TEMP, 1),
      makeSrcToken(kD3DSPR_CONST, 1),
      makeSrcToken(kD3DSPR_CONST, 8),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_COLOROUT, 0),
      makeSrcToken(kD3DSPR_TEMP, 1),
      kD3DSIO_END,
  };
}

std::vector<u32> makePs30TextureLodOpcodeBytecode() {
  using namespace dxmt9::d3d9bc;
  return {
      makeVersionToken(false, 3, 0),
      makeInstructionToken(kD3DSIO_DCL, 2),
      makeDclSamplerTypeToken(),
      makeDstToken(kD3DSPR_SAMPLER, 3),
      makeInstructionToken(kD3DSIO_DCL, 2),
      makeDclSamplerTypeToken(),
      makeDstToken(kD3DSPR_SAMPLER, 4),
      makeInstructionToken(kD3DSIO_TEXLDD, 5),
      makeDstToken(kD3DSPR_TEMP, 0),
      makeSrcToken(kD3DSPR_CONST, 0),
      makeSrcToken(kD3DSPR_SAMPLER, 3),
      makeSrcToken(kD3DSPR_CONST, 2),
      makeSrcToken(kD3DSPR_CONST, 3),
      makeInstructionToken(kD3DSIO_TEXLDL, 3),
      makeDstToken(kD3DSPR_TEMP, 1),
      makeSrcToken(kD3DSPR_CONST, 1),
      makeSrcToken(kD3DSPR_SAMPLER, 4),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_COLOROUT, 0),
      makeSrcToken(kD3DSPR_TEMP, 1),
      kD3DSIO_END,
  };
}

std::vector<u32> makePs30TexlddSamplerTypeBytecode(u32 samplerType) {
  using namespace dxmt9::d3d9bc;
  return {
      makeVersionToken(false, 3, 0),
      makeInstructionToken(kD3DSIO_DCL, 2),
      makeDclSamplerTypeToken(samplerType),
      makeDstToken(kD3DSPR_SAMPLER, 3),
      makeInstructionToken(kD3DSIO_TEXLDD, 5),
      makeDstToken(kD3DSPR_TEMP, 0),
      makeSrcToken(kD3DSPR_CONST, 0),
      makeSrcToken(kD3DSPR_SAMPLER, 3),
      makeSrcToken(kD3DSPR_CONST, 2),
      makeSrcToken(kD3DSPR_CONST, 3),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_COLOROUT, 0),
      makeSrcToken(kD3DSPR_TEMP, 0),
      kD3DSIO_END,
  };
}

std::vector<u32> makePs30TexkillBytecode() {
  using namespace dxmt9::d3d9bc;
  return {
      makeVersionToken(false, 3, 0),
      makeInstructionToken(kD3DSIO_DEF, 5),
      makeDstToken(kD3DSPR_CONST, 0),
      0xbf800000u,
      0x3e800000u,
      0x00000000u,
      0x3f800000u,
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_TEMP, 0),
      makeSrcToken(kD3DSPR_CONST, 0),
      makeInstructionToken(kD3DSIO_TEXKILL, 1),
      makeDstToken(kD3DSPR_TEMP, 0, 0x3u),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_COLOROUT, 0),
      makeSrcToken(kD3DSPR_CONST, 0),
      kD3DSIO_END,
  };
}

std::vector<u32> makePs30FixedOperandCountDecodeBytecode() {
  using namespace dxmt9::d3d9bc;
  return {
      makeVersionToken(false, 3, 0),
      makeInstructionToken(kD3DSIO_DP4, 0),
      makeDstToken(kD3DSPR_TEMP, 0),
      makeSrcToken(kD3DSPR_CONST, 0),
      makeSrcToken(kD3DSPR_CONST, 1),
      makeInstructionToken(kD3DSIO_MAD, 0),
      makeDstToken(kD3DSPR_TEMP, 1),
      makeSrcToken(kD3DSPR_TEMP, 0),
      makeSrcToken(kD3DSPR_CONST, 2),
      makeSrcToken(kD3DSPR_CONST, 3),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_COLOROUT, 0),
      makeSrcToken(kD3DSPR_TEMP, 1),
      kD3DSIO_END,
  };
}

std::vector<u32> makePs30RelativeAddressingBytecode() {
  using namespace dxmt9::d3d9bc;
  return {
      makeVersionToken(false, 3, 0),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeRelativeDstToken(kD3DSPR_TEMP, 0),
      makeSrcToken(kD3DSPR_ADDR, 0),
      makeSrcToken(kD3DSPR_CONST, 0),
      kD3DSIO_END,
  };
}

std::vector<u32> makePs30TempRelativeSourceBytecode() {
  using namespace dxmt9::d3d9bc;
  return {
      makeVersionToken(false, 3, 0),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_TEMP, 0),
      makeSrcToken(kD3DSPR_CONST, 0),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_TEMP, 1),
      makeSrcToken(kD3DSPR_CONST, 1),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_TEMP, 2),
      makeRelativeSrcToken(kD3DSPR_TEMP, 1),
      makeSrcToken(kD3DSPR_ADDR, 0),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_COLOROUT, 0),
      makeSrcToken(kD3DSPR_TEMP, 2),
      kD3DSIO_END,
  };
}

std::vector<u32> makeVs11FixedFunctionOutputBytecode() {
  using namespace dxmt9::d3d9bc;
  return {
      makeVersionToken(true, 1, 1),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_RASTOUT, 0),
      makeSrcToken(kD3DSPR_INPUT, 0),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_ATTROUT, 0),
      makeSrcToken(kD3DSPR_CONST, 0),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_ATTROUT, 1),
      makeSrcToken(kD3DSPR_CONST, 1),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_TEXCRDOUT, 3),
      makeSrcToken(kD3DSPR_CONST, 2),
      kD3DSIO_END,
  };
}

std::vector<u32> makeVs30MissingInputBytecode() {
  using namespace dxmt9::d3d9bc;
  constexpr u32 kD3DDeclUsagePosition = 0u;
  constexpr u32 kD3DDeclUsageTexcoord = 5u;
  return {
      makeVersionToken(true, 3, 0),
      makeInstructionToken(kD3DSIO_DCL, 2),
      makeDclSemanticToken(kD3DDeclUsagePosition, 0u),
      makeDstToken(kD3DSPR_TEXCRDOUT, 0),
      makeInstructionToken(kD3DSIO_DCL, 2),
      makeDclSemanticToken(kD3DDeclUsageTexcoord, 0u),
      makeDstToken(kD3DSPR_TEXCRDOUT, 1),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_TEXCRDOUT, 0),
      makeSrcToken(kD3DSPR_INPUT, 0),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_TEXCRDOUT, 1),
      makeSrcToken(kD3DSPR_INPUT, 5),
      kD3DSIO_END,
  };
}

std::vector<u32> makeVs30TexcoordRelativeDestinationBytecode() {
  using namespace dxmt9::d3d9bc;
  return {
      makeVersionToken(true, 3, 0),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeRelativeDstToken(kD3DSPR_TEXCRDOUT, 1),
      makeSrcToken(kD3DSPR_ADDR, 0),
      makeSrcToken(kD3DSPR_CONST, 1),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_RASTOUT, 0),
      makeSrcToken(kD3DSPR_CONST, 0),
      kD3DSIO_END,
  };
}

std::vector<u32> makeVs20IndexedConstDestinationBytecode() {
  using namespace dxmt9::d3d9bc;
  return {
      makeVersionToken(true, 2, 0),
      // mova a0.x, c0
      makeInstructionToken(kD3DSIO_MOVA, 2),
      makeDstToken(kD3DSPR_ADDR, 0, 0x1u),
      makeSrcToken(kD3DSPR_CONST, 0),
      // mov c[a0+5], c1
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeRelativeDstToken(kD3DSPR_CONST, 5),
      makeSrcToken(kD3DSPR_ADDR, 0),
      makeSrcToken(kD3DSPR_CONST, 1),
      // Keep the shader well-formed for the vertex translator.
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_RASTOUT, 0),
      makeSrcToken(kD3DSPR_CONST, 5),
      kD3DSIO_END,
  };
}

// vs_2_0 shader whose `def cN, ...` literal operands accidentally have
// bit 13 set in their float bit pattern. Without the per-opcode
// rel-addr inhibit, the parser would treat the literal as a register
// with relative addressing, consume the next DWORD as a rel-addr
// token, and shift every subsequent instruction (yielding garbage
// modifiers / opcodes downstream). Source modifier bits 24-27 of the
// shifted token frequently land on `0xF` — the "unsupported D3D
// source modifier 15" deterministic throw is the canonical surface.
std::vector<u32> makeVs20DefLiteralWithRelAddrBitBytecode() {
  using namespace dxmt9::d3d9bc;
  // Float literal bit pattern with bit 13 set: 0x3F802000 ≈ 1.001f.
  // Constructed so the literal cannot be confused with a register
  // token's rel-addr bit on a correct parser.
  constexpr u32 kFloatBit13Set = 0x3F802000u;
  return {
      makeVersionToken(true, 2, 0),
      // def c0, 1.001, 0, 0, 0  (operands 1..4 are float literals; op 1
      // has bit 13 set in its IEEE 754 bit pattern).
      makeInstructionToken(kD3DSIO_DEF, 5),
      makeDstToken(kD3DSPR_CONST, 0),
      kFloatBit13Set,
      0u,
      0u,
      0u,
      // mov oPos, c0 — the well-formed second instruction proves the
      // parser remained aligned across the DEF.
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_RASTOUT, 0),
      makeSrcToken(kD3DSPR_CONST, 0),
      kD3DSIO_END,
  };
}

// vs_2_0 shader that loads a bone index into a0 from v3 (BLENDINDICES)
// and reads a constant via `c[a0+5]` — the canonical D3D9 hardware
// skinning shape. Used to validate the parser consumes the rel-addr
// DWORD correctly and the emitter lowers indexed const access into
// `cFloat[clamp(a0.x + 5, 0, 255)]`.
std::vector<u32> makeVs20IndexedConstSourceBytecode() {
  using namespace dxmt9::d3d9bc;
  return {
      makeVersionToken(true, 2, 0),
      // dcl_blendindices v3
      makeInstructionToken(kD3DSIO_DCL, 2),
      makeDclSemanticToken(2u, 0u),
      makeDstToken(kD3DSPR_INPUT, 3),
      // mova a0, v3
      makeInstructionToken(kD3DSIO_MOVA, 2),
      makeDstToken(kD3DSPR_ADDR, 0, 0x1u),
      makeSrcToken(kD3DSPR_INPUT, 3),
      // mov r0, c[a0+5]
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_TEMP, 0),
      makeRelativeSrcToken(kD3DSPR_CONST, 5),
      makeSrcToken(kD3DSPR_ADDR, 0),
      // mov oPos, r0 — keeps the test bytecode well-formed for the
      // vertex translator's output-semantics validator.
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_RASTOUT, 0),
      makeSrcToken(kD3DSPR_TEMP, 0),
      kD3DSIO_END,
  };
}

// vs_3_0 matrix-palette skinning shape (3DMark05 GT1): two bone indices
// are loaded into a0.x and a0.y via `mova a0.xy, v1` and the two bone
// matrices are read via `c[a0.y + 0]` and `c[a0.x + 0]`. The address
// register a0 is a 4-component vector in D3D9; a translator that models
// it as a scalar (only a0.x) reads the wrong bone matrix for the a0.y
// term and the blended vertex flies off to a garbage position.
std::vector<u32> makeVs30RelAddrYComponentBytecode() {
  using namespace dxmt9::d3d9bc;
  return {
      makeVersionToken(true, 3, 0),
      // dcl_blendindices v1
      makeInstructionToken(kD3DSIO_DCL, 2),
      makeDclSemanticToken(2u, 0u),
      makeDstToken(kD3DSPR_INPUT, 1),
      // mova a0.xy, v1  (writes both a0.x and a0.y)
      makeInstructionToken(kD3DSIO_MOVA, 2),
      makeDstToken(kD3DSPR_ADDR, 0, 0x3u),
      makeSrcToken(kD3DSPR_INPUT, 1),
      // mov r0, c[a0.y + 0]  — rel-addr register selects the .y component
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_TEMP, 0),
      makeRelativeSrcToken(kD3DSPR_CONST, 0),
      makeSrcToken(kD3DSPR_ADDR, 0, 0x55u),
      // mov oPos, r0 — keep the bytecode well-formed for the vertex
      // translator's output-semantics validator.
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_RASTOUT, 0),
      makeSrcToken(kD3DSPR_TEMP, 0),
      kD3DSIO_END,
  };
}

std::vector<u32> makeVs30MovaWriteMaskBytecode() {
  using namespace dxmt9::d3d9bc;
  return {
      makeVersionToken(true, 3, 0),
      // dcl_blendindices v1
      makeInstructionToken(kD3DSIO_DCL, 2),
      makeDclSemanticToken(2u, 0u),
      makeDstToken(kD3DSPR_INPUT, 1),
      // mova a0.x, v1
      makeInstructionToken(kD3DSIO_MOVA, 2),
      makeDstToken(kD3DSPR_ADDR, 0, 0x1u),
      makeSrcToken(kD3DSPR_INPUT, 1),
      // mova a0.y, c0
      makeInstructionToken(kD3DSIO_MOVA, 2),
      makeDstToken(kD3DSPR_ADDR, 0, 0x2u),
      makeSrcToken(kD3DSPR_CONST, 0),
      // mov r0, c[a0.y + 0]
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_TEMP, 0),
      makeRelativeSrcToken(kD3DSPR_CONST, 0),
      makeSrcToken(kD3DSPR_ADDR, 0, 0x55u),
      // mov oPos, r0
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_RASTOUT, 0),
      makeSrcToken(kD3DSPR_TEMP, 0),
      kD3DSIO_END,
  };
}

std::vector<u32> makeVs30VertexTextureBytecode() {
  return {
      0xfffe0300,
      0x05000051, 0xa00f0000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
      0x0200001f, 0x80000000, 0x900f0000,
      0x0200001f, 0x90000000, 0xa00f0800,
      0x0200001f, 0x80000000, 0xe00f0000,
      0x0200001f, 0x8000000a, 0xe00f0001,
      0x0300005f, 0xe00f0001, 0xa0000000, 0xa0e40800,
      0x02000001, 0xe00f0000, 0x90e40000,
      0x0000ffff,
  };
}

std::vector<u32> makePs14ConstantClampBytecode() {
  return {
      0xffff0104,
      0x00000051, 0xa00f0002, 0xbf000000, 0x3fa00000, 0x40000000, 0x3f800000,
      0x00000001, 0x800f0001, 0xa0e40001,
      0x00000002, 0x800f0000, 0x80e40001, 0xa0e40002,
      0x0000ffff,
  };
}

void testD3DBCDecodeAndClassificationFixtures() {
  using namespace dxmt9::d3d9bc;
  namespace translator_test = dxmt9::translator::test;

  DrawDesc desc{};
  const auto module = translator_test::decodeD3DBytecodeForTest(makeShader(makePs30DecodeFixtureBytecode()),
                                                                false,
                                                                desc);

  checkEqual(module.stage, D3DShaderStage::Pixel, "D3DBC decode derives pixel stage from version token");
  checkEqual(module.major, 3u, "D3DBC decode preserves shader model major version");
  checkEqual(module.minor, 0u, "D3DBC decode preserves shader model minor version");
  checkEqual(module.instructions.size(), size_t{3}, "D3DBC decode skips comments and excludes END token");
  checkEqual(module.instructions[0].opcode, kD3DSIO_DCL, "D3DBC decode preserves first opcode after comment");
  checkEqual(module.instructions[1].opcode, kD3DSIO_TEXLDD, "D3DBC decode preserves texture opcode");
  checkEqual(module.instructions[1].operands.size(), size_t{5},
             "D3DBC decode uses fixed operand count for known texture opcodes");
  checkEqual(module.instructions[2].opcode, kD3DSIO_MOV, "D3DBC decode preserves predicated opcode");
  checkEqual(module.instructions[2].controls, 0x5au, "D3DBC decode preserves instruction controls");
  checkEqual(module.instructions[2].predicated, true, "D3DBC decode classifies predicated instructions");
  checkEqual(module.usesTexture, true, "D3DBC decode classifies texture-using opcodes");

  const u32 textureDst = module.instructions[1].operands[0];
  const u32 textureSrc = module.instructions[1].operands[1];
  const u32 samplerSrc = module.instructions[1].operands[2];
  const auto swizzle = translator_test::decodeSwizzleForTest(textureSrc);
  checkRegister(translator_test::decodeRegisterRefForTest(textureDst, module.stage), D3DRegisterKind::Temp, 3u,
                "D3DBC register classification decodes texture destination temp register");
  checkEqual(translator_test::decodeWriteMaskForTest(textureDst), 0x7u,
             "D3DBC operand decode preserves destination write mask");
  checkEqual(translator_test::decodeDestModifierForTest(textureDst), 1u,
             "D3DBC operand decode preserves destination modifier");
  checkRegister(translator_test::decodeRegisterRefForTest(textureSrc, module.stage), D3DRegisterKind::ConstFloat, 9u,
                "D3DBC register classification decodes source constant register");
  checkEqual(swizzle[0], u8{2}, "D3DBC operand decode preserves source swizzle x");
  checkEqual(swizzle[1], u8{1}, "D3DBC operand decode preserves source swizzle y");
  checkEqual(swizzle[2], u8{0}, "D3DBC operand decode preserves source swizzle z");
  checkEqual(swizzle[3], u8{3}, "D3DBC operand decode preserves source swizzle w");
  checkEqual(translator_test::decodeSourceModifierForTest(textureSrc), 11u,
             "D3DBC operand decode preserves source modifier");
  checkRegister(translator_test::decodeRegisterRefForTest(samplerSrc, module.stage), D3DRegisterKind::Sampler, 5u,
                "D3DBC register classification decodes sampler source register");

  const u32 boolSrc = module.instructions[2].operands[1];
  checkRegister(translator_test::decodeRegisterRefForTest(boolSrc, module.stage), D3DRegisterKind::ConstBool, 2u,
                "D3DBC register classification decodes bool constant source register");

  const u32 addressToken = makeSrcToken(kD3DSPR_ADDR, 4);
  checkRegister(translator_test::decodeRegisterRefForTest(addressToken, D3DShaderStage::Vertex),
                D3DRegisterKind::Address, 4u,
                "D3DBC register classification maps addr registers to address kind in vertex shaders");
  checkRegister(translator_test::decodeRegisterRefForTest(addressToken, D3DShaderStage::Pixel),
                D3DRegisterKind::Input, 4u,
                "D3DBC register classification maps addr register type to input kind in pixel shaders");
  checkEqual(translator_test::tokenHasRelativeAddressingForTest(makeRelativeDstToken(kD3DSPR_TEMP, 0)), true,
             "D3DBC operand decode classifies relative-addressing tokens");
}

void testPs30VFaceDecodeAndSourceContract() {
  using namespace dxmt9::d3d9bc;
  namespace translator_test = dxmt9::translator::test;

  DrawDesc desc{};
  const auto module = translator_test::decodeD3DBytecodeForTest(
      makeShader(makePs30VFaceBytecode()), false, desc);

  checkEqual(module.stage, D3DShaderStage::Pixel, "ps_3_0 vFace decode preserves pixel stage");
  checkEqual(module.instructions.size(), size_t{2}, "ps_3_0 vFace decode keeps DCL and MOV");
  checkRegister(translator_test::decodeRegisterRefForTest(module.instructions[0].operands[1], module.stage),
                D3DRegisterKind::MiscType, 1u,
                "ps_3_0 dcl vFace decodes as misc register one");
  checkRegister(translator_test::decodeRegisterRefForTest(module.instructions[1].operands[1], module.stage),
                D3DRegisterKind::MiscType, 1u,
                "ps_3_0 vFace source decodes as misc register one");

  const auto source = translatePixel(makePs30VFaceBytecode());
  checkContains(source, "vFace", "ps_3_0 vFace operand is preserved in source comments");
  checkContains(source, "// mov oC0, vFace", "ps_3_0 vFace source is preserved in source comments");
  checkContains(source, "bool frontFacing [[front_facing]]",
                "ps_3_0 vFace requests the Metal front-facing fragment input");
  checkContains(source, "outColor[0] = float4(frontFacing ? 1.0f : -1.0f);",
                "ps_3_0 vFace lowers to signed front-facing evidence");
}

void testPixelShaderOutputReceivesFixedFunctionFog() {
  // H224 — the D3D9 fog tail is a compile-time variant keyed on the draw's
  // resolved fog state (RS_FOG_ENABLE plus a table/vertex fog mode). The
  // fog-active variant keeps the historical runtime ffpPs.fogMode gate so
  // fog params stay uniform loads and never widen the PSO key.
  const auto source = translatePixelWithRenderStates(
      makePs20ColorInputBytecode(),
      {{RS_FOG_ENABLE, 1u},
       {RS_FOG_TABLE_MODE, static_cast<u32>(FogMode::Linear)}});
  checkContains(source, "if (ffpPs.fogMode != 0u)",
                "fog-active translated pixel shaders dynamically gate fixed-function fog");
  checkContains(source, "dxmt9_apply_fog(color, ffpPs, in.position.z, in.fogFactor)",
                "fog-active translated pixel shaders apply table fog after shader color output");
  checkContains(source, "outColor[0] = color",
                "fogged color is written back to the primary color output");
}

void testTranslatedFragmentTailsSingleAlphaVariantAndFogVariant() {
  // H228 — the alpha-test tail is a SINGLE shader variant: always emitted,
  // reading the per-draw FsVolatile immediate (fragment buffer 5) instead of
  // ffpPs, and identical across alpha-test render-state values so alpha-test
  // toggles never split the PSO. Fog stays the H224 compile-time variant.
  const auto inactive = translatePixel(makePs20ColorInputBytecode());
  checkContains(inactive, "constant FsVolatile& fsVolatile [[buffer(5)]]",
                "translated PS always declares the FsVolatile immediate param");
  checkContains(inactive, "if (fsVolatile.alphaTest != 0u)",
                "translated PS always emits the runtime alpha-test guard");
  checkContains(inactive, "switch (fsVolatile.alphaTest)",
                "translated PS alpha-test switch reads the immediate func");
  checkContains(inactive, "pass = color.a >= fsVolatile.alphaRef",
                "translated PS alpha-test compares against the immediate ref");
  checkContains(inactive, "discard_fragment()",
                "translated PS keeps the discard tail");
  checkNotContains(inactive, "ffpPs.alphaTestEnable",
                   "translated PS no longer reads alpha-test state from ffpPs");
  checkNotContains(inactive, "dxmt9_apply_fog(color, ffpPs",
                   "fog-inactive translated PS emits no fog tail call");
  checkNotContains(inactive, "FfpPsConsts& ffpPs [[buffer(3)]]",
                   "fog-inactive translated PS drops the unused FfpPsConsts param");

  const auto alphaOnly = translatePixelWithRenderStates(
      makePs20ColorInputBytecode(), {{RS_ALPHA_TEST_ENABLE, 1u}});
  checkEqual(alphaOnly, inactive,
             "alpha-test enable produces byte-identical translated fragment source (single variant)");

  const auto fogVertexOnly = translatePixelWithRenderStates(
      makePs20ColorInputBytecode(),
      {{RS_FOG_ENABLE, 1u},
       {RS_FOG_FROM_VERTEX, static_cast<u32>(FogMode::Linear)}});
  checkContains(fogVertexOnly,
                "dxmt9_apply_fog(color, ffpPs, in.position.z, in.fogFactor)",
                "vertex-fog draw state keeps the fog tail (mirrors the ffpPs.fogMode fallback)");
  checkContains(fogVertexOnly, "if (fsVolatile.alphaTest != 0u)",
                "fog-active PS keeps the single-variant alpha-test tail");

  // A fog mode without RS_FOG_ENABLE can never produce a non-zero
  // ffpPs.fogMode at upload time (buildFfpPsConsts zeroes it), so the
  // fog-disabled variant is safe.
  const auto fogModeWithoutEnable = translatePixelWithRenderStates(
      makePs20ColorInputBytecode(),
      {{RS_FOG_TABLE_MODE, static_cast<u32>(FogMode::Linear)}});
  checkNotContains(fogModeWithoutEnable, "dxmt9_apply_fog(color, ffpPs",
                   "fog mode without RS_FOG_ENABLE emits no fog tail");
}

void testTranslatedLegacyBumpEnvRetainsFfpPsWithoutTails() {
  // ps1.x TEXBEM/TEXBEML/BEM read ffpPs.bumpEnvMat/bumpEnvLum, so the
  // FfpPsConsts param must survive even when the fog tail is inactive.
  const auto source = translatePixel(makePs13LegacyTextureFamilyBytecode());
  checkContains(source, "constant FfpPsConsts& ffpPs [[buffer(3)]]",
                "legacy bump-env PS keeps the FfpPsConsts param without the fog tail");
  checkContains(source, "ffpPs.bumpEnvMat[0]",
                "legacy bump-env PS still reads the bump matrix");
  checkNotContains(source, "ffpPs.alphaTestEnable",
                   "legacy bump-env PS alpha test does not read ffpPs (H228 immediates)");
  checkContains(source, "if (fsVolatile.alphaTest != 0u)",
                "legacy bump-env PS keeps the single-variant alpha-test tail");
}

void testFfpFogTailIsCompileTimeGated() {
  // H224 — the FFP pixel key bakes a fog mode whenever a table/vertex fog
  // mode render state is set even while RS_FOG_ENABLE is off. The emitted
  // fog call is now additionally gated on the resolved could-apply
  // predicate; the FfpPsConsts deref stays because FFP stage machinery
  // (tfactor/stageConstants) reads it unconditionally.
  FfpPixelKey key{};
  key.fogMode = FogMode::Linear;
  DrawDesc desc{};
  desc.pixelShader.kind = ShaderRef::Kind::FixedFunctionPixel;
  desc.pixelShader.pixelKey = key;

  const auto inactive = dxmt9::ffp::makeFfpPixelSource(
      key, dxmt9::drawshader::makeShaderSourceContext(desc));
  checkNotContains(inactive, "dxmt9_apply_fog(color, ffpPs",
                   "fog-keyed FFP PS with fog-disabled draw state emits no fog call");
  checkContains(inactive, "ffpPs.textureFactor",
                "FFP PS keeps the FfpPsConsts deref for stage machinery");

  desc.rs.values[RS_FOG_ENABLE] = 1u;
  desc.rs.values[RS_FOG_TABLE_MODE] = static_cast<u32>(FogMode::Linear);
  const auto active = dxmt9::ffp::makeFfpPixelSource(
      key, dxmt9::drawshader::makeShaderSourceContext(desc));
  checkContains(active, "dxmt9_apply_fog(color, ffpPs, fogDepth",
                "fog-active FFP PS keeps the fog call");
}

void testLegacyShaderModelDecodeContracts() {
  using namespace dxmt9::d3d9bc;
  namespace translator_test = dxmt9::translator::test;

  DrawDesc desc{};
  const auto ps11 = translator_test::decodeD3DBytecodeForTest(
      makeShader(makePs11TexcoordTexBytecode()), false, desc);
  checkEqual(ps11.stage, D3DShaderStage::Pixel, "ps_1_1 decode preserves pixel stage");
  checkEqual(ps11.major, 1u, "ps_1_1 decode preserves major version");
  checkEqual(ps11.minor, 1u, "ps_1_1 decode preserves minor version");
  checkEqual(ps11.instructions.size(), size_t{3}, "ps_1_1 decode keeps legacy texture instruction stream aligned");
  checkEqual(ps11.instructions[0].opcode, kD3DSIO_TEXCOORD, "ps_1_1 TEXCOORD opcode decodes");
  checkEqual(ps11.instructions[0].operands.size(), size_t{1}, "ps_1_1 TEXCOORD has destination-stage operand");
  checkEqual(ps11.instructions[1].opcode, kD3DSIO_TEX, "ps_1_1 TEX opcode decodes");
  checkEqual(ps11.instructions[1].operands.size(), size_t{1}, "ps_1_1 TEX has destination-stage operand");
  checkEqual(ps11.usesTexture, true, "ps_1_1 TEX marks the module as texture-using");

  const auto ps13 = translator_test::decodeD3DBytecodeForTest(
      makeShader(makePs13LegacyTextureFamilyBytecode()), false, desc);
  checkEqual(ps13.stage, D3DShaderStage::Pixel, "ps_1_3 decode preserves pixel stage");
  checkEqual(ps13.major, 1u, "ps_1_3 decode preserves major version");
  checkEqual(ps13.minor, 3u, "ps_1_3 decode preserves minor version");
  checkEqual(ps13.instructions.size(), size_t{20}, "ps_1_3 decode keeps the legacy texture family aligned");
  checkEqual(ps13.instructions[2].opcode, kD3DSIO_TEXBEM, "ps_1_3 TEXBEM opcode decodes");
  checkEqual(ps13.instructions[2].operands.size(), size_t{2}, "ps_1_3 TEXBEM has dst/source operands");
  checkEqual(ps13.instructions[14].opcode, kD3DSIO_TEXM3x3SPEC, "ps_1_3 TEXM3x3SPEC opcode decodes");
  checkEqual(ps13.instructions[14].operands.size(), size_t{3}, "ps_1_3 TEXM3x3SPEC has dst/source/eye operands");
  checkEqual(ps13.instructions[18].opcode, kD3DSIO_TEXM3x2DEPTH, "ps_1_3 TEXM3x2DEPTH opcode decodes");
  checkEqual(ps13.instructions[18].operands.size(), size_t{2}, "ps_1_3 TEXM3x2DEPTH has dst/source operands");

  const auto ps14 = translator_test::decodeD3DBytecodeForTest(
      makeShader(makePs14TexcrdTexldDepthBytecode()), false, desc);
  checkEqual(ps14.instructions[0].opcode, kD3DSIO_TEXCOORD, "ps_1_4 TEXCRD opcode decodes");
  checkEqual(ps14.instructions[0].operands.size(), size_t{2}, "ps_1_4 TEXCRD has dst/source operands");
  checkEqual(ps14.instructions[1].opcode, kD3DSIO_TEX, "ps_1_4 TEXLD opcode decodes as TEX token");
  checkEqual(ps14.instructions[1].operands.size(), size_t{2}, "ps_1_4 TEXLD has dst/source operands");
  checkEqual(ps14.instructions[2].opcode, kD3DSIO_TEXDEPTH, "ps_1_4 TEXDEPTH opcode decodes");
  checkEqual(ps14.instructions[2].operands.size(), size_t{1}, "ps_1_4 TEXDEPTH has source operand");

  const auto vs11 = translator_test::decodeD3DBytecodeForTest(
      makeShader(makeVs11FixedFunctionOutputBytecode()), true, desc);
  checkEqual(vs11.stage, D3DShaderStage::Vertex, "vs_1_1 decode preserves vertex stage");
  checkEqual(vs11.major, 1u, "vs_1_1 decode preserves major version");
  checkEqual(vs11.minor, 1u, "vs_1_1 decode preserves minor version");
  checkEqual(vs11.instructions.size(), size_t{4}, "vs_1_1 decode keeps fixed-function output MOVs aligned");
  checkRegister(translator_test::decodeRegisterRefForTest(vs11.instructions[0].operands[0], vs11.stage),
                D3DRegisterKind::RastOut, 0u,
                "vs_1_1 output register oPos decodes as raster output zero");
  checkRegister(translator_test::decodeRegisterRefForTest(vs11.instructions[1].operands[0], vs11.stage),
                D3DRegisterKind::AttrOut, 0u,
                "vs_1_1 output register oD0 decodes as attribute output zero");
  checkRegister(translator_test::decodeRegisterRefForTest(vs11.instructions[3].operands[0], vs11.stage),
                D3DRegisterKind::TexCoordOut, 3u,
                "vs_1_1 output register oT3 decodes as texcoord output three");
}

void testPs30PredicatedInstructionLowersGuard() {
  const auto source = translatePixel(makePs30DecodeFixtureBytecode());
  checkContains(source, "bool p[16];", "predicated instruction declares predicate register storage");
  checkContains(source, "if ((p[0])) {", "predicated instruction lowers to a p0 guard");
  checkContains(source, "outColor[1] = (psConsts.psBoolConst[2] != 0u ? float4(1.0f) : float4(0.0f));",
                "predicated MOV body remains inside translated source");
}

void testD3DOpcodeNamesCoverUnsupportedSurface() {
  using namespace dxmt9::d3d9bc;
  namespace detail = dxmt9::translator::detail_;

  checkEqual(detail::opcodeName(kD3DSIO_BREAKC), std::string("breakc"), "BREAKC has a stable opcode name");
  checkEqual(detail::opcodeName(kD3DSIO_CALLNZ), std::string("callnz"), "CALLNZ has a stable opcode name");
  checkEqual(detail::opcodeName(kD3DSIO_TEXCOORD), std::string("texcoord"), "TEXCOORD has a stable opcode name");
  checkEqual(detail::opcodeName(kD3DSIO_TEXBEM), std::string("texbem"), "TEXBEM has a stable opcode name");
  checkEqual(detail::opcodeName(kD3DSIO_TEXBEML), std::string("texbeml"), "TEXBEML has a stable opcode name");
  checkEqual(detail::opcodeName(kD3DSIO_TEXREG2AR), std::string("texreg2ar"), "TEXREG2AR has a stable opcode name");
  checkEqual(detail::opcodeName(kD3DSIO_TEXREG2GB), std::string("texreg2gb"), "TEXREG2GB has a stable opcode name");
  checkEqual(detail::opcodeName(kD3DSIO_TEXM3x2PAD), std::string("texm3x2pad"),
             "TEXM3x2PAD has a stable opcode name");
  checkEqual(detail::opcodeName(kD3DSIO_TEXM3x2TEX), std::string("texm3x2tex"),
             "TEXM3x2TEX has a stable opcode name");
  checkEqual(detail::opcodeName(kD3DSIO_TEXM3x3PAD), std::string("texm3x3pad"),
             "TEXM3x3PAD has a stable opcode name");
  checkEqual(detail::opcodeName(kD3DSIO_TEXM3x3TEX), std::string("texm3x3tex"),
             "TEXM3x3TEX has a stable opcode name");
  checkEqual(detail::opcodeName(kD3DSIO_TEXM3x3DIFF), std::string("texm3x3diff"),
             "TEXM3x3DIFF has a stable opcode name");
  checkEqual(detail::opcodeName(kD3DSIO_TEXM3x3SPEC), std::string("texm3x3spec"),
             "TEXM3x3SPEC has a stable opcode name");
  checkEqual(detail::opcodeName(kD3DSIO_TEXM3x3VSPEC), std::string("texm3x3vspec"),
             "TEXM3x3VSPEC has a stable opcode name");
  checkEqual(detail::opcodeName(kD3DSIO_TEXREG2RGB), std::string("texreg2rgb"),
             "TEXREG2RGB has a stable opcode name");
  checkEqual(detail::opcodeName(kD3DSIO_TEXDP3TEX), std::string("texdp3tex"),
             "TEXDP3TEX has a stable opcode name");
  checkEqual(detail::opcodeName(kD3DSIO_TEXM3x2DEPTH), std::string("texm3x2depth"),
             "TEXM3x2DEPTH has a stable opcode name");
  checkEqual(detail::opcodeName(kD3DSIO_TEXDP3), std::string("texdp3"), "TEXDP3 has a stable opcode name");
  checkEqual(detail::opcodeName(kD3DSIO_TEXM3x3), std::string("texm3x3"), "TEXM3x3 has a stable opcode name");
}

void testPs20SamplerRegisterSlotMapping() {
  const auto source = translatePixel(makePs20TexturedBytecode(7));
  checkContains(source, "texture2d<float> tex7 [[texture(7)]]", "ps_2_0 declares texture at sampler register slot");
  checkContains(source, "sampler samp7 [[sampler(7)]]", "ps_2_0 declares sampler state at sampler register slot");
  checkContains(source, "tex7.sample(samp7", "ps_2_0 texture sample uses the declared sampler register slot");
}

void testPs20MipLodBiasEmitsShaderSideBias() {
  // D3DSAMP_MIPMAPLODBIAS (specs/d3d9/gap_d3d9.md B.3) is applied at sample time in MSL
  // via texture.sample(sampler, coord, bias(b)) — Metal's MTLSamplerDescriptor
  // has no LOD-bias field. The per-sampler bias rides a dedicated uniform
  // (`SamplerLodBias`) bound at fragment buffer slot 4. When the variant flag
  // is SET the translated fragment must (1) declare that uniform and (2) thread
  // its per-sampler value through bias() on every implicit-gradient (TEX)
  // sample.
  const auto source = translatePixel(makePs20TexturedBytecode(7), /*samplerLodBias=*/true);
  checkContains(source, "struct SamplerLodBias",
                "translated PS declares the per-sampler LOD-bias uniform struct");
  checkContains(source, "constant SamplerLodBias& samplerLodBias [[buffer(4)]]",
                "translated PS binds the LOD-bias uniform at fragment slot 4");
  checkContains(source, "tex7.sample(samp7",
                "ps_2_0 still samples through the declared sampler register slot");
  checkContains(source, "bias(samplerLodBias.bias[7])",
                "ps_2_0 TEX threads the per-sampler LOD bias into bias()");
}

void testPs20MipLodBiasClearOmitsShaderSideBias() {
  // specs/d3d9/gap_d3d9.md B.3 PSO-variant gating: when no active sampler carries a non-zero
  // LOD bias the variant flag is CLEAR, and the emitted MSL must be the
  // pre-feature form — NO slot-4 SamplerLodBias param and a plain
  // sample(sampler, coord) with no bias() argument. This keeps the common
  // no-bias draw off the per-draw slot-4 upload + bind.
  const auto source = translatePixel(makePs20TexturedBytecode(7), /*samplerLodBias=*/false);
  checkNotContains(source, "SamplerLodBias",
                   "bias-off translated PS omits the LOD-bias uniform entirely");
  checkNotContains(source, "buffer(4)",
                   "bias-off translated PS binds nothing at fragment slot 4");
  checkNotContains(source, "bias(",
                   "bias-off translated PS samples without a bias() argument");
  checkContains(source, "tex7.sample(samp7",
                "bias-off translated PS still samples through the sampler register slot");
}

void testFfpMipLodBiasEmitsShaderSideBias() {
  // Same specs/d3d9/gap_d3d9.md B.3 contract for the fixed-function pixel path: with the
  // variant flag SET a textured FFP stage threads its per-sampler LOD bias
  // through bias() on the stage sample. The `SamplerLodBias` uniform is
  // declared and bound at slot 4.
  FfpPixelKey key{};
  key.stages[0].colorOp = static_cast<u32>(TextureOp::SelectArg1);
  key.stages[0].colorArg1 = 2u;  // D3DTA_TEXTURE
  DrawDesc desc{};
  desc.pixelShader.kind = ShaderRef::Kind::FixedFunctionPixel;
  desc.pixelShader.pixelKey = key;
  desc.textures[0].handle = Handle{1u};
  auto context = dxmt9::drawshader::makeShaderSourceContext(desc);
  context.samplerLodBias = true;
  const auto source = dxmt9::ffp::makeFfpPixelSource(key, context);
  checkContains(source, "struct SamplerLodBias",
                "FFP PS declares the per-sampler LOD-bias uniform struct");
  checkContains(source, "constant SamplerLodBias& samplerLodBias [[buffer(4)]]",
                "FFP PS binds the LOD-bias uniform at fragment slot 4");
  checkContains(source, "tex0.sample(samp0",
                "FFP stage 0 samples its bound texture");
  checkContains(source, "bias(samplerLodBias.bias[0])",
                "FFP stage 0 threads the per-sampler LOD bias into bias()");
}

void testFfpMipLodBiasClearOmitsShaderSideBias() {
  // specs/d3d9/gap_d3d9.md B.3 PSO-variant gating, FFP path: when the variant flag is CLEAR
  // the textured FFP fragment must be the pre-feature form — no slot-4
  // SamplerLodBias param and a plain stage sample with no bias() argument.
  FfpPixelKey key{};
  key.stages[0].colorOp = static_cast<u32>(TextureOp::SelectArg1);
  key.stages[0].colorArg1 = 2u;  // D3DTA_TEXTURE
  DrawDesc desc{};
  desc.pixelShader.kind = ShaderRef::Kind::FixedFunctionPixel;
  desc.pixelShader.pixelKey = key;
  desc.textures[0].handle = Handle{1u};
  auto context = dxmt9::drawshader::makeShaderSourceContext(desc);
  context.samplerLodBias = false;
  const auto source = dxmt9::ffp::makeFfpPixelSource(key, context);
  checkNotContains(source, "SamplerLodBias",
                   "bias-off FFP PS omits the LOD-bias uniform entirely");
  checkNotContains(source, "buffer(4)",
                   "bias-off FFP PS binds nothing at fragment slot 4");
  checkNotContains(source, "bias(",
                   "bias-off FFP PS samples without a bias() argument");
  checkContains(source, "tex0.sample(samp0",
                "bias-off FFP PS still samples its bound texture");
}

void testTranslatedAlphaTestDebugStripOmitsTailDiscard() {
  const auto normal = translatePixelWithRenderStates(
      makePs20ColorInputBytecode(), {{RS_ALPHA_TEST_ENABLE, 1u}});
  checkContains(normal, "fsVolatile.alphaTest",
                "translated PS emits the FsVolatile runtime alpha-test guard");
  checkContains(normal, "discard_fragment()",
                "translated PS emits the alpha-test discard tail");
  checkContains(normal, "uint sampleMask [[sample_mask]]",
                "translated PS returns the D3D9 sample mask semantic");

  const auto stripped = translatePixelWithAlphaTestStrip(makePs20ColorInputBytecode());
  checkNotContains(stripped, "fsVolatile.alphaTest",
                   "alpha-test debug strip removes translated PS alpha-test guard");
  checkContains(stripped, "fsVolatile.sampleMask",
                "alpha-test debug strip preserves translated PS sample-mask output");
  checkNotContains(stripped, "discard_fragment()",
                   "alpha-test debug strip removes translated PS alpha-test discard");
}

void testFfpAlphaTestDebugStripOmitsDiscard() {
  // H228: the FFP alpha-test tail no longer keys on FfpPixelKey — it is a
  // single variant reading FsVolatile. The key's legacy alphaTestEnable bits
  // are still set here to prove they do not change the emitted source.
  FfpPixelKey key{};
  key.alphaTestEnable = true;
  key.alphaTestFunc = static_cast<u32>(CompareFunc::GreaterEqual);
  key.stages[0].colorOp = static_cast<u32>(TextureOp::SelectArg1);
  key.stages[0].colorArg1 = 2u;  // D3DTA_TEXTURE
  DrawDesc desc{};
  desc.pixelShader.kind = ShaderRef::Kind::FixedFunctionPixel;
  desc.pixelShader.pixelKey = key;
  desc.textures[0].handle = Handle{1u};
  auto context = dxmt9::drawshader::makeShaderSourceContext(desc);

  const auto normal = dxmt9::ffp::makeFfpPixelSource(key, context);
  checkContains(normal, "constant FsVolatile& fsVolatile [[buffer(5)]]",
                "FFP PS declares the FsVolatile immediate param");
  checkContains(normal, "switch (fsVolatile.alphaTest)",
                "FFP PS emits the FsVolatile alpha-test function switch");
  checkContains(normal, "discard_fragment()",
                "FFP PS emits alpha-test discard");
  checkContains(normal, "uint sampleMask [[sample_mask]]",
                "FFP PS returns the D3D9 sample mask semantic");
  checkNotContains(normal, "ffpPs.alphaTestFunc",
                   "FFP PS no longer reads alpha-test state from ffpPs");

  auto keyWithoutAlpha = key;
  keyWithoutAlpha.alphaTestEnable = false;
  keyWithoutAlpha.alphaTestFunc = 0u;
  const auto sameVariant = dxmt9::ffp::makeFfpPixelSource(keyWithoutAlpha, context);
  checkEqual(sameVariant, normal,
             "FfpPixelKey alpha-test bits do not change the emitted FFP source (single variant)");

  context.stripAlphaTestForDebug = true;
  const auto stripped = dxmt9::ffp::makeFfpPixelSource(key, context);
  checkNotContains(stripped, "fsVolatile.alphaTest",
                   "alpha-test debug strip removes FFP alpha-test guard");
  checkContains(stripped, "fsVolatile.sampleMask",
                "alpha-test debug strip preserves FFP sample-mask output");
  checkNotContains(stripped, "discard_fragment()",
                   "alpha-test debug strip removes FFP alpha-test discard");
}

void testPs30InputSemanticTexcoordMapping() {
  const auto source = translatePixel(makePs30InputSemanticBytecode());
  checkContains(source, "in.texcoord3",
                "ps_3_0 dcl_texcoord semantic index maps input register reads by semantic index");
  checkContains(source, "tex2.sample(samp2", "ps_3_0 texture sample preserves sampler register mapping");
}

void testPairLocalVaryingLivenessKeepsHighTexcoordAndFog() {
  const auto shader = makeShader(makePs30HighTexcoordInputBytecode());
  DrawDesc desc{};
  desc.pixelShader = shader;
  auto context = dxmt9::drawshader::makeShaderSourceContext(desc);
  const auto layout =
      dxmt9::translator::collectTranslatedFragmentVaryingLiveness(shader, context);

  check((layout.texcoordMask & (1u << 7u)) != 0u,
        "FS liveness keeps texcoord7 when the pixel shader reads it");
  check((layout.texcoordMask & (1u << 5u)) == 0u,
        "FS liveness does not keep unrelated texcoord5");
  check(layout.fogFactor,
        "translated fragment liveness keeps fogFactor for the emitted D3D9 fog tail");

  context.vsOutLayout = layout;
  const auto source = dxmt9::translator::makeTranslatedFragmentSource(shader, context);
  checkContains(source, "float4 texcoord7;",
                "pair-local VSOut declares the high texcoord read by FS");
  checkNotContains(source, "float4 texcoord5;",
                   "pair-local VSOut trims unrelated high texcoords");
  checkContains(source, "float fogFactor",
                "pair-local VSOut keeps fogFactor for the emitted fog tail");
  checkContains(source, "in.texcoord7",
                "fragment source reads the preserved high texcoord");
}

void testPs30TexkillLoweringContract() {
  const auto source = translatePixel(makePs30TexkillBytecode());
  checkContains(source, "// texkill r0", "ps_3_0 TEXKILL is named in emitted instruction comments");
  checkContains(source, "if ((r[0]).x < 0.0f || (r[0]).y < 0.0f)",
                "ps_3_0 TEXKILL lowers selected components to a negative-value discard predicate");
  checkContains(source, "discard_fragment()", "ps_3_0 TEXKILL lowers to Metal fragment discard");
  checkContains(source, "outColor[0] = dxmt9_cdef0", "ps_3_0 TEXKILL does not terminate subsequent translation");
}

void testPs20ColorInputUsesLegacyInputMapping() {
  const auto source = translatePixel(makePs20ColorInputBytecode());
  checkContains(source, "outColor[0] = float4(in.color);",
                "ps_2_0 v0 input maps to interpolated color");
  checkNotContains(source, "outColor[0] = in.position;",
                   "ps_2_0 v0 input must not be treated as fragment position");

  const auto secondarySource = translatePixel(makePs20ColorInputBytecode(1));
  checkContains(secondarySource, "outColor[0] = float4(in.secondaryColor);",
                "ps_2_0 v1 input maps to interpolated secondary color");
  checkNotContains(secondarySource, "dxmt9_select_texcoord(in, 0u)",
                   "ps_2_0 v1 input must not be treated as texture coordinate zero");
}

void testVs30OutputSemanticMappingBySemanticIndex() {
  const auto source = translateVertex(makeVs30OutputSemanticBytecode());
  checkContains(source, "outPosition = vsConsts.vsFloatConst[0]", "vs_3_0 position semantic maps to Metal position output");
  checkContains(source, "outTexcoord[2] = vsConsts.vsFloatConst[1]",
                "vs_3_0 texcoord semantic maps by semantic index rather than o-register index");
  checkContains(source, "outSecondaryColor = vsConsts.vsFloatConst[2]", "vs_3_0 color1 semantic maps to secondary color output");
  checkNotContains(source, "outTexcoord[1] = vsConsts.vsFloatConst[1]",
                   "vs_3_0 texcoord semantic does not fall back to raw output register index");
}

void testVsTempTrimIsOptInAndUsesObservedTempRange() {
  {
    const ScopedUnsetEnv noTrim("DXMT9_TRIM_VERTEX_TEMPS");
    const auto source = translateVertex(makeVs30OutputSemanticBytecode());
    checkContains(source, "float4 r[32];",
                  "vertex temp array remains conservative by default");
  }
  {
    const ScopedSetEnv trim("DXMT9_TRIM_VERTEX_TEMPS", "1");
    const auto noTempSource = translateVertex(makeVs30OutputSemanticBytecode());
    checkContains(noTempSource, "float4 r[1];",
                  "vertex temp trim keeps one slot when the shader has no temp use");
    checkNotContains(noTempSource, "float4 r[32];",
                     "vertex temp trim removes the conservative 32-slot array");

    const auto tempFiveSource = translateVertex(makeVs30TempFiveOutputBytecode());
    checkContains(tempFiveSource, "float4 r[6];",
                  "vertex temp trim sizes through the highest observed temp source/dest");
    checkContains(tempFiveSource, "r[5]",
                  "trimmed vertex temp source still references the highest used temp");
  }
}

void testVsOutputScratchTrimIsOptInAndUsesObservedOutputRange() {
  {
    const ScopedUnsetEnv noTrim("DXMT9_TRIM_VS_OUTPUT_SCRATCH");
    const auto source = translateVertex(makeVs30TempFiveOutputBytecode());
    checkContains(source, "float4 outTexcoord[8];",
                  "vertex output texcoord scratch remains conservative by default");
  }
  {
    const ScopedSetEnv trim("DXMT9_TRIM_VS_OUTPUT_SCRATCH", "1");
    const auto minimalLayout = dxmt9::shaders::minimalVSOutLayout();
    const auto noTexcoordSource =
        translateVertexWithVSOutLayout(makeVs30TempFiveOutputBytecode(), minimalLayout);
    checkContains(noTexcoordSource, "float4 outTexcoord[1];",
                  "vertex output scratch trim keeps one slot when no texcoord output is used");
    checkNotContains(noTexcoordSource, "float4 outTexcoord[8];",
                     "vertex output scratch trim removes the conservative 8-slot array");

    const auto texcoordTwoSource =
        translateVertexWithVSOutLayout(makeVs30OutputSemanticBytecode(), minimalLayout);
    checkContains(texcoordTwoSource, "float4 outTexcoord[3];",
                  "vertex output scratch trim sizes through the highest mapped texcoord semantic");
    checkContains(texcoordTwoSource, "outTexcoord[2] = vsConsts.vsFloatConst[1]",
                  "trimmed vertex output scratch still references the highest mapped texcoord");
  }
}

void testVsOutTrimHashAllowlistScopesPairLiveness() {
  const ScopedSetEnv trim("DXMT9_TRIM_UNUSED_VARYINGS", "1");
  const auto vertex = makeShader(makeVs30OutputSemanticBytecode());
  const auto pixel = makeShader(makePs20ColorInputBytecode());

  DrawDesc desc{};
  desc.vertexShader = vertex;
  desc.pixelShader = pixel;
  auto context = dxmt9::drawshader::makeShaderSourceContext(desc);

  {
    const ScopedSetEnv nonMatchingVs("DXMT9_TRIM_UNUSED_VARYINGS_VS_HASHES",
                                     "0x1234");
    const auto layout = dxmt9::drawshader::resolveVSOutLayoutForShaderPair(context);
    checkEqual(dxmt9::shaders::vsoutLayoutKey(layout), 0xfffu,
               "non-matching VS trim allowlist keeps full VSOut");
  }

  {
    const auto matching = std::to_string(vertex.hash);
    const ScopedSetEnv matchingVs("DXMT9_TRIM_UNUSED_VARYINGS_VS_HASHES",
                                  matching.c_str());
    const ScopedSetEnv nonMatchingPs("DXMT9_TRIM_UNUSED_VARYINGS_PS_HASHES",
                                     "0x5678");
    const auto layout = dxmt9::drawshader::resolveVSOutLayoutForShaderPair(context);
    checkEqual(dxmt9::shaders::vsoutLayoutKey(layout), 0xfffu,
               "non-matching PS trim allowlist keeps full VSOut");
  }

  {
    const auto matchingVsHash = std::to_string(vertex.hash);
    const auto matchingPsHash = std::to_string(pixel.hash);
    const ScopedSetEnv matchingVs("DXMT9_TRIM_UNUSED_VARYINGS_VS_HASHES",
                                  matchingVsHash.c_str());
    const ScopedSetEnv matchingPs("DXMT9_TRIM_UNUSED_VARYINGS_PS_HASHES",
                                  matchingPsHash.c_str());
    const auto layout = dxmt9::drawshader::resolveVSOutLayoutForShaderPair(context);
    check(dxmt9::shaders::vsoutLayoutKey(layout) != 0xfffu,
          "matching VS/PS trim allowlist enables pair-local VSOut trim");
    check(layout.color, "color-input PS keeps color varying");
    check(!layout.secondaryColor, "color-input PS drops unread secondary color");
    check(layout.fogFactor, "translated PS tail keeps fogFactor type-checked");
  }
}

void testVsOutPointSizeProbeDropsOnlyPointSize() {
  {
    const ScopedUnsetEnv noProbe("DXMT9_PROBE_DROP_VSOUT_POINT_SIZE");
    const auto layout = dxmt9::shaders::applyVSOutProbeOverrides(
        dxmt9::shaders::fullVSOutLayout());
    check(layout.pointSize, "VSOut point-size probe is off by default");
    checkEqual(dxmt9::shaders::vsoutLayoutKey(layout), 0xfffu,
               "default full VSOut layout key includes pointSize");
  }

  {
    const ScopedSetEnv probe("DXMT9_PROBE_DROP_VSOUT_POINT_SIZE", "1");
    const auto layout = dxmt9::shaders::applyVSOutProbeOverrides(
        dxmt9::shaders::fullVSOutLayout());
    check(!layout.pointSize, "VSOut point-size probe drops pointSize");
    check(layout.color, "VSOut point-size probe keeps color");
    check(layout.secondaryColor, "VSOut point-size probe keeps secondaryColor");
    check(layout.fogFactor, "VSOut point-size probe keeps fogFactor");
    checkEqual(layout.texcoordMask, 0xffu, "VSOut point-size probe keeps all texcoords");
    checkEqual(dxmt9::shaders::vsoutLayoutKey(layout), 0x7ffu,
               "VSOut point-size probe clears only the pointSize layout bit");

    dxmt9::shaders::ShaderPreludeOptions options{};
    options.vsOutLayout = layout;
    const auto prelude = dxmt9::shaders::makeShaderPrelude(options);
    checkNotContains(prelude, "[[point_size]]",
                     "VSOut point-size probe removes Metal point_size attribute");
    checkContains(prelude, "float fogFactor", "VSOut point-size probe leaves fogFactor");
    checkContains(prelude, "float4 texcoord0", "VSOut point-size probe leaves texcoord0");
    checkContains(prelude, "float4 texcoord7", "VSOut point-size probe leaves texcoord7");
  }
}

void testVsOutHalfProbeNarrowsOnlyUserVaryings() {
  {
    const ScopedUnsetEnv noProbe("DXMT9_PROBE_HALF_VSOUT");
    check(!dxmt9::shaders::vsoutProbeHalfEnabled(),
          "VSOut half probe is off by default");

    dxmt9::shaders::ShaderPreludeOptions options{};
    const auto prelude = dxmt9::shaders::makeShaderPrelude(options);
    checkContains(prelude, "return in.texcoord0;",
                  "default VSOut helper keeps the existing float texcoord source shape");
    checkNotContains(prelude, "return float4(in.texcoord0);",
                     "default VSOut helper does not add half-probe casts");
  }

  {
    const ScopedSetEnv probe("DXMT9_PROBE_HALF_VSOUT", "1");
    check(dxmt9::shaders::vsoutProbeHalfEnabled(),
          "VSOut half probe reads the opt-in env flag");

    dxmt9::shaders::ShaderPreludeOptions options{};
    options.halfVSOut = true;
    const auto prelude = dxmt9::shaders::makeShaderPrelude(options);
    checkContains(prelude, "float4 position [[position]]",
                  "VSOut half probe keeps position float4");
    checkContains(prelude, "half4 color",
                  "VSOut half probe narrows color");
    checkContains(prelude, "half4 secondaryColor",
                  "VSOut half probe narrows secondaryColor");
    checkContains(prelude, "half4 texcoord0",
                  "VSOut half probe narrows texcoords");
    checkContains(prelude, "half fogFactor",
                  "VSOut half probe narrows fogFactor");
    checkContains(prelude, "float pointSize [[point_size]]",
                  "VSOut half probe keeps Metal point_size float");
    checkContains(prelude, "return float4(in.texcoord0)",
                  "VSOut half probe casts stage-in texcoords back to float for FS code");

    const auto pixelSource = translatePixel(makePs14TexcrdTexldDepthBytecode());
    checkContains(pixelSource, "r[0] = float4(in.texcoord0);",
                  "VSOut half probe casts direct pixel texcoord inputs back to float");
  }
}

void testVertexDepthOutThrowsDeterministically() {
  checkThrowsContains([] { translateVertex(makeVs20DepthOutBytecode()); },
                      "vertex depth output register is invalid",
                      "D3D9 vertex shaders must not map oDepth to clip-space position.z");
}

void testVs30HighOutputRegisterSemanticMapping() {
  constexpr u32 kD3DDeclTypeFloat2 = 1u;
  constexpr u32 kD3DDeclTypeFloat4 = 3u;
  constexpr u32 kD3DDeclMethodDefault = 0u;
  constexpr u32 kD3DDeclUsagePosition = 0u;
  constexpr u32 kD3DDeclUsageTexcoord = 5u;

  const std::vector<VertexElement> elements{
      VertexElement{0, 0, kD3DDeclTypeFloat4, kD3DDeclMethodDefault, kD3DDeclUsagePosition, 0},
      VertexElement{1, 12, kD3DDeclTypeFloat2, kD3DDeclMethodDefault, kD3DDeclUsageTexcoord, 0},
      VertexElement{1, 20, kD3DDeclTypeFloat2, kD3DDeclMethodDefault, kD3DDeclUsageTexcoord, 1},
  };
  std::array<u32, kMaxStreams> strides{};
  strides[0] = 16u;
  strides[1] = 28u;

  const auto source = translateVertex(
      makeVs30HighRegisterOutputSemanticBytecode(), elements, strides);

  checkContains(source, "device const uchar* stream1 [[buffer(6)]]",
                "high-output VS keeps stream1 on its own Metal buffer slot");
  checkContains(source, "const uint stride1 = 28u",
                "high-output VS preserves stream1 stride at the shader boundary");
  checkContains(source, "dxmt9_load_f32x4(stream0, base + 0u)",
                "high-output VS loads POSITION from stream0");
  checkContains(source, "dxmt9_load_f32x2(stream1, base1 + 12u)",
                "high-output VS loads TEXCOORD0 from stream1 offset 12");
  checkContains(source, "dxmt9_load_f32x2(stream1, base1 + 20u)",
                "high-output VS loads TEXCOORD1 from stream1 offset 20");
  checkContains(source, "outPosition = vin0",
                "POSITION semantic maps to Metal position despite high o-register index");
  checkContains(source, "outTexcoord[0] = vin1",
                "TEXCOORD0 semantic maps by semantic index, not output register 3");
  checkContains(source, "outTexcoord[1] = vin2",
                "TEXCOORD1 semantic maps by semantic index, not output register 4");
  checkNotContains(source, "outTexcoord[3] = vin1",
                   "TEXCOORD0 must not fall back to raw output register 3");
  checkNotContains(source, "outTexcoord[4] = vin2",
                   "TEXCOORD1 must not fall back to raw output register 4");
}

void testVs30VertexDeclarationTypeLoads() {
  constexpr u32 kD3DDeclTypeFloat3 = 2u;
  constexpr u32 kD3DDeclTypeUByte4 = 5u;
  constexpr u32 kD3DDeclTypeShort4N = 10u;
  constexpr u32 kD3DDeclTypeFloat16_2 = 15u;
  constexpr u32 kD3DDeclMethodDefault = 0u;
  constexpr u32 kD3DDeclUsagePosition = 0u;
  constexpr u32 kD3DDeclUsageBlendWeight = 1u;
  constexpr u32 kD3DDeclUsageBlendIndices = 2u;
  constexpr u32 kD3DDeclUsageTexcoord = 5u;

  const auto source = translateVertex(
      makeVs30InputSemanticBytecode(),
      {
          VertexElement{0, 0, kD3DDeclTypeFloat3, kD3DDeclMethodDefault, kD3DDeclUsagePosition, 0},
          VertexElement{0, 12, kD3DDeclTypeShort4N, kD3DDeclMethodDefault, kD3DDeclUsageBlendWeight, 0},
          VertexElement{0, 20, kD3DDeclTypeUByte4, kD3DDeclMethodDefault, kD3DDeclUsageBlendIndices, 0},
          VertexElement{0, 24, kD3DDeclTypeFloat16_2, kD3DDeclMethodDefault, kD3DDeclUsageTexcoord, 0},
      },
      [] {
        std::array<u32, kMaxStreams> strides{};
        strides[0] = 28u;
        return strides;
      }());

  checkContains(source, "dxmt9_load_f32x3(stream0, base + 0u)", "POSITION float3 input loads from stream");
  checkContains(source, "dxmt9_load_i16x4_snorm(stream0, base + 12u)",
                "SHORT4N blend weights are normalized instead of left zero");
  checkContains(source, "dxmt9_load_u8x4(stream0, base + 20u)",
                "UBYTE4 blend indices are loaded instead of left zero");
  checkContains(source, "dxmt9_load_f16x2(stream0, base + 24u)",
                "FLOAT16_2 texcoord is loaded instead of left zero");
  checkContains(source,
                "const uint stride = drawVolatile.vertexStreamStride != 0u ? drawVolatile.vertexStreamStride : 28u",
                "declared stream stride participates in shader input fetch");
}

void testVs30VertexDeclarationUDec3Load() {
  constexpr u32 kD3DDeclTypeFloat3 = 2u;
  constexpr u32 kD3DDeclTypeUDec3 = 13u;
  constexpr u32 kD3DDeclMethodDefault = 0u;
  constexpr u32 kD3DDeclUsagePosition = 0u;
  constexpr u32 kD3DDeclUsageTexcoord = 5u;

  const auto source = translateVertex(
      makeVs30InputSemanticBytecode(),
      {
          VertexElement{0, 0, kD3DDeclTypeFloat3, kD3DDeclMethodDefault, kD3DDeclUsagePosition, 0},
          VertexElement{0, 12, kD3DDeclTypeUDec3, kD3DDeclMethodDefault, kD3DDeclUsageTexcoord, 0},
      },
      [] {
        std::array<u32, kMaxStreams> strides{};
        strides[0] = 16u;
        return strides;
      }());

  checkContains(source, "dxmt9_load_udec3(stream0, base + 12u)",
                "UDEC3 vertex declaration type loads through the dedicated unpack helper");
  checkContains(source, "outTexcoord[2] = vin3",
                "UDEC3 TEXCOORD input remains visible through the programmable VS output");
}

void testVs30MaterializesOnlyReadInputs() {
  constexpr u32 kD3DDeclTypeFloat2 = 1u;
  constexpr u32 kD3DDeclTypeFloat4 = 3u;
  constexpr u32 kD3DDeclMethodDefault = 0u;
  constexpr u32 kD3DDeclUsagePosition = 0u;
  constexpr u32 kD3DDeclUsageTexcoord = 5u;

  const auto source = translateVertex(
      makeVs30SparseInputReadBytecode(),
      {
          VertexElement{0, 0, kD3DDeclTypeFloat4, kD3DDeclMethodDefault, kD3DDeclUsagePosition, 0},
          VertexElement{0, 16, kD3DDeclTypeFloat2, kD3DDeclMethodDefault, kD3DDeclUsageTexcoord, 0},
      },
      [] {
        std::array<u32, kMaxStreams> strides{};
        strides[0] = 24u;
        return strides;
      }());

  checkNotContains(source, "float4 vin[16];",
                   "programmable VS does not materialize the full input register file");
  checkContains(source, "float4 vin7;",
                "programmable VS materializes the sparse input register that is actually read");
  checkContains(source, "vin7 = float4(dxmt9_load_f32x2(stream0, base + 16u), 0.0f, 1.0f);",
                "programmable VS loads the sparse read input from the declaration binding");
  checkContains(source, "outTexcoord[0] = vin7;",
                "sparse input local remains visible to VS output mapping");
  checkNotContains(source, "vin0",
                   "declared but unread input registers are not materialized");
}

void testVs30InputLayoutPreservesStreamBoundaries() {
  constexpr u32 kD3DDeclTypeFloat2 = 1u;
  constexpr u32 kD3DDeclTypeFloat3 = 2u;
  constexpr u32 kD3DDeclTypeUByte4 = 5u;
  constexpr u32 kD3DDeclTypeShort4N = 10u;
  constexpr u32 kD3DDeclMethodDefault = 0u;
  constexpr u32 kD3DDeclUsagePosition = 0u;
  constexpr u32 kD3DDeclUsageBlendWeight = 1u;
  constexpr u32 kD3DDeclUsageBlendIndices = 2u;
  constexpr u32 kD3DDeclUsageTexcoord = 5u;

  const std::vector<VertexElement> elements{
      VertexElement{0, 0, kD3DDeclTypeFloat3, kD3DDeclMethodDefault, kD3DDeclUsagePosition, 0},
      VertexElement{1, 0, kD3DDeclTypeShort4N, kD3DDeclMethodDefault, kD3DDeclUsageBlendWeight, 0},
      VertexElement{1, 8, kD3DDeclTypeUByte4, kD3DDeclMethodDefault, kD3DDeclUsageBlendIndices, 0},
      VertexElement{1, 12, kD3DDeclTypeFloat2, kD3DDeclMethodDefault, kD3DDeclUsageTexcoord, 0},
  };
  std::array<u32, kMaxStreams> strides{};
  strides[0] = 12u;
  strides[1] = 20u;

  const auto layout = decodeVertexInputLayout(makeVs30InputSemanticBytecode(), elements, strides);
  checkEqual(layout.stride, 12u, "stream0 stride remains the DrawVolatile fallback stride");
  checkEqual(layout.streamStrides[0], 12u, "stream0 stride captured in the input layout");
  checkEqual(layout.streamStrides[1], 20u, "stream1 stride captured in the input layout");
  checkEqual(layout.streamMask, (1u << 0u) | (1u << 1u),
             "input layout records every stream used by DCL-bound semantics");
  checkEqual(layout.inputs[0].stream, 0u, "POSITION input remains on stream0");
  checkEqual(layout.inputs[0].offset, 0u, "POSITION input offset");
  checkEqual(layout.inputs[0].type, kD3DDeclTypeFloat3, "POSITION input type");
  checkEqual(layout.inputs[1].stream, 1u, "BLENDWEIGHT input records stream1");
  checkEqual(layout.inputs[2].offset, 8u, "BLENDINDICES input offset records stream1 offset");
  checkEqual(layout.inputs[3].stream, 1u, "TEXCOORD input records stream1");
  checkEqual(layout.inputs[3].offset, 12u, "TEXCOORD input records stream1 offset");

  auto changedStride = strides;
  changedStride[1] = 24u;
  const auto strideChanged =
      decodeVertexInputLayout(makeVs30InputSemanticBytecode(), elements, changedStride);
  check(strideChanged.hash != layout.hash,
        "input-layout hash changes when a nonzero stream stride changes");

  auto movedElements = elements;
  movedElements[3].stream = 0u;
  movedElements[3].offset = 12u;
  const auto movedLayout =
      decodeVertexInputLayout(makeVs30InputSemanticBytecode(), movedElements, strides);
  checkEqual(movedLayout.inputs[3].stream, 0u,
             "input layout follows a semantic moved back to stream0");
  check(movedLayout.hash != layout.hash,
        "input-layout hash changes when a semantic crosses stream boundaries");
}

void testVs30MultiStreamVertexDeclarationLoads() {
  constexpr u32 kD3DDeclTypeFloat2 = 1u;
  constexpr u32 kD3DDeclTypeFloat3 = 2u;
  constexpr u32 kD3DDeclTypeUByte4 = 5u;
  constexpr u32 kD3DDeclTypeShort4N = 10u;
  constexpr u32 kD3DDeclMethodDefault = 0u;
  constexpr u32 kD3DDeclUsagePosition = 0u;
  constexpr u32 kD3DDeclUsageBlendWeight = 1u;
  constexpr u32 kD3DDeclUsageBlendIndices = 2u;
  constexpr u32 kD3DDeclUsageTexcoord = 5u;

  const std::vector<VertexElement> elements{
      VertexElement{0, 0, kD3DDeclTypeFloat3, kD3DDeclMethodDefault, kD3DDeclUsagePosition, 0},
      VertexElement{1, 0, kD3DDeclTypeShort4N, kD3DDeclMethodDefault, kD3DDeclUsageBlendWeight, 0},
      VertexElement{1, 8, kD3DDeclTypeUByte4, kD3DDeclMethodDefault, kD3DDeclUsageBlendIndices, 0},
      VertexElement{1, 12, kD3DDeclTypeFloat2, kD3DDeclMethodDefault, kD3DDeclUsageTexcoord, 0},
  };
  std::array<u32, kMaxStreams> strides{};
  strides[0] = 12u;
  strides[1] = 20u;

  const auto source = translateVertex(makeVs30InputSemanticBytecode(), elements, strides);

  checkContains(source, "device const uchar* stream0 [[buffer(1)]]",
                "stream0 keeps the legacy VS buffer slot");
  checkContains(source, "device const uchar* stream1 [[buffer(6)]]",
                "stream1 gets its own direct VS buffer slot");
  checkContains(source, "const uint stride1 = 20u",
                "stream1 declared stride is preserved at the shader boundary");
  checkContains(source, "uint iid [[instance_id]]",
                "programmable vertex entry exposes Metal instance_id");
  checkContains(source,
                "drawVolatile.streamInstanceDivisors[1] != 0u ? iid / drawVolatile.streamInstanceDivisors[1] : uint(vertexIndex)",
                "stream1 selects per-instance or per-vertex element indexing at draw time");
  checkContains(source, "dxmt9_load_f32x3(stream0, base + 0u)",
                "POSITION remains loaded from stream0");
  checkContains(source, "dxmt9_load_i16x4_snorm(stream1, base1 + 0u)",
                "BLENDWEIGHT declared on stream1 loads from stream1");
  checkContains(source, "dxmt9_load_u8x4(stream1, base1 + 8u)",
                "BLENDINDICES declared on stream1 loads from stream1");
  checkContains(source, "dxmt9_load_f32x2(stream1, base1 + 12u)",
                "TEXCOORD declared on stream1 loads from stream1");
  checkNotContains(source, "dxmt9_load_f32x2(stream0, base + 12u)",
                   "stream1 TEXCOORD must not be fetched from stream0");

  const auto argbufSource = translateVertex(
      makeVs30InputSemanticBytecode(), elements, strides, /*argbufHybridMode=*/true);
  checkContains(argbufSource, "[[buffer(30)]]",
                "argbuf-hybrid VS still binds the argbuf at slot 30");
  checkContains(argbufSource, "device const uchar* stream1 [[buffer(6)]]",
                "argbuf-hybrid VS keeps stream1 on a direct stream buffer slot");
  checkContains(argbufSource, "dxmt9_load_f32x2(stream1, base1 + 12u)",
                "argbuf-hybrid VS loads stream1 attributes from stream1");
}

void testDefaultNoPixelVFlipAndNoVertexYFlip() {
  const auto pixelSource = translatePixel(makePs20TexturedBytecode(0));
  const auto vertexSource = translateVertex(makeVs30OutputSemanticBytecode());
  checkNotContains(pixelSource, "1.0f -", "default pixel texture sampling does not flip V");
  checkNotContains(vertexSource, "out.position.y = -out.position.y", "default vertex output does not flip clip-space Y");
}

void testPs30WriteMaskSwizzleAndSourceModifiers() {
  const auto source = translatePixel(makePs30WriteMaskSwizzleModifierBytecode());
  checkContains(source, "r[0] = float4(psConsts.psFloatConst[0].w, psConsts.psFloatConst[0].z, psConsts.psFloatConst[0].y, psConsts.psFloatConst[0].x);",
                "ps_3_0 source swizzle is preserved in MOV transform");
  checkContains(source, "r[1] = dxmt9_merge(r[1],", "ps_3_0 partial write mask uses masked merge");
  checkContains(source, "-(float4(r[0].y, r[0].x, r[0].w, r[0].z))",
                "ps_3_0 negate source modifier wraps swizzled temp source");
  checkContains(source, "abs(psConsts.psFloatConst[1])", "ps_3_0 abs source modifier wraps constant source");
  checkContains(source, ", 5u);", "ps_3_0 xz write mask is emitted as deterministic mask token");
  checkContains(source, "outColor[0] = r[1];", "ps_3_0 transformed masked result reaches color output");
}

void testPs30MissingSourceModifierCoverage() {
  const auto source = translatePixel(makePs30SourceModifierCoverageBytecode());
  checkContains(source, "r[0] = -(psConsts.psFloatConst[0] * float4(2.0f) - float4(1.0f));",
                "ps_3_0 signneg source modifier lowers to negated signed x2 bias");
  checkContains(source, "r[1] = (float4(1.0f) - psConsts.psFloatConst[1]);",
                "ps_3_0 complement source modifier lowers to 1-src");
  checkContains(source, "r[2] = (psConsts.psFloatConst[2] * float4(2.0f));",
                "ps_3_0 x2 source modifier lowers to multiply by two");
  checkContains(source, "r[3] = -(psConsts.psFloatConst[3] * float4(2.0f));",
                "ps_3_0 x2neg source modifier lowers to negated multiply by two");
  checkContains(source, "r[4] = ((psConsts.psFloatConst[4]) / float4((psConsts.psFloatConst[4]).z));",
                "ps_3_0 dz source modifier lowers to z-projected vector");
  checkContains(source, "r[5] = ((psConsts.psFloatConst[5]) / float4((psConsts.psFloatConst[5]).w));",
                "ps_3_0 dw source modifier lowers to w-projected vector");
  checkContains(source,
                "r[6] = select(float4(1.0f), float4(0.0f), "
                "((psConsts.psBoolConst[0] != 0u ? float4(1.0f) : float4(0.0f))) != float4(0.0f));",
                "ps_3_0 not source modifier lowers boolean-like source to inverted 0/1 float mask");
  checkContains(source, "r[8] = (psConsts.psFloatConst[8] - float4(0.5f));",
                "ps_3_0 bias source modifier lowers to src-0.5");
  checkContains(source, "r[9] = -((psConsts.psFloatConst[9]) - float4(0.5f));",
                "ps_3_0 biasneg source modifier lowers to negated src-0.5");
  checkContains(source, "r[10] = (psConsts.psFloatConst[10] * float4(2.0f) - float4(1.0f));",
                "ps_3_0 sign source modifier lowers to signed x2 bias");
  checkContains(source, "r[11] = -abs(psConsts.psFloatConst[11]);",
                "ps_3_0 absneg source modifier lowers to negated absolute value");
  checkContains(source, "outColor[0] = r[7];",
                "ps_3_0 source modifier coverage result reaches color output");
}

void testPs30ConstIntSourceLowering() {
  const auto source = translatePixel(makePs30ConstIntSourceBytecode());
  checkContains(source, "const int4 dxmt9_cdefi0 = int4(1, 2, 3, 4);",
                "ps_3_0 DEFI hoists to an immutable int4 local");
  checkContains(source, "r[0] = float4(dxmt9_cdefi0);",
                "ps_3_0 CONSTINT source register lowers through the DEFI local");
  checkContains(source, "outColor[0] = r[0];",
                "ps_3_0 CONSTINT source result reaches color output");
}

void testPs30LoopRegisterConstIntLowering() {
  const auto source = translatePixel(makePs30LoopRegisterConstIntBytecode());
  checkContains(source, "const int4 dxmt9_cdefi0 = int4(2, 0, 0, 0);",
                "ps_3_0 DEFI initializes the loop count constant");
  checkContains(source,
                "for (int dxmt9_loop_1 = 0, dxmt9_loopCount_1 = max(0, int(round(float4(dxmt9_cdefi0).x)));",
                "ps_3_0 LOOP aL, i# uses the CONSTINT operand as the loop count");
  checkContains(source, "r[0] = psConsts.psFloatConst[1];",
                "ps_3_0 LOOP body remains translated after the aL operand");
}

void testD3DBCDestModifierPartialPrecisionLowering() {
  using namespace dxmt9::d3d9bc;
  namespace translator_test = dxmt9::translator::test;

  constexpr u32 kD3DSPDMSaturate = 1u;
  constexpr u32 kD3DSPDMPartialPrecision = 2u;
  const u32 combinedDst = makeDstToken(kD3DSPR_TEMP, 7, 0xfu,
                                       kD3DSPDMSaturate | kD3DSPDMPartialPrecision);
  checkEqual(translator_test::decodeDestModifierForTest(combinedDst), 3u,
             "D3DBC operand decode preserves combined destination modifier bits");

  const auto pixelSource = translatePixel(makePs30DestModifierCoverageBytecode());
  checkContains(pixelSource, "r[0] = float4(half4(psConsts.psFloatConst[0]));",
                "ps_3_0 _pp destination lowers through half precision");
  checkContains(pixelSource,
                "r[1] = clamp(float4(half4((psConsts.psFloatConst[1] + psConsts.psFloatConst[2]))), float4(0.0f), float4(1.0f));",
                "ps_3_0 combined _sat/_pp destination preserves both modifier bits");

  const auto vertexSource = translateVertex(makeVs20DestModifierCoverageBytecode());
  checkContains(vertexSource, "r[0] = float4(half4(vsConsts.vsFloatConst[0]));",
                "vs_2_0 _pp destination lowers through half precision");
  checkContains(vertexSource,
                "r[1] = clamp(float4(half4((vsConsts.vsFloatConst[1] + vsConsts.vsFloatConst[2]))), float4(0.0f), float4(1.0f));",
                "vs_2_0 combined _sat/_pp destination preserves both modifier bits");
}

void testPs30CentroidInputModifierLowersToMslInterpolation() {
  const auto source = translatePixel(makePs30CentroidInputBytecode());
  checkContains(source, "float4 color [[centroid_perspective]];",
                "PS input D3DSPDM_MSAMPCENTROID lowers COLOR0 to centroid interpolation");
  checkContains(source, "float4 texcoord3 [[centroid_perspective]];",
                "PS input D3DSPDM_MSAMPCENTROID lowers TEXCOORD3 to centroid interpolation");
  checkContains(source, "float fogFactor [[centroid_perspective]];",
                "PS input D3DSPDM_MSAMPCENTROID lowers FOG to centroid interpolation");
  checkContains(source, "float4 secondaryColor;",
                "non-centroid varyings keep the default interpolation");
}

void testPs30IfElseFlowControlTranslation() {
  const auto source = translatePixel(makePs30IfElseBytecode());
  checkContains(source, "if ((psConsts.psFloatConst[0]).x != 0.0f) {", "ps_3_0 IF condition lowers to scalar branch");
  checkContains(source, "r[0] = psConsts.psFloatConst[1];", "ps_3_0 IF body is translated");
  checkContains(source, "} else {", "ps_3_0 ELSE lowers to structured branch");
  checkContains(source, "r[0] = psConsts.psFloatConst[2];", "ps_3_0 ELSE body is translated");
  checkContains(source, "outColor[0] = r[0];", "ps_3_0 flow-control result reaches color output");
}

void testPs30LoopFlowControlTranslation() {
  const auto source = translatePixel(makePs30LoopBytecode());
  checkContains(source,
                "for (int dxmt9_loop_0 = 0, dxmt9_loopCount_0 = max(0, int(round(psConsts.psFloatConst[0].x))); "
                "dxmt9_loop_0 < dxmt9_loopCount_0; ++dxmt9_loop_0) {",
                "ps_3_0 LOOP lowers to deterministic counted for-loop");
  checkContains(source, "r[0] = psConsts.psFloatConst[1];", "ps_3_0 LOOP body is translated");
  checkContains(source, "// endloop", "ps_3_0 ENDLOOP opcode is preserved in generated source comments");
  checkContains(source, "outColor[0] = r[0];", "ps_3_0 LOOP result reaches color output");
}

void testPs30NestedLoopFlowControlTranslation() {
  const auto source = translatePixel(makePs30NestedLoopBytecode());
  checkContains(source,
                "for (int dxmt9_loop_0 = 0, dxmt9_loopCount_0 = max(0, int(round(psConsts.psFloatConst[0].x))); "
                "dxmt9_loop_0 < dxmt9_loopCount_0; ++dxmt9_loop_0) {",
                "outer ps_3_0 LOOP lowers to a counted for-loop");
  checkContains(source,
                "for (int dxmt9_rep_1 = 0, dxmt9_repCount_1 = max(0, int(round(psConsts.psFloatConst[1].x))); "
                "dxmt9_rep_1 < dxmt9_repCount_1; ++dxmt9_rep_1) {",
                "inner ps_3_0 REP lowers to a distinct counted for-loop");
  checkContains(source, "r[0] = (r[0] + psConsts.psFloatConst[2]);",
                "nested loop body remains inside the generated source");
  checkContains(source, "outColor[0] = r[0];",
                "nested loop result reaches color output after both loops close");
}

void testPs30RepFlowControlTranslation() {
  const auto source = translatePixel(makePs30RepBytecode());
  checkContains(source,
                "for (int dxmt9_rep_0 = 0, dxmt9_repCount_0 = max(0, int(round(psConsts.psFloatConst[2].x))); "
                "dxmt9_rep_0 < dxmt9_repCount_0; ++dxmt9_rep_0) {",
                "ps_3_0 REP lowers to deterministic counted for-loop");
  checkContains(source, "r[1] = psConsts.psFloatConst[3];", "ps_3_0 REP body is translated");
  checkContains(source, "// endrep", "ps_3_0 ENDREP opcode is preserved in generated source comments");
  checkContains(source, "outColor[0] = r[1];", "ps_3_0 REP result reaches color output");
}

void testPs30BreakcFlowControlTranslation() {
  const auto source = translatePixel(makePs30BreakcBytecode());
  checkContains(source,
                "if ((psConsts.psFloatConst[0]).x > (psConsts.psFloatConst[1]).x) { break; }",
                "ps_3_0 BREAKC lowers comparison controls into a conditional break");
  checkContains(source, "r[0] = psConsts.psFloatConst[3];", "ps_3_0 BREAKC body continues after the guard");
  checkContains(source, "outColor[0] = r[0];", "ps_3_0 BREAKC result reaches color output");
}

void testPs30CallLabelRetFlowControlTranslation() {
  const auto source = translatePixel(makePs30CallLabelRetBytecode());
  checkContains(source, "bool dxmt9_call_ret_0 = false;",
                "ps_3_0 CALL creates a call-frame return guard");
  checkContains(source, "r[0] = psConsts.psFloatConst[4];", "ps_3_0 CALL/LABEL body is translated");
  checkContains(source, "outColor[0] = r[0];", "ps_3_0 CALL/LABEL/RET result reaches color output");
}

void testPs30CallnzLabelRetFlowControlTranslation() {
  const auto source = translatePixel(makePs30CallnzLabelRetBytecode());
  checkContains(source,
                "if (((psConsts.psBoolConst[0] != 0u ? float4(1.0f) : float4(0.0f))).x != 0.0f) {",
                "ps_3_0 CALLNZ guards the inlined call body with a bool-source nonzero test");
  checkContains(source, "r[0] = psConsts.psFloatConst[4];", "ps_3_0 CALLNZ/LABEL body is translated");
  checkContains(source, "outColor[0] = r[0];", "ps_3_0 CALLNZ/LABEL/RET result reaches color output");
}

void testPs30CallLabelNestedRetKeepsSubroutineBodyGuarded() {
  const auto source = translatePixel(makePs30CallLabelNestedRetBytecode());

  checkContains(source, "bool dxmt9_call_ret_0 = false;",
                "CALL/LABEL nested RET lowering creates a return flag");
  checkContains(source, "dxmt9_call_ret_0 = true;",
                "RET inside an inlined label body sets the return flag");
  checkContains(source, "if ((!dxmt9_call_ret_0)) {",
                "instructions after a nested RET stay guarded by the call return flag");
  checkContains(source, "r[0] = psConsts.psFloatConst[2];",
                "label body after nested control flow remains available when RET did not execute");
}

void testPs30PredicatedFlowControlTranslation() {
  const auto ifSource = translatePixel(makePs30PredicatedIfBytecode());
  checkContains(ifSource, "if (((p[0])) && ((psConsts.psFloatConst[0]).x != 0.0f)) {",
                "predicated IF combines p0 with the branch condition");
  checkContains(ifSource, "} else if ((p[0])) {",
                "predicated IF keeps ELSE under the same p0 guard");

  const auto breakSource = translatePixel(makePs30PredicatedBreakcBytecode());
  checkContains(breakSource, "if (((p[0])) && ((psConsts.psFloatConst[0]).x > (psConsts.psFloatConst[1]).x)) { break; }",
                "predicated BREAKC combines p0 with the comparison condition");

  const auto callnzSource = translatePixel(makePs30PredicatedCallnzLabelRetBytecode());
  checkContains(callnzSource,
                "if (((p[0])) && (((psConsts.psBoolConst[0] != 0u ? float4(1.0f) : float4(0.0f))).x != 0.0f)) {",
                "predicated CALLNZ combines p0 with the CALLNZ source condition");
}

void testPs30ArithmeticOpcodeLoweringContracts() {
  const auto source = translatePixel(makePs30ArithmeticOpcodeMatrixBytecode());
  checkContains(source, "r[0] = (psConsts.psFloatConst[0] * psConsts.psFloatConst[1] + psConsts.psFloatConst[2]);",
                "MAD lowers to multiply-add expression");
  checkContains(source, "float4(dot((r[0]).xyz, (psConsts.psFloatConst[3]).xyz))", "DP3 lowers to xyz dot splat");
  checkContains(source, "float4(dot(r[0], psConsts.psFloatConst[4]))", "DP4 lowers to full-vector dot splat");
  checkContains(source, "select(psConsts.psFloatConst[7], psConsts.psFloatConst[6], psConsts.psFloatConst[5] >= float4(0.0f))",
                "CMP lowers to sign-test select with source order preserved");
  checkContains(source, "select(float4(0.0f), float4(1.0f), (psConsts.psFloatConst[8]) < (psConsts.psFloatConst[9]))",
                "SLT lowers to boolean select mask");
  checkContains(source, "select(float4(0.0f), float4(1.0f), (psConsts.psFloatConst[10]) >= (psConsts.psFloatConst[11]))",
                "SGE lowers to boolean select mask");
  checkContains(source, "pow(psConsts.psFloatConst[12], psConsts.psFloatConst[13])", "POW lowers to pow source expression");
  checkContains(source, "outColor[0] = r[6];", "arithmetic opcode matrix result reaches color output");
}

void testPs30TranscendentalOpcodeLoweringContracts() {
  const auto source = translatePixel(makePs30TranscendentalOpcodeBytecode());
  checkContains(source, "float4(sin(psConsts.psFloatConst[0]), cos(psConsts.psFloatConst[0]), 0.0f, 0.0f)",
                "SINCOS lowers to sin/cos vector construction");
  checkContains(source, "float4(log2(abs(psConsts.psFloatConst[1])))",
                "LOG lowers to D3D9 abs log2 expression");
  checkContains(source, "float4(exp2(psConsts.psFloatConst[2]))", "EXP lowers to exp2 expression");
  checkContains(source, "outColor[0] = r[2];", "transcendental opcode result reaches color output");
}

void testPs30MatrixOpcodeLoweringContracts() {
  const auto source = translatePixel(makePs30MatrixOpcodeBytecode());
  checkContains(source, "dot(psConsts.psFloatConst[0], psConsts.psFloatConst[4])", "M4x4 starts at the declared matrix constant base");
  checkContains(source, "dot(psConsts.psFloatConst[0], psConsts.psFloatConst[7])", "M4x4 consumes four matrix rows from the base constant");
  checkContains(source, "dot((psConsts.psFloatConst[1]).xyz, psConsts.psFloatConst[8].xyz)", "M3x3 starts at the declared matrix constant base");
  checkContains(source, "dot((psConsts.psFloatConst[1]).xyz, psConsts.psFloatConst[10].xyz)", "M3x3 consumes three xyz matrix rows");
  checkContains(source, "outColor[0] = r[1];", "matrix opcode result reaches color output");
}

void testPs30TextureLodOpcodeLoweringContracts() {
  const auto source = translatePixel(makePs30TextureLodOpcodeBytecode());
  checkContains(source, "texture2d<float> tex3 [[texture(3)]]", "TEXLDD declares the referenced sampler slot");
  checkContains(source, "sampler samp4 [[sampler(4)]]", "TEXLDL declares the referenced sampler state slot");
  checkContains(source,
                "tex3.sample(samp3, (psConsts.psFloatConst[0]).xy, gradient2d((psConsts.psFloatConst[2]).xy, (psConsts.psFloatConst[3]).xy))",
                "TEXLDD lowers to an explicit-gradient sample call");
  checkContains(source, "tex4.sample(samp4, (psConsts.psFloatConst[1]).xy, level(psConsts.psFloatConst[1].w))",
                "TEXLDL lowers to explicit level sample call");
  checkContains(source, "outColor[0] = r[1];", "texture LOD opcode result reaches color output");
}

void testPs30TextureSamplerDimensionalityContracts() {
  const auto cubeSource = translatePixel(makePs30TexlddSamplerTypeBytecode(3u));
  checkContains(cubeSource, "texturecube<float> tex3 [[texture(3)]]",
                "cube DCL sampler declares texturecube in MSL");
  checkContains(cubeSource,
                "tex3.sample(samp3, (psConsts.psFloatConst[0]).xyz, gradientcube((psConsts.psFloatConst[2]).xyz, (psConsts.psFloatConst[3]).xyz))",
                "cube TEXLDD lowers to xyz explicit-gradient sample");

  const auto volumeSource = translatePixel(makePs30TexlddSamplerTypeBytecode(4u));
  checkContains(volumeSource, "texture3d<float> tex3 [[texture(3)]]",
                "volume DCL sampler declares texture3d in MSL");
  checkContains(volumeSource,
                "tex3.sample(samp3, (psConsts.psFloatConst[0]).xyz, gradient3d((psConsts.psFloatConst[2]).xyz, (psConsts.psFloatConst[3]).xyz))",
                "volume TEXLDD lowers to xyz explicit-gradient sample");
}

void testPs11LegacyTexcoordTexLoweringContract() {
  const auto source = translatePixel(makePs11TexcoordTexBytecode());
  checkContains(source, "for (uint i = 0; i < 8u; ++i) { outTexcoord[i] = dxmt9_select_texcoord(in, i); }",
                "ps_1_1 initializes mutable t# registers from interpolated texcoords");
  checkContains(source, "outTexcoord[0] = float4(saturate((in.texcoord0).xyz), 1.0f);",
                "ps_1_1 TEXCOORD clamps the stage texcoord and forces w=1");
  checkContains(source, "texture2d<float> tex0 [[texture(0)]]", "ps_1_1 TEX binds the destination stage texture");
  // samplerLodBias variant flag is clear here (general TEX-lowering contract),
  // so the sample emits the plain form with no bias() arg — the dedicated
  // ps_2_0 / FFP cases cover the flag-set bias() path.
  checkContains(source, "outTexcoord[0] = tex0.sample(samp0, (outTexcoord[0]).xy);",
                "ps_1_1 TEX samples stage 0 through destination t0 (no LOD bias when variant flag clear)");
  checkContains(source, "r[0] = outTexcoord[0];", "ps_1_1 t# source reaches r0 color output");
}

void testVs11FixedFunctionOutputLoweringContract() {
  const auto source = translateVertex(makeVs11FixedFunctionOutputBytecode());
  checkContains(source, "outPosition = vin0;",
                "vs_1_1 oPos writes lower to the Metal position output");
  checkContains(source, "outColor = vsConsts.vsFloatConst[0];",
                "vs_1_1 oD0 writes lower to the primary color output");
  checkContains(source, "outSecondaryColor = vsConsts.vsFloatConst[1];",
                "vs_1_1 oD1 writes lower to the secondary color output");
  checkContains(source, "outTexcoord[3] = vsConsts.vsFloatConst[2];",
                "vs_1_1 oT3 writes lower to the matching texcoord output");
  checkContains(source, "out.position = outPosition;",
                "vs_1_1 translated source returns the lowered position");
}

void testPs14TexcrdTexldTexdepthLoweringContract() {
  const auto source = translatePixel(makePs14TexcrdTexldDepthBytecode());
  checkContains(source, "r[0] = in.texcoord0;",
                "ps_1_4 TEXCRD copies explicit t# source to r#");
  checkContains(source, "texture2d<float> tex1 [[texture(1)]]", "ps_1_4 TEXLD binds destination stage texture");
  // samplerLodBias variant flag clear (general lowering contract) → plain sample.
  checkContains(source, "r[1] = tex1.sample(samp1, (in.texcoord1).xy);",
                "ps_1_4 TEXLD samples destination stage with explicit source coords (no LOD bias when variant flag clear)");
  checkContains(source, "outDepth = clamp((r[5]).x / min((r[5]).y, 1.0f), 0.0f, 1.0f);",
                "ps_1_4 TEXDEPTH writes fragment depth from r5-style source");
}

void testPs13LegacyTextureFamilyLoweringContract() {
  const auto source = translatePixel(makePs13LegacyTextureFamilyBytecode());
  checkContains(source, "ffpPs.bumpEnvMat[0]", "TEXBEM lowers through bump-env matrix state");
  checkContains(source, "ffpPs.bumpEnvLum[1]", "TEXBEML lowers through bump-env luminance state");
  checkContains(source, "float4((outTexcoord[0]).w, (outTexcoord[0]).x, 0.0f, 1.0f)",
                "TEXREG2AR maps source alpha/red to sample coords");
  checkContains(source, "float4((outTexcoord[1]).y, (outTexcoord[1]).z, 0.0f, 1.0f)",
                "TEXREG2GB maps source green/blue to sample coords");
  checkContains(source, "dot((outTexcoord[5]).xyz, (outTexcoord[4]).xyz)",
                "TEXDP3/TEXDP3TEX use destination-stage texcoord dot source rgb");
  checkContains(source, "dxmt9_texm.x =", "TEXM3x2/TEXM3x3 PAD stores matrix row x");
  checkContains(source, "dxmt9_texm.y =", "TEXM3x2TEX/TEXM3x2DEPTH stores matrix row y");
  checkContains(source, "dxmt9_texm.z =", "TEXM3x3 final op stores matrix row z");
  checkContains(source, "reflect(normalize(",
                "TEXM3x3SPEC forms reflection coords from explicit eye vector");
  checkContains(source, "normalize(dxmt9_texm.xyz)",
                "TEXM3x3SPEC normalizes the matrix texture vector");
  checkContains(source, "reflect(normalize(float3(outTexcoord[2].w, outTexcoord[3].w, outTexcoord[4].w))",
                "TEXM3x3VSPEC forms eye vector from texture-coordinate w components");
  checkContains(source, "outDepth = dxmt9_texm.y == 0.0f ? 1.0f : clamp(dxmt9_texm.x / dxmt9_texm.y, 0.0f, 1.0f);",
                "TEXM3x2DEPTH writes fragment depth with zero-y guard");
}

void testPs14BemLoweringContract() {
  const auto source = translatePixel(makePs14BemBytecode());
  checkContains(source, "r[0] = float4((in.texcoord0).xy + float2(ffpPs.bumpEnvMat[0].x",
                "ps_1_4 BEM writes bumped coordinates using destination-stage bump matrix");
}

void testReservedTexm3x3DiffStillThrowsDeterministically() {
  // Wine keeps this opcode name for diagnostics, but its SM1 opcode table marks
  // TEXM3x3DIFF with an empty 0.0..0.0 valid shader-model range.
  checkThrowsContains(
      [] {
        (void)translatePixel(makePs13ReservedTexm3x3DiffBytecode());
      },
      "reserved legacy texture opcode",
      "reserved TEXM3x3DIFF opcode stays a deterministic unsupported path");
}

void testCallnzFixedOperandCountDecodeContract() {
  namespace translator_test = dxmt9::translator::test;
  const auto module = translator_test::decodeD3DBytecodeForTest(
      makeShader(makePs30CallnzFixedOperandCountBytecode()),
      false,
      DrawDesc{});

  checkEqual(module.instructions.size(), size_t{2}, "CALLNZ fixed count keeps the following MOV aligned");
  checkEqual(module.instructions[0].opcode, dxmt9::d3d9bc::kD3DSIO_CALLNZ, "CALLNZ opcode decodes");
  checkEqual(module.instructions[0].operands.size(), size_t{2}, "CALLNZ decodes label plus condition source");
  checkEqual(module.instructions[1].opcode, dxmt9::d3d9bc::kD3DSIO_MOV,
             "CALLNZ fixed operand count preserves the next instruction");
}

void testD3DBCFixedOperandCountDecodeContract() {
  const auto source = translatePixel(makePs30FixedOperandCountDecodeBytecode());
  checkContains(source, "r[0] = float4(dot(psConsts.psFloatConst[0], psConsts.psFloatConst[1]));",
                "known DP4 opcode decodes with its fixed operand count");
  checkContains(source, "r[1] = (r[0] * psConsts.psFloatConst[2] + psConsts.psFloatConst[3]);",
                "known MAD opcode decodes with its fixed operand count");
  checkContains(source, "outColor[0] = r[1];", "fixed operand-count decode result reaches color output");
}

void testPs30RelativeAddressingLowersTempDestinationIndex() {
  const auto source = translatePixel(makePs30RelativeAddressingBytecode());
  checkContains(source,
                "r[clamp(a0.x + 0, 0, 31)] = psConsts.psFloatConst[0];",
                "ps_3_0 temp destination relative addressing lowers to a clamped r[] write");
}

void testPs30RelativeAddressingLowersTempSourceIndex() {
  const auto source = translatePixel(makePs30TempRelativeSourceBytecode());
  checkContains(source,
                "r[clamp(a0.x + 1, 0, 31)]",
                "ps_3_0 temp source relative addressing lowers to a clamped r[] read");
}

void testPs30IndexedConstSourceLowersToClampedConstAccess() {
  const auto source = translatePixel(makePs30IndexedConstSourceBytecode());
  checkContains(source, "cFloat[clamp(a0.x + 7, 0, 223)]",
                "ps_3_0 indexed const source emits clamped a0+N access");
  checkContains(source, "constant float4* cFloat = psConsts.psFloatConst;",
                "ps_3_0 indexed const source aliases the full pixel constant buffer");
}

void testPs30IndexedConstDestinationLowersToClampedMutableConstWrite() {
  const auto source = translatePixel(makePs30IndexedConstDestinationBytecode());
  checkContains(source, "float4 cFloat[224];",
                "ps_3_0 indexed const destination keeps a mutable full-size cFloat array");
  checkContains(source, "cFloat[i] = psConsts.psFloatConst[i];",
                "mutable pixel constants are initialized from host constants before shader writes");
  checkContains(source, "cFloat[clamp(a0.x + 7, 0, 223)] = cFloat[1];",
                "ps_3_0 indexed const destination emits clamped a0+N write");
  checkNotContains(source, "constant float4* cFloat = ",
                   "ps_3_0 indexed const destination must not alias cFloat through a read-only constant pointer");
}

void testPs30FragmentPositionAndMissingInputContracts() {
  const auto positionSource = translatePixel(makePs30PositionInputBytecode());
  checkContains(positionSource, "outColor[0] = in.position;",
                "ps_3_0 POSITION input maps to the Metal fragment position");

  const auto missingSource = translatePixel(makePs30MissingInputBytecode());
  checkContains(missingSource, "outColor[0] = float4(0.0f);",
                "undeclared ps_3_0 input register reads lower to a zero default");
}

void testVs30MissingInputDefaultsToZero() {
  const auto source = translateVertex(makeVs30MissingInputBytecode());
  checkContains(source, "float4 vin5 = float4(0.0f);",
                "vs_3_0 missing input read gets an explicit zero-default local");
  checkContains(source, "outTexcoord[0] = vin5;",
                "undeclared vs_3_0 input reads preserve the zero-default source");
}

void testVs30RelativeAddressingLowersTexcoordDestinationIndex() {
  const auto source = translateVertex(makeVs30TexcoordRelativeDestinationBytecode());
  checkContains(source,
                "outTexcoord[clamp(a0.x + 1, 0, 7)] = vsConsts.vsFloatConst[1];",
                "vs_3_0 texcoord output relative destination lowers to a clamped output write");
}

void testVs20IndexedConstDestinationLowersToClampedMutableConstWrite() {
  const auto source = translateVertex(makeVs20IndexedConstDestinationBytecode());

  checkContains(source, "float4 cFloat[256];",
                "indexed const destination keeps a mutable full-size cFloat array");
  checkContains(source, "cFloat[i] = vsConsts.vsFloatConst[i];",
                "mutable vertex constants are initialized from host constants before shader writes");
  checkContains(source, "cFloat[clamp(a0.x + 5, 0, 255)] = cFloat[1];",
                "indexed const destination emits clamped a0+N write");
  checkNotContains(source, "constant float4* cFloat = ",
                   "indexed const destination must not alias cFloat through a read-only constant pointer");
}

void testVs20DefLiteralWithRelAddrBitDoesNotDriftParser() {
  using namespace dxmt9::d3d9bc;
  namespace translator_test = dxmt9::translator::test;

  DrawDesc desc{};
  const auto module = translator_test::decodeD3DBytecodeForTest(
      makeShader(makeVs20DefLiteralWithRelAddrBitBytecode()), true, desc);

  // Two well-formed instructions: DEF (5 operands), MOV (2 operands).
  // If the parser misreads the bit-13-set float literal as a register
  // with rel-addr, it consumes an extra DWORD and the MOV either
  // disappears or decodes as a different opcode — either way the
  // count or downstream opcode would drift.
  checkEqual(module.instructions.size(), size_t{2},
             "DEF literal with bit 13 must not consume a phantom rel-addr DWORD");
  checkEqual(module.instructions[0].opcode, kD3DSIO_DEF,
             "DEF instruction parses unchanged when its float literal has bit 13 set");
  checkEqual(module.instructions[0].operands.size(), size_t{5},
             "DEF preserves its 5 operands across the inhibited rel-addr probe");
  checkEqual(module.instructions[1].opcode, kD3DSIO_MOV,
             "MOV after DEF stays aligned — no parser drift");
  // Literal positions on DEF must NOT carry rel-addr tokens, even
  // when the literal's bit 13 happens to be set.
  for (size_t i = 1; i < module.instructions[0].relAddrTokens.size(); ++i) {
    if (module.instructions[0].relAddrTokens[i] != 0u) {
      fail("DEF literal operand position " + std::to_string(i) +
           " spuriously captured a rel-addr token");
    }
  }
}

void testVs20IndexedConstSourceParserConsumesRelAddrDword() {
  using namespace dxmt9::d3d9bc;
  namespace translator_test = dxmt9::translator::test;

  DrawDesc desc{};
  const auto module = translator_test::decodeD3DBytecodeForTest(
      makeShader(makeVs20IndexedConstSourceBytecode()), true, desc);

  // 4 instructions: dcl, mova, mov, mov. If the parser miscounts the
  // rel-addr DWORD as the next operand, instruction count drifts and
  // subsequent decode either truncates or surfaces a wrong opcode.
  checkEqual(module.instructions.size(), size_t{4},
             "vs_2_0 indexed-const source bytecode parses into four instructions after rel-addr DWORD consumption");
  checkEqual(module.instructions[1].opcode, kD3DSIO_MOVA,
             "second instruction stays MOVA after parser skips its operand boundary correctly");
  checkEqual(module.instructions[2].opcode, kD3DSIO_MOV,
             "third instruction stays MOV after parser consumes the rel-addr DWORD");

  // The MOV operand 1 (src) carries the rel-addr DWORD that selects
  // the address register a0.
  const auto& movInstr = module.instructions[2];
  checkEqual(movInstr.relAddrTokens.size(), size_t{2},
             "MOV operand-parallel rel-addr vector matches operand count");
  if (movInstr.relAddrTokens[1] == 0u) {
    fail("MOV source operand should carry a non-zero rel-addr token");
  }
  const auto relReg = translator_test::decodeRegisterRefForTest(movInstr.relAddrTokens[1],
                                                                 D3DShaderStage::Vertex);
  checkRegister(relReg, D3DRegisterKind::Address, 0u,
                "rel-addr token decodes to address register a0 in vertex stage");
}

void testVs20IndexedConstSourceLowersToClampedConstAccess() {
  const auto source = translateVertex(makeVs20IndexedConstSourceBytecode());

  // Source-side rel-addr produces clamped indexed access into the
  // full vsFloatConst[256] range (kMaxVertexConstants - 1 = 255).
  checkContains(source, "cFloat[clamp(a0.x + 5, 0, 255)]",
                "indexed const source emits clamped a0+N access");
  // The mova lowers through the address-register write-mask.
  checkContains(source, "a0.x = dxmt9_mova.x;",
                "MOVA still lowers to a masked address-register assignment");
  // Indexed access forces pointer aliasing onto the full constant
  // buffer so the clamp range stays valid.
  checkContains(source, "constant float4* cFloat = ",
                "indexed const access forces pointer aliasing for cFloat");
  // Explicit guarantee against the previous silent-drop bug.
  checkNotContains(source, "cFloat[5]u",
                   "indexed access is no longer rewritten as a static cFloat[5] read");
}

void testVsRelativeAddrHonorsAddressComponent() {
  const auto source = translateVertex(makeVs30RelAddrYComponentBytecode());

  // D3D9 a0 is a 4-component address register. `c[a0.y + 0]` must index
  // with the a0.y component, not collapse to the a0.x scalar. Collapsing
  // to a0.x is the 3DMark05 GT1 skinning corruption (a vertex straddling
  // two bones reads the wrong bone matrix for the second weight and
  // explodes into a spike).
  checkContains(source, "cFloat[clamp(a0.y + 0, 0, 255)]",
                "rel-addr source honors the a0.y component selector");
  checkNotContains(source, "cFloat[clamp(a0 + 0, 0, 255)]",
                   "rel-addr source no longer collapses a0.y to scalar a0");
}

void testVsMovaHonorsDestinationWriteMask() {
  const auto source = translateVertex(makeVs30MovaWriteMaskBytecode());

  checkContains(source, "a0.x = dxmt9_mova.x;",
                "MOVA a0.x writes only the x address component");
  checkContains(source, "a0.y = dxmt9_mova.y;",
                "MOVA a0.y writes only the y address component");
  checkNotContains(source, "a0 = int4(round(",
                   "partial MOVA does not overwrite unrelated address components");
  checkContains(source, "cFloat[clamp(a0.y + 0, 0, 255)]",
                "relative constant read still uses the selected a0 component");
}

void testVs30VertexTextureFetchLowersDeterministically() {
  // Wine visual.c `test_vertex_texture` exercises real VS texture fetch only
  // when caps expose D3DUSAGE_QUERY_VERTEXTEXTURE. The vertex-stage
  // texture/sampler binding ABI now exists, and the MSL translator lowers
  // VS TEXLDL to a Metal `tex<N>.sample(samp<N>, coord, level(coord.w))`
  // call at the vertex function. This test pins the lowering so the
  // closed-gap behavior cannot silently regress to either a throw (the
  // historical pre-ABI behavior) or a quiet no-op.
  //
  // Safe-rejection contract context: commit e867a2a wraps
  // `translateD3DBytecodeToSpirv` in a try/catch that converts any
  // `DecoderReject` / `std::exception` into an empty SpirvModule and
  // bumps a `shader_decoder_reject_*` perf counter. That contract
  // applies to malformed bytecode (see
  // `tests/native/core/shader_bytecode_validation_spec.cpp`); the
  // VS texture fetch fixture here is well-formed and must reach the
  // MSL emitter, so the contract observable for this case is the
  // emitted source string — not a counter bump.
  const auto source = translateVertex(makeVs30VertexTextureBytecode());
  checkContains(source, "tex0.sample(samp0",
                "VS TEXLDL lowers to a vertex-stage Metal sample call on sampler 0");
  checkContains(source, "level(",
                "VS TEXLDL preserves the explicit LOD argument from src.w");
}

void testPs14ConstantSourcesClampBeforeArithmetic() {
  const auto source = translatePixel(makePs14ConstantClampBytecode());
  checkContains(source, "clamp(psConsts.psFloatConst[1], float4(-1.0f), float4(1.0f))",
                "ps_1_4 runtime float constants are clamped before MOV");
  checkContains(source, "clamp(dxmt9_cdef2, float4(-1.0f), float4(1.0f))",
                "ps_1_4 DEF constants are clamped before ADD");
  checkContains(source, "color = r[0];",
                "ps_1_4 final color comes from r0 instead of the SM2+ outColor path");
}

// H226 — translated shaders must read the constant register file in place
// instead of copying the whole bound category into a per-invocation local
// array. The historical `float4 cFloat[N]; for (...) cFloat[i] = ...;`
// prologue cannot be register-allocated for large N and spills to stack
// (device memory) on Apple GPUs: SFIV fullscreen passes paid ~30ms/draw of
// stack write/read traffic and GT1's hidden VS-buffer-write bucket tracked
// the same shape.

std::vector<u32> makePs30HighConstReadBytecode() {
  using namespace dxmt9::d3d9bc;
  return {
      makeVersionToken(false, 3, 0),
      // mov oC0, c136 — a single high-register read must not materialize a
      // 137-entry local register file.
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_COLOROUT, 0),
      makeSrcToken(kD3DSPR_CONST, 136),
      kD3DSIO_END,
  };
}

void testPs30ConstReadsBindConstantBufferInPlace() {
  const auto source = translatePixel(makePs30HighConstReadBytecode());
  checkContains(source, "outColor[0] = psConsts.psFloatConst[136];",
                "ps_3_0 static constant reads reference the bound constant buffer in place");
  checkNotContains(source, "float4 cFloat[",
                   "ps_3_0 without relative addressing must not materialize a local register file");
  checkNotContains(source, "cFloat[i] = psConsts.psFloatConst[i]",
                   "ps_3_0 without relative addressing must not copy the register file per invocation");
  checkNotContains(source, "cFloat",
                   "ps_3_0 without relative addressing must not reference a cFloat local at all");
}

std::vector<u32> makeVs30HighConstReadBytecode() {
  using namespace dxmt9::d3d9bc;
  return {
      makeVersionToken(true, 3, 0),
      // dcl_position o0
      makeInstructionToken(kD3DSIO_DCL, 2),
      makeDclSemanticToken(0u, 0u),
      makeDstToken(kD3DSPR_TEXCRDOUT, 0),
      // mov o0, c200 — SFIV/GT1 VS shaders copied the full 256-entry file
      // (4KB) for reads like this.
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_TEXCRDOUT, 0),
      makeSrcToken(kD3DSPR_CONST, 200),
      kD3DSIO_END,
  };
}

void testVs30ConstReadsBindConstantBufferInPlace() {
  const auto source = translateVertex(makeVs30HighConstReadBytecode());
  checkContains(source, "outPosition = vsConsts.vsFloatConst[200];",
                "vs_3_0 static constant reads reference the bound constant buffer in place");
  checkNotContains(source, "float4 cFloat[",
                   "vs_3_0 without relative addressing must not materialize a local register file");
  checkNotContains(source, "cFloat[i] = vsConsts.vsFloatConst[i]",
                   "vs_3_0 without relative addressing must not copy the register file per invocation");
  checkNotContains(source, "cFloat",
                   "vs_3_0 without relative addressing must not reference a cFloat local at all");
}

std::vector<u32> makePs30DefAndRuntimeConstBytecode() {
  using namespace dxmt9::d3d9bc;
  return {
      makeVersionToken(false, 3, 0),
      // def c0, 0.0, 0.0, 0.0, 1.0
      makeInstructionToken(kD3DSIO_DEF, 5),
      makeDstToken(kD3DSPR_CONST, 0),
      0x00000000u,
      0x00000000u,
      0x00000000u,
      0x3F800000u,
      // defi i0, 2, 0, 0, 0
      makeInstructionToken(kD3DSIO_DEFI, 5),
      makeDstToken(kD3DSPR_CONSTINT, 0),
      2u,
      0u,
      0u,
      0u,
      // defb b0, true
      makeInstructionToken(kD3DSIO_DEFB, 2),
      makeDstToken(kD3DSPR_CONSTBOOL, 0),
      1u,
      // mov r0, c0 — DEF'd register must win at the use site.
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_TEMP, 0),
      makeSrcToken(kD3DSPR_CONST, 0),
      // add r0, r0, c5 — non-DEF'd register reads the bound buffer.
      makeInstructionToken(kD3DSIO_ADD, 3),
      makeDstToken(kD3DSPR_TEMP, 0),
      makeSrcToken(kD3DSPR_TEMP, 0),
      makeSrcToken(kD3DSPR_CONST, 5),
      // if b0
      makeInstructionToken(kD3DSIO_IF, 1),
      makeSrcToken(kD3DSPR_CONSTBOOL, 0),
      // loop aL, i0
      makeInstructionToken(kD3DSIO_LOOP, 2),
      makeSrcToken(kD3DSPR_LOOP, 0),
      makeSrcToken(kD3DSPR_CONSTINT, 0),
      // add r0, r0, c1
      makeInstructionToken(kD3DSIO_ADD, 3),
      makeDstToken(kD3DSPR_TEMP, 0),
      makeSrcToken(kD3DSPR_TEMP, 0),
      makeSrcToken(kD3DSPR_CONST, 1),
      makeInstructionToken(kD3DSIO_ENDLOOP, 0),
      makeInstructionToken(kD3DSIO_ENDIF, 0),
      // mov oC0, r0
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_COLOROUT, 0),
      makeSrcToken(kD3DSPR_TEMP, 0),
      kD3DSIO_END,
  };
}

void testPs30DefConstantsHoistToImmutableLocalsAndWinAtUseSites() {
  const auto source = translatePixel(makePs30DefAndRuntimeConstBytecode());
  checkContains(source, "const float4 dxmt9_cdef0 = float4(0.0f, 0.0f, 0.0f, 1.0f);",
                "ps_3_0 DEF hoists to an immutable float4 local");
  checkContains(source, "const int4 dxmt9_cdefi0 = int4(2, 0, 0, 0);",
                "ps_3_0 DEFI hoists to an immutable int4 local");
  checkContains(source, "const uint dxmt9_cdefb0 = 1u;",
                "ps_3_0 DEFB hoists to an immutable uint local");
  checkContains(source, "r[0] = dxmt9_cdef0;",
                "ps_3_0 DEF'd register use site references the def local (defs win)");
  checkContains(source, "r[0] = (r[0] + psConsts.psFloatConst[5]);",
                "ps_3_0 non-DEF'd register use site reads the bound constant buffer in place");
  checkContains(source, "r[0] = (r[0] + psConsts.psFloatConst[1]);",
                "ps_3_0 loop-body constant read stays a direct buffer read");
  checkContains(source, "if (((dxmt9_cdefb0 != 0u ? float4(1.0f) : float4(0.0f))).x != 0.0f) {",
                "ps_3_0 IF b0 condition references the DEFB local");
  checkContains(source, "max(0, int(round(float4(dxmt9_cdefi0).x)))",
                "ps_3_0 LOOP count references the DEFI local");
  checkNotContains(source, "cFloat",
                   "ps_3_0 DEF-only float file must not materialize a cFloat local");
  checkNotContains(source, "cInt",
                   "ps_3_0 DEF-only int file must not materialize a cInt local");
  checkNotContains(source, "cBool",
                   "ps_3_0 DEF-only bool file must not materialize a cBool local");
}

std::vector<u32> makeVs20DefAndRuntimeConstBytecode() {
  using namespace dxmt9::d3d9bc;
  return {
      makeVersionToken(true, 2, 0),
      // def c0, 1.0, 2.0, 3.0, 4.0
      makeInstructionToken(kD3DSIO_DEF, 5),
      makeDstToken(kD3DSPR_CONST, 0),
      0x3F800000u,
      0x40000000u,
      0x40400000u,
      0x40800000u,
      // mov r0, c0
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_TEMP, 0),
      makeSrcToken(kD3DSPR_CONST, 0),
      // add r0, r0, c5
      makeInstructionToken(kD3DSIO_ADD, 3),
      makeDstToken(kD3DSPR_TEMP, 0),
      makeSrcToken(kD3DSPR_TEMP, 0),
      makeSrcToken(kD3DSPR_CONST, 5),
      // mov oPos, r0
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_RASTOUT, 0),
      makeSrcToken(kD3DSPR_TEMP, 0),
      kD3DSIO_END,
  };
}

void testVs20DefConstantsHoistToImmutableLocalsAndWinAtUseSites() {
  const auto source = translateVertex(makeVs20DefAndRuntimeConstBytecode());
  checkContains(source, "const float4 dxmt9_cdef0 = float4(1.0f, 2.0f, 3.0f, 4.0f);",
                "vs_2_0 DEF hoists to an immutable float4 local");
  checkContains(source, "r[0] = dxmt9_cdef0;",
                "vs_2_0 DEF'd register use site references the def local (defs win)");
  checkContains(source, "r[0] = (r[0] + vsConsts.vsFloatConst[5]);",
                "vs_2_0 non-DEF'd register use site reads the bound constant buffer in place");
  checkNotContains(source, "cFloat",
                   "vs_2_0 DEF-only float file must not materialize a cFloat local");
}

std::vector<u32> makeVs20RelativeConstReadWithDefBytecode() {
  using namespace dxmt9::d3d9bc;
  return {
      makeVersionToken(true, 2, 0),
      // def c6, 1.0, 0.0, 0.0, 0.0
      makeInstructionToken(kD3DSIO_DEF, 5),
      makeDstToken(kD3DSPR_CONST, 6),
      0x3F800000u,
      0u,
      0u,
      0u,
      // dcl_blendindices v3
      makeInstructionToken(kD3DSIO_DCL, 2),
      makeDclSemanticToken(2u, 0u),
      makeDstToken(kD3DSPR_INPUT, 3),
      // mova a0.x, v3
      makeInstructionToken(kD3DSIO_MOVA, 2),
      makeDstToken(kD3DSPR_ADDR, 0, 0x1u),
      makeSrcToken(kD3DSPR_INPUT, 3),
      // mov r0, c[a0+5]
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_TEMP, 0),
      makeRelativeSrcToken(kD3DSPR_CONST, 5),
      makeSrcToken(kD3DSPR_ADDR, 0),
      // add r0, r0, c6 — static read of the DEF'd register in the same
      // shader that addresses the float file relatively.
      makeInstructionToken(kD3DSIO_ADD, 3),
      makeDstToken(kD3DSPR_TEMP, 0),
      makeSrcToken(kD3DSPR_TEMP, 0),
      makeSrcToken(kD3DSPR_CONST, 6),
      // mov oPos, r0
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeDstToken(kD3DSPR_RASTOUT, 0),
      makeSrcToken(kD3DSPR_TEMP, 0),
      kD3DSIO_END,
  };
}

void testVs20RelativeConstReadWithDefKeepsRegisterFileArray() {
  const auto source = translateVertex(makeVs20RelativeConstReadWithDefBytecode());
  checkContains(source, "float4 cFloat[256];",
                "vs_2_0 relative addressing plus DEF keeps the materialized register file");
  checkContains(source,
                "for (uint i = 0; i < 256; ++i) { cFloat[i] = vsConsts.vsFloatConst[i]; }",
                "vs_2_0 relative addressing plus DEF keeps the register-file copy loop");
  checkContains(source, "cFloat[6] = float4(1.0f, 0.0f, 0.0f, 0.0f);",
                "vs_2_0 DEF overwrites the copied array entry in the relative-addressing path");
  checkContains(source, "cFloat[clamp(a0.x + 5, 0, 255)]",
                "vs_2_0 relative constant read stays a clamped array access");
  checkContains(source, "r[0] = (r[0] + cFloat[6]);",
                "vs_2_0 static read of the DEF'd register resolves through the array (defs win)");
}

}  // namespace

int main() {
  try {
    const ScopedUnsetEnv noFragmentColor("DXMT_DEBUG_FORCE_FRAGMENT_COLOR");
    const ScopedUnsetEnv noFragmentMode("DXMT_DEBUG_FRAGMENT_MODE");
    const ScopedUnsetEnv noFullscreenVertex("DXMT_DEBUG_FORCE_FULLSCREEN_VERTEX");
    const ScopedUnsetEnv noPixelVFlip("DXMT_DEBUG_FORCE_PIXEL_V_FLIP");
    const ScopedUnsetEnv noVertexYFlip("DXMT_DEBUG_FLIP_VERTEX_Y");
    const ScopedUnsetEnv noVertexTempTrim("DXMT9_TRIM_VERTEX_TEMPS");
    const ScopedUnsetEnv noVsOutputScratchTrim("DXMT9_TRIM_VS_OUTPUT_SCRATCH");
    const ScopedUnsetEnv noVsOutTrim("DXMT9_TRIM_UNUSED_VARYINGS");
    const ScopedUnsetEnv noVsOutTrimVsHashes("DXMT9_TRIM_UNUSED_VARYINGS_VS_HASHES");
    const ScopedUnsetEnv noVsOutTrimPsHashes("DXMT9_TRIM_UNUSED_VARYINGS_PS_HASHES");
    const ScopedUnsetEnv noVsOutPointSizeProbe("DXMT9_PROBE_DROP_VSOUT_POINT_SIZE");
    const ScopedUnsetEnv noVsOutHalfProbe("DXMT9_PROBE_HALF_VSOUT");

    testD3DBCDecodeAndClassificationFixtures();
    testPs30VFaceDecodeAndSourceContract();
    testPixelShaderOutputReceivesFixedFunctionFog();
    testTranslatedFragmentTailsSingleAlphaVariantAndFogVariant();
    testTranslatedLegacyBumpEnvRetainsFfpPsWithoutTails();
    testFfpFogTailIsCompileTimeGated();
    testLegacyShaderModelDecodeContracts();
    testPs30PredicatedInstructionLowersGuard();
    testD3DOpcodeNamesCoverUnsupportedSurface();
    testPs20SamplerRegisterSlotMapping();
    testPs20MipLodBiasEmitsShaderSideBias();
    testPs20MipLodBiasClearOmitsShaderSideBias();
    testFfpMipLodBiasEmitsShaderSideBias();
    testFfpMipLodBiasClearOmitsShaderSideBias();
    testTranslatedAlphaTestDebugStripOmitsTailDiscard();
    testFfpAlphaTestDebugStripOmitsDiscard();
    testPs30InputSemanticTexcoordMapping();
    testPairLocalVaryingLivenessKeepsHighTexcoordAndFog();
    testPs30TexkillLoweringContract();
    testPs20ColorInputUsesLegacyInputMapping();
    testVs30OutputSemanticMappingBySemanticIndex();
    testVsTempTrimIsOptInAndUsesObservedTempRange();
    testVsOutputScratchTrimIsOptInAndUsesObservedOutputRange();
    testVsOutTrimHashAllowlistScopesPairLiveness();
    testVsOutPointSizeProbeDropsOnlyPointSize();
    testVsOutHalfProbeNarrowsOnlyUserVaryings();
    testVertexDepthOutThrowsDeterministically();
    testVs30HighOutputRegisterSemanticMapping();
    testVs30VertexDeclarationTypeLoads();
    testVs30VertexDeclarationUDec3Load();
    testVs30MaterializesOnlyReadInputs();
    testVs30InputLayoutPreservesStreamBoundaries();
    testVs30MultiStreamVertexDeclarationLoads();
    testDefaultNoPixelVFlipAndNoVertexYFlip();
    testPs30WriteMaskSwizzleAndSourceModifiers();
    testPs30MissingSourceModifierCoverage();
    testPs30ConstIntSourceLowering();
    testPs30LoopRegisterConstIntLowering();
    testD3DBCDestModifierPartialPrecisionLowering();
    testPs30CentroidInputModifierLowersToMslInterpolation();
    testPs30IfElseFlowControlTranslation();
    testPs30LoopFlowControlTranslation();
    testPs30NestedLoopFlowControlTranslation();
    testPs30RepFlowControlTranslation();
    testPs30BreakcFlowControlTranslation();
    testPs30CallLabelRetFlowControlTranslation();
    testPs30CallnzLabelRetFlowControlTranslation();
    testPs30CallLabelNestedRetKeepsSubroutineBodyGuarded();
    testPs30PredicatedFlowControlTranslation();
    testPs30ArithmeticOpcodeLoweringContracts();
    testPs30TranscendentalOpcodeLoweringContracts();
    testPs30MatrixOpcodeLoweringContracts();
    testPs30TextureLodOpcodeLoweringContracts();
    testPs30TextureSamplerDimensionalityContracts();
    testPs11LegacyTexcoordTexLoweringContract();
    testVs11FixedFunctionOutputLoweringContract();
    testPs14TexcrdTexldTexdepthLoweringContract();
    testPs13LegacyTextureFamilyLoweringContract();
    testPs14BemLoweringContract();
    testReservedTexm3x3DiffStillThrowsDeterministically();
    testCallnzFixedOperandCountDecodeContract();
    testD3DBCFixedOperandCountDecodeContract();
    testPs30RelativeAddressingLowersTempDestinationIndex();
    testPs30RelativeAddressingLowersTempSourceIndex();
    testPs30IndexedConstSourceLowersToClampedConstAccess();
    testPs30IndexedConstDestinationLowersToClampedMutableConstWrite();
    testPs30FragmentPositionAndMissingInputContracts();
    testVs30MissingInputDefaultsToZero();
    testVs30RelativeAddressingLowersTexcoordDestinationIndex();
    testVs20IndexedConstDestinationLowersToClampedMutableConstWrite();
    testVs20DefLiteralWithRelAddrBitDoesNotDriftParser();
    testVs20IndexedConstSourceParserConsumesRelAddrDword();
    testVs20IndexedConstSourceLowersToClampedConstAccess();
    testVsRelativeAddrHonorsAddressComponent();
    testVsMovaHonorsDestinationWriteMask();
    testVs30VertexTextureFetchLowersDeterministically();
    testPs14ConstantSourcesClampBeforeArithmetic();
    testPs30ConstReadsBindConstantBufferInPlace();
    testVs30ConstReadsBindConstantBufferInPlace();
    testPs30DefConstantsHoistToImmutableLocalsAndWinAtUseSites();
    testVs20DefConstantsHoistToImmutableLocalsAndWinAtUseSites();
    testVs20RelativeConstReadWithDefKeepsRegisterFileArray();
  } catch (const TestFailure& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  } catch (const std::exception& error) {
    std::cerr << "unexpected exception: " << error.what() << '\n';
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
