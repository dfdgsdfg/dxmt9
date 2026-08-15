#include "device_c_render_tape_identity.hpp"

#include "device_c_render_tape_capture.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace dxmt9::d3d9 {
namespace {

template <typename T>
bool load(std::span<const std::byte> bytes, std::size_t offset,
          T& value) noexcept {
  if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) return false;
  std::memcpy(&value, bytes.data() + offset, sizeof(T));
  return true;
}

bool sameDigest(const RenderTapeDigest& left,
                const RenderTapeDigest& right) noexcept {
  return std::equal(left.begin(), left.end(), right.begin());
}

RenderTapeIdentityValidationResult failure(
    RenderTapeIdentityStatus status, std::uint32_t source = 0xffffffffu,
    std::uint32_t range = 0xffffffffu) noexcept {
  return {.status = status, .failedSource = source, .failedRange = range};
}

} // namespace

RenderTapeIdentityValidationResult validateRenderTapeIdentity(
    std::span<const std::byte> tape,
    const RenderTapeBlobCatalogue& verifiedCatalogue,
    std::span<const std::byte> sidecar,
    RenderTapeIdentityView* out) noexcept {
  if (out) *out = {};
  ImportedRenderTapeView imported;
  auto result = RenderTapeIdentityValidationResult{};
  result.tapeValidation = validateRenderTape(tape, verifiedCatalogue, &imported);
  if (!result.tapeValidation.valid()) {
    result.status = RenderTapeIdentityStatus::InvalidTape;
    return result;
  }

  RenderTapeIdentityHeader header{};
  if (!load(sidecar, 0u, header)) return failure(RenderTapeIdentityStatus::MissingHeader);
  if (header.magic != kRenderTapeIdentityMagic ||
      header.version != kRenderTapeIdentityVersion ||
      header.headerSize != sizeof(header) ||
      header.sourceEntrySize != sizeof(RenderTapeIdentitySource) ||
      header.rangeEntrySize != sizeof(RenderTapeIdentityRange) ||
      header.reserved0 != 0u ||
      (header.authority != static_cast<std::uint32_t>(
                               RenderTapeIdentityAuthority::Capture) &&
       header.authority != static_cast<std::uint32_t>(
                               RenderTapeIdentityAuthority::ProviderReplay))) {
    return failure(RenderTapeIdentityStatus::InvalidHeader);
  }
  const std::uint64_t sourceBytes =
      static_cast<std::uint64_t>(header.sourceCount) * sizeof(RenderTapeIdentitySource);
  const std::uint64_t rangeBytes =
      static_cast<std::uint64_t>(header.rangeCount) * sizeof(RenderTapeIdentityRange);
  const std::uint64_t expectedRangeOffset = sizeof(header) + sourceBytes;
  const std::uint64_t expectedBytes = expectedRangeOffset + rangeBytes;
  if (header.sourceTableOffset != sizeof(header) ||
      header.rangeTableOffset != expectedRangeOffset ||
      expectedRangeOffset > std::numeric_limits<std::uint32_t>::max() ||
      expectedBytes != sidecar.size()) {
    return failure(RenderTapeIdentityStatus::InvalidLayout);
  }
  if (header.eventsBytes != tape.size() ||
      !sameDigest(header.eventsDigest, RenderTapeCaptureSession::sha256(tape))) {
    return failure(RenderTapeIdentityStatus::EventsMismatch);
  }
  if (header.frameId == 0u || header.presentOrdinal == 0u ||
      header.captureToken == 0u) {
    return failure(RenderTapeIdentityStatus::InvalidFrameIdentity);
  }

  std::vector<RenderTapeIdentitySource> sources;
  std::vector<RenderTapeIdentityRange> ranges;
  try {
    sources.resize(header.sourceCount);
    ranges.resize(header.rangeCount);
    if (!sources.empty()) {
      std::memcpy(sources.data(), sidecar.data() + header.sourceTableOffset,
                  sources.size() * sizeof(sources[0]));
    }
    if (!ranges.empty()) {
      std::memcpy(ranges.data(), sidecar.data() + header.rangeTableOffset,
                  ranges.size() * sizeof(ranges[0]));
    }
  } catch (...) {
    return failure(RenderTapeIdentityStatus::AllocationFailed);
  }

  std::vector<std::pair<std::uint64_t, std::pair<std::uint32_t, std::uint32_t>>>
      passes;
  try {
    passes.reserve(ranges.size());
  } catch (...) {
    return failure(RenderTapeIdentityStatus::AllocationFailed);
  }

  std::uint32_t sourceIndex = 0u;
  std::uint32_t expectedFirstRange = 0u;
  std::uint64_t priorSourceOrdinal = 0u;
  std::uint64_t priorSeqId = 0u;
  for (std::uint32_t eventIndex = 0u; eventIndex < imported.events.size();
       ++eventIndex) {
    const auto event = imported.event(eventIndex);
    if (static_cast<RenderTapeEventType>(event.header.type) ==
        RenderTapeEventType::PresentComplete) {
      RenderTapePresentCompleteHeader completion{};
      if (!load(event.payload, 0u, completion) ||
          completion.presentOrdinal != header.presentOrdinal) {
        return failure(RenderTapeIdentityStatus::InvalidFrameIdentity);
      }
    }
    if (static_cast<RenderTapeEventType>(event.header.type) !=
        RenderTapeEventType::CommandChunk) {
      continue;
    }
    if (sourceIndex >= sources.size()) {
      return failure(RenderTapeIdentityStatus::SourceCoverageMismatch, sourceIndex);
    }
    RenderTapeCommandChunkHeader fixed{};
    if (!load(event.payload, 0u, fixed)) {
      return failure(RenderTapeIdentityStatus::InvalidTape, sourceIndex);
    }
    const auto& source = sources[sourceIndex];
    if (source.eventOrdinal != event.header.ordinal ||
        source.recordCount != fixed.recordCount ||
        source.captureToken != header.captureToken ||
        source.reserved0 != 0u ||
        source.firstRange != expectedFirstRange || source.rangeCount == 0u ||
        source.firstRange > ranges.size() ||
        source.rangeCount > ranges.size() - source.firstRange) {
      return failure(RenderTapeIdentityStatus::SourceCoverageMismatch, sourceIndex);
    }
    if (source.sourceOrdinal <= priorSourceOrdinal || source.seqId <= priorSeqId) {
      return failure(RenderTapeIdentityStatus::NonMonotoneSource, sourceIndex);
    }
    priorSourceOrdinal = source.sourceOrdinal;
    priorSeqId = source.seqId;
    std::uint32_t expectedRecord = 0u;
    for (std::uint32_t local = 0u; local < source.rangeCount; ++local) {
      const auto rangeIndex = source.firstRange + local;
      const auto& range = ranges[rangeIndex];
      if (range.eventOrdinal != source.eventOrdinal ||
          range.sourceOrdinal != source.sourceOrdinal ||
          range.seqId != source.seqId || range.logicalPassId == 0u ||
          range.recordCount == 0u || range.firstRecord != expectedRecord ||
          range.passKind == 0u ||
          range.recordCount > source.recordCount - expectedRecord) {
        return failure(RenderTapeIdentityStatus::InvalidRange, sourceIndex,
                       rangeIndex);
      }
      expectedRecord += range.recordCount;
      const auto found = std::find_if(passes.begin(), passes.end(),
          [&](const auto& pass) { return pass.first == range.logicalPassId; });
      const auto membership = std::pair(range.dagPassIndex, range.passKind);
      if (found == passes.end()) {
        passes.push_back({range.logicalPassId, membership});
      } else if (found->second != membership) {
        return failure(RenderTapeIdentityStatus::PassIdentityMismatch,
                       sourceIndex, rangeIndex);
      }
    }
    if (expectedRecord != source.recordCount) {
      return failure(RenderTapeIdentityStatus::RecordCoverageMismatch, sourceIndex);
    }
    expectedFirstRange += source.rangeCount;
    ++sourceIndex;
  }
  if (sourceIndex != sources.size() || expectedFirstRange != ranges.size()) {
    return failure(RenderTapeIdentityStatus::SourceCoverageMismatch, sourceIndex);
  }
  result.status = RenderTapeIdentityStatus::Valid;
  if (out) {
    out->header = header;
    out->sources = std::move(sources);
    out->ranges = std::move(ranges);
  }
  return result;
}

