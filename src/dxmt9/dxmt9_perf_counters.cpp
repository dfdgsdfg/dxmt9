#include "dxmt9_perf_counters.hpp"

#include "dxmt9/core.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

namespace dxmt9::perf {
namespace {

thread_local D3D9SnapshotUniformBuildContext
    gD3D9SnapshotUniformBuildContext =
        D3D9SnapshotUniformBuildContext::None;

bool inD3D9SnapshotUniformBuildBatchMissContext() noexcept {
  return gD3D9SnapshotUniformBuildContext ==
      D3D9SnapshotUniformBuildContext::BatchMiss;
}

// 256-sample sliding ring of nanosecond samples used to compute P50/P95/P99
// in the shutdown report. Lock-free: writers race only on `head_` (atomic
// fetch_add, relaxed) and on the slot store (relaxed). Two threads racing for
// the same slot may overwrite each other's sample, but at the per-draw rate
// the steady-state percentile estimate is unaffected. Reading is one-shot at
// process exit, so a coarse snapshot via relaxed loads is acceptable.
//
// Capacity 256 keeps the per-counter footprint small (~2 KiB of atomics) and
// pushes P99 rank from 63/64 to 253/256, where a single late outlier no longer
// shifts the published percentile by a full bucket. Sort cost at shutdown is
// O(N log N) on N=256 — irrelevant against the rest of the report.
class PercentileRing {
 public:
  static constexpr std::size_t kCapacity = 256;

  void record(std::uint64_t nanoseconds) {
    const auto slot =
        head_.fetch_add(1, std::memory_order_relaxed) % kCapacity;
    samples_[slot].store(nanoseconds, std::memory_order_relaxed);
    auto current = count_.load(std::memory_order_relaxed);
    while (current < kCapacity &&
           !count_.compare_exchange_weak(current, current + 1,
                                         std::memory_order_relaxed)) {
    }
  }

  std::vector<std::uint64_t> snapshot() const {
    const auto size = std::min<std::size_t>(
        count_.load(std::memory_order_relaxed), kCapacity);
    std::vector<std::uint64_t> out;
    out.reserve(size);
    for (std::size_t i = 0; i < size; ++i) {
      out.push_back(samples_[i].load(std::memory_order_relaxed));
    }
    std::sort(out.begin(), out.end());
    return out;
  }

  // p in [0.0, 1.0]. Returns 0 when the ring is empty. Uses nearest-rank
  // (ceil) over the sorted snapshot, stable for the kCapacity range used here
  // and matching what runbook scripts expect.
  std::uint64_t percentile(double p) const {
    const auto sorted = snapshot();
    if (sorted.empty()) {
      return 0;
    }
    if (p <= 0.0) {
      return sorted.front();
    }
    if (p >= 1.0) {
      return sorted.back();
    }
    auto rank = static_cast<std::size_t>(p * static_cast<double>(sorted.size()));
    if (rank >= sorted.size()) {
      rank = sorted.size() - 1;
    }
    return sorted[rank];
  }

 private:
  std::array<std::atomic<std::uint64_t>, kCapacity> samples_{};
  std::atomic<std::uint32_t> head_{0};
  std::atomic<std::uint32_t> count_{0};
};

struct Counters {
  // R-BACK-2.10 / 2.27 admit + ring heap fallback gauges.
  std::atomic<std::uint64_t> chunkAdmit{0};
  std::atomic<std::uint64_t> chunkReject{0};
  // Retired schema fields kept at zero so existing perf-result parsers retain
  // a stable column set across the V1 removal.
  std::atomic<std::uint64_t> commandChunkV2Chunks{0};
  std::atomic<std::uint64_t> commandChunkV2Records{0};
  std::atomic<std::uint64_t> commandChunkV2Bytes{0};
  std::atomic<std::uint64_t> commandChunkV2Rejects{0};
  std::atomic<std::uint64_t> commandChunkV2RegistryResolutions{0};
  std::atomic<std::uint64_t> ringArenaHeapFallbackCount{0};
  std::atomic<std::uint64_t> ringArenaHeapFallbackBytes{0};
  std::atomic<std::uint64_t> ringArenaHeapFallbackCountArgbuf{0};
  std::atomic<std::uint64_t> ringArenaHeapFallbackCountLambda{0};
  std::atomic<std::uint64_t> ringArenaHeapFallbackCountStaging{0};
  std::atomic<std::uint64_t> ringArenaHeapFallbackCountCopyTemp{0};
  std::atomic<std::uint64_t> submitDraw{0};
  std::atomic<std::uint64_t> submitClear{0};
  std::atomic<std::uint64_t> submitStretch{0};
  std::atomic<std::uint64_t> stretchBlitCopy{0};
  std::atomic<std::uint64_t> stretchRenderPass{0};
  std::atomic<std::uint64_t> stretchFullscreen{0};
  std::atomic<std::uint64_t> submitPresent{0};
  std::atomic<std::uint64_t> submitFlush{0};
  std::atomic<std::uint64_t> submitPresentCpuNs{0};
  std::atomic<std::uint64_t> submitPresentCpuMaxNs{0};
  std::atomic<std::uint64_t> submitPresentAcquireCpuNs{0};
  std::atomic<std::uint64_t> submitPresentAcquireCpuMaxNs{0};
  std::atomic<std::uint64_t> submitPresentCommitCpuNs{0};
  std::atomic<std::uint64_t> submitPresentCommitCpuMaxNs{0};
  std::atomic<std::uint64_t> submitPresentBoundaryCpuNs{0};
  std::atomic<std::uint64_t> submitPresentBoundaryCpuMaxNs{0};
  std::atomic<std::uint64_t> prepareSlotForPublishCpuNs{0};
  std::atomic<std::uint64_t> prepareSlotForPublishCpuMaxNs{0};
  std::atomic<std::uint64_t> prepareSlotResourceMarkCpuNs{0};
  std::atomic<std::uint64_t> prepareSlotResourceMarkCpuMaxNs{0};
  std::atomic<std::uint64_t> unpublishedSlotPsoPrefetchCpuNs{0};
  std::atomic<std::uint64_t> unpublishedSlotPsoPrefetchCpuMaxNs{0};
  std::atomic<std::uint64_t> chunkPublishReasonUnknown{0};
  std::atomic<std::uint64_t> chunkPublishReasonDrawLimit{0};
  std::atomic<std::uint64_t> chunkPublishReasonPayloadLimit{0};
  std::atomic<std::uint64_t> chunkPublishReasonPresent{0};
  std::atomic<std::uint64_t> chunkPublishReasonPresentAcquire{0};
  std::atomic<std::uint64_t> chunkPublishReasonFlush{0};
  std::atomic<std::uint64_t> chunkPublishReasonStretchSplit{0};
  std::atomic<std::uint64_t> chunkPublishReasonMapWait{0};
  std::atomic<std::uint64_t> chunkPublishReasonPresentSplitBefore{0};
  std::atomic<std::uint64_t> chunkPublishReasonSemanticBoundary{0};
  std::atomic<std::uint64_t> chunkPublishCommandsUnknown{0};
  std::atomic<std::uint64_t> chunkPublishCommandsDrawLimit{0};
  std::atomic<std::uint64_t> chunkPublishCommandsPayloadLimit{0};
  std::atomic<std::uint64_t> chunkPublishCommandsPresent{0};
  std::atomic<std::uint64_t> chunkPublishCommandsPresentAcquire{0};
  std::atomic<std::uint64_t> chunkPublishCommandsFlush{0};
  std::atomic<std::uint64_t> chunkPublishCommandsStretchSplit{0};
  std::atomic<std::uint64_t> chunkPublishCommandsMapWait{0};
  std::atomic<std::uint64_t> chunkPublishCommandsPresentSplitBefore{0};
  std::atomic<std::uint64_t> chunkPublishCommandsSemanticBoundary{0};
  // H229 open-CB overlap carrier (DXMT9_OPEN_CB_CARRIER) — minimal
  // mechanism-proof set. Publication side:
  std::atomic<std::uint64_t> openCbCarrierWaitStartPublished{0};
  std::atomic<std::uint64_t> openCbCarrierActiveWaitPublished{0};
  std::atomic<std::uint64_t> openCbCarrierProducerWaitPublished{0};
  std::atomic<std::uint64_t> openCbCarrierAttachmentBoundaryPublished{0};
  // Carrier lifecycle on the encode thread:
  std::atomic<std::uint64_t> openCbCarrierPendingStarted{0};
  std::atomic<std::uint64_t> openCbCarrierPendingStartedInWait{0};
  std::atomic<std::uint64_t> openCbCarrierHeadAppended{0};
  std::atomic<std::uint64_t> openCbCarrierTailSubmitted{0};
  // Release attribution (why a pending open CB was submitted early):
  std::atomic<std::uint64_t> openCbCarrierReleasedSemanticWait{0};
  std::atomic<std::uint64_t> openCbCarrierReleasedProducerWait{0};
  std::atomic<std::uint64_t> openCbCarrierReleasedNonAppendable{0};
  std::atomic<std::uint64_t> openCbCarrierReleasedInitializerWait{0};
  std::atomic<std::uint64_t> openCbCarrierReleasedDrain{0};
  std::atomic<std::uint64_t> openCbCarrierReleasedFailPath{0};
  // Tape-gated CPU-ready session join lane (DXMT9_CPU_READY_TAPE):
  std::atomic<std::uint64_t> cpuReadySessionPendingStarted{0};
  std::atomic<std::uint64_t> cpuReadySessionHeadAppended{0};
  std::atomic<std::uint64_t> cpuReadySessionArenaHeadAppended{0};
  std::atomic<std::uint64_t> cpuReadySessionTailSubmitted{0};
  std::atomic<std::uint64_t> cpuReadyRetainedHeadAttempts{0};
  std::atomic<std::uint64_t> cpuReadyRetainedHeadHeld{0};
  std::atomic<std::uint64_t> cpuReadyRetainedHeadLive{0};
  std::atomic<std::uint64_t> cpuReadyRetainedHeadPeak{0};
  std::atomic<std::uint64_t> cpuReadyRetainedHeadSuccessorReady{0};
  std::atomic<std::uint64_t> cpuReadyRetainedHeadFallbackRelease{0};
  std::atomic<std::uint64_t> cpuReadyRetainedHeadFallbackProducerWait{0};
  std::atomic<std::uint64_t> cpuReadyRetainedHeadFallbackInitializer{0};
  std::atomic<std::uint64_t> cpuReadyRetainedHeadFallbackStop{0};
  std::atomic<std::uint64_t> cpuReadyRetainedHeadFallbackWriterGone{0};
  std::atomic<std::uint64_t> cpuReadyRetainedHeadFallbackPressure{0};
  std::atomic<std::uint64_t> cpuReadyRetainedHeadRestoreFailure{0};
  std::atomic<std::uint64_t> cpuReadyRetainedHeadWaitNs{0};
  std::atomic<std::uint64_t> completionSpanShadowBuilt{0};
  std::atomic<std::uint64_t> completionSpanShadowValidated{0};
  std::atomic<std::uint64_t> completionSpanShadowMismatch{0};
  std::atomic<std::uint64_t> completionSpanShadowSourceCount{0};
  std::atomic<std::uint64_t> postEncodeRetireAttempts{0};
  std::atomic<std::uint64_t> postEncodeRetireSuccess{0};
  std::atomic<std::uint64_t> postEncodeRetireSuccessArena{0};
  std::atomic<std::uint64_t> postEncodeRetireSuccessLegacy{0};
  std::atomic<std::uint64_t> postEncodeRetireIneligibleNone{0};
  std::atomic<std::uint64_t> postEncodeRetireIneligiblePendingClear{0};
  std::atomic<std::uint64_t> postEncodeRetireIneligiblePresent{0};
  std::atomic<std::uint64_t> postEncodeRetireIneligibleReadback{0};
  std::atomic<std::uint64_t> postEncodeRetireIneligibleUpdateSurface{0};
  std::atomic<std::uint64_t> postEncodeRetireIneligibleOrderedControl{0};
  std::atomic<std::uint64_t> postEncodeRetireIneligiblePayloadBorrow{0};
  std::atomic<std::uint64_t> postEncodeRetireIneligibleNotOldest{0};
  std::atomic<std::uint64_t> postEncodeRetireIneligibleReceiptCapacity{0};
  std::atomic<std::uint64_t> postEncodeRetireIneligibleInvalid{0};
  std::atomic<std::uint64_t> postEncodeReceiptFailureInvalid{0};
  std::atomic<std::uint64_t> postEncodeReceiptFailureDuplicate{0};
  std::atomic<std::uint64_t> postEncodeReceiptFailureCapacity{0};
  std::atomic<std::uint64_t> postEncodeReceiptFailureStale{0};
  std::atomic<std::uint64_t> postEncodeReceiptFailureWrongState{0};
  std::atomic<std::uint64_t> postEncodeReceiptFailureOther{0};
  std::atomic<std::uint64_t> postEncodeReceiptDepth{0};
  std::atomic<std::uint64_t> postEncodeReceiptPeak{0};
  std::atomic<std::uint64_t> postEncodeResidencySourcesReleased{0};
  std::atomic<std::uint64_t> postEncodeResidencyPagesReleased{0};
  std::atomic<std::uint64_t> postEncodeResidencyBytesReleased{0};
  std::atomic<std::uint64_t> postEncodeWorkCapCloses{0};
  std::atomic<std::uint64_t> gpuOutstandingCompletionSources{0};
  std::atomic<std::uint64_t> gpuOutstandingCompletionSourcesPeak{0};
  std::atomic<std::uint64_t> cpuReadySessionReleasedProducerWait{0};
  std::atomic<std::uint64_t> cpuReadySessionReleasedNonAppendable{0};
  std::atomic<std::uint64_t> cpuReadySessionReleasedInitializerWait{0};
  std::atomic<std::uint64_t> cpuReadySessionReleasedDrain{0};
  std::atomic<std::uint64_t> cpuReadySessionReleasedFailPath{0};
  std::atomic<std::uint64_t> cpuReadySessionLeaseCurrent{0};
  std::atomic<std::uint64_t> cpuReadySessionLeasePeak{0};
  std::atomic<std::uint64_t> cpuReadySessionLeaseAcquisitions{0};
  std::atomic<std::uint64_t> cpuReadySessionLeaseDenials{0};
  std::atomic<std::uint64_t> cpuReadySessionLeaseReservedSources{0};
  std::atomic<std::uint64_t> cpuReadySessionLeaseReservedPages{0};
  std::atomic<std::uint64_t> cpuReadySessionLeaseReservedBytes{0};
  std::atomic<std::uint64_t> cpuReadySessionLeaseReservedDraws{0};
  std::atomic<std::uint64_t> cpuReadySessionLeaseUsedSources{0};
  std::atomic<std::uint64_t> cpuReadySessionLeaseUsedPages{0};
  std::atomic<std::uint64_t> cpuReadySessionLeaseUsedBytes{0};
  std::atomic<std::uint64_t> cpuReadySessionLeaseUsedDraws{0};
  std::atomic<std::uint64_t> cpuReadySessionLeaseSlackSources{0};
  std::atomic<std::uint64_t> cpuReadySessionLeaseSlackPages{0};
  std::atomic<std::uint64_t> cpuReadySessionLeaseSlackBytes{0};
  std::atomic<std::uint64_t> cpuReadySessionLeaseSlackDraws{0};
  std::atomic<std::uint64_t> cpuReadySessionSuccessorHeadroomMinPages{0};
  std::atomic<std::uint64_t> cpuReadySessionCapSources{0};
  std::atomic<std::uint64_t> cpuReadySessionCapPages{0};
  std::atomic<std::uint64_t> cpuReadySessionCapBytes{0};
  std::atomic<std::uint64_t> cpuReadySessionCapDraws{0};
  std::atomic<std::uint64_t> cpuReadySessionCapCommandBuffers{0};
  std::atomic<std::uint64_t> cpuReadySessionCapRequirementSourcesOnly{0};
  std::atomic<std::uint64_t> cpuReadySessionCapRequirementPagesOnly{0};
  std::atomic<std::uint64_t> cpuReadySessionCapRequirementSourcesAndPages{0};
  std::atomic<std::uint64_t> cpuReadySessionCapPredecessorSourcesPeak{0};
  std::atomic<std::uint64_t> cpuReadySessionCapPredecessorPagesPeak{0};
  std::atomic<std::uint64_t> cpuReadySessionCapCandidatePayloadPagesPeak{0};
  std::atomic<std::uint64_t> cpuReadySessionCapCandidateWrapPaddingPagesPeak{0};
  std::atomic<std::uint64_t> cpuReadySessionCapCandidateRequiredPagesPeak{0};
  std::atomic<std::uint64_t> cpuReadySessionCapRequiredTotalSourcesPeak{0};
  std::atomic<std::uint64_t> cpuReadySessionCapRequiredTotalPagesPeak{0};
  std::atomic<std::uint64_t> cpuReadySessionIsolated{0};
  std::atomic<std::uint64_t> cpuReadySessionIsolatedPresent{0};
  std::atomic<std::uint64_t> cpuReadySessionIsolatedCapacityBytes{0};
  std::atomic<std::uint64_t> cpuReadySessionIsolatedOther{0};
  std::atomic<std::uint64_t> cpuReadySessionLegacyRollback{0};
  std::atomic<std::uint64_t> cpuReadySessionInvalidDisposition{0};
  std::atomic<std::uint64_t> cpuReadyMultiSourceWindowsAttempted{0};
  std::atomic<std::uint64_t> cpuReadyMultiSourceWindowsPlanned{0};
  std::atomic<std::uint64_t> cpuReadyMultiSourceWindowSources{0};
  std::atomic<std::uint64_t> cpuReadyMultiSourceWindowCommands{0};
  std::atomic<std::uint64_t> cpuReadyMultiSourceWindowRuns{0};
  std::atomic<std::uint64_t> cpuReadyMultiSourceFallbackEligibility{0};
  std::atomic<std::uint64_t> cpuReadyMultiSourceFallbackNaturalPlan{0};
  std::atomic<std::uint64_t> cpuReadyMultiSourceFallbackInvalidPlan{0};
  std::atomic<std::uint64_t> cpuReadyMultiSourceFallbackRepeatedSource{0};
  std::atomic<std::uint64_t> cpuReadyMultiSourceFallbackResolvedSource{0};
  std::atomic<std::uint64_t> cpuReadyMultiSourceFallbackCompletionSource{0};
  std::atomic<std::uint64_t> cpuReadyMultiSourceFallbackAdmission{0};
  std::atomic<std::uint64_t> cpuReadyMultiSourceFallbackFragmentRange{0};
  std::atomic<std::uint64_t> cpuReadyMultiSourceFallbackCarrier{0};
  std::atomic<std::uint64_t>
      cpuReadyMultiSourceNaturalFallbackWindowsStarted{0};
  std::atomic<std::uint64_t>
      cpuReadyMultiSourceNaturalFallbackWindowsCompleted{0};
  std::atomic<std::uint64_t> cpuReadyMultiSourceNaturalFallbackSources{0};
  std::atomic<std::uint64_t>
      cpuReadyMultiSourcePermutationFallbackWindowsStarted{0};
  std::atomic<std::uint64_t>
      cpuReadyMultiSourcePermutationFallbackWindowsCompleted{0};
  std::atomic<std::uint64_t>
      cpuReadyMultiSourcePermutationFallbackSources{0};
  std::atomic<std::uint64_t>
      cpuReadyMultiSourceEligibilityActiveIncomplete{0};
  std::atomic<std::uint64_t> cpuReadyMultiSourceEligibilityPresent{0};
  std::atomic<std::uint64_t>
      cpuReadyMultiSourceEligibilityNonConsecutiveIdentity{0};
  std::atomic<std::uint64_t> cpuReadyMultiSourceEligibilityOtherBoundary{0};
  std::atomic<std::uint64_t> cpuReadyMultiSourcePlannerInvalidInput{0};
  std::atomic<std::uint64_t> cpuReadyMultiSourcePlannerSeedRejected{0};
  std::atomic<std::uint64_t> cpuReadyMultiSourcePlannerNoActiveTargetMatch{0};
  std::atomic<std::uint64_t> cpuReadyMultiSourcePlannerNoMerge{0};
  std::atomic<std::uint64_t> cpuReadyMultiSourcePlannerNaturalAfterMerge{0};
  std::atomic<std::uint64_t> cpuReadyMultiSourcePlannerPermutationRejected{0};
  std::atomic<std::uint64_t> cpuReadyMultiSourcePlannerMovedHeadUnproved{0};
  std::atomic<std::uint64_t> cpuReadyMultiSourcePlannerPlanned{0};
  std::atomic<std::uint64_t> cpuReadyMultiSourcePlannerMergeSeed{0};
  std::atomic<std::uint64_t> cpuReadyMultiSourcePlannerMergeNonSeedOnly{0};
  std::atomic<std::uint64_t>
      cpuReadyMultiSourcePlannerSeedNaturalMatchDistance1{0};
  std::atomic<std::uint64_t>
      cpuReadyMultiSourcePlannerSeedNaturalMatchDistanceGt1{0};
  std::atomic<std::uint64_t>
      cpuReadyMultiSourcePlannerSeedNaturalMatchDistanceMissing{0};
  std::atomic<std::uint64_t>
      cpuReadyMultiSourcePlannerSeedNaturalMergeOperations{0};
  std::atomic<std::uint64_t>
      cpuReadyMultiSourcePlannerSeedNaturalMergeDistanceTotal{0};
  std::atomic<std::uint64_t>
      cpuReadyMultiSourcePlannerSeedNaturalMergeDistanceMax{0};
  std::atomic<std::uint64_t>
      cpuReadyMultiSourcePlannerSeedNaturalCommandBefore{0};
  std::atomic<std::uint64_t>
      cpuReadyMultiSourcePlannerSeedNaturalCommandAfter{0};
  std::atomic<std::uint64_t>
      cpuReadyMultiSourcePlannerSeedNaturalEmptyIntervening{0};
  std::atomic<std::uint64_t>
      cpuReadyMultiSourcePlannerSeedNaturalShapeAdjacent{0};
  std::atomic<std::uint64_t>
      cpuReadyMultiSourcePlannerSeedNaturalShapeDependencyKept{0};
  std::atomic<std::uint64_t>
      cpuReadyMultiSourcePlannerSeedNaturalShapeCommandless{0};
  std::atomic<std::uint64_t>
      cpuReadyMultiSourcePlannerSeedNaturalShapeMultiMerge{0};
  std::atomic<std::uint64_t>
      cpuReadyMultiSourcePlannerSeedNaturalShapeMissing{0};
  std::atomic<std::uint64_t> cpuReadyMultiSourcePlannerSeedSecondNonDraw{0};
  std::atomic<std::uint64_t> cpuReadyMultiSourcePlannerSeedBlockedCycle{0};
  std::atomic<std::uint64_t> cpuReadyMultiSourceFatalEncodeNull{0};
  std::atomic<std::uint64_t> cpuReadyMultiSourceFatalCarrierFold{0};
  std::atomic<std::uint64_t> cpuReadyMultiSourceCompletionSources{0};
  std::atomic<std::uint64_t> cpuReadyMultiSourceCompletionFifoFailures{0};
  std::atomic<std::uint64_t> chunkPublishSlotResidencySamples{0};
  std::atomic<std::uint64_t> chunkPublishSlotResidencyNs{0};
  std::atomic<std::uint64_t> chunkPublishSlotResidencyMaxNs{0};
  std::atomic<std::uint64_t> chunkPublishSlotResidencyPresentSamples{0};
  std::atomic<std::uint64_t> chunkPublishSlotResidencyPresentNs{0};
  std::atomic<std::uint64_t> chunkPublishSlotResidencyPresentMaxNs{0};
  std::atomic<std::uint64_t> chunkPublishSlotResidencyNonPresentSamples{0};
  std::atomic<std::uint64_t> chunkPublishSlotResidencyNonPresentNs{0};
  std::atomic<std::uint64_t> chunkPublishSlotResidencyNonPresentMaxNs{0};
  std::atomic<std::uint64_t> chunkPublishPresentPrePresentOpportunitySlots{0};
  std::atomic<std::uint64_t> chunkPublishPresentPrePresentOpportunityTailSlots{0};
  std::atomic<std::uint64_t> chunkPublishPresentPrePresentOpportunityNonTailSlots{0};
  std::atomic<std::uint64_t> chunkPublishPresentPrePresentOpportunityCommands{0};
  std::atomic<std::uint64_t> chunkPublishPresentPrePresentOpportunityDrawRuns{0};
  std::atomic<std::uint64_t> chunkPublishPresentPrePresentOpportunityDrawItems{0};
  std::atomic<std::uint64_t> chunkPublishPresentPrePresentOpportunityNonDrawCommands{0};
  std::atomic<std::uint64_t> chunkPublishPresentPrePresentOpportunityPayloadBytes{0};
  std::atomic<std::uint64_t> chunkPublishPresentPrePresentOpportunityResidencyNs{0};
  std::atomic<std::uint64_t> chunkPublishPresentPrePresentOpportunityResidencyMaxNs{0};
  std::atomic<std::uint64_t> chunkPublishPresentPrePresentOpportunityTailEmpty{0};
  std::atomic<std::uint64_t> chunkPublishPresentPrePresentOpportunityTailDrawRun{0};
  std::atomic<std::uint64_t> chunkPublishPresentPrePresentOpportunityTailClear{0};
  std::atomic<std::uint64_t> chunkPublishPresentPrePresentOpportunityTailSurfaceCopy{0};
  std::atomic<std::uint64_t> chunkPublishPresentPrePresentOpportunityTailStretchRect{0};
  std::atomic<std::uint64_t> chunkPublishPresentPrePresentOpportunityTailReadback{0};
  std::atomic<std::uint64_t> chunkPublishPresentPrePresentOpportunityTailColorFill{0};
  std::atomic<std::uint64_t> chunkPublishPresentPrePresentOpportunityTailDepthResolve{0};
  std::atomic<std::uint64_t> chunkPublishPresentPrePresentOpportunityTailPresent{0};
  std::atomic<std::uint64_t> chunkPublishPresentPrePresentOpportunityDrawOnly{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchCpuNs{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchCpuMaxNs{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchCommands{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchCandidates{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchTileCandidates{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchArgbufStage2Candidates{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchArgbufResourceArrayCandidates{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchStateCopyCpuNs{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchStateCopyCpuMaxNs{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchDepthLookupCpuNs{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchDepthLookupCpuMaxNs{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchTileSelectCpuNs{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchTileSelectCpuMaxNs{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchTileBaseLookupCpuNs{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchTileBaseLookupCpuMaxNs{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchTileDrawLookupCpuNs{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchTileDrawLookupCpuMaxNs{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchArgbufSelectCpuNs{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchArgbufSelectCpuMaxNs{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchDrawKeyResolveCpuNs{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchDrawResolveFormatCpuNs{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchDrawResolveVariantKeyCpuNs{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchDrawResolveShaderContextCpuNs{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchDrawResolveX8AlphaCpuNs{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchDrawResolveVsoutLayoutCpuNs{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchDrawResolveFragmentlessCpuNs{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchDrawLookupCpuNs{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchDrawLookupCpuMaxNs{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchDrawSemanticKeyCpuNs{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchDrawSemanticProbeCpuNs{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchDrawSemanticStoreCpuNs{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchDrawSemanticMemoHits{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchDrawSemanticMemoMisses{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchDrawSemanticMemoOverflow{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchDrawSemanticMissProbeKeyHits{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchDrawSemanticMissProbeKeySameSemantic{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffArgbufSelector{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffVertexDecl{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffShader{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffRenderState{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffTextureHandles{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffTextureLod{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffTextureStage{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffSampler{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffAttachment{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffClipPlane{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffConstantUsage{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffSingleField{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffMultiField{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffTextureHandlesOnly{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffTextureHandlesWithOthers{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffHashOnly{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffUnknown{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchDrawResourceShapeMemoCandidates{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchDrawResourceShapeMemoHits{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchDrawResourceShapeMemoMisses{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchDrawResourceShapeMemoOverflow{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchDrawResourceShapeMemoStores{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchDrawResourceShapeMemoValidatedHits{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchDrawResourceShapeMemoValidatedMisses{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchDrawResourceShapeMemoMismatchTextureMask{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchDrawResourceShapeMemoMismatchTextureTypes{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchDrawResourceShapeMemoMismatchX8Alpha{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchDrawResourceShapeMemoMismatchAttachment{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchDrawResourceShapeMemoMismatchSamplerLodBias{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchDrawResourceShapeMemoMismatchVsOut{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchDrawResourceShapeMemoMismatchOther{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchDrawProbeKeyMemoHits{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchDrawProbeKeyMemoMisses{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchDrawProbeKeyMemoOverflow{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchDrawHandleAdjacentCandidates{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchDrawHandleAdjacentHits{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchDrawHandleSlotRepeatHits{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchDrawHandleSlotUnique{0};
  std::atomic<std::uint64_t> encodeSlotPsoPrefetchDrawHandleSlotOverflow{0};
  std::atomic<std::uint64_t> submitDrawRunBatchGroups{0};
  std::atomic<std::uint64_t> submitDrawRunBatchRecords{0};
  std::atomic<std::uint64_t> submitDrawRunBatchMaxRecords{0};
  std::atomic<std::uint64_t> submitDrawRunBatchDiscardedStateRecords{0};
  std::atomic<std::uint64_t> submitDrawRunBatchDiscardedStateBytes{0};
  std::atomic<std::uint64_t> submitDrawRunBindingSnapshotCpuNs{0};
  std::atomic<std::uint64_t> submitDrawRunBindingSnapshotCpuMaxNs{0};
  std::atomic<std::uint64_t> submitDrawRunPayloadBytesCpuNs{0};
  std::atomic<std::uint64_t> submitDrawRunPayloadBytesCpuMaxNs{0};
  std::atomic<std::uint64_t> submitDrawRunSlotPrepareCpuNs{0};
  std::atomic<std::uint64_t> submitDrawRunSlotPrepareCpuMaxNs{0};
  std::atomic<std::uint64_t> submitDrawRunResourceMarkCpuNs{0};
  std::atomic<std::uint64_t> submitDrawRunResourceMarkCpuMaxNs{0};
  std::atomic<std::uint64_t> submitDrawRunAppendCpuNs{0};
  std::atomic<std::uint64_t> submitDrawRunAppendCpuMaxNs{0};
  std::atomic<std::uint64_t> submitDrawRunChunkCommitCpuNs{0};
  std::atomic<std::uint64_t> submitDrawRunChunkCommitCpuMaxNs{0};
  std::atomic<std::uint64_t> submitDrawRunBatchQueueLockCpuNs{0};
  std::atomic<std::uint64_t> submitDrawRunBatchQueueLockCpuMaxNs{0};
  std::atomic<std::uint64_t> submitDrawRunBatchCompatScanCpuNs{0};
  std::atomic<std::uint64_t> submitDrawRunBatchCompatScanCpuMaxNs{0};
  std::atomic<std::uint64_t> submitDrawRunBatchSubmissionAdjacentPairs{0};
  std::atomic<std::uint64_t> submitDrawRunBatchSubmissionAdjacentSameGenerationLane{0};
  std::atomic<std::uint64_t> submitDrawRunBatchCompatPairs{0};
  std::atomic<std::uint64_t> submitDrawRunBatchCompatCompatible{0};
  std::atomic<std::uint64_t> submitDrawRunBatchCompatIncompatible{0};
  std::atomic<std::uint64_t> submitDrawRunBatchCompatSameGenerationLane{0};
  std::atomic<std::uint64_t> submitDrawRunBatchCompatSameGenerationLaneCompatible{0};
  std::atomic<std::uint64_t> submitDrawRunBatchCompatSameGenerationLaneIncompatible{0};
  std::atomic<std::uint64_t> submitDrawRunBatchIncompatTexture{0};
  std::atomic<std::uint64_t> submitDrawRunBatchIncompatSampler{0};
  std::atomic<std::uint64_t> submitDrawRunBatchIncompatTextureStageState{0};
  std::atomic<std::uint64_t> submitDrawRunBatchIncompatRenderState{0};
  std::atomic<std::uint64_t> submitDrawRunBatchIncompatShader{0};
  std::atomic<std::uint64_t> submitDrawRunBatchIncompatVertexDecl{0};
  std::atomic<std::uint64_t> submitDrawRunBatchIncompatAttachment{0};
  std::atomic<std::uint64_t> submitDrawRunBatchIncompatViewport{0};
  std::atomic<std::uint64_t> submitDrawRunBatchIncompatClipPlane{0};
  std::atomic<std::uint64_t> submitDrawRunBatchIncompatLayoutUsage{0};
  std::atomic<std::uint64_t> submitDrawRunBatchIncompatUnknown{0};
  std::atomic<std::uint64_t> submitDrawRunBatchIncompatTextureOnly{0};
  std::atomic<std::uint64_t> submitDrawRunBatchIncompatRsAlphaTestOnly{0};
  std::atomic<std::uint64_t> submitDrawRunBatchIncompatRsBlendOnly{0};
  std::atomic<std::uint64_t> submitDrawRunBatchIncompatRsCullOnly{0};
  std::atomic<std::uint64_t> submitDrawRunBatchIncompatRsDepthOnly{0};
  std::atomic<std::uint64_t> submitDrawRunBatchIncompatRsFogOnly{0};
  std::atomic<std::uint64_t> submitDrawRunBatchIncompatRsTextureFactorOnly{0};
  std::atomic<std::uint64_t> submitDrawRunBatchIncompatRsSingleOther{0};
  std::atomic<std::uint64_t> submitDrawRunBatchIncompatRsMixed{0};
  std::atomic<std::uint64_t> submitDrawRunBatchBindingOverrideCpuNs{0};
  std::atomic<std::uint64_t> submitDrawRunBatchBindingOverrideCpuMaxNs{0};
  std::atomic<std::uint64_t> submitDrawRunBatchBindingSnapshotCpuNs{0};
  std::atomic<std::uint64_t> submitDrawRunBatchBindingSnapshotCpuMaxNs{0};
  std::atomic<std::uint64_t> submitDrawRunBatchPayloadBytesCpuNs{0};
  std::atomic<std::uint64_t> submitDrawRunBatchPayloadBytesCpuMaxNs{0};
  std::atomic<std::uint64_t> submitDrawRunBatchSlotPrepareCpuNs{0};
  std::atomic<std::uint64_t> submitDrawRunBatchSlotPrepareCpuMaxNs{0};
  std::atomic<std::uint64_t> submitDrawRunBatchResourceMarkCpuNs{0};
  std::atomic<std::uint64_t> submitDrawRunBatchResourceMarkCpuMaxNs{0};
  std::atomic<std::uint64_t> submitDrawRunBatchAppendCpuNs{0};
  std::atomic<std::uint64_t> submitDrawRunBatchAppendCpuMaxNs{0};
  std::atomic<std::uint64_t> submitDrawRunBatchAppendReserveCpuNs{0};
  std::atomic<std::uint64_t> submitDrawRunBatchAppendReserveCpuMaxNs{0};
  std::atomic<std::uint64_t> submitDrawRunBatchAppendStateCpuNs{0};
  std::atomic<std::uint64_t> submitDrawRunBatchAppendStateCpuMaxNs{0};
  std::atomic<std::uint64_t> submitDrawRunBatchAppendStatePsoCpuNs{0};
  std::atomic<std::uint64_t> submitDrawRunBatchAppendStatePsoCpuMaxNs{0};
  std::atomic<std::uint64_t> submitDrawRunBatchAppendStateInvariantCpuNs{0};
  std::atomic<std::uint64_t> submitDrawRunBatchAppendStateInvariantCpuMaxNs{0};
  std::atomic<std::uint64_t> submitDrawRunBatchAppendStateSoaCpuNs{0};
  std::atomic<std::uint64_t> submitDrawRunBatchAppendStateSoaCpuMaxNs{0};
  std::atomic<std::uint64_t> submitDrawRunBatchAppendUniformCpuNs{0};
  std::atomic<std::uint64_t> submitDrawRunBatchAppendUniformCpuMaxNs{0};
  std::atomic<std::uint64_t> submitDrawRunBatchAppendPayloadCpuNs{0};
  std::atomic<std::uint64_t> submitDrawRunBatchAppendPayloadCpuMaxNs{0};
  std::atomic<std::uint64_t> submitDrawRunBatchAppendParamCpuNs{0};
  std::atomic<std::uint64_t> submitDrawRunBatchAppendParamCpuMaxNs{0};
  std::atomic<std::uint64_t> submitDrawRunBatchAppendRecordCpuNs{0};
  std::atomic<std::uint64_t> submitDrawRunBatchAppendRecordCpuMaxNs{0};
  std::atomic<std::uint64_t> submitDrawRunBatchAppendPayloadBytes{0};
  std::atomic<std::uint64_t> submitDrawRunBatchAppendParams{0};
  std::atomic<std::uint64_t> submitDrawRunBatchChunkCommitCpuNs{0};
  std::atomic<std::uint64_t> submitDrawRunBatchChunkCommitCpuMaxNs{0};
  std::atomic<std::uint64_t> commandBuffers{0};
  // R-BACK-2.29..2.32 — sub-command-buffer chain instrumentation.
  // subCommandBufferCommits aggregates every mid-chunk commit that a
  // chunk performs (one per split point that closes a non-final
  // MTLCommandBuffer in the chain). chunkSubCBCountMax is the
  // worst-case per-chunk chain length observed (mid + final commits
  // combined), folded in at encodeChunk exit via updateMax.
  std::atomic<std::uint64_t> subCommandBufferCommits{0};
  std::atomic<std::uint64_t> chunkSubCBCountMax{0};
  std::atomic<std::uint64_t> subCommandBufferSplitSuppressedByCap{0};
  // M5 — gpu_command_buffer_errors. Incremented once per Metal
  // CommandBuffer that surfaces WMTCommandBufferStatusError; intended as
  // a regression sentinel (R-BACK GPU faults) — paired with an
  // expected_counters entry on perf probes so a fault trips L3.
  std::atomic<std::uint64_t> gpuCommandBufferErrors{0};
  std::atomic<std::uint64_t> metalBuffers{0};
  std::atomic<std::uint64_t> metalBufferBytes{0};
  std::atomic<std::uint64_t> pipelineBuilds{0};
  std::atomic<std::uint64_t> pipelineHitDraw{0};
  std::atomic<std::uint64_t> pipelineHitFill{0};
  std::atomic<std::uint64_t> pipelineHitStretch{0};
  std::atomic<std::uint64_t> pipelineMissDraw{0};
  std::atomic<std::uint64_t> pipelineMissFill{0};
  std::atomic<std::uint64_t> pipelineMissStretch{0};
  std::atomic<std::uint64_t> pipelineBuildDraw{0};
  std::atomic<std::uint64_t> pipelineBuildFill{0};
  std::atomic<std::uint64_t> pipelineBuildStretch{0};
  std::atomic<std::uint64_t> pipelineBuildPresent{0};
  std::atomic<std::uint64_t> psoSlotsDraw{0};
  std::atomic<std::uint64_t> psoSlotsDrawMax{0};
  std::atomic<std::uint64_t> psoSlotExhausted{0};
  std::atomic<std::uint64_t> psoVariantArgbufStage2{0};
  std::atomic<std::uint64_t> psoVariantTileFfp{0};
  std::atomic<std::uint64_t> sourceLibraryEntries{0};
  std::atomic<std::uint64_t> sourceLibraryEntriesMax{0};
  std::atomic<std::uint64_t> pipelineBuildFailDraw{0};
  std::atomic<std::uint64_t> pipelineBuildFailLibrary{0};
  std::atomic<std::uint64_t> pipelineBuildFailFunction{0};
  std::atomic<std::uint64_t> pipelineBuildFailPso{0};
  std::atomic<std::uint64_t> drawSkippedNoPipeline{0};
  std::atomic<std::uint64_t> shaderVariantKeyHashCpuNs{0};
  std::atomic<std::uint64_t> shaderVariantKeyHashCpuMaxNs{0};
  std::atomic<std::uint64_t> renderPassBegin{0};
  std::atomic<std::uint64_t> renderPassEnd{0};
  std::atomic<std::uint64_t> renderSplitFinal{0};
  std::atomic<std::uint64_t> renderSplitRenderTargetChange{0};
  std::atomic<std::uint64_t> renderSplitHazard{0};
  std::atomic<std::uint64_t> renderSplitClearBarrier{0};
  std::atomic<std::uint64_t> renderSplitSurfaceCopy{0};
  std::atomic<std::uint64_t> renderSplitStretchRect{0};
  std::atomic<std::uint64_t> renderSplitReadback{0};
  std::atomic<std::uint64_t> renderSplitColorFill{0};
  std::atomic<std::uint64_t> renderSplitPresent{0};
  std::atomic<std::uint64_t> renderSplitPresentAcquire{0};
  std::atomic<std::uint64_t> renderSplitTileMidPassIneligible{0};
  std::atomic<std::uint64_t> renderSplitOrderedControl{0};
  std::atomic<std::uint64_t> hazardProbeComparisons{0};
  std::atomic<std::uint64_t> hazardBloomOverlaps{0};
  std::atomic<std::uint64_t> hazardExactOverlaps{0};
  std::atomic<std::uint64_t> hazardBloomFalsePositive{0};
  // Historical V1 importer/replay detail columns remain in the JSON schema
  // at zero for result-parser compatibility. Fields in this block that still
  // have a count* entry are shared by the V2 replay path.
  std::atomic<std::uint64_t> commitChunkDrawSubmissionBatchSubmits{0};
  std::atomic<std::uint64_t> commitChunkDrawSubmissionBatchRecords{0};
  std::atomic<std::uint64_t> commitChunkDrawSubmissionBatchMaxRecords{0};
  std::atomic<std::uint64_t> commitChunkDrawSubmissionBatchSize1{0};
  std::atomic<std::uint64_t> commitChunkDrawSubmissionBatchSize2{0};
  std::atomic<std::uint64_t> commitChunkDrawSubmissionBatchSize3To4{0};
  std::atomic<std::uint64_t> commitChunkDrawSubmissionBatchSize5To8{0};
  std::atomic<std::uint64_t> commitChunkDrawSubmissionBatchSize9To16{0};
  std::atomic<std::uint64_t> commitChunkDrawSubmissionBatchSize17To32{0};
  std::atomic<std::uint64_t> commitChunkDrawSubmissionBatchSize33Plus{0};
  std::atomic<std::uint64_t> d3d9DrawStateCacheHits{0};
  std::atomic<std::uint64_t> d3d9DrawStateCacheMisses{0};
  std::atomic<std::uint64_t> d3d9DrawStateCacheHitWithIndex{0};
  std::atomic<std::uint64_t> d3d9DrawStateCacheMissWithIndex{0};
  std::atomic<std::uint64_t> d3d9DrawStateCacheHitNoIndex{0};
  std::atomic<std::uint64_t> d3d9DrawStateCacheMissNoIndex{0};
  std::atomic<std::uint64_t> d3d9DrawStateCacheDirectHits{0};
  std::atomic<std::uint64_t> d3d9DrawStateCacheDirectMisses{0};
  std::atomic<std::uint64_t> d3d9DrawStateCacheDirectHitWithIndex{0};
  std::atomic<std::uint64_t> d3d9DrawStateCacheDirectMissWithIndex{0};
  std::atomic<std::uint64_t> d3d9DrawStateCacheDirectHitNoIndex{0};
  std::atomic<std::uint64_t> d3d9DrawStateCacheDirectMissNoIndex{0};
  std::atomic<std::uint64_t> d3d9DrawStateCacheBatchHits{0};
  std::atomic<std::uint64_t> d3d9DrawStateCacheBatchMisses{0};
  std::atomic<std::uint64_t> d3d9DrawStateCacheUniformRefreshes{0};
  std::atomic<std::uint64_t> d3d9DrawStateCacheBatchMissReasonUnknown{0};
  std::atomic<std::uint64_t> d3d9DrawStateCacheBatchMissReasonBindingOnly{0};
  std::atomic<std::uint64_t> d3d9DrawStateCacheBatchMissReasonSingleRenderState{0};
  std::atomic<std::uint64_t> d3d9DrawStateCacheBatchMissReasonSingleTexture{0};
  std::atomic<std::uint64_t> d3d9DrawStateCacheBatchMissReasonSingleFvfVdecl{0};
  std::atomic<std::uint64_t> d3d9DrawStateCacheBatchMissReasonSingleShader{0};
  std::atomic<std::uint64_t> d3d9DrawStateCacheBatchMissReasonSingleRtDepth{0};
  std::atomic<std::uint64_t> d3d9DrawStateCacheBatchMissReasonSingleViewportScissor{0};
  std::atomic<std::uint64_t> d3d9DrawStateCacheBatchMissReasonSingleTssSampler{0};
  std::atomic<std::uint64_t> d3d9DrawStateCacheBatchMissReasonSingleFfpClip{0};
  std::atomic<std::uint64_t> d3d9DrawStateCacheBatchMissReasonSingleBroad{0};
  std::atomic<std::uint64_t> d3d9DrawStateCacheBatchMissReasonMixed2{0};
  std::atomic<std::uint64_t> d3d9DrawStateCacheBatchMissReasonMixed3{0};
  std::atomic<std::uint64_t> d3d9DrawStateCacheBatchMissReasonMixed4Plus{0};
  std::atomic<std::uint64_t> d3d9DrawStateCacheBatchMissReasonHasRenderState{0};
  std::atomic<std::uint64_t> d3d9DrawStateCacheBatchMissReasonHasTexture{0};
  std::atomic<std::uint64_t> d3d9DrawStateCacheBatchMissReasonHasFvfVdecl{0};
  std::atomic<std::uint64_t> d3d9DrawStateCacheBatchMissReasonHasShader{0};
  std::atomic<std::uint64_t> d3d9DrawStateCacheBatchMissReasonHasRtDepth{0};
  std::atomic<std::uint64_t> d3d9DrawStateCacheBatchMissReasonHasViewportScissor{0};
  std::atomic<std::uint64_t> d3d9DrawStateCacheBatchMissReasonHasTssSampler{0};
  std::atomic<std::uint64_t> d3d9DrawStateCacheBatchMissReasonHasFfpClip{0};
  std::atomic<std::uint64_t> d3d9DrawStateCacheBatchMissReasonHasBroad{0};
  std::atomic<std::uint64_t> d3d9DrawStateCacheBatchMissReasonHasTextureShader{0};
  std::atomic<std::uint64_t> d3d9DrawStateCacheBatchMissReasonHasTextureFvfVdecl{0};
  std::atomic<std::uint64_t> d3d9DrawStateCacheBatchMissReasonHasShaderFvfVdecl{0};
  std::atomic<std::uint64_t> d3d9DrawStateCacheBatchMissReasonHasTextureTssSampler{0};
  std::atomic<std::uint64_t> d3d9DrawStateCacheBatchMissReasonHasTextureShaderFvfVdecl{0};
  std::atomic<std::uint64_t> d3d9DrawStateCacheBatchMissReasonHasTextureShaderTssSampler{0};
  std::atomic<std::uint64_t> d3d9DrawStateCacheBatchMissReasonHasTextureFvfVdeclTssSampler{0};
  std::atomic<std::uint64_t> d3d9DrawStateCacheBatchMissReasonHasShaderFvfVdeclTssSampler{0};
  std::atomic<std::uint64_t> d3d9DrawStateCacheBatchMissReasonHasTextureShaderFvfVdeclTssSampler{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissSemanticReuseProbeSamples{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissSemanticReuseProbeHits{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissSemanticReuseProbeMisses{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissSemanticReuseProbeHitDistance1{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissSemanticReuseProbeHitDistance2{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissSemanticReuseProbeHitDistance3To4{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissSemanticReuseProbeHitDistance5To8{0};
  std::atomic<std::uint64_t> d3d9DrawStateCacheMissAfterUnknown{0};
  std::atomic<std::uint64_t> d3d9DrawStateCacheMissAfterMutableState{0};
  std::atomic<std::uint64_t> d3d9DrawStateCacheMissAfterDrawPacket{0};
  std::atomic<std::uint64_t> d3d9DrawStateCacheMissAfterRenderState{0};
  std::atomic<std::uint64_t> d3d9DrawStateCacheMissAfterTexture{0};
  std::atomic<std::uint64_t> d3d9DrawStateCacheMissAfterStream{0};
  std::atomic<std::uint64_t> d3d9DrawStateCacheMissAfterIndexBuffer{0};
  std::atomic<std::uint64_t> d3d9DrawStateCacheMissAfterFvfVdecl{0};
  std::atomic<std::uint64_t> d3d9DrawStateCacheMissAfterShader{0};
  std::atomic<std::uint64_t> d3d9DrawStateCacheMissAfterRenderTargetDepth{0};
  std::atomic<std::uint64_t> d3d9DrawStateCacheMissAfterViewportScissor{0};
  std::atomic<std::uint64_t> d3d9DrawStateCacheMissAfterTextureStageSampler{0};
  std::atomic<std::uint64_t> d3d9DrawStateCacheMissAfterFfpState{0};
  std::atomic<std::uint64_t> d3d9DrawStateCacheMissAfterClipPlane{0};
  std::atomic<std::uint64_t> d3d9DrawStateCacheMissAfterStateBlock{0};
  std::atomic<std::uint64_t> d3d9DrawStateCacheMissAfterReset{0};
  std::atomic<std::uint64_t> d3d9DrawStateCacheMissAfterSwapChain{0};
  std::atomic<std::uint64_t> d3d9DrawStateCacheMissAfterTextureLod{0};
  std::atomic<std::uint64_t> drawCalls{0};
  std::atomic<std::uint64_t> drawIndexedCalls{0};
  std::atomic<std::uint64_t> drawExpandedIndexedCalls{0};
  std::atomic<std::uint64_t> drawPrimitiveCount{0};
  std::atomic<std::uint64_t> drawTriangleEstimate{0};
  std::atomic<std::uint64_t> drawVertexCount{0};
  std::atomic<std::uint64_t> drawUpVertexBytes{0};
  std::atomic<std::uint64_t> drawUpIndexBytes{0};
  std::atomic<std::uint64_t> bindTexture{0};
  std::atomic<std::uint64_t> bindSampler{0};
  std::atomic<std::uint64_t> bindTextureSkipped{0};
  std::atomic<std::uint64_t> bindSamplerSkipped{0};
  std::atomic<std::uint64_t> bindVertexBuffer{0};
  std::atomic<std::uint64_t> bindVertexBufferSkipped{0};
  std::atomic<std::uint64_t> bindIndexBuffer{0};
  std::atomic<std::uint64_t> bindIndexBufferSkipped{0};
  std::atomic<std::uint64_t> bindUniformBuffer{0};
  std::atomic<std::uint64_t> bindPipeline{0};
  std::atomic<std::uint64_t> bindPipelineSkipped{0};
  std::atomic<std::uint64_t> bindDepthState{0};
  std::atomic<std::uint64_t> bindDepthStateSkipped{0};
  std::atomic<std::uint64_t> bindViewport{0};
  std::atomic<std::uint64_t> bindViewportSkipped{0};
  std::atomic<std::uint64_t> bindScissor{0};
  std::atomic<std::uint64_t> bindScissorSkipped{0};
  std::atomic<std::uint64_t> bindRasterizer{0};
  std::atomic<std::uint64_t> bindRasterizerSkipped{0};
  std::atomic<std::uint64_t> drawShaderBucketSamples{0};
  std::atomic<std::uint64_t> drawShaderBucketChanges{0};
  std::atomic<std::uint64_t> lastVertexShaderHash{0};
  std::atomic<std::uint64_t> lastPixelShaderHash{0};
  std::atomic<std::uint64_t> lastShaderVariantHash{0};
  std::atomic<std::uint64_t> drawGeometrySamples{0};
  std::atomic<std::uint64_t> drawGeometryFfp{0};
  std::atomic<std::uint64_t> drawGeometryVs{0};
  std::atomic<std::uint64_t> drawGeometryIndexed{0};
  std::atomic<std::uint64_t> drawGeometryIndex16{0};
  std::atomic<std::uint64_t> drawGeometryIndex32{0};
  std::atomic<std::uint64_t> drawGeometryDirect{0};
  std::atomic<std::uint64_t> drawGeometryUp{0};
  std::atomic<std::uint64_t> drawGeometryExpanded{0};
  std::atomic<std::uint64_t> drawGeometryNonZeroBaseVertex{0};
  std::atomic<std::uint64_t> drawGeometryNonZeroStartIndex{0};
  std::atomic<std::uint64_t> drawGeometryNonZeroStream0Offset{0};
  std::atomic<std::uint64_t> drawGeometryLastStream0Stride{0};
  std::atomic<std::uint64_t> drawGeometryLastVertexDeclHash{0};
  std::atomic<std::uint64_t> completionCompatFp16WaitNs{0};
  std::atomic<std::uint64_t> completionCompatMrtWaitNs{0};
  std::atomic<std::uint64_t> completionCompatSrgbWaitNs{0};
  std::atomic<std::uint64_t> completionCompatProjectedWaitNs{0};
  std::atomic<std::uint64_t> completionCompatMsaaWaitNs{0};
  std::atomic<std::uint64_t> completionCompatQueryWaitNs{0};
  std::atomic<std::uint64_t> completionShaderBucketSamples{0};
  std::atomic<std::uint64_t> completionShaderBucketChanges{0};
  std::atomic<std::uint64_t> completionLastVertexShaderHash{0};
  std::atomic<std::uint64_t> completionLastPixelShaderHash{0};
  std::atomic<std::uint64_t> completionLastShaderVariantHash{0};
  std::atomic<std::uint64_t> submitDrawCpuNs{0};
  std::atomic<std::uint64_t> submitDrawCpuMaxNs{0};
  // M4 — per-command-buffer GPU time, sampled via MTLCommandBuffer.GPUStartTime
  // / GPUEndTime when the buffer reaches Completed. Skipped on samples where
  // the driver returns 0 / non-monotonic values.
  std::atomic<std::uint64_t> gpuCommandBufferTimeNs{0};
  std::atomic<std::uint64_t> gpuCommandBufferTimeMaxNs{0};
  std::atomic<std::uint64_t> gpuCommandBufferTimeSamples{0};
  // Stage-boundary MTLCounterSampleBuffer path. These counters close the
  // in-process feedback loop that previously required xctrace's
  // metal-gpu-intervals table for per-render-encoder timings.
  std::atomic<std::uint64_t> renderEncoderGpuTimeNs{0};
  std::atomic<std::uint64_t> renderEncoderGpuTimeMaxNs{0};
  std::atomic<std::uint64_t> renderEncoderGpuTimeSamples{0};
  std::atomic<std::uint64_t> renderEncoderGpuDrawTimeNs{0};
  std::atomic<std::uint64_t> renderEncoderGpuDrawSamples{0};
  std::atomic<std::uint64_t> renderEncoderGpuClearTimeNs{0};
  std::atomic<std::uint64_t> renderEncoderGpuClearSamples{0};
  std::atomic<std::uint64_t> renderEncoderGpuSurfaceCopyTimeNs{0};
  std::atomic<std::uint64_t> renderEncoderGpuSurfaceCopySamples{0};
  std::atomic<std::uint64_t> renderEncoderGpuStretchTimeNs{0};
  std::atomic<std::uint64_t> renderEncoderGpuStretchSamples{0};
  std::atomic<std::uint64_t> renderEncoderGpuColorFillTimeNs{0};
  std::atomic<std::uint64_t> renderEncoderGpuColorFillSamples{0};
  std::atomic<std::uint64_t> renderEncoderGpuDepthResolveTimeNs{0};
  std::atomic<std::uint64_t> renderEncoderGpuDepthResolveSamples{0};
  std::atomic<std::uint64_t> renderEncoderGpuPresentTimeNs{0};
  std::atomic<std::uint64_t> renderEncoderGpuPresentSamples{0};
  std::atomic<std::uint64_t> renderEncoderGpuLastPassType{0};
  std::atomic<std::uint64_t> renderEncoderGpuLastRt{0};
  std::atomic<std::uint64_t> renderEncoderGpuLastDepth{0};
  std::atomic<std::uint64_t> renderEncoderGpuLastPso{0};
  std::atomic<std::uint64_t> encodeChunkCalls{0};
  std::atomic<std::uint64_t> encodeChunkCpuNs{0};
  std::atomic<std::uint64_t> encodeChunkCpuMaxNs{0};
  std::atomic<std::uint64_t> encodeDrawCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawCpuMaxNs{0};
  std::atomic<std::uint64_t> encodeDrawPipelineLookupCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawPipelineLookupCpuMaxNs{0};
  std::atomic<std::uint64_t> encodeDrawUniformBuildCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawUniformBuildCpuMaxNs{0};
  std::atomic<std::uint64_t> encodeDrawFvfDecodeCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawFvfDecodeCpuMaxNs{0};
  std::atomic<std::uint64_t> encodeDrawBindingPacketCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawBindingPacketPlanCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawBindingPacketPlanFragmentCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawBindingPacketPlanVertexCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawBindingPacketPlanExtraStreamCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawBindingPacketPlanRasterCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawBindingPacketCacheCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawBindingPacketCacheKeyCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawBindingPacketCacheHashCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawBindingPacketCacheProbeCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawBindingPacketCacheStoreCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawBindingPacketCacheHits{0};
  std::atomic<std::uint64_t> encodeDrawBindingPacketCacheMisses{0};
  std::atomic<std::uint64_t> encodeDrawBindingPacketCacheCollisions{0};
  std::atomic<std::uint64_t> encodeDrawBindingPacketTextureRecordCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufSetupCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufOpenCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufOpenCallCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufReopenPostCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufReopenTableProbeCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufReopenTableShadowStoreCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufReopenByteAccountCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufReopenCbufCacheProbeCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufReopenCbufDirtyScanCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufReopenCbufForceDirtyCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufOpenReserveCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufOpenSetArgumentBufferCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufTableBindCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufTableBindCalls{0};
  std::atomic<std::uint64_t> encodeDrawArgbufTableBindSkipped{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufUpdateCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufUpdateCalls{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufUpdateDirtyCalls{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufUpdateSkippedClean{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufUpdateWriteCalls{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufBuildCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufUploadCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufSetBufferCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufBuildVsCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufBuildPsCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufBuildFfpVsCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufBuildFfpPsCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufUploadVsCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufUploadPsCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufUploadFfpVsCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufUploadFfpPsCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufSetBufferVsCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufSetBufferPsCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufSetBufferFfpVsCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufSetBufferFfpPsCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufUploadPlanCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufUploadPlanVsCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufUploadPlanPsCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufBindingHashCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufBindingHashVsCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufBindingHashPsCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufBindingHashFfpVsCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufBindingHashFfpPsCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufBindingWriteCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufBindingWriteVsCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufBindingWritePsCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufBindingWriteFfpVsCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufBindingWriteFfpPsCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufObserverCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufObserverVsCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufObserverPsCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufObserverFfpVsCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufObserverFfpPsCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufCacheMergeCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufCachedRepointCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufFullRepointCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufCachedRepointVsCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufCachedRepointPsCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufCachedRepointFfpVsCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufCachedRepointFfpPsCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufCachedRepointCalls{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufCachedRepointBytes{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufCachedRepointVsCalls{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufCachedRepointPsCalls{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufCachedRepointFfpVsCalls{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufCachedRepointFfpPsCalls{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufCachedRepointVsBytes{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufCachedRepointPsBytes{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufCachedRepointFfpVsBytes{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufCachedRepointFfpPsBytes{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufContentProbeCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufContentProbeVsCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufContentProbePsCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufContentProbeFfpPsCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufContentProbeCalls{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufContentProbeVsHits{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufContentProbeVsMisses{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufContentProbePsHits{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufContentProbePsMisses{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufContentProbeFfpPsHits{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufContentProbeFfpPsMisses{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufReopenFullRepointCalls{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufReopenNoDirtyHashMismatch{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufReopenPartialCandidates{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufReopenDirtyVs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufReopenDirtyPs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufReopenDirtyFfpVs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufReopenDirtyFfpPs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufUpdateVsCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufUpdatePsCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufUpdateFfpVsCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufUpdateFfpPsCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufUpdateVsCalls{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufUpdatePsCalls{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufUpdateFfpVsCalls{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufUpdateFfpPsCalls{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufUpdateVsBytes{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufUpdatePsBytes{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufUpdateFfpVsBytes{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufUpdateFfpPsBytes{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufDirtyVsIdentityProbeCalls{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufDirtyVsIdentityHits{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufDirtyVsIdentityMisses{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufDirtyVsIdentityNoCache{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufDirtyVsIdentityHitBytes{0};
  std::atomic<std::uint64_t> encodeDrawArgbufCbufDirtyVsIdentityMissBytes{0};
  std::atomic<std::uint64_t> encodeDrawArgbufPayloadDeltaProbeCalls{0};
  std::atomic<std::uint64_t> encodeDrawArgbufPayloadDeltaFirst{0};
  std::atomic<std::uint64_t> encodeDrawArgbufPayloadDeltaSame{0};
  std::atomic<std::uint64_t> encodeDrawArgbufPayloadDeltaChanged{0};
  std::atomic<std::uint64_t> encodeDrawArgbufPayloadDeltaChangedVs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufPayloadDeltaChangedPs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufPayloadDeltaChangedVsPs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufPayloadDeltaChangedNonConstOnly{0};
  std::atomic<std::uint64_t> encodeDrawArgbufPayloadDeltaChangedVsFloat{0};
  std::atomic<std::uint64_t> encodeDrawArgbufPayloadDeltaChangedVsInt{0};
  std::atomic<std::uint64_t> encodeDrawArgbufPayloadDeltaChangedVsBool{0};
  std::atomic<std::uint64_t> encodeDrawArgbufPayloadDeltaChangedPsFloat{0};
  std::atomic<std::uint64_t> encodeDrawArgbufPayloadDeltaChangedPsInt{0};
  std::atomic<std::uint64_t> encodeDrawArgbufPayloadDeltaChangedPsBool{0};
  std::atomic<std::uint64_t> encodeDrawArgbufPayloadDeltaChangedVsFloatRegs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufPayloadDeltaChangedVsFloatRegsMax{0};
  std::atomic<std::uint64_t> encodeDrawArgbufPayloadDeltaChangedVsFloatRegsLe1{0};
  std::atomic<std::uint64_t> encodeDrawArgbufPayloadDeltaChangedVsFloatRegsLe4{0};
  std::atomic<std::uint64_t> encodeDrawArgbufPayloadDeltaChangedVsFloatRegsLe16{0};
  std::atomic<std::uint64_t> encodeDrawArgbufPayloadDeltaChangedVsFloatRegsLe64{0};
  std::atomic<std::uint64_t> encodeDrawArgbufPayloadDeltaChangedVsFloatRegsGt64{0};
  std::atomic<std::uint64_t> encodeDrawArgbufPayloadDeltaChangedVsFloatRegsLe1Sum{0};
  std::atomic<std::uint64_t> encodeDrawArgbufPayloadDeltaChangedVsFloatRegsLe4Sum{0};
  std::atomic<std::uint64_t> encodeDrawArgbufPayloadDeltaChangedVsFloatRegsLe16Sum{0};
  std::atomic<std::uint64_t> encodeDrawArgbufPayloadDeltaChangedVsFloatRegsLe64Sum{0};
  std::atomic<std::uint64_t> encodeDrawArgbufPayloadDeltaChangedVsFloatRegsGt64Sum{0};
  std::atomic<std::uint64_t> encodeDrawArgbufPayloadDeltaChangedVsFloatPrefixRegs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufPayloadDeltaChangedVsFloatPrefixRegsMax{0};
  std::atomic<std::uint64_t> encodeDrawArgbufPayloadDeltaChangedVsFloatSpanRegs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufPayloadDeltaChangedVsFloatSpanRegsMax{0};
  std::atomic<std::uint64_t> encodeDrawArgbufPayloadDeltaChangedVsFloatFullPrefix{0};
  std::atomic<std::uint64_t> encodeDrawArgbufPayloadDeltaChangedVsFloatFullPrefixRegs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufPayloadDeltaChangedPsFloatRegs{0};
  std::atomic<std::uint64_t> encodeDrawArgbufPayloadDeltaChangedPsFloatRegsMax{0};
  std::atomic<std::uint64_t> encodeDrawArgbufPayloadDeltaChangedPsFloatRegsLe1{0};
  std::atomic<std::uint64_t> encodeDrawArgbufPayloadDeltaChangedPsFloatRegsLe4{0};
  std::atomic<std::uint64_t> encodeDrawArgbufPayloadDeltaChangedPsFloatRegsLe16{0};
  std::atomic<std::uint64_t> encodeDrawArgbufPayloadDeltaChangedPsFloatRegsLe64{0};
  std::atomic<std::uint64_t> encodeDrawArgbufPayloadDeltaChangedPsFloatRegsGt64{0};
  std::atomic<std::uint64_t> encodeDrawArgbufPayloadDeltaChangedPsFloatRegsLe1Sum{0};
  std::atomic<std::uint64_t> encodeDrawArgbufPayloadDeltaChangedPsFloatRegsLe4Sum{0};
  std::atomic<std::uint64_t> encodeDrawArgbufPayloadDeltaChangedPsFloatRegsLe16Sum{0};
  std::atomic<std::uint64_t> encodeDrawArgbufPayloadDeltaChangedPsFloatRegsLe64Sum{0};
  std::atomic<std::uint64_t> encodeDrawArgbufPayloadDeltaChangedPsFloatRegsGt64Sum{0};
  std::atomic<std::uint64_t> encodeDrawArgbufPayloadDeltaReopenFirst{0};
  std::atomic<std::uint64_t> encodeDrawArgbufPayloadDeltaReopenPayloadChanged{0};
  std::atomic<std::uint64_t> encodeDrawArgbufPayloadDeltaReopenPayloadSame{0};
  std::atomic<std::uint64_t> encodeDrawArgbufPayloadDeltaReopenResourceArray{0};
  std::atomic<std::uint64_t> encodeDrawArgbufPayloadDeltaReopenCbufOnly{0};
  std::atomic<std::uint64_t> encodeDrawArgbufPayloadDeltaReopenCbufOnlyFirst{0};
  std::atomic<std::uint64_t>
      encodeDrawArgbufPayloadDeltaReopenCbufOnlyPayloadChanged{0};
  std::atomic<std::uint64_t> encodeDrawStreamBindCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawStreamBindCpuMaxNs{0};
  std::atomic<std::uint64_t> encodeDrawStreamBindRasterPhaseCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawStreamBindRasterPhaseCalls{0};
  std::atomic<std::uint64_t> encodeDrawStreamBindFfpStreamCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawStreamBindFfpStreamCalls{0};
  std::atomic<std::uint64_t> encodeDrawStreamBindShaderStreamCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawStreamBindShaderStreamCalls{0};
  std::atomic<std::uint64_t> encodeDrawStreamBindTexturePhaseCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawStreamBindTexturePhaseCalls{0};
  std::atomic<std::uint64_t> encodeDrawStreamBindIndexPhaseCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawStreamBindIndexPhaseCalls{0};
  std::atomic<std::uint64_t> encodeDrawTextureSamplerFragmentResolveCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawTextureSamplerFragmentResolveCalls{0};
  std::atomic<std::uint64_t> encodeDrawTextureSamplerFragmentResolveTextureCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawTextureSamplerFragmentResolveTextureCalls{0};
  std::atomic<std::uint64_t> encodeDrawTextureSamplerFragmentResourceArrayCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawTextureSamplerFragmentResourceArrayCalls{0};
  std::atomic<std::uint64_t> encodeDrawTextureSamplerFragmentDirectCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawTextureSamplerFragmentDirectCalls{0};
  std::atomic<std::uint64_t> encodeDrawTextureSamplerFragmentDirectTextureCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawTextureSamplerFragmentDirectTextureCalls{0};
  std::atomic<std::uint64_t> encodeDrawTextureSamplerFragmentDirectTextureSetCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawTextureSamplerFragmentDirectTextureSetCalls{0};
  std::atomic<std::uint64_t> encodeDrawTextureSamplerFragmentDirectSamplerCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawTextureSamplerFragmentDirectSamplerCalls{0};
  std::atomic<std::uint64_t> encodeDrawTextureSamplerFragmentDirectSamplerSetCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawTextureSamplerFragmentDirectSamplerSetCalls{0};
  std::atomic<std::uint64_t> encodeDrawTextureSamplerSamplerLookupCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawTextureSamplerSamplerLookupCalls{0};
  std::atomic<std::uint64_t> encodeDrawTextureSamplerSamplerLookupSkippedPrehandle{0};
  std::atomic<std::uint64_t> encodeDrawTextureSamplerLodBiasCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawTextureSamplerLodBiasCalls{0};
  std::atomic<std::uint64_t> encodeDrawTextureSamplerVertexResolveCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawTextureSamplerVertexResolveCalls{0};
  std::atomic<std::uint64_t> encodeDrawTextureSamplerVertexDirectCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawTextureSamplerVertexDirectCalls{0};
  std::atomic<std::uint64_t> encodeDrawRasterStateCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawVertexStreamBindCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawTextureSamplerBindCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawIndexSetupCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawIndexSourceResolveCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawIndexCacheLookupCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawIndexCacheCandidateCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawIndexCacheOriginalMeasureCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawIndexCacheCandidateBuildCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawIndexCacheCandidateReadCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawIndexCacheCandidateAdjacencyCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawIndexCacheCandidateSelectCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawIndexCacheCandidateWriteCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawIndexCacheCandidateSelectCalls{0};
  std::atomic<std::uint64_t> encodeDrawIndexCacheCandidateSelectSlots{0};
  std::atomic<std::uint64_t> encodeDrawIndexCacheCandidateSelectScored{0};
  std::atomic<std::uint64_t> encodeDrawIndexCacheCandidateSelectSkipped{0};
  std::atomic<std::uint64_t> encodeDrawIndexCacheCandidateSelectCandidatesMax{0};
  std::atomic<std::uint64_t> encodeDrawIndexCacheCandidateFrontierDropped{0};
  std::atomic<std::uint64_t> encodeDrawIndexCacheCandidateLazyHeapPops{0};
  std::atomic<std::uint64_t> encodeDrawIndexCacheCandidateLazyRefreshes{0};
  std::atomic<std::uint64_t> encodeDrawIndexCacheCandidateLazyStaleDrops{0};
  std::atomic<std::uint64_t> encodeDrawIndexCacheCandidateLazyAccepted{0};
  std::atomic<std::uint64_t> encodeDrawIndexCacheCandidateBucketVertexVisits{0};
  std::atomic<std::uint64_t> encodeDrawIndexCacheCandidateBucketMoves{0};
  std::atomic<std::uint64_t> encodeDrawIndexCacheCandidateBucketSelected{0};
  std::atomic<std::uint64_t> encodeDrawIndexCacheCandidateUpperBoundRejected{0};
  std::atomic<std::uint64_t> encodeDrawIndexCacheCandidateMeasureCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawIndexCacheGateCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawIndexCacheApplyCpuNs{0};
  std::atomic<std::uint64_t> compatibleIndexedDrawMergePairAttempts{0};
  std::atomic<std::uint64_t> compatibleIndexedDrawMergeCompatiblePairs{0};
  std::atomic<std::uint64_t> compatibleIndexedDrawMergeMultipleRejectPairs{0};
  std::atomic<std::uint64_t> compatibleIndexedDrawMergeSelectedPairs{0};
  std::atomic<std::uint64_t> compatibleIndexedDrawMergeRejectSourceShape{0};
  std::atomic<std::uint64_t> compatibleIndexedDrawMergeRejectNextShape{0};
  std::atomic<std::uint64_t> compatibleIndexedDrawMergeRejectIndexType{0};
  std::atomic<std::uint64_t> compatibleIndexedDrawMergeRejectBaseVertex{0};
  std::atomic<std::uint64_t> compatibleIndexedDrawMergeRejectStartVertex{0};
  std::atomic<std::uint64_t> compatibleIndexedDrawMergeRejectUniform{0};
  std::atomic<std::uint64_t> compatibleIndexedDrawMergeRejectBindingOverride{0};
  std::atomic<std::uint64_t> compatibleIndexedDrawMergeRejectBindingSnapshot{0};
  std::atomic<std::uint64_t> compatibleIndexedDrawMergeRejectNonContiguous{0};
  std::atomic<std::uint64_t> compatibleIndexedDrawMergeRejectOverflow{0};
  std::atomic<std::uint64_t> compatibleIndexedDrawMergeOnlySourceShape{0};
  std::atomic<std::uint64_t> compatibleIndexedDrawMergeOnlyNextShape{0};
  std::atomic<std::uint64_t> compatibleIndexedDrawMergeOnlyIndexType{0};
  std::atomic<std::uint64_t> compatibleIndexedDrawMergeOnlyBaseVertex{0};
  std::atomic<std::uint64_t> compatibleIndexedDrawMergeOnlyStartVertex{0};
  std::atomic<std::uint64_t> compatibleIndexedDrawMergeOnlyUniform{0};
  std::atomic<std::uint64_t> compatibleIndexedDrawMergeOnlyBindingOverride{0};
  std::atomic<std::uint64_t> compatibleIndexedDrawMergeOnlyBindingSnapshot{0};
  std::atomic<std::uint64_t> compatibleIndexedDrawMergeOnlyNonContiguous{0};
  std::atomic<std::uint64_t> compatibleIndexedDrawMergeOnlyOverflow{0};
  std::atomic<std::uint64_t> compatibleIndexedDrawMergeExactRelaxBindingPayload{0};
  std::atomic<std::uint64_t> compatibleIndexedDrawMergeExactRelaxUniform{0};
  std::atomic<std::uint64_t> compatibleIndexedDrawMergeExactRelaxBindingPayloadUniform{0};
  std::atomic<std::uint64_t> compatibleIndexedDrawMergeExactRelaxNonContiguous{0};
  std::atomic<std::uint64_t> compatibleIndexedDrawMergeExactRelaxBindingPayloadNonContiguous{0};
  std::atomic<std::uint64_t> compatibleIndexedDrawMergeExactRelaxUniformNonContiguous{0};
  std::atomic<std::uint64_t> compatibleIndexedDrawMergeExactRelaxBindingPayloadUniformNonContiguous{0};
  std::atomic<std::uint64_t> compatibleIndexedDrawMergeExactRelaxOther{0};
  std::atomic<std::uint64_t> indexedCacheOptCandidateDraws{0};
  std::atomic<std::uint64_t> indexedCacheOptCandidateSkipped{0};
  std::atomic<std::uint64_t> indexedCacheOptCandidateBytes{0};
  std::atomic<std::uint64_t> indexedCacheOptCandidateOriginalMiss16{0};
  std::atomic<std::uint64_t> indexedCacheOptCandidateOriginalMiss32{0};
  std::atomic<std::uint64_t> indexedCacheOptCandidateOriginalMiss64{0};
  std::atomic<std::uint64_t> indexedCacheOptCandidateMiss16{0};
  std::atomic<std::uint64_t> indexedCacheOptCandidateMiss32{0};
  std::atomic<std::uint64_t> indexedCacheOptCandidateMiss64{0};
  std::atomic<std::uint64_t> indexedCacheOptCandidateGatePass{0};
  std::atomic<std::uint64_t> indexedCacheOptCandidateGateFail{0};
  std::atomic<std::uint64_t> indexedCacheOptCandidateOpaqueDepthDraws{0};
  std::atomic<std::uint64_t> indexedCacheOptCandidateScreenBlendDraws{0};
  std::atomic<std::uint64_t> indexedCacheOptCandidatePrimitiveBucket1_63{0};
  std::atomic<std::uint64_t> indexedCacheOptCandidatePrimitiveBucket64_255{0};
  std::atomic<std::uint64_t> indexedCacheOptCandidatePrimitiveBucket256_1023{0};
  std::atomic<std::uint64_t> indexedCacheOptCandidatePrimitiveBucket1024_4095{0};
  std::atomic<std::uint64_t> indexedCacheOptCandidatePrimitiveBucket4096Plus{0};
  std::atomic<std::uint64_t> reorderedIndexCacheLookups{0};
  std::atomic<std::uint64_t> reorderedIndexCacheHits{0};
  std::atomic<std::uint64_t> reorderedIndexCacheRejectedHits{0};
  std::atomic<std::uint64_t> reorderedIndexCacheMisses{0};
  std::atomic<std::uint64_t> reorderedIndexCacheCreated{0};
  std::atomic<std::uint64_t> reorderedIndexCacheCreatedBytes{0};
  std::atomic<std::uint64_t> encodeDrawIssueCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawStreamBindViewportCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawStreamBindFfpCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawStreamBindVsCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawStreamBindTextureCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawStreamBindIndexCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawFvfDecodeDeclCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawFvfDecodeBytesCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawFvfDecodeExpandedCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawUniformBuildMainCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawUniformBuildFfpCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawUniformBuildVsCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawPhaseSetupCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawPhaseArgbufUniformCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawPhaseStreamPrepCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawPhaseFfpVertexCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawPhaseVertexBindCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawPhaseBaseStateCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawPhaseTileFfpFallthroughCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawPhaseRemainderCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawIssueCpuMaxNs{0};
  std::atomic<std::uint64_t> encodeDrawIssueIndexedCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawIssueNonIndexedCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawIssueExpandedIndexedCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawIssueSplitIndexedCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawIssueMetalCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawIssueVisibilityCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawPsoPrefetchHandleAvailable{0};
  std::atomic<std::uint64_t> encodeDrawPsoPrefetchHandleUsed{0};
  std::atomic<std::uint64_t> encodeDrawPsoPrefetchHandleMissing{0};
  std::atomic<std::uint64_t> encodeDrawPsoPrefetchBypassProbe{0};
  std::atomic<std::uint64_t> encodeDrawPsoPrefetchBypassBindingOverride{0};
  std::atomic<std::uint64_t> encodeDrawPsoPrefetchBindingOverride{0};
  std::atomic<std::uint64_t> encodeDrawPsoPrefetchBindingOverrideCompatible{0};
  std::atomic<std::uint64_t> encodeDrawPsoPrefetchBindingOverrideIncompatible{0};
  std::atomic<std::uint64_t> d3d9SnapshotDrawSubmissionCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotDrawSubmissionCpuMaxNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheLookupCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheHitCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheMissCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheDirectHitCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheDirectMissCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchHitCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBindingLayoutCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheUniformRefreshCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheUniformBuildCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheUniformHashCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheMissShaderLayoutCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheMissUniformBuildCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheMissHotBuildCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheDirectMissShaderLayoutCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheDirectMissUniformBuildCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheDirectMissHotBuildCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissShaderLayoutCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissShaderLayoutCompatibleHits{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissShaderLayoutCompatibleMisses{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissShaderLayoutReuseHits{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissShaderLayoutReuseMisses{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissUniformBuildCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissHotBuildCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissHotBuildZeroInitCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissHotBuildKeyCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissHotBuildKeyZeroInitCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissHotBuildKeyStreamCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissHotBuildKeyShaderCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissHotBuildKeyConstantCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissHotBuildKeyTextureCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissHotBuildKeySamplerCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissHotBuildKeyRenderStateCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissHotBuildKeyAttachmentCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissHotBuildKeyUniformCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissHotBuildBindingCopyCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissHotBuildRenderStateCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissHotBuildTextureStageStateCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissHotBuildSamplerStateCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissHotBuildTailCopyCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissHotBuildFlatRenderReuseHits{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissHotBuildFlatRenderReuseMisses{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissHotBuildFlatTssReuseHits{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissHotBuildFlatTssReuseMisses{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissHotBuildFlatSamplerReuseHits{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissHotBuildFlatSamplerReuseMisses{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissUniformNonConstHashReuseHits{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissUniformNonConstHashReuseMisses{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissUniformPayloadReuseFull{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissUniformPayloadReuseNonConst{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissUniformPayloadFullBuild{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissUniformVsConstHashReuse{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissUniformVsConstHashBuild{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissUniformPsConstHashReuse{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissUniformPsConstHashBuild{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissUniformVsConstHashMemoProbe{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissUniformVsConstHashMemoHits{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissUniformVsConstHashMemoMisses{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissUniformVsConstHashMemoStores{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissUniformPsConstHashMemoProbe{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissUniformPsConstHashMemoHits{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissUniformPsConstHashMemoMisses{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissUniformPsConstHashMemoStores{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformBuildCalls{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformBuildVsConstCopyCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformBuildPsConstCopyCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformBuildFfpMatrixCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformBuildFfpMaterialLightCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformBuildTextureTransformCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformBuildClipPlaneCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformBuildHashCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformBuildVsConstHashCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformBuildPsConstHashCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformBuildNonConstHashCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformBuildNonConstHashWorldViewProjCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformBuildNonConstHashFfpWorldViewCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformBuildNonConstHashFfpNormalMatrixCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformBuildNonConstHashMaterialCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformBuildNonConstHashLightsCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformBuildNonConstHashFfpBlendWvpCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformBuildNonConstHashTextureTransformsCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformBuildNonConstHashClipPlanesCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformBuildPayloadCombineHashCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformBuildVsConstHashFull{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformBuildPsConstHashFull{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformBuildVsConstHashFullNoUsage{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformBuildPsConstHashFullNoUsage{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformBuildVsConstHashFullUnknown{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformBuildPsConstHashFullUnknown{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformBuildVsConstHashFullUnknownBytecode{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformBuildPsConstHashFullUnknownBytecode{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformBuildVsConstHashFullUnknownNonBytecode{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformBuildPsConstHashFullUnknownNonBytecode{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformBuildVsConstHashFullIndexedFloat{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformBuildVsConstHashFullIndexedFloatMinSafeBytes{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformBuildVsConstHashFullIndexedFloatPotentialSavedBytes{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformBuildPsConstHashFullIndexedFloat{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformBuildVsConstHashFullIndexedInt{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformBuildPsConstHashFullIndexedInt{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformBuildVsConstHashFullIndexedBool{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformBuildPsConstHashFullIndexedBool{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformBuildVsConstHashBytes{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformBuildPsConstHashBytes{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformMaterialized{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformMaterializedBytes{0};
  std::atomic<std::uint64_t> d3d9SnapshotSubmissionCarrierRecords{0};
  std::atomic<std::uint64_t> d3d9SnapshotSubmissionCarrierBytes{0};
  std::atomic<std::uint64_t> d3d9SnapshotSubmissionCarrierStateStorageBytes{0};
  std::atomic<std::uint64_t> d3d9SnapshotSubmissionCarrierUniformStorageBytes{0};
  std::atomic<std::uint64_t> d3d9SnapshotSubmissionCarrierUnusedUniformStorageRecords{0};
  std::atomic<std::uint64_t> d3d9SnapshotSubmissionCarrierUnusedUniformStorageBytes{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformElided{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformElidedBytes{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformAdjacentSameGen{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformAdjacentSameGenBytes{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformAdjacentSameGenSameState{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformAdjacentSameGenSameStateBytes{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformAdjacentSameGenDiffState{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformAdjacentSameGenDiffStateBytes{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformAdjacentSamePayloadHash{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformAdjacentSamePayloadHashBytes{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformAdjacentSamePayloadHashSameState{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformAdjacentSamePayloadHashSameStateBytes{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformAdjacentSamePayloadHashDiffState{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformAdjacentSamePayloadHashDiffStateBytes{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformAdjacentSamePayloadHashDiffGeneration{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformAdjacentSamePayloadHashDiffGenerationBytes{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformAdjacentPreviousPayload{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformAdjacentSameVsConstHash{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformAdjacentSameVsConstHashSameState{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformAdjacentSameVsConstHashDiffGeneration{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformAdjacentSamePsConstHash{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformAdjacentSamePsConstHashSameState{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformAdjacentSamePsConstHashDiffGeneration{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformAdjacentSameShaderConstHashes{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformAdjacentSameShaderConstHashesSameState{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformAdjacentSameShaderConstHashesDiffGeneration{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformAdjacentSameFixedPayloadHash{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformAdjacentSameFixedPayloadHashSameState{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformAdjacentSameFixedPayloadHashDiffGeneration{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformAdjacentSameFixedAndShaderConstHashes{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformAdjacentSameFixedAndShaderConstHashesSameState{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformAdjacentSameFixedAndShaderConstHashesDiffGeneration{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissUniformBuildCalls{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissUniformBuildVsConstCopyCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissUniformBuildPsConstCopyCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissUniformBuildFfpMatrixCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissUniformBuildFfpMaterialLightCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissUniformBuildTextureTransformCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissUniformBuildClipPlaneCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissUniformBuildHashCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissUniformBuildVsConstHashCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissUniformBuildPsConstHashCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissUniformBuildNonConstHashCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissUniformBuildNonConstHashWorldViewProjCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissUniformBuildNonConstHashFfpWorldViewCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissUniformBuildNonConstHashFfpNormalMatrixCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissUniformBuildNonConstHashMaterialCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissUniformBuildNonConstHashLightsCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissUniformBuildNonConstHashFfpBlendWvpCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissUniformBuildNonConstHashTextureTransformsCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissUniformBuildNonConstHashClipPlanesCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissUniformBuildPayloadCombineHashCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissUniformBuildVsConstHashFull{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissUniformBuildPsConstHashFull{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissUniformBuildVsConstHashFullNoUsage{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissUniformBuildPsConstHashFullNoUsage{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissUniformBuildVsConstHashFullUnknown{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissUniformBuildPsConstHashFullUnknown{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissUniformBuildVsConstHashFullUnknownBytecode{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissUniformBuildPsConstHashFullUnknownBytecode{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissUniformBuildVsConstHashFullUnknownNonBytecode{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissUniformBuildPsConstHashFullUnknownNonBytecode{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissUniformBuildVsConstHashFullIndexedFloat{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissUniformBuildVsConstHashFullIndexedFloatMinSafeBytes{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissUniformBuildVsConstHashFullIndexedFloatPotentialSavedBytes{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissUniformBuildPsConstHashFullIndexedFloat{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissUniformBuildVsConstHashFullIndexedInt{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissUniformBuildPsConstHashFullIndexedInt{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissUniformBuildVsConstHashFullIndexedBool{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissUniformBuildPsConstHashFullIndexedBool{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissUniformBuildVsConstHashBytes{0};
  std::atomic<std::uint64_t> d3d9SnapshotCacheBatchMissUniformBuildPsConstHashBytes{0};
  std::atomic<std::uint64_t> d3d9SnapshotUniformCopyCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotStateCopyCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotStateMaterialized{0};
  std::atomic<std::uint64_t> d3d9SnapshotStateMaterializedBytes{0};
  std::atomic<std::uint64_t> d3d9SnapshotStateElided{0};
  std::atomic<std::uint64_t> d3d9SnapshotStateElidedBytes{0};
  std::atomic<std::uint64_t> d3d9SnapshotDebugSnapshotCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotFlatStateSamples{0};
  std::atomic<std::uint64_t> d3d9SnapshotFlatRenderStateEntries{0};
  std::atomic<std::uint64_t> d3d9SnapshotFlatRenderStateEntriesMax{0};
  std::atomic<std::uint64_t> d3d9SnapshotFlatRenderStateEntriesGt64{0};
  std::atomic<std::uint64_t> d3d9SnapshotFlatRenderStateEntriesGt128{0};
  std::atomic<std::uint64_t> d3d9SnapshotFlatRenderStateOverflow{0};
  std::atomic<std::uint64_t> d3d9SnapshotFlatTssEntries{0};
  std::atomic<std::uint64_t> d3d9SnapshotFlatTssStageEntriesMax{0};
  std::atomic<std::uint64_t> d3d9SnapshotFlatTssOverflow{0};
  std::atomic<std::uint64_t> d3d9SnapshotFlatSamplerEntries{0};
  std::atomic<std::uint64_t> d3d9SnapshotFlatSamplerSlotEntriesMax{0};
  std::atomic<std::uint64_t> d3d9SnapshotFlatSamplerOverflow{0};
  std::atomic<std::uint64_t> d3d9SnapshotBindingOverrideCpuNs{0};
  std::atomic<std::uint64_t> d3d9SnapshotBindingOverrideStreamScans{0};
  std::atomic<std::uint64_t> d3d9SnapshotBindingOverrideStreamRecords{0};
  std::atomic<std::uint64_t> d3d9SnapshotBindingOverrideIndexRecords{0};
  std::atomic<std::uint64_t> drawUniformPayloadLookupCandidateHits{0};
  std::atomic<std::uint64_t> drawUniformPayloadLookupLastHits{0};
  std::atomic<std::uint64_t> drawUniformPayloadLookupBucketHits{0};
  std::atomic<std::uint64_t> drawUniformPayloadLookupBucketMisses{0};
  std::atomic<std::uint64_t> drawUniformPayloadLookupLinearHits{0};
  std::atomic<std::uint64_t> drawUniformPayloadLookupBucketProbes{0};
  std::atomic<std::uint64_t> drawUniformPayloadLookupBucketCollisions{0};
  std::atomic<std::uint64_t> drawUniformPayloadLookupHashCollisions{0};
  std::atomic<std::uint64_t> drawUniformPayloadLookupSemanticHashMisses{0};
  std::atomic<std::uint64_t> drawUniformPayloadLookupSemanticHashMissBytes{0};
  std::atomic<std::uint64_t> drawUniformPayloadLookupCpuNs{0};
  std::atomic<std::uint64_t> drawUniformPayloadLookupCpuMaxNs{0};
  std::atomic<std::uint64_t> drawUniformPayloadLookupBucketCpuNs{0};
  std::atomic<std::uint64_t> drawUniformPayloadLookupBucketCpuMaxNs{0};
  std::atomic<std::uint64_t> drawUniformPayloadAppends{0};
  std::atomic<std::uint64_t> drawUniformPayloadAppendBytes{0};
  std::atomic<std::uint64_t> drawUniformFixedPayloadAppends{0};
  std::atomic<std::uint64_t> drawUniformFixedPayloadAppendBytes{0};
  std::atomic<std::uint64_t> drawUniformVertexConstantsAppends{0};
  std::atomic<std::uint64_t> drawUniformVertexConstantsAppendBytes{0};
  std::atomic<std::uint64_t> drawUniformPixelConstantsAppends{0};
  std::atomic<std::uint64_t> drawUniformPixelConstantsAppendBytes{0};
  std::atomic<std::uint64_t> drawUniformPayloadAppendFixedFindCpuNs{0};
  std::atomic<std::uint64_t> drawUniformPayloadAppendFixedFindCpuMaxNs{0};
  std::atomic<std::uint64_t> drawUniformPayloadAppendVertexFindCpuNs{0};
  std::atomic<std::uint64_t> drawUniformPayloadAppendVertexFindCpuMaxNs{0};
  std::atomic<std::uint64_t> drawUniformPayloadAppendPixelFindCpuNs{0};
  std::atomic<std::uint64_t> drawUniformPayloadAppendPixelFindCpuMaxNs{0};
  std::atomic<std::uint64_t> drawUniformPayloadAppendFixedAppendCpuNs{0};
  std::atomic<std::uint64_t> drawUniformPayloadAppendFixedAppendCpuMaxNs{0};
  std::atomic<std::uint64_t> drawUniformPayloadAppendVertexAppendCpuNs{0};
  std::atomic<std::uint64_t> drawUniformPayloadAppendVertexAppendCpuMaxNs{0};
  std::atomic<std::uint64_t> drawUniformPayloadAppendPixelAppendCpuNs{0};
  std::atomic<std::uint64_t> drawUniformPayloadAppendPixelAppendCpuMaxNs{0};
  std::atomic<std::uint64_t> drawUniformPayloadMaterialized{0};
  std::atomic<std::uint64_t> drawUniformPayloadMaterializedBytes{0};
  std::atomic<std::uint64_t> drawUniformPayloadMaterializeFallbacks{0};
  std::atomic<std::uint64_t> drawUniformPayloadMaterializeCpuNs{0};
  std::atomic<std::uint64_t> drawUniformPayloadMaterializeCpuMaxNs{0};
  std::atomic<std::uint64_t> drawUniformPayloadMaterializedOther{0};
  std::atomic<std::uint64_t> drawUniformPayloadMaterializedOtherBytes{0};
  std::atomic<std::uint64_t> drawUniformPayloadMaterializeOtherCpuNs{0};
  std::atomic<std::uint64_t> drawUniformPayloadMaterializedDrawEncoderCommand{0};
  std::atomic<std::uint64_t> drawUniformPayloadMaterializedDrawEncoderCommandBytes{0};
  std::atomic<std::uint64_t> drawUniformPayloadMaterializeDrawEncoderCommandCpuNs{0};
  std::atomic<std::uint64_t> drawUniformPayloadMaterializedDrawEncoderParam{0};
  std::atomic<std::uint64_t> drawUniformPayloadMaterializedDrawEncoderParamBytes{0};
  std::atomic<std::uint64_t> drawUniformPayloadMaterializeDrawEncoderParamCpuNs{0};
  std::atomic<std::uint64_t> drawUniformPayloadMaterializedFramegraphCommand{0};
  std::atomic<std::uint64_t> drawUniformPayloadMaterializedFramegraphCommandBytes{0};
  std::atomic<std::uint64_t> drawUniformPayloadMaterializeFramegraphCommandCpuNs{0};
  std::atomic<std::uint64_t> drawUniformPayloadMaterializedFramegraphParam{0};
  std::atomic<std::uint64_t> drawUniformPayloadMaterializedFramegraphParamBytes{0};
  std::atomic<std::uint64_t> drawUniformPayloadMaterializeFramegraphParamCpuNs{0};
  std::atomic<std::uint64_t> drawUniformPayloadMaterializedQueueObservation{0};
  std::atomic<std::uint64_t> drawUniformPayloadMaterializedQueueObservationBytes{0};
  std::atomic<std::uint64_t> drawUniformPayloadMaterializeQueueObservationCpuNs{0};
  std::atomic<std::uint64_t> drawUniformPayloadAppendReserveCpuNs{0};
  std::atomic<std::uint64_t> drawUniformPayloadAppendReserveCpuMaxNs{0};
  std::atomic<std::uint64_t> drawUniformPayloadAppendCopyCpuNs{0};
  std::atomic<std::uint64_t> drawUniformPayloadAppendCopyCpuMaxNs{0};
  std::atomic<std::uint64_t> drawUniformPayloadAppendLinkCpuNs{0};
  std::atomic<std::uint64_t> drawUniformPayloadAppendLinkCpuMaxNs{0};
  std::atomic<std::uint64_t> transientUploadCalls{0};
  std::atomic<std::uint64_t> transientUploadBytes{0};
  std::atomic<std::uint64_t> transientUploadCpuNs{0};
  std::atomic<std::uint64_t> transientUploadCpuMaxNs{0};
  std::atomic<std::uint64_t> d3d9BufferLockCalls{0};
  std::atomic<std::uint64_t> d3d9BufferLockNs{0};
  std::atomic<std::uint64_t> d3d9BufferLockMaxNs{0};
  std::atomic<std::uint64_t> d3d9BufferLockBytes{0};
  std::atomic<std::uint64_t> d3d9BufferLockDiscard{0};
  std::atomic<std::uint64_t> d3d9BufferLockNoOverwrite{0};
  std::atomic<std::uint64_t> d3d9BufferLockReadOnly{0};
  std::atomic<std::uint64_t> d3d9BufferLockPlain{0};
  std::atomic<std::uint64_t> d3d9BufferLockFullResource{0};
  std::atomic<std::uint64_t> d3d9BufferLockShadow{0};
  std::atomic<std::uint64_t> d3d9BufferLockShadowBytes{0};
  std::atomic<std::uint64_t> d3d9BufferLockShadowAllocNs{0};
  std::atomic<std::uint64_t> d3d9BufferLockShadowAllocMaxNs{0};
  std::atomic<std::uint64_t> d3d9BufferLockShadowCopyNs{0};
  std::atomic<std::uint64_t> d3d9BufferLockShadowCopyMaxNs{0};
  std::atomic<std::uint64_t> d3d9BufferLockDefaultPool{0};
  std::atomic<std::uint64_t> d3d9BufferLockManagedPool{0};
  std::atomic<std::uint64_t> d3d9BufferLockSystemMemPool{0};
  std::atomic<std::uint64_t> d3d9BufferLockScratchPool{0};
  std::atomic<std::uint64_t> d3d9BufferLockDynamic{0};
  std::atomic<std::uint64_t> d3d9BufferLockWriteOnly{0};
  std::atomic<std::uint64_t> uniformVsConstsCalls{0};
  std::atomic<std::uint64_t> uniformVsConstsBytes{0};
  std::atomic<std::uint64_t> uniformPsConstsCalls{0};
  std::atomic<std::uint64_t> uniformPsConstsBytes{0};
  std::atomic<std::uint64_t> uniformFfpVsCalls{0};
  std::atomic<std::uint64_t> uniformFfpVsBytes{0};
  std::atomic<std::uint64_t> uniformFfpPsCalls{0};
  std::atomic<std::uint64_t> uniformFfpPsBytes{0};
  std::atomic<std::uint64_t> uniformVolatilePushes{0};
  // R-BACK-5.7: managed-texture upload blit counter. Apple Silicon
  // (hasUnifiedMemory) must keep this at 0; non-zero indicates the
  // staging-copy fallback was hit when it should not have been.
  std::atomic<std::uint64_t> managedTextureUploadBlitCount{0};
  std::atomic<std::uint64_t> managedTextureUploadBlitBytes{0};
  std::atomic<std::uint64_t> texturePixelFormatViewSuppressedRtCount{0};
  std::atomic<std::uint64_t> texturePixelFormatViewSuppressedRtBytes{0};
  // R-BACK-14.* — MTLHeap small-resource pooling counters. Aggregate across
  // all heap families (priv-tex / shared-tex-um / shared-buf); per-family
  // breakdown can be added later if profiling shows mis-allocation between
  // families. `direct_fallback` advances on usage-flag mismatch fall-throughs;
  // `fragmentation_failure` advances on heap.makeTexture nil while not full;
  // `compaction` advances on heap retirement; `alloc_failure` on
  // newHeapWithDescriptor failure → fall-through to direct allocation.
  std::atomic<std::uint64_t> heapAllocCount{0};
  std::atomic<std::uint64_t> heapBytesAllocated{0};
  std::atomic<std::uint64_t> heapInstanceCount{0};
  std::atomic<std::uint64_t> heapDirectFallbackCount{0};
  std::atomic<std::uint64_t> heapFragmentationFailureCount{0};
  std::atomic<std::uint64_t> heapCompactionCount{0};
  std::atomic<std::uint64_t> heapAllocFailureCount{0};
  std::atomic<std::uint64_t> useHeapCalls{0};
  std::atomic<std::uint64_t> useResourceCalls{0};
  // R-BACK-13.* — Tile-Shader FFP fast-path counters (Apple Silicon only).
  std::atomic<std::uint64_t> tileFfpPassCount{0};
  std::atomic<std::uint64_t> portableFfpPassCount{0};
  std::atomic<std::uint64_t> tileFfpFallbackPrecision{0};
  std::atomic<std::uint64_t> tileFfpFallbackUnsupportedState{0};
  std::atomic<std::uint64_t> tileFfpFallbackGpuFamily{0};
  std::atomic<std::uint64_t> tileFfpFallbackMidPassIneligible{0};
  std::atomic<std::uint64_t> tileFfpMidPassResplitCount{0};
  // R-BACK-12.22~12.26 — Stage 2 Argbuf hybrid counters.
  std::atomic<std::uint64_t> argbufHybridEncoderCount{0};
  std::atomic<std::uint64_t> stage1EncoderCount{0};
  std::atomic<std::uint64_t> argbufHybridFallbackCount{0};
  std::atomic<std::uint64_t> argbufHybridBytesPerEncoder{0};
  std::atomic<std::uint64_t> stage1BytesPerEncoder{0};
  // R-BACK-3.7 / 3.8 / 4.8: binary-archive prewarming counters.
  std::atomic<std::uint64_t> prewarmEntriesLoaded{0};
  std::atomic<std::uint64_t> prewarmLoadCpuNs{0};
  std::atomic<std::uint64_t> prewarmFailureCorrupt{0};
  std::atomic<std::uint64_t> prewarmFailureSchema{0};
  std::atomic<std::uint64_t> prewarmFailureLockBusy{0};
  std::atomic<std::uint64_t> prewarmFailureMissing{0};
  std::atomic<std::uint64_t> coldCompileCountAfterWarm{0};
  std::atomic<std::uint64_t> archiveBytes{0};
  // R-BACK-3.9 / 3.10 / 3.11 — async prewarm hardening counters.
  std::atomic<std::uint64_t> prewarmDemotedBySize{0};
  std::atomic<std::uint64_t> prewarmAsyncCompletionCpuNs{0};
  std::atomic<std::uint64_t> prewarmMilestoneSaveCount{0};
  std::atomic<std::uint64_t> prewarmSaveSkippedDebugEnvCount{0};
  // D3DBC shader-decoder safe-rejection buckets (declarations in
  // dxmt9_perf_counters.hpp). Each counter MUST be a healthy 0 on a
  // passing probe — non-zero indicates a translator input regressed
  // or a workload genuinely fed malformed bytecode. The contract is
  // that a non-zero count here pairs with an empty SpirvModule +
  // fallback MSL stub, never a process abort.
  std::atomic<std::uint64_t> shaderDecoderRejectTruncated{0};
  std::atomic<std::uint64_t> shaderDecoderRejectUnsupportedVersion{0};
  std::atomic<std::uint64_t> shaderDecoderRejectOobRegister{0};
  std::atomic<std::uint64_t> shaderDecoderRejectMissingEnd{0};
  std::atomic<std::uint64_t> shaderDecoderRejectInvalidOpcode{0};
  // SM3 unsupported register kinds — see specs/d3d9.plan.md §3 P1-2.
  // `kD3DSPR_TEMPFLOAT16` (16) and `kD3DSPR_LABEL` (18) have no MSL
  // lowering; the decoder safe-rejects instead of misbinding.
  std::atomic<std::uint64_t> shaderDecoderRejectTempFloat16Unsupported{0};
  std::atomic<std::uint64_t> shaderDecoderRejectLabelUnsupported{0};
  // D3DDECLUSAGE / D3DDECLMETHOD safe-reject buckets — see
  // specs/d3d9/gap_d3d9.md §A.4 / §A.5. A category-level counter per side
  // keeps kCounterTable compact while still surfacing a real binary
  // that hits one of the unsupported codes.
  std::atomic<std::uint64_t> shaderDecoderRejectDeclUsageUnsupported{0};
  std::atomic<std::uint64_t> shaderDecoderRejectDeclMethodUnsupported{0};
  std::atomic<std::uint64_t> renderPassLoadActionLoad{0};
  std::atomic<std::uint64_t> renderPassLoadActionClear{0};
  std::atomic<std::uint64_t> renderPassLoadActionDontCare{0};
  std::atomic<std::uint64_t> renderPassLoadActionDepthLoad{0};
  std::atomic<std::uint64_t> renderPassLoadActionDepthClear{0};
  std::atomic<std::uint64_t> renderPassLoadActionDepthDontCare{0};
  std::atomic<std::uint64_t> renderPassLoadActionStencilLoad{0};
  std::atomic<std::uint64_t> renderPassLoadActionStencilClear{0};
  std::atomic<std::uint64_t> renderPassLoadActionStencilDontCare{0};
  std::atomic<std::uint64_t> renderPassStoreActionStore{0};
  std::atomic<std::uint64_t> renderPassStoreActionDontCare{0};
  std::atomic<std::uint64_t> renderPassStoreActionResolve{0};
  std::atomic<std::uint64_t> renderPassStoreActionDepthStore{0};
  std::atomic<std::uint64_t> renderPassStoreActionDepthDontCare{0};
  std::atomic<std::uint64_t> renderPassStoreActionStencilStore{0};
  std::atomic<std::uint64_t> renderPassStoreActionStencilDontCare{0};
  std::atomic<std::uint64_t> renderPassTilePreservationBytes{0};
  std::atomic<std::uint64_t> renderPassSameKeyAdjacent{0};
  std::atomic<std::uint64_t> renderPassSameKeyReentry{0};
  std::atomic<std::uint64_t> renderPassSameKeyReentryDistance1{0};
  std::atomic<std::uint64_t> renderPassSameKeyReentryDistance2{0};
  std::atomic<std::uint64_t> renderPassSameKeyReentryDistance3To4{0};
  std::atomic<std::uint64_t> renderPassSameKeyReentryDistance5To8{0};
  std::atomic<std::uint64_t> renderPassSameKeyReentryDistance9To16{0};
  std::atomic<std::uint64_t> renderPassSameKeyReentryDistance17Plus{0};
  std::atomic<std::uint64_t> renderPassSameKeyReentryDistance1SameColor{0};
  std::atomic<std::uint64_t> renderPassSameKeyReentryDistance1SameColorBytes{0};
  std::atomic<std::uint64_t> renderPassSameKeyReentryDistance1SameDepth{0};
  std::atomic<std::uint64_t> renderPassSameKeyReentryDistance1SameDepthBytes{0};
  std::atomic<std::uint64_t> renderPassSameKeyReentryDistance1RtDepthChange{0};
  std::atomic<std::uint64_t> renderPassSameKeyReentryDistance1RtDepthChangeBytes{0};
  std::atomic<std::uint64_t> renderPassSameKeyReentryDistance1SampleChange{0};
  std::atomic<std::uint64_t> renderPassSameKeyReentryDistance1SampleChangeBytes{0};
  std::atomic<std::uint64_t> renderPassSameKeyReentryPreservationBytes{0};
  std::atomic<std::uint64_t> renderPassSameKeyReentryColorPreservationBytes{0};
  std::atomic<std::uint64_t> renderPassSameKeyReentryDepthPreservationBytes{0};
  std::atomic<std::uint64_t> renderPassNaturalFallbackBegin{0};
  std::atomic<std::uint64_t>
      renderPassNaturalFallbackSameWindowReentryDistance1{0};
  std::atomic<std::uint64_t>
      renderPassNaturalFallbackSameWindowReentryDistance2{0};
  std::atomic<std::uint64_t>
      renderPassNaturalFallbackSameWindowReentryDistance3To4{0};
  std::atomic<std::uint64_t>
      renderPassNaturalFallbackCrossWindowReentryDistance1{0};
  std::atomic<std::uint64_t>
      renderPassNaturalFallbackCrossWindowReentryDistance2{0};
  std::atomic<std::uint64_t>
      renderPassNaturalFallbackCrossWindowReentryDistance3To4{0};
  std::atomic<std::uint64_t> activeSeedMergeTicketIssued{0};
  std::atomic<std::uint64_t> activeSeedMergeTicketMatched{0};
  std::atomic<std::uint64_t> activeSeedMergeTicketContinued{0};
  std::atomic<std::uint64_t> activeSeedMergeTicketMismatch{0};
  std::atomic<std::uint64_t> activeSeedMergeTicketUnconsumed{0};
  std::atomic<std::uint64_t> activeSeedMergeWitnessOverflow{0};
  std::atomic<std::uint64_t> activeSeedMergeWitnessMismatch{0};
  std::atomic<std::uint64_t> activeSeedInstanceUnavailable{0};
  std::atomic<std::uint64_t> activeSeedInstanceStale{0};
  std::atomic<std::uint64_t> renderPassActiveSeedBridgeReentryDistance1{0};
  std::atomic<std::uint64_t> renderPassActiveSeedBridgeReentryDistance2{0};
  std::atomic<std::uint64_t> renderPassActiveSeedBridgeReentryDistance3To4{0};
  std::atomic<std::uint64_t> renderPassFinalCloseSessionCap{0};
  std::atomic<std::uint64_t> renderPassFinalCloseIndependent{0};
  std::atomic<std::uint64_t> renderPassFinalCloseInitializer{0};
  std::atomic<std::uint64_t> renderPassFinalCloseProducerWait{0};
  std::atomic<std::uint64_t> renderPassFinalCloseDrain{0};
  std::atomic<std::uint64_t> renderPassFinalCloseFailOther{0};
  std::atomic<std::uint64_t> renderPassCloseAdjacentSessionCap{0};
  std::atomic<std::uint64_t> renderPassCloseAdjacentIndependent{0};
  std::atomic<std::uint64_t> renderPassCloseAdjacentInitializer{0};
  std::atomic<std::uint64_t> renderPassCloseAdjacentProducerWait{0};
  std::atomic<std::uint64_t> renderPassCloseAdjacentDrain{0};
  std::atomic<std::uint64_t> renderPassCloseAdjacentFailOther{0};
  std::atomic<std::uint64_t> renderPassNaturalShortCrossCloseFinal{0};
  std::atomic<std::uint64_t> renderPassNaturalShortCrossCloseRtChange{0};
  std::atomic<std::uint64_t> renderPassNaturalShortCrossCloseHazard{0};
  std::atomic<std::uint64_t> renderPassNaturalShortCrossCloseClear{0};
  std::atomic<std::uint64_t> renderPassNaturalShortCrossCloseSurfaceCopy{0};
  std::atomic<std::uint64_t> renderPassNaturalShortCrossCloseStretchRect{0};
  std::atomic<std::uint64_t> renderPassNaturalShortCrossCloseReadback{0};
  std::atomic<std::uint64_t> renderPassNaturalShortCrossCloseColorFill{0};
  std::atomic<std::uint64_t> renderPassNaturalShortCrossClosePresent{0};
  std::atomic<std::uint64_t> renderPassNaturalShortCrossClosePresentAcquire{0};
  std::atomic<std::uint64_t> renderPassNaturalShortCrossCloseTile{0};
  std::atomic<std::uint64_t> renderPassNaturalShortCrossCloseOrdered{0};
  std::atomic<std::uint64_t> renderPassNaturalShortCrossCloseMatched{0};
  std::atomic<std::uint64_t> renderPassNaturalShortCrossCloseMissing{0};
  std::atomic<std::uint64_t> renderPassShortReentryD1Ordinary{0};
  std::atomic<std::uint64_t> renderPassShortReentryD1NaturalSame{0};
  std::atomic<std::uint64_t> renderPassShortReentryD1NaturalCross{0};
  std::atomic<std::uint64_t> renderPassShortReentryD1Planned{0};
  std::atomic<std::uint64_t> renderPassShortReentryD1EligibilityPresent{0};
  std::atomic<std::uint64_t> renderPassShortReentryD1EligibilityOther{0};
  std::atomic<std::uint64_t> renderPassShortReentryD1PermutationRejected{0};
  std::atomic<std::uint64_t> renderPassShortReentryD1MixedInvalid{0};
  std::atomic<std::uint64_t> renderPassShortReentryD2Ordinary{0};
  std::atomic<std::uint64_t> renderPassShortReentryD2NaturalSame{0};
  std::atomic<std::uint64_t> renderPassShortReentryD2NaturalCross{0};
  std::atomic<std::uint64_t> renderPassShortReentryD2Planned{0};
  std::atomic<std::uint64_t> renderPassShortReentryD2EligibilityPresent{0};
  std::atomic<std::uint64_t> renderPassShortReentryD2EligibilityOther{0};
  std::atomic<std::uint64_t> renderPassShortReentryD2PermutationRejected{0};
  std::atomic<std::uint64_t> renderPassShortReentryD2MixedInvalid{0};
  std::atomic<std::uint64_t> renderPassShortReentryD1SourceAllSame{0};
  std::atomic<std::uint64_t>
      renderPassShortReentryD1SourcePriorInterveningSameCurrentNewer{0};
  std::atomic<std::uint64_t>
      renderPassShortReentryD1SourcePriorOlderInterveningCurrentSame{0};
  std::atomic<std::uint64_t> renderPassShortReentryD1SourceMixedInvalid{0};
  std::atomic<std::uint64_t> renderPassShortReentryD2SourceAllSame{0};
  std::atomic<std::uint64_t>
      renderPassShortReentryD2SourcePriorInterveningSameCurrentNewer{0};
  std::atomic<std::uint64_t>
      renderPassShortReentryD2SourcePriorOlderInterveningCurrentSame{0};
  std::atomic<std::uint64_t> renderPassShortReentryD2SourceMixedInvalid{0};
  std::atomic<std::uint64_t> renderPassShortReentryCloseFinal{0};
  std::atomic<std::uint64_t> renderPassShortReentryCloseRtChange{0};
  std::atomic<std::uint64_t> renderPassShortReentryCloseHazard{0};
  std::atomic<std::uint64_t> renderPassShortReentryCloseClear{0};
  std::atomic<std::uint64_t> renderPassShortReentryCloseSurfaceCopy{0};
  std::atomic<std::uint64_t> renderPassShortReentryCloseStretchRect{0};
  std::atomic<std::uint64_t> renderPassShortReentryCloseReadback{0};
  std::atomic<std::uint64_t> renderPassShortReentryCloseColorFill{0};
  std::atomic<std::uint64_t> renderPassShortReentryClosePresent{0};
  std::atomic<std::uint64_t> renderPassShortReentryClosePresentAcquire{0};
  std::atomic<std::uint64_t> renderPassShortReentryCloseTile{0};
  std::atomic<std::uint64_t> renderPassShortReentryCloseOrdered{0};
  std::atomic<std::uint64_t> renderPassShortReentryCloseMissing{0};
  std::atomic<std::uint64_t> renderPassShortReentryClearOpenTargetCount{0};
  std::atomic<std::uint64_t>
      renderPassShortReentryClearOpenTargetPriorStoreBytes{0};
  std::atomic<std::uint64_t>
      renderPassShortReentryClearOpenTargetCurrentLoadBytes{0};
  std::atomic<std::uint64_t>
      renderPassShortReentryClearOpenNaturalCrossCount{0};
  std::atomic<std::uint64_t>
      renderPassShortReentryClearOpenNaturalCrossPriorStoreBytes{0};
  std::atomic<std::uint64_t>
      renderPassShortReentryClearOpenNaturalCrossCurrentLoadBytes{0};
  std::atomic<std::uint64_t> renderPassCloseLedgerRecorded{0};
  std::atomic<std::uint64_t> renderPassCloseLedgerMissing{0};
  std::atomic<std::uint64_t> renderPassCloseLedgerTerminalAdjacent{0};
  std::atomic<std::uint64_t> renderPassCloseLedgerTerminalNonAdjacent{0};
  std::atomic<std::uint64_t>
      renderPassCloseLedgerTerminalNotReopenedBeforePresent{0};
  std::atomic<std::uint64_t> renderPassFinalCloseLedgerRecorded{0};
  std::atomic<std::uint64_t> renderPassFinalCloseLedgerMissing{0};
  std::atomic<std::uint64_t> renderPassFinalCloseLedgerTerminalAdjacent{0};
  std::atomic<std::uint64_t> renderPassFinalCloseLedgerTerminalNonAdjacent{0};
  std::atomic<std::uint64_t>
      renderPassFinalCloseLedgerTerminalNotReopenedBeforePresent{0};
  std::atomic<std::uint64_t> renderPassTransitionRtChangeSameDepth{0};
  std::atomic<std::uint64_t> renderPassTransitionSameRtDepthChange{0};
  std::atomic<std::uint64_t> renderPassTransitionRtDepthChange{0};
  std::atomic<std::uint64_t> renderPassColorProofAllowNextClear{0};
  std::atomic<std::uint64_t> renderPassColorProofAllowDeadNoPresent{0};
  std::atomic<std::uint64_t> renderPassColorProofBlockNullColor{0};
  std::atomic<std::uint64_t> renderPassColorProofBlockNoLookahead{0};
  std::atomic<std::uint64_t> renderPassColorProofBlockDrawTarget{0};
  std::atomic<std::uint64_t> renderPassColorProofBlockTextureSample{0};
  std::atomic<std::uint64_t> renderPassColorProofBlockSurfaceCopy{0};
  std::atomic<std::uint64_t> renderPassColorProofBlockStretchRect{0};
  std::atomic<std::uint64_t> renderPassColorProofBlockReadback{0};
  std::atomic<std::uint64_t> renderPassColorProofBlockColorFill{0};
  std::atomic<std::uint64_t> renderPassColorProofBlockMsaaResolve{0};
  std::atomic<std::uint64_t> renderPassColorProofBlockPresent{0};
  std::atomic<std::uint64_t> renderPassColorProofBlockDeadNoPresentDisabled{0};
  std::atomic<std::uint64_t> renderPassColorProofBlockClearMismatch{0};
  std::atomic<std::uint64_t> renderPassDepthProofAllowNextClear{0};
  std::atomic<std::uint64_t> renderPassDepthProofAllowDeadNoPresent{0};
  std::atomic<std::uint64_t> renderPassDepthProofBlockNullDepth{0};
  std::atomic<std::uint64_t> renderPassDepthProofBlockNoLookahead{0};
  std::atomic<std::uint64_t> renderPassDepthProofBlockMsaaResolve{0};
  std::atomic<std::uint64_t> renderPassDepthProofBlockDrawDepth{0};
  std::atomic<std::uint64_t> renderPassDepthProofBlockShadowSample{0};
  std::atomic<std::uint64_t> renderPassDepthProofBlockSurfaceCopy{0};
  std::atomic<std::uint64_t> renderPassDepthProofBlockStretchRect{0};
  std::atomic<std::uint64_t> renderPassDepthProofBlockReadback{0};
  std::atomic<std::uint64_t> renderPassDepthProofBlockColorFill{0};
  std::atomic<std::uint64_t> renderPassDepthProofBlockDepthResolve{0};
  std::atomic<std::uint64_t> renderPassDepthProofBlockPresent{0};
  std::atomic<std::uint64_t> renderPassDepthProofBlockClearMismatch{0};
  std::atomic<std::uint64_t> renderPassNoLookaheadEmpty{0};
  std::atomic<std::uint64_t> renderPassNoLookaheadInvalid{0};
  std::atomic<std::uint64_t> renderPassNoLookaheadSuffixExhausted{0};
  std::atomic<std::uint64_t> renderPassNoLookaheadStorageTruncated{0};
  std::atomic<std::uint64_t> renderPassLateStoreUnknownColor{0};
  std::atomic<std::uint64_t> renderPassLateStoreUnknownDepth{0};
  std::atomic<std::uint64_t> renderPassLateStoreUnknownStencil{0};
  std::atomic<std::uint64_t> renderPassLateStoreResolveClearColor{0};
  std::atomic<std::uint64_t> renderPassLateStoreResolveClearDepth{0};
  std::atomic<std::uint64_t> renderPassLateStoreResolveClearStencil{0};
  std::atomic<std::uint64_t> renderPassLateStoreResolveStoreClearMismatch{0};
  std::atomic<std::uint64_t> renderPassLateStoreResolveStoreDraw{0};
  std::atomic<std::uint64_t> renderPassLateStoreResolveStoreSample{0};
  std::atomic<std::uint64_t> renderPassLateStoreResolveStoreReadback{0};
  std::atomic<std::uint64_t> renderPassLateStoreResolveStoreCopy{0};
  std::atomic<std::uint64_t> renderPassLateStoreResolveStoreResolve{0};
  std::atomic<std::uint64_t> renderPassLateStoreResolveStorePresent{0};
  std::atomic<std::uint64_t> renderPassLateStoreResolveStoreIncompatibleClose{0};
  std::atomic<std::uint64_t> renderPassLateStoreResolveStoreDrain{0};
  std::atomic<std::uint64_t> renderPassLateStoreResolveStoreFinalize{0};
  std::atomic<std::uint64_t> renderPassLateStoreResolveStoreError{0};
  std::atomic<std::uint64_t> commandBufferCreateCpuNs{0};
  std::atomic<std::uint64_t> commandBufferCreateCpuMaxNs{0};
  std::atomic<std::uint64_t> commandBufferCommitCpuNs{0};
  std::atomic<std::uint64_t> commandBufferCommitCpuMaxNs{0};
  // Command-chunk boundary B2 — commit_chunk bridge-call latency
  // (countBridgeCommitLatencyNs). This is broader than the bridge ABI
  // crossing: it includes unix-side import/replay/queue-submit work, but
  // excludes asynchronous encode and GPU work after commit_chunk returns.
  std::atomic<std::uint64_t> bridgeCommitLatencyNs{0};
  std::atomic<std::uint64_t> bridgeCommitLatencyMaxNs{0};
  // Retired V1 importer phase columns kept at zero for stable perf output.
  // The raw-enqueue V1 phase columns are retired; the V2 offload counters
  // beginning at offloadReplayCpuNs remain live.
  std::atomic<std::uint64_t> offloadReplayCpuNs{0};
  std::atomic<std::uint64_t> offloadReplayCpuMaxNs{0};
  std::atomic<std::uint64_t> offloadReplayQueueDepthSamples{0};
  std::atomic<std::uint64_t> offloadReplayQueueDepthTotal{0};
  std::atomic<std::uint64_t> offloadReplayQueueDepthMax{0};
  std::atomic<std::uint64_t> offloadReplayQueueDepthGt1{0};
  std::atomic<std::uint64_t> offloadReplayQueueDepthGt2{0};
  std::atomic<std::uint64_t> offloadReplayQueueDepthGt4{0};
  // Drain-fence prologue (drainDeferredReplay).
  std::atomic<std::uint64_t> offloadDrainFenceWaits{0};
  std::atomic<std::uint64_t> offloadDrainFenceWaitNs{0};
  // Offload backpressure attribution: app-thread commit wall (entry to
  // offload-branch return, incl. push/ordinal waits), producer push waits
  // against the bounded raw queue, and worker pop-idle time.
  std::atomic<std::uint64_t> offloadCommitAppNs{0};
  // Heavy opt-in phase split of the synchronous half of commit_chunk
  // (DXMT9_PERF_COMMIT_CHUNK_PHASE_SPLIT). offloadCommitAppNs is one clock pair
  // around the whole call; these five decompose it. presentWait is separated
  // deliberately because it is a blocking wait the parent timer includes, so
  // leaving it lumped makes the CPU terms look larger than they are.
  std::atomic<std::uint64_t> commitChunkPhaseCalls{0};
  std::atomic<std::uint64_t> commitChunkPhasePrepareNs{0};
  std::atomic<std::uint64_t> commitChunkPhaseImportNs{0};
  std::atomic<std::uint64_t> commitChunkPhaseMarkNs{0};
  std::atomic<std::uint64_t> commitChunkPhaseMarkLockNs{0};
  std::atomic<std::uint64_t> commitChunkPhaseEnqueueNs{0};
  std::atomic<std::uint64_t> commitChunkPhasePresentWaitNs{0};
  std::atomic<std::uint64_t> offloadPushBackpressureWaits{0};
  std::atomic<std::uint64_t> offloadPushBackpressureWaitNs{0};
  std::atomic<std::uint64_t> offloadWorkerIdleWaitNs{0};
  std::atomic<std::uint64_t> completionEnqueueSamples{0};
  std::atomic<std::uint64_t> completionEnqueuePendingDepthMax{0};
  std::atomic<std::uint64_t> completionEnqueueWhileWaiting{0};
  std::atomic<std::uint64_t> completionEnqueueWhileWaitingPresent{0};
  std::atomic<std::uint64_t> completionWaits{0};
  std::atomic<std::uint64_t> completionWaitNs{0};
  std::atomic<std::uint64_t> completionWaitMaxNs{0};
  std::atomic<std::uint64_t> completionWaitWithEnqueue{0};
  std::atomic<std::uint64_t> completionWaitWithEnqueueNs{0};
  std::atomic<std::uint64_t> completionWaitWithoutEnqueue{0};
  std::atomic<std::uint64_t> completionWaitWithoutEnqueueNs{0};
  std::atomic<std::uint64_t> completionPresentWaitWithEnqueue{0};
  std::atomic<std::uint64_t> completionPresentWaitWithEnqueueNs{0};
  std::atomic<std::uint64_t> completionPresentWaitWithoutEnqueue{0};
  std::atomic<std::uint64_t> completionPresentWaitWithoutEnqueueNs{0};
  std::atomic<std::uint64_t> completionWaitEnqueuesDuringWait{0};
  std::atomic<std::uint64_t> completionWaitEnqueuesDuringWaitMax{0};
  std::atomic<std::uint64_t> completionSignalDelay{0};
  std::atomic<std::uint64_t> completionSignalDelayNs{0};
  std::atomic<std::uint64_t> completionWaitCommitChunkEntries{0};
  std::atomic<std::uint64_t> completionWaitCommitChunkReplayStarts{0};
  std::atomic<std::uint64_t> completionWaitCommitChunkReplayEnds{0};
  std::atomic<std::uint64_t> completionWaitCommitChunkReplayCpuNs{0};
  std::atomic<std::uint64_t> completionWaitCommitChunkReplayCpuMaxNs{0};
  std::atomic<std::uint64_t> completionWaitCommitPublishes{0};
  std::atomic<std::uint64_t> completionWaitEncodeDequeues{0};
  std::atomic<std::uint64_t> completionWaitCommandBufferCommits{0};
  std::atomic<std::uint64_t> completionWaitStagePublishToEncodeDequeue{0};
  std::atomic<std::uint64_t> completionWaitStagePublishToEncodeDequeueNs{0};
  std::atomic<std::uint64_t> completionWaitStagePublishToEncodeDequeueMaxNs{0};
  std::atomic<std::uint64_t> completionWaitStageEncodeDequeueToCommandBufferCommit{0};
  std::atomic<std::uint64_t> completionWaitStageEncodeDequeueToCommandBufferCommitNs{0};
  std::atomic<std::uint64_t> completionWaitStageEncodeDequeueToCommandBufferCommitMaxNs{0};
  std::atomic<std::uint64_t> completionNoEnqueueWaitToCommitChunkEntry{0};
  std::atomic<std::uint64_t> completionNoEnqueueWaitToCommitChunkEntryNs{0};
  std::atomic<std::uint64_t> completionNoEnqueueWaitToCommitChunkEntryMaxNs{0};
  std::atomic<std::uint64_t> completionNoEnqueueWaitToCommitChunkReplayStart{0};
  std::atomic<std::uint64_t> completionNoEnqueueWaitToCommitChunkReplayStartNs{0};
  std::atomic<std::uint64_t> completionNoEnqueueWaitToCommitChunkReplayStartMaxNs{0};
  std::atomic<std::uint64_t> completionNoEnqueueWaitToCommitChunkReplayEnd{0};
  std::atomic<std::uint64_t> completionNoEnqueueWaitToCommitChunkReplayEndNs{0};
  std::atomic<std::uint64_t> completionNoEnqueueWaitToCommitChunkReplayEndMaxNs{0};
  std::atomic<std::uint64_t> completionNoEnqueueWaitToCommitPublish{0};
  std::atomic<std::uint64_t> completionNoEnqueueWaitToCommitPublishNs{0};
  std::atomic<std::uint64_t> completionNoEnqueueWaitToCommitPublishMaxNs{0};
  std::atomic<std::uint64_t> completionNoEnqueueCommitChunkEntriesBeforePublish{0};
  std::atomic<std::uint64_t> completionNoEnqueueCommitChunkEntriesBeforePublishMax{0};
  std::atomic<std::uint64_t> completionNoEnqueueCommitChunkReplayStartsBeforePublish{0};
  std::atomic<std::uint64_t> completionNoEnqueueCommitChunkReplayStartsBeforePublishMax{0};
  std::atomic<std::uint64_t> completionNoEnqueueCommitChunkReplayEndsBeforePublish{0};
  std::atomic<std::uint64_t> completionNoEnqueueCommitChunkReplayEndsBeforePublishMax{0};
  std::atomic<std::uint64_t> completionNoEnqueueCommitChunkCompletedReplayCpuBeforePublish{0};
  std::atomic<std::uint64_t> completionNoEnqueueCommitChunkCompletedReplayCpuBeforePublishNs{0};
  std::atomic<std::uint64_t> completionNoEnqueueCommitChunkCompletedReplayCpuBeforePublishMaxNs{0};
  std::atomic<std::uint64_t> completionNoEnqueueCommitChunkActiveReplayCpuBeforePublish{0};
  std::atomic<std::uint64_t> completionNoEnqueueCommitChunkActiveReplayCpuBeforePublishNs{0};
  std::atomic<std::uint64_t> completionNoEnqueueCommitChunkActiveReplayCpuBeforePublishMaxNs{0};
  std::atomic<std::uint64_t> completionNoEnqueueCommitChunkInterReplayGapBeforePublish{0};
  std::atomic<std::uint64_t> completionNoEnqueueCommitChunkInterReplayGapBeforePublishNs{0};
  std::atomic<std::uint64_t> completionNoEnqueueCommitChunkInterReplayGapBeforePublishMaxNs{0};
  std::atomic<std::uint64_t> completionNoEnqueueCommitPublishWaitBeforePublish{0};
  std::atomic<std::uint64_t> completionNoEnqueueCommitPublishWaitBeforePublishNs{0};
  std::atomic<std::uint64_t> completionNoEnqueueCommitPublishWaitBeforePublishMaxNs{0};
  std::atomic<std::uint64_t> completionNoEnqueueCommitPublishOnBeforePublishCpu{0};
  std::atomic<std::uint64_t> completionNoEnqueueCommitPublishOnBeforePublishCpuNs{0};
  std::atomic<std::uint64_t> completionNoEnqueueCommitPublishOnBeforePublishCpuMaxNs{0};
  std::atomic<std::uint64_t> completionNoEnqueueCommitChunkShapeSamplesBeforePublish{0};
  std::atomic<std::uint64_t> completionNoEnqueueCommitChunkRecordsBeforePublish{0};
  std::atomic<std::uint64_t> completionNoEnqueueCommitChunkRecordsBeforePublishMax{0};
  std::atomic<std::uint64_t> completionNoEnqueueCommitChunkChunksWithDrawBeforePublish{0};
  std::atomic<std::uint64_t> completionNoEnqueueCommitChunkChunksWithPresentBeforePublish{0};
  std::atomic<std::uint64_t> completionNoEnqueueCommitChunkChunksStateConstOnlyBeforePublish{0};
  std::atomic<std::uint64_t> completionNoEnqueueCommitChunkChunksNoDrawNoPresentBeforePublish{0};
  std::atomic<std::uint64_t> completionNoEnqueueCommitChunkDrawRecordsBeforePublish{0};
  std::atomic<std::uint64_t> completionNoEnqueueCommitChunkConstRecordsBeforePublish{0};
  std::atomic<std::uint64_t> completionNoEnqueueCommitChunkApplyStateRecordsBeforePublish{0};
  std::atomic<std::uint64_t> completionNoEnqueueCommitChunkClearRecordsBeforePublish{0};
  std::atomic<std::uint64_t> completionNoEnqueueCommitChunkPresentRecordsBeforePublish{0};
  std::atomic<std::uint64_t> completionNoEnqueueCommitChunkSurfaceRecordsBeforePublish{0};
  std::atomic<std::uint64_t> completionNoEnqueueCommitChunkQueryRecordsBeforePublish{0};
  std::atomic<std::uint64_t> completionNoEnqueueCommitChunkOtherRecordsBeforePublish{0};
  std::atomic<std::uint64_t> completionNoEnqueueFirstPublishSlotSamples{0};
  std::atomic<std::uint64_t> completionNoEnqueueFirstPublishSlotCommands{0};
  std::atomic<std::uint64_t> completionNoEnqueueFirstPublishSlotCommandsMax{0};
  std::atomic<std::uint64_t> completionNoEnqueueFirstPublishSlotDrawRunCommands{0};
  std::atomic<std::uint64_t> completionNoEnqueueFirstPublishSlotDrawItems{0};
  std::atomic<std::uint64_t> completionNoEnqueueFirstPublishSlotDrawItemsMax{0};
  std::atomic<std::uint64_t> completionNoEnqueueFirstPublishSlotNonDrawCommands{0};
  std::atomic<std::uint64_t> completionNoEnqueueFirstPublishSlotPayloadBytes{0};
  std::atomic<std::uint64_t> completionNoEnqueueFirstPublishSlotPayloadBytesMax{0};
  std::atomic<std::uint64_t> completionNoEnqueueFirstPublishSlotPresentCommands{0};
  std::atomic<std::uint64_t> completionNoEnqueueFirstPublishSlotPrePresentCommands{0};
  std::atomic<std::uint64_t> completionNoEnqueueFirstPublishSlotPrePresentCommandsMax{0};
  std::atomic<std::uint64_t> completionNoEnqueueFirstPublishSlotPrePresentDrawRunCommands{0};
  std::atomic<std::uint64_t> completionNoEnqueueFirstPublishSlotPrePresentDrawItems{0};
  std::atomic<std::uint64_t> completionNoEnqueueFirstPublishSlotPrePresentDrawItemsMax{0};
  std::atomic<std::uint64_t> completionNoEnqueueFirstPublishSlotPrePresentNonDrawCommands{0};
  std::atomic<std::uint64_t> completionNoEnqueueFirstPublishSlotPrePresentPayloadBytes{0};
  std::atomic<std::uint64_t> completionNoEnqueueFirstPublishSlotPrePresentPayloadBytesMax{0};
  std::atomic<std::uint64_t> completionNoEnqueueFirstPublishSlotPostPresentCommands{0};
  std::atomic<std::uint64_t> completionNoEnqueueFirstPublishSlotPresentTailSlots{0};
  std::atomic<std::uint64_t> completionNoEnqueueFirstPublishSlotPresentNonTailSlots{0};
  std::atomic<std::uint64_t> completionNoEnqueueWaitToEncodeDequeue{0};
  std::atomic<std::uint64_t> completionNoEnqueueWaitToEncodeDequeueNs{0};
  std::atomic<std::uint64_t> completionNoEnqueueWaitToEncodeDequeueMaxNs{0};
  std::atomic<std::uint64_t> completionNoEnqueueWaitToCommandBufferCommit{0};
  std::atomic<std::uint64_t> completionNoEnqueueWaitToCommandBufferCommitNs{0};
  std::atomic<std::uint64_t> completionNoEnqueueWaitToCommandBufferCommitMaxNs{0};
  std::atomic<std::uint64_t> encodeDequeueReadyDepthSamples{0};
  std::atomic<std::uint64_t> encodeDequeueReadyDepthTotal{0};
  std::atomic<std::uint64_t> encodeDequeueReadyDepthMax{0};
  std::atomic<std::uint64_t> encodeDequeueReadyDepthGt1{0};
  std::atomic<std::uint64_t> encodeDequeueReadyDepthGt2{0};
  std::atomic<std::uint64_t> encodeDequeueReadyDepthGt4{0};
  std::atomic<std::uint64_t> completionNoEnqueueStageCommitEntryToPublish{0};
  std::atomic<std::uint64_t> completionNoEnqueueStageCommitEntryToPublishNs{0};
  std::atomic<std::uint64_t> completionNoEnqueueStageCommitEntryToPublishMaxNs{0};
  std::atomic<std::uint64_t> completionNoEnqueueStagePublishToEncodeDequeue{0};
  std::atomic<std::uint64_t> completionNoEnqueueStagePublishToEncodeDequeueNs{0};
  std::atomic<std::uint64_t> completionNoEnqueueStagePublishToEncodeDequeueMaxNs{0};
  std::atomic<std::uint64_t> completionNoEnqueueStageEncodeDequeueToCommandBufferCommit{0};
  std::atomic<std::uint64_t> completionNoEnqueueStageEncodeDequeueToCommandBufferCommitNs{0};
  std::atomic<std::uint64_t> completionNoEnqueueStageEncodeDequeueToCommandBufferCommitMaxNs{0};
  std::atomic<std::uint64_t> completionNoEnqueueWaitToNextEnqueue{0};
  std::atomic<std::uint64_t> completionNoEnqueueWaitToNextEnqueueNs{0};
  std::atomic<std::uint64_t> completionNoEnqueueWaitToNextEnqueueMaxNs{0};
  std::atomic<std::uint64_t> completionNoEnqueueWaitToNextPresentEnqueue{0};
  std::atomic<std::uint64_t> completionNoEnqueueWaitToNextPresentEnqueueNs{0};
  std::atomic<std::uint64_t> completionDequeueSamples{0};
  std::atomic<std::uint64_t> completionDequeueAgeNs{0};
  std::atomic<std::uint64_t> completionDequeueAgeMaxNs{0};
  std::atomic<std::uint64_t> completionPendingDepthMax{0};
  std::atomic<std::uint64_t> completionDequeueStatusNotEnqueued{0};
  std::atomic<std::uint64_t> completionDequeueStatusEnqueued{0};
  std::atomic<std::uint64_t> completionDequeueStatusCommitted{0};
  std::atomic<std::uint64_t> completionDequeueStatusScheduled{0};
  std::atomic<std::uint64_t> completionDequeueStatusCompleted{0};
  std::atomic<std::uint64_t> completionDequeueStatusError{0};
  std::atomic<std::uint64_t> completionDequeueStatusUnknown{0};
  std::atomic<std::uint64_t> completionWaitStatusNotEnqueued{0};
  std::atomic<std::uint64_t> completionWaitStatusNotEnqueuedNs{0};
  std::atomic<std::uint64_t> completionWaitStatusEnqueued{0};
  std::atomic<std::uint64_t> completionWaitStatusEnqueuedNs{0};
  std::atomic<std::uint64_t> completionWaitStatusCommitted{0};
  std::atomic<std::uint64_t> completionWaitStatusCommittedNs{0};
  std::atomic<std::uint64_t> completionWaitStatusScheduled{0};
  std::atomic<std::uint64_t> completionWaitStatusScheduledNs{0};
  std::atomic<std::uint64_t> completionWaitStatusUnknown{0};
  std::atomic<std::uint64_t> completionWaitStatusUnknownNs{0};
  std::atomic<std::uint64_t> completionPresentWaits{0};
  std::atomic<std::uint64_t> completionPresentWaitNs{0};
  std::atomic<std::uint64_t> completionPresentWaitMaxNs{0};
  std::atomic<std::uint64_t> completionDrawWaits{0};
  std::atomic<std::uint64_t> completionDrawWaitNs{0};
  std::atomic<std::uint64_t> completionDrawWaitMaxNs{0};
  std::atomic<std::uint64_t> completionBlitWaits{0};
  std::atomic<std::uint64_t> completionBlitWaitNs{0};
  std::atomic<std::uint64_t> completionBlitWaitMaxNs{0};
  std::atomic<std::uint64_t> completionPresentOnlyWaits{0};
  std::atomic<std::uint64_t> completionPresentOnlyWaitNs{0};
  std::atomic<std::uint64_t> completionPresentOnlyWaitMaxNs{0};
  std::atomic<std::uint64_t> completionDrawPresentWaits{0};
  std::atomic<std::uint64_t> completionDrawPresentWaitNs{0};
  std::atomic<std::uint64_t> completionDrawPresentWaitMaxNs{0};
  std::atomic<std::uint64_t> completionDrawStretchWaits{0};
  std::atomic<std::uint64_t> completionDrawStretchWaitNs{0};
  std::atomic<std::uint64_t> completionDrawStretchWaitMaxNs{0};
  std::atomic<std::uint64_t> completionStretchWaits{0};
  std::atomic<std::uint64_t> completionStretchWaitNs{0};
  std::atomic<std::uint64_t> completionStretchWaitMaxNs{0};
  std::atomic<std::uint64_t> completionBlitOnlyWaits{0};
  std::atomic<std::uint64_t> completionBlitOnlyWaitNs{0};
  std::atomic<std::uint64_t> completionBlitOnlyWaitMaxNs{0};
  std::atomic<std::uint64_t> completionOtherWaits{0};
  std::atomic<std::uint64_t> completionOtherWaitNs{0};
  std::atomic<std::uint64_t> completionOtherWaitMaxNs{0};
  std::atomic<std::uint64_t> syncWaits{0};
  std::atomic<std::uint64_t> syncWaitNs{0};
  std::atomic<std::uint64_t> syncWaitMaxNs{0};
  std::atomic<std::uint64_t> queueWriterWaits{0};
  std::atomic<std::uint64_t> queueWriterWaitNs{0};
  std::atomic<std::uint64_t> queueWriterWaitMaxNs{0};
  std::atomic<std::uint64_t> queueCommitWaits{0};
  std::atomic<std::uint64_t> queueCommitWaitNs{0};
  std::atomic<std::uint64_t> queueCommitWaitMaxNs{0};
  std::atomic<std::uint64_t> queueSequenceWaits{0};
  std::atomic<std::uint64_t> queueSequenceWaitNs{0};
  std::atomic<std::uint64_t> queueSequenceWaitMaxNs{0};
  std::atomic<std::uint64_t> cpuReadyTapeResidentSources{0};
  std::atomic<std::uint64_t> cpuReadyTapeResidentSourcesPeak{0};
  std::atomic<std::uint64_t> cpuReadyTapeResidentPages{0};
  std::atomic<std::uint64_t> cpuReadyTapeResidentPagesPeak{0};
  std::atomic<std::uint64_t> cpuReadyTapeReadyEntries{0};
  std::atomic<std::uint64_t> cpuReadyTapeReadyEntriesPeak{0};
  std::atomic<std::uint64_t> cpuReadyTapeAdmissionCloses{0};
  std::atomic<std::uint64_t> cpuReadyTapeAdmissionReopens{0};
  std::atomic<std::uint64_t> cpuReadyTapeWrapPaddingPages{0};
  std::atomic<std::uint64_t> cpuReadyTapeAdmissionWaits{0};
  std::atomic<std::uint64_t> cpuReadyTapeAdmissionWaitNs{0};
  std::atomic<std::uint64_t> cpuReadyTapeAdmissionWaitMaxNs{0};
  std::atomic<std::uint64_t> cpuReadyTapeLegacyOversizeBypass{0};
  std::atomic<std::uint64_t> cpuReadyTapeReclaimWakeups{0};
  std::atomic<std::uint64_t> mapBufferCalls{0};
  std::atomic<std::uint64_t> mapBufferWaitSeq{0};
  std::atomic<std::uint64_t> mapBufferNoWaitSeq{0};
  std::atomic<std::uint64_t> mapBufferTotalNs{0};
  std::atomic<std::uint64_t> mapBufferTotalMaxNs{0};
  std::atomic<std::uint64_t> mapBufferMutexWaitNs{0};
  std::atomic<std::uint64_t> mapBufferMutexWaitMaxNs{0};
  std::atomic<std::uint64_t> mapBufferWaitNs{0};
  std::atomic<std::uint64_t> mapBufferWaitMaxNs{0};
  std::atomic<std::uint64_t> mapBufferDiscard{0};
  std::atomic<std::uint64_t> mapBufferNoOverwrite{0};
  std::atomic<std::uint64_t> mapBufferReadOnly{0};
  std::atomic<std::uint64_t> mapBufferPlain{0};
  std::atomic<std::uint64_t> managedBufferUploads{0};
  std::atomic<std::uint64_t> managedBufferUploadBytes{0};
  std::atomic<std::uint64_t> managedBufferBackingInPlace{0};
  std::atomic<std::uint64_t> managedBufferBackingReuse{0};
  std::atomic<std::uint64_t> managedBufferBackingFresh{0};
  std::atomic<std::uint64_t> presentBoundaryApplied{0};
  std::atomic<std::uint64_t> presentBoundarySkipped{0};
  std::atomic<std::uint64_t> presentBoundaryDeferred{0};
  std::atomic<std::uint64_t> presentBoundaryDeferredWaits{0};
  std::atomic<std::uint64_t> presentBoundaryWaits{0};
  std::atomic<std::uint64_t> presentBoundaryWaitNs{0};
  std::atomic<std::uint64_t> presentBoundaryWaitMaxNs{0};
  std::atomic<std::uint64_t> presentOrdinalBoundaryWaits{0};
  std::atomic<std::uint64_t> presentOrdinalBoundaryWaitNs{0};
  std::atomic<std::uint64_t> completedPresentOrdinal{0};
  std::atomic<std::uint64_t> presentEncoded{0};
  std::atomic<std::uint64_t> presentSkipped{0};
  std::atomic<std::uint64_t> presentFullscreen{0};
  std::atomic<std::uint64_t> presentSourceSelections{0};
  std::atomic<std::uint64_t> presentSourceExplicit{0};
  std::atomic<std::uint64_t> presentSourceCurrentBackBuffer{0};
  std::atomic<std::uint64_t> presentSourceChecks{0};
  std::atomic<std::uint64_t> presentSourceValid{0};
  std::atomic<std::uint64_t> presentSourceMissingSurface{0};
  std::atomic<std::uint64_t> presentSourceMissingTexture{0};
  std::atomic<std::uint64_t> presentSourceResolve{0};
  std::atomic<std::uint64_t> presentSourceInvalidSize{0};
  std::atomic<std::uint64_t> presentSourceWidth{0};
  std::atomic<std::uint64_t> presentSourceHeight{0};
  std::atomic<std::uint64_t> presentSourceFormat{0};
  std::atomic<std::uint64_t> presentSourceSampleCount{0};
  std::atomic<std::uint64_t> presentSourceHandle{0};
  std::atomic<std::uint64_t> presentSourceTextureHandle{0};
  std::atomic<std::uint64_t> presentPass{0};
  std::atomic<std::uint64_t> presentPassSrcWidth{0};
  std::atomic<std::uint64_t> presentPassSrcHeight{0};
  std::atomic<std::uint64_t> presentPassDstWidth{0};
  std::atomic<std::uint64_t> presentPassDstHeight{0};
  std::atomic<std::uint64_t> presentPassDstMaxWidth{0};
  std::atomic<std::uint64_t> presentPassDstMaxHeight{0};
  std::atomic<std::uint64_t> presentScheduleRequestedSync{0};
  std::atomic<std::uint64_t> presentScheduleRequestedImmediate{0};
  std::atomic<std::uint64_t> presentScheduleAfterMinimumDuration{0};
  std::atomic<std::uint64_t> presentScheduleImmediate{0};
  std::atomic<std::uint64_t> presentMinimumDurationNs{0};
  std::atomic<std::uint64_t> presentMinimumDurationMaxNs{0};
  std::atomic<std::uint64_t> presentAcquireWaits{0};
  std::atomic<std::uint64_t> presentAcquireWaitNs{0};
  std::atomic<std::uint64_t> presentAcquireWaitMaxNs{0};
  std::atomic<std::uint64_t> presentAcquireSlowWaits{0};
  std::atomic<std::uint64_t> presentAsyncAcquireRequests{0};
  std::atomic<std::uint64_t> presentAsyncAcquireIssued{0};
  std::atomic<std::uint64_t> presentAsyncAcquireFallbacks{0};
  std::atomic<std::uint64_t> presentAsyncAcquireWaits{0};
  std::atomic<std::uint64_t> presentAsyncAcquireWaitNs{0};
  std::atomic<std::uint64_t> presentAsyncAcquireWaitMaxNs{0};
  std::atomic<std::uint64_t> presentAsyncAcquireSlowWaits{0};
  std::atomic<std::uint64_t> presentTokenWaits{0};
  std::atomic<std::uint64_t> presentTokenWaitNs{0};
  std::atomic<std::uint64_t> presentTokenWaitMaxNs{0};
  std::atomic<std::uint64_t> presentTokenSlowWaits{0};
  std::atomic<std::uint64_t> presentPreAcquireRequests{0};
  std::atomic<std::uint64_t> presentPreAcquireHits{0};
  std::atomic<std::uint64_t> presentPreAcquireMisses{0};
  std::atomic<std::uint64_t> presentPreAcquireWaitNs{0};
  std::atomic<std::uint64_t> presentPreAcquireWaitMaxNs{0};
  std::atomic<std::uint64_t> presentSetPropsWaits{0};
  std::atomic<std::uint64_t> presentSetPropsWaitNs{0};
  // R-BACK-39.2 (Task B11, L1 subset) — frame-graph observe-path gauges.
  // Wired from the FrameGraphBackend observe path (framegraph_backend.cpp):
  // built/coalesced/dead/memoryless reflect the FrameGraph + OptimizerStats
  // that runOptimizer produces per observed chunk; dagDumpsWritten counts
  // observe invocations that ran the build+optimize export side-channel.
  // L2/L3 / on-device counters (framegraph_icb_*,
  // framegraph_virtual_attachment_misclassification_stale_persistent) are
  // DEFERRED — they have no L1 callsite and would trip the callsite audit.
  std::atomic<std::uint64_t> framegraphPassesBuilt{0};
  std::atomic<std::uint64_t> framegraphPassesCoalesced{0};
  std::atomic<std::uint64_t> framegraphPassesDead{0};
  std::atomic<std::uint64_t> framegraphResourcesMemoryless{0};
  std::atomic<std::uint64_t> framegraphDagDumpsWritten{0};
  std::atomic<std::uint64_t> framegraphDceDropped{0};
  std::atomic<std::uint64_t> framegraphDcePreservedUnprovable{0};
  std::atomic<std::uint64_t> framegraphDceCrossChunkProofResources{0};
  std::atomic<std::uint64_t> framegraphDceReplayCommandsOmitted{0};
  std::atomic<std::uint64_t> framegraphDceLookaheadPrefixes{0};
  std::atomic<std::uint64_t> framegraphDceLookaheadPrefixCommands{0};
  std::atomic<std::uint64_t> framegraphDceLookaheadSelected{0};
  std::atomic<std::uint64_t> framegraphDceLookaheadFailOpen{0};
  std::atomic<std::uint64_t> framegraphSourceLocalReturnCandidates{0};
  std::atomic<std::uint64_t> framegraphSourceLocalReturnMerged{0};
  std::atomic<std::uint64_t> framegraphSourceLocalReturnBlockedCycle{0};
  std::atomic<std::uint64_t> framegraphSourceLocalReturnSecondNonDraw{0};
  std::atomic<std::uint64_t> framegraphSourceLocalReturnNonRenderIntervener{0};
  std::atomic<std::uint64_t> framegraphSourceLocalReturnMissingInvariant{0};
  std::atomic<std::uint64_t> framegraphSourceLocalReturnDependencyKept{0};
  std::atomic<std::uint64_t> framegraphSourceLocalReturnMoveBefore{0};
  std::atomic<std::uint64_t> framegraphSourceLocalReturnMoveAfter{0};
  std::atomic<std::uint64_t> framegraphSourceLocalReturnNonDrawIntervener{0};
  std::atomic<std::uint64_t> framegraphSourceLocalReturnSemanticIntervener{0};
  std::atomic<std::uint64_t> framegraphSourceLocalReturnCommandlessIntervener{0};
  std::atomic<std::uint64_t> framegraphSourceLocalReturnCommandless{0};
  std::atomic<std::uint64_t> framegraphSourceLocalReturnLegacyCandidates{0};
  std::atomic<std::uint64_t> framegraphSourceLocalReturnArenaCandidates{0};
  std::atomic<std::uint64_t> framegraphSourceLocalReturnUnknownCandidates{0};
  std::atomic<std::uint64_t> framegraphSourceLocalReturnIdentityKnownCandidates{0};
  std::atomic<std::uint64_t> framegraphSourceLocalReturnIdentityMissingCandidates{0};
  std::atomic<std::uint64_t> framegraphSourceLocalReturnFrontierRollbackSources{0};
  std::atomic<std::uint64_t> framegraphSourceLocalReturnFrontierRollbackCandidates{0};
  std::atomic<std::uint64_t> framegraphSourceLocalReturnFrontierRollbackMerged{0};
  std::atomic<std::uint64_t> framegraphSourceLocalReturnFrontierRollbackInvalidPlanSources{0};
  std::atomic<std::uint64_t> framegraphSourceLocalReturnFrontierRollbackInvalidPlanCandidates{0};
  std::atomic<std::uint64_t> framegraphSourceLocalReturnFrontierRollbackInvalidPlanMerged{0};
  std::atomic<std::uint64_t> framegraphSourceLocalReturnFrontierRollbackLiveSetMismatchSources{0};
  std::atomic<std::uint64_t> framegraphSourceLocalReturnFrontierRollbackLiveSetMismatchCandidates{0};
  std::atomic<std::uint64_t> framegraphSourceLocalReturnFrontierRollbackLiveSetMismatchMerged{0};
  std::atomic<std::uint64_t> framegraphSourceLocalReturnFrontierRollbackDuplicateCommandSources{0};
  std::atomic<std::uint64_t> framegraphSourceLocalReturnFrontierRollbackDuplicateCommandCandidates{0};
  std::atomic<std::uint64_t> framegraphSourceLocalReturnFrontierRollbackDuplicateCommandMerged{0};
  std::atomic<std::uint64_t> framegraphSourceLocalReturnFrontierRollbackMovedHeadUnprovedSources{0};
  std::atomic<std::uint64_t> framegraphSourceLocalReturnFrontierRollbackMovedHeadUnprovedCandidates{0};
  std::atomic<std::uint64_t> framegraphSourceLocalReturnFrontierRollbackMovedHeadUnprovedMerged{0};
  std::atomic<std::uint64_t> framegraphSourceLocalReturnFinalInvalidSources{0};
  std::atomic<std::uint64_t> framegraphSourceLocalReturnFinalInvalidCandidates{0};
  std::atomic<std::uint64_t> framegraphSourceLocalReturnFinalInvalidMerged{0};
  std::atomic<std::uint64_t> framegraphSourceLocalReturnFinalNaturalOrderSources{0};
  std::atomic<std::uint64_t> framegraphSourceLocalReturnFinalNaturalOrderCandidates{0};
  std::atomic<std::uint64_t> framegraphSourceLocalReturnFinalNaturalOrderMerged{0};
  std::atomic<std::uint64_t> framegraphSourceLocalReturnFinalReorderedActivatedSources{0};
  std::atomic<std::uint64_t> framegraphSourceLocalReturnFinalReorderedActivatedCandidates{0};
  std::atomic<std::uint64_t> framegraphSourceLocalReturnFinalReorderedActivatedMerged{0};
  std::atomic<std::uint64_t> framegraphActiveRenderSnapshotAbsent{0};
  std::atomic<std::uint64_t> framegraphActiveRenderSnapshotIncomplete{0};
  std::atomic<std::uint64_t> framegraphActiveRenderSeedApplyApplied{0};
  std::atomic<std::uint64_t> framegraphActiveRenderSeedApplyInvalid{0};
  std::atomic<std::uint64_t> framegraphActiveRenderSeedApplyIncomplete{0};
  std::atomic<std::uint64_t> framegraphActiveRenderSeedApplyOverflow{0};
  std::atomic<std::uint64_t> framegraphActiveRenderSeedAppliedButUnmerged{0};
  std::atomic<std::uint64_t> framegraphActiveRenderSeedPassCoalesceBlockedCycle{0};
  std::atomic<std::uint64_t> framegraphActiveRenderSeedPassCoalesceSecondNonDraw{0};
  std::atomic<std::uint64_t> framegraphActiveRenderSeedMovedHeadProved{0};
  std::atomic<std::uint64_t> framegraphActiveRenderSeedFallbackMovedHeadUnproved{0};
  std::atomic<std::uint64_t> framegraphActiveRenderSeedFallbackInvalidPlan{0};
  std::atomic<std::uint64_t> framegraphActiveRenderSeedFallbackLiveSetMismatch{0};
  std::atomic<std::uint64_t> framegraphActiveRenderSeedFallbackDuplicateCommand{0};
  std::atomic<std::uint64_t> framegraphActiveRenderSeedReplayActivated{0};
  // Sliding rings (R-BENCH-1.2): 64-sample percentile windows paired with the
  // *_max_ms counters above. Used to emit P50/P95/P99 in the shutdown report
  // so regression detection isn't outlier-driven by a single GC pause.
  PercentileRing submitDrawCpuRing;
  PercentileRing gpuCommandBufferTimeRing;
  PercentileRing renderEncoderGpuTimeRing;
  PercentileRing encodeChunkCpuRing;
  PercentileRing encodeDrawCpuRing;
  PercentileRing encodeDrawPipelineLookupCpuRing;
  PercentileRing shaderVariantKeyHashCpuRing;
  PercentileRing encodeDrawUniformBuildCpuRing;
  PercentileRing encodeDrawFvfDecodeCpuRing;
  PercentileRing encodeDrawStreamBindCpuRing;
  PercentileRing encodeDrawIssueCpuRing;
  PercentileRing d3d9SnapshotDrawSubmissionCpuRing;
  PercentileRing transientUploadCpuRing;
  PercentileRing d3d9BufferLockRing;
  PercentileRing d3d9BufferLockShadowAllocRing;
  PercentileRing d3d9BufferLockShadowCopyRing;
  PercentileRing commandBufferCreateCpuRing;
  PercentileRing commandBufferCommitCpuRing;
  PercentileRing submitPresentCpuRing;
  PercentileRing submitPresentAcquireCpuRing;
  PercentileRing submitPresentCommitCpuRing;
  PercentileRing submitPresentBoundaryCpuRing;
  PercentileRing prepareSlotForPublishCpuRing;
  PercentileRing prepareSlotResourceMarkCpuRing;
  PercentileRing unpublishedSlotPsoPrefetchCpuRing;
  PercentileRing chunkPublishSlotResidencyRing;
  PercentileRing chunkPublishSlotResidencyPresentRing;
  PercentileRing chunkPublishSlotResidencyNonPresentRing;
  PercentileRing chunkPublishPresentPrePresentOpportunityResidencyRing;
  PercentileRing encodeSlotPsoPrefetchCpuRing;
  PercentileRing encodeSlotPsoPrefetchStateCopyCpuRing;
  PercentileRing encodeSlotPsoPrefetchDepthLookupCpuRing;
  PercentileRing encodeSlotPsoPrefetchTileSelectCpuRing;
  PercentileRing encodeSlotPsoPrefetchTileBaseLookupCpuRing;
  PercentileRing encodeSlotPsoPrefetchTileDrawLookupCpuRing;
  PercentileRing encodeSlotPsoPrefetchArgbufSelectCpuRing;
  PercentileRing encodeSlotPsoPrefetchDrawLookupCpuRing;
  // V1 boundary B2 — paired with bridgeCommitLatency*Ns above.
  PercentileRing bridgeCommitLatencyRing;
  PercentileRing commitChunkImportCpuRing;
  PercentileRing commitChunkHandleCpuRing;
  PercentileRing commitChunkReplayCpuRing;
  PercentileRing commitChunkDrawBatchSubmitCpuRing;
  PercentileRing commitChunkRawEnqueueCpuRing;
  PercentileRing offloadReplayCpuRing;
  PercentileRing commitChunkDrawRunScanCpuRing;
  PercentileRing commitChunkDrawRunBuildCpuRing;
  PercentileRing commitChunkDrawRunSubmitCpuRing;
  PercentileRing commitChunkDrawRunFinalBindCpuRing;
  PercentileRing commitChunkQueueDrawSubmissionCpuRing;
  PercentileRing commitChunkQueueDrawSubmissionEmplaceCpuRing;
  PercentileRing commitChunkQueueDrawSubmissionSnapshotCpuRing;
  PercentileRing commitChunkIndexBindCpuRing;
  PercentileRing commitChunkReplayPendingFlushCpuRing;
  PercentileRing commitChunkReplayDrawRecordCpuRing;
  PercentileRing commitChunkReplayNonDrawRecordCpuRing;
  PercentileRing commitChunkReplayConstRecordCpuRing;
  PercentileRing commitChunkReplayApplyStateRecordCpuRing;
  PercentileRing commitChunkReplayClearRecordCpuRing;
  PercentileRing commitChunkReplayPresentRecordCpuRing;
  PercentileRing commitChunkReplaySurfaceRecordCpuRing;
  PercentileRing commitChunkReplayQueryRecordCpuRing;
  PercentileRing commitChunkReplayOtherRecordCpuRing;
  PercentileRing commitChunkConstUploadCpuRing;
  PercentileRing completionWaitCommitChunkReplayCpuRing;
  PercentileRing completionWaitStagePublishToEncodeDequeueRing;
  PercentileRing completionWaitStageEncodeDequeueToCommandBufferCommitRing;
  PercentileRing completionDequeueAgeRing;
  PercentileRing completionNoEnqueueWaitToCommitChunkEntryRing;
  PercentileRing completionNoEnqueueWaitToCommitChunkReplayStartRing;
  PercentileRing completionNoEnqueueWaitToCommitChunkReplayEndRing;
  PercentileRing completionNoEnqueueWaitToCommitPublishRing;
  PercentileRing completionNoEnqueueCommitChunkEntriesBeforePublishRing;
  PercentileRing completionNoEnqueueCommitChunkReplayStartsBeforePublishRing;
  PercentileRing completionNoEnqueueCommitChunkReplayEndsBeforePublishRing;
  PercentileRing completionNoEnqueueCommitChunkCompletedReplayCpuBeforePublishRing;
  PercentileRing completionNoEnqueueCommitChunkActiveReplayCpuBeforePublishRing;
  PercentileRing completionNoEnqueueCommitChunkInterReplayGapBeforePublishRing;
  PercentileRing completionNoEnqueueCommitPublishWaitBeforePublishRing;
  PercentileRing completionNoEnqueueCommitPublishOnBeforePublishCpuRing;
  PercentileRing completionNoEnqueueCommitChunkRecordsBeforePublishRing;
  PercentileRing completionNoEnqueueFirstPublishSlotCommandsRing;
  PercentileRing completionNoEnqueueFirstPublishSlotDrawItemsRing;
  PercentileRing completionNoEnqueueFirstPublishSlotPayloadBytesRing;
  PercentileRing completionNoEnqueueFirstPublishSlotPrePresentCommandsRing;
  PercentileRing completionNoEnqueueFirstPublishSlotPrePresentDrawItemsRing;
  PercentileRing completionNoEnqueueFirstPublishSlotPrePresentPayloadBytesRing;
  PercentileRing completionNoEnqueueWaitToEncodeDequeueRing;
  PercentileRing completionNoEnqueueWaitToCommandBufferCommitRing;
  PercentileRing completionNoEnqueueStageCommitEntryToPublishRing;
  PercentileRing completionNoEnqueueStagePublishToEncodeDequeueRing;
  PercentileRing completionNoEnqueueStageEncodeDequeueToCommandBufferCommitRing;
  PercentileRing completionNoEnqueueWaitToNextEnqueueRing;
  PercentileRing completionWaitRing;
  PercentileRing completionPresentWaitRing;
  PercentileRing completionDrawWaitRing;
  PercentileRing completionBlitWaitRing;
  PercentileRing completionPresentOnlyWaitRing;
  PercentileRing completionDrawPresentWaitRing;
  PercentileRing completionDrawStretchWaitRing;
  PercentileRing completionStretchWaitRing;
  PercentileRing completionBlitOnlyWaitRing;
  PercentileRing completionOtherWaitRing;
  PercentileRing syncWaitRing;
  PercentileRing queueWriterWaitRing;
  PercentileRing queueCommitWaitRing;
  PercentileRing queueSequenceWaitRing;
  PercentileRing cpuReadyTapeAdmissionWaitRing;
  PercentileRing mapBufferTotalRing;
  PercentileRing mapBufferMutexWaitRing;
  PercentileRing mapBufferWaitRing;
  PercentileRing presentBoundaryWaitRing;
  PercentileRing presentAcquireWaitRing;
  PercentileRing presentAsyncAcquireWaitRing;
  PercentileRing presentTokenWaitRing;
  PercentileRing presentPreAcquireWaitRing;
  PercentileRing presentSetPropsWaitRing;
};

Counters& counters() {
  static Counters value;
  return value;
}

bool enabledFlag() {
  static const bool value = [] {
    const char* env = std::getenv("DXMT_PERF_COUNTERS");
    return env && env[0] != '\0' && env[0] != '0';
  }();
  return value;
}

std::uint64_t periodicPresentInterval() {
  static const std::uint64_t value = [] {
    const char* env = std::getenv("DXMT_PERF_COUNTERS_PERIODIC_PRESENTS");
    if (!env || env[0] == '\0' || env[0] == '0') {
      return 0ull;
    }
    char* end = nullptr;
    const auto parsed = std::strtoull(env, &end, 10);
    return end != env ? parsed : 0ull;
  }();
  return value;
}

std::uint64_t load(const std::atomic<std::uint64_t>& value) {
  return value.load(std::memory_order_relaxed);
}

std::uint64_t triangleEstimate(std::uint32_t primitiveType, std::uint32_t primitiveCount) {
  switch (primitiveType) {
    case 0:  // PointList
    case 1:  // LineList
    case 2:  // LineStrip
      return 0;
    default:
      return primitiveCount;
  }
}

std::atomic<std::uint64_t>& pipelineHitCounter(Counters& c, PipelineKind kind) {
  switch (kind) {
    case PipelineKind::Draw:
      return c.pipelineHitDraw;
    case PipelineKind::Fill:
      return c.pipelineHitFill;
    case PipelineKind::Stretch:
      return c.pipelineHitStretch;
    case PipelineKind::Present:
      return c.pipelineHitDraw;
  }
  return c.pipelineHitDraw;
}

std::atomic<std::uint64_t>& pipelineMissCounter(Counters& c, PipelineKind kind) {
  switch (kind) {
    case PipelineKind::Draw:
      return c.pipelineMissDraw;
    case PipelineKind::Fill:
      return c.pipelineMissFill;
    case PipelineKind::Stretch:
      return c.pipelineMissStretch;
    case PipelineKind::Present:
      return c.pipelineMissDraw;
  }
  return c.pipelineMissDraw;
}

std::atomic<std::uint64_t>& pipelineBuildCounter(Counters& c, PipelineKind kind) {
  switch (kind) {
    case PipelineKind::Draw:
      return c.pipelineBuildDraw;
    case PipelineKind::Fill:
      return c.pipelineBuildFill;
    case PipelineKind::Stretch:
      return c.pipelineBuildStretch;
    case PipelineKind::Present:
      return c.pipelineBuildPresent;
  }
  return c.pipelineBuildDraw;
}

std::atomic<std::uint64_t>& completionDequeueStatusCounter(Counters& c,
                                                           std::uint64_t status) {
  switch (status) {
    case 0:
      return c.completionDequeueStatusNotEnqueued;
    case 1:
      return c.completionDequeueStatusEnqueued;
    case 2:
      return c.completionDequeueStatusCommitted;
    case 3:
      return c.completionDequeueStatusScheduled;
    case 4:
      return c.completionDequeueStatusCompleted;
    case 5:
      return c.completionDequeueStatusError;
  }
  return c.completionDequeueStatusUnknown;
}

void addCompletionWaitStatusBucket(Counters& c,
                                   std::uint64_t status,
                                   std::uint64_t nanoseconds) {
  std::atomic<std::uint64_t>* count = &c.completionWaitStatusUnknown;
  std::atomic<std::uint64_t>* totalNs = &c.completionWaitStatusUnknownNs;
  switch (status) {
    case 0:
      count = &c.completionWaitStatusNotEnqueued;
      totalNs = &c.completionWaitStatusNotEnqueuedNs;
      break;
    case 1:
      count = &c.completionWaitStatusEnqueued;
      totalNs = &c.completionWaitStatusEnqueuedNs;
      break;
    case 2:
      count = &c.completionWaitStatusCommitted;
      totalNs = &c.completionWaitStatusCommittedNs;
      break;
    case 3:
      count = &c.completionWaitStatusScheduled;
      totalNs = &c.completionWaitStatusScheduledNs;
      break;
    default:
      break;
  }
  count->fetch_add(1, std::memory_order_relaxed);
  totalNs->fetch_add(nanoseconds, std::memory_order_relaxed);
}

std::atomic<std::uint64_t>& splitReasonCounter(Counters& c, EncoderSplitReason reason) {
  switch (reason) {
    case EncoderSplitReason::Final:
      return c.renderSplitFinal;
    case EncoderSplitReason::RenderTargetChange:
      return c.renderSplitRenderTargetChange;
    case EncoderSplitReason::Hazard:
      return c.renderSplitHazard;
    case EncoderSplitReason::ClearBarrier:
      return c.renderSplitClearBarrier;
    case EncoderSplitReason::SurfaceCopy:
      return c.renderSplitSurfaceCopy;
    case EncoderSplitReason::StretchRect:
      return c.renderSplitStretchRect;
    case EncoderSplitReason::Readback:
      return c.renderSplitReadback;
    case EncoderSplitReason::ColorFill:
      return c.renderSplitColorFill;
    case EncoderSplitReason::Present:
      return c.renderSplitPresent;
    case EncoderSplitReason::PresentAcquire:
      return c.renderSplitPresentAcquire;
    case EncoderSplitReason::TileMidPassIneligible:
      return c.renderSplitTileMidPassIneligible;
    case EncoderSplitReason::OrderedControl:
      return c.renderSplitOrderedControl;
  }
  return c.renderSplitFinal;
}

const char* splitReasonName(EncoderSplitReason reason) {
  switch (reason) {
    case EncoderSplitReason::Final:
      return "final";
    case EncoderSplitReason::RenderTargetChange:
      return "rt_change";
    case EncoderSplitReason::Hazard:
      return "hazard";
    case EncoderSplitReason::ClearBarrier:
      return "clear";
    case EncoderSplitReason::SurfaceCopy:
      return "surface_copy";
    case EncoderSplitReason::StretchRect:
      return "stretch";
    case EncoderSplitReason::Readback:
      return "readback";
    case EncoderSplitReason::ColorFill:
      return "color_fill";
    case EncoderSplitReason::Present:
      return "present";
    case EncoderSplitReason::PresentAcquire:
      return "present_acquire";
    case EncoderSplitReason::TileMidPassIneligible:
      return "tile_midpass";
    case EncoderSplitReason::OrderedControl:
      return "ordered_control";
  }
  return "unknown";
}

// Data-driven counter table for the shutdown report. Each entry maps a stable
// counter key to either a Counters atomic field (UnsignedCount/Milliseconds/
// Hex64), a pair of fields (WidthByHeight), or a PercentileRing member with a
// percentile fraction (PercentileMs). Adding a new counter is now (1) add the
// Counters field + (2) one row here, in the position where the key should
// appear in the report stream. Order is load-bearing for the consumer: the
// runbook scripts parse the line in arrival order.
struct CounterEntry {
  enum class Kind : std::uint8_t {
    UnsignedCount,   // " key=%llu"
    Milliseconds,    // " key=%.3f" from a ns counter / 1e6
    Hex64,           // " key=0x%llx" for handle/hash values
    WidthByHeight,   // " key=%llux%llu" using both atomicField and field2
    PercentileMs,    // " key=%.3f" from PercentileRing::percentile(p) / 1e6
    // V1 boundary B2: the historical bridge_commit_latency counter is still
    // reported in raw nanoseconds, but it now represents the whole
    // commit_chunk call wall time, not a raw ABI-only bridge cost.
    PercentileNs,    // " key=%llu" from PercentileRing::percentile(p)
  };

  const char* key;
  Kind kind;
  // Source — one of these three is non-null based on kind:
  //   UnsignedCount/Milliseconds/Hex64: atomicField
  //   WidthByHeight:                    atomicField + field2
  //   PercentileMs:                     ringField + percentile
  std::atomic<std::uint64_t> Counters::* atomicField;
  std::atomic<std::uint64_t> Counters::* field2;
  PercentileRing Counters::* ringField;
  double percentile;
};

constexpr CounterEntry kCounterTable[] = {
    {"chunk_admit", CounterEntry::Kind::UnsignedCount, &Counters::chunkAdmit, nullptr, nullptr, 0.0},
    {"chunk_reject", CounterEntry::Kind::UnsignedCount, &Counters::chunkReject, nullptr, nullptr, 0.0},
    {"command_chunk_v2_chunks", CounterEntry::Kind::UnsignedCount, &Counters::commandChunkV2Chunks, nullptr, nullptr, 0.0},
    {"command_chunk_v2_records", CounterEntry::Kind::UnsignedCount, &Counters::commandChunkV2Records, nullptr, nullptr, 0.0},
    {"command_chunk_v2_bytes", CounterEntry::Kind::UnsignedCount, &Counters::commandChunkV2Bytes, nullptr, nullptr, 0.0},
    {"command_chunk_v2_rejects", CounterEntry::Kind::UnsignedCount, &Counters::commandChunkV2Rejects, nullptr, nullptr, 0.0},
    {"command_chunk_v2_registry_resolutions", CounterEntry::Kind::UnsignedCount, &Counters::commandChunkV2RegistryResolutions, nullptr, nullptr, 0.0},
    {"ring_arena_heap_fallback_count", CounterEntry::Kind::UnsignedCount, &Counters::ringArenaHeapFallbackCount, nullptr, nullptr, 0.0},
    {"ring_arena_heap_fallback_bytes", CounterEntry::Kind::UnsignedCount, &Counters::ringArenaHeapFallbackBytes, nullptr, nullptr, 0.0},
    {"ring_arena_heap_fallback_argbuf", CounterEntry::Kind::UnsignedCount, &Counters::ringArenaHeapFallbackCountArgbuf, nullptr, nullptr, 0.0},
    {"ring_arena_heap_fallback_lambda", CounterEntry::Kind::UnsignedCount, &Counters::ringArenaHeapFallbackCountLambda, nullptr, nullptr, 0.0},
    {"ring_arena_heap_fallback_staging", CounterEntry::Kind::UnsignedCount, &Counters::ringArenaHeapFallbackCountStaging, nullptr, nullptr, 0.0},
    {"ring_arena_heap_fallback_copytemp", CounterEntry::Kind::UnsignedCount, &Counters::ringArenaHeapFallbackCountCopyTemp, nullptr, nullptr, 0.0},
    {"submit_draw", CounterEntry::Kind::UnsignedCount, &Counters::submitDraw, nullptr, nullptr, 0.0},
    {"submit_clear", CounterEntry::Kind::UnsignedCount, &Counters::submitClear, nullptr, nullptr, 0.0},
    {"submit_stretch", CounterEntry::Kind::UnsignedCount, &Counters::submitStretch, nullptr, nullptr, 0.0},
    {"stretch_copy", CounterEntry::Kind::UnsignedCount, &Counters::stretchBlitCopy, nullptr, nullptr, 0.0},
    {"stretch_pass", CounterEntry::Kind::UnsignedCount, &Counters::stretchRenderPass, nullptr, nullptr, 0.0},
    {"stretch_full", CounterEntry::Kind::UnsignedCount, &Counters::stretchFullscreen, nullptr, nullptr, 0.0},
    {"submit_present", CounterEntry::Kind::UnsignedCount, &Counters::submitPresent, nullptr, nullptr, 0.0},
    {"submit_flush", CounterEntry::Kind::UnsignedCount, &Counters::submitFlush, nullptr, nullptr, 0.0},
    {"submit_present_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::submitPresentCpuNs, nullptr, nullptr, 0.0},
    {"submit_present_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::submitPresentCpuMaxNs, nullptr, nullptr, 0.0},
    {"submit_present_cpu_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::submitPresentCpuRing, 0.5},
    {"submit_present_cpu_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::submitPresentCpuRing, 0.95},
    {"submit_present_acquire_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::submitPresentAcquireCpuNs, nullptr, nullptr, 0.0},
    {"submit_present_acquire_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::submitPresentAcquireCpuMaxNs, nullptr, nullptr, 0.0},
    {"submit_present_acquire_cpu_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::submitPresentAcquireCpuRing, 0.5},
    {"submit_present_acquire_cpu_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::submitPresentAcquireCpuRing, 0.95},
    {"submit_present_commit_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::submitPresentCommitCpuNs, nullptr, nullptr, 0.0},
    {"submit_present_commit_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::submitPresentCommitCpuMaxNs, nullptr, nullptr, 0.0},
    {"submit_present_commit_cpu_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::submitPresentCommitCpuRing, 0.5},
    {"submit_present_commit_cpu_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::submitPresentCommitCpuRing, 0.95},
    {"submit_present_boundary_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::submitPresentBoundaryCpuNs, nullptr, nullptr, 0.0},
    {"submit_present_boundary_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::submitPresentBoundaryCpuMaxNs, nullptr, nullptr, 0.0},
    {"submit_present_boundary_cpu_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::submitPresentBoundaryCpuRing, 0.5},
    {"submit_present_boundary_cpu_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::submitPresentBoundaryCpuRing, 0.95},
    {"prepare_slot_publish_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::prepareSlotForPublishCpuNs, nullptr, nullptr, 0.0},
    {"prepare_slot_publish_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::prepareSlotForPublishCpuMaxNs, nullptr, nullptr, 0.0},
    {"prepare_slot_publish_cpu_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::prepareSlotForPublishCpuRing, 0.5},
    {"prepare_slot_publish_cpu_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::prepareSlotForPublishCpuRing, 0.95},
    {"prepare_slot_resource_mark_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::prepareSlotResourceMarkCpuNs, nullptr, nullptr, 0.0},
    {"prepare_slot_resource_mark_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::prepareSlotResourceMarkCpuMaxNs, nullptr, nullptr, 0.0},
    {"prepare_slot_resource_mark_cpu_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::prepareSlotResourceMarkCpuRing, 0.5},
    {"prepare_slot_resource_mark_cpu_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::prepareSlotResourceMarkCpuRing, 0.95},
    {"unpublished_slot_pso_prefetch_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::unpublishedSlotPsoPrefetchCpuNs, nullptr, nullptr, 0.0},
    {"unpublished_slot_pso_prefetch_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::unpublishedSlotPsoPrefetchCpuMaxNs, nullptr, nullptr, 0.0},
    {"unpublished_slot_pso_prefetch_cpu_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::unpublishedSlotPsoPrefetchCpuRing, 0.5},
    {"unpublished_slot_pso_prefetch_cpu_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::unpublishedSlotPsoPrefetchCpuRing, 0.95},
    {"chunk_publish_reason_unknown", CounterEntry::Kind::UnsignedCount, &Counters::chunkPublishReasonUnknown, nullptr, nullptr, 0.0},
    {"chunk_publish_reason_draw_limit", CounterEntry::Kind::UnsignedCount, &Counters::chunkPublishReasonDrawLimit, nullptr, nullptr, 0.0},
    {"chunk_publish_reason_payload_limit", CounterEntry::Kind::UnsignedCount, &Counters::chunkPublishReasonPayloadLimit, nullptr, nullptr, 0.0},
    {"chunk_publish_reason_present", CounterEntry::Kind::UnsignedCount, &Counters::chunkPublishReasonPresent, nullptr, nullptr, 0.0},
    {"chunk_publish_reason_present_acquire", CounterEntry::Kind::UnsignedCount, &Counters::chunkPublishReasonPresentAcquire, nullptr, nullptr, 0.0},
    {"chunk_publish_reason_flush", CounterEntry::Kind::UnsignedCount, &Counters::chunkPublishReasonFlush, nullptr, nullptr, 0.0},
    {"chunk_publish_reason_stretch_split", CounterEntry::Kind::UnsignedCount, &Counters::chunkPublishReasonStretchSplit, nullptr, nullptr, 0.0},
    {"chunk_publish_reason_map_wait", CounterEntry::Kind::UnsignedCount, &Counters::chunkPublishReasonMapWait, nullptr, nullptr, 0.0},
    {"chunk_publish_reason_present_split_before", CounterEntry::Kind::UnsignedCount, &Counters::chunkPublishReasonPresentSplitBefore, nullptr, nullptr, 0.0},
    {"chunk_publish_reason_semantic_boundary", CounterEntry::Kind::UnsignedCount, &Counters::chunkPublishReasonSemanticBoundary, nullptr, nullptr, 0.0},
    {"chunk_publish_commands_unknown", CounterEntry::Kind::UnsignedCount, &Counters::chunkPublishCommandsUnknown, nullptr, nullptr, 0.0},
    {"chunk_publish_commands_draw_limit", CounterEntry::Kind::UnsignedCount, &Counters::chunkPublishCommandsDrawLimit, nullptr, nullptr, 0.0},
    {"chunk_publish_commands_payload_limit", CounterEntry::Kind::UnsignedCount, &Counters::chunkPublishCommandsPayloadLimit, nullptr, nullptr, 0.0},
    {"chunk_publish_commands_present", CounterEntry::Kind::UnsignedCount, &Counters::chunkPublishCommandsPresent, nullptr, nullptr, 0.0},
    {"chunk_publish_commands_present_acquire", CounterEntry::Kind::UnsignedCount, &Counters::chunkPublishCommandsPresentAcquire, nullptr, nullptr, 0.0},
    {"chunk_publish_commands_flush", CounterEntry::Kind::UnsignedCount, &Counters::chunkPublishCommandsFlush, nullptr, nullptr, 0.0},
    {"chunk_publish_commands_stretch_split", CounterEntry::Kind::UnsignedCount, &Counters::chunkPublishCommandsStretchSplit, nullptr, nullptr, 0.0},
    {"chunk_publish_commands_map_wait", CounterEntry::Kind::UnsignedCount, &Counters::chunkPublishCommandsMapWait, nullptr, nullptr, 0.0},
    {"chunk_publish_commands_present_split_before", CounterEntry::Kind::UnsignedCount, &Counters::chunkPublishCommandsPresentSplitBefore, nullptr, nullptr, 0.0},
    {"chunk_publish_commands_semantic_boundary", CounterEntry::Kind::UnsignedCount, &Counters::chunkPublishCommandsSemanticBoundary, nullptr, nullptr, 0.0},
    {"open_cb_carrier_wait_start_published", CounterEntry::Kind::UnsignedCount, &Counters::openCbCarrierWaitStartPublished, nullptr, nullptr, 0.0},
    {"open_cb_carrier_active_wait_published", CounterEntry::Kind::UnsignedCount, &Counters::openCbCarrierActiveWaitPublished, nullptr, nullptr, 0.0},
    {"open_cb_carrier_producer_wait_published", CounterEntry::Kind::UnsignedCount, &Counters::openCbCarrierProducerWaitPublished, nullptr, nullptr, 0.0},
    {"open_cb_carrier_attachment_boundary_published", CounterEntry::Kind::UnsignedCount, &Counters::openCbCarrierAttachmentBoundaryPublished, nullptr, nullptr, 0.0},
    {"open_cb_carrier_pending_started", CounterEntry::Kind::UnsignedCount, &Counters::openCbCarrierPendingStarted, nullptr, nullptr, 0.0},
    {"open_cb_carrier_pending_started_in_wait", CounterEntry::Kind::UnsignedCount, &Counters::openCbCarrierPendingStartedInWait, nullptr, nullptr, 0.0},
    {"open_cb_carrier_head_appended", CounterEntry::Kind::UnsignedCount, &Counters::openCbCarrierHeadAppended, nullptr, nullptr, 0.0},
    {"open_cb_carrier_tail_submitted", CounterEntry::Kind::UnsignedCount, &Counters::openCbCarrierTailSubmitted, nullptr, nullptr, 0.0},
    {"open_cb_carrier_released_semantic_wait", CounterEntry::Kind::UnsignedCount, &Counters::openCbCarrierReleasedSemanticWait, nullptr, nullptr, 0.0},
    {"open_cb_carrier_released_producer_wait", CounterEntry::Kind::UnsignedCount, &Counters::openCbCarrierReleasedProducerWait, nullptr, nullptr, 0.0},
    {"open_cb_carrier_released_non_appendable", CounterEntry::Kind::UnsignedCount, &Counters::openCbCarrierReleasedNonAppendable, nullptr, nullptr, 0.0},
    {"open_cb_carrier_released_initializer_wait", CounterEntry::Kind::UnsignedCount, &Counters::openCbCarrierReleasedInitializerWait, nullptr, nullptr, 0.0},
    {"open_cb_carrier_released_drain", CounterEntry::Kind::UnsignedCount, &Counters::openCbCarrierReleasedDrain, nullptr, nullptr, 0.0},
    {"open_cb_carrier_released_fail_path", CounterEntry::Kind::UnsignedCount, &Counters::openCbCarrierReleasedFailPath, nullptr, nullptr, 0.0},
    {"cpu_ready_session_pending_started", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadySessionPendingStarted, nullptr, nullptr, 0.0},
    {"cpu_ready_session_head_appended", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadySessionHeadAppended, nullptr, nullptr, 0.0},
    {"cpu_ready_session_arena_head_appended", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadySessionArenaHeadAppended, nullptr, nullptr, 0.0},
    {"cpu_ready_session_tail_submitted", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadySessionTailSubmitted, nullptr, nullptr, 0.0},
    {"cpu_ready_retained_head_attempts", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyRetainedHeadAttempts, nullptr, nullptr, 0.0},
    {"cpu_ready_retained_head_held", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyRetainedHeadHeld, nullptr, nullptr, 0.0},
    {"cpu_ready_retained_head_live", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyRetainedHeadLive, nullptr, nullptr, 0.0},
    {"cpu_ready_retained_head_peak", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyRetainedHeadPeak, nullptr, nullptr, 0.0},
    {"cpu_ready_retained_head_successor_ready", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyRetainedHeadSuccessorReady, nullptr, nullptr, 0.0},
    {"cpu_ready_retained_head_fallback_release", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyRetainedHeadFallbackRelease, nullptr, nullptr, 0.0},
    {"cpu_ready_retained_head_fallback_producer_wait", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyRetainedHeadFallbackProducerWait, nullptr, nullptr, 0.0},
    {"cpu_ready_retained_head_fallback_initializer", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyRetainedHeadFallbackInitializer, nullptr, nullptr, 0.0},
    {"cpu_ready_retained_head_fallback_stop", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyRetainedHeadFallbackStop, nullptr, nullptr, 0.0},
    {"cpu_ready_retained_head_fallback_writer_gone", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyRetainedHeadFallbackWriterGone, nullptr, nullptr, 0.0},
    {"cpu_ready_retained_head_fallback_pressure", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyRetainedHeadFallbackPressure, nullptr, nullptr, 0.0},
    {"cpu_ready_retained_head_restore_failure", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyRetainedHeadRestoreFailure, nullptr, nullptr, 0.0},
    {"cpu_ready_retained_head_wait_ms", CounterEntry::Kind::Milliseconds, &Counters::cpuReadyRetainedHeadWaitNs, nullptr, nullptr, 0.0},
    {"completion_span_shadow_built", CounterEntry::Kind::UnsignedCount, &Counters::completionSpanShadowBuilt, nullptr, nullptr, 0.0},
    {"completion_span_shadow_validated", CounterEntry::Kind::UnsignedCount, &Counters::completionSpanShadowValidated, nullptr, nullptr, 0.0},
    {"completion_span_shadow_mismatch", CounterEntry::Kind::UnsignedCount, &Counters::completionSpanShadowMismatch, nullptr, nullptr, 0.0},
    {"completion_span_shadow_source_count", CounterEntry::Kind::UnsignedCount, &Counters::completionSpanShadowSourceCount, nullptr, nullptr, 0.0},
    {"post_encode_retire_attempts", CounterEntry::Kind::UnsignedCount, &Counters::postEncodeRetireAttempts, nullptr, nullptr, 0.0},
    {"post_encode_retire_success", CounterEntry::Kind::UnsignedCount, &Counters::postEncodeRetireSuccess, nullptr, nullptr, 0.0},
    {"post_encode_retire_success_arena", CounterEntry::Kind::UnsignedCount, &Counters::postEncodeRetireSuccessArena, nullptr, nullptr, 0.0},
    {"post_encode_retire_success_legacy", CounterEntry::Kind::UnsignedCount, &Counters::postEncodeRetireSuccessLegacy, nullptr, nullptr, 0.0},
    {"post_encode_retire_ineligible_none", CounterEntry::Kind::UnsignedCount, &Counters::postEncodeRetireIneligibleNone, nullptr, nullptr, 0.0},
    {"post_encode_retire_ineligible_pending_clear", CounterEntry::Kind::UnsignedCount, &Counters::postEncodeRetireIneligiblePendingClear, nullptr, nullptr, 0.0},
    {"post_encode_retire_ineligible_present", CounterEntry::Kind::UnsignedCount, &Counters::postEncodeRetireIneligiblePresent, nullptr, nullptr, 0.0},
    {"post_encode_retire_ineligible_readback", CounterEntry::Kind::UnsignedCount, &Counters::postEncodeRetireIneligibleReadback, nullptr, nullptr, 0.0},
    {"post_encode_retire_ineligible_update_surface", CounterEntry::Kind::UnsignedCount, &Counters::postEncodeRetireIneligibleUpdateSurface, nullptr, nullptr, 0.0},
    {"post_encode_retire_ineligible_ordered_control", CounterEntry::Kind::UnsignedCount, &Counters::postEncodeRetireIneligibleOrderedControl, nullptr, nullptr, 0.0},
    {"post_encode_retire_ineligible_payload_borrow", CounterEntry::Kind::UnsignedCount, &Counters::postEncodeRetireIneligiblePayloadBorrow, nullptr, nullptr, 0.0},
    {"post_encode_retire_ineligible_not_oldest", CounterEntry::Kind::UnsignedCount, &Counters::postEncodeRetireIneligibleNotOldest, nullptr, nullptr, 0.0},
    {"post_encode_retire_ineligible_receipt_capacity", CounterEntry::Kind::UnsignedCount, &Counters::postEncodeRetireIneligibleReceiptCapacity, nullptr, nullptr, 0.0},
    {"post_encode_retire_ineligible_invalid", CounterEntry::Kind::UnsignedCount, &Counters::postEncodeRetireIneligibleInvalid, nullptr, nullptr, 0.0},
    {"post_encode_receipt_failure_invalid", CounterEntry::Kind::UnsignedCount, &Counters::postEncodeReceiptFailureInvalid, nullptr, nullptr, 0.0},
    {"post_encode_receipt_failure_duplicate", CounterEntry::Kind::UnsignedCount, &Counters::postEncodeReceiptFailureDuplicate, nullptr, nullptr, 0.0},
    {"post_encode_receipt_failure_capacity", CounterEntry::Kind::UnsignedCount, &Counters::postEncodeReceiptFailureCapacity, nullptr, nullptr, 0.0},
    {"post_encode_receipt_failure_stale", CounterEntry::Kind::UnsignedCount, &Counters::postEncodeReceiptFailureStale, nullptr, nullptr, 0.0},
    {"post_encode_receipt_failure_wrong_state", CounterEntry::Kind::UnsignedCount, &Counters::postEncodeReceiptFailureWrongState, nullptr, nullptr, 0.0},
    {"post_encode_receipt_failure_other", CounterEntry::Kind::UnsignedCount, &Counters::postEncodeReceiptFailureOther, nullptr, nullptr, 0.0},
    {"post_encode_receipt_depth", CounterEntry::Kind::UnsignedCount, &Counters::postEncodeReceiptDepth, nullptr, nullptr, 0.0},
    {"post_encode_receipt_peak", CounterEntry::Kind::UnsignedCount, &Counters::postEncodeReceiptPeak, nullptr, nullptr, 0.0},
    {"post_encode_residency_sources_released", CounterEntry::Kind::UnsignedCount, &Counters::postEncodeResidencySourcesReleased, nullptr, nullptr, 0.0},
    {"post_encode_residency_pages_released", CounterEntry::Kind::UnsignedCount, &Counters::postEncodeResidencyPagesReleased, nullptr, nullptr, 0.0},
    {"post_encode_residency_bytes_released", CounterEntry::Kind::UnsignedCount, &Counters::postEncodeResidencyBytesReleased, nullptr, nullptr, 0.0},
    {"post_encode_work_cap_closes", CounterEntry::Kind::UnsignedCount, &Counters::postEncodeWorkCapCloses, nullptr, nullptr, 0.0},
    {"gpu_outstanding_completion_sources", CounterEntry::Kind::UnsignedCount, &Counters::gpuOutstandingCompletionSources, nullptr, nullptr, 0.0},
    {"gpu_outstanding_completion_sources_peak", CounterEntry::Kind::UnsignedCount, &Counters::gpuOutstandingCompletionSourcesPeak, nullptr, nullptr, 0.0},
    {"cpu_ready_session_released_producer_wait", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadySessionReleasedProducerWait, nullptr, nullptr, 0.0},
    {"cpu_ready_session_released_non_appendable", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadySessionReleasedNonAppendable, nullptr, nullptr, 0.0},
    {"cpu_ready_session_released_initializer_wait", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadySessionReleasedInitializerWait, nullptr, nullptr, 0.0},
    {"cpu_ready_session_released_drain", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadySessionReleasedDrain, nullptr, nullptr, 0.0},
    {"cpu_ready_session_released_fail_path", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadySessionReleasedFailPath, nullptr, nullptr, 0.0},
    {"cpu_ready_session_lease_current", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadySessionLeaseCurrent, nullptr, nullptr, 0.0},
    {"cpu_ready_session_lease_peak", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadySessionLeasePeak, nullptr, nullptr, 0.0},
    {"cpu_ready_session_lease_acquisitions", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadySessionLeaseAcquisitions, nullptr, nullptr, 0.0},
    {"cpu_ready_session_lease_denials", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadySessionLeaseDenials, nullptr, nullptr, 0.0},
    {"cpu_ready_session_lease_reserved_sources", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadySessionLeaseReservedSources, nullptr, nullptr, 0.0},
    {"cpu_ready_session_lease_reserved_pages", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadySessionLeaseReservedPages, nullptr, nullptr, 0.0},
    {"cpu_ready_session_lease_reserved_bytes", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadySessionLeaseReservedBytes, nullptr, nullptr, 0.0},
    {"cpu_ready_session_lease_reserved_draws", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadySessionLeaseReservedDraws, nullptr, nullptr, 0.0},
    {"cpu_ready_session_lease_used_sources", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadySessionLeaseUsedSources, nullptr, nullptr, 0.0},
    {"cpu_ready_session_lease_used_pages", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadySessionLeaseUsedPages, nullptr, nullptr, 0.0},
    {"cpu_ready_session_lease_used_bytes", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadySessionLeaseUsedBytes, nullptr, nullptr, 0.0},
    {"cpu_ready_session_lease_used_draws", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadySessionLeaseUsedDraws, nullptr, nullptr, 0.0},
    {"cpu_ready_session_lease_slack_sources", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadySessionLeaseSlackSources, nullptr, nullptr, 0.0},
    {"cpu_ready_session_lease_slack_pages", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadySessionLeaseSlackPages, nullptr, nullptr, 0.0},
    {"cpu_ready_session_lease_slack_bytes", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadySessionLeaseSlackBytes, nullptr, nullptr, 0.0},
    {"cpu_ready_session_lease_slack_draws", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadySessionLeaseSlackDraws, nullptr, nullptr, 0.0},
    {"cpu_ready_session_successor_headroom_min_pages", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadySessionSuccessorHeadroomMinPages, nullptr, nullptr, 0.0},
    {"cpu_ready_session_cap_sources", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadySessionCapSources, nullptr, nullptr, 0.0},
    {"cpu_ready_session_cap_pages", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadySessionCapPages, nullptr, nullptr, 0.0},
    {"cpu_ready_session_cap_bytes", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadySessionCapBytes, nullptr, nullptr, 0.0},
    {"cpu_ready_session_cap_draws", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadySessionCapDraws, nullptr, nullptr, 0.0},
    {"cpu_ready_session_cap_command_buffers", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadySessionCapCommandBuffers, nullptr, nullptr, 0.0},
    {"cpu_ready_session_cap_requirement_sources_only", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadySessionCapRequirementSourcesOnly, nullptr, nullptr, 0.0},
    {"cpu_ready_session_cap_requirement_pages_only", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadySessionCapRequirementPagesOnly, nullptr, nullptr, 0.0},
    {"cpu_ready_session_cap_requirement_sources_and_pages", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadySessionCapRequirementSourcesAndPages, nullptr, nullptr, 0.0},
    {"cpu_ready_session_cap_predecessor_sources_peak", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadySessionCapPredecessorSourcesPeak, nullptr, nullptr, 0.0},
    {"cpu_ready_session_cap_predecessor_pages_peak", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadySessionCapPredecessorPagesPeak, nullptr, nullptr, 0.0},
    {"cpu_ready_session_cap_candidate_payload_pages_peak", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadySessionCapCandidatePayloadPagesPeak, nullptr, nullptr, 0.0},
    {"cpu_ready_session_cap_candidate_wrap_padding_pages_peak", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadySessionCapCandidateWrapPaddingPagesPeak, nullptr, nullptr, 0.0},
    {"cpu_ready_session_cap_candidate_required_pages_peak", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadySessionCapCandidateRequiredPagesPeak, nullptr, nullptr, 0.0},
    {"cpu_ready_session_cap_required_total_sources_peak", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadySessionCapRequiredTotalSourcesPeak, nullptr, nullptr, 0.0},
    {"cpu_ready_session_cap_required_total_pages_peak", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadySessionCapRequiredTotalPagesPeak, nullptr, nullptr, 0.0},
    {"cpu_ready_session_isolated", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadySessionIsolated, nullptr, nullptr, 0.0},
    {"cpu_ready_session_isolated_present", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadySessionIsolatedPresent, nullptr, nullptr, 0.0},
    {"cpu_ready_session_isolated_capacity_bytes", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadySessionIsolatedCapacityBytes, nullptr, nullptr, 0.0},
    {"cpu_ready_session_isolated_other", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadySessionIsolatedOther, nullptr, nullptr, 0.0},
    {"cpu_ready_session_legacy_rollback", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadySessionLegacyRollback, nullptr, nullptr, 0.0},
    {"cpu_ready_session_invalid_disposition", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadySessionInvalidDisposition, nullptr, nullptr, 0.0},
    {"cpu_ready_multi_source_windows_attempted", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyMultiSourceWindowsAttempted, nullptr, nullptr, 0.0},
    {"cpu_ready_multi_source_windows_planned", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyMultiSourceWindowsPlanned, nullptr, nullptr, 0.0},
    {"cpu_ready_multi_source_window_sources", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyMultiSourceWindowSources, nullptr, nullptr, 0.0},
    {"cpu_ready_multi_source_window_commands", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyMultiSourceWindowCommands, nullptr, nullptr, 0.0},
    {"cpu_ready_multi_source_window_runs", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyMultiSourceWindowRuns, nullptr, nullptr, 0.0},
    {"cpu_ready_multi_source_fallback_eligibility", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyMultiSourceFallbackEligibility, nullptr, nullptr, 0.0},
    {"cpu_ready_multi_source_fallback_natural_plan", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyMultiSourceFallbackNaturalPlan, nullptr, nullptr, 0.0},
    {"cpu_ready_multi_source_fallback_invalid_plan", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyMultiSourceFallbackInvalidPlan, nullptr, nullptr, 0.0},
    {"cpu_ready_multi_source_fallback_repeated_source", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyMultiSourceFallbackRepeatedSource, nullptr, nullptr, 0.0},
    {"cpu_ready_multi_source_fallback_resolved_source", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyMultiSourceFallbackResolvedSource, nullptr, nullptr, 0.0},
    {"cpu_ready_multi_source_fallback_completion_source", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyMultiSourceFallbackCompletionSource, nullptr, nullptr, 0.0},
    {"cpu_ready_multi_source_fallback_admission", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyMultiSourceFallbackAdmission, nullptr, nullptr, 0.0},
    {"cpu_ready_multi_source_fallback_fragment_range", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyMultiSourceFallbackFragmentRange, nullptr, nullptr, 0.0},
    {"cpu_ready_multi_source_fallback_carrier", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyMultiSourceFallbackCarrier, nullptr, nullptr, 0.0},
    {"cpu_ready_multi_source_natural_fallback_windows_started", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyMultiSourceNaturalFallbackWindowsStarted, nullptr, nullptr, 0.0},
    {"cpu_ready_multi_source_natural_fallback_windows_completed", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyMultiSourceNaturalFallbackWindowsCompleted, nullptr, nullptr, 0.0},
    {"cpu_ready_multi_source_natural_fallback_sources", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyMultiSourceNaturalFallbackSources, nullptr, nullptr, 0.0},
    {"cpu_ready_multi_source_permutation_fallback_windows_started", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyMultiSourcePermutationFallbackWindowsStarted, nullptr, nullptr, 0.0},
    {"cpu_ready_multi_source_permutation_fallback_windows_completed", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyMultiSourcePermutationFallbackWindowsCompleted, nullptr, nullptr, 0.0},
    {"cpu_ready_multi_source_permutation_fallback_sources", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyMultiSourcePermutationFallbackSources, nullptr, nullptr, 0.0},
    {"cpu_ready_multi_source_eligibility_active_incomplete", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyMultiSourceEligibilityActiveIncomplete, nullptr, nullptr, 0.0},
    {"cpu_ready_multi_source_eligibility_present", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyMultiSourceEligibilityPresent, nullptr, nullptr, 0.0},
    {"cpu_ready_multi_source_eligibility_nonconsecutive_identity", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyMultiSourceEligibilityNonConsecutiveIdentity, nullptr, nullptr, 0.0},
    {"cpu_ready_multi_source_eligibility_other_boundary", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyMultiSourceEligibilityOtherBoundary, nullptr, nullptr, 0.0},
    {"cpu_ready_multi_source_planner_invalid_input", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyMultiSourcePlannerInvalidInput, nullptr, nullptr, 0.0},
    {"cpu_ready_multi_source_planner_seed_rejected", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyMultiSourcePlannerSeedRejected, nullptr, nullptr, 0.0},
    {"cpu_ready_multi_source_planner_no_active_target_match", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyMultiSourcePlannerNoActiveTargetMatch, nullptr, nullptr, 0.0},
    {"cpu_ready_multi_source_planner_no_merge", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyMultiSourcePlannerNoMerge, nullptr, nullptr, 0.0},
    {"cpu_ready_multi_source_planner_natural_after_merge", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyMultiSourcePlannerNaturalAfterMerge, nullptr, nullptr, 0.0},
    {"cpu_ready_multi_source_planner_permutation_rejected", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyMultiSourcePlannerPermutationRejected, nullptr, nullptr, 0.0},
    {"cpu_ready_multi_source_planner_moved_head_unproved", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyMultiSourcePlannerMovedHeadUnproved, nullptr, nullptr, 0.0},
    {"cpu_ready_multi_source_planner_planned", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyMultiSourcePlannerPlanned, nullptr, nullptr, 0.0},
    {"cpu_ready_multi_source_planner_merge_seed", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyMultiSourcePlannerMergeSeed, nullptr, nullptr, 0.0},
    {"cpu_ready_multi_source_planner_merge_nonseed_only", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyMultiSourcePlannerMergeNonSeedOnly, nullptr, nullptr, 0.0},
    {"cpu_ready_multi_source_planner_seed_natural_match_distance_1", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyMultiSourcePlannerSeedNaturalMatchDistance1, nullptr, nullptr, 0.0},
    {"cpu_ready_multi_source_planner_seed_natural_match_distance_gt1", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyMultiSourcePlannerSeedNaturalMatchDistanceGt1, nullptr, nullptr, 0.0},
    {"cpu_ready_multi_source_planner_seed_natural_match_distance_missing", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyMultiSourcePlannerSeedNaturalMatchDistanceMissing, nullptr, nullptr, 0.0},
    {"cpu_ready_multi_source_planner_seed_natural_merge_operations", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyMultiSourcePlannerSeedNaturalMergeOperations, nullptr, nullptr, 0.0},
    {"cpu_ready_multi_source_planner_seed_natural_merge_distance_total", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyMultiSourcePlannerSeedNaturalMergeDistanceTotal, nullptr, nullptr, 0.0},
    {"cpu_ready_multi_source_planner_seed_natural_merge_distance_max", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyMultiSourcePlannerSeedNaturalMergeDistanceMax, nullptr, nullptr, 0.0},
    {"cpu_ready_multi_source_planner_seed_natural_command_before", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyMultiSourcePlannerSeedNaturalCommandBefore, nullptr, nullptr, 0.0},
    {"cpu_ready_multi_source_planner_seed_natural_command_after", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyMultiSourcePlannerSeedNaturalCommandAfter, nullptr, nullptr, 0.0},
    {"cpu_ready_multi_source_planner_seed_natural_empty_intervening", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyMultiSourcePlannerSeedNaturalEmptyIntervening, nullptr, nullptr, 0.0},
    {"cpu_ready_multi_source_planner_seed_natural_shape_adjacent", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyMultiSourcePlannerSeedNaturalShapeAdjacent, nullptr, nullptr, 0.0},
    {"cpu_ready_multi_source_planner_seed_natural_shape_dependency_kept", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyMultiSourcePlannerSeedNaturalShapeDependencyKept, nullptr, nullptr, 0.0},
    {"cpu_ready_multi_source_planner_seed_natural_shape_commandless", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyMultiSourcePlannerSeedNaturalShapeCommandless, nullptr, nullptr, 0.0},
    {"cpu_ready_multi_source_planner_seed_natural_shape_multi_merge", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyMultiSourcePlannerSeedNaturalShapeMultiMerge, nullptr, nullptr, 0.0},
    {"cpu_ready_multi_source_planner_seed_natural_shape_missing", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyMultiSourcePlannerSeedNaturalShapeMissing, nullptr, nullptr, 0.0},
    {"cpu_ready_multi_source_planner_seed_second_non_draw", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyMultiSourcePlannerSeedSecondNonDraw, nullptr, nullptr, 0.0},
    {"cpu_ready_multi_source_planner_seed_blocked_cycle", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyMultiSourcePlannerSeedBlockedCycle, nullptr, nullptr, 0.0},
    {"cpu_ready_multi_source_fatal_encode_null", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyMultiSourceFatalEncodeNull, nullptr, nullptr, 0.0},
    {"cpu_ready_multi_source_fatal_carrier_fold", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyMultiSourceFatalCarrierFold, nullptr, nullptr, 0.0},
    {"cpu_ready_multi_source_completion_sources", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyMultiSourceCompletionSources, nullptr, nullptr, 0.0},
    {"cpu_ready_multi_source_completion_fifo_failures", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyMultiSourceCompletionFifoFailures, nullptr, nullptr, 0.0},
    {"chunk_publish_slot_residency_samples", CounterEntry::Kind::UnsignedCount, &Counters::chunkPublishSlotResidencySamples, nullptr, nullptr, 0.0},
    {"chunk_publish_slot_residency_ms", CounterEntry::Kind::Milliseconds, &Counters::chunkPublishSlotResidencyNs, nullptr, nullptr, 0.0},
    {"chunk_publish_slot_residency_max_ms", CounterEntry::Kind::Milliseconds, &Counters::chunkPublishSlotResidencyMaxNs, nullptr, nullptr, 0.0},
    {"chunk_publish_slot_residency_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::chunkPublishSlotResidencyRing, 0.5},
    {"chunk_publish_slot_residency_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::chunkPublishSlotResidencyRing, 0.95},
    {"chunk_publish_slot_residency_present_samples", CounterEntry::Kind::UnsignedCount, &Counters::chunkPublishSlotResidencyPresentSamples, nullptr, nullptr, 0.0},
    {"chunk_publish_slot_residency_present_ms", CounterEntry::Kind::Milliseconds, &Counters::chunkPublishSlotResidencyPresentNs, nullptr, nullptr, 0.0},
    {"chunk_publish_slot_residency_present_max_ms", CounterEntry::Kind::Milliseconds, &Counters::chunkPublishSlotResidencyPresentMaxNs, nullptr, nullptr, 0.0},
    {"chunk_publish_slot_residency_present_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::chunkPublishSlotResidencyPresentRing, 0.5},
    {"chunk_publish_slot_residency_present_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::chunkPublishSlotResidencyPresentRing, 0.95},
    {"chunk_publish_slot_residency_nonpresent_samples", CounterEntry::Kind::UnsignedCount, &Counters::chunkPublishSlotResidencyNonPresentSamples, nullptr, nullptr, 0.0},
    {"chunk_publish_slot_residency_nonpresent_ms", CounterEntry::Kind::Milliseconds, &Counters::chunkPublishSlotResidencyNonPresentNs, nullptr, nullptr, 0.0},
    {"chunk_publish_slot_residency_nonpresent_max_ms", CounterEntry::Kind::Milliseconds, &Counters::chunkPublishSlotResidencyNonPresentMaxNs, nullptr, nullptr, 0.0},
    {"chunk_publish_slot_residency_nonpresent_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::chunkPublishSlotResidencyNonPresentRing, 0.5},
    {"chunk_publish_slot_residency_nonpresent_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::chunkPublishSlotResidencyNonPresentRing, 0.95},
    {"chunk_publish_present_pre_present_opportunity_slots", CounterEntry::Kind::UnsignedCount, &Counters::chunkPublishPresentPrePresentOpportunitySlots, nullptr, nullptr, 0.0},
    {"chunk_publish_present_pre_present_opportunity_tail_slots", CounterEntry::Kind::UnsignedCount, &Counters::chunkPublishPresentPrePresentOpportunityTailSlots, nullptr, nullptr, 0.0},
    {"chunk_publish_present_pre_present_opportunity_nontail_slots", CounterEntry::Kind::UnsignedCount, &Counters::chunkPublishPresentPrePresentOpportunityNonTailSlots, nullptr, nullptr, 0.0},
    {"chunk_publish_present_pre_present_opportunity_commands", CounterEntry::Kind::UnsignedCount, &Counters::chunkPublishPresentPrePresentOpportunityCommands, nullptr, nullptr, 0.0},
    {"chunk_publish_present_pre_present_opportunity_draw_runs", CounterEntry::Kind::UnsignedCount, &Counters::chunkPublishPresentPrePresentOpportunityDrawRuns, nullptr, nullptr, 0.0},
    {"chunk_publish_present_pre_present_opportunity_draw_items", CounterEntry::Kind::UnsignedCount, &Counters::chunkPublishPresentPrePresentOpportunityDrawItems, nullptr, nullptr, 0.0},
    {"chunk_publish_present_pre_present_opportunity_non_draw_commands", CounterEntry::Kind::UnsignedCount, &Counters::chunkPublishPresentPrePresentOpportunityNonDrawCommands, nullptr, nullptr, 0.0},
    {"chunk_publish_present_pre_present_opportunity_payload_bytes", CounterEntry::Kind::UnsignedCount, &Counters::chunkPublishPresentPrePresentOpportunityPayloadBytes, nullptr, nullptr, 0.0},
    {"chunk_publish_present_pre_present_opportunity_residency_ms", CounterEntry::Kind::Milliseconds, &Counters::chunkPublishPresentPrePresentOpportunityResidencyNs, nullptr, nullptr, 0.0},
    {"chunk_publish_present_pre_present_opportunity_residency_max_ms", CounterEntry::Kind::Milliseconds, &Counters::chunkPublishPresentPrePresentOpportunityResidencyMaxNs, nullptr, nullptr, 0.0},
    {"chunk_publish_present_pre_present_opportunity_residency_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::chunkPublishPresentPrePresentOpportunityResidencyRing, 0.5},
    {"chunk_publish_present_pre_present_opportunity_residency_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::chunkPublishPresentPrePresentOpportunityResidencyRing, 0.95},
    {"chunk_publish_present_pre_present_opportunity_tail_empty", CounterEntry::Kind::UnsignedCount, &Counters::chunkPublishPresentPrePresentOpportunityTailEmpty, nullptr, nullptr, 0.0},
    {"chunk_publish_present_pre_present_opportunity_tail_draw_run", CounterEntry::Kind::UnsignedCount, &Counters::chunkPublishPresentPrePresentOpportunityTailDrawRun, nullptr, nullptr, 0.0},
    {"chunk_publish_present_pre_present_opportunity_tail_clear", CounterEntry::Kind::UnsignedCount, &Counters::chunkPublishPresentPrePresentOpportunityTailClear, nullptr, nullptr, 0.0},
    {"chunk_publish_present_pre_present_opportunity_tail_surface_copy", CounterEntry::Kind::UnsignedCount, &Counters::chunkPublishPresentPrePresentOpportunityTailSurfaceCopy, nullptr, nullptr, 0.0},
    {"chunk_publish_present_pre_present_opportunity_tail_stretch_rect", CounterEntry::Kind::UnsignedCount, &Counters::chunkPublishPresentPrePresentOpportunityTailStretchRect, nullptr, nullptr, 0.0},
    {"chunk_publish_present_pre_present_opportunity_tail_readback", CounterEntry::Kind::UnsignedCount, &Counters::chunkPublishPresentPrePresentOpportunityTailReadback, nullptr, nullptr, 0.0},
    {"chunk_publish_present_pre_present_opportunity_tail_color_fill", CounterEntry::Kind::UnsignedCount, &Counters::chunkPublishPresentPrePresentOpportunityTailColorFill, nullptr, nullptr, 0.0},
    {"chunk_publish_present_pre_present_opportunity_tail_depth_resolve", CounterEntry::Kind::UnsignedCount, &Counters::chunkPublishPresentPrePresentOpportunityTailDepthResolve, nullptr, nullptr, 0.0},
    {"chunk_publish_present_pre_present_opportunity_tail_present", CounterEntry::Kind::UnsignedCount, &Counters::chunkPublishPresentPrePresentOpportunityTailPresent, nullptr, nullptr, 0.0},
    {"chunk_publish_present_pre_present_opportunity_draw_only", CounterEntry::Kind::UnsignedCount, &Counters::chunkPublishPresentPrePresentOpportunityDrawOnly, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeSlotPsoPrefetchCpuNs, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeSlotPsoPrefetchCpuMaxNs, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_cpu_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::encodeSlotPsoPrefetchCpuRing, 0.5},
    {"encode_slot_pso_prefetch_cpu_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::encodeSlotPsoPrefetchCpuRing, 0.95},
    {"encode_slot_pso_prefetch_commands", CounterEntry::Kind::UnsignedCount, &Counters::encodeSlotPsoPrefetchCommands, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_candidates", CounterEntry::Kind::UnsignedCount, &Counters::encodeSlotPsoPrefetchCandidates, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_tile_candidates", CounterEntry::Kind::UnsignedCount, &Counters::encodeSlotPsoPrefetchTileCandidates, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_argbuf_stage2_candidates", CounterEntry::Kind::UnsignedCount, &Counters::encodeSlotPsoPrefetchArgbufStage2Candidates, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_argbuf_resource_array_candidates", CounterEntry::Kind::UnsignedCount, &Counters::encodeSlotPsoPrefetchArgbufResourceArrayCandidates, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_state_copy_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeSlotPsoPrefetchStateCopyCpuNs, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_state_copy_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeSlotPsoPrefetchStateCopyCpuMaxNs, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_state_copy_cpu_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::encodeSlotPsoPrefetchStateCopyCpuRing, 0.5},
    {"encode_slot_pso_prefetch_state_copy_cpu_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::encodeSlotPsoPrefetchStateCopyCpuRing, 0.95},
    {"encode_slot_pso_prefetch_depth_lookup_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeSlotPsoPrefetchDepthLookupCpuNs, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_depth_lookup_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeSlotPsoPrefetchDepthLookupCpuMaxNs, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_depth_lookup_cpu_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::encodeSlotPsoPrefetchDepthLookupCpuRing, 0.5},
    {"encode_slot_pso_prefetch_depth_lookup_cpu_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::encodeSlotPsoPrefetchDepthLookupCpuRing, 0.95},
    {"encode_slot_pso_prefetch_tile_select_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeSlotPsoPrefetchTileSelectCpuNs, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_tile_select_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeSlotPsoPrefetchTileSelectCpuMaxNs, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_tile_select_cpu_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::encodeSlotPsoPrefetchTileSelectCpuRing, 0.5},
    {"encode_slot_pso_prefetch_tile_select_cpu_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::encodeSlotPsoPrefetchTileSelectCpuRing, 0.95},
    {"encode_slot_pso_prefetch_tile_base_lookup_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeSlotPsoPrefetchTileBaseLookupCpuNs, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_tile_base_lookup_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeSlotPsoPrefetchTileBaseLookupCpuMaxNs, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_tile_base_lookup_cpu_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::encodeSlotPsoPrefetchTileBaseLookupCpuRing, 0.5},
    {"encode_slot_pso_prefetch_tile_base_lookup_cpu_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::encodeSlotPsoPrefetchTileBaseLookupCpuRing, 0.95},
    {"encode_slot_pso_prefetch_tile_draw_lookup_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeSlotPsoPrefetchTileDrawLookupCpuNs, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_tile_draw_lookup_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeSlotPsoPrefetchTileDrawLookupCpuMaxNs, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_tile_draw_lookup_cpu_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::encodeSlotPsoPrefetchTileDrawLookupCpuRing, 0.5},
    {"encode_slot_pso_prefetch_tile_draw_lookup_cpu_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::encodeSlotPsoPrefetchTileDrawLookupCpuRing, 0.95},
    {"encode_slot_pso_prefetch_argbuf_select_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeSlotPsoPrefetchArgbufSelectCpuNs, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_argbuf_select_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeSlotPsoPrefetchArgbufSelectCpuMaxNs, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_argbuf_select_cpu_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::encodeSlotPsoPrefetchArgbufSelectCpuRing, 0.5},
    {"encode_slot_pso_prefetch_argbuf_select_cpu_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::encodeSlotPsoPrefetchArgbufSelectCpuRing, 0.95},
    {"encode_slot_pso_prefetch_draw_key_resolve_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeSlotPsoPrefetchDrawKeyResolveCpuNs, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_draw_resolve_format_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeSlotPsoPrefetchDrawResolveFormatCpuNs, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_draw_resolve_variant_key_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeSlotPsoPrefetchDrawResolveVariantKeyCpuNs, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_draw_resolve_shader_context_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeSlotPsoPrefetchDrawResolveShaderContextCpuNs, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_draw_resolve_x8_alpha_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeSlotPsoPrefetchDrawResolveX8AlphaCpuNs, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_draw_resolve_vsout_layout_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeSlotPsoPrefetchDrawResolveVsoutLayoutCpuNs, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_draw_resolve_fragmentless_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeSlotPsoPrefetchDrawResolveFragmentlessCpuNs, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_draw_lookup_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeSlotPsoPrefetchDrawLookupCpuNs, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_draw_lookup_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeSlotPsoPrefetchDrawLookupCpuMaxNs, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_draw_lookup_cpu_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::encodeSlotPsoPrefetchDrawLookupCpuRing, 0.5},
    {"encode_slot_pso_prefetch_draw_lookup_cpu_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::encodeSlotPsoPrefetchDrawLookupCpuRing, 0.95},
    {"encode_slot_pso_prefetch_draw_semantic_key_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeSlotPsoPrefetchDrawSemanticKeyCpuNs, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_draw_semantic_probe_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeSlotPsoPrefetchDrawSemanticProbeCpuNs, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_draw_semantic_store_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeSlotPsoPrefetchDrawSemanticStoreCpuNs, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_draw_semantic_memo_hits", CounterEntry::Kind::UnsignedCount, &Counters::encodeSlotPsoPrefetchDrawSemanticMemoHits, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_draw_semantic_memo_misses", CounterEntry::Kind::UnsignedCount, &Counters::encodeSlotPsoPrefetchDrawSemanticMemoMisses, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_draw_semantic_memo_overflow", CounterEntry::Kind::UnsignedCount, &Counters::encodeSlotPsoPrefetchDrawSemanticMemoOverflow, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_draw_semantic_miss_probe_key_hits", CounterEntry::Kind::UnsignedCount, &Counters::encodeSlotPsoPrefetchDrawSemanticMissProbeKeyHits, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_draw_semantic_miss_probe_key_same_semantic", CounterEntry::Kind::UnsignedCount, &Counters::encodeSlotPsoPrefetchDrawSemanticMissProbeKeySameSemantic, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_draw_semantic_miss_probe_key_diff_argbuf_selector", CounterEntry::Kind::UnsignedCount, &Counters::encodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffArgbufSelector, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_draw_semantic_miss_probe_key_diff_vertex_decl", CounterEntry::Kind::UnsignedCount, &Counters::encodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffVertexDecl, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_draw_semantic_miss_probe_key_diff_shader", CounterEntry::Kind::UnsignedCount, &Counters::encodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffShader, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_draw_semantic_miss_probe_key_diff_render_state", CounterEntry::Kind::UnsignedCount, &Counters::encodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffRenderState, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_draw_semantic_miss_probe_key_diff_texture_handles", CounterEntry::Kind::UnsignedCount, &Counters::encodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffTextureHandles, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_draw_semantic_miss_probe_key_diff_texture_lod", CounterEntry::Kind::UnsignedCount, &Counters::encodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffTextureLod, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_draw_semantic_miss_probe_key_diff_texture_stage", CounterEntry::Kind::UnsignedCount, &Counters::encodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffTextureStage, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_draw_semantic_miss_probe_key_diff_sampler", CounterEntry::Kind::UnsignedCount, &Counters::encodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffSampler, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_draw_semantic_miss_probe_key_diff_attachment", CounterEntry::Kind::UnsignedCount, &Counters::encodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffAttachment, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_draw_semantic_miss_probe_key_diff_clip_plane", CounterEntry::Kind::UnsignedCount, &Counters::encodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffClipPlane, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_draw_semantic_miss_probe_key_diff_constant_usage", CounterEntry::Kind::UnsignedCount, &Counters::encodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffConstantUsage, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_draw_semantic_miss_probe_key_diff_single_field", CounterEntry::Kind::UnsignedCount, &Counters::encodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffSingleField, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_draw_semantic_miss_probe_key_diff_multi_field", CounterEntry::Kind::UnsignedCount, &Counters::encodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffMultiField, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_draw_semantic_miss_probe_key_diff_texture_handles_only", CounterEntry::Kind::UnsignedCount, &Counters::encodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffTextureHandlesOnly, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_draw_semantic_miss_probe_key_diff_texture_handles_with_others", CounterEntry::Kind::UnsignedCount, &Counters::encodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffTextureHandlesWithOthers, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_draw_semantic_miss_probe_key_diff_hash_only", CounterEntry::Kind::UnsignedCount, &Counters::encodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffHashOnly, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_draw_semantic_miss_probe_key_diff_unknown", CounterEntry::Kind::UnsignedCount, &Counters::encodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffUnknown, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_draw_resource_shape_memo_candidates", CounterEntry::Kind::UnsignedCount, &Counters::encodeSlotPsoPrefetchDrawResourceShapeMemoCandidates, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_draw_resource_shape_memo_hits", CounterEntry::Kind::UnsignedCount, &Counters::encodeSlotPsoPrefetchDrawResourceShapeMemoHits, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_draw_resource_shape_memo_misses", CounterEntry::Kind::UnsignedCount, &Counters::encodeSlotPsoPrefetchDrawResourceShapeMemoMisses, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_draw_resource_shape_memo_overflow", CounterEntry::Kind::UnsignedCount, &Counters::encodeSlotPsoPrefetchDrawResourceShapeMemoOverflow, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_draw_resource_shape_memo_stores", CounterEntry::Kind::UnsignedCount, &Counters::encodeSlotPsoPrefetchDrawResourceShapeMemoStores, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_draw_resource_shape_memo_validated_hits", CounterEntry::Kind::UnsignedCount, &Counters::encodeSlotPsoPrefetchDrawResourceShapeMemoValidatedHits, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_draw_resource_shape_memo_validated_misses", CounterEntry::Kind::UnsignedCount, &Counters::encodeSlotPsoPrefetchDrawResourceShapeMemoValidatedMisses, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_draw_resource_shape_memo_mismatch_texture_mask", CounterEntry::Kind::UnsignedCount, &Counters::encodeSlotPsoPrefetchDrawResourceShapeMemoMismatchTextureMask, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_draw_resource_shape_memo_mismatch_texture_types", CounterEntry::Kind::UnsignedCount, &Counters::encodeSlotPsoPrefetchDrawResourceShapeMemoMismatchTextureTypes, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_draw_resource_shape_memo_mismatch_x8_alpha", CounterEntry::Kind::UnsignedCount, &Counters::encodeSlotPsoPrefetchDrawResourceShapeMemoMismatchX8Alpha, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_draw_resource_shape_memo_mismatch_attachment", CounterEntry::Kind::UnsignedCount, &Counters::encodeSlotPsoPrefetchDrawResourceShapeMemoMismatchAttachment, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_draw_resource_shape_memo_mismatch_sampler_lod_bias", CounterEntry::Kind::UnsignedCount, &Counters::encodeSlotPsoPrefetchDrawResourceShapeMemoMismatchSamplerLodBias, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_draw_resource_shape_memo_mismatch_vsout", CounterEntry::Kind::UnsignedCount, &Counters::encodeSlotPsoPrefetchDrawResourceShapeMemoMismatchVsOut, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_draw_resource_shape_memo_mismatch_other", CounterEntry::Kind::UnsignedCount, &Counters::encodeSlotPsoPrefetchDrawResourceShapeMemoMismatchOther, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_draw_probe_key_memo_hits", CounterEntry::Kind::UnsignedCount, &Counters::encodeSlotPsoPrefetchDrawProbeKeyMemoHits, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_draw_probe_key_memo_misses", CounterEntry::Kind::UnsignedCount, &Counters::encodeSlotPsoPrefetchDrawProbeKeyMemoMisses, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_draw_probe_key_memo_overflow", CounterEntry::Kind::UnsignedCount, &Counters::encodeSlotPsoPrefetchDrawProbeKeyMemoOverflow, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_draw_handle_adjacent_candidates", CounterEntry::Kind::UnsignedCount, &Counters::encodeSlotPsoPrefetchDrawHandleAdjacentCandidates, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_draw_handle_adjacent_hits", CounterEntry::Kind::UnsignedCount, &Counters::encodeSlotPsoPrefetchDrawHandleAdjacentHits, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_draw_handle_slot_repeat_hits", CounterEntry::Kind::UnsignedCount, &Counters::encodeSlotPsoPrefetchDrawHandleSlotRepeatHits, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_draw_handle_slot_unique", CounterEntry::Kind::UnsignedCount, &Counters::encodeSlotPsoPrefetchDrawHandleSlotUnique, nullptr, nullptr, 0.0},
    {"encode_slot_pso_prefetch_draw_handle_slot_overflow", CounterEntry::Kind::UnsignedCount, &Counters::encodeSlotPsoPrefetchDrawHandleSlotOverflow, nullptr, nullptr, 0.0},
    {"command_buffers", CounterEntry::Kind::UnsignedCount, &Counters::commandBuffers, nullptr, nullptr, 0.0},
    {"sub_command_buffers", CounterEntry::Kind::UnsignedCount, &Counters::subCommandBufferCommits, nullptr, nullptr, 0.0},
    {"chunk_subcb_count_max", CounterEntry::Kind::UnsignedCount, &Counters::chunkSubCBCountMax, nullptr, nullptr, 0.0},
    {"subcb_split_suppressed_by_cap", CounterEntry::Kind::UnsignedCount, &Counters::subCommandBufferSplitSuppressedByCap, nullptr, nullptr, 0.0},
    {"gpu_command_buffer_errors", CounterEntry::Kind::UnsignedCount, &Counters::gpuCommandBufferErrors, nullptr, nullptr, 0.0},
    {"metal_buffers", CounterEntry::Kind::UnsignedCount, &Counters::metalBuffers, nullptr, nullptr, 0.0},
    {"metal_buffer_bytes", CounterEntry::Kind::UnsignedCount, &Counters::metalBufferBytes, nullptr, nullptr, 0.0},
    {"pipeline_builds", CounterEntry::Kind::UnsignedCount, &Counters::pipelineBuilds, nullptr, nullptr, 0.0},
    {"pipeline_hit_draw", CounterEntry::Kind::UnsignedCount, &Counters::pipelineHitDraw, nullptr, nullptr, 0.0},
    {"pipeline_hit_fill", CounterEntry::Kind::UnsignedCount, &Counters::pipelineHitFill, nullptr, nullptr, 0.0},
    {"pipeline_hit_stretch", CounterEntry::Kind::UnsignedCount, &Counters::pipelineHitStretch, nullptr, nullptr, 0.0},
    {"pipeline_miss_draw", CounterEntry::Kind::UnsignedCount, &Counters::pipelineMissDraw, nullptr, nullptr, 0.0},
    {"pipeline_miss_fill", CounterEntry::Kind::UnsignedCount, &Counters::pipelineMissFill, nullptr, nullptr, 0.0},
    {"pipeline_miss_stretch", CounterEntry::Kind::UnsignedCount, &Counters::pipelineMissStretch, nullptr, nullptr, 0.0},
    {"pipeline_build_draw", CounterEntry::Kind::UnsignedCount, &Counters::pipelineBuildDraw, nullptr, nullptr, 0.0},
    {"pipeline_build_fill", CounterEntry::Kind::UnsignedCount, &Counters::pipelineBuildFill, nullptr, nullptr, 0.0},
    {"pipeline_build_stretch", CounterEntry::Kind::UnsignedCount, &Counters::pipelineBuildStretch, nullptr, nullptr, 0.0},
    {"pipeline_build_present", CounterEntry::Kind::UnsignedCount, &Counters::pipelineBuildPresent, nullptr, nullptr, 0.0},
    {"pso_slots_draw", CounterEntry::Kind::UnsignedCount, &Counters::psoSlotsDraw, nullptr, nullptr, 0.0},
    {"pso_slots_draw_max", CounterEntry::Kind::UnsignedCount, &Counters::psoSlotsDrawMax, nullptr, nullptr, 0.0},
    {"pso_slot_exhausted", CounterEntry::Kind::UnsignedCount, &Counters::psoSlotExhausted, nullptr, nullptr, 0.0},
    {"pso_variant_argbuf_stage2", CounterEntry::Kind::UnsignedCount, &Counters::psoVariantArgbufStage2, nullptr, nullptr, 0.0},
    {"pso_variant_tile_ffp", CounterEntry::Kind::UnsignedCount, &Counters::psoVariantTileFfp, nullptr, nullptr, 0.0},
    {"source_library_entries", CounterEntry::Kind::UnsignedCount, &Counters::sourceLibraryEntries, nullptr, nullptr, 0.0},
    {"source_library_entries_max", CounterEntry::Kind::UnsignedCount, &Counters::sourceLibraryEntriesMax, nullptr, nullptr, 0.0},
    {"pipeline_build_fail_draw", CounterEntry::Kind::UnsignedCount, &Counters::pipelineBuildFailDraw, nullptr, nullptr, 0.0},
    {"pipeline_build_fail_library", CounterEntry::Kind::UnsignedCount, &Counters::pipelineBuildFailLibrary, nullptr, nullptr, 0.0},
    {"pipeline_build_fail_function", CounterEntry::Kind::UnsignedCount, &Counters::pipelineBuildFailFunction, nullptr, nullptr, 0.0},
    {"pipeline_build_fail_pso", CounterEntry::Kind::UnsignedCount, &Counters::pipelineBuildFailPso, nullptr, nullptr, 0.0},
    {"draw_skipped_no_pipeline", CounterEntry::Kind::UnsignedCount, &Counters::drawSkippedNoPipeline, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_groups", CounterEntry::Kind::UnsignedCount, &Counters::submitDrawRunBatchGroups, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_records", CounterEntry::Kind::UnsignedCount, &Counters::submitDrawRunBatchRecords, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_max_records", CounterEntry::Kind::UnsignedCount, &Counters::submitDrawRunBatchMaxRecords, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_discarded_state_records", CounterEntry::Kind::UnsignedCount, &Counters::submitDrawRunBatchDiscardedStateRecords, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_discarded_state_bytes", CounterEntry::Kind::UnsignedCount, &Counters::submitDrawRunBatchDiscardedStateBytes, nullptr, nullptr, 0.0},
    {"submit_draw_run_binding_snapshot_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::submitDrawRunBindingSnapshotCpuNs, nullptr, nullptr, 0.0},
    {"submit_draw_run_binding_snapshot_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::submitDrawRunBindingSnapshotCpuMaxNs, nullptr, nullptr, 0.0},
    {"submit_draw_run_payload_bytes_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::submitDrawRunPayloadBytesCpuNs, nullptr, nullptr, 0.0},
    {"submit_draw_run_payload_bytes_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::submitDrawRunPayloadBytesCpuMaxNs, nullptr, nullptr, 0.0},
    {"submit_draw_run_slot_prepare_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::submitDrawRunSlotPrepareCpuNs, nullptr, nullptr, 0.0},
    {"submit_draw_run_slot_prepare_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::submitDrawRunSlotPrepareCpuMaxNs, nullptr, nullptr, 0.0},
    {"submit_draw_run_resource_mark_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::submitDrawRunResourceMarkCpuNs, nullptr, nullptr, 0.0},
    {"submit_draw_run_resource_mark_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::submitDrawRunResourceMarkCpuMaxNs, nullptr, nullptr, 0.0},
    {"submit_draw_run_append_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::submitDrawRunAppendCpuNs, nullptr, nullptr, 0.0},
    {"submit_draw_run_append_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::submitDrawRunAppendCpuMaxNs, nullptr, nullptr, 0.0},
    {"submit_draw_run_chunk_commit_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::submitDrawRunChunkCommitCpuNs, nullptr, nullptr, 0.0},
    {"submit_draw_run_chunk_commit_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::submitDrawRunChunkCommitCpuMaxNs, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_queue_lock_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::submitDrawRunBatchQueueLockCpuNs, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_queue_lock_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::submitDrawRunBatchQueueLockCpuMaxNs, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_compat_scan_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::submitDrawRunBatchCompatScanCpuNs, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_compat_scan_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::submitDrawRunBatchCompatScanCpuMaxNs, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_submission_adjacent_pairs", CounterEntry::Kind::UnsignedCount, &Counters::submitDrawRunBatchSubmissionAdjacentPairs, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_submission_adjacent_same_generation_lane", CounterEntry::Kind::UnsignedCount, &Counters::submitDrawRunBatchSubmissionAdjacentSameGenerationLane, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_compat_pairs", CounterEntry::Kind::UnsignedCount, &Counters::submitDrawRunBatchCompatPairs, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_compat_compatible", CounterEntry::Kind::UnsignedCount, &Counters::submitDrawRunBatchCompatCompatible, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_compat_incompatible", CounterEntry::Kind::UnsignedCount, &Counters::submitDrawRunBatchCompatIncompatible, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_compat_same_generation_lane", CounterEntry::Kind::UnsignedCount, &Counters::submitDrawRunBatchCompatSameGenerationLane, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_compat_same_generation_lane_compatible", CounterEntry::Kind::UnsignedCount, &Counters::submitDrawRunBatchCompatSameGenerationLaneCompatible, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_compat_same_generation_lane_incompatible", CounterEntry::Kind::UnsignedCount, &Counters::submitDrawRunBatchCompatSameGenerationLaneIncompatible, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_incompat_texture", CounterEntry::Kind::UnsignedCount, &Counters::submitDrawRunBatchIncompatTexture, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_incompat_sampler", CounterEntry::Kind::UnsignedCount, &Counters::submitDrawRunBatchIncompatSampler, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_incompat_texture_stage_state", CounterEntry::Kind::UnsignedCount, &Counters::submitDrawRunBatchIncompatTextureStageState, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_incompat_render_state", CounterEntry::Kind::UnsignedCount, &Counters::submitDrawRunBatchIncompatRenderState, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_incompat_shader", CounterEntry::Kind::UnsignedCount, &Counters::submitDrawRunBatchIncompatShader, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_incompat_vertex_decl", CounterEntry::Kind::UnsignedCount, &Counters::submitDrawRunBatchIncompatVertexDecl, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_incompat_attachment", CounterEntry::Kind::UnsignedCount, &Counters::submitDrawRunBatchIncompatAttachment, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_incompat_viewport", CounterEntry::Kind::UnsignedCount, &Counters::submitDrawRunBatchIncompatViewport, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_incompat_clip_plane", CounterEntry::Kind::UnsignedCount, &Counters::submitDrawRunBatchIncompatClipPlane, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_incompat_layout_usage", CounterEntry::Kind::UnsignedCount, &Counters::submitDrawRunBatchIncompatLayoutUsage, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_incompat_unknown", CounterEntry::Kind::UnsignedCount, &Counters::submitDrawRunBatchIncompatUnknown, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_incompat_texture_only", CounterEntry::Kind::UnsignedCount, &Counters::submitDrawRunBatchIncompatTextureOnly, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_incompat_rs_alpha_test_only", CounterEntry::Kind::UnsignedCount, &Counters::submitDrawRunBatchIncompatRsAlphaTestOnly, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_incompat_rs_blend_only", CounterEntry::Kind::UnsignedCount, &Counters::submitDrawRunBatchIncompatRsBlendOnly, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_incompat_rs_cull_only", CounterEntry::Kind::UnsignedCount, &Counters::submitDrawRunBatchIncompatRsCullOnly, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_incompat_rs_depth_only", CounterEntry::Kind::UnsignedCount, &Counters::submitDrawRunBatchIncompatRsDepthOnly, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_incompat_rs_fog_only", CounterEntry::Kind::UnsignedCount, &Counters::submitDrawRunBatchIncompatRsFogOnly, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_incompat_rs_texture_factor_only", CounterEntry::Kind::UnsignedCount, &Counters::submitDrawRunBatchIncompatRsTextureFactorOnly, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_incompat_rs_single_other", CounterEntry::Kind::UnsignedCount, &Counters::submitDrawRunBatchIncompatRsSingleOther, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_incompat_rs_mixed", CounterEntry::Kind::UnsignedCount, &Counters::submitDrawRunBatchIncompatRsMixed, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_binding_override_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::submitDrawRunBatchBindingOverrideCpuNs, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_binding_override_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::submitDrawRunBatchBindingOverrideCpuMaxNs, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_binding_snapshot_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::submitDrawRunBatchBindingSnapshotCpuNs, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_binding_snapshot_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::submitDrawRunBatchBindingSnapshotCpuMaxNs, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_payload_bytes_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::submitDrawRunBatchPayloadBytesCpuNs, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_payload_bytes_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::submitDrawRunBatchPayloadBytesCpuMaxNs, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_slot_prepare_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::submitDrawRunBatchSlotPrepareCpuNs, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_slot_prepare_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::submitDrawRunBatchSlotPrepareCpuMaxNs, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_resource_mark_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::submitDrawRunBatchResourceMarkCpuNs, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_resource_mark_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::submitDrawRunBatchResourceMarkCpuMaxNs, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_append_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::submitDrawRunBatchAppendCpuNs, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_append_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::submitDrawRunBatchAppendCpuMaxNs, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_append_reserve_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::submitDrawRunBatchAppendReserveCpuNs, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_append_reserve_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::submitDrawRunBatchAppendReserveCpuMaxNs, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_append_state_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::submitDrawRunBatchAppendStateCpuNs, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_append_state_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::submitDrawRunBatchAppendStateCpuMaxNs, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_append_state_pso_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::submitDrawRunBatchAppendStatePsoCpuNs, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_append_state_pso_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::submitDrawRunBatchAppendStatePsoCpuMaxNs, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_append_state_invariant_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::submitDrawRunBatchAppendStateInvariantCpuNs, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_append_state_invariant_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::submitDrawRunBatchAppendStateInvariantCpuMaxNs, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_append_state_soa_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::submitDrawRunBatchAppendStateSoaCpuNs, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_append_state_soa_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::submitDrawRunBatchAppendStateSoaCpuMaxNs, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_append_uniform_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::submitDrawRunBatchAppendUniformCpuNs, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_append_uniform_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::submitDrawRunBatchAppendUniformCpuMaxNs, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_append_payload_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::submitDrawRunBatchAppendPayloadCpuNs, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_append_payload_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::submitDrawRunBatchAppendPayloadCpuMaxNs, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_append_param_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::submitDrawRunBatchAppendParamCpuNs, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_append_param_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::submitDrawRunBatchAppendParamCpuMaxNs, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_append_record_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::submitDrawRunBatchAppendRecordCpuNs, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_append_record_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::submitDrawRunBatchAppendRecordCpuMaxNs, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_append_payload_bytes", CounterEntry::Kind::UnsignedCount, &Counters::submitDrawRunBatchAppendPayloadBytes, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_append_params", CounterEntry::Kind::UnsignedCount, &Counters::submitDrawRunBatchAppendParams, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_chunk_commit_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::submitDrawRunBatchChunkCommitCpuNs, nullptr, nullptr, 0.0},
    {"submit_draw_run_batch_chunk_commit_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::submitDrawRunBatchChunkCommitCpuMaxNs, nullptr, nullptr, 0.0},
    {"render_pass_begin", CounterEntry::Kind::UnsignedCount, &Counters::renderPassBegin, nullptr, nullptr, 0.0},
    {"render_pass_end", CounterEntry::Kind::UnsignedCount, &Counters::renderPassEnd, nullptr, nullptr, 0.0},
    {"render_split_final", CounterEntry::Kind::UnsignedCount, &Counters::renderSplitFinal, nullptr, nullptr, 0.0},
    {"render_split_rt_change", CounterEntry::Kind::UnsignedCount, &Counters::renderSplitRenderTargetChange, nullptr, nullptr, 0.0},
    {"render_split_hazard", CounterEntry::Kind::UnsignedCount, &Counters::renderSplitHazard, nullptr, nullptr, 0.0},
    {"render_split_clear", CounterEntry::Kind::UnsignedCount, &Counters::renderSplitClearBarrier, nullptr, nullptr, 0.0},
    {"render_split_surface_copy", CounterEntry::Kind::UnsignedCount, &Counters::renderSplitSurfaceCopy, nullptr, nullptr, 0.0},
    {"render_split_stretch", CounterEntry::Kind::UnsignedCount, &Counters::renderSplitStretchRect, nullptr, nullptr, 0.0},
    {"render_split_readback", CounterEntry::Kind::UnsignedCount, &Counters::renderSplitReadback, nullptr, nullptr, 0.0},
    {"render_split_color_fill", CounterEntry::Kind::UnsignedCount, &Counters::renderSplitColorFill, nullptr, nullptr, 0.0},
    {"render_split_present", CounterEntry::Kind::UnsignedCount, &Counters::renderSplitPresent, nullptr, nullptr, 0.0},
    {"render_split_present_acquire", CounterEntry::Kind::UnsignedCount, &Counters::renderSplitPresentAcquire, nullptr, nullptr, 0.0},
    {"render_split_tile_midpass", CounterEntry::Kind::UnsignedCount, &Counters::renderSplitTileMidPassIneligible, nullptr, nullptr, 0.0},
    {"render_split_ordered_control", CounterEntry::Kind::UnsignedCount, &Counters::renderSplitOrderedControl, nullptr, nullptr, 0.0},
    {"hazard_probe", CounterEntry::Kind::UnsignedCount, &Counters::hazardProbeComparisons, nullptr, nullptr, 0.0},
    {"hazard_bloom", CounterEntry::Kind::UnsignedCount, &Counters::hazardBloomOverlaps, nullptr, nullptr, 0.0},
    {"hazard_exact", CounterEntry::Kind::UnsignedCount, &Counters::hazardExactOverlaps, nullptr, nullptr, 0.0},
    {"hazard_bloom_false_positive", CounterEntry::Kind::UnsignedCount, &Counters::hazardBloomFalsePositive, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_submission_batch_submits", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawSubmissionBatchSubmits, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_submission_batch_records", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawSubmissionBatchRecords, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_submission_batch_max_records", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawSubmissionBatchMaxRecords, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_submission_batch_size_1", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawSubmissionBatchSize1, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_submission_batch_size_2", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawSubmissionBatchSize2, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_submission_batch_size_3_4", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawSubmissionBatchSize3To4, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_submission_batch_size_5_8", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawSubmissionBatchSize5To8, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_submission_batch_size_9_16", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawSubmissionBatchSize9To16, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_submission_batch_size_17_32", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawSubmissionBatchSize17To32, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_submission_batch_size_33_plus", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawSubmissionBatchSize33Plus, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_run_scan_cpu_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::commitChunkDrawRunScanCpuRing, 0.5},
    {"commit_chunk_draw_run_scan_cpu_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::commitChunkDrawRunScanCpuRing, 0.95},
    {"commit_chunk_draw_run_build_cpu_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::commitChunkDrawRunBuildCpuRing, 0.5},
    {"commit_chunk_draw_run_build_cpu_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::commitChunkDrawRunBuildCpuRing, 0.95},
    {"commit_chunk_draw_run_submit_cpu_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::commitChunkDrawRunSubmitCpuRing, 0.5},
    {"commit_chunk_draw_run_submit_cpu_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::commitChunkDrawRunSubmitCpuRing, 0.95},
    {"commit_chunk_draw_run_final_bind_cpu_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::commitChunkDrawRunFinalBindCpuRing, 0.5},
    {"commit_chunk_draw_run_final_bind_cpu_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::commitChunkDrawRunFinalBindCpuRing, 0.95},
    {"commit_chunk_queue_draw_submission_cpu_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::commitChunkQueueDrawSubmissionCpuRing, 0.5},
    {"commit_chunk_queue_draw_submission_cpu_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::commitChunkQueueDrawSubmissionCpuRing, 0.95},
    {"commit_chunk_queue_draw_submission_emplace_cpu_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::commitChunkQueueDrawSubmissionEmplaceCpuRing, 0.5},
    {"commit_chunk_queue_draw_submission_emplace_cpu_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::commitChunkQueueDrawSubmissionEmplaceCpuRing, 0.95},
    {"commit_chunk_queue_draw_submission_snapshot_cpu_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::commitChunkQueueDrawSubmissionSnapshotCpuRing, 0.5},
    {"commit_chunk_queue_draw_submission_snapshot_cpu_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::commitChunkQueueDrawSubmissionSnapshotCpuRing, 0.95},
    {"commit_chunk_index_bind_cpu_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::commitChunkIndexBindCpuRing, 0.5},
    {"commit_chunk_index_bind_cpu_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::commitChunkIndexBindCpuRing, 0.95},
    {"commit_chunk_replay_pending_flush_cpu_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::commitChunkReplayPendingFlushCpuRing, 0.5},
    {"commit_chunk_replay_pending_flush_cpu_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::commitChunkReplayPendingFlushCpuRing, 0.95},
    {"commit_chunk_replay_draw_record_cpu_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::commitChunkReplayDrawRecordCpuRing, 0.5},
    {"commit_chunk_replay_draw_record_cpu_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::commitChunkReplayDrawRecordCpuRing, 0.95},
    {"commit_chunk_replay_non_draw_record_cpu_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::commitChunkReplayNonDrawRecordCpuRing, 0.5},
    {"commit_chunk_replay_non_draw_record_cpu_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::commitChunkReplayNonDrawRecordCpuRing, 0.95},
    {"commit_chunk_replay_const_record_cpu_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::commitChunkReplayConstRecordCpuRing, 0.5},
    {"commit_chunk_replay_const_record_cpu_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::commitChunkReplayConstRecordCpuRing, 0.95},
    {"commit_chunk_replay_apply_state_record_cpu_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::commitChunkReplayApplyStateRecordCpuRing, 0.5},
    {"commit_chunk_replay_apply_state_record_cpu_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::commitChunkReplayApplyStateRecordCpuRing, 0.95},
    {"commit_chunk_replay_clear_record_cpu_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::commitChunkReplayClearRecordCpuRing, 0.5},
    {"commit_chunk_replay_clear_record_cpu_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::commitChunkReplayClearRecordCpuRing, 0.95},
    {"commit_chunk_replay_present_record_cpu_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::commitChunkReplayPresentRecordCpuRing, 0.5},
    {"commit_chunk_replay_present_record_cpu_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::commitChunkReplayPresentRecordCpuRing, 0.95},
    {"commit_chunk_replay_surface_record_cpu_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::commitChunkReplaySurfaceRecordCpuRing, 0.5},
    {"commit_chunk_replay_surface_record_cpu_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::commitChunkReplaySurfaceRecordCpuRing, 0.95},
    {"commit_chunk_replay_query_record_cpu_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::commitChunkReplayQueryRecordCpuRing, 0.5},
    {"commit_chunk_replay_query_record_cpu_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::commitChunkReplayQueryRecordCpuRing, 0.95},
    {"commit_chunk_replay_other_record_cpu_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::commitChunkReplayOtherRecordCpuRing, 0.5},
    {"commit_chunk_replay_other_record_cpu_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::commitChunkReplayOtherRecordCpuRing, 0.95},
    {"commit_chunk_const_upload_cpu_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::commitChunkConstUploadCpuRing, 0.5},
    {"commit_chunk_const_upload_cpu_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::commitChunkConstUploadCpuRing, 0.95},
    {"d3d9_draw_state_cache_hits", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheHits, nullptr, nullptr, 0.0},
    {"d3d9_draw_state_cache_misses", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheMisses, nullptr, nullptr, 0.0},
    {"d3d9_draw_state_cache_hit_with_index", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheHitWithIndex, nullptr, nullptr, 0.0},
    {"d3d9_draw_state_cache_miss_with_index", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheMissWithIndex, nullptr, nullptr, 0.0},
    {"d3d9_draw_state_cache_hit_no_index", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheHitNoIndex, nullptr, nullptr, 0.0},
    {"d3d9_draw_state_cache_miss_no_index", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheMissNoIndex, nullptr, nullptr, 0.0},
    {"d3d9_draw_state_cache_direct_hits", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheDirectHits, nullptr, nullptr, 0.0},
    {"d3d9_draw_state_cache_direct_misses", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheDirectMisses, nullptr, nullptr, 0.0},
    {"d3d9_draw_state_cache_direct_hit_with_index", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheDirectHitWithIndex, nullptr, nullptr, 0.0},
    {"d3d9_draw_state_cache_direct_miss_with_index", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheDirectMissWithIndex, nullptr, nullptr, 0.0},
    {"d3d9_draw_state_cache_direct_hit_no_index", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheDirectHitNoIndex, nullptr, nullptr, 0.0},
    {"d3d9_draw_state_cache_direct_miss_no_index", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheDirectMissNoIndex, nullptr, nullptr, 0.0},
    {"d3d9_draw_state_cache_batch_hits", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheBatchHits, nullptr, nullptr, 0.0},
    {"d3d9_draw_state_cache_batch_misses", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheBatchMisses, nullptr, nullptr, 0.0},
    {"d3d9_draw_state_cache_uniform_refreshes", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheUniformRefreshes, nullptr, nullptr, 0.0},
    {"d3d9_draw_state_cache_batch_miss_reason_unknown", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheBatchMissReasonUnknown, nullptr, nullptr, 0.0},
    {"d3d9_draw_state_cache_batch_miss_reason_binding_only", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheBatchMissReasonBindingOnly, nullptr, nullptr, 0.0},
    {"d3d9_draw_state_cache_batch_miss_reason_single_render_state", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheBatchMissReasonSingleRenderState, nullptr, nullptr, 0.0},
    {"d3d9_draw_state_cache_batch_miss_reason_single_texture", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheBatchMissReasonSingleTexture, nullptr, nullptr, 0.0},
    {"d3d9_draw_state_cache_batch_miss_reason_single_fvf_vdecl", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheBatchMissReasonSingleFvfVdecl, nullptr, nullptr, 0.0},
    {"d3d9_draw_state_cache_batch_miss_reason_single_shader", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheBatchMissReasonSingleShader, nullptr, nullptr, 0.0},
    {"d3d9_draw_state_cache_batch_miss_reason_single_rt_depth", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheBatchMissReasonSingleRtDepth, nullptr, nullptr, 0.0},
    {"d3d9_draw_state_cache_batch_miss_reason_single_viewport_scissor", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheBatchMissReasonSingleViewportScissor, nullptr, nullptr, 0.0},
    {"d3d9_draw_state_cache_batch_miss_reason_single_tss_sampler", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheBatchMissReasonSingleTssSampler, nullptr, nullptr, 0.0},
    {"d3d9_draw_state_cache_batch_miss_reason_single_ffp_clip", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheBatchMissReasonSingleFfpClip, nullptr, nullptr, 0.0},
    {"d3d9_draw_state_cache_batch_miss_reason_single_broad", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheBatchMissReasonSingleBroad, nullptr, nullptr, 0.0},
    {"d3d9_draw_state_cache_batch_miss_reason_mixed_2", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheBatchMissReasonMixed2, nullptr, nullptr, 0.0},
    {"d3d9_draw_state_cache_batch_miss_reason_mixed_3", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheBatchMissReasonMixed3, nullptr, nullptr, 0.0},
    {"d3d9_draw_state_cache_batch_miss_reason_mixed_4plus", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheBatchMissReasonMixed4Plus, nullptr, nullptr, 0.0},
    {"d3d9_draw_state_cache_batch_miss_reason_has_render_state", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheBatchMissReasonHasRenderState, nullptr, nullptr, 0.0},
    {"d3d9_draw_state_cache_batch_miss_reason_has_texture", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheBatchMissReasonHasTexture, nullptr, nullptr, 0.0},
    {"d3d9_draw_state_cache_batch_miss_reason_has_fvf_vdecl", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheBatchMissReasonHasFvfVdecl, nullptr, nullptr, 0.0},
    {"d3d9_draw_state_cache_batch_miss_reason_has_shader", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheBatchMissReasonHasShader, nullptr, nullptr, 0.0},
    {"d3d9_draw_state_cache_batch_miss_reason_has_rt_depth", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheBatchMissReasonHasRtDepth, nullptr, nullptr, 0.0},
    {"d3d9_draw_state_cache_batch_miss_reason_has_viewport_scissor", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheBatchMissReasonHasViewportScissor, nullptr, nullptr, 0.0},
    {"d3d9_draw_state_cache_batch_miss_reason_has_tss_sampler", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheBatchMissReasonHasTssSampler, nullptr, nullptr, 0.0},
    {"d3d9_draw_state_cache_batch_miss_reason_has_ffp_clip", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheBatchMissReasonHasFfpClip, nullptr, nullptr, 0.0},
    {"d3d9_draw_state_cache_batch_miss_reason_has_broad", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheBatchMissReasonHasBroad, nullptr, nullptr, 0.0},
    {"d3d9_draw_state_cache_batch_miss_reason_has_texture_shader", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheBatchMissReasonHasTextureShader, nullptr, nullptr, 0.0},
    {"d3d9_draw_state_cache_batch_miss_reason_has_texture_fvf_vdecl", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheBatchMissReasonHasTextureFvfVdecl, nullptr, nullptr, 0.0},
    {"d3d9_draw_state_cache_batch_miss_reason_has_shader_fvf_vdecl", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheBatchMissReasonHasShaderFvfVdecl, nullptr, nullptr, 0.0},
    {"d3d9_draw_state_cache_batch_miss_reason_has_texture_tss_sampler", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheBatchMissReasonHasTextureTssSampler, nullptr, nullptr, 0.0},
    {"d3d9_draw_state_cache_batch_miss_reason_has_texture_shader_fvf_vdecl", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheBatchMissReasonHasTextureShaderFvfVdecl, nullptr, nullptr, 0.0},
    {"d3d9_draw_state_cache_batch_miss_reason_has_texture_shader_tss_sampler", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheBatchMissReasonHasTextureShaderTssSampler, nullptr, nullptr, 0.0},
    {"d3d9_draw_state_cache_batch_miss_reason_has_texture_fvf_vdecl_tss_sampler", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheBatchMissReasonHasTextureFvfVdeclTssSampler, nullptr, nullptr, 0.0},
    {"d3d9_draw_state_cache_batch_miss_reason_has_shader_fvf_vdecl_tss_sampler", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheBatchMissReasonHasShaderFvfVdeclTssSampler, nullptr, nullptr, 0.0},
    {"d3d9_draw_state_cache_batch_miss_reason_has_texture_shader_fvf_vdecl_tss_sampler", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheBatchMissReasonHasTextureShaderFvfVdeclTssSampler, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_semantic_reuse_probe_samples", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotCacheBatchMissSemanticReuseProbeSamples, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_semantic_reuse_probe_hits", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotCacheBatchMissSemanticReuseProbeHits, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_semantic_reuse_probe_misses", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotCacheBatchMissSemanticReuseProbeMisses, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_semantic_reuse_probe_hit_distance_1", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotCacheBatchMissSemanticReuseProbeHitDistance1, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_semantic_reuse_probe_hit_distance_2", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotCacheBatchMissSemanticReuseProbeHitDistance2, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_semantic_reuse_probe_hit_distance_3_4", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotCacheBatchMissSemanticReuseProbeHitDistance3To4, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_semantic_reuse_probe_hit_distance_5_8", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotCacheBatchMissSemanticReuseProbeHitDistance5To8, nullptr, nullptr, 0.0},
    {"d3d9_draw_state_cache_miss_after_unknown", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheMissAfterUnknown, nullptr, nullptr, 0.0},
    {"d3d9_draw_state_cache_miss_after_mutable_state", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheMissAfterMutableState, nullptr, nullptr, 0.0},
    {"d3d9_draw_state_cache_miss_after_draw_packet", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheMissAfterDrawPacket, nullptr, nullptr, 0.0},
    {"d3d9_draw_state_cache_miss_after_render_state", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheMissAfterRenderState, nullptr, nullptr, 0.0},
    {"d3d9_draw_state_cache_miss_after_texture", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheMissAfterTexture, nullptr, nullptr, 0.0},
    {"d3d9_draw_state_cache_miss_after_stream", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheMissAfterStream, nullptr, nullptr, 0.0},
    {"d3d9_draw_state_cache_miss_after_index_buffer", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheMissAfterIndexBuffer, nullptr, nullptr, 0.0},
    {"d3d9_draw_state_cache_miss_after_fvf_vdecl", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheMissAfterFvfVdecl, nullptr, nullptr, 0.0},
    {"d3d9_draw_state_cache_miss_after_shader", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheMissAfterShader, nullptr, nullptr, 0.0},
    {"d3d9_draw_state_cache_miss_after_render_target_depth", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheMissAfterRenderTargetDepth, nullptr, nullptr, 0.0},
    {"d3d9_draw_state_cache_miss_after_viewport_scissor", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheMissAfterViewportScissor, nullptr, nullptr, 0.0},
    {"d3d9_draw_state_cache_miss_after_texture_stage_sampler", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheMissAfterTextureStageSampler, nullptr, nullptr, 0.0},
    {"d3d9_draw_state_cache_miss_after_ffp_state", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheMissAfterFfpState, nullptr, nullptr, 0.0},
    {"d3d9_draw_state_cache_miss_after_clip_plane", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheMissAfterClipPlane, nullptr, nullptr, 0.0},
    {"d3d9_draw_state_cache_miss_after_state_block", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheMissAfterStateBlock, nullptr, nullptr, 0.0},
    {"d3d9_draw_state_cache_miss_after_reset", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheMissAfterReset, nullptr, nullptr, 0.0},
    {"d3d9_draw_state_cache_miss_after_swap_chain", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheMissAfterSwapChain, nullptr, nullptr, 0.0},
    {"d3d9_draw_state_cache_miss_after_texture_lod", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheMissAfterTextureLod, nullptr, nullptr, 0.0},
    {"draw_calls", CounterEntry::Kind::UnsignedCount, &Counters::drawCalls, nullptr, nullptr, 0.0},
    {"draw_indexed", CounterEntry::Kind::UnsignedCount, &Counters::drawIndexedCalls, nullptr, nullptr, 0.0},
    {"draw_expanded_indexed", CounterEntry::Kind::UnsignedCount, &Counters::drawExpandedIndexedCalls, nullptr, nullptr, 0.0},
    {"draw_primitives", CounterEntry::Kind::UnsignedCount, &Counters::drawPrimitiveCount, nullptr, nullptr, 0.0},
    {"draw_triangles", CounterEntry::Kind::UnsignedCount, &Counters::drawTriangleEstimate, nullptr, nullptr, 0.0},
    {"draw_vertices", CounterEntry::Kind::UnsignedCount, &Counters::drawVertexCount, nullptr, nullptr, 0.0},
    {"draw_up_vertex_bytes", CounterEntry::Kind::UnsignedCount, &Counters::drawUpVertexBytes, nullptr, nullptr, 0.0},
    {"draw_up_index_bytes", CounterEntry::Kind::UnsignedCount, &Counters::drawUpIndexBytes, nullptr, nullptr, 0.0},
    {"bind_texture", CounterEntry::Kind::UnsignedCount, &Counters::bindTexture, nullptr, nullptr, 0.0},
    {"bind_sampler", CounterEntry::Kind::UnsignedCount, &Counters::bindSampler, nullptr, nullptr, 0.0},
    {"bind_texture_skipped", CounterEntry::Kind::UnsignedCount, &Counters::bindTextureSkipped, nullptr, nullptr, 0.0},
    {"bind_sampler_skipped", CounterEntry::Kind::UnsignedCount, &Counters::bindSamplerSkipped, nullptr, nullptr, 0.0},
    {"bind_vertex_buffer", CounterEntry::Kind::UnsignedCount, &Counters::bindVertexBuffer, nullptr, nullptr, 0.0},
    {"bind_vertex_buffer_skipped", CounterEntry::Kind::UnsignedCount, &Counters::bindVertexBufferSkipped, nullptr, nullptr, 0.0},
    {"bind_index_buffer", CounterEntry::Kind::UnsignedCount, &Counters::bindIndexBuffer, nullptr, nullptr, 0.0},
    {"bind_index_buffer_skipped", CounterEntry::Kind::UnsignedCount, &Counters::bindIndexBufferSkipped, nullptr, nullptr, 0.0},
    {"bind_uniform_buffer", CounterEntry::Kind::UnsignedCount, &Counters::bindUniformBuffer, nullptr, nullptr, 0.0},
    {"bind_pipeline", CounterEntry::Kind::UnsignedCount, &Counters::bindPipeline, nullptr, nullptr, 0.0},
    {"bind_pipeline_skipped", CounterEntry::Kind::UnsignedCount, &Counters::bindPipelineSkipped, nullptr, nullptr, 0.0},
    {"bind_depth_state", CounterEntry::Kind::UnsignedCount, &Counters::bindDepthState, nullptr, nullptr, 0.0},
    {"bind_depth_state_skipped", CounterEntry::Kind::UnsignedCount, &Counters::bindDepthStateSkipped, nullptr, nullptr, 0.0},
    {"bind_viewport", CounterEntry::Kind::UnsignedCount, &Counters::bindViewport, nullptr, nullptr, 0.0},
    {"bind_viewport_skipped", CounterEntry::Kind::UnsignedCount, &Counters::bindViewportSkipped, nullptr, nullptr, 0.0},
    {"bind_scissor", CounterEntry::Kind::UnsignedCount, &Counters::bindScissor, nullptr, nullptr, 0.0},
    {"bind_scissor_skipped", CounterEntry::Kind::UnsignedCount, &Counters::bindScissorSkipped, nullptr, nullptr, 0.0},
    {"bind_rasterizer", CounterEntry::Kind::UnsignedCount, &Counters::bindRasterizer, nullptr, nullptr, 0.0},
    {"bind_rasterizer_skipped", CounterEntry::Kind::UnsignedCount, &Counters::bindRasterizerSkipped, nullptr, nullptr, 0.0},
    {"draw_shader_bucket_samples", CounterEntry::Kind::UnsignedCount, &Counters::drawShaderBucketSamples, nullptr, nullptr, 0.0},
    {"draw_shader_bucket_changes", CounterEntry::Kind::UnsignedCount, &Counters::drawShaderBucketChanges, nullptr, nullptr, 0.0},
    {"last_vs", CounterEntry::Kind::Hex64, &Counters::lastVertexShaderHash, nullptr, nullptr, 0.0},
    {"last_ps", CounterEntry::Kind::Hex64, &Counters::lastPixelShaderHash, nullptr, nullptr, 0.0},
    {"last_variant", CounterEntry::Kind::Hex64, &Counters::lastShaderVariantHash, nullptr, nullptr, 0.0},
    {"draw_geometry_samples", CounterEntry::Kind::UnsignedCount, &Counters::drawGeometrySamples, nullptr, nullptr, 0.0},
    {"draw_geometry_ffp", CounterEntry::Kind::UnsignedCount, &Counters::drawGeometryFfp, nullptr, nullptr, 0.0},
    {"draw_geometry_vs", CounterEntry::Kind::UnsignedCount, &Counters::drawGeometryVs, nullptr, nullptr, 0.0},
    {"draw_geometry_indexed", CounterEntry::Kind::UnsignedCount, &Counters::drawGeometryIndexed, nullptr, nullptr, 0.0},
    {"draw_geometry_index16", CounterEntry::Kind::UnsignedCount, &Counters::drawGeometryIndex16, nullptr, nullptr, 0.0},
    {"draw_geometry_index32", CounterEntry::Kind::UnsignedCount, &Counters::drawGeometryIndex32, nullptr, nullptr, 0.0},
    {"draw_geometry_direct", CounterEntry::Kind::UnsignedCount, &Counters::drawGeometryDirect, nullptr, nullptr, 0.0},
    {"draw_geometry_up", CounterEntry::Kind::UnsignedCount, &Counters::drawGeometryUp, nullptr, nullptr, 0.0},
    {"draw_geometry_expanded", CounterEntry::Kind::UnsignedCount, &Counters::drawGeometryExpanded, nullptr, nullptr, 0.0},
    {"draw_geometry_nonzero_base_vertex", CounterEntry::Kind::UnsignedCount, &Counters::drawGeometryNonZeroBaseVertex, nullptr, nullptr, 0.0},
    {"draw_geometry_nonzero_start_index", CounterEntry::Kind::UnsignedCount, &Counters::drawGeometryNonZeroStartIndex, nullptr, nullptr, 0.0},
    {"draw_geometry_nonzero_stream0_offset", CounterEntry::Kind::UnsignedCount, &Counters::drawGeometryNonZeroStream0Offset, nullptr, nullptr, 0.0},
    {"draw_geometry_last_stream0_stride", CounterEntry::Kind::UnsignedCount, &Counters::drawGeometryLastStream0Stride, nullptr, nullptr, 0.0},
    {"draw_geometry_last_decl_hash", CounterEntry::Kind::Hex64, &Counters::drawGeometryLastVertexDeclHash, nullptr, nullptr, 0.0},
    {"completion_compat_fp16_ms", CounterEntry::Kind::Milliseconds, &Counters::completionCompatFp16WaitNs, nullptr, nullptr, 0.0},
    {"completion_compat_mrt_ms", CounterEntry::Kind::Milliseconds, &Counters::completionCompatMrtWaitNs, nullptr, nullptr, 0.0},
    {"completion_compat_srgb_ms", CounterEntry::Kind::Milliseconds, &Counters::completionCompatSrgbWaitNs, nullptr, nullptr, 0.0},
    {"completion_compat_projected_ms", CounterEntry::Kind::Milliseconds, &Counters::completionCompatProjectedWaitNs, nullptr, nullptr, 0.0},
    {"completion_compat_msaa_ms", CounterEntry::Kind::Milliseconds, &Counters::completionCompatMsaaWaitNs, nullptr, nullptr, 0.0},
    {"completion_compat_query_ms", CounterEntry::Kind::Milliseconds, &Counters::completionCompatQueryWaitNs, nullptr, nullptr, 0.0},
    {"completion_shader_bucket_samples", CounterEntry::Kind::UnsignedCount, &Counters::completionShaderBucketSamples, nullptr, nullptr, 0.0},
    {"completion_shader_bucket_changes", CounterEntry::Kind::UnsignedCount, &Counters::completionShaderBucketChanges, nullptr, nullptr, 0.0},
    {"completion_last_vs", CounterEntry::Kind::Hex64, &Counters::completionLastVertexShaderHash, nullptr, nullptr, 0.0},
    {"completion_last_ps", CounterEntry::Kind::Hex64, &Counters::completionLastPixelShaderHash, nullptr, nullptr, 0.0},
    {"completion_last_variant", CounterEntry::Kind::Hex64, &Counters::completionLastShaderVariantHash, nullptr, nullptr, 0.0},
    {"submit_draw_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::submitDrawCpuNs, nullptr, nullptr, 0.0},
    {"submit_draw_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::submitDrawCpuMaxNs, nullptr, nullptr, 0.0},
    {"submit_draw_cpu_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::submitDrawCpuRing, 0.5},
    {"submit_draw_cpu_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::submitDrawCpuRing, 0.95},
    {"submit_draw_cpu_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::submitDrawCpuRing, 0.99},
    {"gpu_command_buffer_time_ms", CounterEntry::Kind::Milliseconds, &Counters::gpuCommandBufferTimeNs, nullptr, nullptr, 0.0},
    {"gpu_command_buffer_time_max_ms", CounterEntry::Kind::Milliseconds, &Counters::gpuCommandBufferTimeMaxNs, nullptr, nullptr, 0.0},
    {"gpu_command_buffer_time_samples", CounterEntry::Kind::UnsignedCount, &Counters::gpuCommandBufferTimeSamples, nullptr, nullptr, 0.0},
    {"gpu_command_buffer_time_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::gpuCommandBufferTimeRing, 0.5},
    {"gpu_command_buffer_time_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::gpuCommandBufferTimeRing, 0.95},
    {"gpu_command_buffer_time_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::gpuCommandBufferTimeRing, 0.99},
    {"render_encoder_gpu_time_ms", CounterEntry::Kind::Milliseconds, &Counters::renderEncoderGpuTimeNs, nullptr, nullptr, 0.0},
    {"render_encoder_gpu_time_max_ms", CounterEntry::Kind::Milliseconds, &Counters::renderEncoderGpuTimeMaxNs, nullptr, nullptr, 0.0},
    {"render_encoder_gpu_time_samples", CounterEntry::Kind::UnsignedCount, &Counters::renderEncoderGpuTimeSamples, nullptr, nullptr, 0.0},
    {"render_encoder_gpu_time_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::renderEncoderGpuTimeRing, 0.5},
    {"render_encoder_gpu_time_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::renderEncoderGpuTimeRing, 0.95},
    {"render_encoder_gpu_time_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::renderEncoderGpuTimeRing, 0.99},
    {"render_encoder_gpu_draw_ms", CounterEntry::Kind::Milliseconds, &Counters::renderEncoderGpuDrawTimeNs, nullptr, nullptr, 0.0},
    {"render_encoder_gpu_draw_samples", CounterEntry::Kind::UnsignedCount, &Counters::renderEncoderGpuDrawSamples, nullptr, nullptr, 0.0},
    {"render_encoder_gpu_clear_ms", CounterEntry::Kind::Milliseconds, &Counters::renderEncoderGpuClearTimeNs, nullptr, nullptr, 0.0},
    {"render_encoder_gpu_clear_samples", CounterEntry::Kind::UnsignedCount, &Counters::renderEncoderGpuClearSamples, nullptr, nullptr, 0.0},
    {"render_encoder_gpu_surface_copy_ms", CounterEntry::Kind::Milliseconds, &Counters::renderEncoderGpuSurfaceCopyTimeNs, nullptr, nullptr, 0.0},
    {"render_encoder_gpu_surface_copy_samples", CounterEntry::Kind::UnsignedCount, &Counters::renderEncoderGpuSurfaceCopySamples, nullptr, nullptr, 0.0},
    {"render_encoder_gpu_stretch_ms", CounterEntry::Kind::Milliseconds, &Counters::renderEncoderGpuStretchTimeNs, nullptr, nullptr, 0.0},
    {"render_encoder_gpu_stretch_samples", CounterEntry::Kind::UnsignedCount, &Counters::renderEncoderGpuStretchSamples, nullptr, nullptr, 0.0},
    {"render_encoder_gpu_color_fill_ms", CounterEntry::Kind::Milliseconds, &Counters::renderEncoderGpuColorFillTimeNs, nullptr, nullptr, 0.0},
    {"render_encoder_gpu_color_fill_samples", CounterEntry::Kind::UnsignedCount, &Counters::renderEncoderGpuColorFillSamples, nullptr, nullptr, 0.0},
    {"render_encoder_gpu_depth_resolve_ms", CounterEntry::Kind::Milliseconds, &Counters::renderEncoderGpuDepthResolveTimeNs, nullptr, nullptr, 0.0},
    {"render_encoder_gpu_depth_resolve_samples", CounterEntry::Kind::UnsignedCount, &Counters::renderEncoderGpuDepthResolveSamples, nullptr, nullptr, 0.0},
    {"render_encoder_gpu_present_ms", CounterEntry::Kind::Milliseconds, &Counters::renderEncoderGpuPresentTimeNs, nullptr, nullptr, 0.0},
    {"render_encoder_gpu_present_samples", CounterEntry::Kind::UnsignedCount, &Counters::renderEncoderGpuPresentSamples, nullptr, nullptr, 0.0},
    {"render_encoder_gpu_last_pass_type", CounterEntry::Kind::UnsignedCount, &Counters::renderEncoderGpuLastPassType, nullptr, nullptr, 0.0},
    {"render_encoder_gpu_last_rt", CounterEntry::Kind::Hex64, &Counters::renderEncoderGpuLastRt, nullptr, nullptr, 0.0},
    {"render_encoder_gpu_last_depth", CounterEntry::Kind::Hex64, &Counters::renderEncoderGpuLastDepth, nullptr, nullptr, 0.0},
    {"render_encoder_gpu_last_pso", CounterEntry::Kind::Hex64, &Counters::renderEncoderGpuLastPso, nullptr, nullptr, 0.0},
    {"encode_chunk_calls", CounterEntry::Kind::UnsignedCount, &Counters::encodeChunkCalls, nullptr, nullptr, 0.0},
    {"encode_chunk_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeChunkCpuNs, nullptr, nullptr, 0.0},
    {"encode_chunk_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeChunkCpuMaxNs, nullptr, nullptr, 0.0},
    {"encode_chunk_cpu_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::encodeChunkCpuRing, 0.5},
    {"encode_chunk_cpu_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::encodeChunkCpuRing, 0.95},
    {"encode_chunk_cpu_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::encodeChunkCpuRing, 0.99},
    {"encode_draw_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawCpuMaxNs, nullptr, nullptr, 0.0},
    {"encode_draw_cpu_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::encodeDrawCpuRing, 0.5},
    {"encode_draw_cpu_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::encodeDrawCpuRing, 0.95},
    {"encode_draw_cpu_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::encodeDrawCpuRing, 0.99},
    {"encode_draw_pipeline_lookup_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawPipelineLookupCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_pipeline_lookup_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawPipelineLookupCpuMaxNs, nullptr, nullptr, 0.0},
    {"encode_draw_pipeline_lookup_cpu_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::encodeDrawPipelineLookupCpuRing, 0.5},
    {"encode_draw_pipeline_lookup_cpu_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::encodeDrawPipelineLookupCpuRing, 0.95},
    {"encode_draw_pipeline_lookup_cpu_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::encodeDrawPipelineLookupCpuRing, 0.99},
    {"shader_variant_key_hash_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::shaderVariantKeyHashCpuNs, nullptr, nullptr, 0.0},
    {"shader_variant_key_hash_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::shaderVariantKeyHashCpuMaxNs, nullptr, nullptr, 0.0},
    {"shader_variant_key_hash_cpu_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::shaderVariantKeyHashCpuRing, 0.5},
    {"shader_variant_key_hash_cpu_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::shaderVariantKeyHashCpuRing, 0.95},
    {"shader_variant_key_hash_cpu_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::shaderVariantKeyHashCpuRing, 0.99},
    {"encode_draw_uniform_build_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawUniformBuildCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_uniform_build_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawUniformBuildCpuMaxNs, nullptr, nullptr, 0.0},
    {"encode_draw_uniform_build_cpu_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::encodeDrawUniformBuildCpuRing, 0.5},
    {"encode_draw_uniform_build_cpu_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::encodeDrawUniformBuildCpuRing, 0.95},
    {"encode_draw_uniform_build_cpu_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::encodeDrawUniformBuildCpuRing, 0.99},
    {"encode_draw_fvf_decode_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawFvfDecodeCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_fvf_decode_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawFvfDecodeCpuMaxNs, nullptr, nullptr, 0.0},
    {"encode_draw_fvf_decode_cpu_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::encodeDrawFvfDecodeCpuRing, 0.5},
    {"encode_draw_fvf_decode_cpu_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::encodeDrawFvfDecodeCpuRing, 0.95},
    {"encode_draw_fvf_decode_cpu_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::encodeDrawFvfDecodeCpuRing, 0.99},
    {"encode_draw_binding_packet_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawBindingPacketCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_binding_packet_plan_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawBindingPacketPlanCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_binding_packet_plan_fragment_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawBindingPacketPlanFragmentCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_binding_packet_plan_vertex_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawBindingPacketPlanVertexCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_binding_packet_plan_extra_stream_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawBindingPacketPlanExtraStreamCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_binding_packet_plan_raster_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawBindingPacketPlanRasterCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_binding_packet_cache_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawBindingPacketCacheCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_binding_packet_cache_key_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawBindingPacketCacheKeyCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_binding_packet_cache_hash_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawBindingPacketCacheHashCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_binding_packet_cache_probe_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawBindingPacketCacheProbeCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_binding_packet_cache_store_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawBindingPacketCacheStoreCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_binding_packet_cache_hits", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawBindingPacketCacheHits, nullptr, nullptr, 0.0},
    {"encode_draw_binding_packet_cache_misses", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawBindingPacketCacheMisses, nullptr, nullptr, 0.0},
    {"encode_draw_binding_packet_cache_collisions", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawBindingPacketCacheCollisions, nullptr, nullptr, 0.0},
    {"encode_draw_binding_packet_texture_record_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawBindingPacketTextureRecordCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_setup_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawArgbufSetupCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_open_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawArgbufOpenCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_open_call_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawArgbufOpenCallCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_reopen_post_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawArgbufReopenPostCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_reopen_table_probe_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawArgbufReopenTableProbeCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_reopen_table_shadow_store_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawArgbufReopenTableShadowStoreCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_reopen_byte_account_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawArgbufReopenByteAccountCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_reopen_cbuf_cache_probe_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawArgbufReopenCbufCacheProbeCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_reopen_cbuf_dirty_scan_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawArgbufReopenCbufDirtyScanCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_reopen_cbuf_force_dirty_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawArgbufReopenCbufForceDirtyCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_open_reserve_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawArgbufOpenReserveCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_open_set_argument_buffer_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawArgbufOpenSetArgumentBufferCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_table_bind_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawArgbufTableBindCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_table_bind_calls", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufTableBindCalls, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_table_bind_skipped", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufTableBindSkipped, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_update_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawArgbufCbufUpdateCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_update_calls", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufCbufUpdateCalls, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_update_dirty_calls", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufCbufUpdateDirtyCalls, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_update_skipped_clean", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufCbufUpdateSkippedClean, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_update_write_calls", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufCbufUpdateWriteCalls, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_build_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawArgbufCbufBuildCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_upload_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawArgbufCbufUploadCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_setbuffer_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawArgbufCbufSetBufferCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_build_vs_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawArgbufCbufBuildVsCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_build_ps_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawArgbufCbufBuildPsCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_build_ffp_vs_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawArgbufCbufBuildFfpVsCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_build_ffp_ps_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawArgbufCbufBuildFfpPsCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_upload_vs_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawArgbufCbufUploadVsCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_upload_ps_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawArgbufCbufUploadPsCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_upload_ffp_vs_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawArgbufCbufUploadFfpVsCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_upload_ffp_ps_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawArgbufCbufUploadFfpPsCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_setbuffer_vs_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawArgbufCbufSetBufferVsCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_setbuffer_ps_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawArgbufCbufSetBufferPsCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_setbuffer_ffp_vs_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawArgbufCbufSetBufferFfpVsCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_setbuffer_ffp_ps_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawArgbufCbufSetBufferFfpPsCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_upload_plan_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawArgbufCbufUploadPlanCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_upload_plan_vs_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawArgbufCbufUploadPlanVsCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_upload_plan_ps_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawArgbufCbufUploadPlanPsCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_binding_hash_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawArgbufCbufBindingHashCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_binding_hash_vs_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawArgbufCbufBindingHashVsCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_binding_hash_ps_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawArgbufCbufBindingHashPsCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_binding_hash_ffp_vs_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawArgbufCbufBindingHashFfpVsCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_binding_hash_ffp_ps_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawArgbufCbufBindingHashFfpPsCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_binding_write_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawArgbufCbufBindingWriteCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_binding_write_vs_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawArgbufCbufBindingWriteVsCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_binding_write_ps_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawArgbufCbufBindingWritePsCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_binding_write_ffp_vs_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawArgbufCbufBindingWriteFfpVsCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_binding_write_ffp_ps_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawArgbufCbufBindingWriteFfpPsCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_observer_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawArgbufCbufObserverCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_observer_vs_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawArgbufCbufObserverVsCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_observer_ps_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawArgbufCbufObserverPsCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_observer_ffp_vs_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawArgbufCbufObserverFfpVsCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_observer_ffp_ps_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawArgbufCbufObserverFfpPsCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_cache_merge_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawArgbufCbufCacheMergeCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_cached_repoint_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawArgbufCbufCachedRepointCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_full_repoint_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawArgbufCbufFullRepointCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_cached_repoint_vs_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawArgbufCbufCachedRepointVsCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_cached_repoint_ps_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawArgbufCbufCachedRepointPsCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_cached_repoint_ffp_vs_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawArgbufCbufCachedRepointFfpVsCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_cached_repoint_ffp_ps_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawArgbufCbufCachedRepointFfpPsCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_cached_repoint_calls", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufCbufCachedRepointCalls, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_cached_repoint_bytes", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufCbufCachedRepointBytes, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_cached_repoint_vs_calls", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufCbufCachedRepointVsCalls, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_cached_repoint_ps_calls", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufCbufCachedRepointPsCalls, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_cached_repoint_ffp_vs_calls", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufCbufCachedRepointFfpVsCalls, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_cached_repoint_ffp_ps_calls", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufCbufCachedRepointFfpPsCalls, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_cached_repoint_vs_bytes", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufCbufCachedRepointVsBytes, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_cached_repoint_ps_bytes", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufCbufCachedRepointPsBytes, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_cached_repoint_ffp_vs_bytes", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufCbufCachedRepointFfpVsBytes, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_cached_repoint_ffp_ps_bytes", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufCbufCachedRepointFfpPsBytes, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_content_probe_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawArgbufCbufContentProbeCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_content_probe_vs_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawArgbufCbufContentProbeVsCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_content_probe_ps_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawArgbufCbufContentProbePsCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_content_probe_ffp_ps_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawArgbufCbufContentProbeFfpPsCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_content_probe_calls", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufCbufContentProbeCalls, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_content_probe_vs_hits", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufCbufContentProbeVsHits, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_content_probe_vs_misses", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufCbufContentProbeVsMisses, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_content_probe_ps_hits", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufCbufContentProbePsHits, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_content_probe_ps_misses", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufCbufContentProbePsMisses, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_content_probe_ffp_ps_hits", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufCbufContentProbeFfpPsHits, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_content_probe_ffp_ps_misses", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufCbufContentProbeFfpPsMisses, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_reopen_full_repoint_calls", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufCbufReopenFullRepointCalls, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_reopen_no_dirty_hash_mismatch", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufCbufReopenNoDirtyHashMismatch, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_reopen_partial_candidates", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufCbufReopenPartialCandidates, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_reopen_dirty_vs", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufCbufReopenDirtyVs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_reopen_dirty_ps", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufCbufReopenDirtyPs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_reopen_dirty_ffp_vs", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufCbufReopenDirtyFfpVs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_reopen_dirty_ffp_ps", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufCbufReopenDirtyFfpPs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_update_vs_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawArgbufCbufUpdateVsCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_update_ps_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawArgbufCbufUpdatePsCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_update_ffp_vs_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawArgbufCbufUpdateFfpVsCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_update_ffp_ps_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawArgbufCbufUpdateFfpPsCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_update_vs_calls", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufCbufUpdateVsCalls, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_update_ps_calls", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufCbufUpdatePsCalls, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_update_ffp_vs_calls", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufCbufUpdateFfpVsCalls, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_update_ffp_ps_calls", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufCbufUpdateFfpPsCalls, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_update_vs_bytes", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufCbufUpdateVsBytes, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_update_ps_bytes", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufCbufUpdatePsBytes, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_update_ffp_vs_bytes", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufCbufUpdateFfpVsBytes, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_update_ffp_ps_bytes", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufCbufUpdateFfpPsBytes, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_dirty_vs_identity_probe_calls", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufCbufDirtyVsIdentityProbeCalls, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_dirty_vs_identity_hits", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufCbufDirtyVsIdentityHits, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_dirty_vs_identity_misses", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufCbufDirtyVsIdentityMisses, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_dirty_vs_identity_no_cache", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufCbufDirtyVsIdentityNoCache, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_dirty_vs_identity_hit_bytes", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufCbufDirtyVsIdentityHitBytes, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_cbuf_dirty_vs_identity_miss_bytes", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufCbufDirtyVsIdentityMissBytes, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_payload_delta_probe_calls", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufPayloadDeltaProbeCalls, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_payload_delta_first", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufPayloadDeltaFirst, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_payload_delta_same", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufPayloadDeltaSame, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_payload_delta_changed", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufPayloadDeltaChanged, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_payload_delta_changed_vs", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufPayloadDeltaChangedVs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_payload_delta_changed_ps", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufPayloadDeltaChangedPs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_payload_delta_changed_vs_ps", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufPayloadDeltaChangedVsPs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_payload_delta_changed_nonconst_only", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufPayloadDeltaChangedNonConstOnly, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_payload_delta_changed_vs_float", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufPayloadDeltaChangedVsFloat, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_payload_delta_changed_vs_int", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufPayloadDeltaChangedVsInt, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_payload_delta_changed_vs_bool", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufPayloadDeltaChangedVsBool, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_payload_delta_changed_ps_float", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufPayloadDeltaChangedPsFloat, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_payload_delta_changed_ps_int", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufPayloadDeltaChangedPsInt, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_payload_delta_changed_ps_bool", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufPayloadDeltaChangedPsBool, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_payload_delta_changed_vs_float_regs", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufPayloadDeltaChangedVsFloatRegs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_payload_delta_changed_vs_float_regs_max", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufPayloadDeltaChangedVsFloatRegsMax, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_payload_delta_changed_vs_float_regs_le1", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufPayloadDeltaChangedVsFloatRegsLe1, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_payload_delta_changed_vs_float_regs_le4", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufPayloadDeltaChangedVsFloatRegsLe4, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_payload_delta_changed_vs_float_regs_le16", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufPayloadDeltaChangedVsFloatRegsLe16, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_payload_delta_changed_vs_float_regs_le64", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufPayloadDeltaChangedVsFloatRegsLe64, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_payload_delta_changed_vs_float_regs_gt64", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufPayloadDeltaChangedVsFloatRegsGt64, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_payload_delta_changed_vs_float_regs_le1_sum", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufPayloadDeltaChangedVsFloatRegsLe1Sum, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_payload_delta_changed_vs_float_regs_le4_sum", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufPayloadDeltaChangedVsFloatRegsLe4Sum, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_payload_delta_changed_vs_float_regs_le16_sum", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufPayloadDeltaChangedVsFloatRegsLe16Sum, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_payload_delta_changed_vs_float_regs_le64_sum", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufPayloadDeltaChangedVsFloatRegsLe64Sum, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_payload_delta_changed_vs_float_regs_gt64_sum", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufPayloadDeltaChangedVsFloatRegsGt64Sum, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_payload_delta_changed_vs_float_prefix_regs", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufPayloadDeltaChangedVsFloatPrefixRegs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_payload_delta_changed_vs_float_prefix_regs_max", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufPayloadDeltaChangedVsFloatPrefixRegsMax, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_payload_delta_changed_vs_float_span_regs", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufPayloadDeltaChangedVsFloatSpanRegs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_payload_delta_changed_vs_float_span_regs_max", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufPayloadDeltaChangedVsFloatSpanRegsMax, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_payload_delta_changed_vs_float_full_prefix", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufPayloadDeltaChangedVsFloatFullPrefix, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_payload_delta_changed_vs_float_full_prefix_regs", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufPayloadDeltaChangedVsFloatFullPrefixRegs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_payload_delta_changed_ps_float_regs", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufPayloadDeltaChangedPsFloatRegs, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_payload_delta_changed_ps_float_regs_max", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufPayloadDeltaChangedPsFloatRegsMax, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_payload_delta_changed_ps_float_regs_le1", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufPayloadDeltaChangedPsFloatRegsLe1, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_payload_delta_changed_ps_float_regs_le4", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufPayloadDeltaChangedPsFloatRegsLe4, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_payload_delta_changed_ps_float_regs_le16", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufPayloadDeltaChangedPsFloatRegsLe16, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_payload_delta_changed_ps_float_regs_le64", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufPayloadDeltaChangedPsFloatRegsLe64, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_payload_delta_changed_ps_float_regs_gt64", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufPayloadDeltaChangedPsFloatRegsGt64, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_payload_delta_changed_ps_float_regs_le1_sum", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufPayloadDeltaChangedPsFloatRegsLe1Sum, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_payload_delta_changed_ps_float_regs_le4_sum", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufPayloadDeltaChangedPsFloatRegsLe4Sum, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_payload_delta_changed_ps_float_regs_le16_sum", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufPayloadDeltaChangedPsFloatRegsLe16Sum, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_payload_delta_changed_ps_float_regs_le64_sum", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufPayloadDeltaChangedPsFloatRegsLe64Sum, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_payload_delta_changed_ps_float_regs_gt64_sum", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufPayloadDeltaChangedPsFloatRegsGt64Sum, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_payload_delta_reopen_first", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufPayloadDeltaReopenFirst, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_payload_delta_reopen_payload_changed", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufPayloadDeltaReopenPayloadChanged, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_payload_delta_reopen_payload_same", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufPayloadDeltaReopenPayloadSame, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_payload_delta_reopen_resource_array", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufPayloadDeltaReopenResourceArray, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_payload_delta_reopen_cbuf_only", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufPayloadDeltaReopenCbufOnly, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_payload_delta_reopen_cbuf_only_first", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufPayloadDeltaReopenCbufOnlyFirst, nullptr, nullptr, 0.0},
    {"encode_draw_argbuf_payload_delta_reopen_cbuf_only_payload_changed", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawArgbufPayloadDeltaReopenCbufOnlyPayloadChanged, nullptr, nullptr, 0.0},
    {"encode_draw_stream_bind_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawStreamBindCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_stream_bind_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawStreamBindCpuMaxNs, nullptr, nullptr, 0.0},
    {"encode_draw_stream_bind_cpu_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::encodeDrawStreamBindCpuRing, 0.5},
    {"encode_draw_stream_bind_cpu_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::encodeDrawStreamBindCpuRing, 0.95},
    {"encode_draw_stream_bind_cpu_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::encodeDrawStreamBindCpuRing, 0.99},
    {"encode_draw_stream_bind_raster_phase_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawStreamBindRasterPhaseCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_stream_bind_raster_phase_calls", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawStreamBindRasterPhaseCalls, nullptr, nullptr, 0.0},
    {"encode_draw_stream_bind_ffp_stream_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawStreamBindFfpStreamCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_stream_bind_ffp_stream_calls", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawStreamBindFfpStreamCalls, nullptr, nullptr, 0.0},
    {"encode_draw_stream_bind_shader_stream_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawStreamBindShaderStreamCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_stream_bind_shader_stream_calls", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawStreamBindShaderStreamCalls, nullptr, nullptr, 0.0},
    {"encode_draw_stream_bind_texture_phase_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawStreamBindTexturePhaseCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_stream_bind_texture_phase_calls", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawStreamBindTexturePhaseCalls, nullptr, nullptr, 0.0},
    {"encode_draw_stream_bind_index_phase_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawStreamBindIndexPhaseCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_stream_bind_index_phase_calls", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawStreamBindIndexPhaseCalls, nullptr, nullptr, 0.0},
    {"encode_draw_texture_sampler_fragment_resolve_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawTextureSamplerFragmentResolveCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_texture_sampler_fragment_resolve_calls", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawTextureSamplerFragmentResolveCalls, nullptr, nullptr, 0.0},
    {"encode_draw_texture_sampler_fragment_resolve_texture_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawTextureSamplerFragmentResolveTextureCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_texture_sampler_fragment_resolve_texture_calls", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawTextureSamplerFragmentResolveTextureCalls, nullptr, nullptr, 0.0},
    {"encode_draw_texture_sampler_fragment_resource_array_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawTextureSamplerFragmentResourceArrayCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_texture_sampler_fragment_resource_array_calls", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawTextureSamplerFragmentResourceArrayCalls, nullptr, nullptr, 0.0},
    {"encode_draw_texture_sampler_fragment_direct_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawTextureSamplerFragmentDirectCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_texture_sampler_fragment_direct_calls", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawTextureSamplerFragmentDirectCalls, nullptr, nullptr, 0.0},
    {"encode_draw_texture_sampler_fragment_direct_texture_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawTextureSamplerFragmentDirectTextureCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_texture_sampler_fragment_direct_texture_calls", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawTextureSamplerFragmentDirectTextureCalls, nullptr, nullptr, 0.0},
    {"encode_draw_texture_sampler_fragment_direct_texture_set_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawTextureSamplerFragmentDirectTextureSetCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_texture_sampler_fragment_direct_texture_set_calls", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawTextureSamplerFragmentDirectTextureSetCalls, nullptr, nullptr, 0.0},
    {"encode_draw_texture_sampler_fragment_direct_sampler_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawTextureSamplerFragmentDirectSamplerCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_texture_sampler_fragment_direct_sampler_calls", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawTextureSamplerFragmentDirectSamplerCalls, nullptr, nullptr, 0.0},
    {"encode_draw_texture_sampler_fragment_direct_sampler_set_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawTextureSamplerFragmentDirectSamplerSetCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_texture_sampler_fragment_direct_sampler_set_calls", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawTextureSamplerFragmentDirectSamplerSetCalls, nullptr, nullptr, 0.0},
    {"encode_draw_texture_sampler_sampler_lookup_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawTextureSamplerSamplerLookupCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_texture_sampler_sampler_lookup_calls", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawTextureSamplerSamplerLookupCalls, nullptr, nullptr, 0.0},
    {"encode_draw_texture_sampler_sampler_lookup_skipped_prehandle", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawTextureSamplerSamplerLookupSkippedPrehandle, nullptr, nullptr, 0.0},
    {"encode_draw_texture_sampler_lod_bias_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawTextureSamplerLodBiasCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_texture_sampler_lod_bias_calls", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawTextureSamplerLodBiasCalls, nullptr, nullptr, 0.0},
    {"encode_draw_texture_sampler_vertex_resolve_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawTextureSamplerVertexResolveCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_texture_sampler_vertex_resolve_calls", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawTextureSamplerVertexResolveCalls, nullptr, nullptr, 0.0},
    {"encode_draw_texture_sampler_vertex_direct_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawTextureSamplerVertexDirectCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_texture_sampler_vertex_direct_calls", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawTextureSamplerVertexDirectCalls, nullptr, nullptr, 0.0},
    {"encode_draw_raster_state_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawRasterStateCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_vertex_stream_bind_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawVertexStreamBindCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_texture_sampler_bind_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawTextureSamplerBindCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_index_setup_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawIndexSetupCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_index_source_resolve_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawIndexSourceResolveCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_index_cache_lookup_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawIndexCacheLookupCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_index_cache_candidate_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawIndexCacheCandidateCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_index_cache_original_measure_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawIndexCacheOriginalMeasureCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_index_cache_candidate_build_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawIndexCacheCandidateBuildCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_index_cache_candidate_read_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawIndexCacheCandidateReadCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_index_cache_candidate_adjacency_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawIndexCacheCandidateAdjacencyCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_index_cache_candidate_select_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawIndexCacheCandidateSelectCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_index_cache_candidate_write_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawIndexCacheCandidateWriteCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_index_cache_candidate_select_calls", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawIndexCacheCandidateSelectCalls, nullptr, nullptr, 0.0},
    {"encode_draw_index_cache_candidate_select_slots", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawIndexCacheCandidateSelectSlots, nullptr, nullptr, 0.0},
    {"encode_draw_index_cache_candidate_select_scored", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawIndexCacheCandidateSelectScored, nullptr, nullptr, 0.0},
    {"encode_draw_index_cache_candidate_select_skipped", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawIndexCacheCandidateSelectSkipped, nullptr, nullptr, 0.0},
    {"encode_draw_index_cache_candidate_select_candidates_max", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawIndexCacheCandidateSelectCandidatesMax, nullptr, nullptr, 0.0},
    {"encode_draw_index_cache_candidate_frontier_dropped", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawIndexCacheCandidateFrontierDropped, nullptr, nullptr, 0.0},
    {"encode_draw_index_cache_candidate_lazy_heap_pops", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawIndexCacheCandidateLazyHeapPops, nullptr, nullptr, 0.0},
    {"encode_draw_index_cache_candidate_lazy_refreshes", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawIndexCacheCandidateLazyRefreshes, nullptr, nullptr, 0.0},
    {"encode_draw_index_cache_candidate_lazy_stale_drops", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawIndexCacheCandidateLazyStaleDrops, nullptr, nullptr, 0.0},
    {"encode_draw_index_cache_candidate_lazy_accepted", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawIndexCacheCandidateLazyAccepted, nullptr, nullptr, 0.0},
    {"encode_draw_index_cache_candidate_bucket_vertex_visits", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawIndexCacheCandidateBucketVertexVisits, nullptr, nullptr, 0.0},
    {"encode_draw_index_cache_candidate_bucket_moves", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawIndexCacheCandidateBucketMoves, nullptr, nullptr, 0.0},
    {"encode_draw_index_cache_candidate_bucket_selected", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawIndexCacheCandidateBucketSelected, nullptr, nullptr, 0.0},
    {"encode_draw_index_cache_candidate_upper_bound_rejected", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawIndexCacheCandidateUpperBoundRejected, nullptr, nullptr, 0.0},
    {"encode_draw_index_cache_candidate_measure_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawIndexCacheCandidateMeasureCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_index_cache_gate_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawIndexCacheGateCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_index_cache_apply_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawIndexCacheApplyCpuNs, nullptr, nullptr, 0.0},
    {"compatible_indexed_draw_merge_pair_attempts", CounterEntry::Kind::UnsignedCount, &Counters::compatibleIndexedDrawMergePairAttempts, nullptr, nullptr, 0.0},
    {"compatible_indexed_draw_merge_compatible_pairs", CounterEntry::Kind::UnsignedCount, &Counters::compatibleIndexedDrawMergeCompatiblePairs, nullptr, nullptr, 0.0},
    {"compatible_indexed_draw_merge_multiple_reject_pairs", CounterEntry::Kind::UnsignedCount, &Counters::compatibleIndexedDrawMergeMultipleRejectPairs, nullptr, nullptr, 0.0},
    {"compatible_indexed_draw_merge_selected_pairs", CounterEntry::Kind::UnsignedCount, &Counters::compatibleIndexedDrawMergeSelectedPairs, nullptr, nullptr, 0.0},
    {"compatible_indexed_draw_merge_reject_source_shape", CounterEntry::Kind::UnsignedCount, &Counters::compatibleIndexedDrawMergeRejectSourceShape, nullptr, nullptr, 0.0},
    {"compatible_indexed_draw_merge_reject_next_shape", CounterEntry::Kind::UnsignedCount, &Counters::compatibleIndexedDrawMergeRejectNextShape, nullptr, nullptr, 0.0},
    {"compatible_indexed_draw_merge_reject_index_type", CounterEntry::Kind::UnsignedCount, &Counters::compatibleIndexedDrawMergeRejectIndexType, nullptr, nullptr, 0.0},
    {"compatible_indexed_draw_merge_reject_base_vertex", CounterEntry::Kind::UnsignedCount, &Counters::compatibleIndexedDrawMergeRejectBaseVertex, nullptr, nullptr, 0.0},
    {"compatible_indexed_draw_merge_reject_start_vertex", CounterEntry::Kind::UnsignedCount, &Counters::compatibleIndexedDrawMergeRejectStartVertex, nullptr, nullptr, 0.0},
    {"compatible_indexed_draw_merge_reject_uniform", CounterEntry::Kind::UnsignedCount, &Counters::compatibleIndexedDrawMergeRejectUniform, nullptr, nullptr, 0.0},
    {"compatible_indexed_draw_merge_reject_binding_override", CounterEntry::Kind::UnsignedCount, &Counters::compatibleIndexedDrawMergeRejectBindingOverride, nullptr, nullptr, 0.0},
    {"compatible_indexed_draw_merge_reject_binding_snapshot", CounterEntry::Kind::UnsignedCount, &Counters::compatibleIndexedDrawMergeRejectBindingSnapshot, nullptr, nullptr, 0.0},
    {"compatible_indexed_draw_merge_reject_noncontiguous", CounterEntry::Kind::UnsignedCount, &Counters::compatibleIndexedDrawMergeRejectNonContiguous, nullptr, nullptr, 0.0},
    {"compatible_indexed_draw_merge_reject_overflow", CounterEntry::Kind::UnsignedCount, &Counters::compatibleIndexedDrawMergeRejectOverflow, nullptr, nullptr, 0.0},
    {"compatible_indexed_draw_merge_only_source_shape", CounterEntry::Kind::UnsignedCount, &Counters::compatibleIndexedDrawMergeOnlySourceShape, nullptr, nullptr, 0.0},
    {"compatible_indexed_draw_merge_only_next_shape", CounterEntry::Kind::UnsignedCount, &Counters::compatibleIndexedDrawMergeOnlyNextShape, nullptr, nullptr, 0.0},
    {"compatible_indexed_draw_merge_only_index_type", CounterEntry::Kind::UnsignedCount, &Counters::compatibleIndexedDrawMergeOnlyIndexType, nullptr, nullptr, 0.0},
    {"compatible_indexed_draw_merge_only_base_vertex", CounterEntry::Kind::UnsignedCount, &Counters::compatibleIndexedDrawMergeOnlyBaseVertex, nullptr, nullptr, 0.0},
    {"compatible_indexed_draw_merge_only_start_vertex", CounterEntry::Kind::UnsignedCount, &Counters::compatibleIndexedDrawMergeOnlyStartVertex, nullptr, nullptr, 0.0},
    {"compatible_indexed_draw_merge_only_uniform", CounterEntry::Kind::UnsignedCount, &Counters::compatibleIndexedDrawMergeOnlyUniform, nullptr, nullptr, 0.0},
    {"compatible_indexed_draw_merge_only_binding_override", CounterEntry::Kind::UnsignedCount, &Counters::compatibleIndexedDrawMergeOnlyBindingOverride, nullptr, nullptr, 0.0},
    {"compatible_indexed_draw_merge_only_binding_snapshot", CounterEntry::Kind::UnsignedCount, &Counters::compatibleIndexedDrawMergeOnlyBindingSnapshot, nullptr, nullptr, 0.0},
    {"compatible_indexed_draw_merge_only_noncontiguous", CounterEntry::Kind::UnsignedCount, &Counters::compatibleIndexedDrawMergeOnlyNonContiguous, nullptr, nullptr, 0.0},
    {"compatible_indexed_draw_merge_only_overflow", CounterEntry::Kind::UnsignedCount, &Counters::compatibleIndexedDrawMergeOnlyOverflow, nullptr, nullptr, 0.0},
    {"compatible_indexed_draw_merge_exact_relax_binding_payload_pairs", CounterEntry::Kind::UnsignedCount, &Counters::compatibleIndexedDrawMergeExactRelaxBindingPayload, nullptr, nullptr, 0.0},
    {"compatible_indexed_draw_merge_exact_relax_uniform_pairs", CounterEntry::Kind::UnsignedCount, &Counters::compatibleIndexedDrawMergeExactRelaxUniform, nullptr, nullptr, 0.0},
    {"compatible_indexed_draw_merge_exact_relax_binding_payload_uniform_pairs", CounterEntry::Kind::UnsignedCount, &Counters::compatibleIndexedDrawMergeExactRelaxBindingPayloadUniform, nullptr, nullptr, 0.0},
    {"compatible_indexed_draw_merge_exact_relax_noncontiguous_pairs", CounterEntry::Kind::UnsignedCount, &Counters::compatibleIndexedDrawMergeExactRelaxNonContiguous, nullptr, nullptr, 0.0},
    {"compatible_indexed_draw_merge_exact_relax_binding_payload_noncontiguous_pairs", CounterEntry::Kind::UnsignedCount, &Counters::compatibleIndexedDrawMergeExactRelaxBindingPayloadNonContiguous, nullptr, nullptr, 0.0},
    {"compatible_indexed_draw_merge_exact_relax_uniform_noncontiguous_pairs", CounterEntry::Kind::UnsignedCount, &Counters::compatibleIndexedDrawMergeExactRelaxUniformNonContiguous, nullptr, nullptr, 0.0},
    {"compatible_indexed_draw_merge_exact_relax_binding_payload_uniform_noncontiguous_pairs", CounterEntry::Kind::UnsignedCount, &Counters::compatibleIndexedDrawMergeExactRelaxBindingPayloadUniformNonContiguous, nullptr, nullptr, 0.0},
    {"compatible_indexed_draw_merge_exact_relax_other_pairs", CounterEntry::Kind::UnsignedCount, &Counters::compatibleIndexedDrawMergeExactRelaxOther, nullptr, nullptr, 0.0},
    {"indexed_cache_opt_candidate_draws", CounterEntry::Kind::UnsignedCount, &Counters::indexedCacheOptCandidateDraws, nullptr, nullptr, 0.0},
    {"indexed_cache_opt_candidate_skipped", CounterEntry::Kind::UnsignedCount, &Counters::indexedCacheOptCandidateSkipped, nullptr, nullptr, 0.0},
    {"indexed_cache_opt_candidate_bytes", CounterEntry::Kind::UnsignedCount, &Counters::indexedCacheOptCandidateBytes, nullptr, nullptr, 0.0},
    {"indexed_cache_opt_candidate_original_miss16", CounterEntry::Kind::UnsignedCount, &Counters::indexedCacheOptCandidateOriginalMiss16, nullptr, nullptr, 0.0},
    {"indexed_cache_opt_candidate_original_miss32", CounterEntry::Kind::UnsignedCount, &Counters::indexedCacheOptCandidateOriginalMiss32, nullptr, nullptr, 0.0},
    {"indexed_cache_opt_candidate_original_miss64", CounterEntry::Kind::UnsignedCount, &Counters::indexedCacheOptCandidateOriginalMiss64, nullptr, nullptr, 0.0},
    {"indexed_cache_opt_candidate_miss16", CounterEntry::Kind::UnsignedCount, &Counters::indexedCacheOptCandidateMiss16, nullptr, nullptr, 0.0},
    {"indexed_cache_opt_candidate_miss32", CounterEntry::Kind::UnsignedCount, &Counters::indexedCacheOptCandidateMiss32, nullptr, nullptr, 0.0},
    {"indexed_cache_opt_candidate_miss64", CounterEntry::Kind::UnsignedCount, &Counters::indexedCacheOptCandidateMiss64, nullptr, nullptr, 0.0},
    {"indexed_cache_opt_candidate_gate_pass", CounterEntry::Kind::UnsignedCount, &Counters::indexedCacheOptCandidateGatePass, nullptr, nullptr, 0.0},
    {"indexed_cache_opt_candidate_gate_fail", CounterEntry::Kind::UnsignedCount, &Counters::indexedCacheOptCandidateGateFail, nullptr, nullptr, 0.0},
    {"indexed_cache_opt_candidate_opaque_depth_draws", CounterEntry::Kind::UnsignedCount, &Counters::indexedCacheOptCandidateOpaqueDepthDraws, nullptr, nullptr, 0.0},
    {"indexed_cache_opt_candidate_screen_blend_draws", CounterEntry::Kind::UnsignedCount, &Counters::indexedCacheOptCandidateScreenBlendDraws, nullptr, nullptr, 0.0},
    {"indexed_cache_opt_candidate_primitive_bucket_1_63", CounterEntry::Kind::UnsignedCount, &Counters::indexedCacheOptCandidatePrimitiveBucket1_63, nullptr, nullptr, 0.0},
    {"indexed_cache_opt_candidate_primitive_bucket_64_255", CounterEntry::Kind::UnsignedCount, &Counters::indexedCacheOptCandidatePrimitiveBucket64_255, nullptr, nullptr, 0.0},
    {"indexed_cache_opt_candidate_primitive_bucket_256_1023", CounterEntry::Kind::UnsignedCount, &Counters::indexedCacheOptCandidatePrimitiveBucket256_1023, nullptr, nullptr, 0.0},
    {"indexed_cache_opt_candidate_primitive_bucket_1024_4095", CounterEntry::Kind::UnsignedCount, &Counters::indexedCacheOptCandidatePrimitiveBucket1024_4095, nullptr, nullptr, 0.0},
    {"indexed_cache_opt_candidate_primitive_bucket_4096_plus", CounterEntry::Kind::UnsignedCount, &Counters::indexedCacheOptCandidatePrimitiveBucket4096Plus, nullptr, nullptr, 0.0},
    {"reordered_index_cache_lookups", CounterEntry::Kind::UnsignedCount, &Counters::reorderedIndexCacheLookups, nullptr, nullptr, 0.0},
    {"reordered_index_cache_hits", CounterEntry::Kind::UnsignedCount, &Counters::reorderedIndexCacheHits, nullptr, nullptr, 0.0},
    {"reordered_index_cache_rejected_hits", CounterEntry::Kind::UnsignedCount, &Counters::reorderedIndexCacheRejectedHits, nullptr, nullptr, 0.0},
    {"reordered_index_cache_misses", CounterEntry::Kind::UnsignedCount, &Counters::reorderedIndexCacheMisses, nullptr, nullptr, 0.0},
    {"reordered_index_cache_created", CounterEntry::Kind::UnsignedCount, &Counters::reorderedIndexCacheCreated, nullptr, nullptr, 0.0},
    {"reordered_index_cache_created_bytes", CounterEntry::Kind::UnsignedCount, &Counters::reorderedIndexCacheCreatedBytes, nullptr, nullptr, 0.0},
    {"encode_draw_issue_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawIssueCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_stream_bind_viewport_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawStreamBindViewportCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_stream_bind_ffp_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawStreamBindFfpCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_stream_bind_vs_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawStreamBindVsCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_stream_bind_texture_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawStreamBindTextureCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_stream_bind_index_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawStreamBindIndexCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_fvf_decode_decl_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawFvfDecodeDeclCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_fvf_decode_bytes_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawFvfDecodeBytesCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_fvf_decode_expanded_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawFvfDecodeExpandedCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_uniform_build_main_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawUniformBuildMainCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_uniform_build_ffp_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawUniformBuildFfpCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_uniform_build_vs_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawUniformBuildVsCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_phase_setup_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawPhaseSetupCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_phase_argbuf_uniform_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawPhaseArgbufUniformCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_phase_stream_prep_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawPhaseStreamPrepCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_phase_ffp_vertex_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawPhaseFfpVertexCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_phase_vertex_bind_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawPhaseVertexBindCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_phase_base_state_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawPhaseBaseStateCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_phase_tile_ffp_fallthrough_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawPhaseTileFfpFallthroughCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_phase_remainder_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawPhaseRemainderCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_issue_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawIssueCpuMaxNs, nullptr, nullptr, 0.0},
    {"encode_draw_issue_cpu_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::encodeDrawIssueCpuRing, 0.5},
    {"encode_draw_issue_cpu_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::encodeDrawIssueCpuRing, 0.95},
    {"encode_draw_issue_cpu_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::encodeDrawIssueCpuRing, 0.99},
    {"encode_draw_issue_indexed_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawIssueIndexedCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_issue_nonindexed_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawIssueNonIndexedCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_issue_expanded_indexed_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawIssueExpandedIndexedCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_issue_split_indexed_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawIssueSplitIndexedCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_issue_metal_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawIssueMetalCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_issue_visibility_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawIssueVisibilityCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_pso_prefetch_handle_available", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawPsoPrefetchHandleAvailable, nullptr, nullptr, 0.0},
    {"encode_draw_pso_prefetch_handle_used", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawPsoPrefetchHandleUsed, nullptr, nullptr, 0.0},
    {"encode_draw_pso_prefetch_handle_missing", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawPsoPrefetchHandleMissing, nullptr, nullptr, 0.0},
    {"encode_draw_pso_prefetch_bypass_probe", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawPsoPrefetchBypassProbe, nullptr, nullptr, 0.0},
    {"encode_draw_pso_prefetch_bypass_binding_override", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawPsoPrefetchBypassBindingOverride, nullptr, nullptr, 0.0},
    {"encode_draw_pso_prefetch_binding_override", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawPsoPrefetchBindingOverride, nullptr, nullptr, 0.0},
    {"encode_draw_pso_prefetch_binding_override_compatible", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawPsoPrefetchBindingOverrideCompatible, nullptr, nullptr, 0.0},
    {"encode_draw_pso_prefetch_binding_override_incompatible", CounterEntry::Kind::UnsignedCount, &Counters::encodeDrawPsoPrefetchBindingOverrideIncompatible, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_draw_submission_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotDrawSubmissionCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_draw_submission_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotDrawSubmissionCpuMaxNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_draw_submission_cpu_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::d3d9SnapshotDrawSubmissionCpuRing, 0.5},
    {"d3d9_snapshot_draw_submission_cpu_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::d3d9SnapshotDrawSubmissionCpuRing, 0.95},
    {"d3d9_snapshot_draw_submission_cpu_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::d3d9SnapshotDrawSubmissionCpuRing, 0.99},
    {"d3d9_snapshot_cache_lookup_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotCacheLookupCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_hit_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotCacheHitCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_miss_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotCacheMissCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_direct_hit_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotCacheDirectHitCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_direct_miss_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotCacheDirectMissCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_hit_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotCacheBatchHitCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotCacheBatchMissCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_binding_layout_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotCacheBindingLayoutCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_uniform_refresh_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotCacheUniformRefreshCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_uniform_build_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotCacheUniformBuildCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_uniform_hash_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotCacheUniformHashCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_miss_shader_layout_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotCacheMissShaderLayoutCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_miss_uniform_build_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotCacheMissUniformBuildCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_miss_hot_build_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotCacheMissHotBuildCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_direct_miss_shader_layout_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotCacheDirectMissShaderLayoutCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_direct_miss_uniform_build_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotCacheDirectMissUniformBuildCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_direct_miss_hot_build_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotCacheDirectMissHotBuildCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_shader_layout_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotCacheBatchMissShaderLayoutCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_shader_layout_compatible_hits", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotCacheBatchMissShaderLayoutCompatibleHits, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_shader_layout_compatible_misses", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotCacheBatchMissShaderLayoutCompatibleMisses, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_shader_layout_reuse_hits", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotCacheBatchMissShaderLayoutReuseHits, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_shader_layout_reuse_misses", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotCacheBatchMissShaderLayoutReuseMisses, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_uniform_build_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotCacheBatchMissUniformBuildCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_hot_build_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotCacheBatchMissHotBuildCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_hot_build_zero_init_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotCacheBatchMissHotBuildZeroInitCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_hot_build_key_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotCacheBatchMissHotBuildKeyCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_hot_build_key_zero_init_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotCacheBatchMissHotBuildKeyZeroInitCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_hot_build_key_stream_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotCacheBatchMissHotBuildKeyStreamCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_hot_build_key_shader_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotCacheBatchMissHotBuildKeyShaderCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_hot_build_key_constant_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotCacheBatchMissHotBuildKeyConstantCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_hot_build_key_texture_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotCacheBatchMissHotBuildKeyTextureCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_hot_build_key_sampler_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotCacheBatchMissHotBuildKeySamplerCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_hot_build_key_render_state_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotCacheBatchMissHotBuildKeyRenderStateCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_hot_build_key_attachment_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotCacheBatchMissHotBuildKeyAttachmentCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_hot_build_key_uniform_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotCacheBatchMissHotBuildKeyUniformCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_hot_build_binding_copy_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotCacheBatchMissHotBuildBindingCopyCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_hot_build_render_state_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotCacheBatchMissHotBuildRenderStateCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_hot_build_texture_stage_state_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotCacheBatchMissHotBuildTextureStageStateCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_hot_build_sampler_state_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotCacheBatchMissHotBuildSamplerStateCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_hot_build_tail_copy_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotCacheBatchMissHotBuildTailCopyCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_hot_build_flat_render_reuse_hits", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotCacheBatchMissHotBuildFlatRenderReuseHits, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_hot_build_flat_render_reuse_misses", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotCacheBatchMissHotBuildFlatRenderReuseMisses, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_hot_build_flat_tss_reuse_hits", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotCacheBatchMissHotBuildFlatTssReuseHits, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_hot_build_flat_tss_reuse_misses", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotCacheBatchMissHotBuildFlatTssReuseMisses, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_hot_build_flat_sampler_reuse_hits", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotCacheBatchMissHotBuildFlatSamplerReuseHits, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_hot_build_flat_sampler_reuse_misses", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotCacheBatchMissHotBuildFlatSamplerReuseMisses, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_uniform_nonconst_hash_reuse_hits", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotCacheBatchMissUniformNonConstHashReuseHits, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_uniform_nonconst_hash_reuse_misses", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotCacheBatchMissUniformNonConstHashReuseMisses, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_uniform_payload_reuse_full", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotCacheBatchMissUniformPayloadReuseFull, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_uniform_payload_reuse_nonconst", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotCacheBatchMissUniformPayloadReuseNonConst, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_uniform_payload_full_build", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotCacheBatchMissUniformPayloadFullBuild, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_uniform_vs_const_hash_reuse", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotCacheBatchMissUniformVsConstHashReuse, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_uniform_vs_const_hash_build", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotCacheBatchMissUniformVsConstHashBuild, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_uniform_ps_const_hash_reuse", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotCacheBatchMissUniformPsConstHashReuse, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_uniform_ps_const_hash_build", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotCacheBatchMissUniformPsConstHashBuild, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_uniform_vs_const_hash_memo_probe", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotCacheBatchMissUniformVsConstHashMemoProbe, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_uniform_vs_const_hash_memo_hits", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotCacheBatchMissUniformVsConstHashMemoHits, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_uniform_vs_const_hash_memo_misses", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotCacheBatchMissUniformVsConstHashMemoMisses, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_uniform_vs_const_hash_memo_stores", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotCacheBatchMissUniformVsConstHashMemoStores, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_uniform_ps_const_hash_memo_probe", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotCacheBatchMissUniformPsConstHashMemoProbe, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_uniform_ps_const_hash_memo_hits", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotCacheBatchMissUniformPsConstHashMemoHits, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_uniform_ps_const_hash_memo_misses", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotCacheBatchMissUniformPsConstHashMemoMisses, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_uniform_ps_const_hash_memo_stores", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotCacheBatchMissUniformPsConstHashMemoStores, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_build_calls", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotUniformBuildCalls, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_build_vs_const_copy_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotUniformBuildVsConstCopyCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_build_ps_const_copy_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotUniformBuildPsConstCopyCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_build_ffp_matrix_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotUniformBuildFfpMatrixCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_build_ffp_material_light_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotUniformBuildFfpMaterialLightCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_build_texture_transform_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotUniformBuildTextureTransformCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_build_clip_plane_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotUniformBuildClipPlaneCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_build_hash_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotUniformBuildHashCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_build_vs_const_hash_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotUniformBuildVsConstHashCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_build_ps_const_hash_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotUniformBuildPsConstHashCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_build_nonconst_hash_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotUniformBuildNonConstHashCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_build_nonconst_hash_world_view_proj_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotUniformBuildNonConstHashWorldViewProjCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_build_nonconst_hash_ffp_world_view_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotUniformBuildNonConstHashFfpWorldViewCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_build_nonconst_hash_ffp_normal_matrix_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotUniformBuildNonConstHashFfpNormalMatrixCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_build_nonconst_hash_material_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotUniformBuildNonConstHashMaterialCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_build_nonconst_hash_lights_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotUniformBuildNonConstHashLightsCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_build_nonconst_hash_ffp_blend_wvp_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotUniformBuildNonConstHashFfpBlendWvpCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_build_nonconst_hash_texture_transforms_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotUniformBuildNonConstHashTextureTransformsCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_build_nonconst_hash_clip_planes_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotUniformBuildNonConstHashClipPlanesCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_build_payload_combine_hash_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotUniformBuildPayloadCombineHashCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_build_vs_const_hash_full", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotUniformBuildVsConstHashFull, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_build_ps_const_hash_full", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotUniformBuildPsConstHashFull, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_build_vs_const_hash_full_no_usage", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotUniformBuildVsConstHashFullNoUsage, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_build_ps_const_hash_full_no_usage", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotUniformBuildPsConstHashFullNoUsage, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_build_vs_const_hash_full_unknown", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotUniformBuildVsConstHashFullUnknown, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_build_ps_const_hash_full_unknown", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotUniformBuildPsConstHashFullUnknown, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_build_vs_const_hash_full_unknown_bytecode", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotUniformBuildVsConstHashFullUnknownBytecode, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_build_ps_const_hash_full_unknown_bytecode", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotUniformBuildPsConstHashFullUnknownBytecode, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_build_vs_const_hash_full_unknown_non_bytecode", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotUniformBuildVsConstHashFullUnknownNonBytecode, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_build_ps_const_hash_full_unknown_non_bytecode", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotUniformBuildPsConstHashFullUnknownNonBytecode, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_build_vs_const_hash_full_indexed_float", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotUniformBuildVsConstHashFullIndexedFloat, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_build_vs_const_hash_full_indexed_float_min_safe_bytes", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotUniformBuildVsConstHashFullIndexedFloatMinSafeBytes, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_build_vs_const_hash_full_indexed_float_potential_saved_bytes", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotUniformBuildVsConstHashFullIndexedFloatPotentialSavedBytes, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_build_ps_const_hash_full_indexed_float", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotUniformBuildPsConstHashFullIndexedFloat, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_build_vs_const_hash_full_indexed_int", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotUniformBuildVsConstHashFullIndexedInt, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_build_ps_const_hash_full_indexed_int", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotUniformBuildPsConstHashFullIndexedInt, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_build_vs_const_hash_full_indexed_bool", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotUniformBuildVsConstHashFullIndexedBool, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_build_ps_const_hash_full_indexed_bool", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotUniformBuildPsConstHashFullIndexedBool, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_build_vs_const_hash_bytes", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotUniformBuildVsConstHashBytes, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_build_ps_const_hash_bytes", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotUniformBuildPsConstHashBytes, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_uniform_build_calls", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotCacheBatchMissUniformBuildCalls, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_uniform_build_vs_const_copy_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotCacheBatchMissUniformBuildVsConstCopyCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_uniform_build_ps_const_copy_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotCacheBatchMissUniformBuildPsConstCopyCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_uniform_build_ffp_matrix_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotCacheBatchMissUniformBuildFfpMatrixCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_uniform_build_ffp_material_light_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotCacheBatchMissUniformBuildFfpMaterialLightCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_uniform_build_texture_transform_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotCacheBatchMissUniformBuildTextureTransformCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_uniform_build_clip_plane_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotCacheBatchMissUniformBuildClipPlaneCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_uniform_build_hash_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotCacheBatchMissUniformBuildHashCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_uniform_build_vs_const_hash_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotCacheBatchMissUniformBuildVsConstHashCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_uniform_build_ps_const_hash_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotCacheBatchMissUniformBuildPsConstHashCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_uniform_build_nonconst_hash_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotCacheBatchMissUniformBuildNonConstHashCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_uniform_build_nonconst_hash_world_view_proj_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotCacheBatchMissUniformBuildNonConstHashWorldViewProjCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_uniform_build_nonconst_hash_ffp_world_view_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotCacheBatchMissUniformBuildNonConstHashFfpWorldViewCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_uniform_build_nonconst_hash_ffp_normal_matrix_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotCacheBatchMissUniformBuildNonConstHashFfpNormalMatrixCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_uniform_build_nonconst_hash_material_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotCacheBatchMissUniformBuildNonConstHashMaterialCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_uniform_build_nonconst_hash_lights_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotCacheBatchMissUniformBuildNonConstHashLightsCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_uniform_build_nonconst_hash_ffp_blend_wvp_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotCacheBatchMissUniformBuildNonConstHashFfpBlendWvpCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_uniform_build_nonconst_hash_texture_transforms_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotCacheBatchMissUniformBuildNonConstHashTextureTransformsCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_uniform_build_nonconst_hash_clip_planes_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotCacheBatchMissUniformBuildNonConstHashClipPlanesCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_uniform_build_payload_combine_hash_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotCacheBatchMissUniformBuildPayloadCombineHashCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_uniform_build_vs_const_hash_full", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotCacheBatchMissUniformBuildVsConstHashFull, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_uniform_build_ps_const_hash_full", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotCacheBatchMissUniformBuildPsConstHashFull, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_uniform_build_vs_const_hash_full_no_usage", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotCacheBatchMissUniformBuildVsConstHashFullNoUsage, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_uniform_build_ps_const_hash_full_no_usage", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotCacheBatchMissUniformBuildPsConstHashFullNoUsage, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_uniform_build_vs_const_hash_full_unknown", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotCacheBatchMissUniformBuildVsConstHashFullUnknown, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_uniform_build_ps_const_hash_full_unknown", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotCacheBatchMissUniformBuildPsConstHashFullUnknown, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_uniform_build_vs_const_hash_full_unknown_bytecode", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotCacheBatchMissUniformBuildVsConstHashFullUnknownBytecode, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_uniform_build_ps_const_hash_full_unknown_bytecode", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotCacheBatchMissUniformBuildPsConstHashFullUnknownBytecode, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_uniform_build_vs_const_hash_full_unknown_non_bytecode", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotCacheBatchMissUniformBuildVsConstHashFullUnknownNonBytecode, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_uniform_build_ps_const_hash_full_unknown_non_bytecode", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotCacheBatchMissUniformBuildPsConstHashFullUnknownNonBytecode, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_uniform_build_vs_const_hash_full_indexed_float", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotCacheBatchMissUniformBuildVsConstHashFullIndexedFloat, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_uniform_build_vs_const_hash_full_indexed_float_min_safe_bytes", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotCacheBatchMissUniformBuildVsConstHashFullIndexedFloatMinSafeBytes, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_uniform_build_vs_const_hash_full_indexed_float_potential_saved_bytes", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotCacheBatchMissUniformBuildVsConstHashFullIndexedFloatPotentialSavedBytes, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_uniform_build_ps_const_hash_full_indexed_float", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotCacheBatchMissUniformBuildPsConstHashFullIndexedFloat, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_uniform_build_vs_const_hash_full_indexed_int", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotCacheBatchMissUniformBuildVsConstHashFullIndexedInt, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_uniform_build_ps_const_hash_full_indexed_int", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotCacheBatchMissUniformBuildPsConstHashFullIndexedInt, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_uniform_build_vs_const_hash_full_indexed_bool", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotCacheBatchMissUniformBuildVsConstHashFullIndexedBool, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_uniform_build_ps_const_hash_full_indexed_bool", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotCacheBatchMissUniformBuildPsConstHashFullIndexedBool, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_uniform_build_vs_const_hash_bytes", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotCacheBatchMissUniformBuildVsConstHashBytes, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_cache_batch_miss_uniform_build_ps_const_hash_bytes", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotCacheBatchMissUniformBuildPsConstHashBytes, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_copy_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotUniformCopyCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_materialized", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotUniformMaterialized, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_materialized_bytes", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotUniformMaterializedBytes, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_submission_carrier_records", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotSubmissionCarrierRecords, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_submission_carrier_bytes", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotSubmissionCarrierBytes, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_submission_carrier_state_storage_bytes", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotSubmissionCarrierStateStorageBytes, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_submission_carrier_uniform_storage_bytes", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotSubmissionCarrierUniformStorageBytes, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_submission_carrier_unused_uniform_storage_records", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotSubmissionCarrierUnusedUniformStorageRecords, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_submission_carrier_unused_uniform_storage_bytes", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotSubmissionCarrierUnusedUniformStorageBytes, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_elided", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotUniformElided, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_elided_bytes", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotUniformElidedBytes, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_adjacent_same_generation", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotUniformAdjacentSameGen, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_adjacent_same_generation_bytes", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotUniformAdjacentSameGenBytes, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_adjacent_same_generation_same_state_lane", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotUniformAdjacentSameGenSameState, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_adjacent_same_generation_same_state_lane_bytes", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotUniformAdjacentSameGenSameStateBytes, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_adjacent_same_generation_diff_state_lane", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotUniformAdjacentSameGenDiffState, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_adjacent_same_generation_diff_state_lane_bytes", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotUniformAdjacentSameGenDiffStateBytes, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_adjacent_same_payload_hash", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotUniformAdjacentSamePayloadHash, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_adjacent_same_payload_hash_bytes", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotUniformAdjacentSamePayloadHashBytes, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_adjacent_same_payload_hash_same_state_lane", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotUniformAdjacentSamePayloadHashSameState, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_adjacent_same_payload_hash_same_state_lane_bytes", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotUniformAdjacentSamePayloadHashSameStateBytes, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_adjacent_same_payload_hash_diff_state_lane", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotUniformAdjacentSamePayloadHashDiffState, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_adjacent_same_payload_hash_diff_state_lane_bytes", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotUniformAdjacentSamePayloadHashDiffStateBytes, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_adjacent_same_payload_hash_diff_generation", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotUniformAdjacentSamePayloadHashDiffGeneration, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_adjacent_same_payload_hash_diff_generation_bytes", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotUniformAdjacentSamePayloadHashDiffGenerationBytes, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_adjacent_previous_payload", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotUniformAdjacentPreviousPayload, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_adjacent_same_vs_const_hash", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotUniformAdjacentSameVsConstHash, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_adjacent_same_vs_const_hash_same_state_lane", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotUniformAdjacentSameVsConstHashSameState, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_adjacent_same_vs_const_hash_diff_generation", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotUniformAdjacentSameVsConstHashDiffGeneration, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_adjacent_same_ps_const_hash", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotUniformAdjacentSamePsConstHash, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_adjacent_same_ps_const_hash_same_state_lane", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotUniformAdjacentSamePsConstHashSameState, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_adjacent_same_ps_const_hash_diff_generation", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotUniformAdjacentSamePsConstHashDiffGeneration, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_adjacent_same_shader_const_hashes", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotUniformAdjacentSameShaderConstHashes, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_adjacent_same_shader_const_hashes_same_state_lane", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotUniformAdjacentSameShaderConstHashesSameState, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_adjacent_same_shader_const_hashes_diff_generation", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotUniformAdjacentSameShaderConstHashesDiffGeneration, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_adjacent_same_fixed_payload_hash", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotUniformAdjacentSameFixedPayloadHash, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_adjacent_same_fixed_payload_hash_same_state_lane", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotUniformAdjacentSameFixedPayloadHashSameState, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_adjacent_same_fixed_payload_hash_diff_generation", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotUniformAdjacentSameFixedPayloadHashDiffGeneration, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_adjacent_same_fixed_and_shader_const_hashes", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotUniformAdjacentSameFixedAndShaderConstHashes, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_adjacent_same_fixed_and_shader_const_hashes_same_state_lane", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotUniformAdjacentSameFixedAndShaderConstHashesSameState, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_uniform_adjacent_same_fixed_and_shader_const_hashes_diff_generation", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotUniformAdjacentSameFixedAndShaderConstHashesDiffGeneration, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_state_copy_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotStateCopyCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_state_materialized", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotStateMaterialized, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_state_materialized_bytes", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotStateMaterializedBytes, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_state_elided", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotStateElided, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_state_elided_bytes", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotStateElidedBytes, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_debug_snapshot_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotDebugSnapshotCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_flat_state_samples", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotFlatStateSamples, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_flat_render_state_entries", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotFlatRenderStateEntries, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_flat_render_state_entries_max", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotFlatRenderStateEntriesMax, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_flat_render_state_entries_gt64", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotFlatRenderStateEntriesGt64, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_flat_render_state_entries_gt128", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotFlatRenderStateEntriesGt128, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_flat_render_state_overflow", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotFlatRenderStateOverflow, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_flat_tss_entries", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotFlatTssEntries, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_flat_tss_stage_entries_max", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotFlatTssStageEntriesMax, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_flat_tss_overflow", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotFlatTssOverflow, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_flat_sampler_entries", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotFlatSamplerEntries, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_flat_sampler_slot_entries_max", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotFlatSamplerSlotEntriesMax, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_flat_sampler_overflow", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotFlatSamplerOverflow, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_binding_override_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9SnapshotBindingOverrideCpuNs, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_binding_override_stream_scans", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotBindingOverrideStreamScans, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_binding_override_stream_records", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotBindingOverrideStreamRecords, nullptr, nullptr, 0.0},
    {"d3d9_snapshot_binding_override_index_records", CounterEntry::Kind::UnsignedCount, &Counters::d3d9SnapshotBindingOverrideIndexRecords, nullptr, nullptr, 0.0},
    {"draw_uniform_payload_lookup_candidate_hits", CounterEntry::Kind::UnsignedCount, &Counters::drawUniformPayloadLookupCandidateHits, nullptr, nullptr, 0.0},
    {"draw_uniform_payload_lookup_last_hits", CounterEntry::Kind::UnsignedCount, &Counters::drawUniformPayloadLookupLastHits, nullptr, nullptr, 0.0},
    {"draw_uniform_payload_lookup_bucket_hits", CounterEntry::Kind::UnsignedCount, &Counters::drawUniformPayloadLookupBucketHits, nullptr, nullptr, 0.0},
    {"draw_uniform_payload_lookup_bucket_misses", CounterEntry::Kind::UnsignedCount, &Counters::drawUniformPayloadLookupBucketMisses, nullptr, nullptr, 0.0},
    {"draw_uniform_payload_lookup_linear_hits", CounterEntry::Kind::UnsignedCount, &Counters::drawUniformPayloadLookupLinearHits, nullptr, nullptr, 0.0},
    {"draw_uniform_payload_lookup_bucket_probes", CounterEntry::Kind::UnsignedCount, &Counters::drawUniformPayloadLookupBucketProbes, nullptr, nullptr, 0.0},
    {"draw_uniform_payload_lookup_bucket_collisions", CounterEntry::Kind::UnsignedCount, &Counters::drawUniformPayloadLookupBucketCollisions, nullptr, nullptr, 0.0},
    {"draw_uniform_payload_lookup_hash_collisions", CounterEntry::Kind::UnsignedCount, &Counters::drawUniformPayloadLookupHashCollisions, nullptr, nullptr, 0.0},
    {"draw_uniform_payload_lookup_semantic_hash_misses", CounterEntry::Kind::UnsignedCount, &Counters::drawUniformPayloadLookupSemanticHashMisses, nullptr, nullptr, 0.0},
    {"draw_uniform_payload_lookup_semantic_hash_miss_bytes", CounterEntry::Kind::UnsignedCount, &Counters::drawUniformPayloadLookupSemanticHashMissBytes, nullptr, nullptr, 0.0},
    {"draw_uniform_payload_lookup_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::drawUniformPayloadLookupCpuNs, nullptr, nullptr, 0.0},
    {"draw_uniform_payload_lookup_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::drawUniformPayloadLookupCpuMaxNs, nullptr, nullptr, 0.0},
    {"draw_uniform_payload_lookup_bucket_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::drawUniformPayloadLookupBucketCpuNs, nullptr, nullptr, 0.0},
    {"draw_uniform_payload_lookup_bucket_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::drawUniformPayloadLookupBucketCpuMaxNs, nullptr, nullptr, 0.0},
    {"draw_uniform_payload_appends", CounterEntry::Kind::UnsignedCount, &Counters::drawUniformPayloadAppends, nullptr, nullptr, 0.0},
    {"draw_uniform_payload_append_bytes", CounterEntry::Kind::UnsignedCount, &Counters::drawUniformPayloadAppendBytes, nullptr, nullptr, 0.0},
    {"draw_uniform_fixed_payload_appends", CounterEntry::Kind::UnsignedCount, &Counters::drawUniformFixedPayloadAppends, nullptr, nullptr, 0.0},
    {"draw_uniform_fixed_payload_append_bytes", CounterEntry::Kind::UnsignedCount, &Counters::drawUniformFixedPayloadAppendBytes, nullptr, nullptr, 0.0},
    {"draw_uniform_vertex_constants_appends", CounterEntry::Kind::UnsignedCount, &Counters::drawUniformVertexConstantsAppends, nullptr, nullptr, 0.0},
    {"draw_uniform_vertex_constants_append_bytes", CounterEntry::Kind::UnsignedCount, &Counters::drawUniformVertexConstantsAppendBytes, nullptr, nullptr, 0.0},
    {"draw_uniform_pixel_constants_appends", CounterEntry::Kind::UnsignedCount, &Counters::drawUniformPixelConstantsAppends, nullptr, nullptr, 0.0},
    {"draw_uniform_pixel_constants_append_bytes", CounterEntry::Kind::UnsignedCount, &Counters::drawUniformPixelConstantsAppendBytes, nullptr, nullptr, 0.0},
    {"draw_uniform_payload_append_fixed_find_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::drawUniformPayloadAppendFixedFindCpuNs, nullptr, nullptr, 0.0},
    {"draw_uniform_payload_append_fixed_find_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::drawUniformPayloadAppendFixedFindCpuMaxNs, nullptr, nullptr, 0.0},
    {"draw_uniform_payload_append_vertex_find_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::drawUniformPayloadAppendVertexFindCpuNs, nullptr, nullptr, 0.0},
    {"draw_uniform_payload_append_vertex_find_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::drawUniformPayloadAppendVertexFindCpuMaxNs, nullptr, nullptr, 0.0},
    {"draw_uniform_payload_append_pixel_find_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::drawUniformPayloadAppendPixelFindCpuNs, nullptr, nullptr, 0.0},
    {"draw_uniform_payload_append_pixel_find_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::drawUniformPayloadAppendPixelFindCpuMaxNs, nullptr, nullptr, 0.0},
    {"draw_uniform_payload_append_fixed_append_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::drawUniformPayloadAppendFixedAppendCpuNs, nullptr, nullptr, 0.0},
    {"draw_uniform_payload_append_fixed_append_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::drawUniformPayloadAppendFixedAppendCpuMaxNs, nullptr, nullptr, 0.0},
    {"draw_uniform_payload_append_vertex_append_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::drawUniformPayloadAppendVertexAppendCpuNs, nullptr, nullptr, 0.0},
    {"draw_uniform_payload_append_vertex_append_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::drawUniformPayloadAppendVertexAppendCpuMaxNs, nullptr, nullptr, 0.0},
    {"draw_uniform_payload_append_pixel_append_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::drawUniformPayloadAppendPixelAppendCpuNs, nullptr, nullptr, 0.0},
    {"draw_uniform_payload_append_pixel_append_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::drawUniformPayloadAppendPixelAppendCpuMaxNs, nullptr, nullptr, 0.0},
    {"draw_uniform_payload_materialized", CounterEntry::Kind::UnsignedCount, &Counters::drawUniformPayloadMaterialized, nullptr, nullptr, 0.0},
    {"draw_uniform_payload_materialized_bytes", CounterEntry::Kind::UnsignedCount, &Counters::drawUniformPayloadMaterializedBytes, nullptr, nullptr, 0.0},
    {"draw_uniform_payload_materialize_fallbacks", CounterEntry::Kind::UnsignedCount, &Counters::drawUniformPayloadMaterializeFallbacks, nullptr, nullptr, 0.0},
    {"draw_uniform_payload_materialize_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::drawUniformPayloadMaterializeCpuNs, nullptr, nullptr, 0.0},
    {"draw_uniform_payload_materialize_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::drawUniformPayloadMaterializeCpuMaxNs, nullptr, nullptr, 0.0},
    {"draw_uniform_payload_materialized_other", CounterEntry::Kind::UnsignedCount, &Counters::drawUniformPayloadMaterializedOther, nullptr, nullptr, 0.0},
    {"draw_uniform_payload_materialized_other_bytes", CounterEntry::Kind::UnsignedCount, &Counters::drawUniformPayloadMaterializedOtherBytes, nullptr, nullptr, 0.0},
    {"draw_uniform_payload_materialize_other_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::drawUniformPayloadMaterializeOtherCpuNs, nullptr, nullptr, 0.0},
    {"draw_uniform_payload_materialized_draw_encoder_command", CounterEntry::Kind::UnsignedCount, &Counters::drawUniformPayloadMaterializedDrawEncoderCommand, nullptr, nullptr, 0.0},
    {"draw_uniform_payload_materialized_draw_encoder_command_bytes", CounterEntry::Kind::UnsignedCount, &Counters::drawUniformPayloadMaterializedDrawEncoderCommandBytes, nullptr, nullptr, 0.0},
    {"draw_uniform_payload_materialize_draw_encoder_command_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::drawUniformPayloadMaterializeDrawEncoderCommandCpuNs, nullptr, nullptr, 0.0},
    {"draw_uniform_payload_materialized_draw_encoder_param", CounterEntry::Kind::UnsignedCount, &Counters::drawUniformPayloadMaterializedDrawEncoderParam, nullptr, nullptr, 0.0},
    {"draw_uniform_payload_materialized_draw_encoder_param_bytes", CounterEntry::Kind::UnsignedCount, &Counters::drawUniformPayloadMaterializedDrawEncoderParamBytes, nullptr, nullptr, 0.0},
    {"draw_uniform_payload_materialize_draw_encoder_param_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::drawUniformPayloadMaterializeDrawEncoderParamCpuNs, nullptr, nullptr, 0.0},
    {"draw_uniform_payload_materialized_framegraph_command", CounterEntry::Kind::UnsignedCount, &Counters::drawUniformPayloadMaterializedFramegraphCommand, nullptr, nullptr, 0.0},
    {"draw_uniform_payload_materialized_framegraph_command_bytes", CounterEntry::Kind::UnsignedCount, &Counters::drawUniformPayloadMaterializedFramegraphCommandBytes, nullptr, nullptr, 0.0},
    {"draw_uniform_payload_materialize_framegraph_command_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::drawUniformPayloadMaterializeFramegraphCommandCpuNs, nullptr, nullptr, 0.0},
    {"draw_uniform_payload_materialized_framegraph_param", CounterEntry::Kind::UnsignedCount, &Counters::drawUniformPayloadMaterializedFramegraphParam, nullptr, nullptr, 0.0},
    {"draw_uniform_payload_materialized_framegraph_param_bytes", CounterEntry::Kind::UnsignedCount, &Counters::drawUniformPayloadMaterializedFramegraphParamBytes, nullptr, nullptr, 0.0},
    {"draw_uniform_payload_materialize_framegraph_param_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::drawUniformPayloadMaterializeFramegraphParamCpuNs, nullptr, nullptr, 0.0},
    {"draw_uniform_payload_materialized_queue_observation", CounterEntry::Kind::UnsignedCount, &Counters::drawUniformPayloadMaterializedQueueObservation, nullptr, nullptr, 0.0},
    {"draw_uniform_payload_materialized_queue_observation_bytes", CounterEntry::Kind::UnsignedCount, &Counters::drawUniformPayloadMaterializedQueueObservationBytes, nullptr, nullptr, 0.0},
    {"draw_uniform_payload_materialize_queue_observation_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::drawUniformPayloadMaterializeQueueObservationCpuNs, nullptr, nullptr, 0.0},
    {"draw_uniform_payload_append_reserve_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::drawUniformPayloadAppendReserveCpuNs, nullptr, nullptr, 0.0},
    {"draw_uniform_payload_append_reserve_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::drawUniformPayloadAppendReserveCpuMaxNs, nullptr, nullptr, 0.0},
    {"draw_uniform_payload_append_copy_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::drawUniformPayloadAppendCopyCpuNs, nullptr, nullptr, 0.0},
    {"draw_uniform_payload_append_copy_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::drawUniformPayloadAppendCopyCpuMaxNs, nullptr, nullptr, 0.0},
    {"draw_uniform_payload_append_link_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::drawUniformPayloadAppendLinkCpuNs, nullptr, nullptr, 0.0},
    {"draw_uniform_payload_append_link_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::drawUniformPayloadAppendLinkCpuMaxNs, nullptr, nullptr, 0.0},
    {"transient_upload_calls", CounterEntry::Kind::UnsignedCount, &Counters::transientUploadCalls, nullptr, nullptr, 0.0},
    {"transient_upload_bytes", CounterEntry::Kind::UnsignedCount, &Counters::transientUploadBytes, nullptr, nullptr, 0.0},
    {"transient_upload_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::transientUploadCpuNs, nullptr, nullptr, 0.0},
    {"transient_upload_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::transientUploadCpuMaxNs, nullptr, nullptr, 0.0},
    {"transient_upload_cpu_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::transientUploadCpuRing, 0.5},
    {"transient_upload_cpu_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::transientUploadCpuRing, 0.95},
    {"transient_upload_cpu_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::transientUploadCpuRing, 0.99},
    {"d3d9_buffer_lock_calls", CounterEntry::Kind::UnsignedCount, &Counters::d3d9BufferLockCalls, nullptr, nullptr, 0.0},
    {"d3d9_buffer_lock_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9BufferLockNs, nullptr, nullptr, 0.0},
    {"d3d9_buffer_lock_max_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9BufferLockMaxNs, nullptr, nullptr, 0.0},
    {"d3d9_buffer_lock_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::d3d9BufferLockRing, 0.5},
    {"d3d9_buffer_lock_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::d3d9BufferLockRing, 0.95},
    {"d3d9_buffer_lock_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::d3d9BufferLockRing, 0.99},
    {"d3d9_buffer_lock_bytes", CounterEntry::Kind::UnsignedCount, &Counters::d3d9BufferLockBytes, nullptr, nullptr, 0.0},
    {"d3d9_buffer_lock_discard", CounterEntry::Kind::UnsignedCount, &Counters::d3d9BufferLockDiscard, nullptr, nullptr, 0.0},
    {"d3d9_buffer_lock_nooverwrite", CounterEntry::Kind::UnsignedCount, &Counters::d3d9BufferLockNoOverwrite, nullptr, nullptr, 0.0},
    {"d3d9_buffer_lock_readonly", CounterEntry::Kind::UnsignedCount, &Counters::d3d9BufferLockReadOnly, nullptr, nullptr, 0.0},
    {"d3d9_buffer_lock_plain", CounterEntry::Kind::UnsignedCount, &Counters::d3d9BufferLockPlain, nullptr, nullptr, 0.0},
    {"d3d9_buffer_lock_full_resource", CounterEntry::Kind::UnsignedCount, &Counters::d3d9BufferLockFullResource, nullptr, nullptr, 0.0},
    {"d3d9_buffer_lock_shadow", CounterEntry::Kind::UnsignedCount, &Counters::d3d9BufferLockShadow, nullptr, nullptr, 0.0},
    {"d3d9_buffer_lock_shadow_bytes", CounterEntry::Kind::UnsignedCount, &Counters::d3d9BufferLockShadowBytes, nullptr, nullptr, 0.0},
    {"d3d9_buffer_lock_shadow_alloc_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9BufferLockShadowAllocNs, nullptr, nullptr, 0.0},
    {"d3d9_buffer_lock_shadow_alloc_max_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9BufferLockShadowAllocMaxNs, nullptr, nullptr, 0.0},
    {"d3d9_buffer_lock_shadow_alloc_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::d3d9BufferLockShadowAllocRing, 0.5},
    {"d3d9_buffer_lock_shadow_alloc_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::d3d9BufferLockShadowAllocRing, 0.95},
    {"d3d9_buffer_lock_shadow_alloc_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::d3d9BufferLockShadowAllocRing, 0.99},
    {"d3d9_buffer_lock_shadow_copy_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9BufferLockShadowCopyNs, nullptr, nullptr, 0.0},
    {"d3d9_buffer_lock_shadow_copy_max_ms", CounterEntry::Kind::Milliseconds, &Counters::d3d9BufferLockShadowCopyMaxNs, nullptr, nullptr, 0.0},
    {"d3d9_buffer_lock_shadow_copy_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::d3d9BufferLockShadowCopyRing, 0.5},
    {"d3d9_buffer_lock_shadow_copy_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::d3d9BufferLockShadowCopyRing, 0.95},
    {"d3d9_buffer_lock_shadow_copy_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::d3d9BufferLockShadowCopyRing, 0.99},
    {"d3d9_buffer_lock_default_pool", CounterEntry::Kind::UnsignedCount, &Counters::d3d9BufferLockDefaultPool, nullptr, nullptr, 0.0},
    {"d3d9_buffer_lock_managed_pool", CounterEntry::Kind::UnsignedCount, &Counters::d3d9BufferLockManagedPool, nullptr, nullptr, 0.0},
    {"d3d9_buffer_lock_systemmem_pool", CounterEntry::Kind::UnsignedCount, &Counters::d3d9BufferLockSystemMemPool, nullptr, nullptr, 0.0},
    {"d3d9_buffer_lock_scratch_pool", CounterEntry::Kind::UnsignedCount, &Counters::d3d9BufferLockScratchPool, nullptr, nullptr, 0.0},
    {"d3d9_buffer_lock_dynamic", CounterEntry::Kind::UnsignedCount, &Counters::d3d9BufferLockDynamic, nullptr, nullptr, 0.0},
    {"d3d9_buffer_lock_writeonly", CounterEntry::Kind::UnsignedCount, &Counters::d3d9BufferLockWriteOnly, nullptr, nullptr, 0.0},
    {"uniform_vs_consts_calls", CounterEntry::Kind::UnsignedCount, &Counters::uniformVsConstsCalls, nullptr, nullptr, 0.0},
    {"uniform_vs_consts_bytes", CounterEntry::Kind::UnsignedCount, &Counters::uniformVsConstsBytes, nullptr, nullptr, 0.0},
    {"uniform_ps_consts_calls", CounterEntry::Kind::UnsignedCount, &Counters::uniformPsConstsCalls, nullptr, nullptr, 0.0},
    {"uniform_ps_consts_bytes", CounterEntry::Kind::UnsignedCount, &Counters::uniformPsConstsBytes, nullptr, nullptr, 0.0},
    {"uniform_ffp_vs_calls", CounterEntry::Kind::UnsignedCount, &Counters::uniformFfpVsCalls, nullptr, nullptr, 0.0},
    {"uniform_ffp_vs_bytes", CounterEntry::Kind::UnsignedCount, &Counters::uniformFfpVsBytes, nullptr, nullptr, 0.0},
    {"uniform_ffp_ps_calls", CounterEntry::Kind::UnsignedCount, &Counters::uniformFfpPsCalls, nullptr, nullptr, 0.0},
    {"uniform_ffp_ps_bytes", CounterEntry::Kind::UnsignedCount, &Counters::uniformFfpPsBytes, nullptr, nullptr, 0.0},
    {"uniform_volatile_pushes", CounterEntry::Kind::UnsignedCount, &Counters::uniformVolatilePushes, nullptr, nullptr, 0.0},
    {"managed_texture_upload_blit_count", CounterEntry::Kind::UnsignedCount, &Counters::managedTextureUploadBlitCount, nullptr, nullptr, 0.0},
    {"managed_texture_upload_blit_bytes", CounterEntry::Kind::UnsignedCount, &Counters::managedTextureUploadBlitBytes, nullptr, nullptr, 0.0},
    {"texture_pixel_format_view_suppressed_rt_count", CounterEntry::Kind::UnsignedCount, &Counters::texturePixelFormatViewSuppressedRtCount, nullptr, nullptr, 0.0},
    {"texture_pixel_format_view_suppressed_rt_bytes", CounterEntry::Kind::UnsignedCount, &Counters::texturePixelFormatViewSuppressedRtBytes, nullptr, nullptr, 0.0},
    {"heap_alloc_count", CounterEntry::Kind::UnsignedCount, &Counters::heapAllocCount, nullptr, nullptr, 0.0},
    {"heap_bytes_allocated", CounterEntry::Kind::UnsignedCount, &Counters::heapBytesAllocated, nullptr, nullptr, 0.0},
    {"heap_instance_count", CounterEntry::Kind::UnsignedCount, &Counters::heapInstanceCount, nullptr, nullptr, 0.0},
    {"heap_direct_fallback_count", CounterEntry::Kind::UnsignedCount, &Counters::heapDirectFallbackCount, nullptr, nullptr, 0.0},
    {"heap_fragmentation_failure_count", CounterEntry::Kind::UnsignedCount, &Counters::heapFragmentationFailureCount, nullptr, nullptr, 0.0},
    {"heap_compaction_count", CounterEntry::Kind::UnsignedCount, &Counters::heapCompactionCount, nullptr, nullptr, 0.0},
    {"heap_alloc_failure_count", CounterEntry::Kind::UnsignedCount, &Counters::heapAllocFailureCount, nullptr, nullptr, 0.0},
    {"use_heap_calls", CounterEntry::Kind::UnsignedCount, &Counters::useHeapCalls, nullptr, nullptr, 0.0},
    {"use_resource_calls", CounterEntry::Kind::UnsignedCount, &Counters::useResourceCalls, nullptr, nullptr, 0.0},
    {"tile_ffp_pass_count", CounterEntry::Kind::UnsignedCount, &Counters::tileFfpPassCount, nullptr, nullptr, 0.0},
    {"portable_ffp_pass_count", CounterEntry::Kind::UnsignedCount, &Counters::portableFfpPassCount, nullptr, nullptr, 0.0},
    {"tile_ffp_fallback_precision", CounterEntry::Kind::UnsignedCount, &Counters::tileFfpFallbackPrecision, nullptr, nullptr, 0.0},
    {"tile_ffp_fallback_unsupported_state", CounterEntry::Kind::UnsignedCount, &Counters::tileFfpFallbackUnsupportedState, nullptr, nullptr, 0.0},
    {"tile_ffp_fallback_gpu_family", CounterEntry::Kind::UnsignedCount, &Counters::tileFfpFallbackGpuFamily, nullptr, nullptr, 0.0},
    {"tile_ffp_fallback_mid_pass_ineligible", CounterEntry::Kind::UnsignedCount, &Counters::tileFfpFallbackMidPassIneligible, nullptr, nullptr, 0.0},
    {"tile_ffp_mid_pass_resplit_count", CounterEntry::Kind::UnsignedCount, &Counters::tileFfpMidPassResplitCount, nullptr, nullptr, 0.0},
    {"argbuf_hybrid_encoder_count", CounterEntry::Kind::UnsignedCount, &Counters::argbufHybridEncoderCount, nullptr, nullptr, 0.0},
    {"stage1_encoder_count", CounterEntry::Kind::UnsignedCount, &Counters::stage1EncoderCount, nullptr, nullptr, 0.0},
    {"argbuf_hybrid_fallback_count", CounterEntry::Kind::UnsignedCount, &Counters::argbufHybridFallbackCount, nullptr, nullptr, 0.0},
    {"argbuf_hybrid_bytes_per_encoder", CounterEntry::Kind::UnsignedCount, &Counters::argbufHybridBytesPerEncoder, nullptr, nullptr, 0.0},
    {"stage1_bytes_per_encoder", CounterEntry::Kind::UnsignedCount, &Counters::stage1BytesPerEncoder, nullptr, nullptr, 0.0},
    {"prewarm_entries_loaded", CounterEntry::Kind::UnsignedCount, &Counters::prewarmEntriesLoaded, nullptr, nullptr, 0.0},
    {"prewarm_load_cpu_ns", CounterEntry::Kind::UnsignedCount, &Counters::prewarmLoadCpuNs, nullptr, nullptr, 0.0},
    {"prewarm_failure_corrupt", CounterEntry::Kind::UnsignedCount, &Counters::prewarmFailureCorrupt, nullptr, nullptr, 0.0},
    {"prewarm_failure_schema", CounterEntry::Kind::UnsignedCount, &Counters::prewarmFailureSchema, nullptr, nullptr, 0.0},
    {"prewarm_failure_lock_busy", CounterEntry::Kind::UnsignedCount, &Counters::prewarmFailureLockBusy, nullptr, nullptr, 0.0},
    {"prewarm_failure_missing", CounterEntry::Kind::UnsignedCount, &Counters::prewarmFailureMissing, nullptr, nullptr, 0.0},
    {"cold_compile_count_after_warm", CounterEntry::Kind::UnsignedCount, &Counters::coldCompileCountAfterWarm, nullptr, nullptr, 0.0},
    {"archive_bytes", CounterEntry::Kind::UnsignedCount, &Counters::archiveBytes, nullptr, nullptr, 0.0},
    {"prewarm_demoted_by_size", CounterEntry::Kind::UnsignedCount, &Counters::prewarmDemotedBySize, nullptr, nullptr, 0.0},
    {"prewarm_async_completion_cpu_ns", CounterEntry::Kind::UnsignedCount, &Counters::prewarmAsyncCompletionCpuNs, nullptr, nullptr, 0.0},
    {"prewarm_milestone_save_count", CounterEntry::Kind::UnsignedCount, &Counters::prewarmMilestoneSaveCount, nullptr, nullptr, 0.0},
    {"prewarm_save_skipped_debug_env_count", CounterEntry::Kind::UnsignedCount, &Counters::prewarmSaveSkippedDebugEnvCount, nullptr, nullptr, 0.0},
    {"shader_decoder_reject_truncated", CounterEntry::Kind::UnsignedCount, &Counters::shaderDecoderRejectTruncated, nullptr, nullptr, 0.0},
    {"shader_decoder_reject_unsupported_version", CounterEntry::Kind::UnsignedCount, &Counters::shaderDecoderRejectUnsupportedVersion, nullptr, nullptr, 0.0},
    {"shader_decoder_reject_oob_register", CounterEntry::Kind::UnsignedCount, &Counters::shaderDecoderRejectOobRegister, nullptr, nullptr, 0.0},
    {"shader_decoder_reject_missing_end", CounterEntry::Kind::UnsignedCount, &Counters::shaderDecoderRejectMissingEnd, nullptr, nullptr, 0.0},
    {"shader_decoder_reject_invalid_opcode", CounterEntry::Kind::UnsignedCount, &Counters::shaderDecoderRejectInvalidOpcode, nullptr, nullptr, 0.0},
    {"shader_decoder_reject_tempfloat16_unsupported", CounterEntry::Kind::UnsignedCount, &Counters::shaderDecoderRejectTempFloat16Unsupported, nullptr, nullptr, 0.0},
    {"shader_decoder_reject_label_unsupported", CounterEntry::Kind::UnsignedCount, &Counters::shaderDecoderRejectLabelUnsupported, nullptr, nullptr, 0.0},
    {"shader_decoder_reject_decl_usage_unsupported", CounterEntry::Kind::UnsignedCount, &Counters::shaderDecoderRejectDeclUsageUnsupported, nullptr, nullptr, 0.0},
    {"shader_decoder_reject_decl_method_unsupported", CounterEntry::Kind::UnsignedCount, &Counters::shaderDecoderRejectDeclMethodUnsupported, nullptr, nullptr, 0.0},
    {"render_pass_load_action_load", CounterEntry::Kind::UnsignedCount, &Counters::renderPassLoadActionLoad, nullptr, nullptr, 0.0},
    {"render_pass_load_action_clear", CounterEntry::Kind::UnsignedCount, &Counters::renderPassLoadActionClear, nullptr, nullptr, 0.0},
    {"render_pass_load_action_dontcare", CounterEntry::Kind::UnsignedCount, &Counters::renderPassLoadActionDontCare, nullptr, nullptr, 0.0},
    {"render_pass_load_action_depth_load", CounterEntry::Kind::UnsignedCount, &Counters::renderPassLoadActionDepthLoad, nullptr, nullptr, 0.0},
    {"render_pass_load_action_depth_clear", CounterEntry::Kind::UnsignedCount, &Counters::renderPassLoadActionDepthClear, nullptr, nullptr, 0.0},
    {"render_pass_load_action_depth_dontcare", CounterEntry::Kind::UnsignedCount, &Counters::renderPassLoadActionDepthDontCare, nullptr, nullptr, 0.0},
    {"render_pass_load_action_stencil_load", CounterEntry::Kind::UnsignedCount, &Counters::renderPassLoadActionStencilLoad, nullptr, nullptr, 0.0},
    {"render_pass_load_action_stencil_clear", CounterEntry::Kind::UnsignedCount, &Counters::renderPassLoadActionStencilClear, nullptr, nullptr, 0.0},
    {"render_pass_load_action_stencil_dontcare", CounterEntry::Kind::UnsignedCount, &Counters::renderPassLoadActionStencilDontCare, nullptr, nullptr, 0.0},
    {"render_pass_store_action_store", CounterEntry::Kind::UnsignedCount, &Counters::renderPassStoreActionStore, nullptr, nullptr, 0.0},
    {"render_pass_store_action_dontcare", CounterEntry::Kind::UnsignedCount, &Counters::renderPassStoreActionDontCare, nullptr, nullptr, 0.0},
    {"render_pass_store_action_resolve", CounterEntry::Kind::UnsignedCount, &Counters::renderPassStoreActionResolve, nullptr, nullptr, 0.0},
    {"render_pass_store_action_depth_store", CounterEntry::Kind::UnsignedCount, &Counters::renderPassStoreActionDepthStore, nullptr, nullptr, 0.0},
    {"render_pass_store_action_depth_dontcare", CounterEntry::Kind::UnsignedCount, &Counters::renderPassStoreActionDepthDontCare, nullptr, nullptr, 0.0},
    {"render_pass_store_action_stencil_store", CounterEntry::Kind::UnsignedCount, &Counters::renderPassStoreActionStencilStore, nullptr, nullptr, 0.0},
    {"render_pass_store_action_stencil_dontcare", CounterEntry::Kind::UnsignedCount, &Counters::renderPassStoreActionStencilDontCare, nullptr, nullptr, 0.0},
    {"render_pass_tile_preservation_bytes", CounterEntry::Kind::UnsignedCount, &Counters::renderPassTilePreservationBytes, nullptr, nullptr, 0.0},
    {"render_pass_same_key_adjacent", CounterEntry::Kind::UnsignedCount, &Counters::renderPassSameKeyAdjacent, nullptr, nullptr, 0.0},
    {"render_pass_same_key_reentry", CounterEntry::Kind::UnsignedCount, &Counters::renderPassSameKeyReentry, nullptr, nullptr, 0.0},
    {"render_pass_same_key_reentry_distance_1", CounterEntry::Kind::UnsignedCount, &Counters::renderPassSameKeyReentryDistance1, nullptr, nullptr, 0.0},
    {"render_pass_same_key_reentry_distance_2", CounterEntry::Kind::UnsignedCount, &Counters::renderPassSameKeyReentryDistance2, nullptr, nullptr, 0.0},
    {"render_pass_same_key_reentry_distance_3_4", CounterEntry::Kind::UnsignedCount, &Counters::renderPassSameKeyReentryDistance3To4, nullptr, nullptr, 0.0},
    {"render_pass_same_key_reentry_distance_5_8", CounterEntry::Kind::UnsignedCount, &Counters::renderPassSameKeyReentryDistance5To8, nullptr, nullptr, 0.0},
    {"render_pass_same_key_reentry_distance_9_16", CounterEntry::Kind::UnsignedCount, &Counters::renderPassSameKeyReentryDistance9To16, nullptr, nullptr, 0.0},
    {"render_pass_same_key_reentry_distance_17_plus", CounterEntry::Kind::UnsignedCount, &Counters::renderPassSameKeyReentryDistance17Plus, nullptr, nullptr, 0.0},
    {"render_pass_same_key_reentry_distance_1_same_color", CounterEntry::Kind::UnsignedCount, &Counters::renderPassSameKeyReentryDistance1SameColor, nullptr, nullptr, 0.0},
    {"render_pass_same_key_reentry_distance_1_same_color_preservation_bytes", CounterEntry::Kind::UnsignedCount, &Counters::renderPassSameKeyReentryDistance1SameColorBytes, nullptr, nullptr, 0.0},
    {"render_pass_same_key_reentry_distance_1_same_depth", CounterEntry::Kind::UnsignedCount, &Counters::renderPassSameKeyReentryDistance1SameDepth, nullptr, nullptr, 0.0},
    {"render_pass_same_key_reentry_distance_1_same_depth_preservation_bytes", CounterEntry::Kind::UnsignedCount, &Counters::renderPassSameKeyReentryDistance1SameDepthBytes, nullptr, nullptr, 0.0},
    {"render_pass_same_key_reentry_distance_1_rt_depth_change", CounterEntry::Kind::UnsignedCount, &Counters::renderPassSameKeyReentryDistance1RtDepthChange, nullptr, nullptr, 0.0},
    {"render_pass_same_key_reentry_distance_1_rt_depth_change_preservation_bytes", CounterEntry::Kind::UnsignedCount, &Counters::renderPassSameKeyReentryDistance1RtDepthChangeBytes, nullptr, nullptr, 0.0},
    {"render_pass_same_key_reentry_distance_1_sample_change", CounterEntry::Kind::UnsignedCount, &Counters::renderPassSameKeyReentryDistance1SampleChange, nullptr, nullptr, 0.0},
    {"render_pass_same_key_reentry_distance_1_sample_change_preservation_bytes", CounterEntry::Kind::UnsignedCount, &Counters::renderPassSameKeyReentryDistance1SampleChangeBytes, nullptr, nullptr, 0.0},
    {"render_pass_same_key_reentry_preservation_bytes", CounterEntry::Kind::UnsignedCount, &Counters::renderPassSameKeyReentryPreservationBytes, nullptr, nullptr, 0.0},
    {"render_pass_same_key_reentry_color_preservation_bytes", CounterEntry::Kind::UnsignedCount, &Counters::renderPassSameKeyReentryColorPreservationBytes, nullptr, nullptr, 0.0},
    {"render_pass_same_key_reentry_depth_preservation_bytes", CounterEntry::Kind::UnsignedCount, &Counters::renderPassSameKeyReentryDepthPreservationBytes, nullptr, nullptr, 0.0},
    {"render_pass_natural_fallback_begin", CounterEntry::Kind::UnsignedCount, &Counters::renderPassNaturalFallbackBegin, nullptr, nullptr, 0.0},
    {"render_pass_natural_fallback_same_window_reentry_distance_1", CounterEntry::Kind::UnsignedCount, &Counters::renderPassNaturalFallbackSameWindowReentryDistance1, nullptr, nullptr, 0.0},
    {"render_pass_natural_fallback_same_window_reentry_distance_2", CounterEntry::Kind::UnsignedCount, &Counters::renderPassNaturalFallbackSameWindowReentryDistance2, nullptr, nullptr, 0.0},
    {"render_pass_natural_fallback_same_window_reentry_distance_3_4", CounterEntry::Kind::UnsignedCount, &Counters::renderPassNaturalFallbackSameWindowReentryDistance3To4, nullptr, nullptr, 0.0},
    {"render_pass_natural_fallback_cross_window_reentry_distance_1", CounterEntry::Kind::UnsignedCount, &Counters::renderPassNaturalFallbackCrossWindowReentryDistance1, nullptr, nullptr, 0.0},
    {"render_pass_natural_fallback_cross_window_reentry_distance_2", CounterEntry::Kind::UnsignedCount, &Counters::renderPassNaturalFallbackCrossWindowReentryDistance2, nullptr, nullptr, 0.0},
    {"render_pass_natural_fallback_cross_window_reentry_distance_3_4", CounterEntry::Kind::UnsignedCount, &Counters::renderPassNaturalFallbackCrossWindowReentryDistance3To4, nullptr, nullptr, 0.0},
    {"active_seed_merge_ticket_issued", CounterEntry::Kind::UnsignedCount, &Counters::activeSeedMergeTicketIssued, nullptr, nullptr, 0.0},
    {"active_seed_merge_ticket_matched", CounterEntry::Kind::UnsignedCount, &Counters::activeSeedMergeTicketMatched, nullptr, nullptr, 0.0},
    {"active_seed_merge_ticket_continued", CounterEntry::Kind::UnsignedCount, &Counters::activeSeedMergeTicketContinued, nullptr, nullptr, 0.0},
    {"active_seed_merge_ticket_mismatch", CounterEntry::Kind::UnsignedCount, &Counters::activeSeedMergeTicketMismatch, nullptr, nullptr, 0.0},
    {"active_seed_merge_ticket_unconsumed", CounterEntry::Kind::UnsignedCount, &Counters::activeSeedMergeTicketUnconsumed, nullptr, nullptr, 0.0},
    {"active_seed_merge_witness_overflow", CounterEntry::Kind::UnsignedCount, &Counters::activeSeedMergeWitnessOverflow, nullptr, nullptr, 0.0},
    {"active_seed_merge_witness_mismatch", CounterEntry::Kind::UnsignedCount, &Counters::activeSeedMergeWitnessMismatch, nullptr, nullptr, 0.0},
    {"active_seed_instance_unavailable", CounterEntry::Kind::UnsignedCount, &Counters::activeSeedInstanceUnavailable, nullptr, nullptr, 0.0},
    {"active_seed_instance_stale", CounterEntry::Kind::UnsignedCount, &Counters::activeSeedInstanceStale, nullptr, nullptr, 0.0},
    {"render_pass_active_seed_bridge_reentry_distance_1", CounterEntry::Kind::UnsignedCount, &Counters::renderPassActiveSeedBridgeReentryDistance1, nullptr, nullptr, 0.0},
    {"render_pass_active_seed_bridge_reentry_distance_2", CounterEntry::Kind::UnsignedCount, &Counters::renderPassActiveSeedBridgeReentryDistance2, nullptr, nullptr, 0.0},
    {"render_pass_active_seed_bridge_reentry_distance_3_4", CounterEntry::Kind::UnsignedCount, &Counters::renderPassActiveSeedBridgeReentryDistance3To4, nullptr, nullptr, 0.0},
    {"render_pass_final_close_session_cap", CounterEntry::Kind::UnsignedCount, &Counters::renderPassFinalCloseSessionCap, nullptr, nullptr, 0.0},
    {"render_pass_final_close_independent", CounterEntry::Kind::UnsignedCount, &Counters::renderPassFinalCloseIndependent, nullptr, nullptr, 0.0},
    {"render_pass_final_close_initializer", CounterEntry::Kind::UnsignedCount, &Counters::renderPassFinalCloseInitializer, nullptr, nullptr, 0.0},
    {"render_pass_final_close_producer_wait", CounterEntry::Kind::UnsignedCount, &Counters::renderPassFinalCloseProducerWait, nullptr, nullptr, 0.0},
    {"render_pass_final_close_drain", CounterEntry::Kind::UnsignedCount, &Counters::renderPassFinalCloseDrain, nullptr, nullptr, 0.0},
    {"render_pass_final_close_fail_other", CounterEntry::Kind::UnsignedCount, &Counters::renderPassFinalCloseFailOther, nullptr, nullptr, 0.0},
    {"render_pass_close_adjacent_session_cap", CounterEntry::Kind::UnsignedCount, &Counters::renderPassCloseAdjacentSessionCap, nullptr, nullptr, 0.0},
    {"render_pass_close_adjacent_independent", CounterEntry::Kind::UnsignedCount, &Counters::renderPassCloseAdjacentIndependent, nullptr, nullptr, 0.0},
    {"render_pass_close_adjacent_initializer", CounterEntry::Kind::UnsignedCount, &Counters::renderPassCloseAdjacentInitializer, nullptr, nullptr, 0.0},
    {"render_pass_close_adjacent_producer_wait", CounterEntry::Kind::UnsignedCount, &Counters::renderPassCloseAdjacentProducerWait, nullptr, nullptr, 0.0},
    {"render_pass_close_adjacent_drain", CounterEntry::Kind::UnsignedCount, &Counters::renderPassCloseAdjacentDrain, nullptr, nullptr, 0.0},
    {"render_pass_close_adjacent_fail_other", CounterEntry::Kind::UnsignedCount, &Counters::renderPassCloseAdjacentFailOther, nullptr, nullptr, 0.0},
    {"render_pass_natural_short_cross_close_final", CounterEntry::Kind::UnsignedCount, &Counters::renderPassNaturalShortCrossCloseFinal, nullptr, nullptr, 0.0},
    {"render_pass_natural_short_cross_close_rt_change", CounterEntry::Kind::UnsignedCount, &Counters::renderPassNaturalShortCrossCloseRtChange, nullptr, nullptr, 0.0},
    {"render_pass_natural_short_cross_close_hazard", CounterEntry::Kind::UnsignedCount, &Counters::renderPassNaturalShortCrossCloseHazard, nullptr, nullptr, 0.0},
    {"render_pass_natural_short_cross_close_clear", CounterEntry::Kind::UnsignedCount, &Counters::renderPassNaturalShortCrossCloseClear, nullptr, nullptr, 0.0},
    {"render_pass_natural_short_cross_close_surface_copy", CounterEntry::Kind::UnsignedCount, &Counters::renderPassNaturalShortCrossCloseSurfaceCopy, nullptr, nullptr, 0.0},
    {"render_pass_natural_short_cross_close_stretch_rect", CounterEntry::Kind::UnsignedCount, &Counters::renderPassNaturalShortCrossCloseStretchRect, nullptr, nullptr, 0.0},
    {"render_pass_natural_short_cross_close_readback", CounterEntry::Kind::UnsignedCount, &Counters::renderPassNaturalShortCrossCloseReadback, nullptr, nullptr, 0.0},
    {"render_pass_natural_short_cross_close_color_fill", CounterEntry::Kind::UnsignedCount, &Counters::renderPassNaturalShortCrossCloseColorFill, nullptr, nullptr, 0.0},
    {"render_pass_natural_short_cross_close_present", CounterEntry::Kind::UnsignedCount, &Counters::renderPassNaturalShortCrossClosePresent, nullptr, nullptr, 0.0},
    {"render_pass_natural_short_cross_close_present_acquire", CounterEntry::Kind::UnsignedCount, &Counters::renderPassNaturalShortCrossClosePresentAcquire, nullptr, nullptr, 0.0},
    {"render_pass_natural_short_cross_close_tile", CounterEntry::Kind::UnsignedCount, &Counters::renderPassNaturalShortCrossCloseTile, nullptr, nullptr, 0.0},
    {"render_pass_natural_short_cross_close_ordered", CounterEntry::Kind::UnsignedCount, &Counters::renderPassNaturalShortCrossCloseOrdered, nullptr, nullptr, 0.0},
    {"render_pass_natural_short_cross_close_matched", CounterEntry::Kind::UnsignedCount, &Counters::renderPassNaturalShortCrossCloseMatched, nullptr, nullptr, 0.0},
    {"render_pass_natural_short_cross_close_missing", CounterEntry::Kind::UnsignedCount, &Counters::renderPassNaturalShortCrossCloseMissing, nullptr, nullptr, 0.0},
    {"render_pass_short_reentry_d1_ordinary", CounterEntry::Kind::UnsignedCount, &Counters::renderPassShortReentryD1Ordinary, nullptr, nullptr, 0.0},
    {"render_pass_short_reentry_d1_natural_same", CounterEntry::Kind::UnsignedCount, &Counters::renderPassShortReentryD1NaturalSame, nullptr, nullptr, 0.0},
    {"render_pass_short_reentry_d1_natural_cross", CounterEntry::Kind::UnsignedCount, &Counters::renderPassShortReentryD1NaturalCross, nullptr, nullptr, 0.0},
    {"render_pass_short_reentry_d1_planned", CounterEntry::Kind::UnsignedCount, &Counters::renderPassShortReentryD1Planned, nullptr, nullptr, 0.0},
    {"render_pass_short_reentry_d1_eligibility_present", CounterEntry::Kind::UnsignedCount, &Counters::renderPassShortReentryD1EligibilityPresent, nullptr, nullptr, 0.0},
    {"render_pass_short_reentry_d1_eligibility_other", CounterEntry::Kind::UnsignedCount, &Counters::renderPassShortReentryD1EligibilityOther, nullptr, nullptr, 0.0},
    {"render_pass_short_reentry_d1_permutation_rejected", CounterEntry::Kind::UnsignedCount, &Counters::renderPassShortReentryD1PermutationRejected, nullptr, nullptr, 0.0},
    {"render_pass_short_reentry_d1_mixed_invalid", CounterEntry::Kind::UnsignedCount, &Counters::renderPassShortReentryD1MixedInvalid, nullptr, nullptr, 0.0},
    {"render_pass_short_reentry_d2_ordinary", CounterEntry::Kind::UnsignedCount, &Counters::renderPassShortReentryD2Ordinary, nullptr, nullptr, 0.0},
    {"render_pass_short_reentry_d2_natural_same", CounterEntry::Kind::UnsignedCount, &Counters::renderPassShortReentryD2NaturalSame, nullptr, nullptr, 0.0},
    {"render_pass_short_reentry_d2_natural_cross", CounterEntry::Kind::UnsignedCount, &Counters::renderPassShortReentryD2NaturalCross, nullptr, nullptr, 0.0},
    {"render_pass_short_reentry_d2_planned", CounterEntry::Kind::UnsignedCount, &Counters::renderPassShortReentryD2Planned, nullptr, nullptr, 0.0},
    {"render_pass_short_reentry_d2_eligibility_present", CounterEntry::Kind::UnsignedCount, &Counters::renderPassShortReentryD2EligibilityPresent, nullptr, nullptr, 0.0},
    {"render_pass_short_reentry_d2_eligibility_other", CounterEntry::Kind::UnsignedCount, &Counters::renderPassShortReentryD2EligibilityOther, nullptr, nullptr, 0.0},
    {"render_pass_short_reentry_d2_permutation_rejected", CounterEntry::Kind::UnsignedCount, &Counters::renderPassShortReentryD2PermutationRejected, nullptr, nullptr, 0.0},
    {"render_pass_short_reentry_d2_mixed_invalid", CounterEntry::Kind::UnsignedCount, &Counters::renderPassShortReentryD2MixedInvalid, nullptr, nullptr, 0.0},
    {"render_pass_short_reentry_d1_source_all_same", CounterEntry::Kind::UnsignedCount, &Counters::renderPassShortReentryD1SourceAllSame, nullptr, nullptr, 0.0},
    {"render_pass_short_reentry_d1_source_prior_intervening_same_current_newer", CounterEntry::Kind::UnsignedCount, &Counters::renderPassShortReentryD1SourcePriorInterveningSameCurrentNewer, nullptr, nullptr, 0.0},
    {"render_pass_short_reentry_d1_source_prior_older_intervening_current_same", CounterEntry::Kind::UnsignedCount, &Counters::renderPassShortReentryD1SourcePriorOlderInterveningCurrentSame, nullptr, nullptr, 0.0},
    {"render_pass_short_reentry_d1_source_mixed_invalid", CounterEntry::Kind::UnsignedCount, &Counters::renderPassShortReentryD1SourceMixedInvalid, nullptr, nullptr, 0.0},
    {"render_pass_short_reentry_d2_source_all_same", CounterEntry::Kind::UnsignedCount, &Counters::renderPassShortReentryD2SourceAllSame, nullptr, nullptr, 0.0},
    {"render_pass_short_reentry_d2_source_prior_intervening_same_current_newer", CounterEntry::Kind::UnsignedCount, &Counters::renderPassShortReentryD2SourcePriorInterveningSameCurrentNewer, nullptr, nullptr, 0.0},
    {"render_pass_short_reentry_d2_source_prior_older_intervening_current_same", CounterEntry::Kind::UnsignedCount, &Counters::renderPassShortReentryD2SourcePriorOlderInterveningCurrentSame, nullptr, nullptr, 0.0},
    {"render_pass_short_reentry_d2_source_mixed_invalid", CounterEntry::Kind::UnsignedCount, &Counters::renderPassShortReentryD2SourceMixedInvalid, nullptr, nullptr, 0.0},
    {"render_pass_short_reentry_close_final", CounterEntry::Kind::UnsignedCount, &Counters::renderPassShortReentryCloseFinal, nullptr, nullptr, 0.0},
    {"render_pass_short_reentry_close_rt_change", CounterEntry::Kind::UnsignedCount, &Counters::renderPassShortReentryCloseRtChange, nullptr, nullptr, 0.0},
    {"render_pass_short_reentry_close_hazard", CounterEntry::Kind::UnsignedCount, &Counters::renderPassShortReentryCloseHazard, nullptr, nullptr, 0.0},
    {"render_pass_short_reentry_close_clear", CounterEntry::Kind::UnsignedCount, &Counters::renderPassShortReentryCloseClear, nullptr, nullptr, 0.0},
    {"render_pass_short_reentry_close_surface_copy", CounterEntry::Kind::UnsignedCount, &Counters::renderPassShortReentryCloseSurfaceCopy, nullptr, nullptr, 0.0},
    {"render_pass_short_reentry_close_stretch_rect", CounterEntry::Kind::UnsignedCount, &Counters::renderPassShortReentryCloseStretchRect, nullptr, nullptr, 0.0},
    {"render_pass_short_reentry_close_readback", CounterEntry::Kind::UnsignedCount, &Counters::renderPassShortReentryCloseReadback, nullptr, nullptr, 0.0},
    {"render_pass_short_reentry_close_color_fill", CounterEntry::Kind::UnsignedCount, &Counters::renderPassShortReentryCloseColorFill, nullptr, nullptr, 0.0},
    {"render_pass_short_reentry_close_present", CounterEntry::Kind::UnsignedCount, &Counters::renderPassShortReentryClosePresent, nullptr, nullptr, 0.0},
    {"render_pass_short_reentry_close_present_acquire", CounterEntry::Kind::UnsignedCount, &Counters::renderPassShortReentryClosePresentAcquire, nullptr, nullptr, 0.0},
    {"render_pass_short_reentry_close_tile", CounterEntry::Kind::UnsignedCount, &Counters::renderPassShortReentryCloseTile, nullptr, nullptr, 0.0},
    {"render_pass_short_reentry_close_ordered", CounterEntry::Kind::UnsignedCount, &Counters::renderPassShortReentryCloseOrdered, nullptr, nullptr, 0.0},
    {"render_pass_short_reentry_close_missing", CounterEntry::Kind::UnsignedCount, &Counters::renderPassShortReentryCloseMissing, nullptr, nullptr, 0.0},
    {"render_pass_short_reentry_clear_open_target_count", CounterEntry::Kind::UnsignedCount, &Counters::renderPassShortReentryClearOpenTargetCount, nullptr, nullptr, 0.0},
    {"render_pass_short_reentry_clear_open_target_prior_store_bytes", CounterEntry::Kind::UnsignedCount, &Counters::renderPassShortReentryClearOpenTargetPriorStoreBytes, nullptr, nullptr, 0.0},
    {"render_pass_short_reentry_clear_open_target_current_load_bytes", CounterEntry::Kind::UnsignedCount, &Counters::renderPassShortReentryClearOpenTargetCurrentLoadBytes, nullptr, nullptr, 0.0},
    {"render_pass_short_reentry_clear_open_natural_cross_count", CounterEntry::Kind::UnsignedCount, &Counters::renderPassShortReentryClearOpenNaturalCrossCount, nullptr, nullptr, 0.0},
    {"render_pass_short_reentry_clear_open_natural_cross_prior_store_bytes", CounterEntry::Kind::UnsignedCount, &Counters::renderPassShortReentryClearOpenNaturalCrossPriorStoreBytes, nullptr, nullptr, 0.0},
    {"render_pass_short_reentry_clear_open_natural_cross_current_load_bytes", CounterEntry::Kind::UnsignedCount, &Counters::renderPassShortReentryClearOpenNaturalCrossCurrentLoadBytes, nullptr, nullptr, 0.0},
    {"render_pass_close_ledger_recorded", CounterEntry::Kind::UnsignedCount, &Counters::renderPassCloseLedgerRecorded, nullptr, nullptr, 0.0},
    {"render_pass_close_ledger_missing", CounterEntry::Kind::UnsignedCount, &Counters::renderPassCloseLedgerMissing, nullptr, nullptr, 0.0},
    {"render_pass_close_ledger_terminal_adjacent", CounterEntry::Kind::UnsignedCount, &Counters::renderPassCloseLedgerTerminalAdjacent, nullptr, nullptr, 0.0},
    {"render_pass_close_ledger_terminal_nonadjacent", CounterEntry::Kind::UnsignedCount, &Counters::renderPassCloseLedgerTerminalNonAdjacent, nullptr, nullptr, 0.0},
    {"render_pass_close_ledger_terminal_not_reopened_before_present", CounterEntry::Kind::UnsignedCount, &Counters::renderPassCloseLedgerTerminalNotReopenedBeforePresent, nullptr, nullptr, 0.0},
    {"render_pass_final_close_ledger_recorded", CounterEntry::Kind::UnsignedCount, &Counters::renderPassFinalCloseLedgerRecorded, nullptr, nullptr, 0.0},
    {"render_pass_final_close_ledger_missing", CounterEntry::Kind::UnsignedCount, &Counters::renderPassFinalCloseLedgerMissing, nullptr, nullptr, 0.0},
    {"render_pass_final_close_ledger_terminal_adjacent", CounterEntry::Kind::UnsignedCount, &Counters::renderPassFinalCloseLedgerTerminalAdjacent, nullptr, nullptr, 0.0},
    {"render_pass_final_close_ledger_terminal_nonadjacent", CounterEntry::Kind::UnsignedCount, &Counters::renderPassFinalCloseLedgerTerminalNonAdjacent, nullptr, nullptr, 0.0},
    {"render_pass_final_close_ledger_terminal_not_reopened_before_present", CounterEntry::Kind::UnsignedCount, &Counters::renderPassFinalCloseLedgerTerminalNotReopenedBeforePresent, nullptr, nullptr, 0.0},
    {"render_pass_transition_rt_change_same_depth", CounterEntry::Kind::UnsignedCount, &Counters::renderPassTransitionRtChangeSameDepth, nullptr, nullptr, 0.0},
    {"render_pass_transition_same_rt_depth_change", CounterEntry::Kind::UnsignedCount, &Counters::renderPassTransitionSameRtDepthChange, nullptr, nullptr, 0.0},
    {"render_pass_transition_rt_depth_change", CounterEntry::Kind::UnsignedCount, &Counters::renderPassTransitionRtDepthChange, nullptr, nullptr, 0.0},
    {"render_pass_color_proof_allow_next_clear", CounterEntry::Kind::UnsignedCount, &Counters::renderPassColorProofAllowNextClear, nullptr, nullptr, 0.0},
    {"render_pass_color_proof_allow_dead_no_present", CounterEntry::Kind::UnsignedCount, &Counters::renderPassColorProofAllowDeadNoPresent, nullptr, nullptr, 0.0},
    {"render_pass_color_proof_block_null_color", CounterEntry::Kind::UnsignedCount, &Counters::renderPassColorProofBlockNullColor, nullptr, nullptr, 0.0},
    {"render_pass_color_proof_block_no_lookahead", CounterEntry::Kind::UnsignedCount, &Counters::renderPassColorProofBlockNoLookahead, nullptr, nullptr, 0.0},
    {"render_pass_color_proof_block_draw_target", CounterEntry::Kind::UnsignedCount, &Counters::renderPassColorProofBlockDrawTarget, nullptr, nullptr, 0.0},
    {"render_pass_color_proof_block_texture_sample", CounterEntry::Kind::UnsignedCount, &Counters::renderPassColorProofBlockTextureSample, nullptr, nullptr, 0.0},
    {"render_pass_color_proof_block_surface_copy", CounterEntry::Kind::UnsignedCount, &Counters::renderPassColorProofBlockSurfaceCopy, nullptr, nullptr, 0.0},
    {"render_pass_color_proof_block_stretch_rect", CounterEntry::Kind::UnsignedCount, &Counters::renderPassColorProofBlockStretchRect, nullptr, nullptr, 0.0},
    {"render_pass_color_proof_block_readback", CounterEntry::Kind::UnsignedCount, &Counters::renderPassColorProofBlockReadback, nullptr, nullptr, 0.0},
    {"render_pass_color_proof_block_color_fill", CounterEntry::Kind::UnsignedCount, &Counters::renderPassColorProofBlockColorFill, nullptr, nullptr, 0.0},
    {"render_pass_color_proof_block_msaa_resolve", CounterEntry::Kind::UnsignedCount, &Counters::renderPassColorProofBlockMsaaResolve, nullptr, nullptr, 0.0},
    {"render_pass_color_proof_block_present", CounterEntry::Kind::UnsignedCount, &Counters::renderPassColorProofBlockPresent, nullptr, nullptr, 0.0},
    {"render_pass_color_proof_block_dead_no_present_disabled", CounterEntry::Kind::UnsignedCount, &Counters::renderPassColorProofBlockDeadNoPresentDisabled, nullptr, nullptr, 0.0},
    {"render_pass_color_proof_block_clear_mismatch", CounterEntry::Kind::UnsignedCount, &Counters::renderPassColorProofBlockClearMismatch, nullptr, nullptr, 0.0},
    {"render_pass_depth_proof_allow_next_clear", CounterEntry::Kind::UnsignedCount, &Counters::renderPassDepthProofAllowNextClear, nullptr, nullptr, 0.0},
    {"render_pass_depth_proof_allow_dead_no_present", CounterEntry::Kind::UnsignedCount, &Counters::renderPassDepthProofAllowDeadNoPresent, nullptr, nullptr, 0.0},
    {"render_pass_depth_proof_block_null_depth", CounterEntry::Kind::UnsignedCount, &Counters::renderPassDepthProofBlockNullDepth, nullptr, nullptr, 0.0},
    {"render_pass_depth_proof_block_no_lookahead", CounterEntry::Kind::UnsignedCount, &Counters::renderPassDepthProofBlockNoLookahead, nullptr, nullptr, 0.0},
    {"render_pass_depth_proof_block_msaa_resolve", CounterEntry::Kind::UnsignedCount, &Counters::renderPassDepthProofBlockMsaaResolve, nullptr, nullptr, 0.0},
    {"render_pass_depth_proof_block_draw_depth", CounterEntry::Kind::UnsignedCount, &Counters::renderPassDepthProofBlockDrawDepth, nullptr, nullptr, 0.0},
    {"render_pass_depth_proof_block_shadow_sample", CounterEntry::Kind::UnsignedCount, &Counters::renderPassDepthProofBlockShadowSample, nullptr, nullptr, 0.0},
    {"render_pass_depth_proof_block_surface_copy", CounterEntry::Kind::UnsignedCount, &Counters::renderPassDepthProofBlockSurfaceCopy, nullptr, nullptr, 0.0},
    {"render_pass_depth_proof_block_stretch_rect", CounterEntry::Kind::UnsignedCount, &Counters::renderPassDepthProofBlockStretchRect, nullptr, nullptr, 0.0},
    {"render_pass_depth_proof_block_readback", CounterEntry::Kind::UnsignedCount, &Counters::renderPassDepthProofBlockReadback, nullptr, nullptr, 0.0},
    {"render_pass_depth_proof_block_color_fill", CounterEntry::Kind::UnsignedCount, &Counters::renderPassDepthProofBlockColorFill, nullptr, nullptr, 0.0},
    {"render_pass_depth_proof_block_depth_resolve", CounterEntry::Kind::UnsignedCount, &Counters::renderPassDepthProofBlockDepthResolve, nullptr, nullptr, 0.0},
    {"render_pass_depth_proof_block_present", CounterEntry::Kind::UnsignedCount, &Counters::renderPassDepthProofBlockPresent, nullptr, nullptr, 0.0},
    {"render_pass_depth_proof_block_clear_mismatch", CounterEntry::Kind::UnsignedCount, &Counters::renderPassDepthProofBlockClearMismatch, nullptr, nullptr, 0.0},
    {"render_pass_no_lookahead_empty", CounterEntry::Kind::UnsignedCount, &Counters::renderPassNoLookaheadEmpty, nullptr, nullptr, 0.0},
    {"render_pass_no_lookahead_invalid", CounterEntry::Kind::UnsignedCount, &Counters::renderPassNoLookaheadInvalid, nullptr, nullptr, 0.0},
    {"render_pass_no_lookahead_suffix_exhausted", CounterEntry::Kind::UnsignedCount, &Counters::renderPassNoLookaheadSuffixExhausted, nullptr, nullptr, 0.0},
    {"render_pass_no_lookahead_storage_truncated", CounterEntry::Kind::UnsignedCount, &Counters::renderPassNoLookaheadStorageTruncated, nullptr, nullptr, 0.0},
    {"render_pass_late_store_unknown_color", CounterEntry::Kind::UnsignedCount, &Counters::renderPassLateStoreUnknownColor, nullptr, nullptr, 0.0},
    {"render_pass_late_store_unknown_depth", CounterEntry::Kind::UnsignedCount, &Counters::renderPassLateStoreUnknownDepth, nullptr, nullptr, 0.0},
    {"render_pass_late_store_unknown_stencil", CounterEntry::Kind::UnsignedCount, &Counters::renderPassLateStoreUnknownStencil, nullptr, nullptr, 0.0},
    {"render_pass_late_store_resolve_clear_color", CounterEntry::Kind::UnsignedCount, &Counters::renderPassLateStoreResolveClearColor, nullptr, nullptr, 0.0},
    {"render_pass_late_store_resolve_clear_depth", CounterEntry::Kind::UnsignedCount, &Counters::renderPassLateStoreResolveClearDepth, nullptr, nullptr, 0.0},
    {"render_pass_late_store_resolve_clear_stencil", CounterEntry::Kind::UnsignedCount, &Counters::renderPassLateStoreResolveClearStencil, nullptr, nullptr, 0.0},
    {"render_pass_late_store_resolve_store_clear_mismatch", CounterEntry::Kind::UnsignedCount, &Counters::renderPassLateStoreResolveStoreClearMismatch, nullptr, nullptr, 0.0},
    {"render_pass_late_store_resolve_store_draw", CounterEntry::Kind::UnsignedCount, &Counters::renderPassLateStoreResolveStoreDraw, nullptr, nullptr, 0.0},
    {"render_pass_late_store_resolve_store_sample", CounterEntry::Kind::UnsignedCount, &Counters::renderPassLateStoreResolveStoreSample, nullptr, nullptr, 0.0},
    {"render_pass_late_store_resolve_store_readback", CounterEntry::Kind::UnsignedCount, &Counters::renderPassLateStoreResolveStoreReadback, nullptr, nullptr, 0.0},
    {"render_pass_late_store_resolve_store_copy", CounterEntry::Kind::UnsignedCount, &Counters::renderPassLateStoreResolveStoreCopy, nullptr, nullptr, 0.0},
    {"render_pass_late_store_resolve_store_resolve", CounterEntry::Kind::UnsignedCount, &Counters::renderPassLateStoreResolveStoreResolve, nullptr, nullptr, 0.0},
    {"render_pass_late_store_resolve_store_present", CounterEntry::Kind::UnsignedCount, &Counters::renderPassLateStoreResolveStorePresent, nullptr, nullptr, 0.0},
    {"render_pass_late_store_resolve_store_incompatible_close", CounterEntry::Kind::UnsignedCount, &Counters::renderPassLateStoreResolveStoreIncompatibleClose, nullptr, nullptr, 0.0},
    {"render_pass_late_store_resolve_store_drain", CounterEntry::Kind::UnsignedCount, &Counters::renderPassLateStoreResolveStoreDrain, nullptr, nullptr, 0.0},
    {"render_pass_late_store_resolve_store_finalize", CounterEntry::Kind::UnsignedCount, &Counters::renderPassLateStoreResolveStoreFinalize, nullptr, nullptr, 0.0},
    {"render_pass_late_store_resolve_store_error", CounterEntry::Kind::UnsignedCount, &Counters::renderPassLateStoreResolveStoreError, nullptr, nullptr, 0.0},
    {"command_buffer_create_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::commandBufferCreateCpuNs, nullptr, nullptr, 0.0},
    {"command_buffer_create_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::commandBufferCreateCpuMaxNs, nullptr, nullptr, 0.0},
    {"command_buffer_create_cpu_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::commandBufferCreateCpuRing, 0.5},
    {"command_buffer_create_cpu_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::commandBufferCreateCpuRing, 0.95},
    {"command_buffer_create_cpu_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::commandBufferCreateCpuRing, 0.99},
    {"command_buffer_commit_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::commandBufferCommitCpuNs, nullptr, nullptr, 0.0},
    {"command_buffer_commit_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::commandBufferCommitCpuMaxNs, nullptr, nullptr, 0.0},
    {"command_buffer_commit_cpu_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::commandBufferCommitCpuRing, 0.5},
    {"command_buffer_commit_cpu_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::commandBufferCommitCpuRing, 0.95},
    {"command_buffer_commit_cpu_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::commandBufferCommitCpuRing, 0.99},
    // V1 boundary B2 — whole commit_chunk bridge-call latency in raw
    // nanoseconds. Sum + max + 3 percentiles.
    {"bridge_commit_latency_ns", CounterEntry::Kind::UnsignedCount, &Counters::bridgeCommitLatencyNs, nullptr, nullptr, 0.0},
    {"bridge_commit_latency_max_ns", CounterEntry::Kind::UnsignedCount, &Counters::bridgeCommitLatencyMaxNs, nullptr, nullptr, 0.0},
    {"bridge_commit_latency_p50_ns", CounterEntry::Kind::PercentileNs, nullptr, nullptr, &Counters::bridgeCommitLatencyRing, 0.5},
    {"bridge_commit_latency_p95_ns", CounterEntry::Kind::PercentileNs, nullptr, nullptr, &Counters::bridgeCommitLatencyRing, 0.95},
    {"bridge_commit_latency_p99_ns", CounterEntry::Kind::PercentileNs, nullptr, nullptr, &Counters::bridgeCommitLatencyRing, 0.99},
    {"commit_chunk_import_cpu_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::commitChunkImportCpuRing, 0.5},
    {"commit_chunk_import_cpu_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::commitChunkImportCpuRing, 0.95},
    {"commit_chunk_import_cpu_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::commitChunkImportCpuRing, 0.99},
    {"commit_chunk_handle_cpu_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::commitChunkHandleCpuRing, 0.5},
    {"commit_chunk_handle_cpu_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::commitChunkHandleCpuRing, 0.95},
    {"commit_chunk_handle_cpu_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::commitChunkHandleCpuRing, 0.99},
    {"commit_chunk_replay_cpu_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::commitChunkReplayCpuRing, 0.5},
    {"commit_chunk_replay_cpu_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::commitChunkReplayCpuRing, 0.95},
    {"commit_chunk_replay_cpu_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::commitChunkReplayCpuRing, 0.99},
    {"commit_chunk_draw_batch_submit_cpu_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::commitChunkDrawBatchSubmitCpuRing, 0.5},
    {"commit_chunk_draw_batch_submit_cpu_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::commitChunkDrawBatchSubmitCpuRing, 0.95},
    {"commit_chunk_draw_batch_submit_cpu_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::commitChunkDrawBatchSubmitCpuRing, 0.99},
    // Commit-replay offload path (DXMT9_OFFLOAD_COMMIT_REPLAY).
    {"commit_chunk_raw_enqueue_cpu_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::commitChunkRawEnqueueCpuRing, 0.5},
    {"commit_chunk_raw_enqueue_cpu_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::commitChunkRawEnqueueCpuRing, 0.95},
    {"commit_chunk_raw_enqueue_cpu_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::commitChunkRawEnqueueCpuRing, 0.99},
    {"offload_replay_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::offloadReplayCpuNs, nullptr, nullptr, 0.0},
    {"offload_replay_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::offloadReplayCpuMaxNs, nullptr, nullptr, 0.0},
    {"offload_replay_cpu_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::offloadReplayCpuRing, 0.5},
    {"offload_replay_cpu_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::offloadReplayCpuRing, 0.95},
    {"offload_replay_cpu_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::offloadReplayCpuRing, 0.99},
    {"offload_replay_queue_depth_samples", CounterEntry::Kind::UnsignedCount, &Counters::offloadReplayQueueDepthSamples, nullptr, nullptr, 0.0},
    {"offload_replay_queue_depth_total", CounterEntry::Kind::UnsignedCount, &Counters::offloadReplayQueueDepthTotal, nullptr, nullptr, 0.0},
    {"offload_replay_queue_depth_max", CounterEntry::Kind::UnsignedCount, &Counters::offloadReplayQueueDepthMax, nullptr, nullptr, 0.0},
    {"offload_replay_queue_depth_gt1", CounterEntry::Kind::UnsignedCount, &Counters::offloadReplayQueueDepthGt1, nullptr, nullptr, 0.0},
    {"offload_replay_queue_depth_gt2", CounterEntry::Kind::UnsignedCount, &Counters::offloadReplayQueueDepthGt2, nullptr, nullptr, 0.0},
    {"offload_replay_queue_depth_gt4", CounterEntry::Kind::UnsignedCount, &Counters::offloadReplayQueueDepthGt4, nullptr, nullptr, 0.0},
    {"offload_drain_fence_waits", CounterEntry::Kind::UnsignedCount, &Counters::offloadDrainFenceWaits, nullptr, nullptr, 0.0},
    {"offload_drain_fence_wait_ms", CounterEntry::Kind::Milliseconds, &Counters::offloadDrainFenceWaitNs, nullptr, nullptr, 0.0},
    {"offload_commit_app_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::offloadCommitAppNs, nullptr, nullptr, 0.0},
    {"commit_chunk_phase_calls", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkPhaseCalls, nullptr, nullptr, 0.0},
    {"commit_chunk_phase_prepare_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::commitChunkPhasePrepareNs, nullptr, nullptr, 0.0},
    {"commit_chunk_phase_import_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::commitChunkPhaseImportNs, nullptr, nullptr, 0.0},
    {"commit_chunk_phase_mark_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::commitChunkPhaseMarkNs, nullptr, nullptr, 0.0},
    {"commit_chunk_phase_mark_lock_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::commitChunkPhaseMarkLockNs, nullptr, nullptr, 0.0},
    {"commit_chunk_phase_enqueue_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::commitChunkPhaseEnqueueNs, nullptr, nullptr, 0.0},
    {"commit_chunk_phase_present_wait_ms", CounterEntry::Kind::Milliseconds, &Counters::commitChunkPhasePresentWaitNs, nullptr, nullptr, 0.0},
    {"offload_push_backpressure_waits", CounterEntry::Kind::UnsignedCount, &Counters::offloadPushBackpressureWaits, nullptr, nullptr, 0.0},
    {"offload_push_backpressure_wait_ms", CounterEntry::Kind::Milliseconds, &Counters::offloadPushBackpressureWaitNs, nullptr, nullptr, 0.0},
    {"offload_worker_idle_wait_ms", CounterEntry::Kind::Milliseconds, &Counters::offloadWorkerIdleWaitNs, nullptr, nullptr, 0.0},
    {"completion_enqueue_samples", CounterEntry::Kind::UnsignedCount, &Counters::completionEnqueueSamples, nullptr, nullptr, 0.0},
    {"completion_enqueue_pending_depth_max", CounterEntry::Kind::UnsignedCount, &Counters::completionEnqueuePendingDepthMax, nullptr, nullptr, 0.0},
    {"completion_enqueue_while_waiting", CounterEntry::Kind::UnsignedCount, &Counters::completionEnqueueWhileWaiting, nullptr, nullptr, 0.0},
    {"completion_enqueue_while_waiting_present", CounterEntry::Kind::UnsignedCount, &Counters::completionEnqueueWhileWaitingPresent, nullptr, nullptr, 0.0},
    {"completion_wait_with_enqueue", CounterEntry::Kind::UnsignedCount, &Counters::completionWaitWithEnqueue, nullptr, nullptr, 0.0},
    {"completion_wait_with_enqueue_ms", CounterEntry::Kind::Milliseconds, &Counters::completionWaitWithEnqueueNs, nullptr, nullptr, 0.0},
    {"completion_wait_without_enqueue", CounterEntry::Kind::UnsignedCount, &Counters::completionWaitWithoutEnqueue, nullptr, nullptr, 0.0},
    {"completion_wait_without_enqueue_ms", CounterEntry::Kind::Milliseconds, &Counters::completionWaitWithoutEnqueueNs, nullptr, nullptr, 0.0},
    {"completion_present_wait_with_enqueue", CounterEntry::Kind::UnsignedCount, &Counters::completionPresentWaitWithEnqueue, nullptr, nullptr, 0.0},
    {"completion_present_wait_with_enqueue_ms", CounterEntry::Kind::Milliseconds, &Counters::completionPresentWaitWithEnqueueNs, nullptr, nullptr, 0.0},
    {"completion_present_wait_without_enqueue", CounterEntry::Kind::UnsignedCount, &Counters::completionPresentWaitWithoutEnqueue, nullptr, nullptr, 0.0},
    {"completion_present_wait_without_enqueue_ms", CounterEntry::Kind::Milliseconds, &Counters::completionPresentWaitWithoutEnqueueNs, nullptr, nullptr, 0.0},
    {"completion_wait_enqueues_during_wait", CounterEntry::Kind::UnsignedCount, &Counters::completionWaitEnqueuesDuringWait, nullptr, nullptr, 0.0},
    {"completion_wait_enqueues_during_wait_max", CounterEntry::Kind::UnsignedCount, &Counters::completionWaitEnqueuesDuringWaitMax, nullptr, nullptr, 0.0},
    {"completion_signal_delay", CounterEntry::Kind::UnsignedCount, &Counters::completionSignalDelay, nullptr, nullptr, 0.0},
    {"completion_signal_delay_ms", CounterEntry::Kind::Milliseconds, &Counters::completionSignalDelayNs, nullptr, nullptr, 0.0},
    {"completion_wait_commit_chunk_entries", CounterEntry::Kind::UnsignedCount, &Counters::completionWaitCommitChunkEntries, nullptr, nullptr, 0.0},
    {"completion_wait_commit_chunk_replay_starts", CounterEntry::Kind::UnsignedCount, &Counters::completionWaitCommitChunkReplayStarts, nullptr, nullptr, 0.0},
    {"completion_wait_commit_chunk_replay_ends", CounterEntry::Kind::UnsignedCount, &Counters::completionWaitCommitChunkReplayEnds, nullptr, nullptr, 0.0},
    {"completion_wait_commit_chunk_replay_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::completionWaitCommitChunkReplayCpuNs, nullptr, nullptr, 0.0},
    {"completion_wait_commit_chunk_replay_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::completionWaitCommitChunkReplayCpuMaxNs, nullptr, nullptr, 0.0},
    {"completion_wait_commit_chunk_replay_cpu_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionWaitCommitChunkReplayCpuRing, 0.5},
    {"completion_wait_commit_chunk_replay_cpu_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionWaitCommitChunkReplayCpuRing, 0.95},
    {"completion_wait_commit_chunk_replay_cpu_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionWaitCommitChunkReplayCpuRing, 0.99},
    {"completion_wait_commit_publish", CounterEntry::Kind::UnsignedCount, &Counters::completionWaitCommitPublishes, nullptr, nullptr, 0.0},
    {"completion_wait_encode_dequeue", CounterEntry::Kind::UnsignedCount, &Counters::completionWaitEncodeDequeues, nullptr, nullptr, 0.0},
    {"completion_wait_command_buffer_commit", CounterEntry::Kind::UnsignedCount, &Counters::completionWaitCommandBufferCommits, nullptr, nullptr, 0.0},
    {"completion_wait_stage_publish_to_encode_dequeue", CounterEntry::Kind::UnsignedCount, &Counters::completionWaitStagePublishToEncodeDequeue, nullptr, nullptr, 0.0},
    {"completion_wait_stage_publish_to_encode_dequeue_ms", CounterEntry::Kind::Milliseconds, &Counters::completionWaitStagePublishToEncodeDequeueNs, nullptr, nullptr, 0.0},
    {"completion_wait_stage_publish_to_encode_dequeue_max_ms", CounterEntry::Kind::Milliseconds, &Counters::completionWaitStagePublishToEncodeDequeueMaxNs, nullptr, nullptr, 0.0},
    {"completion_wait_stage_publish_to_encode_dequeue_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionWaitStagePublishToEncodeDequeueRing, 0.5},
    {"completion_wait_stage_publish_to_encode_dequeue_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionWaitStagePublishToEncodeDequeueRing, 0.95},
    {"completion_wait_stage_publish_to_encode_dequeue_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionWaitStagePublishToEncodeDequeueRing, 0.99},
    {"completion_wait_stage_encode_dequeue_to_command_buffer_commit", CounterEntry::Kind::UnsignedCount, &Counters::completionWaitStageEncodeDequeueToCommandBufferCommit, nullptr, nullptr, 0.0},
    {"completion_wait_stage_encode_dequeue_to_command_buffer_commit_ms", CounterEntry::Kind::Milliseconds, &Counters::completionWaitStageEncodeDequeueToCommandBufferCommitNs, nullptr, nullptr, 0.0},
    {"completion_wait_stage_encode_dequeue_to_command_buffer_commit_max_ms", CounterEntry::Kind::Milliseconds, &Counters::completionWaitStageEncodeDequeueToCommandBufferCommitMaxNs, nullptr, nullptr, 0.0},
    {"completion_wait_stage_encode_dequeue_to_command_buffer_commit_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionWaitStageEncodeDequeueToCommandBufferCommitRing, 0.5},
    {"completion_wait_stage_encode_dequeue_to_command_buffer_commit_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionWaitStageEncodeDequeueToCommandBufferCommitRing, 0.95},
    {"completion_wait_stage_encode_dequeue_to_command_buffer_commit_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionWaitStageEncodeDequeueToCommandBufferCommitRing, 0.99},
    {"completion_no_enqueue_wait_to_commit_chunk_entry", CounterEntry::Kind::UnsignedCount, &Counters::completionNoEnqueueWaitToCommitChunkEntry, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_wait_to_commit_chunk_entry_ms", CounterEntry::Kind::Milliseconds, &Counters::completionNoEnqueueWaitToCommitChunkEntryNs, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_wait_to_commit_chunk_entry_max_ms", CounterEntry::Kind::Milliseconds, &Counters::completionNoEnqueueWaitToCommitChunkEntryMaxNs, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_wait_to_commit_chunk_entry_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionNoEnqueueWaitToCommitChunkEntryRing, 0.5},
    {"completion_no_enqueue_wait_to_commit_chunk_entry_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionNoEnqueueWaitToCommitChunkEntryRing, 0.95},
    {"completion_no_enqueue_wait_to_commit_chunk_entry_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionNoEnqueueWaitToCommitChunkEntryRing, 0.99},
    {"completion_no_enqueue_wait_to_commit_chunk_replay_start", CounterEntry::Kind::UnsignedCount, &Counters::completionNoEnqueueWaitToCommitChunkReplayStart, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_wait_to_commit_chunk_replay_start_ms", CounterEntry::Kind::Milliseconds, &Counters::completionNoEnqueueWaitToCommitChunkReplayStartNs, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_wait_to_commit_chunk_replay_start_max_ms", CounterEntry::Kind::Milliseconds, &Counters::completionNoEnqueueWaitToCommitChunkReplayStartMaxNs, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_wait_to_commit_chunk_replay_start_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionNoEnqueueWaitToCommitChunkReplayStartRing, 0.5},
    {"completion_no_enqueue_wait_to_commit_chunk_replay_start_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionNoEnqueueWaitToCommitChunkReplayStartRing, 0.95},
    {"completion_no_enqueue_wait_to_commit_chunk_replay_start_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionNoEnqueueWaitToCommitChunkReplayStartRing, 0.99},
    {"completion_no_enqueue_wait_to_commit_chunk_replay_end", CounterEntry::Kind::UnsignedCount, &Counters::completionNoEnqueueWaitToCommitChunkReplayEnd, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_wait_to_commit_chunk_replay_end_ms", CounterEntry::Kind::Milliseconds, &Counters::completionNoEnqueueWaitToCommitChunkReplayEndNs, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_wait_to_commit_chunk_replay_end_max_ms", CounterEntry::Kind::Milliseconds, &Counters::completionNoEnqueueWaitToCommitChunkReplayEndMaxNs, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_wait_to_commit_chunk_replay_end_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionNoEnqueueWaitToCommitChunkReplayEndRing, 0.5},
    {"completion_no_enqueue_wait_to_commit_chunk_replay_end_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionNoEnqueueWaitToCommitChunkReplayEndRing, 0.95},
    {"completion_no_enqueue_wait_to_commit_chunk_replay_end_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionNoEnqueueWaitToCommitChunkReplayEndRing, 0.99},
    {"completion_no_enqueue_wait_to_commit_publish", CounterEntry::Kind::UnsignedCount, &Counters::completionNoEnqueueWaitToCommitPublish, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_wait_to_commit_publish_ms", CounterEntry::Kind::Milliseconds, &Counters::completionNoEnqueueWaitToCommitPublishNs, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_wait_to_commit_publish_max_ms", CounterEntry::Kind::Milliseconds, &Counters::completionNoEnqueueWaitToCommitPublishMaxNs, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_wait_to_commit_publish_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionNoEnqueueWaitToCommitPublishRing, 0.5},
    {"completion_no_enqueue_wait_to_commit_publish_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionNoEnqueueWaitToCommitPublishRing, 0.95},
    {"completion_no_enqueue_wait_to_commit_publish_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionNoEnqueueWaitToCommitPublishRing, 0.99},
    {"completion_no_enqueue_commit_chunk_entries_before_publish", CounterEntry::Kind::UnsignedCount, &Counters::completionNoEnqueueCommitChunkEntriesBeforePublish, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_commit_chunk_entries_before_publish_max", CounterEntry::Kind::UnsignedCount, &Counters::completionNoEnqueueCommitChunkEntriesBeforePublishMax, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_commit_chunk_entries_before_publish_p50", CounterEntry::Kind::PercentileNs, nullptr, nullptr, &Counters::completionNoEnqueueCommitChunkEntriesBeforePublishRing, 0.5},
    {"completion_no_enqueue_commit_chunk_entries_before_publish_p95", CounterEntry::Kind::PercentileNs, nullptr, nullptr, &Counters::completionNoEnqueueCommitChunkEntriesBeforePublishRing, 0.95},
    {"completion_no_enqueue_commit_chunk_entries_before_publish_p99", CounterEntry::Kind::PercentileNs, nullptr, nullptr, &Counters::completionNoEnqueueCommitChunkEntriesBeforePublishRing, 0.99},
    {"completion_no_enqueue_commit_chunk_replay_starts_before_publish", CounterEntry::Kind::UnsignedCount, &Counters::completionNoEnqueueCommitChunkReplayStartsBeforePublish, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_commit_chunk_replay_starts_before_publish_max", CounterEntry::Kind::UnsignedCount, &Counters::completionNoEnqueueCommitChunkReplayStartsBeforePublishMax, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_commit_chunk_replay_starts_before_publish_p50", CounterEntry::Kind::PercentileNs, nullptr, nullptr, &Counters::completionNoEnqueueCommitChunkReplayStartsBeforePublishRing, 0.5},
    {"completion_no_enqueue_commit_chunk_replay_starts_before_publish_p95", CounterEntry::Kind::PercentileNs, nullptr, nullptr, &Counters::completionNoEnqueueCommitChunkReplayStartsBeforePublishRing, 0.95},
    {"completion_no_enqueue_commit_chunk_replay_starts_before_publish_p99", CounterEntry::Kind::PercentileNs, nullptr, nullptr, &Counters::completionNoEnqueueCommitChunkReplayStartsBeforePublishRing, 0.99},
    {"completion_no_enqueue_commit_chunk_replay_ends_before_publish", CounterEntry::Kind::UnsignedCount, &Counters::completionNoEnqueueCommitChunkReplayEndsBeforePublish, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_commit_chunk_replay_ends_before_publish_max", CounterEntry::Kind::UnsignedCount, &Counters::completionNoEnqueueCommitChunkReplayEndsBeforePublishMax, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_commit_chunk_replay_ends_before_publish_p50", CounterEntry::Kind::PercentileNs, nullptr, nullptr, &Counters::completionNoEnqueueCommitChunkReplayEndsBeforePublishRing, 0.5},
    {"completion_no_enqueue_commit_chunk_replay_ends_before_publish_p95", CounterEntry::Kind::PercentileNs, nullptr, nullptr, &Counters::completionNoEnqueueCommitChunkReplayEndsBeforePublishRing, 0.95},
    {"completion_no_enqueue_commit_chunk_replay_ends_before_publish_p99", CounterEntry::Kind::PercentileNs, nullptr, nullptr, &Counters::completionNoEnqueueCommitChunkReplayEndsBeforePublishRing, 0.99},
    {"completion_no_enqueue_commit_chunk_completed_replay_cpu_before_publish", CounterEntry::Kind::UnsignedCount, &Counters::completionNoEnqueueCommitChunkCompletedReplayCpuBeforePublish, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_commit_chunk_completed_replay_cpu_before_publish_ms", CounterEntry::Kind::Milliseconds, &Counters::completionNoEnqueueCommitChunkCompletedReplayCpuBeforePublishNs, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_commit_chunk_completed_replay_cpu_before_publish_max_ms", CounterEntry::Kind::Milliseconds, &Counters::completionNoEnqueueCommitChunkCompletedReplayCpuBeforePublishMaxNs, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_commit_chunk_completed_replay_cpu_before_publish_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionNoEnqueueCommitChunkCompletedReplayCpuBeforePublishRing, 0.5},
    {"completion_no_enqueue_commit_chunk_completed_replay_cpu_before_publish_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionNoEnqueueCommitChunkCompletedReplayCpuBeforePublishRing, 0.95},
    {"completion_no_enqueue_commit_chunk_completed_replay_cpu_before_publish_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionNoEnqueueCommitChunkCompletedReplayCpuBeforePublishRing, 0.99},
    {"completion_no_enqueue_commit_chunk_active_replay_cpu_before_publish", CounterEntry::Kind::UnsignedCount, &Counters::completionNoEnqueueCommitChunkActiveReplayCpuBeforePublish, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_commit_chunk_active_replay_cpu_before_publish_ms", CounterEntry::Kind::Milliseconds, &Counters::completionNoEnqueueCommitChunkActiveReplayCpuBeforePublishNs, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_commit_chunk_active_replay_cpu_before_publish_max_ms", CounterEntry::Kind::Milliseconds, &Counters::completionNoEnqueueCommitChunkActiveReplayCpuBeforePublishMaxNs, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_commit_chunk_active_replay_cpu_before_publish_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionNoEnqueueCommitChunkActiveReplayCpuBeforePublishRing, 0.5},
    {"completion_no_enqueue_commit_chunk_active_replay_cpu_before_publish_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionNoEnqueueCommitChunkActiveReplayCpuBeforePublishRing, 0.95},
    {"completion_no_enqueue_commit_chunk_active_replay_cpu_before_publish_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionNoEnqueueCommitChunkActiveReplayCpuBeforePublishRing, 0.99},
    {"completion_no_enqueue_commit_chunk_inter_replay_gap_before_publish", CounterEntry::Kind::UnsignedCount, &Counters::completionNoEnqueueCommitChunkInterReplayGapBeforePublish, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_commit_chunk_inter_replay_gap_before_publish_ms", CounterEntry::Kind::Milliseconds, &Counters::completionNoEnqueueCommitChunkInterReplayGapBeforePublishNs, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_commit_chunk_inter_replay_gap_before_publish_max_ms", CounterEntry::Kind::Milliseconds, &Counters::completionNoEnqueueCommitChunkInterReplayGapBeforePublishMaxNs, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_commit_chunk_inter_replay_gap_before_publish_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionNoEnqueueCommitChunkInterReplayGapBeforePublishRing, 0.5},
    {"completion_no_enqueue_commit_chunk_inter_replay_gap_before_publish_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionNoEnqueueCommitChunkInterReplayGapBeforePublishRing, 0.95},
    {"completion_no_enqueue_commit_chunk_inter_replay_gap_before_publish_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionNoEnqueueCommitChunkInterReplayGapBeforePublishRing, 0.99},
    {"completion_no_enqueue_commit_publish_wait_before_publish", CounterEntry::Kind::UnsignedCount, &Counters::completionNoEnqueueCommitPublishWaitBeforePublish, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_commit_publish_wait_before_publish_ms", CounterEntry::Kind::Milliseconds, &Counters::completionNoEnqueueCommitPublishWaitBeforePublishNs, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_commit_publish_wait_before_publish_max_ms", CounterEntry::Kind::Milliseconds, &Counters::completionNoEnqueueCommitPublishWaitBeforePublishMaxNs, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_commit_publish_wait_before_publish_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionNoEnqueueCommitPublishWaitBeforePublishRing, 0.5},
    {"completion_no_enqueue_commit_publish_wait_before_publish_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionNoEnqueueCommitPublishWaitBeforePublishRing, 0.95},
    {"completion_no_enqueue_commit_publish_wait_before_publish_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionNoEnqueueCommitPublishWaitBeforePublishRing, 0.99},
    {"completion_no_enqueue_commit_publish_on_before_publish_cpu", CounterEntry::Kind::UnsignedCount, &Counters::completionNoEnqueueCommitPublishOnBeforePublishCpu, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_commit_publish_on_before_publish_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::completionNoEnqueueCommitPublishOnBeforePublishCpuNs, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_commit_publish_on_before_publish_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::completionNoEnqueueCommitPublishOnBeforePublishCpuMaxNs, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_commit_publish_on_before_publish_cpu_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionNoEnqueueCommitPublishOnBeforePublishCpuRing, 0.5},
    {"completion_no_enqueue_commit_publish_on_before_publish_cpu_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionNoEnqueueCommitPublishOnBeforePublishCpuRing, 0.95},
    {"completion_no_enqueue_commit_publish_on_before_publish_cpu_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionNoEnqueueCommitPublishOnBeforePublishCpuRing, 0.99},
    {"completion_no_enqueue_commit_chunk_shape_samples_before_publish", CounterEntry::Kind::UnsignedCount, &Counters::completionNoEnqueueCommitChunkShapeSamplesBeforePublish, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_commit_chunk_records_before_publish", CounterEntry::Kind::UnsignedCount, &Counters::completionNoEnqueueCommitChunkRecordsBeforePublish, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_commit_chunk_records_before_publish_max", CounterEntry::Kind::UnsignedCount, &Counters::completionNoEnqueueCommitChunkRecordsBeforePublishMax, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_commit_chunk_records_before_publish_p50", CounterEntry::Kind::PercentileNs, nullptr, nullptr, &Counters::completionNoEnqueueCommitChunkRecordsBeforePublishRing, 0.5},
    {"completion_no_enqueue_commit_chunk_records_before_publish_p95", CounterEntry::Kind::PercentileNs, nullptr, nullptr, &Counters::completionNoEnqueueCommitChunkRecordsBeforePublishRing, 0.95},
    {"completion_no_enqueue_commit_chunk_records_before_publish_p99", CounterEntry::Kind::PercentileNs, nullptr, nullptr, &Counters::completionNoEnqueueCommitChunkRecordsBeforePublishRing, 0.99},
    {"completion_no_enqueue_commit_chunk_chunks_with_draw_before_publish", CounterEntry::Kind::UnsignedCount, &Counters::completionNoEnqueueCommitChunkChunksWithDrawBeforePublish, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_commit_chunk_chunks_with_present_before_publish", CounterEntry::Kind::UnsignedCount, &Counters::completionNoEnqueueCommitChunkChunksWithPresentBeforePublish, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_commit_chunk_chunks_state_const_only_before_publish", CounterEntry::Kind::UnsignedCount, &Counters::completionNoEnqueueCommitChunkChunksStateConstOnlyBeforePublish, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_commit_chunk_chunks_no_draw_no_present_before_publish", CounterEntry::Kind::UnsignedCount, &Counters::completionNoEnqueueCommitChunkChunksNoDrawNoPresentBeforePublish, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_commit_chunk_draw_records_before_publish", CounterEntry::Kind::UnsignedCount, &Counters::completionNoEnqueueCommitChunkDrawRecordsBeforePublish, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_commit_chunk_const_records_before_publish", CounterEntry::Kind::UnsignedCount, &Counters::completionNoEnqueueCommitChunkConstRecordsBeforePublish, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_commit_chunk_apply_state_records_before_publish", CounterEntry::Kind::UnsignedCount, &Counters::completionNoEnqueueCommitChunkApplyStateRecordsBeforePublish, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_commit_chunk_clear_records_before_publish", CounterEntry::Kind::UnsignedCount, &Counters::completionNoEnqueueCommitChunkClearRecordsBeforePublish, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_commit_chunk_present_records_before_publish", CounterEntry::Kind::UnsignedCount, &Counters::completionNoEnqueueCommitChunkPresentRecordsBeforePublish, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_commit_chunk_surface_records_before_publish", CounterEntry::Kind::UnsignedCount, &Counters::completionNoEnqueueCommitChunkSurfaceRecordsBeforePublish, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_commit_chunk_query_records_before_publish", CounterEntry::Kind::UnsignedCount, &Counters::completionNoEnqueueCommitChunkQueryRecordsBeforePublish, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_commit_chunk_other_records_before_publish", CounterEntry::Kind::UnsignedCount, &Counters::completionNoEnqueueCommitChunkOtherRecordsBeforePublish, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_first_publish_slot_samples", CounterEntry::Kind::UnsignedCount, &Counters::completionNoEnqueueFirstPublishSlotSamples, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_first_publish_slot_commands", CounterEntry::Kind::UnsignedCount, &Counters::completionNoEnqueueFirstPublishSlotCommands, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_first_publish_slot_commands_max", CounterEntry::Kind::UnsignedCount, &Counters::completionNoEnqueueFirstPublishSlotCommandsMax, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_first_publish_slot_commands_p50", CounterEntry::Kind::PercentileNs, nullptr, nullptr, &Counters::completionNoEnqueueFirstPublishSlotCommandsRing, 0.5},
    {"completion_no_enqueue_first_publish_slot_commands_p95", CounterEntry::Kind::PercentileNs, nullptr, nullptr, &Counters::completionNoEnqueueFirstPublishSlotCommandsRing, 0.95},
    {"completion_no_enqueue_first_publish_slot_draw_run_commands", CounterEntry::Kind::UnsignedCount, &Counters::completionNoEnqueueFirstPublishSlotDrawRunCommands, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_first_publish_slot_draw_items", CounterEntry::Kind::UnsignedCount, &Counters::completionNoEnqueueFirstPublishSlotDrawItems, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_first_publish_slot_draw_items_max", CounterEntry::Kind::UnsignedCount, &Counters::completionNoEnqueueFirstPublishSlotDrawItemsMax, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_first_publish_slot_draw_items_p50", CounterEntry::Kind::PercentileNs, nullptr, nullptr, &Counters::completionNoEnqueueFirstPublishSlotDrawItemsRing, 0.5},
    {"completion_no_enqueue_first_publish_slot_draw_items_p95", CounterEntry::Kind::PercentileNs, nullptr, nullptr, &Counters::completionNoEnqueueFirstPublishSlotDrawItemsRing, 0.95},
    {"completion_no_enqueue_first_publish_slot_non_draw_commands", CounterEntry::Kind::UnsignedCount, &Counters::completionNoEnqueueFirstPublishSlotNonDrawCommands, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_first_publish_slot_payload_bytes", CounterEntry::Kind::UnsignedCount, &Counters::completionNoEnqueueFirstPublishSlotPayloadBytes, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_first_publish_slot_payload_bytes_max", CounterEntry::Kind::UnsignedCount, &Counters::completionNoEnqueueFirstPublishSlotPayloadBytesMax, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_first_publish_slot_payload_bytes_p50", CounterEntry::Kind::PercentileNs, nullptr, nullptr, &Counters::completionNoEnqueueFirstPublishSlotPayloadBytesRing, 0.5},
    {"completion_no_enqueue_first_publish_slot_payload_bytes_p95", CounterEntry::Kind::PercentileNs, nullptr, nullptr, &Counters::completionNoEnqueueFirstPublishSlotPayloadBytesRing, 0.95},
    {"completion_no_enqueue_first_publish_slot_present_commands", CounterEntry::Kind::UnsignedCount, &Counters::completionNoEnqueueFirstPublishSlotPresentCommands, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_first_publish_slot_pre_present_commands", CounterEntry::Kind::UnsignedCount, &Counters::completionNoEnqueueFirstPublishSlotPrePresentCommands, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_first_publish_slot_pre_present_commands_max", CounterEntry::Kind::UnsignedCount, &Counters::completionNoEnqueueFirstPublishSlotPrePresentCommandsMax, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_first_publish_slot_pre_present_commands_p50", CounterEntry::Kind::PercentileNs, nullptr, nullptr, &Counters::completionNoEnqueueFirstPublishSlotPrePresentCommandsRing, 0.5},
    {"completion_no_enqueue_first_publish_slot_pre_present_commands_p95", CounterEntry::Kind::PercentileNs, nullptr, nullptr, &Counters::completionNoEnqueueFirstPublishSlotPrePresentCommandsRing, 0.95},
    {"completion_no_enqueue_first_publish_slot_pre_present_draw_run_commands", CounterEntry::Kind::UnsignedCount, &Counters::completionNoEnqueueFirstPublishSlotPrePresentDrawRunCommands, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_first_publish_slot_pre_present_draw_items", CounterEntry::Kind::UnsignedCount, &Counters::completionNoEnqueueFirstPublishSlotPrePresentDrawItems, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_first_publish_slot_pre_present_draw_items_max", CounterEntry::Kind::UnsignedCount, &Counters::completionNoEnqueueFirstPublishSlotPrePresentDrawItemsMax, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_first_publish_slot_pre_present_draw_items_p50", CounterEntry::Kind::PercentileNs, nullptr, nullptr, &Counters::completionNoEnqueueFirstPublishSlotPrePresentDrawItemsRing, 0.5},
    {"completion_no_enqueue_first_publish_slot_pre_present_draw_items_p95", CounterEntry::Kind::PercentileNs, nullptr, nullptr, &Counters::completionNoEnqueueFirstPublishSlotPrePresentDrawItemsRing, 0.95},
    {"completion_no_enqueue_first_publish_slot_pre_present_non_draw_commands", CounterEntry::Kind::UnsignedCount, &Counters::completionNoEnqueueFirstPublishSlotPrePresentNonDrawCommands, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_first_publish_slot_pre_present_payload_bytes", CounterEntry::Kind::UnsignedCount, &Counters::completionNoEnqueueFirstPublishSlotPrePresentPayloadBytes, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_first_publish_slot_pre_present_payload_bytes_max", CounterEntry::Kind::UnsignedCount, &Counters::completionNoEnqueueFirstPublishSlotPrePresentPayloadBytesMax, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_first_publish_slot_pre_present_payload_bytes_p50", CounterEntry::Kind::PercentileNs, nullptr, nullptr, &Counters::completionNoEnqueueFirstPublishSlotPrePresentPayloadBytesRing, 0.5},
    {"completion_no_enqueue_first_publish_slot_pre_present_payload_bytes_p95", CounterEntry::Kind::PercentileNs, nullptr, nullptr, &Counters::completionNoEnqueueFirstPublishSlotPrePresentPayloadBytesRing, 0.95},
    {"completion_no_enqueue_first_publish_slot_post_present_commands", CounterEntry::Kind::UnsignedCount, &Counters::completionNoEnqueueFirstPublishSlotPostPresentCommands, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_first_publish_slot_present_tail_slots", CounterEntry::Kind::UnsignedCount, &Counters::completionNoEnqueueFirstPublishSlotPresentTailSlots, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_first_publish_slot_present_nontail_slots", CounterEntry::Kind::UnsignedCount, &Counters::completionNoEnqueueFirstPublishSlotPresentNonTailSlots, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_wait_to_encode_dequeue", CounterEntry::Kind::UnsignedCount, &Counters::completionNoEnqueueWaitToEncodeDequeue, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_wait_to_encode_dequeue_ms", CounterEntry::Kind::Milliseconds, &Counters::completionNoEnqueueWaitToEncodeDequeueNs, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_wait_to_encode_dequeue_max_ms", CounterEntry::Kind::Milliseconds, &Counters::completionNoEnqueueWaitToEncodeDequeueMaxNs, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_wait_to_encode_dequeue_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionNoEnqueueWaitToEncodeDequeueRing, 0.5},
    {"completion_no_enqueue_wait_to_encode_dequeue_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionNoEnqueueWaitToEncodeDequeueRing, 0.95},
    {"completion_no_enqueue_wait_to_encode_dequeue_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionNoEnqueueWaitToEncodeDequeueRing, 0.99},
    {"completion_no_enqueue_wait_to_command_buffer_commit", CounterEntry::Kind::UnsignedCount, &Counters::completionNoEnqueueWaitToCommandBufferCommit, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_wait_to_command_buffer_commit_ms", CounterEntry::Kind::Milliseconds, &Counters::completionNoEnqueueWaitToCommandBufferCommitNs, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_wait_to_command_buffer_commit_max_ms", CounterEntry::Kind::Milliseconds, &Counters::completionNoEnqueueWaitToCommandBufferCommitMaxNs, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_wait_to_command_buffer_commit_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionNoEnqueueWaitToCommandBufferCommitRing, 0.5},
    {"completion_no_enqueue_wait_to_command_buffer_commit_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionNoEnqueueWaitToCommandBufferCommitRing, 0.95},
    {"completion_no_enqueue_wait_to_command_buffer_commit_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionNoEnqueueWaitToCommandBufferCommitRing, 0.99},
    {"encode_dequeue_ready_depth_samples", CounterEntry::Kind::UnsignedCount, &Counters::encodeDequeueReadyDepthSamples, nullptr, nullptr, 0.0},
    {"encode_dequeue_ready_depth_total", CounterEntry::Kind::UnsignedCount, &Counters::encodeDequeueReadyDepthTotal, nullptr, nullptr, 0.0},
    {"encode_dequeue_ready_depth_max", CounterEntry::Kind::UnsignedCount, &Counters::encodeDequeueReadyDepthMax, nullptr, nullptr, 0.0},
    {"encode_dequeue_ready_depth_gt1", CounterEntry::Kind::UnsignedCount, &Counters::encodeDequeueReadyDepthGt1, nullptr, nullptr, 0.0},
    {"encode_dequeue_ready_depth_gt2", CounterEntry::Kind::UnsignedCount, &Counters::encodeDequeueReadyDepthGt2, nullptr, nullptr, 0.0},
    {"encode_dequeue_ready_depth_gt4", CounterEntry::Kind::UnsignedCount, &Counters::encodeDequeueReadyDepthGt4, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_stage_commit_entry_to_publish", CounterEntry::Kind::UnsignedCount, &Counters::completionNoEnqueueStageCommitEntryToPublish, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_stage_commit_entry_to_publish_ms", CounterEntry::Kind::Milliseconds, &Counters::completionNoEnqueueStageCommitEntryToPublishNs, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_stage_commit_entry_to_publish_max_ms", CounterEntry::Kind::Milliseconds, &Counters::completionNoEnqueueStageCommitEntryToPublishMaxNs, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_stage_commit_entry_to_publish_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionNoEnqueueStageCommitEntryToPublishRing, 0.5},
    {"completion_no_enqueue_stage_commit_entry_to_publish_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionNoEnqueueStageCommitEntryToPublishRing, 0.95},
    {"completion_no_enqueue_stage_commit_entry_to_publish_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionNoEnqueueStageCommitEntryToPublishRing, 0.99},
    {"completion_no_enqueue_stage_publish_to_encode_dequeue", CounterEntry::Kind::UnsignedCount, &Counters::completionNoEnqueueStagePublishToEncodeDequeue, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_stage_publish_to_encode_dequeue_ms", CounterEntry::Kind::Milliseconds, &Counters::completionNoEnqueueStagePublishToEncodeDequeueNs, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_stage_publish_to_encode_dequeue_max_ms", CounterEntry::Kind::Milliseconds, &Counters::completionNoEnqueueStagePublishToEncodeDequeueMaxNs, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_stage_publish_to_encode_dequeue_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionNoEnqueueStagePublishToEncodeDequeueRing, 0.5},
    {"completion_no_enqueue_stage_publish_to_encode_dequeue_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionNoEnqueueStagePublishToEncodeDequeueRing, 0.95},
    {"completion_no_enqueue_stage_publish_to_encode_dequeue_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionNoEnqueueStagePublishToEncodeDequeueRing, 0.99},
    {"completion_no_enqueue_stage_encode_dequeue_to_command_buffer_commit", CounterEntry::Kind::UnsignedCount, &Counters::completionNoEnqueueStageEncodeDequeueToCommandBufferCommit, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_stage_encode_dequeue_to_command_buffer_commit_ms", CounterEntry::Kind::Milliseconds, &Counters::completionNoEnqueueStageEncodeDequeueToCommandBufferCommitNs, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_stage_encode_dequeue_to_command_buffer_commit_max_ms", CounterEntry::Kind::Milliseconds, &Counters::completionNoEnqueueStageEncodeDequeueToCommandBufferCommitMaxNs, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_stage_encode_dequeue_to_command_buffer_commit_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionNoEnqueueStageEncodeDequeueToCommandBufferCommitRing, 0.5},
    {"completion_no_enqueue_stage_encode_dequeue_to_command_buffer_commit_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionNoEnqueueStageEncodeDequeueToCommandBufferCommitRing, 0.95},
    {"completion_no_enqueue_stage_encode_dequeue_to_command_buffer_commit_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionNoEnqueueStageEncodeDequeueToCommandBufferCommitRing, 0.99},
    {"completion_no_enqueue_wait_to_next_enqueue", CounterEntry::Kind::UnsignedCount, &Counters::completionNoEnqueueWaitToNextEnqueue, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_wait_to_next_enqueue_ms", CounterEntry::Kind::Milliseconds, &Counters::completionNoEnqueueWaitToNextEnqueueNs, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_wait_to_next_enqueue_max_ms", CounterEntry::Kind::Milliseconds, &Counters::completionNoEnqueueWaitToNextEnqueueMaxNs, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_wait_to_next_enqueue_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionNoEnqueueWaitToNextEnqueueRing, 0.5},
    {"completion_no_enqueue_wait_to_next_enqueue_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionNoEnqueueWaitToNextEnqueueRing, 0.95},
    {"completion_no_enqueue_wait_to_next_enqueue_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionNoEnqueueWaitToNextEnqueueRing, 0.99},
    {"completion_no_enqueue_wait_to_next_present_enqueue", CounterEntry::Kind::UnsignedCount, &Counters::completionNoEnqueueWaitToNextPresentEnqueue, nullptr, nullptr, 0.0},
    {"completion_no_enqueue_wait_to_next_present_enqueue_ms", CounterEntry::Kind::Milliseconds, &Counters::completionNoEnqueueWaitToNextPresentEnqueueNs, nullptr, nullptr, 0.0},
    {"completion_dequeue_samples", CounterEntry::Kind::UnsignedCount, &Counters::completionDequeueSamples, nullptr, nullptr, 0.0},
    {"completion_dequeue_age_ms", CounterEntry::Kind::Milliseconds, &Counters::completionDequeueAgeNs, nullptr, nullptr, 0.0},
    {"completion_dequeue_age_max_ms", CounterEntry::Kind::Milliseconds, &Counters::completionDequeueAgeMaxNs, nullptr, nullptr, 0.0},
    {"completion_dequeue_age_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionDequeueAgeRing, 0.5},
    {"completion_dequeue_age_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionDequeueAgeRing, 0.95},
    {"completion_dequeue_age_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionDequeueAgeRing, 0.99},
    {"completion_pending_depth_max", CounterEntry::Kind::UnsignedCount, &Counters::completionPendingDepthMax, nullptr, nullptr, 0.0},
    {"completion_dequeue_status_not_enqueued", CounterEntry::Kind::UnsignedCount, &Counters::completionDequeueStatusNotEnqueued, nullptr, nullptr, 0.0},
    {"completion_dequeue_status_enqueued", CounterEntry::Kind::UnsignedCount, &Counters::completionDequeueStatusEnqueued, nullptr, nullptr, 0.0},
    {"completion_dequeue_status_committed", CounterEntry::Kind::UnsignedCount, &Counters::completionDequeueStatusCommitted, nullptr, nullptr, 0.0},
    {"completion_dequeue_status_scheduled", CounterEntry::Kind::UnsignedCount, &Counters::completionDequeueStatusScheduled, nullptr, nullptr, 0.0},
    {"completion_dequeue_status_completed", CounterEntry::Kind::UnsignedCount, &Counters::completionDequeueStatusCompleted, nullptr, nullptr, 0.0},
    {"completion_dequeue_status_error", CounterEntry::Kind::UnsignedCount, &Counters::completionDequeueStatusError, nullptr, nullptr, 0.0},
    {"completion_dequeue_status_unknown", CounterEntry::Kind::UnsignedCount, &Counters::completionDequeueStatusUnknown, nullptr, nullptr, 0.0},
    {"completion_wait_status_not_enqueued", CounterEntry::Kind::UnsignedCount, &Counters::completionWaitStatusNotEnqueued, nullptr, nullptr, 0.0},
    {"completion_wait_status_not_enqueued_ms", CounterEntry::Kind::Milliseconds, &Counters::completionWaitStatusNotEnqueuedNs, nullptr, nullptr, 0.0},
    {"completion_wait_status_enqueued", CounterEntry::Kind::UnsignedCount, &Counters::completionWaitStatusEnqueued, nullptr, nullptr, 0.0},
    {"completion_wait_status_enqueued_ms", CounterEntry::Kind::Milliseconds, &Counters::completionWaitStatusEnqueuedNs, nullptr, nullptr, 0.0},
    {"completion_wait_status_committed", CounterEntry::Kind::UnsignedCount, &Counters::completionWaitStatusCommitted, nullptr, nullptr, 0.0},
    {"completion_wait_status_committed_ms", CounterEntry::Kind::Milliseconds, &Counters::completionWaitStatusCommittedNs, nullptr, nullptr, 0.0},
    {"completion_wait_status_scheduled", CounterEntry::Kind::UnsignedCount, &Counters::completionWaitStatusScheduled, nullptr, nullptr, 0.0},
    {"completion_wait_status_scheduled_ms", CounterEntry::Kind::Milliseconds, &Counters::completionWaitStatusScheduledNs, nullptr, nullptr, 0.0},
    {"completion_wait_status_unknown", CounterEntry::Kind::UnsignedCount, &Counters::completionWaitStatusUnknown, nullptr, nullptr, 0.0},
    {"completion_wait_status_unknown_ms", CounterEntry::Kind::Milliseconds, &Counters::completionWaitStatusUnknownNs, nullptr, nullptr, 0.0},
    {"completion_waits", CounterEntry::Kind::UnsignedCount, &Counters::completionWaits, nullptr, nullptr, 0.0},
    {"completion_wait_ms", CounterEntry::Kind::Milliseconds, &Counters::completionWaitNs, nullptr, nullptr, 0.0},
    {"completion_wait_max_ms", CounterEntry::Kind::Milliseconds, &Counters::completionWaitMaxNs, nullptr, nullptr, 0.0},
    {"completion_wait_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionWaitRing, 0.5},
    {"completion_wait_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionWaitRing, 0.95},
    {"completion_wait_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionWaitRing, 0.99},
    {"completion_present_waits", CounterEntry::Kind::UnsignedCount, &Counters::completionPresentWaits, nullptr, nullptr, 0.0},
    {"completion_present_wait_ms", CounterEntry::Kind::Milliseconds, &Counters::completionPresentWaitNs, nullptr, nullptr, 0.0},
    {"completion_present_wait_max_ms", CounterEntry::Kind::Milliseconds, &Counters::completionPresentWaitMaxNs, nullptr, nullptr, 0.0},
    {"completion_present_wait_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionPresentWaitRing, 0.5},
    {"completion_present_wait_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionPresentWaitRing, 0.95},
    {"completion_present_wait_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionPresentWaitRing, 0.99},
    {"completion_draw_waits", CounterEntry::Kind::UnsignedCount, &Counters::completionDrawWaits, nullptr, nullptr, 0.0},
    {"completion_draw_wait_ms", CounterEntry::Kind::Milliseconds, &Counters::completionDrawWaitNs, nullptr, nullptr, 0.0},
    {"completion_draw_wait_max_ms", CounterEntry::Kind::Milliseconds, &Counters::completionDrawWaitMaxNs, nullptr, nullptr, 0.0},
    {"completion_draw_wait_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionDrawWaitRing, 0.5},
    {"completion_draw_wait_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionDrawWaitRing, 0.95},
    {"completion_draw_wait_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionDrawWaitRing, 0.99},
    {"completion_blit_waits", CounterEntry::Kind::UnsignedCount, &Counters::completionBlitWaits, nullptr, nullptr, 0.0},
    {"completion_blit_wait_ms", CounterEntry::Kind::Milliseconds, &Counters::completionBlitWaitNs, nullptr, nullptr, 0.0},
    {"completion_blit_wait_max_ms", CounterEntry::Kind::Milliseconds, &Counters::completionBlitWaitMaxNs, nullptr, nullptr, 0.0},
    {"completion_blit_wait_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionBlitWaitRing, 0.5},
    {"completion_blit_wait_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionBlitWaitRing, 0.95},
    {"completion_blit_wait_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionBlitWaitRing, 0.99},
    {"completion_present_only_waits", CounterEntry::Kind::UnsignedCount, &Counters::completionPresentOnlyWaits, nullptr, nullptr, 0.0},
    {"completion_present_only_wait_ms", CounterEntry::Kind::Milliseconds, &Counters::completionPresentOnlyWaitNs, nullptr, nullptr, 0.0},
    {"completion_present_only_wait_max_ms", CounterEntry::Kind::Milliseconds, &Counters::completionPresentOnlyWaitMaxNs, nullptr, nullptr, 0.0},
    {"completion_present_only_wait_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionPresentOnlyWaitRing, 0.5},
    {"completion_present_only_wait_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionPresentOnlyWaitRing, 0.95},
    {"completion_present_only_wait_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionPresentOnlyWaitRing, 0.99},
    {"completion_draw_present_waits", CounterEntry::Kind::UnsignedCount, &Counters::completionDrawPresentWaits, nullptr, nullptr, 0.0},
    {"completion_draw_present_wait_ms", CounterEntry::Kind::Milliseconds, &Counters::completionDrawPresentWaitNs, nullptr, nullptr, 0.0},
    {"completion_draw_present_wait_max_ms", CounterEntry::Kind::Milliseconds, &Counters::completionDrawPresentWaitMaxNs, nullptr, nullptr, 0.0},
    {"completion_draw_present_wait_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionDrawPresentWaitRing, 0.5},
    {"completion_draw_present_wait_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionDrawPresentWaitRing, 0.95},
    {"completion_draw_present_wait_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionDrawPresentWaitRing, 0.99},
    {"completion_draw_stretch_waits", CounterEntry::Kind::UnsignedCount, &Counters::completionDrawStretchWaits, nullptr, nullptr, 0.0},
    {"completion_draw_stretch_wait_ms", CounterEntry::Kind::Milliseconds, &Counters::completionDrawStretchWaitNs, nullptr, nullptr, 0.0},
    {"completion_draw_stretch_wait_max_ms", CounterEntry::Kind::Milliseconds, &Counters::completionDrawStretchWaitMaxNs, nullptr, nullptr, 0.0},
    {"completion_draw_stretch_wait_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionDrawStretchWaitRing, 0.5},
    {"completion_draw_stretch_wait_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionDrawStretchWaitRing, 0.95},
    {"completion_draw_stretch_wait_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionDrawStretchWaitRing, 0.99},
    {"completion_stretch_waits", CounterEntry::Kind::UnsignedCount, &Counters::completionStretchWaits, nullptr, nullptr, 0.0},
    {"completion_stretch_wait_ms", CounterEntry::Kind::Milliseconds, &Counters::completionStretchWaitNs, nullptr, nullptr, 0.0},
    {"completion_stretch_wait_max_ms", CounterEntry::Kind::Milliseconds, &Counters::completionStretchWaitMaxNs, nullptr, nullptr, 0.0},
    {"completion_stretch_wait_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionStretchWaitRing, 0.5},
    {"completion_stretch_wait_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionStretchWaitRing, 0.95},
    {"completion_stretch_wait_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionStretchWaitRing, 0.99},
    {"completion_blit_only_waits", CounterEntry::Kind::UnsignedCount, &Counters::completionBlitOnlyWaits, nullptr, nullptr, 0.0},
    {"completion_blit_only_wait_ms", CounterEntry::Kind::Milliseconds, &Counters::completionBlitOnlyWaitNs, nullptr, nullptr, 0.0},
    {"completion_blit_only_wait_max_ms", CounterEntry::Kind::Milliseconds, &Counters::completionBlitOnlyWaitMaxNs, nullptr, nullptr, 0.0},
    {"completion_blit_only_wait_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionBlitOnlyWaitRing, 0.5},
    {"completion_blit_only_wait_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionBlitOnlyWaitRing, 0.95},
    {"completion_blit_only_wait_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionBlitOnlyWaitRing, 0.99},
    {"completion_other_waits", CounterEntry::Kind::UnsignedCount, &Counters::completionOtherWaits, nullptr, nullptr, 0.0},
    {"completion_other_wait_ms", CounterEntry::Kind::Milliseconds, &Counters::completionOtherWaitNs, nullptr, nullptr, 0.0},
    {"completion_other_wait_max_ms", CounterEntry::Kind::Milliseconds, &Counters::completionOtherWaitMaxNs, nullptr, nullptr, 0.0},
    {"completion_other_wait_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionOtherWaitRing, 0.5},
    {"completion_other_wait_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionOtherWaitRing, 0.95},
    {"completion_other_wait_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::completionOtherWaitRing, 0.99},
    {"sync_waits", CounterEntry::Kind::UnsignedCount, &Counters::syncWaits, nullptr, nullptr, 0.0},
    {"sync_wait_ms", CounterEntry::Kind::Milliseconds, &Counters::syncWaitNs, nullptr, nullptr, 0.0},
    {"sync_wait_max_ms", CounterEntry::Kind::Milliseconds, &Counters::syncWaitMaxNs, nullptr, nullptr, 0.0},
    {"sync_wait_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::syncWaitRing, 0.5},
    {"sync_wait_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::syncWaitRing, 0.95},
    {"sync_wait_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::syncWaitRing, 0.99},
    {"queue_writer_waits", CounterEntry::Kind::UnsignedCount, &Counters::queueWriterWaits, nullptr, nullptr, 0.0},
    {"queue_writer_wait_ms", CounterEntry::Kind::Milliseconds, &Counters::queueWriterWaitNs, nullptr, nullptr, 0.0},
    {"queue_writer_wait_max_ms", CounterEntry::Kind::Milliseconds, &Counters::queueWriterWaitMaxNs, nullptr, nullptr, 0.0},
    {"queue_writer_wait_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::queueWriterWaitRing, 0.5},
    {"queue_writer_wait_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::queueWriterWaitRing, 0.95},
    {"queue_writer_wait_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::queueWriterWaitRing, 0.99},
    {"queue_commit_waits", CounterEntry::Kind::UnsignedCount, &Counters::queueCommitWaits, nullptr, nullptr, 0.0},
    {"queue_commit_wait_ms", CounterEntry::Kind::Milliseconds, &Counters::queueCommitWaitNs, nullptr, nullptr, 0.0},
    {"queue_commit_wait_max_ms", CounterEntry::Kind::Milliseconds, &Counters::queueCommitWaitMaxNs, nullptr, nullptr, 0.0},
    {"queue_commit_wait_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::queueCommitWaitRing, 0.5},
    {"queue_commit_wait_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::queueCommitWaitRing, 0.95},
    {"queue_commit_wait_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::queueCommitWaitRing, 0.99},
    {"queue_sequence_waits", CounterEntry::Kind::UnsignedCount, &Counters::queueSequenceWaits, nullptr, nullptr, 0.0},
    {"queue_sequence_wait_ms", CounterEntry::Kind::Milliseconds, &Counters::queueSequenceWaitNs, nullptr, nullptr, 0.0},
    {"queue_sequence_wait_max_ms", CounterEntry::Kind::Milliseconds, &Counters::queueSequenceWaitMaxNs, nullptr, nullptr, 0.0},
    {"queue_sequence_wait_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::queueSequenceWaitRing, 0.5},
    {"queue_sequence_wait_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::queueSequenceWaitRing, 0.95},
    {"queue_sequence_wait_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::queueSequenceWaitRing, 0.99},
    {"cpu_ready_tape_resident_sources", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyTapeResidentSources, nullptr, nullptr, 0.0},
    {"cpu_ready_tape_resident_sources_peak", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyTapeResidentSourcesPeak, nullptr, nullptr, 0.0},
    {"cpu_ready_tape_resident_pages", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyTapeResidentPages, nullptr, nullptr, 0.0},
    {"cpu_ready_tape_resident_pages_peak", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyTapeResidentPagesPeak, nullptr, nullptr, 0.0},
    {"cpu_ready_tape_ready_entries", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyTapeReadyEntries, nullptr, nullptr, 0.0},
    {"cpu_ready_tape_ready_entries_peak", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyTapeReadyEntriesPeak, nullptr, nullptr, 0.0},
    {"cpu_ready_tape_admission_closes", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyTapeAdmissionCloses, nullptr, nullptr, 0.0},
    {"cpu_ready_tape_admission_reopens", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyTapeAdmissionReopens, nullptr, nullptr, 0.0},
    {"cpu_ready_tape_wrap_padding_pages", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyTapeWrapPaddingPages, nullptr, nullptr, 0.0},
    {"cpu_ready_tape_admission_waits", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyTapeAdmissionWaits, nullptr, nullptr, 0.0},
    {"cpu_ready_tape_admission_wait_ms", CounterEntry::Kind::Milliseconds, &Counters::cpuReadyTapeAdmissionWaitNs, nullptr, nullptr, 0.0},
    {"cpu_ready_tape_admission_wait_max_ms", CounterEntry::Kind::Milliseconds, &Counters::cpuReadyTapeAdmissionWaitMaxNs, nullptr, nullptr, 0.0},
    {"cpu_ready_tape_admission_wait_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::cpuReadyTapeAdmissionWaitRing, 0.5},
    {"cpu_ready_tape_admission_wait_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::cpuReadyTapeAdmissionWaitRing, 0.95},
    {"cpu_ready_tape_admission_wait_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::cpuReadyTapeAdmissionWaitRing, 0.99},
    {"cpu_ready_tape_legacy_oversize_bypass", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyTapeLegacyOversizeBypass, nullptr, nullptr, 0.0},
    {"cpu_ready_tape_reclaim_wakeups", CounterEntry::Kind::UnsignedCount, &Counters::cpuReadyTapeReclaimWakeups, nullptr, nullptr, 0.0},
    {"map_buffer_calls", CounterEntry::Kind::UnsignedCount, &Counters::mapBufferCalls, nullptr, nullptr, 0.0},
    {"map_buffer_wait_seq", CounterEntry::Kind::UnsignedCount, &Counters::mapBufferWaitSeq, nullptr, nullptr, 0.0},
    {"map_buffer_no_wait_seq", CounterEntry::Kind::UnsignedCount, &Counters::mapBufferNoWaitSeq, nullptr, nullptr, 0.0},
    {"map_buffer_total_ms", CounterEntry::Kind::Milliseconds, &Counters::mapBufferTotalNs, nullptr, nullptr, 0.0},
    {"map_buffer_total_max_ms", CounterEntry::Kind::Milliseconds, &Counters::mapBufferTotalMaxNs, nullptr, nullptr, 0.0},
    {"map_buffer_total_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::mapBufferTotalRing, 0.5},
    {"map_buffer_total_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::mapBufferTotalRing, 0.95},
    {"map_buffer_total_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::mapBufferTotalRing, 0.99},
    {"map_buffer_mutex_wait_ms", CounterEntry::Kind::Milliseconds, &Counters::mapBufferMutexWaitNs, nullptr, nullptr, 0.0},
    {"map_buffer_mutex_wait_max_ms", CounterEntry::Kind::Milliseconds, &Counters::mapBufferMutexWaitMaxNs, nullptr, nullptr, 0.0},
    {"map_buffer_mutex_wait_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::mapBufferMutexWaitRing, 0.5},
    {"map_buffer_mutex_wait_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::mapBufferMutexWaitRing, 0.95},
    {"map_buffer_mutex_wait_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::mapBufferMutexWaitRing, 0.99},
    {"map_buffer_wait_ms", CounterEntry::Kind::Milliseconds, &Counters::mapBufferWaitNs, nullptr, nullptr, 0.0},
    {"map_buffer_wait_max_ms", CounterEntry::Kind::Milliseconds, &Counters::mapBufferWaitMaxNs, nullptr, nullptr, 0.0},
    {"map_buffer_wait_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::mapBufferWaitRing, 0.5},
    {"map_buffer_wait_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::mapBufferWaitRing, 0.95},
    {"map_buffer_wait_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::mapBufferWaitRing, 0.99},
    {"map_buffer_discard", CounterEntry::Kind::UnsignedCount, &Counters::mapBufferDiscard, nullptr, nullptr, 0.0},
    {"map_buffer_nooverwrite", CounterEntry::Kind::UnsignedCount, &Counters::mapBufferNoOverwrite, nullptr, nullptr, 0.0},
    {"map_buffer_readonly", CounterEntry::Kind::UnsignedCount, &Counters::mapBufferReadOnly, nullptr, nullptr, 0.0},
    {"map_buffer_plain", CounterEntry::Kind::UnsignedCount, &Counters::mapBufferPlain, nullptr, nullptr, 0.0},
    {"managed_buffer_uploads", CounterEntry::Kind::UnsignedCount, &Counters::managedBufferUploads, nullptr, nullptr, 0.0},
    {"managed_buffer_upload_bytes", CounterEntry::Kind::UnsignedCount, &Counters::managedBufferUploadBytes, nullptr, nullptr, 0.0},
    {"managed_buffer_backing_in_place", CounterEntry::Kind::UnsignedCount, &Counters::managedBufferBackingInPlace, nullptr, nullptr, 0.0},
    {"managed_buffer_backing_reuse", CounterEntry::Kind::UnsignedCount, &Counters::managedBufferBackingReuse, nullptr, nullptr, 0.0},
    {"managed_buffer_backing_fresh", CounterEntry::Kind::UnsignedCount, &Counters::managedBufferBackingFresh, nullptr, nullptr, 0.0},
    {"present_boundary_applied", CounterEntry::Kind::UnsignedCount, &Counters::presentBoundaryApplied, nullptr, nullptr, 0.0},
    {"present_boundary_skipped", CounterEntry::Kind::UnsignedCount, &Counters::presentBoundarySkipped, nullptr, nullptr, 0.0},
    {"present_boundary_deferred", CounterEntry::Kind::UnsignedCount, &Counters::presentBoundaryDeferred, nullptr, nullptr, 0.0},
    {"present_boundary_deferred_waits", CounterEntry::Kind::UnsignedCount, &Counters::presentBoundaryDeferredWaits, nullptr, nullptr, 0.0},
    {"present_boundary_waits", CounterEntry::Kind::UnsignedCount, &Counters::presentBoundaryWaits, nullptr, nullptr, 0.0},
    {"present_boundary_wait_ms", CounterEntry::Kind::Milliseconds, &Counters::presentBoundaryWaitNs, nullptr, nullptr, 0.0},
    {"present_boundary_wait_max_ms", CounterEntry::Kind::Milliseconds, &Counters::presentBoundaryWaitMaxNs, nullptr, nullptr, 0.0},
    {"present_boundary_wait_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::presentBoundaryWaitRing, 0.5},
    {"present_boundary_wait_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::presentBoundaryWaitRing, 0.95},
    {"present_boundary_wait_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::presentBoundaryWaitRing, 0.99},
    {"present_ordinal_boundary_waits", CounterEntry::Kind::UnsignedCount, &Counters::presentOrdinalBoundaryWaits, nullptr, nullptr, 0.0},
    {"present_ordinal_boundary_wait_ms", CounterEntry::Kind::Milliseconds, &Counters::presentOrdinalBoundaryWaitNs, nullptr, nullptr, 0.0},
    {"completed_present_ordinal", CounterEntry::Kind::UnsignedCount, &Counters::completedPresentOrdinal, nullptr, nullptr, 0.0},
    {"present_encoded", CounterEntry::Kind::UnsignedCount, &Counters::presentEncoded, nullptr, nullptr, 0.0},
    {"present_skipped", CounterEntry::Kind::UnsignedCount, &Counters::presentSkipped, nullptr, nullptr, 0.0},
    {"present_full", CounterEntry::Kind::UnsignedCount, &Counters::presentFullscreen, nullptr, nullptr, 0.0},
    {"present_source_selections", CounterEntry::Kind::UnsignedCount, &Counters::presentSourceSelections, nullptr, nullptr, 0.0},
    {"present_source_explicit", CounterEntry::Kind::UnsignedCount, &Counters::presentSourceExplicit, nullptr, nullptr, 0.0},
    {"present_source_current_backbuffer", CounterEntry::Kind::UnsignedCount, &Counters::presentSourceCurrentBackBuffer, nullptr, nullptr, 0.0},
    {"present_source_checks", CounterEntry::Kind::UnsignedCount, &Counters::presentSourceChecks, nullptr, nullptr, 0.0},
    {"present_source_valid", CounterEntry::Kind::UnsignedCount, &Counters::presentSourceValid, nullptr, nullptr, 0.0},
    {"present_source_missing_surface", CounterEntry::Kind::UnsignedCount, &Counters::presentSourceMissingSurface, nullptr, nullptr, 0.0},
    {"present_source_missing_texture", CounterEntry::Kind::UnsignedCount, &Counters::presentSourceMissingTexture, nullptr, nullptr, 0.0},
    {"present_source_resolve", CounterEntry::Kind::UnsignedCount, &Counters::presentSourceResolve, nullptr, nullptr, 0.0},
    {"present_source_invalid_size", CounterEntry::Kind::UnsignedCount, &Counters::presentSourceInvalidSize, nullptr, nullptr, 0.0},
    {"present_source_handle", CounterEntry::Kind::Hex64, &Counters::presentSourceHandle, nullptr, nullptr, 0.0},
    {"present_source_texture", CounterEntry::Kind::Hex64, &Counters::presentSourceTextureHandle, nullptr, nullptr, 0.0},
    {"present_source_size", CounterEntry::Kind::WidthByHeight, &Counters::presentSourceWidth, &Counters::presentSourceHeight, nullptr, 0.0},
    {"present_source_fmt", CounterEntry::Kind::UnsignedCount, &Counters::presentSourceFormat, nullptr, nullptr, 0.0},
    {"present_source_samples", CounterEntry::Kind::UnsignedCount, &Counters::presentSourceSampleCount, nullptr, nullptr, 0.0},
    {"present_pass", CounterEntry::Kind::UnsignedCount, &Counters::presentPass, nullptr, nullptr, 0.0},
    {"present_src", CounterEntry::Kind::WidthByHeight, &Counters::presentPassSrcWidth, &Counters::presentPassSrcHeight, nullptr, 0.0},
    {"present_dst", CounterEntry::Kind::WidthByHeight, &Counters::presentPassDstWidth, &Counters::presentPassDstHeight, nullptr, 0.0},
    {"present_dst_max", CounterEntry::Kind::WidthByHeight, &Counters::presentPassDstMaxWidth, &Counters::presentPassDstMaxHeight, nullptr, 0.0},
    {"present_schedule_requested_sync", CounterEntry::Kind::UnsignedCount, &Counters::presentScheduleRequestedSync, nullptr, nullptr, 0.0},
    {"present_schedule_requested_immediate", CounterEntry::Kind::UnsignedCount, &Counters::presentScheduleRequestedImmediate, nullptr, nullptr, 0.0},
    {"present_schedule_after_minimum_duration", CounterEntry::Kind::UnsignedCount, &Counters::presentScheduleAfterMinimumDuration, nullptr, nullptr, 0.0},
    {"present_schedule_immediate", CounterEntry::Kind::UnsignedCount, &Counters::presentScheduleImmediate, nullptr, nullptr, 0.0},
    {"present_minimum_duration_ms", CounterEntry::Kind::Milliseconds, &Counters::presentMinimumDurationNs, nullptr, nullptr, 0.0},
    {"present_minimum_duration_max_ms", CounterEntry::Kind::Milliseconds, &Counters::presentMinimumDurationMaxNs, nullptr, nullptr, 0.0},
    {"present_acquire_waits", CounterEntry::Kind::UnsignedCount, &Counters::presentAcquireWaits, nullptr, nullptr, 0.0},
    {"present_acquire_wait_ms", CounterEntry::Kind::Milliseconds, &Counters::presentAcquireWaitNs, nullptr, nullptr, 0.0},
    {"present_acquire_wait_max_ms", CounterEntry::Kind::Milliseconds, &Counters::presentAcquireWaitMaxNs, nullptr, nullptr, 0.0},
    {"present_acquire_wait_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::presentAcquireWaitRing, 0.5},
    {"present_acquire_wait_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::presentAcquireWaitRing, 0.95},
    {"present_acquire_wait_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::presentAcquireWaitRing, 0.99},
    {"present_acquire_slow_waits", CounterEntry::Kind::UnsignedCount, &Counters::presentAcquireSlowWaits, nullptr, nullptr, 0.0},
    {"present_async_acquire_requests", CounterEntry::Kind::UnsignedCount, &Counters::presentAsyncAcquireRequests, nullptr, nullptr, 0.0},
    {"present_async_acquire_issued", CounterEntry::Kind::UnsignedCount, &Counters::presentAsyncAcquireIssued, nullptr, nullptr, 0.0},
    {"present_async_acquire_fallbacks", CounterEntry::Kind::UnsignedCount, &Counters::presentAsyncAcquireFallbacks, nullptr, nullptr, 0.0},
    {"present_async_acquire_waits", CounterEntry::Kind::UnsignedCount, &Counters::presentAsyncAcquireWaits, nullptr, nullptr, 0.0},
    {"present_async_acquire_wait_ms", CounterEntry::Kind::Milliseconds, &Counters::presentAsyncAcquireWaitNs, nullptr, nullptr, 0.0},
    {"present_async_acquire_wait_max_ms", CounterEntry::Kind::Milliseconds, &Counters::presentAsyncAcquireWaitMaxNs, nullptr, nullptr, 0.0},
    {"present_async_acquire_wait_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::presentAsyncAcquireWaitRing, 0.5},
    {"present_async_acquire_wait_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::presentAsyncAcquireWaitRing, 0.95},
    {"present_async_acquire_wait_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::presentAsyncAcquireWaitRing, 0.99},
    {"present_async_acquire_slow_waits", CounterEntry::Kind::UnsignedCount, &Counters::presentAsyncAcquireSlowWaits, nullptr, nullptr, 0.0},
    {"present_token_waits", CounterEntry::Kind::UnsignedCount, &Counters::presentTokenWaits, nullptr, nullptr, 0.0},
    {"present_token_wait_ms", CounterEntry::Kind::Milliseconds, &Counters::presentTokenWaitNs, nullptr, nullptr, 0.0},
    {"present_token_wait_max_ms", CounterEntry::Kind::Milliseconds, &Counters::presentTokenWaitMaxNs, nullptr, nullptr, 0.0},
    {"present_token_wait_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::presentTokenWaitRing, 0.5},
    {"present_token_wait_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::presentTokenWaitRing, 0.95},
    {"present_token_wait_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::presentTokenWaitRing, 0.99},
    {"present_token_slow_waits", CounterEntry::Kind::UnsignedCount, &Counters::presentTokenSlowWaits, nullptr, nullptr, 0.0},
    {"present_preacquire_requests", CounterEntry::Kind::UnsignedCount, &Counters::presentPreAcquireRequests, nullptr, nullptr, 0.0},
    {"present_preacquire_hits", CounterEntry::Kind::UnsignedCount, &Counters::presentPreAcquireHits, nullptr, nullptr, 0.0},
    {"present_preacquire_misses", CounterEntry::Kind::UnsignedCount, &Counters::presentPreAcquireMisses, nullptr, nullptr, 0.0},
    {"present_preacquire_wait_ms", CounterEntry::Kind::Milliseconds, &Counters::presentPreAcquireWaitNs, nullptr, nullptr, 0.0},
    {"present_preacquire_wait_max_ms", CounterEntry::Kind::Milliseconds, &Counters::presentPreAcquireWaitMaxNs, nullptr, nullptr, 0.0},
    {"present_preacquire_wait_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::presentPreAcquireWaitRing, 0.5},
    {"present_preacquire_wait_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::presentPreAcquireWaitRing, 0.95},
    {"present_preacquire_wait_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::presentPreAcquireWaitRing, 0.99},
    {"present_set_props_waits", CounterEntry::Kind::UnsignedCount, &Counters::presentSetPropsWaits, nullptr, nullptr, 0.0},
    {"present_set_props_wait_ms", CounterEntry::Kind::Milliseconds, &Counters::presentSetPropsWaitNs, nullptr, nullptr, 0.0},
    {"present_set_props_wait_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::presentSetPropsWaitRing, 0.5},
    {"present_set_props_wait_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::presentSetPropsWaitRing, 0.95},
    {"present_set_props_wait_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::presentSetPropsWaitRing, 0.99},
    // R-BACK-39.2 (Task B11, L1) — frame-graph observe-path counters.
    {"framegraph_passes_built", CounterEntry::Kind::UnsignedCount, &Counters::framegraphPassesBuilt, nullptr, nullptr, 0.0},
    {"framegraph_passes_coalesced", CounterEntry::Kind::UnsignedCount, &Counters::framegraphPassesCoalesced, nullptr, nullptr, 0.0},
    {"framegraph_passes_dead", CounterEntry::Kind::UnsignedCount, &Counters::framegraphPassesDead, nullptr, nullptr, 0.0},
    {"framegraph_resources_memoryless", CounterEntry::Kind::UnsignedCount, &Counters::framegraphResourcesMemoryless, nullptr, nullptr, 0.0},
    {"framegraph_dag_dumps_written", CounterEntry::Kind::UnsignedCount, &Counters::framegraphDagDumpsWritten, nullptr, nullptr, 0.0},
    {"framegraph_dce_dropped", CounterEntry::Kind::UnsignedCount, &Counters::framegraphDceDropped, nullptr, nullptr, 0.0},
    {"framegraph_dce_preserved_unprovable", CounterEntry::Kind::UnsignedCount, &Counters::framegraphDcePreservedUnprovable, nullptr, nullptr, 0.0},
    {"framegraph_dce_cross_chunk_proof_resources", CounterEntry::Kind::UnsignedCount, &Counters::framegraphDceCrossChunkProofResources, nullptr, nullptr, 0.0},
    {"framegraph_dce_replay_commands_omitted", CounterEntry::Kind::UnsignedCount, &Counters::framegraphDceReplayCommandsOmitted, nullptr, nullptr, 0.0},
    {"framegraph_dce_lookahead_prefixes", CounterEntry::Kind::UnsignedCount, &Counters::framegraphDceLookaheadPrefixes, nullptr, nullptr, 0.0},
    {"framegraph_dce_lookahead_prefix_commands", CounterEntry::Kind::UnsignedCount, &Counters::framegraphDceLookaheadPrefixCommands, nullptr, nullptr, 0.0},
    {"framegraph_dce_lookahead_selected", CounterEntry::Kind::UnsignedCount, &Counters::framegraphDceLookaheadSelected, nullptr, nullptr, 0.0},
    {"framegraph_dce_lookahead_fail_open", CounterEntry::Kind::UnsignedCount, &Counters::framegraphDceLookaheadFailOpen, nullptr, nullptr, 0.0},
    {"framegraph_source_local_return_candidates", CounterEntry::Kind::UnsignedCount, &Counters::framegraphSourceLocalReturnCandidates, nullptr, nullptr, 0.0},
    {"framegraph_source_local_return_merged", CounterEntry::Kind::UnsignedCount, &Counters::framegraphSourceLocalReturnMerged, nullptr, nullptr, 0.0},
    {"framegraph_source_local_return_blocked_cycle", CounterEntry::Kind::UnsignedCount, &Counters::framegraphSourceLocalReturnBlockedCycle, nullptr, nullptr, 0.0},
    {"framegraph_source_local_return_second_non_draw", CounterEntry::Kind::UnsignedCount, &Counters::framegraphSourceLocalReturnSecondNonDraw, nullptr, nullptr, 0.0},
    {"framegraph_source_local_return_non_render_intervener", CounterEntry::Kind::UnsignedCount, &Counters::framegraphSourceLocalReturnNonRenderIntervener, nullptr, nullptr, 0.0},
    {"framegraph_source_local_return_missing_invariant", CounterEntry::Kind::UnsignedCount, &Counters::framegraphSourceLocalReturnMissingInvariant, nullptr, nullptr, 0.0},
    {"framegraph_source_local_return_dependency_kept", CounterEntry::Kind::UnsignedCount, &Counters::framegraphSourceLocalReturnDependencyKept, nullptr, nullptr, 0.0},
    {"framegraph_source_local_return_move_before", CounterEntry::Kind::UnsignedCount, &Counters::framegraphSourceLocalReturnMoveBefore, nullptr, nullptr, 0.0},
    {"framegraph_source_local_return_move_after", CounterEntry::Kind::UnsignedCount, &Counters::framegraphSourceLocalReturnMoveAfter, nullptr, nullptr, 0.0},
    {"framegraph_source_local_return_non_draw_intervener", CounterEntry::Kind::UnsignedCount, &Counters::framegraphSourceLocalReturnNonDrawIntervener, nullptr, nullptr, 0.0},
    {"framegraph_source_local_return_semantic_intervener", CounterEntry::Kind::UnsignedCount, &Counters::framegraphSourceLocalReturnSemanticIntervener, nullptr, nullptr, 0.0},
    {"framegraph_source_local_return_commandless_intervener", CounterEntry::Kind::UnsignedCount, &Counters::framegraphSourceLocalReturnCommandlessIntervener, nullptr, nullptr, 0.0},
    {"framegraph_source_local_return_commandless", CounterEntry::Kind::UnsignedCount, &Counters::framegraphSourceLocalReturnCommandless, nullptr, nullptr, 0.0},
    {"framegraph_source_local_return_legacy_candidates", CounterEntry::Kind::UnsignedCount, &Counters::framegraphSourceLocalReturnLegacyCandidates, nullptr, nullptr, 0.0},
    {"framegraph_source_local_return_arena_candidates", CounterEntry::Kind::UnsignedCount, &Counters::framegraphSourceLocalReturnArenaCandidates, nullptr, nullptr, 0.0},
    {"framegraph_source_local_return_unknown_candidates", CounterEntry::Kind::UnsignedCount, &Counters::framegraphSourceLocalReturnUnknownCandidates, nullptr, nullptr, 0.0},
    {"framegraph_source_local_return_identity_known_candidates", CounterEntry::Kind::UnsignedCount, &Counters::framegraphSourceLocalReturnIdentityKnownCandidates, nullptr, nullptr, 0.0},
    {"framegraph_source_local_return_identity_missing_candidates", CounterEntry::Kind::UnsignedCount, &Counters::framegraphSourceLocalReturnIdentityMissingCandidates, nullptr, nullptr, 0.0},
    {"framegraph_source_local_return_frontier_rollback_sources", CounterEntry::Kind::UnsignedCount, &Counters::framegraphSourceLocalReturnFrontierRollbackSources, nullptr, nullptr, 0.0},
    {"framegraph_source_local_return_frontier_rollback_candidates", CounterEntry::Kind::UnsignedCount, &Counters::framegraphSourceLocalReturnFrontierRollbackCandidates, nullptr, nullptr, 0.0},
    {"framegraph_source_local_return_frontier_rollback_merged", CounterEntry::Kind::UnsignedCount, &Counters::framegraphSourceLocalReturnFrontierRollbackMerged, nullptr, nullptr, 0.0},
    {"framegraph_source_local_return_frontier_rollback_invalid_plan_sources", CounterEntry::Kind::UnsignedCount, &Counters::framegraphSourceLocalReturnFrontierRollbackInvalidPlanSources, nullptr, nullptr, 0.0},
    {"framegraph_source_local_return_frontier_rollback_invalid_plan_candidates", CounterEntry::Kind::UnsignedCount, &Counters::framegraphSourceLocalReturnFrontierRollbackInvalidPlanCandidates, nullptr, nullptr, 0.0},
    {"framegraph_source_local_return_frontier_rollback_invalid_plan_merged", CounterEntry::Kind::UnsignedCount, &Counters::framegraphSourceLocalReturnFrontierRollbackInvalidPlanMerged, nullptr, nullptr, 0.0},
    {"framegraph_source_local_return_frontier_rollback_live_set_mismatch_sources", CounterEntry::Kind::UnsignedCount, &Counters::framegraphSourceLocalReturnFrontierRollbackLiveSetMismatchSources, nullptr, nullptr, 0.0},
    {"framegraph_source_local_return_frontier_rollback_live_set_mismatch_candidates", CounterEntry::Kind::UnsignedCount, &Counters::framegraphSourceLocalReturnFrontierRollbackLiveSetMismatchCandidates, nullptr, nullptr, 0.0},
    {"framegraph_source_local_return_frontier_rollback_live_set_mismatch_merged", CounterEntry::Kind::UnsignedCount, &Counters::framegraphSourceLocalReturnFrontierRollbackLiveSetMismatchMerged, nullptr, nullptr, 0.0},
    {"framegraph_source_local_return_frontier_rollback_duplicate_command_sources", CounterEntry::Kind::UnsignedCount, &Counters::framegraphSourceLocalReturnFrontierRollbackDuplicateCommandSources, nullptr, nullptr, 0.0},
    {"framegraph_source_local_return_frontier_rollback_duplicate_command_candidates", CounterEntry::Kind::UnsignedCount, &Counters::framegraphSourceLocalReturnFrontierRollbackDuplicateCommandCandidates, nullptr, nullptr, 0.0},
    {"framegraph_source_local_return_frontier_rollback_duplicate_command_merged", CounterEntry::Kind::UnsignedCount, &Counters::framegraphSourceLocalReturnFrontierRollbackDuplicateCommandMerged, nullptr, nullptr, 0.0},
    {"framegraph_source_local_return_frontier_rollback_moved_head_unproved_sources", CounterEntry::Kind::UnsignedCount, &Counters::framegraphSourceLocalReturnFrontierRollbackMovedHeadUnprovedSources, nullptr, nullptr, 0.0},
    {"framegraph_source_local_return_frontier_rollback_moved_head_unproved_candidates", CounterEntry::Kind::UnsignedCount, &Counters::framegraphSourceLocalReturnFrontierRollbackMovedHeadUnprovedCandidates, nullptr, nullptr, 0.0},
    {"framegraph_source_local_return_frontier_rollback_moved_head_unproved_merged", CounterEntry::Kind::UnsignedCount, &Counters::framegraphSourceLocalReturnFrontierRollbackMovedHeadUnprovedMerged, nullptr, nullptr, 0.0},
    {"framegraph_source_local_return_final_invalid_sources", CounterEntry::Kind::UnsignedCount, &Counters::framegraphSourceLocalReturnFinalInvalidSources, nullptr, nullptr, 0.0},
    {"framegraph_source_local_return_final_invalid_candidates", CounterEntry::Kind::UnsignedCount, &Counters::framegraphSourceLocalReturnFinalInvalidCandidates, nullptr, nullptr, 0.0},
    {"framegraph_source_local_return_final_invalid_merged", CounterEntry::Kind::UnsignedCount, &Counters::framegraphSourceLocalReturnFinalInvalidMerged, nullptr, nullptr, 0.0},
    {"framegraph_source_local_return_final_natural_order_sources", CounterEntry::Kind::UnsignedCount, &Counters::framegraphSourceLocalReturnFinalNaturalOrderSources, nullptr, nullptr, 0.0},
    {"framegraph_source_local_return_final_natural_order_candidates", CounterEntry::Kind::UnsignedCount, &Counters::framegraphSourceLocalReturnFinalNaturalOrderCandidates, nullptr, nullptr, 0.0},
    {"framegraph_source_local_return_final_natural_order_merged", CounterEntry::Kind::UnsignedCount, &Counters::framegraphSourceLocalReturnFinalNaturalOrderMerged, nullptr, nullptr, 0.0},
    {"framegraph_source_local_return_final_reordered_activated_sources", CounterEntry::Kind::UnsignedCount, &Counters::framegraphSourceLocalReturnFinalReorderedActivatedSources, nullptr, nullptr, 0.0},
    {"framegraph_source_local_return_final_reordered_activated_candidates", CounterEntry::Kind::UnsignedCount, &Counters::framegraphSourceLocalReturnFinalReorderedActivatedCandidates, nullptr, nullptr, 0.0},
    {"framegraph_source_local_return_final_reordered_activated_merged", CounterEntry::Kind::UnsignedCount, &Counters::framegraphSourceLocalReturnFinalReorderedActivatedMerged, nullptr, nullptr, 0.0},
    {"framegraph_active_render_snapshot_absent", CounterEntry::Kind::UnsignedCount, &Counters::framegraphActiveRenderSnapshotAbsent, nullptr, nullptr, 0.0},
    {"framegraph_active_render_snapshot_incomplete", CounterEntry::Kind::UnsignedCount, &Counters::framegraphActiveRenderSnapshotIncomplete, nullptr, nullptr, 0.0},
    {"framegraph_active_render_seed_apply_applied", CounterEntry::Kind::UnsignedCount, &Counters::framegraphActiveRenderSeedApplyApplied, nullptr, nullptr, 0.0},
    {"framegraph_active_render_seed_apply_invalid", CounterEntry::Kind::UnsignedCount, &Counters::framegraphActiveRenderSeedApplyInvalid, nullptr, nullptr, 0.0},
    {"framegraph_active_render_seed_apply_incomplete", CounterEntry::Kind::UnsignedCount, &Counters::framegraphActiveRenderSeedApplyIncomplete, nullptr, nullptr, 0.0},
    {"framegraph_active_render_seed_apply_overflow", CounterEntry::Kind::UnsignedCount, &Counters::framegraphActiveRenderSeedApplyOverflow, nullptr, nullptr, 0.0},
    {"framegraph_active_render_seed_applied_but_unmerged", CounterEntry::Kind::UnsignedCount, &Counters::framegraphActiveRenderSeedAppliedButUnmerged, nullptr, nullptr, 0.0},
    {"framegraph_active_render_seed_pass_coalesce_blocked_cycle", CounterEntry::Kind::UnsignedCount, &Counters::framegraphActiveRenderSeedPassCoalesceBlockedCycle, nullptr, nullptr, 0.0},
    {"framegraph_active_render_seed_pass_coalesce_second_non_draw", CounterEntry::Kind::UnsignedCount, &Counters::framegraphActiveRenderSeedPassCoalesceSecondNonDraw, nullptr, nullptr, 0.0},
    {"framegraph_active_render_seed_moved_head_proved", CounterEntry::Kind::UnsignedCount, &Counters::framegraphActiveRenderSeedMovedHeadProved, nullptr, nullptr, 0.0},
    {"framegraph_active_render_seed_fallback_moved_head_unproved", CounterEntry::Kind::UnsignedCount, &Counters::framegraphActiveRenderSeedFallbackMovedHeadUnproved, nullptr, nullptr, 0.0},
    {"framegraph_active_render_seed_fallback_invalid_plan", CounterEntry::Kind::UnsignedCount, &Counters::framegraphActiveRenderSeedFallbackInvalidPlan, nullptr, nullptr, 0.0},
    {"framegraph_active_render_seed_fallback_live_set_mismatch", CounterEntry::Kind::UnsignedCount, &Counters::framegraphActiveRenderSeedFallbackLiveSetMismatch, nullptr, nullptr, 0.0},
    {"framegraph_active_render_seed_fallback_duplicate_command", CounterEntry::Kind::UnsignedCount, &Counters::framegraphActiveRenderSeedFallbackDuplicateCommand, nullptr, nullptr, 0.0},
    {"framegraph_active_render_seed_replay_activated", CounterEntry::Kind::UnsignedCount, &Counters::framegraphActiveRenderSeedReplayActivated, nullptr, nullptr, 0.0},
};

void report() {
  if (!enabledFlag()) {
    return;
  }
  const Counters& c = counters();
  std::string line;
  line.reserve(65536);
  line += "[dxmt9-perf]";
  for (const auto& e : kCounterTable) {
    char field[128]{};
    switch (e.kind) {
      case CounterEntry::Kind::UnsignedCount:
        std::snprintf(field, sizeof(field), " %s=%llu", e.key,
                      static_cast<unsigned long long>(load(c.*e.atomicField)));
        break;
      case CounterEntry::Kind::Milliseconds:
        std::snprintf(field, sizeof(field), " %s=%.3f", e.key,
                      static_cast<double>(load(c.*e.atomicField)) / 1000000.0);
        break;
      case CounterEntry::Kind::Hex64:
        std::snprintf(field, sizeof(field), " %s=0x%llx", e.key,
                      static_cast<unsigned long long>(load(c.*e.atomicField)));
        break;
      case CounterEntry::Kind::WidthByHeight:
        std::snprintf(field, sizeof(field), " %s=%llux%llu", e.key,
                      static_cast<unsigned long long>(load(c.*e.atomicField)),
                      static_cast<unsigned long long>(load(c.*e.field2)));
        break;
      case CounterEntry::Kind::PercentileMs:
        std::snprintf(field, sizeof(field), " %s=%.3f", e.key,
                      static_cast<double>(
                          (c.*e.ringField).percentile(e.percentile)) /
                          1000000.0);
        break;
      case CounterEntry::Kind::PercentileNs:
        std::snprintf(field, sizeof(field), " %s=%llu", e.key,
                      static_cast<unsigned long long>(
                          (c.*e.ringField).percentile(e.percentile)));
        break;
    }
    line += field;
  }
  line += '\n';
  std::fwrite(line.data(), 1, line.size(), stderr);
  std::fflush(stderr);
}

void ensureRegistered() {
  static const bool registered = [] {
    if (enabledFlag()) {
      std::atexit(report);
    }
    return true;
  }();
  (void)registered;
}

void add(std::atomic<std::uint64_t>& counter, std::uint64_t value = 1) {
  if (!enabled()) {
    return;
  }
  counter.fetch_add(value, std::memory_order_relaxed);
}

// Call only after one outer enabled() decision. Diagnostic batch recorders use
// this to avoid repeating the runtime gate and issuing zero-valued atomics.
void addEnabledNonZero(std::atomic<std::uint64_t>& counter,
                       std::uint64_t value) {
  if (value != 0) {
    counter.fetch_add(value, std::memory_order_relaxed);
  }
}

void addBatchMissUniformBuild(std::atomic<std::uint64_t>& counter,
                              std::uint64_t value = 1) {
  if (!inD3D9SnapshotUniformBuildBatchMissContext()) {
    return;
  }
  add(counter, value);
}

void updateMax(std::atomic<std::uint64_t>& counter, std::uint64_t value) {
  if (!enabled()) {
    return;
  }
  auto current = counter.load(std::memory_order_relaxed);
  while (current < value &&
         !counter.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
  }
}

void updateMin(std::atomic<std::uint64_t>& counter, std::uint64_t value) {
  if (!enabled()) {
    return;
  }
  auto current = counter.load(std::memory_order_relaxed);
  while ((current == 0 || current > value) &&
         !counter.compare_exchange_weak(current, value,
                                        std::memory_order_relaxed)) {
  }
}

void recordCpuTime(std::atomic<std::uint64_t>& total,
                   std::atomic<std::uint64_t>& max,
                   std::uint64_t nanoseconds) {
  add(total, nanoseconds);
  updateMax(max, nanoseconds);
}

void store(std::atomic<std::uint64_t>& counter, std::uint64_t value) {
  if (!enabled()) {
    return;
  }
  counter.store(value, std::memory_order_relaxed);
}

void recordRing(PercentileRing& ring, std::uint64_t nanoseconds) {
  if (!enabled()) {
    return;
  }
  ring.record(nanoseconds);
}

}  // namespace

bool enabled() {
  ensureRegistered();
  return enabledFlag();
}

ScopedD3D9SnapshotUniformBuildContext::ScopedD3D9SnapshotUniformBuildContext(
    D3D9SnapshotUniformBuildContext context) noexcept
    : previous_(gD3D9SnapshotUniformBuildContext) {
  gD3D9SnapshotUniformBuildContext = context;
}

ScopedD3D9SnapshotUniformBuildContext::~ScopedD3D9SnapshotUniformBuildContext() {
  gD3D9SnapshotUniformBuildContext = previous_;
}

void countSubmitDraw() {
  add(counters().submitDraw);
}

void countChunkAdmit() {
  add(counters().chunkAdmit);
}

void countChunkReject() {
  add(counters().chunkReject);
}

void countCommandChunkWire(std::uint32_t version, std::uint64_t records,
                           std::uint64_t bytes,
                           std::uint64_t registryResolutions) {
  if (!enabled()) return;
  if (version == 2u) {
    add(counters().commandChunkV2Chunks);
    add(counters().commandChunkV2Records, records);
    add(counters().commandChunkV2Bytes, bytes);
    add(counters().commandChunkV2RegistryResolutions, registryResolutions);
  }
}

void countCommandChunkV2Reject() {
  add(counters().commandChunkV2Rejects);
}

void countRingArenaHeapFallback(RingArenaKind kind, std::uint64_t bytes) {
  if (!enabled()) return;
  add(counters().ringArenaHeapFallbackCount);
  add(counters().ringArenaHeapFallbackBytes, bytes);
  switch (kind) {
    case RingArenaKind::Argbuf:
      add(counters().ringArenaHeapFallbackCountArgbuf);
      break;
    case RingArenaKind::LambdaStore:
      add(counters().ringArenaHeapFallbackCountLambda);
      break;
    case RingArenaKind::Staging:
      add(counters().ringArenaHeapFallbackCountStaging);
      break;
    case RingArenaKind::CopyTemp:
      add(counters().ringArenaHeapFallbackCountCopyTemp);
      break;
    case RingArenaKind::Unknown:
      break;
  }
}

void countSubmitClear() {
  add(counters().submitClear);
}

void countSubmitStretch() {
  add(counters().submitStretch);
}

void countStretchBlitCopy() {
  add(counters().stretchBlitCopy);
}

void countStretchRenderPass() {
  add(counters().stretchRenderPass);
}

void countStretchFullscreen() {
  add(counters().stretchFullscreen);
}

void countSubmitPresent() {
  add(counters().submitPresent);
}

void countSubmitFlush() {
  add(counters().submitFlush);
}

void countSubmitPresentCpuTime(std::uint64_t nanoseconds) {
  auto& c = counters();
  add(c.submitPresentCpuNs, nanoseconds);
  updateMax(c.submitPresentCpuMaxNs, nanoseconds);
  recordRing(c.submitPresentCpuRing, nanoseconds);
}

void countSubmitPresentAcquireCpuTime(std::uint64_t nanoseconds) {
  auto& c = counters();
  add(c.submitPresentAcquireCpuNs, nanoseconds);
  updateMax(c.submitPresentAcquireCpuMaxNs, nanoseconds);
  recordRing(c.submitPresentAcquireCpuRing, nanoseconds);
}

void countSubmitPresentCommitCpuTime(std::uint64_t nanoseconds) {
  auto& c = counters();
  add(c.submitPresentCommitCpuNs, nanoseconds);
  updateMax(c.submitPresentCommitCpuMaxNs, nanoseconds);
  recordRing(c.submitPresentCommitCpuRing, nanoseconds);
}

void countSubmitPresentBoundaryCpuTime(std::uint64_t nanoseconds) {
  auto& c = counters();
  add(c.submitPresentBoundaryCpuNs, nanoseconds);
  updateMax(c.submitPresentBoundaryCpuMaxNs, nanoseconds);
  recordRing(c.submitPresentBoundaryCpuRing, nanoseconds);
}

void countPrepareSlotForPublishCpuTime(std::uint64_t nanoseconds) {
  auto& c = counters();
  add(c.prepareSlotForPublishCpuNs, nanoseconds);
  updateMax(c.prepareSlotForPublishCpuMaxNs, nanoseconds);
  recordRing(c.prepareSlotForPublishCpuRing, nanoseconds);
}

void countPrepareSlotResourceMarkCpuTime(std::uint64_t nanoseconds) {
  auto& c = counters();
  add(c.prepareSlotResourceMarkCpuNs, nanoseconds);
  updateMax(c.prepareSlotResourceMarkCpuMaxNs, nanoseconds);
  recordRing(c.prepareSlotResourceMarkCpuRing, nanoseconds);
}

void countUnpublishedSlotPsoPrefetchCpuTime(std::uint64_t nanoseconds) {
  auto& c = counters();
  add(c.unpublishedSlotPsoPrefetchCpuNs, nanoseconds);
  updateMax(c.unpublishedSlotPsoPrefetchCpuMaxNs, nanoseconds);
  recordRing(c.unpublishedSlotPsoPrefetchCpuRing, nanoseconds);
}

void countChunkPublishReason(ChunkPublishReason reason,
                             std::uint64_t commandCount) {
  auto& c = counters();
  std::atomic<std::uint64_t>* count = &c.chunkPublishReasonUnknown;
  std::atomic<std::uint64_t>* commands = &c.chunkPublishCommandsUnknown;
  switch (reason) {
  case ChunkPublishReason::DrawLimit:
    count = &c.chunkPublishReasonDrawLimit;
    commands = &c.chunkPublishCommandsDrawLimit;
    break;
  case ChunkPublishReason::PayloadLimit:
    count = &c.chunkPublishReasonPayloadLimit;
    commands = &c.chunkPublishCommandsPayloadLimit;
    break;
  case ChunkPublishReason::Present:
    count = &c.chunkPublishReasonPresent;
    commands = &c.chunkPublishCommandsPresent;
    break;
  case ChunkPublishReason::PresentAcquire:
    count = &c.chunkPublishReasonPresentAcquire;
    commands = &c.chunkPublishCommandsPresentAcquire;
    break;
  case ChunkPublishReason::Flush:
    count = &c.chunkPublishReasonFlush;
    commands = &c.chunkPublishCommandsFlush;
    break;
  case ChunkPublishReason::StretchSplit:
    count = &c.chunkPublishReasonStretchSplit;
    commands = &c.chunkPublishCommandsStretchSplit;
    break;
  case ChunkPublishReason::MapWait:
    count = &c.chunkPublishReasonMapWait;
    commands = &c.chunkPublishCommandsMapWait;
    break;
  case ChunkPublishReason::PresentSplitBefore:
    count = &c.chunkPublishReasonPresentSplitBefore;
    commands = &c.chunkPublishCommandsPresentSplitBefore;
    break;
  case ChunkPublishReason::SemanticBoundary:
    count = &c.chunkPublishReasonSemanticBoundary;
    commands = &c.chunkPublishCommandsSemanticBoundary;
    break;
  // DrawContinuation: the H229 open-CB carrier (DXMT9_OPEN_CB_CARRIER)
  // deliberately does not publish same-key draw-continuation boundaries —
  // removing them was H183's decisive difference from H182. Keep the enum
  // value (table numbering stability) folded into the Unknown bucket.
  case ChunkPublishReason::DrawContinuation:
  case ChunkPublishReason::Unknown:
    break;
  }
  add(*count);
  add(*commands, commandCount);
}

void countChunkPublishPresentPrePresentOpportunityTail(
    ChunkPublishTailCommandKind kind, bool drawOnly) {
  auto& c = counters();
  switch (kind) {
  case ChunkPublishTailCommandKind::Empty:
    add(c.chunkPublishPresentPrePresentOpportunityTailEmpty);
    break;
  case ChunkPublishTailCommandKind::DrawRun:
    add(c.chunkPublishPresentPrePresentOpportunityTailDrawRun);
    break;
  case ChunkPublishTailCommandKind::Clear:
    add(c.chunkPublishPresentPrePresentOpportunityTailClear);
    break;
  case ChunkPublishTailCommandKind::SurfaceCopy:
    add(c.chunkPublishPresentPrePresentOpportunityTailSurfaceCopy);
    break;
  case ChunkPublishTailCommandKind::StretchRect:
    add(c.chunkPublishPresentPrePresentOpportunityTailStretchRect);
    break;
  case ChunkPublishTailCommandKind::Readback:
    add(c.chunkPublishPresentPrePresentOpportunityTailReadback);
    break;
  case ChunkPublishTailCommandKind::ColorFill:
    add(c.chunkPublishPresentPrePresentOpportunityTailColorFill);
    break;
  case ChunkPublishTailCommandKind::DepthResolve:
    add(c.chunkPublishPresentPrePresentOpportunityTailDepthResolve);
    break;
  case ChunkPublishTailCommandKind::Present:
    add(c.chunkPublishPresentPrePresentOpportunityTailPresent);
    break;
  }
  if (drawOnly) {
    add(c.chunkPublishPresentPrePresentOpportunityDrawOnly);
  }
}

void countOpenCbCarrierWaitStartPublished() {
  add(counters().openCbCarrierWaitStartPublished);
}

void countOpenCbCarrierActiveWaitPublished() {
  add(counters().openCbCarrierActiveWaitPublished);
}

void countOpenCbCarrierProducerWaitPublished() {
  add(counters().openCbCarrierProducerWaitPublished);
}

void countOpenCbCarrierAttachmentBoundaryPublished() {
  add(counters().openCbCarrierAttachmentBoundaryPublished);
}

void countOpenCbCarrierPendingStarted(bool duringCompletionWait) {
  auto& c = counters();
  add(c.openCbCarrierPendingStarted);
  if (duringCompletionWait) {
    add(c.openCbCarrierPendingStartedInWait);
  }
}

void countOpenCbCarrierHeadAppended() {
  add(counters().openCbCarrierHeadAppended);
}

void countOpenCbCarrierTailSubmitted() {
  add(counters().openCbCarrierTailSubmitted);
}

void countOpenCbCarrierReleased(OpenCbCarrierReleaseReason reason) {
  auto& c = counters();
  switch (reason) {
  case OpenCbCarrierReleaseReason::SemanticWait:
    add(c.openCbCarrierReleasedSemanticWait);
    break;
  case OpenCbCarrierReleaseReason::ProducerWait:
    add(c.openCbCarrierReleasedProducerWait);
    break;
  case OpenCbCarrierReleaseReason::NonAppendable:
    add(c.openCbCarrierReleasedNonAppendable);
    break;
  case OpenCbCarrierReleaseReason::InitializerWait:
    add(c.openCbCarrierReleasedInitializerWait);
    break;
  case OpenCbCarrierReleaseReason::Drain:
    add(c.openCbCarrierReleasedDrain);
    break;
  case OpenCbCarrierReleaseReason::FailPath:
    add(c.openCbCarrierReleasedFailPath);
    break;
  }
}

void countCpuReadySessionPendingStarted() {
  add(counters().cpuReadySessionPendingStarted);
}

void countCpuReadySessionHeadAppended(bool arenaSource) {
  auto& c = counters();
  add(c.cpuReadySessionHeadAppended);
  if (arenaSource) {
    add(c.cpuReadySessionArenaHeadAppended);
  }
}

void countCpuReadySessionTailSubmitted() {
  add(counters().cpuReadySessionTailSubmitted);
}

void countCpuReadyRetainedHeadAttempt() {
  add(counters().cpuReadyRetainedHeadAttempts);
}

void countCpuReadyRetainedHeadHeld() {
  auto& c = counters();
  add(c.cpuReadyRetainedHeadHeld);
  std::uint64_t live = 0;
  if (enabled()) {
    live = c.cpuReadyRetainedHeadLive.fetch_add(
               1, std::memory_order_relaxed) +
        1;
  }
  updateMax(c.cpuReadyRetainedHeadPeak, live);
}

void countCpuReadyRetainedHeadSuccessorReady() {
  add(counters().cpuReadyRetainedHeadSuccessorReady);
}

void countCpuReadyRetainedHeadFallback(
    CpuReadyRetainedHeadFallbackReason reason) {
  auto& c = counters();
  switch (reason) {
  case CpuReadyRetainedHeadFallbackReason::Release:
    add(c.cpuReadyRetainedHeadFallbackRelease);
    break;
  case CpuReadyRetainedHeadFallbackReason::ProducerWait:
    add(c.cpuReadyRetainedHeadFallbackProducerWait);
    break;
  case CpuReadyRetainedHeadFallbackReason::Initializer:
    add(c.cpuReadyRetainedHeadFallbackInitializer);
    break;
  case CpuReadyRetainedHeadFallbackReason::Stop:
    add(c.cpuReadyRetainedHeadFallbackStop);
    break;
  case CpuReadyRetainedHeadFallbackReason::WriterGone:
    add(c.cpuReadyRetainedHeadFallbackWriterGone);
    break;
  case CpuReadyRetainedHeadFallbackReason::Pressure:
    add(c.cpuReadyRetainedHeadFallbackPressure);
    break;
  }
}

void countCpuReadyRetainedHeadRestoreFailure() {
  add(counters().cpuReadyRetainedHeadRestoreFailure);
}

void recordCpuReadyRetainedHeadWait(std::uint64_t nanoseconds) {
  auto& c = counters();
  add(c.cpuReadyRetainedHeadWaitNs, nanoseconds);
  if (!enabled()) {
    return;
  }
  auto live = c.cpuReadyRetainedHeadLive.load(std::memory_order_relaxed);
  while (live != 0 &&
         !c.cpuReadyRetainedHeadLive.compare_exchange_weak(
             live, live - 1, std::memory_order_relaxed)) {
  }
}

void countCompletionSpanShadowBuilt(std::uint64_t sourceCount) {
  auto& c = counters();
  add(c.completionSpanShadowBuilt);
  add(c.completionSpanShadowSourceCount, sourceCount);
}

void countCompletionSpanShadowValidated() {
  add(counters().completionSpanShadowValidated);
}

void countCompletionSpanShadowMismatch() {
  add(counters().completionSpanShadowMismatch);
}

void countPostEncodeRetireAttempt() {
  add(counters().postEncodeRetireAttempts);
}

void countPostEncodeRetireSuccess(bool arena) {
  auto& c = counters();
  add(c.postEncodeRetireSuccess);
  add(arena ? c.postEncodeRetireSuccessArena
            : c.postEncodeRetireSuccessLegacy);
}

void countPostEncodeRetireIneligible(std::uint32_t reason) {
  auto& c = counters();
  switch (reason) {
  case 0: add(c.postEncodeRetireIneligibleNone); break;
  case 1: add(c.postEncodeRetireIneligiblePendingClear); break;
  case 2: add(c.postEncodeRetireIneligiblePresent); break;
  case 3: add(c.postEncodeRetireIneligibleReadback); break;
  case 4: add(c.postEncodeRetireIneligibleUpdateSurface); break;
  case 5: add(c.postEncodeRetireIneligibleOrderedControl); break;
  case 6: add(c.postEncodeRetireIneligiblePayloadBorrow); break;
  case 7: add(c.postEncodeRetireIneligibleNotOldest); break;
  case 8: add(c.postEncodeRetireIneligibleReceiptCapacity); break;
  default: add(c.postEncodeRetireIneligibleInvalid); break;
  }
}

void countPostEncodeReceiptFailure(std::uint32_t result) {
  auto& c = counters();
  switch (result) {
  case 1: add(c.postEncodeReceiptFailureInvalid); break;
  case 2: add(c.postEncodeReceiptFailureDuplicate); break;
  case 3: add(c.postEncodeReceiptFailureCapacity); break;
  case 4: add(c.postEncodeReceiptFailureStale); break;
  case 5: add(c.postEncodeReceiptFailureWrongState); break;
  default: add(c.postEncodeReceiptFailureOther); break;
  }
}

void recordPostEncodeReceiptDepth(std::uint64_t depth,
                                  std::uint64_t peak) {
  auto& c = counters();
  store(c.postEncodeReceiptDepth, depth);
  updateMax(c.postEncodeReceiptPeak, peak);
}

void countPostEncodeResidencyCreditReleased(std::uint64_t pages,
                                            std::uint64_t bytes) {
  auto& c = counters();
  add(c.postEncodeResidencySourcesReleased);
  add(c.postEncodeResidencyPagesReleased, pages);
  add(c.postEncodeResidencyBytesReleased, bytes);
}

void countPostEncodeWorkCapClose() {
  add(counters().postEncodeWorkCapCloses);
}

void recordGpuOutstandingCompletionSources(std::uint64_t count) {
  auto& c = counters();
  store(c.gpuOutstandingCompletionSources, count);
  updateMax(c.gpuOutstandingCompletionSourcesPeak, count);
}

void countCpuReadySessionReleased(CpuReadySessionReleaseReason reason) {
  auto& c = counters();
  switch (reason) {
  case CpuReadySessionReleaseReason::ProducerWait:
    add(c.cpuReadySessionReleasedProducerWait);
    break;
  case CpuReadySessionReleaseReason::NonAppendable:
    add(c.cpuReadySessionReleasedNonAppendable);
    break;
  case CpuReadySessionReleaseReason::InitializerWait:
    add(c.cpuReadySessionReleasedInitializerWait);
    break;
  case CpuReadySessionReleaseReason::Drain:
    add(c.cpuReadySessionReleasedDrain);
    break;
  case CpuReadySessionReleaseReason::FailPath:
    add(c.cpuReadySessionReleasedFailPath);
    break;
  }
}

void countCpuReadySessionLeaseAcquired(
    std::uint64_t reservedSources,
    std::uint64_t reservedPages,
    std::uint64_t reservedBytes,
    std::uint64_t reservedDraws,
    std::uint64_t successorHeadroomPages) {
  auto& c = counters();
  add(c.cpuReadySessionLeaseAcquisitions);
  std::uint64_t current = 0;
  if (enabled()) {
    current = c.cpuReadySessionLeaseCurrent.fetch_add(
                  1, std::memory_order_relaxed) +
              1;
  }
  updateMax(c.cpuReadySessionLeasePeak, current);
  store(c.cpuReadySessionLeaseReservedSources, reservedSources);
  store(c.cpuReadySessionLeaseReservedPages, reservedPages);
  store(c.cpuReadySessionLeaseReservedBytes, reservedBytes);
  store(c.cpuReadySessionLeaseReservedDraws, reservedDraws);
  updateMin(c.cpuReadySessionSuccessorHeadroomMinPages,
            successorHeadroomPages);
}

void countCpuReadySessionLeaseDenied() {
  add(counters().cpuReadySessionLeaseDenials);
}

void recordCpuReadySessionLeaseUsed(std::uint64_t usedSources,
                                    std::uint64_t usedPages,
                                    std::uint64_t usedBytes,
                                    std::uint64_t usedDraws,
                                    std::uint64_t slackSources,
                                    std::uint64_t slackPages,
                                    std::uint64_t slackBytes,
                                    std::uint64_t slackDraws) {
  auto& c = counters();
  store(c.cpuReadySessionLeaseUsedSources, usedSources);
  store(c.cpuReadySessionLeaseUsedPages, usedPages);
  store(c.cpuReadySessionLeaseUsedBytes, usedBytes);
  store(c.cpuReadySessionLeaseUsedDraws, usedDraws);
  store(c.cpuReadySessionLeaseSlackSources, slackSources);
  store(c.cpuReadySessionLeaseSlackPages, slackPages);
  store(c.cpuReadySessionLeaseSlackBytes, slackBytes);
  store(c.cpuReadySessionLeaseSlackDraws, slackDraws);
}

void countCpuReadySessionLeaseReleased() {
  auto& c = counters();
  if (enabled()) {
    auto current = c.cpuReadySessionLeaseCurrent.load(
        std::memory_order_relaxed);
    while (current != 0 &&
           !c.cpuReadySessionLeaseCurrent.compare_exchange_weak(
               current, current - 1, std::memory_order_relaxed)) {
    }
  }
  store(c.cpuReadySessionLeaseReservedSources, 0);
  store(c.cpuReadySessionLeaseReservedPages, 0);
  store(c.cpuReadySessionLeaseReservedBytes, 0);
  store(c.cpuReadySessionLeaseReservedDraws, 0);
  store(c.cpuReadySessionLeaseUsedSources, 0);
  store(c.cpuReadySessionLeaseUsedPages, 0);
  store(c.cpuReadySessionLeaseUsedBytes, 0);
  store(c.cpuReadySessionLeaseUsedDraws, 0);
  store(c.cpuReadySessionLeaseSlackSources, 0);
  store(c.cpuReadySessionLeaseSlackPages, 0);
  store(c.cpuReadySessionLeaseSlackBytes, 0);
  store(c.cpuReadySessionLeaseSlackDraws, 0);
}

void countCpuReadySessionCapRelease(CpuReadySessionCapDimension dimension) {
  auto& c = counters();
  switch (dimension) {
  case CpuReadySessionCapDimension::Sources:
    add(c.cpuReadySessionCapSources);
    break;
  case CpuReadySessionCapDimension::Pages:
    add(c.cpuReadySessionCapPages);
    break;
  case CpuReadySessionCapDimension::Bytes:
    add(c.cpuReadySessionCapBytes);
    break;
  case CpuReadySessionCapDimension::Draws:
    add(c.cpuReadySessionCapDraws);
    break;
  case CpuReadySessionCapDimension::CommandBuffers:
    add(c.cpuReadySessionCapCommandBuffers);
    break;
  }
}

void recordCpuReadySessionCapRequirement(
    CpuReadySessionCapRequirementAxes axes,
    std::uint64_t predecessorSources,
    std::uint64_t predecessorPages,
    std::uint64_t candidatePayloadPages,
    std::uint64_t candidateWrapPaddingPages,
    std::uint64_t candidateRequiredPages,
    std::uint64_t requiredTotalSources,
    std::uint64_t requiredTotalPages) {
  if (!enabled()) {
    return;
  }
  auto& c = counters();
  switch (axes) {
  case CpuReadySessionCapRequirementAxes::SourcesOnly:
    addEnabledNonZero(c.cpuReadySessionCapRequirementSourcesOnly, 1);
    break;
  case CpuReadySessionCapRequirementAxes::PagesOnly:
    addEnabledNonZero(c.cpuReadySessionCapRequirementPagesOnly, 1);
    break;
  case CpuReadySessionCapRequirementAxes::SourcesAndPages:
    addEnabledNonZero(c.cpuReadySessionCapRequirementSourcesAndPages, 1);
    break;
  }
  const auto updatePeakEnabled = [](std::atomic<std::uint64_t>& counter,
                                    std::uint64_t value) {
    auto current = counter.load(std::memory_order_relaxed);
    while (current < value &&
           !counter.compare_exchange_weak(current, value,
                                          std::memory_order_relaxed)) {
    }
  };
  updatePeakEnabled(c.cpuReadySessionCapPredecessorSourcesPeak,
                    predecessorSources);
  updatePeakEnabled(c.cpuReadySessionCapPredecessorPagesPeak,
                    predecessorPages);
  updatePeakEnabled(c.cpuReadySessionCapCandidatePayloadPagesPeak,
                    candidatePayloadPages);
  updatePeakEnabled(c.cpuReadySessionCapCandidateWrapPaddingPagesPeak,
                    candidateWrapPaddingPages);
  updatePeakEnabled(c.cpuReadySessionCapCandidateRequiredPagesPeak,
                    candidateRequiredPages);
  updatePeakEnabled(c.cpuReadySessionCapRequiredTotalSourcesPeak,
                    requiredTotalSources);
  updatePeakEnabled(c.cpuReadySessionCapRequiredTotalPagesPeak,
                    requiredTotalPages);
}

void countCpuReadySessionDisposition(
    CpuReadySessionDisposition disposition) {
  auto& c = counters();
  switch (disposition) {
  case CpuReadySessionDisposition::Isolated:
    add(c.cpuReadySessionIsolated);
    break;
  case CpuReadySessionDisposition::LegacyRollback:
    add(c.cpuReadySessionLegacyRollback);
    break;
  case CpuReadySessionDisposition::Invalid:
    add(c.cpuReadySessionInvalidDisposition);
    break;
  }
}

void countCpuReadySessionIsolation(
    CpuReadySessionIsolationReason reason) {
  auto& c = counters();
  switch (reason) {
  case CpuReadySessionIsolationReason::Present:
    add(c.cpuReadySessionIsolatedPresent);
    break;
  case CpuReadySessionIsolationReason::CapacityBytes:
    add(c.cpuReadySessionIsolatedCapacityBytes);
    break;
  case CpuReadySessionIsolationReason::Other:
    add(c.cpuReadySessionIsolatedOther);
    break;
  }
}

void countCpuReadyMultiSourceWindowAttempted() {
  add(counters().cpuReadyMultiSourceWindowsAttempted);
}

void countCpuReadyMultiSourceWindowPlanned(std::uint64_t sources,
                                           std::uint64_t commands,
                                           std::uint64_t runs) {
  auto& c = counters();
  add(c.cpuReadyMultiSourceWindowsPlanned);
  add(c.cpuReadyMultiSourceWindowSources, sources);
  add(c.cpuReadyMultiSourceWindowCommands, commands);
  add(c.cpuReadyMultiSourceWindowRuns, runs);
}

void countCpuReadyMultiSourceWindowFallback(
    CpuReadyMultiSourceFallbackReason reason) {
  auto& c = counters();
  switch (reason) {
  case CpuReadyMultiSourceFallbackReason::Eligibility:
    add(c.cpuReadyMultiSourceFallbackEligibility);
    break;
  case CpuReadyMultiSourceFallbackReason::NaturalPlan:
    add(c.cpuReadyMultiSourceFallbackNaturalPlan);
    break;
  case CpuReadyMultiSourceFallbackReason::InvalidPlan:
    add(c.cpuReadyMultiSourceFallbackInvalidPlan);
    break;
  case CpuReadyMultiSourceFallbackReason::RepeatedSource:
    add(c.cpuReadyMultiSourceFallbackRepeatedSource);
    break;
  case CpuReadyMultiSourceFallbackReason::ResolvedSource:
    add(c.cpuReadyMultiSourceFallbackResolvedSource);
    break;
  case CpuReadyMultiSourceFallbackReason::CompletionSource:
    add(c.cpuReadyMultiSourceFallbackCompletionSource);
    break;
  case CpuReadyMultiSourceFallbackReason::Admission:
    add(c.cpuReadyMultiSourceFallbackAdmission);
    break;
  case CpuReadyMultiSourceFallbackReason::FragmentRange:
    add(c.cpuReadyMultiSourceFallbackFragmentRange);
    break;
  case CpuReadyMultiSourceFallbackReason::Carrier:
    add(c.cpuReadyMultiSourceFallbackCarrier);
    break;
  }
}

void countCpuReadyMultiSourceSourceLocalFallbackStarted(
    CpuReadyMultiSourceSourceLocalFallback disposition,
    std::uint64_t sources) {
  auto& c = counters();
  switch (disposition) {
  case CpuReadyMultiSourceSourceLocalFallback::NaturalAfterMerge:
    add(c.cpuReadyMultiSourceNaturalFallbackWindowsStarted);
    add(c.cpuReadyMultiSourceNaturalFallbackSources, sources);
    break;
  case CpuReadyMultiSourceSourceLocalFallback::PermutationRejected:
    add(c.cpuReadyMultiSourcePermutationFallbackWindowsStarted);
    add(c.cpuReadyMultiSourcePermutationFallbackSources, sources);
    break;
  }
}

void countCpuReadyMultiSourceSourceLocalFallbackCompleted(
    CpuReadyMultiSourceSourceLocalFallback disposition) {
  auto& c = counters();
  switch (disposition) {
  case CpuReadyMultiSourceSourceLocalFallback::NaturalAfterMerge:
    add(c.cpuReadyMultiSourceNaturalFallbackWindowsCompleted);
    break;
  case CpuReadyMultiSourceSourceLocalFallback::PermutationRejected:
    add(c.cpuReadyMultiSourcePermutationFallbackWindowsCompleted);
    break;
  }
}

void countCpuReadyMultiSourcePostEffectFatal(
    CpuReadyMultiSourceFatalReason reason) {
  auto& c = counters();
  switch (reason) {
  case CpuReadyMultiSourceFatalReason::EncodeReturnedNull:
    add(c.cpuReadyMultiSourceFatalEncodeNull);
    break;
  case CpuReadyMultiSourceFatalReason::CarrierFold:
    add(c.cpuReadyMultiSourceFatalCarrierFold);
    break;
  }
}

void countCpuReadyMultiSourceEligibilityFallback(
    CpuReadyMultiSourceEligibilityReason reason) {
  auto& c = counters();
  switch (reason) {
  case CpuReadyMultiSourceEligibilityReason::ActiveRenderIncomplete:
    add(c.cpuReadyMultiSourceEligibilityActiveIncomplete);
    break;
  case CpuReadyMultiSourceEligibilityReason::PresentBoundary:
    add(c.cpuReadyMultiSourceEligibilityPresent);
    break;
  case CpuReadyMultiSourceEligibilityReason::NonConsecutiveIdentity:
    add(c.cpuReadyMultiSourceEligibilityNonConsecutiveIdentity);
    break;
  case CpuReadyMultiSourceEligibilityReason::OtherBoundary:
    add(c.cpuReadyMultiSourceEligibilityOtherBoundary);
    break;
  }
}

void recordCpuReadyMultiSourcePlannerOutcome(
    CpuReadyMultiSourcePlannerOutcome outcome,
    CpuReadyMultiSourcePlannerMerge merge,
    std::uint32_t firstMatchingPassDistance,
    CpuReadyMultiSourceSeedMergeAttribution seedAttribution,
    bool seedSecondNonDraw,
    bool seedBlockedCycle) {
  auto& c = counters();
  switch (outcome) {
  case CpuReadyMultiSourcePlannerOutcome::InvalidInput:
    add(c.cpuReadyMultiSourcePlannerInvalidInput);
    break;
  case CpuReadyMultiSourcePlannerOutcome::SeedRejected:
    add(c.cpuReadyMultiSourcePlannerSeedRejected);
    break;
  case CpuReadyMultiSourcePlannerOutcome::NoActiveTargetMatch:
    add(c.cpuReadyMultiSourcePlannerNoActiveTargetMatch);
    break;
  case CpuReadyMultiSourcePlannerOutcome::NoMerge:
    add(c.cpuReadyMultiSourcePlannerNoMerge);
    break;
  case CpuReadyMultiSourcePlannerOutcome::NaturalAfterMerge:
    add(c.cpuReadyMultiSourcePlannerNaturalAfterMerge);
    break;
  case CpuReadyMultiSourcePlannerOutcome::PermutationRejected:
    add(c.cpuReadyMultiSourcePlannerPermutationRejected);
    break;
  case CpuReadyMultiSourcePlannerOutcome::MovedHeadUnproved:
    add(c.cpuReadyMultiSourcePlannerMovedHeadUnproved);
    break;
  case CpuReadyMultiSourcePlannerOutcome::Planned:
    add(c.cpuReadyMultiSourcePlannerPlanned);
    break;
  }
  switch (merge) {
  case CpuReadyMultiSourcePlannerMerge::None:
    break;
  case CpuReadyMultiSourcePlannerMerge::Seed:
    add(c.cpuReadyMultiSourcePlannerMergeSeed);
    break;
  case CpuReadyMultiSourcePlannerMerge::NonSeedOnly:
    add(c.cpuReadyMultiSourcePlannerMergeNonSeedOnly);
    break;
  }
  if (outcome == CpuReadyMultiSourcePlannerOutcome::NaturalAfterMerge &&
      merge == CpuReadyMultiSourcePlannerMerge::Seed) {
    if (firstMatchingPassDistance == 0u) {
      add(c.cpuReadyMultiSourcePlannerSeedNaturalMatchDistanceMissing);
    } else if (firstMatchingPassDistance == 1u) {
      add(c.cpuReadyMultiSourcePlannerSeedNaturalMatchDistance1);
    } else {
      add(c.cpuReadyMultiSourcePlannerSeedNaturalMatchDistanceGt1);
    }

    add(c.cpuReadyMultiSourcePlannerSeedNaturalMergeOperations,
        seedAttribution.seedMergeCount);
    add(c.cpuReadyMultiSourcePlannerSeedNaturalMergeDistanceTotal,
        seedAttribution.seedMergeDistanceTotal);
    updateMax(c.cpuReadyMultiSourcePlannerSeedNaturalMergeDistanceMax,
              seedAttribution.seedMergeDistanceMax);
    add(c.cpuReadyMultiSourcePlannerSeedNaturalCommandBefore,
        seedAttribution.commandBefore);
    add(c.cpuReadyMultiSourcePlannerSeedNaturalCommandAfter,
        seedAttribution.commandAfter);
    add(c.cpuReadyMultiSourcePlannerSeedNaturalEmptyIntervening,
        seedAttribution.emptyIntervening);

    const std::uint64_t classifiedIntervening =
        seedAttribution.commandBefore + seedAttribution.commandAfter +
        seedAttribution.emptyIntervening;
    const bool valid = !seedAttribution.missing &&
        seedAttribution.optimizerMergeCount >= seedAttribution.seedMergeCount &&
        seedAttribution.seedMergeCount != 0u &&
        seedAttribution.seedMergeDistanceMax != 0u &&
        seedAttribution.seedMergeDistanceTotal >=
            seedAttribution.seedMergeCount &&
        seedAttribution.seedMergeDistanceMax <=
            seedAttribution.seedMergeDistanceTotal &&
        classifiedIntervening ==
            seedAttribution.seedMergeDistanceTotal -
                seedAttribution.seedMergeCount;
    // These buckets are exclusive per NaturalAfterMerge+Seed window. Multiple
    // optimizer merges take precedence because later merges can restore FIFO;
    // a command-bearing After in a single-merge natural result is impossible.
    if (!valid) {
      add(c.cpuReadyMultiSourcePlannerSeedNaturalShapeMissing);
    } else if (seedAttribution.optimizerMergeCount > 1u) {
      add(c.cpuReadyMultiSourcePlannerSeedNaturalShapeMultiMerge);
    } else if (seedAttribution.seedMergeDistanceTotal == 1u &&
               classifiedIntervening == 0u) {
      add(c.cpuReadyMultiSourcePlannerSeedNaturalShapeAdjacent);
    } else if (seedAttribution.commandBefore != 0u &&
               seedAttribution.commandAfter == 0u) {
      add(c.cpuReadyMultiSourcePlannerSeedNaturalShapeDependencyKept);
    } else if (seedAttribution.commandBefore == 0u &&
               seedAttribution.commandAfter == 0u &&
               seedAttribution.emptyIntervening != 0u) {
      add(c.cpuReadyMultiSourcePlannerSeedNaturalShapeCommandless);
    } else {
      add(c.cpuReadyMultiSourcePlannerSeedNaturalShapeMissing);
    }
  }
  if (seedSecondNonDraw) {
    add(c.cpuReadyMultiSourcePlannerSeedSecondNonDraw);
  }
  if (seedBlockedCycle) {
    add(c.cpuReadyMultiSourcePlannerSeedBlockedCycle);
  }
}

void recordCpuReadyMultiSourceCompletionRegistration(
    std::uint64_t sources, bool fifoValid) {
  auto& c = counters();
  add(c.cpuReadyMultiSourceCompletionSources, sources);
  if (!fifoValid) {
    add(c.cpuReadyMultiSourceCompletionFifoFailures);
  }
}

void countChunkPublishSlotResidency(ChunkPublishReason reason,
                                    std::uint64_t nanoseconds) {
  auto& c = counters();
  add(c.chunkPublishSlotResidencySamples);
  add(c.chunkPublishSlotResidencyNs, nanoseconds);
  updateMax(c.chunkPublishSlotResidencyMaxNs, nanoseconds);
  recordRing(c.chunkPublishSlotResidencyRing, nanoseconds);

  const bool presentReason =
      reason == ChunkPublishReason::Present ||
      reason == ChunkPublishReason::PresentAcquire ||
      reason == ChunkPublishReason::PresentSplitBefore;
  if (presentReason) {
    add(c.chunkPublishSlotResidencyPresentSamples);
    add(c.chunkPublishSlotResidencyPresentNs, nanoseconds);
    updateMax(c.chunkPublishSlotResidencyPresentMaxNs, nanoseconds);
    recordRing(c.chunkPublishSlotResidencyPresentRing, nanoseconds);
  } else {
    add(c.chunkPublishSlotResidencyNonPresentSamples);
    add(c.chunkPublishSlotResidencyNonPresentNs, nanoseconds);
    updateMax(c.chunkPublishSlotResidencyNonPresentMaxNs, nanoseconds);
    recordRing(c.chunkPublishSlotResidencyNonPresentRing, nanoseconds);
  }
}

void countChunkPublishPresentPrePresentOpportunity(std::uint64_t commandCount,
                                                   std::uint64_t drawRunCount,
                                                   std::uint64_t drawItemCount,
                                                   std::uint64_t nonDrawCommandCount,
                                                   std::uint64_t payloadBytes,
                                                   std::uint64_t residencyNanoseconds,
                                                   bool presentIsTail) {
  auto& c = counters();
  add(c.chunkPublishPresentPrePresentOpportunitySlots);
  if (presentIsTail) {
    add(c.chunkPublishPresentPrePresentOpportunityTailSlots);
  } else {
    add(c.chunkPublishPresentPrePresentOpportunityNonTailSlots);
  }
  add(c.chunkPublishPresentPrePresentOpportunityCommands, commandCount);
  add(c.chunkPublishPresentPrePresentOpportunityDrawRuns, drawRunCount);
  add(c.chunkPublishPresentPrePresentOpportunityDrawItems, drawItemCount);
  add(c.chunkPublishPresentPrePresentOpportunityNonDrawCommands, nonDrawCommandCount);
  add(c.chunkPublishPresentPrePresentOpportunityPayloadBytes, payloadBytes);
  if (residencyNanoseconds != 0) {
    add(c.chunkPublishPresentPrePresentOpportunityResidencyNs, residencyNanoseconds);
    updateMax(c.chunkPublishPresentPrePresentOpportunityResidencyMaxNs,
              residencyNanoseconds);
    recordRing(c.chunkPublishPresentPrePresentOpportunityResidencyRing,
               residencyNanoseconds);
  }
}

void countEncodeSlotPsoPrefetchCpuTime(std::uint64_t nanoseconds) {
  auto& c = counters();
  add(c.encodeSlotPsoPrefetchCpuNs, nanoseconds);
  updateMax(c.encodeSlotPsoPrefetchCpuMaxNs, nanoseconds);
  recordRing(c.encodeSlotPsoPrefetchCpuRing, nanoseconds);
}

void countEncodeSlotPsoPrefetchCommands(std::uint64_t count) {
  add(counters().encodeSlotPsoPrefetchCommands, count);
}

void countEncodeSlotPsoPrefetchCandidates(std::uint64_t count) {
  add(counters().encodeSlotPsoPrefetchCandidates, count);
}

void countEncodeSlotPsoPrefetchTileCandidates(std::uint64_t count) {
  add(counters().encodeSlotPsoPrefetchTileCandidates, count);
}

void countEncodeSlotPsoPrefetchArgbufStage2Candidates(std::uint64_t count) {
  add(counters().encodeSlotPsoPrefetchArgbufStage2Candidates, count);
}

void countEncodeSlotPsoPrefetchArgbufResourceArrayCandidates(std::uint64_t count) {
  add(counters().encodeSlotPsoPrefetchArgbufResourceArrayCandidates, count);
}

void countEncodeSlotPsoPrefetchStateCopyCpuTime(std::uint64_t nanoseconds) {
  auto& c = counters();
  add(c.encodeSlotPsoPrefetchStateCopyCpuNs, nanoseconds);
  updateMax(c.encodeSlotPsoPrefetchStateCopyCpuMaxNs, nanoseconds);
  recordRing(c.encodeSlotPsoPrefetchStateCopyCpuRing, nanoseconds);
}

void countEncodeSlotPsoPrefetchDepthLookupCpuTime(std::uint64_t nanoseconds) {
  auto& c = counters();
  add(c.encodeSlotPsoPrefetchDepthLookupCpuNs, nanoseconds);
  updateMax(c.encodeSlotPsoPrefetchDepthLookupCpuMaxNs, nanoseconds);
  recordRing(c.encodeSlotPsoPrefetchDepthLookupCpuRing, nanoseconds);
}

void countEncodeSlotPsoPrefetchTileSelectCpuTime(std::uint64_t nanoseconds) {
  auto& c = counters();
  add(c.encodeSlotPsoPrefetchTileSelectCpuNs, nanoseconds);
  updateMax(c.encodeSlotPsoPrefetchTileSelectCpuMaxNs, nanoseconds);
  recordRing(c.encodeSlotPsoPrefetchTileSelectCpuRing, nanoseconds);
}

void countEncodeSlotPsoPrefetchTileBaseLookupCpuTime(std::uint64_t nanoseconds) {
  auto& c = counters();
  add(c.encodeSlotPsoPrefetchTileBaseLookupCpuNs, nanoseconds);
  updateMax(c.encodeSlotPsoPrefetchTileBaseLookupCpuMaxNs, nanoseconds);
  recordRing(c.encodeSlotPsoPrefetchTileBaseLookupCpuRing, nanoseconds);
}

void countEncodeSlotPsoPrefetchTileDrawLookupCpuTime(std::uint64_t nanoseconds) {
  auto& c = counters();
  add(c.encodeSlotPsoPrefetchTileDrawLookupCpuNs, nanoseconds);
  updateMax(c.encodeSlotPsoPrefetchTileDrawLookupCpuMaxNs, nanoseconds);
  recordRing(c.encodeSlotPsoPrefetchTileDrawLookupCpuRing, nanoseconds);
}

void countEncodeSlotPsoPrefetchArgbufSelectCpuTime(std::uint64_t nanoseconds) {
  auto& c = counters();
  add(c.encodeSlotPsoPrefetchArgbufSelectCpuNs, nanoseconds);
  updateMax(c.encodeSlotPsoPrefetchArgbufSelectCpuMaxNs, nanoseconds);
  recordRing(c.encodeSlotPsoPrefetchArgbufSelectCpuRing, nanoseconds);
}

void countEncodeSlotPsoPrefetchDrawKeyResolveCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeSlotPsoPrefetchDrawKeyResolveCpuNs, nanoseconds);
}

void countEncodeSlotPsoPrefetchDrawResolveFormatCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeSlotPsoPrefetchDrawResolveFormatCpuNs, nanoseconds);
}

void countEncodeSlotPsoPrefetchDrawResolveVariantKeyCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeSlotPsoPrefetchDrawResolveVariantKeyCpuNs, nanoseconds);
}

void countEncodeSlotPsoPrefetchDrawResolveShaderContextCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeSlotPsoPrefetchDrawResolveShaderContextCpuNs, nanoseconds);
}

void countEncodeSlotPsoPrefetchDrawResolveX8AlphaCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeSlotPsoPrefetchDrawResolveX8AlphaCpuNs, nanoseconds);
}

void countEncodeSlotPsoPrefetchDrawResolveVsoutLayoutCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeSlotPsoPrefetchDrawResolveVsoutLayoutCpuNs, nanoseconds);
}

void countEncodeSlotPsoPrefetchDrawResolveFragmentlessCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeSlotPsoPrefetchDrawResolveFragmentlessCpuNs, nanoseconds);
}

void countEncodeSlotPsoPrefetchDrawLookupCpuTime(std::uint64_t nanoseconds) {
  auto& c = counters();
  add(c.encodeSlotPsoPrefetchDrawLookupCpuNs, nanoseconds);
  updateMax(c.encodeSlotPsoPrefetchDrawLookupCpuMaxNs, nanoseconds);
  recordRing(c.encodeSlotPsoPrefetchDrawLookupCpuRing, nanoseconds);
}

void countEncodeSlotPsoPrefetchDrawSemanticKeyCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeSlotPsoPrefetchDrawSemanticKeyCpuNs, nanoseconds);
}

void countEncodeSlotPsoPrefetchDrawSemanticProbeCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeSlotPsoPrefetchDrawSemanticProbeCpuNs, nanoseconds);
}

void countEncodeSlotPsoPrefetchDrawSemanticStoreCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeSlotPsoPrefetchDrawSemanticStoreCpuNs, nanoseconds);
}

void countEncodeSlotPsoPrefetchDrawSemanticMemoHits(std::uint64_t count) {
  add(counters().encodeSlotPsoPrefetchDrawSemanticMemoHits, count);
}

void countEncodeSlotPsoPrefetchDrawSemanticMemoMisses(std::uint64_t count) {
  add(counters().encodeSlotPsoPrefetchDrawSemanticMemoMisses, count);
}

void countEncodeSlotPsoPrefetchDrawSemanticMemoOverflow(std::uint64_t count) {
  add(counters().encodeSlotPsoPrefetchDrawSemanticMemoOverflow, count);
}

void countEncodeSlotPsoPrefetchDrawSemanticMissProbeKeyHits(std::uint64_t count) {
  add(counters().encodeSlotPsoPrefetchDrawSemanticMissProbeKeyHits, count);
}

void countEncodeSlotPsoPrefetchDrawSemanticMissProbeKeySameSemantic(std::uint64_t count) {
  add(counters().encodeSlotPsoPrefetchDrawSemanticMissProbeKeySameSemantic,
      count);
}

void countEncodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffArgbufSelector(std::uint64_t count) {
  add(counters().encodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffArgbufSelector,
      count);
}

void countEncodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffVertexDecl(std::uint64_t count) {
  add(counters().encodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffVertexDecl,
      count);
}

void countEncodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffShader(std::uint64_t count) {
  add(counters().encodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffShader,
      count);
}

void countEncodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffRenderState(std::uint64_t count) {
  add(counters().encodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffRenderState,
      count);
}

void countEncodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffTextureHandles(std::uint64_t count) {
  add(counters().encodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffTextureHandles,
      count);
}

void countEncodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffTextureLod(std::uint64_t count) {
  add(counters().encodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffTextureLod,
      count);
}

void countEncodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffTextureStage(std::uint64_t count) {
  add(counters().encodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffTextureStage,
      count);
}

void countEncodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffSampler(std::uint64_t count) {
  add(counters().encodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffSampler,
      count);
}

void countEncodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffAttachment(std::uint64_t count) {
  add(counters().encodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffAttachment,
      count);
}

void countEncodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffClipPlane(std::uint64_t count) {
  add(counters().encodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffClipPlane,
      count);
}

void countEncodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffConstantUsage(std::uint64_t count) {
  add(counters().encodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffConstantUsage,
      count);
}

void countEncodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffSingleField(std::uint64_t count) {
  add(counters().encodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffSingleField,
      count);
}

void countEncodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffMultiField(std::uint64_t count) {
  add(counters().encodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffMultiField,
      count);
}

void countEncodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffTextureHandlesOnly(std::uint64_t count) {
  add(counters().encodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffTextureHandlesOnly,
      count);
}

void countEncodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffTextureHandlesWithOthers(std::uint64_t count) {
  add(counters().encodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffTextureHandlesWithOthers,
      count);
}

void countEncodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffHashOnly(std::uint64_t count) {
  add(counters().encodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffHashOnly,
      count);
}

void countEncodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffUnknown(std::uint64_t count) {
  add(counters().encodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffUnknown,
      count);
}

void countEncodeSlotPsoPrefetchDrawResourceShapeMemoCandidates(std::uint64_t count) {
  add(counters().encodeSlotPsoPrefetchDrawResourceShapeMemoCandidates, count);
}

void countEncodeSlotPsoPrefetchDrawResourceShapeMemoHits(std::uint64_t count) {
  add(counters().encodeSlotPsoPrefetchDrawResourceShapeMemoHits, count);
}

void countEncodeSlotPsoPrefetchDrawResourceShapeMemoMisses(std::uint64_t count) {
  add(counters().encodeSlotPsoPrefetchDrawResourceShapeMemoMisses, count);
}

void countEncodeSlotPsoPrefetchDrawResourceShapeMemoOverflow(std::uint64_t count) {
  add(counters().encodeSlotPsoPrefetchDrawResourceShapeMemoOverflow, count);
}

void countEncodeSlotPsoPrefetchDrawResourceShapeMemoStores(std::uint64_t count) {
  add(counters().encodeSlotPsoPrefetchDrawResourceShapeMemoStores, count);
}

void countEncodeSlotPsoPrefetchDrawResourceShapeMemoValidatedHits(std::uint64_t count) {
  add(counters().encodeSlotPsoPrefetchDrawResourceShapeMemoValidatedHits, count);
}

void countEncodeSlotPsoPrefetchDrawResourceShapeMemoValidatedMisses(std::uint64_t count) {
  add(counters().encodeSlotPsoPrefetchDrawResourceShapeMemoValidatedMisses,
      count);
}

void countEncodeSlotPsoPrefetchDrawResourceShapeMemoMismatchTextureMask(std::uint64_t count) {
  add(counters().encodeSlotPsoPrefetchDrawResourceShapeMemoMismatchTextureMask,
      count);
}

void countEncodeSlotPsoPrefetchDrawResourceShapeMemoMismatchTextureTypes(std::uint64_t count) {
  add(counters().encodeSlotPsoPrefetchDrawResourceShapeMemoMismatchTextureTypes,
      count);
}

void countEncodeSlotPsoPrefetchDrawResourceShapeMemoMismatchX8Alpha(std::uint64_t count) {
  add(counters().encodeSlotPsoPrefetchDrawResourceShapeMemoMismatchX8Alpha,
      count);
}

void countEncodeSlotPsoPrefetchDrawResourceShapeMemoMismatchAttachment(std::uint64_t count) {
  add(counters().encodeSlotPsoPrefetchDrawResourceShapeMemoMismatchAttachment,
      count);
}

void countEncodeSlotPsoPrefetchDrawResourceShapeMemoMismatchSamplerLodBias(std::uint64_t count) {
  add(counters().encodeSlotPsoPrefetchDrawResourceShapeMemoMismatchSamplerLodBias,
      count);
}

void countEncodeSlotPsoPrefetchDrawResourceShapeMemoMismatchVsOut(std::uint64_t count) {
  add(counters().encodeSlotPsoPrefetchDrawResourceShapeMemoMismatchVsOut,
      count);
}

void countEncodeSlotPsoPrefetchDrawResourceShapeMemoMismatchOther(std::uint64_t count) {
  add(counters().encodeSlotPsoPrefetchDrawResourceShapeMemoMismatchOther,
      count);
}

void countEncodeSlotPsoPrefetchDrawProbeKeyMemoHits(std::uint64_t count) {
  add(counters().encodeSlotPsoPrefetchDrawProbeKeyMemoHits, count);
}

void countEncodeSlotPsoPrefetchDrawProbeKeyMemoMisses(std::uint64_t count) {
  add(counters().encodeSlotPsoPrefetchDrawProbeKeyMemoMisses, count);
}

void countEncodeSlotPsoPrefetchDrawProbeKeyMemoOverflow(std::uint64_t count) {
  add(counters().encodeSlotPsoPrefetchDrawProbeKeyMemoOverflow, count);
}

void countEncodeSlotPsoPrefetchDrawHandleAdjacentCandidates(std::uint64_t count) {
  add(counters().encodeSlotPsoPrefetchDrawHandleAdjacentCandidates, count);
}

void countEncodeSlotPsoPrefetchDrawHandleAdjacentHits(std::uint64_t count) {
  add(counters().encodeSlotPsoPrefetchDrawHandleAdjacentHits, count);
}

void countEncodeSlotPsoPrefetchDrawHandleSlotRepeatHits(std::uint64_t count) {
  add(counters().encodeSlotPsoPrefetchDrawHandleSlotRepeatHits, count);
}

void countEncodeSlotPsoPrefetchDrawHandleSlotUnique(std::uint64_t count) {
  add(counters().encodeSlotPsoPrefetchDrawHandleSlotUnique, count);
}

void countEncodeSlotPsoPrefetchDrawHandleSlotOverflow(std::uint64_t count) {
  add(counters().encodeSlotPsoPrefetchDrawHandleSlotOverflow, count);
}

void countCommandBuffer() {
  add(counters().commandBuffers);
}

void countSubCommandBufferCommit() {
  add(counters().subCommandBufferCommits);
}

void countSubCommandBufferSplitSuppressedByCap() {
  add(counters().subCommandBufferSplitSuppressedByCap);
}

void recordChunkSubCBCount(std::uint64_t perChunkCount) {
  updateMax(counters().chunkSubCBCountMax, perChunkCount);
}

void countGpuCommandBufferError() {
  add(counters().gpuCommandBufferErrors);
}

void countMetalBuffer(std::size_t bytes) {
  add(counters().metalBuffers);
  add(counters().metalBufferBytes, static_cast<std::uint64_t>(bytes));
}

// R-BACK-39.2 (Task B11, L1) — frame-graph observe-path counters. Magnitude
// variants (built/coalesced/dead/memoryless) mirror the `add(field, n)` byte
// counters; dagDumpsWritten is an event increment like countChunkAdmit.
void countFramegraphPassesBuilt(std::uint64_t passes) {
  add(counters().framegraphPassesBuilt, passes);
}

void countFramegraphPassesCoalesced(std::uint64_t passes) {
  add(counters().framegraphPassesCoalesced, passes);
}

void countFramegraphPassesDead(std::uint64_t passes) {
  add(counters().framegraphPassesDead, passes);
}

void countFramegraphResourcesMemoryless(std::uint64_t resources) {
  add(counters().framegraphResourcesMemoryless, resources);
}

void countFramegraphDagDumpWritten() {
  add(counters().framegraphDagDumpsWritten);
}

void countFramegraphDceDropped(std::uint64_t passes) {
  add(counters().framegraphDceDropped, passes);
}

void countFramegraphDcePreservedUnprovable(std::uint64_t passes) {
  add(counters().framegraphDcePreservedUnprovable, passes);
}

void countFramegraphDceCrossChunkProofResources(std::uint64_t resources) {
  add(counters().framegraphDceCrossChunkProofResources, resources);
}

void countFramegraphDceReplayCommandsOmitted(std::uint64_t commands) {
  add(counters().framegraphDceReplayCommandsOmitted, commands);
}

void countFramegraphDceLookaheadPrefix(std::uint64_t commands) {
  add(counters().framegraphDceLookaheadPrefixes);
  add(counters().framegraphDceLookaheadPrefixCommands, commands);
}

void countFramegraphDceLookaheadSelected() {
  add(counters().framegraphDceLookaheadSelected);
}

void countFramegraphDceLookaheadFailOpen() {
  add(counters().framegraphDceLookaheadFailOpen);
}

void recordFramegraphSourceLocalPassCoalesce(
    FramegraphSourceProvenance provenance, bool sourceIdentityKnown,
    FramegraphSourceLocalPassCoalesceDiagnostic diagnostic) {
  if (diagnostic.candidates == 0 || !enabled()) {
    return;
  }
  auto& c = counters();
  addEnabledNonZero(c.framegraphSourceLocalReturnCandidates,
                    diagnostic.candidates);
  addEnabledNonZero(c.framegraphSourceLocalReturnMerged, diagnostic.merged);
  addEnabledNonZero(c.framegraphSourceLocalReturnBlockedCycle,
                    diagnostic.blockedCycle);
  addEnabledNonZero(c.framegraphSourceLocalReturnSecondNonDraw,
                    diagnostic.secondNonDraw);
  addEnabledNonZero(c.framegraphSourceLocalReturnNonRenderIntervener,
                    diagnostic.nonRenderIntervener);
  addEnabledNonZero(c.framegraphSourceLocalReturnMissingInvariant,
                    diagnostic.missingInvariant);
  addEnabledNonZero(c.framegraphSourceLocalReturnDependencyKept,
                    diagnostic.dependencyKept);
  addEnabledNonZero(c.framegraphSourceLocalReturnMoveBefore,
                    diagnostic.moveBefore);
  addEnabledNonZero(c.framegraphSourceLocalReturnMoveAfter,
                    diagnostic.moveAfter);
  addEnabledNonZero(c.framegraphSourceLocalReturnNonDrawIntervener,
                    diagnostic.nonDrawIntervener);
  addEnabledNonZero(c.framegraphSourceLocalReturnSemanticIntervener,
                    diagnostic.semanticIntervener);
  addEnabledNonZero(c.framegraphSourceLocalReturnCommandlessIntervener,
                    diagnostic.commandlessIntervener);
  addEnabledNonZero(c.framegraphSourceLocalReturnCommandless,
                    diagnostic.commandlessReturn);
  switch (provenance) {
  case FramegraphSourceProvenance::Legacy:
    addEnabledNonZero(c.framegraphSourceLocalReturnLegacyCandidates,
                      diagnostic.candidates);
    break;
  case FramegraphSourceProvenance::Arena:
    addEnabledNonZero(c.framegraphSourceLocalReturnArenaCandidates,
                      diagnostic.candidates);
    break;
  case FramegraphSourceProvenance::Unknown:
    addEnabledNonZero(c.framegraphSourceLocalReturnUnknownCandidates,
                      diagnostic.candidates);
    break;
  }
  addEnabledNonZero(
      sourceIdentityKnown
          ? c.framegraphSourceLocalReturnIdentityKnownCandidates
          : c.framegraphSourceLocalReturnIdentityMissingCandidates,
      diagnostic.candidates);
}

void recordFramegraphSourceLocalReplayOutcome(
    FramegraphSourceLocalReplayOutcome outcome,
    std::uint64_t candidates, std::uint64_t merged) {
  if (candidates == 0 || !enabled()) {
    return;
  }
  auto& c = counters();
  std::atomic<std::uint64_t>* sourcesCounter = nullptr;
  std::atomic<std::uint64_t>* candidatesCounter = nullptr;
  std::atomic<std::uint64_t>* mergedCounter = nullptr;
  switch (outcome) {
  case FramegraphSourceLocalReplayOutcome::FinalInvalid:
    sourcesCounter = &c.framegraphSourceLocalReturnFinalInvalidSources;
    candidatesCounter = &c.framegraphSourceLocalReturnFinalInvalidCandidates;
    mergedCounter = &c.framegraphSourceLocalReturnFinalInvalidMerged;
    break;
  case FramegraphSourceLocalReplayOutcome::FinalNaturalOrder:
    sourcesCounter = &c.framegraphSourceLocalReturnFinalNaturalOrderSources;
    candidatesCounter =
        &c.framegraphSourceLocalReturnFinalNaturalOrderCandidates;
    mergedCounter = &c.framegraphSourceLocalReturnFinalNaturalOrderMerged;
    break;
  case FramegraphSourceLocalReplayOutcome::FinalReorderedActivated:
    sourcesCounter =
        &c.framegraphSourceLocalReturnFinalReorderedActivatedSources;
    candidatesCounter =
        &c.framegraphSourceLocalReturnFinalReorderedActivatedCandidates;
    mergedCounter =
        &c.framegraphSourceLocalReturnFinalReorderedActivatedMerged;
    break;
  }
  addEnabledNonZero(*sourcesCounter, 1);
  addEnabledNonZero(*candidatesCounter, candidates);
  addEnabledNonZero(*mergedCounter, merged);
}

void recordFramegraphSourceLocalFrontierRollback(
    FramegraphSourceLocalFrontierRollbackReason reason,
    std::uint64_t candidates, std::uint64_t merged) {
  if (candidates == 0 || !enabled()) {
    return;
  }
  auto& c = counters();
  std::atomic<std::uint64_t>* sourcesCounter = nullptr;
  std::atomic<std::uint64_t>* candidatesCounter = nullptr;
  std::atomic<std::uint64_t>* mergedCounter = nullptr;
  switch (reason) {
  case FramegraphSourceLocalFrontierRollbackReason::InvalidPlan:
    sourcesCounter =
        &c.framegraphSourceLocalReturnFrontierRollbackInvalidPlanSources;
    candidatesCounter =
        &c.framegraphSourceLocalReturnFrontierRollbackInvalidPlanCandidates;
    mergedCounter =
        &c.framegraphSourceLocalReturnFrontierRollbackInvalidPlanMerged;
    break;
  case FramegraphSourceLocalFrontierRollbackReason::LiveSetMismatch:
    sourcesCounter =
        &c.framegraphSourceLocalReturnFrontierRollbackLiveSetMismatchSources;
    candidatesCounter =
        &c.framegraphSourceLocalReturnFrontierRollbackLiveSetMismatchCandidates;
    mergedCounter =
        &c.framegraphSourceLocalReturnFrontierRollbackLiveSetMismatchMerged;
    break;
  case FramegraphSourceLocalFrontierRollbackReason::DuplicateCommand:
    sourcesCounter =
        &c.framegraphSourceLocalReturnFrontierRollbackDuplicateCommandSources;
    candidatesCounter =
        &c.framegraphSourceLocalReturnFrontierRollbackDuplicateCommandCandidates;
    mergedCounter =
        &c.framegraphSourceLocalReturnFrontierRollbackDuplicateCommandMerged;
    break;
  case FramegraphSourceLocalFrontierRollbackReason::MovedHeadUnproved:
    sourcesCounter =
        &c.framegraphSourceLocalReturnFrontierRollbackMovedHeadUnprovedSources;
    candidatesCounter =
        &c.framegraphSourceLocalReturnFrontierRollbackMovedHeadUnprovedCandidates;
    mergedCounter =
        &c.framegraphSourceLocalReturnFrontierRollbackMovedHeadUnprovedMerged;
    break;
  }
  addEnabledNonZero(c.framegraphSourceLocalReturnFrontierRollbackSources, 1);
  addEnabledNonZero(c.framegraphSourceLocalReturnFrontierRollbackCandidates,
                    candidates);
  addEnabledNonZero(c.framegraphSourceLocalReturnFrontierRollbackMerged,
                    merged);
  addEnabledNonZero(*sourcesCounter, 1);
  addEnabledNonZero(*candidatesCounter, candidates);
  addEnabledNonZero(*mergedCounter, merged);
}

void countFramegraphActiveRenderSeedOutcome(
    FramegraphActiveRenderSeedOutcome outcome, std::uint64_t count) {
  if (count == 0) {
    return;
  }
  auto& c = counters();
  switch (outcome) {
  case FramegraphActiveRenderSeedOutcome::SnapshotAbsent:
    add(c.framegraphActiveRenderSnapshotAbsent, count);
    break;
  case FramegraphActiveRenderSeedOutcome::SnapshotIncomplete:
    add(c.framegraphActiveRenderSnapshotIncomplete, count);
    break;
  case FramegraphActiveRenderSeedOutcome::ApplyApplied:
    add(c.framegraphActiveRenderSeedApplyApplied, count);
    break;
  case FramegraphActiveRenderSeedOutcome::ApplyInvalid:
    add(c.framegraphActiveRenderSeedApplyInvalid, count);
    break;
  case FramegraphActiveRenderSeedOutcome::ApplyIncomplete:
    add(c.framegraphActiveRenderSeedApplyIncomplete, count);
    break;
  case FramegraphActiveRenderSeedOutcome::ApplyOverflow:
    add(c.framegraphActiveRenderSeedApplyOverflow, count);
    break;
  case FramegraphActiveRenderSeedOutcome::AppliedButUnmerged:
    add(c.framegraphActiveRenderSeedAppliedButUnmerged, count);
    break;
  case FramegraphActiveRenderSeedOutcome::PassCoalesceBlockedCycle:
    add(c.framegraphActiveRenderSeedPassCoalesceBlockedCycle, count);
    break;
  case FramegraphActiveRenderSeedOutcome::PassCoalesceSecondNonDraw:
    add(c.framegraphActiveRenderSeedPassCoalesceSecondNonDraw, count);
    break;
  case FramegraphActiveRenderSeedOutcome::MovedHeadProved:
    add(c.framegraphActiveRenderSeedMovedHeadProved, count);
    break;
  case FramegraphActiveRenderSeedOutcome::FallbackMovedHeadUnproved:
    add(c.framegraphActiveRenderSeedFallbackMovedHeadUnproved, count);
    break;
  case FramegraphActiveRenderSeedOutcome::FallbackInvalidPlan:
    add(c.framegraphActiveRenderSeedFallbackInvalidPlan, count);
    break;
  case FramegraphActiveRenderSeedOutcome::FallbackLiveSetMismatch:
    add(c.framegraphActiveRenderSeedFallbackLiveSetMismatch, count);
    break;
  case FramegraphActiveRenderSeedOutcome::FallbackDuplicateCommand:
    add(c.framegraphActiveRenderSeedFallbackDuplicateCommand, count);
    break;
  case FramegraphActiveRenderSeedOutcome::ReplayActivated:
    add(c.framegraphActiveRenderSeedReplayActivated, count);
    break;
  }
}

void countPipelineBuild() {
  add(counters().pipelineBuilds);
}

void countPipelineCacheHit(PipelineKind kind) {
  add(pipelineHitCounter(counters(), kind));
}

void countPipelineCacheMiss(PipelineKind kind) {
  add(pipelineMissCounter(counters(), kind));
}

void countPipelineBuild(PipelineKind kind) {
  countPipelineBuild();
  add(pipelineBuildCounter(counters(), kind));
}

void recordDrawPsoSlotCount(std::uint64_t count) {
  counters().psoSlotsDraw.store(count, std::memory_order_relaxed);
  updateMax(counters().psoSlotsDrawMax, count);
}

void countDrawPsoSlotExhausted() {
  add(counters().psoSlotExhausted);
}

void countDrawPsoVariantArgbufStage2() {
  add(counters().psoVariantArgbufStage2);
}

void countDrawPsoVariantTileFfp() {
  add(counters().psoVariantTileFfp);
}

void recordSourceLibraryEntryCount(std::uint64_t count) {
  counters().sourceLibraryEntries.store(count, std::memory_order_relaxed);
  updateMax(counters().sourceLibraryEntriesMax, count);
}

void countPipelineBuildFailDraw() {
  add(counters().pipelineBuildFailDraw);
}

void countPipelineBuildFailLibrary() {
  add(counters().pipelineBuildFailLibrary);
}

void countPipelineBuildFailFunction() {
  add(counters().pipelineBuildFailFunction);
}

void countPipelineBuildFailPso() {
  add(counters().pipelineBuildFailPso);
}

void countDrawSkippedNoPipeline() {
  add(counters().drawSkippedNoPipeline);
}

void countShaderVariantKeyHashCpuTime(std::uint64_t nanoseconds) {
  add(counters().shaderVariantKeyHashCpuNs, nanoseconds);
  updateMax(counters().shaderVariantKeyHashCpuMaxNs, nanoseconds);
  recordRing(counters().shaderVariantKeyHashCpuRing, nanoseconds);
}

void countRenderPassBegin() {
  add(counters().renderPassBegin);
}

void countRenderPassEnd(EncoderSplitReason reason) {
  add(counters().renderPassEnd);
  add(splitReasonCounter(counters(), reason));
}

void countHazardProbe(bool bloomOverlap, bool exactOverlap) {
  add(counters().hazardProbeComparisons);
  if (bloomOverlap) {
    add(counters().hazardBloomOverlaps);
  }
  if (exactOverlap) {
    add(counters().hazardExactOverlaps);
  }
  if (bloomOverlap && !exactOverlap) {
    add(counters().hazardBloomFalsePositive);
  }
}


void countCommitChunkDrawSubmissionBatch(std::uint32_t recordCount) {
  auto& c = counters();
  add(c.commitChunkDrawSubmissionBatchSubmits);
  add(c.commitChunkDrawSubmissionBatchRecords, recordCount);
  updateMax(c.commitChunkDrawSubmissionBatchMaxRecords, recordCount);
  if (recordCount <= 1u) {
    add(c.commitChunkDrawSubmissionBatchSize1);
  } else if (recordCount == 2u) {
    add(c.commitChunkDrawSubmissionBatchSize2);
  } else if (recordCount <= 4u) {
    add(c.commitChunkDrawSubmissionBatchSize3To4);
  } else if (recordCount <= 8u) {
    add(c.commitChunkDrawSubmissionBatchSize5To8);
  } else if (recordCount <= 16u) {
    add(c.commitChunkDrawSubmissionBatchSize9To16);
  } else if (recordCount <= 32u) {
    add(c.commitChunkDrawSubmissionBatchSize17To32);
  } else {
    add(c.commitChunkDrawSubmissionBatchSize33Plus);
  }
}


void countSubmitDrawRunBatchGroup(std::uint32_t recordCount) {
  auto& c = counters();
  add(c.submitDrawRunBatchGroups);
  add(c.submitDrawRunBatchRecords, recordCount);
  updateMax(c.submitDrawRunBatchMaxRecords, recordCount);
}

void countSubmitDrawRunBatchDiscardedState(std::uint64_t records,
                                           std::uint64_t bytes) {
  auto& c = counters();
  add(c.submitDrawRunBatchDiscardedStateRecords, records);
  add(c.submitDrawRunBatchDiscardedStateBytes, bytes);
}

void countSubmitDrawRunBindingSnapshotCpuTime(std::uint64_t nanoseconds) {
  auto& c = counters();
  recordCpuTime(c.submitDrawRunBindingSnapshotCpuNs,
                c.submitDrawRunBindingSnapshotCpuMaxNs,
                nanoseconds);
}

void countSubmitDrawRunPayloadBytesCpuTime(std::uint64_t nanoseconds) {
  auto& c = counters();
  recordCpuTime(c.submitDrawRunPayloadBytesCpuNs,
                c.submitDrawRunPayloadBytesCpuMaxNs,
                nanoseconds);
}

void countSubmitDrawRunSlotPrepareCpuTime(std::uint64_t nanoseconds) {
  auto& c = counters();
  recordCpuTime(c.submitDrawRunSlotPrepareCpuNs,
                c.submitDrawRunSlotPrepareCpuMaxNs,
                nanoseconds);
}

void countSubmitDrawRunResourceMarkCpuTime(std::uint64_t nanoseconds) {
  auto& c = counters();
  recordCpuTime(c.submitDrawRunResourceMarkCpuNs,
                c.submitDrawRunResourceMarkCpuMaxNs,
                nanoseconds);
}

void countSubmitDrawRunAppendCpuTime(std::uint64_t nanoseconds) {
  auto& c = counters();
  recordCpuTime(c.submitDrawRunAppendCpuNs,
                c.submitDrawRunAppendCpuMaxNs,
                nanoseconds);
}

void countSubmitDrawRunChunkCommitCpuTime(std::uint64_t nanoseconds) {
  auto& c = counters();
  recordCpuTime(c.submitDrawRunChunkCommitCpuNs,
                c.submitDrawRunChunkCommitCpuMaxNs,
                nanoseconds);
}

void countSubmitDrawRunBatchQueueLockCpuTime(std::uint64_t nanoseconds) {
  auto& c = counters();
  recordCpuTime(c.submitDrawRunBatchQueueLockCpuNs,
                c.submitDrawRunBatchQueueLockCpuMaxNs,
                nanoseconds);
}

void countSubmitDrawRunBatchCompatScanCpuTime(std::uint64_t nanoseconds) {
  auto& c = counters();
  recordCpuTime(c.submitDrawRunBatchCompatScanCpuNs,
                c.submitDrawRunBatchCompatScanCpuMaxNs,
                nanoseconds);
}

void countSubmitDrawRunBatchSubmissionAdjacent(bool sameGenerationLane) {
  auto& c = counters();
  add(c.submitDrawRunBatchSubmissionAdjacentPairs);
  if (sameGenerationLane) {
    add(c.submitDrawRunBatchSubmissionAdjacentSameGenerationLane);
  }
}

void countSubmitDrawRunBatchCompatPair(bool sameGenerationLane,
                                       bool compatible) {
  auto& c = counters();
  add(c.submitDrawRunBatchCompatPairs);
  add(compatible ? c.submitDrawRunBatchCompatCompatible
                 : c.submitDrawRunBatchCompatIncompatible);
  if (!sameGenerationLane) {
    return;
  }
  add(c.submitDrawRunBatchCompatSameGenerationLane);
  add(compatible
          ? c.submitDrawRunBatchCompatSameGenerationLaneCompatible
          : c.submitDrawRunBatchCompatSameGenerationLaneIncompatible);
}

void countSubmitDrawRunBatchIncompat(std::uint8_t firstDiffClass,
                                     bool textureOnly) {
  auto& c = counters();
  switch (firstDiffClass) {
  case 0: add(c.submitDrawRunBatchIncompatTexture); break;
  case 1: add(c.submitDrawRunBatchIncompatSampler); break;
  case 2: add(c.submitDrawRunBatchIncompatTextureStageState); break;
  case 3: add(c.submitDrawRunBatchIncompatRenderState); break;
  case 4: add(c.submitDrawRunBatchIncompatShader); break;
  case 5: add(c.submitDrawRunBatchIncompatVertexDecl); break;
  case 6: add(c.submitDrawRunBatchIncompatAttachment); break;
  case 7: add(c.submitDrawRunBatchIncompatViewport); break;
  case 8: add(c.submitDrawRunBatchIncompatClipPlane); break;
  case 9: add(c.submitDrawRunBatchIncompatLayoutUsage); break;
  default: add(c.submitDrawRunBatchIncompatUnknown); break;
  }
  if (textureOnly) {
    add(c.submitDrawRunBatchIncompatTextureOnly);
  }
}

void countSubmitDrawRunBatchIncompatRenderState(std::uint8_t diffClass) {
  auto& c = counters();
  switch (diffClass) {
  case 0: add(c.submitDrawRunBatchIncompatRsAlphaTestOnly); break;
  case 1: add(c.submitDrawRunBatchIncompatRsBlendOnly); break;
  case 2: add(c.submitDrawRunBatchIncompatRsCullOnly); break;
  case 3: add(c.submitDrawRunBatchIncompatRsDepthOnly); break;
  case 4: add(c.submitDrawRunBatchIncompatRsFogOnly); break;
  case 5: add(c.submitDrawRunBatchIncompatRsTextureFactorOnly); break;
  case 6: add(c.submitDrawRunBatchIncompatRsSingleOther); break;
  case 7: add(c.submitDrawRunBatchIncompatRsMixed); break;
  default: break;
  }
}

void countSubmitDrawRunBatchBindingOverrideCpuTime(std::uint64_t nanoseconds) {
  auto& c = counters();
  recordCpuTime(c.submitDrawRunBatchBindingOverrideCpuNs,
                c.submitDrawRunBatchBindingOverrideCpuMaxNs,
                nanoseconds);
}

void countSubmitDrawRunBatchBindingSnapshotCpuTime(std::uint64_t nanoseconds) {
  auto& c = counters();
  recordCpuTime(c.submitDrawRunBatchBindingSnapshotCpuNs,
                c.submitDrawRunBatchBindingSnapshotCpuMaxNs,
                nanoseconds);
}

void countSubmitDrawRunBatchPayloadBytesCpuTime(std::uint64_t nanoseconds) {
  auto& c = counters();
  recordCpuTime(c.submitDrawRunBatchPayloadBytesCpuNs,
                c.submitDrawRunBatchPayloadBytesCpuMaxNs,
                nanoseconds);
}

void countSubmitDrawRunBatchSlotPrepareCpuTime(std::uint64_t nanoseconds) {
  auto& c = counters();
  recordCpuTime(c.submitDrawRunBatchSlotPrepareCpuNs,
                c.submitDrawRunBatchSlotPrepareCpuMaxNs,
                nanoseconds);
}

void countSubmitDrawRunBatchResourceMarkCpuTime(std::uint64_t nanoseconds) {
  auto& c = counters();
  recordCpuTime(c.submitDrawRunBatchResourceMarkCpuNs,
                c.submitDrawRunBatchResourceMarkCpuMaxNs,
                nanoseconds);
}

void countSubmitDrawRunBatchAppendCpuTime(std::uint64_t nanoseconds) {
  auto& c = counters();
  recordCpuTime(c.submitDrawRunBatchAppendCpuNs,
                c.submitDrawRunBatchAppendCpuMaxNs,
                nanoseconds);
}

void countSubmitDrawRunBatchAppendReserveCpuTime(std::uint64_t nanoseconds) {
  auto& c = counters();
  recordCpuTime(c.submitDrawRunBatchAppendReserveCpuNs,
                c.submitDrawRunBatchAppendReserveCpuMaxNs,
                nanoseconds);
}

void countSubmitDrawRunBatchAppendStateCpuTime(std::uint64_t nanoseconds) {
  auto& c = counters();
  recordCpuTime(c.submitDrawRunBatchAppendStateCpuNs,
                c.submitDrawRunBatchAppendStateCpuMaxNs,
                nanoseconds);
}

void countSubmitDrawRunBatchAppendStatePsoCpuTime(std::uint64_t nanoseconds) {
  auto& c = counters();
  recordCpuTime(c.submitDrawRunBatchAppendStatePsoCpuNs,
                c.submitDrawRunBatchAppendStatePsoCpuMaxNs,
                nanoseconds);
}

void countSubmitDrawRunBatchAppendStateInvariantCpuTime(std::uint64_t nanoseconds) {
  auto& c = counters();
  recordCpuTime(c.submitDrawRunBatchAppendStateInvariantCpuNs,
                c.submitDrawRunBatchAppendStateInvariantCpuMaxNs,
                nanoseconds);
}

void countSubmitDrawRunBatchAppendStateSoaCpuTime(std::uint64_t nanoseconds) {
  auto& c = counters();
  recordCpuTime(c.submitDrawRunBatchAppendStateSoaCpuNs,
                c.submitDrawRunBatchAppendStateSoaCpuMaxNs,
                nanoseconds);
}

void countSubmitDrawRunBatchAppendUniformCpuTime(std::uint64_t nanoseconds) {
  auto& c = counters();
  recordCpuTime(c.submitDrawRunBatchAppendUniformCpuNs,
                c.submitDrawRunBatchAppendUniformCpuMaxNs,
                nanoseconds);
}

void countSubmitDrawRunBatchAppendPayloadCpuTime(std::uint64_t nanoseconds) {
  auto& c = counters();
  recordCpuTime(c.submitDrawRunBatchAppendPayloadCpuNs,
                c.submitDrawRunBatchAppendPayloadCpuMaxNs,
                nanoseconds);
}

void countSubmitDrawRunBatchAppendParamCpuTime(std::uint64_t nanoseconds) {
  auto& c = counters();
  recordCpuTime(c.submitDrawRunBatchAppendParamCpuNs,
                c.submitDrawRunBatchAppendParamCpuMaxNs,
                nanoseconds);
}

void countSubmitDrawRunBatchAppendRecordCpuTime(std::uint64_t nanoseconds) {
  auto& c = counters();
  recordCpuTime(c.submitDrawRunBatchAppendRecordCpuNs,
                c.submitDrawRunBatchAppendRecordCpuMaxNs,
                nanoseconds);
}

void countSubmitDrawRunBatchAppendPayloadBytes(std::uint64_t bytes) {
  add(counters().submitDrawRunBatchAppendPayloadBytes, bytes);
}

void countSubmitDrawRunBatchAppendParams(std::uint64_t paramCount) {
  add(counters().submitDrawRunBatchAppendParams, paramCount);
}

void countSubmitDrawRunBatchChunkCommitCpuTime(std::uint64_t nanoseconds) {
  auto& c = counters();
  recordCpuTime(c.submitDrawRunBatchChunkCommitCpuNs,
                c.submitDrawRunBatchChunkCommitCpuMaxNs,
                nanoseconds);
}

void countD3D9DrawStateCacheLookup(bool hit, bool includeIndexBuffer) {
  auto& c = counters();
  if (hit) {
    add(c.d3d9DrawStateCacheHits);
    add(includeIndexBuffer ? c.d3d9DrawStateCacheHitWithIndex
                           : c.d3d9DrawStateCacheHitNoIndex);
  } else {
    add(c.d3d9DrawStateCacheMisses);
    add(includeIndexBuffer ? c.d3d9DrawStateCacheMissWithIndex
                           : c.d3d9DrawStateCacheMissNoIndex);
  }
}

void countD3D9DrawStateCacheDirectLookup(bool hit, bool includeIndexBuffer) {
  auto& c = counters();
  if (hit) {
    add(c.d3d9DrawStateCacheDirectHits);
    add(includeIndexBuffer ? c.d3d9DrawStateCacheDirectHitWithIndex
                           : c.d3d9DrawStateCacheDirectHitNoIndex);
  } else {
    add(c.d3d9DrawStateCacheDirectMisses);
    add(includeIndexBuffer ? c.d3d9DrawStateCacheDirectMissWithIndex
                           : c.d3d9DrawStateCacheDirectMissNoIndex);
  }
}

void countD3D9DrawStateCacheBatchLookup(bool hit) {
  auto& c = counters();
  add(hit ? c.d3d9DrawStateCacheBatchHits
          : c.d3d9DrawStateCacheBatchMisses);
}

void countD3D9DrawStateCacheUniformRefresh() {
  add(counters().d3d9DrawStateCacheUniformRefreshes);
}

void countD3D9DrawStateCacheBatchMissReason(std::uint32_t reasonMask) {
  auto& c = counters();
  using namespace dxmt9::core;
  if (reasonMask == DrawStateInvalidationUnknown) {
    add(c.d3d9DrawStateCacheBatchMissReasonUnknown);
    return;
  }

  enum Category : std::uint32_t {
    CategoryRenderState = 1u << 0,
    CategoryTexture = 1u << 1,
    CategoryFvfVdecl = 1u << 2,
    CategoryShader = 1u << 3,
    CategoryRtDepth = 1u << 4,
    CategoryViewportScissor = 1u << 5,
    CategoryTssSampler = 1u << 6,
    CategoryFfpClip = 1u << 7,
    CategoryBroad = 1u << 8,
  };

  std::uint32_t categories = 0;
  const auto includeIf = [&](std::uint32_t bits, Category category) {
    if ((reasonMask & bits) != 0) {
      categories |= category;
    }
  };

  includeIf(DrawStateInvalidationRenderState, CategoryRenderState);
  includeIf(DrawStateInvalidationTexture | DrawStateInvalidationTextureLod,
            CategoryTexture);
  includeIf(DrawStateInvalidationFvfVdecl, CategoryFvfVdecl);
  includeIf(DrawStateInvalidationShader, CategoryShader);
  includeIf(DrawStateInvalidationRenderTargetDepth, CategoryRtDepth);
  includeIf(DrawStateInvalidationViewportScissor, CategoryViewportScissor);
  includeIf(DrawStateInvalidationTextureStageSampler |
                DrawStateInvalidationTextureStageState |
                DrawStateInvalidationSamplerState,
            CategoryTssSampler);
  includeIf(DrawStateInvalidationFfpState | DrawStateInvalidationClipPlane,
            CategoryFfpClip);
  includeIf(DrawStateInvalidationMutableState |
                DrawStateInvalidationStateBlock |
                DrawStateInvalidationReset |
                DrawStateInvalidationSwapChain,
            CategoryBroad);

  if (categories == 0) {
    add(c.d3d9DrawStateCacheBatchMissReasonBindingOnly);
    return;
  }

  const auto addIfCategory = [&](Category category, auto& counter) {
    if ((categories & category) != 0) {
      add(counter);
    }
  };
  addIfCategory(CategoryRenderState,
                c.d3d9DrawStateCacheBatchMissReasonHasRenderState);
  addIfCategory(CategoryTexture,
                c.d3d9DrawStateCacheBatchMissReasonHasTexture);
  addIfCategory(CategoryFvfVdecl,
                c.d3d9DrawStateCacheBatchMissReasonHasFvfVdecl);
  addIfCategory(CategoryShader,
                c.d3d9DrawStateCacheBatchMissReasonHasShader);
  addIfCategory(CategoryRtDepth,
                c.d3d9DrawStateCacheBatchMissReasonHasRtDepth);
  addIfCategory(CategoryViewportScissor,
                c.d3d9DrawStateCacheBatchMissReasonHasViewportScissor);
  addIfCategory(CategoryTssSampler,
                c.d3d9DrawStateCacheBatchMissReasonHasTssSampler);
  addIfCategory(CategoryFfpClip,
                c.d3d9DrawStateCacheBatchMissReasonHasFfpClip);
  addIfCategory(CategoryBroad,
                c.d3d9DrawStateCacheBatchMissReasonHasBroad);
  const auto addIfAllCategories = [&](std::uint32_t mask, auto& counter) {
    if ((categories & mask) == mask) {
      add(counter);
    }
  };
  addIfAllCategories(CategoryTexture | CategoryShader,
                     c.d3d9DrawStateCacheBatchMissReasonHasTextureShader);
  addIfAllCategories(CategoryTexture | CategoryFvfVdecl,
                     c.d3d9DrawStateCacheBatchMissReasonHasTextureFvfVdecl);
  addIfAllCategories(CategoryShader | CategoryFvfVdecl,
                     c.d3d9DrawStateCacheBatchMissReasonHasShaderFvfVdecl);
  addIfAllCategories(CategoryTexture | CategoryTssSampler,
                     c.d3d9DrawStateCacheBatchMissReasonHasTextureTssSampler);
  addIfAllCategories(CategoryTexture | CategoryShader | CategoryFvfVdecl,
                     c.d3d9DrawStateCacheBatchMissReasonHasTextureShaderFvfVdecl);
  addIfAllCategories(CategoryTexture | CategoryShader | CategoryTssSampler,
                     c.d3d9DrawStateCacheBatchMissReasonHasTextureShaderTssSampler);
  addIfAllCategories(CategoryTexture | CategoryFvfVdecl | CategoryTssSampler,
                     c.d3d9DrawStateCacheBatchMissReasonHasTextureFvfVdeclTssSampler);
  addIfAllCategories(CategoryShader | CategoryFvfVdecl | CategoryTssSampler,
                     c.d3d9DrawStateCacheBatchMissReasonHasShaderFvfVdeclTssSampler);
  addIfAllCategories(
      CategoryTexture | CategoryShader | CategoryFvfVdecl | CategoryTssSampler,
      c.d3d9DrawStateCacheBatchMissReasonHasTextureShaderFvfVdeclTssSampler);

  const auto countBits = [](std::uint32_t value) {
    std::uint32_t count = 0;
    while (value != 0) {
      value &= value - 1;
      ++count;
    }
    return count;
  };
  const auto categoryCount = countBits(categories);
  if (categoryCount >= 4) {
    add(c.d3d9DrawStateCacheBatchMissReasonMixed4Plus);
    return;
  }
  if (categoryCount == 3) {
    add(c.d3d9DrawStateCacheBatchMissReasonMixed3);
    return;
  }
  if (categoryCount == 2) {
    add(c.d3d9DrawStateCacheBatchMissReasonMixed2);
    return;
  }

  switch (categories) {
  case CategoryRenderState:
    add(c.d3d9DrawStateCacheBatchMissReasonSingleRenderState);
    break;
  case CategoryTexture:
    add(c.d3d9DrawStateCacheBatchMissReasonSingleTexture);
    break;
  case CategoryFvfVdecl:
    add(c.d3d9DrawStateCacheBatchMissReasonSingleFvfVdecl);
    break;
  case CategoryShader:
    add(c.d3d9DrawStateCacheBatchMissReasonSingleShader);
    break;
  case CategoryRtDepth:
    add(c.d3d9DrawStateCacheBatchMissReasonSingleRtDepth);
    break;
  case CategoryViewportScissor:
    add(c.d3d9DrawStateCacheBatchMissReasonSingleViewportScissor);
    break;
  case CategoryTssSampler:
    add(c.d3d9DrawStateCacheBatchMissReasonSingleTssSampler);
    break;
  case CategoryFfpClip:
    add(c.d3d9DrawStateCacheBatchMissReasonSingleFfpClip);
    break;
  case CategoryBroad:
    add(c.d3d9DrawStateCacheBatchMissReasonSingleBroad);
    break;
  default:
    add(c.d3d9DrawStateCacheBatchMissReasonUnknown);
    break;
  }
}

void countD3D9SnapshotCacheBatchMissSemanticReuseProbe(bool hit,
                                                       std::uint32_t distance) {
  auto& c = counters();
  add(c.d3d9SnapshotCacheBatchMissSemanticReuseProbeSamples);
  if (!hit) {
    add(c.d3d9SnapshotCacheBatchMissSemanticReuseProbeMisses);
    return;
  }

  add(c.d3d9SnapshotCacheBatchMissSemanticReuseProbeHits);
  if (distance <= 1u) {
    add(c.d3d9SnapshotCacheBatchMissSemanticReuseProbeHitDistance1);
  } else if (distance == 2u) {
    add(c.d3d9SnapshotCacheBatchMissSemanticReuseProbeHitDistance2);
  } else if (distance <= 4u) {
    add(c.d3d9SnapshotCacheBatchMissSemanticReuseProbeHitDistance3To4);
  } else {
    add(c.d3d9SnapshotCacheBatchMissSemanticReuseProbeHitDistance5To8);
  }
}

void countD3D9DrawStateCacheMissReason(std::uint32_t reasonMask) {
  auto& c = counters();
  if (reasonMask == dxmt9::core::DrawStateInvalidationUnknown) {
    add(c.d3d9DrawStateCacheMissAfterUnknown);
    return;
  }
  const auto countIf = [&](std::uint32_t bit, auto& counter) {
    if ((reasonMask & bit) != 0) {
      add(counter);
    }
  };
  countIf(dxmt9::core::DrawStateInvalidationMutableState,
          c.d3d9DrawStateCacheMissAfterMutableState);
  countIf(dxmt9::core::DrawStateInvalidationDrawPacket,
          c.d3d9DrawStateCacheMissAfterDrawPacket);
  countIf(dxmt9::core::DrawStateInvalidationRenderState,
          c.d3d9DrawStateCacheMissAfterRenderState);
  countIf(dxmt9::core::DrawStateInvalidationTexture,
          c.d3d9DrawStateCacheMissAfterTexture);
  countIf(dxmt9::core::DrawStateInvalidationStream,
          c.d3d9DrawStateCacheMissAfterStream);
  countIf(dxmt9::core::DrawStateInvalidationIndexBuffer,
          c.d3d9DrawStateCacheMissAfterIndexBuffer);
  countIf(dxmt9::core::DrawStateInvalidationFvfVdecl,
          c.d3d9DrawStateCacheMissAfterFvfVdecl);
  countIf(dxmt9::core::DrawStateInvalidationShader,
          c.d3d9DrawStateCacheMissAfterShader);
  countIf(dxmt9::core::DrawStateInvalidationRenderTargetDepth,
          c.d3d9DrawStateCacheMissAfterRenderTargetDepth);
  countIf(dxmt9::core::DrawStateInvalidationViewportScissor,
          c.d3d9DrawStateCacheMissAfterViewportScissor);
  countIf(dxmt9::core::DrawStateInvalidationTextureStageSampler,
          c.d3d9DrawStateCacheMissAfterTextureStageSampler);
  countIf(dxmt9::core::DrawStateInvalidationFfpState,
          c.d3d9DrawStateCacheMissAfterFfpState);
  countIf(dxmt9::core::DrawStateInvalidationClipPlane,
          c.d3d9DrawStateCacheMissAfterClipPlane);
  countIf(dxmt9::core::DrawStateInvalidationStateBlock,
          c.d3d9DrawStateCacheMissAfterStateBlock);
  countIf(dxmt9::core::DrawStateInvalidationReset,
          c.d3d9DrawStateCacheMissAfterReset);
  countIf(dxmt9::core::DrawStateInvalidationSwapChain,
          c.d3d9DrawStateCacheMissAfterSwapChain);
  countIf(dxmt9::core::DrawStateInvalidationTextureLod,
          c.d3d9DrawStateCacheMissAfterTextureLod);
}

void countDrawCall(std::uint32_t primitiveType,
                   std::uint32_t primitiveCount,
                   std::uint64_t vertexCount,
                   bool indexed,
                   bool expandedIndexed,
                   std::size_t userVertexBytes,
                   std::size_t userIndexBytes) {
  add(counters().drawCalls);
  if (indexed) {
    add(counters().drawIndexedCalls);
  }
  if (expandedIndexed) {
    add(counters().drawExpandedIndexedCalls);
  }
  add(counters().drawPrimitiveCount, primitiveCount);
  add(counters().drawTriangleEstimate, triangleEstimate(primitiveType, primitiveCount));
  add(counters().drawVertexCount, vertexCount);
  add(counters().drawUpVertexBytes, static_cast<std::uint64_t>(userVertexBytes));
  add(counters().drawUpIndexBytes, static_cast<std::uint64_t>(userIndexBytes));
}

void countBaseStateBind(std::uint32_t textureBinds,
                        std::uint32_t samplerBinds,
                        std::uint32_t vertexBufferBinds,
                        std::uint32_t indexBufferBinds,
                        std::uint32_t uniformBufferBinds,
                        std::uint32_t pipelineBinds,
                        std::uint32_t depthStateBinds,
                        std::uint32_t viewportBinds,
                        std::uint32_t scissorBinds,
                        std::uint32_t rasterizerBinds) {
  add(counters().bindTexture, textureBinds);
  add(counters().bindSampler, samplerBinds);
  add(counters().bindVertexBuffer, vertexBufferBinds);
  add(counters().bindIndexBuffer, indexBufferBinds);
  add(counters().bindUniformBuffer, uniformBufferBinds);
  add(counters().bindPipeline, pipelineBinds);
  add(counters().bindDepthState, depthStateBinds);
  add(counters().bindViewport, viewportBinds);
  add(counters().bindScissor, scissorBinds);
  add(counters().bindRasterizer, rasterizerBinds);
}

void countBaseStateBindSkip(std::uint32_t textureBinds,
                            std::uint32_t samplerBinds) {
  add(counters().bindTextureSkipped, textureBinds);
  add(counters().bindSamplerSkipped, samplerBinds);
}

void countBaseStateBindSkipExtended(std::uint32_t vertexBufferBinds,
                                    std::uint32_t indexBufferBinds,
                                    std::uint32_t pipelineBinds,
                                    std::uint32_t depthStateBinds,
                                    std::uint32_t viewportBinds,
                                    std::uint32_t scissorBinds,
                                    std::uint32_t rasterizerBinds) {
  add(counters().bindVertexBufferSkipped, vertexBufferBinds);
  add(counters().bindIndexBufferSkipped, indexBufferBinds);
  add(counters().bindPipelineSkipped, pipelineBinds);
  add(counters().bindDepthStateSkipped, depthStateBinds);
  add(counters().bindViewportSkipped, viewportBinds);
  add(counters().bindScissorSkipped, scissorBinds);
  add(counters().bindRasterizerSkipped, rasterizerBinds);
}

void countDrawShaderBucket(std::uint64_t vertexShaderHash,
                           std::uint64_t pixelShaderHash,
                           std::uint64_t variantHash) {
  if (!enabled()) {
    return;
  }
  auto& c = counters();
  add(c.drawShaderBucketSamples);
  const bool changed = load(c.lastVertexShaderHash) != vertexShaderHash ||
                       load(c.lastPixelShaderHash) != pixelShaderHash ||
                       load(c.lastShaderVariantHash) != variantHash;
  if (changed) {
    add(c.drawShaderBucketChanges);
  }
  store(c.lastVertexShaderHash, vertexShaderHash);
  store(c.lastPixelShaderHash, pixelShaderHash);
  store(c.lastShaderVariantHash, variantHash);
}

void countDrawGeometryDiagnostics(bool fixedFunctionPath,
                                  bool indexed,
                                  bool index32,
                                  bool direct,
                                  bool up,
                                  bool expanded,
                                  bool nonZeroBaseVertex,
                                  bool nonZeroStartIndex,
                                  bool nonZeroStream0Offset,
                                  std::uint32_t stream0Stride,
                                  std::uint64_t vertexDeclHash) {
  if (!enabled()) {
    return;
  }
  auto& c = counters();
  add(c.drawGeometrySamples);
  add(fixedFunctionPath ? c.drawGeometryFfp : c.drawGeometryVs);
  if (indexed) {
    add(c.drawGeometryIndexed);
    add(index32 ? c.drawGeometryIndex32 : c.drawGeometryIndex16);
  }
  if (direct) {
    add(c.drawGeometryDirect);
  }
  if (up) {
    add(c.drawGeometryUp);
  }
  if (expanded) {
    add(c.drawGeometryExpanded);
  }
  if (nonZeroBaseVertex) {
    add(c.drawGeometryNonZeroBaseVertex);
  }
  if (nonZeroStartIndex) {
    add(c.drawGeometryNonZeroStartIndex);
  }
  if (nonZeroStream0Offset) {
    add(c.drawGeometryNonZeroStream0Offset);
  }
  store(c.drawGeometryLastStream0Stride, stream0Stride);
  store(c.drawGeometryLastVertexDeclHash, vertexDeclHash);
}

void countSubmitDrawCpuTime(std::uint64_t nanoseconds) {
  add(counters().submitDrawCpuNs, nanoseconds);
  updateMax(counters().submitDrawCpuMaxNs, nanoseconds);
  recordRing(counters().submitDrawCpuRing, nanoseconds);
}

void countGpuCommandBufferTime(std::uint64_t nanoseconds) {
  add(counters().gpuCommandBufferTimeNs, nanoseconds);
  add(counters().gpuCommandBufferTimeSamples);
  updateMax(counters().gpuCommandBufferTimeMaxNs, nanoseconds);
  recordRing(counters().gpuCommandBufferTimeRing, nanoseconds);
}

void countRenderEncoderGpuTime(std::uint64_t nanoseconds,
                               std::uint32_t passType,
                               std::uint64_t rtHandle,
                               std::uint64_t depthHandle,
                               std::uint64_t psoHandle) {
  auto& c = counters();
  add(c.renderEncoderGpuTimeNs, nanoseconds);
  add(c.renderEncoderGpuTimeSamples);
  updateMax(c.renderEncoderGpuTimeMaxNs, nanoseconds);
  recordRing(c.renderEncoderGpuTimeRing, nanoseconds);
  switch (passType) {
    case 1:
      add(c.renderEncoderGpuDrawTimeNs, nanoseconds);
      add(c.renderEncoderGpuDrawSamples);
      break;
    case 2:
      add(c.renderEncoderGpuClearTimeNs, nanoseconds);
      add(c.renderEncoderGpuClearSamples);
      break;
    case 3:
      add(c.renderEncoderGpuSurfaceCopyTimeNs, nanoseconds);
      add(c.renderEncoderGpuSurfaceCopySamples);
      break;
    case 4:
      add(c.renderEncoderGpuStretchTimeNs, nanoseconds);
      add(c.renderEncoderGpuStretchSamples);
      break;
    case 5:
      add(c.renderEncoderGpuColorFillTimeNs, nanoseconds);
      add(c.renderEncoderGpuColorFillSamples);
      break;
    case 6:
      add(c.renderEncoderGpuDepthResolveTimeNs, nanoseconds);
      add(c.renderEncoderGpuDepthResolveSamples);
      break;
    case 7:
      add(c.renderEncoderGpuPresentTimeNs, nanoseconds);
      add(c.renderEncoderGpuPresentSamples);
      break;
    default:
      break;
  }
  c.renderEncoderGpuLastPassType.store(passType, std::memory_order_relaxed);
  c.renderEncoderGpuLastRt.store(rtHandle, std::memory_order_relaxed);
  c.renderEncoderGpuLastDepth.store(depthHandle, std::memory_order_relaxed);
  c.renderEncoderGpuLastPso.store(psoHandle, std::memory_order_relaxed);
}

void countEncodeChunkCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeChunkCalls);
  add(counters().encodeChunkCpuNs, nanoseconds);
  updateMax(counters().encodeChunkCpuMaxNs, nanoseconds);
  recordRing(counters().encodeChunkCpuRing, nanoseconds);
}

void countEncodeDrawCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawCpuNs, nanoseconds);
  updateMax(counters().encodeDrawCpuMaxNs, nanoseconds);
  recordRing(counters().encodeDrawCpuRing, nanoseconds);
}

void countEncodeDrawPipelineLookupCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawPipelineLookupCpuNs, nanoseconds);
  updateMax(counters().encodeDrawPipelineLookupCpuMaxNs, nanoseconds);
  recordRing(counters().encodeDrawPipelineLookupCpuRing, nanoseconds);
}

void countEncodeDrawUniformBuildCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawUniformBuildCpuNs, nanoseconds);
  updateMax(counters().encodeDrawUniformBuildCpuMaxNs, nanoseconds);
  recordRing(counters().encodeDrawUniformBuildCpuRing, nanoseconds);
}

void countEncodeDrawFvfDecodeCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawFvfDecodeCpuNs, nanoseconds);
  updateMax(counters().encodeDrawFvfDecodeCpuMaxNs, nanoseconds);
  recordRing(counters().encodeDrawFvfDecodeCpuRing, nanoseconds);
}

void countEncodeDrawBindingPacketCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawBindingPacketCpuNs, nanoseconds);
}

void countEncodeDrawBindingPacketPlanCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawBindingPacketPlanCpuNs, nanoseconds);
}

void countEncodeDrawBindingPacketPlanFragmentCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawBindingPacketPlanFragmentCpuNs, nanoseconds);
}

void countEncodeDrawBindingPacketPlanVertexCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawBindingPacketPlanVertexCpuNs, nanoseconds);
}

void countEncodeDrawBindingPacketPlanExtraStreamCpuTime(
    std::uint64_t nanoseconds) {
  add(counters().encodeDrawBindingPacketPlanExtraStreamCpuNs, nanoseconds);
}

void countEncodeDrawBindingPacketPlanRasterCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawBindingPacketPlanRasterCpuNs, nanoseconds);
}

void countEncodeDrawBindingPacketCacheCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawBindingPacketCacheCpuNs, nanoseconds);
}

void countEncodeDrawBindingPacketCacheKeyCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawBindingPacketCacheKeyCpuNs, nanoseconds);
}

void countEncodeDrawBindingPacketCacheHashCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawBindingPacketCacheHashCpuNs, nanoseconds);
}

void countEncodeDrawBindingPacketCacheProbeCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawBindingPacketCacheProbeCpuNs, nanoseconds);
}

void countEncodeDrawBindingPacketCacheStoreCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawBindingPacketCacheStoreCpuNs, nanoseconds);
}

void countEncodeDrawBindingPacketCacheHits(std::uint64_t hits) {
  add(counters().encodeDrawBindingPacketCacheHits, hits);
}

void countEncodeDrawBindingPacketCacheMisses(std::uint64_t misses) {
  add(counters().encodeDrawBindingPacketCacheMisses, misses);
}

void countEncodeDrawBindingPacketCacheCollisions(std::uint64_t collisions) {
  add(counters().encodeDrawBindingPacketCacheCollisions, collisions);
}

void countEncodeDrawBindingPacketTextureRecordCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawBindingPacketTextureRecordCpuNs, nanoseconds);
}

void countEncodeDrawArgbufSetupCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawArgbufSetupCpuNs, nanoseconds);
}

void countEncodeDrawArgbufOpenCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawArgbufOpenCpuNs, nanoseconds);
}

void countEncodeDrawArgbufOpenCallCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawArgbufOpenCallCpuNs, nanoseconds);
}

void countEncodeDrawArgbufReopenPostCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawArgbufReopenPostCpuNs, nanoseconds);
}

void countEncodeDrawArgbufReopenTableProbeCpuTime(
    std::uint64_t nanoseconds) {
  add(counters().encodeDrawArgbufReopenTableProbeCpuNs, nanoseconds);
}

void countEncodeDrawArgbufReopenTableShadowStoreCpuTime(
    std::uint64_t nanoseconds) {
  add(counters().encodeDrawArgbufReopenTableShadowStoreCpuNs, nanoseconds);
}

void countEncodeDrawArgbufReopenByteAccountCpuTime(
    std::uint64_t nanoseconds) {
  add(counters().encodeDrawArgbufReopenByteAccountCpuNs, nanoseconds);
}

void countEncodeDrawArgbufReopenCbufCacheProbeCpuTime(
    std::uint64_t nanoseconds) {
  add(counters().encodeDrawArgbufReopenCbufCacheProbeCpuNs, nanoseconds);
}

void countEncodeDrawArgbufReopenCbufDirtyScanCpuTime(
    std::uint64_t nanoseconds) {
  add(counters().encodeDrawArgbufReopenCbufDirtyScanCpuNs, nanoseconds);
}

void countEncodeDrawArgbufReopenCbufForceDirtyCpuTime(
    std::uint64_t nanoseconds) {
  add(counters().encodeDrawArgbufReopenCbufForceDirtyCpuNs, nanoseconds);
}

void countEncodeDrawArgbufOpenReserveCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawArgbufOpenReserveCpuNs, nanoseconds);
}

void countEncodeDrawArgbufOpenSetArgumentBufferCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawArgbufOpenSetArgumentBufferCpuNs, nanoseconds);
}

void countEncodeDrawArgbufTableBindCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawArgbufTableBindCpuNs, nanoseconds);
}

void countEncodeDrawArgbufTableBindCalls(std::uint64_t calls) {
  add(counters().encodeDrawArgbufTableBindCalls, calls);
}

void countEncodeDrawArgbufTableBindSkipped(std::uint64_t skips) {
  add(counters().encodeDrawArgbufTableBindSkipped, skips);
}

void countEncodeDrawArgbufCbufUpdateCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawArgbufCbufUpdateCpuNs, nanoseconds);
}

void countEncodeDrawArgbufCbufUpdateCalls(std::uint64_t calls) {
  add(counters().encodeDrawArgbufCbufUpdateCalls, calls);
}

void countEncodeDrawArgbufCbufUpdateDirtyCalls(std::uint64_t calls) {
  add(counters().encodeDrawArgbufCbufUpdateDirtyCalls, calls);
}

void countEncodeDrawArgbufCbufUpdateSkippedClean(std::uint64_t skips) {
  add(counters().encodeDrawArgbufCbufUpdateSkippedClean, skips);
}

void countEncodeDrawArgbufCbufUpdateWriteCalls(std::uint64_t calls) {
  add(counters().encodeDrawArgbufCbufUpdateWriteCalls, calls);
}

void countEncodeDrawArgbufCbufBuildCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawArgbufCbufBuildCpuNs, nanoseconds);
}

void countEncodeDrawArgbufCbufUploadCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawArgbufCbufUploadCpuNs, nanoseconds);
}

void countEncodeDrawArgbufCbufSetBufferCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawArgbufCbufSetBufferCpuNs, nanoseconds);
}

void countEncodeDrawArgbufCbufBuildVsCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawArgbufCbufBuildVsCpuNs, nanoseconds);
}

void countEncodeDrawArgbufCbufBuildPsCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawArgbufCbufBuildPsCpuNs, nanoseconds);
}

void countEncodeDrawArgbufCbufBuildFfpVsCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawArgbufCbufBuildFfpVsCpuNs, nanoseconds);
}

void countEncodeDrawArgbufCbufBuildFfpPsCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawArgbufCbufBuildFfpPsCpuNs, nanoseconds);
}

void countEncodeDrawArgbufCbufUploadVsCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawArgbufCbufUploadVsCpuNs, nanoseconds);
}

void countEncodeDrawArgbufCbufUploadPsCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawArgbufCbufUploadPsCpuNs, nanoseconds);
}

void countEncodeDrawArgbufCbufUploadFfpVsCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawArgbufCbufUploadFfpVsCpuNs, nanoseconds);
}

void countEncodeDrawArgbufCbufUploadFfpPsCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawArgbufCbufUploadFfpPsCpuNs, nanoseconds);
}

void countEncodeDrawArgbufCbufSetBufferVsCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawArgbufCbufSetBufferVsCpuNs, nanoseconds);
}

void countEncodeDrawArgbufCbufSetBufferPsCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawArgbufCbufSetBufferPsCpuNs, nanoseconds);
}

void countEncodeDrawArgbufCbufSetBufferFfpVsCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawArgbufCbufSetBufferFfpVsCpuNs, nanoseconds);
}

void countEncodeDrawArgbufCbufSetBufferFfpPsCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawArgbufCbufSetBufferFfpPsCpuNs, nanoseconds);
}

void countEncodeDrawArgbufCbufUploadPlanCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawArgbufCbufUploadPlanCpuNs, nanoseconds);
}

void countEncodeDrawArgbufCbufUploadPlanVsCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawArgbufCbufUploadPlanVsCpuNs, nanoseconds);
}

void countEncodeDrawArgbufCbufUploadPlanPsCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawArgbufCbufUploadPlanPsCpuNs, nanoseconds);
}

void countEncodeDrawArgbufCbufBindingHashCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawArgbufCbufBindingHashCpuNs, nanoseconds);
}

void countEncodeDrawArgbufCbufBindingHashVsCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawArgbufCbufBindingHashVsCpuNs, nanoseconds);
}

void countEncodeDrawArgbufCbufBindingHashPsCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawArgbufCbufBindingHashPsCpuNs, nanoseconds);
}

void countEncodeDrawArgbufCbufBindingHashFfpVsCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawArgbufCbufBindingHashFfpVsCpuNs, nanoseconds);
}

void countEncodeDrawArgbufCbufBindingHashFfpPsCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawArgbufCbufBindingHashFfpPsCpuNs, nanoseconds);
}

void countEncodeDrawArgbufCbufBindingWriteCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawArgbufCbufBindingWriteCpuNs, nanoseconds);
}

void countEncodeDrawArgbufCbufBindingWriteVsCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawArgbufCbufBindingWriteVsCpuNs, nanoseconds);
}

void countEncodeDrawArgbufCbufBindingWritePsCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawArgbufCbufBindingWritePsCpuNs, nanoseconds);
}

void countEncodeDrawArgbufCbufBindingWriteFfpVsCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawArgbufCbufBindingWriteFfpVsCpuNs, nanoseconds);
}

void countEncodeDrawArgbufCbufBindingWriteFfpPsCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawArgbufCbufBindingWriteFfpPsCpuNs, nanoseconds);
}

void countEncodeDrawArgbufCbufObserverCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawArgbufCbufObserverCpuNs, nanoseconds);
}

void countEncodeDrawArgbufCbufObserverVsCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawArgbufCbufObserverVsCpuNs, nanoseconds);
}

void countEncodeDrawArgbufCbufObserverPsCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawArgbufCbufObserverPsCpuNs, nanoseconds);
}

void countEncodeDrawArgbufCbufObserverFfpVsCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawArgbufCbufObserverFfpVsCpuNs, nanoseconds);
}

void countEncodeDrawArgbufCbufObserverFfpPsCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawArgbufCbufObserverFfpPsCpuNs, nanoseconds);
}

void countEncodeDrawArgbufCbufCacheMergeCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawArgbufCbufCacheMergeCpuNs, nanoseconds);
}

void countEncodeDrawArgbufCbufCachedRepointCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawArgbufCbufCachedRepointCpuNs, nanoseconds);
}

void countEncodeDrawArgbufCbufFullRepointCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawArgbufCbufFullRepointCpuNs, nanoseconds);
}

void countEncodeDrawArgbufCbufCachedRepointVsCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawArgbufCbufCachedRepointVsCpuNs, nanoseconds);
}

void countEncodeDrawArgbufCbufCachedRepointPsCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawArgbufCbufCachedRepointPsCpuNs, nanoseconds);
}

void countEncodeDrawArgbufCbufCachedRepointFfpVsCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawArgbufCbufCachedRepointFfpVsCpuNs, nanoseconds);
}

void countEncodeDrawArgbufCbufCachedRepointFfpPsCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawArgbufCbufCachedRepointFfpPsCpuNs, nanoseconds);
}

void countEncodeDrawArgbufCbufCachedRepointCalls(std::uint64_t calls) {
  add(counters().encodeDrawArgbufCbufCachedRepointCalls, calls);
}

void countEncodeDrawArgbufCbufCachedRepointBytes(std::uint64_t bytes) {
  add(counters().encodeDrawArgbufCbufCachedRepointBytes, bytes);
}

void countEncodeDrawArgbufCbufCachedRepointVsCalls(std::uint64_t calls) {
  add(counters().encodeDrawArgbufCbufCachedRepointVsCalls, calls);
}

void countEncodeDrawArgbufCbufCachedRepointPsCalls(std::uint64_t calls) {
  add(counters().encodeDrawArgbufCbufCachedRepointPsCalls, calls);
}

void countEncodeDrawArgbufCbufCachedRepointFfpVsCalls(std::uint64_t calls) {
  add(counters().encodeDrawArgbufCbufCachedRepointFfpVsCalls, calls);
}

void countEncodeDrawArgbufCbufCachedRepointFfpPsCalls(std::uint64_t calls) {
  add(counters().encodeDrawArgbufCbufCachedRepointFfpPsCalls, calls);
}

void countEncodeDrawArgbufCbufCachedRepointVsBytes(std::uint64_t bytes) {
  add(counters().encodeDrawArgbufCbufCachedRepointVsBytes, bytes);
}

void countEncodeDrawArgbufCbufCachedRepointPsBytes(std::uint64_t bytes) {
  add(counters().encodeDrawArgbufCbufCachedRepointPsBytes, bytes);
}

void countEncodeDrawArgbufCbufCachedRepointFfpVsBytes(std::uint64_t bytes) {
  add(counters().encodeDrawArgbufCbufCachedRepointFfpVsBytes, bytes);
}

void countEncodeDrawArgbufCbufCachedRepointFfpPsBytes(std::uint64_t bytes) {
  add(counters().encodeDrawArgbufCbufCachedRepointFfpPsBytes, bytes);
}

void countEncodeDrawArgbufCbufContentProbeCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawArgbufCbufContentProbeCpuNs, nanoseconds);
}

void countEncodeDrawArgbufCbufContentProbeVsCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawArgbufCbufContentProbeVsCpuNs, nanoseconds);
}

void countEncodeDrawArgbufCbufContentProbePsCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawArgbufCbufContentProbePsCpuNs, nanoseconds);
}

void countEncodeDrawArgbufCbufContentProbeFfpPsCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawArgbufCbufContentProbeFfpPsCpuNs, nanoseconds);
}

void countEncodeDrawArgbufCbufContentProbeCalls(std::uint64_t calls) {
  add(counters().encodeDrawArgbufCbufContentProbeCalls, calls);
}

void countEncodeDrawArgbufCbufContentProbeVsHits(std::uint64_t hits) {
  add(counters().encodeDrawArgbufCbufContentProbeVsHits, hits);
}

void countEncodeDrawArgbufCbufContentProbeVsMisses(std::uint64_t misses) {
  add(counters().encodeDrawArgbufCbufContentProbeVsMisses, misses);
}

void countEncodeDrawArgbufCbufContentProbePsHits(std::uint64_t hits) {
  add(counters().encodeDrawArgbufCbufContentProbePsHits, hits);
}

void countEncodeDrawArgbufCbufContentProbePsMisses(std::uint64_t misses) {
  add(counters().encodeDrawArgbufCbufContentProbePsMisses, misses);
}

void countEncodeDrawArgbufCbufContentProbeFfpPsHits(std::uint64_t hits) {
  add(counters().encodeDrawArgbufCbufContentProbeFfpPsHits, hits);
}

void countEncodeDrawArgbufCbufContentProbeFfpPsMisses(std::uint64_t misses) {
  add(counters().encodeDrawArgbufCbufContentProbeFfpPsMisses, misses);
}

void countEncodeDrawArgbufCbufReopenFullRepointCalls(std::uint64_t calls) {
  add(counters().encodeDrawArgbufCbufReopenFullRepointCalls, calls);
}

void countEncodeDrawArgbufCbufReopenNoDirtyHashMismatch(std::uint64_t calls) {
  add(counters().encodeDrawArgbufCbufReopenNoDirtyHashMismatch, calls);
}

void countEncodeDrawArgbufCbufReopenPartialCandidates(std::uint64_t calls) {
  add(counters().encodeDrawArgbufCbufReopenPartialCandidates, calls);
}

void countEncodeDrawArgbufCbufReopenDirtyVs(std::uint64_t calls) {
  add(counters().encodeDrawArgbufCbufReopenDirtyVs, calls);
}

void countEncodeDrawArgbufCbufReopenDirtyPs(std::uint64_t calls) {
  add(counters().encodeDrawArgbufCbufReopenDirtyPs, calls);
}

void countEncodeDrawArgbufCbufReopenDirtyFfpVs(std::uint64_t calls) {
  add(counters().encodeDrawArgbufCbufReopenDirtyFfpVs, calls);
}

void countEncodeDrawArgbufCbufReopenDirtyFfpPs(std::uint64_t calls) {
  add(counters().encodeDrawArgbufCbufReopenDirtyFfpPs, calls);
}

void countEncodeDrawArgbufCbufUpdateVsCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawArgbufCbufUpdateVsCpuNs, nanoseconds);
}

void countEncodeDrawArgbufCbufUpdatePsCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawArgbufCbufUpdatePsCpuNs, nanoseconds);
}

void countEncodeDrawArgbufCbufUpdateFfpVsCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawArgbufCbufUpdateFfpVsCpuNs, nanoseconds);
}

void countEncodeDrawArgbufCbufUpdateFfpPsCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawArgbufCbufUpdateFfpPsCpuNs, nanoseconds);
}

void countEncodeDrawArgbufCbufUpdateVsCalls(std::uint64_t calls) {
  add(counters().encodeDrawArgbufCbufUpdateVsCalls, calls);
}

void countEncodeDrawArgbufCbufUpdatePsCalls(std::uint64_t calls) {
  add(counters().encodeDrawArgbufCbufUpdatePsCalls, calls);
}

void countEncodeDrawArgbufCbufUpdateFfpVsCalls(std::uint64_t calls) {
  add(counters().encodeDrawArgbufCbufUpdateFfpVsCalls, calls);
}

void countEncodeDrawArgbufCbufUpdateFfpPsCalls(std::uint64_t calls) {
  add(counters().encodeDrawArgbufCbufUpdateFfpPsCalls, calls);
}

void countEncodeDrawArgbufCbufUpdateVsBytes(std::uint64_t bytes) {
  add(counters().encodeDrawArgbufCbufUpdateVsBytes, bytes);
}

void countEncodeDrawArgbufCbufUpdatePsBytes(std::uint64_t bytes) {
  add(counters().encodeDrawArgbufCbufUpdatePsBytes, bytes);
}

void countEncodeDrawArgbufCbufUpdateFfpVsBytes(std::uint64_t bytes) {
  add(counters().encodeDrawArgbufCbufUpdateFfpVsBytes, bytes);
}

void countEncodeDrawArgbufCbufUpdateFfpPsBytes(std::uint64_t bytes) {
  add(counters().encodeDrawArgbufCbufUpdateFfpPsBytes, bytes);
}

void countEncodeDrawArgbufCbufDirtyVsIdentityProbeCalls(std::uint64_t calls) {
  add(counters().encodeDrawArgbufCbufDirtyVsIdentityProbeCalls, calls);
}

void countEncodeDrawArgbufCbufDirtyVsIdentityHits(std::uint64_t hits) {
  add(counters().encodeDrawArgbufCbufDirtyVsIdentityHits, hits);
}

void countEncodeDrawArgbufCbufDirtyVsIdentityMisses(std::uint64_t misses) {
  add(counters().encodeDrawArgbufCbufDirtyVsIdentityMisses, misses);
}

void countEncodeDrawArgbufCbufDirtyVsIdentityNoCache(std::uint64_t calls) {
  add(counters().encodeDrawArgbufCbufDirtyVsIdentityNoCache, calls);
}

void countEncodeDrawArgbufCbufDirtyVsIdentityHitBytes(std::uint64_t bytes) {
  add(counters().encodeDrawArgbufCbufDirtyVsIdentityHitBytes, bytes);
}

void countEncodeDrawArgbufCbufDirtyVsIdentityMissBytes(std::uint64_t bytes) {
  add(counters().encodeDrawArgbufCbufDirtyVsIdentityMissBytes, bytes);
}

void countEncodeDrawArgbufPayloadDeltaProbeCalls(std::uint64_t calls) {
  add(counters().encodeDrawArgbufPayloadDeltaProbeCalls, calls);
}

void countEncodeDrawArgbufPayloadDeltaFirst(std::uint64_t calls) {
  add(counters().encodeDrawArgbufPayloadDeltaFirst, calls);
}

void countEncodeDrawArgbufPayloadDeltaSame(std::uint64_t calls) {
  add(counters().encodeDrawArgbufPayloadDeltaSame, calls);
}

void countEncodeDrawArgbufPayloadDeltaChanged(std::uint64_t calls) {
  add(counters().encodeDrawArgbufPayloadDeltaChanged, calls);
}

void countEncodeDrawArgbufPayloadDeltaChangedVs(std::uint64_t calls) {
  add(counters().encodeDrawArgbufPayloadDeltaChangedVs, calls);
}

void countEncodeDrawArgbufPayloadDeltaChangedPs(std::uint64_t calls) {
  add(counters().encodeDrawArgbufPayloadDeltaChangedPs, calls);
}

void countEncodeDrawArgbufPayloadDeltaChangedVsPs(std::uint64_t calls) {
  add(counters().encodeDrawArgbufPayloadDeltaChangedVsPs, calls);
}

void countEncodeDrawArgbufPayloadDeltaChangedNonConstOnly(std::uint64_t calls) {
  add(counters().encodeDrawArgbufPayloadDeltaChangedNonConstOnly, calls);
}

void countEncodeDrawArgbufPayloadDeltaChangedVsFloat(std::uint64_t calls) {
  add(counters().encodeDrawArgbufPayloadDeltaChangedVsFloat, calls);
}

void countEncodeDrawArgbufPayloadDeltaChangedVsInt(std::uint64_t calls) {
  add(counters().encodeDrawArgbufPayloadDeltaChangedVsInt, calls);
}

void countEncodeDrawArgbufPayloadDeltaChangedVsBool(std::uint64_t calls) {
  add(counters().encodeDrawArgbufPayloadDeltaChangedVsBool, calls);
}

void countEncodeDrawArgbufPayloadDeltaChangedPsFloat(std::uint64_t calls) {
  add(counters().encodeDrawArgbufPayloadDeltaChangedPsFloat, calls);
}

void countEncodeDrawArgbufPayloadDeltaChangedPsInt(std::uint64_t calls) {
  add(counters().encodeDrawArgbufPayloadDeltaChangedPsInt, calls);
}

void countEncodeDrawArgbufPayloadDeltaChangedPsBool(std::uint64_t calls) {
  add(counters().encodeDrawArgbufPayloadDeltaChangedPsBool, calls);
}

void countEncodeDrawArgbufPayloadDeltaChangedVsFloatRegs(std::uint64_t regs) {
  add(counters().encodeDrawArgbufPayloadDeltaChangedVsFloatRegs, regs);
}

void countEncodeDrawArgbufPayloadDeltaChangedVsFloatRegsMax(std::uint64_t regs) {
  updateMax(counters().encodeDrawArgbufPayloadDeltaChangedVsFloatRegsMax, regs);
}

void countEncodeDrawArgbufPayloadDeltaChangedVsFloatRegsLe1(std::uint64_t calls) {
  add(counters().encodeDrawArgbufPayloadDeltaChangedVsFloatRegsLe1, calls);
}

void countEncodeDrawArgbufPayloadDeltaChangedVsFloatRegsLe4(std::uint64_t calls) {
  add(counters().encodeDrawArgbufPayloadDeltaChangedVsFloatRegsLe4, calls);
}

void countEncodeDrawArgbufPayloadDeltaChangedVsFloatRegsLe16(std::uint64_t calls) {
  add(counters().encodeDrawArgbufPayloadDeltaChangedVsFloatRegsLe16, calls);
}

void countEncodeDrawArgbufPayloadDeltaChangedVsFloatRegsLe64(std::uint64_t calls) {
  add(counters().encodeDrawArgbufPayloadDeltaChangedVsFloatRegsLe64, calls);
}

void countEncodeDrawArgbufPayloadDeltaChangedVsFloatRegsGt64(std::uint64_t calls) {
  add(counters().encodeDrawArgbufPayloadDeltaChangedVsFloatRegsGt64, calls);
}

void countEncodeDrawArgbufPayloadDeltaChangedVsFloatRegsLe1Sum(std::uint64_t regs) {
  add(counters().encodeDrawArgbufPayloadDeltaChangedVsFloatRegsLe1Sum, regs);
}

void countEncodeDrawArgbufPayloadDeltaChangedVsFloatRegsLe4Sum(std::uint64_t regs) {
  add(counters().encodeDrawArgbufPayloadDeltaChangedVsFloatRegsLe4Sum, regs);
}

void countEncodeDrawArgbufPayloadDeltaChangedVsFloatRegsLe16Sum(std::uint64_t regs) {
  add(counters().encodeDrawArgbufPayloadDeltaChangedVsFloatRegsLe16Sum, regs);
}

void countEncodeDrawArgbufPayloadDeltaChangedVsFloatRegsLe64Sum(std::uint64_t regs) {
  add(counters().encodeDrawArgbufPayloadDeltaChangedVsFloatRegsLe64Sum, regs);
}

void countEncodeDrawArgbufPayloadDeltaChangedVsFloatRegsGt64Sum(std::uint64_t regs) {
  add(counters().encodeDrawArgbufPayloadDeltaChangedVsFloatRegsGt64Sum, regs);
}

void countEncodeDrawArgbufPayloadDeltaChangedVsFloatPrefixRegs(std::uint64_t regs) {
  add(counters().encodeDrawArgbufPayloadDeltaChangedVsFloatPrefixRegs, regs);
}

void countEncodeDrawArgbufPayloadDeltaChangedVsFloatPrefixRegsMax(std::uint64_t regs) {
  updateMax(counters().encodeDrawArgbufPayloadDeltaChangedVsFloatPrefixRegsMax, regs);
}

void countEncodeDrawArgbufPayloadDeltaChangedVsFloatSpanRegs(std::uint64_t regs) {
  add(counters().encodeDrawArgbufPayloadDeltaChangedVsFloatSpanRegs, regs);
}

void countEncodeDrawArgbufPayloadDeltaChangedVsFloatSpanRegsMax(std::uint64_t regs) {
  updateMax(counters().encodeDrawArgbufPayloadDeltaChangedVsFloatSpanRegsMax, regs);
}

void countEncodeDrawArgbufPayloadDeltaChangedVsFloatFullPrefix(std::uint64_t calls) {
  add(counters().encodeDrawArgbufPayloadDeltaChangedVsFloatFullPrefix, calls);
}

void countEncodeDrawArgbufPayloadDeltaChangedVsFloatFullPrefixRegs(std::uint64_t regs) {
  add(counters().encodeDrawArgbufPayloadDeltaChangedVsFloatFullPrefixRegs, regs);
}

void countEncodeDrawArgbufPayloadDeltaChangedPsFloatRegs(std::uint64_t regs) {
  add(counters().encodeDrawArgbufPayloadDeltaChangedPsFloatRegs, regs);
}

void countEncodeDrawArgbufPayloadDeltaChangedPsFloatRegsMax(std::uint64_t regs) {
  updateMax(counters().encodeDrawArgbufPayloadDeltaChangedPsFloatRegsMax, regs);
}

void countEncodeDrawArgbufPayloadDeltaChangedPsFloatRegsLe1(std::uint64_t calls) {
  add(counters().encodeDrawArgbufPayloadDeltaChangedPsFloatRegsLe1, calls);
}

void countEncodeDrawArgbufPayloadDeltaChangedPsFloatRegsLe4(std::uint64_t calls) {
  add(counters().encodeDrawArgbufPayloadDeltaChangedPsFloatRegsLe4, calls);
}

void countEncodeDrawArgbufPayloadDeltaChangedPsFloatRegsLe16(std::uint64_t calls) {
  add(counters().encodeDrawArgbufPayloadDeltaChangedPsFloatRegsLe16, calls);
}

void countEncodeDrawArgbufPayloadDeltaChangedPsFloatRegsLe64(std::uint64_t calls) {
  add(counters().encodeDrawArgbufPayloadDeltaChangedPsFloatRegsLe64, calls);
}

void countEncodeDrawArgbufPayloadDeltaChangedPsFloatRegsGt64(std::uint64_t calls) {
  add(counters().encodeDrawArgbufPayloadDeltaChangedPsFloatRegsGt64, calls);
}

void countEncodeDrawArgbufPayloadDeltaChangedPsFloatRegsLe1Sum(std::uint64_t regs) {
  add(counters().encodeDrawArgbufPayloadDeltaChangedPsFloatRegsLe1Sum, regs);
}

void countEncodeDrawArgbufPayloadDeltaChangedPsFloatRegsLe4Sum(std::uint64_t regs) {
  add(counters().encodeDrawArgbufPayloadDeltaChangedPsFloatRegsLe4Sum, regs);
}

void countEncodeDrawArgbufPayloadDeltaChangedPsFloatRegsLe16Sum(std::uint64_t regs) {
  add(counters().encodeDrawArgbufPayloadDeltaChangedPsFloatRegsLe16Sum, regs);
}

void countEncodeDrawArgbufPayloadDeltaChangedPsFloatRegsLe64Sum(std::uint64_t regs) {
  add(counters().encodeDrawArgbufPayloadDeltaChangedPsFloatRegsLe64Sum, regs);
}

void countEncodeDrawArgbufPayloadDeltaChangedPsFloatRegsGt64Sum(std::uint64_t regs) {
  add(counters().encodeDrawArgbufPayloadDeltaChangedPsFloatRegsGt64Sum, regs);
}

void countEncodeDrawArgbufPayloadDeltaReopenFirst(std::uint64_t calls) {
  add(counters().encodeDrawArgbufPayloadDeltaReopenFirst, calls);
}

void countEncodeDrawArgbufPayloadDeltaReopenPayloadChanged(std::uint64_t calls) {
  add(counters().encodeDrawArgbufPayloadDeltaReopenPayloadChanged, calls);
}

void countEncodeDrawArgbufPayloadDeltaReopenPayloadSame(std::uint64_t calls) {
  add(counters().encodeDrawArgbufPayloadDeltaReopenPayloadSame, calls);
}

void countEncodeDrawArgbufPayloadDeltaReopenResourceArray(std::uint64_t calls) {
  add(counters().encodeDrawArgbufPayloadDeltaReopenResourceArray, calls);
}

void countEncodeDrawArgbufPayloadDeltaReopenCbufOnly(std::uint64_t calls) {
  add(counters().encodeDrawArgbufPayloadDeltaReopenCbufOnly, calls);
}

void countEncodeDrawArgbufPayloadDeltaReopenCbufOnlyFirst(
    std::uint64_t calls) {
  add(counters().encodeDrawArgbufPayloadDeltaReopenCbufOnlyFirst, calls);
}

void countEncodeDrawArgbufPayloadDeltaReopenCbufOnlyPayloadChanged(
    std::uint64_t calls) {
  add(counters().encodeDrawArgbufPayloadDeltaReopenCbufOnlyPayloadChanged,
      calls);
}

void countEncodeDrawStreamBindCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawStreamBindCpuNs, nanoseconds);
  updateMax(counters().encodeDrawStreamBindCpuMaxNs, nanoseconds);
  recordRing(counters().encodeDrawStreamBindCpuRing, nanoseconds);
}

void countEncodeDrawStreamBindRasterPhaseCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawStreamBindRasterPhaseCpuNs, nanoseconds);
}

void countEncodeDrawStreamBindRasterPhaseCalls(std::uint64_t calls) {
  add(counters().encodeDrawStreamBindRasterPhaseCalls, calls);
}

void countEncodeDrawStreamBindFfpStreamCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawStreamBindFfpStreamCpuNs, nanoseconds);
}

void countEncodeDrawStreamBindFfpStreamCalls(std::uint64_t calls) {
  add(counters().encodeDrawStreamBindFfpStreamCalls, calls);
}

void countEncodeDrawStreamBindShaderStreamCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawStreamBindShaderStreamCpuNs, nanoseconds);
}

void countEncodeDrawStreamBindShaderStreamCalls(std::uint64_t calls) {
  add(counters().encodeDrawStreamBindShaderStreamCalls, calls);
}

void countEncodeDrawStreamBindTexturePhaseCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawStreamBindTexturePhaseCpuNs, nanoseconds);
}

void countEncodeDrawStreamBindTexturePhaseCalls(std::uint64_t calls) {
  add(counters().encodeDrawStreamBindTexturePhaseCalls, calls);
}

void countEncodeDrawStreamBindIndexPhaseCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawStreamBindIndexPhaseCpuNs, nanoseconds);
}

void countEncodeDrawStreamBindIndexPhaseCalls(std::uint64_t calls) {
  add(counters().encodeDrawStreamBindIndexPhaseCalls, calls);
}

void countEncodeDrawTextureSamplerFragmentResolveCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawTextureSamplerFragmentResolveCpuNs, nanoseconds);
}

void countEncodeDrawTextureSamplerFragmentResolveCalls(std::uint64_t calls) {
  add(counters().encodeDrawTextureSamplerFragmentResolveCalls, calls);
}

void countEncodeDrawTextureSamplerFragmentResolveTextureCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawTextureSamplerFragmentResolveTextureCpuNs, nanoseconds);
}

void countEncodeDrawTextureSamplerFragmentResolveTextureCalls(std::uint64_t calls) {
  add(counters().encodeDrawTextureSamplerFragmentResolveTextureCalls, calls);
}

void countEncodeDrawTextureSamplerFragmentResourceArrayCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawTextureSamplerFragmentResourceArrayCpuNs, nanoseconds);
}

void countEncodeDrawTextureSamplerFragmentResourceArrayCalls(std::uint64_t calls) {
  add(counters().encodeDrawTextureSamplerFragmentResourceArrayCalls, calls);
}

void countEncodeDrawTextureSamplerFragmentDirectCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawTextureSamplerFragmentDirectCpuNs, nanoseconds);
}

void countEncodeDrawTextureSamplerFragmentDirectCalls(std::uint64_t calls) {
  add(counters().encodeDrawTextureSamplerFragmentDirectCalls, calls);
}

void countEncodeDrawTextureSamplerFragmentDirectTextureCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawTextureSamplerFragmentDirectTextureCpuNs, nanoseconds);
}

void countEncodeDrawTextureSamplerFragmentDirectTextureCalls(std::uint64_t calls) {
  add(counters().encodeDrawTextureSamplerFragmentDirectTextureCalls, calls);
}

void countEncodeDrawTextureSamplerFragmentDirectTextureSetCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawTextureSamplerFragmentDirectTextureSetCpuNs, nanoseconds);
}

void countEncodeDrawTextureSamplerFragmentDirectTextureSetCalls(std::uint64_t calls) {
  add(counters().encodeDrawTextureSamplerFragmentDirectTextureSetCalls, calls);
}

void countEncodeDrawTextureSamplerFragmentDirectSamplerCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawTextureSamplerFragmentDirectSamplerCpuNs, nanoseconds);
}

void countEncodeDrawTextureSamplerFragmentDirectSamplerCalls(std::uint64_t calls) {
  add(counters().encodeDrawTextureSamplerFragmentDirectSamplerCalls, calls);
}

void countEncodeDrawTextureSamplerFragmentDirectSamplerSetCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawTextureSamplerFragmentDirectSamplerSetCpuNs, nanoseconds);
}

void countEncodeDrawTextureSamplerFragmentDirectSamplerSetCalls(std::uint64_t calls) {
  add(counters().encodeDrawTextureSamplerFragmentDirectSamplerSetCalls, calls);
}

void countEncodeDrawTextureSamplerSamplerLookupCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawTextureSamplerSamplerLookupCpuNs, nanoseconds);
}

void countEncodeDrawTextureSamplerSamplerLookupCalls(std::uint64_t calls) {
  add(counters().encodeDrawTextureSamplerSamplerLookupCalls, calls);
}

void countEncodeDrawTextureSamplerSamplerLookupSkippedPrehandle(std::uint64_t skips) {
  add(counters().encodeDrawTextureSamplerSamplerLookupSkippedPrehandle, skips);
}

void countEncodeDrawTextureSamplerLodBiasCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawTextureSamplerLodBiasCpuNs, nanoseconds);
}

void countEncodeDrawTextureSamplerLodBiasCalls(std::uint64_t calls) {
  add(counters().encodeDrawTextureSamplerLodBiasCalls, calls);
}

void countEncodeDrawTextureSamplerVertexResolveCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawTextureSamplerVertexResolveCpuNs, nanoseconds);
}

void countEncodeDrawTextureSamplerVertexResolveCalls(std::uint64_t calls) {
  add(counters().encodeDrawTextureSamplerVertexResolveCalls, calls);
}

void countEncodeDrawTextureSamplerVertexDirectCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawTextureSamplerVertexDirectCpuNs, nanoseconds);
}

void countEncodeDrawTextureSamplerVertexDirectCalls(std::uint64_t calls) {
  add(counters().encodeDrawTextureSamplerVertexDirectCalls, calls);
}

void countEncodeDrawRasterStateCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawRasterStateCpuNs, nanoseconds);
}

void countEncodeDrawVertexStreamBindCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawVertexStreamBindCpuNs, nanoseconds);
}

void countEncodeDrawTextureSamplerBindCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawTextureSamplerBindCpuNs, nanoseconds);
}

void countEncodeDrawIndexSetupCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawIndexSetupCpuNs, nanoseconds);
}

void countEncodeDrawIndexSourceResolveCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawIndexSourceResolveCpuNs, nanoseconds);
}

void countEncodeDrawIndexCacheLookupCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawIndexCacheLookupCpuNs, nanoseconds);
}

void countEncodeDrawIndexCacheCandidateCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawIndexCacheCandidateCpuNs, nanoseconds);
}

void countEncodeDrawIndexCacheOriginalMeasureCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawIndexCacheOriginalMeasureCpuNs, nanoseconds);
}

void countEncodeDrawIndexCacheCandidateBuildCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawIndexCacheCandidateBuildCpuNs, nanoseconds);
}

void countEncodeDrawIndexCacheCandidateReadCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawIndexCacheCandidateReadCpuNs, nanoseconds);
}

void countEncodeDrawIndexCacheCandidateAdjacencyCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawIndexCacheCandidateAdjacencyCpuNs, nanoseconds);
}

void countEncodeDrawIndexCacheCandidateSelectCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawIndexCacheCandidateSelectCpuNs, nanoseconds);
}

void countEncodeDrawIndexCacheCandidateWriteCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawIndexCacheCandidateWriteCpuNs, nanoseconds);
}

void countEncodeDrawIndexCacheCandidateSelectVolume(std::uint64_t calls,
                                                    std::uint64_t slots,
                                                    std::uint64_t scored,
                                                    std::uint64_t skipped,
                                                    std::uint64_t maxCandidates) {
  auto& c = counters();
  add(c.encodeDrawIndexCacheCandidateSelectCalls, calls);
  add(c.encodeDrawIndexCacheCandidateSelectSlots, slots);
  add(c.encodeDrawIndexCacheCandidateSelectScored, scored);
  add(c.encodeDrawIndexCacheCandidateSelectSkipped, skipped);
  updateMax(c.encodeDrawIndexCacheCandidateSelectCandidatesMax, maxCandidates);
}

void countEncodeDrawIndexCacheCandidateFrontierDropped(std::uint64_t dropped) {
  add(counters().encodeDrawIndexCacheCandidateFrontierDropped, dropped);
}

void countEncodeDrawIndexCacheCandidateLazyFrontier(std::uint64_t heapPops,
                                                    std::uint64_t refreshes,
                                                    std::uint64_t staleDrops,
                                                    std::uint64_t accepted) {
  auto& c = counters();
  add(c.encodeDrawIndexCacheCandidateLazyHeapPops, heapPops);
  add(c.encodeDrawIndexCacheCandidateLazyRefreshes, refreshes);
  add(c.encodeDrawIndexCacheCandidateLazyStaleDrops, staleDrops);
  add(c.encodeDrawIndexCacheCandidateLazyAccepted, accepted);
}

void countEncodeDrawIndexCacheCandidateBucketedSelect(
    std::uint64_t vertexVisits,
    std::uint64_t bucketMoves,
    std::uint64_t selected) {
  auto& c = counters();
  add(c.encodeDrawIndexCacheCandidateBucketVertexVisits, vertexVisits);
  add(c.encodeDrawIndexCacheCandidateBucketMoves, bucketMoves);
  add(c.encodeDrawIndexCacheCandidateBucketSelected, selected);
}

void countEncodeDrawIndexCacheCandidateUpperBoundRejected(std::uint64_t rejected) {
  add(counters().encodeDrawIndexCacheCandidateUpperBoundRejected, rejected);
}

void countEncodeDrawIndexCacheCandidateMeasureCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawIndexCacheCandidateMeasureCpuNs, nanoseconds);
}

void countEncodeDrawIndexCacheGateCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawIndexCacheGateCpuNs, nanoseconds);
}

void countEncodeDrawIndexCacheApplyCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawIndexCacheApplyCpuNs, nanoseconds);
}

void countCompatibleIndexedDrawMergeTelemetry(
    std::uint64_t pairAttempts,
    std::uint64_t compatiblePairs,
    std::uint64_t multipleRejectPairs,
    std::uint64_t selectedPairs,
    const std::array<std::uint64_t,
                     kCompatibleIndexedDrawMergeRejectCount>& rejectPairs,
    const std::array<std::uint64_t,
                     kCompatibleIndexedDrawMergeRejectCount>& onlyRejectPairs,
    const std::array<std::uint64_t,
                     kCompatibleIndexedDrawMergeRelaxationSetCount>&
        exactRelaxationSetPairs,
    std::uint64_t otherRelaxationSetPairs) {
  auto& c = counters();
  add(c.compatibleIndexedDrawMergePairAttempts, pairAttempts);
  add(c.compatibleIndexedDrawMergeCompatiblePairs, compatiblePairs);
  add(c.compatibleIndexedDrawMergeMultipleRejectPairs, multipleRejectPairs);
  add(c.compatibleIndexedDrawMergeSelectedPairs, selectedPairs);

  std::atomic<std::uint64_t>* rejectCounters[] = {
      &c.compatibleIndexedDrawMergeRejectSourceShape,
      &c.compatibleIndexedDrawMergeRejectNextShape,
      &c.compatibleIndexedDrawMergeRejectIndexType,
      &c.compatibleIndexedDrawMergeRejectBaseVertex,
      &c.compatibleIndexedDrawMergeRejectStartVertex,
      &c.compatibleIndexedDrawMergeRejectUniform,
      &c.compatibleIndexedDrawMergeRejectBindingOverride,
      &c.compatibleIndexedDrawMergeRejectBindingSnapshot,
      &c.compatibleIndexedDrawMergeRejectNonContiguous,
      &c.compatibleIndexedDrawMergeRejectOverflow,
  };
  std::atomic<std::uint64_t>* onlyRejectCounters[] = {
      &c.compatibleIndexedDrawMergeOnlySourceShape,
      &c.compatibleIndexedDrawMergeOnlyNextShape,
      &c.compatibleIndexedDrawMergeOnlyIndexType,
      &c.compatibleIndexedDrawMergeOnlyBaseVertex,
      &c.compatibleIndexedDrawMergeOnlyStartVertex,
      &c.compatibleIndexedDrawMergeOnlyUniform,
      &c.compatibleIndexedDrawMergeOnlyBindingOverride,
      &c.compatibleIndexedDrawMergeOnlyBindingSnapshot,
      &c.compatibleIndexedDrawMergeOnlyNonContiguous,
      &c.compatibleIndexedDrawMergeOnlyOverflow,
  };
  static_assert(std::size(rejectCounters) ==
                kCompatibleIndexedDrawMergeRejectCount);
  static_assert(std::size(onlyRejectCounters) ==
                kCompatibleIndexedDrawMergeRejectCount);
  for (std::size_t i = 0u; i < rejectPairs.size(); ++i) {
    add(*rejectCounters[i], rejectPairs[i]);
    add(*onlyRejectCounters[i], onlyRejectPairs[i]);
  }
  add(c.compatibleIndexedDrawMergeExactRelaxBindingPayload,
      exactRelaxationSetPairs[1u]);
  add(c.compatibleIndexedDrawMergeExactRelaxUniform,
      exactRelaxationSetPairs[2u]);
  add(c.compatibleIndexedDrawMergeExactRelaxBindingPayloadUniform,
      exactRelaxationSetPairs[3u]);
  add(c.compatibleIndexedDrawMergeExactRelaxNonContiguous,
      exactRelaxationSetPairs[4u]);
  add(c.compatibleIndexedDrawMergeExactRelaxBindingPayloadNonContiguous,
      exactRelaxationSetPairs[5u]);
  add(c.compatibleIndexedDrawMergeExactRelaxUniformNonContiguous,
      exactRelaxationSetPairs[6u]);
  add(c.compatibleIndexedDrawMergeExactRelaxBindingPayloadUniformNonContiguous,
      exactRelaxationSetPairs[7u]);
  add(c.compatibleIndexedDrawMergeExactRelaxOther, otherRelaxationSetPairs);
}

void countIndexedCacheOptCandidate(bool available,
                                   std::uint64_t bytes,
                                   std::uint64_t originalMiss16,
                                   std::uint64_t originalMiss32,
                                   std::uint64_t originalMiss64,
                                   std::uint64_t candidateMiss16,
                                   std::uint64_t candidateMiss32,
                                   std::uint64_t candidateMiss64) {
  if (!available) {
    add(counters().indexedCacheOptCandidateSkipped);
    return;
  }
  add(counters().indexedCacheOptCandidateDraws);
  add(counters().indexedCacheOptCandidateBytes, bytes);
  add(counters().indexedCacheOptCandidateOriginalMiss16, originalMiss16);
  add(counters().indexedCacheOptCandidateOriginalMiss32, originalMiss32);
  add(counters().indexedCacheOptCandidateOriginalMiss64, originalMiss64);
  add(counters().indexedCacheOptCandidateMiss16, candidateMiss16);
  add(counters().indexedCacheOptCandidateMiss32, candidateMiss32);
  add(counters().indexedCacheOptCandidateMiss64, candidateMiss64);
}

void countIndexedCacheOptCandidateGate(bool passed,
                                       std::uint64_t primitiveCount,
                                       bool opaqueDepth,
                                       bool screenBlend) {
  auto& c = counters();
  add(passed ? c.indexedCacheOptCandidateGatePass
             : c.indexedCacheOptCandidateGateFail);
  if (opaqueDepth) {
    add(c.indexedCacheOptCandidateOpaqueDepthDraws);
  }
  if (screenBlend) {
    add(c.indexedCacheOptCandidateScreenBlendDraws);
  }
  if (primitiveCount < 64u) {
    add(c.indexedCacheOptCandidatePrimitiveBucket1_63);
  } else if (primitiveCount < 256u) {
    add(c.indexedCacheOptCandidatePrimitiveBucket64_255);
  } else if (primitiveCount < 1024u) {
    add(c.indexedCacheOptCandidatePrimitiveBucket256_1023);
  } else if (primitiveCount < 4096u) {
    add(c.indexedCacheOptCandidatePrimitiveBucket1024_4095);
  } else {
    add(c.indexedCacheOptCandidatePrimitiveBucket4096Plus);
  }
}

void countReorderedIndexCacheLookup(bool hit,
                                    bool rejected,
                                    bool created,
                                    std::uint64_t createdBytes) {
  add(counters().reorderedIndexCacheLookups);
  if (hit) {
    add(counters().reorderedIndexCacheHits);
  } else if (rejected) {
    add(counters().reorderedIndexCacheRejectedHits);
  } else {
    add(counters().reorderedIndexCacheMisses);
  }
  if (created) {
    add(counters().reorderedIndexCacheCreated);
    add(counters().reorderedIndexCacheCreatedBytes, createdBytes);
  }
}

void countEncodeDrawStreamBindViewportCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawStreamBindViewportCpuNs, nanoseconds);
}

void countEncodeDrawStreamBindFfpCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawStreamBindFfpCpuNs, nanoseconds);
}

void countEncodeDrawStreamBindVsCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawStreamBindVsCpuNs, nanoseconds);
}

void countEncodeDrawStreamBindTextureCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawStreamBindTextureCpuNs, nanoseconds);
}

void countEncodeDrawStreamBindIndexCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawStreamBindIndexCpuNs, nanoseconds);
}

void countEncodeDrawFvfDecodeDeclCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawFvfDecodeDeclCpuNs, nanoseconds);
}

void countEncodeDrawFvfDecodeBytesCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawFvfDecodeBytesCpuNs, nanoseconds);
}

void countEncodeDrawFvfDecodeExpandedCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawFvfDecodeExpandedCpuNs, nanoseconds);
}

void countEncodeDrawUniformBuildMainCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawUniformBuildMainCpuNs, nanoseconds);
}

void countEncodeDrawUniformBuildFfpCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawUniformBuildFfpCpuNs, nanoseconds);
}

void countEncodeDrawUniformBuildVsCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawUniformBuildVsCpuNs, nanoseconds);
}

void countEncodeDrawPhaseSetupCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawPhaseSetupCpuNs, nanoseconds);
}

void countEncodeDrawPhaseArgbufUniformCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawPhaseArgbufUniformCpuNs, nanoseconds);
}

void countEncodeDrawPhaseStreamPrepCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawPhaseStreamPrepCpuNs, nanoseconds);
}

void countEncodeDrawPhaseFfpVertexCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawPhaseFfpVertexCpuNs, nanoseconds);
}

void countEncodeDrawPhaseVertexBindCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawPhaseVertexBindCpuNs, nanoseconds);
}

void countEncodeDrawPhaseBaseStateCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawPhaseBaseStateCpuNs, nanoseconds);
}

void countEncodeDrawPhaseTileFfpFallthroughCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawPhaseTileFfpFallthroughCpuNs, nanoseconds);
}

void countEncodeDrawPhaseRemainderCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawPhaseRemainderCpuNs, nanoseconds);
}

void countEncodeDrawIssueCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawIssueCpuNs, nanoseconds);
  updateMax(counters().encodeDrawIssueCpuMaxNs, nanoseconds);
  recordRing(counters().encodeDrawIssueCpuRing, nanoseconds);
}

void countEncodeDrawIssueIndexedCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawIssueIndexedCpuNs, nanoseconds);
}

void countEncodeDrawIssueNonIndexedCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawIssueNonIndexedCpuNs, nanoseconds);
}

void countEncodeDrawIssueExpandedIndexedCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawIssueExpandedIndexedCpuNs, nanoseconds);
}

void countEncodeDrawIssueSplitIndexedCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawIssueSplitIndexedCpuNs, nanoseconds);
}

void countEncodeDrawIssueMetalCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawIssueMetalCpuNs, nanoseconds);
}

void countEncodeDrawIssueVisibilityCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawIssueVisibilityCpuNs, nanoseconds);
}

void countEncodeDrawPsoPrefetch(bool handleAvailable,
                                bool usedHandle,
                                bool hasBindingOverride,
                                bool bindingOverrideCompatible,
                                bool bypassProbe) {
  auto& c = counters();
  add(handleAvailable ? c.encodeDrawPsoPrefetchHandleAvailable
                      : c.encodeDrawPsoPrefetchHandleMissing);
  if (usedHandle) {
    add(c.encodeDrawPsoPrefetchHandleUsed);
  }
  if (bypassProbe) {
    add(c.encodeDrawPsoPrefetchBypassProbe);
  }
  if (hasBindingOverride) {
    add(c.encodeDrawPsoPrefetchBindingOverride);
    if (bindingOverrideCompatible) {
      add(c.encodeDrawPsoPrefetchBindingOverrideCompatible);
    } else {
      add(c.encodeDrawPsoPrefetchBindingOverrideIncompatible);
      add(c.encodeDrawPsoPrefetchBypassBindingOverride);
    }
  }
}

void countD3D9SnapshotDrawSubmissionCpuTime(std::uint64_t nanoseconds) {
  add(counters().d3d9SnapshotDrawSubmissionCpuNs, nanoseconds);
  updateMax(counters().d3d9SnapshotDrawSubmissionCpuMaxNs, nanoseconds);
  recordRing(counters().d3d9SnapshotDrawSubmissionCpuRing, nanoseconds);
}

void countD3D9SnapshotCacheLookupCpuTime(std::uint64_t nanoseconds) {
  add(counters().d3d9SnapshotCacheLookupCpuNs, nanoseconds);
}

void countD3D9SnapshotCacheHitCpuTime(std::uint64_t nanoseconds) {
  add(counters().d3d9SnapshotCacheHitCpuNs, nanoseconds);
}

void countD3D9SnapshotCacheMissCpuTime(std::uint64_t nanoseconds) {
  add(counters().d3d9SnapshotCacheMissCpuNs, nanoseconds);
}

void countD3D9SnapshotCacheDirectHitCpuTime(std::uint64_t nanoseconds) {
  add(counters().d3d9SnapshotCacheDirectHitCpuNs, nanoseconds);
}

void countD3D9SnapshotCacheDirectMissCpuTime(std::uint64_t nanoseconds) {
  add(counters().d3d9SnapshotCacheDirectMissCpuNs, nanoseconds);
}

void countD3D9SnapshotCacheBatchHitCpuTime(std::uint64_t nanoseconds) {
  add(counters().d3d9SnapshotCacheBatchHitCpuNs, nanoseconds);
}

void countD3D9SnapshotCacheBatchMissCpuTime(std::uint64_t nanoseconds) {
  add(counters().d3d9SnapshotCacheBatchMissCpuNs, nanoseconds);
}

void countD3D9SnapshotCacheBindingLayoutCpuTime(std::uint64_t nanoseconds) {
  add(counters().d3d9SnapshotCacheBindingLayoutCpuNs, nanoseconds);
}

void countD3D9SnapshotCacheUniformRefreshCpuTime(std::uint64_t nanoseconds) {
  add(counters().d3d9SnapshotCacheUniformRefreshCpuNs, nanoseconds);
}

void countD3D9SnapshotCacheUniformBuildCpuTime(std::uint64_t nanoseconds) {
  add(counters().d3d9SnapshotCacheUniformBuildCpuNs, nanoseconds);
}

void countD3D9SnapshotCacheUniformHashCpuTime(std::uint64_t nanoseconds) {
  add(counters().d3d9SnapshotCacheUniformHashCpuNs, nanoseconds);
}

void countD3D9SnapshotCacheMissShaderLayoutCpuTime(std::uint64_t nanoseconds) {
  add(counters().d3d9SnapshotCacheMissShaderLayoutCpuNs, nanoseconds);
}

void countD3D9SnapshotCacheMissUniformBuildCpuTime(std::uint64_t nanoseconds) {
  add(counters().d3d9SnapshotCacheMissUniformBuildCpuNs, nanoseconds);
}

void countD3D9SnapshotCacheMissHotBuildCpuTime(std::uint64_t nanoseconds) {
  add(counters().d3d9SnapshotCacheMissHotBuildCpuNs, nanoseconds);
}

void countD3D9SnapshotCacheDirectMissShaderLayoutCpuTime(std::uint64_t nanoseconds) {
  add(counters().d3d9SnapshotCacheDirectMissShaderLayoutCpuNs, nanoseconds);
}

void countD3D9SnapshotCacheDirectMissUniformBuildCpuTime(std::uint64_t nanoseconds) {
  add(counters().d3d9SnapshotCacheDirectMissUniformBuildCpuNs, nanoseconds);
}

void countD3D9SnapshotCacheDirectMissHotBuildCpuTime(std::uint64_t nanoseconds) {
  add(counters().d3d9SnapshotCacheDirectMissHotBuildCpuNs, nanoseconds);
}

void countD3D9SnapshotCacheBatchMissShaderLayoutCpuTime(std::uint64_t nanoseconds) {
  add(counters().d3d9SnapshotCacheBatchMissShaderLayoutCpuNs, nanoseconds);
}

void countD3D9SnapshotCacheBatchMissShaderLayoutCompatible(bool compatible) {
  auto& c = counters();
  add(compatible ? c.d3d9SnapshotCacheBatchMissShaderLayoutCompatibleHits
                 : c.d3d9SnapshotCacheBatchMissShaderLayoutCompatibleMisses);
}

void countD3D9SnapshotCacheBatchMissShaderLayoutReuse(bool reused) {
  auto& c = counters();
  add(reused ? c.d3d9SnapshotCacheBatchMissShaderLayoutReuseHits
             : c.d3d9SnapshotCacheBatchMissShaderLayoutReuseMisses);
}

void countD3D9SnapshotCacheBatchMissUniformBuildCpuTime(std::uint64_t nanoseconds) {
  add(counters().d3d9SnapshotCacheBatchMissUniformBuildCpuNs, nanoseconds);
}

void countD3D9SnapshotCacheBatchMissHotBuildCpuTime(std::uint64_t nanoseconds) {
  add(counters().d3d9SnapshotCacheBatchMissHotBuildCpuNs, nanoseconds);
}

void countD3D9SnapshotCacheBatchMissHotBuildZeroInitCpuTime(std::uint64_t nanoseconds) {
  add(counters().d3d9SnapshotCacheBatchMissHotBuildZeroInitCpuNs, nanoseconds);
}

void countD3D9SnapshotCacheBatchMissHotBuildKeyCpuTime(std::uint64_t nanoseconds) {
  add(counters().d3d9SnapshotCacheBatchMissHotBuildKeyCpuNs, nanoseconds);
}

void countD3D9SnapshotCacheBatchMissHotBuildKeyZeroInitCpuTime(std::uint64_t nanoseconds) {
  add(counters().d3d9SnapshotCacheBatchMissHotBuildKeyZeroInitCpuNs, nanoseconds);
}

void countD3D9SnapshotCacheBatchMissHotBuildKeyStreamCpuTime(std::uint64_t nanoseconds) {
  add(counters().d3d9SnapshotCacheBatchMissHotBuildKeyStreamCpuNs, nanoseconds);
}

void countD3D9SnapshotCacheBatchMissHotBuildKeyShaderCpuTime(std::uint64_t nanoseconds) {
  add(counters().d3d9SnapshotCacheBatchMissHotBuildKeyShaderCpuNs, nanoseconds);
}

void countD3D9SnapshotCacheBatchMissHotBuildKeyConstantCpuTime(std::uint64_t nanoseconds) {
  add(counters().d3d9SnapshotCacheBatchMissHotBuildKeyConstantCpuNs, nanoseconds);
}

void countD3D9SnapshotCacheBatchMissHotBuildKeyTextureCpuTime(std::uint64_t nanoseconds) {
  add(counters().d3d9SnapshotCacheBatchMissHotBuildKeyTextureCpuNs, nanoseconds);
}

void countD3D9SnapshotCacheBatchMissHotBuildKeySamplerCpuTime(std::uint64_t nanoseconds) {
  add(counters().d3d9SnapshotCacheBatchMissHotBuildKeySamplerCpuNs, nanoseconds);
}

void countD3D9SnapshotCacheBatchMissHotBuildKeyRenderStateCpuTime(std::uint64_t nanoseconds) {
  add(counters().d3d9SnapshotCacheBatchMissHotBuildKeyRenderStateCpuNs, nanoseconds);
}

void countD3D9SnapshotCacheBatchMissHotBuildKeyAttachmentCpuTime(std::uint64_t nanoseconds) {
  add(counters().d3d9SnapshotCacheBatchMissHotBuildKeyAttachmentCpuNs, nanoseconds);
}

void countD3D9SnapshotCacheBatchMissHotBuildKeyUniformCpuTime(std::uint64_t nanoseconds) {
  add(counters().d3d9SnapshotCacheBatchMissHotBuildKeyUniformCpuNs, nanoseconds);
}

void countD3D9SnapshotCacheBatchMissHotBuildBindingCopyCpuTime(std::uint64_t nanoseconds) {
  add(counters().d3d9SnapshotCacheBatchMissHotBuildBindingCopyCpuNs, nanoseconds);
}

void countD3D9SnapshotCacheBatchMissHotBuildRenderStateCpuTime(std::uint64_t nanoseconds) {
  add(counters().d3d9SnapshotCacheBatchMissHotBuildRenderStateCpuNs, nanoseconds);
}

void countD3D9SnapshotCacheBatchMissHotBuildTextureStageStateCpuTime(std::uint64_t nanoseconds) {
  add(counters().d3d9SnapshotCacheBatchMissHotBuildTextureStageStateCpuNs, nanoseconds);
}

void countD3D9SnapshotCacheBatchMissHotBuildSamplerStateCpuTime(std::uint64_t nanoseconds) {
  add(counters().d3d9SnapshotCacheBatchMissHotBuildSamplerStateCpuNs, nanoseconds);
}

void countD3D9SnapshotCacheBatchMissHotBuildTailCopyCpuTime(std::uint64_t nanoseconds) {
  add(counters().d3d9SnapshotCacheBatchMissHotBuildTailCopyCpuNs, nanoseconds);
}

void countD3D9SnapshotCacheBatchMissHotBuildFlatRenderReuse(bool reused) {
  auto& c = counters();
  add(reused ? c.d3d9SnapshotCacheBatchMissHotBuildFlatRenderReuseHits
             : c.d3d9SnapshotCacheBatchMissHotBuildFlatRenderReuseMisses);
}

void countD3D9SnapshotCacheBatchMissHotBuildFlatTssReuse(bool reused) {
  auto& c = counters();
  add(reused ? c.d3d9SnapshotCacheBatchMissHotBuildFlatTssReuseHits
             : c.d3d9SnapshotCacheBatchMissHotBuildFlatTssReuseMisses);
}

void countD3D9SnapshotCacheBatchMissHotBuildFlatSamplerReuse(bool reused) {
  auto& c = counters();
  add(reused ? c.d3d9SnapshotCacheBatchMissHotBuildFlatSamplerReuseHits
             : c.d3d9SnapshotCacheBatchMissHotBuildFlatSamplerReuseMisses);
}

void countD3D9SnapshotCacheBatchMissUniformNonConstHashReuse(bool reused) {
  auto& c = counters();
  add(reused ? c.d3d9SnapshotCacheBatchMissUniformNonConstHashReuseHits
             : c.d3d9SnapshotCacheBatchMissUniformNonConstHashReuseMisses);
}

void countD3D9SnapshotCacheBatchMissUniformPayloadPath(
    bool reusedFullPayload,
    bool reusedNonConstantPayload) {
  auto& c = counters();
  if (reusedFullPayload) {
    add(c.d3d9SnapshotCacheBatchMissUniformPayloadReuseFull);
  } else if (reusedNonConstantPayload) {
    add(c.d3d9SnapshotCacheBatchMissUniformPayloadReuseNonConst);
  } else {
    add(c.d3d9SnapshotCacheBatchMissUniformPayloadFullBuild);
  }
}

void countD3D9SnapshotCacheBatchMissUniformVsConstHashPath(bool reused) {
  auto& c = counters();
  add(reused ? c.d3d9SnapshotCacheBatchMissUniformVsConstHashReuse
             : c.d3d9SnapshotCacheBatchMissUniformVsConstHashBuild);
}

void countD3D9SnapshotCacheBatchMissUniformPsConstHashPath(bool reused) {
  auto& c = counters();
  add(reused ? c.d3d9SnapshotCacheBatchMissUniformPsConstHashReuse
             : c.d3d9SnapshotCacheBatchMissUniformPsConstHashBuild);
}

void countD3D9SnapshotCacheBatchMissUniformVsConstHashMemoProbe(bool hit) {
  auto& c = counters();
  add(c.d3d9SnapshotCacheBatchMissUniformVsConstHashMemoProbe);
  add(hit ? c.d3d9SnapshotCacheBatchMissUniformVsConstHashMemoHits
          : c.d3d9SnapshotCacheBatchMissUniformVsConstHashMemoMisses);
}

void countD3D9SnapshotCacheBatchMissUniformPsConstHashMemoProbe(bool hit) {
  auto& c = counters();
  add(c.d3d9SnapshotCacheBatchMissUniformPsConstHashMemoProbe);
  add(hit ? c.d3d9SnapshotCacheBatchMissUniformPsConstHashMemoHits
          : c.d3d9SnapshotCacheBatchMissUniformPsConstHashMemoMisses);
}

void countD3D9SnapshotCacheBatchMissUniformVsConstHashMemoStore() {
  add(counters().d3d9SnapshotCacheBatchMissUniformVsConstHashMemoStores);
}

void countD3D9SnapshotCacheBatchMissUniformPsConstHashMemoStore() {
  add(counters().d3d9SnapshotCacheBatchMissUniformPsConstHashMemoStores);
}

void countD3D9SnapshotUniformBuildCall() {
  add(counters().d3d9SnapshotUniformBuildCalls);
  addBatchMissUniformBuild(
      counters().d3d9SnapshotCacheBatchMissUniformBuildCalls);
}

void countD3D9SnapshotUniformBuildVsConstCopyCpuTime(std::uint64_t nanoseconds) {
  add(counters().d3d9SnapshotUniformBuildVsConstCopyCpuNs, nanoseconds);
  addBatchMissUniformBuild(
      counters().d3d9SnapshotCacheBatchMissUniformBuildVsConstCopyCpuNs,
      nanoseconds);
}

void countD3D9SnapshotUniformBuildPsConstCopyCpuTime(std::uint64_t nanoseconds) {
  add(counters().d3d9SnapshotUniformBuildPsConstCopyCpuNs, nanoseconds);
  addBatchMissUniformBuild(
      counters().d3d9SnapshotCacheBatchMissUniformBuildPsConstCopyCpuNs,
      nanoseconds);
}

void countD3D9SnapshotUniformBuildFfpMatrixCpuTime(std::uint64_t nanoseconds) {
  add(counters().d3d9SnapshotUniformBuildFfpMatrixCpuNs, nanoseconds);
  addBatchMissUniformBuild(
      counters().d3d9SnapshotCacheBatchMissUniformBuildFfpMatrixCpuNs,
      nanoseconds);
}

void countD3D9SnapshotUniformBuildFfpMaterialLightCpuTime(std::uint64_t nanoseconds) {
  add(counters().d3d9SnapshotUniformBuildFfpMaterialLightCpuNs, nanoseconds);
  addBatchMissUniformBuild(
      counters().d3d9SnapshotCacheBatchMissUniformBuildFfpMaterialLightCpuNs,
      nanoseconds);
}

void countD3D9SnapshotUniformBuildTextureTransformCpuTime(std::uint64_t nanoseconds) {
  add(counters().d3d9SnapshotUniformBuildTextureTransformCpuNs, nanoseconds);
  addBatchMissUniformBuild(
      counters().d3d9SnapshotCacheBatchMissUniformBuildTextureTransformCpuNs,
      nanoseconds);
}

void countD3D9SnapshotUniformBuildClipPlaneCpuTime(std::uint64_t nanoseconds) {
  add(counters().d3d9SnapshotUniformBuildClipPlaneCpuNs, nanoseconds);
  addBatchMissUniformBuild(
      counters().d3d9SnapshotCacheBatchMissUniformBuildClipPlaneCpuNs,
      nanoseconds);
}

void countD3D9SnapshotUniformBuildHashCpuTime(std::uint64_t nanoseconds) {
  add(counters().d3d9SnapshotUniformBuildHashCpuNs, nanoseconds);
  addBatchMissUniformBuild(
      counters().d3d9SnapshotCacheBatchMissUniformBuildHashCpuNs,
      nanoseconds);
}

void countD3D9SnapshotUniformBuildVsConstHashCpuTime(std::uint64_t nanoseconds) {
  add(counters().d3d9SnapshotUniformBuildVsConstHashCpuNs, nanoseconds);
  addBatchMissUniformBuild(
      counters().d3d9SnapshotCacheBatchMissUniformBuildVsConstHashCpuNs,
      nanoseconds);
}

void countD3D9SnapshotUniformBuildPsConstHashCpuTime(std::uint64_t nanoseconds) {
  add(counters().d3d9SnapshotUniformBuildPsConstHashCpuNs, nanoseconds);
  addBatchMissUniformBuild(
      counters().d3d9SnapshotCacheBatchMissUniformBuildPsConstHashCpuNs,
      nanoseconds);
}

void countD3D9SnapshotUniformBuildNonConstHashCpuTime(std::uint64_t nanoseconds) {
  add(counters().d3d9SnapshotUniformBuildNonConstHashCpuNs, nanoseconds);
  addBatchMissUniformBuild(
      counters().d3d9SnapshotCacheBatchMissUniformBuildNonConstHashCpuNs,
      nanoseconds);
}

void countD3D9SnapshotUniformBuildNonConstHashWorldViewProjCpuTime(std::uint64_t nanoseconds) {
  add(counters().d3d9SnapshotUniformBuildNonConstHashWorldViewProjCpuNs, nanoseconds);
  addBatchMissUniformBuild(
      counters().d3d9SnapshotCacheBatchMissUniformBuildNonConstHashWorldViewProjCpuNs,
      nanoseconds);
}

void countD3D9SnapshotUniformBuildNonConstHashFfpWorldViewCpuTime(std::uint64_t nanoseconds) {
  add(counters().d3d9SnapshotUniformBuildNonConstHashFfpWorldViewCpuNs, nanoseconds);
  addBatchMissUniformBuild(
      counters().d3d9SnapshotCacheBatchMissUniformBuildNonConstHashFfpWorldViewCpuNs,
      nanoseconds);
}

void countD3D9SnapshotUniformBuildNonConstHashFfpNormalMatrixCpuTime(std::uint64_t nanoseconds) {
  add(counters().d3d9SnapshotUniformBuildNonConstHashFfpNormalMatrixCpuNs, nanoseconds);
  addBatchMissUniformBuild(
      counters().d3d9SnapshotCacheBatchMissUniformBuildNonConstHashFfpNormalMatrixCpuNs,
      nanoseconds);
}

void countD3D9SnapshotUniformBuildNonConstHashMaterialCpuTime(std::uint64_t nanoseconds) {
  add(counters().d3d9SnapshotUniformBuildNonConstHashMaterialCpuNs, nanoseconds);
  addBatchMissUniformBuild(
      counters().d3d9SnapshotCacheBatchMissUniformBuildNonConstHashMaterialCpuNs,
      nanoseconds);
}

void countD3D9SnapshotUniformBuildNonConstHashLightsCpuTime(std::uint64_t nanoseconds) {
  add(counters().d3d9SnapshotUniformBuildNonConstHashLightsCpuNs, nanoseconds);
  addBatchMissUniformBuild(
      counters().d3d9SnapshotCacheBatchMissUniformBuildNonConstHashLightsCpuNs,
      nanoseconds);
}

void countD3D9SnapshotUniformBuildNonConstHashFfpBlendWvpCpuTime(std::uint64_t nanoseconds) {
  add(counters().d3d9SnapshotUniformBuildNonConstHashFfpBlendWvpCpuNs, nanoseconds);
  addBatchMissUniformBuild(
      counters().d3d9SnapshotCacheBatchMissUniformBuildNonConstHashFfpBlendWvpCpuNs,
      nanoseconds);
}

void countD3D9SnapshotUniformBuildNonConstHashTextureTransformsCpuTime(std::uint64_t nanoseconds) {
  add(counters().d3d9SnapshotUniformBuildNonConstHashTextureTransformsCpuNs, nanoseconds);
  addBatchMissUniformBuild(
      counters().d3d9SnapshotCacheBatchMissUniformBuildNonConstHashTextureTransformsCpuNs,
      nanoseconds);
}

void countD3D9SnapshotUniformBuildNonConstHashClipPlanesCpuTime(std::uint64_t nanoseconds) {
  add(counters().d3d9SnapshotUniformBuildNonConstHashClipPlanesCpuNs, nanoseconds);
  addBatchMissUniformBuild(
      counters().d3d9SnapshotCacheBatchMissUniformBuildNonConstHashClipPlanesCpuNs,
      nanoseconds);
}

void countD3D9SnapshotUniformBuildPayloadCombineHashCpuTime(std::uint64_t nanoseconds) {
  add(counters().d3d9SnapshotUniformBuildPayloadCombineHashCpuNs, nanoseconds);
  addBatchMissUniformBuild(
      counters().d3d9SnapshotCacheBatchMissUniformBuildPayloadCombineHashCpuNs,
      nanoseconds);
}

void countD3D9SnapshotUniformBuildVsConstHashFull() {
  add(counters().d3d9SnapshotUniformBuildVsConstHashFull);
  addBatchMissUniformBuild(
      counters().d3d9SnapshotCacheBatchMissUniformBuildVsConstHashFull);
}

void countD3D9SnapshotUniformBuildPsConstHashFull() {
  add(counters().d3d9SnapshotUniformBuildPsConstHashFull);
  addBatchMissUniformBuild(
      counters().d3d9SnapshotCacheBatchMissUniformBuildPsConstHashFull);
}

void countD3D9SnapshotUniformBuildVsConstHashFullNoUsage() {
  add(counters().d3d9SnapshotUniformBuildVsConstHashFullNoUsage);
  addBatchMissUniformBuild(
      counters().d3d9SnapshotCacheBatchMissUniformBuildVsConstHashFullNoUsage);
}

void countD3D9SnapshotUniformBuildPsConstHashFullNoUsage() {
  add(counters().d3d9SnapshotUniformBuildPsConstHashFullNoUsage);
  addBatchMissUniformBuild(
      counters().d3d9SnapshotCacheBatchMissUniformBuildPsConstHashFullNoUsage);
}

void countD3D9SnapshotUniformBuildVsConstHashFullUnknown() {
  add(counters().d3d9SnapshotUniformBuildVsConstHashFullUnknown);
  addBatchMissUniformBuild(
      counters().d3d9SnapshotCacheBatchMissUniformBuildVsConstHashFullUnknown);
}

void countD3D9SnapshotUniformBuildPsConstHashFullUnknown() {
  add(counters().d3d9SnapshotUniformBuildPsConstHashFullUnknown);
  addBatchMissUniformBuild(
      counters().d3d9SnapshotCacheBatchMissUniformBuildPsConstHashFullUnknown);
}

void countD3D9SnapshotUniformBuildVsConstHashFullUnknownBytecode() {
  add(counters().d3d9SnapshotUniformBuildVsConstHashFullUnknownBytecode);
  addBatchMissUniformBuild(
      counters().d3d9SnapshotCacheBatchMissUniformBuildVsConstHashFullUnknownBytecode);
}

void countD3D9SnapshotUniformBuildPsConstHashFullUnknownBytecode() {
  add(counters().d3d9SnapshotUniformBuildPsConstHashFullUnknownBytecode);
  addBatchMissUniformBuild(
      counters().d3d9SnapshotCacheBatchMissUniformBuildPsConstHashFullUnknownBytecode);
}

void countD3D9SnapshotUniformBuildVsConstHashFullUnknownNonBytecode() {
  add(counters().d3d9SnapshotUniformBuildVsConstHashFullUnknownNonBytecode);
  addBatchMissUniformBuild(
      counters().d3d9SnapshotCacheBatchMissUniformBuildVsConstHashFullUnknownNonBytecode);
}

void countD3D9SnapshotUniformBuildPsConstHashFullUnknownNonBytecode() {
  add(counters().d3d9SnapshotUniformBuildPsConstHashFullUnknownNonBytecode);
  addBatchMissUniformBuild(
      counters().d3d9SnapshotCacheBatchMissUniformBuildPsConstHashFullUnknownNonBytecode);
}

void countD3D9SnapshotUniformBuildVsConstHashFullIndexedFloat() {
  add(counters().d3d9SnapshotUniformBuildVsConstHashFullIndexedFloat);
  addBatchMissUniformBuild(
      counters().d3d9SnapshotCacheBatchMissUniformBuildVsConstHashFullIndexedFloat);
}

void countD3D9SnapshotUniformBuildVsConstHashFullIndexedFloatMinSafeBytes(
    std::uint64_t bytes) {
  add(counters().d3d9SnapshotUniformBuildVsConstHashFullIndexedFloatMinSafeBytes,
      bytes);
  addBatchMissUniformBuild(
      counters().d3d9SnapshotCacheBatchMissUniformBuildVsConstHashFullIndexedFloatMinSafeBytes,
      bytes);
}

void countD3D9SnapshotUniformBuildVsConstHashFullIndexedFloatPotentialSavedBytes(
    std::uint64_t bytes) {
  add(counters().d3d9SnapshotUniformBuildVsConstHashFullIndexedFloatPotentialSavedBytes,
      bytes);
  addBatchMissUniformBuild(
      counters().d3d9SnapshotCacheBatchMissUniformBuildVsConstHashFullIndexedFloatPotentialSavedBytes,
      bytes);
}

void countD3D9SnapshotUniformBuildPsConstHashFullIndexedFloat() {
  add(counters().d3d9SnapshotUniformBuildPsConstHashFullIndexedFloat);
  addBatchMissUniformBuild(
      counters().d3d9SnapshotCacheBatchMissUniformBuildPsConstHashFullIndexedFloat);
}

void countD3D9SnapshotUniformBuildVsConstHashFullIndexedInt() {
  add(counters().d3d9SnapshotUniformBuildVsConstHashFullIndexedInt);
  addBatchMissUniformBuild(
      counters().d3d9SnapshotCacheBatchMissUniformBuildVsConstHashFullIndexedInt);
}

void countD3D9SnapshotUniformBuildPsConstHashFullIndexedInt() {
  add(counters().d3d9SnapshotUniformBuildPsConstHashFullIndexedInt);
  addBatchMissUniformBuild(
      counters().d3d9SnapshotCacheBatchMissUniformBuildPsConstHashFullIndexedInt);
}

void countD3D9SnapshotUniformBuildVsConstHashFullIndexedBool() {
  add(counters().d3d9SnapshotUniformBuildVsConstHashFullIndexedBool);
  addBatchMissUniformBuild(
      counters().d3d9SnapshotCacheBatchMissUniformBuildVsConstHashFullIndexedBool);
}

void countD3D9SnapshotUniformBuildPsConstHashFullIndexedBool() {
  add(counters().d3d9SnapshotUniformBuildPsConstHashFullIndexedBool);
  addBatchMissUniformBuild(
      counters().d3d9SnapshotCacheBatchMissUniformBuildPsConstHashFullIndexedBool);
}

void countD3D9SnapshotUniformBuildVsConstHashBytes(std::uint64_t bytes) {
  add(counters().d3d9SnapshotUniformBuildVsConstHashBytes, bytes);
  addBatchMissUniformBuild(
      counters().d3d9SnapshotCacheBatchMissUniformBuildVsConstHashBytes,
      bytes);
}

void countD3D9SnapshotUniformBuildPsConstHashBytes(std::uint64_t bytes) {
  add(counters().d3d9SnapshotUniformBuildPsConstHashBytes, bytes);
  addBatchMissUniformBuild(
      counters().d3d9SnapshotCacheBatchMissUniformBuildPsConstHashBytes,
      bytes);
}

void countD3D9SnapshotUniformCopyCpuTime(std::uint64_t nanoseconds) {
  add(counters().d3d9SnapshotUniformCopyCpuNs, nanoseconds);
}

void countD3D9SnapshotUniformMaterialized(std::uint64_t bytes) {
  auto& c = counters();
  add(c.d3d9SnapshotUniformMaterialized);
  add(c.d3d9SnapshotUniformMaterializedBytes, bytes);
}

void countD3D9SnapshotSubmissionCarrier(
    std::uint64_t carrierBytes,
    std::uint64_t stateStorageBytes,
    std::uint64_t uniformStorageBytes,
    bool uniformStorageUnused) {
  auto& c = counters();
  add(c.d3d9SnapshotSubmissionCarrierRecords);
  add(c.d3d9SnapshotSubmissionCarrierBytes, carrierBytes);
  add(c.d3d9SnapshotSubmissionCarrierStateStorageBytes, stateStorageBytes);
  add(c.d3d9SnapshotSubmissionCarrierUniformStorageBytes, uniformStorageBytes);
  if (uniformStorageUnused && uniformStorageBytes != 0) {
    add(c.d3d9SnapshotSubmissionCarrierUnusedUniformStorageRecords);
    add(c.d3d9SnapshotSubmissionCarrierUnusedUniformStorageBytes,
        uniformStorageBytes);
  }
}

void countD3D9SnapshotUniformElided(std::uint64_t bytes) {
  auto& c = counters();
  add(c.d3d9SnapshotUniformElided);
  add(c.d3d9SnapshotUniformElidedBytes, bytes);
}

void countD3D9SnapshotUniformAdjacentSameGeneration(bool sameStateLane,
                                                    std::uint64_t bytes) {
  auto& c = counters();
  add(c.d3d9SnapshotUniformAdjacentSameGen);
  add(c.d3d9SnapshotUniformAdjacentSameGenBytes, bytes);
  if (sameStateLane) {
    add(c.d3d9SnapshotUniformAdjacentSameGenSameState);
    add(c.d3d9SnapshotUniformAdjacentSameGenSameStateBytes, bytes);
  } else {
    add(c.d3d9SnapshotUniformAdjacentSameGenDiffState);
    add(c.d3d9SnapshotUniformAdjacentSameGenDiffStateBytes, bytes);
  }
}

void countD3D9SnapshotUniformAdjacentSamePayloadHash(bool sameStateLane,
                                                     bool sameGeneration,
                                                     std::uint64_t bytes) {
  auto& c = counters();
  add(c.d3d9SnapshotUniformAdjacentSamePayloadHash);
  add(c.d3d9SnapshotUniformAdjacentSamePayloadHashBytes, bytes);
  if (sameStateLane) {
    add(c.d3d9SnapshotUniformAdjacentSamePayloadHashSameState);
    add(c.d3d9SnapshotUniformAdjacentSamePayloadHashSameStateBytes, bytes);
  } else {
    add(c.d3d9SnapshotUniformAdjacentSamePayloadHashDiffState);
    add(c.d3d9SnapshotUniformAdjacentSamePayloadHashDiffStateBytes, bytes);
  }
  if (!sameGeneration) {
    add(c.d3d9SnapshotUniformAdjacentSamePayloadHashDiffGeneration);
    add(c.d3d9SnapshotUniformAdjacentSamePayloadHashDiffGenerationBytes, bytes);
  }
}

void countD3D9SnapshotUniformAdjacentComponentHashes(bool sameStateLane,
                                                     bool sameGeneration,
                                                     bool sameVertexConstants,
                                                     bool samePixelConstants,
                                                     bool sameFixedPayload) {
  auto& c = counters();
  add(c.d3d9SnapshotUniformAdjacentPreviousPayload);
  if (sameFixedPayload) {
    add(c.d3d9SnapshotUniformAdjacentSameFixedPayloadHash);
    if (sameStateLane) {
      add(c.d3d9SnapshotUniformAdjacentSameFixedPayloadHashSameState);
    }
    if (!sameGeneration) {
      add(c.d3d9SnapshotUniformAdjacentSameFixedPayloadHashDiffGeneration);
    }
  }
  if (sameVertexConstants) {
    add(c.d3d9SnapshotUniformAdjacentSameVsConstHash);
    if (sameStateLane) {
      add(c.d3d9SnapshotUniformAdjacentSameVsConstHashSameState);
    }
    if (!sameGeneration) {
      add(c.d3d9SnapshotUniformAdjacentSameVsConstHashDiffGeneration);
    }
  }
  if (samePixelConstants) {
    add(c.d3d9SnapshotUniformAdjacentSamePsConstHash);
    if (sameStateLane) {
      add(c.d3d9SnapshotUniformAdjacentSamePsConstHashSameState);
    }
    if (!sameGeneration) {
      add(c.d3d9SnapshotUniformAdjacentSamePsConstHashDiffGeneration);
    }
  }
  if (sameVertexConstants && samePixelConstants) {
    add(c.d3d9SnapshotUniformAdjacentSameShaderConstHashes);
    if (sameStateLane) {
      add(c.d3d9SnapshotUniformAdjacentSameShaderConstHashesSameState);
    }
    if (!sameGeneration) {
      add(c.d3d9SnapshotUniformAdjacentSameShaderConstHashesDiffGeneration);
    }
  }
  if (sameFixedPayload && sameVertexConstants && samePixelConstants) {
    add(c.d3d9SnapshotUniformAdjacentSameFixedAndShaderConstHashes);
    if (sameStateLane) {
      add(c.d3d9SnapshotUniformAdjacentSameFixedAndShaderConstHashesSameState);
    }
    if (!sameGeneration) {
      add(c.d3d9SnapshotUniformAdjacentSameFixedAndShaderConstHashesDiffGeneration);
    }
  }
}

void countD3D9SnapshotStateCopyCpuTime(std::uint64_t nanoseconds) {
  add(counters().d3d9SnapshotStateCopyCpuNs, nanoseconds);
}

void countD3D9SnapshotStateMaterialized(std::uint64_t bytes) {
  auto& c = counters();
  add(c.d3d9SnapshotStateMaterialized);
  add(c.d3d9SnapshotStateMaterializedBytes, bytes);
}

void countD3D9SnapshotStateElided(std::uint64_t bytes) {
  auto& c = counters();
  add(c.d3d9SnapshotStateElided);
  add(c.d3d9SnapshotStateElidedBytes, bytes);
}

void countD3D9SnapshotDebugSnapshotCpuTime(std::uint64_t nanoseconds) {
  add(counters().d3d9SnapshotDebugSnapshotCpuNs, nanoseconds);
}

void countD3D9SnapshotFlatStateEntries(std::uint32_t renderStateEntries,
                                       std::uint32_t textureStageStateEntries,
                                       std::uint32_t textureStageStateEntryMax,
                                       std::uint32_t samplerStateEntries,
                                       std::uint32_t samplerStateEntryMax,
                                       bool renderStateOverflow,
                                       bool textureStageStateOverflow,
                                       bool samplerStateOverflow) {
  auto& c = counters();
  add(c.d3d9SnapshotFlatStateSamples);
  add(c.d3d9SnapshotFlatRenderStateEntries, renderStateEntries);
  updateMax(c.d3d9SnapshotFlatRenderStateEntriesMax, renderStateEntries);
  if (renderStateEntries > 64u) {
    add(c.d3d9SnapshotFlatRenderStateEntriesGt64);
  }
  if (renderStateEntries > 128u) {
    add(c.d3d9SnapshotFlatRenderStateEntriesGt128);
  }
  if (renderStateOverflow) {
    add(c.d3d9SnapshotFlatRenderStateOverflow);
  }
  add(c.d3d9SnapshotFlatTssEntries, textureStageStateEntries);
  updateMax(c.d3d9SnapshotFlatTssStageEntriesMax, textureStageStateEntryMax);
  if (textureStageStateOverflow) {
    add(c.d3d9SnapshotFlatTssOverflow);
  }
  add(c.d3d9SnapshotFlatSamplerEntries, samplerStateEntries);
  updateMax(c.d3d9SnapshotFlatSamplerSlotEntriesMax, samplerStateEntryMax);
  if (samplerStateOverflow) {
    add(c.d3d9SnapshotFlatSamplerOverflow);
  }
}

void countD3D9SnapshotBindingOverrideCpuTime(std::uint64_t nanoseconds) {
  add(counters().d3d9SnapshotBindingOverrideCpuNs, nanoseconds);
}

void countD3D9SnapshotBindingOverride(std::uint32_t streamScans,
                                      std::uint32_t streamRecords,
                                      bool indexRecord) {
  add(counters().d3d9SnapshotBindingOverrideStreamScans, streamScans);
  add(counters().d3d9SnapshotBindingOverrideStreamRecords, streamRecords);
  if (indexRecord) {
    add(counters().d3d9SnapshotBindingOverrideIndexRecords);
  }
}

void countDrawUniformPayloadLookupCandidateHit() {
  add(counters().drawUniformPayloadLookupCandidateHits);
}

void countDrawUniformPayloadLookupLastHit() {
  add(counters().drawUniformPayloadLookupLastHits);
}

void countDrawUniformPayloadLookupBucketHit() {
  add(counters().drawUniformPayloadLookupBucketHits);
}

void countDrawUniformPayloadLookupBucketMiss() {
  add(counters().drawUniformPayloadLookupBucketMisses);
}

void countDrawUniformPayloadLookupLinearHit() {
  add(counters().drawUniformPayloadLookupLinearHits);
}

void countDrawUniformPayloadLookupBucketProbe(std::uint64_t probes) {
  add(counters().drawUniformPayloadLookupBucketProbes, probes);
}

void countDrawUniformPayloadLookupBucketCollision(std::uint64_t collisions) {
  add(counters().drawUniformPayloadLookupBucketCollisions, collisions);
}

void countDrawUniformPayloadLookupHashCollision(std::uint64_t collisions) {
  add(counters().drawUniformPayloadLookupHashCollisions, collisions);
}

void countDrawUniformPayloadLookupSemanticHashMiss(std::uint64_t bytes) {
  add(counters().drawUniformPayloadLookupSemanticHashMisses);
  add(counters().drawUniformPayloadLookupSemanticHashMissBytes, bytes);
}

void countDrawUniformPayloadLookupCpuTime(std::uint64_t nanoseconds) {
  add(counters().drawUniformPayloadLookupCpuNs, nanoseconds);
  updateMax(counters().drawUniformPayloadLookupCpuMaxNs, nanoseconds);
}

void countDrawUniformPayloadLookupBucketCpuTime(std::uint64_t nanoseconds) {
  add(counters().drawUniformPayloadLookupBucketCpuNs, nanoseconds);
  updateMax(counters().drawUniformPayloadLookupBucketCpuMaxNs, nanoseconds);
}

void countDrawUniformPayloadAppend() {
  add(counters().drawUniformPayloadAppends);
}

void countDrawUniformPayloadAppendBytes(std::uint64_t bytes) {
  add(counters().drawUniformPayloadAppendBytes, bytes);
}

void countDrawUniformFixedPayloadAppend() {
  add(counters().drawUniformFixedPayloadAppends);
}

void countDrawUniformFixedPayloadAppendBytes(std::uint64_t bytes) {
  add(counters().drawUniformFixedPayloadAppendBytes, bytes);
}

void countDrawUniformVertexConstantsAppend() {
  add(counters().drawUniformVertexConstantsAppends);
}

void countDrawUniformVertexConstantsAppendBytes(std::uint64_t bytes) {
  add(counters().drawUniformVertexConstantsAppendBytes, bytes);
}

void countDrawUniformPixelConstantsAppend() {
  add(counters().drawUniformPixelConstantsAppends);
}

void countDrawUniformPixelConstantsAppendBytes(std::uint64_t bytes) {
  add(counters().drawUniformPixelConstantsAppendBytes, bytes);
}

void countDrawUniformPayloadAppendFixedFindCpuTime(
    std::uint64_t nanoseconds) {
  add(counters().drawUniformPayloadAppendFixedFindCpuNs, nanoseconds);
  updateMax(counters().drawUniformPayloadAppendFixedFindCpuMaxNs, nanoseconds);
}

void countDrawUniformPayloadAppendVertexFindCpuTime(
    std::uint64_t nanoseconds) {
  add(counters().drawUniformPayloadAppendVertexFindCpuNs, nanoseconds);
  updateMax(counters().drawUniformPayloadAppendVertexFindCpuMaxNs, nanoseconds);
}

void countDrawUniformPayloadAppendPixelFindCpuTime(
    std::uint64_t nanoseconds) {
  add(counters().drawUniformPayloadAppendPixelFindCpuNs, nanoseconds);
  updateMax(counters().drawUniformPayloadAppendPixelFindCpuMaxNs, nanoseconds);
}

void countDrawUniformPayloadAppendFixedAppendCpuTime(
    std::uint64_t nanoseconds) {
  add(counters().drawUniformPayloadAppendFixedAppendCpuNs, nanoseconds);
  updateMax(counters().drawUniformPayloadAppendFixedAppendCpuMaxNs,
            nanoseconds);
}

void countDrawUniformPayloadAppendVertexAppendCpuTime(
    std::uint64_t nanoseconds) {
  add(counters().drawUniformPayloadAppendVertexAppendCpuNs, nanoseconds);
  updateMax(counters().drawUniformPayloadAppendVertexAppendCpuMaxNs,
            nanoseconds);
}

void countDrawUniformPayloadAppendPixelAppendCpuTime(
    std::uint64_t nanoseconds) {
  add(counters().drawUniformPayloadAppendPixelAppendCpuNs, nanoseconds);
  updateMax(counters().drawUniformPayloadAppendPixelAppendCpuMaxNs,
            nanoseconds);
}

void countDrawUniformPayloadMaterialized(
    DrawUniformPayloadMaterializeSite site,
    std::uint64_t bytes) {
  auto& c = counters();
  add(c.drawUniformPayloadMaterialized);
  add(c.drawUniformPayloadMaterializedBytes, bytes);
  switch (site) {
    case DrawUniformPayloadMaterializeSite::DrawEncoderCommand:
      add(c.drawUniformPayloadMaterializedDrawEncoderCommand);
      add(c.drawUniformPayloadMaterializedDrawEncoderCommandBytes, bytes);
      break;
    case DrawUniformPayloadMaterializeSite::DrawEncoderParam:
      add(c.drawUniformPayloadMaterializedDrawEncoderParam);
      add(c.drawUniformPayloadMaterializedDrawEncoderParamBytes, bytes);
      break;
    case DrawUniformPayloadMaterializeSite::FramegraphCommand:
      add(c.drawUniformPayloadMaterializedFramegraphCommand);
      add(c.drawUniformPayloadMaterializedFramegraphCommandBytes, bytes);
      break;
    case DrawUniformPayloadMaterializeSite::FramegraphParam:
      add(c.drawUniformPayloadMaterializedFramegraphParam);
      add(c.drawUniformPayloadMaterializedFramegraphParamBytes, bytes);
      break;
    case DrawUniformPayloadMaterializeSite::QueueObservation:
      add(c.drawUniformPayloadMaterializedQueueObservation);
      add(c.drawUniformPayloadMaterializedQueueObservationBytes, bytes);
      break;
    case DrawUniformPayloadMaterializeSite::Other:
      add(c.drawUniformPayloadMaterializedOther);
      add(c.drawUniformPayloadMaterializedOtherBytes, bytes);
      break;
  }
}

void countDrawUniformPayloadMaterializeFallback(
    DrawUniformPayloadMaterializeSite site) {
  (void)site;
  add(counters().drawUniformPayloadMaterializeFallbacks);
}

void countDrawUniformPayloadMaterializeCpuTime(
    DrawUniformPayloadMaterializeSite site,
    std::uint64_t nanoseconds) {
  auto& c = counters();
  add(c.drawUniformPayloadMaterializeCpuNs, nanoseconds);
  updateMax(c.drawUniformPayloadMaterializeCpuMaxNs, nanoseconds);
  switch (site) {
    case DrawUniformPayloadMaterializeSite::DrawEncoderCommand:
      add(c.drawUniformPayloadMaterializeDrawEncoderCommandCpuNs, nanoseconds);
      break;
    case DrawUniformPayloadMaterializeSite::DrawEncoderParam:
      add(c.drawUniformPayloadMaterializeDrawEncoderParamCpuNs, nanoseconds);
      break;
    case DrawUniformPayloadMaterializeSite::FramegraphCommand:
      add(c.drawUniformPayloadMaterializeFramegraphCommandCpuNs, nanoseconds);
      break;
    case DrawUniformPayloadMaterializeSite::FramegraphParam:
      add(c.drawUniformPayloadMaterializeFramegraphParamCpuNs, nanoseconds);
      break;
    case DrawUniformPayloadMaterializeSite::QueueObservation:
      add(c.drawUniformPayloadMaterializeQueueObservationCpuNs, nanoseconds);
      break;
    case DrawUniformPayloadMaterializeSite::Other:
      add(c.drawUniformPayloadMaterializeOtherCpuNs, nanoseconds);
      break;
  }
}

void countDrawUniformPayloadAppendReserveCpuTime(std::uint64_t nanoseconds) {
  add(counters().drawUniformPayloadAppendReserveCpuNs, nanoseconds);
  updateMax(counters().drawUniformPayloadAppendReserveCpuMaxNs, nanoseconds);
}

void countDrawUniformPayloadAppendCopyCpuTime(std::uint64_t nanoseconds) {
  add(counters().drawUniformPayloadAppendCopyCpuNs, nanoseconds);
  updateMax(counters().drawUniformPayloadAppendCopyCpuMaxNs, nanoseconds);
}

void countDrawUniformPayloadAppendLinkCpuTime(std::uint64_t nanoseconds) {
  add(counters().drawUniformPayloadAppendLinkCpuNs, nanoseconds);
  updateMax(counters().drawUniformPayloadAppendLinkCpuMaxNs, nanoseconds);
}

void countTransientUploadCpuTime(std::uint64_t nanoseconds, std::size_t bytes) {
  add(counters().transientUploadCalls);
  add(counters().transientUploadBytes, static_cast<std::uint64_t>(bytes));
  add(counters().transientUploadCpuNs, nanoseconds);
  updateMax(counters().transientUploadCpuMaxNs, nanoseconds);
  recordRing(counters().transientUploadCpuRing, nanoseconds);
}

void countD3D9BufferLock(std::uint64_t nanoseconds,
                         std::uint64_t bytes,
                         std::uint64_t shadowAllocNanoseconds,
                         std::uint64_t shadowCopyNanoseconds,
                         std::uint32_t d3dFlags,
                         std::uint32_t usage,
                         std::uint32_t pool,
                         bool fullResource,
                         bool shadowCopy) {
  static constexpr std::uint32_t kD3DLockReadOnly = 0x00000010u;
  static constexpr std::uint32_t kD3DLockNoOverwrite = 0x00001000u;
  static constexpr std::uint32_t kD3DLockDiscard = 0x00002000u;
  static constexpr std::uint32_t kUsageDynamic = 1u << 3;
  static constexpr std::uint32_t kUsageWriteOnly = 1u << 7;
  auto& c = counters();
  add(c.d3d9BufferLockCalls);
  add(c.d3d9BufferLockNs, nanoseconds);
  updateMax(c.d3d9BufferLockMaxNs, nanoseconds);
  recordRing(c.d3d9BufferLockRing, nanoseconds);
  add(c.d3d9BufferLockBytes, bytes);
  const bool discard = (d3dFlags & kD3DLockDiscard) != 0;
  const bool noOverwrite = (d3dFlags & kD3DLockNoOverwrite) != 0;
  if (discard) add(c.d3d9BufferLockDiscard);
  if (noOverwrite) add(c.d3d9BufferLockNoOverwrite);
  if ((d3dFlags & kD3DLockReadOnly) != 0) add(c.d3d9BufferLockReadOnly);
  if (!discard && !noOverwrite) add(c.d3d9BufferLockPlain);
  if (fullResource) add(c.d3d9BufferLockFullResource);
  if (shadowCopy) {
    add(c.d3d9BufferLockShadow);
    add(c.d3d9BufferLockShadowBytes, bytes);
    add(c.d3d9BufferLockShadowAllocNs, shadowAllocNanoseconds);
    updateMax(c.d3d9BufferLockShadowAllocMaxNs, shadowAllocNanoseconds);
    recordRing(c.d3d9BufferLockShadowAllocRing, shadowAllocNanoseconds);
    add(c.d3d9BufferLockShadowCopyNs, shadowCopyNanoseconds);
    updateMax(c.d3d9BufferLockShadowCopyMaxNs, shadowCopyNanoseconds);
    recordRing(c.d3d9BufferLockShadowCopyRing, shadowCopyNanoseconds);
  }
  switch (pool) {
    case 0:
      add(c.d3d9BufferLockDefaultPool);
      break;
    case 1:
      add(c.d3d9BufferLockManagedPool);
      break;
    case 2:
      add(c.d3d9BufferLockSystemMemPool);
      break;
    case 3:
      add(c.d3d9BufferLockScratchPool);
      break;
    default:
      break;
  }
  if ((usage & kUsageDynamic) != 0) add(c.d3d9BufferLockDynamic);
  if ((usage & kUsageWriteOnly) != 0) add(c.d3d9BufferLockWriteOnly);
}

void countUniformVsConsts(std::size_t bytes) {
  add(counters().uniformVsConstsCalls);
  add(counters().uniformVsConstsBytes, static_cast<std::uint64_t>(bytes));
}

void countUniformPsConsts(std::size_t bytes) {
  add(counters().uniformPsConstsCalls);
  add(counters().uniformPsConstsBytes, static_cast<std::uint64_t>(bytes));
}

void countUniformFfpVs(std::size_t bytes) {
  add(counters().uniformFfpVsCalls);
  add(counters().uniformFfpVsBytes, static_cast<std::uint64_t>(bytes));
}

void countUniformFfpPs(std::size_t bytes) {
  add(counters().uniformFfpPsCalls);
  add(counters().uniformFfpPsBytes, static_cast<std::uint64_t>(bytes));
}

void countUniformVolatilePush() {
  add(counters().uniformVolatilePushes);
}

// R-BACK-5.7. Increment only on the discrete path (no hasUnifiedMemory).
// Apple Silicon must never call this. Bytes argument is the staging-copy
// size so a regression's volume is observable, not just its frequency.
void countManagedTextureUploadBlit(std::uint64_t bytes) {
  add(counters().managedTextureUploadBlitCount);
  add(counters().managedTextureUploadBlitBytes, bytes);
}

void countTexturePixelFormatViewSuppressedRt(std::uint64_t bytes) {
  add(counters().texturePixelFormatViewSuppressedRtCount);
  add(counters().texturePixelFormatViewSuppressedRtBytes, bytes);
}

// R-BACK-14.* — MTLHeap pooling counters.
void countHeapAlloc(std::uint64_t bytes) {
  add(counters().heapAllocCount);
  add(counters().heapBytesAllocated, bytes);
}
void countHeapInstance() { add(counters().heapInstanceCount); }
void countHeapDirectFallback() { add(counters().heapDirectFallbackCount); }
void countHeapFragmentationFailure() { add(counters().heapFragmentationFailureCount); }
void countHeapCompaction() { add(counters().heapCompactionCount); }
void countHeapAllocFailure() { add(counters().heapAllocFailureCount); }
void countUseHeap() { add(counters().useHeapCalls); }
void countUseResource() { add(counters().useResourceCalls); }

// R-BACK-13.* — Tile-Shader FFP counters.
void countTileFfpPass() { add(counters().tileFfpPassCount); }
void countPortableFfpPass() { add(counters().portableFfpPassCount); }
void countTileFfpFallbackPrecision() { add(counters().tileFfpFallbackPrecision); }
void countTileFfpFallbackUnsupportedState() {
  add(counters().tileFfpFallbackUnsupportedState);
}
void countTileFfpFallbackGpuFamily() { add(counters().tileFfpFallbackGpuFamily); }
void countTileFfpFallbackMidPassIneligible() {
  add(counters().tileFfpFallbackMidPassIneligible);
}
void countTileFfpMidPassResplit() { add(counters().tileFfpMidPassResplitCount); }

// R-BACK-12.22~12.26 — Stage 2 Argbuf hybrid.
void countArgbufHybridEncoder() { add(counters().argbufHybridEncoderCount); }
void countStage1Encoder() { add(counters().stage1EncoderCount); }
void countArgbufHybridFallback() { add(counters().argbufHybridFallbackCount); }
void countArgbufHybridBytes(std::uint64_t bytes) {
  add(counters().argbufHybridBytesPerEncoder, bytes);
}
void countStage1Bytes(std::uint64_t bytes) {
  add(counters().stage1BytesPerEncoder, bytes);
}

// R-BACK-3.7 / 3.8 / 4.8 — MTLBinaryArchive prewarming + cross-process.
void countPrewarmEntriesLoaded(std::uint64_t entries) {
  add(counters().prewarmEntriesLoaded, entries);
}
void countPrewarmLoadCpuTime(std::uint64_t nanoseconds) {
  add(counters().prewarmLoadCpuNs, nanoseconds);
}
void countPrewarmFailureCorrupt() {
  add(counters().prewarmFailureCorrupt);
}
void countPrewarmFailureSchema() {
  add(counters().prewarmFailureSchema);
}
void countPrewarmFailureLockBusy() {
  add(counters().prewarmFailureLockBusy);
}
void countPrewarmFailureMissing() {
  add(counters().prewarmFailureMissing);
}
void countColdCompileAfterWarm() {
  add(counters().coldCompileCountAfterWarm);
}
void countArchiveBytes(std::uint64_t bytes) {
  counters().archiveBytes.store(bytes, std::memory_order_relaxed);
}
void countPrewarmDemotedBySize() {
  add(counters().prewarmDemotedBySize);
}
void countPrewarmAsyncCompletionCpuTime(std::uint64_t nanoseconds) {
  add(counters().prewarmAsyncCompletionCpuNs, nanoseconds);
}
void countPrewarmMilestoneSave() {
  add(counters().prewarmMilestoneSaveCount);
}
void countPrewarmSaveSkippedDebugEnv() {
  add(counters().prewarmSaveSkippedDebugEnvCount);
}

// D3DBC bytecode safe-rejection — see dxmt9_perf_counters.hpp for the
// reject-bucket taxonomy. These bumps fire from the shader decoder's
// catch site (translateD3DBytecodeToSpirv in dxmt9_shader_decoder.cpp)
// after a malformed-input check converts a parser failure into an
// empty SpirvModule. None of these counters allocate; logging is at
// trace level and gated by dxmt9::util::shouldLog.
void countShaderDecoderRejectTruncated() {
  add(counters().shaderDecoderRejectTruncated);
}
void countShaderDecoderRejectUnsupportedVersion() {
  add(counters().shaderDecoderRejectUnsupportedVersion);
}
void countShaderDecoderRejectOobRegister() {
  add(counters().shaderDecoderRejectOobRegister);
}
void countShaderDecoderRejectMissingEnd() {
  add(counters().shaderDecoderRejectMissingEnd);
}
void countShaderDecoderRejectInvalidOpcode() {
  add(counters().shaderDecoderRejectInvalidOpcode);
}
void countShaderDecoderRejectTempFloat16Unsupported() {
  add(counters().shaderDecoderRejectTempFloat16Unsupported);
}
void countShaderDecoderRejectLabelUnsupported() {
  add(counters().shaderDecoderRejectLabelUnsupported);
}
void countShaderDecoderRejectDeclUsageUnsupported() {
  add(counters().shaderDecoderRejectDeclUsageUnsupported);
}
void countShaderDecoderRejectDeclMethodUnsupported() {
  add(counters().shaderDecoderRejectDeclMethodUnsupported);
}

namespace test {
ShaderDecoderRejectSnapshot snapshotShaderDecoderRejects() {
  const Counters& c = counters();
  return ShaderDecoderRejectSnapshot{
      load(c.shaderDecoderRejectTruncated),
      load(c.shaderDecoderRejectUnsupportedVersion),
      load(c.shaderDecoderRejectOobRegister),
      load(c.shaderDecoderRejectMissingEnd),
      load(c.shaderDecoderRejectInvalidOpcode),
      load(c.shaderDecoderRejectTempFloat16Unsupported),
      load(c.shaderDecoderRejectLabelUnsupported),
      load(c.shaderDecoderRejectDeclUsageUnsupported),
      load(c.shaderDecoderRejectDeclMethodUnsupported),
  };
}

FramegraphObserveSnapshot snapshotFramegraphObserve() {
  const Counters& c = counters();
  return FramegraphObserveSnapshot{
      load(c.framegraphPassesBuilt),
      load(c.framegraphPassesCoalesced),
      load(c.framegraphPassesDead),
      load(c.framegraphResourcesMemoryless),
      load(c.framegraphDagDumpsWritten),
  };
}

FramegraphActiveRenderSeedSnapshot snapshotFramegraphActiveRenderSeed() {
  const Counters& c = counters();
  return FramegraphActiveRenderSeedSnapshot{
      load(c.framegraphActiveRenderSnapshotAbsent),
      load(c.framegraphActiveRenderSnapshotIncomplete),
      load(c.framegraphActiveRenderSeedApplyApplied),
      load(c.framegraphActiveRenderSeedApplyInvalid),
      load(c.framegraphActiveRenderSeedApplyIncomplete),
      load(c.framegraphActiveRenderSeedApplyOverflow),
      load(c.framegraphActiveRenderSeedAppliedButUnmerged),
      load(c.framegraphActiveRenderSeedPassCoalesceBlockedCycle),
      load(c.framegraphActiveRenderSeedPassCoalesceSecondNonDraw),
      load(c.framegraphActiveRenderSeedMovedHeadProved),
      load(c.framegraphActiveRenderSeedFallbackMovedHeadUnproved),
      load(c.framegraphActiveRenderSeedFallbackInvalidPlan),
      load(c.framegraphActiveRenderSeedFallbackLiveSetMismatch),
      load(c.framegraphActiveRenderSeedFallbackDuplicateCommand),
      load(c.framegraphActiveRenderSeedReplayActivated),
  };
}

FramegraphSourceLocalPassCoalesceSnapshot
snapshotFramegraphSourceLocalPassCoalesce() {
  const Counters& c = counters();
  return FramegraphSourceLocalPassCoalesceSnapshot{
      load(c.framegraphSourceLocalReturnCandidates),
      load(c.framegraphSourceLocalReturnMerged),
      load(c.framegraphSourceLocalReturnBlockedCycle),
      load(c.framegraphSourceLocalReturnSecondNonDraw),
      load(c.framegraphSourceLocalReturnNonRenderIntervener),
      load(c.framegraphSourceLocalReturnMissingInvariant),
      load(c.framegraphSourceLocalReturnDependencyKept),
      load(c.framegraphSourceLocalReturnMoveBefore),
      load(c.framegraphSourceLocalReturnMoveAfter),
      load(c.framegraphSourceLocalReturnNonDrawIntervener),
      load(c.framegraphSourceLocalReturnSemanticIntervener),
      load(c.framegraphSourceLocalReturnCommandlessIntervener),
      load(c.framegraphSourceLocalReturnCommandless),
      load(c.framegraphSourceLocalReturnLegacyCandidates),
      load(c.framegraphSourceLocalReturnArenaCandidates),
      load(c.framegraphSourceLocalReturnUnknownCandidates),
      load(c.framegraphSourceLocalReturnIdentityKnownCandidates),
      load(c.framegraphSourceLocalReturnIdentityMissingCandidates),
  };
}

FramegraphSourceLocalReplayOutcomeSnapshot
snapshotFramegraphSourceLocalReplayOutcome() {
  const Counters& c = counters();
  return FramegraphSourceLocalReplayOutcomeSnapshot{
      .frontierRollback = {
          load(c.framegraphSourceLocalReturnFrontierRollbackSources),
          load(c.framegraphSourceLocalReturnFrontierRollbackCandidates),
          load(c.framegraphSourceLocalReturnFrontierRollbackMerged),
      },
      .frontierRollbackInvalidPlan = {
          load(c.framegraphSourceLocalReturnFrontierRollbackInvalidPlanSources),
          load(c.framegraphSourceLocalReturnFrontierRollbackInvalidPlanCandidates),
          load(c.framegraphSourceLocalReturnFrontierRollbackInvalidPlanMerged),
      },
      .frontierRollbackLiveSetMismatch = {
          load(c.framegraphSourceLocalReturnFrontierRollbackLiveSetMismatchSources),
          load(c.framegraphSourceLocalReturnFrontierRollbackLiveSetMismatchCandidates),
          load(c.framegraphSourceLocalReturnFrontierRollbackLiveSetMismatchMerged),
      },
      .frontierRollbackDuplicateCommand = {
          load(c.framegraphSourceLocalReturnFrontierRollbackDuplicateCommandSources),
          load(c.framegraphSourceLocalReturnFrontierRollbackDuplicateCommandCandidates),
          load(c.framegraphSourceLocalReturnFrontierRollbackDuplicateCommandMerged),
      },
      .frontierRollbackMovedHeadUnproved = {
          load(c.framegraphSourceLocalReturnFrontierRollbackMovedHeadUnprovedSources),
          load(c.framegraphSourceLocalReturnFrontierRollbackMovedHeadUnprovedCandidates),
          load(c.framegraphSourceLocalReturnFrontierRollbackMovedHeadUnprovedMerged),
      },
      .finalInvalid = {
          load(c.framegraphSourceLocalReturnFinalInvalidSources),
          load(c.framegraphSourceLocalReturnFinalInvalidCandidates),
          load(c.framegraphSourceLocalReturnFinalInvalidMerged),
      },
      .finalNaturalOrder = {
          load(c.framegraphSourceLocalReturnFinalNaturalOrderSources),
          load(c.framegraphSourceLocalReturnFinalNaturalOrderCandidates),
          load(c.framegraphSourceLocalReturnFinalNaturalOrderMerged),
      },
      .finalReorderedActivated = {
          load(c.framegraphSourceLocalReturnFinalReorderedActivatedSources),
          load(c.framegraphSourceLocalReturnFinalReorderedActivatedCandidates),
          load(c.framegraphSourceLocalReturnFinalReorderedActivatedMerged),
      },
  };
}

CpuReadyMultiSourceSeedNaturalDistanceSnapshot
snapshotCpuReadyMultiSourceSeedNaturalDistance() {
  const Counters& c = counters();
  return CpuReadyMultiSourceSeedNaturalDistanceSnapshot{
      load(c.cpuReadyMultiSourcePlannerSeedNaturalMatchDistanceMissing),
      load(c.cpuReadyMultiSourcePlannerSeedNaturalMatchDistance1),
      load(c.cpuReadyMultiSourcePlannerSeedNaturalMatchDistanceGt1),
  };
}

CpuReadyMultiSourceSeedNaturalAttributionSnapshot
snapshotCpuReadyMultiSourceSeedNaturalAttribution() {
  const Counters& c = counters();
  return CpuReadyMultiSourceSeedNaturalAttributionSnapshot{
      load(c.cpuReadyMultiSourcePlannerSeedNaturalMergeOperations),
      load(c.cpuReadyMultiSourcePlannerSeedNaturalMergeDistanceTotal),
      load(c.cpuReadyMultiSourcePlannerSeedNaturalMergeDistanceMax),
      load(c.cpuReadyMultiSourcePlannerSeedNaturalCommandBefore),
      load(c.cpuReadyMultiSourcePlannerSeedNaturalCommandAfter),
      load(c.cpuReadyMultiSourcePlannerSeedNaturalEmptyIntervening),
      load(c.cpuReadyMultiSourcePlannerSeedNaturalShapeAdjacent),
      load(c.cpuReadyMultiSourcePlannerSeedNaturalShapeDependencyKept),
      load(c.cpuReadyMultiSourcePlannerSeedNaturalShapeCommandless),
      load(c.cpuReadyMultiSourcePlannerSeedNaturalShapeMultiMerge),
      load(c.cpuReadyMultiSourcePlannerSeedNaturalShapeMissing),
  };
}

CpuReadyMultiSourceSourceLocalFallbackSnapshot
snapshotCpuReadyMultiSourceSourceLocalFallback() {
  const Counters& c = counters();
  return CpuReadyMultiSourceSourceLocalFallbackSnapshot{
      load(c.cpuReadyMultiSourceNaturalFallbackWindowsStarted),
      load(c.cpuReadyMultiSourceNaturalFallbackWindowsCompleted),
      load(c.cpuReadyMultiSourceNaturalFallbackSources),
      load(c.cpuReadyMultiSourcePermutationFallbackWindowsStarted),
      load(c.cpuReadyMultiSourcePermutationFallbackWindowsCompleted),
      load(c.cpuReadyMultiSourcePermutationFallbackSources),
  };
}

RenderPassNaturalFallbackAttributionSnapshot
snapshotRenderPassNaturalFallbackAttribution() {
  const Counters& c = counters();
  return RenderPassNaturalFallbackAttributionSnapshot{
      load(c.renderPassNaturalFallbackBegin),
      load(c.renderPassNaturalFallbackSameWindowReentryDistance1),
      load(c.renderPassNaturalFallbackSameWindowReentryDistance2),
      load(c.renderPassNaturalFallbackSameWindowReentryDistance3To4),
      load(c.renderPassNaturalFallbackCrossWindowReentryDistance1),
      load(c.renderPassNaturalFallbackCrossWindowReentryDistance2),
      load(c.renderPassNaturalFallbackCrossWindowReentryDistance3To4),
      load(c.activeSeedMergeTicketIssued),
      load(c.activeSeedMergeTicketMatched),
      load(c.activeSeedMergeTicketContinued),
      load(c.activeSeedMergeTicketMismatch),
      load(c.activeSeedMergeTicketUnconsumed),
      load(c.activeSeedMergeWitnessOverflow),
      load(c.activeSeedMergeWitnessMismatch),
      load(c.activeSeedInstanceUnavailable),
      load(c.activeSeedInstanceStale),
      load(c.renderPassActiveSeedBridgeReentryDistance1),
      load(c.renderPassActiveSeedBridgeReentryDistance2),
      load(c.renderPassActiveSeedBridgeReentryDistance3To4),
  };
}

RenderPassCloseAttributionSnapshot snapshotRenderPassCloseAttribution() {
  const Counters& c = counters();
  return RenderPassCloseAttributionSnapshot{
      load(c.renderPassFinalCloseSessionCap),
      load(c.renderPassFinalCloseIndependent),
      load(c.renderPassFinalCloseInitializer),
      load(c.renderPassFinalCloseProducerWait),
      load(c.renderPassFinalCloseDrain),
      load(c.renderPassFinalCloseFailOther),
      load(c.renderPassCloseAdjacentSessionCap),
      load(c.renderPassCloseAdjacentIndependent),
      load(c.renderPassCloseAdjacentInitializer),
      load(c.renderPassCloseAdjacentProducerWait),
      load(c.renderPassCloseAdjacentDrain),
      load(c.renderPassCloseAdjacentFailOther),
      load(c.renderPassCloseLedgerRecorded),
      load(c.renderPassCloseLedgerMissing),
      load(c.renderPassCloseLedgerTerminalAdjacent),
      load(c.renderPassCloseLedgerTerminalNonAdjacent),
      load(c.renderPassCloseLedgerTerminalNotReopenedBeforePresent),
      load(c.renderPassNaturalShortCrossCloseMatched),
      load(c.renderPassNaturalShortCrossCloseMissing),
      load(c.renderPassFinalCloseLedgerRecorded),
      load(c.renderPassFinalCloseLedgerMissing),
      load(c.renderPassFinalCloseLedgerTerminalAdjacent),
      load(c.renderPassFinalCloseLedgerTerminalNonAdjacent),
      load(c.renderPassFinalCloseLedgerTerminalNotReopenedBeforePresent),
  };
}

RenderPassShortReentryAttributionSnapshot
snapshotRenderPassShortReentryAttribution() {
  const Counters& c = counters();
  return RenderPassShortReentryAttributionSnapshot{
      .distance1Disposition = {
          load(c.renderPassShortReentryD1Ordinary),
          load(c.renderPassShortReentryD1NaturalSame),
          load(c.renderPassShortReentryD1NaturalCross),
          load(c.renderPassShortReentryD1Planned),
          load(c.renderPassShortReentryD1EligibilityPresent),
          load(c.renderPassShortReentryD1EligibilityOther),
          load(c.renderPassShortReentryD1PermutationRejected),
          load(c.renderPassShortReentryD1MixedInvalid),
      },
      .distance2Disposition = {
          load(c.renderPassShortReentryD2Ordinary),
          load(c.renderPassShortReentryD2NaturalSame),
          load(c.renderPassShortReentryD2NaturalCross),
          load(c.renderPassShortReentryD2Planned),
          load(c.renderPassShortReentryD2EligibilityPresent),
          load(c.renderPassShortReentryD2EligibilityOther),
          load(c.renderPassShortReentryD2PermutationRejected),
          load(c.renderPassShortReentryD2MixedInvalid),
      },
      .distance1SourceShape = {
          load(c.renderPassShortReentryD1SourceAllSame),
          load(c.renderPassShortReentryD1SourcePriorInterveningSameCurrentNewer),
          load(c.renderPassShortReentryD1SourcePriorOlderInterveningCurrentSame),
          load(c.renderPassShortReentryD1SourceMixedInvalid),
      },
      .distance2SourceShape = {
          load(c.renderPassShortReentryD2SourceAllSame),
          load(c.renderPassShortReentryD2SourcePriorInterveningSameCurrentNewer),
          load(c.renderPassShortReentryD2SourcePriorOlderInterveningCurrentSame),
          load(c.renderPassShortReentryD2SourceMixedInvalid),
      },
      .priorCloseReason = {
          load(c.renderPassShortReentryCloseFinal),
          load(c.renderPassShortReentryCloseRtChange),
          load(c.renderPassShortReentryCloseHazard),
          load(c.renderPassShortReentryCloseClear),
          load(c.renderPassShortReentryCloseSurfaceCopy),
          load(c.renderPassShortReentryCloseStretchRect),
          load(c.renderPassShortReentryCloseReadback),
          load(c.renderPassShortReentryCloseColorFill),
          load(c.renderPassShortReentryClosePresent),
          load(c.renderPassShortReentryClosePresentAcquire),
          load(c.renderPassShortReentryCloseTile),
          load(c.renderPassShortReentryCloseOrdered),
      },
      .priorCloseMissing = load(c.renderPassShortReentryCloseMissing),
      .clearOpenTargetCount =
          load(c.renderPassShortReentryClearOpenTargetCount),
      .clearOpenTargetPriorStoreBytes =
          load(c.renderPassShortReentryClearOpenTargetPriorStoreBytes),
      .clearOpenTargetCurrentLoadBytes =
          load(c.renderPassShortReentryClearOpenTargetCurrentLoadBytes),
      .clearOpenNaturalCrossCount =
          load(c.renderPassShortReentryClearOpenNaturalCrossCount),
      .clearOpenNaturalCrossPriorStoreBytes =
          load(c.renderPassShortReentryClearOpenNaturalCrossPriorStoreBytes),
      .clearOpenNaturalCrossCurrentLoadBytes =
          load(c.renderPassShortReentryClearOpenNaturalCrossCurrentLoadBytes),
  };
}

RenderPassStoreAccountingSnapshot snapshotRenderPassStoreAccounting() {
  const Counters& c = counters();
  return RenderPassStoreAccountingSnapshot{
      load(c.renderPassStoreActionStore),
      load(c.renderPassStoreActionDontCare),
      load(c.renderPassStoreActionDepthStore),
      load(c.renderPassStoreActionDepthDontCare),
      load(c.renderPassStoreActionStencilStore),
      load(c.renderPassStoreActionStencilDontCare),
      load(c.renderPassTilePreservationBytes),
  };
}
}  // namespace test

// WMTLoadAction: DontCare=0, Load=1, Clear=2 (winemetal.h).
void countRenderPassLoadActionColor(std::uint32_t action) {
  auto& c = counters();
  switch (action) {
    case 0: add(c.renderPassLoadActionDontCare); break;
    case 1: add(c.renderPassLoadActionLoad); break;
    case 2: add(c.renderPassLoadActionClear); break;
    default: break;
  }
}

void countRenderPassLoadActionDepth(std::uint32_t action) {
  auto& c = counters();
  switch (action) {
    case 0: add(c.renderPassLoadActionDepthDontCare); break;
    case 1: add(c.renderPassLoadActionDepthLoad); break;
    case 2: add(c.renderPassLoadActionDepthClear); break;
    default: break;
  }
}

void countRenderPassLoadActionStencil(std::uint32_t action) {
  auto& c = counters();
  switch (action) {
    case 0: add(c.renderPassLoadActionStencilDontCare); break;
    case 1: add(c.renderPassLoadActionStencilLoad); break;
    case 2: add(c.renderPassLoadActionStencilClear); break;
    default: break;
  }
}

// WMTStoreAction: DontCare=0, Store=1, MultisampleResolve=2,
// StoreAndMultisampleResolve=3 (winemetal.h). The combined value bumps both
// store and resolve buckets so each attachment contributes the actions it
// actually performs.
void countRenderPassStoreActionColor(std::uint32_t action) {
  auto& c = counters();
  switch (action) {
    case 0: add(c.renderPassStoreActionDontCare); break;
    case 1: add(c.renderPassStoreActionStore); break;
    case 2: add(c.renderPassStoreActionResolve); break;
    case 3:
      add(c.renderPassStoreActionStore);
      add(c.renderPassStoreActionResolve);
      break;
    default: break;
  }
}

void countRenderPassStoreActionDepth(std::uint32_t action) {
  auto& c = counters();
  switch (action) {
    case 0: add(c.renderPassStoreActionDepthDontCare); break;
    case 1: add(c.renderPassStoreActionDepthStore); break;
    default: break;
  }
}

void countRenderPassStoreActionStencil(std::uint32_t action) {
  auto& c = counters();
  switch (action) {
    case 0: add(c.renderPassStoreActionStencilDontCare); break;
    case 1: add(c.renderPassStoreActionStencilStore); break;
    default: break;
  }
}

void countRenderPassTilePreservationBytes(std::uint64_t bytes) {
  add(counters().renderPassTilePreservationBytes, bytes);
}

void countRenderPassSameKeyAdjacent() {
  add(counters().renderPassSameKeyAdjacent);
}

void countRenderPassSameKeyReentry() {
  add(counters().renderPassSameKeyReentry);
}

void countRenderPassSameKeyReentryDistance(std::uint32_t interveningPasses) {
  auto& c = counters();
  if (interveningPasses <= 1) {
    add(c.renderPassSameKeyReentryDistance1);
  } else if (interveningPasses == 2) {
    add(c.renderPassSameKeyReentryDistance2);
  } else if (interveningPasses <= 4) {
    add(c.renderPassSameKeyReentryDistance3To4);
  } else if (interveningPasses <= 8) {
    add(c.renderPassSameKeyReentryDistance5To8);
  } else if (interveningPasses <= 16) {
    add(c.renderPassSameKeyReentryDistance9To16);
  } else {
    add(c.renderPassSameKeyReentryDistance17Plus);
  }
}

void countRenderPassSameKeyReentryDistance1Shape(bool sameColor,
                                                 bool sameDepth,
                                                 std::uint64_t bytes) {
  auto& c = counters();
  if (sameColor && sameDepth) {
    add(c.renderPassSameKeyReentryDistance1SampleChange);
    add(c.renderPassSameKeyReentryDistance1SampleChangeBytes, bytes);
  } else if (sameColor) {
    add(c.renderPassSameKeyReentryDistance1SameColor);
    add(c.renderPassSameKeyReentryDistance1SameColorBytes, bytes);
  } else if (sameDepth) {
    add(c.renderPassSameKeyReentryDistance1SameDepth);
    add(c.renderPassSameKeyReentryDistance1SameDepthBytes, bytes);
  } else {
    add(c.renderPassSameKeyReentryDistance1RtDepthChange);
    add(c.renderPassSameKeyReentryDistance1RtDepthChangeBytes, bytes);
  }
}

void countRenderPassSameKeyReentryPreservationBytes(std::uint64_t bytes) {
  add(counters().renderPassSameKeyReentryPreservationBytes, bytes);
}

void countRenderPassSameKeyReentryColorPreservationBytes(std::uint64_t bytes) {
  add(counters().renderPassSameKeyReentryColorPreservationBytes, bytes);
}

void countRenderPassSameKeyReentryDepthPreservationBytes(std::uint64_t bytes) {
  add(counters().renderPassSameKeyReentryDepthPreservationBytes, bytes);
}

void countRenderPassNaturalFallbackBegin() {
  add(counters().renderPassNaturalFallbackBegin);
}

void countRenderPassNaturalFallbackReentryDistance(
    std::uint32_t interveningPasses, bool sameWindow) {
  if (interveningPasses < 1u || interveningPasses > 4u) {
    return;
  }
  auto& c = counters();
  if (sameWindow) {
    if (interveningPasses == 1u) {
      add(c.renderPassNaturalFallbackSameWindowReentryDistance1);
    } else if (interveningPasses == 2u) {
      add(c.renderPassNaturalFallbackSameWindowReentryDistance2);
    } else if (interveningPasses <= 4u) {
      add(c.renderPassNaturalFallbackSameWindowReentryDistance3To4);
    }
    return;
  }
  if (interveningPasses == 1u) {
    add(c.renderPassNaturalFallbackCrossWindowReentryDistance1);
  } else if (interveningPasses == 2u) {
    add(c.renderPassNaturalFallbackCrossWindowReentryDistance2);
  } else if (interveningPasses <= 4u) {
    add(c.renderPassNaturalFallbackCrossWindowReentryDistance3To4);
  }
}

void countActiveSeedMergeTicketIssued(std::uint64_t count) {
  add(counters().activeSeedMergeTicketIssued, count);
}

void countActiveSeedMergeTicketMatched(std::uint64_t count) {
  add(counters().activeSeedMergeTicketMatched, count);
}

void countActiveSeedMergeTicketContinued(std::uint64_t count) {
  add(counters().activeSeedMergeTicketContinued, count);
}

void countActiveSeedMergeTicketMismatch(std::uint64_t count) {
  add(counters().activeSeedMergeTicketMismatch, count);
}

void countActiveSeedMergeTicketUnconsumed(std::uint64_t count) {
  add(counters().activeSeedMergeTicketUnconsumed, count);
}

void countActiveSeedMergeWitnessOverflow(std::uint64_t count) {
  add(counters().activeSeedMergeWitnessOverflow, count);
}

void countActiveSeedMergeWitnessMismatch(std::uint64_t count) {
  add(counters().activeSeedMergeWitnessMismatch, count);
}

void countActiveSeedInstanceUnavailable(std::uint64_t count) {
  add(counters().activeSeedInstanceUnavailable, count);
}

void countActiveSeedInstanceStale(std::uint64_t count) {
  add(counters().activeSeedInstanceStale, count);
}

void countRenderPassActiveSeedBridgeReentryDistance(
    std::uint32_t interveningPasses) {
  auto& c = counters();
  if (interveningPasses == 1u) {
    add(c.renderPassActiveSeedBridgeReentryDistance1);
  } else if (interveningPasses == 2u) {
    add(c.renderPassActiveSeedBridgeReentryDistance2);
  } else if (interveningPasses >= 3u && interveningPasses <= 4u) {
    add(c.renderPassActiveSeedBridgeReentryDistance3To4);
  }
}

void countRenderPassFinalCloseCause(
    encoders::SessionFinalizeCause cause) {
  auto& c = counters();
  switch (cause) {
  case encoders::SessionFinalizeCause::SessionCap:
    add(c.renderPassFinalCloseSessionCap);
    break;
  case encoders::SessionFinalizeCause::Independent:
    add(c.renderPassFinalCloseIndependent);
    break;
  case encoders::SessionFinalizeCause::Initializer:
    add(c.renderPassFinalCloseInitializer);
    break;
  case encoders::SessionFinalizeCause::ProducerWait:
    add(c.renderPassFinalCloseProducerWait);
    break;
  case encoders::SessionFinalizeCause::Drain:
    add(c.renderPassFinalCloseDrain);
    break;
  case encoders::SessionFinalizeCause::FailOrOther:
    add(c.renderPassFinalCloseFailOther);
    break;
  }
}

void countRenderPassCloseLedgerAdjacentCause(
    encoders::SessionFinalizeCause cause) {
  auto& c = counters();
  switch (cause) {
  case encoders::SessionFinalizeCause::SessionCap:
    add(c.renderPassCloseAdjacentSessionCap);
    break;
  case encoders::SessionFinalizeCause::Independent:
    add(c.renderPassCloseAdjacentIndependent);
    break;
  case encoders::SessionFinalizeCause::Initializer:
    add(c.renderPassCloseAdjacentInitializer);
    break;
  case encoders::SessionFinalizeCause::ProducerWait:
    add(c.renderPassCloseAdjacentProducerWait);
    break;
  case encoders::SessionFinalizeCause::Drain:
    add(c.renderPassCloseAdjacentDrain);
    break;
  case encoders::SessionFinalizeCause::FailOrOther:
    add(c.renderPassCloseAdjacentFailOther);
    break;
  }
}

void countRenderPassNaturalShortCrossPriorClose(
    EncoderSplitReason reason) {
  auto& c = counters();
  add(c.renderPassNaturalShortCrossCloseMatched);
  switch (reason) {
  case EncoderSplitReason::Final:
    add(c.renderPassNaturalShortCrossCloseFinal);
    break;
  case EncoderSplitReason::RenderTargetChange:
    add(c.renderPassNaturalShortCrossCloseRtChange);
    break;
  case EncoderSplitReason::Hazard:
    add(c.renderPassNaturalShortCrossCloseHazard);
    break;
  case EncoderSplitReason::ClearBarrier:
    add(c.renderPassNaturalShortCrossCloseClear);
    break;
  case EncoderSplitReason::SurfaceCopy:
    add(c.renderPassNaturalShortCrossCloseSurfaceCopy);
    break;
  case EncoderSplitReason::StretchRect:
    add(c.renderPassNaturalShortCrossCloseStretchRect);
    break;
  case EncoderSplitReason::Readback:
    add(c.renderPassNaturalShortCrossCloseReadback);
    break;
  case EncoderSplitReason::ColorFill:
    add(c.renderPassNaturalShortCrossCloseColorFill);
    break;
  case EncoderSplitReason::Present:
    add(c.renderPassNaturalShortCrossClosePresent);
    break;
  case EncoderSplitReason::PresentAcquire:
    add(c.renderPassNaturalShortCrossClosePresentAcquire);
    break;
  case EncoderSplitReason::TileMidPassIneligible:
    add(c.renderPassNaturalShortCrossCloseTile);
    break;
  case EncoderSplitReason::OrderedControl:
    add(c.renderPassNaturalShortCrossCloseOrdered);
    break;
  }
}

void countRenderPassNaturalShortCrossPriorCloseMissing(
    std::uint64_t count) {
  add(counters().renderPassNaturalShortCrossCloseMissing, count);
}

void countRenderPassShortReentryDisposition(
    std::uint32_t interveningPasses,
    std::uint8_t disposition) {
  if (interveningPasses < 1u || interveningPasses > 2u || disposition > 7u) {
    return;
  }
  auto& c = counters();
  std::atomic<std::uint64_t>* bucket = nullptr;
  if (interveningPasses == 1u) {
    switch (disposition) {
    case 0: bucket = &c.renderPassShortReentryD1Ordinary; break;
    case 1: bucket = &c.renderPassShortReentryD1NaturalSame; break;
    case 2: bucket = &c.renderPassShortReentryD1NaturalCross; break;
    case 3: bucket = &c.renderPassShortReentryD1Planned; break;
    case 4: bucket = &c.renderPassShortReentryD1EligibilityPresent; break;
    case 5: bucket = &c.renderPassShortReentryD1EligibilityOther; break;
    case 6: bucket = &c.renderPassShortReentryD1PermutationRejected; break;
    case 7: bucket = &c.renderPassShortReentryD1MixedInvalid; break;
    }
  } else {
    switch (disposition) {
    case 0: bucket = &c.renderPassShortReentryD2Ordinary; break;
    case 1: bucket = &c.renderPassShortReentryD2NaturalSame; break;
    case 2: bucket = &c.renderPassShortReentryD2NaturalCross; break;
    case 3: bucket = &c.renderPassShortReentryD2Planned; break;
    case 4: bucket = &c.renderPassShortReentryD2EligibilityPresent; break;
    case 5: bucket = &c.renderPassShortReentryD2EligibilityOther; break;
    case 6: bucket = &c.renderPassShortReentryD2PermutationRejected; break;
    case 7: bucket = &c.renderPassShortReentryD2MixedInvalid; break;
    }
  }
  if (bucket) {
    add(*bucket);
  }
}

void countRenderPassShortReentrySourceShape(
    std::uint32_t interveningPasses,
    std::uint8_t sourceShape) {
  if (interveningPasses < 1u || interveningPasses > 2u || sourceShape > 3u) {
    return;
  }
  auto& c = counters();
  std::atomic<std::uint64_t>* bucket = nullptr;
  if (interveningPasses == 1u) {
    switch (sourceShape) {
    case 0: bucket = &c.renderPassShortReentryD1SourceAllSame; break;
    case 1:
      bucket =
          &c.renderPassShortReentryD1SourcePriorInterveningSameCurrentNewer;
      break;
    case 2:
      bucket =
          &c.renderPassShortReentryD1SourcePriorOlderInterveningCurrentSame;
      break;
    case 3: bucket = &c.renderPassShortReentryD1SourceMixedInvalid; break;
    }
  } else {
    switch (sourceShape) {
    case 0: bucket = &c.renderPassShortReentryD2SourceAllSame; break;
    case 1:
      bucket =
          &c.renderPassShortReentryD2SourcePriorInterveningSameCurrentNewer;
      break;
    case 2:
      bucket =
          &c.renderPassShortReentryD2SourcePriorOlderInterveningCurrentSame;
      break;
    case 3: bucket = &c.renderPassShortReentryD2SourceMixedInvalid; break;
    }
  }
  if (bucket) {
    add(*bucket);
  }
}

void countRenderPassShortReentryPriorClose(EncoderSplitReason reason) {
  auto& c = counters();
  switch (reason) {
  case EncoderSplitReason::Final: add(c.renderPassShortReentryCloseFinal); break;
  case EncoderSplitReason::RenderTargetChange:
    add(c.renderPassShortReentryCloseRtChange); break;
  case EncoderSplitReason::Hazard: add(c.renderPassShortReentryCloseHazard); break;
  case EncoderSplitReason::ClearBarrier:
    add(c.renderPassShortReentryCloseClear); break;
  case EncoderSplitReason::SurfaceCopy:
    add(c.renderPassShortReentryCloseSurfaceCopy); break;
  case EncoderSplitReason::StretchRect:
    add(c.renderPassShortReentryCloseStretchRect); break;
  case EncoderSplitReason::Readback:
    add(c.renderPassShortReentryCloseReadback); break;
  case EncoderSplitReason::ColorFill:
    add(c.renderPassShortReentryCloseColorFill); break;
  case EncoderSplitReason::Present:
    add(c.renderPassShortReentryClosePresent); break;
  case EncoderSplitReason::PresentAcquire:
    add(c.renderPassShortReentryClosePresentAcquire); break;
  case EncoderSplitReason::TileMidPassIneligible:
    add(c.renderPassShortReentryCloseTile); break;
  case EncoderSplitReason::OrderedControl:
    add(c.renderPassShortReentryCloseOrdered); break;
  }
}

void countRenderPassShortReentryPriorCloseMissing() {
  add(counters().renderPassShortReentryCloseMissing);
}

void countRenderPassShortReentryClearOpenTarget(
    bool naturalCross,
    std::uint64_t priorStoreBytes,
    std::uint64_t currentLoadBytes) {
  auto& c = counters();
  add(c.renderPassShortReentryClearOpenTargetCount);
  add(c.renderPassShortReentryClearOpenTargetPriorStoreBytes,
      priorStoreBytes);
  add(c.renderPassShortReentryClearOpenTargetCurrentLoadBytes,
      currentLoadBytes);
  if (!naturalCross) {
    return;
  }
  add(c.renderPassShortReentryClearOpenNaturalCrossCount);
  add(c.renderPassShortReentryClearOpenNaturalCrossPriorStoreBytes,
      priorStoreBytes);
  add(c.renderPassShortReentryClearOpenNaturalCrossCurrentLoadBytes,
      currentLoadBytes);
}

void countRenderPassCloseLedgerRecorded(std::uint64_t count) {
  add(counters().renderPassCloseLedgerRecorded, count);
}

void countRenderPassCloseLedgerMissing(std::uint64_t count) {
  add(counters().renderPassCloseLedgerMissing, count);
}

void countRenderPassCloseLedgerTerminalAdjacent(std::uint64_t count) {
  add(counters().renderPassCloseLedgerTerminalAdjacent, count);
}

void countRenderPassCloseLedgerTerminalNonAdjacent(std::uint64_t count) {
  add(counters().renderPassCloseLedgerTerminalNonAdjacent, count);
}

void countRenderPassCloseLedgerTerminalNotReopenedBeforePresent(
    std::uint64_t count) {
  add(counters().renderPassCloseLedgerTerminalNotReopenedBeforePresent,
      count);
}

void countRenderPassFinalCloseLedgerRecorded(std::uint64_t count) {
  add(counters().renderPassFinalCloseLedgerRecorded, count);
}

void countRenderPassFinalCloseLedgerMissing(std::uint64_t count) {
  add(counters().renderPassFinalCloseLedgerMissing, count);
}

void countRenderPassFinalCloseLedgerTerminalAdjacent(std::uint64_t count) {
  add(counters().renderPassFinalCloseLedgerTerminalAdjacent, count);
}

void countRenderPassFinalCloseLedgerTerminalNonAdjacent(
    std::uint64_t count) {
  add(counters().renderPassFinalCloseLedgerTerminalNonAdjacent, count);
}

void countRenderPassFinalCloseLedgerTerminalNotReopenedBeforePresent(
    std::uint64_t count) {
  add(counters().renderPassFinalCloseLedgerTerminalNotReopenedBeforePresent,
      count);
}

void countRenderPassTransitionRtChangeSameDepth() {
  add(counters().renderPassTransitionRtChangeSameDepth);
}

void countRenderPassTransitionSameRtDepthChange() {
  add(counters().renderPassTransitionSameRtDepthChange);
}

void countRenderPassTransitionRtDepthChange() {
  add(counters().renderPassTransitionRtDepthChange);
}

void countRenderPassColorStoreProof(RenderPassColorStoreProof proof) {
  auto& c = counters();
  switch (proof) {
  case RenderPassColorStoreProof::AllowNextClear:
    add(c.renderPassColorProofAllowNextClear);
    break;
  case RenderPassColorStoreProof::AllowDeadNoPresent:
    add(c.renderPassColorProofAllowDeadNoPresent);
    break;
  case RenderPassColorStoreProof::BlockNullColor:
    add(c.renderPassColorProofBlockNullColor);
    break;
  case RenderPassColorStoreProof::BlockNoLookahead:
    add(c.renderPassColorProofBlockNoLookahead);
    break;
  case RenderPassColorStoreProof::BlockDrawTarget:
    add(c.renderPassColorProofBlockDrawTarget);
    break;
  case RenderPassColorStoreProof::BlockTextureSample:
    add(c.renderPassColorProofBlockTextureSample);
    break;
  case RenderPassColorStoreProof::BlockSurfaceCopy:
    add(c.renderPassColorProofBlockSurfaceCopy);
    break;
  case RenderPassColorStoreProof::BlockStretchRect:
    add(c.renderPassColorProofBlockStretchRect);
    break;
  case RenderPassColorStoreProof::BlockReadback:
    add(c.renderPassColorProofBlockReadback);
    break;
  case RenderPassColorStoreProof::BlockColorFill:
    add(c.renderPassColorProofBlockColorFill);
    break;
  case RenderPassColorStoreProof::BlockMsaaResolve:
    add(c.renderPassColorProofBlockMsaaResolve);
    break;
  case RenderPassColorStoreProof::BlockPresent:
    add(c.renderPassColorProofBlockPresent);
    break;
  case RenderPassColorStoreProof::BlockDeadNoPresentDisabled:
    add(c.renderPassColorProofBlockDeadNoPresentDisabled);
    break;
  case RenderPassColorStoreProof::BlockClearMismatch:
    add(c.renderPassColorProofBlockClearMismatch);
    break;
  }
}

void countRenderPassDepthStoreProof(RenderPassDepthStoreProof proof) {
  auto& c = counters();
  switch (proof) {
  case RenderPassDepthStoreProof::AllowNextClear:
    add(c.renderPassDepthProofAllowNextClear);
    break;
  case RenderPassDepthStoreProof::AllowDeadNoPresent:
    add(c.renderPassDepthProofAllowDeadNoPresent);
    break;
  case RenderPassDepthStoreProof::BlockNullDepth:
    add(c.renderPassDepthProofBlockNullDepth);
    break;
  case RenderPassDepthStoreProof::BlockNoLookahead:
    add(c.renderPassDepthProofBlockNoLookahead);
    break;
  case RenderPassDepthStoreProof::BlockMsaaResolve:
    add(c.renderPassDepthProofBlockMsaaResolve);
    break;
  case RenderPassDepthStoreProof::BlockDrawDepth:
    add(c.renderPassDepthProofBlockDrawDepth);
    break;
  case RenderPassDepthStoreProof::BlockShadowSample:
    add(c.renderPassDepthProofBlockShadowSample);
    break;
  case RenderPassDepthStoreProof::BlockSurfaceCopy:
    add(c.renderPassDepthProofBlockSurfaceCopy);
    break;
  case RenderPassDepthStoreProof::BlockStretchRect:
    add(c.renderPassDepthProofBlockStretchRect);
    break;
  case RenderPassDepthStoreProof::BlockReadback:
    add(c.renderPassDepthProofBlockReadback);
    break;
  case RenderPassDepthStoreProof::BlockColorFill:
    add(c.renderPassDepthProofBlockColorFill);
    break;
  case RenderPassDepthStoreProof::BlockDepthResolve:
    add(c.renderPassDepthProofBlockDepthResolve);
    break;
  case RenderPassDepthStoreProof::BlockPresent:
    add(c.renderPassDepthProofBlockPresent);
    break;
  case RenderPassDepthStoreProof::BlockClearMismatch:
    add(c.renderPassDepthProofBlockClearMismatch);
    break;
  }
}

void countRenderPassNoLookaheadCause(RenderPassNoLookaheadCause cause) {
  auto& c = counters();
  switch (cause) {
  case RenderPassNoLookaheadCause::Empty:
    add(c.renderPassNoLookaheadEmpty);
    break;
  case RenderPassNoLookaheadCause::Invalid:
    add(c.renderPassNoLookaheadInvalid);
    break;
  case RenderPassNoLookaheadCause::SuffixExhausted:
    add(c.renderPassNoLookaheadSuffixExhausted);
    break;
  case RenderPassNoLookaheadCause::StorageTruncated:
    add(c.renderPassNoLookaheadStorageTruncated);
    break;
  }
}

void countRenderPassLateStoreUnknown(RenderPassLateStoreAspect aspect) {
  auto& c = counters();
  switch (aspect) {
  case RenderPassLateStoreAspect::Color:
    add(c.renderPassLateStoreUnknownColor);
    break;
  case RenderPassLateStoreAspect::Depth:
    add(c.renderPassLateStoreUnknownDepth);
    break;
  case RenderPassLateStoreAspect::Stencil:
    add(c.renderPassLateStoreUnknownStencil);
    break;
  }
}

void countRenderPassLateStoreResolution(
    RenderPassLateStoreAspect aspect,
    RenderPassLateStoreResolutionCause cause) {
  auto& c = counters();
  switch (cause) {
  case RenderPassLateStoreResolutionCause::Clear:
    switch (aspect) {
    case RenderPassLateStoreAspect::Color:
      add(c.renderPassLateStoreResolveClearColor);
      break;
    case RenderPassLateStoreAspect::Depth:
      add(c.renderPassLateStoreResolveClearDepth);
      break;
    case RenderPassLateStoreAspect::Stencil:
      add(c.renderPassLateStoreResolveClearStencil);
      break;
    }
    break;
  case RenderPassLateStoreResolutionCause::ClearMismatch:
    add(c.renderPassLateStoreResolveStoreClearMismatch);
    break;
  case RenderPassLateStoreResolutionCause::Draw:
    add(c.renderPassLateStoreResolveStoreDraw);
    break;
  case RenderPassLateStoreResolutionCause::Sample:
    add(c.renderPassLateStoreResolveStoreSample);
    break;
  case RenderPassLateStoreResolutionCause::Readback:
    add(c.renderPassLateStoreResolveStoreReadback);
    break;
  case RenderPassLateStoreResolutionCause::Copy:
    add(c.renderPassLateStoreResolveStoreCopy);
    break;
  case RenderPassLateStoreResolutionCause::Resolve:
    add(c.renderPassLateStoreResolveStoreResolve);
    break;
  case RenderPassLateStoreResolutionCause::Present:
    add(c.renderPassLateStoreResolveStorePresent);
    break;
  case RenderPassLateStoreResolutionCause::IncompatibleClose:
    add(c.renderPassLateStoreResolveStoreIncompatibleClose);
    break;
  case RenderPassLateStoreResolutionCause::Drain:
    add(c.renderPassLateStoreResolveStoreDrain);
    break;
  case RenderPassLateStoreResolutionCause::Finalize:
    add(c.renderPassLateStoreResolveStoreFinalize);
    break;
  case RenderPassLateStoreResolutionCause::Error:
    add(c.renderPassLateStoreResolveStoreError);
    break;
  }
}

void countCommandBufferCreateCpuTime(std::uint64_t nanoseconds) {
  add(counters().commandBufferCreateCpuNs, nanoseconds);
  updateMax(counters().commandBufferCreateCpuMaxNs, nanoseconds);
  recordRing(counters().commandBufferCreateCpuRing, nanoseconds);
}

void countCommandBufferCommitCpuTime(std::uint64_t nanoseconds) {
  add(counters().commandBufferCommitCpuNs, nanoseconds);
  updateMax(counters().commandBufferCommitCpuMaxNs, nanoseconds);
  recordRing(counters().commandBufferCommitCpuRing, nanoseconds);
}

void countBridgeCommitLatencyNs(std::uint64_t nanoseconds) {
  // Command-chunk boundary B2 — measured across the whole commit_chunk
  // bridge call.
  // This includes unix-side import/replay/queue-submit work and excludes
  // asynchronous encode/GPU work after the call returns. The matching call
  // site is in src/d3d9/device_c_chunk_replay.cpp
  // (dxmt9c_device_commit_chunk).
  add(counters().bridgeCommitLatencyNs, nanoseconds);
  updateMax(counters().bridgeCommitLatencyMaxNs, nanoseconds);
  recordRing(counters().bridgeCommitLatencyRing, nanoseconds);
}

void countOffloadReplayCpuTime(std::uint64_t nanoseconds) {
  add(counters().offloadReplayCpuNs, nanoseconds);
  updateMax(counters().offloadReplayCpuMaxNs, nanoseconds);
  recordRing(counters().offloadReplayCpuRing, nanoseconds);
}

void countOffloadReplayQueueDepth(std::uint64_t depthBeforePush) {
  auto& c = counters();
  add(c.offloadReplayQueueDepthSamples);
  add(c.offloadReplayQueueDepthTotal, depthBeforePush);
  updateMax(c.offloadReplayQueueDepthMax, depthBeforePush);
  if (depthBeforePush > 1) {
    add(c.offloadReplayQueueDepthGt1);
  }
  if (depthBeforePush > 2) {
    add(c.offloadReplayQueueDepthGt2);
  }
  if (depthBeforePush > 4) {
    add(c.offloadReplayQueueDepthGt4);
  }
}

void countOffloadDrainFenceWait() {
  add(counters().offloadDrainFenceWaits);
}

void countOffloadDrainFenceCpuTime(std::uint64_t nanoseconds) {
  add(counters().offloadDrainFenceWaitNs, nanoseconds);
}

void countOffloadCommitAppCpuTime(std::uint64_t nanoseconds) {
  add(counters().offloadCommitAppNs, nanoseconds);
}

void countCommitChunkPhaseCall() { add(counters().commitChunkPhaseCalls); }

void countCommitChunkPhasePrepareCpuTime(std::uint64_t nanoseconds) {
  add(counters().commitChunkPhasePrepareNs, nanoseconds);
}

void countCommitChunkPhaseImportCpuTime(std::uint64_t nanoseconds) {
  add(counters().commitChunkPhaseImportNs, nanoseconds);
}

void countCommitChunkPhaseMarkCpuTime(std::uint64_t nanoseconds) {
  add(counters().commitChunkPhaseMarkNs, nanoseconds);
}

void countCommitChunkPhaseMarkLockCpuTime(std::uint64_t nanoseconds) {
  add(counters().commitChunkPhaseMarkLockNs, nanoseconds);
}

void countCommitChunkPhaseEnqueueCpuTime(std::uint64_t nanoseconds) {
  add(counters().commitChunkPhaseEnqueueNs, nanoseconds);
}

void countCommitChunkPhasePresentWaitTime(std::uint64_t nanoseconds) {
  add(counters().commitChunkPhasePresentWaitNs, nanoseconds);
}

void countOffloadPushBackpressureWait() {
  add(counters().offloadPushBackpressureWaits);
}

void countOffloadPushBackpressureWaitNs(std::uint64_t nanoseconds) {
  add(counters().offloadPushBackpressureWaitNs, nanoseconds);
}

void countOffloadWorkerIdleWaitNs(std::uint64_t nanoseconds) {
  add(counters().offloadWorkerIdleWaitNs, nanoseconds);
}

void countCompletionEnqueue(std::uint64_t pendingDepthAfterPush,
                            bool whileWaiting,
                            bool hasPresent) {
  auto& c = counters();
  add(c.completionEnqueueSamples);
  updateMax(c.completionEnqueuePendingDepthMax, pendingDepthAfterPush);
  if (!whileWaiting) {
    return;
  }
  add(c.completionEnqueueWhileWaiting);
  if (hasPresent) {
    add(c.completionEnqueueWhileWaitingPresent);
  }
}

void countCompletionDequeue(std::uint64_t ageNanoseconds,
                            std::uint64_t pendingDepthAfterPop,
                            std::uint64_t commandBufferStatus) {
  auto& c = counters();
  add(c.completionDequeueSamples);
  add(c.completionDequeueAgeNs, ageNanoseconds);
  updateMax(c.completionDequeueAgeMaxNs, ageNanoseconds);
  updateMax(c.completionPendingDepthMax, pendingDepthAfterPop);
  recordRing(c.completionDequeueAgeRing, ageNanoseconds);
  add(completionDequeueStatusCounter(c, commandBufferStatus));
}

void countCompletionWaitStatus(std::uint64_t nanoseconds,
                               std::uint64_t commandBufferStatus) {
  addCompletionWaitStatusBucket(counters(), commandBufferStatus, nanoseconds);
}

void countCompletionWaitOverlap(std::uint64_t nanoseconds,
                                std::uint64_t enqueuesDuringWait,
                                bool hasPresent) {
  auto& c = counters();
  add(c.completionWaitEnqueuesDuringWait, enqueuesDuringWait);
  updateMax(c.completionWaitEnqueuesDuringWaitMax, enqueuesDuringWait);

  if (enqueuesDuringWait > 0) {
    add(c.completionWaitWithEnqueue);
    add(c.completionWaitWithEnqueueNs, nanoseconds);
    if (hasPresent) {
      add(c.completionPresentWaitWithEnqueue);
      add(c.completionPresentWaitWithEnqueueNs, nanoseconds);
    }
    return;
  }

  add(c.completionWaitWithoutEnqueue);
  add(c.completionWaitWithoutEnqueueNs, nanoseconds);
  if (hasPresent) {
    add(c.completionPresentWaitWithoutEnqueue);
    add(c.completionPresentWaitWithoutEnqueueNs, nanoseconds);
  }
}

void countCompletionSignalDelay(std::uint64_t nanoseconds) {
  auto& c = counters();
  add(c.completionSignalDelay);
  add(c.completionSignalDelayNs, nanoseconds);
}

void countCompletionWaitCommitChunkEntry() {
  add(counters().completionWaitCommitChunkEntries);
}

void countCompletionWaitCommitChunkReplayStart() {
  add(counters().completionWaitCommitChunkReplayStarts);
}

void countCompletionWaitCommitChunkReplayEnd(std::uint64_t replayNanoseconds) {
  auto& c = counters();
  add(c.completionWaitCommitChunkReplayEnds);
  add(c.completionWaitCommitChunkReplayCpuNs, replayNanoseconds);
  updateMax(c.completionWaitCommitChunkReplayCpuMaxNs, replayNanoseconds);
  recordRing(c.completionWaitCommitChunkReplayCpuRing, replayNanoseconds);
}

void countCompletionWaitCommitPublish() {
  add(counters().completionWaitCommitPublishes);
}

void countCompletionWaitEncodeDequeue() {
  add(counters().completionWaitEncodeDequeues);
}

void countCompletionWaitCommandBufferCommit() {
  add(counters().completionWaitCommandBufferCommits);
}

void countCompletionWaitStagePublishToEncodeDequeue(
    std::uint64_t nanoseconds) {
  auto& c = counters();
  add(c.completionWaitStagePublishToEncodeDequeue);
  add(c.completionWaitStagePublishToEncodeDequeueNs, nanoseconds);
  updateMax(c.completionWaitStagePublishToEncodeDequeueMaxNs, nanoseconds);
  recordRing(c.completionWaitStagePublishToEncodeDequeueRing, nanoseconds);
}

void countCompletionWaitStageEncodeDequeueToCommandBufferCommit(
    std::uint64_t nanoseconds) {
  auto& c = counters();
  add(c.completionWaitStageEncodeDequeueToCommandBufferCommit);
  add(c.completionWaitStageEncodeDequeueToCommandBufferCommitNs, nanoseconds);
  updateMax(c.completionWaitStageEncodeDequeueToCommandBufferCommitMaxNs,
            nanoseconds);
  recordRing(c.completionWaitStageEncodeDequeueToCommandBufferCommitRing,
             nanoseconds);
}

void countCompletionNoEnqueueWaitToCommitPublish(std::uint64_t nanoseconds) {
  auto& c = counters();
  add(c.completionNoEnqueueWaitToCommitPublish);
  add(c.completionNoEnqueueWaitToCommitPublishNs, nanoseconds);
  updateMax(c.completionNoEnqueueWaitToCommitPublishMaxNs, nanoseconds);
  recordRing(c.completionNoEnqueueWaitToCommitPublishRing, nanoseconds);
}

void countCompletionNoEnqueueCommitChunksBeforePublish(std::uint64_t entries,
                                                       std::uint64_t replayStarts,
                                                       std::uint64_t replayEnds) {
  auto& c = counters();
  add(c.completionNoEnqueueCommitChunkEntriesBeforePublish, entries);
  updateMax(c.completionNoEnqueueCommitChunkEntriesBeforePublishMax, entries);
  recordRing(c.completionNoEnqueueCommitChunkEntriesBeforePublishRing, entries);
  add(c.completionNoEnqueueCommitChunkReplayStartsBeforePublish, replayStarts);
  updateMax(c.completionNoEnqueueCommitChunkReplayStartsBeforePublishMax,
            replayStarts);
  recordRing(c.completionNoEnqueueCommitChunkReplayStartsBeforePublishRing,
             replayStarts);
  add(c.completionNoEnqueueCommitChunkReplayEndsBeforePublish, replayEnds);
  updateMax(c.completionNoEnqueueCommitChunkReplayEndsBeforePublishMax,
            replayEnds);
  recordRing(c.completionNoEnqueueCommitChunkReplayEndsBeforePublishRing,
             replayEnds);
}

void countCompletionNoEnqueueCommitChunkCompletedReplayCpuBeforePublish(
    std::uint64_t nanoseconds) {
  auto& c = counters();
  add(c.completionNoEnqueueCommitChunkCompletedReplayCpuBeforePublish);
  add(c.completionNoEnqueueCommitChunkCompletedReplayCpuBeforePublishNs,
      nanoseconds);
  updateMax(c.completionNoEnqueueCommitChunkCompletedReplayCpuBeforePublishMaxNs,
            nanoseconds);
  recordRing(c.completionNoEnqueueCommitChunkCompletedReplayCpuBeforePublishRing,
             nanoseconds);
}

void countCompletionNoEnqueueCommitChunkActiveReplayCpuBeforePublish(
    std::uint64_t nanoseconds) {
  auto& c = counters();
  add(c.completionNoEnqueueCommitChunkActiveReplayCpuBeforePublish);
  add(c.completionNoEnqueueCommitChunkActiveReplayCpuBeforePublishNs,
      nanoseconds);
  updateMax(c.completionNoEnqueueCommitChunkActiveReplayCpuBeforePublishMaxNs,
            nanoseconds);
  recordRing(c.completionNoEnqueueCommitChunkActiveReplayCpuBeforePublishRing,
             nanoseconds);
}

void countCompletionNoEnqueueCommitChunkInterReplayGapBeforePublish(
    std::uint64_t nanoseconds) {
  auto& c = counters();
  add(c.completionNoEnqueueCommitChunkInterReplayGapBeforePublish);
  add(c.completionNoEnqueueCommitChunkInterReplayGapBeforePublishNs,
      nanoseconds);
  updateMax(c.completionNoEnqueueCommitChunkInterReplayGapBeforePublishMaxNs,
            nanoseconds);
  recordRing(c.completionNoEnqueueCommitChunkInterReplayGapBeforePublishRing,
             nanoseconds);
}

void countCompletionNoEnqueueCommitPublishWaitBeforePublish(
    std::uint64_t nanoseconds) {
  auto& c = counters();
  add(c.completionNoEnqueueCommitPublishWaitBeforePublish);
  add(c.completionNoEnqueueCommitPublishWaitBeforePublishNs, nanoseconds);
  updateMax(c.completionNoEnqueueCommitPublishWaitBeforePublishMaxNs,
            nanoseconds);
  recordRing(c.completionNoEnqueueCommitPublishWaitBeforePublishRing,
             nanoseconds);
}

void countCompletionNoEnqueueCommitPublishOnBeforePublishCpu(
    std::uint64_t nanoseconds) {
  auto& c = counters();
  add(c.completionNoEnqueueCommitPublishOnBeforePublishCpu);
  add(c.completionNoEnqueueCommitPublishOnBeforePublishCpuNs, nanoseconds);
  updateMax(c.completionNoEnqueueCommitPublishOnBeforePublishCpuMaxNs,
            nanoseconds);
  recordRing(c.completionNoEnqueueCommitPublishOnBeforePublishCpuRing,
             nanoseconds);
}

void countCompletionNoEnqueueCommitChunkRecordShapeBeforePublish(
    std::uint64_t recordCount,
    std::uint64_t drawRecords,
    std::uint64_t constRecords,
    std::uint64_t applyStateRecords,
    std::uint64_t clearRecords,
    std::uint64_t presentRecords,
    std::uint64_t surfaceRecords,
    std::uint64_t queryRecords,
    std::uint64_t otherRecords) {
  auto& c = counters();
  add(c.completionNoEnqueueCommitChunkShapeSamplesBeforePublish);
  add(c.completionNoEnqueueCommitChunkRecordsBeforePublish, recordCount);
  updateMax(c.completionNoEnqueueCommitChunkRecordsBeforePublishMax, recordCount);
  recordRing(c.completionNoEnqueueCommitChunkRecordsBeforePublishRing, recordCount);
  if (drawRecords != 0) {
    add(c.completionNoEnqueueCommitChunkChunksWithDrawBeforePublish);
  }
  if (presentRecords != 0) {
    add(c.completionNoEnqueueCommitChunkChunksWithPresentBeforePublish);
  }
  if (drawRecords == 0 && presentRecords == 0) {
    add(c.completionNoEnqueueCommitChunkChunksNoDrawNoPresentBeforePublish);
  }
  if (recordCount != 0 &&
      recordCount == constRecords + applyStateRecords) {
    add(c.completionNoEnqueueCommitChunkChunksStateConstOnlyBeforePublish);
  }
  add(c.completionNoEnqueueCommitChunkDrawRecordsBeforePublish, drawRecords);
  add(c.completionNoEnqueueCommitChunkConstRecordsBeforePublish, constRecords);
  add(c.completionNoEnqueueCommitChunkApplyStateRecordsBeforePublish,
      applyStateRecords);
  add(c.completionNoEnqueueCommitChunkClearRecordsBeforePublish, clearRecords);
  add(c.completionNoEnqueueCommitChunkPresentRecordsBeforePublish, presentRecords);
  add(c.completionNoEnqueueCommitChunkSurfaceRecordsBeforePublish, surfaceRecords);
  add(c.completionNoEnqueueCommitChunkQueryRecordsBeforePublish, queryRecords);
  add(c.completionNoEnqueueCommitChunkOtherRecordsBeforePublish, otherRecords);
}

void countCompletionNoEnqueueFirstPublishSlotShape(
    std::uint64_t commandCount,
    std::uint64_t drawRunCommands,
    std::uint64_t drawItems,
    std::uint64_t nonDrawCommands,
    std::uint64_t payloadBytes,
    std::uint64_t presentCommands,
    std::uint64_t prePresentCommands,
    std::uint64_t prePresentDrawRunCommands,
    std::uint64_t prePresentDrawItems,
    std::uint64_t prePresentNonDrawCommands,
    std::uint64_t prePresentPayloadBytes,
    std::uint64_t postPresentCommands,
    std::uint64_t presentTailSlots,
    std::uint64_t presentNonTailSlots) {
  auto& c = counters();
  add(c.completionNoEnqueueFirstPublishSlotSamples);
  add(c.completionNoEnqueueFirstPublishSlotCommands, commandCount);
  updateMax(c.completionNoEnqueueFirstPublishSlotCommandsMax, commandCount);
  recordRing(c.completionNoEnqueueFirstPublishSlotCommandsRing, commandCount);
  add(c.completionNoEnqueueFirstPublishSlotDrawRunCommands, drawRunCommands);
  add(c.completionNoEnqueueFirstPublishSlotDrawItems, drawItems);
  updateMax(c.completionNoEnqueueFirstPublishSlotDrawItemsMax, drawItems);
  recordRing(c.completionNoEnqueueFirstPublishSlotDrawItemsRing, drawItems);
  add(c.completionNoEnqueueFirstPublishSlotNonDrawCommands, nonDrawCommands);
  add(c.completionNoEnqueueFirstPublishSlotPayloadBytes, payloadBytes);
  updateMax(c.completionNoEnqueueFirstPublishSlotPayloadBytesMax, payloadBytes);
  recordRing(c.completionNoEnqueueFirstPublishSlotPayloadBytesRing, payloadBytes);
  add(c.completionNoEnqueueFirstPublishSlotPresentCommands, presentCommands);
  add(c.completionNoEnqueueFirstPublishSlotPrePresentCommands, prePresentCommands);
  updateMax(c.completionNoEnqueueFirstPublishSlotPrePresentCommandsMax,
            prePresentCommands);
  recordRing(c.completionNoEnqueueFirstPublishSlotPrePresentCommandsRing,
             prePresentCommands);
  add(c.completionNoEnqueueFirstPublishSlotPrePresentDrawRunCommands,
      prePresentDrawRunCommands);
  add(c.completionNoEnqueueFirstPublishSlotPrePresentDrawItems,
      prePresentDrawItems);
  updateMax(c.completionNoEnqueueFirstPublishSlotPrePresentDrawItemsMax,
            prePresentDrawItems);
  recordRing(c.completionNoEnqueueFirstPublishSlotPrePresentDrawItemsRing,
             prePresentDrawItems);
  add(c.completionNoEnqueueFirstPublishSlotPrePresentNonDrawCommands,
      prePresentNonDrawCommands);
  add(c.completionNoEnqueueFirstPublishSlotPrePresentPayloadBytes,
      prePresentPayloadBytes);
  updateMax(c.completionNoEnqueueFirstPublishSlotPrePresentPayloadBytesMax,
            prePresentPayloadBytes);
  recordRing(c.completionNoEnqueueFirstPublishSlotPrePresentPayloadBytesRing,
             prePresentPayloadBytes);
  add(c.completionNoEnqueueFirstPublishSlotPostPresentCommands,
      postPresentCommands);
  add(c.completionNoEnqueueFirstPublishSlotPresentTailSlots, presentTailSlots);
  add(c.completionNoEnqueueFirstPublishSlotPresentNonTailSlots,
      presentNonTailSlots);
}

void countCompletionNoEnqueueWaitToCommitChunkEntry(std::uint64_t nanoseconds) {
  auto& c = counters();
  add(c.completionNoEnqueueWaitToCommitChunkEntry);
  add(c.completionNoEnqueueWaitToCommitChunkEntryNs, nanoseconds);
  updateMax(c.completionNoEnqueueWaitToCommitChunkEntryMaxNs, nanoseconds);
  recordRing(c.completionNoEnqueueWaitToCommitChunkEntryRing, nanoseconds);
}

void countCompletionNoEnqueueWaitToCommitChunkReplayStart(std::uint64_t nanoseconds) {
  auto& c = counters();
  add(c.completionNoEnqueueWaitToCommitChunkReplayStart);
  add(c.completionNoEnqueueWaitToCommitChunkReplayStartNs, nanoseconds);
  updateMax(c.completionNoEnqueueWaitToCommitChunkReplayStartMaxNs, nanoseconds);
  recordRing(c.completionNoEnqueueWaitToCommitChunkReplayStartRing, nanoseconds);
}

void countCompletionNoEnqueueWaitToCommitChunkReplayEnd(std::uint64_t nanoseconds) {
  auto& c = counters();
  add(c.completionNoEnqueueWaitToCommitChunkReplayEnd);
  add(c.completionNoEnqueueWaitToCommitChunkReplayEndNs, nanoseconds);
  updateMax(c.completionNoEnqueueWaitToCommitChunkReplayEndMaxNs, nanoseconds);
  recordRing(c.completionNoEnqueueWaitToCommitChunkReplayEndRing, nanoseconds);
}

void countCompletionNoEnqueueWaitToEncodeDequeue(std::uint64_t nanoseconds) {
  auto& c = counters();
  add(c.completionNoEnqueueWaitToEncodeDequeue);
  add(c.completionNoEnqueueWaitToEncodeDequeueNs, nanoseconds);
  updateMax(c.completionNoEnqueueWaitToEncodeDequeueMaxNs, nanoseconds);
  recordRing(c.completionNoEnqueueWaitToEncodeDequeueRing, nanoseconds);
}

void countCompletionNoEnqueueWaitToCommandBufferCommit(std::uint64_t nanoseconds) {
  auto& c = counters();
  add(c.completionNoEnqueueWaitToCommandBufferCommit);
  add(c.completionNoEnqueueWaitToCommandBufferCommitNs, nanoseconds);
  updateMax(c.completionNoEnqueueWaitToCommandBufferCommitMaxNs, nanoseconds);
  recordRing(c.completionNoEnqueueWaitToCommandBufferCommitRing, nanoseconds);
}

void countEncodeDequeueReadyDepth(std::uint64_t readyDepthBeforePop) {
  auto& c = counters();
  add(c.encodeDequeueReadyDepthSamples);
  add(c.encodeDequeueReadyDepthTotal, readyDepthBeforePop);
  updateMax(c.encodeDequeueReadyDepthMax, readyDepthBeforePop);
  if (readyDepthBeforePop > 1) {
    add(c.encodeDequeueReadyDepthGt1);
  }
  if (readyDepthBeforePop > 2) {
    add(c.encodeDequeueReadyDepthGt2);
  }
  if (readyDepthBeforePop > 4) {
    add(c.encodeDequeueReadyDepthGt4);
  }
}

void countCompletionNoEnqueueStageCommitEntryToPublish(std::uint64_t nanoseconds) {
  auto& c = counters();
  add(c.completionNoEnqueueStageCommitEntryToPublish);
  add(c.completionNoEnqueueStageCommitEntryToPublishNs, nanoseconds);
  updateMax(c.completionNoEnqueueStageCommitEntryToPublishMaxNs, nanoseconds);
  recordRing(c.completionNoEnqueueStageCommitEntryToPublishRing, nanoseconds);
}

void countCompletionNoEnqueueStagePublishToEncodeDequeue(std::uint64_t nanoseconds) {
  auto& c = counters();
  add(c.completionNoEnqueueStagePublishToEncodeDequeue);
  add(c.completionNoEnqueueStagePublishToEncodeDequeueNs, nanoseconds);
  updateMax(c.completionNoEnqueueStagePublishToEncodeDequeueMaxNs, nanoseconds);
  recordRing(c.completionNoEnqueueStagePublishToEncodeDequeueRing, nanoseconds);
}

void countCompletionNoEnqueueStageEncodeDequeueToCommandBufferCommit(std::uint64_t nanoseconds) {
  auto& c = counters();
  add(c.completionNoEnqueueStageEncodeDequeueToCommandBufferCommit);
  add(c.completionNoEnqueueStageEncodeDequeueToCommandBufferCommitNs, nanoseconds);
  updateMax(c.completionNoEnqueueStageEncodeDequeueToCommandBufferCommitMaxNs, nanoseconds);
  recordRing(c.completionNoEnqueueStageEncodeDequeueToCommandBufferCommitRing, nanoseconds);
}

void countCompletionNoEnqueueWaitToNextEnqueue(std::uint64_t nanoseconds,
                                               bool hasPresent) {
  auto& c = counters();
  add(c.completionNoEnqueueWaitToNextEnqueue);
  add(c.completionNoEnqueueWaitToNextEnqueueNs, nanoseconds);
  updateMax(c.completionNoEnqueueWaitToNextEnqueueMaxNs, nanoseconds);
  recordRing(c.completionNoEnqueueWaitToNextEnqueueRing, nanoseconds);
  if (hasPresent) {
    add(c.completionNoEnqueueWaitToNextPresentEnqueue);
    add(c.completionNoEnqueueWaitToNextPresentEnqueueNs, nanoseconds);
  }
}

void countCompletionWait(std::uint64_t nanoseconds,
                         bool hasDraw,
                         bool hasPresent,
                         bool hasBlit,
                         bool hasStretchRect,
                         std::uint32_t compatFlags,
                         std::uint64_t vertexShaderHash,
                         std::uint64_t pixelShaderHash,
                         std::uint64_t shaderVariantHash) {
  add(counters().completionWaits);
  add(counters().completionWaitNs, nanoseconds);
  updateMax(counters().completionWaitMaxNs, nanoseconds);
  recordRing(counters().completionWaitRing, nanoseconds);
  auto addWaitBucket = [nanoseconds](std::atomic<std::uint64_t>& waits,
                                     std::atomic<std::uint64_t>& waitNs,
                                     std::atomic<std::uint64_t>& maxNs,
                                     PercentileRing& ring) {
    add(waits);
    add(waitNs, nanoseconds);
    updateMax(maxNs, nanoseconds);
    recordRing(ring, nanoseconds);
  };
  if (hasPresent) {
    addWaitBucket(counters().completionPresentWaits,
                  counters().completionPresentWaitNs,
                  counters().completionPresentWaitMaxNs,
                  counters().completionPresentWaitRing);
  } else if (hasDraw) {
    addWaitBucket(counters().completionDrawWaits,
                  counters().completionDrawWaitNs,
                  counters().completionDrawWaitMaxNs,
                  counters().completionDrawWaitRing);
  } else if (hasBlit) {
    addWaitBucket(counters().completionBlitWaits,
                  counters().completionBlitWaitNs,
                  counters().completionBlitWaitMaxNs,
                  counters().completionBlitWaitRing);
  } else {
    addWaitBucket(counters().completionOtherWaits,
                  counters().completionOtherWaitNs,
                  counters().completionOtherWaitMaxNs,
                  counters().completionOtherWaitRing);
  }

  if (hasPresent && !hasDraw && !hasBlit && !hasStretchRect) {
    addWaitBucket(counters().completionPresentOnlyWaits,
                  counters().completionPresentOnlyWaitNs,
                  counters().completionPresentOnlyWaitMaxNs,
                  counters().completionPresentOnlyWaitRing);
  } else if (hasPresent && hasDraw) {
    addWaitBucket(counters().completionDrawPresentWaits,
                  counters().completionDrawPresentWaitNs,
                  counters().completionDrawPresentWaitMaxNs,
                  counters().completionDrawPresentWaitRing);
  } else if (!hasPresent && hasDraw && hasStretchRect) {
    addWaitBucket(counters().completionDrawStretchWaits,
                  counters().completionDrawStretchWaitNs,
                  counters().completionDrawStretchWaitMaxNs,
                  counters().completionDrawStretchWaitRing);
  } else if (!hasPresent && !hasDraw && hasStretchRect) {
    addWaitBucket(counters().completionStretchWaits,
                  counters().completionStretchWaitNs,
                  counters().completionStretchWaitMaxNs,
                  counters().completionStretchWaitRing);
  } else if (!hasPresent && hasBlit && !hasStretchRect) {
    addWaitBucket(counters().completionBlitOnlyWaits,
                  counters().completionBlitOnlyWaitNs,
                  counters().completionBlitOnlyWaitMaxNs,
                  counters().completionBlitOnlyWaitRing);
  }

  constexpr std::uint32_t fp16 = 1u << 0;
  constexpr std::uint32_t mrt = 1u << 1;
  constexpr std::uint32_t srgb = 1u << 2;
  constexpr std::uint32_t projected = 1u << 3;
  constexpr std::uint32_t msaa = 1u << 4;
  constexpr std::uint32_t query = 1u << 5;
  if ((compatFlags & fp16) != 0) add(counters().completionCompatFp16WaitNs, nanoseconds);
  if ((compatFlags & mrt) != 0) add(counters().completionCompatMrtWaitNs, nanoseconds);
  if ((compatFlags & srgb) != 0) add(counters().completionCompatSrgbWaitNs, nanoseconds);
  if ((compatFlags & projected) != 0) add(counters().completionCompatProjectedWaitNs, nanoseconds);
  if ((compatFlags & msaa) != 0) add(counters().completionCompatMsaaWaitNs, nanoseconds);
  if ((compatFlags & query) != 0) add(counters().completionCompatQueryWaitNs, nanoseconds);

  if (vertexShaderHash != 0 || pixelShaderHash != 0 || shaderVariantHash != 0) {
    auto& c = counters();
    add(c.completionShaderBucketSamples);
    const bool changed = load(c.completionLastVertexShaderHash) != vertexShaderHash ||
                         load(c.completionLastPixelShaderHash) != pixelShaderHash ||
                         load(c.completionLastShaderVariantHash) != shaderVariantHash;
    if (changed) {
      add(c.completionShaderBucketChanges);
    }
    store(c.completionLastVertexShaderHash, vertexShaderHash);
    store(c.completionLastPixelShaderHash, pixelShaderHash);
    store(c.completionLastShaderVariantHash, shaderVariantHash);
  }
}

void countSyncWait(std::uint64_t nanoseconds) {
  add(counters().syncWaits);
  add(counters().syncWaitNs, nanoseconds);
  updateMax(counters().syncWaitMaxNs, nanoseconds);
  recordRing(counters().syncWaitRing, nanoseconds);
}

void countQueueWriterWait(std::uint64_t nanoseconds) {
  add(counters().queueWriterWaits);
  add(counters().queueWriterWaitNs, nanoseconds);
  updateMax(counters().queueWriterWaitMaxNs, nanoseconds);
  recordRing(counters().queueWriterWaitRing, nanoseconds);
}

void countQueueCommitWait(std::uint64_t nanoseconds) {
  add(counters().queueCommitWaits);
  add(counters().queueCommitWaitNs, nanoseconds);
  updateMax(counters().queueCommitWaitMaxNs, nanoseconds);
  recordRing(counters().queueCommitWaitRing, nanoseconds);
}

void countQueueSequenceWait(std::uint64_t nanoseconds) {
  add(counters().queueSequenceWaits);
  add(counters().queueSequenceWaitNs, nanoseconds);
  updateMax(counters().queueSequenceWaitMaxNs, nanoseconds);
  recordRing(counters().queueSequenceWaitRing, nanoseconds);
}

void recordCpuReadyTapeStats(std::uint64_t residentSources,
                             std::uint64_t residentPages,
                             std::uint64_t readyEntries,
                             std::uint64_t admissionCloses,
                             std::uint64_t admissionReopens,
                             std::uint64_t wrapPaddingPages) {
  auto& c = counters();
  store(c.cpuReadyTapeResidentSources, residentSources);
  updateMax(c.cpuReadyTapeResidentSourcesPeak, residentSources);
  store(c.cpuReadyTapeResidentPages, residentPages);
  updateMax(c.cpuReadyTapeResidentPagesPeak, residentPages);
  store(c.cpuReadyTapeReadyEntries, readyEntries);
  updateMax(c.cpuReadyTapeReadyEntriesPeak, readyEntries);
  store(c.cpuReadyTapeAdmissionCloses, admissionCloses);
  store(c.cpuReadyTapeAdmissionReopens, admissionReopens);
  store(c.cpuReadyTapeWrapPaddingPages, wrapPaddingPages);
}

void countCpuReadyTapeAdmissionWait(std::uint64_t nanoseconds) {
  auto& c = counters();
  add(c.cpuReadyTapeAdmissionWaits);
  add(c.cpuReadyTapeAdmissionWaitNs, nanoseconds);
  updateMax(c.cpuReadyTapeAdmissionWaitMaxNs, nanoseconds);
  recordRing(c.cpuReadyTapeAdmissionWaitRing, nanoseconds);
}

void countCpuReadyTapeLegacyOversizeBypass() {
  add(counters().cpuReadyTapeLegacyOversizeBypass);
}

void countCpuReadyTapeReclaimWakeup() {
  add(counters().cpuReadyTapeReclaimWakeups);
}

void countMapBufferWait(std::uint64_t totalNanoseconds,
                        std::uint64_t mutexNanoseconds,
                        std::uint64_t sequenceNanoseconds,
                        std::uint32_t flags,
                        bool waited) {
  auto& c = counters();
  add(c.mapBufferCalls);
  add(c.mapBufferTotalNs, totalNanoseconds);
  updateMax(c.mapBufferTotalMaxNs, totalNanoseconds);
  recordRing(c.mapBufferTotalRing, totalNanoseconds);
  add(c.mapBufferMutexWaitNs, mutexNanoseconds);
  updateMax(c.mapBufferMutexWaitMaxNs, mutexNanoseconds);
  recordRing(c.mapBufferMutexWaitRing, mutexNanoseconds);
  if (waited) {
    add(c.mapBufferWaitSeq);
    add(c.mapBufferWaitNs, sequenceNanoseconds);
    updateMax(c.mapBufferWaitMaxNs, sequenceNanoseconds);
    recordRing(c.mapBufferWaitRing, sequenceNanoseconds);
  } else {
    add(c.mapBufferNoWaitSeq);
  }
  const bool discard = (flags & core::UsageDiscard) != 0;
  const bool noOverwrite = (flags & core::UsageNoOverwrite) != 0;
  const bool readOnly = (flags & core::UsageReadOnly) != 0;
  if (discard) add(c.mapBufferDiscard);
  if (noOverwrite) add(c.mapBufferNoOverwrite);
  if (readOnly) add(c.mapBufferReadOnly);
  if (!discard && !noOverwrite) add(c.mapBufferPlain);
}

void countManagedBufferUpload(std::uint64_t bytes) {
  add(counters().managedBufferUploads);
  add(counters().managedBufferUploadBytes, bytes);
}

void countManagedBufferBackingInPlace() {
  add(counters().managedBufferBackingInPlace);
}

void countManagedBufferBackingReuse() {
  add(counters().managedBufferBackingReuse);
}

void countManagedBufferBackingFresh() {
  add(counters().managedBufferBackingFresh);
}

void countPresentBoundaryApplied() {
  add(counters().presentBoundaryApplied);
}

void countPresentBoundarySkipped() {
  add(counters().presentBoundarySkipped);
}

void countPresentBoundaryDeferred() {
  add(counters().presentBoundaryDeferred);
}

void countPresentBoundaryDeferredWait() {
  add(counters().presentBoundaryDeferredWaits);
}

void countPresentBoundaryWait(std::uint64_t nanoseconds) {
  add(counters().presentBoundaryWaits);
  add(counters().presentBoundaryWaitNs, nanoseconds);
  updateMax(counters().presentBoundaryWaitMaxNs, nanoseconds);
  recordRing(counters().presentBoundaryWaitRing, nanoseconds);
}

void countPresentOrdinalBoundaryWait() {
  add(counters().presentOrdinalBoundaryWaits);
}

void countPresentOrdinalBoundaryWaitNs(std::uint64_t nanoseconds) {
  add(counters().presentOrdinalBoundaryWaitNs, nanoseconds);
}

void countCompletedPresentOrdinal() {
  add(counters().completedPresentOrdinal);
}

void countPresentEncoded() {
  if (!enabled()) {
    return;
  }
  const auto value =
      counters().presentEncoded.fetch_add(1, std::memory_order_relaxed) + 1;
  const auto interval = periodicPresentInterval();
  if (interval != 0 && value % interval == 0) {
    report();
  }
}

void countPresentSkipped() {
  add(counters().presentSkipped);
}

void countPresentFullscreen() {
  add(counters().presentFullscreen);
}

void countPresentSourceSelection(bool explicitSource, bool isCurrentBackBuffer) {
  add(counters().presentSourceSelections);
  if (explicitSource) {
    add(counters().presentSourceExplicit);
  }
  if (isCurrentBackBuffer) {
    add(counters().presentSourceCurrentBackBuffer);
  }
}

void countPresentSourceResolved(bool hasSurface,
                                bool hasTexture,
                                bool hasResolveTexture,
                                bool invalidSize,
                                std::uint32_t width,
                                std::uint32_t height,
                                std::uint32_t format,
                                std::uint32_t sampleCount,
                                std::uint64_t sourceHandle,
                                std::uint64_t textureHandle) {
  if (!enabled()) {
    return;
  }
  add(counters().presentSourceChecks);
  if (!hasSurface) {
    add(counters().presentSourceMissingSurface);
  } else if (!hasTexture) {
    add(counters().presentSourceMissingTexture);
  } else {
    add(counters().presentSourceValid);
  }
  if (hasResolveTexture) {
    add(counters().presentSourceResolve);
  }
  if (invalidSize) {
    add(counters().presentSourceInvalidSize);
  }
  store(counters().presentSourceWidth, width);
  store(counters().presentSourceHeight, height);
  store(counters().presentSourceFormat, format);
  store(counters().presentSourceSampleCount, sampleCount);
  store(counters().presentSourceHandle, sourceHandle);
  store(counters().presentSourceTextureHandle, textureHandle);
}

void countPresentPass(std::uint32_t sourceWidth,
                      std::uint32_t sourceHeight,
                      std::uint64_t targetWidth,
                      std::uint64_t targetHeight) {
  if (!enabled()) {
    return;
  }
  add(counters().presentPass);
  store(counters().presentPassSrcWidth, sourceWidth);
  store(counters().presentPassSrcHeight, sourceHeight);
  store(counters().presentPassDstWidth, targetWidth);
  store(counters().presentPassDstHeight, targetHeight);
  updateMax(counters().presentPassDstMaxWidth, targetWidth);
  updateMax(counters().presentPassDstMaxHeight, targetHeight);
}

void countPresentSchedule(bool requestedDisplaySync, double minimumPresentDuration) {
  if (!enabled()) {
    return;
  }
  if (requestedDisplaySync) {
    add(counters().presentScheduleRequestedSync);
  } else {
    add(counters().presentScheduleRequestedImmediate);
  }
  if (minimumPresentDuration > 0.0) {
    add(counters().presentScheduleAfterMinimumDuration);
    const auto nanoseconds =
        static_cast<std::uint64_t>(minimumPresentDuration * 1000000000.0 + 0.5);
    add(counters().presentMinimumDurationNs, nanoseconds);
    updateMax(counters().presentMinimumDurationMaxNs, nanoseconds);
  } else {
    add(counters().presentScheduleImmediate);
  }
}

void countPresentAcquireWait(std::uint64_t nanoseconds) {
  add(counters().presentAcquireWaits);
  add(counters().presentAcquireWaitNs, nanoseconds);
  updateMax(counters().presentAcquireWaitMaxNs, nanoseconds);
  if (nanoseconds >= 1000000ull) {
    add(counters().presentAcquireSlowWaits);
  }
  recordRing(counters().presentAcquireWaitRing, nanoseconds);
}

void countPresentAsyncAcquireRequest() {
  add(counters().presentAsyncAcquireRequests);
}

void countPresentAsyncAcquireIssued() {
  add(counters().presentAsyncAcquireIssued);
}

void countPresentAsyncAcquireFallback() {
  add(counters().presentAsyncAcquireFallbacks);
}

void countPresentAsyncAcquireWait(std::uint64_t nanoseconds) {
  add(counters().presentAsyncAcquireWaits);
  add(counters().presentAsyncAcquireWaitNs, nanoseconds);
  updateMax(counters().presentAsyncAcquireWaitMaxNs, nanoseconds);
  if (nanoseconds >= 1000000ull) {
    add(counters().presentAsyncAcquireSlowWaits);
  }
  recordRing(counters().presentAsyncAcquireWaitRing, nanoseconds);
}

void countPresentTokenWait(std::uint64_t nanoseconds) {
  add(counters().presentTokenWaits);
  add(counters().presentTokenWaitNs, nanoseconds);
  updateMax(counters().presentTokenWaitMaxNs, nanoseconds);
  if (nanoseconds >= 1000000ull) {
    add(counters().presentTokenSlowWaits);
  }
  recordRing(counters().presentTokenWaitRing, nanoseconds);
}

void countPresentPreAcquireRequest() {
  add(counters().presentPreAcquireRequests);
}

void countPresentPreAcquireHit() {
  add(counters().presentPreAcquireHits);
}

void countPresentPreAcquireMiss() {
  add(counters().presentPreAcquireMisses);
}

void countPresentPreAcquireWait(std::uint64_t nanoseconds) {
  add(counters().presentPreAcquireWaitNs, nanoseconds);
  updateMax(counters().presentPreAcquireWaitMaxNs, nanoseconds);
  recordRing(counters().presentPreAcquireWaitRing, nanoseconds);
}

void countPresentSetPropsWait(std::uint64_t nanoseconds) {
  add(counters().presentSetPropsWaits);
  add(counters().presentSetPropsWaitNs, nanoseconds);
  recordRing(counters().presentSetPropsWaitRing, nanoseconds);
}

// --- Per-frame snapshot mode (opt-in) ---------------------------------------
// The cumulative `[dxmt9-perf]` line at process exit hides which frame caused
// a spike. When DXMT9_PERF_FRAME_SAMPLING=1 the encode-thread Present handler
// records a snapshot once per Present and prints a `[dxmt9-perf-frame]` line
// whose values are deltas vs the prior snapshot.
//
// Subset emitted per frame (~30 keys): all timing nanoseconds we already
// expose plus the major volume counters (submit_draw, submit_present,
// render_pass_begin/end, draw_calls/indexed/primitives/triangles/vertices,
// command_buffers, bind_pipeline, present_encoded). Keeps line length
// bounded; the cumulative report at exit still covers all 200+ keys.

bool frameSamplingEnabled() {
  static const bool value = []() {
    if (const char* v = std::getenv("DXMT9_PERF_FRAME_SAMPLING")) {
      return v[0] != '\0' && v[0] != '0';
    }
    return false;
  }();
  return value;
}

bool encoderBreakdownEnabled() {
  static const bool value = []() {
    if (const char* v = std::getenv("DXMT9_PERF_ENCODER_BREAKDOWN")) {
      return v[0] != '\0' && v[0] != '0';
    }
    return false;
  }();
  return enabled() && value;
}

std::uint64_t encoderBreakdownSeqFilter() {
  static const std::uint64_t value = []() -> std::uint64_t {
    const char* env = std::getenv("DXMT9_PERF_ENCODER_BREAKDOWN_SEQ");
    if (!env || env[0] == '\0') {
      return 0;
    }
    char* end = nullptr;
    const auto parsed = std::strtoull(env, &end, 10);
    return end != env ? static_cast<std::uint64_t>(parsed) : 0;
  }();
  return value;
}

namespace {

std::uint64_t parseEnvU64(const char* name) {
  const char* env = std::getenv(name);
  if (!env || env[0] == '\0') {
    return 0;
  }
  char* end = nullptr;
  const auto parsed = std::strtoull(env, &end, 10);
  return end != env ? static_cast<std::uint64_t>(parsed) : 0;
}

}  // namespace

bool encoderBreakdownSeqAllowed(std::uint64_t seq) {
  const auto exact = encoderBreakdownSeqFilter();
  if (exact != 0) {
    return seq == exact;
  }
  static const std::uint64_t minSeq =
      parseEnvU64("DXMT9_PERF_ENCODER_BREAKDOWN_SEQ_MIN");
  static const std::uint64_t maxSeq =
      parseEnvU64("DXMT9_PERF_ENCODER_BREAKDOWN_SEQ_MAX");
  if (minSeq != 0 && seq < minSeq) {
    return false;
  }
  if (maxSeq != 0 && seq > maxSeq) {
    return false;
  }
  return minSeq == 0 || maxSeq == 0 || minSeq <= maxSeq;
}

bool encoderBreakdownSeqFilterActive() {
  if (encoderBreakdownSeqFilter() != 0) {
    return true;
  }
  static const bool value =
      parseEnvU64("DXMT9_PERF_ENCODER_BREAKDOWN_SEQ_MIN") != 0 ||
      parseEnvU64("DXMT9_PERF_ENCODER_BREAKDOWN_SEQ_MAX") != 0;
  return value;
}

void emitEncoderBreakdown(const EncoderBreakdown& b) {
  if (!encoderBreakdownEnabled()) {
    return;
  }
  std::fprintf(
      stderr,
      "[dxmt9-perf-encoder seq=%llu encoder=%llu rt=0x%llx depth=0x%llx "
      "rt_format=%llu rt_width=%llu rt_height=%llu rt_bpp=%llu "
      "rt_alias_texture=0x%llx rt_texture_usage=0x%llx "
      "rt_format_swizzle=%llu rt_texture_needs_shader_read_view=%llu "
      "depth_format=%llu depth_width=%llu depth_height=%llu depth_bpp=%llu "
      "depth_alias_texture=0x%llx depth_texture_usage=0x%llx "
      "depth_format_swizzle=%llu depth_texture_needs_shader_read_view=%llu "
      "color_attachment_count=%llu color0_included=%llu "
      "color0_load_action=%llu color0_store_action=%llu "
      "color0_clear=%llu color_load_bytes=%llu color_store_bytes=%llu "
      "depth_included=%llu depth_load_action=%llu depth_store_action=%llu "
      "depth_clear=%llu depth_load_bytes=%llu depth_store_bytes=%llu "
      "stencil_included=%llu stencil_load_action=%llu stencil_store_action=%llu "
      "stencil_clear=%llu stencil_load_bytes=%llu stencil_store_bytes=%llu "
      "end_reason=%s draw_calls=%llu indexed_draws=%llu "
      "expanded_indexed_draws=%llu ffp_draws=%llu programmable_draws=%llu "
      "pretransformed_draws=%llu textured_draws=%llu "
      "cull_none_draws=%llu cull_front_draws=%llu cull_back_draws=%llu "
      "fill_solid_draws=%llu fill_wireframe_draws=%llu "
      "depth_enabled_draws=%llu depth_write_draws=%llu "
      "depth_func_less_draws=%llu depth_func_lessequal_draws=%llu "
      "depth_func_always_draws=%llu depth_func_other_draws=%llu "
      "scissor_enabled_draws=%llu alpha_blend_enabled_draws=%llu "
      "blend_state_samples=%llu blend_state_changes=%llu "
      "blend_state_unique=%llu blend_state_unique_overflows=%llu "
      "blend_state_last=0x%llx blend_enabled_noop_draws=%llu "
      "blend_constant_factor_draws=%llu "
      "blend_screen_draws=%llu blend_additive_draws=%llu "
      "blend_alpha_composite_draws=%llu "
      "alpha_blend_textured_draws=%llu "
      "alpha_blend_textured_primitives=%llu "
      "alpha_blend_textured_vertices=%llu "
      "alpha_blend_small_draws=%llu "
      "alpha_blend_small_primitives=%llu "
      "alpha_blend_small_vertices=%llu "
      "alpha_test_enabled_draws=%llu alpha_test_effective_draws=%llu "
      "clip_plane_enabled_draws=%llu "
      "point_draws=%llu line_draws=%llu triangle_draws=%llu primitive_count=%llu "
      "triangle_estimate=%llu vertex_count=%llu "
      "route_depth_only_draws=%llu "
      "route_depth_only_primitives=%llu "
      "route_depth_only_vertices=%llu "
      "route_programmable_textured_draws=%llu "
      "route_programmable_textured_primitives=%llu "
      "route_programmable_textured_vertices=%llu "
      "route_programmable_color_draws=%llu "
      "route_programmable_color_primitives=%llu "
      "route_programmable_color_vertices=%llu "
      "route_alpha_blend_primitives=%llu "
      "route_alpha_test_primitives=%llu "
      "tile_ffp_routed_tile_draws=%llu "
      "tile_ffp_routed_tile_primitives=%llu "
      "tile_ffp_routed_tile_vertices=%llu "
      "tile_ffp_routed_portable_draws=%llu "
      "tile_ffp_routed_portable_primitives=%llu "
      "tile_ffp_routed_portable_vertices=%llu "
      "tile_ffp_eligible_draws=%llu "
      "tile_ffp_eligible_primitives=%llu "
      "tile_ffp_eligible_vertices=%llu "
      "tile_ffp_fallback_gpu_family_draws=%llu "
      "tile_ffp_fallback_gpu_family_primitives=%llu "
      "tile_ffp_fallback_not_ffp_draws=%llu "
      "tile_ffp_fallback_not_ffp_primitives=%llu "
      "tile_ffp_fallback_precision_draws=%llu "
      "tile_ffp_fallback_precision_primitives=%llu "
      "tile_ffp_fallback_unsupported_state_draws=%llu "
      "tile_ffp_fallback_unsupported_state_primitives=%llu "
      "indexed_triangle_opaque_depth_write_draws=%llu "
      "indexed_triangle_opaque_depth_write_primitives=%llu "
      "indexed_triangle_opaque_depth_write_vertices=%llu "
      "indexed_triangle_depth_read_draws=%llu "
      "indexed_triangle_depth_read_primitives=%llu "
      "indexed_triangle_depth_read_vertices=%llu "
      "indexed_triangle_alpha_blend_draws=%llu "
      "indexed_triangle_alpha_blend_primitives=%llu "
      "indexed_triangle_alpha_blend_vertices=%llu "
      "indexed_triangle_scissor_draws=%llu "
      "indexed_triangle_scissor_primitives=%llu "
      "indexed_triangle_scissor_vertices=%llu "
      "indexed_triangle_textured_draws=%llu "
      "indexed_triangle_textured_primitives=%llu "
      "indexed_triangle_textured_vertices=%llu "
      "indexed_triangle_large_4096_draws=%llu "
      "indexed_triangle_large_4096_primitives=%llu "
      "indexed_triangle_large_4096_vertices=%llu "
      "indexed_triangle_large_4096_opaque_depth_write_draws=%llu "
      "indexed_triangle_large_4096_opaque_depth_write_primitives=%llu "
      "indexed_triangle_large_4096_opaque_depth_write_vertices=%llu "
      "indexed_triangle_large_4096_depth_read_draws=%llu "
      "indexed_triangle_large_4096_depth_read_primitives=%llu "
      "indexed_triangle_large_4096_depth_read_vertices=%llu "
      "indexed_triangle_large_4096_alpha_blend_draws=%llu "
      "indexed_triangle_large_4096_alpha_blend_primitives=%llu "
      "indexed_triangle_large_4096_alpha_blend_vertices=%llu "
      "indexed_triangle_large_4096_scissor_draws=%llu "
      "indexed_triangle_large_4096_scissor_primitives=%llu "
      "indexed_triangle_large_4096_scissor_vertices=%llu "
      "indexed_triangle_large_4096_textured_draws=%llu "
      "indexed_triangle_large_4096_textured_primitives=%llu "
      "indexed_triangle_large_4096_textured_vertices=%llu "
      "texture_mask_or=0x%llx "
      "fragment_texture_binding_samples=%llu "
      "fragment_texture_binding_mask_or=0x%llx "
      "x8_rt_texture_binding_samples=%llu "
      "x8_rt_texture_binding_mask_or=0x%llx "
      "x8_rt_texture_binding_unique_handles=%llu "
      "x8_rt_texture_binding_unique_handle_overflows=%llu "
      "x8_rt_texture_binding_shader_read_view_samples=%llu "
      "x8_rt_texture_binding_active_rt_alias_samples=%llu "
      "x8_shader_alpha_fill_samples=%llu "
      "x8_shader_alpha_fill_mask_or=0x%llx "
      "x8_rt_texture_binding_last_stage=%llu "
      "x8_rt_texture_binding_last_handle=0x%llx "
      "draw_primitive_min=%llu draw_primitive_max=%llu "
      "draw_vertex_min=%llu draw_vertex_max=%llu "
      "draw_primitive_bucket_1_63=%llu "
      "draw_primitive_bucket_64_255=%llu "
      "draw_primitive_bucket_256_1023=%llu "
      "draw_primitive_bucket_1024_4095=%llu "
      "draw_primitive_bucket_4096_plus=%llu "
      "draw_vertex_bucket_1_255=%llu "
      "draw_vertex_bucket_256_1023=%llu "
      "draw_vertex_bucket_1024_4095=%llu "
      "draw_vertex_bucket_4096_16383=%llu "
      "draw_vertex_bucket_16384_plus=%llu "
      "draw_geometry_signature_samples=%llu "
      "draw_geometry_signature_unique=%llu "
      "draw_geometry_signature_unique_overflows=%llu "
      "draw_geometry_signature_duplicates=%llu "
      "draw_geometry_signature_consecutive_duplicates=%llu "
      "draw_geometry_signature_last=0x%llx "
      "indexed_base_vertex_samples=%llu "
      "indexed_base_vertex_nonzero_draws=%llu "
      "indexed_base_vertex_negative_draws=%llu "
      "indexed_base_vertex_positive_draws=%llu "
      "indexed_base_vertex_min=%lld indexed_base_vertex_max=%lld "
      "native_base_vertex_requested_draws=%llu "
      "native_base_vertex_used_draws=%llu "
      "native_base_vertex_skipped_negative_draws=%llu "
      "split_large_indexed_source_draws=%llu "
      "split_large_indexed_metal_draws=%llu "
      "split_large_indexed_extra_draws=%llu "
      "split_large_indexed_primitive_limit=%llu "
      "split_large_indexed_stream0_span_limit=%llu "
      "split_large_indexed_chunk_stream0_span_max=%llu "
      "split_large_indexed_primitive_count=%llu "
      "indexed_order_probe_draws=%llu "
      "indexed_order_probe_skipped=%llu "
      "indexed_order_probe_bytes=%llu "
      "indexed_order_optimized_draws=%llu "
      "indexed_order_optimized_skipped=%llu "
      "indexed_order_optimized_bytes=%llu "
      "probe_scissor_rect_draws=%llu "
      "probe_scissor_rect_skipped=%llu "
      "probe_scissor_rect_area_delta_pixels=%llu "
      "probe_disable_alpha_blend_draws=%llu "
      "probe_disable_depth_write_draws=%llu "
      "probe_depth_func_always_draws=%llu "
      "probe_force_texture_white_draws=%llu "
      "probe_fragmentless_depth_only_draws=%llu "
      "probe_fragmentless_depth_only_primitives=%llu "
      "probe_fragmentless_depth_only_vertices=%llu "
      "indexed_vertex_reuse_samples=%llu "
      "indexed_vertex_reuse_skipped=%llu "
      "indexed_vertex_reference_count=%llu "
      "indexed_unique_vertex_estimate=%llu "
      "indexed_vertex_cache_miss_estimate_16=%llu "
      "indexed_vertex_cache_miss_estimate_32=%llu "
      "indexed_vertex_cache_miss_estimate_64=%llu "
      "indexed_cache_opt_candidate_draws=%llu "
      "indexed_cache_opt_candidate_skipped=%llu "
      "indexed_cache_opt_candidate_bytes=%llu "
      "indexed_cache_opt_candidate_original_miss16=%llu "
      "indexed_cache_opt_candidate_original_miss32=%llu "
      "indexed_cache_opt_candidate_original_miss64=%llu "
      "indexed_cache_opt_candidate_miss16=%llu "
      "indexed_cache_opt_candidate_miss32=%llu "
      "indexed_cache_opt_candidate_miss64=%llu "
      "indexed_cache_opt_candidate_gate_pass=%llu "
      "indexed_cache_opt_candidate_gate_fail=%llu "
      "indexed_cache_opt_candidate_opaque_depth_draws=%llu "
      "indexed_cache_opt_candidate_screen_blend_draws=%llu "
      "indexed_cache_opt_candidate_primitive_bucket_1_63=%llu "
      "indexed_cache_opt_candidate_primitive_bucket_64_255=%llu "
      "indexed_cache_opt_candidate_primitive_bucket_256_1023=%llu "
      "indexed_cache_opt_candidate_primitive_bucket_1024_4095=%llu "
      "indexed_cache_opt_candidate_primitive_bucket_4096_plus=%llu "
      "reordered_index_cache_lookups=%llu "
      "reordered_index_cache_hits=%llu "
      "reordered_index_cache_rejected_hits=%llu "
      "reordered_index_cache_misses=%llu "
      "reordered_index_cache_created=%llu "
      "reordered_index_cache_created_bytes=%llu "
      "stream0_stride_min=%llu stream0_stride_max=%llu "
      "stream_state_samples=%llu stream_metal_binds=%llu "
      "stream_metal_bind_firsts=%llu "
      "stream_metal_bind_handle_changes=%llu "
      "stream_metal_bind_offset_changes=%llu "
      "stream_unique_handles=%llu stream_unique_handle_overflows=%llu "
      "stream_unique_bytes=%llu stream_unique_dynamic_handles=%llu "
      "stream_unique_writeonly_handles=%llu "
      "stream_unique_default_pool_handles=%llu "
      "stream_unique_managed_pool_handles=%llu "
      "stream_unique_systemmem_pool_handles=%llu "
      "stream_unique_scratch_pool_handles=%llu "
      "stream_handle_changes=%llu stream_offset_changes=%llu "
      "stream_stride_changes=%llu stream0_last_handle=0x%llx "
      "stream0_last_offset=%llu stream0_last_stride=%llu "
      "ib_state_samples=%llu ib_metal_binds=%llu ib_handle_changes=%llu "
      "ib_unique_handles=%llu ib_unique_handle_overflows=%llu "
      "ib_unique_bytes=%llu ib_unique_dynamic_handles=%llu "
      "ib_unique_writeonly_handles=%llu "
      "ib_unique_default_pool_handles=%llu "
      "ib_unique_managed_pool_handles=%llu "
      "ib_unique_systemmem_pool_handles=%llu "
      "ib_unique_scratch_pool_handles=%llu "
      "ib_last_handle=0x%llx pso_state_samples=%llu "
      "pso_handle_changes=%llu pso_unique_handles=%llu "
      "pso_unique_handle_overflows=%llu pso_last_handle=0x%llx "
      "shader_variant_changes=%llu shader_variant_unique=%llu "
      "shader_variant_unique_overflows=%llu shader_variant_last=0x%llx "
      "vertex_shader_last=0x%llx pixel_shader_last=0x%llx "
      "vertex_shader_source_last=%llu pixel_shader_source_last=%llu "
      "vsout_layout_changes=%llu vsout_layout_unique=%llu "
      "vsout_layout_unique_overflows=%llu vsout_layout_last=0x%x "
      "vsout_layout_cache_hits=%llu vsout_layout_cache_misses=%llu "
      "argbuf_table_bytes=%llu argbuf_cbuf_bytes=%llu "
      "argbuf_cbuf_vs_bytes=%llu argbuf_cbuf_ffp_vs_bytes=%llu "
      "argbuf_cbuf_ps_bytes=%llu argbuf_cbuf_ffp_ps_bytes=%llu "
      "argbuf_cbuf_vs_first_bytes=%llu "
      "argbuf_cbuf_vs_rewrite_changed_bytes=%llu "
      "argbuf_cbuf_vs_rewrite_unchanged_bytes=%llu "
      "argbuf_cbuf_vs_float_changed_bytes=%llu "
      "argbuf_cbuf_vs_int_changed_bytes=%llu "
      "argbuf_cbuf_vs_bool_changed_bytes=%llu "
      "argbuf_cbuf_vs_uploads=%llu "
      "argbuf_cbuf_vs_full_struct_uploads=%llu "
      "argbuf_cbuf_vs_usage_unknown_uploads=%llu "
      "argbuf_cbuf_vs_usage_indexed_float_uploads=%llu "
      "argbuf_cbuf_vs_plan_float_regs_sum=%llu "
      "argbuf_cbuf_vs_plan_float_regs_max=%llu "
      "argbuf_cbuf_vs_dirty_float_regs_sum=%llu "
      "argbuf_cbuf_vs_dirty_float_regs_max=%llu "
      "argbuf_cbuf_vs_usage_float_regs_sum=%llu "
      "argbuf_cbuf_vs_usage_float_regs_max=%llu "
      "argbuf_cbuf_ffp_vs_first_bytes=%llu "
      "argbuf_cbuf_ffp_vs_rewrite_changed_bytes=%llu "
      "argbuf_cbuf_ffp_vs_rewrite_unchanged_bytes=%llu "
      "argbuf_cbuf_ffp_vs_matrix_changed_bytes=%llu "
      "argbuf_cbuf_ffp_vs_material_changed_bytes=%llu "
      "argbuf_cbuf_ffp_vs_light_changed_bytes=%llu "
      "argbuf_cbuf_ffp_vs_blend_changed_bytes=%llu "
      "argbuf_cbuf_ffp_vs_tex_transform_changed_bytes=%llu "
      "argbuf_cbuf_ffp_vs_clip_changed_bytes=%llu "
      "argbuf_cbuf_ffp_vs_viewport_changed_bytes=%llu "
      "argbuf_cbuf_ffp_vs_fog_point_changed_bytes=%llu "
      "set_vertex_bytes_calls=%llu set_vertex_bytes_bytes=%llu "
      "set_vertex_bytes_slot5_calls=%llu set_vertex_bytes_slot5_bytes=%llu "
      "set_vertex_bytes_other_calls=%llu set_vertex_bytes_other_bytes=%llu "
      "transient_vertex_bytes=%llu "
      "transient_vertex_user_bytes=%llu "
      "transient_vertex_preupload_bytes=%llu "
      "transient_vertex_decl_fallback_bytes=%llu "
      "transient_vertex_expanded_main_bytes=%llu "
      "transient_vertex_expanded_extra_bytes=%llu "
      "transient_vertex_staged_stream_bytes=%llu "
      "transient_index_bytes=%llu "
      "transient_index_user_bytes=%llu "
      "transient_index_preupload_bytes=%llu "
      "transient_index_shadow_fallback_bytes=%llu "
      "transient_index_probe_reorder_bytes=%llu "
      "transient_index_optimized_order_bytes=%llu "
      "transient_index_staged_ib_bytes=%llu]\n",
      static_cast<unsigned long long>(b.seqId),
      static_cast<unsigned long long>(b.encoderIndex),
      static_cast<unsigned long long>(b.rtHandle),
      static_cast<unsigned long long>(b.depthHandle),
      static_cast<unsigned long long>(b.rtFormat),
      static_cast<unsigned long long>(b.rtWidth),
      static_cast<unsigned long long>(b.rtHeight),
      static_cast<unsigned long long>(b.rtBytesPerPixel),
      static_cast<unsigned long long>(b.rtAliasTexture),
      static_cast<unsigned long long>(b.rtTextureUsage),
      static_cast<unsigned long long>(b.rtFormatNeedsShaderReadSwizzle),
      static_cast<unsigned long long>(b.rtTextureNeedsShaderReadView),
      static_cast<unsigned long long>(b.depthFormat),
      static_cast<unsigned long long>(b.depthWidth),
      static_cast<unsigned long long>(b.depthHeight),
      static_cast<unsigned long long>(b.depthBytesPerPixel),
      static_cast<unsigned long long>(b.depthAliasTexture),
      static_cast<unsigned long long>(b.depthTextureUsage),
      static_cast<unsigned long long>(b.depthFormatNeedsShaderReadSwizzle),
      static_cast<unsigned long long>(b.depthTextureNeedsShaderReadView),
      static_cast<unsigned long long>(b.colorAttachmentCount),
      static_cast<unsigned long long>(b.color0Included),
      static_cast<unsigned long long>(b.color0LoadAction),
      static_cast<unsigned long long>(b.color0StoreAction),
      static_cast<unsigned long long>(b.color0Clear),
      static_cast<unsigned long long>(b.colorLoadBytes),
      static_cast<unsigned long long>(b.colorStoreBytes),
      static_cast<unsigned long long>(b.depthIncluded),
      static_cast<unsigned long long>(b.depthLoadAction),
      static_cast<unsigned long long>(b.depthStoreAction),
      static_cast<unsigned long long>(b.depthClear),
      static_cast<unsigned long long>(b.depthLoadBytes),
      static_cast<unsigned long long>(b.depthStoreBytes),
      static_cast<unsigned long long>(b.stencilIncluded),
      static_cast<unsigned long long>(b.stencilLoadAction),
      static_cast<unsigned long long>(b.stencilStoreAction),
      static_cast<unsigned long long>(b.stencilClear),
      static_cast<unsigned long long>(b.stencilLoadBytes),
      static_cast<unsigned long long>(b.stencilStoreBytes),
      splitReasonName(b.endReason),
      static_cast<unsigned long long>(b.drawCalls),
      static_cast<unsigned long long>(b.indexedDraws),
      static_cast<unsigned long long>(b.expandedIndexedDraws),
      static_cast<unsigned long long>(b.ffpDraws),
      static_cast<unsigned long long>(b.programmableDraws),
      static_cast<unsigned long long>(b.preTransformedDraws),
      static_cast<unsigned long long>(b.texturedDraws),
      static_cast<unsigned long long>(b.cullNoneDraws),
      static_cast<unsigned long long>(b.cullFrontDraws),
      static_cast<unsigned long long>(b.cullBackDraws),
      static_cast<unsigned long long>(b.fillSolidDraws),
      static_cast<unsigned long long>(b.fillWireframeDraws),
      static_cast<unsigned long long>(b.depthEnabledDraws),
      static_cast<unsigned long long>(b.depthWriteDraws),
      static_cast<unsigned long long>(b.depthFuncLessDraws),
      static_cast<unsigned long long>(b.depthFuncLessEqualDraws),
      static_cast<unsigned long long>(b.depthFuncAlwaysDraws),
      static_cast<unsigned long long>(b.depthFuncOtherDraws),
      static_cast<unsigned long long>(b.scissorEnabledDraws),
      static_cast<unsigned long long>(b.alphaBlendEnabledDraws),
      static_cast<unsigned long long>(b.blendStateSamples),
      static_cast<unsigned long long>(b.blendStateChanges),
      static_cast<unsigned long long>(b.blendStateUnique),
      static_cast<unsigned long long>(b.blendStateUniqueOverflows),
      static_cast<unsigned long long>(b.blendStateLast),
      static_cast<unsigned long long>(b.blendEnabledNoopDraws),
      static_cast<unsigned long long>(b.blendConstantFactorDraws),
      static_cast<unsigned long long>(b.blendScreenDraws),
      static_cast<unsigned long long>(b.blendAdditiveDraws),
      static_cast<unsigned long long>(b.blendAlphaCompositeDraws),
      static_cast<unsigned long long>(b.alphaBlendTexturedDraws),
      static_cast<unsigned long long>(b.alphaBlendTexturedPrimitives),
      static_cast<unsigned long long>(b.alphaBlendTexturedVertices),
      static_cast<unsigned long long>(b.alphaBlendSmallDraws),
      static_cast<unsigned long long>(b.alphaBlendSmallPrimitives),
      static_cast<unsigned long long>(b.alphaBlendSmallVertices),
      static_cast<unsigned long long>(b.alphaTestEnabledDraws),
      static_cast<unsigned long long>(b.alphaTestEffectiveDraws),
      static_cast<unsigned long long>(b.clipPlaneEnabledDraws),
      static_cast<unsigned long long>(b.pointDraws),
      static_cast<unsigned long long>(b.lineDraws),
      static_cast<unsigned long long>(b.triangleDraws),
      static_cast<unsigned long long>(b.primitiveCount),
      static_cast<unsigned long long>(b.triangleEstimate),
      static_cast<unsigned long long>(b.vertexCount),
      static_cast<unsigned long long>(b.routeDepthOnlyDraws),
      static_cast<unsigned long long>(b.routeDepthOnlyPrimitives),
      static_cast<unsigned long long>(b.routeDepthOnlyVertices),
      static_cast<unsigned long long>(b.routeProgrammableTexturedDraws),
      static_cast<unsigned long long>(b.routeProgrammableTexturedPrimitives),
      static_cast<unsigned long long>(b.routeProgrammableTexturedVertices),
      static_cast<unsigned long long>(b.routeProgrammableColorDraws),
      static_cast<unsigned long long>(b.routeProgrammableColorPrimitives),
      static_cast<unsigned long long>(b.routeProgrammableColorVertices),
      static_cast<unsigned long long>(b.routeAlphaBlendPrimitives),
      static_cast<unsigned long long>(b.routeAlphaTestPrimitives),
      static_cast<unsigned long long>(b.tileFfpRoutedTileDraws),
      static_cast<unsigned long long>(b.tileFfpRoutedTilePrimitives),
      static_cast<unsigned long long>(b.tileFfpRoutedTileVertices),
      static_cast<unsigned long long>(b.tileFfpRoutedPortableDraws),
      static_cast<unsigned long long>(b.tileFfpRoutedPortablePrimitives),
      static_cast<unsigned long long>(b.tileFfpRoutedPortableVertices),
      static_cast<unsigned long long>(b.tileFfpEligibleDraws),
      static_cast<unsigned long long>(b.tileFfpEligiblePrimitives),
      static_cast<unsigned long long>(b.tileFfpEligibleVertices),
      static_cast<unsigned long long>(b.tileFfpFallbackGpuFamilyDraws),
      static_cast<unsigned long long>(b.tileFfpFallbackGpuFamilyPrimitives),
      static_cast<unsigned long long>(b.tileFfpFallbackNotFfpDraws),
      static_cast<unsigned long long>(b.tileFfpFallbackNotFfpPrimitives),
      static_cast<unsigned long long>(b.tileFfpFallbackPrecisionDraws),
      static_cast<unsigned long long>(b.tileFfpFallbackPrecisionPrimitives),
      static_cast<unsigned long long>(b.tileFfpFallbackUnsupportedStateDraws),
      static_cast<unsigned long long>(b.tileFfpFallbackUnsupportedStatePrimitives),
      static_cast<unsigned long long>(b.indexedTriangleOpaqueDepthWriteDraws),
      static_cast<unsigned long long>(b.indexedTriangleOpaqueDepthWritePrimitives),
      static_cast<unsigned long long>(b.indexedTriangleOpaqueDepthWriteVertices),
      static_cast<unsigned long long>(b.indexedTriangleDepthReadDraws),
      static_cast<unsigned long long>(b.indexedTriangleDepthReadPrimitives),
      static_cast<unsigned long long>(b.indexedTriangleDepthReadVertices),
      static_cast<unsigned long long>(b.indexedTriangleAlphaBlendDraws),
      static_cast<unsigned long long>(b.indexedTriangleAlphaBlendPrimitives),
      static_cast<unsigned long long>(b.indexedTriangleAlphaBlendVertices),
      static_cast<unsigned long long>(b.indexedTriangleScissorDraws),
      static_cast<unsigned long long>(b.indexedTriangleScissorPrimitives),
      static_cast<unsigned long long>(b.indexedTriangleScissorVertices),
      static_cast<unsigned long long>(b.indexedTriangleTexturedDraws),
      static_cast<unsigned long long>(b.indexedTriangleTexturedPrimitives),
      static_cast<unsigned long long>(b.indexedTriangleTexturedVertices),
      static_cast<unsigned long long>(b.indexedTriangleLarge4096Draws),
      static_cast<unsigned long long>(b.indexedTriangleLarge4096Primitives),
      static_cast<unsigned long long>(b.indexedTriangleLarge4096Vertices),
      static_cast<unsigned long long>(b.indexedTriangleLarge4096OpaqueDepthWriteDraws),
      static_cast<unsigned long long>(b.indexedTriangleLarge4096OpaqueDepthWritePrimitives),
      static_cast<unsigned long long>(b.indexedTriangleLarge4096OpaqueDepthWriteVertices),
      static_cast<unsigned long long>(b.indexedTriangleLarge4096DepthReadDraws),
      static_cast<unsigned long long>(b.indexedTriangleLarge4096DepthReadPrimitives),
      static_cast<unsigned long long>(b.indexedTriangleLarge4096DepthReadVertices),
      static_cast<unsigned long long>(b.indexedTriangleLarge4096AlphaBlendDraws),
      static_cast<unsigned long long>(b.indexedTriangleLarge4096AlphaBlendPrimitives),
      static_cast<unsigned long long>(b.indexedTriangleLarge4096AlphaBlendVertices),
      static_cast<unsigned long long>(b.indexedTriangleLarge4096ScissorDraws),
      static_cast<unsigned long long>(b.indexedTriangleLarge4096ScissorPrimitives),
      static_cast<unsigned long long>(b.indexedTriangleLarge4096ScissorVertices),
      static_cast<unsigned long long>(b.indexedTriangleLarge4096TexturedDraws),
      static_cast<unsigned long long>(b.indexedTriangleLarge4096TexturedPrimitives),
      static_cast<unsigned long long>(b.indexedTriangleLarge4096TexturedVertices),
      static_cast<unsigned long long>(b.textureMaskOr),
      static_cast<unsigned long long>(b.fragmentTextureBindingSamples),
      static_cast<unsigned long long>(b.fragmentTextureBindingMaskOr),
      static_cast<unsigned long long>(b.x8RtTextureBindingSamples),
      static_cast<unsigned long long>(b.x8RtTextureBindingMaskOr),
      static_cast<unsigned long long>(b.x8RtTextureBindingUniqueHandles),
      static_cast<unsigned long long>(b.x8RtTextureBindingUniqueHandleOverflows),
      static_cast<unsigned long long>(b.x8RtTextureBindingShaderReadViewSamples),
      static_cast<unsigned long long>(b.x8RtTextureBindingActiveRtAliasSamples),
      static_cast<unsigned long long>(b.x8ShaderAlphaFillSamples),
      static_cast<unsigned long long>(b.x8ShaderAlphaFillMaskOr),
      static_cast<unsigned long long>(b.x8RtTextureBindingLastStage),
      static_cast<unsigned long long>(b.x8RtTextureBindingLastHandle),
      static_cast<unsigned long long>(b.drawPrimitiveCountMin),
      static_cast<unsigned long long>(b.drawPrimitiveCountMax),
      static_cast<unsigned long long>(b.drawVertexCountMin),
      static_cast<unsigned long long>(b.drawVertexCountMax),
      static_cast<unsigned long long>(b.drawPrimitiveBucket1_63),
      static_cast<unsigned long long>(b.drawPrimitiveBucket64_255),
      static_cast<unsigned long long>(b.drawPrimitiveBucket256_1023),
      static_cast<unsigned long long>(b.drawPrimitiveBucket1024_4095),
      static_cast<unsigned long long>(b.drawPrimitiveBucket4096Plus),
      static_cast<unsigned long long>(b.drawVertexBucket1_255),
      static_cast<unsigned long long>(b.drawVertexBucket256_1023),
      static_cast<unsigned long long>(b.drawVertexBucket1024_4095),
      static_cast<unsigned long long>(b.drawVertexBucket4096_16383),
      static_cast<unsigned long long>(b.drawVertexBucket16384Plus),
      static_cast<unsigned long long>(b.drawGeometrySignatureSamples),
      static_cast<unsigned long long>(b.drawGeometrySignatureUnique),
      static_cast<unsigned long long>(b.drawGeometrySignatureUniqueOverflows),
      static_cast<unsigned long long>(b.drawGeometrySignatureDuplicates),
      static_cast<unsigned long long>(b.drawGeometrySignatureConsecutiveDuplicates),
      static_cast<unsigned long long>(b.drawGeometrySignatureLast),
      static_cast<unsigned long long>(b.indexedBaseVertexSamples),
      static_cast<unsigned long long>(b.indexedBaseVertexNonZeroDraws),
      static_cast<unsigned long long>(b.indexedBaseVertexNegativeDraws),
      static_cast<unsigned long long>(b.indexedBaseVertexPositiveDraws),
      static_cast<long long>(b.indexedBaseVertexMin),
      static_cast<long long>(b.indexedBaseVertexMax),
      static_cast<unsigned long long>(b.nativeBaseVertexRequestedDraws),
      static_cast<unsigned long long>(b.nativeBaseVertexUsedDraws),
      static_cast<unsigned long long>(b.nativeBaseVertexSkippedNegativeDraws),
      static_cast<unsigned long long>(b.splitLargeIndexedSourceDraws),
      static_cast<unsigned long long>(b.splitLargeIndexedMetalDraws),
      static_cast<unsigned long long>(b.splitLargeIndexedExtraDraws),
      static_cast<unsigned long long>(b.splitLargeIndexedPrimitiveLimit),
      static_cast<unsigned long long>(b.splitLargeIndexedStream0SpanLimit),
      static_cast<unsigned long long>(b.splitLargeIndexedChunkStream0SpanMax),
      static_cast<unsigned long long>(b.splitLargeIndexedPrimitiveCount),
      static_cast<unsigned long long>(b.indexedOrderProbeDraws),
      static_cast<unsigned long long>(b.indexedOrderProbeSkipped),
      static_cast<unsigned long long>(b.indexedOrderProbeBytes),
      static_cast<unsigned long long>(b.indexedOrderOptimizedDraws),
      static_cast<unsigned long long>(b.indexedOrderOptimizedSkipped),
      static_cast<unsigned long long>(b.indexedOrderOptimizedBytes),
      static_cast<unsigned long long>(b.probeScissorRectDraws),
      static_cast<unsigned long long>(b.probeScissorRectSkipped),
      static_cast<unsigned long long>(b.probeScissorRectAreaDeltaPixels),
      static_cast<unsigned long long>(b.probeDisableAlphaBlendDraws),
      static_cast<unsigned long long>(b.probeDisableDepthWriteDraws),
      static_cast<unsigned long long>(b.probeDepthFuncAlwaysDraws),
      static_cast<unsigned long long>(b.probeForceTextureWhiteDraws),
      static_cast<unsigned long long>(b.probeFragmentlessDepthOnlyDraws),
      static_cast<unsigned long long>(b.probeFragmentlessDepthOnlyPrimitives),
      static_cast<unsigned long long>(b.probeFragmentlessDepthOnlyVertices),
      static_cast<unsigned long long>(b.indexedVertexReuseSamples),
      static_cast<unsigned long long>(b.indexedVertexReuseSkipped),
      static_cast<unsigned long long>(b.indexedVertexReferenceCount),
      static_cast<unsigned long long>(b.indexedUniqueVertexEstimate),
      static_cast<unsigned long long>(b.indexedVertexCacheMissEstimate16),
      static_cast<unsigned long long>(b.indexedVertexCacheMissEstimate32),
      static_cast<unsigned long long>(b.indexedVertexCacheMissEstimate64),
      static_cast<unsigned long long>(b.indexedCacheOptCandidateDraws),
      static_cast<unsigned long long>(b.indexedCacheOptCandidateSkipped),
      static_cast<unsigned long long>(b.indexedCacheOptCandidateBytes),
      static_cast<unsigned long long>(b.indexedCacheOptCandidateOriginalMiss16),
      static_cast<unsigned long long>(b.indexedCacheOptCandidateOriginalMiss32),
      static_cast<unsigned long long>(b.indexedCacheOptCandidateOriginalMiss64),
      static_cast<unsigned long long>(b.indexedCacheOptCandidateMiss16),
      static_cast<unsigned long long>(b.indexedCacheOptCandidateMiss32),
      static_cast<unsigned long long>(b.indexedCacheOptCandidateMiss64),
      static_cast<unsigned long long>(b.indexedCacheOptCandidateGatePass),
      static_cast<unsigned long long>(b.indexedCacheOptCandidateGateFail),
      static_cast<unsigned long long>(b.indexedCacheOptCandidateOpaqueDepthDraws),
      static_cast<unsigned long long>(b.indexedCacheOptCandidateScreenBlendDraws),
      static_cast<unsigned long long>(b.indexedCacheOptCandidatePrimitiveBucket1_63),
      static_cast<unsigned long long>(b.indexedCacheOptCandidatePrimitiveBucket64_255),
      static_cast<unsigned long long>(b.indexedCacheOptCandidatePrimitiveBucket256_1023),
      static_cast<unsigned long long>(b.indexedCacheOptCandidatePrimitiveBucket1024_4095),
      static_cast<unsigned long long>(b.indexedCacheOptCandidatePrimitiveBucket4096Plus),
      static_cast<unsigned long long>(b.reorderedIndexCacheLookups),
      static_cast<unsigned long long>(b.reorderedIndexCacheHits),
      static_cast<unsigned long long>(b.reorderedIndexCacheRejectedHits),
      static_cast<unsigned long long>(b.reorderedIndexCacheMisses),
      static_cast<unsigned long long>(b.reorderedIndexCacheCreated),
      static_cast<unsigned long long>(b.reorderedIndexCacheCreatedBytes),
      static_cast<unsigned long long>(b.stream0StrideMin),
      static_cast<unsigned long long>(b.stream0StrideMax),
      static_cast<unsigned long long>(b.streamStateSamples),
      static_cast<unsigned long long>(b.streamMetalBinds),
      static_cast<unsigned long long>(b.streamMetalBindFirsts),
      static_cast<unsigned long long>(b.streamMetalBindHandleChanges),
      static_cast<unsigned long long>(b.streamMetalBindOffsetChanges),
      static_cast<unsigned long long>(b.streamUniqueHandles),
      static_cast<unsigned long long>(b.streamUniqueHandleOverflows),
      static_cast<unsigned long long>(b.streamUniqueBytes),
      static_cast<unsigned long long>(b.streamUniqueDynamicHandles),
      static_cast<unsigned long long>(b.streamUniqueWriteOnlyHandles),
      static_cast<unsigned long long>(b.streamUniqueDefaultPoolHandles),
      static_cast<unsigned long long>(b.streamUniqueManagedPoolHandles),
      static_cast<unsigned long long>(b.streamUniqueSystemMemPoolHandles),
      static_cast<unsigned long long>(b.streamUniqueScratchPoolHandles),
      static_cast<unsigned long long>(b.streamHandleChanges),
      static_cast<unsigned long long>(b.streamOffsetChanges),
      static_cast<unsigned long long>(b.streamStrideChanges),
      static_cast<unsigned long long>(b.stream0LastHandle),
      static_cast<unsigned long long>(b.stream0LastOffset),
      static_cast<unsigned long long>(b.stream0LastStride),
      static_cast<unsigned long long>(b.ibStateSamples),
      static_cast<unsigned long long>(b.ibMetalBinds),
      static_cast<unsigned long long>(b.ibHandleChanges),
      static_cast<unsigned long long>(b.ibUniqueHandles),
      static_cast<unsigned long long>(b.ibUniqueHandleOverflows),
      static_cast<unsigned long long>(b.ibUniqueBytes),
      static_cast<unsigned long long>(b.ibUniqueDynamicHandles),
      static_cast<unsigned long long>(b.ibUniqueWriteOnlyHandles),
      static_cast<unsigned long long>(b.ibUniqueDefaultPoolHandles),
      static_cast<unsigned long long>(b.ibUniqueManagedPoolHandles),
      static_cast<unsigned long long>(b.ibUniqueSystemMemPoolHandles),
      static_cast<unsigned long long>(b.ibUniqueScratchPoolHandles),
      static_cast<unsigned long long>(b.ibLastHandle),
      static_cast<unsigned long long>(b.psoStateSamples),
      static_cast<unsigned long long>(b.psoHandleChanges),
      static_cast<unsigned long long>(b.psoUniqueHandles),
      static_cast<unsigned long long>(b.psoUniqueHandleOverflows),
      static_cast<unsigned long long>(b.psoLastHandle),
      static_cast<unsigned long long>(b.shaderVariantChanges),
      static_cast<unsigned long long>(b.shaderVariantUnique),
      static_cast<unsigned long long>(b.shaderVariantUniqueOverflows),
      static_cast<unsigned long long>(b.shaderVariantLast),
      static_cast<unsigned long long>(b.vertexShaderLast),
      static_cast<unsigned long long>(b.pixelShaderLast),
      static_cast<unsigned long long>(b.vertexShaderSourceLast),
      static_cast<unsigned long long>(b.pixelShaderSourceLast),
      static_cast<unsigned long long>(b.vsOutLayoutChanges),
      static_cast<unsigned long long>(b.vsOutLayoutUnique),
      static_cast<unsigned long long>(b.vsOutLayoutUniqueOverflows),
      static_cast<unsigned>(b.vsOutLayoutLast),
      static_cast<unsigned long long>(b.vsOutLayoutCacheHits),
      static_cast<unsigned long long>(b.vsOutLayoutCacheMisses),
      static_cast<unsigned long long>(b.argbufTableBytes),
      static_cast<unsigned long long>(b.argbufCbufBytes),
      static_cast<unsigned long long>(b.argbufCbufVsBytes),
      static_cast<unsigned long long>(b.argbufCbufFfpVsBytes),
      static_cast<unsigned long long>(b.argbufCbufPsBytes),
      static_cast<unsigned long long>(b.argbufCbufFfpPsBytes),
      static_cast<unsigned long long>(b.argbufCbufVsFirstBytes),
      static_cast<unsigned long long>(b.argbufCbufVsRewriteChangedBytes),
      static_cast<unsigned long long>(b.argbufCbufVsRewriteUnchangedBytes),
      static_cast<unsigned long long>(b.argbufCbufVsFloatChangedBytes),
      static_cast<unsigned long long>(b.argbufCbufVsIntChangedBytes),
      static_cast<unsigned long long>(b.argbufCbufVsBoolChangedBytes),
      static_cast<unsigned long long>(b.argbufCbufVsUploads),
      static_cast<unsigned long long>(b.argbufCbufVsFullStructUploads),
      static_cast<unsigned long long>(b.argbufCbufVsUsageUnknownUploads),
      static_cast<unsigned long long>(b.argbufCbufVsUsageIndexedFloatUploads),
      static_cast<unsigned long long>(b.argbufCbufVsPlanFloatRegsSum),
      static_cast<unsigned long long>(b.argbufCbufVsPlanFloatRegsMax),
      static_cast<unsigned long long>(b.argbufCbufVsDirtyFloatRegsSum),
      static_cast<unsigned long long>(b.argbufCbufVsDirtyFloatRegsMax),
      static_cast<unsigned long long>(b.argbufCbufVsUsageFloatRegsSum),
      static_cast<unsigned long long>(b.argbufCbufVsUsageFloatRegsMax),
      static_cast<unsigned long long>(b.argbufCbufFfpVsFirstBytes),
      static_cast<unsigned long long>(b.argbufCbufFfpVsRewriteChangedBytes),
      static_cast<unsigned long long>(b.argbufCbufFfpVsRewriteUnchangedBytes),
      static_cast<unsigned long long>(b.argbufCbufFfpVsMatrixChangedBytes),
      static_cast<unsigned long long>(b.argbufCbufFfpVsMaterialChangedBytes),
      static_cast<unsigned long long>(b.argbufCbufFfpVsLightChangedBytes),
      static_cast<unsigned long long>(b.argbufCbufFfpVsBlendChangedBytes),
      static_cast<unsigned long long>(b.argbufCbufFfpVsTexTransformChangedBytes),
      static_cast<unsigned long long>(b.argbufCbufFfpVsClipChangedBytes),
      static_cast<unsigned long long>(b.argbufCbufFfpVsViewportChangedBytes),
      static_cast<unsigned long long>(b.argbufCbufFfpVsFogPointChangedBytes),
      static_cast<unsigned long long>(b.setVertexBytesCalls),
      static_cast<unsigned long long>(b.setVertexBytesBytes),
      static_cast<unsigned long long>(b.setVertexBytesSlot5Calls),
      static_cast<unsigned long long>(b.setVertexBytesSlot5Bytes),
      static_cast<unsigned long long>(b.setVertexBytesOtherCalls),
      static_cast<unsigned long long>(b.setVertexBytesOtherBytes),
      static_cast<unsigned long long>(b.transientVertexBytes),
      static_cast<unsigned long long>(b.transientVertexUserBytes),
      static_cast<unsigned long long>(b.transientVertexPreuploadBytes),
      static_cast<unsigned long long>(b.transientVertexDeclFallbackBytes),
      static_cast<unsigned long long>(b.transientVertexExpandedMainBytes),
      static_cast<unsigned long long>(b.transientVertexExpandedExtraBytes),
      static_cast<unsigned long long>(b.transientVertexStagedStreamBytes),
      static_cast<unsigned long long>(b.transientIndexBytes),
      static_cast<unsigned long long>(b.transientIndexUserBytes),
      static_cast<unsigned long long>(b.transientIndexPreuploadBytes),
      static_cast<unsigned long long>(b.transientIndexShadowFallbackBytes),
      static_cast<unsigned long long>(b.transientIndexProbeReorderBytes),
      static_cast<unsigned long long>(b.transientIndexOptimizedOrderBytes),
      static_cast<unsigned long long>(b.transientIndexStagedIbBytes));
  for (std::size_t i = 0; i < b.streams.size(); ++i) {
    const auto& s = b.streams[i];
    if (!s.valid) {
      continue;
    }
    std::fprintf(
        stderr,
        "[dxmt9-perf-encoder-stream seq=%llu encoder=%llu stream=%zu "
        "samples=%llu metal_binds=%llu metal_bind_firsts=%llu "
        "metal_bind_handle_changes=%llu metal_bind_offset_changes=%llu "
        "unique_handles=%llu unique_handle_overflows=%llu unique_bytes=%llu "
        "unique_dynamic_handles=%llu unique_writeonly_handles=%llu "
        "unique_default_pool_handles=%llu unique_managed_pool_handles=%llu "
        "unique_systemmem_pool_handles=%llu unique_scratch_pool_handles=%llu "
        "handle_changes=%llu "
        "offset_changes=%llu stride_changes=%llu last_handle=0x%llx "
        "last_offset=%llu last_stride=%llu]\n",
        static_cast<unsigned long long>(b.seqId),
        static_cast<unsigned long long>(b.encoderIndex),
        i,
        static_cast<unsigned long long>(s.samples),
        static_cast<unsigned long long>(s.metalBinds),
        static_cast<unsigned long long>(s.metalBindFirsts),
        static_cast<unsigned long long>(s.metalBindHandleChanges),
        static_cast<unsigned long long>(s.metalBindOffsetChanges),
        static_cast<unsigned long long>(s.uniqueHandles),
        static_cast<unsigned long long>(s.uniqueHandleOverflows),
        static_cast<unsigned long long>(s.uniqueBytes),
        static_cast<unsigned long long>(s.uniqueDynamicHandles),
        static_cast<unsigned long long>(s.uniqueWriteOnlyHandles),
        static_cast<unsigned long long>(s.uniqueDefaultPoolHandles),
        static_cast<unsigned long long>(s.uniqueManagedPoolHandles),
        static_cast<unsigned long long>(s.uniqueSystemMemPoolHandles),
        static_cast<unsigned long long>(s.uniqueScratchPoolHandles),
        static_cast<unsigned long long>(s.handleChanges),
        static_cast<unsigned long long>(s.offsetChanges),
        static_cast<unsigned long long>(s.strideChanges),
        static_cast<unsigned long long>(s.lastHandle),
        static_cast<unsigned long long>(s.lastOffset),
        static_cast<unsigned long long>(s.lastStride));
  }
}

CounterSnapshot snapshot() {
  const Counters& c = counters();
  CounterSnapshot s;
  s.submitDraw = load(c.submitDraw);
  s.submitClear = load(c.submitClear);
  s.submitStretch = load(c.submitStretch);
  s.submitPresent = load(c.submitPresent);
  s.submitFlush = load(c.submitFlush);
  s.commandBuffers = load(c.commandBuffers);
  s.renderPassBegin = load(c.renderPassBegin);
  s.renderPassEnd = load(c.renderPassEnd);
  s.drawCalls = load(c.drawCalls);
  s.drawIndexedCalls = load(c.drawIndexedCalls);
  s.drawPrimitiveCount = load(c.drawPrimitiveCount);
  s.drawTriangleEstimate = load(c.drawTriangleEstimate);
  s.drawVertexCount = load(c.drawVertexCount);
  s.bindPipeline = load(c.bindPipeline);
  s.presentEncoded = load(c.presentEncoded);
  s.submitDrawCpuNs = load(c.submitDrawCpuNs);
  s.encodeChunkCalls = load(c.encodeChunkCalls);
  s.encodeChunkCpuNs = load(c.encodeChunkCpuNs);
  s.encodeDrawCpuNs = load(c.encodeDrawCpuNs);
  s.encodeDrawPipelineLookupCpuNs = load(c.encodeDrawPipelineLookupCpuNs);
  s.encodeDrawUniformBuildCpuNs = load(c.encodeDrawUniformBuildCpuNs);
  s.encodeDrawFvfDecodeCpuNs = load(c.encodeDrawFvfDecodeCpuNs);
  s.encodeDrawBindingPacketCpuNs = load(c.encodeDrawBindingPacketCpuNs);
  s.encodeDrawBindingPacketPlanCpuNs = load(c.encodeDrawBindingPacketPlanCpuNs);
  s.encodeDrawBindingPacketPlanFragmentCpuNs =
      load(c.encodeDrawBindingPacketPlanFragmentCpuNs);
  s.encodeDrawBindingPacketPlanVertexCpuNs =
      load(c.encodeDrawBindingPacketPlanVertexCpuNs);
  s.encodeDrawBindingPacketPlanExtraStreamCpuNs =
      load(c.encodeDrawBindingPacketPlanExtraStreamCpuNs);
  s.encodeDrawBindingPacketPlanRasterCpuNs =
      load(c.encodeDrawBindingPacketPlanRasterCpuNs);
  s.encodeDrawBindingPacketCacheCpuNs = load(c.encodeDrawBindingPacketCacheCpuNs);
  s.encodeDrawBindingPacketTextureRecordCpuNs =
      load(c.encodeDrawBindingPacketTextureRecordCpuNs);
  s.encodeDrawArgbufSetupCpuNs = load(c.encodeDrawArgbufSetupCpuNs);
  s.encodeDrawArgbufOpenCpuNs = load(c.encodeDrawArgbufOpenCpuNs);
  s.encodeDrawArgbufCbufUpdateCpuNs = load(c.encodeDrawArgbufCbufUpdateCpuNs);
  s.encodeDrawStreamBindCpuNs = load(c.encodeDrawStreamBindCpuNs);
  s.encodeDrawIssueCpuNs = load(c.encodeDrawIssueCpuNs);
  s.encodeDrawStreamBindViewportCpuNs = load(c.encodeDrawStreamBindViewportCpuNs);
  s.encodeDrawStreamBindFfpCpuNs = load(c.encodeDrawStreamBindFfpCpuNs);
  s.encodeDrawStreamBindVsCpuNs = load(c.encodeDrawStreamBindVsCpuNs);
  s.encodeDrawStreamBindTextureCpuNs = load(c.encodeDrawStreamBindTextureCpuNs);
  s.encodeDrawStreamBindIndexCpuNs = load(c.encodeDrawStreamBindIndexCpuNs);
  s.encodeDrawFvfDecodeDeclCpuNs = load(c.encodeDrawFvfDecodeDeclCpuNs);
  s.encodeDrawFvfDecodeBytesCpuNs = load(c.encodeDrawFvfDecodeBytesCpuNs);
  s.encodeDrawFvfDecodeExpandedCpuNs = load(c.encodeDrawFvfDecodeExpandedCpuNs);
  s.encodeDrawUniformBuildMainCpuNs = load(c.encodeDrawUniformBuildMainCpuNs);
  s.encodeDrawUniformBuildFfpCpuNs = load(c.encodeDrawUniformBuildFfpCpuNs);
  s.encodeDrawUniformBuildVsCpuNs = load(c.encodeDrawUniformBuildVsCpuNs);
  s.encodeDrawPhaseSetupCpuNs = load(c.encodeDrawPhaseSetupCpuNs);
  s.encodeDrawPhaseArgbufUniformCpuNs = load(c.encodeDrawPhaseArgbufUniformCpuNs);
  s.encodeDrawPhaseStreamPrepCpuNs = load(c.encodeDrawPhaseStreamPrepCpuNs);
  s.encodeDrawPhaseFfpVertexCpuNs = load(c.encodeDrawPhaseFfpVertexCpuNs);
  s.encodeDrawPhaseVertexBindCpuNs = load(c.encodeDrawPhaseVertexBindCpuNs);
  s.encodeDrawPhaseBaseStateCpuNs = load(c.encodeDrawPhaseBaseStateCpuNs);
  s.encodeDrawPhaseTileFfpFallthroughCpuNs = load(c.encodeDrawPhaseTileFfpFallthroughCpuNs);
  s.encodeDrawPhaseRemainderCpuNs = load(c.encodeDrawPhaseRemainderCpuNs);
  s.transientUploadCpuNs = load(c.transientUploadCpuNs);
  s.commandBufferCreateCpuNs = load(c.commandBufferCreateCpuNs);
  s.commandBufferCommitCpuNs = load(c.commandBufferCommitCpuNs);
  s.completionWaitNs = load(c.completionWaitNs);
  s.presentAcquireWaitNs = load(c.presentAcquireWaitNs);
  s.presentBoundaryWaitNs = load(c.presentBoundaryWaitNs);
  s.presentTokenWaitNs = load(c.presentTokenWaitNs);
  s.gpuCommandBufferTimeNs = load(c.gpuCommandBufferTimeNs);
  s.gpuCommandBufferTimeSamples = load(c.gpuCommandBufferTimeSamples);
  s.renderEncoderGpuTimeNs = load(c.renderEncoderGpuTimeNs);
  s.renderEncoderGpuTimeSamples = load(c.renderEncoderGpuTimeSamples);
  s.renderEncoderGpuLastPassType = load(c.renderEncoderGpuLastPassType);
  s.renderEncoderGpuLastRt = load(c.renderEncoderGpuLastRt);
  s.renderEncoderGpuLastDepth = load(c.renderEncoderGpuLastDepth);
  s.renderEncoderGpuLastPso = load(c.renderEncoderGpuLastPso);
  s.gpuCommandBufferErrors = load(c.gpuCommandBufferErrors);
  s.subCommandBufferCommits = load(c.subCommandBufferCommits);
  return s;
}

void emitFrameDelta(std::uint64_t frameId,
                    const CounterSnapshot& prev,
                    const CounterSnapshot& curr) {
  using Clock = std::chrono::steady_clock;
  static thread_local Clock::time_point prevEmitTime{};
  const auto now = Clock::now();
  double wallMs = 0.0;
  if (prevEmitTime != Clock::time_point{}) {
    wallMs =
        std::chrono::duration<double, std::milli>(now - prevEmitTime).count();
  }
  prevEmitTime = now;
  const double fps = wallMs > 0.0 ? 1000.0 / wallMs : 0.0;

  auto delta = [](std::uint64_t a, std::uint64_t b) -> std::uint64_t {
    // Atomic loads are relaxed and the snapshot covers many counters,
    // so a later snapshot field could lag a prior one if a writer is
    // mid-update — clamp to 0 instead of wrapping.
    return b >= a ? b - a : 0ull;
  };
  std::fprintf(
      stderr,
      "[dxmt9-perf-frame frame=%llu "
      "wall_ms=%.3f fps=%.3f "
      "submit_draw=%llu submit_clear=%llu submit_stretch=%llu "
      "submit_present=%llu submit_flush=%llu command_buffers=%llu "
      "render_pass_begin=%llu render_pass_end=%llu "
      "draw_calls=%llu draw_indexed=%llu draw_primitives=%llu "
      "draw_triangles=%llu draw_vertices=%llu bind_pipeline=%llu "
      "present_encoded=%llu "
      "submit_draw_cpu_ms=%.3f encode_chunk_calls=%llu "
      "encode_chunk_cpu_ms=%.3f encode_draw_cpu_ms=%.3f "
      "encode_draw_pipeline_lookup_cpu_ms=%.3f "
      "encode_draw_uniform_build_cpu_ms=%.3f "
      "encode_draw_fvf_decode_cpu_ms=%.3f "
      "encode_draw_binding_packet_cpu_ms=%.3f "
      "encode_draw_binding_packet_plan_cpu_ms=%.3f "
      "encode_draw_binding_packet_plan_fragment_cpu_ms=%.3f "
      "encode_draw_binding_packet_plan_vertex_cpu_ms=%.3f "
      "encode_draw_binding_packet_plan_extra_stream_cpu_ms=%.3f "
      "encode_draw_binding_packet_plan_raster_cpu_ms=%.3f "
      "encode_draw_binding_packet_cache_cpu_ms=%.3f "
      "encode_draw_binding_packet_texture_record_cpu_ms=%.3f "
      "encode_draw_argbuf_setup_cpu_ms=%.3f "
      "encode_draw_argbuf_open_cpu_ms=%.3f "
      "encode_draw_argbuf_cbuf_update_cpu_ms=%.3f "
      "encode_draw_stream_bind_cpu_ms=%.3f "
      "encode_draw_issue_cpu_ms=%.3f transient_upload_cpu_ms=%.3f "
      "command_buffer_create_cpu_ms=%.3f command_buffer_commit_cpu_ms=%.3f "
      "completion_wait_ms=%.3f present_acquire_wait_ms=%.3f "
      "present_boundary_wait_ms=%.3f present_token_wait_ms=%.3f "
      "gpu_command_buffer_time_ms=%.3f gpu_command_buffer_time_samples=%llu "
      "render_encoder_gpu_time_ms=%.3f render_encoder_gpu_time_samples=%llu "
      "gpu_command_buffer_errors=%llu sub_command_buffers=%llu]\n",
      static_cast<unsigned long long>(frameId),
      wallMs,
      fps,
      static_cast<unsigned long long>(delta(prev.submitDraw, curr.submitDraw)),
      static_cast<unsigned long long>(delta(prev.submitClear, curr.submitClear)),
      static_cast<unsigned long long>(delta(prev.submitStretch, curr.submitStretch)),
      static_cast<unsigned long long>(delta(prev.submitPresent, curr.submitPresent)),
      static_cast<unsigned long long>(delta(prev.submitFlush, curr.submitFlush)),
      static_cast<unsigned long long>(delta(prev.commandBuffers, curr.commandBuffers)),
      static_cast<unsigned long long>(delta(prev.renderPassBegin, curr.renderPassBegin)),
      static_cast<unsigned long long>(delta(prev.renderPassEnd, curr.renderPassEnd)),
      static_cast<unsigned long long>(delta(prev.drawCalls, curr.drawCalls)),
      static_cast<unsigned long long>(delta(prev.drawIndexedCalls, curr.drawIndexedCalls)),
      static_cast<unsigned long long>(delta(prev.drawPrimitiveCount, curr.drawPrimitiveCount)),
      static_cast<unsigned long long>(delta(prev.drawTriangleEstimate, curr.drawTriangleEstimate)),
      static_cast<unsigned long long>(delta(prev.drawVertexCount, curr.drawVertexCount)),
      static_cast<unsigned long long>(delta(prev.bindPipeline, curr.bindPipeline)),
      static_cast<unsigned long long>(delta(prev.presentEncoded, curr.presentEncoded)),
      static_cast<double>(delta(prev.submitDrawCpuNs, curr.submitDrawCpuNs)) / 1000000.0,
      static_cast<unsigned long long>(delta(prev.encodeChunkCalls, curr.encodeChunkCalls)),
      static_cast<double>(delta(prev.encodeChunkCpuNs, curr.encodeChunkCpuNs)) / 1000000.0,
      static_cast<double>(delta(prev.encodeDrawCpuNs, curr.encodeDrawCpuNs)) / 1000000.0,
      static_cast<double>(delta(prev.encodeDrawPipelineLookupCpuNs,
                                 curr.encodeDrawPipelineLookupCpuNs)) /
          1000000.0,
      static_cast<double>(delta(prev.encodeDrawUniformBuildCpuNs,
                                 curr.encodeDrawUniformBuildCpuNs)) /
          1000000.0,
      static_cast<double>(delta(prev.encodeDrawFvfDecodeCpuNs,
                                 curr.encodeDrawFvfDecodeCpuNs)) /
          1000000.0,
      static_cast<double>(delta(prev.encodeDrawBindingPacketCpuNs,
                                 curr.encodeDrawBindingPacketCpuNs)) /
          1000000.0,
      static_cast<double>(delta(prev.encodeDrawBindingPacketPlanCpuNs,
                                 curr.encodeDrawBindingPacketPlanCpuNs)) /
          1000000.0,
      static_cast<double>(
          delta(prev.encodeDrawBindingPacketPlanFragmentCpuNs,
                curr.encodeDrawBindingPacketPlanFragmentCpuNs)) /
          1000000.0,
      static_cast<double>(
          delta(prev.encodeDrawBindingPacketPlanVertexCpuNs,
                curr.encodeDrawBindingPacketPlanVertexCpuNs)) /
          1000000.0,
      static_cast<double>(
          delta(prev.encodeDrawBindingPacketPlanExtraStreamCpuNs,
                curr.encodeDrawBindingPacketPlanExtraStreamCpuNs)) /
          1000000.0,
      static_cast<double>(
          delta(prev.encodeDrawBindingPacketPlanRasterCpuNs,
                curr.encodeDrawBindingPacketPlanRasterCpuNs)) /
          1000000.0,
      static_cast<double>(delta(prev.encodeDrawBindingPacketCacheCpuNs,
                                 curr.encodeDrawBindingPacketCacheCpuNs)) /
          1000000.0,
      static_cast<double>(
          delta(prev.encodeDrawBindingPacketTextureRecordCpuNs,
                curr.encodeDrawBindingPacketTextureRecordCpuNs)) /
          1000000.0,
      static_cast<double>(delta(prev.encodeDrawArgbufSetupCpuNs,
                                 curr.encodeDrawArgbufSetupCpuNs)) /
          1000000.0,
      static_cast<double>(delta(prev.encodeDrawArgbufOpenCpuNs,
                                 curr.encodeDrawArgbufOpenCpuNs)) /
          1000000.0,
      static_cast<double>(delta(prev.encodeDrawArgbufCbufUpdateCpuNs,
                                 curr.encodeDrawArgbufCbufUpdateCpuNs)) /
          1000000.0,
      static_cast<double>(delta(prev.encodeDrawStreamBindCpuNs,
                                 curr.encodeDrawStreamBindCpuNs)) /
          1000000.0,
      static_cast<double>(delta(prev.encodeDrawIssueCpuNs, curr.encodeDrawIssueCpuNs)) /
          1000000.0,
      static_cast<double>(delta(prev.transientUploadCpuNs, curr.transientUploadCpuNs)) /
          1000000.0,
      static_cast<double>(delta(prev.commandBufferCreateCpuNs,
                                 curr.commandBufferCreateCpuNs)) /
          1000000.0,
      static_cast<double>(delta(prev.commandBufferCommitCpuNs,
                                 curr.commandBufferCommitCpuNs)) /
          1000000.0,
      static_cast<double>(delta(prev.completionWaitNs, curr.completionWaitNs)) / 1000000.0,
      static_cast<double>(delta(prev.presentAcquireWaitNs, curr.presentAcquireWaitNs)) /
          1000000.0,
      static_cast<double>(delta(prev.presentBoundaryWaitNs, curr.presentBoundaryWaitNs)) /
          1000000.0,
      static_cast<double>(delta(prev.presentTokenWaitNs, curr.presentTokenWaitNs)) /
          1000000.0,
      static_cast<double>(delta(prev.gpuCommandBufferTimeNs, curr.gpuCommandBufferTimeNs)) /
          1000000.0,
      static_cast<unsigned long long>(
          delta(prev.gpuCommandBufferTimeSamples, curr.gpuCommandBufferTimeSamples)),
      static_cast<double>(delta(prev.renderEncoderGpuTimeNs, curr.renderEncoderGpuTimeNs)) /
          1000000.0,
      static_cast<unsigned long long>(
          delta(prev.renderEncoderGpuTimeSamples, curr.renderEncoderGpuTimeSamples)),
      static_cast<unsigned long long>(
          delta(prev.gpuCommandBufferErrors, curr.gpuCommandBufferErrors)),
      static_cast<unsigned long long>(
          delta(prev.subCommandBufferCommits, curr.subCommandBufferCommits)));
}

}  // namespace dxmt9::perf
