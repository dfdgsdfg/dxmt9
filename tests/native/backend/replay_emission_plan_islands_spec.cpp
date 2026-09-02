// Model-code binding for `specs/verification/tla/ReplayEmissionPlanIslands.tla`.
//
// The model checks three obligations a divisible raw has and an indivisible
// one never did: the active-raw span witness, the post-separator fail-stop
// cut, and the explicit draw-run closure at a cut. Each is guarded in the
// model by a discipline constant so a deliberate regression configuration can
// delete exactly one and be checked as an expected counterexample.
//
// This spec binds the model to production in two layers.
//
//  1. A truth table over `dxmt9::core::compatibilitySpanAdmission`, the
//     production predicate the model's `AdmitsLeaseOrdinal` mirrors. The
//     model's guard is a projection of this function, not a restatement of
//     it: the queue calls the same function from
//     `beginDirectChunkSlotReplay`, and the Tape calls it again from
//     `extendCompatibilityProducerIdentity`.
//
//  2. A trace-conformance state machine mirroring the model's VARIABLES, with
//     one `Step` per TLA action:
//
//       BeginSpan0          -> Step::BeginSpan0
//       CommitSpan0         -> Step::CommitSpan0
//       ExecuteSeparator    -> Step::ExecuteSeparator
//       BeginSpan2          -> Step::BeginSpan2
//       CommitSpan2         -> Step::CommitSpan2
//       FailSpan2           -> Step::FailSpan2
//       BeginDuplicateSpan0 -> Step::BeginDuplicateSpan0
//       BeginAfterSettled   -> Step::BeginAfterSettled
//
//     The production trace is replayed with every discipline enforced and all
//     invariants must hold. Each counterexample configuration is then
//     replayed as its own step sequence and must violate exactly the
//     invariant the matching `.counterexample.cfg` names, so the negative TLA
//     configs and the native negative cases stay paired.

#include "dxmt9/dxmt9_cpu_ready_tape.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using dxmt9::core::CpuReadyProducerIdentity;
using dxmt9::core::CpuReadySpanAdmission;
using dxmt9::core::CpuReadySpanWitness;
using dxmt9::core::compatibilityProducerIdentityAppendable;
using dxmt9::core::compatibilitySpanAdmission;

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void check(bool condition, std::string_view message) {
  if (!condition) {
    throw TestFailure(std::string(message));
  }
}

// One raw carries one closed producer interval, so every span of it presents
// the identical value.
constexpr CpuReadyProducerIdentity kRawIdentity{
    .firstEventOrdinal = 7,
    .lastEventOrdinal = 7,
    .firstSourceOrdinal = 11,
    .lastSourceOrdinal = 11,
};
constexpr CpuReadyProducerIdentity kNextRawIdentity{
    .firstEventOrdinal = 8,
    .lastEventOrdinal = 8,
    .firstSourceOrdinal = 12,
    .lastSourceOrdinal = 12,
};
constexpr std::uint64_t kRaw = 5;
constexpr std::uint64_t kOtherRaw = 6;

// ---------------------------------------------------------------------------
// Layer 1 -- the shared predicate.

