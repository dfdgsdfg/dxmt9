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
  bool recordedContains = false;
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

struct StateWritePlan {
  StateWriteKind kind = StateWriteKind::NoOp;
  bool writeLive = false;
  bool writePending = false;
  bool writeRecorded = false;
  bool directOrderedCall = false;
  bool semanticTransition = false;
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
    return StateWritePlan{
        .kind = row.kind,
        .writeLive = row.writeLive,
        .writePending = row.writePending,
        .writeRecorded = row.writeRecorded,
        .directOrderedCall = row.directOrderedCall,
        .semanticTransition = semantic,
    };
  }
  return {};
}

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
  for (const auto& row : kAppendTable) {
    if (row.phase != facts.phase ||
        !matches(row.appendSucceeded, facts.appendSucceeded) ||
        !matches(row.explicitDiscard, facts.explicitDiscard)) {
      continue;
    }
    return AppendPlan{
        .next = row.next,
        .consumeRepresentedPending = row.consumeRepresentedPending,
        .retainPreparedProjection = row.retainPreparedProjection,
        .recordDurable = row.recordDurable,
        .valid = row.valid,
    };
  }
  return {};
}

}  // namespace dxmt9::d3d9::pe
