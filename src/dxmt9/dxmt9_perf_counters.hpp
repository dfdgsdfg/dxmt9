#pragma once

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
void countRenderPassBegin();
void countRenderPassEnd(EncoderSplitReason reason);
void countHazardProbe(bool bloomOverlap, bool exactOverlap);
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
void countEncodeChunkCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawPipelineLookupCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawUniformBuildCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawFvfDecodeCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawStreamBindCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawIssueCpuTime(std::uint64_t nanoseconds);
void countTransientUploadCpuTime(std::uint64_t nanoseconds, std::size_t bytes);
void countUniformVsConsts(std::size_t bytes);
void countUniformPsConsts(std::size_t bytes);
void countUniformFfpVs(std::size_t bytes);
void countUniformFfpPs(std::size_t bytes);
void countUniformVolatilePush();
// R-BACK-5.7. Bumps only on the discrete (non-unified-memory) blit path.
void countManagedTextureUploadBlit(std::uint64_t bytes);
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
  std::uint64_t gpuCommandBufferErrors = 0;
};

bool frameSamplingEnabled();
CounterSnapshot snapshot();
void emitFrameDelta(std::uint64_t frameId,
                    const CounterSnapshot& prev,
                    const CounterSnapshot& curr);

}  // namespace dxmt9::perf
