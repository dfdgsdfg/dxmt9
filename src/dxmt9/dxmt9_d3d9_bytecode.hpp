#pragma once

// D3D9 shader bytecode token + opcode tables, plus the intermediate SPIR-V
// representation used by dxmt9's translator. Header-only — lifted out of
// backend_metal.mm's anonymous namespace so the translator body and future
// external consumers can share the definitions. The translator IMPL still
// lives in backend_metal.mm for now; this extraction just gives the types
// + constants a stable home.

#include "dxmt9/core.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace dxmt9::d3d9bc {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

enum class D3DShaderStage : u32 { Vertex, Pixel };

enum class D3DRegisterKind : u32 {
  Temp,
  Input,
  ConstFloat,
  Address,
  RastOut,
  AttrOut,
  TexCoordOut,
  ConstInt,
  ColorOut,
  DepthOut,
  Sampler,
  ConstBool,
  Loop,
  MiscType,
  Predicate,
  Unknown,
};

struct D3DRegisterRef {
  D3DRegisterKind kind = D3DRegisterKind::Unknown;
  u32 index = 0;
  // D3D9 source operands that use relative addressing (`c[a0+N]`,
  // `c[aL+N]`) carry an extra token after the operand DWORD that
  // encodes the address-register source. 0 == no relative addressing.
  // The token is the raw bytecode word; consumers decode kind/index/
  // swizzle from it on demand.
  u32 relAddrToken = 0;
};

struct D3DDecodedInstruction {
  u32 opcode = 0;
  u32 controls = 0;
  bool predicated = false;
  std::vector<u32> operands;
  // Parallel to `operands`. Non-zero entries hold the rel-addr DWORD
  // that immediately followed the corresponding operand token in the
  // bytecode stream. Empty (or all-zero) when no operand has rel-addr.
  std::vector<u32> relAddrTokens;
};

struct SpirvModule {
  std::vector<u32> words;
  u64 hash = 0;
  bool usesTexture = false;
  D3DShaderStage stage = D3DShaderStage::Vertex;
  u32 major = 0;
  u32 minor = 0;
  std::array<::dxmt9::core::TextureType, ::dxmt9::core::kMaxSamplers> samplerTextureTypes{};
  std::vector<D3DDecodedInstruction> instructions;
};

