#include "render_tape_cpu_mutation_boundary.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using dxmt9::test::CpuTapeMutationResult;
using dxmt9::test::CpuTapeMutationWitness;
using dxmt9::test::cpuTapeMutationDeferralGate;
using dxmt9::test::reduceCpuTapeMutationAdmission;

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void check(bool condition, std::string_view message) {
  if (!condition) throw TestFailure(std::string(message));
}

constexpr std::array<CpuTapeMutationResult, 3> kResults{
    CpuTapeMutationResult::ReleaseBeforeMutation,
    CpuTapeMutationResult::DeferUnrelatedPlain,
    CpuTapeMutationResult::DeferCapturedDiscard,
};

constexpr CpuTapeMutationWitness witnessFromBits(std::size_t bits) noexcept {
  return CpuTapeMutationWitness{
      .exactlyOnePending = (bits & (1u << 0u)) != 0u,
      .alreadyDeferredMutation = (bits & (1u << 1u)) != 0u,
      .bufferResolved = (bits & (1u << 2u)) != 0u,
      .defaultPool = (bits & (1u << 3u)) != 0u,
      .referencedByPending = (bits & (1u << 4u)) != 0u,
      .versioned = (bits & (1u << 5u)) != 0u,
      .discard = (bits & (1u << 6u)) != 0u,
      .noWait = (bits & (1u << 7u)) != 0u,
      .capturedBackingValid = (bits & (1u << 8u)) != 0u,
  };
}

// The independent expected expression is intentionally written in the spec,
// rather than calling the reducer twice.  This pins the nine witnesses and
// makes a dropped conjunction a truth-table failure.
constexpr CpuTapeMutationResult expectedResult(
    const CpuTapeMutationWitness& witness) noexcept {
  const bool gate = witness.exactlyOnePending &&
                    !witness.alreadyDeferredMutation &&
                    witness.bufferResolved && witness.defaultPool &&
                    witness.noWait;
  if (gate && !witness.referencedByPending && !witness.versioned &&
      !witness.discard)
    return CpuTapeMutationResult::DeferUnrelatedPlain;
  if (gate && witness.referencedByPending && witness.versioned &&
      witness.discard && witness.capturedBackingValid)
    return CpuTapeMutationResult::DeferCapturedDiscard;
  return CpuTapeMutationResult::ReleaseBeforeMutation;
}

void testExhaustiveAdmissionTruthTable() {
  std::array<std::size_t, kResults.size()> counts{};
  for (std::size_t bits = 0u; bits < (1u << 9u); ++bits) {
    const auto witness = witnessFromBits(bits);
    const auto expected = expectedResult(witness);
    const auto actual = reduceCpuTapeMutationAdmission(witness);
    check(actual == expected, "CPU mutation reducer truth-table drift");
    check(cpuTapeMutationDeferralGate(witness) ==
              (witness.exactlyOnePending &&
               !witness.alreadyDeferredMutation && witness.bufferResolved &&
               witness.defaultPool && witness.noWait),
          "CPU mutation deferral gate dropped or added a witness");
    for (std::size_t index = 0u; index < kResults.size(); ++index) {
      if (actual == kResults[index]) ++counts[index];
    }
  }

  // There are two capturedBackingValid valuations for the unrelated class,
  // one captured-discard valuation, and every other valuation releases.
  check(counts[0] == 509u, "release class is not the complement of the holds");
  check(counts[1] == 2u, "unrelated-plain class cardinality changed");
  check(counts[2] == 1u, "captured-discard class cardinality changed");
}

enum class MutationLane : std::uint8_t { A, B };

// A tiny source/backing model.  It is deliberately separate from the reducer
// so the trace checks ordering and identity, not only enum equality.
struct TapeModel {
  static constexpr std::uint64_t kInitialGeneration = 41u;
  static constexpr std::uint64_t kInitialBytes = 0x4100u;

