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
using ::dxmt9::shaders::kArgbufHybridBindSlot;

namespace {

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
  // Relative-addressing helper: translates the rel-addr DWORD attached
  // to a source operand into the address-register expression that
  // indexes the constant array. vs_2_0/2_x can only use a0; vs_3_0+
  // additionally allows aL inside a loop body. Anything else falls back
  // to a0 (best effort — the bytecode would have failed validation
  // before reaching the translator).
  auto relAddrExpression = [](u32 relAddrToken) -> std::string {
    if (relAddrToken == 0u) {
      return {};
    }
    const auto relReg = decodeRegisterRef(relAddrToken, D3DShaderStage::Vertex);
    return relReg.kind == D3DRegisterKind::Loop ? "aL" : "a0";
  };
  switch (reg.kind) {
    case D3DRegisterKind::Temp:
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

void emitConstantBindings(std::ostringstream& out, bool vertexStage, const ConstantUsage& usage) {
  // Constant pointers route to the per-stage category buffer (VsConsts at slot 0
  // for vertex, PsConsts at slot 0 for fragment). See specs/backend/draw-uniforms.
  const char* container = vertexStage ? "vsConsts" : "psConsts";
  const char* floatMember = vertexStage ? "vsFloatConst" : "psFloatConst";
  const char* intMember = vertexStage ? "vsIntConst" : "psIntConst";
  const char* boolMember = vertexStage ? "vsBoolConst" : "psBoolConst";

  // Each constant category gets pointer-aliasing whenever it is read
  // through relative addressing — `c[a0+N]` must address the full
  // 256-vec4 (16-int4, 16-bool) range, which the local-copy fast path
  // can't guarantee. DEF/DEFI/DEFB writes then become read-only;
  // combining DEF with indexed reads is not observed in any tracked
  // SM2/SM3 shader, so the trade-off is correct on the common case.
  const bool aliasFloat = !usage.mutableConstants || usage.hasIndexedFloat;
  const bool aliasInt = !usage.mutableConstants || usage.hasIndexedInt;
  const bool aliasBool = !usage.mutableConstants || usage.hasIndexedBool;

  if (aliasFloat) {
    out << "  constant float4* cFloat = " << container << "." << floatMember << ";\n";
  } else {
    out << "  float4 cFloat[" << std::max(1u, usage.floatCount) << "];\n";
    out << "  for (uint i = 0; i < " << usage.floatCount << "; ++i) { cFloat[i] = " << container << "."
        << floatMember << "[i]; }\n";
  }
  if (aliasInt) {
    out << "  constant int4* cInt = " << container << "." << intMember << ";\n";
  } else {
    out << "  int4 cInt[" << std::max(1u, usage.intCount) << "];\n";
    out << "  for (uint i = 0; i < " << usage.intCount << "; ++i) { cInt[i] = " << container << "." << intMember
        << "[i]; }\n";
  }
  if (aliasBool) {
    out << "  constant uint* cBool = " << container << "." << boolMember << ";\n";
  } else {
    out << "  uint cBool[" << std::max(1u, usage.boolCount) << "];\n";
    out << "  for (uint i = 0; i < " << usage.boolCount << "; ++i) { cBool[i] = " << container << "." << boolMember
        << "[i]; }\n";
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

// R-BACK-12.22..12.26 MSL routing — emit local `texN` / `sampN` aliases
// that copy the texture/sampler handles out of the bound argument buffer
// at slot 30. Body code is unchanged because it still references `texN`
// and `sampN` by name; only the source of those names moves.
void emitFragmentTextureAliasesFromArgbuf(std::ostringstream& out,
                                          const std::array<bool, kMaxSamplers>& samplerUsage) {
  for (u32 stage = 0; stage < kMaxSamplers; ++stage) {
    if (!samplerUsage[stage]) {
      continue;
    }
    out << "  texture2d<float> tex" << stage << " = abuf->textures[" << stage << "];\n";
    out << "  sampler samp" << stage << " = abuf->samplers[" << stage << "];\n";
  }
}

std::string translateSpirvToMsl(const SpirvModule& module,
                                const ShaderSourceContext& context,
                                bool vertex) {
  std::ostringstream out;
  const bool argbufHybrid = context.argbufHybridMode;
  if (argbufHybrid) {
    out << makeShaderPreludeArgbufHybrid(context.clipPlaneMask != 0);
  } else {
    out << makeShaderPrelude(context.clipPlaneMask != 0);
  }
  if (vertex) {
    const auto inputLayout = decodeVertexShaderInputLayout(module, context);
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
    if (argbufHybrid) {
      // R-BACK-12.22..12.26 MSL routing — single argbuf at slot 30
      // replaces slots 0/3. Vertex stream and DrawVolatile stay direct
      // (design.md §11.4). Re-alias `vsConsts`/`ffpVs` references off
      // the argbuf so downstream emission continues to read by name.
      out << "vertex VSOut dxmt9_vs(uint vid [[vertex_id]],\n";
      out << "                     constant ArgbufLayout const* abuf [[buffer("
          << kArgbufHybridBindSlot << ")]],\n";
      out << "                     device const uchar* stream0 [[buffer(1)]],\n";
      out << "                     constant DrawVolatile& drawVolatile [[buffer(5)]]) {\n";
      out << "  constant VsConsts& vsConsts = *abuf->vsConsts;\n";
      out << "  constant FfpVsConsts& ffpVs = *abuf->ffpVs;\n";
    } else {
      out << "vertex VSOut dxmt9_vs(uint vid [[vertex_id]],\n";
      out << "                     constant VsConsts& vsConsts [[buffer(0)]],\n";
      out << "                     device const uchar* stream0 [[buffer(1)]],\n";
      out << "                     constant FfpVsConsts& ffpVs [[buffer(3)]],\n";
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
      out << "  out.color = outColor;\n";
      out << "  out.secondaryColor = outSecondaryColor;\n";
      const auto maxTex = shaders::vsoutMaxTexcoord();
      for (size_t i = 0; i < maxTex; ++i) {
        out << "  out.texcoord" << i << " = outTexcoord[" << i << "];\n";
      }
      if (shaders::vsoutEmitFogFactor()) {
        out << "  out.fogFactor = outFogFactor;\n";
      }
      if (shaders::vsoutEmitPointSize()) {
        out << "  out.pointSize = outPointSize;\n";
      }
      if (context.clipPlaneMask != 0) {
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
      out << "  const uint stride = drawVolatile.vertexStreamStride != 0u ? drawVolatile.vertexStreamStride : "
          << inputLayout->stride << "u;\n";
      out << "  const int vertexIndex = max(0, int(vid) + drawVolatile.vertexBaseIndex);\n";
      out << "  const uint base = drawVolatile.vertexStreamOffset + uint(vertexIndex) * stride;\n";
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
          case kD3DDeclTypeUByte4:
            out << "  vin[" << i << "] = dxmt9_load_u8x4(stream0, base + " << binding.offset << "u);\n";
            break;
          case kD3DDeclTypeShort2:
            out << "  vin[" << i << "] = float4(dxmt9_load_i16x2(stream0, base + " << binding.offset
                << "u), 0.0f, 1.0f);\n";
            break;
          case kD3DDeclTypeShort4:
            out << "  vin[" << i << "] = dxmt9_load_i16x4(stream0, base + " << binding.offset << "u);\n";
            break;
          case kD3DDeclTypeUByte4N:
            out << "  vin[" << i << "] = dxmt9_load_u8x4_unorm(stream0, base + " << binding.offset << "u);\n";
            break;
          case kD3DDeclTypeShort2N:
            out << "  vin[" << i << "] = float4(dxmt9_load_i16x2_snorm(stream0, base + " << binding.offset
                << "u), 0.0f, 1.0f);\n";
            break;
          case kD3DDeclTypeShort4N:
            out << "  vin[" << i << "] = dxmt9_load_i16x4_snorm(stream0, base + " << binding.offset << "u);\n";
            break;
          case kD3DDeclTypeUShort2N:
            out << "  vin[" << i << "] = float4(dxmt9_load_u16x2_unorm(stream0, base + " << binding.offset
                << "u), 0.0f, 1.0f);\n";
            break;
          case kD3DDeclTypeUShort4N:
            out << "  vin[" << i << "] = dxmt9_load_u16x4_unorm(stream0, base + " << binding.offset << "u);\n";
            break;
          case kD3DDeclTypeUDec3:
            out << "  vin[" << i << "] = dxmt9_load_udec3(stream0, base + " << binding.offset << "u);\n";
            break;
          case kD3DDeclTypeDec3N:
            out << "  vin[" << i << "] = dxmt9_load_dec3n(stream0, base + " << binding.offset << "u);\n";
            break;
          case kD3DDeclTypeFloat16_2:
            out << "  vin[" << i << "] = float4(dxmt9_load_f16x2(stream0, base + " << binding.offset
                << "u), 0.0f, 1.0f);\n";
            break;
          case kD3DDeclTypeFloat16_4:
            out << "  vin[" << i << "] = dxmt9_load_f16x4(stream0, base + " << binding.offset << "u);\n";
            break;
          default:
            break;
        }
      }
    }
	    out << "  int a0 = 0;\n";
	    out << "  int aL = 0;\n";
	    const auto constantUsage = collectConstantUsage(module);
	    // R-SHADER-AIR-SIZE: same alloca-eliding trim as the FS path —
	    // shrink `r[]` to the VS's actual max-written-Temp index. Apple
	    // promotes the smaller array to scalar SSA registers instead of
	    // a 512 B stack frame, freeing GPU register pressure for the
	    // vertex stage. Indexed temp reads fall back to r[32] via the
	    // 31-floor in `collectConstantUsage`.
	    const u32 tempCount =
	        static_cast<u32>(std::max<std::int32_t>(1, constantUsage.maxTempIndex + 1));
	    out << "  float4 r[" << tempCount << "];\n";
	    out << "  for (uint i = 0; i < " << tempCount
	        << "u; ++i) { r[i] = float4(0.0f); }\n";
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
        auto reg = decodeRegisterRef(token, module.stage);
        if (index < instruction.relAddrTokens.size()) {
          reg.relAddrToken = instruction.relAddrTokens[index];
        }
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
    {
      const auto maxTex = shaders::vsoutMaxTexcoord();
      for (size_t i = 0; i < maxTex; ++i) {
        out << "  out.texcoord" << i << " = outTexcoord[" << i << "];\n";
      }
    }
    if (shaders::vsoutEmitFogFactor()) {
      out << "  out.fogFactor = outFogFactor;\n";
    }
    if (shaders::vsoutEmitPointSize()) {
      out << "  out.pointSize = outPointSize;\n";
    }
    out << "  out.position.xy += ffpVs.halfPixelFixup * out.position.w;\n";
    if (context.clipPlaneMask != 0) {
      out << "  for (uint i = 0; i < 6; ++i) {\n";
      out << "    if ((ffpVs.clipPlaneMask & (1u << i)) != 0u) {\n";
      out << "      out.clipDistance[i] = dot(ffpVs.clipPlanes[i], out.position);\n";
      out << "    }\n";
      out << "  }\n";
    }
    out << "  return out;\n";
    out << "}\n";
    out << "// decoded d3d hash " << module.hash << "\n";
    return out.str();
  }

  const auto samplerUsage = collectPixelSamplerUsage(module, context);
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
    if (argbufHybrid) {
      // R-BACK-12.22..12.26 MSL routing — single argbuf at slot 30
      // replaces slots 0/3 plus per-stage texture/sampler slots.
      // Re-aliases on entry so the downstream body code continues to
      // read `psConsts.X`, `ffpPs.X`, `texN.sample(sampN, ...)` by name.
      out << "fragment " << fragmentReturnType
          << " dxmt9_fs(VSOut in [[stage_in]],\n";
      out << "                     constant ArgbufLayout const* abuf [[buffer("
          << kArgbufHybridBindSlot << ")]]) {\n";
      out << "  constant PsConsts& psConsts = *abuf->psConsts;\n";
      out << "  constant FfpPsConsts& ffpPs = *abuf->ffpPs;\n";
      emitFragmentTextureAliasesFromArgbuf(out, samplerUsage);
    } else {
      out << "fragment " << fragmentReturnType
          << " dxmt9_fs(VSOut in [[stage_in]],\n";
      out << "                     constant PsConsts& psConsts [[buffer(0)]],\n";
      out << "                     constant FfpPsConsts& ffpPs [[buffer(3)]], ";
      emitFragmentTextureArguments(out, samplerUsage);
      out << ") {\n";
    }
  } else {
    if (argbufHybrid) {
      out << "fragment " << fragmentReturnType
          << " dxmt9_fs(VSOut in [[stage_in]],\n";
      out << "                     constant ArgbufLayout const* abuf [[buffer("
          << kArgbufHybridBindSlot << ")]]) {\n";
      out << "  constant PsConsts& psConsts = *abuf->psConsts;\n";
      out << "  constant FfpPsConsts& ffpPs = *abuf->ffpPs;\n";
    } else {
      out << "fragment " << fragmentReturnType
          << " dxmt9_fs(VSOut in [[stage_in]],\n";
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
  const auto constantUsage = collectConstantUsage(module);
  // R-SHADER-AIR-SIZE: size r[] / outColor[] to the shader's actual
  // max-written-Temp / max-written-oC index instead of the spec maxima
  // (32 / 4). Apple's MSL → AIR keeps the oversized alloca because of
  // dynamic-index patterns (`r[a0+N]`, etc.); shrinking the array gives
  // the GPU register allocator room to promote remaining slots out of
  // the 512 B (32 × float4) thread-local stack frame, raising
  // occupancy. Both counts have a floor of 1 — every fragment shader
  // writes at least oC0, and the per-frame init loop needs at least
  // one element to be well-formed.
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
	  out << "  float4 r[" << tempCount << "];\n";
	  out << "  for (uint i = 0; i < " << tempCount
	      << "u; ++i) { r[i] = float4(0.0f); }\n";
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
      auto reg = decodeRegisterRef(token, module.stage);
      if (index < instruction.relAddrTokens.size()) {
        reg.relAddrToken = instruction.relAddrTokens[index];
      }
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
