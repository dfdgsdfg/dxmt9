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

bool resolvePayloadRange(
    const core::DrawRunCommandRecord& record,
    core::DrawPayloadRange relative,
    std::span<const core::u8> commandPayload,
    std::size_t schemaSize,
    core::DrawPayloadRange& absolute,
    std::span<const core::u8>& bytes) noexcept {
  if (relative.empty()) {
    absolute = {};
    bytes = {};
    return true;
  }
  if (relative.size != schemaSize ||
      relative.offset > record.payloadSize ||
      relative.size > record.payloadSize - relative.offset ||
      commandPayload.size() != record.payloadSize) {
    return false;
  }

  const std::uint64_t offset =
      static_cast<std::uint64_t>(record.payloadOffset) + relative.offset;
  if (offset > std::numeric_limits<std::uint32_t>::max() ||
      relative.offset > commandPayload.size() ||
      relative.size > commandPayload.size() - relative.offset) {
    return false;
  }
  absolute = core::DrawPayloadRange{
      .offset = static_cast<std::uint32_t>(offset),
      .size = relative.size,
  };
  bytes = commandPayload.subspan(relative.offset, relative.size);
  return true;
}

bool snapshotForResolvedDrawParam(
    const EncodePartitionReplayStream& stream,
    std::uint32_t commandIndex,
    const core::SourceCommandView& sourceCommand,
    std::uint32_t drawParamIndex,
    EncodePartitionEntrySnapshot& snapshot,
    ResolvedEncodePartitionEntry& resolved) noexcept {
  const core::CpuReadyTape::SourceRef streamSource{
      .id = stream.source.sourceId,
      .storage = stream.source.storage,
  };
  const core::SourcePayloadView payload = stream.source.payload;
  if (!stream.valid || !payload.valid() || !streamSource.valid() ||
      stream.source.slotIndex > std::numeric_limits<std::uint32_t>::max()) {
    return false;
  }

  const std::size_t sourceCommandEnd =
      stream.source.commandBegin + stream.source.commandCount;
  if (stream.source.seqId == 0 ||
      (stream.source.slot &&
       stream.source.slot->seqId != stream.source.seqId) ||
      commandIndex < stream.source.commandBegin ||
      commandIndex >= sourceCommandEnd) {
    return false;
  }
  const auto& command = sourceCommand.command;
  if (command.kind != core::MetalCommandKind::DrawRun ||
      !command.drawRunRecord || !command.drawState.hot ||
      !command.drawState.shaderLayout || !command.drawState.debug ||
      commandIndex >= payload.commandCount()) {
    return false;
  }
  const auto& record = *command.drawRunRecord;
  if (command.drawParams.size() != record.paramCount ||
      drawParamIndex < record.firstParam ||
      drawParamIndex - record.firstParam >= record.paramCount ||
      drawParamIndex - record.firstParam >= command.drawParams.size()) {
    return false;
  }
  const auto& param =
      command.drawParams[drawParamIndex - record.firstParam];
  const core::DrawUniformHandle uniformHandle =
      param.uniformHandle.valid() ? param.uniformHandle : record.uniformHandle;
  const auto uniforms = command.drawUniformPayloadRecords;
  const auto* uniform = uniformHandle.valid() &&
                                uniformHandle.index < uniforms.size() &&
                                uniforms[uniformHandle.index].handle ==
                                    uniformHandle
                            ? &uniforms[uniformHandle.index]
                            : nullptr;
  if (!uniform) {
    return false;
  }

  core::DrawPayloadRange bindingOverrideBytes{};
  std::span<const core::u8> bindingOverrideView{};
  if (!resolvePayloadRange(record, param.bindingOverrideRange,
                           command.drawPayloadBytes,
                           sizeof(core::DrawBindingOverride),
                           bindingOverrideBytes, bindingOverrideView)) {
    return false;
  }
  core::DrawPayloadRange bindingSnapshotBytes{};
  std::span<const core::u8> bindingSnapshotView{};
  if (!resolvePayloadRange(record, param.bindingSnapshotRange,
                           command.drawPayloadBytes,
                           sizeof(core::DrawBindingSnapshot),
                           bindingSnapshotBytes, bindingSnapshotView)) {
    return false;
  }

  snapshot = EncodePartitionEntrySnapshot{
      .source = RetainedEncodeSourceLocator{
          .tapeSource = core::CpuReadyTape::SourceRef{
              .id = stream.source.sourceId,
              .storage = stream.source.storage,
          },
          .retainedSourceIndex = 0,
          .slotIndex = static_cast<std::uint32_t>(stream.source.slotIndex),
          .seqId = stream.source.seqId,
      },
      .commandIndex = commandIndex,
      .drawRunRecordIndex = sourceCommand.payloadIndex,
      .stateIndex = record.stateIndex,
      .drawParamIndex = drawParamIndex,
      .uniformHandle = uniformHandle,
      .bindingOverrideBytes = bindingOverrideBytes,
      .bindingSnapshotBytes = bindingSnapshotBytes,
  };
  resolved = ResolvedEncodePartitionEntry{
      .slot = stream.source.slot,
      .command = command,
      .drawRunRecord = &record,
      .drawState = command.drawState,
      .drawParam = &param,
      .uniform = uniform,
      .bindingOverrideBytes = bindingOverrideView,
      .bindingSnapshotBytes = bindingSnapshotView,
  };
  return true;
}

