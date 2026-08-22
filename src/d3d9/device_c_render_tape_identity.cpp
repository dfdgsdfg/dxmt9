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

bool validateRenderTapeIdentitySettlements(
    std::span<const RenderTapeIdentitySource> sources,
    std::span<const RenderTapeIdentitySettlement> settlements) noexcept {
  if (settlements.empty()) return true;
  try {
    std::vector<bool> covered(sources.size(), false);
    std::uint64_t priorEvent = 0u;
    for (const auto& settlement : settlements) {
      if (settlement.eventOrdinal == 0u ||
          settlement.eventOrdinal <= priorEvent ||
          settlement.rawOrdinal == 0u || settlement.buildGeneration == 0u ||
          settlement.firstSourceOrdinal == 0u || settlement.tailSeqId == 0u ||
          settlement.sourceCount == 0u || settlement.reserved0 != 0u) {
        return false;
      }
      priorEvent = settlement.eventOrdinal;

      std::size_t first = sources.size();
      for (std::size_t index = 0; index < sources.size(); ++index) {
        if (sources[index].eventOrdinal == settlement.eventOrdinal &&
            sources[index].sourceOrdinal == settlement.firstSourceOrdinal) {
          first = index;
          break;
        }
      }
      if (first == sources.size() ||
          settlement.sourceCount > sources.size() - first) {
        return false;
      }
      const std::size_t count = settlement.sourceCount;
      for (std::size_t offset = 0; offset < count; ++offset) {
        const std::size_t index = first + offset;
        const auto& source = sources[index];
        if (covered[index] || source.eventOrdinal != settlement.eventOrdinal ||
            source.sourceOrdinal == 0u || source.seqId == 0u) {
          return false;
        }
        if (offset != 0u &&
            (source.sourceOrdinal <= sources[index - 1u].sourceOrdinal ||
             source.seqId <= sources[index - 1u].seqId)) {
          return false;
        }
        covered[index] = true;
        if (offset + 1u == count && source.seqId != settlement.tailSeqId) {
          return false;
        }
      }
    }
    return std::all_of(covered.begin(), covered.end(),
                       [](bool value) { return value; });
  } catch (...) {
    return false;
  }
}

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
      header.settlementEntrySize != sizeof(RenderTapeIdentitySettlement) ||
      header.reserved1 != 0u ||
      header.settlementCount > kMaxRenderTapeIdentitySettlements ||
      (header.authority != static_cast<std::uint32_t>(
                               RenderTapeIdentityAuthority::Capture) &&
       header.authority != static_cast<std::uint32_t>(
                               RenderTapeIdentityAuthority::ProviderReplay) &&
       header.authority != static_cast<std::uint32_t>(
                               RenderTapeIdentityAuthority::DerivedProjection))) {
    return failure(RenderTapeIdentityStatus::InvalidHeader);
  }
  const std::uint64_t sourceBytes =
      static_cast<std::uint64_t>(header.sourceCount) * sizeof(RenderTapeIdentitySource);
  const std::uint64_t rangeBytes =
      static_cast<std::uint64_t>(header.rangeCount) * sizeof(RenderTapeIdentityRange);
  const std::uint64_t settlementBytes = static_cast<std::uint64_t>(
      header.settlementCount) * sizeof(RenderTapeIdentitySettlement);
  const std::uint64_t expectedRangeOffset = sizeof(header) + sourceBytes;
  const std::uint64_t expectedSettlementOffset = expectedRangeOffset + rangeBytes;
  const std::uint64_t expectedBytes = expectedSettlementOffset + settlementBytes;
  if (header.sourceTableOffset != sizeof(header) ||
      header.rangeTableOffset != expectedRangeOffset ||
      header.settlementTableOffset != expectedSettlementOffset ||
      expectedRangeOffset > std::numeric_limits<std::uint32_t>::max() ||
      expectedSettlementOffset > std::numeric_limits<std::uint32_t>::max() ||
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
  std::vector<RenderTapeIdentitySettlement> settlements;
  try {
    sources.resize(header.sourceCount);
    ranges.resize(header.rangeCount);
    settlements.resize(header.settlementCount);
    if (!sources.empty()) {
      std::memcpy(sources.data(), sidecar.data() + header.sourceTableOffset,
                  sources.size() * sizeof(sources[0]));
    }
    if (!ranges.empty()) {
      std::memcpy(ranges.data(), sidecar.data() + header.rangeTableOffset,
                  ranges.size() * sizeof(ranges[0]));
    }
    if (!settlements.empty()) {
      std::memcpy(settlements.data(),
                  sidecar.data() + header.settlementTableOffset,
                  settlements.size() * sizeof(settlements[0]));
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
    RenderTapeCommandChunkHeader fixed{};
    if (!load(event.payload, 0u, fixed)) {
      return failure(RenderTapeIdentityStatus::InvalidTape, sourceIndex);
    }
    std::uint32_t expectedEventRecord = 0u;
    bool foundEventSource = false;
    while (sourceIndex < sources.size() &&
           sources[sourceIndex].eventOrdinal == event.header.ordinal) {
      foundEventSource = true;
      const auto& source = sources[sourceIndex];
      if (source.recordCount == 0u ||
          source.recordCount > std::numeric_limits<std::uint32_t>::max() -
                                   source.firstRecord ||
          expectedEventRecord > fixed.recordCount ||
          source.recordCount > fixed.recordCount - expectedEventRecord ||
          source.firstRecord != expectedEventRecord ||
          source.captureToken != header.captureToken || source.rangeCount == 0u ||
          source.firstRange != expectedFirstRange ||
          source.firstRange > ranges.size() ||
          source.rangeCount > ranges.size() - source.firstRange) {
        return failure(RenderTapeIdentityStatus::SourceCoverageMismatch,
                       sourceIndex);
      }
      if (source.sourceOrdinal <= priorSourceOrdinal ||
          source.seqId <= priorSeqId) {
        return failure(RenderTapeIdentityStatus::NonMonotoneSource,
                       sourceIndex);
      }
      priorSourceOrdinal = source.sourceOrdinal;
      priorSeqId = source.seqId;
      const auto sourceEnd = source.firstRecord + source.recordCount;
      std::uint32_t expectedRecord = source.firstRecord;
      for (std::uint32_t local = 0u; local < source.rangeCount; ++local) {
        const auto rangeIndex = source.firstRange + local;
        const auto& range = ranges[rangeIndex];
        if (range.eventOrdinal != source.eventOrdinal ||
            range.sourceOrdinal != source.sourceOrdinal ||
            range.seqId != source.seqId || range.logicalPassId == 0u ||
            range.recordCount == 0u || range.firstRecord != expectedRecord ||
            range.passKind == 0u || range.firstRecord < source.firstRecord ||
            range.firstRecord > sourceEnd ||
            range.recordCount > sourceEnd - range.firstRecord) {
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
      if (expectedRecord != sourceEnd) {
        return failure(RenderTapeIdentityStatus::RecordCoverageMismatch,
                       sourceIndex);
      }
      expectedEventRecord += source.recordCount;
      expectedFirstRange += source.rangeCount;
      ++sourceIndex;
      if (expectedEventRecord == fixed.recordCount) break;
    }
    if (!foundEventSource || expectedEventRecord != fixed.recordCount) {
      return failure(RenderTapeIdentityStatus::SourceCoverageMismatch,
                     sourceIndex);
    }
  }
  if (sourceIndex != sources.size() || expectedFirstRange != ranges.size()) {
    return failure(RenderTapeIdentityStatus::SourceCoverageMismatch, sourceIndex);
  }
  if (!validateRenderTapeIdentitySettlements(sources, settlements)) {
    return failure(RenderTapeIdentityStatus::InvalidFrameIdentity);
  }
  result.status = RenderTapeIdentityStatus::Valid;
  if (out) {
    out->header = header;
    out->sources = std::move(sources);
    out->ranges = std::move(ranges);
    out->settlements = std::move(settlements);
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
    RenderTapeIdentityValidationResult* validationResult,
    std::span<const RenderTapeIdentitySettlement> settlements) {
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
              sizeof(RenderTapeIdentityRange) ||
      settlements.size() > std::numeric_limits<std::uint32_t>::max()) {
    return {};
  }
  if (settlements.size() > kMaxRenderTapeIdentitySettlements) return {};
  const auto sourceBytes = sources.size() * sizeof(RenderTapeIdentitySource);
  const auto rangeBytes = ranges.size() * sizeof(RenderTapeIdentityRange);
  const auto settlementOffset = sizeof(RenderTapeIdentityHeader) + sourceBytes +
                                rangeBytes;
  if (settlementOffset > std::numeric_limits<std::uint32_t>::max() ||
      settlements.size() >
          (std::numeric_limits<std::uint32_t>::max() - settlementOffset) /
              sizeof(RenderTapeIdentitySettlement)) {
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
      .settlementEntrySize = sizeof(RenderTapeIdentitySettlement),
      .settlementTableOffset = static_cast<std::uint32_t>(settlementOffset),
      .settlementCount = static_cast<std::uint32_t>(settlements.size()),
  };
  std::vector<std::byte> bytes(
      header.settlementTableOffset +
      settlements.size() * sizeof(RenderTapeIdentitySettlement));
  std::memcpy(bytes.data(), &header, sizeof(header));
  if (!sources.empty()) {
    std::memcpy(bytes.data() + header.sourceTableOffset, sources.data(),
                sources.size_bytes());
  }
  if (!ranges.empty()) {
    std::memcpy(bytes.data() + header.rangeTableOffset, ranges.data(),
                ranges.size_bytes());
  }
  if (!settlements.empty()) {
    std::memcpy(bytes.data() + header.settlementTableOffset, settlements.data(),
                settlements.size_bytes());
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
  bool found = false;
  std::uint64_t selectedPass = 0u;
  std::uint32_t selectedDagPass = 0u;
  std::uint32_t selectedPassKind = 0u;
  std::uint32_t cursor = firstRecord;
  for (const auto& source : identity.sources) {
    const auto sourceEnd = static_cast<std::uint64_t>(source.firstRecord) +
                           source.recordCount;
    if (source.eventOrdinal != eventOrdinal ||
        end <= source.firstRecord || firstRecord >= sourceEnd) {
      continue;
    }
    const auto pieceBegin = std::max<std::uint32_t>(cursor, source.firstRecord);
    const auto pieceEnd = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(end, sourceEnd));
    if (pieceBegin != cursor) {
      return false;
    }
    std::uint32_t pieceCursor = pieceBegin;
    for (std::uint32_t local = 0u; local < source.rangeCount; ++local) {
      const auto& range = identity.ranges[source.firstRange + local];
      const auto rangeEnd = static_cast<std::uint64_t>(range.firstRecord) +
                            range.recordCount;
      if (rangeEnd <= pieceCursor) continue;
      if (range.firstRecord > pieceCursor || range.firstRecord >= pieceEnd) {
        break;
      }
      if (!found) {
        selectedPass = range.logicalPassId;
        selectedDagPass = range.dagPassIndex;
        selectedPassKind = range.passKind;
        found = true;
      } else if (range.logicalPassId != selectedPass ||
                 range.dagPassIndex != selectedDagPass ||
                 range.passKind != selectedPassKind) {
        return false;
      }
      pieceCursor = static_cast<std::uint32_t>(
          std::min<std::uint64_t>(rangeEnd, pieceEnd));
      if (pieceCursor == pieceEnd) break;
    }
    if (pieceCursor != pieceEnd) {
      return false;
    }
    cursor = pieceEnd;
    if (cursor == end) {
      if (logicalPassId) *logicalPassId = selectedPass;
      return found;
    }
    if (sourceEnd < end) {
      continue;
    }
    return false;
  }
  return false;
}

bool renderTapeIdentityLocateRecord(
    const RenderTapeIdentityView& identity, std::uint64_t eventOrdinal,
    std::uint32_t recordIndex, RenderTapeIdentitySource* sourceOut,
    RenderTapeIdentityRange* rangeOut) noexcept {
  for (const auto& source : identity.sources) {
    if (source.eventOrdinal != eventOrdinal ||
        recordIndex < source.firstRecord ||
        recordIndex - source.firstRecord >= source.recordCount) {
      continue;
    }
    for (std::uint32_t local = 0u; local < source.rangeCount; ++local) {
      const auto& range = identity.ranges[source.firstRange + local];
      if (recordIndex < range.firstRecord ||
          recordIndex - range.firstRecord >= range.recordCount) {
        continue;
      }
      if (sourceOut) *sourceOut = source;
      if (rangeOut) *rangeOut = range;
      return true;
    }
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
  return appendImpl(captureToken, eventOrdinal, sourceOrdinal, seqId,
                    0u, recordCount, ranges, true);
}

bool RenderTapeProductionIdentityLedger::append(
    std::uint64_t captureToken, std::uint64_t eventOrdinal,
    std::uint64_t sourceOrdinal, std::uint64_t seqId,
    std::uint32_t firstRecord, std::uint32_t recordCount,
    std::span<const RenderTapeProductionPassRange> ranges) noexcept {
  return appendImpl(captureToken, eventOrdinal, sourceOrdinal, seqId,
                    firstRecord, recordCount, ranges, false);
}

bool RenderTapeProductionIdentityLedger::appendImpl(
    std::uint64_t captureToken, std::uint64_t eventOrdinal,
    std::uint64_t sourceOrdinal, std::uint64_t seqId,
    std::uint32_t firstRecord, std::uint32_t recordCount,
    std::span<const RenderTapeProductionPassRange> ranges,
    bool rangesAreSourceLocal) noexcept {
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
    settled_ = false;
    settledEventOrdinal_ = 0u;
    settledSourceOrdinal_ = 0u;
    settledSeqId_ = 0u;
    settlementCount_ = 0u;
    settlementCompleted_.fill(false);
    completedSettlementCount_ = 0u;
    sources_.clear();
    ranges_.clear();
  }
  if (failed_ || (!sources_.empty() &&
      (eventOrdinal < sources_.back().eventOrdinal ||
       sourceOrdinal <= sources_.back().sourceOrdinal ||
       seqId <= sources_.back().seqId)) ||
      ranges_.size() > std::numeric_limits<std::uint32_t>::max() ||
      ranges.size() > std::numeric_limits<std::uint32_t>::max() -
                          ranges_.size()) {
    failed_ = true;
    return false;
  }
  if (!sources_.empty() && eventOrdinal == sources_.back().eventOrdinal) {
    const auto& prior = sources_.back();
    if (prior.firstRecord > std::numeric_limits<std::uint32_t>::max() -
                                prior.recordCount) {
      failed_ = true;
      return false;
    }
    const auto expected = prior.firstRecord + prior.recordCount;
    if (rangesAreSourceLocal) firstRecord = expected;
    if (firstRecord != expected) {
      failed_ = true;
      return false;
    }
  } else if (!sources_.empty() && eventOrdinal <= sources_.back().eventOrdinal) {
    failed_ = true;
    return false;
  } else if (rangesAreSourceLocal) {
    firstRecord = 0u;
  } else if (firstRecord != 0u) {
    failed_ = true;
    return false;
  }
  if (recordCount > std::numeric_limits<std::uint32_t>::max() - firstRecord) {
    failed_ = true;
    return false;
  }
  const auto sourceEnd = firstRecord + recordCount;
  std::uint32_t nextRecord = firstRecord;
  for (const auto& range : ranges) {
    if (range.recordCount == 0u || range.passKind == 0u ||
        range.firstRecord > std::numeric_limits<std::uint32_t>::max() -
                                 (rangesAreSourceLocal ? firstRecord : 0u)) {
      failed_ = true;
      return false;
    }
    const auto eventFirstRecord = range.firstRecord +
        (rangesAreSourceLocal ? firstRecord : 0u);
    if (eventFirstRecord != nextRecord || eventFirstRecord > sourceEnd ||
        range.recordCount > sourceEnd - eventFirstRecord) {
      failed_ = true;
      return false;
    }
    nextRecord += range.recordCount;
  }
  if (nextRecord != sourceEnd ||
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
      const auto eventFirstRecord = range.firstRecord +
          (rangesAreSourceLocal ? firstRecord : 0u);
      std::uint64_t logicalPassId = range.logicalPassId;
      if (logicalPassId == 0u) logicalPassId = nextLogicalPassId_++;
      const auto found = std::find_if(
          ranges_.begin(), ranges_.end(), [&](const auto& prior) {
            return prior.logicalPassId == logicalPassId;
          });
      if (found != ranges_.end() &&
          (found->dagPassIndex != range.dagPassIndex ||
           found->passKind != range.passKind)) {
        failed_ = true;
        return false;
      }
      if (logicalPassId >= nextLogicalPassId_) {
        if (logicalPassId == std::numeric_limits<std::uint64_t>::max()) {
          failed_ = true;
          return false;
        }
        nextLogicalPassId_ = logicalPassId + 1u;
      }
      ranges_.push_back(D9CRenderTapeIdentityRangeEntry{
          .eventOrdinal = eventOrdinal,
          .sourceOrdinal = sourceOrdinal,
          .seqId = seqId,
          .logicalPassId = logicalPassId,
          .firstRecord = eventFirstRecord,
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
        .firstRecord = firstRecord,
        .recordCount = recordCount,
        .firstRange = firstRange,
        .rangeCount = static_cast<std::uint32_t>(ranges.size()),
    });
  } catch (...) {
    failed_ = true;
    sources_.clear();
    ranges_.clear();
    return false;
  }
  return true;
}

bool RenderTapeProductionIdentityLedger::registerExpectedSettlement(
    std::uint64_t captureToken, std::uint64_t eventOrdinal,
    std::uint64_t rawOrdinal, std::uint64_t buildGeneration,
    std::uint64_t firstSourceOrdinal, std::uint64_t tailSeqId,
    std::uint32_t sourceCount) noexcept {
  if (captureToken == 0u || eventOrdinal == 0u || rawOrdinal == 0u ||
      buildGeneration == 0u || firstSourceOrdinal == 0u || tailSeqId == 0u ||
      sourceCount == 0u) {
    fail(captureToken);
    return false;
  }
  std::lock_guard lock(mutex_);
  const bool alreadySettled = std::any_of(
      settlements_.begin(), settlements_.begin() + settlementCount_,
      [&](const auto& settlement) {
        return settlement.eventOrdinal == eventOrdinal;
      });
  if (captureToken_ != captureToken || failed_ ||
      settlementCount_ >= kMaxRenderTapeIdentitySettlements ||
      alreadySettled || sources_.empty() ||
      sources_.back().eventOrdinal != eventOrdinal) {
    failed_ = true;
    return false;
  }
  const auto first = std::find_if(
      sources_.begin(), sources_.end(), [&](const auto& source) {
        return source.eventOrdinal == eventOrdinal &&
               source.sourceOrdinal == firstSourceOrdinal;
      });
  const auto eventFirst = std::find_if(
      sources_.begin(), sources_.end(), [&](const auto& source) {
        return source.eventOrdinal == eventOrdinal;
      });
  if (first == sources_.end() || first != eventFirst ||
      static_cast<std::size_t>(std::distance(sources_.begin(), first)) +
              sourceCount != sources_.size()) {
    failed_ = true;
    return false;
  }
  const auto tail = first + (sourceCount - 1u);
  for (auto source = first; source != sources_.end(); ++source) {
    if (source->eventOrdinal != eventOrdinal ||
        source->captureToken != captureToken) {
      failed_ = true;
      return false;
    }
  }
  if (tail->seqId != tailSeqId) {
    failed_ = true;
    return false;
  }
  settlements_[settlementCount_++] = D9CRenderTapeIdentitySettlementEntry{
      .eventOrdinal = eventOrdinal,
      .rawOrdinal = rawOrdinal,
      .buildGeneration = buildGeneration,
      .firstSourceOrdinal = firstSourceOrdinal,
      .tailSeqId = tailSeqId,
      .sourceCount = sourceCount,
      .reserved0 = 0u,
  };
  settled_ = true;
  settledEventOrdinal_ = eventOrdinal;
  settledSourceOrdinal_ = sources_.back().sourceOrdinal;
  settledSeqId_ = sources_.back().seqId;
  return true;
}

bool RenderTapeProductionIdentityLedger::completeSettlement(
    std::uint64_t captureToken, std::uint64_t eventOrdinal) noexcept {
  std::lock_guard lock(mutex_);
  if (captureToken_ != captureToken || failed_) return false;
  for (std::size_t i = 0; i < settlementCount_; ++i) {
    if (settlements_[i].eventOrdinal != eventOrdinal) continue;
    if (settlementCompleted_[i]) return false;
    settlementCompleted_[i] = true;
    ++completedSettlementCount_;
    return true;
  }
  return false;
}

bool RenderTapeProductionIdentityLedger::expectedTail(
    std::uint64_t captureToken,
    D9CRenderTapeIdentitySettlementEntry& out) const noexcept {
  std::lock_guard lock(mutex_);
  if (captureToken_ != captureToken || failed_ || settlementCount_ == 0u) {
    return false;
  }
  out = settlements_[settlementCount_ - 1u];
  return true;
}

bool RenderTapeProductionIdentityLedger::markAllSettled(
    std::uint64_t captureToken) noexcept {
  std::lock_guard lock(mutex_);
  if (captureToken_ != captureToken || failed_) return false;
  for (std::size_t i = 0; i < settlementCount_; ++i) {
    if (!settlementCompleted_[i]) {
      settlementCompleted_[i] = true;
      ++completedSettlementCount_;
    }
  }
  return completedSettlementCount_ == settlementCount_;
}

void RenderTapeProductionIdentityLedger::fail(
    std::uint64_t captureToken) noexcept {
  std::lock_guard lock(mutex_);
  if (captureToken_ != captureToken) {
    captureToken_ = captureToken;
    nextLogicalPassId_ = 1u;
    sources_.clear();
    ranges_.clear();
    settled_ = false;
    settledEventOrdinal_ = 0u;
    settledSourceOrdinal_ = 0u;
    settledSeqId_ = 0u;
    settlementCount_ = 0u;
    settlementCompleted_.fill(false);
    completedSettlementCount_ = 0u;
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
      !settled_ || completedSettlementCount_ != settlementCount_ ||
      settledEventOrdinal_ == 0u ||
      sources_.size() > std::numeric_limits<std::uint32_t>::max() ||
      ranges_.size() > std::numeric_limits<std::uint32_t>::max()) {
    return false;
  }
  const std::uint64_t sourceBytes =
      sources_.size() * sizeof(D9CRenderTapeIdentitySourceEntry);
  const std::uint64_t rangeBytes =
      ranges_.size() * sizeof(D9CRenderTapeIdentityRangeEntry);
  const std::uint64_t settlementBytes = settlementCount_ *
      sizeof(D9CRenderTapeIdentitySettlementEntry);
  if (sourceBytes > std::numeric_limits<std::uint64_t>::max() - rangeBytes ||
      sourceBytes + rangeBytes >
          std::numeric_limits<std::uint64_t>::max() - settlementBytes) {
    return false;
  }
  const std::uint64_t byteCount = sourceBytes + rangeBytes + settlementBytes;
  out.status = D9C_RENDER_TAPE_IDENTITY_CAPTURE_COMPLETE;
  out.sourceCount = static_cast<std::uint32_t>(sources_.size());
  out.rangeCount = static_cast<std::uint32_t>(ranges_.size());
  out.captureToken = captureToken;
  out.byteCount = byteCount;
  out.eventOrdinal = settledEventOrdinal_;
  out.settlementSourceOrdinal = settledSourceOrdinal_;
  out.settlementSeqId = settledSeqId_;
  out.settlementCount = static_cast<std::uint32_t>(settlementCount_);
  out.reserved1 = 0u;
  out.settlementEntrySize = sizeof(D9CRenderTapeIdentitySettlementEntry);
  out.reserved2 = 0u;
  out.settlementTableOffset = sourceBytes + rangeBytes;
  if (bytes.empty()) {
    return true;
  }
  if (bytes.size() != byteCount) {
    out.status = D9C_RENDER_TAPE_IDENTITY_CAPTURE_FAILED;
    return false;
  }
  std::memcpy(bytes.data(), sources_.data(), sourceBytes);
  std::memcpy(bytes.data() + sourceBytes, ranges_.data(), rangeBytes);
  std::memcpy(bytes.data() + sourceBytes + rangeBytes, settlements_.data(),
              settlementBytes);
  return true;
}

} // namespace dxmt9::d3d9