std::vector<std::byte> buildRenderTapeIdentity(
    std::span<const std::byte> tape,
    const RenderTapeBlobCatalogue& verifiedCatalogue,
    std::uint64_t frameId, std::uint64_t presentOrdinal,
    std::uint64_t captureToken, RenderTapeIdentityAuthority authority,
    std::span<const RenderTapeIdentitySource> sources,
    std::span<const RenderTapeIdentityRange> ranges,
    RenderTapeIdentityValidationResult* validationResult) {
  if (validationResult) *validationResult = {};
  if (sources.size() > std::numeric_limits<std::uint32_t>::max() ||
      ranges.size() > std::numeric_limits<std::uint32_t>::max() ||
      sources.size() >
          (std::numeric_limits<std::uint32_t>::max() -
           sizeof(RenderTapeIdentityHeader)) /
              sizeof(RenderTapeIdentitySource) ||
      ranges.size() >
          (std::numeric_limits<std::uint32_t>::max() -
           sizeof(RenderTapeIdentityHeader) -
           sources.size() * sizeof(RenderTapeIdentitySource)) /
              sizeof(RenderTapeIdentityRange)) {
    return {};
  }
  RenderTapeIdentityHeader header{
      .eventsBytes = tape.size(),
      .frameId = frameId,
      .presentOrdinal = presentOrdinal,
      .captureToken = captureToken,
      .eventsDigest = RenderTapeCaptureSession::sha256(tape),
      .sourceEntrySize = sizeof(RenderTapeIdentitySource),
      .rangeEntrySize = sizeof(RenderTapeIdentityRange),
      .sourceTableOffset = sizeof(RenderTapeIdentityHeader),
      .sourceCount = static_cast<std::uint32_t>(sources.size()),
      .rangeTableOffset = static_cast<std::uint32_t>(
          sizeof(RenderTapeIdentityHeader) +
          sources.size() * sizeof(RenderTapeIdentitySource)),
      .rangeCount = static_cast<std::uint32_t>(ranges.size()),
      .authority = static_cast<std::uint32_t>(authority),
  };
  std::vector<std::byte> bytes(
      header.rangeTableOffset + ranges.size() * sizeof(RenderTapeIdentityRange));
  std::memcpy(bytes.data(), &header, sizeof(header));
  if (!sources.empty()) {
    std::memcpy(bytes.data() + header.sourceTableOffset, sources.data(),
                sources.size_bytes());
  }
  if (!ranges.empty()) {
    std::memcpy(bytes.data() + header.rangeTableOffset, ranges.data(),
                ranges.size_bytes());
  }
  const auto validation = validateRenderTapeIdentity(
      tape, verifiedCatalogue, bytes);
  if (validationResult) *validationResult = validation;
  if (!validation.valid()) return {};
  return bytes;
}

