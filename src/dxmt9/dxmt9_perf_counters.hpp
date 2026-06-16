#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace dxmt9::perf {

bool enabled();

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
void countPrepareSlotPsoPrefetchCpuTime(std::uint64_t nanoseconds);
void countUnpublishedSlotPsoPrefetchCpuTime(std::uint64_t nanoseconds);
void countChunkPublishReason(ChunkPublishReason reason,
                             std::uint64_t commandCount);
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
void countPipelineBuild();
void countPipelineCacheHit(PipelineKind kind);
void countPipelineCacheMiss(PipelineKind kind);
void countPipelineBuild(PipelineKind kind);
void recordDrawPsoSlotCount(std::uint64_t count);
void countDrawPsoSlotExhausted();
void countDrawPsoVariantArgbufStage2();
void countDrawPsoVariantTileFfp();
void recordSourceLibraryEntryCount(std::uint64_t count);
void countPipelineBuildFailDraw();
void countPipelineBuildFailLibrary();
void countPipelineBuildFailFunction();
void countPipelineBuildFailPso();
void countDrawSkippedNoPipeline();
void countShaderVariantKeyHashCpuTime(std::uint64_t nanoseconds);
void countRenderPassBegin();
void countRenderPassEnd(EncoderSplitReason reason);
void countHazardProbe(bool bloomOverlap, bool exactOverlap);
enum CommitChunkDrawDeltaBits : std::uint32_t {
  CommitChunkDrawDeltaRenderState = 1u << 0,
  CommitChunkDrawDeltaTexture = 1u << 1,
  CommitChunkDrawDeltaStream = 1u << 2,
  CommitChunkDrawDeltaFvf = 1u << 3,
  CommitChunkDrawDeltaShader = 1u << 4,
  CommitChunkDrawDeltaVertexDecl = 1u << 5,
  CommitChunkDrawDeltaRenderTarget = 1u << 6,
  CommitChunkDrawDeltaDepthStencil = 1u << 7,
  CommitChunkDrawDeltaViewport = 1u << 8,
  CommitChunkDrawDeltaScissor = 1u << 9,
  CommitChunkDrawDeltaTextureStageState = 1u << 10,
  CommitChunkDrawDeltaSamplerState = 1u << 11,
  CommitChunkDrawDeltaMaterial = 1u << 12,
  CommitChunkDrawDeltaClipPlane = 1u << 13,
  CommitChunkDrawDeltaTransform = 1u << 14,
  CommitChunkDrawDeltaLight = 1u << 15,
  CommitChunkDrawDeltaLightEnable = 1u << 16,
  CommitChunkDrawDeltaIndexBuffer = 1u << 17,
};
void countCommitChunkDrawReplay(bool indexed, std::uint32_t deltaMask);
void countDrawPacketActualChange(std::uint32_t declaredMask,
                                 std::uint32_t actualMask);
void countCommitChunkDrawStreamDeltaDetails(std::uint32_t handleChanges,
                                            std::uint32_t offsetChanges,
                                            std::uint32_t strideChanges);
void countCommitChunkDrawIndexBufferHandleDelta();
void countCommitChunkDrawRunScan(std::uint32_t stop,
                                 std::uint32_t recordCount,
                                 std::uint32_t stopRecordType,
                                 std::uint64_t stopRecordPayloadBytes = 0,
                                 std::uint32_t stopRecordConstCount = 0);
void countCommitChunkDrawRunStateDeltaBucket(std::uint32_t deltaMask);
void countCommitChunkDrawRunBindingOverride(bool streamOverride,
                                            bool indexBufferOverride,
                                            std::size_t bytes);
void countCommitChunkDrawBatchConstUploadPassthrough();
void countCommitChunkDrawSubmissionBatch(std::uint32_t recordCount);
void countCommitChunkApplyDrawStateCpuTime(std::uint64_t nanoseconds);
void countCommitChunkDrawRunScanCpuTime(std::uint64_t nanoseconds);
void countCommitChunkDrawRunBuildCpuTime(std::uint64_t nanoseconds);
void countCommitChunkDrawRunSubmitCpuTime(std::uint64_t nanoseconds);
void countCommitChunkDrawRunFinalBindCpuTime(std::uint64_t nanoseconds);
void countCommitChunkQueueDrawSubmissionCpuTime(std::uint64_t nanoseconds);
void countCommitChunkQueueDrawSubmissionEmplaceCpuTime(std::uint64_t nanoseconds);
void countCommitChunkQueueDrawSubmissionSnapshotCpuTime(std::uint64_t nanoseconds);
void countCommitChunkIndexBindCpuTime(std::uint64_t nanoseconds);
void countCommitChunkReplayPendingFlushCpuTime(std::uint64_t nanoseconds);
void countCommitChunkReplayDrawRecordCpuTime(std::uint64_t nanoseconds);
void countCommitChunkReplayNonDrawRecordCpuTime(std::uint64_t nanoseconds);
void countCommitChunkReplayConstRecordCpuTime(std::uint64_t nanoseconds);
void countCommitChunkReplayApplyStateRecordCpuTime(std::uint64_t nanoseconds);
void countCommitChunkReplayClearRecordCpuTime(std::uint64_t nanoseconds);
void countCommitChunkReplayPresentRecordCpuTime(std::uint64_t nanoseconds);
void countCommitChunkReplaySurfaceRecordCpuTime(std::uint64_t nanoseconds);
void countCommitChunkReplayQueryRecordCpuTime(std::uint64_t nanoseconds);
void countCommitChunkReplayOtherRecordCpuTime(std::uint64_t nanoseconds);
void countCommitChunkConstUploadCpuTime(std::uint64_t nanoseconds);
void countSubmitDrawRunBatchGroup(std::uint32_t recordCount);
void countSubmitDrawRunBatchDiscardedState(std::uint64_t records,
                                           std::uint64_t bytes);
