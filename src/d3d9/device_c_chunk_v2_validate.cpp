#include "device_c_chunk_v2_validate.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace dxmt9::d3d9 {

namespace {

constexpr std::uint32_t kNoIndex = 0xffffffffu;

bool checkedAdd(std::uint64_t a, std::uint64_t b, std::uint64_t& out) {
  if (a > std::numeric_limits<std::uint64_t>::max() - b) {
    return false;
  }
  out = a + b;
  return true;
}

bool checkedMul(std::uint64_t a, std::uint64_t b, std::uint64_t& out) {
  if (a != 0u && b > std::numeric_limits<std::uint64_t>::max() / a) {
    return false;
  }
  out = a * b;
  return true;
}

bool alignUp(std::uint64_t value, std::uint32_t alignment,
             std::uint64_t& out) {
  if (alignment == 0u || (alignment & (alignment - 1u)) != 0u) {
    return false;
  }
  const auto mask = static_cast<std::uint64_t>(alignment - 1u);
  if (value > std::numeric_limits<std::uint64_t>::max() - mask) {
    return false;
  }
  out = (value + mask) & ~mask;
  return true;
}

bool rangeValid(std::size_t size, std::uint64_t offset,
                std::uint64_t bytes) {
  return offset <= size && bytes <= size - offset;
}

bool pointerAligned(const std::byte* base, std::uint64_t offset,
                    std::size_t alignment) {
  if (!base) {
    return false;
  }
  return (reinterpret_cast<std::uintptr_t>(base) + offset) % alignment == 0u;
}

bool zeroBytes(std::span<const std::byte> bytes, std::uint64_t begin,
               std::uint64_t end) {
  if (begin > end || !rangeValid(bytes.size(), begin, end - begin)) {
    return false;
  }
  return std::all_of(bytes.begin() + static_cast<std::ptrdiff_t>(begin),
                     bytes.begin() + static_cast<std::ptrdiff_t>(end),
                     [](std::byte value) { return value == std::byte{0}; });
}

template <typename T>
bool load(std::span<const std::byte> bytes, std::uint64_t offset, T& out) {
  if (!rangeValid(bytes.size(), offset, sizeof(T))) {
    return false;
  }
  std::memcpy(&out, bytes.data() + offset, sizeof(T));
  return true;
}

V2ValidationResult failure(V2ValidationStatus status,
                           std::uint32_t record = kNoIndex,
                           std::uint32_t section = kNoIndex,
                           std::uint32_t handle = kNoIndex,
                           std::uint32_t byteOffset = 0u) {
  return V2ValidationResult{
      .status = status,
      .failedRecordIndex = record,
      .failedSectionIndex = section,
      .failedHandleIndex = handle,
      .byteOffset = byteOffset,
  };
}

bool isBoolean(std::uint32_t value) {
  return value <= 1u;
}

std::uint32_t constantElementSize(std::uint32_t type) {
  switch (type) {
    case D9C_COMMAND_RECORD_SET_VS_CONST_F:
    case D9C_COMMAND_RECORD_SET_VS_CONST_I:
    case D9C_COMMAND_RECORD_SET_PS_CONST_F:
    case D9C_COMMAND_RECORD_SET_PS_CONST_I:
      return 16u;
    case D9C_COMMAND_RECORD_SET_VS_CONST_B:
    case D9C_COMMAND_RECORD_SET_PS_CONST_B:
      return 4u;
    default:
      return 0u;
  }
}

std::uint32_t constantRegisterLimit(std::uint32_t type) {
  switch (type) {
    case D9C_COMMAND_RECORD_SET_VS_CONST_F:
      return D9C_DRAW_PACKET_MAX_CONST_VS_F;
    case D9C_COMMAND_RECORD_SET_VS_CONST_I:
      return D9C_DRAW_PACKET_MAX_CONST_VS_I;
    case D9C_COMMAND_RECORD_SET_VS_CONST_B:
      return D9C_DRAW_PACKET_MAX_CONST_VS_B;
    case D9C_COMMAND_RECORD_SET_PS_CONST_F:
      return D9C_DRAW_PACKET_MAX_CONST_PS_F;
    case D9C_COMMAND_RECORD_SET_PS_CONST_I:
      return D9C_DRAW_PACKET_MAX_CONST_PS_I;
    case D9C_COMMAND_RECORD_SET_PS_CONST_B:
      return D9C_DRAW_PACKET_MAX_CONST_PS_B;
    default:
      return 0u;
  }
}

std::uint32_t constantSectionLimit(std::uint16_t kind) {
  switch (kind) {
    case D9C_COMMAND_CHUNK_V2_SECTION_VS_CONST_F:
      return D9C_DRAW_PACKET_MAX_CONST_VS_F;
    case D9C_COMMAND_CHUNK_V2_SECTION_VS_CONST_I:
      return D9C_DRAW_PACKET_MAX_CONST_VS_I;
    case D9C_COMMAND_CHUNK_V2_SECTION_VS_CONST_B:
      return D9C_DRAW_PACKET_MAX_CONST_VS_B;
    case D9C_COMMAND_CHUNK_V2_SECTION_PS_CONST_F:
      return D9C_DRAW_PACKET_MAX_CONST_PS_F;
    case D9C_COMMAND_CHUNK_V2_SECTION_PS_CONST_I:
      return D9C_DRAW_PACKET_MAX_CONST_PS_I;
    case D9C_COMMAND_CHUNK_V2_SECTION_PS_CONST_B:
      return D9C_DRAW_PACKET_MAX_CONST_PS_B;
    default:
      return 0u;
  }
}

bool primitiveElementCount(std::uint32_t type, std::uint32_t primitiveCount,
                           std::uint64_t& count) {
  switch (type) {
    case 1u:
      count = primitiveCount;
      return true;
    case 2u:
      return checkedMul(primitiveCount, 2u, count);
    case 3u:
      return checkedAdd(primitiveCount, 1u, count);
    case 4u:
      return checkedMul(primitiveCount, 3u, count);
    case 5u:
    case 6u:
      return checkedAdd(primitiveCount, 2u, count);
    default:
      return false;
  }
}

V2ValidationResult markHandleReference(
    std::uint32_t handleIndex, std::uint32_t expectedKind, bool nullable,
    std::uint32_t recordIndex,
    const D9CCommandChunkWireRecordHeaderV2& record,
    std::span<const D9CCommandChunkWireHandleEntryV2> handles,
    V2ValidationScratch& scratch, std::uint32_t byteOffset,
    std::uint32_t sectionIndex = kNoIndex) {
  if (handleIndex == D9C_COMMAND_CHUNK_V2_NULL_HANDLE_INDEX) {
    return nullable ? V2ValidationResult{.status = V2ValidationStatus::Valid}
                    : failure(V2ValidationStatus::InvalidHandleReference,
                              recordIndex, sectionIndex, kNoIndex, byteOffset);
  }
  const auto sliceEnd = static_cast<std::uint64_t>(record.firstHandle) +
                        record.handleCount;
  if (handleIndex < record.firstHandle || handleIndex >= sliceEnd ||
      handleIndex >= handles.size()) {
    return failure(V2ValidationStatus::InvalidHandleReference, recordIndex,
                   sectionIndex, handleIndex, byteOffset);
  }
  if (handles[handleIndex].kind != expectedKind) {
    return failure(V2ValidationStatus::InvalidHandleReference, recordIndex,
                   sectionIndex, handleIndex, byteOffset);
  }
  scratch.referencedHandles[handleIndex] = 1u;
  return V2ValidationResult{.status = V2ValidationStatus::Valid};
}

V2ValidationResult validateFixedRecord(
    std::span<const std::byte> payload, std::uint32_t recordIndex,
    const D9CCommandChunkWireRecordHeaderV2& record,
    std::span<const D9CCommandChunkWireHandleEntryV2> handles,
    V2ValidationScratch& scratch) {
  const auto type = record.type;
  const auto elementSize = constantElementSize(type);
  if (elementSize != 0u) {
    D9CCommandChunkWireSetConstV2 fixed{};
    if (!load(payload, 0u, fixed)) {
      return failure(V2ValidationStatus::InvalidPayloadSize, recordIndex);
    }
    const auto limit = constantRegisterLimit(type);
    const auto registerEnd = static_cast<std::uint64_t>(fixed.startRegister) +
                             fixed.registerCount;
    std::uint64_t dataBytes = 0u;
    std::uint64_t expected = 0u;
    if (registerEnd > limit ||
        !checkedMul(fixed.registerCount, elementSize, dataBytes) ||
        !checkedAdd(sizeof(fixed), dataBytes, expected) ||
        expected != payload.size()) {
      return failure(V2ValidationStatus::InvalidConstantRange, recordIndex);
    }
    return V2ValidationResult{.status = V2ValidationStatus::Valid};
  }

  switch (type) {
    case D9C_COMMAND_RECORD_CLEAR: {
      D9CCommandChunkWireClearV2 fixed{};
      if (!load(payload, 0u, fixed)) {
        return failure(V2ValidationStatus::InvalidPayloadSize, recordIndex);
      }
      std::uint64_t rectBytes = 0u;
      std::uint64_t expected = 0u;
      if (fixed.rectOffset != sizeof(fixed) ||
          !checkedMul(fixed.rectCount, sizeof(D9CRect), rectBytes) ||
          !checkedAdd(fixed.rectOffset, rectBytes, expected) ||
          expected != payload.size()) {
        return failure(V2ValidationStatus::InvalidPayloadSize, recordIndex);
      }
      break;
    }
    case D9C_COMMAND_RECORD_PRESENT: {
      D9CCommandChunkWirePresentV2 fixed{};
      if (payload.size() != sizeof(fixed) || !load(payload, 0u, fixed)) {
        return failure(V2ValidationStatus::InvalidPayloadSize, recordIndex);
      }
      if (!isBoolean(fixed.hasSrc) || !isBoolean(fixed.hasDst) ||
          fixed.reserved0 != 0u) {
        return failure(V2ValidationStatus::NonZeroReserved, recordIndex);
      }
      break;
    }
    case D9C_COMMAND_RECORD_STRETCH_RECT: {
      D9CCommandChunkWireStretchRectV2 fixed{};
      if (payload.size() != sizeof(fixed) || !load(payload, 0u, fixed)) {
        return failure(V2ValidationStatus::InvalidPayloadSize, recordIndex);
      }
      if (!isBoolean(fixed.hasSrcRect) || !isBoolean(fixed.hasDstRect) ||
          fixed.reserved0 != 0u) {
        return failure(V2ValidationStatus::NonZeroReserved, recordIndex);
      }
      break;
    }
    case D9C_COMMAND_RECORD_COLOR_FILL: {
      D9CCommandChunkWireColorFillV2 fixed{};
      if (payload.size() != sizeof(fixed) || !load(payload, 0u, fixed)) {
        return failure(V2ValidationStatus::InvalidPayloadSize, recordIndex);
      }
      if (!isBoolean(fixed.hasRect) || fixed.reserved0 != 0u) {
        return failure(V2ValidationStatus::NonZeroReserved, recordIndex);
      }
      break;
    }
    case D9C_COMMAND_RECORD_UPDATE_SURFACE: {
      D9CCommandChunkWireUpdateSurfaceV2 fixed{};
      if (payload.size() != sizeof(fixed) || !load(payload, 0u, fixed)) {
        return failure(V2ValidationStatus::InvalidPayloadSize, recordIndex);
      }
      if (!isBoolean(fixed.hasSrcRect) || !isBoolean(fixed.hasDstPoint)) {
        return failure(V2ValidationStatus::InvalidPayloadSize, recordIndex);
      }
      break;
    }
    default: {
      const auto* rule = v2RecordRule(type);
      if (!rule || payload.size() != rule->fixedPayloadSize) {
        return failure(V2ValidationStatus::InvalidPayloadSize, recordIndex);
      }
      break;
    }
  }

  for (const auto& field : kV2RecordHandleFieldRules) {
    if (field.recordType != type) {
      continue;
    }
    std::uint32_t index = D9C_COMMAND_CHUNK_V2_NULL_HANDLE_INDEX;
    if (!load(payload, field.payloadOffset, index)) {
      return failure(V2ValidationStatus::InvalidPayloadSize, recordIndex);
    }
    const auto result = markHandleReference(
        index, field.handleKind, field.nullable, recordIndex, record, handles,
        scratch, field.payloadOffset);
    if (!result.valid()) {
      return result;
    }
  }
  return V2ValidationResult{.status = V2ValidationStatus::Valid};
}

bool sectionSlotAt(std::span<const std::byte> bytes, std::uint32_t offset,
                   std::uint32_t& slot) {
  return load(bytes, offset, slot);
}

V2ValidationResult validateSectionElements(
    const D9CCommandChunkWireSectionDescV2& desc,
    std::span<const std::byte> bytes,
    std::uint32_t recordIndex, std::uint32_t sectionIndex,
    const D9CCommandChunkWireRecordHeaderV2& record,
    std::span<const D9CCommandChunkWireHandleEntryV2> handles,
    V2ValidationScratch& scratch) {
  const auto* handleRule = v2SectionHandleFieldRule(desc.kind);
  std::uint32_t previousSlot = 0u;
  bool havePreviousSlot = false;
  for (std::uint32_t i = 0u; i < desc.count; ++i) {
    const auto elementOffset = static_cast<std::uint64_t>(i) * desc.elementSize;
    if (handleRule) {
      std::uint32_t index = D9C_COMMAND_CHUNK_V2_NULL_HANDLE_INDEX;
      if (!load(bytes, elementOffset + handleRule->payloadOffset, index)) {
        return failure(V2ValidationStatus::InvalidSectionRange, recordIndex,
                       sectionIndex);
      }
      const auto result = markHandleReference(
          index, handleRule->handleKind, handleRule->nullable, recordIndex,
          record, handles, scratch,
          static_cast<std::uint32_t>(desc.payloadOffset + elementOffset +
                                     handleRule->payloadOffset),
          sectionIndex);
      if (!result.valid()) {
        return result;
      }
    }

    switch (desc.kind) {
      case D9C_COMMAND_CHUNK_V2_SECTION_TEXTURE: {
        D9CCommandChunkWireTextureBindingV2 value{};
        load(bytes, elementOffset, value);
        if (value.slot >= D9C_DRAW_PACKET_MAX_TEXTURES ||
            !isBoolean(value.valid) || value.reserved0 != 0u ||
            (!value.valid &&
             value.handleIndex != D9C_COMMAND_CHUNK_V2_NULL_HANDLE_INDEX)) {
          return failure(V2ValidationStatus::InvalidSectionSchema, recordIndex,
                         sectionIndex);
        }
        previousSlot = value.slot;
        break;
      }
      case D9C_COMMAND_CHUNK_V2_SECTION_STREAM: {
        D9CCommandChunkWireStreamBindingV2 value{};
        load(bytes, elementOffset, value);
        if (value.slot >= D9C_DRAW_PACKET_MAX_STREAMS ||
            !isBoolean(value.valid) || value.reserved0 != 0u ||
            (!value.valid &&
             value.handleIndex != D9C_COMMAND_CHUNK_V2_NULL_HANDLE_INDEX)) {
          return failure(V2ValidationStatus::InvalidSectionSchema, recordIndex,
                         sectionIndex);
        }
        previousSlot = value.slot;
        break;
      }
      case D9C_COMMAND_CHUNK_V2_SECTION_SHADER: {
        D9CCommandChunkWireShaderBindingV2 value{};
        load(bytes, elementOffset, value);
        if (value.stage > D9C_COMMAND_CHUNK_V2_SHADER_STAGE_PIXEL ||
            !isBoolean(value.valid) || value.reserved0 != 0u ||
            (!value.valid &&
             value.handleIndex != D9C_COMMAND_CHUNK_V2_NULL_HANDLE_INDEX)) {
          return failure(V2ValidationStatus::InvalidSectionSchema, recordIndex,
                         sectionIndex);
        }
        previousSlot = value.stage;
        break;
      }
      case D9C_COMMAND_CHUNK_V2_SECTION_VERTEX_INPUT: {
        D9CCommandChunkWireVertexInputV2 value{};
        load(bytes, elementOffset, value);
        if (!isBoolean(value.valid) ||
            value.kind > D9C_COMMAND_CHUNK_V2_VERTEX_INPUT_DECLARATION ||
            (!value.valid &&
             (value.value != 0u ||
              value.handleIndex !=
                  D9C_COMMAND_CHUNK_V2_NULL_HANDLE_INDEX)) ||
            (value.valid &&
             value.kind == D9C_COMMAND_CHUNK_V2_VERTEX_INPUT_FVF &&
             value.handleIndex !=
                 D9C_COMMAND_CHUNK_V2_NULL_HANDLE_INDEX)) {
          return failure(V2ValidationStatus::InvalidSectionSchema, recordIndex,
                         sectionIndex);
        }
        break;
      }
      case D9C_COMMAND_CHUNK_V2_SECTION_INDEX_BUFFER: {
        D9CCommandChunkWireIndexBindingV2 value{};
        load(bytes, elementOffset, value);
        if (!isBoolean(value.valid) ||
            (!value.valid &&
             value.handleIndex != D9C_COMMAND_CHUNK_V2_NULL_HANDLE_INDEX)) {
          return failure(V2ValidationStatus::InvalidSectionSchema, recordIndex,
                         sectionIndex);
        }
        break;
      }
      case D9C_COMMAND_CHUNK_V2_SECTION_RENDER_TARGET: {
        D9CCommandChunkWireRenderTargetBindingV2 value{};
        load(bytes, elementOffset, value);
        if (value.slot >= D9C_DRAW_PACKET_MAX_RENDER_TARGETS ||
            !isBoolean(value.valid) || value.reserved0 != 0u ||
            (!value.valid &&
             value.handleIndex != D9C_COMMAND_CHUNK_V2_NULL_HANDLE_INDEX)) {
          return failure(V2ValidationStatus::InvalidSectionSchema, recordIndex,
                         sectionIndex);
        }
        previousSlot = value.slot;
        break;
      }
      case D9C_COMMAND_CHUNK_V2_SECTION_DEPTH_STENCIL: {
        D9CCommandChunkWireDepthStencilBindingV2 value{};
        load(bytes, elementOffset, value);
        if (!isBoolean(value.valid) ||
            (!value.valid &&
             value.handleIndex != D9C_COMMAND_CHUNK_V2_NULL_HANDLE_INDEX)) {
          return failure(V2ValidationStatus::InvalidSectionSchema, recordIndex,
                         sectionIndex);
        }
        break;
      }
      case D9C_COMMAND_CHUNK_V2_SECTION_CLIP_PLANE: {
        D9CCommandChunkWireClipPlaneV2 value{};
        load(bytes, elementOffset, value);
        if (value.slot >= 6u || value.reserved0 != 0u) {
          return failure(V2ValidationStatus::InvalidSectionSchema, recordIndex,
                         sectionIndex);
        }
        previousSlot = value.slot;
        break;
      }
      case D9C_COMMAND_CHUNK_V2_SECTION_LIGHT: {
        D9CCommandChunkWireLightV2 value{};
        load(bytes, elementOffset, value);
        if (value.slot >= D9C_DRAW_PACKET_MAX_LIGHTS ||
            value.reserved0 != 0u) {
          return failure(V2ValidationStatus::InvalidSectionSchema, recordIndex,
                         sectionIndex);
        }
        previousSlot = value.slot;
        break;
      }
      case D9C_COMMAND_CHUNK_V2_SECTION_LIGHT_ENABLE: {
        D9CCommandChunkWireLightEnableV2 value{};
        load(bytes, elementOffset, value);
        if (value.slot >= D9C_DRAW_PACKET_MAX_LIGHTS ||
            !isBoolean(value.enabled)) {
          return failure(V2ValidationStatus::InvalidSectionSchema, recordIndex,
                         sectionIndex);
        }
        previousSlot = value.slot;
        break;
      }
      case D9C_COMMAND_CHUNK_V2_SECTION_TRANSFORM: {
        D9CDrawPacketTransform value{};
        load(bytes, elementOffset, value);
        if (value.reserved != 0u) {
          return failure(V2ValidationStatus::NonZeroReserved, recordIndex,
                         sectionIndex);
        }
        break;
      }
      default:
        break;
    }

    const bool slotOrderedSection =
        desc.kind == D9C_COMMAND_CHUNK_V2_SECTION_TEXTURE ||
        desc.kind == D9C_COMMAND_CHUNK_V2_SECTION_STREAM ||
        desc.kind == D9C_COMMAND_CHUNK_V2_SECTION_SHADER ||
        desc.kind == D9C_COMMAND_CHUNK_V2_SECTION_RENDER_TARGET ||
        desc.kind == D9C_COMMAND_CHUNK_V2_SECTION_CLIP_PLANE ||
        desc.kind == D9C_COMMAND_CHUNK_V2_SECTION_LIGHT ||
        desc.kind == D9C_COMMAND_CHUNK_V2_SECTION_LIGHT_ENABLE;
    if (slotOrderedSection) {
      if (havePreviousSlot && previousSlot <= [&] {
            std::uint32_t prior = 0u;
            sectionSlotAt(bytes,
                          static_cast<std::uint32_t>((i - 1u) *
                                                     desc.elementSize),
                          prior);
            return prior;
          }()) {
        return failure(V2ValidationStatus::InvalidSectionSchema, recordIndex,
                       sectionIndex);
      }
      havePreviousSlot = true;
    }
  }
  return V2ValidationResult{.status = V2ValidationStatus::Valid};
}

V2ValidationResult validateSparseRecord(
    std::span<const std::byte> payload, std::uint32_t recordIndex,
    const D9CCommandChunkWireRecordHeaderV2& record,
    std::span<const D9CCommandChunkWireHandleEntryV2> handles,
    V2ValidationScratch& scratch) {
  D9CCommandChunkWireDrawHeaderV2 draw{};
  if (!load(payload, 0u, draw) || draw.reserved0 != 0u ||
      (draw.flags & ~D9C_COMMAND_CHUNK_V2_DRAW_FLAG_FULL_SNAPSHOT) != 0u) {
    return failure(V2ValidationStatus::InvalidDrawHeader, recordIndex);
  }

  const bool isApply = record.type == D9C_COMMAND_RECORD_APPLY_STATE;
  if (isApply) {
    if (draw.primitiveType != 0u || draw.baseVertex != 0 ||
        draw.minVertex != 0u || draw.numVertices != 0u ||
        draw.startVertex != 0u || draw.startIndex != 0u ||
        draw.primitiveCount != 0u || draw.stride != 0u ||
        draw.indexFormat != 0u) {
      return failure(V2ValidationStatus::InvalidDrawHeader, recordIndex);
    }
  } else {
    std::uint64_t ignored = 0u;
    if (!primitiveElementCount(draw.primitiveType, draw.primitiveCount,
                               ignored)) {
      return failure(V2ValidationStatus::InvalidDrawHeader, recordIndex);
    }
  }

  switch (record.type) {
    case D9C_COMMAND_RECORD_DRAW_PRIMITIVE:
      if (draw.baseVertex != 0 || draw.minVertex != 0u ||
          draw.numVertices != 0u || draw.startIndex != 0u ||
          draw.stride != 0u || draw.indexFormat != 0u) {
        return failure(V2ValidationStatus::InvalidDrawHeader, recordIndex);
      }
      break;
    case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE:
      if (draw.startVertex != 0u || draw.stride != 0u ||
          draw.indexFormat != 0u) {
        return failure(V2ValidationStatus::InvalidDrawHeader, recordIndex);
      }
      break;
    case D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP:
      if (draw.baseVertex != 0 || draw.minVertex != 0u ||
          draw.numVertices != 0u || draw.startVertex != 0u ||
          draw.startIndex != 0u || draw.stride == 0u ||
          draw.indexFormat != 0u) {
        return failure(V2ValidationStatus::InvalidDrawHeader, recordIndex);
      }
      break;
    case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP:
      if (draw.baseVertex != 0 || draw.startVertex != 0u ||
          draw.startIndex != 0u || draw.numVertices == 0u ||
          draw.stride == 0u ||
          (draw.indexFormat != 101u && draw.indexFormat != 102u)) {
        return failure(V2ValidationStatus::InvalidDrawHeader, recordIndex);
      }
      break;
    default:
      break;
  }

  std::uint64_t tableBytes = 0u;
  std::uint64_t tableEnd = 0u;
  std::uint64_t expectedPayloadStart = 0u;
  if (draw.sectionTableOffset != sizeof(draw) ||
      !checkedMul(draw.sectionCount,
                  sizeof(D9CCommandChunkWireSectionDescV2), tableBytes) ||
      !checkedAdd(draw.sectionTableOffset, tableBytes, tableEnd) ||
      !alignUp(tableEnd, alignof(std::uint32_t), expectedPayloadStart) ||
      draw.sectionPayloadOffset != expectedPayloadStart ||
      !rangeValid(payload.size(), draw.sectionTableOffset, tableBytes) ||
      !rangeValid(payload.size(), draw.sectionPayloadOffset, 0u) ||
      (draw.sectionCount != 0u &&
       !pointerAligned(payload.data(), draw.sectionTableOffset,
                       alignof(D9CCommandChunkWireSectionDescV2)))) {
    return failure(V2ValidationStatus::InvalidSectionTable, recordIndex);
  }
  if (!zeroBytes(payload, tableEnd, draw.sectionPayloadOffset)) {
    return failure(V2ValidationStatus::NonZeroPadding, recordIndex, kNoIndex,
                   kNoIndex, static_cast<std::uint32_t>(tableEnd));
  }

  const auto sections =
      draw.sectionCount == 0u
          ? std::span<const D9CCommandChunkWireSectionDescV2>{}
          : std::span<const D9CCommandChunkWireSectionDescV2>{
                reinterpret_cast<const D9CCommandChunkWireSectionDescV2*>(
                    payload.data() + draw.sectionTableOffset),
                draw.sectionCount};
  std::uint64_t expectedSectionOffset = draw.sectionPayloadOffset;
  std::uint16_t previousKind = 0u;
  bool sawTexture = false;
  bool sawStream = false;
  bool sawUpIndex = false;
  bool sawUpVertex = false;
  std::uint32_t upIndexBytes = 0u;
  std::uint32_t upVertexBytes = 0u;

  for (std::uint32_t i = 0u; i < sections.size(); ++i) {
    const auto& desc = sections[i];
    const auto* rule = v2SectionRule(desc.kind);
    if (!rule || desc.kind <= previousKind) {
      return failure(V2ValidationStatus::InvalidSectionOrder, recordIndex, i);
    }
    previousKind = desc.kind;
    if (desc.elementSize != rule->elementSize || desc.count == 0u ||
        desc.count > rule->maxCount ||
        ((rule->ruleFlags & V2SectionRuleSingle) != 0u && desc.count != 1u)) {
      return failure(V2ValidationStatus::InvalidSectionSchema, recordIndex, i);
    }

    std::uint64_t expectedBytes = 0u;
    if (!checkedMul(desc.count, desc.elementSize, expectedBytes)) {
      return failure(V2ValidationStatus::InvalidSectionRange, recordIndex, i);
    }
    if ((rule->ruleFlags & V2SectionRuleConstantRange) != 0u &&
        !checkedAdd(expectedBytes,
                    sizeof(D9CCommandChunkWireConstantRangeV2),
                    expectedBytes)) {
      return failure(V2ValidationStatus::InvalidSectionRange, recordIndex, i);
    }
    if (desc.byteSize != expectedBytes) {
      return failure(V2ValidationStatus::InvalidSectionSchema, recordIndex, i);
    }

    std::uint64_t alignedOffset = 0u;
    std::uint64_t sectionEnd = 0u;
    if (!alignUp(expectedSectionOffset, rule->payloadAlignment, alignedOffset) ||
        desc.payloadOffset != alignedOffset ||
        !rangeValid(payload.size(), desc.payloadOffset, desc.byteSize) ||
        !pointerAligned(payload.data(), desc.payloadOffset,
                        rule->payloadAlignment) ||
        !checkedAdd(desc.payloadOffset, desc.byteSize, sectionEnd)) {
      return failure(V2ValidationStatus::InvalidSectionRange, recordIndex, i);
    }
    if (!zeroBytes(payload, expectedSectionOffset, alignedOffset)) {
      return failure(V2ValidationStatus::NonZeroPadding, recordIndex, i,
                     kNoIndex,
                     static_cast<std::uint32_t>(expectedSectionOffset));
    }

    const auto bytes = payload.subspan(desc.payloadOffset, desc.byteSize);
    if ((rule->ruleFlags & V2SectionRuleConstantRange) != 0u) {
      D9CCommandChunkWireConstantRangeV2 range{};
      const auto limit = constantSectionLimit(desc.kind);
      if (!load(bytes, 0u, range) || range.registerCount != desc.count ||
          static_cast<std::uint64_t>(range.startRegister) +
                  range.registerCount >
              limit) {
        return failure(V2ValidationStatus::InvalidConstantRange, recordIndex,
                       i);
      }
    } else {
      const auto elementResult = validateSectionElements(
          desc, bytes, recordIndex, i, record, handles, scratch);
      if (!elementResult.valid()) {
        return elementResult;
      }
    }

    if (desc.kind == D9C_COMMAND_CHUNK_V2_SECTION_TEXTURE) {
      sawTexture = true;
      if ((draw.flags & D9C_COMMAND_CHUNK_V2_DRAW_FLAG_FULL_SNAPSHOT) != 0u) {
        if (desc.count != D9C_DRAW_PACKET_MAX_TEXTURES) {
          return failure(V2ValidationStatus::InvalidFullSnapshot, recordIndex,
                         i);
        }
        for (std::uint32_t slot = 0u; slot < desc.count; ++slot) {
          D9CCommandChunkWireTextureBindingV2 value{};
          load(bytes, static_cast<std::uint64_t>(slot) * desc.elementSize,
               value);
          if (value.slot != slot || value.valid != 1u) {
            return failure(V2ValidationStatus::InvalidFullSnapshot,
                           recordIndex, i);
          }
        }
      }
    } else if (desc.kind == D9C_COMMAND_CHUNK_V2_SECTION_STREAM) {
      sawStream = true;
      if ((draw.flags & D9C_COMMAND_CHUNK_V2_DRAW_FLAG_FULL_SNAPSHOT) != 0u) {
        if (desc.count != D9C_DRAW_PACKET_MAX_STREAMS) {
          return failure(V2ValidationStatus::InvalidFullSnapshot, recordIndex,
                         i);
        }
        for (std::uint32_t slot = 0u; slot < desc.count; ++slot) {
          D9CCommandChunkWireStreamBindingV2 value{};
          load(bytes, static_cast<std::uint64_t>(slot) * desc.elementSize,
               value);
          if (value.slot != slot || value.valid != 1u) {
            return failure(V2ValidationStatus::InvalidFullSnapshot,
                           recordIndex, i);
          }
        }
      }
    } else if (desc.kind == D9C_COMMAND_CHUNK_V2_SECTION_UP_INDEX_DATA) {
      sawUpIndex = true;
      upIndexBytes = desc.byteSize;
    } else if (desc.kind == D9C_COMMAND_CHUNK_V2_SECTION_UP_VERTEX_DATA) {
      sawUpVertex = true;
      upVertexBytes = desc.byteSize;
    }
    expectedSectionOffset = sectionEnd;
  }

  if (expectedSectionOffset != payload.size()) {
    return failure(V2ValidationStatus::NonCanonicalPayloadLayout, recordIndex);
  }
  if ((draw.flags & D9C_COMMAND_CHUNK_V2_DRAW_FLAG_FULL_SNAPSHOT) != 0u &&
      (!sawTexture || !sawStream)) {
    return failure(V2ValidationStatus::InvalidFullSnapshot, recordIndex);
  }

  std::uint64_t primitiveElements = 0u;
  std::uint64_t expectedBytes = 0u;
  if (record.type == D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP) {
    if (sawUpIndex || !sawUpVertex ||
        !primitiveElementCount(draw.primitiveType, draw.primitiveCount,
                               primitiveElements) ||
        !checkedMul(primitiveElements, draw.stride, expectedBytes) ||
        expectedBytes != upVertexBytes) {
      return failure(V2ValidationStatus::InvalidUpData, recordIndex);
    }
  } else if (record.type == D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP) {
    const auto indexSize = draw.indexFormat == 102u ? 4u : 2u;
    if (!sawUpIndex || !sawUpVertex ||
        !primitiveElementCount(draw.primitiveType, draw.primitiveCount,
                               primitiveElements) ||
        !checkedMul(primitiveElements, indexSize, expectedBytes) ||
        expectedBytes != upIndexBytes ||
        !checkedMul(draw.numVertices, draw.stride, expectedBytes) ||
        expectedBytes != upVertexBytes) {
      return failure(V2ValidationStatus::InvalidUpData, recordIndex);
    }
  } else if (sawUpIndex || sawUpVertex) {
    return failure(V2ValidationStatus::InvalidUpData, recordIndex);
  }

  return V2ValidationResult{.status = V2ValidationStatus::Valid};
}

}  // namespace