bool renderTapeIdentityOwnsSelection(
    const RenderTapeIdentityView& identity, std::uint64_t eventOrdinal,
    std::uint32_t firstRecord, std::uint32_t recordCount,
    std::uint64_t* logicalPassId) noexcept {
  if (recordCount == 0u) return false;
  const auto end = static_cast<std::uint64_t>(firstRecord) + recordCount;
  for (const auto& source : identity.sources) {
    if (source.eventOrdinal != eventOrdinal || end > source.recordCount) continue;
    for (std::uint32_t local = 0u; local < source.rangeCount; ++local) {
      const auto& range = identity.ranges[source.firstRange + local];
      const auto rangeEnd = static_cast<std::uint64_t>(range.firstRecord) +
                            range.recordCount;
      if (firstRecord >= range.firstRecord && end <= rangeEnd) {
        if (logicalPassId) *logicalPassId = range.logicalPassId;
        return true;
      }
    }
    return false;
  }
  return false;
}

const char* renderTapeIdentityStatusName(
    RenderTapeIdentityStatus status) noexcept {
  switch (status) {
  case RenderTapeIdentityStatus::Valid: return "valid";
  case RenderTapeIdentityStatus::InvalidTape: return "invalid-tape";
  case RenderTapeIdentityStatus::MissingHeader: return "missing-header";
  case RenderTapeIdentityStatus::InvalidHeader: return "invalid-header";
  case RenderTapeIdentityStatus::InvalidLayout: return "invalid-layout";
  case RenderTapeIdentityStatus::EventsMismatch: return "events-mismatch";
  case RenderTapeIdentityStatus::InvalidFrameIdentity:
    return "invalid-frame-identity";
  case RenderTapeIdentityStatus::SourceCoverageMismatch:
    return "source-coverage-mismatch";
  case RenderTapeIdentityStatus::NonMonotoneSource:
    return "non-monotone-source";
  case RenderTapeIdentityStatus::InvalidRange: return "invalid-range";
  case RenderTapeIdentityStatus::RecordCoverageMismatch:
    return "record-coverage-mismatch";
  case RenderTapeIdentityStatus::PassIdentityMismatch:
    return "pass-identity-mismatch";
  case RenderTapeIdentityStatus::AllocationFailed: return "allocation-failed";
  }
  return "unknown";
}

