#pragma once

#include <cstdint>

namespace dxmt9::d3d9::pe {

// Production/model/native-test vocabulary for the PE recorder's two bounded
// transitions.  Keep the names isomorphic to PeRecorderTransition.tla.
enum class RecorderPhase : std::uint8_t {
  Live,
  Recording,
};

enum class WriteOrigin : std::uint8_t {
  ExplicitSet,
  PriorValueOperation,
};

struct StateWriteFacts {
  RecorderPhase phase = RecorderPhase::Live;
  WriteOrigin origin = WriteOrigin::ExplicitSet;
  bool liveContains = false;
  bool liveEquals = false;
  bool pendingContains = false;
  bool recordedContains = false;
};

enum class StateWriteKind : std::uint8_t {
  NoOp,
  RetainPending,
  QueueDelta,
  RecordExplicit,
  ApplyPriorValueOnly,
};

struct StateWritePlan {
  StateWriteKind kind = StateWriteKind::NoOp;
  bool writeLive = false;
  bool writePending = false;
  bool writeRecorded = false;
  bool directOrderedCall = false;
  bool semanticTransition = false;
};

// PeRecorderTransition!PlanRecorderStateWrite is the exact TLA+ twin.  The
// liveEquals premise is meaningful only when the live table contains the key;
// normalize the impossible pair to "different" so malformed/test facts fail
// closed into a real write rather than dropping state.
constexpr StateWritePlan
planRecorderStateWrite(StateWriteFacts facts) noexcept {
  const bool liveEquals = facts.liveContains && facts.liveEquals;
  if (facts.phase == RecorderPhase::Live) {
    if (liveEquals) {
      return StateWritePlan{
          .kind = facts.pendingContains ? StateWriteKind::RetainPending
                                        : StateWriteKind::NoOp,
      };
    }
    return StateWritePlan{
        .kind = StateWriteKind::QueueDelta,
        .writeLive = true,
        .writePending = true,
        .semanticTransition = true,
    };
  }

  if (facts.origin == WriteOrigin::PriorValueOperation) {
    return StateWritePlan{
        .kind = StateWriteKind::ApplyPriorValueOnly,
        .writeLive = true,
        // MultiplyTransform is Wine's enumerated exception: it acts on the
        // primary live state even while a state block is being recorded.
        .directOrderedCall = true,
        .semanticTransition = !liveEquals,
    };
  }

  return StateWritePlan{
      .kind = StateWriteKind::RecordExplicit,
      .writeRecorded = true,
      // An explicit Set during Begin/End updates only the recording state.
      // The PE-owned StateBlockRecorded table is that state; neither the
      // primary LiveShadow nor the backend primary state may be touched.
      .directOrderedCall = false,
      // Treat every validated explicit recording write as semantic. Equality
      // against LiveShadow says nothing about equality against the independent
      // recorded domain, and the transition intentionally avoids exposing raw
      // recorded-table values merely to suppress this cold event.
      .semanticTransition = true,
  };
}

enum class AppendSettlement : std::uint8_t {
  Prepared,
  Accepted,
  Failed,
  Discarded,
};

struct AppendFacts {
  AppendSettlement phase = AppendSettlement::Prepared;
  bool appendSucceeded = false;
  bool explicitDiscard = false;
};

struct AppendPlan {
  AppendSettlement next = AppendSettlement::Prepared;
  bool consumeRepresentedPending = false;
  bool retainPreparedProjection = true;
  bool recordDurable = false;
  bool valid = false;
};

// PeRecorderTransition!SettleRecorderAppend is the exact TLA+ twin.  Only a
// Prepared witness can settle, and success/discard are mutually exclusive.
// Repeated or contradictory settlement therefore performs no effects.
constexpr AppendPlan settleRecorderAppend(AppendFacts facts) noexcept {
  if (facts.phase != AppendSettlement::Prepared ||
      (facts.appendSucceeded && facts.explicitDiscard)) {
    return AppendPlan{
        .next = facts.phase,
        .retainPreparedProjection = facts.phase == AppendSettlement::Prepared,
    };
  }
  if (facts.appendSucceeded) {
    return AppendPlan{
        .next = AppendSettlement::Accepted,
        .consumeRepresentedPending = true,
        .retainPreparedProjection = false,
        .recordDurable = true,
        .valid = true,
    };
  }
  if (facts.explicitDiscard) {
    return AppendPlan{
        .next = AppendSettlement::Discarded,
        .retainPreparedProjection = false,
        .valid = true,
    };
  }
  return AppendPlan{
      .next = AppendSettlement::Failed,
      .retainPreparedProjection = true,
      .valid = true,
  };
}

}  // namespace dxmt9::d3d9::pe
