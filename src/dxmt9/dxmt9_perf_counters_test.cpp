#include "dxmt9_perf_counters.hpp"

#include "dxmt9_perf_counters_internal.hpp"

namespace dxmt9::perf::test {

using detail::Counters;
using detail::counters;
using detail::load;

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
}  // namespace dxmt9::perf::test
