#include "device_c_cpu_ready_plan.hpp"

#include <cstring>
#include <limits>

namespace dxmt9::d3d9 {
namespace {

bool addCount(std::size_t& target, std::size_t value) noexcept {
  if (value > std::numeric_limits<std::uint32_t>::max() - target) {
    return false;
  }
  target += value;
  return true;
}

bool multiplyU32Count(std::size_t count,
                      std::size_t elementSize,
                      std::size_t& result) noexcept {
  constexpr auto kMax =
      static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max());
  if (elementSize != 0 && count > kMax / elementSize) {
    return false;
  }
  result = count * elementSize;
  return true;
}

bool addAlignedBytes(std::size_t& target,
                     std::size_t byteCount,
                     std::size_t alignment) noexcept {
  constexpr auto kMax =
      static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max());
  const auto mask = alignment - 1;
  if (alignment == 0 || (alignment & mask) != 0 || target > kMax - mask) {
    return false;
  }
  const auto aligned = (target + mask) & ~mask;
  if (byteCount > kMax - aligned) {
    return false;
  }
  target = aligned + byteCount;
  return true;
}

template <typename T>
bool loadFixed(const ImportedRecordV2View& record, T& out) noexcept {
  if (record.payload.size() < sizeof(T)) {
    return false;
  }
  std::memcpy(&out, record.payload.data(), sizeof(T));
  return true;
}

V2CpuReadyPlan fallback(RawOrdinal rawOrdinal,
                        V2ReplayLane lane,
                        V2ReplayReason reason) noexcept {
  return V2CpuReadyPlan{
      .rawOrdinal = rawOrdinal,
      .lane = lane,
      .reason = reason,
  };
}

bool addDrawCapacities(core::SourcePayloadCapacity& capacity,
                       const ImportedRecordV2View& record) noexcept {
  if (!addCount(capacity.commandHeaders, 1) ||
      !addCount(capacity.drawHotStates, 1) ||
      !addCount(capacity.drawShaderLayouts, 1) ||
      !addCount(capacity.drawDebugSnapshots, 1) ||
      !addCount(capacity.drawPsoSubviews, 1) ||
      !addCount(capacity.drawUniformFixedPayloads, 1) ||
      !addCount(capacity.drawUniformVertexConstants, 1) ||
      !addCount(capacity.drawUniformPixelConstants, 1) ||
      !addCount(capacity.drawUniformPayloads, 1) ||
      !addCount(capacity.drawParams, 1) ||
      !addCount(capacity.drawRunRecords, 1)) {
    return false;
  }

  // Only raw UP ranges become draw payload bytes. State/constant sections are
  // materialized into their typed regions. Each independent append uses the
  // same maximum alignment as ArenaByteBuffer construction will use.
  for (std::size_t i = 0; i < record.sections.size(); ++i) {
    const auto section = record.section(i);
    if (section.payload.size() != section.descriptor.byteSize) {
      return false;
    }
    switch (section.descriptor.kind) {
    case D9C_COMMAND_CHUNK_V2_SECTION_UP_INDEX_DATA:
    case D9C_COMMAND_CHUNK_V2_SECTION_UP_VERTEX_DATA:
      if (!addAlignedBytes(capacity.drawPayloadBytes,
                           section.descriptor.byteSize,
                           kV2ArenaDrawPayloadAppendAlignment)) {
        return false;
      }
      break;
    default:
      break;
    }
  }
  return addAlignedBytes(capacity.drawPayloadBytes,
                         sizeof(core::DrawBindingOverride),
                         kV2ArenaDrawPayloadAppendAlignment) &&
         addAlignedBytes(capacity.drawPayloadBytes,
                         sizeof(core::DrawBindingSnapshot),
                         kV2ArenaDrawPayloadAppendAlignment);
}

}  // namespace

