#pragma once

#include "d3d9_pe_recorder_settlement.hpp"

#include <cstdint>

namespace dxmt9::d3d9::pe {

// Host-buildable commit settlement vocabulary.  This is deliberately a
// value-only companion to CommandChunkBuilder and RenderTape: it decides
// whether the next side effect is legal, but owns no bytes, refs, or journal.
enum class RecorderCommitPhase : std::uint8_t {
  Unsealed,
  Sealed,
  Accepted,
  CaptureSettled,
  Draining,
  Drained,
  Reset,
  WarmAdvanced,
  Poisoned,
  Discarded,
};

enum class RecorderCommitEvent : std::uint8_t {
  SealAccepted,
  SealFailed,
  BridgeAccepted,
  BridgePreEffectFailed,
  BridgeEffectUnknown,
  CaptureMaterialized,
  CaptureRejected,
  CaptureSkipped,
  DrainPending,
  DrainAlias,
  DrainParent,
  DrainComplete,
  BuilderReset,
  WarmEpochAdvance,
  ExplicitDiscard,
  DeviceReset,
};

enum class RecorderCommitAction : std::uint8_t {
  NoOp,
  Retry,
  Seal,
  AcceptCommand,
  CaptureCommit,
  CaptureReject,
  BeginDrain,
  DestroyAlias,
  DestroyParent,
  FinishDrain,
  ResetBuilder,
  AdvanceWarmEpoch,
  DiscardAll,
  FailStop,
  Invalid,
};

struct RecorderCommitFacts {
  RecorderCommitPhase phase = RecorderCommitPhase::Unsealed;
  RecorderCommitEvent event = RecorderCommitEvent::SealFailed;
  bool aliasesRemain = false;
  bool parentPending = false;
};

class RecorderCommitPlan {
 public:
  constexpr RecorderCommitPhase next() const noexcept { return next_; }
  constexpr RecorderCommitAction action() const noexcept { return action_; }
  constexpr bool valid() const noexcept { return valid_; }
  constexpr bool preserveRetryBytes() const noexcept {
    return action_ == RecorderCommitAction::Retry;
  }
  constexpr bool commandAccepted() const noexcept {
    return action_ == RecorderCommitAction::AcceptCommand;
  }
  constexpr bool objectDestroy() const noexcept {
    return action_ == RecorderCommitAction::DestroyAlias ||
        action_ == RecorderCommitAction::DestroyParent;
  }
  constexpr bool resetBuilder() const noexcept {
    return action_ == RecorderCommitAction::ResetBuilder ||
        action_ == RecorderCommitAction::DiscardAll;
  }
  constexpr bool advanceWarmEpoch() const noexcept {
    return action_ == RecorderCommitAction::AdvanceWarmEpoch;
  }
  constexpr bool poisons() const noexcept {
    return action_ == RecorderCommitAction::FailStop;
  }

 private:
  constexpr RecorderCommitPlan(RecorderCommitPhase next,
                               RecorderCommitAction action, bool valid)
      : next_(next), action_(action), valid_(valid) {}

  static constexpr bool isKnownTransition(RecorderCommitPhase next,
                                           RecorderCommitAction action) noexcept {
    switch (action) {
    case RecorderCommitAction::NoOp:
      return next == RecorderCommitPhase::CaptureSettled ||
          next == RecorderCommitPhase::Unsealed;
    case RecorderCommitAction::Retry:
      return next == RecorderCommitPhase::Unsealed ||
          next == RecorderCommitPhase::Sealed;
    case RecorderCommitAction::Seal:
      return next == RecorderCommitPhase::Sealed;
    case RecorderCommitAction::AcceptCommand:
      return next == RecorderCommitPhase::Accepted;
    case RecorderCommitAction::CaptureCommit:
    case RecorderCommitAction::CaptureReject:
      return next == RecorderCommitPhase::CaptureSettled;
    case RecorderCommitAction::BeginDrain:
    case RecorderCommitAction::DestroyAlias:
      return next == RecorderCommitPhase::Draining;
    case RecorderCommitAction::DestroyParent:
    case RecorderCommitAction::FinishDrain:
      return next == RecorderCommitPhase::Drained;
    case RecorderCommitAction::ResetBuilder:
      return next == RecorderCommitPhase::Reset;
    case RecorderCommitAction::AdvanceWarmEpoch:
      return next == RecorderCommitPhase::WarmAdvanced;
    case RecorderCommitAction::DiscardAll:
      return next == RecorderCommitPhase::Discarded;
    case RecorderCommitAction::FailStop:
      return next == RecorderCommitPhase::Poisoned;
    case RecorderCommitAction::Invalid:
      return false;
    }
    return false;
  }

  static constexpr RecorderCommitPlan fromAction(
      RecorderCommitPhase next, RecorderCommitAction action) noexcept {
    return RecorderCommitPlan(next, action, isKnownTransition(next, action));
  }

  RecorderCommitPhase next_;
  RecorderCommitAction action_;
  bool valid_;

