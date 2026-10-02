#include "device_c_render_tape_identity_v3.hpp"

#include "device_c_render_tape_capture.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace dxmt9::d3d9 {
namespace {

using Prefix = RenderTapeIdentityV3PrefixKind;
using Phase = RenderTapeIdentityV3SourcePhase;
using Kind = RenderTapeIdentityV3DispositionKind;
using Status = RenderTapeIdentityV3Status;
using Observation = RenderTapeIdentityV3Observation;
// The authenticated descriptor uses D3DQUERYTYPE, not core::QueryType's
// different compact enumeration. Never infer this value from QUERY_ISSUE.
constexpr std::uint32_t kWireOcclusionQueryType = 9u;

template <typename T>
bool load(std::span<const std::byte> bytes, std::size_t offset,
          T& value) noexcept {
  if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) return false;
  std::memcpy(&value, bytes.data() + offset, sizeof(T));
  return true;
}

bool headerAdmitted(const RenderTapeIdentityV3Header& header) noexcept {
  const auto kind = static_cast<Prefix>(header.baselineKind);
  return header.magic == kRenderTapeIdentityMagic &&
      header.version == kRenderTapeIdentityV3Version &&
      header.headerSize == sizeof(header) && header.frameId &&
      header.presentOrdinal && header.captureToken && header.captureEpoch &&
      header.queue && header.baselineCompletionEdge &&
      header.initializerEpoch &&
      header.baselineCompletedSequence >= header.baselineSequence &&
      header.baselineCompletedInitializer >= header.baselineInitializer &&
      ((kind == Prefix::EmptyBaseline && !header.baselineSequence &&
        !header.baselineCompletedSequence) ||
       (kind == Prefix::ArmBaseline && header.baselineSequence)) &&
      header.authority == static_cast<std::uint32_t>(
          RenderTapeIdentityAuthority::Capture) &&
      !header.reserved0 && !header.reserved1 && !header.reserved2 && !header.reserved3;
}

bool noGpuMembership(const RenderTapeIdentityV3Disposition& row) noexcept {
  return row.commandIndex == kRenderTapeIdentityV3NoIndex &&
      !row.firstSubdraw && !row.subdrawCount;
}

bool commandShapeAdmitted(const RenderTapeIdentityV3Command& row,
    std::span<const RenderTapeIdentityV3Source> sources,
    std::span<const RenderTapeIdentityV3Coordinator> coordinators) noexcept {
  const bool draw = row.kind == static_cast<std::uint32_t>(RenderTapeIdentityV3CommandKind::DrawRun);
  if (row.sourceIndex >= sources.size() ||
      row.physicalCommandIndex >= sources[row.sourceIndex].commandCount ||
      row.kind < static_cast<std::uint32_t>(RenderTapeIdentityV3CommandKind::DrawRun) ||
      row.kind > static_cast<std::uint32_t>(RenderTapeIdentityV3CommandKind::Present) ||
      draw != (row.drawCount != 0u) || row.drawCount > kRenderTapeIdentityV3MaxRows ||
      row.reserved0 || row.reserved1 ||
      (row.coordinatorIndex != kRenderTapeIdentityV3NoIndex && row.coordinatorIndex >= coordinators.size()))
    return false;
  if (row.coordinatorIndex != kRenderTapeIdentityV3NoIndex &&
      !renderTapeIdentityV3CoordinatorCommandAdmitted(
          coordinators[row.coordinatorIndex].recordType,
          static_cast<RenderTapeIdentityV3CommandKind>(row.kind))) return false;
  if (row.membership == static_cast<std::uint32_t>(RenderTapeIdentityV3Membership::Pass))
    return row.logicalPassId && row.dagPassIndex != kRenderTapeIdentityV3NoIndex && row.passKind <= 4u;
  return !draw && row.membership == static_cast<std::uint32_t>(
      RenderTapeIdentityV3Membership::NoPassCommand) && !row.logicalPassId &&
      row.dagPassIndex == kRenderTapeIdentityV3NoIndex && !row.passKind;
}

bool prefixShapeAdmitted(const RenderTapeIdentityV3Header& header,
    const RenderTapeIdentityV3Coordinator& row,
    std::span<const RenderTapeIdentityV3Source> sources,
    std::span<const RenderTapeIdentityV3Initializer> initializers) noexcept {
  if (!row.prefixInitializer) {
    if (row.prefixInitializerEpoch || row.initializerIndex != kRenderTapeIdentityV3NoIndex)
      return false;
  } else {
    if (row.prefixInitializerEpoch != header.initializerEpoch) return false;
    if (row.prefixInitializer <= header.baselineCompletedInitializer) {
      if (row.initializerIndex != kRenderTapeIdentityV3NoIndex) return false;
    } else if (row.initializerIndex >= initializers.size() ||
        initializers[row.initializerIndex].sequence != row.prefixInitializer ||
        initializers[row.initializerIndex].epoch != row.prefixInitializerEpoch ||
        initializers[row.initializerIndex].publicationEdge >= row.dispatchEdge ||
        renderTapeIdentityV3InitializerPhase(initializers[row.initializerIndex]) == Phase::Invalid)
      return false;
  }
  const auto kind = static_cast<Prefix>(row.prefixKind);
  if (kind == Prefix::None)
    return row.sourceIndex == kRenderTapeIdentityV3NoIndex &&
        !row.prefixSequence && !row.prefixInitializer && !row.commandEnd &&
        !row.writerCaptureId && !row.writerGeneration && !row.storageGeneration &&
        row.observation == static_cast<std::uint32_t>(Observation::None);
  if (kind == Prefix::Unbound)
    return row.sourceIndex == kRenderTapeIdentityV3NoIndex &&
        !row.prefixSequence && row.commandEnd && row.writerCaptureId && row.writerGeneration &&
        row.storageGeneration &&
        (row.observation == static_cast<std::uint32_t>(Observation::None) ||
         row.observation == static_cast<std::uint32_t>(Observation::Pending));
  if (kind == Prefix::EmptyBaseline || kind == Prefix::ArmBaseline)
    return row.prefixKind == header.baselineKind &&
        row.sourceIndex == kRenderTapeIdentityV3NoIndex &&
        row.prefixSequence == header.baselineSequence &&
        !row.commandEnd &&
        !row.writerCaptureId && !row.writerGeneration && !row.storageGeneration;
  if (kind != Prefix::RealSource || row.sourceIndex >= sources.size()) return false;
  const auto& source = sources[row.sourceIndex];
  return row.prefixSequence && row.commandEnd && row.prefixSequence == source.seqId &&
      row.writerCaptureId == source.writerCaptureId &&
      row.writerGeneration == source.writerGeneration &&
      row.storageGeneration == source.storageGeneration &&
      row.commandEnd <= source.commandCount &&
      renderTapeIdentityV3SourcePhase(source) != Phase::Invalid;
}

