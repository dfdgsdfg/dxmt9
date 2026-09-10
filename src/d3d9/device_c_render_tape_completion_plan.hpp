#pragma once

// Fixed-input Render Tape completion-plan format and its cold runtime binder.
//
// The plan names immutable Tape event ordinals and event-local submission
// ordinals.  Queue sequence/source/storage identities are deliberately absent
// from the file: they do not exist until production replay publishes a real
// QueueCompletionSource.  RenderTapeCompletionPlanController binds those
// identities immediately before the caller's submit callback and owns the
// bounded submit/release frontiers thereafter.

#include "device_c_render_tape_completion.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <type_traits>

namespace dxmt9::d3d9 {

inline constexpr std::uint64_t kRenderTapeCompletionPlanMagic =
    0x31564e414c505844ull;  // "DXPLANV1"
inline constexpr std::uint32_t kRenderTapeCompletionPlanVersion = 1u;
inline constexpr std::size_t kRenderTapeCompletionPlanCapacity = 32u;
inline constexpr std::size_t kRenderTapeCompletionPlanDigestBytes = 32u;

enum class RenderTapeCompletionPlanRoute : std::uint32_t {
  Any = 0u,
  ProductionSlot = 1u,
  Compatibility = 2u,
};

enum class RenderTapeCompletionPlanDisposition : std::uint8_t {
  Accepted,
  InvalidSize,
  InvalidHeader,
  InvalidDigest,
  InvalidRoute,
  InvalidRepeat,
  InvalidCapacity,
  InvalidEvent,
  NonMonotonicSubmit,
  DuplicateSubmit,
  DuplicateRelease,
  MissingRelease,
  InvalidReleaseBoundary,
  RouteMismatch,
  StaleIteration,
  StaleEvent,
  UnexpectedSubmit,
  DuplicateSubmission,
  InvalidActualSource,
  ActualSourceOrder,
  ReleaseBeforeSubmit,
  SkippedReleaseFrontier,
  DuplicateReleaseAttempt,
  CallbackFailure,
  MissingSubmit,
  Exhausted,
  Unresolved,
};

struct RenderTapeCompletionPlanResult {
  RenderTapeCompletionPlanDisposition disposition =
      RenderTapeCompletionPlanDisposition::Accepted;
  std::uint32_t row = 0u;

  constexpr bool accepted() const noexcept {
    return disposition == RenderTapeCompletionPlanDisposition::Accepted;
  }
};

struct RenderTapeCompletionPlanHeader {
  std::uint64_t magic = kRenderTapeCompletionPlanMagic;
  std::uint32_t version = kRenderTapeCompletionPlanVersion;
  std::uint32_t headerSize = sizeof(RenderTapeCompletionPlanHeader);
  std::uint32_t byteSize = 0u;
  std::uint32_t tapeEventCount = 0u;
  std::uint32_t submitCount = 0u;
  std::uint32_t releaseCount = 0u;
  std::uint32_t repeatCount = 0u;
  RenderTapeCompletionPlanRoute route = RenderTapeCompletionPlanRoute::Any;
  std::uint32_t reserved0 = 0u;
  std::uint32_t reserved1 = 0u;
  std::array<std::byte, kRenderTapeCompletionPlanDigestBytes> eventsDigest{};
};

struct RenderTapeCompletionPlanSubmitRow {
  std::uint32_t eventOrdinal = 0u;
  std::uint32_t eventSubmissionOrdinal = 0u;

  friend constexpr bool operator==(
      const RenderTapeCompletionPlanSubmitRow&,
      const RenderTapeCompletionPlanSubmitRow&) = default;
};

struct RenderTapeCompletionPlanReleaseRow {
  std::uint32_t releaseAtEventOrdinal = 0u;
  std::uint32_t sourceEventOrdinal = 0u;
  std::uint32_t sourceSubmissionOrdinal = 0u;