  friend constexpr RecorderCommitPlan commitPlan(
      RecorderCommitPhase, RecorderCommitAction) noexcept;
  friend constexpr RecorderCommitPlan invalidCommitPlan() noexcept;
};

constexpr RecorderCommitPlan commitPlan(RecorderCommitPhase next,
                                        RecorderCommitAction action) noexcept {
  return RecorderCommitPlan::fromAction(next, action);
}

constexpr RecorderCommitPlan invalidCommitPlan() noexcept {
  return RecorderCommitPlan(RecorderCommitPhase::Unsealed,
                            RecorderCommitAction::Invalid, false);
}

// The production commit/discard paths and native/TLA witnesses use this exact
// transition. A locally proven pre-effect bridge failure remains retryable;
// an entered-call failure is effect-unknown and fail-stop. Capture rejection
// occurs only after command acceptance; parent destruction is illegal while an
// alias remains; and reset/epoch advancement are distinct post-drain effects.
constexpr RecorderCommitPlan settleRecorderCommit(
    RecorderCommitFacts facts) noexcept {
  if (facts.event == RecorderCommitEvent::ExplicitDiscard ||
      facts.event == RecorderCommitEvent::DeviceReset) {
    if (facts.phase == RecorderCommitPhase::Discarded) {
      return invalidCommitPlan();
    }
    return commitPlan(RecorderCommitPhase::Discarded,
                      RecorderCommitAction::DiscardAll);
  }

  switch (facts.phase) {
  case RecorderCommitPhase::Unsealed:
    if (facts.event == RecorderCommitEvent::SealAccepted)
      return commitPlan(RecorderCommitPhase::Sealed,
                        RecorderCommitAction::Seal);
    if (facts.event == RecorderCommitEvent::SealFailed)
      return commitPlan(RecorderCommitPhase::Unsealed,
                        RecorderCommitAction::Retry);
    break;
  case RecorderCommitPhase::Sealed:
    if (facts.event == RecorderCommitEvent::BridgeAccepted)
      return commitPlan(RecorderCommitPhase::Accepted,
                        RecorderCommitAction::AcceptCommand);
    if (facts.event == RecorderCommitEvent::BridgePreEffectFailed)
      return commitPlan(RecorderCommitPhase::Sealed,
                        RecorderCommitAction::Retry);
    if (facts.event == RecorderCommitEvent::BridgeEffectUnknown)
      return commitPlan(RecorderCommitPhase::Poisoned,
                        RecorderCommitAction::FailStop);
    break;
  case RecorderCommitPhase::Accepted:
    if (facts.event == RecorderCommitEvent::CaptureMaterialized)
      return commitPlan(RecorderCommitPhase::CaptureSettled,
                        RecorderCommitAction::CaptureCommit);
    if (facts.event == RecorderCommitEvent::CaptureRejected)
      return commitPlan(RecorderCommitPhase::CaptureSettled,
                        RecorderCommitAction::CaptureReject);
    if (facts.event == RecorderCommitEvent::CaptureSkipped)
      return commitPlan(RecorderCommitPhase::CaptureSettled,
                        RecorderCommitAction::NoOp);
    break;
  case RecorderCommitPhase::CaptureSettled:
    if (facts.event == RecorderCommitEvent::DrainPending)
      return commitPlan(RecorderCommitPhase::Draining,
                        RecorderCommitAction::BeginDrain);
    break;
  case RecorderCommitPhase::Draining:
    if (facts.event == RecorderCommitEvent::DrainAlias && facts.aliasesRemain)
      return commitPlan(RecorderCommitPhase::Draining,
                        RecorderCommitAction::DestroyAlias);
    if (facts.event == RecorderCommitEvent::DrainParent &&
        recorderParentDrainAllowed(facts.aliasesRemain,
                                   facts.parentPending)) {
      return commitPlan(RecorderCommitPhase::Drained,
                        RecorderCommitAction::DestroyParent);
    }
    if (facts.event == RecorderCommitEvent::DrainComplete &&
        !facts.aliasesRemain && !facts.parentPending) {
      return commitPlan(RecorderCommitPhase::Drained,
                        RecorderCommitAction::FinishDrain);
    }
    break;
  case RecorderCommitPhase::Drained:
    if (facts.event == RecorderCommitEvent::BuilderReset)
      return commitPlan(RecorderCommitPhase::Reset,
                        RecorderCommitAction::ResetBuilder);
    break;
  case RecorderCommitPhase::Reset:
    if (facts.event == RecorderCommitEvent::WarmEpochAdvance)
      return commitPlan(RecorderCommitPhase::WarmAdvanced,
                        RecorderCommitAction::AdvanceWarmEpoch);
    break;
  case RecorderCommitPhase::WarmAdvanced:
    if (facts.event == RecorderCommitEvent::DrainComplete)
      return commitPlan(RecorderCommitPhase::Unsealed,
                        RecorderCommitAction::NoOp);
    break;
  case RecorderCommitPhase::Discarded:
  case RecorderCommitPhase::Poisoned:
    break;
  }
  return invalidCommitPlan();
}

}  // namespace dxmt9::d3d9::pe
