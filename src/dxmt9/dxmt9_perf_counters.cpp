#include "dxmt9_perf_counters.hpp"

#include "dxmt9_perf_counters_internal.hpp"

#include "dxmt9/core.hpp"

#include <array>
#include <atomic>
#include <cstdlib>
#include <cstdint>

namespace dxmt9::perf {
namespace detail {

Counters& counters() {
  static Counters value;
  return value;
}

std::uint64_t load(const std::atomic<std::uint64_t>& value) {
  return value.load(std::memory_order_relaxed);
}

}  // namespace detail

using detail::Counters;
using detail::PercentileRing;
using detail::counters;
using detail::load;

namespace {

thread_local D3D9SnapshotUniformBuildContext
    gD3D9SnapshotUniformBuildContext =
        D3D9SnapshotUniformBuildContext::None;

bool inD3D9SnapshotUniformBuildBatchMissContext() noexcept {
  return gD3D9SnapshotUniformBuildContext ==
      D3D9SnapshotUniformBuildContext::BatchMiss;
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

void ensureRegistered() {
  static const bool registered = [] {
    if (enabledFlag()) {
      std::atexit(detail::report);
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
  // DrawContinuation is retained for counter-table numbering stability and
  // folded into the Unknown bucket.
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
    detail::report();
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

}  // namespace dxmt9::perf
