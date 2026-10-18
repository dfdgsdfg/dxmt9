#include "device_c_render_tape_provenance_validation.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

namespace dxmt9::d3d9 {
namespace {

using Prefix = RenderTapeIdentityV3PrefixKind;
using Phase = RenderTapeIdentityV3SourcePhase;
using Kind = RenderTapeIdentityV3DispositionKind;
using Status = TapeProvenanceStatus;
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

bool noGpuMembership(const RenderTapeIdentityV3Disposition& row) noexcept {
  return row.commandIndex == kRenderTapeIdentityV3NoIndex &&
      !row.firstSubdraw && !row.subdrawCount;
}

template <typename Sources>
bool prefixShapeAdmitted(const TapeProvenanceContext& context,
    const RenderTapeIdentityV3Coordinator& row,
    const Sources& sources,
    std::span<const RenderTapeIdentityV3Initializer> initializers) noexcept {
  if (!row.prefixInitializer) {
    if (row.prefixInitializerEpoch || row.initializerIndex != kRenderTapeIdentityV3NoIndex)
      return false;
  } else {
    if (row.prefixInitializerEpoch != context.initializerEpoch) return false;
    if (row.prefixInitializer <= context.baselineCompletedInitializer) {
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
    return row.prefixKind == context.baselineKind &&
        row.sourceIndex == kRenderTapeIdentityV3NoIndex &&
        row.prefixSequence == context.baselineSequence &&
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

bool currentIssueAdmitted(const TapeProvenanceContext& context,
    const RenderTapeIdentityV3Coordinator& coordinator,
    const RenderTapeIdentityV3CurrentIssue& current) noexcept {
  return current.capture.token == context.captureToken &&
      current.capture.epoch == context.captureEpoch && !current.cancelled &&
      coordinator.queryIdentity.kind == D9C_CHUNK_HANDLE_KIND_QUERY &&
      coordinator.queryIdentity.objectId && coordinator.queryIdentity.generation &&
      renderTapeIdentityV3SameIdentity(coordinator.queryIdentity, current.identity) &&
      coordinator.issueCycle && coordinator.issueCycle == current.cycle &&
      coordinator.issueOrdinal && coordinator.issueOrdinal == current.ordinal &&
      coordinator.deviceEpoch && coordinator.deviceEpoch == current.deviceEpoch &&
      coordinator.flags == 1u && coordinator.dispatchResult >= 0;
}

} // namespace

bool tapeProvenanceContextAdmitted(const TapeProvenanceContext& context) noexcept {
  const auto kind = static_cast<Prefix>(context.baselineKind);
  return context.captureToken && context.captureEpoch && context.queue &&
      (context.coordinatorDomain == TapeCoordinatorAuthority::SessionRelease ||
       context.coordinatorDomain == TapeCoordinatorAuthority::OrdinarySerial) &&
      context.baselineCompletionEdge && context.initializerEpoch &&
      context.baselineCompletedSequence >= context.baselineSequence &&
      context.baselineCompletedInitializer >= context.baselineInitializer &&
      ((kind == Prefix::EmptyBaseline && !context.baselineSequence &&
        !context.baselineCompletedSequence) ||
       (kind == Prefix::ArmBaseline && context.baselineSequence));
}

namespace {

template <typename Sources, typename Coordinators>
bool commandShapeAdmitted(const TapePhysicalCommand& row,
    const Sources& sources,
    const Coordinators& coordinators) noexcept {
  const bool draw = row.kind == static_cast<std::uint32_t>(RenderTapeIdentityV3CommandKind::DrawRun);
  if (row.sourceIndex >= sources.size() ||
      row.physicalCommandIndex >= sources[row.sourceIndex].commandCount ||
      row.kind < static_cast<std::uint32_t>(RenderTapeIdentityV3CommandKind::DrawRun) ||
      row.kind > static_cast<std::uint32_t>(RenderTapeIdentityV3CommandKind::Present) ||
      draw != (row.drawCount != 0u) || row.drawCount > kRenderTapeIdentityV3MaxRows ||
      (row.coordinatorIndex != kRenderTapeIdentityV3NoIndex && row.coordinatorIndex >= coordinators.size()))
    return false;
  if (row.coordinatorIndex != kRenderTapeIdentityV3NoIndex &&
      !renderTapeIdentityV3CoordinatorCommandAdmitted(
          coordinators[row.coordinatorIndex].recordType,
          static_cast<RenderTapeIdentityV3CommandKind>(row.kind))) return false;
  return true;
}

template <typename Sources>
bool coordinatorShapeAdmitted(const TapeProvenanceContext& context,
    const RenderTapeIdentityV3Coordinator& row,
    const Sources& sources,
    std::span<const RenderTapeIdentityV3Initializer> initializers,
    TapeCoordinatorAdmissionEvidence admission = {}) noexcept {
  const auto* topology = recordTopology(row.recordType);
  if (!topology || topology->topology != RecordReplayTopology::OrderedControl ||
      !row.eventOrdinal || !row.rawOrdinal || !row.buildGeneration ||
      row.dispatchEdge <= context.baselineCompletionEdge ||
      row.dispatchResult < 0 || admission.authority != context.coordinatorDomain ||
      row.releaseAction > static_cast<std::uint32_t>(
          core::metalqueue::SessionReleaseAction::SubmitAndWait) ||
      row.releaseWriter > static_cast<std::uint32_t>(
          core::metalqueue::SessionReleaseWriter::RetainQueryRange) ||
      row.observation > static_cast<std::uint32_t>(Observation::Completed) ||
      row.reserved0 || row.reserved1 ||
      !prefixShapeAdmitted(context, row, sources, initializers)) return false;
  switch (admission.authority) {
  case TapeCoordinatorAuthority::SessionRelease:
    if (!row.releaseOrdinal || !row.releaseGeneration || admission.admissionEdge) return false;
    break;
  case TapeCoordinatorAuthority::OrdinarySerial:
    if (row.releaseOrdinal || row.releaseGeneration || row.releaseAction || row.releaseWriter ||
        admission.admissionEdge <= context.baselineCompletionEdge ||
        admission.admissionEdge >= row.dispatchEdge) return false;
    break;
  default: return false;
  }
  if (row.recordType != D9C_COMMAND_RECORD_QUERY_ISSUE)
    return queryFieldsEmpty(row) && row.prefixKind == static_cast<std::uint32_t>(
        Prefix::None) && row.releaseWriter == static_cast<std::uint32_t>(
        core::metalqueue::SessionReleaseWriter::Publish);
  if (row.queryIdentity.kind != D9C_CHUNK_HANDLE_KIND_QUERY ||
      !row.queryIdentity.objectId || !row.queryIdentity.generation ||
      !row.issueCycle || !row.issueOrdinal || !row.deviceEpoch ||
      !renderTapeQueryDataBytes(row.queryType) ||
      (row.flags != 1u && row.flags != 2u)) return false;
  if (admission.authority == TapeCoordinatorAuthority::SessionRelease &&
      row.releaseWriter == static_cast<std::uint32_t>(
          core::metalqueue::SessionReleaseWriter::RetainQueryRange) &&
      row.queryType != kWireOcclusionQueryType)
    return false;
  return row.flags == 2u
      ? row.prefixKind == static_cast<std::uint32_t>(Prefix::None)
      : row.prefixKind != static_cast<std::uint32_t>(Prefix::None);
}

template <typename Sources>
bool observationAdmitted(
    const TapeProvenanceContext& context,
    const RenderTapeIdentityV3Coordinator& row,
    const RenderTapeIdentityV3CurrentIssue& current,
    const Sources& sources,
    std::span<const RenderTapeIdentityV3Initializer> initializers,
    RenderTapeIdentityV3Observation observation,
    TapeCoordinatorAdmissionEvidence admission = {}) noexcept {
  if (!tapeProvenanceContextAdmitted(context) ||
      !currentIssueAdmitted(context, row, current) ||
      !coordinatorShapeAdmitted(context, row, sources, initializers, admission)) return false;
  const auto kind = static_cast<Prefix>(row.prefixKind);
  if (observation == Observation::Pending) return kind != Prefix::None;
  if (observation != Observation::Ready && observation != Observation::Completed)
    return false;
  if (row.prefixInitializer > context.baselineCompletedInitializer &&
      renderTapeIdentityV3InitializerPhase(initializers[row.initializerIndex]) != Phase::Completed)
    return false;
  if (kind == Prefix::EmptyBaseline || kind == Prefix::ArmBaseline) return true;
  if (kind != Prefix::RealSource || row.sourceIndex >= sources.size()) return false;
  const auto& source = sources[row.sourceIndex];
  return renderTapeIdentityV3SourcePhase(source) == Phase::Completed;
}

} // namespace

bool tapeProvenanceCommandShapeAdmitted(const TapePhysicalCommand& row,
    std::span<const RenderTapeIdentityV3Source> sources,
    std::span<const RenderTapeIdentityV3Coordinator> coordinators) noexcept {
  return commandShapeAdmitted(row, sources, coordinators);
}

bool tapeProvenanceCoordinatorShapeAdmitted(const TapeProvenanceContext& context,
    const RenderTapeIdentityV3Coordinator& row,
    std::span<const RenderTapeIdentityV3Source> sources,
    std::span<const RenderTapeIdentityV3Initializer> initializers,
    TapeCoordinatorAdmissionEvidence admission) noexcept {
  return coordinatorShapeAdmitted(context, row, sources, initializers, admission);
}

bool tapeProvenanceObservationAdmitted(const TapeProvenanceContext& context,
    const RenderTapeIdentityV3Coordinator& row,
    const RenderTapeIdentityV3CurrentIssue& current,
    std::span<const RenderTapeIdentityV3Source> sources,
    std::span<const RenderTapeIdentityV3Initializer> initializers,
    RenderTapeIdentityV3Observation observation,
    TapeCoordinatorAdmissionEvidence admission) noexcept {
  return observationAdmitted(context, row, current, sources, initializers, observation, admission);
}

Status buildTapeCanonicalFacts(std::span<const std::byte> tape,
    const RenderTapeBlobCatalogue& catalogue,
    std::uint64_t presentOrdinal, std::uint32_t maxRows,
    std::vector<RenderTapeIdentityV3EventFact>& events,
    std::vector<RenderTapeIdentityV3RecordFact>& records,
    TapeCoordinatorFailure* failure) noexcept {
  if (failure) *failure = {};
  try {
    std::vector<RenderTapeIdentityV3EventFact> eventFacts;
    std::vector<RenderTapeIdentityV3RecordFact> recordFacts;
    ImportedRenderTapeView imported;
    if (!validateRenderTape(tape, catalogue, &imported).valid())
      return Status::InvalidTape;
    struct QueryDescriptor {
      D9CWireObjectIdentity identity{};
      RenderTapeQueryCycleState cycle{};
    };
    std::vector<QueryDescriptor> queries;
    for (std::size_t i = 0u; i < imported.events.size(); ++i) {
      if (eventFacts.size() >= maxRows) return Status::CapacityExceeded;
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
          if (queries.size() >= maxRows) return Status::CapacityExceeded;
          queries.push_back({object.identity, renderTapeInitialQueryCycle(descriptor)});
        }
      } else if (type == RenderTapeEventType::PresentComplete) {
        RenderTapePresentCompleteHeader present{};
        if (!load(event.payload, 0u, present) ||
            present.presentOrdinal != presentOrdinal) return Status::InvalidHeader;
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
            if (!control) return rejectTapeCoordinator(failure,
                TapeCoordinatorFailureReason::CanonicalControl, kRenderTapeIdentityV3NoIndex, fact);
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
            if (query == queries.end() || !renderTapeAdvanceQueryCycle(query->cycle, issue.flags, true)) {
              fact.queryFlags = issue.flags;
              if (query != queries.end()) {
                fact.queryType = query->cycle.type;
                fact.issueCycle = query->cycle.issueCycle;
                fact.issueOrdinal = query->cycle.issueOrdinal;
              }
              return rejectTapeCoordinator(failure,
                  TapeCoordinatorFailureReason::QueryCycle, kRenderTapeIdentityV3NoIndex, fact);
            }
            fact.queryType = query->cycle.type;
            fact.queryFlags = issue.flags;
            fact.issueCycle = query->cycle.issueCycle;
            fact.issueOrdinal = query->cycle.issueOrdinal;
          }
          if (recordFacts.size() >= maxRows) return Status::CapacityExceeded;
          recordFacts.push_back(fact);
        }
      }
      eventFacts.push_back(eventFact);
    }
    events.swap(eventFacts);
    records.swap(recordFacts);
    return Status::Valid;
  } catch (...) {
    return Status::AllocationFailed;
  }
}

