#include "device_c_render_tape_source_plan_v3.hpp"

#include "device_c_render_tape_capture.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <string_view>
#include <utility>

namespace dxmt9::d3d9 {
namespace {

using Status = CaptureSourceReplayPlanV3Status;
using Action = CaptureSourceReplayPlanV3Action;
using ActionKind = CaptureSourceReplayPlanV3ActionKind;
using Cut = RenderTapeIdentityV3CutKind;

template <typename T>
bool load(std::span<const std::byte> bytes, T& value) noexcept {
  if (bytes.size() < sizeof(value)) return false;
  std::memcpy(&value, bytes.data(), sizeof(value));
  return true;
}

void appendU32(std::vector<std::byte>& bytes, std::uint32_t value) {
  for (unsigned shift = 0u; shift != 32u; shift += 8u)
    bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffu));
}

void appendU64(std::vector<std::byte>& bytes, std::uint64_t value) {
  for (unsigned shift = 0u; shift != 64u; shift += 8u)
    bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffu));
}

void appendDigest(std::vector<std::byte>& bytes, const RenderTapeDigest& digest) {
  bytes.insert(bytes.end(), digest.begin(), digest.end());
}

Status referencedBlobs(const ImportedRenderTapeView& tape,
    const RenderTapeBlobCatalogue& catalogue,
    std::vector<RenderTapeBlob>& output) {
  const auto reference = [&](const RenderTapeDigest& digest,
                             std::uint64_t size) {
    if (!size) return false;
    const auto prior = std::find_if(output.begin(), output.end(),
        [&](const auto& blob) { return blob.digest == digest; });
    if (prior != output.end() && prior->size != size) return false;
    bool verified = false;
    for (const auto& blob : catalogue.blobs) {
      if (blob.digest != digest) continue;
      // Catalogue order must not hide a conflicting same-digest length.
      if (blob.size != size || blob.reserved0 || blob.verified != 1u)
        return false;
      verified = true;
    }
    if (!verified) return false;
    if (prior == output.end())
      output.push_back({.digest = digest, .size = size, .verified = 1u});
    return true;
  };
  for (std::size_t index = 0u; index < tape.events.size(); ++index) {
    const auto event = tape.event(index);
    const auto type = static_cast<RenderTapeEventType>(event.header.type);
    if (type == RenderTapeEventType::ObjectDefine) {
      RenderTapeObjectDefineHeader header{};
      if (!load(event.payload, header)) return Status::InvalidInput;
      if (header.payloadValidity == static_cast<std::uint32_t>(
              RenderTapeDigestValidity::Sha256) &&
          !reference(header.immutablePayloadDigest, header.immutablePayloadBytes))
        return Status::BlobIdentityMismatch;
    } else if (type == RenderTapeEventType::ResourceMutation) {
      RenderTapeResourceMutationHeader header{};
      if (!load(event.payload, header)) return Status::InvalidInput;
      if (!reference(header.digest, header.byteSize))
        return Status::BlobIdentityMismatch;
    }
  }
  std::sort(output.begin(), output.end(), [](const auto& left, const auto& right) {
    return left.digest < right.digest;
  });
  return Status::Valid;
}

struct Publication {
  std::uint64_t eventOrdinal = 0u;
  std::uint64_t edge = 0u;
  std::uint32_t position = 0u;
  std::uint32_t cutKind = 0u;
  std::uint32_t cutRecord = 0u;
  std::uint32_t tableIndex = 0u;
  ActionKind kind = ActionKind::ExpectSourcePublication;
};

std::uint32_t publicationPosition(std::uint32_t kind,
                                  std::uint32_t record) noexcept {
  if (kind == static_cast<std::uint32_t>(Cut::BeforeEvent)) return 0u;
  if (kind == static_cast<std::uint32_t>(Cut::AfterEvent)) return UINT32_MAX;
  return record;
}

