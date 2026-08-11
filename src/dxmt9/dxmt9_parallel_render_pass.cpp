#include "dxmt9_parallel_render_pass.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>

namespace dxmt9::encoders {
namespace {

using Fallback = SealedParallelPassSnapshotFallback;

SealedParallelPassSnapshotResult rejectSnapshot(
    Fallback fallback, bool considered = true) noexcept {
  return SealedParallelPassSnapshotResult{
      .fallback = fallback,
      .considered = considered,
  };
}

bool rangeContainsOnlyFinalPresent(
    const EncodePartitionRangeSnapshot& range,
    const EncodePartitionReplayStream& stream) noexcept {
  if (range.kind != EncodePartitionRangeKind::CommandSegment ||
      range.replayOrdinalCount != 1u ||
      static_cast<std::size_t>(range.replayOrdinalBegin) >=
          stream.replayOrdinalCount() ||
      static_cast<std::size_t>(range.replayOrdinalBegin) + 1u !=
          stream.replayOrdinalCount()) {
    return false;
  }
  std::uint32_t commandIndex = 0;
  return stream.commandIndexAt(range.replayOrdinalBegin, commandIndex) &&
      stream.source.payload.commandAt(commandIndex).kind() ==
          core::MetalCommandKind::Present;
}

}  // namespace

SealedParallelPassSnapshotResult produceSealedParallelPassSnapshot(
    const SealedParallelPassSnapshotInput& input,
    SealedParallelPassSnapshot& snapshot) noexcept {
  snapshot.reset();
  if (!input.stream || input.ranges.empty()) {
    return rejectSnapshot(Fallback::PlanMissing);
  }
  if (!input.planValidated) {
    return rejectSnapshot(Fallback::PlanNotValidated);
  }
  const auto& stream = *input.stream;
  if (!stream.valid || !stream.source.source.valid() ||
      stream.source.seqId == 0u || !stream.source.payload.valid() ||
      stream.replayOrdinalCount() == 0u) {
    return rejectSnapshot(Fallback::ReplayInvalid);
  }
  if (!input.sourceStartsPass) {
    return rejectSnapshot(Fallback::UnsealedStart);
  }
  if (!input.sourceEndsPass) {
    return rejectSnapshot(Fallback::UnsealedEnd);
  }

  bool firstDraw = true;
  bool terminalPresent = false;
  std::uint64_t drawCount = 0u;
  std::uint32_t childCount = 0u;
  for (const auto& range : input.ranges) {
    if (range.kind == EncodePartitionRangeKind::CommandSegment) {
      if (terminalPresent || childCount == 0u ||
          !rangeContainsOnlyFinalPresent(range, stream)) {
        snapshot.reset();
        return rejectSnapshot(Fallback::CoordinatorCommand);
      }
      terminalPresent = true;
      continue;
    }
    if (terminalPresent) {
      snapshot.reset();
      return rejectSnapshot(Fallback::CoordinatorCommand);
    }
    if (childCount >= kParallelRenderPassChildCapacity) {
      snapshot.reset();
      return rejectSnapshot(Fallback::ChildCapacity);
    }
    const auto resolved = resolveEncodePartition(range, stream);
    if (!resolved || !resolved.partition.entry.drawState.hot ||
        !resolved.partition.entry.drawState.shaderLayout ||
        !resolved.partition.entry.drawState.debug ||
        !resolved.partition.entry.drawParam ||
        !resolved.partition.entry.uniform ||
        resolved.partition.drawParams.empty()) {
      snapshot.reset();
      return rejectSnapshot(Fallback::FirstDrawSnapshot);
    }

    const auto& hot = *resolved.partition.entry.drawState.hot;
    const auto attachments = core::makeRenderAttachmentKey(hot);
    const auto writes = core::makeRenderAttachmentWriteSet(hot);
    const auto reads = core::makeDrawEntryReadSet(
        resolved.partition.entry.drawState);
    if (!writes.complete() || !reads.complete()) {
      snapshot.reset();
      return rejectSnapshot(Fallback::FirstDrawSnapshot);
    }
    if (firstDraw) {
      snapshot.attachments = attachments;
      snapshot.attachmentWrites = writes;
      firstDraw = false;
    } else if (attachments != snapshot.attachments ||
               writes != snapshot.attachmentWrites) {
      snapshot.reset();
      return rejectSnapshot(Fallback::AttachmentMismatch);
    }
    if (snapshot.attachmentWrites.overlaps(reads)) {
      snapshot.reset();
      return rejectSnapshot(Fallback::ResourceHazard);
    }
    if (range.drawEntryCount >
        std::numeric_limits<std::uint64_t>::max() - drawCount) {
      snapshot.reset();
      return rejectSnapshot(Fallback::FirstDrawSnapshot);
    }

    snapshot.ranges[childCount] = range;
    snapshot.firstDraws[childCount] = ParallelFirstDrawSnapshot{
        .provenance = range.entry,
        .entryRender = core::RenderContinuationKey{
            .attachments = attachments,
            .entryReads = reads,
            .route = core::RenderRoute::Unknown,
            .passActionEpoch = 1u,
            .flags = core::RenderContinuationKeyValid |
                     core::RenderContinuationEntryStateComplete,
        },
        .generation = stream.source.seqId,
        .complete = stream.source.seqId != 0u,
    };
    drawCount += range.drawEntryCount;
    ++childCount;
  }

  if (childCount < 2u) {
    snapshot.reset();
    return rejectSnapshot(Fallback::TooFewChildren);
  }
  snapshot.source = stream.source.source;
  snapshot.seqId = stream.source.seqId;
  snapshot.drawCount = drawCount;
  snapshot.childCount = childCount;
  snapshot.hasTerminalPresent = terminalPresent;
  return SealedParallelPassSnapshotResult{
      .fallback = Fallback::None,
      .drawCount = drawCount,
      .childCount = childCount,
      .considered = true,
      .sealed = true,
      .eligible = true,
  };
}

}  // namespace dxmt9::encoders
