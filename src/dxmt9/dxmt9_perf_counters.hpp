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
void countSubmitDrawRunBatchGroup(std::uint32_t recordCount);
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
void countEncodeDrawStreamBindCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawIssueCpuTime(std::uint64_t nanoseconds);
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
// round trip (PE -> unix import -> seqId assignment -> return). Sampled
// at the d3d9 PE-side bridge entry (dxmt9c_device_commit_chunk in
// device_c_chunk_replay.cpp) so the measurement isolates the bridge ABI
// crossing cost from encode and GPU work, and so a regression in
// marshalling / importer validation surfaces independently of the
// existing chunk_admit / chunk_reject opcode counts.
void countBridgeCommitLatencyNs(std::uint64_t nanoseconds);
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
  std::uint64_t alphaTestEnabledDraws = 0;
  std::uint64_t alphaTestEffectiveDraws = 0;
  std::uint64_t clipPlaneEnabledDraws = 0;
  std::uint64_t pointDraws = 0;
  std::uint64_t lineDraws = 0;
  std::uint64_t triangleDraws = 0;
  std::uint64_t primitiveCount = 0;
  std::uint64_t triangleEstimate = 0;
  std::uint64_t vertexCount = 0;
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
  std::uint64_t splitLargeIndexedPrimitiveCount = 0;
  std::uint64_t indexedOrderProbeDraws = 0;
  std::uint64_t indexedOrderProbeSkipped = 0;
  std::uint64_t indexedOrderProbeBytes = 0;
  std::uint64_t indexedVertexReuseSamples = 0;
  std::uint64_t indexedVertexReuseSkipped = 0;
  std::uint64_t indexedVertexReferenceCount = 0;
  std::uint64_t indexedUniqueVertexEstimate = 0;
  std::uint64_t indexedVertexCacheMissEstimate16 = 0;
  std::uint64_t indexedVertexCacheMissEstimate32 = 0;
  std::uint64_t indexedVertexCacheMissEstimate64 = 0;
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
  std::uint64_t transientIndexBytes = 0;
  std::uint64_t transientIndexUserBytes = 0;
  std::uint64_t transientIndexPreuploadBytes = 0;
  std::uint64_t transientIndexShadowFallbackBytes = 0;
  std::uint64_t transientIndexProbeReorderBytes = 0;
  std::array<EncoderStreamBreakdown, kEncoderBreakdownMaxStreams> streams{};
};

bool encoderBreakdownEnabled();
std::uint64_t encoderBreakdownSeqFilter();
void emitEncoderBreakdown(const EncoderBreakdown& breakdown);

}  // namespace dxmt9::perf
