#include "device_c_render_tape_source_plan_v4.hpp"

#include "device_c_render_tape_capture.hpp"
#include "device_c_render_tape_provenance_validation.hpp"

#include <algorithm>
#include <cstring>
#include <initializer_list>
#include <new>
#include <string_view>
#include <utility>

namespace dxmt9::d3d9 {
namespace {

using Status = CaptureSourceReplayPlanV4Status;
using Action = CaptureSourceReplayPlanV4Action;
using ActionKind = CaptureSourceReplayPlanV4ActionKind;
using Cut = RenderTapeIdentityV3CutKind;
constexpr auto noIndex = kRenderTapeIdentityV4NoIndex;

void u32(std::vector<std::byte>& bytes, std::initializer_list<std::uint32_t> values) {
  for (const auto value : values)
    for (unsigned shift = 0u; shift != 32u; shift += 8u)
      bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffu));
}
void u64(std::vector<std::byte>& bytes, std::initializer_list<std::uint64_t> values) {
  for (const auto value : values)
    for (unsigned shift = 0u; shift != 64u; shift += 8u)
      bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffu));
}
void digest(std::vector<std::byte>& bytes, const RenderTapeDigest& value) {
  bytes.insert(bytes.end(), value.begin(), value.end());
}
void table(std::vector<std::byte>& bytes, RenderTapeIdentityV3Table value) {
  u32(bytes, {value.offset, value.count, value.entrySize});
}
void wireIdentity(std::vector<std::byte>& bytes, D9CWireObjectIdentity value) {
  u32(bytes, {value.kind, value.generation});
  u64(bytes, {value.objectId});
}
void replayIdentity(std::vector<std::byte>& bytes,
    RenderTapeIdentityV4ReplaySourceIdentity value) {
  u64(bytes, {value.frame, value.source, value.lastSource, value.sequence});
}

Status referencedBlobs(std::span<const std::byte> tape,
    const RenderTapeBlobCatalogue& catalogue, std::vector<RenderTapeBlob>& output) {
  ImportedRenderTapeView imported;
  // This is an already fully admitted tape; this import only locates the
  // immutable references. It does not replace the shared canonical grammar.
  if (!importPrevalidatedRenderTape(tape, imported)) return Status::InvalidInput;
  const auto reference = [&](const RenderTapeDigest& hash, std::uint64_t size) {
    if (!size) return false;
    const auto prior = std::find_if(output.begin(), output.end(),
        [&](const auto& blob) { return blob.digest == hash; });
    if (prior != output.end() && prior->size != size) return false;
    bool verified = false;
    for (const auto& blob : catalogue.blobs) {
      if (blob.digest != hash) continue;
      if (blob.size != size || blob.reserved0 || blob.verified != 1u) return false;
      verified = true;
    }
    if (!verified) return false;
    if (prior == output.end()) output.push_back({.digest = hash, .size = size, .verified = 1u});
    return true;
  };
  for (std::size_t index = 0u; index < imported.events.size(); ++index) {
    const auto event = imported.event(index);
    if (event.header.type == static_cast<std::uint32_t>(RenderTapeEventType::ObjectDefine)) {
      RenderTapeObjectDefineHeader header{};
      if (event.payload.size() < sizeof(header)) return Status::InvalidInput;
      std::memcpy(&header, event.payload.data(), sizeof(header));
      if (header.payloadValidity == static_cast<std::uint32_t>(RenderTapeDigestValidity::Sha256) &&
          !reference(header.immutablePayloadDigest, header.immutablePayloadBytes))
        return Status::BlobIdentityMismatch;
    } else if (event.header.type == static_cast<std::uint32_t>(RenderTapeEventType::ResourceMutation)) {
      RenderTapeResourceMutationHeader header{};
      if (event.payload.size() < sizeof(header)) return Status::InvalidInput;
      std::memcpy(&header, event.payload.data(), sizeof(header));
      if (!reference(header.digest, header.byteSize)) return Status::BlobIdentityMismatch;
    }
  }
  std::sort(output.begin(), output.end(),
      [](const auto& left, const auto& right) { return left.digest < right.digest; });
  return Status::Valid;
}

struct Publication {
  std::uint64_t eventOrdinal = 0u, edge = 0u;
  std::uint32_t position = 0u, cutKind = 0u, cutRecord = 0u, tableIndex = 0u;
  ActionKind kind = ActionKind::ExpectSourcePublication;
};
std::uint32_t position(std::uint32_t kind, std::uint32_t record) noexcept {
  if (kind == static_cast<std::uint32_t>(Cut::BeforeEvent)) return 0u;
  return kind == static_cast<std::uint32_t>(Cut::AfterEvent) ? UINT32_MAX : record;
}

