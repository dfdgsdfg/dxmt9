#include "device_c_render_tape_identity_v4.hpp"

#include "device_c_render_tape_capture.hpp"
#include "device_c_render_tape_provenance_validation.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

namespace dxmt9::d3d9 {
namespace {

using Status = RenderTapeIdentityV4Status;

template <typename T>
bool load(std::span<const std::byte> bytes, std::size_t offset, T& value) noexcept {
  if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) return false;
  std::memcpy(&value, bytes.data() + offset, sizeof(T));
  return true;
}

TapeProvenanceContext provenanceContext(const RenderTapeIdentityV4Header& header) noexcept {
  return {header.captureToken, header.captureEpoch, header.queue,
      header.baselineSequence, header.baselineInitializer,
      header.baselineCompletedSequence, header.initializerEpoch,
      header.baselineCompletedInitializer, header.baselineCompletionEdge,
      header.baselineKind, static_cast<TapeCoordinatorAuthority>(header.coordinatorDomain)};
}

Status provenanceStatus(TapeProvenanceStatus status) noexcept {
  switch (status) {
  case TapeProvenanceStatus::Valid: return Status::Valid;
  case TapeProvenanceStatus::InvalidTape: return Status::InvalidTape;
  case TapeProvenanceStatus::InvalidHeader: return Status::InvalidHeader;
  case TapeProvenanceStatus::InvalidSource: return Status::InvalidSource;
  case TapeProvenanceStatus::InvalidCut: return Status::InvalidCut;
  case TapeProvenanceStatus::RecordCoverageMismatch: return Status::RecordCoverageMismatch;
  case TapeProvenanceStatus::InvalidDisposition: return Status::InvalidDisposition;
  case TapeProvenanceStatus::InvalidCoordinator: return Status::InvalidCoordinator;
  case TapeProvenanceStatus::InvalidPrefix: return Status::InvalidPrefix;
  case TapeProvenanceStatus::InvalidSettlement: return Status::InvalidSettlement;
  case TapeProvenanceStatus::Incomplete: return Status::Incomplete;
  case TapeProvenanceStatus::CapacityExceeded: return Status::CapacityExceeded;
  case TapeProvenanceStatus::AllocationFailed: return Status::AllocationFailed;
  }
  return Status::InvalidHeader;
}

bool headerAdmitted(const RenderTapeIdentityV4Header& h) noexcept {
  return h.magic == kRenderTapeIdentityMagic && h.version == kRenderTapeIdentityV4Version &&
      h.headerSize == sizeof(h) && h.frameId && h.presentOrdinal &&
      tapeProvenanceContextAdmitted(provenanceContext(h)) &&
      h.authority == static_cast<std::uint32_t>(RenderTapeIdentityAuthority::Capture) &&
      h.requiredExecutionTopologyVersion == 1u && !h.reserved0 && !h.reserved1 &&
      !h.reserved2 && !h.reserved3 && !h.reserved4;
}

const RenderTapeIdentityV3Coordinator& coordinatorAt(const void* rows, std::size_t index) noexcept {
  return static_cast<const RenderTapeIdentityV4Coordinator*>(rows)[index].provenance;
}

TapeCoordinatorAdmissionEvidence coordinatorAdmissionAt(const void* rows, std::size_t index) noexcept {
  const auto& row = static_cast<const RenderTapeIdentityV4Coordinator*>(rows)[index];
  return {row.reserved0 ? static_cast<TapeCoordinatorAuthority>(0u)
                       : static_cast<TapeCoordinatorAuthority>(row.authority), row.admissionEdge};
}

RenderTapeIdentityV3Source sourceAt(const void* rows, std::size_t index) noexcept {
  return static_cast<const RenderTapeIdentityV4Source*>(rows)[index].lifecycle;
}

bool sourceVersionFieldsAdmitted(const void* rows, std::size_t index) noexcept {
  return !static_cast<const RenderTapeIdentityV4Source*>(rows)[index].reserved0;
}

TapePhysicalCommand physicalCommandAt(const void* rows, std::size_t index) noexcept {
  const auto& command = static_cast<const RenderTapeIdentityV4PhysicalCommand*>(rows)[index];
  return {command.sourceIndex, command.physicalCommandIndex, command.kind,
      command.drawCount, command.coordinatorIndex};
}

bool versionCommandFieldsAdmitted(const void* rows, std::size_t index) noexcept {
  return !static_cast<const RenderTapeIdentityV4PhysicalCommand*>(rows)[index].reserved0;
}

bool sameReplayIdentity(const RenderTapeIdentityV4ReplaySourceIdentity& left,
    const RenderTapeIdentityV4ReplaySourceIdentity& right) noexcept {
  return left.frame == right.frame && left.source == right.source &&
      left.lastSource == right.lastSource && left.sequence == right.sequence;
}

bool emptyReceipt(const RenderTapeIdentityV4ReplayReceipt& receipt) noexcept {
  return sameReplayIdentity(receipt.identity, {}) && !receipt.queueSequence &&
      !receipt.buildGeneration && !receipt.sourceGeneration && !receipt.storageGeneration &&
      receipt.kind == static_cast<std::uint32_t>(RenderTapeIdentityV4ReceiptKind::None) &&
      receipt.controlIndex == kRenderTapeIdentityV4NoIndex && !receipt.commandCount && !receipt.reserved0;
}

bool committedReceiptAdmitted(const RenderTapeIdentityV4View& identity,
    const RenderTapeIdentityV4ReplayAttempt& attempt) noexcept {
  const auto& receipt = attempt.receipt;
  if (receipt.reserved0 || !sameReplayIdentity(attempt.identity, receipt.identity) ||
      !receipt.queueSequence || receipt.commandCount != attempt.stagedCommandCount)
    return false;
  const auto kind = static_cast<RenderTapeIdentityV4ReceiptKind>(receipt.kind);
  if (kind == RenderTapeIdentityV4ReceiptKind::Compatibility)
    return receipt.queueSequence == attempt.identity.sequence && !receipt.buildGeneration &&
        !receipt.sourceGeneration && !receipt.storageGeneration &&
        receipt.controlIndex == kRenderTapeIdentityV4NoIndex;
  if ((kind != RenderTapeIdentityV4ReceiptKind::DirectChunkSlot &&
       kind != RenderTapeIdentityV4ReceiptKind::Arena) || !receipt.buildGeneration ||
      !receipt.sourceGeneration || !receipt.storageGeneration ||
      receipt.controlIndex == kRenderTapeIdentityV4NoIndex) return false;
  // Arena retains its real first-ticket batch anchor. Direct spans may commit
  // into Writing and share a later publication with different ticket build
  // generations/raws; their ephemeral receipt fields are actual attempt
  // evidence, not aliases of that eventual publication's metadata.
  const auto anchor = std::find_if(identity.sources.begin(), identity.sources.end(), [&](const auto& source) {
    const auto& lifetime = source.lifecycle;
    return lifetime.seqId == receipt.queueSequence &&
        lifetime.writerGeneration == receipt.sourceGeneration &&
        lifetime.storageGeneration == receipt.storageGeneration &&
        source.publicationControlIndex == receipt.controlIndex &&
        lifetime.publicationEdge > attempt.beginEdge &&
        (kind == RenderTapeIdentityV4ReceiptKind::DirectChunkSlot ||
         (source.rawOrdinal == attempt.rawOrdinal &&
          source.publicationBuildGeneration == receipt.buildGeneration &&
          lifetime.publicationEdge < attempt.terminalEdge));
  });
  if (anchor == identity.sources.end()) return false;
  const auto anchorIndex = static_cast<std::size_t>(anchor - identity.sources.begin());
  const auto end = std::uint64_t{attempt.firstRecord} + attempt.recordCount;
  bool anchorContributes = false;
  for (const auto& row : identity.dispositions) {
    if (row.eventOrdinal != attempt.eventOrdinal ||
        row.kind != static_cast<std::uint32_t>(RenderTapeIdentityV3DispositionKind::GpuCommand) ||
        row.firstRecord >= end || std::uint64_t{row.firstRecord} + row.recordCount <= attempt.firstRecord)
      continue;
    if (row.commandIndex >= identity.commands.size()) return false;
    const auto sourceIndex = identity.commands[row.commandIndex].sourceIndex;
    if (kind == RenderTapeIdentityV4ReceiptKind::DirectChunkSlot && sourceIndex != anchorIndex)
      return false;
    anchorContributes |= sourceIndex == anchorIndex;
  }
  for (const auto& command : identity.commands) {
    if (command.coordinatorIndex == kRenderTapeIdentityV4NoIndex) continue;
    if (command.coordinatorIndex >= identity.coordinators.size()) return false;
    const auto& control = identity.coordinators[command.coordinatorIndex].provenance;
    if (control.eventOrdinal != attempt.eventOrdinal || control.recordIndex < attempt.firstRecord ||
        control.recordIndex >= end) continue;
    if (kind == RenderTapeIdentityV4ReceiptKind::DirectChunkSlot && command.sourceIndex != anchorIndex)
      return false;
    anchorContributes |= command.sourceIndex == anchorIndex;
  }
  return anchorContributes;
}

bool coordinatorAttemptAdmitted(const RenderTapeIdentityV4View& identity,
    const RenderTapeIdentityV3Coordinator& control,
    TapeCoordinatorAdmissionEvidence admission) noexcept {
  return std::any_of(identity.attempts.begin(), identity.attempts.end(), [&](const auto& attempt) {
    return attempt.outcome == static_cast<std::uint32_t>(RenderTapeIdentityV4AttemptOutcome::Committed) &&
        attempt.eventOrdinal == control.eventOrdinal && attempt.rawOrdinal == control.rawOrdinal &&
        attempt.attemptGeneration == control.buildGeneration &&
        control.recordIndex >= attempt.firstRecord &&
        std::uint64_t{control.recordIndex} < std::uint64_t{attempt.firstRecord} + attempt.recordCount &&
        attempt.beginEdge < control.dispatchEdge && control.dispatchEdge < attempt.terminalEdge &&
        attempt.effectEdge && attempt.effectEdge <= control.dispatchEdge &&
        (admission.authority == TapeCoordinatorAuthority::SessionRelease
            ? !admission.admissionEdge
            : admission.authority == TapeCoordinatorAuthority::OrdinarySerial &&
              attempt.beginEdge < admission.admissionEdge &&
              attempt.effectEdge < admission.admissionEdge &&
              admission.admissionEdge < control.dispatchEdge);
  });
}

TapeRawSettlement rawSettlementAt(const void* rows, std::size_t index) noexcept {
  const auto& row = static_cast<const RenderTapeIdentityV4View*>(rows)->settlements[index];
  return {row.eventOrdinal, row.rawOrdinal, row.replayEdge, row.recordCount};
}

bool rawSettlementVersionFieldsAdmitted(const void* rows, std::size_t index) noexcept {
  return !static_cast<const RenderTapeIdentityV4View*>(rows)->settlements[index].reserved0;
}

bool rawSettlementCoordinatorAdmitted(const void* rows, std::size_t,
    const RenderTapeIdentityV3Coordinator& control) noexcept {
  const auto& identity = *static_cast<const RenderTapeIdentityV4View*>(rows);
  for (std::size_t index = 0u; index < identity.coordinators.size(); ++index)
    if (&identity.coordinators[index].provenance == &control)
      return coordinatorAttemptAdmitted(identity, control,
          coordinatorAdmissionAt(identity.coordinators.data(), index));
  return false;
}

Status validateReplayAttempts(const RenderTapeIdentityV4View& identity,
    std::span<const RenderTapeIdentityV3EventFact> events,
    std::vector<std::uint64_t>& edges, bool requireComplete) {
  if (identity.attempts.size() > kRenderTapeIdentityV4MaxRows) return Status::CapacityExceeded;
  edges.reserve(identity.attempts.size() * 3u);
  std::uint64_t priorGeneration = 0u, priorTerminal = identity.header.baselineCompletionEdge;
  std::uint64_t eventOrdinal = 0u, rawOrdinal = 0u;
  std::uint32_t committedEnd = 0u, recordCount = 0u;
  RenderTapeIdentityV4ReplaySourceIdentity rawIdentity{};
  bool haveCommitted = false;
  const auto closeRaw = [&]() -> Status {
    if (!eventOrdinal) return Status::Valid;
    const auto settlement = std::find_if(identity.settlements.begin(), identity.settlements.end(),
        [&](const auto& row) { return row.eventOrdinal == eventOrdinal; });
    if (settlement == identity.settlements.end())
      return requireComplete ? Status::Incomplete : Status::Valid;
    if (settlement->rawOrdinal != rawOrdinal || !haveCommitted || committedEnd != recordCount ||
        settlement->replayEdge <= priorTerminal) return Status::InvalidTransaction;
    return Status::Valid;
  };
  for (const auto& attempt : identity.attempts) {
    const auto event = std::find_if(events.begin(), events.end(),
        [&](const auto& value) { return value.ordinal == attempt.eventOrdinal; });
    const auto end = std::uint64_t{attempt.firstRecord} + attempt.recordCount;
    if (event == events.end() || event->type != static_cast<std::uint32_t>(RenderTapeEventType::CommandChunk) ||
        !attempt.rawOrdinal || attempt.attemptGeneration <= priorGeneration || attempt.attemptGeneration == UINT64_MAX ||
        attempt.beginEdge <= priorTerminal || attempt.terminalEdge <= attempt.beginEdge ||
        (attempt.effectEdge && (attempt.effectEdge <= attempt.beginEdge || attempt.effectEdge >= attempt.terminalEdge)) ||
        !attempt.identity.source || attempt.identity.lastSource < attempt.identity.source ||
        attempt.identity.sequence != attempt.rawOrdinal || end > event->recordCount ||
        (attempt.stagedCommandCount && !attempt.recordCount)) return Status::InvalidTransaction;
    if (attempt.eventOrdinal != eventOrdinal) {
      const auto status = closeRaw();
      if (status != Status::Valid) return status;
      if (attempt.eventOrdinal <= eventOrdinal || attempt.rawOrdinal <= rawOrdinal)
        return Status::InvalidTransaction;
      eventOrdinal = attempt.eventOrdinal;
      rawOrdinal = attempt.rawOrdinal;
      rawIdentity = attempt.identity;
      recordCount = event->recordCount;
      committedEnd = 0u;
      haveCommitted = false;
    } else if (attempt.rawOrdinal != rawOrdinal || !sameReplayIdentity(attempt.identity, rawIdentity))
      return Status::InvalidTransaction;
    const auto outcome = static_cast<RenderTapeIdentityV4AttemptOutcome>(attempt.outcome);
    if (outcome == RenderTapeIdentityV4AttemptOutcome::FailedStop)
      return Status::InvalidTransaction;
    if (outcome == RenderTapeIdentityV4AttemptOutcome::Committed) {
      if (attempt.firstRecord != committedEnd) return Status::InvalidTransaction;
      if (!committedReceiptAdmitted(identity, attempt)) return Status::InvalidReceipt;
      committedEnd = static_cast<std::uint32_t>(end);
      haveCommitted = true;
    } else if (outcome == RenderTapeIdentityV4AttemptOutcome::RolledBack) {
      // Local rollback preserves a committed prefix. Retrying the remaining
      // suffix is distinct from rerunning that prefix or the entire raw.
      if (attempt.firstRecord < committedEnd || attempt.effectEdge) return Status::InvalidTransaction;
      if (!emptyReceipt(attempt.receipt)) return Status::InvalidReceipt;
    } else return Status::InvalidTransaction;
    edges.push_back(attempt.beginEdge);
    if (attempt.effectEdge) edges.push_back(attempt.effectEdge);
    edges.push_back(attempt.terminalEdge);
    priorGeneration = attempt.attemptGeneration;
    priorTerminal = attempt.terminalEdge;
  }
  const auto status = closeRaw();
  if (status != Status::Valid) return status;
  for (const auto& settlement : identity.settlements)
    if (std::none_of(identity.attempts.begin(), identity.attempts.end(), [&](const auto& attempt) {
          return attempt.eventOrdinal == settlement.eventOrdinal && attempt.rawOrdinal == settlement.rawOrdinal;
        })) return Status::InvalidTransaction;
  for (const auto& event : events)
    if (event.type == static_cast<std::uint32_t>(RenderTapeEventType::CommandChunk) &&
        std::none_of(identity.attempts.begin(), identity.attempts.end(), [&](const auto& attempt) {
          return attempt.eventOrdinal == event.ordinal;
        })) return Status::InvalidTransaction;
  for (std::size_t index = 0u; index < identity.coordinators.size(); ++index)
    if (!coordinatorAttemptAdmitted(identity, identity.coordinators[index].provenance,
        coordinatorAdmissionAt(identity.coordinators.data(), index))) return Status::InvalidCoordinator;
  return Status::Valid;
}

template <typename T>
bool table(RenderTapeIdentityV3Table& descriptor, std::uint64_t& cursor,
           const std::vector<T>& rows) noexcept {
  if (rows.size() > kRenderTapeIdentityV4MaxRows || cursor > UINT32_MAX) return false;
  descriptor = {static_cast<std::uint32_t>(cursor), static_cast<std::uint32_t>(rows.size()),
                sizeof(T)};
  cursor += rows.size() * std::uint64_t{sizeof(T)};
  return cursor <= UINT32_MAX;
}

template <typename T>
bool importTable(std::span<const std::byte> bytes, RenderTapeIdentityV3Table descriptor,
                 std::uint64_t& cursor, std::vector<T>& rows) {
  if (descriptor.count > kRenderTapeIdentityV4MaxRows || descriptor.offset != cursor ||
      descriptor.entrySize != sizeof(T)) return false;
  cursor += std::uint64_t{descriptor.count} * sizeof(T);
  if (cursor > bytes.size()) return false;
  rows.resize(descriptor.count);
  if (!rows.empty()) std::memcpy(rows.data(), bytes.data() + descriptor.offset,
                                 rows.size() * sizeof(T));
  return true;
}

template <typename T>
void storeTable(std::vector<std::byte>& bytes, RenderTapeIdentityV3Table descriptor,
                const std::vector<T>& rows) noexcept {
  if (!rows.empty()) std::memcpy(bytes.data() + descriptor.offset, rows.data(),
                                rows.size() * sizeof(T));
}

} // namespace

