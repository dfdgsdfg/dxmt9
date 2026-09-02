#include "device_c_cpu_ready_plan.hpp"
#include "device_c_ordered_control.hpp"
// Only this translation unit needs the shared derived-dimension helper; the
// planner header must not pull the whole direct-continuation surface in.
#include "../dxmt9/dxmt9_direct_continuation.hpp"

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

// The direct replay sink can only append ordinary point/line/triangle-list
// and triangle-strip draws through submitDirectReplayDrawFromCurrentState.
// TriangleFan is
// intentionally handled by the legacy draw path (the sink's record-level
// batch predicate rejects it), so allowing it into a Direct ChunkSlot would
// silently call submitDrawRun while a direct transaction is active.  That
// marks the transaction failed without returning an HRESULT and turns the
// later commit into a misleading D3DERR_INVALIDCALL. Keep planner admission
// closed over the same primitive boundary as the replay sink.
bool directDrawRecordSupported(const ImportedChunkView& imported,
                               std::size_t recordIndex) noexcept {
  const auto type = imported.records[recordIndex].type;
  switch (type) {
  case D9C_COMMAND_RECORD_DRAW_PRIMITIVE:
  case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE:
    return static_cast<core::PrimitiveType>(
               imported.record(recordIndex).drawHeader.primitiveType - 1u) !=
           core::PrimitiveType::TriangleFan;
  default:
    return true;
  }
}

template <typename T>
bool loadFixed(const ImportedRecordView& record, T& out) noexcept {
  if (record.payload.size() < sizeof(T)) {
    return false;
  }
  std::memcpy(&out, record.payload.data(), sizeof(T));
  return true;
}

// Island membership has exactly one authority: `kRecordTopology`'s
// `islandResident` row plus the runtime primitive predicate above. Before
// this helper existed, `classifyDirectChunkSlotRange` and
// `makeDirectSlotCapacity` each restated the same alphabet in a private
// switch, and both already disagreed with `classifyDirectChunkSlotReplay` on
// six kinds (Clear, UpdateSurface, StretchRect, ColorFill, RESZ,
// GenerateMipmaps). Widening the island by one family must be a one-row edit
// in the table, not four coordinated switch edits that can silently diverge.
enum class IslandMembership : std::uint8_t {
  // Island-resident and GPU-producing.
  Draw,
  // Island-resident; mutates replay state and emits no command header.
  StateOnly,
  // Structurally valid, but not island-resident. This is the historical
  // `Unsupported` disposition, not a rejection of the raw.
  Outside,
  // The record disagrees with its own wire rule.
  Malformed,
};