Status actions(const RenderTapeIdentityV4View& identity,
    std::span<const CaptureSourceReplayPlanV4Event> events, std::size_t records,
    std::vector<Action>& output) {
  const auto maximum = std::uint64_t{events.size()} + records + identity.sources.size() +
      identity.initializers.size() + identity.settlements.size();
  if (maximum > UINT32_MAX) return Status::CapacityExceeded;
  output.reserve(static_cast<std::size_t>(maximum));
  std::vector<Publication> publications;
  publications.reserve(identity.sources.size() + identity.initializers.size());
  for (std::uint32_t index = 0u; index < identity.sources.size(); ++index) {
    const auto& row = identity.sources[index].lifecycle;
    publications.push_back({row.cutEventOrdinal, row.publicationEdge,
        position(row.cutKind, row.cutRecord), row.cutKind, row.cutRecord,
        index, ActionKind::ExpectSourcePublication});
  }
  for (std::uint32_t index = 0u; index < identity.initializers.size(); ++index) {
    const auto& row = identity.initializers[index];
    publications.push_back({row.cutEventOrdinal, row.publicationEdge,
        position(row.cutKind, row.cutRecord), row.cutKind, row.cutRecord,
        index, ActionKind::ExpectInitializerPublication});
  }
  std::sort(publications.begin(), publications.end(), [](const auto& left, const auto& right) {
    if (left.eventOrdinal != right.eventOrdinal) return left.eventOrdinal < right.eventOrdinal;
    if (left.position != right.position) return left.position < right.position;
    return left.edge < right.edge;
  });
  auto previousEdge = identity.header.baselineCompletionEdge;
  for (const auto& publication : publications) {
    if (publication.edge <= previousEdge) return Status::InvalidPublicationOrder;
    previousEdge = publication.edge;
  }
  std::size_t publicationIndex = 0u;
  std::uint32_t dispositionIndex = 0u;
  const auto atCut = [&](std::uint32_t eventIndex, std::uint32_t cutPosition) {
    const auto ordinal = events[eventIndex].eventOrdinal;
    while (publicationIndex < publications.size() &&
        publications[publicationIndex].eventOrdinal == ordinal &&
        publications[publicationIndex].position == cutPosition) {
      const auto& publication = publications[publicationIndex++];
      output.push_back({.kind = publication.kind, .eventOrdinal = ordinal,
          .evidenceEdge = publication.edge, .eventIndex = eventIndex,
          .tableIndex = publication.tableIndex, .cutKind = publication.cutKind,
          .cutRecord = publication.cutRecord});
    }
  };
  for (std::uint32_t eventIndex = 0u; eventIndex < events.size(); ++eventIndex) {
    const auto& event = events[eventIndex];
    atCut(eventIndex, 0u);
    if (event.commandChunkIndex != noIndex) {
      auto attemptIndex = event.firstAttemptIndex;
      const auto attemptEnd = std::uint64_t{attemptIndex} + event.attemptCount;
      for (std::uint32_t record = 0u; record < event.recordCount; ++record) {
        while (attemptIndex < attemptEnd &&
            (identity.attempts[attemptIndex].outcome != static_cast<std::uint32_t>(
                RenderTapeIdentityV4AttemptOutcome::Committed) ||
             std::uint64_t{identity.attempts[attemptIndex].firstRecord} +
                identity.attempts[attemptIndex].recordCount <= record)) ++attemptIndex;
        while (dispositionIndex < identity.dispositions.size() &&
            (identity.dispositions[dispositionIndex].eventOrdinal < event.eventOrdinal ||
             (identity.dispositions[dispositionIndex].eventOrdinal == event.eventOrdinal &&
              std::uint64_t{identity.dispositions[dispositionIndex].firstRecord} +
                 identity.dispositions[dispositionIndex].recordCount <= record))) ++dispositionIndex;
        if (attemptIndex >= attemptEnd || dispositionIndex >= identity.dispositions.size())
          return Status::InvalidIdentity;
        const auto& attempt = identity.attempts[attemptIndex];
        const auto& disposition = identity.dispositions[dispositionIndex];
        if (attempt.eventOrdinal != event.eventOrdinal || record < attempt.firstRecord ||
            disposition.eventOrdinal != event.eventOrdinal || record < disposition.firstRecord)
          return Status::InvalidIdentity;
        output.push_back({.kind = ActionKind::DispatchRecord, .eventOrdinal = event.eventOrdinal,
            .eventIndex = eventIndex, .recordIndex = record, .dispositionIndex = dispositionIndex,
            .attemptIndex = attemptIndex, .tableIndex = disposition.kind == static_cast<std::uint32_t>(
                RenderTapeIdentityV3DispositionKind::Coordinator) ? disposition.coordinatorIndex : noIndex});
        atCut(eventIndex, record + 1u);
      }
    } else {
      output.push_back({.kind = ActionKind::ReplayEvent, .eventOrdinal = event.eventOrdinal,
          .eventIndex = eventIndex});
    }
    atCut(eventIndex, UINT32_MAX);
    if (event.commandChunkIndex != noIndex) {
      output.push_back({.kind = ActionKind::AcknowledgeRaw, .eventOrdinal = event.eventOrdinal,
          .evidenceEdge = identity.settlements[event.rawSettlementIndex].replayEdge,
          .eventIndex = eventIndex, .tableIndex = event.rawSettlementIndex});
    }
  }
  return publicationIndex == publications.size() ? Status::Valid : Status::InvalidPublicationOrder;
}

