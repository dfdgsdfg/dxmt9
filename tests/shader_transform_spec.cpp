#include "dxmt9/core.hpp"
#include "../src/dxmt9/dxmt9_d3d9_bytecode.hpp"
#include "../src/dxmt9/dxmt9_draw_shader.hpp"
#include "../src/dxmt9/dxmt9_shader_translator.hpp"

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

namespace {

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

[[noreturn]] void fail(std::string message) {
  throw TestFailure(std::move(message));
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

u32 makeSwizzle(u32 x, u32 y, u32 z, u32 w) {
  return (x & 0x3u) | ((y & 0x3u) << 2) | ((z & 0x3u) << 4) | ((w & 0x3u) << 6);
}

u32 makeDclSemanticToken(u32 usage, u32 usageIndex = 0u) {
  return (1u << 31) | (usage & 0xfu) | ((usageIndex & 0xfu) << 16);
}

u32 makeDclTextureCoordToken(u32 writeMask = 0xfu) {
  return (1u << 31) | ((writeMask & 0xfu) << 16);
}

u32 makeDclSamplerTypeToken() {
  return (1u << 31) | encodeRegisterType(dxmt9::d3d9bc::kD3DSPR_INPUT);
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
      makeInstructionToken(kD3DSIO_TEXLDD, 3),
      makeDstToken(kD3DSPR_TEMP, 0),
      makeSrcToken(kD3DSPR_CONST, 0),
      makeSrcToken(kD3DSPR_SAMPLER, 3),
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
      makeSrcToken(kD3DSPR_CONST, 0),
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
  checkEqual(module.instructions[1].operands.size(), size_t{3},
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

void testVs30OutputSemanticMappingBySemanticIndex() {
  const auto source = translateVertex(makeVs30OutputSemanticBytecode());
  checkContains(source, "outPosition = cFloat[0]", "vs_3_0 position semantic maps to Metal position output");
  checkContains(source, "outTexcoord[2] = cFloat[1]",
                "vs_3_0 texcoord semantic maps by semantic index rather than o-register index");
  checkContains(source, "outSecondaryColor = cFloat[2]", "vs_3_0 color1 semantic maps to secondary color output");
  checkNotContains(source, "outTexcoord[1] = cFloat[1]",
                   "vs_3_0 texcoord semantic does not fall back to raw output register index");
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

void testPs30CallLabelRetFlowControlTranslation() {
  const auto source = translatePixel(makePs30CallLabelRetBytecode());
  checkContains(source, "// call label 7", "ps_3_0 CALL target is preserved in generated source comments");
  checkContains(source, "do {", "ps_3_0 CALL opens a deterministic single-pass call block");
  checkContains(source, "// label 7", "ps_3_0 LABEL target is preserved in generated source comments");
  checkContains(source, "r[0] = cFloat[4];", "ps_3_0 CALL/LABEL body is translated");
  checkContains(source, "break;", "ps_3_0 RET inside CALL lowers to block break");
  checkContains(source, "} while (false);", "ps_3_0 RET closes the single-pass call block");
  checkContains(source, "outColor[0] = r[0];", "ps_3_0 CALL/LABEL/RET result reaches color output");
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
  checkContains(source, "tex3.sample(samp3, (cFloat[0]).xy)", "TEXLDD lowers to deterministic sample call");
  checkContains(source, "tex4.sample(samp4, (cFloat[1]).xy, level(cFloat[1].w))",
                "TEXLDL lowers to explicit level sample call");
  checkContains(source, "outColor[0] = r[1];", "texture LOD opcode result reaches color output");
}

void testD3DBCFixedOperandCountDecodeContract() {
  const auto source = translatePixel(makePs30FixedOperandCountDecodeBytecode());
  checkContains(source, "r[0] = float4(dot(cFloat[0], cFloat[1]));",
                "known DP4 opcode decodes with its fixed operand count");
  checkContains(source, "r[1] = (r[0] * cFloat[2] + cFloat[3]);",
                "known MAD opcode decodes with its fixed operand count");
  checkContains(source, "outColor[0] = r[1];", "fixed operand-count decode result reaches color output");
}

void testPs30RelativeAddressingThrowsDeterministically() {
  checkThrowsContains(
      [] {
        (void)translatePixel(makePs30RelativeAddressingBytecode());
      },
      "relative addressing is not supported yet",
      "ps_3_0 relative-addressing bytecode reports deterministic unsupported contract");
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
    testPs20SamplerRegisterSlotMapping();
    testPs30InputSemanticTexcoordMapping();
    testVs30OutputSemanticMappingBySemanticIndex();
    testDefaultNoPixelVFlipAndNoVertexYFlip();
    testPs30WriteMaskSwizzleAndSourceModifiers();
    testPs30IfElseFlowControlTranslation();
    testPs30LoopFlowControlTranslation();
    testPs30RepFlowControlTranslation();
    testPs30CallLabelRetFlowControlTranslation();
    testPs30ArithmeticOpcodeLoweringContracts();
    testPs30TranscendentalOpcodeLoweringContracts();
    testPs30MatrixOpcodeLoweringContracts();
    testPs30TextureLodOpcodeLoweringContracts();
    testD3DBCFixedOperandCountDecodeContract();
    testPs30RelativeAddressingThrowsDeterministically();
  } catch (const TestFailure& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  } catch (const std::exception& error) {
    std::cerr << "unexpected exception: " << error.what() << '\n';
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