bool snapshotForDrawParam(
    const EncodePartitionReplayStream& stream,
    std::size_t replayOrdinal,
    std::uint32_t drawParamIndex,
    EncodePartitionEntrySnapshot& snapshot,
    ResolvedEncodePartitionEntry& resolved) noexcept {
  std::uint32_t commandIndex = 0;
  if (!stream.valid || !stream.source.payload.valid() ||
      !stream.commandIndexAt(replayOrdinal, commandIndex)) {
    return false;
  }
  const auto command = stream.source.payload.commandAt(commandIndex);
  return snapshotForResolvedDrawParam(stream, commandIndex, command,
                                      drawParamIndex, snapshot, resolved);
}

EncodePartitionRangeValidationResult rejectRange(
    EncodePartitionRangeValidation validation,
    std::size_t rangeIndex,
    EncodePartitionEntryValidation entryValidation =
        EncodePartitionEntryValidation::RetainedSourceIndexOutOfRange) noexcept {
  return EncodePartitionRangeValidationResult{
      .validation = validation,
      .entryValidation = entryValidation,
      .rangeIndex = rangeIndex,
  };
}

}  // namespace

EncodePartitionEntryResolution resolveEncodePartitionEntry(
    const EncodePartitionEntrySnapshot& snapshot,
    std::span<const core::metalqueue::ResolvedPublishedSource> sources) noexcept {
  if (snapshot.source.retainedSourceIndex >= sources.size()) {
    return reject(Validation::RetainedSourceIndexOutOfRange);
  }

  const auto& source = sources[snapshot.source.retainedSourceIndex];
  const core::CpuReadyTape::SourceRef resolvedSource{
      .id = source.sourceId,
      .storage = source.storage,
  };
  const core::ChunkSlot* slot = source.slot;
  if (!snapshot.source.tapeSource.valid() || !resolvedSource.valid() ||
      !source.payload.valid() ||
      source.sourceId != snapshot.source.tapeSource.id ||
      source.storage != snapshot.source.tapeSource.storage ||
      source.slotIndex != snapshot.source.slotIndex ||
      source.seqId == 0 || source.seqId != snapshot.source.seqId ||
      (slot && (slot->seqId != snapshot.source.seqId ||
                source.payload.legacyPayload() != slot))) {
    return reject(Validation::SourceIdentityMismatch);
  }

  const std::size_t sourcePayloadCommandCount = source.payload.commandCount();
  if (source.commandBegin > sourcePayloadCommandCount ||
      source.commandCount >
          sourcePayloadCommandCount - source.commandBegin) {
    return reject(Validation::SourceCommandRangeInvalid);
  }
  const std::size_t sourceCommandEnd =
      source.commandBegin + source.commandCount;
  if (snapshot.commandIndex < source.commandBegin ||
      snapshot.commandIndex >= sourceCommandEnd) {
    return reject(Validation::CommandIndexOutOfRange);
  }

  const auto sourceCommand = source.payload.commandAt(snapshot.commandIndex);
  const auto& command = sourceCommand.command;
  if (command.kind != core::MetalCommandKind::DrawRun ||
      !command.drawRunRecord) {
    return reject(Validation::CommandIsNotDrawRun);
  }
  if (sourceCommand.payloadIndex != snapshot.drawRunRecordIndex) {
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
      snapshot.drawParamIndex - record.firstParam >=
          command.drawParams.size()) {
    return reject(Validation::DrawParamIndexMismatch);
  }

  const auto& param = command.drawParams[
      snapshot.drawParamIndex - record.firstParam];
  const core::DrawUniformHandle effectiveUniform =
      param.uniformHandle.valid() ? param.uniformHandle : record.uniformHandle;
  const auto uniforms = command.drawUniformPayloadRecords;
  const auto* uniform = effectiveUniform.valid() &&
                                effectiveUniform.index < uniforms.size() &&
                                uniforms[effectiveUniform.index].handle ==
                                    effectiveUniform
                            ? &uniforms[effectiveUniform.index]
                            : nullptr;
  if (!uniform || !(effectiveUniform == snapshot.uniformHandle)) {
    return reject(Validation::UniformHandleMismatch);
  }

  core::DrawPayloadRange bindingOverrideBytes{};
  std::span<const core::u8> bindingOverrideView{};
  if (!resolvePayloadRange(record, param.bindingOverrideRange,
                           command.drawPayloadBytes,
                           sizeof(core::DrawBindingOverride),
                           bindingOverrideBytes, bindingOverrideView) ||
      !rangeEquals(bindingOverrideBytes, snapshot.bindingOverrideBytes)) {
    return reject(Validation::BindingOverrideRangeMismatch);
  }
  core::DrawPayloadRange bindingSnapshotBytes{};
  std::span<const core::u8> bindingSnapshotView{};
  if (!resolvePayloadRange(record, param.bindingSnapshotRange,
                           command.drawPayloadBytes,
                           sizeof(core::DrawBindingSnapshot),
                           bindingSnapshotBytes, bindingSnapshotView) ||
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
          .bindingOverrideBytes = bindingOverrideView,
          .bindingSnapshotBytes = bindingSnapshotView,
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
    core::SourcePayloadView payload,
    std::uint64_t seqId,
    std::size_t commandBegin,
    std::size_t commandCount,
    bool commandOrderActive,
    std::span<const std::uint32_t> commandOrder,
    std::span<std::size_t> replayOrdinalByCommandIndex,
    core::CpuReadyTape::SourceRef tapeSource) noexcept {
  const bool sourceRangeValid =
      commandBegin <= payload.commandCount() &&
      commandCount <= payload.commandCount() - commandBegin;
  const std::size_t replayCount =
      commandOrderActive ? commandOrder.size() : commandCount;
  bool replaySelectionValid =
      tapeSource.valid() && sourceRangeValid &&
      replayCount <= std::numeric_limits<std::uint32_t>::max();
  if (replaySelectionValid && !commandOrderActive) {
    replaySelectionValid = commandOrder.empty();
    if (replaySelectionValid && commandCount != 0u) {
      replaySelectionValid =
          commandBegin <= std::numeric_limits<std::uint32_t>::max() &&
          commandCount - 1u <=
              std::numeric_limits<std::uint32_t>::max() - commandBegin;
    }
  } else if (replaySelectionValid) {
    replaySelectionValid = commandOrder.size() <= commandCount;
    if (replaySelectionValid && !commandOrder.empty()) {
      replaySelectionValid =
          replayOrdinalByCommandIndex.size() >= commandCount;
    }
    if (replaySelectionValid &&
        replayOrdinalByCommandIndex.size() >= commandCount) {
      const auto missingOrdinal = std::numeric_limits<std::size_t>::max();
      for (std::size_t& ordinal :
           replayOrdinalByCommandIndex.first(commandCount)) {
        ordinal = missingOrdinal;
      }
      const std::size_t commandEnd = commandBegin + commandCount;
      for (std::size_t ordinal = 0;
           ordinal < commandOrder.size();
           ++ordinal) {
        const std::size_t commandIndex = commandOrder[ordinal];
        if (commandIndex < commandBegin || commandIndex >= commandEnd) {
          replaySelectionValid = false;
          break;
        }
        const std::size_t relative = commandIndex - commandBegin;
        if (replayOrdinalByCommandIndex[relative] != missingOrdinal) {
          replaySelectionValid = false;
          break;
        }
        replayOrdinalByCommandIndex[relative] = ordinal;
      }
    }
  }
  return EncodePartitionReplayStream{
      .source = core::metalqueue::ResolvedPublishedSource{
          .source = tapeSource,
          .slotIndex = slotIndex,
          .seqId = seqId,
          .payload = payload,
          .sourceId = tapeSource.id,
          .storage = tapeSource.storage,
          .slot = payload.legacyPayload(),
          .commandBegin = commandBegin,
          .commandCount = commandCount,
      },
      .commandOrderActive = commandOrderActive,
      .commandOrder = commandOrder,
      .valid = replaySelectionValid,
  };
}

EncodePartitionReplayStream makeEncodePartitionReplayStream(
    std::size_t slotIndex,
    const core::ChunkSlot& slot,
    std::size_t commandBegin,
    std::size_t commandCount,
    bool commandOrderActive,
    std::span<const std::uint32_t> commandOrder,
    std::span<std::size_t> replayOrdinalByCommandIndex,
    core::CpuReadyTape::SourceRef tapeSource) noexcept {
  return makeEncodePartitionReplayStream(
      slotIndex, core::SourcePayloadView(slot), slot.seqId, commandBegin,
      commandCount, commandOrderActive, commandOrder,
      replayOrdinalByCommandIndex, tapeSource);
}

bool buildEncodePartitionEntrySnapshot(
    const EncodePartitionReplayStream& stream,
    std::size_t replayOrdinal,
    std::uint32_t drawParamIndex,
    EncodePartitionEntrySnapshot& snapshot) noexcept {
  ResolvedEncodePartitionEntry resolved{};
  return snapshotForDrawParam(stream, replayOrdinal, drawParamIndex, snapshot,
                              resolved);
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

  const std::span<const core::metalqueue::ResolvedPublishedSource> sources(
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
      drawEnd > runEnd ||
      range.entry.drawParamIndex < record.firstParam ||
      range.entry.drawParamIndex - record.firstParam >
          entry.entry.command.drawParams.size() ||
      range.drawEntryCount >
          entry.entry.command.drawParams.size() -
              (range.entry.drawParamIndex - record.firstParam)) {
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
          .drawParams = entry.entry.command.drawParams.subspan(
              range.entry.drawParamIndex - record.firstParam,
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
  ResolvedEncodePartition partition{};
  return next(range, partition);
}

bool EncodePartitionIdentityCursor::next(
    EncodePartitionRangeSnapshot& range,
    ResolvedEncodePartition& partition) noexcept {
  if (!stream_ || !stream_->valid ||
      replayOrdinal_ >= stream_->replayOrdinalCount()) {
    return false;
  }

  if (pendingDrawRun_) {
    range = pendingRange_;
    partition = pendingPartition_;
    pendingDrawRun_ = false;
    ++replayOrdinal_;
    return true;
  }

  std::uint32_t commandIndex = 0;
  if (stream_->commandIndexAt(replayOrdinal_, commandIndex)) {
    const auto sourceCommand =
        stream_->source.payload.commandAt(commandIndex);
    const auto& command = sourceCommand.command;
    EncodePartitionEntrySnapshot entry{};
    ResolvedEncodePartitionEntry resolved{};
    if (command.kind == core::MetalCommandKind::DrawRun &&
        command.drawRunRecord && command.drawRunRecord->paramCount != 0u &&
        snapshotForResolvedDrawParam(
            *stream_, commandIndex, sourceCommand,
            command.drawRunRecord->firstParam, entry, resolved)) {
      range = EncodePartitionRangeSnapshot{
          .kind = EncodePartitionRangeKind::DrawRunEntries,
          .replayOrdinalBegin = static_cast<std::uint32_t>(replayOrdinal_),
          .replayOrdinalCount = 1u,
          .drawEntryCount = command.drawRunRecord->paramCount,
          .entry = entry,
      };
      partition = ResolvedEncodePartition{
          .entry = resolved,
          .drawParams = command.drawParams,
      };
      ++replayOrdinal_;
      return true;
    }
  }

  const std::size_t begin = replayOrdinal_++;
  while (replayOrdinal_ < stream_->replayOrdinalCount()) {
    std::uint32_t nextCommandIndex = 0;
    if (stream_->commandIndexAt(replayOrdinal_, nextCommandIndex)) {
      const auto nextSourceCommand =
          stream_->source.payload.commandAt(nextCommandIndex);
      const auto& next = nextSourceCommand.command;
      EncodePartitionEntrySnapshot entry{};
      ResolvedEncodePartitionEntry resolved{};
      if (next.kind == core::MetalCommandKind::DrawRun &&
          next.drawRunRecord && next.drawRunRecord->paramCount != 0u &&
          snapshotForResolvedDrawParam(
              *stream_, nextCommandIndex, nextSourceCommand,
              next.drawRunRecord->firstParam, entry, resolved)) {
        pendingRange_ = EncodePartitionRangeSnapshot{
            .kind = EncodePartitionRangeKind::DrawRunEntries,
            .replayOrdinalBegin =
                static_cast<std::uint32_t>(replayOrdinal_),
            .replayOrdinalCount = 1u,
            .drawEntryCount = next.drawRunRecord->paramCount,
            .entry = entry,
        };
        pendingPartition_ = ResolvedEncodePartition{
            .entry = resolved,
            .drawParams = next.drawParams,
        };
        pendingDrawRun_ = true;
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
  partition = {};
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
    if (!identityCursor_.next(identityRange_, identityPartition_)) {
      return false;
    }
    batch = EncodePartitionSerialBatch{
        .kind = identityRange_.kind,
        .replayOrdinalBegin = identityRange_.replayOrdinalBegin,
        .replayOrdinalCount = identityRange_.replayOrdinalCount,
        .ranges = std::span<const EncodePartitionRangeSnapshot>(
            &identityRange_, 1u),
        .identityResolved =
            identityRange_.kind == EncodePartitionRangeKind::DrawRunEntries,
        .identityPartition = identityPartition_,
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
      .identityResolved = false,
  };
  return true;
}

}  // namespace dxmt9::encoders
