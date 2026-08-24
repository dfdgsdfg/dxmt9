#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>

#include "d3d9_pe_state_shadow.hpp"

enum class PeStateBlockApplyPhase : std::uint8_t { Prepare, Backend, Commit };
enum class PeStateBlockApplyAction : std::uint8_t {
    Continue, Publish, Preserve, Poison,
};
enum class PeStateBlockEndAction : std::uint8_t {
    Continue, Publish, Preserve, Poison,
};
enum class PeStateBlockEndPhase : std::uint8_t { PreEffect, Backend, Wrapper };

enum class PeStateBlockPhase : std::uint8_t {
    Idle,
    Recording,
    EndPublication,
    ApplyPrepared,
    Poisoned,
    Terminal,
};

enum class PeStateBlockEvent : std::uint8_t {
    PoisonRequested,
    BeginFailed,
    BeginAccepted,
    EndPreEffectFailed,
    EndBackendFailed,
    EndBackendAccepted,
    EndWrapperFailed,
    EndPublished,
    CapturePreEffectFailed,
    CaptureBackendFailed,
    CapturePublished,
    ApplyPrepareFailed,
    ApplyPrepared,
    ApplyBackendFailed,
    ApplyBackendAccepted,
    ResetStarted,
    ResetFailed,
    ResetAccepted,
    Teardown,
};

enum class PeStateBlockAction : std::uint8_t {
    Preserve,
    BeginRecording,
    EnterEndPublication,
    PublishEnd,
    FailStop,
    PublishCapture,
    RetainApplyRefs,
    TransferApplyRefs,
    AbandonForReset,
    RecoverReset,
    Teardown,
};

enum class PeStateBlockCandidateEffect : std::uint8_t {
    Preserve,
    Discard,
};

enum class PeStateBlockStagedRefEffect : std::uint8_t {
    Preserve,
    Retain,
    Release,
    Transfer,
};

enum class PeStateBlockCaptureEffect : std::uint8_t {
    Preserve,
    Publish,
};

struct PeStateBlockTransitionFacts {
    PeStateBlockPhase phase = PeStateBlockPhase::Idle;
    PeStateBlockEvent event = PeStateBlockEvent::BeginFailed;
};

class PeStateBlockTransitionPlan {
public:
    constexpr bool valid() const noexcept { return valid_; }
    constexpr PeStateBlockPhase next() const noexcept { return next_; }
    constexpr PeStateBlockAction action() const noexcept { return action_; }
    constexpr PeStateBlockCandidateEffect candidateEffect() const noexcept {
        return candidateEffect_;
    }
    constexpr PeStateBlockStagedRefEffect stagedRefEffect() const noexcept {
        return stagedRefEffect_;
    }
    constexpr PeStateBlockCaptureEffect captureEffect() const noexcept {
        return captureEffect_;
    }
    constexpr bool poisons() const noexcept {
        return next_ == PeStateBlockPhase::Poisoned;
    }

private:
    constexpr PeStateBlockTransitionPlan(
        PeStateBlockPhase next, PeStateBlockAction action,
        PeStateBlockCandidateEffect candidateEffect,
        PeStateBlockStagedRefEffect stagedRefEffect,
        PeStateBlockCaptureEffect captureEffect, bool valid) noexcept
        : next_(next), action_(action), candidateEffect_(candidateEffect),
          stagedRefEffect_(stagedRefEffect), captureEffect_(captureEffect),
          valid_(valid) {}

    PeStateBlockPhase next_;
    PeStateBlockAction action_;
    PeStateBlockCandidateEffect candidateEffect_;
    PeStateBlockStagedRefEffect stagedRefEffect_;
    PeStateBlockCaptureEffect captureEffect_;
    bool valid_;

    friend constexpr PeStateBlockTransitionPlan
    planPeStateBlockTransition(PeStateBlockTransitionFacts) noexcept;
};