IslandMembership islandMembership(const ImportedChunkView& imported,
                                  std::size_t index) noexcept {
  const auto record = imported.record(index);
  if (record.header.type != imported.records[index].type ||
      record.payload.size() != imported.records[index].payloadSize) {
    return IslandMembership::Malformed;
  }
  const auto* rule = recordRule(record.header.type);
  const auto* topology = recordTopology(record.header.type);
  if (!rule || !topology) {
    // Outside the closed 21-kind alphabet. The complete wire validator ran
    // before import, so an unrecognized kind here is a family this gate does
    // not own rather than a malformed record.
    return IslandMembership::Outside;
  }
  // The same fixed-header presence check the per-kind `loadFixed` calls used
  // to perform, driven by the structural table instead of by the kind.
  if (record.payload.size() < rule->fixedPayloadSize) {
    return IslandMembership::Malformed;
  }
  if (!topology->islandResident) {
    return IslandMembership::Outside;
  }
  if (topology->topology != RecordReplayTopology::DirectDraw) {
    return IslandMembership::StateOnly;
  }
  // drawHeader is memcpy'd from the payload at import for every Draw-flagged
  // kind, so this is the value the old per-call loadFixed produced.
  const auto primitiveType = record.drawHeader.primitiveType;
  if (primitiveType < 1u || primitiveType > 6u) {
    return IslandMembership::Malformed;
  }
  return directDrawRecordSupported(imported, index)
             ? IslandMembership::Draw
             : IslandMembership::Outside;
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
  case D9C_COMMAND_RECORD_GENERATE_MIPMAPS:
    return addCount(capacity.commandHeaders, 1) &&
                   addCount(capacity.generateMipmapsRecords, 1)
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
  // The u32 checks above are this planner's own overflow contract; the
  // derived dimensions themselves are owned by one shared pure function so
  // the producer plan, the empty-slot provisioning budget and the continuation
  // admission predicate cannot drift apart (R-BACK-2.104).
  core::applyDerivedDrawCapacity(capacity, drawCount);
  return capacity.drawUniformVertexConstantBytes == vertexConstantBytes &&
         capacity.drawUniformPixelConstantBytes == pixelConstantBytes;
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

// The ordinary direct ChunkSlot path has no Arena segment boundary to carry
// an oversized source.  Keep this independent structural pass deliberately
// narrow: only non-UP draws and state/constants may use the final-slot
// representation.  All other record families retain their existing
// pre-effect fallback disposition.
std::optional<core::SourcePayloadLayout> makeDirectSlotCapacity(
    const ImportedChunkView& imported, std::size_t pageSize,
    core::SourcePayloadCapacity& capacity) noexcept {
  std::size_t drawCount = 0;
  bool sawDraw = false;
  for (std::size_t i = 0; i < imported.records.size(); ++i) {
    const auto record = imported.record(i);
    switch (islandMembership(imported, i)) {
    case IslandMembership::Draw:
      if (addDirectRecordCapacity(capacity, drawCount, record) !=
          DirectRecordResult::GpuProducing) {
        return std::nullopt;
      }
      sawDraw = true;
      break;
    case IslandMembership::StateOnly:
      if (addDirectRecordCapacity(capacity, drawCount, record) !=
          DirectRecordResult::StateOnly) {
        return std::nullopt;
      }
      break;
    case IslandMembership::Outside:
    case IslandMembership::Malformed:
      return std::nullopt;
    }
  }
  if (!sawDraw || !addDerivedDrawCapacity(capacity, drawCount)) {
    return std::nullopt;
  }
  return core::makeSourcePayloadLayout(
      capacity, pageSize, std::numeric_limits<std::uint32_t>::max());
}


// ---------------------------------------------------------------------------
// Source-wide replay emission planning.

// One record's role in the total partition, derived only from the closed
// kRecordTopology table plus the runtime primitive predicate. There is no
// second opinion about the alphabet here: an unknown type is the only value
// that cannot be placed in a segment.
enum class EmissionRecordRole : std::uint8_t {
  IslandDraw,
  IslandStateOnly,
  Coordinator,
  OrderedControl,
  CompatibilityDraw,
  Unknown,
};

EmissionRecordRole emissionRecordRole(
    const ImportedChunkView& imported, std::size_t index) noexcept {
  const auto* topology = recordTopology(imported.records[index].type);
  if (!topology) {
    return EmissionRecordRole::Unknown;
  }
  switch (topology->topology) {
  case RecordReplayTopology::DirectDraw:
    // TriangleFan is structurally valid but expands through the
    // compatibility sink, which bypasses the direct appender entirely.
    return directDrawRecordSupported(imported, index)
               ? EmissionRecordRole::IslandDraw
               : EmissionRecordRole::CompatibilityDraw;
  case RecordReplayTopology::DirectStateOnly:
    return EmissionRecordRole::IslandStateOnly;
  case RecordReplayTopology::CoordinatorCommand:
    return EmissionRecordRole::Coordinator;
  case RecordReplayTopology::OrderedControl:
    return EmissionRecordRole::OrderedControl;
  case RecordReplayTopology::CompatibilityDraw:
    return EmissionRecordRole::CompatibilityDraw;
  case RecordReplayTopology::Count:
    break;
  }
  return EmissionRecordRole::Unknown;
}

// Derive the executable partition from the descriptive one. Ordered controls
// and compatibility ranges are the only cuts; a coordinator locator stays
// inside the span that owns it, because the direct assembler has a typed
// appender for every coordinator command and Present.
//
// The per-span capacity is rebuilt from the span's own records rather than
// summed from the segment capacities: an island segment's capacity already
// carries the non-linear derived dimensions for *its* draw count, and
// `chunkSlotUniformLookupBucketCount` is not additive, so summing two islands
// under-reserves the lookup buckets and would force the assembler to resize
// final storage inside the transaction (forbidden by R-BACK-2.86).
EmissionPlanReason deriveEmissionLeaseSpans(
    const ImportedChunkView& imported, std::size_t pageSize,
    ReplayEmissionPlan& plan) noexcept {
  try {
    plan.leaseSpans.reserve(plan.segments.size());
  } catch (...) {
    return EmissionPlanReason::Storage;
  }

  const auto cutForSegment = [](EmissionSegmentKind kind) noexcept {
    switch (kind) {
    case EmissionSegmentKind::OrderedControlLocator:
      return EmissionSpanCut::OrderedControl;
    case EmissionSegmentKind::CompatibilityRange:
      return EmissionSpanCut::CompatibilityRange;
    case EmissionSegmentKind::DirectIsland:
    case EmissionSegmentKind::StateOnlyRun:
    case EmissionSegmentKind::CoordinatorLocator:
    case EmissionSegmentKind::Count:
      break;
    }
    return EmissionSpanCut::EndOfRaw;
  };

  bool overflow = false;
  const auto finishSpan = [&](EmissionLeaseSpan& span,
                              EmissionSpanCut cut) noexcept {
    span.trailingCut = cut;
    span.ownsLease = span.drawCount != 0;
    // A lease appends Present through the build context, which parks it and
    // emits it once at commit as the slot's publication boundary. Only a
    // single Present sitting on the span's final segment therefore lands in
    // source order; anything the lease appends after a Present would execute
    // before it. Recorded per span here, enforced once for the raw below.
    if (span.presentCount == 1u && span.segmentCount != 0u) {
      const std::size_t lastSegment =
          static_cast<std::size_t>(span.firstSegmentIndex) +
          span.segmentCount - 1u;
      span.presentTrailingCoordinator =
          lastSegment < plan.segments.size() &&
          plan.segments[lastSegment].kind ==
              EmissionSegmentKind::CoordinatorLocator &&
          plan.segments[lastSegment].locatorRecordType ==
              D9C_COMMAND_RECORD_PRESENT;
    }
    if (span.ownsLease) {
      core::SourcePayloadCapacity capacity{};
      std::size_t drawCount = 0;
      for (std::uint32_t i = span.firstRecordIndex; i < span.endRecordIndex();
           ++i) {
        const auto record = imported.record(i);
        switch (addDirectRecordCapacity(capacity, drawCount, record)) {
        case DirectRecordResult::StateOnly:
        case DirectRecordResult::GpuProducing:
          break;
        case DirectRecordResult::Overflow:
        case DirectRecordResult::Invalid:
        case DirectRecordResult::Unsupported:
          overflow = true;
          return;
        }
      }
      if (drawCount != span.drawCount ||
          !addDerivedDrawCapacity(capacity, drawCount)) {
        overflow = true;
        return;
      }
      const auto layout = core::makeSourcePayloadLayout(
          capacity, pageSize, std::numeric_limits<std::uint32_t>::max());
      if (!layout || layout->usedBytes == 0 ||
          capacity.commandHeaders != span.drawCount + span.coordinatorCount) {
        overflow = true;
        return;
      }
      span.capacity = capacity;
      span.plannedBytes = layout->usedBytes;
    }
    plan.leaseSpans.push_back(span);
    span = EmissionLeaseSpan{};
  };

  EmissionLeaseSpan current{};
  bool currentOpen = false;
  for (std::uint32_t segmentIndex = 0;
       segmentIndex < plan.segments.size() && !overflow; ++segmentIndex) {
    const auto& segment = plan.segments[segmentIndex];
    const auto cut = cutForSegment(segment.kind);
    if (cut != EmissionSpanCut::EndOfRaw) {
      if (currentOpen) {
        finishSpan(current, cut);
        currentOpen = false;
        if (overflow) break;
      }
      // The separator is itself an ordinary span of exactly one segment, so
      // the executable partition still covers the raw with no gap.
      EmissionLeaseSpan separator{
          .firstRecordIndex = segment.firstRecordIndex,
          .recordCount = segment.recordCount,
          .firstSegmentIndex = segmentIndex,
          .segmentCount = 1u,
          .trailingCut = cut,
      };
      plan.leaseSpans.push_back(separator);
      continue;
    }
    if (!currentOpen) {
      current = EmissionLeaseSpan{
          .firstRecordIndex = segment.firstRecordIndex,
          .firstSegmentIndex = segmentIndex,
      };
      currentOpen = true;
    }
    current.recordCount += segment.recordCount;
    ++current.segmentCount;
    current.drawCount += segment.drawCount;
    if (segment.kind == EmissionSegmentKind::DirectIsland) {
      ++current.islandCount;
    } else if (segment.kind == EmissionSegmentKind::CoordinatorLocator) {
      ++current.coordinatorCount;
      if (segment.locatorRecordType == D9C_COMMAND_RECORD_PRESENT) {
        ++current.presentCount;
        current.containsTerminalPresent =
            segmentIndex + 1u == plan.segments.size();
      }
    }
  }
  if (currentOpen && !overflow) {
    finishSpan(current, EmissionSpanCut::EndOfRaw);
  }
  if (overflow) {
    plan.leaseSpans.clear();
    return EmissionPlanReason::Capacity;
  }

  // Lease ordinals are dense over the lease-owning spans only, so a gap in
  // the sequence the queue observes is a real ordering fault rather than an
  // artifact of an interleaved ordinary span.
  std::uint32_t leaseOrdinal = 0;
  std::size_t lastLeaseSpan = plan.leaseSpans.size();
  for (std::size_t i = 0; i < plan.leaseSpans.size(); ++i) {
    if (!plan.leaseSpans[i].ownsLease) continue;
    plan.leaseSpans[i].leaseOrdinal = leaseOrdinal++;
    lastLeaseSpan = i;
  }
  if (lastLeaseSpan < plan.leaseSpans.size()) {
    plan.leaseSpans[lastLeaseSpan].finalLeaseSpan = true;
  }

  // Exact coverage again, on the executable partition this time. Nothing
  // downstream may execute a span list that does not cover the raw.
  std::size_t coveredRecords = 0;
  std::size_t coveredSegments = 0;
  for (const auto& span : plan.leaseSpans) {
    if (span.recordCount == 0 || span.segmentCount == 0 ||
        span.firstRecordIndex != coveredRecords ||
        span.firstSegmentIndex != coveredSegments) {
      plan.leaseSpans.clear();
      return EmissionPlanReason::Coverage;
    }
    coveredRecords += span.recordCount;
    coveredSegments += span.segmentCount;
  }
  if (coveredRecords != imported.records.size() ||
      coveredSegments != plan.segments.size()) {
    plan.leaseSpans.clear();
    return EmissionPlanReason::Coverage;
  }
  plan.leaseSpanCount = plan.leaseSpans.size();
  return EmissionPlanReason::Complete;
}

ReplayEmissionPlan emissionReject(RawOrdinal rawOrdinal,
                                  EmissionPlanReason reason) noexcept {
  ReplayEmissionPlan plan;
  plan.rawOrdinal = rawOrdinal;
  plan.reason = reason;
  plan.leaseBlock = EmissionLeaseBlock::IncompletePlan;
  return plan;
}

}  // namespace

