#pragma once

#include <array>
#include <cstdint>
#include <string_view>

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
};

enum class StateWriteKind : std::uint8_t {
  NoOp,
  RetainPending,
  QueueDelta,
  RecordExplicit,
  ApplyPriorValueOnly,
};

enum class AppendSettlement : std::uint8_t {
  Prepared,
  Accepted,
  Failed,
  Discarded,
};

class StateWritePlan {
 public:
  constexpr StateWriteKind kind() const noexcept { return kind_; }
  constexpr bool valid() const noexcept { return valid_; }
  constexpr bool writeLive() const noexcept {
    return kind_ == StateWriteKind::QueueDelta ||
        kind_ == StateWriteKind::ApplyPriorValueOnly;
  }
  constexpr bool writePending() const noexcept {
    return kind_ == StateWriteKind::QueueDelta;
  }
  constexpr bool writeRecorded() const noexcept {
    return kind_ == StateWriteKind::RecordExplicit;
  }
  constexpr bool directOrderedCall() const noexcept {
    return kind_ == StateWriteKind::ApplyPriorValueOnly;
  }
  constexpr bool semanticTransition() const noexcept { return semantic_; }

 private:
  constexpr StateWritePlan(StateWriteKind kind, bool semantic, bool valid)
      : kind_(kind), semantic_(semantic), valid_(valid) {}

  static constexpr StateWritePlan invalid() noexcept {
    return StateWritePlan(StateWriteKind::NoOp, false, false);
  }
  static constexpr StateWritePlan fromKind(StateWriteKind kind,
                                            bool semantic) noexcept {
    return StateWritePlan(kind, semantic, true);
  }

  StateWriteKind kind_;
  bool semantic_;
  bool valid_;

  friend constexpr StateWritePlan planRecorderStateWrite(StateWriteFacts) noexcept;
};

enum class TruthValue : std::uint8_t {
  False,
  True,
  Any,
};

enum class SemanticRule : std::uint8_t {
  False,
  True,
  AnyNotEqualLive,
};

enum class OriginRule : std::uint8_t {
  ExplicitSet,
  PriorValueOperation,
  Any,
};

struct StateWriteTableRow {
  RecorderPhase phase;
  OriginRule origin;
  TruthValue liveEquals;
  TruthValue pendingContains;
  StateWriteKind kind;
  bool writeLive;
  bool writePending;
  bool writeRecorded;
  bool directOrderedCall;
  SemanticRule semanticTransition;
};

constexpr bool tableBool(std::string_view value) noexcept {
  return value == "True";
}

constexpr TruthValue tableTruth(std::string_view value) noexcept {
  return value == "Any" ? TruthValue::Any
      : value == "True" ? TruthValue::True
                        : TruthValue::False;
}

constexpr OriginRule tableOrigin(std::string_view value) noexcept {
  return value == "Any" ? OriginRule::Any
      : value == "ExplicitSet" ? OriginRule::ExplicitSet
                               : OriginRule::PriorValueOperation;
}

constexpr SemanticRule tableSemantic(std::string_view value) noexcept {
  return value == "True" ? SemanticRule::True
      : value == "AnyNotEqualLive" ? SemanticRule::AnyNotEqualLive
                                   : SemanticRule::False;
}

