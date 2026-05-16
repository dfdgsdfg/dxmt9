#include "dxmt9_shader_decoder.hpp"

#include "dxmt9_d3d9_bytecode.hpp"
#include "dxmt9_draw_shader.hpp"
#include "dxmt9/assert.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// D3D9 bytecode → SpirvModule decoding layer. Token unpacking, opcode tables,
// per-module analysis passes, and the streaming bytecode parser. The Metal IR
// half (dxmt9_shader_metal_ir.cpp) consumes everything declared in
// dxmt9_shader_decoder.hpp; nothing in this TU emits MSL source text.

namespace dxmt9::translator::detail_ {

using namespace ::dxmt9::core;
using namespace ::dxmt9::d3d9bc;
using namespace ::dxmt9::ffp;

namespace {

constexpr u32 kD3DSPTextureTypeShift = 27u;
constexpr u32 kD3DSPTextureTypeMask = 0x78000000u;
constexpr u32 kD3DSTT2D = 2u;
constexpr u32 kD3DSTTCube = 3u;
constexpr u32 kD3DSTTVolume = 4u;

::dxmt9::core::TextureType decodeDclTextureType(u32 token) {
  switch ((token & kD3DSPTextureTypeMask) >> kD3DSPTextureTypeShift) {
    case kD3DSTTCube:
      return ::dxmt9::core::TextureType::Cube;
    case kD3DSTTVolume:
      return ::dxmt9::core::TextureType::Volume;
    case kD3DSTT2D:
    default:
      return ::dxmt9::core::TextureType::TwoD;
  }
}

}  // namespace

std::array<u8, 4> decodeSwizzle(u32 token) {
  return {static_cast<u8>((token >> 16) & 0x3u), static_cast<u8>((token >> 18) & 0x3u),
          static_cast<u8>((token >> 20) & 0x3u), static_cast<u8>((token >> 22) & 0x3u)};
}

u32 decodeRegisterType(u32 token) {
  return ((token >> 28) & 0x7u) | (((token >> 11) & 0x3u) << 3);
}

u32 decodeRegisterIndex(u32 token) {
  return token & 0x7ffu;
}

u32 decodeSourceModifier(u32 token) {
  return (token >> 24) & 0xfu;
}

u32 decodeDestModifier(u32 token) {
  return (token >> 20) & 0xfu;
}

u32 decodeWriteMask(u32 token) {
  return (token >> 16) & 0xfu;
}

bool tokenHasRelativeAddressing(u32 token) {
  return ((token >> 13) & 0x1u) != 0;
}

D3DRegisterKind decodeRegisterKind(u32 type, D3DShaderStage stage) {
  switch (type) {
    case kD3DSPR_TEMP:
      return D3DRegisterKind::Temp;
    case kD3DSPR_INPUT:
      return D3DRegisterKind::Input;
    case kD3DSPR_CONST:
      return D3DRegisterKind::ConstFloat;
    case kD3DSPR_ADDR:
      return stage == D3DShaderStage::Vertex ? D3DRegisterKind::Address : D3DRegisterKind::Input;
    case kD3DSPR_RASTOUT:
      return D3DRegisterKind::RastOut;
    case kD3DSPR_ATTROUT:
      return D3DRegisterKind::AttrOut;
    case kD3DSPR_TEXCRDOUT:
      return D3DRegisterKind::TexCoordOut;
    case kD3DSPR_CONSTINT:
      return D3DRegisterKind::ConstInt;
    case kD3DSPR_COLOROUT:
      return D3DRegisterKind::ColorOut;
    case kD3DSPR_DEPTHOUT:
      return D3DRegisterKind::DepthOut;
    case kD3DSPR_SAMPLER:
      return D3DRegisterKind::Sampler;
    case kD3DSPR_CONSTBOOL:
      return D3DRegisterKind::ConstBool;
    case kD3DSPR_LOOP:
      return D3DRegisterKind::Loop;
    case kD3DSPR_MISCTYPE:
      return D3DRegisterKind::MiscType;
    case kD3DSPR_PREDICATE:
      return D3DRegisterKind::Predicate;
    default:
      return D3DRegisterKind::Unknown;
  }
}

D3DRegisterRef decodeRegisterRef(u32 token, D3DShaderStage stage) {
  return {decodeRegisterKind(decodeRegisterType(token), stage), decodeRegisterIndex(token)};
}

u32 decodeLabelIndex(u32 token) {
  return token & 0x7ffu;
}

std::string opcodeName(u32 opcode) {
  switch (opcode) {
    case kD3DSIO_NOP:
      return "nop";
    case kD3DSIO_MOV:
      return "mov";
    case kD3DSIO_ADD:
      return "add";
    case kD3DSIO_SUB:
      return "sub";
    case kD3DSIO_MAD:
      return "mad";
    case kD3DSIO_MUL:
      return "mul";
    case kD3DSIO_RCP:
      return "rcp";
    case kD3DSIO_RSQ:
      return "rsq";
    case kD3DSIO_DP3:
      return "dp3";
    case kD3DSIO_DP4:
      return "dp4";
    case kD3DSIO_MIN:
      return "min";
    case kD3DSIO_MAX:
      return "max";
    case kD3DSIO_SLT:
      return "slt";
    case kD3DSIO_SGE:
      return "sge";
    case kD3DSIO_EXP:
      return "exp";
    case kD3DSIO_LOG:
      return "log";
    case kD3DSIO_M4x4:
      return "m4x4";
    case kD3DSIO_M4x3:
      return "m4x3";
    case kD3DSIO_M3x4:
      return "m3x4";
    case kD3DSIO_M3x3:
      return "m3x3";
    case kD3DSIO_M3x2:
      return "m3x2";
    case kD3DSIO_CALL:
      return "call";
    case kD3DSIO_CALLNZ:
      return "callnz";
    case kD3DSIO_LRP:
      return "lrp";
    case kD3DSIO_FRC:
      return "frc";
    case kD3DSIO_SINCOS:
      return "sincos";
    case kD3DSIO_REP:
      return "rep";
    case kD3DSIO_ENDREP:
      return "endrep";
    case kD3DSIO_IF:
      return "if";
    case kD3DSIO_IFC:
      return "ifc";
    case kD3DSIO_ELSE:
      return "else";
    case kD3DSIO_ENDIF:
      return "endif";
    case kD3DSIO_BREAK:
      return "break";
    case kD3DSIO_BREAKC:
      return "breakc";
    case kD3DSIO_RET:
      return "ret";
    case kD3DSIO_LOOP:
      return "loop";
    case kD3DSIO_ENDLOOP:
      return "endloop";
    case kD3DSIO_LABEL:
      return "label";
    case kD3DSIO_DCL:
      return "dcl";
    case kD3DSIO_DEFB:
      return "defb";
    case kD3DSIO_DEFI:
      return "defi";
    case kD3DSIO_POW:
      return "pow";
    case kD3DSIO_CRS:
      return "crs";
    case kD3DSIO_SGN:
      return "sgn";
    case kD3DSIO_ABS:
      return "abs";
    case kD3DSIO_NRM:
      return "nrm";
    case kD3DSIO_CND:
      return "cnd";
    case kD3DSIO_DEF:
      return "def";
    case kD3DSIO_TEXCOORD:
      return "texcoord";
    case kD3DSIO_TEX:
      return "tex";
    case kD3DSIO_TEXKILL:
      return "texkill";
    case kD3DSIO_TEXBEM:
      return "texbem";
    case kD3DSIO_TEXBEML:
      return "texbeml";
    case kD3DSIO_TEXREG2AR:
      return "texreg2ar";
    case kD3DSIO_TEXREG2GB:
      return "texreg2gb";
    case kD3DSIO_TEXM3x2PAD:
      return "texm3x2pad";
    case kD3DSIO_TEXM3x2TEX:
      return "texm3x2tex";
    case kD3DSIO_TEXM3x3PAD:
      return "texm3x3pad";
    case kD3DSIO_TEXM3x3TEX:
      return "texm3x3tex";
    case kD3DSIO_TEXM3x3DIFF:
      return "texm3x3diff";
    case kD3DSIO_TEXM3x3SPEC:
      return "texm3x3spec";
    case kD3DSIO_TEXM3x3VSPEC:
      return "texm3x3vspec";
    case kD3DSIO_TEXDEPTH:
      return "texdepth";
    case kD3DSIO_CMP:
      return "cmp";
    case kD3DSIO_BEM:
      return "bem";
    case kD3DSIO_TEXREG2RGB:
      return "texreg2rgb";
    case kD3DSIO_TEXDP3TEX:
      return "texdp3tex";
    case kD3DSIO_TEXM3x2DEPTH:
      return "texm3x2depth";
    case kD3DSIO_TEXDP3:
      return "texdp3";
    case kD3DSIO_TEXM3x3:
      return "texm3x3";
    case kD3DSIO_DP2ADD:
      return "dp2add";
    case kD3DSIO_DSX:
      return "dsx";
    case kD3DSIO_DSY:
      return "dsy";
    case kD3DSIO_TEXLDD:
      return "texldd";
    case kD3DSIO_SETP:
      return "setp";
    case kD3DSIO_TEXLDL:
      return "texldl";
    case kD3DSIO_BREAKP:
      return "breakp";
    case kD3DSIO_MOVA:
      return "mova";
    case kD3DSIO_EXPP:
      return "expp";
    case kD3DSIO_LOGP:
      return "logp";
    case kD3DSIO_PHASE:
      return "phase";
    case kD3DSIO_COMMENT:
      return "comment";
    case kD3DSIO_END:
      return "end";
    default:
      return "opcode_" + std::to_string(opcode);
  }
}

u32 fixedOperandCount(u32 opcode) {
  switch (opcode) {
    case kD3DSIO_NOP:
    case kD3DSIO_PHASE:
      return 0;
    case kD3DSIO_ELSE:
    case kD3DSIO_ENDIF:
    case kD3DSIO_ENDLOOP:
    case kD3DSIO_ENDREP:
    case kD3DSIO_RET:
    case kD3DSIO_BREAK:
      return 0;
    case kD3DSIO_MOV:
    case kD3DSIO_DEFB:
    case kD3DSIO_RCP:
    case kD3DSIO_RSQ:
    case kD3DSIO_FRC:
    case kD3DSIO_DSX:
    case kD3DSIO_DSY:
    case kD3DSIO_SETP:
    case kD3DSIO_BREAKP:
    case kD3DSIO_MOVA:
    case kD3DSIO_LOG:
    case kD3DSIO_LOGP:
    case kD3DSIO_EXP:
    case kD3DSIO_EXPP:
    case kD3DSIO_SGN:
    case kD3DSIO_ABS:
    case kD3DSIO_NRM:
      return 2;
    case kD3DSIO_LABEL:
    case kD3DSIO_CALL:
    case kD3DSIO_IF:
    case kD3DSIO_TEXKILL:
    case kD3DSIO_LOOP:
    case kD3DSIO_REP:
      return 1;
    case kD3DSIO_IFC:
    case kD3DSIO_BREAKC:
    case kD3DSIO_CALLNZ:
      // IFC/BREAKC store the comparison mode in instruction.controls.
      // CALLNZ stores a literal label token followed by a condition source.
      return 2;
    case kD3DSIO_ADD:
    case kD3DSIO_SUB:
    case kD3DSIO_MUL:
    case kD3DSIO_DP3:
    case kD3DSIO_DP4:
    case kD3DSIO_MIN:
    case kD3DSIO_MAX:
    case kD3DSIO_POW:
    case kD3DSIO_CRS:
    case kD3DSIO_TEXLDL:
    case kD3DSIO_SLT:
    case kD3DSIO_SGE:
    case kD3DSIO_M4x4:
    case kD3DSIO_M4x3:
    case kD3DSIO_M3x4:
    case kD3DSIO_M3x3:
    case kD3DSIO_M3x2:
      return 3;
    case kD3DSIO_TEXLDD:
      return 5;
    case kD3DSIO_MAD:
    case kD3DSIO_LRP:
    case kD3DSIO_CND:
    case kD3DSIO_CMP:
    case kD3DSIO_DP2ADD:
      return 4;
    case kD3DSIO_DEF:
    case kD3DSIO_DEFI:
      return 5;
    case kD3DSIO_COMMENT:
    case kD3DSIO_END:
      return 0;
    default:
      throw std::runtime_error("unsupported SM1.x opcode");
  }
}

bool opcodeWritesFirstOperand(u32 opcode) {
  switch (opcode) {
    case kD3DSIO_DEF:
    case kD3DSIO_DEFI:
    case kD3DSIO_DEFB:
    case kD3DSIO_MOV:
    case kD3DSIO_ADD:
    case kD3DSIO_SUB:
    case kD3DSIO_MUL:
    case kD3DSIO_MAD:
    case kD3DSIO_MIN:
    case kD3DSIO_MAX:
    case kD3DSIO_SLT:
    case kD3DSIO_SGE:
    case kD3DSIO_EXP:
    case kD3DSIO_LOG:
    case kD3DSIO_EXPP:
    case kD3DSIO_LOGP:
    case kD3DSIO_SINCOS:
    case kD3DSIO_M4x4:
    case kD3DSIO_M4x3:
    case kD3DSIO_M3x4:
    case kD3DSIO_M3x3:
    case kD3DSIO_M3x2:
    case kD3DSIO_RCP:
    case kD3DSIO_RSQ:
    case kD3DSIO_FRC:
    case kD3DSIO_LRP:
    case kD3DSIO_DP3:
    case kD3DSIO_DP4:
    case kD3DSIO_CND:
    case kD3DSIO_CMP:
    case kD3DSIO_DP2ADD:
    case kD3DSIO_POW:
    case kD3DSIO_CRS:
    case kD3DSIO_SGN:
    case kD3DSIO_ABS:
    case kD3DSIO_NRM:
    case kD3DSIO_TEX:
    case kD3DSIO_DSX:
    case kD3DSIO_DSY:
    case kD3DSIO_TEXLDD:
    case kD3DSIO_TEXLDL:
      return true;
    default:
      return false;
  }
}

namespace {

struct LabelRange {
  u32 label = 0;
  size_t labelInstruction = 0;
  size_t bodyBegin = 0;
  size_t retInstruction = 0;
};

std::optional<size_t> findLabelRange(const std::vector<LabelRange>& labels, u32 label) {
  for (size_t i = 0; i < labels.size(); ++i) {
    if (labels[i].label == label) {
      return i;
    }
  }
  return std::nullopt;
}

bool instructionIsInsideLabelRange(const std::vector<LabelRange>& labels, size_t instructionIndex) {
  for (const auto& range : labels) {
    if (instructionIndex >= range.labelInstruction && instructionIndex <= range.retInstruction) {
      return true;
    }
  }
  return false;
}

std::vector<LabelRange> discoverLabelRanges(const std::vector<D3DDecodedInstruction>& instructions) {
  std::vector<LabelRange> labels;
  for (size_t i = 0; i < instructions.size(); ++i) {
    const auto& instruction = instructions[i];
    if (instruction.opcode != kD3DSIO_LABEL) {
      continue;
    }
    if (instruction.operands.empty()) {
      throw std::runtime_error("LABEL requires a label operand");
    }
    const u32 label = decodeLabelIndex(instruction.operands[0]);
    if (findLabelRange(labels, label)) {
      throw std::runtime_error("duplicate D3D LABEL");
    }
    size_t ret = instructions.size();
    for (size_t j = i + 1; j < instructions.size(); ++j) {
      if (instructions[j].opcode == kD3DSIO_LABEL) {
        throw std::runtime_error("nested D3D LABEL is unsupported");
      }
      if (instructions[j].opcode == kD3DSIO_RET) {
        ret = j;
        break;
      }
    }
    if (ret == instructions.size()) {
      throw std::runtime_error("D3D LABEL body is missing RET");
    }
    labels.push_back(LabelRange{label, i, i + 1, ret});
    i = ret;
  }
  return labels;
}

void appendExpandedInstructionRange(std::vector<D3DDecodedInstruction>& out,
                                    const std::vector<D3DDecodedInstruction>& instructions,
                                    const std::vector<LabelRange>& labels,
                                    size_t begin,
                                    size_t end,
                                    std::vector<u32>& activeLabels) {
  for (size_t i = begin; i < end; ++i) {
    const auto& instruction = instructions[i];
    if (instruction.opcode == kD3DSIO_LABEL) {
      if (instruction.operands.empty()) {
        throw std::runtime_error("LABEL requires a label operand");
      }
      if (const auto labelIndex = findLabelRange(labels, decodeLabelIndex(instruction.operands[0]))) {
        i = labels[*labelIndex].retInstruction;
      }
      continue;
    }
    if (instruction.opcode == kD3DSIO_RET) {
      continue;
    }
    if (instruction.opcode == kD3DSIO_CALL || instruction.opcode == kD3DSIO_CALLNZ) {
      if (instruction.operands.empty()) {
        throw std::runtime_error("CALL requires a label operand");
      }
      const u32 label = decodeLabelIndex(instruction.operands[0]);
      const auto labelIndex = findLabelRange(labels, label);
      if (!labelIndex) {
        throw std::runtime_error("D3D CALL target label is missing");
      }
      if (std::find(activeLabels.begin(), activeLabels.end(), label) != activeLabels.end()) {
        throw std::runtime_error("recursive D3D CALL is unsupported");
      }
      if (instruction.opcode == kD3DSIO_CALLNZ) {
        if (instruction.operands.size() < 2) {
          throw std::runtime_error("CALLNZ requires a label operand and condition source");
        }
        D3DDecodedInstruction syntheticIf{};
        syntheticIf.opcode = kD3DSIO_IF;
        syntheticIf.predicated = instruction.predicated;
        syntheticIf.operands.push_back(instruction.operands[1]);
        syntheticIf.relAddrTokens.push_back(instruction.relAddrTokens.size() > 1 ? instruction.relAddrTokens[1] : 0u);
        out.push_back(std::move(syntheticIf));
      } else if (instruction.predicated) {
        D3DDecodedInstruction syntheticIf{};
        syntheticIf.opcode = kD3DSIO_IF;
        syntheticIf.operands.push_back((1u << 31) | (0u & 0x7ffu) | ((kD3DSPR_PREDICATE & 0x7u) << 28) |
                                       (((kD3DSPR_PREDICATE >> 3) & 0x3u) << 11));
        syntheticIf.relAddrTokens.push_back(0u);
        out.push_back(std::move(syntheticIf));
      }
      activeLabels.push_back(label);
      const auto& range = labels[*labelIndex];
      appendExpandedInstructionRange(out, instructions, labels, range.bodyBegin, range.retInstruction, activeLabels);
      activeLabels.pop_back();
      if (instruction.opcode == kD3DSIO_CALLNZ || instruction.predicated) {
        D3DDecodedInstruction syntheticEndIf{};
        syntheticEndIf.opcode = kD3DSIO_ENDIF;
        out.push_back(std::move(syntheticEndIf));
      }
      continue;
    }
    out.push_back(instruction);
  }
}

void inlineShaderSubroutines(SpirvModule& module) {
  const bool hasLabel = std::any_of(module.instructions.begin(), module.instructions.end(),
                                   [](const D3DDecodedInstruction& instruction) {
                                     return instruction.opcode == kD3DSIO_LABEL;
                                   });
  if (!hasLabel) {
    return;
  }
  const auto labels = discoverLabelRanges(module.instructions);
  std::vector<D3DDecodedInstruction> expanded;
  std::vector<u32> activeLabels;
  for (size_t i = 0; i < module.instructions.size(); ++i) {
    if (instructionIsInsideLabelRange(labels, i)) {
      continue;
    }
    appendExpandedInstructionRange(expanded, module.instructions, labels, i, i + 1, activeLabels);
  }
  module.instructions = std::move(expanded);
}

}  // namespace

bool isConstantRegisterKind(D3DRegisterKind kind) {
  return kind == D3DRegisterKind::ConstFloat ||
         kind == D3DRegisterKind::ConstInt ||
         kind == D3DRegisterKind::ConstBool;
}

bool isTextureSampleOpcode(u32 opcode) {
  return opcode == kD3DSIO_TEX ||
         opcode == kD3DSIO_TEXLDD ||
         opcode == kD3DSIO_TEXLDL;
}

bool isLegacyTextureSampleOpcode(u32 opcode) {
  switch (opcode) {
    case kD3DSIO_TEX:
    case kD3DSIO_TEXBEM:
    case kD3DSIO_TEXBEML:
    case kD3DSIO_TEXREG2AR:
    case kD3DSIO_TEXREG2GB:
    case kD3DSIO_TEXM3x2TEX:
    case kD3DSIO_TEXM3x3TEX:
    case kD3DSIO_TEXM3x3SPEC:
    case kD3DSIO_TEXM3x3VSPEC:
    case kD3DSIO_TEXREG2RGB:
    case kD3DSIO_TEXDP3TEX:
      return true;
    default:
      return false;
  }
}

std::optional<u32> legacyPixelOperandCount(u32 opcode, u32 major, u32 minor, D3DShaderStage stage) {
  if (stage != D3DShaderStage::Pixel || major != 1u) {
    return std::nullopt;
  }

  switch (opcode) {
    case kD3DSIO_TEXCOORD:
      return minor >= 4u ? 2u : 1u;
    case kD3DSIO_TEX:
      return minor >= 4u ? 2u : 1u;
    case kD3DSIO_TEXDEPTH:
      return 1u;
    case kD3DSIO_TEXBEM:
    case kD3DSIO_TEXBEML:
    case kD3DSIO_TEXREG2AR:
    case kD3DSIO_TEXREG2GB:
    case kD3DSIO_TEXM3x2PAD:
    case kD3DSIO_TEXM3x2TEX:
    case kD3DSIO_TEXM3x3PAD:
    case kD3DSIO_TEXM3x3TEX:
    case kD3DSIO_TEXM3x3DIFF:
    case kD3DSIO_TEXM3x3VSPEC:
    case kD3DSIO_TEXREG2RGB:
    case kD3DSIO_TEXDP3TEX:
    case kD3DSIO_TEXM3x2DEPTH:
    case kD3DSIO_TEXDP3:
    case kD3DSIO_TEXM3x3:
      return 2u;
    case kD3DSIO_TEXM3x3SPEC:
    case kD3DSIO_BEM:
      return 3u;
    default:
      return std::nullopt;
  }
}

u32 legacyTextureStageIndex(const D3DDecodedInstruction& instruction) {
  if (instruction.operands.empty()) {
    return 0u;
  }
  return std::min<u32>(decodeRegisterIndex(instruction.operands[0]), kMaxSamplers - 1u);
}

u32 matrixConstantRows(u32 opcode) {
  switch (opcode) {
    case kD3DSIO_M4x4:
    case kD3DSIO_M3x4:
      return 4;
    case kD3DSIO_M4x3:
    case kD3DSIO_M3x3:
      return 3;
    case kD3DSIO_M3x2:
      return 2;
    default:
      return 1;
  }
}

u32 textureSamplerIndex(const D3DDecodedInstruction& instruction, D3DShaderStage stage) {
  for (size_t i = 1; i < instruction.operands.size(); ++i) {
    const auto reg = decodeRegisterRef(instruction.operands[i], stage);
    if (reg.kind == D3DRegisterKind::Sampler) {
      return std::min<u32>(reg.index, kMaxSamplers - 1u);
    }
  }
  return 0u;
}

std::optional<VertexOutputMapping> vertexOutputMapping(const D3DRegisterRef& reg,
                                                       const VertexOutputSemantics* semantics) {
  if (!semantics || reg.kind != D3DRegisterKind::TexCoordOut || reg.index >= semantics->size()) {
    return std::nullopt;
  }
  const auto& semantic = (*semantics)[reg.index];
  if (!semantic.valid) {
    return std::nullopt;
  }

  switch (semantic.usage) {
    case kD3DDeclUsagePosition:
    case kD3DDeclUsagePositionT:
      return VertexOutputMapping{VertexOutputMapping::Target::Position, 0};
    case kD3DDeclUsageTexcoord:
      return VertexOutputMapping{VertexOutputMapping::Target::Texcoord, semantic.usageIndex};
    case kD3DDeclUsageColor:
      return VertexOutputMapping{semantic.usageIndex == 0u ? VertexOutputMapping::Target::Color
                                                           : VertexOutputMapping::Target::SecondaryColor,
                                 semantic.usageIndex};
    case kD3DDeclUsageFog:
      return VertexOutputMapping{VertexOutputMapping::Target::Fog, 0};
    case kD3DDeclUsagePSize:
      return VertexOutputMapping{VertexOutputMapping::Target::PointSize, 0};
    default:
      return VertexOutputMapping{VertexOutputMapping::Target::Texcoord, reg.index};
  }
}

std::optional<VertexShaderInputLayout> decodeVertexShaderInputLayout(const SpirvModule& module,
                                                                     const ShaderSourceContext& context) {
  if (module.stage != D3DShaderStage::Vertex) {
    return std::nullopt;
  }

  VertexShaderInputLayout layout;
  layout.stride = computeVertexDeclStride(context.vertexDecl);
  for (u32 stream = 0; stream < layout.streamStrides.size(); ++stream) {
    layout.streamStrides[stream] = computeVertexDeclStreamStride(context.vertexDecl, stream);
  }
  bool hasBinding = false;
  for (const auto& instruction : module.instructions) {
    if (instruction.opcode != kD3DSIO_DCL || instruction.operands.size() < 2) {
      continue;
    }
    const u32 semanticToken = instruction.operands[0];
    const auto dst = decodeRegisterRef(instruction.operands[1], module.stage);
    if (dst.kind != D3DRegisterKind::Input || dst.index >= layout.inputs.size()) {
      continue;
    }
    const u32 usage = (semanticToken & kD3DSP_DCL_USAGE_MASK) >> kD3DSP_DCL_USAGE_SHIFT;
    const u32 usageIndex = (semanticToken & kD3DSP_DCL_USAGEINDEX_MASK) >> kD3DSP_DCL_USAGEINDEX_SHIFT;
    for (const auto& element : context.vertexDecl.elements) {
      if (element.stream >= layout.streamStrides.size()) {
        continue;
      }
      if (element.usage == usage && element.usageIndex == usageIndex) {
        layout.inputs[dst.index] = VertexInputBinding{
            .valid = true,
            .stream = element.stream,
            .offset = element.offset,
            .type = element.type,
            .usage = usage,
            .usageIndex = usageIndex,
        };
        layout.streamMask |= 1u << element.stream;
        hasBinding = true;
        break;
      }
    }
  }
  if (!hasBinding) {
    return std::nullopt;
  }
  layout.hash = hashVertexShaderInputLayout(layout);
  return layout;
}

VertexOutputSemantics collectVertexOutputSemantics(const SpirvModule& module) {
  VertexOutputSemantics semantics{};
  if (module.stage != D3DShaderStage::Vertex || module.major < 3u) {
    return semantics;
  }

  for (const auto& instruction : module.instructions) {
    if (instruction.opcode != kD3DSIO_DCL || instruction.operands.size() < 2) {
      continue;
    }
    const auto dst = decodeRegisterRef(instruction.operands[1], module.stage);
    if (dst.kind != D3DRegisterKind::TexCoordOut || dst.index >= semantics.size()) {
      continue;
    }
    const u32 semanticToken = instruction.operands[0];
    semantics[dst.index] = VertexOutputSemantic{
        .valid = true,
        .usage = (semanticToken & kD3DSP_DCL_USAGE_MASK) >> kD3DSP_DCL_USAGE_SHIFT,
        .usageIndex = (semanticToken & kD3DSP_DCL_USAGEINDEX_MASK) >> kD3DSP_DCL_USAGEINDEX_SHIFT,
    };
  }
  return semantics;
}

PixelInputSemantics collectPixelInputSemantics(const SpirvModule& module) {
  PixelInputSemantics semantics{};
  if (module.stage != D3DShaderStage::Pixel || module.major < 3u) {
    return semantics;
  }
  for (const auto& instruction : module.instructions) {
    if (instruction.opcode != kD3DSIO_DCL || instruction.operands.size() < 2) {
      continue;
    }
    const auto dst = decodeRegisterRef(instruction.operands[1], module.stage);
    if (dst.kind != D3DRegisterKind::Input || dst.index >= semantics.size()) {
      continue;
    }
    const u32 semanticToken = instruction.operands[0];
    semantics[dst.index] = PixelInputSemantic{
        .valid = true,
        .usage = (semanticToken & kD3DSP_DCL_USAGE_MASK) >> kD3DSP_DCL_USAGE_SHIFT,
        .usageIndex = (semanticToken & kD3DSP_DCL_USAGEINDEX_MASK) >> kD3DSP_DCL_USAGEINDEX_SHIFT,
    };
  }
  return semantics;
}

u32 pixelColorOutputCount(const SpirvModule& module) {
  if (module.stage != D3DShaderStage::Pixel) {
    return 1u;
  }
  u32 count = 1u;
  for (const auto& instruction : module.instructions) {
    if (instruction.operands.empty() || !opcodeWritesFirstOperand(instruction.opcode)) {
      continue;
    }
    const auto dst = decodeRegisterRef(instruction.operands[0], module.stage);
    if (dst.kind == D3DRegisterKind::ColorOut && dst.index < kMaxRenderTargets) {
      count = std::max(count, dst.index + 1u);
    }
  }
  return count;
}

bool pixelWritesDepth(const SpirvModule& module) {
  if (module.stage != D3DShaderStage::Pixel) {
    return false;
  }
  for (const auto& instruction : module.instructions) {
    if (instruction.opcode == kD3DSIO_TEXDEPTH || instruction.opcode == kD3DSIO_TEXM3x2DEPTH) {
      return true;
    }
    if (instruction.operands.empty() || !opcodeWritesFirstOperand(instruction.opcode)) {
      continue;
    }
    const auto dst = decodeRegisterRef(instruction.operands[0], module.stage);
    if (dst.kind == D3DRegisterKind::DepthOut) {
      return true;
    }
  }
  return false;
}

bool pixelUsesTexcoordOut(const SpirvModule& module) {
  if (module.stage != D3DShaderStage::Pixel) {
    return false;
  }
  if (module.major == 1u) {
    return true;
  }
  for (const auto& instruction : module.instructions) {
    const bool writesDest = opcodeWritesFirstOperand(instruction.opcode);
    for (size_t i = 0; i < instruction.operands.size(); ++i) {
      // Skip DEF/DEFI/DEFB literal operands (only operand 0 is a register).
      if ((instruction.opcode == kD3DSIO_DEF ||
           instruction.opcode == kD3DSIO_DEFI ||
           instruction.opcode == kD3DSIO_DEFB) &&
          i > 0) {
        continue;
      }
      // LABEL/CALL operands index a label, not a register.
      if ((instruction.opcode == kD3DSIO_LABEL || instruction.opcode == kD3DSIO_CALL) && i == 0) {
        continue;
      }
      // Operand 0 is a destination only when the opcode actually writes it;
      // otherwise treat it as a source for kind checking.
      (void)writesDest;
      const auto reg = decodeRegisterRef(instruction.operands[i], module.stage);
      if (reg.kind == D3DRegisterKind::TexCoordOut) {
        return true;
      }
    }
  }
  return false;
}

std::array<bool, kMaxSamplers> collectPixelSamplerUsage(const SpirvModule& module,
                                                        const ShaderSourceContext& context) {
  std::array<bool, kMaxSamplers> usage{};
  if (module.stage != D3DShaderStage::Pixel) {
    return usage;
  }
  for (const auto& instruction : module.instructions) {
    if (module.major == 1u && isLegacyTextureSampleOpcode(instruction.opcode)) {
      usage[legacyTextureStageIndex(instruction)] = true;
    } else if (isTextureSampleOpcode(instruction.opcode)) {
      usage[textureSamplerIndex(instruction, module.stage)] = true;
    }
  }
  if (!std::any_of(usage.begin(), usage.end(), [](bool used) { return used; }) &&
      (module.usesTexture || context.textures[0])) {
    usage[0] = true;
  }
  return usage;
}

void noteConstantUsage(ConstantUsage& usage, D3DRegisterKind kind, u32 index) {
  switch (kind) {
    case D3DRegisterKind::ConstFloat:
      usage.hasFloat = true;
      usage.floatCount = std::max(usage.floatCount, index + 1u);
      break;
    case D3DRegisterKind::ConstInt:
      usage.hasInt = true;
      usage.intCount = std::max(usage.intCount, index + 1u);
      break;
    case D3DRegisterKind::ConstBool:
      usage.hasBool = true;
      usage.boolCount = std::max(usage.boolCount, index + 1u);
      break;
    default:
      break;
  }
}

ConstantUsage collectConstantUsage(const SpirvModule& module) {
  ConstantUsage usage;
  for (const auto& instruction : module.instructions) {
    if (instruction.operands.empty()) {
      continue;
    }

    if (opcodeWritesFirstOperand(instruction.opcode)) {
      const auto dst = decodeRegisterRef(instruction.operands[0], module.stage);
      if (isConstantRegisterKind(dst.kind)) {
        usage.mutableConstants = true;
        noteConstantUsage(usage, dst.kind, dst.index);
      }
      // Track max Temp / Output-Color index so the emitter sizes
      // `r[]` / `outColor[]` to actual usage rather than the spec
      // maxima (32 / 4). Apple's MSL → AIR keeps oversized alloca
      // because of dynamic-index patterns; under-sizing the array
      // gives the register allocator room to promote remaining slots
      // to GPU registers (measured impact in the smoke harness).
      // Indexed dest writes (`mov r[a0+N], ...`) escape to the full
      // 31-index ceiling — the relAddr token can pick any slot at
      // runtime and trimming the array would corrupt downstream reads.
      const bool indexedDst =
          !instruction.relAddrTokens.empty() && instruction.relAddrTokens[0] != 0u;
      if (dst.kind == D3DRegisterKind::Temp) {
        if (indexedDst) {
          if (usage.maxTempIndex < 31) usage.maxTempIndex = 31;
        } else {
          const auto idx = static_cast<std::int32_t>(dst.index);
          if (idx > usage.maxTempIndex) usage.maxTempIndex = idx;
        }
      } else if (dst.kind == D3DRegisterKind::ColorOut) {
        const auto idx = static_cast<std::int32_t>(dst.index);
        if (idx > usage.maxColorIndex) usage.maxColorIndex = idx;
      }
    }

    size_t sourceBegin = 1;
    switch (instruction.opcode) {
      case kD3DSIO_IF:
      case kD3DSIO_IFC:
      case kD3DSIO_LOOP:
      case kD3DSIO_REP:
      case kD3DSIO_BREAKP:
      case kD3DSIO_TEXDEPTH:
        sourceBegin = 0;
        break;
      case kD3DSIO_DEF:
      case kD3DSIO_DEFI:
      case kD3DSIO_DEFB:
      case kD3DSIO_DCL:
      case kD3DSIO_LABEL:
      case kD3DSIO_CALL:
        sourceBegin = instruction.operands.size();
        break;
      default:
        break;
    }

    for (size_t i = sourceBegin; i < instruction.operands.size(); ++i) {
      const auto src = decodeRegisterRef(instruction.operands[i], module.stage);
      noteConstantUsage(usage, src.kind, src.index);
      const bool indexedSrc =
          i < instruction.relAddrTokens.size() && instruction.relAddrTokens[i] != 0u;
      if (src.kind == D3DRegisterKind::Temp) {
        // Source-side `r-N` reads also bound the trimmed array size.
        // Indexed access (`r[a0+N]`) can hit any slot at runtime — fall
        // back to the full 31-index maximum so the emitter keeps the
        // spec-max array.
        if (indexedSrc) {
          if (usage.maxTempIndex < 31) usage.maxTempIndex = 31;
        } else {
          const auto idx = static_cast<std::int32_t>(src.index);
          if (idx > usage.maxTempIndex) usage.maxTempIndex = idx;
        }
      }
      const bool indexed =
          i < instruction.relAddrTokens.size() && instruction.relAddrTokens[i] != 0u;
      if (indexed) {
        switch (src.kind) {
          case D3DRegisterKind::ConstFloat:
            usage.hasIndexedFloat = true;
            break;
          case D3DRegisterKind::ConstInt:
            usage.hasIndexedInt = true;
            break;
          case D3DRegisterKind::ConstBool:
            usage.hasIndexedBool = true;
            break;
          default:
            break;
        }
      }
    }

    const u32 rows = matrixConstantRows(instruction.opcode);
    if (rows > 1 && instruction.operands.size() > 2) {
      const auto base = decodeRegisterRef(instruction.operands[2], module.stage);
      if (base.kind == D3DRegisterKind::ConstFloat) {
        noteConstantUsage(usage, base.kind, base.index + rows - 1u);
      }
    }
  }
  return usage;
}

bool shaderUsesPredicateRegisters(const SpirvModule& module) {
  for (const auto& instruction : module.instructions) {
    if (instruction.predicated) {
      return true;
    }
    if (instruction.operands.empty()) {
      continue;
    }

    size_t sourceBegin = 1;
    if (instruction.opcode == kD3DSIO_SETP) {
      if (decodeRegisterRef(instruction.operands[0], module.stage).kind == D3DRegisterKind::Predicate) {
        return true;
      }
    }
    switch (instruction.opcode) {
      case kD3DSIO_IF:
      case kD3DSIO_IFC:
      case kD3DSIO_LOOP:
      case kD3DSIO_REP:
      case kD3DSIO_BREAKP:
        sourceBegin = 0;
        break;
      case kD3DSIO_DEF:
      case kD3DSIO_DEFI:
      case kD3DSIO_DEFB:
      case kD3DSIO_DCL:
      case kD3DSIO_LABEL:
      case kD3DSIO_CALL:
        sourceBegin = instruction.operands.size();
        break;
      default:
        break;
    }
    for (size_t i = sourceBegin; i < instruction.operands.size(); ++i) {
      if (decodeRegisterRef(instruction.operands[i], module.stage).kind == D3DRegisterKind::Predicate) {
        return true;
      }
    }
  }
  return false;
}

SpirvModule translateD3DBytecodeToSpirv(const ShaderRef& shader,
                                        bool vertex,
                                        const ShaderSourceContext& context) {
  SpirvModule module;
  const auto& bytes = shader.bytecode.bytes;
  if (bytes.size() % sizeof(u32) != 0) {
    throw std::runtime_error("D3D bytecode size is not DWORD aligned");
  }

  const u64 bytecodeHash = shader.bytecode.hash ? shader.bytecode.hash : hashBytes(std::as_bytes(std::span(bytes)));
  module.hash = bytecodeHash ^ (vertex ? 0x5356505653455254ull : 0x5350465348454453ull) ^
                context.clipPlaneMask ^ (static_cast<u64>(context.sampleCount) << 32);
  module.words.reserve(bytes.size() / sizeof(u32));
  module.stage = vertex ? D3DShaderStage::Vertex : D3DShaderStage::Pixel;

  auto readWord = [&](size_t offset) {
    u32 word = 0;
    std::memcpy(&word, bytes.data() + offset, sizeof(u32));
    return word;
  };

  if (bytes.empty()) {
    throw std::runtime_error("empty D3D bytecode");
  }

  const u32 versionToken = readWord(0);
  module.words.push_back(versionToken);
  const u32 shaderType = versionToken >> 16;
  if (shaderType == 0xfffeu) {
    module.stage = D3DShaderStage::Vertex;
  } else if (shaderType == 0xffffu) {
    module.stage = D3DShaderStage::Pixel;
  } else {
    throw std::runtime_error("invalid D3D shader version token");
  }
  module.major = (versionToken >> 8) & 0xffu;
  module.minor = versionToken & 0xffu;
  if ((module.stage == D3DShaderStage::Vertex && !(module.major == 1u && module.minor == 1u) &&
       module.major < 2u) ||
      (module.stage == D3DShaderStage::Pixel && module.major < 1u) ||
      module.major > 3u) {
    throw std::runtime_error("only vs_1_1/SM 2.x/3.x vertex and ps_1_x/SM 2.x/3.x pixel bytecode is supported");
  }

  size_t offset = sizeof(u32);
  while (offset < bytes.size()) {
    const u32 token = readWord(offset);
    module.words.push_back(token);
    offset += sizeof(u32);

    const u32 opcode = token & 0xffffu;
    if (opcode == kD3DSIO_END) {
      break;
    }
    if (opcode == kD3DSIO_COMMENT) {
      const u32 commentWords = (token >> 16) & 0x7fffu;
      const size_t commentBytes = static_cast<size_t>(commentWords) * sizeof(u32);
      if (offset + commentBytes > bytes.size()) {
        throw std::runtime_error("truncated D3D comment token");
      }
      offset += commentBytes;
      continue;
    }
    if (opcode == kD3DSIO_PHASE) {
      continue;
    }

    u32 operandCount = 0;
    if (const auto legacyCount = legacyPixelOperandCount(opcode, module.major, module.minor, module.stage)) {
      operandCount = *legacyCount;
    } else try {
      operandCount = fixedOperandCount(opcode);
    } catch (const std::runtime_error&) {
      operandCount = (token >> 24) & 0xfu;
      if (operandCount == 0) {
        switch (opcode) {
          case kD3DSIO_NOP:
          case kD3DSIO_ELSE:
          case kD3DSIO_ENDIF:
          case kD3DSIO_ENDLOOP:
          case kD3DSIO_ENDREP:
          case kD3DSIO_RET:
          case kD3DSIO_BREAK:
          case kD3DSIO_PHASE:
          case kD3DSIO_COMMENT:
          case kD3DSIO_END:
            break;
          default: {
            std::ostringstream message;
            message << "missing D3D operand count"
                    << " opcode=" << opcodeName(opcode)
                    << " token=0x" << std::hex << token
                    << " offset=0x" << offset - sizeof(u32);
            if (!module.instructions.empty()) {
              const auto& previous = module.instructions.back();
              message << " prevOpcode=" << opcodeName(previous.opcode)
                      << " prevTokenCount=" << std::dec << previous.operands.size()
                      << " prevOperands=[";
              for (size_t i = 0; i < previous.operands.size(); ++i) {
                if (i != 0) {
                  message << ",";
                }
                message << "0x" << std::hex << previous.operands[i];
              }
              message << "]";
            }
            const size_t rawCount = module.words.size();
            const size_t rawStart = rawCount > 8 ? rawCount - 8 : 0;
            message << " rawWords=[";
            for (size_t i = rawStart; i < rawCount; ++i) {
              if (i != rawStart) {
                message << ",";
              }
              message << "0x" << std::hex << module.words[i];
            }
            message << "]";
            throw std::runtime_error(message.str());
          }
        }
      }
    }
    D3DDecodedInstruction instruction;
    instruction.opcode = opcode;
    instruction.controls = (token >> 16) & 0xffu;
    instruction.predicated = ((token >> 28) & 0x1u) != 0;
    instruction.operands.reserve(operandCount);
    instruction.relAddrTokens.assign(operandCount, 0u);
    // D3D9 operand encoding: when an *operand* token has the rel-addr
    // bit (bit 13) set, an additional DWORD immediately follows that
    // encodes the address-register source. The DWORD is NOT counted in
    // the operandCount field of the instruction header. Read N
    // operands and consume one extra DWORD per operand that carries a
    // rel-addr bit.
    //
    // Caveat: a handful of opcodes carry *literal* operands (immediate
    // floats / ints) instead of register tokens. The bit-13 rel-addr
    // probe is only meaningful for register operands; on a literal
    // value it would mistakenly consume the next word and shift every
    // subsequent instruction. The inhibit list below mirrors the D3D9
    // spec's per-opcode operand layout.
    auto operandIsRegister = [](u32 op, u32 i) {
      switch (op) {
        case kD3DSIO_DEF:
        case kD3DSIO_DEFI:
          return i == 0;  // [dst, f32 x4] / [dst, i32 x4]
        case kD3DSIO_DEFB:
          return i == 0;  // [dst, bool]
        case kD3DSIO_LABEL:
        case kD3DSIO_CALL:
          return false;   // label index only
        case kD3DSIO_CALLNZ:
          return i != 0;  // label index, then predicate src
        default:
          return true;
      }
    };
    for (u32 i = 0; i < operandCount; ++i) {
      if (offset + sizeof(u32) > bytes.size()) {
        throw std::runtime_error("truncated D3D instruction operand");
      }
      const u32 operandToken = readWord(offset);
      offset += sizeof(u32);
      module.words.push_back(operandToken);
      instruction.operands.push_back(operandToken);
      if (operandIsRegister(opcode, i) && tokenHasRelativeAddressing(operandToken)) {
        if (offset + sizeof(u32) > bytes.size()) {
          throw std::runtime_error("truncated D3D rel-addr operand");
        }
        const u32 relAddrToken = readWord(offset);
        offset += sizeof(u32);
        module.words.push_back(relAddrToken);
        instruction.relAddrTokens[i] = relAddrToken;
      }
    }
    if (opcode == kD3DSIO_TEX || opcode == kD3DSIO_TEXLDD || opcode == kD3DSIO_TEXLDL ||
        opcode == kD3DSIO_TEXBEM || opcode == kD3DSIO_TEXBEML || opcode == kD3DSIO_TEXREG2AR ||
        opcode == kD3DSIO_TEXREG2GB || opcode == kD3DSIO_TEXM3x2TEX || opcode == kD3DSIO_TEXM3x3TEX ||
        opcode == kD3DSIO_TEXM3x3SPEC || opcode == kD3DSIO_TEXM3x3VSPEC || opcode == kD3DSIO_TEXREG2RGB ||
        opcode == kD3DSIO_TEXDP3TEX) {
      module.usesTexture = true;
    }
    if (opcode == kD3DSIO_DCL && instruction.operands.size() >= 2) {
      const auto dst = decodeRegisterRef(instruction.operands[1], module.stage);
      if (dst.kind == D3DRegisterKind::Sampler && dst.index < module.samplerTextureTypes.size()) {
        module.samplerTextureTypes[dst.index] = decodeDclTextureType(instruction.operands[0]);
      }
    }
    module.instructions.push_back(std::move(instruction));
  }

  inlineShaderSubroutines(module);
  return module;
}

}  // namespace dxmt9::translator::detail_
