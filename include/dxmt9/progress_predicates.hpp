#pragma once

#include <cstdint>

namespace dxmt9::queue {

// Shared pure-value projections for ConcurrentProgressSignals.tla. Query
// GetData is a non-blocking poll predicate; the other two functions are the
// predicates of their owning condition-variable waits.
constexpr bool queryGetDataPollSatisfied(std::uint64_t completedSeq,
                                         std::uint64_t targetSeq) noexcept {
  return completedSeq >= targetSeq;
}

constexpr bool presentTokenWaitSatisfied(
    std::uint64_t completedPresentToken, std::uint64_t targetToken,
    bool stopped, bool aborted) noexcept {
  return stopped || aborted || completedPresentToken >= targetToken;
}

constexpr bool ringAdmissionWaitSatisfied(
    bool stopped, bool poisoned, bool arenaBuildActive,
    bool arenaBuildContextPresent, bool controlSlotsFree,
    bool reserveStillPressured) noexcept {
  return stopped || poisoned ||
      (!arenaBuildActive && !arenaBuildContextPresent && controlSlotsFree &&
       !reserveStillPressured);
}

}  // namespace dxmt9::queue