struct PeStateBlockTransitionRow {
    PeStateBlockPhase phase;
    PeStateBlockEvent event;
    PeStateBlockPhase next;
    PeStateBlockAction action;
    PeStateBlockCandidateEffect candidateEffect;
    PeStateBlockStagedRefEffect stagedRefEffect;
    PeStateBlockCaptureEffect captureEffect;
};

#define DXMT9_PE_STATEBLOCK_ROW(phase_, event_, next_, action_, candidate_, \
                                staged_, capture_)                         \
    PeStateBlockTransitionRow{                                             \
        PeStateBlockPhase::phase_, PeStateBlockEvent::event_,              \
        PeStateBlockPhase::next_, PeStateBlockAction::action_,             \
        PeStateBlockCandidateEffect::candidate_,                            \
        PeStateBlockStagedRefEffect::staged_,                               \
        PeStateBlockCaptureEffect::capture_},
inline constexpr auto kPeStateBlockTransitionTable = std::array{
#include "d3d9_pe_stateblock_transition_table.inc"
};
#undef DXMT9_PE_STATEBLOCK_ROW

constexpr PeStateBlockTransitionPlan planPeStateBlockTransition(
    PeStateBlockTransitionFacts facts) noexcept {
    for (const auto& row : kPeStateBlockTransitionTable) {
        if (row.phase == facts.phase && row.event == facts.event) {
            return PeStateBlockTransitionPlan(
                row.next, row.action, row.candidateEffect,
                row.stagedRefEffect, row.captureEffect, true);
        }
    }
    return PeStateBlockTransitionPlan(
        facts.phase, PeStateBlockAction::Preserve,
        PeStateBlockCandidateEffect::Preserve,
        PeStateBlockStagedRefEffect::Preserve,
        PeStateBlockCaptureEffect::Preserve, false);
}

constexpr bool peStateBlockRecorderWriteAllowed(
    PeStateBlockPhase phase) noexcept {
    return phase != PeStateBlockPhase::Poisoned &&
           phase != PeStateBlockPhase::Terminal;
}

struct PeStateBlockRecordingEpochStep {
    std::uint64_t value = 0;
    bool valid = false;
};

template<typename Fn>
concept PeStateBlockNothrowRelease =
    std::is_nothrow_invocable_v<Fn&, StateBlockTextureRef> &&
    std::is_nothrow_invocable_v<Fn&, StateBlockStreamSourceValue::BufferRef> &&
    std::is_nothrow_invocable_v<Fn&, StateBlockVertexShaderRef> &&
    std::is_nothrow_invocable_v<Fn&, StateBlockPixelShaderRef> &&
    std::is_nothrow_invocable_v<Fn&, StateBlockVertexDeclarationRef> &&
    std::is_nothrow_invocable_v<Fn&, StateBlockIndexBufferRef> &&
    std::is_nothrow_invocable_v<Fn&, StateBlockRenderTargetRef> &&
    std::is_nothrow_invocable_v<Fn&, StateBlockDepthStencilRef>;

// The epoch is deliberately monotonic across Reset.  Reusing an epoch would
// make a capability retained from an earlier Begin indistinguishable from a
// capability issued after the reset (an ABA write witness).
constexpr PeStateBlockRecordingEpochStep peStateBlockNextRecordingEpoch(
    std::uint64_t current) noexcept {
    if (current == std::numeric_limits<std::uint64_t>::max())
        return {current, false};
    return {current + 1u, true};
}

constexpr bool peStateBlockPoisonAfterReset(bool backendResetSucceeded,
                                            bool priorPoison) noexcept {
    return backendResetSucceeded ? false : priorPoison;
}

