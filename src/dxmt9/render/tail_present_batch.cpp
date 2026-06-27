#include "tail_present_batch.hpp"

#include "../dxmt9_perf_counters.hpp"

#include <algorithm>
#include <chrono>
#include <utility>
#include <vector>

namespace dxmt9::render {

bool slotIsPresentOnlyTail(const core::ChunkSlot& slot) noexcept {
  return slot.commandHeaders.size() == 1u &&
         slot.commandHeaders.front().kind == core::MetalCommandKind::Present &&
         slot.presentRecords.size() == 1u;
}

bool slotHasFinalPresentTail(const core::ChunkSlot& slot) noexcept {
  return !slot.commandHeaders.empty() &&
         slot.commandHeaders.back().kind == core::MetalCommandKind::Present &&
         slot.presentRecords.size() == 1u;
}

bool slotIsOpenCbPreencodeHead(const core::ChunkSlot& slot) noexcept {
  return slot.publishReason == perf::ChunkPublishReason::PresentSplitBefore &&
         !slot.commandsEmpty() &&
         slot.presentRecords.empty();
}

bool slotCanBeOpenCbSessionHead(const core::ChunkSlot& slot) noexcept {
  return !slot.commandsEmpty() && slot.presentRecords.empty();
}

bool slotIsSemanticOpenCbSessionHead(
    const core::ChunkSlot& slot) noexcept {
  return slot.publishReason == perf::ChunkPublishReason::SemanticBoundary &&
         slotCanBeOpenCbSessionHead(slot);
}

bool slotCanAppendToOpenCbPending(const core::ChunkSlot& slot,
                                  bool carryRenderSession,
                                  bool hasPendingSession,
                                  bool tailReadyForCurrentHead) noexcept {
  if (slotHasFinalPresentTail(slot)) {
    return true;
  }
  if (!hasPendingSession) {
    return false;
  }
  if (!carryRenderSession) {
    return slotIsOpenCbPreencodeHead(slot);
  }
  if (tailReadyForCurrentHead) {
    return slotCanBeOpenCbSessionHead(slot);
  }
  return slotIsSemanticOpenCbSessionHead(slot);
}

bool slotCanStartOpenCbPendingSession(const core::ChunkSlot& slot,
                                      bool carryRenderSession,
                                      bool tailReadyForCurrentHead) noexcept {
  if (!slotCanBeOpenCbSessionHead(slot)) {
    return false;
  }
  if (!carryRenderSession) {
    return slotIsOpenCbPreencodeHead(slot);
  }
  if (tailReadyForCurrentHead) {
    return true;
  }
  return slotIsSemanticOpenCbSessionHead(slot);
}

bool openCbPresentTailNeedsPrePresentSplit(
    bool openCbPreencodeTailPresent,
    bool hasCurrentPrePresentWork) noexcept {
  return openCbPreencodeTailPresent && hasCurrentPrePresentWork;
}

bool openCbPendingAllowsSemanticMidChunkCommits(
    bool appendToPending) noexcept {
  // Open-CB pending carriers must not turn source boundaries into extra
  // command buffers, but semantic pass/barrier boundaries should still follow
  // the normal mid-chunk chain policy. This preserves the single-publish
  // command-buffer shape while the session owner keeps a compatible render
  // encoder open across sources.
  static_cast<void>(appendToPending);
  return true;
}

bool openCbPendingCanReleaseAtSemanticBoundary(
    bool sourceIsSemanticBoundary,
    bool sourceHasFinalPresentTail,
    OpenCbSemanticBoundaryReleaseMode mode,
    bool completionWaitActive,
    bool semanticReleaseAlreadyUsedDuringWait) noexcept {
  if (!sourceIsSemanticBoundary || sourceHasFinalPresentTail) {
    return false;
  }
  if (mode == OpenCbSemanticBoundaryReleaseMode::Deterministic) {
    return true;
  }
  return completionWaitActive && !semanticReleaseAlreadyUsedDuringWait;
}

bool openCbPendingShouldReleaseBeforeReadySource(
    bool readySlotsEmpty,
    bool canReleaseAtSemanticBoundary,
    OpenCbSemanticBoundaryReleaseMode mode,
    bool completionWaitActive,
    bool semanticReleaseAlreadyUsedDuringWait) noexcept {
  if (readySlotsEmpty || !canReleaseAtSemanticBoundary) {
    return false;
  }
  return openCbPendingCanReleaseAtSemanticBoundary(
      /*sourceIsSemanticBoundary=*/true,
      /*sourceHasFinalPresentTail=*/false,
      mode,
      completionWaitActive,
      semanticReleaseAlreadyUsedDuringWait);
}

bool openCbPendingShouldAppendReadySourceBeforeSemanticRelease(
    bool readySlotsEmpty,
    bool canReleaseAtSemanticBoundary,
    OpenCbSemanticBoundaryReleaseMode mode,
    bool completionWaitActive,
    bool semanticReleaseAlreadyUsedDuringWait,
    bool firstReadySourceCanAppendToPending) noexcept {
  return mode == OpenCbSemanticBoundaryReleaseMode::CompletionWait &&
         completionWaitActive &&
         !semanticReleaseAlreadyUsedDuringWait &&
         firstReadySourceCanAppendToPending &&
         openCbPendingShouldReleaseBeforeReadySource(
             readySlotsEmpty,
             canReleaseAtSemanticBoundary,
             mode,
             completionWaitActive,
             semanticReleaseAlreadyUsedDuringWait);
}

bool openCbPendingReadySourceBlocksSemanticReleaseNoCompletionWait(
    bool readySlotsEmpty,
    bool canReleaseAtSemanticBoundary,
    OpenCbSemanticBoundaryReleaseMode mode,
    bool completionWaitActive) noexcept {
  return !readySlotsEmpty &&
         canReleaseAtSemanticBoundary &&
         mode == OpenCbSemanticBoundaryReleaseMode::CompletionWait &&
         !completionWaitActive;
}

OpenCbSemanticReleaseNoCompletionWaitBlock
classifyOpenCbPendingSemanticReleaseNoCompletionWaitBlock(
    bool readySlotsEmpty,
    bool canReleaseAtSemanticBoundary,
    OpenCbSemanticBoundaryReleaseMode mode,
    bool completionWaitActive,
    bool writerActive) noexcept {
  if (!readySlotsEmpty ||
      !canReleaseAtSemanticBoundary ||
      mode != OpenCbSemanticBoundaryReleaseMode::CompletionWait ||
      completionWaitActive) {
    return OpenCbSemanticReleaseNoCompletionWaitBlock::None;
  }
  return writerActive
      ? OpenCbSemanticReleaseNoCompletionWaitBlock::WriterActive
      : OpenCbSemanticReleaseNoCompletionWaitBlock::WriterInactive;
}

OpenCbSemanticReleaseWriterActiveSlotState
classifyOpenCbPendingSemanticReleaseWriterActiveSlotState(
    OpenCbSemanticReleaseNoCompletionWaitBlock block,
    bool writingSlotEmpty,
    bool writingSlotHasPresent) noexcept {
  if (block != OpenCbSemanticReleaseNoCompletionWaitBlock::WriterActive) {
    return OpenCbSemanticReleaseWriterActiveSlotState::None;
  }
  if (writingSlotEmpty) {
    return OpenCbSemanticReleaseWriterActiveSlotState::Empty;
  }
  return writingSlotHasPresent
      ? OpenCbSemanticReleaseWriterActiveSlotState::PresentBearing
      : OpenCbSemanticReleaseWriterActiveSlotState::NonPresentWork;
}

bool openCbPendingShouldCpuReadyPublishWriterActiveSlot(
    bool readySlotsEmpty,
    bool canReleaseAtSemanticBoundary,
    OpenCbSemanticBoundaryReleaseMode mode,
    bool completionWaitActive,
    bool writerActive,
    bool writingSlotEmpty,
    bool writingSlotHasPresent) noexcept {
  const auto block = classifyOpenCbPendingSemanticReleaseNoCompletionWaitBlock(
      readySlotsEmpty, canReleaseAtSemanticBoundary, mode,
      completionWaitActive, writerActive);
  return classifyOpenCbPendingSemanticReleaseWriterActiveSlotState(
             block, writingSlotEmpty, writingSlotHasPresent) ==
         OpenCbSemanticReleaseWriterActiveSlotState::NonPresentWork;
}

bool openCbPendingShouldCpuReadyPublishActiveWaitSlot(
    bool readySlotsEmpty,
    bool canReleaseAtSemanticBoundary,
    OpenCbSemanticBoundaryReleaseMode mode,
    bool completionWaitActive,
    bool semanticReleaseAlreadyUsedDuringWait,
    bool writerActive,
    bool writingSlotEmpty,
    bool writingSlotHasPresent) noexcept {
  return readySlotsEmpty &&
         canReleaseAtSemanticBoundary &&
         mode == OpenCbSemanticBoundaryReleaseMode::CompletionWait &&
         completionWaitActive &&
         !semanticReleaseAlreadyUsedDuringWait &&
         writerActive &&
         !writingSlotEmpty &&
         !writingSlotHasPresent;
}

bool openCbShouldCpuReadyPublishWaitStartSlot(
    bool readySlotsEmpty,
    bool hasPendingRecord,
    bool completionWaitActive,
    bool stopRequested,
    bool writerActive,
    bool writingSlotEmpty,
    bool writingSlotHasPresent) noexcept {
  return readySlotsEmpty &&
         !hasPendingRecord &&
         completionWaitActive &&
         !stopRequested &&
         writerActive &&
         !writingSlotEmpty &&
         !writingSlotHasPresent;
}

bool openCbPendingShouldSubmitBeforeInitializerWait(
    bool canAppendToPending,
    bool pendingSessionHasActiveRender,
    bool initializerHasPendingUploads) noexcept {
  return canAppendToPending &&
         pendingSessionHasActiveRender &&
         initializerHasPendingUploads;
}

bool openCbPendingCompletionWaitTransitionNeedsRecheck(
    bool completionWaitActive,
    bool waitObservedCompletionWaitActive,
    OpenCbSemanticBoundaryReleaseMode mode,
    bool canReleaseAtSemanticBoundary,
    bool semanticReleaseAlreadyUsedDuringWait) noexcept {
  if (mode == OpenCbSemanticBoundaryReleaseMode::Deterministic) {
    return false;
  }
  if (completionWaitActive &&
      canReleaseAtSemanticBoundary &&
      !semanticReleaseAlreadyUsedDuringWait) {
    return true;
  }
  return waitObservedCompletionWaitActive && !completionWaitActive;
}

OpenCbPendingTailWaitAction selectOpenCbPendingTailWaitAction(
    bool hasPendingRecord,
    bool readySlotsEmpty,
    bool stopRequested,
    bool writerActive,
    bool timeoutEnabled) noexcept {
  if (!hasPendingRecord || !readySlotsEmpty) {
    return OpenCbPendingTailWaitAction::None;
  }
  static_cast<void>(timeoutEnabled);
  if (stopRequested || !writerActive) {
    return OpenCbPendingTailWaitAction::SubmitPending;
  }
  return OpenCbPendingTailWaitAction::WaitForReady;
}

bool canCoalesceTailPresentBatch(
    std::span<const core::metalqueue::ReadySlotSnapshot> sources) noexcept {
  if (sources.size() < 2u) {
    return false;
  }

  if (!sources.back().slot) {
    return false;
  }
  const auto& tail = *sources.back().slot;
  if (!slotHasFinalPresentTail(tail)) {
    return false;
  }
  for (const auto& source : sources.first(sources.size() - 1u)) {
    if (!source.slot) {
      return false;
    }
    const auto& head = *source.slot;
    if (head.commandsEmpty() ||
        !head.presentRecords.empty() ||
        slotHasFinalPresentTail(head)) {
      return false;
    }
  }
  return true;
}

std::size_t selectTailPresentBatchPrefix(
    const std::deque<std::size_t>& readySlots,
    std::span<const core::ChunkSlot> slots,
    std::size_t maxCount) noexcept {
  if (maxCount < 2u || readySlots.size() < 2u) {
    return 0;
  }

  const std::size_t limit = std::min(maxCount, readySlots.size());
  for (std::size_t i = 0; i < limit; ++i) {
    const std::size_t slotIndex = readySlots[i];
    if (slotIndex >= slots.size()) {
      return 0;
    }

    const auto& slot = slots[slotIndex];
    if (slotHasFinalPresentTail(slot)) {
      return i == 0u ? 0u : i + 1u;
    }
    if (slot.commandsEmpty() || !slot.presentRecords.empty()) {
      return 0;
    }
  }

  return 0;
}

std::size_t selectOpenCbTailPresentBatchPrefix(
    const std::deque<std::size_t>& readySlots,
    std::span<const core::ChunkSlot> slots,
    std::size_t maxCount) noexcept {
  if (maxCount < 2u || readySlots.size() < 2u) {
    return 0;
  }

  const std::size_t limit = std::min(maxCount, readySlots.size());
  for (std::size_t i = 0; i < limit; ++i) {
    const std::size_t slotIndex = readySlots[i];
    if (slotIndex >= slots.size()) {
      return 0;
    }

    const auto& slot = slots[slotIndex];
    if (slotHasFinalPresentTail(slot)) {
      return i == 0u ? 0u : i + 1u;
    }
    if (!slotCanBeOpenCbSessionHead(slot)) {
      return 0;
    }
  }

  return 0;
}

std::optional<core::metalqueue::QueueSubmissionRecord> encodeTailPresentBatch(
    encoders::EncodeContext& ctx,
    std::span<core::metalqueue::ReadySlotSnapshot> sources,
    DagObserver& observer) {
  if (!canCoalesceTailPresentBatch(sources)) {
    return std::nullopt;
  }

  std::vector<core::metalqueue::QueueCompletionSource> completionSources;
  completionSources.reserve(sources.size());
  for (const auto& source : sources) {
    completionSources.push_back(
        core::metalqueue::completionSourceForReadySlot(source));
  }

  auto session = encoders::makeEncodeChunkSession();
  std::optional<core::metalqueue::QueueSubmissionRecord> pending;
  auto finalizePendingPrefix = [&]() -> std::optional<core::metalqueue::QueueSubmissionRecord> {
    if (!pending.has_value() || !pending->commandBuffer) {
      return std::nullopt;
    }
    if (!encoders::finalizeEncodeChunkSessionIntoSubmission(
            ctx, *session, *pending)) {
      return std::nullopt;
    }
    if (!encoders::retainEncodeChunkSessionUntilSubmissionComplete(
            std::move(session), *pending)) {
      return std::nullopt;
    }
    return std::move(pending);
  };

  for (std::size_t i = 0; i < sources.size(); ++i) {
    auto& source = sources[i];
    if (!source.slot) {
      return std::nullopt;
    }
    auto& slot = *source.slot;

    // Match the normal single-source encode path. The old diagnostic batch
    // prefetched a copied aggregate slot; the session path keeps source
    // storage immutable and prefetches each live slot in place.
    if (!slot.prefetchedPipelinesSealed()) {
      const auto prefetchStarted = std::chrono::steady_clock::now();
      ctx.queue.prefetchSlotPipelines(slot);
      perf::countEncodeSlotPsoPrefetchCpuTime(static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - prefetchStarted)
              .count()));
    }

