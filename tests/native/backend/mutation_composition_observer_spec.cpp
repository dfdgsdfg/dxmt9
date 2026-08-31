#include "dxmt9/mutation_composition_observer.hpp"
#include "dxmt9/mutation_composition_predicates.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

namespace mo = dxmt9::resources::mutation_observer;

void check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "mutation composition observer: " << message << '\n';
    std::exit(1);
  }
}

mo::MutationEvent event(std::uint64_t resource, std::uint64_t generation,
                        std::uint64_t ordinal, mo::Disposition disposition,
                        std::uint64_t offset, std::uint64_t bytes,
                        std::uint64_t cpuNs = 0u, bool successful = true,
                        dxmt9::resources::MutationSourceKind source =
                            dxmt9::resources::MutationSourceKind::SynchronousMutation) {
  mo::MutationEvent result{};
  result.identity = {.resource = resource,
                     .backingGeneration = generation,
                     .sourceOrdinal = ordinal};
  result.sourceKind = source;
  result.renderTapeIdentity = {.kind = 1u, .generation = 1u, .objectId = 1u};
  result.disposition = disposition;
  result.byteOffset = offset;
  result.byteSize = bytes;
  result.timing.shadowCopyNs = cpuNs;
  result.successful = successful;
  return result;
}

void testMixedSyncAndDeferredShareOrderingDomain() {
  mo::MutationCompositionObserver observer;

  // The production ordinals intentionally disagree: a deferred queue item
  // can carry a large replaySeq while a synchronous unlock's retained source
  // ordinal starts at one.  Composition uses the observer-issued ordering
  // identity, so both valid adjacency directions remain candidates.
  check(observer.recordMutation(event(
            0x2au, 1u, 900u, mo::Disposition::Plain, 0u, 8u, 0u, true,
            dxmt9::resources::MutationSourceKind::SynchronousMutation)),
        "synchronous mutation accepted before deferred mutation");
  check(observer.recordMutation(event(
            0x2au, 1u, 2u, mo::Disposition::Plain, 8u, 8u, 0u, true,
            dxmt9::resources::MutationSourceKind::DeferredMutation)),
        "deferred mutation accepted after synchronous mutation");
  check(observer.snapshot().candidateCalls == 1u,
        "sync-to-deferred adjacency uses one ordering domain");
  check(observer.eventAt(0u)->orderingIdentity.source ==
            dxmt9::resources::MutationSourceKind::SynchronousMutation &&
            observer.eventAt(1u)->orderingIdentity.source ==
            dxmt9::resources::MutationSourceKind::DeferredMutation &&
            observer.eventAt(0u)->orderingIdentity.generation ==
            observer.eventAt(1u)->orderingIdentity.generation,
        "mixed events retain typed generation-qualified identities");

  mo::MutationCompositionObserver reverse;
  check(reverse.recordMutation(event(
            0x2bu, 1u, 900u, mo::Disposition::Plain, 0u, 8u, 0u, true,
            dxmt9::resources::MutationSourceKind::DeferredMutation)),
        "deferred mutation accepted before synchronous mutation");
  check(reverse.recordMutation(event(
            0x2bu, 1u, 2u, mo::Disposition::Plain, 8u, 8u, 0u, true,
            dxmt9::resources::MutationSourceKind::SynchronousMutation)),
        "synchronous mutation accepted after deferred mutation");
  check(reverse.snapshot().candidateCalls == 1u,
        "deferred-to-sync adjacency uses one ordering domain");
}

void testOrderingPolicyGenerationResetFailsClosed() {
  dxmt9::resources::MutationOrderingPolicy policy;
  const auto before = policy.issue(
      dxmt9::resources::MutationSourceKind::SynchronousMutation);
  check(before.valid(), "ordering policy issues a qualified identity");
  check(policy.reset(), "ordering policy advances on reset");
  const auto after = policy.issue(
      dxmt9::resources::MutationSourceKind::DeferredMutation);
  check(after.valid() && after.generation != before.generation,
        "reset qualifies the restarted ordinal");
  check(!dxmt9::resources::mutationOrderingPrecedes(before, after),
        "cross-generation ordering is rejected");
  check(!policy.issue(static_cast<dxmt9::resources::MutationSourceKind>(0xffu))
             .valid(),
        "unsupported source kind is rejected closed");
}