Status validateRenderTapeIdentityV4Rows(const RenderTapeIdentityV4View& identity,
    std::span<const RenderTapeIdentityV3EventFact> events,
    std::span<const RenderTapeIdentityV3RecordFact> records, bool requireComplete) noexcept {
  if (!headerAdmitted(identity.header)) return Status::InvalidHeader;
  for (const auto count : {identity.sources.size(), identity.initializers.size(),
          identity.commands.size(), identity.dispositions.size(), identity.coordinators.size(),
          identity.settlements.size(), identity.attempts.size(), events.size(), records.size()})
    if (count > kRenderTapeIdentityV4MaxRows) return Status::CapacityExceeded;
  try {
    std::vector<std::uint64_t> edges;
    const auto status = validateReplayAttempts(identity, events, edges, requireComplete);
    if (status != Status::Valid) return status;
    const TapeProvenanceRows rows{
        {identity.sources.data(), identity.sources.size(), sourceAt, sourceVersionFieldsAdmitted},
        identity.initializers,
        {identity.commands.data(), identity.commands.size(),
         physicalCommandAt, versionCommandFieldsAdmitted},
        identity.dispositions,
        {identity.coordinators.data(), identity.coordinators.size(), coordinatorAt, coordinatorAdmissionAt},
        {&identity, identity.settlements.size(), rawSettlementAt,
         rawSettlementVersionFieldsAdmitted, rawSettlementCoordinatorAdmitted}, edges};
    return provenanceStatus(validateTapeProvenanceRows(
        provenanceContext(identity.header), rows, events, records,
        kRenderTapeIdentityV4MaxRows, requireComplete));
  } catch (...) { return Status::AllocationFailed; }
}