Status actions(const RenderTapeIdentityV3View& identity,
    std::span<const CaptureSourceReplayPlanV3Event> events,
    std::vector<Action>& output) {
  std::vector<Publication> publications;
  publications.reserve(identity.sources.size() + identity.initializers.size());
  for (std::uint32_t index = 0u; index < identity.sources.size(); ++index) {
    const auto& row = identity.sources[index];
    publications.push_back({row.cutEventOrdinal, row.publicationEdge,
        publicationPosition(row.cutKind, row.cutRecord), row.cutKind,
        row.cutRecord, index, ActionKind::ExpectSourcePublication});
  }
  for (std::uint32_t index = 0u; index < identity.initializers.size(); ++index) {
    const auto& row = identity.initializers[index];
    publications.push_back({row.cutEventOrdinal, row.publicationEdge,
        publicationPosition(row.cutKind, row.cutRecord), row.cutKind,
        row.cutRecord, index, ActionKind::ExpectInitializerPublication});
  }
  std::sort(publications.begin(), publications.end(),
      [](const auto& left, const auto& right) {
        if (left.eventOrdinal != right.eventOrdinal)
          return left.eventOrdinal < right.eventOrdinal;
        if (left.position != right.position) return left.position < right.position;
        return left.edge < right.edge;
      });
  std::uint64_t previousEdge = identity.header.baselineCompletionEdge;
  for (const auto& publication : publications) {
    if (publication.edge <= previousEdge) return Status::InvalidPublicationOrder;
    previousEdge = publication.edge;
  }
  std::size_t publicationIndex = 0u;
  std::uint32_t dispositionIndex = 0u;
  std::uint32_t settlementIndex = 0u;
  const auto atCut = [&](std::uint32_t eventIndex, std::uint32_t position) {
    const auto ordinal = events[eventIndex].eventOrdinal;
    while (publicationIndex < publications.size() &&
           publications[publicationIndex].eventOrdinal == ordinal &&
           publications[publicationIndex].position == position) {
      const auto& publication = publications[publicationIndex++];
      output.push_back({.kind = publication.kind,
          .eventOrdinal = ordinal, .publicationEdge = publication.edge,
          .eventIndex = eventIndex, .tableIndex = publication.tableIndex,
          .cutKind = publication.cutKind, .cutRecord = publication.cutRecord});
    }
  };
  for (std::uint32_t eventIndex = 0u; eventIndex < events.size(); ++eventIndex) {
    const auto& event = events[eventIndex];
    atCut(eventIndex, 0u);
    if (event.type == static_cast<std::uint32_t>(RenderTapeEventType::CommandChunk)) {
      for (std::uint32_t record = 0u; record < event.recordCount; ++record) {
        while (dispositionIndex < identity.dispositions.size() &&
               (identity.dispositions[dispositionIndex].eventOrdinal < event.eventOrdinal ||
                (identity.dispositions[dispositionIndex].eventOrdinal == event.eventOrdinal &&
                 std::uint64_t{identity.dispositions[dispositionIndex].firstRecord} +
                     identity.dispositions[dispositionIndex].recordCount <= record)))
          ++dispositionIndex;
        if (dispositionIndex >= identity.dispositions.size())
          return Status::InvalidIdentity;
        const auto& disposition = identity.dispositions[dispositionIndex];
        if (disposition.eventOrdinal != event.eventOrdinal ||
            record < disposition.firstRecord ||
            record >= std::uint64_t{disposition.firstRecord} + disposition.recordCount)
          return Status::InvalidIdentity;
        const auto coordinator = disposition.kind == static_cast<std::uint32_t>(
            RenderTapeIdentityV3DispositionKind::Coordinator)
            ? disposition.coordinatorIndex : kRenderTapeIdentityV3NoIndex;
        output.push_back({.kind = ActionKind::DispatchRecord,
            .eventOrdinal = event.eventOrdinal, .eventIndex = eventIndex,
            .recordIndex = record, .dispositionIndex = dispositionIndex,
            .tableIndex = coordinator});
        atCut(eventIndex, record + 1u);
      }
    } else {
      output.push_back({.kind = ActionKind::ReplayEvent,
          .eventOrdinal = event.eventOrdinal, .eventIndex = eventIndex});
    }
    atCut(eventIndex, UINT32_MAX);
    if (event.type == static_cast<std::uint32_t>(RenderTapeEventType::CommandChunk)) {
      if (settlementIndex >= identity.settlements.size() ||
          identity.settlements[settlementIndex].eventOrdinal != event.eventOrdinal)
        return Status::InvalidIdentity;
      output.push_back({.kind = ActionKind::SettleCommandEvent,
          .eventOrdinal = event.eventOrdinal, .eventIndex = eventIndex,
          .tableIndex = settlementIndex++});
    }
    if (output.size() > UINT32_MAX) return Status::CapacityExceeded;
  }
  return publicationIndex == publications.size() &&
      settlementIndex == identity.settlements.size()
      ? Status::Valid : Status::InvalidPublicationOrder;
}

