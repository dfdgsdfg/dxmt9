#include "dxmt9/assert.hpp"
#include "dxmt9/copy_materialization_ledger.hpp"
#include "dxmt9/core.hpp"
#include "dxmt9/dxmt9_device.hpp"
#include "dxmt9/dxmt9_d3d9_bytecode.hpp"
#include "../dxmt9/dxmt9_perf_counters.hpp"
#include "util/config/config.hpp"
#include "util/log/log.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <type_traits>
#include <vector>

namespace dxmt9::core {

using detail::DrawParamInlineStorage;
using detail::DrawPayloadArenaStorage;
using detail::kDrawRunInlineParamCapacity;
using detail::kDrawRunInlinePayloadCapacity;
namespace bc = ::dxmt9::d3d9bc;

// Split from core.cpp; keep this unit private to the D3D9 frontend.
namespace {

constexpr u64 kFnvOffset = 1469598103934665603ull;
constexpr u64 kFnvPrime = 1099511628211ull;

class PerfScope {
 public:
  explicit PerfScope(void (*record)(std::uint64_t)) : record_(record) {
    if (record_) {
      start_ = std::chrono::steady_clock::now();
    }
  }
  ~PerfScope() {
    if (!record_) return;
    const auto end = std::chrono::steady_clock::now();
    record_(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start_).count()));
  }
  PerfScope(const PerfScope&) = delete;
  PerfScope& operator=(const PerfScope&) = delete;

 private:
  void (*record_)(std::uint64_t) = nullptr;
  std::chrono::steady_clock::time_point start_;
};

u64 hashCombine(u64 seed, u64 value) {
  seed ^= value;
  seed *= kFnvPrime;
  return seed;
}

template <typename T> u64 hashTrivial(const T &value) {
  static_assert(std::is_trivially_copyable_v<T>);
  const auto *bytes =
      reinterpret_cast<const std::byte *>(std::addressof(value));
  u64 hash = kFnvOffset;
  for (size_t i = 0; i < sizeof(T); ++i) {
    hash ^= static_cast<u64>(bytes[i]);
    hash *= kFnvPrime;
  }
  return hash;
}

u64 hashBytes(std::span<const std::byte> bytes) {
  u64 hash = kFnvOffset;
  for (const auto byte : bytes) {
    hash ^= static_cast<u64>(byte);
    hash *= kFnvPrime;
  }
  return hash;
}

u64 hashStateDigest(std::size_t count, u64 rollingHash) {
  u64 hash = hashCombine(kFnvOffset, static_cast<u64>(count));
  hash = hashCombine(hash, rollingHash);
  return hash;
}

template <std::size_t MaxEntries>
u64 hashStateMap(const StateValueTable<MaxEntries> &values) {
  return hashStateDigest(values.size(), values.rollingHash);
}

template <std::size_t MaxEntries>
FlatStateSet<MaxEntries> makeFlatStateSet(const auto &values) {
  FlatStateSet<MaxEntries> set{};
  set.hash = hashStateMap(values);
  set.overflow = values.size() > MaxEntries;
  for (const auto &entry : values) {
    if (set.count >= MaxEntries) {
      continue;
    }
    set.entries[set.count++] =
        FlatStateEntry{.state = entry.first, .value = entry.second};
  }
  std::sort(set.entries.begin(), set.entries.begin() + set.count,
            [](const FlatStateEntry &a, const FlatStateEntry &b) {
              return a.state < b.state;
            });
  return set;
}

constexpr auto kFlatRenderStatePreservedKeys = std::to_array<u32>({
    RS_Z_ENABLE,
    RS_FILL_MODE,
    9u,   // D3DRS_SHADEMODE.
    RS_Z_WRITE_ENABLE,
    RS_ALPHA_TEST_ENABLE,
    16u,  // D3DRS_LASTPIXEL.
    RS_SRC_BLEND,
    RS_DEST_BLEND,
    RS_CULL_MODE,
    RS_Z_FUNC,
    RS_ALPHA_REF,
    RS_ALPHA_FUNC,
    26u,  // D3DRS_DITHERENABLE.
    RS_ALPHABLEND_ENABLE,
    RS_FOG_ENABLE,
    RS_SPECULAR_ENABLE,
    RS_FOG_COLOR,
    RS_FOG_TABLE_MODE,
    RS_FOG_START,
    RS_FOG_END,
    RS_FOG_DENSITY,
    RS_RANGE_FOG,
    RS_STENCIL_ENABLE,
    RS_STENCIL_FAIL,
    RS_STENCIL_ZFAIL,
    RS_STENCIL_PASS,
    RS_STENCIL_FUNC,
    RS_STENCIL_REF,
    RS_STENCIL_MASK,
    RS_STENCIL_WRITEMASK,
    RS_TEXTURE_FACTOR,
    RS_WRAP0,
    RS_WRAP1,
    RS_WRAP2,
    RS_WRAP3,
    RS_WRAP4,
    RS_WRAP5,
    RS_WRAP6,
    RS_WRAP7,
    RS_CLIPPING,
    RS_LIGHTING,
    RS_AMBIENT,
    RS_FOG_FROM_VERTEX,
    141u,  // D3DRS_COLORVERTEX.
    RS_LOCAL_VIEWER,
    RS_NORMALIZE_NORMALS,
    RS_DIFFUSE_MATERIAL_SOURCE,
    RS_SPECULAR_MATERIAL_SOURCE,
    RS_AMBIENT_MATERIAL_SOURCE,
    RS_EMISSIVE_MATERIAL_SOURCE,
    RS_VERTEX_BLEND,
    RS_CLIP_PLANE_ENABLE,
    RS_POINTSIZE,
    RS_POINTSIZE_MIN,
    RS_POINT_SPRITE_ENABLE,
    RS_POINT_SCALE_ENABLE,
    RS_POINTSCALE_A,
    RS_POINTSCALE_B,
    RS_POINTSCALE_C,
    161u,  // D3DRS_MULTISAMPLEANTIALIAS.
    162u,  // D3DRS_MULTISAMPLEMASK.
    163u,  // D3DRS_PATCHEDGESTYLE.
    RS_POINTSIZE_MAX,
    RS_INDEXED_VERTEX_BLEND_ENABLE,
    RS_COLOR_WRITE_ENABLE,
    170u,  // D3DRS_TWEENFACTOR.
    RS_BLEND_OP,
    172u,  // D3DRS_POSITIONDEGREE.
    173u,  // D3DRS_NORMALDEGREE.
    RS_SCISSOR_TEST_ENABLE,
    RS_SLOPE_SCALE_DEPTH_BIAS,
    176u,  // D3DRS_ANTIALIASEDLINEENABLE.
    178u,  // D3DRS_MINTESSELLATIONLEVEL.
    179u,  // D3DRS_MAXTESSELLATIONLEVEL.
    180u,  // D3DRS_ADAPTIVETESS_X.
    RS_ADAPTIVETESS_Y,
    182u,  // D3DRS_ADAPTIVETESS_Z.
    183u,  // D3DRS_ADAPTIVETESS_W.
    184u,  // D3DRS_ENABLEADAPTIVETESSELLATION.
    RS_TWO_SIDED_STENCIL_MODE,
    RS_STENCIL_CCW_FAIL,
    RS_STENCIL_CCW_ZFAIL,
    RS_STENCIL_CCW_PASS,
    RS_STENCIL_CCW_FUNC,
    RS_COLOR_WRITE_ENABLE1,
    RS_COLOR_WRITE_ENABLE2,
    RS_COLOR_WRITE_ENABLE3,
    RS_BLEND_FACTOR,
    RS_SRGB_WRITE_ENABLE,
    RS_DEPTH_BIAS,
    RS_WRAP8,
    RS_WRAP9,
    RS_WRAP10,
    RS_WRAP11,
    RS_WRAP12,
    RS_WRAP13,
    RS_WRAP14,
    RS_WRAP15,
    RS_SEPARATE_ALPHA_BLEND_ENABLE,
    RS_SRC_BLEND_ALPHA,
    RS_DEST_BLEND_ALPHA,
    RS_BLEND_OP_ALPHA,
});

template <std::size_t MaxEntries, typename StateTable>
void appendFlatStateEntryIfPresent(FlatStateSet<MaxEntries> &set,
                                   const StateTable &values, u32 state) {
  for (u32 i = 0; i < set.count; ++i) {
    if (set.entries[i].state == state) {
      return;
    }
  }
  if (set.count >= MaxEntries || !values.contains(state)) {
    return;
  }
  set.entries[set.count++] =
      FlatStateEntry{.state = state, .value = values.at(state)};
}

template <std::size_t MaxEntries, typename StateTable, std::size_t PreserveCount>
FlatStateSet<MaxEntries> makePrioritizedFlatStateSet(
    const StateTable &values,
    const std::array<u32, PreserveCount> &preservedStates) {
  FlatStateSet<MaxEntries> set{};
  set.hash = hashStateMap(values);
  set.overflow = values.size() > MaxEntries;

  for (const u32 state : preservedStates) {
    appendFlatStateEntryIfPresent(set, values, state);
  }
  for (const auto &entry : values) {
    appendFlatStateEntryIfPresent(set, values, entry.first);
  }
  std::sort(set.entries.begin(), set.entries.begin() + set.count,
            [](const FlatStateEntry &a, const FlatStateEntry &b) {
              return a.state < b.state;
            });
  return set;
}


u64 hashVertexDeclElements(const VertexDeclSnapshot &decl) {
  u64 hash = hashCombine(kFnvOffset, static_cast<u64>(decl.elements.size()));
  hash = hashCombine(hash, decl.fvf);
  for (const auto &element : decl.elements) {
    hash = hashCombine(hash, hashTrivial(element));
  }
  return hash;
}

u64 hashShaderRefSummary(const ShaderRef &shader) {
  u64 hash = hashCombine(kFnvOffset, static_cast<u64>(shader.kind));
  hash = hashCombine(hash, shader.hash);
  hash = hashCombine(hash, shader.bytecode.hash);
  return hash;
}

u32 readShaderWord(std::span<const u8> bytes, std::size_t offset) {
  u32 value = 0;
  std::memcpy(&value, bytes.data() + offset, sizeof(value));
  return value;
}

u32 shaderRegisterType(u32 token) {
  return ((token >> 28) & 0x7u) | (((token >> 11) & 0x3u) << 3);
}

u32 shaderRegisterIndex(u32 token) {
  return token & 0x7ffu;
}

bool shaderTokenHasRelativeAddressing(u32 token) {
  return ((token >> 13) & 0x1u) != 0;
}

std::optional<u32> legacyPixelOperandCount(u32 opcode, u32 major, u32 minor) {
  if (major != 1u) {
    return std::nullopt;
  }

  switch (opcode) {
    case bc::kD3DSIO_TEXCOORD:
      return minor >= 4u ? 2u : 1u;
    case bc::kD3DSIO_TEX:
      return minor >= 4u ? 2u : 1u;
    case bc::kD3DSIO_TEXDEPTH:
      return 1u;
    case bc::kD3DSIO_TEXBEM:
    case bc::kD3DSIO_TEXBEML:
    case bc::kD3DSIO_TEXREG2AR:
    case bc::kD3DSIO_TEXREG2GB:
    case bc::kD3DSIO_TEXM3x2PAD:
    case bc::kD3DSIO_TEXM3x2TEX:
    case bc::kD3DSIO_TEXM3x3PAD:
    case bc::kD3DSIO_TEXM3x3TEX:
    case bc::kD3DSIO_TEXM3x3DIFF:
    case bc::kD3DSIO_TEXM3x3VSPEC:
    case bc::kD3DSIO_TEXREG2RGB:
    case bc::kD3DSIO_TEXDP3TEX:
    case bc::kD3DSIO_TEXM3x2DEPTH:
    case bc::kD3DSIO_TEXDP3:
    case bc::kD3DSIO_TEXM3x3:
      return 2u;
    case bc::kD3DSIO_TEXM3x3SPEC:
    case bc::kD3DSIO_BEM:
      return 3u;
    default:
      return std::nullopt;
  }
}

u32 fixedShaderOperandCount(u32 opcode) {
  switch (opcode) {
    case bc::kD3DSIO_NOP:
    case bc::kD3DSIO_PHASE:
    case bc::kD3DSIO_ELSE:
    case bc::kD3DSIO_ENDIF:
    case bc::kD3DSIO_ENDLOOP:
    case bc::kD3DSIO_ENDREP:
    case bc::kD3DSIO_RET:
    case bc::kD3DSIO_BREAK:
      return 0;
    case bc::kD3DSIO_IF:
    case bc::kD3DSIO_TEXKILL:
    case bc::kD3DSIO_LABEL:
    case bc::kD3DSIO_CALL:
      return 1;
    case bc::kD3DSIO_MOV:
    case bc::kD3DSIO_DEFB:
    case bc::kD3DSIO_RCP:
    case bc::kD3DSIO_RSQ:
    case bc::kD3DSIO_FRC:
    case bc::kD3DSIO_DSX:
    case bc::kD3DSIO_DSY:
    case bc::kD3DSIO_SETP:
    case bc::kD3DSIO_BREAKP:
    case bc::kD3DSIO_MOVA:
    case bc::kD3DSIO_LOG:
    case bc::kD3DSIO_LOGP:
    case bc::kD3DSIO_EXP:
    case bc::kD3DSIO_EXPP:
    case bc::kD3DSIO_SGN:
    case bc::kD3DSIO_ABS:
    case bc::kD3DSIO_NRM:
    case bc::kD3DSIO_IFC:
    case bc::kD3DSIO_BREAKC:
    case bc::kD3DSIO_CALLNZ:
      return 2;
    case bc::kD3DSIO_ADD:
    case bc::kD3DSIO_SUB:
    case bc::kD3DSIO_MUL:
    case bc::kD3DSIO_DP3:
    case bc::kD3DSIO_DP4:
    case bc::kD3DSIO_MIN:
    case bc::kD3DSIO_MAX:
    case bc::kD3DSIO_POW:
    case bc::kD3DSIO_CRS:
    case bc::kD3DSIO_TEXLDL:
    case bc::kD3DSIO_SLT:
    case bc::kD3DSIO_SGE:
    case bc::kD3DSIO_M4x4:
    case bc::kD3DSIO_M4x3:
    case bc::kD3DSIO_M3x4:
    case bc::kD3DSIO_M3x3:
    case bc::kD3DSIO_M3x2:
      return 3;
    case bc::kD3DSIO_MAD:
    case bc::kD3DSIO_LRP:
    case bc::kD3DSIO_CND:
    case bc::kD3DSIO_CMP:
    case bc::kD3DSIO_DP2ADD:
      return 4;
    case bc::kD3DSIO_TEXLDD:
    case bc::kD3DSIO_DEF:
    case bc::kD3DSIO_DEFI:
      return 5;
    default:
      return 0;
  }
}

bool shaderOperandIsRegister(u32 opcode, u32 operandIndex) {
  switch (opcode) {
    case bc::kD3DSIO_DEF:
    case bc::kD3DSIO_DEFI:
    case bc::kD3DSIO_DEFB:
      return operandIndex == 0;
    case bc::kD3DSIO_LABEL:
    case bc::kD3DSIO_CALL:
      return false;
    case bc::kD3DSIO_CALLNZ:
      return operandIndex != 0;
    default:
      return true;
  }
}

bool shaderOpcodeWritesFirstOperand(u32 opcode) {
  switch (opcode) {
    case bc::kD3DSIO_DEF:
    case bc::kD3DSIO_DEFI:
    case bc::kD3DSIO_DEFB:
    case bc::kD3DSIO_MOV:
    case bc::kD3DSIO_ADD:
    case bc::kD3DSIO_SUB:
    case bc::kD3DSIO_MUL:
    case bc::kD3DSIO_MAD:
    case bc::kD3DSIO_MIN:
    case bc::kD3DSIO_MAX:
    case bc::kD3DSIO_SLT:
    case bc::kD3DSIO_SGE:
    case bc::kD3DSIO_EXP:
    case bc::kD3DSIO_LOG:
    case bc::kD3DSIO_EXPP:
    case bc::kD3DSIO_LOGP:
    case bc::kD3DSIO_M4x4:
    case bc::kD3DSIO_M4x3:
    case bc::kD3DSIO_M3x4:
    case bc::kD3DSIO_M3x3:
    case bc::kD3DSIO_M3x2:
    case bc::kD3DSIO_RCP:
    case bc::kD3DSIO_RSQ:
    case bc::kD3DSIO_FRC:
    case bc::kD3DSIO_LRP:
    case bc::kD3DSIO_DP3:
    case bc::kD3DSIO_DP4:
    case bc::kD3DSIO_CND:
    case bc::kD3DSIO_CMP:
    case bc::kD3DSIO_DP2ADD:
    case bc::kD3DSIO_POW:
    case bc::kD3DSIO_CRS:
    case bc::kD3DSIO_SGN:
    case bc::kD3DSIO_ABS:
    case bc::kD3DSIO_NRM:
    case bc::kD3DSIO_TEX:
    case bc::kD3DSIO_DSX:
    case bc::kD3DSIO_DSY:
    case bc::kD3DSIO_TEXLDD:
    case bc::kD3DSIO_TEXLDL:
      return true;
    default:
      return false;
  }
}

u32 matrixConstantRows(u32 opcode) {
  switch (opcode) {
    case bc::kD3DSIO_M4x4:
    case bc::kD3DSIO_M3x4:
      return 4;
    case bc::kD3DSIO_M4x3:
    case bc::kD3DSIO_M3x3:
      return 3;
    case bc::kD3DSIO_M3x2:
      return 2;
    default:
      return 1;
  }
}

void noteShaderConstantUsage(ShaderConstantUsageBounds& usage,
                             u32 registerType,
                             u32 registerIndex,
                             bool indexed) {
  const auto nextCount = static_cast<std::uint16_t>(
      std::min<u32>(registerIndex + 1u, std::numeric_limits<std::uint16_t>::max()));
  switch (registerType) {
    case bc::kD3DSPR_CONST:
      usage.floatCount = std::max(usage.floatCount, nextCount);
      usage.indexedFloat = usage.indexedFloat || indexed;
      break;
    case bc::kD3DSPR_CONSTINT:
      usage.intCount = std::max(usage.intCount, nextCount);
      usage.indexedInt = usage.indexedInt || indexed;
      break;
    case bc::kD3DSPR_CONSTBOOL:
      usage.boolCount = std::max(usage.boolCount, nextCount);
      usage.indexedBool = usage.indexedBool || indexed;
      break;
    default:
      break;
  }
}

ShaderConstantUsageBounds scanShaderConstantUsage(const ShaderRef& shader) {
  ShaderConstantUsageBounds usage{};
  if (shader.kind != ShaderRef::Kind::Bytecode) {
    usage.unknown = false;
    return usage;
  }
  if (shader.bytecode.bytes.size() < sizeof(u32)) {
    return usage;
  }

  const auto bytes = std::span<const u8>(shader.bytecode.bytes.data(), shader.bytecode.bytes.size());
  const u32 version = readShaderWord(bytes, 0);
  const u32 major = (version >> 8) & 0xffu;
  const u32 minor = version & 0xffu;
  std::size_t offset = sizeof(u32);
  usage.unknown = false;

  while (offset + sizeof(u32) <= bytes.size()) {
    const u32 token = readShaderWord(bytes, offset);
    offset += sizeof(u32);
    const u32 opcode = token & 0xffffu;
    if (opcode == bc::kD3DSIO_END) {
      return usage;
    }
    if (opcode == bc::kD3DSIO_COMMENT) {
      const std::size_t commentBytes =
          static_cast<std::size_t>((token >> 16) & 0x7fffu) * sizeof(u32);
      if (offset + commentBytes > bytes.size()) {
        usage.unknown = true;
        return usage;
      }
      offset += commentBytes;
      continue;
    }
    if (opcode == bc::kD3DSIO_PHASE) {
      continue;
    }

    u32 operandCount = legacyPixelOperandCount(opcode, major, minor)
        .value_or(fixedShaderOperandCount(opcode));
    if (operandCount == 0 && ((token >> 24) & 0xfu) != 0) {
      operandCount = (token >> 24) & 0xfu;
    }

    std::array<u32, 8> operands{};
    std::array<bool, 8> indexedOperands{};
    if (operandCount > operands.size()) {
      usage.unknown = true;
      return usage;
    }
    for (u32 i = 0; i < operandCount; ++i) {
      if (offset + sizeof(u32) > bytes.size()) {
        usage.unknown = true;
        return usage;
      }
      const u32 operand = readShaderWord(bytes, offset);
      offset += sizeof(u32);
      operands[i] = operand;
      if (shaderOperandIsRegister(opcode, i) && shaderTokenHasRelativeAddressing(operand)) {
        if (offset + sizeof(u32) > bytes.size()) {
          usage.unknown = true;
          return usage;
        }
        offset += sizeof(u32);
        indexedOperands[i] = true;
      }
    }

    std::size_t sourceBegin = 0;
    if (shaderOpcodeWritesFirstOperand(opcode)) {
      sourceBegin = 1;
      if (operandCount > 0) {
        noteShaderConstantUsage(usage, shaderRegisterType(operands[0]),
                                shaderRegisterIndex(operands[0]), indexedOperands[0]);
      }
    }
    switch (opcode) {
      case bc::kD3DSIO_DEF:
      case bc::kD3DSIO_DEFI:
      case bc::kD3DSIO_DEFB:
      case bc::kD3DSIO_DCL:
      case bc::kD3DSIO_LABEL:
      case bc::kD3DSIO_CALL:
        sourceBegin = operandCount;
        break;
      default:
        break;
    }
    for (std::size_t i = sourceBegin; i < operandCount; ++i) {
      noteShaderConstantUsage(usage, shaderRegisterType(operands[i]),
                              shaderRegisterIndex(operands[i]), indexedOperands[i]);
    }
    const u32 rows = matrixConstantRows(opcode);
    if (rows > 1 && operandCount > 2) {
      const u32 registerType = shaderRegisterType(operands[2]);
      if (registerType == bc::kD3DSPR_CONST) {
        noteShaderConstantUsage(usage, registerType,
                                shaderRegisterIndex(operands[2]) + rows - 1u,
                                indexedOperands[2]);
      }
    }
  }
  usage.unknown = true;
  return usage;
}

u64 hashTextureTransforms(
    const std::array<Matrix4x4, kMaxTextureStages> &transforms) {
  u64 hash = hashCombine(kFnvOffset, transforms.size());
  for (const auto &transform : transforms) {
    hash = hashCombine(hash, hashTrivial(transform));
  }
  return hash;
}

bool matrixIsIdentity(const Matrix4x4 &matrix) {
  for (u32 row = 0; row < 4; ++row) {
    for (u32 col = 0; col < 4; ++col) {
      const float expected = row == col ? 1.0f : 0.0f;
      if (std::fabs(matrix.m[row * 4 + col] - expected) > 1.0e-6f) {
        return false;
      }
    }
  }
  return true;
}

u32 nonIdentityTextureTransformStageMask(
    const std::array<Matrix4x4, kMaxTextureStages> &transforms) {
  u32 mask = 0;
  for (u32 stage = 0; stage < transforms.size(); ++stage) {
    if (!matrixIsIdentity(transforms[stage])) {
      mask |= 1u << stage;
    }
  }
  return mask;
}

u64 hashBlendWorldViewProj(const std::array<Matrix4x4, 4> &transforms) {
  u64 hash = hashCombine(kFnvOffset, transforms.size());
  for (const auto &transform : transforms) {
    hash = hashCombine(hash, hashTrivial(transform));
  }
  return hash;
}

u64 hashClipPlanes(const std::array<ClipPlane, kMaxClipPlanes> &planes) {
  u64 hash = hashCombine(kFnvOffset, planes.size());
  for (const auto &plane : planes) {
    hash = hashCombine(hash, hashTrivial(plane));
  }
  return hash;
}

u64 hashLight(const Light &light);
u64 hashMaterial(const Material &material);

struct ShaderConstantHashResult {
  u64 hash = 0;
  std::uint64_t bytes = 0;
  std::uint64_t indexedFloatMinSafeBytes = 0;
  std::uint64_t indexedFloatPotentialSavedBytes = 0;
  u16 floatCount = 0;
  u16 intCount = 0;
  u16 boolCount = 0;
  bool full = false;
  bool noUsage = false;
  bool unknown = false;
  bool indexedFloat = false;
  bool indexedInt = false;
  bool indexedBool = false;
};

struct DrawUniformPayloadHashOptions {
  const ShaderConstantUsageBounds *vertexUsage = nullptr;
  const ShaderConstantUsageBounds *pixelUsage = nullptr;
  const DrawUniformPayloadHashes *reusableNonConstantHashes = nullptr;
  const DrawUniformPayloadHashes *reusableShaderConstantHashes = nullptr;
  detail::ShaderConstantHashIndex<kMaxVertexConstants>
      *vertexConstantHashIndex = nullptr;
  detail::ShaderConstantHashIndex<kMaxPixelConstants>
      *pixelConstantHashIndex = nullptr;
  u64 vertexConstantGeneration = 0;
  u64 pixelConstantGeneration = 0;
  bool reuseVertexConstantsHash = false;
  bool reusePixelConstantsHash = false;
  bool vertexUsageFromBytecode = false;
  bool pixelUsageFromBytecode = false;
  bool recordSnapshotPerf = false;
};

template <std::size_t TreeSize>
u64 shaderConstantFenwickPrefix(
    const std::array<u64, TreeSize> &tree, std::size_t count) {
  count = std::min<std::size_t>(count, TreeSize - 1u);
  u64 hash = 0;
  for (auto index = count; index != 0; index -= index & (~index + 1u)) {
    hash ^= tree[index];
  }
  return hash;
}

