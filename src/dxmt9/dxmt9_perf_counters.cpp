#include "dxmt9_perf_counters.hpp"

#include "dxmt9/core.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <cstdint>
#include <vector>

namespace dxmt9::perf {
namespace {

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
  std::atomic<std::uint64_t> submitDrawRunBatchGroups{0};
  std::atomic<std::uint64_t> submitDrawRunBatchRecords{0};
  std::atomic<std::uint64_t> submitDrawRunBatchMaxRecords{0};
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
  std::atomic<std::uint64_t> hazardProbeComparisons{0};
  std::atomic<std::uint64_t> hazardBloomOverlaps{0};
  std::atomic<std::uint64_t> hazardExactOverlaps{0};
  std::atomic<std::uint64_t> hazardBloomFalsePositive{0};
  std::atomic<std::uint64_t> commitChunkDrawRecords{0};
  std::atomic<std::uint64_t> commitChunkDrawIndexed{0};
  std::atomic<std::uint64_t> commitChunkDrawNoDelta{0};
  std::atomic<std::uint64_t> commitChunkDrawStateDelta{0};
  std::atomic<std::uint64_t> commitChunkDrawRunScans{0};
  std::atomic<std::uint64_t> commitChunkDrawRunSubmits{0};
  std::atomic<std::uint64_t> commitChunkDrawRunRecords{0};
  std::atomic<std::uint64_t> commitChunkDrawRunBindingOverrideRecords{0};
  std::atomic<std::uint64_t> commitChunkDrawRunBindingOverrideBytes{0};
  std::atomic<std::uint64_t> commitChunkDrawRunBindingOverrideStreamRecords{0};
  std::atomic<std::uint64_t> commitChunkDrawRunBindingOverrideIbRecords{0};
  std::atomic<std::uint64_t> commitChunkDrawBatchConstUploadPassthrough{0};
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
  std::atomic<std::uint64_t> d3d9DrawStateCacheUniformRefreshes{0};
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
  std::atomic<std::uint64_t> commitChunkDrawRunBreakFirstDelta{0};
  std::atomic<std::uint64_t> commitChunkDrawRunBreakStateDelta{0};
  std::atomic<std::uint64_t> commitChunkDrawRunBreakType{0};
  std::atomic<std::uint64_t> commitChunkDrawRunBreakEnd{0};
  std::atomic<std::uint64_t> commitChunkDrawRunBreakInvalid{0};
  std::atomic<std::uint64_t> commitChunkDrawRunBreakTypeConstantUpload{0};
  std::atomic<std::uint64_t> commitChunkDrawRunBreakTypeConstantUploadBytes{0};
  std::atomic<std::uint64_t> commitChunkDrawRunBreakTypeConstantUploadRegisters{0};
  std::atomic<std::uint64_t> commitChunkDrawRunBreakTypeConstVsF{0};
  std::atomic<std::uint64_t> commitChunkDrawRunBreakTypeConstVsFBytes{0};
  std::atomic<std::uint64_t> commitChunkDrawRunBreakTypeConstVsFRegisters{0};
  std::atomic<std::uint64_t> commitChunkDrawRunBreakTypeConstVsI{0};
  std::atomic<std::uint64_t> commitChunkDrawRunBreakTypeConstVsIBytes{0};
  std::atomic<std::uint64_t> commitChunkDrawRunBreakTypeConstVsIRegisters{0};
  std::atomic<std::uint64_t> commitChunkDrawRunBreakTypeConstVsB{0};
  std::atomic<std::uint64_t> commitChunkDrawRunBreakTypeConstVsBBytes{0};
  std::atomic<std::uint64_t> commitChunkDrawRunBreakTypeConstVsBRegisters{0};
  std::atomic<std::uint64_t> commitChunkDrawRunBreakTypeConstPsF{0};
  std::atomic<std::uint64_t> commitChunkDrawRunBreakTypeConstPsFBytes{0};
  std::atomic<std::uint64_t> commitChunkDrawRunBreakTypeConstPsFRegisters{0};
  std::atomic<std::uint64_t> commitChunkDrawRunBreakTypeConstPsI{0};
  std::atomic<std::uint64_t> commitChunkDrawRunBreakTypeConstPsIBytes{0};
  std::atomic<std::uint64_t> commitChunkDrawRunBreakTypeConstPsIRegisters{0};
  std::atomic<std::uint64_t> commitChunkDrawRunBreakTypeConstPsB{0};
  std::atomic<std::uint64_t> commitChunkDrawRunBreakTypeConstPsBBytes{0};
  std::atomic<std::uint64_t> commitChunkDrawRunBreakTypeConstPsBRegisters{0};
  std::atomic<std::uint64_t> commitChunkDrawRunBreakTypeStateApply{0};
  std::atomic<std::uint64_t> commitChunkDrawRunBreakTypeDrawUp{0};
  std::atomic<std::uint64_t> commitChunkDrawRunBreakTypeClear{0};
  std::atomic<std::uint64_t> commitChunkDrawRunBreakTypePresent{0};
  std::atomic<std::uint64_t> commitChunkDrawRunBreakTypeSurfaceOp{0};
  std::atomic<std::uint64_t> commitChunkDrawRunBreakTypeQueryIssue{0};
  std::atomic<std::uint64_t> commitChunkDrawRunBreakTypeReadback{0};
  std::atomic<std::uint64_t> commitChunkDrawRunBreakTypeOther{0};
  std::atomic<std::uint64_t> commitChunkDrawRunBreakStateDeltaStreamOnly{0};
  std::atomic<std::uint64_t> commitChunkDrawRunBreakStateDeltaIbOnly{0};
  std::atomic<std::uint64_t> commitChunkDrawRunBreakStateDeltaTextureOnly{0};
  std::atomic<std::uint64_t> commitChunkDrawRunBreakStateDeltaShaderOnly{0};
  std::atomic<std::uint64_t> commitChunkDrawRunBreakStateDeltaFvfVdeclOnly{0};
  std::atomic<std::uint64_t> commitChunkDrawRunBreakStateDeltaOtherOnly{0};
  std::atomic<std::uint64_t> commitChunkDrawRunBreakStateDeltaMixed{0};
  std::atomic<std::uint64_t> commitChunkDrawRunBreakStateDeltaMixedGroup2{0};
  std::atomic<std::uint64_t> commitChunkDrawRunBreakStateDeltaMixedGroup3{0};
  std::atomic<std::uint64_t> commitChunkDrawRunBreakStateDeltaMixedGroup4Plus{0};
  std::atomic<std::uint64_t> commitChunkDrawRunBreakStateDeltaStreamIbOnly{0};
  std::atomic<std::uint64_t> commitChunkDrawRunBreakStateDeltaMixedWithStream{0};
  std::atomic<std::uint64_t> commitChunkDrawRunBreakStateDeltaMixedWithIb{0};
  std::atomic<std::uint64_t> commitChunkDrawRunBreakStateDeltaMixedWithTexture{0};
  std::atomic<std::uint64_t> commitChunkDrawRunBreakStateDeltaMixedWithShader{0};
  std::atomic<std::uint64_t> commitChunkDrawRunBreakStateDeltaMixedWithFvfVdecl{0};
  std::atomic<std::uint64_t> commitChunkDrawRunBreakStateDeltaMixedWithOther{0};
  std::atomic<std::uint64_t> commitChunkDrawRunBreakStateDeltaMixedPairStreamIb{0};
  std::atomic<std::uint64_t> commitChunkDrawRunBreakStateDeltaMixedPairStreamTexture{0};
  std::atomic<std::uint64_t> commitChunkDrawRunBreakStateDeltaMixedPairStreamShader{0};
  std::atomic<std::uint64_t> commitChunkDrawRunBreakStateDeltaMixedPairStreamFvfVdecl{0};
  std::atomic<std::uint64_t> commitChunkDrawRunBreakStateDeltaMixedPairIbTexture{0};
  std::atomic<std::uint64_t> commitChunkDrawRunBreakStateDeltaMixedPairIbShader{0};
  std::atomic<std::uint64_t> commitChunkDrawRunBreakStateDeltaMixedPairIbFvfVdecl{0};
  std::atomic<std::uint64_t> commitChunkDrawRunBreakStateDeltaMixedPairTextureShader{0};
  std::atomic<std::uint64_t> commitChunkDrawRunBreakStateDeltaMixedPairTextureFvfVdecl{0};
  std::atomic<std::uint64_t> commitChunkDrawRunBreakStateDeltaMixedPairShaderFvfVdecl{0};
  std::atomic<std::uint64_t> commitChunkDrawDeltaRenderState{0};
  std::atomic<std::uint64_t> commitChunkDrawDeltaTexture{0};
  std::atomic<std::uint64_t> commitChunkDrawDeltaStream{0};
  std::atomic<std::uint64_t> commitChunkDrawDeltaFvf{0};
  std::atomic<std::uint64_t> commitChunkDrawDeltaShader{0};
  std::atomic<std::uint64_t> commitChunkDrawDeltaVertexDecl{0};
  std::atomic<std::uint64_t> commitChunkDrawDeltaRenderTarget{0};
  std::atomic<std::uint64_t> commitChunkDrawDeltaDepthStencil{0};
  std::atomic<std::uint64_t> commitChunkDrawDeltaViewport{0};
  std::atomic<std::uint64_t> commitChunkDrawDeltaScissor{0};
  std::atomic<std::uint64_t> commitChunkDrawDeltaTextureStageState{0};
  std::atomic<std::uint64_t> commitChunkDrawDeltaSamplerState{0};
  std::atomic<std::uint64_t> commitChunkDrawDeltaMaterial{0};
  std::atomic<std::uint64_t> commitChunkDrawDeltaClipPlane{0};
  std::atomic<std::uint64_t> commitChunkDrawDeltaTransform{0};
  std::atomic<std::uint64_t> commitChunkDrawDeltaLight{0};
  std::atomic<std::uint64_t> commitChunkDrawDeltaLightEnable{0};
  std::atomic<std::uint64_t> commitChunkDrawDeltaIndexBuffer{0};
  std::atomic<std::uint64_t> commitChunkDrawDeltaStreamHandle{0};
  std::atomic<std::uint64_t> commitChunkDrawDeltaStreamOffset{0};
  std::atomic<std::uint64_t> commitChunkDrawDeltaStreamStride{0};
  std::atomic<std::uint64_t> commitChunkDrawDeltaIndexBufferHandle{0};
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
  std::atomic<std::uint64_t> encodeDrawStreamBindCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawStreamBindCpuMaxNs{0};
  std::atomic<std::uint64_t> encodeDrawRasterStateCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawVertexStreamBindCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawTextureSamplerBindCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawIndexSetupCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawIndexSourceResolveCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawIndexCacheLookupCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawIndexCacheCandidateCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawIndexCacheOriginalMeasureCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawIndexCacheCandidateBuildCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawIndexCacheCandidateMeasureCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawIndexCacheGateCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawIndexCacheApplyCpuNs{0};
  std::atomic<std::uint64_t> indexedCacheOptCandidateDraws{0};
  std::atomic<std::uint64_t> indexedCacheOptCandidateSkipped{0};
  std::atomic<std::uint64_t> indexedCacheOptCandidateBytes{0};
  std::atomic<std::uint64_t> indexedCacheOptCandidateOriginalMiss16{0};
  std::atomic<std::uint64_t> indexedCacheOptCandidateOriginalMiss32{0};
  std::atomic<std::uint64_t> indexedCacheOptCandidateOriginalMiss64{0};
  std::atomic<std::uint64_t> indexedCacheOptCandidateMiss16{0};
  std::atomic<std::uint64_t> indexedCacheOptCandidateMiss32{0};
  std::atomic<std::uint64_t> indexedCacheOptCandidateMiss64{0};
  std::atomic<std::uint64_t> reorderedIndexCacheLookups{0};
  std::atomic<std::uint64_t> reorderedIndexCacheHits{0};
  std::atomic<std::uint64_t> reorderedIndexCacheRejectedHits{0};
  std::atomic<std::uint64_t> reorderedIndexCacheMisses{0};
  std::atomic<std::uint64_t> reorderedIndexCacheCreated{0};
  std::atomic<std::uint64_t> reorderedIndexCacheCreatedBytes{0};
  std::atomic<std::uint64_t> encodeDrawIssueCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawIssueCpuMaxNs{0};
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
  // specs/gap_d3d9.md §A.4 / §A.5. A category-level counter per side
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
  std::atomic<std::uint64_t> renderPassSameKeyReentryPreservationBytes{0};
  std::atomic<std::uint64_t> renderPassSameKeyReentryColorPreservationBytes{0};
  std::atomic<std::uint64_t> renderPassSameKeyReentryDepthPreservationBytes{0};
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
  std::atomic<std::uint64_t> commandBufferCreateCpuNs{0};
  std::atomic<std::uint64_t> commandBufferCreateCpuMaxNs{0};
  std::atomic<std::uint64_t> commandBufferCommitCpuNs{0};
  std::atomic<std::uint64_t> commandBufferCommitCpuMaxNs{0};
  // V1 boundary B2 — bridge commit latency (countBridgeCommitLatencyNs).
  // Measured at the d3d9 PE-side commit_chunk entry, isolated from any
  // encode or GPU work so a marshalling / importer regression surfaces
  // even on near-empty chunks where chunk_admit alone would be flat.
  std::atomic<std::uint64_t> bridgeCommitLatencyNs{0};
  std::atomic<std::uint64_t> bridgeCommitLatencyMaxNs{0};
  std::atomic<std::uint64_t> completionWaits{0};
  std::atomic<std::uint64_t> completionWaitNs{0};
  std::atomic<std::uint64_t> completionWaitMaxNs{0};
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
  std::atomic<std::uint64_t> presentBoundaryApplied{0};
  std::atomic<std::uint64_t> presentBoundarySkipped{0};
  std::atomic<std::uint64_t> presentBoundaryWaits{0};
  std::atomic<std::uint64_t> presentBoundaryWaitNs{0};
  std::atomic<std::uint64_t> presentBoundaryWaitMaxNs{0};
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
  // V1 boundary B2 — paired with bridgeCommitLatency*Ns above.
  PercentileRing bridgeCommitLatencyRing;
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
    // V1 boundary B2: bridge commit latency is reported in raw nanoseconds
    // (not /1e6 ms) because per-call bridge cost is sub-microsecond and a
    // ms-rounded value collapses to 0.000 across an entire run.
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
    {"hazard_probe", CounterEntry::Kind::UnsignedCount, &Counters::hazardProbeComparisons, nullptr, nullptr, 0.0},
    {"hazard_bloom", CounterEntry::Kind::UnsignedCount, &Counters::hazardBloomOverlaps, nullptr, nullptr, 0.0},
    {"hazard_exact", CounterEntry::Kind::UnsignedCount, &Counters::hazardExactOverlaps, nullptr, nullptr, 0.0},
    {"hazard_bloom_false_positive", CounterEntry::Kind::UnsignedCount, &Counters::hazardBloomFalsePositive, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_records", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawRecords, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_indexed", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawIndexed, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_no_delta", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawNoDelta, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_state_delta", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawStateDelta, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_run_scans", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawRunScans, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_run_submits", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawRunSubmits, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_run_records", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawRunRecords, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_run_binding_override_records", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawRunBindingOverrideRecords, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_run_binding_override_bytes", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawRunBindingOverrideBytes, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_run_binding_override_stream_records", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawRunBindingOverrideStreamRecords, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_run_binding_override_ib_records", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawRunBindingOverrideIbRecords, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_batch_const_upload_passthrough", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawBatchConstUploadPassthrough, nullptr, nullptr, 0.0},
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
    {"d3d9_draw_state_cache_hits", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheHits, nullptr, nullptr, 0.0},
    {"d3d9_draw_state_cache_misses", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheMisses, nullptr, nullptr, 0.0},
    {"d3d9_draw_state_cache_hit_with_index", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheHitWithIndex, nullptr, nullptr, 0.0},
    {"d3d9_draw_state_cache_miss_with_index", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheMissWithIndex, nullptr, nullptr, 0.0},
    {"d3d9_draw_state_cache_hit_no_index", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheHitNoIndex, nullptr, nullptr, 0.0},
    {"d3d9_draw_state_cache_miss_no_index", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheMissNoIndex, nullptr, nullptr, 0.0},
    {"d3d9_draw_state_cache_uniform_refreshes", CounterEntry::Kind::UnsignedCount, &Counters::d3d9DrawStateCacheUniformRefreshes, nullptr, nullptr, 0.0},
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
    {"commit_chunk_draw_run_break_first_delta", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawRunBreakFirstDelta, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_run_break_state_delta", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawRunBreakStateDelta, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_run_break_type", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawRunBreakType, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_run_break_end", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawRunBreakEnd, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_run_break_invalid", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawRunBreakInvalid, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_run_break_type_const_upload", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawRunBreakTypeConstantUpload, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_run_break_type_const_upload_bytes", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawRunBreakTypeConstantUploadBytes, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_run_break_type_const_upload_registers", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawRunBreakTypeConstantUploadRegisters, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_run_break_type_const_vs_f", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawRunBreakTypeConstVsF, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_run_break_type_const_vs_f_bytes", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawRunBreakTypeConstVsFBytes, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_run_break_type_const_vs_f_registers", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawRunBreakTypeConstVsFRegisters, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_run_break_type_const_vs_i", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawRunBreakTypeConstVsI, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_run_break_type_const_vs_i_bytes", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawRunBreakTypeConstVsIBytes, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_run_break_type_const_vs_i_registers", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawRunBreakTypeConstVsIRegisters, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_run_break_type_const_vs_b", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawRunBreakTypeConstVsB, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_run_break_type_const_vs_b_bytes", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawRunBreakTypeConstVsBBytes, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_run_break_type_const_vs_b_registers", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawRunBreakTypeConstVsBRegisters, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_run_break_type_const_ps_f", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawRunBreakTypeConstPsF, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_run_break_type_const_ps_f_bytes", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawRunBreakTypeConstPsFBytes, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_run_break_type_const_ps_f_registers", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawRunBreakTypeConstPsFRegisters, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_run_break_type_const_ps_i", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawRunBreakTypeConstPsI, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_run_break_type_const_ps_i_bytes", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawRunBreakTypeConstPsIBytes, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_run_break_type_const_ps_i_registers", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawRunBreakTypeConstPsIRegisters, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_run_break_type_const_ps_b", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawRunBreakTypeConstPsB, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_run_break_type_const_ps_b_bytes", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawRunBreakTypeConstPsBBytes, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_run_break_type_const_ps_b_registers", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawRunBreakTypeConstPsBRegisters, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_run_break_type_apply_state", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawRunBreakTypeStateApply, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_run_break_type_draw_up", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawRunBreakTypeDrawUp, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_run_break_type_clear", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawRunBreakTypeClear, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_run_break_type_present", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawRunBreakTypePresent, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_run_break_type_surface_op", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawRunBreakTypeSurfaceOp, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_run_break_type_query_issue", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawRunBreakTypeQueryIssue, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_run_break_type_readback", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawRunBreakTypeReadback, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_run_break_type_other", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawRunBreakTypeOther, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_run_break_state_delta_stream_only", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawRunBreakStateDeltaStreamOnly, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_run_break_state_delta_ib_only", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawRunBreakStateDeltaIbOnly, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_run_break_state_delta_texture_only", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawRunBreakStateDeltaTextureOnly, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_run_break_state_delta_shader_only", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawRunBreakStateDeltaShaderOnly, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_run_break_state_delta_fvf_vdecl_only", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawRunBreakStateDeltaFvfVdeclOnly, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_run_break_state_delta_other_only", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawRunBreakStateDeltaOtherOnly, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_run_break_state_delta_mixed", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawRunBreakStateDeltaMixed, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_run_break_state_delta_mixed_group2", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawRunBreakStateDeltaMixedGroup2, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_run_break_state_delta_mixed_group3", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawRunBreakStateDeltaMixedGroup3, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_run_break_state_delta_mixed_group4plus", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawRunBreakStateDeltaMixedGroup4Plus, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_run_break_state_delta_stream_ib_only", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawRunBreakStateDeltaStreamIbOnly, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_run_break_state_delta_mixed_with_stream", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawRunBreakStateDeltaMixedWithStream, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_run_break_state_delta_mixed_with_ib", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawRunBreakStateDeltaMixedWithIb, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_run_break_state_delta_mixed_with_texture", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawRunBreakStateDeltaMixedWithTexture, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_run_break_state_delta_mixed_with_shader", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawRunBreakStateDeltaMixedWithShader, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_run_break_state_delta_mixed_with_fvf_vdecl", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawRunBreakStateDeltaMixedWithFvfVdecl, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_run_break_state_delta_mixed_with_other", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawRunBreakStateDeltaMixedWithOther, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_run_break_state_delta_mixed_pair_stream_ib", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawRunBreakStateDeltaMixedPairStreamIb, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_run_break_state_delta_mixed_pair_stream_texture", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawRunBreakStateDeltaMixedPairStreamTexture, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_run_break_state_delta_mixed_pair_stream_shader", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawRunBreakStateDeltaMixedPairStreamShader, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_run_break_state_delta_mixed_pair_stream_fvf_vdecl", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawRunBreakStateDeltaMixedPairStreamFvfVdecl, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_run_break_state_delta_mixed_pair_ib_texture", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawRunBreakStateDeltaMixedPairIbTexture, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_run_break_state_delta_mixed_pair_ib_shader", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawRunBreakStateDeltaMixedPairIbShader, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_run_break_state_delta_mixed_pair_ib_fvf_vdecl", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawRunBreakStateDeltaMixedPairIbFvfVdecl, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_run_break_state_delta_mixed_pair_texture_shader", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawRunBreakStateDeltaMixedPairTextureShader, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_run_break_state_delta_mixed_pair_texture_fvf_vdecl", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawRunBreakStateDeltaMixedPairTextureFvfVdecl, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_run_break_state_delta_mixed_pair_shader_fvf_vdecl", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawRunBreakStateDeltaMixedPairShaderFvfVdecl, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_delta_render_state", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawDeltaRenderState, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_delta_texture", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawDeltaTexture, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_delta_stream", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawDeltaStream, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_delta_fvf", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawDeltaFvf, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_delta_shader", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawDeltaShader, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_delta_vdecl", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawDeltaVertexDecl, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_delta_rt", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawDeltaRenderTarget, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_delta_ds", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawDeltaDepthStencil, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_delta_viewport", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawDeltaViewport, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_delta_scissor", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawDeltaScissor, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_delta_tss", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawDeltaTextureStageState, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_delta_sampler", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawDeltaSamplerState, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_delta_material", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawDeltaMaterial, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_delta_clip", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawDeltaClipPlane, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_delta_transform", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawDeltaTransform, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_delta_light", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawDeltaLight, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_delta_light_enable", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawDeltaLightEnable, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_delta_ib", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawDeltaIndexBuffer, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_delta_stream_handle", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawDeltaStreamHandle, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_delta_stream_offset", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawDeltaStreamOffset, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_delta_stream_stride", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawDeltaStreamStride, nullptr, nullptr, 0.0},
    {"commit_chunk_draw_delta_ib_handle", CounterEntry::Kind::UnsignedCount, &Counters::commitChunkDrawDeltaIndexBufferHandle, nullptr, nullptr, 0.0},
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
    {"encode_draw_stream_bind_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawStreamBindCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_stream_bind_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawStreamBindCpuMaxNs, nullptr, nullptr, 0.0},
    {"encode_draw_stream_bind_cpu_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::encodeDrawStreamBindCpuRing, 0.5},
    {"encode_draw_stream_bind_cpu_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::encodeDrawStreamBindCpuRing, 0.95},
    {"encode_draw_stream_bind_cpu_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::encodeDrawStreamBindCpuRing, 0.99},
    {"encode_draw_raster_state_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawRasterStateCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_vertex_stream_bind_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawVertexStreamBindCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_texture_sampler_bind_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawTextureSamplerBindCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_index_setup_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawIndexSetupCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_index_source_resolve_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawIndexSourceResolveCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_index_cache_lookup_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawIndexCacheLookupCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_index_cache_candidate_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawIndexCacheCandidateCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_index_cache_original_measure_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawIndexCacheOriginalMeasureCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_index_cache_candidate_build_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawIndexCacheCandidateBuildCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_index_cache_candidate_measure_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawIndexCacheCandidateMeasureCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_index_cache_gate_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawIndexCacheGateCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_index_cache_apply_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawIndexCacheApplyCpuNs, nullptr, nullptr, 0.0},
    {"indexed_cache_opt_candidate_draws", CounterEntry::Kind::UnsignedCount, &Counters::indexedCacheOptCandidateDraws, nullptr, nullptr, 0.0},
    {"indexed_cache_opt_candidate_skipped", CounterEntry::Kind::UnsignedCount, &Counters::indexedCacheOptCandidateSkipped, nullptr, nullptr, 0.0},
    {"indexed_cache_opt_candidate_bytes", CounterEntry::Kind::UnsignedCount, &Counters::indexedCacheOptCandidateBytes, nullptr, nullptr, 0.0},
    {"indexed_cache_opt_candidate_original_miss16", CounterEntry::Kind::UnsignedCount, &Counters::indexedCacheOptCandidateOriginalMiss16, nullptr, nullptr, 0.0},
    {"indexed_cache_opt_candidate_original_miss32", CounterEntry::Kind::UnsignedCount, &Counters::indexedCacheOptCandidateOriginalMiss32, nullptr, nullptr, 0.0},
    {"indexed_cache_opt_candidate_original_miss64", CounterEntry::Kind::UnsignedCount, &Counters::indexedCacheOptCandidateOriginalMiss64, nullptr, nullptr, 0.0},
    {"indexed_cache_opt_candidate_miss16", CounterEntry::Kind::UnsignedCount, &Counters::indexedCacheOptCandidateMiss16, nullptr, nullptr, 0.0},
    {"indexed_cache_opt_candidate_miss32", CounterEntry::Kind::UnsignedCount, &Counters::indexedCacheOptCandidateMiss32, nullptr, nullptr, 0.0},
    {"indexed_cache_opt_candidate_miss64", CounterEntry::Kind::UnsignedCount, &Counters::indexedCacheOptCandidateMiss64, nullptr, nullptr, 0.0},
    {"reordered_index_cache_lookups", CounterEntry::Kind::UnsignedCount, &Counters::reorderedIndexCacheLookups, nullptr, nullptr, 0.0},
    {"reordered_index_cache_hits", CounterEntry::Kind::UnsignedCount, &Counters::reorderedIndexCacheHits, nullptr, nullptr, 0.0},
    {"reordered_index_cache_rejected_hits", CounterEntry::Kind::UnsignedCount, &Counters::reorderedIndexCacheRejectedHits, nullptr, nullptr, 0.0},
    {"reordered_index_cache_misses", CounterEntry::Kind::UnsignedCount, &Counters::reorderedIndexCacheMisses, nullptr, nullptr, 0.0},
    {"reordered_index_cache_created", CounterEntry::Kind::UnsignedCount, &Counters::reorderedIndexCacheCreated, nullptr, nullptr, 0.0},
    {"reordered_index_cache_created_bytes", CounterEntry::Kind::UnsignedCount, &Counters::reorderedIndexCacheCreatedBytes, nullptr, nullptr, 0.0},
    {"encode_draw_issue_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawIssueCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_issue_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawIssueCpuMaxNs, nullptr, nullptr, 0.0},
    {"encode_draw_issue_cpu_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::encodeDrawIssueCpuRing, 0.5},
    {"encode_draw_issue_cpu_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::encodeDrawIssueCpuRing, 0.95},
    {"encode_draw_issue_cpu_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::encodeDrawIssueCpuRing, 0.99},
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
    {"render_pass_same_key_reentry_preservation_bytes", CounterEntry::Kind::UnsignedCount, &Counters::renderPassSameKeyReentryPreservationBytes, nullptr, nullptr, 0.0},
    {"render_pass_same_key_reentry_color_preservation_bytes", CounterEntry::Kind::UnsignedCount, &Counters::renderPassSameKeyReentryColorPreservationBytes, nullptr, nullptr, 0.0},
    {"render_pass_same_key_reentry_depth_preservation_bytes", CounterEntry::Kind::UnsignedCount, &Counters::renderPassSameKeyReentryDepthPreservationBytes, nullptr, nullptr, 0.0},
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
    // V1 boundary B2 — bridge commit latency in raw nanoseconds (sub-us
    // per-call cost would round to 0 in ms). Sum + max + 3 percentiles.
    {"bridge_commit_latency_ns", CounterEntry::Kind::UnsignedCount, &Counters::bridgeCommitLatencyNs, nullptr, nullptr, 0.0},
    {"bridge_commit_latency_max_ns", CounterEntry::Kind::UnsignedCount, &Counters::bridgeCommitLatencyMaxNs, nullptr, nullptr, 0.0},
    {"bridge_commit_latency_p50_ns", CounterEntry::Kind::PercentileNs, nullptr, nullptr, &Counters::bridgeCommitLatencyRing, 0.5},
    {"bridge_commit_latency_p95_ns", CounterEntry::Kind::PercentileNs, nullptr, nullptr, &Counters::bridgeCommitLatencyRing, 0.95},
    {"bridge_commit_latency_p99_ns", CounterEntry::Kind::PercentileNs, nullptr, nullptr, &Counters::bridgeCommitLatencyRing, 0.99},
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
    {"present_boundary_applied", CounterEntry::Kind::UnsignedCount, &Counters::presentBoundaryApplied, nullptr, nullptr, 0.0},
    {"present_boundary_skipped", CounterEntry::Kind::UnsignedCount, &Counters::presentBoundarySkipped, nullptr, nullptr, 0.0},
    {"present_boundary_waits", CounterEntry::Kind::UnsignedCount, &Counters::presentBoundaryWaits, nullptr, nullptr, 0.0},
    {"present_boundary_wait_ms", CounterEntry::Kind::Milliseconds, &Counters::presentBoundaryWaitNs, nullptr, nullptr, 0.0},
    {"present_boundary_wait_max_ms", CounterEntry::Kind::Milliseconds, &Counters::presentBoundaryWaitMaxNs, nullptr, nullptr, 0.0},
    {"present_boundary_wait_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::presentBoundaryWaitRing, 0.5},
    {"present_boundary_wait_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::presentBoundaryWaitRing, 0.95},
    {"present_boundary_wait_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::presentBoundaryWaitRing, 0.99},
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
};

void report() {
  if (!enabledFlag()) {
    return;
  }
  const Counters& c = counters();
  std::fprintf(stderr, "[dxmt9-perf]");
  for (const auto& e : kCounterTable) {
    switch (e.kind) {
      case CounterEntry::Kind::UnsignedCount:
        std::fprintf(stderr, " %s=%llu", e.key,
                     static_cast<unsigned long long>(load(c.*e.atomicField)));
        break;
      case CounterEntry::Kind::Milliseconds:
        std::fprintf(stderr, " %s=%.3f", e.key,
                     static_cast<double>(load(c.*e.atomicField)) / 1000000.0);
        break;
      case CounterEntry::Kind::Hex64:
        std::fprintf(stderr, " %s=0x%llx", e.key,
                     static_cast<unsigned long long>(load(c.*e.atomicField)));
        break;
      case CounterEntry::Kind::WidthByHeight:
        std::fprintf(stderr, " %s=%llux%llu", e.key,
                     static_cast<unsigned long long>(load(c.*e.atomicField)),
                     static_cast<unsigned long long>(load(c.*e.field2)));
        break;
      case CounterEntry::Kind::PercentileMs:
        std::fprintf(stderr, " %s=%.3f", e.key,
                     static_cast<double>((c.*e.ringField).percentile(e.percentile)) /
                         1000000.0);
        break;
      case CounterEntry::Kind::PercentileNs:
        std::fprintf(stderr, " %s=%llu", e.key,
                     static_cast<unsigned long long>(
                         (c.*e.ringField).percentile(e.percentile)));
        break;
    }
  }
  std::fputc('\n', stderr);
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

void updateMax(std::atomic<std::uint64_t>& counter, std::uint64_t value) {
  if (!enabled()) {
    return;
  }
  auto current = counter.load(std::memory_order_relaxed);
  while (current < value &&
         !counter.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
  }
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

void countSubmitDraw() {
  add(counters().submitDraw);
}

void countChunkAdmit() {
  add(counters().chunkAdmit);
}

void countChunkReject() {
  add(counters().chunkReject);
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

void countCommitChunkDrawReplay(bool indexed, std::uint32_t deltaMask) {
  auto& c = counters();
  add(c.commitChunkDrawRecords);
  if (indexed) {
    add(c.commitChunkDrawIndexed);
  }
  if (deltaMask == 0) {
    add(c.commitChunkDrawNoDelta);
    return;
  }

  add(c.commitChunkDrawStateDelta);
  auto addIf = [&](std::uint32_t bit, std::atomic<std::uint64_t>& counter) {
    if ((deltaMask & bit) != 0) {
      add(counter);
    }
  };
  addIf(CommitChunkDrawDeltaRenderState, c.commitChunkDrawDeltaRenderState);
  addIf(CommitChunkDrawDeltaTexture, c.commitChunkDrawDeltaTexture);
  addIf(CommitChunkDrawDeltaStream, c.commitChunkDrawDeltaStream);
  addIf(CommitChunkDrawDeltaFvf, c.commitChunkDrawDeltaFvf);
  addIf(CommitChunkDrawDeltaShader, c.commitChunkDrawDeltaShader);
  addIf(CommitChunkDrawDeltaVertexDecl, c.commitChunkDrawDeltaVertexDecl);
  addIf(CommitChunkDrawDeltaRenderTarget, c.commitChunkDrawDeltaRenderTarget);
  addIf(CommitChunkDrawDeltaDepthStencil, c.commitChunkDrawDeltaDepthStencil);
  addIf(CommitChunkDrawDeltaViewport, c.commitChunkDrawDeltaViewport);
  addIf(CommitChunkDrawDeltaScissor, c.commitChunkDrawDeltaScissor);
  addIf(CommitChunkDrawDeltaTextureStageState, c.commitChunkDrawDeltaTextureStageState);
  addIf(CommitChunkDrawDeltaSamplerState, c.commitChunkDrawDeltaSamplerState);
  addIf(CommitChunkDrawDeltaMaterial, c.commitChunkDrawDeltaMaterial);
  addIf(CommitChunkDrawDeltaClipPlane, c.commitChunkDrawDeltaClipPlane);
  addIf(CommitChunkDrawDeltaTransform, c.commitChunkDrawDeltaTransform);
  addIf(CommitChunkDrawDeltaLight, c.commitChunkDrawDeltaLight);
  addIf(CommitChunkDrawDeltaLightEnable, c.commitChunkDrawDeltaLightEnable);
  addIf(CommitChunkDrawDeltaIndexBuffer, c.commitChunkDrawDeltaIndexBuffer);
}

void countCommitChunkDrawStreamDeltaDetails(std::uint32_t handleChanges,
                                            std::uint32_t offsetChanges,
                                            std::uint32_t strideChanges) {
  auto& c = counters();
  add(c.commitChunkDrawDeltaStreamHandle, handleChanges);
  add(c.commitChunkDrawDeltaStreamOffset, offsetChanges);
  add(c.commitChunkDrawDeltaStreamStride, strideChanges);
}

void countCommitChunkDrawIndexBufferHandleDelta() {
  add(counters().commitChunkDrawDeltaIndexBufferHandle);
}

void countCommitChunkDrawRunScan(std::uint32_t stop,
                                 std::uint32_t recordCount,
                                 std::uint32_t stopRecordType,
                                 std::uint64_t stopRecordPayloadBytes,
                                 std::uint32_t stopRecordConstCount) {
  auto& c = counters();
  add(c.commitChunkDrawRunScans);
  if (recordCount >= 2u) {
    add(c.commitChunkDrawRunSubmits);
    add(c.commitChunkDrawRunRecords, recordCount);
    return;
  }

  // Mirrors ImportedDrawRunScanStop without exposing device_c headers here.
  switch (stop) {
  case 1:
    add(c.commitChunkDrawRunBreakFirstDelta);
    break;
  case 2:
    add(c.commitChunkDrawRunBreakEnd);
    break;
  case 3:
    add(c.commitChunkDrawRunBreakInvalid);
    break;
  case 4:
  case 6:
    add(c.commitChunkDrawRunBreakType);
    switch (stopRecordType) {
    case 3:
    case 4:
      add(c.commitChunkDrawRunBreakTypeDrawUp);
      break;
    case 14:
    case 15:
    case 16:
    case 17:
    case 18:
    case 19:
      add(c.commitChunkDrawRunBreakTypeConstantUpload);
      add(c.commitChunkDrawRunBreakTypeConstantUploadBytes, stopRecordPayloadBytes);
      add(c.commitChunkDrawRunBreakTypeConstantUploadRegisters, stopRecordConstCount);
      switch (stopRecordType) {
      case 14:
        add(c.commitChunkDrawRunBreakTypeConstVsF);
        add(c.commitChunkDrawRunBreakTypeConstVsFBytes, stopRecordPayloadBytes);
        add(c.commitChunkDrawRunBreakTypeConstVsFRegisters, stopRecordConstCount);
        break;
      case 15:
        add(c.commitChunkDrawRunBreakTypeConstVsI);
        add(c.commitChunkDrawRunBreakTypeConstVsIBytes, stopRecordPayloadBytes);
        add(c.commitChunkDrawRunBreakTypeConstVsIRegisters, stopRecordConstCount);
        break;
      case 16:
        add(c.commitChunkDrawRunBreakTypeConstVsB);
        add(c.commitChunkDrawRunBreakTypeConstVsBBytes, stopRecordPayloadBytes);
        add(c.commitChunkDrawRunBreakTypeConstVsBRegisters, stopRecordConstCount);
        break;
      case 17:
        add(c.commitChunkDrawRunBreakTypeConstPsF);
        add(c.commitChunkDrawRunBreakTypeConstPsFBytes, stopRecordPayloadBytes);
        add(c.commitChunkDrawRunBreakTypeConstPsFRegisters, stopRecordConstCount);
        break;
      case 18:
        add(c.commitChunkDrawRunBreakTypeConstPsI);
        add(c.commitChunkDrawRunBreakTypeConstPsIBytes, stopRecordPayloadBytes);
        add(c.commitChunkDrawRunBreakTypeConstPsIRegisters, stopRecordConstCount);
        break;
      case 19:
        add(c.commitChunkDrawRunBreakTypeConstPsB);
        add(c.commitChunkDrawRunBreakTypeConstPsBBytes, stopRecordPayloadBytes);
        add(c.commitChunkDrawRunBreakTypeConstPsBRegisters, stopRecordConstCount);
        break;
      default:
        break;
      }
      break;
    case 20:
      add(c.commitChunkDrawRunBreakTypeClear);
      break;
    case 21:
      add(c.commitChunkDrawRunBreakTypePresent);
      break;
    case 22:
    case 23:
    case 24:
    case 25:
    case 29:
      add(c.commitChunkDrawRunBreakTypeSurfaceOp);
      break;
    case 26:
      add(c.commitChunkDrawRunBreakTypeQueryIssue);
      break;
    case 27:
      add(c.commitChunkDrawRunBreakTypeReadback);
      break;
    case 28:
      add(c.commitChunkDrawRunBreakTypeStateApply);
      break;
    default:
      add(c.commitChunkDrawRunBreakTypeOther);
      break;
    }
    break;
  case 5:
    add(c.commitChunkDrawRunBreakStateDelta);
    break;
  default:
    break;
  }
}

void countCommitChunkDrawRunStateDeltaBucket(std::uint32_t deltaMask) {
  auto& c = counters();
  constexpr std::uint32_t streamMask = CommitChunkDrawDeltaStream;
  constexpr std::uint32_t ibMask = CommitChunkDrawDeltaIndexBuffer;
  constexpr std::uint32_t textureMask =
      CommitChunkDrawDeltaTexture |
      CommitChunkDrawDeltaTextureStageState |
      CommitChunkDrawDeltaSamplerState;
  constexpr std::uint32_t shaderMask = CommitChunkDrawDeltaShader;
  constexpr std::uint32_t fvfVdeclMask =
      CommitChunkDrawDeltaFvf |
      CommitChunkDrawDeltaVertexDecl;
  constexpr std::uint32_t knownMask =
      streamMask | ibMask | textureMask | shaderMask | fvfVdeclMask;

  const bool stream = (deltaMask & streamMask) != 0;
  const bool ib = (deltaMask & ibMask) != 0;
  const bool texture = (deltaMask & textureMask) != 0;
  const bool shader = (deltaMask & shaderMask) != 0;
  const bool fvfVdecl = (deltaMask & fvfVdeclMask) != 0;
  const bool other = (deltaMask & ~knownMask) != 0;
  const unsigned groups =
      (stream ? 1u : 0u) +
      (ib ? 1u : 0u) +
      (texture ? 1u : 0u) +
      (shader ? 1u : 0u) +
      (fvfVdecl ? 1u : 0u) +
      (other ? 1u : 0u);

  if (groups != 1u) {
    add(c.commitChunkDrawRunBreakStateDeltaMixed);
    if (groups == 2u) {
      add(c.commitChunkDrawRunBreakStateDeltaMixedGroup2);
      if (stream && ib) {
        add(c.commitChunkDrawRunBreakStateDeltaStreamIbOnly);
      }
    } else if (groups == 3u) {
      add(c.commitChunkDrawRunBreakStateDeltaMixedGroup3);
    } else {
      add(c.commitChunkDrawRunBreakStateDeltaMixedGroup4Plus);
    }
    if (stream) add(c.commitChunkDrawRunBreakStateDeltaMixedWithStream);
    if (ib) add(c.commitChunkDrawRunBreakStateDeltaMixedWithIb);
    if (texture) add(c.commitChunkDrawRunBreakStateDeltaMixedWithTexture);
    if (shader) add(c.commitChunkDrawRunBreakStateDeltaMixedWithShader);
    if (fvfVdecl) add(c.commitChunkDrawRunBreakStateDeltaMixedWithFvfVdecl);
    if (other) add(c.commitChunkDrawRunBreakStateDeltaMixedWithOther);
    if (stream && ib) add(c.commitChunkDrawRunBreakStateDeltaMixedPairStreamIb);
    if (stream && texture) add(c.commitChunkDrawRunBreakStateDeltaMixedPairStreamTexture);
    if (stream && shader) add(c.commitChunkDrawRunBreakStateDeltaMixedPairStreamShader);
    if (stream && fvfVdecl) add(c.commitChunkDrawRunBreakStateDeltaMixedPairStreamFvfVdecl);
    if (ib && texture) add(c.commitChunkDrawRunBreakStateDeltaMixedPairIbTexture);
    if (ib && shader) add(c.commitChunkDrawRunBreakStateDeltaMixedPairIbShader);
    if (ib && fvfVdecl) add(c.commitChunkDrawRunBreakStateDeltaMixedPairIbFvfVdecl);
    if (texture && shader) add(c.commitChunkDrawRunBreakStateDeltaMixedPairTextureShader);
    if (texture && fvfVdecl) add(c.commitChunkDrawRunBreakStateDeltaMixedPairTextureFvfVdecl);
    if (shader && fvfVdecl) add(c.commitChunkDrawRunBreakStateDeltaMixedPairShaderFvfVdecl);
  } else if (stream) {
    add(c.commitChunkDrawRunBreakStateDeltaStreamOnly);
  } else if (ib) {
    add(c.commitChunkDrawRunBreakStateDeltaIbOnly);
  } else if (texture) {
    add(c.commitChunkDrawRunBreakStateDeltaTextureOnly);
  } else if (shader) {
    add(c.commitChunkDrawRunBreakStateDeltaShaderOnly);
  } else if (fvfVdecl) {
    add(c.commitChunkDrawRunBreakStateDeltaFvfVdeclOnly);
  } else {
    add(c.commitChunkDrawRunBreakStateDeltaOtherOnly);
  }
}

void countCommitChunkDrawRunBindingOverride(bool streamOverride,
                                            bool indexBufferOverride,
                                            std::size_t bytes) {
  auto& c = counters();
  add(c.commitChunkDrawRunBindingOverrideRecords);
  add(c.commitChunkDrawRunBindingOverrideBytes, static_cast<std::uint64_t>(bytes));
  if (streamOverride) {
    add(c.commitChunkDrawRunBindingOverrideStreamRecords);
  }
  if (indexBufferOverride) {
    add(c.commitChunkDrawRunBindingOverrideIbRecords);
  }
}

void countCommitChunkDrawBatchConstUploadPassthrough() {
  add(counters().commitChunkDrawBatchConstUploadPassthrough);
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

void countD3D9DrawStateCacheUniformRefresh() {
  add(counters().d3d9DrawStateCacheUniformRefreshes);
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

void countEncodeDrawStreamBindCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawStreamBindCpuNs, nanoseconds);
  updateMax(counters().encodeDrawStreamBindCpuMaxNs, nanoseconds);
  recordRing(counters().encodeDrawStreamBindCpuRing, nanoseconds);
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

void countEncodeDrawIndexCacheCandidateMeasureCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawIndexCacheCandidateMeasureCpuNs, nanoseconds);
}

void countEncodeDrawIndexCacheGateCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawIndexCacheGateCpuNs, nanoseconds);
}

void countEncodeDrawIndexCacheApplyCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawIndexCacheApplyCpuNs, nanoseconds);
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

void countEncodeDrawIssueCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawIssueCpuNs, nanoseconds);
  updateMax(counters().encodeDrawIssueCpuMaxNs, nanoseconds);
  recordRing(counters().encodeDrawIssueCpuRing, nanoseconds);
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

void countRenderPassSameKeyReentryPreservationBytes(std::uint64_t bytes) {
  add(counters().renderPassSameKeyReentryPreservationBytes, bytes);
}

void countRenderPassSameKeyReentryColorPreservationBytes(std::uint64_t bytes) {
  add(counters().renderPassSameKeyReentryColorPreservationBytes, bytes);
}

void countRenderPassSameKeyReentryDepthPreservationBytes(std::uint64_t bytes) {
  add(counters().renderPassSameKeyReentryDepthPreservationBytes, bytes);
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
  // V1 boundary B2 — measured at the d3d9 PE-side commit_chunk entry,
  // covers the WINE_UNIX_CALL marshalling, importer validation, and
  // seqId assignment cost. The matching call site is in
  // src/d3d9/device_c_chunk_replay.cpp (dxmt9c_device_commit_chunk).
  add(counters().bridgeCommitLatencyNs, nanoseconds);
  updateMax(counters().bridgeCommitLatencyMaxNs, nanoseconds);
  recordRing(counters().bridgeCommitLatencyRing, nanoseconds);
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

void countPresentBoundaryApplied() {
  add(counters().presentBoundaryApplied);
}

void countPresentBoundarySkipped() {
  add(counters().presentBoundarySkipped);
}

void countPresentBoundaryWait(std::uint64_t nanoseconds) {
  add(counters().presentBoundaryWaits);
  add(counters().presentBoundaryWaitNs, nanoseconds);
  updateMax(counters().presentBoundaryWaitMaxNs, nanoseconds);
  recordRing(counters().presentBoundaryWaitRing, nanoseconds);
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
      "alpha_test_enabled_draws=%llu alpha_test_effective_draws=%llu "
      "clip_plane_enabled_draws=%llu "
      "point_draws=%llu line_draws=%llu triangle_draws=%llu primitive_count=%llu "
      "triangle_estimate=%llu vertex_count=%llu "
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
      "transient_index_bytes=%llu "
      "transient_index_user_bytes=%llu "
      "transient_index_preupload_bytes=%llu "
      "transient_index_shadow_fallback_bytes=%llu "
      "transient_index_probe_reorder_bytes=%llu "
      "transient_index_optimized_order_bytes=%llu]\n",
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
      static_cast<unsigned long long>(b.alphaTestEnabledDraws),
      static_cast<unsigned long long>(b.alphaTestEffectiveDraws),
      static_cast<unsigned long long>(b.clipPlaneEnabledDraws),
      static_cast<unsigned long long>(b.pointDraws),
      static_cast<unsigned long long>(b.lineDraws),
      static_cast<unsigned long long>(b.triangleDraws),
      static_cast<unsigned long long>(b.primitiveCount),
      static_cast<unsigned long long>(b.triangleEstimate),
      static_cast<unsigned long long>(b.vertexCount),
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
      static_cast<unsigned long long>(b.transientIndexBytes),
      static_cast<unsigned long long>(b.transientIndexUserBytes),
      static_cast<unsigned long long>(b.transientIndexPreuploadBytes),
      static_cast<unsigned long long>(b.transientIndexShadowFallbackBytes),
      static_cast<unsigned long long>(b.transientIndexProbeReorderBytes),
      static_cast<unsigned long long>(b.transientIndexOptimizedOrderBytes));
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
  s.encodeDrawStreamBindCpuNs = load(c.encodeDrawStreamBindCpuNs);
  s.encodeDrawIssueCpuNs = load(c.encodeDrawIssueCpuNs);
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
  auto delta = [](std::uint64_t a, std::uint64_t b) -> std::uint64_t {
    // Atomic loads are relaxed and the snapshot covers many counters,
    // so a later snapshot field could lag a prior one if a writer is
    // mid-update — clamp to 0 instead of wrapping.
    return b >= a ? b - a : 0ull;
  };
  std::fprintf(
      stderr,
      "[dxmt9-perf-frame frame=%llu "
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
      "encode_draw_stream_bind_cpu_ms=%.3f "
      "encode_draw_issue_cpu_ms=%.3f transient_upload_cpu_ms=%.3f "
      "command_buffer_create_cpu_ms=%.3f command_buffer_commit_cpu_ms=%.3f "
      "completion_wait_ms=%.3f present_acquire_wait_ms=%.3f "
      "present_boundary_wait_ms=%.3f present_token_wait_ms=%.3f "
      "gpu_command_buffer_time_ms=%.3f gpu_command_buffer_time_samples=%llu "
      "render_encoder_gpu_time_ms=%.3f render_encoder_gpu_time_samples=%llu "
      "gpu_command_buffer_errors=%llu sub_command_buffers=%llu]\n",
      static_cast<unsigned long long>(frameId),
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