constexpr PeStateBlockApplyAction peStateBlockApplyTransition(
    PeStateBlockApplyPhase phase, bool accepted) noexcept {
    const PeStateBlockEvent event = phase == PeStateBlockApplyPhase::Prepare
        ? (accepted ? PeStateBlockEvent::ApplyPrepared
                    : PeStateBlockEvent::ApplyPrepareFailed)
        : (accepted ? PeStateBlockEvent::ApplyBackendAccepted
                    : PeStateBlockEvent::ApplyBackendFailed);
    const PeStateBlockPhase current = phase == PeStateBlockApplyPhase::Prepare
        ? PeStateBlockPhase::Idle
        : PeStateBlockPhase::ApplyPrepared;
    const auto plan = planPeStateBlockTransition({current, event});
    if (!plan.valid()) return PeStateBlockApplyAction::Poison;
    if (plan.action() == PeStateBlockAction::RetainApplyRefs)
        return PeStateBlockApplyAction::Continue;
    if (plan.action() == PeStateBlockAction::TransferApplyRefs)
        return PeStateBlockApplyAction::Publish;
    if (plan.action() == PeStateBlockAction::FailStop)
        return PeStateBlockApplyAction::Poison;
    return PeStateBlockApplyAction::Preserve;
}

constexpr PeStateBlockEndAction peStateBlockEndTransition(
    PeStateBlockEndPhase phase, bool accepted) noexcept {
    if (phase == PeStateBlockEndPhase::PreEffect && accepted)
        return PeStateBlockEndAction::Continue;
    const PeStateBlockEvent event = phase == PeStateBlockEndPhase::PreEffect
        ? PeStateBlockEvent::EndPreEffectFailed
        : phase == PeStateBlockEndPhase::Backend
            ? (accepted ? PeStateBlockEvent::EndBackendAccepted
                        : PeStateBlockEvent::EndBackendFailed)
            : (accepted ? PeStateBlockEvent::EndPublished
                        : PeStateBlockEvent::EndWrapperFailed);
    const PeStateBlockPhase current = phase == PeStateBlockEndPhase::Wrapper
        ? PeStateBlockPhase::EndPublication
        : PeStateBlockPhase::Recording;
    const auto plan = planPeStateBlockTransition({current, event});
    if (!plan.valid()) return PeStateBlockEndAction::Poison;
    if (plan.action() == PeStateBlockAction::EnterEndPublication)
        return PeStateBlockEndAction::Publish;
    if (plan.action() == PeStateBlockAction::PublishEnd)
        return PeStateBlockEndAction::Publish;
    if (plan.action() == PeStateBlockAction::FailStop)
        return PeStateBlockEndAction::Poison;
    return PeStateBlockEndAction::Preserve;
}

// Single owner for the complete PE StateBlock transaction domain. The device
// supplies COM retain/release operations at its boundary; this owner keeps all
// lifecycle flags, the recording candidate, and category-qualified Apply
// staging private so they cannot be advanced independently.
class PeStateBlockTransactionState {
public:
    PeStateBlockTransactionState() noexcept = default;
    PeStateBlockTransactionState(const PeStateBlockTransactionState&) = delete;
    PeStateBlockTransactionState& operator=(
        const PeStateBlockTransactionState&) = delete;

    // A capability is issued only while the transaction is in Recording.
    // The constructor is private, so callers cannot forge a phase witness;
    // mutable writer access is scoped to withWriter and rechecks the phase.
    class RecordingCapability {
    public:
        explicit operator bool() const noexcept {
            return valid_ && owner_ != nullptr &&
                   owner_->phase_ == PeStateBlockPhase::Recording &&
                   owner_->recordingEpoch_ == epoch_;
        }

        template <typename Fn>
            requires std::is_nothrow_invocable_v<
                Fn&&, StateBlockRecorded::Writer&>
        bool withWriter(Fn&& fn) noexcept {
            if (!static_cast<bool>(*this)) return false;
            auto writer = owner_->recorded_.writer();
            std::forward<Fn>(fn)(writer);
            return true;
        }

    private:
        explicit RecordingCapability(PeStateBlockTransactionState& owner) noexcept
            : owner_(&owner), epoch_(owner.recordingEpoch_),
              valid_(owner.phase_ == PeStateBlockPhase::Recording) {}
        PeStateBlockTransactionState* owner_ = nullptr;
        std::uint64_t epoch_ = 0;
        bool valid_ = false;
        friend class PeStateBlockTransactionState;
    };

    RecordingCapability recordingCapability() noexcept {
        return RecordingCapability(*this);
    }

