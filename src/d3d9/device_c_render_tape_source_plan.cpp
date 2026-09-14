#include "device_c_render_tape_source_plan.hpp"

#include <algorithm>
#include <cstring>
#include <new>
#include <string_view>
#include <utility>

namespace dxmt9::d3d9 {
namespace {

constexpr std::uint32_t kCaptureSourceReplayPlanFormatVersion = 1u;
constexpr std::string_view kCaptureSourceReplayPlanDigestDomain =
    "dxmt9.capture-source-replay-plan";

bool loadCommandChunkHeader(std::span<const std::byte> payload,
                            RenderTapeCommandChunkHeader& out) noexcept {
  if (payload.size() < sizeof(out)) return false;
  std::memcpy(&out, payload.data(), sizeof(out));
  return true;
}

void appendBytes(std::vector<std::byte>& bytes,
                 std::span<const std::byte> append) {
  bytes.insert(bytes.end(), append.begin(), append.end());
}

void appendU32(std::vector<std::byte>& bytes, std::uint32_t value) {
  for (unsigned shift = 0u; shift != 32u; shift += 8u)
    bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffu));
}

void appendU64(std::vector<std::byte>& bytes, std::uint64_t value) {
  for (unsigned shift = 0u; shift != 64u; shift += 8u)
    bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffu));
}

RenderTapeDigest digestPlan(std::span<const std::byte> tape,
                            std::span<const std::byte> sidecar,
                            std::span<const CaptureSourceReplayRow> rows) {
  const auto tapeDigest = RenderTapeCaptureSession::sha256(tape);
  const auto sidecarDigest = RenderTapeCaptureSession::sha256(sidecar);

  std::vector<std::byte> canonical;
  const auto domain = std::as_bytes(std::span<const char>(
      kCaptureSourceReplayPlanDigestDomain.data(),
      kCaptureSourceReplayPlanDigestDomain.size()));
  canonical.reserve(domain.size() + 4u + tapeDigest.size() +
                    sidecarDigest.size() + 4u +
                    rows.size() * (sizeof(std::uint64_t) * 3u +
                                   sizeof(std::uint32_t) * 3u));
  appendBytes(canonical, domain);
  appendU32(canonical, kCaptureSourceReplayPlanFormatVersion);
  appendBytes(canonical, tapeDigest);
  appendBytes(canonical, sidecarDigest);
  appendU32(canonical, static_cast<std::uint32_t>(rows.size()));
  for (const auto& row : rows) {
    appendU64(canonical, row.eventOrdinal);
    appendU64(canonical, row.sourceOrdinal);
    appendU64(canonical, row.capturedSeqId);
    appendU32(canonical, row.commandChunkIndex);
    appendU32(canonical, row.firstRecord);
    appendU32(canonical, row.recordCount);
  }
  return RenderTapeCaptureSession::sha256(canonical);
}

bool sameCommandChunk(const RenderTapeCommandChunkHeader& header,
                      const CommandChunkEnvelope& envelope) noexcept {
  return header.wireVersion == envelope.version &&
         header.recordCount == envelope.recordCount &&
         header.handleCount == envelope.handleCount;
}

bool sameBytes(std::span<const std::byte> left,
               std::span<const std::byte> right) noexcept {
  return left.size() == right.size() &&
         std::equal(left.begin(), left.end(), right.begin());
}

}  // namespace

CaptureSourceReplayPlan::CaptureSourceReplayPlan(
    CaptureSourceReplayPlan&& other) noexcept
    : status_(other.status_), rows_(std::move(other.rows_)),
      rangesByCommandChunk_(std::move(other.rangesByCommandChunk_)),
      digest_(other.digest_) {
  other.status_ = CaptureSourceReplayPlanStatus::InvalidInput;
  other.digest_ = {};
}

CaptureSourceReplayPlan& CaptureSourceReplayPlan::operator=(
    CaptureSourceReplayPlan&& other) noexcept {
  if (this == &other) return *this;
  status_ = other.status_;
  rows_ = std::move(other.rows_);
  rangesByCommandChunk_ = std::move(other.rangesByCommandChunk_);
  digest_ = other.digest_;
  other.status_ = CaptureSourceReplayPlanStatus::InvalidInput;
  other.digest_ = {};
  return *this;
}