  friend constexpr bool operator==(
      const RenderTapeCompletionPlanReleaseRow&,
      const RenderTapeCompletionPlanReleaseRow&) = default;
};

// The on-disk representation is fixed-size and pointer-free.  Unused rows
// must remain zero; this makes the digest independent of an allocator or
// vector capacity and keeps malformed trailing actions visible to validation.
struct RenderTapeCompletionPlanFile {
  RenderTapeCompletionPlanHeader header{};
  std::array<RenderTapeCompletionPlanSubmitRow,
             kRenderTapeCompletionPlanCapacity>
      submits{};
  std::array<RenderTapeCompletionPlanReleaseRow,
             kRenderTapeCompletionPlanCapacity>
      releases{};
};

static_assert(std::is_standard_layout_v<RenderTapeCompletionPlanFile>);
static_assert(std::is_trivially_copyable_v<RenderTapeCompletionPlanFile>);
static_assert(sizeof(RenderTapeCompletionPlanHeader) == 80u);
static_assert(offsetof(RenderTapeCompletionPlanHeader, eventsDigest) == 48u);
static_assert(sizeof(RenderTapeCompletionPlanSubmitRow) == 8u);
static_assert(sizeof(RenderTapeCompletionPlanReleaseRow) == 12u);
static_assert(offsetof(RenderTapeCompletionPlanFile, submits) == 80u);
static_assert(offsetof(RenderTapeCompletionPlanFile, releases) == 336u);
static_assert(sizeof(RenderTapeCompletionPlanFile) == 720u);

inline RenderTapeCompletionPlanResult decodeRenderTapeCompletionPlan(
    std::span<const std::byte> bytes,
    RenderTapeCompletionPlanFile& out) noexcept {
  if (bytes.size() != sizeof(RenderTapeCompletionPlanFile)) {
    return {RenderTapeCompletionPlanDisposition::InvalidSize};
  }
  std::memcpy(&out, bytes.data(), sizeof(out));
  return {};
}

inline RenderTapeCompletionPlanResult validateRenderTapeCompletionPlan(
    const RenderTapeCompletionPlanFile& plan) noexcept {
  const auto& header = plan.header;
  if (header.magic != kRenderTapeCompletionPlanMagic ||
      header.version != kRenderTapeCompletionPlanVersion ||
      header.headerSize != sizeof(RenderTapeCompletionPlanHeader) ||
      header.byteSize != sizeof(RenderTapeCompletionPlanFile) ||
      header.tapeEventCount == 0u || header.reserved0 != 0u ||
      header.reserved1 != 0u) {
    return {RenderTapeCompletionPlanDisposition::InvalidHeader};
  }
  bool digestNonZero = false;
  for (const auto byte : header.eventsDigest) {
    digestNonZero |= byte != std::byte{};
  }
  if (!digestNonZero) {
    return {RenderTapeCompletionPlanDisposition::InvalidDigest};
  }
  if (header.repeatCount == 0u || header.repeatCount >
                                      kRenderTapeCompletionPlanCapacity) {
    return {RenderTapeCompletionPlanDisposition::InvalidRepeat};
  }
  if (header.submitCount == 0u ||
      header.submitCount > kRenderTapeCompletionPlanCapacity ||
      header.releaseCount == 0u ||
      header.releaseCount > kRenderTapeCompletionPlanCapacity) {
    return {RenderTapeCompletionPlanDisposition::InvalidCapacity};
  }
  if (header.route != RenderTapeCompletionPlanRoute::Any &&
      header.route != RenderTapeCompletionPlanRoute::ProductionSlot &&
      header.route != RenderTapeCompletionPlanRoute::Compatibility) {
    return {RenderTapeCompletionPlanDisposition::InvalidRoute};
  }

  const auto validEvent = [&](std::uint32_t event) {
    return event != 0u && event <= header.tapeEventCount;
  };
  for (std::size_t index = 0u; index < header.submitCount; ++index) {
    const auto row = plan.submits[index];
    if (!validEvent(row.eventOrdinal) || row.eventSubmissionOrdinal == 0u) {
      return {RenderTapeCompletionPlanDisposition::InvalidEvent,
              static_cast<std::uint32_t>(index)};
    }
    if (index != 0u) {
      const auto previous = plan.submits[index - 1u];
      if (row == previous) {
        return {RenderTapeCompletionPlanDisposition::DuplicateSubmit,
                static_cast<std::uint32_t>(index)};
      }
      if (row.eventOrdinal < previous.eventOrdinal ||
          (row.eventOrdinal == previous.eventOrdinal &&
           row.eventSubmissionOrdinal != previous.eventSubmissionOrdinal + 1u) ||
          (row.eventOrdinal != previous.eventOrdinal &&
           row.eventSubmissionOrdinal != 1u)) {
        return {RenderTapeCompletionPlanDisposition::NonMonotonicSubmit,
                static_cast<std::uint32_t>(index)};
      }
    } else if (row.eventSubmissionOrdinal != 1u) {
      return {RenderTapeCompletionPlanDisposition::NonMonotonicSubmit, 0u};
    }
  }

  for (std::size_t index = 0u; index < header.releaseCount; ++index) {
    const auto row = plan.releases[index];
    if (!validEvent(row.releaseAtEventOrdinal) ||
        !validEvent(row.sourceEventOrdinal) ||
        row.sourceSubmissionOrdinal == 0u ||
        row.releaseAtEventOrdinal < row.sourceEventOrdinal) {
      return {RenderTapeCompletionPlanDisposition::InvalidReleaseBoundary,
              static_cast<std::uint32_t>(index)};
    }
    if (index != 0u && row.releaseAtEventOrdinal <
                           plan.releases[index - 1u].releaseAtEventOrdinal) {
      return {RenderTapeCompletionPlanDisposition::InvalidReleaseBoundary,
              static_cast<std::uint32_t>(index)};
    }
    for (std::size_t prior = 0u; prior < index; ++prior) {
      if (row.sourceEventOrdinal == plan.releases[prior].sourceEventOrdinal &&
          row.sourceSubmissionOrdinal ==
              plan.releases[prior].sourceSubmissionOrdinal) {
        return {RenderTapeCompletionPlanDisposition::DuplicateRelease,
                static_cast<std::uint32_t>(index)};
      }
    }
    bool hasSubmit = false;
    for (std::size_t submit = 0u; submit < header.submitCount; ++submit) {
      if (plan.submits[submit].eventOrdinal == row.sourceEventOrdinal &&
          plan.submits[submit].eventSubmissionOrdinal ==
              row.sourceSubmissionOrdinal) {
        hasSubmit = true;
        break;
      }
    }
    if (!hasSubmit) {
      return {RenderTapeCompletionPlanDisposition::MissingRelease,
              static_cast<std::uint32_t>(index)};
    }
    if (index >= header.submitCount) {
      return {RenderTapeCompletionPlanDisposition::MissingRelease,
              static_cast<std::uint32_t>(index)};
    }
    const auto expectedSubmit = plan.submits[index];
    if (row.sourceEventOrdinal != expectedSubmit.eventOrdinal ||
        row.sourceSubmissionOrdinal != expectedSubmit.eventSubmissionOrdinal) {
      return {RenderTapeCompletionPlanDisposition::SkippedReleaseFrontier,
              static_cast<std::uint32_t>(index)};
    }
  }
  for (std::size_t submit = 0u; submit < header.submitCount; ++submit) {
    std::size_t releases = 0u;
    for (std::size_t release = 0u; release < header.releaseCount; ++release) {
      releases +=
          plan.submits[submit].eventOrdinal ==
                  plan.releases[release].sourceEventOrdinal &&
              plan.submits[submit].eventSubmissionOrdinal ==
                  plan.releases[release].sourceSubmissionOrdinal;
    }
    if (releases != 1u) {
      return {RenderTapeCompletionPlanDisposition::MissingRelease,
              static_cast<std::uint32_t>(submit)};
    }
  }
  for (std::size_t index = header.submitCount;
       index < kRenderTapeCompletionPlanCapacity; ++index) {
    if (plan.submits[index] != RenderTapeCompletionPlanSubmitRow{}) {
      return {RenderTapeCompletionPlanDisposition::InvalidCapacity,
              static_cast<std::uint32_t>(index)};
    }
  }
  for (std::size_t index = header.releaseCount;
       index < kRenderTapeCompletionPlanCapacity; ++index) {
    if (plan.releases[index] != RenderTapeCompletionPlanReleaseRow{}) {
      return {RenderTapeCompletionPlanDisposition::InvalidCapacity,
              static_cast<std::uint32_t>(index)};
    }
  }
  return {};
}

inline RenderTapeCompletionPlanResult validateRenderTapeCompletionPlan(
    const RenderTapeCompletionPlanFile& plan,
    std::span<const std::byte> expectedEventsDigest) noexcept {
  const auto structural = validateRenderTapeCompletionPlan(plan);
  if (!structural.accepted()) return structural;
  if (expectedEventsDigest.size() != kRenderTapeCompletionPlanDigestBytes ||
      std::memcmp(plan.header.eventsDigest.data(), expectedEventsDigest.data(),
                  kRenderTapeCompletionPlanDigestBytes) != 0) {
    return {RenderTapeCompletionPlanDisposition::InvalidDigest};
  }
  return {};
}

struct RenderTapeQualifiedCompletionKey {
  std::uint64_t seqId = 0u;
  std::uint32_t sourceIndex = 0u;
  std::uint64_t sourceGeneration = 0u;
  std::uint32_t storageFirstPage = 0u;
  std::uint32_t storagePageCount = 0u;
  std::uint64_t storageGeneration = 0u;