    template <typename Fn>
        requires std::is_nothrow_invocable_v<
            Fn&&, StateBlockRecorded::Writer&>
    bool withRecordingWriter(Fn&& fn) noexcept {
        return recordingCapability().withWriter(std::forward<Fn>(fn));
    }

    const StateBlockRecorded& recordedSnapshot() const noexcept {
        return recorded_.snapshot();
    }

    // Reset owns candidate cleanup while the phase is not Recording; this is
    // deliberately separate from the issued recording capability.
    void clearRecordedCandidateForReset() noexcept {
        recorded_.writer().constants().clearForBegin();
    }

    PeStateBlockPhase phase() const noexcept { return phase_; }
    bool isRecording() const noexcept {
        return phase_ == PeStateBlockPhase::Recording;
    }
    bool isInsideEnd() const noexcept {
        return phase_ == PeStateBlockPhase::EndPublication;
    }
    bool isPoisoned() const noexcept {
        return phase_ == PeStateBlockPhase::Poisoned;
    }
    bool isApplyPrepared() const noexcept {
        return phase_ == PeStateBlockPhase::ApplyPrepared;
    }
    bool writeAllowed() const noexcept {
        return peStateBlockRecorderWriteAllowed(phase_);
    }

    template<typename Release>
        requires PeStateBlockNothrowRelease<Release>
    bool beginAccepted(Release&& release) noexcept {
        const auto plan = transition(PeStateBlockEvent::BeginAccepted);
        if (!plan.valid()) return false;
        const auto epoch = peStateBlockNextRecordingEpoch(recordingEpoch_);
        if (!epoch.valid) {
            poison(std::forward<Release>(release));
            return false;
        }
        if (plan.candidateEffect() == PeStateBlockCandidateEffect::Discard)
            discardRecorded(std::forward<Release>(release));
        recordingEpoch_ = epoch.value;
        phase_ = plan.next();
        return true;
    }

    void beginFailed() noexcept {
        phase_ = transition(PeStateBlockEvent::BeginFailed).next();
    }

    void endPreEffectFailed() noexcept {
        phase_ = transition(PeStateBlockEvent::EndPreEffectFailed).next();
    }

    template<typename Release>
        requires PeStateBlockNothrowRelease<Release>
    void abandonRecording(Release&& release) noexcept {
        const auto plan = transition(PeStateBlockEvent::ResetStarted);
        if (!plan.valid()) return;
        if (plan.candidateEffect() == PeStateBlockCandidateEffect::Discard)
            discardRecorded(std::forward<Release>(release));
        phase_ = plan.next();
    }

    template<typename Release>
        requires PeStateBlockNothrowRelease<Release>
    void failEnd(Release&& release) noexcept {
        const auto event = isInsideEnd()
            ? PeStateBlockEvent::EndWrapperFailed
            : PeStateBlockEvent::EndBackendFailed;
        const auto plan = transition(event);
        if (!plan.valid()) return;
        if (plan.candidateEffect() == PeStateBlockCandidateEffect::Discard)
            discardRecorded(std::forward<Release>(release));
        phase_ = plan.next();
    }

    void enterEndPublication() noexcept {
        const auto plan = transition(PeStateBlockEvent::EndBackendAccepted);
        if (plan.valid()) phase_ = plan.next();
    }

    template<typename Release>
        requires PeStateBlockNothrowRelease<Release>
    void finishEndPublication(bool published, Release&& release) noexcept {
        const auto event = published ? PeStateBlockEvent::EndPublished
                                     : PeStateBlockEvent::EndWrapperFailed;
        const auto plan = transition(event);
        if (!plan.valid()) return;
        if (plan.candidateEffect() == PeStateBlockCandidateEffect::Discard)
            discardRecorded(std::forward<Release>(release));
        phase_ = plan.next();
    }

    template<typename Release>
        requires PeStateBlockNothrowRelease<Release>
    void poison(Release&& release) noexcept {
        const auto plan = transition(PeStateBlockEvent::PoisonRequested);
        if (!plan.valid()) return;
        if (plan.candidateEffect() == PeStateBlockCandidateEffect::Discard)
            discardRecorded(release);
        if (plan.stagedRefEffect() == PeStateBlockStagedRefEffect::Release)
            discardPrepared(release);
        phase_ = plan.next();
    }