// Lexicographic tape anchors, not derived source identities. AfterEvent has
// a sentinel cursor only in this local comparison, never in a wire cut.
using Position = std::pair<std::uint64_t, std::uint64_t>;
bool cutPosition(const RenderTapeIdentityV3Source& source,
    std::span<const RenderTapeIdentityV3EventFact> events,
    Position& out) noexcept {
  const auto event = std::find_if(events.begin(), events.end(),
      [&](const auto& value) { return value.ordinal == source.cutEventOrdinal; });
  if (event == events.end()) return false;
  switch (static_cast<RenderTapeIdentityV3CutKind>(source.cutKind)) {
  case RenderTapeIdentityV3CutKind::BeforeEvent:
    if (source.cutRecord) return false;
    out = {event->ordinal, 0u};
    return true;
  case RenderTapeIdentityV3CutKind::AfterRecord:
    if (event->type != static_cast<std::uint32_t>(RenderTapeEventType::CommandChunk) ||
        !source.cutRecord || source.cutRecord > event->recordCount) return false;
    out = {event->ordinal, source.cutRecord};
    return true;
  case RenderTapeIdentityV3CutKind::AfterEvent:
    if (source.cutRecord) return false;
    out = {event->ordinal, UINT64_MAX};
    return true;
  }
  return false;
}

bool cutPosition(const RenderTapeIdentityV3Initializer& initializer,
    std::span<const RenderTapeIdentityV3EventFact> events,
    Position& out) noexcept {
  const RenderTapeIdentityV3Source anchor{
      .cutEventOrdinal = initializer.cutEventOrdinal,
      .cutKind = initializer.cutKind, .cutRecord = initializer.cutRecord};
  return cutPosition(anchor, events, out);
}

bool queryFieldsEmpty(const RenderTapeIdentityV3Coordinator& row) noexcept {
  return !row.queryIdentity.kind && !row.queryIdentity.generation &&
      !row.queryIdentity.objectId && !row.issueCycle && !row.issueOrdinal &&
      !row.deviceEpoch && !row.flags && !row.queryType;
}

bool coordinatorShapeAdmitted(const RenderTapeIdentityV3Header& header,
    const RenderTapeIdentityV3Coordinator& row,
    std::span<const RenderTapeIdentityV3Source> sources,
    std::span<const RenderTapeIdentityV3Initializer> initializers) noexcept {
  const auto* topology = recordTopology(row.recordType);
  if (!topology || topology->topology != RecordReplayTopology::OrderedControl ||
      !row.eventOrdinal || !row.rawOrdinal || !row.buildGeneration ||
      row.dispatchEdge <= header.baselineCompletionEdge ||
      !row.releaseOrdinal || !row.releaseGeneration || row.dispatchResult < 0 ||
      row.releaseAction > static_cast<std::uint32_t>(
          core::metalqueue::SessionReleaseAction::SubmitAndWait) ||
      row.releaseWriter > static_cast<std::uint32_t>(
          core::metalqueue::SessionReleaseWriter::RetainQueryRange) ||
      row.observation > static_cast<std::uint32_t>(Observation::Completed) ||
      row.reserved0 || row.reserved1 ||
      !prefixShapeAdmitted(header, row, sources, initializers)) return false;
  if (row.recordType != D9C_COMMAND_RECORD_QUERY_ISSUE)
    return queryFieldsEmpty(row) && row.prefixKind == static_cast<std::uint32_t>(
        Prefix::None) && row.releaseWriter == static_cast<std::uint32_t>(
        core::metalqueue::SessionReleaseWriter::Publish);
  if (row.queryIdentity.kind != D9C_CHUNK_HANDLE_KIND_QUERY ||
      !row.queryIdentity.objectId || !row.queryIdentity.generation ||
      !row.issueCycle || !row.issueOrdinal || !row.deviceEpoch ||
      !renderTapeQueryDataBytes(row.queryType) ||
      (row.flags != 1u && row.flags != 2u)) return false;
  if (row.releaseWriter == static_cast<std::uint32_t>(
          core::metalqueue::SessionReleaseWriter::RetainQueryRange) &&
      row.queryType != kWireOcclusionQueryType)
    return false;
  return row.flags == 2u
      ? row.prefixKind == static_cast<std::uint32_t>(Prefix::None)
      : row.prefixKind != static_cast<std::uint32_t>(Prefix::None);
}

bool appendTable(RenderTapeIdentityV3Table& table, std::uint64_t& cursor,
                 std::size_t count, std::uint32_t size) noexcept {
  if (count > kRenderTapeIdentityV3MaxRows || cursor > UINT32_MAX) return false;
  table = {static_cast<std::uint32_t>(cursor),
           static_cast<std::uint32_t>(count), size};
  cursor += std::uint64_t{size} * count;
  return cursor <= UINT32_MAX;
}

bool layout(RenderTapeIdentityV3Header& header,
            const RenderTapeIdentityV3View& identity,
            std::uint64_t& size) noexcept {
  size = sizeof(header);
  return appendTable(header.sources, size, identity.sources.size(),
                     sizeof(RenderTapeIdentityV3Source)) &&
      appendTable(header.initializers, size, identity.initializers.size(),
                  sizeof(RenderTapeIdentityV3Initializer)) &&
      appendTable(header.commands, size, identity.commands.size(),
                  sizeof(RenderTapeIdentityV3Command)) &&
      appendTable(header.dispositions, size, identity.dispositions.size(),
                  sizeof(RenderTapeIdentityV3Disposition)) &&
      appendTable(header.coordinators, size, identity.coordinators.size(),
                  sizeof(RenderTapeIdentityV3Coordinator)) &&
      appendTable(header.settlements, size, identity.settlements.size(),
                  sizeof(RenderTapeIdentityV3ReplaySettlement));
}

bool tableAdmitted(RenderTapeIdentityV3Table table, std::uint64_t& cursor,
                   std::uint32_t entrySize) noexcept {
  if (table.offset != cursor || table.entrySize != entrySize ||
      table.count > kRenderTapeIdentityV3MaxRows) return false;
  cursor += std::uint64_t{table.count} * entrySize;
  return cursor <= UINT32_MAX;
}

template <typename T>
void copyTable(std::vector<T>& rows, std::span<const std::byte> bytes,
               RenderTapeIdentityV3Table table) {
  rows.resize(table.count);
  if (!rows.empty())
    std::memcpy(rows.data(), bytes.data() + table.offset,
                rows.size() * sizeof(T));
}