ImportedSectionV2View ImportedRecordV2View::section(
    std::size_t index) const noexcept {
  if (index >= sections.size()) {
    return {};
  }
  const auto& descriptor = sections[index];
  if (!rangeValid(payload.size(), descriptor.payloadOffset,
                  descriptor.byteSize)) {
    return {};
  }
  return ImportedSectionV2View{
      .descriptor = descriptor,
      .payload = payload.subspan(descriptor.payloadOffset,
                                 descriptor.byteSize),
  };
}

ImportedRecordV2View ImportedChunkV2View::record(
    std::size_t index) const noexcept {
  if (index >= records.size()) {
    return {};
  }
  const auto header = records[index];
  if (!rangeValid(payloadArena.size(), header.payloadOffset,
                  header.payloadSize) ||
      header.firstHandle > handles.size() ||
      header.handleCount > handles.size() - header.firstHandle) {
    return {};
  }
  ImportedRecordV2View view{
      .header = header,
      .payload = payloadArena.subspan(header.payloadOffset,
                                      header.payloadSize),
      .handles = handles.subspan(header.firstHandle, header.handleCount),
  };
  const auto* rule = v2RecordRule(header.type);
  if (!rule || (rule->ruleFlags & V2RecordRuleSparseState) == 0u ||
      view.payload.size() < sizeof(view.drawHeader)) {
    return view;
  }
  std::memcpy(&view.drawHeader, view.payload.data(), sizeof(view.drawHeader));
  std::uint64_t tableBytes = 0u;
  if (!checkedMul(view.drawHeader.sectionCount,
                  sizeof(D9CCommandChunkWireSectionDescV2), tableBytes) ||
      !rangeValid(view.payload.size(), view.drawHeader.sectionTableOffset,
                  tableBytes) ||
      (view.drawHeader.sectionCount != 0u &&
       !pointerAligned(view.payload.data(),
                       view.drawHeader.sectionTableOffset,
                       alignof(D9CCommandChunkWireSectionDescV2)))) {
    return view;
  }
  view.sections =
      view.drawHeader.sectionCount == 0u
          ? std::span<const D9CCommandChunkWireSectionDescV2>{}
          : std::span<const D9CCommandChunkWireSectionDescV2>{
                reinterpret_cast<const D9CCommandChunkWireSectionDescV2*>(
                    view.payload.data() + view.drawHeader.sectionTableOffset),
                view.drawHeader.sectionCount};
  return view;
}