ReplayEmissionPlan planReplayEmission(
    const ImportedChunkView& imported, RawOrdinal rawOrdinal,
    std::size_t pageSize) noexcept {
  if (rawOrdinal == 0 || pageSize == 0 ||
      imported.records.size() != imported.header.recordCount) {
    return emissionReject(rawOrdinal, EmissionPlanReason::MalformedView);
  }
  ReplayEmissionPlan plan;
  plan.rawOrdinal = rawOrdinal;
  try {
    plan.segments.reserve(imported.records.size());
  } catch (...) {
    return emissionReject(rawOrdinal, EmissionPlanReason::Storage);
  }

  // One left-to-right pass. Greedy accumulation is maximal because every
  // record's role is a per-record predicate independent of position, so no
  // later record can change an earlier one's classification.
  //
  // Aggregate capacity accumulates only what a lease would append: island
  // draws and coordinator locators. It is still accumulated when the raw
  // turns out to be lease-blocked, and simply not published in that case.
  core::SourcePayloadCapacity aggregate{};
  std::size_t aggregateDrawCount = 0;

  // Pending island-resident run. It is classified by what terminates it: a
  // run holding a draw becomes a DirectIsland, and a zero-draw run becomes a
  // StateOnlyRun unless a compatibility draw follows, in which case the
  // leading state belongs to that following GPU-producing record.
  core::SourcePayloadCapacity pendingCapacity{};
  std::size_t pendingDrawCount = 0;
  std::size_t pendingFirstRecord = 0;
  bool pendingOpen = false;

  bool pushFailed = false;
  const auto push = [&](EmissionSegment segment) noexcept {
    // segments was reserved to the record count and every segment holds at
    // least one record, so this cannot reallocate.
    if (plan.segments.size() == plan.segments.capacity()) {
      pushFailed = true;
      return;
    }
    plan.segments.push_back(segment);
  };

  // Closes the pending resident run as an island or a state-only run. When
  // `foldIntoFollowingCompatibility` is set and the run holds no draw, the
  // run is left un-pushed so the caller can start the compatibility range at
  // the run's first record instead.
  const auto closePending =
      [&](std::size_t endExclusive,
          bool foldIntoFollowingCompatibility) noexcept -> bool {
    if (!pendingOpen) return true;
    if (pendingDrawCount == 0 && foldIntoFollowingCompatibility) {
      return true;  // caller keeps pendingFirstRecord as the range start
    }
    const auto recordCount = endExclusive - pendingFirstRecord;
    if (pendingDrawCount != 0) {
      auto capacity = pendingCapacity;
      if (!addDerivedDrawCapacity(capacity, pendingDrawCount)) {
        return false;
      }
      push(EmissionSegment{
          .kind = EmissionSegmentKind::DirectIsland,
          .firstRecordIndex = static_cast<std::uint32_t>(pendingFirstRecord),
          .recordCount = static_cast<std::uint32_t>(recordCount),
          .drawCount = static_cast<std::uint32_t>(pendingDrawCount),
          .locatorRecordType = 0u,
          .capacity = capacity,
      });
      ++plan.islandCount;
      plan.directIslandRecordCount += recordCount;
    } else {
      push(EmissionSegment{
          .kind = EmissionSegmentKind::StateOnlyRun,
          .firstRecordIndex = static_cast<std::uint32_t>(pendingFirstRecord),
          .recordCount = static_cast<std::uint32_t>(recordCount),
          .drawCount = 0u,
          .locatorRecordType = 0u,
          .capacity = {},
      });
      ++plan.stateOnlyRunCount;
    }
    pendingCapacity = {};
    pendingDrawCount = 0;
    pendingOpen = false;
    return true;
  };

  // Compatibility ranges are maximal: an interstitial state run folded in
  // from the left must extend the previous range rather than start a second
  // one beside it.
  const auto appendCompatibility = [&](std::size_t firstRecord,
                                       std::size_t recordCount) noexcept {
    if (!plan.segments.empty()) {
      auto& previous = plan.segments.back();
      if (previous.kind == EmissionSegmentKind::CompatibilityRange &&
          static_cast<std::size_t>(previous.firstRecordIndex) +
                  previous.recordCount ==
              firstRecord) {
        previous.recordCount += static_cast<std::uint32_t>(recordCount);
        plan.compatibilityRecordCount += recordCount;
        return;
      }
    }
    push(EmissionSegment{
        .kind = EmissionSegmentKind::CompatibilityRange,
        .firstRecordIndex = static_cast<std::uint32_t>(firstRecord),
        .recordCount = static_cast<std::uint32_t>(recordCount),
        .drawCount = 0u,
        .locatorRecordType = 0u,
        .capacity = {},
    });
    ++plan.compatibilityRangeCount;
    plan.compatibilityRecordCount += recordCount;
  };

  for (std::size_t i = 0; i < imported.records.size(); ++i) {
    const auto record = imported.record(i);
    if (record.header.type != imported.records[i].type ||
        record.payload.size() != imported.records[i].payloadSize) {
      return emissionReject(rawOrdinal, EmissionPlanReason::MalformedView);
    }
    const auto role = emissionRecordRole(imported, i);
    if (role == EmissionRecordRole::Unknown) {
      return emissionReject(rawOrdinal, EmissionPlanReason::UnknownRecord);
    }
    switch (role) {
    case EmissionRecordRole::IslandDraw:
    case EmissionRecordRole::IslandStateOnly: {
      if (!pendingOpen) {
        pendingFirstRecord = i;
        pendingOpen = true;
      }
      std::size_t localDraws = pendingDrawCount;
      const auto localResult =
          addDirectRecordCapacity(pendingCapacity, localDraws, record);
      switch (localResult) {
      case DirectRecordResult::Invalid:
        return emissionReject(rawOrdinal, EmissionPlanReason::MalformedView);
      case DirectRecordResult::Overflow:
        return emissionReject(rawOrdinal, EmissionPlanReason::Overflow);
      case DirectRecordResult::Unsupported:
        return emissionReject(rawOrdinal, EmissionPlanReason::UnknownRecord);
      case DirectRecordResult::StateOnly:
      case DirectRecordResult::GpuProducing:
        break;
      }
      pendingDrawCount = localDraws;
      // The aggregate mirrors the same append into the shared reservation.
      std::size_t totalDraws = aggregateDrawCount;
      if (addDirectRecordCapacity(aggregate, totalDraws, record) !=
          localResult) {
        return emissionReject(rawOrdinal, EmissionPlanReason::MalformedView);
      }
      aggregateDrawCount = totalDraws;
      break;
    }
    case EmissionRecordRole::Coordinator: {
      if (!closePending(i, /*foldIntoFollowingCompatibility=*/false)) {
        return emissionReject(rawOrdinal, EmissionPlanReason::Overflow);
      }
      core::SourcePayloadCapacity locator{};
      std::size_t locatorDraws = 0;
      std::size_t ignoredDraws = 0;
      const auto locatorResult =
          addDirectRecordCapacity(locator, locatorDraws, record);
      const auto aggregateResult =
          addDirectRecordCapacity(aggregate, ignoredDraws, record);
      if (locatorResult != aggregateResult ||
          locatorResult != DirectRecordResult::GpuProducing ||
          locatorDraws != 0 || ignoredDraws != 0) {
        // Returning here rather than continuing keeps the partition and the
        // capacity accounting in agreement: a skipped locator would leave a
        // coverage hole that reports the wrong typed reason.
        return emissionReject(
            rawOrdinal,
            locatorResult == DirectRecordResult::Overflow ||
                    aggregateResult == DirectRecordResult::Overflow
                ? EmissionPlanReason::Overflow
                : EmissionPlanReason::MalformedView);
      }
      push(EmissionSegment{
          .kind = EmissionSegmentKind::CoordinatorLocator,
          .firstRecordIndex = static_cast<std::uint32_t>(i),
          .recordCount = 1u,
          .drawCount = 0u,
          .locatorRecordType = record.header.type,
          .capacity = locator,
      });
      ++plan.coordinatorCount;
      if (record.header.type == D9C_COMMAND_RECORD_PRESENT) {
        plan.containsPresent = true;
        ++plan.presentCount;
      }
      break;
    }
    case EmissionRecordRole::OrderedControl: {
      if (!closePending(i, /*foldIntoFollowingCompatibility=*/false)) {
        return emissionReject(rawOrdinal, EmissionPlanReason::Overflow);
      }
      push(EmissionSegment{
          .kind = EmissionSegmentKind::OrderedControlLocator,
          .firstRecordIndex = static_cast<std::uint32_t>(i),
          .recordCount = 1u,
          .drawCount = 0u,
          .locatorRecordType = record.header.type,
          .capacity = {},
      });
      ++plan.orderedControlCount;
      break;
    }
    case EmissionRecordRole::CompatibilityDraw: {
      const std::size_t rangeFirst =
          pendingOpen && pendingDrawCount == 0 ? pendingFirstRecord : i;
      if (!closePending(i, /*foldIntoFollowingCompatibility=*/true)) {
        return emissionReject(rawOrdinal, EmissionPlanReason::Overflow);
      }
      pendingOpen = false;
      pendingCapacity = {};
      pendingDrawCount = 0;
      appendCompatibility(rangeFirst, i + 1u - rangeFirst);
      break;
    }
    case EmissionRecordRole::Unknown:
      return emissionReject(rawOrdinal, EmissionPlanReason::UnknownRecord);
    }
    if (pushFailed) {
      return emissionReject(rawOrdinal, EmissionPlanReason::Storage);
    }
  }
  if (!closePending(imported.records.size(),
                    /*foldIntoFollowingCompatibility=*/false)) {
    return emissionReject(rawOrdinal, EmissionPlanReason::Overflow);
  }
  if (pushFailed) {
    return emissionReject(rawOrdinal, EmissionPlanReason::Storage);
  }

  // Exact-coverage fold. This is the same partition proof replayResolvedChunk
  // already performs on arenaSegments, applied before any effect.
  std::size_t covered = 0;
  for (const auto& segment : plan.segments) {
    if (segment.recordCount == 0 || segment.firstRecordIndex != covered ||
        covered > imported.records.size() ||
        segment.recordCount > imported.records.size() - covered) {
      return emissionReject(rawOrdinal, EmissionPlanReason::Coverage);
    }
    covered += segment.recordCount;
  }
  if (covered != imported.records.size()) {
    return emissionReject(rawOrdinal, EmissionPlanReason::Coverage);
  }

  if (const auto spanReason =
          deriveEmissionLeaseSpans(imported, pageSize, plan);
      spanReason != EmissionPlanReason::Complete) {
    return emissionReject(rawOrdinal, spanReason);
  }

  plan.directDrawCount = aggregateDrawCount;
  plan.reason = EmissionPlanReason::Complete;
  plan.coverageExact = true;

  // The partition is total regardless of what follows. The lease question is
  // separate and reported in a fixed precedence order.
  //
  // Present ordering comes first, because it is the one block that reports a
  // wrong-order *emission* rather than an unsupported record. Present is not
  // appended at its serial index: the build context parks it and commit emits
  // it once, last, as the slot's publication boundary, and a second Present in
  // one transaction is refused outright. So a lease can express exactly one
  // Present, as its final segment, and nothing else. Any other Present shape
  // fails the whole raw closed -- not just the offending span -- because the
  // remaining spans are only meaningful as a refinement of a stream this
  // router has already declined to emit.
  for (const auto& span : plan.leaseSpans) {
    if (!span.ownsLease || span.presentCount == 0u) continue;
    if (span.presentTrailingCoordinator) continue;
    plan.leaseBlock = EmissionLeaseBlock::PresentOrdering;
    return plan;
  }
  if (plan.orderedControlCount != 0) {
    plan.leaseBlock = EmissionLeaseBlock::OrderedControl;
    return plan;
  }
  if (plan.compatibilityRecordCount != 0) {
    plan.leaseBlock = EmissionLeaseBlock::CompatibilityRange;
    return plan;
  }
  if (plan.islandCount == 0 || aggregateDrawCount == 0) {
    plan.leaseBlock = EmissionLeaseBlock::NoIsland;
    return plan;
  }
  // Derived dimensions are a non-linear function of the total draw count
  // (chunkSlotUniformLookupBucketCount), so they must be computed once on the
  // aggregate. Summing per-island derived dimensions under-reserves the
  // lookup buckets and would make an append resize storage mid-transaction.
  if (!addDerivedDrawCapacity(aggregate, aggregateDrawCount)) {
    return emissionReject(rawOrdinal, EmissionPlanReason::Overflow);
  }
  const auto layout = core::makeSourcePayloadLayout(
      aggregate, pageSize, std::numeric_limits<std::uint32_t>::max());
  if (!layout || layout->usedBytes == 0) {
    return emissionReject(rawOrdinal, EmissionPlanReason::Capacity);
  }
  plan.slotCommandCount = aggregateDrawCount + plan.coordinatorCount;
  if (aggregate.commandHeaders != plan.slotCommandCount) {
    return emissionReject(rawOrdinal, EmissionPlanReason::MalformedView);
  }
  plan.aggregateCapacity = aggregate;
  plan.aggregatePlannedBytes = layout->usedBytes;
  plan.leaseBlock = EmissionLeaseBlock::None;
  return plan;
}

