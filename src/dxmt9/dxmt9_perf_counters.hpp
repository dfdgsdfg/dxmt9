#pragma once

#include "dxmt9_session_finalize_cause.hpp"
#include "dxmt9/core_compat_draw_batch.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace dxmt9::encoders {
enum class ProductionPartitionFallbackReason : std::uint8_t;
enum class ParallelPassBindingRejectReason : std::uint8_t;
enum class ParallelPassDirectBindingMode : std::uint8_t;
struct ParallelPassEconomicsDecision;
struct ParallelPassEconomicsSummary;
struct ParallelPassExecutionDecision;
struct SealedParallelPassSnapshotBatchResult;
struct ParallelPassAdapterDecision;
}  // namespace dxmt9::encoders

namespace dxmt9::render {
enum class FirstLeaseCapacityWaitAction : std::uint8_t;
enum class FirstLeaseReadyHeadEligibility : std::uint8_t;
enum class PartitionExecutionMode : std::uint8_t;
enum class PartitionModeRequest : std::uint8_t;
}  // namespace dxmt9::render

namespace dxmt9::perf {

bool enabled();

enum class OffloadReplayStage : std::uint8_t {
  Done = 0,
  Plan = 1,
  ArenaAdmission = 2,
  Encode = 3,
};

struct SchedulingProgressFrontierSnapshot {
  std::uint64_t cpuReadyFirstLeaseWaitEnter = 0;
  std::uint64_t cpuReadyFirstLeaseWaitCurrent = 0;
  std::uint64_t cpuReadyFirstLeaseActionRetryGeneration = 0;
  std::uint64_t cpuReadyFirstLeaseActionPressureSerial = 0;
  std::uint64_t cpuReadyFirstLeaseActionProducerWaitSerial = 0;
  std::uint64_t cpuReadyFirstLeaseActionStop = 0;
  std::uint64_t cpuReadyFirstLeaseWaitNoAdmissionPressure = 0;
  std::uint64_t cpuReadyFirstLeaseWaitCreditExhausted = 0;
  std::uint64_t cpuReadyFirstLeaseIneligibleNonArena = 0;
  std::uint64_t cpuReadyFirstLeaseIneligiblePresent = 0;
  std::uint64_t cpuReadyFirstLeaseIneligibleOrdinaryCapacity = 0;
  std::uint64_t cpuReadyFirstLeaseIneligibleHighWater = 0;
  std::uint64_t cpuReadyFirstLeaseCreditRearmed = 0;
  std::uint64_t cpuReadyFirstLeaseObservedGeneration = 0;
  std::uint64_t cpuReadyFirstLeaseCurrentGeneration = 0;
  std::uint64_t cpuReadyFirstLeaseHeadSeq = 0;
  std::uint64_t cpuReadyFirstLeaseHeadSourceOrdinal = 0;
  std::uint64_t cpuReadyArenaAdmissionWaitEnter = 0;
  std::uint64_t cpuReadyArenaAdmissionWaitCurrent = 0;
  std::uint64_t cpuReadyArenaAdmissionExitRetry = 0;
  std::uint64_t cpuReadyArenaAdmissionExitStop = 0;
  std::uint64_t offloadDrainWaitCurrent = 0;
  std::uint64_t offloadPushWaitCurrent = 0;
  std::uint64_t offloadReplayInflightRaw = 0;
  std::uint64_t offloadReplayStage = 0;
};

void recordCpuReadyFirstLeaseEligibility(
    render::FirstLeaseReadyHeadEligibility eligibility);
void enterCpuReadyFirstLeaseWait(
    bool admissionPressure, bool serialProgressAvailable,
    std::uint64_t observedGeneration, std::uint64_t currentGeneration,
    std::uint64_t headSeq, std::uint64_t headSourceOrdinal);
void updateCpuReadyFirstLeaseWaitGenerations(
    std::uint64_t observedGeneration, std::uint64_t currentGeneration);
void exitCpuReadyFirstLeaseWait(
    render::FirstLeaseCapacityWaitAction action);
void countCpuReadyFirstLeaseCreditRearmed();
void enterCpuReadyArenaAdmissionWait();
void exitCpuReadyArenaAdmissionWait(bool retry);
void enterOffloadDrainWait();
void exitOffloadDrainWait();
void enterOffloadPushWait();
void exitOffloadPushWait();
void recordOffloadReplayInflightRaw(bool inFlight);
void recordOffloadReplayStage(OffloadReplayStage stage);
SchedulingProgressFrontierSnapshot snapshotSchedulingProgressFrontier();
void reportSchedulingProgressThreshold();

enum class PipelineKind : std::uint8_t {
  Draw,
  Fill,
  Stretch,
  Present,
};

enum class EncoderSplitReason : std::uint8_t {
  Final,
  RenderTargetChange,
  Hazard,
  ClearBarrier,
  SurfaceCopy,
  StretchRect,
  Readback,
  ColorFill,
  Present,
  PresentAcquire,
  TileMidPassIneligible,
  OrderedControl,
};

enum class RenderPassDepthStoreProof : std::uint8_t {
  AllowNextClear,
  AllowDeadNoPresent,
  BlockNullDepth,
  BlockNoLookahead,
  BlockMsaaResolve,
  BlockDrawDepth,
  BlockShadowSample,
  BlockSurfaceCopy,
  BlockStretchRect,
  BlockReadback,
  BlockColorFill,
  BlockDepthResolve,
  BlockPresent,
  BlockClearMismatch,
};

enum class RenderPassColorStoreProof : std::uint8_t {
  AllowNextClear,
  AllowDeadNoPresent,
  BlockNullColor,
  BlockNoLookahead,
  BlockDrawTarget,
  BlockTextureSample,
  BlockSurfaceCopy,
  BlockStretchRect,
  BlockReadback,
  BlockColorFill,
  BlockMsaaResolve,
  BlockPresent,
  BlockDeadNoPresentDisabled,
  BlockClearMismatch,
};

enum class RenderPassNoLookaheadCause : std::uint8_t {
  Empty,
  Invalid,
  SuffixExhausted,
  StorageTruncated,
};

enum class RenderPassLateStoreAspect : std::uint8_t {
  Color,
  Depth,
  Stencil,
};

enum class RenderPassLateStoreResolutionCause : std::uint8_t {
  Clear,
  ClearMismatch,
  Draw,
  Sample,
  Readback,
  Copy,
  Resolve,
  Present,
  IncompatibleClose,
  Drain,
  Finalize,
  Error,
};

// Frame-allocator arena identity for ring_arena_heap_fallback_*
// counters (R-BACK-2.27). Aggregate (`Unknown`) is reported when the
// caller does not have access to the FrameAllocators tag.
enum class RingArenaKind : std::uint8_t {
  Unknown = 0,
  Argbuf,
  LambdaStore,
  Staging,
  CopyTemp,
};

enum class D3D9SnapshotUniformBuildContext : std::uint8_t {
  None = 0,
  BatchMiss,
};

enum class DrawUniformPayloadMaterializeSite : std::uint8_t {
  Other = 0,
  DrawEncoderCommand,
  DrawEncoderParam,
  FramegraphCommand,
  FramegraphParam,
  QueueObservation,
};

enum class ChunkPublishReason : std::uint8_t {
  Unknown = 0,
  DrawLimit,
  PayloadLimit,
  Present,
  PresentAcquire,
  PresentSplitBefore,
  Flush,
  StretchSplit,
  MapWait,
  SemanticBoundary,
  DrawContinuation,
  // A populated final slot could not hold the next lease span's exact
  // reservation, so the span published the existing extent and retried
  // against a fresh empty slot instead of degrading to draw-by-draw replay.
  DirectCapacityRotation,
  // Default-off CPU-ready experiment: one immutable non-Present frame prefix
  // published only after reserving the compatibility Present tail.
  EarlyPrefix,
};

enum class ChunkPublishTailCommandKind : std::uint8_t {
  Empty = 0,
  DrawRun,
  Clear,
  SurfaceCopy,
  StretchRect,
  Readback,
  ColorFill,
  DepthResolve,
  Present,
};

class ScopedD3D9SnapshotUniformBuildContext {
 public:
  explicit ScopedD3D9SnapshotUniformBuildContext(
      D3D9SnapshotUniformBuildContext context) noexcept;
  ~ScopedD3D9SnapshotUniformBuildContext();

  ScopedD3D9SnapshotUniformBuildContext(
      const ScopedD3D9SnapshotUniformBuildContext&) = delete;
  ScopedD3D9SnapshotUniformBuildContext& operator=(
      const ScopedD3D9SnapshotUniformBuildContext&) = delete;

 private:
  D3D9SnapshotUniformBuildContext previous_;
};

// R-BACK-2.10 / 2.27: chunk admit/reject + ring arena heap fallback
// counters. Aggregate values surface bridge backpressure and ring
// exhaustion that R-BACK-2.6/2.13 invariants assume bounded.
void countChunkAdmit();
void countChunkReject();
void countCommandChunkWire(std::uint32_t version, std::uint64_t records,
                           std::uint64_t bytes,
                           std::uint64_t registryResolutions = 0u);
void countCommandChunkReject();
// One gated observation per raw direct-replay classification. The three
// payload axes intentionally remain separate so callers can compare raw,
// record, and wire-byte populations without reconstructing them.
enum class DirectChunkSlotReplayDisposition : std::uint8_t {
  Direct = 0,
  DirectOversized,
  DirectWithPresentTail,
  LegacyStateOnly,
  LegacySegmented,
  LegacyUpDraw,
  LegacyPresent,
  LegacyUnsupported,
  LegacyOversized,
  LegacyCaptureOrTrace,
  InlineOrderedControl,
  RejectInvalid,
  Count,
};
enum class DirectChunkSlotReplayOutcome : std::uint8_t {
  NotAttempted = 0,
  ImportRejected,
  PlanRejected,
  BeginLegacyPreEffectFailure,
  BeginFailStopped,
  ReplayFailed,
  CommitFailed,
  Committed,
  Count,
};
void recordDirectChunkSlotReplayDisposition(
    DirectChunkSlotReplayDisposition disposition,
    DirectChunkSlotReplayOutcome outcome, std::uint64_t records,
    std::uint64_t wireBytes);
// Direct whole-range gate telemetry. Cheap rejection is decided before queue
// admission/materialization; post-materialization fallback means the private
// final-slot transaction was entered and then rolled back before compatibility
// replay took ownership.
void countDirectChunkSlotReplayCheapRejected();
void countDirectChunkSlotReplayPostMaterializationFallback();
void countDirectChunkSlotContinuationAttempted();
void countDirectChunkSlotContinuationAdmitted();
void countDirectChunkSlotContinuationCapacityRejected();
void countDirectChunkSlotContinuationStructuralRejected();
void countDirectChunkSlotContinuationIdentityRejected();
void countDirectChunkSlotContinuationCommitted();
void countDirectChunkSlotContinuationPopulatedFallback();
void countDirectChunkSlotContinuationCapacityRotated();
// Empty-slot storage provisioning (R-BACK-2.104). Count-only and perf-gated.
// `provisionFailed` targets zero and means staged allocation did not publish;
// `provisionSkippedNonEmpty` targets zero: a non-zero row means a slot carried
// no commands but was not empty in every direct dimension, so provisioning
// declined to reallocate and the exact per-transaction reserve stood alone.
// `sourceExceedsBudget` is the *expected* residual: a source whose own exact
// plan is larger than the bounded slot budget, i.e. a genuine budget rotation
// rather than an artefact of exact-fit reservation.
void countDirectChunkSlotSlotProvisioned();
void countDirectChunkSlotSlotProvisionFailed();
void countDirectChunkSlotSlotProvisionSkippedNonEmpty();
void countDirectChunkSlotSlotProvisionReused();
void countDirectChunkSlotSlotProvisionReuseRejected();
void countDirectChunkSlotSlotProvisionSourceExceedsBudget();
void countDirectSlotCapacityLeaseDenied();
void recordDirectSlotCapacityLease(std::uint64_t retainedBytes,
                                   std::uint64_t stagedBytes,
                                   bool generationAdvanced,
                                   bool rolledBack);
// Lease-span routing observability (R-BACK-2.102). Count-only and perf-gated:
// `ordinaryFallbackDraws` and `soaGrowthAfterReserve` both target zero, so a
// non-zero row is the signal that a direct span degraded or that a reserved
// destination grew inside its transaction.
void countReplaySpanLease(std::uint64_t draws, std::uint64_t commands,
                          std::uint64_t islands, std::uint64_t coordinators);
void countReplaySpanOrderedControlCut();
void countReplaySpanCompatibilityCut();
void countReplaySpanOrdinaryFallbackDraws(std::uint64_t draws);
void countReplaySpanSoaGrowthAfterReserve();
void countReplaySpanStateProjections(std::uint64_t projections);
void countReplaySpanSameRawAdmitted();
void countReplaySpanSameRawRejected();
void countReplaySpanPlanRejected();
void countReplaySpanSeparatorFailStop();

// Heavy opt-in source-planner attribution. The planner keeps this observation
// in a file-local TLS aggregate and publishes it once per raw source, so the
// normal path has no per-record atomic writes. The split is enabled only when
// both DXMT_PERF_COUNTERS and DXMT9_PERF_REPLAY_EMISSION_PLAN_SPLIT are set.
// Per-record work-shape counters and clocks require the additional
// DXMT9_PERF_RECORD_CAPACITY_DELTA_WORK and
// DXMT9_PERF_RECORD_CAPACITY_DELTA_TIMING gates; none of these diagnostic
// runs is an FPS promotion sample.
struct ReplayEmissionPlanObservation {
  std::uint64_t planCalls = 0;
  std::uint64_t workPlanCalls = 0;
  std::uint64_t childTimingPlanCalls = 0;
  std::uint64_t records = 0;
  std::uint64_t totalNs = 0;
  std::uint64_t computeCalls = 0;
  std::uint64_t computeNs = 0;
  std::uint64_t applyCalls = 0;
  std::uint64_t applyNs = 0;
  std::uint64_t applyFailures = 0;
  std::uint64_t capacityDimensionCheckVisits = 0;
  std::uint64_t capacityDimensionAddVisits = 0;
  std::uint64_t capacityDimensionNonzeroVisits = 0;
};

void recordReplayEmissionPlanObservation(
    const ReplayEmissionPlanObservation& observation);

// Compatibility-lane draw-run island batching (see
// include/dxmt9/core_compat_draw_batch.hpp). `published` counts queue
// acquisitions: one per island, however many draws it folded.
void countCompatibilityDrawBatchPublished(std::uint64_t draws);
void countCompatibilityDrawBatchDecision(
    core::CompatibilityDrawBatchAdmission admission,
    core::CompatibilityDrawBatchCut cut);
void countRingArenaHeapFallback(RingArenaKind kind, std::uint64_t bytes);

void countSubmitDraw();
void countSubmitClear();
void countSubmitStretch();
void countStretchBlitCopy();
void countStretchRenderPass();
void countStretchFullscreen();
void countSubmitPresent();
void countSubmitFlush();
void countSubmitPresentCpuTime(std::uint64_t nanoseconds);
void countSubmitPresentAcquireCpuTime(std::uint64_t nanoseconds);
void countSubmitPresentCommitCpuTime(std::uint64_t nanoseconds);
void countSubmitPresentBoundaryCpuTime(std::uint64_t nanoseconds);
void countPrepareSlotForPublishCpuTime(std::uint64_t nanoseconds);
void countPrepareSlotResourceMarkCpuTime(std::uint64_t nanoseconds);
void countUnpublishedSlotPsoPrefetchCpuTime(std::uint64_t nanoseconds);
void countChunkPublishReason(ChunkPublishReason reason,
                             std::uint64_t commandCount);
// Tape-gated CPU-ready session join lane (DXMT9_CPU_READY_TAPE). Release
// reasons are the ordered producer/queue-progress fences only — this lane
// deliberately has no completion-wait or producer-quiescence release.
enum class CpuReadySessionReleaseReason : std::uint8_t {
  ProducerWait,
  NonAppendable,
  InitializerWait,
  Drain,
  FailPath,
};
enum class CpuReadySessionCapDimension : std::uint8_t {
  Sources,
  Pages,
  Bytes,
  Draws,
  CommandBuffers,
};
enum class CpuReadySessionCapRequirementAxes : std::uint8_t {
  SourcesOnly,
  PagesOnly,
  SourcesAndPages,
};
enum class CpuReadySessionDisposition : std::uint8_t {
  Isolated,
  LegacyRollback,
  Invalid,
};
enum class CpuReadySessionIsolationReason : std::uint8_t {
  Present,
  CapacityBytes,
  Other,
};
enum class CpuReadyRetainedHeadFallbackReason : std::uint8_t {
  Release,
  ProducerWait,
  Initializer,
  Stop,
  WriterGone,
  Pressure,
};
enum class CpuReadyRetainedHeadFrontier : std::uint8_t {
  Fresh,
  ActiveRender,
};
enum class CpuReadyTerminalSuffixFrontier : std::uint8_t {
  Fresh,
  ActiveRender,
};
enum class CpuReadyRetainedHeadRejectReason : std::uint8_t {
  BorrowShape,
  PayloadInvalid,
  TerminalSuffixOwned,
  Present,
  SourceCompatibility,
  Admission,
  CapacitySnapshot,
  WritingSuccessorMissing,
  WritingSuccessorInvalid,
  ReservationRace,
};
void countCpuReadySessionPendingStarted();
enum class CpuReadyEarlyPrefixFallback : std::uint8_t {
  AlreadyPublished = 0,
  NoTailCredit,
  OrderedControl,
  Ineligible,
  Capacity,
};
void countCpuReadyEarlyPrefixCandidate();
void countCpuReadyEarlyPrefixPublished();
void countCpuReadyEarlyPrefixFallback(CpuReadyEarlyPrefixFallback reason);
void countCpuReadyEarlyPrefixTailReserved();
void countCpuReadyEarlyPrefixSessionParked();
void countCpuReadyEarlyPrefixSessionJoined();
void countCpuReadyEarlyPrefixSessionJoinFailedPreEffect();
void countEncodePartitionPlan(bool explicitPlan,
                              std::uint64_t rangeCount,
                              std::uint64_t drawRangeCount,
                              std::uint64_t drawCount,
                              std::uint64_t subdividedDrawRuns,
                              std::uint64_t mergePreservedIdentity,
                              encoders::ProductionPartitionFallbackReason
                                  fallbackReason,
                              std::uint64_t plannerNanoseconds);
void countRenderPartitionProvider(render::PartitionModeRequest requestedMode,
                                  render::PartitionExecutionMode resolvedMode);
void countParallelPassDecision(
    const encoders::ParallelPassExecutionDecision& decision);
void countParallelPassWorkerBatch(std::uint32_t tasks);
void countParallelPassWorkerTaskBegin();
void countParallelPassWorkerTaskEnd(std::uint64_t cpuNs);
void countParallelPassWorkerWallTime(std::uint64_t wallNs);

// Heavy opt-in per-child phase split, gated by
// `encoders::parallelChildSplitPerfEnabled()`
// (`DXMT9_PERF_PARALLEL_CHILD_SPLIT`). Callers must not read the clock or
// call these unless that gate is enabled; see
// `agents/rules/environment_variables_perf.rules.md`.
void countParallelChildSetup(std::uint64_t cpuNs);
void countParallelChildBody(std::uint64_t cpuNs);
void countParallelChildEnd(std::uint64_t cpuNs);

// Cold, typed evidence seam for the Render Tape parallel-join worker.  This
// deliberately reads the existing raw counters instead of extending the
// canonical shutdown-report schema; production provider tools can therefore
// emit a bounded JSON sidecar without scraping stderr.
struct RenderTapeParallelJoinSnapshot {
  std::uint64_t selected = 0;
  std::uint64_t children = 0;
  std::uint64_t draws = 0;
  std::uint64_t workerBatches = 0;
  std::uint64_t workerTasks = 0;
  std::uint64_t workerCpuNs = 0;
  std::uint64_t workerWallNs = 0;
  std::uint64_t workerActivePeak = 0;
  std::uint64_t preEffectFallbacks = 0;
  std::uint64_t gpuCommandBufferErrors = 0;
};

RenderTapeParallelJoinSnapshot snapshotRenderTapeParallelJoin() noexcept;

void countParallelPassBindingReject(
    encoders::ParallelPassBindingRejectReason reason);
void countParallelPassBindingSelected(
    encoders::ParallelPassDirectBindingMode mode,
    std::uint32_t childCount,
    std::uint64_t drawCount);
void countParallelPassEconomics(
    const encoders::ParallelPassEconomicsSummary& summary,
    const encoders::ParallelPassEconomicsDecision& decision);
void countParallelPassShadow(
    const encoders::SealedParallelPassSnapshotBatchResult& result);
void countParallelPassAdapter(
    const encoders::ParallelPassAdapterDecision& decision);

// Typed breakdown of `parallel_pass_adapter_certificate_invalid`, keyed by
// the `ParallelPassSemanticPlanFailure` checkpoint that rejected the
// candidate. The fields sum to the aggregate counter
// (`dxmt9-parallel-render-pass-spec` pins the conservation).
struct ParallelPassAdapterCertificateInvalidSnapshot {
  std::uint64_t missingSnapshot = 0;
  std::uint64_t sourceIdentity = 0;
  std::uint64_t passIdentity = 0;
  std::uint64_t coordinatorProof = 0;
  std::uint64_t attachmentProof = 0;
  std::uint64_t resourceProof = 0;
  std::uint64_t firstDrawProof = 0;
  std::uint64_t childCapacity = 0;
  std::uint64_t childPlan = 0;
  std::uint64_t coverage = 0;
  std::uint64_t arithmetic = 0;