void countSubmitDrawRunBindingSnapshotCpuTime(std::uint64_t nanoseconds);
void countSubmitDrawRunPayloadBytesCpuTime(std::uint64_t nanoseconds);
void countSubmitDrawRunSlotPrepareCpuTime(std::uint64_t nanoseconds);
void countSubmitDrawRunResourceMarkCpuTime(std::uint64_t nanoseconds);
void countSubmitDrawRunAppendCpuTime(std::uint64_t nanoseconds);
void countSubmitDrawRunChunkCommitCpuTime(std::uint64_t nanoseconds);
void countSubmitDrawRunBatchCompatScanCpuTime(std::uint64_t nanoseconds);
void countSubmitDrawRunBatchSubmissionAdjacent(bool sameGenerationLane);
void countSubmitDrawRunBatchCompatPair(bool sameGenerationLane, bool compatible);
void countSubmitDrawRunBatchBindingOverrideCpuTime(std::uint64_t nanoseconds);
void countSubmitDrawRunBatchBindingSnapshotCpuTime(std::uint64_t nanoseconds);
void countSubmitDrawRunBatchPayloadBytesCpuTime(std::uint64_t nanoseconds);
void countSubmitDrawRunBatchSlotPrepareCpuTime(std::uint64_t nanoseconds);
void countSubmitDrawRunBatchResourceMarkCpuTime(std::uint64_t nanoseconds);
void countSubmitDrawRunBatchAppendCpuTime(std::uint64_t nanoseconds);
void countSubmitDrawRunBatchAppendReserveCpuTime(std::uint64_t nanoseconds);
void countSubmitDrawRunBatchAppendStateCpuTime(std::uint64_t nanoseconds);
void countSubmitDrawRunBatchAppendStatePsoCpuTime(std::uint64_t nanoseconds);
void countSubmitDrawRunBatchAppendStateInvariantCpuTime(std::uint64_t nanoseconds);
void countSubmitDrawRunBatchAppendStateSoaCpuTime(std::uint64_t nanoseconds);
void countSubmitDrawRunBatchAppendUniformCpuTime(std::uint64_t nanoseconds);
void countSubmitDrawRunBatchAppendPayloadCpuTime(std::uint64_t nanoseconds);
void countSubmitDrawRunBatchAppendParamCpuTime(std::uint64_t nanoseconds);
void countSubmitDrawRunBatchAppendRecordCpuTime(std::uint64_t nanoseconds);
void countSubmitDrawRunBatchAppendPayloadBytes(std::uint64_t bytes);
void countSubmitDrawRunBatchAppendParams(std::uint64_t paramCount);
void countSubmitDrawRunBatchChunkCommitCpuTime(std::uint64_t nanoseconds);
void countD3D9DrawStateCacheLookup(bool hit, bool includeIndexBuffer);
void countD3D9DrawStateCacheDirectLookup(bool hit, bool includeIndexBuffer);
void countD3D9DrawStateCacheBatchLookup(bool hit);
void countD3D9DrawStateCacheUniformRefresh();
void countD3D9DrawStateCacheMissReason(std::uint32_t reasonMask);
void countD3D9DrawStateCacheBatchMissReason(std::uint32_t reasonMask);
void countD3D9SnapshotDrawSubmissionCpuTime(std::uint64_t nanoseconds);
void countD3D9SnapshotCacheLookupCpuTime(std::uint64_t nanoseconds);
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
void countD3D9SnapshotUniformCopyCpuTime(std::uint64_t nanoseconds);
void countD3D9SnapshotUniformMaterialized(std::uint64_t bytes);
void countD3D9SnapshotUniformMaterializedCompactOpportunity(
    std::uint64_t candidateBytes,
    std::uint64_t savedBytes,
    std::uint64_t fixedBytes,
    std::uint64_t vertexBytes,
    std::uint64_t pixelBytes);