std::span<const ProviderReplayRecordRange>
CaptureSourceReplayPlan::rangesForCommandChunk(
    std::size_t commandChunkIndex) const noexcept {
  if (commandChunkIndex >= rangesByCommandChunk_.size()) return {};
  return rangesByCommandChunk_[commandChunkIndex];
}

CaptureSourceReplayPlanStatus buildCaptureSourceReplayPlan(
    std::span<const std::byte> tape,
    const render_tape_runtime::GeneralPreflightPlan& plan,
    std::span<const std::byte> identitySidecar,
    CaptureSourceReplayPlan& out) noexcept {
  try {
    if (tape.empty() || identitySidecar.empty() ||
        plan.commandChunks.empty() || plan.events.empty()) {
      return CaptureSourceReplayPlanStatus::InvalidInput;
    }

    RenderTapeIdentityView identity;
    const auto identityValidation = validateRenderTapeIdentity(
        tape, plan.catalogue, identitySidecar, &identity);
    if (!identityValidation.valid())
      return CaptureSourceReplayPlanStatus::InvalidIdentity;
    if (identity.header.authority != static_cast<std::uint32_t>(
                                         RenderTapeIdentityAuthority::Capture)) {
      return CaptureSourceReplayPlanStatus::WrongAuthority;
    }
    if (identity.sources.empty() || identity.settlements.empty()) {
      return CaptureSourceReplayPlanStatus::MissingSettlements;
    }

    // Re-import the authenticated input rather than borrowing event metadata
    // from the caller's plan.  This prevents a stale plan view from changing
    // the event ordinal used for provenance or digest construction.
    ImportedRenderTapeView authenticatedTape;
    if (!importPrevalidatedRenderTape(tape, authenticatedTape) ||
        authenticatedTape.events.size() != plan.tape.events.size() ||
        plan.events.size() != authenticatedTape.events.size()) {
      return CaptureSourceReplayPlanStatus::EventMappingMismatch;
    }

    CaptureSourceReplayPlan candidate;
    candidate.rangesByCommandChunk_.resize(plan.commandChunks.size());
    std::vector<bool> commandChunkMapped(plan.commandChunks.size(), false);
    std::vector<bool> eventMapped(authenticatedTape.events.size(), false);
    std::vector<std::uint64_t> commandEventOrdinals;
    commandEventOrdinals.reserve(plan.commandChunks.size());
    candidate.rows_.reserve(identity.sources.size());

    std::size_t nextSource = 0u;
    for (std::size_t planEventIndex = 0u;
         planEventIndex < plan.events.size(); ++planEventIndex) {
      const auto& replayEvent = plan.events[planEventIndex];
      if (replayEvent.eventIndex != planEventIndex ||
          replayEvent.eventIndex >= authenticatedTape.events.size()) {
        return CaptureSourceReplayPlanStatus::EventMappingMismatch;
      }
      const auto event = authenticatedTape.event(replayEvent.eventIndex);
      if (replayEvent.type !=
          static_cast<RenderTapeEventType>(event.header.type)) {
        return CaptureSourceReplayPlanStatus::EventMappingMismatch;
      }
      if (replayEvent.type != RenderTapeEventType::CommandChunk) continue;
      if (replayEvent.eventIndex >= authenticatedTape.events.size() ||
          replayEvent.index >= plan.commandChunks.size() ||
          eventMapped[replayEvent.eventIndex] ||
          commandChunkMapped[replayEvent.index]) {
        return CaptureSourceReplayPlanStatus::EventMappingMismatch;
      }

      if (static_cast<RenderTapeEventType>(event.header.type) !=
          RenderTapeEventType::CommandChunk) {
        return CaptureSourceReplayPlanStatus::EventMappingMismatch;
      }
      RenderTapeCommandChunkHeader header{};
      const auto& commandChunk = plan.commandChunks[replayEvent.index];
      if (commandChunk.envelope.recordCount > kProviderSourceReplayMaxRecords) {
        return CaptureSourceReplayPlanStatus::SourceCoverageMismatch;
      }
      if (!loadCommandChunkHeader(event.payload, header) ||
          !sameCommandChunk(header, commandChunk.envelope) ||
          event.payload.size() < sizeof(header) ||
          !sameBytes(event.payload.subspan(sizeof(header)), commandChunk.bytes)) {
        return CaptureSourceReplayPlanStatus::EventMappingMismatch;
      }

      eventMapped[replayEvent.eventIndex] = true;
      commandChunkMapped[replayEvent.index] = true;
      commandEventOrdinals.push_back(event.header.ordinal);

      if (nextSource < identity.sources.size() &&
          identity.sources[nextSource].eventOrdinal < event.header.ordinal) {
        return CaptureSourceReplayPlanStatus::SourceCoverageMismatch;
      }
      auto& ranges = candidate.rangesByCommandChunk_[replayEvent.index];
      const auto firstRow = candidate.rows_.size();
      while (nextSource < identity.sources.size() &&
             identity.sources[nextSource].eventOrdinal == event.header.ordinal) {
        const auto& source = identity.sources[nextSource++];
        if (source.recordCount == 0u || source.sourceOrdinal == 0u ||
            source.seqId == 0u ||
            static_cast<std::uint64_t>(source.firstRecord) +
                    source.recordCount > commandChunk.envelope.recordCount) {
          return CaptureSourceReplayPlanStatus::SourceCoverageMismatch;
        }
        ranges.push_back({source.firstRecord, source.recordCount});
        candidate.rows_.push_back({
            .eventOrdinal = source.eventOrdinal,
            .sourceOrdinal = source.sourceOrdinal,
            .capturedSeqId = source.seqId,
            .commandChunkIndex = replayEvent.index,
            .firstRecord = source.firstRecord,
            .recordCount = source.recordCount,
        });
      }
      if (firstRow == candidate.rows_.size() ||
          !validateProviderReplayRecordRanges(
              commandChunk.envelope.recordCount, ranges)) {
        return CaptureSourceReplayPlanStatus::SourceCoverageMismatch;
      }
    }

    if (commandEventOrdinals.size() != plan.commandChunks.size() ||
        nextSource != identity.sources.size() ||
        std::any_of(commandChunkMapped.begin(), commandChunkMapped.end(),
                    [](bool mapped) { return !mapped; })) {
      return CaptureSourceReplayPlanStatus::EventMappingMismatch;
    }

    // The settlement rows are completion evidence for exactly the command
    // event set admitted above.  The identity validator proves source
    // coverage; this check prevents a valid but unrelated settlement table
    // from being paired with the replay plan.
    if (identity.settlements.size() != commandEventOrdinals.size()) {
      return CaptureSourceReplayPlanStatus::SourceCoverageMismatch;
    }
    for (std::size_t index = 0u; index < commandEventOrdinals.size(); ++index) {
      if (identity.settlements[index].eventOrdinal !=
          commandEventOrdinals[index]) {
        return CaptureSourceReplayPlanStatus::SourceCoverageMismatch;
      }
    }

    candidate.digest_ = digestPlan(tape, identitySidecar, candidate.rows_);
    candidate.status_ = CaptureSourceReplayPlanStatus::Valid;
    out = std::move(candidate);
    return CaptureSourceReplayPlanStatus::Valid;
  } catch (const std::bad_alloc&) {
    return CaptureSourceReplayPlanStatus::AllocationFailed;
  } catch (...) {
    return CaptureSourceReplayPlanStatus::InvalidInput;
  }
}

const char* captureSourceReplayPlanStatusName(
    CaptureSourceReplayPlanStatus status) noexcept {
  switch (status) {
  case CaptureSourceReplayPlanStatus::Valid: return "valid";
  case CaptureSourceReplayPlanStatus::InvalidInput: return "invalid-input";
  case CaptureSourceReplayPlanStatus::InvalidIdentity: return "invalid-identity";
  case CaptureSourceReplayPlanStatus::WrongAuthority: return "wrong-authority";
  case CaptureSourceReplayPlanStatus::MissingSettlements:
    return "missing-settlements";
  case CaptureSourceReplayPlanStatus::EventMappingMismatch:
    return "event-mapping-mismatch";
  case CaptureSourceReplayPlanStatus::SourceCoverageMismatch:
    return "source-coverage-mismatch";
  case CaptureSourceReplayPlanStatus::AllocationFailed:
    return "allocation-failed";
  }
  return "unknown";
}

}  // namespace dxmt9::d3d9
