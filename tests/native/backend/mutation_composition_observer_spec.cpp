#include "dxmt9/mutation_composition_observer.hpp"

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
                        std::uint64_t cpuNs = 0u, bool successful = true) {
  mo::MutationEvent result{};
  result.identity = {.resource = resource,
                     .backingGeneration = generation,
                     .sourceOrdinal = ordinal};
  result.disposition = disposition;
  result.byteOffset = offset;
  result.byteSize = bytes;
  result.timing.shadowCopyNs = cpuNs;
  result.successful = successful;
  return result;
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

}  // namespace

int main() {
  testAtoBtoAAndGenerationQualification();
  testMergeUseDistanceAndBarriers();
  testZeroUseDiscardChainFailureAndOverlap();
  testInvalidAndDefaultDisabledPath();
  return 0;
}
