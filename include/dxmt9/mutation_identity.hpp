#pragma once

#include <cstdint>
#include <limits>

namespace dxmt9::resources {

enum class MutationSourceKind : std::uint8_t {
  SynchronousMutation,
  DeferredMutation,
  ReplayUse,
};

inline constexpr bool mutationSourceKindValid(
    MutationSourceKind source) noexcept {
  switch (source) {
    case MutationSourceKind::SynchronousMutation:
    case MutationSourceKind::DeferredMutation:
    case MutationSourceKind::ReplayUse:
      return true;
  }
  return false;
}

// The production source ordinal remains useful for exact settlement and
// replay diagnostics, but it is not a shared domain: synchronous unlocks and
// deferred replay use different producers.  The observer assigns this typed
// identity at ingress so composition compares one ordered domain instead of
// guessing between producer-local counters.
struct MutationOrderingIdentity {
  std::uint64_t generation = 0u;
  std::uint64_t ordinal = 0u;
  MutationSourceKind source = MutationSourceKind::SynchronousMutation;

  constexpr bool valid() const noexcept {
    return generation != 0u && ordinal != 0u &&
           mutationSourceKindValid(source);
  }
};

inline constexpr bool mutationOrderingPrecedes(
    const MutationOrderingIdentity& earlier,
    const MutationOrderingIdentity& later) noexcept {
  return earlier.valid() && later.valid() &&
         earlier.generation == later.generation &&
         earlier.ordinal < later.ordinal;
}

// Fixed-width, allocation-free policy used only by the cold observer.  A
// reset advances the generation before ordinals restart; generation overflow
// permanently invalidates the policy rather than reusing an identity.
class MutationOrderingPolicy final {
 public:
  [[nodiscard]] constexpr MutationOrderingIdentity issue(
      MutationSourceKind source) noexcept {
    if (!valid_ || !mutationSourceKindValid(source) || nextOrdinal_ == 0u ||
        nextOrdinal_ ==
                                      std::numeric_limits<std::uint64_t>::max())
      return {};
    const auto result = MutationOrderingIdentity{
        .generation = generation_, .ordinal = nextOrdinal_, .source = source};
    ++nextOrdinal_;
    return result;
  }

  [[nodiscard]] constexpr bool reset() noexcept {
    if (!valid_ || generation_ == std::numeric_limits<std::uint64_t>::max()) {
      valid_ = false;
      return false;
    }
    ++generation_;
    nextOrdinal_ = 1u;
    return true;
  }

  [[nodiscard]] constexpr std::uint64_t generation() const noexcept {
    return generation_;
  }

  [[nodiscard]] constexpr std::uint64_t nextOrdinal() const noexcept {
    return valid_ ? nextOrdinal_ : 0u;
  }

  [[nodiscard]] constexpr bool valid() const noexcept { return valid_; }

 private:
  std::uint64_t generation_ = 1u;
  std::uint64_t nextOrdinal_ = 1u;
  bool valid_ = true;
};

// Canonical identity shared by managed mutation offload, the observer, and
// Render Tape's ResourceMutation interpretation. `resource` is the stable
// slot-plus-generation handle value, `backingGeneration` is the
// BufferRecord::contentRevision, and `sourceOrdinal` is the producer's native
// queue/tape position retained for settlement and source-facing diagnostics.
// Wrapper pointers and Metal object addresses are never part of this identity.
struct MutationSourceIdentity {
  std::uint64_t resource = 0u;
  std::uint64_t backingGeneration = 0u;
  std::uint64_t sourceOrdinal = 0u;

  constexpr bool valid() const noexcept {
    return resource != 0u && backingGeneration != 0u && sourceOrdinal != 0u;
  }
};

inline constexpr bool sameMutationResourceGeneration(
    const MutationSourceIdentity& left,
    const MutationSourceIdentity& right) noexcept {
  return left.resource == right.resource &&
         left.backingGeneration == right.backingGeneration;
}

inline constexpr bool mutationSourceOrdinalPrecedes(
    const MutationSourceIdentity& earlier,
    const MutationSourceIdentity& later) noexcept {
  return earlier.sourceOrdinal != 0u && later.sourceOrdinal != 0u &&
         earlier.sourceOrdinal < later.sourceOrdinal;
}

}  // namespace dxmt9::resources