void spanAdmissionTruthTable() {
  // The reason a witness exists at all: the cross-raw rule demands a strict
  // +1 in both dimensions, so it can never describe two spans of one raw.
  check(!compatibilityProducerIdentityAppendable(kRawIdentity, kRawIdentity),
        "the cross-raw predicate refuses a same-interval continuation");
  check(compatibilityProducerIdentityAppendable(kRawIdentity,
                                                kNextRawIdentity),
        "the cross-raw predicate still admits the adjacent next raw");

  // No witness: the cross-raw predicate remains the only authority.
  check(compatibilitySpanAdmission(CpuReadySpanWitness{}, kRaw, 0u,
                                   kRawIdentity) ==
            CpuReadySpanAdmission::CrossRaw,
        "an inactive witness defers to the cross-raw rule");
  // A span-unqualified caller (rawOrdinal == 0) is the historical whole-raw
  // path and must be byte-identical to the pre-span behaviour.
  check(compatibilitySpanAdmission(
            CpuReadySpanWitness{.rawOrdinal = kRaw,
                                .lastSpanOrdinal = 0u,
                                .producerIdentity = kRawIdentity},
            0u, 0u, kRawIdentity) ==
            CpuReadySpanAdmission::CrossRaw,
        "an unqualified caller never takes the same-raw path");
  // A different raw never reaches the witnessed path either.
  check(compatibilitySpanAdmission(
            CpuReadySpanWitness{.rawOrdinal = kRaw,
                                .lastSpanOrdinal = 0u,
                                .producerIdentity = kRawIdentity},
            kOtherRaw, 1u, kNextRawIdentity) ==
            CpuReadySpanAdmission::CrossRaw,
        "a different raw is decided by the unchanged cross-raw rule");

  // The one admitted case: the exact witnessed raw, unsettled, immediate
  // successor ordinal, identical closed interval.
  check(compatibilitySpanAdmission(
            CpuReadySpanWitness{.rawOrdinal = kRaw,
                                .lastSpanOrdinal = 0u,
                                .producerIdentity = kRawIdentity},
            kRaw, 1u, kRawIdentity) ==
            CpuReadySpanAdmission::SameRawSpan,
        "the immediate successor span of the active raw is admitted");

  // Every rejection is its own reason, so a fallback is never unclassified.
  check(compatibilitySpanAdmission(
            CpuReadySpanWitness{.rawOrdinal = kRaw,
                                .lastSpanOrdinal = 1u,
                                .producerIdentity = kRawIdentity},
            kRaw, 1u, kRawIdentity) ==
            CpuReadySpanAdmission::DuplicateSpan,
        "a repeated span ordinal is a duplicate");
  check(compatibilitySpanAdmission(
            CpuReadySpanWitness{.rawOrdinal = kRaw,
                                .lastSpanOrdinal = 1u,
                                .producerIdentity = kRawIdentity},
            kRaw, 0u, kRawIdentity) ==
            CpuReadySpanAdmission::DuplicateSpan,
        "an out-of-order span ordinal is a duplicate");
  check(compatibilitySpanAdmission(
            CpuReadySpanWitness{.rawOrdinal = kRaw,
                                .lastSpanOrdinal = 1u,
                                .producerIdentity = kRawIdentity},
            kRaw, 3u, kRawIdentity) ==
            CpuReadySpanAdmission::SkippedSpan,
        "a non-adjacent span ordinal is a skip");
  check(compatibilitySpanAdmission(
            CpuReadySpanWitness{.rawOrdinal = kRaw,
                                .lastSpanOrdinal = 1u,
                                .settled = true,
                                .producerIdentity = kRawIdentity},
            kRaw, 2u, kRawIdentity) ==
            CpuReadySpanAdmission::SettledRaw,
        "nothing may extend a raw after its final span");
  check(compatibilitySpanAdmission(
            CpuReadySpanWitness{.rawOrdinal = kRaw,
                                .lastSpanOrdinal = 0u,
                                .producerIdentity = kRawIdentity},
            kRaw, 1u, kNextRawIdentity) ==
            CpuReadySpanAdmission::IdentityMismatch,
        "the same raw presenting a different interval is rejected");

  // Settlement dominates ordinal arithmetic: a settled raw is refused even
  // for what would otherwise be its immediate successor.
  check(compatibilitySpanAdmission(
            CpuReadySpanWitness{.rawOrdinal = kRaw,
                                .lastSpanOrdinal = 0u,
                                .settled = true,
                                .producerIdentity = kRawIdentity},
            kRaw, 1u, kRawIdentity) ==
            CpuReadySpanAdmission::SettledRaw,
        "settlement is checked before the ordinal relation");

  // A saturated ordinal cannot wrap into a false successor.
  check(compatibilitySpanAdmission(
            CpuReadySpanWitness{
                .rawOrdinal = kRaw,
                .lastSpanOrdinal = std::numeric_limits<std::uint32_t>::max(),
                .producerIdentity = kRawIdentity},
            kRaw, 0u, kRawIdentity) ==
            CpuReadySpanAdmission::DuplicateSpan,
        "a saturated witness ordinal never admits a wrapped successor");
}

// ---------------------------------------------------------------------------
// Layer 2 -- trace conformance.

enum class Discipline : std::uint8_t { Enforced, Removed };

struct Disciplines {
  Discipline spanIdentity = Discipline::Enforced;
  Discipline rawLocalWitness = Discipline::Enforced;
  Discipline separatorCut = Discipline::Enforced;
  Discipline runClosure = Discipline::Enforced;
};

enum class IntervalIdentity : std::uint8_t {
  None,
  PreviousRaw,
  ActiveRaw,
  SlotAggregate,
};

enum class Stage : std::uint8_t {
  Init,
  Span0Begun,
  Span0Committed,
  SeparatorDone,
  Span2Begun,
  Done,
  FailStopped,
  LegacyWholeRaw,
};

