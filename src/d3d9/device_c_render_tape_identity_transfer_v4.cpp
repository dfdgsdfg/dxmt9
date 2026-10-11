#include "device_c_render_tape_identity_transfer_v4.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace dxmt9::d3d9 {
namespace {

using Status = RenderTapeIdentityV4TransferStatus;

bool unboundDigest(const RenderTapeDigest& digest) noexcept {
  return std::all_of(digest.begin(), digest.end(),
      [](std::byte value) { return value == std::byte{}; });
}

bool snapshotHeadersAdmitted(const RenderTapeIdentityV4Header& identity,
                            const RenderTapeExecutionTopologyV1Header& topology) noexcept {
  return identity.magic == kRenderTapeIdentityMagic &&
      identity.version == kRenderTapeIdentityV4Version &&
      identity.headerSize == sizeof(identity) && !identity.eventsBytes &&
      unboundDigest(identity.eventsDigest) &&
      identity.authority == static_cast<std::uint32_t>(RenderTapeIdentityAuthority::Capture) &&
      identity.requiredExecutionTopologyVersion == kRenderTapeExecutionTopologyV1Version &&
      !identity.reserved0 && !identity.reserved1 && !identity.reserved2 && !identity.reserved3 &&
      !identity.reserved4 &&
      (identity.coordinatorDomain == static_cast<std::uint32_t>(TapeCoordinatorAuthority::SessionRelease) ||
       identity.coordinatorDomain == static_cast<std::uint32_t>(TapeCoordinatorAuthority::OrdinarySerial)) &&
      topology.magic == kRenderTapeExecutionTopologyV1Magic &&
      topology.version == kRenderTapeExecutionTopologyV1Version &&
      topology.headerSize == sizeof(topology) &&
      !topology.identityBytes && !topology.eventsBytes &&
      unboundDigest(topology.identityDigest) && unboundDigest(topology.eventsDigest) &&
      !topology.reserved0 && !topology.reserved1;
}

bool sameCapture(const RenderTapeIdentityV4Header& identity,
                 const RenderTapeExecutionTopologyV1Header& topology) noexcept {
  return identity.captureToken && identity.captureEpoch && identity.queue &&
      identity.captureToken == topology.captureToken &&
      identity.captureEpoch == topology.captureEpoch && identity.queue == topology.queue;
}

bool coordinatorDomainsAdmitted(const RenderTapeIdentityV4View& identity) noexcept {
  for (const auto& row : identity.coordinators) {
    const auto& control = row.provenance;
    if (row.reserved0 || row.authority != identity.header.coordinatorDomain) return false;
    if (row.authority == static_cast<std::uint32_t>(TapeCoordinatorAuthority::SessionRelease)) {
      if (row.admissionEdge || !control.releaseOrdinal || !control.releaseGeneration) return false;
    } else if (row.authority == static_cast<std::uint32_t>(TapeCoordinatorAuthority::OrdinarySerial)) {
      if (control.releaseOrdinal || control.releaseGeneration || control.releaseAction || control.releaseWriter ||
          row.admissionEdge <= identity.header.baselineCompletionEdge ||
          row.admissionEdge >= control.dispatchEdge) return false;
    } else return false;
  }
  return true;
}

template <typename T>
bool layoutTable(RenderTapeIdentityV3Table& table,
                 std::uint64_t& cursor, const std::vector<T>& rows) noexcept {
  if (rows.size() > kRenderTapeIdentityV4MaxRows || cursor > UINT32_MAX)
    return false;
  table = {static_cast<std::uint32_t>(cursor),
           static_cast<std::uint32_t>(rows.size()), sizeof(T)};
  cursor += std::uint64_t{sizeof(T)} * rows.size();
  return cursor <= UINT32_MAX;
}

template <typename T>
Status admitTable(RenderTapeIdentityV3Table table, std::uint64_t& cursor,
                  std::size_t bytes) noexcept {
  if (table.count > kRenderTapeIdentityV4MaxRows) return Status::CapacityExceeded;
  if (table.offset != cursor || table.entrySize != sizeof(T))
    return Status::InvalidLayout;
  cursor += std::uint64_t{sizeof(T)} * table.count;
  if (cursor > UINT32_MAX) return Status::CapacityExceeded;
  return cursor <= bytes ? Status::Valid : Status::InvalidLayout;
}

template <typename T>
void storeTable(std::vector<std::byte>& bytes, RenderTapeIdentityV3Table table,
                const std::vector<T>& rows) noexcept {
  if (!rows.empty())
    std::memcpy(bytes.data() + table.offset, rows.data(), rows.size() * sizeof(T));
}

template <typename T>
void copyTable(std::vector<T>& rows, std::span<const std::byte> bytes,
               RenderTapeIdentityV3Table table) {
  rows.resize(table.count);
  if (!rows.empty())
    std::memcpy(rows.data(), bytes.data() + table.offset, rows.size() * sizeof(T));
}

} // namespace

