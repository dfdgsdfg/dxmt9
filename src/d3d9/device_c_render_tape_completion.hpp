#pragma once

// Cold, bounded completion control for fixed-input Render Tape experiments.
//
// This header deliberately owns no queue, Metal, or resource state.  It is a
// value reducer used by an experiment adapter: the submit/release callbacks
// supplied by a caller must perform the real production operation.  Keeping
// admission here makes malformed schedules fail before the callback can have
// an effect, while keeping lifetime decisions in QueueLifecycleController and
// resources::Pool.

#include <array>
#include <cstddef>
#include <cstdint>

namespace dxmt9::d3d9 {

inline constexpr std::size_t kRenderTapeCompletionScheduleCapacity = 32u;
inline constexpr std::size_t kRenderTapeCompletionSubmissionCapacity = 32u;

struct RenderTapeCompletionKey {
  std::uint64_t iteration = 0;
  std::uint64_t submission = 0;
  std::uint32_t source = 0;
  std::uint32_t actionOrdinal = 0;
  // Queue source generation is part of the qualified identity. Zero is kept
  // as an exact model-only value for the small reducer traces; production
  // adapters must populate it from QueueCompletionSource::source.id.
  std::uint64_t sourceGeneration = 0;

  friend constexpr bool operator==(const RenderTapeCompletionKey&, const RenderTapeCompletionKey&) = default;
};

struct RenderTapeCompletionAction {
  RenderTapeCompletionKey key{};

  friend constexpr bool operator==(const RenderTapeCompletionAction&, const RenderTapeCompletionAction&) = default;
};

enum class RenderTapeCompletionDisposition : std::uint8_t {
  Accepted,
  Capacity,
  InvalidKey,
  DuplicateAction,
  ReleaseBeforeSubmit,
  DuplicateRelease,
  Stale,
  SkippedFrontier,
  Exhausted,
  Unresolved,
};

struct RenderTapeCompletionResult {
  RenderTapeCompletionDisposition disposition =
      RenderTapeCompletionDisposition::Accepted;

  constexpr bool accepted() const noexcept {
    return disposition == RenderTapeCompletionDisposition::Accepted;
  }
};

// A fixed schedule is intentionally a small value.  An experiment receipt
// authenticates the input and schedule separately; this object only enforces
// the temporal predicates that are shared by the native adapter and its
// bounded reducer/model.
class RenderTapeCompletionSchedule final {
 public:
  using Key = RenderTapeCompletionKey;
  using Action = RenderTapeCompletionAction;
  using Result = RenderTapeCompletionResult;

  constexpr RenderTapeCompletionSchedule() = default;

  Result append(Action action) noexcept {
    const auto key = action.key;
    if (activeIteration_ != 0u || frontier_ != 0u) {
      return {RenderTapeCompletionDisposition::Stale};
    }
    if (actionCount_ == actions_.size()) {
      return {RenderTapeCompletionDisposition::Capacity};
    }
    if (key.iteration == 0u || key.submission == 0u ||
        key.actionOrdinal != actionCount_ ||
        (actionCount_ != 0u &&
         key.iteration < actions_[actionCount_ - 1u].key.iteration)) {
      return {RenderTapeCompletionDisposition::InvalidKey};
    }
    for (std::size_t i = 0; i < actionCount_; ++i) {
      if (sameIdentity(actions_[i].key, key)) {
        return {RenderTapeCompletionDisposition::DuplicateAction};
      }
    }
    actions_[actionCount_++] = action;
    return {};
  }

  constexpr std::size_t size() const noexcept { return actionCount_; }
  constexpr std::size_t nextAction() const noexcept { return frontier_; }
  constexpr bool exhausted() const noexcept {
    return frontier_ == actionCount_;
  }
  constexpr const Action& action(std::size_t index) const noexcept {
    return actions_[index];
  }

  // Iteration identity is explicit even when a caller uses one schedule for
  // more than one fixed-input repetition.  This prevents an old receipt from
  // being accepted after reset/resource reuse.
  Result beginIteration(std::uint64_t iteration) noexcept {
    if (failed_ || iteration == 0u || activeIteration_ != 0u ||
        submittedCount_ != 0u || iteration <= lastCompletedIteration_) {
      return {RenderTapeCompletionDisposition::Stale};
    }
    if (frontier_ >= actionCount_ ||
        actions_[frontier_].key.iteration != iteration) {
      return {RenderTapeCompletionDisposition::Stale};
    }
    activeIteration_ = iteration;
    iterationEnd_ = frontier_ + 1u;
    while (iterationEnd_ < actionCount_ &&
           actions_[iterationEnd_].key.iteration == iteration) {
      ++iterationEnd_;
    }
    submitFrontier_ = frontier_;
    return {};
  }

  template <typename Submit>
  Result submit(Key key, Submit&& submitCallback) {
    const auto admitted = submit(key);
    if (!admitted.accepted()) {
      return admitted;
    }
    if (!submitCallback()) {
      --submittedCount_;
      failed_ = true;
      return {RenderTapeCompletionDisposition::Unresolved};
    }
    return admitted;
  }

