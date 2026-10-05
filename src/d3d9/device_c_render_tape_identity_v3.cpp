#include "device_c_render_tape_identity_v3.hpp"

#include "device_c_render_tape_capture.hpp"
#include "device_c_render_tape_provenance_validation.hpp"

#include <cstring>
#include <utility>

namespace dxmt9::d3d9 {
namespace {

using Prefix = RenderTapeIdentityV3PrefixKind;
using Phase = RenderTapeIdentityV3SourcePhase;
using Kind = RenderTapeIdentityV3DispositionKind;
using Status = RenderTapeIdentityV3Status;

template <typename T>
bool load(std::span<const std::byte> bytes, std::size_t offset,
          T& value) noexcept {
  if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) return false;
  std::memcpy(&value, bytes.data() + offset, sizeof(T));
  return true;
}

TapeProvenanceContext provenanceContext(const RenderTapeIdentityV3Header& header) noexcept {
  return {header.captureToken, header.captureEpoch, header.queue,
      header.baselineSequence, header.baselineInitializer,
      header.baselineCompletedSequence, header.initializerEpoch,
      header.baselineCompletedInitializer, header.baselineCompletionEdge,
      header.baselineKind};
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

bool runtimeContextAdmitted(const RenderTapeIdentityV3Header& header) noexcept {
  return header.magic == kRenderTapeIdentityMagic &&
      header.version == kRenderTapeIdentityV3Version &&
      header.headerSize == sizeof(header) && header.frameId &&
      tapeProvenanceContextAdmitted(provenanceContext(header)) &&
      header.authority == static_cast<std::uint32_t>(
          RenderTapeIdentityAuthority::Capture) &&
      !header.reserved0 && !header.reserved1 && !header.reserved2 && !header.reserved3;
}

bool headerAdmitted(const RenderTapeIdentityV3Header& header) noexcept {
  return runtimeContextAdmitted(header) && header.presentOrdinal;
}

bool noGpuMembership(const RenderTapeIdentityV3Disposition& row) noexcept {
  return row.commandIndex == kRenderTapeIdentityV3NoIndex &&
      !row.firstSubdraw && !row.subdrawCount;
}

TapePhysicalCommand physicalCommand(const RenderTapeIdentityV3Command& row) noexcept {
  return {row.sourceIndex, row.physicalCommandIndex, row.kind,
      row.drawCount, row.coordinatorIndex};
}

bool versionCommandShapeAdmitted(const RenderTapeIdentityV3Command& row) noexcept {
  if (row.reserved0 || row.reserved1) return false;
  if (row.membership == static_cast<std::uint32_t>(RenderTapeIdentityV3Membership::Pass))
    return row.logicalPassId && row.dagPassIndex != kRenderTapeIdentityV3NoIndex && row.passKind <= 4u;
  return row.kind != static_cast<std::uint32_t>(RenderTapeIdentityV3CommandKind::DrawRun) &&
      row.membership == static_cast<std::uint32_t>(
          RenderTapeIdentityV3Membership::NoPassCommand) && !row.logicalPassId &&
      row.dagPassIndex == kRenderTapeIdentityV3NoIndex && !row.passKind;
}

TapePhysicalCommand physicalCommandAt(const void* rows, std::size_t index) noexcept {
  return physicalCommand(static_cast<const RenderTapeIdentityV3Command*>(rows)[index]);
}

bool versionCommandFieldsAdmitted(const void* rows, std::size_t index) noexcept {
  const auto* commands = static_cast<const RenderTapeIdentityV3Command*>(rows);
  const auto& command = commands[index];
  if (!versionCommandShapeAdmitted(command)) return false;
  for (std::size_t prior = 0u; prior < index; ++prior) {
    const auto& priorCommand = commands[prior];
    if (command.logicalPassId && priorCommand.logicalPassId == command.logicalPassId &&
        (priorCommand.dagPassIndex != command.dagPassIndex ||
         priorCommand.passKind != command.passKind)) return false;
  }
  return true;
}

bool commandShapeAdmitted(const RenderTapeIdentityV3Command& row,
    std::span<const RenderTapeIdentityV3Source> sources,
    std::span<const RenderTapeIdentityV3Coordinator> coordinators) noexcept {
  return tapeProvenanceCommandShapeAdmitted(physicalCommand(row), sources, coordinators) &&
      versionCommandShapeAdmitted(row);
}

bool coordinatorShapeAdmitted(const RenderTapeIdentityV3Header& header,
    const RenderTapeIdentityV3Coordinator& row,
    std::span<const RenderTapeIdentityV3Source> sources,
    std::span<const RenderTapeIdentityV3Initializer> initializers) noexcept {
  return tapeProvenanceCoordinatorShapeAdmitted(
      provenanceContext(header), row, sources, initializers);
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

bool observationAdmitted(
    const RenderTapeIdentityV3Header& header,
    const RenderTapeIdentityV3Coordinator& row,
    const RenderTapeIdentityV3CurrentIssue& current,
    std::span<const RenderTapeIdentityV3Source> sources,
    std::span<const RenderTapeIdentityV3Initializer> initializers,
    RenderTapeIdentityV3Observation observation) noexcept {
  return tapeProvenanceObservationAdmitted(
      provenanceContext(header), row, current, sources, initializers, observation);
}

} // namespace

bool renderTapeIdentityV3ObservationAdmitted(
    const RenderTapeIdentityV3Header& header,
    const RenderTapeIdentityV3Coordinator& row,
    const RenderTapeIdentityV3CurrentIssue& current,
    std::span<const RenderTapeIdentityV3Source> sources,
    std::span<const RenderTapeIdentityV3Initializer> initializers,
    RenderTapeIdentityV3Observation observation) noexcept {
  return headerAdmitted(header) &&
      observationAdmitted(header, row, current, sources, initializers, observation);
}

bool renderTapeIdentityV3RuntimeObservationAdmitted(
    const RenderTapeIdentityV3Header& context,
    const RenderTapeIdentityV3Coordinator& row,
    const RenderTapeIdentityV3CurrentIssue& current,
    std::span<const RenderTapeIdentityV3Source> sources,
    std::span<const RenderTapeIdentityV3Initializer> initializers,
    RenderTapeIdentityV3Observation observation) noexcept {
  return runtimeContextAdmitted(context) &&
      observationAdmitted(context, row, current, sources, initializers, observation);
}

Status validateRenderTapeIdentityV3Rows(
    const RenderTapeIdentityV3View& identity,
    std::span<const RenderTapeIdentityV3EventFact> events,
    std::span<const RenderTapeIdentityV3RecordFact> records,
    bool requireComplete) noexcept {
  if (!headerAdmitted(identity.header)) return Status::InvalidHeader;
  const TapeProvenanceRows rows{
      identity.sources, identity.initializers,
      {identity.commands.data(), identity.commands.size(),
       physicalCommandAt, versionCommandFieldsAdmitted},
      identity.dispositions, identity.coordinators, identity.settlements};
  return provenanceStatus(validateTapeProvenanceRows(
      provenanceContext(identity.header), rows, events, records,
      kRenderTapeIdentityV3MaxRows, requireComplete));
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
    auto status = provenanceStatus(buildTapeCanonicalFacts(
        tape, catalogue, header.presentOrdinal, kRenderTapeIdentityV3MaxRows, events, records));
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
      !renderTapeIdentityV3LimitsAdmitted(limits) ||
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
