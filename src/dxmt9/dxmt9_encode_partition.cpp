#include "dxmt9_encode_partition.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>

namespace dxmt9::encoders {
namespace {

using Validation = EncodePartitionEntryValidation;

EncodePartitionEntryResolution reject(Validation validation) noexcept {
  return EncodePartitionEntryResolution{.validation = validation};
}

bool rangeEquals(core::DrawPayloadRange lhs,
                 core::DrawPayloadRange rhs) noexcept {
  return lhs.offset == rhs.offset && lhs.size == rhs.size;
}

bool absolutePayloadRange(const core::DrawRunCommandRecord& record,
                          core::DrawPayloadRange relative,
                          std::size_t arenaSize,
                          std::size_t schemaSize,
                          core::DrawPayloadRange& absolute) noexcept {
  if (relative.empty()) {
    absolute = {};
    return true;
  }
  if (relative.size != schemaSize ||
      relative.offset > record.payloadSize ||
      relative.size > record.payloadSize - relative.offset ||
      record.payloadOffset > arenaSize ||
      record.payloadSize > arenaSize - record.payloadOffset) {
    return false;
  }

  const std::uint64_t offset =
      static_cast<std::uint64_t>(record.payloadOffset) + relative.offset;
  if (offset > std::numeric_limits<std::uint32_t>::max() ||
      offset > arenaSize || relative.size > arenaSize - offset) {
    return false;
  }
  absolute = core::DrawPayloadRange{
      .offset = static_cast<std::uint32_t>(offset),
      .size = relative.size,
  };
  return true;
}

std::span<const core::u8> payloadBytes(
    const core::ChunkSlot& slot,
    core::DrawPayloadRange range) noexcept {
  if (range.empty()) {
    return {};
  }
  return std::span<const core::u8>(slot.drawPayloadArena.data() + range.offset,
                                   range.size);
}

bool snapshotForDrawParam(
    const EncodePartitionReplayStream& stream,
    std::size_t replayOrdinal,
    std::uint32_t drawParamIndex,
    EncodePartitionEntrySnapshot& snapshot) noexcept {
  std::uint32_t commandIndex = 0;
  if (!stream.valid || !stream.source.slot ||
      stream.source.slotIndex > std::numeric_limits<std::uint32_t>::max() ||
      !stream.commandIndexAt(replayOrdinal, commandIndex)) {
    return false;
  }

  const core::ChunkSlot& slot = *stream.source.slot;
  const auto command = slot.commandAt(commandIndex);
  if (command.kind != core::MetalCommandKind::DrawRun ||
      !command.drawRunRecord || !command.drawState.hot ||
      !command.drawState.shaderLayout || !command.drawState.debug ||
      commandIndex >= slot.commandHeaders.size()) {
    return false;
  }
  const auto& record = *command.drawRunRecord;
  if (drawParamIndex < record.firstParam ||
      drawParamIndex - record.firstParam >= record.paramCount ||
      drawParamIndex >= slot.drawParams.size()) {
    return false;
  }
  const auto& param = slot.drawParams[drawParamIndex];
  const core::DrawUniformHandle uniformHandle =
      param.uniformHandle.valid() ? param.uniformHandle : record.uniformHandle;
  if (!slot.drawUniformPayloadRecord(uniformHandle)) {
    return false;
  }

  core::DrawPayloadRange bindingOverrideBytes{};
  if (!absolutePayloadRange(record, param.bindingOverrideRange,
                            slot.drawPayloadArena.size(),
                            sizeof(core::DrawBindingOverride),
                            bindingOverrideBytes)) {
    return false;
  }
  core::DrawPayloadRange bindingSnapshotBytes{};
  if (!absolutePayloadRange(record, param.bindingSnapshotRange,
                            slot.drawPayloadArena.size(),
                            sizeof(core::DrawBindingSnapshot),
                            bindingSnapshotBytes)) {
    return false;
  }

  snapshot = EncodePartitionEntrySnapshot{
      .source = RetainedEncodeSourceLocator{
          .sourceOrdinal = 0,
          .slotIndex = static_cast<std::uint32_t>(stream.source.slotIndex),
          .seqId = stream.source.seqId,
      },
      .commandIndex = commandIndex,
      .drawRunRecordIndex = slot.commandHeaders[commandIndex].payloadIndex.value,
      .stateIndex = record.stateIndex,
      .drawParamIndex = drawParamIndex,
      .uniformHandle = uniformHandle,
      .bindingOverrideBytes = bindingOverrideBytes,
      .bindingSnapshotBytes = bindingSnapshotBytes,
  };
  const std::span<const core::metalqueue::ReadySlotSnapshot> sources(
      &stream.source, 1u);
  return static_cast<bool>(resolveEncodePartitionEntry(snapshot, sources));
}

EncodePartitionRangeValidationResult rejectRange(
    EncodePartitionRangeValidation validation,
    std::size_t rangeIndex,
    EncodePartitionEntryValidation entryValidation =
        EncodePartitionEntryValidation::SourceOrdinalOutOfRange) noexcept {
  return EncodePartitionRangeValidationResult{
      .validation = validation,
      .entryValidation = entryValidation,
      .rangeIndex = rangeIndex,
  };
}

}  // namespace

EncodePartitionEntryResolution resolveEncodePartitionEntry(
    const EncodePartitionEntrySnapshot& snapshot,
    std::span<const core::metalqueue::ReadySlotSnapshot> sources) noexcept {
  if (snapshot.source.sourceOrdinal >= sources.size()) {
    return reject(Validation::SourceOrdinalOutOfRange);
  }

  const auto& source = sources[snapshot.source.sourceOrdinal];
  const core::ChunkSlot* slot = source.slot;
  if (!slot || source.slotIndex != snapshot.source.slotIndex ||
      source.seqId == 0 || source.seqId != snapshot.source.seqId ||
      slot->seqId != snapshot.source.seqId) {
    return reject(Validation::SourceIdentityMismatch);
  }

  const std::size_t slotCommandCount = slot->commandCount();
  if (source.commandBegin > slotCommandCount ||
      source.commandCount > slotCommandCount - source.commandBegin) {
    return reject(Validation::SourceCommandRangeInvalid);
  }
  const std::size_t sourceCommandEnd =
      source.commandBegin + source.commandCount;
  if (snapshot.commandIndex < source.commandBegin ||
      snapshot.commandIndex >= sourceCommandEnd) {
    return reject(Validation::CommandIndexOutOfRange);
  }

  const auto command = slot->commandAt(snapshot.commandIndex);
  if (command.kind != core::MetalCommandKind::DrawRun ||
      !command.drawRunRecord) {
    return reject(Validation::CommandIsNotDrawRun);
  }
  const auto& header = slot->commandHeaders[snapshot.commandIndex];
  if (header.payloadIndex.value != snapshot.drawRunRecordIndex ||
      snapshot.drawRunRecordIndex >= slot->drawRunRecords.size() ||
      command.drawRunRecord !=
          &slot->drawRunRecords[snapshot.drawRunRecordIndex]) {
    return reject(Validation::DrawRunRecordMismatch);
  }

  const auto& record = *command.drawRunRecord;
  if (record.stateIndex != snapshot.stateIndex ||
      !command.drawState.hot || !command.drawState.shaderLayout ||
      !command.drawState.debug) {
    return reject(Validation::StateIndexMismatch);
  }
  if (snapshot.drawParamIndex < record.firstParam ||
      snapshot.drawParamIndex - record.firstParam >= record.paramCount ||
      snapshot.drawParamIndex >= slot->drawParams.size()) {
    return reject(Validation::DrawParamIndexMismatch);
  }

  const auto& param = slot->drawParams[snapshot.drawParamIndex];
  const core::DrawUniformHandle effectiveUniform =
      param.uniformHandle.valid() ? param.uniformHandle : record.uniformHandle;
  const auto* uniform = slot->drawUniformPayloadRecord(effectiveUniform);
  if (!uniform || !(effectiveUniform == snapshot.uniformHandle)) {
    return reject(Validation::UniformHandleMismatch);
  }

  core::DrawPayloadRange bindingOverrideBytes{};
  if (!absolutePayloadRange(record, param.bindingOverrideRange,
                            slot->drawPayloadArena.size(),
                            sizeof(core::DrawBindingOverride),
                            bindingOverrideBytes) ||
      !rangeEquals(bindingOverrideBytes, snapshot.bindingOverrideBytes)) {
    return reject(Validation::BindingOverrideRangeMismatch);
  }
  core::DrawPayloadRange bindingSnapshotBytes{};
  if (!absolutePayloadRange(record, param.bindingSnapshotRange,
                            slot->drawPayloadArena.size(),
                            sizeof(core::DrawBindingSnapshot),
                            bindingSnapshotBytes) ||
      !rangeEquals(bindingSnapshotBytes, snapshot.bindingSnapshotBytes)) {
    return reject(Validation::BindingSnapshotRangeMismatch);
  }

  return EncodePartitionEntryResolution{
      .validation = Validation::Valid,
      .entry = ResolvedEncodePartitionEntry{
          .slot = slot,
          .command = command,
          .drawRunRecord = &record,
          .drawState = command.drawState,
          .drawParam = &param,
          .uniform = uniform,
          .bindingOverrideBytes = payloadBytes(*slot, bindingOverrideBytes),
          .bindingSnapshotBytes = payloadBytes(*slot, bindingSnapshotBytes),
      },
  };
}

std::size_t EncodePartitionReplayStream::replayOrdinalCount() const noexcept {
  return commandOrderActive ? commandOrder.size() : source.commandCount;
}

bool EncodePartitionReplayStream::commandIndexAt(
    std::size_t replayOrdinal,
    std::uint32_t& commandIndex) const noexcept {
  if (!valid || replayOrdinal >= replayOrdinalCount()) {
    return false;
  }
  const std::size_t selected = commandOrderActive
      ? static_cast<std::size_t>(commandOrder[replayOrdinal])
      : source.commandBegin + replayOrdinal;
  if (selected > std::numeric_limits<std::uint32_t>::max()) {
    return false;
  }
  commandIndex = static_cast<std::uint32_t>(selected);
  return true;
}

EncodePartitionReplayStream makeEncodePartitionReplayStream(
    std::size_t slotIndex,
    const core::ChunkSlot& slot,
    std::size_t commandBegin,
    std::size_t commandCount,
    bool commandOrderActive,
    std::span<const std::uint32_t> commandOrder) noexcept {
  const bool sourceRangeValid =
      commandBegin <= slot.commandCount() &&
      commandCount <= slot.commandCount() - commandBegin;
  const std::size_t replayCount =
      commandOrderActive ? commandOrder.size() : commandCount;
  return EncodePartitionReplayStream{
      .source = core::metalqueue::ReadySlotSnapshot{
          .slotIndex = slotIndex,
          .seqId = slot.seqId,
          .hasPresent = false,
          .commandBegin = commandBegin,
          .commandCount = commandCount,
          .slot = const_cast<core::ChunkSlot*>(&slot),
      },
      .commandOrderActive = commandOrderActive,
      .commandOrder = commandOrder,
      .valid = sourceRangeValid &&
               replayCount <= std::numeric_limits<std::uint32_t>::max(),
  };
}

bool buildEncodePartitionEntrySnapshot(
    const EncodePartitionReplayStream& stream,
    std::size_t replayOrdinal,
    std::uint32_t drawParamIndex,
    EncodePartitionEntrySnapshot& snapshot) noexcept {
  return snapshotForDrawParam(stream, replayOrdinal, drawParamIndex, snapshot);
}

EncodePartitionResolution resolveEncodePartition(
    const EncodePartitionRangeSnapshot& range,
    const EncodePartitionReplayStream& stream) noexcept {
  if (!stream.valid) {
    return {};
  }
  if (range.kind != EncodePartitionRangeKind::DrawRunEntries ||
      range.replayOrdinalCount != 1u || range.drawEntryCount == 0u) {
    return EncodePartitionResolution{
        .validation = range.kind == EncodePartitionRangeKind::DrawRunEntries
            ? (range.replayOrdinalCount == 1u
                   ? EncodePartitionRangeValidation::DrawRunEntriesEmpty
                   : EncodePartitionRangeValidation::DrawRunReplayCountInvalid)
            : EncodePartitionRangeValidation::DrawRunCommandMismatch,
    };
  }
  std::uint32_t effectiveCommandIndex = 0;
  if (!stream.commandIndexAt(range.replayOrdinalBegin,
                             effectiveCommandIndex) ||
      range.entry.commandIndex != effectiveCommandIndex) {
    return EncodePartitionResolution{
        .validation = EncodePartitionRangeValidation::DrawRunCommandMismatch,
    };
  }

  const std::span<const core::metalqueue::ReadySlotSnapshot> sources(
      &stream.source, 1u);
  const auto entry = resolveEncodePartitionEntry(range.entry, sources);
  if (!entry) {
    return EncodePartitionResolution{
        .validation = EncodePartitionRangeValidation::EntryResolutionFailed,
        .entryValidation = entry.validation,
    };
  }
  const auto& record = *entry.entry.drawRunRecord;
  const std::uint64_t drawEnd =
      static_cast<std::uint64_t>(range.entry.drawParamIndex) +
      range.drawEntryCount;
  const std::uint64_t runEnd =
      static_cast<std::uint64_t>(record.firstParam) + record.paramCount;
  if (drawEnd > std::numeric_limits<std::uint32_t>::max() ||
      runEnd > std::numeric_limits<std::uint32_t>::max() ||
      drawEnd > runEnd || !entry.entry.slot ||
      drawEnd > entry.entry.slot->drawParams.size()) {
    return EncodePartitionResolution{
        .validation = EncodePartitionRangeValidation::DrawEntryOverflow,
        .entryValidation = EncodePartitionEntryValidation::Valid,
    };
  }

  return EncodePartitionResolution{
      .validation = EncodePartitionRangeValidation::Valid,
      .entryValidation = EncodePartitionEntryValidation::Valid,
      .partition = ResolvedEncodePartition{
          .entry = entry.entry,
          .drawParams = std::span<const core::DrawParam>(
              entry.entry.slot->drawParams.data() +
                  range.entry.drawParamIndex,
              range.drawEntryCount),
      },
  };
}

EncodePartitionRangeValidationResult validateEncodePartitionRanges(
    std::span<const EncodePartitionRangeSnapshot> ranges,
    const EncodePartitionReplayStream& stream) noexcept {
  if (!stream.valid) {
    return rejectRange(EncodePartitionRangeValidation::ReplayStreamInvalid, 0);
  }
  if (stream.replayOrdinalCount() >
      std::numeric_limits<std::uint32_t>::max()) {
    return rejectRange(EncodePartitionRangeValidation::ReplayStreamTooLarge, 0);
  }

  std::uint32_t expectedOrdinal = 0;
  std::uint32_t expectedDrawParam = 0;
  bool drawRunIncomplete = false;
  for (std::size_t i = 0; i < ranges.size(); ++i) {
    const auto& range = ranges[i];
    if (range.kind == EncodePartitionRangeKind::CommandSegment) {
      if (range.replayOrdinalCount == 0u) {
        return rejectRange(
            EncodePartitionRangeValidation::CommandSegmentEmpty, i);
      }
      if (range.drawEntryCount != 0u) {
        return rejectRange(
            EncodePartitionRangeValidation::CommandSegmentHasDrawEntries, i);
      }
      if (drawRunIncomplete) {
        return rejectRange(
            EncodePartitionRangeValidation::DrawCoveragePartialTail, i);
      }
      const std::uint64_t end =
          static_cast<std::uint64_t>(range.replayOrdinalBegin) +
          range.replayOrdinalCount;
      if (end > std::numeric_limits<std::uint32_t>::max()) {
        return rejectRange(
            EncodePartitionRangeValidation::ReplayOrdinalOverflow, i);
      }
      if (range.replayOrdinalBegin < expectedOrdinal) {
        return rejectRange(
            EncodePartitionRangeValidation::ReplayCoverageOverlap, i);
      }
      if (range.replayOrdinalBegin > expectedOrdinal) {
        return rejectRange(
            EncodePartitionRangeValidation::ReplayCoverageGap, i);
      }
      if (end > stream.replayOrdinalCount()) {
        return rejectRange(
            EncodePartitionRangeValidation::ReplayOrdinalOverflow, i);
      }
      expectedOrdinal = static_cast<std::uint32_t>(end);
      continue;
    }

    if (range.replayOrdinalCount != 1u) {
      return rejectRange(
          EncodePartitionRangeValidation::DrawRunReplayCountInvalid, i);
    }
    if (range.drawEntryCount == 0u) {
      return rejectRange(
          EncodePartitionRangeValidation::DrawRunEntriesEmpty, i);
    }
    if (range.replayOrdinalBegin < expectedOrdinal) {
      return rejectRange(
          EncodePartitionRangeValidation::ReplayCoverageOverlap, i);
    }
    if (range.replayOrdinalBegin > expectedOrdinal) {
      return rejectRange(
          EncodePartitionRangeValidation::ReplayCoverageGap, i);
    }

    const auto resolved = resolveEncodePartition(range, stream);
    if (!resolved) {
      return rejectRange(resolved.validation, i, resolved.entryValidation);
    }
    const auto& record = *resolved.partition.entry.drawRunRecord;
    const std::uint32_t drawBegin = range.entry.drawParamIndex;
    if (drawRunIncomplete) {
      if (drawBegin < expectedDrawParam) {
        return rejectRange(
            EncodePartitionRangeValidation::DrawCoverageOverlap, i);
      }
      if (drawBegin > expectedDrawParam) {
        return rejectRange(
            EncodePartitionRangeValidation::DrawCoverageGap, i);
      }
    } else if (drawBegin != record.firstParam) {
      return rejectRange(
          drawBegin < record.firstParam
              ? EncodePartitionRangeValidation::DrawCoverageOverlap
              : EncodePartitionRangeValidation::DrawCoverageGap,
          i);
    }
    const std::uint64_t drawEnd =
        static_cast<std::uint64_t>(drawBegin) + range.drawEntryCount;
    const std::uint64_t runEnd =
        static_cast<std::uint64_t>(record.firstParam) + record.paramCount;
    if (drawEnd > std::numeric_limits<std::uint32_t>::max() ||
        runEnd > std::numeric_limits<std::uint32_t>::max() ||
        drawEnd > runEnd) {
      return rejectRange(
          EncodePartitionRangeValidation::DrawEntryOverflow, i);
    }
    expectedDrawParam = static_cast<std::uint32_t>(drawEnd);
    drawRunIncomplete = drawEnd != runEnd;
    if (!drawRunIncomplete) {
      ++expectedOrdinal;
      expectedDrawParam = 0;
    }
  }

  if (drawRunIncomplete) {
    return rejectRange(
        EncodePartitionRangeValidation::DrawCoveragePartialTail,
        ranges.size());
  }
  if (expectedOrdinal != stream.replayOrdinalCount()) {
    return rejectRange(
        EncodePartitionRangeValidation::ReplayCoverageGap, ranges.size());
  }
  return EncodePartitionRangeValidationResult{
      .validation = EncodePartitionRangeValidation::Valid,
      .entryValidation = EncodePartitionEntryValidation::Valid,
      .rangeIndex = ranges.size(),
  };
}

EncodePartitionIdentityCursor::EncodePartitionIdentityCursor(
    const EncodePartitionReplayStream& stream) noexcept
    : stream_(&stream) {}

bool EncodePartitionIdentityCursor::next(
    EncodePartitionRangeSnapshot& range) noexcept {
  if (!stream_ || !stream_->valid ||
      replayOrdinal_ >= stream_->replayOrdinalCount()) {
    return false;
  }

  std::uint32_t commandIndex = 0;
  if (stream_->commandIndexAt(replayOrdinal_, commandIndex)) {
    const auto command = stream_->source.slot->commandAt(commandIndex);
    EncodePartitionEntrySnapshot entry{};
    if (command.kind == core::MetalCommandKind::DrawRun &&
        command.drawRunRecord && command.drawRunRecord->paramCount != 0u &&
        buildEncodePartitionEntrySnapshot(
            *stream_, replayOrdinal_, command.drawRunRecord->firstParam,
            entry)) {
      range = EncodePartitionRangeSnapshot{
          .kind = EncodePartitionRangeKind::DrawRunEntries,
          .replayOrdinalBegin = static_cast<std::uint32_t>(replayOrdinal_),
          .replayOrdinalCount = 1u,
          .drawEntryCount = command.drawRunRecord->paramCount,
          .entry = entry,
      };
      ++replayOrdinal_;
      return true;
    }
  }

  const std::size_t begin = replayOrdinal_++;
  while (replayOrdinal_ < stream_->replayOrdinalCount()) {
    std::uint32_t nextCommandIndex = 0;
    if (stream_->commandIndexAt(replayOrdinal_, nextCommandIndex)) {
      const auto next = stream_->source.slot->commandAt(nextCommandIndex);
      EncodePartitionEntrySnapshot entry{};
      if (next.kind == core::MetalCommandKind::DrawRun &&
          next.drawRunRecord && next.drawRunRecord->paramCount != 0u &&
          buildEncodePartitionEntrySnapshot(
              *stream_, replayOrdinal_, next.drawRunRecord->firstParam,
              entry)) {
        break;
      }
    }
    ++replayOrdinal_;
  }
  range = EncodePartitionRangeSnapshot{
      .kind = EncodePartitionRangeKind::CommandSegment,
      .replayOrdinalBegin = static_cast<std::uint32_t>(begin),
      .replayOrdinalCount =
          static_cast<std::uint32_t>(replayOrdinal_ - begin),
      .drawEntryCount = 0u,
  };
  return true;
}

EncodePartitionSerialCursor::EncodePartitionSerialCursor(
    const EncodePartitionReplayStream& stream,
    std::span<const EncodePartitionRangeSnapshot> explicitRanges,
    bool useExplicitPlan) noexcept
    : explicitRanges_(explicitRanges),
      useExplicitPlan_(useExplicitPlan),
      identityCursor_(stream) {}

bool EncodePartitionSerialCursor::next(
    EncodePartitionSerialBatch& batch) noexcept {
  if (!useExplicitPlan_) {
    if (!identityCursor_.next(identityRange_)) {
      return false;
    }
    batch = EncodePartitionSerialBatch{
        .kind = identityRange_.kind,
        .replayOrdinalBegin = identityRange_.replayOrdinalBegin,
        .replayOrdinalCount = identityRange_.replayOrdinalCount,
        .ranges = std::span<const EncodePartitionRangeSnapshot>(
            &identityRange_, 1u),
    };
    return true;
  }
  if (explicitIndex_ >= explicitRanges_.size()) {
    return false;
  }

  const std::size_t begin = explicitIndex_++;
  const auto& first = explicitRanges_[begin];
  if (first.kind == EncodePartitionRangeKind::DrawRunEntries) {
    while (explicitIndex_ < explicitRanges_.size()) {
      const auto& next = explicitRanges_[explicitIndex_];
      if (next.kind != EncodePartitionRangeKind::DrawRunEntries ||
          next.replayOrdinalBegin != first.replayOrdinalBegin) {
        break;
      }
      ++explicitIndex_;
    }
  }
  batch = EncodePartitionSerialBatch{
      .kind = first.kind,
      .replayOrdinalBegin = first.replayOrdinalBegin,
      .replayOrdinalCount = first.replayOrdinalCount,
      .ranges = explicitRanges_.subspan(begin, explicitIndex_ - begin),
  };
  return true;
}

}  // namespace dxmt9::encoders