// Format 1: domain without NUL; u32 format; (u64 exact bytes, SHA256) for
// Tape, identity.v4, topology.v1; sorted referenced blobs; complete headers and
// every owned table in declared field order, with u32 counts. Derived event,
// canonical-record and action tables follow. Integers are explicit LE, signed
// result bits are encoded as u32, and there is no native padding. Exact hashes
// bind layouts and original event bytes; semantic fields bind the owned plan.
std::vector<std::byte> canonicalDigestBytes(std::span<const std::byte> tape,
    std::span<const std::byte> identityBytes, std::span<const std::byte> topologyBytes,
    const CaptureSourceReplayPlanV4& plan) {
  std::vector<std::byte> bytes;
  constexpr std::string_view domain = kCaptureSourceReplayPlanV4DigestDomain;
  const auto domainBytes = std::as_bytes(std::span(domain.data(), domain.size()));
  bytes.insert(bytes.end(), domainBytes.begin(), domainBytes.end());
  u32(bytes, {kCaptureSourceReplayPlanV4FormatVersion});
  for (const auto component : {tape, identityBytes, topologyBytes}) {
    u64(bytes, {component.size()});
    digest(bytes, RenderTapeCaptureSession::sha256(component));
  }
  u32(bytes, {static_cast<std::uint32_t>(plan.referencedBlobs().size())});
  for (const auto& row : plan.referencedBlobs()) {
    digest(bytes, row.digest);
    u64(bytes, {row.size});
    u32(bytes, {row.verified, row.reserved0});
  }
  const auto& identity = plan.identity();
  const auto& h = identity.header;
  u64(bytes, {h.magic, h.eventsBytes, h.frameId, h.presentOrdinal, h.captureToken});
  digest(bytes, h.eventsDigest);
  u32(bytes, {h.version, h.headerSize});
  u64(bytes, {h.captureEpoch, h.queue, h.baselineSequence, h.baselineInitializer,
      h.baselineCompletedSequence, h.initializerEpoch, h.baselineCompletedInitializer,
      h.baselineCompletionEdge});
  u32(bytes, {h.baselineKind, h.authority});
  for (const auto descriptor : {h.sources, h.initializers, h.commands, h.dispositions,
      h.coordinators, h.settlements, h.attempts}) table(bytes, descriptor);
  u32(bytes, {h.requiredExecutionTopologyVersion, h.reserved0, h.reserved1, h.reserved2, h.reserved3,
      h.coordinatorDomain, h.reserved4});
  u32(bytes, {static_cast<std::uint32_t>(identity.sources.size())});
  for (const auto& row : identity.sources) {
    const auto& life = row.lifecycle;
    u64(bytes, {life.sourceOrdinal, life.seqId, life.writerCaptureId, life.writerGeneration,
        life.storageGeneration, life.publicationEdge, life.submissionEdge, life.completionEdge,
        life.completedSequence, life.cutEventOrdinal});
    u32(bytes, {life.commandCount, life.cutKind, life.cutRecord, life.reserved0});
    u64(bytes, {row.rawOrdinal, row.publicationBuildGeneration});
    u32(bytes, {row.publicationControlIndex, row.reserved0});
  }
  u32(bytes, {static_cast<std::uint32_t>(identity.initializers.size())});
  for (const auto& row : identity.initializers) {
    u64(bytes, {row.sequence, row.epoch, row.publicationEdge, row.submissionEdge,
        row.completionEdge, row.completedSequence, row.cutEventOrdinal});
    u32(bytes, {row.cutKind, row.cutRecord, row.reserved0, row.reserved1});
  }
  u32(bytes, {static_cast<std::uint32_t>(identity.commands.size())});
  for (const auto& row : identity.commands)
    u32(bytes, {row.sourceIndex, row.physicalCommandIndex, row.kind, row.drawCount,
        row.coordinatorIndex, row.reserved0});
  u32(bytes, {static_cast<std::uint32_t>(identity.dispositions.size())});
  for (const auto& row : identity.dispositions) {
    u64(bytes, {row.eventOrdinal});
    u32(bytes, {row.firstRecord, row.recordCount, row.kind, row.commandIndex,
        row.firstSubdraw, row.subdrawCount, row.coordinatorIndex, row.reserved0});
  }
  u32(bytes, {static_cast<std::uint32_t>(identity.coordinators.size())});
  for (const auto& coordinator : identity.coordinators) {
    const auto& row = coordinator.provenance;
    u64(bytes, {coordinator.admissionEdge});
    u32(bytes, {coordinator.authority, coordinator.reserved0});
    u64(bytes, {row.eventOrdinal, row.rawOrdinal, row.buildGeneration, row.dispatchEdge,
        row.releaseOrdinal, row.releaseGeneration});
    wireIdentity(bytes, row.queryIdentity);
    u64(bytes, {row.issueCycle, row.issueOrdinal, row.deviceEpoch, row.writerCaptureId,
        row.writerGeneration, row.storageGeneration, row.prefixSequence,
        row.prefixInitializer, row.prefixInitializerEpoch});
    u32(bytes, {row.recordIndex, row.recordType, row.flags, row.queryType,
        static_cast<std::uint32_t>(row.dispatchResult), row.releaseAction, row.releaseWriter,
        row.prefixKind, row.sourceIndex, row.initializerIndex, row.commandEnd,
        row.observation, row.reserved0, row.reserved1});
  }
  u32(bytes, {static_cast<std::uint32_t>(identity.settlements.size())});
  for (const auto& row : identity.settlements) {
    u64(bytes, {row.eventOrdinal, row.rawOrdinal, row.replayEdge});
    u32(bytes, {row.recordCount, row.reserved0});
  }
  u32(bytes, {static_cast<std::uint32_t>(identity.attempts.size())});
  for (const auto& row : identity.attempts) {
    u64(bytes, {row.eventOrdinal, row.rawOrdinal, row.attemptGeneration,
        row.beginEdge, row.terminalEdge, row.effectEdge});
    replayIdentity(bytes, row.identity);
    replayIdentity(bytes, row.receipt.identity);
    u64(bytes, {row.receipt.queueSequence, row.receipt.buildGeneration,
        row.receipt.sourceGeneration, row.receipt.storageGeneration});
    u32(bytes, {row.receipt.kind, row.receipt.controlIndex, row.receipt.commandCount,
        row.receipt.reserved0, row.firstRecord, row.recordCount, row.outcome, row.stagedCommandCount});
  }
  const auto& topology = plan.topology();
  const auto& t = topology.header;
  u64(bytes, {t.magic, t.identityBytes, t.eventsBytes, t.captureToken, t.captureEpoch, t.queue});
  digest(bytes, t.identityDigest); digest(bytes, t.eventsDigest);
  u32(bytes, {t.version, t.headerSize});
  table(bytes, t.plans); table(bytes, t.placements);
  u32(bytes, {t.reserved0, t.reserved1});
  u32(bytes, {static_cast<std::uint32_t>(topology.plans.size())});
  for (const auto& row : topology.plans) {
    u64(bytes, {row.planId, row.acceptanceEdge});
    u32(bytes, {row.route, row.fallbackReason, row.firstPlacement,
        row.placementCount, row.executedCount, row.reserved0});
  }
  u32(bytes, {static_cast<std::uint32_t>(topology.placements.size())});
  for (const auto& row : topology.placements)
    u32(bytes, {row.planIndex, row.commandIndex, row.outcome, row.executionOrdinal,
        row.dagPassIndex, row.passKind, row.omissionReason, row.reserved0});
  u32(bytes, {static_cast<std::uint32_t>(plan.events().size())});
  for (const auto& row : plan.events()) {
    u64(bytes, {row.eventOrdinal, row.rawOrdinal});
    u32(bytes, {row.type, row.recordCount, row.commandChunkIndex,
        row.firstAttemptIndex, row.attemptCount, row.rawSettlementIndex});
  }
  u32(bytes, {static_cast<std::uint32_t>(plan.records().size())});
  for (const auto& row : plan.records()) {
    u64(bytes, {row.eventOrdinal});
    u32(bytes, {row.recordIndex, row.recordType, row.requiredReleaseAction, row.queryFlags});
    wireIdentity(bytes, row.queryIdentity);
    u32(bytes, {row.queryType});
    u64(bytes, {row.issueCycle, row.issueOrdinal});
  }
  u32(bytes, {static_cast<std::uint32_t>(plan.actions().size())});
  for (const auto& row : plan.actions()) {
    u32(bytes, {static_cast<std::uint32_t>(row.kind)});
    u64(bytes, {row.eventOrdinal, row.evidenceEdge});
    u32(bytes, {row.eventIndex, row.recordIndex, row.dispositionIndex, row.attemptIndex,
        row.tableIndex, row.cutKind, row.cutRecord});
  }
  return bytes;
}

} // namespace