template <typename T>
void storeTable(std::vector<std::byte>& bytes,
    RenderTapeIdentityV3Table table, const std::vector<T>& rows) noexcept {
  if (!rows.empty())
    std::memcpy(bytes.data() + table.offset, rows.data(), rows.size() * sizeof(T));
}

Status canonicalFacts(std::span<const std::byte> tape,
    const RenderTapeBlobCatalogue& catalogue,
    const RenderTapeIdentityV3Header& header,
    std::vector<RenderTapeIdentityV3EventFact>& events,
    std::vector<RenderTapeIdentityV3RecordFact>& records) {
  ImportedRenderTapeView imported;
  if (!validateRenderTape(tape, catalogue, &imported).valid())
    return Status::InvalidTape;
  struct QueryDescriptor {
    D9CWireObjectIdentity identity{};
    RenderTapeQueryCycleState cycle{};
  };
  std::vector<QueryDescriptor> queries;
  for (std::size_t i = 0u; i < imported.events.size(); ++i) {
    const auto event = imported.event(i);
    RenderTapeIdentityV3EventFact eventFact{event.header.ordinal, event.header.type, 0u};
    const auto type = static_cast<RenderTapeEventType>(event.header.type);
    if (type == RenderTapeEventType::ObjectDefine) {
      RenderTapeObjectDefineHeader object{};
      if (!load(event.payload, 0u, object)) return Status::InvalidTape;
      if (object.identity.kind == D9C_CHUNK_HANDLE_KIND_QUERY) {
        RenderTapeQueryDescriptorDecoded descriptor{};
        if (sizeof(object) > event.payload.size() ||
            !renderTapeLoadQueryDescriptor(
                event.payload.subspan(sizeof(object), object.descriptorBytes),
                descriptor)) return Status::InvalidTape;
        queries.push_back({object.identity, renderTapeInitialQueryCycle(descriptor)});
      }
    } else if (type == RenderTapeEventType::PresentComplete) {
      RenderTapePresentCompleteHeader present{};
      if (!load(event.payload, 0u, present) ||
          present.presentOrdinal != header.presentOrdinal) return Status::InvalidHeader;
    } else if (type == RenderTapeEventType::CommandChunk) {
      RenderTapeCommandChunkHeader chunkHeader{};
      ImportedChunkView chunk{};
      if (!load(event.payload, 0u, chunkHeader) ||
          !importPrevalidatedCommandChunk(event.payload.subspan(sizeof(chunkHeader)),
              {.version = chunkHeader.wireVersion,
               .recordCount = chunkHeader.recordCount,
               .handleCount = chunkHeader.handleCount}, chunk)) return Status::InvalidTape;
      eventFact.recordCount = chunkHeader.recordCount;
      for (std::uint32_t recordIndex = 0u; recordIndex < chunk.records.size(); ++recordIndex) {
        const auto record = chunk.record(recordIndex);
        RenderTapeIdentityV3RecordFact fact{
            .eventOrdinal = event.header.ordinal,
            .recordIndex = recordIndex,
            .recordType = record.header.type};
        if (recordTopology(record.header.type)->topology == RecordReplayTopology::OrderedControl) {
          const auto control = makeOrderedControlDisposition(record, 1u, recordIndex);
          if (!control) return Status::InvalidCoordinator;
          fact.requiredReleaseAction = static_cast<std::uint32_t>(control->requiredReleaseAction);
        }
        if (record.header.type == D9C_COMMAND_RECORD_QUERY_ISSUE) {
          D9CCommandChunkWireQueryIssue issue{};
          if (!load(record.payload, 0u, issue) || issue.queryHandleIndex >= chunk.handles.size())
            return Status::InvalidTape;
          const auto& handle = chunk.handles[issue.queryHandleIndex];
          fact.queryIdentity = {handle.kind, handle.generation, handle.objectId};
          auto query = std::find_if(queries.begin(), queries.end(), [&](const auto& value) {
            return renderTapeIdentityV3SameIdentity(value.identity, fact.queryIdentity);
          });
          if (query == queries.end() || !renderTapeAdvanceQueryCycle(query->cycle, issue.flags, true))
            return Status::InvalidCoordinator;
          fact.queryType = query->cycle.type;
          fact.queryFlags = issue.flags;
          fact.issueCycle = query->cycle.issueCycle;
          fact.issueOrdinal = query->cycle.issueOrdinal;
        }
        records.push_back(fact);
        if (records.size() > kRenderTapeIdentityV3MaxRows) return Status::CapacityExceeded;
      }
    }
    events.push_back(eventFact);
    if (events.size() > kRenderTapeIdentityV3MaxRows) return Status::CapacityExceeded;
  }
  return Status::Valid;
}

} // namespace

bool renderTapeIdentityV3ObservationAdmitted(
    const RenderTapeIdentityV3Header& header,
    const RenderTapeIdentityV3Coordinator& row,
    const RenderTapeIdentityV3CurrentIssue& current,
    std::span<const RenderTapeIdentityV3Source> sources,
    std::span<const RenderTapeIdentityV3Initializer> initializers,
    RenderTapeIdentityV3Observation observation) noexcept {
  if (!headerAdmitted(header) ||
      !renderTapeIdentityV3CurrentIssueAdmitted(header, row, current) ||
      !coordinatorShapeAdmitted(header, row, sources, initializers)) return false;
  const auto kind = static_cast<Prefix>(row.prefixKind);
  if (observation == Observation::Pending) return kind != Prefix::None;
  if (observation != Observation::Ready && observation != Observation::Completed)
    return false;
  if (row.prefixInitializer > header.baselineCompletedInitializer &&
      renderTapeIdentityV3InitializerPhase(initializers[row.initializerIndex]) != Phase::Completed)
    return false;
  if (kind == Prefix::EmptyBaseline || kind == Prefix::ArmBaseline) return true;
  if (kind != Prefix::RealSource || row.sourceIndex >= sources.size()) return false;
  const auto& source = sources[row.sourceIndex];
  return renderTapeIdentityV3SourcePhase(source) == Phase::Completed;
}