Status validateRenderTapeIdentityV4(std::span<const std::byte> tape,
    const RenderTapeBlobCatalogue& catalogue, std::span<const std::byte> sidecar,
    RenderTapeIdentityV4View* out, bool requireComplete) noexcept {
  if (out) *out = {};
  RenderTapeIdentityV4View identity;
  if (!load(sidecar, 0u, identity.header) || !headerAdmitted(identity.header)) return Status::InvalidHeader;
  const auto& h = identity.header;
  for (const auto t : {h.sources, h.initializers, h.commands, h.dispositions,
          h.coordinators, h.settlements, h.attempts})
    if (t.count > kRenderTapeIdentityV4MaxRows) return Status::CapacityExceeded;
  if (h.eventsBytes != tape.size() || h.eventsDigest != RenderTapeCaptureSession::sha256(tape))
    return Status::EventsMismatch;
  try {
    std::uint64_t cursor = sizeof(h);
    if (!importTable(sidecar, h.sources, cursor, identity.sources) ||
        !importTable(sidecar, h.initializers, cursor, identity.initializers) ||
        !importTable(sidecar, h.commands, cursor, identity.commands) ||
        !importTable(sidecar, h.dispositions, cursor, identity.dispositions) ||
        !importTable(sidecar, h.coordinators, cursor, identity.coordinators) ||
        !importTable(sidecar, h.settlements, cursor, identity.settlements) ||
        !importTable(sidecar, h.attempts, cursor, identity.attempts) || cursor != sidecar.size())
      return Status::InvalidLayout;
    std::vector<RenderTapeIdentityV3EventFact> events;
    std::vector<RenderTapeIdentityV3RecordFact> records;
    auto status = provenanceStatus(buildTapeCanonicalFacts(
        tape, catalogue, h.presentOrdinal, kRenderTapeIdentityV4MaxRows, events, records));
    if (status == Status::Valid) status = validateRenderTapeIdentityV4Rows(identity, events, records, requireComplete);
    if (status == Status::Valid && out) *out = std::move(identity);
    return status;
  } catch (...) { return Status::AllocationFailed; }
}

