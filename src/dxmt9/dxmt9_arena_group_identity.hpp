#pragma once

#include <cstdint>

namespace dxmt9::core {

// Value identity of one strict Arena publication group. Possessing this value
// does not prove completion; Tape and the post-encode receipt ledger publish
// a settlement only after their respective completion transition succeeds.
struct ArenaGroupIdentity {
  std::uint64_t rawOrdinal = 0;
  std::uint64_t buildGeneration = 0;
  std::uint64_t firstSourceOrdinal = 0;
  std::uint64_t tailSeqId = 0;
  std::uint32_t sourceCount = 0;
  bool hasPresent = false;

  constexpr bool valid() const noexcept {
    return rawOrdinal != 0 && buildGeneration != 0 &&
           firstSourceOrdinal != 0 && tailSeqId != 0 && sourceCount != 0;
  }

  friend constexpr bool operator==(ArenaGroupIdentity,
                                   ArenaGroupIdentity) noexcept = default;
};

}  // namespace dxmt9::core
