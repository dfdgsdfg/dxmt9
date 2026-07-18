#include "d3d9_pe_chunk_v2_builder.hpp"

#include <array>
#include <cstring>
#include <limits>

namespace dxmt9::d3d9::pe {

namespace {

template <typename T>
std::span<const std::byte> asBytes(std::span<const T> values) {
  return std::as_bytes(values);
}

bool validSectionCount(std::uint16_t kind, std::size_t count) {
  const auto* rule = v2SectionRule(kind);
  return rule && count != 0u && count <= rule->maxCount &&
         count <= std::numeric_limits<std::uint32_t>::max();
}

bool validConstant(std::uint16_t kind,
                   const SparseConstantRangeV2Input& input) {
  const auto* rule = v2SectionRule(kind);
  if (!input.present()) {
    return input.startRegister == 0u && input.registerBytes.empty();
  }
  const auto end = static_cast<std::uint64_t>(input.startRegister) +
                   input.registerCount;
  const auto byteSize = static_cast<std::uint64_t>(input.registerCount) *
                        (rule ? rule->elementSize : 0u);
  return rule && (rule->ruleFlags & V2SectionRuleConstantRange) != 0u &&
         input.registerCount <= rule->maxCount && end <= rule->maxCount &&
         byteSize == input.registerBytes.size();
}

template <typename T>
bool appendPlainSection(
    CommandChunkV2Builder& builder, std::uint16_t kind,
    std::span<const T> values,
    std::array<D9CCommandChunkWireSectionDescV2,
               D9C_COMMAND_CHUNK_V2_SECTION_COUNT>& descs,
    std::uint32_t& sectionIndex) {
  if (values.empty()) {
    return true;
  }
  const auto* rule = v2SectionRule(kind);
  std::uint32_t offset = 0u;
  if (!rule || !validSectionCount(kind, values.size()) ||
      !builder.appendPayload(asBytes(values), rule->payloadAlignment,
                             &offset)) {
    return false;
  }
  descs[sectionIndex++] = D9CCommandChunkWireSectionDescV2{
      .kind = kind,
      .elementSize = rule->elementSize,
      .count = static_cast<std::uint32_t>(values.size()),
      .payloadOffset = offset,
      .byteSize = static_cast<std::uint32_t>(values.size_bytes()),
  };
  return true;
}

bool appendConstantSection(
    CommandChunkV2Builder& builder, std::uint16_t kind,
    const SparseConstantRangeV2Input& input,
    std::array<D9CCommandChunkWireSectionDescV2,
               D9C_COMMAND_CHUNK_V2_SECTION_COUNT>& descs,
    std::uint32_t& sectionIndex) {
  if (!input.present()) {
    return true;
  }
  const auto* rule = v2SectionRule(kind);
  const D9CCommandChunkWireConstantRangeV2 range{
      .startRegister = input.startRegister,
      .registerCount = input.registerCount,
  };
  std::uint32_t offset = 0u;
  if (!rule || !validConstant(kind, input) ||
      !builder.appendPayloadValue(range, &offset) ||
      !builder.appendPayload(input.registerBytes)) {
    return false;
  }
  descs[sectionIndex++] = D9CCommandChunkWireSectionDescV2{
      .kind = kind,
      .elementSize = rule->elementSize,
      .count = input.registerCount,
      .payloadOffset = offset,
      .byteSize = static_cast<std::uint32_t>(
          sizeof(range) + input.registerBytes.size()),
  };
  return true;
}

bool appendRawSection(
    CommandChunkV2Builder& builder, std::uint16_t kind,
    std::span<const std::byte> bytes,
    std::array<D9CCommandChunkWireSectionDescV2,
               D9C_COMMAND_CHUNK_V2_SECTION_COUNT>& descs,
    std::uint32_t& sectionIndex) {
  if (bytes.empty()) {
    return true;
  }
  const auto* rule = v2SectionRule(kind);
  std::uint32_t offset = 0u;
  if (!rule || !validSectionCount(kind, bytes.size()) ||
      !builder.appendPayload(bytes, rule->payloadAlignment, &offset)) {
    return false;
  }
  descs[sectionIndex++] = D9CCommandChunkWireSectionDescV2{
      .kind = kind,
      .elementSize = rule->elementSize,
      .count = static_cast<std::uint32_t>(bytes.size()),
      .payloadOffset = offset,
      .byteSize = static_cast<std::uint32_t>(bytes.size()),
  };
  return true;
}

bool appendNullableHandle(CommandChunkV2Builder& builder,
                          const PeWireObjectRef& object,
                          std::uint32_t expectedKind,
                          std::uint32_t& index) {
  index = D9C_COMMAND_CHUNK_V2_NULL_HANDLE_INDEX;
  if (!object.object) {
    return true;
  }
  return builder.appendHandle(object, expectedKind, index);
}

template <typename Wire, std::size_t Capacity, typename Prepare>
bool appendBindingSection(
    CommandChunkV2Builder& builder, std::uint16_t kind,
    std::span<const SparseBindingV2Input<Wire>> inputs, Prepare&& prepare,
    std::array<D9CCommandChunkWireSectionDescV2,
               D9C_COMMAND_CHUNK_V2_SECTION_COUNT>& descs,
    std::uint32_t& sectionIndex) {
  if (inputs.empty()) {
    return true;
  }
  if (!validSectionCount(kind, inputs.size()) || inputs.size() > Capacity) {
    return false;
  }
  std::array<Wire, Capacity> values{};
  for (std::size_t i = 0u; i < inputs.size(); ++i) {
    values[i] = inputs[i].wire;
    if (!prepare(values[i], inputs[i].object)) {
      return false;
    }
  }
  return appendPlainSection(
      builder, kind,
      std::span<const Wire>(values.data(), inputs.size()), descs,
      sectionIndex);
}

std::uint32_t sectionCount(const SparseStateV2Input& state) {
  std::uint32_t count = 0u;
  const auto add = [&count](bool present) {
    count += present ? 1u : 0u;
  };
  add(!state.renderStates.empty());
  add(!state.textures.empty());
  add(!state.streams.empty());
  add(!state.shaders.empty());
  add(!state.vertexInputs.empty());
  add(!state.indexBuffers.empty());
  add(!state.renderTargets.empty());
  add(!state.depthStencils.empty());
  add(!state.viewports.empty());
  add(!state.scissors.empty());
  add(!state.materials.empty());
  add(!state.clipPlanes.empty());
  add(!state.textureStageStates.empty());
  add(!state.samplerStates.empty());
  add(!state.transforms.empty());
  add(!state.lights.empty());
  add(!state.lightEnables.empty());
  add(state.vsFloatConstants.present());
  add(state.vsIntConstants.present());
  add(state.vsBoolConstants.present());
  add(state.psFloatConstants.present());
  add(state.psIntConstants.present());
  add(state.psBoolConstants.present());
  add(!state.upIndexData.empty());
  add(!state.upVertexData.empty());
  return count;
}

bool orderedSlot(std::uint32_t slot, bool& havePrevious,
                 std::uint32_t& previous) {
  if (havePrevious && slot <= previous) {
    return false;
  }
  havePrevious = true;
  previous = slot;
  return true;
}

}  // namespace

bool appendSparseRecordV2(CommandChunkV2Builder& builder,
                          std::uint32_t type,
                          D9CCommandChunkWireDrawHeaderV2 draw,
                          const SparseStateV2Input& state) noexcept {
  const auto* recordRule = v2RecordRule(type);
  const auto count = sectionCount(state);
  if (!recordRule ||
      (recordRule->ruleFlags & V2RecordRuleSparseState) == 0u ||
      count > D9C_COMMAND_CHUNK_V2_SECTION_COUNT ||
      !validConstant(D9C_COMMAND_CHUNK_V2_SECTION_VS_CONST_F,
                     state.vsFloatConstants) ||
      !validConstant(D9C_COMMAND_CHUNK_V2_SECTION_VS_CONST_I,
                     state.vsIntConstants) ||
      !validConstant(D9C_COMMAND_CHUNK_V2_SECTION_VS_CONST_B,
                     state.vsBoolConstants) ||
      !validConstant(D9C_COMMAND_CHUNK_V2_SECTION_PS_CONST_F,
                     state.psFloatConstants) ||
      !validConstant(D9C_COMMAND_CHUNK_V2_SECTION_PS_CONST_I,
                     state.psIntConstants) ||
      !validConstant(D9C_COMMAND_CHUNK_V2_SECTION_PS_CONST_B,
                     state.psBoolConstants)) {
    return false;
  }

  draw.sectionCount = count;
  draw.sectionTableOffset = sizeof(draw);
  draw.sectionPayloadOffset =
      sizeof(draw) + count * sizeof(D9CCommandChunkWireSectionDescV2);
  draw.reserved0 = 0u;
  if (!builder.beginRecord(type) || !builder.appendPayloadValue(draw)) {
    return false;
  }

  std::array<D9CCommandChunkWireSectionDescV2,
             D9C_COMMAND_CHUNK_V2_SECTION_COUNT>
      descs{};
  const auto descBytes = std::span<const std::byte>(
      reinterpret_cast<const std::byte*>(descs.data()),
      count * sizeof(descs[0]));
  if (!builder.appendPayload(descBytes,
                             alignof(D9CCommandChunkWireSectionDescV2))) {
    return false;
  }

  std::uint32_t sectionIndex = 0u;
  bool havePrevious = false;
  std::uint32_t previous = 0u;
  const auto texturePrepare = [&](D9CCommandChunkWireTextureBindingV2& value,
                                  const PeWireObjectRef& object) {
    value.reserved0 = 0u;
    value.handleIndex = D9C_COMMAND_CHUNK_V2_NULL_HANDLE_INDEX;
    return value.valid <= 1u &&
           orderedSlot(value.slot, havePrevious, previous) &&
           (!value.valid || appendNullableHandle(
                                builder, object,
                                D9C_CHUNK_HANDLE_KIND_TEXTURE,
                                value.handleIndex));
  };
  if (!appendPlainSection(builder, D9C_COMMAND_CHUNK_V2_SECTION_RENDER_STATE,
                          state.renderStates, descs, sectionIndex) ||
      !appendBindingSection<
          D9CCommandChunkWireTextureBindingV2,
          D9C_DRAW_PACKET_MAX_TEXTURES>(
          builder, D9C_COMMAND_CHUNK_V2_SECTION_TEXTURE, state.textures,
          texturePrepare, descs, sectionIndex)) {
    builder.rollbackRecord();
    return false;
  }

  havePrevious = false;
  previous = 0u;
  const auto streamPrepare = [&](D9CCommandChunkWireStreamBindingV2& value,
                                 const PeWireObjectRef& object) {
    value.reserved0 = 0u;
    value.handleIndex = D9C_COMMAND_CHUNK_V2_NULL_HANDLE_INDEX;
    return value.valid <= 1u &&
           orderedSlot(value.slot, havePrevious, previous) &&
           (!value.valid || appendNullableHandle(
                                builder, object, D9C_CHUNK_HANDLE_KIND_BUFFER,
                                value.handleIndex));
  };
  if (!appendBindingSection<D9CCommandChunkWireStreamBindingV2,
                            D9C_DRAW_PACKET_MAX_STREAMS>(
          builder, D9C_COMMAND_CHUNK_V2_SECTION_STREAM, state.streams,
          streamPrepare, descs, sectionIndex)) {
    builder.rollbackRecord();
    return false;
  }

  havePrevious = false;
  previous = 0u;
  const auto shaderPrepare = [&](D9CCommandChunkWireShaderBindingV2& value,
                                 const PeWireObjectRef& object) {
    value.reserved0 = 0u;
    value.handleIndex = D9C_COMMAND_CHUNK_V2_NULL_HANDLE_INDEX;
    return value.valid <= 1u &&
           orderedSlot(value.stage, havePrevious, previous) &&
           (!value.valid || appendNullableHandle(
                                builder, object, D9C_CHUNK_HANDLE_KIND_SHADER,
                                value.handleIndex));
  };
  if (!appendBindingSection<D9CCommandChunkWireShaderBindingV2, 2u>(
          builder, D9C_COMMAND_CHUNK_V2_SECTION_SHADER, state.shaders,
          shaderPrepare, descs, sectionIndex)) {
    builder.rollbackRecord();
    return false;
  }

  const auto vertexInputPrepare = [&builder](
                                      D9CCommandChunkWireVertexInputV2& value,
                                      const PeWireObjectRef& object) {
    value.handleIndex = D9C_COMMAND_CHUNK_V2_NULL_HANDLE_INDEX;
    if (value.valid > 1u ||
        value.kind > D9C_COMMAND_CHUNK_V2_VERTEX_INPUT_DECLARATION) {
      return false;
    }
    if (!value.valid) {
      value.value = 0u;
      return true;
    }
    if (value.kind == D9C_COMMAND_CHUNK_V2_VERTEX_INPUT_FVF) {
      return !object.object;
    }
    return appendNullableHandle(builder, object,
                                D9C_CHUNK_HANDLE_KIND_VERTEX_DECL,
                                value.handleIndex);
  };
  if (!appendBindingSection<D9CCommandChunkWireVertexInputV2, 1u>(
          builder, D9C_COMMAND_CHUNK_V2_SECTION_VERTEX_INPUT,
          state.vertexInputs, vertexInputPrepare, descs, sectionIndex)) {
    builder.rollbackRecord();
    return false;
  }

  const auto indexPrepare = [&builder](D9CCommandChunkWireIndexBindingV2& value,
                                      const PeWireObjectRef& object) {
    value.handleIndex = D9C_COMMAND_CHUNK_V2_NULL_HANDLE_INDEX;
    return value.valid <= 1u &&
           (!value.valid || appendNullableHandle(
                                builder, object, D9C_CHUNK_HANDLE_KIND_BUFFER,
                                value.handleIndex));
  };
  if (!appendBindingSection<D9CCommandChunkWireIndexBindingV2, 1u>(
          builder, D9C_COMMAND_CHUNK_V2_SECTION_INDEX_BUFFER,
          state.indexBuffers, indexPrepare, descs, sectionIndex)) {
    builder.rollbackRecord();
    return false;
  }

  havePrevious = false;
  previous = 0u;
  const auto renderTargetPrepare = [&builder, &havePrevious, &previous](
                                       D9CCommandChunkWireRenderTargetBindingV2& value,
                                       const PeWireObjectRef& object) {
    value.reserved0 = 0u;
    value.handleIndex = D9C_COMMAND_CHUNK_V2_NULL_HANDLE_INDEX;
    return value.valid <= 1u &&
           orderedSlot(value.slot, havePrevious, previous) &&
           (!value.valid || appendNullableHandle(
                                builder, object, D9C_CHUNK_HANDLE_KIND_SURFACE,
                                value.handleIndex));
  };
  if (!appendBindingSection<
          D9CCommandChunkWireRenderTargetBindingV2,
          D9C_DRAW_PACKET_MAX_RENDER_TARGETS>(
          builder, D9C_COMMAND_CHUNK_V2_SECTION_RENDER_TARGET,
          state.renderTargets, renderTargetPrepare, descs, sectionIndex)) {
    builder.rollbackRecord();
    return false;
  }

  const auto depthPrepare = [&builder](
                                  D9CCommandChunkWireDepthStencilBindingV2& value,
                                  const PeWireObjectRef& object) {
    value.handleIndex = D9C_COMMAND_CHUNK_V2_NULL_HANDLE_INDEX;
    return value.valid <= 1u &&
           (!value.valid || appendNullableHandle(
                                builder, object, D9C_CHUNK_HANDLE_KIND_SURFACE,
                                value.handleIndex));
  };
  if (!appendBindingSection<D9CCommandChunkWireDepthStencilBindingV2, 1u>(
          builder, D9C_COMMAND_CHUNK_V2_SECTION_DEPTH_STENCIL,
          state.depthStencils, depthPrepare, descs, sectionIndex) ||
      !appendPlainSection(builder, D9C_COMMAND_CHUNK_V2_SECTION_VIEWPORT,
                          state.viewports, descs, sectionIndex) ||
      !appendPlainSection(builder, D9C_COMMAND_CHUNK_V2_SECTION_SCISSOR,
                          state.scissors, descs, sectionIndex) ||
      !appendPlainSection(builder, D9C_COMMAND_CHUNK_V2_SECTION_MATERIAL,
                          state.materials, descs, sectionIndex) ||
      !appendPlainSection(builder, D9C_COMMAND_CHUNK_V2_SECTION_CLIP_PLANE,
                          state.clipPlanes, descs, sectionIndex) ||
      !appendPlainSection(
          builder, D9C_COMMAND_CHUNK_V2_SECTION_TEXTURE_STAGE_STATE,
          state.textureStageStates, descs, sectionIndex) ||
      !appendPlainSection(builder,
                          D9C_COMMAND_CHUNK_V2_SECTION_SAMPLER_STATE,
                          state.samplerStates, descs, sectionIndex) ||
      !appendPlainSection(builder, D9C_COMMAND_CHUNK_V2_SECTION_TRANSFORM,
                          state.transforms, descs, sectionIndex) ||
      !appendPlainSection(builder, D9C_COMMAND_CHUNK_V2_SECTION_LIGHT,
                          state.lights, descs, sectionIndex) ||
      !appendPlainSection(builder, D9C_COMMAND_CHUNK_V2_SECTION_LIGHT_ENABLE,
                          state.lightEnables, descs, sectionIndex) ||
      !appendConstantSection(builder,
                             D9C_COMMAND_CHUNK_V2_SECTION_VS_CONST_F,
                             state.vsFloatConstants, descs, sectionIndex) ||
      !appendConstantSection(builder,
                             D9C_COMMAND_CHUNK_V2_SECTION_VS_CONST_I,
                             state.vsIntConstants, descs, sectionIndex) ||
      !appendConstantSection(builder,
                             D9C_COMMAND_CHUNK_V2_SECTION_VS_CONST_B,
                             state.vsBoolConstants, descs, sectionIndex) ||
      !appendConstantSection(builder,
                             D9C_COMMAND_CHUNK_V2_SECTION_PS_CONST_F,
                             state.psFloatConstants, descs, sectionIndex) ||
      !appendConstantSection(builder,
                             D9C_COMMAND_CHUNK_V2_SECTION_PS_CONST_I,
                             state.psIntConstants, descs, sectionIndex) ||
      !appendConstantSection(builder,
                             D9C_COMMAND_CHUNK_V2_SECTION_PS_CONST_B,
                             state.psBoolConstants, descs, sectionIndex) ||
      !appendRawSection(builder, D9C_COMMAND_CHUNK_V2_SECTION_UP_INDEX_DATA,
                        state.upIndexData, descs, sectionIndex) ||
      !appendRawSection(builder, D9C_COMMAND_CHUNK_V2_SECTION_UP_VERTEX_DATA,
                        state.upVertexData, descs, sectionIndex)) {
    builder.rollbackRecord();
    return false;
  }

  if (sectionIndex != count ||
      !builder.overwritePayload(
          draw.sectionTableOffset,
          std::span<const std::byte>(
              reinterpret_cast<const std::byte*>(descs.data()),
              count * sizeof(descs[0]))) ||
      !builder.commitRecord()) {
    builder.rollbackRecord();
    return false;
  }
  return true;
}

bool appendApplyStateV2(CommandChunkV2Builder& builder,
                        std::uint32_t flags,
                        const SparseStateV2Input& state) noexcept {
  D9CCommandChunkWireDrawHeaderV2 draw{};
  draw.flags = flags;
  return appendSparseRecordV2(builder, D9C_COMMAND_RECORD_APPLY_STATE, draw,
                              state);
}

}  // namespace dxmt9::d3d9::pe

