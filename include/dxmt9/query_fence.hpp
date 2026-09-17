#pragma once

#include <cstdint>

namespace dxmt9::query {

// A queue incarnation plus an actually published prefix, never a core API
// ordinal or a prediction of the next writer's sequence. Sequence zero is a
// valid empty prefix only when the queue incarnation is nonzero.
struct Fence {
  std::uint64_t queue = 0;
  std::uint64_t sequence = 0;
  std::uint64_t initializer = 0;
  constexpr bool valid() const noexcept { return queue != 0; }
};

struct Progress {
  std::uint64_t queue = 0;
  std::uint64_t published = 0;
  std::uint64_t completed = 0;
  bool failed = false;
  std::uint64_t initializerPublished = 0;
  std::uint64_t initializerCompleted = 0;
};

enum class Poll : std::uint8_t { Invalid, Failed, Pending, Complete };

// QueryPublicationFence / Poll: publication alone never proves completion.
// Terminal state wins over a high watermark (including an empty prefix).
constexpr Poll classify(Fence fence, Progress progress) noexcept {
  if (!fence.valid() || fence.queue != progress.queue ||
      progress.completed > progress.published ||
      fence.sequence > progress.published ||
      progress.initializerCompleted > progress.initializerPublished ||
      fence.initializer > progress.initializerPublished) return Poll::Invalid;
  if (progress.failed) return Poll::Failed;
  return progress.completed >= fence.sequence &&
                 progress.initializerCompleted >= fence.initializer
      ? Poll::Complete : Poll::Pending;
}

// A sticky async progress obligation survives the caller's return. A later
// request cannot weaken it; completion of an older prefix cannot clear it.
constexpr std::uint64_t submissionTarget(std::uint64_t requested,
                                         std::uint64_t completed) noexcept {
  return requested > completed ? requested : 0;
}

}  // namespace dxmt9::query
