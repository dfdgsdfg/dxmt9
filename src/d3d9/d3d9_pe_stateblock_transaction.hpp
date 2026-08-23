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

// Reset is the only device-lifecycle boundary that can recover the
// fail-stop latch.  Validation/bridge/backend failure must leave an already
// poisoned recorder poisoned; only a backend reset that returned success may
// clear it and discard any pre-effect Apply staging.
constexpr bool peStateBlockPoisonAfterReset(bool backendResetSucceeded,
                                            bool priorPoison) noexcept {
    return backendResetSucceeded ? false : priorPoison;
}

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