DirectChunkSlotReplayDisposition emissionPlanDisposition(
    const ReplayEmissionPlan& plan, bool captureOrTrace) noexcept {
  if (captureOrTrace) {
    return DirectChunkSlotReplayDisposition::LegacyCaptureOrTrace;
  }
  if (!plan.partitioned()) {
    switch (plan.reason) {
    case EmissionPlanReason::MalformedView:
    case EmissionPlanReason::UnknownRecord:
      return DirectChunkSlotReplayDisposition::RejectInvalid;
    case EmissionPlanReason::Coverage:
      return DirectChunkSlotReplayDisposition::LegacySegmented;
    case EmissionPlanReason::Overflow:
    case EmissionPlanReason::Capacity:
    case EmissionPlanReason::Storage:
      return DirectChunkSlotReplayDisposition::LegacyOversized;
    case EmissionPlanReason::Complete:
    case EmissionPlanReason::Count:
      break;
    }
    return DirectChunkSlotReplayDisposition::LegacyUnsupported;
  }
  switch (plan.leaseBlock) {
  case EmissionLeaseBlock::None:
    // Reaching None means the Present rule below already held for every
    // lease-owning span, so a Present here is exactly one Present sitting on
    // its span's final segment -- the R-BACK-2.87 Present-tail shape. Report
    // it separately so that population stays distinguishable from ordinary
    // multi-island direct construction. Earlier coordinators (a leading Clear,
    // say) do not change the shape and deliberately do not disqualify it:
    // only Present has an ordering constraint, so only Present locators are
    // counted here.
    return plan.containsPresent
               ? DirectChunkSlotReplayDisposition::DirectWithPresentTail
               : DirectChunkSlotReplayDisposition::Direct;
  case EmissionLeaseBlock::PresentOrdering:
    // Standalone-after-work, non-trailing, or duplicate Present. The whole raw
    // falls back to compatibility replay before any effect.
    return DirectChunkSlotReplayDisposition::LegacyPresent;
  case EmissionLeaseBlock::OrderedControl:
    return DirectChunkSlotReplayDisposition::InlineOrderedControl;
  case EmissionLeaseBlock::CompatibilityRange:
    return DirectChunkSlotReplayDisposition::LegacyUpDraw;
  case EmissionLeaseBlock::NoIsland:
    return DirectChunkSlotReplayDisposition::LegacyStateOnly;
  case EmissionLeaseBlock::IncompletePlan:
  case EmissionLeaseBlock::Count:
    break;
  }
  return DirectChunkSlotReplayDisposition::LegacyUnsupported;
}

