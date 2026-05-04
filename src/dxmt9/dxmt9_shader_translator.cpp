#include "dxmt9_shader_translator.hpp"

#include "dxmt9_d3d9_bytecode.hpp"
#include "dxmt9_debug_trace.hpp"
#include "dxmt9_ffp_shaders.hpp"
#include "dxmt9_shader_sources.hpp"
#include "dxmt9/assert.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// Pull the sibling namespaces into an intermediate namespace so the
// translator's own inner scope doesn't shadow `dxmt9::`. All translator
// code lives in `dxmt9::translator::detail_`; the two public functions
// are re-exported via thin wrappers at the end.
namespace dxmt9::translator::detail_ {

using namespace ::dxmt9::core;
using namespace ::dxmt9::d3d9bc;
using namespace ::dxmt9::ffp;
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using f32 = float;
using i32 = std::int32_t;

using ::dxmt9::shaders::makeShaderPrelude;

constexpr u32 kD3DDeclUsagePosition = 0u;
constexpr u32 kD3DDeclUsagePSize = 4u;
constexpr u32 kD3DDeclUsageTexcoord = 5u;
constexpr u32 kD3DDeclUsagePositionT = 9u;
constexpr u32 kD3DDeclUsageColor = 10u;
constexpr u32 kD3DDeclUsageFog = 11u;

std::string formatFloatLiteral(f32 value) {
  std::ostringstream out;
  out << std::setprecision(9) << value;
  std::string text = out.str();
  if (text.find_first_of(".eE") == std::string::npos) {
    text += ".0";
  }
  if (text == "-0" || text == "-0.0") {
    text = "0.0";
  }
  text += "f";
  return text;
}

std::string formatFloatVec4(const std::array<f32, 4>& values) {
  std::ostringstream out;
  out << "float4(";
  for (size_t i = 0; i < values.size(); ++i) {
    if (i != 0) {
      out << ", ";
    }
    out << formatFloatLiteral(values[i]);
  }
  out << ")";
  return out.str();
}

std::string formatIntVec4(const std::array<i32, 4>& values) {
  std::ostringstream out;
  out << "int4(";
  for (size_t i = 0; i < values.size(); ++i) {
    if (i != 0) {
      out << ", ";
    }
    out << values[i];
  }
  out << ")";
  return out.str();
}

std::string componentName(u32 component) {
  switch (component & 3u) {
    case 0:
      return "x";
    case 1:
      return "y";
    case 2:
      return "z";
    default:
      return "w";
  }
}

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

std::string registerName(const D3DRegisterRef& reg, D3DShaderStage stage, bool dest = false) {
  std::ostringstream out;
  switch (reg.kind) {
    case D3DRegisterKind::Temp:
      out << "r" << reg.index;
      break;
    case D3DRegisterKind::Input:
      out << "v" << reg.index;
      break;
    case D3DRegisterKind::ConstFloat:
      out << "c" << reg.index;
      break;
    case D3DRegisterKind::Address:
      out << "a" << reg.index;
      break;
    case D3DRegisterKind::RastOut:
      if (stage == D3DShaderStage::Vertex) {
        if (reg.index == 0) {
          out << "oPos";
        } else if (reg.index == 1) {
          out << "oFog";
        } else if (reg.index == 2) {
          out << "oPts";
        } else {
          out << "oR" << reg.index;
        }
      } else {
        out << "rout" << reg.index;
      }
      break;
    case D3DRegisterKind::AttrOut:
      out << "oD" << reg.index;
      break;
    case D3DRegisterKind::TexCoordOut:
      if (stage == D3DShaderStage::Vertex) {
        out << "oT" << reg.index;
      } else {
        out << "vTex" << reg.index;
      }
      break;
    case D3DRegisterKind::ConstInt:
      out << "i" << reg.index;
      break;
    case D3DRegisterKind::ColorOut:
      out << "oC" << reg.index;
      break;
    case D3DRegisterKind::DepthOut:
      out << "oDepth";
      break;
    case D3DRegisterKind::Sampler:
      out << "s" << reg.index;
      break;
    case D3DRegisterKind::ConstBool:
      out << "b" << reg.index;
      break;
    case D3DRegisterKind::Loop:
      out << "aL";
      break;
    case D3DRegisterKind::MiscType:
      if (stage == D3DShaderStage::Pixel && reg.index == 0) {
        out << "vPos";
      } else if (stage == D3DShaderStage::Pixel && reg.index == 1) {
        out << "vFace";
      } else {
        out << "misc" << reg.index;
      }
      break;
    case D3DRegisterKind::Predicate:
      out << "p" << reg.index;
      break;
    case D3DRegisterKind::Unknown:
      out << (dest ? "dst" : "src") << reg.index;
      break;
  }
  return out.str();
}

std::string applySourceModifier(std::string expr, u32 modifier) {
  switch (modifier) {
    case 0:
      return expr;
    case 1:
      return "-(" + expr + ")";
    case 2:
      return "(" + expr + " - float4(0.5f))";
    case 3:
      return "-((" + expr + ") - float4(0.5f))";
    case 4:
      return "(" + expr + " * float4(2.0f) - float4(1.0f))";
    case 11:
      return "abs(" + expr + ")";
    case 12:
      return "-abs(" + expr + ")";
    default:
      throw std::runtime_error("unsupported D3D source modifier " + std::to_string(modifier));
  }
}

std::string applySwizzle(const std::string& expr, const std::array<u8, 4>& swizzle) {
  if (swizzle[0] == 0 && swizzle[1] == 1 && swizzle[2] == 2 && swizzle[3] == 3) {
    return expr;
  }
  std::ostringstream out;
  out << "float4(" << expr << "." << componentName(swizzle[0]) << ", " << expr << "." << componentName(swizzle[1])
      << ", " << expr << "." << componentName(swizzle[2]) << ", " << expr << "." << componentName(swizzle[3])
      << ")";
  return out.str();
}

struct VertexOutputSemantic {
  bool valid = false;
  u32 usage = 0;
  u32 usageIndex = 0;
};

using VertexOutputSemantics = std::array<VertexOutputSemantic, 16>;

struct VertexOutputMapping {
  enum class Target {
    Position,
    Texcoord,
    Color,
    SecondaryColor,
    Fog,
    PointSize,
  };

