#include "device_c_cpu_ready_plan.hpp"
#include "device_c_ordered_control.hpp"

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
bool loadFixed(const ImportedRecordView& record, T& out) noexcept {
  if (record.payload.size() < sizeof(T)) {
    return false;
  }
  std::memcpy(&out, record.payload.data(), sizeof(T));
  return true;
}

CpuReadyPlan fallback(RawOrdinal rawOrdinal,
                        ReplayLane lane,
                        ReplayReason reason,
                        bool containsOrderedControls = false) noexcept {
  return CpuReadyPlan{
      .rawOrdinal = rawOrdinal,
      .lane = lane,
      .reason = reason,
      .containsOrderedControls = containsOrderedControls,
  };
}

bool addTriangleFanPayloadCapacity(
    core::SourcePayloadCapacity& capacity,
    const ImportedRecordView& record,
    const D9CCommandChunkWireDrawHeader& draw) noexcept {
  if (draw.primitiveCount >
      std::numeric_limits<std::uint32_t>::max() / 3u) {
    return false;
  }
  const std::size_t triangleElementCount =
      static_cast<std::size_t>(draw.primitiveCount) * 3u;
  std::size_t elementSize = 0;
  switch (record.header.type) {
  case D9C_COMMAND_RECORD_DRAW_PRIMITIVE:
    elementSize = draw.primitiveCount <=
                          std::numeric_limits<std::uint16_t>::max() - 1u
                      ? sizeof(std::uint16_t)
                      : sizeof(std::uint32_t);
    break;
  case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE:
    // The sparse wire form inherits the bound index-buffer format. Plan the
    // UInt32 decomposition so structural admission is independent of replay
    // state.
    elementSize = sizeof(std::uint32_t);
    break;
  case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP:
    elementSize = draw.indexFormat == 102u ? sizeof(std::uint32_t)
                                           : sizeof(std::uint16_t);
    break;
  case D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP:
    elementSize = draw.stride;
    break;
  default:
    return false;
  }
  std::size_t byteCount = 0;
  return multiplyU32Count(triangleElementCount, elementSize, byteCount) &&
         (byteCount == 0 ||
          addAlignedBytes(capacity.drawPayloadBytes, byteCount,
                          kArenaDrawPayloadAppendAlignment));
}

bool addDrawCapacities(
    core::SourcePayloadCapacity& capacity,
    const ImportedRecordView& record,
    const D9CCommandChunkWireDrawHeader& draw) noexcept {
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
    case D9C_COMMAND_CHUNK_SECTION_UP_INDEX_DATA:
      if (draw.primitiveType == 6 &&
          record.header.type ==
              D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP) {
        break;
      }
      [[fallthrough]];
    case D9C_COMMAND_CHUNK_SECTION_UP_VERTEX_DATA:
      if (draw.primitiveType == 6 &&
          record.header.type == D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP) {
        break;
      }
      if (!addAlignedBytes(capacity.drawPayloadBytes,
                           section.descriptor.byteSize,
                           kArenaDrawPayloadAppendAlignment)) {
        return false;
      }
      break;
    default:
      break;
    }
  }
  return (draw.primitiveType != 6 ||
          addTriangleFanPayloadCapacity(capacity, record, draw)) &&
         addAlignedBytes(capacity.drawPayloadBytes,
                         sizeof(core::DrawBindingOverride),
                         kArenaDrawPayloadAppendAlignment) &&
         addAlignedBytes(capacity.drawPayloadBytes,
                         sizeof(core::DrawBindingSnapshot),
                         kArenaDrawPayloadAppendAlignment);
}

enum class DirectRecordResult {
  StateOnly,
  GpuProducing,
  Invalid,
  Overflow,
  Unsupported,
};

