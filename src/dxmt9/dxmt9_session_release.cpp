#include "dxmt9_session_release.hpp"

#include <algorithm>

namespace dxmt9::core::metalqueue {

bool sessionReleaseFenceCovered(const SessionReleaseEvent& event,
                                std::uint64_t coveredRawOrdinal,
                                std::uint64_t coveredSeqId) noexcept {
  return event.valid() &&
         (event.fenceRawOrdinal == 0 ||
          coveredRawOrdinal >= event.fenceRawOrdinal) &&
         (event.fenceSeqId == 0 || coveredSeqId >= event.fenceSeqId);
}

bool sessionReleaseCompletionSatisfies(
    SessionReleaseAction action,
    SessionReleaseCompletion completion) noexcept {
  switch (action) {
  case SessionReleaseAction::ClosePass:
    return completion == SessionReleaseCompletion::PassClosed ||
           completion == SessionReleaseCompletion::SessionSubmitted;
  case SessionReleaseAction::SubmitSession:
  case SessionReleaseAction::SubmitAndWait:
    // SubmitAndWait acknowledgement deliberately stops at submission. The
    // posting thread observes GPU completion through its sequence wait.
    return completion == SessionReleaseCompletion::SessionSubmitted;
  }
  return false;
}

bool SessionReleaseState::ordinaryReason(
    SessionReleaseReason reason) noexcept {
  switch (reason) {
  case SessionReleaseReason::Shutdown:
  case SessionReleaseReason::DeviceLoss:
    return false;
  default:
    return true;
  }
}

bool SessionReleaseState::terminalReason(
    SessionReleaseReason reason) noexcept {
  return reason == SessionReleaseReason::Shutdown ||
         reason == SessionReleaseReason::DeviceLoss;
}

bool SessionReleaseState::fenceNotBefore(
    std::uint64_t rawOrdinal,
    std::uint64_t seqId,
    std::uint64_t priorRawOrdinal,
    std::uint64_t priorSeqId) noexcept {
  return rawOrdinal >= priorRawOrdinal && seqId >= priorSeqId;
}

std::uint64_t SessionReleaseState::allocateOrdinal() noexcept {
  if (nextOrdinal_ == 0 ||
      nextOrdinal_ == std::numeric_limits<std::uint64_t>::max()) {
    return 0;
  }
  return nextOrdinal_++;
}

SessionReleaseSnapshot SessionReleaseState::snapshotFor(
    const Latch& latch,
    SessionReleaseOrigin origin) const noexcept {
  if (!latch.active) {
    return {};
  }
  return SessionReleaseSnapshot{
      .origin = origin,
      .event = latch.event,
      .generation = latch.generation,
  };
}

SessionReleasePostResult SessionReleaseState::postQueued(
    SessionReleaseReason reason,
    SessionReleaseAction action,
    std::uint64_t fenceRawOrdinal,
    std::uint64_t fenceSeqId) noexcept {
  if (orderedFull()) {
    return {.status = SessionReleasePostStatus::Full};
  }
  const std::uint64_t ordinal = allocateOrdinal();
  if (ordinal == 0) {
    return {.status = SessionReleasePostStatus::Exhausted};
  }
  const SessionReleaseEvent event{
      .ordinal = ordinal,
      .reason = reason,
      .action = action,
      .fenceRawOrdinal = fenceRawOrdinal,
      .fenceSeqId = fenceSeqId,
  };
  ordered_[orderedTail_] = event;
  orderedTail_ = nextIndex(orderedTail_);
  ++orderedCount_;
  lastFenceRawOrdinal_ = fenceRawOrdinal;
  lastFenceSeqId_ = fenceSeqId;
  return {
      .status = SessionReleasePostStatus::Posted,
      .snapshot = SessionReleaseSnapshot{
          .origin = SessionReleaseOrigin::Ordered,
          .event = event,
          .generation = 1,
      },
  };
}

SessionReleasePostResult SessionReleaseState::tryPostOrdered(
    SessionReleaseReason reason,
    SessionReleaseAction action,
    std::uint64_t fenceRawOrdinal,
    std::uint64_t fenceSeqId) noexcept {
  if (terminalRequested_) {
    return {.status = SessionReleasePostStatus::Stopped};
  }
  if (!ordinaryReason(reason) ||
      !fenceNotBefore(fenceRawOrdinal, fenceSeqId,
                      lastFenceRawOrdinal_, lastFenceSeqId_)) {
    return {.status = SessionReleasePostStatus::Invalid};
  }
  return postQueued(reason, action, fenceRawOrdinal, fenceSeqId);
}

SessionReleasePostResult SessionReleaseState::postOrAdvanceLatch(
    Latch& latch,
    SessionReleaseOrigin origin,
    SessionReleaseReason reason,
    SessionReleaseAction action,
    std::uint64_t fenceRawOrdinal,
    std::uint64_t fenceSeqId) noexcept {
  if (!fenceNotBefore(fenceRawOrdinal, fenceSeqId,
                      lastFenceRawOrdinal_, lastFenceSeqId_)) {
    return {.status = SessionReleasePostStatus::Invalid};
  }
  if (latch.active) {
    if (latch.generation == std::numeric_limits<std::uint64_t>::max()) {
      return {.status = SessionReleasePostStatus::Exhausted};
    }
    ++latch.generation;
    latch.event.fenceRawOrdinal =
        std::max(latch.event.fenceRawOrdinal, fenceRawOrdinal);
    latch.event.fenceSeqId =
        std::max(latch.event.fenceSeqId, fenceSeqId);
    if (reason == SessionReleaseReason::DeviceLoss) {
      latch.event.reason = reason;
    }
    lastFenceRawOrdinal_ = fenceRawOrdinal;
    lastFenceSeqId_ = fenceSeqId;
    return {
        .status = SessionReleasePostStatus::Advanced,
        .snapshot = snapshotFor(latch, origin),
    };
  }

  const std::uint64_t ordinal = allocateOrdinal();
  if (ordinal == 0) {
    return {.status = SessionReleasePostStatus::Exhausted};
  }
  latch.event = SessionReleaseEvent{
      .ordinal = ordinal,
      .reason = reason,
      .action = action,
      .fenceRawOrdinal = fenceRawOrdinal,
      .fenceSeqId = fenceSeqId,
  };
  latch.generation = 1;
  latch.active = true;
  lastFenceRawOrdinal_ = fenceRawOrdinal;
  lastFenceSeqId_ = fenceSeqId;
  return {
      .status = SessionReleasePostStatus::Posted,
      .snapshot = snapshotFor(latch, origin),
  };
}

SessionReleasePostResult SessionReleaseState::requestTerminal(
    SessionReleaseReason reason,
    std::uint64_t fenceRawOrdinal,
    std::uint64_t fenceSeqId) noexcept {
  if (!terminalReason(reason)) {
    return {.status = SessionReleasePostStatus::Invalid};
  }
  if (terminalRequested_ && !terminal_.active) {
    return {.status = SessionReleasePostStatus::Stopped};
  }
  // Terminal requests are sticky control state, not optional observations.
  // Unknown (zero) or stale caller fences therefore inherit the most recent
  // globally ordered waterline instead of making Shutdown/DeviceLoss lossy.
  fenceRawOrdinal = std::max(fenceRawOrdinal, lastFenceRawOrdinal_);
  fenceSeqId = std::max(fenceSeqId, lastFenceSeqId_);
  const auto result = postOrAdvanceLatch(
      terminal_, SessionReleaseOrigin::Terminal, reason,
      SessionReleaseAction::SubmitSession, fenceRawOrdinal, fenceSeqId);
  if (result.accepted()) {
    terminalRequested_ = true;
  }
  return result;
}

std::optional<SessionReleaseSnapshot>
SessionReleaseState::peekNext() const noexcept {
  std::optional<SessionReleaseSnapshot> next;
  const auto consider = [&next](SessionReleaseSnapshot candidate) {
    if (!candidate.valid()) {
      return;
    }
    if (!next || candidate.event.ordinal < next->event.ordinal) {
      next = candidate;
    }
  };
  if (!orderedEmpty()) {
    consider(SessionReleaseSnapshot{
        .origin = SessionReleaseOrigin::Ordered,
        .event = ordered_[orderedHead_],
        .generation = 1,
    });
  }
  consider(snapshotFor(terminal_, SessionReleaseOrigin::Terminal));
  return next;
}

SessionReleaseAckResult SessionReleaseState::acknowledge(
    const SessionReleaseSnapshot& snapshot,
    SessionReleaseCompletion completion,
    std::uint64_t coveredRawOrdinal,
    std::uint64_t coveredSeqId) noexcept {
  if (!snapshot.valid()) {
    return SessionReleaseAckResult::Invalid;
  }
  const auto next = peekNext();
  if (!next || next->origin != snapshot.origin ||
      next->event.ordinal != snapshot.event.ordinal) {
    return SessionReleaseAckResult::NotNext;
  }
  if (next->generation != snapshot.generation ||
      next->event != snapshot.event) {
    return SessionReleaseAckResult::Stale;
  }
  if (!sessionReleaseFenceCovered(next->event, coveredRawOrdinal,
                                  coveredSeqId)) {
    return SessionReleaseAckResult::FenceNotCovered;
  }
  if (!sessionReleaseCompletionSatisfies(next->event.action, completion)) {
    return SessionReleaseAckResult::ActionNotCompleted;
  }

  switch (next->origin) {
  case SessionReleaseOrigin::Ordered:
    ordered_[orderedHead_] = {};
    orderedHead_ = nextIndex(orderedHead_);
    --orderedCount_;
    break;
  case SessionReleaseOrigin::Terminal:
    terminal_ = {};
    break;
  }
  acknowledgedOrdinal_ = next->event.ordinal;
  return SessionReleaseAckResult::Acknowledged;
}

bool SessionReleaseState::hasPending() const noexcept {
  return !orderedEmpty() || terminal_.active;
}

}  // namespace dxmt9::core::metalqueue