bool RenderTapeProductionIdentityLedger::append(
    std::uint64_t captureToken, std::uint64_t eventOrdinal,
    std::uint64_t sourceOrdinal, std::uint64_t seqId,
    std::uint32_t recordCount,
    std::span<const RenderTapeProductionPassRange> ranges) noexcept {
  if (captureToken == 0u || eventOrdinal == 0u || sourceOrdinal == 0u ||
      seqId == 0u || recordCount == 0u || ranges.empty()) {
    fail(captureToken);
    return false;
  }
  std::lock_guard lock(mutex_);
  if (captureToken_ != captureToken) {
    captureToken_ = captureToken;
    nextLogicalPassId_ = 1u;
    failed_ = false;
    sources_.clear();
    ranges_.clear();
  }
  if (failed_ || (!sources_.empty() &&
      (eventOrdinal <= sources_.back().eventOrdinal ||
       sourceOrdinal <= sources_.back().sourceOrdinal ||
       seqId <= sources_.back().seqId)) ||
      ranges_.size() > std::numeric_limits<std::uint32_t>::max() ||
      ranges.size() > std::numeric_limits<std::uint32_t>::max() -
                          ranges_.size()) {
    failed_ = true;
    return false;
  }
  std::uint32_t nextRecord = 0u;
  for (const auto& range : ranges) {
    if (range.firstRecord != nextRecord || range.recordCount == 0u ||
        range.passKind == 0u ||
        range.recordCount > recordCount - nextRecord) {
      failed_ = true;
      return false;
    }
    nextRecord += range.recordCount;
  }
  if (nextRecord != recordCount ||
      nextLogicalPassId_ >
          std::numeric_limits<std::uint64_t>::max() - ranges.size()) {
    failed_ = true;
    return false;
  }
  try {
    sources_.reserve(sources_.size() + 1u);
    ranges_.reserve(ranges_.size() + ranges.size());
    const auto firstRange = static_cast<std::uint32_t>(ranges_.size());
    for (const auto& range : ranges) {
      ranges_.push_back(D9CRenderTapeIdentityRangeEntry{
          .eventOrdinal = eventOrdinal,
          .sourceOrdinal = sourceOrdinal,
          .seqId = seqId,
          .logicalPassId = nextLogicalPassId_++,
          .firstRecord = range.firstRecord,
          .recordCount = range.recordCount,
          .dagPassIndex = range.dagPassIndex,
          .passKind = range.passKind,
      });
    }
    sources_.push_back(D9CRenderTapeIdentitySourceEntry{
        .eventOrdinal = eventOrdinal,
        .sourceOrdinal = sourceOrdinal,
        .seqId = seqId,
        .captureToken = captureToken,
        .recordCount = recordCount,
        .firstRange = firstRange,
        .rangeCount = static_cast<std::uint32_t>(ranges.size()),
        .reserved0 = 0u,
    });
  } catch (...) {
    failed_ = true;
    sources_.clear();
    ranges_.clear();
    return false;
  }
  return true;
}

void RenderTapeProductionIdentityLedger::fail(
    std::uint64_t captureToken) noexcept {
  std::lock_guard lock(mutex_);
  if (captureToken_ != captureToken) {
    captureToken_ = captureToken;
    nextLogicalPassId_ = 1u;
    sources_.clear();
    ranges_.clear();
  }
  failed_ = true;
}

bool RenderTapeProductionIdentityLedger::copy(
    std::uint64_t captureToken,
    D9CRenderTapeIdentityCaptureResult& out,
    std::span<std::byte> bytes) const noexcept {
  out = {};
  out.status = D9C_RENDER_TAPE_IDENTITY_CAPTURE_FAILED;
  std::lock_guard lock(mutex_);
  if (captureToken == 0u || captureToken_ != captureToken || failed_ ||
      sources_.empty() || ranges_.empty() ||
      sources_.size() > std::numeric_limits<std::uint32_t>::max() ||
      ranges_.size() > std::numeric_limits<std::uint32_t>::max()) {
    return false;
  }
  const std::uint64_t sourceBytes =
      sources_.size() * sizeof(D9CRenderTapeIdentitySourceEntry);
  const std::uint64_t rangeBytes =
      ranges_.size() * sizeof(D9CRenderTapeIdentityRangeEntry);
  if (sourceBytes > std::numeric_limits<std::uint64_t>::max() - rangeBytes) {
    return false;
  }
  const std::uint64_t byteCount = sourceBytes + rangeBytes;
  out.status = D9C_RENDER_TAPE_IDENTITY_CAPTURE_COMPLETE;
  out.sourceCount = static_cast<std::uint32_t>(sources_.size());
  out.rangeCount = static_cast<std::uint32_t>(ranges_.size());
  out.captureToken = captureToken;
  out.byteCount = byteCount;
  if (bytes.empty()) {
    return true;
  }
  if (bytes.size() != byteCount) {
    out.status = D9C_RENDER_TAPE_IDENTITY_CAPTURE_FAILED;
    return false;
  }
  std::memcpy(bytes.data(), sources_.data(), sourceBytes);
  std::memcpy(bytes.data() + sourceBytes, ranges_.data(), rangeBytes);
  return true;
}

} // namespace dxmt9::d3d9