template <std::size_t TreeSize>
void setShaderConstantFenwickValue(
    std::array<u64, TreeSize> &tree, std::size_t index, u64 value) {
  if (index >= TreeSize - 1u) {
    return;
  }
  const auto previous =
      shaderConstantFenwickPrefix(tree, index + 1u) ^
      shaderConstantFenwickPrefix(tree, index);
  const auto delta = previous ^ value;
  for (auto cursor = index + 1u; cursor < TreeSize;
       cursor += cursor & (~cursor + 1u)) {
    tree[cursor] ^= delta;
  }
}

template <typename T>
u64 hashShaderConstantRegister(const T &value, std::size_t index, u64 domain) {
  u64 hash = hashCombine(kFnvOffset, domain);
  hash = hashCombine(hash, static_cast<u64>(index));
  hash = hashCombine(hash, hashTrivial(value));
  return hash;
}

void clearShaderConstantHashDirtyRange(
    detail::ShaderConstantHashDirtyRange &range) {
  range = {};
}

template <typename Values, std::size_t TreeSize>
std::uint64_t syncShaderConstantHashDirtyRange(
    std::array<u64, TreeSize> &tree, const Values &values,
    detail::ShaderConstantHashDirtyRange &range, u64 domain) {
  if (!range.dirty) {
    return 0;
  }
  const auto begin =
      std::min<std::size_t>(range.begin, values.size());
  const auto end = std::min<std::size_t>(range.end, values.size());
  for (auto index = begin; index < end; ++index) {
    setShaderConstantFenwickValue(
        tree, index,
        hashShaderConstantRegister(values[index], index, domain));
  }
  clearShaderConstantHashDirtyRange(range);
  return static_cast<std::uint64_t>(end - begin) * sizeof(values[0]);
}

template <std::size_t FloatCount>
std::uint64_t syncShaderConstantHashIndex(
    detail::ShaderConstantHashIndex<FloatCount> &index,
    const ShaderConstantSnapshot<FloatCount> &constants, u64 generation) {
  constexpr u64 kFloatDomain = 0x494e4348464c4f41ull;
  constexpr u64 kIntDomain = 0x494e4348494e5420ull;
  constexpr u64 kBoolDomain = 0x494e4348424f4f4cull;
  const bool hasDirtyRange = index.floatDirty.dirty || index.intDirty.dirty ||
                             index.boolDirty.dirty;
  if (!index.valid ||
      (index.generation != generation && !hasDirtyRange)) {
    index.floatTree.fill(0);
    index.intTree.fill(0);
    index.boolTree.fill(0);
    for (std::size_t i = 0; i < constants.float4.size(); ++i) {
      setShaderConstantFenwickValue(
          index.floatTree, i,
          hashShaderConstantRegister(constants.float4[i], i, kFloatDomain));
    }
    for (std::size_t i = 0; i < constants.int4.size(); ++i) {
      setShaderConstantFenwickValue(
          index.intTree, i,
          hashShaderConstantRegister(constants.int4[i], i, kIntDomain));
    }
    for (std::size_t i = 0; i < constants.bools.size(); ++i) {
      setShaderConstantFenwickValue(
          index.boolTree, i,
          hashShaderConstantRegister(constants.bools[i], i, kBoolDomain));
    }
    clearShaderConstantHashDirtyRange(index.floatDirty);
    clearShaderConstantHashDirtyRange(index.intDirty);
    clearShaderConstantHashDirtyRange(index.boolDirty);
    index.generation = generation;
    index.valid = true;
    return sizeof(constants.float4) + sizeof(constants.int4) +
           sizeof(constants.bools);
  }

  std::uint64_t bytes = 0;
  bytes += syncShaderConstantHashDirtyRange(
      index.floatTree, constants.float4, index.floatDirty, kFloatDomain);
  bytes += syncShaderConstantHashDirtyRange(
      index.intTree, constants.int4, index.intDirty, kIntDomain);
  bytes += syncShaderConstantHashDirtyRange(
      index.boolTree, constants.bools, index.boolDirty, kBoolDomain);
  index.generation = generation;
  index.valid = true;
  return bytes;
}

template <std::size_t FloatCount>
ShaderConstantHashResult hashShaderConstantsIncrementallyForUsage(
    detail::ShaderConstantHashIndex<FloatCount> &index,
    const ShaderConstantSnapshot<FloatCount> &constants, u64 generation,
    const ShaderConstantUsageBounds *usage) {
  const auto bytes = syncShaderConstantHashIndex(index, constants, generation);
  const bool full =
      !usage || usage->unknown || usage->indexedFloat || usage->indexedInt ||
      usage->indexedBool;
  const bool indexedFloatOnly =
      usage && !usage->unknown && usage->indexedFloat && !usage->indexedInt &&
      !usage->indexedBool;
  const auto floatCount = static_cast<u16>(
      full ? FloatCount : std::min<std::size_t>(usage->floatCount, FloatCount));
  const auto intCount = static_cast<u16>(
      full && !indexedFloatOnly
          ? kMaxIntegerConstants
          : std::min<std::size_t>(usage ? usage->intCount : 0u,
                                  kMaxIntegerConstants));
  const auto boolCount = static_cast<u16>(
      full && !indexedFloatOnly
          ? kMaxBoolConstants
          : std::min<std::size_t>(usage ? usage->boolCount : 0u,
                                  kMaxBoolConstants));

  u64 flags = full ? 1u : 0u;
  flags |= !usage ? 1u << 1u : 0u;
  flags |= usage && usage->unknown ? 1u << 2u : 0u;
  flags |= usage && usage->indexedFloat ? 1u << 3u : 0u;
  flags |= usage && usage->indexedInt ? 1u << 4u : 0u;
  flags |= usage && usage->indexedBool ? 1u << 5u : 0u;

  constexpr u64 kIncrementalHashDomain = 0x494e435348434f4eull;
  u64 hash = hashCombine(kFnvOffset, kIncrementalHashDomain);
  hash = hashCombine(hash, static_cast<u64>(FloatCount));
  hash = hashCombine(hash, static_cast<u64>(floatCount));
  hash = hashCombine(hash,
                     shaderConstantFenwickPrefix(index.floatTree, floatCount));
  hash = hashCombine(hash, static_cast<u64>(intCount));
  hash = hashCombine(hash,
                     shaderConstantFenwickPrefix(index.intTree, intCount));
  hash = hashCombine(hash, static_cast<u64>(boolCount));
  hash = hashCombine(hash,
                     shaderConstantFenwickPrefix(index.boolTree, boolCount));
  hash = hashCombine(hash, flags);
  return ShaderConstantHashResult{
      .hash = hash,
      .bytes = bytes,
      .floatCount = floatCount,
      .intCount = intCount,
      .boolCount = boolCount,
      .full = full,
      .noUsage = !usage,
      .unknown = usage ? usage->unknown : false,
      .indexedFloat = usage ? usage->indexedFloat : false,
      .indexedInt = usage ? usage->indexedInt : false,
      .indexedBool = usage ? usage->indexedBool : false,
  };
}

template <typename T, std::size_t Count>
u64 hashArrayPrefix(const std::array<T, Count> &values, std::size_t count) {
  static_assert(std::is_trivially_copyable_v<T>);
  const auto clamped = std::min<std::size_t>(count, values.size());
  return hashBytes(std::as_bytes(std::span<const T>(values.data(), clamped)));
}

template <std::size_t FloatCount>
std::uint64_t indexedFloatMinimumSafeHashBytes(
    const ShaderConstantSnapshot<FloatCount> &constants,
    const ShaderConstantUsageBounds *usage) {
  if (!usage || !usage->indexedFloat) {
    return sizeof(constants);
  }
  const auto intCount =
      std::min<std::size_t>(usage->intCount, constants.int4.size());
  const auto boolCount =
      std::min<std::size_t>(usage->boolCount, constants.bools.size());
  return sizeof(constants.float4) +
         static_cast<std::uint64_t>(intCount) * sizeof(constants.int4[0]) +
         static_cast<std::uint64_t>(boolCount) * sizeof(constants.bools[0]);
}

template <std::size_t FloatCount>
u64 hashIndexedFloatShaderConstantsForUsage(
    const ShaderConstantSnapshot<FloatCount> &constants,
    const ShaderConstantUsageBounds &usage) {
  const auto intCount =
      std::min<std::size_t>(usage.intCount, constants.int4.size());
  const auto boolCount =
      std::min<std::size_t>(usage.boolCount, constants.bools.size());

  u64 hash = hashCombine(kFnvOffset, static_cast<u64>(constants.float4.size()));
  hash = hashCombine(hash,
                     hashArrayPrefix(constants.float4, constants.float4.size()));
  hash = hashCombine(hash, static_cast<u64>(intCount));
  hash = hashCombine(hash, hashArrayPrefix(constants.int4, intCount));
  hash = hashCombine(hash, static_cast<u64>(boolCount));
  hash = hashCombine(hash, hashArrayPrefix(constants.bools, boolCount));
  return hash;
}

template <std::size_t FloatCount>
ShaderConstantHashResult hashShaderConstantsForUsage(
    const ShaderConstantSnapshot<FloatCount> &constants,
    const ShaderConstantUsageBounds *usage) {
  const bool full =
      !usage || usage->unknown || usage->indexedFloat || usage->indexedInt ||
      usage->indexedBool;
  if (full) {
    const bool indexedFloatOnly =
        usage && !usage->unknown && usage->indexedFloat &&
        !usage->indexedInt && !usage->indexedBool;
    const auto indexedFloatMinSafeBytes =
        indexedFloatMinimumSafeHashBytes(constants, usage);
    const auto indexedFloatPotentialSavedBytes =
        indexedFloatMinSafeBytes < sizeof(constants)
            ? sizeof(constants) - indexedFloatMinSafeBytes
            : 0u;
    return ShaderConstantHashResult{
        .hash = indexedFloatOnly
            ? hashIndexedFloatShaderConstantsForUsage(constants, *usage)
            : hashTrivial(constants),
        .bytes = indexedFloatOnly ? indexedFloatMinSafeBytes : sizeof(constants),
        .indexedFloatMinSafeBytes = indexedFloatMinSafeBytes,
        .indexedFloatPotentialSavedBytes = indexedFloatPotentialSavedBytes,
        .floatCount = static_cast<u16>(constants.float4.size()),
        .intCount = static_cast<u16>(
            indexedFloatOnly ? std::min<std::size_t>(usage->intCount, constants.int4.size())
                             : constants.int4.size()),
        .boolCount = static_cast<u16>(
            indexedFloatOnly ? std::min<std::size_t>(usage->boolCount, constants.bools.size())
                             : constants.bools.size()),
        .full = true,
        .noUsage = !usage,
        .unknown = usage ? usage->unknown : false,
        .indexedFloat = usage ? usage->indexedFloat : false,
        .indexedInt = usage ? usage->indexedInt : false,
        .indexedBool = usage ? usage->indexedBool : false,
    };
  }

  const auto floatCount =
      std::min<std::size_t>(usage->floatCount, constants.float4.size());
  const auto intCount =
      std::min<std::size_t>(usage->intCount, constants.int4.size());
  const auto boolCount =
      std::min<std::size_t>(usage->boolCount, constants.bools.size());

  u64 hash = hashCombine(kFnvOffset, static_cast<u64>(floatCount));
  hash = hashCombine(hash, hashArrayPrefix(constants.float4, floatCount));
  hash = hashCombine(hash, static_cast<u64>(intCount));
  hash = hashCombine(hash, hashArrayPrefix(constants.int4, intCount));
  hash = hashCombine(hash, static_cast<u64>(boolCount));
  hash = hashCombine(hash, hashArrayPrefix(constants.bools, boolCount));

  const auto bytes =
      static_cast<std::uint64_t>(floatCount) * sizeof(constants.float4[0]) +
      static_cast<std::uint64_t>(intCount) * sizeof(constants.int4[0]) +
      static_cast<std::uint64_t>(boolCount) * sizeof(constants.bools[0]);
  return ShaderConstantHashResult{
      .hash = hash,
      .bytes = bytes,
      .floatCount = static_cast<u16>(floatCount),
      .intCount = static_cast<u16>(intCount),
      .boolCount = static_cast<u16>(boolCount),
      .full = false,
      .noUsage = false,
      .unknown = false,
      .indexedFloat = false,
      .indexedInt = false,
      .indexedBool = false,
  };
}

void hashDrawUniformShaderConstantSnapshots(
    const VertexShaderConstants &vsConst, const PixelShaderConstants &psConst,
    DrawUniformPayloadHashes &hashes,
    DrawUniformPayloadHashOptions options = {}) {
  const bool recordPerf = options.recordSnapshotPerf && dxmt9::perf::enabled();
  auto recorder = [&](void (*fn)(std::uint64_t)) {
    return recordPerf ? fn : nullptr;
  };

  if (options.reuseVertexConstantsHash &&
      options.reusableShaderConstantHashes) {
    hashes.vertexConstantsHash =
        options.reusableShaderConstantHashes->vertexConstantsHash;
    hashes.vertexConstantsBytes =
        options.reusableShaderConstantHashes->vertexConstantsBytes;
    hashes.vertexFloatConstantCount =
        options.reusableShaderConstantHashes->vertexFloatConstantCount;
    hashes.vertexIntConstantCount =
        options.reusableShaderConstantHashes->vertexIntConstantCount;
    hashes.vertexBoolConstantCount =
        options.reusableShaderConstantHashes->vertexBoolConstantCount;
  } else {
    PerfScope scope(recorder(
        dxmt9::perf::countD3D9SnapshotUniformBuildVsConstHashCpuTime));
    const auto result = options.vertexConstantHashIndex
        ? hashShaderConstantsIncrementallyForUsage(
              *options.vertexConstantHashIndex, vsConst,
              options.vertexConstantGeneration, options.vertexUsage)
        : hashShaderConstantsForUsage(vsConst, options.vertexUsage);
    hashes.vertexConstantsHash = result.hash;
    hashes.vertexConstantsBytes = result.bytes;
    hashes.vertexFloatConstantCount = result.floatCount;
    hashes.vertexIntConstantCount = result.intCount;
    hashes.vertexBoolConstantCount = result.boolCount;
    if (recordPerf) {
      dxmt9::perf::countD3D9SnapshotUniformBuildVsConstHashBytes(result.bytes);
      if (result.full) {
        dxmt9::perf::countD3D9SnapshotUniformBuildVsConstHashFull();
        if (result.noUsage) {
          dxmt9::perf::countD3D9SnapshotUniformBuildVsConstHashFullNoUsage();
        }
        if (result.unknown) {
          dxmt9::perf::countD3D9SnapshotUniformBuildVsConstHashFullUnknown();
          if (options.vertexUsageFromBytecode) {
            dxmt9::perf::countD3D9SnapshotUniformBuildVsConstHashFullUnknownBytecode();
          } else {
            dxmt9::perf::countD3D9SnapshotUniformBuildVsConstHashFullUnknownNonBytecode();
          }
        }
        if (result.indexedFloat) {
          dxmt9::perf::countD3D9SnapshotUniformBuildVsConstHashFullIndexedFloat();
          dxmt9::perf::countD3D9SnapshotUniformBuildVsConstHashFullIndexedFloatMinSafeBytes(
              result.indexedFloatMinSafeBytes);
          dxmt9::perf::countD3D9SnapshotUniformBuildVsConstHashFullIndexedFloatPotentialSavedBytes(
              result.indexedFloatPotentialSavedBytes);
        }
        if (result.indexedInt) {
          dxmt9::perf::countD3D9SnapshotUniformBuildVsConstHashFullIndexedInt();
        }
        if (result.indexedBool) {
          dxmt9::perf::countD3D9SnapshotUniformBuildVsConstHashFullIndexedBool();
        }
      }
    }
  }
  if (options.reusePixelConstantsHash &&
      options.reusableShaderConstantHashes) {
    hashes.pixelConstantsHash =
        options.reusableShaderConstantHashes->pixelConstantsHash;
    hashes.pixelConstantsBytes =
        options.reusableShaderConstantHashes->pixelConstantsBytes;
    hashes.pixelFloatConstantCount =
        options.reusableShaderConstantHashes->pixelFloatConstantCount;
    hashes.pixelIntConstantCount =
        options.reusableShaderConstantHashes->pixelIntConstantCount;
    hashes.pixelBoolConstantCount =
        options.reusableShaderConstantHashes->pixelBoolConstantCount;
  } else {
    PerfScope scope(recorder(
        dxmt9::perf::countD3D9SnapshotUniformBuildPsConstHashCpuTime));
    const auto result = options.pixelConstantHashIndex
        ? hashShaderConstantsIncrementallyForUsage(
              *options.pixelConstantHashIndex, psConst,
              options.pixelConstantGeneration, options.pixelUsage)
        : hashShaderConstantsForUsage(psConst, options.pixelUsage);
    hashes.pixelConstantsHash = result.hash;
    hashes.pixelConstantsBytes = result.bytes;
    hashes.pixelFloatConstantCount = result.floatCount;
    hashes.pixelIntConstantCount = result.intCount;
    hashes.pixelBoolConstantCount = result.boolCount;
    if (recordPerf) {
      dxmt9::perf::countD3D9SnapshotUniformBuildPsConstHashBytes(result.bytes);
      if (result.full) {
        dxmt9::perf::countD3D9SnapshotUniformBuildPsConstHashFull();
        if (result.noUsage) {
          dxmt9::perf::countD3D9SnapshotUniformBuildPsConstHashFullNoUsage();
        }
        if (result.unknown) {
          dxmt9::perf::countD3D9SnapshotUniformBuildPsConstHashFullUnknown();
          if (options.pixelUsageFromBytecode) {
            dxmt9::perf::countD3D9SnapshotUniformBuildPsConstHashFullUnknownBytecode();
          } else {
            dxmt9::perf::countD3D9SnapshotUniformBuildPsConstHashFullUnknownNonBytecode();
          }
        }
        if (result.indexedFloat) {
          dxmt9::perf::countD3D9SnapshotUniformBuildPsConstHashFullIndexedFloat();
        }
        if (result.indexedInt) {
          dxmt9::perf::countD3D9SnapshotUniformBuildPsConstHashFullIndexedInt();
        }
        if (result.indexedBool) {
          dxmt9::perf::countD3D9SnapshotUniformBuildPsConstHashFullIndexedBool();
        }
      }
    }
  }
}

void hashDrawUniformShaderConstantComponents(
    const DrawUniformPayload &payload, DrawUniformPayloadHashes &hashes,
    DrawUniformPayloadHashOptions options = {}) {
  hashDrawUniformShaderConstantSnapshots(payload.vsConst, payload.psConst,
                                         hashes, options);
}

void hashDrawUniformNonConstantComponents(
    const DrawUniformPayload &payload, DrawUniformPayloadHashes &hashes,
    DrawUniformPayloadHashOptions options = {}) {
  const bool recordPerf = options.recordSnapshotPerf && dxmt9::perf::enabled();
  auto recorder = [&](void (*fn)(std::uint64_t)) {
    return recordPerf ? fn : nullptr;
  };

  {
    PerfScope scope(recorder(
        dxmt9::perf::countD3D9SnapshotUniformBuildNonConstHashCpuTime));
    {
      PerfScope fieldScope(recorder(
          dxmt9::perf::countD3D9SnapshotUniformBuildNonConstHashWorldViewProjCpuTime));
      hashes.worldViewProjHash = hashTrivial(payload.worldViewProj);
    }
    hashes.ffpViewHash = hashTrivial(payload.ffpView);
    {
      PerfScope fieldScope(recorder(
          dxmt9::perf::countD3D9SnapshotUniformBuildNonConstHashFfpWorldViewCpuTime));
      hashes.ffpWorldViewHash = hashTrivial(payload.ffpWorldView);
    }
    {
      PerfScope fieldScope(recorder(
          dxmt9::perf::countD3D9SnapshotUniformBuildNonConstHashFfpNormalMatrixCpuTime));
      hashes.ffpNormalMatrixHash = hashTrivial(payload.ffpNormalMatrix);
    }
    {
      PerfScope fieldScope(recorder(
          dxmt9::perf::countD3D9SnapshotUniformBuildNonConstHashMaterialCpuTime));
      hashes.materialHash = hashMaterial(payload.material);
    }
    {
      PerfScope fieldScope(recorder(
          dxmt9::perf::countD3D9SnapshotUniformBuildNonConstHashLightsCpuTime));
      for (std::size_t i = 0; i < payload.lights.size(); ++i) {
        hashes.lightHashes[i] = hashLight(payload.lights[i]);
      }
    }
    {
      PerfScope fieldScope(recorder(
          dxmt9::perf::countD3D9SnapshotUniformBuildNonConstHashFfpBlendWvpCpuTime));
      hashes.ffpBlendWorldViewProjHash =
          hashBlendWorldViewProj(payload.ffpBlendWorldViewProj);
    }
    hashes.ffpBlendWorldViewHash =
        hashBlendWorldViewProj(payload.ffpBlendWorldView);
    hashes.ffpBlendNormalMatrixHash =
        hashBlendWorldViewProj(payload.ffpBlendNormalMatrix);
    {
      PerfScope fieldScope(recorder(
          dxmt9::perf::countD3D9SnapshotUniformBuildNonConstHashTextureTransformsCpuTime));
      hashes.textureTransformsHash =
          hashTextureTransforms(payload.textureTransforms);
      hashes.nonIdentityTextureTransformStageMask =
          nonIdentityTextureTransformStageMask(payload.textureTransforms);
    }
    {
      PerfScope fieldScope(recorder(
          dxmt9::perf::countD3D9SnapshotUniformBuildNonConstHashClipPlanesCpuTime));
      hashes.clipPlanesHash = hashClipPlanes(payload.clipPlanes);
    }
  }
}

void copyDrawUniformNonConstantHashes(
    DrawUniformPayloadHashes &dst,
    const DrawUniformPayloadHashes &src) noexcept {
  dst.worldViewProjHash = src.worldViewProjHash;
  dst.ffpViewHash = src.ffpViewHash;
  dst.ffpWorldViewHash = src.ffpWorldViewHash;
  dst.ffpNormalMatrixHash = src.ffpNormalMatrixHash;
  dst.materialHash = src.materialHash;
  dst.lightHashes = src.lightHashes;
  dst.ffpBlendWorldViewProjHash = src.ffpBlendWorldViewProjHash;
  dst.ffpBlendWorldViewHash = src.ffpBlendWorldViewHash;
  dst.ffpBlendNormalMatrixHash = src.ffpBlendNormalMatrixHash;
  dst.textureTransformsHash = src.textureTransformsHash;
  dst.nonIdentityTextureTransformStageMask =
      src.nonIdentityTextureTransformStageMask;
  dst.clipPlanesHash = src.clipPlanesHash;
}

u64 combineDrawUniformPayloadHashes(const DrawUniformPayloadHashes &hashes,
                                    u32 clipPlaneMask,
                                    bool recordSnapshotPerf = false) {
  const bool recordPerf = recordSnapshotPerf && dxmt9::perf::enabled();
  auto recorder = [&](void (*fn)(std::uint64_t)) {
    return recordPerf ? fn : nullptr;
  };

  PerfScope scope(recorder(
      dxmt9::perf::countD3D9SnapshotUniformBuildPayloadCombineHashCpuTime));
  u64 hash = hashCombine(kFnvOffset, hashes.vertexConstantsHash);
  hash = hashCombine(hash, hashes.pixelConstantsHash);
  hash = hashCombine(hash, hashes.worldViewProjHash);
  hash = hashCombine(hash, hashes.ffpViewHash);
  hash = hashCombine(hash, hashes.ffpWorldViewHash);
  hash = hashCombine(hash, hashes.ffpNormalMatrixHash);
  hash = hashCombine(hash, hashes.materialHash);
  for (const auto lightHash : hashes.lightHashes) {
    hash = hashCombine(hash, lightHash);
  }
  hash = hashCombine(hash, hashes.ffpBlendWorldViewProjHash);
  hash = hashCombine(hash, hashes.ffpBlendWorldViewHash);
  hash = hashCombine(hash, hashes.ffpBlendNormalMatrixHash);
  hash = hashCombine(hash, hashes.textureTransformsHash);
  hash = hashCombine(hash, clipPlaneMask);
  hash = hashCombine(hash, hashes.clipPlanesHash);
  return hash;
}

u64 combineDrawUniformFixedPayloadHash(
    const DrawUniformPayloadHashes &hashes, u32 clipPlaneMask) {
  u64 hash = hashCombine(kFnvOffset, hashes.worldViewProjHash);
  hash = hashCombine(hash, hashes.ffpViewHash);
  hash = hashCombine(hash, hashes.ffpWorldViewHash);
  hash = hashCombine(hash, hashes.ffpNormalMatrixHash);
  hash = hashCombine(hash, hashes.materialHash);
  for (const auto lightHash : hashes.lightHashes) {
    hash = hashCombine(hash, lightHash);
  }
  hash = hashCombine(hash, hashes.ffpBlendWorldViewProjHash);
  hash = hashCombine(hash, hashes.ffpBlendWorldViewHash);
  hash = hashCombine(hash, hashes.ffpBlendNormalMatrixHash);
  hash = hashCombine(hash, hashes.textureTransformsHash);
  hash = hashCombine(hash, clipPlaneMask);
  hash = hashCombine(hash, hashes.clipPlanesHash);
  return hash;
}

u64 hashDrawUniformPayload(const DrawUniformPayload &payload,
                           DrawUniformPayloadHashes *componentHashes = nullptr,
                           DrawUniformPayloadHashOptions options = {}) {
  DrawUniformPayloadHashes hashes{};
  hashDrawUniformShaderConstantComponents(payload, hashes, options);
  if (options.reusableNonConstantHashes) {
    copyDrawUniformNonConstantHashes(
        hashes, *options.reusableNonConstantHashes);
  } else {
    hashDrawUniformNonConstantComponents(payload, hashes, options);
  }
  if (componentHashes) {
    *componentHashes = hashes;
  }
  return combineDrawUniformPayloadHashes(
      hashes, payload.clipPlaneMask, options.recordSnapshotPerf);
}