enum class Disposition : std::uint8_t { Unset, Direct, Legacy, FailStop };

// One step per TLA action, in the mapping documented at the top of this file.
enum class Step : std::uint8_t {
  BeginSpan0,
  CommitSpan0,
  ExecuteSeparator,
  BeginSpan2,
  CommitSpan2,
  FailSpan2,
  BeginDuplicateSpan0,
  BeginAfterSettled,
};

// Mirrors the model's VARIABLES exactly.
struct ModelState {
  Stage stage = Stage::Init;
  Disposition disposition = Disposition::Unset;
  CpuReadySpanWitness witness{};
  IntervalIdentity slotInterval = IntervalIdentity::PreviousRaw;
  IntervalIdentity witnessInterval = IntervalIdentity::None;
  std::array<std::uint32_t, 3> emitted{};
  bool separatorExecuted = false;
  bool runOpen = false;
  bool runStraddledCut = false;
  bool effectsStarted = false;
  bool span2Fails = false;
};

// The model's `LeaseOrdinal`: ordinary spans consume no ordinal.
constexpr std::uint32_t leaseOrdinal(std::size_t span) {
  return span == 0u ? 0u : 1u;
}

// The model's admission guard, evaluated through the *production* predicate
// so the two cannot drift.
bool admitsLeaseOrdinal(const ModelState& state, std::uint32_t ordinal,
                        Disciplines disciplines) {
  if (disciplines.spanIdentity == Discipline::Removed) {
    return true;
  }
  return compatibilitySpanAdmission(state.witness, kRaw, ordinal,
                                    kRawIdentity) ==
         CpuReadySpanAdmission::SameRawSpan;
}

// Returns false when the step is disabled in this state, exactly as a TLA
// action guard would be.
bool applyStep(ModelState& state, Step step, Disciplines disciplines) {
  switch (step) {
  case Step::BeginSpan0:
    if (state.stage != Stage::Init || state.witness.active()) return false;
    state.stage = Stage::Span0Begun;
    state.runOpen = true;
    return true;
  case Step::CommitSpan0:
    if (state.stage != Stage::Span0Begun) return false;
    state.stage = Stage::Span0Committed;
    state.disposition = Disposition::Direct;
    state.slotInterval = IntervalIdentity::SlotAggregate;
    state.witnessInterval =
        disciplines.rawLocalWitness == Discipline::Enforced
        ? IntervalIdentity::ActiveRaw
        : IntervalIdentity::SlotAggregate;
    state.witness = CpuReadySpanWitness{
        .rawOrdinal = kRaw,
        .lastSpanOrdinal = leaseOrdinal(0),
        .settled = false,
        .producerIdentity =
            disciplines.rawLocalWitness == Discipline::Enforced
            ? kRawIdentity
            : CpuReadyProducerIdentity{6u, 7u, 10u, 11u}};
    ++state.emitted[0];
    state.effectsStarted = true;
    if (disciplines.runClosure == Discipline::Enforced) state.runOpen = false;
    return true;
  case Step::ExecuteSeparator:
    if (state.stage != Stage::Span0Committed) return false;
    state.stage = Stage::SeparatorDone;
    state.separatorExecuted = true;
    ++state.emitted[1];
    state.runStraddledCut = state.runStraddledCut || state.runOpen;
    state.runOpen = false;
    return true;
  case Step::BeginSpan2:
    if (state.stage != Stage::SeparatorDone ||
        !admitsLeaseOrdinal(state, leaseOrdinal(2), disciplines)) {
      return false;
    }
    state.stage = Stage::Span2Begun;
    state.runOpen = true;
    return true;
  case Step::CommitSpan2:
    if (state.stage != Stage::Span2Begun || state.span2Fails) return false;
    state.stage = Stage::Done;
    state.witness.lastSpanOrdinal = leaseOrdinal(2);
    state.witness.settled = true;
    ++state.emitted[2];
    if (disciplines.runClosure == Discipline::Enforced) state.runOpen = false;
    return true;
  case Step::FailSpan2:
    if (state.stage != Stage::Span2Begun || !state.span2Fails) return false;
    if (disciplines.separatorCut == Discipline::Enforced) {
      state.stage = Stage::FailStopped;
      state.disposition = Disposition::FailStop;
      return true;
    }
    // The retired whole-raw Legacy retry: sound only while a raw was
    // indivisible, and a second emission of every span once it is not.
    state.stage = Stage::LegacyWholeRaw;
    state.disposition = Disposition::Legacy;
    for (auto& count : state.emitted) ++count;
    return true;
  case Step::BeginDuplicateSpan0:
    if (state.stage != Stage::Span0Committed ||
        !admitsLeaseOrdinal(state, leaseOrdinal(0), disciplines)) {
      return false;
    }
    ++state.emitted[0];
    return true;
  case Step::BeginAfterSettled:
    if (state.stage != Stage::Done ||
        !admitsLeaseOrdinal(state, leaseOrdinal(2), disciplines)) {
      return false;
    }
    ++state.emitted[2];
    return true;
  }
  return false;
}

