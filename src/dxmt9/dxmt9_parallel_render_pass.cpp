#include "dxmt9_parallel_render_pass.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace dxmt9::encoders {
namespace {

using Fallback = SealedParallelPassSnapshotFallback;
using Kind = core::MetalCommandKind;

constexpr std::size_t fallbackIndex(Fallback fallback) noexcept {
  return static_cast<std::size_t>(fallback);
}

SealedParallelPassSnapshotBatchResult rejectBatch(
    Fallback fallback,
    SealedParallelPassSnapshotBatch& snapshots) noexcept {
  snapshots.reset();
  SealedParallelPassSnapshotBatchResult result{
      .fallback = fallback,
      .considered = true,
  };
  if (fallback != Fallback::None && fallback != Fallback::Count) {
    result.rejectionCounts[fallbackIndex(fallback)] = 1u;
  }
  return result;
}

void noteRejection(SealedParallelPassSnapshotBatchResult& result,
                   Fallback fallback) noexcept {
  if (fallback == Fallback::None || fallback == Fallback::Count) {
    return;
  }
  ++result.rejectionCounts[fallbackIndex(fallback)];
  if (result.fallback == Fallback::None) {
    result.fallback = fallback;
  }
}

bool appendResources(core::ExactResourceSet& destination,
                     const core::ExactResourceSet& source) noexcept {
  if (!destination.complete() || !source.complete()) {
    destination.flags &= ~core::ExactResourceSetComplete;
    return false;
  }
  for (std::uint32_t i = 0; i < source.count; ++i) {
    if (!destination.add(source.handles[i])) {
      return false;
    }
  }
  return destination.complete();
}

core::RenderAttachmentKey attachmentKeyForClear(
    const core::ClearCommandView& clear) noexcept {
  core::RenderAttachmentKey key{
      .color = clear.colorAttachments,
      .depthStencil = clear.depthStencil,
  };
  for (const auto& attachment : key.color) {
    key.sampleCount = std::max(key.sampleCount, attachment.sampleCount);
  }
  key.sampleCount = std::max(key.sampleCount,
                             key.depthStencil.sampleCount);
  return key;
}

ParallelPassCommandLocator commandLocator(
    const EncodePartitionReplayStream& stream,
    std::size_t replayOrdinal,
    std::uint32_t commandIndex,
    Kind kind) noexcept {
  if (replayOrdinal > std::numeric_limits<std::uint32_t>::max()) {
    return {};
  }
  return ParallelPassCommandLocator{
      .source = RetainedEncodeSourceLocator{
          .tapeSource = stream.source.source,
          .retainedSourceIndex = 0u,
          .slotIndex = static_cast<std::uint32_t>(stream.source.slotIndex),
          .seqId = stream.source.seqId,
      },
      .replayOrdinal = static_cast<std::uint32_t>(replayOrdinal),
      .commandIndex = commandIndex,
      .kind = kind,
      .valid = stream.source.source.valid() && stream.source.seqId != 0u &&
          stream.source.slotIndex <= std::numeric_limits<std::uint32_t>::max(),
  };
}

bool isAllowedCoordinatorBoundary(Kind kind) noexcept {
  return kind == Kind::Clear || kind == Kind::Present;
}

}  // namespace