  bool pending = false;
  bool sourceCompleted = false;
  bool sourceCompletedBeforeRealLock = false;
  bool enteredRealLockWhilePending = false;
  bool realLockEntered = false;
  bool blockingMapWhilePending = false;
  bool selfWaitCounterexample = false;
  bool secondPendingRejected = false;
  bool secondMutationRejected = false;
  bool deferred = false;
  bool deferredApplied = false;
  bool backingVerifiedBeforeCompletion = false;
  bool capturedBackingRetained = false;
  bool bytesGenerationMismatch = false;
  std::uint32_t mutationCount = 0u;
  std::uint64_t currentBackingGeneration = kInitialGeneration;
  std::uint64_t currentBackingBytes = kInitialBytes;
  std::uint64_t capturedBackingGeneration = 0u;
  std::uint64_t capturedBackingBytes = 0u;
  std::uint64_t retainedBackingGeneration = 0u;
  std::uint64_t retainedBackingBytes = 0u;
  CpuTapeMutationResult decision =
      CpuTapeMutationResult::ReleaseBeforeMutation;

  bool publish() noexcept {
    if (pending) {
      secondPendingRejected = true;
      return false;
    }
    pending = true;
    sourceCompleted = false;
    capturedBackingGeneration = currentBackingGeneration;
    capturedBackingBytes = currentBackingBytes;
    retainedBackingGeneration = capturedBackingGeneration;
    retainedBackingBytes = capturedBackingBytes;
    capturedBackingRetained = true;
    return true;
  }

  bool completeSource() noexcept {
    if (!pending) return false;
    pending = false;
    sourceCompleted = true;
    return true;
  }

  void enterRealLock(bool noWait) noexcept {
    realLockEntered = true;
    enteredRealLockWhilePending = pending;
    sourceCompletedBeforeRealLock = sourceCompleted && !pending;
    // A deferred no-wait mutation is allowed to enter the lock while the
    // source is pending.  Only a blocking map while its own source is pending
    // can wait for the acknowledgement it is withholding.
    if (pending && !noWait) {
      blockingMapWhilePending = true;
      selfWaitCounterexample = true;
    }
  }

  bool mutate(const CpuTapeMutationWitness& witness, bool bypassNoWait = false,
              bool overwriteCapturedOldBacking = false) noexcept {
    if (!pending || mutationCount != 0u) {
      secondMutationRejected = mutationCount != 0u;
      return false;
    }
    ++mutationCount;
    decision = reduceCpuTapeMutationAdmission(witness);

    // Negative control: pretending no-wait admission succeeded enters a
    // blocking map while the source remains pending, exposing the self-wait.
    if (bypassNoWait) {
      deferred = true;
      enterRealLock(false);
      return true;
    }

    if (decision == CpuTapeMutationResult::ReleaseBeforeMutation) {
      if (!completeSource()) return false;
      enterRealLock(witness.noWait);
      return true;
    }

    deferred = true;
    // A safe deferred mutation is a nonblocking real lock plus the actual
    // mutation while the source is pending.  Completion follows the mutation;
    // it is not a substitute for applying the mutation.
    enterRealLock(witness.noWait);
    if (decision == CpuTapeMutationResult::DeferCapturedDiscard) {
      // DISCARD rotates to new storage, while the old generation remains the
      // source's immutable capture until completion.
      currentBackingGeneration = capturedBackingGeneration + 1u;
      currentBackingBytes = capturedBackingBytes + 0x100u;
      if (overwriteCapturedOldBacking) {
        retainedBackingGeneration = currentBackingGeneration;
        retainedBackingBytes = currentBackingBytes;
      }
      backingVerifiedBeforeCompletion =
          retainedBackingGeneration == capturedBackingGeneration &&
          retainedBackingBytes == capturedBackingBytes;
      if (!backingVerifiedBeforeCompletion) bytesGenerationMismatch = true;
    } else {
      backingVerifiedBeforeCompletion = true;
    }
    deferredApplied = true;
    return true;
  }

  bool finishDeferredMutation() noexcept {
    if (!deferred || !deferredApplied) return false;
    return !pending;
  }

  bool completeAndFinish() noexcept {
    if (deferred) {
      if (!backingVerifiedBeforeCompletion) return false;
      if (pending && !completeSource()) return false;
      return finishDeferredMutation();
    }
    return true;
  }
};