namespace dxmt9::d3d9::pe {

namespace {

std::uint64_t legacyWireValue(D9CWireHandle value) noexcept {
  return static_cast<std::uint64_t>(value.lo) |
         (static_cast<std::uint64_t>(value.hi) << 32u);
}

void* legacyWireObject(std::uint64_t value) noexcept {
  return reinterpret_cast<void*>(static_cast<std::uintptr_t>(value));
}

bool cachedLegacyObject(std::uint64_t value, std::uint32_t kind,
                        PeWireObjectRef& out) noexcept {
  return lookupCachedWireObjectRef(legacyWireObject(value), kind, out);
}

bool cachedLegacyObject(D9CWireHandle value, std::uint32_t kind,
                        PeWireObjectRef& out) noexcept {
  return cachedLegacyObject(legacyWireValue(value), kind, out);
}

template <typename T>
bool loadLegacy(std::span<const std::byte> bytes, T& out) noexcept {
  if (bytes.size() < sizeof(T)) {
    return false;
  }
  std::memcpy(&out, bytes.data(), sizeof(T));
  return true;
}

bool legacyRange(std::span<const std::byte> bytes, std::uint32_t offset,
                 std::uint32_t size,
                 std::span<const std::byte>& out) noexcept {
  if (offset > bytes.size() || size > bytes.size() - offset) {
    return false;
  }
  out = bytes.subspan(offset, size);
  return true;
}

std::uint32_t legacyConstantElementSize(std::uint32_t type) noexcept {
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

struct LegacySparseStorage {
  std::array<D9CCommandChunkWireRenderStateV2,
             D9C_DRAW_PACKET_MAX_RENDER_STATES>
      renderStates{};
  std::array<SparseBindingV2Input<D9CCommandChunkWireTextureBindingV2>,
             D9C_DRAW_PACKET_MAX_TEXTURES>
      textures{};
  std::array<SparseBindingV2Input<D9CCommandChunkWireStreamBindingV2>,
             D9C_DRAW_PACKET_MAX_STREAMS>
      streams{};
  std::array<SparseBindingV2Input<D9CCommandChunkWireShaderBindingV2>, 2u>
      shaders{};
  std::array<SparseBindingV2Input<D9CCommandChunkWireVertexInputV2>, 1u>
      vertexInputs{};
  std::array<SparseBindingV2Input<D9CCommandChunkWireIndexBindingV2>, 1u>
      indexBuffers{};
  std::array<SparseBindingV2Input<D9CCommandChunkWireRenderTargetBindingV2>,
             D9C_DRAW_PACKET_MAX_RENDER_TARGETS>
      renderTargets{};
  std::array<SparseBindingV2Input<D9CCommandChunkWireDepthStencilBindingV2>,
             1u>
      depthStencils{};
  std::array<D9CViewport, 1u> viewports{};
  std::array<D9CRect, 1u> scissors{};
  std::array<D9CMaterial, 1u> materials{};
  std::array<D9CCommandChunkWireClipPlaneV2, 6u> clipPlanes{};
  std::array<D9CCommandChunkWireLightV2, D9C_DRAW_PACKET_MAX_LIGHTS> lights{};
  std::array<D9CCommandChunkWireLightEnableV2,
             D9C_DRAW_PACKET_MAX_LIGHTS>
      lightEnables{};
  std::uint32_t textureCount = 0u;
  std::uint32_t streamCount = 0u;
  std::uint32_t shaderCount = 0u;
  std::uint32_t vertexInputCount = 0u;
  std::uint32_t indexBufferCount = 0u;
  std::uint32_t renderTargetCount = 0u;
  std::uint32_t depthStencilCount = 0u;
  std::uint32_t clipPlaneCount = 0u;
  std::uint32_t lightCount = 0u;
  std::uint32_t lightEnableCount = 0u;
};

bool populateLegacySparseState(
    const D9CDrawPrimitivePacket& packet,
    std::span<const std::byte> recordBytes,
    std::uint32_t constDeltaBase,
    const D9CDrawIndexedPrimitivePacket* indexed,
    std::span<const std::byte> upIndexData,
    std::span<const std::byte> upVertexData,
    LegacySparseStorage& storage,
    SparseStateV2Input& state) noexcept {
  if (packet.renderStateCount > storage.renderStates.size() ||
      packet.tssCount > D9C_DRAW_PACKET_MAX_TSS ||
      packet.samplerStateCount > D9C_DRAW_PACKET_MAX_SAMPLER ||
      packet.transformCount > D9C_DRAW_PACKET_MAX_TRANSFORMS) {
    return false;
  }
  for (std::uint32_t i = 0u; i < packet.renderStateCount; ++i) {
    storage.renderStates[i] = D9CCommandChunkWireRenderStateV2{
        .state = packet.renderStates[i].state,
        .value = packet.renderStates[i].value,
    };
  }
  state.renderStates = std::span(storage.renderStates).first(
      packet.renderStateCount);

  for (std::uint32_t slot = 0u; slot < D9C_DRAW_PACKET_MAX_TEXTURES;
       ++slot) {
    if ((packet.textureMask & (1u << slot)) == 0u) {
      continue;
    }
    auto& value = storage.textures[storage.textureCount++];
    value.wire.slot = slot;
    value.wire.valid = 1u;
    if (!cachedLegacyObject(packet.textures[slot],
                            D9C_CHUNK_HANDLE_KIND_TEXTURE, value.object)) {
      return false;
    }
  }
  state.textures = std::span(storage.textures).first(storage.textureCount);

  for (std::uint32_t slot = 0u; slot < D9C_DRAW_PACKET_MAX_STREAMS; ++slot) {
    if ((packet.streamSourceMask & (1u << slot)) == 0u) {
      continue;
    }
    const auto& source = packet.streamSources[slot];
    auto& value = storage.streams[storage.streamCount++];
    value.wire.slot = slot;
    value.wire.valid = 1u;
    value.wire.offset = source.offset;
    value.wire.stride = source.stride;
    value.wire.frequency = 0u;
    if (!cachedLegacyObject(source.buffer, D9C_CHUNK_HANDLE_KIND_BUFFER,
                            value.object)) {
      return false;
    }
  }
  state.streams = std::span(storage.streams).first(storage.streamCount);

  const auto appendShader = [&](std::uint32_t stage, std::uint32_t valid,
                                D9CWireHandle handle) {
    if (!valid) {
      return true;
    }
    auto& value = storage.shaders[storage.shaderCount++];
    value.wire.stage = stage;
    value.wire.valid = 1u;
    return cachedLegacyObject(handle, D9C_CHUNK_HANDLE_KIND_SHADER,
                              value.object);
  };
  if (!appendShader(D9C_COMMAND_CHUNK_V2_SHADER_STAGE_VERTEX,
                    packet.vsValid, packet.vsHandle) ||
      !appendShader(D9C_COMMAND_CHUNK_V2_SHADER_STAGE_PIXEL,
                    packet.psValid, packet.psHandle)) {
    return false;
  }
  state.shaders = std::span(storage.shaders).first(storage.shaderCount);

  if (packet.vdeclValid || packet.fvfValid) {
    auto& value = storage.vertexInputs[storage.vertexInputCount++];
    value.wire.valid = 1u;
    value.wire.value = packet.fvf;
    if (packet.vdeclValid) {
      value.wire.kind = D9C_COMMAND_CHUNK_V2_VERTEX_INPUT_DECLARATION;
      if (!cachedLegacyObject(packet.vdeclHandle,
                              D9C_CHUNK_HANDLE_KIND_VERTEX_DECL,
                              value.object)) {
        return false;
      }
    } else {
      value.wire.kind = D9C_COMMAND_CHUNK_V2_VERTEX_INPUT_FVF;
    }
  }
  state.vertexInputs =
      std::span(storage.vertexInputs).first(storage.vertexInputCount);

  if (indexed && indexed->ibValid) {
    auto& value = storage.indexBuffers[storage.indexBufferCount++];
    value.wire.valid = 1u;
    if (!cachedLegacyObject(indexed->ibHandle, D9C_CHUNK_HANDLE_KIND_BUFFER,
                            value.object)) {
      return false;
    }
  }
  state.indexBuffers =
      std::span(storage.indexBuffers).first(storage.indexBufferCount);

  for (std::uint32_t slot = 0u;
       slot < D9C_DRAW_PACKET_MAX_RENDER_TARGETS; ++slot) {
    if ((packet.rtMask & (1u << slot)) == 0u) {
      continue;
    }
    auto& value = storage.renderTargets[storage.renderTargetCount++];
    value.wire.slot = slot;
    value.wire.valid = 1u;
    if (!cachedLegacyObject(packet.rtHandles[slot],
                            D9C_CHUNK_HANDLE_KIND_SURFACE, value.object)) {
      return false;
    }
  }
  state.renderTargets =
      std::span(storage.renderTargets).first(storage.renderTargetCount);

  if (packet.dsValid) {
    auto& value = storage.depthStencils[storage.depthStencilCount++];
    value.wire.valid = 1u;
    if (!cachedLegacyObject(packet.dsHandle,
                            D9C_CHUNK_HANDLE_KIND_SURFACE, value.object)) {
      return false;
    }
  }
  state.depthStencils =
      std::span(storage.depthStencils).first(storage.depthStencilCount);

  if (packet.viewportValid) {
    storage.viewports[0] = packet.viewport;
    state.viewports = storage.viewports;
  }
  if (packet.scissorValid) {
    storage.scissors[0] = packet.scissor;
    state.scissors = storage.scissors;
  }
  if (packet.materialValid) {
    storage.materials[0] = packet.material;
    state.materials = storage.materials;
  }
  for (std::uint32_t slot = 0u; slot < 6u; ++slot) {
    if ((packet.clipPlaneMask & (1u << slot)) == 0u) {
      continue;
    }
    auto& value = storage.clipPlanes[storage.clipPlaneCount++];
    value.slot = slot;
    std::memcpy(value.values, &packet.clipPlanes[slot * 4u],
                sizeof(value.values));
  }
  state.clipPlanes =
      std::span(storage.clipPlanes).first(storage.clipPlaneCount);
  state.textureStageStates =
      std::span(packet.tss).first(packet.tssCount);
  state.samplerStates =
      std::span(packet.samplerStates).first(packet.samplerStateCount);
  state.transforms =
      std::span(packet.transforms).first(packet.transformCount);

  for (std::uint32_t slot = 0u; slot < D9C_DRAW_PACKET_MAX_LIGHTS; ++slot) {
    if ((packet.lightSlotMask & (1u << slot)) != 0u) {
      auto& value = storage.lights[storage.lightCount++];
      value.slot = slot;
      value.light = packet.lights[slot];
    }
    if ((packet.lightEnableValidMask & (1u << slot)) != 0u) {
      auto& value = storage.lightEnables[storage.lightEnableCount++];
      value.slot = slot;
      value.enabled = (packet.lightEnableMask & (1u << slot)) != 0u;
    }
  }
  state.lights = std::span(storage.lights).first(storage.lightCount);
  state.lightEnables =
      std::span(storage.lightEnables).first(storage.lightEnableCount);

  std::array<SparseConstantRangeV2Input*,
             D9C_DRAW_PACKET_CONST_DELTA_COUNT>
      constants = {{
          &state.vsFloatConstants,
          &state.vsIntConstants,
          &state.vsBoolConstants,
          &state.psFloatConstants,
          &state.psIntConstants,
          &state.psBoolConstants,
      }};
  for (std::uint32_t kind = 0u;
       kind < D9C_DRAW_PACKET_CONST_DELTA_COUNT; ++kind) {
    const auto& source = packet.constDeltaSections[kind];
    if (!source.valid ||
        !d9c_draw_packet_const_delta_section_range_valid(
            kind, source.startRegister, source.registerCount)) {
      continue;
    }
    const auto slice = d9c_draw_packet_const_delta_section_slice(
        &packet, constDeltaBase, kind);
    std::span<const std::byte> registerBytes;
    if (!legacyRange(recordBytes, slice.payloadOffset, slice.payloadSize,
                     registerBytes)) {
      return false;
    }
    *constants[kind] = SparseConstantRangeV2Input{
        .startRegister = source.startRegister,
        .registerCount = source.registerCount,
        .registerBytes = registerBytes,
    };
  }
  state.upIndexData = upIndexData;
  state.upVertexData = upVertexData;
  return true;
}

bool appendLegacySparseRecord(CommandChunkV2Builder& builder,
                              std::uint32_t type,
                              std::span<const std::byte> bytes) noexcept {
  D9CCommandChunkWireDrawHeaderV2 draw{};
  LegacySparseStorage storage{};
  SparseStateV2Input state{};
  const D9CDrawPrimitivePacket* packet = nullptr;
  const D9CDrawIndexedPrimitivePacket* indexed = nullptr;
  std::uint32_t constDeltaBase = 0u;
  std::span<const std::byte> upIndexData;
  std::span<const std::byte> upVertexData;

  D9CCommandRecordDrawPrimitive primitive{};
  D9CCommandRecordDrawIndexedPrimitive indexedPrimitive{};
  D9CCommandRecordDrawPrimitiveUP primitiveUp{};
  D9CCommandRecordDrawIndexedPrimitiveUP indexedPrimitiveUp{};
  D9CCommandRecordApplyState apply{};
  switch (type) {
    case D9C_COMMAND_RECORD_DRAW_PRIMITIVE:
      if (!loadLegacy(bytes, primitive)) return false;
      packet = &primitive.packet;
      draw.primitiveType = packet->primitiveType;
      draw.startVertex = packet->startVertex;
      draw.primitiveCount = packet->primitiveCount;
      constDeltaBase = d9c_command_record_draw_primitive_const_delta_offset();
      break;
    case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE:
      if (!loadLegacy(bytes, indexedPrimitive)) return false;
      packet = &indexedPrimitive.packet.state;
      indexed = &indexedPrimitive.packet;
      draw.primitiveType = packet->primitiveType;
      draw.baseVertex = indexed->baseVertex;
      draw.minVertex = indexed->minVertex;
      draw.numVertices = indexed->numVertices;
      draw.startIndex = indexed->startIndex;
      draw.primitiveCount = indexed->primitiveCount;
      constDeltaBase =
          d9c_command_record_draw_indexed_primitive_const_delta_offset();
      break;
    case D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP:
      if (!loadLegacy(bytes, primitiveUp)) return false;
      packet = &primitiveUp.packet.state;
      draw.primitiveType = packet->primitiveType;
      draw.primitiveCount = primitiveUp.packet.primitiveCount;
      draw.stride = primitiveUp.packet.stride;
      if (!legacyRange(bytes, primitiveUp.packet.vertexDataOffset,
                       primitiveUp.packet.vertexDataSize, upVertexData)) {
        return false;
      }
      constDeltaBase =
          d9c_command_record_draw_primitive_up_const_delta_offset(
              &primitiveUp.packet);
      break;
    case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP:
      if (!loadLegacy(bytes, indexedPrimitiveUp)) return false;
      packet = &indexedPrimitiveUp.packet.state;
      draw.primitiveType = packet->primitiveType;
      draw.minVertex = indexedPrimitiveUp.packet.minVertex;
      draw.numVertices = indexedPrimitiveUp.packet.numVertices;
      draw.primitiveCount = indexedPrimitiveUp.packet.primitiveCount;
      draw.stride = indexedPrimitiveUp.packet.stride;
      draw.indexFormat = indexedPrimitiveUp.packet.indexFormat;
      if (!legacyRange(bytes, indexedPrimitiveUp.packet.indexDataOffset,
                       indexedPrimitiveUp.packet.indexDataSize, upIndexData) ||
          !legacyRange(bytes, indexedPrimitiveUp.packet.vertexDataOffset,
                       indexedPrimitiveUp.packet.vertexDataSize,
                       upVertexData)) {
        return false;
      }
      constDeltaBase =
          d9c_command_record_draw_indexed_primitive_up_const_delta_offset(
              &indexedPrimitiveUp.packet);
      break;
    case D9C_COMMAND_RECORD_APPLY_STATE:
      if (!loadLegacy(bytes, apply)) return false;
      packet = &apply.packet;
      constDeltaBase = sizeof(apply);
      break;
    default:
      return false;
  }

  if (!packet || !populateLegacySparseState(
                     *packet, bytes, constDeltaBase, indexed, upIndexData,
                     upVertexData, storage, state)) {
    return false;
  }
  const auto allTextures =
      (1u << D9C_DRAW_PACKET_MAX_TEXTURES) - 1u;
  const auto allStreams = (1u << D9C_DRAW_PACKET_MAX_STREAMS) - 1u;
  if (packet->textureMask == allTextures &&
      packet->streamSourceMask == allStreams) {
    draw.flags |= D9C_COMMAND_CHUNK_V2_DRAW_FLAG_FULL_SNAPSHOT;
  }
  return type == D9C_COMMAND_RECORD_APPLY_STATE
             ? appendApplyStateV2(builder, draw.flags, state)
             : appendSparseRecordV2(builder, type, draw, state);
}

}  // namespace

bool appendLegacyCommandRecordAsV2(
    CommandChunkV2Builder& builder,
    std::span<const std::byte> bytes) noexcept {
  D9CCommandRecordHeader header{};
  if (!loadLegacy(bytes, header) || header.size != bytes.size()) {
    return false;
  }
  switch (header.type) {
    case D9C_COMMAND_RECORD_DRAW_PRIMITIVE:
    case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE:
    case D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP:
    case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP:
    case D9C_COMMAND_RECORD_APPLY_STATE:
      return appendLegacySparseRecord(builder, header.type, bytes);
    case D9C_COMMAND_RECORD_SET_VS_CONST_F:
    case D9C_COMMAND_RECORD_SET_VS_CONST_I:
    case D9C_COMMAND_RECORD_SET_VS_CONST_B:
    case D9C_COMMAND_RECORD_SET_PS_CONST_F:
    case D9C_COMMAND_RECORD_SET_PS_CONST_I:
    case D9C_COMMAND_RECORD_SET_PS_CONST_B: {
      D9CCommandRecordSetConst record{};
      if (!loadLegacy(bytes, record)) return false;
      const auto elementSize = legacyConstantElementSize(header.type);
      const auto payloadSize = static_cast<std::uint64_t>(record.count) *
                               elementSize;
      if (payloadSize > std::numeric_limits<std::uint32_t>::max()) {
        return false;
      }
      std::span<const std::byte> values;
      return legacyRange(bytes, sizeof(record),
                         static_cast<std::uint32_t>(payloadSize), values) &&
             sizeof(record) + payloadSize == bytes.size() &&
             appendSetConstantsV2(builder, header.type, record.start,
                                  record.count, values);
    }
    case D9C_COMMAND_RECORD_CLEAR: {
      D9CCommandRecordClear record{};
      if (!loadLegacy(bytes, record) ||
          record.rectCount >
              std::numeric_limits<std::uint32_t>::max() / sizeof(D9CRect)) {
        return false;
      }
      std::span<const std::byte> rectBytes;
      if (!legacyRange(bytes, record.rectOffset,
                       record.rectCount * sizeof(D9CRect), rectBytes)) {
        return false;
      }
      return appendClearV2(
          builder,
          D9CCommandChunkWireClearV2{
              .flags = record.flags,
              .colorARGB = record.colorARGB,
              .z = record.z,
              .stencil = record.stencil,
              .rectCount = 0u,
              .rectOffset = 0u,
          },
          std::span<const D9CRect>(
              reinterpret_cast<const D9CRect*>(rectBytes.data()),
              record.rectCount));
    }
    case D9C_COMMAND_RECORD_PRESENT: {
      D9CCommandRecordPresent record{};
      return loadLegacy(bytes, record) &&
             appendPresentV2(
                 builder,
                 D9CCommandChunkWirePresentV2{
                     .hwnd = record.hwnd,
                     .flags = record.flags,
                     .hasSrc = record.hasSrc,
                     .hasDst = record.hasDst,
                     .reserved0 = 0u,
                     .src = record.src,
                     .dst = record.dst,
                 });
    }
    case D9C_COMMAND_RECORD_STRETCH_RECT: {
      D9CCommandRecordStretchRect record{};
      PeWireObjectRef src{};
      PeWireObjectRef dst{};
      return loadLegacy(bytes, record) &&
             cachedLegacyObject(record.srcWire,
                                D9C_CHUNK_HANDLE_KIND_SURFACE, src) &&
             cachedLegacyObject(record.dstWire,
                                D9C_CHUNK_HANDLE_KIND_SURFACE, dst) &&
             appendStretchRectV2(
                 builder,
                 D9CCommandChunkWireStretchRectV2{
                     .srcHandleIndex = 0u,
                     .dstHandleIndex = 0u,
                     .hasSrcRect = record.hasSrcRect,
                     .hasDstRect = record.hasDstRect,
                     .filter = record.filter,
                     .reserved0 = 0u,
                     .srcRect = record.srcRect,
                     .dstRect = record.dstRect,
                 }, src, dst);
    }
    case D9C_COMMAND_RECORD_COLOR_FILL: {
      D9CCommandRecordColorFill record{};
      PeWireObjectRef surface{};
      return loadLegacy(bytes, record) &&
             cachedLegacyObject(record.surfaceWire,
                                D9C_CHUNK_HANDLE_KIND_SURFACE, surface) &&
             appendColorFillV2(
                 builder,
                 D9CCommandChunkWireColorFillV2{
                     .surfaceHandleIndex = 0u,
                     .colorARGB = record.colorARGB,
                     .hasRect = record.hasRect,
                     .reserved0 = 0u,
                     .rect = record.rect,
                 }, surface);
    }
    case D9C_COMMAND_RECORD_UPDATE_TEXTURE: {
      D9CCommandRecordUpdateTexture record{};
      PeWireObjectRef src{};
      PeWireObjectRef dst{};
      return loadLegacy(bytes, record) &&
             cachedLegacyObject(record.srcWire,
                                D9C_CHUNK_HANDLE_KIND_TEXTURE, src) &&
             cachedLegacyObject(record.dstWire,
                                D9C_CHUNK_HANDLE_KIND_TEXTURE, dst) &&
             appendUpdateTextureV2(builder, src, dst);
    }
    case D9C_COMMAND_RECORD_UPDATE_SURFACE: {
      D9CCommandRecordUpdateSurface record{};
      PeWireObjectRef src{};
      PeWireObjectRef dst{};
      return loadLegacy(bytes, record) &&
             cachedLegacyObject(record.srcWire,
                                D9C_CHUNK_HANDLE_KIND_SURFACE, src) &&
             cachedLegacyObject(record.dstWire,
                                D9C_CHUNK_HANDLE_KIND_SURFACE, dst) &&
             appendUpdateSurfaceV2(
                 builder,
                 D9CCommandChunkWireUpdateSurfaceV2{
                     .srcHandleIndex = 0u,
                     .dstHandleIndex = 0u,
                     .hasSrcRect = record.hasSrcRect,
                     .hasDstPoint = record.hasDstPoint,
                     .srcRect = record.srcRect,
                     .dstPoint = record.dstPoint,
                 }, src, dst);
    }
    case D9C_COMMAND_RECORD_QUERY_ISSUE: {
      D9CCommandRecordQueryIssue record{};
      PeWireObjectRef query{};
      return loadLegacy(bytes, record) &&
             cachedLegacyObject(record.queryWire,
                                D9C_CHUNK_HANDLE_KIND_QUERY, query) &&
             appendQueryIssueV2(builder, record.flags, query);
    }
    case D9C_COMMAND_RECORD_READBACK: {
      D9CCommandRecordReadback record{};
      PeWireObjectRef src{};
      PeWireObjectRef dst{};
      return loadLegacy(bytes, record) &&
             cachedLegacyObject(record.srcWire,
                                D9C_CHUNK_HANDLE_KIND_SURFACE, src) &&
             cachedLegacyObject(record.dstWire,
                                D9C_CHUNK_HANDLE_KIND_SURFACE, dst) &&
             appendReadbackV2(builder, src, dst);
    }
    case D9C_COMMAND_RECORD_RESZ_DEPTH_RESOLVE: {
      D9CCommandRecordReszDepthResolve record{};
      PeWireObjectRef src{};
      PeWireObjectRef dst{};
      return loadLegacy(bytes, record) &&
             cachedLegacyObject(record.msaaDepthHandle,
                                D9C_CHUNK_HANDLE_KIND_SURFACE, src) &&
             cachedLegacyObject(record.intzDestHandle,
                                D9C_CHUNK_HANDLE_KIND_TEXTURE, dst) &&
             appendReszDepthResolveV2(builder, src, dst);
    }
    default:
      return false;
  }
}

}  // namespace dxmt9::d3d9::pe