  Result submit(Key key) noexcept {
    if (failed_) {
      return {RenderTapeCompletionDisposition::Unresolved};
    }
    if (activeIteration_ == 0u || key.iteration != activeIteration_ ||
        key.submission == 0u) {
      return {RenderTapeCompletionDisposition::Stale};
    }
    for (std::size_t i = 0; i < submittedCount_; ++i) {
      if (submitted_[i].key == key) {
        return {RenderTapeCompletionDisposition::DuplicateAction};
      }
    }
    if (submitFrontier_ == iterationEnd_) {
      return {RenderTapeCompletionDisposition::Exhausted};
    }
    if (actions_[submitFrontier_].key != key) {
      for (std::size_t i = submitFrontier_ + 1u; i < iterationEnd_; ++i) {
        if (sameIdentity(actions_[i].key, key)) {
          return {RenderTapeCompletionDisposition::SkippedFrontier};
        }
      }
      return {RenderTapeCompletionDisposition::Stale};
    }
    if (submittedCount_ == submitted_.size()) {
      return {RenderTapeCompletionDisposition::Capacity};
    }
    submitted_[submittedCount_++] = Submission{key, false};
    ++submitFrontier_;
    return {};
  }

  // A runner uses this when its input contains one more completion request
  // after the authenticated action list.  It has no production callback and
  // exists so exhaustion is a named result rather than an implicit drain.
  Result releaseNext() const noexcept {
    if (failed_) {
      return {RenderTapeCompletionDisposition::Unresolved};
    }
    if (activeIteration_ == 0u) {
      return {RenderTapeCompletionDisposition::Stale};
    }
    return {frontier_ == iterationEnd_
                ? RenderTapeCompletionDisposition::Exhausted
                : RenderTapeCompletionDisposition::ReleaseBeforeSubmit};
  }

  template <typename Release>
  Result release(Key key, Release&& releaseCallback) {
    const auto admitted = release(key);
    if (!admitted.accepted()) {
      return admitted;
    }
    if (!releaseCallback()) {
      // A failed production acknowledgment is terminal for this adapter. It
      // is not rolled back as a successful completion; the caller retains the
      // consumed frontier and can report the executed prefix.
      failed_ = true;
      return {RenderTapeCompletionDisposition::Unresolved};
    }
    return admitted;
  }

  Result release(Key key) noexcept {
    if (failed_) {
      return {RenderTapeCompletionDisposition::Unresolved};
    }
    if (activeIteration_ == 0u || key.iteration != activeIteration_) {
      return {RenderTapeCompletionDisposition::Stale};
    }
    for (std::size_t i = 0; i < submittedCount_; ++i) {
      if (submitted_[i].key == key) {
        if (submitted_[i].released) {
          return {RenderTapeCompletionDisposition::DuplicateRelease};
        }
        break;
      }
      if (i + 1u == submittedCount_) {
        return {RenderTapeCompletionDisposition::ReleaseBeforeSubmit};
      }
    }
    if (frontier_ == iterationEnd_) {
      return {RenderTapeCompletionDisposition::Exhausted};
    }
    if (actions_[frontier_].key != key) {
      for (std::size_t i = frontier_ + 1u; i < iterationEnd_; ++i) {
        if (actions_[i].key == key) {
          return {RenderTapeCompletionDisposition::SkippedFrontier};
        }
      }
      return {RenderTapeCompletionDisposition::Stale};
    }
    for (std::size_t i = 0; i < submittedCount_; ++i) {
      if (submitted_[i].key == key) {
        submitted_[i].released = true;
        ++frontier_;
        return {};
      }
    }
    return {RenderTapeCompletionDisposition::ReleaseBeforeSubmit};
  }

  Result finishIteration() noexcept {
    if (activeIteration_ == 0u) {
      return {RenderTapeCompletionDisposition::Stale};
    }
    if (failed_ || frontier_ != iterationEnd_ ||
        submitFrontier_ != iterationEnd_) {
      return {RenderTapeCompletionDisposition::Unresolved};
    }
    for (std::size_t i = 0; i < submittedCount_; ++i) {
      if (!submitted_[i].released) {
        return {RenderTapeCompletionDisposition::Unresolved};
      }
    }
    lastCompletedIteration_ = activeIteration_;
    activeIteration_ = 0u;
    submittedCount_ = 0u;
    submitFrontier_ = frontier_;
    return {};
  }

 private:
  static constexpr bool sameIdentity(const Key& lhs,
                                     const Key& rhs) noexcept {
    return lhs.iteration == rhs.iteration &&
           lhs.submission == rhs.submission && lhs.source == rhs.source &&
           lhs.sourceGeneration == rhs.sourceGeneration;
  }

  struct Submission {
    Key key{};
    bool released = false;
  };

  std::array<Action, kRenderTapeCompletionScheduleCapacity> actions_{};
  std::array<Submission, kRenderTapeCompletionSubmissionCapacity> submitted_{};
  std::size_t actionCount_ = 0;
  std::size_t submittedCount_ = 0;
  std::size_t frontier_ = 0;
  std::size_t submitFrontier_ = 0;
  std::size_t iterationEnd_ = 0;
  std::uint64_t activeIteration_ = 0;
  std::uint64_t lastCompletedIteration_ = 0;
  bool failed_ = false;
};

}  // namespace dxmt9::d3d9