  constexpr std::uint64_t total() const noexcept {
    return missingSnapshot + sourceIdentity + passIdentity +
        coordinatorProof + attachmentProof + resourceProof +
        firstDrawProof + childCapacity + childPlan + coverage + arithmetic;
  }
};

ParallelPassAdapterCertificateInvalidSnapshot
snapshotParallelPassAdapterCertificateInvalid() noexcept;

// Typed breakdown of adapter-considered candidates whose certificate was
// valid but the proof-core selector still rejected them, keyed by
// `ParallelPassCandidateSelectionFailure`. The fields sum to
// `parallelPassAdapterCertificateValid - parallelPassAdapterSelected`
// (`dxmt9-parallel-render-pass-spec` pins the conservation).
struct ParallelPassAdapterSelectionSnapshot {
  std::uint64_t empty = 0;
  std::uint64_t invalidPlan = 0;
  std::uint64_t invalidEconomics = 0;
  std::uint64_t nonPositiveBenefit = 0;
  std::uint64_t arithmetic = 0;
  std::uint64_t invalidCandidateOrdinal = 0;

  constexpr std::uint64_t total() const noexcept {
    return empty + invalidPlan + invalidEconomics + nonPositiveBenefit +
        arithmetic + invalidCandidateOrdinal;
  }
};

ParallelPassAdapterSelectionSnapshot
snapshotParallelPassAdapterSelection() noexcept;
void countCpuReadySessionHeadAppended(bool arenaSource);
void countCpuReadySessionTailSubmitted();
void countCompletionSpanShadowBuilt(std::uint64_t sourceCount);
void countCompletionSpanShadowValidated();
void countCompletionSpanShadowMismatch();
void countPostEncodeRetireAttempt();
void countPostEncodeRetireSuccess(bool arena);
void countPostEncodeRetireIneligible(std::uint32_t reason);
void countPostEncodeReceiptFailure(std::uint32_t result);
void recordPostEncodeReceiptDepth(std::uint64_t depth,
                                  std::uint64_t peak);
void countPostEncodeResidencyCreditReleased(std::uint64_t pages,
                                            std::uint64_t bytes);
void countPostEncodeWorkCapClose();
void recordGpuOutstandingCompletionSources(std::uint64_t count);
void countCpuReadySessionReleased(CpuReadySessionReleaseReason reason);
void countCpuReadySessionLeaseAcquired(
    std::uint64_t reservedSources,
    std::uint64_t reservedPages,
    std::uint64_t reservedBytes,
    std::uint64_t reservedDraws,
    std::uint64_t successorHeadroomPages);
void countCpuReadySessionLeaseDenied();
void recordCpuReadySessionLeaseUsed(std::uint64_t usedSources,
                                    std::uint64_t usedPages,
                                    std::uint64_t usedBytes,
                                    std::uint64_t usedDraws,
                                    std::uint64_t slackSources,
                                    std::uint64_t slackPages,
                                    std::uint64_t slackBytes,
                                    std::uint64_t slackDraws);
void countCpuReadySessionLeaseReleased();
void countCpuReadySessionCapRelease(CpuReadySessionCapDimension dimension);
void recordCpuReadySessionCapRequirement(
    CpuReadySessionCapRequirementAxes axes,
    std::uint64_t predecessorSources,
    std::uint64_t predecessorPages,
    std::uint64_t candidatePayloadPages,
    std::uint64_t candidateWrapPaddingPages,
    std::uint64_t candidateRequiredPages,
    std::uint64_t requiredTotalSources,
    std::uint64_t requiredTotalPages);
void countCpuReadySessionDisposition(CpuReadySessionDisposition disposition);
void countCpuReadySessionIsolation(
    CpuReadySessionIsolationReason reason);
void countCpuReadyRetainedHeadAttempt(CpuReadyRetainedHeadFrontier frontier);
void countCpuReadyRetainedHeadHeld(CpuReadyRetainedHeadFrontier frontier);
void countCpuReadyRetainedHeadSuccessorReady(
    CpuReadyRetainedHeadFrontier frontier);
void countCpuReadyRetainedHeadFallback(
    CpuReadyRetainedHeadFallbackReason reason);
void countCpuReadyRetainedHeadRestoreFailure();
void recordCpuReadyRetainedHeadWait(std::uint64_t nanoseconds);
void countCpuReadyRetainedHeadRejected(
    CpuReadyRetainedHeadRejectReason reason);
void countCpuReadyNextSourceIntentArmed();
void countCpuReadyNextSourceIntentCanceled();
void countCpuReadyRetainedHeadIntentSelected();
void countCpuReadyRetainedHeadIntentSuccessorReady();
void countCpuReadyTerminalSuffixPrefix(
    CpuReadyTerminalSuffixFrontier frontier);
void countCpuReadyTerminalSuffixJoined(
    CpuReadyTerminalSuffixFrontier frontier);
void countCpuReadyTerminalSuffixNaturalDrain(
    CpuReadyTerminalSuffixFrontier frontier);
enum class CpuReadyMultiSourceFallbackReason : std::uint8_t {
  Eligibility,
  NaturalPlan,
  InvalidPlan,
  RepeatedSource,
  ResolvedSource,
  CompletionSource,
  Admission,
  FragmentRange,
  Carrier,
};
enum class CpuReadyMultiSourceFatalReason : std::uint8_t {
  EncodeReturnedNull,
  CarrierFold,
};
enum class CpuReadyMultiSourceEligibilityReason : std::uint8_t {
  ActiveRenderIncomplete,
  PresentBoundary,
  NonConsecutiveIdentity,
  OtherBoundary,
};
enum class CpuReadyMultiSourcePlannerOutcome : std::uint8_t {
  InvalidInput,
  SeedRejected,
  NoActiveTargetMatch,
  NoMerge,
  NaturalAfterMerge,
  PermutationRejected,
  MovedHeadUnproved,
  Planned,
};
enum class CpuReadyMultiSourcePlannerMerge : std::uint8_t {
  None,
  Seed,
  NonSeedOnly,
};
enum class CpuReadyMultiSourceSourceLocalFallback : std::uint8_t {
  NaturalAfterMerge,
  PermutationRejected,
};
struct CpuReadyMultiSourceSeedMergeAttribution {
  std::uint32_t optimizerMergeCount = 0;
  std::uint32_t seedMergeCount = 0;
  std::uint64_t seedMergeDistanceTotal = 0;
  std::uint32_t seedMergeDistanceMax = 0;
  std::uint64_t commandBefore = 0;
  std::uint64_t commandAfter = 0;
  std::uint64_t emptyIntervening = 0;
  bool missing = false;
};
void countCpuReadyMultiSourceWindowAttempted();
void countCpuReadyMultiSourceWindowPlanned(std::uint64_t sources,
                                           std::uint64_t commands,
                                           std::uint64_t runs);
void countCpuReadyMultiSourceWindowFallback(
    CpuReadyMultiSourceFallbackReason reason);
void countCpuReadyMultiSourceSourceLocalFallbackStarted(
    CpuReadyMultiSourceSourceLocalFallback disposition,
    std::uint64_t sources);
void countCpuReadyMultiSourceSourceLocalFallbackCompleted(
    CpuReadyMultiSourceSourceLocalFallback disposition);
void countCpuReadyMultiSourceEligibilityFallback(
    CpuReadyMultiSourceEligibilityReason reason);
void recordCpuReadyMultiSourcePlannerOutcome(
    CpuReadyMultiSourcePlannerOutcome outcome,
    CpuReadyMultiSourcePlannerMerge merge,
    std::uint32_t firstMatchingPassDistance,
    CpuReadyMultiSourceSeedMergeAttribution seedAttribution,
    bool seedSecondNonDraw,
    bool seedBlockedCycle);
void countCpuReadyMultiSourcePostEffectFatal(
    CpuReadyMultiSourceFatalReason reason);
void recordCpuReadyMultiSourceCompletionRegistration(
    std::uint64_t sources, bool fifoValid);
void countRenderPassNaturalFallbackBegin();
void countRenderPassNaturalFallbackReentryDistance(
    std::uint32_t interveningPasses, bool sameWindow);
void countActiveSeedMergeTicketIssued(std::uint64_t count = 1);
void countActiveSeedMergeTicketMatched(std::uint64_t count = 1);
void countActiveSeedMergeTicketContinued(std::uint64_t count = 1);
void countActiveSeedMergeTicketMismatch(std::uint64_t count = 1);
void countActiveSeedMergeTicketUnconsumed(std::uint64_t count = 1);
void countActiveSeedMergeWitnessOverflow(std::uint64_t count = 1);
void countActiveSeedMergeWitnessMismatch(std::uint64_t count = 1);
void countActiveSeedInstanceUnavailable(std::uint64_t count = 1);
void countActiveSeedInstanceStale(std::uint64_t count = 1);
void countRenderPassActiveSeedBridgeReentryDistance(
    std::uint32_t interveningPasses);
void countRenderPassFinalCloseCause(
    encoders::SessionFinalizeCause cause);
void countRenderPassCloseLedgerAdjacentCause(
    encoders::SessionFinalizeCause cause);
void countRenderPassNaturalShortCrossPriorClose(EncoderSplitReason reason);
void countRenderPassNaturalShortCrossPriorCloseMissing(
    std::uint64_t count = 1);
void countRenderPassCloseLedgerRecorded(std::uint64_t count = 1);
void countRenderPassCloseLedgerMissing(std::uint64_t count = 1);
void countRenderPassCloseLedgerTerminalAdjacent(std::uint64_t count = 1);
void countRenderPassCloseLedgerTerminalNonAdjacent(std::uint64_t count = 1);
void countRenderPassCloseLedgerTerminalNotReopenedBeforePresent(
    std::uint64_t count = 1);
void countRenderPassFinalCloseLedgerRecorded(std::uint64_t count = 1);
void countRenderPassFinalCloseLedgerMissing(std::uint64_t count = 1);
void countRenderPassFinalCloseLedgerTerminalAdjacent(
    std::uint64_t count = 1);
void countRenderPassFinalCloseLedgerTerminalNonAdjacent(
    std::uint64_t count = 1);
void countRenderPassFinalCloseLedgerTerminalNotReopenedBeforePresent(
    std::uint64_t count = 1);
void countChunkPublishPresentPrePresentOpportunityTail(
    ChunkPublishTailCommandKind kind, bool drawOnly);
void countChunkPublishSlotResidency(ChunkPublishReason reason,
                                    std::uint64_t nanoseconds);
void countChunkPublishPresentPrePresentOpportunity(std::uint64_t commandCount,
                                                   std::uint64_t drawRunCount,
                                                   std::uint64_t drawItemCount,
                                                   std::uint64_t nonDrawCommandCount,
                                                   std::uint64_t payloadBytes,
                                                   std::uint64_t residencyNanoseconds,
                                                   bool presentIsTail);
void countEncodeSlotPsoPrefetchCpuTime(std::uint64_t nanoseconds);
void countEncodeSlotPsoPrefetchCommands(std::uint64_t count);
void countEncodeSlotPsoPrefetchCandidates(std::uint64_t count = 1);
void countEncodeSlotPsoPrefetchTileCandidates(std::uint64_t count = 1);
void countEncodeSlotPsoPrefetchArgbufStage2Candidates(std::uint64_t count = 1);
void countEncodeSlotPsoPrefetchArgbufResourceArrayCandidates(std::uint64_t count = 1);
void countEncodeSlotPsoPrefetchStateCopyCpuTime(std::uint64_t nanoseconds);
void countEncodeSlotPsoPrefetchDepthLookupCpuTime(std::uint64_t nanoseconds);
void countEncodeSlotPsoPrefetchTileSelectCpuTime(std::uint64_t nanoseconds);
void countEncodeSlotPsoPrefetchTileBaseLookupCpuTime(std::uint64_t nanoseconds);
void countEncodeSlotPsoPrefetchTileDrawLookupCpuTime(std::uint64_t nanoseconds);
void countEncodeSlotPsoPrefetchArgbufSelectCpuTime(std::uint64_t nanoseconds);
void countEncodeSlotPsoPrefetchDrawKeyResolveCpuTime(std::uint64_t nanoseconds);
void countEncodeSlotPsoPrefetchDrawResolveFormatCpuTime(std::uint64_t nanoseconds);
void countEncodeSlotPsoPrefetchDrawResolveVariantKeyCpuTime(std::uint64_t nanoseconds);
void countEncodeSlotPsoPrefetchDrawResolveShaderContextCpuTime(std::uint64_t nanoseconds);
void countEncodeSlotPsoPrefetchDrawResolveX8AlphaCpuTime(std::uint64_t nanoseconds);
void countEncodeSlotPsoPrefetchDrawResolveVsoutLayoutCpuTime(std::uint64_t nanoseconds);
void countEncodeSlotPsoPrefetchDrawResolveFragmentlessCpuTime(std::uint64_t nanoseconds);
void countEncodeSlotPsoPrefetchDrawLookupCpuTime(std::uint64_t nanoseconds);
void countEncodeSlotPsoPrefetchDrawSemanticKeyCpuTime(std::uint64_t nanoseconds);
void countEncodeSlotPsoPrefetchDrawSemanticProbeCpuTime(std::uint64_t nanoseconds);
void countEncodeSlotPsoPrefetchDrawSemanticStoreCpuTime(std::uint64_t nanoseconds);
void countEncodeSlotPsoPrefetchDrawSemanticMemoHits(std::uint64_t count = 1);
void countEncodeSlotPsoPrefetchDrawSemanticMemoMisses(std::uint64_t count = 1);
void countEncodeSlotPsoPrefetchDrawSemanticMemoOverflow(std::uint64_t count = 1);
void countEncodeSlotPsoPrefetchDrawSemanticMissProbeKeyHits(std::uint64_t count = 1);
void countEncodeSlotPsoPrefetchDrawSemanticMissProbeKeySameSemantic(std::uint64_t count = 1);
void countEncodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffArgbufSelector(std::uint64_t count = 1);
void countEncodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffVertexDecl(std::uint64_t count = 1);
void countEncodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffShader(std::uint64_t count = 1);
void countEncodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffRenderState(std::uint64_t count = 1);
void countEncodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffTextureHandles(std::uint64_t count = 1);
void countEncodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffTextureLod(std::uint64_t count = 1);
void countEncodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffTextureStage(std::uint64_t count = 1);
void countEncodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffSampler(std::uint64_t count = 1);
void countEncodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffAttachment(std::uint64_t count = 1);
void countEncodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffClipPlane(std::uint64_t count = 1);
void countEncodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffConstantUsage(std::uint64_t count = 1);
void countEncodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffSingleField(std::uint64_t count = 1);
void countEncodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffMultiField(std::uint64_t count = 1);
void countEncodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffTextureHandlesOnly(std::uint64_t count = 1);
void countEncodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffTextureHandlesWithOthers(std::uint64_t count = 1);
void countEncodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffHashOnly(std::uint64_t count = 1);
void countEncodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffUnknown(std::uint64_t count = 1);
void countEncodeSlotPsoPrefetchDrawResourceShapeMemoCandidates(std::uint64_t count = 1);
void countEncodeSlotPsoPrefetchDrawResourceShapeMemoHits(std::uint64_t count = 1);
void countEncodeSlotPsoPrefetchDrawResourceShapeMemoMisses(std::uint64_t count = 1);
void countEncodeSlotPsoPrefetchDrawResourceShapeMemoOverflow(std::uint64_t count = 1);
void countEncodeSlotPsoPrefetchDrawResourceShapeMemoStores(std::uint64_t count = 1);
void countEncodeSlotPsoPrefetchDrawResourceShapeMemoValidatedHits(std::uint64_t count = 1);
void countEncodeSlotPsoPrefetchDrawResourceShapeMemoValidatedMisses(std::uint64_t count = 1);
void countEncodeSlotPsoPrefetchDrawResourceShapeMemoMismatchTextureMask(std::uint64_t count = 1);
void countEncodeSlotPsoPrefetchDrawResourceShapeMemoMismatchTextureTypes(std::uint64_t count = 1);
void countEncodeSlotPsoPrefetchDrawResourceShapeMemoMismatchX8Alpha(std::uint64_t count = 1);
void countEncodeSlotPsoPrefetchDrawResourceShapeMemoMismatchAttachment(std::uint64_t count = 1);
void countEncodeSlotPsoPrefetchDrawResourceShapeMemoMismatchSamplerLodBias(std::uint64_t count = 1);
void countEncodeSlotPsoPrefetchDrawResourceShapeMemoMismatchVsOut(std::uint64_t count = 1);
void countEncodeSlotPsoPrefetchDrawResourceShapeMemoMismatchOther(std::uint64_t count = 1);
void countEncodeSlotPsoPrefetchDrawProbeKeyMemoHits(std::uint64_t count = 1);
void countEncodeSlotPsoPrefetchDrawProbeKeyMemoMisses(std::uint64_t count = 1);
void countEncodeSlotPsoPrefetchDrawProbeKeyMemoOverflow(std::uint64_t count = 1);
void countEncodeSlotPsoPrefetchDrawHandleAdjacentCandidates(std::uint64_t count = 1);
void countEncodeSlotPsoPrefetchDrawHandleAdjacentHits(std::uint64_t count = 1);
void countEncodeSlotPsoPrefetchDrawHandleSlotRepeatHits(std::uint64_t count = 1);
void countEncodeSlotPsoPrefetchDrawHandleSlotUnique(std::uint64_t count = 1);
void countEncodeSlotPsoPrefetchDrawHandleSlotOverflow(std::uint64_t count = 1);
void countCommandBuffer();
// R-BACK-2.29..2.32: a single chunk may commit through a chain of
// MTLCommandBuffer instances. countSubCommandBufferCommit() fires once per
// mid-chunk commit (the chain's tail commit is already accounted for via
// countCommandBuffer / commit-cpu-time), and the chunk's local sub-CB
// count is folded into chunkSubCBCountMax at the chunk's exit so the
// table surfaces both total mid-chunk commits and the worst-case per-chunk
// chain length.
void countSubCommandBufferCommit();
void countSubCommandBufferSplitSuppressedByCap();
// R-BACK-2.29..2.32 — fold a chunk's local sub-CB chain length into the
// chunkSubCBCountMax atomic via updateMax. Encode thread calls this once
// at encodeChunk exit; not a counting-style helper, so it is excluded from
// the audit_perf_counter_callsites.py count*() rule by name.
void recordChunkSubCBCount(std::uint64_t perChunkCount);
void countGpuCommandBufferError();
void countMetalBuffer(std::size_t bytes);
// R-BACK-39.2 (Task B11, L1 subset) — frame-graph observe-path counters,
// driven from FrameGraphBackend's observe path
// (render/framegraph_backend.cpp). built/coalesced/dead/memoryless read the
// built FrameGraph + OptimizerStats that runOptimizer produces per observed
// chunk; the dump counter bumps once per observe export. DEFERRED to L2/
// on-device (no L1 callsite, would fail audit_perf_counter_callsites.py):
// framegraph_icb_* and
// framegraph_virtual_attachment_misclassification_stale_persistent.
void countFramegraphPassesBuilt(std::uint64_t passes);
void countFramegraphPassesCoalesced(std::uint64_t passes);
void countFramegraphPassesDead(std::uint64_t passes);
void countFramegraphResourcesMemoryless(std::uint64_t resources);
void countFramegraphDagDumpWritten();
void countFramegraphDceDropped(std::uint64_t passes);
void countFramegraphDcePreservedUnprovable(std::uint64_t passes);
void countFramegraphDceCrossChunkProofResources(std::uint64_t resources);
void countFramegraphDceReplayCommandsOmitted(std::uint64_t commands);
void countFramegraphDceLookaheadPrefix(std::uint64_t commands);
void countFramegraphDceLookaheadSelected();
void countFramegraphDceLookaheadFailOpen();
enum class FramegraphSourceProvenance : std::uint8_t {
  Unknown,
  Legacy,
  Arena,
};
struct FramegraphSourceLocalPassCoalesceDiagnostic {
  std::uint64_t candidates = 0;
  std::uint64_t merged = 0;
  std::uint64_t blockedCycle = 0;
  std::uint64_t secondNonDraw = 0;
  std::uint64_t nonRenderIntervener = 0;
  std::uint64_t missingInvariant = 0;
  std::uint64_t dependencyKept = 0;
  std::uint64_t moveBefore = 0;
  std::uint64_t moveAfter = 0;
  std::uint64_t nonDrawIntervener = 0;
  std::uint64_t semanticIntervener = 0;
  std::uint64_t commandlessIntervener = 0;
  std::uint64_t commandlessReturn = 0;
};
void recordFramegraphSourceLocalPassCoalesce(
    FramegraphSourceProvenance provenance, bool sourceIdentityKnown,
    FramegraphSourceLocalPassCoalesceDiagnostic diagnostic);
enum class FramegraphSourceLocalReplayOutcome : std::uint8_t {
  FinalInvalid,
  // The final source-local command order is natural. A DCE-only drop may still
  // activate a replay plan without changing that order.
  FinalNaturalOrder,
  // A reordered plan was installed in EncodeChunkOptions and passed to encode.
  FinalReorderedActivated,
};
void recordFramegraphSourceLocalReplayOutcome(
    FramegraphSourceLocalReplayOutcome outcome,
    std::uint64_t candidates, std::uint64_t merged);
enum class FramegraphSourceLocalFrontierRollbackReason : std::uint8_t {
  InvalidPlan,
  LiveSetMismatch,
  DuplicateCommand,
  MovedHeadUnproved,
};
// A single typed recorder updates both the broad FrontierRollback outcome and
// its mutually exclusive reason, preserving all three conservation axes by
// construction.
void recordFramegraphSourceLocalFrontierRollback(
    FramegraphSourceLocalFrontierRollbackReason reason,
    std::uint64_t candidates, std::uint64_t merged);
// Production active-render planning diagnostics. One typed call keeps the
// encode hot path free of strings and maps each conservative decision to a
// stable report key.
enum class FramegraphActiveRenderSeedOutcome : std::uint8_t {
  SnapshotAbsent,
  SnapshotIncomplete,
  ApplyApplied,
  ApplyInvalid,
  ApplyIncomplete,
  ApplyOverflow,
  AppliedButUnmerged,
  PassCoalesceBlockedCycle,
  PassCoalesceSecondNonDraw,
  MovedHeadProved,
  FallbackMovedHeadUnproved,
  FallbackInvalidPlan,
  FallbackLiveSetMismatch,
  FallbackDuplicateCommand,
  ReplayActivated,
};
void countFramegraphActiveRenderSeedOutcome(
    FramegraphActiveRenderSeedOutcome outcome, std::uint64_t count = 1);
void countPipelineBuild();
void countPipelineCacheHit(PipelineKind kind);
void countPipelineCacheMiss(PipelineKind kind);
void countPipelineBuild(PipelineKind kind);
void recordDrawPsoSlotCount(std::uint64_t count);
void countDrawPsoSlotExhausted();
void countDrawPsoVariantArgbufStage2();
void countDrawPsoVariantTileFfp();
void recordSourceLibraryEntryCount(std::uint64_t count);
enum class PsoCacheLookupDisposition : std::uint8_t {
  Hit,
  Miss,
  Stale,
};
enum class PsoCacheKeyAxis : std::uint8_t {
  SourceTuple,
  BackendIdentity,
  VertexSource,
  FragmentSource,
  TileSource,
  VsoutShape,
  TextureMask,
  TextureTypes,
  SampledDepthShape,
  Fetch4Shape,
  X8Shape,
  SampleCount,
  ColorFormatShape,
  BlendShape,
  DepthStencilShape,
  ModeBits,
};
enum class PsoBackendIdentityAxis : std::uint8_t {
  VertexShader,
  PixelShader,
  ClipPlaneMask,
  VertexLayout,
  VertexElementLayout,
  Stream0Offset,
  ExtraStreamOffsets,
  Stream0Stride,
  ExtraStreamStrides,
  Fvf,
  DepthFormat,
  StencilFormat,
};
// These APIs are observation-only and are intended for the pipeline-cache
// implementation and the standalone key-cardinality observer. Every call is
// a cached gate plus relaxed atomics when diagnostics are enabled; the normal
// path does not allocate or construct a diagnostic payload.
bool psoCacheDiagnosticsEnabled();
void recordPsoCacheProbeLookup(PsoCacheLookupDisposition disposition) noexcept;
void recordPsoCacheFinalLookup(PsoCacheLookupDisposition disposition,
                               bool hitAfterSource) noexcept;
void recordPsoCacheFinalInsertion() noexcept;
void recordPsoCacheSourceGeneration(bool success,
                                     std::uint64_t sourceBytes) noexcept;
void recordPsoCacheSourceLibraryLookup(bool hit, bool insertion) noexcept;
void recordPsoCacheSlotPublication(std::uint64_t publishNanoseconds,
                                   bool segmentAllocated,
                                   std::uint64_t segmentBytes) noexcept;
void recordPsoCacheDiagnosticTrackerOverflow() noexcept;
void recordPsoCacheDistinctKeyAxis(PsoCacheKeyAxis axis) noexcept;
void recordPsoBackendDistinctIdentityAxis(
    PsoBackendIdentityAxis axis) noexcept;
void recordPsoCacheFinalFanout(std::uint64_t fanout) noexcept;
void countPipelineBuildFailDraw();
void countPipelineBuildFailLibrary();
void countPipelineBuildFailFunction();
void countPipelineBuildFailPso();
void countDrawSkippedNoPipeline();
void countShaderVariantKeyHashCpuTime(std::uint64_t nanoseconds);
void countRenderPassBegin();
void countRenderPassEnd(EncoderSplitReason reason);
void countHazardProbe(bool bloomOverlap, bool exactOverlap);
void countSubmitDrawRunBatchInputRuns(std::uint64_t runs);
void countSubmitDrawRunBatchEmittedRun();
void countSubmitDrawRunBatchSegment();
void countSubmitDrawRunBindingSnapshotCpuTime(std::uint64_t nanoseconds);
void countSubmitDrawRunPayloadBytesCpuTime(std::uint64_t nanoseconds);
void countSubmitDrawRunSlotPrepareCpuTime(std::uint64_t nanoseconds);
void countSubmitDrawRunResourceMarkCpuTime(std::uint64_t nanoseconds);
void countSubmitDrawRunAppendCpuTime(std::uint64_t nanoseconds);
void countSubmitDrawRunChunkCommitCpuTime(std::uint64_t nanoseconds);
void countD3D9DrawStateCacheLookup(bool hit, bool includeIndexBuffer);
void countD3D9DrawStateCacheDirectLookup(bool hit, bool includeIndexBuffer);
void countD3D9DrawStateCacheBatchLookup(bool hit);
void countD3D9DrawStateCacheUniformRefresh();
void countD3D9DrawStateCacheMissReason(std::uint32_t reasonMask);
void countD3D9DrawStateCacheBatchMissReason(std::uint32_t reasonMask);
void countD3D9SnapshotCacheHitCpuTime(std::uint64_t nanoseconds);
void countD3D9SnapshotCacheMissCpuTime(std::uint64_t nanoseconds);
void countD3D9SnapshotCacheDirectHitCpuTime(std::uint64_t nanoseconds);
void countD3D9SnapshotCacheDirectMissCpuTime(std::uint64_t nanoseconds);
void countD3D9SnapshotCacheBatchHitCpuTime(std::uint64_t nanoseconds);
void countD3D9SnapshotCacheBatchMissCpuTime(std::uint64_t nanoseconds);
void countD3D9SnapshotCacheBindingLayoutCpuTime(std::uint64_t nanoseconds);
void countD3D9SnapshotCacheUniformRefreshCpuTime(std::uint64_t nanoseconds);
void countD3D9SnapshotCacheUniformBuildCpuTime(std::uint64_t nanoseconds);
void countD3D9SnapshotCacheUniformHashCpuTime(std::uint64_t nanoseconds);
void countD3D9SnapshotCacheMissShaderLayoutCpuTime(std::uint64_t nanoseconds);
void countD3D9SnapshotCacheMissUniformBuildCpuTime(std::uint64_t nanoseconds);
void countD3D9SnapshotCacheMissHotBuildCpuTime(std::uint64_t nanoseconds);
void countD3D9SnapshotCacheDirectMissShaderLayoutCpuTime(std::uint64_t nanoseconds);
void countD3D9SnapshotCacheDirectMissUniformBuildCpuTime(std::uint64_t nanoseconds);
void countD3D9SnapshotCacheDirectMissHotBuildCpuTime(std::uint64_t nanoseconds);
void countD3D9SnapshotCacheBatchMissShaderLayoutCpuTime(std::uint64_t nanoseconds);
void countD3D9SnapshotCacheBatchMissShaderLayoutCompatible(bool compatible);
void countD3D9SnapshotCacheBatchMissShaderLayoutReuse(bool reused);
void countD3D9SnapshotCacheBatchMissUniformBuildCpuTime(std::uint64_t nanoseconds);
void countD3D9SnapshotCacheBatchMissHotBuildCpuTime(std::uint64_t nanoseconds);
void countD3D9SnapshotCacheBatchMissHotBuildZeroInitCpuTime(std::uint64_t nanoseconds);
void countD3D9SnapshotCacheBatchMissHotBuildKeyCpuTime(std::uint64_t nanoseconds);
void countD3D9SnapshotCacheBatchMissHotBuildKeyZeroInitCpuTime(std::uint64_t nanoseconds);
void countD3D9SnapshotCacheBatchMissHotBuildKeyStreamCpuTime(std::uint64_t nanoseconds);
void countD3D9SnapshotCacheBatchMissHotBuildKeyShaderCpuTime(std::uint64_t nanoseconds);
void countD3D9SnapshotCacheBatchMissHotBuildKeyConstantCpuTime(std::uint64_t nanoseconds);
void countD3D9SnapshotCacheBatchMissHotBuildKeyTextureCpuTime(std::uint64_t nanoseconds);
void countD3D9SnapshotCacheBatchMissHotBuildKeySamplerCpuTime(std::uint64_t nanoseconds);
void countD3D9SnapshotCacheBatchMissHotBuildKeyRenderStateCpuTime(std::uint64_t nanoseconds);
void countD3D9SnapshotCacheBatchMissHotBuildKeyAttachmentCpuTime(std::uint64_t nanoseconds);
void countD3D9SnapshotCacheBatchMissHotBuildKeyUniformCpuTime(std::uint64_t nanoseconds);
void countD3D9SnapshotCacheBatchMissHotBuildBindingCopyCpuTime(std::uint64_t nanoseconds);
void countD3D9SnapshotCacheBatchMissHotBuildRenderStateCpuTime(std::uint64_t nanoseconds);
void countD3D9SnapshotCacheBatchMissHotBuildTextureStageStateCpuTime(std::uint64_t nanoseconds);
void countD3D9SnapshotCacheBatchMissHotBuildSamplerStateCpuTime(std::uint64_t nanoseconds);
void countD3D9SnapshotCacheBatchMissHotBuildTailCopyCpuTime(std::uint64_t nanoseconds);
void countD3D9SnapshotCacheBatchMissHotBuildFlatRenderReuse(bool reused);
void countD3D9SnapshotCacheBatchMissHotBuildFlatTssReuse(bool reused);
void countD3D9SnapshotCacheBatchMissHotBuildFlatSamplerReuse(bool reused);
void countD3D9SnapshotCacheBatchMissUniformNonConstHashReuse(bool reused);
void countD3D9SnapshotCacheBatchMissUniformPayloadPath(
    bool reusedFullPayload,
    bool reusedNonConstantPayload);
void countD3D9SnapshotCacheBatchMissUniformVsConstHashPath(bool reused);
void countD3D9SnapshotCacheBatchMissUniformPsConstHashPath(bool reused);
void countD3D9SnapshotCacheBatchMissUniformVsConstHashMemoProbe(bool hit);
void countD3D9SnapshotCacheBatchMissUniformPsConstHashMemoProbe(bool hit);
void countD3D9SnapshotCacheBatchMissUniformVsConstHashMemoStore();
void countD3D9SnapshotCacheBatchMissUniformPsConstHashMemoStore();
void countD3D9SnapshotCacheBatchMissSemanticReuseProbe(bool hit,
                                                       std::uint32_t distance);
// Opt-in semantic classification of a batch snapshot-cache miss. These
// counters are intentionally separate from the reuse-probe history: this
// observer compares the previous lane snapshot with the rebuilt snapshot and
// reports which semantic dimensions changed, without affecting cache use.
void countD3D9SnapshotCacheBatchMissSemanticObservation(
    bool sameSemantic, bool shaderLayoutChanged,
    bool uniformGenerationChanged, bool uniformPayloadChanged,
    bool resourceIdentityChanged);
void countD3D9SnapshotUniformBuildCall();
void countD3D9SnapshotUniformBuildVsConstCopyCpuTime(std::uint64_t nanoseconds);
void countD3D9SnapshotUniformBuildPsConstCopyCpuTime(std::uint64_t nanoseconds);
void countD3D9SnapshotUniformBuildFfpMatrixCpuTime(std::uint64_t nanoseconds);
void countD3D9SnapshotUniformBuildFfpMaterialLightCpuTime(std::uint64_t nanoseconds);
void countD3D9SnapshotUniformBuildTextureTransformCpuTime(std::uint64_t nanoseconds);
void countD3D9SnapshotUniformBuildClipPlaneCpuTime(std::uint64_t nanoseconds);
void countD3D9SnapshotUniformBuildHashCpuTime(std::uint64_t nanoseconds);
void countD3D9SnapshotUniformBuildVsConstHashCpuTime(std::uint64_t nanoseconds);
void countD3D9SnapshotUniformBuildPsConstHashCpuTime(std::uint64_t nanoseconds);
void countD3D9SnapshotUniformBuildNonConstHashCpuTime(std::uint64_t nanoseconds);
void countD3D9SnapshotUniformBuildNonConstHashWorldViewProjCpuTime(std::uint64_t nanoseconds);
void countD3D9SnapshotUniformBuildNonConstHashFfpWorldViewCpuTime(std::uint64_t nanoseconds);
void countD3D9SnapshotUniformBuildNonConstHashFfpNormalMatrixCpuTime(std::uint64_t nanoseconds);
void countD3D9SnapshotUniformBuildNonConstHashMaterialCpuTime(std::uint64_t nanoseconds);
void countD3D9SnapshotUniformBuildNonConstHashLightsCpuTime(std::uint64_t nanoseconds);
void countD3D9SnapshotUniformBuildNonConstHashFfpBlendWvpCpuTime(std::uint64_t nanoseconds);
void countD3D9SnapshotUniformBuildNonConstHashTextureTransformsCpuTime(std::uint64_t nanoseconds);
void countD3D9SnapshotUniformBuildNonConstHashClipPlanesCpuTime(std::uint64_t nanoseconds);
void countD3D9SnapshotUniformBuildPayloadCombineHashCpuTime(std::uint64_t nanoseconds);
void countD3D9SnapshotUniformBuildVsConstHashFull();
void countD3D9SnapshotUniformBuildPsConstHashFull();
void countD3D9SnapshotUniformBuildVsConstHashFullNoUsage();
void countD3D9SnapshotUniformBuildPsConstHashFullNoUsage();
void countD3D9SnapshotUniformBuildVsConstHashFullUnknown();
void countD3D9SnapshotUniformBuildPsConstHashFullUnknown();
void countD3D9SnapshotUniformBuildVsConstHashFullUnknownBytecode();
void countD3D9SnapshotUniformBuildPsConstHashFullUnknownBytecode();
void countD3D9SnapshotUniformBuildVsConstHashFullUnknownNonBytecode();
void countD3D9SnapshotUniformBuildPsConstHashFullUnknownNonBytecode();
void countD3D9SnapshotUniformBuildVsConstHashFullIndexedFloat();
void countD3D9SnapshotUniformBuildVsConstHashFullIndexedFloatMinSafeBytes(std::uint64_t bytes);
void countD3D9SnapshotUniformBuildVsConstHashFullIndexedFloatPotentialSavedBytes(std::uint64_t bytes);
void countD3D9SnapshotUniformBuildPsConstHashFullIndexedFloat();
void countD3D9SnapshotUniformBuildVsConstHashFullIndexedInt();
void countD3D9SnapshotUniformBuildPsConstHashFullIndexedInt();
void countD3D9SnapshotUniformBuildVsConstHashFullIndexedBool();
void countD3D9SnapshotUniformBuildPsConstHashFullIndexedBool();
void countD3D9SnapshotUniformBuildVsConstHashBytes(std::uint64_t bytes);
void countD3D9SnapshotUniformBuildPsConstHashBytes(std::uint64_t bytes);
void countDrawUniformPayloadLookupCandidateHit();
void countDrawUniformPayloadLookupLastHit();
void countDrawUniformPayloadLookupBucketHit();
void countDrawUniformPayloadLookupBucketMiss();
void countDrawUniformPayloadLookupLinearHit();
void countDrawUniformPayloadLookupBucketProbe(std::uint64_t probes);
void countDrawUniformPayloadLookupBucketCollision(std::uint64_t collisions);
void countDrawUniformPayloadLookupHashCollision(std::uint64_t collisions);
void countDrawUniformPayloadLookupSemanticHashMiss(std::uint64_t bytes);
void countDrawUniformPayloadLookupCpuTime(std::uint64_t nanoseconds);
void countDrawUniformPayloadLookupBucketCpuTime(std::uint64_t nanoseconds);
void countDrawUniformPayloadAppend();
void countDrawUniformPayloadAppendBytes(std::uint64_t bytes);
void countDrawUniformFixedPayloadAppend();
void countDrawUniformFixedPayloadAppendBytes(std::uint64_t bytes);
void countDrawUniformVertexConstantsAppend();
void countDrawUniformVertexConstantsAppendBytes(std::uint64_t bytes);
void countDrawUniformPixelConstantsAppend();
void countDrawUniformPixelConstantsAppendBytes(std::uint64_t bytes);
void countDrawUniformPayloadAppendFixedFindCpuTime(std::uint64_t nanoseconds);
void countDrawUniformPayloadAppendVertexFindCpuTime(std::uint64_t nanoseconds);
void countDrawUniformPayloadAppendPixelFindCpuTime(std::uint64_t nanoseconds);
void countDrawUniformPayloadAppendFixedAppendCpuTime(std::uint64_t nanoseconds);
void countDrawUniformPayloadAppendVertexAppendCpuTime(std::uint64_t nanoseconds);
void countDrawUniformPayloadAppendPixelAppendCpuTime(std::uint64_t nanoseconds);
void countDrawUniformPayloadMaterialized(DrawUniformPayloadMaterializeSite site,
                                         std::uint64_t bytes);
void countDrawUniformPayloadMaterializeFallback(
    DrawUniformPayloadMaterializeSite site);
void countDrawUniformPayloadMaterializeCpuTime(
    DrawUniformPayloadMaterializeSite site,
    std::uint64_t nanoseconds);
void countDrawUniformPayloadAppendReserveCpuTime(std::uint64_t nanoseconds);
void countDrawUniformPayloadAppendCopyCpuTime(std::uint64_t nanoseconds);
void countDrawUniformPayloadAppendLinkCpuTime(std::uint64_t nanoseconds);
void countDrawCall(std::uint32_t primitiveType,
                   std::uint32_t primitiveCount,
                   std::uint64_t vertexCount,
                   bool indexed,
                   bool expandedIndexed,
                   std::size_t userVertexBytes,
                   std::size_t userIndexBytes);
void countBaseStateBind(std::uint32_t textureBinds,
                        std::uint32_t samplerBinds,
                        std::uint32_t vertexBufferBinds,
                        std::uint32_t indexBufferBinds,
                        std::uint32_t uniformBufferBinds,
                        std::uint32_t pipelineBinds,
                        std::uint32_t depthStateBinds,
                        std::uint32_t viewportBinds,
                        std::uint32_t scissorBinds,
                        std::uint32_t rasterizerBinds);
void countBaseStateBindSkip(std::uint32_t textureBinds,
                            std::uint32_t samplerBinds);
// Extended skip-tracking for the seven bind classes that previously had
// no _skipped counter. Added 2026-06-05 per the present-pacing
// encode-budget attribution; the encoder calls these from each
// `set*Cached` lambda when the shadow check returns a hit.
void countBaseStateBindSkipExtended(std::uint32_t vertexBufferBinds,
                                    std::uint32_t indexBufferBinds,
                                    std::uint32_t pipelineBinds,
                                    std::uint32_t depthStateBinds,
                                    std::uint32_t viewportBinds,
                                    std::uint32_t scissorBinds,
                                    std::uint32_t rasterizerBinds);
void countDrawShaderBucket(std::uint64_t vertexShaderHash,
                           std::uint64_t pixelShaderHash,
                           std::uint64_t variantHash);
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
                                  std::uint64_t vertexDeclHash);
