#include "dxmt9_shader_translator.hpp"

#include "dxmt9_d3d9_bytecode.hpp"
#include "dxmt9_debug_trace.hpp"
#include "dxmt9_draw_shader.hpp"
#include "dxmt9_ffp_shaders.hpp"
#include "dxmt9_shader_decoder.hpp"
#include "dxmt9_shader_sources.hpp"
#include "dxmt9/assert.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// D3D9 SpirvModule → Metal Shading Language source emission. Consumes the
// decoded bytecode produced by dxmt9_shader_decoder.cpp and emits a complete
// MSL translation unit. Owns the public translator entry points
// (makeTranslatedVertexSource/makeTranslatedFragmentSource) and the
// translator::test seam.

namespace dxmt9::translator::detail_ {

using namespace ::dxmt9::core;
using namespace ::dxmt9::d3d9bc;
using namespace ::dxmt9::ffp;
using f32 = float;

using ::dxmt9::shaders::makeShaderPrelude;
using ::dxmt9::shaders::makeShaderPreludeArgbufHybrid;
using ::dxmt9::shaders::makeShaderPreludeArgbufResourceArray;
using ::dxmt9::shaders::ShaderPreludeOptions;
using ::dxmt9::shaders::kArgbufHybridBindSlot;
using ::dxmt9::shaders::kArgbufResourceArrayStageCount;

namespace {

std::string pixelPositionExpression(const std::string& pixelInputs) {
  return pixelInputs + ".position";
}

std::string formatFloatLiteral(f32 value) {
  // IEEE-754 special values (Wine fp_special_test). The default
  // `ostringstream` output emits "nan", "-inf", "inf" without a
  // C-literal suffix; appending ".0f" produces invalid MSL like
  // `nan.0f`. Emit them as the canonical math.h macros instead so
  // Metal's compiler accepts the constant unchanged.
  if (std::isnan(value)) {
    return std::string("NAN");
  }
  if (std::isinf(value)) {
    return value < 0.0f ? std::string("-INFINITY") : std::string("INFINITY");
  }
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

std::string vertexStreamName(u32 stream) {
  return "stream" + std::to_string(stream);
}

std::string vertexStreamBaseName(u32 stream) {
  return stream == 0 ? "base" : "base" + std::to_string(stream);
}

std::string vertexInputName(u32 index) {
  return "vin" + std::to_string(index);
}

void emitExtraVertexStreamParameters(std::ostringstream& out,
                                     const std::optional<VertexShaderInputLayout>& inputLayout) {
  if (!inputLayout) {
    return;
  }
  for (u32 stream = 1; stream < inputLayout->streamStrides.size(); ++stream) {
    if ((inputLayout->streamMask & (1u << stream)) == 0u) {
      continue;
    }
    out << "                     device const uchar* " << vertexStreamName(stream)
        << " [[buffer(" << vertexShaderStreamBufferSlot(stream) << ")]],\n";
  }
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
  const std::string value = "(" + expr + ")";
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
    case 5:
      return "-(" + expr + " * float4(2.0f) - float4(1.0f))";
    case 6:
      return "(float4(1.0f) - " + expr + ")";
    case 7:
      return "(" + expr + " * float4(2.0f))";
    case 8:
      return "-(" + expr + " * float4(2.0f))";
    case 9:
      return "(" + value + " / float4(" + value + ".z))";
    case 10:
      return "(" + value + " / float4(" + value + ".w))";
    case 11:
      return "abs(" + expr + ")";
    case 12:
      return "-abs(" + expr + ")";
    case 13:
      return "select(float4(1.0f), float4(0.0f), " + value + " != float4(0.0f))";
    default:
      throw std::runtime_error("unsupported D3D source modifier " + std::to_string(modifier));
  }
}

std::string applyDestModifier(std::string expr, u32 modifier) {
  if ((modifier & 0x2u) != 0u) {
    expr = "float4(half4(" + expr + "))";
  }
  if ((modifier & 0x1u) != 0u) {
    expr = "clamp(" + expr + ", float4(0.0f), float4(1.0f))";
  }
  return expr;
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

std::string texkillMaskCondition(const std::string& value, u32 mask) {
  std::ostringstream condition;
  bool first = true;
  for (u32 component = 0; component < 4; ++component) {
    if ((mask & (1u << component)) == 0u) {
      continue;
    }
    if (!first) {
      condition << " || ";
    }
    condition << "(" << value << ")." << componentName(component) << " < 0.0f";
    first = false;
  }
  return first ? std::string("false") : condition.str();
}

void emitMovaAddressAssign(std::ostream& out, const std::string& value, u32 mask) {
  if ((mask & 0xfu) == 0xfu) {
    out << "  a0 = int4(round(" << value << "));\n";
    return;
  }
  if ((mask & 0xfu) == 0u) {
    return;
  }

  out << "  {\n";
  out << "    int4 dxmt9_mova = int4(round(" << value << "));\n";
  for (u32 component = 0; component < 4; ++component) {
    if ((mask & (1u << component)) != 0u) {
      const std::string name = componentName(component);
      out << "    a0." << name << " = dxmt9_mova." << name << ";\n";
    }
  }
  out << "  }\n";
}

std::string combineBooleanTerms(const std::vector<std::string>& terms) {
  std::ostringstream out;
  bool first = true;
  for (const auto& term : terms) {
    if (term.empty()) {
      continue;
    }
    if (!first) {
      out << " && ";
    }
    out << "(" << term << ")";
    first = false;
  }
  return out.str();
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

std::string relAddrExpression(u32 relAddrToken) {
  if (relAddrToken == 0u) {
    return {};
  }
  const auto relReg = decodeRegisterRef(relAddrToken, D3DShaderStage::Vertex);
  // The loop counter aL is genuinely scalar.
  if (relReg.kind == D3DRegisterKind::Loop) {
    return "aL";
  }
  // D3D9 a0 is a 4-component address register. The rel-addr source token
  // names which component replicates as the index via its first swizzle
  // selector (bits [17:16]); matrix-palette skinning loads independent
  // bone indices into a0.x/a0.y and reads c[a0.y] / c[a0.x]. Collapsing
  // to a0.x reads the wrong bone matrix and explodes the vertex.
  return "a0." + componentName((relAddrToken >> 16) & 0x3u);
}

u32 constantRegisterMaxIndex(D3DRegisterKind kind, bool vertexStage) {
  switch (kind) {
    case D3DRegisterKind::ConstFloat:
      return vertexStage ? ::dxmt9::core::kMaxVertexConstants - 1u
                         : ::dxmt9::core::kMaxPixelConstants - 1u;
    case D3DRegisterKind::ConstInt:
      return ::dxmt9::core::kMaxIntegerConstants - 1u;
    case D3DRegisterKind::ConstBool:
      return ::dxmt9::core::kMaxBoolConstants - 1u;
    default:
      return 0u;
  }
}

std::string constantRegisterArrayName(D3DRegisterKind kind) {
  switch (kind) {
    case D3DRegisterKind::ConstFloat:
      return "cFloat";
    case D3DRegisterKind::ConstInt:
      return "cInt";
    case D3DRegisterKind::ConstBool:
      return "cBool";
    default:
      break;
  }
  throw std::runtime_error("not a constant register");
}

std::string constantDestinationTarget(const D3DRegisterRef& dst, bool vertexStage) {
  const std::string arrayName = constantRegisterArrayName(dst.kind);
  if (dst.relAddrToken == 0u) {
    return arrayName + "[" + std::to_string(dst.index) + "]";
  }
  return arrayName + "[clamp(" + relAddrExpression(dst.relAddrToken) + " + "
         + std::to_string(dst.index) + ", 0, "
         + std::to_string(constantRegisterMaxIndex(dst.kind, vertexStage)) + ")]";
}

std::string tempDestinationTarget(const D3DRegisterRef& dst, u32 maxIndex) {
  if (dst.relAddrToken == 0u) {
    return "r[" + std::to_string(dst.index) + "]";
  }
  return "r[clamp(" + relAddrExpression(dst.relAddrToken) + " + " + std::to_string(dst.index) +
         ", 0, " + std::to_string(maxIndex) + ")]";
}

std::string texcoordDestinationTarget(const D3DRegisterRef& dst) {
  if (dst.relAddrToken == 0u) {
    return "outTexcoord[" + std::to_string(std::min<u32>(dst.index, kMaxTextureStages - 1u)) + "]";
  }
  return "outTexcoord[clamp(" + relAddrExpression(dst.relAddrToken) + " + " +
         std::to_string(dst.index) + ", 0, " + std::to_string(kMaxTextureStages - 1u) + ")]";
}

void promoteIndexedConstantDestinations(ConstantUsage& usage, const SpirvModule& module, bool vertexStage) {
  for (const auto& instruction : module.instructions) {
    if (instruction.operands.empty() || !opcodeWritesFirstOperand(instruction.opcode) ||
        instruction.relAddrTokens.empty() || instruction.relAddrTokens[0] == 0u) {
      continue;
    }
    const auto dst = decodeRegisterRef(instruction.operands[0], module.stage);
    switch (dst.kind) {
      case D3DRegisterKind::ConstFloat:
        usage.mutableConstants = true;
        usage.hasFloat = true;
        usage.floatCount = constantRegisterMaxIndex(dst.kind, vertexStage) + 1u;
        break;
      case D3DRegisterKind::ConstInt:
        usage.mutableConstants = true;
        usage.hasInt = true;
        usage.intCount = constantRegisterMaxIndex(dst.kind, vertexStage) + 1u;
        break;
      case D3DRegisterKind::ConstBool:
        usage.mutableConstants = true;
        usage.hasBool = true;
        usage.boolCount = constantRegisterMaxIndex(dst.kind, vertexStage) + 1u;
        break;
      default:
        break;
    }
  }
}

D3DRegisterRef decodeOperandRegister(const D3DDecodedInstruction& instruction, size_t index,
                                     D3DShaderStage stage) {
  auto reg = decodeRegisterRef(instruction.operands[index], stage);
  if (index < instruction.relAddrTokens.size()) {
    reg.relAddrToken = instruction.relAddrTokens[index];
  }
  return reg;
}

void requireSupportedDestinationAddressing(const D3DRegisterRef& dst) {
  if (dst.relAddrToken == 0u || dst.kind == D3DRegisterKind::ConstFloat ||
      dst.kind == D3DRegisterKind::ConstInt || dst.kind == D3DRegisterKind::ConstBool ||
      dst.kind == D3DRegisterKind::Temp || dst.kind == D3DRegisterKind::TexCoordOut) {
    return;
  }
  throw std::runtime_error("destination relative addressing is only supported for temp, texcoord output, and constant registers");
}

struct VertexInputReadUsage {
  std::array<bool, 16> reads{};
  bool indexedRead = false;
};

VertexInputReadUsage collectVertexInputReadUsage(const SpirvModule& module) {
  VertexInputReadUsage usage{};
  if (module.stage != D3DShaderStage::Vertex) {
    return usage;
  }

  for (const auto& instruction : module.instructions) {
    switch (instruction.opcode) {
      case kD3DSIO_COMMENT:
      case kD3DSIO_PHASE:
      case kD3DSIO_DCL:
      case kD3DSIO_DEF:
      case kD3DSIO_DEFI:
      case kD3DSIO_DEFB:
        continue;
      default:
        break;
    }

    const size_t firstSource = opcodeWritesFirstOperand(instruction.opcode) ? 1u : 0u;
    for (size_t i = firstSource; i < instruction.operands.size(); ++i) {
      const auto reg = decodeOperandRegister(instruction, i, module.stage);
      if (reg.kind != D3DRegisterKind::Input) {
        continue;
      }
      if (reg.relAddrToken != 0u) {
        usage.indexedRead = true;
      }
      if (reg.index < usage.reads.size()) {
        usage.reads[reg.index] = true;
      }
    }
  }

  return usage;
}

std::string readOperandExpression(const D3DDecodedInstruction& instruction, const D3DRegisterRef& reg,
                                  const std::string& vertexInputs, const std::string& pixelInputs,
                                  bool vertexStage, bool vertexInputArray,
                                  const std::string& outPosition, const std::string& outColor,
                                  const std::string& outSecondaryColor, const std::string& outTexcoord,
                                  const std::string& outFogFactor, const std::string& outPointSize,
                                  const std::string& tempPrefix, const std::string& constPrefix,
                                  const std::string& intPrefix, const std::string& boolPrefix,
                                  const std::string& predicatePrefix,
                                  const VertexOutputSemantics* vertexOutputSemantics = nullptr,
                                  u32 maxTempIndex = 31u) {
  (void)instruction;
  switch (reg.kind) {
    case D3DRegisterKind::Temp:
      if (reg.relAddrToken != 0u) {
        return tempPrefix + "[clamp(" + relAddrExpression(reg.relAddrToken) + " + " +
               std::to_string(reg.index) + ", 0, " + std::to_string(maxTempIndex) + ")]";
      }
      return tempPrefix + "[" + std::to_string(reg.index) + "]";
    case D3DRegisterKind::ConstFloat:
      if (reg.relAddrToken != 0u) {
        const std::string maxIdx = vertexStage ? std::to_string(::dxmt9::core::kMaxVertexConstants - 1u)
                                               : std::to_string(::dxmt9::core::kMaxPixelConstants - 1u);
        return constPrefix + "[clamp(" + relAddrExpression(reg.relAddrToken) + " + "
               + std::to_string(reg.index) + ", 0, " + maxIdx + ")]";
      }
      return constPrefix + "[" + std::to_string(reg.index) + "]";
    case D3DRegisterKind::ConstInt:
      if (reg.relAddrToken != 0u) {
        return "float4(" + intPrefix + "[clamp(" + relAddrExpression(reg.relAddrToken) + " + "
               + std::to_string(reg.index) + ", 0, "
               + std::to_string(::dxmt9::core::kMaxIntegerConstants - 1u) + ")])";
      }
      return "float4(" + intPrefix + "[" + std::to_string(reg.index) + "])";
    case D3DRegisterKind::ConstBool:
      if (reg.relAddrToken != 0u) {
        return "(" + boolPrefix + "[clamp(" + relAddrExpression(reg.relAddrToken) + " + "
               + std::to_string(reg.index) + ", 0, "
               + std::to_string(::dxmt9::core::kMaxBoolConstants - 1u) + ")] != 0u ? float4(1.0f) : float4(0.0f))";
      }
      return "(" + boolPrefix + "[" + std::to_string(reg.index) + "] != 0u ? float4(1.0f) : float4(0.0f))";
    case D3DRegisterKind::Input:
      if (vertexStage) {
        if (vertexInputArray) {
          if (reg.relAddrToken != 0u) {
            return vertexInputs + "[clamp(" + relAddrExpression(reg.relAddrToken) + " + " +
                   std::to_string(reg.index) + ", 0, 15)]";
          }
          return vertexInputs + "[" + std::to_string(reg.index) + "]";
        }
        return vertexInputName(reg.index);
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
      if (!vertexStage && reg.index == 0) {
        return pixelPositionExpression(pixelInputs);
      }
      if (!vertexStage && reg.index == 1) {
        return "float4(frontFacing ? 1.0f : -1.0f)";
      }
      return "float4(0.0f)";
    case D3DRegisterKind::Sampler:
    case D3DRegisterKind::Unknown:
      return "float4(0.0f)";
    case D3DRegisterKind::Predicate:
      return "(" + predicatePrefix + "[" + std::to_string(reg.index) + "] ? float4(1.0f) : float4(0.0f))";
  }
  return "float4(0.0f)";
}

std::string decodeOperandToken(const u32 token, D3DShaderStage stage, bool destination) {
  D3DRegisterRef reg = decodeRegisterRef(token, stage);
  std::string name = registerName(reg, stage, destination);
  if (destination && tokenHasRelativeAddressing(token)) {
    name += "[]";
  }
  return name;
}

void emitConstantBindings(std::ostringstream& out, bool vertexStage, const ConstantUsage& usage) {
  // Constant pointers route to the per-stage category buffer (VsConsts at slot 0
  // for vertex, PsConsts at slot 0 for fragment). See specs/backend/draw-uniforms.
  const char* container = vertexStage ? "vsConsts" : "psConsts";
  const char* floatMember = vertexStage ? "vsFloatConst" : "psFloatConst";
  const char* intMember = vertexStage ? "vsIntConst" : "psIntConst";
  const char* boolMember = vertexStage ? "vsBoolConst" : "psBoolConst";

  const u32 floatCount = usage.hasIndexedFloat
                             ? constantRegisterMaxIndex(D3DRegisterKind::ConstFloat, vertexStage) + 1u
                             : usage.floatCount;
  const u32 intCount = usage.hasIndexedInt
                           ? constantRegisterMaxIndex(D3DRegisterKind::ConstInt, vertexStage) + 1u
                           : usage.intCount;
  const u32 boolCount = usage.hasIndexedBool
                            ? constantRegisterMaxIndex(D3DRegisterKind::ConstBool, vertexStage) + 1u
                            : usage.boolCount;
  const bool aliasFloat = !usage.mutableConstants;
  const bool aliasInt = !usage.mutableConstants;
  const bool aliasBool = !usage.mutableConstants;

  if (aliasFloat) {
    out << "  constant float4* cFloat = " << container << "." << floatMember << ";\n";
  } else {
    out << "  float4 cFloat[" << std::max(1u, floatCount) << "];\n";
    out << "  for (uint i = 0; i < " << floatCount << "; ++i) { cFloat[i] = "
        << container << "." << floatMember << "[i]; }\n";
  }
  if (aliasInt) {
    out << "  constant int4* cInt = " << container << "." << intMember << ";\n";
  } else {
    out << "  int4 cInt[" << std::max(1u, intCount) << "];\n";
    out << "  for (uint i = 0; i < " << intCount << "; ++i) { cInt[i] = "
        << container << "." << intMember << "[i]; }\n";
  }
  if (aliasBool) {
    out << "  constant uint* cBool = " << container << "." << boolMember << ";\n";
  } else {
    out << "  uint cBool[" << std::max(1u, boolCount) << "];\n";
    out << "  for (uint i = 0; i < " << boolCount << "; ++i) { cBool[i] = "
        << container << "." << boolMember << "[i]; }\n";
  }
}

void emitPredicateBindings(std::ostringstream& out, bool usesPredicateRegisters) {
  if (!usesPredicateRegisters) {
    return;
  }
  out << "  bool p[" << kMaxBoolConstants << "];\n";
  out << "  for (uint i = 0; i < " << kMaxBoolConstants << "; ++i) { p[i] = false; }\n";
}

std::string readPixelInputSemanticExpression(const PixelInputSemantic& semantic,
                                             const std::string& pixelInputs,
                                             u32 fallbackTexcoordIndex) {
  switch (semantic.usage) {
    case kD3DDeclUsagePosition:
    case kD3DDeclUsagePositionT:
      return pixelPositionExpression(pixelInputs);
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
  return "dxmt9_select_texcoord(" + pixelInputs + ", " +
         std::to_string(std::min<u32>(fallbackTexcoordIndex, kMaxTextureStages - 1u)) + "u)";
}

std::string readPixelInputFallbackExpression(u32 index, const std::string& pixelInputs) {
  if (index == 0) {
    return "float4(" + pixelInputs + ".color)";
  }
  if (index == 1) {
    return "float4(" + pixelInputs + ".secondaryColor)";
  }
  if (index == 3) {
    return "float4(" + pixelInputs + ".fogFactor)";
  }
  return "float4(0.0f)";
}

void markCentroidInput(ShaderPreludeOptions& options,
                       const PixelInputSemantic& semantic,
                       u32 fallbackTexcoordIndex) {
  if (!semantic.centroid) {
    return;
  }
  switch (semantic.usage) {
    case kD3DDeclUsageTexcoord:
      if (semantic.usageIndex < kMaxTextureStages) {
        options.centroidTexcoordMask |= 1u << semantic.usageIndex;
      }
      return;
    case kD3DDeclUsageColor:
      if (semantic.usageIndex == 0u) {
        options.centroidColor = true;
        return;
      }
      if (semantic.usageIndex == 1u) {
        options.centroidSecondaryColor = true;
        return;
      }
      break;
    case kD3DDeclUsageFog:
      options.centroidFogFactor = true;
      return;
    case kD3DDeclUsagePosition:
    case kD3DDeclUsagePositionT:
    case kD3DDeclUsagePSize:
      return;
    default:
      break;
  }
  if (fallbackTexcoordIndex < kMaxTextureStages) {
    options.centroidTexcoordMask |= 1u << fallbackTexcoordIndex;
  }
}

ShaderPreludeOptions makePreludeOptions(const SpirvModule& module,
                                        const ShaderSourceContext& context,
                                        bool vertex) {
  ShaderPreludeOptions options;
  options.withClipDistances = context.clipPlaneMask != 0;
  options.vsOutLayout = context.vsOutLayout;
  if (vertex) {
    return options;
  }
  const auto semantics = collectPixelInputSemantics(module);
  for (u32 index = 0; index < semantics.size(); ++index) {
    if (semantics[index].valid) {
      markCentroidInput(options, semantics[index], index);
    }
  }
  return options;
}

void markTexcoord(shaders::VSOutLayout& layout, u32 index) {
  if (index < kMaxTextureStages) {
    layout.texcoordMask |= 1u << index;
  }
}

void markPixelSemanticRead(shaders::VSOutLayout& layout,
                           const PixelInputSemantic& semantic,
                           u32 fallbackTexcoordIndex) {
  switch (semantic.usage) {
    case kD3DDeclUsagePSize:
      layout.pointSize = true;
      return;
    case kD3DDeclUsageTexcoord:
      markTexcoord(layout, semantic.usageIndex);
      return;
    case kD3DDeclUsageColor:
      if (semantic.usageIndex == 0u) {
        layout.color = true;
      } else if (semantic.usageIndex == 1u) {
        layout.secondaryColor = true;
      }
      return;
    case kD3DDeclUsageFog:
      layout.fogFactor = true;
      return;
    case kD3DDeclUsagePosition:
    case kD3DDeclUsagePositionT:
      return;
    default:
      markTexcoord(layout, fallbackTexcoordIndex);
      return;
  }
}

void markPixelInputTokenRead(shaders::VSOutLayout& layout,
                             u32 token,
                             const PixelInputSemantics& semantics) {
  const u32 type = decodeRegisterType(token);
  const u32 index = decodeRegisterIndex(token);
  switch (type) {
    case kD3DSPR_INPUT:
      if (index < semantics.size() && semantics[index].valid) {
        markPixelSemanticRead(layout, semantics[index], index);
        return;
      }
      // SM1.x and undeclared-input fallbacks used by readOperandExpression /
      // readPixelInputExpression. Mark conservatively when the historical
      // fallback maps are ambiguous.
      if (index == 0u) {
        layout.color = true;
      } else if (index == 1u) {
        markTexcoord(layout, 0u);
        layout.secondaryColor = true;
      } else if (index == 2u) {
        layout.secondaryColor = true;
      } else if (index == 3u) {
        layout.fogFactor = true;
      } else {
        markTexcoord(layout, index);
      }
      return;
    case kD3DSPR_ADDR:
      markTexcoord(layout, index);
      return;
    case kD3DSPR_RASTOUT:
      if (index == 1u) {
        layout.fogFactor = true;
      } else if (index == 2u) {
        layout.pointSize = true;
      }
      return;
    default:
      return;
  }
}

size_t sourceOperandBegin(const D3DDecodedInstruction& instruction) {
  size_t sourceBegin = opcodeWritesFirstOperand(instruction.opcode) ? 1u : 0u;
  switch (instruction.opcode) {
    case kD3DSIO_IF:
    case kD3DSIO_IFC:
    case kD3DSIO_BREAKP:
    case kD3DSIO_TEXDEPTH:
      sourceBegin = 0u;
      break;
    case kD3DSIO_LOOP:
      sourceBegin = instruction.operands.size() > 1u ? 1u : 0u;
      break;
    case kD3DSIO_REP:
      sourceBegin = 0u;
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
  return sourceBegin;
}

shaders::VSOutLayout collectFragmentVaryingLiveness(const SpirvModule& module) {
  auto layout = shaders::minimalVSOutLayout();
  // texcoord0 stays present so dxmt9_select_texcoord's fallback lane is always
  // valid even when an emitted helper is present but no shader body calls it.
  markTexcoord(layout, 0u);
  if (module.stage != D3DShaderStage::Pixel) {
    return layout;
  }

  const auto semantics = collectPixelInputSemantics(module);
  for (const auto& instruction : module.instructions) {
    const size_t begin = sourceOperandBegin(instruction);
    for (size_t i = begin; i < instruction.operands.size(); ++i) {
      markPixelInputTokenRead(layout, instruction.operands[i], semantics);
    }
    if (module.major == 1u && isTextureSampleOpcode(instruction.opcode) &&
        !instruction.operands.empty()) {
      markPixelInputTokenRead(layout, instruction.operands[0], semantics);
    }
  }

  // The translated fragment tail always emits runtime D3D9 alpha/fog handling
  // and passes in.fogFactor to dxmt9_apply_fog. Even when ffpPs.fogMode is zero
  // at runtime, Metal must type-check the field.
  layout.fogFactor = true;

  if (const char* mode = std::getenv("DXMT_DEBUG_FRAGMENT_MODE");
      mode && mode[0] != '\0') {
    if (std::strcmp(mode, "uv") == 0 ||
        std::strcmp(mode, "uv_saturate") == 0 ||
        std::strcmp(mode, "tex0_uv") == 0 ||
        std::strcmp(mode, "tex0_uv_clamp") == 0 ||
        std::strcmp(mode, "tex0_uv_flip") == 0) {
      markTexcoord(layout, 0u);
    }
  }
  return layout;
}

std::string readPixelInputExpression(u32 token,
                                     const std::string& pixelInputs,
                                     const PixelInputSemantics& semantics) {
  const u32 type = decodeRegisterType(token);
  const u32 index = decodeRegisterIndex(token);
  switch (type) {
    case kD3DSPR_INPUT:
      if (index < semantics.size() && semantics[index].valid) {
        return readPixelInputSemanticExpression(semantics[index], pixelInputs, index);
      }
      break;
    case kD3DSPR_ADDR:
      return "dxmt9_select_texcoord(" + pixelInputs + ", " + std::to_string(index) + "u)";
    case kD3DSPR_RASTOUT:
      if (index == 0) {
        return pixelPositionExpression(pixelInputs);
      }
      if (index == 1) {
        return "float4(" + pixelInputs + ".fogFactor)";
      }
      if (index == 2) {
        return "float4(" + pixelInputs + ".pointSize)";
      }
      break;
    case kD3DSPR_MISCTYPE:
      if (index == 0) {
        return pixelPositionExpression(pixelInputs);
      }
      break;
    default:
      break;
  }

  return readPixelInputFallbackExpression(index, pixelInputs);
}

TextureType samplerTextureType(const SpirvModule& module,
                               const ShaderSourceContext& context,
                               u32 sampler) {
  if (sampler >= kMaxSamplers) {
    return TextureType::TwoD;
  }
  if (module.major >= 2u) {
    return module.samplerTextureTypes[sampler];
  }
  return sampler < context.textureTypes.size() ? context.textureTypes[sampler] : TextureType::TwoD;
}

// R-BACK-12.22..12.26 (resource-array sub-mode) — fragment-stage
// eligibility. The slot-30 argbuf texture array is homogeneously
// `texture2d<float>` (the host MTLArgumentEncoder descriptor table is a
// single shape per queue), so a fragment that samples a cube or volume
// texture cannot alias `tex<stage>` off `abuf.textures[stage]` without a
// type mismatch. Such shaders fall back to the constants-only hybrid
// prelude + the direct [[texture(N)]] lane — characterized limitation;
// the main session's cube/volume readback exercises this fallback, not
// the argbuf array path. Returns true iff every USED fragment sampler is
// a 2D (or 2D-array) texture.
bool pixelResourceArrayEligible(const SpirvModule& module,
                                const ShaderSourceContext& context,
                                const std::array<bool, kMaxSamplers>& samplerUsage) {
  for (u32 stage = 0; stage < kMaxSamplers; ++stage) {
    if (!samplerUsage[stage]) {
      continue;
    }
    if (stage >= shaders::kArgbufResourceArrayStageCount) {
      // Only s0..s7 ride the argbuf array; a higher stage keeps the shader
      // on the direct lane.
      return false;
    }
    const auto type = samplerTextureType(module, context, stage);
    if (type != TextureType::TwoD && type != TextureType::Array2D) {
      return false;
    }
  }
  return true;
}

std::string textureTypeName(TextureType type) {
  switch (type) {
    case TextureType::Cube:
      return "texturecube<float>";
    case TextureType::Volume:
      return "texture3d<float>";
    case TextureType::TwoD:
    case TextureType::Array2D:
    default:
      return "texture2d<float>";
  }
}

std::string sampleCoordExpression(TextureType type, const std::string& coord, bool flip2D) {
  switch (type) {
    case TextureType::Cube:
    case TextureType::Volume:
      return "(" + coord + ").xyz";
    case TextureType::TwoD:
    case TextureType::Array2D:
    default:
      if (flip2D) {
        return "float2((" + coord + ").x, 1.0f - (" + coord + ").y)";
      }
      return "(" + coord + ").xy";
  }
}

std::string textureGradientExpression(TextureType type,
                                      const std::string& ddx,
                                      const std::string& ddy) {
  switch (type) {
    case TextureType::Cube:
      return "gradientcube((" + ddx + ").xyz, (" + ddy + ").xyz)";
    case TextureType::Volume:
      return "gradient3d((" + ddx + ").xyz, (" + ddy + ").xyz)";
    case TextureType::TwoD:
    case TextureType::Array2D:
    default:
      return "gradient2d((" + ddx + ").xy, (" + ddy + ").xy)";
  }
}

void emitFragmentTextureArguments(std::ostringstream& out,
                                  const std::array<bool, kMaxSamplers>& samplerUsage,
                                  const SpirvModule& module,
                                  const ShaderSourceContext& context) {
  bool first = true;
  for (u32 stage = 0; stage < kMaxSamplers; ++stage) {
    if (!samplerUsage[stage]) {
      continue;
    }
    if (!first) {
      out << ", ";
    }
    first = false;
    out << textureTypeName(samplerTextureType(module, context, stage)) << " tex" << stage
        << " [[texture(" << stage << ")]], "
        << "sampler samp" << stage << " [[sampler(" << stage << ")]]";
  }
}

std::array<bool, kMaxVertexTextureSamplers> collectVertexSamplerUsage(const SpirvModule& module) {
  std::array<bool, kMaxVertexTextureSamplers> usage{};
  if (module.stage != D3DShaderStage::Vertex) {
    return usage;
  }
  for (const auto& instruction : module.instructions) {
    if (!isTextureSampleOpcode(instruction.opcode)) {
      continue;
    }
    const u32 sampler = textureSamplerIndex(instruction, module.stage);
    if (sampler < usage.size()) {
      usage[sampler] = true;
    }
  }
  return usage;
}

void emitVertexTextureArguments(std::ostringstream& out,
                                const std::array<bool, kMaxVertexTextureSamplers>& samplerUsage,
                                const SpirvModule& module,
                                const ShaderSourceContext& context) {
  for (u32 stage = 0; stage < kMaxVertexTextureSamplers; ++stage) {
    if (!samplerUsage[stage]) {
      continue;
    }
    out << "                     " << textureTypeName(samplerTextureType(module, context, stage))
        << " tex" << stage << " [[texture(" << stage << ")]], "
        << "sampler samp" << stage << " [[sampler(" << stage << ")]],\n";
  }
}

bool pixelUsesVFaceInput(const SpirvModule& module) {
  if (module.stage != D3DShaderStage::Pixel) {
    return false;
  }
  for (const auto& instruction : module.instructions) {
    for (size_t i = 0; i < instruction.operands.size(); ++i) {
      if (instruction.opcode == kD3DSIO_DCL && i == 0) {
        continue;
      }
      if ((instruction.opcode == kD3DSIO_DEF ||
           instruction.opcode == kD3DSIO_DEFI ||
           instruction.opcode == kD3DSIO_DEFB) &&
          i > 0) {
        continue;
      }
      if ((instruction.opcode == kD3DSIO_LABEL ||
           instruction.opcode == kD3DSIO_CALL ||
           instruction.opcode == kD3DSIO_CALLNZ) &&
          i == 0) {
        continue;
      }
      const auto reg = decodeRegisterRef(instruction.operands[i], module.stage);
      if (reg.kind == D3DRegisterKind::MiscType && reg.index == 1) {
        return true;
      }
    }
  }
  return false;
}

std::string translateSpirvToMsl(const SpirvModule& module,
                                const ShaderSourceContext& context,
                                bool vertex) {
  std::ostringstream out;
  const bool argbufHybrid = context.argbufHybridMode;
  // R-BACK-12.22..12.26 (resource-array sub-mode) — decide the fragment
  // resource-array eligibility BEFORE emitting the prelude (the extended
  // ArgbufLayout with texture/sampler arrays must replace the 4-pointer
  // form). Only the fragment stage rides the argbuf texture array; the
  // vertex stage keeps its direct vertex-texture lane. Cube/volume / s8+
  // shaders fall back to the constants-only hybrid prelude (see
  // pixelResourceArrayEligible).
  const bool fragmentArgbufResourceArray = [&] {
    if (vertex || !argbufHybrid || !context.argbufResourceArray) {
      return false;
    }
    const auto usage = collectPixelSamplerUsage(module, context);
    const bool anyUsed =
        std::any_of(usage.begin(), usage.end(), [](bool u) { return u; });
    return anyUsed && pixelResourceArrayEligible(module, context, usage);
  }();
  const auto preludeOptions = makePreludeOptions(module, context, vertex);
  if (fragmentArgbufResourceArray) {
    out << makeShaderPreludeArgbufResourceArray(preludeOptions);
  } else if (argbufHybrid) {
    out << makeShaderPreludeArgbufHybrid(preludeOptions);
  } else {
    out << makeShaderPrelude(preludeOptions);
  }
  if (vertex) {
    const auto inputLayout = decodeVertexShaderInputLayout(module, context);
    const auto outputSemantics = collectVertexOutputSemantics(module);
    const auto vertexSamplerUsage = collectVertexSamplerUsage(module);
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
          trace << " v" << i << "->s" << binding.stream
                << "/off" << binding.offset << "/type" << binding.type
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
    if (argbufHybrid) {
      // R-BACK-12.22..12.26 MSL routing — single argbuf at slot 30
      // replaces slots 0/3. Vertex stream and DrawVolatile stay direct
      // (design.md §11.4). Re-alias `vsConsts`/`ffpVs` references off
      // the argbuf so downstream emission continues to read by name.
      out << "vertex VSOut dxmt9_vs(uint vid [[vertex_id]],\n";
      out << "                     constant ArgbufLayout& abuf [[buffer("
          << kArgbufHybridBindSlot << ")]],\n";
      out << "                     device const uchar* stream0 [[buffer(1)]],\n";
      emitExtraVertexStreamParameters(out, inputLayout);
      emitVertexTextureArguments(out, vertexSamplerUsage, module, context);
      out << "                     constant DrawVolatile& drawVolatile [[buffer(5)]]) {\n";
      out << "  constant VsConsts& vsConsts = *abuf.vsConsts;\n";
      out << "  constant FfpVsConsts& ffpVs = *abuf.ffpVs;\n";
    } else {
      out << "vertex VSOut dxmt9_vs(uint vid [[vertex_id]],\n";
      out << "                     constant VsConsts& vsConsts [[buffer(0)]],\n";
      out << "                     device const uchar* stream0 [[buffer(1)]],\n";
      emitExtraVertexStreamParameters(out, inputLayout);
      out << "                     constant FfpVsConsts& ffpVs [[buffer(3)]],\n";
      emitVertexTextureArguments(out, vertexSamplerUsage, module, context);
      out << "                     constant DrawVolatile& drawVolatile [[buffer(5)]]) {\n";
    }
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
      if (shaders::vsoutEmitColor(context.vsOutLayout)) {
        out << "  out.color = outColor;\n";
      }
      if (shaders::vsoutEmitSecondaryColor(context.vsOutLayout)) {
        out << "  out.secondaryColor = outSecondaryColor;\n";
      }
      for (size_t i = 0; i < kMaxTextureStages; ++i) {
        if (!shaders::vsoutEmitTexcoord(context.vsOutLayout, i)) continue;
        out << "  out.texcoord" << i << " = outTexcoord[" << i << "];\n";
      }
      if (shaders::vsoutEmitFogFactor(context.vsOutLayout)) {
        out << "  out.fogFactor = outFogFactor;\n";
      }
      if (shaders::vsoutEmitPointSize(context.vsOutLayout)) {
        out << "  out.pointSize = outPointSize;\n";
      }
      if (context.clipPlaneMask != 0) {
        // Translated D3D-bytecode VS path that did not write any clip
        // distance: seed the Apple-only `[[clip_distance]]` slot with +1
        // so disabled planes don't accidentally discard fragments.
        out << "  out.clipDistance0 = 1.0f;\n";
      }
      out << "  return out;\n";
      out << "}\n";
      out << "// decoded d3d hash " << module.hash << "\n";
      return out.str();
    }
    const auto inputReadUsage = collectVertexInputReadUsage(module);
    const bool useVertexInputArray = inputReadUsage.indexedRead;
    bool needsVertexFetch = false;
    if (inputLayout) {
      for (size_t i = 0; i < inputLayout->inputs.size(); ++i) {
        if ((useVertexInputArray || inputReadUsage.reads[i]) && inputLayout->inputs[i].valid) {
          needsVertexFetch = true;
          break;
        }
      }
    }
    auto emitVertexInputLoad = [&](const VertexInputBinding& binding, const std::string& target) {
      const std::string streamName = vertexStreamName(binding.stream);
      const std::string baseName = vertexStreamBaseName(binding.stream);
      switch (binding.type) {
        case kD3DDeclTypeFloat1:
          out << "  " << target << " = float4(dxmt9_load_f32(" << streamName << ", " << baseName << " + " << binding.offset
              << "u), 0.0f, 0.0f, 1.0f);\n";
          break;
        case kD3DDeclTypeFloat2:
          out << "  " << target << " = float4(dxmt9_load_f32x2(" << streamName << ", " << baseName << " + " << binding.offset
              << "u), 0.0f, 1.0f);\n";
          break;
        case kD3DDeclTypeFloat3:
          out << "  " << target << " = float4(dxmt9_load_f32x3(" << streamName << ", " << baseName << " + " << binding.offset
              << "u), 1.0f);\n";
          break;
        case kD3DDeclTypeFloat4:
          out << "  " << target << " = dxmt9_load_f32x4(" << streamName << ", " << baseName << " + " << binding.offset << "u);\n";
          break;
        case kD3DDeclTypeD3DColor:
          out << "  " << target << " = dxmt9_load_d3dcolor(" << streamName << ", " << baseName << " + " << binding.offset << "u);\n";
          break;
        case kD3DDeclTypeUByte4:
          out << "  " << target << " = dxmt9_load_u8x4(" << streamName << ", " << baseName << " + " << binding.offset << "u);\n";
          break;
        case kD3DDeclTypeShort2:
          out << "  " << target << " = float4(dxmt9_load_i16x2(" << streamName << ", " << baseName << " + " << binding.offset
              << "u), 0.0f, 1.0f);\n";
          break;
        case kD3DDeclTypeShort4:
          out << "  " << target << " = dxmt9_load_i16x4(" << streamName << ", " << baseName << " + " << binding.offset << "u);\n";
          break;
        case kD3DDeclTypeUByte4N:
          out << "  " << target << " = dxmt9_load_u8x4_unorm(" << streamName << ", " << baseName << " + " << binding.offset << "u);\n";
          break;
        case kD3DDeclTypeShort2N:
          out << "  " << target << " = float4(dxmt9_load_i16x2_snorm(" << streamName << ", " << baseName << " + " << binding.offset
              << "u), 0.0f, 1.0f);\n";
          break;
        case kD3DDeclTypeShort4N:
          out << "  " << target << " = dxmt9_load_i16x4_snorm(" << streamName << ", " << baseName << " + " << binding.offset << "u);\n";
          break;
        case kD3DDeclTypeUShort2N:
          out << "  " << target << " = float4(dxmt9_load_u16x2_unorm(" << streamName << ", " << baseName << " + " << binding.offset
              << "u), 0.0f, 1.0f);\n";
          break;
        case kD3DDeclTypeUShort4N:
          out << "  " << target << " = dxmt9_load_u16x4_unorm(" << streamName << ", " << baseName << " + " << binding.offset << "u);\n";
          break;
        case kD3DDeclTypeUDec3:
          out << "  " << target << " = dxmt9_load_udec3(" << streamName << ", " << baseName << " + " << binding.offset << "u);\n";
          break;
        case kD3DDeclTypeDec3N:
          out << "  " << target << " = dxmt9_load_dec3n(" << streamName << ", " << baseName << " + " << binding.offset << "u);\n";
          break;
        case kD3DDeclTypeFloat16_2:
          out << "  " << target << " = float4(dxmt9_load_f16x2(" << streamName << ", " << baseName << " + " << binding.offset
              << "u), 0.0f, 1.0f);\n";
          break;
        case kD3DDeclTypeFloat16_4:
          out << "  " << target << " = dxmt9_load_f16x4(" << streamName << ", " << baseName << " + " << binding.offset << "u);\n";
          break;
        default:
          break;
      }
    };

    if (useVertexInputArray) {
      out << "  float4 vin[16];\n";
      out << "  for (uint i = 0; i < 16u; ++i) { vin[i] = float4(0.0f); }\n";
      out << "  vin[0] = float4(dxmt9_positions[vid % 3], 0.0f, 1.0f);\n";
    } else {
      for (size_t i = 0; i < inputReadUsage.reads.size(); ++i) {
        if (!inputReadUsage.reads[i]) {
          continue;
        }
        const auto binding = inputLayout ? inputLayout->inputs[i] : VertexInputBinding{};
        if (binding.valid) {
          out << "  float4 " << vertexInputName(static_cast<u32>(i)) << ";\n";
        } else if (i == 0) {
          out << "  float4 " << vertexInputName(static_cast<u32>(i))
              << " = float4(dxmt9_positions[vid % 3], 0.0f, 1.0f);\n";
        } else {
          out << "  float4 " << vertexInputName(static_cast<u32>(i)) << " = float4(0.0f);\n";
        }
      }
    }
    if (inputLayout && needsVertexFetch) {
      out << "  const uint stride = drawVolatile.vertexStreamStride != 0u ? drawVolatile.vertexStreamStride : "
          << inputLayout->stride << "u;\n";
      out << "  const int vertexIndex = max(0, int(vid) + drawVolatile.vertexBaseIndex);\n";
      out << "  const uint base = drawVolatile.vertexStreamOffset + uint(vertexIndex) * stride;\n";
      for (u32 stream = 1; stream < inputLayout->streamStrides.size(); ++stream) {
        if ((inputLayout->streamMask & (1u << stream)) == 0u) {
          continue;
        }
        out << "  const uint stride" << stream << " = " << inputLayout->streamStrides[stream] << "u;\n";
        out << "  const uint base" << stream << " = uint(vertexIndex) * stride" << stream << ";\n";
      }
      for (size_t i = 0; i < inputLayout->inputs.size(); ++i) {
        const auto& binding = inputLayout->inputs[i];
        if (!binding.valid || (!useVertexInputArray && !inputReadUsage.reads[i])) {
          continue;
        }
        const std::string target =
            useVertexInputArray ? ("vin[" + std::to_string(i) + "]")
                                : vertexInputName(static_cast<u32>(i));
        emitVertexInputLoad(binding, target);
      }
    }
	    out << "  int4 a0 = int4(0);\n";
	    out << "  int aL = 0;\n";
	    // R-SHADER-AIR-SIZE: VS path keeps the full 32-temp array. FS
	    // sizing alone gives us the bulk of the GPU-register-pressure
	    // win (per-fragment thread runs in parallel × hundreds of
	    // thousands of pixels — each saved alloca matters), while VS
	    // is one thread per vertex and the analysis miss surface
	    // (subroutine bodies, indexed reads in CALL targets, control-
	    // flow joins) is wider. Visual regressions on SFIV stage-
	    // transition overlays (pink gradient on the loading screen)
	    // when both VS and FS were sized suggest the VS scan misses
	    // a corner case; keep VS conservative until that's audited.
	    out << "  float4 r[32];\n";
	    out << "  for (uint i = 0; i < 32u; ++i) { r[i] = float4(0.0f); }\n";
      auto constantUsage = collectConstantUsage(module);
      promoteIndexedConstantDestinations(constantUsage, module, true);
	    emitConstantBindings(out, true, constantUsage);
    emitPredicateBindings(out, shaderUsesPredicateRegisters(module));
    std::vector<FlowBlock> controlStack;
    std::vector<bool> callConditionalStack;
    std::vector<std::string> callReturnStack;
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
        } else if ((instruction.opcode == kD3DSIO_LABEL || instruction.opcode == kD3DSIO_CALL
                    || instruction.opcode == kD3DSIO_CALLNZ) && i == 0) {
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
        auto reg = decodeOperandRegister(instruction, index, module.stage);
        std::string expr = readOperandExpression(instruction, reg, "vin", "in", true,
                                                 useVertexInputArray,
                                                 "outPosition", "outColor", "outSecondaryColor", "outTexcoord",
                                                 "outFogFactor", "outPointSize", "r", "cFloat", "cInt", "cBool",
                                                 "p", &outputSemantics);
        expr = applySwizzle(expr, decodeSwizzle(token));
        expr = applySourceModifier(std::move(expr), decodeSourceModifier(token));
        return expr;
      };

	    auto emitMaskedAssign = [&](const std::string& target, const std::string& value, u32 mask, bool scalar = false) {
        const std::string finalValue =
            applyDestModifier(value, decodeDestModifier(instruction.operands.empty() ? 0u : instruction.operands[0]));
	      if (scalar) {
	        out << "  " << target << " = " << finalValue << ".x;\n";
	        return;
	      }
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
      auto sampleCoord = [&](u32 sampler, const std::string& coord) {
        return sampleCoordExpression(samplerTextureType(module, context, sampler), coord, false);
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

      auto currentGuard = [&](const D3DDecodedInstruction& guardedInstruction) {
        std::vector<std::string> terms;
        if (!callReturnStack.empty()) {
          terms.push_back("!" + callReturnStack.back());
        }
        if (guardedInstruction.predicated) {
          terms.push_back("p[0]");
        }
        return combineBooleanTerms(terms);
      };
      auto guardedCondition = [&](const D3DDecodedInstruction& guardedInstruction,
                                  const std::string& condition) {
        const auto guard = currentGuard(guardedInstruction);
        if (guard.empty()) {
          return condition;
        }
        std::vector<std::string> terms;
        terms.push_back(guard);
        terms.push_back(condition);
        return combineBooleanTerms(terms);
      };
      auto emitInstructionGuardOpen = [&] {
        const auto guard = currentGuard(instruction);
        if (guard.empty()) {
          return false;
        }
        out << "  if (" << guard << ") {\n";
        return true;
      };

      if (instruction.opcode == kDXMT9_INTERNAL_CALL_BEGIN) {
        const auto flag = "dxmt9_call_ret_" + std::to_string(instruction.controls);
        out << "  bool " << flag << " = false;\n";
        callReturnStack.push_back(flag);
        continue;
      }
      if (instruction.opcode == kDXMT9_INTERNAL_CALL_END) {
        if (callReturnStack.empty()) {
          throw std::runtime_error("internal CALL_END without CALL_BEGIN");
        }
        callReturnStack.pop_back();
        continue;
      }
      if (instruction.opcode == kDXMT9_INTERNAL_CALL_RET) {
        if (callReturnStack.empty()) {
          out << "  return out;\n";
        } else {
          out << "  " << callReturnStack.back() << " = true;\n";
        }
        continue;
      }
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
        callConditionalStack.push_back(false);
        continue;
      }
      if (instruction.opcode == kD3DSIO_CALLNZ) {
        if (instruction.operands.size() < 2) {
          throw std::runtime_error("CALLNZ requires a label operand and condition source");
        }
        out << "  // callnz label " << decodeLabelIndex(instruction.operands[0]) << "\n";
        out << "  if ((" << readSrc(1) << ").x != 0.0f) {\n";
        out << "  do {\n";
        callConditionalStack.push_back(true);
        continue;
      }
      if (instruction.opcode == kD3DSIO_RET) {
        if (!callConditionalStack.empty()) {
          const bool conditionalCall = callConditionalStack.back();
          callConditionalStack.pop_back();
          out << "  break;\n";
          out << "  } while (false);\n";
          if (conditionalCall) {
            out << "  }\n";
          }
        } else {
          out << "  return out;\n";
        }
        continue;
      }
      if (instruction.opcode == kD3DSIO_IF) {
        if (instruction.operands.empty()) {
          throw std::runtime_error("IF requires a condition operand");
        }
        const auto guard = currentGuard(instruction);
        out << "  if (" << guardedCondition(instruction, "(" + readSrc(0) + ").x != 0.0f") << ") {\n";
        controlStack.push_back(FlowBlock{instruction.opcode, false, guard});
        continue;
      }
      if (instruction.opcode == kD3DSIO_IFC) {
        if (instruction.operands.size() < 2) {
          throw std::runtime_error("IFC requires two source operands");
        }
        // D3DSPC_* comparison code lives in the low 4 bits of
        // `instruction.controls` (extracted from token bits 16..23 by
        // the decoder). Reuse the IF FlowBlock type so the matching
        // ELSE/ENDIF in the bytecode continues to pop correctly.
        const char* op = "==";
        switch (instruction.controls & 0xfu) {
          case 1: op = ">";  break;  // D3DSPC_GT
          case 2: op = "=="; break;  // D3DSPC_EQ
          case 3: op = ">="; break;  // D3DSPC_GE
          case 4: op = "<";  break;  // D3DSPC_LT
          case 5: op = "!="; break;  // D3DSPC_NE
          case 6: op = "<="; break;  // D3DSPC_LE
          default: op = "=="; break; // reserved (0) — treat as EQ
        }
        const auto guard = currentGuard(instruction);
        out << "  if (" << guardedCondition(instruction, "(" + readSrc(0) + ").x " + op + " (" + readSrc(1) + ").x") << ") {\n";
        controlStack.push_back(FlowBlock{kD3DSIO_IF, false, guard});
        continue;
      }
      if (instruction.opcode == kD3DSIO_ELSE) {
        if (controlStack.empty() || controlStack.back().opcode != kD3DSIO_IF || controlStack.back().sawElse) {
          throw std::runtime_error("ELSE without matching IF");
        }
        controlStack.back().sawElse = true;
        if (controlStack.back().guard.empty()) {
          out << "  } else {\n";
        } else {
          out << "  } else if (" << controlStack.back().guard << ") {\n";
        }
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
        const auto countSource = instruction.opcode == kD3DSIO_LOOP && instruction.operands.size() > 1
                                     ? readSrc(1)
                                     : readSrc(0);
        const auto countExpr = "max(0, int(round(" + countSource + ".x)))";
        const auto guard = currentGuard(instruction);
        const auto loopGuard = guard.empty() ? std::string{} : guard + " && ";
        if (instruction.opcode == kD3DSIO_LOOP) {
          out << "  for (int dxmt9_loop_" << loopIndex << " = 0, dxmt9_loopCount_" << loopIndex << " = "
              << countExpr << "; " << loopGuard << "dxmt9_loop_" << loopIndex << " < dxmt9_loopCount_" << loopIndex
              << "; ++dxmt9_loop_" << loopIndex << ") {\n";
        } else {
          out << "  for (int dxmt9_rep_" << loopIndex << " = 0, dxmt9_repCount_" << loopIndex << " = " << countExpr
              << "; " << loopGuard << "dxmt9_rep_" << loopIndex << " < dxmt9_repCount_" << loopIndex << "; ++dxmt9_rep_"
              << loopIndex << ") {\n";
        }
        controlStack.push_back(FlowBlock{instruction.opcode, false, guard});
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
        const auto guard = currentGuard(instruction);
        if (guard.empty()) {
          out << "  break;\n";
        } else {
          out << "  if (" << guard << ") { break; }\n";
        }
        continue;
      }
      if (instruction.opcode == kD3DSIO_BREAKP) {
        if (instruction.operands.empty()) {
          throw std::runtime_error("BREAKP requires a predicate operand");
        }
        out << "  if (" << guardedCondition(instruction, "(" + readSrc(0) + ").x != 0.0f") << ") { break; }\n";
        continue;
      }
      if (instruction.opcode == kD3DSIO_BREAKC) {
        if (instruction.operands.size() < 2) {
          throw std::runtime_error("BREAKC requires two source operands");
        }
        const char* op = "==";
        switch (instruction.controls & 0xfu) {
          case 1: op = ">";  break;  // D3DSPC_GT
          case 2: op = "=="; break;  // D3DSPC_EQ
          case 3: op = ">="; break;  // D3DSPC_GE
          case 4: op = "<";  break;  // D3DSPC_LT
          case 5: op = "!="; break;  // D3DSPC_NE
          case 6: op = "<="; break;  // D3DSPC_LE
          default: op = "=="; break;
        }
        out << "  if (" << guardedCondition(instruction, "(" + readSrc(0) + ").x " + op + " (" + readSrc(1) + ").x") << ") { break; }\n";
        continue;
      }

      const bool predicatedBody = emitInstructionGuardOpen();
      switch (instruction.opcode) {
        case kD3DSIO_NOP:
          break;
        case kD3DSIO_MOV: {
        if (instruction.operands.size() < 2) {
          throw std::runtime_error("MOV requires 2 operands");
        }
        const auto dst = decodeOperandRegister(instruction, 0, module.stage);
        requireSupportedDestinationAddressing(dst);
          const auto dstMask = decodeWriteMask(instruction.operands[0]);
          const auto value = readSrc(1);
          switch (dst.kind) {
            case D3DRegisterKind::Temp:
              emitMaskedAssign(tempDestinationTarget(dst, 31u), value, dstMask);
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
              if (dst.relAddrToken != 0u) {
                emitMaskedAssign(texcoordDestinationTarget(dst), value, dstMask);
              } else if (!emitVertexOutputAssign(dst, value, dstMask)) {
                emitMaskedAssign(texcoordDestinationTarget(dst), value, dstMask);
              }
              break;
            case D3DRegisterKind::ColorOut:
              emitMaskedAssign(pixelColorTarget(dst.index), value, dstMask);
              break;
            case D3DRegisterKind::DepthOut:
              throw std::runtime_error("vertex depth output register is invalid");
            case D3DRegisterKind::ConstFloat:
              out << "  " << constantDestinationTarget(dst, true) << " = " << value << ";\n";
              break;
            case D3DRegisterKind::ConstInt:
              out << "  " << constantDestinationTarget(dst, true) << " = int4(" << value << ");\n";
              break;
            case D3DRegisterKind::ConstBool:
              out << "  " << constantDestinationTarget(dst, true) << " = "
                  << value << ".x != 0.0f ? 1u : 0u;\n";
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
        const auto dst = decodeOperandRegister(instruction, 0, module.stage);
        requireSupportedDestinationAddressing(dst);
        const auto dstMask = decodeWriteMask(instruction.operands[0]);
        const auto value = readSrc(1);
        switch (dst.kind) {
          case D3DRegisterKind::Address:
            // a0 is a 4-component address register; MOVA rounds each
            // component so later relative addressing can index by any of
            // a0.x/.y/.z/.w (matrix-palette skinning uses a0.x and a0.y).
            emitMovaAddressAssign(out, value, dstMask);
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
        const auto dst = decodeOperandRegister(instruction, 0, module.stage);
        requireSupportedDestinationAddressing(dst);
        if (dst.kind != D3DRegisterKind::Predicate) {
          throw std::runtime_error("SETP requires a predicate register destination");
        }
        out << "  p[" << dst.index << "] = (" << readSrc(1) << ").x != 0.0f;\n";
        break;
      }
      case kD3DSIO_TEXKILL:
        throw std::runtime_error("TEXKILL is only valid in pixel shaders");
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
      case kD3DSIO_LIT:
      case kD3DSIO_DST:
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
          const auto dst = decodeOperandRegister(instruction, 0, module.stage);
          requireSupportedDestinationAddressing(dst);
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
              value = "float4(log2(abs(" + readSrc(1) + ")))";
              break;
            case kD3DSIO_LIT: {
              // D3D9 LIT: dst = (1, max(src.x, 0),
              //                  src.x > 0 ? pow(max(src.y, 0), clamp(src.w, -128, 128)) : 0,
              //                  1). Used by fixed-function-style lighting helpers.
              const auto src = readSrc(1);
              value = std::string("([&](){ float4 lit_src = ") + src + ";"
                      " float lit_x = lit_src.x;"
                      " float lit_y = lit_src.y;"
                      " float lit_w = clamp(lit_src.w, -128.0f, 128.0f);"
                      " float lit_z = (lit_x > 0.0f) ? pow(max(lit_y, 0.0f), lit_w) : 0.0f;"
                      " return float4(1.0f, max(lit_x, 0.0f), lit_z, 1.0f); }())";
              break;
            }
            case kD3DSIO_DST: {
              // D3D9 DST (Distance Vector): dst = (1, src0.y * src1.y,
              //                                    src0.z, src1.w).
              // Mate of fixed-function attenuation helpers (DST and LIT
              // typically appear together to build distance-falloff
              // lighting). Source swizzles are honored by readSrc(); the
              // write mask is applied downstream by the existing dstMask
              // assignment site.
              value = std::string("float4(1.0f, (") + readSrc(1) + ").y * (" + readSrc(2) + ").y, (" +
                      readSrc(1) + ").z, (" + readSrc(2) + ").w)";
              break;
            }
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
              if (instruction.coissue && module.stage == D3DShaderStage::Pixel &&
                  module.major == 1u && module.minor < 4u) {
                value = readSrc(2);
              } else {
                value = "select(" + readSrc(3) + ", " + readSrc(2) + ", " + readSrc(1) + " > float4(0.5f))";
              }
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
              throw std::runtime_error("TEXLDD is invalid in vertex shaders");
            case kD3DSIO_TEXLDL:
              {
                const auto sampler = textureSamplerIndex(instruction, module.stage);
                if (sampler >= kMaxVertexTextureSamplers) {
                  throw std::runtime_error("vertex texture sampler index out of range");
                }
                const auto coord = readSrc(1);
                value = "tex" + std::to_string(sampler) + ".sample(samp" +
                        std::to_string(sampler) + ", " + sampleCoord(sampler, coord) +
                        ", level(" + coord + ".w))";
              }
              break;
            case kD3DSIO_TEX:
              throw std::runtime_error("TEX requires pixel-shader implicit gradients");
            default:
              throw std::runtime_error("unsupported arithmetic opcode");
          }
          switch (dst.kind) {
            case D3DRegisterKind::Temp:
              emitMaskedAssign(tempDestinationTarget(dst, 31u), value, dstMask);
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
              if (dst.relAddrToken != 0u) {
                emitMaskedAssign(texcoordDestinationTarget(dst), value, dstMask);
              } else if (!emitVertexOutputAssign(dst, value, dstMask)) {
                emitMaskedAssign(texcoordDestinationTarget(dst), value, dstMask);
              }
              break;
            case D3DRegisterKind::ColorOut:
              emitMaskedAssign(pixelColorTarget(dst.index), value, dstMask);
              break;
            case D3DRegisterKind::DepthOut:
              throw std::runtime_error("vertex depth output register is invalid");
            default:
              throw std::runtime_error("unsupported arithmetic destination");
          }
          break;
        }
        case kD3DSIO_DEF: {
          if (instruction.operands.size() < 5) {
            throw std::runtime_error("DEF requires 5 operands");
          }
          const auto dst = decodeOperandRegister(instruction, 0, module.stage);
          requireSupportedDestinationAddressing(dst);
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
          out << "  " << constantDestinationTarget(dst, true) << " = " << formatFloatVec4(values) << ";\n";
          break;
        }
        case kD3DSIO_DEFI: {
          if (instruction.operands.size() < 5) {
            throw std::runtime_error("DEFI requires 5 operands");
          }
          const auto dst = decodeOperandRegister(instruction, 0, module.stage);
          requireSupportedDestinationAddressing(dst);
          const auto values = std::array<i32, 4>{static_cast<i32>(instruction.operands[1]),
                                                 static_cast<i32>(instruction.operands[2]),
                                                 static_cast<i32>(instruction.operands[3]),
                                                 static_cast<i32>(instruction.operands[4])};
          if (dst.kind != D3DRegisterKind::ConstInt) {
            throw std::runtime_error("DEFI requires an integer constant destination");
          }
          out << "  " << constantDestinationTarget(dst, true) << " = " << formatIntVec4(values) << ";\n";
          break;
        }
        case kD3DSIO_DEFB: {
          if (instruction.operands.size() < 2) {
            throw std::runtime_error("DEFB requires 2 operands");
          }
          const auto dst = decodeOperandRegister(instruction, 0, module.stage);
          requireSupportedDestinationAddressing(dst);
          if (dst.kind != D3DRegisterKind::ConstBool) {
            throw std::runtime_error("DEFB requires a boolean constant destination");
          }
          out << "  " << constantDestinationTarget(dst, true) << " = "
              << (instruction.operands[1] != 0u ? "1u" : "0u") << ";\n";
          break;
        }
        case kD3DSIO_DCL:
          // No-op for now: DCL informs semantics, but the current translator maps outputs by register class.
          break;
        case kD3DSIO_TEXCOORD:
        case kD3DSIO_TEXBEM:
        case kD3DSIO_TEXBEML:
        case kD3DSIO_TEXREG2AR:
        case kD3DSIO_TEXREG2GB:
        case kD3DSIO_TEXM3x2PAD:
        case kD3DSIO_TEXM3x2TEX:
        case kD3DSIO_TEXM3x3PAD:
        case kD3DSIO_TEXM3x3TEX:
        case kD3DSIO_TEXM3x3DIFF:
        case kD3DSIO_TEXM3x3SPEC:
        case kD3DSIO_TEXM3x3VSPEC:
        case kD3DSIO_BEM:
        case kD3DSIO_TEXDEPTH:
        case kD3DSIO_TEXREG2RGB:
        case kD3DSIO_TEXDP3TEX:
        case kD3DSIO_TEXM3x2DEPTH:
        case kD3DSIO_TEXDP3:
        case kD3DSIO_TEXM3x3:
          throw std::runtime_error("unsupported legacy texture opcode: " + opcodeName(instruction.opcode));
        default:
          throw std::runtime_error("unsupported D3D opcode: " + opcodeName(instruction.opcode));
      }
      if (predicatedBody) {
        out << "  }\n";
      }
    }
    if (!controlStack.empty()) {
      throw std::runtime_error("unbalanced D3D control flow");
    }
    if (!callConditionalStack.empty()) {
      throw std::runtime_error("unbalanced D3D CALL/RET");
    }
    if (!callReturnStack.empty()) {
      throw std::runtime_error("unbalanced internal D3D CALL frame");
    }

    out << "  out.position = outPosition;\n";
    if (::dxmt9::debug::flipTranslatedVertexY()) {
      out << "  out.position.y = -out.position.y;\n";
    }
    if (shaders::vsoutEmitColor(context.vsOutLayout)) {
      out << "  out.color = outColor;\n";
    }
    if (shaders::vsoutEmitSecondaryColor(context.vsOutLayout)) {
      out << "  out.secondaryColor = outSecondaryColor;\n";
    }
    {
      for (size_t i = 0; i < kMaxTextureStages; ++i) {
        if (!shaders::vsoutEmitTexcoord(context.vsOutLayout, i)) continue;
        out << "  out.texcoord" << i << " = outTexcoord[" << i << "];\n";
      }
    }
    if (shaders::vsoutEmitFogFactor(context.vsOutLayout)) {
      out << "  out.fogFactor = outFogFactor;\n";
    }
    if (shaders::vsoutEmitPointSize(context.vsOutLayout)) {
      out << "  out.pointSize = outPointSize;\n";
    }
    out << "  out.position.xy += ffpVs.halfPixelFixup * out.position.w;\n";
    if (context.clipPlaneMask != 0) {
      // Single `[[clip_distance]]` min-fold — see comment in
      // `dxmt9_ffp_shaders.cpp`. Apple Metal only honours one
      // [[clip_distance]] output; we collapse D3D9's six clip planes
      // into one slot via min(d_i) over the runtime-enabled mask.
      out << "  float dxmt9_minClip = 1.0f;\n";
      out << "  for (uint i = 0u; i < 6u; ++i) {\n";
      out << "    if ((ffpVs.clipPlaneMask & (1u << i)) != 0u) {\n";
      out << "      const float d = dot(ffpVs.clipPlanes[i], out.position);\n";
      out << "      dxmt9_minClip = min(dxmt9_minClip, d);\n";
      out << "    }\n";
      out << "  }\n";
      out << "  out.clipDistance0 = dxmt9_minClip;\n";
    }
    out << "  return out;\n";
    out << "}\n";
    out << "// decoded d3d hash " << module.hash << "\n";
    return out.str();
  }

  const auto samplerUsage = collectPixelSamplerUsage(module, context);
  const auto pixelInputSemantics = collectPixelInputSemantics(module);
  const bool textured = std::any_of(samplerUsage.begin(), samplerUsage.end(), [](bool used) { return used; });
  // D3DSAMP_MIPMAPLODBIAS (gap_d3d9 B.3) PSO-variant gate: only emit the slot-4
  // SamplerLodBias param + thread bias() through sample() when the variant key
  // flagged a non-zero sampler LOD bias. When clear, the translated MSL is
  // byte-identical to the pre-MIPMAPLODBIAS plain-sample form, and the encoder
  // skips the slot-4 bind on the same predicate. Only meaningful when the
  // fragment actually samples a texture.
  const bool emitLodBias = context.samplerLodBias && textured;
  const bool usesVFaceInput = pixelUsesVFaceInput(module);
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

  // D3DSAMP_MIPMAPLODBIAS (gap_d3d9 B.3): per-sampler mip LOD bias is applied
  // at sample time via `texture.sample(..., bias(b))`; Metal samplers carry no
  // LOD-bias field. The bias rides a dedicated fragment uniform bound at slot 4
  // (host struct dxmt9::state::SamplerLodBias). Declared inline here — byte
  // identical float[8] — so the shared prelude stays untouched. PSO-variant
  // gated: only emitted when some active sampler carries a non-zero LOD bias.
  if (emitLodBias) {
    out << "struct SamplerLodBias {\n";
    out << "  float bias[" << kMaxTextureStages << "];\n";
    out << "};\n";
  }
  const char* fragmentReturnType = usesFragmentOutStruct ? "FSOut" : "float4";
  auto emitFrontFacingParameter = [&] {
    if (usesVFaceInput) {
      out << "                     bool frontFacing [[front_facing]],\n";
    }
  };
  if (textured) {
    if (fragmentArgbufResourceArray) {
      // R-BACK-12.22..12.26 (resource-array sub-mode) — texture/sampler
      // resources ride the slot-30 argbuf arrays; the entry point declares
      // NO [[texture(N)]] / [[sampler(N)]] params. The alias block below
      // re-binds `tex<stage>` / `samp<stage>` (all 2D — guaranteed by
      // pixelResourceArrayEligible) off the argbuf so every translated
      // sample site (tex<stage>.sample(samp<stage>, ...)) is byte-identical
      // to the direct lane.
      out << "fragment " << fragmentReturnType
          << " dxmt9_fs(VSOut in [[stage_in]],\n";
      emitFrontFacingParameter();
      out << "                     constant ArgbufLayout& abuf [[buffer("
          << kArgbufHybridBindSlot << ")]]";
      if (emitLodBias) {
        out << ",\n                     constant SamplerLodBias& samplerLodBias [[buffer(4)]]";
      }
      out << ") {\n";
      out << "  constant PsConsts& psConsts = *abuf.psConsts;\n";
      out << "  constant FfpPsConsts& ffpPs = *abuf.ffpPs;\n";
      for (u32 stage = 0; stage < kMaxSamplers; ++stage) {
        if (!samplerUsage[stage]) {
          continue;
        }
        out << "  texture2d<float> tex" << stage << " = abuf.textures["
            << stage << "];\n";
        out << "  sampler samp" << stage << " = abuf.samplers[" << stage
            << "];\n";
      }
    } else if (argbufHybrid) {
      // R-BACK-12.22..12.26 MSL routing — single argbuf at slot 30
      // replaces uniform slots 0/3. Texture/sampler parameters remain
      // direct so texture-bound Stage 2 draws share the proven Stage 1
      // resource binding lane while uniforms use the argbuf.
      out << "fragment " << fragmentReturnType
          << " dxmt9_fs(VSOut in [[stage_in]],\n";
      emitFrontFacingParameter();
      out << "                     constant ArgbufLayout& abuf [[buffer("
          << kArgbufHybridBindSlot << ")]], ";
      emitFragmentTextureArguments(out, samplerUsage, module, context);
      if (emitLodBias) {
        out << ",\n                     constant SamplerLodBias& samplerLodBias [[buffer(4)]]";
      }
      out << ") {\n";
      out << "  constant PsConsts& psConsts = *abuf.psConsts;\n";
      out << "  constant FfpPsConsts& ffpPs = *abuf.ffpPs;\n";
    } else {
      out << "fragment " << fragmentReturnType
          << " dxmt9_fs(VSOut in [[stage_in]],\n";
      emitFrontFacingParameter();
      out << "                     constant PsConsts& psConsts [[buffer(0)]],\n";
      out << "                     constant FfpPsConsts& ffpPs [[buffer(3)]], ";
      emitFragmentTextureArguments(out, samplerUsage, module, context);
      if (emitLodBias) {
        out << ",\n                     constant SamplerLodBias& samplerLodBias [[buffer(4)]]";
      }
      out << ") {\n";
    }
  } else {
    if (argbufHybrid) {
      out << "fragment " << fragmentReturnType
          << " dxmt9_fs(VSOut in [[stage_in]],\n";
      emitFrontFacingParameter();
      out << "                     constant ArgbufLayout& abuf [[buffer("
          << kArgbufHybridBindSlot << ")]]) {\n";
      out << "  constant PsConsts& psConsts = *abuf.psConsts;\n";
      out << "  constant FfpPsConsts& ffpPs = *abuf.ffpPs;\n";
    } else {
      out << "fragment " << fragmentReturnType
          << " dxmt9_fs(VSOut in [[stage_in]],\n";
      emitFrontFacingParameter();
      out << "                     constant PsConsts& psConsts [[buffer(0)]],\n";
      out << "                     constant FfpPsConsts& ffpPs [[buffer(3)]]) {\n";
    }
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
  // R-SHADER-AIR-SIZE (FS only): size r[] and outColor[] to the
  // shader's actual max-written-Temp / max-written-oC index instead of
  // the spec maxima (32 / 4). DXMT_DEBUG_FORCE_FRAGMENT_COLOR isolation
  // showed Apple's MSL → AIR is NOT eliding the per-fragment zero-init
  // loops as expected — keeping `r[32]` charges ~96 ms of GPU work per
  // SFIV main-scene CB on top of a ~1.3 ms force-color floor. The
  // `collectConstantUsage` scan handles indexed dst/src writes by
  // forcing maxTempIndex to 31, so the trim is safe for those. VS
  // path stays at the spec maximum until the scan is audited for
  // subroutine/control-flow corner cases — a prior reversion of the
  // shared FS+VS trim caused a pink-gradient regression that the FS
  // half alone is not implicated in.
  auto constantUsage = collectConstantUsage(module);
  promoteIndexedConstantDestinations(constantUsage, module, false);
  const u32 tempCount =
      static_cast<u32>(std::max<std::int32_t>(1, constantUsage.maxTempIndex + 1));
  const u32 colorCount =
      static_cast<u32>(std::max<std::int32_t>(1, constantUsage.maxColorIndex + 1));
  out << "  float4 color = float4(1.0f);\n";
  out << "  float4 outColor[" << colorCount << "];\n";
  for (u32 i = 0; i < colorCount; ++i) {
    out << "  outColor[" << i << "] = float4(1.0f);\n";
  }
  out << "  float4 ignoredColor = float4(0.0f);\n";
  out << "  float4 outSecondaryColor = float4(0.0f);\n";
  // R-SHADER-FS-DEAD-OT: SM2+ pixel shaders never write to oT* registers
  // and the SM1.x `mov tN, …` pattern is the only producer. Scanning
  // the module lets us skip the dead 8 × float4 zero-init loop per
  // fragment for the common case. The compiler appears to DCE it
  // anyway (~1% measured), but the IR-level skip is the source of
  // truth.
  const bool usesTexcoordOut = pixelUsesTexcoordOut(module);
  if (usesTexcoordOut) {
    out << "  float4 outTexcoord[" << kMaxTextureStages << "];\n";
    if (module.major == 1u) {
      out << "  for (uint i = 0; i < " << kMaxTextureStages
          << "u; ++i) { outTexcoord[i] = dxmt9_select_texcoord(in, i); }\n";
    } else {
      out << "  for (uint i = 0; i < " << kMaxTextureStages
          << "u; ++i) { outTexcoord[i] = float4(0.0f, 0.0f, 0.0f, 1.0f); }\n";
    }
  }
  out << "  float4 ignoredTexcoord = float4(0.0f);\n";
  out << "  float4 dxmt9_texm = float4(0.0f);\n";
  out << "  float4 outPosition = float4(0.0f);\n";
  out << "  float outDepth = in.position.z;\n";
  out << "  float outFogFactor = 1.0f;\n";
  out << "  float outPointSize = 1.0f;\n";
	  out << "  int a0 = 0;\n";
	  out << "  int aL = 0;\n";
	  out << "  float4 r[" << tempCount << "];\n";
	  out << "  for (uint i = 0; i < " << tempCount
	      << "u; ++i) { r[i] = float4(0.0f); }\n";
    emitConstantBindings(out, false, constantUsage);
    emitPredicateBindings(out, shaderUsesPredicateRegisters(module));
    std::vector<FlowBlock> controlStack;
    std::vector<bool> callConditionalStack;
    std::vector<std::string> callReturnStack;
    u32 legacyM3x3PadCount = 0;
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
      } else if ((instruction.opcode == kD3DSIO_LABEL || instruction.opcode == kD3DSIO_CALL
                  || instruction.opcode == kD3DSIO_CALLNZ) && i == 0) {
        out << "label" << decodeLabelIndex(instruction.operands[i]);
      } else {
        out << decodeOperandToken(instruction.operands[i], module.stage, destination);
      }
    }
    out << "\n";

    auto readSrc = [&](size_t index, bool clampSm1Constants = true) {
      if (index >= instruction.operands.size()) {
        std::ostringstream message;
        message << "missing D3D source operand"
                << " opcode=" << opcodeName(instruction.opcode)
                << " requestedIndex=" << index
                << " operandCount=" << instruction.operands.size();
        throw std::runtime_error(message.str());
      }
      const auto token = instruction.operands[index];
      auto reg = decodeOperandRegister(instruction, index, module.stage);
      std::string expr;
      if (module.major == 1u && module.minor < 4u && decodeRegisterType(token) == kD3DSPR_ADDR) {
        expr = "outTexcoord[" + std::to_string(std::min<u32>(decodeRegisterIndex(token), kMaxTextureStages - 1u)) + "]";
      } else if (reg.kind == D3DRegisterKind::Input) {
        expr = readPixelInputExpression(token, "in", pixelInputSemantics);
      } else {
        expr = readOperandExpression(instruction, reg, "float4(0.0f)", "in", false, false, "outPosition",
                                     "outColor", "outSecondaryColor", "outTexcoord", "outFogFactor",
                                     "outPointSize", "r", "cFloat", "cInt", "cBool", "p",
                                     nullptr, tempCount - 1u);
      }
      if (clampSm1Constants && module.major == 1u && reg.kind == D3DRegisterKind::ConstFloat) {
        expr = "clamp(" + expr + ", float4(-1.0f), float4(1.0f))";
      }
      expr = applySwizzle(expr, decodeSwizzle(token));
      expr = applySourceModifier(std::move(expr), decodeSourceModifier(token));
      return expr;
    };
    auto sourceIsVPos = [&](size_t index) {
      if (index >= instruction.operands.size()) {
        return false;
      }
      const auto reg = decodeOperandRegister(instruction, index, module.stage);
      return reg.kind == D3DRegisterKind::MiscType && reg.index == 0;
    };

    auto emitMaskedAssign = [&](const std::string& target, const std::string& value, u32 mask, bool scalar = false) {
      const std::string finalValue =
          applyDestModifier(value, decodeDestModifier(instruction.operands.empty() ? 0u : instruction.operands[0]));
      if (scalar) {
        out << "  " << target << " = " << finalValue << ".x;\n";
        return;
      }
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
    auto pixelColorWriteTarget = [&](u32 index) {
      return module.major == 1u ? std::string("r[0]") : pixelColorTarget(index);
    };
    auto texcoordTarget = [](u32 index) {
      if (index >= kMaxTextureStages) {
        return std::string("ignoredTexcoord");
      }
      return std::string("outTexcoord[") + std::to_string(index) + "]";
    };
    const bool forcePixelVFlip = ::dxmt9::debug::forcePixelVFlip();
    auto sampleCoord = [&](u32 sampler, const std::string& coord) {
      return sampleCoordExpression(samplerTextureType(module, context, sampler), coord, forcePixelVFlip);
    };
    auto sampleTexture = [&](u32 sampler, const std::string& coord) {
      if (context.unboundTextureFallback &&
          (sampler >= context.textures.size() || !context.textures[sampler])) {
        return std::string("float4(0.0f, 0.0f, 0.0f, 1.0f)");
      }
      // D3DSAMP_MIPMAPLODBIAS (gap_d3d9 B.3): thread the per-sampler mip LOD
      // bias into the implicit-gradient sample. The bias rides the slot-4
      // SamplerLodBias uniform (8 stages). PSO-variant gated on the same flag
      // as the slot-4 param declaration above: when clear the sample is the
      // pre-feature plain-sample form with no bias() argument. Explicit-LOD /
      // gradient opcodes (TEXLDL/TEXLDD) supply their own level and are handled
      // at their own sites without bias().
      std::string biasArg;
      if (emitLodBias && sampler < kMaxTextureStages) {
        biasArg = ", bias(samplerLodBias.bias[" + std::to_string(sampler) + "])";
      }
      return "tex" + std::to_string(sampler) + ".sample(samp" + std::to_string(sampler) + ", "
             + sampleCoord(sampler, coord) + biasArg + ")";
    };
    auto legacyStage = [&] {
      if (instruction.operands.empty()) {
        throw std::runtime_error("legacy texture opcode requires a destination/stage operand");
      }
      return std::min<u32>(decodeRegisterIndex(instruction.operands[0]), kMaxTextureStages - 1u);
    };
    auto legacyTexcoordInput = [](u32 stage) {
      return "dxmt9_select_texcoord(in, " + std::to_string(stage) + "u)";
    };
    auto legacySample = [&](u32 stage, const std::string& coord) {
      return sampleTexture(stage, coord);
    };
    auto legacyDstTarget = [&](u32 token) {
      const auto index = decodeRegisterIndex(token);
      switch (decodeRegisterType(token)) {
        case kD3DSPR_TEMP:
          return "r[" + std::to_string(std::min<u32>(index, tempCount - 1u)) + "]";
        case kD3DSPR_ADDR:
          return index < kMaxTextureStages ? "outTexcoord[" + std::to_string(index) + "]"
                                           : std::string("ignoredTexcoord");
        case kD3DSPR_COLOROUT:
          return pixelColorWriteTarget(index);
        default: {
          const auto reg = decodeRegisterRef(token, module.stage);
          if (reg.kind == D3DRegisterKind::Temp) {
            return "r[" + std::to_string(std::min<u32>(reg.index, tempCount - 1u)) + "]";
          }
          if (reg.kind == D3DRegisterKind::ColorOut) {
            return pixelColorWriteTarget(reg.index);
          }
          if (reg.kind == D3DRegisterKind::TexCoordOut) {
            return texcoordTarget(reg.index);
          }
          throw std::runtime_error("unsupported legacy texture destination");
        }
      }
    };
    auto legacyAssign = [&](const std::string& value) {
      emitMaskedAssign(legacyDstTarget(instruction.operands[0]), value, decodeWriteMask(instruction.operands[0]));
    };
    auto legacyDot = [&](size_t srcIndex) {
      const auto stage = legacyStage();
      return "dot((" + texcoordTarget(stage) + ").xyz, (" + readSrc(srcIndex) + ").xyz)";
    };
    auto legacyBumpCoord = [&](u32 stage, const std::string& base, const std::string& bump) {
      const auto s = std::to_string(stage);
      return "float4((" + base + ").xy + float2(ffpPs.bumpEnvMat[" + s + "].x * ((" + bump +
             ").x - 0.5f) + ffpPs.bumpEnvMat[" + s + "].z * ((" + bump +
             ").y - 0.5f), ffpPs.bumpEnvMat[" + s + "].y * ((" + bump +
             ").x - 0.5f) + ffpPs.bumpEnvMat[" + s + "].w * ((" + bump +
             ").y - 0.5f)), 0.0f, 1.0f)";
    };

    auto currentGuard = [&](const D3DDecodedInstruction& guardedInstruction) {
      std::vector<std::string> terms;
      if (!callReturnStack.empty()) {
        terms.push_back("!" + callReturnStack.back());
      }
      if (guardedInstruction.predicated) {
        terms.push_back("p[0]");
      }
      return combineBooleanTerms(terms);
    };
    auto guardedCondition = [&](const D3DDecodedInstruction& guardedInstruction,
                                const std::string& condition) {
      const auto guard = currentGuard(guardedInstruction);
      if (guard.empty()) {
        return condition;
      }
      std::vector<std::string> terms;
      terms.push_back(guard);
      terms.push_back(condition);
      return combineBooleanTerms(terms);
    };
    auto emitInstructionGuardOpen = [&] {
      const auto guard = currentGuard(instruction);
      if (guard.empty()) {
        return false;
      }
      out << "  if (" << guard << ") {\n";
      return true;
    };

    if (instruction.opcode == kDXMT9_INTERNAL_CALL_BEGIN) {
      const auto flag = "dxmt9_call_ret_" + std::to_string(instruction.controls);
      out << "  bool " << flag << " = false;\n";
      callReturnStack.push_back(flag);
      continue;
    }
    if (instruction.opcode == kDXMT9_INTERNAL_CALL_END) {
      if (callReturnStack.empty()) {
        throw std::runtime_error("internal CALL_END without CALL_BEGIN");
      }
      callReturnStack.pop_back();
      continue;
    }
    if (instruction.opcode == kDXMT9_INTERNAL_CALL_RET) {
      if (callReturnStack.empty()) {
        out << "  return color;\n";
      } else {
        out << "  " << callReturnStack.back() << " = true;\n";
      }
      continue;
    }
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
      callConditionalStack.push_back(false);
      continue;
    }
    if (instruction.opcode == kD3DSIO_CALLNZ) {
      if (instruction.operands.size() < 2) {
        throw std::runtime_error("CALLNZ requires a label operand and condition source");
      }
      out << "  // callnz label " << decodeLabelIndex(instruction.operands[0]) << "\n";
      out << "  if ((" << readSrc(1) << ").x != 0.0f) {\n";
      out << "  do {\n";
      callConditionalStack.push_back(true);
      continue;
    }
    if (instruction.opcode == kD3DSIO_RET) {
      if (!callConditionalStack.empty()) {
        const bool conditionalCall = callConditionalStack.back();
        callConditionalStack.pop_back();
        out << "  break;\n";
        out << "  } while (false);\n";
        if (conditionalCall) {
          out << "  }\n";
        }
      } else {
        out << "  return color;\n";
      }
      continue;
    }
    if (instruction.opcode == kD3DSIO_IF) {
      if (instruction.operands.empty()) {
        throw std::runtime_error("IF requires a condition operand");
      }
      const auto guard = currentGuard(instruction);
      out << "  if (" << guardedCondition(instruction, "(" + readSrc(0) + ").x != 0.0f") << ") {\n";
      controlStack.push_back(FlowBlock{instruction.opcode, false, guard});
      continue;
    }
    if (instruction.opcode == kD3DSIO_IFC) {
      if (instruction.operands.size() < 2) {
        throw std::runtime_error("IFC requires two source operands");
      }
      const char* op = "==";
      switch (instruction.controls & 0xfu) {
        case 1: op = ">";  break;  // D3DSPC_GT
        case 2: op = "=="; break;  // D3DSPC_EQ
        case 3: op = ">="; break;  // D3DSPC_GE
        case 4: op = "<";  break;  // D3DSPC_LT
        case 5: op = "!="; break;  // D3DSPC_NE
        case 6: op = "<="; break;  // D3DSPC_LE
        default: op = "=="; break;
      }
      const auto guard = currentGuard(instruction);
      out << "  if (" << guardedCondition(instruction, "(" + readSrc(0) + ").x " + op + " (" + readSrc(1) + ").x") << ") {\n";
      controlStack.push_back(FlowBlock{kD3DSIO_IF, false, guard});
      continue;
    }
    if (instruction.opcode == kD3DSIO_ELSE) {
      if (controlStack.empty() || controlStack.back().opcode != kD3DSIO_IF || controlStack.back().sawElse) {
        throw std::runtime_error("ELSE without matching IF");
      }
      controlStack.back().sawElse = true;
      if (controlStack.back().guard.empty()) {
        out << "  } else {\n";
      } else {
        out << "  } else if (" << controlStack.back().guard << ") {\n";
      }
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
      const auto countSource = instruction.opcode == kD3DSIO_LOOP && instruction.operands.size() > 1
                                   ? readSrc(1)
                                   : readSrc(0);
      const auto countExpr = "max(0, int(round(" + countSource + ".x)))";
      const auto guard = currentGuard(instruction);
      const auto loopGuard = guard.empty() ? std::string{} : guard + " && ";
      if (instruction.opcode == kD3DSIO_LOOP) {
        out << "  for (int dxmt9_loop_" << loopIndex << " = 0, dxmt9_loopCount_" << loopIndex << " = " << countExpr
            << "; " << loopGuard << "dxmt9_loop_" << loopIndex << " < dxmt9_loopCount_" << loopIndex << "; ++dxmt9_loop_"
            << loopIndex << ") {\n";
      } else {
        out << "  for (int dxmt9_rep_" << loopIndex << " = 0, dxmt9_repCount_" << loopIndex << " = " << countExpr
            << "; " << loopGuard << "dxmt9_rep_" << loopIndex << " < dxmt9_repCount_" << loopIndex << "; ++dxmt9_rep_"
            << loopIndex << ") {\n";
      }
      controlStack.push_back(FlowBlock{instruction.opcode, false, guard});
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
      const auto guard = currentGuard(instruction);
      if (guard.empty()) {
        out << "  break;\n";
      } else {
        out << "  if (" << guard << ") { break; }\n";
      }
      continue;
    }
    if (instruction.opcode == kD3DSIO_BREAKP) {
      if (instruction.operands.empty()) {
        throw std::runtime_error("BREAKP requires a predicate operand");
      }
      out << "  if (" << guardedCondition(instruction, "(" + readSrc(0) + ").x != 0.0f") << ") { break; }\n";
      continue;
    }
    if (instruction.opcode == kD3DSIO_BREAKC) {
      if (instruction.operands.size() < 2) {
        throw std::runtime_error("BREAKC requires two source operands");
      }
      const char* op = "==";
      switch (instruction.controls & 0xfu) {
        case 1: op = ">";  break;  // D3DSPC_GT
        case 2: op = "=="; break;  // D3DSPC_EQ
        case 3: op = ">="; break;  // D3DSPC_GE
        case 4: op = "<";  break;  // D3DSPC_LT
        case 5: op = "!="; break;  // D3DSPC_NE
        case 6: op = "<="; break;  // D3DSPC_LE
        default: op = "=="; break;
      }
      out << "  if (" << guardedCondition(instruction, "(" + readSrc(0) + ").x " + op + " (" + readSrc(1) + ").x") << ") { break; }\n";
      continue;
    }

    const bool predicatedBody = emitInstructionGuardOpen();
    switch (instruction.opcode) {
      case kD3DSIO_NOP:
        break;
      case kD3DSIO_DEF: {
        const auto dst = decodeOperandRegister(instruction, 0, module.stage);
        requireSupportedDestinationAddressing(dst);
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
        out << "  " << constantDestinationTarget(dst, false) << " = " << formatFloatVec4(values) << ";\n";
        break;
      }
      case kD3DSIO_DEFI: {
        const auto dst = decodeOperandRegister(instruction, 0, module.stage);
        requireSupportedDestinationAddressing(dst);
        const auto values = std::array<i32, 4>{static_cast<i32>(instruction.operands[1]),
                                               static_cast<i32>(instruction.operands[2]),
                                               static_cast<i32>(instruction.operands[3]),
                                               static_cast<i32>(instruction.operands[4])};
        if (dst.kind != D3DRegisterKind::ConstInt) {
          throw std::runtime_error("DEFI requires an integer constant destination");
        }
        out << "  " << constantDestinationTarget(dst, false) << " = " << formatIntVec4(values) << ";\n";
        break;
      }
      case kD3DSIO_DEFB: {
        const auto dst = decodeOperandRegister(instruction, 0, module.stage);
        requireSupportedDestinationAddressing(dst);
        if (dst.kind != D3DRegisterKind::ConstBool) {
          throw std::runtime_error("DEFB requires a boolean constant destination");
        }
        out << "  " << constantDestinationTarget(dst, false) << " = "
            << (instruction.operands[1] != 0u ? "1u" : "0u") << ";\n";
        break;
      }
      case kD3DSIO_MOV: {
        const auto dst = decodeOperandRegister(instruction, 0, module.stage);
        requireSupportedDestinationAddressing(dst);
        const auto dstMask = decodeWriteMask(instruction.operands[0]);
        const auto value = readSrc(1);
        switch (dst.kind) {
          case D3DRegisterKind::Temp:
            emitMaskedAssign(tempDestinationTarget(dst, tempCount - 1u), value, dstMask);
            break;
          case D3DRegisterKind::ColorOut:
            emitMaskedAssign(pixelColorWriteTarget(dst.index), value, dstMask);
            break;
          case D3DRegisterKind::TexCoordOut:
            emitMaskedAssign(texcoordDestinationTarget(dst), value, dstMask);
            break;
          case D3DRegisterKind::DepthOut:
            emitMaskedAssign("outDepth", value, dstMask, true);
            break;
          case D3DRegisterKind::ConstFloat:
            out << "  " << constantDestinationTarget(dst, false) << " = " << value << ";\n";
            break;
          case D3DRegisterKind::ConstInt:
            out << "  " << constantDestinationTarget(dst, false) << " = int4(" << value << ");\n";
            break;
          case D3DRegisterKind::ConstBool:
            out << "  " << constantDestinationTarget(dst, false) << " = "
                << value << ".x != 0.0f ? 1u : 0u;\n";
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
        const auto dst = decodeOperandRegister(instruction, 0, module.stage);
        requireSupportedDestinationAddressing(dst);
        const auto dstMask = decodeWriteMask(instruction.operands[0]);
        const auto value = readSrc(1);
        switch (dst.kind) {
          case D3DRegisterKind::Address:
            // a0 is a 4-component address register; MOVA rounds each
            // component so later relative addressing can index by any of
            // a0.x/.y/.z/.w (matrix-palette skinning uses a0.x and a0.y).
            emitMovaAddressAssign(out, value, dstMask);
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
        const auto dst = decodeOperandRegister(instruction, 0, module.stage);
        requireSupportedDestinationAddressing(dst);
        if (dst.kind != D3DRegisterKind::Predicate) {
          throw std::runtime_error("SETP requires a predicate register destination");
        }
        out << "  p[" << dst.index << "] = (" << readSrc(1) << ").x != 0.0f;\n";
        break;
      }
      case kD3DSIO_TEXKILL: {
        if (instruction.operands.empty()) {
          throw std::runtime_error("TEXKILL requires a source operand");
        }
        auto reg = decodeRegisterRef(instruction.operands[0], module.stage);
        if (!instruction.relAddrTokens.empty()) {
          reg.relAddrToken = instruction.relAddrTokens[0];
        }
        std::string value;
        if (reg.kind == D3DRegisterKind::Input) {
          value = readPixelInputExpression(instruction.operands[0], "in", pixelInputSemantics);
        } else {
          value = readOperandExpression(instruction, reg, "float4(0.0f)", "in", false, false, "outPosition",
                                        "outColor", "outSecondaryColor", "outTexcoord", "outFogFactor",
                                        "outPointSize", "r", "cFloat", "cInt", "cBool", "p",
                                        nullptr, tempCount - 1u);
        }
        out << "  if (" << texkillMaskCondition(value, decodeWriteMask(instruction.operands[0])) << ") {\n";
        out << "    discard_fragment();\n";
        out << "  }\n";
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
      case kD3DSIO_LIT:
      case kD3DSIO_DST:
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
        const auto dst = decodeOperandRegister(instruction, 0, module.stage);
        requireSupportedDestinationAddressing(dst);
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
            value = "float4(log2(abs(" + readSrc(1) + ")))";
            break;
          case kD3DSIO_LIT: {
            // D3D9 LIT: see the corresponding emit in the SM2+ branch
            // above. Same expansion duplicated here so the SM1 lowering
            // path (which uses this separate switch) gets the lighting
            // helper too — Wine fp_special_test's vs_lit sub-case
            // depends on this opcode reaching MSL on vs_2_0.
            const auto src = readSrc(1);
            value = std::string("([&](){ float4 lit_src = ") + src + ";"
                    " float lit_x = lit_src.x;"
                    " float lit_y = lit_src.y;"
                    " float lit_w = clamp(lit_src.w, -128.0f, 128.0f);"
                    " float lit_z = (lit_x > 0.0f) ? pow(max(lit_y, 0.0f), lit_w) : 0.0f;"
                    " return float4(1.0f, max(lit_x, 0.0f), lit_z, 1.0f); }())";
            break;
          }
          case kD3DSIO_DST: {
            // D3D9 DST mirror in the SM1 lowering branch — see the SM2+
            // arm above for the canonical formula.
            value = std::string("float4(1.0f, (") + readSrc(1) + ").y * (" + readSrc(2) + ").y, (" +
                    readSrc(1) + ").z, (" + readSrc(2) + ").w)";
            break;
          }
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
            if (instruction.coissue && module.stage == D3DShaderStage::Pixel &&
                module.major == 1u && module.minor < 4u) {
              value = readSrc(2);
            } else {
              value = "select(" + readSrc(3) + ", " + readSrc(2) + ", " + readSrc(1) + " > float4(0.5f))";
            }
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
              const auto sampler =
                  module.major == 1u ? legacyStage() : textureSamplerIndex(instruction, module.stage);
              const auto coord =
                  module.major == 1u && module.minor < 4u ? texcoordTarget(sampler) : readSrc(1);
              value = sampleTexture(sampler, coord);
            }
            break;
          case kD3DSIO_DSX:
            value = sourceIsVPos(1) ? "float4(1.0f, 0.0f, 0.0f, 0.0f)" : "dfdx(" + readSrc(1) + ")";
            break;
          case kD3DSIO_DSY:
            value = sourceIsVPos(1) ? "float4(0.0f, 1.0f, 0.0f, 0.0f)" : "dfdy(" + readSrc(1) + ")";
            break;
          case kD3DSIO_TEXLDD:
            {
              const auto sampler = textureSamplerIndex(instruction, module.stage);
              const auto coord = readSrc(1);
              const auto ddx = readSrc(3);
              const auto ddy = readSrc(4);
              if (context.unboundTextureFallback &&
                  (sampler >= context.textures.size() || !context.textures[sampler])) {
                value = "float4(0.0f, 0.0f, 0.0f, 1.0f)";
              } else {
                value = "tex" + std::to_string(sampler) + ".sample(samp" + std::to_string(sampler) + ", " +
                        sampleCoord(sampler, coord) + ", " +
                        textureGradientExpression(samplerTextureType(module, context, sampler), ddx, ddy) + ")";
              }
            }
            break;
	          case kD3DSIO_TEXLDL:
	            {
	              const auto sampler = textureSamplerIndex(instruction, module.stage);
	              const auto coord = readSrc(1);
	              if (context.unboundTextureFallback &&
	                  (sampler >= context.textures.size() || !context.textures[sampler])) {
	                value = "float4(0.0f, 0.0f, 0.0f, 1.0f)";
	              } else {
	                value = "tex" + std::to_string(sampler) + ".sample(samp" + std::to_string(sampler) + ", " +
                        sampleCoord(sampler, coord) + ", level(" + coord + ".w))";
	              }
	            }
	            break;
          default:
            throw std::runtime_error("unsupported arithmetic opcode");
        }
        switch (dst.kind) {
          case D3DRegisterKind::Temp:
            emitMaskedAssign(tempDestinationTarget(dst, tempCount - 1u), value, dstMask);
            break;
          case D3DRegisterKind::Input:
            if (module.major == 1u && decodeRegisterType(instruction.operands[0]) == kD3DSPR_ADDR) {
              emitMaskedAssign(texcoordTarget(dst.index), value, dstMask);
              break;
            }
            throw std::runtime_error("unsupported arithmetic input destination");
          case D3DRegisterKind::ColorOut:
            emitMaskedAssign(pixelColorWriteTarget(dst.index), value, dstMask);
            break;
          case D3DRegisterKind::TexCoordOut:
            emitMaskedAssign(texcoordDestinationTarget(dst), value, dstMask);
            break;
          case D3DRegisterKind::DepthOut:
            emitMaskedAssign("outDepth", value, dstMask, true);
            break;
          case D3DRegisterKind::ConstFloat:
            out << "  " << constantDestinationTarget(dst, false) << " = " << value << ";\n";
            break;
          case D3DRegisterKind::ConstInt:
            out << "  " << constantDestinationTarget(dst, false) << " = int4(" << value << ");\n";
            break;
          case D3DRegisterKind::ConstBool:
            out << "  " << constantDestinationTarget(dst, false) << " = "
                << value << ".x != 0.0f ? 1u : 0u;\n";
            break;
          default:
            throw std::runtime_error("unsupported arithmetic destination");
        }
        break;
      }
      case kD3DSIO_DCL:
        break;
      case kD3DSIO_TEXCOORD: {
        if (module.major != 1u) {
          throw std::runtime_error("TEXCOORD is only implemented for SM1.x pixel shaders");
        }
        if (module.minor >= 4u) {
          legacyAssign(readSrc(1));
        } else {
          const auto stage = legacyStage();
          legacyAssign("float4(saturate((" + legacyTexcoordInput(stage) + ").xyz), 1.0f)");
        }
        break;
      }
      case kD3DSIO_TEXBEM:
      case kD3DSIO_TEXBEML: {
        const auto stage = legacyStage();
        const auto coord = legacyBumpCoord(stage, texcoordTarget(stage), readSrc(1));
        std::string value = legacySample(stage, coord);
        if (instruction.opcode == kD3DSIO_TEXBEML) {
          value = "(" + value + " * saturate((" + readSrc(1) + ").z * ffpPs.bumpEnvLum[" +
                  std::to_string(stage) + "].x + ffpPs.bumpEnvLum[" + std::to_string(stage) + "].y))";
        }
        legacyAssign(value);
        break;
      }
      case kD3DSIO_TEXREG2AR: {
        const auto stage = legacyStage();
        const auto src = readSrc(1);
        legacyAssign(legacySample(stage, "float4((" + src + ").w, (" + src + ").x, 0.0f, 1.0f)"));
        break;
      }
      case kD3DSIO_TEXREG2GB: {
        const auto stage = legacyStage();
        const auto src = readSrc(1);
        legacyAssign(legacySample(stage, "float4((" + src + ").y, (" + src + ").z, 0.0f, 1.0f)"));
        break;
      }
      case kD3DSIO_TEXM3x2PAD:
        out << "  dxmt9_texm.x = " << legacyDot(1) << ";\n";
        break;
      case kD3DSIO_TEXM3x2TEX: {
        const auto stage = legacyStage();
        out << "  dxmt9_texm.y = " << legacyDot(1) << ";\n";
        legacyAssign(legacySample(stage, "dxmt9_texm"));
        break;
      }
      case kD3DSIO_TEXM3x3PAD: {
        const char component = (legacyM3x3PadCount++ % 2u) == 0u ? 'x' : 'y';
        out << "  dxmt9_texm." << component << " = " << legacyDot(1) << ";\n";
        break;
      }
      case kD3DSIO_TEXM3x3TEX: {
        const auto stage = legacyStage();
        out << "  dxmt9_texm.z = " << legacyDot(1) << ";\n";
        legacyM3x3PadCount = 0;
        legacyAssign(legacySample(stage, "dxmt9_texm"));
        break;
      }
      case kD3DSIO_TEXM3x3DIFF:
        throw std::runtime_error("reserved legacy texture opcode: " + opcodeName(instruction.opcode));
      case kD3DSIO_TEXM3x3SPEC: {
        const auto stage = legacyStage();
        out << "  dxmt9_texm.z = " << legacyDot(1) << ";\n";
        legacyM3x3PadCount = 0;
        const auto eye = readSrc(2, /*clampSm1Constants=*/false);
        legacyAssign(legacySample(stage, "float4(reflect(normalize((" + eye + ").xyz), "
                                  "normalize(dxmt9_texm.xyz)), 1.0f)"));
        break;
      }
      case kD3DSIO_TEXM3x3VSPEC: {
        const auto stage = legacyStage();
        const auto s0 = stage >= 2u ? stage - 2u : 0u;
        const auto s1 = stage >= 1u ? stage - 1u : 0u;
        out << "  dxmt9_texm.z = " << legacyDot(1) << ";\n";
        legacyM3x3PadCount = 0;
        const auto eye = "float3(" + texcoordTarget(s0) + ".w, " + texcoordTarget(s1) + ".w, " +
                         texcoordTarget(stage) + ".w)";
        legacyAssign(legacySample(stage, "float4(reflect(normalize(" + eye + "), "
                                  "normalize(dxmt9_texm.xyz)), 1.0f)"));
        break;
      }
      case kD3DSIO_BEM: {
        const auto stage = legacyStage();
        legacyAssign(legacyBumpCoord(stage, readSrc(1), readSrc(2)));
        break;
      }
      case kD3DSIO_TEXDEPTH:
        out << "  outDepth = clamp((" << readSrc(0) << ").x / min((" << readSrc(0)
            << ").y, 1.0f), 0.0f, 1.0f);\n";
        break;
      case kD3DSIO_TEXREG2RGB: {
        const auto stage = legacyStage();
        legacyAssign(legacySample(stage, readSrc(1)));
        break;
      }
      case kD3DSIO_TEXDP3TEX: {
        const auto stage = legacyStage();
        legacyAssign(legacySample(stage, "float4(" + legacyDot(1) + ", 0.0f, 0.0f, 1.0f)"));
        break;
      }
      case kD3DSIO_TEXM3x2DEPTH:
        out << "  dxmt9_texm.y = " << legacyDot(1) << ";\n";
        out << "  outDepth = dxmt9_texm.y == 0.0f ? 1.0f : clamp(dxmt9_texm.x / dxmt9_texm.y, 0.0f, 1.0f);\n";
        break;
      case kD3DSIO_TEXDP3:
        legacyAssign("float4(" + legacyDot(1) + ")");
        break;
      case kD3DSIO_TEXM3x3:
        out << "  dxmt9_texm.z = " << legacyDot(1) << ";\n";
        legacyM3x3PadCount = 0;
        legacyAssign("float4(dxmt9_texm.xyz, 1.0f)");
        break;
      default:
        throw std::runtime_error("unsupported D3D opcode: " + opcodeName(instruction.opcode));
    }
    if (predicatedBody) {
      out << "  }\n";
    }
    }
    if (!controlStack.empty()) {
      throw std::runtime_error("unbalanced D3D control flow");
    }
  if (!callConditionalStack.empty()) {
    throw std::runtime_error("unbalanced D3D CALL/RET");
  }
  if (!callReturnStack.empty()) {
    throw std::runtime_error("unbalanced internal D3D CALL frame");
  }
  if (module.major == 1u) {
    out << "  color = r[0];\n";
  } else {
    out << "  color = outColor[0];\n";
  }
	  out << "  if (ffpPs.alphaTestEnable != 0u) {\n";
  out << "    bool pass = true;\n";
  out << "    switch (ffpPs.alphaTestFunc) {\n";
  out << "      case 2u: pass = color.a < ffpPs.alphaRef; break;\n";
  out << "      case 3u: pass = color.a == ffpPs.alphaRef; break;\n";
  out << "      case 4u: pass = color.a <= ffpPs.alphaRef; break;\n";
  out << "      case 5u: pass = color.a > ffpPs.alphaRef; break;\n";
  out << "      case 6u: pass = color.a != ffpPs.alphaRef; break;\n";
  out << "      case 7u: pass = color.a >= ffpPs.alphaRef; break;\n";
  out << "      case 8u: pass = true; break;\n";
  out << "      default: pass = true; break;\n";
  out << "    }\n";
  out << "    if (!pass) {\n";
	  out << "      discard_fragment();\n";
	  out << "    }\n";
		  out << "  }\n";
		  out << "  if (ffpPs.fogMode != 0u) {\n";
		  out << "    color = dxmt9_apply_fog(color, ffpPs, in.position.z, in.fogFactor);\n";
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

}  // namespace

std::string makeTranslatedVertexSource(const ShaderRef& shader,
                                       const ShaderSourceContext& context) {
  return translateSpirvToMsl(translateD3DBytecodeToSpirv(shader, true, context), context, true);
}

// DXMT9_FS_HALF_PRECISION post-pass — rewrites the emitted FS body to use
// half (fp16) for every internal `float`/`float4` local, intermediate
// expression, and texture-sample return type. Apple Silicon GPUs have
// ~2× FP16 ALU throughput vs FP32; SFIV is 98% fragment-bound (verified
// via DXMT_DEBUG_FORCE_FRAGMENT_COLOR A/B), so the in-shader precision
// is the only dxmt9-side lever that meaningfully moves per-frame GPU
// time without breaking D3D9 transparency.
//
// Why post-pass instead of conditional emit at every site: the emitter
// produces ~50 distinct `float4` / `float` literal strings across the FS
// path (including non-trivial expressions like
// `clamp(..., float4(0.0f), float4(1.0f))`). A post-pass converts the
// whole FS function body at once with a couple of targeted regex
// substitutions — safer and far smaller diff than threading a `bool half`
// through every emit helper.
//
// Preserves:
//  * Host-side struct layouts (VSOut / VsConsts / PsConsts / FfpVsConsts /
//    FfpPsConsts) — these match the CPU writer, narrowing happens at the
//    use site.
//  * VS function body — VS is not the bottleneck (vertex GPU time is ~5%
//    of total per task-2 measurement).
//
// Modifies:
//  * FSOut struct fields — `float4 colorN [[color(N)]]` → `half4 colorN`
//    so the FS can `return half4(...)` natively. MSL writes half through
//    the framebuffer at whatever pixel-format conversion the RT requires
//    (RGBA8 downcast, RGBA16F native).
//  * Bare-return FS signature — `fragment float4 dxmt9_fs` →
//    `fragment half4 dxmt9_fs` for the single-color-output-no-depth path
//    where there is no FSOut struct.
//  * FS function body — all `float4`/`float3`/`float2`/`float ` →
//    `half`-prefixed.
//  * Texture sample type — `texture2d<float>` → `texture2d<half>` so
//    samples return half4 matching the half-typed body locals.
//  * Constant-buffer reads — `psConsts.psFloatConst[X]` is wrapped with
//    `half4(...)` since the buffer is host-side float4 and Apple's MSL
//    refuses implicit float→half on assignment.
std::string applyFsHalfPrecisionRewrite(std::string source) {
  // Locate the FS function open brace and matching close brace.
  const std::string openMarker = "fragment ";
  auto open = source.find(openMarker);
  if (open == std::string::npos) return source;
  auto braceOpen = source.find('{', open);
  if (braceOpen == std::string::npos) return source;
  // Find matching close brace by depth counting (skips nested struct
  // initializers, switch statements, etc.).
  std::size_t depth = 1;
  std::size_t i = braceOpen + 1;
  for (; i < source.size() && depth > 0; ++i) {
    if (source[i] == '{') ++depth;
    else if (source[i] == '}') --depth;
  }
  if (depth != 0) return source;
  const auto braceClose = i;  // one past the '}'

  auto replaceAll = [](std::string& s, std::string_view from, std::string_view to) {
    if (from.empty()) return;
    std::size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
      s.replace(pos, from.size(), to);
      pos += to.size();
    }
  };

  // (1) FS body — order matters: handle `float4` first so the more-
  // specific token doesn't get consumed by the `float ` rule.
  std::string body = source.substr(braceOpen, braceClose - braceOpen);
  replaceAll(body, "float4", "half4");
  replaceAll(body, "float2", "half2");
  replaceAll(body, "float3", "half3");
  replaceAll(body, "float ", "half ");
  // (1c) Helper renames — `dxmt9_merge` is renamed to its half overload
  // (71 SFIV FS calls). `dxmt9_select_texcoord` intentionally stays on
  // the float-typed original: the texture `.sample(sampler, coord)`
  // overload requires `float2 coord` regardless of texel type, so the
  // texcoord must remain float at the sample site. Other use sites of
  // select_texcoord that feed into a half4 expression (e.g.
  // `half4(normalize((dxmt9_select_texcoord(...)).xyz), 0)` rely on
  // MSL's narrow-on-assign through outer expressions; the half4 ctor
  // accepts float3 for SIMD lanes via component-wise narrowing.
  replaceAll(body, "dxmt9_merge(", "dxmt9_merge_h(");
  // (1d) Float literals → half literals. Apple's constructor matching
  // refuses `half4(half3, 0.0f)` (no `(half3, float)` candidate); the
  // `h` suffix produces a half literal that matches. The 'f' is removed
  // unconditionally on `<digit>.<digit>f` patterns in the body.
  {
    std::string out;
    out.reserve(body.size());
    for (std::size_t k = 0; k < body.size();) {
      // Match `<digit>+.<digit>+f` (or `.<digit>+f`) literal endings.
      auto digit = [](char c) { return c >= '0' && c <= '9'; };
      if (k + 1 < body.size() && body[k] == 'f') {
        // Look back to verify previous char is digit, and before that a
        // chain of digits/dots matching a literal.
        if (k > 0 && (digit(body[k - 1]) ||
                       (k > 1 && body[k - 1] == '.' && digit(body[k - 2])))) {
          // Confirm following char ends the token (paren / comma / op /
          // whitespace). If it's an identifier char or letter, this `f`
          // is part of a longer identifier — don't rewrite.
          const char next = body[k + 1];
          if (!(digit(next) || next == '_' || (next >= 'a' && next <= 'z') ||
                (next >= 'A' && next <= 'Z'))) {
            out.push_back('h');
            ++k;
            continue;
          }
        }
      }
      out.push_back(body[k]);
      ++k;
    }
    body = std::move(out);
  }
  // (1b) Cast every host-float-typed constant-buffer read to half4. The
  // PsConsts struct is shared with the CPU writer so we cannot change
  // its layout. `replaceAll` is fine here because the source never emits
  // the wrapped form first — the rewrite is idempotent for our emitter
  // patterns (no `half4(psConsts.psFloatConst[...])` exists yet).
  replaceAll(body, "psConsts.psFloatConst[",
             "half4(psConsts.psFloatConst[xCAST_OPEN");
  replaceAll(body, "xCAST_OPEN", "");
  // Now every `psConsts.psFloatConst[ ... ]` is `half4(psConsts.psFloatConst[ ... ]`
  // — we still need to close the wrapping `)` after the matching `]`.
  // Walk and insert. (The pattern always appears as `[<expr>]` with no
  // nested brackets in our emitter.)
  {
    std::string out;
    out.reserve(body.size() + 64);
    const std::string marker = "half4(psConsts.psFloatConst[";
    std::size_t p = 0;
    while (p < body.size()) {
      auto hit = body.find(marker, p);
      if (hit == std::string::npos) { out.append(body, p, std::string::npos); break; }
      out.append(body, p, hit - p);
      out.append(marker);
      // copy until the matching ']' then insert ')'
      auto bracket = body.find(']', hit + marker.size());
      if (bracket == std::string::npos) {
        // malformed — bail and keep original
        out.append(body, hit + marker.size(), std::string::npos);
        break;
      }
      out.append(body, hit + marker.size(), bracket - (hit + marker.size()) + 1);
      out.push_back(')');
      p = bracket + 1;
    }
    body = std::move(out);
  }

  // (2) Function signature (before braceOpen): convert texture2d<float>
  // → texture2d<half> so the sample call returns half4 matching the
  // half body.
  std::string sig = source.substr(0, braceOpen);
  replaceAll(sig, "texture2d<float>", "texture2d<half>");
  // (2b) Bare-return path — `fragment float4 dxmt9_fs(` → `fragment half4
  // dxmt9_fs(`. Leaves `fragment FSOut dxmt9_fs(` (struct return) alone;
  // FSOut struct rewrite below handles that case.
  replaceAll(sig, "fragment float4 dxmt9_fs", "fragment half4 dxmt9_fs");

  // (3) Source preamble (before signature) — FSOut struct field types.
  // The struct is emitted just before the fragment function. Rewriting
  // `float4 colorN [[color(N)]]` to `half4 colorN [[color(N)]]` matches
  // the body's half-typed `result.colorN = ...` assignments.
  std::string head = std::move(sig);
  // Walk `float4 color%u [[color(%u)]]` patterns and rewrite. There are
  // at most kMaxRenderTargets such fields per struct, so a simple
  // scan-and-replace is fine.
  {
    std::string out;
    out.reserve(head.size() + 16);
    std::size_t p = 0;
    while (p < head.size()) {
      auto hit = head.find("float4 color", p);
      if (hit == std::string::npos) { out.append(head, p, std::string::npos); break; }
      // Confirm this is an FSOut struct field: the next non-digit char
      // after `color` should be a space followed by `[[color(`.
      auto after = hit + std::string("float4 color").size();
      auto digitsEnd = after;
      while (digitsEnd < head.size() && std::isdigit(static_cast<unsigned char>(head[digitsEnd]))) {
        ++digitsEnd;
      }
      if (digitsEnd < head.size() && head.compare(digitsEnd, std::strlen(" [[color("), " [[color(") == 0) {
        // It's a field declaration. Rewrite.
        out.append(head, p, hit - p);
        out.append("half4 color");
        out.append(head, after, digitsEnd - after);
        p = digitsEnd;
      } else {
        // Not a field — copy through.
        out.append(head, p, hit - p + 1);
        p = hit + 1;
      }
    }
    head = std::move(out);
  }

  return head + body + source.substr(braceClose);
}

std::string makeTranslatedFragmentSource(const ShaderRef& shader,
                                         const ShaderSourceContext& context) {
  auto source = translateSpirvToMsl(translateD3DBytecodeToSpirv(shader, false, context), context, false);
  if (shaders::fsHalfPrecisionEnabled()) {
    source = applyFsHalfPrecisionRewrite(std::move(source));
  }
  return source;
}

}  // namespace dxmt9::translator::detail_

namespace dxmt9::translator {

std::string makeTranslatedVertexSource(const ::dxmt9::core::ShaderRef& shader,
                                        const ::dxmt9::drawshader::ShaderSourceContext& context) {
  return detail_::makeTranslatedVertexSource(shader, context);
}

std::string makeTranslatedFragmentSource(const ::dxmt9::core::ShaderRef& shader,
                                          const ::dxmt9::drawshader::ShaderSourceContext& context) {
  return detail_::makeTranslatedFragmentSource(shader, context);
}

::dxmt9::shaders::VSOutLayout collectTranslatedFragmentVaryingLiveness(
    const ::dxmt9::core::ShaderRef& shader,
    const ::dxmt9::drawshader::ShaderSourceContext& context) {
  auto module = detail_::translateD3DBytecodeToSpirv(shader, /*vertex=*/false, context);
  return detail_::collectFragmentVaryingLiveness(module);
}

namespace test {

::dxmt9::d3d9bc::SpirvModule decodeD3DBytecodeForTest(const ::dxmt9::core::ShaderRef& shader,
                                                       bool vertex,
                                                       const ::dxmt9::core::fixture::DrawDesc& desc) {
  return detail_::translateD3DBytecodeToSpirv(
      shader, vertex, ::dxmt9::drawshader::makeShaderSourceContext(desc));
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