Status validateRenderTapeIdentityV3Rows(
    const RenderTapeIdentityV3View& identity,
    std::span<const RenderTapeIdentityV3EventFact> events,
    std::span<const RenderTapeIdentityV3RecordFact> records,
    bool requireComplete) noexcept {
  if (!headerAdmitted(identity.header)) return Status::InvalidHeader;
  if (identity.sources.size() > kRenderTapeIdentityV3MaxRows ||
      identity.initializers.size() > kRenderTapeIdentityV3MaxRows ||
      identity.commands.size() > kRenderTapeIdentityV3MaxRows ||
      identity.dispositions.size() > kRenderTapeIdentityV3MaxRows ||
      identity.coordinators.size() > kRenderTapeIdentityV3MaxRows ||
      identity.settlements.size() > kRenderTapeIdentityV3MaxRows ||
      events.size() > kRenderTapeIdentityV3MaxRows ||
      records.size() > kRenderTapeIdentityV3MaxRows) return Status::CapacityExceeded;
  try {
    std::vector<Position> cuts;
    std::vector<std::uint64_t> edges{identity.header.baselineCompletionEdge};
    std::vector<std::uint64_t> commandTails(identity.sources.size(), 0u);
    struct CommandAnchors {
      Position first{UINT64_MAX, UINT64_MAX};
      Position last{};
      bool covered = false;
    };
    std::vector<CommandAnchors> commandAnchors(identity.commands.size());
    std::vector<std::uint64_t> subdrawTails(identity.commands.size(), 0u);
    std::vector<bool> coordinatorUsed(identity.coordinators.size(), false);
    std::uint64_t priorSource = 0u;
    std::uint64_t priorSequence = identity.header.baselineCompletedSequence;
    Position priorCut{};
    for (const auto& source : identity.sources) {
      const auto phase = renderTapeIdentityV3SourcePhase(source);
      if (phase == Phase::Invalid || source.sourceOrdinal <= priorSource ||
          source.seqId <= priorSequence ||
          source.publicationEdge <= identity.header.baselineCompletionEdge)
        return Status::InvalidSource;
      if (requireComplete && phase != Phase::Completed) return Status::Incomplete;
      Position cut{};
      if (!cutPosition(source, events, cut) || cut < priorCut) return Status::InvalidCut;
      if (!cuts.empty() && source.publicationEdge <= identity.sources[cuts.size() - 1u].publicationEdge)
        return Status::InvalidSource;
      for (std::size_t i = 0u; i < cuts.size(); ++i)
        if (source.writerCaptureId == identity.sources[i].writerCaptureId)
          return Status::InvalidSource;
      cuts.push_back(cut);
      edges.push_back(source.publicationEdge);
      if (source.submissionEdge) edges.push_back(source.submissionEdge);
      if (source.completionEdge) edges.push_back(source.completionEdge);
      priorSource = source.sourceOrdinal;
      priorSequence = source.seqId;
      priorCut = cut;
    }
    std::uint64_t priorInitializer = identity.header.baselineCompletedInitializer;
    std::uint64_t priorInitializerEdge = identity.header.baselineCompletionEdge;
    Position priorInitializerCut{};
    for (const auto& initializer : identity.initializers) {
      const auto phase = renderTapeIdentityV3InitializerPhase(initializer);
      Position cut{};
      if (phase == Phase::Invalid || initializer.epoch != identity.header.initializerEpoch ||
          initializer.sequence <= priorInitializer || initializer.publicationEdge <= priorInitializerEdge)
        return Status::InvalidSource;
      if (!cutPosition(initializer, events, cut) || cut < priorInitializerCut) return Status::InvalidCut;
      if (requireComplete && phase != Phase::Completed) return Status::Incomplete;
      edges.push_back(initializer.publicationEdge);
      if (initializer.submissionEdge) edges.push_back(initializer.submissionEdge);
      if (initializer.completionEdge) edges.push_back(initializer.completionEdge);
      priorInitializer = initializer.sequence;
      priorInitializerEdge = initializer.publicationEdge;
      priorInitializerCut = cut;
    }
    std::uint32_t priorCommandSource = 0u;
    bool haveCommand = false;
    for (std::size_t index = 0u; index < identity.commands.size(); ++index) {
      const auto& command = identity.commands[index];
      if (!commandShapeAdmitted(command, identity.sources, identity.coordinators) ||
          (haveCommand && command.sourceIndex < priorCommandSource) ||
          command.physicalCommandIndex != commandTails[command.sourceIndex])
        return Status::InvalidDisposition;
      ++commandTails[command.sourceIndex];
      haveCommand = true;
      priorCommandSource = command.sourceIndex;
      for (std::size_t prior = 0u; prior < index; ++prior) {
        const auto& priorCommand = identity.commands[prior];
        if (command.logicalPassId && priorCommand.logicalPassId == command.logicalPassId &&
            (priorCommand.dagPassIndex != command.dagPassIndex ||
             priorCommand.passKind != command.passKind)) return Status::InvalidDisposition;
      }
      if (command.coordinatorIndex != kRenderTapeIdentityV3NoIndex) {
        const auto& owner = identity.coordinators[command.coordinatorIndex];
        const Position start{owner.eventOrdinal, owner.recordIndex};
        const Position end{owner.eventOrdinal, std::uint64_t{owner.recordIndex} + 1u};
        if (cuts[command.sourceIndex] < end) return Status::InvalidCut;
        commandAnchors[index] = {start, end, true};
        subdrawTails[index] = command.drawCount;
      }
    }
    for (std::size_t index = 0u; index < commandTails.size(); ++index)
      if (commandTails[index] != identity.sources[index].commandCount)
        return Status::InvalidDisposition;

    std::size_t recordCursor = 0u;
    std::size_t dispositionCursor = 0u;
    std::size_t settlementCursor = 0u;
    std::uint64_t priorEvent = 0u;
    std::uint64_t priorRaw = 0u;
    for (const auto& event : events) {
      if (!event.ordinal || event.ordinal <= priorEvent) return Status::InvalidTape;
      priorEvent = event.ordinal;
      if (event.type != static_cast<std::uint32_t>(RenderTapeEventType::CommandChunk)) {
        if (event.recordCount) return Status::InvalidTape;
        continue;
      }
      std::uint64_t covered = 0u;
      while (dispositionCursor < identity.dispositions.size() &&
             identity.dispositions[dispositionCursor].eventOrdinal == event.ordinal) {
        const auto& row = identity.dispositions[dispositionCursor++];
        const auto end = std::uint64_t{row.firstRecord} + row.recordCount;
        if (!row.recordCount || row.firstRecord != covered || end > event.recordCount || row.reserved0)
          return Status::RecordCoverageMismatch;
        const auto kind = static_cast<Kind>(row.kind);
        if (kind == Kind::GpuCommand) {
          if (row.commandIndex >= identity.commands.size() ||
              row.coordinatorIndex != kRenderTapeIdentityV3NoIndex)
            return Status::InvalidDisposition;
          const auto& command = identity.commands[row.commandIndex];
          auto& anchors = commandAnchors[row.commandIndex];
          if (command.coordinatorIndex != kRenderTapeIdentityV3NoIndex ||
              cuts[command.sourceIndex] < Position{event.ordinal, end})
            return Status::InvalidDisposition;
          if (command.kind == static_cast<std::uint32_t>(RenderTapeIdentityV3CommandKind::DrawRun)) {
            const auto subdrawEnd = std::uint64_t{row.firstSubdraw} + row.subdrawCount;
            if (!row.subdrawCount || row.subdrawCount != row.recordCount ||
                row.firstSubdraw != subdrawTails[row.commandIndex] || subdrawEnd > command.drawCount)
              return Status::InvalidDisposition;
            subdrawTails[row.commandIndex] = subdrawEnd;
          } else if (row.firstSubdraw || row.subdrawCount || row.recordCount != 1u || anchors.covered)
            return Status::InvalidDisposition;
          anchors.first = std::min(anchors.first, Position{event.ordinal, row.firstRecord});
          anchors.last = std::max(anchors.last, Position{event.ordinal, end});
          anchors.covered = true;
        } else if (kind == Kind::StateOnly) {
          if (!noGpuMembership(row) || row.coordinatorIndex != kRenderTapeIdentityV3NoIndex)
            return Status::InvalidDisposition;
        } else if (kind == Kind::Coordinator) {
          if (!noGpuMembership(row) || row.recordCount != 1u ||
              row.coordinatorIndex >= identity.coordinators.size() || coordinatorUsed[row.coordinatorIndex])
            return Status::InvalidCoordinator;
          coordinatorUsed[row.coordinatorIndex] = true;
        } else return Status::InvalidDisposition;
        for (std::uint64_t index = row.firstRecord; index < end; ++index) {
          if (recordCursor >= records.size()) return Status::RecordCoverageMismatch;
          const auto& fact = records[recordCursor++];
          if (fact.eventOrdinal != event.ordinal || fact.recordIndex != index ||
              !renderTapeIdentityV3DispositionAdmitted(fact.recordType, kind))
            return Status::InvalidDisposition;
          if (kind == Kind::GpuCommand &&
              identity.commands[row.commandIndex].kind != static_cast<std::uint32_t>(
                  renderTapeIdentityV3RecordCommandKind(fact.recordType)))
            return Status::InvalidDisposition;
          if (kind == Kind::Coordinator) {
            const auto& control = identity.coordinators[row.coordinatorIndex];
            if (!coordinatorShapeAdmitted(identity.header, control, identity.sources, identity.initializers) ||
                control.eventOrdinal != event.ordinal || control.recordIndex != index ||
                control.recordType != fact.recordType || control.releaseAction != fact.requiredReleaseAction)
              return Status::InvalidCoordinator;
            if (fact.recordType == D9C_COMMAND_RECORD_QUERY_ISSUE &&
                (control.flags != fact.queryFlags || control.queryType != fact.queryType ||
                 !renderTapeIdentityV3SameIdentity(control.queryIdentity, fact.queryIdentity) ||
                 control.issueCycle != fact.issueCycle || control.issueOrdinal != fact.issueOrdinal))
              return Status::InvalidCoordinator;
            if (control.prefixInitializer > identity.header.baselineCompletedInitializer) {
              Position initializerCut{};
              if (!cutPosition(identity.initializers[control.initializerIndex], events, initializerCut) ||
                  initializerCut > Position{event.ordinal, index + 1u}) return Status::InvalidPrefix;
            }
          }
        }
        covered = end;
      }
      if (covered != event.recordCount) return Status::RecordCoverageMismatch;
      if (settlementCursor >= identity.settlements.size()) {
        if (requireComplete) return Status::Incomplete;
        continue;
      }
      const auto& settlement = identity.settlements[settlementCursor];
      if (settlement.eventOrdinal != event.ordinal) {
        if (!requireComplete && settlement.eventOrdinal > event.ordinal) continue;
        return Status::InvalidSettlement;
      }
      if (!settlement.rawOrdinal || settlement.rawOrdinal <= priorRaw ||
          !settlement.buildGeneration || settlement.replayEdge <= identity.header.baselineCompletionEdge ||
          settlement.recordCount != event.recordCount || settlement.reserved0)
        return Status::InvalidSettlement;
      for (const auto& control : identity.coordinators)
        if (control.eventOrdinal == event.ordinal &&
            (control.rawOrdinal != settlement.rawOrdinal ||
             control.buildGeneration != settlement.buildGeneration ||
             control.dispatchEdge >= settlement.replayEdge)) return Status::InvalidSettlement;
      edges.push_back(settlement.replayEdge);
      priorRaw = settlement.rawOrdinal;
      ++settlementCursor;
    }
    if (recordCursor != records.size() || dispositionCursor != identity.dispositions.size())
      return Status::RecordCoverageMismatch;
    if (settlementCursor != identity.settlements.size()) return Status::InvalidSettlement;
    if (!std::all_of(coordinatorUsed.begin(), coordinatorUsed.end(), [](bool value) { return value; }))
      return Status::InvalidCoordinator;
    // Physical commands are counted once; canonical DrawRun contributors
    // partition subdraws even when state-only records separate them.
    for (std::size_t index = 0u; index < identity.commands.size(); ++index)
      if (!commandAnchors[index].covered || subdrawTails[index] != identity.commands[index].drawCount)
        return Status::InvalidDisposition;
    for (std::size_t index = 1u; index < identity.commands.size(); ++index)
      if (commandAnchors[index - 1u].last > commandAnchors[index].first &&
          (identity.commands[index].coordinatorIndex == kRenderTapeIdentityV3NoIndex ||
           identity.commands[index - 1u].coordinatorIndex != identity.commands[index].coordinatorIndex))
        return Status::InvalidDisposition;
    for (std::size_t index = 0u; index < identity.commands.size(); ++index) {
      if (identity.commands[index].coordinatorIndex != kRenderTapeIdentityV3NoIndex) continue;
      for (const auto& control : identity.coordinators) {
        const Position boundary{control.eventOrdinal, control.recordIndex};
        if (commandAnchors[index].first < boundary && boundary < commandAnchors[index].last)
          return Status::InvalidDisposition;
      }
    }
    for (std::size_t controlIndex = 0u; controlIndex < identity.coordinators.size(); ++controlIndex) {
      const auto& control = identity.coordinators[controlIndex];
      edges.push_back(control.dispatchEdge);
      if (control.recordType != D9C_COMMAND_RECORD_QUERY_ISSUE || control.flags == 2u) continue;
      const auto kind = static_cast<Prefix>(control.prefixKind);
      if (kind == Prefix::Unbound) {
        if (requireComplete) return Status::Incomplete;
        continue;
      }
      if (kind == Prefix::RealSource) {
        const auto& source = identity.sources[control.sourceIndex];
        const Position endPosition{control.eventOrdinal, control.recordIndex};
        if (cuts[control.sourceIndex] <= endPosition && control.commandEnd != source.commandCount)
          return Status::InvalidPrefix;
        std::uint64_t exactEnd = 0u;
        for (std::size_t commandIndex = 0u; commandIndex < identity.commands.size(); ++commandIndex) {
          const auto& command = identity.commands[commandIndex];
          const auto& anchors = commandAnchors[commandIndex];
          if (command.sourceIndex != control.sourceIndex) continue;
          if (anchors.last <= endPosition || command.coordinatorIndex == controlIndex) {
            exactEnd = std::max(exactEnd, std::uint64_t{command.physicalCommandIndex} + 1u);
          } else if (anchors.first < endPosition || command.physicalCommandIndex < control.commandEnd)
            return Status::InvalidPrefix;
        }
        if (exactEnd != control.commandEnd) return Status::InvalidPrefix;
      }
      if (control.observation != static_cast<std::uint32_t>(Observation::None)) {
        const RenderTapeIdentityV3CurrentIssue current{
            {identity.header.captureToken, identity.header.captureEpoch},
            control.queryIdentity, control.issueCycle, control.issueOrdinal, control.deviceEpoch, false};
        if (!renderTapeIdentityV3ObservationAdmitted(identity.header, control, current,
                identity.sources, identity.initializers,
                static_cast<Observation>(control.observation))) return Status::InvalidPrefix;
      }
    }
    std::sort(edges.begin(), edges.end());
    if (std::adjacent_find(edges.begin(), edges.end()) != edges.end()) return Status::InvalidSource;
    return Status::Valid;
  } catch (...) {
    return Status::AllocationFailed;
  }
}