DirectChunkSlotRangeClass classifyDirectChunkSlotRange(
    const ImportedChunkView& imported) noexcept {
  if (imported.records.size() != imported.header.recordCount) {
    return DirectChunkSlotRangeClass::Malformed;
  }
  bool sawDraw = false;
  for (std::size_t i = 0; i < imported.records.size(); ++i) {
    switch (islandMembership(imported, i)) {
    case IslandMembership::Draw:
      sawDraw = true;
      break;
    case IslandMembership::StateOnly:
      break;
    case IslandMembership::Outside:
      // Valid controls, resource operations, UP draws, and TriangleFan draws
      // are deliberately rejected by this cheap whole-range gate.
      // Compatibility replay owns their ordering and keeps the final-slot
      // path carrier-free.
      return DirectChunkSlotRangeClass::Unsupported;
    case IslandMembership::Malformed:
      return DirectChunkSlotRangeClass::Malformed;
    }
  }
  return sawDraw ? DirectChunkSlotRangeClass::Eligible
                 : DirectChunkSlotRangeClass::Empty;
}

std::optional<DirectChunkSlotRangePlan> planDirectChunkSlotRange(
    const ImportedChunkView& imported, std::size_t pageSize) noexcept {
  if (classifyDirectChunkSlotRange(imported) !=
      DirectChunkSlotRangeClass::Eligible) {
    return std::nullopt;
  }
  core::SourcePayloadCapacity capacity{};
  const auto layout = makeDirectSlotCapacity(imported, pageSize, capacity);
  if (!layout) {
    return std::nullopt;
  }
  std::size_t drawCount = 0;
  for (const auto& record : imported.records) {
    if (record.type == D9C_COMMAND_RECORD_DRAW_PRIMITIVE ||
        record.type == D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE) {
      ++drawCount;
    }
  }
  return DirectChunkSlotRangePlan{
      .classification = DirectChunkSlotRangeClass::Eligible,
      .capacity = capacity,
      .drawCount = drawCount,
      .plannedBytes = layout->usedBytes,
  };
}

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
  const auto oversizedFallback = [&]() noexcept {
    auto result = fallback(rawOrdinal, ReplayLane::Legacy,
                           ReplayReason::Oversize);
    core::SourcePayloadCapacity directSlotCapacity{};
    const auto directSlotLayout = makeDirectSlotCapacity(
        imported, options.pageSize, directSlotCapacity);
    if (directSlotLayout) {
      result.directSlotCapacity = directSlotCapacity;
      result.directSlotLayout = directSlotLayout;
      result.directSlotSingleSource = true;
    }
    return result;
  };
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
      return oversizedFallback();
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
        return oversizedFallback();
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
      return oversizedFallback();
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
      return oversizedFallback();
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
      return oversizedFallback();
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