    void markApplyPrepared() noexcept {
        const auto plan = transition(PeStateBlockEvent::ApplyPrepared);
        if (plan.valid() &&
            plan.stagedRefEffect() == PeStateBlockStagedRefEffect::Retain) {
            phase_ = plan.next();
        }
    }

    template<typename Release>
        requires PeStateBlockNothrowRelease<Release>
    void failPreparedApply(Release&& release) noexcept {
        const auto plan = transition(PeStateBlockEvent::ApplyBackendFailed);
        if (!plan.valid()) return;
        if (plan.stagedRefEffect() == PeStateBlockStagedRefEffect::Release)
            discardPrepared(std::forward<Release>(release));
        phase_ = plan.next();
    }

    void finishPreparedApply() noexcept {
        const auto plan = transition(PeStateBlockEvent::ApplyBackendAccepted);
        if (plan.valid() &&
            plan.stagedRefEffect() == PeStateBlockStagedRefEffect::Transfer) {
            phase_ = plan.next();
        }
    }

    template<typename Release>
        requires PeStateBlockNothrowRelease<Release>
    void resetSucceeded(Release&& release) noexcept {
        const auto plan = transition(PeStateBlockEvent::ResetAccepted);
        if (!plan.valid()) return;
        if (plan.stagedRefEffect() == PeStateBlockStagedRefEffect::Release)
            discardPrepared(std::forward<Release>(release));
        phase_ = plan.next();
    }

    void resetFailed() noexcept {
        phase_ = transition(PeStateBlockEvent::ResetFailed).next();
    }

    template<typename Release>
        requires PeStateBlockNothrowRelease<Release>
    void discardAll(Release&& release) noexcept {
        const auto plan = transition(PeStateBlockEvent::Teardown);
        if (!plan.valid()) return;
        if (plan.candidateEffect() == PeStateBlockCandidateEffect::Discard)
            discardRecorded(release);
        if (plan.stagedRefEffect() == PeStateBlockStagedRefEffect::Release)
            discardPrepared(release);
        phase_ = plan.next();
    }