DirectRecordResult addDirectRecordCapacity(
    core::SourcePayloadCapacity& capacity,
    std::size_t& drawCount,
    const ImportedRecordView& record) noexcept {
  switch (record.header.type) {
  case D9C_COMMAND_RECORD_DRAW_PRIMITIVE:
  case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE:
  case D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP:
  case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP: {
    D9CCommandChunkWireDrawHeader draw{};
    if (!loadFixed(record, draw) || draw.primitiveType < 1 ||
        draw.primitiveType > 6) {
      return DirectRecordResult::Invalid;
    }
    if (!addDrawCapacities(capacity, record, draw) ||
        !addCount(drawCount, 1)) {
      return DirectRecordResult::Overflow;
    }
    return DirectRecordResult::GpuProducing;
  }
  case D9C_COMMAND_RECORD_SET_VS_CONST_F:
  case D9C_COMMAND_RECORD_SET_VS_CONST_I:
  case D9C_COMMAND_RECORD_SET_VS_CONST_B:
  case D9C_COMMAND_RECORD_SET_PS_CONST_F:
  case D9C_COMMAND_RECORD_SET_PS_CONST_I:
  case D9C_COMMAND_RECORD_SET_PS_CONST_B:
  case D9C_COMMAND_RECORD_APPLY_STATE:
    return DirectRecordResult::StateOnly;
  case D9C_COMMAND_RECORD_CLEAR: {
    D9CCommandChunkWireClear clear{};
    if (!loadFixed(record, clear)) {
      return DirectRecordResult::Invalid;
    }
    if (!addCount(capacity.commandHeaders, 1) ||
        !addCount(capacity.clearRecords, 1) ||
        !addCount(capacity.clearRects, clear.rectCount)) {
      return DirectRecordResult::Overflow;
    }
    return DirectRecordResult::GpuProducing;
  }
  case D9C_COMMAND_RECORD_UPDATE_SURFACE:
    return addCount(capacity.commandHeaders, 1) &&
                   addCount(capacity.surfaceCopyRecords, 1)
               ? DirectRecordResult::GpuProducing
               : DirectRecordResult::Overflow;
  case D9C_COMMAND_RECORD_STRETCH_RECT:
    return addCount(capacity.commandHeaders, 1) &&
                   addCount(capacity.stretchRectRecords, 1)
               ? DirectRecordResult::GpuProducing
               : DirectRecordResult::Overflow;
  case D9C_COMMAND_RECORD_COLOR_FILL:
    return addCount(capacity.commandHeaders, 1) &&
                   addCount(capacity.colorFillRecords, 1)
               ? DirectRecordResult::GpuProducing
               : DirectRecordResult::Overflow;
  case D9C_COMMAND_RECORD_RESZ_DEPTH_RESOLVE:
    return addCount(capacity.commandHeaders, 1) &&
                   addCount(capacity.depthResolveRecords, 1)
               ? DirectRecordResult::GpuProducing
               : DirectRecordResult::Overflow;
  case D9C_COMMAND_RECORD_PRESENT: {
    D9CCommandChunkWirePresent present{};
    if (!loadFixed(record, present)) {
      return DirectRecordResult::Invalid;
    }
    return addCount(capacity.commandHeaders, 1) &&
                   addCount(capacity.presentRecords, 1)
               ? DirectRecordResult::GpuProducing
               : DirectRecordResult::Overflow;
  }
  default:
    return DirectRecordResult::Unsupported;
  }
}