void countSubmitDrawCpuTime(std::uint64_t nanoseconds);
// M4 — per-command-buffer GPU time (MTLCommandBuffer.GPUEndTime -
// GPUStartTime) in nanoseconds. Driver-returned 0/non-monotonic values
// must be filtered at the call site.
void countGpuCommandBufferTime(std::uint64_t nanoseconds);
// Stage-boundary counter sample path: per-render-encoder GPU duration
// resolved from MTLCounterSampleBuffer timestamps. rt/depth handles are kept
// as the first DOD buckets. passType is the MetalCommandKind-aligned render
// helper bucket; psoHandle is (generation << 32) | slot when the encoder
// carries a resolved PsoHandle.
void countRenderEncoderGpuTime(std::uint64_t nanoseconds,
                               std::uint32_t passType,
                               std::uint64_t rtHandle,
                               std::uint64_t depthHandle,
                               std::uint64_t psoHandle);
void countEncodeChunkCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawPipelineLookupCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawUniformBuildCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawFvfDecodeCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawBindingPacketCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawBindingPacketPlanCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawBindingPacketPlanFragmentCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawBindingPacketPlanVertexCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawBindingPacketPlanExtraStreamCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawBindingPacketPlanRasterCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawBindingPacketCacheCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawBindingPacketCacheKeyCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawBindingPacketCacheHashCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawBindingPacketCacheProbeCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawBindingPacketCacheStoreCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawBindingPacketCacheHits(std::uint64_t hits);
void countEncodeDrawBindingPacketCacheMisses(std::uint64_t misses);
void countEncodeDrawBindingPacketCacheCollisions(std::uint64_t collisions);
void countEncodeDrawBindingPacketTextureRecordCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawArgbufSetupCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawArgbufOpenCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawArgbufOpenCallCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawArgbufReopenPostCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawArgbufReopenTableProbeCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawArgbufReopenTableShadowStoreCpuTime(
    std::uint64_t nanoseconds);
