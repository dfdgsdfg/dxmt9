#pragma once

#include <array>
#include <cstdint>

namespace dxmt9::d3d9::pe {

enum class RecorderSettlementPoint : std::uint8_t {
  CapacityPre,
  Emitter,
  CapacityPost,
  Bridge,
};

enum class RecorderSettlementResult : std::uint8_t {
  Succeeded,
  FailedPreEffect,
  FailedEffectUnknown,
};

enum class RecorderSettlementAction : std::uint8_t {
  Continue,
  RetryUnattempted,
  RollbackEmitter,
  KeepAccepted,
  PoisonFailStop,
};

struct RecorderSettlementFacts {
  RecorderSettlementPoint point = RecorderSettlementPoint::CapacityPre;
  RecorderSettlementResult result = RecorderSettlementResult::FailedPreEffect;
};

struct RecorderSettlementRow {
  RecorderSettlementPoint point;
  RecorderSettlementResult result;
  RecorderSettlementAction action;
  bool acceptedRecord;
  bool retryable;
  bool rollbackEmitter;
  bool poison;
};

constexpr bool settlementBool(const char* value) noexcept {
  return value[0] == 'T';
}

#define DXMT9_PE_RECORDER_SETTLEMENT_ROW(                               \
    point_, result_, action_, accepted_, retry_, rollback_, poison_)    \
  RecorderSettlementRow{                                                \
      RecorderSettlementPoint::point_, RecorderSettlementResult::result_, \
      RecorderSettlementAction::action_, settlementBool(#accepted_),   \
      settlementBool(#retry_), settlementBool(#rollback_),             \
      settlementBool(#poison_)},
inline constexpr auto kRecorderSettlementTable = std::array{
#include "d3d9_pe_recorder_settlement_table.inc"
};
#undef DXMT9_PE_RECORDER_SETTLEMENT_ROW

class RecorderSettlementPlan {
 public:
  constexpr bool valid() const noexcept { return valid_; }
  constexpr RecorderSettlementAction action() const noexcept {
    return action_;
  }
  constexpr bool acceptedRecord() const noexcept { return acceptedRecord_; }
  constexpr bool retryable() const noexcept { return retryable_; }
  constexpr bool rollbackEmitter() const noexcept { return rollbackEmitter_; }
  constexpr bool poison() const noexcept { return poison_; }

 private:
  constexpr RecorderSettlementPlan(RecorderSettlementAction action,
                                   bool acceptedRecord, bool retryable,
                                   bool rollbackEmitter, bool poison,
                                   bool valid) noexcept
      : action_(action), acceptedRecord_(acceptedRecord), retryable_(retryable),
        rollbackEmitter_(rollbackEmitter), poison_(poison), valid_(valid) {}

  RecorderSettlementAction action_;
  bool acceptedRecord_;
  bool retryable_;
  bool rollbackEmitter_;
  bool poison_;
  bool valid_;

  friend constexpr RecorderSettlementPlan planRecorderSettlement(
      RecorderSettlementFacts) noexcept;
};

constexpr RecorderSettlementPlan planRecorderSettlement(
    RecorderSettlementFacts facts) noexcept {
  for (const auto& row : kRecorderSettlementTable) {
    if (row.point == facts.point && row.result == facts.result) {
      return RecorderSettlementPlan(row.action, row.acceptedRecord,
                                    row.retryable, row.rollbackEmitter,
                                    row.poison, true);
    }
  }
  return RecorderSettlementPlan(RecorderSettlementAction::PoisonFailStop,
                                false, false, false, true, false);
}

// Classify the result of a nested flush at an append capacity boundary.  A
// failed flush is retryable only while the production transaction remains
// unpoisoned; an entered bridge failure has unknown effect and is fail-stop.
constexpr RecorderSettlementResult recorderFlushSettlementResult(
    bool succeeded, bool recorderPoisoned) noexcept {
  return succeeded
      ? RecorderSettlementResult::Succeeded
      : recorderPoisoned ? RecorderSettlementResult::FailedEffectUnknown
                         : RecorderSettlementResult::FailedPreEffect;
}

constexpr bool recorderCaptureMayRetract(bool commandAccepted) noexcept {
  return !commandAccepted;
}

constexpr bool recorderParentDrainAllowed(bool aliasesRemain,
                                          bool parentPending) noexcept {
  return !aliasesRemain && parentPending;
}

constexpr bool recorderResetAfterDrainAllowed(bool pendingRefsRemain,
                                              bool aliasesRemain,
                                              bool parentPending) noexcept {
  return !pendingRefsRemain && !aliasesRemain && !parentPending;
}

constexpr bool recorderWarmAdvanceAllowed(bool builderReset,
                                          bool drainComplete) noexcept {
  return builderReset && drainComplete;
}

}  // namespace dxmt9::d3d9::pe