// This table is also the source for the generated TLA module.  Keep the
// production predicate table-driven so a model/table edit cannot silently
// leave the C++ branch algebra behind.
#define DXMT9_PE_STATE_WRITE_ROW(                                         \
    phase_, origin_, equal_, pending_, kind_, live_, pendingWrite_,       \
    recorded_, direct_, semantic_)                                        \
  StateWriteTableRow{                                                      \
      .phase = RecorderPhase::phase_,                                      \
      .origin = tableOrigin(#origin_),                                     \
      .liveEquals = tableTruth(#equal_),                                   \
      .pendingContains = tableTruth(#pending_),                            \
      .kind = StateWriteKind::kind_,                                       \
      .writeLive = tableBool(#live_),                                      \
      .writePending = tableBool(#pendingWrite_),                           \
      .writeRecorded = tableBool(#recorded_),                              \
      .directOrderedCall = tableBool(#direct_),                            \
      .semanticTransition = tableSemantic(#semantic_)},
#define DXMT9_PE_APPEND_ROW(...) /* not a state row */
inline constexpr auto kStateWriteTable = std::array{
#include "d3d9_pe_transition_table.inc"
};
#undef DXMT9_PE_APPEND_ROW
#undef DXMT9_PE_STATE_WRITE_ROW

struct AppendTableRow {
  AppendSettlement phase;
  TruthValue appendSucceeded;
  TruthValue explicitDiscard;
  AppendSettlement next;
  bool consumeRepresentedPending;
  bool retainPreparedProjection;
  bool recordDurable;
  bool valid;
};

#define DXMT9_PE_STATE_WRITE_ROW(...) /* not an append row */
#define DXMT9_PE_APPEND_ROW(                                            \
    phase_, succeeded_, discard_, next_, consume_, retain_, durable_,   \
    valid_)                                                              \
  AppendTableRow{                                                        \
      .phase = AppendSettlement::phase_,                                 \
      .appendSucceeded = tableTruth(#succeeded_),                        \
      .explicitDiscard = tableTruth(#discard_),                          \
      .next = AppendSettlement::next_,                                   \
      .consumeRepresentedPending = tableBool(#consume_),                 \
      .retainPreparedProjection = tableBool(#retain_),                   \
      .recordDurable = tableBool(#durable_),                             \
      .valid = tableBool(#valid_)},
inline constexpr auto kAppendTable = std::array{
#include "d3d9_pe_transition_table.inc"
};
#undef DXMT9_PE_APPEND_ROW
#undef DXMT9_PE_STATE_WRITE_ROW

constexpr bool matches(TruthValue expected, bool actual) noexcept {
  return expected == TruthValue::Any ||
      (expected == TruthValue::True) == actual;
}

constexpr bool matches(OriginRule expected, WriteOrigin actual) noexcept {
  return expected == OriginRule::Any ||
      (expected == OriginRule::ExplicitSet &&
       actual == WriteOrigin::ExplicitSet) ||
      (expected == OriginRule::PriorValueOperation &&
       actual == WriteOrigin::PriorValueOperation);
}

// PeRecorderTransition!PlanRecorderStateWrite is the exact TLA+ twin.  The
// liveEquals premise is meaningful only when the live table contains the key;
// normalize the impossible pair to "different" so malformed/test facts fail
// closed into a real write rather than dropping state.
constexpr StateWritePlan
planRecorderStateWrite(StateWriteFacts facts) noexcept {
  const bool liveEquals = facts.liveContains && facts.liveEquals;
  for (const auto& row : kStateWriteTable) {
    if (row.phase != facts.phase || !matches(row.origin, facts.origin) ||
        !matches(row.liveEquals, liveEquals) ||
        !matches(row.pendingContains, facts.pendingContains)) {
      continue;
    }
    const bool semantic = row.semanticTransition == SemanticRule::True ||
        (row.semanticTransition == SemanticRule::AnyNotEqualLive &&
         !liveEquals);
    const StateWritePlan plan = StateWritePlan::fromKind(row.kind, semantic);
    // The row remains the canonical model/code matrix.  Reject a row whose
    // effect bits disagree with the kind-derived DOD queries rather than
    // allowing an impossible public plan to be represented.
    if (plan.writeLive() != row.writeLive ||
        plan.writePending() != row.writePending ||
        plan.writeRecorded() != row.writeRecorded ||
        plan.directOrderedCall() != row.directOrderedCall ||
        plan.semanticTransition() != semantic) {
      return StateWritePlan::invalid();
    }
    return plan;
  }
  return StateWritePlan::invalid();
}

struct AppendFacts {
  AppendSettlement phase = AppendSettlement::Prepared;
  bool appendSucceeded = false;
  bool explicitDiscard = false;
};

enum class AppendPlanKind : std::uint8_t {
  Accepted,
  Retry,
  Discarded,
  Invalid,
};

class AppendPlan {
 public:
  constexpr AppendSettlement next() const noexcept { return next_; }
  constexpr bool valid() const noexcept {
    return kind_ != AppendPlanKind::Invalid;
  }
  constexpr bool consumeRepresentedPending() const noexcept {
    return kind_ == AppendPlanKind::Accepted;
  }
  constexpr bool retainPreparedProjection() const noexcept {
    return kind_ == AppendPlanKind::Retry;
  }
  constexpr bool recordDurable() const noexcept {
    return kind_ == AppendPlanKind::Accepted;
  }

 private:
  constexpr AppendPlan(AppendSettlement next, AppendPlanKind kind) noexcept
      : next_(next), kind_(kind) {}

  static constexpr AppendPlan invalid(AppendSettlement next) noexcept {
    return AppendPlan(next, AppendPlanKind::Invalid);
  }

  static constexpr AppendPlan fromRow(const AppendTableRow& row) noexcept {
    if (!row.valid) {
      return invalid(row.next);
    }
    const AppendPlanKind kind =
        row.next == AppendSettlement::Accepted &&
                row.consumeRepresentedPending && row.recordDurable &&
                !row.retainPreparedProjection
            ? AppendPlanKind::Accepted
        : row.next == AppendSettlement::Failed &&
                !row.consumeRepresentedPending && !row.recordDurable &&
                row.retainPreparedProjection
            ? AppendPlanKind::Retry
        : row.next == AppendSettlement::Discarded &&
                !row.consumeRepresentedPending && !row.recordDurable &&
                !row.retainPreparedProjection
            ? AppendPlanKind::Discarded
            : AppendPlanKind::Invalid;
    const AppendPlan plan(row.next, kind);
    if (kind == AppendPlanKind::Invalid) {
      return invalid(row.next);
    }
    return plan;
  }

  AppendSettlement next_;
  AppendPlanKind kind_;

  friend constexpr AppendPlan settleRecorderAppend(AppendFacts) noexcept;
};

constexpr bool appendTableRowCanonical(const AppendTableRow& row) noexcept {
  if (!row.valid) {
    return row.next == row.phase && !row.consumeRepresentedPending &&
        !row.recordDurable &&
        (row.next == AppendSettlement::Prepared
             ? row.retainPreparedProjection
             : !row.retainPreparedProjection);
  }
  return (row.next == AppendSettlement::Accepted &&
          row.consumeRepresentedPending && !row.retainPreparedProjection &&
          row.recordDurable) ||
      (row.next == AppendSettlement::Failed &&
       !row.consumeRepresentedPending && row.retainPreparedProjection &&
       !row.recordDurable) ||
      (row.next == AppendSettlement::Discarded &&
       !row.consumeRepresentedPending && !row.retainPreparedProjection &&
       !row.recordDurable);
}

constexpr bool appendTableCanonical() noexcept {
  for (const auto& row : kAppendTable) {
    if (!appendTableRowCanonical(row)) {
      return false;
    }
  }
  return true;
}

static_assert(appendTableCanonical(), "append table effect drift");

// PeRecorderTransition!SettleRecorderAppend is the exact TLA+ twin.  Only a
// Prepared witness can settle, and success/discard are mutually exclusive.
// Repeated or contradictory settlement therefore performs no effects.
constexpr AppendPlan settleRecorderAppend(AppendFacts facts) noexcept {
  for (const auto& row : kAppendTable) {
    if (row.phase != facts.phase ||
        !matches(row.appendSucceeded, facts.appendSucceeded) ||
        !matches(row.explicitDiscard, facts.explicitDiscard)) {
      continue;
    }
    return AppendPlan::fromRow(row);
  }
  return AppendPlan::invalid(facts.phase);
}

}  // namespace dxmt9::d3d9::pe