void countEncodeDrawArgbufReopenByteAccountCpuTime(
    std::uint64_t nanoseconds);
void countEncodeDrawArgbufReopenCbufCacheProbeCpuTime(
    std::uint64_t nanoseconds);
void countEncodeDrawArgbufReopenCbufDirtyScanCpuTime(
    std::uint64_t nanoseconds);
void countEncodeDrawArgbufReopenCbufForceDirtyCpuTime(
    std::uint64_t nanoseconds);
void countEncodeDrawArgbufOpenReserveCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawArgbufOpenSetArgumentBufferCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawArgbufTableBindCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawArgbufTableBindCalls(std::uint64_t calls);
void countEncodeDrawArgbufTableBindSkipped(std::uint64_t skips);
void countEncodeDrawArgbufCbufUpdateCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawArgbufCbufUpdateCalls(std::uint64_t calls);
void countEncodeDrawArgbufCbufUpdateDirtyCalls(std::uint64_t calls);
void countEncodeDrawArgbufCbufUpdateSkippedClean(std::uint64_t skips);
void countEncodeDrawArgbufCbufUpdateWriteCalls(std::uint64_t calls);
void countEncodeDrawArgbufCbufBuildCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawArgbufCbufUploadCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawArgbufCbufSetBufferCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawArgbufCbufBuildVsCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawArgbufCbufBuildPsCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawArgbufCbufBuildFfpVsCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawArgbufCbufBuildFfpPsCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawArgbufCbufUploadVsCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawArgbufCbufUploadPsCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawArgbufCbufUploadFfpVsCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawArgbufCbufUploadFfpPsCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawArgbufCbufSetBufferVsCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawArgbufCbufSetBufferPsCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawArgbufCbufSetBufferFfpVsCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawArgbufCbufSetBufferFfpPsCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawArgbufCbufUploadPlanCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawArgbufCbufUploadPlanVsCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawArgbufCbufUploadPlanPsCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawArgbufCbufBindingHashCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawArgbufCbufBindingHashVsCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawArgbufCbufBindingHashPsCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawArgbufCbufBindingHashFfpVsCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawArgbufCbufBindingHashFfpPsCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawArgbufCbufBindingWriteCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawArgbufCbufBindingWriteVsCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawArgbufCbufBindingWritePsCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawArgbufCbufBindingWriteFfpVsCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawArgbufCbufBindingWriteFfpPsCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawArgbufCbufObserverCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawArgbufCbufObserverVsCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawArgbufCbufObserverPsCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawArgbufCbufObserverFfpVsCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawArgbufCbufObserverFfpPsCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawArgbufCbufCacheMergeCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawArgbufCbufCachedRepointCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawArgbufCbufFullRepointCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawArgbufCbufCachedRepointVsCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawArgbufCbufCachedRepointPsCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawArgbufCbufCachedRepointFfpVsCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawArgbufCbufCachedRepointFfpPsCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawArgbufCbufCachedRepointCalls(std::uint64_t calls);
void countEncodeDrawArgbufCbufCachedRepointBytes(std::uint64_t bytes);
void countEncodeDrawArgbufCbufCachedRepointVsCalls(std::uint64_t calls);
void countEncodeDrawArgbufCbufCachedRepointPsCalls(std::uint64_t calls);
void countEncodeDrawArgbufCbufCachedRepointFfpVsCalls(std::uint64_t calls);
void countEncodeDrawArgbufCbufCachedRepointFfpPsCalls(std::uint64_t calls);
void countEncodeDrawArgbufCbufCachedRepointVsBytes(std::uint64_t bytes);
void countEncodeDrawArgbufCbufCachedRepointPsBytes(std::uint64_t bytes);
void countEncodeDrawArgbufCbufCachedRepointFfpVsBytes(std::uint64_t bytes);
void countEncodeDrawArgbufCbufCachedRepointFfpPsBytes(std::uint64_t bytes);
void countEncodeDrawArgbufCbufContentProbeCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawArgbufCbufContentProbeVsCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawArgbufCbufContentProbePsCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawArgbufCbufContentProbeFfpPsCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawArgbufCbufContentProbeCalls(std::uint64_t calls);
void countEncodeDrawArgbufCbufContentProbeVsHits(std::uint64_t hits);
void countEncodeDrawArgbufCbufContentProbeVsMisses(std::uint64_t misses);
void countEncodeDrawArgbufCbufContentProbePsHits(std::uint64_t hits);
void countEncodeDrawArgbufCbufContentProbePsMisses(std::uint64_t misses);
void countEncodeDrawArgbufCbufContentProbeFfpPsHits(std::uint64_t hits);
void countEncodeDrawArgbufCbufContentProbeFfpPsMisses(std::uint64_t misses);
void countEncodeDrawArgbufCbufReopenFullRepointCalls(std::uint64_t calls);
void countEncodeDrawArgbufCbufReopenNoDirtyHashMismatch(std::uint64_t calls);
void countEncodeDrawArgbufCbufReopenPartialCandidates(std::uint64_t calls);
void countEncodeDrawArgbufCbufReopenDirtyVs(std::uint64_t calls);
void countEncodeDrawArgbufCbufReopenDirtyPs(std::uint64_t calls);
void countEncodeDrawArgbufCbufReopenDirtyFfpVs(std::uint64_t calls);
void countEncodeDrawArgbufCbufReopenDirtyFfpPs(std::uint64_t calls);
void countEncodeDrawArgbufCbufUpdateVsCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawArgbufCbufUpdatePsCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawArgbufCbufUpdateFfpVsCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawArgbufCbufUpdateFfpPsCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawArgbufCbufUpdateVsCalls(std::uint64_t calls);
void countEncodeDrawArgbufCbufUpdatePsCalls(std::uint64_t calls);
void countEncodeDrawArgbufCbufUpdateFfpVsCalls(std::uint64_t calls);
void countEncodeDrawArgbufCbufUpdateFfpPsCalls(std::uint64_t calls);
void countEncodeDrawArgbufCbufUpdateVsBytes(std::uint64_t bytes);
void countEncodeDrawArgbufCbufUpdatePsBytes(std::uint64_t bytes);
void countEncodeDrawArgbufCbufUpdateFfpVsBytes(std::uint64_t bytes);
void countEncodeDrawArgbufCbufUpdateFfpPsBytes(std::uint64_t bytes);
void countEncodeDrawArgbufCbufDirtyVsIdentityProbeCalls(std::uint64_t calls);
void countEncodeDrawArgbufCbufDirtyVsIdentityHits(std::uint64_t hits);
void countEncodeDrawArgbufCbufDirtyVsIdentityMisses(std::uint64_t misses);
void countEncodeDrawArgbufCbufDirtyVsIdentityNoCache(std::uint64_t calls);
void countEncodeDrawArgbufCbufDirtyVsIdentityHitBytes(std::uint64_t bytes);
void countEncodeDrawArgbufCbufDirtyVsIdentityMissBytes(std::uint64_t bytes);
void countEncodeDrawArgbufPayloadDeltaProbeCalls(std::uint64_t calls);
void countEncodeDrawArgbufPayloadDeltaFirst(std::uint64_t calls);
void countEncodeDrawArgbufPayloadDeltaSame(std::uint64_t calls);
void countEncodeDrawArgbufPayloadDeltaChanged(std::uint64_t calls);
void countEncodeDrawArgbufPayloadDeltaChangedVs(std::uint64_t calls);
void countEncodeDrawArgbufPayloadDeltaChangedPs(std::uint64_t calls);
void countEncodeDrawArgbufPayloadDeltaChangedVsPs(std::uint64_t calls);
void countEncodeDrawArgbufPayloadDeltaChangedNonConstOnly(std::uint64_t calls);
void countEncodeDrawArgbufPayloadDeltaChangedVsFloat(std::uint64_t calls);
void countEncodeDrawArgbufPayloadDeltaChangedVsInt(std::uint64_t calls);
void countEncodeDrawArgbufPayloadDeltaChangedVsBool(std::uint64_t calls);
void countEncodeDrawArgbufPayloadDeltaChangedPsFloat(std::uint64_t calls);
void countEncodeDrawArgbufPayloadDeltaChangedPsInt(std::uint64_t calls);
void countEncodeDrawArgbufPayloadDeltaChangedPsBool(std::uint64_t calls);
void countEncodeDrawArgbufPayloadDeltaChangedVsFloatRegs(std::uint64_t regs);
void countEncodeDrawArgbufPayloadDeltaChangedVsFloatRegsMax(std::uint64_t regs);
void countEncodeDrawArgbufPayloadDeltaChangedVsFloatRegsLe1(std::uint64_t calls);
void countEncodeDrawArgbufPayloadDeltaChangedVsFloatRegsLe4(std::uint64_t calls);
void countEncodeDrawArgbufPayloadDeltaChangedVsFloatRegsLe16(std::uint64_t calls);
void countEncodeDrawArgbufPayloadDeltaChangedVsFloatRegsLe64(std::uint64_t calls);
void countEncodeDrawArgbufPayloadDeltaChangedVsFloatRegsGt64(std::uint64_t calls);
void countEncodeDrawArgbufPayloadDeltaChangedVsFloatRegsLe1Sum(std::uint64_t regs);
void countEncodeDrawArgbufPayloadDeltaChangedVsFloatRegsLe4Sum(std::uint64_t regs);
void countEncodeDrawArgbufPayloadDeltaChangedVsFloatRegsLe16Sum(std::uint64_t regs);
void countEncodeDrawArgbufPayloadDeltaChangedVsFloatRegsLe64Sum(std::uint64_t regs);
void countEncodeDrawArgbufPayloadDeltaChangedVsFloatRegsGt64Sum(std::uint64_t regs);
void countEncodeDrawArgbufPayloadDeltaChangedVsFloatPrefixRegs(std::uint64_t regs);
void countEncodeDrawArgbufPayloadDeltaChangedVsFloatPrefixRegsMax(std::uint64_t regs);
void countEncodeDrawArgbufPayloadDeltaChangedVsFloatSpanRegs(std::uint64_t regs);
void countEncodeDrawArgbufPayloadDeltaChangedVsFloatSpanRegsMax(std::uint64_t regs);
void countEncodeDrawArgbufPayloadDeltaChangedVsFloatFullPrefix(std::uint64_t calls);
void countEncodeDrawArgbufPayloadDeltaChangedVsFloatFullPrefixRegs(std::uint64_t regs);
void countEncodeDrawArgbufPayloadDeltaChangedPsFloatRegs(std::uint64_t regs);
void countEncodeDrawArgbufPayloadDeltaChangedPsFloatRegsMax(std::uint64_t regs);
void countEncodeDrawArgbufPayloadDeltaChangedPsFloatRegsLe1(std::uint64_t calls);
void countEncodeDrawArgbufPayloadDeltaChangedPsFloatRegsLe4(std::uint64_t calls);
void countEncodeDrawArgbufPayloadDeltaChangedPsFloatRegsLe16(std::uint64_t calls);
void countEncodeDrawArgbufPayloadDeltaChangedPsFloatRegsLe64(std::uint64_t calls);
void countEncodeDrawArgbufPayloadDeltaChangedPsFloatRegsGt64(std::uint64_t calls);
void countEncodeDrawArgbufPayloadDeltaChangedPsFloatRegsLe1Sum(std::uint64_t regs);
void countEncodeDrawArgbufPayloadDeltaChangedPsFloatRegsLe4Sum(std::uint64_t regs);
void countEncodeDrawArgbufPayloadDeltaChangedPsFloatRegsLe16Sum(std::uint64_t regs);
void countEncodeDrawArgbufPayloadDeltaChangedPsFloatRegsLe64Sum(std::uint64_t regs);
void countEncodeDrawArgbufPayloadDeltaChangedPsFloatRegsGt64Sum(std::uint64_t regs);
void countEncodeDrawArgbufPayloadDeltaReopenFirst(std::uint64_t calls);
void countEncodeDrawArgbufPayloadDeltaReopenPayloadChanged(std::uint64_t calls);
void countEncodeDrawArgbufPayloadDeltaReopenPayloadSame(std::uint64_t calls);
void countEncodeDrawArgbufPayloadDeltaReopenResourceArray(std::uint64_t calls);
void countEncodeDrawArgbufPayloadDeltaReopenCbufOnly(std::uint64_t calls);
void countEncodeDrawArgbufPayloadDeltaReopenCbufOnlyFirst(std::uint64_t calls);
void countEncodeDrawArgbufPayloadDeltaReopenCbufOnlyPayloadChanged(
    std::uint64_t calls);
void countEncodeDrawStreamBindCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawStreamBindRasterPhaseCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawStreamBindRasterPhaseCalls(std::uint64_t calls);
void countEncodeDrawStreamBindFfpStreamCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawStreamBindFfpStreamCalls(std::uint64_t calls);
void countEncodeDrawStreamBindShaderStreamCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawStreamBindShaderStreamCalls(std::uint64_t calls);
void countEncodeDrawStreamBindTexturePhaseCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawStreamBindTexturePhaseCalls(std::uint64_t calls);
void countEncodeDrawStreamBindIndexPhaseCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawStreamBindIndexPhaseCalls(std::uint64_t calls);
void countEncodeDrawTextureSamplerFragmentResolveCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawTextureSamplerFragmentResolveCalls(std::uint64_t calls);
void countEncodeDrawTextureSamplerFragmentResolveTextureCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawTextureSamplerFragmentResolveTextureCalls(std::uint64_t calls);
void countEncodeDrawTextureSamplerFragmentResourceArrayCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawTextureSamplerFragmentResourceArrayCalls(std::uint64_t calls);
void countEncodeDrawTextureSamplerFragmentDirectCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawTextureSamplerFragmentDirectCalls(std::uint64_t calls);
void countEncodeDrawTextureSamplerFragmentDirectTextureCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawTextureSamplerFragmentDirectTextureCalls(std::uint64_t calls);
void countEncodeDrawTextureSamplerFragmentDirectTextureSetCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawTextureSamplerFragmentDirectTextureSetCalls(std::uint64_t calls);
void countEncodeDrawTextureSamplerFragmentDirectSamplerCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawTextureSamplerFragmentDirectSamplerCalls(std::uint64_t calls);
void countEncodeDrawTextureSamplerFragmentDirectSamplerSetCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawTextureSamplerFragmentDirectSamplerSetCalls(std::uint64_t calls);
void countEncodeDrawTextureSamplerSamplerLookupCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawTextureSamplerSamplerLookupCalls(std::uint64_t calls);
void countEncodeDrawTextureSamplerSamplerLookupSkippedPrehandle(std::uint64_t skips);
void countEncodeDrawTextureSamplerLodBiasCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawTextureSamplerLodBiasCalls(std::uint64_t calls);
void countEncodeDrawTextureSamplerVertexResolveCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawTextureSamplerVertexResolveCalls(std::uint64_t calls);
void countEncodeDrawTextureSamplerVertexDirectCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawTextureSamplerVertexDirectCalls(std::uint64_t calls);
void countEncodeDrawRasterStateCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawVertexStreamBindCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawTextureSamplerBindCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawIndexSetupCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawIndexSourceResolveCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawIndexCacheLookupCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawIndexCacheCandidateCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawIndexCacheOriginalMeasureCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawIndexCacheCandidateBuildCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawIndexCacheCandidateReadCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawIndexCacheCandidateAdjacencyCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawIndexCacheCandidateSelectCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawIndexCacheCandidateWriteCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawIndexCacheCandidateSelectVolume(std::uint64_t calls,
                                                    std::uint64_t slots,
                                                    std::uint64_t scored,
                                                    std::uint64_t skipped,
                                                    std::uint64_t maxCandidates);
void countEncodeDrawIndexCacheCandidateMeasureCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawIndexCacheGateCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawIndexCacheApplyCpuTime(std::uint64_t nanoseconds);
inline constexpr std::size_t kCompatibleIndexedDrawMergeRejectCount = 10u;
inline constexpr std::size_t
    kCompatibleIndexedDrawMergeRelaxationSetCount = 8u;
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
    std::uint64_t otherRelaxationSetPairs);
void countIndexedCacheOptCandidate(bool available,
                                   std::uint64_t bytes,
                                   std::uint64_t originalMiss16,
                                   std::uint64_t originalMiss32,
                                   std::uint64_t originalMiss64,
                                   std::uint64_t candidateMiss16,
                                   std::uint64_t candidateMiss32,
                                   std::uint64_t candidateMiss64);
void countIndexedCacheOptCandidateGate(bool passed,
                                       std::uint64_t primitiveCount,
                                       bool opaqueDepth,
                                       bool screenBlend);
void countIndexedCacheOptCandidatePreEligibility(bool eligible);
void countIndexedCacheOptCandidateBudgetAbort(
    std::uint8_t reason);
void countReorderedIndexCacheLookup(bool hit,
                                    bool rejected,
                                    bool created,
                                    std::uint64_t createdBytes);
void countEncodeDrawIssueCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawStreamBindViewportCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawStreamBindFfpCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawStreamBindVsCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawStreamBindTextureCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawStreamBindIndexCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawFvfDecodeDeclCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawFvfDecodeBytesCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawFvfDecodeExpandedCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawUniformBuildMainCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawUniformBuildFfpCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawUniformBuildVsCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawPhaseSetupCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawPhaseArgbufUniformCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawPhaseStreamPrepCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawPhaseFfpVertexCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawPhaseVertexBindCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawPhaseBaseStateCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawPhaseTileFfpFallthroughCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawPhaseRemainderCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawIssueIndexedCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawIssueNonIndexedCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawIssueExpandedIndexedCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawIssueSplitIndexedCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawIssueMetalCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawIssueVisibilityCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawPsoPrefetch(bool handleAvailable,
                                bool usedHandle,
                                bool hasBindingOverride,
                                bool bindingOverrideCompatible,
                                bool bypassProbe);
void countTransientUploadCpuTime(std::uint64_t nanoseconds, std::size_t bytes);
void countD3D9BufferLock(std::uint64_t nanoseconds,
                         std::uint64_t bytes,
                         std::uint64_t shadowAllocNanoseconds,
                         std::uint64_t shadowCopyNanoseconds,
                         std::uint32_t d3dFlags,
                         std::uint32_t usage,
                         std::uint32_t pool,
                         bool fullResource,
                         bool shadowCopy);
// dxmt9c_buffer_unlock attribution (present-pacing-bridge-crossing-decomposition.237:
// unmeasured 82.1us/call PE round-trip, 3x lock's, with two candidate owners —
// the wow64 shadow->native writeback memcpy and the core b->obj->unlock() call
// — that could not be separated from PE-side timing alone).
// nanoseconds times the whole call; writebackNanoseconds/writebackBytes time
// the shadow writeback memcpy (0/0 when no shadow writeback ran); coreNanoseconds
// times b->obj->unlock(); uploaded is !lastLockReadOnly at call time; shadowActive
// is b->wow64Lock.active on entry.
void countD3D9BufferUnlock(std::uint64_t nanoseconds,
                           std::uint64_t writebackNanoseconds,
                           std::uint64_t coreNanoseconds,
                           std::uint64_t writebackBytes,
                           bool uploaded,
                           bool shadowActive);
// R-BACK-44.2 — the Managed mutation-offload subset of the family above, so the
// synchronous half's shrink is attributable rather than inferred from the
// aggregate. `stagedBytes` is the exact dirty span copied into task-owned
// storage; `stageNanoseconds` times that copy plus the reservation;
// `rotateNanoseconds` times the synchronous logical backing rotation (step 2).
// A deferred unlock never reaches `uploadBufferData`, so it contributes to
// neither `d3d9_buffer_upload_full_*` nor `managed_buffer_upload_bytes`.
void countD3D9BufferUnlockDeferred(std::uint64_t stagedBytes,
                                   std::uint64_t stageNanoseconds,
                                   std::uint64_t rotateNanoseconds);
// R-BACK-44.2 step-1 failure: admitted class, reserve/stage rejected, nothing
// rotated, no lock state cleared, retryable HRESULT returned to the app.
void countD3D9BufferUnlockDeferredRejected();
// Buffer::unlock backend-call split (present-pacing-bridge-crossing-decomposition.237
// R-237.4: decomposes d3d9_buffer_unlock_core_ms, the b->obj->unlock() span above,
// into its three backend calls in src/d3d9/core_buffer.cpp). Full/range uploads are
// mutually exclusive per unlock (exact-range NOOVERWRITE takes the range path; every
// other upload takes the full path); unmap always runs. pool is the static_cast<u32>
// of dxmt9::core::Pool at call time; only Default/Managed get a byte-and-call split,
// matching the range path being Default-only by construction.
// R-237.5: behind DXMT9_DISCARD_RANGE_UPLOAD, a Default+Dynamic+DISCARD unlock
// also takes the range path; `discard` distinguishes that admitted case so an
// A/B can gate on the mechanism firing (d3d9_buffer_upload_range_discard_calls
// / _bytes) without splitting the shared range timing.
void countD3D9BufferUploadFull(std::uint64_t nanoseconds,
                                std::uint64_t bytes,
                                std::uint32_t pool);
void countD3D9BufferUploadRange(std::uint64_t nanoseconds, std::uint64_t bytes,
                                 bool discard);
void countD3D9BufferUnmap(std::uint64_t nanoseconds);
// dxmt9c_surface_lock_rect attribution (state-churn-encode-append-decomposition.24/.26:
// 1.33 ms/call, 0.58 ms/present on GT2, confirmed NOT a drain-fence wait).
// coreNanoseconds times the s->obj->lockRect(...) call into core Surface/Texture
// lockRect; shadowNanoseconds times the whole wow64-pointer-shadow block
// (alloc-or-reuse + copyNativeToShadow) when the wow64 shadow path is taken (0
// on 64-bit or when the native pointer already fits 32 bits).
void countD3D9SurfaceLockRect(std::uint64_t coreNanoseconds,
                              std::uint64_t shadowNanoseconds,
                              std::uint64_t bytes,
                              bool discard);
// Micro-split of the surface shadow block above: alloc covers the
// releaseShadowLock + allocateLow4GB pair when the cached shadow was missing
// or too small; copy covers copyNativeToShadow on every shadowed lock.
void countSurfaceLockShadowAlloc(std::uint64_t nanoseconds, std::uint64_t bytes);
void countSurfaceLockShadowCopy(std::uint64_t nanoseconds, std::uint64_t bytes);
// dxmt9c_texture_lock_rect equivalent of the surface counters above; shares
// the same opcode family and cost shape (core lockRect call vs. wow64 shadow
// alloc/copy block).
void countD3D9TextureLockRect(std::uint64_t coreNanoseconds,
                              std::uint64_t shadowNanoseconds,
                              std::uint64_t bytes,
                              bool discard);
// Times the D3DLOCK_DISCARD zero-fill (`storage.bytes.assign(...)`) inside
// core Texture::lockRect (src/d3d9/core_texture.cpp). Texture::lockRect is
// the single non-palettized lockRect body shared by both
// dxmt9c_texture_lock_rect and dxmt9c_surface_lock_rect (the latter via
// Surface::lockRect's texture-container delegation), so this one counter
// pair attributes the fill cost regardless of which PE entry point locked
// the resource. It does NOT cover Surface's own standalone-backing discard
// fill (core_surface.cpp, non-texture-container surfaces such as plain
// render targets) — that is a separate storage class outside the scope of
// this task and remains uninstrumented.
void countTextureLockDiscardFillCpuTime(std::uint64_t nanoseconds, std::uint64_t bytes);
void countUniformVsConsts(std::size_t bytes);
void countUniformPsConsts(std::size_t bytes);
void countUniformFfpVs(std::size_t bytes);
void countUniformFfpPs(std::size_t bytes);
void countUniformVolatilePush();
// R-BACK-5.7. Bumps only on the discrete (non-unified-memory) blit path.
void countManagedTextureUploadBlit(std::uint64_t bytes);
void countTexturePixelFormatViewSuppressedRt(std::uint64_t bytes);
// R-BACK-14.* — MTLHeap pooling counters.
void countHeapAlloc(std::uint64_t bytes);
void countHeapInstance();
void countHeapDirectFallback();
void countHeapFragmentationFailure();
void countHeapCompaction();
void countHeapAllocFailure();
void countUseHeap();
void countUseResource();
// Wow64 shadow-lock low-4GB block pool (device_c_low4gb_pool.hpp,
// wired in device_c_marshal.cpp's acquireLow4GB/releaseLow4GB). A hit
// avoids the OS low-address-scan allocation entirely; a miss falls
// through to allocateLow4GB's slow path; an eviction is a release that
// could not be pooled (non-poolable size, bucket full, or over the
// pool's total-byte cap) and was freed directly instead.
void countSurfaceLockShadowPoolHit();
void countSurfaceLockShadowPoolMiss();
void countSurfaceLockShadowPoolEviction();
// R-BACK-13.* — Tile-Shader FFP counters.
void countTileFfpPass();
void countPortableFfpPass();
void countTileFfpFallbackPrecision();
void countTileFfpFallbackUnsupportedState();
void countTileFfpFallbackGpuFamily();
void countTileFfpFallbackMidPassIneligible();
void countTileFfpMidPassResplit();
// R-BACK-12.22~12.26 — Stage 2 Argbuf hybrid counters.
void countArgbufHybridEncoder();
void countStage1Encoder();
void countArgbufHybridFallback();
void countArgbufHybridBytes(std::uint64_t bytes);
void countStage1Bytes(std::uint64_t bytes);
// D3DBC bytecode safe-rejection counters. The shader decoder
// (translateD3DBytecodeToSpirv) treats malformed bytecode as input,
// not as an internal invariant violation: it returns an empty
// SpirvModule (which makes the upper translator emit a benign fallback
// MSL stub) and bumps one of the buckets below so a regression in a
// real workload surfaces in the `[dxmt9-perf]` line. Hot path stays
// allocation-free; logging is gated at trace level.
//   * truncated:           bytes < 4, mid-instruction truncation,
//                          or unterminated comment/rel-addr trailer
//   * unsupported_version: version DWORD is not vs/ps within the
//                          1.x-3.x supported range
//   * oob_register:        register index exceeds the spec maximum
//                          for its register file (e.g. cN >= 256,
//                          sN >= kMaxSamplers, rN >= 32)
//   * missing_end:         token stream ran out before kD3DSIO_END
//   * invalid_opcode:      decoder rejected an opcode whose operand
//                          count cannot be determined (catch-all for
//                          internal parser errors lifted out of
//                          std::runtime_error throws)
//   * tempfloat16_unsupported: a register operand encoded the SM3
//                          `D3DSPR_TEMPFLOAT16` (16) kind. dxmt9 has
//                          no fp16 lowering path so this rejects
//                          rather than silently misbinding as fp32.
//                          See specs/d3d9.plan.md §3 P1-2.
//   * label_unsupported:   a register operand encoded the SM3
//                          `D3DSPR_LABEL` (18) kind. Opcode-level
//                          LABEL / CALL / CALLNZ are already inlined
//                          by `inlineShaderSubroutines`; this bucket
//                          only fires for a register source/dest with
//                          the label kind, which has no MSL lowering.
//   * decl_usage_unsupported:  a vertex-declaration DCL usage code
//                          (D3DDECLUSAGE) is one of the six values
//                          dxmt9 cannot lower (TANGENT=6, BINORMAL=7,
//                          TESSFACTOR=8, FOG=11, DEPTH=12, SAMPLE=13).
//                          Single category counter — per-code
//                          inventory lives in specs/d3d9/gap_d3d9.md §A.4.
//   * decl_method_unsupported: a vertex-declaration DCL method code
//                          (D3DDECLMETHOD) is non-DEFAULT (PARTIALU=1,
//                          PARTIALV=2, CROSSUV=3, UV=4, LOOKUP=5,
//                          LOOKUPPRESAMPLED=6). dxmt9 only honors the
//                          DEFAULT (0) read path; the rest require
//                          tessellator stages with no Metal mapping.
//                          See specs/d3d9/gap_d3d9.md §A.5.
void countShaderDecoderRejectTruncated();
void countShaderDecoderRejectUnsupportedVersion();
void countShaderDecoderRejectOobRegister();
void countShaderDecoderRejectMissingEnd();
void countShaderDecoderRejectInvalidOpcode();
void countShaderDecoderRejectTempFloat16Unsupported();
void countShaderDecoderRejectLabelUnsupported();
void countShaderDecoderRejectDeclUsageUnsupported();
void countShaderDecoderRejectDeclMethodUnsupported();