void applyDrawUniformPayloadHashes(FlatDrawStateRecord &hot,
                                   const DrawUniformPayloadHashes &hashes) {
  hot.vertexConstantsHash = hashes.vertexConstantsHash;
  hot.pixelConstantsHash = hashes.pixelConstantsHash;
  hot.worldViewProjHash = hashes.worldViewProjHash;
  hot.ffpBlendWorldViewProjHash = hashes.ffpBlendWorldViewProjHash;
  hot.textureTransformsHash = hashes.textureTransformsHash;
  hot.nonIdentityTextureTransformStageMask =
      hashes.nonIdentityTextureTransformStageMask;
  hot.clipPlanesHash = hashes.clipPlanesHash;
  hot.key.vertexConstantsHash = hashes.vertexConstantsHash;
  hot.key.pixelConstantsHash = hashes.pixelConstantsHash;
  hot.key.worldViewProjHash = hashes.worldViewProjHash;
  hot.key.ffpBlendWorldViewProjHash = hashes.ffpBlendWorldViewProjHash;
  hot.key.textureTransformsHash = hashes.textureTransformsHash;
  hot.key.nonIdentityTextureTransformStageMask =
      hashes.nonIdentityTextureTransformStageMask;
  hot.key.clipPlanesHash = hashes.clipPlanesHash;
}

u64 hashViewportScissor(const ViewportScissor &viewport) {
  u64 hash = hashCombine(kFnvOffset, hashTrivial(viewport.viewport));
  hash = hashCombine(hash, hashTrivial(viewport.scissor));
  hash = hashCombine(hash, viewport.scissorEnabled ? 1u : 0u);
  return hash;
}

bool renderTraceEnabled() {
  static const bool enabled = [] {
    return dxmt9::util::getenvFlag("DXMT_TRACE_RENDER");
  }();
  return enabled;
}


bool batchMissSemanticReuseProbeEnabled() {
  static const bool enabled = [] {
    return dxmt9::util::getenvFlag(
        "DXMT9_PERF_BATCH_MISS_SEMANTIC_REUSE_PROBE");
  }();
  return enabled;
}

bool batchMissShaderHashMemoProbeEnabled() {
  static const bool enabled = [] {
    return dxmt9::util::getenvFlag(
        "DXMT9_PERF_BATCH_MISS_SHADER_HASH_MEMO_PROBE");
  }();
  return enabled;
}

void emitRenderTrace(const char *fmt, ...) {
  if (!renderTraceEnabled()) {
    return;
  }
  va_list args;
  va_start(args, fmt);
  dxmt9::util::vlogf(dxmt9::util::LogLevel::Info, "dxmt9-render", fmt, args);
  va_end(args);
}

// D3DFVF_POSITION_MASK spans bit 14 (D3DFVF_XYZW = 0x4002), so the stride
// inferer must read 4-component XYZW positions as 16 bytes — not collapse
// them to XYZ via the legacy 0x000e mask. Aligns with the shared
// `dxmt9_ffp_shaders.hpp` constant. Wine reference: visual.c `test_ffp_w`.
constexpr u32 kFvfPositionMask = 0x400eu;
constexpr u32 kFvfXyzrhw = 0x0004u;
constexpr u32 kFvfXyz = 0x0002u;
constexpr u32 kFvfXyzw = 0x4002u;
constexpr u32 kFvfNormal = 0x0010u;
constexpr u32 kFvfDiffuse = 0x0040u;
constexpr u32 kFvfSpecular = 0x0080u;
constexpr u32 kFvfTexCountMask = 0x0f00u;
constexpr u32 kFvfTexCountShift = 8u;

constexpr u32 kDeclTypeFloat1 = 0u;
constexpr u32 kDeclTypeFloat2 = 1u;
constexpr u32 kDeclTypeFloat3 = 2u;
constexpr u32 kDeclTypeFloat4 = 3u;
constexpr u32 kDeclTypeD3DColor = 4u;
constexpr u32 kDeclTypeUByte4 = 5u;
constexpr u32 kDeclTypeShort2 = 6u;
constexpr u32 kDeclTypeShort4 = 7u;
constexpr u32 kDeclTypeUByte4N = 8u;
constexpr u32 kDeclTypeShort2N = 9u;
constexpr u32 kDeclTypeShort4N = 10u;
constexpr u32 kDeclTypeUShort2N = 11u;
constexpr u32 kDeclTypeUShort4N = 12u;
constexpr u32 kDeclTypeUDec3 = 13u;
constexpr u32 kDeclTypeDec3N = 14u;
constexpr u32 kDeclTypeFloat16_2 = 15u;
constexpr u32 kDeclTypeFloat16_4 = 16u;
u32 declTypeSize(u32 type) {
  switch (type) {
  case kDeclTypeFloat1:
    return 4;
  case kDeclTypeFloat2:
    return 8;
  case kDeclTypeFloat3:
    return 12;
  case kDeclTypeFloat4:
    return 16;
  case kDeclTypeD3DColor:
  case kDeclTypeUByte4:
  case kDeclTypeUByte4N:
  case kDeclTypeShort2:
  case kDeclTypeShort2N:
  case kDeclTypeUShort2N:
  case kDeclTypeUDec3:
  case kDeclTypeDec3N:
  case kDeclTypeFloat16_2:
    return 4;
  case kDeclTypeShort4:
  case kDeclTypeShort4N:
  case kDeclTypeUShort4N:
  case kDeclTypeFloat16_4:
    return 8;
  default:
    return 0;
  }
}

u32 fvfTexcoordSize(u32 fvf, u32 index) {
  const u32 code = (fvf >> (16u + index * 2u)) & 0x3u;
  switch (code) {
  case 1u:
    return 3;
  case 2u:
    return 4;
  case 3u:
    return 1;
  default:
    return 2;
  }
}

u32 inferStreamZeroStride(const VertexDeclSnapshot &vertexDecl) {
  if (vertexDecl.streams[0].stride != 0) {
    return vertexDecl.streams[0].stride;
  }

  if (!vertexDecl.elements.empty()) {
    u32 stride = 0;
    for (const auto &element : vertexDecl.elements) {
      if (element.stream != 0) {
        continue;
      }
      const u32 size = declTypeSize(element.type);
      if (size != 0) {
        stride = std::max(stride, static_cast<u32>(element.offset + size));
      }
    }
    return stride;
  }

  const u32 fvf = vertexDecl.fvf;
  const u32 position = fvf & kFvfPositionMask;
  u32 stride = 0;
  if (position == kFvfXyzrhw || position == kFvfXyzw) {
    stride = 16u;
  } else if (position == kFvfXyz) {
    stride = 12u;
  } else {
    return 0;
  }
  if ((fvf & kFvfNormal) != 0) {
    stride += 12u;
  }
  if ((fvf & kFvfDiffuse) != 0) {
    stride += 4u;
  }
  if ((fvf & kFvfSpecular) != 0) {
    stride += 4u;
  }
  const u32 texCount = (fvf & kFvfTexCountMask) >> kFvfTexCountShift;
  for (u32 i = 0; i < texCount; ++i) {
    stride += fvfTexcoordSize(fvf, i) * 4u;
  }
  return stride;
}

std::vector<u8> decomposeTriangleFanVertices(std::span<const u8> vertices,
                                             u32 primitiveCount, u32 stride) {
  if (primitiveCount == 0 || stride == 0) {
    return {};
  }

  const u32 sourceVertexCount = primitiveCount + 2u;
  const auto requiredBytes = static_cast<size_t>(sourceVertexCount) * stride;
  if (vertices.size() < requiredBytes) {
    return {};
  }

  std::vector<u8> out(static_cast<size_t>(primitiveCount) * 3u * stride);
  auto appendVertex = [&](size_t &offset, u32 index) {
    const auto sourceOffset = static_cast<size_t>(index) * stride;
    std::memcpy(out.data() + offset, vertices.data() + sourceOffset, stride);
    offset += stride;
  };

  size_t offset = 0;
  for (u32 i = 1; i + 1u < sourceVertexCount; ++i) {
    appendVertex(offset, 0);
    appendVertex(offset, i);
    appendVertex(offset, i + 1u);
  }
  return out;
}

template <typename Index>
bool writeTriangleFanIndexBytes(std::vector<u8> &out,
                                std::span<const u8> indices,
                                u32 primitiveCount) {
  static_assert(std::is_same_v<Index, u16> || std::is_same_v<Index, u32>);
  if (primitiveCount == 0) {
    out.clear();
    return true;
  }

  const auto sourceIndexCount = static_cast<size_t>(primitiveCount) + 2u;
  const auto requiredBytes = sourceIndexCount * sizeof(Index);
  if (indices.size() < requiredBytes) {
    out.clear();
    return false;
  }

  out.resize(static_cast<size_t>(primitiveCount) * 3u * sizeof(Index));
  auto readIndex = [&](size_t index) {
    Index value = 0;
    std::memcpy(&value, indices.data() + index * sizeof(Index), sizeof(Index));
    return value;
  };
  auto writeIndex = [&](size_t &offset, Index value) {
    std::memcpy(out.data() + offset, &value, sizeof(Index));
    offset += sizeof(Index);
  };

  const Index center = readIndex(0);
  size_t offset = 0;
  for (size_t i = 1; i + 1u < sourceIndexCount; ++i) {
    writeIndex(offset, center);
    writeIndex(offset, readIndex(i));
    writeIndex(offset, readIndex(i + 1u));
  }
  return true;
}

template <typename Index>
void writeSequentialTriangleFanIndexBytes(std::vector<u8> &out,
                                          u32 primitiveCount) {
  static_assert(std::is_same_v<Index, u16> || std::is_same_v<Index, u32>);
  if (primitiveCount == 0) {
    out.clear();
    return;
  }

  out.resize(static_cast<size_t>(primitiveCount) * 3u * sizeof(Index));
  auto writeIndex = [&](size_t &offset, Index value) {
    std::memcpy(out.data() + offset, &value, sizeof(Index));
    offset += sizeof(Index);
  };

  size_t offset = 0;
  for (u32 i = 1; i <= primitiveCount; ++i) {
    writeIndex(offset, Index{0});
    writeIndex(offset, static_cast<Index>(i));
    writeIndex(offset, static_cast<Index>(i + 1u));
  }
}

IndexType writeSequentialTriangleFanIndexPayload(std::vector<u8> &out,
                                                 u32 primitiveCount) {
  if (primitiveCount <=
      static_cast<u32>(std::numeric_limits<u16>::max()) - 1u) {
    writeSequentialTriangleFanIndexBytes<u16>(out, primitiveCount);
    return IndexType::UInt16;
  }
  writeSequentialTriangleFanIndexBytes<u32>(out, primitiveCount);
  return IndexType::UInt32;
}

bool writeIndexedTriangleFanIndexPayload(std::vector<u8> &out,
                                         std::span<const u8> sourceIndexBytes,
                                         u32 primitiveCount, u32 startIndex,
                                         IndexType indexType) {
  const auto indexSize =
      indexType == IndexType::UInt32 ? sizeof(u32) : sizeof(u16);
  const auto firstIndexByte = static_cast<size_t>(startIndex) * indexSize;
  if (firstIndexByte > sourceIndexBytes.size()) {
    return false;
  }
  const auto fanIndexBytes = sourceIndexBytes.subspan(firstIndexByte);
  return indexType == IndexType::UInt32
             ? writeTriangleFanIndexBytes<u32>(out, fanIndexBytes,
                                               primitiveCount)
             : writeTriangleFanIndexBytes<u16>(out, fanIndexBytes,
                                               primitiveCount);
}

template <typename StateValues> u64 hashMap(const StateValues &values) {
  u64 hash = kFnvOffset;
  std::vector<std::pair<u32, u32>> sorted;
  sorted.reserve(values.size());
  for (const auto &entry : values) {
    sorted.emplace_back(entry.first, entry.second);
  }
  std::sort(sorted.begin(), sorted.end(),
            [](auto &a, auto &b) { return a.first < b.first; });
  for (const auto &[key, value] : sorted) {
    hash = hashCombine(hash, key);
    hash = hashCombine(hash, value);
  }
  return hash;
}

template <std::size_t MaxEntries>
u64 hashMap(const StateValueTable<MaxEntries> &values) {
  return hashStateDigest(values.size(), values.rollingHash);
}

u64 hashMap(const TransformTable &values) {
  return hashStateDigest(values.size(), values.rollingHash);
}

u64 hashColor(const ColorRGBA &color) {
  return hashCombine(
      hashCombine(
          hashCombine(hashCombine(kFnvOffset, std::bit_cast<u32>(color.r)),
                      std::bit_cast<u32>(color.g)),
          std::bit_cast<u32>(color.b)),
      std::bit_cast<u32>(color.a));
}

Matrix4x4 identityMatrix() {
  Matrix4x4 matrix{};
  matrix.m = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
              0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
  return matrix;
}

Matrix4x4 multiplyMatrix(const Matrix4x4 &left, const Matrix4x4 &right) {
  Matrix4x4 result{};
  for (size_t row = 0; row < 4; ++row) {
    for (size_t col = 0; col < 4; ++col) {
      float sum = 0.0f;
      for (size_t k = 0; k < 4; ++k) {
        sum += left.m[row * 4 + k] * right.m[k * 4 + col];
      }
      result.m[row * 4 + col] = sum;
    }
  }
  return result;
}

Matrix4x4 transposeMatrix(const Matrix4x4 &matrix) {
  Matrix4x4 result{};
  for (size_t row = 0; row < 4; ++row) {
    for (size_t col = 0; col < 4; ++col) {
      result.m[row * 4 + col] = matrix.m[col * 4 + row];
    }
  }
  return result;
}

bool invertMatrix(const Matrix4x4 &matrix, Matrix4x4 *out) {
  std::array<double, 32> aug{};
  for (size_t row = 0; row < 4; ++row) {
    for (size_t col = 0; col < 4; ++col) {
      aug[row * 8 + col] = matrix.m[row * 4 + col];
      aug[row * 8 + 4 + col] = (row == col) ? 1.0 : 0.0;
    }
  }

  for (size_t col = 0; col < 4; ++col) {
    size_t pivotRow = col;
    double pivot = std::fabs(aug[pivotRow * 8 + col]);
    for (size_t row = col + 1; row < 4; ++row) {
      const double candidate = std::fabs(aug[row * 8 + col]);
      if (candidate > pivot) {
        pivot = candidate;
        pivotRow = row;
      }
    }
    if (pivot < 1.0e-20) {
      return false;
    }
    if (pivotRow != col) {
      for (size_t i = 0; i < 8; ++i) {
        std::swap(aug[col * 8 + i], aug[pivotRow * 8 + i]);
      }
    }
    const double invPivot = 1.0 / aug[col * 8 + col];
    for (size_t i = 0; i < 8; ++i) {
      aug[col * 8 + i] *= invPivot;
    }
    for (size_t row = 0; row < 4; ++row) {
      if (row == col) {
        continue;
      }
      const double factor = aug[row * 8 + col];
      if (factor == 0.0) {
        continue;
      }
      for (size_t i = 0; i < 8; ++i) {
        aug[row * 8 + i] -= factor * aug[col * 8 + i];
      }
    }
  }

  Matrix4x4 inverse{};
  for (size_t row = 0; row < 4; ++row) {
    for (size_t col = 0; col < 4; ++col) {
      inverse.m[row * 4 + col] = static_cast<float>(aug[row * 8 + 4 + col]);
    }
  }
  *out = inverse;
  return true;
}

ClipPlane transformFfpClipPlane(const Matrix4x4 &inverseViewProjection,
                                const ClipPlane &plane) {
  ClipPlane out{};
  for (size_t row = 0; row < 4; ++row) {
    float sum = 0.0f;
    for (size_t col = 0; col < 4; ++col) {
      sum += inverseViewProjection.m[row * 4 + col] * plane[col];
    }
    out[row] = sum;
  }
  return out;
}

Matrix4x4 lookupTransform(const DeviceState &state, u32 key) {
  if (state.transforms.contains(key)) {
    return state.transforms.at(key);
  }
  return identityMatrix();
}

u64 hashLight(const Light &light) {
  u64 hash = kFnvOffset;
  hash = hashCombine(hash, static_cast<u64>(light.type));
  hash = hashCombine(hash, static_cast<u64>(light.enabled));
  hash = hashCombine(hash, hashColor(light.diffuse));
  hash = hashCombine(hash, hashColor(light.specular));
  hash = hashCombine(hash, hashColor(light.ambient));
  for (float v : light.position) {
    hash = hashCombine(hash, std::bit_cast<u32>(v));
  }
  for (float v : light.direction) {
    hash = hashCombine(hash, std::bit_cast<u32>(v));
  }
  hash = hashCombine(hash, std::bit_cast<u32>(light.range));
  hash = hashCombine(hash, std::bit_cast<u32>(light.falloff));
  hash = hashCombine(hash, std::bit_cast<u32>(light.attenuation0));
  hash = hashCombine(hash, std::bit_cast<u32>(light.attenuation1));
  hash = hashCombine(hash, std::bit_cast<u32>(light.attenuation2));
  hash = hashCombine(hash, std::bit_cast<u32>(light.theta));
  hash = hashCombine(hash, std::bit_cast<u32>(light.phi));
  return hash;
}

u64 hashMaterial(const Material &material) {
  u64 hash = kFnvOffset;
  hash = hashCombine(hash, hashColor(material.emissive));
  hash = hashCombine(hash, hashColor(material.ambient));
  hash = hashCombine(hash, hashColor(material.diffuse));
  hash = hashCombine(hash, hashColor(material.specular));
  hash = hashCombine(hash, std::bit_cast<u32>(material.power));
  return hash;
}

u64 hashFfpVertexKey(const FfpVertexKey &key) {
  u64 hash = kFnvOffset;
  hash = hashCombine(hash, static_cast<u64>(key.lightingEnabled));
  hash = hashCombine(hash, static_cast<u64>(key.specularEnabled));
  hash = hashCombine(hash, static_cast<u64>(key.normalizeNormals));
  hash = hashCombine(hash, static_cast<u64>(key.localViewer));
  hash = hashCombine(hash, static_cast<u64>(key.colorVertexEnabled));
  for (bool enabled : key.lightEnabled) {
    hash = hashCombine(hash, static_cast<u64>(enabled));
  }
  for (u32 type : key.lightType) {
    hash = hashCombine(hash, type);
  }
  for (u32 mode : key.colorMaterialMode) {
    hash = hashCombine(hash, mode);
  }
  hash = hashCombine(hash, static_cast<u64>(key.fogMode));
  hash = hashCombine(hash, static_cast<u64>(key.fogFromVertex));
  hash = hashCombine(hash, static_cast<u64>(key.rangeFog));
  for (u32 value : key.texCoordGen) {
    hash = hashCombine(hash, value);
  }
  for (u32 value : key.texTransformFlags) {
    hash = hashCombine(hash, value);
  }
  hash = hashCombine(hash, key.vertexBlend);
  hash = hashCombine(hash, static_cast<u64>(key.indexedVertexBlend));
  hash = hashCombine(hash, key.clipPlaneMask);
  hash = hashCombine(hash, static_cast<u64>(key.pointSpriteEnable));
  hash = hashCombine(hash, static_cast<u64>(key.pointScaleEnable));
  return hash;
}

u64 hashFfpPixelKey(const FfpPixelKey &key) {
  u64 hash = kFnvOffset;
  for (const auto &stage : key.stages) {
    hash = hashCombine(hash, stage.colorOp);
    hash = hashCombine(hash, stage.colorArg1);
    hash = hashCombine(hash, stage.colorArg2);
    hash = hashCombine(hash, stage.colorArg0);
    hash = hashCombine(hash, stage.alphaOp);
    hash = hashCombine(hash, stage.alphaArg1);
    hash = hashCombine(hash, stage.alphaArg2);
    hash = hashCombine(hash, stage.alphaArg0);
    hash = hashCombine(hash, stage.resultArg);
    hash = hashCombine(hash, stage.texType);
    hash = hashCombine(hash, stage.texCoordIndex);
  }
  hash = hashCombine(hash, static_cast<u64>(key.fogMode));
  hash = hashCombine(hash, static_cast<u64>(key.alphaTestEnable));
  hash = hashCombine(hash, key.alphaTestFunc);
  hash = hashCombine(hash, static_cast<u64>(key.pointSpriteEnable));
  return hash;
}

u64 hashShaderBytecode(const ShaderBytecode &bytecode) {
  if (bytecode.hash != 0) {
    return bytecode.hash;
  }
  return hashBytes(std::as_bytes(
      std::span<const u8>(bytecode.bytes.data(), bytecode.bytes.size())));
}

u64 hashShaderRef(const ShaderRef &ref) {
  switch (ref.kind) {
  case ShaderRef::Kind::Bytecode:
    return ref.hash != 0 ? ref.hash : hashShaderBytecode(ref.bytecode);
  case ShaderRef::Kind::FixedFunctionVertex:
    return ref.vertexKey
               ? (ref.vertexKey->hash ? ref.vertexKey->hash
                                      : hashFfpVertexKey(*ref.vertexKey))
               : 0;
  case ShaderRef::Kind::FixedFunctionPixel:
    return ref.pixelKey ? (ref.pixelKey->hash ? ref.pixelKey->hash
                                              : hashFfpPixelKey(*ref.pixelKey))
                        : 0;
  case ShaderRef::Kind::None:
    return 0;
  }
  return 0;
}

[[maybe_unused]] u64 hashStateState(const DeviceState &state) {
  u64 hash = kFnvOffset;
  hash = hashCombine(hash, std::bit_cast<u32>(state.viewport.x));
  hash = hashCombine(hash, std::bit_cast<u32>(state.viewport.y));
  hash = hashCombine(hash, std::bit_cast<u32>(state.viewport.width));
  hash = hashCombine(hash, std::bit_cast<u32>(state.viewport.height));
  hash = hashCombine(hash, std::bit_cast<u32>(state.viewport.minZ));
  hash = hashCombine(hash, std::bit_cast<u32>(state.viewport.maxZ));
  hash = hashCombine(hash, static_cast<u64>(state.scissorEnabled));
  hash = hashCombine(hash, state.scissorRect.left);
  hash = hashCombine(hash, state.scissorRect.top);
  hash = hashCombine(hash, state.scissorRect.right);
  hash = hashCombine(hash, state.scissorRect.bottom);
  hash = hashCombine(hash, hashMap(state.renderStates));
  for (const auto &stage : state.textureStageStates) {
    hash = hashCombine(hash, hashMap(stage));
  }
  for (const auto &sampler : state.samplerStates) {
    hash = hashCombine(hash, hashMap(sampler));
  }
  hash = hashCombine(hash, hashMap(state.transforms));
  for (const auto &clipPlane : state.clipPlanes) {
    for (float value : clipPlane) {
      hash = hashCombine(hash, std::bit_cast<u32>(value));
    }
  }
  for (const auto &light : state.lights) {
    hash = hashCombine(hash, hashLight(light));
  }
  for (bool enabled : state.lightEnabled) {
    hash = hashCombine(hash, static_cast<u64>(enabled));
  }
  hash = hashCombine(hash, hashMaterial(state.material));
  hash = hashCombine(hash, state.fvf);
  hash = hashCombine(hash, hashShaderRef(state.vertexShader));
  hash = hashCombine(hash, hashShaderRef(state.pixelShader));
  hash = hashCombine(hash,
                     state.indexBuffer ? state.indexBuffer->handle().value : 0);
  hash =
      hashCombine(hash, static_cast<u64>(state.indexType == IndexType::UInt32));
  for (const auto &tex : state.textures) {
    hash = hashCombine(hash, tex ? tex->handle().value : 0);
    hash = hashCombine(hash, tex ? tex->lod() : 0);
  }
  for (const auto &rt : state.renderTargets) {
    hash = hashCombine(hash, rt.handle.value);
    hash = hashCombine(hash, rt.level);
  }
  hash = hashCombine(hash, state.depthStencil.handle.value);
  hash = hashCombine(hash, state.depthStencil.level);
  hash = hashCombine(hash, static_cast<u64>(state.inScene));
  return hash;
}

}  // namespace

std::vector<u32> decomposeTriangleFanIndices(std::span<const u32> indices) {
  std::vector<u32> out;
  if (indices.size() < 3) {
    return out;
  }
  out.reserve((indices.size() - 2) * 3);
  for (size_t i = 1; i + 1 < indices.size(); ++i) {
    out.push_back(indices[0]);
    out.push_back(indices[i]);
    out.push_back(indices[i + 1]);
  }
  return out;
}

PrimitiveType canonicalPrimitiveType(PrimitiveType primitiveType) {
  return primitiveType == PrimitiveType::TriangleFan
             ? PrimitiveType::TriangleList
             : primitiveType;
}

u32 drawInstanceCountFromState(const DeviceState& state) noexcept {
  const u32 frequency = state.streamFrequencies[0];
  if ((frequency & kStreamSourceIndexedData) == 0) {
    return 1;
  }
  return std::max(frequency & kStreamSourceFrequencyMask, 1u);
}

VertexDeclSnapshot makeVertexDeclSnapshotFromState(const DeviceState &state) {
  VertexDeclSnapshot decl = state.vertexDecl;
  decl.streams.fill({});
  for (size_t i = 0; i < kMaxStreams; ++i) {
    decl.streams[i].buffer = state.streamBuffers[i];
    decl.streams[i].offset = state.streamOffsets[i];
    decl.streams[i].stride = state.streamStrides[i];
  }
  return decl;
}

