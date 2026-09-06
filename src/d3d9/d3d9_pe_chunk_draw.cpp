#include "d3d9_pe_chunk_builder.hpp"
#include "d3d9_pe_producer_views.hpp"

#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <utility>

namespace dxmt9::d3d9::pe {

namespace {

bool validSectionCount(std::uint16_t kind, std::size_t count) {
  const auto* rule = sectionRule(kind);
  return rule && count != 0u && count <= rule->maxCount &&
         count <= std::numeric_limits<std::uint32_t>::max();
}

bool validConstant(std::uint16_t kind,
                   const SparseConstantRangeInput& input) {
  const auto* rule = sectionRule(kind);
  if (!input.present()) {
    return input.startRegister == 0u && input.registerBytes.empty();
  }
  const auto end = static_cast<std::uint64_t>(input.startRegister) +
                   input.registerCount;
  const auto byteSize = static_cast<std::uint64_t>(input.registerCount) *
                        (rule ? rule->elementSize : 0u);
  return rule && (rule->ruleFlags & SectionRuleConstantRange) != 0u &&
         input.registerCount <= rule->maxCount && end <= rule->maxCount &&
         byteSize == input.registerBytes.size();
}

template <typename T>
bool appendPlainSection(
    CommandChunkBuilder& builder, std::uint16_t kind,
    std::span<const T> values,
    std::array<D9CCommandChunkWireSectionDesc,
               D9C_COMMAND_CHUNK_SECTION_COUNT>& descs,
    std::uint32_t& sectionIndex) {
  if (values.empty()) {
    return true;
  }
  const auto* rule = sectionRule(kind);
  std::uint32_t offset = 0u;
  if (!rule || !validSectionCount(kind, values.size()) ||
      !builder.appendSectionPayload(kind, values, &offset)) {
    return false;
  }
  descs[sectionIndex++] = D9CCommandChunkWireSectionDesc{
      .kind = kind,
      .elementSize = rule->elementSize,
      .count = static_cast<std::uint32_t>(values.size()),
      .payloadOffset = offset,
      .byteSize = static_cast<std::uint32_t>(values.size_bytes()),
  };
  return true;
}

bool appendConstantSection(
    CommandChunkBuilder& builder, std::uint16_t kind,
    const SparseConstantRangeInput& input,
    std::array<D9CCommandChunkWireSectionDesc,
               D9C_COMMAND_CHUNK_SECTION_COUNT>& descs,
    std::uint32_t& sectionIndex) {
  if (!input.present()) {
    return true;
  }
  const auto* rule = sectionRule(kind);
  std::uint32_t offset = 0u;
  if (!rule || !validConstant(kind, input) ||
      !builder.appendConstantSectionPayload(
          kind, input.startRegister, input.registerCount,
          input.registerBytes, &offset)) {
    return false;
  }
  descs[sectionIndex++] = D9CCommandChunkWireSectionDesc{
      .kind = kind,
      .elementSize = rule->elementSize,
      .count = input.registerCount,
      .payloadOffset = offset,
      .byteSize = static_cast<std::uint32_t>(
          sizeof(D9CCommandChunkWireConstantRange) +
          input.registerBytes.size()),
  };
  return true;
}

bool appendRawSection(
    CommandChunkBuilder& builder, std::uint16_t kind,
    std::span<const std::byte> bytes,
    std::array<D9CCommandChunkWireSectionDesc,
               D9C_COMMAND_CHUNK_SECTION_COUNT>& descs,
    std::uint32_t& sectionIndex) {
  if (bytes.empty()) {
    return true;
  }
  const auto* rule = sectionRule(kind);
  std::uint32_t offset = 0u;
  if (!rule || !validSectionCount(kind, bytes.size()) ||
      !builder.appendUpDataSectionPayload(kind, bytes, &offset)) {
    return false;
  }
  descs[sectionIndex++] = D9CCommandChunkWireSectionDesc{
      .kind = kind,
      .elementSize = rule->elementSize,
      .count = static_cast<std::uint32_t>(bytes.size()),
      .payloadOffset = offset,
      .byteSize = static_cast<std::uint32_t>(bytes.size()),
  };
  return true;
}

bool appendNullableHandle(CommandChunkBuilder& builder,
                          const PeWireObjectRef& object,
                          std::uint32_t expectedKind,
                          std::uint32_t& index) {
  index = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX;
  if (!object.object) {
    return true;
  }
  return builder.appendHandle(object, expectedKind, index);
}

template <typename Wire, std::size_t Capacity, typename Prepare>
bool appendBindingSection(
    CommandChunkBuilder& builder, std::uint16_t kind,
    std::span<const SparseBindingInput<Wire>> inputs, Prepare&& prepare,
    std::array<D9CCommandChunkWireSectionDesc,
               D9C_COMMAND_CHUNK_SECTION_COUNT>& descs,
    std::uint32_t& sectionIndex) {
  if (inputs.empty()) {
    return true;
  }
  if (!validSectionCount(kind, inputs.size()) || inputs.size() > Capacity) {
    return false;
  }
  const auto* rule = sectionRule(kind);
  std::uint32_t offset = 0u;
  if (!rule || !builder.appendGeneratedSectionPayload<Wire>(
                   kind, inputs.size(),
                   [&](std::size_t index, Wire& value) noexcept {
                     value = inputs[index].wire;
                     return prepare(value, inputs[index].object);
                   },
                   &offset)) {
    return false;
  }
  descs[sectionIndex++] = D9CCommandChunkWireSectionDesc{
      .kind = kind,
      .elementSize = rule->elementSize,
      .count = static_cast<std::uint32_t>(inputs.size()),
      .payloadOffset = offset,
      .byteSize = static_cast<std::uint32_t>(inputs.size() * sizeof(Wire)),
  };
  return true;
}

std::uint32_t sectionCount(const SparseStateInput& state) {
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

bool appendSparseRecord(CommandChunkBuilder& builder,
                          std::uint32_t type,
                          D9CCommandChunkWireDrawHeader draw,
                          const SparseStateInput& state) noexcept {
  const auto* rule = recordRule(type);
  const auto count = sectionCount(state);
  if (!rule ||
      (rule->ruleFlags & RecordRuleSparseState) == 0u ||
      count > D9C_COMMAND_CHUNK_SECTION_COUNT ||
      !validConstant(D9C_COMMAND_CHUNK_SECTION_VS_CONST_F,
                     state.vsFloatConstants) ||
      !validConstant(D9C_COMMAND_CHUNK_SECTION_VS_CONST_I,
                     state.vsIntConstants) ||
      !validConstant(D9C_COMMAND_CHUNK_SECTION_VS_CONST_B,
                     state.vsBoolConstants) ||
      !validConstant(D9C_COMMAND_CHUNK_SECTION_PS_CONST_F,
                     state.psFloatConstants) ||
      !validConstant(D9C_COMMAND_CHUNK_SECTION_PS_CONST_I,
                     state.psIntConstants) ||
      !validConstant(D9C_COMMAND_CHUNK_SECTION_PS_CONST_B,
                     state.psBoolConstants)) {
    return false;
  }

  draw.sectionCount = count;
  draw.sectionTableOffset = sizeof(draw);
  draw.sectionPayloadOffset =
      sizeof(draw) + count * sizeof(D9CCommandChunkWireSectionDesc);
  draw.reserved0 = 0u;
  if (!builder.beginRecord(type) || !builder.appendPayloadValue(draw)) {
    return false;
  }

  std::array<D9CCommandChunkWireSectionDesc,
             D9C_COMMAND_CHUNK_SECTION_COUNT>
      descs{};
  if (!builder.appendSectionTable(
          std::span<const D9CCommandChunkWireSectionDesc>(descs.data(),
                                                          count))) {
    return false;
  }

  std::uint32_t sectionIndex = 0u;
  bool havePrevious = false;
  std::uint32_t previous = 0u;
  const auto texturePrepare = [&](D9CCommandChunkWireTextureBinding& value,
                                  const PeWireObjectRef& object) noexcept {
    value.reserved0 = 0u;
    value.handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX;
    return value.valid <= 1u &&
           orderedSlot(value.slot, havePrevious, previous) &&
           (!value.valid || appendNullableHandle(
                                builder, object,
                                D9C_CHUNK_HANDLE_KIND_TEXTURE,
                                value.handleIndex));
  };
  if (!appendPlainSection(builder, D9C_COMMAND_CHUNK_SECTION_RENDER_STATE,
                          state.renderStates, descs, sectionIndex) ||
      !appendBindingSection<
          D9CCommandChunkWireTextureBinding,
          D9C_DRAW_PACKET_MAX_TEXTURES>(
          builder, D9C_COMMAND_CHUNK_SECTION_TEXTURE, state.textures,
          texturePrepare, descs, sectionIndex)) {
    builder.rollbackRecord();
    return false;
  }

  havePrevious = false;
  previous = 0u;
  const auto streamPrepare = [&](D9CCommandChunkWireStreamBinding& value,
                                 const PeWireObjectRef& object) noexcept {
    value.reserved0 = 0u;
    value.handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX;
    return value.valid <= 1u &&
           orderedSlot(value.slot, havePrevious, previous) &&
           (!value.valid || appendNullableHandle(
                                builder, object, D9C_CHUNK_HANDLE_KIND_BUFFER,
                                value.handleIndex));
  };
  if (!appendBindingSection<D9CCommandChunkWireStreamBinding,
                            D9C_DRAW_PACKET_MAX_STREAMS>(
          builder, D9C_COMMAND_CHUNK_SECTION_STREAM, state.streams,
          streamPrepare, descs, sectionIndex)) {
    builder.rollbackRecord();
    return false;
  }

  havePrevious = false;
  previous = 0u;
  const auto shaderPrepare = [&](D9CCommandChunkWireShaderBinding& value,
                                 const PeWireObjectRef& object) noexcept {
    value.reserved0 = 0u;
    value.handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX;
    return value.valid <= 1u &&
           orderedSlot(value.stage, havePrevious, previous) &&
           (!value.valid || appendNullableHandle(
                                builder, object, D9C_CHUNK_HANDLE_KIND_SHADER,
                                value.handleIndex));
  };
  if (!appendBindingSection<D9CCommandChunkWireShaderBinding, 2u>(
          builder, D9C_COMMAND_CHUNK_SECTION_SHADER, state.shaders,
          shaderPrepare, descs, sectionIndex)) {
    builder.rollbackRecord();
    return false;
  }

  const auto vertexInputPrepare = [&builder](
                                      D9CCommandChunkWireVertexInput& value,
                                      const PeWireObjectRef& object) noexcept {
    value.handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX;
    if (value.valid > 1u ||
        value.kind > D9C_COMMAND_CHUNK_VERTEX_INPUT_DECLARATION) {
      return false;
    }
    if (!value.valid) {
      value.value = 0u;
      return true;
    }
    if (value.kind == D9C_COMMAND_CHUNK_VERTEX_INPUT_FVF) {
      return !object.object;
    }
    return appendNullableHandle(builder, object,
                                D9C_CHUNK_HANDLE_KIND_VERTEX_DECL,
                                value.handleIndex);
  };
  if (!appendBindingSection<D9CCommandChunkWireVertexInput, 1u>(
          builder, D9C_COMMAND_CHUNK_SECTION_VERTEX_INPUT,
          state.vertexInputs, vertexInputPrepare, descs, sectionIndex)) {
    builder.rollbackRecord();
    return false;
  }

  const auto indexPrepare = [&builder](D9CCommandChunkWireIndexBinding& value,
                                      const PeWireObjectRef& object) noexcept {
    value.handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX;
    return value.valid <= 1u &&
           (!value.valid || appendNullableHandle(
                                builder, object, D9C_CHUNK_HANDLE_KIND_BUFFER,
                                value.handleIndex));
  };
  if (!appendBindingSection<D9CCommandChunkWireIndexBinding, 1u>(
          builder, D9C_COMMAND_CHUNK_SECTION_INDEX_BUFFER,
          state.indexBuffers, indexPrepare, descs, sectionIndex)) {
    builder.rollbackRecord();
    return false;
  }

  havePrevious = false;
  previous = 0u;
  const auto renderTargetPrepare = [&builder, &havePrevious, &previous](
                                       D9CCommandChunkWireRenderTargetBinding& value,
                                       const PeWireObjectRef& object) noexcept {
    value.reserved0 = 0u;
    value.handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX;
    return value.valid <= 1u &&
           orderedSlot(value.slot, havePrevious, previous) &&
           (!value.valid || appendNullableHandle(
                                builder, object, D9C_CHUNK_HANDLE_KIND_SURFACE,
                                value.handleIndex));
  };
  if (!appendBindingSection<
          D9CCommandChunkWireRenderTargetBinding,
          D9C_DRAW_PACKET_MAX_RENDER_TARGETS>(
          builder, D9C_COMMAND_CHUNK_SECTION_RENDER_TARGET,
          state.renderTargets, renderTargetPrepare, descs, sectionIndex)) {
    builder.rollbackRecord();
    return false;
  }

  const auto depthPrepare = [&builder](
                                  D9CCommandChunkWireDepthStencilBinding& value,
                                  const PeWireObjectRef& object) noexcept {
    value.handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX;
    return value.valid <= 1u &&
           (!value.valid || appendNullableHandle(
                                builder, object, D9C_CHUNK_HANDLE_KIND_SURFACE,
                                value.handleIndex));
  };
  if (!appendBindingSection<D9CCommandChunkWireDepthStencilBinding, 1u>(
          builder, D9C_COMMAND_CHUNK_SECTION_DEPTH_STENCIL,
          state.depthStencils, depthPrepare, descs, sectionIndex) ||
      !appendPlainSection(builder, D9C_COMMAND_CHUNK_SECTION_VIEWPORT,
                          state.viewports, descs, sectionIndex) ||
      !appendPlainSection(builder, D9C_COMMAND_CHUNK_SECTION_SCISSOR,
                          state.scissors, descs, sectionIndex) ||
      !appendPlainSection(builder, D9C_COMMAND_CHUNK_SECTION_MATERIAL,
                          state.materials, descs, sectionIndex) ||
      !appendPlainSection(builder, D9C_COMMAND_CHUNK_SECTION_CLIP_PLANE,
                          state.clipPlanes, descs, sectionIndex) ||
      !appendPlainSection(
          builder, D9C_COMMAND_CHUNK_SECTION_TEXTURE_STAGE_STATE,
          state.textureStageStates, descs, sectionIndex) ||
      !appendPlainSection(builder,
                          D9C_COMMAND_CHUNK_SECTION_SAMPLER_STATE,
                          state.samplerStates, descs, sectionIndex) ||
      !appendPlainSection(builder, D9C_COMMAND_CHUNK_SECTION_TRANSFORM,
                          state.transforms, descs, sectionIndex) ||
      !appendPlainSection(builder, D9C_COMMAND_CHUNK_SECTION_LIGHT,
                          state.lights, descs, sectionIndex) ||
      !appendPlainSection(builder, D9C_COMMAND_CHUNK_SECTION_LIGHT_ENABLE,
                          state.lightEnables, descs, sectionIndex) ||
      !appendConstantSection(builder,
                             D9C_COMMAND_CHUNK_SECTION_VS_CONST_F,
                             state.vsFloatConstants, descs, sectionIndex) ||
      !appendConstantSection(builder,
                             D9C_COMMAND_CHUNK_SECTION_VS_CONST_I,
                             state.vsIntConstants, descs, sectionIndex) ||
      !appendConstantSection(builder,
                             D9C_COMMAND_CHUNK_SECTION_VS_CONST_B,
                             state.vsBoolConstants, descs, sectionIndex) ||
      !appendConstantSection(builder,
                             D9C_COMMAND_CHUNK_SECTION_PS_CONST_F,
                             state.psFloatConstants, descs, sectionIndex) ||
      !appendConstantSection(builder,
                             D9C_COMMAND_CHUNK_SECTION_PS_CONST_I,
                             state.psIntConstants, descs, sectionIndex) ||
      !appendConstantSection(builder,
                             D9C_COMMAND_CHUNK_SECTION_PS_CONST_B,
                             state.psBoolConstants, descs, sectionIndex) ||
      !appendRawSection(builder, D9C_COMMAND_CHUNK_SECTION_UP_INDEX_DATA,
                        state.upIndexData, descs, sectionIndex) ||
      !appendRawSection(builder, D9C_COMMAND_CHUNK_SECTION_UP_VERTEX_DATA,
                        state.upVertexData, descs, sectionIndex)) {
    builder.rollbackRecord();
    return false;
  }

  if (sectionIndex != count ||
      !builder.overwriteSectionTable(
          draw.sectionTableOffset,
          std::span<const D9CCommandChunkWireSectionDesc>(descs.data(),
                                                          count)) ||
      !builder.commitRecord()) {
    builder.rollbackRecord();
    return false;
  }
  return true;
}

bool appendApplyState(CommandChunkBuilder& builder,
                        std::uint32_t flags,
                        const SparseStateInput& state) noexcept {
  D9CCommandChunkWireDrawHeader draw{};
  draw.flags = flags;
  return appendSparseRecord(builder, D9C_COMMAND_RECORD_APPLY_STATE, draw,
                              state);
}

namespace {

template <typename T, typename Visit>
bool appendVisitedSection(
    CommandChunkBuilder& builder, std::uint16_t kind, std::size_t count,
    Visit&& visit,
    std::array<D9CCommandChunkWireSectionDesc,
               D9C_COMMAND_CHUNK_SECTION_COUNT>& descs,
    std::uint32_t& sectionIndex) {
  if (count == 0u) {
    return true;
  }
  const auto* rule = sectionRule(kind);
  std::uint32_t offset = 0u;
  if (!rule || !validSectionCount(kind, count) ||
      !builder.appendVisitedSectionPayload<T>(
          kind, count, std::forward<Visit>(visit), &offset)) {
    return false;
  }
  descs[sectionIndex++] = D9CCommandChunkWireSectionDesc{
      .kind = kind,
      .elementSize = rule->elementSize,
      .count = static_cast<std::uint32_t>(count),
      .payloadOffset = offset,
      .byteSize = static_cast<std::uint32_t>(count * sizeof(T)),
  };
  return true;
}

std::uint32_t planSectionCount(
    const SparseStatePlan::SourceFacts& source,
    const SparseStatePlan::DestinationFacts& destination) noexcept {
  std::uint32_t count = 0u;
  const auto add = [&count](bool present) noexcept {
    count += present ? 1u : 0u;
  };
  add(source.renderStateCount != 0u);
  add(source.textureMask != 0u);
  add(destination.streamMask != 0u);
  add(source.shaderMask != 0u);
  add(source.vertexInput);
  add(destination.indexBuffer);
  add(source.renderTargetMask != 0u);
  add(source.depthStencil);
  add(source.viewport);
  add(source.scissor);
  add(source.material);
  add(source.clipPlaneMask != 0u);
  add(source.textureStageStateCount != 0u);
  add(source.samplerStateCount != 0u);
  add(source.transformCount != 0u);
  add(source.lightMask != 0u);
  add(source.lightEnableMask != 0u);
  add(source.vsFloatConstants.present());
  add(source.vsIntConstants.present());
  add(source.vsBoolConstants.present());
  add(source.psFloatConstants.present());
  add(source.psIntConstants.present());
  add(source.psBoolConstants.present());
  add(!source.payloads.upIndex.empty());
  add(!source.payloads.upVertex.empty());
  return count;
}

D9CCommandChunkWireDrawHeader planDrawHeader(
    const SparseStatePlan::SourceFacts& source,
    std::uint32_t flags) noexcept {
  D9CCommandChunkWireDrawHeader header{};
  header.flags = flags;
  switch (source.draw.recordType) {
    case D9C_COMMAND_RECORD_DRAW_PRIMITIVE:
      header.primitiveType = source.draw.primitiveType;
      header.startVertex = source.draw.startVertex;
      header.primitiveCount = source.draw.primitiveCount;
      break;
    case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE:
      header.primitiveType = source.draw.primitiveType;
      header.baseVertex = source.draw.baseVertex;
      header.minVertex = source.draw.minVertex;
      header.numVertices = source.draw.numVertices;
      header.startIndex = source.draw.startIndex;
      header.primitiveCount = source.draw.primitiveCount;
      break;
    case D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP:
      header.primitiveType = source.draw.primitiveType;
      header.primitiveCount = source.draw.primitiveCount;
      header.stride = source.draw.stride;
      break;
    case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP:
      header.primitiveType = source.draw.primitiveType;
      header.minVertex = source.draw.minVertex;
      header.numVertices = source.draw.numVertices;
      header.primitiveCount = source.draw.primitiveCount;
      header.stride = source.draw.stride;
      header.indexFormat = source.draw.indexFormat;
      break;
    default:
      break;
  }
  return header;
}

bool appendSparsePlanRecordWithSources(
    CommandChunkBuilder& builder, std::uint32_t type,
    std::uint32_t flags, const SparseStatePlan::SourceFacts& source,
    const SparseStatePlan::DestinationFacts& destination,
    const PeHotStateShadow& shadow,
    const PeBindingView& bindings) noexcept {
  const auto* record = recordRule(type);
  const auto count = planSectionCount(source, destination);
  if (!record || (record->ruleFlags & RecordRuleSparseState) == 0u ||
      !source.prepared || !destination.chunkContextFinalized ||
      source.draw.recordType != type ||
      count > D9C_COMMAND_CHUNK_SECTION_COUNT ||
      std::popcount(source.textureMask) > D9C_DRAW_PACKET_MAX_TEXTURES ||
      std::popcount(destination.streamMask) > D9C_DRAW_PACKET_MAX_STREAMS ||
      std::popcount(source.renderTargetMask) >
          D9C_DRAW_PACKET_MAX_RENDER_TARGETS ||
      std::popcount(source.clipPlaneMask) > 6 ||
      std::popcount(source.lightMask) > D9C_DRAW_PACKET_MAX_LIGHTS ||
      std::popcount(source.lightEnableMask) >
          D9C_DRAW_PACKET_MAX_LIGHTS ||
      !validConstant(D9C_COMMAND_CHUNK_SECTION_VS_CONST_F,
                     source.vsFloatConstants) ||
      !validConstant(D9C_COMMAND_CHUNK_SECTION_VS_CONST_I,
                     source.vsIntConstants) ||
      !validConstant(D9C_COMMAND_CHUNK_SECTION_VS_CONST_B,
                     source.vsBoolConstants) ||
      !validConstant(D9C_COMMAND_CHUNK_SECTION_PS_CONST_F,
                     source.psFloatConstants) ||
      !validConstant(D9C_COMMAND_CHUNK_SECTION_PS_CONST_I,
                     source.psIntConstants) ||
      !validConstant(D9C_COMMAND_CHUNK_SECTION_PS_CONST_B,
                     source.psBoolConstants)) {
    return false;
  }

  auto draw = planDrawHeader(source, flags);
  draw.sectionCount = count;
  draw.sectionTableOffset = sizeof(draw);
  draw.sectionPayloadOffset =
      sizeof(draw) + count * sizeof(D9CCommandChunkWireSectionDesc);
  if (!builder.beginRecord(type) || !builder.appendPayloadValue(draw)) {
    return false;
  }
  std::array<D9CCommandChunkWireSectionDesc,
             D9C_COMMAND_CHUNK_SECTION_COUNT> descs{};
  if (!builder.appendSectionTable(
          std::span<const D9CCommandChunkWireSectionDesc>(descs.data(),
                                                          count))) {
    return false;
  }

  std::uint32_t sectionIndex = 0u;
  const auto renderStates = source.fullSnapshot
      ? shadow.renderStateShadowTyped()
      : shadow.pendingRenderStatesTyped();
  if (renderStates.size() != source.renderStateCount ||
      !appendVisitedSection<D9CCommandChunkWireRenderState>(
          builder, D9C_COMMAND_CHUNK_SECTION_RENDER_STATE,
          source.renderStateCount,
          [&](const auto& emit) noexcept {
            bool ok = true;
            renderStates.forEach(
                [&](RenderStateSlot state, std::uint32_t value) noexcept {
                  ok = ok && emit(D9CCommandChunkWireRenderState{
                      .state = rawSlot(state), .value = value});
                });
            return ok;
          },
          descs, sectionIndex)) {
    builder.rollbackRecord();
    return false;
  }

  if (!appendVisitedSection<D9CCommandChunkWireTextureBinding>(
          builder, D9C_COMMAND_CHUNK_SECTION_TEXTURE,
          std::popcount(source.textureMask),
          [&](const auto& emit) noexcept {
            for (std::uint32_t slot = 0u;
                 slot < D9C_DRAW_PACKET_MAX_TEXTURES; ++slot) {
              if ((source.textureMask & (1u << slot)) == 0u) continue;
              D9CCommandChunkWireTextureBinding value{};
              value.slot = slot;
              value.valid = 1u;
              if (!appendNullableHandle(builder, bindings.textures[slot],
                                        D9C_CHUNK_HANDLE_KIND_TEXTURE,
                                        value.handleIndex) ||
                  !emit(value)) {
                return false;
              }
            }
            return true;
          },
          descs, sectionIndex) ||
      !appendVisitedSection<D9CCommandChunkWireStreamBinding>(
          builder, D9C_COMMAND_CHUNK_SECTION_STREAM,
          std::popcount(destination.streamMask),
          [&](const auto& emit) noexcept {
            for (std::uint32_t slot = 0u;
                 slot < D9C_DRAW_PACKET_MAX_STREAMS; ++slot) {
              if ((destination.streamMask & (1u << slot)) == 0u) continue;
              D9CCommandChunkWireStreamBinding value{};
              value.slot = slot;
              value.valid = 1u;
              value.offset = bindings.streams[slot].offset;
              value.stride = bindings.streams[slot].stride;
              if (!appendNullableHandle(builder, bindings.streams[slot].buffer,
                                        D9C_CHUNK_HANDLE_KIND_BUFFER,
                                        value.handleIndex) ||
                  !emit(value)) {
                return false;
              }
            }
            return true;
          },
          descs, sectionIndex) ||
      !appendVisitedSection<D9CCommandChunkWireShaderBinding>(
          builder, D9C_COMMAND_CHUNK_SECTION_SHADER,
          std::popcount(source.shaderMask),
          [&](const auto& emit) noexcept {
            for (std::uint32_t stage = 0u; stage < 2u; ++stage) {
              if ((source.shaderMask & (1u << stage)) == 0u) continue;
              D9CCommandChunkWireShaderBinding value{};
              value.stage = stage;
              value.valid = 1u;
              const auto& source = stage == 0u ? bindings.vs : bindings.ps;
              if (!appendNullableHandle(builder, source,
                                        D9C_CHUNK_HANDLE_KIND_SHADER,
                                        value.handleIndex) ||
                  !emit(value)) {
                return false;
              }
            }
            return true;
          },
          descs, sectionIndex)) {
    builder.rollbackRecord();
    return false;
  }

  if (!appendVisitedSection<D9CCommandChunkWireVertexInput>(
          builder, D9C_COMMAND_CHUNK_SECTION_VERTEX_INPUT,
          source.vertexInput ? 1u : 0u,
          [&](const auto& emit) noexcept {
            D9CCommandChunkWireVertexInput value{};
            value.valid = 1u;
            value.kind = source.vertexInputKind;
            value.value = bindings.fvf;
            value.handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX;
            if (value.kind ==
                    D9C_COMMAND_CHUNK_VERTEX_INPUT_DECLARATION &&
                !appendNullableHandle(builder, bindings.vdecl,
                                      D9C_CHUNK_HANDLE_KIND_VERTEX_DECL,
                                      value.handleIndex)) {
              return false;
            }
            return emit(value);
          },
          descs, sectionIndex) ||
      !appendVisitedSection<D9CCommandChunkWireIndexBinding>(
          builder, D9C_COMMAND_CHUNK_SECTION_INDEX_BUFFER,
          destination.indexBuffer ? 1u : 0u,
          [&](const auto& emit) noexcept {
            D9CCommandChunkWireIndexBinding value{};
            value.valid = 1u;
            return appendNullableHandle(builder, bindings.indexBuffer,
                                        D9C_CHUNK_HANDLE_KIND_BUFFER,
                                        value.handleIndex) && emit(value);
          },
          descs, sectionIndex) ||
      !appendVisitedSection<D9CCommandChunkWireRenderTargetBinding>(
          builder, D9C_COMMAND_CHUNK_SECTION_RENDER_TARGET,
          std::popcount(source.renderTargetMask),
          [&](const auto& emit) noexcept {
            for (std::uint32_t slot = 0u;
                 slot < D9C_DRAW_PACKET_MAX_RENDER_TARGETS; ++slot) {
              if ((source.renderTargetMask & (1u << slot)) == 0u) continue;
              D9CCommandChunkWireRenderTargetBinding value{};
              value.slot = slot;
              value.valid = 1u;
              if (!appendNullableHandle(builder, bindings.renderTargets[slot],
                                        D9C_CHUNK_HANDLE_KIND_SURFACE,
                                        value.handleIndex) ||
                  !emit(value)) {
                return false;
              }
            }
            return true;
          },
          descs, sectionIndex) ||
      !appendVisitedSection<D9CCommandChunkWireDepthStencilBinding>(
          builder, D9C_COMMAND_CHUNK_SECTION_DEPTH_STENCIL,
          source.depthStencil ? 1u : 0u,
          [&](const auto& emit) noexcept {
            D9CCommandChunkWireDepthStencilBinding value{};
            value.valid = 1u;
            return appendNullableHandle(builder, bindings.depthStencil,
                                        D9C_CHUNK_HANDLE_KIND_SURFACE,
                                        value.handleIndex) && emit(value);
          },
          descs, sectionIndex)) {
    builder.rollbackRecord();
    return false;
  }

  if (!appendVisitedSection<D9CViewport>(
          builder, D9C_COMMAND_CHUNK_SECTION_VIEWPORT,
          source.viewport ? 1u : 0u,
          [&](const auto& emit) noexcept {
            return emit(shadow.viewportShadow());
          }, descs, sectionIndex) ||
      !appendVisitedSection<D9CRect>(
          builder, D9C_COMMAND_CHUNK_SECTION_SCISSOR,
          source.scissor ? 1u : 0u,
          [&](const auto& emit) noexcept {
            return emit(shadow.scissorShadow());
          }, descs, sectionIndex) ||
      !appendVisitedSection<D9CMaterial>(
          builder, D9C_COMMAND_CHUNK_SECTION_MATERIAL,
          source.material ? 1u : 0u,
          [&](const auto& emit) noexcept {
            return emit(shadow.materialShadow());
          }, descs, sectionIndex) ||
      !appendVisitedSection<D9CCommandChunkWireClipPlane>(
          builder, D9C_COMMAND_CHUNK_SECTION_CLIP_PLANE,
          std::popcount(source.clipPlaneMask),
          [&](const auto& emit) noexcept {
            for (std::uint32_t slot = 0u; slot < 6u; ++slot) {
              if ((source.clipPlaneMask & (1u << slot)) == 0u) continue;
              D9CCommandChunkWireClipPlane value{};
              value.slot = slot;
              std::memcpy(value.values,
                          &shadow.clipPlaneShadow()[slot * 4u],
                          sizeof(value.values));
              if (!emit(value)) return false;
            }
            return true;
          }, descs, sectionIndex)) {
    builder.rollbackRecord();
    return false;
  }

  const auto textureStageStates = source.fullSnapshot
      ? shadow.tssShadowTyped()
      : shadow.pendingTssTyped();
  const auto samplerStates = source.fullSnapshot
      ? shadow.samplerStateShadowTyped()
      : shadow.pendingSamplerStatesTyped();
  const auto transforms = source.fullSnapshot
      ? shadow.transformShadowTyped()
      : shadow.pendingTransformsTyped();
  if (textureStageStates.size() != source.textureStageStateCount ||
      samplerStates.size() != source.samplerStateCount ||
      transforms.size() != source.transformCount ||
      !appendVisitedSection<D9CDrawPacketTextureStageState>(
          builder, D9C_COMMAND_CHUNK_SECTION_TEXTURE_STAGE_STATE,
          source.textureStageStateCount,
          [&](const auto& emit) noexcept {
            bool ok = true;
            textureStageStates.forEach(
                [&](TextureStageIndex stage, TextureStageStateType type,
                    std::uint32_t value) noexcept {
                  ok = ok && emit(D9CDrawPacketTextureStageState{
                      rawSlot(stage), rawSlot(type), value});
                });
            return ok;
          }, descs, sectionIndex) ||
      !appendVisitedSection<D9CDrawPacketSamplerState>(
          builder, D9C_COMMAND_CHUNK_SECTION_SAMPLER_STATE,
          source.samplerStateCount,
          [&](const auto& emit) noexcept {
            bool ok = true;
            samplerStates.forEach(
                [&](SamplerIndex sampler, SamplerStateType type,
                    std::uint32_t value) noexcept {
                  ok = ok && emit(D9CDrawPacketSamplerState{
                      rawSlot(sampler), rawSlot(type), value});
                });
            return ok;
          }, descs, sectionIndex) ||
      !appendVisitedSection<D9CDrawPacketTransform>(
          builder, D9C_COMMAND_CHUNK_SECTION_TRANSFORM,
          source.transformCount,
          [&](const auto& emit) noexcept {
            bool ok = true;
            transforms.forEach(
                [&](TransformState state, const D9CMatrix& matrix) noexcept {
                  ok = ok && emit(D9CDrawPacketTransform{
                      rawSlot(state), 0u, matrix});
                });
            return ok;
          }, descs, sectionIndex) ||
      !appendVisitedSection<D9CCommandChunkWireLight>(
          builder, D9C_COMMAND_CHUNK_SECTION_LIGHT,
          std::popcount(source.lightMask),
          [&](const auto& emit) noexcept {
            for (std::uint32_t slot = 0u;
                 slot < D9C_DRAW_PACKET_MAX_LIGHTS; ++slot) {
              if ((source.lightMask & (1u << slot)) == 0u) continue;
              if (!emit(D9CCommandChunkWireLight{
                      .slot = slot,
                      .reserved0 = 0u,
                      .light = shadow.lightShadow()[slot]})) {
                return false;
              }
            }
            return true;
          }, descs, sectionIndex) ||
      !appendVisitedSection<D9CCommandChunkWireLightEnable>(
          builder, D9C_COMMAND_CHUNK_SECTION_LIGHT_ENABLE,
          std::popcount(source.lightEnableMask),
          [&](const auto& emit) noexcept {
            const std::uint32_t lightEnableSource = source.fullSnapshot
                ? shadow.lightEnableShadow()
                : shadow.pendingLightEnableMask();
            for (std::uint32_t slot = 0u;
                 slot < D9C_DRAW_PACKET_MAX_LIGHTS; ++slot) {
              if ((source.lightEnableMask & (1u << slot)) == 0u) continue;
              if (!emit(D9CCommandChunkWireLightEnable{
                      .slot = slot,
                      .enabled = (lightEnableSource & (1u << slot)) != 0u})) {
                return false;
              }
            }
            return true;
          }, descs, sectionIndex) ||
      !appendConstantSection(builder, D9C_COMMAND_CHUNK_SECTION_VS_CONST_F,
                             source.vsFloatConstants, descs, sectionIndex) ||
      !appendConstantSection(builder, D9C_COMMAND_CHUNK_SECTION_VS_CONST_I,
                             source.vsIntConstants, descs, sectionIndex) ||
      !appendConstantSection(builder, D9C_COMMAND_CHUNK_SECTION_VS_CONST_B,
                             source.vsBoolConstants, descs, sectionIndex) ||
      !appendConstantSection(builder, D9C_COMMAND_CHUNK_SECTION_PS_CONST_F,
                             source.psFloatConstants, descs, sectionIndex) ||
      !appendConstantSection(builder, D9C_COMMAND_CHUNK_SECTION_PS_CONST_I,
                             source.psIntConstants, descs, sectionIndex) ||
      !appendConstantSection(builder, D9C_COMMAND_CHUNK_SECTION_PS_CONST_B,
                             source.psBoolConstants, descs, sectionIndex) ||
      !appendRawSection(builder, D9C_COMMAND_CHUNK_SECTION_UP_INDEX_DATA,
                        source.payloads.upIndex, descs, sectionIndex) ||
      !appendRawSection(builder, D9C_COMMAND_CHUNK_SECTION_UP_VERTEX_DATA,
                        source.payloads.upVertex, descs, sectionIndex)) {
    builder.rollbackRecord();
    return false;
  }

  if (sectionIndex != count ||
      !builder.overwriteSectionTable(
          draw.sectionTableOffset,
          std::span<const D9CCommandChunkWireSectionDesc>(descs.data(),
                                                          count)) ||
      !builder.commitRecord()) {
    builder.rollbackRecord();
    return false;
  }
  return true;
}

bool appendSparsePlanRecord(const RecorderLockCapability& access,
                            CommandChunkBuilder& builder,
                            std::uint32_t type,
                            std::uint32_t flags,
                            const SparseStatePlan& plan,
                            const SparseStatePlan::SourceFacts& source,
                            const SparseStatePlan::DestinationFacts& destination)
    noexcept {
  bool appended = false;
  return plan.withEmitSources(
      access,
      [&](const PeHotStateShadow& shadow,
          const PeBindingView& bindings) noexcept {
        appended = appendSparsePlanRecordWithSources(
            builder, type, flags, source, destination, shadow, bindings);
      }) && appended;
}

}  // namespace

bool appendSparseStatePlan(const RecorderLockCapability& access,
                           CommandChunkBuilder& builder,
                           std::uint32_t type,
                           const SparseStatePlan& plan) noexcept {
  const auto* const source = plan.source(access);
  return source && appendSparsePlanRecord(access, builder, type,
                                           source->drawFlags, plan, *source,
                                           plan.destination());
}

bool appendApplyStatePlan(const RecorderLockCapability& access,
                          CommandChunkBuilder& builder,
                          std::uint32_t flags,
                          const SparseStatePlan& plan) noexcept {
  const auto* const source = plan.source(access);
  return source &&
         appendSparsePlanRecord(access, builder,
                                D9C_COMMAND_RECORD_APPLY_STATE, flags, plan,
                                *source, plan.destination());
}

}  // namespace dxmt9::d3d9::pe
