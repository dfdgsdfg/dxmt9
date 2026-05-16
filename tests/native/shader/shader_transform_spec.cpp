#include "dxmt9/core.hpp"
#include "../../../src/dxmt9/dxmt9_d3d9_bytecode.hpp"
#include "../../../src/dxmt9/dxmt9_draw_shader.hpp"
#include "../../../src/dxmt9/dxmt9_ffp_shaders.hpp"
#include "../../../src/dxmt9/dxmt9_shader_decoder.hpp"
#include "../../../src/dxmt9/dxmt9_shader_translator.hpp"

#include <array>
#include <cstdlib>
#include <exception>
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

std::string translateVertex(std::span<const u32> words) {
  const auto shader = makeShader(words);
  DrawDesc desc{};
  desc.vertexShader = shader;
  return dxmt9::translator::makeTranslatedVertexSource(
      shader, dxmt9::drawshader::makeShaderSourceContext(desc));
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
  // Temp destination relative addressing is not a supported D3D9
  // destination class for this translator. The rel-addr DWORD that
  // follows the destination operand satisfies the parser, but the IR
  // layer must still throw deterministically instead of dropping the
  // dynamic index.
  return {
      makeVersionToken(false, 3, 0),
      makeInstructionToken(kD3DSIO_MOV, 2),
      makeRelativeDstToken(kD3DSPR_TEMP, 0),
      makeSrcToken(kD3DSPR_ADDR, 0),
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
// `cFloat[clamp(a0 + 5, 0, 255)]`.
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

void testPs30PredicatedInstructionLowersGuard() {
  const auto source = translatePixel(makePs30DecodeFixtureBytecode());
  checkContains(source, "bool p[16];", "predicated instruction declares predicate register storage");
  checkContains(source, "if (p[0]) {", "predicated instruction lowers to a p0 guard");
  checkContains(source, "outColor[1] = (cBool[2] != 0u ? float4(1.0f) : float4(0.0f));",
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

void testPs30InputSemanticTexcoordMapping() {
  const auto source = translatePixel(makePs30InputSemanticBytecode());
  checkContains(source, "dxmt9_select_texcoord(in, 3u)",
                "ps_3_0 dcl_texcoord semantic index maps input register reads by semantic index");
  checkContains(source, "tex2.sample(samp2", "ps_3_0 texture sample preserves sampler register mapping");
}

void testPs30TexkillLoweringContract() {
  const auto source = translatePixel(makePs30TexkillBytecode());
  checkContains(source, "// texkill r0", "ps_3_0 TEXKILL is named in emitted instruction comments");
  checkContains(source, "if ((r[0]).x < 0.0f || (r[0]).y < 0.0f)",
                "ps_3_0 TEXKILL lowers selected components to a negative-value discard predicate");
  checkContains(source, "discard_fragment()", "ps_3_0 TEXKILL lowers to Metal fragment discard");
  checkContains(source, "outColor[0] = cFloat[0]", "ps_3_0 TEXKILL does not terminate subsequent translation");
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
  checkContains(source, "outPosition = cFloat[0]", "vs_3_0 position semantic maps to Metal position output");
  checkContains(source, "outTexcoord[2] = cFloat[1]",
                "vs_3_0 texcoord semantic maps by semantic index rather than o-register index");
  checkContains(source, "outSecondaryColor = cFloat[2]", "vs_3_0 color1 semantic maps to secondary color output");
  checkNotContains(source, "outTexcoord[1] = cFloat[1]",
                   "vs_3_0 texcoord semantic does not fall back to raw output register index");
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
  checkContains(source, "outPosition = vin[0]",
                "POSITION semantic maps to Metal position despite high o-register index");
  checkContains(source, "outTexcoord[0] = vin[1]",
                "TEXCOORD0 semantic maps by semantic index, not output register 3");
  checkContains(source, "outTexcoord[1] = vin[2]",
                "TEXCOORD1 semantic maps by semantic index, not output register 4");
  checkNotContains(source, "outTexcoord[3] = vin[1]",
                   "TEXCOORD0 must not fall back to raw output register 3");
  checkNotContains(source, "outTexcoord[4] = vin[2]",
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
  checkContains(source, "r[0] = float4(cFloat[0].w, cFloat[0].z, cFloat[0].y, cFloat[0].x);",
                "ps_3_0 source swizzle is preserved in MOV transform");
  checkContains(source, "r[1] = dxmt9_merge(r[1],", "ps_3_0 partial write mask uses masked merge");
  checkContains(source, "-(float4(r[0].y, r[0].x, r[0].w, r[0].z))",
                "ps_3_0 negate source modifier wraps swizzled temp source");
  checkContains(source, "abs(cFloat[1])", "ps_3_0 abs source modifier wraps constant source");
  checkContains(source, ", 5u);", "ps_3_0 xz write mask is emitted as deterministic mask token");
  checkContains(source, "outColor[0] = r[1];", "ps_3_0 transformed masked result reaches color output");
}

void testPs30MissingSourceModifierCoverage() {
  const auto source = translatePixel(makePs30SourceModifierCoverageBytecode());
  checkContains(source, "r[0] = -(cFloat[0] * float4(2.0f) - float4(1.0f));",
                "ps_3_0 signneg source modifier lowers to negated signed x2 bias");
  checkContains(source, "r[1] = (float4(1.0f) - cFloat[1]);",
                "ps_3_0 complement source modifier lowers to 1-src");
  checkContains(source, "r[2] = (cFloat[2] * float4(2.0f));",
                "ps_3_0 x2 source modifier lowers to multiply by two");
  checkContains(source, "r[3] = -(cFloat[3] * float4(2.0f));",
                "ps_3_0 x2neg source modifier lowers to negated multiply by two");
  checkContains(source, "r[4] = ((cFloat[4]) / float4((cFloat[4]).z));",
                "ps_3_0 dz source modifier lowers to z-projected vector");
  checkContains(source, "r[5] = ((cFloat[5]) / float4((cFloat[5]).w));",
                "ps_3_0 dw source modifier lowers to w-projected vector");
  checkContains(source,
                "r[6] = select(float4(1.0f), float4(0.0f), "
                "((cBool[0] != 0u ? float4(1.0f) : float4(0.0f))) != float4(0.0f));",
                "ps_3_0 not source modifier lowers boolean-like source to inverted 0/1 float mask");
  checkContains(source, "outColor[0] = r[7];",
                "ps_3_0 source modifier coverage result reaches color output");
}

void testPs30IfElseFlowControlTranslation() {
  const auto source = translatePixel(makePs30IfElseBytecode());
  checkContains(source, "if ((cFloat[0]).x != 0.0f) {", "ps_3_0 IF condition lowers to scalar branch");
  checkContains(source, "r[0] = cFloat[1];", "ps_3_0 IF body is translated");
  checkContains(source, "} else {", "ps_3_0 ELSE lowers to structured branch");
  checkContains(source, "r[0] = cFloat[2];", "ps_3_0 ELSE body is translated");
  checkContains(source, "outColor[0] = r[0];", "ps_3_0 flow-control result reaches color output");
}

void testPs30LoopFlowControlTranslation() {
  const auto source = translatePixel(makePs30LoopBytecode());
  checkContains(source,
                "for (int dxmt9_loop_0 = 0, dxmt9_loopCount_0 = max(0, int(round(cFloat[0].x))); "
                "dxmt9_loop_0 < dxmt9_loopCount_0; ++dxmt9_loop_0) {",
                "ps_3_0 LOOP lowers to deterministic counted for-loop");
  checkContains(source, "r[0] = cFloat[1];", "ps_3_0 LOOP body is translated");
  checkContains(source, "// endloop", "ps_3_0 ENDLOOP opcode is preserved in generated source comments");
  checkContains(source, "outColor[0] = r[0];", "ps_3_0 LOOP result reaches color output");
}

void testPs30RepFlowControlTranslation() {
  const auto source = translatePixel(makePs30RepBytecode());
  checkContains(source,
                "for (int dxmt9_rep_0 = 0, dxmt9_repCount_0 = max(0, int(round(cFloat[2].x))); "
                "dxmt9_rep_0 < dxmt9_repCount_0; ++dxmt9_rep_0) {",
                "ps_3_0 REP lowers to deterministic counted for-loop");
  checkContains(source, "r[1] = cFloat[3];", "ps_3_0 REP body is translated");
  checkContains(source, "// endrep", "ps_3_0 ENDREP opcode is preserved in generated source comments");
  checkContains(source, "outColor[0] = r[1];", "ps_3_0 REP result reaches color output");
}

void testPs30BreakcFlowControlTranslation() {
  const auto source = translatePixel(makePs30BreakcBytecode());
  checkContains(source,
                "if ((cFloat[0]).x > (cFloat[1]).x) { break; }",
                "ps_3_0 BREAKC lowers comparison controls into a conditional break");
  checkContains(source, "r[0] = cFloat[3];", "ps_3_0 BREAKC body continues after the guard");
  checkContains(source, "outColor[0] = r[0];", "ps_3_0 BREAKC result reaches color output");
}

void testPs30CallLabelRetFlowControlTranslation() {
  const auto source = translatePixel(makePs30CallLabelRetBytecode());
  checkContains(source, "r[0] = cFloat[4];", "ps_3_0 CALL/LABEL body is translated");
  checkContains(source, "outColor[0] = r[0];", "ps_3_0 CALL/LABEL/RET result reaches color output");
}

void testPs30CallnzLabelRetFlowControlTranslation() {
  const auto source = translatePixel(makePs30CallnzLabelRetBytecode());
  checkContains(source,
                "if (((cBool[0] != 0u ? float4(1.0f) : float4(0.0f))).x != 0.0f) {",
                "ps_3_0 CALLNZ guards the inlined call body with a bool-source nonzero test");
  checkContains(source, "r[0] = cFloat[4];", "ps_3_0 CALLNZ/LABEL body is translated");
  checkContains(source, "outColor[0] = r[0];", "ps_3_0 CALLNZ/LABEL/RET result reaches color output");
}

void testPs30ArithmeticOpcodeLoweringContracts() {
  const auto source = translatePixel(makePs30ArithmeticOpcodeMatrixBytecode());
  checkContains(source, "r[0] = (cFloat[0] * cFloat[1] + cFloat[2]);",
                "MAD lowers to multiply-add expression");
  checkContains(source, "float4(dot((r[0]).xyz, (cFloat[3]).xyz))", "DP3 lowers to xyz dot splat");
  checkContains(source, "float4(dot(r[0], cFloat[4]))", "DP4 lowers to full-vector dot splat");
  checkContains(source, "select(cFloat[7], cFloat[6], cFloat[5] >= float4(0.0f))",
                "CMP lowers to sign-test select with source order preserved");
  checkContains(source, "select(float4(0.0f), float4(1.0f), (cFloat[8]) < (cFloat[9]))",
                "SLT lowers to boolean select mask");
  checkContains(source, "select(float4(0.0f), float4(1.0f), (cFloat[10]) >= (cFloat[11]))",
                "SGE lowers to boolean select mask");
  checkContains(source, "pow(cFloat[12], cFloat[13])", "POW lowers to pow source expression");
  checkContains(source, "outColor[0] = r[6];", "arithmetic opcode matrix result reaches color output");
}

void testPs30TranscendentalOpcodeLoweringContracts() {
  const auto source = translatePixel(makePs30TranscendentalOpcodeBytecode());
  checkContains(source, "float4(sin(cFloat[0]), cos(cFloat[0]), 0.0f, 0.0f)",
                "SINCOS lowers to sin/cos vector construction");
  checkContains(source, "float4(log2(max(cFloat[1], float4(1.0e-8f))))",
                "LOG lowers to clamped log2 expression");
  checkContains(source, "float4(exp2(cFloat[2]))", "EXP lowers to exp2 expression");
  checkContains(source, "outColor[0] = r[2];", "transcendental opcode result reaches color output");
}

void testPs30MatrixOpcodeLoweringContracts() {
  const auto source = translatePixel(makePs30MatrixOpcodeBytecode());
  checkContains(source, "dot(cFloat[0], cFloat[4])", "M4x4 starts at the declared matrix constant base");
  checkContains(source, "dot(cFloat[0], cFloat[7])", "M4x4 consumes four matrix rows from the base constant");
  checkContains(source, "dot((cFloat[1]).xyz, cFloat[8].xyz)", "M3x3 starts at the declared matrix constant base");
  checkContains(source, "dot((cFloat[1]).xyz, cFloat[10].xyz)", "M3x3 consumes three xyz matrix rows");
  checkContains(source, "outColor[0] = r[1];", "matrix opcode result reaches color output");
}

void testPs30TextureLodOpcodeLoweringContracts() {
  const auto source = translatePixel(makePs30TextureLodOpcodeBytecode());
  checkContains(source, "texture2d<float> tex3 [[texture(3)]]", "TEXLDD declares the referenced sampler slot");
  checkContains(source, "sampler samp4 [[sampler(4)]]", "TEXLDL declares the referenced sampler state slot");
  checkContains(source,
                "tex3.sample(samp3, (cFloat[0]).xy, gradient2d((cFloat[2]).xy, (cFloat[3]).xy))",
                "TEXLDD lowers to an explicit-gradient sample call");
  checkContains(source, "tex4.sample(samp4, (cFloat[1]).xy, level(cFloat[1].w))",
                "TEXLDL lowers to explicit level sample call");
  checkContains(source, "outColor[0] = r[1];", "texture LOD opcode result reaches color output");
}

void testPs30TextureSamplerDimensionalityContracts() {
  const auto cubeSource = translatePixel(makePs30TexlddSamplerTypeBytecode(3u));
  checkContains(cubeSource, "texturecube<float> tex3 [[texture(3)]]",
                "cube DCL sampler declares texturecube in MSL");
  checkContains(cubeSource,
                "tex3.sample(samp3, (cFloat[0]).xyz, gradientcube((cFloat[2]).xyz, (cFloat[3]).xyz))",
                "cube TEXLDD lowers to xyz explicit-gradient sample");

  const auto volumeSource = translatePixel(makePs30TexlddSamplerTypeBytecode(4u));
  checkContains(volumeSource, "texture3d<float> tex3 [[texture(3)]]",
                "volume DCL sampler declares texture3d in MSL");
  checkContains(volumeSource,
                "tex3.sample(samp3, (cFloat[0]).xyz, gradient3d((cFloat[2]).xyz, (cFloat[3]).xyz))",
                "volume TEXLDD lowers to xyz explicit-gradient sample");
}

void testPs11LegacyTexcoordTexLoweringContract() {
  const auto source = translatePixel(makePs11TexcoordTexBytecode());
  checkContains(source, "for (uint i = 0; i < 8u; ++i) { outTexcoord[i] = dxmt9_select_texcoord(in, i); }",
                "ps_1_1 initializes mutable t# registers from interpolated texcoords");
  checkContains(source, "outTexcoord[0] = float4(saturate((dxmt9_select_texcoord(in, 0u)).xyz), 1.0f);",
                "ps_1_1 TEXCOORD clamps the stage texcoord and forces w=1");
  checkContains(source, "texture2d<float> tex0 [[texture(0)]]", "ps_1_1 TEX binds the destination stage texture");
  checkContains(source, "outTexcoord[0] = tex0.sample(samp0, (outTexcoord[0]).xy);",
                "ps_1_1 TEX samples stage 0 through destination t0");
  checkContains(source, "outColor[0] = outTexcoord[0];", "ps_1_1 t# source reaches color output");
}

void testPs14TexcrdTexldTexdepthLoweringContract() {
  const auto source = translatePixel(makePs14TexcrdTexldDepthBytecode());
  checkContains(source, "r[0] = dxmt9_select_texcoord(in, 0u);",
                "ps_1_4 TEXCRD copies explicit t# source to r#");
  checkContains(source, "texture2d<float> tex1 [[texture(1)]]", "ps_1_4 TEXLD binds destination stage texture");
  checkContains(source, "r[1] = tex1.sample(samp1, (dxmt9_select_texcoord(in, 1u)).xy);",
                "ps_1_4 TEXLD samples destination stage with explicit source coords");
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
  checkContains(source, "reflect(normalize((cFloat[0]).xyz), normalize(dxmt9_texm.xyz))",
                "TEXM3x3SPEC forms reflection coords from explicit eye vector");
  checkContains(source, "reflect(normalize(float3(outTexcoord[2].w, outTexcoord[3].w, outTexcoord[4].w))",
                "TEXM3x3VSPEC forms eye vector from texture-coordinate w components");
  checkContains(source, "outDepth = dxmt9_texm.y == 0.0f ? 1.0f : clamp(dxmt9_texm.x / dxmt9_texm.y, 0.0f, 1.0f);",
                "TEXM3x2DEPTH writes fragment depth with zero-y guard");
}

void testPs14BemLoweringContract() {
  const auto source = translatePixel(makePs14BemBytecode());
  checkContains(source, "r[0] = float4((dxmt9_select_texcoord(in, 0u)).xy + float2(ffpPs.bumpEnvMat[0].x",
                "ps_1_4 BEM writes bumped coordinates using destination-stage bump matrix");
}

void testReservedTexm3x3DiffStillThrowsDeterministically() {
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
  checkContains(source, "r[0] = float4(dot(cFloat[0], cFloat[1]));",
                "known DP4 opcode decodes with its fixed operand count");
  checkContains(source, "r[1] = (r[0] * cFloat[2] + cFloat[3]);",
                "known MAD opcode decodes with its fixed operand count");
  checkContains(source, "outColor[0] = r[1];", "fixed operand-count decode result reaches color output");
}

void testPs30RelativeAddressingLowersTempDestinationIndex() {
  const auto source = translatePixel(makePs30RelativeAddressingBytecode());
  checkContains(source,
                "r[clamp(a0 + 0, 0, 31)] = cFloat[0];",
                "ps_3_0 temp destination relative addressing lowers to a clamped r[] write");
}

void testVs20IndexedConstDestinationLowersToClampedMutableConstWrite() {
  const auto source = translateVertex(makeVs20IndexedConstDestinationBytecode());

  checkContains(source, "float4 cFloat[256];",
                "indexed const destination keeps a mutable full-size cFloat array");
  checkContains(source, "cFloat[clamp(a0 + 5, 0, 255)] = cFloat[1];",
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
  checkContains(source, "cFloat[clamp(a0 + 5, 0, 255)]",
                "indexed const source emits clamped a0+N access");
  // The mova lowers to the existing a0 = int(round(...)) pattern.
  checkContains(source, "a0 = int(round(",
                "MOVA still lowers to the address-register assignment");
  // Indexed access forces pointer aliasing onto the full constant
  // buffer so the clamp range stays valid.
  checkContains(source, "constant float4* cFloat = ",
                "indexed const access forces pointer aliasing for cFloat");
  // Explicit guarantee against the previous silent-drop bug.
  checkNotContains(source, "cFloat[5]u",
                   "indexed access is no longer rewritten as a static cFloat[5] read");
}

}  // namespace

int main() {
  try {
    const ScopedUnsetEnv noFragmentColor("DXMT_DEBUG_FORCE_FRAGMENT_COLOR");
    const ScopedUnsetEnv noFragmentMode("DXMT_DEBUG_FRAGMENT_MODE");
    const ScopedUnsetEnv noFullscreenVertex("DXMT_DEBUG_FORCE_FULLSCREEN_VERTEX");
    const ScopedUnsetEnv noPixelVFlip("DXMT_DEBUG_FORCE_PIXEL_V_FLIP");
    const ScopedUnsetEnv noVertexYFlip("DXMT_DEBUG_FLIP_VERTEX_Y");

    testD3DBCDecodeAndClassificationFixtures();
    testPs30PredicatedInstructionLowersGuard();
    testD3DOpcodeNamesCoverUnsupportedSurface();
    testPs20SamplerRegisterSlotMapping();
    testPs30InputSemanticTexcoordMapping();
    testPs30TexkillLoweringContract();
    testPs20ColorInputUsesLegacyInputMapping();
    testVs30OutputSemanticMappingBySemanticIndex();
    testVertexDepthOutThrowsDeterministically();
    testVs30HighOutputRegisterSemanticMapping();
    testVs30VertexDeclarationTypeLoads();
    testVs30InputLayoutPreservesStreamBoundaries();
    testVs30MultiStreamVertexDeclarationLoads();
    testDefaultNoPixelVFlipAndNoVertexYFlip();
    testPs30WriteMaskSwizzleAndSourceModifiers();
    testPs30MissingSourceModifierCoverage();
    testPs30IfElseFlowControlTranslation();
    testPs30LoopFlowControlTranslation();
    testPs30RepFlowControlTranslation();
    testPs30BreakcFlowControlTranslation();
    testPs30CallLabelRetFlowControlTranslation();
    testPs30CallnzLabelRetFlowControlTranslation();
    testPs30ArithmeticOpcodeLoweringContracts();
    testPs30TranscendentalOpcodeLoweringContracts();
    testPs30MatrixOpcodeLoweringContracts();
    testPs30TextureLodOpcodeLoweringContracts();
    testPs30TextureSamplerDimensionalityContracts();
    testPs11LegacyTexcoordTexLoweringContract();
    testPs14TexcrdTexldTexdepthLoweringContract();
    testPs13LegacyTextureFamilyLoweringContract();
    testPs14BemLoweringContract();
    testReservedTexm3x3DiffStillThrowsDeterministically();
    testCallnzFixedOperandCountDecodeContract();
    testD3DBCFixedOperandCountDecodeContract();
    testPs30RelativeAddressingLowersTempDestinationIndex();
    testVs20IndexedConstDestinationLowersToClampedMutableConstWrite();
    testVs20DefLiteralWithRelAddrBitDoesNotDriftParser();
    testVs20IndexedConstSourceParserConsumesRelAddrDword();
    testVs20IndexedConstSourceLowersToClampedConstAccess();
  } catch (const TestFailure& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  } catch (const std::exception& error) {
    std::cerr << "unexpected exception: " << error.what() << '\n';
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