bool renderTapeAttachmentHistoryMatchesCapture(std::span<const std::byte> bytes,
    const RenderTapeIdentityV4Header& identity,
    const RenderTapeActionWitnessV1& witness) noexcept {
  RenderTapeAttachmentHistoryHeader h;
  std::vector<RenderTapeAttachmentHistoryRow> rows;
  return decodeRenderTapeAttachmentHistory(bytes, h, rows) &&
      renderTapeActionWitnessV1MatchesBinding(witness,
          h.token, h.epoch, h.queue) && witness.armProfile == 0u &&
      h.token == identity.captureToken && h.epoch == identity.captureEpoch &&
      h.queue == identity.queue &&
      h.completedSequence == identity.baselineCompletedSequence &&
      h.completedSequence == identity.baselineSequence &&
      h.completedInitializer == identity.baselineCompletedInitializer &&
      h.completedInitializer == identity.baselineInitializer;
}

Status encodeRenderTapeIdentityV4SnapshotTransfer(
    const RenderTapeIdentityV4View& identity,
    const RenderTapeExecutionTopologyV1View& topology,
    const RenderTapeActionWitnessV1& actionWitness,
    std::span<const std::byte> attachmentHistory,
    std::vector<std::byte>& out) noexcept {
  if (attachmentHistory.empty())
    return encodeRenderTapeIdentityV4SnapshotTransfer(identity, topology, actionWitness, out);
  out.clear();
  if (!renderTapeAttachmentHistoryMatchesCapture(attachmentHistory, identity.header, actionWitness))
    return Status::CaptureMismatch;
  std::vector<std::byte> snapshot;
  const auto status = encodeRenderTapeIdentityV4SnapshotTransfer(identity, topology, actionWitness, snapshot);
  if (status != Status::Valid) return status;
  RenderTapeHistoryTransferHeader h;
  h.totalBytes = std::uint64_t{sizeof(h)} + snapshot.size() + attachmentHistory.size();
  if (h.totalBytes > UINT32_MAX) return Status::CapacityExceeded;
  h.snapshotBytes = static_cast<std::uint32_t>(snapshot.size());
  h.historyBytes = static_cast<std::uint32_t>(attachmentHistory.size());
  try {
    std::vector<std::byte> pending(h.totalBytes);
    std::memcpy(pending.data(), &h, sizeof(h));
    std::memcpy(pending.data() + sizeof(h), snapshot.data(), snapshot.size());
    std::memcpy(pending.data() + sizeof(h) + snapshot.size(), attachmentHistory.data(), attachmentHistory.size());
    out = std::move(pending);
    return Status::Valid;
  } catch (...) { return Status::AllocationFailed; }
}

Status encodeRenderTapeIdentityV4SnapshotTransfer(
    const RenderTapeIdentityV4View& identity,
    const RenderTapeExecutionTopologyV1View& topology,
    std::vector<std::byte>& out) noexcept {
  return encodeRenderTapeIdentityV4SnapshotTransfer(identity, topology,
      RenderTapeActionWitnessV1{}, out);
}