ViewportScissor makeViewportScissorFromState(const DeviceState &state) {
  ViewportScissor viewport{};
  viewport.viewport = state.viewport;
  viewport.scissor = state.scissorRect;
  viewport.scissorEnabled =
      state.renderStates.contains(RS_SCISSOR_TEST_ENABLE) &&
      state.renderStates.at(RS_SCISSOR_TEST_ENABLE) != 0;
  return viewport;
}

void refreshShaderLayoutExtraStreamStrides(DrawShaderLayoutContext &shaderLayout,
                                           const DeviceState &state) {
  for (auto &stream : shaderLayout.vertexDecl.streams) {
    stream.buffer = nullptr;
    stream.offset = 0;
    stream.stride = 0;
  }
  for (size_t i = 1; i < kMaxStreams; ++i) {
    shaderLayout.vertexDecl.streams[i].stride = state.streamStrides[i];
  }
}

u32 clipPlaneMaskFromState(const DeviceState &state) {
  if (state.renderStates.contains(RS_CLIPPING) &&
      state.renderStates.at(RS_CLIPPING) == 0u) {
    return 0u;
  }
  return state.renderStates.contains(RS_CLIP_PLANE_ENABLE)
             ? state.renderStates.at(RS_CLIP_PLANE_ENABLE)
             : 0u;
}

std::array<Matrix4x4, kMaxTextureStages>
makeTextureTransformsFromState(const DeviceState &state) {
  std::array<Matrix4x4, kMaxTextureStages> transforms{};
  for (size_t i = 0; i < kMaxTextureStages; ++i) {
    transforms[i] =
        lookupTransform(state, XFORM_TEXTURE_BASE + static_cast<u32>(i));
  }
  return transforms;
}

std::array<ClipPlane, kMaxClipPlanes>
makeClipPlanesFromState(const DeviceState &state, u32 clipPlaneMask) {
  std::array<ClipPlane, kMaxClipPlanes> clipPlanes{};
  // Fixed-function planes are world-space; programmable planes are already
  // in the clip space produced by the app's vertex shader.
  const bool programmable =
      state.vertexShader.kind == ShaderRef::Kind::Bytecode;
  Matrix4x4 inverseViewProjection{};
  bool hasInverseViewProjection = false;
  if (!programmable) {
    const Matrix4x4 view = lookupTransform(state, XFORM_VIEW);
    const Matrix4x4 projection = lookupTransform(state, XFORM_PROJECTION);
    hasInverseViewProjection = invertMatrix(
        multiplyMatrix(view, projection), &inverseViewProjection);
  }
  for (size_t i = 0; i < kMaxClipPlanes; ++i) {
    if ((clipPlaneMask & (1u << i)) != 0) {
      clipPlanes[i] = programmable || !hasInverseViewProjection
                          ? state.clipPlanes[i]
                          : transformFfpClipPlane(inverseViewProjection,
                                                  state.clipPlanes[i]);
    }
  }
  return clipPlanes;
}

Matrix4x4 makeWorldViewProjFromState(const DeviceState &state) {
  const Matrix4x4 world = lookupTransform(state, XFORM_WORLD_BASE);
  const Matrix4x4 view = lookupTransform(state, XFORM_VIEW);
  const Matrix4x4 proj = lookupTransform(state, XFORM_PROJECTION);
  return multiplyMatrix(multiplyMatrix(world, view), proj);
}

Matrix4x4 makeWorldViewFromState(const DeviceState &state) {
  const Matrix4x4 world = lookupTransform(state, XFORM_WORLD_BASE);
  const Matrix4x4 view = lookupTransform(state, XFORM_VIEW);
  return multiplyMatrix(world, view);
}

Matrix4x4 makeNormalMatrixFromWorldView(const Matrix4x4 &worldView) {
  Matrix4x4 inverse{};
  if (!invertMatrix(worldView, &inverse)) {
    return identityMatrix();
  }
  return transposeMatrix(inverse);
}

struct FfpBlendTransforms {
  std::array<Matrix4x4, 4> worldViewProj{};
  std::array<Matrix4x4, 4> worldView{};
  std::array<Matrix4x4, 4> normalMatrix{};
};

FfpBlendTransforms makeBlendTransformsFromState(const DeviceState &state) {
  FfpBlendTransforms transforms{};
  const Matrix4x4 view = lookupTransform(state, XFORM_VIEW);
  const Matrix4x4 proj = lookupTransform(state, XFORM_PROJECTION);
  for (size_t i = 0; i < transforms.worldView.size(); ++i) {
    const Matrix4x4 world =
        lookupTransform(state, XFORM_WORLD_BASE + static_cast<u32>(i));
    transforms.worldView[i] = multiplyMatrix(world, view);
    transforms.normalMatrix[i] =
        makeNormalMatrixFromWorldView(transforms.worldView[i]);
    transforms.worldViewProj[i] =
        multiplyMatrix(transforms.worldView[i], proj);
  }
  return transforms;
}

ShaderRef makeVertexShaderRefFromState(const DeviceState &state) {
  if (state.vertexShader.kind == ShaderRef::Kind::Bytecode) {
    return state.vertexShader;
  }
  ShaderRef shader{};
  shader.kind = ShaderRef::Kind::FixedFunctionVertex;
  shader.vertexKey = makeFfpVertexKey(state);
  shader.hash = shader.vertexKey->hash;
  return shader;
}

ShaderRef makePixelShaderRefFromState(const DeviceState &state) {
  if (state.pixelShader.kind == ShaderRef::Kind::Bytecode) {
    return state.pixelShader;
  }
  ShaderRef shader{};
  shader.kind = ShaderRef::Kind::FixedFunctionPixel;
  shader.pixelKey = makeFfpPixelKey(state);
  shader.hash = shader.pixelKey->hash;
  return shader;
}

DrawShaderLayoutContext
makeDrawShaderLayoutContextFromState(const DeviceState &state) {
  DrawShaderLayoutContext context{};
  context.vertexDecl = makeVertexDeclSnapshotFromState(state);
  context.vertexShader = makeVertexShaderRefFromState(state);
  context.pixelShader = makePixelShaderRefFromState(state);
  context.vertexConstantUsage = scanShaderConstantUsage(context.vertexShader);
  context.pixelConstantUsage = scanShaderConstantUsage(context.pixelShader);
  context.clipPlaneMask = clipPlaneMaskFromState(state);
  return context;
}

DrawUniformPayload makeDrawUniformPayloadFromState(
    const DeviceState &state, u32 clipPlaneMask,
    DrawUniformPayloadHashes *componentHashes = nullptr,
    bool recordSnapshotPerf = false,
    const DrawShaderLayoutContext *shaderLayout = nullptr,
    const DrawUniformPayloadHashes *reusableNonConstantHashes = nullptr,
    const DrawUniformPayloadHashes *reusableShaderConstantHashes = nullptr,
    bool reuseVertexConstantsHash = false,
    bool reusePixelConstantsHash = false,
    u64 vertexConstantGeneration = 0,
    u64 pixelConstantGeneration = 0,
    detail::ShaderConstantHashIndex<kMaxVertexConstants>
        *vertexConstantHashIndex = nullptr,
    detail::ShaderConstantHashIndex<kMaxPixelConstants>
        *pixelConstantHashIndex = nullptr) {
  const bool recordPerf = recordSnapshotPerf && dxmt9::perf::enabled();
  auto recorder = [&](void (*fn)(std::uint64_t)) {
    return recordPerf ? fn : nullptr;
  };
  if (recordPerf) {
    dxmt9::perf::countD3D9SnapshotUniformBuildCall();
  }
  DrawUniformPayload payload{};
  {
    PerfScope scope(recorder(
        dxmt9::perf::countD3D9SnapshotUniformBuildVsConstCopyCpuTime));
    payload.vsConst = state.vsConst;
  }
  {
    PerfScope scope(recorder(
        dxmt9::perf::countD3D9SnapshotUniformBuildPsConstCopyCpuTime));
    payload.psConst = state.psConst;
  }
  {
    PerfScope scope(recorder(
        dxmt9::perf::countD3D9SnapshotUniformBuildFfpMatrixCpuTime));
    payload.ffpView = lookupTransform(state, XFORM_VIEW);
    payload.ffpWorldView = makeWorldViewFromState(state);
    payload.ffpNormalMatrix = makeNormalMatrixFromWorldView(payload.ffpWorldView);
    payload.worldViewProj = makeWorldViewProjFromState(state);
    const auto blendTransforms = makeBlendTransformsFromState(state);
    payload.ffpBlendWorldViewProj = blendTransforms.worldViewProj;
    payload.ffpBlendWorldView = blendTransforms.worldView;
    payload.ffpBlendNormalMatrix = blendTransforms.normalMatrix;
  }
  {
    PerfScope scope(recorder(
        dxmt9::perf::countD3D9SnapshotUniformBuildFfpMaterialLightCpuTime));
    payload.material = state.material;
    payload.lights = state.lights;
  }
  {
    PerfScope scope(recorder(
        dxmt9::perf::countD3D9SnapshotUniformBuildTextureTransformCpuTime));
    payload.textureTransforms = makeTextureTransformsFromState(state);
  }
  payload.clipPlaneMask = clipPlaneMask;
  {
    PerfScope scope(recorder(
        dxmt9::perf::countD3D9SnapshotUniformBuildClipPlaneCpuTime));
    payload.clipPlanes = makeClipPlanesFromState(state, clipPlaneMask);
  }
  {
    PerfScope scope(recorder(
        dxmt9::perf::countD3D9SnapshotUniformBuildHashCpuTime));
    const DrawUniformPayloadHashOptions options{
        .vertexUsage = shaderLayout ? &shaderLayout->vertexConstantUsage : nullptr,
        .pixelUsage = shaderLayout ? &shaderLayout->pixelConstantUsage : nullptr,
        .reusableNonConstantHashes = reusableNonConstantHashes,
        .reusableShaderConstantHashes = reusableShaderConstantHashes,
        .vertexConstantHashIndex = vertexConstantHashIndex,
        .pixelConstantHashIndex = pixelConstantHashIndex,
        .vertexConstantGeneration = vertexConstantGeneration,
        .pixelConstantGeneration = pixelConstantGeneration,
        .reuseVertexConstantsHash = reuseVertexConstantsHash,
        .reusePixelConstantsHash = reusePixelConstantsHash,
        .vertexUsageFromBytecode =
            shaderLayout && shaderLayout->vertexShader.kind == ShaderRef::Kind::Bytecode,
        .pixelUsageFromBytecode =
            shaderLayout && shaderLayout->pixelShader.kind == ShaderRef::Kind::Bytecode,
        .recordSnapshotPerf = recordSnapshotPerf,
    };
    DrawUniformPayloadHashes hashes{};
    payload.hash = hashDrawUniformPayload(payload, &hashes, options);
    payload.vertexConstantsHash = hashes.vertexConstantsHash;
    payload.pixelConstantsHash = hashes.pixelConstantsHash;
    payload.vertexFloatConstantCount = hashes.vertexFloatConstantCount;
    payload.vertexIntConstantCount = hashes.vertexIntConstantCount;
    payload.vertexBoolConstantCount = hashes.vertexBoolConstantCount;
    payload.pixelFloatConstantCount = hashes.pixelFloatConstantCount;
    payload.pixelIntConstantCount = hashes.pixelIntConstantCount;
    payload.pixelBoolConstantCount = hashes.pixelBoolConstantCount;
    payload.fixedPayloadHash =
        combineDrawUniformFixedPayloadHash(hashes, payload.clipPlaneMask);
    if (componentHashes) {
      *componentHashes = hashes;
    }
  }
  return payload;
}

void refreshDrawUniformPayloadShaderConstantsFromState(
    const DeviceState &state, DrawUniformPayload &payload,
    DrawUniformPayloadHashes &componentHashes,
    const DrawShaderLayoutContext &shaderLayout,
    bool recordSnapshotPerf = false,
    bool reuseVertexConstants = false,
    bool reusePixelConstants = false,
    u64 vertexConstantGeneration = 0,
    u64 pixelConstantGeneration = 0,
    detail::ShaderConstantHashIndex<kMaxVertexConstants>
        *vertexConstantHashIndex = nullptr,
    detail::ShaderConstantHashIndex<kMaxPixelConstants>
        *pixelConstantHashIndex = nullptr) {
  const bool recordPerf = recordSnapshotPerf && dxmt9::perf::enabled();
  auto recorder = [&](void (*fn)(std::uint64_t)) {
    return recordPerf ? fn : nullptr;
  };
  if (recordPerf) {
    dxmt9::perf::countD3D9SnapshotUniformBuildCall();
  }
  if (!reuseVertexConstants) {
    PerfScope scope(recorder(
        dxmt9::perf::countD3D9SnapshotUniformBuildVsConstCopyCpuTime));
    payload.vsConst = state.vsConst;
  }
  if (!reusePixelConstants) {
    PerfScope scope(recorder(
        dxmt9::perf::countD3D9SnapshotUniformBuildPsConstCopyCpuTime));
    payload.psConst = state.psConst;
  }
  {
    PerfScope scope(recorder(
        dxmt9::perf::countD3D9SnapshotUniformBuildHashCpuTime));
    const DrawUniformPayloadHashOptions options{
        .vertexUsage = &shaderLayout.vertexConstantUsage,
        .pixelUsage = &shaderLayout.pixelConstantUsage,
        .reusableShaderConstantHashes = &componentHashes,
        .vertexConstantHashIndex = vertexConstantHashIndex,
        .pixelConstantHashIndex = pixelConstantHashIndex,
        .vertexConstantGeneration = vertexConstantGeneration,
        .pixelConstantGeneration = pixelConstantGeneration,
        .reuseVertexConstantsHash = reuseVertexConstants,
        .reusePixelConstantsHash = reusePixelConstants,
        .vertexUsageFromBytecode =
            shaderLayout.vertexShader.kind == ShaderRef::Kind::Bytecode,
        .pixelUsageFromBytecode =
            shaderLayout.pixelShader.kind == ShaderRef::Kind::Bytecode,
        .recordSnapshotPerf = recordSnapshotPerf,
    };
    hashDrawUniformShaderConstantComponents(payload, componentHashes, options);
    payload.vertexConstantsHash = componentHashes.vertexConstantsHash;
    payload.pixelConstantsHash = componentHashes.pixelConstantsHash;
    payload.vertexFloatConstantCount = componentHashes.vertexFloatConstantCount;
    payload.vertexIntConstantCount = componentHashes.vertexIntConstantCount;
    payload.vertexBoolConstantCount = componentHashes.vertexBoolConstantCount;
    payload.pixelFloatConstantCount = componentHashes.pixelFloatConstantCount;
    payload.pixelIntConstantCount = componentHashes.pixelIntConstantCount;
    payload.pixelBoolConstantCount = componentHashes.pixelBoolConstantCount;
    payload.fixedPayloadHash = combineDrawUniformFixedPayloadHash(
        componentHashes, payload.clipPlaneMask);
    payload.hash = combineDrawUniformPayloadHashes(
        componentHashes, payload.clipPlaneMask, recordSnapshotPerf);
  }
}

namespace fixture {

DrawDesc makeDrawDescFromState(const DeviceState &state,
                               const DrawCallArgs &args) {
  DrawDesc desc;
  const auto shaderLayout = makeDrawShaderLayoutContextFromState(state);
  const auto uniforms =
      makeDrawUniformPayloadFromState(state, shaderLayout.clipPlaneMask);
  desc.primitiveType = canonicalPrimitiveType(args.primitiveType);
  desc.primitiveCount = args.primitiveCount;
  desc.startVertex = args.startVertex;
  desc.baseVertexIndex = args.baseVertexIndex;
  desc.startIndex = args.startIndex;
  desc.indexType = args.indexType;
  desc.indexBuffer = state.indexBuffer ? state.indexBuffer->handle() : Handle{};
  desc.vertexDecl = shaderLayout.vertexDecl;
  desc.streamFrequencies = state.streamFrequencies;
  desc.rs.values = state.renderStates;
  for (size_t i = 0; i < kMaxTextures; ++i) {
    desc.textures[i].handle =
        state.textures[i] ? state.textures[i]->handle() : Handle{};
    desc.textures[i].lod = state.textures[i] ? state.textures[i]->lod() : 0;
    if (i < kMaxTextureStages) {
      desc.textures[i].stageStates = state.textureStageStates[i];
    } else {
      desc.textures[i].stageStates.clear();
    }
  }
  for (size_t i = 0; i < kMaxSamplers; ++i) {
    desc.samplers[i].states = state.samplerStates[i];
  }
  desc.rts.color = state.renderTargets;
  desc.rts.depthStencil = state.depthStencil;
  desc.viewport = makeViewportScissorFromState(state);
  desc.clipPlaneMask = shaderLayout.clipPlaneMask;
  desc.worldViewProj = uniforms.worldViewProj;
  desc.ffpView = uniforms.ffpView;
  desc.ffpWorldView = uniforms.ffpWorldView;
  desc.ffpNormalMatrix = uniforms.ffpNormalMatrix;
  desc.material = uniforms.material;
  desc.lights = uniforms.lights;
  desc.ffpBlendWorldViewProj = uniforms.ffpBlendWorldViewProj;
  desc.ffpBlendWorldView = uniforms.ffpBlendWorldView;
  desc.ffpBlendNormalMatrix = uniforms.ffpBlendNormalMatrix;
  desc.textureTransforms = uniforms.textureTransforms;
  desc.clipPlanes = uniforms.clipPlanes;
  desc.vertexShader = shaderLayout.vertexShader;
  desc.pixelShader = shaderLayout.pixelShader;
  desc.vsConst = uniforms.vsConst;
  desc.psConst = uniforms.psConst;
  return desc;
}

FlatDrawStateKey makeFlatDrawStateKey(const DrawDesc &desc) {
  FlatDrawStateKey key{};

  for (size_t i = 0; i < kMaxStreams; ++i) {
    const auto &stream = desc.vertexDecl.streams[i];
    key.streamBuffers[i] = stream.buffer ? stream.buffer->handle() : Handle{};
    key.streamOffsets[i] = stream.offset;
    key.streamStrides[i] = stream.stride;
    key.streamFrequencies[i] = desc.streamFrequencies[i];
    if (key.streamBuffers[i]) {
      key.streamMask |= 1u << i;
    }
  }

  key.indexBuffer = desc.indexBuffer;
  key.vertexElementCount = static_cast<u32>(desc.vertexDecl.elements.size());
  key.fvf = desc.vertexDecl.fvf;
  key.vertexDeclHash = hashVertexDeclElements(desc.vertexDecl);
  key.vertexShaderKind = desc.vertexShader.kind;
  key.pixelShaderKind = desc.pixelShader.kind;
  key.vertexShaderHash = hashShaderRefSummary(desc.vertexShader);
  key.pixelShaderHash = hashShaderRefSummary(desc.pixelShader);
  const auto vertexUsage = scanShaderConstantUsage(desc.vertexShader);
  const auto pixelUsage = scanShaderConstantUsage(desc.pixelShader);
  key.vertexConstantsHash =
      hashShaderConstantsForUsage(desc.vsConst, &vertexUsage).hash;
  key.pixelConstantsHash =
      hashShaderConstantsForUsage(desc.psConst, &pixelUsage).hash;

  for (size_t i = 0; i < kMaxTextures; ++i) {
    key.textures[i] = desc.textures[i].handle;
    key.textureLods[i] = desc.textures[i].lod;
    if (key.textures[i]) {
      key.textureMask |= 1u << i;
    }
    if (i < kMaxTextureStages) {
      key.textureStageStateHashes[i] =
          hashStateMap(desc.textures[i].stageStates);
    }
  }

  for (size_t i = 0; i < kMaxSamplers; ++i) {
    key.samplerStateHashes[i] = hashStateMap(desc.samplers[i].states);
    if (!desc.samplers[i].states.empty()) {
      key.samplerStateMask |= 1u << i;
    }
  }

  key.renderStateHash = hashStateMap(desc.rs.values);
  key.colorAttachments = desc.rts.color;
  key.depthStencil = desc.rts.depthStencil;
  for (size_t i = 0; i < kMaxRenderTargets; ++i) {
    if (key.colorAttachments[i].handle) {
      key.renderTargetMask |= 1u << i;
    }
  }

  key.viewportHash = hashViewportScissor(desc.viewport);
  key.worldViewProjHash = hashTrivial(desc.worldViewProj);
  key.ffpBlendWorldViewProjHash = hashBlendWorldViewProj(desc.ffpBlendWorldViewProj);
  key.textureTransformsHash = hashTextureTransforms(desc.textureTransforms);
  key.nonIdentityTextureTransformStageMask =
      nonIdentityTextureTransformStageMask(desc.textureTransforms);
  key.clipPlaneMask = desc.clipPlaneMask;
  key.clipPlanesHash = hashClipPlanes(desc.clipPlanes);

  return key;
}

FlatDrawStateRecord makeFlatDrawStateRecord(const DrawDesc &desc) {
  FlatDrawStateRecord record{};
  record.key = makeFlatDrawStateKey(desc);
  record.streamBuffers = record.key.streamBuffers;
  record.streamOffsets = record.key.streamOffsets;
  record.streamStrides = record.key.streamStrides;
  record.streamFrequencies = record.key.streamFrequencies;
  record.streamMask = record.key.streamMask;
  record.indexBuffer = record.key.indexBuffer;
  record.textures = record.key.textures;
  record.textureLods = record.key.textureLods;
  record.textureMask = record.key.textureMask;
  record.renderStates = makePrioritizedFlatStateSet<kMaxFlatRenderStates>(
      desc.rs.values, kFlatRenderStatePreservedKeys);
  for (size_t i = 0; i < kMaxTextureStages; ++i) {
    record.textureStageStates[i] =
        makeFlatStateSet<kMaxFlatTextureStageStates>(desc.textures[i].stageStates);
  }
  for (size_t i = 0; i < kMaxSamplers; ++i) {
    record.samplerStates[i] =
        makeFlatStateSet<kMaxSamplerStates>(desc.samplers[i].states);
  }
  record.colorAttachments = record.key.colorAttachments;
  record.depthStencil = record.key.depthStencil;
  record.renderTargetMask = record.key.renderTargetMask;
  record.viewport = desc.viewport;
  record.vertexConstantsHash = record.key.vertexConstantsHash;
  record.pixelConstantsHash = record.key.pixelConstantsHash;
  record.worldViewProjHash = record.key.worldViewProjHash;
  record.ffpBlendWorldViewProjHash = record.key.ffpBlendWorldViewProjHash;
  record.textureTransformsHash = record.key.textureTransformsHash;
  record.nonIdentityTextureTransformStageMask =
      record.key.nonIdentityTextureTransformStageMask;
  record.clipPlaneMask = record.key.clipPlaneMask;
  record.clipPlanesHash = record.key.clipPlanesHash;
  return record;
}

} // namespace fixture