// Digest format 1, deliberately independent of sidecar native layout:
// domain (without NUL), u32 format, u64 tape bytes + SHA, u64 sidecar bytes +
// SHA, u32 blob count + sorted (SHA,u64 bytes), semantic header below, then
// sources/initializers/commands/dispositions/coordinators/settlements. Each
// table begins with u32 count and encodes every declared row field in order,
// including reserved zeros, with no alignment gaps. Header offsets/sizes and
// eventsDigest are already bound by the exact input component identities.
std::vector<std::byte> canonicalDigestBytes(std::span<const std::byte> tape,
    std::span<const std::byte> sidecar,
    const RenderTapeIdentityV3View& identity,
    std::span<const RenderTapeBlob> blobs) {
  std::vector<std::byte> bytes;
  constexpr std::string_view domain = kCaptureSourceReplayPlanV3DigestDomain;
  const auto domainBytes = std::as_bytes(std::span(domain.data(), domain.size()));
  bytes.insert(bytes.end(), domainBytes.begin(), domainBytes.end());
  appendU32(bytes, kCaptureSourceReplayPlanV3FormatVersion);
  appendU64(bytes, tape.size());
  appendDigest(bytes, RenderTapeCaptureSession::sha256(tape));
  appendU64(bytes, sidecar.size());
  appendDigest(bytes, RenderTapeCaptureSession::sha256(sidecar));
  appendU32(bytes, static_cast<std::uint32_t>(blobs.size()));
  for (const auto& blob : blobs) {
    appendDigest(bytes, blob.digest);
    appendU64(bytes, blob.size);
  }
  const auto& h = identity.header;
  appendU64(bytes, h.frameId);
  appendU64(bytes, h.presentOrdinal);
  appendU64(bytes, h.captureToken);
  appendU32(bytes, h.version);
  appendU64(bytes, h.captureEpoch);
  appendU64(bytes, h.queue);
  appendU64(bytes, h.baselineSequence);
  appendU64(bytes, h.baselineInitializer);
  appendU64(bytes, h.baselineCompletedSequence);
  appendU64(bytes, h.initializerEpoch);
  appendU64(bytes, h.baselineCompletedInitializer);
  appendU64(bytes, h.baselineCompletionEdge);
  appendU32(bytes, h.baselineKind);
  appendU32(bytes, h.authority);
  appendU32(bytes, h.reserved0);
  appendU32(bytes, h.reserved1);
  appendU32(bytes, h.reserved2);
  appendU32(bytes, h.reserved3);

  appendU32(bytes, static_cast<std::uint32_t>(identity.sources.size()));
  for (const auto& row : identity.sources) {
    appendU64(bytes, row.sourceOrdinal);
    appendU64(bytes, row.seqId);
    appendU64(bytes, row.writerCaptureId);
    appendU64(bytes, row.writerGeneration);
    appendU64(bytes, row.storageGeneration);
    appendU64(bytes, row.publicationEdge);
    appendU64(bytes, row.submissionEdge);
    appendU64(bytes, row.completionEdge);
    appendU64(bytes, row.completedSequence);
    appendU64(bytes, row.cutEventOrdinal);
    appendU32(bytes, row.commandCount);
    appendU32(bytes, row.cutKind);
    appendU32(bytes, row.cutRecord);
    appendU32(bytes, row.reserved0);
  }
  appendU32(bytes, static_cast<std::uint32_t>(identity.initializers.size()));
  for (const auto& row : identity.initializers) {
    appendU64(bytes, row.sequence);
    appendU64(bytes, row.epoch);
    appendU64(bytes, row.publicationEdge);
    appendU64(bytes, row.submissionEdge);
    appendU64(bytes, row.completionEdge);
    appendU64(bytes, row.completedSequence);
    appendU64(bytes, row.cutEventOrdinal);
    appendU32(bytes, row.cutKind);
    appendU32(bytes, row.cutRecord);
    appendU32(bytes, row.reserved0);
    appendU32(bytes, row.reserved1);
  }
  appendU32(bytes, static_cast<std::uint32_t>(identity.commands.size()));
  for (const auto& row : identity.commands) {
    appendU64(bytes, row.logicalPassId);
    appendU32(bytes, row.sourceIndex);
    appendU32(bytes, row.physicalCommandIndex);
    appendU32(bytes, row.kind);
    appendU32(bytes, row.drawCount);
    appendU32(bytes, row.membership);
    appendU32(bytes, row.dagPassIndex);
    appendU32(bytes, row.passKind);
    appendU32(bytes, row.coordinatorIndex);
    appendU32(bytes, row.reserved0);
    appendU32(bytes, row.reserved1);
  }
  appendU32(bytes, static_cast<std::uint32_t>(identity.dispositions.size()));
  for (const auto& row : identity.dispositions) {
    appendU64(bytes, row.eventOrdinal);
    appendU32(bytes, row.firstRecord);
    appendU32(bytes, row.recordCount);
    appendU32(bytes, row.kind);
    appendU32(bytes, row.commandIndex);
    appendU32(bytes, row.firstSubdraw);
    appendU32(bytes, row.subdrawCount);
    appendU32(bytes, row.coordinatorIndex);
    appendU32(bytes, row.reserved0);
  }
  appendU32(bytes, static_cast<std::uint32_t>(identity.coordinators.size()));
  for (const auto& row : identity.coordinators) {
    appendU64(bytes, row.eventOrdinal);
    appendU64(bytes, row.rawOrdinal);
    appendU64(bytes, row.buildGeneration);
    appendU64(bytes, row.dispatchEdge);
    appendU64(bytes, row.releaseOrdinal);
    appendU64(bytes, row.releaseGeneration);
    appendU32(bytes, row.queryIdentity.kind);
    appendU32(bytes, row.queryIdentity.generation);
    appendU64(bytes, row.queryIdentity.objectId);
    appendU64(bytes, row.issueCycle);
    appendU64(bytes, row.issueOrdinal);
    appendU64(bytes, row.deviceEpoch);
    appendU64(bytes, row.writerCaptureId);
    appendU64(bytes, row.writerGeneration);
    appendU64(bytes, row.storageGeneration);
    appendU64(bytes, row.prefixSequence);
    appendU64(bytes, row.prefixInitializer);
    appendU64(bytes, row.prefixInitializerEpoch);
    appendU32(bytes, row.recordIndex);
    appendU32(bytes, row.recordType);
    appendU32(bytes, row.flags);
    appendU32(bytes, row.queryType);
    appendU32(bytes, static_cast<std::uint32_t>(row.dispatchResult));
    appendU32(bytes, row.releaseAction);
    appendU32(bytes, row.releaseWriter);
    appendU32(bytes, row.prefixKind);
    appendU32(bytes, row.sourceIndex);
    appendU32(bytes, row.initializerIndex);
    appendU32(bytes, row.commandEnd);
    appendU32(bytes, row.observation);
    appendU32(bytes, row.reserved0);
    appendU32(bytes, row.reserved1);
  }
  appendU32(bytes, static_cast<std::uint32_t>(identity.settlements.size()));
  for (const auto& row : identity.settlements) {
    appendU64(bytes, row.eventOrdinal);
    appendU64(bytes, row.rawOrdinal);
    appendU64(bytes, row.buildGeneration);
    appendU64(bytes, row.replayEdge);
    appendU32(bytes, row.recordCount);
    appendU32(bytes, row.reserved0);
  }
  return bytes;
}

} // namespace

