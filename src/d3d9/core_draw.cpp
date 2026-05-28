#include "dxmt9/assert.hpp"
#include "dxmt9/core.hpp"
#include "dxmt9/dxmt9_device.hpp"
#include "dxmt9/dxmt9_d3d9_bytecode.hpp"
#include "util/config/config.hpp"
#include "util/log/log.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdarg>
#include <cstdio>
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
  if (shader.kind != ShaderRef::Kind::Bytecode || shader.bytecode.bytes.size() < sizeof(u32)) {
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

u64 hashDrawUniformPayload(const DrawUniformPayload &payload) {
  u64 hash = hashCombine(kFnvOffset, hashTrivial(payload.vsConst));
  hash = hashCombine(hash, hashTrivial(payload.psConst));
  hash = hashCombine(hash, hashTrivial(payload.worldViewProj));
  hash = hashCombine(hash, hashTrivial(payload.ffpWorldView));
  hash = hashCombine(hash, hashTrivial(payload.ffpNormalMatrix));
  hash = hashCombine(hash, hashMaterial(payload.material));
  for (const auto &light : payload.lights) {
    hash = hashCombine(hash, hashLight(light));
  }
  hash = hashCombine(hash, hashBlendWorldViewProj(payload.ffpBlendWorldViewProj));
  hash = hashCombine(hash, hashTextureTransforms(payload.textureTransforms));
  hash = hashCombine(hash, payload.clipPlaneMask);
  hash = hashCombine(hash, hashClipPlanes(payload.clipPlanes));
  return hash;
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

ClipPlane transformClipPlane(const Matrix4x4 &transform,
                             const ClipPlane &plane) {
  Matrix4x4 inverse{};
  if (!invertMatrix(transform, &inverse)) {
    return plane;
  }
  const Matrix4x4 inverseTranspose = transposeMatrix(inverse);
  ClipPlane out{};
  for (size_t row = 0; row < 4; ++row) {
    float sum = 0.0f;
    for (size_t col = 0; col < 4; ++col) {
      sum += inverseTranspose.m[row * 4 + col] * plane[col];
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

} // namespace

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

u32 clipPlaneMaskFromState(const DeviceState &state) {
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
makeClipPlanesFromState(const DeviceState &state, u32 clipPlaneMask,
                        const Matrix4x4 &worldViewProj) {
  std::array<ClipPlane, kMaxClipPlanes> clipPlanes{};
  for (size_t i = 0; i < kMaxClipPlanes; ++i) {
    if ((clipPlaneMask & (1u << i)) != 0) {
      clipPlanes[i] = transformClipPlane(worldViewProj, state.clipPlanes[i]);
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

std::array<Matrix4x4, 4> makeBlendWorldViewProjFromState(const DeviceState &state) {
  std::array<Matrix4x4, 4> transforms{};
  const Matrix4x4 view = lookupTransform(state, XFORM_VIEW);
  const Matrix4x4 proj = lookupTransform(state, XFORM_PROJECTION);
  for (size_t i = 0; i < transforms.size(); ++i) {
    const Matrix4x4 world =
        lookupTransform(state, XFORM_WORLD_BASE + static_cast<u32>(i));
    transforms[i] = multiplyMatrix(multiplyMatrix(world, view), proj);
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

DrawUniformPayload makeDrawUniformPayloadFromState(const DeviceState &state,
                                                   u32 clipPlaneMask) {
  DrawUniformPayload payload{};
  payload.vsConst = state.vsConst;
  payload.psConst = state.psConst;
  payload.ffpWorldView = makeWorldViewFromState(state);
  payload.ffpNormalMatrix = makeNormalMatrixFromWorldView(payload.ffpWorldView);
  payload.worldViewProj = makeWorldViewProjFromState(state);
  payload.material = state.material;
  payload.lights = state.lights;
  payload.ffpBlendWorldViewProj = makeBlendWorldViewProjFromState(state);
  payload.textureTransforms = makeTextureTransformsFromState(state);
  payload.clipPlaneMask = clipPlaneMask;
  payload.clipPlanes =
      makeClipPlanesFromState(state, clipPlaneMask, payload.worldViewProj);
  payload.hash = hashDrawUniformPayload(payload);
  return payload;
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
  desc.ffpBlendWorldViewProj = uniforms.ffpBlendWorldViewProj;
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
  key.vertexConstantsHash = hashTrivial(desc.vsConst);
  key.pixelConstantsHash = hashTrivial(desc.psConst);

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
  record.streamMask = record.key.streamMask;
  record.indexBuffer = record.key.indexBuffer;
  record.textures = record.key.textures;
  record.textureLods = record.key.textureLods;
  record.textureMask = record.key.textureMask;
  record.renderStates = makeFlatStateSet<kMaxStateSlots>(desc.rs.values);
  for (size_t i = 0; i < kMaxTextureStages; ++i) {
    record.textureStageStates[i] =
        makeFlatStateSet<kMaxTextureStageStates>(desc.textures[i].stageStates);
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
  record.clipPlaneMask = record.key.clipPlaneMask;
  record.clipPlanesHash = record.key.clipPlanesHash;
  return record;
}

} // namespace fixture

namespace {

FlatDrawStateKey makeFlatDrawStateKeyFromState(
    const DeviceState &state, const DrawShaderLayoutContext &shaderLayout,
    const DrawUniformPayload &uniforms, const ViewportScissor &viewport) {
  FlatDrawStateKey key{};

  for (size_t i = 0; i < kMaxStreams; ++i) {
    key.streamBuffers[i] =
        state.streamBuffers[i] ? state.streamBuffers[i]->handle() : Handle{};
    key.streamOffsets[i] = state.streamOffsets[i];
    key.streamStrides[i] = state.streamStrides[i];
    if (key.streamBuffers[i]) {
      key.streamMask |= 1u << i;
    }
  }

  key.indexBuffer = state.indexBuffer ? state.indexBuffer->handle() : Handle{};
  key.vertexElementCount =
      static_cast<u32>(shaderLayout.vertexDecl.elements.size());
  key.fvf = shaderLayout.vertexDecl.fvf;
  key.vertexDeclHash = hashVertexDeclElements(shaderLayout.vertexDecl);
  key.vertexShaderKind = shaderLayout.vertexShader.kind;
  key.pixelShaderKind = shaderLayout.pixelShader.kind;
  key.vertexShaderHash = hashShaderRefSummary(shaderLayout.vertexShader);
  key.pixelShaderHash = hashShaderRefSummary(shaderLayout.pixelShader);
  key.vertexConstantsHash = hashTrivial(state.vsConst);
  key.pixelConstantsHash = hashTrivial(state.psConst);

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

  for (size_t i = 0; i < kMaxSamplers; ++i) {
    key.samplerStateHashes[i] = hashStateMap(state.samplerStates[i]);
    if (!state.samplerStates[i].empty()) {
      key.samplerStateMask |= 1u << i;
    }
  }

  key.renderStateHash = hashStateMap(state.renderStates);
  key.colorAttachments = state.renderTargets;
  key.depthStencil = state.depthStencil;
  for (size_t i = 0; i < kMaxRenderTargets; ++i) {
    if (key.colorAttachments[i].handle) {
      key.renderTargetMask |= 1u << i;
    }
  }

  key.viewportHash = hashViewportScissor(viewport);
  key.worldViewProjHash = hashTrivial(uniforms.worldViewProj);
  key.ffpBlendWorldViewProjHash = hashBlendWorldViewProj(uniforms.ffpBlendWorldViewProj);
  key.textureTransformsHash = hashTextureTransforms(uniforms.textureTransforms);
  key.clipPlaneMask = shaderLayout.clipPlaneMask;
  key.clipPlanesHash = hashClipPlanes(uniforms.clipPlanes);
  return key;
}

FlatDrawStateRecord makeFlatDrawStateRecordFromState(
    const DeviceState &state, const DrawShaderLayoutContext &shaderLayout,
    const DrawUniformPayload &uniforms, const ViewportScissor &viewport) {
  FlatDrawStateRecord record{};
  record.key =
      makeFlatDrawStateKeyFromState(state, shaderLayout, uniforms, viewport);
  record.streamBuffers = record.key.streamBuffers;
  record.streamOffsets = record.key.streamOffsets;
  record.streamStrides = record.key.streamStrides;
  record.streamMask = record.key.streamMask;
  record.indexBuffer = record.key.indexBuffer;
  record.textures = record.key.textures;
  record.textureLods = record.key.textureLods;
  record.textureMask = record.key.textureMask;
  record.renderStates = makeFlatStateSet<kMaxStateSlots>(state.renderStates);
  for (size_t i = 0; i < kMaxTextureStages; ++i) {
    record.textureStageStates[i] =
        makeFlatStateSet<kMaxTextureStageStates>(state.textureStageStates[i]);
  }
  for (size_t i = 0; i < kMaxSamplers; ++i) {
    record.samplerStates[i] =
        makeFlatStateSet<kMaxSamplerStates>(state.samplerStates[i]);
  }
  record.colorAttachments = record.key.colorAttachments;
  record.depthStencil = record.key.depthStencil;
  record.renderTargetMask = record.key.renderTargetMask;
  record.viewport = viewport;
  record.vertexConstantsHash = record.key.vertexConstantsHash;
  record.pixelConstantsHash = record.key.pixelConstantsHash;
  record.worldViewProjHash = record.key.worldViewProjHash;
  record.ffpBlendWorldViewProjHash = record.key.ffpBlendWorldViewProjHash;
  record.textureTransformsHash = record.key.textureTransformsHash;
  record.clipPlaneMask = record.key.clipPlaneMask;
  record.clipPlanesHash = record.key.clipPlanesHash;
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
  payload.ffpWorldView = desc.ffpWorldView;
  payload.ffpNormalMatrix = desc.ffpNormalMatrix;
  payload.material = desc.material;
  payload.lights = desc.lights;
  payload.ffpBlendWorldViewProj = desc.ffpBlendWorldViewProj;
  payload.textureTransforms = desc.textureTransforms;
  payload.clipPlaneMask = desc.clipPlaneMask;
  payload.clipPlanes = desc.clipPlanes;
  payload.hash = hashDrawUniformPayload(payload);
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
  auto uniforms =
      makeDrawUniformPayloadFromState(state, shaderLayout.clipPlaneMask);
  const auto viewport = makeViewportScissorFromState(state);
  auto hot =
      makeFlatDrawStateRecordFromState(state, shaderLayout, uniforms, viewport);
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
  constexpr auto kMaxRange = std::numeric_limits<u32>::max();
  const std::uint64_t requiredSize =
      static_cast<std::uint64_t>(drawPayloadStorageSize(payloadArena)) +
      static_cast<std::uint64_t>(vertexBytes.size()) +
      static_cast<std::uint64_t>(indexBytes.size());
  if (requiredSize > kMaxRange) {
    return false;
  }

  return appendDrawPayload(payloadArena, vertexBytes, param.userVertexRange) &&
         appendDrawPayload(payloadArena, indexBytes, param.userIndexRange);
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
        !drawPayloadRangeValid(payloadSize, param.userIndexRange)) {
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

void Device::submitDrawRunInternalFromState(
    DeviceState baseState, std::span<const DrawParam> draws,
    std::span<const DrawParamPayloadView> payloads) {
  if (draws.empty()) {
    return;
  }
  if (!drawRunUsesBoundIndexBuffer(draws, payloads)) {
    baseState.indexBuffer.reset();
  }
  auto shaderLayout = makeDrawShaderLayoutContextFromState(baseState);
  auto uniforms =
      makeDrawUniformPayloadFromState(baseState, shaderLayout.clipPlaneMask);
  const auto viewport = makeViewportScissorFromState(baseState);
  auto hot = makeFlatDrawStateRecordFromState(baseState, shaderLayout, uniforms,
                                              viewport);
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
  submitDrawRunInternal(std::move(state), uniforms, draws, payloads);
}

void Device::invalidateDrawStateCache() noexcept {
  ++drawStateGeneration_;
  if (drawStateGeneration_ == 0) {
    drawStateGeneration_ = 1;
  }
}

const Device::CachedBaseDrawState &
Device::cachedBaseDrawState(bool includeIndexBuffer) {
  auto &cache =
      includeIndexBuffer ? drawStateCacheWithIndex_ : drawStateCacheNoIndex_;
  if (cache.valid && cache.generation == drawStateGeneration_) {
    return cache;
  }

  DeviceState baseState = state_;
  if (!includeIndexBuffer) {
    baseState.indexBuffer.reset();
  }
  cache.shaderLayout = makeDrawShaderLayoutContextFromState(baseState);
  cache.uniforms = makeDrawUniformPayloadFromState(
      baseState, cache.shaderLayout.clipPlaneMask);
  const auto viewport = makeViewportScissorFromState(baseState);
  cache.hot = makeFlatDrawStateRecordFromState(baseState, cache.shaderLayout,
                                               cache.uniforms, viewport);
  cache.generation = drawStateGeneration_;
  cache.valid = true;
  return cache;
}

void Device::submitDrawRunInternalFromCurrentState(
    std::span<const DrawParam> draws,
    std::span<const DrawParamPayloadView> payloads) {
  if (draws.empty()) {
    return;
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
  submitDrawRunInternal(std::move(state), cached.uniforms, draws, payloads);
}

void Device::submitDrawRunInternal(
    CanonicalDrawState state, const DrawUniformPayload &uniforms,
    std::span<const DrawParam> draws,
    std::span<const DrawParamPayloadView> payloads) {
  if (draws.empty()) {
    return;
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
  if (activeOcclusionQuery_) {
    for (const auto &draw : draws) {
      activeOcclusionCount_ += draw.primitiveCount;
    }
  }
  upperDevice_->submitDrawRun(std::move(state), uniforms, draws, payloads);
  submittedSequenceId_ += drawCount;
  DXMT_ASSERT(submittedSequenceId_ >= completedSequenceId_);
}

HResult Device::drawPrimitiveRun(std::span<const DrawParam> draws) {
  if (draws.empty()) {
    return D3D_OK;
  }

  std::vector<DrawParam> normalized(draws.begin(), draws.end());
  std::vector<std::vector<u8>> indexPayloadStorage(normalized.size());
  std::vector<DrawParamPayloadView> payloads(normalized.size());

  for (std::size_t i = 0; i < normalized.size(); ++i) {
    auto &draw = normalized[i];
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

  submitDrawRunInternalFromCurrentState(normalized, payloads);
  return D3D_OK;
}

HResult Device::drawPrimitive(PrimitiveType type, u32 primitiveCount,
                              u32 startVertex) {
  DrawParam draw{};
  draw.primitiveType = canonicalPrimitiveType(type);
  draw.primitiveCount = primitiveCount;
  draw.startVertex = startVertex;
  draw.indexType = state_.indexType;
  draw.indexed = false;
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
    submitDrawRunInternalFromCurrentState(
        std::span<const DrawParam>(&draw, 1),
        std::span<const DrawParamPayloadView>(&payload, 1));
    if (state_.inScene) {
      // No-op; draw submission is immediate in the core harness.
    }
    return D3D_OK;
  }
  submitDrawRunInternalFromCurrentState(std::span<const DrawParam>(&draw, 1));
  if (state_.inScene) {
    // No-op; draw submission is immediate in the core harness.
  }
  return D3D_OK;
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
    submitDrawRunInternalFromCurrentState(
        std::span<const DrawParam>(&draw, 1),
        std::span<const DrawParamPayloadView>(&payload, 1));
    return D3D_OK;
  }
  submitDrawRunInternalFromCurrentState(std::span<const DrawParam>(&draw, 1));
  return D3D_OK;
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
    submitDrawRunInternalFromState(
        drawState, std::span<const DrawParam>(&draw, 1),
        std::span<const DrawParamPayloadView>(&payload, 1));
    return D3D_OK;
  } else {
    const DrawParamPayloadView payload{
        .userVertexData = std::span<const u8>(upVertexScratch_.data(),
                                              upVertexScratch_.size()),
    };
    submitDrawRunInternalFromState(
        drawState, std::span<const DrawParam>(&draw, 1),
        std::span<const DrawParamPayloadView>(&payload, 1));
    return D3D_OK;
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
  submitDrawRunInternalFromState(
      drawState, std::span<const DrawParam>(&draw, 1),
      std::span<const DrawParamPayloadView>(&payload, 1));
  return D3D_OK;
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