void countD3D9SnapshotUniformElided(std::uint64_t bytes);
void countD3D9SnapshotUniformAdjacentSameGeneration(bool sameStateLane,
                                                    std::uint64_t bytes);
void countD3D9SnapshotUniformAdjacentSamePayloadHash(bool sameStateLane,
                                                     bool sameGeneration,
                                                     std::uint64_t bytes);
void countD3D9SnapshotUniformAdjacentComponentHashes(bool sameStateLane,
                                                     bool sameGeneration,
                                                     bool sameVertexConstants,
                                                     bool samePixelConstants,
                                                     bool sameFixedPayload);
void countD3D9SnapshotStateCopyCpuTime(std::uint64_t nanoseconds);
void countD3D9SnapshotStateMaterialized(std::uint64_t bytes);
void countD3D9SnapshotStateElided(std::uint64_t bytes);
void countD3D9SnapshotDebugSnapshotCpuTime(std::uint64_t nanoseconds);
void countD3D9SnapshotFlatStateEntries(std::uint32_t renderStateEntries,
                                       std::uint32_t textureStageStateEntries,
                                       std::uint32_t textureStageStateEntryMax,
                                       std::uint32_t samplerStateEntries,
                                       std::uint32_t samplerStateEntryMax,
                                       bool renderStateOverflow,
                                       bool textureStageStateOverflow,
                                       bool samplerStateOverflow);
void countD3D9SnapshotBindingOverrideCpuTime(std::uint64_t nanoseconds);
void countD3D9SnapshotBindingOverride(std::uint32_t streamScans,
                                      std::uint32_t streamRecords,
                                      bool indexRecord);
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
void countEncodeDrawIndexCacheCandidateFrontierDropped(std::uint64_t dropped);
void countEncodeDrawIndexCacheCandidateLazyFrontier(std::uint64_t heapPops,
                                                    std::uint64_t refreshes,
                                                    std::uint64_t staleDrops,
                                                    std::uint64_t accepted);
void countEncodeDrawIndexCacheCandidateBucketedSelect(
    std::uint64_t vertexVisits,
    std::uint64_t bucketMoves,
    std::uint64_t selected);
void countEncodeDrawIndexCacheCandidateUpperBoundRejected(std::uint64_t rejected);
void countEncodeDrawIndexCacheCandidateMeasureCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawIndexCacheGateCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawIndexCacheApplyCpuTime(std::uint64_t nanoseconds);
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
void countReorderedIndexCacheLookup(bool hit,
                                    bool rejected,
                                    bool created,
                                    std::uint64_t createdBytes);
void countEncodeDrawIssueCpuTime(std::uint64_t nanoseconds);
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
//                          inventory lives in specs/gap_d3d9.md §A.4.
//   * decl_method_unsupported: a vertex-declaration DCL method code
//                          (D3DDECLMETHOD) is non-DEFAULT (PARTIALU=1,
//                          PARTIALV=2, CROSSUV=3, UV=4, LOOKUP=5,
//                          LOOKUPPRESAMPLED=6). dxmt9 only honors the
//                          DEFAULT (0) read path; the rest require
//                          tessellator stages with no Metal mapping.
//                          See specs/gap_d3d9.md §A.5.
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
void countRenderPassTransitionRtChangeSameDepth();
void countRenderPassTransitionSameRtDepthChange();
void countRenderPassTransitionRtDepthChange();
void countRenderPassColorStoreProof(RenderPassColorStoreProof proof);
void countRenderPassDepthStoreProof(RenderPassDepthStoreProof proof);
void countCommandBufferCreateCpuTime(std::uint64_t nanoseconds);
void countCommandBufferCommitCpuTime(std::uint64_t nanoseconds);
// R-VERIF / V1 boundary B2 — wall-clock latency of one commit_chunk()
// round trip (PE -> unix import/replay/queue submit -> return). Sampled
// at dxmt9c_device_commit_chunk in device_c_chunk_replay.cpp. This excludes
// asynchronous encode and GPU work after the call returns, but includes
// importer validation, handle/resource marking, record replay, and queued
// draw submission construction.
void countBridgeCommitLatencyNs(std::uint64_t nanoseconds);
void countCommitChunkImportCpuTime(std::uint64_t nanoseconds);
void countCommitChunkHandleCpuTime(std::uint64_t nanoseconds);
void countCommitChunkReplayCpuTime(std::uint64_t nanoseconds);
void countCommitChunkDrawBatchSubmitCpuTime(std::uint64_t nanoseconds);
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
void countMapBufferWait(std::uint64_t totalNanoseconds,
                        std::uint64_t mutexNanoseconds,
                        std::uint64_t sequenceNanoseconds,
                        std::uint32_t flags,
                        bool waited);
void countPresentBoundaryApplied();
void countPresentBoundarySkipped();
void countPresentBoundaryWait(std::uint64_t nanoseconds);
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