Status buildCaptureSourceReplayPlanV4(std::span<const std::byte> tape,
    const RenderTapeBlobCatalogue& verifiedCatalogue, std::span<const std::byte> identityBytes,
    std::span<const std::byte> topologyBytes, CaptureSourceReplayPlanV4& out) noexcept {
  out = CaptureSourceReplayPlanV4{};
  const auto reject = [&](Status status) { out.status_ = status; return status; };
  if (tape.empty() || identityBytes.empty() || topologyBytes.empty()) return Status::InvalidInput;
  try {
    CaptureSourceReplayPlanV4 candidate;
    const auto identityStatus = validateRenderTapeIdentityV4(
        tape, verifiedCatalogue, identityBytes, &candidate.identity_, true);
    if (identityStatus == RenderTapeIdentityV4Status::AllocationFailed) return reject(Status::AllocationFailed);
    if (identityStatus == RenderTapeIdentityV4Status::CapacityExceeded) return reject(Status::CapacityExceeded);
    if (identityStatus != RenderTapeIdentityV4Status::Valid) return reject(Status::InvalidIdentity);
    const auto topologyStatus = validateRenderTapeExecutionTopologyV1(
        tape, verifiedCatalogue, identityBytes, topologyBytes, &candidate.topology_);
    if (topologyStatus == RenderTapeExecutionTopologyV1Status::AllocationFailed) return reject(Status::AllocationFailed);
    if (topologyStatus == RenderTapeExecutionTopologyV1Status::CapacityExceeded) return reject(Status::CapacityExceeded);
    if (topologyStatus != RenderTapeExecutionTopologyV1Status::Valid) return reject(Status::InvalidTopology);
    std::vector<RenderTapeIdentityV3EventFact> events;
    const auto facts = buildTapeCanonicalFacts(tape, verifiedCatalogue,
        candidate.identity_.header.presentOrdinal, kRenderTapeIdentityV4MaxRows,
        events, candidate.records_);
    if (facts == TapeProvenanceStatus::AllocationFailed) return reject(Status::AllocationFailed);
    if (facts == TapeProvenanceStatus::CapacityExceeded) return reject(Status::CapacityExceeded);
    if (facts != TapeProvenanceStatus::Valid) return reject(Status::InvalidInput);
    candidate.events_.reserve(events.size());
    std::uint32_t chunkIndex = 0u, settlementIndex = 0u, attemptIndex = 0u;
    for (const auto& event : events) {
      CaptureSourceReplayPlanV4Event metadata{.eventOrdinal = event.ordinal,
          .type = event.type, .recordCount = event.recordCount};
      if (event.type == static_cast<std::uint32_t>(RenderTapeEventType::CommandChunk)) {
        if (settlementIndex >= candidate.identity_.settlements.size() ||
            candidate.identity_.settlements[settlementIndex].eventOrdinal != event.ordinal)
          return reject(Status::InvalidIdentity);
        metadata.commandChunkIndex = chunkIndex++;
        metadata.rawSettlementIndex = settlementIndex;
        metadata.rawOrdinal = candidate.identity_.settlements[settlementIndex++].rawOrdinal;
        metadata.firstAttemptIndex = attemptIndex;
        while (attemptIndex < candidate.identity_.attempts.size() &&
            candidate.identity_.attempts[attemptIndex].eventOrdinal == event.ordinal) ++attemptIndex;
        metadata.attemptCount = attemptIndex - metadata.firstAttemptIndex;
        if (!metadata.attemptCount) return reject(Status::InvalidIdentity);
      }
      candidate.events_.push_back(metadata);
    }
    if (attemptIndex != candidate.identity_.attempts.size() ||
        settlementIndex != candidate.identity_.settlements.size()) return reject(Status::InvalidIdentity);
    auto status = referencedBlobs(tape, verifiedCatalogue, candidate.referencedBlobs_);
    if (status != Status::Valid) return reject(status);
    status = actions(candidate.identity_, candidate.events_, candidate.records_.size(), candidate.actions_);
    if (status != Status::Valid) return reject(status);
    candidate.digestCanonicalBytes_ = canonicalDigestBytes(tape, identityBytes, topologyBytes, candidate);
    candidate.digest_ = RenderTapeCaptureSession::sha256(candidate.digestCanonicalBytes_);
    candidate.status_ = Status::Valid;
    out = std::move(candidate);
    return Status::Valid;
  } catch (const std::bad_alloc&) { return reject(Status::AllocationFailed); }
  catch (...) { return reject(Status::InvalidInput); }
}