bool addDerivedDrawCapacity(core::SourcePayloadCapacity& capacity,
                            std::size_t drawCount) noexcept {
  std::size_t vertexConstantBytes = 0;
  std::size_t pixelConstantBytes = 0;
  if (!multiplyU32Count(drawCount, sizeof(core::VertexShaderConstants),
                        vertexConstantBytes) ||
      !multiplyU32Count(drawCount, sizeof(core::PixelShaderConstants),
                        pixelConstantBytes)) {
    return false;
  }
  capacity.drawUniformVertexConstantBytes = vertexConstantBytes;
  capacity.drawUniformPixelConstantBytes = pixelConstantBytes;

  const auto lookupBucketCount =
      core::detail::chunkSlotUniformLookupBucketCount(drawCount);
  capacity.drawUniformPayloadLookupHeads = lookupBucketCount;
  capacity.drawUniformPayloadLookupTails = lookupBucketCount;
  capacity.drawUniformPayloadLookupNext = drawCount;
  capacity.drawUniformVertexConstantsLookupHeads = lookupBucketCount;
  capacity.drawUniformVertexConstantsLookupTails = lookupBucketCount;
  capacity.drawUniformVertexConstantsLookupNext = drawCount;
  capacity.drawUniformPixelConstantsLookupHeads = lookupBucketCount;
  capacity.drawUniformPixelConstantsLookupTails = lookupBucketCount;
  capacity.drawUniformPixelConstantsLookupNext = drawCount;
  return true;
}

std::optional<core::SourcePayloadLayout> makeSegmentLayout(
    const core::SourcePayloadCapacity& baseCapacity,
    std::size_t drawCount,
    std::size_t pageSize) noexcept {
  auto capacity = baseCapacity;
  if (!addDerivedDrawCapacity(capacity, drawCount)) {
    return std::nullopt;
  }
  return core::makeSourcePayloadLayout(
      capacity, pageSize, std::numeric_limits<std::uint32_t>::max());
}

struct SegmentAccumulator {
  core::SourcePayloadCapacity capacity{};
  std::size_t drawCount = 0;
  std::size_t firstRecordIndex = 0;
  std::size_t lastRecordIndex = 0;
  bool active = false;
};

}  // namespace