  constexpr bool valid() const noexcept {
    return seqId != 0u &&
           sourceIndex != std::numeric_limits<std::uint32_t>::max() &&
           sourceGeneration != 0u && storagePageCount != 0u &&
           storageGeneration != 0u &&
           storageFirstPage <=
               std::numeric_limits<std::uint32_t>::max() -
                   (storagePageCount - 1u);
  }
  friend constexpr bool operator==(
      const RenderTapeQualifiedCompletionKey&,
      const RenderTapeQualifiedCompletionKey&) = default;
};

struct RenderTapeCompletionPlanLocator {
  std::uint32_t eventOrdinal = 0u;
  std::uint32_t eventSubmissionOrdinal = 0u;

  friend constexpr bool operator==(
      const RenderTapeCompletionPlanLocator&,
      const RenderTapeCompletionPlanLocator&) = default;
};

class RenderTapeCompletionPlanController final {
 public:
  using Result = RenderTapeCompletionPlanResult;
  using Locator = RenderTapeCompletionPlanLocator;
  using ActualKey = RenderTapeQualifiedCompletionKey;

  explicit RenderTapeCompletionPlanController(
      const RenderTapeCompletionPlanFile& plan,
      std::span<const std::byte> expectedEventsDigest) noexcept
      : plan_(plan) {
    if (expectedEventsDigest.size() == kRenderTapeCompletionPlanDigestBytes) {
      std::memcpy(expectedDigest_.data(), expectedEventsDigest.data(),
                  kRenderTapeCompletionPlanDigestBytes);
      hasExpectedDigest_ = true;
    }
  }

