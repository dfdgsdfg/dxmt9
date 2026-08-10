#include "../../../src/dxmt9/dxmt9_d3d9_bytecode.hpp"
#include "../../../src/dxmt9/dxmt9_shader_decoder.hpp"

#include <array>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

using dxmt9::d3d9bc::u32;

namespace bc = dxmt9::d3d9bc;
namespace detail = dxmt9::translator::detail_;

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

template <typename Actual, typename Expected>
void checkEqual(const Actual& actual, const Expected& expected, const std::string& message) {
  if (!(actual == expected)) {
    std::ostringstream out;
    out << message << " (expected " << expected << ", got " << actual << ")";
    fail(out.str());
  }
}

std::string opcodeLabel(u32 opcode, std::string_view name) {
  std::ostringstream out;
  out << name << " (0x" << std::hex << opcode << ")";
  return out.str();
}

struct OpcodeAudit {
  u32 opcode = 0;
  std::string_view name;
  int fixedOperandCount = -1;
  bool writesFirstOperand = false;
  bool textureSample = false;
  u32 matrixRows = 1;
};

constexpr int kUnsupportedFixedOperandCount = -1;

constexpr std::array<OpcodeAudit, 83> kOpcodeAudit = {{
    {bc::kD3DSIO_NOP, "nop", 0, false, false, 1},
    {bc::kD3DSIO_MOV, "mov", 2, true, false, 1},
    {bc::kD3DSIO_ADD, "add", 3, true, false, 1},
    {bc::kD3DSIO_SUB, "sub", 3, true, false, 1},
    {bc::kD3DSIO_MAD, "mad", 4, true, false, 1},
    {bc::kD3DSIO_MUL, "mul", 3, true, false, 1},
    {bc::kD3DSIO_RCP, "rcp", 2, true, false, 1},
    {bc::kD3DSIO_RSQ, "rsq", 2, true, false, 1},
    {bc::kD3DSIO_DP3, "dp3", 3, true, false, 1},
    {bc::kD3DSIO_DP4, "dp4", 3, true, false, 1},
    {bc::kD3DSIO_MIN, "min", 3, true, false, 1},
    {bc::kD3DSIO_MAX, "max", 3, true, false, 1},
    {bc::kD3DSIO_SLT, "slt", 3, true, false, 1},
    {bc::kD3DSIO_SGE, "sge", 3, true, false, 1},
    {bc::kD3DSIO_EXP, "exp", 2, true, false, 1},
    {bc::kD3DSIO_LOG, "log", 2, true, false, 1},
    {bc::kD3DSIO_LRP, "lrp", 4, true, false, 1},
    {bc::kD3DSIO_FRC, "frc", 2, true, false, 1},
    {bc::kD3DSIO_M4x4, "m4x4", 3, true, false, 4},
    {bc::kD3DSIO_M4x3, "m4x3", 3, true, false, 3},
    {bc::kD3DSIO_M3x4, "m3x4", 3, true, false, 4},
    {bc::kD3DSIO_M3x3, "m3x3", 3, true, false, 3},
    {bc::kD3DSIO_M3x2, "m3x2", 3, true, false, 2},
    {bc::kD3DSIO_CALL, "call", 1, false, false, 1},
    {bc::kD3DSIO_CALLNZ, "callnz", 2, false, false, 1},
    {bc::kD3DSIO_LOOP, "loop", kUnsupportedFixedOperandCount, false, false, 1},
    {bc::kD3DSIO_RET, "ret", 0, false, false, 1},
    {bc::kD3DSIO_ENDLOOP, "endloop", 0, false, false, 1},
    {bc::kD3DSIO_LABEL, "label", 1, false, false, 1},
    {bc::kD3DSIO_DCL, "dcl", kUnsupportedFixedOperandCount, false, false, 1},
    {bc::kD3DSIO_POW, "pow", 3, true, false, 1},
    {bc::kD3DSIO_CRS, "crs", 3, true, false, 1},
    {bc::kD3DSIO_SGN, "sgn", 2, true, false, 1},
    {bc::kD3DSIO_ABS, "abs", 2, true, false, 1},
    {bc::kD3DSIO_NRM, "nrm", 2, true, false, 1},
    {bc::kD3DSIO_SINCOS, "sincos", kUnsupportedFixedOperandCount, true, false, 1},
    {bc::kD3DSIO_REP, "rep", kUnsupportedFixedOperandCount, false, false, 1},
    {bc::kD3DSIO_ENDREP, "endrep", 0, false, false, 1},
    {bc::kD3DSIO_IF, "if", 1, false, false, 1},
    {bc::kD3DSIO_IFC, "ifc", 2, false, false, 1},
    {bc::kD3DSIO_ELSE, "else", 0, false, false, 1},
    {bc::kD3DSIO_ENDIF, "endif", 0, false, false, 1},
    {bc::kD3DSIO_BREAK, "break", 0, false, false, 1},
    {bc::kD3DSIO_BREAKC, "breakc", 2, false, false, 1},
    {bc::kD3DSIO_MOVA, "mova", 2, false, false, 1},
    {bc::kD3DSIO_DEFB, "defb", 2, true, false, 1},
    {bc::kD3DSIO_DEFI, "defi", 5, true, false, 1},
    {bc::kD3DSIO_TEXCOORD, "texcoord", kUnsupportedFixedOperandCount, false, false, 1},
    {bc::kD3DSIO_TEXKILL, "texkill", 1, false, false, 1},
    {bc::kD3DSIO_TEX, "tex", kUnsupportedFixedOperandCount, true, true, 1},
    {bc::kD3DSIO_TEXBEM, "texbem", kUnsupportedFixedOperandCount, false, false, 1},
    {bc::kD3DSIO_TEXBEML, "texbeml", kUnsupportedFixedOperandCount, false, false, 1},
    {bc::kD3DSIO_TEXREG2AR, "texreg2ar", kUnsupportedFixedOperandCount, false, false, 1},
    {bc::kD3DSIO_TEXREG2GB, "texreg2gb", kUnsupportedFixedOperandCount, false, false, 1},
    {bc::kD3DSIO_TEXM3x2PAD, "texm3x2pad", kUnsupportedFixedOperandCount, false, false, 1},
    {bc::kD3DSIO_TEXM3x2TEX, "texm3x2tex", kUnsupportedFixedOperandCount, false, false, 1},
    {bc::kD3DSIO_TEXM3x3PAD, "texm3x3pad", kUnsupportedFixedOperandCount, false, false, 1},
    {bc::kD3DSIO_TEXM3x3TEX, "texm3x3tex", kUnsupportedFixedOperandCount, false, false, 1},
    {bc::kD3DSIO_TEXM3x3DIFF, "texm3x3diff", kUnsupportedFixedOperandCount, false, false, 1},
    {bc::kD3DSIO_TEXM3x3SPEC, "texm3x3spec", kUnsupportedFixedOperandCount, false, false, 1},
    {bc::kD3DSIO_TEXM3x3VSPEC, "texm3x3vspec", kUnsupportedFixedOperandCount, false, false, 1},
    {bc::kD3DSIO_EXPP, "expp", 2, true, false, 1},
    {bc::kD3DSIO_LOGP, "logp", 2, true, false, 1},
    {bc::kD3DSIO_CND, "cnd", 4, true, false, 1},
    {bc::kD3DSIO_DEF, "def", 5, true, false, 1},
    {bc::kD3DSIO_TEXREG2RGB, "texreg2rgb", kUnsupportedFixedOperandCount, false, false, 1},
    {bc::kD3DSIO_TEXDP3TEX, "texdp3tex", kUnsupportedFixedOperandCount, false, false, 1},
    {bc::kD3DSIO_TEXM3x2DEPTH, "texm3x2depth", kUnsupportedFixedOperandCount, false, false, 1},
    {bc::kD3DSIO_TEXDP3, "texdp3", kUnsupportedFixedOperandCount, false, false, 1},
    {bc::kD3DSIO_TEXM3x3, "texm3x3", kUnsupportedFixedOperandCount, false, false, 1},
    {bc::kD3DSIO_TEXDEPTH, "texdepth", kUnsupportedFixedOperandCount, false, false, 1},
    {bc::kD3DSIO_CMP, "cmp", 4, true, false, 1},
    {bc::kD3DSIO_BEM, "bem", kUnsupportedFixedOperandCount, false, false, 1},
    {bc::kD3DSIO_DP2ADD, "dp2add", 4, true, false, 1},
    {bc::kD3DSIO_DSX, "dsx", 2, true, false, 1},
    {bc::kD3DSIO_DSY, "dsy", 2, true, false, 1},
    {bc::kD3DSIO_TEXLDD, "texldd", 5, true, true, 1},
    {bc::kD3DSIO_SETP, "setp", 2, false, false, 1},
    {bc::kD3DSIO_TEXLDL, "texldl", 3, true, true, 1},
    {bc::kD3DSIO_BREAKP, "breakp", 2, false, false, 1},
    {bc::kD3DSIO_PHASE, "phase", 0, false, false, 1},
    {bc::kD3DSIO_COMMENT, "comment", 0, false, false, 1},
    {bc::kD3DSIO_END, "end", 0, false, false, 1},
}};