SealedParallelPassSnapshotBatchResult produceSealedParallelPassSnapshots(
    const SealedParallelPassSnapshotInput& input,
    SealedParallelPassSnapshotBatch& snapshots) noexcept {
  snapshots.reset();
  if (!input.stream || input.ranges.empty()) {
    return rejectBatch(Fallback::PlanMissing, snapshots);
  }
  if (!input.planValidated) {
    return rejectBatch(Fallback::PlanNotValidated, snapshots);
  }
  const auto& stream = *input.stream;
  if (!stream.valid || !stream.source.source.valid() ||
      stream.source.seqId == 0u || !stream.source.payload.valid() ||
      stream.replayOrdinalCount() == 0u ||
      stream.replayOrdinalCount() >
          std::numeric_limits<std::uint32_t>::max()) {
    return rejectBatch(Fallback::ReplayInvalid, snapshots);
  }
  if (input.firstPassActionEpoch == 0u) {
    return rejectBatch(Fallback::PassActionEpoch, snapshots);
  }
  if (input.hasQuery || input.hasUpdateTexture || input.hasOrderedControl ||
      input.hasInitializerWait || input.hasSidecarObservation) {
    return rejectBatch(Fallback::CoordinatorCommand, snapshots);
  }

  // Query, UpdateTexture, and ordered controls are normally excluded before a
  // SourcePayloadView exists. Any other helper command that does reach this
  // stream is still a source-wide fail-closed observation input. Clear and
  // Present are the two coordinator-owned pass boundaries this producer can
  // reason about without executing them.
  for (std::size_t ordinal = 0;
       ordinal < stream.replayOrdinalCount(); ++ordinal) {
    std::uint32_t commandIndex = 0;
    if (!stream.commandIndexAt(ordinal, commandIndex)) {
      return rejectBatch(Fallback::ReplayInvalid, snapshots);
    }
    const Kind kind = stream.source.payload.commandAt(commandIndex).kind();
    if (kind != Kind::DrawRun && !isAllowedCoordinatorBoundary(kind)) {
      return rejectBatch(Fallback::CoordinatorCommand, snapshots);
    }
  }

  SealedParallelPassSnapshotBatchResult result{
      .fallback = Fallback::None,
      .considered = true,
  };
  SealedParallelPassSnapshot current{};
  ParallelPassCommandLocator pendingClear{};
  core::RenderAttachmentKey pendingClearAttachments{};
  std::size_t rangeIndex = 0u;
  std::uint64_t passActionEpoch = input.firstPassActionEpoch;
  bool passActionEpochValid = true;
  bool active = false;
  bool startProven = input.sourceStartsPass;
  bool attachmentKnown = false;
  bool candidateRejected = false;
  Fallback candidateFallback = Fallback::None;

  auto rejectCandidate = [&](Fallback fallback) {
    if (!candidateRejected) {
      candidateRejected = true;
      candidateFallback = fallback;
    }
  };
  auto advancePassActionEpoch = [&]() {
    if (!passActionEpochValid) {
      return;
    }
    if (passActionEpoch == std::numeric_limits<std::uint64_t>::max()) {
      passActionEpochValid = false;
      return;
    }
    ++passActionEpoch;
  };
  auto resetCandidate = [&]() {
    current.reset();
    active = false;
    attachmentKnown = false;
    candidateRejected = false;
    candidateFallback = Fallback::None;
  };
  auto startCandidate = [&](std::size_t ordinal,
                            std::uint32_t commandIndex,
                            const core::RenderAttachmentKey& attachments) {
    resetCandidate();
    active = true;
    attachmentKnown = true;
    current.source = stream.source.source;
    current.seqId = stream.source.seqId;
    const bool leadingClearMatches =
        pendingClear.valid && pendingClearAttachments == attachments;
    if (pendingClear.valid && !leadingClearMatches) {
      advancePassActionEpoch();
    }
    current.passActionEpoch = passActionEpoch;
    current.replayOrdinalBegin = static_cast<std::uint32_t>(ordinal);
    current.attachments = attachments;
    current.firstDraw = commandLocator(
        stream, ordinal, commandIndex, Kind::DrawRun);
    if (leadingClearMatches) {
      current.leadingClear = pendingClear;
    }
    pendingClear = {};
    if (!startProven) {
      rejectCandidate(Fallback::UnsealedStart);
    }
    if (!passActionEpochValid || current.passActionEpoch == 0u) {
      rejectCandidate(Fallback::PassActionEpoch);
    }
  };
  auto sealCandidate = [&](std::uint32_t replayOrdinalEnd,
                           ParallelPassCommandLocator sealingCommand,
                           bool sealedAtSourceEnd) {
    if (!active) {
      return;
    }
    ++result.candidateCount;
    current.replayOrdinalEnd = replayOrdinalEnd;
    current.sealingCommand = sealingCommand;
    current.sealedAtSourceEnd = sealedAtSourceEnd;
    if (!startProven) {
      rejectCandidate(Fallback::UnsealedStart);
    }
    if (replayOrdinalEnd <= current.replayOrdinalBegin) {
      rejectCandidate(Fallback::UnsealedEnd);
    }
    if (!candidateRejected) {
      ++result.sealedCount;
      if (current.childCount < 2u) {
        rejectCandidate(Fallback::TooFewChildren);
      } else if (current.childCount > kParallelRenderPassChildCapacity) {
        rejectCandidate(Fallback::ChildCapacity);
      }
    }
    if (!candidateRejected &&
        snapshots.count >= snapshots.passes.size()) {
      rejectCandidate(Fallback::PassCapacity);
    }
    if (candidateRejected) {
      noteRejection(result, candidateFallback);
    } else {
      snapshots.passes[snapshots.count++] = current;
      ++result.eligibleCount;
      result.childCount += current.childCount;
      result.drawCount += current.drawCount;
      result.childCountMax = std::max(result.childCountMax,
                                      current.childCount);
      result.drawCountMax = std::max(result.drawCountMax,
                                     current.drawCount);
    }
    resetCandidate();
  };

  for (std::size_t ordinal = 0;
       ordinal < stream.replayOrdinalCount(); ++ordinal) {
    std::uint32_t commandIndex = 0;
    if (!stream.commandIndexAt(ordinal, commandIndex)) {
      return rejectBatch(Fallback::ReplayInvalid, snapshots);
    }
    const auto source = stream.source.payload.commandAt(commandIndex);
    const Kind kind = source.kind();
    const auto locator = commandLocator(stream, ordinal, commandIndex, kind);

    if (kind == Kind::Clear) {
      const bool endedPass = active;
      sealCandidate(static_cast<std::uint32_t>(ordinal), locator, false);
      if (!source.clear.has_value() || !locator.valid) {
        return rejectBatch(Fallback::ReplayInvalid, snapshots);
      }
      if (endedPass || pendingClear.valid) {
        advancePassActionEpoch();
      }
      if (source.clear->rects.empty()) {
        pendingClear = locator;
        pendingClearAttachments = attachmentKeyForClear(*source.clear);
      } else {
        pendingClear = {};
        advancePassActionEpoch();
      }
      startProven = true;
      continue;
    }
    if (kind == Kind::Present) {
      sealCandidate(static_cast<std::uint32_t>(ordinal), locator, false);
      pendingClear = {};
      startProven = true;
      advancePassActionEpoch();
      continue;
    }
    if (kind != Kind::DrawRun || !source.command.drawState.hot ||
        !source.command.drawState.shaderLayout ||
        !source.command.drawState.debug ||
        !source.command.drawRunRecord || source.command.drawParams.empty() ||
        !locator.valid) {
      return rejectBatch(Fallback::ReplayInvalid, snapshots);
    }

    const auto attachments =
        core::makeRenderAttachmentKey(*source.command.drawState.hot);
    if (active && attachmentKnown && attachments != current.attachments) {
      sealCandidate(static_cast<std::uint32_t>(ordinal), locator, false);
      startProven = true;
      advancePassActionEpoch();
    }
    if (!active) {
      startCandidate(ordinal, commandIndex, attachments);
    }

    const auto writes =
        core::makeRenderAttachmentWriteSet(*source.command.drawState.hot);
    const auto reads = core::makeDrawEntryReadSet(source.command.drawState);
    if (!writes.complete() || !reads.complete() ||
        (current.attachmentWrites.count != 0u &&
         writes != current.attachmentWrites)) {
      rejectCandidate(writes != current.attachmentWrites &&
                              current.attachmentWrites.count != 0u
                          ? Fallback::AttachmentMismatch
                          : Fallback::ResourceSetIncomplete);
    }
    if (current.attachmentWrites.count == 0u) {
      current.attachmentWrites = writes;
    }
    if (!appendResources(current.resourceReads, reads)) {
      rejectCandidate(Fallback::ResourceSetIncomplete);
    }
    if (current.attachmentWrites.overlaps(reads)) {
      rejectCandidate(Fallback::ResourceHazard);
    }
    if (source.command.drawParams.size() >
        std::numeric_limits<std::uint64_t>::max() - current.drawCount) {
      rejectCandidate(Fallback::FirstDrawSnapshot);
    } else {
      current.drawCount += source.command.drawParams.size();
    }

    while (rangeIndex < input.ranges.size()) {
      const auto& range = input.ranges[rangeIndex];
      const std::uint64_t rangeEnd =
          static_cast<std::uint64_t>(range.replayOrdinalBegin) +
          range.replayOrdinalCount;
      if (rangeEnd > ordinal) {
        break;
      }
      ++rangeIndex;
    }
    const std::size_t drawRangeBegin = rangeIndex;
    if (rangeIndex < input.ranges.size() &&
        input.ranges[rangeIndex].kind ==
            EncodePartitionRangeKind::CommandSegment &&
        input.ranges[rangeIndex].replayOrdinalBegin <= ordinal &&
        static_cast<std::uint64_t>(
            input.ranges[rangeIndex].replayOrdinalBegin) +
                input.ranges[rangeIndex].replayOrdinalCount > ordinal) {
      rejectCandidate(Fallback::NonChildDrawRun);
    } else {
      while (rangeIndex < input.ranges.size() &&
             input.ranges[rangeIndex].kind ==
                 EncodePartitionRangeKind::DrawRunEntries &&
             input.ranges[rangeIndex].replayOrdinalBegin == ordinal) {
        const auto& range = input.ranges[rangeIndex++];
        if (current.childCount >= kParallelRenderPassChildCapacity) {
          rejectCandidate(Fallback::ChildCapacity);
          continue;
        }
        const auto resolved = resolveEncodePartition(range, stream);
        if (!resolved || !resolved.partition.entry.drawState.hot ||
            !resolved.partition.entry.drawState.shaderLayout ||
            !resolved.partition.entry.drawState.debug ||
            !resolved.partition.entry.drawParam ||
            !resolved.partition.entry.uniform ||
            resolved.partition.drawParams.empty()) {
          rejectCandidate(Fallback::FirstDrawSnapshot);
          continue;
        }
        current.ranges[current.childCount] = range;
        current.firstDraws[current.childCount] = ParallelFirstDrawSnapshot{
            .provenance = range.entry,
            .entryRender = core::RenderContinuationKey{
                .attachments = attachments,
                .entryReads = reads,
                .route = core::RenderRoute::Unknown,
                .passActionEpoch = current.passActionEpoch,
                .flags = core::RenderContinuationKeyValid |
                         core::RenderContinuationEntryStateComplete,
            },
            .generation = stream.source.seqId,
            .complete = stream.source.seqId != 0u && reads.complete(),
        };
        ++current.childCount;
      }
      if (rangeIndex == drawRangeBegin) {
        rejectCandidate(Fallback::NonChildDrawRun);
      }
    }
  }

  if (active) {
    if (input.sourceEndsPass) {
      sealCandidate(static_cast<std::uint32_t>(stream.replayOrdinalCount()),
                    {}, true);
    } else {
      rejectCandidate(Fallback::UnsealedEnd);
      sealCandidate(static_cast<std::uint32_t>(stream.replayOrdinalCount()),
                    {}, false);
    }
  }
  result.eligibleCountMax = result.eligibleCount;
  return result;
}

}  // namespace dxmt9::encoders