CaptureSourceReplayPlanV3Status buildCaptureSourceReplayPlanV3(
    std::span<const std::byte> tape,
    const RenderTapeBlobCatalogue& verifiedCatalogue,
    std::span<const std::byte> identitySidecar,
    CaptureSourceReplayPlanV3& out) noexcept {
  out = CaptureSourceReplayPlanV3{};
  const auto reject = [&](Status status) {
    out.status_ = status;
    return status;
  };
  if (tape.empty() || identitySidecar.empty()) return Status::InvalidInput;
  try {
    CaptureSourceReplayPlanV3 candidate;
    const auto validation = validateRenderTapeIdentityV3(
        tape, verifiedCatalogue, identitySidecar, &candidate.identity_, true);
    if (validation == RenderTapeIdentityV3Status::AllocationFailed)
      return reject(Status::AllocationFailed);
    if (validation == RenderTapeIdentityV3Status::CapacityExceeded)
      return reject(Status::CapacityExceeded);
    if (validation != RenderTapeIdentityV3Status::Valid)
      return reject(Status::InvalidIdentity);
    ImportedRenderTapeView imported;
    if (!importPrevalidatedRenderTape(tape, imported) ||
        imported.events.size() > kRenderTapeIdentityV3MaxRows)
      return reject(Status::InvalidInput);
    candidate.events_.reserve(imported.events.size());
    std::uint32_t commandChunkIndex = 0u;
    for (std::size_t index = 0u; index < imported.events.size(); ++index) {
      const auto event = imported.event(index);
      CaptureSourceReplayPlanV3Event metadata{
          .eventOrdinal = event.header.ordinal, .type = event.header.type};
      if (event.header.type == static_cast<std::uint32_t>(
              RenderTapeEventType::CommandChunk)) {
        RenderTapeCommandChunkHeader chunk{};
        if (!load(event.payload, chunk)) return reject(Status::InvalidInput);
        metadata.recordCount = chunk.recordCount;
        metadata.commandChunkIndex = commandChunkIndex++;
      }
      candidate.events_.push_back(metadata);
    }
    auto status = referencedBlobs(imported, verifiedCatalogue,
                                  candidate.referencedBlobs_);
    if (status != Status::Valid) return reject(status);
    status = actions(candidate.identity_, candidate.events_, candidate.actions_);
    if (status != Status::Valid) return reject(status);
    candidate.digestCanonicalBytes_ = canonicalDigestBytes(
        tape, identitySidecar, candidate.identity_, candidate.referencedBlobs_);
    candidate.digest_ = RenderTapeCaptureSession::sha256(
        candidate.digestCanonicalBytes_);
    candidate.status_ = Status::Valid;
    out = std::move(candidate);
    return Status::Valid;
  } catch (const std::bad_alloc&) {
    return reject(Status::AllocationFailed);
  } catch (...) {
    return reject(Status::InvalidInput);
  }
}