void testAtoBtoAAndGenerationQualification() {
  mo::MutationCompositionObserver observer;
  check(observer.recordMutation(
            event(0x11u, 1u, 1u, mo::Disposition::Plain, 0u, 8u, 3u)),
        "first A mutation accepted");
  check(observer.recordMutation(
            event(0x22u, 1u, 2u, mo::Disposition::Plain, 0u, 8u)),
        "B mutation accepted");
  // Returning to A is not allowed to compose with A's prior generation. The
  // resource handle and backing generation remain part of the source key.
  check(observer.recordMutation(
            event(0x11u, 2u, 3u, mo::Disposition::Plain, 8u, 8u)),
        "new A generation accepted as an observation");
  const auto snapshot = observer.snapshot();
  check(snapshot.mutationCalls == 3u, "A-B-A mutations conserved");
  check(snapshot.rejectionCounts[static_cast<std::size_t>(
            mo::RejectionReason::DifferentGeneration)] == 1u,
        "A generation change is a composition rejection");
  check(observer.eventAt(2u)->identity.resource == 0x11u &&
            observer.eventAt(2u)->identity.backingGeneration == 2u,
        "event retains source-qualified A identity");
}

void testFinalRejectionPreservesReturningGenerationPredecessor() {
  mo::MutationCompositionObserver observer;
  check(observer.recordMutation(
            event(0x12u, 1u, 4u, mo::Disposition::Plain, 0u, 8u)),
        "first generation-one mutation accepted");
  check(observer.recordMutation(
            event(0x12u, 2u, 5u, mo::Disposition::Plain, 0u, 8u)),
        "interleaved generation-two mutation accepted");
  check(observer.recordMutation(
            event(0x12u, 1u, 6u, mo::Disposition::Plain, 8u, 8u)),
        "returning generation-one mutation accepted");
  observer.finalize();
  const auto snapshot = observer.snapshot();
  check(snapshot.candidateCalls == 1u,
        "returning generation compares with its own last event");
  check(snapshot.finalRejectionCounts[static_cast<std::size_t>(
            mo::RejectionReason::DifferentGeneration)] == 1u,
        "only the first event of generation two observes the generation edge");
}

void testMergeUseDistanceAndBarriers() {
  mo::MutationCompositionObserver observer;
  check(observer.recordMutation(
            event(0x33u, 7u, 10u, mo::Disposition::Plain, 0u, 16u)),
        "plain mutation accepted");
  check(observer.recordMutation(event(0x33u, 7u, 11u,
                                      mo::Disposition::NoOverwrite, 16u, 8u,
                                      17u)),
        "disjoint NOOVERWRITE mutation accepted");
  observer.observeUse({.resource = 0x33u,
                       .backingGeneration = 7u,
                       .sourceOrdinal = 14u},
                      mo::ObserverKind::GpuUse);
  // A GPU observer is a conservative barrier to any later same-generation
  // composition, even though the observer itself only records facts.
  check(observer.recordMutation(event(0x33u, 7u, 15u, mo::Disposition::Plain,
                                      24u, 4u)),
        "post-observer mutation accepted for accounting");

  const auto snapshot = observer.snapshot();
  check(snapshot.mergeableRangePairs == 1u, "one range pair is mergeable");
  check(snapshot.mergeableUnionBytes == 24u, "union bytes are conservative");
  check(snapshot.mergeableOverlapBytes == 0u, "disjoint ranges have no overlap");
  check(snapshot.candidateCalls == 1u && snapshot.candidateBytesSaved == 8u,
        "candidate saved bytes count only the later materialization");
  check(snapshot.candidateCpuTimeSavedNs == 17u,
        "candidate saved CPU time is measured later-event time");
  check(snapshot.mutationBytes == 28u && snapshot.shadowCopyNs == 17u,
        "mutation byte and measured phase counters are conserved");
  check(snapshot.firstUseGpuCount == 1u &&
            snapshot.firstUseDistanceTotal == 4u,
        "first GPU use distance is source-ordinal qualified");
  check(observer.eventAt(0u)->firstUseDistance == 4u &&
            observer.eventAt(1u)->firstUseKind == mo::ObserverKind::GpuUse,
        "each mutation retains its first-use distance and observer kind");
  check(snapshot.rejectionCounts[static_cast<std::size_t>(
            mo::RejectionReason::Barrier)] == 1u,
        "GPU use blocks later composition");
  check(snapshot.barrierCounts[static_cast<std::size_t>(
            mo::BarrierReason::Unknown)] == 0u,
        "implicit use does not invent a typed barrier count");
}