constexpr CpuTapeMutationWitness plainWitness() noexcept {
  return CpuTapeMutationWitness{
      .exactlyOnePending = true,
      .alreadyDeferredMutation = false,
      .bufferResolved = true,
      .defaultPool = true,
      .referencedByPending = false,
      .versioned = false,
      .discard = false,
      .noWait = true,
      .capturedBackingValid = false,
  };
}

constexpr CpuTapeMutationWitness capturedDiscardWitness() noexcept {
  return CpuTapeMutationWitness{
      .exactlyOnePending = true,
      .alreadyDeferredMutation = false,
      .bufferResolved = true,
      .defaultPool = true,
      .referencedByPending = true,
      .versioned = true,
      .discard = true,
      .noWait = true,
      .capturedBackingValid = true,
  };
}

void testBoundedPublishMutationCompletion(MutationLane lane,
                                           CpuTapeMutationWitness witness) {
  TapeModel model;
  check(model.publish(), "first publish is admitted");
  check(!model.publish(), "one pending source bounds a second publish");
  check(model.secondPendingRejected,
        "second pending publish is explicitly rejected");
  const auto originalGeneration = model.capturedBackingGeneration;
  const auto originalBytes = model.capturedBackingBytes;

  check(model.mutate(witness), "one bounded mutation is admitted");
  check(model.mutationCount == 1u,
        "bounded model admits at most one mutation");
  check(!model.mutate(witness), "second mutation is rejected");
  check(model.secondMutationRejected,
        "second mutation rejection is observable");

  if (lane == MutationLane::A) {
    check(model.decision == CpuTapeMutationResult::DeferUnrelatedPlain,
          "mutation A uses unrelated-plain hold");
    check(model.pending && model.realLockEntered &&
              model.enteredRealLockWhilePending && model.deferredApplied &&
              !model.selfWaitCounterexample,
          "unrelated no-wait hold mutates while source remains pending");
  } else {
    check(model.decision == CpuTapeMutationResult::DeferCapturedDiscard,
          "mutation B uses captured-discard hold");
    check(model.pending && model.realLockEntered &&
              model.enteredRealLockWhilePending && model.deferredApplied &&
              !model.selfWaitCounterexample &&
              model.backingVerifiedBeforeCompletion,
          "captured discard mutates while retaining its source backing");
    check(model.currentBackingGeneration != originalGeneration &&
              model.capturedBackingGeneration == originalGeneration &&
              model.capturedBackingBytes == originalBytes,
          "discard rotates backing without changing source capture");
  }
  check(model.completeAndFinish(), "completion then deferred mutation succeeds");
  check(!model.pending && model.sourceCompleted && model.realLockEntered &&
            model.enteredRealLockWhilePending &&
            !model.selfWaitCounterexample && !model.blockingMapWhilePending &&
            !model.bytesGenerationMismatch,
        "deferred mutation completes after its safe no-wait real lock");
  if (lane == MutationLane::B) {
    check(model.capturedBackingGeneration == originalGeneration &&
              model.capturedBackingBytes == originalBytes,
          "captured discard retains original backing generation and bytes");
  }
}

void testBoundedStateExplorer() {
  for (std::size_t bits = 0u; bits < (1u << 9u); ++bits) {
    const auto witness = witnessFromBits(bits);
    const auto result = reduceCpuTapeMutationAdmission(witness);
    TapeModel model;
    check(model.publish(), "bounded explorer publishes one source");
    check(model.mutate(witness),
          "bounded explorer admits one mutation transition");
    if (result == CpuTapeMutationResult::ReleaseBeforeMutation) {
      check(!model.pending && model.sourceCompletedBeforeRealLock &&
                model.realLockEntered && !model.blockingMapWhilePending,
            "release path acknowledges before real lock");
      continue;
    }
    check(model.pending && model.realLockEntered &&
              model.enteredRealLockWhilePending && model.deferredApplied &&
              !model.selfWaitCounterexample,
          "deferred path locks nonblocking while source is pending");
    check(model.completeAndFinish(),
          "deferred path reaches lock after completion");
    check(!model.pending && model.enteredRealLockWhilePending &&
              !model.blockingMapWhilePending && !model.bytesGenerationMismatch,
          "every admitted hold completes after its safe real lock");
  }
}

enum class TapeAction : std::uint8_t { Publish, MutateA, MutateB, Complete };