Status validateRenderTapeIdentityV3(std::span<const std::byte> tape,
    const RenderTapeBlobCatalogue& catalogue,
    std::span<const std::byte> sidecar, RenderTapeIdentityV3View* out,
    bool requireComplete) noexcept {
  if (out) *out = {};
  RenderTapeIdentityV3View identity{};
  if (!load(sidecar, 0u, identity.header) || !headerAdmitted(identity.header))
    return Status::InvalidHeader;
  const auto& header = identity.header;
  std::uint64_t cursor = sizeof(header);
  if (!tableAdmitted(header.sources, cursor, sizeof(RenderTapeIdentityV3Source)) ||
      !tableAdmitted(header.initializers, cursor, sizeof(RenderTapeIdentityV3Initializer)) ||
      !tableAdmitted(header.commands, cursor, sizeof(RenderTapeIdentityV3Command)) ||
      !tableAdmitted(header.dispositions, cursor, sizeof(RenderTapeIdentityV3Disposition)) ||
      !tableAdmitted(header.coordinators, cursor, sizeof(RenderTapeIdentityV3Coordinator)) ||
      !tableAdmitted(header.settlements, cursor, sizeof(RenderTapeIdentityV3ReplaySettlement)) ||
      cursor != sidecar.size()) return Status::InvalidLayout;
  if (header.eventsBytes != tape.size() ||
      header.eventsDigest != RenderTapeCaptureSession::sha256(tape)) return Status::EventsMismatch;
  try {
    copyTable(identity.sources, sidecar, header.sources);
    copyTable(identity.initializers, sidecar, header.initializers);
    copyTable(identity.commands, sidecar, header.commands);
    copyTable(identity.dispositions, sidecar, header.dispositions);
    copyTable(identity.coordinators, sidecar, header.coordinators);
    copyTable(identity.settlements, sidecar, header.settlements);
    std::vector<RenderTapeIdentityV3EventFact> events;
    std::vector<RenderTapeIdentityV3RecordFact> records;
    auto status = canonicalFacts(tape, catalogue, header, events, records);
    if (status != Status::Valid) return status;
    status = validateRenderTapeIdentityV3Rows(identity, events, records, requireComplete);
    if (status == Status::Valid && out) *out = std::move(identity);
    return status;
  } catch (...) {
    return Status::AllocationFailed;
  }
}

