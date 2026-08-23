#pragma once

#include <cstdint>

// Pure state-machine seam for the cold StateBlock Apply interval.  The
// device owns allocations/refs and the backend call; this helper only binds
// phase outcomes to the required failure disposition.
enum class PeStateBlockApplyPhase : std::uint8_t {
    Prepare,
    Backend,
    Commit,
};

enum class PeStateBlockApplyAction : std::uint8_t {
    Continue,
    Publish,
    Preserve,
    Poison,
};

constexpr PeStateBlockApplyAction peStateBlockApplyTransition(
    PeStateBlockApplyPhase phase, bool accepted) noexcept {
    if (accepted) {
        return phase == PeStateBlockApplyPhase::Prepare
            ? PeStateBlockApplyAction::Continue
            : PeStateBlockApplyAction::Publish;
    }
    return phase == PeStateBlockApplyPhase::Backend
        ? PeStateBlockApplyAction::Poison
        : (phase == PeStateBlockApplyPhase::Commit
               ? PeStateBlockApplyAction::Poison
               : PeStateBlockApplyAction::Preserve);
}