DirectChunkSlotReplayDisposition classifyDirectChunkSlotReplay(
    const ImportedChunkView& imported, const CpuReadyPlan& plan,
    bool captureOrTrace) noexcept {
  if (captureOrTrace) {
    return DirectChunkSlotReplayDisposition::LegacyCaptureOrTrace;
  }
  if (plan.lane == ReplayLane::Reject) {
    return DirectChunkSlotReplayDisposition::RejectInvalid;
  }
  if (plan.containsOrderedControls || plan.lane == ReplayLane::Inline) {
    return DirectChunkSlotReplayDisposition::InlineOrderedControl;
  }
  if (plan.reason == ReplayReason::Oversize) {
    if (plan.directChunkSlotCandidate()) {
      bool sawDraw = false;
      bool islandOnly = true;
      for (std::size_t i = 0; i < imported.records.size() && islandOnly; ++i) {
        switch (islandMembership(imported, i)) {
        case IslandMembership::Draw:
          sawDraw = true;
          break;
        case IslandMembership::StateOnly:
          break;
        case IslandMembership::Outside:
        case IslandMembership::Malformed:
          islandOnly = false;
          break;
        }
      }
      if (islandOnly && sawDraw) {
        return DirectChunkSlotReplayDisposition::DirectOversized;
      }
    }
    return DirectChunkSlotReplayDisposition::LegacyOversized;
  }
  const auto directPresentTailShape = [&]() noexcept {
    // The Present tail is a property of the destination that will own the
    // raw, not of the Arena planner. Keying it only to directArenaCandidate()
    // made `DirectWithPresentTail` unreachable for every raw the ordinary
    // final-slot lane owns, which is exactly the shape the environment
    // documentation names. Accept either destination shape.
    const bool arenaShape = plan.directArenaCandidate() &&
        plan.sourceCount == 1u && plan.arenaLayout.has_value();
    if ((!arenaShape && !plan.directChunkSlotCandidate()) ||
        imported.records.size() < 2u ||
        imported.records.back().type != D9C_COMMAND_RECORD_PRESENT) {
      return false;
    }
    bool sawDraw = false;
    for (std::size_t i = 0; i + 1u < imported.records.size(); ++i) {
      switch (islandMembership(imported, i)) {
      case IslandMembership::Draw:
        sawDraw = true;
        continue;
      case IslandMembership::StateOnly:
        continue;
      case IslandMembership::Outside:
      case IslandMembership::Malformed:
        break;
      }
      // The single optional leading Clear is the only non-island record the
      // Present-tail shape admits, and only at the head of the raw.
      if (i != 0u ||
          imported.records[i].type != D9C_COMMAND_RECORD_CLEAR) {
        return false;
      }
    }
    return sawDraw;
  };
  if (plan.reason == ReplayReason::Present) {
    return directPresentTailShape()
               ? DirectChunkSlotReplayDisposition::DirectWithPresentTail
               : DirectChunkSlotReplayDisposition::LegacyPresent;
  }
  if (plan.lane == ReplayLane::StateOnly) {
    return DirectChunkSlotReplayDisposition::LegacyStateOnly;
  }
  // Ordinary Direct ChunkSlot construction has one queue-owned destination,
  // but that destination is not page-segmented. A single logical source may
  // therefore consume several planner segments as one aggregate, provided
  // the planner produced its checked packed layout. Multiple logical sources
  // still require the Arena batch transaction and remain a typed fallback.
  if (plan.directArenaCandidate() &&
      (plan.sourceCount != 1u || !plan.arenaLayout.has_value())) {
    return DirectChunkSlotReplayDisposition::LegacySegmented;
  }
  if (!plan.directArenaCandidate()) {
    return DirectChunkSlotReplayDisposition::LegacyUnsupported;
  }
  for (std::size_t i = 0; i < imported.records.size(); ++i) {
    switch (islandMembership(imported, i)) {
    case IslandMembership::Draw:
    case IslandMembership::StateOnly:
      continue;
    case IslandMembership::Malformed:
      return DirectChunkSlotReplayDisposition::LegacyUnsupported;
    case IslandMembership::Outside:
      break;
    }
    const auto* topology = recordTopology(imported.records[i].type);
    if (!topology) {
      return DirectChunkSlotReplayDisposition::LegacyUnsupported;
    }
    switch (topology->topology) {
    case RecordReplayTopology::CoordinatorCommand:
      // The Arena builder owns every coordinator command except Present,
      // whose tail shape decides between the direct and legacy dispositions.
      if (imported.records[i].type == D9C_COMMAND_RECORD_PRESENT) {
        return directPresentTailShape()
                   ? DirectChunkSlotReplayDisposition::DirectWithPresentTail
                   : DirectChunkSlotReplayDisposition::LegacyPresent;
      }
      continue;
    case RecordReplayTopology::OrderedControl:
      return DirectChunkSlotReplayDisposition::InlineOrderedControl;
    case RecordReplayTopology::CompatibilityDraw:
      return DirectChunkSlotReplayDisposition::LegacyUpDraw;
    case RecordReplayTopology::DirectDraw:
      // A TriangleFan draw is island-resident by kind but not by primitive.
      return DirectChunkSlotReplayDisposition::LegacyUnsupported;
    case RecordReplayTopology::DirectStateOnly:
    case RecordReplayTopology::Count:
      break;
    }
    return DirectChunkSlotReplayDisposition::LegacyUnsupported;
  }
  return DirectChunkSlotReplayDisposition::Direct;
}

}  // namespace dxmt9::d3d9