Status validateTapeProvenanceRows(
    const TapeProvenanceContext& context, const TapeProvenanceRows& identity,
    std::span<const RenderTapeIdentityV3EventFact> events,
    std::span<const RenderTapeIdentityV3RecordFact> records,
    std::uint32_t maxRows, bool requireComplete, TapeCoordinatorFailure* failure) noexcept {
  if (failure) *failure = {};
  if (!tapeProvenanceContextAdmitted(context)) return Status::InvalidHeader;
  if (identity.sources.size() > maxRows ||
      identity.initializers.size() > maxRows ||
      identity.commands.size() > maxRows ||
      identity.dispositions.size() > maxRows ||
      identity.coordinators.size() > maxRows ||
      identity.settlements.size() > maxRows ||
      identity.transactionEdges.size() > std::size_t{maxRows} * 3u ||
      events.size() > maxRows ||
      records.size() > maxRows) return Status::CapacityExceeded;
  if (!identity.sources.valid()) return Status::InvalidSource;
  if (!identity.commands.valid()) return Status::InvalidDisposition;
  if (!identity.coordinators.valid()) return rejectTapeCoordinator(
      failure, TapeCoordinatorFailureReason::CoordinatorTable);
  if (!identity.settlements.valid()) return Status::InvalidSettlement;
  try {
    std::vector<Position> cuts;
    std::vector<std::uint64_t> edges{context.baselineCompletionEdge};
    edges.insert(edges.end(), identity.transactionEdges.begin(), identity.transactionEdges.end());
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
    std::uint64_t priorSequence = context.baselineCompletedSequence;
    Position priorCut{};
    for (std::size_t sourceIndex = 0u; sourceIndex < identity.sources.size(); ++sourceIndex) {
      const auto source = identity.sources[sourceIndex];
      const auto phase = renderTapeIdentityV3SourcePhase(source);
      if (phase == Phase::Invalid || !identity.sources.versionFieldsAdmitted(sourceIndex) ||
          source.sourceOrdinal <= priorSource ||
          source.seqId <= priorSequence ||
          source.publicationEdge <= context.baselineCompletionEdge)
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
    std::uint64_t priorInitializer = context.baselineCompletedInitializer;
    std::uint64_t priorInitializerEdge = context.baselineCompletionEdge;
    Position priorInitializerCut{};
    for (const auto& initializer : identity.initializers) {
      const auto phase = renderTapeIdentityV3InitializerPhase(initializer);
      Position cut{};
      if (phase == Phase::Invalid || initializer.epoch != context.initializerEpoch ||
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
          command.drawCount > maxRows || !identity.commands.versionFieldsAdmitted(index) ||
          (haveCommand && command.sourceIndex < priorCommandSource) ||
          command.physicalCommandIndex != commandTails[command.sourceIndex])
        return Status::InvalidDisposition;
      ++commandTails[command.sourceIndex];
      haveCommand = true;
      priorCommandSource = command.sourceIndex;
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
            return rejectTapeCoordinator(failure, TapeCoordinatorFailureReason::DispositionMembership,
                row.coordinatorIndex, {.eventOrdinal = event.ordinal, .recordIndex = row.firstRecord});
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
            const auto admission = identity.coordinators.admission(row.coordinatorIndex);
            const auto expectedAction = fact.recordType == D9C_COMMAND_RECORD_READBACK
                ? core::metalqueue::SessionReleaseAction::SubmitAndWait
                : fact.recordType == D9C_COMMAND_RECORD_UPDATE_TEXTURE
                    ? core::metalqueue::SessionReleaseAction::SubmitSession
                    : core::metalqueue::SessionReleaseAction::ClosePass;
            if (!coordinatorShapeAdmitted(context, control, identity.sources, identity.initializers, admission) ||
                control.eventOrdinal != event.ordinal || control.recordIndex != index ||
                control.recordType != fact.recordType ||
                fact.requiredReleaseAction != static_cast<std::uint32_t>(expectedAction) ||
                (admission.authority == TapeCoordinatorAuthority::SessionRelease &&
                 control.releaseAction != fact.requiredReleaseAction))
              return rejectTapeCoordinator(failure, TapeCoordinatorFailureReason::ShapeOrRecord,
                  row.coordinatorIndex, fact);
            if (fact.recordType == D9C_COMMAND_RECORD_QUERY_ISSUE &&
                (control.flags != fact.queryFlags || control.queryType != fact.queryType ||
                 !renderTapeIdentityV3SameIdentity(control.queryIdentity, fact.queryIdentity) ||
                 control.issueCycle != fact.issueCycle || control.issueOrdinal != fact.issueOrdinal))
              return rejectTapeCoordinator(failure, TapeCoordinatorFailureReason::QueryIdentity,
                  row.coordinatorIndex, fact);
            if (control.prefixInitializer > context.baselineCompletedInitializer) {
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
          !identity.settlements.versionFieldsAdmitted(settlementCursor) ||
          settlement.replayEdge <= context.baselineCompletionEdge ||
          settlement.recordCount != event.recordCount)
        return Status::InvalidSettlement;
      for (const auto& control : identity.coordinators)
        if (control.eventOrdinal == event.ordinal &&
            (control.rawOrdinal != settlement.rawOrdinal ||
             !identity.settlements.coordinatorAdmitted(settlementCursor, control) ||
             control.dispatchEdge >= settlement.replayEdge)) return Status::InvalidSettlement;
      edges.push_back(settlement.replayEdge);
      priorRaw = settlement.rawOrdinal;
      ++settlementCursor;
    }
    if (recordCursor != records.size() || dispositionCursor != identity.dispositions.size())
      return Status::RecordCoverageMismatch;
    if (settlementCursor != identity.settlements.size()) return Status::InvalidSettlement;
    const auto unused = std::find(coordinatorUsed.begin(), coordinatorUsed.end(), false);
    if (unused != coordinatorUsed.end())
      return rejectTapeCoordinator(failure, TapeCoordinatorFailureReason::UnusedCoordinator,
          static_cast<std::uint32_t>(unused - coordinatorUsed.begin()));
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
      const auto admission = identity.coordinators.admission(controlIndex);
      if (admission.admissionEdge) edges.push_back(admission.admissionEdge);
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
            {context.captureToken, context.captureEpoch},
            control.queryIdentity, control.issueCycle, control.issueOrdinal, control.deviceEpoch, false};
        if (!observationAdmitted(context, control, current,
                identity.sources, identity.initializers,
                static_cast<Observation>(control.observation), admission)) return Status::InvalidPrefix;
      }
    }
    std::sort(edges.begin(), edges.end());
    if (std::adjacent_find(edges.begin(), edges.end()) != edges.end()) return Status::InvalidSource;
    return Status::Valid;
  } catch (...) {
    return Status::AllocationFailed;
  }
}

} // namespace dxmt9::d3d9