V2ValidationResult validateCommandChunkV2(
    std::span<const std::byte> blob, const V2ChunkEnvelope& envelope,
    ImportedChunkV2View* out, V2ValidationScratch& scratch) noexcept {
  if (out) {
    *out = {};
  }
  D9CCommandChunkWireHeaderV2 header{};
  if (!load(blob, 0u, header)) {
    return failure(V2ValidationStatus::MissingHeader);
  }
  if (envelope.version != D9C_COMMAND_CHUNK_VERSION_V2 ||
      header.version != envelope.version) {
    return failure(V2ValidationStatus::OuterVersionMismatch);
  }
  if (header.headerSize != D9C_COMMAND_CHUNK_WIRE_HEADER_V2_SIZE ||
      header.recordHeaderSize !=
          D9C_COMMAND_CHUNK_WIRE_RECORD_HEADER_V2_SIZE ||
      header.handleEntrySize !=
          D9C_COMMAND_CHUNK_WIRE_HANDLE_ENTRY_V2_SIZE) {
    return failure(V2ValidationStatus::InvalidHeader);
  }
  if (header.reserved0 != 0u || header.reserved1 != 0u) {
    return failure(V2ValidationStatus::NonZeroReserved);
  }
  if (header.recordCount != envelope.recordCount ||
      header.handleCount != envelope.handleCount) {
    return failure(V2ValidationStatus::OuterCountMismatch);
  }
  if (!pointerAligned(blob.data(), 0u,
                      alignof(D9CCommandChunkWireHeaderV2))) {
    return failure(V2ValidationStatus::InvalidAlignment);
  }

  std::uint64_t recordBytes = 0u;
  std::uint64_t handleBytes = 0u;
  std::uint64_t recordEnd = 0u;
  std::uint64_t expectedHandleOffset = 0u;
  std::uint64_t handleEnd = 0u;
  std::uint64_t expectedPayloadOffset = 0u;
  std::uint64_t blobEnd = 0u;
  if (!checkedMul(header.recordCount, header.recordHeaderSize, recordBytes) ||
      !checkedMul(header.handleCount, header.handleEntrySize, handleBytes) ||
      !checkedAdd(header.recordTableOffset, recordBytes, recordEnd) ||
      !alignUp(recordEnd, alignof(D9CCommandChunkWireHandleEntryV2),
               expectedHandleOffset) ||
      !checkedAdd(header.handleTableOffset, handleBytes, handleEnd) ||
      !alignUp(handleEnd, alignof(std::uint32_t), expectedPayloadOffset) ||
      !checkedAdd(header.payloadArenaOffset, header.payloadArenaSize, blobEnd) ||
      header.recordTableOffset != header.headerSize ||
      header.handleTableOffset != expectedHandleOffset ||
      header.payloadArenaOffset != expectedPayloadOffset ||
      blobEnd != blob.size() ||
      !rangeValid(blob.size(), header.recordTableOffset, recordBytes) ||
      !rangeValid(blob.size(), header.handleTableOffset, handleBytes) ||
      !rangeValid(blob.size(), header.payloadArenaOffset,
                  header.payloadArenaSize)) {
    return failure(V2ValidationStatus::NonCanonicalChunkLayout);
  }
  if (!zeroBytes(blob, recordEnd, header.handleTableOffset) ||
      !zeroBytes(blob, handleEnd, header.payloadArenaOffset)) {
    return failure(V2ValidationStatus::NonZeroPadding);
  }
  if ((header.recordCount != 0u &&
       !pointerAligned(blob.data(), header.recordTableOffset,
                       alignof(D9CCommandChunkWireRecordHeaderV2))) ||
      (header.handleCount != 0u &&
       !pointerAligned(blob.data(), header.handleTableOffset,
                       alignof(D9CCommandChunkWireHandleEntryV2))) ||
      (header.payloadArenaSize != 0u &&
       !pointerAligned(blob.data(), header.payloadArenaOffset,
                       alignof(std::uint32_t)))) {
    return failure(V2ValidationStatus::InvalidAlignment);
  }

  ImportedChunkV2View candidate{
      .header = header,
      .records =
          header.recordCount == 0u
              ? std::span<const D9CCommandChunkWireRecordHeaderV2>{}
              : std::span<const D9CCommandChunkWireRecordHeaderV2>{
                    reinterpret_cast<
                        const D9CCommandChunkWireRecordHeaderV2*>(
                        blob.data() + header.recordTableOffset),
                    header.recordCount},
      .handles =
          header.handleCount == 0u
              ? std::span<const D9CCommandChunkWireHandleEntryV2>{}
              : std::span<const D9CCommandChunkWireHandleEntryV2>{
                    reinterpret_cast<
                        const D9CCommandChunkWireHandleEntryV2*>(
                        blob.data() + header.handleTableOffset),
                    header.handleCount},
      .payloadArena =
          blob.subspan(header.payloadArenaOffset, header.payloadArenaSize),
  };

  try {
    scratch.referencedHandles.resize(header.handleCount);
  } catch (...) {
    return failure(V2ValidationStatus::ScratchAllocationFailed);
  }
  std::fill(scratch.referencedHandles.begin(),
            scratch.referencedHandles.end(), 0u);

  for (std::uint32_t i = 0u; i < candidate.handles.size(); ++i) {
    const auto& handle = candidate.handles[i];
    if (handle.kind > D9C_CHUNK_HANDLE_KIND_QUERY ||
        handle.generation == 0u || handle.objectId == 0u) {
      return failure(V2ValidationStatus::InvalidHandleEntry, kNoIndex,
                     kNoIndex, i);
    }
  }

  std::uint64_t expectedFirstHandle = 0u;
  std::uint64_t expectedPayloadEnd = 0u;
  for (std::uint32_t i = 0u; i < candidate.records.size(); ++i) {
    const auto& record = candidate.records[i];
    const auto* rule = v2RecordRule(record.type);
    if (!rule) {
      return failure(V2ValidationStatus::InvalidRecordType, i);
    }
    if ((record.flags & ~rule->allowedRecordFlags) != 0u) {
      return failure(V2ValidationStatus::InvalidRecordFlags, i);
    }
    if (record.reserved0 != 0u || record.reserved1 != 0u) {
      return failure(V2ValidationStatus::NonZeroReserved, i);
    }
    const auto handleEndForRecord =
        static_cast<std::uint64_t>(record.firstHandle) + record.handleCount;
    if (record.firstHandle != expectedFirstHandle ||
        handleEndForRecord > candidate.handles.size()) {
      return failure(V2ValidationStatus::NonCanonicalHandleSlice, i);
    }
    expectedFirstHandle = handleEndForRecord;
    for (std::uint32_t a = record.firstHandle;
         a < handleEndForRecord; ++a) {
      for (std::uint32_t b = a + 1u; b < handleEndForRecord; ++b) {
        const auto& left = candidate.handles[a];
        const auto& right = candidate.handles[b];
        if (left.kind == right.kind && left.generation == right.generation &&
            left.objectId == right.objectId) {
          return failure(V2ValidationStatus::InvalidHandleEntry, i, kNoIndex,
                         b);
        }
      }
    }

    std::uint64_t alignedPayloadOffset = 0u;
    std::uint64_t payloadEndForRecord = 0u;
    if (!alignUp(expectedPayloadEnd, rule->payloadAlignment,
                 alignedPayloadOffset) ||
        record.payloadOffset != alignedPayloadOffset ||
        record.payloadSize < rule->fixedPayloadSize ||
        !rangeValid(candidate.payloadArena.size(), record.payloadOffset,
                    record.payloadSize) ||
        !checkedAdd(record.payloadOffset, record.payloadSize,
                    payloadEndForRecord) ||
        !pointerAligned(candidate.payloadArena.data(), record.payloadOffset,
                        rule->payloadAlignment)) {
      return failure(V2ValidationStatus::NonCanonicalPayloadLayout, i);
    }
    if (!zeroBytes(candidate.payloadArena, expectedPayloadEnd,
                   alignedPayloadOffset)) {
      return failure(V2ValidationStatus::NonZeroPadding, i, kNoIndex,
                     kNoIndex,
                     static_cast<std::uint32_t>(expectedPayloadEnd));
    }
    expectedPayloadEnd = payloadEndForRecord;
    const auto payload = candidate.payloadArena.subspan(record.payloadOffset,
                                                        record.payloadSize);
    V2ValidationResult recordResult{};
    if ((rule->ruleFlags & V2RecordRuleSparseState) != 0u) {
      recordResult = validateSparseRecord(payload, i, record, candidate.handles,
                                          scratch);
    } else {
      recordResult = validateFixedRecord(payload, i, record, candidate.handles,
                                         scratch);
    }
    if (!recordResult.valid()) {
      return recordResult;
    }
    for (std::uint32_t handle = record.firstHandle;
         handle < handleEndForRecord; ++handle) {
      if (scratch.referencedHandles[handle] == 0u) {
        return failure(V2ValidationStatus::HandleSliceMismatch, i, kNoIndex,
                       handle);
      }
    }
  }

  if (expectedFirstHandle != candidate.handles.size()) {
    return failure(V2ValidationStatus::NonCanonicalHandleSlice);
  }
  if (expectedPayloadEnd != candidate.payloadArena.size()) {
    return failure(V2ValidationStatus::NonCanonicalPayloadLayout);
  }
  if (out) {
    *out = candidate;
  }
  return V2ValidationResult{.status = V2ValidationStatus::Valid};
}

