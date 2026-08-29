#pragma once

#include <cstdint>

namespace dxmt9::resources {

// Canonical identity shared by managed mutation offload, the observer, and
// Render Tape's ResourceMutation interpretation. `resource` is the stable
// slot-plus-generation handle value, `backingGeneration` is the
// BufferRecord::contentRevision, and `sourceOrdinal` is the ordered queue/tape
// source position. Wrapper pointers and Metal object addresses are never part
// of this identity.
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