  Result validate() const noexcept {
    if (!hasExpectedDigest_) {
      return {RenderTapeCompletionPlanDisposition::InvalidDigest};
    }
    return validateRenderTapeCompletionPlan(plan_, expectedDigest_);
  }

  Result beginIteration(std::uint32_t iteration,
                        RenderTapeCompletionPlanRoute route) noexcept {
    const auto validation = validate();
    if (!validation.accepted()) return validation;
    if (failed_) return {RenderTapeCompletionPlanDisposition::Unresolved};
    if (active_) {
      if (plan_.header.route != RenderTapeCompletionPlanRoute::Any &&
          plan_.header.route != route) {
        return terminal(RenderTapeCompletionPlanDisposition::RouteMismatch);
      }
      return terminal(RenderTapeCompletionPlanDisposition::StaleIteration);
    }
    if (plan_.header.route != RenderTapeCompletionPlanRoute::Any &&
        plan_.header.route != route) {
      return {RenderTapeCompletionPlanDisposition::RouteMismatch};
    }
    if (iteration == 0u || iteration > plan_.header.repeatCount ||
        iteration <= completedIterations_ ||
        iteration != completedIterations_ + 1u) {
      return {RenderTapeCompletionPlanDisposition::StaleIteration};
    }
    active_ = true;
    nextSubmit_ = 0u;
    nextRelease_ = 0u;
    lastEvent_ = 0u;
    lastSeq_ = 0u;
    boundCount_ = 0u;
    return {};
  }