    template<typename Retain>
        requires std::is_nothrow_invocable_v<Retain&, StateBlockTextureRef>
    bool stageTexture(StateBlockTextureSlot slot, StateBlockTextureRef value,
                      Retain&& retain) noexcept {
        if (phase_ != PeStateBlockPhase::Idle || !slot.valid() ||
            (stagedTextureMask_ & (1u << rawSlot(slot))) != 0u) return false;
        retain(value);
        stagedTextures_[rawSlot(slot)] = value;
        stagedTextureMask_ |= 1u << rawSlot(slot);
        return true;
    }
    template<typename Retain>
        requires std::is_nothrow_invocable_v<
            Retain&, StateBlockStreamSourceValue::BufferRef>
    bool stageStream(StateBlockStreamSlot slot, StateBlockStreamSourceValue value,
                     Retain&& retain) noexcept {
        if (phase_ != PeStateBlockPhase::Idle || !slot.valid() ||
            (stagedStreamMask_ & (1u << rawSlot(slot))) != 0u) return false;
        retain(value.buffer);
        stagedStreams_[rawSlot(slot)] = value;
        stagedStreamMask_ |= 1u << rawSlot(slot);
        return true;
    }
    template<typename Retain>
        requires std::is_nothrow_invocable_v<Retain&, StateBlockVertexShaderRef>
    bool stageVertexShader(StateBlockVertexShaderRef value,
                           Retain&& retain) noexcept {
        if (phase_ != PeStateBlockPhase::Idle) return false;
        return stageSingleton(stagedVertexShader_, stagedVertexShaderValid_, value,
                              std::forward<Retain>(retain));
    }
    template<typename Retain>
        requires std::is_nothrow_invocable_v<Retain&, StateBlockPixelShaderRef>
    bool stagePixelShader(StateBlockPixelShaderRef value,
                          Retain&& retain) noexcept {
        if (phase_ != PeStateBlockPhase::Idle) return false;
        return stageSingleton(stagedPixelShader_, stagedPixelShaderValid_, value,
                              std::forward<Retain>(retain));
    }
    template<typename Retain>
        requires std::is_nothrow_invocable_v<Retain&, StateBlockIndexBufferRef>
    bool stageIndexBuffer(StateBlockIndexBufferRef value,
                          Retain&& retain) noexcept {
        if (phase_ != PeStateBlockPhase::Idle) return false;
        return stageSingleton(stagedIndexBuffer_, stagedIndexBufferValid_, value,
                              std::forward<Retain>(retain));
    }
    template<typename Retain>
        requires std::is_nothrow_invocable_v<Retain&, StateBlockRenderTargetRef>
    bool stageRenderTarget(StateBlockRenderTargetSlot slot,
                           StateBlockRenderTargetRef value,
                           Retain&& retain) noexcept {
        if (phase_ != PeStateBlockPhase::Idle || !slot.valid() ||
            (stagedRenderTargetMask_ & (1u << rawSlot(slot))) != 0u) return false;
        retain(value);
        stagedRenderTargets_[rawSlot(slot)] = value;
        stagedRenderTargetMask_ |= 1u << rawSlot(slot);
        return true;
    }
    template<typename Retain>
        requires std::is_nothrow_invocable_v<Retain&, StateBlockDepthStencilRef>
    bool stageDepthStencil(StateBlockDepthStencilRef value,
                           Retain&& retain) noexcept {
        if (phase_ != PeStateBlockPhase::Idle) return false;
        return stageSingleton(stagedDepthStencil_, stagedDepthStencilValid_, value,
                              std::forward<Retain>(retain));
    }

    StateBlockTextureRef takeTexture(StateBlockTextureSlot slot) noexcept {
        if (phase_ != PeStateBlockPhase::ApplyPrepared || !slot.valid()) return {};
        stagedTextureMask_ &= ~(1u << rawSlot(slot));
        return take(stagedTextures_[rawSlot(slot)]);
    }
    StateBlockStreamSourceValue takeStream(StateBlockStreamSlot slot) noexcept {
        if (phase_ != PeStateBlockPhase::ApplyPrepared || !slot.valid()) return {};
        stagedStreamMask_ &= ~(1u << rawSlot(slot));
        return take(stagedStreams_[rawSlot(slot)]);
    }
    StateBlockVertexShaderRef takeVertexShader() noexcept {
        if (phase_ != PeStateBlockPhase::ApplyPrepared) return {};
        stagedVertexShaderValid_ = false;
        return take(stagedVertexShader_);
    }
    StateBlockPixelShaderRef takePixelShader() noexcept {
        if (phase_ != PeStateBlockPhase::ApplyPrepared) return {};
        stagedPixelShaderValid_ = false;
        return take(stagedPixelShader_);
    }
    StateBlockIndexBufferRef takeIndexBuffer() noexcept {
        if (phase_ != PeStateBlockPhase::ApplyPrepared) return {};
        stagedIndexBufferValid_ = false;
        return take(stagedIndexBuffer_);
    }
    StateBlockRenderTargetRef takeRenderTarget(
        StateBlockRenderTargetSlot slot) noexcept {
        if (phase_ != PeStateBlockPhase::ApplyPrepared || !slot.valid()) return {};
        stagedRenderTargetMask_ &= ~(1u << rawSlot(slot));
        return take(stagedRenderTargets_[rawSlot(slot)]);
    }
    StateBlockDepthStencilRef takeDepthStencil() noexcept {
        if (phase_ != PeStateBlockPhase::ApplyPrepared) return {};
        stagedDepthStencilValid_ = false;
        return take(stagedDepthStencil_);
    }