std::vector<std::byte> buildRenderTapeIdentityV4(std::span<const std::byte> tape,
    const RenderTapeBlobCatalogue& catalogue, const RenderTapeIdentityV4View& identity, Status* outStatus) {
  auto finish = [&](Status status) { if (outStatus) *outStatus = status; };
  try {
    auto header = identity.header;
    if (!headerAdmitted(header)) { finish(Status::InvalidHeader); return {}; }
    std::uint64_t cursor = sizeof(header);
    if (!table(header.sources, cursor, identity.sources) || !table(header.initializers, cursor, identity.initializers) ||
        !table(header.commands, cursor, identity.commands) || !table(header.dispositions, cursor, identity.dispositions) ||
        !table(header.coordinators, cursor, identity.coordinators) || !table(header.settlements, cursor, identity.settlements) ||
        !table(header.attempts, cursor, identity.attempts)) {
      finish(Status::CapacityExceeded); return {};
    }
    header.eventsBytes = tape.size(); header.eventsDigest = RenderTapeCaptureSession::sha256(tape);
    std::vector<std::byte> bytes(cursor);
    std::memcpy(bytes.data(), &header, sizeof(header));
    storeTable(bytes, header.sources, identity.sources); storeTable(bytes, header.initializers, identity.initializers);
    storeTable(bytes, header.commands, identity.commands); storeTable(bytes, header.dispositions, identity.dispositions);
    storeTable(bytes, header.coordinators, identity.coordinators); storeTable(bytes, header.settlements, identity.settlements);
    storeTable(bytes, header.attempts, identity.attempts);
    const auto status = validateRenderTapeIdentityV4(tape, catalogue, bytes);
    finish(status);
    return status == Status::Valid ? std::move(bytes) : std::vector<std::byte>{};
  } catch (...) { finish(Status::AllocationFailed); return {}; }
}

} // namespace dxmt9::d3d9
