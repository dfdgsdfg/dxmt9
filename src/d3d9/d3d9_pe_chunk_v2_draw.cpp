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