bool eachRecordEmittedOnce(const ModelState& state) {
  for (const auto count : state.emitted) {
    if (count > 1u) return false;
  }
  return true;
}

bool noLegacyRetryAfterSeparator(const ModelState& state) {
  return !state.separatorExecuted || state.disposition != Disposition::Legacy;
}

bool runClosedAcrossSeparator(const ModelState& state) {
  return !state.runStraddledCut;
}

bool rawLocalWitnessSeparated(const ModelState& state) {
  const bool installed = state.stage == Stage::Span0Committed ||
      state.stage == Stage::SeparatorDone || state.stage == Stage::Span2Begun ||
      state.stage == Stage::Done;
  return !installed ||
      (state.slotInterval == IntervalIdentity::SlotAggregate &&
       state.witnessInterval == IntervalIdentity::ActiveRaw &&
       state.witness.producerIdentity == kRawIdentity);
}

bool noPostSettlementExtension(const ModelState& state) {
  return !state.witness.settled ||
         state.stage == Stage::Done || state.stage == Stage::FailStopped;
}

bool failStopIsTerminal(const ModelState& state) {
  return state.stage != Stage::FailStopped ||
         (state.effectsStarted && state.disposition == Disposition::FailStop);
}

struct TraceResult {
  ModelState state{};
  std::size_t appliedSteps = 0;
};

TraceResult runTrace(std::span<const Step> steps, Disciplines disciplines,
                     bool span2Fails) {
  TraceResult result;
  result.state.span2Fails = span2Fails;
  for (const auto step : steps) {
    if (!applyStep(result.state, step, disciplines)) break;
    ++result.appliedSteps;
  }
  return result;
}

void productionTraceHoldsEveryInvariant() {
  // The success interleaving: two direct spans separated by an executed
  // ordered control, each span committing its own destination.
  const std::array success{Step::BeginSpan0, Step::CommitSpan0,
                           Step::ExecuteSeparator, Step::BeginSpan2,
                           Step::CommitSpan2};
  auto result = runTrace(success, Disciplines{}, /*span2Fails=*/false);
  check(result.appliedSteps == success.size(),
        "every production step is enabled in the production configuration");
  check(result.state.stage == Stage::Done,
        "the production trace settles the raw");
  check(eachRecordEmittedOnce(result.state) &&
            noLegacyRetryAfterSeparator(result.state) &&
            runClosedAcrossSeparator(result.state) &&
            rawLocalWitnessSeparated(result.state) &&
            noPostSettlementExtension(result.state) &&
            failStopIsTerminal(result.state),
        "the production trace holds every invariant");
  check(result.state.witness.settled &&
            result.state.witness.lastSpanOrdinal == 1u,
        "the final span settles the witness at the last lease ordinal");

  // The failure interleaving: span 2 fails after the separator executed.
  const std::array failure{Step::BeginSpan0, Step::CommitSpan0,
                           Step::ExecuteSeparator, Step::BeginSpan2,
                           Step::FailSpan2};
  auto failed = runTrace(failure, Disciplines{}, /*span2Fails=*/true);
  check(failed.appliedSteps == failure.size(),
        "the failure trace is fully enabled");
  check(failed.state.stage == Stage::FailStopped &&
            failed.state.disposition == Disposition::FailStop,
        "a post-separator failure is a typed fail-stop");
  check(eachRecordEmittedOnce(failed.state) &&
            noLegacyRetryAfterSeparator(failed.state) &&
            runClosedAcrossSeparator(failed.state) &&
            rawLocalWitnessSeparated(failed.state) &&
            failStopIsTerminal(failed.state),
        "the failure trace holds every invariant");

  // Duplicate and post-settlement spans are simply not enabled.
  const std::array duplicate{Step::BeginSpan0, Step::CommitSpan0,
                             Step::BeginDuplicateSpan0};
  auto duplicated = runTrace(duplicate, Disciplines{}, false);
  check(duplicated.appliedSteps == 2u,
        "a duplicate span ordinal is refused by the witness");
  const std::array afterSettled{Step::BeginSpan0, Step::CommitSpan0,
                                Step::ExecuteSeparator, Step::BeginSpan2,
                                Step::CommitSpan2, Step::BeginAfterSettled};
  auto settled = runTrace(afterSettled, Disciplines{}, false);
  check(settled.appliedSteps == 5u,
        "a post-settlement span is refused by the witness");
  check(eachRecordEmittedOnce(settled.state) &&
            noPostSettlementExtension(settled.state),
        "refusing the post-settlement span keeps the raw emitted once");
}