V2CpuReadyPlan planCpuReadyChunkV2(
    const ImportedChunkV2View& imported,
    RawOrdinal rawOrdinal,
    V2CpuReadyPlanOptions options) noexcept {
  if (rawOrdinal == 0 || options.pageSize == 0 || options.maxPages == 0 ||
      imported.records.size() != imported.header.recordCount) {
    return fallback(rawOrdinal, V2ReplayLane::Reject,
                    V2ReplayReason::InvalidImportedView);
  }

  V2CpuReadyPlan plan{
      .rawOrdinal = rawOrdinal,
      .lane = V2ReplayLane::DirectArenaCandidate,
      .reason = V2ReplayReason::Eligible,
  };
  std::size_t drawCount = 0;

  for (std::size_t i = 0; i < imported.records.size(); ++i) {
    const auto record = imported.record(i);
    if (record.header.type != imported.records[i].type ||
        record.payload.size() != imported.records[i].payloadSize) {
      return fallback(rawOrdinal, V2ReplayLane::Reject,
                      V2ReplayReason::InvalidImportedView);
    }

    switch (record.header.type) {
    case D9C_COMMAND_RECORD_DRAW_PRIMITIVE:
    case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE:
    case D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP:
    case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP: {
      D9CCommandChunkWireDrawHeaderV2 draw{};
      if (!loadFixed(record, draw) || draw.primitiveType < 1 ||
          draw.primitiveType > 6) {
        return fallback(rawOrdinal, V2ReplayLane::Reject,
                        V2ReplayReason::InvalidImportedView);
      }
      if (draw.primitiveType == 6) {
        return fallback(rawOrdinal, V2ReplayLane::Legacy,
                        V2ReplayReason::TriangleFan);
      }
      if (!addDrawCapacities(plan.capacity, record) ||
          !addCount(drawCount, 1)) {
        return fallback(rawOrdinal, V2ReplayLane::Reject,
                        V2ReplayReason::StructuralOverflow);
      }
      plan.logicalSource = true;
      break;
    }
    case D9C_COMMAND_RECORD_SET_VS_CONST_F:
    case D9C_COMMAND_RECORD_SET_VS_CONST_I:
    case D9C_COMMAND_RECORD_SET_VS_CONST_B:
      break;
    case D9C_COMMAND_RECORD_SET_PS_CONST_F:
    case D9C_COMMAND_RECORD_SET_PS_CONST_I:
    case D9C_COMMAND_RECORD_SET_PS_CONST_B:
      break;
    case D9C_COMMAND_RECORD_APPLY_STATE:
      break;
    case D9C_COMMAND_RECORD_CLEAR: {
      D9CCommandChunkWireClearV2 clear{};
      if (!loadFixed(record, clear)) {
        return fallback(rawOrdinal, V2ReplayLane::Reject,
                        V2ReplayReason::InvalidImportedView);
      }
      if (!addCount(plan.capacity.commandHeaders, 1) ||
          !addCount(plan.capacity.clearRecords, 1) ||
          !addCount(plan.capacity.clearRects, clear.rectCount)) {
        return fallback(rawOrdinal, V2ReplayLane::Reject,
                        V2ReplayReason::StructuralOverflow);
      }
      plan.logicalSource = true;
      break;
    }
    case D9C_COMMAND_RECORD_UPDATE_SURFACE:
      if (!addCount(plan.capacity.commandHeaders, 1) ||
          !addCount(plan.capacity.surfaceCopyRecords, 1)) {
        return fallback(rawOrdinal, V2ReplayLane::Reject,
                        V2ReplayReason::StructuralOverflow);
      }
      plan.logicalSource = true;
      break;
    case D9C_COMMAND_RECORD_STRETCH_RECT:
      if (!addCount(plan.capacity.commandHeaders, 1) ||
          !addCount(plan.capacity.stretchRectRecords, 1)) {
        return fallback(rawOrdinal, V2ReplayLane::Reject,
                        V2ReplayReason::StructuralOverflow);
      }
      plan.logicalSource = true;
      break;
    case D9C_COMMAND_RECORD_COLOR_FILL:
      if (!addCount(plan.capacity.commandHeaders, 1) ||
          !addCount(plan.capacity.colorFillRecords, 1)) {
        return fallback(rawOrdinal, V2ReplayLane::Reject,
                        V2ReplayReason::StructuralOverflow);
      }
      plan.logicalSource = true;
      break;
    case D9C_COMMAND_RECORD_RESZ_DEPTH_RESOLVE:
      if (!addCount(plan.capacity.commandHeaders, 1) ||
          !addCount(plan.capacity.depthResolveRecords, 1)) {
        return fallback(rawOrdinal, V2ReplayLane::Reject,
                        V2ReplayReason::StructuralOverflow);
      }
      plan.logicalSource = true;
      break;
    case D9C_COMMAND_RECORD_QUERY_ISSUE:
      return fallback(rawOrdinal, V2ReplayLane::Legacy,
                      V2ReplayReason::Query);
    case D9C_COMMAND_RECORD_READBACK:
      return fallback(rawOrdinal, V2ReplayLane::Inline,
                      V2ReplayReason::Readback);
    case D9C_COMMAND_RECORD_PRESENT:
      return fallback(rawOrdinal, V2ReplayLane::Legacy,
                      V2ReplayReason::Present);
    case D9C_COMMAND_RECORD_UPDATE_TEXTURE:
      return fallback(rawOrdinal, V2ReplayLane::Legacy,
                      V2ReplayReason::UpdateTexture);
    default:
      return fallback(rawOrdinal, V2ReplayLane::Reject,
                      V2ReplayReason::UnknownRecord);
    }
  }

  if (!plan.logicalSource) {
    plan.lane = V2ReplayLane::StateOnly;
    return plan;
  }

  std::size_t vertexConstantBytes = 0;
  std::size_t pixelConstantBytes = 0;
  if (!multiplyU32Count(drawCount, sizeof(core::VertexShaderConstants),
                        vertexConstantBytes) ||
      !multiplyU32Count(drawCount, sizeof(core::PixelShaderConstants),
                        pixelConstantBytes)) {
    return fallback(rawOrdinal, V2ReplayLane::Reject,
                    V2ReplayReason::StructuralOverflow);
  }
  plan.capacity.drawUniformVertexConstantBytes = vertexConstantBytes;
  plan.capacity.drawUniformPixelConstantBytes = pixelConstantBytes;

  const auto lookupBucketCount =
      core::detail::chunkSlotUniformLookupBucketCount(drawCount);
  plan.capacity.drawUniformPayloadLookupHeads = lookupBucketCount;
  plan.capacity.drawUniformPayloadLookupTails = lookupBucketCount;
  plan.capacity.drawUniformPayloadLookupNext = drawCount;
  plan.capacity.drawUniformVertexConstantsLookupHeads = lookupBucketCount;
  plan.capacity.drawUniformVertexConstantsLookupTails = lookupBucketCount;
  plan.capacity.drawUniformVertexConstantsLookupNext = drawCount;
  plan.capacity.drawUniformPixelConstantsLookupHeads = lookupBucketCount;
  plan.capacity.drawUniformPixelConstantsLookupTails = lookupBucketCount;
  plan.capacity.drawUniformPixelConstantsLookupNext = drawCount;

  const auto layout = core::makeSourcePayloadLayout(
      plan.capacity, options.pageSize,
      std::numeric_limits<std::uint32_t>::max());
  if (!layout) {
    return fallback(rawOrdinal, V2ReplayLane::Reject,
                    V2ReplayReason::StructuralOverflow);
  }
  if (layout->pageCount > options.maxPages) {
    return fallback(rawOrdinal, V2ReplayLane::Legacy,
                    V2ReplayReason::Oversize);
  }
  plan.layout = *layout;
  return plan;
}

}  // namespace dxmt9::d3d9
