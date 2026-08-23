#pragma once

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
  Discarded,
};

enum class RecorderCommitEvent : std::uint8_t {
  SealAccepted,
  SealFailed,
  BridgeAccepted,
  BridgeFailed,
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
  Invalid,
};

struct RecorderCommitFacts {
  RecorderCommitPhase phase = RecorderCommitPhase::Unsealed;
  RecorderCommitEvent event = RecorderCommitEvent::SealFailed;
  bool aliasesRemain = false;
  bool parentPending = false;
};

struct RecorderCommitPlan {
  RecorderCommitPhase next = RecorderCommitPhase::Unsealed;
  RecorderCommitAction action = RecorderCommitAction::Invalid;
  bool valid = false;
  bool preserveRetryBytes = false;
  bool commandAccepted = false;
  bool objectDestroy = false;
  bool resetBuilder = false;
  bool advanceWarmEpoch = false;
};

constexpr RecorderCommitPlan commitPlan(
    RecorderCommitPhase next, RecorderCommitAction action,
    bool preserveRetryBytes = false, bool commandAccepted = false,
    bool objectDestroy = false, bool resetBuilder = false,
    bool advanceWarmEpoch = false) noexcept {
  return RecorderCommitPlan{
      .next = next,
      .action = action,
      .valid = true,
      .preserveRetryBytes = preserveRetryBytes,
      .commandAccepted = commandAccepted,
      .objectDestroy = objectDestroy,
      .resetBuilder = resetBuilder,
      .advanceWarmEpoch = advanceWarmEpoch,
  };
}

constexpr RecorderCommitPlan invalidCommitPlan() noexcept {
  return {};
}

// The production commit/discard paths and native/TLA witnesses use this exact
// transition.  In particular, bridge failure remains Sealed and preserves the
// sealed bytes; capture rejection occurs only after command acceptance; parent
// destruction is illegal while an alias remains; and reset/epoch advancement
// are distinct post-drain effects.
constexpr RecorderCommitPlan settleRecorderCommit(
    RecorderCommitFacts facts) noexcept {
  if (facts.event == RecorderCommitEvent::ExplicitDiscard ||
      facts.event == RecorderCommitEvent::DeviceReset) {
    if (facts.phase == RecorderCommitPhase::Discarded) {
      return invalidCommitPlan();
    }
    return commitPlan(RecorderCommitPhase::Discarded,
                      RecorderCommitAction::DiscardAll, false, false, false,
                      true, false);
  }

  switch (facts.phase) {
  case RecorderCommitPhase::Unsealed:
    if (facts.event == RecorderCommitEvent::SealAccepted)
      return commitPlan(RecorderCommitPhase::Sealed,
                        RecorderCommitAction::Seal);
    if (facts.event == RecorderCommitEvent::SealFailed)
      return commitPlan(RecorderCommitPhase::Unsealed,
                        RecorderCommitAction::Retry, true);
    break;
  case RecorderCommitPhase::Sealed:
    if (facts.event == RecorderCommitEvent::BridgeAccepted)
      return commitPlan(RecorderCommitPhase::Accepted,
                        RecorderCommitAction::AcceptCommand, false, true);
    if (facts.event == RecorderCommitEvent::BridgeFailed)
      return commitPlan(RecorderCommitPhase::Sealed,
                        RecorderCommitAction::Retry, true);
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
                        RecorderCommitAction::DestroyAlias, false, false, true);
    if (facts.event == RecorderCommitEvent::DrainParent &&
        !facts.aliasesRemain && facts.parentPending) {
      return commitPlan(RecorderCommitPhase::Drained,
                        RecorderCommitAction::DestroyParent, false, false, true);
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
                        RecorderCommitAction::ResetBuilder, false, false,
                        false, true);
    break;
  case RecorderCommitPhase::Reset:
    if (facts.event == RecorderCommitEvent::WarmEpochAdvance)
      return commitPlan(RecorderCommitPhase::WarmAdvanced,
                        RecorderCommitAction::AdvanceWarmEpoch, false, false,
                        false, false, true);
    break;
  case RecorderCommitPhase::WarmAdvanced:
    if (facts.event == RecorderCommitEvent::DrainComplete)
      return commitPlan(RecorderCommitPhase::Unsealed,
                        RecorderCommitAction::NoOp);
    break;
  case RecorderCommitPhase::Discarded:
    break;
  }
  return invalidCommitPlan();
}

}  // namespace dxmt9::d3d9::pe