  Target target = Target::Texcoord;
  u32 index = 0;
};

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

std::string readVertexOutputMapping(const VertexOutputMapping& mapping,
                                    const std::string& outPosition,
                                    const std::string& outColor,
                                    const std::string& outSecondaryColor,
                                    const std::string& outTexcoord,
                                    const std::string& outFogFactor,
                                    const std::string& outPointSize) {
  switch (mapping.target) {
    case VertexOutputMapping::Target::Position:
      return outPosition;
    case VertexOutputMapping::Target::Texcoord:
      return outTexcoord + "[" + std::to_string(std::min<u32>(mapping.index, kMaxTextureStages - 1u)) + "]";
    case VertexOutputMapping::Target::Color:
      return outColor;
    case VertexOutputMapping::Target::SecondaryColor:
      return outSecondaryColor;
    case VertexOutputMapping::Target::Fog:
      return "float4(" + outFogFactor + ")";
    case VertexOutputMapping::Target::PointSize:
      return "float4(" + outPointSize + ")";
  }
  return "float4(0.0f)";
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
    case kD3DSIO_ELSE:
      return "else";
    case kD3DSIO_ENDIF:
      return "endif";
    case kD3DSIO_BREAK:
      return "break";
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
    case kD3DSIO_TEX:
      return "tex";
    case kD3DSIO_TEXDEPTH:
      return "texdepth";
    case kD3DSIO_CMP:
      return "cmp";
    case kD3DSIO_BEM:
      return "bem";
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
    case kD3DSIO_LOOP:
    case kD3DSIO_REP:
      return 1;
    case kD3DSIO_ADD:
    case kD3DSIO_SUB:
    case kD3DSIO_MUL:
    case kD3DSIO_DP3:
    case kD3DSIO_DP4:
    case kD3DSIO_MIN:
    case kD3DSIO_MAX:
    case kD3DSIO_POW:
    case kD3DSIO_CRS:
    case kD3DSIO_TEXLDD:
    case kD3DSIO_TEXLDL:
    case kD3DSIO_SLT:
    case kD3DSIO_SGE:
    case kD3DSIO_M4x4:
    case kD3DSIO_M4x3:
    case kD3DSIO_M3x4:
    case kD3DSIO_M3x3:
    case kD3DSIO_M3x2:
      return 3;
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

std::string readOperandExpression(const D3DDecodedInstruction& instruction, const D3DRegisterRef& reg,
                                  const std::string& vertexInputs, const std::string& pixelInputs,
                                  bool vertexStage, const std::string& outPosition, const std::string& outColor,
                                  const std::string& outSecondaryColor, const std::string& outTexcoord,
                                  const std::string& outFogFactor, const std::string& outPointSize,
                                  const std::string& tempPrefix, const std::string& constPrefix,
                                  const std::string& intPrefix, const std::string& boolPrefix,
                                  const std::string& predicatePrefix,
                                  const VertexOutputSemantics* vertexOutputSemantics = nullptr) {
  (void)instruction;
  switch (reg.kind) {
    case D3DRegisterKind::Temp:
      return tempPrefix + "[" + std::to_string(reg.index) + "]";
    case D3DRegisterKind::ConstFloat:
      return constPrefix + "[" + std::to_string(reg.index) + "]";
    case D3DRegisterKind::ConstInt:
      return "float4(" + intPrefix + "[" + std::to_string(reg.index) + "])";
    case D3DRegisterKind::ConstBool:
      return "(" + boolPrefix + "[" + std::to_string(reg.index) + "] != 0u ? float4(1.0f) : float4(0.0f))";
    case D3DRegisterKind::Input:
      if (vertexStage) {
        return vertexInputs + "[" + std::to_string(reg.index) + "]";
      }
      if (reg.index == 0) {
        return "float4(" + pixelInputs + ".color)";
      }
      if (reg.index == 1) {
        return "float4(" + pixelInputs + ".texcoord0, 0.0f, 1.0f)";
      }
      if (reg.index == 2) {
        return "float4(" + pixelInputs + ".secondaryColor)";
      }
      if (reg.index == 3) {
        return "float4(" + pixelInputs + ".fogFactor)";
      }
      return "float4(0.0f)";
    case D3DRegisterKind::RastOut:
      if (!vertexStage) {
        return "float4(0.0f)";
      }
      if (reg.index == 0) {
        return outPosition;
      }
      if (reg.index == 1) {
        return "float4(" + outFogFactor + ")";
      }
      if (reg.index == 2) {
        return "float4(" + outPointSize + ")";
      }
      return "float4(0.0f)";
    case D3DRegisterKind::AttrOut:
      if (vertexStage) {
        return reg.index == 0 ? outColor : outSecondaryColor;
      }
      return "float4(0.0f)";
    case D3DRegisterKind::TexCoordOut:
      if (vertexStage) {
        if (auto mapped = vertexOutputMapping(reg, vertexOutputSemantics)) {
          return readVertexOutputMapping(*mapped, outPosition, outColor, outSecondaryColor,
                                         outTexcoord, outFogFactor, outPointSize);
        }
      }
      return outTexcoord + "[" + std::to_string(std::min<u32>(reg.index, kMaxTextureStages - 1u)) + "]";
    case D3DRegisterKind::ColorOut:
      if (!vertexStage) {
        return outColor + "[" + std::to_string(std::min<u32>(reg.index, kMaxRenderTargets - 1u)) + "]";
      }
      return outColor;
    case D3DRegisterKind::DepthOut:
      return "float4(0.0f)";
    case D3DRegisterKind::Address:
    case D3DRegisterKind::Loop:
      if (reg.kind == D3DRegisterKind::Address) {
        return "float4(a0)";
      }
      return "float4(aL)";
    case D3DRegisterKind::MiscType:
    case D3DRegisterKind::Sampler:
    case D3DRegisterKind::Unknown:
      return "float4(0.0f)";
    case D3DRegisterKind::Predicate:
      return "(" + predicatePrefix + "[" + std::to_string(reg.index) + "] ? float4(1.0f) : float4(0.0f))";
  }
  return "float4(0.0f)";
}

std::string decodeOperandToken(const u32 token, D3DShaderStage stage, bool destination) {
  if (destination && tokenHasRelativeAddressing(token)) {
    throw std::runtime_error("relative addressing is not supported yet");
  }
  D3DRegisterRef reg = decodeRegisterRef(token, stage);
  return registerName(reg, stage, destination);
}

u32 decodeLabelIndex(u32 token) {
  return token & 0x7ffu;
}

std::optional<VertexShaderInputLayout> decodeVertexShaderInputLayout(const SpirvModule& module, const DrawDesc& desc) {
  if (module.stage != D3DShaderStage::Vertex) {
    return std::nullopt;
  }

  VertexShaderInputLayout layout;
  layout.stride = computeVertexDeclStride(desc);
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
    for (const auto& element : desc.vertexDecl.elements) {
      if (element.stream != 0) {
        continue;
      }
      if (element.usage == usage && element.usageIndex == usageIndex) {
        layout.inputs[dst.index] = VertexInputBinding{
            .valid = true,
            .offset = element.offset,
            .type = element.type,
            .usage = usage,
            .usageIndex = usageIndex,
        };
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

bool isConstantRegisterKind(D3DRegisterKind kind) {
  return kind == D3DRegisterKind::ConstFloat ||
         kind == D3DRegisterKind::ConstInt ||
         kind == D3DRegisterKind::ConstBool;
}

struct ConstantUsage {
  bool mutableConstants = false;
  bool hasFloat = false;
  bool hasInt = false;
  bool hasBool = false;
  u32 floatCount = 0;
  u32 intCount = 0;
  u32 boolCount = 0;
};

struct PixelInputSemantic {
  bool valid = false;
  u32 usage = 0;
  u32 usageIndex = 0;
};

using PixelInputSemantics = std::array<PixelInputSemantic, 16>;

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

PixelInputSemantics collectPixelInputSemantics(const SpirvModule& module) {
  PixelInputSemantics semantics{};
  if (module.stage != D3DShaderStage::Pixel) {
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

bool isTextureSampleOpcode(u32 opcode) {
  return opcode == kD3DSIO_TEX ||
         opcode == kD3DSIO_TEXLDD ||
         opcode == kD3DSIO_TEXLDL;
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

std::array<bool, kMaxSamplers> collectPixelSamplerUsage(const SpirvModule& module, const DrawDesc& desc) {
  std::array<bool, kMaxSamplers> usage{};
  if (module.stage != D3DShaderStage::Pixel) {
    return usage;
  }
  for (const auto& instruction : module.instructions) {
    if (isTextureSampleOpcode(instruction.opcode)) {
      usage[textureSamplerIndex(instruction, module.stage)] = true;
    }
  }
  if (!std::any_of(usage.begin(), usage.end(), [](bool used) { return used; }) &&
      (module.usesTexture || desc.textures[0].handle != Handle{})) {
    usage[0] = true;
  }
  return usage;
}

void emitFragmentTextureArguments(std::ostringstream& out,
                                  const std::array<bool, kMaxSamplers>& samplerUsage) {
  bool first = true;
  for (u32 stage = 0; stage < kMaxSamplers; ++stage) {
    if (!samplerUsage[stage]) {
      continue;
    }
    if (!first) {
      out << ", ";
    }
    first = false;
    out << "texture2d<float> tex" << stage << " [[texture(" << stage << ")]], "
        << "sampler samp" << stage << " [[sampler(" << stage << ")]]";
  }
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
    }

    size_t sourceBegin = 1;
    switch (instruction.opcode) {
      case kD3DSIO_IF:
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
      const auto src = decodeRegisterRef(instruction.operands[i], module.stage);
      noteConstantUsage(usage, src.kind, src.index);
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

void emitConstantBindings(std::ostringstream& out, bool vertexStage, const ConstantUsage& usage) {
  const char* floatMember = vertexStage ? "vsFloatConst" : "psFloatConst";
  const char* intMember = vertexStage ? "vsIntConst" : "psIntConst";
  const char* boolMember = vertexStage ? "vsBoolConst" : "psBoolConst";

  if (!usage.mutableConstants) {
    out << "  constant float4* cFloat = uniforms." << floatMember << ";\n";
    out << "  constant int4* cInt = uniforms." << intMember << ";\n";
    out << "  constant uint* cBool = uniforms." << boolMember << ";\n";
    return;
  }

  out << "  float4 cFloat[" << std::max(1u, usage.floatCount) << "];\n";
  out << "  int4 cInt[" << std::max(1u, usage.intCount) << "];\n";
  out << "  uint cBool[" << std::max(1u, usage.boolCount) << "];\n";
  out << "  for (uint i = 0; i < " << usage.floatCount << "; ++i) { cFloat[i] = uniforms." << floatMember << "[i]; }\n";
  out << "  for (uint i = 0; i < " << usage.intCount << "; ++i) { cInt[i] = uniforms." << intMember << "[i]; }\n";
  out << "  for (uint i = 0; i < " << usage.boolCount << "; ++i) { cBool[i] = uniforms." << boolMember << "[i]; }\n";
}

void emitPredicateBindings(std::ostringstream& out, bool usesPredicateRegisters) {
  if (!usesPredicateRegisters) {
    return;
  }
  out << "  bool p[" << kMaxBoolConstants << "];\n";
  out << "  for (uint i = 0; i < " << kMaxBoolConstants << "; ++i) { p[i] = false; }\n";
}

std::string readPixelInputSemanticExpression(const PixelInputSemantic& semantic,
                                             const std::string& pixelInputs) {
  switch (semantic.usage) {
    case kD3DDeclUsagePosition:
    case kD3DDeclUsagePositionT:
      return pixelInputs + ".position";
    case kD3DDeclUsagePSize:
      return "float4(" + pixelInputs + ".pointSize)";
    case kD3DDeclUsageTexcoord:
      return "dxmt9_select_texcoord(" + pixelInputs + ", " + std::to_string(semantic.usageIndex) + "u)";
    case kD3DDeclUsageColor:
      if (semantic.usageIndex == 0) {
        return "float4(" + pixelInputs + ".color)";
      }
      if (semantic.usageIndex == 1) {
        return "float4(" + pixelInputs + ".secondaryColor)";
      }
      break;
    case kD3DDeclUsageFog:
      return "float4(" + pixelInputs + ".fogFactor)";
    default:
      break;
  }
  return "float4(0.0f)";
}

std::string readPixelInputFallbackExpression(u32 index, const std::string& pixelInputs) {
  if (index == 0) {
    return "float4(" + pixelInputs + ".color)";
  }
  if (index == 1) {
    return "dxmt9_select_texcoord(" + pixelInputs + ", 0u)";
  }
  if (index == 2) {
    return "float4(" + pixelInputs + ".secondaryColor)";
  }
  if (index == 3) {
    return "float4(" + pixelInputs + ".fogFactor)";
  }
  return "float4(0.0f)";
}

std::string readPixelInputExpression(u32 token,
                                     const std::string& pixelInputs,
                                     const PixelInputSemantics& semantics) {
  const u32 type = decodeRegisterType(token);
  const u32 index = decodeRegisterIndex(token);
  switch (type) {
    case kD3DSPR_INPUT:
      if (index < semantics.size() && semantics[index].valid) {
        return readPixelInputSemanticExpression(semantics[index], pixelInputs);
      }
      break;
    case kD3DSPR_ADDR:
      return "dxmt9_select_texcoord(" + pixelInputs + ", " + std::to_string(index) + "u)";
    case kD3DSPR_RASTOUT:
      if (index == 0) {
        return pixelInputs + ".position";
      }
      if (index == 1) {
        return "float4(" + pixelInputs + ".fogFactor)";
      }
      if (index == 2) {
        return "float4(" + pixelInputs + ".pointSize)";
      }
      break;
    default:
      break;
  }

  return readPixelInputFallbackExpression(index, pixelInputs);
}

struct FlowBlock {
  u32 opcode = 0;
  bool sawElse = false;
};

SpirvModule translateD3DBytecodeToSpirv(const ShaderRef& shader, bool vertex, const DrawDesc& desc) {
  SpirvModule module;
  const auto& bytes = shader.bytecode.bytes;
  if (bytes.size() % sizeof(u32) != 0) {
    throw std::runtime_error("D3D bytecode size is not DWORD aligned");
  }

  const u64 bytecodeHash = shader.bytecode.hash ? shader.bytecode.hash : hashBytes(std::as_bytes(std::span(bytes)));
  module.hash = bytecodeHash ^ (vertex ? 0x5356505653455254ull : 0x5350465348454453ull) ^ desc.clipPlaneMask ^
                (static_cast<u64>(desc.rts.color[0].sampleCount) << 32);
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
  if (module.major < 2 || module.major > 3) {
    throw std::runtime_error("only SM 2.x and 3.x bytecode is supported");
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
    try {
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
    const size_t operandBytes = static_cast<size_t>(operandCount) * sizeof(u32);
    if (offset + operandBytes > bytes.size()) {
      throw std::runtime_error("truncated D3D instruction");
    }

    D3DDecodedInstruction instruction;
    instruction.opcode = opcode;
    instruction.controls = (token >> 16) & 0xffu;
    instruction.predicated = ((token >> 28) & 0x1u) != 0;
    instruction.operands.reserve(operandCount);
    for (u32 i = 0; i < operandCount; ++i) {
      instruction.operands.push_back(readWord(offset + static_cast<size_t>(i) * sizeof(u32)));
      module.words.push_back(instruction.operands.back());
    }
    if (opcode == kD3DSIO_TEX || opcode == kD3DSIO_TEXLDD || opcode == kD3DSIO_TEXLDL || opcode == kD3DSIO_TEXDEPTH ||
        opcode == kD3DSIO_TEXDP3 || opcode == kD3DSIO_TEXDP3TEX || opcode == kD3DSIO_TEXM3x2DEPTH ||
        opcode == kD3DSIO_TEXM3x3 || opcode == kD3DSIO_TEXREG2RGB || opcode == kD3DSIO_BEM) {
      module.usesTexture = true;
    }
    module.instructions.push_back(std::move(instruction));
    offset += operandBytes;
  }

  return module;
}

std::string translateSpirvToMsl(const SpirvModule& module, const DrawDesc& desc, bool vertex) {
  std::ostringstream out;
  out << makeShaderPrelude(desc.clipPlaneMask != 0);
  if (vertex) {
    const auto inputLayout = decodeVertexShaderInputLayout(module, desc);
    const auto outputSemantics = collectVertexOutputSemantics(module);
    const bool traceShaderInputs = [] {
      const char* env = std::getenv("DXMT_TRACE_SHADER_INPUTS");
      return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
    }();
    if (traceShaderInputs) {
      std::ostringstream trace;
      trace << "[dxmt9-shader] vertex inputs";
      for (const auto& instruction : module.instructions) {
        if (instruction.opcode != kD3DSIO_DCL || instruction.operands.size() < 2) {
          continue;
        }
        const u32 semanticToken = instruction.operands[0];
        const auto dst = decodeRegisterRef(instruction.operands[1], module.stage);
        trace << " dcl(v" << dst.index
              << ":usage=" << ((semanticToken & kD3DSP_DCL_USAGE_MASK) >> kD3DSP_DCL_USAGE_SHIFT)
              << ",idx=" << ((semanticToken & kD3DSP_DCL_USAGEINDEX_MASK) >> kD3DSP_DCL_USAGEINDEX_SHIFT)
              << ",tok=0x" << std::hex << semanticToken << ",reg=0x" << instruction.operands[1] << std::dec << ")";
      }
      if (inputLayout) {
        trace << " mapped";
        for (size_t i = 0; i < inputLayout->inputs.size(); ++i) {
          const auto& binding = inputLayout->inputs[i];
          if (!binding.valid) {
            continue;
          }
          trace << " v" << i << "->off" << binding.offset << "/type" << binding.type
                << "/usage" << binding.usage << ":" << binding.usageIndex;
        }
      } else {
        trace << " mapped=none";
      }
      trace << " outputs";
      bool hasOutput = false;
      for (size_t i = 0; i < outputSemantics.size(); ++i) {
        const auto& semantic = outputSemantics[i];
        if (!semantic.valid) {
          continue;
        }
        hasOutput = true;
        trace << " o" << i << "->usage" << semantic.usage << ":" << semantic.usageIndex;
      }
      if (!hasOutput) {
        trace << "=none";
      }
      std::fprintf(stderr, "%s\n", trace.str().c_str());
      std::fflush(stderr);
    }
    out << "vertex VSOut dxmt9_vs(uint vid [[vertex_id]], constant DrawUniforms& uniforms [[buffer(0)]], "
           "device const uchar* stream0 [[buffer(1)]]) {\n";
    out << "  VSOut out;\n";
    out << "  float2 dxmt9_positions[3] = { float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0) };\n";
    out << "  float4 outPosition = float4(dxmt9_positions[vid % 3], 0.0, 1.0);\n";
    out << "  float4 outColor = float4(1.0f);\n";
    out << "  float4 outSecondaryColor = float4(0.0f);\n";
    out << "  float4 outTexcoord[" << kMaxTextureStages << "];\n";
    out << "  for (uint i = 0; i < " << kMaxTextureStages
        << "u; ++i) { outTexcoord[i] = float4(0.0f, 0.0f, 0.0f, 1.0f); }\n";
    out << "  float4 ignoredTexcoord = float4(0.0f);\n";
    out << "  float outFogFactor = 1.0f;\n";
    out << "  float outPointSize = 1.0f;\n";
    if (::dxmt9::debug::forceFullscreenVertexShader()) {
      out << "  out.position = outPosition;\n";
      out << "  out.color = outColor;\n";
      out << "  out.secondaryColor = outSecondaryColor;\n";
      for (size_t i = 0; i < kMaxTextureStages; ++i) {
        out << "  out.texcoord" << i << " = outTexcoord[" << i << "];\n";
      }
      out << "  out.fogFactor = outFogFactor;\n";
      out << "  out.pointSize = outPointSize;\n";
      if (desc.clipPlaneMask != 0) {
        out << "  for (uint i = 0; i < 6; ++i) { out.clipDistance[i] = 1.0f; }\n";
      }
      out << "  return out;\n";
      out << "}\n";
      out << "// decoded d3d hash " << module.hash << "\n";
      return out.str();
    }
    out << "  float4 vin[16];\n";
    out << "  for (uint i = 0; i < 16u; ++i) { vin[i] = float4(0.0f); }\n";
    out << "  vin[0] = float4(dxmt9_positions[vid % 3], 0.0f, 1.0f);\n";
    if (inputLayout) {
      out << "  const uint stride = uniforms.vertexStreamStride != 0u ? uniforms.vertexStreamStride : "
          << inputLayout->stride << "u;\n";
      out << "  const int vertexIndex = max(0, int(vid) + uniforms.vertexBaseIndex);\n";
      out << "  const uint base = uniforms.vertexStreamOffset + uint(vertexIndex) * stride;\n";
      for (size_t i = 0; i < inputLayout->inputs.size(); ++i) {
        const auto& binding = inputLayout->inputs[i];
        if (!binding.valid) {
          continue;
        }
        switch (binding.type) {
          case kD3DDeclTypeFloat1:
            out << "  vin[" << i << "] = float4(dxmt9_load_f32(stream0, base + " << binding.offset
                << "u), 0.0f, 0.0f, 1.0f);\n";
            break;
          case kD3DDeclTypeFloat2:
            out << "  vin[" << i << "] = float4(dxmt9_load_f32x2(stream0, base + " << binding.offset
                << "u), 0.0f, 1.0f);\n";
            break;
          case kD3DDeclTypeFloat3:
            out << "  vin[" << i << "] = float4(dxmt9_load_f32x3(stream0, base + " << binding.offset
                << "u), 1.0f);\n";
            break;
          case kD3DDeclTypeFloat4:
            out << "  vin[" << i << "] = dxmt9_load_f32x4(stream0, base + " << binding.offset << "u);\n";
            break;
          case kD3DDeclTypeD3DColor:
            out << "  vin[" << i << "] = dxmt9_load_d3dcolor(stream0, base + " << binding.offset << "u);\n";
            break;
          default:
            break;
        }
      }
    }
	    out << "  int a0 = 0;\n";
	    out << "  int aL = 0;\n";
	    out << "  float4 r[32];\n";
	    out << "  for (uint i = 0; i < 32u; ++i) { r[i] = float4(0.0f); }\n";
	    const auto constantUsage = collectConstantUsage(module);
	    emitConstantBindings(out, true, constantUsage);
	    emitPredicateBindings(out, shaderUsesPredicateRegisters(module));
    std::vector<FlowBlock> controlStack;
    size_t callDepth = 0;
    for (size_t instructionIndex = 0; instructionIndex < module.instructions.size(); ++instructionIndex) {
      const auto& instruction = module.instructions[instructionIndex];
      if (instruction.opcode == kD3DSIO_COMMENT || instruction.opcode == kD3DSIO_PHASE) {
        continue;
      }
      out << "  // " << opcodeName(instruction.opcode);
      for (size_t i = 0; i < instruction.operands.size(); ++i) {
        const bool destination = i == 0;
        out << (i == 0 ? " " : ", ");
        if (instruction.opcode == kD3DSIO_DEF && i > 0) {
          out << formatFloatLiteral(std::bit_cast<f32>(instruction.operands[i]));
        } else if (instruction.opcode == kD3DSIO_DEFI && i > 0) {
          out << static_cast<i32>(instruction.operands[i]);
        } else if (instruction.opcode == kD3DSIO_DEFB && i > 0) {
          out << (instruction.operands[i] != 0u ? "true" : "false");
        } else if (instruction.opcode == kD3DSIO_LABEL || instruction.opcode == kD3DSIO_CALL) {
          out << "label" << decodeLabelIndex(instruction.operands[i]);
        } else {
          out << decodeOperandToken(instruction.operands[i], module.stage, destination);
        }
      }
      out << "\n";

      auto readSrc = [&](size_t index) {
        if (index >= instruction.operands.size()) {
          std::ostringstream message;
          message << "missing D3D source operand"
                  << " opcode=" << opcodeName(instruction.opcode)
                  << " requestedIndex=" << index
                  << " operandCount=" << instruction.operands.size();
          throw std::runtime_error(message.str());
        }
        const auto token = instruction.operands[index];
        const auto reg = decodeRegisterRef(token, module.stage);
        std::string expr = readOperandExpression(instruction, reg, "vin", "in", true,
                                                 "outPosition", "outColor", "outSecondaryColor", "outTexcoord",
                                                 "outFogFactor", "outPointSize", "r", "cFloat", "cInt", "cBool",
                                                 "p", &outputSemantics);
        expr = applySwizzle(expr, decodeSwizzle(token));
        expr = applySourceModifier(std::move(expr), decodeSourceModifier(token));
        return expr;
      };

	    auto emitMaskedAssign = [&](const std::string& target, const std::string& value, u32 mask, bool scalar = false) {
	      if (scalar) {
	        out << "  " << target << " = " << value << ".x;\n";
	        return;
	      }
        const std::string finalValue = decodeDestModifier(instruction.operands.empty() ? 0u : instruction.operands[0]) ==
                                               1u
                                           ? "clamp(" + value + ", float4(0.0f), float4(1.0f))"
                                           : value;
        if (mask == 0xfu) {
          out << "  " << target << " = " << finalValue << ";\n";
        } else {
	        out << "  " << target << " = dxmt9_merge(" << target << ", " << finalValue << ", " << mask << "u);\n";
	      }
	    };
	    auto pixelColorTarget = [](u32 index) {
	      if (index >= kMaxRenderTargets) {
	        return std::string("ignoredColor");
	      }
	      return std::string("outColor[") + std::to_string(index) + "]";
	    };
	    auto texcoordTarget = [](u32 index) {
	      if (index >= kMaxTextureStages) {
	        return std::string("ignoredTexcoord");
	      }
	      return std::string("outTexcoord[") + std::to_string(index) + "]";
	    };
      auto emitVertexOutputAssign = [&](const D3DRegisterRef& dst, const std::string& value, u32 mask) {
        const auto mapped = vertexOutputMapping(dst, &outputSemantics);
        if (!mapped) {
          return false;
        }
        switch (mapped->target) {
          case VertexOutputMapping::Target::Position:
            emitMaskedAssign("outPosition", value, mask);
            break;
          case VertexOutputMapping::Target::Texcoord:
            emitMaskedAssign(texcoordTarget(mapped->index), value, mask);
            break;
          case VertexOutputMapping::Target::Color:
            emitMaskedAssign("outColor", value, mask);
            break;
          case VertexOutputMapping::Target::SecondaryColor:
            emitMaskedAssign("outSecondaryColor", value, mask);
            break;
          case VertexOutputMapping::Target::Fog:
            emitMaskedAssign("outFogFactor", value, mask, true);
            break;
          case VertexOutputMapping::Target::PointSize:
            emitMaskedAssign("outPointSize", value, mask, true);
            break;
        }
        return true;
      };

      if (instruction.opcode == kD3DSIO_LABEL) {
        if (instruction.operands.empty()) {
          throw std::runtime_error("LABEL requires a label operand");
        }
        out << "  // label " << decodeLabelIndex(instruction.operands[0]) << "\n";
        continue;
      }
      if (instruction.opcode == kD3DSIO_CALL) {
        if (instruction.operands.empty()) {
          throw std::runtime_error("CALL requires a label operand");
        }
        out << "  // call label " << decodeLabelIndex(instruction.operands[0]) << "\n";
        out << "  do {\n";
        ++callDepth;
        continue;
      }
      if (instruction.opcode == kD3DSIO_RET) {
        if (callDepth > 0) {
          --callDepth;
          out << "  break;\n";
          out << "  } while (false);\n";
        } else {
          out << "  return out;\n";
        }
        continue;
      }
      if (instruction.opcode == kD3DSIO_IF) {
        if (instruction.operands.empty()) {
          throw std::runtime_error("IF requires a condition operand");
        }
        out << "  if ((" << readSrc(0) << ").x != 0.0f) {\n";
        controlStack.push_back(FlowBlock{instruction.opcode, false});
        continue;
      }
      if (instruction.opcode == kD3DSIO_ELSE) {
        if (controlStack.empty() || controlStack.back().opcode != kD3DSIO_IF || controlStack.back().sawElse) {
          throw std::runtime_error("ELSE without matching IF");
        }
        controlStack.back().sawElse = true;
        out << "  } else {\n";
        continue;
      }
      if (instruction.opcode == kD3DSIO_ENDIF) {
        if (controlStack.empty() || controlStack.back().opcode != kD3DSIO_IF) {
          throw std::runtime_error("ENDIF without matching IF");
        }
        controlStack.pop_back();
        out << "  }\n";
        continue;
      }
      if (instruction.opcode == kD3DSIO_LOOP || instruction.opcode == kD3DSIO_REP) {
        if (instruction.operands.empty()) {
          throw std::runtime_error("loop requires a count operand");
        }
        const auto loopIndex = instructionIndex;
        const auto countExpr = "max(0, int(round(" + readSrc(0) + ".x)))";
        if (instruction.opcode == kD3DSIO_LOOP) {
          out << "  for (int dxmt9_loop_" << loopIndex << " = 0, dxmt9_loopCount_" << loopIndex << " = "
              << countExpr << "; dxmt9_loop_" << loopIndex << " < dxmt9_loopCount_" << loopIndex
              << "; ++dxmt9_loop_" << loopIndex << ") {\n";
        } else {
          out << "  for (int dxmt9_rep_" << loopIndex << " = 0, dxmt9_repCount_" << loopIndex << " = " << countExpr
              << "; dxmt9_rep_" << loopIndex << " < dxmt9_repCount_" << loopIndex << "; ++dxmt9_rep_"
              << loopIndex << ") {\n";
        }
        controlStack.push_back(FlowBlock{instruction.opcode, false});
        continue;
      }
      if (instruction.opcode == kD3DSIO_ENDLOOP || instruction.opcode == kD3DSIO_ENDREP) {
        if (controlStack.empty() ||
            (instruction.opcode == kD3DSIO_ENDLOOP && controlStack.back().opcode != kD3DSIO_LOOP) ||
            (instruction.opcode == kD3DSIO_ENDREP && controlStack.back().opcode != kD3DSIO_REP)) {
          throw std::runtime_error("loop end without matching opener");
        }
        controlStack.pop_back();
        out << "  }\n";
        continue;
      }
      if (instruction.opcode == kD3DSIO_BREAK) {
        out << "  break;\n";
        continue;
      }
      if (instruction.opcode == kD3DSIO_BREAKP) {
        if (instruction.operands.empty()) {
          throw std::runtime_error("BREAKP requires a predicate operand");
        }
        out << "  if ((" << readSrc(0) << ").x != 0.0f) { break; }\n";
        continue;
      }

      switch (instruction.opcode) {
        case kD3DSIO_NOP:
          break;
        case kD3DSIO_MOV: {
        if (instruction.operands.size() < 2) {
          throw std::runtime_error("MOV requires 2 operands");
        }
        const auto dst = decodeRegisterRef(instruction.operands[0], module.stage);
          const auto dstMask = decodeWriteMask(instruction.operands[0]);
          const auto value = readSrc(1);
          switch (dst.kind) {
            case D3DRegisterKind::Temp:
              emitMaskedAssign("r[" + std::to_string(dst.index) + "]", value, dstMask);
              break;
            case D3DRegisterKind::RastOut:
              if (dst.index == 0) {
                emitMaskedAssign("outPosition", value, dstMask);
              } else if (dst.index == 1) {
                emitMaskedAssign("outFogFactor", value, dstMask, true);
              } else if (dst.index == 2) {
                emitMaskedAssign("outPointSize", value, dstMask, true);
              } else {
                throw std::runtime_error("unsupported raster output register");
              }
              break;
            case D3DRegisterKind::AttrOut:
              if (dst.index == 0) {
                emitMaskedAssign("outColor", value, dstMask);
              } else if (dst.index == 1) {
                emitMaskedAssign("outSecondaryColor", value, dstMask);
              } else {
                throw std::runtime_error("unsupported attribute output register");
              }
              break;
            case D3DRegisterKind::TexCoordOut:
              if (!emitVertexOutputAssign(dst, value, dstMask)) {
                emitMaskedAssign(texcoordTarget(dst.index), value, dstMask);
              }
              break;
	          case D3DRegisterKind::ColorOut:
	            emitMaskedAssign(pixelColorTarget(dst.index), value, dstMask);
	            break;
            case D3DRegisterKind::ConstFloat:
              out << "  cFloat[" << dst.index << "] = " << value << ";\n";
              break;
            case D3DRegisterKind::ConstInt:
              out << "  cInt[" << dst.index << "] = int4(" << value << ");\n";
              break;
            case D3DRegisterKind::ConstBool:
              out << "  cBool[" << dst.index << "] = " << value << ".x != 0.0f ? 1u : 0u;\n";
              break;
          default:
            throw std::runtime_error("unsupported MOV destination");
        }
        break;
      }
      case kD3DSIO_MOVA: {
        if (instruction.operands.size() < 2) {
          throw std::runtime_error("MOVA requires 2 operands");
        }
        const auto dst = decodeRegisterRef(instruction.operands[0], module.stage);
        const auto value = readSrc(1);
        switch (dst.kind) {
          case D3DRegisterKind::Address:
            out << "  a0 = int(round(" << value << ".x));\n";
            break;
          case D3DRegisterKind::Loop:
            out << "  aL = int(round(" << value << ".x));\n";
            break;
          default:
            throw std::runtime_error("MOVA requires an address register destination");
        }
        break;
      }
      case kD3DSIO_SETP: {
        if (instruction.operands.size() < 2) {
          throw std::runtime_error("SETP requires 2 operands");
        }
        const auto dst = decodeRegisterRef(instruction.operands[0], module.stage);
        if (dst.kind != D3DRegisterKind::Predicate) {
          throw std::runtime_error("SETP requires a predicate register destination");
        }
        out << "  p[" << dst.index << "] = (" << readSrc(1) << ").x != 0.0f;\n";
        break;
      }
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
        case kD3DSIO_TEXLDL: {
          if (instruction.operands.size() < 2) {
            throw std::runtime_error("missing D3D destination or source operand");
          }
          const auto dst = decodeRegisterRef(instruction.operands[0], module.stage);
          const auto dstMask = decodeWriteMask(instruction.operands[0]);
          std::string value;
          switch (instruction.opcode) {
            case kD3DSIO_ADD:
              value = "(" + readSrc(1) + " + " + readSrc(2) + ")";
              break;
            case kD3DSIO_SUB:
              value = "(" + readSrc(1) + " - " + readSrc(2) + ")";
              break;
            case kD3DSIO_MUL:
              value = "(" + readSrc(1) + " * " + readSrc(2) + ")";
              break;
            case kD3DSIO_MAD:
              value = "(" + readSrc(1) + " * " + readSrc(2) + " + " + readSrc(3) + ")";
              break;
            case kD3DSIO_MIN:
              value = "min(" + readSrc(1) + ", " + readSrc(2) + ")";
              break;
            case kD3DSIO_MAX:
              value = "max(" + readSrc(1) + ", " + readSrc(2) + ")";
              break;
            case kD3DSIO_SLT:
              value = "select(float4(0.0f), float4(1.0f), (" + readSrc(1) + ") < (" + readSrc(2) + "))";
              break;
            case kD3DSIO_SGE:
              value = "select(float4(0.0f), float4(1.0f), (" + readSrc(1) + ") >= (" + readSrc(2) + "))";
              break;
            case kD3DSIO_EXP:
            case kD3DSIO_EXPP:
              value = "float4(exp2(" + readSrc(1) + "))";
              break;
            case kD3DSIO_LOG:
            case kD3DSIO_LOGP:
              value = "float4(log2(max(" + readSrc(1) + ", float4(1.0e-8f))))";
              break;
            case kD3DSIO_SINCOS:
              value = "float4(sin(" + readSrc(1) + "), cos(" + readSrc(1) + "), 0.0f, 0.0f)";
              break;
            case kD3DSIO_M4x4: {
              const auto base = decodeRegisterRef(instruction.operands[2], module.stage);
              if (base.kind != D3DRegisterKind::ConstFloat) {
                throw std::runtime_error("M4x4 requires a float constant matrix base");
              }
              const auto src = readSrc(1);
              value = "float4(dot(" + src + ", cFloat[" + std::to_string(base.index + 0) + "]), dot(" + src +
                      ", cFloat[" + std::to_string(base.index + 1) + "]), dot(" + src + ", cFloat[" +
                      std::to_string(base.index + 2) + "]), dot(" + src + ", cFloat[" +
                      std::to_string(base.index + 3) + "]))";
              break;
            }
            case kD3DSIO_M4x3: {
              const auto base = decodeRegisterRef(instruction.operands[2], module.stage);
              if (base.kind != D3DRegisterKind::ConstFloat) {
                throw std::runtime_error("M4x3 requires a float constant matrix base");
              }
              const auto src = readSrc(1);
              value = "float4(dot(" + src + ", cFloat[" + std::to_string(base.index + 0) + "]), dot(" + src +
                      ", cFloat[" + std::to_string(base.index + 1) + "]), dot(" + src + ", cFloat[" +
                      std::to_string(base.index + 2) + "]), 0.0f)";
              break;
            }
            case kD3DSIO_M3x4: {
              const auto base = decodeRegisterRef(instruction.operands[2], module.stage);
              if (base.kind != D3DRegisterKind::ConstFloat) {
                throw std::runtime_error("M3x4 requires a float constant matrix base");
              }
              const auto src = readSrc(1);
              value = "float4(dot((" + src + ").xyz, cFloat[" + std::to_string(base.index + 0) +
                      "].xyz), dot((" + src + ").xyz, cFloat[" + std::to_string(base.index + 1) +
                      "].xyz), dot((" + src + ").xyz, cFloat[" + std::to_string(base.index + 2) +
                      "].xyz), dot((" + src + ").xyz, cFloat[" + std::to_string(base.index + 3) + "].xyz))";
              break;
            }
            case kD3DSIO_M3x3: {
              const auto base = decodeRegisterRef(instruction.operands[2], module.stage);
              if (base.kind != D3DRegisterKind::ConstFloat) {
                throw std::runtime_error("M3x3 requires a float constant matrix base");
              }
              const auto src = readSrc(1);
              value = "float4(dot((" + src + ").xyz, cFloat[" + std::to_string(base.index + 0) +
                      "].xyz), dot((" + src + ").xyz, cFloat[" + std::to_string(base.index + 1) +
                      "].xyz), dot((" + src + ").xyz, cFloat[" + std::to_string(base.index + 2) +
                      "].xyz), 0.0f)";
              break;
            }
            case kD3DSIO_M3x2: {
              const auto base = decodeRegisterRef(instruction.operands[2], module.stage);
              if (base.kind != D3DRegisterKind::ConstFloat) {
                throw std::runtime_error("M3x2 requires a float constant matrix base");
              }
              const auto src = readSrc(1);
              value = "float4(dot((" + src + ").xyz, cFloat[" + std::to_string(base.index + 0) +
                      "].xyz), dot((" + src + ").xyz, cFloat[" + std::to_string(base.index + 1) +
                      "].xyz), 0.0f, 0.0f)";
              break;
            }
            case kD3DSIO_RCP:
              value = "float4(1.0f) / max(" + readSrc(1) + ", float4(1.0e-8f))";
              break;
            case kD3DSIO_RSQ:
              value = "rsqrt(max(" + readSrc(1) + ", float4(1.0e-8f)))";
              break;
            case kD3DSIO_FRC:
              value = "fract(" + readSrc(1) + ")";
              break;
            case kD3DSIO_LRP:
              value = "mix(" + readSrc(3) + ", " + readSrc(2) + ", " + readSrc(1) + ")";
              break;
            case kD3DSIO_DP3:
              value = "float4(dot((" + readSrc(1) + ").xyz, (" + readSrc(2) + ").xyz))";
              break;
            case kD3DSIO_DP4:
              value = "float4(dot(" + readSrc(1) + ", " + readSrc(2) + "))";
              break;
            case kD3DSIO_CND:
              value = "select(" + readSrc(3) + ", " + readSrc(2) + ", " + readSrc(1) + " > float4(0.5f))";
              break;
            case kD3DSIO_CMP:
              value = "select(" + readSrc(3) + ", " + readSrc(2) + ", " + readSrc(1) + " >= float4(0.0f))";
              break;
            case kD3DSIO_DP2ADD:
              value = "float4(dot((" + readSrc(1) + ").xy, (" + readSrc(2) + ").xy) + (" + readSrc(3) + ").x)";
              break;
            case kD3DSIO_POW:
              value = "pow(" + readSrc(1) + ", " + readSrc(2) + ")";
              break;
            case kD3DSIO_CRS:
              value = "float4(cross((" + readSrc(1) + ").xyz, (" + readSrc(2) + ").xyz), 0.0f)";
              break;
            case kD3DSIO_SGN:
              value = "sign(" + readSrc(1) + ")";
              break;
            case kD3DSIO_ABS:
              value = "abs(" + readSrc(1) + ")";
              break;
            case kD3DSIO_NRM:
              value = "float4(normalize((" + readSrc(1) + ").xyz), 0.0f)";
              break;
            case kD3DSIO_DSX:
              value = "dfdx(" + readSrc(1) + ")";
              break;
            case kD3DSIO_DSY:
              value = "dfdy(" + readSrc(1) + ")";
              break;
            case kD3DSIO_TEXLDD:
              value = "tex0.sample(samp0, " + readSrc(1) + ".xy)";
              break;
            case kD3DSIO_TEXLDL:
              value = "tex0.sample(samp0, " + readSrc(1) + ".xy, level(" + readSrc(1) + ".w))";
              break;
            case kD3DSIO_TEX:
              value = "tex0.sample(samp0, " + readSrc(1) + ".xy)";
              break;
            default:
              throw std::runtime_error("unsupported arithmetic opcode");
          }
          if (decodeDestModifier(instruction.operands[0]) == 1u) {
            value = "clamp(" + value + ", float4(0.0f), float4(1.0f))";
          }
          switch (dst.kind) {
            case D3DRegisterKind::Temp:
              emitMaskedAssign("r[" + std::to_string(dst.index) + "]", value, dstMask);
              break;
            case D3DRegisterKind::RastOut:
              if (dst.index == 0) {
                emitMaskedAssign("outPosition", value, dstMask);
              } else if (dst.index == 1) {
                emitMaskedAssign("outFogFactor", value, dstMask, true);
              } else if (dst.index == 2) {
                emitMaskedAssign("outPointSize", value, dstMask, true);
              } else {
                throw std::runtime_error("unsupported raster output register");
              }
              break;
            case D3DRegisterKind::AttrOut:
              if (dst.index == 0) {
                emitMaskedAssign("outColor", value, dstMask);
              } else if (dst.index == 1) {
                emitMaskedAssign("outSecondaryColor", value, dstMask);
              } else {
                throw std::runtime_error("unsupported attribute output register");
              }
              break;
            case D3DRegisterKind::TexCoordOut:
              if (!emitVertexOutputAssign(dst, value, dstMask)) {
                emitMaskedAssign(texcoordTarget(dst.index), value, dstMask);
              }
              break;
	          case D3DRegisterKind::ColorOut:
	            emitMaskedAssign(pixelColorTarget(dst.index), value, dstMask);
	            break;
            case D3DRegisterKind::DepthOut:
              throw std::runtime_error("depth output is not supported yet");
            default:
              throw std::runtime_error("unsupported arithmetic destination");
          }
          break;
        }
        case kD3DSIO_DEF: {
          if (instruction.operands.size() < 5) {
            throw std::runtime_error("DEF requires 5 operands");
          }
          const auto dst = decodeRegisterRef(instruction.operands[0], module.stage);
          const auto values = std::array<f32, 4>{std::bit_cast<f32>(instruction.operands[1]),
                                                 std::bit_cast<f32>(instruction.operands[2]),
                                                 std::bit_cast<f32>(instruction.operands[3]),
                                                 std::bit_cast<f32>(instruction.operands[4])};
          if (dst.kind != D3DRegisterKind::ConstFloat) {
            std::ostringstream message;
            message << "DEF requires a float constant destination"
                    << " token=0x" << std::hex << instruction.operands[0]
                    << " regType=" << std::dec << decodeRegisterType(instruction.operands[0])
                    << " regIndex=" << dst.index
                    << " kind=" << static_cast<u32>(dst.kind);
            throw std::runtime_error(message.str());
          }
          out << "  cFloat[" << dst.index << "] = " << formatFloatVec4(values) << ";\n";
          break;
        }
        case kD3DSIO_DEFI: {
          if (instruction.operands.size() < 5) {
            throw std::runtime_error("DEFI requires 5 operands");
          }
          const auto dst = decodeRegisterRef(instruction.operands[0], module.stage);
          const auto values = std::array<i32, 4>{static_cast<i32>(instruction.operands[1]),
                                                 static_cast<i32>(instruction.operands[2]),
                                                 static_cast<i32>(instruction.operands[3]),
                                                 static_cast<i32>(instruction.operands[4])};
          if (dst.kind != D3DRegisterKind::ConstInt) {
            throw std::runtime_error("DEFI requires an integer constant destination");
          }
          out << "  cInt[" << dst.index << "] = " << formatIntVec4(values) << ";\n";
          break;
        }
        case kD3DSIO_DEFB: {
          if (instruction.operands.size() < 2) {
            throw std::runtime_error("DEFB requires 2 operands");
          }
          const auto dst = decodeRegisterRef(instruction.operands[0], module.stage);
          if (dst.kind != D3DRegisterKind::ConstBool) {
            throw std::runtime_error("DEFB requires a boolean constant destination");
          }
          out << "  cBool[" << dst.index << "] = " << (instruction.operands[1] != 0u ? "1u" : "0u") << ";\n";
          break;
        }
        case kD3DSIO_DCL:
          // No-op for now: DCL informs semantics, but the current translator maps outputs by register class.
          break;
        case kD3DSIO_BEM:
        case kD3DSIO_TEXDEPTH:
        case kD3DSIO_TEXREG2RGB:
        case kD3DSIO_TEXDP3TEX:
        case kD3DSIO_TEXM3x2DEPTH:
        case kD3DSIO_TEXDP3:
        case kD3DSIO_TEXM3x3:
          // Texture instructions are lowered through the supported TEXLD-style sample path above when present.
          break;
        default:
          throw std::runtime_error("unsupported D3D opcode: " + opcodeName(instruction.opcode));
      }
    }
    if (!controlStack.empty()) {
      throw std::runtime_error("unbalanced D3D control flow");
    }
    if (callDepth != 0) {
      throw std::runtime_error("unbalanced D3D CALL/RET");
    }

    out << "  out.position = outPosition;\n";
    if (::dxmt9::debug::flipTranslatedVertexY()) {
      out << "  out.position.y = -out.position.y;\n";
    }
    out << "  out.color = outColor;\n";
    out << "  out.secondaryColor = outSecondaryColor;\n";
    for (size_t i = 0; i < kMaxTextureStages; ++i) {
      out << "  out.texcoord" << i << " = outTexcoord[" << i << "];\n";
    }
    out << "  out.fogFactor = outFogFactor;\n";
    out << "  out.pointSize = outPointSize;\n";
    out << "  out.position.xy += uniforms.halfPixelFixup * out.position.w;\n";
    if (desc.clipPlaneMask != 0) {
      out << "  for (uint i = 0; i < 6; ++i) {\n";
      out << "    if ((uniforms.clipPlaneMask & (1u << i)) != 0u) {\n";
      out << "      out.clipDistance[i] = dot(uniforms.clipPlanes[i], out.position);\n";
      out << "    }\n";
      out << "  }\n";
    }
    out << "  return out;\n";
    out << "}\n";
    out << "// decoded d3d hash " << module.hash << "\n";
    return out.str();
  }

  const auto samplerUsage = collectPixelSamplerUsage(module, desc);
  const auto pixelInputSemantics = collectPixelInputSemantics(module);
  const bool textured = std::any_of(samplerUsage.begin(), samplerUsage.end(), [](bool used) { return used; });
  const bool traceShaderInputs = [] {
    const char* env = std::getenv("DXMT_TRACE_SHADER_INPUTS");
    return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
  }();
  if (traceShaderInputs) {
    std::ostringstream trace;
    trace << "[dxmt9-shader] pixel inputs";
    for (const auto& instruction : module.instructions) {
      if (instruction.opcode != kD3DSIO_DCL || instruction.operands.empty()) {
        continue;
      }
      trace << " dcl(" << decodeOperandToken(instruction.operands[0], module.stage, true)
            << ",type=" << decodeRegisterType(instruction.operands[0])
            << ",tok=0x" << std::hex << instruction.operands[0] << std::dec << ")";
    }
    std::fprintf(stderr, "%s\n", trace.str().c_str());
    std::fflush(stderr);
  }
  const u32 colorOutputCount = pixelColorOutputCount(module);
  const bool writesDepth = pixelWritesDepth(module);
  const bool usesFragmentOutStruct = colorOutputCount > 1u || writesDepth;
  if (usesFragmentOutStruct) {
    out << "struct FSOut {\n";
    for (u32 i = 0; i < colorOutputCount; ++i) {
      out << "  float4 color" << i << " [[color(" << i << ")]];\n";
    }
    if (writesDepth) {
      out << "  float depth [[depth(any)]];\n";
    }
    out << "};\n";
  }

  const char* fragmentReturnType = usesFragmentOutStruct ? "FSOut" : "float4";
  if (textured) {
    out << "fragment " << fragmentReturnType
        << " dxmt9_fs(VSOut in [[stage_in]], constant DrawUniforms& uniforms [[buffer(0)]], ";
    emitFragmentTextureArguments(out, samplerUsage);
    out << ") {\n";
  } else {
    out << "fragment " << fragmentReturnType
        << " dxmt9_fs(VSOut in [[stage_in]], constant DrawUniforms& uniforms [[buffer(0)]]) {\n";
  }
  auto emitFragmentDebugReturn = [&](std::string_view valueExpr) {
    if (usesFragmentOutStruct) {
      out << "  FSOut result;\n";
      for (u32 i = 0; i < colorOutputCount; ++i) {
        out << "  result.color" << i << " = " << valueExpr << ";\n";
      }
      if (writesDepth) {
        out << "  result.depth = in.position.z;\n";
      }
      out << "  return result;\n";
    } else {
      out << "  return " << valueExpr << ";\n";
    }
  };
  if (::dxmt9::debug::forceFragmentShaderColor()) {
    emitFragmentDebugReturn("float4(1.0f, 0.0f, 1.0f, 1.0f)");
    out << "}\n";
    out << "// decoded d3d hash " << module.hash << "\n";
    return out.str();
  }
  if (const char* mode = std::getenv("DXMT_DEBUG_FRAGMENT_MODE"); mode && mode[0] != '\0') {
    if (std::strcmp(mode, "uv") == 0) {
      emitFragmentDebugReturn("float4(fract(dxmt9_select_texcoord(in, 0u).xy), 0.0f, 1.0f)");
    } else if (std::strcmp(mode, "uv_saturate") == 0) {
      emitFragmentDebugReturn("float4(saturate(dxmt9_select_texcoord(in, 0u).xy), 0.0f, 1.0f)");
    } else if (textured && std::strcmp(mode, "tex0_center") == 0) {
      emitFragmentDebugReturn("tex0.sample(samp0, float2(0.5f, 0.5f))");
    } else if (textured && std::strcmp(mode, "tex0_uv") == 0) {
      emitFragmentDebugReturn("tex0.sample(samp0, dxmt9_select_texcoord(in, 0u).xy)");
    } else if (textured && std::strcmp(mode, "tex0_uv_clamp") == 0) {
      emitFragmentDebugReturn("tex0.sample(samp0, clamp(dxmt9_select_texcoord(in, 0u).xy, float2(0.0f), float2(1.0f)))");
    } else if (textured && std::strcmp(mode, "tex0_uv_flip") == 0) {
      emitFragmentDebugReturn("tex0.sample(samp0, float2(dxmt9_select_texcoord(in, 0u).x, 1.0f - dxmt9_select_texcoord(in, 0u).y))");
    } else {
      emitFragmentDebugReturn("float4(0.0f, 1.0f, 0.0f, 1.0f)");
    }
    out << "}\n";
    out << "// decoded d3d hash " << module.hash << "\n";
    return out.str();
  }
  out << "  float4 color = float4(1.0f);\n";
  out << "  float4 outColor[" << kMaxRenderTargets << "];\n";
  for (u32 i = 0; i < kMaxRenderTargets; ++i) {
    out << "  outColor[" << i << "] = float4(1.0f);\n";
  }
  out << "  float4 ignoredColor = float4(0.0f);\n";
  out << "  float4 outSecondaryColor = float4(0.0f);\n";
  out << "  float4 outTexcoord[" << kMaxTextureStages << "];\n";
  out << "  for (uint i = 0; i < " << kMaxTextureStages
      << "u; ++i) { outTexcoord[i] = float4(0.0f, 0.0f, 0.0f, 1.0f); }\n";
  out << "  float4 ignoredTexcoord = float4(0.0f);\n";
  out << "  float4 outPosition = float4(0.0f);\n";
  out << "  float outDepth = in.position.z;\n";
  out << "  float outFogFactor = 1.0f;\n";
  out << "  float outPointSize = 1.0f;\n";
	  out << "  int a0 = 0;\n";
	  out << "  int aL = 0;\n";
	  out << "  float4 r[32];\n";
	  out << "  for (uint i = 0; i < 32u; ++i) { r[i] = float4(0.0f); }\n";
	  const auto constantUsage = collectConstantUsage(module);
	  emitConstantBindings(out, false, constantUsage);
	  emitPredicateBindings(out, shaderUsesPredicateRegisters(module));
    std::vector<FlowBlock> controlStack;
    size_t callDepth = 0;
    for (size_t instructionIndex = 0; instructionIndex < module.instructions.size(); ++instructionIndex) {
      const auto& instruction = module.instructions[instructionIndex];
    if (instruction.opcode == kD3DSIO_COMMENT || instruction.opcode == kD3DSIO_PHASE) {
      continue;
    }
    out << "  // " << opcodeName(instruction.opcode);
    for (size_t i = 0; i < instruction.operands.size(); ++i) {
      const bool destination = i == 0;
      out << (i == 0 ? " " : ", ");
      if (instruction.opcode == kD3DSIO_DEF && i > 0) {
        out << formatFloatLiteral(std::bit_cast<f32>(instruction.operands[i]));
      } else if (instruction.opcode == kD3DSIO_DEFI && i > 0) {
        out << static_cast<i32>(instruction.operands[i]);
      } else if (instruction.opcode == kD3DSIO_DEFB && i > 0) {
        out << (instruction.operands[i] != 0u ? "true" : "false");
      } else if (instruction.opcode == kD3DSIO_LABEL || instruction.opcode == kD3DSIO_CALL) {
        out << "label" << decodeLabelIndex(instruction.operands[i]);
      } else {
        out << decodeOperandToken(instruction.operands[i], module.stage, destination);
      }
    }
    out << "\n";

    auto readSrc = [&](size_t index) {
      if (index >= instruction.operands.size()) {
        std::ostringstream message;
        message << "missing D3D source operand"
                << " opcode=" << opcodeName(instruction.opcode)
                << " requestedIndex=" << index
                << " operandCount=" << instruction.operands.size();
        throw std::runtime_error(message.str());
      }
      const auto token = instruction.operands[index];
      const auto reg = decodeRegisterRef(token, module.stage);
      std::string expr;
      if (reg.kind == D3DRegisterKind::Input) {
        expr = readPixelInputExpression(token, "in", pixelInputSemantics);
      } else {
        expr = readOperandExpression(instruction, reg, "float4(0.0f)", "in", false, "outPosition",
                                     "outColor", "outSecondaryColor", "outTexcoord", "outFogFactor",
                                     "outPointSize", "r", "cFloat", "cInt", "cBool", "p");
      }
      expr = applySwizzle(expr, decodeSwizzle(token));
      expr = applySourceModifier(std::move(expr), decodeSourceModifier(token));
      return expr;
    };

    auto emitMaskedAssign = [&](const std::string& target, const std::string& value, u32 mask, bool scalar = false) {
      if (scalar) {
        out << "  " << target << " = " << value << ".x;\n";
        return;
      }
      const std::string finalValue = decodeDestModifier(instruction.operands.empty() ? 0u : instruction.operands[0]) ==
                                             1u
                                         ? "clamp(" + value + ", float4(0.0f), float4(1.0f))"
                                         : value;
      if (mask == 0xfu) {
        out << "  " << target << " = " << finalValue << ";\n";
      } else {
        out << "  " << target << " = dxmt9_merge(" << target << ", " << finalValue << ", " << mask << "u);\n";
      }
    };
    auto pixelColorTarget = [](u32 index) {
      if (index >= kMaxRenderTargets) {
        return std::string("ignoredColor");
      }
      return std::string("outColor[") + std::to_string(index) + "]";
    };
    auto texcoordTarget = [](u32 index) {
      if (index >= kMaxTextureStages) {
        return std::string("ignoredTexcoord");
      }
      return std::string("outTexcoord[") + std::to_string(index) + "]";
    };
    const bool forcePixelVFlip = ::dxmt9::debug::forcePixelVFlip();
    auto sampleCoord = [forcePixelVFlip](const std::string& coord) {
      if (forcePixelVFlip) {
        return "float2((" + coord + ").x, 1.0f - (" + coord + ").y)";
      }
      return "(" + coord + ").xy";
    };

    if (instruction.opcode == kD3DSIO_LABEL) {
      if (instruction.operands.empty()) {
        throw std::runtime_error("LABEL requires a label operand");
      }
      out << "  // label " << decodeLabelIndex(instruction.operands[0]) << "\n";
      continue;
    }
    if (instruction.opcode == kD3DSIO_CALL) {
      if (instruction.operands.empty()) {
        throw std::runtime_error("CALL requires a label operand");
      }
      out << "  // call label " << decodeLabelIndex(instruction.operands[0]) << "\n";
      out << "  do {\n";
      ++callDepth;
      continue;
    }
    if (instruction.opcode == kD3DSIO_RET) {
      if (callDepth > 0) {
        --callDepth;
        out << "  break;\n";
        out << "  } while (false);\n";
      } else {
        out << "  return color;\n";
      }
      continue;
    }
    if (instruction.opcode == kD3DSIO_IF) {
      if (instruction.operands.empty()) {
        throw std::runtime_error("IF requires a condition operand");
      }
      out << "  if ((" << readSrc(0) << ").x != 0.0f) {\n";
      controlStack.push_back(FlowBlock{instruction.opcode, false});
      continue;
    }
    if (instruction.opcode == kD3DSIO_ELSE) {
      if (controlStack.empty() || controlStack.back().opcode != kD3DSIO_IF || controlStack.back().sawElse) {
        throw std::runtime_error("ELSE without matching IF");
      }
      controlStack.back().sawElse = true;
      out << "  } else {\n";
      continue;
    }
    if (instruction.opcode == kD3DSIO_ENDIF) {
      if (controlStack.empty() || controlStack.back().opcode != kD3DSIO_IF) {
        throw std::runtime_error("ENDIF without matching IF");
      }
      controlStack.pop_back();
      out << "  }\n";
      continue;
    }
    if (instruction.opcode == kD3DSIO_LOOP || instruction.opcode == kD3DSIO_REP) {
      if (instruction.operands.empty()) {
        throw std::runtime_error("loop requires a count operand");
      }
      const auto loopIndex = instructionIndex;
      const auto countExpr = "max(0, int(round(" + readSrc(0) + ".x)))";
      if (instruction.opcode == kD3DSIO_LOOP) {
        out << "  for (int dxmt9_loop_" << loopIndex << " = 0, dxmt9_loopCount_" << loopIndex << " = " << countExpr
            << "; dxmt9_loop_" << loopIndex << " < dxmt9_loopCount_" << loopIndex << "; ++dxmt9_loop_"
            << loopIndex << ") {\n";
      } else {
        out << "  for (int dxmt9_rep_" << loopIndex << " = 0, dxmt9_repCount_" << loopIndex << " = " << countExpr
            << "; dxmt9_rep_" << loopIndex << " < dxmt9_repCount_" << loopIndex << "; ++dxmt9_rep_"
            << loopIndex << ") {\n";
      }
      controlStack.push_back(FlowBlock{instruction.opcode, false});
      continue;
    }
    if (instruction.opcode == kD3DSIO_ENDLOOP || instruction.opcode == kD3DSIO_ENDREP) {
      if (controlStack.empty() ||
          (instruction.opcode == kD3DSIO_ENDLOOP && controlStack.back().opcode != kD3DSIO_LOOP) ||
          (instruction.opcode == kD3DSIO_ENDREP && controlStack.back().opcode != kD3DSIO_REP)) {
        throw std::runtime_error("loop end without matching opener");
      }
      controlStack.pop_back();
      out << "  }\n";
      continue;
    }
    if (instruction.opcode == kD3DSIO_BREAK) {
      out << "  break;\n";
      continue;
    }
    if (instruction.opcode == kD3DSIO_BREAKP) {
      if (instruction.operands.empty()) {
        throw std::runtime_error("BREAKP requires a predicate operand");
      }
      out << "  if ((" << readSrc(0) << ").x != 0.0f) { break; }\n";
      continue;
    }

    switch (instruction.opcode) {
      case kD3DSIO_NOP:
        break;
      case kD3DSIO_DEF: {
        const auto dst = decodeRegisterRef(instruction.operands[0], module.stage);
        const auto values = std::array<f32, 4>{std::bit_cast<f32>(instruction.operands[1]),
                                               std::bit_cast<f32>(instruction.operands[2]),
                                               std::bit_cast<f32>(instruction.operands[3]),
                                               std::bit_cast<f32>(instruction.operands[4])};
        if (dst.kind != D3DRegisterKind::ConstFloat) {
          std::ostringstream message;
          message << "DEF requires a float constant destination"
                  << " token=0x" << std::hex << instruction.operands[0]
                  << " regType=" << std::dec << decodeRegisterType(instruction.operands[0])
                  << " regIndex=" << dst.index
                  << " kind=" << static_cast<u32>(dst.kind);
          throw std::runtime_error(message.str());
        }
        out << "  cFloat[" << dst.index << "] = " << formatFloatVec4(values) << ";\n";
        break;
      }
      case kD3DSIO_DEFI: {
        const auto dst = decodeRegisterRef(instruction.operands[0], module.stage);
        const auto values = std::array<i32, 4>{static_cast<i32>(instruction.operands[1]),
                                               static_cast<i32>(instruction.operands[2]),
                                               static_cast<i32>(instruction.operands[3]),
                                               static_cast<i32>(instruction.operands[4])};
        if (dst.kind != D3DRegisterKind::ConstInt) {
          throw std::runtime_error("DEFI requires an integer constant destination");
        }
        out << "  cInt[" << dst.index << "] = " << formatIntVec4(values) << ";\n";
        break;
      }
      case kD3DSIO_DEFB: {
        const auto dst = decodeRegisterRef(instruction.operands[0], module.stage);
        if (dst.kind != D3DRegisterKind::ConstBool) {
          throw std::runtime_error("DEFB requires a boolean constant destination");
        }
        out << "  cBool[" << dst.index << "] = " << (instruction.operands[1] != 0u ? "1u" : "0u") << ";\n";
        break;
      }
      case kD3DSIO_MOV: {
        const auto dst = decodeRegisterRef(instruction.operands[0], module.stage);
        const auto dstMask = decodeWriteMask(instruction.operands[0]);
        const auto value = readSrc(1);
        switch (dst.kind) {
          case D3DRegisterKind::Temp:
            emitMaskedAssign("r[" + std::to_string(dst.index) + "]", value, dstMask);
            break;
          case D3DRegisterKind::ColorOut:
            emitMaskedAssign(pixelColorTarget(dst.index), value, dstMask);
            break;
          case D3DRegisterKind::TexCoordOut:
            emitMaskedAssign(texcoordTarget(dst.index), value, dstMask);
            break;
          case D3DRegisterKind::DepthOut:
            emitMaskedAssign("outDepth", value, dstMask, true);
            break;
          case D3DRegisterKind::ConstFloat:
            out << "  cFloat[" << dst.index << "] = " << value << ";\n";
            break;
          case D3DRegisterKind::ConstInt:
            out << "  cInt[" << dst.index << "] = int4(" << value << ");\n";
            break;
          case D3DRegisterKind::ConstBool:
            out << "  cBool[" << dst.index << "] = " << value << ".x != 0.0f ? 1u : 0u;\n";
            break;
          default:
            throw std::runtime_error("unsupported MOV destination");
        }
        break;
      }
      case kD3DSIO_MOVA: {
        if (instruction.operands.size() < 2) {
          throw std::runtime_error("MOVA requires 2 operands");
        }
        const auto dst = decodeRegisterRef(instruction.operands[0], module.stage);
        const auto value = readSrc(1);
        switch (dst.kind) {
          case D3DRegisterKind::Address:
            out << "  a0 = int(round(" << value << ".x));\n";
            break;
          case D3DRegisterKind::Loop:
            out << "  aL = int(round(" << value << ".x));\n";
            break;
          default:
            throw std::runtime_error("MOVA requires an address register destination");
        }
        break;
      }
      case kD3DSIO_SETP: {
        if (instruction.operands.size() < 2) {
          throw std::runtime_error("SETP requires 2 operands");
        }
        const auto dst = decodeRegisterRef(instruction.operands[0], module.stage);
        if (dst.kind != D3DRegisterKind::Predicate) {
          throw std::runtime_error("SETP requires a predicate register destination");
        }
        out << "  p[" << dst.index << "] = (" << readSrc(1) << ").x != 0.0f;\n";
        break;
      }
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
      case kD3DSIO_TEXLDL: {
        const auto dst = decodeRegisterRef(instruction.operands[0], module.stage);
        const auto dstMask = decodeWriteMask(instruction.operands[0]);
        std::string value;
        switch (instruction.opcode) {
          case kD3DSIO_ADD:
            value = "(" + readSrc(1) + " + " + readSrc(2) + ")";
            break;
          case kD3DSIO_SUB:
            value = "(" + readSrc(1) + " - " + readSrc(2) + ")";
            break;
          case kD3DSIO_MUL:
            value = "(" + readSrc(1) + " * " + readSrc(2) + ")";
            break;
          case kD3DSIO_MAD:
            value = "(" + readSrc(1) + " * " + readSrc(2) + " + " + readSrc(3) + ")";
            break;
          case kD3DSIO_MIN:
            value = "min(" + readSrc(1) + ", " + readSrc(2) + ")";
            break;
          case kD3DSIO_MAX:
            value = "max(" + readSrc(1) + ", " + readSrc(2) + ")";
            break;
          case kD3DSIO_SLT:
            value = "select(float4(0.0f), float4(1.0f), (" + readSrc(1) + ") < (" + readSrc(2) + "))";
            break;
          case kD3DSIO_SGE:
            value = "select(float4(0.0f), float4(1.0f), (" + readSrc(1) + ") >= (" + readSrc(2) + "))";
            break;
          case kD3DSIO_EXP:
          case kD3DSIO_EXPP:
            value = "float4(exp2(" + readSrc(1) + "))";
            break;
          case kD3DSIO_LOG:
          case kD3DSIO_LOGP:
            value = "float4(log2(max(" + readSrc(1) + ", float4(1.0e-8f))))";
            break;
          case kD3DSIO_SINCOS:
            value = "float4(sin(" + readSrc(1) + "), cos(" + readSrc(1) + "), 0.0f, 0.0f)";
            break;
          case kD3DSIO_M4x4: {
            const auto base = decodeRegisterRef(instruction.operands[2], module.stage);
            if (base.kind != D3DRegisterKind::ConstFloat) {
              throw std::runtime_error("M4x4 requires a float constant matrix base");
            }
            const auto src = readSrc(1);
            value = "float4(dot(" + src + ", cFloat[" + std::to_string(base.index + 0) + "]), dot(" + src +
                    ", cFloat[" + std::to_string(base.index + 1) + "]), dot(" + src + ", cFloat[" +
                    std::to_string(base.index + 2) + "]), dot(" + src + ", cFloat[" +
                    std::to_string(base.index + 3) + "]))";
            break;
          }
          case kD3DSIO_M4x3: {
            const auto base = decodeRegisterRef(instruction.operands[2], module.stage);
            if (base.kind != D3DRegisterKind::ConstFloat) {
              throw std::runtime_error("M4x3 requires a float constant matrix base");
            }
            const auto src = readSrc(1);
            value = "float4(dot(" + src + ", cFloat[" + std::to_string(base.index + 0) + "]), dot(" + src +
                    ", cFloat[" + std::to_string(base.index + 1) + "]), dot(" + src + ", cFloat[" +
                    std::to_string(base.index + 2) + "]), 0.0f)";
            break;
          }
          case kD3DSIO_M3x4: {
            const auto base = decodeRegisterRef(instruction.operands[2], module.stage);
            if (base.kind != D3DRegisterKind::ConstFloat) {
              throw std::runtime_error("M3x4 requires a float constant matrix base");
            }
            const auto src = readSrc(1);
            value = "float4(dot((" + src + ").xyz, cFloat[" + std::to_string(base.index + 0) +
                    "].xyz), dot((" + src + ").xyz, cFloat[" + std::to_string(base.index + 1) +
                    "].xyz), dot((" + src + ").xyz, cFloat[" + std::to_string(base.index + 2) +
                    "].xyz), dot((" + src + ").xyz, cFloat[" + std::to_string(base.index + 3) + "].xyz))";
            break;
          }
          case kD3DSIO_M3x3: {
            const auto base = decodeRegisterRef(instruction.operands[2], module.stage);
            if (base.kind != D3DRegisterKind::ConstFloat) {
              throw std::runtime_error("M3x3 requires a float constant matrix base");
            }
            const auto src = readSrc(1);
            value = "float4(dot((" + src + ").xyz, cFloat[" + std::to_string(base.index + 0) +
                    "].xyz), dot((" + src + ").xyz, cFloat[" + std::to_string(base.index + 1) +
                    "].xyz), dot((" + src + ").xyz, cFloat[" + std::to_string(base.index + 2) +
                    "].xyz), 0.0f)";
            break;
          }
          case kD3DSIO_M3x2: {
            const auto base = decodeRegisterRef(instruction.operands[2], module.stage);
            if (base.kind != D3DRegisterKind::ConstFloat) {
              throw std::runtime_error("M3x2 requires a float constant matrix base");
            }
            const auto src = readSrc(1);
            value = "float4(dot((" + src + ").xyz, cFloat[" + std::to_string(base.index + 0) +
                    "].xyz), dot((" + src + ").xyz, cFloat[" + std::to_string(base.index + 1) +
                    "].xyz), 0.0f, 0.0f)";
            break;
          }
          case kD3DSIO_RCP:
            value = "float4(1.0f) / max(" + readSrc(1) + ", float4(1.0e-8f))";
            break;
          case kD3DSIO_RSQ:
            value = "rsqrt(max(" + readSrc(1) + ", float4(1.0e-8f)))";
            break;
          case kD3DSIO_FRC:
            value = "fract(" + readSrc(1) + ")";
            break;
          case kD3DSIO_LRP:
            value = "mix(" + readSrc(3) + ", " + readSrc(2) + ", " + readSrc(1) + ")";
            break;
          case kD3DSIO_DP3:
            value = "float4(dot((" + readSrc(1) + ").xyz, (" + readSrc(2) + ").xyz))";
            break;
          case kD3DSIO_DP4:
            value = "float4(dot(" + readSrc(1) + ", " + readSrc(2) + "))";
            break;
          case kD3DSIO_CND:
            value = "select(" + readSrc(3) + ", " + readSrc(2) + ", " + readSrc(1) + " > float4(0.5f))";
            break;
          case kD3DSIO_CMP:
            value = "select(" + readSrc(3) + ", " + readSrc(2) + ", " + readSrc(1) + " >= float4(0.0f))";
            break;
          case kD3DSIO_DP2ADD:
            value = "float4(dot((" + readSrc(1) + ").xy, (" + readSrc(2) + ").xy) + (" + readSrc(3) + ").x)";
            break;
          case kD3DSIO_POW:
            value = "pow(" + readSrc(1) + ", " + readSrc(2) + ")";
            break;
          case kD3DSIO_CRS:
            value = "float4(cross((" + readSrc(1) + ").xyz, (" + readSrc(2) + ").xyz), 0.0f)";
            break;
          case kD3DSIO_SGN:
            value = "sign(" + readSrc(1) + ")";
            break;
          case kD3DSIO_ABS:
            value = "abs(" + readSrc(1) + ")";
            break;
          case kD3DSIO_NRM:
            value = "float4(normalize((" + readSrc(1) + ").xyz), 0.0f)";
            break;
	      case kD3DSIO_TEX:
	            {
	              const auto sampler = textureSamplerIndex(instruction, module.stage);
	              const auto coord = readSrc(1);
	              value = "tex" + std::to_string(sampler) + ".sample(samp" + std::to_string(sampler) + ", " +
	                      sampleCoord(coord) + ")";
	            }
	            break;
          case kD3DSIO_DSX:
            value = "dfdx(" + readSrc(1) + ")";
            break;
          case kD3DSIO_DSY:
            value = "dfdy(" + readSrc(1) + ")";
            break;
	          case kD3DSIO_TEXLDD:
	            {
	              const auto sampler = textureSamplerIndex(instruction, module.stage);
	              const auto coord = readSrc(1);
	              value = "tex" + std::to_string(sampler) + ".sample(samp" + std::to_string(sampler) + ", " +
	                      sampleCoord(coord) + ")";
	            }
	            break;
	          case kD3DSIO_TEXLDL:
	            {
	              const auto sampler = textureSamplerIndex(instruction, module.stage);
	              const auto coord = readSrc(1);
	              value = "tex" + std::to_string(sampler) + ".sample(samp" + std::to_string(sampler) + ", " +
	                      sampleCoord(coord) + ", level(" + coord + ".w))";
	            }
	            break;
          default:
            throw std::runtime_error("unsupported arithmetic opcode");
        }
        if (decodeDestModifier(instruction.operands[0]) == 1u) {
          value = "clamp(" + value + ", float4(0.0f), float4(1.0f))";
        }
        switch (dst.kind) {
          case D3DRegisterKind::Temp:
            emitMaskedAssign("r[" + std::to_string(dst.index) + "]", value, dstMask);
            break;
          case D3DRegisterKind::ColorOut:
            emitMaskedAssign(pixelColorTarget(dst.index), value, dstMask);
            break;
          case D3DRegisterKind::TexCoordOut:
            emitMaskedAssign(texcoordTarget(dst.index), value, dstMask);
            break;
          case D3DRegisterKind::DepthOut:
            emitMaskedAssign("outDepth", value, dstMask, true);
            break;
          case D3DRegisterKind::ConstFloat:
            out << "  cFloat[" << dst.index << "] = " << value << ";\n";
            break;
          case D3DRegisterKind::ConstInt:
            out << "  cInt[" << dst.index << "] = int4(" << value << ");\n";
            break;
          case D3DRegisterKind::ConstBool:
            out << "  cBool[" << dst.index << "] = " << value << ".x != 0.0f ? 1u : 0u;\n";
            break;
          default:
            throw std::runtime_error("unsupported arithmetic destination");
        }
        break;
      }
      case kD3DSIO_DCL:
        break;
      case kD3DSIO_BEM:
      case kD3DSIO_TEXDEPTH:
      case kD3DSIO_TEXREG2RGB:
      case kD3DSIO_TEXDP3TEX:
      case kD3DSIO_TEXM3x2DEPTH:
      case kD3DSIO_TEXDP3:
      case kD3DSIO_TEXM3x3:
        break;
      default:
        throw std::runtime_error("unsupported D3D opcode: " + opcodeName(instruction.opcode));
    }
    }
    if (!controlStack.empty()) {
      throw std::runtime_error("unbalanced D3D control flow");
    }
    if (callDepth != 0) {
      throw std::runtime_error("unbalanced D3D CALL/RET");
    }
	  out << "  color = outColor[0];\n";
	  out << "  if (uniforms.alphaTestEnable != 0u) {\n";
  out << "    bool pass = true;\n";
  out << "    switch (uniforms.alphaTestFunc) {\n";
  out << "      case 2u: pass = color.a < uniforms.alphaRef; break;\n";
  out << "      case 3u: pass = color.a == uniforms.alphaRef; break;\n";
  out << "      case 4u: pass = color.a <= uniforms.alphaRef; break;\n";
  out << "      case 5u: pass = color.a > uniforms.alphaRef; break;\n";
  out << "      case 6u: pass = color.a != uniforms.alphaRef; break;\n";
  out << "      case 7u: pass = color.a >= uniforms.alphaRef; break;\n";
  out << "      case 8u: pass = true; break;\n";
  out << "      default: pass = true; break;\n";
  out << "    }\n";
  out << "    if (!pass) {\n";
	  out << "      discard_fragment();\n";
	  out << "    }\n";
		  out << "  }\n";
		  out << "  outColor[0] = color;\n";
		  if (usesFragmentOutStruct) {
		    out << "  FSOut result;\n";
		    for (u32 i = 0; i < colorOutputCount; ++i) {
		      out << "  result.color" << i << " = outColor[" << i << "];\n";
		    }
		    if (writesDepth) {
		      out << "  result.depth = outDepth;\n";
		    }
		    out << "  return result;\n";
		  } else {
	    out << "  return color;\n";
	  }
	  out << "}\n";
  out << "// decoded d3d hash " << module.hash << "\n";
  return out.str();
}

std::string makeTranslatedVertexSource(const ShaderRef& shader, const DrawDesc& desc) {
  return translateSpirvToMsl(translateD3DBytecodeToSpirv(shader, true, desc), desc, true);
}

std::string makeTranslatedFragmentSource(const ShaderRef& shader, const DrawDesc& desc) {
  return translateSpirvToMsl(translateD3DBytecodeToSpirv(shader, false, desc), desc, false);
}

}  // namespace dxmt9::translator::detail_

namespace dxmt9::translator {

std::string makeTranslatedVertexSource(const ::dxmt9::core::ShaderRef& shader,
                                        const ::dxmt9::core::DrawDesc& desc) {
  return detail_::makeTranslatedVertexSource(shader, desc);
}

std::string makeTranslatedFragmentSource(const ::dxmt9::core::ShaderRef& shader,
                                          const ::dxmt9::core::DrawDesc& desc) {
  return detail_::makeTranslatedFragmentSource(shader, desc);
}

namespace test {

::dxmt9::d3d9bc::SpirvModule decodeD3DBytecodeForTest(const ::dxmt9::core::ShaderRef& shader,
                                                       bool vertex,
                                                       const ::dxmt9::core::DrawDesc& desc) {
  return detail_::translateD3DBytecodeToSpirv(shader, vertex, desc);
}

::dxmt9::d3d9bc::D3DRegisterRef decodeRegisterRefForTest(std::uint32_t token,
                                                          ::dxmt9::d3d9bc::D3DShaderStage stage) {
  return detail_::decodeRegisterRef(token, stage);
}

std::array<std::uint8_t, 4> decodeSwizzleForTest(std::uint32_t token) {
  return detail_::decodeSwizzle(token);
}

std::uint32_t decodeSourceModifierForTest(std::uint32_t token) {
  return detail_::decodeSourceModifier(token);
}

std::uint32_t decodeDestModifierForTest(std::uint32_t token) {
  return detail_::decodeDestModifier(token);
}

std::uint32_t decodeWriteMaskForTest(std::uint32_t token) {
  return detail_::decodeWriteMask(token);
}

bool tokenHasRelativeAddressingForTest(std::uint32_t token) {
  return detail_::tokenHasRelativeAddressing(token);
}

}  // namespace test

}  // namespace dxmt9::translator