// SM2/SM3 shader opcodes (D3DSIO_*). Only the opcode space reachable from
// D3D9 vertex/pixel shaders is listed.
inline constexpr u32 kD3DSIO_NOP = 0;
inline constexpr u32 kD3DSIO_MOV = 1;
inline constexpr u32 kD3DSIO_ADD = 2;
inline constexpr u32 kD3DSIO_SUB = 3;
inline constexpr u32 kD3DSIO_MAD = 4;
inline constexpr u32 kD3DSIO_MUL = 5;
inline constexpr u32 kD3DSIO_RCP = 6;
inline constexpr u32 kD3DSIO_RSQ = 7;
inline constexpr u32 kD3DSIO_DP3 = 8;
inline constexpr u32 kD3DSIO_DP4 = 9;
inline constexpr u32 kD3DSIO_MIN = 10;
inline constexpr u32 kD3DSIO_MAX = 11;
inline constexpr u32 kD3DSIO_SLT = 12;
inline constexpr u32 kD3DSIO_SGE = 13;
inline constexpr u32 kD3DSIO_EXP = 14;
inline constexpr u32 kD3DSIO_LOG = 15;
inline constexpr u32 kD3DSIO_LRP = 18;
inline constexpr u32 kD3DSIO_FRC = 19;
inline constexpr u32 kD3DSIO_M4x4 = 20;
inline constexpr u32 kD3DSIO_M4x3 = 21;
inline constexpr u32 kD3DSIO_M3x4 = 22;
inline constexpr u32 kD3DSIO_M3x3 = 23;
inline constexpr u32 kD3DSIO_M3x2 = 24;
inline constexpr u32 kD3DSIO_CALL = 25;
inline constexpr u32 kD3DSIO_CALLNZ = 26;
inline constexpr u32 kD3DSIO_LOOP = 27;
inline constexpr u32 kD3DSIO_RET = 28;
inline constexpr u32 kD3DSIO_ENDLOOP = 29;
inline constexpr u32 kD3DSIO_LABEL = 30;
inline constexpr u32 kD3DSIO_DCL = 31;
inline constexpr u32 kD3DSIO_POW = 32;
inline constexpr u32 kD3DSIO_CRS = 33;
inline constexpr u32 kD3DSIO_SGN = 34;
inline constexpr u32 kD3DSIO_ABS = 35;
inline constexpr u32 kD3DSIO_NRM = 36;
inline constexpr u32 kD3DSIO_SINCOS = 37;
inline constexpr u32 kD3DSIO_REP = 38;
inline constexpr u32 kD3DSIO_ENDREP = 39;
inline constexpr u32 kD3DSIO_IF = 40;
inline constexpr u32 kD3DSIO_IFC = 41;
inline constexpr u32 kD3DSIO_ELSE = 42;
inline constexpr u32 kD3DSIO_ENDIF = 43;
inline constexpr u32 kD3DSIO_BREAK = 44;
inline constexpr u32 kD3DSIO_BREAKC = 45;
inline constexpr u32 kD3DSIO_MOVA = 46;
inline constexpr u32 kD3DSIO_DEFB = 47;
inline constexpr u32 kD3DSIO_DEFI = 48;
inline constexpr u32 kD3DSIO_TEXCOORD = 64;
inline constexpr u32 kD3DSIO_TEXKILL = 65;
inline constexpr u32 kD3DSIO_TEX = 66;
inline constexpr u32 kD3DSIO_TEXBEM = 67;
inline constexpr u32 kD3DSIO_TEXBEML = 68;
inline constexpr u32 kD3DSIO_TEXREG2AR = 69;
inline constexpr u32 kD3DSIO_TEXREG2GB = 70;
inline constexpr u32 kD3DSIO_TEXM3x2PAD = 71;
inline constexpr u32 kD3DSIO_TEXM3x2TEX = 72;
inline constexpr u32 kD3DSIO_TEXM3x3PAD = 73;
inline constexpr u32 kD3DSIO_TEXM3x3TEX = 74;
inline constexpr u32 kD3DSIO_TEXM3x3DIFF = 75;
inline constexpr u32 kD3DSIO_TEXM3x3SPEC = 76;
inline constexpr u32 kD3DSIO_TEXM3x3VSPEC = 77;
inline constexpr u32 kD3DSIO_EXPP = 78;
inline constexpr u32 kD3DSIO_LOGP = 79;
inline constexpr u32 kD3DSIO_CND = 80;
inline constexpr u32 kD3DSIO_DEF = 81;
inline constexpr u32 kD3DSIO_TEXREG2RGB = 82;
inline constexpr u32 kD3DSIO_TEXDP3TEX = 83;
inline constexpr u32 kD3DSIO_TEXM3x2DEPTH = 84;
inline constexpr u32 kD3DSIO_TEXDP3 = 85;
inline constexpr u32 kD3DSIO_TEXM3x3 = 86;
inline constexpr u32 kD3DSIO_TEXDEPTH = 87;
inline constexpr u32 kD3DSIO_CMP = 88;
inline constexpr u32 kD3DSIO_BEM = 89;
inline constexpr u32 kD3DSIO_DP2ADD = 90;
inline constexpr u32 kD3DSIO_DSX = 91;
inline constexpr u32 kD3DSIO_DSY = 92;
inline constexpr u32 kD3DSIO_TEXLDD = 93;
inline constexpr u32 kD3DSIO_SETP = 94;
inline constexpr u32 kD3DSIO_TEXLDL = 95;
inline constexpr u32 kD3DSIO_BREAKP = 96;
inline constexpr u32 kD3DSIO_PHASE = 0xfffdu;
inline constexpr u32 kD3DSIO_COMMENT = 0xfffeu;
inline constexpr u32 kD3DSIO_END = 0xffffu;

// Register type codes (D3DSPR_*).
inline constexpr u32 kD3DSPR_TEMP = 0;
inline constexpr u32 kD3DSPR_INPUT = 1;
inline constexpr u32 kD3DSPR_CONST = 2;
inline constexpr u32 kD3DSPR_ADDR = 3;
inline constexpr u32 kD3DSPR_RASTOUT = 4;
inline constexpr u32 kD3DSPR_ATTROUT = 5;
inline constexpr u32 kD3DSPR_TEXCRDOUT = 6;
inline constexpr u32 kD3DSPR_CONSTINT = 7;
inline constexpr u32 kD3DSPR_COLOROUT = 8;
inline constexpr u32 kD3DSPR_DEPTHOUT = 9;
inline constexpr u32 kD3DSPR_SAMPLER = 10;
inline constexpr u32 kD3DSPR_CONSTBOOL = 14;
inline constexpr u32 kD3DSPR_LOOP = 15;
inline constexpr u32 kD3DSPR_MISCTYPE = 17;
inline constexpr u32 kD3DSPR_PREDICATE = 19;

// Dcl token usage/usage-index shifts.
inline constexpr u32 kD3DSP_DCL_USAGE_SHIFT = 0u;
inline constexpr u32 kD3DSP_DCL_USAGE_MASK = 0x0000000fu;
inline constexpr u32 kD3DSP_DCL_USAGEINDEX_SHIFT = 16u;
inline constexpr u32 kD3DSP_DCL_USAGEINDEX_MASK = 0x000f0000u;

}  // namespace dxmt9::d3d9bc
