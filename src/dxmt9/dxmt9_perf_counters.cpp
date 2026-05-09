#include "dxmt9_perf_counters.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <cstdint>
#include <vector>

namespace dxmt9::perf {
namespace {

// 64-sample sliding ring of nanosecond samples used to compute P50/P95/P99 in
// the shutdown report. Lock-free: writers race only on `head_` (atomic
// fetch_add, relaxed) and on the slot store (relaxed). Two threads racing for
// the same slot may overwrite each other's sample, but at the per-draw rate
// the steady-state percentile estimate is unaffected. Reading is one-shot at
// process exit, so a coarse snapshot via relaxed loads is acceptable.
class PercentileRing {
 public:
  static constexpr std::size_t kCapacity = 64;

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
  // (ceil) over the sorted snapshot, which is stable for small N (<=64) and
  // matches what runbook scripts expect.
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
  std::atomic<std::uint64_t> commandBuffers{0};
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
  std::atomic<std::uint64_t> bindVertexBuffer{0};
  std::atomic<std::uint64_t> bindIndexBuffer{0};
  std::atomic<std::uint64_t> bindUniformBuffer{0};
  std::atomic<std::uint64_t> bindPipeline{0};
  std::atomic<std::uint64_t> bindDepthState{0};
  std::atomic<std::uint64_t> bindViewport{0};
  std::atomic<std::uint64_t> bindScissor{0};
  std::atomic<std::uint64_t> bindRasterizer{0};
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
  std::atomic<std::uint64_t> encodeDrawIssueCpuNs{0};
  std::atomic<std::uint64_t> encodeDrawIssueCpuMaxNs{0};
  std::atomic<std::uint64_t> transientUploadCalls{0};
  std::atomic<std::uint64_t> transientUploadBytes{0};
  std::atomic<std::uint64_t> transientUploadCpuNs{0};
  std::atomic<std::uint64_t> transientUploadCpuMaxNs{0};
  std::atomic<std::uint64_t> uniformVsConstsCalls{0};
  std::atomic<std::uint64_t> uniformVsConstsBytes{0};
  std::atomic<std::uint64_t> uniformPsConstsCalls{0};
  std::atomic<std::uint64_t> uniformPsConstsBytes{0};
  std::atomic<std::uint64_t> uniformFfpVsCalls{0};
  std::atomic<std::uint64_t> uniformFfpVsBytes{0};
  std::atomic<std::uint64_t> uniformFfpPsCalls{0};
  std::atomic<std::uint64_t> uniformFfpPsBytes{0};
  std::atomic<std::uint64_t> uniformVolatilePushes{0};
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
  std::atomic<std::uint64_t> commandBufferCreateCpuNs{0};
  std::atomic<std::uint64_t> commandBufferCreateCpuMaxNs{0};
  std::atomic<std::uint64_t> commandBufferCommitCpuNs{0};
  std::atomic<std::uint64_t> commandBufferCommitCpuMaxNs{0};
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
  PercentileRing encodeChunkCpuRing;
  PercentileRing encodeDrawCpuRing;
  PercentileRing encodeDrawPipelineLookupCpuRing;
  PercentileRing encodeDrawUniformBuildCpuRing;
  PercentileRing encodeDrawFvfDecodeCpuRing;
  PercentileRing encodeDrawStreamBindCpuRing;
  PercentileRing encodeDrawIssueCpuRing;
  PercentileRing transientUploadCpuRing;
  PercentileRing commandBufferCreateCpuRing;
  PercentileRing commandBufferCommitCpuRing;
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

constexpr std::array<CounterEntry, 328> kCounterTable = {{
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
    {"bind_vertex_buffer", CounterEntry::Kind::UnsignedCount, &Counters::bindVertexBuffer, nullptr, nullptr, 0.0},
    {"bind_index_buffer", CounterEntry::Kind::UnsignedCount, &Counters::bindIndexBuffer, nullptr, nullptr, 0.0},
    {"bind_uniform_buffer", CounterEntry::Kind::UnsignedCount, &Counters::bindUniformBuffer, nullptr, nullptr, 0.0},
    {"bind_pipeline", CounterEntry::Kind::UnsignedCount, &Counters::bindPipeline, nullptr, nullptr, 0.0},
    {"bind_depth_state", CounterEntry::Kind::UnsignedCount, &Counters::bindDepthState, nullptr, nullptr, 0.0},
    {"bind_viewport", CounterEntry::Kind::UnsignedCount, &Counters::bindViewport, nullptr, nullptr, 0.0},
    {"bind_scissor", CounterEntry::Kind::UnsignedCount, &Counters::bindScissor, nullptr, nullptr, 0.0},
    {"bind_rasterizer", CounterEntry::Kind::UnsignedCount, &Counters::bindRasterizer, nullptr, nullptr, 0.0},
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
    {"encode_draw_issue_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawIssueCpuNs, nullptr, nullptr, 0.0},
    {"encode_draw_issue_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::encodeDrawIssueCpuMaxNs, nullptr, nullptr, 0.0},
    {"encode_draw_issue_cpu_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::encodeDrawIssueCpuRing, 0.5},
    {"encode_draw_issue_cpu_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::encodeDrawIssueCpuRing, 0.95},
    {"encode_draw_issue_cpu_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::encodeDrawIssueCpuRing, 0.99},
    {"transient_upload_calls", CounterEntry::Kind::UnsignedCount, &Counters::transientUploadCalls, nullptr, nullptr, 0.0},
    {"transient_upload_bytes", CounterEntry::Kind::UnsignedCount, &Counters::transientUploadBytes, nullptr, nullptr, 0.0},
    {"transient_upload_cpu_ms", CounterEntry::Kind::Milliseconds, &Counters::transientUploadCpuNs, nullptr, nullptr, 0.0},
    {"transient_upload_cpu_max_ms", CounterEntry::Kind::Milliseconds, &Counters::transientUploadCpuMaxNs, nullptr, nullptr, 0.0},
    {"transient_upload_cpu_p50_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::transientUploadCpuRing, 0.5},
    {"transient_upload_cpu_p95_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::transientUploadCpuRing, 0.95},
    {"transient_upload_cpu_p99_ms", CounterEntry::Kind::PercentileMs, nullptr, nullptr, &Counters::transientUploadCpuRing, 0.99},
    {"uniform_vs_consts_calls", CounterEntry::Kind::UnsignedCount, &Counters::uniformVsConstsCalls, nullptr, nullptr, 0.0},
    {"uniform_vs_consts_bytes", CounterEntry::Kind::UnsignedCount, &Counters::uniformVsConstsBytes, nullptr, nullptr, 0.0},
    {"uniform_ps_consts_calls", CounterEntry::Kind::UnsignedCount, &Counters::uniformPsConstsCalls, nullptr, nullptr, 0.0},
    {"uniform_ps_consts_bytes", CounterEntry::Kind::UnsignedCount, &Counters::uniformPsConstsBytes, nullptr, nullptr, 0.0},
    {"uniform_ffp_vs_calls", CounterEntry::Kind::UnsignedCount, &Counters::uniformFfpVsCalls, nullptr, nullptr, 0.0},
    {"uniform_ffp_vs_bytes", CounterEntry::Kind::UnsignedCount, &Counters::uniformFfpVsBytes, nullptr, nullptr, 0.0},
    {"uniform_ffp_ps_calls", CounterEntry::Kind::UnsignedCount, &Counters::uniformFfpPsCalls, nullptr, nullptr, 0.0},
    {"uniform_ffp_ps_bytes", CounterEntry::Kind::UnsignedCount, &Counters::uniformFfpPsBytes, nullptr, nullptr, 0.0},
    {"uniform_volatile_pushes", CounterEntry::Kind::UnsignedCount, &Counters::uniformVolatilePushes, nullptr, nullptr, 0.0},
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
}};

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

void countEncodeDrawIssueCpuTime(std::uint64_t nanoseconds) {
  add(counters().encodeDrawIssueCpuNs, nanoseconds);
  updateMax(counters().encodeDrawIssueCpuMaxNs, nanoseconds);
  recordRing(counters().encodeDrawIssueCpuRing, nanoseconds);
}

void countTransientUploadCpuTime(std::uint64_t nanoseconds, std::size_t bytes) {
  add(counters().transientUploadCalls);
  add(counters().transientUploadBytes, static_cast<std::uint64_t>(bytes));
  add(counters().transientUploadCpuNs, nanoseconds);
  updateMax(counters().transientUploadCpuMaxNs, nanoseconds);
  recordRing(counters().transientUploadCpuRing, nanoseconds);
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
      "present_boundary_wait_ms=%.3f present_token_wait_ms=%.3f]\n",
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
          1000000.0);
}

}  // namespace dxmt9::perf
