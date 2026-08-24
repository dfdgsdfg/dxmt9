#pragma once

#include <array>
#include <cstdint>

enum class PeStateBlockValueCategory : std::uint8_t {
    RenderState,
    Transform,
    Sampler,
};

struct PeStateBlockQualifiedValue {
    PeStateBlockValueCategory category = PeStateBlockValueCategory::RenderState;
    std::uint16_t key = 0u;
    std::uint32_t value = 0u;
    std::uint64_t ordinal = 0u;

    friend constexpr bool operator==(PeStateBlockQualifiedValue,
                                     PeStateBlockQualifiedValue) = default;
};

enum class PeStateBlockValueEvent : std::uint8_t {
    CapturePreEffectFailed,
    CaptureBackendFailed,
    CaptureAccepted,
    ApplyPreEffectFailed,
    ApplyPrepared,
    ApplyBackendFailed,
    ApplyAccepted,
};

enum class PeStateBlockValueAction : std::uint8_t {
    Preserve,
    PoisonFailStop,
    PublishCapture,
    PublishApply,
};

struct PeStateBlockValueRow {
    PeStateBlockValueEvent event;
    PeStateBlockValueAction action;
    bool preserveTrackedSet;
    bool refreshSnapshot;
    bool publishLive;
    bool poison;
};

constexpr bool peStateBlockValueBool(const char* value) noexcept {
    return value[0] == 'T';
}

#define DXMT9_PE_STATEBLOCK_VALUE_ROW(                                   \
    event_, action_, preserve_, refresh_, publish_, poison_)             \
    PeStateBlockValueRow{                                                 \
        PeStateBlockValueEvent::event_, PeStateBlockValueAction::action_, \
        peStateBlockValueBool(#preserve_),                               \
        peStateBlockValueBool(#refresh_),                                \
        peStateBlockValueBool(#publish_),                                \
        peStateBlockValueBool(#poison_)},
inline constexpr auto kPeStateBlockValueTable = std::array{
#include "d3d9_pe_stateblock_value_table.inc"
};
#undef DXMT9_PE_STATEBLOCK_VALUE_ROW

class PeStateBlockValuePlan {
public:
    constexpr bool valid() const noexcept { return valid_; }
    constexpr PeStateBlockValueAction action() const noexcept { return action_; }
    constexpr bool preserveTrackedSet() const noexcept {
        return preserveTrackedSet_;
    }
    constexpr bool refreshSnapshot() const noexcept { return refreshSnapshot_; }
    constexpr bool publishLive() const noexcept { return publishLive_; }
    constexpr bool poison() const noexcept { return poison_; }

private:
    constexpr PeStateBlockValuePlan(
        PeStateBlockValueAction action, bool preserveTrackedSet,
        bool refreshSnapshot, bool publishLive, bool poison,
        bool valid) noexcept
        : action_(action), preserveTrackedSet_(preserveTrackedSet),
          refreshSnapshot_(refreshSnapshot), publishLive_(publishLive),
          poison_(poison), valid_(valid) {}

    PeStateBlockValueAction action_;
    bool preserveTrackedSet_;
    bool refreshSnapshot_;
    bool publishLive_;
    bool poison_;
    bool valid_;

    friend constexpr PeStateBlockValuePlan planPeStateBlockValue(
        PeStateBlockValueEvent) noexcept;
};

constexpr PeStateBlockValuePlan planPeStateBlockValue(
    PeStateBlockValueEvent event) noexcept {
    for (const auto& row : kPeStateBlockValueTable) {
        if (row.event == event) {
            return PeStateBlockValuePlan(
                row.action, row.preserveTrackedSet, row.refreshSnapshot,
                row.publishLive, row.poison, true);
        }
    }
    return PeStateBlockValuePlan(PeStateBlockValueAction::PoisonFailStop,
                                 true, false, false, true, false);
}

constexpr bool peStateBlockCaptureValueMatchesFrozenSet(
    PeStateBlockQualifiedValue prior, PeStateBlockQualifiedValue candidate,
    bool tracked) noexcept {
    if (!tracked) return false;
    return prior.category == candidate.category && prior.key == candidate.key &&
        candidate.ordinal > prior.ordinal;
}

constexpr bool peStateBlockApplyPublishesLatest(
    PeStateBlockQualifiedValue captured, PeStateBlockQualifiedValue published,
    bool tracked) noexcept {
    return tracked && captured == published;
}