namespace {

void bumpGeneration(u64 &generation) noexcept {
  ++generation;
  if (generation == 0) {
    generation = 1;
  }
}

struct FlatDrawStateRecordBuildPerfOptions {
  bool recordBatchMissHotBuild = false;
  bool preserveReusableFlatStateSets = false;
  const FlatRenderStateSet *reusableRenderStates = nullptr;
  const std::array<FlatStateSet<kMaxFlatTextureStageStates>,
                   kMaxTextureStages> *reusableTextureStageStates = nullptr;
  const std::array<FlatStateSet<kMaxSamplerStates>,
                   kMaxSamplers> *reusableSamplerStates = nullptr;
};

struct FlatDrawStateUniformInputs {
  const DrawUniformPayloadHashes *hashes = nullptr;
  const DrawUniformPayload *payload = nullptr;
};

FlatDrawStateKey makeFlatDrawStateKeyFromState(
    const DeviceState &state, const DrawShaderLayoutContext &shaderLayout,
    const FlatDrawStateUniformInputs &uniforms,
    const ViewportScissor &viewport,
    FlatDrawStateRecordBuildPerfOptions perfOptions = {}) {
  const bool recordPerf =
      perfOptions.recordBatchMissHotBuild && dxmt9::perf::enabled();
  auto recorder = [&](void (*fn)(std::uint64_t)) {
    return recordPerf ? fn : nullptr;
  };
  FlatDrawStateKey key = [&] {
    PerfScope scope(recorder(
        dxmt9::perf::countD3D9SnapshotCacheBatchMissHotBuildKeyZeroInitCpuTime));
    return FlatDrawStateKey{};
  }();

  {
    PerfScope scope(recorder(
        dxmt9::perf::countD3D9SnapshotCacheBatchMissHotBuildKeyStreamCpuTime));
    for (size_t i = 0; i < kMaxStreams; ++i) {
      key.streamBuffers[i] =
          state.streamBuffers[i] ? state.streamBuffers[i]->handle() : Handle{};
      key.streamOffsets[i] = state.streamOffsets[i];
      key.streamStrides[i] = state.streamStrides[i];
      key.streamFrequencies[i] = state.streamFrequencies[i];
      if (key.streamBuffers[i]) {
        key.streamMask |= 1u << i;
      }
    }
    key.indexBuffer = state.indexBuffer ? state.indexBuffer->handle() : Handle{};
  }

  {
    PerfScope scope(recorder(
        dxmt9::perf::countD3D9SnapshotCacheBatchMissHotBuildKeyShaderCpuTime));
    key.vertexElementCount =
        static_cast<u32>(shaderLayout.vertexDecl.elements.size());
    key.fvf = shaderLayout.vertexDecl.fvf;
    key.vertexDeclHash = hashVertexDeclElements(shaderLayout.vertexDecl);
    key.vertexShaderKind = shaderLayout.vertexShader.kind;
    key.pixelShaderKind = shaderLayout.pixelShader.kind;
    key.vertexShaderHash = hashShaderRefSummary(shaderLayout.vertexShader);
    key.pixelShaderHash = hashShaderRefSummary(shaderLayout.pixelShader);
  }

  {
    PerfScope scope(recorder(
        dxmt9::perf::countD3D9SnapshotCacheBatchMissHotBuildKeyConstantCpuTime));
    key.vertexConstantsHash =
        uniforms.hashes ? uniforms.hashes->vertexConstantsHash
                        : hashTrivial(state.vsConst);
    key.pixelConstantsHash =
        uniforms.hashes ? uniforms.hashes->pixelConstantsHash
                        : hashTrivial(state.psConst);
  }

  {
    PerfScope scope(recorder(
        dxmt9::perf::countD3D9SnapshotCacheBatchMissHotBuildKeyTextureCpuTime));
    for (size_t i = 0; i < kMaxTextures; ++i) {
      key.textures[i] =
          state.textures[i] ? state.textures[i]->handle() : Handle{};
      key.textureLods[i] = state.textures[i] ? state.textures[i]->lod() : 0;
      if (key.textures[i]) {
        key.textureMask |= 1u << i;
      }
      if (i < kMaxTextureStages) {
        key.textureStageStateHashes[i] =
            hashStateMap(state.textureStageStates[i]);
      }
    }
  }

  {
    PerfScope scope(recorder(
        dxmt9::perf::countD3D9SnapshotCacheBatchMissHotBuildKeySamplerCpuTime));
    for (size_t i = 0; i < kMaxSamplers; ++i) {
      key.samplerStateHashes[i] = hashStateMap(state.samplerStates[i]);
      if (!state.samplerStates[i].empty()) {
        key.samplerStateMask |= 1u << i;
      }
    }
  }

  {
    PerfScope scope(recorder(dxmt9::perf::
        countD3D9SnapshotCacheBatchMissHotBuildKeyRenderStateCpuTime));
    key.renderStateHash = hashStateMap(state.renderStates);
  }

  {
    PerfScope scope(recorder(dxmt9::perf::
        countD3D9SnapshotCacheBatchMissHotBuildKeyAttachmentCpuTime));
    key.colorAttachments = state.renderTargets;
    key.depthStencil = state.depthStencil;
    for (size_t i = 0; i < kMaxRenderTargets; ++i) {
      if (key.colorAttachments[i].handle) {
        key.renderTargetMask |= 1u << i;
      }
    }
  }

  {
    PerfScope scope(recorder(
        dxmt9::perf::countD3D9SnapshotCacheBatchMissHotBuildKeyUniformCpuTime));
    DXMT_ASSERT(uniforms.hashes || uniforms.payload);
    const auto *payload = uniforms.payload;
    key.viewportHash = hashViewportScissor(viewport);
    key.worldViewProjHash =
        uniforms.hashes
            ? uniforms.hashes->worldViewProjHash
            : (payload ? hashTrivial(payload->worldViewProj) : 0);
    key.ffpBlendWorldViewProjHash =
        uniforms.hashes
            ? uniforms.hashes->ffpBlendWorldViewProjHash
            : (payload ? hashBlendWorldViewProj(
                             payload->ffpBlendWorldViewProj)
                       : 0);
    key.textureTransformsHash =
        uniforms.hashes
            ? uniforms.hashes->textureTransformsHash
            : (payload ? hashTextureTransforms(payload->textureTransforms) : 0);
    key.nonIdentityTextureTransformStageMask =
        uniforms.hashes
            ? uniforms.hashes->nonIdentityTextureTransformStageMask
            : (payload ? nonIdentityTextureTransformStageMask(
                             payload->textureTransforms)
                       : 0);
    key.clipPlaneMask = shaderLayout.clipPlaneMask;
    key.clipPlanesHash =
        uniforms.hashes ? uniforms.hashes->clipPlanesHash
                        : (payload ? hashClipPlanes(payload->clipPlanes) : 0);
  }
  return key;
}

void refreshFlatDrawStateRecordFromState(
    FlatDrawStateRecord &record, const DeviceState &state,
    const DrawShaderLayoutContext &shaderLayout,
    const FlatDrawStateUniformInputs &uniforms,
    const ViewportScissor &viewport,
    FlatDrawStateRecordBuildPerfOptions perfOptions = {}) {
  const bool recordPerf =
      perfOptions.recordBatchMissHotBuild && dxmt9::perf::enabled();
  auto recorder = [&](void (*fn)(std::uint64_t)) {
    return recordPerf ? fn : nullptr;
  };
  {
    PerfScope scope(recorder(
        dxmt9::perf::countD3D9SnapshotCacheBatchMissHotBuildKeyCpuTime));
    record.key =
        makeFlatDrawStateKeyFromState(state, shaderLayout, uniforms,
                                      viewport, perfOptions);
  }
  {
    PerfScope scope(recorder(dxmt9::perf::
        countD3D9SnapshotCacheBatchMissHotBuildBindingCopyCpuTime));
    record.streamBuffers = record.key.streamBuffers;
    record.streamOffsets = record.key.streamOffsets;
    record.streamStrides = record.key.streamStrides;
    record.streamFrequencies = record.key.streamFrequencies;
    record.streamMask = record.key.streamMask;
    record.indexBuffer = record.key.indexBuffer;
    record.textures = record.key.textures;
    record.textureLods = record.key.textureLods;
    record.textureMask = record.key.textureMask;
  }
  {
    PerfScope scope(recorder(dxmt9::perf::
        countD3D9SnapshotCacheBatchMissHotBuildRenderStateCpuTime));
    if (perfOptions.reusableRenderStates) {
      if (!perfOptions.preserveReusableFlatStateSets) {
        record.renderStates = *perfOptions.reusableRenderStates;
      }
    } else {
      record.renderStates = makePrioritizedFlatStateSet<kMaxFlatRenderStates>(
          state.renderStates, kFlatRenderStatePreservedKeys);
    }
  }
  {
    PerfScope scope(recorder(dxmt9::perf::
        countD3D9SnapshotCacheBatchMissHotBuildTextureStageStateCpuTime));
    if (perfOptions.reusableTextureStageStates) {
      if (!perfOptions.preserveReusableFlatStateSets) {
        record.textureStageStates = *perfOptions.reusableTextureStageStates;
      }
    } else {
      for (size_t i = 0; i < kMaxTextureStages; ++i) {
        record.textureStageStates[i] = makeFlatStateSet<
            kMaxFlatTextureStageStates>(state.textureStageStates[i]);
      }
    }
  }
  {
    PerfScope scope(recorder(dxmt9::perf::
        countD3D9SnapshotCacheBatchMissHotBuildSamplerStateCpuTime));
    if (perfOptions.reusableSamplerStates) {
      if (!perfOptions.preserveReusableFlatStateSets) {
        record.samplerStates = *perfOptions.reusableSamplerStates;
      }
    } else {
      for (size_t i = 0; i < kMaxSamplers; ++i) {
        record.samplerStates[i] =
            makeFlatStateSet<kMaxSamplerStates>(state.samplerStates[i]);
      }
    }
  }
  {
    PerfScope scope(recorder(
        dxmt9::perf::countD3D9SnapshotCacheBatchMissHotBuildTailCopyCpuTime));
    record.colorAttachments = record.key.colorAttachments;
    record.depthStencil = record.key.depthStencil;
    record.renderTargetMask = record.key.renderTargetMask;
    record.viewport = viewport;
    record.vertexConstantsHash = record.key.vertexConstantsHash;
    record.pixelConstantsHash = record.key.pixelConstantsHash;
    record.worldViewProjHash = record.key.worldViewProjHash;
    record.ffpBlendWorldViewProjHash = record.key.ffpBlendWorldViewProjHash;
    record.textureTransformsHash = record.key.textureTransformsHash;
    record.nonIdentityTextureTransformStageMask =
        record.key.nonIdentityTextureTransformStageMask;
    record.clipPlaneMask = record.key.clipPlaneMask;
    record.clipPlanesHash = record.key.clipPlanesHash;
  }
}

FlatDrawStateRecord makeFlatDrawStateRecordFromState(
    const DeviceState &state, const DrawShaderLayoutContext &shaderLayout,
    const FlatDrawStateUniformInputs &uniforms,
    const ViewportScissor &viewport,
    FlatDrawStateRecordBuildPerfOptions perfOptions = {}) {
  const bool recordPerf =
      perfOptions.recordBatchMissHotBuild && dxmt9::perf::enabled();
  auto recorder = [&](void (*fn)(std::uint64_t)) {
    return recordPerf ? fn : nullptr;
  };
  FlatDrawStateRecord record = [&] {
    PerfScope scope(recorder(
        dxmt9::perf::countD3D9SnapshotCacheBatchMissHotBuildZeroInitCpuTime));
    return FlatDrawStateRecord{};
  }();
  refreshFlatDrawStateRecordFromState(record, state, shaderLayout, uniforms,
                                      viewport, perfOptions);
  return record;
}

} // namespace

namespace fixture {

DrawShaderLayoutContext makeDrawShaderLayoutContext(const DrawDesc &desc) {
  DrawShaderLayoutContext context{};
  context.vertexDecl = desc.vertexDecl;
  context.vertexShader = desc.vertexShader;
  context.pixelShader = desc.pixelShader;
  context.vertexConstantUsage = scanShaderConstantUsage(context.vertexShader);
  context.pixelConstantUsage = scanShaderConstantUsage(context.pixelShader);
  context.clipPlaneMask = desc.clipPlaneMask;
  return context;
}

DrawUniformPayload makeDrawUniformPayload(const DrawDesc &desc) {
  DrawUniformPayload payload{};
  payload.vsConst = desc.vsConst;
  payload.psConst = desc.psConst;
  payload.worldViewProj = desc.worldViewProj;
  payload.ffpView = desc.ffpView;
  payload.ffpWorldView = desc.ffpWorldView;
  payload.ffpNormalMatrix = desc.ffpNormalMatrix;
  payload.material = desc.material;
  payload.lights = desc.lights;
  payload.ffpBlendWorldViewProj = desc.ffpBlendWorldViewProj;
  payload.ffpBlendWorldView = desc.ffpBlendWorldView;
  payload.ffpBlendNormalMatrix = desc.ffpBlendNormalMatrix;
  payload.textureTransforms = desc.textureTransforms;
  payload.clipPlaneMask = desc.clipPlaneMask;
  payload.clipPlanes = desc.clipPlanes;
  DrawUniformPayloadHashes hashes{};
  payload.hash = hashDrawUniformPayload(payload, &hashes);
  payload.vertexConstantsHash = hashes.vertexConstantsHash;
  payload.pixelConstantsHash = hashes.pixelConstantsHash;
  payload.vertexFloatConstantCount = hashes.vertexFloatConstantCount;
  payload.vertexIntConstantCount = hashes.vertexIntConstantCount;
  payload.vertexBoolConstantCount = hashes.vertexBoolConstantCount;
  payload.pixelFloatConstantCount = hashes.pixelFloatConstantCount;
  payload.pixelIntConstantCount = hashes.pixelIntConstantCount;
  payload.pixelBoolConstantCount = hashes.pixelBoolConstantCount;
  payload.fixedPayloadHash =
      combineDrawUniformFixedPayloadHash(hashes, payload.clipPlaneMask);
  return payload;
}

DrawDebugSnapshot makeDrawDebugSnapshot(const DrawDesc &desc,
                                        const FlatDrawStateRecord &hot) {
  DrawDebugSnapshot snapshot{};
  snapshot.primitiveType = desc.primitiveType;
  snapshot.primitiveCount = desc.primitiveCount;
  snapshot.startVertex = desc.startVertex;
  snapshot.baseVertexIndex = desc.baseVertexIndex;
  snapshot.startIndex = desc.startIndex;
  snapshot.indexType = desc.indexType;
  snapshot.userVertexBytes = static_cast<u32>(std::min<std::size_t>(
      desc.userVertexData.size(), std::numeric_limits<u32>::max()));
  snapshot.userIndexBytes = static_cast<u32>(std::min<std::size_t>(
      desc.userIndexData.size(), std::numeric_limits<u32>::max()));
  snapshot.streamMask = hot.streamMask;
  snapshot.textureMask = hot.textureMask;
  snapshot.samplerStateMask = hot.key.samplerStateMask;
  snapshot.renderTargetMask = hot.renderTargetMask;
  snapshot.renderStateHash = hot.key.renderStateHash;
  snapshot.vertexDeclHash = hot.key.vertexDeclHash;
  snapshot.vertexShaderHash = hot.key.vertexShaderHash;
  snapshot.pixelShaderHash = hot.key.pixelShaderHash;
  return snapshot;
}

} // namespace fixture

DrawDebugSnapshot makeDrawDebugSnapshot(const DrawCallArgs &args,
                                        const FlatDrawStateRecord &hot) {
  DrawDebugSnapshot snapshot{};
  snapshot.primitiveType = canonicalPrimitiveType(args.primitiveType);
  snapshot.primitiveCount = args.primitiveCount;
  snapshot.startVertex = args.startVertex;
  snapshot.baseVertexIndex = args.baseVertexIndex;
  snapshot.startIndex = args.startIndex;
  snapshot.indexType = args.indexType;
  snapshot.streamMask = hot.streamMask;
  snapshot.textureMask = hot.textureMask;
  snapshot.samplerStateMask = hot.key.samplerStateMask;
  snapshot.renderTargetMask = hot.renderTargetMask;
  snapshot.renderStateHash = hot.key.renderStateHash;
  snapshot.vertexDeclHash = hot.key.vertexDeclHash;
  snapshot.vertexShaderHash = hot.key.vertexShaderHash;
  snapshot.pixelShaderHash = hot.key.pixelShaderHash;
  return snapshot;
}

CanonicalDrawState makeCanonicalDrawStateFromState(const DeviceState &state,
                                                   const DrawCallArgs &args) {
  auto shaderLayout = makeDrawShaderLayoutContextFromState(state);
  DrawUniformPayloadHashes uniformHashes{};
  makeDrawUniformPayloadFromState(state, shaderLayout.clipPlaneMask,
                                  &uniformHashes,
                                  /*recordSnapshotPerf=*/false,
                                  &shaderLayout);
  const auto viewport = makeViewportScissorFromState(state);
  auto hot = makeFlatDrawStateRecordFromState(
      state, shaderLayout,
      FlatDrawStateUniformInputs{.hashes = &uniformHashes}, viewport);
  auto debug = makeDrawDebugSnapshot(args, hot);
  return CanonicalDrawState{std::move(hot), std::move(shaderLayout),
                            std::move(debug)};
}

namespace {

std::span<const DrawParam>
drawParamStorageSpan(const DrawParamInlineStorage &storage) noexcept {
  if (storage.overflowMode) {
    return std::span<const DrawParam>(storage.overflow.data(),
                                      storage.overflow.size());
  }
  return std::span<const DrawParam>(storage.inlineData.data(),
                                    storage.inlineSize);
}

std::span<const u8>
drawPayloadStorageSpan(const DrawPayloadArenaStorage &storage) noexcept {
  if (storage.overflowMode) {
    return std::span<const u8>(storage.overflow.data(),
                               storage.overflow.size());
  }
  return std::span<const u8>(storage.inlineData.data(), storage.inlineSize);
}

std::size_t
drawParamStorageSize(const DrawParamInlineStorage &storage) noexcept {
  return storage.overflowMode ? storage.overflow.size() : storage.inlineSize;
}

std::size_t
drawPayloadStorageSize(const DrawPayloadArenaStorage &storage) noexcept {
  return storage.overflowMode ? storage.overflow.size() : storage.inlineSize;
}

void reserveDrawParams(DrawParamInlineStorage &storage, std::size_t count) {
  if (count <= kDrawRunInlineParamCapacity || storage.overflowMode) {
    if (storage.overflowMode)
      storage.overflow.reserve(count);
    return;
  }
  storage.overflow.reserve(count);
  storage.overflow.insert(storage.overflow.end(), storage.inlineData.begin(),
                          storage.inlineData.begin() +
                              static_cast<std::ptrdiff_t>(storage.inlineSize));
  storage.inlineSize = 0;
  storage.overflowMode = true;
}

void reserveDrawPayload(DrawPayloadArenaStorage &storage, std::size_t bytes) {
  if (bytes <= kDrawRunInlinePayloadCapacity || storage.overflowMode) {
    if (storage.overflowMode)
      storage.overflow.reserve(bytes);
    return;
  }
  storage.overflow.reserve(bytes);
  storage.overflow.insert(storage.overflow.end(), storage.inlineData.begin(),
                          storage.inlineData.begin() +
                              static_cast<std::ptrdiff_t>(storage.inlineSize));
  storage.inlineSize = 0;
  storage.overflowMode = true;
}

void appendDrawParam(DrawParamInlineStorage &storage, DrawParam param) {
  if (storage.overflowMode) {
    storage.overflow.push_back(std::move(param));
    return;
  }
  if (storage.inlineSize < kDrawRunInlineParamCapacity) {
    storage.inlineData[storage.inlineSize++] = std::move(param);
    return;
  }
  reserveDrawParams(storage, kDrawRunInlineParamCapacity + 1);
  storage.overflow.push_back(std::move(param));
}

bool appendDrawPayload(DrawPayloadArenaStorage &storage,
                       std::span<const u8> bytes, DrawPayloadRange &range) {
  range = {};
  if (bytes.empty()) {
    return true;
  }

  constexpr auto kMaxRange = std::numeric_limits<u32>::max();
  const auto currentSize = drawPayloadStorageSize(storage);
  const std::uint64_t requiredSize = static_cast<std::uint64_t>(currentSize) +
                                     static_cast<std::uint64_t>(bytes.size());
  if (requiredSize > kMaxRange) {
    return false;
  }

  range = DrawPayloadRange{
      .offset = static_cast<u32>(currentSize),
      .size = static_cast<u32>(bytes.size()),
  };

  if (storage.overflowMode) {
    storage.overflow.insert(storage.overflow.end(), bytes.begin(), bytes.end());
    return true;
  }
  if (currentSize + bytes.size() <= kDrawRunInlinePayloadCapacity) {
    std::copy(bytes.begin(), bytes.end(),
              storage.inlineData.begin() +
                  static_cast<std::ptrdiff_t>(storage.inlineSize));
    storage.inlineSize += bytes.size();
    return true;
  }
  reserveDrawPayload(storage, static_cast<std::size_t>(requiredSize));
  storage.overflow.insert(storage.overflow.end(), bytes.begin(), bytes.end());
  return true;
}

bool packDrawParamPayload(DrawParam &param,
                          DrawPayloadArenaStorage &payloadArena,
                          DrawParamPayloadView payload) {
  const auto vertexBytes = payload.userVertexData;
  const auto indexBytes = payload.userIndexData;
  const auto bindingOverrideBytes = payload.bindingOverrideData;
  const auto bindingSnapshotBytes = payload.bindingSnapshotData;
  constexpr auto kMaxRange = std::numeric_limits<u32>::max();
  const std::uint64_t requiredSize =
      static_cast<std::uint64_t>(drawPayloadStorageSize(payloadArena)) +
      static_cast<std::uint64_t>(vertexBytes.size()) +
      static_cast<std::uint64_t>(indexBytes.size()) +
      static_cast<std::uint64_t>(bindingOverrideBytes.size()) +
      static_cast<std::uint64_t>(bindingSnapshotBytes.size());
  if (requiredSize > kMaxRange) {
    return false;
  }

  return appendDrawPayload(payloadArena, vertexBytes, param.userVertexRange) &&
         appendDrawPayload(payloadArena, indexBytes, param.userIndexRange) &&
         appendDrawPayload(payloadArena, bindingOverrideBytes,
                           param.bindingOverrideRange) &&
         appendDrawPayload(payloadArena, bindingSnapshotBytes,
                           param.bindingSnapshotRange);
}

bool drawPayloadRangeValid(std::size_t payloadSize,
                           DrawPayloadRange range) noexcept {
  return static_cast<std::uint64_t>(range.offset) +
             static_cast<std::uint64_t>(range.size) <=
         static_cast<std::uint64_t>(payloadSize);
}

} // namespace

namespace fixture {

void drawRunClear(DrawRunDesc &run) {
  if (run.scratch_.draws.overflowMode) {
    run.scratch_.draws.overflow.clear();
  } else {
    run.scratch_.draws.inlineSize = 0;
  }
  if (run.scratch_.payload.overflowMode) {
    run.scratch_.payload.overflow.clear();
  } else {
    run.scratch_.payload.inlineSize = 0;
  }
}

void drawRunReserve(DrawRunDesc &run, std::size_t drawCount,
                    std::size_t payloadBytes) {
  reserveDrawParams(run.scratch_.draws, drawCount);
  reserveDrawPayload(run.scratch_.payload, payloadBytes);
}

bool drawRunAppend(DrawRunDesc &run, DrawParam param,
                   DrawParamPayloadView payload) {
  param.userVertexRange = {};
  param.userIndexRange = {};
  param.bindingOverrideRange = {};
  param.bindingSnapshotRange = {};
  if (!packDrawParamPayload(param, run.scratch_.payload, payload)) {
    return false;
  }
  appendDrawParam(run.scratch_.draws, std::move(param));
  return true;
}

DrawRunView drawRunView(const DrawRunDesc &run) noexcept {
  return DrawRunView{
      .draws = drawParamStorageSpan(run.scratch_.draws),
      .payloadArena = drawPayloadStorageSpan(run.scratch_.payload),
      .uniforms = run.uniforms_,
      .uniformHandleCandidate = run.uniformHandleCandidate_,
  };
}

void drawRunSetUniformPayload(DrawRunDesc &run,
                              const DrawUniformPayload &payload) noexcept {
  run.uniforms_ = &payload;
}

void drawRunSetUniformHandleCandidate(DrawRunDesc &run,
                                      DrawUniformHandle handle) noexcept {
  run.uniformHandleCandidate_ = handle;
}

const DrawUniformPayload &
drawRunUniformPayload(const DrawRunDesc &run) noexcept {
  DXMT_ASSERT(run.uniforms_ != nullptr &&
              "draw-run missing uniform payload view");
  return *run.uniforms_;
}

bool drawRunEmpty(const DrawRunDesc &run) noexcept {
  return drawParamStorageSize(run.scratch_.draws) == 0;
}

std::size_t drawRunDrawCount(const DrawRunDesc &run) noexcept {
  return drawParamStorageSize(run.scratch_.draws);
}

std::size_t drawRunPayloadSize(const DrawRunDesc &run) noexcept {
  return drawPayloadStorageSize(run.scratch_.payload);
}

std::span<const DrawParam> drawRunDraws(const DrawRunDesc &run) noexcept {
  return drawParamStorageSpan(run.scratch_.draws);
}

std::span<const u8> drawRunPayloadArena(const DrawRunDesc &run) noexcept {
  return drawPayloadStorageSpan(run.scratch_.payload);
}

std::span<const u8> drawRunPayloadBytes(const DrawRunDesc &run,
                                        DrawPayloadRange range) noexcept {
  return core::drawRunPayloadBytes(range, drawRunPayloadArena(run));
}

bool drawRunValidate(const DrawRunDesc &run) noexcept {
  const auto payloadSize = drawRunPayloadSize(run);
  for (const auto &param : drawRunDraws(run)) {
    if (!drawPayloadRangeValid(payloadSize, param.userVertexRange) ||
        !drawPayloadRangeValid(payloadSize, param.userIndexRange) ||
        !drawPayloadRangeValid(payloadSize, param.bindingOverrideRange) ||
        !drawPayloadRangeValid(payloadSize, param.bindingSnapshotRange)) {
      return false;
    }
  }
  return true;
}

} // namespace fixture

DrawParamPayloadView
drawPayloadAt(std::span<const DrawParamPayloadView> payloads,
              std::size_t index) {
  if (index < payloads.size()) {
    return payloads[index];
  }
  return {};
}

bool drawRunUsesBoundIndexBuffer(
    std::span<const DrawParam> draws,
    std::span<const DrawParamPayloadView> payloads) {
  for (std::size_t i = 0; i < draws.size(); ++i) {
    const auto &draw = draws[i];
    if (draw.indexed && drawPayloadAt(payloads, i).userIndexData.empty()) {
      return true;
    }
  }
  return false;
}

bool drawStateInvalidationIsBindingOnly(u32 reasonMask) noexcept {
  constexpr u32 kBindingOnlyMask =
      DrawStateInvalidationDrawPacket |
      DrawStateInvalidationStream |
      DrawStateInvalidationIndexBuffer;
  return reasonMask != DrawStateInvalidationUnknown &&
         (reasonMask & ~kBindingOnlyMask) == 0;
}

bool drawStateInvalidationAffectsUniformNonConstants(u32 reasonMask) noexcept {
  constexpr u32 kNonConstantMask =
      DrawStateInvalidationMutableState |
      DrawStateInvalidationRenderState |
      DrawStateInvalidationFfpState |
      DrawStateInvalidationClipPlane |
      DrawStateInvalidationStateBlock |
      DrawStateInvalidationReset |
      DrawStateInvalidationSwapChain;
  return reasonMask == DrawStateInvalidationUnknown ||
         (reasonMask & kNonConstantMask) != 0;
}

bool drawStateInvalidationMayAffectShaderConstants(u32 reasonMask) noexcept {
  constexpr u32 kShaderConstantMask =
      DrawStateInvalidationMutableState |
      DrawStateInvalidationStateBlock |
      DrawStateInvalidationReset;
  return reasonMask == DrawStateInvalidationUnknown ||
         (reasonMask & kShaderConstantMask) != 0;
}