// Each case below is the native translation of one `.counterexample.cfg`, and
// must violate exactly the invariant that configuration names.
void spanIdentityCounterexampleTrace() {
  const Disciplines relaxed{.spanIdentity = Discipline::Removed};
  const std::array steps{Step::BeginSpan0, Step::CommitSpan0,
                         Step::BeginDuplicateSpan0};
  auto result = runTrace(steps, relaxed, /*span2Fails=*/false);
  check(result.appliedSteps == steps.size(),
        "dropping the witness enables the duplicate span");
  check(!eachRecordEmittedOnce(result.state),
        "ReplayEmissionPlanIslands.span-identity.counterexample violates "
        "EachRecordEmittedOnce");
  check(result.state.emitted[0] == 2u,
        "the raw's first span is emitted twice without the witness");
}

void rawLocalWitnessCounterexampleTrace() {
  const Disciplines aggregateWitness{
      .rawLocalWitness = Discipline::Removed,
  };
  const std::array steps{Step::BeginSpan0, Step::CommitSpan0};
  auto result = runTrace(steps, aggregateWitness, /*span2Fails=*/false);
  check(result.appliedSteps == steps.size(),
        "the populated slot installs the deliberately wrong aggregate witness");
  check(!rawLocalWitnessSeparated(result.state),
        "ReplayEmissionPlanIslands.raw-local-witness.counterexample violates "
        "RawLocalWitnessSeparated");
  check(compatibilitySpanAdmission(result.state.witness, kRaw,
                                   leaseOrdinal(2), kRawIdentity) ==
            CpuReadySpanAdmission::IdentityMismatch,
        "the aggregate witness rejects the unchanged second-span identity");
}

void separatorCutCounterexampleTrace() {
  const Disciplines relaxed{.separatorCut = Discipline::Removed};
  const std::array steps{Step::BeginSpan0, Step::CommitSpan0,
                         Step::ExecuteSeparator, Step::BeginSpan2,
                         Step::FailSpan2};
  auto result = runTrace(steps, relaxed, /*span2Fails=*/true);
  check(result.appliedSteps == steps.size(),
        "dropping the cut enables the whole-raw Legacy retry");
  check(!noLegacyRetryAfterSeparator(result.state),
        "ReplayEmissionPlanIslands.separator-cut.counterexample violates "
        "NoLegacyRetryAfterSeparator");
  check(!eachRecordEmittedOnce(result.state),
        "the retry re-emits the committed prefix and the separator");
}

void runClosureCounterexampleTrace() {
  const Disciplines relaxed{.runClosure = Discipline::Removed};
  const std::array steps{Step::BeginSpan0, Step::CommitSpan0,
                         Step::ExecuteSeparator};
  auto result = runTrace(steps, relaxed, /*span2Fails=*/false);
  check(result.appliedSteps == steps.size(),
        "dropping the closure leaves the run open across the cut");
  check(!runClosedAcrossSeparator(result.state),
        "ReplayEmissionPlanIslands.run-closure.counterexample violates "
        "RunClosedAcrossSeparator");
  // The same trace under the production configuration must be clean, so the
  // difference is the discipline and nothing else.
  auto enforced = runTrace(steps, Disciplines{}, /*span2Fails=*/false);
  check(runClosedAcrossSeparator(enforced.state),
        "the identical trace is clean with the closure enforced");
}

}  // namespace

int main() {
  try {
    spanAdmissionTruthTable();
    productionTraceHoldsEveryInvariant();
    spanIdentityCounterexampleTrace();
    rawLocalWitnessCounterexampleTrace();
    separatorCutCounterexampleTrace();
    runClosureCounterexampleTrace();
  } catch (const std::exception& error) {
    std::cerr << "replay_emission_plan_islands_spec failed: " << error.what()
              << '\n';
    return 1;
  }
  return 0;
}
