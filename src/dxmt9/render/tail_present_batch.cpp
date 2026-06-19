#include "tail_present_batch.hpp"

#include "../dxmt9_perf_counters.hpp"

#include <algorithm>
#include <chrono>
#include <vector>

namespace dxmt9::render {

bool slotIsPresentOnlyTail(const core::ChunkSlot& slot) noexcept {
  return slot.commandHeaders.size() == 1u &&
         slot.commandHeaders.front().kind == core::MetalCommandKind::Present &&
         slot.presentRecords.size() == 1u;
}

bool slotIsOpenCbPreencodeHead(const core::ChunkSlot& slot) noexcept {
  return slot.publishReason == perf::ChunkPublishReason::PresentSplitBefore &&
         !slot.commandsEmpty() &&
         slot.presentRecords.empty();
}

bool canCoalesceTailPresentBatch(
    std::span<const core::metalqueue::ReadySlotSnapshot> sources) noexcept {
  if (sources.size() < 2u) {
    return false;
  }

  const auto& tail = sources.back().slot;
  if (!slotIsPresentOnlyTail(tail)) {
    return false;
  }
  for (const auto& source : sources.first(sources.size() - 1u)) {
    const auto& head = source.slot;
    if (head.commandsEmpty() ||
        !head.presentRecords.empty() ||
        slotIsPresentOnlyTail(head)) {
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
    if (slotIsPresentOnlyTail(slot)) {
      return i == 0u ? 0u : i + 1u;
    }
    if (slot.commandsEmpty() || !slot.presentRecords.empty()) {
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

  const auto& tail = sources.back();
  const auto& present = tail.slot.presentRecords.front();

  auto& combined = sources.front().slot;
  combined.seqId = tail.slot.seqId;
  combined.state = core::ChunkSlot::State::Encoding;
  combined.pipelinePrefetchSealed = false;
  combined.pipelinePrefetchCommandCursor = 0;
  for (const auto& head : sources.subspan(1u, sources.size() - 2u)) {
    if (!combined.appendCommandsFrom(head.slot)) {
      return std::nullopt;
    }
  }
  combined.appendPresent(present.present, present.presentSource);
  // Match the normal single-source encode path after the tail Present is
  // appended; otherwise every draw in the combined slot rebuilds pipeline state.
  const auto prefetchStarted = std::chrono::steady_clock::now();
  ctx.queue.prefetchSlotPipelines(combined);
  perf::countEncodeSlotPsoPrefetchCpuTime(static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - prefetchStarted)
          .count()));

  observer.observeAndExport(combined);
  auto submission = encoders::encodeChunk(ctx, tail.slotIndex, combined);
  if (submission.has_value()) {
    submission->slotIndex = tail.slotIndex;
    submission->seqId = tail.slot.seqId;
    if (submission->completionSources.empty()) {
      submission->completionSources = std::move(completionSources);
    }
  }
  return submission;
}

}  // namespace dxmt9::render