bool drawStateInvalidationAffectsShaderLayout(u32 reasonMask) noexcept {
  constexpr u32 kShaderLayoutMask =
      DrawStateInvalidationMutableState |
      DrawStateInvalidationRenderState |
      DrawStateInvalidationTexture |
      DrawStateInvalidationFvfVdecl |
      DrawStateInvalidationShader |
      DrawStateInvalidationFfpState |
      DrawStateInvalidationClipPlane |
      DrawStateInvalidationStateBlock |
      DrawStateInvalidationReset |
      DrawStateInvalidationSwapChain |
      DrawStateInvalidationTextureStageState;
  return reasonMask == DrawStateInvalidationUnknown ||
         (reasonMask & kShaderLayoutMask) != 0;
}

HResult Device::submitDrawRunInternalFromState(
    DeviceState baseState, std::span<const DrawParam> draws,
    std::span<const DrawParamPayloadView> payloads) {
  if (draws.empty()) {
    return D3D_OK;
  }
  if (!drawRunUsesBoundIndexBuffer(draws, payloads)) {
    baseState.indexBuffer.reset();
  }
  auto shaderLayout = makeDrawShaderLayoutContextFromState(baseState);
  DrawUniformPayloadHashes uniformHashes{};
  auto uniforms =
      makeDrawUniformPayloadFromState(baseState, shaderLayout.clipPlaneMask,
                                      &uniformHashes,
                                      /*recordSnapshotPerf=*/false,
                                      &shaderLayout);
  const auto viewport = makeViewportScissorFromState(baseState);
  auto hot = makeFlatDrawStateRecordFromState(
      baseState, shaderLayout,
      FlatDrawStateUniformInputs{.hashes = &uniformHashes}, viewport);
  auto debug = makeDrawDebugSnapshot(
      DrawCallArgs{draws.front().primitiveType, draws.front().primitiveCount,
                   draws.front().startVertex, draws.front().baseVertexIndex,
                   draws.front().startIndex, draws.front().indexType},
      hot);
  CanonicalDrawState state{
      std::move(hot),
      std::move(shaderLayout),
      std::move(debug),
  };
  return submitDrawRunInternal(std::move(state), uniforms, draws, payloads);
}

void Device::invalidateDrawStateCache(u32 reasonMask) noexcept {
  if (reasonMask == DrawStateInvalidationUnknown) {
    drawStateInvalidationReasonMask_ = DrawStateInvalidationUnknown;
  } else {
    drawStateInvalidationReasonMask_ |= reasonMask;
  }
  bumpGeneration(drawStateGeneration_);
  invalidateDrawFlatStateSetCache(reasonMask);
  if (!drawStateInvalidationIsBindingOnly(reasonMask)) {
    bumpGeneration(drawStableStateGeneration_);
    if (drawStateInvalidationAffectsShaderLayout(reasonMask)) {
      bumpGeneration(drawShaderLayoutGeneration_);
    }
    if (drawStateInvalidationAffectsUniformNonConstants(reasonMask)) {
      invalidateDrawUniformNonConstantCache();
    }
    if (drawStateInvalidationMayAffectShaderConstants(reasonMask)) {
      invalidateDrawShaderConstantsCache();
    } else {
      invalidateDrawUniformCache();
    }
  }
}

void Device::invalidateDrawUniformCache() noexcept {
  bumpGeneration(drawUniformGeneration_);
}

namespace {

template <std::size_t FloatCount>
void invalidateShaderConstantHashIndex(
    detail::ShaderConstantHashIndex<FloatCount> &index) noexcept {
  index.valid = false;
  index.generation = 0;
  index.floatDirty = {};
  index.intDirty = {};
  index.boolDirty = {};
}

void markShaderConstantHashDirtyRange(
    detail::ShaderConstantHashDirtyRange &range,
    u32 start, u32 count, std::size_t limit) noexcept {
  if (count == 0 || start >= limit) {
    return;
  }
  const auto begin = static_cast<u16>(start);
  const auto end = static_cast<u16>(std::min<std::uint64_t>(
      static_cast<std::uint64_t>(start) + count, limit));
  if (!range.dirty) {
    range.begin = begin;
    range.end = end;
    range.dirty = true;
    return;
  }
  range.begin = std::min(range.begin, begin);
  range.end = std::max(range.end, end);
}

template <std::size_t FloatCount>
bool markShaderConstantHashIndexDirty(
    detail::ShaderConstantHashIndex<FloatCount> &index,
    detail::ShaderConstantRegisterClass registerClass,
    u32 start, u32 count) noexcept {
  std::size_t limit = 0;
  detail::ShaderConstantHashDirtyRange *range = nullptr;
  switch (registerClass) {
    case detail::ShaderConstantRegisterClass::Float:
      limit = FloatCount;
      range = &index.floatDirty;
      break;
    case detail::ShaderConstantRegisterClass::Int:
      limit = kMaxIntegerConstants;
      range = &index.intDirty;
      break;
    case detail::ShaderConstantRegisterClass::Bool:
      limit = kMaxBoolConstants;
      range = &index.boolDirty;
      break;
  }
  if (!range || count == 0 || start >= limit) {
    return false;
  }
  markShaderConstantHashDirtyRange(*range, start, count, limit);
  return true;
}

} // namespace

void Device::invalidateDrawShaderConstantsCache() noexcept {
  bumpGeneration(drawUniformGeneration_);
  bumpGeneration(drawVertexShaderConstantGeneration_);
  bumpGeneration(drawPixelShaderConstantGeneration_);
  invalidateShaderConstantHashIndex(drawVertexShaderConstantHashIndex_);
  invalidateShaderConstantHashIndex(drawPixelShaderConstantHashIndex_);
}

void Device::invalidateDrawVertexShaderConstantsCache() noexcept {
  bumpGeneration(drawUniformGeneration_);
  bumpGeneration(drawVertexShaderConstantGeneration_);
  invalidateShaderConstantHashIndex(drawVertexShaderConstantHashIndex_);
}

void Device::invalidateDrawPixelShaderConstantsCache() noexcept {
  bumpGeneration(drawUniformGeneration_);
  bumpGeneration(drawPixelShaderConstantGeneration_);
  invalidateShaderConstantHashIndex(drawPixelShaderConstantHashIndex_);
}

void Device::invalidateDrawVertexShaderConstantsCacheRange(
    detail::ShaderConstantRegisterClass registerClass,
    u32 start, u32 count) noexcept {
  if (!markShaderConstantHashIndexDirty(
          drawVertexShaderConstantHashIndex_, registerClass, start, count)) {
    return;
  }
  bumpGeneration(drawUniformGeneration_);
  bumpGeneration(drawVertexShaderConstantGeneration_);
}

void Device::invalidateDrawPixelShaderConstantsCacheRange(
    detail::ShaderConstantRegisterClass registerClass,
    u32 start, u32 count) noexcept {
  if (!markShaderConstantHashIndexDirty(
          drawPixelShaderConstantHashIndex_, registerClass, start, count)) {
    return;
  }
  bumpGeneration(drawUniformGeneration_);
  bumpGeneration(drawPixelShaderConstantGeneration_);
}

DeviceState &Device::mutableVertexShaderFloatConstantsState(
    u32 start, u32 count) noexcept {
  invalidateDrawVertexShaderConstantsCacheRange(
      detail::ShaderConstantRegisterClass::Float, start, count);
  return state_;
}

DeviceState &Device::mutableVertexShaderIntConstantsState(
    u32 start, u32 count) noexcept {
  invalidateDrawVertexShaderConstantsCacheRange(
      detail::ShaderConstantRegisterClass::Int, start, count);
  return state_;
}

DeviceState &Device::mutableVertexShaderBoolConstantsState(
    u32 start, u32 count) noexcept {
  invalidateDrawVertexShaderConstantsCacheRange(
      detail::ShaderConstantRegisterClass::Bool, start, count);
  return state_;
}

DeviceState &Device::mutablePixelShaderFloatConstantsState(
    u32 start, u32 count) noexcept {
  invalidateDrawPixelShaderConstantsCacheRange(
      detail::ShaderConstantRegisterClass::Float, start, count);
  return state_;
}

DeviceState &Device::mutablePixelShaderIntConstantsState(
    u32 start, u32 count) noexcept {
  invalidateDrawPixelShaderConstantsCacheRange(
      detail::ShaderConstantRegisterClass::Int, start, count);
  return state_;
}

DeviceState &Device::mutablePixelShaderBoolConstantsState(
    u32 start, u32 count) noexcept {
  invalidateDrawPixelShaderConstantsCacheRange(
      detail::ShaderConstantRegisterClass::Bool, start, count);
  return state_;
}

void Device::invalidateDrawUniformNonConstantCache() noexcept {
  bumpGeneration(drawUniformNonConstantGeneration_);
}

void Device::invalidateDrawFlatStateSetCache(u32 reasonMask) noexcept {
  const bool broad =
      reasonMask == DrawStateInvalidationUnknown ||
      (reasonMask & (DrawStateInvalidationMutableState |
                     DrawStateInvalidationStateBlock |
                     DrawStateInvalidationReset)) != 0;
  const bool hasSpecificStageSampler =
      (reasonMask & (DrawStateInvalidationTextureStageState |
                     DrawStateInvalidationSamplerState)) != 0;
  const bool aggregateStageSamplerOnly =
      !hasSpecificStageSampler &&
      (reasonMask & DrawStateInvalidationTextureStageSampler) != 0;

  if (broad || (reasonMask & DrawStateInvalidationRenderState) != 0) {
    bumpGeneration(drawRenderStateFlatGeneration_);
  }
  if (broad ||
      aggregateStageSamplerOnly ||
      (reasonMask & DrawStateInvalidationTextureStageState) != 0) {
    bumpGeneration(drawTextureStageStateFlatGeneration_);
  }
  if (broad ||
      aggregateStageSamplerOnly ||
      (reasonMask & DrawStateInvalidationSamplerState) != 0) {
    bumpGeneration(drawSamplerStateFlatGeneration_);
  }
}

const Device::CachedBaseDrawState &
Device::cachedBaseDrawState(bool includeIndexBuffer) {
  auto &cache =
      includeIndexBuffer ? drawStateCacheWithIndex_ : drawStateCacheNoIndex_;
  const auto vertexConstantGeneration =
      drawVertexShaderConstantGeneration_;
  const auto pixelConstantGeneration =
      drawPixelShaderConstantGeneration_;
  auto *vertexConstantHashIndex = &drawVertexShaderConstantHashIndex_;
  auto *pixelConstantHashIndex = &drawPixelShaderConstantHashIndex_;
  const auto refreshUniforms = [&]() {
    const bool reuseVertexConstants =
        cache.vertexShaderConstantGeneration ==
        drawVertexShaderConstantGeneration_;
    const bool reusePixelConstants =
        cache.pixelShaderConstantGeneration ==
        drawPixelShaderConstantGeneration_;
    {
      PerfScope uniformBuildScope(
          dxmt9::perf::countD3D9SnapshotCacheUniformBuildCpuTime);
      refreshDrawUniformPayloadShaderConstantsFromState(
          state_, cache.uniforms, cache.uniformHashes, cache.shaderLayout,
          /*recordSnapshotPerf=*/true, reuseVertexConstants,
          reusePixelConstants, vertexConstantGeneration,
          pixelConstantGeneration, vertexConstantHashIndex,
          pixelConstantHashIndex);
    }
    {
      PerfScope uniformHashScope(
          dxmt9::perf::countD3D9SnapshotCacheUniformHashCpuTime);
      applyDrawUniformPayloadHashes(cache.hot, cache.uniformHashes);
    }
    cache.uniformFixedPayload = makeDrawUniformFixedPayload(cache.uniforms);
    cache.uniformPayloadHash = cache.uniforms.hash;
    cache.uniformFixedPayloadHash = cache.uniforms.fixedPayloadHash;
    cache.fullUniformsValid = true;
    cache.uniformFixedPayloadValid = true;
    cache.uniformGeneration = drawUniformGeneration_;
    cache.vertexShaderConstantGeneration = drawVertexShaderConstantGeneration_;
    cache.pixelShaderConstantGeneration = drawPixelShaderConstantGeneration_;
  };
  if (cache.valid && cache.generation == drawStateGeneration_) {
    PerfScope hitScope(dxmt9::perf::countD3D9SnapshotCacheHitCpuTime);
    PerfScope directHitScope(
        dxmt9::perf::countD3D9SnapshotCacheDirectHitCpuTime);
    dxmt9::perf::countD3D9DrawStateCacheLookup(/*hit=*/true, includeIndexBuffer);
    dxmt9::perf::countD3D9DrawStateCacheDirectLookup(/*hit=*/true,
                                                     includeIndexBuffer);
    if (cache.uniformGeneration != drawUniformGeneration_) {
      dxmt9::perf::countD3D9DrawStateCacheUniformRefresh();
      PerfScope uniformRefreshScope(
          dxmt9::perf::countD3D9SnapshotCacheUniformRefreshCpuTime);
      refreshUniforms();
    }
    return cache;
  }
  {
    PerfScope missScope(dxmt9::perf::countD3D9SnapshotCacheMissCpuTime);
    PerfScope directMissScope(
        dxmt9::perf::countD3D9SnapshotCacheDirectMissCpuTime);
    dxmt9::perf::countD3D9DrawStateCacheLookup(/*hit=*/false, includeIndexBuffer);
    dxmt9::perf::countD3D9DrawStateCacheDirectLookup(/*hit=*/false,
                                                     includeIndexBuffer);
    dxmt9::perf::countD3D9DrawStateCacheMissReason(
        drawStateInvalidationReasonMask_);

    DeviceState baseState = state_;
    if (!includeIndexBuffer) {
      baseState.indexBuffer.reset();
    }
    const bool hadCachedState = cache.valid;
    const auto previousVertexConstantGeneration =
        cache.vertexShaderConstantGeneration;
    const auto previousPixelConstantGeneration =
        cache.pixelShaderConstantGeneration;
    const auto previousVertexConstantUsage =
        cache.shaderLayout.vertexConstantUsage;
    const auto previousPixelConstantUsage =
        cache.shaderLayout.pixelConstantUsage;
    {
      PerfScope shaderLayoutScope(
          dxmt9::perf::countD3D9SnapshotCacheMissShaderLayoutCpuTime);
      PerfScope directShaderLayoutScope(
          dxmt9::perf::countD3D9SnapshotCacheDirectMissShaderLayoutCpuTime);
      cache.shaderLayout = makeDrawShaderLayoutContextFromState(baseState);
    }
    const bool reuseVertexConstantsHash =
        hadCachedState &&
        previousVertexConstantGeneration == drawVertexShaderConstantGeneration_ &&
        previousVertexConstantUsage == cache.shaderLayout.vertexConstantUsage;
    const bool reusePixelConstantsHash =
        hadCachedState &&
        previousPixelConstantGeneration == drawPixelShaderConstantGeneration_ &&
        previousPixelConstantUsage == cache.shaderLayout.pixelConstantUsage;
    const DrawUniformPayloadHashes *reusableShaderConstantHashes =
        (reuseVertexConstantsHash || reusePixelConstantsHash)
            ? &cache.uniformHashes
            : nullptr;
    DrawUniformPayloadHashes uniformHashes{};
    {
      PerfScope uniformBuildScope(
          dxmt9::perf::countD3D9SnapshotCacheMissUniformBuildCpuTime);
      PerfScope directUniformBuildScope(
          dxmt9::perf::countD3D9SnapshotCacheDirectMissUniformBuildCpuTime);
      cache.uniforms = makeDrawUniformPayloadFromState(
          baseState, cache.shaderLayout.clipPlaneMask, &uniformHashes,
          /*recordSnapshotPerf=*/true, &cache.shaderLayout,
          /*reusableNonConstantHashes=*/nullptr, reusableShaderConstantHashes,
          reuseVertexConstantsHash, reusePixelConstantsHash,
          vertexConstantGeneration, pixelConstantGeneration,
          vertexConstantHashIndex, pixelConstantHashIndex);
      cache.uniformHashes = uniformHashes;
      cache.uniformFixedPayload = makeDrawUniformFixedPayload(cache.uniforms);
      cache.uniformPayloadHash = cache.uniforms.hash;
      cache.uniformFixedPayloadHash = cache.uniforms.fixedPayloadHash;
      cache.fullUniformsValid = true;
      cache.uniformFixedPayloadValid = true;
    }
    {
      PerfScope hotBuildScope(
          dxmt9::perf::countD3D9SnapshotCacheMissHotBuildCpuTime);
      PerfScope directHotBuildScope(
          dxmt9::perf::countD3D9SnapshotCacheDirectMissHotBuildCpuTime);
      const auto viewport = makeViewportScissorFromState(baseState);
      cache.hot = makeFlatDrawStateRecordFromState(
          baseState, cache.shaderLayout,
          FlatDrawStateUniformInputs{.hashes = &uniformHashes}, viewport);
    }
    cache.generation = drawStateGeneration_;
    cache.shaderLayoutGeneration = drawShaderLayoutGeneration_;
    cache.uniformGeneration = drawUniformGeneration_;
    cache.vertexShaderConstantGeneration = drawVertexShaderConstantGeneration_;
    cache.pixelShaderConstantGeneration = drawPixelShaderConstantGeneration_;
    cache.uniformNonConstantGeneration = drawUniformNonConstantGeneration_;
    cache.renderStateFlatGeneration = drawRenderStateFlatGeneration_;
    cache.textureStageStateFlatGeneration = drawTextureStageStateFlatGeneration_;
    cache.samplerStateFlatGeneration = drawSamplerStateFlatGeneration_;
    cache.valid = true;
    drawStateInvalidationReasonMask_ = DrawStateInvalidationUnknown;
  }
  return cache;
}