V2ValidationResult validateCommandChunkV2(
    std::span<const std::byte> blob, const V2ChunkEnvelope& envelope,
    ImportedChunkV2View* out) noexcept {
  thread_local V2ValidationScratch scratch;
  return validateCommandChunkV2(blob, envelope, out, scratch);
}

bool importPrevalidatedCommandChunkV2(
    std::span<const std::byte> blob, const V2ChunkEnvelope& envelope,
    ImportedChunkV2View& out) noexcept {
  out = {};
  D9CCommandChunkWireHeaderV2 header{};
  if (!load(blob, 0u, header) ||
      envelope.version != D9C_COMMAND_CHUNK_VERSION_V2 ||
      header.version != envelope.version ||
      header.recordCount != envelope.recordCount ||
      header.handleCount != envelope.handleCount ||
      header.recordHeaderSize != sizeof(D9CCommandChunkWireRecordHeaderV2) ||
      header.handleEntrySize != sizeof(D9CCommandChunkWireHandleEntryV2)) {
    return false;
  }

  std::uint64_t recordBytes = 0u;
  std::uint64_t handleBytes = 0u;
  if (!checkedMul(header.recordCount, header.recordHeaderSize, recordBytes) ||
      !checkedMul(header.handleCount, header.handleEntrySize, handleBytes) ||
      !rangeValid(blob.size(), header.recordTableOffset, recordBytes) ||
      !rangeValid(blob.size(), header.handleTableOffset, handleBytes) ||
      !rangeValid(blob.size(), header.payloadArenaOffset,
                  header.payloadArenaSize) ||
      (header.recordCount != 0u &&
       !pointerAligned(blob.data(), header.recordTableOffset,
                       alignof(D9CCommandChunkWireRecordHeaderV2))) ||
      (header.handleCount != 0u &&
       !pointerAligned(blob.data(), header.handleTableOffset,
                       alignof(D9CCommandChunkWireHandleEntryV2)))) {
    return false;
  }

  out = ImportedChunkV2View{
      .header = header,
      .records =
          header.recordCount == 0u
              ? std::span<const D9CCommandChunkWireRecordHeaderV2>{}
              : std::span<const D9CCommandChunkWireRecordHeaderV2>{
                    reinterpret_cast<
                        const D9CCommandChunkWireRecordHeaderV2*>(
                        blob.data() + header.recordTableOffset),
                    header.recordCount},
      .handles =
          header.handleCount == 0u
              ? std::span<const D9CCommandChunkWireHandleEntryV2>{}
              : std::span<const D9CCommandChunkWireHandleEntryV2>{
                    reinterpret_cast<
                        const D9CCommandChunkWireHandleEntryV2*>(
                        blob.data() + header.handleTableOffset),
                    header.handleCount},
      .payloadArena =
          blob.subspan(header.payloadArenaOffset, header.payloadArenaSize),
  };
  return true;
}

}  // namespace dxmt9::d3d9
