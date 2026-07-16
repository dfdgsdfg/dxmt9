#include "open_cb_carrier.hpp"

#include <algorithm>

namespace dxmt9::render {

bool openCbCarrierSlotHasFinalPresentTail(const core::ChunkSlot& slot) noexcept {
  return !slot.commandHeaders.empty() &&
         slot.commandHeaders.back().kind == core::MetalCommandKind::Present &&
         slot.presentRecords.size() == 1u;
}

bool openCbCarrierSlotCanBeSessionHead(const core::ChunkSlot& slot) noexcept {
  return !slot.commandsEmpty() && slot.presentRecords.empty();
}

bool openCbCarrierSlotCanAppendToPending(const core::ChunkSlot& slot,
                                         bool hasPendingSession) noexcept {
  if (openCbCarrierSlotHasFinalPresentTail(slot)) {
    return true;
  }
  if (!hasPendingSession) {
    return false;
  }
  return openCbCarrierSlotCanBeSessionHead(slot);
}

bool openCbCarrierPresentTailNeedsPrePresentSplit(
    bool carrierEnabled,
    bool hasCurrentPrePresentWork) noexcept {
  return carrierEnabled && hasCurrentPrePresentWork;
}

namespace {

std::uint32_t drawAttachmentMaxSampleCount(
    const core::FlatDrawStateRecord& hot) noexcept {
  std::uint32_t sampleCount = 1;
  for (const auto& attachment : hot.colorAttachments) {
    sampleCount = std::max(sampleCount, attachment.sampleCount);
  }
  sampleCount = std::max(sampleCount, hot.depthStencil.sampleCount);
  return sampleCount;
}

}  // namespace

bool openCbCarrierDrawAttachmentKeysMatch(
    const core::FlatDrawStateRecord& lhs,
    const core::FlatDrawStateRecord& rhs) noexcept {
  for (std::size_t i = 0; i < core::kMaxRenderTargets; ++i) {
    if (lhs.colorAttachments[i].handle != rhs.colorAttachments[i].handle) {
      return false;
    }
  }
  return lhs.depthStencil.handle == rhs.depthStencil.handle &&
         drawAttachmentMaxSampleCount(lhs) == drawAttachmentMaxSampleCount(rhs);
}

bool openCbCarrierShouldPublishWaitStartSlot(bool readySlotsEmpty,
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

bool openCbCarrierShouldPublishActiveWaitSlot(
    bool readySlotsEmpty,
    bool pendingCanReleaseAtSemanticBoundary,
    bool completionWaitActive,
    bool semanticReleaseUsedDuringWait,
    bool writerActive,
    bool writingSlotEmpty,
    bool writingSlotHasPresent) noexcept {
  return readySlotsEmpty &&
         pendingCanReleaseAtSemanticBoundary &&
         completionWaitActive &&
         !semanticReleaseUsedDuringWait &&
         writerActive &&
         !writingSlotEmpty &&
         !writingSlotHasPresent;
}

bool openCbCarrierCanReleasePendingAtSemanticBoundary(
    bool pendingCanReleaseAtSemanticBoundary,
    bool completionWaitActive,
    bool semanticReleaseUsedDuringWait) noexcept {
  return pendingCanReleaseAtSemanticBoundary &&
         completionWaitActive &&
         !semanticReleaseUsedDuringWait;
}

bool openCbCarrierShouldAppendReadyBeforeRelease(
    bool pendingCanReleaseAtSemanticBoundary,
    bool completionWaitActive,
    bool semanticReleaseUsedDuringWait,
    bool firstReadySourceCanAppendToPending) noexcept {
  return firstReadySourceCanAppendToPending &&
         openCbCarrierCanReleasePendingAtSemanticBoundary(
             pendingCanReleaseAtSemanticBoundary,
             completionWaitActive,
             semanticReleaseUsedDuringWait);
}

bool openCbCarrierShouldSubmitForProducerWait(
    bool hasPendingRecord,
    bool producerSequenceWaitActive) noexcept {
  return hasPendingRecord && producerSequenceWaitActive;
}

bool openCbCarrierShouldSubmitBeforeInitializerWait(
    bool sourceCanAppendToPending,
    bool pendingSessionHasActiveRender,
    bool initializerHasPendingUploads) noexcept {
  return sourceCanAppendToPending &&
         pendingSessionHasActiveRender &&
         initializerHasPendingUploads;
}

bool openCbCarrierWaitTransitionNeedsRecheck(
    bool completionWaitActive,
    bool waitObservedCompletionWaitActive,
    bool pendingCanReleaseAtSemanticBoundary,
    bool semanticReleaseUsedDuringWait) noexcept {
  if (completionWaitActive &&
      pendingCanReleaseAtSemanticBoundary &&
      !semanticReleaseUsedDuringWait) {
    return true;
  }
  return waitObservedCompletionWaitActive && !completionWaitActive;
}

OpenCbCarrierPendingWaitAction selectOpenCbCarrierPendingWaitAction(
    bool hasPendingRecord,
    bool readySlotsEmpty,
    bool stopRequested,
    bool writerActive) noexcept {
  if (!hasPendingRecord || !readySlotsEmpty) {
    return OpenCbCarrierPendingWaitAction::None;
  }
  if (stopRequested || !writerActive) {
    return OpenCbCarrierPendingWaitAction::SubmitPending;
  }
  return OpenCbCarrierPendingWaitAction::WaitForReady;
}

std::size_t selectOpenCbCarrierBatchPrefix(
    const std::deque<std::size_t>& readySlots,
    std::span<const core::ChunkSlot> slots,
    std::size_t maxCount) noexcept {
  if (maxCount == 0u || readySlots.empty()) {
    return 0;
  }

  const std::size_t limit = std::min(maxCount, readySlots.size());
  std::size_t sessionHeadPrefix = 0;
  for (std::size_t i = 0; i < limit; ++i) {
    const std::size_t slotIndex = readySlots[i];
    if (slotIndex >= slots.size()) {
      return 0;
    }

    const auto& slot = slots[slotIndex];
    if (openCbCarrierSlotHasFinalPresentTail(slot)) {
      return i == 0u ? 0u : i + 1u;
    }
    if (!openCbCarrierSlotCanBeSessionHead(slot)) {
      return 0;
    }
    ++sessionHeadPrefix;
  }

  return sessionHeadPrefix;
}

}  // namespace dxmt9::render
