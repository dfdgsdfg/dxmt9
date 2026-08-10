#include "device_c_chunk_replay.hpp"

#include <algorithm>
#include <cstring>

namespace dxmt9::d3d9 {

namespace {

template <typename T>
std::span<const T> typedSection(const ImportedSectionView& section) {
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

void* bindingObject(const ResolvedRecordView& record,
                    std::uint32_t handleIndex) {
  return record.objectForAbsoluteIndex(handleIndex);
}

dxmt9::core::PrimitiveType primitiveType(std::uint32_t value) {
  return static_cast<dxmt9::core::PrimitiveType>(value - 1u);
}

SparseDrawCall makeDrawCall(const ResolvedRecordView& record) {
  const auto& draw = record.wire.drawHeader;
  SparseDrawCall call{
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

bool isSparseRecord(std::uint32_t type) noexcept {
  const auto* rule = recordRule(type);
  return rule && (rule->ruleFlags & RecordRuleSparseState) != 0u;
}

std::int32_t replaySparseRecord(
    const ResolvedRecordView& record,
    SparseReplaySink& sink) noexcept {
  if (!isSparseRecord(record.wire.header.type) ||
      record.objects.size() != record.wire.header.handleCount ||
      std::any_of(record.objects.begin(), record.objects.end(),
                  [](const void* object) { return object == nullptr; })) {
    return kCommandChunkDecodeFailure;
  }

  const bool applyOnly =
      record.wire.header.type == D9C_COMMAND_RECORD_APPLY_STATE;
  auto call = applyOnly ? SparseDrawCall{} : makeDrawCall(record);
  for (std::size_t sectionIndex = 0u;
       sectionIndex < record.wire.sections.size(); ++sectionIndex) {
    const auto section = record.wire.section(sectionIndex);
    std::int32_t result = 0;
    switch (section.descriptor.kind) {
      case D9C_COMMAND_CHUNK_SECTION_RENDER_STATE:
        result = sink.setRenderStates(
            typedSection<D9CCommandChunkWireRenderState>(section));
        break;
      case D9C_COMMAND_CHUNK_SECTION_TEXTURE:
        for (const auto& value :
             typedSection<D9CCommandChunkWireTextureBinding>(section)) {
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
      case D9C_COMMAND_CHUNK_SECTION_STREAM:
        for (const auto& value :
             typedSection<D9CCommandChunkWireStreamBinding>(section)) {
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
      case D9C_COMMAND_CHUNK_SECTION_SHADER:
        for (const auto& value :
             typedSection<D9CCommandChunkWireShaderBinding>(section)) {
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
      case D9C_COMMAND_CHUNK_SECTION_VERTEX_INPUT: {
        const auto& value =
            typedSection<D9CCommandChunkWireVertexInput>(section).front();
        if (value.valid) {
          result = sink.setVertexInput(
              value.kind, value.value,
              bindingObject(record, value.handleIndex));
        }
        break;
      }
      case D9C_COMMAND_CHUNK_SECTION_INDEX_BUFFER: {
        const auto& value =
            typedSection<D9CCommandChunkWireIndexBinding>(section).front();
        if (value.valid) {
          result = sink.setIndexBuffer(
              bindingObject(record, value.handleIndex));
        }
        break;
      }
      case D9C_COMMAND_CHUNK_SECTION_RENDER_TARGET:
        for (const auto& value :
             typedSection<D9CCommandChunkWireRenderTargetBinding>(section)) {
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
      case D9C_COMMAND_CHUNK_SECTION_DEPTH_STENCIL: {
        const auto& value =
            typedSection<D9CCommandChunkWireDepthStencilBinding>(section)
                .front();
        if (value.valid) {
          result = sink.setDepthStencil(
              bindingObject(record, value.handleIndex));
        }
        break;
      }
      case D9C_COMMAND_CHUNK_SECTION_VIEWPORT:
        result = sink.setViewport(typedSection<D9CViewport>(section).front());
        break;
      case D9C_COMMAND_CHUNK_SECTION_SCISSOR:
        result = sink.setScissor(typedSection<D9CRect>(section).front());
        break;
      case D9C_COMMAND_CHUNK_SECTION_MATERIAL:
        result = sink.setMaterial(typedSection<D9CMaterial>(section).front());
        break;
      case D9C_COMMAND_CHUNK_SECTION_CLIP_PLANE:
        for (const auto& value :
             typedSection<D9CCommandChunkWireClipPlane>(section)) {
          result = sink.setClipPlane(value);
          if (failed(result)) {
            return result;
          }
        }
        break;
      case D9C_COMMAND_CHUNK_SECTION_TEXTURE_STAGE_STATE:
        result = sink.setTextureStageStates(
            typedSection<D9CDrawPacketTextureStageState>(section));
        break;
      case D9C_COMMAND_CHUNK_SECTION_SAMPLER_STATE:
        result = sink.setSamplerStates(
            typedSection<D9CDrawPacketSamplerState>(section));
        break;
      case D9C_COMMAND_CHUNK_SECTION_TRANSFORM:
        result = sink.setTransforms(
            typedSection<D9CDrawPacketTransform>(section));
        break;
      case D9C_COMMAND_CHUNK_SECTION_LIGHT:
        result = sink.setLights(
            typedSection<D9CCommandChunkWireLight>(section));
        break;
      case D9C_COMMAND_CHUNK_SECTION_LIGHT_ENABLE:
        result = sink.setLightEnables(
            typedSection<D9CCommandChunkWireLightEnable>(section));
        break;
      case D9C_COMMAND_CHUNK_SECTION_VS_CONST_F:
      case D9C_COMMAND_CHUNK_SECTION_VS_CONST_I:
      case D9C_COMMAND_CHUNK_SECTION_VS_CONST_B:
      case D9C_COMMAND_CHUNK_SECTION_PS_CONST_F:
      case D9C_COMMAND_CHUNK_SECTION_PS_CONST_I:
      case D9C_COMMAND_CHUNK_SECTION_PS_CONST_B: {
        D9CCommandChunkWireConstantRange range{};
        if (!load(section.payload, range)) {
          return kCommandChunkDecodeFailure;
        }
        result = sink.setConstants(
            section.descriptor.kind, range,
            section.payload.subspan(sizeof(range)));
        break;
      }
      case D9C_COMMAND_CHUNK_SECTION_UP_INDEX_DATA:
        call.payload.userIndexData =
            std::span<const dxmt9::core::u8>(
                reinterpret_cast<const dxmt9::core::u8*>(
                    section.payload.data()),
                section.payload.size());
        break;
      case D9C_COMMAND_CHUNK_SECTION_UP_VERTEX_DATA:
        call.payload.userVertexData =
            std::span<const dxmt9::core::u8>(
                reinterpret_cast<const dxmt9::core::u8*>(
                    section.payload.data()),
                section.payload.size());
        break;
      default:
        return kCommandChunkDecodeFailure;
    }
    if (failed(result)) {
      return result;
    }
  }

  return applyOnly ? sink.finishApplyState(record.wire.drawHeader.flags)
                   : sink.draw(call);
}

}  // namespace dxmt9::d3d9