  template <typename Submit>
  Result submit(std::uint32_t eventOrdinal,
                std::uint32_t eventSubmissionOrdinal, ActualKey actual,
                Submit&& callback) {
    if (!active_) return {RenderTapeCompletionPlanDisposition::StaleIteration};
    if (failed_) return {RenderTapeCompletionPlanDisposition::Unresolved};
    if (eventOrdinal == 0u || eventOrdinal < lastEvent_ ||
        eventOrdinal > plan_.header.tapeEventCount) {
      return terminal(RenderTapeCompletionPlanDisposition::StaleEvent);
    }
    const Locator locator{eventOrdinal, eventSubmissionOrdinal};
    if (nextSubmit_ == plan_.header.submitCount) {
      return terminal(RenderTapeCompletionPlanDisposition::Exhausted,
                      static_cast<std::uint32_t>(nextSubmit_));
    }
    const auto expected = plan_.submits[nextSubmit_];
    if (expected.eventOrdinal != locator.eventOrdinal ||
        expected.eventSubmissionOrdinal != locator.eventSubmissionOrdinal) {
      for (std::size_t index = 0u; index < boundCount_; ++index) {
        if (bound_[index].locator == locator) {
          return terminal(RenderTapeCompletionPlanDisposition::DuplicateSubmission,
                          static_cast<std::uint32_t>(index));
        }
      }
      return terminal(RenderTapeCompletionPlanDisposition::UnexpectedSubmit,
                      static_cast<std::uint32_t>(nextSubmit_));
    }
    if (!actual.valid() || (lastSeq_ != 0u && actual.seqId <= lastSeq_)) {
      return terminal(
          actual.valid() ? RenderTapeCompletionPlanDisposition::ActualSourceOrder
                         : RenderTapeCompletionPlanDisposition::InvalidActualSource,
          static_cast<std::uint32_t>(nextSubmit_));
    }
    for (std::size_t index = 0u; index < boundCount_; ++index) {
      if (bound_[index].actual == actual ||
          (bound_[index].actual.sourceIndex == actual.sourceIndex &&
           bound_[index].actual.sourceGeneration ==
               actual.sourceGeneration) ||
          storageOverlapsSameGeneration(bound_[index].actual, actual)) {
        return terminal(RenderTapeCompletionPlanDisposition::DuplicateSubmission,
                        static_cast<std::uint32_t>(index));
      }
    }
    if (!callback()) {
      failed_ = true;
      return {RenderTapeCompletionPlanDisposition::CallbackFailure,
              static_cast<std::uint32_t>(nextSubmit_)};
    }
    bound_[boundCount_++] = Bound{locator, actual, false};
    lastSeq_ = actual.seqId;
    ++nextSubmit_;
    return {};
  }

  template <typename Release>
  Result releaseDue(std::uint32_t eventOrdinal, Release&& callback) {
    if (!active_) return {RenderTapeCompletionPlanDisposition::StaleIteration};
    if (failed_) return {RenderTapeCompletionPlanDisposition::Unresolved};
    if (eventOrdinal == 0u || eventOrdinal < lastEvent_ ||
        eventOrdinal > plan_.header.tapeEventCount) {
      return terminal(RenderTapeCompletionPlanDisposition::StaleEvent);
    }
    if (nextRelease_ < plan_.header.releaseCount &&
        plan_.releases[nextRelease_].releaseAtEventOrdinal < eventOrdinal) {
      return terminal(RenderTapeCompletionPlanDisposition::Unresolved,
                      static_cast<std::uint32_t>(nextRelease_));
    }
    while (nextRelease_ < plan_.header.releaseCount &&
           plan_.releases[nextRelease_].releaseAtEventOrdinal <= eventOrdinal) {
      const auto release = plan_.releases[nextRelease_];
      const Locator locator{release.sourceEventOrdinal,
                            release.sourceSubmissionOrdinal};
      const auto found = findBound(locator);
      if (found == boundCount_) {
        return terminal(RenderTapeCompletionPlanDisposition::ReleaseBeforeSubmit,
                        static_cast<std::uint32_t>(nextRelease_));
      }
      if (bound_[found].released) {
        return terminal(RenderTapeCompletionPlanDisposition::DuplicateReleaseAttempt,
                        static_cast<std::uint32_t>(nextRelease_));
      }
      if (found != nextRelease_) {
        return terminal(RenderTapeCompletionPlanDisposition::SkippedReleaseFrontier,
                        static_cast<std::uint32_t>(nextRelease_));
      }
      if (!callback(bound_[found].actual)) {
        failed_ = true;
        return {RenderTapeCompletionPlanDisposition::CallbackFailure,
                static_cast<std::uint32_t>(nextRelease_)};
      }
      bound_[found].released = true;
      ++nextRelease_;
    }
    lastEvent_ = eventOrdinal;
    return {};
  }