const Device::CachedBaseDrawState &
Device::cachedBaseDrawStateForSubmissionBatch() {
  auto &cache = drawStateCacheBindingAgnostic_;
  const auto vertexConstantGeneration =
      drawVertexShaderConstantGeneration_;
  const auto pixelConstantGeneration =
      drawPixelShaderConstantGeneration_;
  auto *vertexConstantHashIndex = &drawVertexShaderConstantHashIndex_;
  auto *pixelConstantHashIndex = &drawPixelShaderConstantHashIndex_;
  const auto refreshBindingLayout = [&]() {
    refreshShaderLayoutExtraStreamStrides(cache.shaderLayout, state_);
  };
  const auto refreshUniforms = [&]() {
    const bool reuseVertexConstants =
        cache.vertexShaderConstantGeneration ==
        drawVertexShaderConstantGeneration_;
    const bool reusePixelConstants =
        cache.pixelShaderConstantGeneration ==
        drawPixelShaderConstantGeneration_;
    {
      if (!cache.fullUniformsValid) {
        DrawUniformPayloadHashes uniformHashes{};
        {
          PerfScope uniformBuildScope(
              dxmt9::perf::countD3D9SnapshotCacheUniformBuildCpuTime);
          cache.uniforms = makeDrawUniformPayloadFromState(
              state_, cache.shaderLayout.clipPlaneMask, &uniformHashes,
              /*recordSnapshotPerf=*/true, &cache.shaderLayout,
              cache.uniformFixedPayloadValid ? &cache.uniformHashes : nullptr,
              &cache.uniformHashes, reuseVertexConstants,
              reusePixelConstants, vertexConstantGeneration,
              pixelConstantGeneration, vertexConstantHashIndex,
              pixelConstantHashIndex);
        }
        cache.uniformHashes = uniformHashes;
      } else {
        {
          PerfScope uniformBuildScope(
              dxmt9::perf::countD3D9SnapshotCacheUniformBuildCpuTime);
          refreshDrawUniformPayloadShaderConstantsFromState(
              state_, cache.uniforms, cache.uniformHashes, cache.shaderLayout,
              /*recordSnapshotPerf=*/true, reuseVertexConstants,
              reusePixelConstants, vertexConstantGeneration,
              pixelConstantGeneration, vertexConstantHashIndex,
              pixelConstantHashIndex);
        }
      }
      cache.uniformFixedPayload = makeDrawUniformFixedPayload(cache.uniforms);
      cache.uniformPayloadHash = cache.uniforms.hash;
      cache.uniformFixedPayloadHash = cache.uniforms.fixedPayloadHash;
      cache.fullUniformsValid = true;
      cache.uniformFixedPayloadValid = true;
    }
    {
      PerfScope uniformHashScope(
          dxmt9::perf::countD3D9SnapshotCacheUniformHashCpuTime);
      applyDrawUniformPayloadHashes(cache.hot, cache.uniformHashes);
      clearDrawStateBindingFields(cache.hot);
    }
    cache.uniformGeneration = drawUniformGeneration_;
    cache.vertexShaderConstantGeneration = drawVertexShaderConstantGeneration_;
    cache.pixelShaderConstantGeneration = drawPixelShaderConstantGeneration_;
  };
  if (cache.valid && cache.generation == drawStableStateGeneration_) {
    PerfScope hitScope(dxmt9::perf::countD3D9SnapshotCacheHitCpuTime);
    PerfScope batchHitScope(
        dxmt9::perf::countD3D9SnapshotCacheBatchHitCpuTime);
    dxmt9::perf::countD3D9DrawStateCacheLookup(/*hit=*/true,
                                                /*includeIndexBuffer=*/false);
    dxmt9::perf::countD3D9DrawStateCacheBatchLookup(/*hit=*/true);
    // Debug cross-check for the same-generation trust chain: a hit must mean
    // the cached snapshot still matches the live DeviceState. A stale hit
    // here would silently ride the {stateGeneration, lane} stamp into
    // draw-run batching and re-draw with another draw's shaders/declaration
    // (the GT1 t=40s giant-triangle class), which submission-level asserts
    // cannot see because every stale submission is self-consistent. This
    // used to be a budgeted fprintf forensic probe; now that
    // shaderLayoutGeneration guards reuse (see above), a hit is expected to
    // always be fresh, so any staleness here is a hard invariant violation.
    //
    // The cached shaderLayout stores the EFFECTIVE shader (a null live
    // shader normalizes to an FFP ref at layout-build time), so compare
    // normalization-aware: a live Bytecode shader must match the cached
    // bytecode; a live null shader is stale only if the cache still holds a
    // Bytecode ref.
#ifndef NDEBUG
    {
      const auto staleShader = [](const ShaderRef& live, const ShaderRef& cached) {
        if (live.kind == ShaderRef::Kind::Bytecode) {
          return cached.kind != ShaderRef::Kind::Bytecode ||
                 !(cached.bytecode == live.bytecode);
        }
        return cached.kind == ShaderRef::Kind::Bytecode;
      };
      DXMT_ASSERT(!staleShader(state_.vertexShader, cache.shaderLayout.vertexShader));
      DXMT_ASSERT(!staleShader(state_.pixelShader, cache.shaderLayout.pixelShader));
      DXMT_ASSERT(cache.shaderLayout.vertexDecl.elements == state_.vertexDecl.elements);
    }
#endif
    if (drawStateInvalidationIsBindingOnly(drawStateInvalidationReasonMask_)) {
      drawStateInvalidationReasonMask_ = DrawStateInvalidationUnknown;
    }
    {
      PerfScope bindingLayoutScope(
          dxmt9::perf::countD3D9SnapshotCacheBindingLayoutCpuTime);
      refreshBindingLayout();
    }
    if (cache.uniformGeneration != drawUniformGeneration_ ||
        !cache.fullUniformsValid) {
      dxmt9::perf::countD3D9DrawStateCacheUniformRefresh();
      PerfScope uniformRefreshScope(
          dxmt9::perf::countD3D9SnapshotCacheUniformRefreshCpuTime);
      refreshUniforms();
    }
    return cache;
  }
  {
    PerfScope missScope(dxmt9::perf::countD3D9SnapshotCacheMissCpuTime);
    PerfScope batchMissScope(
        dxmt9::perf::countD3D9SnapshotCacheBatchMissCpuTime);
    dxmt9::perf::countD3D9DrawStateCacheLookup(/*hit=*/false,
                                                /*includeIndexBuffer=*/false);
    dxmt9::perf::countD3D9DrawStateCacheBatchLookup(/*hit=*/false);
    const u32 invalidationReasonMask = drawStateInvalidationReasonMask_;
    dxmt9::perf::countD3D9DrawStateCacheMissReason(invalidationReasonMask);
    dxmt9::perf::countD3D9DrawStateCacheBatchMissReason(
        invalidationReasonMask);
    const bool hadCachedState = cache.valid;
    const auto previousVertexConstantGeneration =
        cache.vertexShaderConstantGeneration;
    const auto previousPixelConstantGeneration =
        cache.pixelShaderConstantGeneration;
    const auto previousVertexConstantUsage =
        cache.shaderLayout.vertexConstantUsage;
    const auto previousPixelConstantUsage =
        cache.shaderLayout.pixelConstantUsage;
    const auto previousClipPlaneMask = cache.shaderLayout.clipPlaneMask;
    const bool previousProgrammableVertexShader =
        cache.shaderLayout.vertexShader.kind == ShaderRef::Kind::Bytecode;

    // Key layout reuse off the dedicated layout generation, NOT the shared
    // reason-mask accumulator: the mask is consumed/cleared by whichever
    // cache lane rebuilds first, so it can lose another lane's
    // Shader/FvfVdecl bits and let this lane reuse a stale shader layout
    // under a fresh stable generation (GT1 t=40s giant-triangle bug: rigid
    // props re-drawn with the soldiers' skinning VS + UBYTE4 declaration).
    const bool reuseShaderLayout =
        cache.valid &&
        cache.shaderLayoutGeneration == drawShaderLayoutGeneration_;
    dxmt9::perf::countD3D9SnapshotCacheBatchMissShaderLayoutReuse(
        reuseShaderLayout);
    const bool recordShaderLayoutCompatibility =
        dxmt9::perf::enabled() && cache.valid && !reuseShaderLayout;
    DrawShaderLayoutContext previousShaderLayout{};
    if (recordShaderLayoutCompatibility) {
      previousShaderLayout = cache.shaderLayout;
    }

    if (!reuseShaderLayout) {
      PerfScope shaderLayoutScope(
          dxmt9::perf::countD3D9SnapshotCacheMissShaderLayoutCpuTime);
      PerfScope batchShaderLayoutScope(
          dxmt9::perf::countD3D9SnapshotCacheBatchMissShaderLayoutCpuTime);
      cache.shaderLayout = makeDrawShaderLayoutContextFromState(state_);
    }
    const bool sameClipPlaneCoordinateSpace =
        !hadCachedState ||
        (previousClipPlaneMask | cache.shaderLayout.clipPlaneMask) == 0 ||
        previousProgrammableVertexShader ==
            (cache.shaderLayout.vertexShader.kind == ShaderRef::Kind::Bytecode);
    {
      PerfScope bindingLayoutScope(
          dxmt9::perf::countD3D9SnapshotCacheBindingLayoutCpuTime);
      refreshBindingLayout();
    }
    dxmt9::perf::countD3D9SnapshotCacheBatchMissShaderLayoutCompatible(
        reuseShaderLayout ||
        (recordShaderLayoutCompatibility &&
         shaderLayoutsCompatibleForDrawRunBatch(previousShaderLayout,
                                                cache.shaderLayout)));
    DrawUniformPayloadHashes uniformHashes{};
    const DrawUniformPayloadHashes *reusableNonConstantHashes =
        cache.valid &&
            sameClipPlaneCoordinateSpace &&
            cache.uniformNonConstantGeneration ==
                drawUniformNonConstantGeneration_
            ? &cache.uniformHashes
            : nullptr;
    dxmt9::perf::countD3D9SnapshotCacheBatchMissUniformNonConstHashReuse(
        reusableNonConstantHashes != nullptr);
    const bool reuseVertexConstantsHash =
        hadCachedState &&
        previousVertexConstantGeneration == drawVertexShaderConstantGeneration_ &&
        previousVertexConstantUsage == cache.shaderLayout.vertexConstantUsage;
    const bool reusePixelConstantsHash =
        hadCachedState &&
        previousPixelConstantGeneration == drawPixelShaderConstantGeneration_ &&
        previousPixelConstantUsage == cache.shaderLayout.pixelConstantUsage;
    const DrawUniformPayloadHashes *reusableShaderConstantHashes =
        (reuseVertexConstantsHash || reusePixelConstantsHash)
            ? &cache.uniformHashes
            : nullptr;
    const bool matchingUniformPayload =
        hadCachedState &&
        sameClipPlaneCoordinateSpace &&
        cache.uniformNonConstantGeneration == drawUniformNonConstantGeneration_ &&
        previousVertexConstantGeneration == drawVertexShaderConstantGeneration_ &&
        previousPixelConstantGeneration == drawPixelShaderConstantGeneration_ &&
        previousVertexConstantUsage == cache.shaderLayout.vertexConstantUsage &&
        previousPixelConstantUsage == cache.shaderLayout.pixelConstantUsage &&
        previousClipPlaneMask == cache.shaderLayout.clipPlaneMask;
    const bool reuseUniformPayload =
        matchingUniformPayload && cache.fullUniformsValid;
    const bool canReuseNonConstantPayload =
        reusableNonConstantHashes != nullptr && cache.fullUniformsValid;
    const bool reuseNonConstantPayload =
        !reuseUniformPayload && canReuseNonConstantPayload;
    dxmt9::perf::countD3D9SnapshotCacheBatchMissUniformPayloadPath(
        reuseUniformPayload, reuseNonConstantPayload);
    dxmt9::perf::countD3D9SnapshotCacheBatchMissUniformVsConstHashPath(
        reuseUniformPayload || reuseVertexConstantsHash);
    dxmt9::perf::countD3D9SnapshotCacheBatchMissUniformPsConstHashPath(
        reuseUniformPayload || reusePixelConstantsHash);
    const bool shaderHashMemoProbe =
        batchMissShaderHashMemoProbeEnabled();
    const auto probeShaderHashMemo =
        [](const auto& memo, u64 generation,
           const ShaderConstantUsageBounds& usage) {
          for (const auto& entry : memo) {
            if (entry.valid && entry.generation == generation &&
                entry.usage == usage) {
              return true;
            }
          }
          return false;
        };
    const auto storeShaderHashMemo =
        [](auto& memo, u32& cursor, u64 generation,
           const ShaderConstantUsageBounds& usage, u64 hash) {
          const auto memoSize = memo.size();
          auto& slot = memo[cursor % memoSize];
          slot.usage = usage;
          slot.generation = generation;
          slot.hash = hash;
          slot.valid = true;
          cursor = static_cast<u32>((cursor + 1u) % memoSize);
        };
    if (shaderHashMemoProbe && !reuseUniformPayload &&
        !reuseVertexConstantsHash) {
      dxmt9::perf::countD3D9SnapshotCacheBatchMissUniformVsConstHashMemoProbe(
          probeShaderHashMemo(drawStateCacheBatchMissVsConstHashMemo_,
                              drawVertexShaderConstantGeneration_,
                              cache.shaderLayout.vertexConstantUsage));
    }
    if (shaderHashMemoProbe && !reuseUniformPayload &&
        !reusePixelConstantsHash) {
      dxmt9::perf::countD3D9SnapshotCacheBatchMissUniformPsConstHashMemoProbe(
          probeShaderHashMemo(drawStateCacheBatchMissPsConstHashMemo_,
                              drawPixelShaderConstantGeneration_,
                              cache.shaderLayout.pixelConstantUsage));
    }
    {
      if (reuseUniformPayload) {
        uniformHashes = cache.uniformHashes;
      } else if (reuseNonConstantPayload) {
        PerfScope uniformBuildScope(
            dxmt9::perf::countD3D9SnapshotCacheMissUniformBuildCpuTime);
        PerfScope batchUniformBuildScope(
            dxmt9::perf::countD3D9SnapshotCacheBatchMissUniformBuildCpuTime);
        dxmt9::perf::ScopedD3D9SnapshotUniformBuildContext
            uniformBuildContext(
                dxmt9::perf::D3D9SnapshotUniformBuildContext::BatchMiss);
        uniformHashes = cache.uniformHashes;
        refreshDrawUniformPayloadShaderConstantsFromState(
            state_, cache.uniforms, uniformHashes, cache.shaderLayout,
            /*recordSnapshotPerf=*/true, reuseVertexConstantsHash,
            reusePixelConstantsHash, vertexConstantGeneration,
            pixelConstantGeneration, vertexConstantHashIndex,
            pixelConstantHashIndex);
        cache.uniformHashes = uniformHashes;
      } else {
        PerfScope uniformBuildScope(
            dxmt9::perf::countD3D9SnapshotCacheMissUniformBuildCpuTime);
        PerfScope batchUniformBuildScope(
            dxmt9::perf::countD3D9SnapshotCacheBatchMissUniformBuildCpuTime);
        dxmt9::perf::ScopedD3D9SnapshotUniformBuildContext
            uniformBuildContext(
                dxmt9::perf::D3D9SnapshotUniformBuildContext::BatchMiss);
        cache.uniforms = makeDrawUniformPayloadFromState(
            state_, cache.shaderLayout.clipPlaneMask, &uniformHashes,
            /*recordSnapshotPerf=*/true, &cache.shaderLayout,
            reusableNonConstantHashes, reusableShaderConstantHashes,
            reuseVertexConstantsHash, reusePixelConstantsHash,
            vertexConstantGeneration, pixelConstantGeneration,
            vertexConstantHashIndex, pixelConstantHashIndex);
        cache.uniformHashes = uniformHashes;
      }
      cache.uniformFixedPayload = makeDrawUniformFixedPayload(cache.uniforms);
      cache.uniformPayloadHash = cache.uniforms.hash;
      cache.uniformFixedPayloadHash = cache.uniforms.fixedPayloadHash;
      cache.fullUniformsValid = true;
      cache.uniformFixedPayloadValid = true;
    }
    if (shaderHashMemoProbe) {
      storeShaderHashMemo(drawStateCacheBatchMissVsConstHashMemo_,
                          drawStateCacheBatchMissVsConstHashMemoCursor_,
                          drawVertexShaderConstantGeneration_,
                          cache.shaderLayout.vertexConstantUsage,
                          cache.uniformHashes.vertexConstantsHash);
      dxmt9::perf::countD3D9SnapshotCacheBatchMissUniformVsConstHashMemoStore();
      storeShaderHashMemo(drawStateCacheBatchMissPsConstHashMemo_,
                          drawStateCacheBatchMissPsConstHashMemoCursor_,
                          drawPixelShaderConstantGeneration_,
                          cache.shaderLayout.pixelConstantUsage,
                          cache.uniformHashes.pixelConstantsHash);
      dxmt9::perf::countD3D9SnapshotCacheBatchMissUniformPsConstHashMemoStore();
    }
    {
      PerfScope hotBuildScope(
          dxmt9::perf::countD3D9SnapshotCacheMissHotBuildCpuTime);
      PerfScope batchHotBuildScope(
          dxmt9::perf::countD3D9SnapshotCacheBatchMissHotBuildCpuTime);
      const auto viewport = makeViewportScissorFromState(state_);
      const bool reuseRenderStates =
          cache.valid &&
          cache.renderStateFlatGeneration == drawRenderStateFlatGeneration_;
      const bool reuseTextureStageStates =
          cache.valid &&
          cache.textureStageStateFlatGeneration ==
              drawTextureStageStateFlatGeneration_;
      const bool reuseSamplerStates =
          cache.valid &&
          cache.samplerStateFlatGeneration == drawSamplerStateFlatGeneration_;
      dxmt9::perf::countD3D9SnapshotCacheBatchMissHotBuildFlatRenderReuse(
          reuseRenderStates);
      dxmt9::perf::countD3D9SnapshotCacheBatchMissHotBuildFlatTssReuse(
          reuseTextureStageStates);
      dxmt9::perf::countD3D9SnapshotCacheBatchMissHotBuildFlatSamplerReuse(
          reuseSamplerStates);
      const FlatDrawStateRecordBuildPerfOptions hotBuildPerf{
          .recordBatchMissHotBuild = true,
          .preserveReusableFlatStateSets = true,
          .reusableRenderStates =
              reuseRenderStates ? &cache.hot.renderStates : nullptr,
          .reusableTextureStageStates =
              reuseTextureStageStates ? &cache.hot.textureStageStates : nullptr,
          .reusableSamplerStates =
              reuseSamplerStates ? &cache.hot.samplerStates : nullptr,
      };
      refreshFlatDrawStateRecordFromState(
          cache.hot, state_, cache.shaderLayout,
          FlatDrawStateUniformInputs{.hashes = &uniformHashes}, viewport,
          hotBuildPerf);
      clearDrawStateBindingFields(cache.hot);
    }
    if (batchMissSemanticReuseProbeEnabled()) {
      constexpr std::size_t kProbeSize =
          std::tuple_size_v<decltype(drawStateCacheBatchMissSemanticProbe_)>;
      const auto keyHash = hashTrivial(cache.hot.key);
      bool hit = false;
      std::uint32_t distance = 0;
      for (std::size_t offset = 0; offset < kProbeSize; ++offset) {
        const auto index =
            (drawStateCacheBatchMissSemanticProbeCursor_ + kProbeSize - 1u -
             offset) %
            kProbeSize;
        const auto &entry = drawStateCacheBatchMissSemanticProbe_[index];
        if (entry.valid && entry.hash == keyHash &&
            entry.key == cache.hot.key) {
          hit = true;
          distance = static_cast<std::uint32_t>(offset + 1u);
          break;
        }
      }
      dxmt9::perf::countD3D9SnapshotCacheBatchMissSemanticReuseProbe(
          hit, distance);

      auto &slot =
          drawStateCacheBatchMissSemanticProbe_
              [drawStateCacheBatchMissSemanticProbeCursor_ % kProbeSize];
      slot.key = cache.hot.key;
      slot.hash = keyHash;
      slot.valid = true;
      drawStateCacheBatchMissSemanticProbeCursor_ =
          static_cast<u32>((drawStateCacheBatchMissSemanticProbeCursor_ + 1u) %
                           kProbeSize);
    }
    cache.generation = drawStableStateGeneration_;
    cache.shaderLayoutGeneration = drawShaderLayoutGeneration_;
    cache.uniformGeneration = drawUniformGeneration_;
    cache.vertexShaderConstantGeneration = drawVertexShaderConstantGeneration_;
    cache.pixelShaderConstantGeneration = drawPixelShaderConstantGeneration_;
    cache.uniformNonConstantGeneration = drawUniformNonConstantGeneration_;
    cache.renderStateFlatGeneration = drawRenderStateFlatGeneration_;
    cache.textureStageStateFlatGeneration = drawTextureStageStateFlatGeneration_;
    cache.samplerStateFlatGeneration = drawSamplerStateFlatGeneration_;
    cache.valid = true;
    drawStateInvalidationReasonMask_ = DrawStateInvalidationUnknown;
  }
  return cache;
}

HResult Device::submitDrawRunInternalFromCurrentState(
    std::span<const DrawParam> draws,
    std::span<const DrawParamPayloadView> payloads) {
  if (draws.empty()) {
    return D3D_OK;
  }
  const auto &cached =
      cachedBaseDrawState(drawRunUsesBoundIndexBuffer(draws, payloads));
  CanonicalDrawState state{
      cached.hot,
      cached.shaderLayout,
      makeDrawDebugSnapshot(
          DrawCallArgs{draws.front().primitiveType,
                       draws.front().primitiveCount, draws.front().startVertex,
                       draws.front().baseVertexIndex, draws.front().startIndex,
                       draws.front().indexType},
          cached.hot),
  };
  return submitDrawRunInternal(std::move(state), cached.uniforms, draws,
                               payloads);
}


DrawBindingOverride Device::compatibilityDrawBindingOverride(
    const DrawParam& draw, const CachedBaseDrawState& cached) const noexcept {
  DrawBindingOverride bindingOverride{};
  for (u32 stream = 0; stream < kMaxStreams; ++stream) {
    if (!state_.streamBuffers[stream]) {
      continue;
    }
    bindingOverride.streamMask |= 1u << stream;
    bindingOverride.streams[stream] = DrawStreamBindingOverride{
        .buffer = state_.streamBuffers[stream]->handle(),
        .offset = state_.streamOffsets[stream],
        .stride = state_.streamStrides[stream],
    };
  }
  if (draw.indexed) {
    bindingOverride.indexBuffer =
        state_.indexBuffer ? state_.indexBuffer->handle() : Handle{};
    bindingOverride.indexType = draw.indexType;
    bindingOverride.indexBufferValid = true;
  }
  bindingOverride.alphaTestEnable =
      flatStateOr(cached.hot.renderStates, RS_ALPHA_TEST_ENABLE, 0u);
  bindingOverride.alphaTestFunc = flatStateOr(
      cached.hot.renderStates, RS_ALPHA_FUNC,
      static_cast<u32>(CompareFunc::Always));
  bindingOverride.alphaTestRef =
      flatStateOr(cached.hot.renderStates, RS_ALPHA_REF, 0u);
  bindingOverride.alphaTestStateValid = true;
  return bindingOverride;
}

std::array<u64, kCompatibilityDrawBatchGenerationCount>
Device::compatibilityDrawBatchGenerations() const noexcept {
  using Gen = CompatibilityDrawBatchGeneration;
  const auto slot = [](Gen value) noexcept {
    return static_cast<std::size_t>(value);
  };
  std::array<u64, kCompatibilityDrawBatchGenerationCount> generations{};
  generations[slot(Gen::StableState)] = drawStableStateGeneration_;
  generations[slot(Gen::ShaderLayout)] = drawShaderLayoutGeneration_;
  generations[slot(Gen::Uniform)] = drawUniformGeneration_;
  generations[slot(Gen::VertexShaderConstant)] =
      drawVertexShaderConstantGeneration_;
  generations[slot(Gen::PixelShaderConstant)] =
      drawPixelShaderConstantGeneration_;
  generations[slot(Gen::UniformNonConstant)] =
      drawUniformNonConstantGeneration_;
  generations[slot(Gen::RenderStateFlat)] = drawRenderStateFlatGeneration_;
  generations[slot(Gen::TextureStageStateFlat)] =
      drawTextureStageStateFlatGeneration_;
  generations[slot(Gen::SamplerStateFlat)] = drawSamplerStateFlatGeneration_;
  return generations;
}

CompatibilityDrawBatchIdentity Device::compatibilityDrawBatchIdentity(
    const DrawParam& draw,
    const DrawBindingOverride& binding) const noexcept {
  CompatibilityDrawBatchIdentity identity{};
  identity.generations = compatibilityDrawBatchGenerations();
  for (u32 stream = 0; stream < kMaxStreams; ++stream) {
    identity.streamBuffers[stream] = binding.streams[stream].buffer.value;
    identity.streamOffsets[stream] = binding.streams[stream].offset;
    // `refreshShaderLayoutExtraStreamStrides` reads the LIVE stride for every
    // stream >= 1 regardless of whether a buffer is bound, so the override's
    // (bound-only) stride is not a complete witness for the shader layout.
    identity.streamStrides[stream] = state_.streamStrides[stream];
  }
  identity.streamMask = binding.streamMask;
  identity.indexBuffer = binding.indexBuffer.value;
  identity.indexType = static_cast<std::uint32_t>(binding.indexType);
  identity.indexed = draw.indexed;
  identity.indexBufferValid = binding.indexBufferValid;
  identity.alphaTestEnable = binding.alphaTestEnable;
  identity.alphaTestFunc = binding.alphaTestFunc;
  identity.alphaTestRef = binding.alphaTestRef;
  identity.alphaTestStateValid = binding.alphaTestStateValid;
  return identity;
}

CompatibilityDrawBatchFlushStatus
Device::flushCompatibilityReplayDrawBatch() noexcept {
  if (compatBatchDraws_.empty() || compatBatchFlushing_) {
    return compatBatchFailed_ ? CompatibilityDrawBatchFlushStatus::Failed
                              : CompatibilityDrawBatchFlushStatus::Empty;
  }
  // The pending run is retired on every exit, thrown or not: leaving it behind
  // after a failed publish would let a later flush submit the same draws twice,
  // and leaving `compatBatchFlushing_` set would disable batching for the rest
  // of the process. Clearing keeps the vectors' capacity, so a steady-state
  // island pays no allocation.
  compatBatchFlushing_ = true;
  struct FlushScope {
    Device* device;
    ~FlushScope() {
      device->compatBatchDraws_.clear();
      device->compatBatchPayloads_.clear();
      device->compatBatchRuns_.clear();
      device->compatBatchEntries_.clear();
      device->compatBatchIdentity_ = {};
      device->compatBatchFlushing_ = false;
    }
  } flushScope{this};
  const auto draws = static_cast<std::uint64_t>(compatBatchDraws_.size());
  try {
    // A Device without an upper provider has nowhere to publish final wire.
    if (!upperDevice_) {
      compatBatchFailed_ = true;
      return CompatibilityDrawBatchFlushStatus::Failed;
    }
    compatBatchEntries_.reserve(compatBatchRuns_.size());
    for (auto& run : compatBatchRuns_) {
      compatBatchEntries_.push_back(DrawRunBatchEntry{
          .state = &run.state,
          .uniforms = &run.uniforms,
          .draws = std::span<const DrawParam>(
              compatBatchDraws_.data() + run.firstDraw, run.drawCount),
          .payloads = std::span<const DrawParamPayloadView>(
              compatBatchPayloads_.data() + run.firstDraw, run.drawCount),
      });
    }
    // One source-local publication carries all stateful runs. The production
    // DeviceImpl maps this to one CommandQueue transaction; the base virtual
    // hook remains a safe compatibility fallback for test providers.
    if (!upperDevice_->submitDrawRunBatch(compatBatchEntries_)) {
      compatBatchFailed_ = true;
      return CompatibilityDrawBatchFlushStatus::Failed;
    }
    dxmt9::perf::countCompatibilityDrawBatchPublished(draws);
    return CompatibilityDrawBatchFlushStatus::Published;
  } catch (...) {
    compatBatchFailed_ = true;
    return CompatibilityDrawBatchFlushStatus::Failed;
  }
}

DirectReplayDrawResult Device::submitCompatibilityReplayDrawBatched(
    DrawParam draw, DrawParamPayloadView payload) {
  // UP/TriangleFan and tracing still need an isolated effect. Ordinary draws
  // are retained as a bounded sequence of stateful runs and published in one
  // queue transaction at the next observable/control cut.
  const bool batchable =
      draw.primitiveType != PrimitiveType::TriangleFan &&
      payload.userVertexData.empty() && payload.userIndexData.empty() &&
      !renderTraceEnabled();
  if (!batchable) {
    const auto decision = core::compatibilityDrawBatchAdmission(
        /*batchable=*/false,
        static_cast<std::uint32_t>(compatBatchDraws_.size()),
        compatBatchIdentity_, compatBatchIdentity_);
    dxmt9::perf::countCompatibilityDrawBatchDecision(
        decision.admission, decision.cut);
    const auto flush = flushCompatibilityReplayDrawBatch();
    if (flush == CompatibilityDrawBatchFlushStatus::Failed) {
      return {D3DERR_DEVICELOST,
              DirectReplayDrawDisposition::AcceptedFailStop};
    }
    return submitDirectReplayDrawFromCurrentState(draw, payload, nullptr);
  }

  draw.primitiveType = canonicalPrimitiveType(draw.primitiveType);
  if (draw.indexed) {
    draw.indexType = state_.indexType;
  }

  // The cache may rebuild between stateful runs. The prior run owns a copied
  // canonical state, so that rebuild closes only the run, not the whole source
  // batch. This is the key difference from the old identical-state island.
  const bool openRunCacheIntact =
      !compatBatchRuns_.empty() && drawStateCacheBindingAgnostic_.valid &&
      compatBatchIdentity_.generations == compatibilityDrawBatchGenerations();
  const auto& cachedForOverride = openRunCacheIntact
      ? drawStateCacheBindingAgnostic_
      : cachedBaseDrawStateForSubmissionBatch();
  const auto bindingOverride =
      compatibilityDrawBindingOverride(draw, cachedForOverride);
  const auto identity = compatibilityDrawBatchIdentity(draw, bindingOverride);

  // The old helper's capacity is per identical-state run. Capacity for this
  // source-local batch is the total flat draw storage, handled below; use the
  // full uint32 range here so an identity change only closes the current run.
  const bool capacityCut =
      compatBatchDraws_.size() >= core::kCompatibilityDrawBatchMaxDraws;
  const auto decision = capacityCut
      ? core::CompatibilityDrawBatchDecision{
            core::CompatibilityDrawBatchAdmission::FlushAndStart,
            core::CompatibilityDrawBatchCut::Capacity}
      : core::compatibilityDrawBatchAdmission(
            /*batchable=*/true,
            compatBatchRuns_.empty()
                ? 0u
                : static_cast<std::uint32_t>(
                      compatBatchRuns_.back().drawCount),
            compatBatchIdentity_, identity,
            std::numeric_limits<std::uint32_t>::max());
  dxmt9::perf::countCompatibilityDrawBatchDecision(decision.admission,
                                                   decision.cut);
  if (capacityCut) {
    const auto flush = flushCompatibilityReplayDrawBatch();
    if (flush == CompatibilityDrawBatchFlushStatus::Failed) {
      return {D3DERR_DEVICELOST,
              DirectReplayDrawDisposition::AcceptedFailStop};
    }
    // The flush scope retired the old runs. The current cache is already the
    // state for this incoming draw and is borrowed only while constructing its
    // new owned run below.
  }
  const bool newRun = compatBatchRuns_.empty() ||
      decision.admission == core::CompatibilityDrawBatchAdmission::FlushAndStart;
  if (compatBatchRuns_.empty()) {
    try {
      compatBatchDraws_.reserve(core::kCompatibilityDrawBatchMaxDraws);
      compatBatchPayloads_.reserve(core::kCompatibilityDrawBatchMaxDraws);
      compatBatchRuns_.reserve(core::kCompatibilityDrawBatchMaxDraws);
      compatBatchEntries_.reserve(core::kCompatibilityDrawBatchMaxDraws);
    } catch (...) {
      compatBatchFailed_ = true;
      return {D3DERR_DEVICELOST,
              DirectReplayDrawDisposition::AcceptedFailStop};
    }
  }
  try {
    if (newRun) {
      DXMT_ASSERT(cachedForOverride.fullUniformsValid);
      CompatibilityBatchRun run{
          .state = CanonicalDrawState{
              cachedForOverride.hot,
              cachedForOverride.shaderLayout,
              makeDrawDebugSnapshot(
                  DrawCallArgs{draw.primitiveType, draw.primitiveCount,
                               draw.startVertex, draw.baseVertexIndex,
                               draw.startIndex, draw.indexType},
                  cachedForOverride.hot)},
          .uniforms = cachedForOverride.uniforms,
          .bindingOverride = bindingOverride,
          .firstDraw = compatBatchDraws_.size(),
          .drawCount = 0,
      };
      compatBatchRuns_.push_back(std::move(run));
      compatBatchIdentity_ = identity;
    }
    auto& run = compatBatchRuns_.back();
    payload.bindingOverrideData = drawBindingOverrideBytes(run.bindingOverride);
    compatBatchDraws_.push_back(draw);
    compatBatchPayloads_.push_back(payload);
    ++run.drawCount;
  } catch (...) {
    // A partial stateful batch cannot safely fall back after a source-local
    // publication has begun. Retire it and fail closed instead.
    compatBatchFailed_ = true;
    return {D3DERR_DEVICELOST,
            DirectReplayDrawDisposition::AcceptedFailStop};
  }
  if (activeOcclusionQuery_) {
    activeOcclusionCount_ += draw.primitiveCount;
  }
  ++submittedSequenceId_;
  DXMT_ASSERT(submittedSequenceId_ >= completedSequenceId_);
  return {D3D_OK, DirectReplayDrawDisposition::Appended};
}