Status encodeRenderTapeIdentityV4SnapshotTransfer(
    const RenderTapeIdentityV4View& identity,
    const RenderTapeExecutionTopologyV1View& topology,
    const RenderTapeActionWitnessV1& actionWitness,
    std::vector<std::byte>& out) noexcept {
  out.clear();
  if (!snapshotHeadersAdmitted(identity.header, topology.header) ||
      !renderTapeActionWitnessV1Valid(actionWitness)) return Status::InvalidHeader;
  if (!sameCapture(identity.header, topology.header)) return Status::CaptureMismatch;
  if (actionWitness.evidence == static_cast<std::uint32_t>(
          RenderTapeActionWitnessEvidenceV1::QueueObserved) &&
      !renderTapeActionWitnessV1MatchesBinding(actionWitness, identity.header.captureToken,
          identity.header.captureEpoch, identity.header.queue)) return Status::CaptureMismatch;
  if (!coordinatorDomainsAdmitted(identity)) return Status::InvalidLayout;
  RenderTapeIdentityV4TransferHeader transfer{};
  transfer.identity = identity.header;
  transfer.topology = topology.header;
  transfer.actionWitness = actionWitness;
  auto& i = transfer.identity;
  auto& t = transfer.topology;
  std::uint64_t size = sizeof(transfer);
  if (!layoutTable(i.sources, size, identity.sources) ||
      !layoutTable(i.initializers, size, identity.initializers) ||
      !layoutTable(i.commands, size, identity.commands) ||
      !layoutTable(i.dispositions, size, identity.dispositions) ||
      !layoutTable(i.coordinators, size, identity.coordinators) ||
      !layoutTable(i.settlements, size, identity.settlements) ||
      !layoutTable(i.attempts, size, identity.attempts) ||
      !layoutTable(t.plans, size, topology.plans) ||
      !layoutTable(t.placements, size, topology.placements) ||
      size > std::numeric_limits<std::size_t>::max()) return Status::CapacityExceeded;
  transfer.totalBytes = size;
  try {
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    std::memcpy(bytes.data(), &transfer, sizeof(transfer));
    storeTable(bytes, i.sources, identity.sources);
    storeTable(bytes, i.initializers, identity.initializers);
    storeTable(bytes, i.commands, identity.commands);
    storeTable(bytes, i.dispositions, identity.dispositions);
    storeTable(bytes, i.coordinators, identity.coordinators);
    storeTable(bytes, i.settlements, identity.settlements);
    storeTable(bytes, i.attempts, identity.attempts);
    storeTable(bytes, t.plans, topology.plans);
    storeTable(bytes, t.placements, topology.placements);
    out = std::move(bytes);
    return Status::Valid;
  } catch (...) {
    return Status::AllocationFailed;
  }
}