CaptureSourceReplayPlanV3::CaptureSourceReplayPlanV3(
    CaptureSourceReplayPlanV3&& other) noexcept {
  *this = std::move(other);
}

CaptureSourceReplayPlanV3& CaptureSourceReplayPlanV3::operator=(
    CaptureSourceReplayPlanV3&& other) noexcept {
  if (this == &other) return *this;
  status_ = other.status_;
  identity_ = std::move(other.identity_);
  events_ = std::move(other.events_);
  actions_ = std::move(other.actions_);
  referencedBlobs_ = std::move(other.referencedBlobs_);
  digestCanonicalBytes_ = std::move(other.digestCanonicalBytes_);
  digest_ = other.digest_;
  other.status_ = Status::InvalidInput;
  other.digest_ = {};
  return *this;
}

bool CaptureSourceReplayCursorV3::begin(
    const CaptureSourceReplayPlanV3& plan) noexcept {
  if (status_ != CaptureSourceReplayCursorV3Status::Uninitialized || !plan.valid()) {
    status_ = CaptureSourceReplayCursorV3Status::Rejected;
    return false;
  }
  digest_ = plan.digest();
  count_ = plan.actions().size();
  status_ = count_ ? CaptureSourceReplayCursorV3Status::Ready
                  : CaptureSourceReplayCursorV3Status::Complete;
  return true;
}

bool CaptureSourceReplayCursorV3::bound(
    const CaptureSourceReplayPlanV3& plan) noexcept {
  if ((status_ != CaptureSourceReplayCursorV3Status::Ready &&
       status_ != CaptureSourceReplayCursorV3Status::Complete) ||
      !plan.valid() || plan.digest() != digest_ || plan.actions().size() != count_) {
    status_ = CaptureSourceReplayCursorV3Status::Rejected;
    return false;
  }
  return true;
}

bool CaptureSourceReplayCursorV3::peek(const CaptureSourceReplayPlanV3& plan,
    CaptureSourceReplayPlanV3Action& action) noexcept {
  if (!bound(plan) || next_ == count_) return false;
  action = plan.actions()[next_];
  return true;
}

bool CaptureSourceReplayCursorV3::acknowledge(
    const CaptureSourceReplayPlanV3& plan,
    const CaptureSourceReplayPlanV3Action& action) noexcept {
  if (!bound(plan) || next_ == count_ || plan.actions()[next_] != action) {
    status_ = CaptureSourceReplayCursorV3Status::Rejected;
    return false;
  }
  ++next_;
  if (next_ == count_) status_ = CaptureSourceReplayCursorV3Status::Complete;
  return true;
}

const char* captureSourceReplayPlanV3StatusName(
    CaptureSourceReplayPlanV3Status status) noexcept {
  switch (status) {
  case Status::Valid: return "valid";
  case Status::InvalidInput: return "invalid-input";
  case Status::InvalidIdentity: return "invalid-identity";
  case Status::BlobIdentityMismatch: return "blob-identity-mismatch";
  case Status::InvalidPublicationOrder: return "invalid-publication-order";
  case Status::CapacityExceeded: return "capacity-exceeded";
  case Status::AllocationFailed: return "allocation-failed";
  }
  return "unknown";
}

} // namespace dxmt9::d3d9