namespace test {
// Test-only seam: snapshot the `shader_decoder_reject_*` buckets in
// declaration order. Returned values are raw counter atoms — when
// `DXMT_PERF_COUNTERS` is unset the count*() helpers no-op and the
// snapshot reads zero. Spec callers set the env var before invoking
// the decoder.
struct ShaderDecoderRejectSnapshot {
  std::uint64_t truncated = 0;
  std::uint64_t unsupportedVersion = 0;
  std::uint64_t oobRegister = 0;
  std::uint64_t missingEnd = 0;
  std::uint64_t invalidOpcode = 0;
  std::uint64_t tempFloat16Unsupported = 0;
  std::uint64_t labelUnsupported = 0;
  std::uint64_t declUsageUnsupported = 0;
  std::uint64_t declMethodUnsupported = 0;
};
ShaderDecoderRejectSnapshot snapshotShaderDecoderRejects();

// Test-only seam for the R-BACK-39.2 (Task B11) frame-graph observe-path
// counters. Raw counter atoms — zero unless DXMT_PERF_COUNTERS is set and the
// observe path ran. Mirrors snapshotShaderDecoderRejects so the observe spec
// can assert the counters moved without a full report parse.
struct FramegraphObserveSnapshot {
  std::uint64_t passesBuilt = 0;
  std::uint64_t passesCoalesced = 0;
  std::uint64_t passesDead = 0;
  std::uint64_t resourcesMemoryless = 0;
  std::uint64_t dagDumpsWritten = 0;
};
FramegraphObserveSnapshot snapshotFramegraphObserve();

struct FramegraphActiveRenderSeedSnapshot {
  std::uint64_t snapshotAbsent = 0;
  std::uint64_t snapshotIncomplete = 0;
  std::uint64_t applyApplied = 0;
  std::uint64_t applyInvalid = 0;
  std::uint64_t applyIncomplete = 0;
  std::uint64_t applyOverflow = 0;
  std::uint64_t appliedButUnmerged = 0;
  std::uint64_t passCoalesceBlockedCycle = 0;
  std::uint64_t passCoalesceSecondNonDraw = 0;
  std::uint64_t movedHeadProved = 0;
  std::uint64_t fallbackMovedHeadUnproved = 0;
  std::uint64_t fallbackInvalidPlan = 0;
  std::uint64_t fallbackLiveSetMismatch = 0;
  std::uint64_t fallbackDuplicateCommand = 0;
  std::uint64_t replayActivated = 0;
};
FramegraphActiveRenderSeedSnapshot snapshotFramegraphActiveRenderSeed();

struct FramegraphSourceLocalPassCoalesceSnapshot {
  std::uint64_t candidates = 0;
  std::uint64_t merged = 0;
  std::uint64_t blockedCycle = 0;
  std::uint64_t secondNonDraw = 0;
  std::uint64_t nonRenderIntervener = 0;
  std::uint64_t missingInvariant = 0;
  std::uint64_t dependencyKept = 0;
  std::uint64_t moveBefore = 0;
  std::uint64_t moveAfter = 0;
  std::uint64_t nonDrawIntervener = 0;
  std::uint64_t semanticIntervener = 0;
  std::uint64_t commandlessIntervener = 0;
  std::uint64_t commandlessReturn = 0;
  std::uint64_t legacyCandidates = 0;
  std::uint64_t arenaCandidates = 0;
  std::uint64_t unknownCandidates = 0;
  std::uint64_t identityKnownCandidates = 0;
  std::uint64_t identityMissingCandidates = 0;
};
FramegraphSourceLocalPassCoalesceSnapshot
snapshotFramegraphSourceLocalPassCoalesce();

struct FramegraphSourceLocalReplayOutcomeCount {
  std::uint64_t sources = 0;
  std::uint64_t candidates = 0;
  std::uint64_t merged = 0;
};
struct FramegraphSourceLocalReplayOutcomeSnapshot {
  FramegraphSourceLocalReplayOutcomeCount frontierRollback{};
  FramegraphSourceLocalReplayOutcomeCount frontierRollbackInvalidPlan{};
  FramegraphSourceLocalReplayOutcomeCount frontierRollbackLiveSetMismatch{};
  FramegraphSourceLocalReplayOutcomeCount frontierRollbackDuplicateCommand{};
  FramegraphSourceLocalReplayOutcomeCount frontierRollbackMovedHeadUnproved{};
  FramegraphSourceLocalReplayOutcomeCount finalInvalid{};
  FramegraphSourceLocalReplayOutcomeCount finalNaturalOrder{};
  FramegraphSourceLocalReplayOutcomeCount finalReorderedActivated{};
};
FramegraphSourceLocalReplayOutcomeSnapshot
snapshotFramegraphSourceLocalReplayOutcome();

struct CpuReadyMultiSourceSeedNaturalDistanceSnapshot {
  std::uint64_t missing = 0;
  std::uint64_t adjacent = 0;
  std::uint64_t intervening = 0;
};
CpuReadyMultiSourceSeedNaturalDistanceSnapshot
snapshotCpuReadyMultiSourceSeedNaturalDistance();

struct CpuReadyMultiSourceSeedNaturalAttributionSnapshot {
  std::uint64_t mergeOperations = 0;
  std::uint64_t mergeDistanceTotal = 0;
  std::uint64_t mergeDistanceMax = 0;
  std::uint64_t commandBefore = 0;
  std::uint64_t commandAfter = 0;
  std::uint64_t emptyIntervening = 0;
  std::uint64_t adjacent = 0;
  std::uint64_t dependencyKept = 0;
  std::uint64_t commandless = 0;
  std::uint64_t multiMerge = 0;
  std::uint64_t missing = 0;
};
CpuReadyMultiSourceSeedNaturalAttributionSnapshot
snapshotCpuReadyMultiSourceSeedNaturalAttribution();

struct CpuReadyMultiSourceSourceLocalFallbackSnapshot {
  std::uint64_t naturalStarted = 0;
  std::uint64_t naturalCompleted = 0;
  std::uint64_t naturalSources = 0;
  std::uint64_t permutationStarted = 0;
  std::uint64_t permutationCompleted = 0;
  std::uint64_t permutationSources = 0;
};
CpuReadyMultiSourceSourceLocalFallbackSnapshot
snapshotCpuReadyMultiSourceSourceLocalFallback();

struct RenderPassNaturalFallbackAttributionSnapshot {
  std::uint64_t begins = 0;
  std::uint64_t sameWindowDistance1 = 0;
  std::uint64_t sameWindowDistance2 = 0;
  std::uint64_t sameWindowDistance3To4 = 0;
  std::uint64_t crossWindowDistance1 = 0;
  std::uint64_t crossWindowDistance2 = 0;
  std::uint64_t crossWindowDistance3To4 = 0;
  std::uint64_t seedTicketsIssued = 0;
  std::uint64_t seedTicketsMatched = 0;
  std::uint64_t seedTicketsContinued = 0;
  std::uint64_t seedTicketsMismatch = 0;
  std::uint64_t seedTicketsUnconsumed = 0;
  std::uint64_t seedWitnessOverflow = 0;
  std::uint64_t seedWitnessMismatch = 0;
  std::uint64_t seedInstanceUnavailable = 0;
  std::uint64_t seedInstanceStale = 0;
  std::uint64_t seedBridgeDistance1 = 0;
  std::uint64_t seedBridgeDistance2 = 0;
  std::uint64_t seedBridgeDistance3To4 = 0;
};
RenderPassNaturalFallbackAttributionSnapshot
snapshotRenderPassNaturalFallbackAttribution();

struct RenderPassCloseAttributionSnapshot {
  std::uint64_t finalSessionCap = 0;
  std::uint64_t finalIndependent = 0;
  std::uint64_t finalInitializer = 0;
  std::uint64_t finalProducerWait = 0;
  std::uint64_t finalDrain = 0;
  std::uint64_t finalFailOther = 0;
  std::uint64_t adjacentSessionCap = 0;
  std::uint64_t adjacentIndependent = 0;
  std::uint64_t adjacentInitializer = 0;
  std::uint64_t adjacentProducerWait = 0;
  std::uint64_t adjacentDrain = 0;
  std::uint64_t adjacentFailOther = 0;
  std::uint64_t recorded = 0;
  std::uint64_t missing = 0;
  std::uint64_t terminalAdjacent = 0;
  std::uint64_t terminalNonAdjacent = 0;
  std::uint64_t terminalNotReopenedBeforePresent = 0;
  std::uint64_t shortCrossMatched = 0;
  std::uint64_t shortCrossMissing = 0;
  std::uint64_t finalRecorded = 0;
  std::uint64_t finalMissing = 0;
  std::uint64_t finalTerminalAdjacent = 0;
  std::uint64_t finalTerminalNonAdjacent = 0;
  std::uint64_t finalTerminalNotReopenedBeforePresent = 0;
};
RenderPassCloseAttributionSnapshot snapshotRenderPassCloseAttribution();

struct RenderPassShortReentryAttributionSnapshot {
  std::array<std::uint64_t, 8> distance1Disposition{};
  std::array<std::uint64_t, 8> distance2Disposition{};
  std::array<std::uint64_t, 4> distance1SourceShape{};
  std::array<std::uint64_t, 4> distance2SourceShape{};
  std::array<std::uint64_t, 12> priorCloseReason{};
  std::uint64_t priorCloseMissing = 0;
  std::uint64_t clearOpenTargetCount = 0;
  std::uint64_t clearOpenTargetPriorStoreBytes = 0;
  std::uint64_t clearOpenTargetCurrentLoadBytes = 0;
  std::uint64_t clearOpenNaturalCrossCount = 0;
  std::uint64_t clearOpenNaturalCrossPriorStoreBytes = 0;
  std::uint64_t clearOpenNaturalCrossCurrentLoadBytes = 0;
};
RenderPassShortReentryAttributionSnapshot
snapshotRenderPassShortReentryAttribution();

struct RenderPassStoreAccountingSnapshot {
  std::uint64_t colorStore = 0;
  std::uint64_t colorDontCare = 0;
  std::uint64_t depthStore = 0;
  std::uint64_t depthDontCare = 0;
  std::uint64_t stencilStore = 0;
  std::uint64_t stencilDontCare = 0;
  std::uint64_t tilePreservationBytes = 0;
};
RenderPassStoreAccountingSnapshot snapshotRenderPassStoreAccounting();

// Test-only seam for the queue-owned CPU-ready session lease.  The production
// counter remains private; this exposes only the live count needed by the
// terminal-drain proof, not the mutable counter storage.
std::uint64_t cpuReadySessionLeaseCurrent();

struct CpuReadySupplySnapshot {
  std::uint64_t legacyReplayEntryToPublish = 0;
  std::uint64_t legacyPublishToDequeue = 0;
  std::uint64_t arenaReplayEntryToPublish = 0;
  std::uint64_t arenaPublishToDequeue = 0;
  std::uint64_t attributionMisses = 0;
  std::uint64_t ledgerOverflows = 0;
};
CpuReadySupplySnapshot snapshotCpuReadySupply();
}  // namespace test

// R-BACK-3.7 / 3.8 / 4.8 — MTLBinaryArchive prewarming counters.
void countPrewarmEntriesLoaded(std::uint64_t entries);
void countPrewarmLoadCpuTime(std::uint64_t nanoseconds);
void countPrewarmFailureCorrupt();
void countPrewarmFailureSchema();
void countPrewarmFailureLockBusy();
void countPrewarmFailureMissing();
void countColdCompileAfterWarm();
void countArchiveBytes(std::uint64_t bytes);
// R-BACK-3.9 — Full prewarm demoted to lazy-equivalent behavior because
// the on-disk archive exceeded DXMT9_ARCHIVE_MAX_PREWARM_MB.
void countPrewarmDemotedBySize();
// R-BACK-3.9 — wall time (ns) of the whole async Full-load background
// thread, from spawn to archive attach + backfill drain. Broader than
// countPrewarmLoadCpuTime (which only covers the time inside run()'s
// locked classification/load span) — this is the end-to-end async op.
void countPrewarmAsyncCompletionCpuTime(std::uint64_t nanoseconds);
// R-BACK-3.10 — a bounded mid-session archive save actually ran.
void countPrewarmMilestoneSave();
// R-BACK-3.11 — an archive save (mid-session or shutdown) was skipped
// because this session's shader debug-env key was non-default.
void countPrewarmSaveSkippedDebugEnv();
// Render-pass load/store action histograms (R-BACK-15.10/15.11).
// `action` is the raw WMTLoadAction / WMTStoreAction enum value. Callers
// cast `static_cast<std::uint32_t>(load_action)`; we keep the API decoupled
// from winemetal.h so dxmt9_perf_counters.hpp stays lightweight.
void countRenderPassLoadActionColor(std::uint32_t action);
void countRenderPassLoadActionDepth(std::uint32_t action);
void countRenderPassLoadActionStencil(std::uint32_t action);
void countRenderPassStoreActionColor(std::uint32_t action);
void countRenderPassStoreActionDepth(std::uint32_t action);
void countRenderPassStoreActionStencil(std::uint32_t action);
void countRenderPassTilePreservationBytes(std::uint64_t bytes);
void countRenderPassSameKeyAdjacent();
void countRenderPassSameKeyReentry();
void countRenderPassSameKeyReentryDistance(std::uint32_t interveningPasses);
void countRenderPassSameKeyReentryDistance1Shape(bool sameColor,
                                                 bool sameDepth,
                                                 std::uint64_t bytes);
void countRenderPassSameKeyReentryPreservationBytes(std::uint64_t bytes);
void countRenderPassSameKeyReentryColorPreservationBytes(std::uint64_t bytes);
void countRenderPassSameKeyReentryDepthPreservationBytes(std::uint64_t bytes);
void countRenderPassShortReentryDisposition(std::uint32_t interveningPasses,
                                            std::uint8_t disposition);
void countRenderPassShortReentrySourceShape(std::uint32_t interveningPasses,
                                            std::uint8_t sourceShape);
void countRenderPassShortReentryPriorClose(EncoderSplitReason reason);
void countRenderPassShortReentryPriorCloseMissing();
void countRenderPassShortReentryClearOpenTarget(
    bool naturalCross,
    std::uint64_t priorStoreBytes,
    std::uint64_t currentLoadBytes);
void countRenderPassTransitionRtChangeSameDepth();
void countRenderPassTransitionSameRtDepthChange();
void countRenderPassTransitionRtDepthChange();
void countRenderPassColorStoreProof(RenderPassColorStoreProof proof);
void countRenderPassDepthStoreProof(RenderPassDepthStoreProof proof);
void countRenderPassNoLookaheadCause(RenderPassNoLookaheadCause cause);
void countRenderPassLateStoreUnknown(RenderPassLateStoreAspect aspect);
void countRenderPassLateStoreResolution(
    RenderPassLateStoreAspect aspect,
    RenderPassLateStoreResolutionCause cause);
void countCommandBufferCreateCpuTime(std::uint64_t nanoseconds);
void countCommandBufferCommitCpuTime(std::uint64_t nanoseconds);
// R-VERIF / command-chunk boundary B2 — wall-clock latency of one
// commit_chunk()
// round trip (PE -> unix import/replay/queue submit -> return). Sampled
// at dxmt9c_device_commit_chunk in device_c_chunk_replay.cpp. This excludes
// asynchronous encode and GPU work after the call returns, but includes
// importer validation, handle/resource marking, record replay, and queued
// draw submission construction.
void countBridgeCommitLatencyNs(std::uint64_t nanoseconds);
// Commit-replay offload path (DXMT9_OFFLOAD_COMMIT_REPLAY): CPU cost of the
// deferred replayRawChunk() call on the ReplayOffloadWorker thread. See
// device_c_replay_offload.{hpp,cpp} and the commit-chunk offload branch in
// device_c_chunk_replay.cpp.
void countOffloadReplayCpuTime(std::uint64_t nanoseconds);
// ReplayOffloadQueue::depth() sampled just before each push(), mirroring
// countEncodeDequeueReadyDepth's "depth before pop" convention.
void countOffloadReplayQueueDepth(std::uint64_t depthBeforePush);
// Drain-fence prologue (drainDeferredReplay in device_c_replay_offload.cpp):
// every direct (non-commit_chunk) dxmt9c_device_* bridge call fences on a
// non-empty ReplayOffloadQueue before issuing its own PE-side effect, so app
// reads/writes observe offload-replayed state in program order. Counted only
// on the non-empty-queue path (queue().depth() == 0 is a plain return with
// no counter touch), mirroring countPresentBoundaryDeferredWait's
// count-only-when-actually-waited convention.
void countOffloadDrainFenceWait();
void countOffloadDrainFenceCpuTime(std::uint64_t nanoseconds);
// Offload backpressure attribution (count-only-when-actually-waited for the
// push/idle pair; commit-app is the app thread's full offload-branch wall).
void countOffloadCommitAppCpuTime(std::uint64_t nanoseconds);
// Heavy opt-in phase split of that same wall (DXMT9_PERF_COMMIT_CHUNK_PHASE_SPLIT).
// presentWait is broken out because the parent timer includes a blocking wait;
// leaving it lumped overstates the CPU terms.
void countCommitChunkPhaseCall();
void countCommitChunkPhasePrepareCpuTime(std::uint64_t nanoseconds);
void countCommitChunkPhaseImportCpuTime(std::uint64_t nanoseconds);
void countCommitChunkPhaseMarkCpuTime(std::uint64_t nanoseconds);
// Of that mark phase, how much was spent acquiring the CommandQueue mutex.
// Wired into both the legacy markChunkResources path and the default
// markChunkResourcesAndCaptureBufferBindings path (2026-08-20; the latter was
// previously unwired and always reported 0 for this counter).
void countCommitChunkPhaseMarkLockCpuTime(std::uint64_t nanoseconds);
// Sub-phase split of commit_chunk_phase_mark_cpu_ms itself, attributing the
// mark phase's ~56.5us/call (GT2, 62% of the sync half) to its three PE-side
// owners: the resolved-handle dedup loop, the upperDevice mark/capture call
// (which subsumes mark_lock's acquire wait), and the buffer-snapshot sort.
// dedup + core + sort do not sum exactly to the parent commit_chunk_phase_mark
// timer because the parent also covers persistResolvedResourcesAndCaptureBindings's
// own bookkeeping around them (raw.resourceEntries assignment, branch/return
// plumbing) — small relative to the three, but real.
void countCommitChunkPhaseMarkDedupCpuTime(std::uint64_t nanoseconds);
void countCommitChunkPhaseMarkCoreCpuTime(std::uint64_t nanoseconds);
void countCommitChunkPhaseMarkSortCpuTime(std::uint64_t nanoseconds);
// Per-call denominators so dedup/core/sort cost can be divided into a
// per-handle / per-buffer figure. Same env gate as the phase timers above.
void countCommitChunkPhaseMarkHandles(std::uint64_t handleCount);
void countCommitChunkPhaseMarkBuffers(std::uint64_t bufferCount);
// Opt-in bounded overlap ledger for the ingress handle walk versus the final
// slot publish scan. The normal path only evaluates its cached enable flag at
// the caller; no identity or storage is created while disabled.
void countResourceMarkOverlapIngress(std::uint64_t entries = 1);
void countResourceMarkOverlapPublish(std::uint64_t entries = 1);
void countResourceMarkOverlapUnique();
void countResourceMarkOverlapIngressDuplicate();
void countResourceMarkOverlapPublishDuplicate();
void countResourceMarkOverlapCovered();
void countResourceMarkOverlapStale();
void countResourceMarkOverlapNoIngress();
void countResourceMarkOverlapCollisionProbes(std::uint64_t probes);
void countResourceMarkOverlapOverflow();
void countCommitChunkPhaseEnqueueCpuTime(std::uint64_t nanoseconds);
void countCommitChunkPhasePresentWaitTime(std::uint64_t nanoseconds);
// R-BACK-43.6 — frozen-ticket re-stamp observability. Called once per
// `restampIfTicketAdvancedLocked` with whether the ticket had moved, so
// `mark_ticket_restamp_fires / mark_ticket_restamp_checks` measures how often
// a concurrent publish really lands inside a lock-free mark window. Always on:
// two gated atomic adds on a path that already holds `CommandQueue::mutex_`.
void countMarkTicketRestamp(bool restamped);
void countOffloadPushBackpressureWait();
void countOffloadPushBackpressureWaitNs(std::uint64_t nanoseconds);
void countOffloadWorkerIdleWaitNs(std::uint64_t nanoseconds);
// R-BACK-44.3 — one Managed buffer mutation task applied on the offload worker
// at its reserved FIFO position. `copyForwardBytes` is the untouched region
// copied from the pool CPU shadow into the leased backing; `patchBytes` is the
// staged dirty span written into both the leased backing and the shadow.
void countOffloadBufferMutationApplied(std::uint64_t nanoseconds,
                                       std::uint64_t copyForwardBytes,
                                       std::uint64_t patchBytes);
// R-BACK-44.7 — a queued mutation task released without application by a
// teardown/fail-stop drain. Its draws are discarded on the same paths.
void countOffloadBufferMutationDiscarded();
void countCompletionEnqueue(std::uint64_t pendingDepthAfterPush,
                            bool whileWaiting,
                            bool hasPresent);
void countCompletionDequeue(std::uint64_t ageNanoseconds,
                            std::uint64_t pendingDepthAfterPop,
                            std::uint64_t commandBufferStatus);
void countCompletionWaitStatus(std::uint64_t nanoseconds,
                               std::uint64_t commandBufferStatus);
void countCompletionWaitOverlap(std::uint64_t nanoseconds,
                                std::uint64_t enqueuesDuringWait,
                                bool hasPresent);
void countCompletionSignalDelay(std::uint64_t nanoseconds);
void countCompletionWaitCommitChunkEntry();
void countCompletionWaitCommitChunkReplayStart();
void countCompletionWaitCommitChunkReplayEnd(std::uint64_t replayNanoseconds);
void countCompletionWaitCommitPublish();
void countCompletionWaitEncodeDequeue();
void countCompletionWaitCommandBufferCommit();
void countCompletionWaitStagePublishToEncodeDequeue(std::uint64_t nanoseconds);
void countCompletionWaitStageEncodeDequeueToCommandBufferCommit(
    std::uint64_t nanoseconds);