std::vector<std::byte> buildRenderTapeIdentityV3(std::span<const std::byte> tape,
    const RenderTapeBlobCatalogue& catalogue,
    const RenderTapeIdentityV3View& identity, Status* status) {
  if (status) *status = Status::InvalidHeader;
  try {
    auto header = identity.header;
    header.eventsBytes = tape.size();
    header.eventsDigest = RenderTapeCaptureSession::sha256(tape);
    std::uint64_t size = 0u;
    if (!layout(header, identity, size)) {
      if (status) *status = Status::CapacityExceeded;
      return {};
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    std::memcpy(bytes.data(), &header, sizeof(header));
    storeTable(bytes, header.sources, identity.sources);
    storeTable(bytes, header.initializers, identity.initializers);
    storeTable(bytes, header.commands, identity.commands);
    storeTable(bytes, header.dispositions, identity.dispositions);
    storeTable(bytes, header.coordinators, identity.coordinators);
    storeTable(bytes, header.settlements, identity.settlements);
    const auto result = validateRenderTapeIdentityV3(tape, catalogue, bytes);
    if (status) *status = result;
    return result == Status::Valid ? std::move(bytes) : std::vector<std::byte>{};
  } catch (...) {
    if (status) *status = Status::AllocationFailed;
    return {};
  }
}

bool RenderTapeIdentityV3Ledger::begin(RenderTapeIdentityV3Header header,
                                     RenderTapeIdentityV3Limits limits) noexcept {
  if (active_ || failed_ || !headerAdmitted(header) ||
      limits.maxSources > kRenderTapeIdentityV3MaxRows ||
      limits.maxInitializers > kRenderTapeIdentityV3MaxRows ||
      limits.maxCommands > kRenderTapeIdentityV3MaxRows ||
      limits.maxDispositions > kRenderTapeIdentityV3MaxRows ||
      limits.maxCoordinators > kRenderTapeIdentityV3MaxRows ||
      limits.maxSettlements > kRenderTapeIdentityV3MaxRows ||
      std::uint64_t{limits.maxSources} + limits.maxCoordinators > kRenderTapeIdentityV3MaxRows ||
      header.baselineCompletionEdge == UINT64_MAX) return reject();
  try {
    identity_.sources.reserve(limits.maxSources);
    identity_.initializers.reserve(limits.maxInitializers);
    identity_.commands.reserve(limits.maxCommands);
    identity_.dispositions.reserve(limits.maxDispositions);
    identity_.coordinators.reserve(limits.maxCoordinators);
    identity_.settlements.reserve(limits.maxSettlements);
    sourceOwners_.reserve(limits.maxSources);
    queryOwners_.reserve(limits.maxCoordinators);
    writers_.reserve(std::size_t{limits.maxSources} + limits.maxCoordinators);
  } catch (...) { return reject(); }
  identity_.header = header;
  limits_ = limits;
  nextEdge_ = header.baselineCompletionEdge + 1u;
  active_ = true;
  return true;
}

bool RenderTapeIdentityV3Ledger::admit(RenderTapeIdentityV3CaptureKey key) noexcept {
  if (!active_ || failed_ || key.token != identity_.header.captureToken ||
      key.epoch != identity_.header.captureEpoch) return reject();
  return true;
}

std::uint64_t RenderTapeIdentityV3Ledger::edge() noexcept {
  if (!nextEdge_ || nextEdge_ == UINT64_MAX) { reject(); return 0u; }
  return nextEdge_++;
}

std::uint64_t RenderTapeIdentityV3Ledger::writerId(query::WriterIdentity owner) noexcept {
  if (!owner.valid() || owner.queue != identity_.header.queue) { reject(); return 0u; }
  for (const auto& writer : writers_)
    if (writer.owner == owner) return writer.captureId;
  if (writers_.size() >= std::size_t{limits_.maxSources} + limits_.maxCoordinators) {
    reject(); return 0u;
  }
  try {
    const auto id = static_cast<std::uint64_t>(writers_.size()) + 1u;
    writers_.push_back({owner, id});
    return id;
  } catch (...) { reject(); return 0u; }
}

bool RenderTapeIdentityV3Ledger::publishSource(RenderTapeIdentityV3CaptureKey key,
    RenderTapeIdentityV3Source source, query::WriterIdentity owner,
    std::uint32_t* sourceIndex) noexcept {
  if (sourceIndex) *sourceIndex = kRenderTapeIdentityV3NoIndex;
  if (!admit(key) || identity_.sources.size() >= limits_.maxSources ||
      source.submissionEdge || source.completionEdge || source.completedSequence ||
      (source.writerGeneration && source.writerGeneration != owner.generation) ||
      (source.storageGeneration && source.storageGeneration != owner.storageGeneration)) return reject();
  source.writerCaptureId = writerId(owner);
  source.writerGeneration = owner.generation;
  source.storageGeneration = owner.storageGeneration;
  source.publicationEdge = edge();
  if (failed_ || renderTapeIdentityV3SourcePhase(source) != Phase::Published ||
      source.seqId <= identity_.header.baselineCompletedSequence) return reject();
  if (!identity_.sources.empty() &&
      (source.seqId <= identity_.sources.back().seqId ||
       source.sourceOrdinal <= identity_.sources.back().sourceOrdinal)) return reject();
  for (const auto& prior : identity_.sources)
    if (prior.writerCaptureId == source.writerCaptureId) return reject();
  try {
    identity_.sources.push_back(source);
    sourceOwners_.push_back(owner);
  } catch (...) { return reject(); }
  if (sourceIndex) *sourceIndex = static_cast<std::uint32_t>(identity_.sources.size() - 1u);
  return true;
}

bool RenderTapeIdentityV3Ledger::appendCommand(RenderTapeIdentityV3CaptureKey key,
    RenderTapeIdentityV3Command command) noexcept {
  if (!admit(key) || identity_.commands.size() >= limits_.maxCommands ||
      !commandShapeAdmitted(command, identity_.sources, identity_.coordinators)) return reject();
  std::uint64_t expected = 0u;
  if (!identity_.commands.empty()) {
    const auto& prior = identity_.commands.back();
    if (command.sourceIndex < prior.sourceIndex) return reject();
    if (command.sourceIndex == prior.sourceIndex)
      expected = std::uint64_t{prior.physicalCommandIndex} + 1u;
  }
  if (command.physicalCommandIndex != expected) return reject();
  try { identity_.commands.push_back(command); }
  catch (...) { return reject(); }
  return true;
}

bool RenderTapeIdentityV3Ledger::appendDisposition(RenderTapeIdentityV3CaptureKey key,
    RenderTapeIdentityV3Disposition row, std::uint32_t canonicalRecordType) noexcept {
  if (!admit(key) || identity_.dispositions.size() >= limits_.maxDispositions ||
      !row.eventOrdinal || !row.recordCount ||
      std::uint64_t{row.firstRecord} + row.recordCount > UINT32_MAX ||
      row.reserved0 ||
      !renderTapeIdentityV3DispositionAdmitted(canonicalRecordType, static_cast<Kind>(row.kind)))
    return reject();
  if (!identity_.dispositions.empty()) {
    const auto& prior = identity_.dispositions.back();
    if (row.eventOrdinal < prior.eventOrdinal ||
        (row.eventOrdinal == prior.eventOrdinal &&
         row.firstRecord != std::uint64_t{prior.firstRecord} + prior.recordCount)) return reject();
  }
  if (row.kind == static_cast<std::uint32_t>(Kind::GpuCommand)) {
    if (row.commandIndex >= identity_.commands.size()) return reject();
    const auto& command = identity_.commands[row.commandIndex];
    if (command.coordinatorIndex != kRenderTapeIdentityV3NoIndex ||
        command.kind != static_cast<std::uint32_t>(
            renderTapeIdentityV3RecordCommandKind(canonicalRecordType))) return reject();
    if (command.kind == static_cast<std::uint32_t>(RenderTapeIdentityV3CommandKind::DrawRun)) {
      if (!row.subdrawCount || row.subdrawCount != row.recordCount ||
          std::uint64_t{row.firstSubdraw} + row.subdrawCount > command.drawCount) return reject();
    } else if (row.recordCount != 1u || row.firstSubdraw || row.subdrawCount) return reject();
  } else if (!noGpuMembership(row)) return reject();
  if (row.kind == static_cast<std::uint32_t>(Kind::Coordinator)) {
    if (row.recordCount != 1u || row.coordinatorIndex >= identity_.coordinators.size()) return reject();
  } else if (row.coordinatorIndex != kRenderTapeIdentityV3NoIndex) return reject();
  try { identity_.dispositions.push_back(row); }
  catch (...) { return reject(); }
  return true;
}

bool RenderTapeIdentityV3Ledger::publishInitializer(RenderTapeIdentityV3CaptureKey key,
    RenderTapeIdentityV3Initializer initializer, std::uint32_t* initializerIndex) noexcept {
  if (initializerIndex) *initializerIndex = kRenderTapeIdentityV3NoIndex;
  if (!admit(key) || identity_.initializers.size() >= limits_.maxInitializers ||
      initializer.epoch != identity_.header.initializerEpoch ||
      initializer.sequence <= identity_.header.baselineCompletedInitializer ||
      initializer.submissionEdge || initializer.completionEdge || initializer.completedSequence)
    return reject();
  initializer.publicationEdge = edge();
  if (failed_ || renderTapeIdentityV3InitializerPhase(initializer) != Phase::Published ||
      (!identity_.initializers.empty() &&
       initializer.sequence <= identity_.initializers.back().sequence)) return reject();
  try { identity_.initializers.push_back(initializer); }
  catch (...) { return reject(); }
  if (initializerIndex) *initializerIndex = static_cast<std::uint32_t>(identity_.initializers.size() - 1u);
  return true;
}

bool RenderTapeIdentityV3Ledger::dispatchCoordinator(RenderTapeIdentityV3CaptureKey key,
    RenderTapeIdentityV3Coordinator row, query::WriterIdentity owner,
    std::uint32_t* coordinatorIndex) noexcept {
  if (coordinatorIndex) *coordinatorIndex = kRenderTapeIdentityV3NoIndex;
  if (!admit(key) || identity_.coordinators.size() >= limits_.maxCoordinators) return reject();
  if (row.prefixKind == static_cast<std::uint32_t>(Prefix::Unbound)) {
    row.writerCaptureId = writerId(owner);
    row.writerGeneration = owner.generation;
    row.storageGeneration = owner.storageGeneration;
  } else if (owner.valid()) return reject();
  row.dispatchEdge = edge();
  if (failed_ || !coordinatorShapeAdmitted(identity_.header, row,
          identity_.sources, identity_.initializers)) return reject();
  for (const auto& prior : identity_.coordinators)
    if (prior.eventOrdinal == row.eventOrdinal && prior.recordIndex == row.recordIndex) return reject();
  try {
    identity_.coordinators.push_back(row);
    queryOwners_.push_back(owner);
  } catch (...) { return reject(); }
  if (coordinatorIndex) *coordinatorIndex = static_cast<std::uint32_t>(identity_.coordinators.size() - 1u);
  return true;
}

bool RenderTapeIdentityV3Ledger::settleReplay(RenderTapeIdentityV3CaptureKey key,
    RenderTapeIdentityV3ReplaySettlement row) noexcept {
  if (!admit(key) || identity_.settlements.size() >= limits_.maxSettlements ||
      !row.eventOrdinal || !row.rawOrdinal || !row.buildGeneration || row.reserved0) return reject();
  if (!identity_.settlements.empty() &&
      (row.eventOrdinal <= identity_.settlements.back().eventOrdinal ||
       row.rawOrdinal <= identity_.settlements.back().rawOrdinal)) return reject();
  row.replayEdge = edge();
  if (failed_) return false;
  try { identity_.settlements.push_back(row); }
  catch (...) { return reject(); }
  return true;
}

bool RenderTapeIdentityV3Ledger::submitSource(RenderTapeIdentityV3CaptureKey key,
                                            std::uint32_t index) noexcept {
  if (!admit(key) || index >= identity_.sources.size()) return reject();
  const auto stamp = edge();
  return !failed_ && renderTapeIdentityV3SubmitSource(identity_.sources[index], stamp)
      ? true : reject();
}

bool RenderTapeIdentityV3Ledger::completeSource(RenderTapeIdentityV3CaptureKey key,
    std::uint32_t index, std::uint64_t completedSequence) noexcept {
  if (!admit(key) || index >= identity_.sources.size()) return reject();
  const auto stamp = edge();
  return !failed_ && renderTapeIdentityV3CompleteSource(identity_.sources[index], stamp, completedSequence)
      ? true : reject();
}

bool RenderTapeIdentityV3Ledger::submitInitializer(RenderTapeIdentityV3CaptureKey key,
                                                 std::uint32_t index) noexcept {
  if (!admit(key) || index >= identity_.initializers.size()) return reject();
  const auto stamp = edge();
  return !failed_ && renderTapeIdentityV3SubmitInitializer(identity_.initializers[index], stamp)
      ? true : reject();
}

bool RenderTapeIdentityV3Ledger::completeInitializer(RenderTapeIdentityV3CaptureKey key,
    std::uint32_t index, std::uint64_t completedInitializer) noexcept {
  if (!admit(key) || index >= identity_.initializers.size()) return reject();
  const auto stamp = edge();
  return !failed_ && renderTapeIdentityV3CompleteInitializer(identity_.initializers[index], stamp, completedInitializer)
      ? true : reject();
}

bool RenderTapeIdentityV3Ledger::bindQueryPrefix(RenderTapeIdentityV3CaptureKey key,
    std::uint32_t coordinatorIndex, std::uint32_t sourceIndex) noexcept {
  if (!admit(key) || coordinatorIndex >= identity_.coordinators.size() ||
      sourceIndex >= identity_.sources.size()) return reject();
  auto& row = identity_.coordinators[coordinatorIndex];
  const auto& source = identity_.sources[sourceIndex];
  if (!renderTapeIdentityV3BindPrefixAdmitted(row, source,
          queryOwners_[coordinatorIndex], sourceOwners_[sourceIndex])) return reject();
  row.prefixKind = static_cast<std::uint32_t>(Prefix::RealSource);
  row.sourceIndex = sourceIndex;
  row.prefixSequence = source.seqId;
  // commandEnd and initializer are intentionally unchanged by publication.
  return true;
}

bool RenderTapeIdentityV3Ledger::observeQuery(RenderTapeIdentityV3CaptureKey key,
    std::uint32_t index, const RenderTapeIdentityV3CurrentIssue& current,
    RenderTapeIdentityV3Observation observation) noexcept {
  if (!admit(key) || index >= identity_.coordinators.size()) return reject();
  auto& row = identity_.coordinators[index];
  if (!renderTapeIdentityV3ObservationAdmitted(identity_.header, row, current,
          identity_.sources, identity_.initializers, observation)) return reject();
  row.observation = static_cast<std::uint32_t>(observation);
  return true;
}

bool RenderTapeIdentityV3Ledger::snapshot(RenderTapeIdentityV3CaptureKey key,
                                        RenderTapeIdentityV3View& out) const noexcept {
  out = {};
  if (!active_ || failed_ || key.token != identity_.header.captureToken ||
      key.epoch != identity_.header.captureEpoch) return false;
  try { out = identity_; }
  catch (...) { out = {}; return false; }
  return true;
}

bool RenderTapeIdentityV3Ledger::serialize(RenderTapeIdentityV3CaptureKey key,
    std::span<const std::byte> tape, const RenderTapeBlobCatalogue& catalogue,
    std::vector<std::byte>& out) noexcept {
  out.clear();
  if (!admit(key)) return false;
  Status status{};
  auto bytes = buildRenderTapeIdentityV3(tape, catalogue, identity_, &status);
  if (status != Status::Valid) return reject();
  out = std::move(bytes);
  return true;
}

void RenderTapeIdentityV3Ledger::fail(RenderTapeIdentityV3CaptureKey key) noexcept {
  if (active_ && key.token == identity_.header.captureToken &&
      key.epoch == identity_.header.captureEpoch) failed_ = true;
}

} // namespace dxmt9::d3d9