DirectReplayDrawResult Device::submitDirectReplayDrawFromCurrentState(
    DrawParam draw, DrawParamPayloadView payload,
    const DirectReplayDrawAppendCapability* appendCapability) {
  // Every queue submission funnel publishes an open compatibility run first, so
  // no later path can overtake draws the sink has already accepted.
  if (flushCompatibilityReplayDrawBatch() ==
      CompatibilityDrawBatchFlushStatus::Failed) {
    return {D3DERR_DEVICELOST,
            DirectReplayDrawDisposition::AcceptedFailStop};
  }
  if (draw.primitiveType == PrimitiveType::TriangleFan) {
    return {D3DERR_INVALIDCALL,
            DirectReplayDrawDisposition::LegacyPreEffectFailure};
  }

  draw.primitiveType = canonicalPrimitiveType(draw.primitiveType);
  const auto& cached = cachedBaseDrawStateForSubmissionBatch();
  const auto bindingOverride = compatibilityDrawBindingOverride(draw, cached);
  payload.bindingOverrideData = drawBindingOverrideBytes(bindingOverride);

  if (renderTraceEnabled()) {
    CanonicalDrawState state{cached.hot, cached.shaderLayout,
                             DrawDebugSnapshot{}};
    const std::span<const DrawParam> draws(&draw, 1u);
    const std::span<const DrawParamPayloadView> payloads(&payload, 1u);
    const auto hr = submitDrawRunInternal(std::move(state), cached.uniforms,
                                          draws, payloads);
    return {hr, hr != D3D_OK
                ? DirectReplayDrawDisposition::AcceptedFailStop
                : DirectReplayDrawDisposition::LegacyUnsupported};
  }

  const DirectReplayDrawInput input{
      .hot = &cached.hot,
      .shaderLayout = &cached.shaderLayout,
      .uniforms = &cached.uniforms,
      .debug = makeDrawDebugSnapshot(
          DrawCallArgs{draw.primitiveType, draw.primitiveCount,
                       draw.startVertex, draw.baseVertexIndex,
                       draw.startIndex, draw.indexType},
          cached.hot),
      .draw = draw,
      .payload = payload,
  };
  DXMT_ASSERT(cached.fullUniformsValid);
  const auto disposition = appendCapability
      ? appendCapability->append(input)
      : upperDevice_
            ? upperDevice_->submitDirectReplayDraw(input)
            : DirectReplayDrawDisposition::AcceptedFailStop;
  if (appendCapability) {
    if (disposition != DirectReplayDrawDisposition::Appended) {
      return {disposition == DirectReplayDrawDisposition::AcceptedFailStop
                  ? D3DERR_DEVICELOST
                  : D3DERR_INVALIDCALL,
              disposition};
    }
  } else if (directReplayDrawPermitsLegacyFallback(disposition)) {
    // Typed Legacy dispositions are pre-effect. Test/stub devices and any
    // backend that cannot adopt the destination transaction retain the
    // existing serial submission path without making borrowed spans escape.
    CanonicalDrawState state{cached.hot, cached.shaderLayout, input.debug};
    const std::span<const DrawParam> draws(&draw, 1u);
    const std::span<const DrawParamPayloadView> payloads(&payload, 1u);
    upperDevice_->submitDrawRun(std::move(state), cached.uniforms, draws,
                                payloads);
  } else if (disposition ==
             DirectReplayDrawDisposition::AcceptedFailStop) {
    return {D3DERR_DEVICELOST, disposition};
  }
  if (activeOcclusionQuery_) {
    activeOcclusionCount_ += draw.primitiveCount;
  }
  ++submittedSequenceId_;
  DXMT_ASSERT(submittedSequenceId_ >= completedSequenceId_);
  return {D3D_OK, disposition};
}

HResult Device::submitDrawRunInternal(
    CanonicalDrawState state, const DrawUniformPayload &uniforms,
    std::span<const DrawParam> draws,
    std::span<const DrawParamPayloadView> payloads) {
  // Queue submission funnel: publish an open compatibility draw-run island
  // before any later submission can overtake the draws it already accepted.
  // Re-entrant from the flush itself, which the flush's own guard absorbs.
  if (flushCompatibilityReplayDrawBatch() ==
      CompatibilityDrawBatchFlushStatus::Failed) {
    deviceLost_ = true;
    return D3DERR_DEVICELOST;
  }
  if (draws.empty()) {
    return D3D_OK;
  }
  const auto firstPayload = drawPayloadAt(payloads, 0);
  state.debug = makeDrawDebugSnapshot(
      DrawCallArgs{draws.front().primitiveType, draws.front().primitiveCount,
                   draws.front().startVertex, draws.front().baseVertexIndex,
                   draws.front().startIndex, draws.front().indexType},
      state.hot);
  state.debug.userVertexBytes = static_cast<u32>(std::min<std::size_t>(
      firstPayload.userVertexData.size(), std::numeric_limits<u32>::max()));
  state.debug.userIndexBytes = static_cast<u32>(std::min<std::size_t>(
      firstPayload.userIndexData.size(), std::numeric_limits<u32>::max()));

  const auto &hot = state.hot;
  if (renderTraceEnabled()) {
    const auto &shader = state.shaderLayout;
    const auto stageState = [&](size_t stageIndex, u32 key) -> u32 {
      if (stageIndex >= hot.textureStageStates.size()) {
        return 0u;
      }
      return flatStateOr(hot.textureStageStates[stageIndex], key, 0u);
    };
    const auto renderState = [&](u32 key, u32 fallback = 0u) -> u32 {
      return flatStateOr(hot.renderStates, key, fallback);
    };
    for (size_t i = 0; i < draws.size(); ++i) {
      const auto &draw = draws[i];
      emitRenderTrace(
          "draw seq=%llu primType=%u primCount=%u startVertex=%u baseVertex=%d "
          "startIndex=%u rt0=0x%llx ds=0x%llx tex0=0x%llx vs=%u ps=%u "
          "vsHash=0x%llx psHash=0x%llx stateHash=0x%llx fvf=0x%x lighting=%u "
          "cull=%u alphaTest=%u alphaBlend=%u srcBlend=%u dstBlend=%u "
          "colorOp0=%u alphaOp0=%u tcIdx0=0x%x ttff0=0x%x colorOp1=%u "
          "alphaOp1=%u tcIdx1=0x%x ttff1=0x%x scissor=%u "
          "scissorRect=%d,%d-%d,%d clipMask=0x%x indexed=%u",
          static_cast<unsigned long long>(submittedSequenceId_ + 1 + i),
          static_cast<unsigned>(draw.primitiveType), draw.primitiveCount,
          draw.startVertex, draw.baseVertexIndex, draw.startIndex,
          static_cast<unsigned long long>(hot.colorAttachments[0].handle.value),
          static_cast<unsigned long long>(hot.depthStencil.handle.value),
          static_cast<unsigned long long>(hot.textures[0].value),
          static_cast<unsigned>(shader.vertexShader.kind),
          static_cast<unsigned>(shader.pixelShader.kind),
          static_cast<unsigned long long>(hashShaderRef(shader.vertexShader)),
          static_cast<unsigned long long>(hashShaderRef(shader.pixelShader)),
          static_cast<unsigned long long>(hot.key.renderStateHash),
          shader.vertexDecl.fvf, renderState(RS_LIGHTING),
          renderState(RS_CULL_MODE, static_cast<u32>(CullMode::Ccw)),
          renderState(RS_ALPHA_TEST_ENABLE), renderState(RS_ALPHABLEND_ENABLE),
          renderState(RS_SRC_BLEND), renderState(RS_DEST_BLEND),
          stageState(0, TSS_COLOR_OP), stageState(0, TSS_ALPHA_OP),
          stageState(0, TSS_TEXCOORD_INDEX),
          stageState(0, TSS_TEXTURE_TRANSFORM_FLAGS),
          stageState(1, TSS_COLOR_OP), stageState(1, TSS_ALPHA_OP),
          stageState(1, TSS_TEXCOORD_INDEX),
          stageState(1, TSS_TEXTURE_TRANSFORM_FLAGS),
          hot.viewport.scissorEnabled ? 1u : 0u,
          hot.viewport.scissor.left, hot.viewport.scissor.top,
          hot.viewport.scissor.right, hot.viewport.scissor.bottom,
          hot.clipPlaneMask,
          draw.indexed ? 1u : 0u);
    }
  }

  const auto drawCount = static_cast<u64>(draws.size());
  if (!upperDevice_) {
    deviceLost_ = true;
    return D3DERR_DEVICELOST;
  }
  if (activeOcclusionQuery_) {
    for (const auto &draw : draws) {
      activeOcclusionCount_ += draw.primitiveCount;
    }
  }
  upperDevice_->submitDrawRun(std::move(state), uniforms, draws, payloads);
  submittedSequenceId_ += drawCount;
  DXMT_ASSERT(submittedSequenceId_ >= completedSequenceId_);
  return D3D_OK;
}


HResult Device::drawPrimitiveRun(std::span<const DrawParam> draws) {
  return drawPrimitiveRun(draws, {});
}

HResult Device::drawPrimitiveRun(std::span<const DrawParam> draws,
                                 std::span<const DrawParamPayloadView> externalPayloads) {
  if (draws.empty()) {
    return D3D_OK;
  }

  std::vector<DrawParam> normalized(draws.begin(), draws.end());
  std::vector<std::vector<u8>> indexPayloadStorage(normalized.size());
  std::vector<DrawParamPayloadView> payloads(normalized.size());
  for (std::size_t i = 0; i < payloads.size(); ++i) {
    payloads[i] = drawPayloadAt(externalPayloads, i);
  }

  for (std::size_t i = 0; i < normalized.size(); ++i) {
    auto &draw = normalized[i];
    draw.instanceCount = drawInstanceCountFromState(state_);
    const auto originalType = draw.primitiveType;
    draw.primitiveType = canonicalPrimitiveType(draw.primitiveType);
    if (originalType != PrimitiveType::TriangleFan) {
      continue;
    }

    auto &indexPayload = indexPayloadStorage[i];
    if (draw.indexed) {
      if (!state_.indexBuffer) {
        return D3DERR_INVALIDCALL;
      }
      if (!writeIndexedTriangleFanIndexPayload(
              indexPayload, state_.indexBuffer->bytes(), draw.primitiveCount,
              draw.startIndex, draw.indexType)) {
        return D3DERR_INVALIDCALL;
      }
    } else {
      draw.indexed = true;
      draw.baseVertexIndex = static_cast<i32>(draw.startVertex);
      draw.indexType = writeSequentialTriangleFanIndexPayload(
          indexPayload, draw.primitiveCount);
    }
    draw.startIndex = 0;
    payloads[i].userIndexData =
        std::span<const u8>(indexPayload.data(), indexPayload.size());
  }

  return submitDrawRunInternalFromCurrentState(normalized, payloads);
}

HResult Device::drawPrimitive(PrimitiveType type, u32 primitiveCount,
                              u32 startVertex) {
  DrawParam draw{};
  draw.primitiveType = canonicalPrimitiveType(type);
  draw.primitiveCount = primitiveCount;
  draw.startVertex = startVertex;
  draw.indexType = state_.indexType;
  draw.indexed = false;
  draw.instanceCount = drawInstanceCountFromState(state_);
  if (type == PrimitiveType::TriangleFan) {
    draw.indexed = true;
    draw.baseVertexIndex = static_cast<i32>(startVertex);
    draw.startIndex = 0;
    draw.indexType =
        writeSequentialTriangleFanIndexPayload(upIndexScratch_, primitiveCount);
    const DrawParamPayloadView payload{
        .userIndexData =
            std::span<const u8>(upIndexScratch_.data(), upIndexScratch_.size()),
    };
    const auto hr = submitDrawRunInternalFromCurrentState(
        std::span<const DrawParam>(&draw, 1),
        std::span<const DrawParamPayloadView>(&payload, 1));
    if (state_.inScene) {
      // No-op; draw submission is immediate in the core harness.
    }
    return hr;
  }
  const auto hr =
      submitDrawRunInternalFromCurrentState(std::span<const DrawParam>(&draw, 1));
  if (state_.inScene) {
    // No-op; draw submission is immediate in the core harness.
  }
  return hr;
}

HResult Device::drawIndexedPrimitive(PrimitiveType type, u32 primitiveCount,
                                     u32 startVertex, i32 baseVertexIndex,
                                     u32 startIndex, IndexType indexType) {
  DrawParam draw{};
  draw.primitiveType = canonicalPrimitiveType(type);
  draw.primitiveCount = primitiveCount;
  draw.startVertex = startVertex;
  draw.baseVertexIndex = baseVertexIndex;
  draw.startIndex = startIndex;
  draw.indexType = indexType;
  draw.indexed = true;
  draw.instanceCount = drawInstanceCountFromState(state_);
  if (type == PrimitiveType::TriangleFan) {
    if (!state_.indexBuffer) {
      return D3DERR_INVALIDCALL;
    }
    if (!writeIndexedTriangleFanIndexPayload(
            upIndexScratch_, state_.indexBuffer->bytes(), primitiveCount,
            startIndex, indexType)) {
      return D3DERR_INVALIDCALL;
    }
    draw.startIndex = 0;
    const DrawParamPayloadView payload{
        .userIndexData =
            std::span<const u8>(upIndexScratch_.data(), upIndexScratch_.size()),
    };
    const auto hr = submitDrawRunInternalFromCurrentState(
        std::span<const DrawParam>(&draw, 1),
        std::span<const DrawParamPayloadView>(&payload, 1));
    return hr;
  }
  return submitDrawRunInternalFromCurrentState(std::span<const DrawParam>(&draw, 1));
}

HResult Device::drawPrimitiveUP(PrimitiveType type, u32 primitiveCount,
                                std::span<const u8> vertexData,
                                u32 vertexStride) {
  upVertexScratch_.assign(vertexData.begin(), vertexData.end());
  DeviceState drawState = state_;
  // UP draws source stream 0 from caller memory, not the currently bound VB.
  drawState.streamBuffers[0].reset();
  drawState.streamOffsets[0] = 0;
  if (vertexStride != 0) {
    drawState.streamStrides[0] = vertexStride;
  }
  DrawParam draw{};
  draw.primitiveType = canonicalPrimitiveType(type);
  draw.primitiveCount = primitiveCount;
  draw.indexType = state_.indexType;
  draw.indexed = false;
  if (type == PrimitiveType::TriangleFan) {
    const u32 stride =
        vertexStride != 0
            ? vertexStride
            : inferStreamZeroStride(makeVertexDeclSnapshotFromState(drawState));
    auto decomposed =
        decomposeTriangleFanVertices(vertexData, primitiveCount, stride);
    if (primitiveCount != 0 && decomposed.empty()) {
      return D3DERR_INVALIDCALL;
    }
    const DrawParamPayloadView payload{
        .userVertexData =
            std::span<const u8>(decomposed.data(), decomposed.size()),
    };
    return submitDrawRunInternalFromState(
        drawState, std::span<const DrawParam>(&draw, 1),
        std::span<const DrawParamPayloadView>(&payload, 1));
  } else {
    const DrawParamPayloadView payload{
        .userVertexData = std::span<const u8>(upVertexScratch_.data(),
                                              upVertexScratch_.size()),
    };
    return submitDrawRunInternalFromState(
        drawState, std::span<const DrawParam>(&draw, 1),
        std::span<const DrawParamPayloadView>(&payload, 1));
  }
}

HResult Device::drawIndexedPrimitiveUP(PrimitiveType type, u32 primitiveCount,
                                       std::span<const u8> vertexData,
                                       std::span<const u8> indexData,
                                       IndexType indexType, u32 vertexStride) {
  upVertexScratch_.assign(vertexData.begin(), vertexData.end());
  if (type == PrimitiveType::TriangleFan) {
    const bool decomposed =
        indexType == IndexType::UInt32
            ? writeTriangleFanIndexBytes<u32>(upIndexScratch_, indexData,
                                              primitiveCount)
            : writeTriangleFanIndexBytes<u16>(upIndexScratch_, indexData,
                                              primitiveCount);
    if (!decomposed) {
      return D3DERR_INVALIDCALL;
    }
    type = PrimitiveType::TriangleList;
  } else {
    upIndexScratch_.assign(indexData.begin(), indexData.end());
  }
  DeviceState drawState = state_;
  // Indexed UP draws also source stream 0 from caller memory.
  drawState.streamBuffers[0].reset();
  drawState.streamOffsets[0] = 0;
  if (vertexStride != 0) {
    drawState.streamStrides[0] = vertexStride;
  }
  DrawParam draw{};
  draw.primitiveType = canonicalPrimitiveType(type);
  draw.primitiveCount = primitiveCount;
  draw.indexType = indexType;
  draw.indexed = true;
  const DrawParamPayloadView payload{
      .userVertexData =
          std::span<const u8>(upVertexScratch_.data(), upVertexScratch_.size()),
      .userIndexData =
          std::span<const u8>(upIndexScratch_.data(), upIndexScratch_.size()),
  };
  return submitDrawRunInternalFromState(
      drawState, std::span<const DrawParam>(&draw, 1),
      std::span<const DrawParamPayloadView>(&payload, 1));
}

// Determinism: for a fixed `DeviceState` value, repeated calls produce
// byte-identical keys (including `key.hash`). Every read uses
// `contains() ? at() : default` so a missing render state collapses to a
// well-defined zero rather than reading uninitialized storage. Sensitivity:
// any bit toggled in the render-state map fields enumerated below MUST
// alter the resulting key — silent collisions cause PSO cache merging.
// Tests: `tests/native/core/core_ffp_state_key_spec.cpp` and
// `tests/native/backend/ffp_key_determinism_spec.cpp` enforce both
// directions.
FfpVertexKey makeFfpVertexKey(const DeviceState &state) {
  FfpVertexKey key;
  key.lightingEnabled = state.renderStates.contains(RS_LIGHTING) &&
                        state.renderStates.at(RS_LIGHTING) != 0;
  key.specularEnabled = state.renderStates.contains(RS_SPECULAR_ENABLE) &&
                        state.renderStates.at(RS_SPECULAR_ENABLE) != 0;
  key.normalizeNormals = state.renderStates.contains(RS_NORMALIZE_NORMALS) &&
                         state.renderStates.at(RS_NORMALIZE_NORMALS) != 0;
  key.localViewer = !state.renderStates.contains(RS_LOCAL_VIEWER) ||
                    state.renderStates.at(RS_LOCAL_VIEWER) != 0;
  key.colorVertexEnabled = !state.renderStates.contains(141u /*RS_COLORVERTEX*/) ||
                           state.renderStates.at(141u /*RS_COLORVERTEX*/) != 0;
  for (size_t i = 0; i < kMaxLights; ++i) {
    key.lightEnabled[i] = state.lightEnabled[i];
    key.lightType[i] = static_cast<u32>(state.lights[i].type);
  }
  auto materialSourceState = [&](u32 stateKey) {
    if (!key.colorVertexEnabled) {
      return 0u;
    }
    return state.renderStates.contains(stateKey) ? state.renderStates.at(stateKey) : 0u;
  };
  key.colorMaterialMode[0] = materialSourceState(RS_EMISSIVE_MATERIAL_SOURCE);
  key.colorMaterialMode[1] = materialSourceState(RS_AMBIENT_MATERIAL_SOURCE);
  key.colorMaterialMode[2] = materialSourceState(RS_DIFFUSE_MATERIAL_SOURCE);
  key.colorMaterialMode[3] = materialSourceState(RS_SPECULAR_MATERIAL_SOURCE);
  const auto tableFogMode =
      static_cast<FogMode>(state.renderStates.contains(RS_FOG_TABLE_MODE)
                               ? state.renderStates.at(RS_FOG_TABLE_MODE)
                               : 0);
  const auto vertexFogMode =
      static_cast<FogMode>(state.renderStates.contains(RS_FOG_FROM_VERTEX)
                               ? state.renderStates.at(RS_FOG_FROM_VERTEX)
                               : 0);
  key.fogMode = tableFogMode != FogMode::None ? tableFogMode : vertexFogMode;
  key.fogFromVertex = state.renderStates.contains(RS_FOG_FROM_VERTEX) &&
                      state.renderStates.at(RS_FOG_FROM_VERTEX) != 0;
  key.rangeFog = state.renderStates.contains(RS_RANGE_FOG) &&
                 state.renderStates.at(RS_RANGE_FOG) != 0;
  for (size_t i = 0; i < kMaxTextureStages; ++i) {
    key.texCoordGen[i] =
        state.textureStageStates[i].contains(TSS_TEXCOORD_INDEX)
            ? state.textureStageStates[i].at(TSS_TEXCOORD_INDEX)
            : 0;
    key.texTransformFlags[i] =
        state.textureStageStates[i].contains(TSS_TEXTURE_TRANSFORM_FLAGS)
            ? state.textureStageStates[i].at(TSS_TEXTURE_TRANSFORM_FLAGS)
            : 0;
  }
  key.vertexBlend = state.renderStates.contains(RS_VERTEX_BLEND)
                        ? state.renderStates.at(RS_VERTEX_BLEND)
                        : 0;
  key.indexedVertexBlend =
      state.renderStates.contains(RS_INDEXED_VERTEX_BLEND_ENABLE) &&
      state.renderStates.at(RS_INDEXED_VERTEX_BLEND_ENABLE) != 0;
  key.clipPlaneMask = state.renderStates.contains(RS_CLIP_PLANE_ENABLE)
                          ? state.renderStates.at(RS_CLIP_PLANE_ENABLE)
                          : 0;
  key.pointSpriteEnable =
      state.renderStates.contains(RS_POINT_SPRITE_ENABLE) &&
      state.renderStates.at(RS_POINT_SPRITE_ENABLE) != 0;
  key.pointScaleEnable =
      state.renderStates.contains(RS_POINT_SCALE_ENABLE) &&
      state.renderStates.at(RS_POINT_SCALE_ENABLE) != 0;
  key.hash = hashFfpVertexKey(key);
  return key;
}

// Same determinism + sensitivity contract as `makeFfpVertexKey`. Each
// per-stage field reads from `state.textureStageStates[stage]` with a 0
// default when absent, so the key is order-independent on the underlying
// map storage. Guard tests live alongside the vertex builder.
FfpPixelKey makeFfpPixelKey(const DeviceState &state) {
  FfpPixelKey key;
  for (size_t stage = 0; stage < kMaxTextureStages; ++stage) {
    const auto &map = state.textureStageStates[stage];
    auto &out = key.stages[stage];
    out.colorOp = map.contains(TSS_COLOR_OP) ? map.at(TSS_COLOR_OP) : 0;
    out.colorArg1 = map.contains(TSS_COLOR_ARG1) ? map.at(TSS_COLOR_ARG1) : 0;
    out.colorArg2 = map.contains(TSS_COLOR_ARG2) ? map.at(TSS_COLOR_ARG2) : 0;
    // D3DTSS_COLORARG0/ALPHAARG0 default to D3DTA_CURRENT (1), not D3DTA_DIFFUSE (0)
    // — the triadic ops (MULTIPLYADD/LERP) read arg0 and rely on this default.
    out.colorArg0 = map.contains(TSS_COLOR_ARG0) ? map.at(TSS_COLOR_ARG0) : 1;
    out.alphaOp = map.contains(TSS_ALPHA_OP) ? map.at(TSS_ALPHA_OP) : 0;
    out.alphaArg1 = map.contains(TSS_ALPHA_ARG1) ? map.at(TSS_ALPHA_ARG1) : 0;
    out.alphaArg2 = map.contains(TSS_ALPHA_ARG2) ? map.at(TSS_ALPHA_ARG2) : 0;
    out.alphaArg0 = map.contains(TSS_ALPHA_ARG0) ? map.at(TSS_ALPHA_ARG0) : 1;
    out.resultArg = map.contains(TSS_RESULT_ARG) ? map.at(TSS_RESULT_ARG) : 0;
    out.texType = map.contains(TSS_TEXTURE_TYPE) ? map.at(TSS_TEXTURE_TYPE) : 0;
    out.texCoordIndex =
        map.contains(TSS_TEXCOORD_INDEX) ? map.at(TSS_TEXCOORD_INDEX) : 0;
  }
  const auto tableFogMode =
      static_cast<FogMode>(state.renderStates.contains(RS_FOG_TABLE_MODE)
                               ? state.renderStates.at(RS_FOG_TABLE_MODE)
                               : 0);
  const auto vertexFogMode =
      static_cast<FogMode>(state.renderStates.contains(RS_FOG_FROM_VERTEX)
                               ? state.renderStates.at(RS_FOG_FROM_VERTEX)
                               : 0);
  key.fogMode = tableFogMode != FogMode::None ? tableFogMode : vertexFogMode;
  key.alphaTestEnable = state.renderStates.contains(RS_ALPHA_TEST_ENABLE) &&
                        state.renderStates.at(RS_ALPHA_TEST_ENABLE) != 0;
  key.alphaTestFunc = state.renderStates.contains(RS_ALPHA_FUNC)
                          ? state.renderStates.at(RS_ALPHA_FUNC)
                          : 0;
  key.pointSpriteEnable =
      state.renderStates.contains(RS_POINT_SPRITE_ENABLE) &&
      state.renderStates.at(RS_POINT_SPRITE_ENABLE) != 0;
  key.hash = hashFfpPixelKey(key);
  return key;
}

} // namespace dxmt9::core
