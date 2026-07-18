#include "device_c_chunk_v2_replay.hpp"

#include <algorithm>
#include <cstring>

namespace dxmt9::d3d9 {

namespace {

template <typename T>
std::span<const T> typedSection(const ImportedSectionV2View& section) {
  return std::span<const T>(
      reinterpret_cast<const T*>(section.payload.data()),
      section.descriptor.count);
}

template <typename T>
bool load(std::span<const std::byte> bytes, T& value) {
  if (bytes.size() < sizeof(T)) {
    return false;
  }
  std::memcpy(&value, bytes.data(), sizeof(T));
  return true;
}

bool failed(std::int32_t result) {
  return result < 0;
}

void* bindingObject(const ResolvedRecordV2View& record,
                    std::uint32_t handleIndex) {
  return record.objectForAbsoluteIndex(handleIndex);
}

dxmt9::core::PrimitiveType primitiveType(std::uint32_t value) {
  return static_cast<dxmt9::core::PrimitiveType>(value - 1u);
}

SparseDrawCallV2 makeDrawCall(const ResolvedRecordV2View& record) {
  const auto& draw = record.wire.drawHeader;
  SparseDrawCallV2 call{
      .flags = draw.flags,
      .minVertex = draw.minVertex,
      .numVertices = draw.numVertices,
      .stride = draw.stride,
      .indexFormat = draw.indexFormat,
  };
  call.param.primitiveType = primitiveType(draw.primitiveType);
  call.param.primitiveCount = draw.primitiveCount;
  call.param.startVertex = draw.startVertex;
  call.param.baseVertexIndex = draw.baseVertex;
  call.param.startIndex = draw.startIndex;
  call.param.indexed =
      record.wire.header.type == D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE ||
      record.wire.header.type ==
          D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP;
  if (draw.indexFormat == 102u) {
    call.param.indexType = dxmt9::core::IndexType::UInt32;
  }
  return call;
}

}  // namespace

bool isSparseRecordV2(std::uint32_t type) noexcept {
  const auto* rule = v2RecordRule(type);
  return rule && (rule->ruleFlags & V2RecordRuleSparseState) != 0u;
}

std::int32_t replaySparseRecordV2(
    const ResolvedRecordV2View& record,
    SparseReplaySinkV2& sink) noexcept {
  if (!isSparseRecordV2(record.wire.header.type) ||
      record.objects.size() != record.wire.header.handleCount ||
      std::any_of(record.objects.begin(), record.objects.end(),
                  [](const void* object) { return object == nullptr; })) {
    return kCommandChunkV2DecodeFailure;
  }

  const bool applyOnly =
      record.wire.header.type == D9C_COMMAND_RECORD_APPLY_STATE;
  auto call = applyOnly ? SparseDrawCallV2{} : makeDrawCall(record);
  for (std::size_t sectionIndex = 0u;
       sectionIndex < record.wire.sections.size(); ++sectionIndex) {
    const auto section = record.wire.section(sectionIndex);
    std::int32_t result = 0;
    switch (section.descriptor.kind) {
      case D9C_COMMAND_CHUNK_V2_SECTION_RENDER_STATE:
        result = sink.setRenderStates(
            typedSection<D9CCommandChunkWireRenderStateV2>(section));
        break;
      case D9C_COMMAND_CHUNK_V2_SECTION_TEXTURE:
        for (const auto& value :
             typedSection<D9CCommandChunkWireTextureBindingV2>(section)) {
          if (!value.valid) {
            continue;
          }
          result = sink.setTexture(
              value.slot, bindingObject(record, value.handleIndex));
          if (failed(result)) {
            return result;
          }
        }
        break;
      case D9C_COMMAND_CHUNK_V2_SECTION_STREAM:
        for (const auto& value :
             typedSection<D9CCommandChunkWireStreamBindingV2>(section)) {
          if (!value.valid) {
            continue;
          }
          result = sink.setStream(
              value, bindingObject(record, value.handleIndex));
          if (failed(result)) {
            return result;
          }
        }
        break;
      case D9C_COMMAND_CHUNK_V2_SECTION_SHADER:
        for (const auto& value :
             typedSection<D9CCommandChunkWireShaderBindingV2>(section)) {
          if (!value.valid) {
            continue;
          }
          result = sink.setShader(
              value.stage, bindingObject(record, value.handleIndex));
          if (failed(result)) {
            return result;
          }
        }
        break;
      case D9C_COMMAND_CHUNK_V2_SECTION_VERTEX_INPUT: {
        const auto& value =
            typedSection<D9CCommandChunkWireVertexInputV2>(section).front();
        if (value.valid) {
          result = sink.setVertexInput(
              value.kind, value.value,
              bindingObject(record, value.handleIndex));
        }
        break;
      }
      case D9C_COMMAND_CHUNK_V2_SECTION_INDEX_BUFFER: {
        const auto& value =
            typedSection<D9CCommandChunkWireIndexBindingV2>(section).front();
        if (value.valid) {
          result = sink.setIndexBuffer(
              bindingObject(record, value.handleIndex));
        }
        break;
      }
      case D9C_COMMAND_CHUNK_V2_SECTION_RENDER_TARGET:
        for (const auto& value :
             typedSection<D9CCommandChunkWireRenderTargetBindingV2>(section)) {
          if (!value.valid) {
            continue;
          }
          result = sink.setRenderTarget(
              value.slot, bindingObject(record, value.handleIndex));
          if (failed(result)) {
            return result;
          }
        }
        break;
      case D9C_COMMAND_CHUNK_V2_SECTION_DEPTH_STENCIL: {
        const auto& value =
            typedSection<D9CCommandChunkWireDepthStencilBindingV2>(section)
                .front();
        if (value.valid) {
          result = sink.setDepthStencil(
              bindingObject(record, value.handleIndex));
        }
        break;
      }
      case D9C_COMMAND_CHUNK_V2_SECTION_VIEWPORT:
        result = sink.setViewport(typedSection<D9CViewport>(section).front());
        break;
      case D9C_COMMAND_CHUNK_V2_SECTION_SCISSOR:
        result = sink.setScissor(typedSection<D9CRect>(section).front());
        break;
      case D9C_COMMAND_CHUNK_V2_SECTION_MATERIAL:
        result = sink.setMaterial(typedSection<D9CMaterial>(section).front());
        break;
      case D9C_COMMAND_CHUNK_V2_SECTION_CLIP_PLANE:
        for (const auto& value :
             typedSection<D9CCommandChunkWireClipPlaneV2>(section)) {
          result = sink.setClipPlane(value);
          if (failed(result)) {
            return result;
          }
        }
        break;
      case D9C_COMMAND_CHUNK_V2_SECTION_TEXTURE_STAGE_STATE:
        result = sink.setTextureStageStates(
            typedSection<D9CDrawPacketTextureStageState>(section));
        break;
      case D9C_COMMAND_CHUNK_V2_SECTION_SAMPLER_STATE:
        result = sink.setSamplerStates(
            typedSection<D9CDrawPacketSamplerState>(section));
        break;
      case D9C_COMMAND_CHUNK_V2_SECTION_TRANSFORM:
        result = sink.setTransforms(
            typedSection<D9CDrawPacketTransform>(section));
        break;
      case D9C_COMMAND_CHUNK_V2_SECTION_LIGHT:
        result = sink.setLights(
            typedSection<D9CCommandChunkWireLightV2>(section));
        break;
      case D9C_COMMAND_CHUNK_V2_SECTION_LIGHT_ENABLE:
        result = sink.setLightEnables(
            typedSection<D9CCommandChunkWireLightEnableV2>(section));
        break;
      case D9C_COMMAND_CHUNK_V2_SECTION_VS_CONST_F:
      case D9C_COMMAND_CHUNK_V2_SECTION_VS_CONST_I:
      case D9C_COMMAND_CHUNK_V2_SECTION_VS_CONST_B:
      case D9C_COMMAND_CHUNK_V2_SECTION_PS_CONST_F:
      case D9C_COMMAND_CHUNK_V2_SECTION_PS_CONST_I:
      case D9C_COMMAND_CHUNK_V2_SECTION_PS_CONST_B: {
        D9CCommandChunkWireConstantRangeV2 range{};
        if (!load(section.payload, range)) {
          return kCommandChunkV2DecodeFailure;
        }
        result = sink.setConstants(
            section.descriptor.kind, range,
            section.payload.subspan(sizeof(range)));
        break;
      }
      case D9C_COMMAND_CHUNK_V2_SECTION_UP_INDEX_DATA:
        call.payload.userIndexData =
            std::span<const dxmt9::core::u8>(
                reinterpret_cast<const dxmt9::core::u8*>(
                    section.payload.data()),
                section.payload.size());
        break;
      case D9C_COMMAND_CHUNK_V2_SECTION_UP_VERTEX_DATA:
        call.payload.userVertexData =
            std::span<const dxmt9::core::u8>(
                reinterpret_cast<const dxmt9::core::u8*>(
                    section.payload.data()),
                section.payload.size());
        break;
      default:
        return kCommandChunkV2DecodeFailure;
    }
    if (failed(result)) {
      return result;
    }
  }

  return applyOnly ? sink.finishApplyState(record.wire.drawHeader.flags)
                   : sink.draw(call);
}

}  // namespace dxmt9::d3d9