void countCompletionNoEnqueueWaitToCommitChunkEntry(std::uint64_t nanoseconds);
void countCompletionNoEnqueueWaitToCommitChunkReplayStart(std::uint64_t nanoseconds);
void countCompletionNoEnqueueWaitToCommitChunkReplayEnd(std::uint64_t nanoseconds);
void countCompletionNoEnqueueWaitToCommitPublish(std::uint64_t nanoseconds);
void countCompletionNoEnqueueWaitToEncodeDequeue(std::uint64_t nanoseconds);
void countCompletionNoEnqueueWaitToCommandBufferCommit(std::uint64_t nanoseconds);
void countEncodeDequeueReadyDepth(std::uint64_t readyDepthBeforePop);
void countCompletionNoEnqueueCommitChunksBeforePublish(std::uint64_t entries,
                                                       std::uint64_t replayStarts,
                                                       std::uint64_t replayEnds);
void countCompletionNoEnqueueCommitChunkCompletedReplayCpuBeforePublish(
    std::uint64_t nanoseconds);
void countCompletionNoEnqueueCommitChunkActiveReplayCpuBeforePublish(
    std::uint64_t nanoseconds);
void countCompletionNoEnqueueCommitChunkInterReplayGapBeforePublish(
    std::uint64_t nanoseconds);
void countCompletionNoEnqueueCommitPublishWaitBeforePublish(
    std::uint64_t nanoseconds);
void countCompletionNoEnqueueCommitPublishOnBeforePublishCpu(
    std::uint64_t nanoseconds);
void countCompletionNoEnqueueCommitChunkRecordShapeBeforePublish(
    std::uint64_t recordCount,
    std::uint64_t drawRecords,
    std::uint64_t constRecords,
    std::uint64_t applyStateRecords,
    std::uint64_t clearRecords,
    std::uint64_t presentRecords,
    std::uint64_t surfaceRecords,
    std::uint64_t queryRecords,
    std::uint64_t otherRecords);
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
    std::uint64_t presentNonTailSlots);
void countCompletionNoEnqueueStageCommitEntryToPublish(std::uint64_t nanoseconds);
void countCompletionNoEnqueueStagePublishToEncodeDequeue(std::uint64_t nanoseconds);
void countCompletionNoEnqueueStageEncodeDequeueToCommandBufferCommit(std::uint64_t nanoseconds);
void countCompletionNoEnqueueWaitToNextEnqueue(std::uint64_t nanoseconds,
                                               bool hasPresent);
void countCompletionWait(std::uint64_t nanoseconds,
                         bool hasDraw,
                         bool hasPresent,
                         bool hasBlit,
                         bool hasStretchRect,
                         std::uint32_t compatFlags,
                         std::uint64_t vertexShaderHash,
                         std::uint64_t pixelShaderHash,
                         std::uint64_t shaderVariantHash);
void countSyncWait(std::uint64_t nanoseconds);
void countQueueWriterWait(std::uint64_t nanoseconds);
void countQueueCommitWait(std::uint64_t nanoseconds);
void countQueueSequenceWait(std::uint64_t nanoseconds);
void recordCpuReadyTapeStats(std::uint64_t residentSources,
                             std::uint64_t residentPages,
                             std::uint64_t readyEntries,
                             std::uint64_t admissionCloses,
                             std::uint64_t admissionReopens,
                             std::uint64_t wrapPaddingPages);
void countCpuReadyTapeAdmissionWait(std::uint64_t nanoseconds);
void countCpuReadyTapeLegacyOversizeBypass();
void countCpuReadyTapeReclaimWakeup();

// Source-qualified supply timing for the CPU-ready tape.  These are
// observation-only counters: the queue pairs a bounded SourceRef/control
// identity and reports a sample only after both edges are present.  Arena
// publication is intentionally exposed as an API so its owner can use the
// same ledger without adding timing fields to the source payload.
enum class CpuReadySupplyClass : std::uint8_t {
  Legacy,
  Arena,
};

enum class CpuReadySupplyStage : std::uint8_t {
  ReplayEntryToPublish,
  PublishToEncodeDequeue,
};

void recordCpuReadySupplyLatency(CpuReadySupplyClass sourceClass,
                                 CpuReadySupplyStage stage,
                                 std::uint64_t nanoseconds);
void countCpuReadySupplyAttributionMiss(CpuReadySupplyClass sourceClass,
                                        CpuReadySupplyStage stage);
void countCpuReadySupplyLedgerOverflow();
void countMapBufferWait(std::uint64_t totalNanoseconds,
                        std::uint64_t mutexNanoseconds,
                        std::uint64_t sequenceNanoseconds,
                        std::uint32_t flags,
                        bool waited);
// R-BACK-5.11 — MANAGED buffer CPU-shadow upload/backing-version outcomes.
void countManagedBufferUpload(std::uint64_t bytes);
void countManagedBufferBackingInPlace();
void countManagedBufferBackingReuse();
void countManagedBufferBackingFresh();
void countPresentBoundaryApplied();
void countPresentBoundarySkipped();
void countPresentBoundaryDeferred();
void countPresentBoundaryDeferredWait();
void countPresentBoundaryWait(std::uint64_t nanoseconds);
// Commit-replay offload present-ordinal boundary (TLA+: PresentFrameLatency
// ordinal variant). Split into two calls (unlike countPresentBoundaryWait)
// to match CommandQueue::waitPresentOrdinalBoundary's pre-wait/post-wait
// call sites.
void countPresentOrdinalBoundaryWait();
void countPresentOrdinalBoundaryWaitNs(std::uint64_t nanoseconds);
// Incremented once per present retired in drainCompletedSequence, alongside
// presentCompletedSeqId — the ordinal counterpart consumed by
// waitPresentOrdinalBoundary.
void countCompletedPresentOrdinal();
void countPresentEncoded();
void countPresentSkipped();
void countPresentFullscreen();
void countPresentSourceSelection(bool explicitSource, bool isCurrentBackBuffer);
void countPresentSourceResolved(bool hasSurface,
                                bool hasTexture,
                                bool hasResolveTexture,
                                bool invalidSize,
                                std::uint32_t width,
                                std::uint32_t height,
                                std::uint32_t format,
                                std::uint32_t sampleCount,
                                std::uint64_t sourceHandle,
                                std::uint64_t textureHandle);
void countPresentPass(std::uint32_t sourceWidth,
                      std::uint32_t sourceHeight,
                      std::uint64_t targetWidth,
                      std::uint64_t targetHeight);
void countPresentSchedule(bool requestedDisplaySync, double minimumPresentDuration);
void countPresentAcquireWait(std::uint64_t nanoseconds);
void countPresentAsyncAcquireRequest();
void countPresentAsyncAcquireIssued();
void countPresentAsyncAcquireFallback();
void countPresentAsyncAcquireWait(std::uint64_t nanoseconds);
void countPresentTokenWait(std::uint64_t nanoseconds);
void countPresentPreAcquireRequest();
void countPresentPreAcquireHit();
void countPresentPreAcquireMiss();
void countPresentPreAcquireWait(std::uint64_t nanoseconds);
void countPresentSetPropsWait(std::uint64_t nanoseconds);

// Per-frame snapshot mode (opt-in via DXMT9_PERF_FRAME_SAMPLING=1).
//
// Default off → callers pay only one bool check at the trigger point.
// When enabled, the encode-thread Present handler takes a snapshot at
// each Present and emits a `[dxmt9-perf-frame frame=N ...]` line whose
// values are deltas vs the previous snapshot. The focused subset
// (~30 keys: timing nanoseconds + major volume counters) keeps the
// per-frame line length bounded; the cumulative `[dxmt9-perf]` report
// at exit still covers everything.
//
// CounterSnapshot is a POD copy of the subset, suitable for
// `static thread_local` storage at the trigger site.
struct CounterSnapshot {
  std::uint64_t submitDraw = 0;
  std::uint64_t submitClear = 0;
  std::uint64_t submitStretch = 0;
  std::uint64_t submitPresent = 0;
  std::uint64_t submitFlush = 0;
  std::uint64_t commandBuffers = 0;
  std::uint64_t renderPassBegin = 0;
  std::uint64_t renderPassEnd = 0;
  std::uint64_t drawCalls = 0;
  std::uint64_t drawIndexedCalls = 0;
  std::uint64_t drawPrimitiveCount = 0;
  std::uint64_t drawTriangleEstimate = 0;
  std::uint64_t drawVertexCount = 0;
  std::uint64_t bindPipeline = 0;
  std::uint64_t presentEncoded = 0;
  std::uint64_t submitDrawCpuNs = 0;
  std::uint64_t encodeChunkCalls = 0;
  std::uint64_t encodeChunkCpuNs = 0;
  std::uint64_t encodeDrawCpuNs = 0;
  std::uint64_t encodeDrawPipelineLookupCpuNs = 0;
  std::uint64_t encodeDrawUniformBuildCpuNs = 0;
  std::uint64_t encodeDrawFvfDecodeCpuNs = 0;
  std::uint64_t encodeDrawBindingPacketCpuNs = 0;
  std::uint64_t encodeDrawBindingPacketPlanCpuNs = 0;
  std::uint64_t encodeDrawBindingPacketPlanFragmentCpuNs = 0;
  std::uint64_t encodeDrawBindingPacketPlanVertexCpuNs = 0;
  std::uint64_t encodeDrawBindingPacketPlanExtraStreamCpuNs = 0;
  std::uint64_t encodeDrawBindingPacketPlanRasterCpuNs = 0;
  std::uint64_t encodeDrawBindingPacketCacheCpuNs = 0;
  std::uint64_t encodeDrawBindingPacketTextureRecordCpuNs = 0;
  std::uint64_t encodeDrawArgbufSetupCpuNs = 0;
  std::uint64_t encodeDrawArgbufOpenCpuNs = 0;
  std::uint64_t encodeDrawArgbufCbufUpdateCpuNs = 0;
  std::uint64_t encodeDrawStreamBindCpuNs = 0;
  std::uint64_t encodeDrawIssueCpuNs = 0;
  // Sequential partition of encodeDraw (.13). The PerfScope children time the
  // regions they wrap and leave the branches between them uncounted -- 34% of
  // encode_draw had no counter on it. These marks partition the whole call, so
  // every nanosecond between entry and return lands in exactly one phase.
  // Per-call-site split of the encode_draw children that appear more than once.
  // Without these the named/unnamed split per phase cannot be computed at all:
  // stream_bind has five sites and fvf_decode three, spread across phases, so
  // the aggregate cannot be assigned to any one of them (.14).
  std::uint64_t encodeDrawStreamBindViewportCpuNs = 0;
  std::uint64_t encodeDrawStreamBindFfpCpuNs = 0;
  std::uint64_t encodeDrawStreamBindVsCpuNs = 0;
  std::uint64_t encodeDrawStreamBindTextureCpuNs = 0;
  std::uint64_t encodeDrawStreamBindIndexCpuNs = 0;
  std::uint64_t encodeDrawFvfDecodeDeclCpuNs = 0;
  std::uint64_t encodeDrawFvfDecodeBytesCpuNs = 0;
  std::uint64_t encodeDrawFvfDecodeExpandedCpuNs = 0;
  std::uint64_t encodeDrawUniformBuildMainCpuNs = 0;
  std::uint64_t encodeDrawUniformBuildFfpCpuNs = 0;
  std::uint64_t encodeDrawUniformBuildVsCpuNs = 0;
  std::uint64_t encodeDrawPhaseSetupCpuNs = 0;
  std::uint64_t encodeDrawPhaseArgbufUniformCpuNs = 0;
  std::uint64_t encodeDrawPhaseStreamPrepCpuNs = 0;
  std::uint64_t encodeDrawPhaseFfpVertexCpuNs = 0;
  std::uint64_t encodeDrawPhaseVertexBindCpuNs = 0;
  std::uint64_t encodeDrawPhaseBaseStateCpuNs = 0;
  std::uint64_t encodeDrawPhaseTileFfpFallthroughCpuNs = 0;
  std::uint64_t encodeDrawPhaseRemainderCpuNs = 0;
  std::uint64_t transientUploadCpuNs = 0;
  std::uint64_t commandBufferCreateCpuNs = 0;
  std::uint64_t commandBufferCommitCpuNs = 0;
  std::uint64_t completionWaitNs = 0;
  std::uint64_t presentAcquireWaitNs = 0;
  std::uint64_t presentBoundaryWaitNs = 0;
  std::uint64_t presentTokenWaitNs = 0;
  // R-BACK-2.29: per-frame mid-chunk commit count so the policy A/B
  // (DXMT9_MID_CHUNK_COMMIT_POLICY) can be observed frame-by-frame.
  std::uint64_t subCommandBufferCommits = 0;
  // M4/M5 surface in per-frame line so encode_chunk_cpu vs GPU wall time
  // can be compared frame-by-frame, and a GPU fault that erupts mid-run is
  // visible without grepping the cumulative line at exit.
  std::uint64_t gpuCommandBufferTimeNs = 0;
  std::uint64_t gpuCommandBufferTimeSamples = 0;
  std::uint64_t renderEncoderGpuTimeNs = 0;
  std::uint64_t renderEncoderGpuTimeSamples = 0;
  std::uint64_t renderEncoderGpuLastPassType = 0;
  std::uint64_t renderEncoderGpuLastRt = 0;
  std::uint64_t renderEncoderGpuLastDepth = 0;
  std::uint64_t renderEncoderGpuLastPso = 0;
  std::uint64_t gpuCommandBufferErrors = 0;
};

bool frameSamplingEnabled();
CounterSnapshot snapshot();
void emitFrameDelta(std::uint64_t frameId,
                    const CounterSnapshot& prev,
                    const CounterSnapshot& curr);

inline constexpr std::size_t kEncoderBreakdownMaxStreams = 16;

struct EncoderStreamBreakdown {
  bool valid = false;
  std::uint64_t samples = 0;
  std::uint64_t metalBinds = 0;
  std::uint64_t metalBindFirsts = 0;
  std::uint64_t metalBindHandleChanges = 0;
  std::uint64_t metalBindOffsetChanges = 0;
  std::uint64_t uniqueHandles = 0;
  std::uint64_t uniqueHandleOverflows = 0;
  std::uint64_t uniqueBytes = 0;
  std::uint64_t uniqueDynamicHandles = 0;
  std::uint64_t uniqueWriteOnlyHandles = 0;
  std::uint64_t uniqueDefaultPoolHandles = 0;
  std::uint64_t uniqueManagedPoolHandles = 0;
  std::uint64_t uniqueSystemMemPoolHandles = 0;
  std::uint64_t uniqueScratchPoolHandles = 0;
  std::uint64_t handleChanges = 0;
  std::uint64_t offsetChanges = 0;
  std::uint64_t strideChanges = 0;
  std::uint64_t lastHandle = 0;
  std::uint64_t lastOffset = 0;
  std::uint64_t lastStride = 0;
};

