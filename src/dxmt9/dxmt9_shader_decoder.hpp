#pragma once

// Internal interface between dxmt9_shader_decoder.cpp (D3D9 bytecode parsing)
// and dxmt9_shader_metal_ir.cpp (Metal source emission). The public translator
// API stays in dxmt9_shader_translator.hpp; this header is intentionally
// scoped to the two translator implementation files.
//
// All symbols here live in `dxmt9::translator::detail_`, mirroring the namespace
// the original monolithic dxmt9_shader_translator.cpp used. Function-name
// stability matters because the public test seam in
// dxmt9_shader_translator.hpp forwards into these helpers.

#include "dxmt9_d3d9_bytecode.hpp"
#include "dxmt9_draw_shader.hpp"
#include "dxmt9_ffp_shaders.hpp"
#include "dxmt9/core.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace dxmt9::translator::detail_ {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using i32 = std::int32_t;

using ::dxmt9::d3d9bc::D3DDecodedInstruction;
using ::dxmt9::d3d9bc::D3DRegisterKind;
using ::dxmt9::d3d9bc::D3DRegisterRef;
using ::dxmt9::d3d9bc::D3DShaderStage;
using ::dxmt9::d3d9bc::SpirvModule;
using ShaderRef = ::dxmt9::core::ShaderRef;
using ShaderSourceContext = ::dxmt9::drawshader::ShaderSourceContext;
using VertexShaderInputLayout = ::dxmt9::ffp::VertexShaderInputLayout;

// --- Token-level decode primitives -----------------------------------------

std::array<u8, 4> decodeSwizzle(u32 token);
u32 decodeRegisterType(u32 token);
u32 decodeRegisterIndex(u32 token);
u32 decodeSourceModifier(u32 token);
u32 decodeDestModifier(u32 token);
u32 decodeWriteMask(u32 token);
bool tokenHasRelativeAddressing(u32 token);
D3DRegisterKind decodeRegisterKind(u32 type, D3DShaderStage stage);
D3DRegisterRef decodeRegisterRef(u32 token, D3DShaderStage stage);
u32 decodeLabelIndex(u32 token);

// --- Opcode classification --------------------------------------------------

std::string opcodeName(u32 opcode);
u32 fixedOperandCount(u32 opcode);
bool opcodeWritesFirstOperand(u32 opcode);
bool isConstantRegisterKind(D3DRegisterKind kind);
bool isTextureSampleOpcode(u32 opcode);
u32 matrixConstantRows(u32 opcode);
u32 textureSamplerIndex(const D3DDecodedInstruction& instruction, D3DShaderStage stage);

// --- Decoded analysis structures shared with the IR layer -------------------

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

struct PixelInputSemantic {
  bool valid = false;
  u32 usage = 0;
  u32 usageIndex = 0;
};

using PixelInputSemantics = std::array<PixelInputSemantic, 16>;

struct ConstantUsage {
  bool mutableConstants = false;
  bool hasFloat = false;
  bool hasInt = false;
  bool hasBool = false;
  u32 floatCount = 0;
  u32 intCount = 0;
  u32 boolCount = 0;
};

struct FlowBlock {
  u32 opcode = 0;
  bool sawElse = false;
};

// DCL-usage codes that the translator special-cases.
constexpr u32 kD3DDeclUsagePosition = 0u;
constexpr u32 kD3DDeclUsagePSize = 4u;
constexpr u32 kD3DDeclUsageTexcoord = 5u;
constexpr u32 kD3DDeclUsagePositionT = 9u;
constexpr u32 kD3DDeclUsageColor = 10u;
constexpr u32 kD3DDeclUsageFog = 11u;

// --- Module-level analysis passes ------------------------------------------

std::optional<VertexOutputMapping> vertexOutputMapping(const D3DRegisterRef& reg,
                                                       const VertexOutputSemantics* semantics);

std::optional<VertexShaderInputLayout> decodeVertexShaderInputLayout(const SpirvModule& module,
                                                                     const ShaderSourceContext& context);
VertexOutputSemantics collectVertexOutputSemantics(const SpirvModule& module);
PixelInputSemantics collectPixelInputSemantics(const SpirvModule& module);
u32 pixelColorOutputCount(const SpirvModule& module);
bool pixelWritesDepth(const SpirvModule& module);
std::array<bool, ::dxmt9::core::kMaxSamplers> collectPixelSamplerUsage(const SpirvModule& module,
                                                                       const ShaderSourceContext& context);
void noteConstantUsage(ConstantUsage& usage, D3DRegisterKind kind, u32 index);
ConstantUsage collectConstantUsage(const SpirvModule& module);
bool shaderUsesPredicateRegisters(const SpirvModule& module);

// --- Top-level decoder entry point -----------------------------------------

// Parses a D3D9 vertex or pixel shader bytecode blob into the SpirvModule
// intermediate consumed by the Metal IR layer. `vertex==true` selects the
// vertex stage; `context` supplies clip-plane mask, sample count, and the
// vertex declaration used to compute the bytecode hash and stride.
SpirvModule translateD3DBytecodeToSpirv(const ShaderRef& shader,
                                        bool vertex,
                                        const ShaderSourceContext& context);

}  // namespace dxmt9::translator::detail_