    observer.observeAndExport(slot);

    encoders::EncodeChunkOptions options{};
    options.allowInjectedCommandBufferMidChunkCommits =
        openCbPendingAllowsSemanticMidChunkCommits(
            pending.has_value());
    options.disablePresentAcquireSplit = true;
    options.session = session.get();
    options.deferSessionFinalization = i + 1u < sources.size();
    options.sessionSource = completionSources[i];
    options.sessionLookaheadSources = sources.subspan(i);
    if (pending.has_value()) {
      options.commandBuffer = pending->commandBuffer;
    }

    auto submission = encoders::encodeChunk(
        ctx, source.slotIndex, slot, std::move(options));
    if (!submission.has_value() || !submission->commandBuffer) {
      if (pending.has_value()) {
        return finalizePendingPrefix();
      }
      return std::nullopt;
    }
    pending = std::move(*submission);
  }

  if (!pending.has_value()) {
    return std::nullopt;
  }
  const auto& tail = sources.back();
  DXMT_ASSERT(tail.slot != nullptr);
  pending->slotIndex = tail.slotIndex;
  pending->seqId = tail.slot->seqId;
  if (pending->completionSources.empty()) {
    pending->completionSources = std::move(completionSources);
  }
  if (!encoders::retainEncodeChunkSessionUntilSubmissionComplete(
          std::move(session), *pending)) {
    return std::nullopt;
  }
  return pending;
}

}  // namespace dxmt9::render
