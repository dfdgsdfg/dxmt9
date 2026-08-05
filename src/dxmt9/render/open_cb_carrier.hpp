#pragma once

#include "../dxmt9_backend_types.hpp"
#include "../dxmt9_queue.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

// H229 open-CB overlap carrier — pure decision helpers.
//
// This is a fresh, minimal re-implementation of the H157-H188 open-CB
// tail-Present carrier family removed in 6379d5c8, with the H183-proven
// configuration baked in instead of the retired 12-knob env family:
//   * render-session carry is always on (EncodeChunkSessionState owner),
//   * semantic/attachment boundary publication is always on,
//   * pending work releases at completion waits AND producer waits
//     (f1fb1b04's lesson), with the CompletionWait release mode,
//   * same-key draw-continuation publication stays removed (H183's
//     decisive difference from H182),
//   * no wallclock release timeout.
// The only runtime gate is DXMT9_OPEN_CB_CARRIER, resolved read-once in
// dxmt9_command_queue.cpp. See specs/backend/gap.md R-BACK-2.39/2.43 and
// docs/perfomance/present-pacing/log.md H183-H189/H227-H229.

namespace dxmt9::render {

// Chunk classification -------------------------------------------------

// Final chunk of a frame: exactly one Present record and Present is the
// last command. Appending this into a pending open CB closes the carrier.
bool openCbCarrierSlotHasFinalPresentTail(const core::ChunkSlot& slot) noexcept;

// A chunk the carrier may keep the command buffer open across: non-empty
// and present-free.
bool openCbCarrierSlotCanBeSessionHead(const core::ChunkSlot& slot) noexcept;

// Whether a dequeued ready source can append into the pending open CB.
// Present tails always append (they close the carrier); other sources
// require an already-active carry session and a session-head shape.
bool openCbCarrierSlotCanAppendToPending(const core::ChunkSlot& slot,
                                         bool hasPendingSession) noexcept;

// Producer-side publication --------------------------------------------

// Present tail split: with the carrier on, non-empty pre-present work is
// published as its own PresentSplitBefore chunk so the encode thread can
// treat it as an appendable head and the Present-only tail as the closer.
bool openCbCarrierPresentTailNeedsPrePresentSplit(
    bool carrierEnabled,
    bool hasCurrentPrePresentWork) noexcept;

// Attachment identity used for the draw-source boundary publish. A change
// of color/depth attachment key (or max sample count) at the next draw is
// a semantic render-pass boundary worth publishing to the encode thread.
bool openCbCarrierDrawAttachmentKeysMatch(
    const core::FlatDrawStateRecord& lhs,
    const core::FlatDrawStateRecord& rhs) noexcept;

// Wait-start CPU-ready publication: with the encoder idle (no ready
// sources, no pending open CB) during an active completion wait, the
// producer's writing slot (non-empty, present-free) is published so the
// wait window gets CPU-ready work.
bool openCbCarrierShouldPublishWaitStartSlot(bool readySlotsEmpty,
                                             bool hasPendingRecord,
                                             bool completionWaitActive,
                                             bool stopRequested,
                                             bool writerActive,
                                             bool writingSlotEmpty,
                                             bool writingSlotHasPresent) noexcept;

// Active-wait CPU-ready publication: a releasable pending open CB exists,
// no ready source is visible, and the completion wait is active — publish
// the producer's non-empty present-free writing slot so the pending CB can
// append it instead of releasing early.
bool openCbCarrierShouldPublishActiveWaitSlot(
    bool readySlotsEmpty,
    bool pendingCanReleaseAtSemanticBoundary,
    bool completionWaitActive,
    bool semanticReleaseUsedDuringWait,
    bool writerActive,
    bool writingSlotEmpty,
    bool writingSlotHasPresent) noexcept;

// Pending release decisions (CompletionWait mode baked) ------------------

// A pending open CB whose head was a semantic-boundary chunk may release
// once per completion wait while that wait is active.
bool openCbCarrierCanReleasePendingAtSemanticBoundary(
    bool pendingCanReleaseAtSemanticBoundary,
    bool completionWaitActive,
    bool semanticReleaseUsedDuringWait) noexcept;

// When a ready source arrived while a releasable pending open CB exists,
// prefer appending the source into the pending CB over releasing it —
// keeping the carrier open extends the single-CB span (H183 shape).
bool openCbCarrierShouldAppendReadyBeforeRelease(
    bool pendingCanReleaseAtSemanticBoundary,
    bool completionWaitActive,
    bool semanticReleaseUsedDuringWait,
    bool firstReadySourceCanAppendToPending) noexcept;

// f1fb1b04: pending open-CB work must also release while the PRODUCER is
// blocked on a sequence wait (Lock/Map), not only during completion waits;
// otherwise the producer deadlocks against its own unsubmitted work.
bool openCbCarrierShouldSubmitForProducerWait(
    bool hasPendingRecord,
    bool producerSequenceWaitActive) noexcept;

// The resource initializer cannot upload while the carrier holds an active
// render session open; submit the pending CB before the initializer wait.
bool openCbCarrierShouldSubmitBeforeInitializerWait(
    bool sourceCanAppendToPending,
    bool pendingSessionHasActiveRender,
    bool initializerHasPendingUploads) noexcept;

// Completion-wait transitions observed while blocked on the encode CV need
// a loop recheck: a wait became active with an unreleased releasable
// pending CB, or the observed wait ended (resets the once-per-wait latch).
bool openCbCarrierWaitTransitionNeedsRecheck(
    bool completionWaitActive,
    bool waitObservedCompletionWaitActive,
    bool pendingCanReleaseAtSemanticBoundary,
    bool semanticReleaseUsedDuringWait) noexcept;

enum class OpenCbCarrierPendingWaitAction : std::uint8_t {
  None,
  WaitForReady,
  SubmitPending,
};

// With a pending open CB and no ready source: submit on stop or inactive
// writer (fail-open drain), otherwise wait for more work.
OpenCbCarrierPendingWaitAction selectOpenCbCarrierPendingWaitAction(
    bool hasPendingRecord,
    bool readySlotsEmpty,
    bool stopRequested,
    bool writerActive) noexcept;

// Source-kind-neutral classification -------------------------------------
//
// Payload-view twins of the ChunkSlot predicates above. They classify a
// published source by its immutable command stream regardless of whether the
// payload is a legacy ChunkSlot or an Arena SourcePayloadBlock, so the
// Tape-gated session lane can admit both kinds into one FIFO prefix.
bool openCbCarrierSourceHasFinalPresentTail(
    core::SourcePayloadView payload) noexcept;
bool openCbCarrierSourceCanBeSessionHead(
    core::SourcePayloadView payload) noexcept;
bool openCbCarrierSourceCanAppendToPending(core::SourcePayloadView payload,
                                           bool hasPendingSession) noexcept;

// Encode-side prefix selection -------------------------------------------

// FIFO prefix for dequeueReadySlotBatchPrefix: either a run of appendable
// session heads terminated by a final Present tail (head..tail carrier), or
// the maximal run of session heads with no tail visible yet. Returns 0 to
// fall back to single-source dequeue.
std::size_t selectOpenCbCarrierBatchPrefix(
    std::span<const core::metalqueue::ResolvedPublishedSource> candidates) noexcept;

// Source-kind-neutral prefix selector for the Tape-gated session lane. Same
// head..tail shape as selectOpenCbCarrierBatchPrefix but admits Legacy and
// Arena payloads into the same compatible prefix.
std::size_t selectCpuReadySessionBatchPrefix(
    std::span<const core::metalqueue::ResolvedPublishedSource> candidates) noexcept;

}  // namespace dxmt9::render