CaptureSourceReplayPlanV4::CaptureSourceReplayPlanV4(CaptureSourceReplayPlanV4&& other) noexcept {
  *this = std::move(other);
}
CaptureSourceReplayPlanV4& CaptureSourceReplayPlanV4::operator=(CaptureSourceReplayPlanV4&& other) noexcept {
  if (this == &other) return *this;
  status_ = other.status_;
  identity_ = std::move(other.identity_); topology_ = std::move(other.topology_);
  events_ = std::move(other.events_); records_ = std::move(other.records_);
  actions_ = std::move(other.actions_); referencedBlobs_ = std::move(other.referencedBlobs_);
  digestCanonicalBytes_ = std::move(other.digestCanonicalBytes_); digest_ = other.digest_;
  other.status_ = Status::InvalidInput; other.digest_ = {};
  return *this;
}
bool CaptureSourceReplayCursorV4::begin(const CaptureSourceReplayPlanV4& plan) noexcept {
  if (status_ != CaptureSourceReplayCursorV4Status::Uninitialized || !plan.valid()) {
    status_ = CaptureSourceReplayCursorV4Status::Rejected; return false;
  }
  digest_ = plan.digest(); count_ = plan.actions().size();
  status_ = count_ ? CaptureSourceReplayCursorV4Status::Ready : CaptureSourceReplayCursorV4Status::Complete;
  return true;
}
bool CaptureSourceReplayCursorV4::bound(const CaptureSourceReplayPlanV4& plan) noexcept {
  if ((status_ != CaptureSourceReplayCursorV4Status::Ready && status_ != CaptureSourceReplayCursorV4Status::Complete) ||
      !plan.valid() || plan.digest() != digest_ || plan.actions().size() != count_) {
    status_ = CaptureSourceReplayCursorV4Status::Rejected; return false;
  }
  return true;
}
bool CaptureSourceReplayCursorV4::peek(const CaptureSourceReplayPlanV4& plan, Action& action) noexcept {
  if (!bound(plan) || next_ == count_) return false;
  action = plan.actions()[next_]; return true;
}
bool CaptureSourceReplayCursorV4::acknowledge(const CaptureSourceReplayPlanV4& plan, const Action& action) noexcept {
  if (!bound(plan) || next_ == count_ || plan.actions()[next_] != action) {
    status_ = CaptureSourceReplayCursorV4Status::Rejected; return false;
  }
  if (++next_ == count_) status_ = CaptureSourceReplayCursorV4Status::Complete;
  return true;
}
const char* captureSourceReplayPlanV4StatusName(Status status) noexcept {
  switch (status) {
  case Status::Valid: return "valid";
  case Status::InvalidInput: return "invalid-input";
  case Status::InvalidIdentity: return "invalid-identity";
  case Status::InvalidTopology: return "invalid-topology";
  case Status::BlobIdentityMismatch: return "blob-identity-mismatch";
  case Status::InvalidPublicationOrder: return "invalid-publication-order";
  case Status::CapacityExceeded: return "capacity-exceeded";
  case Status::AllocationFailed: return "allocation-failed";
  }
  return "unknown";
}

} // namespace dxmt9::d3d9