    bool hasPreparedApply() const noexcept {
        return stagedTextureMask_ != 0u || stagedStreamMask_ != 0u ||
               stagedRenderTargetMask_ != 0u || stagedVertexShaderValid_ ||
               stagedPixelShaderValid_ || stagedIndexBufferValid_ ||
               stagedDepthStencilValid_;
    }

    template<typename Release>
        requires PeStateBlockNothrowRelease<Release>
    void discardPrepared(Release&& release) noexcept {
        auto& releaseRef = release;
        discardArray(stagedTextures_, stagedTextureMask_, releaseRef);
        discardArray(stagedStreams_, stagedStreamMask_, releaseRef);
        discardSingleton(stagedVertexShader_, stagedVertexShaderValid_, releaseRef);
        discardSingleton(stagedPixelShader_, stagedPixelShaderValid_, releaseRef);
        discardSingleton(stagedIndexBuffer_, stagedIndexBufferValid_, releaseRef);
        discardArray(stagedRenderTargets_, stagedRenderTargetMask_, releaseRef);
        discardSingleton(stagedDepthStencil_, stagedDepthStencilValid_, releaseRef);
    }

private:
    PeStateBlockTransitionPlan transition(PeStateBlockEvent event) const noexcept {
        const auto plan = planPeStateBlockTransition({phase_, event});
        // Every production lifecycle call is required to have a generated row.
        // Invalid calls fail closed in the existing phase in release builds.
        return plan;
    }

    template<typename Release>
    void discardRecorded(Release&& release) noexcept {
        recorded_.snapshot().forEachOwnedComRef(release);
        recorded_.writer().clear();
    }
    template<typename Ref, typename Retain>
        requires std::is_nothrow_invocable_v<Retain&, Ref>
    static bool stageSingleton(Ref& destination, bool& occupied, Ref value,
                               Retain&& retain) noexcept {
        if (occupied) return false;
        retain(value);
        destination = value;
        occupied = true;
        return true;
    }
    template<typename T>
    static T take(T& value) noexcept {
        T result = value;
        value = T{};
        return result;
    }
    template<typename Ref, typename Release>
    static void discardSingleton(Ref& value, bool& occupied,
                                 Release& release) noexcept {
        if (occupied) release(ownedRef(value));
        value = Ref{};
        occupied = false;
    }
    template<typename T, std::size_t Slots, typename Release>
    static void discardArray(std::array<T, Slots>& values,
                             std::uint32_t& mask, Release& release) noexcept {
        for (std::size_t slot = 0; slot < Slots; ++slot) {
            if ((mask & (1u << slot)) != 0u)
                release(ownedRef(values[slot]));
            values[slot] = T{};
        }
        mask = 0u;
    }

    template<typename Ref>
    static Ref ownedRef(Ref value) noexcept {
        return value;
    }
    static StateBlockStreamSourceValue::BufferRef ownedRef(
        const StateBlockStreamSourceValue& value) noexcept {
        return value.buffer;
    }

    PeStateBlockPhase phase_ = PeStateBlockPhase::Idle;
    std::uint64_t recordingEpoch_ = 0u;
    StateBlockRecorded recorded_{};
    std::array<StateBlockTextureRef, kPeTextureSlots> stagedTextures_{};
    std::uint32_t stagedTextureMask_ = 0u;
    std::array<StateBlockStreamSourceValue, D9C_DRAW_PACKET_MAX_STREAMS>
        stagedStreams_{};
    std::uint32_t stagedStreamMask_ = 0u;
    StateBlockVertexShaderRef stagedVertexShader_{};
    bool stagedVertexShaderValid_ = false;
    StateBlockPixelShaderRef stagedPixelShader_{};
    bool stagedPixelShaderValid_ = false;
    StateBlockIndexBufferRef stagedIndexBuffer_{};
    bool stagedIndexBufferValid_ = false;
    std::array<StateBlockRenderTargetRef, D9C_DRAW_PACKET_MAX_RENDER_TARGETS>
        stagedRenderTargets_{};
    std::uint32_t stagedRenderTargetMask_ = 0u;
    StateBlockDepthStencilRef stagedDepthStencil_{};
    bool stagedDepthStencilValid_ = false;
};
