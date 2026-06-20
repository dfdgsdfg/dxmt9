#pragma once

#include "dag_observer.hpp"

#include "../dxmt9_draw_encoder.hpp"
#include "../dxmt9_queue.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>

namespace dxmt9::render {

bool slotIsPresentOnlyTail(const core::ChunkSlot& slot) noexcept;

bool slotHasFinalPresentTail(const core::ChunkSlot& slot) noexcept;

bool slotIsOpenCbPreencodeHead(const core::ChunkSlot& slot) noexcept;

bool slotCanAppendToOpenCbPending(const core::ChunkSlot& slot,
                                  bool carryRenderSession,
                                  bool hasPendingSession) noexcept;

bool slotCanStartOpenCbPendingSession(const core::ChunkSlot& slot,
                                      bool carryRenderSession) noexcept;

bool openCbPendingAllowsSemanticMidChunkCommits(
    bool appendToPending) noexcept;

enum class OpenCbSemanticBoundaryReleaseMode : std::uint8_t {
  CompletionWait,
  Deterministic,
};

bool openCbPendingCanReleaseAtSemanticBoundary(
    bool sourceIsSemanticBoundary,
    bool sourceHasFinalPresentTail,
    OpenCbSemanticBoundaryReleaseMode mode,
    bool completionWaitActive,
    bool semanticReleaseAlreadyUsedDuringWait) noexcept;

bool openCbPendingShouldReleaseBeforeReadySource(
    bool readySlotsEmpty,
    bool canReleaseAtSemanticBoundary,
    OpenCbSemanticBoundaryReleaseMode mode,
    bool completionWaitActive,
    bool semanticReleaseAlreadyUsedDuringWait) noexcept;

bool openCbPendingShouldSubmitBeforeInitializerWait(
    bool canAppendToPending,
    bool pendingSessionHasActiveRender,
    bool initializerHasPendingUploads) noexcept;

bool openCbPendingCompletionWaitTransitionNeedsRecheck(
    bool completionWaitActive,
    bool waitObservedCompletionWaitActive,
    OpenCbSemanticBoundaryReleaseMode mode,
    bool canReleaseAtSemanticBoundary,
    bool semanticReleaseAlreadyUsedDuringWait) noexcept;

enum class OpenCbPendingTailWaitAction : std::uint8_t {
  None,
  WaitForReady,
  SubmitPending,
};

OpenCbPendingTailWaitAction selectOpenCbPendingTailWaitAction(
    bool hasPendingRecord,
    bool readySlotsEmpty,
    bool stopRequested,
    bool writerActive,
    bool timeoutEnabled) noexcept;

bool canCoalesceTailPresentBatch(
    std::span<const core::metalqueue::ReadySlotSnapshot> sources) noexcept;

std::size_t selectTailPresentBatchPrefix(
    const std::deque<std::size_t>& readySlots,
    std::span<const core::ChunkSlot> slots,
    std::size_t maxCount) noexcept;

std::size_t selectOpenCbTailPresentBatchPrefix(
    const std::deque<std::size_t>& readySlots,
    std::span<const core::ChunkSlot> slots,
    std::size_t maxCount) noexcept;

std::optional<core::metalqueue::QueueSubmissionRecord> encodeTailPresentBatch(
    encoders::EncodeContext& ctx,
    std::span<core::metalqueue::ReadySlotSnapshot> sources,
    DagObserver& observer);

}  // namespace dxmt9::render