  Result endEvent(std::uint32_t eventOrdinal) noexcept {
    if (failed_) return {RenderTapeCompletionPlanDisposition::Unresolved};
    if (!active_ || eventOrdinal == 0u || eventOrdinal < lastEvent_ ||
        eventOrdinal > plan_.header.tapeEventCount) {
      return active_ ? terminal(RenderTapeCompletionPlanDisposition::StaleEvent)
                     : Result{RenderTapeCompletionPlanDisposition::StaleEvent};
    }
    if (nextSubmit_ < plan_.header.submitCount &&
        plan_.submits[nextSubmit_].eventOrdinal <= eventOrdinal) {
      return terminal(RenderTapeCompletionPlanDisposition::MissingSubmit,
                      static_cast<std::uint32_t>(nextSubmit_));
    }
    if (nextRelease_ < plan_.header.releaseCount &&
        plan_.releases[nextRelease_].releaseAtEventOrdinal <= eventOrdinal) {
      return terminal(RenderTapeCompletionPlanDisposition::Unresolved,
                      static_cast<std::uint32_t>(nextRelease_));
    }
    lastEvent_ = eventOrdinal;
    return {};
  }

  Result finishIteration() noexcept {
    if (!active_) return {RenderTapeCompletionPlanDisposition::StaleIteration};
    if (failed_ || nextSubmit_ != plan_.header.submitCount ||
        nextRelease_ != plan_.header.releaseCount || boundCount_ != nextSubmit_ ||
        lastEvent_ != plan_.header.tapeEventCount) {
      failed_ = true;
      return {RenderTapeCompletionPlanDisposition::Unresolved};
    }
    active_ = false;
    ++completedIterations_;
    return {};
  }

  bool failed() const noexcept { return failed_; }
  std::size_t submitted() const noexcept { return nextSubmit_; }
  std::size_t released() const noexcept { return nextRelease_; }
  std::size_t nextSubmit() const noexcept { return nextSubmit_; }
  std::size_t nextRelease() const noexcept { return nextRelease_; }
  const RenderTapeCompletionPlanFile& plan() const noexcept { return plan_; }

 private:
  static bool storageOverlapsSameGeneration(const ActualKey& left,
                                            const ActualKey& right) noexcept {
    if (left.storageGeneration != right.storageGeneration) return false;
    const auto leftEnd = static_cast<std::uint64_t>(left.storageFirstPage) +
                         left.storagePageCount;
    const auto rightEnd = static_cast<std::uint64_t>(right.storageFirstPage) +
                          right.storagePageCount;
    return left.storageFirstPage < rightEnd &&
           right.storageFirstPage < leftEnd;
  }

  Result terminal(RenderTapeCompletionPlanDisposition disposition,
                  std::uint32_t row = 0u) noexcept {
    failed_ = true;
    return {disposition, row};
  }

  struct Bound {
    Locator locator{};
    ActualKey actual{};
    bool released = false;
  };

  std::size_t findBound(Locator locator) const noexcept {
    for (std::size_t index = 0u; index < boundCount_; ++index) {
      if (bound_[index].locator == locator) return index;
    }
    return boundCount_;
  }

  RenderTapeCompletionPlanFile plan_{};
  std::array<std::byte, kRenderTapeCompletionPlanDigestBytes> expectedDigest_{};
  std::array<Bound, kRenderTapeCompletionPlanCapacity> bound_{};
  std::size_t nextSubmit_ = 0u;
  std::size_t nextRelease_ = 0u;
  std::size_t boundCount_ = 0u;
  std::uint32_t completedIterations_ = 0u;
  std::uint32_t lastEvent_ = 0u;
  std::uint64_t lastSeq_ = 0u;
  bool active_ = false;
  bool failed_ = false;
  bool hasExpectedDigest_ = false;
};

}  // namespace dxmt9::d3d9