CpuReadyPlan planCpuReadyChunk(
    const ImportedChunkView& imported,
    RawOrdinal rawOrdinal,
    CpuReadyPlanOptions options) noexcept {
  if (rawOrdinal == 0 || options.pageSize == 0 || options.maxPages == 0 ||
      options.maxOrdinaryPagesPerSegment == 0 ||
      options.maxSourcesPerChunk == 0 ||
      options.maxSegmentsPerSource == 0 ||
      options.maxSegmentsPerSource >
          core::kMaxArenaSourcePayloadSegments ||
      options.maxPagesPerSource == 0 ||
      imported.records.size() != imported.header.recordCount) {
    return fallback(rawOrdinal, ReplayLane::Reject,
                    ReplayReason::InvalidImportedView);
  }

  // Ordered controls force whole-raw compatibility replay, but every control
  // in the immutable stream must be structurally valid before replay applies
  // any D3D or Metal effect. Keep this preflight allocation-free and retain
  // only the routing fact; replay rebuilds each exact-index disposition.
  bool containsOrderedControls = false;
  ReplayLane orderedLane = ReplayLane::Legacy;
  ReplayReason orderedReason = ReplayReason::Eligible;
  for (std::size_t i = 0; i < imported.records.size(); ++i) {
    const auto record = imported.record(i);
    if (record.header.type != imported.records[i].type ||
        record.payload.size() != imported.records[i].payloadSize) {
      return fallback(rawOrdinal, ReplayLane::Reject,
                      ReplayReason::InvalidImportedView);
    }

    ReplayReason reason = ReplayReason::Eligible;
    switch (record.header.type) {
    case D9C_COMMAND_RECORD_QUERY_ISSUE:
      reason = ReplayReason::Query;
      break;
    case D9C_COMMAND_RECORD_READBACK:
      reason = ReplayReason::Readback;
      break;
    case D9C_COMMAND_RECORD_UPDATE_TEXTURE:
      reason = ReplayReason::UpdateTexture;
      break;
    default:
      continue;
    }
    if (!makeOrderedControlDisposition(record, rawOrdinal, i)) {
      return fallback(rawOrdinal, ReplayLane::Reject,
                      ReplayReason::InvalidImportedView);
    }
    if (!containsOrderedControls) {
      orderedReason = reason;
    }
    containsOrderedControls = true;
    if (reason == ReplayReason::Readback) {
      // Any synchronous observation makes the complete raw an Inline lane,
      // even if an earlier Query first selected compatibility replay.
      orderedLane = ReplayLane::Inline;
      orderedReason = reason;
    }
  }
  if (containsOrderedControls) {
    return fallback(rawOrdinal, orderedLane, orderedReason, true);
  }

  CpuReadyPlan plan{
      .rawOrdinal = rawOrdinal,
      .lane = ReplayLane::DirectArenaCandidate,
      .reason = ReplayReason::Eligible,
  };
  try {
    // At most one physical segment can be committed by each raw record, and
    // at most one source can own each non-empty segment. Reserve only against
    // the immutable producer view; no plan-sized object lives on the stack.
    plan.segments.reserve(imported.records.size());
    plan.sources.reserve(
        std::min(options.maxSourcesPerChunk, imported.records.size()));
  } catch (...) {
    return fallback(rawOrdinal, ReplayLane::Reject,
                    ReplayReason::StructuralOverflow);
  }
  const std::size_t maxPagesPerSource =
      std::min(options.maxPages, options.maxPagesPerSource);
  std::size_t totalDrawCount = 0;
  SegmentAccumulator current{};
  bool sawPresent = false;
  // State-only records after the previous segment's final GPU record stay
  // pending until the next GPU record fixes their construction segment.
  std::size_t nextSegmentRecordIndex = 0;

  const auto appendSegment = [&](const SegmentAccumulator& segment,
                                 bool jumbo) noexcept {
    if (!segment.active) {
      return false;
    }
    auto capacity = segment.capacity;
    if (!addDerivedDrawCapacity(capacity, segment.drawCount)) {
      return false;
    }
    const auto layout = core::makeSourcePayloadLayout(
        capacity, options.pageSize,
        std::numeric_limits<std::uint32_t>::max());
    if (!layout || segment.firstRecordIndex >
                       std::numeric_limits<std::uint32_t>::max() ||
        segment.lastRecordIndex < segment.firstRecordIndex ||
        segment.lastRecordIndex - segment.firstRecordIndex >=
            std::numeric_limits<std::uint32_t>::max()) {
      return false;
    }
    plan.segments.push_back(CpuReadySegmentPlan{
        .firstRecordIndex =
            static_cast<std::uint32_t>(segment.firstRecordIndex),
        .recordCount = static_cast<std::uint32_t>(
            segment.lastRecordIndex - segment.firstRecordIndex + 1),
        .jumbo = jumbo,
        .capacity = capacity,
        .layout = *layout,
    });
    ++plan.segmentCount;
    return true;
  };

  for (std::size_t i = 0; i < imported.records.size(); ++i) {
    const auto record = imported.record(i);
    if (record.header.type != imported.records[i].type ||
        record.payload.size() != imported.records[i].payloadSize) {
      return fallback(rawOrdinal, ReplayLane::Reject,
                      ReplayReason::InvalidImportedView);
    }

    if (record.header.type == D9C_COMMAND_RECORD_PRESENT) {
      if (sawPresent || i + 1u != imported.records.size()) {
        return fallback(rawOrdinal, ReplayLane::Legacy,
                        ReplayReason::Present);
      }
      sawPresent = true;
    }

    auto aggregateCapacity = plan.capacity;
    std::size_t aggregateDrawCount = totalDrawCount;
    const auto aggregateResult = addDirectRecordCapacity(
        aggregateCapacity, aggregateDrawCount, record);
    if (aggregateResult == DirectRecordResult::Invalid) {
      return fallback(rawOrdinal, ReplayLane::Reject,
                      ReplayReason::InvalidImportedView);
    }
    if (aggregateResult == DirectRecordResult::Overflow) {
      return fallback(rawOrdinal, ReplayLane::Reject,
                      ReplayReason::StructuralOverflow);
    }
    if (aggregateResult == DirectRecordResult::Unsupported) {
      return fallback(rawOrdinal, ReplayLane::Reject,
                      ReplayReason::UnknownRecord);
    }
    plan.capacity = aggregateCapacity;
    totalDrawCount = aggregateDrawCount;
    if (aggregateResult == DirectRecordResult::StateOnly) {
      continue;
    }

    auto candidate = current;
    if (!candidate.active) {
      candidate.active = true;
      candidate.firstRecordIndex = nextSegmentRecordIndex;
    }
    candidate.lastRecordIndex = i;
    const auto candidateResult = addDirectRecordCapacity(
        candidate.capacity, candidate.drawCount, record);
    if (candidateResult != DirectRecordResult::GpuProducing) {
      return fallback(rawOrdinal, ReplayLane::Reject,
                      ReplayReason::StructuralOverflow);
    }
    auto candidateLayout = makeSegmentLayout(
        candidate.capacity, candidate.drawCount, options.pageSize);
    if (!candidateLayout) {
      return fallback(rawOrdinal, ReplayLane::Reject,
                      ReplayReason::StructuralOverflow);
    }
    if (candidateLayout->pageCount <=
        options.maxOrdinaryPagesPerSegment) {
      current = candidate;
      plan.logicalSource = true;
      continue;
    }

    if (current.active && !appendSegment(current, false)) {
      return fallback(rawOrdinal, ReplayLane::Legacy,
                      ReplayReason::Oversize);
    }
    if (current.active) {
      nextSegmentRecordIndex = current.lastRecordIndex + 1u;
    }
    SegmentAccumulator indivisible{};
    indivisible.active = true;
    indivisible.firstRecordIndex = nextSegmentRecordIndex;
    indivisible.lastRecordIndex = i;
    if (addDirectRecordCapacity(indivisible.capacity,
                                indivisible.drawCount,
                                record) !=
        DirectRecordResult::GpuProducing) {
      return fallback(rawOrdinal, ReplayLane::Reject,
                      ReplayReason::StructuralOverflow);
    }
    const auto indivisibleLayout = makeSegmentLayout(
        indivisible.capacity, indivisible.drawCount, options.pageSize);
    if (!indivisibleLayout) {
      return fallback(rawOrdinal, ReplayLane::Reject,
                      ReplayReason::StructuralOverflow);
    }
    if (indivisibleLayout->pageCount >
            options.maxOrdinaryPagesPerSegment) {
      if (indivisibleLayout->pageCount > maxPagesPerSource ||
          !appendSegment(indivisible, true)) {
        return fallback(rawOrdinal, ReplayLane::Legacy,
                        ReplayReason::Oversize);
      }
      nextSegmentRecordIndex = i + 1u;
      current = {};
    } else {
      current = indivisible;
    }
    plan.logicalSource = true;
  }

  if (!plan.logicalSource) {
    plan.lane = ReplayLane::StateOnly;
    return plan;
  }

  if (current.active) {
    current.lastRecordIndex = imported.records.size() - 1u;
    if (!appendSegment(current, false)) {
      return fallback(rawOrdinal, ReplayLane::Legacy,
                      ReplayReason::Oversize);
    }
  } else if (nextSegmentRecordIndex < imported.records.size()) {
    auto& tail = plan.segments[plan.segmentCount - 1u];
    const std::size_t tailCount =
        imported.records.size() - tail.firstRecordIndex;
    if (tailCount > std::numeric_limits<std::uint32_t>::max()) {
      return fallback(rawOrdinal, ReplayLane::Reject,
                      ReplayReason::StructuralOverflow);
    }
    tail.recordCount = static_cast<std::uint32_t>(tailCount);
  }
  if (!addDerivedDrawCapacity(plan.capacity, totalDrawCount)) {
    return fallback(rawOrdinal, ReplayLane::Reject,
                    ReplayReason::StructuralOverflow);
  }
  // Group the already planned physical blocks into bounded logical sources.
  // The old one-source behavior is the default; source segmentation is an
  // explicit planner capability and does not alter the physical block plans.
  std::size_t sourceSegmentBegin = 0;
  while (sourceSegmentBegin < plan.segmentCount) {
    if (plan.sourceCount >= options.maxSourcesPerChunk) {
      return fallback(rawOrdinal, ReplayLane::Legacy,
                      ReplayReason::Oversize);
    }
    std::size_t sourceSegmentEnd = sourceSegmentBegin;
    std::array<core::SourcePayloadLayout,
               core::kMaxArenaSourcePayloadSegments>
        sourceLayouts{};
    std::optional<core::ArenaSourcePayloadLayout> sourceLayout;
    while (sourceSegmentEnd < plan.segmentCount &&
           sourceSegmentEnd - sourceSegmentBegin <
               options.maxSegmentsPerSource) {
      const auto candidateCount = sourceSegmentEnd - sourceSegmentBegin + 1u;
      for (std::size_t i = 0; i < candidateCount; ++i) {
        sourceLayouts[i] =
            plan.segments[sourceSegmentBegin + i].layout;
      }
      const auto candidate = core::makeArenaSourcePayloadLayout(
          std::span(sourceLayouts).first(candidateCount), options.pageSize,
          maxPagesPerSource);
      if (!candidate) {
        break;
      }
      sourceLayout = *candidate;
      ++sourceSegmentEnd;
    }
    if (!sourceLayout || sourceSegmentEnd == sourceSegmentBegin) {
      // Each physical block was checked against maxPagesPerSource when it
      // was formed. Reaching this branch means the bounded planner cannot
      // represent the requested source grouping.
      return fallback(rawOrdinal, ReplayLane::Legacy,
                      ReplayReason::Oversize);
    }

    const auto& first = plan.segments[sourceSegmentBegin];
    const auto& last = plan.segments[sourceSegmentEnd - 1u];
    if (last.firstRecordIndex < first.firstRecordIndex ||
        last.recordCount > std::numeric_limits<std::uint32_t>::max() -
                               last.firstRecordIndex ||
        last.firstRecordIndex + last.recordCount < first.firstRecordIndex ||
        last.firstRecordIndex + last.recordCount - first.firstRecordIndex ==
            0u) {
      return fallback(rawOrdinal, ReplayLane::Reject,
                      ReplayReason::StructuralOverflow);
    }
    if (sourceSegmentBegin > std::numeric_limits<std::uint32_t>::max() ||
        sourceSegmentEnd - sourceSegmentBegin >
            std::numeric_limits<std::uint32_t>::max()) {
      return fallback(rawOrdinal, ReplayLane::Reject,
                      ReplayReason::StructuralOverflow);
    }
    plan.sources.push_back(CpuReadySourcePlan{
        .firstSegmentIndex = static_cast<std::uint32_t>(sourceSegmentBegin),
        .segmentCount = static_cast<std::uint32_t>(
            sourceSegmentEnd - sourceSegmentBegin),
        .firstRecordIndex = first.firstRecordIndex,
        .recordCount = static_cast<std::uint32_t>(
            last.firstRecordIndex + last.recordCount -
            first.firstRecordIndex),
        .jumbo = std::any_of(
            plan.segments.begin() + sourceSegmentBegin,
            plan.segments.begin() + sourceSegmentEnd,
            [](const CpuReadySegmentPlan& segment) { return segment.jumbo; }),
        .arenaLayout = *sourceLayout,
    });
    ++plan.sourceCount;
    sourceSegmentBegin = sourceSegmentEnd;
  }

  if (plan.sourceCount == 0) {
    return fallback(rawOrdinal, ReplayLane::Reject,
                    ReplayReason::StructuralOverflow);
  }
  // Preserve the old aggregate adapter only when there is one source. The
  // multi-source plan is intentionally invisible to the current production
  // caller until its source transaction is implemented.
  if (plan.sourceCount == 1) {
    plan.arenaLayout = plan.sources[0].arenaLayout;
    if (plan.segmentCount == 1) {
      plan.layout = plan.segments[0].layout;
    }
  }
  return plan;
}

}  // namespace dxmt9::d3d9