Status decodeRenderTapeIdentityV4SnapshotTransfer(
    std::span<const std::byte> bytes,
    RenderTapeIdentityV4CaptureKey expectedCapture,
    std::uint64_t expectedQueue,
    RenderTapeIdentityV4View& identityOut,
    RenderTapeExecutionTopologyV1View& topologyOut,
    RenderTapeActionWitnessV1* witnessOut,
    std::vector<std::byte>* attachmentHistoryOut) noexcept {
  identityOut = {};
  topologyOut = {};
  if (witnessOut) *witnessOut = {};
  if (attachmentHistoryOut) attachmentHistoryOut->clear();
  RenderTapeHistoryTransferHeader envelope;
  if (bytes.size() >= sizeof(envelope)) {
    std::memcpy(&envelope, bytes.data(), sizeof(envelope));
    if (envelope.magic == RenderTapeHistoryTransferHeader{}.magic) {
      if (envelope.version != 4u || envelope.headerBytes != sizeof(envelope) ||
          envelope.totalBytes != bytes.size() || envelope.totalBytes > UINT32_MAX ||
          std::uint64_t(envelope.snapshotBytes) + envelope.historyBytes + sizeof(envelope) != bytes.size() ||
          envelope.snapshotBytes < sizeof(RenderTapeIdentityV4TransferHeader) ||
          envelope.historyBytes < sizeof(RenderTapeAttachmentHistoryHeader) ||
          envelope.historyBytes > sizeof(RenderTapeAttachmentHistoryHeader) +
              kAttachmentHistoryMaxRows * sizeof(RenderTapeAttachmentHistoryRow))
        return Status::InvalidLayout;
      const auto snapshot = bytes.subspan(sizeof(envelope), envelope.snapshotBytes);
      std::uint64_t innerMagic;
      std::memcpy(&innerMagic, snapshot.data(), sizeof(innerMagic));
      // No recursion, nested wrappers or legacy unknown witness inside V4.
      if (innerMagic != kRenderTapeIdentityV4TransferMagic) return Status::InvalidHeader;
      RenderTapeIdentityV4View identity;
      RenderTapeExecutionTopologyV1View topology;
      RenderTapeActionWitnessV1 witness;
      const auto status = decodeRenderTapeIdentityV4SnapshotTransfer(snapshot, expectedCapture,
          expectedQueue, identity, topology, &witness);
      if (status != Status::Valid) return status;
      const auto history = bytes.subspan(sizeof(envelope) + envelope.snapshotBytes);
      if (!renderTapeAttachmentHistoryMatchesCapture(history, identity.header, witness))
        return Status::CaptureMismatch;
      try {
        std::vector<std::byte> ownedHistory(history.begin(), history.end());
        identityOut = std::move(identity);
        topologyOut = std::move(topology);
        if (witnessOut) *witnessOut = witness;
        if (attachmentHistoryOut) *attachmentHistoryOut = std::move(ownedHistory);
        return Status::Valid;
      } catch (...) { return Status::AllocationFailed; }
    }
  }
  RenderTapeIdentityV4TransferHeader transfer{};
  RenderTapeIdentityV4TransferLegacyHeader legacy{};
  if (bytes.size() < sizeof(legacy)) return Status::InvalidHeader;
  std::memcpy(&legacy, bytes.data(), sizeof(legacy));
  std::uint64_t headerBytes = 0u;
  if (legacy.magic == kRenderTapeIdentityV4TransferLegacyMagic &&
      legacy.version == kRenderTapeIdentityV4TransferLegacyVersion) {
    // The common V2 prefix stops before the new explicit witness. Its first
    // table may contain any POD row bytes; none can create action authority.
    std::memcpy(&transfer, &legacy, sizeof(legacy));
    headerBytes = sizeof(legacy);
  } else if (legacy.magic == kRenderTapeIdentityV4TransferMagic &&
      legacy.version == kRenderTapeIdentityV4TransferVersion) {
    if (bytes.size() < sizeof(transfer)) return Status::InvalidHeader;
    std::memcpy(&transfer, bytes.data(), sizeof(transfer));
    headerBytes = sizeof(transfer);
  } else return Status::InvalidHeader;
  const auto& i = transfer.identity;
  const auto& t = transfer.topology;
  if (transfer.headerSize != headerBytes || transfer.reserved0 || transfer.reserved1 ||
      !snapshotHeadersAdmitted(i, t) ||
      !renderTapeActionWitnessV1Valid(transfer.actionWitness)) return Status::InvalidHeader;
  if (!sameCapture(i, t) || !expectedCapture.token || !expectedCapture.epoch || !expectedQueue ||
      i.captureToken != expectedCapture.token || i.captureEpoch != expectedCapture.epoch ||
      i.queue != expectedQueue) return Status::CaptureMismatch;
  if (transfer.actionWitness.evidence == static_cast<std::uint32_t>(
          RenderTapeActionWitnessEvidenceV1::QueueObserved) &&
      !renderTapeActionWitnessV1MatchesBinding(transfer.actionWitness,
          expectedCapture.token, expectedCapture.epoch, expectedQueue))
    return Status::CaptureMismatch;
  if (transfer.totalBytes > UINT32_MAX) return Status::CapacityExceeded;
  if (transfer.totalBytes != bytes.size()) return Status::InvalidLayout;
  std::uint64_t size = headerBytes;
  for (const auto status : {
      admitTable<RenderTapeIdentityV4Source>(i.sources, size, bytes.size()),
      admitTable<RenderTapeIdentityV3Initializer>(i.initializers, size, bytes.size()),
      admitTable<RenderTapeIdentityV4PhysicalCommand>(i.commands, size, bytes.size()),
      admitTable<RenderTapeIdentityV3Disposition>(i.dispositions, size, bytes.size()),
      admitTable<RenderTapeIdentityV4Coordinator>(i.coordinators, size, bytes.size()),
      admitTable<RenderTapeIdentityV4RawSettlement>(i.settlements, size, bytes.size()),
      admitTable<RenderTapeIdentityV4ReplayAttempt>(i.attempts, size, bytes.size()),
      admitTable<RenderTapeExecutionTopologyV1Plan>(t.plans, size, bytes.size()),
      admitTable<RenderTapeExecutionTopologyV1Placement>(t.placements, size, bytes.size())}) {
    if (status != Status::Valid) return status;
  }
  if (size != transfer.totalBytes) return Status::InvalidLayout;
  try {
    RenderTapeIdentityV4View identity;
    RenderTapeExecutionTopologyV1View topology;
    identity.header = i;
    topology.header = t;
    copyTable(identity.sources, bytes, i.sources);
    copyTable(identity.initializers, bytes, i.initializers);
    copyTable(identity.commands, bytes, i.commands);
    copyTable(identity.dispositions, bytes, i.dispositions);
    copyTable(identity.coordinators, bytes, i.coordinators);
    copyTable(identity.settlements, bytes, i.settlements);
    copyTable(identity.attempts, bytes, i.attempts);
    if (!coordinatorDomainsAdmitted(identity)) return Status::InvalidLayout;
    copyTable(topology.plans, bytes, t.plans);
    copyTable(topology.placements, bytes, t.placements);
    identityOut = std::move(identity);
    topologyOut = std::move(topology);
    if (witnessOut) *witnessOut = transfer.actionWitness;
    return Status::Valid;
  } catch (...) {
    return Status::AllocationFailed;
  }
}

const char* renderTapeIdentityV4TransferStatusName(Status status) noexcept {
  switch (status) {
  case Status::Valid: return "valid";
  case Status::InvalidHeader: return "invalid-header";
  case Status::InvalidLayout: return "invalid-layout";
  case Status::CaptureMismatch: return "capture-mismatch";
  case Status::CapacityExceeded: return "capacity-exceeded";
  case Status::AllocationFailed: return "allocation-failed";
  }
  return "unknown";
}

} // namespace dxmt9::d3d9