void testZeroUseDiscardChainFailureAndOverlap() {
  mo::MutationCompositionObserver observer;
  check(observer.recordMutation(event(0x44u, 1u, 20u,
                                      mo::Disposition::Discard, 0u, 32u)),
        "first discard accepted");
  check(observer.recordMutation(event(0x44u, 2u, 21u,
                                      mo::Disposition::Discard, 0u, 32u)),
        "second discard accepted");
  check(observer.recordMutation(event(0x55u, 1u, 22u,
                                      mo::Disposition::NoOverwrite, 0u, 8u)),
        "overlap fixture first range accepted");
  check(observer.recordMutation(event(0x55u, 1u, 23u,
                                      mo::Disposition::NoOverwrite, 4u, 8u,
                                      9u)),
        "overlap fixture second range accepted");
  check(observer.recordMutation(event(0x66u, 1u, 24u,
                                      mo::Disposition::Plain, 0u, 4u, 0u,
                                      false)),
        "failed event retained for rejection accounting");
  check(observer.recordMutation(event(0x66u, 1u, 25u,
                                      mo::Disposition::Plain, 0u, 4u)),
        "retry event retained after failure");
  observer.observeBarrier({.resource = 0x66u,
                           .backingGeneration = 1u,
                           .sourceOrdinal = 26u},
                          mo::BarrierReason::Failure);
  check(observer.recordMutation(event(0x77u, 1u, 30u,
                                      mo::Disposition::Plain, 0u, 4u)),
        "CPU observer fixture mutation accepted");
  observer.observeUse({.resource = 0x77u,
                       .backingGeneration = 1u,
                       .sourceOrdinal = 31u},
                      mo::ObserverKind::CpuObserver);
  observer.finalize();

  const auto snapshot = observer.snapshot();
  check(snapshot.zeroUseGenerations == 4u,
        "all unobserved generations are counted once");
  check(snapshot.discardToDiscardDeadChains == 1u,
        "discard-to-discard dead chain is visible");
  check(snapshot.firstUseCpuCount == 1u,
        "CPU observer is distinguished from GPU use");
  check(snapshot.rejectionCounts[static_cast<std::size_t>(
            mo::RejectionReason::RangeOverlap)] == 1u,
        "overlapping NOOVERWRITE pair is conservatively rejected");
  check(snapshot.rejectionCounts[static_cast<std::size_t>(
            mo::RejectionReason::Failure)] == 1u,
        "failed adjacent mutation is rejected conservatively");
  check(snapshot.barrierCounts[static_cast<std::size_t>(
            mo::BarrierReason::Failure)] == 1u,
        "typed failure barrier is counted");
}

void testInvalidAndDefaultDisabledPath() {
  mo::MutationCompositionObserver observer;
  check(!observer.recordMutation(event(0u, 1u, 1u, mo::Disposition::Plain,
                                       0u, 1u)),
        "invalid resource identity is rejected");
  check(observer.snapshot().invalidOrDroppedEvents == 1u,
        "invalid identity is conserved as a dropped observation");
  check(mo::activeMutationCompositionObserver() == nullptr,
        "observer is default-disabled");
  {
    mo::ScopedMutationCompositionObserver installed(observer);
    check(mo::activeMutationCompositionObserver() == &observer,
          "explicit installation enables observer");
  }
  check(mo::activeMutationCompositionObserver() == nullptr,
        "scope restores disabled path");
}