int fixedOperandCountOrUnsupported(u32 opcode) {
  try {
    return static_cast<int>(detail::fixedOperandCount(opcode));
  } catch (const std::runtime_error& error) {
    checkEqual(std::string_view(error.what()), std::string_view("unsupported SM1.x opcode"),
               "unsupported fixed operand count reports stable error text");
    return kUnsupportedFixedOperandCount;
  }
}

void testNoThrowOperandCountProbeMatchesThrowingCompatibilitySeam() {
  static_assert(noexcept(detail::fixedOperandCountIfKnown(0u)));
  for (const auto& entry : kOpcodeAudit) {
    const auto count = detail::fixedOperandCountIfKnown(entry.opcode);
    if (entry.fixedOperandCount == kUnsupportedFixedOperandCount) {
      check(!count.has_value(),
            "unsupported opcode stays absent on the no-throw decode path");
    } else {
      check(count.has_value() &&
                static_cast<int>(*count) == entry.fixedOperandCount,
            "known opcode preserves its fixed operand count on the no-throw "
            "decode path");
    }
  }
}

void testKnownD3DSIOOpcodesHaveStableClassification() {
  for (size_t i = 0; i < kOpcodeAudit.size(); ++i) {
    const auto& entry = kOpcodeAudit[i];
    const auto label = opcodeLabel(entry.opcode, entry.name);

    if (i != 0) {
      check(kOpcodeAudit[i - 1].opcode < entry.opcode,
            "opcode audit table must stay sorted and duplicate-free");
    }

    checkEqual(detail::opcodeName(entry.opcode), std::string(entry.name),
               "opcodeName drift for " + label);
    checkEqual(fixedOperandCountOrUnsupported(entry.opcode), entry.fixedOperandCount,
               "fixedOperandCount drift for " + label);
    checkEqual(detail::opcodeWritesFirstOperand(entry.opcode), entry.writesFirstOperand,
               "opcodeWritesFirstOperand drift for " + label);
    checkEqual(detail::isTextureSampleOpcode(entry.opcode), entry.textureSample,
               "isTextureSampleOpcode drift for " + label);
    checkEqual(detail::matrixConstantRows(entry.opcode), entry.matrixRows,
               "matrixConstantRows drift for " + label);
  }
}