// Per-render-encoder write/state breakdown (opt-in via
// DXMT9_PERF_ENCODER_BREAKDOWN=1). This emits one
// `[dxmt9-perf-encoder ...]` summary line plus one
// `[dxmt9-perf-encoder-stream ...]` line for each used stream when a render
// encoder closes. The data is intentionally not part of the cumulative counter
// table: it is high-cardinality diagnostic output meant to be joined with Xcode
// encoder labels/counters.
struct EncoderBreakdown {
  std::uint64_t seqId = 0;
  std::uint64_t encoderIndex = 0;
  std::uint64_t rtHandle = 0;
  std::uint64_t depthHandle = 0;
  std::uint64_t rtFormat = 0;
  std::uint64_t rtWidth = 0;
  std::uint64_t rtHeight = 0;
  std::uint64_t rtBytesPerPixel = 0;
  std::uint64_t rtAliasTexture = 0;
  std::uint64_t rtTextureUsage = 0;
  std::uint64_t rtFormatNeedsShaderReadSwizzle = 0;
  std::uint64_t rtTextureNeedsShaderReadView = 0;
  std::uint64_t depthFormat = 0;
  std::uint64_t depthWidth = 0;
  std::uint64_t depthHeight = 0;
  std::uint64_t depthBytesPerPixel = 0;
  std::uint64_t depthAliasTexture = 0;
  std::uint64_t depthTextureUsage = 0;
  std::uint64_t depthFormatNeedsShaderReadSwizzle = 0;
  std::uint64_t depthTextureNeedsShaderReadView = 0;
  std::uint64_t colorAttachmentCount = 0;
  std::uint64_t color0Included = 0;
  std::uint64_t color0LoadAction = 0;
  std::uint64_t color0StoreAction = 0;
  std::uint64_t color0Clear = 0;
  std::uint64_t colorLoadBytes = 0;
  std::uint64_t colorStoreBytes = 0;
  std::uint64_t depthIncluded = 0;
  std::uint64_t depthLoadAction = 0;
  std::uint64_t depthStoreAction = 0;
  std::uint64_t depthClear = 0;
  std::uint64_t depthLoadBytes = 0;
  std::uint64_t depthStoreBytes = 0;
  std::uint64_t stencilIncluded = 0;
  std::uint64_t stencilLoadAction = 0;
  std::uint64_t stencilStoreAction = 0;
  std::uint64_t stencilClear = 0;
  std::uint64_t stencilLoadBytes = 0;
  std::uint64_t stencilStoreBytes = 0;
  EncoderSplitReason endReason = EncoderSplitReason::Final;
  std::uint64_t drawCalls = 0;
  std::uint64_t indexedDraws = 0;
  std::uint64_t expandedIndexedDraws = 0;
  std::uint64_t ffpDraws = 0;
  std::uint64_t programmableDraws = 0;
  std::uint64_t preTransformedDraws = 0;
  std::uint64_t texturedDraws = 0;
  std::uint64_t cullNoneDraws = 0;
  std::uint64_t cullFrontDraws = 0;
  std::uint64_t cullBackDraws = 0;
  std::uint64_t fillSolidDraws = 0;
  std::uint64_t fillWireframeDraws = 0;
  std::uint64_t depthEnabledDraws = 0;
  std::uint64_t depthWriteDraws = 0;
  std::uint64_t depthFuncLessDraws = 0;
  std::uint64_t depthFuncLessEqualDraws = 0;
  std::uint64_t depthFuncAlwaysDraws = 0;
  std::uint64_t depthFuncOtherDraws = 0;
  std::uint64_t scissorEnabledDraws = 0;
  std::uint64_t alphaBlendEnabledDraws = 0;
  std::uint64_t blendStateSamples = 0;
  std::uint64_t blendStateChanges = 0;
  std::uint64_t blendStateUnique = 0;
  std::uint64_t blendStateUniqueOverflows = 0;
  std::uint64_t blendStateLast = 0;
  std::uint64_t blendEnabledNoopDraws = 0;
  std::uint64_t blendConstantFactorDraws = 0;
  std::uint64_t blendScreenDraws = 0;
  std::uint64_t blendAdditiveDraws = 0;
  std::uint64_t blendAlphaCompositeDraws = 0;
  std::uint64_t alphaBlendTexturedDraws = 0;
  std::uint64_t alphaBlendTexturedPrimitives = 0;
  std::uint64_t alphaBlendTexturedVertices = 0;
  std::uint64_t alphaBlendSmallDraws = 0;
  std::uint64_t alphaBlendSmallPrimitives = 0;
  std::uint64_t alphaBlendSmallVertices = 0;
  std::uint64_t alphaTestEnabledDraws = 0;
  std::uint64_t alphaTestEffectiveDraws = 0;
  std::uint64_t clipPlaneEnabledDraws = 0;
  std::uint64_t pointDraws = 0;
  std::uint64_t lineDraws = 0;
  std::uint64_t triangleDraws = 0;
  std::uint64_t primitiveCount = 0;
  std::uint64_t triangleEstimate = 0;
  std::uint64_t vertexCount = 0;
  std::uint64_t routeDepthOnlyDraws = 0;
  std::uint64_t routeDepthOnlyPrimitives = 0;
  std::uint64_t routeDepthOnlyVertices = 0;
  std::uint64_t routeProgrammableTexturedDraws = 0;
  std::uint64_t routeProgrammableTexturedPrimitives = 0;
  std::uint64_t routeProgrammableTexturedVertices = 0;
  std::uint64_t routeProgrammableColorDraws = 0;
  std::uint64_t routeProgrammableColorPrimitives = 0;
  std::uint64_t routeProgrammableColorVertices = 0;
  std::uint64_t routeAlphaBlendPrimitives = 0;
  std::uint64_t routeAlphaTestPrimitives = 0;
  std::uint64_t tileFfpRoutedTileDraws = 0;
  std::uint64_t tileFfpRoutedTilePrimitives = 0;
  std::uint64_t tileFfpRoutedTileVertices = 0;
  std::uint64_t tileFfpRoutedPortableDraws = 0;
  std::uint64_t tileFfpRoutedPortablePrimitives = 0;
  std::uint64_t tileFfpRoutedPortableVertices = 0;
  std::uint64_t tileFfpEligibleDraws = 0;
  std::uint64_t tileFfpEligiblePrimitives = 0;
  std::uint64_t tileFfpEligibleVertices = 0;
  std::uint64_t tileFfpFallbackGpuFamilyDraws = 0;
  std::uint64_t tileFfpFallbackGpuFamilyPrimitives = 0;
  std::uint64_t tileFfpFallbackNotFfpDraws = 0;
  std::uint64_t tileFfpFallbackNotFfpPrimitives = 0;
  std::uint64_t tileFfpFallbackPrecisionDraws = 0;
  std::uint64_t tileFfpFallbackPrecisionPrimitives = 0;
  std::uint64_t tileFfpFallbackUnsupportedStateDraws = 0;
  std::uint64_t tileFfpFallbackUnsupportedStatePrimitives = 0;
  std::uint64_t indexedTriangleOpaqueDepthWriteDraws = 0;
  std::uint64_t indexedTriangleOpaqueDepthWritePrimitives = 0;
  std::uint64_t indexedTriangleOpaqueDepthWriteVertices = 0;
  std::uint64_t indexedTriangleDepthReadDraws = 0;
  std::uint64_t indexedTriangleDepthReadPrimitives = 0;
  std::uint64_t indexedTriangleDepthReadVertices = 0;
  std::uint64_t indexedTriangleAlphaBlendDraws = 0;
  std::uint64_t indexedTriangleAlphaBlendPrimitives = 0;
  std::uint64_t indexedTriangleAlphaBlendVertices = 0;
  std::uint64_t indexedTriangleScissorDraws = 0;
  std::uint64_t indexedTriangleScissorPrimitives = 0;
  std::uint64_t indexedTriangleScissorVertices = 0;
  std::uint64_t indexedTriangleTexturedDraws = 0;
  std::uint64_t indexedTriangleTexturedPrimitives = 0;
  std::uint64_t indexedTriangleTexturedVertices = 0;
  std::uint64_t indexedTriangleLarge4096Draws = 0;
  std::uint64_t indexedTriangleLarge4096Primitives = 0;
  std::uint64_t indexedTriangleLarge4096Vertices = 0;
  std::uint64_t indexedTriangleLarge4096OpaqueDepthWriteDraws = 0;
  std::uint64_t indexedTriangleLarge4096OpaqueDepthWritePrimitives = 0;
  std::uint64_t indexedTriangleLarge4096OpaqueDepthWriteVertices = 0;
  std::uint64_t indexedTriangleLarge4096DepthReadDraws = 0;
  std::uint64_t indexedTriangleLarge4096DepthReadPrimitives = 0;
  std::uint64_t indexedTriangleLarge4096DepthReadVertices = 0;
  std::uint64_t indexedTriangleLarge4096AlphaBlendDraws = 0;
  std::uint64_t indexedTriangleLarge4096AlphaBlendPrimitives = 0;
  std::uint64_t indexedTriangleLarge4096AlphaBlendVertices = 0;
  std::uint64_t indexedTriangleLarge4096ScissorDraws = 0;
  std::uint64_t indexedTriangleLarge4096ScissorPrimitives = 0;
  std::uint64_t indexedTriangleLarge4096ScissorVertices = 0;
  std::uint64_t indexedTriangleLarge4096TexturedDraws = 0;
  std::uint64_t indexedTriangleLarge4096TexturedPrimitives = 0;
  std::uint64_t indexedTriangleLarge4096TexturedVertices = 0;
  std::uint64_t drawPrimitiveCountMin = 0;
  std::uint64_t drawPrimitiveCountMax = 0;
  std::uint64_t drawVertexCountMin = 0;
  std::uint64_t drawVertexCountMax = 0;
  std::uint64_t drawPrimitiveBucket1_63 = 0;
  std::uint64_t drawPrimitiveBucket64_255 = 0;
  std::uint64_t drawPrimitiveBucket256_1023 = 0;
  std::uint64_t drawPrimitiveBucket1024_4095 = 0;
  std::uint64_t drawPrimitiveBucket4096Plus = 0;
  std::uint64_t drawVertexBucket1_255 = 0;
  std::uint64_t drawVertexBucket256_1023 = 0;
  std::uint64_t drawVertexBucket1024_4095 = 0;
  std::uint64_t drawVertexBucket4096_16383 = 0;
  std::uint64_t drawVertexBucket16384Plus = 0;
  std::uint64_t textureMaskOr = 0;
  std::uint64_t fragmentTextureBindingSamples = 0;
  std::uint64_t fragmentTextureBindingMaskOr = 0;
  std::uint64_t x8RtTextureBindingSamples = 0;
  std::uint64_t x8RtTextureBindingMaskOr = 0;
  std::uint64_t x8RtTextureBindingUniqueHandles = 0;
  std::uint64_t x8RtTextureBindingUniqueHandleOverflows = 0;
  std::uint64_t x8RtTextureBindingShaderReadViewSamples = 0;
  std::uint64_t x8RtTextureBindingActiveRtAliasSamples = 0;
  std::uint64_t x8ShaderAlphaFillSamples = 0;
  std::uint64_t x8ShaderAlphaFillMaskOr = 0;
  std::uint64_t x8RtTextureBindingLastStage = 0;
  std::uint64_t x8RtTextureBindingLastHandle = 0;
  std::uint64_t drawGeometrySignatureSamples = 0;
  std::uint64_t drawGeometrySignatureUnique = 0;
  std::uint64_t drawGeometrySignatureUniqueOverflows = 0;
  std::uint64_t drawGeometrySignatureDuplicates = 0;
  std::uint64_t drawGeometrySignatureConsecutiveDuplicates = 0;
  std::uint64_t drawGeometrySignatureLast = 0;
  std::uint64_t indexedBaseVertexSamples = 0;
  std::uint64_t indexedBaseVertexNonZeroDraws = 0;
  std::uint64_t indexedBaseVertexNegativeDraws = 0;
  std::uint64_t indexedBaseVertexPositiveDraws = 0;
  std::int64_t indexedBaseVertexMin = 0;
  std::int64_t indexedBaseVertexMax = 0;
  std::uint64_t nativeBaseVertexRequestedDraws = 0;
  std::uint64_t nativeBaseVertexUsedDraws = 0;
  std::uint64_t nativeBaseVertexSkippedNegativeDraws = 0;
  std::uint64_t splitLargeIndexedSourceDraws = 0;
  std::uint64_t splitLargeIndexedMetalDraws = 0;
  std::uint64_t splitLargeIndexedExtraDraws = 0;
  std::uint64_t splitLargeIndexedPrimitiveLimit = 0;
  std::uint64_t splitLargeIndexedStream0SpanLimit = 0;
  std::uint64_t splitLargeIndexedChunkStream0SpanMax = 0;
  std::uint64_t splitLargeIndexedPrimitiveCount = 0;
  std::uint64_t indexedOrderProbeDraws = 0;
  std::uint64_t indexedOrderProbeSkipped = 0;
  std::uint64_t indexedOrderProbeBytes = 0;
  std::uint64_t indexedOrderOptimizedDraws = 0;
  std::uint64_t indexedOrderOptimizedSkipped = 0;
  std::uint64_t indexedOrderOptimizedBytes = 0;
  std::uint64_t probeScissorRectDraws = 0;
  std::uint64_t probeScissorRectSkipped = 0;
  std::uint64_t probeScissorRectAreaDeltaPixels = 0;
  std::uint64_t probeDisableAlphaBlendDraws = 0;
  std::uint64_t probeDisableDepthWriteDraws = 0;
  std::uint64_t probeDepthFuncAlwaysDraws = 0;
  std::uint64_t probeForceTextureWhiteDraws = 0;
  std::uint64_t probeFragmentlessDepthOnlyDraws = 0;
  std::uint64_t probeFragmentlessDepthOnlyPrimitives = 0;
  std::uint64_t probeFragmentlessDepthOnlyVertices = 0;
  std::uint64_t indexedVertexReuseSamples = 0;
  std::uint64_t indexedVertexReuseSkipped = 0;
  std::uint64_t indexedVertexReferenceCount = 0;
  std::uint64_t indexedUniqueVertexEstimate = 0;
  std::uint64_t indexedVertexCacheMissEstimate16 = 0;
  std::uint64_t indexedVertexCacheMissEstimate32 = 0;
  std::uint64_t indexedVertexCacheMissEstimate64 = 0;
  std::uint64_t indexedCacheOptCandidateDraws = 0;
  std::uint64_t indexedCacheOptCandidateSkipped = 0;
  std::uint64_t indexedCacheOptCandidateBytes = 0;
  std::uint64_t indexedCacheOptCandidateOriginalMiss16 = 0;
  std::uint64_t indexedCacheOptCandidateOriginalMiss32 = 0;
  std::uint64_t indexedCacheOptCandidateOriginalMiss64 = 0;
  std::uint64_t indexedCacheOptCandidateMiss16 = 0;
  std::uint64_t indexedCacheOptCandidateMiss32 = 0;
  std::uint64_t indexedCacheOptCandidateMiss64 = 0;
  std::uint64_t indexedCacheOptCandidateGatePass = 0;
  std::uint64_t indexedCacheOptCandidateGateFail = 0;
  std::uint64_t indexedCacheOptCandidateOpaqueDepthDraws = 0;
  std::uint64_t indexedCacheOptCandidateScreenBlendDraws = 0;
  std::uint64_t indexedCacheOptCandidatePrimitiveBucket1_63 = 0;
  std::uint64_t indexedCacheOptCandidatePrimitiveBucket64_255 = 0;
  std::uint64_t indexedCacheOptCandidatePrimitiveBucket256_1023 = 0;
  std::uint64_t indexedCacheOptCandidatePrimitiveBucket1024_4095 = 0;
  std::uint64_t indexedCacheOptCandidatePrimitiveBucket4096Plus = 0;
  std::uint64_t reorderedIndexCacheLookups = 0;
  std::uint64_t reorderedIndexCacheHits = 0;
  std::uint64_t reorderedIndexCacheRejectedHits = 0;
  std::uint64_t reorderedIndexCacheMisses = 0;
  std::uint64_t reorderedIndexCacheCreated = 0;
  std::uint64_t reorderedIndexCacheCreatedBytes = 0;
  std::uint64_t stream0StrideMin = 0;
  std::uint64_t stream0StrideMax = 0;
  std::uint64_t streamStateSamples = 0;
  std::uint64_t streamMetalBinds = 0;
  std::uint64_t streamMetalBindFirsts = 0;
  std::uint64_t streamMetalBindHandleChanges = 0;
  std::uint64_t streamMetalBindOffsetChanges = 0;
  std::uint64_t streamUniqueHandles = 0;
  std::uint64_t streamUniqueHandleOverflows = 0;
  std::uint64_t streamUniqueBytes = 0;
  std::uint64_t streamUniqueDynamicHandles = 0;
  std::uint64_t streamUniqueWriteOnlyHandles = 0;
  std::uint64_t streamUniqueDefaultPoolHandles = 0;
  std::uint64_t streamUniqueManagedPoolHandles = 0;
  std::uint64_t streamUniqueSystemMemPoolHandles = 0;
  std::uint64_t streamUniqueScratchPoolHandles = 0;
  std::uint64_t streamHandleChanges = 0;
  std::uint64_t streamOffsetChanges = 0;
  std::uint64_t streamStrideChanges = 0;
  std::uint64_t stream0LastHandle = 0;
  std::uint64_t stream0LastOffset = 0;
  std::uint64_t stream0LastStride = 0;
  std::uint64_t ibStateSamples = 0;
  std::uint64_t ibMetalBinds = 0;
  std::uint64_t ibHandleChanges = 0;
  std::uint64_t ibUniqueHandles = 0;
  std::uint64_t ibUniqueHandleOverflows = 0;
  std::uint64_t ibUniqueBytes = 0;
  std::uint64_t ibUniqueDynamicHandles = 0;
  std::uint64_t ibUniqueWriteOnlyHandles = 0;
  std::uint64_t ibUniqueDefaultPoolHandles = 0;
  std::uint64_t ibUniqueManagedPoolHandles = 0;
  std::uint64_t ibUniqueSystemMemPoolHandles = 0;
  std::uint64_t ibUniqueScratchPoolHandles = 0;
  std::uint64_t ibLastHandle = 0;
  std::uint64_t psoStateSamples = 0;
  std::uint64_t psoHandleChanges = 0;
  std::uint64_t psoUniqueHandles = 0;
  std::uint64_t psoUniqueHandleOverflows = 0;
  std::uint64_t psoLastHandle = 0;
  std::uint64_t shaderVariantChanges = 0;
  std::uint64_t shaderVariantUnique = 0;
  std::uint64_t shaderVariantUniqueOverflows = 0;
  std::uint64_t shaderVariantLast = 0;
  std::uint64_t vertexShaderLast = 0;
  std::uint64_t pixelShaderLast = 0;
  std::uint64_t vertexShaderSourceLast = 0;
  std::uint64_t pixelShaderSourceLast = 0;
  std::uint64_t vsOutLayoutChanges = 0;
  std::uint64_t vsOutLayoutUnique = 0;
  std::uint64_t vsOutLayoutUniqueOverflows = 0;
  std::uint32_t vsOutLayoutLast = 0;
  std::uint64_t vsOutLayoutCacheHits = 0;
  std::uint64_t vsOutLayoutCacheMisses = 0;
  std::uint64_t argbufTableBytes = 0;
  std::uint64_t argbufCbufBytes = 0;
  std::uint64_t argbufCbufVsBytes = 0;
  std::uint64_t argbufCbufFfpVsBytes = 0;
  std::uint64_t argbufCbufPsBytes = 0;
  std::uint64_t argbufCbufFfpPsBytes = 0;
  std::uint64_t argbufCbufVsFirstBytes = 0;
  std::uint64_t argbufCbufVsRewriteChangedBytes = 0;
  std::uint64_t argbufCbufVsRewriteUnchangedBytes = 0;
  std::uint64_t argbufCbufVsFloatChangedBytes = 0;
  std::uint64_t argbufCbufVsIntChangedBytes = 0;
  std::uint64_t argbufCbufVsBoolChangedBytes = 0;
  std::uint64_t argbufCbufVsUploads = 0;
  std::uint64_t argbufCbufVsFullStructUploads = 0;
  std::uint64_t argbufCbufVsUsageUnknownUploads = 0;
  std::uint64_t argbufCbufVsUsageIndexedFloatUploads = 0;
  std::uint64_t argbufCbufVsPlanFloatRegsSum = 0;
  std::uint64_t argbufCbufVsPlanFloatRegsMax = 0;
  std::uint64_t argbufCbufVsDirtyFloatRegsSum = 0;
  std::uint64_t argbufCbufVsDirtyFloatRegsMax = 0;
  std::uint64_t argbufCbufVsUsageFloatRegsSum = 0;
  std::uint64_t argbufCbufVsUsageFloatRegsMax = 0;
  std::uint64_t argbufCbufFfpVsFirstBytes = 0;
  std::uint64_t argbufCbufFfpVsRewriteChangedBytes = 0;
  std::uint64_t argbufCbufFfpVsRewriteUnchangedBytes = 0;
  std::uint64_t argbufCbufFfpVsMatrixChangedBytes = 0;
  std::uint64_t argbufCbufFfpVsMaterialChangedBytes = 0;
  std::uint64_t argbufCbufFfpVsLightChangedBytes = 0;
  std::uint64_t argbufCbufFfpVsBlendChangedBytes = 0;
  std::uint64_t argbufCbufFfpVsTexTransformChangedBytes = 0;
  std::uint64_t argbufCbufFfpVsClipChangedBytes = 0;
  std::uint64_t argbufCbufFfpVsViewportChangedBytes = 0;
  std::uint64_t argbufCbufFfpVsFogPointChangedBytes = 0;
  std::uint64_t setVertexBytesCalls = 0;
  std::uint64_t setVertexBytesBytes = 0;
  std::uint64_t setVertexBytesSlot5Calls = 0;
  std::uint64_t setVertexBytesSlot5Bytes = 0;
  std::uint64_t setVertexBytesOtherCalls = 0;
  std::uint64_t setVertexBytesOtherBytes = 0;
  std::uint64_t transientVertexBytes = 0;
  std::uint64_t transientVertexUserBytes = 0;
  std::uint64_t transientVertexPreuploadBytes = 0;
  std::uint64_t transientVertexDeclFallbackBytes = 0;
  std::uint64_t transientVertexExpandedMainBytes = 0;
  std::uint64_t transientVertexExpandedExtraBytes = 0;
  std::uint64_t transientVertexStagedStreamBytes = 0;
  std::uint64_t transientIndexBytes = 0;
  std::uint64_t transientIndexUserBytes = 0;
  std::uint64_t transientIndexPreuploadBytes = 0;
  std::uint64_t transientIndexShadowFallbackBytes = 0;
  std::uint64_t transientIndexProbeReorderBytes = 0;
  std::uint64_t transientIndexOptimizedOrderBytes = 0;
  std::uint64_t transientIndexStagedIbBytes = 0;
  std::array<EncoderStreamBreakdown, kEncoderBreakdownMaxStreams> streams{};
};

bool encoderBreakdownEnabled();
std::uint64_t encoderBreakdownSeqFilter();
bool encoderBreakdownSeqFilterActive();
bool encoderBreakdownSeqAllowed(std::uint64_t seq);
void emitEncoderBreakdown(const EncoderBreakdown& breakdown);

}  // namespace dxmt9::perf