constexpr std::array<TapeAction, 4> kTapeActions{
    TapeAction::Publish,
    TapeAction::MutateA,
    TapeAction::MutateB,
    TapeAction::Complete,
};

void applyAction(TapeModel& model, TapeAction action) {
  switch (action) {
  case TapeAction::Publish:
    (void)model.publish();
    break;
  case TapeAction::MutateA:
    (void)model.mutate(plainWitness());
    break;
  case TapeAction::MutateB:
    (void)model.mutate(capturedDiscardWitness());
    break;
  case TapeAction::Complete:
    (void)model.completeAndFinish();
    break;
  }
}

void checkBoundedStateInvariants(const TapeModel& model) {
  check(model.mutationCount <= 1u,
        "bounded action model never admits a second mutation");
  check(!model.selfWaitCounterexample && !model.blockingMapWhilePending,
        "production action traces never block while withholding acknowledgement");
  if (model.enteredRealLockWhilePending) {
    check(model.deferred && model.deferredApplied && model.decision !=
              CpuTapeMutationResult::ReleaseBeforeMutation,
          "only an admitted deferred mutation enters lock while pending");
  }
  if (model.decision == CpuTapeMutationResult::DeferCapturedDiscard &&
      model.deferredApplied) {
    check(model.backingVerifiedBeforeCompletion,
          "captured discard checks old backing before completion");
  }
}

void enumerateActionSequences(std::size_t length) {
  std::size_t traceCount = 1u;
  for (std::size_t index = 0u; index < length; ++index)
    traceCount *= kTapeActions.size();

  for (std::size_t encoded = 0u; encoded < traceCount; ++encoded) {
    auto value = encoded;
    TapeModel model;
    for (std::size_t step = 0u; step < length; ++step) {
      const auto action = kTapeActions[value % kTapeActions.size()];
      value /= kTapeActions.size();
      applyAction(model, action);
      checkBoundedStateInvariants(model);
    }
  }
}

void testExhaustiveBoundedActionSequences() {
  // Every word of length four and five over Publish/MutateA/MutateB/Complete
  // is explored. Invalid transitions are rejected by the model guards; valid
  // transitions still exercise the same pending/mutation/backing invariants.
  enumerateActionSequences(4u);
  enumerateActionSequences(5u);
}

void testNegativeControls() {
  {
    TapeModel model;
    check(model.publish(), "no-wait control publishes a source");
    auto witness = plainWitness();
    witness.noWait = false;
    check(reduceCpuTapeMutationAdmission(witness) ==
              CpuTapeMutationResult::ReleaseBeforeMutation,
          "no-wait false must decline a hold");
    check(model.mutate(witness, true),
          "negative control forces the invalid hold");
    check(model.selfWaitCounterexample && model.blockingMapWhilePending,
          "bypassing no-wait exposes the self-wait counterexample");
    check(!model.sourceCompletedBeforeRealLock,
          "invalid hold withheld the source acknowledgement");
  }
  {
    TapeModel model;
    check(model.publish(), "old-backing control publishes a source");
    const auto witness = capturedDiscardWitness();
    check(model.mutate(witness, false, true),
          "old-backing control forces one captured discard");
    check(!model.completeAndFinish(),
          "old-backing control rejects completion after corruption");
    check(model.bytesGenerationMismatch,
          "overwriting captured old backing exposes byte/generation mismatch");
  }
  {
    TapeModel model;
    check(model.publish(), "second-pending control publishes source A");
    check(!model.publish(), "second pending source is rejected");
    check(model.secondPendingRejected && model.pending,
          "second pending rejection preserves source A");
  }
}

}  // namespace

int main() {
  try {
    testExhaustiveAdmissionTruthTable();
    testBoundedPublishMutationCompletion(MutationLane::A, plainWitness());
    testBoundedPublishMutationCompletion(MutationLane::B,
                                         capturedDiscardWitness());
    testBoundedStateExplorer();
    testExhaustiveBoundedActionSequences();
    testNegativeControls();
    std::cout << "render tape CPU mutation boundary spec passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "render_tape_cpu_mutation_boundary_spec failed: "
              << error.what() << '\n';
    return 1;
  }
}