void testProductionCompositionTruthTable() {
  const auto base = event(0x88u, 4u, 40u, mo::Disposition::Plain, 0u, 8u);
  const auto candidate = event(0x88u, 4u, 41u, mo::Disposition::Plain, 8u, 8u);
  check(mo::classifyComposition(base, candidate, false) ==
            mo::CompositionDecision::Candidate,
        "production candidate predicate accepts plain adjacent patches");
  auto altered = candidate;
  altered.renderTapeIdentity = {};
  check(mo::classifyComposition(base, altered, false) ==
            mo::CompositionDecision::RenderTapeIdentity,
        "production predicate rejects one missing Render Tape identity");
  altered = candidate;
  auto missingTape = base;
  missingTape.renderTapeIdentity = {};
  altered.renderTapeIdentity = {};
  check(mo::classifyComposition(missingTape, altered, false) ==
            mo::CompositionDecision::RenderTapeIdentity,
        "production predicate rejects two missing Render Tape identities");
  altered = candidate;
  altered.identity.resource++;
  check(mo::classifyComposition(base, altered, false) ==
            mo::CompositionDecision::DifferentResource,
        "production predicate rejects resource mismatch");
  altered = candidate;
  altered.identity.backingGeneration++;
  check(mo::classifyComposition(base, altered, false) ==
            mo::CompositionDecision::DifferentGeneration,
        "production predicate rejects generation mismatch");
  altered = candidate;
  altered.identity.sourceOrdinal = base.identity.sourceOrdinal;
  check(mo::classifyComposition(base, altered, false) ==
            mo::CompositionDecision::SourceOrder,
        "production predicate rejects non-monotone source order");
  check(mo::classifyComposition(base, candidate, true) ==
            mo::CompositionDecision::Barrier,
        "production predicate rejects intervening barrier");
  altered = candidate;
  altered.completion = mo::CompletionDisposition::Pending;
  check(mo::classifyComposition(base, altered, false) ==
            mo::CompositionDecision::Completion,
        "production predicate rejects incomplete event");
  altered = candidate;
  altered.disposition = mo::Disposition::Discard;
  check(mo::classifyComposition(base, altered, false) ==
            mo::CompositionDecision::Disposition,
        "production predicate rejects discard freshness boundary");
  altered = candidate;
  altered.disposition = mo::Disposition::NoOverwrite;
  altered.byteOffset = 4u;
  check(mo::classifyComposition(
            event(0x88u, 4u, 40u, mo::Disposition::NoOverwrite, 0u, 8u),
            altered, false) == mo::CompositionDecision::RangeOverlap,
        "production predicate rejects overlapping no-overwrite ranges");
}

void testCompletionOverflowAndReset() {
  mo::MutationCompositionObserver observer;
  auto pending = event(0x99u, 1u, 50u, mo::Disposition::Plain, 0u, 4u);
  pending.completion = mo::CompletionDisposition::Pending;
  check(observer.recordMutation(pending), "pending mutation accepted");
  observer.settleMutation(pending.identity,
                          mo::CompletionDisposition::Completed);
  pending.identity.sourceOrdinal = 51u;
  check(observer.recordMutation(pending), "failed pending mutation accepted");
  observer.settleMutation(pending.identity,
                          mo::CompletionDisposition::Failed);
  pending.identity.sourceOrdinal = 52u;
  check(observer.recordMutation(pending), "discarded pending mutation accepted");
  observer.settleMutation(pending.identity,
                          mo::CompletionDisposition::Discarded);
  auto snapshot = observer.snapshot();
  check(snapshot.completedMutations == 1u && snapshot.failedMutations == 1u &&
            snapshot.discardedMutations == 1u,
        "completion dispositions settle exactly once");
  check(snapshot.pendingMutations == 0u &&
            snapshot.rejectionCounts[static_cast<std::size_t>(
                mo::RejectionReason::Completion)] != 0u,
        "record-time pending adjacency is retained as provisional evidence");
  observer.finalize();
  snapshot = observer.snapshot();
  check(snapshot.finalRejectionCounts[static_cast<std::size_t>(
                mo::RejectionReason::Completion)] == 0u &&
            snapshot.finalRejectionCounts[static_cast<std::size_t>(
                mo::RejectionReason::Failure)] != 0u,
        "final rejection view reflects settled terminal dispositions");

  for (std::size_t i = observer.eventCount();
       i < mo::MutationCompositionObserver::kMaxEvents + 1u; ++i) {
    auto bounded = event(0xaau, 1u, 100u + i, mo::Disposition::Plain, 0u, 1u);
    check(observer.recordMutation(bounded) ||
              observer.snapshot().invalidOrDroppedEvents != 0u,
          "capacity overflow is observable");
  }
  check(observer.snapshot().invalidOrDroppedEvents != 0u,
        "event capacity overflow is counted");
  check(observer.snapshot().overflowEvents != 0u,
        "event capacity overflow is reported separately from invalid input");
  observer.notePresent();
  observer.reset();
  check(observer.eventCount() == 0u && observer.windowPresents() == 0u &&
            observer.snapshot().mutationCalls == 0u,
        "window reset clears bounded state");
}

}  // namespace

int main() {
  testMixedSyncAndDeferredShareOrderingDomain();
  testOrderingPolicyGenerationResetFailsClosed();
  testAtoBtoAAndGenerationQualification();
  testFinalRejectionPreservesReturningGenerationPredecessor();
  testMergeUseDistanceAndBarriers();
  testZeroUseDiscardChainFailureAndOverlap();
  testInvalidAndDefaultDisabledPath();
  testProductionCompositionTruthTable();
  testCompletionOverflowAndReset();
  return 0;
}