void testLegacyTextureOpcodesKeepSpecialDecodeClassification() {
  constexpr std::array<u32, 19> kLegacyTextureOpcodes = {{
      bc::kD3DSIO_TEXCOORD,
      bc::kD3DSIO_TEXBEM,
      bc::kD3DSIO_TEXBEML,
      bc::kD3DSIO_TEXREG2AR,
      bc::kD3DSIO_TEXREG2GB,
      bc::kD3DSIO_TEXM3x2PAD,
      bc::kD3DSIO_TEXM3x2TEX,
      bc::kD3DSIO_TEXM3x3PAD,
      bc::kD3DSIO_TEXM3x3TEX,
      bc::kD3DSIO_TEXM3x3DIFF,
      bc::kD3DSIO_TEXM3x3SPEC,
      bc::kD3DSIO_TEXM3x3VSPEC,
      bc::kD3DSIO_TEXREG2RGB,
      bc::kD3DSIO_TEXDP3TEX,
      bc::kD3DSIO_TEXM3x2DEPTH,
      bc::kD3DSIO_TEXDP3,
      bc::kD3DSIO_TEXM3x3,
      bc::kD3DSIO_TEXDEPTH,
      bc::kD3DSIO_BEM,
  }};

  for (const u32 opcode : kLegacyTextureOpcodes) {
    const auto label = opcodeLabel(opcode, detail::opcodeName(opcode));
    checkEqual(fixedOperandCountOrUnsupported(opcode), kUnsupportedFixedOperandCount,
               "legacy texture opcode keeps model-version-specific operand count for " + label);
    checkEqual(detail::opcodeWritesFirstOperand(opcode), false,
               "legacy texture opcode writes are handled by SM1-specific lowering for " + label);
    checkEqual(detail::isTextureSampleOpcode(opcode), false,
               "legacy texture opcode samples are handled by SM1-specific lowering for " + label);
  }
}

void testReservedTexm3x3DiffKeepsInvalidSm1Classification() {
  // Wine's SM1 table keeps the token/name but marks TEXM3x3DIFF with an
  // empty 0.0..0.0 shader-model range and no backend handler.
  constexpr u32 kOpcode = bc::kD3DSIO_TEXM3x3DIFF;

  checkEqual(detail::opcodeName(kOpcode), std::string("texm3x3diff"),
             "reserved TEXM3x3DIFF keeps a stable diagnostic name");
  checkEqual(fixedOperandCountOrUnsupported(kOpcode), kUnsupportedFixedOperandCount,
             "reserved TEXM3x3DIFF does not become a generic fixed-count opcode");
  checkEqual(detail::opcodeWritesFirstOperand(kOpcode), false,
             "reserved TEXM3x3DIFF writes must stay behind SM1-specific validation");
  checkEqual(detail::isTextureSampleOpcode(kOpcode), false,
             "reserved TEXM3x3DIFF must not allocate implicit sampler evidence");
}

}  // namespace

int main() {
  try {
    testNoThrowOperandCountProbeMatchesThrowingCompatibilitySeam();
    testKnownD3DSIOOpcodesHaveStableClassification();
    testLegacyTextureOpcodesKeepSpecialDecodeClassification();
    testReservedTexm3x3DiffKeepsInvalidSm1Classification();
  } catch (const TestFailure& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  } catch (const std::exception& error) {
    std::cerr << "unexpected exception: " << error.what() << '\n';
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
