#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <type_traits>

namespace dxmt9::core::metalqueue {

inline constexpr std::size_t kMaxOrderedSessionReleaseEvents = 32;

enum class SessionReleaseReason : std::uint8_t {
  Present,
  ExplicitFlush,
  DirectObservation,
  ProducerSequenceWait,
  SessionCap,
  IndependentSubmission,
  InitializerWait,
  Shutdown,
  DeviceLoss,
};

enum class SessionReleaseAction : std::uint8_t {
  ClosePass,
  SubmitSession,
  SubmitAndWait,
};

// Ordered release identity. Acknowledgement means the requested pass/session
// action has happened; SubmitAndWait completion remains the posting thread's
// separate waitForSequence obligation.
struct SessionReleaseEvent {
  std::uint64_t ordinal = 0;
  SessionReleaseReason reason = SessionReleaseReason::Present;
  SessionReleaseAction action = SessionReleaseAction::ClosePass;
  std::uint64_t fenceRawOrdinal = 0;
  std::uint64_t fenceSeqId = 0;

  constexpr bool valid() const noexcept { return ordinal != 0; }

  friend constexpr bool operator==(SessionReleaseEvent,
                                   SessionReleaseEvent) noexcept = default;
};

static_assert(std::is_trivially_copyable_v<SessionReleaseEvent>);
static_assert(std::is_standard_layout_v<SessionReleaseEvent>);

enum class SessionReleaseOrigin : std::uint8_t {
  Ordered,
  Terminal,
};

struct SessionReleaseSnapshot {
  SessionReleaseOrigin origin = SessionReleaseOrigin::Ordered;
  SessionReleaseEvent event{};
  std::uint64_t generation = 0;

  constexpr bool valid() const noexcept {
    return event.valid() && generation != 0;
  }

  friend constexpr bool operator==(SessionReleaseSnapshot,
                                   SessionReleaseSnapshot) noexcept = default;
};

static_assert(std::is_trivially_copyable_v<SessionReleaseSnapshot>);
static_assert(std::is_standard_layout_v<SessionReleaseSnapshot>);

enum class SessionReleasePostStatus : std::uint8_t {
  Posted,
  Advanced,
  Full,
  Invalid,
  Stopped,
  Exhausted,
};

struct SessionReleasePostResult {
  SessionReleasePostStatus status = SessionReleasePostStatus::Invalid;
  SessionReleaseSnapshot snapshot{};

  constexpr bool accepted() const noexcept {
    return status == SessionReleasePostStatus::Posted ||
           status == SessionReleasePostStatus::Advanced;
  }
};

enum class SessionReleaseCompletion : std::uint8_t {
  PassClosed,
  SessionSubmitted,
};

enum class SessionReleaseAckResult : std::uint8_t {
  Acknowledged,
  Invalid,
  NotNext,
  Stale,
  FenceNotCovered,
  ActionNotCompleted,
};

bool sessionReleaseFenceCovered(const SessionReleaseEvent& event,
                                std::uint64_t coveredRawOrdinal,
                                std::uint64_t coveredSeqId) noexcept;
bool sessionReleaseCompletionSatisfies(
    SessionReleaseAction action,
    SessionReleaseCompletion completion) noexcept;

// Fixed-capacity, queue-lock-owned release transport. This class performs no
// locking and allocates no memory. Ordinary semantic/API and fixed-cap events
// retain strict FIFO identity. Only terminal events use a coalescing latch.
class SessionReleaseState {
 public:
  static constexpr std::size_t orderedCapacity() noexcept {
    return kMaxOrderedSessionReleaseEvents;
  }

  SessionReleasePostResult tryPostOrdered(
      SessionReleaseReason reason,
      SessionReleaseAction action,
      std::uint64_t fenceRawOrdinal,
      std::uint64_t fenceSeqId) noexcept;

  SessionReleasePostResult requestTerminal(
      SessionReleaseReason reason,
      std::uint64_t fenceRawOrdinal,
      std::uint64_t fenceSeqId) noexcept;

  std::optional<SessionReleaseSnapshot> peekNext() const noexcept;

  SessionReleaseAckResult acknowledge(
      const SessionReleaseSnapshot& snapshot,
      SessionReleaseCompletion completion,
      std::uint64_t coveredRawOrdinal,
      std::uint64_t coveredSeqId) noexcept;

  bool hasPending() const noexcept;
  bool orderedEmpty() const noexcept { return orderedCount_ == 0; }
  bool orderedFull() const noexcept {
    return orderedCount_ == ordered_.size();
  }
  std::size_t orderedSize() const noexcept { return orderedCount_; }
  bool terminalRequested() const noexcept { return terminalRequested_; }
  bool terminalPending() const noexcept { return terminal_.active; }
  std::uint64_t acknowledgedOrdinal() const noexcept {
    return acknowledgedOrdinal_;
  }

 private:
  struct Latch {
    SessionReleaseEvent event{};
    std::uint64_t generation = 0;
    bool active = false;
  };

  static constexpr std::size_t nextIndex(std::size_t index) noexcept {
    return (index + 1u) % kMaxOrderedSessionReleaseEvents;
  }

  static bool ordinaryReason(SessionReleaseReason reason) noexcept;
  static bool terminalReason(SessionReleaseReason reason) noexcept;
  static bool fenceNotBefore(std::uint64_t rawOrdinal,
                             std::uint64_t seqId,
                             std::uint64_t priorRawOrdinal,
                             std::uint64_t priorSeqId) noexcept;

  std::uint64_t allocateOrdinal() noexcept;
  SessionReleasePostResult postQueued(
      SessionReleaseReason reason,
      SessionReleaseAction action,
      std::uint64_t fenceRawOrdinal,
      std::uint64_t fenceSeqId) noexcept;
  SessionReleasePostResult postOrAdvanceLatch(
      Latch& latch,
      SessionReleaseOrigin origin,
      SessionReleaseReason reason,
      SessionReleaseAction action,
      std::uint64_t fenceRawOrdinal,
      std::uint64_t fenceSeqId) noexcept;
  SessionReleaseSnapshot snapshotFor(const Latch& latch,
                                     SessionReleaseOrigin origin) const noexcept;

  std::array<SessionReleaseEvent,
             kMaxOrderedSessionReleaseEvents> ordered_{};
  std::size_t orderedHead_ = 0;
  std::size_t orderedTail_ = 0;
  std::size_t orderedCount_ = 0;
  Latch terminal_{};
  std::uint64_t nextOrdinal_ = 1;
  std::uint64_t acknowledgedOrdinal_ = 0;
  std::uint64_t lastFenceRawOrdinal_ = 0;
  std::uint64_t lastFenceSeqId_ = 0;
  bool terminalRequested_ = false;
};

}  // namespace dxmt9::core::metalqueue
